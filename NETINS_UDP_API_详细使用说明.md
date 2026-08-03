# NetIns UDP Mini API 详细使用说明

本文是详细版手册；需要快速接入时，可先阅读
[`NETINS_UDP_API_使用说明.md`](NETINS_UDP_API_使用说明.md)。

本文面向集成 Mini API 的第三方 C/C++ 开发者，说明当前 API v1 的公开接口、连接建立、
GET/POST 文件收发、状态查询、错误处理和连接关闭流程。

公开接口以 [`include/xquic/netins_udp.h`](include/xquic/netins_udp.h) 为准。应用程序只需：

```c
#include <xquic/netins_udp.h>
```

首次接入建议按以下顺序阅读：

| 接入角色 | 建议先读 |
|---|---|
| 只开发客户端 | 第1、2、5、6、8、11、12、14节 |
| 只开发服务端 | 第1、2、5、7、9、11、12、14节 |
| 需要状态和运维统计 | 第3、4、10、16节 |
| 准备正式产品交付 | 第13、14、17、18节 |

第三方接入时应同时取得：

```text
include/xquic/netins_udp.h
目标架构对应的 libxquic.so 或 libxquic-static.a
动态/静态依赖库
本使用说明
客户端和服务端参考 demo
```

头文件、库和应用程序必须来自同一批交付版本。加载动态库后可通过
`netins_udp_get_api_version()` 检查运行库的 API 版本是否与头文件中的
`NETINS_UDP_API_VERSION` 一致。

## 1. API 能做什么

Mini API 是基于 QUIC/HTTP3 的阻塞式完整文件任务 API，不是 TCP socket 风格的字节流 API。

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

`remote_path` 表示 `root_path` 下的资源路径，但采用 URL path 写法，因此即使文件就在
`root_path` 本目录下也必须以 `/` 开头。例如 `root_path=/root/test_xquic_server` 时，
`/kkk.txt` 对应 `/root/test_xquic_server/kkk.txt`，不是操作系统根目录下的 `/kkk.txt`。
`remote_path` 不能只有 `/`；服务端拒绝空路径、`.`、`..`、反斜杠以及符号链接穿越。
POST 会创建或截断目标文件，但其父目录必须已经存在。

### 1.1 当前API的适用范围

当前 v1 适合以下场景：

- Linux/AARCH64 设备作为客户端，Linux 服务器作为文件网关；
- 一至八张网卡参与同一条 QUIC 多路径连接；
- 通过 GET 下载完整文件，或通过 POST 上传完整文件；
- 同一客户端连接上串行执行多个完整文件任务；
- 由专用工作线程承担阻塞式连接和文件任务；
- 需要查询连接、路径、RTT、字节和关闭原因统计。

当前 v1 不提供以下能力：

- 不提供 socket 风格的 `send()`、`recv()`、`read()`、`write()`；
- 不提供任意内存块、消息或流式业务数据接口；
- 不提供异步任务、进度回调或跨线程取消接口；
- 不提供断点续传、范围下载、暂停后继续或任务队列；
- 不支持直接传域名，连接地址必须是数值 IPv4；
- 不支持 IPv6；
- 一个客户端公开配置最多八张网卡；
- 服务端公开处理模型为串行 `accept → serve → close`；
- 不内置用户认证、目录权限、配额、限速或文件哈希接口。

因此，上层应用应把一个 `netins_udp_t` 理解为“一个由单线程独占的连接/监听对象”，把
一次 `netins_udp_transfer()` 理解为“一个完整文件任务”，而不是把它当成传统 socket。

### 1.2 最小客户端接入清单

客户端至少需要完成：

1. 调用 `netins_udp_init()`；
2. 用 `NETINS_UDP_ROLE_CLIENT` 创建 handle；
3. 配置一至八张网卡；
4. 按需要配置调度器、拥塞控制、SNI、超时和日志；
5. 调用 `netins_udp_connect()`；
6. 初始化 task/result 并调用 `netins_udp_transfer()`；
7. 检查返回值、分片数、字节数和业务文件；
8. 在失败时读取错误信息和关闭原因；
9. 调用 `netins_udp_shutdown()` 和 `netins_udp_close()`；
10. 最后调用 `netins_udp_cleanup()`。

### 1.3 最小服务端接入清单

服务端至少需要完成：

1. 调用 `netins_udp_init()`；
2. 用 `NETINS_UDP_ROLE_SERVER` 创建 listener handle；
3. 配置证书、私钥、调度器、超时和日志；
4. 调用 `netins_udp_listen()`；
5. 循环调用 `netins_udp_accept()`；
6. 为每个 accepted handle 初始化 `netins_udp_serve_config_t`；
7. 设置文件服务 `root_path` 并调用 `netins_udp_serve()`；
8. 查询错误/关闭原因后关闭 accepted handle；
9. 停止监听时关闭 listener；
10. 所有 handle 释放后调用 `netins_udp_cleanup()`。

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

### 3.6 逐函数调用说明

本节给出每个公开函数的调用前提、参数和返回后状态。代码中的最终函数声明仍以公开头文件为准。

#### `netins_udp_get_api_version()`

```c
uint32_t netins_udp_get_api_version(void);
```

- 不需要先调用 `netins_udp_init()`；
- 返回运行库支持的 API ABI 版本；
- 应与编译时头文件的 `NETINS_UDP_API_VERSION` 比较；
- 版本不一致时不要继续创建 handle。

```c
if (netins_udp_get_api_version() != NETINS_UDP_API_VERSION) {
    fprintf(stderr, "Mini API header/library mismatch\n");
    return -1;
}
```

#### `netins_udp_init()` / `netins_udp_cleanup()`

```c
int netins_udp_init(void);
int netins_udp_cleanup(void);
```

- 它们是进程级、带锁、引用计数接口；
- 每次成功 `init()` 都必须对应一次 `cleanup()`；
- 多个模块可以分别 init/cleanup，但必须保证自己的配对关系；
- 最后一份初始化引用清理时，如果仍有任何 handle，`cleanup()` 返回 `-1`，`errno=EBUSY`；
- 没有初始化引用时继续 cleanup，返回 `EINVAL`；
- `cleanup()` 失败不会替调用方释放 handle。

推荐由应用最外层统一管理，而不是在每次文件任务前后反复初始化。

#### `netins_udp_new()`

```c
netins_udp_t *netins_udp_new(int role);
```

允许的 role：

```text
NETINS_UDP_ROLE_CLIENT
NETINS_UDP_ROLE_SERVER
```

- 必须先成功调用 `netins_udp_init()`；
- 成功返回 `NEW` 状态 handle；
- 失败返回 `NULL`，常见 `errno` 为 `EPERM`（未初始化）、`EINVAL`（角色错误）、`ENOMEM`；
- 不能用它创建 `NETINS_UDP_ROLE_ACCEPTED`；
- 返回的每个非NULL handle 都必须且只能调用一次 `netins_udp_close()`。

