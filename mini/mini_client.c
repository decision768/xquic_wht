#include "mini_client.h"
#include <inttypes.h>
#include <stdint.h>
#include <netdb.h>
#include <ctype.h>
#include <strings.h>
#include <poll.h>
#include <fcntl.h>
#include <limits.h>

#define CHUNK_SIZE (8 * 1024)
#define XQC_STREAM_RANGE_SIZE_DEFAULT (4 * 1024 * 1024)
#define CLIENT_SEND_FILE "client_sent.txt"  //used for send file path
#define CLIENT_RECV_FILE "client_received.bin" //used for receive file path
#define MINI_CONFIG_FILE "client_config.txt"

static void xqc_mini_cli_conn_ready_to_create_path(const xqc_cid_t *cid, void *conn_user_data);
static void xqc_mini_cli_path_removed(const xqc_cid_t *cid, uint64_t path_id, void *conn_user_data);

static void xqc_mini_cli_dump_path_bindings(xqc_mini_cli_user_conn_t *user_conn);
static int xqc_mini_cli_prepare_user_path(xqc_mini_cli_user_conn_t *user_conn,
    xqc_mini_cli_user_path_t *path);
static xqc_mini_cli_user_path_t *xqc_mini_cli_find_inactive_path(xqc_mini_cli_user_conn_t *user_conn);

static int xqc_mini_cli_bind_to_interface(int fd, const char *interface_name, int family);
static int xqc_mini_cli_set_local_addr(xqc_mini_cli_user_path_t *path);
static void xqc_mini_cli_format_addr_port(const struct sockaddr *addr, socklen_t addrlen,
    char *buf, size_t buflen);
static int xqc_mini_cli_load_config_file(xqc_mini_cli_args_t *args, const char *path);
static void *xqc_mini_cli_rx_thread_main(void *arg);
static int xqc_mini_cli_start_path_rx_thread(xqc_mini_cli_user_path_t *path);
static void xqc_mini_cli_stop_path_rx_thread(xqc_mini_cli_user_path_t *path);
static void xqc_mini_cli_rx_queue_event_callback(int fd, short what, void *arg);


static size_t
xqc_mini_cli_get_stream_range_size(const xqc_mini_cli_user_conn_t *user_conn)
{
    if (user_conn != NULL && user_conn->ctx != NULL
        && user_conn->ctx->args != NULL
        && user_conn->ctx->args->net_cfg.stream_range_size > 0) {
        return user_conn->ctx->args->net_cfg.stream_range_size;
    }

    return XQC_STREAM_RANGE_SIZE_DEFAULT;
}


int
xqc_mini_cli_try_schedule_requests(xqc_mini_cli_user_conn_t *user_conn)
{
    int ret;

    if (user_conn == NULL || user_conn->ctx == NULL) {
        return XQC_ERROR;
    }

    while (user_conn->active_requests < user_conn->max_concurrent_requests
        && user_conn->next_stream_index < user_conn->target_requests)
    {
        int stream_index = user_conn->next_stream_index;
        xqc_mini_cli_user_stream_t *user_stream = calloc(1, sizeof(xqc_mini_cli_user_stream_t));
        if (user_stream == NULL) {
            printf("[error] calloc user_stream failed for stream %d\n", stream_index);
            return XQC_ERROR;
        }

        ret = xqc_mini_cli_send_h3_req(user_conn, user_stream, stream_index);
        if (ret < 0) {
            free(user_stream);
            return XQC_ERROR;
        }

        user_conn->next_stream_index++;
        user_conn->active_requests++;
    }

    return XQC_OK;
}




//统计并输出所有流上传完成的汇总信息
static void
xqc_mini_cli_notify_upload_finished(xqc_mini_cli_user_stream_t *user_stream)
{
    if (user_stream->send_finished) {
        return;
    }

    xqc_mini_cli_user_conn_t *user_conn = user_stream->user_conn;

    user_stream->send_finished = 1;
    user_conn->upload_finished_streams++;
    user_conn->upload_total_bytes += user_stream->total_sent;

    if (user_conn->upload_finished_streams >= user_conn->target_requests) {
        xqc_usec_t end_time = xqc_now();
        double duration_ms = (user_conn->upload_start_time > 0)
            ? (end_time - user_conn->upload_start_time) / 1000.0
            : 0.0;
        double mbps = duration_ms > 0
            ? (user_conn->upload_total_bytes * 8.0) / (duration_ms * 1000.0)
            : 0.0;
        printf("[stats] all streams sent: %zu bytes, time=%.3f ms, speed=%.3f Mbps\n",
            user_conn->upload_total_bytes, duration_ms, mbps);
    }
}

//根据并发流索引计算对应的文件分片偏移与长度
static void
xqc_mini_cli_compute_stream_segment(const xqc_mini_cli_user_conn_t *user_conn,
    int stream_index, size_t *offset, size_t *length)
{
    size_t total_size = user_conn->send_file_size;
    size_t stream_range_size = xqc_mini_cli_get_stream_range_size(user_conn);

    if (stream_index < 0 ) {
        if (offset) {
            *offset = 0;
        }
        if (length) {
            *length = 0;
        }
        return;
    }

    size_t start = (size_t)stream_index * stream_range_size;
    if (start >= total_size) {
        if (offset) {
            *offset = total_size;
        }
        if (length) {
            *length = 0;
        }
        return;
    }

    // size_t base = stream_total > 0 ? total_size / (size_t)stream_total : 0;
    // size_t remainder = stream_total > 0 ? total_size % (size_t)stream_total : 0;

    // size_t start = base * (size_t)stream_index;
    // if (stream_index < (int)remainder) {
    //     start += (size_t)stream_index;
    // } else {
    //     start += remainder;
    // }

    // size_t len = base;
    // if (stream_index < (int)remainder) {
    //     len += 1;
    // }
    size_t remaining = total_size - start;
    size_t len = remaining < stream_range_size ? remaining : stream_range_size;


    if (offset) {
        *offset = start;
    }
    if (length) {
        *length = len;
    }
}
//引擎的ssl配置：这里应该是加密所使用算法和组别
void
xqc_mini_cli_init_engine_ssl_config(xqc_engine_ssl_config_t *ssl_cfg, xqc_mini_cli_args_t *args)
{
    ssl_cfg->ciphers = args->quic_cfg.ciphers;
    ssl_cfg->groups = args->quic_cfg.groups;
}

//初始化引擎与传输层回调函数
void
xqc_mini_cli_init_callback(xqc_engine_callback_t *cb, xqc_transport_callbacks_t *tcb, xqc_mini_cli_args_t *args)
{
    static xqc_engine_callback_t callback = {
        .set_event_timer = xqc_mini_cli_set_event_timer,
        // .log_callbacks = {
        //     .xqc_log_write_err = xqc_mini_cli_write_log_file,
        //     .xqc_log_write_stat = xqc_mini_cli_write_log_file,
        //     .xqc_qlog_event_write = xqc_mini_cli_write_qlog_file
        // },
        // .keylog_cb = xqc_mini_cli_keylog_cb,
        /* disable log/keylog output for the mini client */
        //.log_callbacks = {0},
        //.keylog_cb = NULL,
    };
    if (args->env_cfg.use_zlog) {
            callback.log_callbacks.xqc_log_write_err = xqc_mini_cli_write_log_file;
            callback.log_callbacks.xqc_log_write_stat = xqc_mini_cli_write_log_file;
            callback.log_callbacks.xqc_qlog_event_write = xqc_mini_cli_write_qlog_file;
            callback.keylog_cb = xqc_mini_cli_keylog_cb;
    } else {
    /* disable log/keylog output for the mini server */
        callback.log_callbacks = (xqc_log_callbacks_t){0};
        //callback.log_callbacks.xqc_qlog_event_write = xqc_mini_cli_write_qlog_file;
        callback.keylog_cb = NULL;
    }
    static xqc_transport_callbacks_t transport_cbs = {
        .write_socket = xqc_mini_cli_write_socket,
        .write_socket_ex = xqc_mini_cli_write_socket_ex,
        .save_token = xqc_mini_cli_save_token,
        .save_session_cb = xqc_mini_cli_save_session_cb,
        .save_tp_cb = xqc_mini_cli_save_tp_cb,
        .ready_to_create_path_notify = xqc_mini_cli_conn_ready_to_create_path,
        .path_removed_notify = xqc_mini_cli_path_removed,
    };

    *cb = callback;
    *tcb = transport_cbs;
}
//初始化并创建 xquic 客户端引擎
int
xqc_mini_cli_init_xquic_engine(xqc_mini_cli_ctx_t *ctx, xqc_mini_cli_args_t *args)
{
    int ret;
    xqc_config_t egn_cfg;
    xqc_engine_callback_t callback;
    xqc_engine_ssl_config_t ssl_cfg = {0};
    xqc_transport_callbacks_t transport_cbs;
    
    /* get default parameters of xquic engine */
    ret = xqc_engine_get_default_config(&egn_cfg, XQC_ENGINE_CLIENT);
    if (ret < 0) {
        return XQC_ERROR;
    }

    /* init ssl config */
    xqc_mini_cli_init_engine_ssl_config(&ssl_cfg, args);

    /* init engine & transport callbacks */
    xqc_mini_cli_init_callback(&callback, &transport_cbs, args);

    /* create client engine */
    ctx->engine = xqc_engine_create(XQC_ENGINE_CLIENT, &egn_cfg, &ssl_cfg,
                                    &callback, &transport_cbs, ctx);
    if (ctx->engine == NULL) {
        printf("[error] xqc_engine_create error\n");
        return XQC_ERROR;
    }

    ctx->ev_engine = event_new(ctx->eb, -1, 0, xqc_mini_cli_engine_cb, ctx);
    return XQC_OK;
}
//转输入地址和端口为sockaddr_in
void
xqc_mini_cli_convert_text_to_sockaddr(int type,
    const char *addr_text, unsigned int port,
    struct sockaddr **saddr, socklen_t *saddr_len)
{
    *saddr = calloc(1, sizeof(struct sockaddr_in));
    struct sockaddr_in *addr_v4 = (struct sockaddr_in *)(*saddr);
    inet_pton(type, addr_text, &(addr_v4->sin_addr.s_addr));
    
    addr_v4->sin_family = type;
    addr_v4->sin_port = htons(port);
    *saddr_len = sizeof(struct sockaddr_in);
    
}
//初始化客户端默认参数
void
xqc_mini_cli_init_args(xqc_mini_cli_args_t *args)
{
    /* init network args */
    args->net_cfg.conn_timeout = 20;
    args->net_cfg.multi_interface_cnt = 0;
    for (int i = 0; i < MAX_PATH_CNT; i++) {
        memset(args->net_cfg.multi_interface[i], 0, sizeof(args->net_cfg.multi_interface[i]));
    }
    args->net_cfg.kernel_sndbuf = 16 * 1024 * 1024;
    args->net_cfg.kernel_revbuf = 16 * 1024 * 1024;
    args->net_cfg.user_send_buf_size = CHUNK_SIZE;
    args->net_cfg.user_recv_buf_size = XQC_PACKET_BUF_LEN;
    args->net_cfg.stream_range_size = XQC_STREAM_RANGE_SIZE_DEFAULT;

    strncpy(args->net_cfg.server_addr, DEFAULT_IP, sizeof(args->net_cfg.server_addr) - 1);
    args->net_cfg.server_addr[sizeof(args->net_cfg.server_addr) - 1] = '\0';
    args->net_cfg.server_port = DEFAULT_PORT;
    /**
     * init quic config
     * it's recommended to replace the constant value with option arguments according to actual needs
     * XQC_TLS_CIPHERS和XQC_TLS_GROUPS对应加密算法，在xquic.h里面
     * 这里的设置多路径调度算法为minrtt
     */
    strncpy(args->quic_cfg.ciphers, XQC_TLS_CIPHERS, CIPHER_SUIT_LEN - 1);
    strncpy(args->quic_cfg.groups, XQC_TLS_GROUPS, TLS_GROUPS_LEN - 1);
    args->quic_cfg.multipath = 1;
    strncpy(args->quic_cfg.mp_sched, "balanced", sizeof(args->quic_cfg.mp_sched));
    args->quic_cfg.cc = CC_TYPE_BBR;

    /* init environmen args */
    // args->env_cfg.log_level = XQC_LOG_DEBUG;
    //定义了输出的日志相关内容
    strncpy(args->env_cfg.log_path, LOG_PATH, sizeof(args->env_cfg.log_path));
    args->env_cfg.use_zlog = 1;
    strncpy(args->env_cfg.zlog_conf, "zlog.conf", sizeof(args->env_cfg.zlog_conf));
    strncpy(args->env_cfg.zlog_category, "xquic_mini_client", sizeof(args->env_cfg.zlog_category));


    strncpy(args->env_cfg.out_file_dir, OUT_DIR, sizeof(args->env_cfg.out_file_dir));
    strncpy(args->env_cfg.key_out_path, KEY_PATH, sizeof(args->env_cfg.key_out_path));
    strncpy(args->env_cfg.download_path, CLIENT_RECV_FILE, sizeof(args->env_cfg.download_path));
    args->env_cfg.download_target[0] = '\0';
    strncpy(args->env_cfg.upload_path, CLIENT_SEND_FILE, sizeof(args->env_cfg.upload_path));

    /* init request args */
    /*
    *  请求直接设计为GET方法，后续应该对其进行更改，协议种类为https
    */
    args->req_cfg.method = REQUEST_METHOD_POST;   
    strncpy(args->req_cfg.scheme, "https", sizeof(args->req_cfg.scheme));
    strncpy(args->req_cfg.path, "/", sizeof(args->req_cfg.path));
    strncpy(args->req_cfg.url, "/", sizeof(args->req_cfg.url));
    strncpy(args->req_cfg.host, DEFAULT_HOST, sizeof(args->req_cfg.host));
    args->req_stream_cnt = 1;
    args->send_data_len = 0;
}

