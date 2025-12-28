/*
 * Asterisk -- An open source telephony toolkit.
 *
 * See http://www.asterisk.org for more information about
 * the Asterisk project. Please do not directly contact
 * any of the maintainers of this project for assistance;
 * the project provides a web site, mailing lists and IRC
 * channels for your use.
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License Version 2. See the LICENSE file
 * at the top of the source tree.
 *
 * Please follow coding guidelines
 * http://svn.digium.com/view/asterisk/trunk/doc/CODING-GUIDELINES
 */

/*! \file
 *
 * \brief Implementation of the Asterisk's Speech API via Vosk
 *
 * \author Nickolay V. Shmyrev <nshmyrev@alphacephei.com>
 *
 * \ingroup applications
 */

/* Asterisk includes. */
#include "asterisk.h"
#include "asterisk/logger.h"
#include "asterisk/channel.h"
#include "asterisk/manager.h"

#define AST_MODULE "res_speech_vosk"
#include <asterisk/module.h>
#include <asterisk/config.h>
#include <asterisk/frame.h>
#include <asterisk/speech.h>
#include <asterisk/format_cache.h>
#include <asterisk/json.h>
#include <asterisk/lock.h>

#include <asterisk/http_websocket.h>


#define VOSK_ENGINE_NAME "vosk"
#define VOSK_ENGINE_CONFIG "res_speech_vosk.conf"
#define VOSK_BUF_SIZE 3200


/** \brief Forward declaration of speech (client object) */
typedef struct vosk_speech_t vosk_speech_t;
/** \brief Forward declaration of engine (global object) */
typedef struct vosk_engine_t vosk_engine_t;

/** \brief Declaration of Vosk speech structure */
struct vosk_speech_t {
        /* Name of the speech object to be used for logging */
        char                    *name;
        /* Websocket connection */
        struct ast_websocket    *ws;
        /* Buffer for frames */
        char                    buf[VOSK_BUF_SIZE];
        int                     offset;
        char                    *last_result;
        struct timeval          start_time; /* THE TIME KEEPER */
        char                    chan_name[AST_CHANNEL_NAME]; /* Added this */
        char                    chan_uniqueid[AST_MAX_UNIQUEID]; /* Added this */
};

/** \brief Declaration of Vosk recognition engine */
struct vosk_engine_t {
        /* Websocket url*/
        char                    *ws_url;
};


static struct vosk_engine_t vosk_engine;

/** \brief Set up the speech structure within the engine */
static int vosk_recog_create(struct ast_speech *speech, struct ast_format *format)
{
        vosk_speech_t *vosk_speech;
        enum ast_websocket_result result;


        vosk_speech = ast_calloc(1, sizeof(*vosk_speech));
        if (!vosk_speech) {
                return -1;
        }



        vosk_speech->name = "vosk";
        speech->data = vosk_speech;

        ast_debug(1, "(%s) Create speech resource %s\n", vosk_speech->name, vosk_engine.ws_url);

        vosk_speech->ws = ast_websocket_client_create(vosk_engine.ws_url, "ws", NULL, &result);
        if (!vosk_speech->ws) {
                ast_free(speech->data);
                speech->data = NULL;
                return -1;
        }

        ast_debug(1, "(%s) Created speech resource result %d\n", vosk_speech->name, result);

        return 0;
}

/* =========================
 *  Final Production Version
 * ========================= */

/* Helper: flush any remaining buffered audio */
static void vosk_flush_tail(vosk_speech_t *vosk_speech)
{
        if (vosk_speech && vosk_speech->ws && vosk_speech->offset > 0) {
                ast_debug(3, "(%s) Flushing tail audio: %d bytes\n",
                          vosk_speech->name, vosk_speech->offset);

                if (ast_websocket_write(vosk_speech->ws,
                                        AST_WEBSOCKET_OPCODE_BINARY,
                                        vosk_speech->buf,
                                        vosk_speech->offset) < 0) {
                        ast_log(LOG_WARNING, "(%s) WebSocket write failed (tail flush)\n",
                                vosk_speech->name);
                }
                vosk_speech->offset = 0;
        }
}

