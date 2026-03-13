#include "mini_server.h"
#include <stdint.h>
#include <strings.h>
// 去除字符串首尾空白字符
static char *
xqc_mini_svr_trim_space(char *text)
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

// 读取并解析服务器配置文件
static int
xqc_mini_svr_load_config_file(xqc_mini_svr_args_t *args, const char *path)
{
    if (path == NULL || path[0] == '\0') {
        return XQC_ERROR;
    }

    FILE *fp = fopen(path, "r");
    if (fp == NULL) {
        fprintf(stderr, "fopen failed: path='%s', errno=%d (%s)\n",
            path, errno, strerror(errno));
        return XQC_ERROR;
    }

    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        char *line_ptr = xqc_mini_svr_trim_space(line);
        if (line_ptr[0] == '\0' || line_ptr[0] == '#') {
            continue;
        }

        char *eq = strchr(line_ptr, '=');
        if (eq == NULL) {
            continue;
        }

        *eq = '\0';
        char *key = xqc_mini_svr_trim_space(line_ptr);
        char *value = xqc_mini_svr_trim_space(eq + 1);
        if (key[0] == '\0' || value[0] == '\0') {
            continue;
        }

        if (strcmp(key, "listen_addr") == 0 || strcmp(key, "server_addr") == 0) {
            memset(args->net_cfg.ip, 0, sizeof(args->net_cfg.ip));
            strncpy(args->net_cfg.ip, value, sizeof(args->net_cfg.ip) - 1);

        } else if (strcmp(key, "listen_port") == 0 || strcmp(key, "server_port") == 0) {
            char *endptr = NULL;
            long port = strtol(value, &endptr, 10);
            if (endptr != value && *endptr == '\0' && port > 0 && port <= UINT16_MAX) {
                args->net_cfg.port = (unsigned short)port;
            }

        } else if (strcmp(key, "data_dir") == 0) {
            memset(args->env_cfg.data_dir, 0, sizeof(args->env_cfg.data_dir));
            strncpy(args->env_cfg.data_dir, value, sizeof(args->env_cfg.data_dir) - 1);

        }else if (strcmp(key, "stream_range_size") == 0) {
            char *endptr = NULL;
            unsigned long long size = strtoull(value, &endptr, 10);
            if (endptr != value && *endptr == '\0' && size > 0) {
                args->net_cfg.stream_range_size = (size_t)size;
            }
        }
        else if (strcmp(key, "default_send_file") == 0) {
            memset(args->env_cfg.default_send_file, 0,
                sizeof(args->env_cfg.default_send_file));
            strncpy(args->env_cfg.default_send_file, value,
                sizeof(args->env_cfg.default_send_file) - 1);
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
 
        }else if (strcmp(key, "kernel_sndbuf") == 0) {
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
        } else if (strcmp(key, "upload_path") == 0 || strcmp(key, "upload_file") == 0) {
            memset(args->env_cfg.upload_path, 0, sizeof(args->env_cfg.upload_path));
            strncpy(args->env_cfg.upload_path, value, sizeof(args->env_cfg.upload_path) - 1);
        }else if (strcmp(key, "use_zlog") == 0) {
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


// 初始化服务器 SSL 配置（证书/密钥/票据等）
void
xqc_mini_svr_init_ssl_config(xqc_engine_ssl_config_t  *ssl_cfg, xqc_mini_svr_args_t *args)
{
    ssl_cfg->private_key_file = args->env_cfg.private_key_file;
    ssl_cfg->cert_file = args->env_cfg.cert_file;
    ssl_cfg->ciphers = args->quic_cfg.ciphers;
    ssl_cfg->groups = args->quic_cfg.groups;

    /* for server, load session ticket key if there exists */
    if (args->quic_cfg.session_ticket_key_len <= 0) {
        ssl_cfg->session_ticket_key_data = NULL;
        ssl_cfg->session_ticket_key_len = 0;

    } else {
        ssl_cfg->session_ticket_key_data = args->quic_cfg.session_ticket_key_data;
        ssl_cfg->session_ticket_key_len = args->quic_cfg.session_ticket_key_len;
    }
    // xqc_transport_params_t tp;
    // memset(&tp, 0, sizeof(tp));

    // // 调大流控窗口
    // tp.initial_max_data = 2ULL * 1024 * 1024 * 1024;
    // tp.initial_max_stream_data_bidi_local  = 2ULL * 1024 * 1024 * 1024;
    // tp.initial_max_stream_data_bidi_remote = 2ULL * 1024 * 1024 * 1024;
    // tp.initial_max_stream_data_uni         = 2ULL * 1024 * 1024 * 1024;
    // tp.initial_max_streams_bidi = 16;
    // tp.initial_max_streams_uni  = 16;

    // // multipath 如果需要：

    // ssl_cfg.transport_params = tp;
}
// 初始化服务器默认参数
void
xqc_mini_svr_init_args(xqc_mini_svr_args_t *args)
{
    int ret;
    char *p = NULL;

    /* init network args */
    strncpy(args->net_cfg.ip, DEFAULT_IP, sizeof(args->net_cfg.ip) - 1);
    args->net_cfg.port = DEFAULT_PORT;
    args->net_cfg.kernel_sndbuf = 16 * 1024 * 1024;
    args->net_cfg.kernel_revbuf = 16 * 1024 * 1024;
    args->net_cfg.user_send_buf_size = XQC_PACKET_BUF_LEN;
    args->net_cfg.user_recv_buf_size = XQC_PACKET_BUF_LEN;
    args->net_cfg.stream_range_size = 4 * 1024 * 1024;


    /**
     * init quic config
     * it's recommended to replace the constant value with option arguments according to actual needs
     */
    p = args->quic_cfg.session_ticket_key_data;
    ret = xqc_mini_read_file_data(p,
        SESSION_TICKET_KEY_BUF_LEN, SESSION_TICKET_KEY_FILE);
    args->quic_cfg.session_ticket_key_len = ret > 0 ? ret : 0;
    args->quic_cfg.cc = CC_TYPE_BBR;
    args->quic_cfg.multipath = 1;
    strncpy(args->quic_cfg.mp_sched, "bw", 32);
    strncpy(args->quic_cfg.ciphers, XQC_TLS_CIPHERS, CIPHER_SUIT_LEN - 1);
    strncpy(args->quic_cfg.groups, XQC_TLS_GROUPS, TLS_GROUPS_LEN - 1);

    /* init env config */
    strncpy(args->env_cfg.log_path, LOG_PATH, TLS_GROUPS_LEN - 1);
    args->env_cfg.use_zlog = 1;
    strncpy(args->env_cfg.zlog_conf, "zlog.conf", sizeof(args->env_cfg.zlog_conf));
    strncpy(args->env_cfg.zlog_category, "xquic_mini_server", sizeof(args->env_cfg.zlog_category));

    strncpy(args->env_cfg.key_out_path, KEY_PATH, PATH_LEN - 1);
    strncpy(args->env_cfg.private_key_file, PRIV_KEY_PATH, PATH_LEN - 1);
    strncpy(args->env_cfg.cert_file, CERT_PEM_PATH, PATH_LEN - 1);
    strncpy(args->env_cfg.data_dir, SOURCE_DIR, sizeof(args->env_cfg.data_dir) - 1);
    strncpy(args->env_cfg.default_send_file, SERVER_DEFAULT_FILE,
        sizeof(args->env_cfg.default_send_file) - 1);
    
}

/*f参数用于客户端未指定GET文件时，默认的回传文件，而d是数据目录*/
int
xqc_mini_svr_parse_cmd_args(xqc_mini_svr_args_t *args, int argc, char *argv[])
{
    int opt;

    optind = 1;

    while ((opt = getopt(argc, argv, "a:p:f:d:")) != -1) {
        switch (opt) {
        case 'a':
            memset(args->net_cfg.ip, 0, sizeof(args->net_cfg.ip));
            strncpy(args->net_cfg.ip, optarg, sizeof(args->net_cfg.ip) - 1);
            printf("[stats] option listen addr=%s\n", args->net_cfg.ip);
            break;
        case 'p': {
            char *endptr = NULL;
            long port = strtol(optarg, &endptr, 10);
            if (endptr == optarg || *endptr != '\0' || port <= 0 || port > UINT16_MAX) {
                printf("[error] invalid port: %s\n", optarg);
                return XQC_ERROR;
            }
            args->net_cfg.port = (unsigned short)port;
            printf("[stats] option listen port=%u\n", args->net_cfg.port);
            break;
        }
        case 'f':
            memset(args->env_cfg.default_send_file, 0,
                sizeof(args->env_cfg.default_send_file));
            strncpy(args->env_cfg.default_send_file, optarg,
                sizeof(args->env_cfg.default_send_file) - 1);
            printf("[stats] default send file=%s\n",
                args->env_cfg.default_send_file);
            break;
        case 'd':
            memset(args->env_cfg.data_dir, 0, sizeof(args->env_cfg.data_dir));
            strncpy(args->env_cfg.data_dir, optarg,
                sizeof(args->env_cfg.data_dir) - 1);
            printf("[stats] data directory=%s\n", args->env_cfg.data_dir);
            break;
        default:
            break;
        }
    }

    return XQC_OK;
}

// 初始化服务器上下文（事件循环/日志等）
int
xqc_mini_svr_init_ctx(xqc_mini_svr_ctx_t *ctx, xqc_mini_svr_args_t *args)
{
    memset(ctx, 0, sizeof(xqc_mini_svr_ctx_t));

    /* init event base */
    struct event_base *eb = event_base_new();
    ctx->eb = eb;

    ctx->args = args;
    /* init log writer fd */
    // ctx->log_fd = xqc_mini_svr_open_log_file(ctx);
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
        ctx->log_fd = xqc_mini_svr_open_log_file(ctx);
        if (ctx->log_fd < 0) {
            printf("[error] open log file failed\n");
            return XQC_ERROR;
        }
    }

    // /* init keylog writer fd */
    // ctx->keylog_fd = xqc_mini_svr_open_keylog_file(ctx);
    // if (ctx->keylog_fd < 0) {
    //     printf("[error] open keylog file failed\n");
    //     return XQC_ERROR;
    // }
    
    return XQC_OK;
}

/**
 * @brief init engine & transport callbacks
 */
// 初始化引擎与传输层回调集合
void
xqc_mini_svr_init_callback(xqc_engine_callback_t *cb, xqc_transport_callbacks_t *tcb,
    xqc_mini_svr_args_t *args)
{
    static xqc_engine_callback_t callback = {
        .set_event_timer = xqc_mini_svr_set_event_timer,
        // .log_callbacks = {
        //     .xqc_log_write_err = xqc_mini_svr_write_log_file,
        //     .xqc_log_write_stat = xqc_mini_svr_write_log_file,
        //     .xqc_qlog_event_write = xqc_mini_svr_write_qlog_file
        // },
        // .keylog_cb = xqc_mini_svr_keylog_cb,

        /* disable log/keylog output for the mini server */
        // .log_callbacks = {0},
        // .keylog_cb = NULL,
        

    };
    if (args->env_cfg.use_zlog) {
        callback.log_callbacks.xqc_log_write_err = xqc_mini_svr_write_log_file;
        callback.log_callbacks.xqc_log_write_stat = xqc_mini_svr_write_log_file;
        callback.log_callbacks.xqc_qlog_event_write = xqc_mini_svr_write_qlog_file;
        callback.keylog_cb = NULL;
    } else {
    /* disable log/keylog output for the mini server */
        callback.log_callbacks = (xqc_log_callbacks_t){0};
        //callback.log_callbacks.xqc_qlog_event_write = xqc_mini_svr_write_qlog_file;
        callback.keylog_cb = NULL;
    }

    static xqc_transport_callbacks_t transport_cbs = {
        .server_accept = xqc_mini_svr_accept,
        .write_socket = xqc_mini_svr_write_socket,
        .write_socket_ex = xqc_mini_svr_write_socket_ex,
        .conn_update_cid_notify = xqc_mini_svr_conn_update_cid_notify,
    };

    *cb = callback;
    *tcb = transport_cbs;
}

/**
 * @brief init xquic server engine
 */
// 创建并初始化 xquic 服务器引擎
int
xqc_mini_svr_init_xquic_engine(xqc_mini_svr_ctx_t *ctx, xqc_mini_svr_args_t *args)
{
    int ret;
    xqc_config_t egn_cfg;
    xqc_engine_callback_t callback;
    xqc_engine_ssl_config_t ssl_cfg = {0};
    xqc_transport_callbacks_t transport_cbs;

    /* get default parameters of xquic engine */
    ret = xqc_engine_get_default_config(&egn_cfg, XQC_ENGINE_SERVER);
    if (ret < 0) {
        return XQC_ERROR;
    }

    /* init ssl config */
    xqc_mini_svr_init_ssl_config(&ssl_cfg, args);

    /* init engine & transport callbacks */
    xqc_mini_svr_init_callback(&callback, &transport_cbs, args);
    
    /* create server engine */
    ctx->engine = xqc_engine_create(XQC_ENGINE_SERVER, &egn_cfg, &ssl_cfg,
                                    &callback, &transport_cbs, ctx);
    if (ctx->engine == NULL) {
        printf("[error] xqc_engine_create error\n");
        return XQC_ERROR;
    }

    ctx->ev_engine = event_new(ctx->eb, -1, 0, xqc_mini_svr_engine_cb, ctx);

    return XQC_OK;
}

// 初始化服务器环境（参数、配置与上下文）
int
xqc_mini_svr_init_env(xqc_mini_svr_ctx_t *ctx, xqc_mini_svr_args_t *args)
{
    int ret = XQC_OK;

    /* init server args */
    xqc_mini_svr_init_args(args);

    /* load config file defaults if present */
    xqc_mini_svr_load_config_file(args, MINI_SERVER_CONFIG_FILE);

    /* init server ctx */
    ret = xqc_mini_svr_init_ctx(ctx, args);

    return ret;
}
// 根据配置选择拥塞控制回调
xqc_cong_ctrl_callback_t
xqc_mini_svr_get_cc_cb(xqc_mini_svr_args_t *args)
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
// 根据配置选择多路径调度回调
xqc_scheduler_callback_t
xqc_mini_svr_get_sched_cb(xqc_mini_svr_args_t *args)
{
    xqc_scheduler_callback_t sched = xqc_minrtt_scheduler_cb;
    if (strncmp(args->quic_cfg.mp_sched, "minrtt", strlen("minrtt")) == 0) {
        sched = xqc_minrtt_scheduler_cb;

    } else if (strncmp(args->quic_cfg.mp_sched, "backup", strlen("backup")) == 0) {
        sched = xqc_backup_scheduler_cb;
    }
    else if(strncmp(args->quic_cfg.mp_sched, "act", strlen("act")) == 0){
        sched = xqc_act_scheduler_cb;
    }
    else if(strncmp(args->quic_cfg.mp_sched, "bw", strlen("bw")) == 0){
        sched = xqc_bandwidth_scheduler_cb;
    }
    return sched;
}
// 初始化连接层设置（拥塞控制、多路径等）
void
xqc_mini_svr_init_conn_settings(xqc_engine_t *engine, xqc_mini_svr_args_t *args)
{
    /* parse congestion control callback */
    xqc_cong_ctrl_callback_t ccc = xqc_mini_svr_get_cc_cb(args);
    /* parse mp scheduler callback */
    xqc_scheduler_callback_t sched = xqc_mini_svr_get_sched_cb(args);

    /* init connection settings */
    xqc_conn_settings_t conn_settings = {
        .cong_ctrl_callback = ccc,
        .cc_params = {
            .customize_on = 1,
            .init_cwnd = 128,
            .bbr_enable_lt_bw = 1,
        },
        .spurious_loss_detect_on = 1,
        .init_idle_time_out = 60000,
        .enable_multipath = args->quic_cfg.multipath,
        .scheduler_callback = sched,
        .standby_path_probe_timeout = 1000,
        .adaptive_ack_frequency = 0,
        .anti_amplification_limit = 4,
        .recv_rate_bytes_per_sec = 0,
        .mp_ack_on_any_path = 0,
        .mp_enable_reinjection = 0,
        .ack_frequency = 1,
        // .enable_stream_rate_limit = 1,
        .mp_ping_on = 1,
        .init_recv_window =  64 * 1024 * 1024,  // ✅ 2GB 接收窗口
    };

    /* set customized connection settings to engine ctx */
    xqc_server_set_conn_settings(engine, &conn_settings);
}

// 创建并绑定 socket，同时设置内核缓冲区大小
static int
xqc_mini_svr_init_socket(int family, struct sockaddr *local_addr,
        socklen_t local_addrlen, const xqc_mini_svr_net_config_t *cfg)

{
    int size;
    int opt_reuseaddr;
    int flags = 1;
    int fd = socket(family, SOCK_DGRAM, 0);
    if (fd < 0) {
        printf("create socket failed, errno: %d\n", get_sys_errno());
        return XQC_ERROR;
    }

    /* non-block */
#ifdef XQC_SYS_WINDOWS
    if (ioctlsocket(fd, FIONBIO, &flags) == SOCKET_ERROR) {
		goto err;
	}
#else
    if (fcntl(fd, F_SETFL, O_NONBLOCK) == -1) {
        printf("set socket nonblock failed, errno: %d\n", get_sys_errno());
        goto err;
    }
#endif

    /* reuse port */
    opt_reuseaddr = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt_reuseaddr, sizeof(opt_reuseaddr)) < 0) {
        printf("setsockopt failed, errno: %d\n", get_sys_errno());
        goto err;
    }

    /* send/recv buffer size */
    if (cfg && cfg->kernel_revbuf > 0) {
        size = cfg->kernel_revbuf;
        if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &size, sizeof(int)) < 0) {
            printf("setsockopt failed, errno: %d\n", get_sys_errno());
            goto err;
        }

    }

    if (cfg && cfg->kernel_sndbuf > 0) {
        size = cfg->kernel_sndbuf;
        if (setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &size, sizeof(int)) < 0) {
            printf("setsockopt failed, errno: %d\n", get_sys_errno());
            goto err;
        }

    }

    /* bind port */
    if (bind(fd, local_addr, local_addrlen) < 0) {
        printf("bind socket failed, family: %d, errno: %d, %s\n", family, 
            get_sys_errno(), strerror(get_sys_errno()));
        goto err;
    }

    return fd;

