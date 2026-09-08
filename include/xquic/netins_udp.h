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
#define NETINS_UDP_API_RELEASE_VERSION "6.5"
#define NETINS_UDP_MAX_PATHS 8U
#define NETINS_UDP_MAX_INTERFACES 8U
#define NETINS_UDP_INTERFACE_NAME_LEN 64U
#define NETINS_UDP_ADDRESS_LEN 128U
#define NETINS_UDP_ATTEMPT_ID_LEN 128U
#define NETINS_UDP_FILE_PATH_LEN 1024U
#define NETINS_UDP_SERVER_NAME_LEN 256U
#define NETINS_UDP_ZLOG_CATEGORY_LEN 64U
#define NETINS_UDP_ERROR_MSG_LEN 256U
#define NETINS_UDP_REMOTE_PATH_LEN 1024U
#define NETINS_UDP_TLS_PEM_MAX_LEN (1024U * 1024U)
#define NETINS_UDP_STREAM_LANE_COUNT 4U

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
    NETINS_UDP_OPT_ZLOG = 11,
    NETINS_UDP_OPT_TLS_MEMORY = 12,
    /*
     * int boolean, configured before connect()/listen().  It enables the
     * h3-ext ALPN required by the data-stream API.  Ordinary file clients
     * keep using the original h3 ALPN unless this option is explicitly set.
     */
    NETINS_UDP_OPT_STREAM_MODE = 13,
    NETINS_UDP_OPT_TELEMETRY = 14
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

typedef struct netins_udp_tls_memory_config_s {
    uint32_t struct_size;
    uint32_t api_version;
    const void *certificate_pem;
    size_t certificate_pem_len;
    const void *private_key_pem;
    size_t private_key_pem_len;
    uint64_t reserved[4];
} netins_udp_tls_memory_config_t;

/*
 * Server-only PEM input. set_opt() copies both buffers before returning, so
 * the caller may release or reuse them after a successful call.
 */
#define NETINS_UDP_TLS_MEMORY_CONFIG_INITIALIZER \
    { sizeof(netins_udp_tls_memory_config_t), NETINS_UDP_API_VERSION, \
      NULL, 0, NULL, 0, {0} }

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

typedef enum netins_udp_telemetry_role_e {
    NETINS_UDP_TELEMETRY_ROLE_CLIENT = 1,
    NETINS_UDP_TELEMETRY_ROLE_SERVER = 2
} netins_udp_telemetry_role_t;

typedef enum netins_udp_telemetry_sample_kind_e {
    NETINS_UDP_TELEMETRY_START = 1,
    NETINS_UDP_TELEMETRY_PERIODIC = 2,
    NETINS_UDP_TELEMETRY_END = 3
} netins_udp_telemetry_sample_kind_t;

typedef struct netins_udp_path_telemetry_s {
    uint64_t path_id;
    uint32_t path_index;
    int path_state;
    int app_status;
    char interface_name[NETINS_UDP_INTERFACE_NAME_LEN];
    char local_address[NETINS_UDP_ADDRESS_LEN];
    uint64_t cwnd_bytes;
    uint64_t effective_cwnd_bytes;
    uint64_t inflight_packets;
    uint64_t inflight_bytes;
    uint64_t srtt_us;
    uint64_t sent_bytes;
} netins_udp_path_telemetry_t;

typedef struct netins_udp_telemetry_sample_s {
    uint32_t struct_size;
    uint32_t api_version;
    char attempt_id[NETINS_UDP_ATTEMPT_ID_LEN];
    int role;
    int direction;
    int sample_kind;
    uint32_t sample_seq;
    uint64_t scheduled_mono_us;
    uint64_t actual_mono_us;
    uint64_t realtime_us;
    uint64_t lateness_us;
    uint32_t missed_intervals;
    uint32_t path_count;
    uint64_t connection_credit_bytes;
    uint64_t active_stream_credit_bytes;
    uint64_t effective_swnd_bytes;
    netins_udp_path_telemetry_t paths[NETINS_UDP_MAX_PATHS];
    uint64_t reserved[4];
} netins_udp_telemetry_sample_t;

/* Called synchronously on the Mini event-loop thread. Return nonzero to stop
 * telemetry for this connection. The callback must not call this handle. */
typedef int (*netins_udp_telemetry_cb)(void *user_data,
    const netins_udp_telemetry_sample_t *sample);

typedef struct netins_udp_telemetry_config_s {
    uint32_t struct_size;
    uint32_t api_version;
    int enabled;
    uint32_t interval_ms;
    char attempt_id[NETINS_UDP_ATTEMPT_ID_LEN];
    netins_udp_telemetry_cb callback;
    void *user_data;
    uint64_t reserved[4];
} netins_udp_telemetry_config_t;

