#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "api/draft/netins_udp_v2.h"
#include "netins_udp_v2_transport.h"

struct netins_udp_client_s {
    int state;
    int error_code;
    char error_message[NETINS_UDP_V2_TRANSPORT_ERROR_MSG_LEN];
    uint32_t close_timeout_ms;
};

static netins_udp_v2_transport_config_t g_last_config;
static netins_udp_v2_transport_client_t *g_last_client;
static int g_create_count;
static int g_close_count;
static int g_start_result;
static int g_wait_result;
static pthread_mutex_t g_wait_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_wait_cond = PTHREAD_COND_INITIALIZER;
static int g_block_wait;
static int g_wait_entered;
static int g_close_requested;
static int g_block_start;
static int g_start_entered;
static int g_start_continue;

int
netins_udp_v2_transport_client_create(
    const netins_udp_v2_transport_config_t *config,
    netins_udp_v2_transport_client_t **out_client)
{
    g_last_config = *config;
    netins_udp_v2_transport_client_t *client = calloc(1, sizeof(*client));
    assert(client != NULL);
    client->state = NETINS_UDP_V2_TRANSPORT_NEW;
    client->close_timeout_ms = config->close_timeout_ms;
    *out_client = client;
    g_last_client = client;
    g_create_count++;
    return 0;
}

int
netins_udp_v2_transport_client_start(
    netins_udp_v2_transport_client_t *client)
{
    pthread_mutex_lock(&g_wait_lock);
    if (g_block_start) {
        g_start_entered = 1;
        pthread_cond_broadcast(&g_wait_cond);
        while (!g_start_continue) {
            pthread_cond_wait(&g_wait_cond, &g_wait_lock);
        }
    }
    pthread_mutex_unlock(&g_wait_lock);
    if (g_start_result != 0) {
        client->state = NETINS_UDP_V2_TRANSPORT_ERROR;
        client->error_code = EIO;
        strcpy(client->error_message, "mock start failed");
        errno = EIO;
        return -1;
    }
    client->state = NETINS_UDP_V2_TRANSPORT_CONNECTING;
    return 0;
}

int
netins_udp_v2_transport_client_wait_connected(
    netins_udp_v2_transport_client_t *client, uint32_t timeout_ms)
{
    assert(timeout_ms != 0);
    pthread_mutex_lock(&g_wait_lock);
    if (g_block_wait) {
        g_wait_entered = 1;
        pthread_cond_broadcast(&g_wait_cond);
        while (!g_close_requested) {
            pthread_cond_wait(&g_wait_cond, &g_wait_lock);
        }
        client->state = NETINS_UDP_V2_TRANSPORT_ERROR;
        client->error_code = ECANCELED;
        strcpy(client->error_message, "mock connect canceled");
        pthread_mutex_unlock(&g_wait_lock);
        errno = ECANCELED;
        return -1;
    }
    pthread_mutex_unlock(&g_wait_lock);
    if (g_wait_result != 0) {
        client->state = NETINS_UDP_V2_TRANSPORT_ERROR;
        client->error_code = ETIMEDOUT;
        strcpy(client->error_message, "mock handshake timeout");
        errno = ETIMEDOUT;
        return -1;
    }
    client->state = NETINS_UDP_V2_TRANSPORT_CONNECTED;
    return 0;
}

int
netins_udp_v2_transport_client_get_state(
    netins_udp_v2_transport_client_t *client)
{
    return client->state;
}

int
netins_udp_v2_transport_client_get_last_error_code(
    netins_udp_v2_transport_client_t *client)
{
    return client->error_code;
}

int
netins_udp_v2_transport_client_get_last_error_msg(
    netins_udp_v2_transport_client_t *client,
    char *buffer, size_t buffer_len)
{
    snprintf(buffer, buffer_len, "%s", client->error_message);
    return 0;
}

void
netins_udp_v2_transport_client_set_close_timeout(
    netins_udp_v2_transport_client_t *client, uint32_t close_timeout_ms)
{
    client->close_timeout_ms = close_timeout_ms;
}

