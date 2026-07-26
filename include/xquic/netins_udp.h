#ifndef NETINS_UDP_API_H
#define NETINS_UDP_API_H

/**
 * @file netins_udp.h
 * @brief XQUIC UDP/QUIC 独立客户端连接与状态查询接口。
 *
 * 本接口既可读取 mini_client 和 mini_server 已经建立的真实 XQUIC 连接，也可
 * 由第三方创建一个独立 QUIC/H3 客户端连接。状态部分主要输出连接生命周期、
 * 多路径状态、每条路径地址、收发字节、RTT、丢包率和区间速率等信息。
 *
 * 与 TCP/MPTCP 版本的 netins 接口不同：
 * 1. netins_udp_client_* 可独立创建和关闭客户端连接；
 * 2. netins_udp_stream_* 在独立客户端连接上发送 HTTP/3 请求和收发业务正文；
 * 3. mini_client/mini_server 已有连接仍由原程序和 Libevent 线程管理；
 * 4. 状态 getter 只读取缓存快照，不能修改调度、拥塞或已有 mini 连接；
 * 5. 注册表只在当前进程内有效，connection_id 不是 QUIC CID；
 * 6. 第三方代码必须与连接宿主使用同一份 netins_udp 实现。
 *
 * 线程说明：所有公开查询函数均可从第三方线程调用。查询函数只复制互斥锁
 * 保护的缓存快照，不会在第三方线程中调用 XQUIC 引擎。mini 程序在
 * XQUIC/Libevent 线程中刷新快照，独立客户端在自己的 poll 引擎线程中刷新；
 * 当前刷新周期均约为 500 ms，因此第三方通常每 500 ms 或 1 s 查询一次即可。
 *
 * 输出结构体说明：调用 getter 前必须使用对应的 INITIALIZER 初始化，例如：
 *
 *     netins_udp_snapshot_t snapshot = NETINS_UDP_SNAPSHOT_INITIALIZER;
 *     int ret = netins_udp_observer_get_snapshot(handle, &snapshot);
 *
 * 除 loss_rate 外，所有 *_mbps 字段单位均为 Mbit/s；所有 *_us 字段单位均为
 * 微秒；所有 bytes_* 字段单位均为字节。
 */

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

/** 当前公共 API/ABI 版本。 */
#define NETINS_UDP_API_VERSION            1U
/** 单个连接最多输出的路径数量，与当前 XQUIC 路径上限一致。 */
#define NETINS_UDP_MAX_PATHS              8U
/** 独立客户端最多同时持有的 HTTP/3 stream 数量。 */
#define NETINS_UDP_MAX_STREAMS            128U
/** ALPN 字符串缓冲区长度。 */
#define NETINS_UDP_ALPN_LEN               64U
/** XQUIC 连接诊断字符串缓冲区长度。 */
#define NETINS_UDP_CONN_INFO_LEN          1024U
/** XQUIC 扩展连接诊断字符串缓冲区长度。 */
#define NETINS_UDP_EXTERN_CONN_INFO_LEN   1024U
/** 网卡接口名缓冲区长度，例如 rmnet_data0 或 usb0。 */
#define NETINS_UDP_INTERFACE_NAME_LEN     32U
/** IP:port 地址字符串缓冲区长度。 */
#define NETINS_UDP_ADDRESS_LEN            128U
/** 错误信息字符串缓冲区长度。 */
#define NETINS_UDP_ERROR_MSG_LEN          256U
/** 独立客户端服务器地址缓冲区长度。 */
#define NETINS_UDP_SERVER_ADDRESS_LEN     128U
/** 独立客户端 TLS SNI/主机名缓冲区长度。 */
#define NETINS_UDP_SERVER_NAME_LEN        256U
/** 当前独立客户端连接接口的版本。 */
#define NETINS_UDP_CLIENT_API_VERSION     1U
/** 当前 HTTP/3 stream 收发接口的版本。 */
#define NETINS_UDP_STREAM_API_VERSION     1U
/** HTTP 方法缓冲区长度。 */
#define NETINS_UDP_METHOD_LEN             16U
/** HTTP scheme 缓冲区长度。 */
#define NETINS_UDP_SCHEME_LEN             16U
/** HTTP 请求路径缓冲区长度。 */
#define NETINS_UDP_REQUEST_PATH_LEN       1024U
/** HTTP Content-Type 缓冲区长度。 */
#define NETINS_UDP_CONTENT_TYPE_LEN       128U
/** 无效的进程内 connection_id。 */
#define NETINS_UDP_INVALID_CONNECTION_ID  UINT64_MAX

/** 不透明的连接状态观察器，第三方不能直接访问其内部成员。 */
typedef struct netins_udp_observer_s netins_udp_observer_t;
/** 不透明的独立 QUIC/H3 客户端对象。 */
typedef struct netins_udp_client_s netins_udp_client_t;
/** 不透明的 HTTP/3 请求/stream 对象。 */
typedef struct netins_udp_stream_s netins_udp_stream_t;

