#include <xquic/netins_udp.h>

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct client_options_s {
    const char *host;
    const char *local_path;
    const char *remote_path;
    const char *server_name;
    const char *interfaces[NETINS_UDP_MAX_INTERFACES];
    uint32_t interface_count;
    uint32_t timeout_ms;
    uint16_t port;
    int direction;
    int scheduler;
    int congestion_control;
} client_options_t;

static void
usage(const char *program)
{
    fprintf(stderr,
        "Usage: %s --get|--post --local FILE --remote /PATH [options]\n"
        "Options:\n"
        "  --host IPv4             Server address (default 127.0.0.1)\n"
        "  --port PORT             Server port (default 8443)\n"
        "  --interface NAME        Client interface; repeat at most twice\n"
        "  --server-name NAME      TLS SNI name\n"
        "  --scheduler NAME        default|minrtt|backup|balanced|rap|act|\n"
        "                          bandwidth|fastprobe|adaptive\n"
        "  --cc NAME               default|cubic|bbr\n"
        "  --timeout-ms MS         Connect and transfer timeout; 0 is infinite\n",
        program);
}

static int
parse_u32(const char *text, uint32_t maximum, uint32_t *out)
{
    char *end = NULL;
    unsigned long value;

    errno = 0;
    value = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value > maximum) {
        return -1;
    }

    *out = (uint32_t)value;
    return 0;
}

static int
parse_scheduler(const char *name, int *out)
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
        {"adaptive", NETINS_UDP_SCHEDULER_ADAPTIVE},
    };
    size_t i;

    for (i = 0; i < sizeof(schedulers) / sizeof(schedulers[0]); ++i) {
        if (strcmp(name, schedulers[i].name) == 0) {
            *out = schedulers[i].value;
            return 0;
        }
    }
    return -1;
}

static int
parse_cc(const char *name, int *out)
{
    if (strcmp(name, "default") == 0) {
        *out = NETINS_UDP_CC_DEFAULT;
    } else if (strcmp(name, "cubic") == 0) {
        *out = NETINS_UDP_CC_CUBIC;
    } else if (strcmp(name, "bbr") == 0) {
        *out = NETINS_UDP_CC_BBR;
    } else {
        return -1;
    }
    return 0;
}

static int
need_value(int argc, char **argv, int *index, const char **out)
{
    if (*index + 1 >= argc) {
        fprintf(stderr, "%s requires a value\n", argv[*index]);
        return -1;
    }
    *out = argv[++(*index)];
    return 0;
}

static int
parse_args(int argc, char **argv, client_options_t *options)
{
    int i;

    memset(options, 0, sizeof(*options));
    options->host = "127.0.0.1";
    options->port = 8443;
    options->direction = 0;
    options->scheduler = NETINS_UDP_SCHEDULER_DEFAULT;
    options->congestion_control = NETINS_UDP_CC_DEFAULT;

    for (i = 1; i < argc; ++i) {
        const char *value = NULL;
        uint32_t number;

        if (strcmp(argv[i], "--get") == 0) {
            options->direction = NETINS_UDP_TRANSFER_GET;
        } else if (strcmp(argv[i], "--post") == 0) {
            options->direction = NETINS_UDP_TRANSFER_POST;
        } else if (strcmp(argv[i], "--host") == 0) {
            if (need_value(argc, argv, &i, &options->host) != 0) {
                return -1;
            }
        } else if (strcmp(argv[i], "--port") == 0) {
            if (need_value(argc, argv, &i, &value) != 0
                || parse_u32(value, 65535, &number) != 0 || number == 0)
            {
                return -1;
            }
            options->port = (uint16_t)number;
        } else if (strcmp(argv[i], "--local") == 0) {
            if (need_value(argc, argv, &i, &options->local_path) != 0) {
                return -1;
            }
        } else if (strcmp(argv[i], "--remote") == 0) {
            if (need_value(argc, argv, &i, &options->remote_path) != 0) {
                return -1;
            }
        } else if (strcmp(argv[i], "--server-name") == 0) {
            if (need_value(argc, argv, &i, &options->server_name) != 0) {
                return -1;
            }
        } else if (strcmp(argv[i], "--interface") == 0) {
            if (options->interface_count >= NETINS_UDP_MAX_INTERFACES
                || need_value(argc, argv, &i,
                    &options->interfaces[options->interface_count]) != 0)
            {
                return -1;
            }
            ++options->interface_count;
        } else if (strcmp(argv[i], "--timeout-ms") == 0) {
            if (need_value(argc, argv, &i, &value) != 0
                || parse_u32(value, UINT32_MAX, &options->timeout_ms) != 0)
            {
                return -1;
            }
        } else if (strcmp(argv[i], "--scheduler") == 0) {
            if (need_value(argc, argv, &i, &value) != 0
                || parse_scheduler(value, &options->scheduler) != 0)
            {
                return -1;
            }
        } else if (strcmp(argv[i], "--cc") == 0) {
            if (need_value(argc, argv, &i, &value) != 0
                || parse_cc(value, &options->congestion_control) != 0)
            {
                return -1;
            }
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            return -1;
        }
    }

    if (options->direction == 0 || options->local_path == NULL
        || options->remote_path == NULL)
    {
        return -1;
    }
    return 0;
}

