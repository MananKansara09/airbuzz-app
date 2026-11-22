#include "ab_session.h"
#include "fake_band.c"
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/wait.h>

/* Fork then exec keeps Golden Gate's process-global network state isolated. */
typedef struct {
    pthread_mutex_t mutex;
    int transport, result, intensity, done_count, refused;
    size_t packet_size, sent, received;
    double handoff_time, done_time;
    char message[160];
} TestContext;

typedef struct {
    int values[16], applies, posts, reads, resets, sessions, links, final, after_reset;
    double apply_times[16], reset_time, first_read;
} Observations;

static void test_send(void* raw, const uint8_t* data, size_t size) {
    TestContext* context = raw;
    assert(size > 0 && size <= context->packet_size);
    assert(send(context->transport, data, size, 0) == (ssize_t)size);
    pthread_mutex_lock(&context->mutex);
    ++context->sent;
    pthread_mutex_unlock(&context->mutex);
}

static void test_done(void* raw, int result, int intensity, const char* message) {
    TestContext* context = raw;
    pthread_mutex_lock(&context->mutex);
    ++context->done_count;
    context->result = result;
    context->intensity = intensity;
    context->done_time = now_seconds();
    snprintf(context->message, sizeof(context->message), "%s", message ? message : "");
    pthread_mutex_unlock(&context->mutex);
}

static void test_log(void* raw, const char* line) {
    TestContext* context = raw;
    pthread_mutex_lock(&context->mutex);
    if (strstr(line, "Handoff started")) {
        assert(context->handoff_time == 0);
        context->handoff_time = now_seconds();
    }
    if (strstr(line, "Handoff identity refused")) ++context->refused;
    fprintf(stderr, "core: %s\n", line);
    pthread_mutex_unlock(&context->mutex);
}

static void observe(BandEvent event, Observations* seen, double handoff_time) {
    switch (event.type) {
        case 'A':
            assert(seen->applies < 16);
            seen->values[seen->applies] = event.value;
            seen->apply_times[seen->applies++] = event.time;
            break;
        case 'P': ++seen->posts; break;
        case 'Q': if (!seen->reads++) seen->first_read = event.time; break;
        case 'S': ++seen->sessions; break;
        case 'G': ++seen->links; break;
        case 'F': seen->final = event.value; break;
        case 'N': seen->after_reset = event.value; break;
        case 'R':
            if (handoff_time > 0 && event.time >= handoff_time) {
                ++seen->resets;
                seen->reset_time = event.time;
            }
            break;
        default: assert(event.type == 'B'); break;
    }
}

static pid_t spawn_band(const char* executable, BandConfig config,
                        int transport[2], int control[2]) {
    assert(socketpair(AF_UNIX, SOCK_DGRAM, 0, transport) == 0);
    assert(socketpair(AF_UNIX, SOCK_DGRAM, 0, control) == 0);
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        close(transport[0]); close(control[0]);
        char settings[160];
        snprintf(settings, sizeof(settings), "%d,%zu,%d,%d,%d,%d,%d,%d,%d,%d", config.intensity,
            config.packet_size, config.bad_identity, config.drop_after_post,
            config.partial_post, config.ignore_first, config.ack_only,
            config.restore_deadline, transport[1], control[1]);
        execl(executable, executable, "--band", settings, (char*)NULL);
        _exit(127);
    }
    close(transport[1]); close(control[1]);
    return child;
}

