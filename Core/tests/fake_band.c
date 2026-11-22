/* Included by loopback_test.c so the test fixture needs no public header. */
#include "xp/coap/gg_coap.h"
#include "xp/common/gg_buffer.h"
#include "xp/common/gg_io.h"
#include "xp/common/gg_logging.h"
#include "xp/loop/gg_loop.h"
#include "xp/loop/extensions/gg_loop_fd.h"
#include "xp/gattlink/gg_gattlink_generic_client.h"
#include "xp/module/gg_module.h"
#include "xp/stack_builder/gg_stack_builder.h"
#include "xp/tls/gg_tls.h"
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    int intensity;
    size_t packet_size;
    bool bad_identity, drop_after_post, partial_post;
    bool ignore_first, ack_only, restore_deadline, abort_after_post;
} BandConfig;

typedef struct { char type; int value; double time; } BandEvent;

typedef struct {
    GG_LoopFileDescriptorEventHandler handler;
    void* owner;
    bool control;
} BandFd;

typedef struct {
    GG_IMPLEMENTS(GG_DataSink);
    GG_IMPLEMENTS(GG_EventListener);
    GG_IMPLEMENTS(GG_CoapRequestHandler);
    GG_Loop* loop;
    GG_Stack* stack;
    GG_CoapEndpoint* endpoint;
    GG_StackElementPortInfo bottom, top;
    BandConfig config;
    int transport, control, posts, links, after_reset;
    bool dropping, withholding, reset_sent;
    BandFd transport_handler, control_handler;
} FakeBand;

static double now_seconds(void) {
    struct timespec time;
    assert(clock_gettime(CLOCK_MONOTONIC, &time) == 0);
    return (double)time.tv_sec + (double)time.tv_nsec / 1000000000.0;
}

static void band_event(FakeBand* band, char type, int value) {
    BandEvent event = {.type = type, .value = value, .time = now_seconds()};
    assert(send(band->control, &event, sizeof(event), 0) == sizeof(event));
}

static GG_Result band_send(GG_DataSink* _self, GG_Buffer* buffer,
                           const GG_BufferMetadata* metadata) {
    FakeBand* band = GG_SELF(FakeBand, GG_DataSink);
    (void)metadata;
    size_t size = GG_Buffer_GetDataSize(buffer);
    assert(size <= band->config.packet_size);
    if (!band->dropping && (!band->withholding || (GG_Buffer_GetData(buffer)[0] & 0x80))) {
        assert(send(band->transport, GG_Buffer_GetData(buffer), size, 0) == (ssize_t)size);
    }
    return GG_SUCCESS;
}

static GG_Result band_sink_listener(GG_DataSink* _self, GG_DataSinkListener* listener) {
    (void)_self;
    (void)listener;
    return GG_SUCCESS;
}

GG_IMPLEMENT_INTERFACE(FakeBand, GG_DataSink) {band_send, band_sink_listener};

static void band_reset(void* context) {
    FakeBand* band = context;
    band->reset_sent = true;
    band->after_reset = 0;
    band_event(band, 'R', 0);
    assert(GG_Stack_Reset(band->stack) == GG_SUCCESS);
}

static void band_on_event(GG_EventListener* _self, const GG_Event* event) {
    FakeBand* band = GG_SELF(FakeBand, GG_EventListener);
    if (event->type != GG_EVENT_TYPE_STACK_EVENT_FORWARD) return;
    const GG_Event* forwarded = ((const GG_StackForwardEvent*)event)->forwarded;
    if (forwarded->type == GG_EVENT_TYPE_GATTLINK_SESSION_READY) {
        ++band->links;
        band->reset_sent = false;
        band_event(band, 'G', 0);
    }
    if (forwarded->type == GG_EVENT_TYPE_GATTLINK_SESSION_RESET) band->withholding = false;
    if (forwarded->type != GG_EVENT_TYPE_TLS_STATE_CHANGE) return;
    GG_DtlsProtocolStatus status;
    GG_DtlsProtocol_GetStatus((GG_DtlsProtocol*)forwarded->source, &status);
    if (status.state == GG_TLS_STATE_SESSION) band_event(band, 'S', 0);
    if (status.state == GG_TLS_STATE_ERROR) {
        /* Air resets Gattlink after refusal, without dropping the BLE transport. */
        assert(status.last_error == GG_ERROR_TLS_FATAL_ALERT_MESSAGE);
        assert(GG_Loop_InvokeAsync(band->loop, band_reset, band) == GG_SUCCESS);
    }
}

GG_IMPLEMENT_INTERFACE(FakeBand, GG_EventListener) {band_on_event};

