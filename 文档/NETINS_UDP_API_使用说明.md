# NetIns UDP Mini API 使用说明

本文面向集成 Mini API 的第三方 C/C++ 开发者，说明当前 API v1 的公开接口、连接建立、
GET/POST 文件收发、持续数据流、状态查询、错误处理和连接关闭流程。

公开接口以 [`include/xquic/netins_udp.h`](../include/xquic/netins_udp.h) 为准。应用程序只需：

```c
#include <xquic/netins_udp.h>
```

## 1. API 能做什么

Mini API 同时提供基于 QUIC/HTTP3 的完整文件任务 API，以及需要显式启用的持续数据流 API。
数据流不是透明 TCP socket：SDK 会把连续字节切成带逻辑 offset 的记录，并聚合到 4 条
H3 Extended Bytestream lane，以保留多流聚合、跨 lane 重排和完成回执能力。

- `netins_udp_connect()` 建立客户端 QUIC/HTTP3 连接；
- `netins_udp_transfer()` 在已连接客户端上执行一次完整 GET 或 POST；
- `netins_udp_listen()` 和 `netins_udp_accept()` 建立服务端监听并接收连接；
- `netins_udp_serve()` 在 accepted 连接上处理 GET/POST，直到对端关闭；
- `netins_udp_stream_open()` / `accept()` 建立 4-lane 半双工逻辑数据流；
- `netins_udp_stream_write()` 由 `direction` 指定的一端调用，可一边接收生产者 Buffer、一边持续发送；
- `netins_udp_stream_finish()` 等待对端长度与摘要验证回执；
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

`remote_path` 表示 `root_path` 下的资源路径，但采用 URL path 写法，因此即使文件就在
`root_path` 本目录下也必须以 `/` 开头。例如 `root_path=/root/test_xquic_server` 时，
`/kkk.txt` 对应 `/root/test_xquic_server/kkk.txt`，不是操作系统根目录下的 `/kkk.txt`。
`remote_path` 不能只有 `/`；服务端拒绝空路径、`.`、`..`、反斜杠以及符号链接穿越。
POST 会创建或截断目标文件，但其父目录必须已经存在。

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
netins_udp_set_opt(超时/日志/可选TLS覆盖...)  listen 前设置
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

当前 Mini API 交付版本为 `6.5`，可通过头文件中的
`NETINS_UDP_API_RELEASE_VERSION` 获取该版本字符串。服务端调用 `listen()` 时还会通过 INFO
日志输出 `Mini API server version=6.5`。

6.5 的数据流内部线协议为 version 4：lane header 携带半双工方向，除 payload/record CREDIT
外，服务端发送方向还使用 READY 握手。数据流客户端和服务端必须同时使用 6.5，不能与
6.4/v3 混用。该变化不进入原文件 API 的 H3 请求/响应协议，文件 API 流程保持不变。

`NETINS_UDP_API_VERSION` 当前仍为 `1`，它是结构体初始化和 ABI 兼容检查使用的整数版本号，
不是交付版本字符串。公开函数共 28 个。

### 3.1 全局初始化

| 接口 | 返回值 | 说明 |
|---|---|---|
| `uint32_t netins_udp_get_api_version(void)` | ABI 整数版本号 | 可在加载库后检查头文件与动态库 ABI 是否一致；当前返回 `1` |
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

### 3.4 持续数据流

| 接口 | 说明 |
|---|---|
| `netins_udp_stream_open()` | 在启用流模式的已连接 CLIENT 上创建逻辑流；方向决定该端发送还是接收 |
| `netins_udp_stream_accept()` | 在启用流模式的 ACCEPTED handle 上接受同方向逻辑流；方向必须与客户端一致 |
| `netins_udp_stream_write()` | 只能由当前方向的发送端调用；复制调用方 Buffer；`*accepted` 先清零，支持阻塞、超时、非阻塞和部分 accepted；累计 offset 溢出返回 `EOVERFLOW` |
| `netins_udp_stream_flush()` | 等待本地队列提交给 XQUIC，不等于对端已经收到 |
| `netins_udp_stream_finish()` | 发送端等待可靠回执；接收端等待完整 FIN，并等待回执及反向 FIN 被 QUIC ACK。若合规接收端验收完成后立即以 `H3_NO_ERROR` 关闭，且发送端已提交全部数据记录及各 lane FIN，该正常关闭可替代尚未执行的最终回执回调；反向情况下，若合规发送端收到回执后立即正常关闭，且接收端已完整交付数据并提交全部回执/反向 FIN，该正常关闭同样构成完成确认 |
| `netins_udp_stream_reset()` | 取消本地逻辑流；API v1 不把应用错误码传给对端 |
| `netins_udp_stream_get_snapshot()` | 查询队列、重排、记录数和逐 lane 字节统计 |
| `netins_udp_stream_close()` | 释放流对象；每个对象只能关闭一次 |

两端都必须在 `connect()` / `listen()` 前设置 `NETINS_UDP_OPT_STREAM_MODE=1`。本交付版本的
每个 stream-mode 连接承载一个活动逻辑流。文件传输和数据流分别使用普通连接模式与
stream mode；对 stream-mode 连接调用 `netins_udp_transfer()` 或 `netins_udp_serve()` 会返回
`EOPNOTSUPP`。背压、回调与关闭约束在本手册后续章节给出，Demo 和正式压测命令见第 15 章。

`netins_udp_stream_config_t.direction` 选择本次半双工数据方向：

