#ifndef NETINS_UDP_API_H
#define NETINS_UDP_API_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32) || defined(__CYGWIN__)
#define NETINS_UDP_PUBLIC __declspec(dllexport)
#elif defined(__GNUC__) || defined(__clang__)
#define NETINS_UDP_PUBLIC __attribute__((visibility("default")))
#else
#define NETINS_UDP_PUBLIC
#endif

#define NETINS_UDP_API_VERSION 1U
#define NETINS_UDP_MAX_PATHS 8U
#define NETINS_UDP_MAX_INTERFACES 2U
#define NETINS_UDP_INTERFACE_NAME_LEN 64U
#define NETINS_UDP_ADDRESS_LEN 128U
#define NETINS_UDP_FILE_PATH_LEN 1024U
#define NETINS_UDP_SERVER_NAME_LEN 256U
#define NETINS_UDP_ZLOG_CATEGORY_LEN 64U
#define NETINS_UDP_ERROR_MSG_LEN 256U
#define NETINS_UDP_REMOTE_PATH_LEN 1024U

typedef struct netins_udp_s netins_udp_t;

typedef enum netins_udp_result_e {
    NETINS_UDP_OK = 0,
    NETINS_UDP_ERROR = -1
} netins_udp_result_t;

typedef enum netins_udp_role_e {
    NETINS_UDP_ROLE_NONE = 0,
    NETINS_UDP_ROLE_CLIENT = 1,
    NETINS_UDP_ROLE_SERVER = 2,
    NETINS_UDP_ROLE_ACCEPTED = 3
} netins_udp_role_t;

typedef enum netins_udp_state_e {
    NETINS_UDP_STATE_NEW = 0,
    NETINS_UDP_STATE_CONNECTING = 1,
    NETINS_UDP_STATE_LISTENING = 2,
    NETINS_UDP_STATE_CONNECTED = 3,
    NETINS_UDP_STATE_CLOSING = 4,
    NETINS_UDP_STATE_CLOSED = 5,
    NETINS_UDP_STATE_ERROR = 6
} netins_udp_state_t;

typedef enum netins_udp_close_source_e {
    NETINS_UDP_CLOSE_NONE = 0,
    NETINS_UDP_CLOSE_LOCAL = 1,
    NETINS_UDP_CLOSE_PEER = 2,
    NETINS_UDP_CLOSE_TRANSPORT = 3,
    NETINS_UDP_CLOSE_TIMEOUT = 4,
    NETINS_UDP_CLOSE_INTERNAL = 5
} netins_udp_close_source_t;

typedef enum netins_udp_path_state_e {
    NETINS_UDP_PATH_INIT = 0,
    NETINS_UDP_PATH_VALIDATING = 1,
    NETINS_UDP_PATH_ACTIVE = 2,
    NETINS_UDP_PATH_CLOSING = 3,
    NETINS_UDP_PATH_CLOSED = 4
} netins_udp_path_state_t;

typedef enum netins_udp_app_path_status_e {
    NETINS_UDP_APP_PATH_NONE = 0,
    NETINS_UDP_APP_PATH_STANDBY = 1,
    NETINS_UDP_APP_PATH_AVAILABLE = 2,
    NETINS_UDP_APP_PATH_FROZEN = 3
} netins_udp_app_path_status_t;

typedef enum netins_udp_log_level_e {
    NETINS_UDP_LOG_DEBUG = 0,
    NETINS_UDP_LOG_INFO = 1,
    NETINS_UDP_LOG_WARN = 2,
    NETINS_UDP_LOG_ERROR = 3
} netins_udp_log_level_t;

typedef void (*netins_udp_logger_cb)(void *user_data, int level,
    const char *message);

typedef enum netins_udp_scheduler_e {
    NETINS_UDP_SCHEDULER_DEFAULT = 0,
    NETINS_UDP_SCHEDULER_MINRTT = 1,
    NETINS_UDP_SCHEDULER_BACKUP = 2,
    NETINS_UDP_SCHEDULER_BALANCED = 3,
    NETINS_UDP_SCHEDULER_RAP = 4,
    NETINS_UDP_SCHEDULER_ACT = 5,
    NETINS_UDP_SCHEDULER_BANDWIDTH = 6,
    NETINS_UDP_SCHEDULER_FASTPROBE = 7,
    NETINS_UDP_SCHEDULER_ADAPTIVE = 8,
    NETINS_UDP_SCHEDULER_FASTPROBE5 = 9,
    NETINS_UDP_SCHEDULER_FASTPROBE10 = 10,
    NETINS_UDP_SCHEDULER_FASTPROBE20 = 11,
    NETINS_UDP_SCHEDULER_FASTONLY = 12
} netins_udp_scheduler_t;

