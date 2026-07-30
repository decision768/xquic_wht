# NetIns UDP API 使用说明

## 概述

NetIns UDP 是基于 XQUIC 封装的 QUIC/多路径 QUIC 连接库。第三方程序通过
统一的 C API 完成：

- 初始化和释放运行环境；
- 创建客户端或服务端对象；
- 建立、监听和接受连接；
- 可靠、有序地发送和接收数据；
- 正常关闭连接；
- 查询连接状态、多路径状态和逐路径统计；
- 查询基础错误码。

调用方只需要包含公共头文件：

```c
#include <xquic/netins_udp.h>
```

常用调用链：

```text
客户端：init → new → set_opt → connect → send/recv → shutdown → close → cleanup
服务端：init → new → set_opt → listen → accept → recv/send → shutdown → close → cleanup
```

---

## 1. 返回值和错误处理

普通操作函数：

- 成功：返回 `NETINS_UDP_OK`，值为 0；
- 失败：返回 `NETINS_UDP_ERROR`，值为 -1。

创建或接受对象的函数：

- 成功：返回对象指针；
- 失败：返回 `NULL`。

数据收发函数：

- 成功：返回实际收发字节数，可能小于请求长度；
- `recv()` 返回 0：对端已经正常结束发送；
- 失败：返回 -1。

失败后通过以下接口查询详细错误：

```c
int netins_udp_get_last_error_code(netins_udp_t *netins);

int netins_udp_get_last_error_msg(netins_udp_t *netins,
    char *buffer, size_t buffer_len);
```

错误码采用 `errno` 风格，例如：

- `EINVAL`：参数错误；
- `ENOTCONN`：连接尚未建立；
- `EAGAIN`：非阻塞模式下暂时不可操作；
- `ETIMEDOUT`：操作超时；
- `ECONNRESET`：连接被对端重置；
- `EOPNOTSUPP`：当前配置不受支持。

使用示例：

```c
char error_message[NETINS_UDP_ERROR_MSG_LEN] = {0};

if (netins_udp_connect(conn, host, port) != NETINS_UDP_OK) {
    netins_udp_get_last_error_msg(conn, error_message,
        sizeof(error_message));
    fprintf(stderr, "connect failed: code=%d, message=%s\n",
        netins_udp_get_last_error_code(conn), error_message);
}
```

---

## 2. 初始化和释放

### `netins_udp_init()`

初始化 NetIns UDP 运行环境。

```c
int netins_udp_init(void);
```

程序使用其他 NetIns UDP API 前必须先调用该函数。

### `netins_udp_cleanup()`

释放 NetIns UDP 运行环境。

```c
int netins_udp_cleanup(void);
```

调用前必须关闭所有 client、listener 和 accepted connection。若仍有对象未释放，
函数返回 `NETINS_UDP_ERROR`，错误码为 `EBUSY`。

### `netins_udp_get_api_version()`

查询运行库的 API 版本。

```c
uint32_t netins_udp_get_api_version(void);
```

建议程序启动时检查头文件与库版本是否匹配：

```c
if (netins_udp_get_api_version() != NETINS_UDP_API_VERSION) {
    fprintf(stderr, "NetIns UDP API version mismatch\n");
    return -1;
}
```

---

## 3. 创建和释放连接对象

### `netins_udp_new()`

创建一个尚未连接的对象。

```c
netins_udp_t *netins_udp_new(int type);
```

`type` 可取：

| 类型 | 说明 |
|---|---|
| `NETINS_UDP_TYPE_QUIC` | 普通单路径 QUIC |
| `NETINS_UDP_TYPE_MPQUIC` | 请求启用多路径 QUIC |

返回值：

- 成功：返回连接对象；
- 失败：返回 `NULL`，并设置 `errno`。

`NETINS_UDP_TYPE_MPQUIC` 表示请求多路径，不代表连接建立后一定有两条活动路径。
实际状态必须在建连后调用 `netins_udp_is_multipath()` 或
`netins_udp_get_info()` 查询。

### `netins_udp_close()`

关闭并释放对象。

```c
void netins_udp_close(netins_udp_t *netins);
```

调用后对象指针立即失效，不得再次使用或重复关闭。
`netins_udp_close(NULL)` 可以安全返回。

---

## 4. 常用连接选项

连接选项通过以下函数设置：

```c
int netins_udp_set_opt(netins_udp_t *netins, int option,
    const void *value, size_t value_len);
```

除特别说明外，应在 `connect()` 或 `listen()` 之前设置。