//初始化客户端上下文（事件循环、日志输出fd等）
int
xqc_mini_cli_init_ctx(xqc_mini_cli_ctx_t *ctx, xqc_mini_cli_args_t *args)
{
    memset(ctx, 0, sizeof(xqc_mini_cli_ctx_t));

    /* init event base */
    struct event_base *eb = event_base_new();
    ctx->eb = eb;

    ctx->args = args;

    // /* init log writer fd */
    // ctx->log_fd = xqc_mini_cli_open_log_file(ctx);
    // if (ctx->log_fd < 0) {
    //     printf("[error] open log file failed\n");
    //     return XQC_ERROR;
    // }

    ctx->zlog_cat = NULL;
    ctx->log_fd = 0;
    if (ctx->args->env_cfg.use_zlog) {
        if (zlog_init(ctx->args->env_cfg.zlog_conf) != 0) {
            printf("[error] zlog_init failed: %s\n", ctx->args->env_cfg.zlog_conf);
            return XQC_ERROR;
        }
        ctx->zlog_cat = zlog_get_category(ctx->args->env_cfg.zlog_category);
        if (ctx->zlog_cat == NULL) {
            printf("[error] zlog_get_category failed: %s\n", ctx->args->env_cfg.zlog_category);
            zlog_fini();
            return XQC_ERROR;
        }
    }
    if (!ctx->args->env_cfg.use_zlog || ctx->zlog_cat == NULL) {
        // /* init log writer fd */
        ctx->log_fd = xqc_mini_cli_open_log_file(ctx);
        if (ctx->log_fd < 0) {
            printf("[error] open log file failed\n");
            return XQC_ERROR;
        }
    }


    /* init keylog writer fd */
    ctx->keylog_fd = xqc_mini_cli_open_keylog_file(ctx);
    if (ctx->keylog_fd < 0) {
        printf("[error] open keylog file failed\n");
        return XQC_ERROR;
    }

    return 0;
}


// 初始化客户端环境（参数、配置文件与上下文）
int
xqc_mini_cli_init_env(xqc_mini_cli_ctx_t *ctx, xqc_mini_cli_args_t *args)
{
    int ret = XQC_OK;

    /* init client args */
    xqc_mini_cli_init_args(args);

    /* load config file defaults if present */
    xqc_mini_cli_load_config_file(args, MINI_CONFIG_FILE);
    
    /* init client ctx */
    ret = xqc_mini_cli_init_ctx(ctx, args);

    return ret;
}
// 根据配置选择多路径调度回调
xqc_scheduler_callback_t
xqc_mini_cli_get_sched_cb(xqc_mini_cli_args_t *args)
{
    xqc_scheduler_callback_t sched = xqc_minrtt_scheduler_cb;
    if (strncmp(args->quic_cfg.mp_sched, "minrtt", strlen("minrtt")) == 0) {
        sched = xqc_minrtt_scheduler_cb;

    } else if (strncmp(args->quic_cfg.mp_sched, "backup", strlen("backup")) == 0) {
        sched = xqc_backup_scheduler_cb;
    }
    else if (strncmp(args->quic_cfg.mp_sched, "balanced", strlen("balanced")) == 0) {
        sched = xqc_balanced_scheduler_cb;
    }
    else if (strncmp(args->quic_cfg.mp_sched, "rap", strlen("rap")) == 0) {
        sched = xqc_rap_scheduler_cb;
    }
    else if (strncmp(args->quic_cfg.mp_sched, "act", strlen("act")) == 0) {
        sched = xqc_act_scheduler_cb;
    }
    else if (strncmp(args->quic_cfg.mp_sched, "bw", strlen("bw")) == 0) {
        sched = xqc_bandwidth_scheduler_cb;
    }
    return sched;
}
// 根据配置选择拥塞控制回调
xqc_cong_ctrl_callback_t
xqc_mini_cli_get_cc_cb(xqc_mini_cli_args_t *args)
{
    xqc_cong_ctrl_callback_t ccc = xqc_bbr_cb;
    switch (args->quic_cfg.cc) {
    case CC_TYPE_BBR:
        ccc = xqc_bbr_cb;
        break;
    case CC_TYPE_CUBIC:
        ccc = xqc_cubic_cb;
        break;
    default:
        break;
    }
    return ccc;
}

// 初始化连接层设置（拥塞控制、多路径等）
void
xqc_mini_cli_init_conn_settings(xqc_conn_settings_t *settings, xqc_mini_cli_args_t *args)
{
    /* parse congestion control callback */
    xqc_cong_ctrl_callback_t ccc = xqc_mini_cli_get_cc_cb(args);
    /* parse mp scheduler callback */
    xqc_scheduler_callback_t sched = xqc_mini_cli_get_sched_cb(args);

    /* init connection settings */
    memset(settings, 0, sizeof(xqc_conn_settings_t));
    settings->cong_ctrl_callback = ccc;
    settings->cc_params.customize_on = 1;
    settings->cc_params.init_cwnd = 128;
    //settings->so_sndbuf = 1024*1024;
    settings->proto_version = XQC_VERSION_V1;
    settings->spurious_loss_detect_on = 1;
    settings->scheduler_callback = sched;
    settings->reinj_ctl_callback = xqc_deadline_reinj_ctl_cb;
    settings->adaptive_ack_frequency = 0;
    settings->ack_frequency = 1;      
    // settings->max_ack_delay = 5;
    //settings->mp_ack_on_any_path = 1;

    settings->enable_multipath = args->quic_cfg.multipath;
    //settings->enable_stream_rate_limit = 1;
    settings->init_recv_window = 64*1024*1024;
    settings->multipath_version = XQC_MULTIPATH_10;
    settings->recv_rate_bytes_per_sec = 0;
    //settings->max_datagram_frame_size = 1350;
    settings->mp_ping_on = 1;
    settings->mp_enable_reinjection = 0;
}
// 初始化 HTTP3 回调与上下文
int
xqc_mini_cli_init_alpn_ctx(xqc_mini_cli_ctx_t *ctx)
{
    int ret = XQC_OK;

    /* init http3 callbacks */
    xqc_h3_callbacks_t h3_cbs = {
        .h3c_cbs = {
            .h3_conn_create_notify = xqc_mini_cli_h3_conn_create_notify,
            .h3_conn_close_notify = xqc_mini_cli_h3_conn_close_notify,
            .h3_conn_handshake_finished = xqc_mini_cli_h3_conn_handshake_finished,
        },
        .h3r_cbs = {
            .h3_request_create_notify = xqc_mini_cli_h3_request_create_notify,
            .h3_request_close_notify = xqc_mini_cli_h3_request_close_notify,
            .h3_request_read_notify = xqc_mini_cli_h3_request_read_notify,
            .h3_request_write_notify = xqc_mini_cli_h3_request_write_notify,
        }
    };

    /* init http3 context */
    ret = xqc_h3_ctx_init(ctx->engine, &h3_cbs);
    if (ret != XQC_OK) {
        printf("init h3 context error, ret: %d\n", ret);
        return ret;
    }

    return ret;
}
// 初始化引擎上下文（ALPN 等）
int
xqc_mini_cli_init_engine_ctx(xqc_mini_cli_ctx_t *ctx)
{
    int ret;

    /* init alpn ctx */
    ret = xqc_mini_cli_init_alpn_ctx(ctx);

    return ret;
}
// 释放客户端上下文资源
void
xqc_mini_cli_free_ctx(xqc_mini_cli_ctx_t *ctx)
{
    xqc_mini_cli_close_keylog_file(ctx);
    xqc_mini_cli_close_log_file(ctx);

    if (ctx->args && ctx->args->env_cfg.use_zlog && ctx->zlog_cat != NULL) {
        zlog_fini();
        ctx->zlog_cat = NULL;
    }
    
    if (ctx->args) {
        free(ctx->args);
        ctx->args = NULL;
    }
}
// 初始化 0-RTT（加密的） 相关数据（ticket/TP/token）
void
xqc_mini_cli_init_0rtt(xqc_mini_cli_args_t *args)
{
    /* read session ticket */
    int ret = xqc_mini_read_file_data(args->quic_cfg.session_ticket,
        SESSION_TICKET_BUF_MAX_SIZE, SESSION_TICKET_FILE);
    args->quic_cfg.session_ticket_len = ret > 0 ? ret : 0;

    /* read transport params */
    ret = xqc_mini_read_file_data(args->quic_cfg.transport_parameter,
        TRANSPORT_PARAMS_MAX_SIZE, TRANSPORT_PARAMS_FILE);
    args->quic_cfg.transport_parameter_len = ret > 0 ? ret : 0;

    /* read token */
    ret = xqc_mini_cli_read_token(
        args->quic_cfg.token, TOKEN_MAX_SIZE);
    args->quic_cfg.token_len = ret > 0 ? ret : 0;
}
// 初始化连接级 SSL 配置
void
xqc_mini_cli_init_conn_ssl_config(xqc_conn_ssl_config_t *conn_ssl_config, xqc_mini_cli_args_t *args)
{
    /* set session ticket and transport parameter args */
    if (args->quic_cfg.session_ticket_len < 0 || args->quic_cfg.transport_parameter_len < 0) {
        conn_ssl_config->session_ticket_data = NULL;
        conn_ssl_config->transport_parameter_data = NULL;

    } else {
        conn_ssl_config->session_ticket_data = args->quic_cfg.session_ticket;
        conn_ssl_config->session_ticket_len = args->quic_cfg.session_ticket_len;
        conn_ssl_config->transport_parameter_data = args->quic_cfg.transport_parameter;
        conn_ssl_config->transport_parameter_data_len = args->quic_cfg.transport_parameter_len;
    }
}

