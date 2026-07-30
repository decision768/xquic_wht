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
configure_listener(netins_udp_t *listener, const char *certificate,
    const char *private_key)
{
    uint32_t timeout_ms = NETINS_UDP_DEMO_TIMEOUT_MS;
    netins_udp_tls_config_t tls = NETINS_UDP_TLS_CONFIG_INITIALIZER;
    snprintf(tls.certificate_file, sizeof(tls.certificate_file), "%s",
        certificate);
    snprintf(tls.private_key_file, sizeof(tls.private_key_file), "%s",
        private_key);
    return netins_udp_set_opt(listener, NETINS_UDP_OPT_TLS,
            &tls, sizeof(tls)) == NETINS_UDP_OK
        && netins_udp_set_opt(listener, NETINS_UDP_OPT_SEND_TIMEOUT_MS,
            &timeout_ms, sizeof(timeout_ms)) == NETINS_UDP_OK
        && netins_udp_set_opt(listener, NETINS_UDP_OPT_RECV_TIMEOUT_MS,
            &timeout_ms, sizeof(timeout_ms)) == NETINS_UDP_OK
        && netins_udp_set_opt(listener, NETINS_UDP_OPT_CLOSE_TIMEOUT_MS,
            &timeout_ms, sizeof(timeout_ms)) == NETINS_UDP_OK
        ? 0 : -1;
}

int
main(int argc, char **argv)
{
    if (argc != 7) {
        fprintf(stderr,
            "usage: %s BIND_HOST PORT CERTIFICATE PRIVATE_KEY "
            "RECEIVE_FILE SEND_FILE\n",
            argv[0]);
        return 2;
    }
    int port = parse_port(argv[2]);
    if (port < 0) {
        fprintf(stderr, "invalid port: %s\n", argv[2]);
        return 2;
    }

    int result = 1;
    netins_udp_t *listener = NULL;
    netins_udp_t *connection = NULL;
    unsigned char *buffer = NULL;
    if (netins_udp_init() != NETINS_UDP_OK) {
        fprintf(stderr, "netins_udp_init failed: errno=%d\n", errno);
        return 1;
    }

    listener = netins_udp_new(NETINS_UDP_TYPE_MPQUIC);
    const char *bind_host = strcmp(argv[1], "*") == 0 ? NULL : argv[1];
    if (listener == NULL
        || configure_listener(listener, argv[3], argv[4]) != 0
        || netins_udp_listen(listener, bind_host, port) == NULL)
    {
        print_connection_error("listen", listener);
        goto cleanup;
    }
    connection = netins_udp_accept(listener, NULL, NULL);
    if (connection == NULL) {
        print_connection_error("accept", listener);
        goto cleanup;
    }

    /*
     * This is a one-connection demo. The accepted handle must remain usable
     * after its independently owned listener is closed.
     */
    netins_udp_close(listener);
    listener = NULL;

    buffer = malloc(NETINS_UDP_DEMO_BUFFER_SIZE);
    if (buffer == NULL) {
        fprintf(stderr, "failed to allocate transfer buffer\n");
        goto cleanup;
    }
    uint64_t bytes_received = 0;
    uint64_t bytes_sent = 0;
    if (receive_file(connection, argv[5], buffer,
            NETINS_UDP_DEMO_BUFFER_SIZE, &bytes_received) != 0)
    {
        print_connection_error("receive_file", connection);
        goto cleanup;
    }
    if (send_file(connection, argv[6], buffer,
            NETINS_UDP_DEMO_BUFFER_SIZE, &bytes_sent) != 0)
    {
        print_connection_error("send_file", connection);
        goto cleanup;
    }

    netins_udp_snapshot_t snapshot = NETINS_UDP_SNAPSHOT_INITIALIZER;
    if (netins_udp_get_snapshot(connection, &snapshot) == NETINS_UDP_OK) {
        printf("received=%llu sent=%llu paths=%u active_paths=%u "
               "multipath=%d\n",
            (unsigned long long)bytes_received,
            (unsigned long long)bytes_sent,
            snapshot.path_count, snapshot.info.active_path_count,
            snapshot.info.multipath_active);
    } else {
        printf("received=%llu sent=%llu\n",
            (unsigned long long)bytes_received,
            (unsigned long long)bytes_sent);
    }
    (void)netins_udp_shutdown(connection, 0);
    result = 0;

cleanup:
    free(buffer);
    if (connection != NULL) {
        netins_udp_close(connection);
    }
    if (listener != NULL) {
        netins_udp_close(listener);
    }
    if (netins_udp_cleanup() != NETINS_UDP_OK && result == 0) {
        fprintf(stderr, "netins_udp_cleanup failed: errno=%d\n", errno);
        result = 1;
    }
    return result;
}
