#ifndef NETINS_UDP_INTERNAL_H
#define NETINS_UDP_INTERNAL_H

#include <xquic/netins_udp.h>

#include "../../mini/mini_backend.h"

#if NETINS_UDP_MAX_INTERFACES > MINI_BACKEND_MAX_INTERFACES
#error "Public interface capacity exceeds Mini backend capacity"
#endif

#if NETINS_UDP_STREAM_LANE_COUNT != MINI_BACKEND_STREAM_LANE_COUNT
#error "Public and Mini backend stream lane counts must match"
#endif

#define NETINS_UDP_HANDLE_MAGIC 0x4e554450U
#define NETINS_UDP_STREAM_MAGIC 0x4e535452U

typedef enum netins_udp_tls_source_e {
    NETINS_UDP_TLS_SOURCE_NONE = 0,
    NETINS_UDP_TLS_SOURCE_BUILTIN = 1,
    NETINS_UDP_TLS_SOURCE_FILE = 2,
    NETINS_UDP_TLS_SOURCE_MEMORY = 3
} netins_udp_tls_source_t;

struct netins_udp_s {
    uint32_t magic;
    int role;
    int state;
    int last_error_code;
    char last_error_msg[NETINS_UDP_ERROR_MSG_LEN];
    int close_source;
    int close_system_error;
    int close_transport_error;
    char close_message[NETINS_UDP_ERROR_MSG_LEN];
    netins_udp_logger_cb logger;
    void *logger_user_data;
    int logger_minimum_level;

    uint32_t connect_timeout_ms;
    uint32_t accept_timeout_ms;
    uint32_t close_timeout_ms;
    mini_backend_config_t backend_config;
    int tls_source;
    int tls_certificate_fd;
    int tls_private_key_fd;
    int owns_tls_fds;

    mini_backend_client_t *client_backend;
    mini_backend_server_t *server_backend;
    mini_backend_server_connection_t *server_connection;
    struct netins_udp_stream_s *active_stream;
};

struct netins_udp_stream_s {
    uint32_t magic;
    netins_udp_t *connection;
    mini_backend_stream_t *backend_stream;
    uint32_t flags;
    netins_udp_stream_data_cb on_data;
    void *user_data;
};

#endif