#### `netins_udp_set_opt()`

```c
int netins_udp_set_opt(netins_udp_t *handle,
    int option, const void *value, size_t value_len);
```

- 成功返回0，失败返回-1；
- `handle`、`value` 和 `value_len` 必须与具体 option 匹配；
- 除 logger 外，调用时 handle 必须为 `NEW`；
- 配置接口只允许 CLIENT；connect timeout 只允许 CLIENT；accept timeout 只允许 SERVER；
- 未知 option 返回 `ENOPROTOOPT`；
- 在错误角色上使用选项通常返回 `EOPNOTSUPP`；
- 在 connect/listen 后修改非 logger 选项返回 `EBUSY`；
- 成功时 API 会复制配置内容，logger 的 `user_data` 仅保存指针。

每次 set_opt 都应检查返回值，不要把多个调用全部忽略后再到 connect/listen 阶段排查。

#### `netins_udp_connect()`

```c
int netins_udp_connect(netins_udp_t *client,
    const char *host, uint16_t port);
```

调用前提：

- handle 角色必须是 CLIENT；
- 状态必须是 `NEW`；
- `host` 必须是非空的数值 IPv4；
- `port` 必须为1～65535；
- 所有连接期配置必须已经完成。

调用过程中状态进入 `CONNECTING`。成功返回0并进入 `CONNECTED`，表示 QUIC/TLS/HTTP3
握手已经完成。失败返回-1并进入 `ERROR`，此 handle 不应再次 connect，应查询错误后 close。
建连等待上限来自 `NETINS_UDP_OPT_CONNECT_TIMEOUT_MS`。

#### `netins_udp_listen()`

```c
int netins_udp_listen(netins_udp_t *server,
    const char *host, uint16_t port);
```

调用前提：

- handle 角色必须是 SERVER；
- 状态必须是 `NEW`；
- `host` 必须是数值 IPv4；服务器通常使用 `0.0.0.0` 或指定本地地址；
- `port` 必须非0；
- 必须先通过 `NETINS_UDP_OPT_TLS` 配置证书和私钥。

成功后状态为 `LISTENING`。失败时状态进入 `ERROR`，读取错误信息后关闭并重新创建 listener。

#### `netins_udp_accept()`

```c
netins_udp_t *netins_udp_accept(netins_udp_t *server);
```

- 只能传入 `LISTENING` 状态的 SERVER handle；
- 阻塞等待时间来自 `NETINS_UDP_OPT_ACCEPT_TIMEOUT_MS`；0表示无限等待；
- 成功返回由调用方拥有的 `ROLE_ACCEPTED`、`CONNECTED` 状态 handle；
- 失败或超时返回 `NULL`，错误保存在 listener handle 上；
- accept 超时不等于 listener 必须销毁，上层可根据状态和错误策略决定是否继续 accept；
- accepted handle 继承 listener 的 backend、close timeout 和初始 logger；
- accepted handle 会保持共享 server backend 存活，即使 listener handle 先被关闭，它本身仍必须
  单独 close。

当前 listener 与其 accepted handle 共享事件循环，因此所有操作仍需串行，不要把每个 accepted
handle 直接分发给不同线程并发调用。

#### `netins_udp_transfer()`

```c
int netins_udp_transfer(netins_udp_t *client,
    const netins_udp_transfer_t *task,
    netins_udp_transfer_result_t *result);
```

调用前提：

- client 必须是 `CONNECTED` 状态 CLIENT；
- task 和 result 都必须使用对应初始化器；
- direction 只能是 GET 或 POST；
- `local_path` 必须非空且长度小于 `NETINS_UDP_FILE_PATH_LEN`；
- `remote_path` 必须以 `/` 开头、不能只有 `/`，长度小于
  `NETINS_UDP_REMOTE_PATH_LEN`；
- task 的两个字符串在阻塞调用返回前必须保持有效。

成功返回0，并填充完整 result。失败返回-1，result 可能包含已经完成的部分字节、分片和时间，
这些值只能用于诊断，不能当作成功结果。参数校验失败时连接通常仍可用；超时、I/O、协议错误
或底层连接关闭可能使 client 进入 `ERROR`。失败后先调用 `netins_udp_get_state()`，只有状态仍为
`CONNECTED` 时才可考虑修正参数后重试，否则应关闭并重建连接。

#### `netins_udp_serve()`

```c
int netins_udp_serve(netins_udp_t *connection,
    const netins_udp_serve_config_t *config);
```

- 只能传入 `CONNECTED` 状态的 ROLE_ACCEPTED handle；
- config 必须使用初始化器；
- `root_path` 必须非空、长度小于 `NETINS_UDP_FILE_PATH_LEN`，并指向可访问目录；
- `timeout_ms=0` 表示一直服务到对端关闭；
- 调用会配置文件根目录并阻塞驱动该连接的GET/POST请求；
- 正常对端关闭时返回0，accepted 状态为 `CLOSED`；
- 超时或连接错误返回-1，查询错误和关闭原因后释放 accepted handle；
- 一个 accepted handle 只对应一个连接，不应在 serve 返回后拿去接受另一连接。

#### `netins_udp_get_state()`

```c
int netins_udp_get_state(netins_udp_t *handle);
```

- 返回 `netins_udp_state_t`，不是普通0/-1结果；
- 调用会非阻塞处理一次内部事件，因此可能发现对端关闭并更新状态；
- 无效 handle 时返回 `NETINS_UDP_STATE_ERROR` 并设置 `errno=EINVAL`；
- 不能仅看到返回 `ERROR` 就假定 `errno` 是最新连接错误，应结合 last error 和 close reason。

#### `netins_udp_is_multipath()`

```c
int netins_udp_is_multipath(netins_udp_t *handle);
```

- 返回1：多路径模式开启且当前活动路径数大于1；
- 返回0：当前不满足上述条件；
- 返回-1：查询失败；
- 它反映运行时活动状态，不等同于“配置了多个接口”；
- 调用会先处理一次非阻塞内部事件。

#### `netins_udp_get_path_count()` / `netins_udp_get_path_statis()`

```c
int netins_udp_get_path_count(netins_udp_t *handle,
    uint32_t *out_count);
int netins_udp_get_path_statis(netins_udp_t *handle,
    size_t path_index, netins_udp_path_statis_t *out);
```

- `out_count` 必须非NULL；
- path output 必须使用 `NETINS_UDP_PATH_STATIS_INITIALIZER`；
- `path_index` 是0开始的快照数组下标，不是 `path_id`；
- 下标不存在时返回 `ENOENT`；
- listener 没有单连接数据面，通常返回0条路径；
- 路径可能在两次查询之间变化，若要求同一时刻一致性应使用一次 `get_snapshot()`。

#### `netins_udp_get_snapshot()`

```c
int netins_udp_get_snapshot(netins_udp_t *handle,
    netins_udp_snapshot_t *out);
```

