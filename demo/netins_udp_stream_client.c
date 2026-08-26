#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#include <xquic/netins_udp.h>

#define STREAM_DEMO_CHUNK (256U * 1024U)

static void
stream_demo_fill(unsigned char *data, size_t data_len, uint64_t offset,
    uint32_t seed)
{
    size_t i;
    for (i = 0; i < data_len; i++) {
        data[i] = (unsigned char)(seed + (offset + i) * 131U);
    }
}

int
main(int argc, char **argv)
{
    if (argc != 6) {
        fprintf(stderr, "usage: %s HOST PORT BYTES LANES SEED\n", argv[0]);
        return 2;
    }
    const char *host = argv[1];
    uint16_t port = (uint16_t)strtoul(argv[2], NULL, 10);
    uint64_t total_bytes = strtoull(argv[3], NULL, 10);
    uint32_t lane_count = (uint32_t)strtoul(argv[4], NULL, 10);
    uint32_t seed = (uint32_t)strtoul(argv[5], NULL, 10);
    int result = 1;
    netins_udp_t *client = NULL;
    netins_udp_stream_t *stream = NULL;
    unsigned char *buffer = NULL;

    if (port == 0 || lane_count == 0 || netins_udp_init() != 0) {
        perror("initialize stream client");
        return 1;
    }
    client = netins_udp_new(NETINS_UDP_ROLE_CLIENT);
    int enabled = 1;
    if (client == NULL
        || netins_udp_set_opt(client, NETINS_UDP_OPT_STREAM_MODE,
            &enabled, sizeof(enabled)) != 0
        || netins_udp_connect(client, host, port) != 0)
    {
        perror("connect stream client");
        goto cleanup;
    }

    netins_udp_stream_config_t config =
        NETINS_UDP_STREAM_CONFIG_INITIALIZER;
    config.lane_count = lane_count;
    if (netins_udp_stream_open(client, &config, &stream) != 0) {
        perror("open data stream");
        goto cleanup;
    }
    buffer = malloc(STREAM_DEMO_CHUNK);
    if (buffer == NULL) {
        perror("allocate stream buffer");
        goto cleanup;
    }

    uint64_t offset = 0;
    while (offset < total_bytes) {
        size_t count = total_bytes - offset < STREAM_DEMO_CHUNK
            ? (size_t)(total_bytes - offset) : STREAM_DEMO_CHUNK;
        stream_demo_fill(buffer, count, offset, seed);
        size_t consumed = 0;
        while (consumed < count) {
            size_t accepted = 0;
            if (netins_udp_stream_write(stream, buffer + consumed,
                    count - consumed, 60000, &accepted) != 0)
            {
                perror("write data stream");
                goto cleanup;
            }
            consumed += accepted;
            offset += accepted;
        }
    }

    netins_udp_stream_result_t stream_result =
        NETINS_UDP_STREAM_RESULT_INITIALIZER;
    if (netins_udp_stream_finish(stream, 60000, &stream_result) != 0) {
        perror("finish data stream");
        goto cleanup;
    }
    printf("[stream_api_result] role=client success=1 lanes=%u"
           " accepted=%" PRIu64 " peer_received=%" PRIu64
           " elapsed_us=%" PRIu64 "\n",
        lane_count, stream_result.accepted_bytes,
        stream_result.peer_received_bytes, stream_result.elapsed_us);
    result = 0;

cleanup:
    free(buffer);
    netins_udp_stream_close(stream);
    netins_udp_close(client);
    if (netins_udp_cleanup() != 0 && result == 0) {
        perror("cleanup stream client");
        result = 1;
    }
    return result;
}
