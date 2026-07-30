#ifndef NETINS_UDP_INTERNAL_H
#define NETINS_UDP_INTERNAL_H

#include <xquic/netins_udp.h>

#include "../../mini/mini_backend.h"

#define NETINS_UDP_HANDLE_MAGIC 0x4e554450U

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

    mini_backend_client_t *client_backend;
    mini_backend_server_t *server_backend;
    mini_backend_server_connection_t *server_connection;
};

#endif
