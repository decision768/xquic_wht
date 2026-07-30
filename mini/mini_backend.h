#ifndef XQC_MINI_BACKEND_H
#define XQC_MINI_BACKEND_H

#include <stddef.h>
#include <stdint.h>

#define MINI_BACKEND_MAX_INTERFACES 2
#define MINI_BACKEND_INTERFACE_NAME_LEN 64
#define MINI_BACKEND_SCHEDULER_NAME_LEN 32
#define MINI_BACKEND_HOST_NAME_LEN 256
#define MINI_BACKEND_FILE_PATH_LEN 1024

typedef struct mini_backend_client_s mini_backend_client_t;
typedef struct mini_backend_server_s mini_backend_server_t;
typedef struct xqc_mini_svr_user_conn_s mini_backend_server_connection_t;

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
} mini_backend_config_t;

void mini_backend_config_init(mini_backend_config_t *config);

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
int mini_backend_client_is_connected(const mini_backend_client_t *client);
int mini_backend_client_is_closed(const mini_backend_client_t *client);
int mini_backend_client_last_error(const mini_backend_client_t *client);
int mini_backend_client_active_path_count(const mini_backend_client_t *client);

int mini_backend_server_create(const mini_backend_config_t *config,
    mini_backend_server_t **out_server);
int mini_backend_server_listen(mini_backend_server_t *server,
    const char *host, uint16_t port);
int mini_backend_server_run(mini_backend_server_t *server);
int mini_backend_server_poll(mini_backend_server_t *server);
int mini_backend_server_last_error(const mini_backend_server_t *server);
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
    const mini_backend_server_connection_t *connection);
int mini_backend_server_connection_is_closed(
    const mini_backend_server_connection_t *connection);
int mini_backend_server_connection_last_error(
    const mini_backend_server_connection_t *connection);

#endif