void
netins_udp_v2_transport_client_request_close(
    netins_udp_v2_transport_client_t *client)
{
    pthread_mutex_lock(&g_wait_lock);
    g_close_requested = 1;
    if (client->state != NETINS_UDP_V2_TRANSPORT_ERROR) {
        client->state = NETINS_UDP_V2_TRANSPORT_CLOSING;
    }
    pthread_cond_broadcast(&g_wait_cond);
    pthread_mutex_unlock(&g_wait_lock);
}

void
netins_udp_v2_transport_client_close(
    netins_udp_v2_transport_client_t *client)
{
    if (g_last_client == client) {
        g_last_client = NULL;
    }
    free(client);
    g_close_count++;
}

static void
set_http3_test_options(netins_udp_t *client)
{
    int protocol = NETINS_UDP_APP_HTTP3;
    assert(netins_udp_set_opt(client, NETINS_UDP_OPT_APP_PROTOCOL,
        &protocol, sizeof(protocol)) == NETINS_UDP_OK);

    netins_udp_tls_config_t tls = NETINS_UDP_TLS_CONFIG_INITIALIZER;
    tls.verify_peer = 0;
    strcpy(tls.server_name, "test.example");
    assert(netins_udp_set_opt(client, NETINS_UDP_OPT_TLS,
        &tls, sizeof(tls)) == NETINS_UDP_OK);
}

typedef struct connect_thread_result_s {
    netins_udp_t *client;
    int result;
    int error_code;
} connect_thread_result_t;

static void *
connect_thread_main(void *argument)
{
    connect_thread_result_t *thread_result = argument;
    thread_result->result = netins_udp_connect(thread_result->client,
        "203.0.113.2", 443);
    thread_result->error_code = errno;
    return NULL;
}