/** \brief Write audio to the speech engine */
static int vosk_recog_write(struct ast_speech *speech, void *data, int len)
{
        vosk_speech_t *vosk_speech = speech ? speech->data : NULL;
        char *res = NULL;
        int res_len;
        int rc = 0;

        /* Avoid early returns that could break lock symmetry */
        if (!vosk_speech || !vosk_speech->ws) {
                rc = -1;
                goto out_no_unlock;
        }

        if (len <= 0) {
                rc = 0;
                goto out_no_unlock;
        }

        /* Reject absurdly large single frames */
        if (len > VOSK_BUF_SIZE) {
                ast_log(LOG_ERROR, "(%s) Frame too large: %d > %d\n",
                        vosk_speech->name, len, VOSK_BUF_SIZE);
                rc = -1;
                goto out_no_unlock;
        }

        /* If it won't fit, flush what we have */
        if (vosk_speech->offset + len > VOSK_BUF_SIZE) {
                if (vosk_speech->offset > 0) {
                        if (ast_websocket_write(vosk_speech->ws,
                                                AST_WEBSOCKET_OPCODE_BINARY,
                                                vosk_speech->buf,
                                                vosk_speech->offset) < 0) {
                                ast_log(LOG_WARNING,
                                        "(%s) WebSocket write failed (pre-flush)\n",
                                        vosk_speech->name);
                        }                        
                } 
                /* Always reset offset after detecting overflow */
                vosk_speech->offset = 0;
        }

        memcpy(vosk_speech->buf + vosk_speech->offset, data, len);
        vosk_speech->offset += len;

        /* Only possible to be exactly full here */
        if (vosk_speech->offset == VOSK_BUF_SIZE) {
                if (ast_websocket_write(vosk_speech->ws,
                                        AST_WEBSOCKET_OPCODE_BINARY,
                                        vosk_speech->buf,
                                        VOSK_BUF_SIZE) < 0) {
                        ast_log(LOG_WARNING,
                                "(%s) WebSocket write failed (full chunk)\n",
                                vosk_speech->name);
                }
                vosk_speech->offset = 0;
        }

        /* Release lock while performing websocket I/O */
        ast_mutex_unlock(&speech->lock);

        /* Drain all pending recognition results */
        while (ast_websocket_wait_for_input(vosk_speech->ws, 0) > 0) {
                res_len = ast_websocket_read_string(vosk_speech->ws, &res);
                if (res_len < 0 || !res) {
                        break;
                }

                struct ast_json_error err;
                struct ast_json *j = ast_json_load_string(res, &err);

                /* Always free the websocket string */
                ast_free(res);
                res = NULL;

                if (!j) {
                        ast_log(LOG_ERROR, "(%s) JSON parse error: %s\n",
                                vosk_speech->name, err.text);
                        continue;
                }

                const char *partial = ast_json_object_string_get(j, "partial");
                const char *text    = ast_json_object_string_get(j, "text");

                if (partial && !ast_strlen_zero(partial)) {
                        ast_free(vosk_speech->last_result);
                        vosk_speech->last_result = ast_strdup(partial);

                        /* Calculate how many MS have passed since we started listening */
                        long ms_offset = ast_tvdiff_ms(ast_tvnow(), vosk_speech->start_time);

                        /* NOTE: Channel metadata will only be captured if the dialplan sets:
                         *   same => n,Set(SPEECH_ENGINE(channel)=${CHANNEL})
                         *   same => n,Set(SPEECH_ENGINE(uniqueid)=${UNIQUEID})
                         * after calling SpeechCreate(vosk).
                         * Otherwise, diagnostic placeholders will appear in AMI events.
                         */
                        
                        if (strlen(partial) < 2500) {
                                manager_event(EVENT_FLAG_REPORTING, "VoskPartial",
                                    "Channel: %s\r\n"
                                    "Uniqueid: %s\r\n"
                                    "TimeCode: %ld\r\n"
                                    "PartialText: %s\r\n",
                                    vosk_speech->chan_name[0] ? vosk_speech->chan_name : "not_set_in_dialplan",
                                    vosk_speech->chan_uniqueid[0] ? vosk_speech->chan_uniqueid : "not_set_in_dialplan",
                                    ms_offset,
                                    partial);
                        }

                } else if (text && !ast_strlen_zero(text)) {
                        ast_free(vosk_speech->last_result);
                        vosk_speech->last_result = ast_strdup(text);
                        ast_speech_change_state(speech, AST_SPEECH_STATE_DONE);
                }

                ast_json_free(j);
        }

        /* Re-acquire lock before returning */
        ast_mutex_lock(&speech->lock);

out_no_unlock:
        if (res) {
                ast_free(res);
        }
        return rc;
}