| direction | 数据发送端 | 数据接收端 / `on_data` 所在端 |
|---|---|---|
| `NETINS_UDP_STREAM_DIRECTION_CLIENT_TO_SERVER` | CLIENT / `stream_open()` 端 | ACCEPTED / `stream_accept()` 端 |
| `NETINS_UDP_STREAM_DIRECTION_SERVER_TO_CLIENT` | ACCEPTED / `stream_accept()` 端 | CLIENT / `stream_open()` 端 |

初始化器默认使用 `CLIENT_TO_SERVER`。发送端可以调用
`write()`/`flush()`，接收端必须提供 `on_data`；两端最终都调用 `finish()`。

### 3.5 状态和统计

| 接口 | 返回值/输出 | 说明 |
|---|---|---|
| `int netins_udp_get_state(netins_udp_t *h)` | `netins_udp_state_t` | 返回状态枚举；无活动数据流时非阻塞泵一次内部事件，活动流期间读取后台 worker 发布的连接状态 |
| `int netins_udp_is_multipath(netins_udp_t *h)` | `1`/`0`/`-1` | 只有多路径模式开启且活动路径大于 1 才返回 1 |
| `int netins_udp_get_path_count(netins_udp_t *h, uint32_t *count)` | `0/-1` | 获取当前路径总数 |
| `int netins_udp_get_path_statis(netins_udp_t *h, size_t index, netins_udp_path_statis_t *out)` | `0/-1` | 获取指定路径统计 |
| `int netins_udp_get_snapshot(netins_udp_t *h, netins_udp_snapshot_t *out)` | `0/-1` | 一次获取连接、握手、多路径及所有路径统计 |
| `int netins_udp_get_close_reason(netins_udp_t *h, netins_udp_close_reason_t *out)` | `0/-1` | 获取关闭来源、系统错误、传输错误和文本 |

`get_state()` 不是普通的 `0/-1` 状态码函数。正常情况下它直接返回
`NETINS_UDP_STATE_NEW` 至 `NETINS_UDP_STATE_ERROR` 中的一个值。
连接和路径查询可以在数据流活动期间调用；它们与后台 XQUIC engine 驱动串行化，不会再次
进入同一个 event base，可用于观察网卡移除、剩余活动路径和恢复后的路径重新加入。

### 3.6 错误详情

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
netins_udp_tls_memory_config_t tls_memory =
    NETINS_UDP_TLS_MEMORY_CONFIG_INITIALIZER;
netins_udp_logger_config_t logger = NETINS_UDP_LOGGER_CONFIG_INITIALIZER;
netins_udp_zlog_config_t zlog = NETINS_UDP_ZLOG_CONFIG_INITIALIZER;
netins_udp_transfer_t task = NETINS_UDP_TRANSFER_INITIALIZER;
netins_udp_transfer_result_t result =
    NETINS_UDP_TRANSFER_RESULT_INITIALIZER;
netins_udp_serve_config_t serve = NETINS_UDP_SERVE_CONFIG_INITIALIZER;
netins_udp_snapshot_t snapshot = NETINS_UDP_SNAPSHOT_INITIALIZER;
netins_udp_path_statis_t path = NETINS_UDP_PATH_STATIS_INITIALIZER;
netins_udp_close_reason_t reason = NETINS_UDP_CLOSE_REASON_INITIALIZER;
netins_udp_stream_config_t stream_config =
    NETINS_UDP_STREAM_CONFIG_INITIALIZER;
netins_udp_stream_result_t stream_result =
    NETINS_UDP_STREAM_RESULT_INITIALIZER;
netins_udp_stream_snapshot_t stream_snapshot =
    NETINS_UDP_STREAM_SNAPSHOT_INITIALIZER;
```

不要对结构体只做局部赋值，也不要手工猜测 `struct_size`。

所有结构体开头的 `struct_size` 和 `api_version` 均由初始化器填写；名称包含 `reserved` 的
字段由 API 保留，调用方不要修改。配置/任务结构体由调用方填写业务字段，结果/状态结构体则
只需初始化，具体内容由 API 回填。

### 5.1 公开结构体字段与使用位置

#### `netins_udp_interfaces_t`：客户端网卡配置（输入）

| 字段 | 调用方如何填写 | 含义 |
|---|---|---|
| `interface_count` | 设置为 1～8 | 本次连接期望使用的网卡数量 |
| `names[i]` | 写入系统网卡名，例如 `rmnet_data0`、`usb0` | 第 `i` 张网卡；下标必须小于 `interface_count` |

填写后，在客户端 `connect()` 前传给：

```c
netins_udp_set_opt(client, NETINS_UDP_OPT_INTERFACES,
    &interfaces, sizeof(interfaces));
