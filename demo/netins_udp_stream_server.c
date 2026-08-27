#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <xquic/netins_udp.h>

#include "netins_udp_stream_demo.h"

typedef struct stream_demo_receiver_s {
    uint32_t seed;
    uint32_t callback_delay_ms;
    size_t client_index;
    uint64_t received;
    uint64_t first_bad_offset;
    int progress_reported;
    int mismatch;
} stream_demo_receiver_t;

typedef struct stream_demo_server_options_s {
    const char *listen_ip;
    uint64_t expected_bytes;
    size_t receive_queue_size;
    size_t client_count;
    uint32_t seed;
    uint32_t finish_timeout_ms;
    uint32_t callback_delay_ms;
    uint32_t accept_delay_ms;
    uint16_t port;
} stream_demo_server_options_t;

static void
stream_demo_server_usage(const char *program)
{
    fprintf(stderr,
        "usage: %s LISTEN_IP PORT EXPECTED_BYTES SEED"
        " [RECEIVE_QUEUE_BYTES [FINISH_TIMEOUT_MS [CLIENTS]]]"
        " [--callback-delay-ms N] [--accept-delay-ms N]\n"
        "  lanes are fixed at %u; delay options are test-only and default to 0\n",
        program, NETINS_UDP_STREAM_DEMO_LANE_COUNT);
}

static int
stream_demo_parse_u64(const char *text, uint64_t maximum, uint64_t *out)
{
    char *end = NULL;
    unsigned long long value;

    errno = 0;
    value = strtoull(text, &end, 10);
    if (text[0] == '-' || errno != 0 || end == text || *end != '\0'
        || value > maximum)
    {
        return -1;
    }
    *out = (uint64_t)value;
    return 0;
}

static int
stream_demo_parse_args(int argc, char **argv,
    stream_demo_server_options_t *options)
{
    uint64_t number;
    int positional = 0;
    int i;

    if (argc < 5) {
        return -1;
    }
    memset(options, 0, sizeof(*options));
    options->listen_ip = argv[1];
    options->receive_queue_size = NETINS_UDP_STREAM_DEFAULT_QUEUE_SIZE;
    options->client_count = 1;
    options->finish_timeout_ms = 60000;
    if (stream_demo_parse_u64(argv[2], UINT16_MAX, &number) != 0
        || number == 0)
    {
        return -1;
    }
    options->port = (uint16_t)number;
    if (stream_demo_parse_u64(argv[3], UINT64_MAX,
            &options->expected_bytes) != 0
        || stream_demo_parse_u64(argv[4], UINT32_MAX, &number) != 0)
    {
        return -1;
    }
    options->seed = (uint32_t)number;

    for (i = 5; i < argc; i++) {
        if (strcmp(argv[i], "--callback-delay-ms") == 0
            || strcmp(argv[i], "--accept-delay-ms") == 0)
        {
            int callback_delay = strcmp(argv[i],
                "--callback-delay-ms") == 0;
            if (++i >= argc
                || stream_demo_parse_u64(argv[i], UINT32_MAX,
                    &number) != 0)
            {
                return -1;
            }
            if (callback_delay) {
                options->callback_delay_ms = (uint32_t)number;
            } else {
                options->accept_delay_ms = (uint32_t)number;
            }
            continue;
        }
        if (strncmp(argv[i], "--", 2) == 0
            || stream_demo_parse_u64(argv[i], UINT64_MAX, &number) != 0)
        {
            return -1;
        }
        switch (positional++) {
        case 0:
            if (number > SIZE_MAX) {
                return -1;
            }
            options->receive_queue_size = (size_t)number;
            break;
        case 1:
            if (number > UINT32_MAX) {
                return -1;
            }
            options->finish_timeout_ms = (uint32_t)number;
            break;
        case 2:
            if (number == 0 || number > SIZE_MAX) {
                return -1;
            }
            options->client_count = (size_t)number;
            break;
        default:
            return -1;
        }
    }
    return 0;
}

static void
stream_demo_sleep_ms(uint32_t delay_ms)
{
    struct timespec remaining = {
        .tv_sec = (time_t)(delay_ms / 1000),
        .tv_nsec = (long)(delay_ms % 1000) * 1000000L,
    };
    while (delay_ms != 0 && nanosleep(&remaining, &remaining) != 0
        && errno == EINTR)
    {
    }
}

static void
stream_demo_print_snapshot(netins_udp_stream_t *stream)
{
    netins_udp_stream_snapshot_t snapshot =
        NETINS_UDP_STREAM_SNAPSHOT_INITIALIZER;
    if (netins_udp_stream_get_snapshot(stream, &snapshot) != 0) {
        perror("get stream snapshot");
        return;
    }
    printf("[stream_api_snapshot] role=server state=%d lanes=%u"
           " accepted=%" PRIu64 " reorder=%" PRIu64
           " reorder_high_water=%" PRIu64
           " records_received=%" PRIu64
           " records_delivered=%" PRIu64
           " preaccept_lanes=%u"
           " last_error=%d app_error=%" PRIu64 "\n",
        snapshot.state, snapshot.lane_count, snapshot.accepted_bytes,
        snapshot.reorder_bytes, snapshot.reorder_high_water,
        snapshot.records_received, snapshot.records_delivered,
        snapshot.preaccept_lane_count,
        snapshot.last_error, snapshot.app_error_code);
    uint32_t i;
    for (i = 0; i < snapshot.lane_count; i++) {
        printf("[stream_api_lane] role=server lane=%u sent=%" PRIu64
               " received=%" PRIu64 "\n",
            i, snapshot.lane_payload_sent[i],
            snapshot.lane_payload_received[i]);
    }
}

