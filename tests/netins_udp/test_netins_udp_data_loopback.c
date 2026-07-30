#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <xquic/netins_udp.h>

#define DATA_LOOPBACK_SIZE ((20U * 1024U * 1024U) + 17U)
#define DATA_LOOPBACK_CHUNK (4U * 1024U * 1024U)
#define DATA_LOOPBACK_TIMEOUT_MS 30000U

typedef struct data_loopback_context_s {
    netins_udp_t *listener;
    size_t client_send_size;
    size_t server_send_size;
    size_t client_received;
    int server_result;
} data_loopback_context_t;

static void
fill_pattern(unsigned char *buffer, size_t length, size_t offset,
    unsigned int seed)
{
    for (size_t i = 0; i < length; ++i) {
        buffer[i] = (unsigned char)(((offset + i) * 131U + seed) & 0xffU);
    }
}

static int
verify_pattern(const unsigned char *buffer, size_t length, size_t offset,
    unsigned int seed)
{
    for (size_t i = 0; i < length; ++i) {
        unsigned char expected =
            (unsigned char)(((offset + i) * 131U + seed) & 0xffU);
        if (buffer[i] != expected) {
            return -1;
        }
    }
    return 0;
}

static int
send_pattern(netins_udp_t *connection, size_t total_size,
    unsigned int seed)
{
    unsigned char *buffer = malloc(DATA_LOOPBACK_CHUNK);
    if (buffer == NULL) {
        return -1;
    }
    size_t offset = 0;
    while (offset < total_size) {
        size_t amount = total_size - offset;
        if (amount > DATA_LOOPBACK_CHUNK) {
            amount = DATA_LOOPBACK_CHUNK;
        }
        fill_pattern(buffer, amount, offset, seed);
        size_t sent_offset = 0;
        while (sent_offset < amount) {
            ssize_t sent = netins_udp_send(connection,
                buffer + sent_offset, amount - sent_offset,
                NETINS_UDP_MSG_NONE);
            if (sent <= 0) {
                free(buffer);
                return -1;
            }
            sent_offset += (size_t)sent;
        }
        offset += amount;
    }
    int result = netins_udp_send(connection, NULL, 0,
        NETINS_UDP_MSG_FIN) == 0 ? 0 : -1;
    free(buffer);
    return result;
}

static int
receive_pattern(netins_udp_t *connection, size_t expected_size,
    unsigned int seed, size_t *out_received)
{
    unsigned char *buffer = malloc(DATA_LOOPBACK_CHUNK);
    if (buffer == NULL) {
        return -1;
    }
    size_t offset = 0;
    int result = 0;
    for (;;) {
        ssize_t received = netins_udp_recv(connection, buffer,
            DATA_LOOPBACK_CHUNK, NETINS_UDP_MSG_NONE);
        if (received < 0) {
            result = -1;
            break;
        }
        if (received == 0) {
            break;
        }
        if ((size_t)received > expected_size - offset
            || verify_pattern(buffer, (size_t)received, offset, seed) != 0)
        {
            result = -1;
            break;
        }
        offset += (size_t)received;
    }
    if (offset != expected_size) {
        result = -1;
    }
    *out_received = offset;
    free(buffer);
    return result;
}

static void *
data_loopback_server_main(void *argument)
{
    data_loopback_context_t *context = argument;
    netins_udp_t *connection =
        netins_udp_accept(context->listener, NULL, NULL);
    if (connection == NULL) {
        context->server_result = -1;
        return NULL;
    }

    size_t received = 0;
    if (receive_pattern(connection, context->client_send_size, 17U,
            &received) != 0
        || received != context->client_send_size
        || send_pattern(connection, context->server_send_size, 83U) != 0)
    {
        context->server_result = -1;
    }

    /*
     * Mirror the standalone server: finish the response and begin graceful
     * shutdown immediately. The accepted data connection remains alive until
     * the client has consumed every response lane and closes the connection.
     */
    if (context->server_result == 0
        && netins_udp_shutdown(connection, 0) != NETINS_UDP_OK)
    {
        context->server_result = -1;
    }
    netins_udp_close(connection);
    return NULL;
}