```

#### `netins_udp_tls_config_t`：TLS 配置（输入）

| 字段 | 使用角色 | 调用方如何填写 |
|---|---|---|
| `server_name` | CLIENT | TLS SNI/证书名称；当前可以填写服务器 IP 字符串 |
| `certificate_file` | SERVER | 服务端证书文件路径，例如 `./server.crt` |
| `private_key_file` | SERVER | 与证书匹配的私钥路径，例如 `./server.key` |

客户端不使用 `certificate_file` 和 `private_key_file`。服务端不设置 TLS 证书选项时自动使用
API 内置证书；需要用文件覆盖时，在 `listen()` 前填写证书和私钥并传给
`NETINS_UDP_OPT_TLS`。

#### `netins_udp_tls_memory_config_t`：服务端内存 TLS 配置（输入）

| 字段 | 调用方如何填写 | 含义 |
|---|---|---|
| `certificate_pem` | 指向完整 PEM 证书 Buffer | 服务端证书内容 |
| `certificate_pem_len` | PEM Buffer 的实际字节数 | 不能为 0，最大 `NETINS_UDP_TLS_PEM_MAX_LEN` |
| `private_key_pem` | 指向完整 PEM 私钥 Buffer | 与证书匹配的私钥内容 |
| `private_key_pem_len` | PEM Buffer 的实际字节数 | 不能为 0，最大 `NETINS_UDP_TLS_PEM_MAX_LEN` |

填写后在服务端 `listen()` 前传给 `NETINS_UDP_OPT_TLS_MEMORY`。API 在 `set_opt()` 返回成功前
已经复制两个 Buffer，调用方随后可以释放或复用原内存。客户端不支持该选项。

#### `netins_udp_logger_config_t`：API 日志回调（输入）

| 字段 | 调用方如何填写 | 含义 |
|---|---|---|
| `callback` | 日志回调函数，或 `NULL` | 同步接收 API/mini 运行日志；`NULL` 表示不接收 |
| `user_data` | 任意调用方指针 | API 调用回调时原样传回，常用于传递文件句柄或对象上下文 |
| `minimum_level` | `DEBUG/INFO/WARN/ERROR` | 只回调不低于该等级的日志 |

通过 `NETINS_UDP_OPT_LOGGER` 设置。它是唯一允许在活动 handle 上替换的选项；回调执行期间
不能重新调用当前 handle，`message` 只在本次回调期间有效。活动 handle 的替换调用返回后，
该 handle 的后台 worker 已经切换完成，旧 callback/`user_data` 可以释放。stream mode 的
ACCEPTED handle 使用独立日志配置且初始为空；需要 API 层连接日志时应在 ACCEPTED handle
上单独设置。它不会覆盖 listener 共享 I/O worker 的 logger；如需修改共享 worker 的 logger，
必须在 SERVER listener handle 上设置。关闭 stream listener 返回前，共享 worker 会停止使用
listener 的 callback/`user_data`，此后可以释放对应日志上下文。文件 API 的 accepted logger
继承行为保持不变。

#### `netins_udp_zlog_config_t`：zlog 配置（输入）

| 字段 | 调用方如何填写 | 含义 |
|---|---|---|
| `enabled` | `0` 或 `1` | `0` 关闭，`1` 启用 zlog；初始化后默认为 0 |
| `minimum_level` | `DEBUG/INFO/WARN/ERROR` | zlog 最低记录等级 |
| `config_file` | zlog 配置文件路径 | 决定日志文件、格式和滚动策略 |
| `category` | zlog category 名称 | 例如 `xquic_mini_api_client` |

仅在需要 zlog 时填写，并在 `connect()` 或 `listen()` 前通过 `NETINS_UDP_OPT_ZLOG` 设置；
不设置该选项不会初始化 zlog。

#### `netins_udp_transfer_t`：单次文件任务（输入）

| 字段 | 调用方如何填写 | 含义 |
|---|---|---|
| `direction` | `NETINS_UDP_TRANSFER_GET` 或 `NETINS_UDP_TRANSFER_POST` | 下载或上传 |
| `timeout_ms` | 毫秒数；0 表示无限等待 | 本次完整文件任务的超时 |
| `local_path` | 客户端本地文件路径 | GET 的下载目标；POST 的上传源文件 |
| `remote_path` | 以 `/` 开头的服务端资源路径 | GET 的服务端源文件；POST 的服务端目标文件 |

它不传给 `set_opt()`，而是直接传给：

```c
netins_udp_transfer(client, &task, &result);
```

`local_path` 和 `remote_path` 指向的字符串必须保持有效，直到本次 `netins_udp_transfer()`
返回。

#### `netins_udp_transfer_result_t`：文件任务结果（输出）

| 字段 | API 返回的含义 |
|---|---|
| `total_bytes` | 本次任务的文件总字节数 |
| `total_fragments` | 计划传输的业务分片数 |
| `completed_fragments` | 已完成的业务分片数；成功时应等于 `total_fragments` |
| `elapsed_us` | 与原 Mini 一致的有效传输区间，单位微秒 |
| `throughput_mbps` | 按 `total_bytes * 8 / elapsed_us` 得到的 Mbps |

调用方只需在调用前使用初始化器，不要预填结果字段；`netins_udp_transfer()` 返回后读取。

#### `netins_udp_serve_config_t`：accepted 连接的服务配置（输入）

| 字段 | 调用方如何填写 | 含义 |
|---|---|---|
| `root_path` | 服务端文件根目录 | 所有 `remote_path` 都限制在该目录内解析 |
| `timeout_ms` | 毫秒数；0 表示无限等待 | `netins_udp_serve()` 的服务超时 |

它不传给 `set_opt()`。在 `netins_udp_accept()` 返回 accepted handle 后使用：

```c
netins_udp_serve(connection, &serve);
```

`root_path` 指向的字符串必须保持有效，直到 `netins_udp_serve()` 返回。

#### `netins_udp_stream_config_t`：半双工数据流配置（输入）

| 字段 | 调用方如何填写 | 含义 |
|---|---|---|
| `direction` | 两个 `NETINS_UDP_STREAM_DIRECTION_*` 值之一 | 本次逻辑流的大流量数据方向；两端必须一致 |
| `flags` | `NONE` 或 `NONBLOCK` | 控制发送端 `write()` 在本地队列满时等待还是返回 `EAGAIN` |
| `reserved_size[2]` | 保持为 0 | ABI 保留槽；不是队列配置入口 |
| `on_data` | 接收端必须填写 | 按逻辑 offset 递增交付；Buffer 只在回调期间有效 |
| `user_data` | 可选 | 原样传给 `on_data` |

客户端和服务端分别把同一个 `direction` 配置传给 `netins_udp_stream_open()` 与
`netins_udp_stream_accept()`。发送端不需要 `on_data`；接收端缺少回调会以 `EINVAL` 拒绝。

`on_data()` 由 SDK 的接收 delivery worker 调用，不在应用主线程中执行。传入的 `data`
Buffer 只在本次回调期间有效；需要在回调返回后继续处理时，应用必须先复制数据。回调应尽快
完成数据消费、复制或状态通知并返回，不能在回调内对同一条 stream 调用
`netins_udp_stream_finish()`、`netins_udp_stream_close()`，也不能等待依赖该 stream 后续收包、
回执或完成状态的条件。否则会形成自等待，或者在当前 delivery worker 尚未退出时释放其
stream 对象。需要结束传输时，推荐由回调记录状态或通知业务线程，再由负责该 stream 生命周期
的线程在回调返回后执行 `reset()`、`finish()` 和 `close()`。

#### `netins_udp_path_statis_t`：单条路径统计（输出）

| 字段 | API 返回的含义 |
|---|---|
| `path_id` / `path_index` | 传输层路径标识和数组下标 |
| `path_state` | `INIT`、`VALIDATING`、`ACTIVE`、`CLOSING` 或 `CLOSED` |
| `app_status` | `NONE`、`STANDBY`、`AVAILABLE` 或 `FROZEN` |
| `interface_name` | 路径绑定的网卡名 |
| `local_address` / `peer_address` | 路径本地地址和对端地址 |
| `packets_sent` / `packets_received` | 该路径累计收发包数 |
| `bytes_sent` / `bytes_received` | 该路径累计收发字节数 |
| `reinjected_bytes_sent` / `reinjected_bytes_received` | 路径重注入字节统计 |
| `effective_bytes_received` | 去除重复影响后的有效接收字节统计 |
| `srtt_us` | 该路径平滑 RTT，单位微秒 |
| `cwnd_bytes` / `bytes_inflight` | API v1 固定为 0；当前只能读取连接级 inflight |

调用方初始化后，通过 `netins_udp_get_path_statis(handle, path_index, &path)` 获取；也可以从
`snapshot.paths[i]` 读取。上述字段均由 API 填写，不要由调用方修改。

#### `netins_udp_close_reason_t`：关闭原因（输出）

| 字段 | API 返回的含义 |
|---|---|
| `source` | `NONE`、`LOCAL`、`PEER`、`TRANSPORT`、`TIMEOUT` 或 `INTERNAL` |
| `system_error` | 系统调用错误码；无系统错误时为 0 |
| `transport_error` | QUIC/传输层错误码；无此类错误时为 0 |
| `message` | 可读错误说明 |

通过 `netins_udp_get_close_reason(handle, &reason)` 获取；也包含在
`snapshot.close_reason` 中。必须在 `netins_udp_close()` 释放 handle 前查询。

#### `netins_udp_snapshot_t`：连接完整快照（输出）

| 字段 | API 返回的含义 |
|---|---|
| `snapshot_time_us` | 生成快照的时间戳，单位微秒 |
| `role` / `state` | handle 角色和当前生命周期状态 |
| `handshake_finished` | QUIC/TLS 握手是否完成 |
| `multipath_mode` / `multipath_state` | 是否启用多路径及当前多路径状态 |
| `path_count` / `active_path_count` | 已知路径数和当前活动路径数 |
| `packets_sent` / `packets_received` | 连接级累计收发包数 |
| `packets_lost` / `spurious_packets_lost` | 连接级丢包及伪丢包统计 |
| `srtt_us` / `min_rtt_us` | 连接级平滑 RTT 和最小 RTT，单位微秒 |
| `bytes_inflight` | 连接级当前在途字节数 |
| `total_app_bytes` | 连接级累计应用数据字节数 |
| `standby_path_app_bytes` | 备用路径承载的应用数据字节数 |
| `paths[]` | 每条路径的 `netins_udp_path_statis_t` |
| `close_reason` | 当前记录的关闭原因 |

调用方初始化后，通过 `netins_udp_get_snapshot(handle, &snapshot)` 获取。快照中的所有业务
字段均由 API 填写；它是某个时刻的副本，后续状态变化不会自动更新旧快照。

`NETINS_UDP_OPT_SCHEDULER`、`NETINS_UDP_OPT_CONGESTION_CONTROL`、各类 timeout 和 socket
buffer 使用普通 `int` 或 `uint32_t`，不是结构体。

#### `netins_udp_stream_snapshot_t`：数据流快照（输出）

`send_queue_bytes`、`reorder_bytes` 及其 high-water 字段统计 payload，queue capacity
同时作为内部内存预算。为避免大量小写入放大节点开销，每个 record 至少消耗 256 字节
内部额度，接收端还通过独立的累计 record CREDIT 限制未交付记录数。因此小 record 场景的
瞬时 payload 高水位可能明显低于配置容量，这是预期的内存保护行为。

除队列高水位、record 计数和逐 lane 字节统计外，`preaccept_lane_count` 仅在服务端
ACCEPT 端点有意义（无论该端在本次方向中负责发送还是接收）：
它记录调用 `netins_udp_stream_accept()` 开始时，服务端已经完整缓存的 40 字节 lane header
数量。正常业务无需依赖该字段；客户端 OPEN 端点该字段为 0。

### 5.2 `netins_udp_set_opt()` 选项

| option | `value` 类型 | 角色 | 默认/约束 |
|---|---|---|---|
| `NETINS_UDP_OPT_INTERFACES` | `netins_udp_interfaces_t` | CLIENT | 1～8 个非空网卡名 |
| `NETINS_UDP_OPT_SCHEDULER` | `int`，值为 `netins_udp_scheduler_t` | CLIENT/SERVER | GET 使用 ADAPTIVE；POST 客户端默认 BALANCED，可选择其他模式 |
| `NETINS_UDP_OPT_CONGESTION_CONTROL` | `int`，值为 `netins_udp_congestion_control_t` | CLIENT/SERVER | default/CUBIC/BBR |
| `NETINS_UDP_OPT_TLS` | `netins_udp_tls_config_t` | CLIENT/SERVER | client 只用 SNI；server 用文件证书覆盖内置证书 |
| `NETINS_UDP_OPT_TLS_MEMORY` | `netins_udp_tls_memory_config_t` | SERVER | 用 PEM Buffer 覆盖内置证书；每个 Buffer 最大 1 MiB |
| `NETINS_UDP_OPT_CONNECT_TIMEOUT_MS` | `uint32_t` | CLIENT | 默认 10000 ms；不能为 0 |
| `NETINS_UDP_OPT_ACCEPT_TIMEOUT_MS` | `uint32_t` | SERVER | 默认 0，表示无限等待 |
| `NETINS_UDP_OPT_CLOSE_TIMEOUT_MS` | `uint32_t` | CLIENT/SERVER | 默认 3000 ms |
| `NETINS_UDP_OPT_KERNEL_SNDBUF` | 正 `int` | CLIENT/SERVER | UDP socket 发送缓冲区 |
| `NETINS_UDP_OPT_KERNEL_RCVBUF` | 正 `int` | CLIENT/SERVER | UDP socket 接收缓冲区 |
| `NETINS_UDP_OPT_LOGGER` | `netins_udp_logger_config_t` | 所有 handle | 唯一可在活动 handle 上替换的选项 |
| `NETINS_UDP_OPT_ZLOG` | `netins_udp_zlog_config_t` | CLIENT/SERVER | 默认关闭；必须在 connect/listen 前设置 |

`value_len` 必须传实际对象大小，例如：

```c
int cc = NETINS_UDP_CC_BBR;
if (netins_udp_set_opt(client, NETINS_UDP_OPT_CONGESTION_CONTROL,
        &cc, sizeof(cc)) != 0) {
    /* 处理错误 */
}
```

### 5.3 单路径和多路径

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
- 三至八个接口：目标路径数与接口数相同；
- 未设置接口：API 保留 mini 的自动多路径目标，实际活动路径由本机地址、路由和握手结果决定，
  默认目标仍为双路径，不等于一定存在两条可用物理链路；
- 应以 `snapshot.active_path_count` 或 `netins_udp_is_multipath()` 判断运行时实际结果。

### 5.4 调度器与拥塞控制

常用调度器包括：

```text
ADAPTIVE
MINRTT
RAP
BALANCED
```

文件服务端 Demo 和文件客户端 Demo 的 GET 使用 `adaptive`。POST 可用
`--scheduler` 选择 `minrtt`、`rap`、`balanced` 等模式，未传参数时默认 `balanced`。

数据流 POST 由客户端发送，stream client 默认 `balanced`，也可以通过 `--scheduler NAME`
选择其他模式；GET 由服务端发送并使用 `adaptive`。scheduler 是连接级选项，必须在
`connect()` / `listen()` 前确定，连接
建立后不能切换。

拥塞控制可选值：

```text
NETINS_UDP_CC_DEFAULT
NETINS_UDP_CC_CUBIC
NETINS_UDP_CC_BBR
```

GET 的大流量发送端是服务端，POST 的大流量发送端是客户端。因此测试调度器和拥塞控制时，
要同时考虑数据发送端的配置，而不能只观察请求发起端。

### 5.5 TLS

服务端有三种证书来源，同一个 handle 只能选择一种自定义来源：

1. 不配置证书：`listen()` 自动使用 API 内置的固定证书和私钥，不需要部署外部文件；
2. 文件覆盖：通过现有 `NETINS_UDP_OPT_TLS` 设置证书和私钥路径；
3. 内存覆盖：通过 `NETINS_UDP_OPT_TLS_MEMORY` 传入 PEM Buffer。

文件覆盖示例：

```c
netins_udp_tls_config_t tls = NETINS_UDP_TLS_CONFIG_INITIALIZER;

