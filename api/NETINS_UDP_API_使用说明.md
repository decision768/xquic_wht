# NetIns UDP API 使用说明

## 1. 概述

NetIns UDP 是基于 XQUIC 的 QUIC/Multipath QUIC 封装库。第三方只需要使用：

```c
#include <xquic/netins_udp.h>
```

公共接口统一采用 `netins_udp_xxx()` 命名，不要求调用方选择两套 API。它提供：

- QUIC 或 MPQUIC 客户端；
- HTTP/3 服务端 listener 和 accepted connection；
- HTTP/3 GET、POST、请求体和响应体收发；
- 连接状态、多路径状态、每路径统计和关闭原因；
- 可加入 `poll`/`epoll` 的独立通知描述符。

API 封装现有 XQUIC 能力，不改变拥塞控制、调度器、mini 程序的 UDP
收包方式或 XQUIC 公共 ABI。当前公共数据面明确为 HTTP/3，不提供类似 TCP
连接字节流的 `netins_udp_send()`/`netins_udp_recv()`。

## 2. 头文件、库和兼容标识

安装产物：

- 头文件：`include/xquic/netins_udp.h`
- 动态库：`lib/libxquic.so`
- 静态库：`lib/libxquic.a`
- 文档和示例：`${CMAKE_INSTALL_DOCDIR}/netins_udp`

进程启动时可校验头文件和库是否匹配：

```c
if (netins_udp_get_api_version() != NETINS_UDP_API_VERSION) {
    /* 拒绝加载不匹配的库 */
}
```

`NETINS_UDP_API_VERSION` 是 ABI 兼容标识，不是让业务方选择不同产品接口的
开关。所有公开结构体都包含 `struct_size` 和 `api_version`。必须使用对应的
`NETINS_UDP_*_INITIALIZER` 或初始化函数，不能传入未初始化结构体。

## 3. 返回值和错误

- 普通函数成功返回 `NETINS_UDP_OK`（0），失败返回
  `NETINS_UDP_ERROR`（-1）。
- `netins_udp_new()`、`netins_udp_accept()` 等指针函数失败返回 `NULL`。
- HTTP/3 收发成功返回实际处理的字节数，失败返回 -1；接收返回 0 表示
  对端已经结束该方向的 body。
- 失败时同步设置 `errno`。
- 连接对象的详细错误通过 `netins_udp_get_last_error_code()` 和
  `netins_udp_get_last_error_msg()` 查询。
- request 对象的错误通过同名的
  `netins_udp_h3_request_get_last_error_xxx()` 查询。
- 连接关闭后的来源、系统错误、QUIC transport error 和 application error
  通过 `netins_udp_get_close_reason()` 查询。

错误信息属于对象的最近一次调用结果，其他线程继续操作同一对象后可能更新；
需要记录时应立即复制。

## 4. 全局初始化和对象模型

### 4.1 初始化

```c
int netins_udp_init(void);
int netins_udp_cleanup(void);
```

`netins_udp_init()` 是线程安全、引用计数的。每次成功初始化都应有一次
`netins_udp_cleanup()`。最后一次 cleanup 时仍有连接、listener、accepted
connection、request 对象，或已经与对象脱离但尚未退出的异步 DNS 解析线程，
会返回 -1 并置 `errno=EBUSY`。关闭所有对象后若仍返回 `EBUSY`，应等待解析
结束并重试 cleanup，不能在此之前卸载动态库。

推荐顺序：

```c
netins_udp_init();
/* 创建、使用并关闭所有对象 */
netins_udp_cleanup();
```

### 4.2 连接对象

```c
netins_udp_t *netins_udp_new(int type);
void netins_udp_close(netins_udp_t *netins);
```

`type` 可为：

- `NETINS_UDP_TYPE_QUIC`：请求普通 QUIC；
- `NETINS_UDP_TYPE_MPQUIC`：请求多路径 QUIC。

MPQUIC 是请求意图，不等于一定有两条活动路径。建立连接后应通过
`netins_udp_is_multipath()`、`netins_udp_get_path_count()` 或 snapshot
确认实际协商和路径状态。