### 4.1 配置客户端多路径网卡

`netins_udp_interfaces_t` 是 NetIns UDP 定义的客户端网卡配置结构体，用来
指定建立连接时可用于创建路径的本地网络接口。它不是连接对象，也不表示
连接建立后的实际路径状态。

公共结构定义如下：

```c
typedef struct netins_udp_interfaces_s {
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t interface_count;
    uint32_t reserved32;
    char names[NETINS_UDP_MAX_PATHS]
              [NETINS_UDP_INTERFACE_NAME_LEN];
    uint64_t reserved[4];
} netins_udp_interfaces_t;
```

字段说明：

| 字段 | 是否由调用方填写 | 说明 |
|---|---:|---|
| `struct_size` | 否 | 结构体大小，由初始化宏填写 |
| `api_version` | 否 | 公共 API 版本，由初始化宏填写 |
| `interface_count` | 是 | `names[]` 中有效的网卡名称数量 |
| `reserved32` | 否 | 预留字段，必须保持为 0 |
| `names[]` | 是 | Linux 本地网卡名称，按期望的路径顺序填写 |
| `reserved[]` | 否 | 预留字段，必须保持为 0 |

当前版本最多配置 `NETINS_UDP_MAX_PATHS`（8）个接口；每个接口名称的存储
空间为 `NETINS_UDP_INTERFACE_NAME_LEN`（32）字节，其中包含字符串末尾的
空字符。因此，有效名称最长为 31 个字符。

必须使用 `NETINS_UDP_INTERFACES_INITIALIZER` 初始化结构体。初始化宏会填写
`struct_size` 和 `api_version`，并将接口数量和预留字段初始化为 0：

```c
netins_udp_interfaces_t interfaces =
    NETINS_UDP_INTERFACES_INITIALIZER;

interfaces.interface_count = 2;
snprintf(interfaces.names[0], sizeof(interfaces.names[0]),
    "%s", "rmnet_data0");
snprintf(interfaces.names[1], sizeof(interfaces.names[1]),
    "%s", "usb0");

if (netins_udp_set_opt(conn, NETINS_UDP_OPT_INTERFACES,
        &interfaces, sizeof(interfaces)) != NETINS_UDP_OK)
{
    /* 处理错误 */
}
```

`netins_udp_set_opt()` 会复制整个结构体和其中的网卡名称。函数成功返回后，
调用方可以立即复用或释放原变量。

接口顺序采用创建时配置：

- path0：`rmnet_data0`；
- path1：`usb0`。

使用规则：

- 该选项只适用于客户端，必须在 `netins_udp_connect()` 前设置；
- listener 不接受该选项，设置时返回 `NETINS_UDP_ERROR`，错误码为
  `EOPNOTSUPP`；
- `interface_count` 不能超过 8，每个有效的 `names[i]` 都不能为空；
- `interface_count=0` 或未设置该选项时，客户端使用系统默认路由；
- 网卡名称只是指定客户端尝试创建路径时使用的接口，不保证对应路径一定建立
  成功；
- 连接建立后应调用 `netins_udp_is_multipath()`、
  `netins_udp_get_path_count()` 和 `netins_udp_get_path_statis()` 确认实际
  多路径状态。

### 4.2 配置客户端证书校验

```c
netins_udp_tls_config_t tls =
    NETINS_UDP_TLS_CONFIG_INITIALIZER;

snprintf(tls.server_name, sizeof(tls.server_name),
    "%s", "server.example.com");
snprintf(tls.ca_file, sizeof(tls.ca_file),
    "%s", "/opt/netins/certs/ca.pem");

if (netins_udp_set_opt(conn, NETINS_UDP_OPT_TLS,
        &tls, sizeof(tls)) != NETINS_UDP_OK)
{
    /* 处理错误 */
}
```

客户端默认校验服务端证书。`ca_file` 为空时使用系统信任库。
生产环境不建议关闭证书校验。

### 4.3 配置服务端证书

listener 在 `listen()` 前必须配置证书和私钥：

```c
netins_udp_tls_config_t tls =
    NETINS_UDP_TLS_CONFIG_INITIALIZER;

snprintf(tls.certificate_file, sizeof(tls.certificate_file),
    "%s", "/opt/netins/certs/server.crt");
snprintf(tls.private_key_file, sizeof(tls.private_key_file),
    "%s", "/opt/netins/certs/server.key");

if (netins_udp_set_opt(listener, NETINS_UDP_OPT_TLS,
        &tls, sizeof(tls)) != NETINS_UDP_OK)
{
    /* 处理错误 */
}
```

