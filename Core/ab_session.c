#include "ab_session.h"
#include "ab_settings.h"
#include "xp/coap/gg_coap.h"
#include "xp/common/gg_buffer.h"
#include "xp/common/gg_logging.h"
#include "xp/common/gg_memory.h"
#include "xp/common/gg_threads.h"
#include "xp/common/gg_timer.h"
#include "xp/loop/gg_loop.h"
#include "xp/gattlink/gg_gattlink_generic_client.h"
#include <stdatomic.h>
#include "xp/module/gg_module.h"
#include "xp/stack_builder/gg_stack_builder.h"
#include "xp/tls/gg_tls.h"
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
/* The pinned Apple port omits freeing the opaque wrapper. CMake renames
 * its destructor so this adapter can complete ownership cleanup. */
void AB_GG_Semaphore_DestroyStorage(GG_Semaphore* semaphore);
void GG_Semaphore_Destroy(GG_Semaphore* semaphore) {
    if (!semaphore) return;
    AB_GG_Semaphore_DestroyStorage(semaphore);
    GG_FreeMemory(semaphore);
}
#define HANDSHAKE_RETRY_MS 6000
#define MAX_HANDSHAKE_ATTEMPTS 2
#define LOST_HANDSHAKE_RACE (-1)
#define RESTORE_MS 1500
typedef enum { IDLE, TLS, FIRST, TAP, ALT, PAUSE, ORIGINAL, VERIFY,
               CORRECT, FINAL_READ, RESTORE, HANDOFF, DONE } Phase;