同一个 `netins_udp_t` 只能选择一次 `connect` 或 `listen`。`netins_udp_close()`
会释放句柄，返回后指针失效，不得再次调用。传入 `NULL` 可以安全返回。

对象角色：

- client：`new -> connect -> shutdown/close`
- listener：`new -> listen -> accept... -> close`
- accepted connection：`accept -> request_accept... -> shutdown/close`

accepted connection 与 listener 在内部共享服务端 UDP socket 和 XQUIC engine，
但拥有独立的 QUIC 生命周期和通知描述符。关闭 listener 会唤醒阻塞的 accept，
不会强制释放已经交给应用的 accepted connection。

## 5. 连接选项

```c
int netins_udp_set_opt(netins_udp_t *obj, int option,
    const void *value, size_t value_len);
int netins_udp_get_opt(netins_udp_t *obj, int option,
    void *value, size_t *value_len);
```

主要选项：

| 选项 | 值类型 | 默认值 | 说明 |
|---|---:|---:|---|
| `NETINS_UDP_OPT_NONBLOCK` | `int` | 0 | 0 阻塞，1 非阻塞 |
| `NETINS_UDP_OPT_CONNECT_TIMEOUT_MS` | `uint32_t` | 20000 | 完整连接建立超时（含 DNS、socket/engine 初始化和握手），不得为 0 |
| `NETINS_UDP_OPT_ACCEPT_TIMEOUT_MS` | `uint32_t` | 0 | accept 超时，0 表示无限等待 |
| `NETINS_UDP_OPT_SEND_TIMEOUT_MS` | `uint32_t` | 0 | request send 超时，0 表示无限等待 |
| `NETINS_UDP_OPT_RECV_TIMEOUT_MS` | `uint32_t` | 0 | request recv/accept 超时，0 表示无限等待 |
| `NETINS_UDP_OPT_CLOSE_TIMEOUT_MS` | `uint32_t` | 1000 | 优雅关闭等待时间，不得为 0 |
| `NETINS_UDP_OPT_IDLE_TIMEOUT_MS` | `uint32_t` | 0 | 交给 XQUIC 的连接空闲超时配置 |
| `NETINS_UDP_OPT_KERNEL_SNDBUF` | `int` | 16 MiB | UDP socket 发送缓冲区请求值 |
| `NETINS_UDP_OPT_KERNEL_RCVBUF` | `int` | 16 MiB | UDP socket 接收缓冲区请求值 |
| `NETINS_UDP_OPT_INTERFACES` | `netins_udp_interfaces_t` | 空 | 客户端路径接口名 |
| `NETINS_UDP_OPT_APP_PROTOCOL` | `int` | HTTP/3 | 当前只接受 `NETINS_UDP_APP_HTTP3` |
| `NETINS_UDP_OPT_TLS` | `netins_udp_tls_config_t` | 校验证书 | TLS 和证书文件 |
| `NETINS_UDP_OPT_INITIAL_CWND_PACKETS` | `uint32_t` | 100 | 初始拥塞窗口包数 |
| `NETINS_UDP_OPT_SCHEDULER` | `int` | default | XQUIC 多路径调度器 |
| `NETINS_UDP_OPT_CONGESTION_CONTROL` | `int` | default | XQUIC 拥塞控制算法 |

除以下运行时选项外，其余选项必须在 `connect()`/`listen()` 前设置：

- `NONBLOCK`
- `SEND_TIMEOUT_MS`
- `RECV_TIMEOUT_MS`
- `CLOSE_TIMEOUT_MS`
- listener 上的 `ACCEPT_TIMEOUT_MS`

连接开始后修改其他选项返回 `EBUSY`。`get_opt()` 中的 `value_len` 是输入/输出
参数；`value=NULL` 可先查询所需长度，缓冲区过小返回 `ENOBUFS` 并写回长度。

运行时选项采用“创建时快照”语义：已创建或已 accept 的 HTTP/3 request 保留其
创建时的 `NONBLOCK`、发送/接收/关闭超时；父连接上的后续修改只作用于之后创建
的 request。accepted connection 在 accept 时继承 listener 的运行时选项，此后
可独立修改；listener 的修改不追溯到已经返回给应用的 accepted connection。

