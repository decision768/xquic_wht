#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#include <xquic/netins_udp.h>

typedef struct stream_demo_receiver_s {
    uint32_t seed;
    uint64_t received;
    uint64_t first_bad_offset;
    int mismatch;
} stream_demo_receiver_t;

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
    if (argc != 6) {
        fprintf(stderr,
            "usage: %s LISTEN_IP PORT EXPECTED_BYTES LANES SEED\n", argv[0]);
        return 2;
    }
    const char *listen_ip = argv[1];
    uint16_t port = (uint16_t)strtoul(argv[2], NULL, 10);
    uint64_t expected_bytes = strtoull(argv[3], NULL, 10);
    uint32_t lane_count = (uint32_t)strtoul(argv[4], NULL, 10);
    stream_demo_receiver_t receiver = {
        .seed = (uint32_t)strtoul(argv[5], NULL, 10),
        .first_bad_offset = UINT64_MAX,
    };
    int result = 1;
    netins_udp_t *server = NULL;
    netins_udp_t *connection = NULL;
    netins_udp_stream_t *stream = NULL;

    if (port == 0 || lane_count == 0 || netins_udp_init() != 0) {
        perror("initialize stream server");
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
    connection = netins_udp_accept(server);
    if (connection == NULL) {
        perror("accept stream connection");
        goto cleanup;
    }

    netins_udp_stream_config_t config =
        NETINS_UDP_STREAM_CONFIG_INITIALIZER;
    config.lane_count = lane_count;
    config.on_data = stream_demo_on_data;
    config.user_data = &receiver;
    if (netins_udp_stream_accept(connection, &config, &stream) != 0) {
        perror("accept data stream");
        goto cleanup;
    }
    netins_udp_stream_result_t stream_result =
        NETINS_UDP_STREAM_RESULT_INITIALIZER;
    if (netins_udp_stream_finish(stream, 60000, &stream_result) != 0) {
        perror("finish received stream");
        goto cleanup;
    }
    int success = !receiver.mismatch
        && (expected_bytes == 0 || receiver.received == expected_bytes)
        && stream_result.peer_received_bytes == receiver.received;
    printf("[stream_api_result] role=server success=%d lanes=%u"
           " received=%" PRIu64 " peer_received=%" PRIu64
           " first_bad_offset=%" PRIu64 " elapsed_us=%" PRIu64 "\n",
        success, lane_count, receiver.received,
        stream_result.peer_received_bytes, receiver.first_bad_offset,
        stream_result.elapsed_us);
    result = success ? 0 : 1;

cleanup:
    netins_udp_stream_close(stream);
    netins_udp_close(connection);
    netins_udp_close(server);
    if (netins_udp_cleanup() != 0 && result == 0) {
        perror("cleanup stream server");
        result = 1;
    }
    return result;
}
