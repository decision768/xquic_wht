#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include <xquic/netins_udp.h>

#define STREAM_DEMO_CHUNK (256U * 1024U)

static void
stream_demo_print_snapshot(netins_udp_stream_t *stream)
{
    netins_udp_stream_snapshot_t snapshot =
        NETINS_UDP_STREAM_SNAPSHOT_INITIALIZER;
    if (netins_udp_stream_get_snapshot(stream, &snapshot) != 0) {
        perror("get stream snapshot");
        return;
    }
    printf("[stream_api_snapshot] role=client state=%d lanes=%u"
           " accepted=%" PRIu64 " peer_received=%" PRIu64
           " send_queue=%" PRIu64 " send_high_water=%" PRIu64
           " records_enqueued=%" PRIu64
           " records_submitted=%" PRIu64
           " last_error=%d app_error=%" PRIu64 "\n",
        snapshot.state, snapshot.lane_count, snapshot.accepted_bytes,
        snapshot.peer_received_bytes, snapshot.send_queue_bytes,
        snapshot.send_queue_high_water, snapshot.records_enqueued,
        snapshot.records_submitted, snapshot.last_error,
        snapshot.app_error_code);
    uint32_t i;
    for (i = 0; i < snapshot.lane_count; i++) {
        printf("[stream_api_lane] role=client lane=%u sent=%" PRIu64
               " received=%" PRIu64 "\n",
            i, snapshot.lane_payload_sent[i],
            snapshot.lane_payload_received[i]);
    }
}

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
    if (argc < 6 || argc > 11) {
        fprintf(stderr,
            "usage: %s HOST PORT BYTES LANES SEED"
            " [SEND_QUEUE_BYTES [RECEIVE_QUEUE_BYTES [FLAGS"
            " [WRITE_TIMEOUT_MS [FINISH_TIMEOUT_MS]]]]]\n", argv[0]);
        return 2;
    }
    const char *host = argv[1];
    uint16_t port = (uint16_t)strtoul(argv[2], NULL, 10);
    uint64_t total_bytes = strtoull(argv[3], NULL, 10);
    uint32_t lane_count = (uint32_t)strtoul(argv[4], NULL, 10);
    uint32_t seed = (uint32_t)strtoul(argv[5], NULL, 10);
    size_t send_queue_size = argc > 6
        ? (size_t)strtoull(argv[6], NULL, 10)
        : NETINS_UDP_STREAM_DEFAULT_QUEUE_SIZE;
    size_t receive_queue_size = argc > 7
        ? (size_t)strtoull(argv[7], NULL, 10)
        : NETINS_UDP_STREAM_DEFAULT_QUEUE_SIZE;
    uint32_t flags = argc > 8
        ? (uint32_t)strtoul(argv[8], NULL, 10) : 0;
    uint32_t write_timeout_ms = argc > 9
        ? (uint32_t)strtoul(argv[9], NULL, 10) : 60000;
    uint32_t finish_timeout_ms = argc > 10
        ? (uint32_t)strtoul(argv[10], NULL, 10) : 60000;
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
    config.flags = flags;
    config.send_queue_size = send_queue_size;
    config.receive_queue_size = receive_queue_size;
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
    uint64_t eagain_count = 0;
    uint64_t partial_count = 0;
    while (offset < total_bytes) {
        size_t count = total_bytes - offset < STREAM_DEMO_CHUNK
            ? (size_t)(total_bytes - offset) : STREAM_DEMO_CHUNK;
        stream_demo_fill(buffer, count, offset, seed);
        size_t consumed = 0;
        while (consumed < count) {
            size_t accepted = 0;
            size_t requested = count - consumed;
            int write_result = netins_udp_stream_write(stream,
                buffer + consumed, requested, write_timeout_ms, &accepted);
            consumed += accepted;
            offset += accepted;
            if (accepted < requested) {
                partial_count++;
            }
            if (write_result != 0 && errno == EAGAIN
                && (flags & NETINS_UDP_STREAM_FLAG_NONBLOCK) != 0)
            {
                struct timespec pause_time = { 0, 1000000L };
                eagain_count++;
                (void)nanosleep(&pause_time, NULL);
                continue;
            }
            if (write_result != 0) {
                perror("write data stream");
                stream_demo_print_snapshot(stream);
                goto cleanup;
            }
            if (accepted == 0) {
                errno = EIO;
                perror("write made no progress");
                stream_demo_print_snapshot(stream);
                goto cleanup;
            }
        }
    }

    netins_udp_stream_result_t stream_result =
        NETINS_UDP_STREAM_RESULT_INITIALIZER;
    if (netins_udp_stream_finish(stream, finish_timeout_ms,
            &stream_result) != 0)
    {
        perror("finish data stream");
        stream_demo_print_snapshot(stream);
        goto cleanup;
    }
    stream_demo_print_snapshot(stream);
    double throughput_mbps = stream_result.elapsed_us == 0 ? 0.0
        : (double)stream_result.peer_received_bytes * 8.0
            / (double)stream_result.elapsed_us;
    printf("[stream_api_result] role=client success=1 lanes=%u"
           " accepted=%" PRIu64 " peer_received=%" PRIu64
           " elapsed_us=%" PRIu64 " throughput_mbps=%.3f"
           " eagain=%" PRIu64 " partial=%" PRIu64 "\n",
        lane_count, stream_result.accepted_bytes,
        stream_result.peer_received_bytes, stream_result.elapsed_us,
        throughput_mbps, eagain_count, partial_count);
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
