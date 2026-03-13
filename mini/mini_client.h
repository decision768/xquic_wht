#ifndef XQC_MINI_CLIENT_H
#define XQC_MINI_CLIENT_H

#include <ctype.h>
#include <stdio.h>
#include <errno.h>
#include <memory.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <event2/event.h>
#include <xquic/xquic.h>
#include <xquic/xqc_http3.h>
#include <xquic/xquic_typedef.h>
#include <zlog.h>

#ifdef XQC_SYS_WINDOWS
#pragma comment(lib,"ws2_32.lib")
#pragma comment(lib,"event.lib")
#pragma comment(lib, "Iphlpapi.lib")
#pragma comment(lib, "Bcrypt.lib")
#include "../tests/getopt.h"
#else
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <ifaddrs.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netdb.h>
#include <getopt.h>
#endif


#include "../tests/platform.h"
#include "common.h"
#include "mini_client_cb.h"


#define DEFAULT_IP   "10.12.54.143"
#define DEFAULT_PORT 8443
#define DEFAULT_HOST "test.xquic.com"


#define SESSION_TICKET_BUF_MAX_SIZE 8192
#define TRANSPORT_PARAMS_MAX_SIZE 8192
#define TOKEN_MAX_SIZE 8192
#define MAX_PATH_CNT 2
#define XQC_MINI_MAX_STREAMS 16
#define XQC_PACKET_BUF_LEN 65536
#define XQC_MINI_PATH_ID_INVALID ((uint64_t)-1)
#define XQC_MINI_INTERFACE_NAME_MAX_LEN 64
#define XQC_SOCKET_READ_BUDGET_PER_EVENT 32
#define SESSION_TICKET_FILE         "session_ticket"
#define TRANSPORT_PARAMS_FILE       "transport_params"
#define TOKEN_FILE                  "token"

#define LOG_PATH "clog.log"
#define KEY_PATH "ckeys.log"
#define OUT_DIR  "."

/**
 * net config definition
 * net config is those arguments about socket information
 * all configuration on net should be put under this section
 */

typedef struct xqc_mini_cli_net_config_s {
    int                 conn_timeout;
    xqc_usec_t          last_socket_time;

    int                 multi_interface_cnt;
    char                multi_interface[MAX_PATH_CNT][XQC_MINI_INTERFACE_NAME_MAX_LEN];
    int                 kernel_sndbuf;
    int                 kernel_revbuf;
    size_t              user_send_buf_size;
    size_t              user_recv_buf_size;
    size_t              stream_range_size;



    // /* server addr info */
    // struct sockaddr    *addr;
    // socklen_t           addr_len;
    char                server_addr[64];
    unsigned short      server_port;
} xqc_mini_cli_net_config_t;

/**
 * quic config definition
 * quic config is those arguments required by quic features, including connection settings, ssl configs, etc.
 * all configuration on quic should be put under this section
 */

typedef struct xqc_mini_cli_quic_config_s {
    /* cipher config */
    char        ciphers[CIPHER_SUIT_LEN];
    char        groups[TLS_GROUPS_LEN];

    /* connection ssl config */
    char        session_ticket[SESSION_TICKET_BUF_MAX_SIZE];
    int         session_ticket_len;
    char        transport_parameter[TRANSPORT_PARAMS_MAX_SIZE];
    int         transport_parameter_len;

    char        token[TOKEN_MAX_SIZE];
    int         token_len;
    
    int         no_encryption;

    /* multipath */
    int         multipath;     // mp option, 0: disable, 1: enable
    char        mp_sched[32];  // mp scheduler, minrtt/backup

    /* congestion control */
    CC_TYPE     cc;             // cc algrithm, bbr/cubic
} xqc_mini_cli_quic_config_t;

/**
 * the environment config definition
 * environment config is those arguments about IO inputs and outputs
 * all configuration on environment should be put under this section
 */

typedef struct xqc_mini_cli_env_config_s {
    /* log config */
    char log_path[PATH_LEN];
    int use_zlog;
    char zlog_conf[PATH_LEN];
    char zlog_category[64];


    /* tls certificates */
    char private_key_file[PATH_LEN];
    char cert_file[PATH_LEN];

    /* key export */
    char key_out_path[PATH_LEN];

    /* output file */
    char out_file_dir[PATH_LEN];

    /* download target */
    char download_path[PATH_LEN];

     /* download request path */
    char download_target[PATH_LEN];

    /* upload source */
    char upload_path[PATH_LEN];

} xqc_mini_cli_env_config_t;


/**
 * the request config definition
 * request config is those arguments about request information
 * all configuration on request should be put under this section
 */