err:
    close(fd);
    return -1;
}
// 创建 UDP socket 并绑定到指定地址
static int
xqc_mini_svr_create_socket(xqc_mini_svr_user_conn_t *user_conn, xqc_mini_svr_net_config_t* cfg)
{
    /* ipv4 socket */
    user_conn->local_addr->sin_family = AF_INET;
    user_conn->local_addr->sin_port = htons(cfg->port);
     if (strlen(cfg->ip) == 0 || strcmp(cfg->ip, "0.0.0.0") == 0) {
        user_conn->local_addr->sin_addr.s_addr = htonl(INADDR_ANY);

    } else {
        struct in_addr listen_addr = {0};
        if (inet_pton(AF_INET, cfg->ip, &listen_addr) != 1) {
            printf("[error] invalid listen ip: %s\n", cfg->ip);
            return -1;
        }
        user_conn->local_addr->sin_addr = listen_addr;
    }
    user_conn->local_addrlen = sizeof(struct sockaddr_in);
    user_conn->fd = xqc_mini_svr_init_socket(AF_INET, (struct sockaddr*)user_conn->local_addr,
        user_conn->local_addrlen, cfg);

    printf("[stats] create ipv4 socket fd: %d success, bind socket to ip: %s, port: %u\n", user_conn->fd, cfg->ip, cfg->port);

    if (!user_conn->fd) {
        return -1;
    }

    return 0;
}
// 初始化 HTTP/3(ALPN) 上下文
int
xqc_mini_svr_init_alpn_ctx(xqc_engine_t *engine)
{
    int ret = 0;

    /* init http3 callbacks */
    xqc_h3_callbacks_t h3_cbs = {
        .h3c_cbs = {
            .h3_conn_create_notify = xqc_mini_svr_h3_conn_create_notify,
            .h3_conn_close_notify = xqc_mini_svr_h3_conn_close_notify,
            .h3_conn_handshake_finished = xqc_mini_svr_h3_conn_handshake_finished,
        },
        .h3r_cbs = {
            .h3_request_create_notify = xqc_mini_svr_h3_request_create_notify,
            .h3_request_close_notify = xqc_mini_svr_h3_request_close_notify,
            .h3_request_read_notify = xqc_mini_svr_h3_request_read_notify,
            .h3_request_write_notify = xqc_mini_svr_h3_request_write_notify,
        }
    };

    /* init http3 context */
    ret = xqc_h3_ctx_init(engine, &h3_cbs);
    if (ret != XQC_OK) {
        printf("init h3 context error, ret: %d\n", ret);
        return ret;
    }

    return ret;
}
// 初始化引擎上下文（连接配置与 ALPN）
int
xqc_mini_svr_init_engine_ctx(xqc_mini_svr_ctx_t *ctx, xqc_mini_svr_args_t *args)
{
    int ret;

    /* init connection settings */
    xqc_mini_svr_init_conn_settings(ctx->engine, args);

    /* init alpn ctx */
    ret = xqc_mini_svr_init_alpn_ctx(ctx->engine);

    return ret;
}