snprintf(tls.certificate_file, sizeof(tls.certificate_file),
    "%s", "/root/test_xquic_server/server.crt");
snprintf(tls.private_key_file, sizeof(tls.private_key_file),
    "%s", "/root/test_xquic_server/server.key");

netins_udp_set_opt(server, NETINS_UDP_OPT_TLS, &tls, sizeof(tls));
```

内存覆盖示例：

```c
netins_udp_tls_memory_config_t tls_memory =
    NETINS_UDP_TLS_MEMORY_CONFIG_INITIALIZER;

tls_memory.certificate_pem = certificate_pem;
tls_memory.certificate_pem_len = certificate_pem_len;
tls_memory.private_key_pem = private_key_pem;
tls_memory.private_key_pem_len = private_key_pem_len;

netins_udp_set_opt(server, NETINS_UDP_OPT_TLS_MEMORY,
    &tls_memory, sizeof(tls_memory));
```

文件和内存模式都是对默认内置证书的覆盖。不要在同一个服务端 handle 上先后设置文件和内存
证书；API 会返回 `EALREADY`。同一来源可在 `listen()` 前重新设置。内置证书适用于当前封闭
部署环境，不应将其当作每台服务器独立的身份凭据。

当前 Mini 客户端没有启用证书链、有效期和主机名验证，内置证书保持与既有部署相同的兼容
行为。如果后续启用 `XQC_TLS_CERT_FLAG_NEED_VERIFY`，必须先更换为有效证书并配置对应的
信任链和服务端名称；不能继续依赖当前兼容证书。

客户端不支持客户端证书，`certificate_file` 和 `private_key_file` 必须留空；如需 SNI，只设置：

```c
netins_udp_tls_config_t tls = NETINS_UDP_TLS_CONFIG_INITIALIZER;
snprintf(tls.server_name, sizeof(tls.server_name), "%s", "124.70.219.194");
netins_udp_set_opt(client, NETINS_UDP_OPT_TLS, &tls, sizeof(tls));
```

### 5.6 日志

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
- 数据流客户端和服务端后台 I/O worker 也使用当前 logger，回调可能在后台线程执行；
- 活动 handle 上替换 logger 返回后，旧回调不会再被该 handle 的后台 worker 使用；
- ACCEPTED handle 的 logger 与 SERVER listener 的共享 worker logger 相互隔离；
- callback 为 `NULL` 时，API 及数据流后台 worker 驱动期间的 mini stdout 被抑制；
- `DEBUG=0`、`INFO=1`、`WARN=2`、`ERROR=3`。

内置 zlog 默认关闭，不调用 `NETINS_UDP_OPT_ZLOG` 时不会执行 `zlog_init()`，也不会创建
zlog 日志文件。需要记录 XQUIC 引擎日志时，在 `connect()` 或 `listen()` 前显式配置：

```c
netins_udp_zlog_config_t zlog = NETINS_UDP_ZLOG_CONFIG_INITIALIZER;
zlog.enabled = 1;
zlog.minimum_level = NETINS_UDP_LOG_WARN; /* 只写 WARN/ERROR */
snprintf(zlog.config_file, sizeof(zlog.config_file), "%s", "./zlog.conf");
snprintf(zlog.category, sizeof(zlog.category), "%s",
    "xquic_mini_api_client");

