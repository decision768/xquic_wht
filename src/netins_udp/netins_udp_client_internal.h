#ifndef NETINS_UDP_CLIENT_INTERNAL_H
#define NETINS_UDP_CLIENT_INTERNAL_H

#include <stdint.h>

#include <xquic/netins_udp.h>
#include <xquic/xquic.h>

#define NETINS_UDP_CLIENT_H3_MAX_HEADERS       16U
#define NETINS_UDP_CLIENT_H3_HEADER_NAME_LEN   64U
#define NETINS_UDP_CLIENT_H3_HEADER_VALUE_LEN  256U

typedef struct netins_udp_client_h3_header_s {
    char name[NETINS_UDP_CLIENT_H3_HEADER_NAME_LEN];
    char value[NETINS_UDP_CLIENT_H3_HEADER_VALUE_LEN];
} netins_udp_client_h3_header_t;

typedef struct netins_udp_client_h3_config_s {
    char method[NETINS_UDP_METHOD_LEN];
    char path[NETINS_UDP_REQUEST_PATH_LEN];
    char authority[NETINS_UDP_SERVER_NAME_LEN];
    char content_type[NETINS_UDP_CONTENT_TYPE_LEN];
    uint64_t content_length;
    int content_length_unknown;
    uint32_t header_count;
    netins_udp_client_h3_header_t
        headers[NETINS_UDP_CLIENT_H3_MAX_HEADERS];
} netins_udp_client_h3_config_t;

typedef struct netins_udp_client_h3_info_s {
    uint64_t stream_id;
    int response_status;
    int request_headers_ready;
    int response_headers_ready;
    int send_finished;
    int receive_finished;
    int stream_error;
    uint64_t bytes_accepted;
    uint64_t bytes_delivered;
    char request_authority[NETINS_UDP_SERVER_NAME_LEN];
    int response_content_length_known;
    uint64_t response_content_length;
    char response_content_type[NETINS_UDP_CONTENT_TYPE_LEN];
    uint32_t peer_header_count;
    netins_udp_client_h3_header_t
        peer_headers[NETINS_UDP_CLIENT_H3_MAX_HEADERS];
} netins_udp_client_h3_info_t;

/*
 * Private controls used by the V2 lifecycle adapter.  The public V1 client
 * continues to use the same defaults and blocking connect behavior.
 */
int netins_udp_client_configure_transport(netins_udp_client_t *client,
    const xqc_cong_ctrl_callback_t *congestion_control,
    const xqc_scheduler_callback_t *scheduler,
    uint32_t idle_timeout_ms, uint32_t close_timeout_ms);
void netins_udp_client_set_close_timeout(netins_udp_client_t *client,
    uint32_t close_timeout_ms);
int netins_udp_client_start_async(netins_udp_client_t *client);
void netins_udp_client_request_close(netins_udp_client_t *client);
void netins_udp_client_request_shutdown(netins_udp_client_t *client,
    uint64_t application_error);
int netins_udp_client_h3_request_open(netins_udp_client_t *client,
    const netins_udp_client_h3_config_t *config,
    netins_udp_stream_t **out_stream);
int netins_udp_client_h3_request_get_info(netins_udp_stream_t *stream,
    netins_udp_client_h3_info_t *out);
int64_t netins_udp_client_h3_request_recv(netins_udp_stream_t *stream,
    void *buffer, size_t capacity, int *fin, int peek,
    uint32_t timeout_ms);
void netins_udp_client_h3_request_close_async(
    netins_udp_stream_t *stream);
void netins_udp_client_h3_request_abandon(
    netins_udp_stream_t *stream);

#endif /* NETINS_UDP_CLIENT_INTERNAL_H */