- out 必须使用 `NETINS_UDP_SNAPSHOT_INITIALIZER`；
- 成功后一次返回角色、状态、握手、多路径、连接统计、路径数组和关闭原因；
- 快照是调用时副本，不会自动变化；
- 对 listener 查询时连接级和路径级数据通常为0；
- 快照查询会先非阻塞泵一次事件，状态可能在本次调用中改变。

#### `netins_udp_get_close_reason()`

```c
int netins_udp_get_close_reason(netins_udp_t *handle,
    netins_udp_close_reason_t *out);
```

- out 必须使用初始化器；
- 没有发生或请求关闭时 `source` 可以为 `NONE`；
- 本地 shutdown、对端关闭、transport 错误、超时和内部错误使用不同 source；
- 必须在 `netins_udp_close()` 前调用；
- 该结构描述“连接为什么关闭”，与最近一次普通参数错误不是同一个概念。

#### `netins_udp_shutdown()`

```c
int netins_udp_shutdown(netins_udp_t *handle);
```

- 可用于 CLIENT、SERVER listener 和 ACCEPTED；
- `CLOSED` 状态再次 shutdown 返回0；
- 客户端和 accepted 的协议关闭受 close timeout 限制；
- listener shutdown 停止服务端 backend；
- 成功后状态为 `CLOSED`；失败后为 `ERROR`；
- shutdown 不释放 handle，之后仍必须调用 close。

#### `netins_udp_close()`

```c
void netins_udp_close(netins_udp_t *handle);
```

- 最终释放 handle；
- 如果尚未 CLOSED，会先尽力执行一次 shutdown；
- 返回 `void`，因此需要关注关闭错误时必须先显式调用 shutdown；
- 返回后指针立即失效；
- 不能重复调用，不能再读取状态、错误、统计或关闭原因；
- 它不会替调用方自动执行全局 cleanup。

#### `netins_udp_get_last_error_code()` / `netins_udp_get_last_error_msg()`

```c
int netins_udp_get_last_error_code(netins_udp_t *handle);
int netins_udp_get_last_error_msg(netins_udp_t *handle,
    char *buffer, size_t buffer_len);
```

- 最近一次 handle 操作成功时，错误通常会被清零；
- code 为0表示当前没有保存的错误；
- message 建议使用 `NETINS_UDP_ERROR_MSG_LEN` 大小缓冲区；
- 缓冲区过小返回-1并设置 `errno=ENOBUFS`，不会静默截断；
- 错误码可能来自参数检查、POSIX系统调用或底层 backend，不应把所有非0值都直接传给
  `strerror()`；
- handle 为NULL或已经 close 后不能查询。

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

### 4.3 状态与允许操作

| 当前状态 | CLIENT主要允许操作 | SERVER主要允许操作 | ACCEPTED主要允许操作 |
|---|---|---|---|
| `NEW` | set_opt、connect、shutdown、close | set_opt、listen、shutdown、close | 不会以NEW创建 |
| `CONNECTING` | 正在 connect 内部使用 | 不适用 | 不适用 |
| `LISTENING` | 不适用 | accept、查询、shutdown、close | 不适用 |
| `CONNECTED` | transfer、查询、shutdown、close | 不适用 | serve、查询、shutdown、close |
| `CLOSING` | 等待当前关闭调用返回 | 等待当前关闭调用返回 | 等待当前关闭调用返回 |
| `CLOSED` | 查询、close | 查询、close | 查询、close |
| `ERROR` | 查询错误/快照/关闭原因、shutdown、close | 同左 | 同左 |

表中的“允许”仍以函数本身的角色校验为准。因为 handle 不是线程安全对象，应用不应在另一个
线程观察到 `CONNECTING` 或 `CLOSING` 后并发调用它。

典型客户端状态变化：

```text
NEW → CONNECTING → CONNECTED → CLOSING → CLOSED
                  └→ ERROR ─────────────→ close
```

典型 listener/accepted 状态变化：

```text
SERVER:   NEW → LISTENING → CLOSING → CLOSED
ACCEPTED:       CONNECTED → serve → CLOSED
                     └────→ ERROR → close
```

### 4.4 公开枚举类型速查

| 类型 | 用途 | 主要取值 |
|---|---|---|
| `netins_udp_result_t` | 通用结果常量 | `NETINS_UDP_OK`、`NETINS_UDP_ERROR` |
| `netins_udp_role_t` | handle角色 | `NONE`、`CLIENT`、`SERVER`、`ACCEPTED` |
| `netins_udp_state_t` | handle生命周期状态 | `NEW`～`ERROR` |
| `netins_udp_close_source_t` | 关闭来源 | `NONE`、`LOCAL`、`PEER`、`TRANSPORT`、`TIMEOUT`、`INTERNAL` |
| `netins_udp_path_state_t` | 传输层路径状态 | `INIT`、`VALIDATING`、`ACTIVE`、`CLOSING`、`CLOSED` |
| `netins_udp_app_path_status_t` | 应用层路径可用状态 | `NONE`、`STANDBY`、`AVAILABLE`、`FROZEN` |
| `netins_udp_log_level_t` | 日志等级 | `DEBUG=0`、`INFO=1`、`WARN=2`、`ERROR=3` |
| `netins_udp_scheduler_t` | 多路径调度器 | 正式接入建议显式使用 `ADAPTIVE`、`MINRTT` 或 `RAP` |
| `netins_udp_congestion_control_t` | 拥塞控制 | `DEFAULT`、`CUBIC`、`BBR` |
| `netins_udp_opt_t` | `set_opt()` 选项编号 | `INTERFACES`～`ZLOG` |
| `netins_udp_transfer_direction_t` | 文件任务方向 | `GET`、`POST` |

`ROLE_NONE` 主要用于未设置/输出默认值，不能传给 `netins_udp_new()`。枚举字段虽然在结构体中
以 `int` 保存，也应使用头文件枚举常量比较，不要依赖自己猜测的数字。

## 5. 配置接口

所有公开结构体都必须使用对应的 `*_INITIALIZER` 初始化。初始化器会设置
`struct_size` 和 `api_version`，API 用这两个字段检查 ABI：

```c
netins_udp_interfaces_t interfaces = NETINS_UDP_INTERFACES_INITIALIZER;
netins_udp_tls_config_t tls = NETINS_UDP_TLS_CONFIG_INITIALIZER;
netins_udp_logger_config_t logger = NETINS_UDP_LOGGER_CONFIG_INITIALIZER;
netins_udp_zlog_config_t zlog = NETINS_UDP_ZLOG_CONFIG_INITIALIZER;
netins_udp_transfer_t task = NETINS_UDP_TRANSFER_INITIALIZER;
netins_udp_transfer_result_t result =
    NETINS_UDP_TRANSFER_RESULT_INITIALIZER;
netins_udp_serve_config_t serve = NETINS_UDP_SERVE_CONFIG_INITIALIZER;
netins_udp_snapshot_t snapshot = NETINS_UDP_SNAPSHOT_INITIALIZER;
netins_udp_path_statis_t path = NETINS_UDP_PATH_STATIS_INITIALIZER;
netins_udp_close_reason_t reason = NETINS_UDP_CLOSE_REASON_INITIALIZER;
```