typedef struct xqc_mini_cli_req_config_s {
    char            path[RESOURCE_LEN];         /* request path */
    char            scheme[8];                  /* request scheme, http/https */
    REQUEST_METHOD  method;
    char            host[256];                  /* request host */
    // char            auth[AUTHORITY_LEN];
    char            url[URL_LEN];               /* original url */
} xqc_mini_cli_req_config_t;


typedef struct xqc_mini_cli_args_s {
    /* network args */
    xqc_mini_cli_net_config_t   net_cfg;

    /* xquic args */
    xqc_mini_cli_quic_config_t  quic_cfg;

    /* environment args */
    xqc_mini_cli_env_config_t   env_cfg;

    /* request args */
    xqc_mini_cli_req_config_t   req_cfg;

    /* request stream count */
    int                         req_stream_cnt;

    /* payload size for POST body; 0 means use existing file size */
    size_t                      send_data_len;
} xqc_mini_cli_args_t;


typedef struct xqc_mini_cli_ctx_s {
    struct event_base   *eb;

    xqc_mini_cli_args_t *args;      // client arguments for current context

    xqc_engine_t        *engine;    // xquic engine for current context
    struct event        *ev_engine;

    int                 log_fd;
    int                 keylog_fd;

    zlog_category_t    *zlog_cat;
    
} xqc_mini_cli_ctx_t;

struct xqc_mini_cli_user_conn_s;

typedef struct xqc_mini_cli_user_path_s {
    struct xqc_mini_cli_user_conn_s *user_conn;
    int                     fd;
    int                     get_local_addr;
    int                     is_active;
    int                     prepared;
    uint64_t                path_id;
    struct sockaddr        *local_addr;
    socklen_t               local_addrlen;
    struct sockaddr        *peer_addr;
    socklen_t               peer_addrlen;

    struct event            *ev_socket;
    char                    interface_name[XQC_MINI_INTERFACE_NAME_MAX_LEN];
    //用于测试路径是否失效
    int                     consecutive_write_failures;
    pthread_t               rx_thread;
    int                     rx_thread_running;

} xqc_mini_cli_user_path_t;

typedef struct xqc_mini_cli_rx_packet_s {
    int                             path_index;
    size_t                          data_len;
    xqc_usec_t                      recv_time;
    struct xqc_mini_cli_rx_packet_s *next;
    unsigned char                   data[];
} xqc_mini_cli_rx_packet_t;


typedef struct xqc_mini_cli_user_conn_s {
    xqc_cid_t               cid;
    xqc_h3_conn_t          *h3_conn;

    xqc_mini_cli_ctx_t     *ctx;

    xqc_mini_cli_user_path_t paths[MAX_PATH_CNT];
    int                     active_path_cnt;

    int                     target_requests;
    int                     max_concurrent_requests;
    int                     next_stream_index;
    int                     active_requests;
    int                     completed_requests;
    int                     total_requests_known;
    size_t                  send_file_size;
    char                    send_file_path[PATH_LEN];
    

    xqc_msec_t              upload_start_time;
    int                     upload_finished_streams;
    size_t                  upload_total_bytes;

    FILE                   *download_fp;
    char                    download_path[PATH_LEN];
    size_t                  download_total_bytes;
    size_t                  download_expected_bytes;
    size_t                  download_received_bytes;
    int                     download_progress_percent;
    int                     download_finished_streams;
    xqc_msec_t              download_start_time;

    int                     next_fallback_path_index;
    int                     handshake_finished;
    int                     requests_launched;
    int                     path_wait_rounds_after_handshake;
    xqc_usec_t              last_path_probe_ping_time;



    struct event            *ev_timeout;
    struct event            *ev_path_retry;
    struct event            *ev_rx_queue;

    int                     rx_notify_fds[2];
    int                     rx_threads_running;
    pthread_mutex_t         rx_queue_lock;
    xqc_mini_cli_rx_packet_t *rx_queue_head;
    xqc_mini_cli_rx_packet_t *rx_queue_tail;
    int                     rx_notify_pending;


} xqc_mini_cli_user_conn_t;

