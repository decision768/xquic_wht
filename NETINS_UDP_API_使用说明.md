# NetIns UDP Mini API 使用说明

本文面向集成 Mini API 的第三方 C/C++ 开发者，说明当前 API v1 的公开接口、连接建立、
GET/POST 文件收发、状态查询、错误处理和连接关闭流程。

公开接口以 [`include/xquic/netins_udp.h`](include/xquic/netins_udp.h) 为准。应用程序只需：

```c
#include <xquic/netins_udp.h>
```

不要包含 `mini/*.h`，也不要直接操作 XQUIC engine、CID、HTTP/3 request、Libevent 或
mini 私有上下文。

## 1. API 能做什么

Mini API 是基于 QUIC/HTTP3 的阻塞式完整文件任务 API，不是 TCP socket 风格的字节流 API。
当前没有公开 `send()`、`recv()`、`read()` 或 `write()` 接口：

- `netins_udp_connect()` 建立客户端 QUIC/HTTP3 连接；
- `netins_udp_transfer()` 在已连接客户端上执行一次完整 GET 或 POST；
- `netins_udp_listen()` 和 `netins_udp_accept()` 建立服务端监听并接收连接；
- `netins_udp_serve()` 在 accepted 连接上处理 GET/POST，直到对端关闭；
- `netins_udp_shutdown()` 执行协议关闭；
- `netins_udp_close()` 释放 handle 和本地资源。

GET/POST 的文件含义如下：

| 操作 | `local_path`（客户端） | `remote_path`（服务端） | 数据方向 |
|---|---|---|---|
| GET | 下载结果文件 | `root_path` 下的源文件 | 服务端 → 客户端 |
| POST | 要上传的源文件 | `root_path` 下的目标文件 | 客户端 → 服务端 |

例如服务端 `root_path=/root/test_xquic_server`：

- GET `/upload/1` 读取 `/root/test_xquic_server/upload/1`；
- POST `/download/kkk.txt` 写入 `/root/test_xquic_server/download/kkk.txt`。

`remote_path` 必须以 `/` 开头，不能只有 `/`。服务端拒绝空路径、`.`、`..`、反斜杠以及
符号链接穿越；POST 会创建或截断目标文件，但其父目录必须已经存在。

## 2. 生命周期总览

### 2.1 客户端

```text
netins_udp_init()
    ↓
netins_udp_new(NETINS_UDP_ROLE_CLIENT)
    ↓
netins_udp_set_opt()                 可选；connect 前设置
    ↓
netins_udp_connect()
    ↓
netins_udp_transfer(GET/POST)        同一连接可顺序执行多次
    ↓
状态/路径/快照查询                    可选
    ↓
netins_udp_shutdown()
    ↓
netins_udp_close()
    ↓
netins_udp_cleanup()
```

### 2.2 服务端

```text
netins_udp_init()
    ↓
netins_udp_new(NETINS_UDP_ROLE_SERVER)
    ↓
netins_udp_set_opt(TLS/超时/日志...)  listen 前设置
    ↓
netins_udp_listen()
    ↓
netins_udp_accept()                  返回 ROLE_ACCEPTED handle
    ↓
netins_udp_serve()                   处理该连接，直到对端关闭/超时
    ↓
netins_udp_shutdown(accepted)
netins_udp_close(accepted)
    ↓
继续 accept，或关闭 listener
    ↓
netins_udp_shutdown(server)
netins_udp_close(server)
netins_udp_cleanup()
```

## 3. 全部公开 API

当前 `NETINS_UDP_API_VERSION` 为 `1`，公开函数共 20 个。

### 3.1 全局初始化

| 接口 | 返回值 | 说明 |
|---|---|---|
| `uint32_t netins_udp_get_api_version(void)` | API 版本号 | 可在加载库后检查头文件与动态库版本 |
| `int netins_udp_init(void)` | `0` 成功，`-1` 失败 | 进程级初始化，内部引用计数 |
| `int netins_udp_cleanup(void)` | `0` 成功，`-1` 失败 | 与 `init()` 配对；最终清理时仍有 handle 会以 `EBUSY` 失败 |

### 3.2 Handle 与配置

| 接口 | 返回值 | 说明 |
|---|---|---|
| `netins_udp_t *netins_udp_new(int role)` | handle 或 `NULL` | 只允许创建 `CLIENT` 或 `SERVER` |
| `int netins_udp_set_opt(netins_udp_t *h, int opt, const void *value, size_t len)` | `0/-1` | 复制配置；除 logger 外必须在 `NEW` 状态调用 |
| `void netins_udp_close(netins_udp_t *h)` | 无 | 必要时自动尝试 shutdown，然后释放 handle；每个 handle 只能调用一次 |

`netins_udp_accept()` 返回的 `NETINS_UDP_ROLE_ACCEPTED` handle 也归调用方所有，必须单独
`netins_udp_close()`。

### 3.3 建连、监听和收发

