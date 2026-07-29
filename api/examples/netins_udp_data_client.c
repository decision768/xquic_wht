#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <xquic/netins_udp.h>

#define NETINS_UDP_DEMO_BUFFER_SIZE (4U * 1024U * 1024U)
#define NETINS_UDP_DEMO_TIMEOUT_MS  120000U

static int
parse_port(const char *text)
{
    char *end = NULL;
    errno = 0;
    long value = strtol(text, &end, 10);
    return errno == 0 && end != text && *end == '\0'
        && value > 0 && value <= 65535 ? (int)value : -1;
}

static void
print_connection_error(const char *operation, netins_udp_t *connection)
{
    char message[NETINS_UDP_ERROR_MSG_LEN] = {0};
    int error_code = errno;
    if (connection != NULL) {
        int api_error = netins_udp_get_last_error_code(connection);
        if (api_error != 0) {
            error_code = api_error;
        }
        (void)netins_udp_get_last_error_msg(connection, message,
            sizeof(message));
    }
    fprintf(stderr, "%s failed: error=%d message=%s\n", operation,
        error_code, message);
}

static int
send_all(netins_udp_t *connection, const void *data, size_t length)
{
    const unsigned char *cursor = data;
    size_t offset = 0;
    while (offset < length) {
        ssize_t sent = netins_udp_send(connection, cursor + offset,
            length - offset, NETINS_UDP_MSG_NONE);
        if (sent < 0) {
            return -1;
        }
        if (sent == 0) {
            errno = EIO;
            return -1;
        }
        offset += (size_t)sent;
    }
    return 0;
}

static int
send_file(netins_udp_t *connection, const char *path,
    unsigned char *buffer, size_t buffer_size, uint64_t *out_bytes)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return -1;
    }
    uint64_t total = 0;
    int result = 0;
    for (;;) {
        size_t amount = fread(buffer, 1, buffer_size, file);
        if (amount > 0) {
            if (send_all(connection, buffer, amount) != 0) {
                result = -1;
                break;
            }
            total += amount;
        }
        if (amount < buffer_size) {
            if (ferror(file)) {
                result = -1;
            }
            break;
        }
    }
    if (fclose(file) != 0 && result == 0) {
        result = -1;
    }
    if (result == 0
        && netins_udp_send(connection, NULL, 0, NETINS_UDP_MSG_FIN) < 0)
    {
        result = -1;
    }
    if (result == 0) {
        *out_bytes = total;
    }
    return result;
}

static int
receive_file(netins_udp_t *connection, const char *path,
    unsigned char *buffer, size_t buffer_size, uint64_t *out_bytes)
{
    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        return -1;
    }
    uint64_t total = 0;
    int result = 0;
    for (;;) {
        ssize_t received = netins_udp_recv(connection, buffer,
            buffer_size, NETINS_UDP_MSG_NONE);
        if (received < 0) {
            result = -1;
            break;
        }
        if (received == 0) {
            break;
        }
        if (fwrite(buffer, 1, (size_t)received, file)
            != (size_t)received)
        {
            result = -1;
            break;
        }
        total += (uint64_t)received;
    }
    if (fclose(file) != 0 && result == 0) {
        result = -1;
    }
    if (result == 0) {
        *out_bytes = total;
    }
    return result;
}