/** API 函数返回值。 */
typedef enum netins_udp_result_e {
    NETINS_UDP_OK = 0,                 /**< 调用成功。 */
    NETINS_UDP_ERROR = -1,             /**< 通用错误，详细信息通过 last_error 获取。 */
    NETINS_UDP_NOT_FOUND = -2,         /**< 连接或路径不存在，可能已经关闭。 */
    NETINS_UDP_BUFFER_TOO_SMALL = -3,  /**< 输出缓冲区或 ID 数组容量不足。 */
    NETINS_UDP_VERSION_MISMATCH = -4,  /**< API 版本或结构体大小不匹配。 */
    NETINS_UDP_WOULD_BLOCK = -5,       /**< 暂无数据/空间，或等待超时，可重试。 */
    NETINS_UDP_CLOSED = -6             /**< stream 或连接已经关闭。 */
} netins_udp_result_t;

/** 当前连接位于客户端还是服务端。 */
typedef enum netins_udp_role_e {
    NETINS_UDP_ROLE_UNKNOWN = 0, /**< 尚未绑定真实连接。 */
    NETINS_UDP_ROLE_CLIENT = 1,  /**< mini_client 中的连接。 */
    NETINS_UDP_ROLE_SERVER = 2   /**< mini_server 中的连接。 */
} netins_udp_role_t;

/** 连接生命周期状态。 */
typedef enum netins_udp_conn_state_e {
    NETINS_UDP_STATE_NEW = 0,         /**< 句柄已创建，但尚未绑定 XQUIC。 */
    NETINS_UDP_STATE_CONNECTING = 1,  /**< 已绑定，QUIC 握手尚未完成。 */
    NETINS_UDP_STATE_CONNECTED = 2,   /**< QUIC/H3 握手已经完成。 */
    NETINS_UDP_STATE_CLOSING = 3,     /**< 连接正在关闭。 */
    NETINS_UDP_STATE_CLOSED = 4,      /**< 正常关闭，可由已持有句柄读取最终快照。 */
    NETINS_UDP_STATE_ERROR = 5        /**< 因错误关闭或状态接口发生错误。 */
} netins_udp_conn_state_t;

/** XQUIC 传输层路径状态。 */
typedef enum netins_udp_path_state_e {
    NETINS_UDP_PATH_INIT = 0,        /**< 路径刚创建，尚未开始或完成验证。 */
    NETINS_UDP_PATH_VALIDATING = 1,  /**< 正在交换 PATH_CHALLENGE/RESPONSE。 */
    NETINS_UDP_PATH_ACTIVE = 2,      /**< 路径验证成功，可参与传输。 */
    NETINS_UDP_PATH_CLOSING = 3,     /**< 路径正在关闭。 */
    NETINS_UDP_PATH_CLOSED = 4       /**< 路径已经关闭。 */
} netins_udp_path_state_t;

/** 应用层对路径设置的可用状态。 */
typedef enum netins_udp_app_path_status_e {
    NETINS_UDP_APP_PATH_NONE = 0,       /**< 尚未设置应用状态。 */
    NETINS_UDP_APP_PATH_STANDBY = 1,    /**< 备用路径，通常不优先承载数据。 */
    NETINS_UDP_APP_PATH_AVAILABLE = 2,  /**< 应用允许调度器使用该路径。 */
    NETINS_UDP_APP_PATH_FROZEN = 3      /**< 路径被冻结，不应继续承载数据。 */
} netins_udp_app_path_status_t;

/** netins_udp 自身的日志级别。 */
typedef enum netins_udp_log_level_e {
    NETINS_UDP_LOG_LEVEL_DEBUG = 0, /**< 调试信息。 */
    NETINS_UDP_LOG_LEVEL_INFO = 1,  /**< 正常状态信息。 */
    NETINS_UDP_LOG_LEVEL_WARN = 2,  /**< 可恢复的异常。 */
    NETINS_UDP_LOG_LEVEL_ERROR = 3  /**< 错误信息。 */
} netins_udp_log_level_t;

/** HTTP/3 stream 生命周期。 */
typedef enum netins_udp_stream_state_e {
    NETINS_UDP_STREAM_STATE_NEW = 0,      /**< 对象已创建，尚未建立 XQUIC stream。 */
    NETINS_UDP_STREAM_STATE_OPENING = 1,  /**< 正在创建 stream 或发送请求头。 */
    NETINS_UDP_STREAM_STATE_OPEN = 2,     /**< 请求头已发送，可以收发正文。 */
    NETINS_UDP_STREAM_STATE_CLOSING = 3,  /**< 正在主动关闭。 */
    NETINS_UDP_STREAM_STATE_CLOSED = 4,   /**< 正文结束或 stream 已关闭。 */
    NETINS_UDP_STREAM_STATE_ERROR = 5     /**< stream 因错误关闭。 */
} netins_udp_stream_state_t;

/**
 * 独立 QUIC/H3 客户端配置。
 *
 * 第三方应先调用 netins_udp_client_config_init()，再填写服务器地址、端口
 * 和可选网卡。当前实现由客户端对象自己创建 XQUIC engine、UDP socket
 * 和后台事件线程，不依赖 mini_client。
 */