static void
api_logger(void *user_data, int level, const char *message)
{
    FILE *stream = user_data != NULL ? (FILE *)user_data : stderr;
    fprintf(stream, "[mini-api:%d] %s", level, message);
}

static void
print_handle_error(netins_udp_t *handle, const char *operation)
{
    char message[NETINS_UDP_ERROR_MSG_LEN] = {0};
    int code = errno;

    if (handle != NULL) {
        code = netins_udp_get_last_error_code(handle);
        if (netins_udp_get_last_error_msg(handle, message, sizeof(message)) != 0) {
            message[0] = '\0';
        }
    }
    fprintf(stderr, "%s failed: code=%d message=%s\n",
        operation, code, message[0] != '\0' ? message : "unavailable");
}

static int
copy_option_string(char *destination, size_t capacity, const char *source)
{
    int written;

    if (source == NULL) {
        return 0;
    }
    written = snprintf(destination, capacity, "%s", source);
    return written >= 0 && (size_t)written < capacity ? 0 : -1;
}

static int
configure_client(netins_udp_t *client, const client_options_t *options)
{
    netins_udp_logger_config_t logger = NETINS_UDP_LOGGER_CONFIG_INITIALIZER;
    netins_udp_interfaces_t interfaces = NETINS_UDP_INTERFACES_INITIALIZER;
    netins_udp_tls_config_t tls = NETINS_UDP_TLS_CONFIG_INITIALIZER;
    uint32_t i;

    logger.callback = api_logger;
    logger.user_data = stderr;
    logger.minimum_level = NETINS_UDP_LOG_INFO;
    if (netins_udp_set_opt(client, NETINS_UDP_OPT_LOGGER,
            &logger, sizeof(logger)) != 0
        || netins_udp_set_opt(client, NETINS_UDP_OPT_SCHEDULER,
            &options->scheduler, sizeof(options->scheduler)) != 0
        || netins_udp_set_opt(client, NETINS_UDP_OPT_CONGESTION_CONTROL,
            &options->congestion_control,
            sizeof(options->congestion_control)) != 0
        || netins_udp_set_opt(client, NETINS_UDP_OPT_CONNECT_TIMEOUT_MS,
            &options->timeout_ms, sizeof(options->timeout_ms)) != 0)
    {
        return -1;
    }

    if (options->interface_count > 0) {
        interfaces.interface_count = options->interface_count;
        for (i = 0; i < options->interface_count; ++i) {
            if (copy_option_string(interfaces.names[i],
                    sizeof(interfaces.names[i]), options->interfaces[i]) != 0)
            {
                errno = ENAMETOOLONG;
                return -1;
            }
        }
        if (netins_udp_set_opt(client, NETINS_UDP_OPT_INTERFACES,
                &interfaces, sizeof(interfaces)) != 0)
        {
            return -1;
        }
    }

    if (options->server_name != NULL) {
        if (copy_option_string(tls.server_name, sizeof(tls.server_name),
                options->server_name) != 0)
        {
            errno = ENAMETOOLONG;
            return -1;
        }
        if (netins_udp_set_opt(client, NETINS_UDP_OPT_TLS,
                &tls, sizeof(tls)) != 0)
        {
            return -1;
        }
    }

    return 0;
}