static GG_Result empty_ack(const GG_CoapMessage* request, GG_CoapMessage** response) {
    /* A wire-format empty ACK has no token or payload. Pinned GG classifies
     * code zero as a request, so the wrapper must bound this case too. */
    return GG_CoapMessage_Create(0, GG_COAP_MESSAGE_TYPE_ACK, NULL, 0,
        GG_CoapMessage_GetMessageId(request), NULL, 0, NULL, 0, response);
}

static GG_Result band_request(GG_CoapRequestHandler* _self, GG_CoapEndpoint* endpoint,
                              const GG_CoapMessage* request, GG_CoapResponder* responder,
                              const GG_BufferMetadata* metadata, GG_CoapMessage** response) {
    FakeBand* band = GG_SELF(FakeBand, GG_CoapRequestHandler);
    (void)responder;
    (void)metadata;
    const uint8_t* payload = GG_CoapMessage_GetPayload(request);
    size_t size = GG_CoapMessage_GetPayloadSize(request);
    uint8_t code = GG_CoapMessage_GetCode(request);
    const uint8_t read_payload[] = {0x0a, 1, 1};
    const uint8_t prefix[] = {0x0a, 4, 0x0a, 2, 8};
    if (code == GG_COAP_METHOD_PUT) {
        if (size != sizeof(read_payload) || memcmp(payload, read_payload, size)) {
            return GG_COAP_MESSAGE_CODE_BAD_REQUEST;
        }
        uint8_t reply[] = {0x0a, 4, 0x0a, 2, 8, (uint8_t)band->config.intensity};
        band_event(band, 'Q', band->config.intensity);
        if (band->config.ack_only) return empty_ack(request, response);
        return GG_CoapEndpoint_CreateResponse(endpoint, request, GG_COAP_MESSAGE_CODE_CONTENT,
                                              NULL, 0, reply, sizeof(reply), response);
    }
    if (code != GG_COAP_METHOD_POST || !size || size % 6) {
        return GG_COAP_MESSAGE_CODE_BAD_REQUEST;
    }
    for (size_t i = 0; i < size; i += 6) {
        if (memcmp(payload + i, prefix, 5) || (payload[i + 5] != 2 && payload[i + 5] != 3)) {
            return GG_COAP_MESSAGE_CODE_BAD_REQUEST;
        }
    }
    ++band->posts;
    band_event(band, 'P', band->posts);
    for (size_t i = 0; i < size; i += 6) {
        if (band->config.partial_post && band->posts == 1 && i == 6) break;
        int value = payload[i + 5];
        if (value != band->config.intensity) {
            band->config.intensity = value;
            band_event(band, 'A', value);
        }
    }
    if (band->config.drop_after_post) band->dropping = true;
    if (band->config.restore_deadline && band->posts == 1) return empty_ack(request, response);
    return GG_CoapEndpoint_CreateResponse(endpoint, request, GG_COAP_MESSAGE_CODE_CHANGED,
                                          NULL, 0, NULL, 0, response);
}

GG_IMPLEMENT_INTERFACE(FakeBand, GG_CoapRequestHandler) {band_request};

static void band_fd_event(GG_LoopEventHandler* _self, GG_Loop* loop) {
    BandFd* fd = GG_SELF_M(handler.base, BandFd, GG_LoopEventHandler);
    FakeBand* band = fd->owner;
    uint8_t packet[65536];
    ssize_t count = recv(fd->handler.fd, packet, sizeof(packet), 0);
    assert(count > 0);
    if (fd->control) {
        assert(count == 1 && packet[0] == 'X');
        GG_Loop_RequestTermination(loop);
    } else {
        assert((size_t)count <= band->config.packet_size);
        if (band->reset_sent) ++band->after_reset;
        if (!band->dropping) {
            GG_StaticBuffer buffer;
            GG_StaticBuffer_Init(&buffer, packet, (size_t)count);
            assert(GG_DataSink_PutData(band->bottom.sink,
                GG_StaticBuffer_AsBuffer(&buffer), NULL) == GG_SUCCESS);
        }
    }
}

GG_IMPLEMENT_INTERFACE(BandFd, GG_LoopEventHandler) {band_fd_event};

static void band_monitor(FakeBand* band, BandFd* fd, int socket, bool control) {
    fd->handler.fd = socket;
    fd->handler.event_mask = GG_EVENT_FLAG_FD_CAN_READ;
    fd->owner = band;
    fd->control = control;
    GG_SET_INTERFACE(&fd->handler.base, BandFd, GG_LoopEventHandler);
    assert(GG_Loop_AddFileDescriptorHandler(band->loop, &fd->handler) == GG_SUCCESS);
}

