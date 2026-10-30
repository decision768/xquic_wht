#ifndef XQC_MINI_BACKEND_H
#define XQC_MINI_BACKEND_H

#include <stddef.h>
#include <stdint.h>

#define MINI_BACKEND_MAX_INTERFACES 8
#define MINI_BACKEND_INTERFACE_NAME_LEN 64
#define MINI_BACKEND_SCHEDULER_NAME_LEN 32
#define MINI_BACKEND_HOST_NAME_LEN 256
#define MINI_BACKEND_FILE_PATH_LEN 1024
#define MINI_BACKEND_MAX_PATHS 8
#define MINI_BACKEND_ADDRESS_LEN 128
#define MINI_BACKEND_STREAM_LANE_COUNT 4U

typedef struct mini_backend_client_s mini_backend_client_t;
typedef struct mini_backend_server_s mini_backend_server_t;
typedef struct xqc_mini_svr_user_conn_s mini_backend_server_connection_t;
typedef struct mini_backend_stream_s mini_backend_stream_t;
typedef void (*mini_backend_logger_fn)(void *user_data, int level,
    const char *message);
typedef void (*mini_backend_stream_data_fn)(void *user_data,
    const void *data, size_t data_len, uint64_t offset);

typedef enum mini_backend_cc_e {
    MINI_BACKEND_CC_BBR = 0,
    MINI_BACKEND_CC_CUBIC = 1
} mini_backend_cc_t;

typedef enum mini_backend_transfer_direction_e {
    MINI_BACKEND_TRANSFER_GET = 1,
    MINI_BACKEND_TRANSFER_POST = 2
} mini_backend_transfer_direction_t;

typedef struct mini_backend_transfer_result_s {
    uint64_t total_bytes;
    uint32_t total_fragments;
    uint32_t completed_fragments;
    uint64_t elapsed_us;
} mini_backend_transfer_result_t;

typedef struct mini_backend_path_snapshot_s {
    uint64_t path_id;
    uint32_t path_index;
    int path_state;
    int app_status;
    char interface_name[MINI_BACKEND_INTERFACE_NAME_LEN];
    char local_address[MINI_BACKEND_ADDRESS_LEN];
    char peer_address[MINI_BACKEND_ADDRESS_LEN];
    uint64_t packets_sent;
    uint64_t packets_received;
    uint64_t bytes_sent;
    uint64_t bytes_received;
    uint64_t reinjected_bytes_sent;
    uint64_t reinjected_bytes_received;
    uint64_t effective_bytes_received;
    uint64_t srtt_us;
} mini_backend_path_snapshot_t;

typedef struct mini_backend_snapshot_s {
    uint64_t snapshot_time_us;
    int handshake_finished;
    int multipath_mode;
    int multipath_state;
    uint32_t path_count;
    uint32_t active_path_count;
    uint32_t packets_sent;
    uint32_t packets_received;
    uint32_t packets_lost;
    uint32_t spurious_packets_lost;
    uint64_t srtt_us;
    uint64_t min_rtt_us;
    uint64_t bytes_inflight;
    uint64_t total_app_bytes;
    uint64_t standby_path_app_bytes;
    mini_backend_path_snapshot_t paths[MINI_BACKEND_MAX_PATHS];
} mini_backend_snapshot_t;

typedef struct mini_backend_config_s {
    int load_config_file;
    int api_mode;

    int has_interfaces;
    int interface_count;
    char interfaces[MINI_BACKEND_MAX_INTERFACES]
        [MINI_BACKEND_INTERFACE_NAME_LEN];

    int has_scheduler;
    char scheduler[MINI_BACKEND_SCHEDULER_NAME_LEN];

    int has_congestion_control;
    int congestion_control;

    int has_kernel_sndbuf;
    int kernel_sndbuf;
    int has_kernel_rcvbuf;
    int kernel_rcvbuf;

    int has_tls;
    char tls_server_name[MINI_BACKEND_HOST_NAME_LEN];
    char certificate_file[MINI_BACKEND_FILE_PATH_LEN];
    char private_key_file[MINI_BACKEND_FILE_PATH_LEN];

    mini_backend_logger_fn logger;
    void *logger_user_data;
    int logger_minimum_level;

    int has_zlog;
    int zlog_enabled;
    int zlog_minimum_level;
    char zlog_config_file[MINI_BACKEND_FILE_PATH_LEN];
    char zlog_category[64];

    int stream_mode;
} mini_backend_config_t;