不要对结构体只做局部赋值，也不要手工猜测 `struct_size`。

所有结构体开头的 `struct_size` 和 `api_version` 均由初始化器填写；名称包含 `reserved` 的
字段由 API 保留，调用方不要修改。配置/任务结构体由调用方填写业务字段，结果/状态结构体则
只需初始化，具体内容由 API 回填。

### 5.1 公共常量、内存所有权和字符串规则

头文件中的长度和数量常量如下：

| 常量 | 当前值 | 用途 |
|---|---:|---|
| `NETINS_UDP_API_VERSION` | 1 | 当前公开 ABI 版本 |
| `NETINS_UDP_MAX_INTERFACES` | 8 | 一个客户端可配置的最大网卡数 |
| `NETINS_UDP_MAX_PATHS` | 8 | 快照结构能够保存的最大路径统计数 |
| `NETINS_UDP_INTERFACE_NAME_LEN` | 64 | 网卡名缓冲区长度，包含结尾 `\0` |
| `NETINS_UDP_ADDRESS_LEN` | 128 | 本地/对端地址文本缓冲区长度 |
| `NETINS_UDP_FILE_PATH_LEN` | 1024 | 本地文件、root、证书和zlog路径上限 |
| `NETINS_UDP_SERVER_NAME_LEN` | 256 | TLS `server_name` 缓冲区长度 |
| `NETINS_UDP_ZLOG_CATEGORY_LEN` | 64 | zlog category 缓冲区长度 |
| `NETINS_UDP_ERROR_MSG_LEN` | 256 | 错误和关闭说明缓冲区长度 |
| `NETINS_UDP_REMOTE_PATH_LEN` | 1024 | `remote_path` 长度上限 |

固定数组的有效字符串长度必须小于对应常量，因为最后一个字节要保存 `\0`。推荐使用
`snprintf()`，不要使用无边界的 `strcpy()`：

```c
if (snprintf(interfaces.names[0], sizeof(interfaces.names[0]),
        "%s", interface_name) >= (int)sizeof(interfaces.names[0])) {
    /* 名称过长 */
}
```

标量选项必须使用表中准确的C类型。不要把 `long`、`size_t` 或枚举变量地址在大小不匹配时
直接传入。调度器和拥塞控制虽然有枚举类型，当前接口要求以 `int` 对象传入：

```c
int scheduler = NETINS_UDP_SCHEDULER_MINRTT;
uint32_t close_timeout_ms = 3000;

netins_udp_set_opt(handle, NETINS_UDP_OPT_SCHEDULER,
    &scheduler, sizeof(scheduler));
netins_udp_set_opt(handle, NETINS_UDP_OPT_CLOSE_TIMEOUT_MS,
    &close_timeout_ms, sizeof(close_timeout_ms));
```

除 `NETINS_UDP_OPT_LOGGER` 外，所有选项都只能在 handle 为 `NEW` 状态时设置。accepted handle
创建时已经是 `CONNECTED`，会继承 listener 的 backend、close timeout 和 logger 配置；它不能
重新设置 TLS、调度器或 socket buffer，但可以单独替换 logger。listener 创建 accepted handle
之后再修改 listener logger，不会自动修改已经存在的 accepted handle。

socket buffer 的数值是向操作系统提出的请求值，内核可能按系统上下限调整；需要确认实际值时
应在目标平台通过系统工具或底层 socket 诊断验证，不能假设设置值就是最终内核值。

API 的数据复制和生命周期规则：

| 数据 | 是否由API复制 | 调用方需要保持多久 |
|---|---|---|
| `set_opt()` 的接口、TLS、zlog和标量配置 | 是 | `set_opt()` 返回后即可修改原变量 |
| logger 的 `callback` 函数地址 | 保存 | handle 存活且 logger 有效期间函数必须可调用 |
| logger 的 `user_data` | 只保存指针 | 直到替换 logger 或关闭 handle |
| `task.local_path` / `task.remote_path` | 任务期间读取 | 必须保持到 `transfer()` 返回 |
| `serve.root_path` | `serve()` 配置期间读取 | 必须保持到 `serve()` 返回 |
| `result`、`snapshot`、`path`、`reason` | API写入调用方对象 | 返回后归调用方读取，不含需释放的内部指针 |
| `netins_udp_t *` | API分配 | 调用方必须且只能 `close()` 一次 |

`netins_udp_close()` 返回后 handle 已经释放。不要重复关闭，也不要把它继续传给查询或错误接口。
输出结构体可以重复使用，但每次调用前都应重新赋值为对应的 `*_INITIALIZER`，避免把旧数据
误认为本次结果。

### 5.2 公开结构体字段与使用位置

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

客户端不使用 `certificate_file` 和 `private_key_file`；服务端必须在 `listen()` 前填写证书和
私钥。填写后传给 `NETINS_UDP_OPT_TLS`。

#### `netins_udp_logger_config_t`：API 日志回调（输入）

| 字段 | 调用方如何填写 | 含义 |
|---|---|---|
| `callback` | 日志回调函数，或 `NULL` | 同步接收 API/mini 运行日志；`NULL` 表示不接收 |
| `user_data` | 任意调用方指针 | API 调用回调时原样传回，常用于传递文件句柄或对象上下文 |
| `minimum_level` | `DEBUG/INFO/WARN/ERROR` | 只回调不低于该等级的日志 |

通过 `NETINS_UDP_OPT_LOGGER` 设置。它是唯一允许在活动 handle 上替换的选项；回调执行期间
不能重新调用当前 handle，`message` 只在本次回调期间有效。

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

### 5.3 `netins_udp_set_opt()` 选项