typedef struct netins_udp_client_config_s {
    /** 结构体字节大小。 */
    uint32_t struct_size;
    /** NETINS_UDP_CLIENT_API_VERSION。 */
    uint32_t api_version;
    /** 服务器 IP 地址或可由 getaddrinfo() 解析的主机名。 */
    char server_address[NETINS_UDP_SERVER_ADDRESS_LEN];
    /** 服务器 UDP 端口。 */
    uint16_t server_port;
    /** 对齐预留字段，必须保持为 0。 */
    uint16_t reserved16;
    /** TLS SNI/HTTP 主机名；为空时使用 server_address。 */
    char server_name[NETINS_UDP_SERVER_NAME_LEN];
    /**
     * 按优先顺序配置的本地网卡名。interface_count=0 时使用系统默认路由。
     * 当前实现支持在 XQUIC 允许创建路径后使用后续接口建立多路径。
     */
    char interface_names[NETINS_UDP_MAX_PATHS]
        [NETINS_UDP_INTERFACE_NAME_LEN];
    /** interface_names[] 中有效元素数量。 */
    uint32_t interface_count;
    /** 非 0 时协商并尝试建立多路径。 */
    int enable_multipath;
    /** connect() 等待握手的最长时间，0 使用默认 20000 ms。 */
    uint32_t connect_timeout_ms;
    /** SO_SNDBUF 字节数，0 使用默认 16 MiB。 */
    int kernel_sndbuf;
    /** SO_RCVBUF 字节数，0 使用默认 16 MiB。 */
    int kernel_rcvbuf;
    /** 初始拥塞窗口包数，0 使用当前项目默认值 100。 */
    uint32_t initial_cwnd_packets;
    /** 1 表示校验证书；0 保持当前测试环境的不强制校验行为。 */
    int verify_certificate;
    /** ABI 预留字段，第三方不得使用。 */
    uint64_t reserved[8];
} netins_udp_client_config_t;

/** 独立客户端配置的零值初始化器；推荐再调用 config_init() 设置默认值。 */
#define NETINS_UDP_CLIENT_CONFIG_INITIALIZER \
    { sizeof(netins_udp_client_config_t), NETINS_UDP_CLIENT_API_VERSION }

/**
 * HTTP/3 请求配置。
 *
 * 当前接口面向 GET 和 POST。POST 必须在 open 前提供准确的
 * request_body_length；这也让接口能与当前 mini_server 的 Content-Length
 * 和单分片上传元数据规则直接兼容。
 */
typedef struct netins_udp_stream_config_s {
    uint32_t struct_size;
    uint32_t api_version;
    /** HTTP 方法，默认 "GET"。 */
    char method[NETINS_UDP_METHOD_LEN];
    /** HTTP scheme，默认 "https"。 */
    char scheme[NETINS_UDP_SCHEME_LEN];
    /** 请求路径，默认 "/"。 */
    char path[NETINS_UDP_REQUEST_PATH_LEN];
    /** HTTP authority/host；为空时使用客户端 server_name/server_address。 */
    char authority[NETINS_UDP_SERVER_NAME_LEN];
    /** 请求正文类型，默认 "application/octet-stream"。 */
    char content_type[NETINS_UDP_CONTENT_TYPE_LEN];
    /** POST 请求正文总字节数；GET 必须为 0。 */
    uint64_t request_body_length;
    /** 调用方尚未被 XQUIC 消费的发送数据上限，0 使用默认 4 MiB。 */
    size_t send_queue_limit_bytes;
    /** 等待调用方读取的响应正文上限，0 使用默认 16 MiB。 */
    size_t recv_queue_limit_bytes;
    uint64_t reserved[8];
} netins_udp_stream_config_t;

#define NETINS_UDP_STREAM_CONFIG_INITIALIZER \
    { sizeof(netins_udp_stream_config_t), NETINS_UDP_STREAM_API_VERSION }

/** HTTP/3 stream 的实时状态和业务字节计数。 */
typedef struct netins_udp_stream_info_s {
    uint32_t struct_size;
    uint32_t api_version;
    /** netins_udp_stream_state_t。 */
    int state;
    /** HTTP 响应状态码；尚未收到响应头时为 0。 */
    int response_status;
    int request_headers_sent;
    int response_headers_received;
    int send_finished;
    int recv_finished;
    /** XQUIC stream 错误码；正常时为 0。 */
    int stream_error;
    uint32_t reserved32;
    /** XQUIC stream_id；创建完成前为 UINT64_MAX。 */
    uint64_t stream_id;
    uint64_t request_body_length;
    /** 已由 netins_udp_stream_send() 接受并复制的正文总量。 */
    uint64_t bytes_accepted;
    /** 已由 XQUIC 接受的请求正文总量。 */
    uint64_t bytes_sent;
    /** 已从 XQUIC 读取并进入接收队列的响应正文总量。 */
    uint64_t bytes_received;
    /** 已通过 netins_udp_stream_recv() 交付给调用方的正文总量。 */
    uint64_t bytes_delivered;
    /** 当前发送、接收队列中的字节数。 */
    size_t send_queue_bytes;
    size_t recv_queue_bytes;
    /** 响应 Content-Length；响应没有该字段时为 0。 */
    uint64_t response_content_length;
    char response_content_type[NETINS_UDP_CONTENT_TYPE_LEN];
    uint64_t reserved[8];
} netins_udp_stream_info_t;

#define NETINS_UDP_STREAM_INFO_INITIALIZER \
    { sizeof(netins_udp_stream_info_t), NETINS_UDP_STREAM_API_VERSION }