### 4.4 阻塞方式和超时

常用选项：

| 选项 | 值类型 | 默认值 | 说明 |
|---|---:|---:|---|
| `NETINS_UDP_OPT_NONBLOCK` | `int` | 0 | 0 阻塞，1 非阻塞 |
| `NETINS_UDP_OPT_CONNECT_TIMEOUT_MS` | `uint32_t` | 20000 | 建连超时 |
| `NETINS_UDP_OPT_ACCEPT_TIMEOUT_MS` | `uint32_t` | 0 | accept 超时；0 表示一直等待 |
| `NETINS_UDP_OPT_SEND_TIMEOUT_MS` | `uint32_t` | 0 | 发送超时 |
| `NETINS_UDP_OPT_RECV_TIMEOUT_MS` | `uint32_t` | 0 | 接收超时 |
| `NETINS_UDP_OPT_CLOSE_TIMEOUT_MS` | `uint32_t` | 1000 | 关闭等待时间 |
| `NETINS_UDP_OPT_IDLE_TIMEOUT_MS` | `uint32_t` | 0 | 连接空闲超时 |

示例：

```c
uint32_t timeout_ms = 30000;

netins_udp_set_opt(conn, NETINS_UDP_OPT_SEND_TIMEOUT_MS,
    &timeout_ms, sizeof(timeout_ms));
netins_udp_set_opt(conn, NETINS_UDP_OPT_RECV_TIMEOUT_MS,
    &timeout_ms, sizeof(timeout_ms));
```

阻塞方式和收发超时在首次建立数据通道时采用创建时快照，因此应在第一次
`send()` 或 `recv()` 之前完成配置。

---

## 5. 客户端建立连接

### `netins_udp_connect()`

连接指定服务器。

```c
int netins_udp_connect(netins_udp_t *netins,
    const char *host, int port);
```

参数：

- `netins`：通过 `netins_udp_new()` 创建的对象；
- `host`：服务器 IP 地址或域名；
- `port`：服务器端口，范围为 1～65535。

阻塞模式下，函数在域名解析、QUIC 握手和连接初始化完成后返回。

使用示例：

```c
netins_udp_t *conn =
    netins_udp_new(NETINS_UDP_TYPE_MPQUIC);
if (conn == NULL) {
    /* 处理错误 */
}

if (netins_udp_connect(conn, "192.0.2.10", 8443)
    != NETINS_UDP_OK)
{
    /* 查询错误并关闭对象 */
}
```

连接成功后可通过以下方式确认状态：

```c
if (netins_udp_get_state(conn) == NETINS_UDP_STATE_CONNECTED) {
    printf("connected\n");
}
```

---

## 6. 服务端监听和接受连接

### `netins_udp_listen()`

开始监听。

```c
netins_udp_t *netins_udp_listen(netins_udp_t *netins,
    const char *host, int port);
```

参数：

- `netins`：通过 `netins_udp_new()` 创建的 listener 对象；
- `host`：监听地址；传 `NULL` 或空字符串表示监听所有地址；
- `port`：监听端口。

返回值：

- 成功：返回输入的 listener 对象；
- 失败：返回 `NULL`。输入对象仍可用于查询错误和关闭。

### `netins_udp_accept()`

接受一个客户端连接。

```c
netins_udp_t *netins_udp_accept(netins_udp_t *listener,
    struct sockaddr *addr, socklen_t *addrlen);
```

参数：

- `listener`：监听对象；
- `addr`、`addrlen`：可选，用于返回客户端地址；不需要时传 `NULL`。

返回值：

- 成功：返回一个新的 accepted connection；
- 失败：返回 `NULL`。

accepted connection 是独立对象，必须单独调用 `netins_udp_close()`。
关闭 listener 不会使已经接受的连接立即失效。

使用示例：

```c
netins_udp_t *listener =
    netins_udp_new(NETINS_UDP_TYPE_MPQUIC);
if (listener == NULL) {
    /* 处理错误 */
}

/* 先设置服务端证书 */

if (netins_udp_listen(listener, NULL, 8443) == NULL) {
    /* 查询错误并关闭 listener */
}

netins_udp_t *connection =
    netins_udp_accept(listener, NULL, NULL);
if (connection == NULL) {
    /* 查询 listener 错误 */
}
```

---

## 7. 数据发送和接收

NetIns UDP 的连接级数据接口提供可靠、有序的字节数据传输。调用方不需要处理
底层 UDP 数据包，也不需要理解内部协议对象。

