#ifndef NETINS_UDP_V2_TRANSPORT_H
#define NETINS_UDP_V2_TRANSPORT_H

#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

#define NETINS_UDP_V2_TRANSPORT_MAX_PATHS          8U
#define NETINS_UDP_V2_TRANSPORT_HOST_LEN           128U
#define NETINS_UDP_V2_TRANSPORT_SERVER_NAME_LEN    256U
#define NETINS_UDP_V2_TRANSPORT_INTERFACE_NAME_LEN 32U
#define NETINS_UDP_V2_TRANSPORT_ERROR_MSG_LEN      256U
#define NETINS_UDP_V2_TRANSPORT_FILE_PATH_LEN      512U
#define NETINS_UDP_V2_TRANSPORT_METHOD_LEN         16U
#define NETINS_UDP_V2_TRANSPORT_REQUEST_PATH_LEN   1024U
#define NETINS_UDP_V2_TRANSPORT_CONTENT_TYPE_LEN   128U
#define NETINS_UDP_V2_TRANSPORT_MAX_H3_HEADERS     16U
#define NETINS_UDP_V2_TRANSPORT_H3_HEADER_NAME_LEN 64U
#define NETINS_UDP_V2_TRANSPORT_H3_HEADER_VALUE_LEN 256U

#define NETINS_UDP_V2_EVENT_CONNECTED             (1U << 0)
#define NETINS_UDP_V2_EVENT_ACCEPTABLE            (1U << 1)
#define NETINS_UDP_V2_EVENT_READABLE              (1U << 2)
#define NETINS_UDP_V2_EVENT_WRITABLE              (1U << 3)
#define NETINS_UDP_V2_EVENT_PATH_CHANGED          (1U << 4)
#define NETINS_UDP_V2_EVENT_CLOSED                (1U << 5)
#define NETINS_UDP_V2_EVENT_ERROR                 (1U << 6)
#define NETINS_UDP_V2_EVENT_H3_REQUEST_ACCEPTABLE (1U << 8)

typedef void (*netins_udp_v2_transport_notify_cb)(void *user_data,
    uint32_t events);

typedef struct netins_udp_client_s netins_udp_v2_transport_client_t;
typedef struct netins_udp_stream_s netins_udp_v2_transport_request_t;
typedef struct netins_udp_server_s netins_udp_v2_transport_server_t;
typedef struct netins_udp_server_connection_s
    netins_udp_v2_transport_server_connection_t;
typedef struct netins_udp_server_request_s
    netins_udp_v2_transport_server_request_t;

typedef enum netins_udp_v2_transport_state_e {
    NETINS_UDP_V2_TRANSPORT_NEW = 0,
    NETINS_UDP_V2_TRANSPORT_CONNECTING = 1,
    NETINS_UDP_V2_TRANSPORT_LISTENING = 2,
    NETINS_UDP_V2_TRANSPORT_CONNECTED = 3,
    NETINS_UDP_V2_TRANSPORT_CLOSING = 4,
    NETINS_UDP_V2_TRANSPORT_CLOSED = 5,
    NETINS_UDP_V2_TRANSPORT_ERROR = 6
} netins_udp_v2_transport_state_t;

typedef struct netins_udp_v2_transport_path_snapshot_s {
    uint64_t path_id;
    uint32_t path_index;
    int path_state;
    int app_status;
    int cwnd_blocked;
    char interface_name[NETINS_UDP_V2_TRANSPORT_INTERFACE_NAME_LEN];
    char local_address[NETINS_UDP_V2_TRANSPORT_HOST_LEN];
    char peer_address[NETINS_UDP_V2_TRANSPORT_HOST_LEN];
    uint64_t bytes_sent;
    uint64_t bytes_received;
    uint64_t bytes_wire_sent;
    uint64_t bytes_wire_received;
    uint64_t bytes_acked;
    uint64_t bytes_lost;
    uint64_t bytes_inflight;
    uint64_t cwnd_bytes;
    uint64_t srtt_us;
    uint64_t latest_rtt_us;
    uint64_t min_rtt_us;
    uint64_t last_received_time_us;
    uint64_t last_sent_time_us;
    double send_mbps;
    double receive_mbps;
    double acked_mbps;
    double loss_rate;
} netins_udp_v2_transport_path_snapshot_t;