| option | `value` 类型 | 角色 | 默认/约束 |
|---|---|---|---|
| `NETINS_UDP_OPT_INTERFACES` | `netins_udp_interfaces_t` | CLIENT | 1～8 个非空网卡名；不设置时默认目标仍为双路径 |
| `NETINS_UDP_OPT_SCHEDULER` | `int`，值为 `netins_udp_scheduler_t` | CLIENT/SERVER | `DEFAULT` 表示沿用角色默认值；当前 client 为 BALANCED，server 为 ADAPTIVE |
| `NETINS_UDP_OPT_CONGESTION_CONTROL` | `int`，值为 `netins_udp_congestion_control_t` | CLIENT/SERVER | `DEFAULT` 当前解析为 BBR；也可显式设置 CUBIC/BBR |
| `NETINS_UDP_OPT_TLS` | `netins_udp_tls_config_t` | CLIENT/SERVER | client 只使用 SNI；server 必须设置证书和私钥 |
| `NETINS_UDP_OPT_CONNECT_TIMEOUT_MS` | `uint32_t` | CLIENT | 默认 10000 ms；不能为 0 |
| `NETINS_UDP_OPT_ACCEPT_TIMEOUT_MS` | `uint32_t` | SERVER | 默认 0，表示无限等待 |
| `NETINS_UDP_OPT_CLOSE_TIMEOUT_MS` | `uint32_t` | CLIENT/SERVER | 默认 3000 ms；0表示关闭等待不设期限 |
| `NETINS_UDP_OPT_KERNEL_SNDBUF` | 正 `int` | CLIENT/SERVER | UDP socket 发送缓冲区；不设置时使用系统/底层默认值 |
| `NETINS_UDP_OPT_KERNEL_RCVBUF` | 正 `int` | CLIENT/SERVER | UDP socket 接收缓冲区；不设置时使用系统/底层默认值 |
| `NETINS_UDP_OPT_LOGGER` | `netins_udp_logger_config_t` | 所有 handle | 默认 callback 为 NULL；唯一可在活动 handle 上替换的选项 |
| `NETINS_UDP_OPT_ZLOG` | `netins_udp_zlog_config_t` | CLIENT/SERVER | 默认关闭；必须在 connect/listen 前设置 |

`value_len` 必须传实际对象大小，例如：

```c
int cc = NETINS_UDP_CC_BBR;
if (netins_udp_set_opt(client, NETINS_UDP_OPT_CONGESTION_CONTROL,
        &cc, sizeof(cc)) != 0) {
    /* 处理错误 */
}
```

### 5.4 单路径和多路径

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

显式绑定网卡还要求运行进程有权执行目标系统的设备绑定操作。若普通用户下出现
`SO_BINDTODEVICE`/permission denied，应检查内核权限和进程 capabilities；生产部署建议授予
最小必要能力，不要仅为了绑定网卡长期以不受限制的 root 权限运行整个业务进程。

配置的每张网卡都应在 connect 前具备 IPv4 地址和到服务器的有效路由。API 可以在路径掉线后尝试
恢复，但不能替上层配置系统地址、策略路由、DNS或防火墙。

### 5.5 调度器与拥塞控制

第三方正式接入建议优先使用以下调度器：

```text
ADAPTIVE
MINRTT
RAP
```

`NETINS_UDP_SCHEDULER_DEFAULT` 表示不覆盖底层角色默认值，不等于固定的某一种算法：

- 客户端底层默认调度器当前为 `balanced`；
- 服务端底层默认调度器当前为 `adaptive`；
- 服务端 Demo 为了让行为明确，也将默认值显式设置为 `adaptive`。

如果第三方要求版本间行为稳定，不应依赖 `DEFAULT`，而应在 `connect()`/`listen()` 前显式
设置具体枚举。例如：

```c
int scheduler = NETINS_UDP_SCHEDULER_MINRTT;
if (netins_udp_set_opt(handle, NETINS_UDP_OPT_SCHEDULER,
        &scheduler, sizeof(scheduler)) != 0) {
    /* 处理错误 */
}
```

服务端 Demo 未传入 `--scheduler` 时使用 `adaptive`；如需指定，可传入
`--scheduler minrtt` 或 `--scheduler rap`。

拥塞控制可选值：

```text
NETINS_UDP_CC_DEFAULT
NETINS_UDP_CC_CUBIC
NETINS_UDP_CC_BBR
```

`NETINS_UDP_CC_DEFAULT` 当前在客户端和服务端都解析为 BBR。正式测试建议显式设置 BBR 或
CUBIC，避免后续底层默认值变化影响对比结果。

GET 的大流量发送端是服务端，POST 的大流量发送端是客户端。因此测试调度器和拥塞控制时，
要同时考虑数据发送端的配置，而不能只观察请求发起端。

### 5.6 TLS

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

必须理解当前 API v1 的 TLS 边界：

- 服务端证书和私钥用于建立加密 QUIC/TLS 连接；
- 客户端当前只公开 `server_name`，没有公开 CA 文件、系统信任库、证书固定或校验开关；
- 当前连接配置没有启用完整的服务端证书身份校验；
- `server_name` 当前主要用于 SNI，填写它不等于已经验证服务器身份；
- 当前不支持客户端证书，也不提供 mTLS；
- API 也没有内置 token、账号或业务鉴权回调。

因此，当前版本不应在完全不可信的公网环境中仅依赖此 TLS 配置承担身份认证。受控部署至少应
配合防火墙、VPN、专网、固定地址和上层访问控制。若作为通用第三方生产 SDK 交付，应先扩展
客户端证书校验以及服务端客户端认证能力。

### 5.7 日志

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

回调运行在触发日志的 API 调用线程上。日志处理应尽量短，不要在回调中执行长时间阻塞、
调用 `netins_udp_cleanup()`，或重新调用当前 handle。若需要写复杂日志系统，建议在回调中
复制 `message` 后投递给应用自己的队列。

`callback=NULL` 会抑制通过 Mini API 日志路由的常规 stdout；当前底层仍有少量系统错误路径
可能直接写 stderr。对宿主进程标准输出有严格要求时，还应在集成测试中验证错误路径，并由
进程管理层妥善接管 stdout/stderr。

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

即使运行时不启用 zlog，当前 Mini API 构建仍需要 zlog 头文件和库参与编译/链接；“默认关闭”
表示不会在运行时初始化或创建 zlog 文件。构建环境必须提供配置的 zlog 静态库；运行环境是否
还需要独立 `libzlog.so`，取决于最终交付库采用静态还是动态链接，应以 `ldd libxquic.so`
结果为准。

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

GET 会先获得远端文件总长度，再按 4 MiB 分片，最多保持 8 个并发分片任务。
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

POST 按 4 MiB 分片，最多保持 4 个并发分片任务。服务端目标父目录必须预先创建。

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

### 6.5 文件创建、覆盖和失败后的处理

GET 本地文件行为：

- `local_path` 可以是绝对路径，也可以相对于应用当前工作目录；
- 父目录必须存在并可写；
- 目标文件存在时会被截断覆盖；
- 成功返回前会等待异步写入队列完成并执行 `fsync()`；
- 传输失败时可能留下部分文件，不会自动恢复旧文件或删除失败文件。

POST 服务端文件行为：

- `local_path` 必须是客户端可读取的源文件；
- `remote_path` 的父目录必须在服务端 `root_path` 下已经存在并可写；
- 服务端直接创建或截断最终目标文件，并按总长度进行预分配/截断；
- 失败时可能留下部分内容，甚至可能出现“文件大小正确但部分区间尚未写完”的文件；
- 同一时间不要让多个客户端 POST 到同一个 `remote_path`；API v1 不提供文件锁或冲突检测。

