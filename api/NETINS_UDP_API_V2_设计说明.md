# NetIns UDP API V2 设计说明

## 1. 文档状态

本文档是 `netins_udp` V2 的接口契约，阶段二以此文档和
`api/draft/netins_udp_v2.h` 为准。

当前正式构建仍使用 V1 头文件 `include/xquic/netins_udp.h`。V2 草案暂不参与
编译、安装或符号导出，避免在观察器重命名和 V2 实现完成之前破坏当前分支。

V2 是一次明确的 ABI 断代，不保证与尚未发布的 V1 API 二进制兼容。阶段三完成
后，草案头文件才会替换正式公共头文件，并同步更新动态库导出符号和 SONAME。

## 2. 设计目标

V2 的调用方式尽量接近 TCP/MPTCP 版本 NetIns：

```c
netins_udp_t *conn = netins_udp_new(NETINS_UDP_TYPE_MPQUIC);
if (conn == NULL) {
    /* errno */
}

if (netins_udp_connect(conn, host, port) != NETINS_UDP_OK) {
    /* netins_udp_get_last_error_* */
}

ssize_t sent = netins_udp_send(conn, data, length, 0);
ssize_t received = netins_udp_recv(conn, buffer, capacity, 0);
netins_udp_close(conn);
```

同时必须正确表达 QUIC 与 TCP 不同的对象关系：

- 一个监听 UDP socket/engine 可以承载多个 QUIC 连接。
- 一个 QUIC 连接可以承载多个双向或单向 stream。
- HTTP/3 request 是 stream 上的应用协议对象，不等同于原始字节流。
- 多路径请求可能协商失败或只剩一条有效路径，不能只根据创建参数判断结果。
- accepted connection 可以共享 listener 的 socket/engine，但连接关闭必须相互独立。

## 3. 已冻结的核心决定

| 项目 | V2 决定 |
|---|---|
| 公共连接对象 | `netins_udp_t` 是统一句柄 |
| 句柄角色 | 未连接对象、客户端、监听器、已接受连接 |
| 创建接口 | `netins_udp_new(NETINS_UDP_TYPE_QUIC/MPQUIC)` |
| 关闭接口 | `netins_udp_close()` 关闭真实传输并释放句柄 |
| 默认模式 | 阻塞 |
| 普通收发 | 默认双向 QUIC stream 的字节流 |
| HTTP/3 | 独立的 `netins_udp_h3_request_*` 扩展 |
| 多路径结果 | 连接后通过 `is_multipath/info/path_count` 查询 |
| 状态结构 | 调用方提供结构体，API 复制输出 |
| 错误码 | 返回 `-1`，详细错误为 errno 风格 |
| 线程模型 | API 内部同步；回调线程归属固定并文档化 |
| `get_fd()` | 返回通知 fd，不返回任何路径 UDP socket |
| API 版本 | `NETINS_UDP_API_VERSION == 2` |

V2 不返回指向内部统计结构的裸指针，也不复制 TCP NetIns 中“只能在
connect/listen 后设置选项”的限制。需要创建 socket/engine 的选项应在 NEW 状态
设置，由 API 在后续创建底层资源时应用。

## 4. 对象和生命周期

### 4.1 初始化

进程第一次创建对象前调用：

```c
int netins_udp_init(void);
```

初始化调用线程安全并采用引用计数。每次成功初始化必须对应一次：

```c
int netins_udp_cleanup(void);
```

最后一次 cleanup 时仍存在公共连接、stream 或 request 对象，返回
`NETINS_UDP_ERROR` 并设置 `errno=EBUSY`，初始化引用不减少。

`netins_udp_new()` 不隐式调用初始化。未初始化时返回 `NULL` 并设置
`errno=EPERM`。

### 4.2 统一句柄

`netins_udp_new(type)` 返回 NEW 状态的句柄。一个句柄只能选择一次角色：