// 组装 HTTP/3 请求头
int 
xqc_mini_cli_format_h3_req(xqc_http_header_t *headers,
    xqc_mini_cli_req_config_t* req_cfg, size_t body_len,
    xqc_mini_cli_user_stream_t *user_stream)
{
    /* response header buf list */
    
    if (user_stream == NULL || user_stream->user_conn == NULL) {
        printf("[error] invalid user_stream for header formatting\n");
        return XQC_ERROR;
    }
    snprintf(user_stream->header_content_length,
        sizeof(user_stream->header_content_length), "%zu", body_len);
    snprintf(user_stream->header_stream_index,
        sizeof(user_stream->header_stream_index), "%d", user_stream->stream_index);
    snprintf(user_stream->header_stream_count,
        sizeof(user_stream->header_stream_count), "%d",
        user_stream->user_conn->target_requests);
    snprintf(user_stream->header_stream_offset,
        sizeof(user_stream->header_stream_offset), "%zu", user_stream->chunk_offset);
    snprintf(user_stream->header_total_size,
        sizeof(user_stream->header_total_size), "%zu",
        user_stream->user_conn->send_file_size);

    printf(">>>send_body_total_size:%s\n", user_stream->header_content_length);

    xqc_http_header_t req_hdr[] = {
        {
            .name = {.iov_base = ":method", .iov_len = 7},
            .value = {.iov_base = method_s[req_cfg->method], .iov_len = strlen(method_s[req_cfg->method])},
            .flags = 0,
        },
        {
            .name = {.iov_base = ":scheme", .iov_len = 7},
            .value = {.iov_base = req_cfg->scheme, .iov_len = strlen(req_cfg->scheme)},
            .flags = 0,
        },
        {
            .name   = {.iov_base = "host", .iov_len = 4},
            .value  = {.iov_base = req_cfg->host, .iov_len = strlen(req_cfg->host)},
            .flags  = 0,
        },
        {
            .name = {.iov_base = ":path", .iov_len = 5},
            .value = {.iov_base = req_cfg->url, .iov_len = strlen(req_cfg->url)},
            .flags = 0,
        },
        {
            .name   = {.iov_base = "content-type", .iov_len = 12},
            .value  = {.iov_base = "text/plain", .iov_len = 10},
            .flags  = 0,
        },
        {
            .name   = {.iov_base = "content-length", .iov_len = 14},
            .value  = {.iov_base = user_stream->header_content_length,
                .iov_len = strlen(user_stream->header_content_length)},
            .flags  = 0,
        },
        {
            .name = {.iov_base = "x-stream-index", .iov_len = strlen("x-stream-index")},
            .value = {.iov_base = user_stream->header_stream_index,
                .iov_len = strlen(user_stream->header_stream_index)},
            .flags = 0,
        },
        {
            .name = {.iov_base = "x-stream-count", .iov_len = strlen("x-stream-count")},
            .value = {.iov_base = user_stream->header_stream_count,
                .iov_len = strlen(user_stream->header_stream_count)},
            .flags = 0,
        },
        {
            .name = {.iov_base = "x-stream-offset", .iov_len = strlen("x-stream-offset")},
            .value = {.iov_base = user_stream->header_stream_offset,
                .iov_len = strlen(user_stream->header_stream_offset)},
            .flags = 0,
        },
        {
            .name = {.iov_base = "x-total-length", .iov_len = strlen("x-total-length")},
            .value = {.iov_base = user_stream->header_total_size,
                .iov_len = strlen(user_stream->header_total_size)},
            .flags = 0,
        },
    };

    size_t req_sz = sizeof(req_hdr) / sizeof(req_hdr[0]);
    if (req_sz > H3_HDR_CNT) {
        printf("[error] header length is too large, request_size: %zd\n", req_sz);
        return XQC_ERROR;
    }

    for (size_t i = 0; i < req_sz; i++) {
        headers[i] = req_hdr[i];
    }
    
    return req_sz;
}
// 实际分片发送 HTTP/3 请求体
int
xqc_mini_cli_request_send(xqc_h3_request_t *h3_request, xqc_mini_cli_user_stream_t *user_stream)
{
    

    // 发送 POST Body，最后一块要 fin = 1
    // ret = xqc_h3_request_send_body(h3_request,
    //                                (unsigned char *)body,
    //                                body_len,
    //                                1);
    // if (ret < 0) {
    //     printf("[error] send body failed: %d\n", ret);
    //     return -1;
    // }
    if (user_stream->send_finished) {
        return XQC_OK;
    }

    unsigned char *buffer = user_stream->send_buffer;
    xqc_mini_cli_ctx_t *ctx = user_stream->user_conn->ctx;
    size_t buffer_capacity = ctx->args->net_cfg.user_send_buf_size > 0
        ? ctx->args->net_cfg.user_send_buf_size
        : CHUNK_SIZE;

    if (user_stream->file_size == 0) {
        ssize_t n = xqc_h3_request_send_body(h3_request, NULL, 0, 1);
        if (n == -XQC_EAGAIN) {
            return XQC_OK;
        }

        if (n < 0) {
            printf("[error] send zero-length body failed: %zd\n", n);
            return -1;
        }
        printf("[stream %d] zero-length segment sent, offset %zu\n",
            user_stream->stream_index, user_stream->chunk_offset);
        xqc_mini_cli_notify_upload_finished(user_stream);
        return XQC_OK;
    }
    if (!buffer) {
        perror("malloc");
        return -1;
    }
    while (user_stream->total_sent < user_stream->file_size
        || user_stream->buffered_sent < user_stream->buffered_len) {
        
         if (user_stream->buffered_sent == user_stream->buffered_len) {
            if (user_stream->send_body_fp == NULL) {
                printf("[error] send file is not opened for stream %d\n",
                    user_stream->stream_index);
                return -1;
            }
            if (fseek(user_stream->send_body_fp,
                    (long)(user_stream->chunk_offset + user_stream->total_sent), SEEK_SET) != 0) {
                perror("fseek");
                return -1;
            }

            
            size_t remaining = user_stream->file_size - user_stream->total_sent;
            size_t chunk = remaining < buffer_capacity ? remaining : buffer_capacity;

            if (chunk == 0) {
                break;
            }
            user_stream->buffered_len = fread(buffer, 1, chunk, user_stream->send_body_fp);
            user_stream->buffered_sent = 0;
            if (user_stream->buffered_len == 0) {
                if (feof(user_stream->send_body_fp)) {
                    if (user_stream->total_sent < user_stream->file_size) {
                        printf("[error] unexpected EOF after %zu/%zu bytes\n",
                               user_stream->total_sent, user_stream->file_size);
                        return -1;
                    }
                    break;
                }
                if (ferror(user_stream->send_body_fp)) {
                    perror("fread");
                    return -1;
                }
            }

        }
        
        size_t bytes_left_in_buffer = user_stream->buffered_len - user_stream->buffered_sent;
        

        size_t prev_total_sent = user_stream->total_sent;
        size_t prev_buffered_sent = user_stream->buffered_sent;

         ssize_t n = xqc_h3_request_send_body(h3_request,
            buffer + user_stream->buffered_sent, bytes_left_in_buffer, 0);
        if (n == -XQC_EAGAIN) {
            //ctx->args->net_cfg.last_socket_time = xqc_now();
            //printf("[info] send paused at start, waiting for write_notify\n");
            //break;
            xqc_engine_main_logic(user_stream->user_conn->ctx->engine);
            if (user_stream->total_sent == prev_total_sent
                && user_stream->buffered_sent == prev_buffered_sent) {
                break;
            }
            continue;
        }
        if (n < 0) {
            printf("[error] send body failed: %zd\n", n);
            return -1;
        }
        user_stream->buffered_sent += n;
        user_stream->total_sent += n;

        // printf("[stream %d] sent %zu/%zu bytes from offset %zu.\n",
        //     user_stream->stream_index,
        //     user_stream->total_sent,
        //     user_stream->file_size,
        //     user_stream->chunk_offset);
        ctx->args->net_cfg.last_socket_time = xqc_now();
        //xqc_engine_main_logic(user_stream->user_conn->ctx->engine);

        if (user_stream->buffered_sent < user_stream->buffered_len) {
            continue;
        }

        if (user_stream->total_sent >= user_stream->file_size
            && user_stream->buffered_sent >= user_stream->buffered_len) {
            break;
        }
    }

    if (user_stream->total_sent >= user_stream->file_size
        && !user_stream->send_finished) {
        ssize_t fin_ret = xqc_h3_request_send_body(h3_request, NULL, 0, 1);
        if (fin_ret == -XQC_EAGAIN) {
            return XQC_OK;
        }
        if (fin_ret < 0) {
            printf("[error] send fin failed: %zd\n", fin_ret);
            return -1;
        }
        xqc_usec_t end_time = xqc_now();
        double duration_ms = (user_stream->start_time > 0)
            ? (end_time - user_stream->start_time) / 1000.0
            : 0.0;
        double mbps = duration_ms > 0
            ? (user_stream->total_sent * 8.0) / (duration_ms * 1000.0)
            : 0.0;
        printf("[stream %d at offset %zu] send finished: %zu bytes, time=%.3f ms, speed=%.3f Mbps\n",
            user_stream->stream_index, user_stream->chunk_offset, user_stream->total_sent, duration_ms, mbps);
        xqc_mini_cli_notify_upload_finished(user_stream);
    }

    return XQC_OK;
}

// 创建并发送 HTTP/3 请求（头与体），调用xqc_mini_cli_request_send完成请求体的分片发送
int
xqc_mini_cli_send_h3_req(xqc_mini_cli_user_conn_t *user_conn, xqc_mini_cli_user_stream_t *user_stream, int stream_index)
{
    user_stream->user_conn = user_conn;
    user_stream->stream_index = stream_index;
    size_t chunk_offset = 0;
    size_t chunk_length = 0;
    xqc_mini_cli_compute_stream_segment(user_conn, stream_index, &chunk_offset, &chunk_length);
    user_stream->chunk_offset = chunk_offset;
    user_stream->file_size = chunk_length;

    xqc_stream_settings_t settings = { .recv_rate_bytes_per_sec = 0 };
    user_stream->h3_request = xqc_h3_request_create(user_conn->ctx->engine, &user_conn->cid,
        &settings, user_stream);
    if (user_stream->h3_request == NULL) {
        printf("[error] xqc_h3_request_create error\n");
        return XQC_ERROR;
    }

    int ret, fin;
    /* send packet header/body */
    xqc_http_header_t header[H3_HDR_CNT];
    xqc_mini_cli_req_config_t* req_cfg;

    req_cfg = &user_stream->user_conn->ctx->args->req_cfg;
    

    user_stream->send_body_fp = NULL;
    user_stream->send_buffer = NULL;
    user_stream->total_sent = 0;
    user_stream->response_status = 0;

    if (req_cfg->method == REQUEST_METHOD_POST) {
        FILE *fp = fopen(user_conn->send_file_path, "rb");
        if (!fp) {
            perror("fopen");
            return -1;
        }
        user_stream->send_body_fp = fp;
    }


    if (req_cfg->method == REQUEST_METHOD_GET) {
        user_stream->recv_offset = user_stream->chunk_offset;
        user_stream->recv_file_path[0] = '\0';
    }
    
    if (req_cfg->method == REQUEST_METHOD_POST) {
        printf("[stream %d] segment offset=%zu length=%zu of total %zu bytes\n",
            stream_index, user_stream->chunk_offset, user_stream->file_size,
            user_conn->send_file_size);
    }

    ret = xqc_mini_cli_format_h3_req(header, req_cfg, user_stream->file_size, user_stream);
    if (ret > 0) {
        user_stream->h3_hdrs.headers = header;
        user_stream->h3_hdrs.count = ret;
        if (user_conn->upload_start_time == 0) {
            user_conn->upload_start_time = xqc_now();
        }
        if (user_stream->start_time == 0) {
            user_stream->start_time = xqc_now();
        }
        /* send header: GET has no request body, send FIN with headers */
        uint8_t hdr_fin = (req_cfg->method == REQUEST_METHOD_GET) ? 1 : 0;
        ret = xqc_h3_request_send_headers(user_stream->h3_request, &user_stream->h3_hdrs, hdr_fin);

        if (ret < 0) {
            printf("[error] xqc_mini_cli_h3_request_send error %d\n", ret);
            return -1;
        } else {
            printf("[stats] xqc_mini_cli_h3_request_send success \n");
            user_stream->hdr_sent = 1;
        }
    }
    if (req_cfg->method == REQUEST_METHOD_GET) {
        return XQC_OK;
    }
    if (user_stream->file_size > 0) {
        size_t buffer_capacity = user_conn->ctx->args->net_cfg.user_send_buf_size > 0
            ? user_conn->ctx->args->net_cfg.user_send_buf_size
            : CHUNK_SIZE;
        user_stream->send_buffer = malloc(buffer_capacity);

        if (user_stream->send_buffer == NULL) {
            perror("malloc");
            return -1;
        }
    }
    user_stream->buffered_len = 0;
    user_stream->buffered_sent = 0;
    user_stream->start_time = xqc_now();
    xqc_mini_cli_request_send(user_stream->h3_request, user_stream);

    /* generate engine main log to send packets */
    xqc_engine_main_logic(user_conn->ctx->engine);
    return XQC_OK;
}