/*! \brief Stop the in-progress recognition */
static int vosk_recog_stop(struct ast_speech *speech)
{
        vosk_speech_t *vosk_speech = speech ? speech->data : NULL;

        if (vosk_speech && vosk_speech->ws) {
                /* Flush remaining audio so final words are processed */
                vosk_flush_tail(vosk_speech);

                /* Optional: explicit EOF on stop */
                ast_websocket_write_string(vosk_speech->ws, "{\"eof\":1}");
        }

        ast_debug(1, "(%s) Stop recognition\n",
                  vosk_speech ? vosk_speech->name : "vosk");

        ast_speech_change_state(speech, AST_SPEECH_STATE_NOT_READY);
        return 0;
}

/** \brief Destroy any data set on the speech structure by the engine */
static int vosk_recog_destroy(struct ast_speech *speech)
{
        const char *eof = "{\"eof\": 1}";
        vosk_speech_t *vosk_speech = speech ? speech->data : NULL;

        if (!vosk_speech) {
                return 0;
        }

        ast_debug(1, "(%s) Destroy speech resource\n", vosk_speech->name);

        if (vosk_speech->ws) {
                int fd = ast_websocket_fd(vosk_speech->ws);

                /* Flush tail audio BEFORE EOF */
                vosk_flush_tail(vosk_speech);

                if (fd > 0) {
                        ast_websocket_write_string(vosk_speech->ws, eof);
                        ast_websocket_close(vosk_speech->ws, 1000);
                        shutdown(fd, SHUT_RDWR);
                }
                ast_websocket_unref(vosk_speech->ws);
                vosk_speech->ws = NULL;
        }

        ast_free(vosk_speech->last_result);
        ast_free(vosk_speech);
        speech->data = NULL;

        return 0;
}

/*! \brief Load a local grammar on the speech structure */
static int vosk_recog_load_grammar(struct ast_speech *speech, const char *grammar_name, const char *grammar_path)
{
        return 0;
}

/** \brief Unload a local grammar */
static int vosk_recog_unload_grammar(struct ast_speech *speech, const char *grammar_name)
{
        return 0;
}

/** \brief Activate a loaded grammar */
static int vosk_recog_activate_grammar(struct ast_speech *speech, const char *grammar_name)
{
        return 0;
}

/** \brief Deactivate a loaded grammar */
static int vosk_recog_deactivate_grammar(struct ast_speech *speech, const char *grammar_name)
{
        return 0;
}

/** \brief Signal DTMF was received */
static int vosk_recog_dtmf(struct ast_speech *speech, const char *dtmf)
{
        vosk_speech_t *vosk_speech = speech->data;
        ast_verb(4, "(%s) Signal DTMF %s\n", vosk_speech->name, dtmf);
        return 0;
}

/** brief Prepare engine to accept audio */
static int vosk_recog_start(struct ast_speech *speech)
{
        vosk_speech_t *vosk_speech = speech->data;
       
        /* Mark the 'Zero' point for timecodes */
        vosk_speech->start_time = ast_tvnow();

        ast_debug(1, "(%s) Start recognition\n", vosk_speech->name);
        ast_speech_change_state(speech, AST_SPEECH_STATE_READY);
        return 0;
}

/** \brief Change an engine specific setting */
static int vosk_recog_change(struct ast_speech *speech, const char *name, const char *value)
{
    vosk_speech_t *vosk_speech = speech ? speech->data : NULL;

    if (!vosk_speech || ast_strlen_zero(name)) {
        return -1;
    }

    ast_debug(2, "(%s) Change setting name: %s value:%s\n",
              vosk_speech->name, name, S_OR(value, ""));

    if (!strcasecmp(name, "channel")) {
        ast_copy_string(vosk_speech->chan_name, S_OR(value, ""),
                        sizeof(vosk_speech->chan_name));
        return 0;
    }

    if (!strcasecmp(name, "uniqueid")) {
        ast_copy_string(vosk_speech->chan_uniqueid, S_OR(value, ""),
                        sizeof(vosk_speech->chan_uniqueid));
        return 0;
    }

    return 0;
}