/**
 * 日志回调。
 * @param level netins_udp_log_level_t 日志级别。
 * @param message 只在本次回调期间有效的字符串。
 * @param user_data 注册回调时传入的第三方上下文。
 * @note 回调可能在 XQUIC/Libevent 线程执行，不能长时间阻塞。
 */
typedef void (*netins_udp_logger_cb)(int level, const char *message,
    void *user_data);

/** 连接身份、生命周期和多路径状态。 */
typedef struct netins_udp_info_s {
    /** 结构体字节大小，由 NETINS_UDP_INFO_INITIALIZER 设置。 */
    uint32_t struct_size;
    /** 调用方使用的 API 版本，由 INITIALIZER 设置。 */
    uint32_t api_version;
    /** 进程内连接编号；不是 QUIC CID，进程重启后不可继续使用。 */
    uint64_t connection_id;

    /** netins_udp_role_t，表示客户端或服务端。 */
    int role;
    /** netins_udp_conn_state_t，表示当前连接生命周期。 */
    int connection_state;
    /** 1 表示 QUIC/H3 握手完成，0 表示尚未完成。 */
    int handshake_finished;
    /** 当前实现固定为 1，表示底层传输使用 UDP。 */
    int use_udp;
    /** 当前实现固定为 1，表示 UDP 上运行 QUIC。 */
    int use_quic;
    /**
     * XQUIC 多路径协商结果：
     * 0 = 未启用多路径；1 = Single PNS 多路径；2 = Multiple PNS 多路径。
     */
    int multipath_mode;
    /**
     * 双路径建立结果：
     * 0 = 未尝试建立第二条路径；
     * 1 = 已建立且至少两条路径验证成功；
     * 2 = 尝试建立第二条路径，但验证尚未成功或已经失败。
     */
    int mp_state;
    /** XQUIC 连接错误码；正常连接和正常关闭时通常为 0。 */
    int conn_error;

    /** 当前快照中实际输出到 paths[] 的路径数量。 */
    uint32_t path_count;
    /** transport_state 为 ACTIVE 的路径数量。 */
    uint32_t active_path_count;
    /** transport_state 为 VALIDATING 的路径数量。 */
    uint32_t validating_path_count;
    /** app_status 为 AVAILABLE 的路径数量。 */
    uint32_t available_path_count;
    /** app_status 为 STANDBY 的路径数量。 */
    uint32_t standby_path_count;
    /** app_status 为 FROZEN 的路径数量。 */
    uint32_t frozen_path_count;

    /** 协商完成的应用层协议，例如 "h3"。 */
    char alpn[NETINS_UDP_ALPN_LEN];
    /** XQUIC 生成的连接诊断文本，仅供展示，不应作为稳定格式解析。 */
    char conn_info[NETINS_UDP_CONN_INFO_LEN];
    /** XQUIC 生成的扩展诊断文本，仅供展示，不应作为稳定格式解析。 */
    char extern_conn_info[NETINS_UDP_EXTERN_CONN_INFO_LEN];
    /** ABI 预留字段，第三方不得读取、写入或假设其含义。 */
    uint64_t reserved[8];
} netins_udp_info_t;

/** 所有路径汇总后的连接级累计统计和区间速率。 */
typedef struct netins_udp_statis_s {
    /** 结构体字节大小，由 NETINS_UDP_STATIS_INITIALIZER 设置。 */
    uint32_t struct_size;
    /** 调用方使用的 API 版本，由 INITIALIZER 设置。 */
    uint32_t api_version;
    /** CLOCK_MONOTONIC 快照时间，单位微秒，不是日期或 Unix 时间戳。 */
    uint64_t snapshot_time_us;
    /** 本次和上次内部刷新之间的间隔，单位微秒。 */
    uint64_t sample_interval_us;

    /** 携带 STREAM/DATAGRAM 的已编码发送字节，包含重传和重注入子集。 */
    uint64_t bytes_sent;
    /** 所有已发送 QUIC 包的线上字节，包括 ACK、PING 等控制包。 */
    uint64_t bytes_wire_sent;
    /** 发送端通过 ACK 交付统计确认的累计字节。 */
    uint64_t bytes_delivered;
    /** bytes_wire_sent 中属于重传包的字节子集，不能再次与总量相加。 */
    uint64_t bytes_retransmitted;
    /** 收到的携带 STREAM/DATAGRAM 的已编码包字节，可能包含重复数据。 */
    uint64_t bytes_recv;
    /**
     * xqc_stream_recv() 向上层移动的、按 stream offset 去重后的 STREAM
     * payload；不等于精确文件正文。
     */
    uint64_t bytes_recv_effective;
    /** bytes_sent 中属于多路径重注入副本的字节子集。 */
    uint64_t bytes_sent_reinject;
    /** bytes_recv 中属于重注入副本包的已编码字节子集。 */
    uint64_t bytes_recv_reinject;
    /** 有效接收数据中由重注入副本成功交付的 STREAM 载荷字节。 */
    uint64_t bytes_recv_effective_reinject;

    /** 所有路径累计发送的 QUIC 包数量。 */
    uint64_t packets_sent;
    /** 所有路径累计接收的 QUIC 包数量。 */
    uint64_t packets_recv;
    /** 拥塞/丢包检测累计标记为丢失的包数量。 */
    uint64_t packets_lost;
    /** 后续又被确认的误判丢包数量。 */
    uint64_t packets_spurious_lost;
    /** 被判定丢失的 QUIC DATAGRAM 数量。 */
    uint64_t datagrams_lost;
    /** 连接初始路径的平滑 RTT，单位微秒；无样本时为 0。 */
    uint64_t srtt_us;
    /** 连接初始路径观测到的最小 RTT，单位微秒；无样本时为 0。 */
    uint64_t min_rtt_us;
    /** 所有路径当前未被 ACK 确认的在途字节之和。 */
    uint64_t inflight_bytes;

    /** bytes_sent 在 sample_interval_us 内的增量速率，单位 Mbit/s。 */
    double send_mbps;
    /** bytes_wire_sent 在采样区间内的增量速率，单位 Mbit/s。 */
    double wire_send_mbps;
    /** bytes_delivered 在采样区间内的增量速率，单位 Mbit/s。 */
    double delivery_mbps;
    /** bytes_recv 在采样区间内的增量速率，单位 Mbit/s。 */
    double recv_mbps;
    /** bytes_recv_effective 在采样区间内的增量速率，单位 Mbit/s。 */
    double effective_recv_mbps;
    /** 累计丢包比例 packets_lost / packets_sent，范围通常为 0.0 到 1.0。 */
    double loss_rate;
    /** ABI 预留字段，第三方不得使用。 */
    uint64_t reserved[8];
} netins_udp_statis_t;

