#ifndef NETINS_UDP_V2_TRANSPORT_H
#define NETINS_UDP_V2_TRANSPORT_H

#include <stddef.h>
#include <stdint.h>

#define NETINS_UDP_V2_TRANSPORT_MAX_PATHS          8U
#define NETINS_UDP_V2_TRANSPORT_HOST_LEN           128U
#define NETINS_UDP_V2_TRANSPORT_SERVER_NAME_LEN    256U
#define NETINS_UDP_V2_TRANSPORT_INTERFACE_NAME_LEN 32U
#define NETINS_UDP_V2_TRANSPORT_ERROR_MSG_LEN      256U
#define NETINS_UDP_V2_TRANSPORT_METHOD_LEN         16U
#define NETINS_UDP_V2_TRANSPORT_REQUEST_PATH_LEN   1024U
#define NETINS_UDP_V2_TRANSPORT_CONTENT_TYPE_LEN   128U
#define NETINS_UDP_V2_TRANSPORT_MAX_H3_HEADERS     16U
#define NETINS_UDP_V2_TRANSPORT_H3_HEADER_NAME_LEN 64U
#define NETINS_UDP_V2_TRANSPORT_H3_HEADER_VALUE_LEN 256U

typedef struct netins_udp_client_s netins_udp_v2_transport_client_t;
typedef struct netins_udp_stream_s netins_udp_v2_transport_request_t;

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

typedef struct netins_udp_v2_transport_h3_header_s {
    char name[NETINS_UDP_V2_TRANSPORT_H3_HEADER_NAME_LEN];
    char value[NETINS_UDP_V2_TRANSPORT_H3_HEADER_VALUE_LEN];
} netins_udp_v2_transport_h3_header_t;

typedef struct netins_udp_v2_transport_h3_config_s {
    char method[NETINS_UDP_V2_TRANSPORT_METHOD_LEN];
    char path[NETINS_UDP_V2_TRANSPORT_REQUEST_PATH_LEN];
    char authority[NETINS_UDP_V2_TRANSPORT_SERVER_NAME_LEN];
    char content_type[NETINS_UDP_V2_TRANSPORT_CONTENT_TYPE_LEN];
    uint64_t content_length;
    int content_length_unknown;
    uint32_t header_count;
    netins_udp_v2_transport_h3_header_t
        headers[NETINS_UDP_V2_TRANSPORT_MAX_H3_HEADERS];
} netins_udp_v2_transport_h3_config_t;

typedef struct netins_udp_v2_transport_h3_info_s {
    uint64_t stream_id;
    int response_status;
    int request_headers_ready;
    int response_headers_ready;
    int send_finished;
    int receive_finished;
    int stream_error;
    uint64_t bytes_accepted;
    uint64_t bytes_delivered;
    char request_authority[
        NETINS_UDP_V2_TRANSPORT_SERVER_NAME_LEN];
    int response_content_length_known;
    uint64_t response_content_length;
    char response_content_type[
        NETINS_UDP_V2_TRANSPORT_CONTENT_TYPE_LEN];
    uint32_t peer_header_count;
    netins_udp_v2_transport_h3_header_t
        peer_headers[NETINS_UDP_V2_TRANSPORT_MAX_H3_HEADERS];
} netins_udp_v2_transport_h3_info_t;

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
int netins_udp_v2_transport_h3_request_open(
    netins_udp_v2_transport_client_t *client,
    const netins_udp_v2_transport_h3_config_t *config,
    netins_udp_v2_transport_request_t **out_request);
int64_t netins_udp_v2_transport_h3_request_send(
    netins_udp_v2_transport_request_t *request,
    const void *data, size_t length, int fin, uint32_t timeout_ms,
    int wait_forever);
int64_t netins_udp_v2_transport_h3_request_recv(
    netins_udp_v2_transport_request_t *request,
    void *data, size_t length, int *fin, int peek,
    uint32_t timeout_ms, int wait_forever);
int netins_udp_v2_transport_h3_request_get_info(
    netins_udp_v2_transport_request_t *request,
    netins_udp_v2_transport_h3_info_t *out);
void netins_udp_v2_transport_h3_request_close_async(
    netins_udp_v2_transport_request_t *request);
void netins_udp_v2_transport_h3_request_close(
    netins_udp_v2_transport_request_t *request,
    uint32_t timeout_ms);

#endif /* NETINS_UDP_V2_TRANSPORT_H */
