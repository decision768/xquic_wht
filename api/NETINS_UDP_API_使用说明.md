# NetIns UDP API 使用说明

## 概述

NetIns UDP 是基于 QUIC 和 Multipath QUIC 的网络连接库，提供客户端连接、
服务端监听、HTTP/3 数据收发、多路径状态查询、连接统计、事件通知和日志回调。

应用程序只需包含公共头文件：

```c
#include <xquic/netins_udp.h>
```

所有公共函数均使用 `netins_udp_xxx()` 命名。数据收发以 HTTP/3 request 为
单位，支持 GET、POST、自定义请求头、请求体和响应体。

## 目录

1. [交付文件与链接方式](#交付文件与链接方式)
2. [返回值与错误处理](#返回值与错误处理)
3. [API 函数说明](#api-函数说明)
4. [客户端示例](#客户端示例)
5. [服务端示例](#服务端示例)
6. [使用注意事项](#使用注意事项)

## 交付文件与链接方式

安装后的主要文件：

- 头文件：`include/xquic/netins_udp.h`
- 动态库：`lib/libxquic.so`
- 静态库：`lib/libxquic.a`
- 文档与示例：`${CMAKE_INSTALL_DOCDIR}/netins_udp`

编译应用程序：

```sh
cc app.c -I/opt/netins-udp/include -L/opt/netins-udp/lib \
  -lxquic -lssl -lcrypto -lpthread -ldl -lm
```

建议在程序启动时校验头文件与运行库版本：

```c
if (netins_udp_get_api_version() != NETINS_UDP_API_VERSION) {
    /* 头文件与运行库不匹配。 */
    return -1;
}
```

所有带 `struct_size` 和 `api_version` 的公共结构体必须使用对应的
`NETINS_UDP_*_INITIALIZER` 或初始化函数进行初始化。

## 返回值与错误处理

- 普通函数成功返回 `NETINS_UDP_OK`，失败返回 `NETINS_UDP_ERROR`。
- 返回对象指针的函数失败时返回 `NULL`。
- 数据收发函数成功时返回实际处理的字节数，失败返回 `-1`。
- `recv` 返回 `0` 表示当前 HTTP body 已结束。
- 失败时同步设置 `errno`。
- 连接错误通过 `netins_udp_get_last_error_code()` 和
  `netins_udp_get_last_error_msg()` 查询。
- request 错误通过 `netins_udp_h3_request_get_last_error_code()` 和
  `netins_udp_h3_request_get_last_error_msg()` 查询。
- 连接关闭原因通过 `netins_udp_get_close_reason()` 查询。

错误码采用 errno 风格，常见值包括：

| 错误码 | 说明 |
|---|---|
| `EINVAL` | 参数、结构体版本或调用对象不正确 |
| `EALREADY` | 连接、监听或关闭操作已经开始 |
| `EBUSY` | 当前状态不允许修改该选项 |
| `EINPROGRESS` | 非阻塞连接正在建立 |
| `EAGAIN` | 非阻塞操作暂时无法继续 |
| `ETIMEDOUT` | 操作超时 |
| `ENOTCONN` | 连接尚未建立或已经断开 |
| `ENOBUFS` | 输出缓冲区或有界发送/接收队列不足 |
| `EOPNOTSUPP` | 当前构建或协议不支持该功能 |

错误信息属于对象的最近一次调用结果。多个线程共享同一对象时，应在失败后立即
复制错误码和错误文本。

## API 函数说明

### 1. 库初始化

#### `netins_udp_init()`

初始化 NetIns UDP 运行环境。

```c
int netins_udp_init(void);
```

该函数线程安全并采用引用计数。每次成功调用都必须对应一次
`netins_udp_cleanup()`。

返回值：

- 成功：`NETINS_UDP_OK`
- 失败：`NETINS_UDP_ERROR`

#### `netins_udp_cleanup()`

释放一次初始化引用。

```c
int netins_udp_cleanup(void);
```

最后一次 cleanup 前必须关闭全部 connection、listener、accepted connection
和 HTTP/3 request。仍有对象或异步连接任务存在时函数返回
`NETINS_UDP_ERROR`，并设置 `errno=EBUSY`。

#### `netins_udp_get_api_version()`

返回运行库实现的公共 API 版本。

```c
uint32_t netins_udp_get_api_version(void);
```

### 2. 连接对象管理

#### `netins_udp_new()`

创建未连接对象。

```c
netins_udp_t *netins_udp_new(int type);
```

参数：

- `type`：
  - `NETINS_UDP_TYPE_QUIC`：普通 QUIC
  - `NETINS_UDP_TYPE_MPQUIC`：请求 Multipath QUIC

返回值：

- 成功：连接对象
- 失败：`NULL`

`NETINS_UDP_TYPE_MPQUIC` 表示请求启用多路径。连接建立后应通过
`netins_udp_is_multipath()` 或 `netins_udp_get_snapshot()` 确认实际状态。

#### `netins_udp_shutdown()`

发起连接关闭，但不释放对象。

```c
int netins_udp_shutdown(netins_udp_t *netins,
    uint64_t application_error);
```

调用后仍可查询最终状态、统计和关闭原因。listener 不支持该函数。

#### `netins_udp_close()`

关闭并释放对象。

```c
void netins_udp_close(netins_udp_t *netins);
```

`netins_udp_close(NULL)` 可以安全返回。非空指针在函数返回后立即失效，不得再次
使用或重复关闭。

每个 HTTP/3 request 必须单独调用 `netins_udp_h3_request_close()`。关闭父连接
不会代替应用程序释放 request 对象。

### 3. 连接选项

#### `netins_udp_set_opt()`

设置连接选项。

```c
int netins_udp_set_opt(netins_udp_t *netins, int option,
    const void *value, size_t value_len);
```

#### `netins_udp_get_opt()`

读取连接选项。

```c
int netins_udp_get_opt(netins_udp_t *netins, int option,
    void *value, size_t *value_len);
```

`value_len` 是输入/输出参数。`value` 为 `NULL` 时只返回所需长度；缓冲区过小
时函数返回 `NETINS_UDP_ERROR`，设置 `errno=ENOBUFS`，并写回所需长度。

选项：

| 选项 | 值类型 | 默认值 | 说明 |
|---|---:|---:|---|
| `NETINS_UDP_OPT_NONBLOCK` | `int` | 0 | 0 阻塞，1 非阻塞 |
| `NETINS_UDP_OPT_CONNECT_TIMEOUT_MS` | `uint32_t` | 20000 | 完整连接建立超时 |
| `NETINS_UDP_OPT_ACCEPT_TIMEOUT_MS` | `uint32_t` | 0 | accept 超时，0 表示无限等待 |
| `NETINS_UDP_OPT_SEND_TIMEOUT_MS` | `uint32_t` | 0 | request send 超时 |
| `NETINS_UDP_OPT_RECV_TIMEOUT_MS` | `uint32_t` | 0 | request recv/accept 超时 |
| `NETINS_UDP_OPT_CLOSE_TIMEOUT_MS` | `uint32_t` | 1000 | 关闭等待时间 |
| `NETINS_UDP_OPT_IDLE_TIMEOUT_MS` | `uint32_t` | 0 | QUIC 连接空闲超时 |
| `NETINS_UDP_OPT_KERNEL_SNDBUF` | `int` | 16 MiB | UDP socket 发送缓冲区请求值 |
| `NETINS_UDP_OPT_KERNEL_RCVBUF` | `int` | 16 MiB | UDP socket 接收缓冲区请求值 |
| `NETINS_UDP_OPT_INTERFACES` | `netins_udp_interfaces_t` | 空 | 客户端使用的网络接口 |
| `NETINS_UDP_OPT_APP_PROTOCOL` | `int` | HTTP/3 | 支持 `NETINS_UDP_APP_HTTP3` |
| `NETINS_UDP_OPT_TLS` | `netins_udp_tls_config_t` | 校验证书 | TLS 配置 |
| `NETINS_UDP_OPT_INITIAL_CWND_PACKETS` | `uint32_t` | 100 | 初始拥塞窗口包数 |
| `NETINS_UDP_OPT_SCHEDULER` | `int` | default | 多路径调度器 |
| `NETINS_UDP_OPT_CONGESTION_CONTROL` | `int` | default | 拥塞控制算法 |
| `NETINS_UDP_OPT_LOG_LEVEL` | `int` | INFO | 日志回调阈值 |

以下选项可以在连接或监听开始后修改：

- `NETINS_UDP_OPT_NONBLOCK`
- `NETINS_UDP_OPT_SEND_TIMEOUT_MS`
- `NETINS_UDP_OPT_RECV_TIMEOUT_MS`
- `NETINS_UDP_OPT_CLOSE_TIMEOUT_MS`
- `NETINS_UDP_OPT_LOG_LEVEL`
- listener 的 `NETINS_UDP_OPT_ACCEPT_TIMEOUT_MS`

其他选项必须在 `netins_udp_connect()` 或 `netins_udp_listen()` 前设置。

request 在创建时复制父连接的阻塞方式和超时配置。accepted connection 在
accept 时复制 listener 的运行时配置。这些值采用创建时快照语义，后续修改父
对象不会改变已经创建的子对象。

#### `netins_udp_tls_config_init()`

初始化 TLS 配置。

```c
void netins_udp_tls_config_init(netins_udp_tls_config_t *config);
```

也可以使用：

```c
netins_udp_tls_config_t tls = NETINS_UDP_TLS_CONFIG_INITIALIZER;
```

客户端常用字段：

- `verify_peer`：是否校验服务端证书，默认 1。
- `server_name`：证书校验名称和 TLS SNI。

listener 必须设置：

- `certificate_file`
- `private_key_file`

### 4. 日志回调

#### `netins_udp_set_logger()`

设置对象的日志回调。

```c
typedef void (*netins_udp_logger_cb)(int level,
    const char *message);

int netins_udp_set_logger(netins_udp_t *netins,
    netins_udp_logger_cb logger);
```

日志级别：

- `NETINS_UDP_LOG_LEVEL_DEBUG`
- `NETINS_UDP_LOG_LEVEL_INFO`
- `NETINS_UDP_LOG_LEVEL_WARN`
- `NETINS_UDP_LOG_LEVEL_ERROR`

默认未注册回调，因此库不会通过该接口输出日志。传入 `NULL` 可关闭回调。
默认阈值为 INFO，可通过 `NETINS_UDP_OPT_LOG_LEVEL` 修改。

日志仅用于低频生命周期和异常信息。RTT、CWND、inflight、路径流量和吞吐应
通过统计接口读取。

回调可能在 API 调用线程或库工作线程执行。`message` 只在本次回调期间有效。
回调必须快速返回，不得阻塞，也不得使用同一句柄重入 NetIns UDP。

accepted connection 在 accept 时继承 listener 的 logger 和日志级别，之后
可以独立修改。

示例：

```c
static void
logger_callback(int level, const char *message)
{
    static const char *names[] = {
        "DEBUG", "INFO", "WARN", "ERROR"
    };
    if (level >= 0 && level <= 3) {
        fprintf(stderr, "[%s] %s\n", names[level], message);
    }
}

netins_udp_set_logger(connection, logger_callback);
```

### 5. 客户端连接

#### `netins_udp_connect()`

建立客户端连接。

```c
int netins_udp_connect(netins_udp_t *netins,
    const char *host, int port);
```

参数：

- `netins`：由 `netins_udp_new()` 创建的对象
- `host`：服务端 IP 地址或域名
- `port`：服务端端口，范围 1～65535

阻塞模式下，函数在 DNS、QUIC 握手和 HTTP/3 建立完成后返回。

非阻塞模式下，尚未完成时返回 `NETINS_UDP_ERROR` 并设置
`errno=EINPROGRESS`。应用程序应使用 `netins_udp_get_fd()`、
`netins_udp_get_events()` 和 `netins_udp_get_state()` 等待最终结果。

同一对象只能调用一次 connect，失败后仍应调用 `netins_udp_close()`。

### 6. 服务端连接

#### `netins_udp_listen()`

启动 HTTP/3 listener。

```c
netins_udp_t *netins_udp_listen(netins_udp_t *netins,
    const char *host, int port);
```

`host` 为 `NULL` 或空字符串时监听可用地址。调用前必须通过
`NETINS_UDP_OPT_TLS` 配置证书和私钥。

返回值：

- 成功：返回输入的 listener 对象
- 失败：返回 `NULL`

失败后输入对象仍然有效，可查询错误并关闭。

#### `netins_udp_accept()`

接收客户端连接。

```c
netins_udp_t *netins_udp_accept(netins_udp_t *listener,
    struct sockaddr *addr, socklen_t *addrlen);
```

返回的 accepted connection 是独立对象，必须单独关闭。关闭 listener 不会使
已经返回的 accepted connection 指针失效。

`addr` 和 `addrlen` 可用于获取对端地址；不需要时可同时传入 `NULL`。

阻塞方式由 listener 的 `NONBLOCK` 和 `ACCEPT_TIMEOUT_MS` 控制。非阻塞且
暂无连接时返回 `NULL`，并设置 `errno=EAGAIN`。

### 7. HTTP/3 请求配置

#### `netins_udp_h3_request_config_init()`

初始化客户端请求配置。

```c
void netins_udp_h3_request_config_init(
    netins_udp_h3_request_config_t *config);
```

默认值为 GET、路径 `/`、`application/octet-stream` 和空请求体。

主要字段：

- `method`：HTTP 方法
- `path`：请求路径
- `authority`：HTTP `:authority`，为空时使用 TLS server name
- `content_type`：Content-Type
- `content_length`：请求体长度

长度未知时将 `content_length` 设置为
`NETINS_UDP_CONTENT_LENGTH_UNKNOWN`。

#### `netins_udp_h3_request_config_add_header()`

添加自定义请求头。

```c
int netins_udp_h3_request_config_add_header(
    netins_udp_h3_request_config_t *config,
    const char *name, const char *value);
```

header 名必须为小写，不能以 `:` 开头，不能覆盖由库管理的 pseudo-header、
`content-length`、`content-type` 和 `host`。

#### `netins_udp_h3_response_config_init()`

初始化服务端响应配置。

```c
void netins_udp_h3_response_config_init(
    netins_udp_h3_response_config_t *config);
```

默认状态码为 200。

#### `netins_udp_h3_response_config_add_header()`

添加自定义响应头。

```c
int netins_udp_h3_response_config_add_header(
    netins_udp_h3_response_config_t *config,
    const char *name, const char *value);
```

### 8. HTTP/3 request 管理

#### `netins_udp_h3_request_open()`

在客户端连接上创建并发送 HTTP/3 请求头。

```c
int netins_udp_h3_request_open(netins_udp_t *netins,
    const netins_udp_h3_request_config_t *config,
    netins_udp_h3_request_t **out_request);
```

连接必须处于 `NETINS_UDP_STATE_CONNECTED`。

#### `netins_udp_h3_request_accept()`

在 accepted connection 上接收 HTTP/3 请求。

```c
int netins_udp_h3_request_accept(netins_udp_t *netins,
    netins_udp_h3_request_t **out_request);
```

非阻塞且暂无请求时函数返回 `NETINS_UDP_ERROR`，并设置 `errno=EAGAIN`。

#### `netins_udp_h3_request_set_response()`

设置服务端响应头。

```c
int netins_udp_h3_request_set_response(
    netins_udp_h3_request_t *request,
    const netins_udp_h3_response_config_t *config);
```

每个 request 只能成功设置一次最终响应。状态码范围为 200～599，发送响应体前
必须先成功调用该函数。

HEAD、204、205 和 304 响应不发送 body。违反响应 body 规则时返回
`EOPNOTSUPP`。

#### `netins_udp_h3_request_close()`

关闭并释放 request。

```c
void netins_udp_h3_request_close(
    netins_udp_h3_request_t *request);
```

每个成功创建或接收的 request 都必须关闭。

### 9. HTTP/3 数据收发

#### `netins_udp_h3_request_send()`

发送请求体或响应体。

```c
ssize_t netins_udp_h3_request_send(
    netins_udp_h3_request_t *request,
    const void *data, size_t length, int flags);
```

#### `netins_udp_h3_request_recv()`

接收请求体或响应体。

```c
ssize_t netins_udp_h3_request_recv(
    netins_udp_h3_request_t *request,
    void *data, size_t length, int flags);
```

flags：

- `NETINS_UDP_MSG_NONE`
- `NETINS_UDP_MSG_DONTWAIT`：仅本次调用非阻塞
- `NETINS_UDP_MSG_PEEK`：仅用于 recv
- `NETINS_UDP_MSG_FIN`：仅用于 send，表示该方向 body 结束

send 可能部分成功。返回正数但小于 `length` 时，应用程序必须继续发送剩余
数据。recv 返回 0 表示 body 已结束。

有限超时到期时返回 `NETINS_UDP_ERROR` 并设置 `errno=ETIMEDOUT`；非阻塞且
暂无进展时返回 `NETINS_UDP_ERROR` 并设置 `errno=EAGAIN`。

固定长度 body 的实际发送总字节数必须等于 `content_length`。长度未知时，
最后一次发送必须使用 `NETINS_UDP_MSG_FIN`。

### 10. Request 信息和错误

#### `netins_udp_h3_request_get_info()`

读取 request 状态和统计。

```c
int netins_udp_h3_request_get_info(
    netins_udp_h3_request_t *request,
    netins_udp_h3_request_info_t *out);
```

输出结构必须使用 `NETINS_UDP_H3_REQUEST_INFO_INITIALIZER` 初始化。

主要字段：

- `stream_id`
- `response_status`
- `request_headers_ready`
- `response_headers_ready`
- `send_finished`
- `receive_finished`
- `bytes_sent`
- `bytes_received`
- `peer_headers`

`bytes_sent` 和 `bytes_received` 只统计 HTTP body，不包含 HTTP/3 framing、
QPACK 和 headers。

#### Request 错误查询

```c
int netins_udp_h3_request_get_last_error_code(
    netins_udp_h3_request_t *request);

int netins_udp_h3_request_get_last_error_msg(
    netins_udp_h3_request_t *request,
    char *buffer, size_t buffer_len);
```

### 11. 连接状态和统计

#### `netins_udp_get_state()`

返回连接状态。

```c
int netins_udp_get_state(netins_udp_t *netins);
```

状态值：

- `NETINS_UDP_STATE_NEW`
- `NETINS_UDP_STATE_CONNECTING`
- `NETINS_UDP_STATE_LISTENING`
- `NETINS_UDP_STATE_CONNECTED`
- `NETINS_UDP_STATE_CLOSING`
- `NETINS_UDP_STATE_CLOSED`
- `NETINS_UDP_STATE_ERROR`

#### `netins_udp_is_multipath()`

查询连接是否实际启用多路径。

```c
int netins_udp_is_multipath(netins_udp_t *netins);
```

返回 1 表示已启用，0 表示未启用，失败返回 `NETINS_UDP_ERROR`。

#### `netins_udp_get_path_count()`

读取已观察到的路径数量。

```c
int netins_udp_get_path_count(netins_udp_t *netins,
    uint32_t *out_count);
```

#### `netins_udp_get_info()`

读取连接基本信息。

```c
int netins_udp_get_info(netins_udp_t *netins,
    netins_udp_info_t *out);
```

输出结构必须使用 `NETINS_UDP_INFO_INITIALIZER` 初始化。

#### `netins_udp_get_statis()`

读取连接统计。

```c
int netins_udp_get_statis(netins_udp_t *netins,
    netins_udp_statis_t *out);
```

输出结构必须使用 `NETINS_UDP_STATIS_INITIALIZER` 初始化。

#### `netins_udp_get_path_statis()`

读取指定路径统计。

```c
int netins_udp_get_path_statis(netins_udp_t *netins,
    size_t path_index, netins_udp_path_statis_t *out);
```

`path_index` 是连接生命周期内稳定的路径槽位。输出结构必须使用
`NETINS_UDP_PATH_STATIS_INITIALIZER` 初始化。

#### `netins_udp_get_snapshot()`

一次性读取连接、总统计、逐路径统计和关闭原因。

```c
int netins_udp_get_snapshot(netins_udp_t *netins,
    netins_udp_snapshot_t *out);
```

输出结构必须使用 `NETINS_UDP_SNAPSHOT_INITIALIZER` 初始化。持续监控时推荐使用
该函数，避免在不同时间分别读取多组统计。

主要统计：

- `bytes_sent` / `bytes_received`：去重后的 QUIC STREAM payload
- `bytes_wire_sent` / `bytes_wire_received`：UDP 线速字节
- `bytes_acked`
- `bytes_lost`：重传字节代理值
- `bytes_inflight`
- `send_mbps` / `receive_mbps` / `acked_mbps`
- `loss_rate`
- 每条路径的 RTT、CWND、inflight、流量和状态

逐路径 payload 统计不重复计算 retransmission 和 reinjection 数据，因此不等于
网卡线速字节。需要线速统计时使用 `bytes_wire_sent` 和
`bytes_wire_received`。

### 12. 连接错误和关闭原因

#### 连接错误查询

```c
int netins_udp_get_last_error_code(netins_udp_t *netins);

int netins_udp_get_last_error_msg(netins_udp_t *netins,
    char *buffer, size_t buffer_len);
```

#### `netins_udp_get_close_reason()`

读取连接关闭原因。

```c
int netins_udp_get_close_reason(netins_udp_t *netins,
    netins_udp_close_reason_t *out);
```

输出结构必须使用 `NETINS_UDP_CLOSE_REASON_INITIALIZER` 初始化。

关闭来源：

- `NETINS_UDP_CLOSE_LOCAL`
- `NETINS_UDP_CLOSE_PEER`
- `NETINS_UDP_CLOSE_TRANSPORT`
- `NETINS_UDP_CLOSE_IDLE_TIMEOUT`
- `NETINS_UDP_CLOSE_HANDSHAKE_TIMEOUT`
- `NETINS_UDP_CLOSE_NETWORK_ERROR`
- `NETINS_UDP_CLOSE_INTERNAL_ERROR`

### 13. poll/epoll 通知

#### Connection/listener 通知

```c
int netins_udp_get_fd(netins_udp_t *netins);

int netins_udp_get_events(netins_udp_t *netins,
    uint32_t *out_events);
```

#### Request 通知

```c
int netins_udp_h3_request_get_fd(
    netins_udp_h3_request_t *request);

int netins_udp_h3_request_get_events(
    netins_udp_h3_request_t *request,
    uint32_t *out_events);
```

通知 fd 可加入 `poll` 或 `epoll`，只监听可读事件。该 fd 由库拥有，应用程序
不得直接 read 或 close；收到可读通知后必须调用对应的 `get_events()`。

事件：

| 事件 | 适用对象 | 说明 |
|---|---|---|
| `NETINS_UDP_EVENT_CONNECTED` | client/connection | 连接建立 |
| `NETINS_UDP_EVENT_ACCEPTABLE` | listener | 有连接可以 accept |
| `NETINS_UDP_EVENT_H3_REQUEST_ACCEPTABLE` | accepted connection | 有 request 可以接收 |
| `NETINS_UDP_EVENT_READABLE` | request | body 可读 |
| `NETINS_UDP_EVENT_WRITABLE` | request | body 可写 |
| `NETINS_UDP_EVENT_PATH_CHANGED` | connection | 路径状态变化 |
| `NETINS_UDP_EVENT_CLOSED` | connection | 连接关闭 |
| `NETINS_UDP_EVENT_ERROR` | connection/request | 发生错误 |

对于 `ACCEPTABLE`、`H3_REQUEST_ACCEPTABLE`、`READABLE` 和 `WRITABLE`，应用程序
应持续处理，直到相应操作返回 `NETINS_UDP_ERROR` 并设置 `errno=EAGAIN`。

## 客户端示例

以下示例建立 Multipath QUIC 连接并发送 GET 请求：

```c
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <xquic/netins_udp.h>

int
download_file(const char *host, int port, const char *server_name)
{
    int result = -1;
    netins_udp_t *client = NULL;
    netins_udp_h3_request_t *request = NULL;

    if (netins_udp_init() != NETINS_UDP_OK) {
        return -1;
    }

    client = netins_udp_new(NETINS_UDP_TYPE_MPQUIC);
    if (client == NULL) {
        goto done;
    }

    netins_udp_interfaces_t interfaces =
        NETINS_UDP_INTERFACES_INITIALIZER;
    interfaces.interface_count = 2;
    snprintf(interfaces.names[0], sizeof(interfaces.names[0]),
        "rmnet_data0");
    snprintf(interfaces.names[1], sizeof(interfaces.names[1]),
        "usb0");
    if (netins_udp_set_opt(client, NETINS_UDP_OPT_INTERFACES,
            &interfaces, sizeof(interfaces)) != NETINS_UDP_OK) {
        goto done;
    }

    netins_udp_tls_config_t tls = NETINS_UDP_TLS_CONFIG_INITIALIZER;
    snprintf(tls.server_name, sizeof(tls.server_name), "%s",
        server_name);
    if (netins_udp_set_opt(client, NETINS_UDP_OPT_TLS,
            &tls, sizeof(tls)) != NETINS_UDP_OK) {
        goto done;
    }

    if (netins_udp_connect(client, host, port) != NETINS_UDP_OK) {
        goto done;
    }

    netins_udp_h3_request_config_t config;
    netins_udp_h3_request_config_init(&config);
    snprintf(config.method, sizeof(config.method), "GET");
    snprintf(config.path, sizeof(config.path), "/file.bin");

    if (netins_udp_h3_request_open(client, &config, &request)
        != NETINS_UDP_OK) {
        goto done;
    }

    unsigned char buffer[64 * 1024];
    for (;;) {
        ssize_t received = netins_udp_h3_request_recv(request,
            buffer, sizeof(buffer), NETINS_UDP_MSG_NONE);
        if (received > 0) {
            /* 处理或保存 buffer[0..received)。 */
            continue;
        }
        if (received == 0) {
            result = 0;
        }
        break;
    }

done:
    if (result != 0 && client != NULL) {
        char message[NETINS_UDP_ERROR_MSG_LEN] = {0};
        if (request != NULL) {
            netins_udp_h3_request_get_last_error_msg(request,
                message, sizeof(message));
        } else {
            netins_udp_get_last_error_msg(client, message,
                sizeof(message));
        }
        fprintf(stderr, "NetIns UDP error: %s\n", message);
    }
    netins_udp_h3_request_close(request);
    netins_udp_close(client);
    netins_udp_cleanup();
    return result;
}
```

## 服务端示例

以下示例监听连接并返回一个简单响应：

```c
#include <stdio.h>
#include <string.h>
#include <xquic/netins_udp.h>

int
serve_once(int port, const char *certificate, const char *private_key)
{
    int result = -1;
    netins_udp_t *listener = NULL;
    netins_udp_t *connection = NULL;
    netins_udp_h3_request_t *request = NULL;

    if (netins_udp_init() != NETINS_UDP_OK) {
        return -1;
    }

    listener = netins_udp_new(NETINS_UDP_TYPE_MPQUIC);
    if (listener == NULL) {
        goto done;
    }

    netins_udp_tls_config_t tls = NETINS_UDP_TLS_CONFIG_INITIALIZER;
    snprintf(tls.certificate_file, sizeof(tls.certificate_file),
        "%s", certificate);
    snprintf(tls.private_key_file, sizeof(tls.private_key_file),
        "%s", private_key);
    if (netins_udp_set_opt(listener, NETINS_UDP_OPT_TLS,
            &tls, sizeof(tls)) != NETINS_UDP_OK) {
        goto done;
    }

    if (netins_udp_listen(listener, NULL, port) == NULL) {
        goto done;
    }

    connection = netins_udp_accept(listener, NULL, NULL);
    if (connection == NULL) {
        goto done;
    }

    if (netins_udp_h3_request_accept(connection, &request)
        != NETINS_UDP_OK) {
        goto done;
    }

    unsigned char buffer[4096];
    for (;;) {
        ssize_t received = netins_udp_h3_request_recv(request,
            buffer, sizeof(buffer), NETINS_UDP_MSG_NONE);
        if (received > 0) {
            /* 处理请求体。 */
            continue;
        }
        if (received < 0) {
            goto done;
        }
        break;
    }

    static const char body[] = "OK";
    netins_udp_h3_response_config_t response;
    netins_udp_h3_response_config_init(&response);
    response.status = 200;
    response.content_length = sizeof(body) - 1;
    snprintf(response.content_type, sizeof(response.content_type),
        "text/plain");

    if (netins_udp_h3_request_set_response(request, &response)
        != NETINS_UDP_OK) {
        goto done;
    }
    size_t sent_total = 0;
    while (sent_total < sizeof(body) - 1) {
        ssize_t sent = netins_udp_h3_request_send(request,
            body + sent_total, sizeof(body) - 1 - sent_total,
            NETINS_UDP_MSG_FIN);
        if (sent <= 0) {
            goto done;
        }
        sent_total += (size_t)sent;
    }

    result = 0;

done:
    netins_udp_h3_request_close(request);
    if (connection != NULL) {
        netins_udp_shutdown(connection, 0);
    }
    netins_udp_close(connection);
    netins_udp_close(listener);
    netins_udp_cleanup();
    return result;
}
```

## 使用注意事项

### 1. 资源管理

- 每次成功的 `netins_udp_init()` 都必须对应一次 cleanup。
- 每个 connection、listener 和 accepted connection 都必须单独 close。
- 每个 HTTP/3 request 都必须单独 close。
- close 返回后对象指针失效。

推荐关闭顺序：

1. 停止发起新的业务操作。
2. 关闭全部 request。
3. 对连接调用 `netins_udp_shutdown()`。
4. 关闭 accepted connection 或 client。
5. 关闭 listener。
6. 调用 `netins_udp_cleanup()`。

### 2. 线程安全

公共连接状态、选项、统计和生命周期函数带有同步保护。不同对象可以在不同线程
使用。

同一对象开始 close 后，应用程序不得再从其他线程提交新调用。回调函数不得
使用同一句柄重入 API。

### 3. 阻塞与非阻塞

connect、accept、request accept、send 和 recv 默认阻塞。可通过
`NETINS_UDP_OPT_NONBLOCK` 或单次调用的 `NETINS_UDP_MSG_DONTWAIT` 使用非阻塞
方式。

使用 poll/epoll 时，收到通知后必须继续处理到 `EAGAIN`，否则可能持续收到可读
通知。

### 4. 数据发送

send 可能部分成功，必须检查返回值并发送剩余数据。固定长度 body 的发送总量
必须与声明的 `content_length` 一致。

### 5. 多路径

请求 MPQUIC 不代表始终存在两条活动路径。连接建立后应读取：

- `multipath_active`
- `path_count`
- `active_path_count`
- `validating_path_count`
- 每条路径的 `path_state` 和 `app_status`

路径状态可能在连接生命周期中变化。需要持续监控时，应结合
`NETINS_UDP_EVENT_PATH_CHANGED` 和 `netins_udp_get_snapshot()`。

### 6. 网络和证书

接口绑定依赖 Linux 路由、接口状态和 `SO_BINDTODEVICE` 权限。证书文件、
私钥文件和信任链必须在运行环境中可访问。

不支持的 TLS、调度器或拥塞控制配置会返回 `NETINS_UDP_ERROR`，并设置
`errno=EOPNOTSUPP`。

### 7. 功能限制

- 应用数据面为 HTTP/3 request/response。
- 客户端请求方法支持 GET 和 POST。
- 不提供 connection-level TCP 风格字节流接口。
- 客户端支持系统默认信任链和服务端证书校验。
- 服务端必须配置证书和私钥。
- 支持 Linux x86_64 和 AArch64；应使用与目标平台一致的库和依赖项。
