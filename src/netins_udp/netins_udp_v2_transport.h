#ifndef NETINS_UDP_V2_TRANSPORT_H
#define NETINS_UDP_V2_TRANSPORT_H

#include <stddef.h>
#include <stdint.h>

#define NETINS_UDP_V2_TRANSPORT_MAX_PATHS          8U
#define NETINS_UDP_V2_TRANSPORT_HOST_LEN           128U
#define NETINS_UDP_V2_TRANSPORT_SERVER_NAME_LEN    256U
#define NETINS_UDP_V2_TRANSPORT_INTERFACE_NAME_LEN 32U
#define NETINS_UDP_V2_TRANSPORT_ERROR_MSG_LEN      256U

typedef struct netins_udp_client_s netins_udp_v2_transport_client_t;

typedef enum netins_udp_v2_transport_state_e {
    NETINS_UDP_V2_TRANSPORT_NEW = 0,
    NETINS_UDP_V2_TRANSPORT_CONNECTING = 1,
    NETINS_UDP_V2_TRANSPORT_CONNECTED = 2,
    NETINS_UDP_V2_TRANSPORT_CLOSING = 3,
    NETINS_UDP_V2_TRANSPORT_CLOSED = 4,
    NETINS_UDP_V2_TRANSPORT_ERROR = 5
} netins_udp_v2_transport_state_t;

/*
 * Private, version-independent configuration passed from the V2 object to the
 * existing standalone client.  This avoids including the V1 and V2 public
 * headers in the same translation unit.
 */
typedef struct netins_udp_v2_transport_config_s {
    char host[NETINS_UDP_V2_TRANSPORT_HOST_LEN];
    uint16_t port;
    char server_name[NETINS_UDP_V2_TRANSPORT_SERVER_NAME_LEN];
    uint32_t interface_count;
    char interface_names[NETINS_UDP_V2_TRANSPORT_MAX_PATHS]
        [NETINS_UDP_V2_TRANSPORT_INTERFACE_NAME_LEN];
    int enable_multipath;
    uint32_t connect_timeout_ms;
    uint32_t idle_timeout_ms;
    uint32_t close_timeout_ms;
    int kernel_sndbuf;
    int kernel_rcvbuf;
    uint32_t initial_cwnd_packets;
    int verify_peer;
    int scheduler;
    int congestion_control;
} netins_udp_v2_transport_config_t;

int netins_udp_v2_transport_client_create(
    const netins_udp_v2_transport_config_t *config,
    netins_udp_v2_transport_client_t **out_client);
int netins_udp_v2_transport_client_start(
    netins_udp_v2_transport_client_t *client);
int netins_udp_v2_transport_client_wait_connected(
    netins_udp_v2_transport_client_t *client, uint32_t timeout_ms);
int netins_udp_v2_transport_client_get_state(
    netins_udp_v2_transport_client_t *client);
int netins_udp_v2_transport_client_get_last_error_code(
    netins_udp_v2_transport_client_t *client);
int netins_udp_v2_transport_client_get_last_error_msg(
    netins_udp_v2_transport_client_t *client,
    char *buffer, size_t buffer_len);
void netins_udp_v2_transport_client_set_close_timeout(
    netins_udp_v2_transport_client_t *client, uint32_t close_timeout_ms);
void netins_udp_v2_transport_client_request_close(
    netins_udp_v2_transport_client_t *client);
void netins_udp_v2_transport_client_close(
    netins_udp_v2_transport_client_t *client);

#endif /* NETINS_UDP_V2_TRANSPORT_H */