typedef enum netins_udp_congestion_control_e {
    NETINS_UDP_CC_DEFAULT = 0,
    NETINS_UDP_CC_CUBIC = 1,
    NETINS_UDP_CC_BBR = 2
} netins_udp_congestion_control_t;

typedef enum netins_udp_opt_e {
    NETINS_UDP_OPT_INTERFACES = 1,
    NETINS_UDP_OPT_SCHEDULER = 2,
    NETINS_UDP_OPT_CONGESTION_CONTROL = 3,
    NETINS_UDP_OPT_TLS = 4,
    NETINS_UDP_OPT_CONNECT_TIMEOUT_MS = 5,
    NETINS_UDP_OPT_ACCEPT_TIMEOUT_MS = 6,
    NETINS_UDP_OPT_CLOSE_TIMEOUT_MS = 7,
    NETINS_UDP_OPT_KERNEL_SNDBUF = 8,
    NETINS_UDP_OPT_KERNEL_RCVBUF = 9,
    NETINS_UDP_OPT_LOGGER = 10,
    NETINS_UDP_OPT_ZLOG = 11
} netins_udp_opt_t;

typedef enum netins_udp_transfer_direction_e {
    NETINS_UDP_TRANSFER_GET = 1,
    NETINS_UDP_TRANSFER_POST = 2
} netins_udp_transfer_direction_t;

typedef struct netins_udp_interfaces_s {
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t interface_count;
    uint32_t reserved32;
    char names[NETINS_UDP_MAX_INTERFACES][NETINS_UDP_INTERFACE_NAME_LEN];
    uint64_t reserved[4];
} netins_udp_interfaces_t;

#define NETINS_UDP_INTERFACES_INITIALIZER \
    { sizeof(netins_udp_interfaces_t), NETINS_UDP_API_VERSION, 0, 0, {{0}}, {0} }

typedef struct netins_udp_tls_config_s {
    uint32_t struct_size;
    uint32_t api_version;
    char server_name[NETINS_UDP_SERVER_NAME_LEN];
    char certificate_file[NETINS_UDP_FILE_PATH_LEN];
    char private_key_file[NETINS_UDP_FILE_PATH_LEN];
    uint64_t reserved[4];
} netins_udp_tls_config_t;

/* Client handles use server_name only; client certificates are unsupported. */
#define NETINS_UDP_TLS_CONFIG_INITIALIZER \
    { sizeof(netins_udp_tls_config_t), NETINS_UDP_API_VERSION, {0}, {0}, {0}, {0} }

typedef struct netins_udp_transfer_s {
    uint32_t struct_size;
    uint32_t api_version;
    int direction;
    uint32_t timeout_ms;
    const char *local_path;
    const char *remote_path;
    uint64_t reserved[4];
} netins_udp_transfer_t;

#define NETINS_UDP_TRANSFER_INITIALIZER \
    { sizeof(netins_udp_transfer_t), NETINS_UDP_API_VERSION, \
      NETINS_UDP_TRANSFER_GET, 0, NULL, NULL, {0} }

typedef struct netins_udp_transfer_result_s {
    uint32_t struct_size;
    uint32_t api_version;
    uint64_t total_bytes;
    uint32_t total_fragments;
    uint32_t completed_fragments;
    /* Mini-compatible transfer interval; see netins_udp_transfer(). */
    uint64_t elapsed_us;
    double throughput_mbps;
    uint64_t reserved[4];
} netins_udp_transfer_result_t;

#define NETINS_UDP_TRANSFER_RESULT_INITIALIZER \
    { sizeof(netins_udp_transfer_result_t), NETINS_UDP_API_VERSION, \
      0, 0, 0, 0, 0.0, {0} }

typedef struct netins_udp_serve_config_s {
    uint32_t struct_size;
    uint32_t api_version;
    const char *root_path;
    uint32_t timeout_ms;
    uint32_t reserved32;
    uint64_t reserved[4];
} netins_udp_serve_config_t;