// 处理 socket 可写事件
void
xqc_mini_svr_socket_write_handler(xqc_mini_svr_user_conn_t *user_conn, int fd)
{
    DEBUG
    printf("[stats] socket write handler\n");
}
// 处理 socket 可读事件并投递给引擎
void
xqc_mini_svr_socket_read_handler(xqc_mini_svr_user_conn_t *user_conn, int fd)
{
    //static size_t total_recv = 0;   // 累计接收字节数
    ssize_t recv_size;
    struct sockaddr_storage peer_addr;
    size_t packet_cnt = 0;
    socklen_t peer_addrlen = sizeof(peer_addr);
    xqc_mini_svr_ctx_t *ctx = user_conn->ctx;

    size_t buffer_capacity = ctx->args->net_cfg.user_recv_buf_size > 0
        ? ctx->args->net_cfg.user_recv_buf_size
        : XQC_PACKET_BUF_LEN;
    unsigned char packet_buf[XQC_PACKET_BUF_LEN];
    if (packet_buf == NULL) {
        printf("[error] allocate recv buffer failed\n");
        return;
    }


    ctx->current_fd = fd;

    for (;;) {
        if (packet_cnt >= XQC_SOCKET_READ_BUDGET_PER_EVENT) {
            break;
        }

        recv_size = recvfrom(fd, packet_buf, sizeof(packet_buf), 0,
                             (struct sockaddr *)&peer_addr, &peer_addrlen);

        if (recv_size < 0) {
            if (get_sys_errno() == EAGAIN || get_sys_errno() == EWOULDBLOCK) {
                break;  // 已读完
            }
            perror("[server recv] recvfrom error");
            break;
        }

        if (recv_size == 0) {
            // UDP 通常不会有 0 字节，这里仅作保护
            break;
        }
        
        struct sockaddr_in *pa = (struct sockaddr_in*)&peer_addr;
         
        char peer_ip[INET_ADDRSTRLEN];

        inet_ntop(AF_INET, &pa->sin_addr, peer_ip, sizeof peer_ip);
        //printf(">>>recv from %s: %ld bytes.\n",peer_ip,recv_size);

        memcpy(user_conn->peer_addr, &peer_addr, peer_addrlen);
        user_conn->peer_addrlen = peer_addrlen;
        user_conn->local_addrlen = sizeof(struct sockaddr_in6);

        if (getsockname(user_conn->fd,
                        (struct sockaddr *)user_conn->local_addr,
                        &user_conn->local_addrlen) != 0) {
            perror("[server recv] getsockname error");
        }
        packet_cnt++;
        // 🚀 核心：将 UDP 包交给 XQUIC 引擎
        xqc_int_t ret = xqc_engine_packet_process(
            ctx->engine,
            packet_buf,
            recv_size,
            (struct sockaddr *)user_conn->local_addr,
            user_conn->local_addrlen,
            (struct sockaddr *)user_conn->peer_addr,
            user_conn->peer_addrlen,
            xqc_now(),
            user_conn
        );

        if (ret != XQC_OK) {
            fprintf(stderr, "[error] xqc_engine_packet_process ret=%d\n", ret);
        }

        // total_recv += recv_size;
        // // 每 10MB 打印一次
        // if (total_recv % (10 * 1024 * 1024) < recv_size) {
        //     printf("[server socket] total UDP received: %.2f MB\n",
        //            (double)total_recv / (1024 * 1024));
        // }    
    }

    // ✅ 通知 QUIC 引擎本轮接收完成
    xqc_engine_finish_recv(ctx->engine);
}
// libevent socket 事件回调分发
static void
xqc_mini_svr_socket_event_callback(int fd, short what, void *arg)
{
    //DEBUG;
    xqc_mini_svr_user_conn_t *user_conn = (xqc_mini_svr_user_conn_t *)arg;
    if (what & EV_WRITE) {
        xqc_mini_svr_socket_write_handler(user_conn, fd);

    } else if (what & EV_READ) {
        xqc_mini_svr_socket_read_handler(user_conn, fd);

    } else {
        printf("event callback: fd=%d, what=%d\n", fd, what);
        exit(1);
    }
}
// 释放服务器上下文资源
void
xqc_mini_svr_free_ctx(xqc_mini_svr_ctx_t *ctx)
{
    xqc_mini_svr_close_keylog_file(ctx);
    xqc_mini_svr_close_log_file(ctx);
    if (ctx->args && ctx->args->env_cfg.use_zlog && ctx->zlog_cat != NULL) {
        zlog_fini();
        ctx->zlog_cat = NULL;
    }

    if (ctx->args) {
        free(ctx->args);
        ctx->args = NULL;
    }

}