非阻塞 `connect()` 不在调用线程执行 DNS、socket 绑定、XQUIC engine 初始化或
握手；这些工作由内部 worker 完成。成功立即完成时可直接返回 0，否则返回
`-1/EINPROGRESS`，最终结果通过状态与通知获取。异步状态进入 `CONNECTED`
时，建立阶段的临时 `EINPROGRESS` 会被清除；进入 `ERROR` 时，错误查询接口会
直接同步底层失败原因，无需先调用状态接口。

如果参数检查或底层创建在 transport 对象生成前失败，连接句柄仍可用于
`get_state()`、`get_info()`、`get_snapshot()`、错误查询和 `close()`；此时连接
ID 为无效值，路径和 transport 计数为空，不会因为缺少底层对象而让状态快照
再次失败。

接口名使用 Linux `SO_BINDTODEVICE` 时可能需要网络管理能力。listener 不接受
`INTERFACES` 选项，因为 listener 只拥有一套共享 UDP 接收路径。

## 6. 客户端连接

阻塞示例：

```c
netins_udp_t *client = NULL;

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
snprintf(interfaces.names[0], sizeof(interfaces.names[0]), "rmnet_data0");
snprintf(interfaces.names[1], sizeof(interfaces.names[1]), "usb0");
if (netins_udp_set_opt(client, NETINS_UDP_OPT_INTERFACES,
        &interfaces, sizeof(interfaces)) != NETINS_UDP_OK) {
    goto done;
}

netins_udp_tls_config_t tls = NETINS_UDP_TLS_CONFIG_INITIALIZER;
snprintf(tls.server_name, sizeof(tls.server_name), "test.xquic.com");
if (netins_udp_set_opt(client, NETINS_UDP_OPT_TLS,
        &tls, sizeof(tls)) != NETINS_UDP_OK) {
    goto done;
}

if (netins_udp_connect(client, "10.12.54.143", 8443)
    != NETINS_UDP_OK) {
    goto done;
}

/* 此时 QUIC/HTTP3 握手已经完成，额外路径可能仍在验证。 */

done:
netins_udp_close(client);
netins_udp_cleanup();
```

非阻塞模式下，`netins_udp_connect()` 启动连接后返回 -1，`errno=EINPROGRESS`。
将 `netins_udp_get_fd(client)` 加入 `poll`/`epoll`，收到通知后调用
`netins_udp_get_events()`；出现 `CONNECTED` 后再检查 state。

客户端当前支持系统默认信任链和 `verify_peer`。自定义 CA 文件、客户端证书和
私钥尚未由当前 client backend 支持，设置后 connect 返回 `EOPNOTSUPP`。

## 7. 服务端生命周期

listener 必须配置证书和私钥：

```c
netins_udp_t *listener = netins_udp_new(NETINS_UDP_TYPE_MPQUIC);

netins_udp_tls_config_t tls = NETINS_UDP_TLS_CONFIG_INITIALIZER;
snprintf(tls.certificate_file, sizeof(tls.certificate_file),
    "/etc/netins/server.crt");
snprintf(tls.private_key_file, sizeof(tls.private_key_file),
    "/etc/netins/server.key");
netins_udp_set_opt(listener, NETINS_UDP_OPT_TLS, &tls, sizeof(tls));

if (netins_udp_listen(listener, NULL, 8443) == NULL) {
    /* 查询 listener 的 last error */
}

netins_udp_t *connection = netins_udp_accept(listener, NULL, NULL);
if (connection != NULL) {
    /* 处理 connection */
    netins_udp_shutdown(connection, 0);
    netins_udp_close(connection);
}
netins_udp_close(listener);
```

阻塞 accept 根据 `ACCEPT_TIMEOUT_MS` 等待。非阻塞或暂无连接时返回 `NULL`，
`errno=EAGAIN`。如果需要对端地址，可按 POSIX `accept` 方式同时传入
`struct sockaddr *addr` 和 `socklen_t *addrlen`。