// 根据路径索引获取要绑定的网卡名称
static const char *
xqc_mini_cli_get_interface_for_path(xqc_mini_cli_user_conn_t *user_conn, int path_index)
{
    xqc_mini_cli_net_config_t *net_cfg = &user_conn->ctx->args->net_cfg;
    if (path_index < 0 || path_index >= net_cfg->multi_interface_cnt) {
        return NULL;
    }

    if (net_cfg->multi_interface[path_index][0] == '\0') {
        return NULL;
    }

    return net_cfg->multi_interface[path_index];
}
// 计算期望创建的路径数量
int
xqc_mini_cli_get_target_path_count(xqc_mini_cli_user_conn_t *user_conn)
{

    int target = user_conn->ctx->args->net_cfg.multi_interface_cnt;
    if (target <= 0) {
        return MAX_PATH_CNT;
    }

    if (target > MAX_PATH_CNT) {
        target = MAX_PATH_CNT;
    }
    if(target < 1) {
        target = 1;
    }
    return target;
}
// 查找一个未激活的路径槽位
static xqc_mini_cli_user_path_t *
xqc_mini_cli_find_inactive_path(xqc_mini_cli_user_conn_t *user_conn)
{
    for (int i = 0; i < MAX_PATH_CNT; i++) {
        if (!user_conn->paths[i].is_active) {
            return &user_conn->paths[i];
        }
    }

    return NULL;
}
// 准备路径结构及其 socket 与地址信息
static int
xqc_mini_cli_prepare_user_path(xqc_mini_cli_user_conn_t *user_conn, xqc_mini_cli_user_path_t *path)
{
    int ret;
    xqc_mini_cli_ctx_t *ctx = user_conn->ctx;
    int path_index = (int)(path - user_conn->paths);
    const char *interface_name = xqc_mini_cli_get_interface_for_path(user_conn, path_index);
    
    if (path->prepared) {
        return XQC_OK;
    }

    if (path->ev_socket) {
        event_del(path->ev_socket);
        event_free(path->ev_socket);
        path->ev_socket = NULL;
    }

    if (path->fd >= 0) {
        close(path->fd);
        path->fd = -1;
    }

    path->user_conn = user_conn;
    path->get_local_addr = 0;
    path->is_active = 0;
    path->prepared = 0;
    path->path_id = XQC_MINI_PATH_ID_INVALID;
    path->consecutive_write_failures = 0;
    memset(path->interface_name, 0, sizeof(path->interface_name));
    if (interface_name != NULL) {
        strncpy(path->interface_name, interface_name, sizeof(path->interface_name) - 1);
    }

    if (path->local_addr == NULL) {
        path->local_addr = (struct sockaddr *)calloc(1, sizeof(struct sockaddr_in));
        if (path->local_addr == NULL) {
            return XQC_ERROR;
        }
    } else {
        memset(path->local_addr, 0, sizeof(struct sockaddr_in));
    }

    if (xqc_mini_cli_set_local_addr(path) != XQC_OK) {
        printf("[warn] set local address for path[%d] failed, fallback to wildcard\n", path_index);
    }
    if (path->peer_addr != NULL) {
        free(path->peer_addr);
        path->peer_addr = NULL;
        path->peer_addrlen = 0;
    }
    
    xqc_mini_cli_convert_text_to_sockaddr(AF_INET, ctx->args->net_cfg.server_addr,
        ctx->args->net_cfg.server_port, &(path->peer_addr), &(path->peer_addrlen));
    
    ret = xqc_mini_cli_init_socket(path);
    if (ret != XQC_OK) {
        return ret;
    }

    path->ev_socket = event_new(ctx->eb, path->fd, EV_READ | EV_PERSIST,
        xqc_mini_cli_socket_event_callback, path);
    if (path->ev_socket == NULL) {
        close(path->fd);
        path->fd = -1;
        return XQC_ERROR;
    }

    path->prepared = 1;

    if (path->interface_name[0] != '\0') {
        printf("[stats] path[%d] prepared interface %s on fd %d (inactive)\n", path_index,
            path->interface_name, path->fd);
    } else {
        printf("[stats] path[%d] prepared on fd %d (inactive)\n", path_index, path->fd);
    }
    

    return XQC_OK;
}
// 输出当前激活路径与绑定信息
static void
xqc_mini_cli_dump_path_bindings(xqc_mini_cli_user_conn_t *user_conn)
{
    int unique_fds[MAX_PATH_CNT];
    int unique_cnt = 0;

    for (int i = 0; i < MAX_PATH_CNT; i++) {
        unique_fds[i] = -1;
    }

    for (int i = 0; i < MAX_PATH_CNT; i++) {
        xqc_mini_cli_user_path_t *path = &user_conn->paths[i];
        if (!path->is_active || path->fd < 0) {
            continue;
        }

        const char *ifname = path->interface_name[0] != '\0' ?
            path->interface_name : "(default)";
        char local_buf[INET6_ADDRSTRLEN + 16] = {0};
        xqc_mini_cli_format_addr_port(path->local_addr, path->local_addrlen,
            local_buf, sizeof(local_buf));
        const char *local_str = local_buf[0] != '\0' ? local_buf : "-";
        printf("[stats] path[%d] fd=%d interface=%s local=%s\n", i, path->fd, ifname,
            local_str);

        int j;
        for (j = 0; j < unique_cnt; j++) {
            if (unique_fds[j] == path->fd) {
                printf("[warn] path[%d] shares fd %d with another active path\n", i, path->fd);
                break;
            }
        }

        if (j == unique_cnt && unique_cnt < MAX_PATH_CNT) {
            unique_fds[unique_cnt++] = path->fd;
        }
    }

    if (unique_cnt >= 2) {
        printf("[stats] detected %d distinct fds bound to interfaces above\n", unique_cnt);
    }
}
/*创建套接字，限制套接字对应的内核缓冲区大小 */
int
xqc_mini_cli_init_socket(xqc_mini_cli_user_path_t *user_path)
{
    int fd, size;
    xqc_mini_cli_ctx_t *ctx = user_path->user_conn->ctx;
    struct sockaddr *addr = user_path->local_addr;
    int path_index = (int)(user_path - user_path->user_conn->paths);
    const char *interface_name = xqc_mini_cli_get_interface_for_path(user_path->user_conn, path_index);

    fd = socket(addr->sa_family, SOCK_DGRAM, 0);
    if (fd < 0) {
        printf("[error] create socket failed, errno: %d\n", get_sys_errno());
        return XQC_ERROR;
    }
    if (setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE,
                   interface_name, strlen(interface_name)) < 0) {
        perror("SO_BINDTODEVICE");
    }
#ifdef XQC_SYS_WINDOWS
    if (ioctlsocket(fd, FIONBIO, &flags) == SOCKET_ERROR) {
		goto err;
	}
#else
    if (fcntl(fd, F_SETFL, O_NONBLOCK) == -1) {
        printf("[error] set socket nonblock failed, errno: %d\n", get_sys_errno());
        goto err;
    }
#endif

    if (ctx->args->net_cfg.kernel_revbuf > 0) {
        size = ctx->args->net_cfg.kernel_revbuf;
        if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &size, sizeof(int)) < 0) {
            printf("[error] setsockopt failed, errno: %d\n", get_sys_errno());
            goto err;
        }
    }

    if (ctx->args->net_cfg.kernel_sndbuf > 0) {
        size = ctx->args->net_cfg.kernel_sndbuf;
        if (setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &size, sizeof(int)) < 0) {
            printf("[error] setsockopt failed, errno: %d\n", get_sys_errno());
            goto err;
        }
    }

#if !defined(__APPLE__)
    int val = IP_PMTUDISC_DO;
    setsockopt(fd, IPPROTO_IP, IP_MTU_DISCOVER, &val, sizeof(val));
#endif
    if (user_path->local_addrlen > 0) {
        if (bind(fd, (struct sockaddr *)user_path->local_addr, user_path->local_addrlen) < 0) {
            printf("[error] bind local address failed, errno: %d\n", get_sys_errno());
            goto err;
        }
    }
#if !defined(__APPLE__)
    if (connect(fd, (struct sockaddr *)user_path->peer_addr, user_path->peer_addrlen) < 0) {
        printf("[error] connect socket failed, errno: %d\n", get_sys_errno());
        goto err;
    }
#endif

    ctx->args->net_cfg.last_socket_time = xqc_now();
    printf("[stats] init socket succesfully \n");

    user_path->fd = fd;

    return XQC_OK;
err:
    close(fd);
    return XQC_ERROR;
}
// 处理 socket 可写事件，现在没啥用
void
xqc_mini_cli_socket_write_handler(xqc_mini_cli_user_path_t *user_path, int fd)
{
    DEBUG
    printf("[stats] socket write handler\n");
}

//实际接收QUIC数据包的函数，是传输层和QUIC层的交付，
// 后面回调里面xqc_mini_cli_h3_request_read_notify仅是对接收的包进行处理，不需要再读取socket了，是QUIC层和应用层的交互
static int
xqc_mini_cli_enqueue_rx_packet(xqc_mini_cli_user_path_t *user_path,
    const unsigned char *packet_buf, size_t recv_size, xqc_usec_t recv_time)
{

    xqc_mini_cli_user_conn_t *user_conn = user_path->user_conn;
    xqc_mini_cli_rx_packet_t *pkt = malloc(sizeof(*pkt) + recv_size);
    if (pkt == NULL) {
        return 0;
    }
    pkt->path_index = (int)(user_path - user_conn->paths);
    pkt->data_len = recv_size;
    pkt->recv_time = recv_time;
    pkt->next = NULL;
    memcpy(pkt->data, packet_buf, recv_size);

    int need_notify = 0;
    pthread_mutex_lock(&user_conn->rx_queue_lock);
    if (user_conn->rx_queue_tail != NULL) {
        user_conn->rx_queue_tail->next = pkt;
    } else {
        user_conn->rx_queue_head = pkt;
    }
    user_conn->rx_queue_tail = pkt;
    if (!user_conn->rx_notify_pending) {
        user_conn->rx_notify_pending = 1;
        need_notify = 1;
    }
    pthread_mutex_unlock(&user_conn->rx_queue_lock);

    return need_notify;
}

static void *
xqc_mini_cli_rx_thread_main(void *arg)
{
    xqc_mini_cli_user_path_t *user_path = (xqc_mini_cli_user_path_t *)arg;
    xqc_mini_cli_user_conn_t *user_conn = user_path->user_conn;
    xqc_mini_cli_ctx_t *ctx = user_conn->ctx;

    size_t buffer_capacity = ctx->args->net_cfg.user_recv_buf_size > 0
        ? ctx->args->net_cfg.user_recv_buf_size
        : XQC_PACKET_BUF_LEN;
    unsigned char *packet_buf = malloc(buffer_capacity);
    if (packet_buf == NULL) {
        return NULL;
    }


    while (user_conn->rx_threads_running && user_path->rx_thread_running && user_path->fd >= 0) {
        struct pollfd pfd = { .fd = user_path->fd, .events = POLLIN };
        int pr = poll(&pfd, 1, 100);
        if (pr <= 0) {
            continue;
        }
        if (!(pfd.revents & POLLIN)) {
            continue;
        }
        int need_notify_batch = 0;
        while (user_conn->rx_threads_running && user_path->rx_thread_running) {
            ssize_t recv_size = recvfrom(user_path->fd, packet_buf, buffer_capacity, 0, NULL, NULL);
            if (recv_size < 0) {
                if (get_sys_errno() == EAGAIN || get_sys_errno() == EWOULDBLOCK) {
                    break;
                }
                break;
            }
            if (recv_size == 0) {
                break;
            }

            if (user_path->get_local_addr == 0) {
                user_path->get_local_addr = 1;
                user_path->local_addrlen = sizeof(struct sockaddr_storage);
                if (getsockname(user_path->fd, (struct sockaddr *)user_path->local_addr,
                    &user_path->local_addrlen) != 0)
                {
                    user_path->local_addrlen = 0;
                }
            }

            xqc_usec_t recv_time = xqc_now();
            ctx->args->net_cfg.last_socket_time = recv_time;
            if (xqc_mini_cli_enqueue_rx_packet(user_path, packet_buf, (size_t)recv_size, recv_time)) {
                need_notify_batch = 1;
            }

        }
        if (need_notify_batch && user_conn->rx_notify_fds[1] >= 0) {
            uint64_t one = 1;
            (void)write(user_conn->rx_notify_fds[1], &one, sizeof(one));
        }
    }
        /* recv quic packet from server */
    free(packet_buf);
    return NULL;
}
static int
xqc_mini_cli_start_path_rx_thread(xqc_mini_cli_user_path_t *path)
{
    if (path == NULL || path->fd < 0) {
        return XQC_ERROR;
    }
    if (path->rx_thread_running) {
        return XQC_OK;
    }

    path->rx_thread_running = 1;
    if (pthread_create(&path->rx_thread, NULL, xqc_mini_cli_rx_thread_main, path) != 0) {
        path->rx_thread_running = 0;
        return XQC_ERROR;
    }
    return XQC_OK;
}
static void
xqc_mini_cli_stop_path_rx_thread(xqc_mini_cli_user_path_t *path)
{
    if (path == NULL || !path->rx_thread_running) {
        return;
    }

    path->rx_thread_running = 0;
    pthread_join(path->rx_thread, NULL);
}