当前 API 不执行临时文件加原子 rename，也不计算 SHA-256。需要生产级文件发布语义时，推荐
由业务层采用唯一临时 `remote_path`，传输完成后校验大小和哈希，再由服务器侧受控逻辑执行
原子改名。不要仅凭目标文件存在或文件大小正确就判定失败传输已经完整。

成功后至少检查：

```c
if (result.completed_fragments != result.total_fragments
    || result.total_bytes != expected_size
    || result.elapsed_us == 0) {
    /* 结果不满足业务验收要求 */
}
```

零字节文件是协议允许的特殊情况，此时不能套用 `total_bytes > 0` 的大文件吞吐验收条件。用于
带宽测试时应使用足够大的非空文件。

### 6.6 任务超时和连接复用判断

`task.timeout_ms` 只控制本次文件任务，和 connect、accept、serve、close timeout 相互独立。
设置为0表示本次任务无限等待。正式应用通常应设置有限值，并在超时后重建连接。

任务失败后的推荐处理：

```c
int state = netins_udp_get_state(client);
if (state == NETINS_UDP_STATE_CONNECTED) {
    /* 仅参数/远端业务错误等可恢复情况，按业务规则决定是否重试。 */
} else {
    /* 查询错误和关闭原因，然后 shutdown/close，创建新连接。 */
}
```

不要只根据 `errno` 判断是否复用；应同时检查 state、last error 和 close reason。也不要在一次
transfer 尚未返回时从另一个线程调用 shutdown 试图取消任务。

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

### 7.3 `root_path` 与远端路径映射

`root_path` 是服务端文件命名空间根目录。客户端看到的 `/` 不是操作系统根目录，而是这个
目录：

```text
root_path=/srv/mini-data
remote_path=/download/a.bin
实际文件=/srv/mini-data/download/a.bin
```

服务端开始 serve 时会规范化并打开 root 目录。请求路径按目录分量通过受限制的相对打开方式
解析，拒绝空分量、`.`、`..`、反斜杠和符号链接穿越。POST 只会创建最后一级文件，不会自动
创建中间目录：

```sh
mkdir -p /srv/mini-data/download
```

部署前应使用专用、最小权限的 root 目录，不建议直接把 `/`、用户 home 或包含敏感配置的目录
作为 `root_path`。运行服务端的系统用户只应拥有业务所需的读取和写入权限。

### 7.4 串行服务模型的部署含义

当前 listener、accepted handle 和 server backend 共用事件循环，一个 `serve()` 调用会一直
阻塞到该客户端关闭或超时。因此示例中的“最多处理N个连接”表示顺序处理N次，不表示同时
并发N个连接。

如果部署目标是“一块主板对应一个专用网关进程”，可以保持当前模型。如果一台服务器需要
同时服务多台设备，不应简单为每个 accepted handle 创建一个线程；应选择以下之一：

- 每个设备/租户启动独立服务进程和端口；
- 在API层增加统一的非阻塞事件调度和多连接管理；
- 等待后续公开的并发服务接口。