```text
NEW ──connect──> CONNECTING ──handshake──> CONNECTED
 │
 └──listen────> LISTENING ──accept──> 新的 ACCEPTED/CONNECTED 句柄

CONNECTED/LISTENING ──shutdown/close/error──> CLOSING ──> CLOSED/ERROR
```

规则：

- 同一对象不能既 connect 又 listen。
- 已失败的 connect/listen 不允许在原对象上重试；调用方关闭后重新 new。
- `accept()` 返回独立分配的句柄，必须单独 close。
- listener 关闭会唤醒阻塞的 accept，并拒绝新连接。
- listener 关闭时，已经返回给调用方的 accepted 连接继续存活；实现通过内部
  engine 引用计数保证共享资源直到最后一个连接关闭。
- accepted 连接继承 listener 的 transport type、应用协议、TLS、拥塞控制、
  调度器和 socket 缓冲配置；日志回调、阻塞模式和收发超时按 accept 完成时的
  listener 配置复制，之后可以在 accepted 连接上独立修改运行期选项。
- `shutdown()` 发起协议级优雅关闭但不释放公共句柄，便于读取最终状态和原因。
- `close()` 是最终释放操作，返回后指针失效，重复 close 属于调用方错误。

### 4.3 阻塞和非阻塞

默认行为：

- `connect()` 阻塞到握手和应用协议准备完成，或连接超时。
- `accept()` 阻塞到一个连接达到可交付状态。
- `send/recv`、stream 和 H3 body 收发在没有空间/数据时阻塞。
- `send/recv` 超时值为 0 表示无限等待。
- 默认连接超时 20 秒，默认关闭等待 1 秒。

设置 `NETINS_UDP_OPT_NONBLOCK=1` 后：

- `connect()` 启动成功但尚未完成时返回 `-1/EINPROGRESS`。
- `accept()` 暂无连接时返回 `NULL/EAGAIN`。
- 收发暂时无法推进时返回 `-1/EAGAIN`。
- 单次调用可用 `NETINS_UDP_MSG_DONTWAIT` 覆盖阻塞设置。

## 5. 传输类型和多路径回退

`NETINS_UDP_TYPE_MPQUIC` 表示请求启用多路径，不保证最终一定有两条活动路径。

以下情况允许连接继续以单路径工作：

- 对端没有协商多路径。
- 第二块网卡不可用。
- 第二条路径创建或验证失败。
- 运行过程中某一条路径失效。

调用方必须在连接建立后查询：

```c
netins_udp_is_multipath(conn);
netins_udp_get_info(conn, &info);
netins_udp_get_path_count(conn, &count);
```

冻结语义：

- `multipath_requested` 表示创建意图。
- `multipath_mode` 表示 XQUIC 协商结果。
- `multipath_active` 只有在当前至少两条路径可用于传输时为 1。
- `path_count` 是快照中可查询的路径数量。
- `active_path_count` 是当前 ACTIVE 路径数量。

## 6. 应用协议与数据语义

### 6.1 RAW_STREAM

默认应用模式为 `NETINS_UDP_APP_RAW_STREAM`，使用库私有 ALPN
`netins-udp/1`。

连接建立阶段由库创建并确认一条默认双向 stream。因此阻塞 connect/accept
成功返回时，连接级 `send/recv` 已经可用。

RAW_STREAM 只保证本库 V2 客户端和服务端之间互通，不承诺与任意第三方 QUIC
应用协议直接互通。

```c
netins_udp_send(conn, ...);
netins_udp_recv(conn, ...);
```

只操作这条默认 stream，不会在多条 stream 之间拼接数据。额外 stream 必须使用
`netins_udp_stream_*`。

### 6.2 HTTP/3

HTTP/3 模式必须在 connect/listen 前设置：

```c
int protocol = NETINS_UDP_APP_HTTP3;
netins_udp_set_opt(conn, NETINS_UDP_OPT_APP_PROTOCOL,
    &protocol, sizeof(protocol));
```