| 接口 | 适用角色 | 阻塞行为 |
|---|---|---|
| `int netins_udp_connect(netins_udp_t *client, const char *host, uint16_t port)` | CLIENT | 阻塞到握手完成、失败或建连超时 |
| `int netins_udp_listen(netins_udp_t *server, const char *host, uint16_t port)` | SERVER | 创建并绑定 UDP listener |
| `netins_udp_t *netins_udp_accept(netins_udp_t *server)` | SERVER | 阻塞到连接可用或 accept 超时 |
| `int netins_udp_transfer(netins_udp_t *client, const netins_udp_transfer_t *task, netins_udp_transfer_result_t *result)` | 已连接 CLIENT | 阻塞到一次 GET/POST 文件任务完成、失败或超时 |
| `int netins_udp_serve(netins_udp_t *accepted, const netins_udp_serve_config_t *config)` | ACCEPTED | 阻塞处理该连接，直到对端关闭、失败或超时 |
| `int netins_udp_shutdown(netins_udp_t *h)` | CLIENT/SERVER/ACCEPTED | 阻塞执行关闭，受 close timeout 限制 |

当前 `connect()` 和 `listen()` 的 `host` 必须是数值 IPv4，例如 `124.70.219.194`、
`127.0.0.1` 或 `0.0.0.0`，不直接进行域名解析。TLS SNI 通过 `NETINS_UDP_OPT_TLS`
中的 `server_name` 单独配置。

### 3.4 状态和统计

| 接口 | 返回值/输出 | 说明 |
|---|---|---|
| `int netins_udp_get_state(netins_udp_t *h)` | `netins_udp_state_t` | 返回状态枚举；调用时会非阻塞泵一次内部事件 |
| `int netins_udp_is_multipath(netins_udp_t *h)` | `1`/`0`/`-1` | 只有多路径模式开启且活动路径大于 1 才返回 1 |
| `int netins_udp_get_path_count(netins_udp_t *h, uint32_t *count)` | `0/-1` | 获取当前路径总数 |
| `int netins_udp_get_path_statis(netins_udp_t *h, size_t index, netins_udp_path_statis_t *out)` | `0/-1` | 获取指定路径统计 |
| `int netins_udp_get_snapshot(netins_udp_t *h, netins_udp_snapshot_t *out)` | `0/-1` | 一次获取连接、握手、多路径及所有路径统计 |
| `int netins_udp_get_close_reason(netins_udp_t *h, netins_udp_close_reason_t *out)` | `0/-1` | 获取关闭来源、系统错误、传输错误和文本 |

`get_state()` 不是普通的 `0/-1` 状态码函数。正常情况下它直接返回
`NETINS_UDP_STATE_NEW` 至 `NETINS_UDP_STATE_ERROR` 中的一个值。

### 3.5 错误详情

| 接口 | 说明 |
|---|---|
| `int netins_udp_get_last_error_code(netins_udp_t *h)` | 返回该 handle 最近一次连接级错误码；无错误时为 0 |
| `int netins_udp_get_last_error_msg(netins_udp_t *h, char *buf, size_t len)` | 将最近错误文本复制到调用方缓冲区 |

除 `get_state()`、`is_multipath()`、`get_last_error_code()` 和指针返回函数外，公开的
`int` 函数通常以 `0` 表示成功、`-1` 表示失败。失败时同时检查：

1. `errno`：API 参数、系统调用或通用错误；
2. `netins_udp_get_last_error_code()`：handle 保存的错误码；
3. `netins_udp_get_last_error_msg()`：便于打印的错误说明；
4. `netins_udp_get_close_reason()`：连接为什么关闭。

## 4. 角色和状态

### 4.1 Handle 角色

| 枚举 | 创建来源 | 用途 |
|---|---|---|
| `NETINS_UDP_ROLE_CLIENT` | `netins_udp_new()` | connect、GET、POST |
| `NETINS_UDP_ROLE_SERVER` | `netins_udp_new()` | listen、accept |
| `NETINS_UDP_ROLE_ACCEPTED` | `netins_udp_accept()` | serve 一个已接受连接 |

应用不能直接用 `netins_udp_new()` 创建 `ROLE_ACCEPTED`。

### 4.2 状态

| 状态 | 含义 |
|---|---|
| `NETINS_UDP_STATE_NEW` | handle 已创建，尚未 connect/listen |
| `NETINS_UDP_STATE_CONNECTING` | 客户端正在建连 |
| `NETINS_UDP_STATE_LISTENING` | 服务端正在监听 |
| `NETINS_UDP_STATE_CONNECTED` | 客户端或 accepted 连接可用 |
| `NETINS_UDP_STATE_CLOSING` | 正在关闭 |
| `NETINS_UDP_STATE_CLOSED` | 已完成关闭 |
| `NETINS_UDP_STATE_ERROR` | 发生不可继续的连接错误 |

发生传输超时、传输协议错误或连接关闭后，不应继续在该 client handle 上调用
`netins_udp_transfer()`；应关闭它并重新创建连接。

## 5. 配置接口

所有公开结构体都必须使用对应的 `*_INITIALIZER` 初始化。初始化器会设置
`struct_size` 和 `api_version`，API 用这两个字段检查 ABI：