static void
xqc_mini_cli_drain_rx_queue(xqc_mini_cli_user_conn_t *user_conn)
{
    xqc_mini_cli_ctx_t *ctx = user_conn->ctx;
    int processed = 0;
    for (;;) {
        pthread_mutex_lock(&user_conn->rx_queue_lock);
        xqc_mini_cli_rx_packet_t *pkt = user_conn->rx_queue_head;
        if (pkt != NULL) {
            user_conn->rx_queue_head = pkt->next;
            if (user_conn->rx_queue_head == NULL) {
                user_conn->rx_queue_tail = NULL;
            }
        }
        pthread_mutex_unlock(&user_conn->rx_queue_lock);
        if (pkt == NULL) {
            break;
        }

        if (pkt->path_index >= 0 && pkt->path_index < MAX_PATH_CNT) {
            xqc_mini_cli_user_path_t *path = &user_conn->paths[pkt->path_index];
            if (path->peer_addr != NULL && path->local_addr != NULL) {
                xqc_int_t ret = xqc_engine_packet_process(ctx->engine, pkt->data, pkt->data_len,
                    path->local_addr, path->local_addrlen,
                    path->peer_addr, path->peer_addrlen,
                    pkt->recv_time, user_conn);
                if (ret != XQC_OK) {
                    printf("[error] process packet from queue failed, ret:%d\n", ret);
                } else {
                    processed = 1;
                }
            }
        }

        free(pkt);
    }


    if (processed) {
        xqc_engine_finish_recv(ctx->engine);
    }
}


static void
xqc_mini_cli_rx_queue_event_callback(int fd, short what, void *arg)
{
    (void)what;
    xqc_mini_cli_user_conn_t *user_conn = (xqc_mini_cli_user_conn_t *)arg;
    uint64_t cnt = 0;
    while (read(fd, &cnt, sizeof(cnt)) > 0) {
    }
    xqc_mini_cli_drain_rx_queue(user_conn);
}

//实际接收QUIC数据包的函数，是传输层和QUIC层的交付，
// 后面回调里面xqc_mini_cli_h3_request_read_notify仅是对接收的包进行处理，不需要再读取socket了，是QUIC层和应用层的交互
void
xqc_mini_cli_socket_read_handler(xqc_mini_cli_user_path_t *user_path, int fd)
{
    (void)fd;
    xqc_mini_cli_drain_rx_queue(user_path->user_conn);

}

// 去除字符串首尾空白字符
static char *
xqc_mini_cli_trim_space(char *text)
{
    if (text == NULL) {
        return NULL;
    }

    while (*text && isspace((unsigned char)*text)) {
        text++;
    }

    if (*text == '\0') {
        return text;
    }

    char *end = text + strlen(text) - 1;
    while (end > text && isspace((unsigned char)*end)) {
        *end = '\0';
        end--;
    }

    return text;
}
// 解析 interface 列表并写入配置
static void
xqc_mini_cli_apply_interface_list(xqc_mini_cli_args_t *args, const char *value)
{
    if (value == NULL) {
        return;
    }

    args->net_cfg.multi_interface_cnt = 0;
    for (int i = 0; i < MAX_PATH_CNT; i++) {
        memset(args->net_cfg.multi_interface[i], 0, sizeof(args->net_cfg.multi_interface[i]));
    }

    char list_buf[256] = {0};
    strncpy(list_buf, value, sizeof(list_buf) - 1);

    char *token = strtok(list_buf, ", ");
    while (token && args->net_cfg.multi_interface_cnt < MAX_PATH_CNT) {
        char *trimmed = xqc_mini_cli_trim_space(token);
        if (trimmed[0] != '\0') {
            strncpy(args->net_cfg.multi_interface[args->net_cfg.multi_interface_cnt],
                trimmed, XQC_MINI_INTERFACE_NAME_MAX_LEN - 1);
            args->net_cfg.multi_interface_cnt++;
        }
        token = strtok(NULL, ", ");
    }
}
// 读取并解析客户端配置文件
static int
xqc_mini_cli_load_config_file(xqc_mini_cli_args_t *args, const char *path)
{
    if (path == NULL || path[0] == '\0') {
        return XQC_OK;
    }

    FILE *fp = fopen(path, "r");
    if (fp == NULL) {
        return XQC_OK;
    }

    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        char *line_ptr = xqc_mini_cli_trim_space(line);
        if (line_ptr[0] == '\0' || line_ptr[0] == '#') {
            continue;
        }

        char *eq = strchr(line_ptr, '=');
        if (eq == NULL) {
            continue;
        }

        *eq = '\0';
        char *key = xqc_mini_cli_trim_space(line_ptr);
        char *value = xqc_mini_cli_trim_space(eq + 1);
        if (key[0] == '\0' || value[0] == '\0') {
            continue;
        }

        if (strcmp(key, "server_addr") == 0) {
            strncpy(args->net_cfg.server_addr, value, sizeof(args->net_cfg.server_addr) - 1);
            args->net_cfg.server_addr[sizeof(args->net_cfg.server_addr) - 1] = '\0';

        } else if (strcmp(key, "server_port") == 0) {
            char *endptr = NULL;
            long port = strtol(value, &endptr, 10);
            if (endptr != value && *endptr == '\0' && port > 0 && port <= UINT16_MAX) {
                args->net_cfg.server_port = (unsigned short)port;
            }

        } else if (strcmp(key, "stream_cnt") == 0) {
            char *endptr = NULL;
            long stream_cnt = strtol(value, &endptr, 10);
            if (endptr != value && *endptr == '\0' && stream_cnt > 0) {
                if (stream_cnt > XQC_MINI_MAX_STREAMS) {
                    stream_cnt = XQC_MINI_MAX_STREAMS;
                }
                args->req_stream_cnt = (int)stream_cnt;
            }

        }
        else if (strcmp(key, "stream_range_size") == 0) {
            char *endptr = NULL;
            unsigned long long size = strtoull(value, &endptr, 10);
            if (endptr != value && *endptr == '\0' && size > 0) {
                args->net_cfg.stream_range_size = (size_t)size;
            }
        }
        else if (strcmp(key, "send_data_len") == 0) {
            char *endptr = NULL;
            unsigned long long size = strtoull(value, &endptr, 10);
            if (endptr != value && *endptr == '\0') {
                args->send_data_len = (size_t)size;
            }

        }
        else if (strcmp(key, "method") == 0) {
            char method_buf[8] = {0};
            size_t len = strlen(value);
            if (len >= sizeof(method_buf)) {
                len = sizeof(method_buf) - 1;
            }
            for (size_t i = 0; i < len; i++) {
                method_buf[i] = (char)toupper((unsigned char)value[i]);
            }
            if (strcmp(method_buf, "GET") == 0) {
                args->req_cfg.method = REQUEST_METHOD_GET;
            } else if (strcmp(method_buf, "POST") == 0) {
                args->req_cfg.method = REQUEST_METHOD_POST;
            }

        } else if (strcmp(key, "cc") == 0) {
            if (strcmp(value, "bbr") == 0) {
                args->quic_cfg.cc = CC_TYPE_BBR;
            } else if (strcmp(value, "cubic") == 0) {
                args->quic_cfg.cc = CC_TYPE_CUBIC;
            }

        } else if (strcmp(key, "mp_sched") == 0) {
            if (strcmp(value, "minrtt") == 0
                || strcmp(value, "backup") == 0
                || strcmp(value, "balanced") == 0
                || strcmp(value, "rap") == 0
                || strcmp(value, "act") == 0
                || strcmp(value, "bw") == 0) {
                memset(args->quic_cfg.mp_sched, 0, sizeof(args->quic_cfg.mp_sched));
                strncpy(args->quic_cfg.mp_sched, value, sizeof(args->quic_cfg.mp_sched) - 1);
            }
 
        }
        else if (strcmp(key, "interface") == 0 || strcmp(key, "interfaces") == 0) {
            xqc_mini_cli_apply_interface_list(args, value);

        } else if (strcmp(key, "kernel_sndbuf") == 0) {
            char *endptr = NULL;
            long size = strtol(value, &endptr, 10);
            if (endptr != value && *endptr == '\0' && size > 0) {
                args->net_cfg.kernel_sndbuf = (int)size;
            }

        } else if (strcmp(key, "kernel_revbuf") == 0) {
            char *endptr = NULL;
            long size = strtol(value, &endptr, 10);
            if (endptr != value && *endptr == '\0' && size > 0) {
                args->net_cfg.kernel_revbuf = (int)size;
            }

        } else if (strcmp(key, "user_send_buf_size") == 0) {
            char *endptr = NULL;
            unsigned long long size = strtoull(value, &endptr, 10);
            if (endptr != value && *endptr == '\0' && size > 0) {
                args->net_cfg.user_send_buf_size = (size_t)size;
            }

        } else if (strcmp(key, "user_recv_buf_size") == 0) {
            char *endptr = NULL;
            unsigned long long size = strtoull(value, &endptr, 10);
            if (endptr != value && *endptr == '\0' && size > 0) {
                args->net_cfg.user_recv_buf_size = (size_t)size;
            }
        }else if (strcmp(key, "download_path") == 0) {
            memset(args->env_cfg.download_path, 0, sizeof(args->env_cfg.download_path));
            strncpy(args->env_cfg.download_path, value, sizeof(args->env_cfg.download_path) - 1);
        } else if (strcmp(key, "download_target") == 0) {
            memset(args->env_cfg.download_target, 0, sizeof(args->env_cfg.download_target));
            strncpy(args->env_cfg.download_target, value, sizeof(args->env_cfg.download_target) - 1);
            if (args->req_cfg.url[0] == '/' && args->req_cfg.url[1] == '\0') {
                strncpy(args->req_cfg.url, value, sizeof(args->req_cfg.url) - 1);
                args->req_cfg.url[sizeof(args->req_cfg.url) - 1] = '\0';
                strncpy(args->req_cfg.path, args->req_cfg.url, sizeof(args->req_cfg.path) - 1);
                args->req_cfg.path[sizeof(args->req_cfg.path) - 1] = '\0';
            }
        }else if (strcmp(key, "upload_path") == 0 || strcmp(key, "send_file") == 0) {
            memset(args->env_cfg.upload_path, 0, sizeof(args->env_cfg.upload_path));
            strncpy(args->env_cfg.upload_path, value, sizeof(args->env_cfg.upload_path) - 1);

        } else if (strcmp(key, "use_zlog") == 0) {
            if (strcmp(value, "1") == 0 || strcasecmp(value, "true") == 0) {
                args->env_cfg.use_zlog = 1;
            } else {
                args->env_cfg.use_zlog = 0;
            }
        } else if (strcmp(key, "zlog_conf") == 0) {
            memset(args->env_cfg.zlog_conf, 0, sizeof(args->env_cfg.zlog_conf));
            strncpy(args->env_cfg.zlog_conf, value, sizeof(args->env_cfg.zlog_conf) - 1);
        } else if (strcmp(key, "zlog_category") == 0) {
            memset(args->env_cfg.zlog_category, 0, sizeof(args->env_cfg.zlog_category));
            strncpy(args->env_cfg.zlog_category, value, sizeof(args->env_cfg.zlog_category) - 1);
        }
    }

    fclose(fp);
    return XQC_OK;
}
// libevent socket 事件回调分发，查看socket是否可写或者可读
static void
xqc_mini_cli_socket_event_callback(int fd, short what, void *arg)
{
    //DEBUG;
    xqc_mini_cli_user_path_t *user_path = (xqc_mini_cli_user_path_t *)arg;

    if (what & EV_WRITE) {
        xqc_mini_cli_socket_write_handler(user_path, fd);

    } else if (what & EV_READ) {
        xqc_mini_cli_socket_read_handler(user_path, fd);

    } else {
        printf("event callback: fd=%d, what=%d\n", fd, what);
        exit(1);
    }
}

