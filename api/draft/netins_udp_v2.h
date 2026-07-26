#ifndef NETINS_UDP_V2_CONTRACT_H
#define NETINS_UDP_V2_CONTRACT_H

/*
 * netins_udp V2 public API contract draft.
 *
 * This file freezes the intended V2 source and ABI surface. It is deliberately
 * kept under api/draft and is not installed or compiled until the V1 observer
 * object has been renamed and the V2 implementation is introduced.
 */

#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/types.h>

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

#define NETINS_UDP_API_VERSION              2U
#define NETINS_UDP_MAX_PATHS                8U
#define NETINS_UDP_INTERFACE_NAME_LEN       32U
#define NETINS_UDP_ADDRESS_LEN              128U
#define NETINS_UDP_ALPN_LEN                 64U
#define NETINS_UDP_ERROR_MSG_LEN            256U
#define NETINS_UDP_SERVER_NAME_LEN          256U
#define NETINS_UDP_FILE_PATH_LEN            512U
#define NETINS_UDP_METHOD_LEN               16U
#define NETINS_UDP_REQUEST_PATH_LEN         1024U
#define NETINS_UDP_CONTENT_TYPE_LEN         128U
#define NETINS_UDP_INVALID_CONNECTION_ID    UINT64_MAX
#define NETINS_UDP_INVALID_STREAM_ID        UINT64_MAX
#define NETINS_UDP_ALL_PATHS                (-1)

typedef struct netins_udp_s netins_udp_t;
typedef struct netins_udp_stream_s netins_udp_stream_t;
typedef struct netins_udp_h3_request_s netins_udp_h3_request_t;

/*
 * Lifecycle functions return NETINS_UDP_OK or NETINS_UDP_ERROR. Functions
 * returning pointers use NULL for failure. send/recv functions use -1 for
 * failure. Detailed errors are errno-style values stored on the owning handle.
 */
typedef enum netins_udp_result_e {
    NETINS_UDP_OK = 0,
    NETINS_UDP_ERROR = -1
} netins_udp_result_t;

/* Requested transport. MPQUIC may negotiate or fall back to one active path. */
typedef enum netins_udp_type_e {
    NETINS_UDP_TYPE_QUIC = 1,
    NETINS_UDP_TYPE_MPQUIC = 2
} netins_udp_type_t;

/* Application protocol selected before connect/listen. */
typedef enum netins_udp_app_protocol_e {
    /* netins-udp/1: default bidirectional QUIC byte stream. */
    NETINS_UDP_APP_RAW_STREAM = 1,
    /* HTTP/3: use netins_udp_h3_request_*; connection send/recv is invalid. */
    NETINS_UDP_APP_HTTP3 = 2
} netins_udp_app_protocol_t;