int
main(int argc, char **argv)
{
    if (argc != 4
        && (argc != 5 || strcmp(argv[4], "--empty-upload") != 0))
    {
        fprintf(stderr,
            "usage: %s CERTIFICATE PRIVATE_KEY PORT [--empty-upload]\n",
            argv[0]);
        return 2;
    }
    int port = atoi(argv[3]);
    if (port <= 0 || port > 65535) {
        return 2;
    }

    data_loopback_context_t context;
    memset(&context, 0, sizeof(context));
    context.client_send_size =
        argc == 5 ? 0 : DATA_LOOPBACK_SIZE;
    context.server_send_size = DATA_LOOPBACK_SIZE;
    if (netins_udp_init() != NETINS_UDP_OK) {
        return 1;
    }

    int result = 1;
    netins_udp_t *client = NULL;
    context.listener = netins_udp_new(NETINS_UDP_TYPE_QUIC);
    netins_udp_tls_config_t server_tls = NETINS_UDP_TLS_CONFIG_INITIALIZER;
    snprintf(server_tls.certificate_file,
        sizeof(server_tls.certificate_file), "%s", argv[1]);
    snprintf(server_tls.private_key_file,
        sizeof(server_tls.private_key_file), "%s", argv[2]);
    uint32_t timeout_ms = DATA_LOOPBACK_TIMEOUT_MS;
    if (context.listener == NULL
        || netins_udp_set_opt(context.listener, NETINS_UDP_OPT_TLS,
            &server_tls, sizeof(server_tls)) != NETINS_UDP_OK
        || netins_udp_set_opt(context.listener,
            NETINS_UDP_OPT_SEND_TIMEOUT_MS,
            &timeout_ms, sizeof(timeout_ms)) != NETINS_UDP_OK
        || netins_udp_set_opt(context.listener,
            NETINS_UDP_OPT_RECV_TIMEOUT_MS,
            &timeout_ms, sizeof(timeout_ms)) != NETINS_UDP_OK
        || netins_udp_set_opt(context.listener,
            NETINS_UDP_OPT_CLOSE_TIMEOUT_MS,
            &timeout_ms, sizeof(timeout_ms)) != NETINS_UDP_OK
        || netins_udp_listen(context.listener, "127.0.0.1", port) == NULL)
    {
        goto cleanup;
    }

    client = netins_udp_new(NETINS_UDP_TYPE_QUIC);
    netins_udp_tls_config_t client_tls = NETINS_UDP_TLS_CONFIG_INITIALIZER;
    client_tls.verify_peer = 0;
    snprintf(client_tls.server_name, sizeof(client_tls.server_name), "%s",
        "127.0.0.1");
    if (client == NULL
        || netins_udp_set_opt(client, NETINS_UDP_OPT_TLS,
            &client_tls, sizeof(client_tls)) != NETINS_UDP_OK
        || netins_udp_set_opt(client, NETINS_UDP_OPT_SEND_TIMEOUT_MS,
            &timeout_ms, sizeof(timeout_ms)) != NETINS_UDP_OK
        || netins_udp_set_opt(client, NETINS_UDP_OPT_RECV_TIMEOUT_MS,
            &timeout_ms, sizeof(timeout_ms)) != NETINS_UDP_OK
        || netins_udp_set_opt(client, NETINS_UDP_OPT_CLOSE_TIMEOUT_MS,
            &timeout_ms, sizeof(timeout_ms)) != NETINS_UDP_OK
        || netins_udp_connect(client, "127.0.0.1", port) != NETINS_UDP_OK)
    {
        goto cleanup;
    }

    pthread_t server_thread;
    if (pthread_create(&server_thread, NULL,
            data_loopback_server_main, &context) != 0)
    {
        goto cleanup;
    }
    int client_transfer_result = 0;
    if (send_pattern(client, context.client_send_size, 17U) != 0
        || receive_pattern(client, context.server_send_size, 83U,
            &context.client_received) != 0)
    {
        client_transfer_result = -1;
        (void)netins_udp_shutdown(client, 1);
    }
    if (client_transfer_result == 0
        && netins_udp_shutdown(client, 0) != NETINS_UDP_OK)
    {
        client_transfer_result = -1;
    }

    (void)pthread_join(server_thread, NULL);
    if (client_transfer_result == 0 && context.server_result == 0
        && context.client_received == context.server_send_size)
    {
        result = 0;
    }

cleanup:
    if (client != NULL) {
        netins_udp_close(client);
    }
    if (context.listener != NULL) {
        netins_udp_close(context.listener);
    }
    if (netins_udp_cleanup() != NETINS_UDP_OK) {
        result = 1;
    }
    printf("netins_udp_data_loopback_test: %s\n",
        result == 0 ? "PASS" : "FAIL");
    return result;
}