netins_udp_set_opt(handle, NETINS_UDP_OPT_ZLOG, &zlog, sizeof(zlog));
```

zlog 的具体文件路径、滚动策略和格式由 `zlog.conf` 决定。API 使用进程级引用计数管理
`zlog_init()/zlog_fini()`；同一进程中的多个活动 handle 可以使用同一个配置文件和不同
category，但不能同时使用不同的 zlog 配置文件。日志回调与 zlog 可以同时使用：前者接收
Mini API/mini 运行日志，后者接收 XQUIC 引擎日志和 qlog。demo 的 `--log-level` 会同时
设置两者的数字阈值。

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

`netins_udp_transfer()` 仍会阻塞到整个文件任务完成、失败或超时，但成功结果中的
`result.elapsed_us` 使用与原 Mini 相同的测速边界：GET 从第一个响应体字节计到最后一个响应体
字节，不包含异步落盘和 `fsync()`；POST 从第一次提交请求体计到所有请求体被 XQUIC 接受，
属于应用提交速率，不表示服务端已经完整接收。该字段不包含此前的 `connect()` 握手，也不包含
之后的 `shutdown()` 和 `close()`。
`result.throughput_mbps` 的计算为：

```text
total_bytes × 8 / elapsed_us
```

数值单位正好是 Mbit/s（按十进制 10^6）。

### 6.5 客户端数据流

数据流不调用 `netins_udp_transfer()`。必须在 `connect()` 前开启流模式：

```c
int stream_mode = 1;
if (netins_udp_set_opt(client, NETINS_UDP_OPT_STREAM_MODE,
        &stream_mode, sizeof(stream_mode)) != 0) {
    /* 处理错误 */
}
```

Client 作为 POST 发送端时，持续从业务 Buffer 取数据并写入逻辑流。下面的 `data`、
`data_len` 代表应用当前已准备好的一段数据：

```c
netins_udp_stream_config_t config = NETINS_UDP_STREAM_CONFIG_INITIALIZER;
config.direction = NETINS_UDP_STREAM_DIRECTION_CLIENT_TO_SERVER;