// 释放用户连接资源
void
xqc_mini_svr_free_user_conn(xqc_mini_svr_user_conn_t *user_conn)
{
    if (user_conn->local_addr) {
        free(user_conn->local_addr);
        user_conn->local_addr = NULL;
    }
    if (user_conn->peer_addr) {
        free(user_conn->peer_addr);
        user_conn->peer_addr = NULL;
    }
    if (user_conn) {
        free(user_conn);
        user_conn = NULL;
    }
}
// 创建并初始化用户连接结构
xqc_mini_svr_user_conn_t *
xqc_mini_svr_create_user_conn(xqc_mini_svr_ctx_t *ctx)
{
    int ret;
    xqc_mini_svr_user_conn_t *user_conn = calloc(1, sizeof(xqc_mini_svr_user_conn_t));

    user_conn->ctx = ctx;

    user_conn->local_addr = (struct sockaddr_in *)calloc(1, sizeof(struct sockaddr_in));
    user_conn->peer_addr = (struct sockaddr_in *)calloc(1, sizeof(struct sockaddr_in));
    
    /* init server socket and save to ctx->fd */
    ret = xqc_mini_svr_create_socket(user_conn, &ctx->args->net_cfg);
    if (ret < 0) {
        printf("[error] xqc_create_socket error\n");
        goto error;
    }

    /* bind socket event callback to fd event */
    user_conn->ev_socket = event_new(ctx->eb, user_conn->fd, EV_READ | EV_PERSIST,
        xqc_mini_svr_socket_event_callback, user_conn);
    event_add(user_conn->ev_socket, NULL);

    return user_conn;
error:
    xqc_mini_svr_free_user_conn(user_conn);
    return NULL;
}