```c
netins_udp_interfaces_t interfaces = NETINS_UDP_INTERFACES_INITIALIZER;
netins_udp_tls_config_t tls = NETINS_UDP_TLS_CONFIG_INITIALIZER;
netins_udp_logger_config_t logger = NETINS_UDP_LOGGER_CONFIG_INITIALIZER;
netins_udp_transfer_t task = NETINS_UDP_TRANSFER_INITIALIZER;
netins_udp_transfer_result_t result =
    NETINS_UDP_TRANSFER_RESULT_INITIALIZER;
netins_udp_serve_config_t serve = NETINS_UDP_SERVE_CONFIG_INITIALIZER;
netins_udp_snapshot_t snapshot = NETINS_UDP_SNAPSHOT_INITIALIZER;
netins_udp_path_statis_t path = NETINS_UDP_PATH_STATIS_INITIALIZER;
netins_udp_close_reason_t reason = NETINS_UDP_CLOSE_REASON_INITIALIZER;
```

不要对结构体只做局部赋值，也不要手工猜测 `struct_size`。

### 5.1 `netins_udp_set_opt()` 选项

| option | `value` 类型 | 角色 | 默认/约束 |
|---|---|---|---|
| `NETINS_UDP_OPT_INTERFACES` | `netins_udp_interfaces_t` | CLIENT | 1～2 个非空网卡名 |
| `NETINS_UDP_OPT_SCHEDULER` | `int`，值为 `netins_udp_scheduler_t` | CLIENT/SERVER | `DEFAULT` 表示不覆盖 mini 默认值 |
| `NETINS_UDP_OPT_CONGESTION_CONTROL` | `int`，值为 `netins_udp_congestion_control_t` | CLIENT/SERVER | default/CUBIC/BBR |
| `NETINS_UDP_OPT_TLS` | `netins_udp_tls_config_t` | CLIENT/SERVER | client 只用 SNI；server 必须设置证书和私钥 |
| `NETINS_UDP_OPT_CONNECT_TIMEOUT_MS` | `uint32_t` | CLIENT | 默认 10000 ms；不能为 0 |
| `NETINS_UDP_OPT_ACCEPT_TIMEOUT_MS` | `uint32_t` | SERVER | 默认 0，表示无限等待 |
| `NETINS_UDP_OPT_CLOSE_TIMEOUT_MS` | `uint32_t` | CLIENT/SERVER | 默认 3000 ms |
| `NETINS_UDP_OPT_KERNEL_SNDBUF` | 正 `int` | CLIENT/SERVER | UDP socket 发送缓冲区 |
| `NETINS_UDP_OPT_KERNEL_RCVBUF` | 正 `int` | CLIENT/SERVER | UDP socket 接收缓冲区 |
| `NETINS_UDP_OPT_LOGGER` | `netins_udp_logger_config_t` | 所有 handle | 唯一可在活动 handle 上替换的选项 |

`value_len` 必须传实际对象大小，例如：

```c
int cc = NETINS_UDP_CC_BBR;
if (netins_udp_set_opt(client, NETINS_UDP_OPT_CONGESTION_CONTROL,
        &cc, sizeof(cc)) != 0) {
    /* 处理错误 */
}
```

### 5.2 单路径和双路径

建议集成方总是显式设置接口，避免结果依赖设备默认路由：

```c
netins_udp_interfaces_t interfaces = NETINS_UDP_INTERFACES_INITIALIZER;

/* 单路径 */
interfaces.interface_count = 1;
snprintf(interfaces.names[0], sizeof(interfaces.names[0]), "%s",
    "rmnet_data0");

/* 双路径时再设置：
interfaces.interface_count = 2;
snprintf(interfaces.names[1], sizeof(interfaces.names[1]), "%s", "usb0");
*/

netins_udp_set_opt(client, NETINS_UDP_OPT_INTERFACES,
    &interfaces, sizeof(interfaces));
```

- 一个接口：目标为单路径；
- 两个接口：目标为双路径；
- 未设置接口：API 保留 mini 的自动多路径目标，实际活动路径由本机地址、路由和握手结果决定，
  不等于一定存在两条可用物理链路；
- 应以 `snapshot.active_path_count` 或 `netins_udp_is_multipath()` 判断运行时实际结果。

当前 RX 预算依据传输层活动路径数选择：少于 2 条活动路径使用单路径预算 40；达到 2 条
活动路径使用双路径每路径预算 256。DEBUG 日志中的 `[diag_rx_budget]`、
`[diag_path_map]` 和 `[diag_rx_path_state]` 可用于核对。

### 5.3 调度器与拥塞控制

调度器可选值：

```text
DEFAULT, MINRTT, BACKUP, BALANCED, RAP, ACT, BANDWIDTH,
FASTPROBE, ADAPTIVE, FASTPROBE5, FASTPROBE10,
FASTPROBE20, FASTONLY
```

拥塞控制可选值：

```text
NETINS_UDP_CC_DEFAULT
NETINS_UDP_CC_CUBIC
NETINS_UDP_CC_BBR
```

GET 的大流量发送端是服务端，POST 的大流量发送端是客户端。因此测试调度器和拥塞控制时，
要同时考虑数据发送端的配置，而不能只观察请求发起端。

### 5.4 TLS

