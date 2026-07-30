#ifndef NETINS_UDP_API_H
#define NETINS_UDP_API_H

/*
 * NetIns UDP public API.
 *
 * Provides client connections, server listeners, accepted connections,
 * HTTP/3 request data transfer, multipath status and statistics, readiness
 * notifications, logging, errors and close reasons.
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
#define NETINS_UDP_MAX_H3_HEADERS           16U
#define NETINS_UDP_H3_HEADER_NAME_LEN       64U
#define NETINS_UDP_H3_HEADER_VALUE_LEN      256U
#define NETINS_UDP_INVALID_CONNECTION_ID    UINT64_MAX
#define NETINS_UDP_INVALID_STREAM_ID        UINT64_MAX
#define NETINS_UDP_CONTENT_LENGTH_UNKNOWN   UINT64_MAX
#define NETINS_UDP_ALL_PATHS                (-1)

typedef struct netins_udp_s netins_udp_t;
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

/* Application protocol used for request and response data transfer. */
typedef enum netins_udp_app_protocol_e {
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

/*
 * Log messages are valid only for the duration of the callback. The callback
 * may run on an API caller or an internal transport worker thread, must return
 * promptly, and must not call back into NetIns UDP with the same handle.
 */
typedef void (*netins_udp_logger_cb)(int level, const char *message);

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
    NETINS_UDP_CC_BBR = 2,
    /* Requires XQUIC to be built with XQC_ENABLE_BBR2=ON. */
    NETINS_UDP_CC_BBR2 = 3,
    /* Requires XQUIC to be built with XQC_ENABLE_RENO=ON. */
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

/*
 * Events reported by connection and child notification descriptors.
 * CONNECTED/PATH_CHANGED/CLOSED/ERROR are latched transition events.
 * ACCEPTABLE/READABLE/WRITABLE and H3_REQUEST_ACCEPTABLE are level
 * conditions returned by get_events(). To avoid a receive-only event loop
 * spinning, a continuously true WRITABLE level does not by itself keep the
 * shared notification fd readable; later writable transitions still wake it.
 * Bit 7 is reserved for ABI stability.
 */
typedef enum netins_udp_event_e {
    NETINS_UDP_EVENT_NONE = 0,
    NETINS_UDP_EVENT_CONNECTED = 1U << 0,
    NETINS_UDP_EVENT_ACCEPTABLE = 1U << 1,
    NETINS_UDP_EVENT_READABLE = 1U << 2,
    NETINS_UDP_EVENT_WRITABLE = 1U << 3,
    NETINS_UDP_EVENT_PATH_CHANGED = 1U << 4,
    NETINS_UDP_EVENT_CLOSED = 1U << 5,
    NETINS_UDP_EVENT_ERROR = 1U << 6,
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
    NETINS_UDP_OPT_LOG_LEVEL = 16            /* int: netins_udp_log_level_t */
} netins_udp_opt_t;

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
 * cert/key are required for listeners. Client certificate authentication is
 * not supported by this release, so client certificate fields and
 * require_client_certificate must remain empty/zero. Peer verification
 * defaults to enabled. A client may set ca_file to a PEM trust store; an empty
 * ca_file uses the platform default trust paths.
 */
typedef struct netins_udp_tls_config_s {
    uint32_t struct_size;
    uint32_t api_version;
    int verify_peer;
    int require_client_certificate;
    char server_name[NETINS_UDP_SERVER_NAME_LEN];
    char certificate_file[NETINS_UDP_FILE_PATH_LEN];
    char private_key_file[NETINS_UDP_FILE_PATH_LEN];
    char ca_file[NETINS_UDP_FILE_PATH_LEN];
    uint64_t reserved[4];
} netins_udp_tls_config_t;

#define NETINS_UDP_TLS_CONFIG_INITIALIZER \
    { sizeof(netins_udp_tls_config_t), NETINS_UDP_API_VERSION, 1, 0 }

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
    /* Nonzero when more than NETINS_UDP_MAX_PATHS existed over this lifetime. */
    uint32_t path_table_truncated;
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
    /*
     * In connection-level send/recv mode these are the application bytes
     * accepted by send() and consumed by recv(); private aggregation framing
     * is excluded. Before that mode is selected, or when using the advanced
     * request API, they are unique QUIC STREAM payload bytes and may include
     * application-protocol control streams. QUIC packet/frame overhead,
     * retransmissions and reinjection duplicates are always excluded.
     */
    uint64_t bytes_sent;
    uint64_t bytes_received;
    uint64_t bytes_wire_sent;
    uint64_t bytes_wire_received;
    uint64_t bytes_acked;
    /*
     * Encoded retransmitted bytes used as a loss-volume proxy. XQUIC does not
     * retain an exact lost-byte counter; loss_rate is the packet-based ratio.
     */
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
    /*
     * Stable slot assigned when path_id is first observed.  It never changes
     * or gets reused before the owning connection is closed.
     */
    uint32_t path_index;
    int path_state;           /* netins_udp_path_state_t */
    int app_status;           /* netins_udp_app_path_status_t */
    int cwnd_blocked;
    char interface_name[NETINS_UDP_INTERFACE_NAME_LEN];
    char local_address[NETINS_UDP_ADDRESS_LEN];
    char peer_address[NETINS_UDP_ADDRESS_LEN];
    /*
     * Unique STREAM payload attribution: sent bytes belong to the path of the
     * original transmission; received bytes belong to the retained copy that
     * supplies the newly consumed stream offsets. Retransmission/reinjection
     * duplicates are not added again.
     */
    uint64_t bytes_sent;
    uint64_t bytes_received;
    uint64_t bytes_acked;
    /* Same retransmitted-byte loss proxy as netins_udp_statis_t. */
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

typedef struct netins_udp_h3_header_s {
    char name[NETINS_UDP_H3_HEADER_NAME_LEN];
    char value[NETINS_UDP_H3_HEADER_VALUE_LEN];
} netins_udp_h3_header_t;

typedef struct netins_udp_h3_request_config_s {
    uint32_t struct_size;
    uint32_t api_version;
    char method[NETINS_UDP_METHOD_LEN];
    char path[NETINS_UDP_REQUEST_PATH_LEN];
    /*
     * HTTP :authority. When empty, the client uses its TLS server name (or
     * connect host) and includes a non-default HTTPS port automatically.
     */
    char authority[NETINS_UDP_SERVER_NAME_LEN];
    char content_type[NETINS_UDP_CONTENT_TYPE_LEN];
    uint64_t content_length;
    uint32_t header_count;
    uint32_t reserved32;
    netins_udp_h3_header_t headers[NETINS_UDP_MAX_H3_HEADERS];
    uint64_t reserved[4];
} netins_udp_h3_request_config_t;

#define NETINS_UDP_H3_REQUEST_CONFIG_INITIALIZER \
    { sizeof(netins_udp_h3_request_config_t), NETINS_UDP_API_VERSION }

typedef struct netins_udp_h3_response_config_s {
    uint32_t struct_size;
    uint32_t api_version;
    int status;
    uint32_t header_count;
    uint64_t content_length;
    char content_type[NETINS_UDP_CONTENT_TYPE_LEN];
    netins_udp_h3_header_t headers[NETINS_UDP_MAX_H3_HEADERS];
    uint64_t reserved[4];
} netins_udp_h3_response_config_t;

#define NETINS_UDP_H3_RESPONSE_CONFIG_INITIALIZER \
    { sizeof(netins_udp_h3_response_config_t), NETINS_UDP_API_VERSION, 200 }

typedef struct netins_udp_h3_request_info_s {
    uint32_t struct_size;
    uint32_t api_version;
    uint64_t stream_id;
    int response_status;
    int request_headers_ready;
    int response_headers_ready;
    int send_finished;
    int receive_finished;
    int last_error_code;
    uint32_t peer_header_count;
    char method[NETINS_UDP_METHOD_LEN];
    char path[NETINS_UDP_REQUEST_PATH_LEN];
    char authority[NETINS_UDP_SERVER_NAME_LEN];
    char request_content_type[NETINS_UDP_CONTENT_TYPE_LEN];
    char response_content_type[NETINS_UDP_CONTENT_TYPE_LEN];
    /* UNKNOWN when an accepted peer request omitted Content-Length. */
    uint64_t request_content_length;
    /* UNKNOWN when the peer response omitted Content-Length. */
    uint64_t response_content_length;
    /* HTTP body bytes only; H3/QPACK framing and headers are excluded. */
    uint64_t bytes_sent;
    uint64_t bytes_received;
    netins_udp_h3_header_t peer_headers[NETINS_UDP_MAX_H3_HEADERS];
    uint64_t reserved[4];
} netins_udp_h3_request_info_t;

#define NETINS_UDP_H3_REQUEST_INFO_INITIALIZER \
    { sizeof(netins_udp_h3_request_info_t), NETINS_UDP_API_VERSION, \
      NETINS_UDP_INVALID_STREAM_ID }

/* Process-wide initialization. Calls are thread-safe and reference counted. */
NETINS_UDP_PUBLIC int netins_udp_init(void);
/*
 * Releases one initialization reference. The final cleanup fails with EBUSY
 * while public handles or asynchronous DNS resolver tasks still exist. Close
 * all handles and retry cleanup after the resolver completes.
 */
NETINS_UDP_PUBLIC int netins_udp_cleanup(void);
/* Returns the ABI version implemented by the linked NetIns UDP library. */
NETINS_UDP_PUBLIC uint32_t netins_udp_get_api_version(void);

/*
 * Creates an unconnected object. On allocation/initialization failure, returns
 * NULL and sets errno. The requested type is an intent; query actual
 * multipath_active after a connection is established.
 */
NETINS_UDP_PUBLIC netins_udp_t *netins_udp_new(int type);

/*
 * Installs a per-object logger. NULL disables logging and is the default.
 * The default threshold is INFO; set NETINS_UDP_OPT_LOG_LEVEL to DEBUG for
 * additional low-frequency lifecycle details. Detailed path and throughput
 * data remains available through snapshot/statistics APIs and is not emitted
 * as log text. Accepted connections inherit the listener's logger and level
 * at accept time, then remain independently configurable.
 *
 * Replacing or disabling a logger waits for callbacks already in progress.
 */
NETINS_UDP_PUBLIC int netins_udp_set_logger(netins_udp_t *netins,
    netins_udp_logger_cb logger);

/*
 * Gracefully closes the represented connection/listener, releases its
 * ownership and invalidates the pointer. Shared listener workers stop when
 * their last accepted-connection reference is released. Calling close twice
 * is invalid. Once close begins, callers must not start new calls on the
 * handle; calls already inside the API are canceled/waited before memory is
 * released. Closing a listener also wakes blocked accept calls. Outstanding
 * child HTTP/3 request handles are aborted and detached but remain valid for
 * get_info/close; the caller must still close every child handle.
 */
NETINS_UDP_PUBLIC void netins_udp_close(netins_udp_t *netins);

/*
 * Options may be set in NEW state. Runtime options are limited to NONBLOCK,
 * send/receive/close timeouts, LOG_LEVEL, plus ACCEPT_TIMEOUT_MS on a listener.
 * Immutable options fail with EBUSY after connect/listen begins.
 * Runtime values are captured when a child HTTP/3 request is opened/accepted;
 * changing its parent later affects subsequent child handles only. Accepted
 * connections likewise inherit listener runtime values at accept time and are
 * independently configurable afterward.
 *
 * get_opt uses value_len as an in/out byte count. value may be NULL to query
 * the required size. A short buffer fails with ENOBUFS after storing the
 * required size.
 */
NETINS_UDP_PUBLIC int netins_udp_set_opt(netins_udp_t *netins,
    int option, const void *value, size_t value_len);
NETINS_UDP_PUBLIC int netins_udp_get_opt(netins_udp_t *netins,
    int option, void *value, size_t *value_len);

NETINS_UDP_PUBLIC void netins_udp_tls_config_init(
    netins_udp_tls_config_t *config);

/*
 * Blocking connect returns after DNS, the QUIC handshake and application
 * protocol setup complete. Nonblocking connect performs DNS and transport
 * startup asynchronously, returns -1/EINPROGRESS, and reports completion
 * through state/events. CONNECT_TIMEOUT_MS covers the complete establishment
 * sequence in both modes. If setup fails before a transport object exists,
 * the handle remains valid for state/info/snapshot/error queries and close;
 * transport counters and paths are reported as zero/empty.
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
 * controlled by listener options. The returned connection remains valid until
 * it is closed, including after the listener has been closed.
 */
NETINS_UDP_PUBLIC netins_udp_t *netins_udp_accept(
    netins_udp_t *listener, struct sockaddr *addr, socklen_t *addrlen);

/*
 * Initiates graceful transport shutdown without releasing the handle. For the
 * connection-level data API, application_error==0 first finishes the hidden
 * send direction. An accepted server connection then allows the client to
 * consume the completed receive direction and close the connection, bounded by
 * the configured close timeout. A nonzero application_error is an immediate
 * error close.
 * Final state/statistics/close reason remain queryable until close().
 */
NETINS_UDP_PUBLIC int netins_udp_shutdown(netins_udp_t *netins,
    uint64_t application_error);

/*
 * Reliable, ordered connection-level application data. These functions hide
 * the private parallel transport channels and expose one continuous byte
 * stream: callers do not create or accept child requests for this mode.
 *
 * send/recv are valid on a connected client or an accepted server connection,
 * but not on a listener. A successful call may transfer fewer bytes than
 * requested. recv returns 0 after the peer has cleanly finished its send
 * direction. NETINS_UDP_MSG_FIN finishes the local send direction after the
 * bytes accepted by that call; it takes effect only when the return value
 * equals length. NETINS_UDP_MSG_DONTWAIT applies to either call;
 * NETINS_UDP_MSG_PEEK applies only to recv.
 *
 * The API preserves byte order but not individual send-call boundaries.
 * One sender and one receiver may operate concurrently. Additional senders
 * and receivers are serialized by the library.
 * Bytes counted by a positive send return value have been copied into
 * library-owned bounded queues; the caller may reuse that input memory.
 * NONBLOCK and send/receive/close timeouts are captured when the private data
 * channels are first created; configure them before the first send/recv call.
 *
 * Do not mix these connection-level functions with the advanced HTTP/3
 * request API on the same connection. The first selected data mode remains
 * fixed for the connection lifetime; attempts to select the other mode fail
 * with EBUSY.
 */
NETINS_UDP_PUBLIC ssize_t netins_udp_send(netins_udp_t *netins,
    const void *data, size_t length, int flags);
NETINS_UDP_PUBLIC ssize_t netins_udp_recv(netins_udp_t *netins,
    void *data, size_t length, int flags);

/*
 * HTTP/3 request extension. Client opens requests; server accepts requests.
 * Body send/recv has the same partial-transfer, blocking and ownership rules as
 * the connection options. config_init selects GET, "/",
 * application/octet-stream
 * and a zero-length request body. A zero-length request/response is finished
 * with its headers. For a non-empty body, FIN is valid only on the call that
 * accepts the final declared content-length bytes. If content_length is
 * NETINS_UDP_CONTENT_LENGTH_UNKNOWN, Content-Length is omitted and FIN defines
 * the body end. Application-specific fields are added only when explicitly
 * configured. Custom header names must be lowercase, must not start with ':',
 * and cannot replace headers managed by the library (content-length,
 * content-type, host or pseudo-headers). HTTP/3 connection-specific fields are
 * rejected; TE is accepted only with the value "trailers". A finite blocking
 * timeout expires with ETIMEDOUT; nonblocking lack of progress returns EAGAIN.
 * Server request accept uses the accepted connection's NONBLOCK and
 * RECV_TIMEOUT_MS options.
 * set_response is single-shot, accepts one final status from 200 through 599,
 * and must succeed before a server response body is sent. Responses to HEAD,
 * 204, 205 or 304 have no body: 204 omits Content-Length, 205
 * requires Content-Length 0, and 304 may advertise a representation length
 * but still forbids DATA. Sending body bytes for these cases fails with
 * EOPNOTSUPP. Applications own every accepted request handle independently
 * and must close it even after
 * its parent connection has closed. Closing a server request flushes a
 * response whose complete body and FIN have already been accepted by send;
 * an incomplete response is canceled.
 */
NETINS_UDP_PUBLIC void netins_udp_h3_request_config_init(
    netins_udp_h3_request_config_t *config);
NETINS_UDP_PUBLIC int netins_udp_h3_request_config_add_header(
    netins_udp_h3_request_config_t *config,
    const char *name, const char *value);
NETINS_UDP_PUBLIC void netins_udp_h3_response_config_init(
    netins_udp_h3_response_config_t *config);
NETINS_UDP_PUBLIC int netins_udp_h3_response_config_add_header(
    netins_udp_h3_response_config_t *config,
    const char *name, const char *value);
NETINS_UDP_PUBLIC int netins_udp_h3_request_open(netins_udp_t *netins,
    const netins_udp_h3_request_config_t *config,
    netins_udp_h3_request_t **out_request);
NETINS_UDP_PUBLIC int netins_udp_h3_request_accept(netins_udp_t *netins,
    netins_udp_h3_request_t **out_request);
NETINS_UDP_PUBLIC int netins_udp_h3_request_set_response(
    netins_udp_h3_request_t *request,
    const netins_udp_h3_response_config_t *config);
NETINS_UDP_PUBLIC ssize_t netins_udp_h3_request_send(
    netins_udp_h3_request_t *request, const void *data,
    size_t length, int flags);
NETINS_UDP_PUBLIC ssize_t netins_udp_h3_request_recv(
    netins_udp_h3_request_t *request, void *data,
    size_t length, int flags);
NETINS_UDP_PUBLIC int netins_udp_h3_request_get_info(
    netins_udp_h3_request_t *request,
    netins_udp_h3_request_info_t *out);
NETINS_UDP_PUBLIC int netins_udp_h3_request_get_last_error_code(
    netins_udp_h3_request_t *request);
NETINS_UDP_PUBLIC int netins_udp_h3_request_get_last_error_msg(
    netins_udp_h3_request_t *request, char *buffer, size_t buffer_len);
NETINS_UDP_PUBLIC int netins_udp_h3_request_get_fd(
    netins_udp_h3_request_t *request);
NETINS_UDP_PUBLIC int netins_udp_h3_request_get_events(
    netins_udp_h3_request_t *request, uint32_t *out_events);
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
 * pending notifications. get_events atomically drains the notification,
 * returns latched transitions plus current level conditions, then rearms the
 * descriptor before returning if any level condition is still true.
 *
 * Listener ACCEPTABLE and accepted-connection H3_REQUEST_ACCEPTABLE are
 * connection-level conditions. HTTP/3 request READABLE/WRITABLE are reported
 * by the request's own get_fd/get_events descriptor.
 */
NETINS_UDP_PUBLIC int netins_udp_get_fd(netins_udp_t *netins);
NETINS_UDP_PUBLIC int netins_udp_get_events(netins_udp_t *netins,
    uint32_t *out_events);

#ifdef __cplusplus
}
#endif

#endif /* NETINS_UDP_API_H */
