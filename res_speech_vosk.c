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

/** \brief Engine state for robustness and diagnostics */
enum vosk_state {
        VOSK_STATE_INIT = 0,
        VOSK_STATE_CONNECTED,
        VOSK_STATE_FAILED,
        VOSK_STATE_CLOSED,
};

/** \brief Declaration of Vosk speech structure */
struct vosk_speech_t {
        /* Name of the speech object to be used for logging */
        char                    *name;

        /* Websocket connection (protected by lock) */
        struct ast_websocket    *ws;

        /* Protects all fields below */
        ast_mutex_t              lock;
        ast_cond_t               cond;

        /* Serialize engine entry vs destroy */
        unsigned int             closing:1;
        unsigned int             active_calls;

        /* Buffer for frames */
        char                    buf[VOSK_BUF_SIZE];
        int                     offset;

        char                    *last_result;
        struct timeval          start_time; /* THE TIME KEEPER */

        char                    chan_name[AST_CHANNEL_NAME];
        char                    chan_uniqueid[AST_MAX_UNIQUEID];
        char                    *last_partial_sent;

        /* Engine state and basic error counters */
        enum vosk_state         state;
        unsigned int            send_errors;
        unsigned int            recv_errors;
        unsigned int            json_errors;
        unsigned int            overflow_events;
};

/** \brief Declaration of Vosk recognition engine */
struct vosk_engine_t {
        /* Websocket url*/
        char                    *ws_url;
};

static struct vosk_engine_t vosk_engine;

/* ---------- engine entry/exit guards (prevents destroy races) ---------- */

static int vosk_enter(vosk_speech_t *vs)
{
        int ok = 0;

        ast_mutex_lock(&vs->lock);
        if (!vs->closing) {
                vs->active_calls++;
                ok = 1;
        }
        ast_mutex_unlock(&vs->lock);

        return ok;
}

static void vosk_leave(vosk_speech_t *vs)
{
        ast_mutex_lock(&vs->lock);
        if (vs->active_calls > 0) {
                vs->active_calls--;
        }
        if (vs->closing && vs->active_calls == 0) {
                ast_cond_signal(&vs->cond);
        }
        ast_mutex_unlock(&vs->lock);
}

/* Helper: flush any remaining buffered audio (safe vs destroy/stop) */
static void vosk_flush_tail(vosk_speech_t *vosk_speech)
{
        struct ast_websocket *ws = NULL;
        int bytes = 0;

        if (!vosk_speech) {
                return;
        }

        ast_mutex_lock(&vosk_speech->lock);
        if (vosk_speech->ws && vosk_speech->offset > 0 && !vosk_speech->closing) {
                ws = vosk_speech->ws;
                ast_websocket_ref(ws);
                bytes = vosk_speech->offset;
                vosk_speech->offset = 0;
        }
        ast_mutex_unlock(&vosk_speech->lock);

        if (!ws) {
                return;
        }

        ast_debug(3, "(%s) Flushing tail audio: %d bytes\n",
                  vosk_speech->name, bytes);

        if (ast_websocket_write(ws,
                                AST_WEBSOCKET_OPCODE_BINARY,
                                vosk_speech->buf,
                                bytes) < 0) {
                ast_log(LOG_WARNING, "(%s) WebSocket write failed (tail flush)\n",
                        vosk_speech->name);
                ast_mutex_lock(&vosk_speech->lock);
                vosk_speech->send_errors++;
                ast_mutex_unlock(&vosk_speech->lock);
        }

        ast_websocket_unref(ws);
}

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

        ast_mutex_init(&vosk_speech->lock);
        ast_cond_init(&vosk_speech->cond, NULL);

        vosk_speech->closing = 0;
        vosk_speech->active_calls = 0;

        /* Initial state before websocket connect attempt */
        vosk_speech->state = VOSK_STATE_INIT;
        vosk_speech->send_errors = 0;
        vosk_speech->recv_errors = 0;
        vosk_speech->json_errors = 0;
        vosk_speech->overflow_events = 0;

        ast_debug(1, "(%s) Create speech resource %s\n", vosk_speech->name, vosk_engine.ws_url);

        vosk_speech->ws = ast_websocket_client_create(vosk_engine.ws_url, "ws", NULL, &result);
        if (!vosk_speech->ws) {
                ast_cond_destroy(&vosk_speech->cond);
                ast_mutex_destroy(&vosk_speech->lock);
                ast_free(speech->data);
                speech->data = NULL;
                return -1;
        }

        ast_debug(1, "(%s) Created speech resource result %d\n", vosk_speech->name, result);
        return 0;
}