// 创建并初始化 QUIC 连接
int
xqc_mini_cli_init_xquic_connection(xqc_mini_cli_user_conn_t *user_conn)
{
    
    xqc_conn_ssl_config_t conn_ssl_config = {0};
    xqc_conn_settings_t conn_settings = {0};
    xqc_mini_cli_ctx_t *ctx;
    xqc_mini_cli_args_t *args;

    ctx = user_conn->ctx;
    args = ctx->args;

    /* load 0-rtt args */
    xqc_mini_cli_init_0rtt(ctx->args);

    /* init connection settings */
    xqc_mini_cli_init_conn_settings(&conn_settings, ctx->args);

    /* init connection ssl config */
    xqc_mini_cli_init_conn_ssl_config(&conn_ssl_config, ctx->args);

    xqc_mini_cli_user_path_t *path = &user_conn->paths[0];
    

    /* build connection */
    const xqc_cid_t *cid = xqc_h3_connect(ctx->engine, &conn_settings, args->quic_cfg.token,
        args->quic_cfg.token_len, args->req_cfg.host, args->quic_cfg.no_encryption, &conn_ssl_config,
        path->peer_addr, path->peer_addrlen, user_conn);
    if (cid == NULL) {
        return XQC_ERROR;
    }
    memcpy(&user_conn->cid, cid, sizeof(xqc_cid_t));
    printf("[stats] init xquic connection success \n");
    
    return XQC_OK;
}



// 填充通配地址作为本地绑定地址
static void
xqc_mini_cli_fill_wildcard_local_addr(struct sockaddr *local_addr, socklen_t *addrlen)
{
    struct sockaddr_in *addr_v4 = (struct sockaddr_in *)local_addr;
    memset(addr_v4, 0, sizeof(struct sockaddr_in));
    addr_v4->sin_family = AF_INET;
    addr_v4->sin_addr.s_addr = INADDR_ANY;
    addr_v4->sin_port = 0;
    *addrlen = sizeof(struct sockaddr_in);
}

// 将 sockaddr 格式化为字符串 IP:port
static void
xqc_mini_cli_format_addr_port(const struct sockaddr *addr, socklen_t addrlen, char *buf, size_t buflen)
{
    if (buflen == 0) {
        return;
    }

    buf[0] = '\0';

    if (addr == NULL || addrlen == 0) {
        return;
    }

    char ip[INET6_ADDRSTRLEN] = {0};
    uint16_t port = 0;

    if (addr->sa_family == AF_INET) {
        const struct sockaddr_in *addr_v4 = (const struct sockaddr_in *)addr;
        if (inet_ntop(AF_INET, &addr_v4->sin_addr, ip, sizeof(ip)) == NULL) {
            return;
        }
        port = ntohs(addr_v4->sin_port);
        snprintf(buf, buflen, "%s:%u", ip, port);
        return;
    }

    if (addr->sa_family == AF_INET6) {
        const struct sockaddr_in6 *addr_v6 = (const struct sockaddr_in6 *)addr;
        if (inet_ntop(AF_INET6, &addr_v6->sin6_addr, ip, sizeof(ip)) == NULL) {
            return;
        }
        port = ntohs(addr_v6->sin6_port);
        snprintf(buf, buflen, "[%s]:%u", ip, port);
        return;
    }

    snprintf(buf, buflen, "af%d", addr->sa_family);
}

// 查询指定网卡的本地地址
static int
xqc_mini_cli_query_interface_addr(const char *interface_name, int desired_family,
    struct sockaddr_storage *storage, socklen_t *addrlen)
{
#if defined(XQC_SYS_WINDOWS)
    (void)interface_name;
    (void)storage;
    (void)addrlen;
    return XQC_ERROR;
#else
    struct ifaddrs *ifaddr = NULL;
    if (getifaddrs(&ifaddr) != 0) {
        printf("[error] getifaddrs failed for %s: %d\n", interface_name, errno);
        return XQC_ERROR;
    }

    int ret = XQC_ERROR;
    struct ifaddrs *ifa = NULL;
    struct sockaddr_storage v6_candidate = {0};
    socklen_t v6_len = 0;
    int have_v6 = 0;

    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_name == NULL || ifa->ifa_addr == NULL) {
            continue;
        }
        if (strcmp(ifa->ifa_name, interface_name) != 0) {
            continue;
        }

        if (desired_family == AF_UNSPEC || ifa->ifa_addr->sa_family == desired_family) {
            if (ifa->ifa_addr->sa_family == AF_INET) {
                memcpy(storage, ifa->ifa_addr, sizeof(struct sockaddr_in));
                struct sockaddr_in *addr_v4 = (struct sockaddr_in *)storage;
                addr_v4->sin_port = 0;
                *addrlen = sizeof(struct sockaddr_in);
                ret = XQC_OK;
                break;
            }

            if (ifa->ifa_addr->sa_family == AF_INET6) {
                memcpy(storage, ifa->ifa_addr, sizeof(struct sockaddr_in6));
                struct sockaddr_in6 *addr_v6 = (struct sockaddr_in6 *)storage;
                addr_v6->sin6_port = 0;
                *addrlen = sizeof(struct sockaddr_in6);
                ret = XQC_OK;
                break;
            }
        }

        if (desired_family == AF_UNSPEC && ifa->ifa_addr->sa_family == AF_INET6) {
            memcpy(&v6_candidate, ifa->ifa_addr, sizeof(struct sockaddr_in6));
            struct sockaddr_in6 *addr_v6 = (struct sockaddr_in6 *)&v6_candidate;
            addr_v6->sin6_port = 0;
            v6_len = sizeof(struct sockaddr_in6);
            have_v6 = 1;
        }
    }

    if (ret != XQC_OK && desired_family == AF_UNSPEC && have_v6) {
        memcpy(storage, &v6_candidate, v6_len);
        *addrlen = v6_len;
        ret = XQC_OK;
    }

    freeifaddrs(ifaddr);
    return ret;
#endif
}

// 根据接口名设置本地地址（失败则回退通配）
static int
xqc_mini_cli_set_local_addr(xqc_mini_cli_user_path_t *path)
{
    socklen_t addrlen = 0;
    int ret = XQC_OK;
    if (path->interface_name[0] != '\0') {
        struct sockaddr_storage storage = {0};
        int desired_family = AF_UNSPEC;
        if (path->peer_addr != NULL) {
            desired_family = path->peer_addr->sa_family;
        } else {
            desired_family = AF_INET;
        }

        ret = xqc_mini_cli_query_interface_addr(path->interface_name, desired_family,
            &storage, &addrlen);
        if (ret == XQC_OK) {
            memcpy(path->local_addr, &storage, addrlen);
            path->local_addrlen = addrlen;
            return XQC_OK;
        }

        printf("[warn] query interface %s address failed\n", path->interface_name);
    } else {
        ret = XQC_OK;
    }

    xqc_mini_cli_fill_wildcard_local_addr(path->local_addr, &addrlen);
    path->local_addrlen = addrlen;
    return ret;
}

// 初始化路径并绑定 socket 到事件循环
static int
xqc_mini_cli_init_user_path(xqc_mini_cli_user_conn_t *user_conn, xqc_mini_cli_user_path_t *path,
    uint64_t path_id)
{
    int path_index = (int)(path - user_conn->paths);
    
    int ret = xqc_mini_cli_prepare_user_path(user_conn, path);
    
    if (ret != XQC_OK) {
        return ret;
    }

    path->get_local_addr = 0;

    if (event_add(path->ev_socket, NULL) != 0) {
        printf("[error] event_add failed for path[%d]\n", path_index);
        event_free(path->ev_socket);
        path->ev_socket = NULL;
        if (path->fd >= 0) {
            close(path->fd);
            path->fd = -1;
        }
        path->prepared = 0;
        return XQC_ERROR;
    }

    path->path_id = path_id;
    path->is_active = 1;

    if (xqc_mini_cli_start_path_rx_thread(path) != XQC_OK) {
        printf("[error] start rx thread failed for path[%d]\n", path_index);
        path->is_active = 0;
        if (path->ev_socket) {
            event_del(path->ev_socket);
            event_free(path->ev_socket);
            path->ev_socket = NULL;
        }
        if (path->fd >= 0) {
            close(path->fd);
            path->fd = -1;
        }
        path->prepared = 0;
        return XQC_ERROR;
    }


    if (path->interface_name[0] != '\0') {
        printf("[stats] path[%d] interface %s bound fd %d\n", path_index,
            path->interface_name, path->fd);
    } else {
        printf("[stats] path[%d] bound fd %d\n", path_index, path->fd);
    }

    xqc_mini_cli_dump_path_bindings(user_conn);

    return XQC_OK;
}

static int
xqc_mini_cli_create_new_path(xqc_mini_cli_user_conn_t *user_conn)
{
    if (user_conn == NULL || user_conn->ctx == NULL) {
        return XQC_ERROR;
    }

    if (!user_conn->ctx->args->quic_cfg.multipath) {
        return XQC_ERROR;
    }

    int target_cnt = xqc_mini_cli_get_target_path_count(user_conn);
    if (user_conn->active_path_cnt >= target_cnt) {
        printf("[warn] reach max path count, ignore new path creation\n");
        return XQC_ERROR;
    }

    xqc_mini_cli_user_path_t *path = xqc_mini_cli_find_inactive_path(user_conn);
    if (path == NULL) {
        printf("[warn] no inactive path slot available for new path\n");
        return XQC_ERROR;
    }

    uint64_t new_path_id = 0;
    int ret = xqc_conn_create_path(user_conn->ctx->engine, &user_conn->cid, &new_path_id, 0);
    if (ret != XQC_OK) {
        if (ret == -XQC_EMP_NO_AVAIL_PATH_ID) {
            printf("[warn] xqc_conn_create_path delayed: no available path id yet, will retry\n");
        } else if (ret == -XQC_EMP_NOT_SUPPORT_MP) {
            printf("[warn] xqc_conn_create_path skipped: multipath not ready, will retry\n");
        } else {
            printf("[error] xqc_conn_create_path error:%d\n", ret);
        }
        return ret;
    }
    if (!path->prepared) {
        ret = xqc_mini_cli_prepare_user_path(user_conn, path);
        if (ret != XQC_OK) {
            printf("[error] prepare new path socket failed, ret:%d\n", ret);
            xqc_conn_close_path(user_conn->ctx->engine, &user_conn->cid, new_path_id);
            return ret;
        }
    }

    ret = xqc_mini_cli_init_user_path(user_conn, path, new_path_id);
    if (ret != XQC_OK) {
        printf("[error] init new path failed, ret:%d\n", ret);
        xqc_conn_close_path(user_conn->ctx->engine, &user_conn->cid, new_path_id);
        return ret;
    }

    user_conn->active_path_cnt++;
    printf("[stats] new path created, path_id=%"PRIu64"\n", new_path_id);
    xqc_engine_main_logic(user_conn->ctx->engine);
    return XQC_OK;
}


// 连接可创建新路径时的回调处理
static void
xqc_mini_cli_conn_ready_to_create_path(const xqc_cid_t *cid, void *conn_user_data)
{
    xqc_mini_cli_user_conn_t *user_conn = (xqc_mini_cli_user_conn_t *)conn_user_data;
    (void)xqc_mini_cli_create_new_path(user_conn);
}

// 路径被移除时的清理回调
static void
xqc_mini_cli_path_removed(const xqc_cid_t *cid, uint64_t path_id, void *conn_user_data)
{
    xqc_mini_cli_user_conn_t *user_conn = (xqc_mini_cli_user_conn_t *)conn_user_data;
    if (user_conn == NULL) {
        return;
    }

    for (int i = 0; i < MAX_PATH_CNT; i++) {
        xqc_mini_cli_user_path_t *path = &user_conn->paths[i];
        if (path->is_active && path->path_id == path_id) {
            path->is_active = 0;
            xqc_mini_cli_stop_path_rx_thread(path);
            if (user_conn->active_path_cnt > 0) {
                user_conn->active_path_cnt--;
            }
            if (path->ev_socket) {
                event_del(path->ev_socket);
                event_free(path->ev_socket);
                path->ev_socket = NULL;
            }
            if (path->fd >= 0) {
                close(path->fd);
                path->fd = -1;
            }
            path->prepared = 0;
            path->consecutive_write_failures = 0;
            path->path_id = XQC_MINI_PATH_ID_INVALID;
            printf("[stats] path removed, path_id=%"PRIu64"\n", path_id);
            xqc_mini_cli_dump_path_bindings(user_conn);
            xqc_engine_main_logic(user_conn->ctx->engine);
            break;
        }
    }
}