#define NETINS_UDP_SERVE_CONFIG_INITIALIZER \
    { sizeof(netins_udp_serve_config_t), NETINS_UDP_API_VERSION, \
      NULL, 0, 0, {0} }

typedef struct netins_udp_logger_config_s {
    uint32_t struct_size;
    uint32_t api_version;
    /*
     * The callback runs synchronously and message is valid only for the
     * duration of the call. Do not re-enter this handle from the callback.
     * A NULL callback suppresses mini backend stdout for API-driven calls.
     */
    netins_udp_logger_cb callback;
    void *user_data;
    int minimum_level;
    uint32_t reserved32;
    uint64_t reserved[4];
} netins_udp_logger_config_t;

#define NETINS_UDP_LOGGER_CONFIG_INITIALIZER \
    { sizeof(netins_udp_logger_config_t), NETINS_UDP_API_VERSION, \
      NULL, NULL, NETINS_UDP_LOG_DEBUG, 0, {0} }

typedef struct netins_udp_zlog_config_s {
    uint32_t struct_size;
    uint32_t api_version;
    /*
     * Zlog is disabled unless enabled is nonzero. When enabled, configure
     * this option before connect() or listen(). The numeric levels match
     * netins_udp_log_level_t.
     */
    int enabled;
    int minimum_level;
    char config_file[NETINS_UDP_FILE_PATH_LEN];
    char category[NETINS_UDP_ZLOG_CATEGORY_LEN];
    uint64_t reserved[4];
} netins_udp_zlog_config_t;

#define NETINS_UDP_ZLOG_CONFIG_INITIALIZER \
    { sizeof(netins_udp_zlog_config_t), NETINS_UDP_API_VERSION, \
      0, NETINS_UDP_LOG_DEBUG, {0}, {0}, {0} }

typedef struct netins_udp_path_statis_s {
    uint32_t struct_size;
    uint32_t api_version;
    uint64_t path_id;
    uint32_t path_index;
    int path_state;
    int app_status;
    char interface_name[NETINS_UDP_INTERFACE_NAME_LEN];
    char local_address[NETINS_UDP_ADDRESS_LEN];
    char peer_address[NETINS_UDP_ADDRESS_LEN];
    uint64_t packets_sent;
    uint64_t packets_received;
    uint64_t bytes_sent;
    uint64_t bytes_received;
    uint64_t reinjected_bytes_sent;
    uint64_t reinjected_bytes_received;
    uint64_t effective_bytes_received;
    uint64_t srtt_us;
    /*
     * Reserved for a future XQUIC ABI that exposes per-path values.
     * They are zero in API version 1; use snapshot.bytes_inflight for the
     * current connection-level inflight value.
     */
    uint64_t cwnd_bytes;
    uint64_t bytes_inflight;
    uint64_t reserved[4];
} netins_udp_path_statis_t;

#define NETINS_UDP_PATH_STATIS_INITIALIZER \
    { sizeof(netins_udp_path_statis_t), NETINS_UDP_API_VERSION, UINT64_MAX, \
      0, 0, 0, {0}, {0}, {0}, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, {0} }

typedef struct netins_udp_close_reason_s {
    uint32_t struct_size;
    uint32_t api_version;
    int source;
    int system_error;
    int transport_error;
    uint32_t reserved32;
    char message[NETINS_UDP_ERROR_MSG_LEN];
    uint64_t reserved[4];
} netins_udp_close_reason_t;

#define NETINS_UDP_CLOSE_REASON_INITIALIZER \
    { sizeof(netins_udp_close_reason_t), NETINS_UDP_API_VERSION, \
      NETINS_UDP_CLOSE_NONE, 0, 0, 0, {0}, {0} }

typedef struct netins_udp_snapshot_s {
    uint32_t struct_size;
    uint32_t api_version;
    uint64_t snapshot_time_us;
    int role;
    int state;
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
    netins_udp_path_statis_t paths[NETINS_UDP_MAX_PATHS];
    netins_udp_close_reason_t close_reason;
    uint64_t reserved[8];
} netins_udp_snapshot_t;

