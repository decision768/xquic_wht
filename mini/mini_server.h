#ifndef XQC_MINI_SERVER_H
#define XQC_MINI_SERVER_H

#include <ctype.h>
#include <stdio.h>
#include <errno.h>
#include <memory.h>
#include <string.h>
#include <stdlib.h>
#include <xquic/xquic.h>
#include <event2/event.h>
#include <xquic/xqc_http3.h>
#include <xquic/xquic_typedef.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <zlog.h>

#include "common.h"
#include "../demo/xqc_hq.h"
#include "../tests/platform.h"
#include "mini_server_cb.h"
#define SERVER_DEFAULT_FILE "server_data.bin"
#define DEFAULT_IP   "10.12.54.143"
#define DEFAULT_PORT 8443
#define XQC_PACKET_BUF_LEN 65536
#define XQC_SOCKET_READ_BUDGET_PER_EVENT 32

/**
 * net config definition
 * net config is those arguments about socket information
 * all configuration on net should be put under this section
 */
typedef struct xqc_mini_svr_net_config_s
{
    /* server addr info */
    char    ip[64];
    unsigned short   port;

    /* idle persist timeout */
    int     conn_timeout;

    
    int     kernel_sndbuf;
    int     kernel_revbuf;
    size_t  user_send_buf_size;
    size_t  user_recv_buf_size;
    size_t  stream_range_size;

} xqc_mini_svr_net_config_t;

/**
 * quic config definition
 * quic config is those arguments required by quic features, including connection settings, ssl configs, etc.
 * all configuration on quic should be put under this section
 */
#define SESSION_TICKET_KEY_FILE     "session_ticket.key"
#define SESSION_TICKET_KEY_BUF_LEN  2048

typedef struct xqc_mini_svr_quic_config_s
{
    /* cipher config */
    char        ciphers[CIPHER_SUIT_LEN];
    char        groups[TLS_GROUPS_LEN];

    /* multipath */
    int         multipath;     // mp option, 0: disable, 1: enable
    char        mp_sched[32];  // mp scheduler, minrtt/backup

    /* congestion control */
    CC_TYPE     cc;             // cc algrithm, bbr/cubic


    /* server should load session ticket key to enable 0-RTT */
    char        session_ticket_key_data[SESSION_TICKET_KEY_BUF_LEN];
    size_t      session_ticket_key_len;
    
} xqc_mini_svr_quic_config_t;

/**
 * the environment config definition
 * environment config is those arguments about IO inputs and outputs
 * all configuration on environment should be put under this section
 */

#define LOG_PATH "slog.log"
#define KEY_PATH "skeys.log"
#define SOURCE_DIR  "."
#define PRIV_KEY_PATH "server.key"
#define CERT_PEM_PATH "server.crt"
#define MINI_SERVER_CONFIG_FILE "server_config.txt"

typedef struct xqc_mini_svr_env_config_s {
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

    /* directory for serving files */
    char data_dir[PATH_LEN];

    /* default file when client does not provide path */
    char default_send_file[PATH_LEN];
    /* generated response size for GET downloads; 0 means use file length */

    /* upload output file */
    char upload_path[PATH_LEN];


} xqc_mini_svr_env_config_t;

typedef struct xqc_mini_svr_args_s {
    /* network args */
    xqc_mini_svr_net_config_t    net_cfg;

    /* xquic args */
    xqc_mini_svr_quic_config_t   quic_cfg;

    /* environment args */
    xqc_mini_svr_env_config_t    env_cfg;

} xqc_mini_svr_args_t;

typedef struct xqc_mini_svr_ctx_s {
    struct event_base   *eb;

    /* used to remember fd type to send stateless reset */
    int                 current_fd;

    xqc_engine_t        *engine;    // xquic engine for current context
    struct event        *ev_engine;

    xqc_mini_svr_args_t *args;      // server arguments for current context

    int                 log_fd;
    int                 keylog_fd;

    zlog_category_t    *zlog_cat;
} xqc_mini_svr_ctx_t;


typedef struct xqc_mini_svr_user_conn_s {
    struct event           *ev_timeout;
    xqc_cid_t               cid;
    xqc_mini_svr_ctx_t     *ctx;

    /* ipv4 server */
    int                     fd;
    struct sockaddr_in     *local_addr;
    socklen_t               local_addrlen;
    struct event           *ev_socket;
    struct sockaddr_in     *peer_addr;
    socklen_t               peer_addrlen;

    uint64_t                last_mp_state_log_us;

} xqc_mini_svr_user_conn_t;


void xqc_mini_svr_init_ssl_config(xqc_engine_ssl_config_t  *ssl_cfg, xqc_mini_svr_args_t *args);

void xqc_mini_svr_init_args(xqc_mini_svr_args_t *args);

int xqc_mini_svr_init_ctx(xqc_mini_svr_ctx_t *ctx, xqc_mini_svr_args_t *args);
/**
 * @brief init engine & transport callbacks
 */
void xqc_mini_svr_init_callback(xqc_engine_callback_t *cb, xqc_transport_callbacks_t *tcb,
    xqc_mini_svr_args_t *args);
/**
 * @brief init xquic server engine
 */
int xqc_mini_svr_init_xquic_engine(xqc_mini_svr_ctx_t *ctx, xqc_mini_svr_args_t *args);

int xqc_mini_svr_init_env(xqc_mini_svr_ctx_t *ctx, xqc_mini_svr_args_t *args);

int xqc_mini_svr_init_engine_ctx(xqc_mini_svr_ctx_t *ctx, xqc_mini_svr_args_t *args);
int xqc_mini_svr_parse_cmd_args(xqc_mini_svr_args_t *args, int argc, char *argv[]);
xqc_mini_svr_user_conn_t * xqc_mini_svr_create_user_conn(xqc_mini_svr_ctx_t *ctx);
void xqc_mini_svr_init_conn_settings(xqc_engine_t *engine, xqc_mini_svr_args_t *args);

static int xqc_mini_svr_init_socket(int family, struct sockaddr *local_addr,
    socklen_t local_addrlen, const xqc_mini_svr_net_config_t *cfg);


static void xqc_mini_svr_socket_event_callback(int fd, short what, void *arg);
void xqc_mini_svr_free_ctx(xqc_mini_svr_ctx_t *ctx);
void xqc_mini_svr_free_user_conn(xqc_mini_svr_user_conn_t *user_conn);
#endif