/** 单条 XQUIC 路径的状态、累计统计和区间速率。 */
typedef struct netins_udp_path_statis_s {
    /** 结构体字节大小，由 NETINS_UDP_PATH_STATIS_INITIALIZER 设置。 */
    uint32_t struct_size;
    /** 调用方使用的 API 版本，由 INITIALIZER 设置。 */
    uint32_t api_version;
    /** XQUIC path_id；不等于 paths[] 数组下标，不应假设永远只有 0 和 1。 */
    uint64_t path_id;
    /** netins_udp_path_state_t，表示传输层路径状态。 */
    int transport_state;
    /** netins_udp_app_path_status_t，表示应用层路径可用状态。 */
    int app_status;

    /**
     * 客户端绑定的网卡名，例如 rmnet_data0、usb0。
     * 服务端无法确定接口名或路径尚未完成映射时可能为空字符串。
     */
    char interface_name[NETINS_UDP_INTERFACE_NAME_LEN];
    /** 本地 IP 和端口，例如 "10.0.0.2:41000"；尚未知时为空。 */
    char local_address[NETINS_UDP_ADDRESS_LEN];
    /** 对端 IP 和端口，例如 "1.2.3.4:8443"；尚未知时为空。 */
    char peer_address[NETINS_UDP_ADDRESS_LEN];

    /** 本路径累计发送的 QUIC 包数量。 */
    uint64_t packets_sent;
    /** 本路径累计接收的 QUIC 包数量。 */
    uint64_t packets_recv;
    /** 本路径携带 STREAM/DATAGRAM 的已编码发送字节。 */
    uint64_t bytes_sent;
    /** 本路径所有已发送 QUIC 包的线上字节。 */
    uint64_t bytes_wire_sent;
    /** 本路径通过 ACK 交付统计确认的累计字节。 */
    uint64_t bytes_delivered;
    /** 本路径发送字节中属于多路径重注入副本的子集。 */
    uint64_t bytes_sent_reinject;
    /** 本路径发送字节中属于重传包的子集。 */
    uint64_t bytes_sent_retransmitted;
    /** 本路径收到的携带 STREAM/DATAGRAM 的已编码包字节。 */
    uint64_t bytes_recv;
    /** 本路径接收字节中属于重注入副本包的子集。 */
    uint64_t bytes_recv_reinject;
    /** 本路径向上层移动的、按 stream offset 去重后的 STREAM payload。 */
    uint64_t bytes_recv_effective;
    /** 本路径有效接收字节中由重注入副本成功交付的子集。 */
    uint64_t bytes_recv_effective_reinject;
    /** 本路径的平滑 RTT，单位微秒；尚无 RTT 样本时可能为初始值。 */
    uint64_t srtt_us;

    /** 本路径 bytes_sent 的区间速率，单位 Mbit/s。 */
    double send_mbps;
    /** 本路径 bytes_wire_sent 的区间速率，单位 Mbit/s。 */
    double wire_send_mbps;
    /** 本路径 bytes_delivered 的区间速率，单位 Mbit/s。 */
    double delivery_mbps;
    /** 本路径 bytes_recv 的区间速率，单位 Mbit/s。 */
    double recv_mbps;
    /** 本路径 bytes_recv_effective 的区间速率，单位 Mbit/s。 */
    double effective_recv_mbps;
    /** ABI 预留字段，第三方不得使用。 */
    uint64_t reserved[8];
} netins_udp_path_statis_t;