netins_udp_stream_t *stream = NULL;
if (netins_udp_stream_open(client, &config, &stream) != 0) {
    /* 处理错误 */
    return;
}

size_t offset = 0;
int failed = 0;
while (offset < data_len) {
    size_t accepted = 0;
    int rc = netins_udp_stream_write(stream,
        (const unsigned char *)data + offset, data_len - offset,
        60000, &accepted);
    offset += accepted;
    if (rc != 0) {
        failed = 1;
        break;
    }
}

if (!failed && netins_udp_stream_flush(stream, 60000) == 0) {
    netins_udp_stream_result_t result = NETINS_UDP_STREAM_RESULT_INITIALIZER;
    if (netins_udp_stream_finish(stream, 600000, &result) == 0) {
        /* result.peer_received_bytes 是已验证的对端接收字节数。 */
    }
} else {
    (void)netins_udp_stream_reset(stream, 0);
}
netins_udp_stream_close(stream);
```

一次 `write()` 会复制已 accepted 的 Buffer，返回后应用可以立即复用该部分内存。数据流
内部队列和 lane 分流由 API 管理，业务代码只需处理 Buffer 写入和背压。
`NONBLOCK` 模式下，应用可在 `errno==EAGAIN` 时等待可写条件后从已 accepted 的位置继续；
其他错误进入 `reset()`/`close()` 清理。

Client 作为 GET 接收端时，把 `direction` 改为
`NETINS_UDP_STREAM_DIRECTION_SERVER_TO_CLIENT`，并在 `open()` 前填写 `config.on_data` 和
`config.user_data`。`on_data` 按 offset 递增接收数据；回调中只复制/消费数据或通知业务线程，
不在回调内对同一 stream 执行 `finish()`/`close()`。

## 7. 服务端：监听、接收和处理连接

### 7.1 创建 listener

```c
netins_udp_init();
netins_udp_t *server = netins_udp_new(NETINS_UDP_ROLE_SERVER);

