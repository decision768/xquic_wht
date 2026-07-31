#include <xquic/netins_udp.h>

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct server_options_s {
    const char *bind_address;
    const char *certificate_file;
    const char *private_key_file;
    const char *root_path;
    uint32_t accept_timeout_ms;
    uint32_t serve_timeout_ms;
    uint32_t max_connections;
    uint16_t port;
} server_options_t;

static void
usage(const char *program)
{
    fprintf(stderr,
        "Usage: %s --cert FILE --key FILE --root DIR [options]\n"
        "Options:\n"
        "  --bind IPv4             Listen address (default 0.0.0.0)\n"
        "  --port PORT             Listen port (default 8443)\n"
        "  --accept-timeout-ms MS  Per-accept timeout; 0 is infinite\n"
        "  --serve-timeout-ms MS   Per-connection timeout; 0 is infinite\n"
        "  --max-connections N     Connections to serve (default 1)\n",
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
parse_args(int argc, char **argv, server_options_t *options)
{
    int i;

    memset(options, 0, sizeof(*options));
    options->bind_address = "0.0.0.0";
    options->port = 8443;
    options->max_connections = 1;

    for (i = 1; i < argc; ++i) {
        const char *value = NULL;
        uint32_t number;

        if (strcmp(argv[i], "--bind") == 0) {
            if (need_value(argc, argv, &i, &options->bind_address) != 0) {
                return -1;
            }
        } else if (strcmp(argv[i], "--port") == 0) {
            if (need_value(argc, argv, &i, &value) != 0
                || parse_u32(value, 65535, &number) != 0 || number == 0)
            {
                return -1;
            }
            options->port = (uint16_t)number;
        } else if (strcmp(argv[i], "--cert") == 0) {
            if (need_value(argc, argv, &i, &options->certificate_file) != 0) {
                return -1;
            }
        } else if (strcmp(argv[i], "--key") == 0) {
            if (need_value(argc, argv, &i, &options->private_key_file) != 0) {
                return -1;
            }
        } else if (strcmp(argv[i], "--root") == 0) {
            if (need_value(argc, argv, &i, &options->root_path) != 0) {
                return -1;
            }
        } else if (strcmp(argv[i], "--accept-timeout-ms") == 0) {
            if (need_value(argc, argv, &i, &value) != 0
                || parse_u32(value, UINT32_MAX,
                    &options->accept_timeout_ms) != 0)
            {
                return -1;
            }
        } else if (strcmp(argv[i], "--serve-timeout-ms") == 0) {
            if (need_value(argc, argv, &i, &value) != 0
                || parse_u32(value, UINT32_MAX,
                    &options->serve_timeout_ms) != 0)
            {
                return -1;
            }
        } else if (strcmp(argv[i], "--max-connections") == 0) {
            if (need_value(argc, argv, &i, &value) != 0
                || parse_u32(value, UINT32_MAX,
                    &options->max_connections) != 0
                || options->max_connections == 0)
            {
                return -1;
            }
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            return -1;
        }
    }

    return options->certificate_file != NULL
        && options->private_key_file != NULL
        && options->root_path != NULL ? 0 : -1;
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
    int written = snprintf(destination, capacity, "%s", source);
    return written >= 0 && (size_t)written < capacity ? 0 : -1;
}

static int
configure_server(netins_udp_t *server, const server_options_t *options)
{
    netins_udp_logger_config_t logger = NETINS_UDP_LOGGER_CONFIG_INITIALIZER;
    netins_udp_tls_config_t tls = NETINS_UDP_TLS_CONFIG_INITIALIZER;

    logger.callback = api_logger;
    logger.user_data = stderr;
    logger.minimum_level = NETINS_UDP_LOG_DEBUG;
    if (copy_option_string(tls.certificate_file,
            sizeof(tls.certificate_file), options->certificate_file) != 0
        || copy_option_string(tls.private_key_file,
            sizeof(tls.private_key_file), options->private_key_file) != 0)
    {
        errno = ENAMETOOLONG;
        return -1;
    }

    if (netins_udp_set_opt(server, NETINS_UDP_OPT_LOGGER,
            &logger, sizeof(logger)) != 0
        || netins_udp_set_opt(server, NETINS_UDP_OPT_TLS,
            &tls, sizeof(tls)) != 0
        || netins_udp_set_opt(server, NETINS_UDP_OPT_ACCEPT_TIMEOUT_MS,
            &options->accept_timeout_ms,
            sizeof(options->accept_timeout_ms)) != 0)
    {
        return -1;
    }
    return 0;
}

int
main(int argc, char **argv)
{
    server_options_t options;
    netins_udp_t *server = NULL;
    uint32_t served = 0;
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

    server = netins_udp_new(NETINS_UDP_ROLE_SERVER);
    if (server == NULL) {
        perror("netins_udp_new");
        goto done;
    }
    if (configure_server(server, &options) != 0) {
        print_handle_error(server, "configure");
        goto done;
    }
    if (netins_udp_listen(server, options.bind_address, options.port) != 0) {
        print_handle_error(server, "listen");
        goto done;
    }

    printf("listening address=%s port=%" PRIu16 " root=%s\n",
        options.bind_address, options.port, options.root_path);
    while (served < options.max_connections) {
        netins_udp_t *connection = netins_udp_accept(server);
        netins_udp_serve_config_t serve = NETINS_UDP_SERVE_CONFIG_INITIALIZER;

        if (connection == NULL) {
            print_handle_error(server, "accept");
            goto done;
        }

        serve.root_path = options.root_path;
        serve.timeout_ms = options.serve_timeout_ms;
        if (netins_udp_serve(connection, &serve) != 0) {
            print_handle_error(connection, "serve");
            netins_udp_close(connection);
            goto done;
        }

        (void)netins_udp_shutdown(connection);
        netins_udp_close(connection);
        ++served;
        printf("served connection=%" PRIu32 "/%" PRIu32 "\n",
            served, options.max_connections);
    }
    exit_code = EXIT_SUCCESS;

done:
    if (server != NULL) {
        (void)netins_udp_shutdown(server);
        netins_udp_close(server);
    }
    if (initialized) {
        if (netins_udp_cleanup() != 0) {
            perror("netins_udp_cleanup");
            exit_code = EXIT_FAILURE;
        }
    }
    return exit_code;
}