typedef struct netins_udp_v2_transport_snapshot_s {
    uint64_t connection_id;
    int state;
    int handshake_finished;
    int multipath_mode;
    int multipath_active;
    int connection_error;
    uint32_t path_count;
    uint32_t active_path_count;
    uint32_t validating_path_count;
    uint32_t path_table_truncated;
    char alpn[64];
    char local_address[NETINS_UDP_V2_TRANSPORT_HOST_LEN];
    char peer_address[NETINS_UDP_V2_TRANSPORT_HOST_LEN];
    uint64_t snapshot_time_us;
    uint64_t sample_interval_us;
    uint64_t bytes_sent;
    uint64_t bytes_received;
    uint64_t bytes_wire_sent;
    uint64_t bytes_wire_received;
    uint64_t bytes_acked;
    uint64_t bytes_lost;
    uint64_t bytes_inflight;
    double send_mbps;
    double receive_mbps;
    double acked_mbps;
    double loss_rate;
    netins_udp_v2_transport_path_snapshot_t
        paths[NETINS_UDP_V2_TRANSPORT_MAX_PATHS];
} netins_udp_v2_transport_snapshot_t;

/*
 * Private, version-independent configuration passed from the unified object
 * to the compatibility client backend. This keeps the installed header and
 * compatibility structure contract in separate translation units.
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
    netins_udp_v2_transport_notify_cb notify;
    void *notify_user_data;
} netins_udp_v2_transport_config_t;

typedef struct netins_udp_v2_transport_server_config_s {
    char host[NETINS_UDP_V2_TRANSPORT_HOST_LEN];
    uint16_t port;
    char certificate_file[NETINS_UDP_V2_TRANSPORT_FILE_PATH_LEN];
    char private_key_file[NETINS_UDP_V2_TRANSPORT_FILE_PATH_LEN];
    int enable_multipath;
    uint32_t idle_timeout_ms;
    int kernel_sndbuf;
    int kernel_rcvbuf;
    uint32_t initial_cwnd_packets;
    int scheduler;
    int congestion_control;
    netins_udp_v2_transport_notify_cb notify;
    void *notify_user_data;
} netins_udp_v2_transport_server_config_t;

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

typedef struct netins_udp_v2_transport_h3_response_config_s {
    int status;
    char content_type[NETINS_UDP_V2_TRANSPORT_CONTENT_TYPE_LEN];
    uint64_t content_length;
    int content_length_unknown;
    uint32_t header_count;
    netins_udp_v2_transport_h3_header_t
        headers[NETINS_UDP_V2_TRANSPORT_MAX_H3_HEADERS];
} netins_udp_v2_transport_h3_response_config_t;

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
    char request_method[NETINS_UDP_V2_TRANSPORT_METHOD_LEN];
    char request_path[NETINS_UDP_V2_TRANSPORT_REQUEST_PATH_LEN];
    char request_authority[
        NETINS_UDP_V2_TRANSPORT_SERVER_NAME_LEN];
    char request_content_type[
        NETINS_UDP_V2_TRANSPORT_CONTENT_TYPE_LEN];
    int request_content_length_known;
    uint64_t request_content_length;
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
int netins_udp_v2_transport_client_get_snapshot(
    netins_udp_v2_transport_client_t *client,
    netins_udp_v2_transport_snapshot_t *out);
int netins_udp_v2_transport_client_get_events(
    netins_udp_v2_transport_client_t *client, uint32_t *out_events);
void netins_udp_v2_transport_client_set_notify(
    netins_udp_v2_transport_client_t *client,
    netins_udp_v2_transport_notify_cb notify, void *user_data);
void netins_udp_v2_transport_client_set_close_timeout(
    netins_udp_v2_transport_client_t *client, uint32_t close_timeout_ms);
void netins_udp_v2_transport_client_request_close(
    netins_udp_v2_transport_client_t *client);
void netins_udp_v2_transport_client_request_shutdown(
    netins_udp_v2_transport_client_t *client, uint64_t application_error);
void netins_udp_v2_transport_client_close(
    netins_udp_v2_transport_client_t *client);

int netins_udp_v2_transport_server_create(
    const netins_udp_v2_transport_server_config_t *config,
    netins_udp_v2_transport_server_t **out_server);
int netins_udp_v2_transport_server_get_state(
    netins_udp_v2_transport_server_t *server);
int netins_udp_v2_transport_server_get_last_error_code(
    netins_udp_v2_transport_server_t *server);
int netins_udp_v2_transport_server_get_last_error_msg(
    netins_udp_v2_transport_server_t *server,
    char *buffer, size_t buffer_len);
int netins_udp_v2_transport_server_get_events(
    netins_udp_v2_transport_server_t *server, uint32_t *out_events);
void netins_udp_v2_transport_server_set_notify(
    netins_udp_v2_transport_server_t *server,
    netins_udp_v2_transport_notify_cb notify, void *user_data);
int netins_udp_v2_transport_server_accept(
    netins_udp_v2_transport_server_t *server,
    uint32_t timeout_ms, int wait_forever,
    netins_udp_v2_transport_server_connection_t **out_connection);
void netins_udp_v2_transport_server_begin_close(
    netins_udp_v2_transport_server_t *server);
void netins_udp_v2_transport_server_close(
    netins_udp_v2_transport_server_t *server);

int netins_udp_v2_transport_server_connection_get_state(
    netins_udp_v2_transport_server_connection_t *connection);
int netins_udp_v2_transport_server_connection_get_last_error_code(
    netins_udp_v2_transport_server_connection_t *connection);
int netins_udp_v2_transport_server_connection_get_last_error_msg(
    netins_udp_v2_transport_server_connection_t *connection,
    char *buffer, size_t buffer_len);
int netins_udp_v2_transport_server_connection_get_peer_address(
    netins_udp_v2_transport_server_connection_t *connection,
    struct sockaddr_storage *address, socklen_t *address_len);
int netins_udp_v2_transport_server_connection_get_snapshot(
    netins_udp_v2_transport_server_connection_t *connection,
    netins_udp_v2_transport_snapshot_t *out);
int netins_udp_v2_transport_server_connection_get_events(
    netins_udp_v2_transport_server_connection_t *connection,
    uint32_t *out_events);
void netins_udp_v2_transport_server_connection_set_notify(
    netins_udp_v2_transport_server_connection_t *connection,
    netins_udp_v2_transport_notify_cb notify, void *user_data);
void netins_udp_v2_transport_server_connection_request_shutdown(
    netins_udp_v2_transport_server_connection_t *connection,
    uint64_t application_error);
void netins_udp_v2_transport_server_connection_close(
    netins_udp_v2_transport_server_connection_t *connection,
    uint32_t timeout_ms);
int netins_udp_v2_transport_server_request_accept(
    netins_udp_v2_transport_server_connection_t *connection,
    uint32_t timeout_ms, int wait_forever,
    netins_udp_v2_transport_server_request_t **out_request);
int netins_udp_v2_transport_server_request_set_response(
    netins_udp_v2_transport_server_request_t *request,
    const netins_udp_v2_transport_h3_response_config_t *config);
int64_t netins_udp_v2_transport_server_request_send(
    netins_udp_v2_transport_server_request_t *request,
    const void *data, size_t length, int fin, uint32_t timeout_ms,
    int wait_forever);
int64_t netins_udp_v2_transport_server_request_recv(
    netins_udp_v2_transport_server_request_t *request,
    void *data, size_t length, int *fin, int peek,
    uint32_t timeout_ms, int wait_forever);
int netins_udp_v2_transport_server_request_get_info(
    netins_udp_v2_transport_server_request_t *request,
    netins_udp_v2_transport_h3_info_t *out);
int netins_udp_v2_transport_server_request_get_events(
    netins_udp_v2_transport_server_request_t *request,
    uint32_t *out_events);
void netins_udp_v2_transport_server_request_set_notify(
    netins_udp_v2_transport_server_request_t *request,
    netins_udp_v2_transport_notify_cb notify, void *user_data);
void netins_udp_v2_transport_server_request_close_async(
    netins_udp_v2_transport_server_request_t *request);
void netins_udp_v2_transport_server_request_close(
    netins_udp_v2_transport_server_request_t *request,
    uint32_t timeout_ms);
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
int netins_udp_v2_transport_h3_request_get_events(
    netins_udp_v2_transport_request_t *request, uint32_t *out_events);
void netins_udp_v2_transport_h3_request_set_notify(
    netins_udp_v2_transport_request_t *request,
    netins_udp_v2_transport_notify_cb notify, void *user_data);
void netins_udp_v2_transport_h3_request_close_async(
    netins_udp_v2_transport_request_t *request);
void netins_udp_v2_transport_h3_request_close(
    netins_udp_v2_transport_request_t *request,
    uint32_t timeout_ms);

struct netins_udp_observer_s;
int netins_udp_v2_transport_snapshot_from_observer(
    struct netins_udp_observer_s *observer,
    netins_udp_v2_transport_snapshot_t *out);

#endif /* NETINS_UDP_V2_TRANSPORT_H */