if (netins_udp_listen(server, "0.0.0.0", 18446) != 0) {
    /* 处理错误 */
}
```

这里没有设置 TLS 证书选项，因此使用内置证书。需要自有证书时，可在 `listen()` 前增加
5.5 节的文件或内存配置。

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

### 7.3 服务端数据流

服务端也必须在 `listen()` 前设置 `NETINS_UDP_OPT_STREAM_MODE=1`。`accept()` 返回连接后，
不调用 `netins_udp_serve()`，而是使用与 Client 相同的 `direction` 接受逻辑流：

```c
netins_udp_t *connection = netins_udp_accept(server);
if (connection == NULL) {
    /* 处理错误 */
    return;
}
netins_udp_stream_config_t config = NETINS_UDP_STREAM_CONFIG_INITIALIZER;
config.direction = NETINS_UDP_STREAM_DIRECTION_CLIENT_TO_SERVER;
config.on_data = on_stream_data;   /* POST 时 Server 是接收端 */
config.user_data = &receiver_state;

netins_udp_stream_t *stream = NULL;
if (netins_udp_stream_accept(connection, &config, &stream) != 0) {
    netins_udp_close(connection);
    return;
}
netins_udp_stream_result_t result = NETINS_UDP_STREAM_RESULT_INITIALIZER;
(void)netins_udp_stream_finish(stream, 600000, &result);
netins_udp_stream_close(stream);
netins_udp_close(connection);
```

GET 时把 `direction` 改为 `SERVER_TO_CLIENT`，Server 不填 `on_data`，而是在
`stream_accept()` 成功后按照 6.5 节的写入、`flush()`、`finish()` 步骤发送数据。两端方向
不一致会以协议错误结束；同一连接不能再进入文件 `transfer()`/`serve()` 流程。

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

    int scheduler = NETINS_UDP_SCHEDULER_ADAPTIVE;
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

    /* 可选：以下配置使用外部文件覆盖默认内置证书。 */
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
- 数据流 `on_data()` 回调不能关闭或阻塞等待同一条 stream；需异步处理的数据必须先复制，
  stream 的 `reset()`、`finish()` 和 `close()` 应交给负责其生命周期的业务线程；
- `connect()`、`accept()`、`transfer()`、`serve()` 和 `shutdown()` 都可能阻塞；
- 在 UI 或业务主线程中集成时，建议由一个专用工作线程独占整个 handle 生命周期；
- 不要用另一个线程并发调用 `shutdown()` 来中断正在运行的 `transfer()`；
- transfer/serve 超时为 0 表示无限等待；
- connect timeout 不能为 0；accept timeout 为 0 表示无限等待。

API 内部事件循环使用阻塞 `EVLOOP_ONCE` 驱动，不再用固定 1 ms 轮询；显式状态查询只进行
一次非阻塞事件泵。

## 13. 传输语义

文件传输沿用当前 mini 数据面的以下参数：

- 文件分片：4 MiB；
- GET 并发 request：8；
- POST 并发 request：4；
- GET 先通过响应头获取总文件长度；
- 使用 offset `pread()`/`pwrite()`；
- 完成一个 request 后自动补充下一个分片；
- GET 返回前等待异步写队列和 `fsync()`；
- 0 字节文件合法；
- 任务超时会关闭/终止连接，避免迟到 request 继续写文件。

数据流使用 4-lane 聚合。GET 方向使用 `adaptive`；POST 默认使用 `balanced`，Demo 可用
`--scheduler` 选择其他上传调度器。

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

- [`demo/netins_udp_mini_api_client.c`](../demo/netins_udp_mini_api_client.c)
- [`demo/netins_udp_mini_api_server.c`](../demo/netins_udp_mini_api_server.c)
- [`demo/netins_udp_stream_client.c`](../demo/netins_udp_stream_client.c)
- [`demo/netins_udp_stream_server.c`](../demo/netins_udp_stream_server.c)

构建后得到：

```text
build/mini/netins_udp_mini_api_client
build/mini/netins_udp_mini_api_server
build/mini/netins_udp_stream_client
build/mini/netins_udp_stream_server
```

命令行参数和 C API 的对应关系：

| demo 参数 | C API |
|---|---|
| `--host`、`--port` | `netins_udp_connect()` |
| `--interface` | `NETINS_UDP_OPT_INTERFACES` |
| `--server-name` | client `NETINS_UDP_OPT_TLS` |
| client `--scheduler` | `NETINS_UDP_OPT_SCHEDULER`；GET 使用 adaptive，文件/流 POST 默认 balanced 并可选 |
| `--cc` | `NETINS_UDP_OPT_CONGESTION_CONTROL` |
| `--get`/`--post` | `task.direction` |
| `--local` | `task.local_path` |
| `--remote` | `task.remote_path` |
| `--timeout-ms` | `task.timeout_ms`，demo 同时用于 connect |
| `--log-level` | callback 与 zlog 的 `minimum_level`，0～3 |
| `--zlog-config`/`--zlog-category` | `NETINS_UDP_OPT_ZLOG`；不传时 zlog 关闭 |
| server `--cert`/`--key` | 可选；server `NETINS_UDP_OPT_TLS` 文件覆盖；不传时使用内置证书 |
| server `--root` | `serve.root_path` |
| server scheduler | GET 使用 `NETINS_UDP_SCHEDULER_ADAPTIVE` |
| server `--accept-timeout-ms` | `NETINS_UDP_OPT_ACCEPT_TIMEOUT_MS` |
| server `--serve-timeout-ms` | `serve.timeout_ms` |

### 15.1 数据流 Demo 参数

数据流 client 的命令格式如下：

```text
netins_udp_stream_client HOST PORT BYTES SEED
  [FLAGS [WRITE_TIMEOUT_MS [FINISH_TIMEOUT_MS]]]
  [--get|--post] [--interface NAME]... [--scheduler NAME]
  [--write-chunk-size N] [--callback-delay-ms N]