HTTP/3 模式中，连接级 `netins_udp_send/recv` 和原始
`netins_udp_stream_*` 返回 `-1/EOPNOTSUPP`。GET/POST 使用：

```c
netins_udp_h3_request_open();
netins_udp_h3_request_accept();
netins_udp_h3_request_send();
netins_udp_h3_request_recv();
```

客户端通过 request config 提供 method、path、authority、content type 和
content length。服务端通过 `netins_udp_h3_request_set_response()` 设置状态码和
响应正文长度。

当前 mini 的 GET/POST、多 stream 分片和文件偏移元数据将在阶段七迁入 H3
扩展；这些元数据不进入普通 QUIC stream API。

客户端 H3 调用顺序：

```c
netins_udp_h3_request_config_t config;
netins_udp_h3_request_config_init(&config);
/* 填写 method/path/content_length */
netins_udp_h3_request_open(conn, &config, &request);
/* POST/content_length>0 时循环发送正文，最后一次带 FIN；GET 跳过此步。 */
netins_udp_h3_request_send(request, data, length,
    NETINS_UDP_MSG_FIN);
netins_udp_h3_request_recv(request, buffer, capacity, 0);
netins_udp_h3_request_close(request);
```

服务端 H3 调用顺序：

```c
netins_udp_h3_request_accept(conn, &request);
netins_udp_h3_request_recv(request, buffer, capacity, 0);
netins_udp_h3_request_set_response(request, 200,
    "application/octet-stream", response_length);
netins_udp_h3_request_send(request, response, response_length,
    NETINS_UDP_MSG_FIN);
netins_udp_h3_request_close(request);
```

### 6.3 send/recv

收发接口遵循以下稳定规则：

- 成功返回本次实际接受或交付的字节数，允许部分收发。
- 返回 `-1` 表示失败，通过错误接口或 `errno` 获取原因。
- `recv()` 返回 0 只表示已经收到 FIN 且缓冲数据全部交付。
- `length=0` 返回 0；发送端不会因此自动发送 FIN。
- 发送 FIN 使用 `NETINS_UDP_MSG_FIN`。
- `NETINS_UDP_MSG_PEEK` 只能用于 recv。
- `NETINS_UDP_MSG_DONTWAIT` 可用于 send 或 recv。
- 未定义的 flags 返回 `-1/EOPNOTSUPP`。
- send 返回后，调用方可以立即修改或释放输入缓冲区。
- recv 只写入调用方提供的缓冲区，缓冲区所有权不转移。

V2 flags 是库自身的稳定数值，不承诺与 Linux `MSG_*` 数值相同。

## 7. 服务端模型

listener 内部拥有：

- 监听 UDP socket。
- XQUIC server engine。
- engine/I/O 工作线程。
- 握手完成连接队列。
- incoming stream 或 H3 request 队列。

accepted handle：

- 有独立状态、错误、统计和通知 fd。
- 与 listener 共享底层 UDP socket 和 engine。
- 不允许直接读取共享 UDP socket。
- close 时只关闭对应 QUIC connection。

TLS 证书和私钥通过 `NETINS_UDP_OPT_TLS` 在 listen 前配置。缺少服务端证书或
私钥时 listen 返回错误。

`listen(host, port)` 中 host 为 `NULL` 或空字符串时监听通配地址。连接端 host
不能为空，可以是 IP 地址或域名；V2 契约支持通过 `getaddrinfo()` 解析 IPv4 和
IPv6，并逐个尝试返回的地址。端口有效范围是 1 到 65535。

服务端基本调用顺序：

```c
netins_udp_t *listener = netins_udp_new(NETINS_UDP_TYPE_MPQUIC);
netins_udp_set_opt(listener, NETINS_UDP_OPT_TLS, &tls, sizeof(tls));
if (netins_udp_listen(listener, NULL, 8443) == NULL) {
    /* 查询 listener 错误后 close */
}

netins_udp_t *client = netins_udp_accept(listener, NULL, NULL);
if (client != NULL) {
    netins_udp_recv(client, buffer, sizeof(buffer), 0);
    netins_udp_send(client, response, response_length,
        NETINS_UDP_MSG_FIN);
    netins_udp_close(client);
}
netins_udp_close(listener);
```