static void run_case(const char* executable, const char* name, BandConfig config,
                     AB_Operation operation, bool success) {
    fprintf(stderr, "CASE %s (%zu-byte transport)\n", name, config.packet_size);
    int transport[2], control[2];
    pid_t child = spawn_band(executable, config, transport, control);
    TestContext context = {.mutex = PTHREAD_MUTEX_INITIALIZER,
        .transport = transport[0], .packet_size = config.packet_size};
    Observations seen = {.final = -1, .after_reset = -1};
    AB_Session* session = AB_Session_Create(test_send, test_done, test_log,
                                           &context, config.packet_size);
    assert(session);
    unsigned operation_ms = (config.drop_after_post || config.restore_deadline) ? 4000 : 22000;
    unsigned handoff_ms = (config.drop_after_post || config.restore_deadline) ? 1000 : 6000;
    AB_Session_SetTimeouts(session, operation_ms, handoff_ms,
                           config.drop_after_post ? 500 : 3000);
    double start = now_seconds();
    assert(AB_Session_Start(session, operation) == AB_OK);
    assert(AB_Session_Start(session, operation) < 0);
    bool completed = false;
    double aborted = 0;
    while (!completed) {
        assert(now_seconds() - start < (operation_ms + handoff_ms) / 1000.0 + 2);
        struct pollfd descriptors[] = {{transport[0], POLLIN, 0}, {control[0], POLLIN, 0}};
        assert(poll(descriptors, 2, 10) >= 0);
        /* Diagnostic events never drive the core: only transport packets do. */
        BandEvent event;
        while (recv(control[0], &event, sizeof(event), MSG_DONTWAIT) == sizeof(event)) {
            pthread_mutex_lock(&context.mutex);
            double handoff = context.handoff_time;
            pthread_mutex_unlock(&context.mutex);
            observe(event, &seen, handoff);
        }
        if (config.abort_after_post && seen.applies && !aborted) {
            aborted = now_seconds();
            AB_Session_Abort(session);
        }
        uint8_t packet[65536];
        ssize_t size;
        while ((size = recv(transport[0], packet, sizeof(packet), MSG_DONTWAIT)) > 0) {
            assert((size_t)size <= config.packet_size);
            ++context.received;
            AB_Session_Receive(session, packet, (size_t)size);
        }
        pthread_mutex_lock(&context.mutex);
        completed = context.done_count != 0;
        pthread_mutex_unlock(&context.mutex);
        int child_status;
        assert(waitpid(child, &child_status, WNOHANG) == 0);
    }
    assert(AB_Session_IsMuted(session));
    AB_Session_Destroy(session);
    /* The reset diagnostic may arrive after the last control drain but before
     * transport delivery makes done visible. It never drives core state. */
    BandEvent late;
    while (recv(control[0], &late, sizeof(late), MSG_DONTWAIT) == sizeof(late)) {
        observe(late, &seen, context.handoff_time);
    }
    assert(context.done_count == 1);
    assert(context.sent > 0 && context.received > 0);
    assert(context.handoff_time > start && context.done_time >= context.handoff_time);
    assert(context.done_time - context.handoff_time <= handoff_ms / 1000.0 + 0.5);
    assert(context.done_time - start <= operation_ms / 1000.0 + 0.5);
    assert(context.done_time - start < 22.1);
    fprintf(stderr, "result=%d intensity=%d message=%s\n", context.result,
            context.intensity, context.message);
    assert((context.result == AB_OK) == success);
    if (!success) assert(context.result < 0);
    if (config.drop_after_post) {
        assert(context.done_time - context.handoff_time >= handoff_ms / 1000.0 - 0.1);
        assert(seen.resets == 0);
    } else {
        assert(context.done_time - context.handoff_time < 3);
        assert(context.refused > 0);
        assert(seen.resets > 0 && seen.reset_time <= context.done_time);
        assert(seen.links >= 2);
    }
    assert(send(control[0], "X", 1, 0) == 1);
    int status;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    BandEvent event;
    while (recv(control[0], &event, sizeof(event), MSG_DONTWAIT) == sizeof(event)) {
        assert(event.type != 'R');
        observe(event, &seen, context.handoff_time);
    }
    assert(seen.final == config.intensity);
    if (!config.drop_after_post) assert(seen.after_reset == 0);
    if (config.ignore_first) {
        assert(seen.links >= 3 && seen.sessions == 1);
        assert(seen.first_read - start >= 5.9 && seen.first_read - start < 9);
    }
    if (config.ack_only) {
        assert(context.result == AB_ERR_TIMEOUT);
        assert(!strcmp(context.message, "Band request timed out"));
        assert(context.handoff_time - seen.first_read >= 2.9);
        assert(context.handoff_time - seen.first_read < 3.2);
        assert(context.done_time - seen.first_read < 6);
    }
    if (config.restore_deadline) {
        assert(context.result == AB_ERR_TIMEOUT && seen.posts == 2);
        /* Use the core's integer-millisecond clock, not a fractional-ms epoch. */
        double work_deadline = ((uint64_t)(start * 1000) + operation_ms - handoff_ms - 1500) / 1000.0;
        assert(seen.apply_times[1] >= work_deadline);
        assert(seen.apply_times[1] - work_deadline < 1.5);
        assert(context.handoff_time >= seen.apply_times[1]);
    }
    if (config.abort_after_post) {
        assert(aborted > 0 && context.done_time - aborted < 3);
        assert(seen.posts == 2);
    }
    if (success) assert(context.intensity == config.intensity);
    if (operation == AB_OP_READ || config.bad_identity || config.ack_only) {
        assert(seen.applies == 0 && seen.posts == 0);
    } else {
        assert(seen.applies == 2);
        assert(seen.values[0] == (config.intensity == 2 ? 3 : 2));
        assert(seen.values[1] == config.intensity);
        if (operation == AB_OP_DOUBLE && !config.abort_after_post) {
            assert(seen.apply_times[1] - seen.apply_times[0] >= 0.7);
            assert(seen.posts == 2);
        } else if (config.restore_deadline || config.abort_after_post) {
            assert(seen.posts == 2);
        } else if (config.partial_post) {
            assert(seen.posts == 2 && seen.reads >= 3);
        } else if (!config.drop_after_post) {
            assert(seen.posts == 1 && seen.reads >= 2);
        }
    }
    close(transport[0]); close(control[0]);
    pthread_mutex_destroy(&context.mutex);
    fprintf(stderr, "PASS %s\n", name);
}