### `netins_udp_send()`

发送数据。

```c
ssize_t netins_udp_send(netins_udp_t *netins,
    const void *data, size_t length, int flags);
```

返回值：

- 大于 0：实际接收并排队的字节数；
- 0：本次没有数据；
- -1：发送失败。

一次调用可能只接收部分数据，调用方必须根据返回值循环发送。

常用 `flags`：

| 标志 | 说明 |
|---|---|
| `NETINS_UDP_MSG_NONE` | 普通发送 |
| `NETINS_UDP_MSG_DONTWAIT` | 本次调用使用非阻塞方式 |
| `NETINS_UDP_MSG_FIN` | 当前数据发送完成后结束本端发送方向 |

`NETINS_UDP_MSG_FIN` 不等于释放连接。发送 FIN 后仍然可以继续接收对端数据。

完整发送示例：

```c
static int
send_all(netins_udp_t *conn, const void *data, size_t length, int finish)
{
    const unsigned char *bytes = data;
    size_t offset = 0;

    while (offset < length) {
        int flags = finish ? NETINS_UDP_MSG_FIN
                           : NETINS_UDP_MSG_NONE;
        ssize_t sent = netins_udp_send(conn, bytes + offset,
            length - offset, flags);
        if (sent < 0) {
            return -1;
        }
        if (sent == 0) {
            return -1;
        }
        offset += (size_t)sent;
    }

    if (finish && length == 0) {
        return netins_udp_send(conn, NULL, 0,
            NETINS_UDP_MSG_FIN) == 0 ? 0 : -1;
    }
    return 0;
}
```

### `netins_udp_recv()`

接收数据。

```c
ssize_t netins_udp_recv(netins_udp_t *netins,
    void *data, size_t length, int flags);
```

返回值：

- 大于 0：实际接收字节数；
- 0：对端已经正常结束发送；
- -1：接收失败。

常用 `flags`：

| 标志 | 说明 |
|---|---|
| `NETINS_UDP_MSG_NONE` | 普通接收 |
| `NETINS_UDP_MSG_DONTWAIT` | 本次调用使用非阻塞方式 |
| `NETINS_UDP_MSG_PEEK` | 查看数据但不从接收队列移除 |

接收示例：

```c
unsigned char buffer[64 * 1024];

for (;;) {
    ssize_t received = netins_udp_recv(conn, buffer,
        sizeof(buffer), NETINS_UDP_MSG_NONE);
    if (received > 0) {
        /* 处理 buffer[0..received-1] */
        continue;
    }
    if (received == 0) {
        /* 对端正常结束发送 */
        break;
    }
    /* 查询并处理错误 */
    break;
}
```

### 数据边界和线程

- 数据按发送顺序可靠交付；
- 不保留每次 `send()` 的调用边界；
- 一个发送线程和一个接收线程可以同时使用同一连接；
- `send()` 返回正数后，调用方可以立即复用原输入缓冲区；
- listener 不能调用 `send()` 或 `recv()`。

### 发送文件或连续数据

文件内容和普通内存数据使用同一组 `netins_udp_send()` /
`netins_udp_recv()` 接口。发送端循环读取文件并调用 `send()`，接收端循环调用
`recv()` 并写入文件即可。库内部自动使用聚合数据通道，调用方不需要创建额外
的数据对象或设置并发数量。

客户端未设置算法选项时默认使用 RAP 调度和 BBR 拥塞控制。如需显式选择，
必须在 `netins_udp_connect()` 或 `netins_udp_listen()` 之前设置：

```c
int scheduler = NETINS_UDP_SCHEDULER_RAP;
int cc = NETINS_UDP_CC_BBR;

netins_udp_set_opt(conn, NETINS_UDP_OPT_SCHEDULER,
    &scheduler, sizeof(scheduler));
netins_udp_set_opt(conn, NETINS_UDP_OPT_CONGESTION_CONTROL,
    &cc, sizeof(cc));
```

服务端需要改变发送侧算法时，对 listener 设置上述选项；后续接受的连接会
继承 listener 的设置。

---

## 8. 关闭连接

### `netins_udp_shutdown()`

发起连接关闭，但不释放对象。

```c
int netins_udp_shutdown(netins_udp_t *netins,
    uint64_t application_error);
```

正常关闭使用：

```c
netins_udp_shutdown(conn, 0);
```

`application_error=0` 时，库会结束发送方向。服务端 accepted connection 会等待
客户端完成接收并关闭连接，等待时间受关闭超时配置限制。非零值表示错误关闭。