static void
print_snapshot(netins_udp_t *client)
{
    netins_udp_snapshot_t snapshot = NETINS_UDP_SNAPSHOT_INITIALIZER;
    uint32_t i;

    if (netins_udp_get_snapshot(client, &snapshot) != 0) {
        print_handle_error(client, "snapshot");
        return;
    }

    printf("connection multipath=%d paths=%" PRIu32
           " active=%" PRIu32 " srtt_us=%" PRIu64
           " inflight=%" PRIu64 "\n",
        netins_udp_is_multipath(client), snapshot.path_count,
        snapshot.active_path_count, snapshot.srtt_us,
        snapshot.bytes_inflight);

    for (i = 0; i < snapshot.path_count; ++i) {
        const netins_udp_path_statis_t *path = &snapshot.paths[i];
        printf("path[%" PRIu32 "] id=%" PRIu64 " state=%d app=%d"
               " iface=%s local=%s peer=%s tx=%" PRIu64
               " rx=%" PRIu64 " srtt_us=%" PRIu64 "\n",
            i, path->path_id, path->path_state, path->app_status,
            path->interface_name, path->local_address, path->peer_address,
            path->bytes_sent, path->bytes_received, path->srtt_us);
    }
}

int
main(int argc, char **argv)
{
    client_options_t options;
    netins_udp_t *client = NULL;
    netins_udp_transfer_t task = NETINS_UDP_TRANSFER_INITIALIZER;
    netins_udp_transfer_result_t result =
        NETINS_UDP_TRANSFER_RESULT_INITIALIZER;
    int initialized = 0;
    int exit_code = EXIT_FAILURE;

    if (parse_args(argc, argv, &options) != 0) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    if (netins_udp_init() != 0) {
        perror("netins_udp_init");
        goto done;
    }
    initialized = 1;

    client = netins_udp_new(NETINS_UDP_ROLE_CLIENT);
    if (client == NULL) {
        perror("netins_udp_new");
        goto done;
    }
    if (configure_client(client, &options) != 0) {
        print_handle_error(client, "configure");
        goto done;
    }
    if (netins_udp_connect(client, options.host, options.port) != 0) {
        print_handle_error(client, "connect");
        goto done;
    }

    task.direction = options.direction;
    task.timeout_ms = options.timeout_ms;
    task.local_path = options.local_path;
    task.remote_path = options.remote_path;
    if (netins_udp_transfer(client, &task, &result) != 0) {
        print_handle_error(client, "transfer");
        goto done;
    }

    printf("transfer direction=%s bytes=%" PRIu64
           " fragments=%" PRIu32 "/%" PRIu32
           " elapsed_us=%" PRIu64 " throughput_mbps=%.3f\n",
        options.direction == NETINS_UDP_TRANSFER_GET ? "GET" : "POST",
        result.total_bytes, result.completed_fragments,
        result.total_fragments, result.elapsed_us, result.throughput_mbps);
    print_snapshot(client);
    exit_code = EXIT_SUCCESS;

done:
    if (client != NULL) {
        (void)netins_udp_shutdown(client);
        netins_udp_close(client);
    }
    if (initialized) {
        if (netins_udp_cleanup() != 0) {
            perror("netins_udp_cleanup");
            exit_code = EXIT_FAILURE;
        }
    }
    return exit_code;
}
