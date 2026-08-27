#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#include <xquic/netins_udp.h>

#include "netins_udp_stream_demo.h"

typedef struct stream_demo_receiver_s {
    uint32_t seed;
    uint64_t received;
    uint64_t first_bad_offset;
    int mismatch;
} stream_demo_receiver_t;

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
           " last_error=%d app_error=%" PRIu64 "\n",
        snapshot.state, snapshot.lane_count, snapshot.accepted_bytes,
        snapshot.reorder_bytes, snapshot.reorder_high_water,
        snapshot.records_received, snapshot.records_delivered,
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
}

int
main(int argc, char **argv)
{
    if (argc < 5 || argc > 8) {
        fprintf(stderr,
            "usage: %s LISTEN_IP PORT EXPECTED_BYTES SEED"
            " [RECEIVE_QUEUE_BYTES [FINISH_TIMEOUT_MS [CLIENTS]]]\n",
            argv[0]);
        fprintf(stderr, "  lanes are fixed at %u\n",
            NETINS_UDP_STREAM_DEMO_LANE_COUNT);
        return 2;
    }
    const char *listen_ip = argv[1];
    uint16_t port = (uint16_t)strtoul(argv[2], NULL, 10);
    uint64_t expected_bytes = strtoull(argv[3], NULL, 10);
    uint32_t seed = (uint32_t)strtoul(argv[4], NULL, 10);
    size_t receive_queue_size = argc > 5
        ? (size_t)strtoull(argv[5], NULL, 10)
        : NETINS_UDP_STREAM_DEFAULT_QUEUE_SIZE;
    uint32_t finish_timeout_ms = argc > 6
        ? (uint32_t)strtoul(argv[6], NULL, 10) : 60000;
    size_t client_count = argc > 7
        ? (size_t)strtoull(argv[7], NULL, 10) : 1;
    int result = 1;
    netins_udp_t *server = NULL;
    netins_udp_t **connections = calloc(client_count, sizeof(*connections));
    netins_udp_stream_t **streams = calloc(client_count, sizeof(*streams));
    stream_demo_receiver_t *receivers =
        calloc(client_count, sizeof(*receivers));

    if (port == 0 || client_count == 0
        || connections == NULL || streams == NULL || receivers == NULL
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
        || netins_udp_listen(server, listen_ip, port) != 0)
    {
        perror("listen stream server");
        goto cleanup;
    }
    printf("[stream_api_ready] listen=%s port=%u\n",
        listen_ip, (unsigned)port);
    printf("[stream_api_config] role=server lanes=%u\n",
        NETINS_UDP_STREAM_DEMO_LANE_COUNT);
    fflush(stdout);
    for (size_t i = 0; i < client_count; i++) {
        connections[i] = netins_udp_accept(server);
        if (connections[i] == NULL) {
            perror("accept stream connection");
            goto cleanup;
        }
        receivers[i].seed = seed;
        receivers[i].first_bad_offset = UINT64_MAX;
        netins_udp_stream_config_t config =
            NETINS_UDP_STREAM_CONFIG_INITIALIZER;
        config.receive_queue_size = receive_queue_size;
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
    for (size_t i = 0; i < client_count; i++) {
        netins_udp_stream_result_t stream_result =
            NETINS_UDP_STREAM_RESULT_INITIALIZER;
        if (netins_udp_stream_finish(streams[i], finish_timeout_ms,
                &stream_result) != 0)
        {
            perror("finish received stream");
            stream_demo_print_snapshot(streams[i]);
            all_success = 0;
            continue;
        }
        stream_demo_print_snapshot(streams[i]);
        int success = !receivers[i].mismatch
            && (expected_bytes == 0
                || receivers[i].received == expected_bytes)
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
    for (size_t i = 0; i < client_count; i++) {
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