调用后仍可以查询最终连接状态，直到调用 `netins_udp_close()` 释放对象。

### 推荐关闭顺序

```c
/* 1. 停止发起新的 send/recv */
netins_udp_shutdown(conn, 0);

/* 2. 如有需要，查询最终连接状态 */

/* 3. 释放连接对象 */
netins_udp_close(conn);

/* 4. 所有对象关闭后释放运行环境 */
netins_udp_cleanup();
```

对于服务端：

1. 关闭所有 accepted connection；
2. 关闭 listener；
3. 调用 `netins_udp_cleanup()`。

---

## 9. 连接状态查询

### `netins_udp_get_state()`

```c
int netins_udp_get_state(netins_udp_t *netins);
```

可能的状态：

| 状态 | 说明 |
|---|---|
| `NETINS_UDP_STATE_NEW` | 对象已创建，尚未连接或监听 |
| `NETINS_UDP_STATE_CONNECTING` | 正在建立连接 |
| `NETINS_UDP_STATE_LISTENING` | 正在监听 |
| `NETINS_UDP_STATE_CONNECTED` | 连接已建立 |
| `NETINS_UDP_STATE_CLOSING` | 正在关闭 |
| `NETINS_UDP_STATE_CLOSED` | 已关闭 |
| `NETINS_UDP_STATE_ERROR` | 连接发生错误 |

### `netins_udp_get_info()`

```c
int netins_udp_get_info(netins_udp_t *netins,
    netins_udp_info_t *out);
```

输出结构必须使用初始化宏：

```c
netins_udp_info_t info = NETINS_UDP_INFO_INITIALIZER;

if (netins_udp_get_info(conn, &info) == NETINS_UDP_OK) {
    printf("state=%d paths=%u active_paths=%u multipath=%d\n",
        info.state, info.path_count, info.active_path_count,
        info.multipath_active);
}
```

常用字段：

- `state`：当前连接状态；
- `handshake_finished`：握手是否完成；
- `multipath_requested`：创建对象时是否请求多路径；
- `multipath_active`：当前是否实际启用多路径；
- `path_count`：连接生命周期中已记录的路径数量；
- `active_path_count`：当前活动路径数量；
- `local_address`、`peer_address`：连接地址。

---

## 10. 多路径和逐路径查询

### `netins_udp_is_multipath()`

查询当前连接是否实际启用多路径。

```c
int netins_udp_is_multipath(netins_udp_t *netins);
```

返回值：

- 1：多路径已启用；
- 0：当前不是多路径连接；
- -1：查询失败。

### `netins_udp_get_path_count()`

查询路径数量。

```c
int netins_udp_get_path_count(netins_udp_t *netins,
    uint32_t *out_count);
```

### `netins_udp_get_path_statis()`

查询指定路径状态和统计。

```c
int netins_udp_get_path_statis(netins_udp_t *netins,
    size_t path_index, netins_udp_path_statis_t *out);
```

使用示例：

```c
uint32_t path_count = 0;

if (netins_udp_get_path_count(conn, &path_count)
    == NETINS_UDP_OK)
{
    for (uint32_t i = 0; i < path_count; ++i) {
        netins_udp_path_statis_t path =
            NETINS_UDP_PATH_STATIS_INITIALIZER;

        if (netins_udp_get_path_statis(conn, i, &path)
            != NETINS_UDP_OK)
        {
            continue;
        }

        printf("path=%u interface=%s state=%d "
               "sent=%llu recv=%llu srtt=%llu us "
               "cwnd=%llu inflight=%llu\n",
            path.path_index,
            path.interface_name,
            path.path_state,
            (unsigned long long)path.bytes_sent,
            (unsigned long long)path.bytes_received,
            (unsigned long long)path.srtt_us,
            (unsigned long long)path.cwnd_bytes,
            (unsigned long long)path.bytes_inflight);
    }
}
```

路径状态：

| 状态 | 说明 |
|---|---|
| `NETINS_UDP_PATH_INIT` | 路径初始化 |
| `NETINS_UDP_PATH_VALIDATING` | 路径正在验证 |
| `NETINS_UDP_PATH_ACTIVE` | 路径可用 |
| `NETINS_UDP_PATH_CLOSING` | 路径正在关闭 |
| `NETINS_UDP_PATH_CLOSED` | 路径已关闭 |
| `NETINS_UDP_PATH_FAILED` | 路径失败 |

常用路径字段：