/** \brief Write audio to the speech engine */
static int vosk_recog_write(struct ast_speech *speech, void *data, int len)
{
        vosk_speech_t *vosk_speech = speech ? speech->data : NULL;
        struct ast_websocket *ws = NULL;
        char *res = NULL;
        int res_len;

        if (!vosk_speech) {
                return -1;
        }

        if (!vosk_enter(vosk_speech)) {
                return -1;
        }

        if (len <= 0) {
                vosk_leave(vosk_speech);
                return 0;
        }

        if (len > VOSK_BUF_SIZE) {
                ast_log(LOG_ERROR, "(%s) Frame too large: %d > %d\n",
                        vosk_speech->name, len, VOSK_BUF_SIZE);
                vosk_leave(vosk_speech);
                return -1;
        }

        /* Touch shared state under lock, but do NOT do websocket I/O while locked */
        int flush_bytes = 0;
        int full_chunk = 0;

        ast_mutex_lock(&vosk_speech->lock);

        if (!vosk_speech->ws || vosk_speech->closing) {
                ast_mutex_unlock(&vosk_speech->lock);
                vosk_leave(vosk_speech);
                return -1;
        }

        ws = vosk_speech->ws;
        ast_websocket_ref(ws);

        if (vosk_speech->offset + len > VOSK_BUF_SIZE && vosk_speech->offset > 0) {
                flush_bytes = vosk_speech->offset;
                vosk_speech->offset = 0;
                vosk_speech->overflow_events++;
        }

        memcpy(vosk_speech->buf + vosk_speech->offset, data, len);
        vosk_speech->offset += len;
        full_chunk = (vosk_speech->offset == VOSK_BUF_SIZE);

        ast_mutex_unlock(&vosk_speech->lock);

        /* Write any pre-flush chunk */
        if (flush_bytes > 0) {
                if (ast_websocket_write(ws, AST_WEBSOCKET_OPCODE_BINARY,
                                        vosk_speech->buf, flush_bytes) < 0) {
                        ast_log(LOG_WARNING, "(%s) WebSocket write failed (pre-flush)\n", vosk_speech->name);
                        ast_mutex_lock(&vosk_speech->lock);
                        vosk_speech->send_errors++;
                        ast_mutex_unlock(&vosk_speech->lock);
                }
        }

        /* Write full chunk */
        if (full_chunk) {
                if (ast_websocket_write(ws, AST_WEBSOCKET_OPCODE_BINARY,
                                        vosk_speech->buf, VOSK_BUF_SIZE) < 0) {
                        ast_log(LOG_WARNING, "(%s) WebSocket write failed (full chunk)\n", vosk_speech->name);
                        ast_mutex_lock(&vosk_speech->lock);
                        vosk_speech->send_errors++;
                        ast_mutex_unlock(&vosk_speech->lock);
                }

                ast_mutex_lock(&vosk_speech->lock);
                vosk_speech->offset = 0;
                ast_mutex_unlock(&vosk_speech->lock);
        }

        /* Drain pending recognition results (non-blocking) */
        while (ast_websocket_wait_for_input(ws, 0) > 0) {

                /* If destroy started, stop draining immediately */
                ast_mutex_lock(&vosk_speech->lock);
                int closing = vosk_speech->closing;
                ast_mutex_unlock(&vosk_speech->lock);
                if (closing) {
                        break;
                }

                res_len = ast_websocket_read_string(ws, &res);
                if (res_len < 0 || !res) {
                        ast_mutex_lock(&vosk_speech->lock);
                        vosk_speech->recv_errors++;
                        ast_mutex_unlock(&vosk_speech->lock);
                        break;
                }

                struct ast_json_error err;
                struct ast_json *j = ast_json_load_string(res, &err);

                ast_free(res);
                res = NULL;

                if (!j) {
                        ast_log(LOG_ERROR, "(%s) JSON parse error: %s\n", vosk_speech->name, err.text);
                        ast_mutex_lock(&vosk_speech->lock);
                        vosk_speech->json_errors++;
                        ast_mutex_unlock(&vosk_speech->lock);
                        continue;
                }

                const char *partial = ast_json_object_string_get(j, "partial");
                const char *text    = ast_json_object_string_get(j, "text");

                if (partial && !ast_strlen_zero(partial)) {
                        long ms_offset = ast_tvdiff_ms(ast_tvnow(), vosk_speech->start_time);

                        /* Update last_result and dedupe state under lock */
                        char chan_name[AST_CHANNEL_NAME];
                        char chan_uniqueid[AST_MAX_UNIQUEID];
                        int emit = 0;

                        ast_mutex_lock(&vosk_speech->lock);

                        if (!vosk_speech->closing) {
                                ast_free(vosk_speech->last_result);
                                vosk_speech->last_result = ast_strdup(partial);

                                emit = (!vosk_speech->last_partial_sent ||
                                        strcmp(vosk_speech->last_partial_sent, partial) != 0);

                                if (emit) {
                                        ast_free(vosk_speech->last_partial_sent);
                                        vosk_speech->last_partial_sent = ast_strdup(partial);
                                }

                                ast_copy_string(chan_name,
                                        vosk_speech->chan_name[0] ? vosk_speech->chan_name : "not_set_in_dialplan",
                                        sizeof(chan_name));
                                ast_copy_string(chan_uniqueid,
                                        vosk_speech->chan_uniqueid[0] ? vosk_speech->chan_uniqueid : "not_set_in_dialplan",
                                        sizeof(chan_uniqueid));
                        }
                        ast_mutex_unlock(&vosk_speech->lock);

                        if (emit && strlen(partial) < 2500) {
                                manager_event(EVENT_FLAG_REPORTING, "VoskPartial",
                                    "Channel: %s\r\n"
                                    "Uniqueid: %s\r\n"
                                    "TimeCode: %ld\r\n"
                                    "PartialText: %s\r\n",
                                    chan_name, chan_uniqueid, ms_offset, partial);
                        }

                } else if (text && !ast_strlen_zero(text)) {

                        ast_mutex_lock(&vosk_speech->lock);
                        if (!vosk_speech->closing) {
                                ast_free(vosk_speech->last_result);
                                vosk_speech->last_result = ast_strdup(text);
                        }
                        int allow_state_change = !vosk_speech->closing;
                        ast_mutex_unlock(&vosk_speech->lock);

                        if (allow_state_change) {
                                ast_speech_change_state(speech, AST_SPEECH_STATE_DONE);
                        }
                }

                ast_json_free(j);
        }

        if (res) {
                ast_free(res);
        }

        ast_websocket_unref(ws);
        vosk_leave(vosk_speech);
        return 0;
}