```

`--interface` 可以重复 1～8 次并映射到 `NETINS_UDP_OPT_INTERFACES`。只传一个接口用于测
单网卡；传入 `--interface rmnet_data0 --interface usb0` 用于双路径聚合。未传接口时保留
底层默认选路，但性能验收必须显式指定，避免受默认路由影响。

不传方向时兼容旧命令并默认 POST。POST 中 client 是发送端，`--write-chunk-size` 可调整调用侧
每次写入大小，默认 256 KiB；GET 中 client 是接收端，`--callback-delay-ms` 可用于慢回调调试。
性能压测不应启用人工回调延迟。

数据流 server 的命令格式：

```text
netins_udp_stream_server LISTEN_IP PORT EXPECTED_BYTES SEED
  [FINISH_TIMEOUT_MS [CLIENTS]]
  [--get|--post] [--write-chunk-size N]
  [--write-timeout-ms N] [--flags N]
  [--callback-delay-ms N] [--accept-delay-ms N]
```

不传方向时 server 同样默认 POST。POST 中 server 是接收端，`--callback-delay-ms` 模拟慢回调；
GET 中 server 是发送端，使用 `--write-chunk-size`、`--write-timeout-ms` 和 `--flags` 控制写入行为。
`--accept-delay-ms` 在连接 accept 后、逻辑 `stream_accept()` 前
等待，用于验证两个方向在 accept 前都只有四个 lane header。延迟 accept 用例还必须在服务端
快照看到 `preaccept_lanes=4`；只有延迟日志和最终传输成功不足以判定该路径已被覆盖。
delay 默认均为 0，性能测试不得启用。

两端必须选择同一个方向，`BYTES` 和
`EXPECTED_BYTES` 必须一致；传 0 表示严格验收空数据流，仍会完成四 lane FIN、回执和统计。
`BYTES`/`EXPECTED_BYTES` 只是 Demo 生成和校验模拟数据的参数；公共 Stream API 本身不要求发送前
预知总长度，业务可以一边产生 Buffer 一边连续 `write()`，最后以 `finish()` 结束。

### 15.2 正式 GET+POST 聚合压测

交付压测脚本为 `tests/stream_api_benchmark.py`，每轮传输 100 MiB，顺序为
GET `5G1 -> Agg -> 5G2`，然后 POST `5G1 -> Agg -> 5G2`。例如：

```bash
python tests/stream_api_benchmark.py --iterations 10
```

脚本复用文件压测环境已部署的 `monitor_board_resource.sh` 和
`monitor_server_memory.sh`。输出包含每次测试的简化速率日志、GET/POST 聚合率汇总、
资源汇总和资源采样 CSV；不保留 Client/Server 每次运行的原始 stdout/stderr，速率日志也不写入
各 lane 流量。脚本不操作网卡开关，断链恢复需要单独人工验证。

## 16. 常见错误

### `netins_udp_new()` 返回 NULL，`errno=EPERM`

没有先成功调用 `netins_udp_init()`。

### `set_opt()` 返回 `EBUSY`

除 logger 外，选项必须在 `connect()` 或 `listen()` 前设置。

### `connect()` 返回 `EINVAL`

检查 client 角色、当前状态、端口和 host。host 当前必须是数值 IPv4，不能直接传域名。

### `listen()` 提示 TLS 证书不可用

默认内置证书会在 `listen()` 时自动加载。若提示不可用，检查运行环境是否支持 `memfd`、
是否挂载 `/proc`，以及 `/proc/self/fd` 是否可读。使用文件覆盖时，还要检查证书和私钥路径。

### GET/POST 路径无效

`remote_path` 必须采用类似 `/upload/1` 或 `/kkk.txt` 的 URL path 写法。开头的 `/` 表示
从服务端 `root_path` 开始解析，不表示操作系统根目录；即使访问 `root_path` 本目录下的文件
也必须保留这个 `/`。不能使用单独 `/`、含 `..` 或反斜杠的路径。POST 目标父目录必须在
服务端 `root_path` 内预先存在。

### 配置了多个接口但 `is_multipath()` 返回 0

多个接口只是配置目标。还需检查接口是否存在、地址和路由是否有效、至少两条路径是否通过验证。
以 `snapshot.active_path_count` 和 DEBUG 路径日志为准。

### `cleanup()` 返回 `EBUSY`

仍有 client、server 或 accepted handle 未 `netins_udp_close()`。

### 传输失败后能否继续复用连接

参数校验类错误且连接仍为 `CONNECTED` 时可修正参数后重试；超时、协议错误、transport close
会把状态置为 `ERROR`，此时应关闭并重新创建连接。
