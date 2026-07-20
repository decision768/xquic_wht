
/**
 * @copyright Copyright (c) 2022, Alibaba Group Holding Limited
 */

#ifndef _XQC_ENGINE_H_INCLUDED_
#define _XQC_ENGINE_H_INCLUDED_


#include <xquic/xquic_typedef.h>
#include <xquic/xquic.h>
#include "src/tls/xqc_tls.h"
#include "src/common/xqc_list.h"
#include "src/transport/xqc_packet_in.h"

#define XQC_RESET_CNT_ARRAY_LEN 16384


typedef enum {
    XQC_ENG_FLAG_RUNNING    = 1 << 0,
    XQC_ENG_FLAG_NO_DESTROY = 1 << 1,
} xqc_engine_flag_t;

/*
 * Persistent receive-work contract.  The initial implementation is used by
 * the compatibility wrapper on the engine thread; ownership can later move to
 * a preallocated path ring without changing xqc_engine_packet_process().
 */
typedef struct xqc_rx_work_s {
    xqc_engine_t           *engine;
    unsigned char           packet_in_buf[XQC_MAX_PACKET_IN_LEN];
    size_t                  packet_in_size;
    struct sockaddr_storage local_addr;
    socklen_t               local_addrlen;
    struct sockaddr_storage peer_addr;
    socklen_t               peer_addrlen;
    xqc_usec_t              recv_time;
    void                   *user_data;
    xqc_bool_t              prepared;
} xqc_rx_work_t;


typedef struct xqc_alpn_registration_s {
    xqc_list_head_t             head;

    /* content of application layer protocol */
    char                       *alpn;

    /* length of alpn string */
    size_t                      alpn_len;

    /* Application-Layer-Protocol callback functions */
    xqc_app_proto_callbacks_t   ap_cbs;

    void                       *alp_ctx;

} xqc_alpn_registration_t;


typedef struct xqc_engine_s {
    /* for engine itself */
    xqc_engine_type_t               eng_type;
    xqc_engine_callback_t           eng_callback;
    xqc_engine_flag_t               eng_flag;

    /* for connections */
    xqc_config_t                   *config;
    xqc_str_hash_table_t           *conns_hash;             /* scid */
    xqc_str_hash_table_t           *conns_hash_dcid;        /* For reset packet */
    xqc_str_hash_table_t           *conns_hash_sr_token;    /* For stateless reset */
    xqc_pq_t                       *conns_active_pq;        /* In process */
    xqc_pq_t                       *conns_wait_wakeup_pq;   /* Need wakeup after next tick time */
    uint8_t                         reset_sent_cnt[XQC_RESET_CNT_ARRAY_LEN]; /* remote addr hash */
    xqc_usec_t                      reset_sent_cnt_cleared;

    /* tls context */
    xqc_tls_ctx_t                  *tls_ctx;

    xqc_log_t                      *log;
    xqc_random_generator_t         *rand_generator;

    /* for user */
    void                           *user_data;

    /* callback functions for connection transport events */
    xqc_transport_callbacks_t       transport_cbs;

    /* list of xqc_alpn_registration_t */
    xqc_list_head_t                 alpn_reg_list;

    xqc_conn_settings_t             default_conn_settings;

    char                            scid_buf[XQC_MAX_CID_LEN * 2 + 1];
    char                            dcid_buf[XQC_MAX_CID_LEN * 2 + 1];
    char                            sr_token_buf[XQC_STATELESS_RESET_TOKENLEN * 2 + 1];
    char                            conn_flag_str_buf[1024];
    char                            frame_type_buf[128];
    char                            local_addr_str[INET6_ADDRSTRLEN];
    char                            peer_addr_str[INET6_ADDRSTRLEN];

    void                           *priv_ctx;

} xqc_engine_t;



xqc_usec_t xqc_engine_wakeup_after(xqc_engine_t *engine);


/**
 * Create engine config.
 * @param engine_type  XQC_ENGINE_SERVER or XQC_ENGINE_CLIENT
 */
xqc_config_t *xqc_engine_config_create(xqc_engine_type_t engine_type);

void xqc_engine_config_destroy(xqc_config_t *config);


/**
 * @return > 0 : user should call xqc_engine_main_logic after N ms
 */
xqc_usec_t xqc_engine_wakeup_after(xqc_engine_t *engine);

void xqc_engine_wakeup_once(xqc_engine_t *engine);

xqc_connection_t *xqc_engine_conns_hash_find(xqc_engine_t *engine, const xqc_cid_t *cid, char type);

void xqc_engine_process_conn(xqc_connection_t *conn, xqc_usec_t now);

xqc_int_t xqc_engine_packet_prepare_path(xqc_engine_t *engine,
    const unsigned char *packet_in_buf, size_t packet_in_size,
    const struct sockaddr *local_addr, socklen_t local_addrlen,
    const struct sockaddr *peer_addr, socklen_t peer_addrlen,
    xqc_usec_t recv_time, void *user_data, xqc_rx_work_t *work);
xqc_int_t xqc_engine_commit_rx_work(xqc_engine_t *engine,
    xqc_rx_work_t *work);
void xqc_engine_release_rx_work(xqc_engine_t *engine, xqc_rx_work_t *work);

void xqc_engine_main_logic_internal(xqc_engine_t *engine);

void xqc_engine_conn_logic(xqc_engine_t *engine, xqc_connection_t *conn);

xqc_int_t xqc_engine_add_wakeup_queue(xqc_engine_t *engine, xqc_connection_t *conn);

xqc_int_t xqc_engine_remove_wakeup_queue(xqc_engine_t *engine, xqc_connection_t *conn);

xqc_int_t xqc_engine_add_active_queue(xqc_engine_t *engine, xqc_connection_t *conn);

xqc_int_t xqc_engine_remove_active_queue(xqc_engine_t *engine, xqc_connection_t *conn);

xqc_int_t xqc_engine_get_alpn_callbacks(xqc_engine_t *engine, const char *alpn,
    size_t alpn_len, xqc_app_proto_callbacks_t *cbs);

xqc_bool_t xqc_engine_is_sendmmsg_on(xqc_engine_t *engine, xqc_connection_t *conn);

#endif