typedef struct mini_backend_stream_config_s {
    uint32_t direction;
    uint32_t flags;
    size_t send_queue_size;
    size_t receive_queue_size;
    mini_backend_stream_data_fn on_data;
    void *user_data;
    mini_backend_logger_fn logger;
    void *logger_user_data;
    int logger_minimum_level;
    /* The server owns one shared XQUIC/libevent driver for all connections. */
    int external_event_driver;
    /* Complete lane headers buffered before server stream_accept began. */
    uint32_t preaccept_lane_count;
} mini_backend_stream_config_t;

typedef struct mini_backend_stream_result_s {
    uint64_t accepted_bytes;
    uint64_t peer_received_bytes;
    uint64_t elapsed_us;
} mini_backend_stream_result_t;

typedef enum mini_backend_stream_state_e {
    MINI_BACKEND_STREAM_STATE_ACTIVE = 1,
    MINI_BACKEND_STREAM_STATE_FINISHING = 2,
    MINI_BACKEND_STREAM_STATE_COMPLETED = 3,
    MINI_BACKEND_STREAM_STATE_RESET = 4,
    MINI_BACKEND_STREAM_STATE_ERROR = 5
} mini_backend_stream_state_t;

typedef struct mini_backend_stream_snapshot_s {
    uint64_t snapshot_time_us;
    int endpoint;
    int state;
    uint32_t lane_count;
    uint32_t flags;
    uint32_t preaccept_lane_count;
    uint64_t accepted_bytes;
    uint64_t peer_received_bytes;
    uint64_t send_queue_bytes;
    uint64_t send_queue_capacity;
    uint64_t send_queue_high_water;
    uint64_t reorder_bytes;
    uint64_t reorder_capacity;
    uint64_t reorder_high_water;
    uint64_t records_enqueued;
    uint64_t records_submitted;
    uint64_t records_received;
    uint64_t records_delivered;
    uint64_t lane_payload_sent[MINI_BACKEND_STREAM_LANE_COUNT];
    uint64_t lane_payload_received[MINI_BACKEND_STREAM_LANE_COUNT];
    int last_error;
    uint64_t app_error_code;
} mini_backend_stream_snapshot_t;

void mini_backend_config_init(mini_backend_config_t *config);
void mini_backend_log_scope_enter(const mini_backend_config_t *config);
void mini_backend_log_scope_leave(void);

int mini_backend_client_create(const mini_backend_config_t *config,
    mini_backend_client_t **out_client);
int mini_backend_client_connect(mini_backend_client_t *client,
    const char *host, uint16_t port, uint32_t timeout_ms);
int mini_backend_client_run(mini_backend_client_t *client);
int mini_backend_client_poll(mini_backend_client_t *client);
int mini_backend_client_transfer(mini_backend_client_t *client,
    int direction, const char *local_path, const char *remote_path,
    uint32_t timeout_ms, mini_backend_transfer_result_t *result);
int mini_backend_client_shutdown(mini_backend_client_t *client,
    uint32_t timeout_ms);
void mini_backend_client_destroy(mini_backend_client_t *client);
int mini_backend_client_is_connected(mini_backend_client_t *client);
int mini_backend_client_is_closed(mini_backend_client_t *client);
int mini_backend_client_last_error(mini_backend_client_t *client);
int mini_backend_client_active_path_count(mini_backend_client_t *client);
int mini_backend_client_get_snapshot(mini_backend_client_t *client,
    mini_backend_snapshot_t *snapshot);