#define NETINS_UDP_SNAPSHOT_INITIALIZER \
    { sizeof(netins_udp_snapshot_t), NETINS_UDP_API_VERSION, \
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, \
      { NETINS_UDP_PATH_STATIS_INITIALIZER }, \
      NETINS_UDP_CLOSE_REASON_INITIALIZER, {0} }

NETINS_UDP_PUBLIC uint32_t netins_udp_get_api_version(void);

/*
 * init()/cleanup() are process-wide and reference counted. cleanup() fails
 * with EBUSY while handles from the final reference still exist.
 */
NETINS_UDP_PUBLIC int netins_udp_init(void);
NETINS_UDP_PUBLIC int netins_udp_cleanup(void);

/*
 * new() creates a client or server handle. The caller owns every returned
 * handle and releases it exactly once with close(). Options are copied and
 * may only be changed while the handle is in NEW state, except LOGGER,
 * which may be replaced on a live handle.
 */
NETINS_UDP_PUBLIC netins_udp_t *netins_udp_new(int role);
NETINS_UDP_PUBLIC int netins_udp_set_opt(netins_udp_t *handle,
    int option, const void *value, size_t value_len);

/*
 * connect(), listen(), accept(), and shutdown() are blocking. Their timeout
 * limits come from the corresponding uint32_t options. A zero accept timeout
 * waits indefinitely. A server must configure TLS certificate/key before
 * listen(). host is currently a numeric IPv4 address; TLS server_name is
 * configured separately. An accepted handle keeps the shared listener
 * backend alive, so closing the server handle does not invalidate accepted
 * connections.
 */
NETINS_UDP_PUBLIC int netins_udp_connect(netins_udp_t *client,
    const char *host, uint16_t port);
NETINS_UDP_PUBLIC int netins_udp_listen(netins_udp_t *server,
    const char *host, uint16_t port);
NETINS_UDP_PUBLIC netins_udp_t *netins_udp_accept(netins_udp_t *server);

/*
 * transfer() runs one complete GET or POST task on a connected client.
 * The 4 MiB fragments, POST concurrency 4, GET concurrency 8 and request
 * replenishment policy are fixed internally. A timeout of zero waits
 * indefinitely. remote_path must be an absolute URL path such as "/a.bin".
 *
 * serve() configures an accepted connection's remote file namespace and
 * drives its shared event loop until the peer closes or timeout_ms expires.
 * Remote paths are confined to root_path; ".." and symbolic-link traversal
 * are rejected.
 */
NETINS_UDP_PUBLIC int netins_udp_transfer(netins_udp_t *client,
    const netins_udp_transfer_t *task,
    netins_udp_transfer_result_t *result);
NETINS_UDP_PUBLIC int netins_udp_serve(netins_udp_t *connection,
    const netins_udp_serve_config_t *config);

NETINS_UDP_PUBLIC int netins_udp_get_state(netins_udp_t *handle);
NETINS_UDP_PUBLIC int netins_udp_is_multipath(netins_udp_t *handle);
NETINS_UDP_PUBLIC int netins_udp_get_path_count(netins_udp_t *handle,
    uint32_t *out_count);
NETINS_UDP_PUBLIC int netins_udp_get_path_statis(netins_udp_t *handle,
    size_t path_index, netins_udp_path_statis_t *out);
NETINS_UDP_PUBLIC int netins_udp_get_snapshot(netins_udp_t *handle,
    netins_udp_snapshot_t *out);
NETINS_UDP_PUBLIC int netins_udp_get_close_reason(netins_udp_t *handle,
    netins_udp_close_reason_t *out);
NETINS_UDP_PUBLIC int netins_udp_shutdown(netins_udp_t *handle);
NETINS_UDP_PUBLIC void netins_udp_close(netins_udp_t *handle);

/*
 * Functions returning int use 0 for success and -1 for failure unless they
 * return an enum state. Failure sets errno and, for a valid handle, stores a
 * connection-level error code/message retrievable below. Handles are not
 * safe for concurrent calls. Callers must serialize all operations on a
 * listener and every accepted handle that shares its server backend.
 */
NETINS_UDP_PUBLIC int netins_udp_get_last_error_code(
    netins_udp_t *handle);
NETINS_UDP_PUBLIC int netins_udp_get_last_error_msg(
    netins_udp_t *handle, char *buffer, size_t buffer_len);

#ifdef __cplusplus
}
#endif

#endif