服务端当前不支持客户端证书校验和自定义 CA；设置
`require_client_certificate` 或 `ca_file` 后 listen 返回 `EOPNOTSUPP`。

## 8. HTTP/3 请求和数据收发

`authority` 为空时，客户端自动使用 TLS `server_name`（未设置则使用 connect
host）生成 HTTP/3 `:authority`；HTTPS 非默认端口会自动追加，IPv6 字面量会
使用方括号。API 不会自动加入 mini 传输协议的 `x-stream-*` 头；确有兼容需求时
由调用方通过 `netins_udp_h3_request_config_add_header()` 显式添加。

### 8.1 客户端 GET

```c
netins_udp_h3_request_config_t request_config;
netins_udp_h3_request_config_init(&request_config);
snprintf(request_config.method, sizeof(request_config.method), "GET");
snprintf(request_config.path, sizeof(request_config.path), "/file.bin");

netins_udp_h3_request_t *request = NULL;
if (netins_udp_h3_request_open(client, &request_config, &request)
    != NETINS_UDP_OK) {
    /* error */
}

unsigned char buffer[64 * 1024];
for (;;) {
    ssize_t n = netins_udp_h3_request_recv(
        request, buffer, sizeof(buffer), NETINS_UDP_MSG_NONE);
    if (n > 0) {
        /* 消费 buffer[0..n) */
    } else if (n == 0) {
        break;
    } else {
        /* error */
        break;
    }
}
netins_udp_h3_request_close(request);
```

### 8.2 客户端 POST

固定长度 body 的 `content_length` 必须等于最终发送的总字节数。最后一批数据
使用 `NETINS_UDP_MSG_FIN`：

```c
netins_udp_h3_request_config_t cfg;
netins_udp_h3_request_config_init(&cfg);
snprintf(cfg.method, sizeof(cfg.method), "POST");
snprintf(cfg.path, sizeof(cfg.path), "/upload");
cfg.content_length = file_size;
snprintf(cfg.content_type, sizeof(cfg.content_type),
    "application/octet-stream");

netins_udp_h3_request_t *request = NULL;
netins_udp_h3_request_open(client, &cfg, &request);

/* send 可能部分成功，调用方必须循环发送剩余数据。 */
ssize_t n = netins_udp_h3_request_send(request, data, length,
    is_final_chunk ? NETINS_UDP_MSG_FIN : NETINS_UDP_MSG_NONE);
```

若长度未知，将 `content_length` 设为
`NETINS_UDP_CONTENT_LENGTH_UNKNOWN`，由最后一次 `FIN` 定义 body 结束。

### 8.3 服务端请求和响应

accepted connection 收到请求后：

```c
netins_udp_h3_request_t *request = NULL;
if (netins_udp_h3_request_accept(connection, &request)
    == NETINS_UDP_OK) {
    netins_udp_h3_request_info_t info =
        NETINS_UDP_H3_REQUEST_INFO_INITIALIZER;
    netins_udp_h3_request_get_info(request, &info);

    /* 循环 recv，直到返回 0，或者业务明确提前响应/关闭。 */

    netins_udp_h3_response_config_t response;
    netins_udp_h3_response_config_init(&response);
    response.status = 200;
    response.content_length = 2;
    snprintf(response.content_type, sizeof(response.content_type),
        "text/plain");
    netins_udp_h3_request_set_response(request, &response);
    netins_udp_h3_request_send(request, "OK", 2, NETINS_UDP_MSG_FIN);
    netins_udp_h3_request_close(request);
}
```

`set_response()` 每个服务端 request 只能成功一次，status 范围为 200～599。
应用提前响应或公开 close request 时，未读请求体由后端丢弃式 drain，不会让
大 POST 因应用不再读取而永久卡住。

HEAD 请求以及 204、205、304 响应没有响应 body。204 不在报文中发送
`Content-Length`；205 必须将 `content_length` 设为 0；304 可以声明资源的
表示长度，但仍不能发送 DATA。对这些响应调用 body send 会返回
`EOPNOTSUPP`。

### 8.4 收发共同语义