int mini_backend_client_stream_open(mini_backend_client_t *client,
    const mini_backend_stream_config_t *config,
    mini_backend_stream_t **out_stream);

int mini_backend_server_create(const mini_backend_config_t *config,
    mini_backend_server_t **out_server);
int mini_backend_server_listen(mini_backend_server_t *server,
    const char *host, uint16_t port);
int mini_backend_server_run(mini_backend_server_t *server);
int mini_backend_server_poll(mini_backend_server_t *server);
int mini_backend_server_last_error(mini_backend_server_t *server);
void mini_backend_server_set_logger(mini_backend_server_t *server,
    mini_backend_logger_fn logger, void *user_data, int minimum_level);
int mini_backend_server_accept(mini_backend_server_t *server,
    uint32_t timeout_ms, mini_backend_server_connection_t **out_connection);
void mini_backend_server_retain(mini_backend_server_t *server);
void mini_backend_server_shutdown(mini_backend_server_t *server);
void mini_backend_server_release(mini_backend_server_t *server);

int mini_backend_server_connection_shutdown(mini_backend_server_t *server,
    mini_backend_server_connection_t *connection, uint32_t timeout_ms);
int mini_backend_server_connection_serve(mini_backend_server_t *server,
    mini_backend_server_connection_t *connection, const char *root_path,
    uint32_t timeout_ms);
void mini_backend_server_connection_release(mini_backend_server_t *server,
    mini_backend_server_connection_t *connection);
int mini_backend_server_connection_is_connected(
    mini_backend_server_t *server,
    const mini_backend_server_connection_t *connection);
int mini_backend_server_connection_is_closed(
    mini_backend_server_t *server,
    const mini_backend_server_connection_t *connection);
int mini_backend_server_connection_last_error(
    mini_backend_server_t *server,
    const mini_backend_server_connection_t *connection);
int mini_backend_server_connection_get_snapshot(
    mini_backend_server_t *server,
    const mini_backend_server_connection_t *connection,
    mini_backend_snapshot_t *snapshot);
int mini_backend_server_stream_accept(mini_backend_server_t *server,
    mini_backend_server_connection_t *connection,
    const mini_backend_stream_config_t *config,
    mini_backend_stream_t **out_stream);

/* Called only by the Mini server's XQUIC callbacks. */
void mini_backend_server_on_connection_created(void *backend,
    mini_backend_server_connection_t *connection);
void mini_backend_server_on_handshake_finished(void *backend,
    mini_backend_server_connection_t *connection);
void mini_backend_server_on_connection_closed(void *backend,
    mini_backend_server_connection_t *connection, int error_code);
void mini_backend_server_on_listener_error(void *backend, int error_code);
void mini_backend_server_on_stream_connection_closed(void *backend,
    mini_backend_server_connection_t *connection, int error_code);

int mini_backend_stream_write(mini_backend_stream_t *stream,
    const void *data, size_t data_len, uint32_t timeout_ms,
    size_t *accepted);
int mini_backend_stream_flush(mini_backend_stream_t *stream,
    uint32_t timeout_ms);
int mini_backend_stream_finish(mini_backend_stream_t *stream,
    uint32_t timeout_ms, mini_backend_stream_result_t *result);
int mini_backend_stream_reset(mini_backend_stream_t *stream,
    uint64_t error_code);
int mini_backend_stream_get_snapshot(mini_backend_stream_t *stream,
    mini_backend_stream_snapshot_t *snapshot);
void mini_backend_stream_set_logger(mini_backend_stream_t *stream,
    mini_backend_logger_fn logger, void *user_data, int minimum_level);
void mini_backend_stream_destroy(mini_backend_stream_t *stream);

#endif