## 8. 选项

| 选项 | 值类型 | 可设置阶段 |
|---|---|---|
| `NONBLOCK` | `int` | 任意非 CLOSED 状态 |
| `CONNECT_TIMEOUT_MS` | `uint32_t` | NEW |
| `ACCEPT_TIMEOUT_MS` | `uint32_t` | NEW/LISTENING |
| `SEND_TIMEOUT_MS` | `uint32_t` | 任意非 CLOSED 状态 |
| `RECV_TIMEOUT_MS` | `uint32_t` | 任意非 CLOSED 状态 |
| `CLOSE_TIMEOUT_MS` | `uint32_t` | 任意非 CLOSED 状态 |
| `IDLE_TIMEOUT_MS` | `uint32_t` | NEW |
| `KERNEL_SNDBUF` | `int` | NEW |
| `KERNEL_RCVBUF` | `int` | NEW |
| `INTERFACES` | `netins_udp_interfaces_t` | NEW |
| `APP_PROTOCOL` | `int` | NEW |
| `TLS` | `netins_udp_tls_config_t` | NEW |
| `INITIAL_CWND_PACKETS` | `uint32_t` | NEW |
| `SCHEDULER` | `int` | NEW |
| `CONGESTION_CONTROL` | `int` | NEW |
| `SOCKOPT` | `netins_udp_sockopt_t` | NEW 或底层支持的运行期 |

不可变选项在 connect/listen 开始后返回 `EBUSY`。编译时未启用的拥塞控制器或
调度器返回 `ENOTSUP`，不静默替换为其他算法。

接口列表为空时使用系统默认路由。MPQUIC 配置少于两块网卡时允许建立连接，但
最终 `multipath_active` 通常为 0。

`INTERFACES` 用于客户端路径 socket。服务端监听地址由 listen 的 host 决定，
listener 设置 `INTERFACES` 返回 `EOPNOTSUPP`。内核 socket 缓冲选项会应用到
客户端的每条路径 socket，或服务端共享监听 socket。

## 9. 状态、统计与 ABI

所有输出结构都包含：

```c
uint32_t struct_size;
uint32_t api_version;
```

调用方必须使用对应 `*_INITIALIZER`。getter 要求：

- `api_version == NETINS_UDP_API_VERSION`。
- `struct_size` 至少为当前 V2 结构大小。
- API 只写入自己已知的 V2 字段，不写调用方可能追加的尾部空间。
- getter 返回复制快照，调用方拥有输出内存。
- getter 不把 XQUIC 内部指针暴露给调用方。

V2 公共结构内部使用固定宽度整数、固定大小字符数组和预留字段。枚举值存入
结构体时使用 `int`，避免把编译器相关 enum 大小写入 ABI。

每路径统计至少包含：

- path ID、网卡名称和地址。
- transport/app path 状态。
- 收发、ACK、丢包和 inflight 字节。
- SRTT、latest RTT、min RTT。
- CWND 和 cwnd blocked。
- 区间收发/ACK 吞吐。
- 最近收发时间。

统计字段定义：

- `bytes_sent/received` 是去重后的应用数据字节，不包含 QUIC/H3 framing。
- `bytes_wire_sent/received` 是包含控制包、重传和重注入的 UDP payload 字节。
- `bytes_acked` 是被 ACK 确认的已发送 wire 字节。
- `bytes_inflight` 是快照时尚未确认的 wire 字节。
- `*_mbps` 按相邻内部快照的区间增量计算，单位为 Mbit/s。
- RTT 和时间字段单位为微秒，CWND/inflight 字段单位为字节。

## 10. 错误和关闭原因

规则：