static void fake_band_run(BandConfig config, int transport, int control) {
    FakeBand band = {.config = config, .transport = transport, .control = control,
                     .withholding = config.ignore_first};
    assert(GG_Module_Initialize() == GG_SUCCESS);
    assert(GG_LogManager_Configure("plist:.level=WARNING") == GG_SUCCESS);
    assert(GG_Loop_Create(&band.loop) == GG_SUCCESS);
    assert(GG_Loop_BindToCurrentThread(band.loop) == GG_SUCCESS);
    GG_SET_INTERFACE(&band, FakeBand, GG_DataSink);
    GG_SET_INTERFACE(&band, FakeBand, GG_EventListener);
    GG_SET_INTERFACE(&band, FakeBand, GG_CoapRequestHandler);
    const uint8_t key[] = {0x81, 6, 0x54, 0xe3, 0x36, 0xad, 0xca, 0xb0,
                           0xa0, 0x3c, 0x60, 0xf7, 0x4a, 0xa0, 0xb6, 0xfb};
    const uint16_t ciphers[] = {GG_TLS_PSK_WITH_AES_128_CCM,
        GG_TLS_PSK_WITH_AES_128_GCM_SHA256, GG_TLS_ECDHE_PSK_WITH_AES_128_CBC_SHA256};
    GG_TlsClientOptions tls = {
        .base = {.cipher_suites = ciphers, .cipher_suites_count = 3},
        .psk_identity = (const uint8_t*)(config.bad_identity ? "WRONG_KEY" : "BOOTSTRAP"),
        .psk_identity_size = 9, .psk = key, .psk_size = sizeof(key)
    };
    GG_StackElementGattlinkParameters gatt = {
        .buffer_size = 8192, .initial_max_fragment_size = (uint16_t)config.packet_size
    };
    GG_StackElementDatagramSocketParameters socket = {.local_port = 0, .remote_port = 0};
    GG_StackBuilderParameters parameters[] = {
        {GG_STACK_ELEMENT_TYPE_GATTLINK, &gatt},
        {GG_STACK_ELEMENT_TYPE_DTLS_CLIENT, &tls},
        {GG_STACK_ELEMENT_TYPE_DATAGRAM_SOCKET, &socket}
    };
    assert(GG_StackBuilder_BuildStack("DSNG", parameters, 3, GG_STACK_ROLE_NODE,
        NULL, band.loop, NULL, GG_CAST(&band, GG_DataSink), &band.stack) == GG_SUCCESS);
    assert(GG_Stack_GetPortById(band.stack, GG_STACK_ELEMENT_ID_BOTTOM,
        GG_STACK_PORT_ID_BOTTOM, &band.bottom) == GG_SUCCESS);
    assert(GG_Stack_GetPortById(band.stack, GG_STACK_ELEMENT_ID_TOP,
        GG_STACK_PORT_ID_TOP, &band.top) == GG_SUCCESS);
    assert(GG_CoapEndpoint_Create(GG_Loop_GetTimerScheduler(band.loop),
        band.top.sink, band.top.source, &band.endpoint) == GG_SUCCESS);
    assert(GG_CoapEndpoint_RegisterRequestHandler(band.endpoint, "settings",
        GG_COAP_REQUEST_HANDLER_FLAG_ALLOW_PUT | GG_COAP_REQUEST_HANDLER_FLAG_ALLOW_POST,
        GG_CAST(&band, GG_CoapRequestHandler)) == GG_SUCCESS);
    GG_EventEmitter_SetListener(GG_Stack_AsEventEmitter(band.stack), GG_CAST(&band, GG_EventListener));
    assert(GG_Stack_Start(band.stack) == GG_SUCCESS);
    band_event(&band, 'B', 0);
    band_monitor(&band, &band.transport_handler, transport, false);
    band_monitor(&band, &band.control_handler, control, true);
    assert(GG_Loop_Run(band.loop) == GG_SUCCESS);
    assert(GG_Loop_RemoveFileDescriptorHandler(band.loop,
        &band.transport_handler.handler) == GG_SUCCESS);
    assert(GG_Loop_RemoveFileDescriptorHandler(band.loop,
        &band.control_handler.handler) == GG_SUCCESS);
    /* Drain already delivered hub packets before reporting the final count. */
    uint8_t queued[65536];
    ssize_t size;
    while ((size = recv(transport, queued, sizeof(queued), MSG_DONTWAIT)) > 0) {
        assert((size_t)size <= config.packet_size);
        if (band.reset_sent) ++band.after_reset;
    }
    band_event(&band, 'N', band.after_reset);
    band_event(&band, 'F', band.config.intensity);
    GG_DataSource_SetDataSink(band.top.source, NULL);
    GG_CoapEndpoint_Destroy(band.endpoint);
    GG_Stack_Destroy(band.stack);
    GG_Loop_Destroy(band.loop);
    GG_Module_Terminate();
    close(transport);
    close(control);
}