- `interface_name`：对应网卡名称；
- `path_state`：路径状态；
- `bytes_sent`、`bytes_received`：路径业务字节数；
- `send_mbps`、`receive_mbps`：当前统计区间速率；
- `srtt_us`、`latest_rtt_us`、`min_rtt_us`：路径 RTT；
- `cwnd_bytes`：拥塞窗口；
- `bytes_inflight`：在途字节数；
- `cwnd_blocked`：是否受拥塞窗口限制；
- `loss_rate`：丢包率统计。

路径下标在一个连接生命周期内保持稳定，不会因为路径关闭和重新建立而复用。

---

## 11. 完整客户端示例

下面的示例通过两张网卡建立多路径连接，发送一条消息并接收服务端响应。

```c
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <xquic/netins_udp.h>

static void
print_error(const char *operation, netins_udp_t *conn)
{
    char message[NETINS_UDP_ERROR_MSG_LEN] = {0};
    if (conn != NULL) {
        netins_udp_get_last_error_msg(conn, message, sizeof(message));
    }
    fprintf(stderr, "%s failed: code=%d message=%s\n",
        operation,
        conn != NULL ? netins_udp_get_last_error_code(conn) : errno,
        message);
}

static int
send_all(netins_udp_t *conn, const void *data, size_t length, int finish)
{
    const unsigned char *bytes = data;
    size_t offset = 0;
    while (offset < length) {
        ssize_t sent = netins_udp_send(conn, bytes + offset,
            length - offset,
            finish ? NETINS_UDP_MSG_FIN : NETINS_UDP_MSG_NONE);
        if (sent < 0) {
            return -1;
        }
        if (sent == 0) {
            return -1;
        }
        offset += (size_t)sent;
    }
    return 0;
}

int
main(void)
{
    int result = 1;
    netins_udp_t *conn = NULL;

    if (netins_udp_init() != NETINS_UDP_OK) {
        return 1;
    }

    conn = netins_udp_new(NETINS_UDP_TYPE_MPQUIC);
    if (conn == NULL) {
        goto done;
    }

    netins_udp_interfaces_t interfaces =
        NETINS_UDP_INTERFACES_INITIALIZER;
    interfaces.interface_count = 2;
    snprintf(interfaces.names[0], sizeof(interfaces.names[0]),
        "%s", "rmnet_data0");
    snprintf(interfaces.names[1], sizeof(interfaces.names[1]),
        "%s", "usb0");
    if (netins_udp_set_opt(conn, NETINS_UDP_OPT_INTERFACES,
            &interfaces, sizeof(interfaces)) != NETINS_UDP_OK)
    {
        print_error("set interfaces", conn);
        goto done;
    }

    netins_udp_tls_config_t tls =
        NETINS_UDP_TLS_CONFIG_INITIALIZER;
    snprintf(tls.server_name, sizeof(tls.server_name),
        "%s", "server.example.com");
    snprintf(tls.ca_file, sizeof(tls.ca_file),
        "%s", "/opt/netins/certs/ca.pem");
    if (netins_udp_set_opt(conn, NETINS_UDP_OPT_TLS,
            &tls, sizeof(tls)) != NETINS_UDP_OK)
    {
        print_error("set TLS", conn);
        goto done;
    }

    if (netins_udp_connect(conn, "192.0.2.10", 8443)
        != NETINS_UDP_OK)
    {
        print_error("connect", conn);
        goto done;
    }

    printf("connected, multipath=%d\n",
        netins_udp_is_multipath(conn));

    const char message[] = "hello from NetIns UDP";
    if (send_all(conn, message, sizeof(message) - 1, 1) != 0) {
        print_error("send", conn);
        goto done;
    }

    unsigned char buffer[4096];
    for (;;) {
        ssize_t received = netins_udp_recv(conn, buffer,
            sizeof(buffer), NETINS_UDP_MSG_NONE);
        if (received > 0) {
            fwrite(buffer, 1, (size_t)received, stdout);
            continue;
        }
        if (received == 0) {
            break;
        }
        print_error("recv", conn);
        goto done;
    }

    result = 0;

done:
    if (conn != NULL) {
        if (netins_udp_get_state(conn) == NETINS_UDP_STATE_CONNECTED) {
            (void)netins_udp_shutdown(conn, 0);
        }
        netins_udp_close(conn);
    }
    (void)netins_udp_cleanup();
    return result;
}
```

---

## 12. 完整服务端示例

