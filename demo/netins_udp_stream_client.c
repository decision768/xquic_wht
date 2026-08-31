#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <xquic/netins_udp.h>

#include "netins_udp_stream_demo.h"

#define STREAM_DEMO_CHUNK (256U * 1024U)

typedef struct stream_demo_receiver_s {
    uint32_t seed;
    uint32_t callback_delay_ms;
    uint64_t received;
    uint64_t first_bad_offset;
    int progress_reported;
    int mismatch;
} stream_demo_receiver_t;

typedef struct stream_demo_options_s {
    const char *host;
    const char *interfaces[NETINS_UDP_MAX_INTERFACES];
    uint64_t total_bytes;
    size_t write_chunk_size;
    uint32_t seed;
    uint32_t flags;
    uint32_t write_timeout_ms;
    uint32_t finish_timeout_ms;
    uint32_t callback_delay_ms;
    uint32_t interface_count;
    uint16_t port;
    uint32_t direction;
    int scheduler;
} stream_demo_options_t;

static void
stream_demo_usage(const char *program)
{
    fprintf(stderr,
        "usage: %s HOST PORT BYTES SEED"
        " [FLAGS [WRITE_TIMEOUT_MS [FINISH_TIMEOUT_MS]]]"
        " [--get|--post] [--interface NAME]... [--scheduler NAME]"
        " [--write-chunk-size N] [--callback-delay-ms N]\n"
        "  lanes are fixed at %u; --interface may be repeated up to %u times\n"
        "  direction defaults to POST (client sends); GET makes the client receive\n"
        "  write chunk defaults to %u bytes\n"
        "  --write-chunk-size and --callback-delay-ms are test-only\n"
        "  POST scheduler: default|minrtt|backup|balanced|rap|act|bandwidth|"
        "fastprobe|fastprobe5|fastprobe10|fastprobe20|fastonly|adaptive"
        " (default balanced); GET uses adaptive\n",
        program, NETINS_UDP_STREAM_DEMO_LANE_COUNT,
        NETINS_UDP_MAX_INTERFACES, STREAM_DEMO_CHUNK);
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
stream_demo_parse_scheduler(const char *name, int *out)
{
    static const struct {
        const char *name;
        int value;
    } schedulers[] = {
        {"default", NETINS_UDP_SCHEDULER_DEFAULT},
        {"minrtt", NETINS_UDP_SCHEDULER_MINRTT},
        {"backup", NETINS_UDP_SCHEDULER_BACKUP},
        {"balanced", NETINS_UDP_SCHEDULER_BALANCED},
        {"rap", NETINS_UDP_SCHEDULER_RAP},
        {"act", NETINS_UDP_SCHEDULER_ACT},
        {"bandwidth", NETINS_UDP_SCHEDULER_BANDWIDTH},
        {"fastprobe", NETINS_UDP_SCHEDULER_FASTPROBE},
        {"fastprobe5", NETINS_UDP_SCHEDULER_FASTPROBE5},
        {"fastprobe10", NETINS_UDP_SCHEDULER_FASTPROBE10},
        {"fastprobe20", NETINS_UDP_SCHEDULER_FASTPROBE20},
        {"fastonly", NETINS_UDP_SCHEDULER_FASTONLY},
        {"adaptive", NETINS_UDP_SCHEDULER_ADAPTIVE},
    };
    size_t i;

    for (i = 0; i < sizeof(schedulers) / sizeof(schedulers[0]); i++) {
        if (strcmp(name, schedulers[i].name) == 0) {
            *out = schedulers[i].value;
            return 0;
        }
    }
    return -1;
}

static const char *
stream_demo_scheduler_name(int scheduler)
{
    static const char *names[] = {
        "default", "minrtt", "backup", "balanced", "rap", "act",
        "bandwidth", "fastprobe", "adaptive", "fastprobe5",
        "fastprobe10", "fastprobe20", "fastonly"
    };
    return scheduler >= NETINS_UDP_SCHEDULER_DEFAULT
            && (size_t)scheduler < sizeof(names) / sizeof(names[0])
        ? names[scheduler] : "unknown";
}

static int
stream_demo_parse_args(int argc, char **argv, stream_demo_options_t *options)
{
    uint64_t number;
    int positional = 0;
    int i;

    if (argc < 5) {
        return -1;
    }
    memset(options, 0, sizeof(*options));
    options->host = argv[1];
    options->write_chunk_size = STREAM_DEMO_CHUNK;
    options->write_timeout_ms = 60000;
    options->finish_timeout_ms = 60000;
    options->direction = NETINS_UDP_STREAM_DIRECTION_CLIENT_TO_SERVER;
    options->scheduler = NETINS_UDP_SCHEDULER_BALANCED;
    if (stream_demo_parse_u64(argv[2], UINT16_MAX, &number) != 0
        || number == 0)
    {
        return -1;
    }
    options->port = (uint16_t)number;
    if (stream_demo_parse_u64(argv[3], UINT64_MAX,
            &options->total_bytes) != 0
        || stream_demo_parse_u64(argv[4], UINT32_MAX, &number) != 0)
    {
        return -1;
    }
    options->seed = (uint32_t)number;

    for (i = 5; i < argc; i++) {
        if (strcmp(argv[i], "--get") == 0) {
            options->direction =
                NETINS_UDP_STREAM_DIRECTION_SERVER_TO_CLIENT;
            continue;
        }
        if (strcmp(argv[i], "--post") == 0) {
            options->direction =
                NETINS_UDP_STREAM_DIRECTION_CLIENT_TO_SERVER;
            continue;
        }
        if (strcmp(argv[i], "--interface") == 0) {
            if (++i >= argc
                || options->interface_count >= NETINS_UDP_MAX_INTERFACES
                || argv[i][0] == '\0'
                || strlen(argv[i]) >= NETINS_UDP_INTERFACE_NAME_LEN)
            {
                return -1;
            }
            options->interfaces[options->interface_count++] = argv[i];
            continue;
        }
        if (strcmp(argv[i], "--scheduler") == 0) {
            if (++i >= argc
                || stream_demo_parse_scheduler(argv[i],
                    &options->scheduler) != 0)
            {
                return -1;
            }
            continue;
        }
        if (strcmp(argv[i], "--write-chunk-size") == 0) {
            if (++i >= argc
                || stream_demo_parse_u64(argv[i], SIZE_MAX, &number) != 0
                || number == 0)
            {
                return -1;
            }
            options->write_chunk_size = (size_t)number;
            continue;
        }
        if (strcmp(argv[i], "--callback-delay-ms") == 0) {
            if (++i >= argc
                || stream_demo_parse_u64(argv[i], UINT32_MAX, &number) != 0)
            {
                return -1;
            }
            options->callback_delay_ms = (uint32_t)number;
            continue;
        }
        if (strncmp(argv[i], "--", 2) == 0
            || stream_demo_parse_u64(argv[i], UINT64_MAX, &number) != 0)
        {
            return -1;
        }
        switch (positional++) {
        case 0:
            if (number > UINT32_MAX) {
                return -1;
            }
            options->flags = (uint32_t)number;
            break;
        case 1:
            if (number > UINT32_MAX) {
                return -1;
            }
            options->write_timeout_ms = (uint32_t)number;
            break;
        case 2:
            if (number > UINT32_MAX) {
                return -1;
            }
            options->finish_timeout_ms = (uint32_t)number;
            break;
        default:
            return -1;
        }
    }
    if (options->direction
        == NETINS_UDP_STREAM_DIRECTION_SERVER_TO_CLIENT)
    {
        options->scheduler = NETINS_UDP_SCHEDULER_ADAPTIVE;
    }
    return 0;
}

static int
stream_demo_configure_client(netins_udp_t *client,
    const stream_demo_options_t *options)
{
    netins_udp_interfaces_t interfaces = NETINS_UDP_INTERFACES_INITIALIZER;
    int enabled = 1;
    uint32_t i;

    if (netins_udp_set_opt(client, NETINS_UDP_OPT_STREAM_MODE,
            &enabled, sizeof(enabled)) != 0
        || netins_udp_set_opt(client, NETINS_UDP_OPT_SCHEDULER,
            &options->scheduler, sizeof(options->scheduler)) != 0)
    {
        return -1;
    }
    if (options->interface_count == 0) {
        return 0;
    }
    interfaces.interface_count = options->interface_count;
    for (i = 0; i < options->interface_count; i++) {
        int written = snprintf(interfaces.names[i], sizeof(interfaces.names[i]),
            "%s", options->interfaces[i]);
        if (written < 0 || (size_t)written >= sizeof(interfaces.names[i])) {
            errno = ENAMETOOLONG;
            return -1;
        }
    }
    return netins_udp_set_opt(client, NETINS_UDP_OPT_INTERFACES,
        &interfaces, sizeof(interfaces));
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
    printf("[stream_api_snapshot] role=client state=%d lanes=%u"
           " accepted=%" PRIu64 " peer_received=%" PRIu64
           " send_queue=%" PRIu64 " send_high_water=%" PRIu64
           " reorder=%" PRIu64 " reorder_high_water=%" PRIu64
           " records_enqueued=%" PRIu64
           " records_submitted=%" PRIu64
           " records_received=%" PRIu64
           " records_delivered=%" PRIu64
           " last_error=%d app_error=%" PRIu64 "\n",
        snapshot.state, snapshot.lane_count, snapshot.accepted_bytes,
        snapshot.peer_received_bytes, snapshot.send_queue_bytes,
        snapshot.send_queue_high_water, snapshot.reorder_bytes,
        snapshot.reorder_high_water, snapshot.records_enqueued,
        snapshot.records_submitted, snapshot.records_received,
        snapshot.records_delivered, snapshot.last_error,
        snapshot.app_error_code);
    uint32_t i;
    for (i = 0; i < snapshot.lane_count; i++) {
        printf("[stream_api_lane] role=client lane=%u sent=%" PRIu64
               " received=%" PRIu64 "\n",
            i, snapshot.lane_payload_sent[i],
            snapshot.lane_payload_received[i]);
    }
}

static const char *
stream_demo_direction_name(uint32_t direction)
{
    return direction == NETINS_UDP_STREAM_DIRECTION_SERVER_TO_CLIENT
        ? "get" : "post";
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
stream_demo_fill(unsigned char *data, size_t data_len, uint64_t offset,
    uint32_t seed)
{
    size_t i;
    for (i = 0; i < data_len; i++) {
        data[i] = (unsigned char)(seed + (offset + i) * 131U);
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
        printf("[stream_api_progress] role=client direction=get"
               " received=%" PRIu64 "\n", receiver->received);
        fflush(stdout);
    }
    stream_demo_sleep_ms(receiver->callback_delay_ms);
}

int
main(int argc, char **argv)
{
    stream_demo_options_t options;
    if (stream_demo_parse_args(argc, argv, &options) != 0) {
        stream_demo_usage(argv[0]);
        return 2;
    }
    int result = 1;
    netins_udp_t *client = NULL;
    netins_udp_stream_t *stream = NULL;
    unsigned char *buffer = NULL;
    stream_demo_receiver_t receiver = {
        .seed = options.seed,
        .callback_delay_ms = options.callback_delay_ms,
        .first_bad_offset = UINT64_MAX,
    };

    if (netins_udp_init() != 0) {
        perror("initialize stream client");
        return 1;
    }
    client = netins_udp_new(NETINS_UDP_ROLE_CLIENT);
    if (client == NULL
        || stream_demo_configure_client(client, &options) != 0
        || netins_udp_connect(client, options.host, options.port) != 0)
    {
        perror("connect stream client");
        goto cleanup;
    }
    printf("[stream_api_config] role=client direction=%s lanes=%u"
           " scheduler=%s interfaces=%u write_chunk=%zu"
           " callback_delay_ms=%u\n",
        stream_demo_direction_name(options.direction),
        NETINS_UDP_STREAM_DEMO_LANE_COUNT,
        stream_demo_scheduler_name(options.scheduler),
        options.interface_count, options.write_chunk_size,
        options.callback_delay_ms);
    for (uint32_t i = 0; i < options.interface_count; i++) {
        printf("[stream_api_interface] index=%u name=%s\n",
            i, options.interfaces[i]);
    }

    netins_udp_stream_config_t config =
        NETINS_UDP_STREAM_CONFIG_INITIALIZER;
    config.direction = options.direction;
    config.flags = options.flags;
    if (options.direction
        == NETINS_UDP_STREAM_DIRECTION_SERVER_TO_CLIENT)
    {
        config.on_data = stream_demo_on_data;
        config.user_data = &receiver;
    }
    if (netins_udp_stream_open(client, &config, &stream) != 0) {
        perror("open data stream");
        goto cleanup;
    }

    uint64_t offset = 0;
    uint64_t eagain_count = 0;
    uint64_t partial_count = 0;
    if (options.direction
        == NETINS_UDP_STREAM_DIRECTION_CLIENT_TO_SERVER)
    {
        buffer = malloc(options.write_chunk_size);
        if (buffer == NULL) {
            perror("allocate stream buffer");
            goto cleanup;
        }
        while (offset < options.total_bytes) {
            size_t count = options.total_bytes - offset
                    < options.write_chunk_size
                ? (size_t)(options.total_bytes - offset)
                : options.write_chunk_size;
            stream_demo_fill(buffer, count, offset, options.seed);
            size_t consumed = 0;
            while (consumed < count) {
                size_t accepted = 0;
                size_t requested = count - consumed;
                int write_result = netins_udp_stream_write(stream,
                    buffer + consumed, requested, options.write_timeout_ms,
                    &accepted);
                consumed += accepted;
                offset += accepted;
                if (accepted < requested) {
                    partial_count++;
                }
                if (write_result != 0 && errno == EAGAIN
                    && (options.flags & NETINS_UDP_STREAM_FLAG_NONBLOCK) != 0)
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
    }

    netins_udp_stream_result_t stream_result =
        NETINS_UDP_STREAM_RESULT_INITIALIZER;
    if (netins_udp_stream_finish(stream, options.finish_timeout_ms,
            &stream_result) != 0)
    {
        perror("finish data stream");
        stream_demo_print_snapshot(stream);
        goto cleanup;
    }
    stream_demo_print_snapshot(stream);
    int success = stream_result.accepted_bytes == options.total_bytes
        && stream_result.peer_received_bytes == options.total_bytes;
    if (options.direction
        == NETINS_UDP_STREAM_DIRECTION_SERVER_TO_CLIENT)
    {
        success = success && !receiver.mismatch
            && receiver.received == options.total_bytes;
    }
    double throughput_mbps = stream_result.elapsed_us == 0 ? 0.0
        : (double)stream_result.peer_received_bytes * 8.0
            / (double)stream_result.elapsed_us;
    printf("[stream_api_result] role=client direction=%s success=%d lanes=%u"
           " accepted=%" PRIu64 " peer_received=%" PRIu64
           " received=%" PRIu64 " first_bad_offset=%" PRIu64
           " elapsed_us=%" PRIu64 " throughput_mbps=%.3f"
           " eagain=%" PRIu64 " partial=%" PRIu64 "\n",
        stream_demo_direction_name(options.direction), success,
        NETINS_UDP_STREAM_DEMO_LANE_COUNT, stream_result.accepted_bytes,
        stream_result.peer_received_bytes, receiver.received,
        receiver.first_bad_offset, stream_result.elapsed_us,
        throughput_mbps, eagain_count, partial_count);
    result = success ? 0 : 1;

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