服务端在 `listen()` 前必须设置证书和私钥：

```c
netins_udp_tls_config_t tls = NETINS_UDP_TLS_CONFIG_INITIALIZER;

snprintf(tls.certificate_file, sizeof(tls.certificate_file),
    "%s", "/root/test_xquic_server/server.crt");
snprintf(tls.private_key_file, sizeof(tls.private_key_file),
    "%s", "/root/test_xquic_server/server.key");

netins_udp_set_opt(server, NETINS_UDP_OPT_TLS, &tls, sizeof(tls));
```

客户端不支持客户端证书，`certificate_file` 和 `private_key_file` 必须留空；如需 SNI，只设置：

```c
netins_udp_tls_config_t tls = NETINS_UDP_TLS_CONFIG_INITIALIZER;
snprintf(tls.server_name, sizeof(tls.server_name), "%s", "124.70.219.194");
netins_udp_set_opt(client, NETINS_UDP_OPT_TLS, &tls, sizeof(tls));
```

### 5.5 日志

```c
static void
api_logger(void *user_data, int level, const char *message)
{
    FILE *out = user_data != NULL ? (FILE *)user_data : stderr;
    fprintf(out, "[mini-api:%d] %s", level, message);
}

netins_udp_logger_config_t logger = NETINS_UDP_LOGGER_CONFIG_INITIALIZER;
logger.callback = api_logger;
logger.user_data = stderr;
logger.minimum_level = NETINS_UDP_LOG_DEBUG;
netins_udp_set_opt(handle, NETINS_UDP_OPT_LOGGER, &logger, sizeof(logger));
```

日志回调是同步调用的：

- `message` 只在本次回调期间有效，如需长期保存必须复制；
- 不能从回调中重入同一个 handle；
- callback 为 `NULL` 时，API 驱动期间的 mini stdout 被抑制；
- `DEBUG=0`、`INFO=1`、`WARN=2`、`ERROR=3`。

## 6. 客户端：创建连接和收发

### 6.1 创建客户端并连接

```c
if (netins_udp_init() != 0) {
    perror("netins_udp_init");
    return 1;
}

netins_udp_t *client = netins_udp_new(NETINS_UDP_ROLE_CLIENT);
if (client == NULL) {
    perror("netins_udp_new");
    netins_udp_cleanup();
    return 1;
}

/* 在这里 set_opt；connect 后除 logger 外不能再修改。 */
if (netins_udp_connect(client, "124.70.219.194", 18446) != 0) {
    /* 打印 handle 错误，随后 close/cleanup */
}
```

`connect()` 返回 0 时，QUIC/TLS/HTTP3 握手已经完成，可以调用 `transfer()`。

### 6.2 GET 下载

```c
netins_udp_transfer_t task = NETINS_UDP_TRANSFER_INITIALIZER;
netins_udp_transfer_result_t result =
    NETINS_UDP_TRANSFER_RESULT_INITIALIZER;

task.direction = NETINS_UDP_TRANSFER_GET;
task.remote_path = "/upload/1";            /* 服务端源文件 */
task.local_path = "./get-agg-200m.bin";    /* 客户端输出文件 */
task.timeout_ms = 600000;                  /* 0 表示无限等待 */

if (netins_udp_transfer(client, &task, &result) == 0) {
    printf("GET bytes=%llu elapsed_us=%llu speed=%.3f Mbps\n",
        (unsigned long long)result.total_bytes,
        (unsigned long long)result.elapsed_us,
        result.throughput_mbps);
}
```

GET 会先获得远端文件总长度，再按 4 MiB 分片，最多保持 8 个并发 HTTP/3 request。
成功返回前会等待所有分片、异步本地写入和 `fsync()` 完成。

### 6.3 POST 上传

```c
netins_udp_transfer_t task = NETINS_UDP_TRANSFER_INITIALIZER;
netins_udp_transfer_result_t result =
    NETINS_UDP_TRANSFER_RESULT_INITIALIZER;

task.direction = NETINS_UDP_TRANSFER_POST;
task.local_path = "./download/kkk.txt";    /* 客户端源文件 */
task.remote_path = "/download/kkk.txt";    /* 服务端目标文件 */
task.timeout_ms = 600000;

if (netins_udp_transfer(client, &task, &result) == 0) {
    printf("POST bytes=%llu elapsed_us=%llu speed=%.3f Mbps\n",
        (unsigned long long)result.total_bytes,
        (unsigned long long)result.elapsed_us,
        result.throughput_mbps);
}
```

POST 按 4 MiB 分片，最多保持 4 个并发 HTTP/3 request。服务端目标父目录必须预先创建。

### 6.4 同一连接执行多个任务

同一 client handle 可以顺序调用多次 `netins_udp_transfer()`：

```c
run_get(client, "/upload/1", "./first.bin");
run_post(client, "./download/kkk.txt", "/download/kkk.txt");
run_get(client, "/upload/2", "./second.bin");
```

不能在多个线程上同时对同一连接调用 transfer，也不能在一次 transfer 尚未返回时启动下一次。