```c
#include <stdio.h>
#include <string.h>

#include <xquic/netins_udp.h>

int
main(void)
{
    int result = 1;
    netins_udp_t *listener = NULL;
    netins_udp_t *connection = NULL;

    if (netins_udp_init() != NETINS_UDP_OK) {
        return 1;
    }

    listener = netins_udp_new(NETINS_UDP_TYPE_MPQUIC);
    if (listener == NULL) {
        goto done;
    }

    netins_udp_tls_config_t tls =
        NETINS_UDP_TLS_CONFIG_INITIALIZER;
    snprintf(tls.certificate_file, sizeof(tls.certificate_file),
        "%s", "/opt/netins/certs/server.crt");
    snprintf(tls.private_key_file, sizeof(tls.private_key_file),
        "%s", "/opt/netins/certs/server.key");
    if (netins_udp_set_opt(listener, NETINS_UDP_OPT_TLS,
            &tls, sizeof(tls)) != NETINS_UDP_OK)
    {
        goto done;
    }

    if (netins_udp_listen(listener, NULL, 8443) == NULL) {
        goto done;
    }

    connection = netins_udp_accept(listener, NULL, NULL);
    if (connection == NULL) {
        goto done;
    }

    unsigned char buffer[64 * 1024];
    for (;;) {
        ssize_t received = netins_udp_recv(connection, buffer,
            sizeof(buffer), NETINS_UDP_MSG_NONE);
        if (received > 0) {
            printf("received %zd bytes\n", received);
            continue;
        }
        if (received == 0) {
            break;
        }
        goto done;
    }

    const char response[] = "server received data\n";
    size_t offset = 0;
    while (offset < sizeof(response) - 1) {
        ssize_t sent = netins_udp_send(connection,
            response + offset, sizeof(response) - 1 - offset,
            NETINS_UDP_MSG_FIN);
        if (sent < 0) {
            goto done;
        }
        if (sent == 0) {
            goto done;
        }
        offset += (size_t)sent;
    }

    (void)netins_udp_shutdown(connection, 0);
    result = 0;

done:
    if (connection != NULL) {
        netins_udp_close(connection);
    }
    if (listener != NULL) {
        netins_udp_close(listener);
    }
    (void)netins_udp_cleanup();
    return result;
}
```

实际服务端通常会循环调用 `accept()`，并为每个 accepted connection 创建独立
工作线程或任务。

---

## 13. UDP API Demo

交付包提供两个可以独立编译和运行的 Demo：

| 程序 | 运行环境 | 作用 |
|---|---|---|
| `netins_udp_data_client` | AArch64 板卡或 Linux 客户端 | 建立连接、发送文件、接收文件并查询多路径状态 |
| `netins_udp_data_server` | Linux x86_64 服务器 | 监听和接受连接、接收文件、发送文件 |

两个 Demo 只包含公共头文件：

```c
#include <xquic/netins_udp.h>
```

它们不包含 mini 或 `src/netins_udp/` 中的私有头文件，可作为第三方程序接入
`netins_udp.h` 和 XQUIC 库的参考。

### 13.1 编译Demo

配置CMake时，`NETINS_UDP_BUILD_DATA_DEMO` 默认开启。也可以显式指定：

```sh
cmake -S . -B build \
  -DNETINS_UDP_BUILD_DATA_DEMO=ON \
  [其他平台和依赖参数]
```

AArch64客户端：

```sh
cmake --build build-api-client-aarch64-final \
  --target netins_udp_data_client \
  -j$(nproc)
```

x86_64服务端：

```sh
cmake --build build-api-server-x86_64 \
  --target netins_udp_data_server \
  -j$(nproc)
```

### 13.2 服务端Demo

参数格式：

```text
netins_udp_data_server
  BIND_HOST PORT CERTIFICATE PRIVATE_KEY RECEIVE_FILE SEND_FILE
```

运行示例：

```sh
./netins_udp_data_server \
  '*' 18446 \
  server.crt server.key \
  received-from-client.bin server-send.bin
```

- `BIND_HOST`：监听地址；`*` 表示监听所有本地地址；
- `RECEIVE_FILE`：保存客户端上传的数据，原文件会被覆盖；
- `SEND_FILE`：客户端上传结束后，服务端发送给客户端的数据；
- 当前Demo处理一个连接后退出，下一轮测试需要重新启动。

### 13.3 客户端Demo

参数格式：

```text
netins_udp_data_client
  HOST PORT SERVER_NAME CA_FILE
  SEND_FILE RECEIVE_FILE
  [INTERFACE ...]
```

双路径运行示例：

```sh
./netins_udp_data_client \
  124.70.219.194 18446 \
  124.70.219.194 ca.crt \
  client-send.bin received-download.bin \
  rmnet_data0 usb0
```