void
xqc_mini_cli_try_rebuild_paths(xqc_mini_cli_user_conn_t *user_conn)
{
    if (user_conn == NULL || user_conn->ctx == NULL) {
        return;
    }

    if (!user_conn->ctx->args->quic_cfg.multipath) {
        return;
    }

    int target_cnt = xqc_mini_cli_get_target_path_count(user_conn);
    // printf("[stats] try to rebuild paths, current cnt: %d, target cnt: %d\n",
    //     user_conn->active_path_cnt, target_cnt);
    while (user_conn->active_path_cnt < target_cnt) {
        int ret = xqc_mini_cli_create_new_path(user_conn);
        if (ret != XQC_OK) {
            if (ret == -XQC_EMP_NO_AVAIL_PATH_ID || ret == -XQC_EMP_NOT_SUPPORT_MP) {
                /* multipath control frames/cids are not ready yet, wait for periodic retry */
            }
            break;
        }
    }
}

// 客户端主流程：创建连接并发起请求
int
xqc_mini_cli_launch_requests(xqc_mini_cli_user_conn_t *user_conn)
{
    int ret;
    if (user_conn == NULL || user_conn->ctx == NULL) {
        return XQC_ERROR;
    }
    if (user_conn->requests_launched) {
        return XQC_OK;
    }

    printf("[stats] launch requests, max concurrency=%d\n",
        user_conn->max_concurrent_requests);


    user_conn->requests_launched = 1;
    return xqc_mini_cli_try_schedule_requests(user_conn);
}
int
xqc_mini_cli_main_process(xqc_mini_cli_user_conn_t *user_conn, xqc_mini_cli_ctx_t *ctx)
{
    int ret;
    xqc_mini_cli_args_t *args;

    user_conn->ctx = ctx;
    args = ctx->args;

    ret = xqc_mini_cli_init_xquic_connection(user_conn);
    if (ret < 0) {
        printf("[error] mini socket init xquic connection failed\n");
        return XQC_ERROR;
    }

    printf("[stats] requests are deferred until handshake/path readiness\n");

    return XQC_OK;
}

// 创建并初始化用户连接结构
xqc_mini_cli_user_conn_t *
xqc_mini_cli_user_conn_create(xqc_mini_cli_ctx_t *ctx)
{
    int ret;
    xqc_mini_cli_user_conn_t *user_conn = calloc(1, sizeof(xqc_mini_cli_user_conn_t));

    user_conn->ctx = ctx;
    user_conn->rx_notify_fds[0] = -1;
    user_conn->rx_notify_fds[1] = -1;
    user_conn->rx_threads_running = 1;
    pthread_mutex_init(&user_conn->rx_queue_lock, NULL);

    if (pipe(user_conn->rx_notify_fds) != 0) {
        printf("[error] create rx notify pipe failed\n");
        free(user_conn);
        return NULL;
    }
    fcntl(user_conn->rx_notify_fds[0], F_SETFL, O_NONBLOCK);
    fcntl(user_conn->rx_notify_fds[1], F_SETFL, O_NONBLOCK);
    user_conn->ev_rx_queue = event_new(ctx->eb, user_conn->rx_notify_fds[0], EV_READ | EV_PERSIST,
        xqc_mini_cli_rx_queue_event_callback, user_conn);
    if (user_conn->ev_rx_queue == NULL || event_add(user_conn->ev_rx_queue, NULL) != 0) {
        printf("[error] create rx queue event failed\n");
        if (user_conn->ev_rx_queue) {
            event_free(user_conn->ev_rx_queue);
        }
        close(user_conn->rx_notify_fds[0]);
        close(user_conn->rx_notify_fds[1]);
        pthread_mutex_destroy(&user_conn->rx_queue_lock);
        free(user_conn);
        return NULL;
    }

    for (int i = 0; i < MAX_PATH_CNT; i++) {
        user_conn->paths[i].fd = -1;
        user_conn->paths[i].user_conn = user_conn;
        user_conn->paths[i].is_active = 0;
        user_conn->paths[i].prepared = 0;
        user_conn->paths[i].path_id = XQC_MINI_PATH_ID_INVALID;
        user_conn->paths[i].rx_thread_running = 0;
    }
    /* set connection timeout */
    struct timeval tv;
    tv.tv_sec = ctx->args->net_cfg.conn_timeout;
    tv.tv_usec = 0;
    user_conn->ev_timeout = event_new(ctx->eb, -1, 0, xqc_mini_cli_timeout_callback, user_conn);
    event_add(user_conn->ev_timeout, &tv);
    /* set path retry timer */
    tv.tv_sec = 2;
    tv.tv_usec = 0;
    user_conn->ev_path_retry = event_new(ctx->eb, -1, 0, xqc_mini_cli_path_retry_callback, user_conn);
    event_add(user_conn->ev_path_retry, &tv);


    
    xqc_mini_cli_user_path_t *path0 = &user_conn->paths[0];
    
    ret = xqc_mini_cli_init_user_path(user_conn, path0, 0);
    
    // printf("path_id: %"PRIu64", address_path: %s,peer_address:%s\n",
    //            path0->path_id,inet_ntoa(((struct sockaddr_in*)path0->local_addr)->sin_addr),inet_ntoa(((struct sockaddr_in*)path0->peer_addr)->sin_addr));
    if (ret < 0) {
        printf("[error] mini socket init socket failed\n");
        xqc_mini_cli_free_user_conn(user_conn);
        return NULL;
    }

    user_conn->active_path_cnt = 1;
    user_conn->send_file_size = ctx->args->send_data_len;
    
    user_conn->upload_start_time = 0;
    user_conn->upload_finished_streams = 0;
    user_conn->upload_total_bytes = 0;
    user_conn->download_fp = NULL;
    user_conn->download_path[0] = '\0';
    user_conn->download_total_bytes = 0;
    user_conn->download_expected_bytes = 0;
    user_conn->download_received_bytes = 0;
    user_conn->download_progress_percent = -1;
    user_conn->download_finished_streams = 0;
    user_conn->download_start_time = 0;
    user_conn->next_fallback_path_index = 0;
    user_conn->handshake_finished = 0;
    user_conn->requests_launched = 0;
    user_conn->path_wait_rounds_after_handshake = 0;
    user_conn->last_path_probe_ping_time = 0;

   
    memset(user_conn->send_file_path, 0, sizeof(user_conn->send_file_path));
    if (ctx->args->req_cfg.method == REQUEST_METHOD_POST) {
        strncpy(user_conn->send_file_path, ctx->args->env_cfg.upload_path,
            sizeof(user_conn->send_file_path) - 1);
        user_conn->send_file_path[sizeof(user_conn->send_file_path) - 1] = '\0';
        FILE *send_fp = fopen(user_conn->send_file_path, "rb");
        if (send_fp == NULL) {
            perror("fopen");
            printf("[error] failed to open send file '%s'\n", user_conn->send_file_path);
            xqc_mini_cli_free_user_conn(user_conn);
            return NULL;
        }
        if (fseek(send_fp, 0, SEEK_END) != 0) {
            perror("fseek");
            fclose(send_fp);
            xqc_mini_cli_free_user_conn(user_conn);
            return NULL;
        }
        long file_length = ftell(send_fp);
        if (file_length < 0) {
            perror("ftell");
            fclose(send_fp);
            xqc_mini_cli_free_user_conn(user_conn);
            return NULL;
        }
        fclose(send_fp);
        size_t actual_size = (size_t)file_length;
        if (user_conn->send_file_size == 0 || user_conn->send_file_size > actual_size) {
            if (user_conn->send_file_size > actual_size) {
                printf("[warn] requested payload size %zu exceeds file size %zu, clamp\n",
                    user_conn->send_file_size, actual_size);
            }
            user_conn->send_file_size = actual_size;
        }

        if (user_conn->send_file_size == 0) {
            printf("[warn] send data length is 0, will send empty body\n");
        } else {
            printf("[stats] source file '%s' size=%zu bytes\n",
                user_conn->send_file_path, user_conn->send_file_size);
        }
       
    }

    int stream_concurrency = ctx->args->req_stream_cnt;
    if (stream_concurrency <= 0) {
        stream_concurrency = 1;
    }
    int stream_target = stream_concurrency;
    // if (stream_target > XQC_MINI_MAX_STREAMS) {
    //     printf("[warn] exceed max stream count %d, clamp to limit\n", XQC_MINI_MAX_STREAMS);
    //     stream_target = XQC_MINI_MAX_STREAMS;
    // }
    //  if (ctx->args->req_cfg.method == REQUEST_METHOD_POST) {
    //     if (user_conn->send_file_size == 0 && stream_target != 1) {
    //         printf("[warn] send file is empty, forcing single stream\n");
    if (ctx->args->req_cfg.method == REQUEST_METHOD_POST) {
        if (user_conn->send_file_size == 0) {
            stream_target = 1;
        }
        // if (user_conn->send_file_size > 0
        //     && (size_t)stream_target > user_conn->send_file_size) {
        //     printf("[warn] stream count %d exceeds file bytes %zu, clamp\n",
        //         stream_target, user_conn->send_file_size);
        //     stream_target = (int)user_conn->send_file_size;
        //     if (stream_target <= 0) {
        //         stream_target = 1;
        //     }
        // }
        else {
            size_t stream_range_size = xqc_mini_cli_get_stream_range_size(user_conn);
            size_t required = (user_conn->send_file_size + stream_range_size - 1)
                / stream_range_size;
            if (required > (size_t)INT_MAX) {
                stream_target = INT_MAX;
            } else {
                stream_target = (int)required;
    }
        }
        printf("[stats] POST upload uses fixed stream range %zu bytes, stream count=%d\n",
            xqc_mini_cli_get_stream_range_size(user_conn), stream_target);

    }
    if (stream_concurrency > XQC_MINI_MAX_STREAMS) {
        stream_concurrency = XQC_MINI_MAX_STREAMS;
    }
    if (stream_concurrency > stream_target) {
        stream_concurrency = stream_target;
    }
    if (stream_concurrency <= 0) {
        stream_concurrency = 1;
    }

    ctx->args->req_stream_cnt = stream_concurrency;
    user_conn->max_concurrent_requests = stream_concurrency;

    user_conn->target_requests = stream_target;
    user_conn->next_stream_index = 0;
    user_conn->active_requests = 0;

    user_conn->completed_requests = 0;
    user_conn->total_requests_known =
        (ctx->args->req_cfg.method == REQUEST_METHOD_POST) ? 1 : 0;


    int target_prepare = xqc_mini_cli_get_target_path_count(user_conn);
    for (int i = 1; i < target_prepare; i++) {
        xqc_mini_cli_user_path_t *path = &user_conn->paths[i];
        ret = xqc_mini_cli_prepare_user_path(user_conn, path);
        //ret = xqc_mini_cli_init_user_path(user_conn, path, i);
        if (ret != XQC_OK) {
            printf("[warn] pre-bind for path[%d] failed, ret:%d\n", i, ret);
        }
    }


    return user_conn;
}