`result.elapsed_us` 从文件任务开始计到任务完成，不包含此前的 `connect()` 握手，也不包含之后
的 `shutdown()` 和 `close()`；用于计算纯文件任务吞吐比 `/usr/bin/time` 更合适。
`result.throughput_mbps` 的计算为：

```text
total_bytes × 8 / elapsed_us
```

数值单位正好是 Mbit/s（按十进制 10^6）。

## 7. 服务端：监听、接收和处理连接

### 7.1 创建 listener

```c
netins_udp_init();
netins_udp_t *server = netins_udp_new(NETINS_UDP_ROLE_SERVER);

netins_udp_tls_config_t tls = NETINS_UDP_TLS_CONFIG_INITIALIZER;
snprintf(tls.certificate_file, sizeof(tls.certificate_file),
    "%s", "/root/test_xquic_server/server.crt");
snprintf(tls.private_key_file, sizeof(tls.private_key_file),
    "%s", "/root/test_xquic_server/server.key");
netins_udp_set_opt(server, NETINS_UDP_OPT_TLS, &tls, sizeof(tls));

if (netins_udp_listen(server, "0.0.0.0", 18446) != 0) {
    /* 处理错误 */
}
```

服务端证书和私钥是当前 QUIC/TLS 握手的必需项，不能省略。

### 7.2 accept 和 serve

```c
netins_udp_t *connection = netins_udp_accept(server);
if (connection != NULL) {
    netins_udp_serve_config_t config =
        NETINS_UDP_SERVE_CONFIG_INITIALIZER;
    config.root_path = "/root/test_xquic_server";
    config.timeout_ms = 600000;

    (void)netins_udp_serve(connection, &config);
    (void)netins_udp_shutdown(connection);
    netins_udp_close(connection);
}
```

`serve()` 同时支持客户端 GET 和 POST，不需要服务端再调用单独的 send/recv：

- GET：从 `root_path` 下读取文件并响应；
- POST：在 `root_path` 下创建/截断目标并按偏移写入；
- 返回 0：对端正常关闭，accepted handle 已进入 `CLOSED`；
- 返回 -1：超时或连接错误，读取错误信息后关闭 accepted handle。

当前公开 API 的 listener 处理模型是串行的：`accept()` 一个连接后调用阻塞 `serve()`，
该连接结束后再接受下一个连接。若上层需要并发服务多个客户端，必须为共享 server backend
设计串行事件泵/调度；不能简单让多个线程同时调用同一 listener 及其 accepted handle。

## 8. 完整客户端示例

下面示例建立双路径连接，先 GET，再在同一连接上 POST，最后查询快照并关闭。
生产程序应根据实际业务决定执行哪个任务。