static void test_already_lost(size_t packet_size, bool abort_before_start) {
    int transport[2];
    assert(socketpair(AF_UNIX, SOCK_DGRAM, 0, transport) == 0);
    TestContext context = {.mutex = PTHREAD_MUTEX_INITIALIZER,
        .transport = transport[0], .packet_size = packet_size};
    AB_Session* session = AB_Session_Create(test_send, test_done, test_log, &context, packet_size);
    assert(session);
    double started = now_seconds();
    if (abort_before_start) AB_Session_Abort(session);
    else {
        AB_Session_LinkLost(session);
        assert(AB_Session_Start(session, AB_OP_TAP) == AB_OK);
    }
    for (;;) {
        pthread_mutex_lock(&context.mutex);
        bool done = context.done_count != 0;
        pthread_mutex_unlock(&context.mutex);
        if (done) break;
        assert(now_seconds() - started < 0.5);
        struct timespec delay = {.tv_nsec = 1000000};
        nanosleep(&delay, NULL);
    }
    assert(AB_Session_IsMuted(session));
    AB_Session_Destroy(session);
    assert(context.done_count == 1);
    assert(context.result == (abort_before_start ? AB_ERR_TIMEOUT : AB_ERR_LINK));
    if (abort_before_start) assert(context.sent == 0);
    else assert(context.done_time - context.handoff_time < 0.05);
    close(transport[0]); close(transport[1]);
    pthread_mutex_destroy(&context.mutex);
    puts(abort_before_start ? "PASS abort before start sends nothing" :
         "PASS already lost link completes handoff immediately");
}