/** 一次原子复制得到的完整连接快照。 */
typedef struct netins_udp_snapshot_s {
    /** 结构体字节大小，由 NETINS_UDP_SNAPSHOT_INITIALIZER 设置。 */
    uint32_t struct_size;
    /** 调用方使用的 API 版本，由 INITIALIZER 设置。 */
    uint32_t api_version;
    /** 连接身份、生命周期和多路径状态。 */
    netins_udp_info_t info;
    /** 所有路径汇总后的连接级统计。 */
    netins_udp_statis_t statis;
    /** paths[] 中有效元素数量，数值与 info.path_count 相同。 */
    uint32_t path_count;
    /** 对齐和 ABI 预留字段，第三方不得使用。 */
    uint32_t reserved32;
    /**
     * 每路径统计。只允许读取 [0, path_count)；数组可包含 INIT、
     * VALIDATING、ACTIVE 或 CLOSING 路径，并非只输出 ACTIVE 路径。
     */
    netins_udp_path_statis_t paths[NETINS_UDP_MAX_PATHS];
    /** ABI 预留字段，第三方不得使用。 */
    uint64_t reserved[8];
} netins_udp_snapshot_t;

/** 初始化 netins_udp_info_t，调用 netins_udp_observer_get_info() 前必须使用。 */
#define NETINS_UDP_INFO_INITIALIZER \
    { sizeof(netins_udp_info_t), NETINS_UDP_API_VERSION }
/** 初始化 netins_udp_statis_t，调用 netins_udp_observer_get_statis() 前必须使用。 */
#define NETINS_UDP_STATIS_INITIALIZER \
    { sizeof(netins_udp_statis_t), NETINS_UDP_API_VERSION }
/** 初始化单路径输出，调用 netins_udp_observer_get_path_statis() 前必须使用。 */
#define NETINS_UDP_PATH_STATIS_INITIALIZER \
    { sizeof(netins_udp_path_statis_t), NETINS_UDP_API_VERSION }
/** 初始化完整快照，调用 snapshot getter 前必须使用。 */
#define NETINS_UDP_SNAPSHOT_INITIALIZER \
    { sizeof(netins_udp_snapshot_t), NETINS_UDP_API_VERSION }

/**
 * 获取当前动态库实现的 API 版本。
 * @return NETINS_UDP_API_VERSION；当前为 1。
 */
NETINS_UDP_PUBLIC uint32_t netins_udp_get_api_version(void);

/**
 * 使用当前项目推荐值初始化独立客户端配置。
 * @param config 调用方分配的配置结构，不能为空。
 * @note 调用后必须填写 server_address 和 server_port。
 */
NETINS_UDP_PUBLIC void netins_udp_client_config_init(
    netins_udp_client_config_t *config);

/**
 * 创建一个尚未连接的独立 QUIC/H3 客户端对象。
 * @param config 已初始化的客户端配置。
 * @param out_client 输出客户端对象，成功后由 netins_udp_client_close() 释放。
 * @return netins_udp_result_t。
 * @note 本函数只分配资源，不启动网络线程，也不依赖 mini_client。
 */
NETINS_UDP_PUBLIC int netins_udp_client_create(
    const netins_udp_client_config_t *config,
    netins_udp_client_t **out_client);

/**
 * 建立真实 QUIC/H3 连接并等待握手完成。
 * @param client netins_udp_client_create() 返回的客户端对象。
 * @return NETINS_UDP_OK；超时或建连失败返回 NETINS_UDP_ERROR。
 * @note 最长等待时间来自 config.connect_timeout_ms。即使等待超时，后台握手
 *       仍可能继续，调用方可再次使用 netins_udp_client_wait_connected() 等待，
 *       或调用 netins_udp_client_close() 取消连接。
 */
NETINS_UDP_PUBLIC int netins_udp_client_connect(
    netins_udp_client_t *client);

/**
 * 等待已启动客户端完成 QUIC/H3 握手。
 * @param client 有效客户端对象。
 * @param timeout_ms 等待时间；0 表示仅检查当前状态，不阻塞。
 * @return NETINS_UDP_OK；超时、关闭或错误返回 NETINS_UDP_ERROR。
 */
NETINS_UDP_PUBLIC int netins_udp_client_wait_connected(
    netins_udp_client_t *client, uint32_t timeout_ms);

/** 获取独立客户端当前 netins_udp_conn_state_t 状态。 */
NETINS_UDP_PUBLIC int netins_udp_client_get_state(
    netins_udp_client_t *client);

/**
 * 获取独立客户端连接在进程内状态注册表中的 connection_id。
 * 尚未成功创建 XQUIC 连接时返回 NETINS_UDP_INVALID_CONNECTION_ID。
 */
NETINS_UDP_PUBLIC uint64_t netins_udp_client_get_connection_id(
    netins_udp_client_t *client);

/**
 * 获取独立客户端的完整状态快照。
 * @param out 调用前必须使用 NETINS_UDP_SNAPSHOT_INITIALIZER。
 */
NETINS_UDP_PUBLIC int netins_udp_client_get_snapshot(
    netins_udp_client_t *client, netins_udp_snapshot_t *out);

/** 获取独立客户端最后一次连接/运行时错误码。 */
NETINS_UDP_PUBLIC int netins_udp_client_get_last_error_code(
    netins_udp_client_t *client);