```c
#include <xquic/netins_udp.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>

static void
print_error(netins_udp_t *handle, const char *operation)
{
    char message[NETINS_UDP_ERROR_MSG_LEN] = {0};
    int code = errno;

    if (handle != NULL) {
        code = netins_udp_get_last_error_code(handle);
        (void)netins_udp_get_last_error_msg(handle, message, sizeof(message));
    }
    fprintf(stderr, "%s failed: code=%d message=%s\n",
        operation, code, message[0] != '\0' ? message : "unavailable");
}

static int
run_transfer(netins_udp_t *client, int direction,
    const char *local_path, const char *remote_path)
{
    netins_udp_transfer_t task = NETINS_UDP_TRANSFER_INITIALIZER;
    netins_udp_transfer_result_t result =
        NETINS_UDP_TRANSFER_RESULT_INITIALIZER;

    task.direction = direction;
    task.local_path = local_path;
    task.remote_path = remote_path;
    task.timeout_ms = 600000;

    if (netins_udp_transfer(client, &task, &result) != 0) {
        print_error(client, "netins_udp_transfer");
        return -1;
    }
    printf("%s bytes=%llu fragments=%u/%u elapsed_us=%llu"
           " throughput=%.3f Mbps\n",
        direction == NETINS_UDP_TRANSFER_GET ? "GET" : "POST",
        (unsigned long long)result.total_bytes,
        result.completed_fragments, result.total_fragments,
        (unsigned long long)result.elapsed_us,
        result.throughput_mbps);
    return 0;
}

int
main(void)
{
    netins_udp_t *client = NULL;
    int initialized = 0;
    int exit_code = 1;

    if (netins_udp_init() != 0) {
        perror("netins_udp_init");
        goto done;
    }
    initialized = 1;

    if (netins_udp_get_api_version() != NETINS_UDP_API_VERSION) {
        fprintf(stderr, "Mini API version mismatch\n");
        goto done;
    }

    client = netins_udp_new(NETINS_UDP_ROLE_CLIENT);
    if (client == NULL) {
        perror("netins_udp_new");
        goto done;
    }

    netins_udp_interfaces_t interfaces =
        NETINS_UDP_INTERFACES_INITIALIZER;
    interfaces.interface_count = 2;
    snprintf(interfaces.names[0], sizeof(interfaces.names[0]),
        "%s", "rmnet_data0");
    snprintf(interfaces.names[1], sizeof(interfaces.names[1]),
        "%s", "usb0");
    if (netins_udp_set_opt(client, NETINS_UDP_OPT_INTERFACES,
            &interfaces, sizeof(interfaces)) != 0) {
        print_error(client, "set interfaces");
        goto done;
    }

    int scheduler = NETINS_UDP_SCHEDULER_BALANCED;
    int cc = NETINS_UDP_CC_BBR;
    uint32_t connect_timeout_ms = 30000;
    if (netins_udp_set_opt(client, NETINS_UDP_OPT_SCHEDULER,
            &scheduler, sizeof(scheduler)) != 0
        || netins_udp_set_opt(client, NETINS_UDP_OPT_CONGESTION_CONTROL,
            &cc, sizeof(cc)) != 0
        || netins_udp_set_opt(client, NETINS_UDP_OPT_CONNECT_TIMEOUT_MS,
            &connect_timeout_ms, sizeof(connect_timeout_ms)) != 0) {
        print_error(client, "configure client");
        goto done;
    }

    netins_udp_tls_config_t tls = NETINS_UDP_TLS_CONFIG_INITIALIZER;
    snprintf(tls.server_name, sizeof(tls.server_name),
        "%s", "124.70.219.194");
    if (netins_udp_set_opt(client, NETINS_UDP_OPT_TLS,
            &tls, sizeof(tls)) != 0) {
        print_error(client, "set client TLS");
        goto done;
    }

    if (netins_udp_connect(client, "124.70.219.194", 18446) != 0) {
        print_error(client, "netins_udp_connect");
        goto done;
    }

    if (run_transfer(client, NETINS_UDP_TRANSFER_GET,
            "./get-agg-200m.bin", "/upload/1") != 0) {
        goto done;
    }
    if (run_transfer(client, NETINS_UDP_TRANSFER_POST,
            "./download/kkk.txt", "/download/kkk.txt") != 0) {
        goto done;
    }

    netins_udp_snapshot_t snapshot = NETINS_UDP_SNAPSHOT_INITIALIZER;
    if (netins_udp_get_snapshot(client, &snapshot) == 0) {
        printf("state=%d handshake=%d paths=%u active=%u multipath=%d\n",
            snapshot.state, snapshot.handshake_finished,
            snapshot.path_count, snapshot.active_path_count,
            netins_udp_is_multipath(client));
    }
    exit_code = 0;

done:
    if (client != NULL) {
        if (netins_udp_shutdown(client) != 0 && exit_code == 0) {
            print_error(client, "netins_udp_shutdown");
            exit_code = 1;
        }
        netins_udp_close(client);
    }
    if (initialized && netins_udp_cleanup() != 0) {
        perror("netins_udp_cleanup");
        exit_code = 1;
    }
    return exit_code;
}
```

## 9. 完整服务端示例

下面示例在 `0.0.0.0:18446` 监听，顺序处理 3 个客户端连接：

```c
#include <xquic/netins_udp.h>

#include <errno.h>
#include <stdio.h>

static void
print_error(netins_udp_t *handle, const char *operation)
{
    char message[NETINS_UDP_ERROR_MSG_LEN] = {0};
    int code = errno;

    if (handle != NULL) {
        code = netins_udp_get_last_error_code(handle);
        (void)netins_udp_get_last_error_msg(handle, message, sizeof(message));
    }
    fprintf(stderr, "%s failed: code=%d message=%s\n",
        operation, code, message[0] != '\0' ? message : "unavailable");
}

int
main(void)
{
    netins_udp_t *server = NULL;
    int initialized = 0;
    int exit_code = 1;

    if (netins_udp_init() != 0) {
        perror("netins_udp_init");
        goto done;
    }
    initialized = 1;

    server = netins_udp_new(NETINS_UDP_ROLE_SERVER);
    if (server == NULL) {
        perror("netins_udp_new");
        goto done;
    }

    netins_udp_tls_config_t tls = NETINS_UDP_TLS_CONFIG_INITIALIZER;
    snprintf(tls.certificate_file, sizeof(tls.certificate_file),
        "%s", "/root/test_xquic_server/server.crt");
    snprintf(tls.private_key_file, sizeof(tls.private_key_file),
        "%s", "/root/test_xquic_server/server.key");
    if (netins_udp_set_opt(server, NETINS_UDP_OPT_TLS,
            &tls, sizeof(tls)) != 0) {
        print_error(server, "set server TLS");
        goto done;
    }

    uint32_t accept_timeout_ms = 0; /* 无限等待 */
    if (netins_udp_set_opt(server, NETINS_UDP_OPT_ACCEPT_TIMEOUT_MS,
            &accept_timeout_ms, sizeof(accept_timeout_ms)) != 0) {
        print_error(server, "set accept timeout");
        goto done;
    }

    if (netins_udp_listen(server, "0.0.0.0", 18446) != 0) {
        print_error(server, "netins_udp_listen");
        goto done;
    }
    printf("listening on 0.0.0.0:18446\n");

    for (unsigned int i = 0; i < 3; ++i) {
        netins_udp_t *connection = netins_udp_accept(server);
        if (connection == NULL) {
            print_error(server, "netins_udp_accept");
            goto done;
        }

        netins_udp_serve_config_t serve =
            NETINS_UDP_SERVE_CONFIG_INITIALIZER;
        serve.root_path = "/root/test_xquic_server";
        serve.timeout_ms = 600000;

        if (netins_udp_serve(connection, &serve) != 0) {
            print_error(connection, "netins_udp_serve");
            (void)netins_udp_shutdown(connection);
            netins_udp_close(connection);
            goto done;
        }

        (void)netins_udp_shutdown(connection);
        netins_udp_close(connection);
        printf("served connection %u/3\n", i + 1);
    }
    exit_code = 0;

done:
    if (server != NULL) {
        (void)netins_udp_shutdown(server);
        netins_udp_close(server);
    }
    if (initialized && netins_udp_cleanup() != 0) {
        perror("netins_udp_cleanup");
        exit_code = 1;
    }
    return exit_code;
}
```