static void test_destroy(const char* executable, size_t packet_size) {
    TestContext empty = {.mutex = PTHREAD_MUTEX_INITIALIZER, .packet_size = packet_size};
    AB_Session* unused = AB_Session_Create(test_send, test_done, test_log, &empty, packet_size);
    assert(unused);
    AB_Session_Destroy(unused);
    assert(empty.done_count == 0 && empty.sent == 0);
    pthread_mutex_destroy(&empty.mutex);

    BandConfig config = {.intensity = 3, .packet_size = packet_size};
    int transport[2], control[2];
    pid_t child = spawn_band(executable, config, transport, control);
    TestContext context = {.mutex = PTHREAD_MUTEX_INITIALIZER,
        .transport = transport[0], .packet_size = config.packet_size};
    AB_Session* session = AB_Session_Create(test_send, test_done, test_log,
                                           &context, config.packet_size);
    assert(session);
    AB_Session_SetTimeouts(session, 12000, 5000, 3000);
    assert(AB_Session_Start(session, AB_OP_DOUBLE) == AB_OK);
    double started = now_seconds();
    bool post_seen = false;
    while (!post_seen) {
        assert(now_seconds() - started < 5);
        struct pollfd fds[] = {{transport[0], POLLIN, 0}, {control[0], POLLIN, 0}};
        assert(poll(fds, 2, 10) >= 0);
        uint8_t packet[65536];
        ssize_t size;
        while ((size = recv(transport[0], packet, sizeof(packet), MSG_DONTWAIT)) > 0) {
            assert((size_t)size <= config.packet_size);
            AB_Session_Receive(session, packet, (size_t)size);
        }
        BandEvent event;
        while (recv(control[0], &event, sizeof(event), MSG_DONTWAIT) == sizeof(event)) {
            if (event.type == 'P') post_seen = true;
            assert(event.type != 'R');
        }
    }
    /* The response and 800 ms double timer may still be queued during teardown. */
    AB_Session_Destroy(session);
    assert(context.done_count == 0);
    assert(send(control[0], "X", 1, 0) == 1);
    int status;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    close(transport[0]); close(control[0]);
    pthread_mutex_destroy(&context.mutex);
    puts("PASS destroy before start and during double");
}

int main(int argc, char** argv) {
    if (argc == 3 && !strcmp(argv[1], "--band")) {
        BandConfig config = {0};
        int bad, drop, partial, ignore, ack, restore, transport, control;
        assert(sscanf(argv[2], "%d,%zu,%d,%d,%d,%d,%d,%d,%d,%d", &config.intensity,
            &config.packet_size, &bad, &drop, &partial, &ignore, &ack, &restore,
            &transport, &control) == 10);
        config.ignore_first = ignore; config.ack_only = ack; config.restore_deadline = restore;
        config.bad_identity = bad; config.drop_after_post = drop; config.partial_post = partial;
        fake_band_run(config, transport, control);
        return 0;
    }
    assert(argc == 1);
    const size_t packet_sizes[] = {244, 60};
    for (size_t index = 0; index < 2; ++index) {
        BandConfig config = {.intensity = 3, .packet_size = packet_sizes[index]};
        run_case(argv[0], "read high", config, AB_OP_READ, true);
        config.intensity = 2;
        run_case(argv[0], "read low", config, AB_OP_READ, true);
        config.intensity = 1;
        run_case(argv[0], "reject off", config, AB_OP_READ, false);
        config.intensity = 2;
        run_case(argv[0], "tap low", config, AB_OP_TAP, true);
        config.intensity = 3;
        run_case(argv[0], "tap high", config, AB_OP_TAP, true);
        run_case(argv[0], "double high", config, AB_OP_DOUBLE, true);
        config.partial_post = true;
        run_case(argv[0], "verify repair", config, AB_OP_TAP, true);
        config.partial_post = false; config.bad_identity = true;
        run_case(argv[0], "reject wrong identity", config, AB_OP_TAP, false);
        config.bad_identity = false; config.drop_after_post = true;
        run_case(argv[0], "drop after post", config, AB_OP_TAP, false);
        config.drop_after_post = false; config.ignore_first = true;
        run_case(argv[0], "retry first handshake", config, AB_OP_TAP, true);
        config.ignore_first = false; config.ack_only = true;
        run_case(argv[0], "empty ACK without response", config, AB_OP_TAP, false);
        config.ack_only = false; config.restore_deadline = config.partial_post = true;
        run_case(argv[0], "restore at deadline", config, AB_OP_TAP, false);
        config.restore_deadline = config.partial_post = false; config.abort_after_post = true;
        run_case(argv[0], "abort during double", config, AB_OP_DOUBLE, false);
        test_destroy(argv[0], config.packet_size);
        test_already_lost(config.packet_size, false);
        test_already_lost(config.packet_size, true);
    }
    puts("All Golden Gate loopback assertions passed.");
    return 0;
}