单路径运行时只填写一个网卡，例如：

```sh
./netins_udp_data_client \
  124.70.219.194 18446 \
  124.70.219.194 ca.crt \
  client-send.bin received-download.bin \
  usb0
```

- `HOST`、`PORT`：服务端地址和端口；
- `SERVER_NAME`：证书中的服务器名称；
- `CA_FILE`：用于校验服务端证书的CA文件；
- `SEND_FILE`：发送给服务端的文件；
- `RECEIVE_FILE`：保存服务端返回数据的文件；
- `[INTERFACE ...]`：一个或多个本地网络接口名称。

`CA_FILE` 使用 `-` 会关闭服务端证书校验，只允许用于临时联调；正式部署必须
配置可信CA文件。

### 13.4 GET和POST测试

Demo按照以下顺序完成双向传输：

```text
客户端发送 SEND_FILE
→ 服务端保存到 RECEIVE_FILE
→ 服务端发送 SEND_FILE
→ 客户端保存到 RECEIVE_FILE
```

只测试GET时，客户端可以使用空上传文件：

```sh
: > empty.bin
```

只测试POST时，服务端可以使用空发送文件：

```sh
: > server-empty.bin
```

成功输出示例：

```text
sent=104857600 received=0 paths=2 active_paths=2 multipath=1
```

其中：

- `sent`：应用成功发送的字节数；
- `received`：应用成功接收的字节数；
- `paths`：连接生命周期内记录的路径数量；
- `active_paths`：当前活动路径数量；
- `multipath`：1表示多路径已经启用，0表示当前为单路径。

### 13.5 本机回环压力测试

交付包中的 `scripts/netins_udp_api_stress.sh` 可在Linux x86_64服务器上反复
启动两个Demo，执行双向文件传输并逐字节校验结果：

```sh
./scripts/netins_udp_api_stress.sh \
  build-api-server-x86_64 \
  server.crt server.key \
  18446 5 200
```

最后两个参数分别表示循环次数和每个方向的文件大小（MiB）。脚本生成的文件与
日志保存在其输出的临时目录中。该脚本用于验证API收发正确性和重复创建、销毁
稳定性；它是本机回环测试，不用于衡量5G多路径吞吐。

---

## 14. 编译和链接

动态库链接示例：

```sh
cc app.c -I/opt/netins-udp/include \
  -L/opt/netins-udp/lib \
  -lxquic -lssl -lcrypto -lpthread -ldl -lm \
  -o app
```

静态库链接示例：

```sh
cc app.c -I/opt/netins-udp/include \
  /opt/netins-udp/lib/libxquic-static.a \
  -lssl -lcrypto -lpthread -ldl -lm \
  -o app
```

运行时需要保证头文件与库文件来自同一交付版本。

---

## 15. 使用注意事项

### 资源管理

- 每次成功的 `netins_udp_init()` 都应对应一次 `netins_udp_cleanup()`；
- 每个 `netins_udp_new()` 创建的对象都必须关闭；
- 每个 `netins_udp_accept()` 返回的连接都必须单独关闭；
- `netins_udp_close()` 返回后不得再次使用该指针。

### 多路径判断

- 创建 `NETINS_UDP_TYPE_MPQUIC` 对象只表示请求使用多路径；
- 必须在连接建立后查询 `netins_udp_is_multipath()`；
- 路径数量和活动路径数量应通过 `netins_udp_get_path_count()` 或
  `netins_udp_get_info()` 查询；
- 单条链路故障时，连接可能保持有效，但实际吞吐会随剩余路径能力变化。

### 数据收发

- `send()` 可能部分完成，必须循环处理；
- `recv()` 返回 0 表示对端正常结束发送，不是错误；
- `NETINS_UDP_MSG_FIN` 用于结束本端发送方向；
- 不要依赖一次 `send()` 对应一次 `recv()`；
- 普通连续数据和文件内容均使用固定大小缓冲区循环调用 `send/recv`。

### 阻塞行为

- 默认情况下 `connect()`、`accept()`、`send()` 和 `recv()` 为阻塞调用；
- 非阻塞模式下暂时无法完成时返回 -1，并设置 `EAGAIN` 或 `EINPROGRESS`；
- 超时和非阻塞选项建议在首次收发之前配置完成。

### 线程使用

- 不同连接对象可以由不同线程使用；
- 同一连接允许一个发送线程与一个接收线程并行；
- 开始 `close()` 后不得再提交新的调用。
