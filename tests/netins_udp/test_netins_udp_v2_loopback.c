#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <xquic/netins_udp.h>

#define LOOPBACK_REQUEST_COUNT 5
#define LOOPBACK_CHUNK_SIZE (64U * 1024U)
#define LOOPBACK_LARGE_POST_SIZE ((17U * 1024U * 1024U) + 1U)
#define LOOPBACK_SMALL_POST_SIZE (1U * 1024U * 1024U)
#define LOOPBACK_TIMEOUT_MS 30000U
#define LOOPBACK_HANDSHAKE_TIMEOUT_MS 200U

typedef struct loopback_context_s {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    netins_udp_t *listener;
    netins_udp_t *accepted;
    int start_accept;
    int stop_server;
    int accepted_ready;
    int listener_closed;
    int clients_done;
    int server_result;
} loopback_context_t;

typedef struct request_context_s {
    netins_udp_t *connection;
    size_t body_size;
    unsigned int index;
    int result;
} request_context_t;

typedef struct server_request_context_s {
    netins_udp_h3_request_t *request;
    int result;
} server_request_context_t;

static int
wait_connection_events(netins_udp_t *connection, uint32_t wanted)
{
    int fd = netins_udp_get_fd(connection);
    if (fd < 0) {
        return -1;
    }
    for (;;) {
        struct pollfd descriptor = {
            .fd = fd,
            .events = POLLIN,
        };
        int poll_result;
        do {
            poll_result = poll(&descriptor, 1, LOOPBACK_TIMEOUT_MS);
        } while (poll_result < 0 && errno == EINTR);
        if (poll_result <= 0
            || (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
        {
            errno = poll_result == 0 ? ETIMEDOUT
                : (errno != 0 ? errno : EIO);
            return -1;
        }
        uint32_t events = 0;
        if (netins_udp_get_events(connection, &events) != NETINS_UDP_OK) {
            return -1;
        }
        if ((events & wanted) != 0) {
            return 0;
        }
        if ((events & (NETINS_UDP_EVENT_CLOSED | NETINS_UDP_EVENT_ERROR))
            != 0)
        {
            errno = ECONNABORTED;
            return -1;
        }
    }
}

static int
wait_request_events(netins_udp_h3_request_t *request, uint32_t wanted)
{
    int fd = netins_udp_h3_request_get_fd(request);
    if (fd < 0) {
        return -1;
    }
    for (;;) {
        struct pollfd descriptor = {
            .fd = fd,
            .events = POLLIN,
        };
        int poll_result;
        do {
            poll_result = poll(&descriptor, 1, LOOPBACK_TIMEOUT_MS);
        } while (poll_result < 0 && errno == EINTR);
        if (poll_result <= 0
            || (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
        {
            errno = poll_result == 0 ? ETIMEDOUT
                : (errno != 0 ? errno : EIO);
            return -1;
        }
        uint32_t events = 0;
        if (netins_udp_h3_request_get_events(request, &events)
            != NETINS_UDP_OK)
        {
            return -1;
        }
        if ((events & wanted) != 0) {
            return 0;
        }
        if ((events & (NETINS_UDP_EVENT_CLOSED | NETINS_UDP_EVENT_ERROR))
            != 0)
        {
            errno = ECONNABORTED;
            return -1;
        }
    }
}

static void
print_connection_error(const char *operation, netins_udp_t *connection)
{
    char message[NETINS_UDP_ERROR_MSG_LEN] = {0};
    if (connection != NULL) {
        (void)netins_udp_get_last_error_msg(connection, message,
            sizeof(message));
    }
    fprintf(stderr, "%s failed: errno=%d message=%s\n", operation, errno,
        message);
}

static void *
server_request_main(void *argument)
{
    server_request_context_t *context = argument;
    netins_udp_h3_response_config_t response;
    netins_udp_h3_response_config_init(&response);
    response.status = 204;
    response.content_length = 0;
    /*
     * A 204 response finishes on its headers. The public API intentionally
     * rejects every body-send call for no-body statuses, including a
     * zero-length FIN.
     */
    if (netins_udp_h3_request_set_response(context->request, &response)
        != NETINS_UDP_OK)
    {
        context->result = -1;
    }
    /*
     * Intentionally close without reading the POST body. The backend must
     * retain the XQUIC request and discard-drain it in bounded batches.
     */
    netins_udp_h3_request_close(context->request);
    return NULL;
}

static void *
server_main(void *argument)
{
    loopback_context_t *context = argument;
    pthread_mutex_lock(&context->lock);
    while (!context->start_accept) {
        pthread_cond_wait(&context->cond, &context->lock);
    }
    int stop_server = context->stop_server;
    pthread_mutex_unlock(&context->lock);
    if (stop_server) {
        return NULL;
    }

    netins_udp_t *accepted = netins_udp_accept(context->listener, NULL, NULL);
    pthread_mutex_lock(&context->lock);
    if (accepted == NULL) {
        context->server_result = -1;
        context->accepted_ready = 1;
        pthread_cond_broadcast(&context->cond);
        pthread_mutex_unlock(&context->lock);
        return NULL;
    }
    context->accepted = accepted;
    context->accepted_ready = 1;
    pthread_cond_broadcast(&context->cond);
    while (!context->listener_closed) {
        pthread_cond_wait(&context->cond, &context->lock);
    }
    pthread_mutex_unlock(&context->lock);

    pthread_t workers[LOOPBACK_REQUEST_COUNT];
    server_request_context_t requests[LOOPBACK_REQUEST_COUNT];
    memset(workers, 0, sizeof(workers));
    memset(requests, 0, sizeof(requests));
    unsigned int started = 0;
    for (unsigned int i = 0; i < LOOPBACK_REQUEST_COUNT; ++i) {
        if (wait_connection_events(accepted,
                NETINS_UDP_EVENT_H3_REQUEST_ACCEPTABLE) != 0
            || netins_udp_h3_request_accept(accepted, &requests[i].request)
                != NETINS_UDP_OK)
        {
            print_connection_error("request_accept", accepted);
            context->server_result = -1;
            break;
        }
        if (pthread_create(&workers[i], NULL, server_request_main,
                &requests[i]) != 0)
        {
            netins_udp_h3_request_close(requests[i].request);
            requests[i].request = NULL;
            context->server_result = -1;
            break;
        }
        started++;
    }

    for (unsigned int i = 0; i < started; ++i) {
        (void)pthread_join(workers[i], NULL);
        if (requests[i].result != 0) {
            context->server_result = -1;
        }
    }

    pthread_mutex_lock(&context->lock);
    while (!context->clients_done) {
        pthread_cond_wait(&context->cond, &context->lock);
    }
    pthread_mutex_unlock(&context->lock);

    netins_udp_close(accepted);
    context->accepted = NULL;
    return NULL;
}

static void *
client_request_main(void *argument)
{
    request_context_t *context = argument;
    netins_udp_h3_request_config_t config;
    netins_udp_h3_request_config_init(&config);
    snprintf(config.method, sizeof(config.method), "%s",
        context->body_size == 0 ? "GET" : "POST");
    snprintf(config.path, sizeof(config.path), "/stage8/%u", context->index);
    snprintf(config.authority, sizeof(config.authority), "%s",
        "127.0.0.1");
    config.content_length = context->body_size;

    netins_udp_h3_request_t *request = NULL;
    if (netins_udp_h3_request_open(context->connection, &config, &request)
        != NETINS_UDP_OK)
    {
        context->result = -1;
        return NULL;
    }
    if (context->body_size != 0
        && wait_request_events(request, NETINS_UDP_EVENT_WRITABLE) != 0)
    {
        netins_udp_h3_request_close(request);
        context->result = -1;
        return NULL;
    }

    unsigned char *chunk = malloc(LOOPBACK_CHUNK_SIZE);
    if (chunk == NULL) {
        netins_udp_h3_request_close(request);
        context->result = -1;
        return NULL;
    }
    memset(chunk, (int)(context->index + 1U), LOOPBACK_CHUNK_SIZE);

    size_t sent_total = 0;
    while (sent_total < context->body_size) {
        size_t remaining = context->body_size - sent_total;
        size_t amount = remaining < LOOPBACK_CHUNK_SIZE
            ? remaining : LOOPBACK_CHUNK_SIZE;
        int flags = amount == remaining ? NETINS_UDP_MSG_FIN : 0;
        ssize_t sent = netins_udp_h3_request_send(request, chunk, amount,
            flags);
        if (sent <= 0) {
            context->result = -1;
            break;
        }
        sent_total += (size_t)sent;
    }
    /* GET is finished with its request headers by request_open(). */
    free(chunk);

    unsigned char response_byte;
    if (context->result == 0
        && wait_request_events(request, NETINS_UDP_EVENT_READABLE) != 0)
    {
        context->result = -1;
    }
    if (context->result == 0
        && netins_udp_h3_request_recv(request, &response_byte,
            sizeof(response_byte), 0) != 0)
    {
        context->result = -1;
    }
    netins_udp_h3_request_info_t info =
        NETINS_UDP_H3_REQUEST_INFO_INITIALIZER;
    if (context->result == 0
        && (netins_udp_h3_request_get_info(request, &info) != NETINS_UDP_OK
            || info.response_status != 204
            || info.bytes_sent != context->body_size))
    {
        context->result = -1;
    }
    netins_udp_h3_request_close(request);
    return NULL;
}

int
main(int argc, char **argv)
{
    if (argc != 4) {
        fprintf(stderr, "usage: %s CERTIFICATE PRIVATE_KEY PORT\n", argv[0]);
        return 2;
    }
    int port = atoi(argv[3]);
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "invalid port\n");
        return 2;
    }

    loopback_context_t context;
    memset(&context, 0, sizeof(context));
    if (pthread_mutex_init(&context.lock, NULL) != 0
        || pthread_cond_init(&context.cond, NULL) != 0)
    {
        return 1;
    }
    if (netins_udp_init() != NETINS_UDP_OK) {
        return 1;
    }

    context.listener = netins_udp_new(NETINS_UDP_TYPE_QUIC);
    if (context.listener == NULL) {
        return 1;
    }
    netins_udp_tls_config_t server_tls = NETINS_UDP_TLS_CONFIG_INITIALIZER;
    snprintf(server_tls.certificate_file,
        sizeof(server_tls.certificate_file), "%s", argv[1]);
    snprintf(server_tls.private_key_file,
        sizeof(server_tls.private_key_file), "%s", argv[2]);
    uint32_t timeout_ms = LOOPBACK_TIMEOUT_MS;
    int nonblock = 1;
    if (netins_udp_set_opt(context.listener, NETINS_UDP_OPT_TLS,
            &server_tls, sizeof(server_tls)) != NETINS_UDP_OK
        || netins_udp_set_opt(context.listener, NETINS_UDP_OPT_NONBLOCK,
            &nonblock, sizeof(nonblock)) != NETINS_UDP_OK
        || netins_udp_set_opt(context.listener, NETINS_UDP_OPT_SEND_TIMEOUT_MS,
            &timeout_ms, sizeof(timeout_ms)) != NETINS_UDP_OK
        || netins_udp_set_opt(context.listener, NETINS_UDP_OPT_RECV_TIMEOUT_MS,
            &timeout_ms, sizeof(timeout_ms)) != NETINS_UDP_OK
        || netins_udp_listen(context.listener, "127.0.0.1", port) == NULL)
    {
        print_connection_error("listen", context.listener);
        netins_udp_close(context.listener);
        (void)netins_udp_cleanup();
        return 1;
    }

    pthread_t server_thread;
    if (pthread_create(&server_thread, NULL, server_main, &context) != 0) {
        netins_udp_close(context.listener);
        (void)netins_udp_cleanup();
        return 1;
    }

    netins_udp_t *client = netins_udp_new(NETINS_UDP_TYPE_QUIC);
    int main_result = 0;
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
        || netins_udp_connect(client, "127.0.0.1", port) != NETINS_UDP_OK)
    {
        print_connection_error("connect", client);
        main_result = -1;
    }

    if (main_result == 0
        && (wait_connection_events(client, NETINS_UDP_EVENT_CONNECTED) != 0
            || wait_connection_events(context.listener,
                NETINS_UDP_EVENT_ACCEPTABLE) != 0))
    {
        main_result = -1;
    }

    pthread_mutex_lock(&context.lock);
    context.stop_server = main_result != 0;
    context.start_accept = 1;
    pthread_cond_broadcast(&context.cond);
    while (!context.stop_server && !context.accepted_ready) {
        pthread_cond_wait(&context.cond, &context.lock);
    }
    int accepted_ready = context.accepted_ready
        && context.server_result == 0;
    pthread_mutex_unlock(&context.lock);

    if (accepted_ready) {
        /*
         * Stage 6 ownership regression: the accepted connection must continue
         * on the shared UDP socket/engine after its public listener closes.
         */
        netins_udp_close(context.listener);
        context.listener = NULL;
        pthread_mutex_lock(&context.lock);
        context.listener_closed = 1;
        pthread_cond_broadcast(&context.cond);
        pthread_mutex_unlock(&context.lock);
    }

    pthread_t client_threads[LOOPBACK_REQUEST_COUNT];
    request_context_t requests[LOOPBACK_REQUEST_COUNT];
    memset(client_threads, 0, sizeof(client_threads));
    memset(requests, 0, sizeof(requests));
    unsigned int started = 0;
    if (main_result == 0 && accepted_ready) {
        for (unsigned int i = 0; i < LOOPBACK_REQUEST_COUNT; ++i) {
            requests[i].connection = client;
            requests[i].body_size = i == 0 ? 0
                : (i == 1
                    ? LOOPBACK_LARGE_POST_SIZE
                    : LOOPBACK_SMALL_POST_SIZE);
            requests[i].index = i;
            if (pthread_create(&client_threads[i], NULL,
                    client_request_main, &requests[i]) != 0)
            {
                requests[i].result = -1;
                break;
            }
            started++;
        }
    }
    for (unsigned int i = 0; i < started; ++i) {
        (void)pthread_join(client_threads[i], NULL);
        if (requests[i].result != 0) {
            main_result = -1;
        }
    }

    pthread_mutex_lock(&context.lock);
    context.clients_done = 1;
    pthread_cond_broadcast(&context.cond);
    pthread_mutex_unlock(&context.lock);
    (void)pthread_join(server_thread, NULL);

    if (client != NULL && accepted_ready
        && wait_connection_events(client,
            NETINS_UDP_EVENT_CLOSED | NETINS_UDP_EVENT_ERROR) != 0)
    {
        main_result = -1;
    }
    if (client != NULL) {
        netins_udp_close(client);
    }

    /*
     * A bound UDP blackhole makes the handshake timeout deterministic without
     * relying on an unused port or an external network.
     */
    int blackhole_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in blackhole_address;
    memset(&blackhole_address, 0, sizeof(blackhole_address));
    blackhole_address.sin_family = AF_INET;
    blackhole_address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t blackhole_address_len = sizeof(blackhole_address);
    if (blackhole_fd < 0
        || bind(blackhole_fd, (const struct sockaddr *)&blackhole_address,
            sizeof(blackhole_address)) != 0
        || getsockname(blackhole_fd,
            (struct sockaddr *)&blackhole_address,
            &blackhole_address_len) != 0)
    {
        main_result = -1;
    } else {
        netins_udp_t *timeout_client =
            netins_udp_new(NETINS_UDP_TYPE_QUIC);
        netins_udp_tls_config_t timeout_tls =
            NETINS_UDP_TLS_CONFIG_INITIALIZER;
        timeout_tls.verify_peer = 0;
        uint32_t handshake_timeout_ms = LOOPBACK_HANDSHAKE_TIMEOUT_MS;
        if (timeout_client == NULL
            || netins_udp_set_opt(timeout_client, NETINS_UDP_OPT_TLS,
                &timeout_tls, sizeof(timeout_tls)) != NETINS_UDP_OK
            || netins_udp_set_opt(timeout_client,
                NETINS_UDP_OPT_CONNECT_TIMEOUT_MS,
                &handshake_timeout_ms,
                sizeof(handshake_timeout_ms)) != NETINS_UDP_OK
            || netins_udp_connect(timeout_client, "127.0.0.1",
                ntohs(blackhole_address.sin_port)) != NETINS_UDP_ERROR)
        {
            main_result = -1;
        } else {
            netins_udp_close_reason_t close_reason =
                NETINS_UDP_CLOSE_REASON_INITIALIZER;
            if (netins_udp_get_close_reason(timeout_client, &close_reason)
                    != NETINS_UDP_OK
                || close_reason.source
                    != NETINS_UDP_CLOSE_HANDSHAKE_TIMEOUT
                || close_reason.system_error != ETIMEDOUT)
            {
                main_result = -1;
            }
        }
        if (timeout_client != NULL) {
            netins_udp_close(timeout_client);
        }
    }
    if (blackhole_fd >= 0) {
        close(blackhole_fd);
    }
    if (context.listener != NULL) {
        netins_udp_close(context.listener);
        context.listener = NULL;
    }
    int cleanup_result = netins_udp_cleanup();
    pthread_cond_destroy(&context.cond);
    pthread_mutex_destroy(&context.lock);
    if (main_result != 0 || context.server_result != 0
        || cleanup_result != NETINS_UDP_OK
        || started != LOOPBACK_REQUEST_COUNT)
    {
        fprintf(stderr, "netins_udp_v2_loopback_test: FAIL\n");
        return 1;
    }
    printf("netins_udp_v2_loopback_test: PASS\n");
    return 0;
}