无论采用哪种方式，都应在进程外增加连接数、文件大小、磁盘空间和请求速率限制。API v1
本身不提供这些配额能力。

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
    int saved_errno = errno;
    char message[NETINS_UDP_ERROR_MSG_LEN] = {0};
    netins_udp_close_reason_t reason =
        NETINS_UDP_CLOSE_REASON_INITIALIZER;
    int api_code = saved_errno;

    if (handle != NULL) {
        api_code = netins_udp_get_last_error_code(handle);
        (void)netins_udp_get_last_error_msg(handle, message, sizeof(message));
        (void)netins_udp_get_close_reason(handle, &reason);
    }
    fprintf(stderr,
        "%s failed: errno=%d api=%d message=%s "
        "close_source=%d system=%d transport=%d close_message=%s\n",
        operation, saved_errno, api_code,
        message[0] != '\0' ? message : "unavailable",
        reason.source, reason.system_error, reason.transport_error,
        reason.message[0] != '\0' ? reason.message : "unavailable");
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
    if (result.completed_fragments != result.total_fragments) {
        fprintf(stderr, "incomplete fragments: %u/%u\n",
            result.completed_fragments, result.total_fragments);
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

    int scheduler = NETINS_UDP_SCHEDULER_ADAPTIVE;
    int cc = NETINS_UDP_CC_BBR;
    if (netins_udp_set_opt(server, NETINS_UDP_OPT_SCHEDULER,
            &scheduler, sizeof(scheduler)) != 0
        || netins_udp_set_opt(server, NETINS_UDP_OPT_CONGESTION_CONTROL,
            &cc, sizeof(cc)) != 0) {
        print_error(server, "configure server transport");
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
    int saved_errno = errno;
    char message[NETINS_UDP_ERROR_MSG_LEN] = {0};
    netins_udp_close_reason_t reason =
        NETINS_UDP_CLOSE_REASON_INITIALIZER;
    int api_code = saved_errno;

    if (handle != NULL) {
        api_code = netins_udp_get_last_error_code(handle);
        (void)netins_udp_get_last_error_msg(handle, message, sizeof(message));
        (void)netins_udp_get_close_reason(handle, &reason);
    }
    fprintf(stderr,
        "%s failed: errno=%d api=%d message=%s "
        "close_source=%d system=%d transport=%d close_message=%s\n",
        operation, saved_errno, api_code,
        message[0] != '\0' ? message : "unavailable",
        reason.source, reason.system_error, reason.transport_error,
        reason.message[0] != '\0' ? reason.message : "unavailable");
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

    if (netins_udp_get_api_version() != NETINS_UDP_API_VERSION) {
        fprintf(stderr, "Mini API version mismatch\n");
        goto done;
    }

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

API 内部在没有待处理事件时使用阻塞等待，不再进行固定 1 ms 轮询；显式状态查询只执行一次
非阻塞状态更新。

## 13. 固定传输语义

为保持与当前 mini 数据面一致，下列参数固定在库内部，API v1 不开放修改：

- 文件分片：4 MiB；
- GET 并发 request：8；
- POST 并发 request：4；
- GET 先通过响应头获取总文件长度；
- 使用 offset `pread()`/`pwrite()`；
- 完成一个分片任务后自动补充下一个分片；
- GET 返回前等待异步写队列和 `fsync()`；
- 0 字节文件合法；
- 任务超时会关闭/终止连接，避免迟到的分片任务继续写文件。

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

### 14.1 交付目录示例

推荐第三方交付包按架构分开，不要把 x86_64 和 AARCH64 库放在同一个无标识目录：

```text
mini-api-sdk/
├── include/xquic/netins_udp.h
├── lib/aarch64/libxquic.so
├── lib/x86_64/libxquic.so
├── bin/aarch64/netins_udp_mini_api_client
├── bin/x86_64/netins_udp_mini_api_server
├── NETINS_UDP_API_使用说明.md
├── NETINS_UDP_编译指南.md
├── RELEASE_NOTES.md
└── VERSION
```

证书、私钥、SSH凭据和大型测试文件不属于SDK公共文件，不应打包进源码或公共交付物。

### 14.2 第三方CMake接入示例

假设调用方通过 `-DNETINS_UDP_SDK_ROOT=/opt/mini-api-sdk` 指定SDK目录：

```cmake
cmake_minimum_required(VERSION 3.16)
project(mini_api_consumer C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT DEFINED NETINS_UDP_SDK_ROOT)
    message(FATAL_ERROR "Set NETINS_UDP_SDK_ROOT")
endif()

find_path(NETINS_UDP_INCLUDE_DIR
    NAMES xquic/netins_udp.h
    PATHS "${NETINS_UDP_SDK_ROOT}/include"
    NO_DEFAULT_PATH)

find_library(NETINS_UDP_XQUIC_LIBRARY
    NAMES xquic
    PATHS "${NETINS_UDP_SDK_ROOT}/lib"
    NO_DEFAULT_PATH)

if(NOT NETINS_UDP_INCLUDE_DIR OR NOT NETINS_UDP_XQUIC_LIBRARY)
    message(FATAL_ERROR "Mini API header or shared library not found")
endif()

add_executable(my_mini_client main.c)
target_include_directories(my_mini_client PRIVATE
    "${NETINS_UDP_INCLUDE_DIR}")
target_link_libraries(my_mini_client PRIVATE
    "${NETINS_UDP_XQUIC_LIBRARY}")
```

如果交付包按架构使用 `lib/aarch64`、`lib/x86_64`，应由工具链或上层构建参数把正确架构目录
传给 `find_library()`，不能在目标机上尝试链接另一架构的库。

### 14.3 C++调用

公开头文件已经包含 `extern "C"` 保护，可以直接从 C++ 包含：

```cpp
#include <xquic/netins_udp.h>
```

API 不抛 C++ 异常，也不会接管 C++ 对象生命周期。建议使用一个 RAII 包装类在析构时执行
`shutdown/close`，但仍需在进程级单独管理 `init/cleanup` 引用。

### 14.4 运行时动态库检查

部署到目标机后先检查架构和依赖：

```sh
file ./my_mini_client
file /opt/mini-api-sdk/lib/libxquic.so
ldd ./my_mini_client
ldd /opt/mini-api-sdk/lib/libxquic.so
```

如果提示找不到 `libxquic.so` 或依赖库，可在测试阶段设置 `LD_LIBRARY_PATH`。正式部署更推荐
把库安装到受控系统目录、使用启动脚本设置专用环境，或在应用链接时设置正确的 rpath；不要
依赖某个开发用户 shell 中临时配置的环境变量。

### 14.5 静态库注意事项

`libxquic-static.a` 包含 Mini API 对象，但静态库不会自动携带依赖链接顺序。第三方需要使用与
SDK相同架构、ABI和TLS实现的依赖库。出现 `undefined reference to zlog_init`、TLS符号或
Libevent符号时，通常不是 API 头文件问题，而是依赖库缺失、顺序错误或架构不匹配。

正式SDK最好提供经过验证的 consumer CMake 示例或完整 link response file，并记录每个依赖的
版本、架构、许可证和SHA-256。

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
| `--log-level` | callback 与 zlog 的 `minimum_level`，0～3 |
| `--zlog-config`/`--zlog-category` | `NETINS_UDP_OPT_ZLOG`；不传时 zlog 关闭 |
| server `--cert`/`--key` | server `NETINS_UDP_OPT_TLS` |
| server `--root` | `serve.root_path` |
| server `--scheduler` | server `NETINS_UDP_OPT_SCHEDULER`；默认 adaptive |
| server `--accept-timeout-ms` | `NETINS_UDP_OPT_ACCEPT_TIMEOUT_MS` |
| server `--serve-timeout-ms` | `serve.timeout_ms` |

## 16. 常见错误

常见 `errno`/错误码含义：

| 错误 | 常见原因 | 建议处理 |
|---|---|---|
| `EINVAL` | 空指针、错误角色/状态、IPv4/端口/路径/结构体版本无效 | 检查调用前提和初始化器 |
| `EPERM` | `netins_udp_new()` 前没有 init | 先完成进程级初始化 |
| `EBUSY` | 活动状态修改选项；最终 cleanup 时仍有handle；zlog配置冲突 | 按生命周期设置并释放资源 |
| `EOPNOTSUPP` | 在错误角色设置选项；客户端配置证书；平台不支持 | 移除不支持的配置或调整角色 |
| `ENOPROTOOPT` | 未知 option | 检查头文件和库版本 |
| `ETIMEDOUT` | connect/accept/transfer/serve/shutdown 超时 | 查询状态和关闭原因，通常重建连接 |
| `ENOENT` | 路径/文件/索引/category不存在 | 检查服务端root、文件和配置 |
| `ENOBUFS` | 错误文本缓冲区过小 | 使用 `NETINS_UDP_ERROR_MSG_LEN` |
| `EIO` / `EPROTO` | 本地I/O或协议层错误 | 保存日志和快照，关闭并重建连接 |
| `ECONNRESET` | 对端或传输层异常关闭 | 查询 transport error，重新连接 |

底层 backend/transport 错误有时也会作为正整数保存到 last error。只有确认它属于POSIX errno
时才应使用 `strerror(code)`；通用日志应同时打印数字、last error message 和 close reason。

推荐错误打印函数：

```c
static void
print_netins_error(netins_udp_t *handle, const char *operation)
{
    int saved_errno = errno;
    char message[NETINS_UDP_ERROR_MSG_LEN] = {0};
    netins_udp_close_reason_t reason =
        NETINS_UDP_CLOSE_REASON_INITIALIZER;

    int api_code = handle != NULL
        ? netins_udp_get_last_error_code(handle) : saved_errno;
    if (handle != NULL) {
        (void)netins_udp_get_last_error_msg(handle,
            message, sizeof(message));
        (void)netins_udp_get_close_reason(handle, &reason);
    }

    fprintf(stderr,
        "%s failed errno=%d api=%d message=%s "
        "close_source=%d system=%d transport=%d close_message=%s\n",
        operation, saved_errno, api_code,
        message[0] != '\0' ? message : "-",
        reason.source, reason.system_error, reason.transport_error,
        reason.message[0] != '\0' ? reason.message : "-");
}
```

必须在 `netins_udp_close()` 前调用该函数；close 后 handle 已失效。

### `netins_udp_new()` 返回 NULL，`errno=EPERM`

没有先成功调用 `netins_udp_init()`。

### `set_opt()` 返回 `EBUSY`

除 logger 外，选项必须在 `connect()` 或 `listen()` 前设置。

### `connect()` 返回 `EINVAL`

检查 client 角色、当前状态、端口和 host。host 当前必须是数值 IPv4，不能直接传域名。

### `listen()` 提示必须配置 TLS

服务端必须在 listen 前通过 `NETINS_UDP_OPT_TLS` 设置可读的证书和私钥路径。

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

### `get_state()` 返回 `ERROR`，但函数没有返回-1

`get_state()` 的返回值本身就是状态枚举。无效 handle 和真实连接错误都可能看到
`NETINS_UDP_STATE_ERROR`；应结合 `errno`、last error 和 close reason 分析。

### `get_path_count()` 返回0

在NEW状态、listener上或连接尚未形成路径统计时可能为0。客户端应在 connect 成功后查询；
accepted 应在握手/serve期间查询。需要判断实际聚合时还要看 `active_path_count`，不能只看总数。

### 设置两个接口但只有一条活动路径

依次检查网卡是否UP、是否有IPv4、是否有到对端的源地址路由、是否能绑定设备、服务端是否
支持多路径以及第二路径是否验证成功。配置数量表示目标，不保证物理链路一定可用。

### GET失败后为什么本地文件还在

GET直接创建/截断 `local_path`，失败时不会自动删除部分文件。业务层应删除失败文件，或先下载
到临时名称，成功校验后再原子改名。

### POST失败后为什么服务器文件大小看起来正确

服务端会先按总长度创建/截断目标，失败时可能留下预分配文件。文件大小正确不代表所有分片
都完成，应检查客户端结果、服务端日志并进行哈希校验。

### zlog启用后 connect/listen 失败

检查 `config_file` 是否存在可读、category 是否在 zlog 配置中定义。同一进程已有活动 handle
使用另一个 zlog 配置文件时会冲突；所有活动 handle 应使用同一个配置文件。

### 未设置日志回调但仍看到stderr

callback=NULL 会抑制常规 Mini API 日志路由，但少量底层系统错误路径仍可能直接写 stderr。
宿主程序应接管标准错误，并在交付前覆盖异常路径测试。

### 能否用另一个线程取消transfer

不能。当前 handle 不支持并发调用，也没有 cancel API。应给任务设置有限 timeout，并由一个
工作线程独占整个 handle 生命周期。

### 能否同时服务多个客户端

当前公开服务模型不是并发 accepted-handle API。默认示例是串行处理。需要并发时使用进程隔离
或扩展统一事件调度，不能简单对共享 backend 多线程调用。

### 是否可以只配置 `server_name` 就完成服务器身份校验

不可以。当前 v1 的 `server_name` 主要用于SNI，API没有公开CA/证书固定和验证开关。部署到
不可信网络前必须增加身份校验能力，或通过受控专网/VPN和上层认证降低风险。

## 17. 生产部署边界和安全注意事项

当前API已经提供传输加密、路径约束和完整文件任务，但第三方正式生产部署仍需由应用或网关
补齐以下能力。

### 17.1 身份认证和访问控制

- 客户端当前没有完整服务器证书验证入口；
- 服务端没有客户端证书、token或认证回调；
- 没有按用户、设备或目录区分GET/POST权限；
- 只要能访问UDP端口，连接方就可能请求root目录内允许解析的文件。

在扩展API前，至少使用防火墙、专网/VPN、固定源地址、独立端口/进程和最小权限root目录。
这些外部措施不能替代最终的证书验证和业务鉴权，但可以界定受控试生产范围。

### 17.2 文件和磁盘保护

- 设置最大允许文件大小；
- POST前检查磁盘剩余空间和配额；
- 避免多个连接写同一路径；
- 使用临时文件、哈希和原子改名；
- 失败后清理临时/部分文件；
- 不把系统目录、证书目录或用户home作为root；
- 服务进程使用非root、最小权限账号运行。

API v1本身不限制上传总量。正式网关如果不做配额和磁盘保护，恶意或错误客户端可能造成磁盘
耗尽。

### 17.3 资源和可用性保护

- 在进程外限制连接数、进程数和UDP访问来源；
- 为 connect、transfer、serve、close 设置符合业务的有限超时；
- 对失败率、PTO、活动路径数、磁盘空间、RSS和CPU建立监控；
- 保存错误码、关闭原因和必要的原始日志；
- 对断网、单路径掉线、磁盘满、权限错误、服务器重启和客户端强杀做故障测试；
- 对AARCH64客户端和x86_64服务器分别进行长稳和资源泄漏测试。

### 17.4 API兼容性

当前 ABI 版本为1。第三方不应：

- 复制或推测 opaque handle 内部布局；
- 手工填写 `struct_size`；
- 修改 reserved 字段；
- 使用未在交付说明中承诺的内部符号；
- 混用不同批次头文件和动态库；
- 依赖当前未承诺永久不变的 role DEFAULT 行为。

升级库时至少执行 API version、导出符号、结构体、客户端/服务端互通和性能回归检查。

## 18. 第三方接入验收清单

### 18.1 编译前

- [ ] 取得目标架构正确的头文件、共享/静态库和依赖；
- [ ] 核对 `NETINS_UDP_API_VERSION`；
- [ ] 明确客户端和服务端各自架构、编译器ABI和TLS实现；
- [ ] 准备服务端证书、私钥和专用root目录；
- [ ] 明确一张/两张网卡的接口名、地址和路由；
- [ ] 明确调度器、拥塞控制和所有超时值。

### 18.2 客户端功能

- [ ] 单网卡GET成功，字节/分片/文件校验通过；
- [ ] 单网卡POST成功，服务端文件校验通过；
- [ ] 双网卡GET/POST成功，活动路径数符合预期；
- [ ] 同一连接顺序执行多个任务成功；
- [ ] 一张网卡中断时符合恢复预期；
- [ ] 超时和远端文件不存在时能够正确清理/重建连接；
- [ ] 失败GET不会被业务误认为完整文件。

### 18.3 服务端功能

- [ ] listener绑定地址和UDP端口正确；
- [ ] 证书/私钥权限和加载正常；
- [ ] root路径映射正确，`..`和符号链接穿越被拒绝；
- [ ] POST父目录、磁盘空间和权限正确；
- [ ] accepted handle和listener均被正确关闭；
- [ ] 串行连接容量满足当前部署模型，或已经采用进程隔离。

### 18.4 稳定性和资源

- [ ] 完成目标轮数GET/POST压力测试；
- [ ] 完成长时间稳定性测试；
- [ ] 检查客户端和服务端CPU、RSS、HWM及增长趋势；
- [ ] 检查文件描述符、线程和磁盘文件是否泄漏；
- [ ] 覆盖断网、服务器退出、磁盘满、权限错误和进程强杀；
- [ ] 保存版本号、构建参数、库SHA-256和原始测试结果。

### 18.5 正式上线前

- [ ] 已明确当前TLS身份校验和业务鉴权边界；
- [ ] 已配置防火墙/VPN/专网或完成API安全扩展；
- [ ] 已设置文件大小、磁盘配额、连接和速率限制；
- [ ] 已采用临时文件、哈希和原子发布策略；
- [ ] 已明确升级、回滚、日志留存和故障恢复方案。