## 10. 快照和路径统计

推荐优先使用一次性的 `netins_udp_get_snapshot()`：

```c
netins_udp_snapshot_t snapshot = NETINS_UDP_SNAPSHOT_INITIALIZER;

if (netins_udp_get_snapshot(client, &snapshot) == 0) {
    printf("paths=%u active=%u srtt=%llu inflight=%llu app=%llu\n",
        snapshot.path_count,
        snapshot.active_path_count,
        (unsigned long long)snapshot.srtt_us,
        (unsigned long long)snapshot.bytes_inflight,
        (unsigned long long)snapshot.total_app_bytes);

    for (uint32_t i = 0; i < snapshot.path_count; ++i) {
        const netins_udp_path_statis_t *p = &snapshot.paths[i];
        printf("path[%u] id=%llu iface=%s state=%d app=%d"
               " tx=%llu rx=%llu srtt=%llu\n",
            i, (unsigned long long)p->path_id, p->interface_name,
            p->path_state, p->app_status,
            (unsigned long long)p->bytes_sent,
            (unsigned long long)p->bytes_received,
            (unsigned long long)p->srtt_us);
    }
}
```

关键字段：

| 字段 | 含义 |
|---|---|
| `handshake_finished` | QUIC/TLS 握手是否完成 |
| `multipath_mode` | 连接是否开启多路径能力 |
| `active_path_count` | 传输层当前活动路径数 |
| `packets_sent/received/lost` | 连接级包统计 |
| `srtt_us/min_rtt_us` | 连接级 RTT |
| `bytes_inflight` | 连接级在途字节 |
| `total_app_bytes` | 连接应用数据累计值 |
| `paths[]` | 每路径接口、地址、包、字节和 SRTT |

当前 XQUIC 公共统计 ABI 不提供每路径 CWND 和 inflight，因此 API v1 的
`path.cwnd_bytes`、`path.bytes_inflight` 固定为 0；连接级
`snapshot.bytes_inflight` 可正常使用。

如果只查询一条路径，可用独立输出结构：

```c
uint32_t path_count = 0;
if (netins_udp_get_path_count(client, &path_count) == 0
    && path_count > 0) {
    netins_udp_path_statis_t path =
        NETINS_UDP_PATH_STATIS_INITIALIZER;
    if (netins_udp_get_path_statis(client, 0, &path) == 0) {
        printf("path0 interface=%s bytes_received=%llu\n",
            path.interface_name,
            (unsigned long long)path.bytes_received);
    }
}
```

## 11. 正确关闭和异常清理

推荐统一使用如下清理顺序：

```c
if (handle != NULL) {
    (void)netins_udp_shutdown(handle);
    netins_udp_close(handle);
    handle = NULL;
}
if (initialized) {
    (void)netins_udp_cleanup();
}
```

规则：

- `shutdown()` 负责协议关闭，可以返回错误；
- `close()` 负责最终释放，返回 `void`，即使未先 shutdown 也会自行尝试；
- 读取错误、快照或关闭原因必须在 `close()` 前完成；
- `close()` 后 handle 立即失效，不能再查询状态；
- listener 关闭不会立即使已接受的 accepted handle 失效，共享 backend 会保持到最后一个
  accepted handle 被释放；
- 最后一次 `cleanup()` 前必须释放所有 client、server 和 accepted handle；
- 每次成功的 `init()` 都要有一次 `cleanup()` 配对。

需要单独判断关闭来源时：

```c
netins_udp_close_reason_t reason =
    NETINS_UDP_CLOSE_REASON_INITIALIZER;

if (netins_udp_get_close_reason(handle, &reason) == 0) {
    printf("source=%d system=%d transport=%d message=%s\n",
        reason.source, reason.system_error,
        reason.transport_error, reason.message);
}
```

`reason.source` 可能是 `NONE`、`LOCAL`、`PEER`、`TRANSPORT`、`TIMEOUT` 或
`INTERNAL`。该查询同样必须发生在 `netins_udp_close()` 之前。

## 12. 线程、阻塞和超时

- 单个 handle 不是并发安全的，调用方必须串行调用；
- listener 及共享其 backend 的 accepted handle 也必须串行访问；
- logger 回调不能重入当前 handle；
- `connect()`、`accept()`、`transfer()`、`serve()` 和 `shutdown()` 都可能阻塞；
- 在 UI 或业务主线程中集成时，建议由一个专用工作线程独占整个 handle 生命周期；
- 不要用另一个线程并发调用 `shutdown()` 来中断正在运行的 `transfer()`；
- transfer/serve 超时为 0 表示无限等待；
- connect timeout 不能为 0；accept timeout 为 0 表示无限等待。