// 连接结束时清理事件
void
xqc_mini_cli_on_connection_finish(xqc_mini_svr_user_conn_t *user_conn)
{
    if (user_conn->ev_timeout) {
        event_del(user_conn->ev_timeout);
        user_conn->ev_timeout = NULL;
    }

    if (user_conn->ev_socket) {
        event_del(user_conn->ev_socket);
        user_conn->ev_timeout = NULL;
    }
}

// int
// main(int argc, char *argv[])
// {
//     int ret;
//     xqc_mini_svr_ctx_t svr_ctx = {0}, *ctx = &svr_ctx;
//     xqc_mini_svr_args_t *args = NULL;
//     xqc_mini_svr_user_conn_t *user_conn = NULL;

//     args = calloc(1, sizeof(xqc_mini_svr_args_t));
//     if (args == NULL) {
//         printf("[error] calloc args failed\n");
//         goto exit;
//     }

//     /* init env (for windows) */
//     xqc_platform_init_env();

//     /* init server environment */
//     ret = xqc_mini_svr_init_env(ctx, args);
//     if (ret < 0) {
//         printf("[error] init server environment failed\n");
//         goto exit;
//     }

//     ret = xqc_mini_svr_parse_cmd_args(args, argc, argv);
//     if (ret != XQC_OK) {
//         goto exit;
//     }
//     /* create & init engine to ctx->engine */
//     ret = xqc_mini_svr_init_xquic_engine(ctx, args);
//     if (ret < 0) {
//         printf("[error] init xquic engine failed\n");
//         goto exit;
//     }

//     /* init engine ctx */
//     ret = xqc_mini_svr_init_engine_ctx(ctx, args);
//     if (ret < 0) {
//         printf("[error] init engine ctx failed\n");
//         goto exit;
//     }

//     /* initiate user_conn */
//     user_conn = xqc_mini_svr_create_user_conn(ctx);

//     /* start event loop */
//     event_base_dispatch(ctx->eb);

// exit:
//     xqc_engine_destroy(ctx->engine);
//     xqc_mini_svr_free_ctx(ctx);
//     if (user_conn) {
//         xqc_mini_svr_free_user_conn(user_conn);
//     }

//     return 0;
// }