// 释放用户连接及其资源
void
xqc_mini_cli_free_user_conn(xqc_mini_cli_user_conn_t *user_conn)
{
    if (user_conn == NULL) {
        return;
    }
    if (user_conn->download_fp) {
        fclose(user_conn->download_fp);
        user_conn->download_fp = NULL;
    }
    user_conn->rx_threads_running = 0;
    for (int i = 0; i < MAX_PATH_CNT; i++) {
        xqc_mini_cli_user_path_t *path = &user_conn->paths[i];
        xqc_mini_cli_stop_path_rx_thread(path);
        if (path->ev_socket) {
            event_del(path->ev_socket);
            event_free(path->ev_socket);
            path->ev_socket = NULL;
        }
        if (path->fd >= 0) {
            close(path->fd);
            path->fd = -1;
        }
        free(path->peer_addr);
        free(path->local_addr);
        path->peer_addr = NULL;
        path->local_addr = NULL;
        path->prepared = 0;
        path->is_active = 0;
        path->path_id = XQC_MINI_PATH_ID_INVALID;
    }

    
    if (user_conn->ev_rx_queue) {
        event_del(user_conn->ev_rx_queue);
        event_free(user_conn->ev_rx_queue);
        user_conn->ev_rx_queue = NULL;
    }
    if (user_conn->rx_notify_fds[0] >= 0) {
        close(user_conn->rx_notify_fds[0]);
        user_conn->rx_notify_fds[0] = -1;
    }
    if (user_conn->rx_notify_fds[1] >= 0) {
        close(user_conn->rx_notify_fds[1]);
        user_conn->rx_notify_fds[1] = -1;
    }
    xqc_mini_cli_rx_packet_t *pkt = NULL;
    pthread_mutex_lock(&user_conn->rx_queue_lock);
    pkt = user_conn->rx_queue_head;
    user_conn->rx_queue_head = NULL;
    user_conn->rx_queue_tail = NULL;
    pthread_mutex_unlock(&user_conn->rx_queue_lock);
    while (pkt) {
        xqc_mini_cli_rx_packet_t *next = pkt->next;
        free(pkt);
        pkt = next;
    }
    pthread_mutex_destroy(&user_conn->rx_queue_lock);

    if (user_conn->ev_timeout) {
        event_del(user_conn->ev_timeout);
        event_free(user_conn->ev_timeout);
        user_conn->ev_timeout = NULL;
    }
    if (user_conn->ev_path_retry) {
        event_del(user_conn->ev_path_retry);
        event_free(user_conn->ev_path_retry);
        user_conn->ev_path_retry = NULL;
    }
    

    free(user_conn);
}

// 连接结束时清理事件与 socket
void
xqc_mini_cli_on_connection_finish(xqc_mini_cli_user_conn_t *user_conn)
{
     if (user_conn == NULL) {
        return;
    }
    if (user_conn->ev_timeout) {
        event_del(user_conn->ev_timeout);
        event_free(user_conn->ev_timeout);
        user_conn->ev_timeout = NULL;
    }
    if (user_conn->ev_path_retry) {
        event_del(user_conn->ev_path_retry);
        event_free(user_conn->ev_path_retry);
        user_conn->ev_path_retry = NULL;
    }

    user_conn->rx_threads_running = 0;
    for (int i = 0; i < MAX_PATH_CNT; i++) {
        xqc_mini_cli_user_path_t *path = &user_conn->paths[i];
        xqc_mini_cli_stop_path_rx_thread(path);
        if (path->ev_socket) {
            event_del(path->ev_socket);
            event_free(path->ev_socket);
            path->ev_socket = NULL;
        }
        if (path->fd >= 0) {
            close(path->fd);
            path->fd = -1;
        }
        
    }
    if (user_conn->ev_rx_queue) {
        event_del(user_conn->ev_rx_queue);
        event_free(user_conn->ev_rx_queue);
        user_conn->ev_rx_queue = NULL;
    }
    if (user_conn->rx_notify_fds[0] >= 0) {
        close(user_conn->rx_notify_fds[0]);
        user_conn->rx_notify_fds[0] = -1;
    }
    if (user_conn->rx_notify_fds[1] >= 0) {
        close(user_conn->rx_notify_fds[1]);
        user_conn->rx_notify_fds[1] = -1;
    }

}

// int main(int argc, char *argv[])
// {
//     int ret;
//     xqc_mini_cli_ctx_t cli_ctx = {0}, *ctx = &cli_ctx;
//     xqc_mini_cli_args_t *args = NULL;
//     xqc_mini_cli_user_conn_t *user_conn = NULL;

//     args = calloc(1, sizeof(xqc_mini_cli_args_t));
//     if (args == NULL) {
//         printf("[error] calloc args failed\n");
//         goto exit;
//     }

//     /* init env (for windows) */
//     xqc_platform_init_env();

//     /* init client environment (ctx & args) */
//     ret = xqc_mini_cli_init_env(ctx, args);
//     if (ret < 0) {
//         goto exit;
//     }
//     ret = xqc_mini_cli_parse_cmd_args(args, argc, argv);
//     if (ret != XQC_OK) {
//         goto exit;
//     }
//     /* init client engine */
//     ret = xqc_mini_cli_init_xquic_engine(ctx, args);
//     if (ret < 0) {
//         printf("[error] init xquic engine failed\n");
//         goto exit;
//     }

//     /* init engine ctx */
//     ret = xqc_mini_cli_init_engine_ctx(ctx);
//     if (ret < 0) {
//         printf("[error] init engine ctx failed\n");
//         goto exit;
//     }

//     user_conn = xqc_mini_cli_user_conn_create(ctx);
//     if (user_conn == NULL) {
//         printf("[error] init user_conn failed.\n");
//         goto exit;
//     }

//     /* cli main process: build connection, process request, etc. */
//     xqc_mini_cli_main_process(user_conn, ctx);

//     /* start event loop */
//     event_base_dispatch(ctx->eb);

// exit:
//     xqc_engine_destroy(ctx->engine);
//     xqc_mini_cli_on_connection_finish(user_conn);
//     xqc_mini_cli_free_ctx(ctx);
//     xqc_mini_cli_free_user_conn(user_conn);

//     return 0;
// }

// 解析命令行参数并写入配置，目前未使用该函数
int
xqc_mini_cli_parse_cmd_args(xqc_mini_cli_args_t *args, int argc, char *argv[])
{
    int opt;


    optind = 1;


     while ((opt = getopt(argc, argv, "i:s:a:p:m:M:u:o:l:")) != -1) {
        switch (opt) {
        
        case 'i':
            if (args->net_cfg.multi_interface_cnt >= MAX_PATH_CNT) {
                printf("[warn] exceed max path count %d, ignore interface %s\n",
                    MAX_PATH_CNT, optarg);
                break;
            }
            memset(args->net_cfg.multi_interface[args->net_cfg.multi_interface_cnt], 0,
                sizeof(args->net_cfg.multi_interface[args->net_cfg.multi_interface_cnt]));
            strncpy(args->net_cfg.multi_interface[args->net_cfg.multi_interface_cnt], optarg,
                XQC_MINI_INTERFACE_NAME_MAX_LEN - 1);
            printf("[stats] option interface[%d]=%s\n",
                args->net_cfg.multi_interface_cnt,
                args->net_cfg.multi_interface[args->net_cfg.multi_interface_cnt]);
            args->net_cfg.multi_interface_cnt++;
            break;
        case 'm':
            if (strcmp(optarg, "minrtt") != 0
                && strcmp(optarg, "backup") != 0
                && strcmp(optarg, "balanced") != 0
                && strcmp(optarg, "rap") != 0
                && strcmp(optarg, "act") != 0
                && strcmp(optarg, "bw") != 0) {
                printf("[warn] unsupported scheduler %s, keep default %s\n",
                    optarg, args->quic_cfg.mp_sched);
                break;
            }

            memset(args->quic_cfg.mp_sched, 0, sizeof(args->quic_cfg.mp_sched));
            strncpy(args->quic_cfg.mp_sched, optarg, sizeof(args->quic_cfg.mp_sched) - 1);
            printf("[stats] multipath scheduler set to %s\n", args->quic_cfg.mp_sched);
            break;
        case 's':
        {
            char *endptr = NULL;
            long stream_cnt = strtol(optarg, &endptr, 10);
            if (endptr == optarg || *endptr != '\0') {
                printf("[warn] invalid stream count '%s', keep default\n", optarg);
                break;
            }
            if (stream_cnt <= 0) {
                printf("[warn] stream count must be positive, keep default\n");
                break;
            }
            if (stream_cnt > XQC_MINI_MAX_STREAMS) {
                printf("[warn] stream count %ld exceeds limit %d, clamp\n", stream_cnt, XQC_MINI_MAX_STREAMS);
                stream_cnt = XQC_MINI_MAX_STREAMS;
            }
            args->req_stream_cnt = (int)stream_cnt;
            printf("[stats] option request streams=%d\n", args->req_stream_cnt);
            break;
        }
        case 'a':
            memset(args->net_cfg.server_addr, 0, sizeof(args->net_cfg.server_addr));
            strncpy(args->net_cfg.server_addr, optarg, sizeof(args->net_cfg.server_addr) - 1);
            printf("[stats] option server addr=%s\n", args->net_cfg.server_addr);
            break;
        case 'p': {
            char *endptr = NULL;
            long port = strtol(optarg, &endptr, 10);
            if (endptr == optarg || *endptr != '\0' || port <= 0 || port > UINT16_MAX) {
                printf("[error] invalid port: %s\n", optarg);
                return XQC_ERROR;
            }
            args->net_cfg.server_port = (unsigned short)port;
            printf("[stats] option server port=%u\n", args->net_cfg.server_port);
            break;
        }
        case 'M': {
            char method_buf[8] = {0};
            size_t len = strlen(optarg);
            if (len >= sizeof(method_buf)) {
                len = sizeof(method_buf) - 1;
            }
            for (size_t i = 0; i < len; i++) {
                method_buf[i] = (char)toupper((unsigned char)optarg[i]);
            }
            if (strcmp(method_buf, "GET") == 0) {
                args->req_cfg.method = REQUEST_METHOD_GET;
                printf("[stats] HTTP method set to GET\n");
            } else if (strcmp(method_buf, "POST") == 0) {
                args->req_cfg.method = REQUEST_METHOD_POST;
                printf("[stats] HTTP method set to POST\n");
            } else {
                printf("[warn] unsupported method %s, keep default %s\n",
                    optarg, method_s[args->req_cfg.method]);
            }
            break;
        }
        case 'u':
            strncpy(args->req_cfg.url, optarg, sizeof(args->req_cfg.url) - 1);
            args->req_cfg.url[sizeof(args->req_cfg.url) - 1] = '\0';
            strncpy(args->req_cfg.path, args->req_cfg.url, sizeof(args->req_cfg.path) - 1);
            args->req_cfg.path[sizeof(args->req_cfg.path) - 1] = '\0';
            printf("[stats] option request path=%s\n", args->req_cfg.url);
            break;
        case 'o':
            strncpy(args->env_cfg.download_path, optarg,
                sizeof(args->env_cfg.download_path) - 1);
            args->env_cfg.download_path[sizeof(args->env_cfg.download_path) - 1] = '\0';
            printf("[stats] option download file=%s\n", args->env_cfg.download_path);
            break;
        case 'l': {
            char *endptr = NULL;
            unsigned long long bytes = strtoull(optarg, &endptr, 10);
            if (endptr == optarg || *endptr != '\0') {
                printf("[warn] invalid payload size '%s', keep default\n", optarg);
                break;
            }
            args->send_data_len = (size_t)bytes;
            printf("[stats] option send data length=%zu bytes\n", args->send_data_len);
            break;
        }
        default:
            break;
        }
    }
    return XQC_OK;
}

static int
xqc_mini_cli_bind_to_interface(int fd, const char *interface_name, int family)
{
#if !defined(XQC_SYS_WINDOWS)
#if !defined(__APPLE__)
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, interface_name, sizeof(ifr.ifr_name) - 1);
    printf("[stats] bind fd %d to interface %s\n", fd, interface_name);
    if (setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, (char *)&ifr, sizeof(ifr)) < 0) {
        printf("[error] bind to nic error: %d\n", errno);
        return XQC_ERROR;
    }
#else
    uint32_t if_index = if_nametoindex(interface_name);
    if (if_index == 0) {
        printf("[error] if_nametoindex failed for %s: %d\n", interface_name, errno);
        return XQC_ERROR;
    }

    printf("[stats] bind fd %d to interface %s (index %u)\n", fd, interface_name, if_index);

    int err = 0;
    if (family == AF_INET || family == AF_UNSPEC) {
        if (setsockopt(fd, IPPROTO_IP, IP_BOUND_IF, &if_index, sizeof(if_index)) == 0) {
            return XQC_OK;
        }
        err = errno;
        if (family == AF_INET) {
            printf("[error] bind to nic error: %d\n", err);
            return XQC_ERROR;
        }
    }

    if (family == AF_INET6 || family == AF_UNSPEC) {
        if (setsockopt(fd, IPPROTO_IPV6, IPV6_BOUND_IF, &if_index, sizeof(if_index)) == 0) {
            return XQC_OK;
        }
        err = errno;
        printf("[error] bind to nic error: %d\n", err);
        return XQC_ERROR;
    }

    printf("[error] unsupported address family %d for binding\n", family);
    return XQC_ERROR;
#endif
#else
    (void)fd;
    (void)interface_name;
    (void)family;
#endif
    return XQC_OK;
}