API 内部事件循环使用阻塞 `EVLOOP_ONCE` 驱动，不再用固定 1 ms 轮询；显式状态查询只进行
一次非阻塞事件泵。

## 13. 固定传输语义

为保持与当前 mini 数据面一致，下列参数固定在库内部，API v1 不开放修改：

- 文件分片：4 MiB；
- GET 并发 request：8；
- POST 并发 request：4；
- GET 先通过响应头获取总文件长度；
- 使用 offset `pread()`/`pwrite()`；
- 完成一个 request 后自动补充下一个分片；
- GET 返回前等待异步写队列和 `fsync()`；
- 0 字节文件合法；
- 任务超时会关闭/终止连接，避免迟到 request 继续写文件。

这里的“8/4”是应用层 HTTP/3 request 并发数，“40/256”是 UDP RX 每轮处理预算，两者不是
同一个限制。

## 14. 编译和链接

API 实现编入 `libxquic.so` 和 `libxquic-static.a`，不是单独的
`libnetins_udp.a`。构建时必须启用：

```bash
-DXQC_ENABLE_MINI_API=ON
```

使用交付的共享库时，一个最小编译命令形态为：

```bash
cc -O2 -I/path/to/package/include client.c \
  -L/path/to/package/lib -lxquic \
  -Wl,-rpath,/path/to/package/lib \
  -o mini_api_client_app
```

运行时确保 `libxquic.so` 及其动态依赖可被加载。也可使用：

```bash
export LD_LIBRARY_PATH=/path/to/package/lib:$LD_LIBRARY_PATH
```

静态链接还需要按目标架构链接 Tongsuo/BabaSSL、Libevent、zlog、pthread、dl 和 math，
依赖顺序与路径容易因平台不同而变化，建议直接复用工程 CMake target，不要只在 gcc 命令
末尾添加 `libxquic-static.a`。AARCH64 客户端和 Linux x86_64 服务端的完整构建参数见
[`NETINS_UDP_编译指南.md`](NETINS_UDP_编译指南.md)。

## 15. 随附 demo 与命令行对应关系

仓库中的参考源码：

- [`demo/netins_udp_mini_api_client.c`](demo/netins_udp_mini_api_client.c)
- [`demo/netins_udp_mini_api_server.c`](demo/netins_udp_mini_api_server.c)

构建后得到：

```text
build/mini/netins_udp_mini_api_client
build/mini/netins_udp_mini_api_server
```

命令行参数和 C API 的对应关系：

| demo 参数 | C API |
|---|---|
| `--host`、`--port` | `netins_udp_connect()` |
| `--interface` | `NETINS_UDP_OPT_INTERFACES` |
| `--server-name` | client `NETINS_UDP_OPT_TLS` |
| `--scheduler` | `NETINS_UDP_OPT_SCHEDULER` |
| `--cc` | `NETINS_UDP_OPT_CONGESTION_CONTROL` |
| `--get`/`--post` | `task.direction` |
| `--local` | `task.local_path` |
| `--remote` | `task.remote_path` |
| `--timeout-ms` | `task.timeout_ms`，demo 同时用于 connect |
| server `--cert`/`--key` | server `NETINS_UDP_OPT_TLS` |
| server `--root` | `serve.root_path` |
| server `--accept-timeout-ms` | `NETINS_UDP_OPT_ACCEPT_TIMEOUT_MS` |
| server `--serve-timeout-ms` | `serve.timeout_ms` |

## 16. 常见错误

### `netins_udp_new()` 返回 NULL，`errno=EPERM`

没有先成功调用 `netins_udp_init()`。

### `set_opt()` 返回 `EBUSY`

除 logger 外，选项必须在 `connect()` 或 `listen()` 前设置。

### `connect()` 返回 `EINVAL`

检查 client 角色、当前状态、端口和 host。host 当前必须是数值 IPv4，不能直接传域名。

### `listen()` 提示必须配置 TLS

服务端必须在 listen 前通过 `NETINS_UDP_OPT_TLS` 设置可读的证书和私钥路径。

### GET/POST 路径无效

`remote_path` 必须类似 `/upload/1`，不能是相对路径、单独 `/`、含 `..` 或反斜杠的路径。
POST 目标父目录必须在服务端 `root_path` 内预先存在。

### 配置了两个接口但 `is_multipath()` 返回 0

两个接口只是配置目标。还需检查接口是否存在、地址和路由是否有效、两条路径是否通过验证。
以 `snapshot.active_path_count` 和 DEBUG 路径日志为准。

### `cleanup()` 返回 `EBUSY`

仍有 client、server 或 accepted handle 未 `netins_udp_close()`。

### 传输失败后能否继续复用连接

参数校验类错误且连接仍为 `CONNECTED` 时可修正参数后重试；超时、协议错误、transport close
会把状态置为 `ERROR`，此时应关闭并重新创建连接。