- send 允许部分成功，返回正数时必须继续发送余下数据。
- recv 返回 0 表示当前 HTTP body 已结束。
- `NETINS_UDP_MSG_DONTWAIT` 只影响本次调用；无进展返回 `EAGAIN`。
- `NETINS_UDP_MSG_PEEK` 仅用于 recv。
- `NETINS_UDP_MSG_FIN` 仅用于 send。
- 有限超时到期返回 `ETIMEDOUT`。
- body 队列有界，应用线程不直接进入 XQUIC engine。
- client 每个 request 默认预留 4 MiB 发送队列和 16 MiB 接收队列，每连接
  总预算分别为 64 MiB 和 256 MiB；超过预算时新建 request 返回
  `ENOBUFS`。
- server 每个 request 的发送/接收预留各为 4 MiB；每 accepted connection
  的发送/接收总预算各为 64 MiB，全 server 的发送/接收总预算各为
  256 MiB。预算是并发 request 的最坏情况预留上限，不是立即实际分配量。
- `bytes_sent/bytes_received` 在 request info 中只统计 HTTP body，不含 H3
  headers、QPACK 和 framing。
- 每个 request 都必须独立 close，即使父连接已经关闭。

自定义 header 名必须是小写、不能以 `:` 开头，也不能覆盖库管理的
pseudo-header、`content-length`、`content-type` 或 `host`。HTTP/3 禁止的
`connection`、`proxy-connection`、`keep-alive`、`transfer-encoding` 和
`upgrade` 会被拒绝；`te` 只允许值 `trailers`。

## 9. 状态、多路径和统计

```c
int state = netins_udp_get_state(connection);
int multipath = netins_udp_is_multipath(connection);

uint32_t count = 0;
netins_udp_get_path_count(connection, &count);

netins_udp_snapshot_t snapshot = NETINS_UDP_SNAPSHOT_INITIALIZER;
netins_udp_get_snapshot(connection, &snapshot);
```

推荐一次读取 `netins_udp_get_snapshot()`，它在同一个复制点返回：

- `info`：角色、状态、连接 ID、握手、多路径和路径数量；
- `statis`：总流量、速率、ACK、丢失代理值和 inflight；
- `paths[]`：路径 ID、稳定 path index、接口、地址、RTT、CWND 和流量；
- `close_reason`：关闭来源和错误。

`path_index` 是连接生命周期内首次观察到路径时分配的稳定槽位，不重用。
调用 `netins_udp_get_path_statis(connection, index, ...)` 时传入的是这个槽位
索引。最多公开 `NETINS_UDP_MAX_PATHS` 条；历史路径超过上限时
`path_table_truncated` 非零。

统计定义：

- `bytes_sent/bytes_received`：去重后的 QUIC STREAM payload；不含 QUIC
  packet/frame 开销、重传和 reinjection 重复字节；
- 逐路径 `bytes_sent` 归属于首次发送该唯一 payload 的路径；逐路径
  `bytes_received` 归属于最终提供新消费 stream offset 的那份接收数据所在
  路径。因此它们用于唯一应用数据归因，不等于逐路径线速流量；
- 连接级 stream payload 在 HTTP/3 模式下包含 H3/QPACK/control stream；
- `bytes_wire_sent/bytes_wire_received`：UDP 线速字节；
- `bytes_lost`：编码后的重传字节代理值，不是精确丢失字节；
- `loss_rate`：基于 packet 的丢失比例；
- `send_mbps/receive_mbps/acked_mbps`：相邻快照采样区间速率。

这些接口复用 NetIns 私有 observer，不修改 `xqc_conn_get_stats()` 的公共
结构和原有语义。

`close_reason` 采用“首次终止原因冻结”语义：本地 shutdown、超时、对端关闭
或网络错误一旦成为关闭原因，后续 transport 状态不会改写其 source 和
application error；后续观察到的原始 QUIC transport error 只会补充到尚为空
的 `transport_error` 字段。

## 10. poll/epoll 通知

每个 listener、client、accepted connection 和 HTTP/3 request 都拥有独立
通知描述符：