/*! \brief Stop the in-progress recognition */
static int vosk_recog_stop(struct ast_speech *speech)
{
        vosk_speech_t *vosk_speech = speech ? speech->data : NULL;
        struct ast_websocket *ws = NULL;

        if (!vosk_speech) {
                return 0;
        }

        if (!vosk_enter(vosk_speech)) {
                return 0;
        }

        ast_mutex_lock(&vosk_speech->lock);
        if (vosk_speech->ws && !vosk_speech->closing) {
                ws = vosk_speech->ws;
                ast_websocket_ref(ws);
        }
        ast_mutex_unlock(&vosk_speech->lock);

        if (ws) {
                vosk_flush_tail(vosk_speech);
                ast_websocket_write_string(ws, "{\"eof\":1}");
                ast_websocket_unref(ws);
        }

        ast_debug(1, "(%s) Stop recognition\n", vosk_speech->name);
        ast_speech_change_state(speech, AST_SPEECH_STATE_NOT_READY);

        vosk_leave(vosk_speech);
        return 0;
}

/** \brief Destroy any data set on the speech structure by the engine */
static int vosk_recog_destroy(struct ast_speech *speech)
{
        const char *eof = "{\"eof\": 1}";
        vosk_speech_t *vs = speech ? speech->data : NULL;
        struct ast_websocket *ws = NULL;

        if (!vs) {
                return 0;
        }

        ast_debug(1, "(%s) Destroy speech resource\n", vs->name);

        /*
         * FIX: detach + close websocket FIRST to break any in-flight blocking I/O,
         * then wait for active_calls to drain. Never shutdown(fd) yourself.
         */
        ast_mutex_lock(&vs->lock);
        vs->closing = 1;

        ws = vs->ws;
        vs->ws = NULL;

        if (ws) {
                ast_websocket_ref(ws);
        }
        ast_mutex_unlock(&vs->lock);

        if (ws) {
                /* Best-effort EOF and close to force writers/readers to unwind */
                ast_websocket_write_string(ws, eof);
                ast_websocket_close(ws, 1000);
                ast_websocket_unref(ws);
                ws = NULL;
        }

        /* Now wait for all in-flight calls to exit */
        ast_mutex_lock(&vs->lock);
        while (vs->active_calls > 0) {
                ast_cond_wait(&vs->cond, &vs->lock);
        }

        ast_free(vs->last_result);
        vs->last_result = NULL;

        ast_free(vs->last_partial_sent);
        vs->last_partial_sent = NULL;

        ast_mutex_unlock(&vs->lock);

        ast_cond_destroy(&vs->cond);
        ast_mutex_destroy(&vs->lock);

        ast_free(vs);
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

        if (!vosk_enter(vosk_speech)) {
                return -1;
        }

        ast_debug(2, "(%s) Change setting name: %s value:%s\n",
                  vosk_speech->name, name, S_OR(value, ""));

        if (!strcasecmp(name, "channel")) {
                ast_mutex_lock(&vosk_speech->lock);
                ast_copy_string(vosk_speech->chan_name, S_OR(value, ""),
                                sizeof(vosk_speech->chan_name));
                ast_mutex_unlock(&vosk_speech->lock);
                vosk_leave(vosk_speech);
                return 0;
        }

        if (!strcasecmp(name, "uniqueid")) {
                ast_mutex_lock(&vosk_speech->lock);
                ast_copy_string(vosk_speech->chan_uniqueid, S_OR(value, ""),
                                sizeof(vosk_speech->chan_uniqueid));
                ast_mutex_unlock(&vosk_speech->lock);
                vosk_leave(vosk_speech);
                return 0;
        }

        vosk_leave(vosk_speech);
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
        vosk_speech_t *vosk_speech = speech ? speech->data : NULL;

        if (!vosk_speech) {
                return NULL;
        }

        if (!vosk_enter(vosk_speech)) {
                return NULL;
        }

        speech_result = ast_calloc(1, sizeof(*speech_result));
        if (!speech_result) {
                vosk_leave(vosk_speech);
                return NULL;
        }

        ast_mutex_lock(&vosk_speech->lock);
        speech_result->text = ast_strdup(vosk_speech->last_result);
        ast_mutex_unlock(&vosk_speech->lock);

        speech_result->score = 100;

        ast_set_flag(speech, AST_SPEECH_HAVE_RESULTS);

        vosk_leave(vosk_speech);
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