- 生命周期函数失败返回 `NETINS_UDP_ERROR`。
- 指针函数失败返回 `NULL`。
- 数据函数失败返回 `-1`。
- 详细错误码使用 errno 风格，例如 `EINVAL`、`EAGAIN`、`ETIMEDOUT`、
  `ENOTCONN`、`ECONNRESET`、`EOPNOTSUPP`。
- 失败同时设置当前线程的 `errno` 和拥有者句柄的 last error。
- 成功的非查询操作清除拥有者句柄的 last error。
- 纯 getter 和 error getter 不清除 last error。
- last error 是句柄级兼容信息，不是线程局部存储；并发调用时应优先使用失败
  调用所在工作线程的 `errno`。

连接关闭原因与 API 调用错误分离：

```c
netins_udp_get_close_reason(conn, &reason);
```

关闭原因包含来源、系统错误、QUIC transport error、application error 和稳定
文本说明。关闭后、最终 close 前仍可查询。

## 11. `get_fd()` 语义

QUIC accepted connection 不能拥有独占 UDP fd，因此 V2 的：

```c
int netins_udp_get_fd(netins_udp_t *netins);
```

返回库维护的通知 fd，而不是 UDP socket。

规则：

- 可用于 epoll/select/poll。
- 调用方不得直接 read/write/close。
- fd 可读后调用 `netins_udp_get_events()` 获取并确认事件。
- 每个 listener/connection 都有独立通知 fd。
- 关闭句柄后 fd 失效。
- `READABLE/WRITABLE` 表示默认 stream 或当前 HTTP/3 对象可能推进；
  `STREAM_ACCEPTABLE` 和 `H3_REQUEST_ACCEPTABLE` 分别表示对应接受队列非空。

该设计避免第三方读取 UDP 包并与 API engine 发生双接收。

## 12. 线程安全和回调

V2 目标是公共 API 线程安全：

- 不同连接可以由不同线程同时操作。
- 同一连接允许一个发送线程和一个接收线程并行。
- 不同 stream/request 可以并行操作。
- 同一 stream 同一方向的并发调用由 API 串行化，但调用方不应依赖调用顺序。
- `close()` 与正在阻塞的操作并发时，API 唤醒阻塞调用并使其以
  `ECANCELED/ECONNABORTED` 结束，然后再释放资源。

日志回调由 API engine 工作线程调用。回调要求：

- 不能长时间阻塞。
- 不能在回调中 close 当前对象。
- message 只在回调期间有效。
- user_data 生命周期由调用方管理。

## 13. V1 到 V2 迁移

| V1 | V2 |
|---|---|
| `netins_udp_new(void)` 状态对象 | 重命名为内部 `netins_udp_observer_new()` |
| `netins_udp_close()` 释放状态引用 | `netins_udp_close()` 关闭真实对象 |
| `netins_udp_open(connection_id)` | 诊断观察器扩展，不属于核心传输 API |
| `netins_udp_client_create()` | `netins_udp_new()` + `netins_udp_set_opt()` |
| `netins_udp_client_connect()` | `netins_udp_connect()` |
| `netins_udp_client_close()` | `netins_udp_close()` |
| V1 `netins_udp_stream_*` H3 request | V2 `netins_udp_h3_request_*` |
| 无服务端对象 | `listen/accept` |
| 无普通 QUIC stream | 连接级 send/recv + V2 stream API |
| client snapshot | 统一句柄 snapshot |

V1 观察器注册表可保留为内部诊断能力，阶段三使用
`netins_udp_observer_*` 命名，不能继续占用统一连接 API 名称。

## 14. 阶段二不实现的内容

本阶段不做以下工作：

- 不替换正式 `include/xquic/netins_udp.h`。
- 不修改当前 mini_client/mini_server 调用。
- 不实现 server engine、accept 队列或默认 stream。
- 不改变拥塞控制、调度器和 RX 线程行为。
- 不新增正式导出符号。

阶段三首先完成观察器重命名和内部模块拆分，并保证现有 V1 功能仍可编译；之后
才将统一连接对象逐步接入正式实现。