struct AB_Session {
    GG_IMPLEMENTS(GG_DataSink);
    GG_IMPLEMENTS(GG_TlsKeyResolver);
    GG_IMPLEMENTS(GG_EventListener);
    GG_IMPLEMENTS(GG_CoapResponseListener);
    GG_IMPLEMENTS(GG_TimerListener);
    GG_Loop* loop;
    GG_Stack* stack;
    GG_CoapEndpoint* endpoint;
    GG_StackElementPortInfo bottom, top;
    GG_Timer* timer;
    GG_CoapRequestHandle request;
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t ready;
    bool initialized, started, pending, replied, connected, lost, tls_error;
    bool gatt_open, write_sent, aborting;
    atomic_bool muted;
    unsigned attempts;
    AB_SendFn send;
    AB_DoneFn done;
    AB_LogFn log;
    void* context;
    size_t packet_size;
    AB_Operation operation;
    Phase phase;
    int original, result, reply;
    const char* message;
    unsigned operation_ms, handoff_ms, request_ms;
    uint64_t deadline, handoff_deadline, pause_deadline, request_deadline, handshake_deadline;
};
static pthread_once_t module_once = PTHREAD_ONCE_INIT;
static GG_Result module_result;
static void initialize_module(void) {
    module_result = GG_Module_Initialize();
    GG_LogManager_Configure("plist:.level=WARNING");
}
static uint64_t now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}
static void log_line(AB_Session* s, const char* line) { if (s->log) s->log(s->context, line); }
static void clear_stack(AB_Session* s) {
    if (s->endpoint) {
        GG_DataSource_SetDataSink(s->top.source, NULL);
        GG_CoapEndpoint_Destroy(s->endpoint);
        s->endpoint = NULL;
    }
    if (s->stack) { GG_Stack_Destroy(s->stack); s->stack = NULL; }
    s->pending = s->replied = s->connected = s->tls_error = s->gatt_open = false;
}
static GG_Result send_packet(GG_DataSink* _self, GG_Buffer* buffer, const GG_BufferMetadata* metadata) {
    AB_Session* s = GG_SELF(AB_Session, GG_DataSink);
    (void)metadata;
    size_t size = GG_Buffer_GetDataSize(buffer);
    if (size > s->packet_size) return GG_ERROR_OUT_OF_RANGE;
    if (!atomic_load(&s->muted) && s->phase != DONE) s->send(s->context, GG_Buffer_GetData(buffer), size);
    return GG_SUCCESS;
}
static GG_Result sink_listener(GG_DataSink* _self, GG_DataSinkListener* listener) {
    (void)_self; (void)listener; return GG_SUCCESS;
}
GG_IMPLEMENT_INTERFACE(AB_Session, GG_DataSink) {send_packet, sink_listener};
static GG_Result resolve_key(GG_TlsKeyResolver* _self, const uint8_t* id, size_t size,
                             uint8_t* key, size_t* key_size) {
    AB_Session* s = GG_SELF(AB_Session, GG_TlsKeyResolver);
    static const uint8_t bootstrap[] = {0x81, 6, 0x54, 0xe3, 0x36, 0xad, 0xca, 0xb0,
                                       0xa0, 0x3c, 0x60, 0xf7, 0x4a, 0xa0, 0xb6, 0xfb};
    if (s->phase == HANDOFF) {
        log_line(s, "Handoff identity refused");
        return GG_ERROR_NO_SUCH_ITEM;
    }
    if (size != 9 || memcmp(id, "BOOTSTRAP", 9)) return GG_ERROR_NO_SUCH_ITEM;
    if (*key_size < sizeof(bootstrap)) return GG_ERROR_NOT_ENOUGH_SPACE;
    memcpy(key, bootstrap, sizeof(bootstrap));
    *key_size = sizeof(bootstrap);
    return GG_SUCCESS;
}
GG_IMPLEMENT_INTERFACE(AB_Session, GG_TlsKeyResolver) {resolve_key};
static void stack_event(GG_EventListener* _self, const GG_Event* event) {
    AB_Session* s = GG_SELF(AB_Session, GG_EventListener);
    if (event->type != GG_EVENT_TYPE_STACK_EVENT_FORWARD) return;
    const GG_Event* forwarded = ((const GG_StackForwardEvent*)event)->forwarded;
    if (forwarded->type == GG_EVENT_TYPE_GATTLINK_SESSION_READY) s->gatt_open = true;
    if (forwarded->type == GG_EVENT_TYPE_GATTLINK_SESSION_RESET) {
        /* Startup emits no reset event. Require this stack's own open anyway. */
        if (s->phase == HANDOFF && s->gatt_open) atomic_store(&s->muted, true);
        s->gatt_open = false;
    }
    if (forwarded->type != GG_EVENT_TYPE_TLS_STATE_CHANGE) return;
    GG_DtlsProtocolStatus status;
    GG_DtlsProtocol_GetStatus((GG_DtlsProtocol*)forwarded->source, &status);
    s->connected = status.state == GG_TLS_STATE_SESSION;
    if (s->connected) s->tls_error = false;
    if (status.state == GG_TLS_STATE_ERROR) {
        s->tls_error = true;
        if (s->phase == TLS && status.last_error == LOST_HANDSHAKE_RACE) {
            uint64_t expedited = now_ms() + 1500;
            if (expedited < s->handshake_deadline) s->handshake_deadline = expedited;
        }
    }
}
GG_IMPLEMENT_INTERFACE(AB_Session, GG_EventListener) {stack_event};
static GG_Result build_stack(AB_Session* s) {
    const uint16_t ciphers[] = {GG_TLS_PSK_WITH_AES_128_CCM,
        GG_TLS_PSK_WITH_AES_128_GCM_SHA256, GG_TLS_ECDHE_PSK_WITH_AES_128_CBC_SHA256};
    GG_TlsServerOptions tls = {.base = {.cipher_suites = ciphers, .cipher_suites_count = 3},
                              .key_resolver = GG_CAST(s, GG_TlsKeyResolver)};
    GG_StackElementGattlinkParameters gatt = {.buffer_size = 8192,
        .initial_max_fragment_size = (uint16_t)s->packet_size};
    /* Zero selects the builder's default CoAP ports and HUB IP configuration. */
    GG_StackElementDatagramSocketParameters socket = {0};
    GG_StackBuilderParameters params[] = {{GG_STACK_ELEMENT_TYPE_GATTLINK, &gatt},
        {GG_STACK_ELEMENT_TYPE_DTLS_SERVER, &tls}, {GG_STACK_ELEMENT_TYPE_DATAGRAM_SOCKET, &socket}};
    GG_Result r = GG_StackBuilder_BuildStack("DSNG", params, 3, GG_STACK_ROLE_HUB,
        NULL, s->loop, NULL, GG_CAST(s, GG_DataSink), &s->stack);
    if (GG_FAILED(r)) return r;
    r = GG_Stack_GetPortById(s->stack, GG_STACK_ELEMENT_ID_BOTTOM, GG_STACK_PORT_ID_BOTTOM, &s->bottom);
    if (GG_FAILED(r)) return r;
    r = GG_Stack_GetPortById(s->stack, GG_STACK_ELEMENT_ID_TOP, GG_STACK_PORT_ID_TOP, &s->top);
    if (GG_FAILED(r)) return r;
    GG_EventEmitter_SetListener(GG_Stack_AsEventEmitter(s->stack), GG_CAST(s, GG_EventListener));
    r = GG_CoapEndpoint_Create(GG_Loop_GetTimerScheduler(s->loop), s->top.sink, s->top.source, &s->endpoint);
    return GG_FAILED(r) ? r : GG_Stack_Start(s->stack);
}
static void acknowledged(GG_CoapResponseListener* _self) { (void)_self; }
static void request_error(GG_CoapResponseListener* _self, GG_Result error, const char* message) {
    AB_Session* s = GG_SELF(AB_Session, GG_CoapResponseListener);
    (void)message;
    s->reply = error == GG_ERROR_TIMEOUT ? AB_ERR_TIMEOUT : AB_ERR_COAP;
    s->pending = false; s->replied = true;
}
static void response(GG_CoapResponseListener* _self, GG_CoapMessage* response) {
    AB_Session* s = GG_SELF(AB_Session, GG_CoapResponseListener);
    s->reply = (GG_CoapMessage_GetCode(response) >> 5) == 2 ? AB_OK : AB_ERR_COAP;
    if (s->reply == AB_OK && (s->phase == FIRST || s->phase == VERIFY || s->phase == FINAL_READ)) {
        s->reply = AB_Settings_Decode(GG_CoapMessage_GetPayload(response), GG_CoapMessage_GetPayloadSize(response));
        if (!s->reply) s->reply = AB_ERR_SETTING;
    }
    s->pending = false; s->replied = true;
}
GG_IMPLEMENT_INTERFACE(AB_Session, GG_CoapResponseListener) {acknowledged, request_error, response};
static void request(AB_Session* s, Phase phase, int first, int second) {
    s->phase = phase; s->replied = false; s->pending = true;
    s->request_deadline = now_ms() + (phase == RESTORE ? RESTORE_MS : s->request_ms);
    uint8_t payload[12] = {10, 1, 1};
    size_t size = first ? AB_Settings_Encode(payload, first, second) : 3;
    GG_CoapMessageOptionParam path = GG_COAP_MESSAGE_OPTION_PARAM_STRING(URI_PATH, "settings");
    /* The wrapper owns the hard deadline, including an ACK with no response.
     * Keep GG's no-retry fallback later so it cannot mask that deadline. */
    GG_CoapClientParameters params = {.ack_timeout = s->request_ms + 1000, .max_resend_count = 0};
    GG_Result r = GG_CoapEndpoint_SendRequest(s->endpoint, first ? GG_COAP_METHOD_POST : GG_COAP_METHOD_PUT,
        &path, 1, payload, size, &params, GG_CAST(s, GG_CoapResponseListener), &s->request);
    if (GG_FAILED(r)) { s->pending = false; s->replied = true; s->reply = AB_ERR_COAP; }
    else if (first) s->write_sent = true;
}
static void complete(AB_Session* s);
static void handoff(AB_Session* s) {
    clear_stack(s);
    s->phase = HANDOFF;
    s->handoff_deadline = now_ms() + s->handoff_ms;
    log_line(s, "Handoff started");
    if (s->lost) { complete(s); return; }
    if (GG_FAILED(build_stack(s))) log_line(s, "Handoff stack could not start; waiting for channel release");
}
static void fail(AB_Session* s, int error, const char* message) {
    s->result = error; s->message = message;
    if (s->pending) { GG_CoapEndpoint_CancelRequest(s->endpoint, s->request); s->pending = false; }
    if (s->write_sent && s->original && s->phase != RESTORE && s->endpoint && !s->lost) {
        log_line(s, "Attempting best-effort restore");
        request(s, RESTORE, s->original, 0);
    } else handoff(s);
}
static void complete(AB_Session* s) {
    atomic_store(&s->muted, true); s->phase = DONE; clear_stack(s);
    GG_Timer_Unschedule(s->timer);
    log_line(s, "Handoff finished");
    s->done(s->context, s->result, s->original, s->message);
}
static void advance(AB_Session* s) {
    int value = s->reply;
    s->replied = false;
    if (s->phase == RESTORE) { handoff(s); return; }
    if (value < 0) {
        fail(s, value, value == AB_ERR_SETTING ? "Unsupported haptic setting" : "Band request failed or timed out");
        return;
    }
    switch (s->phase) {
        case FIRST:
            s->original = value;
            if (s->operation == AB_OP_READ) handoff(s);
            else request(s, s->operation == AB_OP_TAP ? TAP : ALT, 5 - value,
                         s->operation == AB_OP_TAP ? value : 0);
            break;
        case ALT: s->phase = PAUSE; s->pause_deadline = now_ms() + 800; break;
        case TAP: case ORIGINAL: case CORRECT:
            request(s, s->phase == CORRECT ? FINAL_READ : VERIFY, 0, 0); break;
        case VERIFY:
            if (value == s->original) handoff(s);
            else request(s, CORRECT, s->original, 0);
            break;
        case FINAL_READ:
            if (value == s->original) handoff(s);
            else fail(s, AB_ERR_SETTING, "Could not restore the original strength");
            break;
        default: break;
    }
}
static void begin_handshake(AB_Session* s) {
    clear_stack(s);
    s->phase = TLS; ++s->attempts;
    s->handshake_deadline = now_ms() + HANDSHAKE_RETRY_MS;
    log_line(s, s->attempts == 1 ? "Starting band handshake" : "Retrying band handshake");
    if (GG_FAILED(build_stack(s))) fail(s, AB_ERR_STACK, "Could not build band stack");
}
static void tick(GG_TimerListener* _self, GG_Timer* timer, uint32_t elapsed) {
    AB_Session* s = GG_SELF(AB_Session, GG_TimerListener);
    (void)elapsed;
    uint64_t now = now_ms();
    if (s->phase == HANDOFF) {
        if (atomic_load(&s->muted) || s->lost || now >= s->handoff_deadline) { complete(s); return; }
    } else if (s->phase == RESTORE) {
        /* Restore owns its reserved window even after the work deadline. */
        if (s->replied || s->lost || now >= s->request_deadline) handoff(s);
    } else if (s->phase != IDLE && s->phase != DONE) {
        if (s->lost) fail(s, AB_ERR_LINK, "Band link was lost");
        else if (now >= s->deadline) fail(s, AB_ERR_TIMEOUT, "Band operation timed out");
        else if (s->phase == TLS) {
            if (s->connected) request(s, FIRST, 0, 0);
            else if (now >= s->handshake_deadline) {
                if (s->attempts < MAX_HANDSHAKE_ATTEMPTS) begin_handshake(s);
                else fail(s, s->tls_error ? AB_ERR_TLS : AB_ERR_TIMEOUT, "Band handshake timed out");
            }
        } else if (s->tls_error) fail(s, AB_ERR_TLS, "Band authentication failed");
        else if (s->pending && now >= s->request_deadline) fail(s, AB_ERR_TIMEOUT, "Band request timed out");
        else if (s->phase == PAUSE && now >= s->pause_deadline) request(s, ORIGINAL, s->original, 0);
        else if (s->replied) advance(s);
    }
    if (s->phase != DONE) GG_Timer_Schedule(timer, GG_CAST(s, GG_TimerListener), 5);
}
GG_IMPLEMENT_INTERFACE(AB_Session, GG_TimerListener) {tick};
static void* run_loop(void* context) {
    AB_Session* s = context;
    GG_Loop_BindToCurrentThread(s->loop);
    pthread_mutex_lock(&s->mutex);
    s->initialized = true;
    pthread_cond_signal(&s->ready);
    pthread_mutex_unlock(&s->mutex);
    GG_Loop_Run(s->loop);
    clear_stack(s);
    GG_Timer_Destroy(s->timer);
    GG_Loop_Destroy(s->loop);
    return NULL;
}
AB_Session* AB_Session_Create(AB_SendFn send, AB_DoneFn done, AB_LogFn log, void* context, size_t packet_size) {
    if (!send || !done || packet_size < 20 || packet_size > 65535) return NULL;
    pthread_once(&module_once, initialize_module);
    if (GG_FAILED(module_result)) return NULL;
    AB_Session* s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->send = send; s->done = done; s->log = log; s->context = context; s->packet_size = packet_size;
    atomic_init(&s->muted, false);
    s->operation_ms = 22000; s->handoff_ms = 6000; s->request_ms = 3000; s->message = "Finished";
    GG_SET_INTERFACE(s, AB_Session, GG_DataSink);
    GG_SET_INTERFACE(s, AB_Session, GG_TlsKeyResolver);
    GG_SET_INTERFACE(s, AB_Session, GG_EventListener);
    GG_SET_INTERFACE(s, AB_Session, GG_CoapResponseListener);
    GG_SET_INTERFACE(s, AB_Session, GG_TimerListener);
    if (GG_FAILED(GG_Loop_Create(&s->loop))) { free(s); return NULL; }
    if (GG_FAILED(GG_TimerScheduler_CreateTimer(GG_Loop_GetTimerScheduler(s->loop), &s->timer))) {
        GG_Loop_Destroy(s->loop); free(s); return NULL;
    }
    pthread_mutex_init(&s->mutex, NULL); pthread_cond_init(&s->ready, NULL);
    if (pthread_create(&s->thread, NULL, run_loop, s)) {
        GG_Timer_Destroy(s->timer); GG_Loop_Destroy(s->loop);
        pthread_cond_destroy(&s->ready); pthread_mutex_destroy(&s->mutex); free(s); return NULL;
    }
    pthread_mutex_lock(&s->mutex);
    while (!s->initialized) pthread_cond_wait(&s->ready, &s->mutex);
    pthread_mutex_unlock(&s->mutex);
    return s;
}
static void start(void* context) {
    AB_Session* s = context;
    if (s->aborting || s->phase == DONE) return;
    /* Shared work budget reserves restore and handoff, even after two attempts. */
    s->deadline = now_ms() + s->operation_ms - s->handoff_ms - RESTORE_MS;
    begin_handshake(s);
    GG_Timer_Schedule(s->timer, GG_CAST(s, GG_TimerListener), 5);
}
int AB_Session_Start(AB_Session* s, AB_Operation op) {
    if (!s || op < AB_OP_READ || op > AB_OP_DOUBLE) return AB_ERR_ARGUMENT;
    pthread_mutex_lock(&s->mutex);
    int result = AB_ERR_BUSY;
    if (!s->started) {
        s->started = true; s->operation = op;
        result = GG_SUCCEEDED(GG_Loop_InvokeAsync(s->loop, start, s)) ? AB_OK : AB_ERR_STACK;
    }
    pthread_mutex_unlock(&s->mutex);
    return result;
}
typedef struct { AB_Session* session; size_t size; uint8_t bytes[]; } Packet;
static void receive(void* context) {
    Packet* p = context;
    AB_Session* s = p->session;
    if (s->stack && !atomic_load(&s->muted) && s->phase != DONE) {
        GG_StaticBuffer buffer;
        GG_StaticBuffer_Init(&buffer, p->bytes, p->size);
        GG_DataSink_PutData(s->bottom.sink, GG_StaticBuffer_AsBuffer(&buffer), NULL);
    }
    free(p);
}
void AB_Session_Receive(AB_Session* s, const uint8_t* bytes, size_t size) {
    if (!s || !bytes || !size || size > s->packet_size) return;
    Packet* p = malloc(sizeof(*p) + size);
    if (!p) return;
    p->session = s; p->size = size; memcpy(p->bytes, bytes, size);
    if (GG_FAILED(GG_Loop_InvokeAsync(s->loop, receive, p))) free(p);
}
static void link_lost(void* context) { ((AB_Session*)context)->lost = true; }
void AB_Session_LinkLost(AB_Session* s) { if (s) GG_Loop_InvokeAsync(s->loop, link_lost, s); }
static void abort_session(void* context) {
    AB_Session* s = context;
    if (s->phase == DONE || s->aborting) return;
    s->aborting = true;
    s->handoff_ms = 1000; /* 1.5 s restore + 1 s handoff leaves app cleanup slack. */
    s->result = AB_ERR_TIMEOUT; s->message = "Band operation aborted";
    if (s->phase == IDLE) { complete(s); return; }
    if (s->phase == HANDOFF) {
        uint64_t limit = now_ms() + s->handoff_ms;
        if (limit < s->handoff_deadline) s->handoff_deadline = limit;
    } else if (s->phase != RESTORE) fail(s, s->result, s->message);
    if (s->phase != DONE) GG_Timer_Schedule(s->timer, GG_CAST(s, GG_TimerListener), 5);
}
void AB_Session_Abort(AB_Session* s) { if (s) GG_Loop_InvokeAsync(s->loop, abort_session, s); }
int AB_Session_IsMuted(AB_Session* s) { return !s || atomic_load(&s->muted); }
static void stop(void* context) { GG_Loop_RequestTermination(((AB_Session*)context)->loop); }
void AB_Session_Destroy(AB_Session* s) {
    if (!s) return;
    /* External callers have stopped: GG drains their earlier FIFO messages
     * (including owned RX packets) before executing this stop callback. */
    GG_Loop_InvokeAsync(s->loop, stop, s);
    pthread_join(s->thread, NULL);
    pthread_cond_destroy(&s->ready); pthread_mutex_destroy(&s->mutex); free(s);
}
#ifdef AB_TESTING
void AB_Session_SetTimeouts(AB_Session* s, unsigned operation, unsigned handoff_ms, unsigned request_ms) {
    if (!s || !handoff_ms || operation <= handoff_ms + RESTORE_MS || !request_ms) return;
    pthread_mutex_lock(&s->mutex);
    if (!s->started) { s->operation_ms = operation; s->handoff_ms = handoff_ms; s->request_ms = request_ms; }
    pthread_mutex_unlock(&s->mutex);
}
#endif