/** \brief Get an engine specific attribute */
static int vosk_recog_get_settings(struct ast_speech *speech, const char *name, char *buf, size_t len)
{
        vosk_speech_t *vosk_speech = speech->data;
        ast_debug(1, "(%s) Get settings name: %s\n", vosk_speech->name, name);
        return -1;
}

/** \brief Change the type of results we want back */
static int vosk_recog_change_results_type(struct ast_speech *speech, enum ast_speech_results_type results_type)
{
        return -1;
}

/** \brief Try to get result */
struct ast_speech_result *vosk_recog_get(struct ast_speech *speech)
{
        struct ast_speech_result *speech_result;

        vosk_speech_t *vosk_speech = speech->data;
        speech_result = ast_calloc(sizeof(struct ast_speech_result), 1);
        speech_result->text = ast_strdup(vosk_speech->last_result);
        speech_result->score = 100;

        ast_set_flag(speech, AST_SPEECH_HAVE_RESULTS);
        return speech_result;
}

/** \brief Speech engine declaration */
static struct ast_speech_engine ast_engine = {
        VOSK_ENGINE_NAME,
        vosk_recog_create,
        vosk_recog_destroy,
        vosk_recog_load_grammar,
        vosk_recog_unload_grammar,
        vosk_recog_activate_grammar,
        vosk_recog_deactivate_grammar,        
        vosk_recog_write,
        vosk_recog_dtmf,
        vosk_recog_start,
        vosk_recog_change,
        vosk_recog_get_settings,
        vosk_recog_change_results_type,
        vosk_recog_get
};

/** \brief Load Vosk engine configuration (/etc/asterisk/res_speech_vosk.conf)*/
static int vosk_engine_config_load(void)
{
        const char *value = NULL;
        struct ast_flags config_flags = { 0 };
        struct ast_config *cfg = ast_config_load(VOSK_ENGINE_CONFIG, config_flags);

        if (!cfg) {
                ast_log(LOG_WARNING, "No such configuration file %s\n", VOSK_ENGINE_CONFIG);
                return -1;
        }

        if ((value = ast_variable_retrieve(cfg, "general", "url")) != NULL) {
                ast_log(LOG_DEBUG, "general.url=%s\n", value);
                vosk_engine.ws_url = ast_strdup(value);
        }

        if (!vosk_engine.ws_url) {
                vosk_engine.ws_url = ast_strdup("ws://localhost");
        }

        ast_config_destroy(cfg);
        return 0;
}

/** \brief Load module */
static int load_module(void)
{
        ast_log(LOG_NOTICE, "Load res_speech_vosk module\n");

        /* Load engine configuration */
        if (vosk_engine_config_load()) {
                return AST_MODULE_LOAD_DECLINE;
        }

        /* ONE-TIME WARNING about channel metadata requirement */        
        ast_log(LOG_NOTICE,
            "Vosk Speech Recognition loaded. "
            "NOTE: For channel metadata in AMI events, add to dialplan:\n"
            "      same => n,SpeechCreate(vosk)\n"
            "      same => n,Set(SPEECH_ENGINE(channel)=${CHANNEL})\n"
            "      same => n,Set(SPEECH_ENGINE(uniqueid)=${UNIQUEID})\n"
            "      Events will show 'not_set_in_dialplan' if omitted.\n");
                
        ast_engine.formats = ast_format_cap_alloc(AST_FORMAT_CAP_FLAG_DEFAULT);
        if (!ast_engine.formats) {
                ast_log(LOG_ERROR, "Failed to alloc media format capabilities\n");
                return AST_MODULE_LOAD_FAILURE;
        }
        ast_format_cap_append(ast_engine.formats, ast_format_slin, 0);

        if (ast_speech_register(&ast_engine)) {
                ast_log(LOG_ERROR, "Failed to register module\n");
                return AST_MODULE_LOAD_FAILURE;
        }

        return AST_MODULE_LOAD_SUCCESS;
}

/** \brief Unload module */
static int unload_module(void)
{
        ast_log(LOG_NOTICE, "Unload res_speech_vosk module\n");

        if (ast_speech_unregister(VOSK_ENGINE_NAME)) {
                ast_log(LOG_ERROR, "Failed to unregister module\n");
        }

        ast_free(vosk_engine.ws_url);
        vosk_engine.ws_url = NULL;

        return 0;
}

AST_MODULE_INFO_STANDARD(ASTERISK_GPL_KEY, "Vosk Speech Engine");