```c
int fd = netins_udp_get_fd(connection);
/* 将 fd 加入 poll/epoll，只监听可读。 */

uint32_t events = 0;
if (netins_udp_get_events(connection, &events) == NETINS_UDP_OK) {
    if (events & NETINS_UDP_EVENT_CONNECTED) { /* ... */ }
    if (events & NETINS_UDP_EVENT_H3_REQUEST_ACCEPTABLE) { /* ... */ }
}
```

request 使用 `netins_udp_h3_request_get_fd()` 和
`netins_udp_h3_request_get_events()`。

事件：

- `CONNECTED`
- `ACCEPTABLE`
- `H3_REQUEST_ACCEPTABLE`
- `READABLE`
- `WRITABLE`
- `PATH_CHANGED`
- `CLOSED`
- `ERROR`

`CONNECTED/PATH_CHANGED/CLOSED/ERROR` 是锁存的状态变化；
`ACCEPTABLE/READABLE/WRITABLE/H3_REQUEST_ACCEPTABLE` 是当前条件。必须调用
`get_events()` 消费通知；不要直接 read，也不要 close 该 fd。若 level 条件
仍成立，库会在返回前重新唤醒，因此正确用法是持续 accept/recv/send 直到
`EAGAIN`，避免 epoll 忙循环。

通知 fd 不是 UDP socket。第三方不能用它直接收 UDP 包，因而不会形成第二个
UDP 接收者或破坏 mini/Libevent 的收包逻辑。

## 11. 关闭顺序和线程安全

推荐顺序：

1. 停止向对象发起新调用；
2. 关闭所有 HTTP/3 request；
3. 对连接按需调用 `netins_udp_shutdown()` 并查询最终状态；
4. `netins_udp_close(connection)`；
5. `netins_udp_close(listener)`；
6. 所有公开对象都释放后调用 `netins_udp_cleanup()`。

公开 getter、选项和生命周期入口内部有同步保护。不同对象可以由不同线程使用。
同一对象发生 `close` 时，库会取消/等待已经进入 API 的调用，但应用仍需保证
close 开始后不再从其他线程提交新调用。指针一旦 close 返回即失效。

应用传入的选项结构体、字符串、header 和发送数据在调用返回前由库复制或消费；
getter 将结果复制到调用方提供的结构体。通知 fd 和所有对象内存由库拥有。

协议处理、ACK、定时器和应用回调仍由对应的 XQUIC engine 串行执行；API 的
线程安全不意味着同一 engine 内协议处理会并行。

## 12. 当前边界

- 公共应用协议仅为 HTTP/3。
- 不发布 connection-level raw stream API。
- client 暂不支持自定义 CA 和客户端证书。
- server 暂不支持客户端证书校验。
- listener 暂不接受按接口创建多套 UDP 收包 socket。
- Linux x86_64 是当前主要验收环境；AArch64 仍需在实际交付工具链上编译、
  链接和运行验证。
- API 不承诺对应用隐藏网络权限问题；`SO_BINDTODEVICE`、路由、证书和防火墙
  仍由部署环境保证。

## 13. 编译与示例

```sh
cmake -S . -B build \
  -DSSL_TYPE=babassl \
  -DSSL_PATH=/path/to/Tongsuo-or-BabaSSL \
  -DNETINS_UDP_BUILD_TESTS=ON \
  -DNETINS_UDP_BUILD_CONNECT_EXAMPLE=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

示例文件：

- `netins_udp_connect_example.c`：阻塞连接和 snapshot；
- `netins_udp_client_lifecycle_example.c`：完整 client 生命周期；
- `netins_udp_server_lifecycle_example.c`：listen/accept/request/response；
- `netins_udp_h3_request_example.c`：GET 接收；
- `netins_udp_snapshot_example.c`：连接和逐路径统计。

命令行连接检查：

```sh
./build/netins_udp_connect_cli \
  10.12.54.143 8443 test.xquic.com rmnet_data0 usb0
```

正式交付前还应在目标 x86_64/AArch64 环境完成真实 GET、POST、大文件、并发
request、listener/accepted 不同关闭顺序、断网、超时、路径切换和长时间运行
验收。