typedef struct xqc_mini_cli_user_stream_s {
    xqc_mini_cli_user_conn_t   *user_conn;
    int                         stream_index;

    /* send file*/
    FILE                  *send_body_fp;
    FILE                   *recv_body_fp;
    unsigned char          *send_buffer;
    size_t                 total_sent;
    size_t                 file_size;
    size_t                 buffered_len;
    size_t                 buffered_sent;
    size_t                 chunk_offset;
    size_t                 recv_offset;
    

    /* stat for IO */
    size_t                      send_body_len;
    size_t                      recv_body_len;
    size_t                      expected_content_length;
    int                         recv_fin;
    int                         slot_released;
    xqc_msec_t                  start_time;
    int                         send_finished;


    /* h3 request content */
    xqc_h3_request_t           *h3_request;

    xqc_http_headers_t          h3_hdrs;
    uint8_t                     hdr_sent;

    char                       *send_body_buff;
    int                         send_body_size;
    size_t                      send_offset;

    char                        header_content_length[32];
    char                        header_stream_index[16];
    char                        header_stream_count[16];
    char                        header_stream_offset[32];
    char                        header_total_size[32];
    
    char                        recv_file_path[PATH_LEN];
    int                         response_status;

} xqc_mini_cli_user_stream_t;



void xqc_mini_cli_init_engine_ssl_config(xqc_engine_ssl_config_t *ssl_cfg, xqc_mini_cli_args_t *args);
void xqc_mini_cli_try_rebuild_paths(xqc_mini_cli_user_conn_t *user_conn);

void xqc_mini_cli_init_callback(xqc_engine_callback_t *cb, xqc_transport_callbacks_t *tcb, xqc_mini_cli_args_t *args);

int xqc_mini_cli_init_xquic_engine(xqc_mini_cli_ctx_t *ctx, xqc_mini_cli_args_t *args);

void xqc_mini_cli_convert_text_to_sockaddr(int type,
    const char *addr_text, unsigned int port,
    struct sockaddr **saddr, socklen_t *saddr_len);

void xqc_mini_cli_init_args(xqc_mini_cli_args_t *args);

int xqc_mini_cli_init_ctx(xqc_mini_cli_ctx_t *ctx, xqc_mini_cli_args_t *args);

int xqc_mini_cli_init_env(xqc_mini_cli_ctx_t *ctx, xqc_mini_cli_args_t *args);

xqc_scheduler_callback_t xqc_mini_cli_get_sched_cb(xqc_mini_cli_args_t *args);
xqc_cong_ctrl_callback_t xqc_mini_cli_get_cc_cb(xqc_mini_cli_args_t *args);
void xqc_mini_cli_init_conn_settings(xqc_conn_settings_t *settings, xqc_mini_cli_args_t *args);

int xqc_mini_cli_init_alpn_ctx(xqc_mini_cli_ctx_t *ctx);
int xqc_mini_cli_init_engine_ctx(xqc_mini_cli_ctx_t *ctx);

void xqc_mini_cli_free_ctx(xqc_mini_cli_ctx_t *ctx);

void xqc_mini_cli_init_0rtt(xqc_mini_cli_args_t *args);

void xqc_mini_cli_init_conn_ssl_config(xqc_conn_ssl_config_t *conn_ssl_config, xqc_mini_cli_args_t *args);

int xqc_mini_cli_format_h3_req(xqc_http_header_t *headers,xqc_mini_cli_req_config_t* req_cfg, size_t body_len,xqc_mini_cli_user_stream_t *user_stream);

int xqc_mini_cli_request_send(xqc_h3_request_t *h3_request, xqc_mini_cli_user_stream_t *user_stream);

int xqc_mini_cli_send_h3_req(xqc_mini_cli_user_conn_t *user_conn, xqc_mini_cli_user_stream_t *user_stream, int stream_index);

int xqc_mini_cli_init_socket(xqc_mini_cli_user_path_t *user_path);

void xqc_mini_cli_socket_write_handler(xqc_mini_cli_user_path_t *user_path, int fd);

void xqc_mini_cli_socket_read_handler(xqc_mini_cli_user_path_t *user_path, int fd);

static void xqc_mini_cli_socket_event_callback(int fd, short what, void *arg);
int xqc_mini_cli_init_xquic_connection(xqc_mini_cli_user_conn_t *user_conn);

int xqc_mini_cli_main_process(xqc_mini_cli_user_conn_t *user_conn, xqc_mini_cli_ctx_t *ctx);
xqc_mini_cli_user_conn_t *xqc_mini_cli_user_conn_create(xqc_mini_cli_ctx_t *ctx);

void xqc_mini_cli_free_user_conn(xqc_mini_cli_user_conn_t *user_conn);
void xqc_mini_cli_on_connection_finish(xqc_mini_cli_user_conn_t *user_conn);

int xqc_mini_cli_parse_cmd_args(xqc_mini_cli_args_t *args, int argc, char *argv[]);

int xqc_mini_cli_get_target_path_count(xqc_mini_cli_user_conn_t *user_conn);

int xqc_mini_cli_launch_requests(xqc_mini_cli_user_conn_t *user_conn);
int xqc_mini_cli_try_schedule_requests(xqc_mini_cli_user_conn_t *user_conn);
#endif