/** 将独立客户端最后一次错误说明复制到调用方缓冲区。 */
NETINS_UDP_PUBLIC int netins_udp_client_get_last_error_msg(
    netins_udp_client_t *client, char *buffer, size_t buffer_len);

/** 使用推荐值初始化 HTTP/3 stream 配置。 */
NETINS_UDP_PUBLIC void netins_udp_stream_config_init(
    netins_udp_stream_config_t *config);

/**
 * 在已完成握手的独立客户端上创建一个 HTTP/3 请求。
 *
 * 本函数把创建任务交给客户端引擎线程后立即返回。调用方可以马上用 send()
 * 排队 POST 正文，也可以调用 wait_open() 等待请求头真正发送完成。
 */
NETINS_UDP_PUBLIC int netins_udp_stream_open(
    netins_udp_client_t *client,
    const netins_udp_stream_config_t *config,
    netins_udp_stream_t **out_stream);

/** 等待 HTTP/3 请求头发送完成；timeout_ms=0 表示只检查当前状态。 */
NETINS_UDP_PUBLIC int netins_udp_stream_wait_open(
    netins_udp_stream_t *stream, uint32_t timeout_ms);

/**
 * 复制请求正文到有界发送队列。
 *
 * @return >=0 为本次接受的字节数；负数为 netins_udp_result_t。
 * @note timeout_ms=0 为非阻塞。队列空间不足时允许部分写入；调用方必须继续
 *       发送剩余数据。fin 仅在本次全部数据被接受时生效。POST 达到配置的
 *       request_body_length 时也会自动发送 FIN。
 */
NETINS_UDP_PUBLIC int64_t netins_udp_stream_send(
    netins_udp_stream_t *stream, const void *data, size_t size,
    int fin, uint32_t timeout_ms);

/**
 * 从有界接收队列读取响应正文。
 *
 * @param fin 输出 1 表示响应正文已经结束且队列已读空。
 * @return >0 为读取字节数；0 只用于返回 EOF；负数为 netins_udp_result_t。
 * @note timeout_ms=0 为非阻塞；暂时无数据返回 NETINS_UDP_WOULD_BLOCK。
 */
NETINS_UDP_PUBLIC int64_t netins_udp_stream_recv(
    netins_udp_stream_t *stream, void *buffer, size_t capacity,
    int *fin, uint32_t timeout_ms);

/** 获取 stream 状态、HTTP 响应信息和业务字节计数。 */
NETINS_UDP_PUBLIC int netins_udp_stream_get_info(
    netins_udp_stream_t *stream, netins_udp_stream_info_t *out);

/**
 * 关闭并释放 stream 句柄。
 *
 * timeout_ms=0 时只发起关闭；尚未完成返回 NETINS_UDP_WOULD_BLOCK，句柄仍有效。
 * 返回 NETINS_UDP_OK 后句柄失效。调用 client_close() 前应先关闭其 stream。
 */
NETINS_UDP_PUBLIC int netins_udp_stream_close(
    netins_udp_stream_t *stream, uint32_t timeout_ms);

/**
 * 关闭连接、停止后台线程并释放独立客户端。
 * @note 可安全传入 NULL。调用后客户端指针失效。
 */
NETINS_UDP_PUBLIC void netins_udp_client_close(
    netins_udp_client_t *client);

/**
 * 创建一个尚未绑定 XQUIC 连接的状态观察器。
 * @return 成功返回新句柄，内存不足时返回 NULL。
 * @note 该函数主要供 mini_client/mini_server 宿主集成代码使用。
 *       普通第三方查询现有连接时应使用 netins_udp_observer_open()。
 */
NETINS_UDP_PUBLIC netins_udp_observer_t *netins_udp_observer_new(void);

/**
 * 释放当前调用方持有的一次句柄引用。
 * @param netins netins_udp_observer_new() 或 netins_udp_observer_open() 返回的句柄；可为 NULL。
 * @note 本函数绝不会关闭 XQUIC/QUIC 连接，只管理状态接口句柄的引用计数。
 *       同一个引用只能释放一次，释放后不得继续使用该指针。
 */
NETINS_UDP_PUBLIC void netins_udp_observer_close(netins_udp_observer_t *netins);

/**
 * 根据进程内 connection_id 打开一个仍在注册表中的活动连接。
 * @param connection_id 由 netins_udp_observer_list_connections() 或 info 输出获得。
 * @return 成功返回带独立引用的句柄；连接不存在或已经关闭时返回 NULL。
 * @note 返回的句柄必须由调用方使用 netins_udp_observer_close() 释放。
 *       若在连接关闭前成功 open，关闭后仍可通过该句柄读取最终快照。
 */
NETINS_UDP_PUBLIC netins_udp_observer_t *netins_udp_observer_open(uint64_t connection_id);

/**
 * 枚举当前进程内仍然存活并已注册的 XQUIC 连接。
 *
 * 推荐调用两次：第一次 connection_ids=NULL、capacity=0，用于取得数量；
 * 分配数组后第二次取得 ID。两次调用间连接数量可能变化，调用方应处理
 * NETINS_UDP_BUFFER_TOO_SMALL。
 *
 * @param connection_ids 输出 ID 数组；查询数量时传 NULL。
 * @param capacity connection_ids 可容纳的 uint64_t 元素数量。
 * @param connection_count 输出当前实际需要的元素数量，不能为空。
 * @return NETINS_UDP_OK；容量不足返回 NETINS_UDP_BUFFER_TOO_SMALL；
 *         参数错误返回 NETINS_UDP_ERROR。
 */