#define NETINS_UDP_TELEMETRY_CONFIG_INITIALIZER \
    { sizeof(netins_udp_telemetry_config_t), NETINS_UDP_API_VERSION, \
      0, 250, {0}, NULL, NULL, {0} }

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

typedef struct netins_udp_stream_s netins_udp_stream_t;

typedef enum netins_udp_stream_flag_e {
    NETINS_UDP_STREAM_FLAG_NONE = 0,
    NETINS_UDP_STREAM_FLAG_NONBLOCK = 1U << 0
} netins_udp_stream_flag_t;

/* Direction of one half-duplex logical data stream.  A zero configuration
 * value is accepted as a legacy alias for CLIENT_TO_SERVER, but is never
 * transmitted on the wire. */
typedef enum netins_udp_stream_direction_e {
    NETINS_UDP_STREAM_DIRECTION_CLIENT_TO_SERVER = 1,
    NETINS_UDP_STREAM_DIRECTION_SERVER_TO_CLIENT = 2
} netins_udp_stream_direction_t;

typedef enum netins_udp_stream_endpoint_e {
    NETINS_UDP_STREAM_ENDPOINT_OPEN = 1,
    NETINS_UDP_STREAM_ENDPOINT_ACCEPT = 2
} netins_udp_stream_endpoint_t;

typedef enum netins_udp_stream_state_e {
    NETINS_UDP_STREAM_STATE_ACTIVE = 1,
    NETINS_UDP_STREAM_STATE_FINISHING = 2,
    NETINS_UDP_STREAM_STATE_COMPLETED = 3,
    NETINS_UDP_STREAM_STATE_RESET = 4,
    NETINS_UDP_STREAM_STATE_ERROR = 5
} netins_udp_stream_state_t;

typedef void (*netins_udp_stream_data_cb)(
    netins_udp_stream_t *stream,
    const void *data,
    size_t data_len,
    uint64_t offset,
    void *user_data);

typedef struct netins_udp_stream_config_s {
    uint32_t struct_size;
    uint32_t api_version;
    /* One netins_udp_stream_direction_t value.  Kept uint32_t to preserve the
     * ABI layout of the former reserved32 field. */
    uint32_t direction;
    uint32_t flags;
    /* Reserved ABI slots.  Set to zero. */
    size_t reserved_size[2];
    netins_udp_stream_data_cb on_data;
    void *user_data;
    uint64_t reserved[6];
} netins_udp_stream_config_t;

#define NETINS_UDP_STREAM_CONFIG_INITIALIZER \
    { sizeof(netins_udp_stream_config_t), NETINS_UDP_API_VERSION, \
      NETINS_UDP_STREAM_DIRECTION_CLIENT_TO_SERVER, \
      NETINS_UDP_STREAM_FLAG_NONE, {0}, NULL, NULL, {0} }

typedef struct netins_udp_stream_result_s {
    uint32_t struct_size;
    uint32_t api_version;
    uint64_t accepted_bytes;
    uint64_t peer_received_bytes;
    uint64_t elapsed_us;
    uint64_t reserved[5];
} netins_udp_stream_result_t;

#define NETINS_UDP_STREAM_RESULT_INITIALIZER \
    { sizeof(netins_udp_stream_result_t), NETINS_UDP_API_VERSION, \
      0, 0, 0, {0} }

typedef struct netins_udp_stream_snapshot_s {
    uint32_t struct_size;
    uint32_t api_version;
    uint64_t snapshot_time_us;
    int endpoint;
    int state;
    uint32_t lane_count;
    uint32_t flags;
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
    uint64_t lane_payload_sent[NETINS_UDP_STREAM_LANE_COUNT];
    uint64_t lane_payload_received[NETINS_UDP_STREAM_LANE_COUNT];
    int last_error;
    /* Complete lane headers buffered before server stream_accept began. */
    uint32_t preaccept_lane_count;
    uint64_t app_error_code;
    uint64_t reserved[6];
} netins_udp_stream_snapshot_t;

#define NETINS_UDP_STREAM_SNAPSHOT_INITIALIZER \
    { sizeof(netins_udp_stream_snapshot_t), NETINS_UDP_API_VERSION, \
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, \
      {0}, {0}, 0, 0, 0, {0} }

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
 * which may be replaced on a live handle.  On return from a live LOGGER
 * replacement, that handle's background worker no longer uses the previous
 * callback/user_data.  A stream-mode ACCEPTED handle starts with an empty
 * independent logger; set LOGGER on that handle if API-level connection logs
 * are required.  It never replaces the listener's shared server-worker
 * logger.  Closing a stream listener waits until that worker has detached the
 * listener callback/user_data.  File-mode logger inheritance is unchanged.
 * As with every API call, LOGGER replacement must not be made reentrantly from
 * its callback.
 */
NETINS_UDP_PUBLIC netins_udp_t *netins_udp_new(int role);
NETINS_UDP_PUBLIC int netins_udp_set_opt(netins_udp_t *handle,
    int option, const void *value, size_t value_len);