static int
configure_client(netins_udp_t *connection, const char *server_name,
    const char *ca_file, int interface_count, char **interface_names)
{
    uint32_t timeout_ms = NETINS_UDP_DEMO_TIMEOUT_MS;
    if (netins_udp_set_opt(connection, NETINS_UDP_OPT_CONNECT_TIMEOUT_MS,
            &timeout_ms, sizeof(timeout_ms)) != NETINS_UDP_OK
        || netins_udp_set_opt(connection, NETINS_UDP_OPT_SEND_TIMEOUT_MS,
            &timeout_ms, sizeof(timeout_ms)) != NETINS_UDP_OK
        || netins_udp_set_opt(connection, NETINS_UDP_OPT_RECV_TIMEOUT_MS,
            &timeout_ms, sizeof(timeout_ms)) != NETINS_UDP_OK)
    {
        return -1;
    }

    netins_udp_tls_config_t tls = NETINS_UDP_TLS_CONFIG_INITIALIZER;
    snprintf(tls.server_name, sizeof(tls.server_name), "%s",
        server_name);
    if (strcmp(ca_file, "-") == 0) {
        tls.verify_peer = 0;
    } else {
        snprintf(tls.ca_file, sizeof(tls.ca_file), "%s", ca_file);
    }
    if (netins_udp_set_opt(connection, NETINS_UDP_OPT_TLS,
            &tls, sizeof(tls)) != NETINS_UDP_OK)
    {
        return -1;
    }

    if (interface_count > 0) {
        netins_udp_interfaces_t interfaces =
            NETINS_UDP_INTERFACES_INITIALIZER;
        interfaces.interface_count = (uint32_t)interface_count;
        for (int i = 0; i < interface_count; ++i) {
            snprintf(interfaces.names[i], sizeof(interfaces.names[i]),
                "%s", interface_names[i]);
        }
        if (netins_udp_set_opt(connection, NETINS_UDP_OPT_INTERFACES,
                &interfaces, sizeof(interfaces)) != NETINS_UDP_OK)
        {
            return -1;
        }
    }
    return 0;
}

int
main(int argc, char **argv)
{
    if (argc < 7 || argc > 7 + (int)NETINS_UDP_MAX_PATHS) {
        fprintf(stderr,
            "usage: %s HOST PORT SERVER_NAME CA_FILE SEND_FILE "
            "RECEIVE_FILE [INTERFACE ...]\n",
            argv[0]);
        return 2;
    }
    int port = parse_port(argv[2]);
    int interface_count = argc - 7;
    if (port < 0) {
        fprintf(stderr, "invalid port: %s\n", argv[2]);
        return 2;
    }

    int result = 1;
    netins_udp_t *connection = NULL;
    unsigned char *buffer = NULL;
    if (netins_udp_init() != NETINS_UDP_OK) {
        fprintf(stderr, "netins_udp_init failed: errno=%d\n", errno);
        return 1;
    }

    connection = netins_udp_new(NETINS_UDP_TYPE_MPQUIC);
    if (connection == NULL
        || configure_client(connection, argv[3], argv[4],
            interface_count, &argv[7]) != 0
        || netins_udp_connect(connection, argv[1], port) != NETINS_UDP_OK)
    {
        print_connection_error("client setup/connect", connection);
        goto cleanup;
    }

    buffer = malloc(NETINS_UDP_DEMO_BUFFER_SIZE);
    if (buffer == NULL) {
        fprintf(stderr, "failed to allocate transfer buffer\n");
        goto cleanup;
    }
    uint64_t bytes_sent = 0;
    uint64_t bytes_received = 0;
    if (send_file(connection, argv[5], buffer,
            NETINS_UDP_DEMO_BUFFER_SIZE, &bytes_sent) != 0)
    {
        print_connection_error("send_file", connection);
        goto cleanup;
    }
    if (receive_file(connection, argv[6], buffer,
            NETINS_UDP_DEMO_BUFFER_SIZE, &bytes_received) != 0)
    {
        print_connection_error("receive_file", connection);
        goto cleanup;
    }

    netins_udp_snapshot_t snapshot = NETINS_UDP_SNAPSHOT_INITIALIZER;
    if (netins_udp_get_snapshot(connection, &snapshot) == NETINS_UDP_OK) {
        printf("sent=%llu received=%llu paths=%u active_paths=%u "
               "multipath=%d\n",
            (unsigned long long)bytes_sent,
            (unsigned long long)bytes_received,
            snapshot.path_count, snapshot.info.active_path_count,
            snapshot.info.multipath_active);
    } else {
        printf("sent=%llu received=%llu\n",
            (unsigned long long)bytes_sent,
            (unsigned long long)bytes_received);
    }
    (void)netins_udp_shutdown(connection, 0);
    result = 0;

cleanup:
    free(buffer);
    if (connection != NULL) {
        netins_udp_close(connection);
    }
    if (netins_udp_cleanup() != NETINS_UDP_OK && result == 0) {
        fprintf(stderr, "netins_udp_cleanup failed: errno=%d\n", errno);
        result = 1;
    }
    return result;
}