NETINS_UDP_PUBLIC int netins_udp_observer_list_connections(uint64_t *connection_ids,
    size_t capacity, size_t *connection_count);

/**
 * 从指定句柄复制一份完整且线程安全的缓存快照。
 * @param netins netins_udp_observer_open()/new() 获得的有效句柄。
 * @param out 输出完整快照；调用前必须使用 NETINS_UDP_SNAPSHOT_INITIALIZER。
 * @return netins_udp_result_t。
 * @note 本函数不直接调用 XQUIC，因此第三方线程可安全调用。
 */
NETINS_UDP_PUBLIC int netins_udp_observer_get_snapshot(netins_udp_observer_t *netins,
    netins_udp_snapshot_t *out);

/**
 * 根据 connection_id 一次性取得完整快照，无需调用方长期持有句柄。
 * @param connection_id 当前进程内的活动连接编号。
 * @param out 输出完整快照；调用前必须使用 NETINS_UDP_SNAPSHOT_INITIALIZER。
 * @return NETINS_UDP_OK；连接已关闭或不存在返回 NETINS_UDP_NOT_FOUND；
 *         也可能返回版本或结构体大小错误。
 */
NETINS_UDP_PUBLIC int netins_udp_observer_get_snapshot_by_id(uint64_t connection_id,
    netins_udp_snapshot_t *out);

/**
 * 只获取连接身份、生命周期和多路径状态。
 * @param netins 有效句柄。
 * @param out 输出信息；调用前必须使用 NETINS_UDP_INFO_INITIALIZER。
 * @return netins_udp_result_t。
 */
NETINS_UDP_PUBLIC int netins_udp_observer_get_info(netins_udp_observer_t *netins,
    netins_udp_info_t *out);

/**
 * 只获取所有路径汇总后的连接级统计。
 * @param netins 有效句柄。
 * @param out 输出统计；调用前必须使用 NETINS_UDP_STATIS_INITIALIZER。
 * @return netins_udp_result_t。
 */
NETINS_UDP_PUBLIC int netins_udp_observer_get_statis(netins_udp_observer_t *netins,
    netins_udp_statis_t *out);

/**
 * 根据快照数组下标获取一条路径的统计。
 * @param netins 有效句柄。
 * @param path_index paths[] 数组下标，范围为 [0, info.path_count)；
 *        该参数不是 XQUIC path_id。
 * @param out 输出路径统计；调用前必须使用
 *        NETINS_UDP_PATH_STATIS_INITIALIZER。
 * @return NETINS_UDP_OK；下标越界返回 NETINS_UDP_NOT_FOUND；
 *         也可能返回版本或结构体大小错误。
 */
NETINS_UDP_PUBLIC int netins_udp_observer_get_path_statis(netins_udp_observer_t *netins,
    size_t path_index, netins_udp_path_statis_t *out);

/**
 * 设置或取消该状态句柄的日志回调。
 * @param netins 有效句柄。
 * @param logger 日志回调；传 NULL 表示取消回调。
 * @param user_data 原样传回 logger 的第三方上下文，生命周期由第三方管理。
 * @return NETINS_UDP_OK 或 NETINS_UDP_ERROR。
 * @note 回调可能在 XQUIC/Libevent 线程运行，回调内不要阻塞或重新进入传输。
 */
NETINS_UDP_PUBLIC int netins_udp_observer_set_logger(netins_udp_observer_t *netins,
    netins_udp_logger_cb logger, void *user_data);

/**
 * 获取该句柄最后一次 API 错误码。
 * @param netins 有效句柄。
 * @return 0 表示无 API 错误；其余通常为 errno 风格错误码。
 * @note 该值与 info.conn_error 表示的 XQUIC 连接错误不是同一个概念。
 */
NETINS_UDP_PUBLIC int netins_udp_observer_get_last_error_code(netins_udp_observer_t *netins);

/**
 * 将该句柄最后一次 API 错误说明复制到调用方缓冲区。
 * @param netins 有效句柄。
 * @param buffer 输出字符串缓冲区。
 * @param buffer_len 缓冲区字节长度，必须大于 0。
 * @return NETINS_UDP_OK 或 NETINS_UDP_ERROR。
 */
NETINS_UDP_PUBLIC int netins_udp_observer_get_last_error_msg(netins_udp_observer_t *netins,
    char *buffer, size_t buffer_len);

/** 将 netins_udp_conn_state_t 转换为只读英文名称；返回值无需释放。 */
NETINS_UDP_PUBLIC const char *netins_udp_conn_state_name(int state);
/** 将 netins_udp_path_state_t 转换为只读英文名称；返回值无需释放。 */
NETINS_UDP_PUBLIC const char *netins_udp_path_state_name(int state);
/** 将 netins_udp_app_path_status_t 转换为只读英文名称；返回值无需释放。 */
NETINS_UDP_PUBLIC const char *netins_udp_app_path_status_name(int status);

#ifdef __cplusplus
}
#endif

#endif /* NETINS_UDP_API_H */