typedef enum netins_udp_role_e {
    NETINS_UDP_ROLE_NONE = 0,
    NETINS_UDP_ROLE_CLIENT = 1,
    NETINS_UDP_ROLE_LISTENER = 2,
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

typedef enum netins_udp_path_state_e {
    NETINS_UDP_PATH_INIT = 0,
    NETINS_UDP_PATH_VALIDATING = 1,
    NETINS_UDP_PATH_ACTIVE = 2,
    NETINS_UDP_PATH_CLOSING = 3,
    NETINS_UDP_PATH_CLOSED = 4,
    NETINS_UDP_PATH_FAILED = 5
} netins_udp_path_state_t;

typedef enum netins_udp_app_path_status_e {
    NETINS_UDP_APP_PATH_NONE = 0,
    NETINS_UDP_APP_PATH_STANDBY = 1,
    NETINS_UDP_APP_PATH_AVAILABLE = 2,
    NETINS_UDP_APP_PATH_FROZEN = 3
} netins_udp_app_path_status_t;

typedef enum netins_udp_close_source_e {
    NETINS_UDP_CLOSE_NONE = 0,
    NETINS_UDP_CLOSE_LOCAL = 1,
    NETINS_UDP_CLOSE_PEER = 2,
    NETINS_UDP_CLOSE_TRANSPORT = 3,
    NETINS_UDP_CLOSE_IDLE_TIMEOUT = 4,
    NETINS_UDP_CLOSE_HANDSHAKE_TIMEOUT = 5,
    NETINS_UDP_CLOSE_NETWORK_ERROR = 6,
    NETINS_UDP_CLOSE_INTERNAL_ERROR = 7
} netins_udp_close_source_t;

typedef enum netins_udp_log_level_e {
    NETINS_UDP_LOG_LEVEL_DEBUG = 0,
    NETINS_UDP_LOG_LEVEL_INFO = 1,
    NETINS_UDP_LOG_LEVEL_WARN = 2,
    NETINS_UDP_LOG_LEVEL_ERROR = 3
} netins_udp_log_level_t;

typedef enum netins_udp_scheduler_e {
    NETINS_UDP_SCHEDULER_DEFAULT = 0,
    NETINS_UDP_SCHEDULER_MINRTT = 1,
    NETINS_UDP_SCHEDULER_BACKUP = 2,
    NETINS_UDP_SCHEDULER_BALANCED = 3,
    NETINS_UDP_SCHEDULER_RAP = 4,
    NETINS_UDP_SCHEDULER_ACT = 5,
    NETINS_UDP_SCHEDULER_BANDWIDTH = 6,
    NETINS_UDP_SCHEDULER_FASTPROBE = 7,
    NETINS_UDP_SCHEDULER_ADAPTIVE = 8
} netins_udp_scheduler_t;

typedef enum netins_udp_congestion_control_e {
    NETINS_UDP_CC_DEFAULT = 0,
    NETINS_UDP_CC_CUBIC = 1,
    NETINS_UDP_CC_BBR = 2,
    NETINS_UDP_CC_BBR2 = 3,
    NETINS_UDP_CC_RENO = 4
} netins_udp_congestion_control_t;

/*
 * Stable library flags. They are not aliases for platform MSG_* values.
 * Unsupported or directionally invalid flags fail with EOPNOTSUPP/EINVAL.
 */
typedef enum netins_udp_msg_flag_e {
    NETINS_UDP_MSG_NONE = 0,
    NETINS_UDP_MSG_DONTWAIT = 1U << 0,
    NETINS_UDP_MSG_PEEK = 1U << 1, /* recv only */
    NETINS_UDP_MSG_FIN = 1U << 2   /* send only */
} netins_udp_msg_flag_t;

/* Events reported by the notification descriptor returned by get_fd(). */
typedef enum netins_udp_event_e {
    NETINS_UDP_EVENT_NONE = 0,
    NETINS_UDP_EVENT_CONNECTED = 1U << 0,
    NETINS_UDP_EVENT_ACCEPTABLE = 1U << 1,
    NETINS_UDP_EVENT_READABLE = 1U << 2,
    NETINS_UDP_EVENT_WRITABLE = 1U << 3,
    NETINS_UDP_EVENT_PATH_CHANGED = 1U << 4,
    NETINS_UDP_EVENT_CLOSED = 1U << 5,
    NETINS_UDP_EVENT_ERROR = 1U << 6,
    NETINS_UDP_EVENT_STREAM_ACCEPTABLE = 1U << 7,
    NETINS_UDP_EVENT_H3_REQUEST_ACCEPTABLE = 1U << 8
} netins_udp_event_t;

typedef enum netins_udp_opt_e {
    NETINS_UDP_OPT_NONBLOCK = 1,             /* int */
    NETINS_UDP_OPT_CONNECT_TIMEOUT_MS = 2,   /* uint32_t */
    NETINS_UDP_OPT_ACCEPT_TIMEOUT_MS = 3,    /* uint32_t */
    NETINS_UDP_OPT_SEND_TIMEOUT_MS = 4,      /* uint32_t */
    NETINS_UDP_OPT_RECV_TIMEOUT_MS = 5,      /* uint32_t */
    NETINS_UDP_OPT_CLOSE_TIMEOUT_MS = 6,     /* uint32_t */
    NETINS_UDP_OPT_IDLE_TIMEOUT_MS = 7,      /* uint32_t */
    NETINS_UDP_OPT_KERNEL_SNDBUF = 8,        /* int */
    NETINS_UDP_OPT_KERNEL_RCVBUF = 9,        /* int */
    NETINS_UDP_OPT_INTERFACES = 10,          /* netins_udp_interfaces_t */
    NETINS_UDP_OPT_APP_PROTOCOL = 11,        /* int: netins_udp_app_protocol_t */
    NETINS_UDP_OPT_TLS = 12,                 /* netins_udp_tls_config_t */
    NETINS_UDP_OPT_INITIAL_CWND_PACKETS = 13,/* uint32_t */
    NETINS_UDP_OPT_SCHEDULER = 14,           /* int: netins_udp_scheduler_t */
    NETINS_UDP_OPT_CONGESTION_CONTROL = 15,  /* int: netins_udp_congestion_control_t */
    NETINS_UDP_OPT_SOCKOPT = 16              /* netins_udp_sockopt_t */
} netins_udp_opt_t;

typedef void (*netins_udp_logger_cb)(int level, const char *message,
    void *user_data);

typedef struct netins_udp_interfaces_s {
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t interface_count;
    uint32_t reserved32;
    char names[NETINS_UDP_MAX_PATHS][NETINS_UDP_INTERFACE_NAME_LEN];
    uint64_t reserved[4];
} netins_udp_interfaces_t;

#define NETINS_UDP_INTERFACES_INITIALIZER \
    { sizeof(netins_udp_interfaces_t), NETINS_UDP_API_VERSION }

/*
 * The library copies this structure and all strings during set_opt().
 * cert/key are required for listeners. Client certificate fields may be empty.
 */
typedef struct netins_udp_tls_config_s {
    uint32_t struct_size;
    uint32_t api_version;
    int verify_peer;
    uint32_t reserved32;
    char server_name[NETINS_UDP_SERVER_NAME_LEN];
    char certificate_file[NETINS_UDP_FILE_PATH_LEN];
    char private_key_file[NETINS_UDP_FILE_PATH_LEN];
    char ca_file[NETINS_UDP_FILE_PATH_LEN];
    uint64_t reserved[4];
} netins_udp_tls_config_t;

#define NETINS_UDP_TLS_CONFIG_INITIALIZER \
    { sizeof(netins_udp_tls_config_t), NETINS_UDP_API_VERSION }

/*
 * Raw UDP socket option. path_index=-1 applies to every current and future
 * path socket. A non-negative index addresses the configured path slot.
 * optval is consumed synchronously and is not retained after set_opt().
 */
typedef struct netins_udp_sockopt_s {
    uint32_t struct_size;
    uint32_t api_version;
    int path_index;
    int level;
    int optname;
    const void *optval;
    socklen_t optlen;
    uint32_t reserved32;
    uint64_t reserved[2];
} netins_udp_sockopt_t;

#define NETINS_UDP_SOCKOPT_INITIALIZER \
    { sizeof(netins_udp_sockopt_t), NETINS_UDP_API_VERSION, \
      NETINS_UDP_ALL_PATHS }

typedef struct netins_udp_info_s {
    uint32_t struct_size;
    uint32_t api_version;
    uint64_t connection_id;
    int requested_type;       /* netins_udp_type_t */
    int role;                 /* netins_udp_role_t */
    int state;                /* netins_udp_state_t */
    int app_protocol;         /* netins_udp_app_protocol_t */
    int handshake_finished;
    int multipath_mode;       /* 0=single, 1=single-PNS MP, 2=multi-PNS MP */
    int multipath_requested;
    int multipath_active;
    uint32_t path_count;
    uint32_t active_path_count;
    uint32_t validating_path_count;
    uint32_t reserved32;
    char alpn[NETINS_UDP_ALPN_LEN];
    char local_address[NETINS_UDP_ADDRESS_LEN];
    char peer_address[NETINS_UDP_ADDRESS_LEN];
    uint64_t reserved[8];
} netins_udp_info_t;

#define NETINS_UDP_INFO_INITIALIZER \
    { sizeof(netins_udp_info_t), NETINS_UDP_API_VERSION, \
      NETINS_UDP_INVALID_CONNECTION_ID }

typedef struct netins_udp_statis_s {
    uint32_t struct_size;
    uint32_t api_version;
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
    uint64_t reserved[8];
} netins_udp_statis_t;

#define NETINS_UDP_STATIS_INITIALIZER \
    { sizeof(netins_udp_statis_t), NETINS_UDP_API_VERSION }

typedef struct netins_udp_path_statis_s {
    uint32_t struct_size;
    uint32_t api_version;
    uint64_t path_id;
    int path_state;           /* netins_udp_path_state_t */
    int app_status;           /* netins_udp_app_path_status_t */
    int socket_fd_available;
    int cwnd_blocked;
    char interface_name[NETINS_UDP_INTERFACE_NAME_LEN];
    char local_address[NETINS_UDP_ADDRESS_LEN];
    char peer_address[NETINS_UDP_ADDRESS_LEN];
    uint64_t bytes_sent;
    uint64_t bytes_received;
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
    uint64_t reserved[8];
} netins_udp_path_statis_t;

#define NETINS_UDP_PATH_STATIS_INITIALIZER \
    { sizeof(netins_udp_path_statis_t), NETINS_UDP_API_VERSION, UINT64_MAX }

typedef struct netins_udp_close_reason_s {
    uint32_t struct_size;
    uint32_t api_version;
    int source;               /* netins_udp_close_source_t */
    int system_error;         /* errno-style error, 0 when not applicable */
    uint64_t transport_error;
    uint64_t application_error;
    char message[NETINS_UDP_ERROR_MSG_LEN];
    uint64_t reserved[4];
} netins_udp_close_reason_t;

#define NETINS_UDP_CLOSE_REASON_INITIALIZER \
    { sizeof(netins_udp_close_reason_t), NETINS_UDP_API_VERSION }

typedef struct netins_udp_snapshot_s {
    uint32_t struct_size;
    uint32_t api_version;
    netins_udp_info_t info;
    netins_udp_statis_t statis;
    uint32_t path_count;
    uint32_t reserved32;
    netins_udp_path_statis_t paths[NETINS_UDP_MAX_PATHS];
    netins_udp_close_reason_t close_reason;
    uint64_t reserved[8];
} netins_udp_snapshot_t;

#define NETINS_UDP_SNAPSHOT_INITIALIZER \
    { sizeof(netins_udp_snapshot_t), NETINS_UDP_API_VERSION, \
      NETINS_UDP_INFO_INITIALIZER, NETINS_UDP_STATIS_INITIALIZER }

typedef struct netins_udp_stream_info_s {
    uint32_t struct_size;
    uint32_t api_version;
    uint64_t stream_id;
    int local_initiated;
    int bidirectional;
    int send_finished;
    int receive_finished;
    int last_error_code;
    uint32_t reserved32;
    uint64_t bytes_sent;
    uint64_t bytes_received;
    uint64_t reserved[4];
} netins_udp_stream_info_t;

#define NETINS_UDP_STREAM_INFO_INITIALIZER \
    { sizeof(netins_udp_stream_info_t), NETINS_UDP_API_VERSION, \
      NETINS_UDP_INVALID_STREAM_ID }

typedef struct netins_udp_h3_request_config_s {
    uint32_t struct_size;
    uint32_t api_version;
    char method[NETINS_UDP_METHOD_LEN];
    char path[NETINS_UDP_REQUEST_PATH_LEN];
    char authority[NETINS_UDP_SERVER_NAME_LEN];
    char content_type[NETINS_UDP_CONTENT_TYPE_LEN];
    uint64_t content_length;
    uint64_t reserved[8];
} netins_udp_h3_request_config_t;

#define NETINS_UDP_H3_REQUEST_CONFIG_INITIALIZER \
    { sizeof(netins_udp_h3_request_config_t), NETINS_UDP_API_VERSION }

typedef struct netins_udp_h3_request_info_s {
    uint32_t struct_size;
    uint32_t api_version;
    uint64_t stream_id;
    int response_status;
    int headers_received;
    int send_finished;
    int receive_finished;
    int last_error_code;
    char method[NETINS_UDP_METHOD_LEN];
    char path[NETINS_UDP_REQUEST_PATH_LEN];
    char content_type[NETINS_UDP_CONTENT_TYPE_LEN];
    uint64_t content_length;
    uint64_t bytes_sent;
    uint64_t bytes_received;
    uint64_t reserved[8];
} netins_udp_h3_request_info_t;

#define NETINS_UDP_H3_REQUEST_INFO_INITIALIZER \
    { sizeof(netins_udp_h3_request_info_t), NETINS_UDP_API_VERSION, \
      NETINS_UDP_INVALID_STREAM_ID }

/* Process-wide initialization. Calls are thread-safe and reference counted. */
NETINS_UDP_PUBLIC int netins_udp_init(void);
/*
 * Releases one initialization reference. The final cleanup fails with EBUSY
 * while public handles still exist.
 */
NETINS_UDP_PUBLIC int netins_udp_cleanup(void);
NETINS_UDP_PUBLIC uint32_t netins_udp_get_api_version(void);

/*
 * Creates an unconnected object. On allocation/initialization failure, returns
 * NULL and sets errno. The requested type is an intent; query actual
 * multipath_active after a connection is established.
 */
NETINS_UDP_PUBLIC netins_udp_t *netins_udp_new(int type);

/*
 * Gracefully closes the represented connection/listener, stops owned workers,
 * releases all resources and invalidates the pointer. Calling close twice is
 * invalid. Closing a listener also wakes blocked accept calls.
 */
NETINS_UDP_PUBLIC void netins_udp_close(netins_udp_t *netins);

/*
 * Options may be set in NEW state. Runtime options are limited to NONBLOCK,
 * send/receive timeouts and compatible socket options. Immutable options fail
 * with EBUSY after connect/listen begins.
 */
NETINS_UDP_PUBLIC int netins_udp_set_opt(netins_udp_t *netins,
    int option, const void *value, size_t value_len);
NETINS_UDP_PUBLIC int netins_udp_get_opt(netins_udp_t *netins,
    int option, void *value, size_t *value_len);

NETINS_UDP_PUBLIC int netins_udp_set_logger(netins_udp_t *netins,
    netins_udp_logger_cb logger, void *user_data);

/*
 * Blocking connect returns after the QUIC handshake and application protocol
 * setup complete. Nonblocking connect returns -1/EINPROGRESS and completion is
 * reported through state/events.
 */
NETINS_UDP_PUBLIC int netins_udp_connect(netins_udp_t *netins,
    const char *host, int port);

/*
 * listen transitions the same object to LISTENING and returns it. On failure,
 * it returns NULL but the input object remains valid for error query/close.
 */
NETINS_UDP_PUBLIC netins_udp_t *netins_udp_listen(netins_udp_t *netins,
    const char *host, int port);

/*
 * Returns an independently owned accepted connection. Blocking behavior is
 * controlled by listener options. Accepted connections share the listener's
 * UDP socket/engine internally but have independent QUIC close lifecycles.
 */
NETINS_UDP_PUBLIC netins_udp_t *netins_udp_accept(
    netins_udp_t *listener, struct sockaddr *addr, socklen_t *addrlen);

/*
 * Initiates graceful transport shutdown without releasing the handle, allowing
 * final state/statistics/close reason to be queried before netins_udp_close().
 */
NETINS_UDP_PUBLIC int netins_udp_shutdown(netins_udp_t *netins,
    uint64_t application_error);

/*
 * send/recv operate only on the default bidirectional stream in RAW_STREAM
 * mode. They support partial transfers. recv returns 0 only after stream FIN
 * and all buffered data have been delivered. On return from send, the caller
 * may immediately reuse or release the supplied buffer.
 */
NETINS_UDP_PUBLIC ssize_t netins_udp_send(netins_udp_t *netins,
    const void *data, size_t length, int flags);
NETINS_UDP_PUBLIC ssize_t netins_udp_recv(netins_udp_t *netins,
    void *data, size_t length, int flags);

/* Additional raw QUIC streams; invalid in HTTP3 mode. */
NETINS_UDP_PUBLIC int netins_udp_stream_open(netins_udp_t *netins,
    netins_udp_stream_t **out_stream);
NETINS_UDP_PUBLIC int netins_udp_stream_accept(netins_udp_t *netins,
    netins_udp_stream_t **out_stream);
NETINS_UDP_PUBLIC ssize_t netins_udp_stream_send(
    netins_udp_stream_t *stream, const void *data, size_t length, int flags);
NETINS_UDP_PUBLIC ssize_t netins_udp_stream_recv(
    netins_udp_stream_t *stream, void *data, size_t length, int flags);
NETINS_UDP_PUBLIC int netins_udp_stream_get_info(
    netins_udp_stream_t *stream, netins_udp_stream_info_t *out);
NETINS_UDP_PUBLIC int netins_udp_stream_shutdown(
    netins_udp_stream_t *stream, uint64_t application_error);
NETINS_UDP_PUBLIC void netins_udp_stream_close(
    netins_udp_stream_t *stream);

/*
 * HTTP/3 request extension. Client opens requests; server accepts requests.
 * Body send/recv has the same partial-transfer, blocking and ownership rules as
 * raw stream send/recv. config_init selects GET, "/", application/octet-stream
 * and a zero-length request body. A zero-length request/response is finished
 * with its headers. For a non-empty body, FIN is valid only on the call that
 * accepts the final declared content-length bytes.
 */
NETINS_UDP_PUBLIC void netins_udp_h3_request_config_init(
    netins_udp_h3_request_config_t *config);
NETINS_UDP_PUBLIC int netins_udp_h3_request_open(netins_udp_t *netins,
    const netins_udp_h3_request_config_t *config,
    netins_udp_h3_request_t **out_request);
NETINS_UDP_PUBLIC int netins_udp_h3_request_accept(netins_udp_t *netins,
    netins_udp_h3_request_t **out_request);
NETINS_UDP_PUBLIC int netins_udp_h3_request_set_response(
    netins_udp_h3_request_t *request, int status,
    const char *content_type, uint64_t content_length);
NETINS_UDP_PUBLIC ssize_t netins_udp_h3_request_send(
    netins_udp_h3_request_t *request, const void *data,
    size_t length, int flags);
NETINS_UDP_PUBLIC ssize_t netins_udp_h3_request_recv(
    netins_udp_h3_request_t *request, void *data,
    size_t length, int flags);
NETINS_UDP_PUBLIC int netins_udp_h3_request_get_info(
    netins_udp_h3_request_t *request,
    netins_udp_h3_request_info_t *out);
NETINS_UDP_PUBLIC void netins_udp_h3_request_close(
    netins_udp_h3_request_t *request);

/* Thread-safe copy-out status APIs. */
NETINS_UDP_PUBLIC int netins_udp_get_state(netins_udp_t *netins);
NETINS_UDP_PUBLIC int netins_udp_is_multipath(netins_udp_t *netins);
NETINS_UDP_PUBLIC int netins_udp_get_path_count(netins_udp_t *netins,
    uint32_t *out_count);
NETINS_UDP_PUBLIC int netins_udp_get_info(netins_udp_t *netins,
    netins_udp_info_t *out);
NETINS_UDP_PUBLIC int netins_udp_get_statis(netins_udp_t *netins,
    netins_udp_statis_t *out);
NETINS_UDP_PUBLIC int netins_udp_get_path_statis(netins_udp_t *netins,
    size_t path_index, netins_udp_path_statis_t *out);
NETINS_UDP_PUBLIC int netins_udp_get_snapshot(netins_udp_t *netins,
    netins_udp_snapshot_t *out);
NETINS_UDP_PUBLIC int netins_udp_get_close_reason(netins_udp_t *netins,
    netins_udp_close_reason_t *out);

NETINS_UDP_PUBLIC int netins_udp_get_last_error_code(
    netins_udp_t *netins);
NETINS_UDP_PUBLIC int netins_udp_get_last_error_msg(
    netins_udp_t *netins, char *buffer, size_t buffer_len);

/*
 * Returns a library-owned readiness-notification descriptor, never a path UDP
 * socket. Accepted connections have independent notification descriptors even
 * when their transport socket/engine is shared with a listener. The caller
 * must not read/close the descriptor directly; call get_events to acknowledge
 * pending notifications.
 */
NETINS_UDP_PUBLIC int netins_udp_get_fd(netins_udp_t *netins);
NETINS_UDP_PUBLIC int netins_udp_get_events(netins_udp_t *netins,
    uint32_t *out_events);

NETINS_UDP_PUBLIC const char *netins_udp_state_name(int state);
NETINS_UDP_PUBLIC const char *netins_udp_path_state_name(int state);
NETINS_UDP_PUBLIC const char *netins_udp_app_path_status_name(int status);

#ifdef __cplusplus
}
#endif

#endif /* NETINS_UDP_V2_CONTRACT_H */
