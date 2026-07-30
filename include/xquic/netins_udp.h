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
#define NETINS_UDP_MAX_INTERFACES 2U
#define NETINS_UDP_INTERFACE_NAME_LEN 64U
#define NETINS_UDP_FILE_PATH_LEN 1024U
#define NETINS_UDP_SERVER_NAME_LEN 256U
#define NETINS_UDP_ERROR_MSG_LEN 256U

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
    NETINS_UDP_OPT_KERNEL_RCVBUF = 9
} netins_udp_opt_t;

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
 * may only be changed while the handle is in NEW state.
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

NETINS_UDP_PUBLIC int netins_udp_get_state(netins_udp_t *handle);
NETINS_UDP_PUBLIC int netins_udp_shutdown(netins_udp_t *handle);
NETINS_UDP_PUBLIC void netins_udp_close(netins_udp_t *handle);

/*
 * Functions returning int use 0 for success and -1 for failure unless they
 * return an enum state. Failure sets errno and, for a valid handle, stores a
 * connection-level error code/message retrievable below. Handles are not
 * safe for concurrent calls; callers must serialize access to each handle
 * and to accepted handles sharing one server backend.
 */
NETINS_UDP_PUBLIC int netins_udp_get_last_error_code(
    netins_udp_t *handle);
NETINS_UDP_PUBLIC int netins_udp_get_last_error_msg(
    netins_udp_t *handle, char *buffer, size_t buffer_len);

#ifdef __cplusplus
}
#endif

#endif