static void
stream_demo_on_data(netins_udp_stream_t *stream, const void *data,
    size_t data_len, uint64_t offset, void *user_data)
{
    (void)stream;
    stream_demo_receiver_t *receiver = user_data;
    const unsigned char *bytes = data;
    size_t i;
    if (offset != receiver->received && !receiver->mismatch) {
        receiver->mismatch = 1;
        receiver->first_bad_offset = receiver->received;
    }
    for (i = 0; i < data_len && !receiver->mismatch; i++) {
        unsigned char expected =
            (unsigned char)(receiver->seed + (offset + i) * 131U);
        if (bytes[i] != expected) {
            receiver->mismatch = 1;
            receiver->first_bad_offset = offset + i;
        }
    }
    receiver->received = offset + data_len;
    if (!receiver->progress_reported && data_len != 0) {
        receiver->progress_reported = 1;
        printf("[stream_api_progress] role=server client=%zu"
               " received=%" PRIu64 "\n",
            receiver->client_index, receiver->received);
        fflush(stdout);
    }
    stream_demo_sleep_ms(receiver->callback_delay_ms);
}

int
main(int argc, char **argv)
{
    stream_demo_server_options_t options;
    if (stream_demo_parse_args(argc, argv, &options) != 0) {
        stream_demo_server_usage(argv[0]);
        return 2;
    }
    int result = 1;
    netins_udp_t *server = NULL;
    netins_udp_t **connections = calloc(options.client_count,
        sizeof(*connections));
    netins_udp_stream_t **streams = calloc(options.client_count,
        sizeof(*streams));
    stream_demo_receiver_t *receivers =
        calloc(options.client_count, sizeof(*receivers));

    if (connections == NULL || streams == NULL || receivers == NULL
        || netins_udp_init() != 0)
    {
        perror("initialize stream server");
        free(receivers);
        free(streams);
        free(connections);
        return 1;
    }
    server = netins_udp_new(NETINS_UDP_ROLE_SERVER);
    int enabled = 1;
    if (server == NULL
        || netins_udp_set_opt(server, NETINS_UDP_OPT_STREAM_MODE,
            &enabled, sizeof(enabled)) != 0
        || netins_udp_listen(server, options.listen_ip, options.port) != 0)
    {
        perror("listen stream server");
        goto cleanup;
    }
    printf("[stream_api_ready] listen=%s port=%u\n",
        options.listen_ip, (unsigned)options.port);
    printf("[stream_api_config] role=server lanes=%u receive_queue=%zu"
           " callback_delay_ms=%u accept_delay_ms=%u\n",
        NETINS_UDP_STREAM_DEMO_LANE_COUNT, options.receive_queue_size,
        options.callback_delay_ms, options.accept_delay_ms);
    fflush(stdout);
    for (size_t i = 0; i < options.client_count; i++) {
        connections[i] = netins_udp_accept(server);
        if (connections[i] == NULL) {
            perror("accept stream connection");
            goto cleanup;
        }
        if (options.accept_delay_ms != 0) {
            printf("[stream_api_accept_delay] client=%zu delay_ms=%u\n",
                i, options.accept_delay_ms);
            fflush(stdout);
            stream_demo_sleep_ms(options.accept_delay_ms);
        }
        receivers[i].seed = options.seed;
        receivers[i].callback_delay_ms = options.callback_delay_ms;
        receivers[i].client_index = i;
        receivers[i].first_bad_offset = UINT64_MAX;
        netins_udp_stream_config_t config =
            NETINS_UDP_STREAM_CONFIG_INITIALIZER;
        config.receive_queue_size = options.receive_queue_size;
        config.on_data = stream_demo_on_data;
        config.user_data = &receivers[i];
        if (netins_udp_stream_accept(connections[i], &config,
                &streams[i]) != 0)
        {
            perror("accept data stream");
            goto cleanup;
        }
    }

    int all_success = 1;
    for (size_t i = 0; i < options.client_count; i++) {
        netins_udp_stream_result_t stream_result =
            NETINS_UDP_STREAM_RESULT_INITIALIZER;
        if (netins_udp_stream_finish(streams[i], options.finish_timeout_ms,
                &stream_result) != 0)
        {
            perror("finish received stream");
            stream_demo_print_snapshot(streams[i]);
            all_success = 0;
            continue;
        }
        stream_demo_print_snapshot(streams[i]);
        int success = !receivers[i].mismatch
            && (options.expected_bytes == 0
                || receivers[i].received == options.expected_bytes)
            && stream_result.peer_received_bytes == receivers[i].received;
        printf("[stream_api_result] role=server client=%zu success=%d"
               " lanes=%u received=%" PRIu64 " peer_received=%" PRIu64
               " first_bad_offset=%" PRIu64 " elapsed_us=%" PRIu64 "\n",
            i, success, NETINS_UDP_STREAM_DEMO_LANE_COUNT,
            receivers[i].received,
            stream_result.peer_received_bytes,
            receivers[i].first_bad_offset, stream_result.elapsed_us);
        if (!success) {
            all_success = 0;
        }
    }
    result = all_success ? 0 : 1;

cleanup:
    for (size_t i = 0; i < options.client_count; i++) {
        netins_udp_stream_close(streams[i]);
        netins_udp_close(connections[i]);
    }
    netins_udp_close(server);
    if (netins_udp_cleanup() != 0 && result == 0) {
        perror("cleanup stream server");
        result = 1;
    }
    free(receivers);
    free(streams);
    free(connections);
    return result;
}