int
main(void)
{
    errno = 0;
    assert(netins_udp_new(NETINS_UDP_TYPE_QUIC) == NULL);
    assert(errno == EPERM);
    assert(netins_udp_cleanup() == NETINS_UDP_ERROR);
    assert(errno == EPERM);

    assert(netins_udp_init() == NETINS_UDP_OK);
    errno = 0;
    assert(netins_udp_new(999) == NULL);
    assert(errno == EINVAL);

    netins_udp_t *client = netins_udp_new(NETINS_UDP_TYPE_MPQUIC);
    assert(client != NULL);
    assert(netins_udp_get_state(client) == NETINS_UDP_STATE_NEW);
    assert(netins_udp_cleanup() == NETINS_UDP_ERROR);
    assert(errno == EBUSY);

    size_t option_len = 0;
    assert(netins_udp_get_opt(client, NETINS_UDP_OPT_APP_PROTOCOL,
        NULL, &option_len) == NETINS_UDP_OK);
    assert(option_len == sizeof(int));
    unsigned char short_buffer[1];
    option_len = sizeof(short_buffer);
    assert(netins_udp_get_opt(client, NETINS_UDP_OPT_APP_PROTOCOL,
        short_buffer, &option_len) == NETINS_UDP_ERROR);
    assert(errno == ENOBUFS);
    assert(option_len == sizeof(int));

    netins_udp_interfaces_t interfaces =
        NETINS_UDP_INTERFACES_INITIALIZER;
    interfaces.interface_count = 2;
    strcpy(interfaces.names[0], "rmnet_data0");
    strcpy(interfaces.names[1], "usb0");
    assert(netins_udp_set_opt(client, NETINS_UDP_OPT_INTERFACES,
        &interfaces, sizeof(interfaces)) == NETINS_UDP_OK);
    set_http3_test_options(client);

    uint32_t connect_timeout_ms = 1234;
    assert(netins_udp_set_opt(client,
        NETINS_UDP_OPT_CONNECT_TIMEOUT_MS, &connect_timeout_ms,
        sizeof(connect_timeout_ms)) == NETINS_UDP_OK);
    int scheduler = NETINS_UDP_SCHEDULER_ADAPTIVE;
    assert(netins_udp_set_opt(client, NETINS_UDP_OPT_SCHEDULER,
        &scheduler, sizeof(scheduler)) == NETINS_UDP_OK);
    int congestion_control = NETINS_UDP_CC_BBR2;
    assert(netins_udp_set_opt(client,
        NETINS_UDP_OPT_CONGESTION_CONTROL, &congestion_control,
        sizeof(congestion_control)) == NETINS_UDP_OK);

    assert(netins_udp_connect(client, "192.0.2.1", 8443)
        == NETINS_UDP_OK);
    assert(netins_udp_get_state(client) == NETINS_UDP_STATE_CONNECTED);
    assert(g_create_count == 1);
    assert(g_last_config.enable_multipath == 1);
    assert(g_last_config.interface_count == 2);
    assert(strcmp(g_last_config.interface_names[0], "rmnet_data0") == 0);
    assert(strcmp(g_last_config.interface_names[1], "usb0") == 0);
    assert(strcmp(g_last_config.server_name, "test.example") == 0);
    assert(g_last_config.connect_timeout_ms == 1234);
    assert(g_last_config.scheduler == NETINS_UDP_SCHEDULER_ADAPTIVE);
    assert(g_last_config.congestion_control == NETINS_UDP_CC_BBR2);

    uint32_t close_timeout_ms = 2500;
    assert(netins_udp_set_opt(client, NETINS_UDP_OPT_CLOSE_TIMEOUT_MS,
        &close_timeout_ms, sizeof(close_timeout_ms)) == NETINS_UDP_OK);
    assert(g_last_client->close_timeout_ms == close_timeout_ms);
    assert(netins_udp_set_opt(client, NETINS_UDP_OPT_INTERFACES,
        &interfaces, sizeof(interfaces)) == NETINS_UDP_ERROR);
    assert(errno == EBUSY);

    netins_udp_close(client);
    assert(g_close_count == 1);
    assert(netins_udp_cleanup() == NETINS_UDP_OK);

    assert(netins_udp_init() == NETINS_UDP_OK);
    client = netins_udp_new(NETINS_UDP_TYPE_QUIC);
    assert(client != NULL);
    set_http3_test_options(client);
    pthread_mutex_lock(&g_wait_lock);
    g_block_start = 1;
    g_start_entered = 0;
    g_start_continue = 0;
    pthread_mutex_unlock(&g_wait_lock);
    connect_thread_result_t start_thread_result = {
        .client = client,
        .result = NETINS_UDP_ERROR,
        .error_code = 0,
    };
    pthread_t start_thread;
    assert(pthread_create(&start_thread, NULL, connect_thread_main,
        &start_thread_result) == 0);
    pthread_mutex_lock(&g_wait_lock);
    while (!g_start_entered) {
        pthread_cond_wait(&g_wait_cond, &g_wait_lock);
    }
    pthread_mutex_unlock(&g_wait_lock);
    assert(netins_udp_get_state(client) == NETINS_UDP_STATE_CONNECTING);
    assert(netins_udp_set_opt(client, NETINS_UDP_OPT_INTERFACES,
        &interfaces, sizeof(interfaces)) == NETINS_UDP_ERROR);
    assert(errno == EBUSY);
    pthread_mutex_lock(&g_wait_lock);
    g_start_continue = 1;
    pthread_cond_broadcast(&g_wait_cond);
    pthread_mutex_unlock(&g_wait_lock);
    assert(pthread_join(start_thread, NULL) == 0);
    assert(start_thread_result.result == NETINS_UDP_OK);
    assert(netins_udp_get_state(client) == NETINS_UDP_STATE_CONNECTED);
    pthread_mutex_lock(&g_wait_lock);
    g_block_start = 0;
    pthread_mutex_unlock(&g_wait_lock);
    netins_udp_close(client);
    assert(netins_udp_cleanup() == NETINS_UDP_OK);

    assert(netins_udp_init() == NETINS_UDP_OK);
    client = netins_udp_new(NETINS_UDP_TYPE_QUIC);
    assert(client != NULL);
    set_http3_test_options(client);
    int nonblock = 1;
    assert(netins_udp_set_opt(client, NETINS_UDP_OPT_NONBLOCK,
        &nonblock, sizeof(nonblock)) == NETINS_UDP_OK);
    assert(netins_udp_connect(client, "198.51.100.1", 443)
        == NETINS_UDP_ERROR);
    assert(errno == EINPROGRESS);
    assert(netins_udp_get_state(client) == NETINS_UDP_STATE_CONNECTING);
    g_last_client->state = NETINS_UDP_V2_TRANSPORT_CONNECTED;
    assert(netins_udp_get_state(client) == NETINS_UDP_STATE_CONNECTED);
    netins_udp_close(client);
    assert(netins_udp_cleanup() == NETINS_UDP_OK);

    assert(netins_udp_init() == NETINS_UDP_OK);
    client = netins_udp_new(NETINS_UDP_TYPE_QUIC);
    assert(client != NULL);
    set_http3_test_options(client);
    pthread_mutex_lock(&g_wait_lock);
    g_block_wait = 1;
    g_wait_entered = 0;
    g_close_requested = 0;
    pthread_mutex_unlock(&g_wait_lock);
    connect_thread_result_t thread_result = {
        .client = client,
        .result = NETINS_UDP_OK,
        .error_code = 0,
    };
    pthread_t connect_thread;
    assert(pthread_create(&connect_thread, NULL, connect_thread_main,
        &thread_result) == 0);
    pthread_mutex_lock(&g_wait_lock);
    while (!g_wait_entered) {
        pthread_cond_wait(&g_wait_cond, &g_wait_lock);
    }
    pthread_mutex_unlock(&g_wait_lock);
    netins_udp_close(client);
    assert(pthread_join(connect_thread, NULL) == 0);
    assert(thread_result.result == NETINS_UDP_ERROR);
    assert(thread_result.error_code == ECANCELED);
    pthread_mutex_lock(&g_wait_lock);
    g_block_wait = 0;
    pthread_mutex_unlock(&g_wait_lock);
    assert(netins_udp_cleanup() == NETINS_UDP_OK);

    assert(netins_udp_init() == NETINS_UDP_OK);
    client = netins_udp_new(NETINS_UDP_TYPE_QUIC);
    assert(client != NULL);
    set_http3_test_options(client);
    g_wait_result = -1;
    assert(netins_udp_connect(client, "198.51.100.2", 443)
        == NETINS_UDP_ERROR);
    assert(errno == ETIMEDOUT);
    assert(netins_udp_get_state(client) == NETINS_UDP_STATE_ERROR);
    assert(netins_udp_get_last_error_code(client) == ETIMEDOUT);
    close_timeout_ms = 3000;
    assert(netins_udp_set_opt(client, NETINS_UDP_OPT_CLOSE_TIMEOUT_MS,
        &close_timeout_ms, sizeof(close_timeout_ms)) == NETINS_UDP_OK);
    assert(g_last_client->close_timeout_ms == close_timeout_ms);
    uint32_t accept_timeout_ms = 100;
    assert(netins_udp_set_opt(client, NETINS_UDP_OPT_ACCEPT_TIMEOUT_MS,
        &accept_timeout_ms, sizeof(accept_timeout_ms)) == NETINS_UDP_ERROR);
    assert(errno == EBUSY);
    g_wait_result = 0;
    netins_udp_close(client);
    assert(netins_udp_cleanup() == NETINS_UDP_OK);

    assert(netins_udp_init() == NETINS_UDP_OK);
    client = netins_udp_new(NETINS_UDP_TYPE_QUIC);
    assert(client != NULL);
    set_http3_test_options(client);
    g_start_result = -1;
    assert(netins_udp_connect(client, "198.51.100.3", 443)
        == NETINS_UDP_ERROR);
    assert(errno == EIO);
    assert(netins_udp_get_state(client) == NETINS_UDP_STATE_ERROR);
    assert(netins_udp_get_last_error_code(client) == EIO);
    g_start_result = 0;
    netins_udp_close(client);
    assert(netins_udp_cleanup() == NETINS_UDP_OK);

    assert(netins_udp_init() == NETINS_UDP_OK);
    client = netins_udp_new(NETINS_UDP_TYPE_QUIC);
    assert(client != NULL);
    assert(netins_udp_connect(client, "203.0.113.1", 443)
        == NETINS_UDP_ERROR);
    assert(errno == EOPNOTSUPP);
    assert(netins_udp_get_state(client) == NETINS_UDP_STATE_ERROR);
    assert(netins_udp_get_last_error_code(client) == EOPNOTSUPP);
    netins_udp_close(client);
    assert(netins_udp_cleanup() == NETINS_UDP_OK);

    assert(g_create_count == g_close_count);
    return 0;
}