/*
 * connect(), listen(), accept(), and shutdown() are blocking. Their timeout
 * limits come from the corresponding uint32_t options. A zero accept timeout
 * waits indefinitely. A server uses the built-in certificate/key unless file
 * or memory credentials are configured before listen(). host is currently a
 * numeric IPv4 address; TLS server_name is configured separately. An accepted
 * handle keeps the shared listener backend alive, so closing the server
 * handle does not invalidate accepted connections.
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

/*
 * Data streams are half-duplex logical transfers in API version 1.  Direction
 * selects either the client opener or the server accepter as sender.  The
 * sender calls stream_write(); the receiver supplies on_data.  Both sides call
 * finish(): the sender waits for a verified peer receipt, while the receiver
 * waits for peer FIN and for the receipt/reverse FINs to be QUIC-acknowledged.
 * If a compliant receiver finishes and closes with H3_NO_ERROR before the
 * sender's final receipt callback runs, that graceful close also completes a
 * sender that has already submitted every payload record and lane FIN.
 * If a compliant sender closes the connection with H3_NO_ERROR immediately
 * after receiving the verified receipt, that graceful close also completes
 * the receiver after all receipt/reverse-FIN bytes have been submitted.
 * A logical stream may aggregate
 * exactly NETINS_UDP_STREAM_LANE_COUNT H3 Extended Bytestream lanes and
 * delivers
 * received data to on_data in increasing logical-offset order.
 * on_data runs on a bounded delivery worker, outside the XQUIC event thread
 * and outside the stream mutex.  Its memory is valid only during the
 * callback.  The receiver advertises cumulative payload and record credit
 * only after on_data returns, so a slow callback backpressures the sender
 * without allowing tiny records to grow allocator/list overhead without
 * bound.  The callback must not close or wait on the same stream.
 *
 * NETINS_UDP_OPT_STREAM_MODE must be enabled on both endpoints before
 * connect()/listen().  One logical data stream may be active per connection;
 * file transfer/serve calls reject every stream-mode connection with
 * EOPNOTSUPP, even before a logical stream has been opened or accepted.
 * For a non-NULL `accepted` output, stream_write() initializes `*accepted` to
 * zero before validation and then reports bytes copied before a later error.
 */
NETINS_UDP_PUBLIC int netins_udp_stream_open(
    netins_udp_t *connected_client,
    const netins_udp_stream_config_t *config,
    netins_udp_stream_t **out_stream);
NETINS_UDP_PUBLIC int netins_udp_stream_accept(
    netins_udp_t *accepted_connection,
    const netins_udp_stream_config_t *config,
    netins_udp_stream_t **out_stream);
NETINS_UDP_PUBLIC int netins_udp_stream_write(
    netins_udp_stream_t *stream, const void *data, size_t data_len,
    uint32_t timeout_ms, size_t *accepted);
NETINS_UDP_PUBLIC int netins_udp_stream_flush(
    netins_udp_stream_t *stream, uint32_t timeout_ms);
NETINS_UDP_PUBLIC int netins_udp_stream_finish(
    netins_udp_stream_t *stream, uint32_t timeout_ms,
    netins_udp_stream_result_t *result);
NETINS_UDP_PUBLIC int netins_udp_stream_reset(
    netins_udp_stream_t *stream, uint64_t error_code);
/*
 * get_snapshot() is non-blocking with respect to network progress and may be
 * called while another application thread is writing or waiting in finish().
 * Queue byte counts are instantaneous; high-water and record/lane counters
 * are cumulative for the lifetime of the logical stream.  app_error_code is
 * the local value supplied to reset(); transport/protocol failures appear in
 * last_error.  On an accepter, preaccept_lane_count reports complete lane
 * headers already buffered when stream_accept() began; it is zero on an
 * opener.  Version 1 does not transmit the application reset code to the peer
 * and does not provide reconnect/resume across QUIC connections.
 */
NETINS_UDP_PUBLIC int netins_udp_stream_get_snapshot(
    netins_udp_stream_t *stream, netins_udp_stream_snapshot_t *snapshot);
NETINS_UDP_PUBLIC void netins_udp_stream_close(
    netins_udp_stream_t *stream);

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
 * connection-level error code/message retrievable below. Calls on the same
 * handle are not generally safe concurrently. In stream mode, a listener and
 * distinct accepted-connection handles may be used by different threads;
 * each individual handle and stream lifetime must still be serialized.
 */
NETINS_UDP_PUBLIC int netins_udp_get_last_error_code(
    netins_udp_t *handle);
NETINS_UDP_PUBLIC int netins_udp_get_last_error_msg(
    netins_udp_t *handle, char *buffer, size_t buffer_len);

#ifdef __cplusplus
}
#endif

#endif
