# XQUIC Mini 客户端与服务器开发指南

本指南介绍如何构建、配置、运行和集成当前 `mini` 源码包中的 XQUIC `mini_client` 与 `mini_server`。

> **本代码快照的范围**
>
> - 基于 XQUIC 的 HTTP/3
> - 客户端最多可使用两个网络接口，并维护两条活跃的 QUIC 路径
> - 支持 GET 下载与 POST 上传
> - 支持基于文件区间（range）的多流传输
> - 多路径调度器配置在数据发送端
> - 客户端支持 BBR 或 CUBIC 拥塞控制；服务器当前默认使用 BBR
> - 基于 Libevent 的引擎事件循环
>
> 当前代码属于科研开发版本。虽然其他程序可以调用其导出的 C 函数，但这些接口尚未封装为稳定且具备版本管理能力的 SDK。

---

## 目录

1. [系统架构](#1-系统架构)
2. [源码目录结构](#2-源码目录结构)
3. [依赖与构建](#3-依赖与构建)
4. [证书与运行时文件](#4-证书与运行时文件)
5. [客户端配置](#5-客户端配置)
6. [服务器配置](#6-服务器配置)
7. [运行服务器与客户端](#7-运行服务器与客户端)
8. [上传与下载流程](#8-上传与下载流程)
9. [调度器与拥塞控制选择](#9-调度器与拥塞控制选择)
10. [在其他应用程序中使用本代码](#10-在其他应用程序中使用本代码)
11. [HTTP/3 应用层协议](#11-http3-应用层协议)
12. [线程与回调规则](#12-线程与回调规则)
13. [日志、统计与验证](#13-日志统计与验证)
14. [故障排查](#14-故障排查)
15. [建议的生产级 API](#15-建议的生产级-api)

---

## 1. 系统架构

### 1.1 组成模块

```text
嵌入式设备                                           云端服务器
┌──────────────────────────────────┐                 ┌──────────────────────────────┐
│ 应用程序                          │                 │ 应用程序/文件服务             │
│  ├─ 上传数据源                    │                 │  ├─ GET 文件数据源             │
│  └─ 下载数据接收端                │                 │  └─ POST 上传写入端            │
├──────────────────────────────────┤                 ├──────────────────────────────┤
│ mini_client HTTP/3 回调           │                 │ mini_server HTTP/3 回调        │
├──────────────────────────────────┤                 ├──────────────────────────────┤
│ XQUIC 引擎                        │  MPQUIC/HTTP3   │ XQUIC 引擎                     │
├──────────────────────────────────┤◄───────────────►├──────────────────────────────┤
│ UDP 套接字 A ─ 5G 运营商 A        │                 │ UDP 监听套接字                 │
│ UDP 套接字 B ─ 5G 运营商 B        │                 │                              │
└──────────────────────────────────┘                 └──────────────────────────────┘
```

客户端将每个 UDP 套接字绑定到配置的网络接口。XQUIC 负责创建并维护与这些接口对应的多路径 QUIC 路径。

### 1.2 不同传输方向的控制端

真正决定大批量数据如何发送的调度器和拥塞控制器位于**应用数据发送端**。

| 操作 | 数据发送端 | 控制大流量数据的调度器/拥塞控制 |
|---|---|---|
| POST 上传 | `mini_client` | 客户端调度器与客户端拥塞控制 |
| GET 下载 | `mini_server` | 服务器调度器与服务器拥塞控制 |

仅修改客户端调度器，通常不会改变服务器向客户端分配 GET 数据的方式。进行下载实验时，应配置或修改服务器端调度器。

### 1.3 传输分段

实现会将文件划分为固定大小的逻辑分段：

```text
segment_offset = stream_index × stream_range_size
segment_length = min(stream_range_size, total_size - segment_offset)
```

- 对于 POST，客户端预先知道源文件大小，并在传输前计算分段流的总数。
- 对于 GET，客户端先发起一个请求，通过服务器返回的 `x-total-length` 响应头获知完整文件长度，再继续调度其余区间请求。
- `stream_cnt` 表示同时活跃的 HTTP/3 请求上限，不一定等于分段总数。

---

## 2. 源码目录结构


```text
XQUIC/
├── third_party/
│   └── Tongsuo-8.3.0/             
└── xquic-1.8.3/
    ├── CMakeLists.txt            
    ├── demo/                      
    ├── tests/                    
    ├── mini/
    ├── cmake/
    ├── src/           
    └── build/                     
        └── mini/
            ├── mini_client        
```
本项目主要关注 `mini` 目录中的源码：
```text
mini/
├── CMakeLists.txt
├── client_config.txt
├── server_config.txt
├── common.c
├── common.h
├── mini_client.c
├── mini_client.h
├── mini_client_cb.c
├── mini_client_cb.h
├── mini_client_main.c
├── mini_server.c
├── mini_server.h
├── mini_server_cb.c
├── mini_server_cb.h
├── mini_server_main.c
└── get1GBfile.py
```

主要文件及其职责如下。

| 文件 | 主要职责 |
|---|---|
| `mini_client_main.c` | 客户端进程生命周期 |
| `mini_client.c` | 配置、路径与套接字管理、客户端引擎初始化、请求创建及上传数据源处理 |
| `mini_client_cb.c` | 客户端 XQUIC/HTTP/3 回调、下载数据接收与请求完成处理 |
| `mini_server_main.c` | 服务器进程生命周期 |
| `mini_server.c` | 服务器配置、套接字、引擎、调度器与拥塞控制初始化 |
| `mini_server_cb.c` | 服务器回调、GET 响应生成与 POST 数据接收 |
| `common.h/.c` | 共用枚举、协议头与文件辅助函数 |
| `client_config.txt` | 从当前工作目录读取的客户端运行时配置 |
| `server_config.txt` | 从当前工作目录读取的服务器运行时配置 |


---

## 3. 依赖与构建

### 3.1 必需组件

- XQUIC 源码及静态库目标 `xquic-static`
- CMake
- 支持 C11 的 C 编译器
- Libevent 及其 pthread 支持
- pthread
- 所选 XQUIC 构建方式所需的 TLS/密码学依赖


### 3.2 将目录集成到 XQUIC

将本目录放到 XQUIC 源码根目录下：

```text
xquic-1.8.3/
├── CMakeLists.txt
├── demo/
├── tests/
├── mini/                 # 本代码包
└── ...
```


### 3.3 本机编译

在 `XQUIC/xquic-1.8.3/build` 目录中执行：

```bash
make -j
```

生成的可执行文件位于 `build/mini/` 目录，分别为 `mini_client` 和 `mini_server`。


### 3.4 为 ARM 设备交叉编译

使用项目的 ARM 工具链文件：

```bash
cmake -S . -B build-arm \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=/path/to/aarch64-toolchain.cmake
cmake --build build-arm --target mini_client -j
```




## 4. 证书与运行时文件

### 4.1 服务器证书文件

若未在代码中另行指定，服务器会在当前工作目录中查找以下文件：

```text
server.crt
server.key
```

开发环境中可生成自签名证书：

```bash
openssl req -x509 -newkey rsa:2048 -nodes \
  -keyout server.key \
  -out server.crt \
  -days 365 \
  -subj "/CN=test.xquic.com"
```

客户端目前默认将 `test.xquic.com` 写入 HTTP `Host` 请求头。

### 4.2 可选的会话恢复文件

代码可能读取或创建：

```text
session_ticket
transport_params
token
session_ticket.key
```

即使没有有效的 `session_ticket.key`，服务器仍可运行，但无法使用 0-RTT 或会话恢复功能。

### 4.3 日志文件

默认文件名包括：

```text
clog.log
ckeys.log
slog.log
skeys.log
```

启用 zlog 时，必须提供 `zlog.conf`，并在其中定义相应的日志类别。

---

## 5. 客户端配置

客户端从**当前工作目录**读取 `client_config.txt`。

> 代码中已经实现命令行解析器，但 `mini_client_main.c` 对 `xqc_mini_cli_parse_cmd_args()` 的调用目前已被注释。运行时应修改 `client_config.txt`，或在创建引擎前通过代码设置相关字段。

### 5.1 客户端支持的配置项

| 配置项 | 取值 | 含义 |
|---|---|---|
| `server_addr` | IPv4 地址 | 云端服务器地址 |
| `server_port` | `1..65535` | UDP 监听端口 |
| `method` | `GET` 或 `POST` | 下载或上传 |
| `stream_cnt` | 正整数，最大 16 | 最大并发 HTTP/3 请求数 |
| `send_data_len` | 字节数；`0` 表示完整文件 | 限制 POST 上传长度 |
| `cc` | `bbr`、`cubic` | 客户端拥塞控制器 |
| `mp_sched` | `minrtt`、`backup`、`balanced`、`rap`、`act`、`bw` | 客户端调度器 |
| `interfaces` | 逗号分隔的接口名 | 最多两个客户端接口 |
| `kernel_sndbuf` | 字节数 | 申请设置的 UDP 套接字发送缓冲区大小 |
| `kernel_revbuf` | 字节数 | 申请设置的 UDP 套接字接收缓冲区大小；拼写与代码一致 |
| `user_send_buf_size` | 字节数 | 应用上传读取/发送分块大小 |
| `user_recv_buf_size` | 字节数 | 用户态接收配置值 |
| `download_target` | HTTP 路径 | GET 请求的远端文件路径 |
| `download_path` | 本地路径 | GET 下载文件的保存路径 |
| `send_file` | 本地路径 | `upload_path` 的别名 |
| `use_zlog` | `0/1`、`false/true` | 是否启用 zlog 回调 |


未知配置项和格式有误的行会被忽略。

### 5.2 双路径上传示例

```ini
server_addr=124.70.219.194
server_port=8443

# request settings
method=POST
stream_cnt=4
#send_data_len=1048576000

# algorithm settings
cc=bbr
mp_sched=rap

# interface list (comma separated, up to MAX_PATH_CNT)
interfaces=rmnet_data0,usb0

# kernel socket buffers (bytes)
kernel_sndbuf=16777216
kernel_revbuf=16777216

# user-space buffers (bytes)
user_send_buf_size=32768
user_recv_buf_size=32768

#file settings
send_file=./download/kkk.txt
download_path=./download/kkk.txt
download_target=/upload/ccc.bin

use_zlog=0
```

### 5.3 双路径下载示例

```ini
server_addr=124.70.219.194
server_port=8443

# request settings
method=GET
stream_cnt=8
#send_data_len=1048576000

# algorithm settings
cc=cubic
mp_sched=rap

# interface list (comma separated, up to MAX_PATH_CNT)
interfaces=usb0

# kernel socket buffers (bytes)
kernel_sndbuf=16777216
kernel_revbuf=16777216

# user-space buffers (bytes)
user_send_buf_size=8192
user_recv_buf_size=65536

#file settings
send_file=./download/kkk.txt
download_path=./download/kkk.txt
download_target=/upload/ccc.bin

use_zlog=0
```

### 5.4 单路径基准

```ini
interfaces=usb0
```

将其依次设置为各个 5G 接口，并分别进行测试。

### 5.5 网络接口要求

每个配置的接口必须：

- 已存在并处于启用状态
- 配有可用的 IPv4 地址
- 具备通往服务器的路由
- 允许当前进程通过 `SO_BINDTODEVICE` 绑定


检查接口状态：

```bash
ip -br addr
ip route
```

---

## 6. 服务器配置

服务器从**当前工作目录**读取 `server_config.txt`。

> 代码中已经实现命令行解析器，但 `mini_server_main.c` 对 `xqc_mini_svr_parse_cmd_args()` 的调用目前已被注释。

### 6.1 服务器支持的配置项

| 配置项 | 取值 | 含义 |
|---|---|---|
| `listen_addr` | IPv4 地址或 `0.0.0.0` | UDP 绑定地址 |
| `listen_port` | `1..65535` | UDP 绑定端口 |
| `data_dir` | 目录 | GET 文件的根目录 |
| `default_send_file` | 文件名 | 请求路径为 `/` 时使用的 GET 文件 |
| `stream_range_size` | 字节数 | 服务器端预期分段大小 |
| `mp_sched` | 调度器名称 | 服务器多路径调度器 |
| `kernel_sndbuf` | 字节数 | UDP 套接字发送缓冲区 |
| `kernel_revbuf` | 字节数 | UDP 套接字接收缓冲区；拼写与代码一致 |
| `user_send_buf_size` | 字节数 | 用户态发送设置 |
| `user_recv_buf_size` | 字节数 | 用户态接收设置 |
| `upload_path` | 路径 | POST 上传数据的目标文件 |
| `use_zlog` | `0/1`、`false/true` | 是否启用 zlog 回调 |

### 6.2 服务器调度器支持情况

当前服务器回调选择器对名称的映射如下：

| 名称 | 实际回调 |
|---|---|
| `minrtt` | `xqc_minrtt_scheduler_cb` |
| `backup` | `xqc_backup_scheduler_cb` |
| `act` | `xqc_act_scheduler_cb` |
| `bw` | `xqc_bandwidth_scheduler_cb` |

服务器当前的默认配置仅启用 `minrtt`。

### 6.3 服务器拥塞控制

服务器当前初始化为：

```c
args->quic_cfg.cc = CC_TYPE_BBR;
```

服务器配置解析器目前**不会解析** `cc=`。因此，除非应用程序在引擎初始化前通过代码修改 `args->quic_cfg.cc`，或扩展配置解析器，否则服务器发送 GET 数据时将使用 BBR。

### 6.4 服务器示例

```ini
listen_addr=0.0.0.0
listen_port=8443

# data directory and default file
data_dir=.
default_send_file=hhh.bin

mp_sched=minrtt

# kernel socket buffers (bytes)
kernel_sndbuf=8192
kernel_revbuf=16777216

# user-space buffers (bytes)
user_send_buf_size=32768
user_recv_buf_size=32768

upload_path=./upload/ccc.bin

use_zlog=0
```


### 6.5 防火墙

开放所配置的 UDP 端口，例如：

```bash
sudo ufw allow 8443/udp
```

若服务器部署在云平台，还需在安全组或网络防火墙中放行该 UDP 端口。

---

## 7. 运行服务器与客户端

### 7.1 准备服务器文件

```bash
mkdir -p /srv/XQUIC
fallocate -l 1G /srv/XQUIC/server_data.bin
```

也可以使用随附的生成脚本，但应根据当前构建目录修正其输出路径。

### 7.2 启动服务器

从包含下列文件的目录运行：

- `server_config.txt`
- `server.crt`
- `server.key`
- 可选的 `zlog.conf` 和会话票据文件

```bash
./mini_server
```

预期启动状态：

- UDP 套接字已绑定到指定地址和端口
- XQUIC 服务器引擎初始化成功
- HTTP/3 回调注册完成
- 已为接入的连接启用多路径功能

### 7.3 启动客户端

从包含 `client_config.txt` 及相应上传源文件的目录运行：

```bash
sudo ./mini_client
```

客户端将依次：

1. 初始化第一个网络接口和 QUIC 路径；
2. 建立 HTTP/3 连接；
3. 若有配置，则准备第二个网络接口/路径；
4. 等待所需数量的路径进入活跃状态；
5. 启动 GET 或 POST 流；
6. 运行 Libevent 事件循环，直至任务完成。

### 7.4 文件完整性验证

GET 下载：

```bash
sha256sum /srv/XQUIC/server_data.bin
sha256sum /data/downloaded.bin
```

POST 上传：

```bash
sha256sum /data/upload.bin
sha256sum /srv/XQUIC/received_upload.bin
```

完整文件传输结束后，两端的哈希值应一致。

---

## 8. 上传与下载流程

### 8.1 POST 上传

客户端执行以下步骤：

1. 打开 `upload_path`；
2. 获取文件长度；
3. 当 `send_data_len` 非零时，将上传长度限制为该值；
4. 按 `stream_range_size` 将数据划分为多个分段；
5. 最多同时打开 `stream_cnt` 个 HTTP/3 流；
6. 携带偏移量元数据发送每个分段。

服务器根据声明的偏移量使用 `pwrite()` 写入各分段，因此多个流可以并行填充文件的不同区域。

### 8.2 GET 下载

客户端起初只创建一个 GET 请求，因为此时还不知道远端文件的大小。

服务器：

1. 以 `data_dir` 为根目录解析请求路径；
2. 通过 `x-total-length` 返回完整文件长度；
3. 发送请求的文件分段。

随后客户端：

1. 计算所需分段总数；
2. 最多保持 `stream_cnt` 个请求处于活跃状态；
3. 使用 `pwrite()` 将每个分段写入 `download_path` 中的对应位置；
4. 所有分段完成后关闭连接。

### 8.3 请求路径映射

对于请求：

```text
/server_data.bin
```

服务器会去掉路径开头的斜杠，并将其解析为：

```text
<data_dir>/server_data.bin
```

请求路径为 `/` 时，使用 `default_send_file`。

若将该服务器暴露给不可信用户，应用程序必须验证并净化请求路径。当前演示代码的路径处理仅适用于受控实验，不应直接作为公网文件服务器使用。

---

## 9. 调度器与拥塞控制选择

### 9.1 客户端可用调度器

客户端映射关系如下：

| 配置值 | 回调 |
|---|---|
| `minrtt` | `xqc_minrtt_scheduler_cb` |
| `backup` | `xqc_backup_scheduler_cb` |
| `balanced` | `xqc_balanced_scheduler_cb` |
| `rap` | `xqc_rap_scheduler_cb` |
| `act` | `xqc_act_scheduler_cb` |
| `bw` | `xqc_bandwidth_scheduler_cb` |

### 9.2 服务器可用调度器

服务器当前映射：

```text
minrtt, backup, act, bw
```

### 9.3 拥塞控制

客户端配置当前支持：

```text
bbr, cubic
```

服务器默认使用 BBR。若希望通过 `server_config.txt` 选择其他服务器拥塞控制器，应增加与客户端解析器类似的 `cc` 配置分支。


## 10. 在其他应用程序中使用本代码

可以采用两种集成方式。

### 10.1 方式 A：调用独立可执行程序

这是集成风险最低的方式。应用程序需要：

1. 写入 `client_config.txt` 或 `server_config.txt`；
2. 启动相应可执行程序；
3. 监控标准输出、标准错误及输出文件；
4. 按需要终止或重新启动进程。

这种方式可以隔离 XQUIC 事件循环，并简化崩溃后的恢复流程。

### 10.2 方式 B：嵌入现有 C/C++ 程序

其他 C/C++ 程序可以直接调用当前提供的函数，并沿用现有生命周期。

#### 客户端生命周期

```c
#include "mini_client.h"

int run_XQUIC_client(void)
{
    int rc = XQC_ERROR;
    xqc_mini_cli_ctx_t ctx = {0};
    xqc_mini_cli_args_t *args = calloc(1, sizeof(*args));
    xqc_mini_cli_user_conn_t *conn = NULL;

    if (args == NULL) {
        return XQC_ERROR;
    }

    xqc_platform_init_env();

    /* 加载默认值和 client_config.txt，然后创建事件上下文。 */
    if (xqc_mini_cli_init_env(&ctx, args) != XQC_OK) {
        goto out;
    }

    /* 若需通过代码覆盖配置，必须在创建引擎前完成。 */
    args->req_cfg.method = REQUEST_METHOD_GET;
    args->req_stream_cnt = 2;
    args->quic_cfg.cc = CC_TYPE_BBR;
    strncpy(args->quic_cfg.mp_sched, "minrtt",
            sizeof(args->quic_cfg.mp_sched) - 1);

    strncpy(args->net_cfg.server_addr, "203.0.113.10",
            sizeof(args->net_cfg.server_addr) - 1);
    args->net_cfg.server_port = 8443;

    args->net_cfg.multi_interface_cnt = 2;
    strncpy(args->net_cfg.multi_interface[0], "rmnet_data0",
            XQC_MINI_INTERFACE_NAME_MAX_LEN - 1);
    strncpy(args->net_cfg.multi_interface[1], "rmnet_data1",
            XQC_MINI_INTERFACE_NAME_MAX_LEN - 1);

    strncpy(args->env_cfg.download_target, "/server_data.bin",
            sizeof(args->env_cfg.download_target) - 1);
    strncpy(args->req_cfg.url, args->env_cfg.download_target,
            sizeof(args->req_cfg.url) - 1);
    strncpy(args->req_cfg.path, args->req_cfg.url,
            sizeof(args->req_cfg.path) - 1);
    strncpy(args->env_cfg.download_path, "/data/download.bin",
            sizeof(args->env_cfg.download_path) - 1);

    if (xqc_mini_cli_init_xquic_engine(&ctx, args) != XQC_OK) {
        goto out;
    }
    if (xqc_mini_cli_init_engine_ctx(&ctx) != XQC_OK) {
        goto out;
    }

    conn = xqc_mini_cli_user_conn_create(&ctx);
    if (conn == NULL) {
        goto out;
    }

    if (xqc_mini_cli_main_process(conn, &ctx) != XQC_OK) {
        goto out;
    }

    rc = event_base_dispatch(ctx.eb);

out:
    if (ctx.engine != NULL) {
        xqc_engine_destroy(ctx.engine);
    }
    if (conn != NULL) {
        xqc_mini_cli_on_connection_finish(conn);
        xqc_mini_cli_free_user_conn(conn);
    }
    xqc_mini_cli_free_ctx(&ctx);
    return rc;
}
```

#### 服务器生命周期

```c
#include "mini_server.h"

int run_XQUIC_server(void)
{
    int rc = XQC_ERROR;
    xqc_mini_svr_ctx_t ctx = {0};
    xqc_mini_svr_args_t *args = calloc(1, sizeof(*args));
    xqc_mini_svr_user_conn_t *conn = NULL;

    if (args == NULL) {
        return XQC_ERROR;
    }

    xqc_platform_init_env();

    /* 加载默认值和 server_config.txt，然后创建事件上下文。 */
    if (xqc_mini_svr_init_env(&ctx, args) != XQC_OK) {
        goto out;
    }

    /* 在创建引擎前通过代码覆盖配置。 */
    strncpy(args->net_cfg.ip, "0.0.0.0", sizeof(args->net_cfg.ip) - 1);
    args->net_cfg.port = 8443;
    args->quic_cfg.cc = CC_TYPE_BBR;
    strncpy(args->quic_cfg.mp_sched, "minrtt",
            sizeof(args->quic_cfg.mp_sched) - 1);
    strncpy(args->env_cfg.data_dir, "/srv/XQUIC",
            sizeof(args->env_cfg.data_dir) - 1);

    if (xqc_mini_svr_init_xquic_engine(&ctx, args) != XQC_OK) {
        goto out;
    }
    if (xqc_mini_svr_init_engine_ctx(&ctx, args) != XQC_OK) {
        goto out;
    }

    conn = xqc_mini_svr_create_user_conn(&ctx);
    if (conn == NULL) {
        goto out;
    }

    rc = event_base_dispatch(ctx.eb);

out:
    if (ctx.engine != NULL) {
        xqc_engine_destroy(ctx.engine);
    }
    if (conn != NULL) {
        xqc_mini_svr_free_user_conn(conn);
    }
    xqc_mini_svr_free_ctx(&ctx);
    return rc;
}
```

### 10.3 用应用数据替换文件 I/O

演示程序使用文件作为应用接口。

| 操作 | 当前实现位置 | 集成到业务应用时的替换方式 |
|---|---|---|
| 客户端上传数据源 | `mini_client.c` 中的 `xqc_mini_cli_request_send()` | 从应用环形缓冲区或数据生产回调中读取 |
| 客户端下载数据接收 | `mini_client_cb.c` 中的 `xqc_mini_cli_h3_request_read_notify()` | 将数据交给应用回调或队列，而不是直接调用 `pwrite()` |
| 服务器 POST 接收端 | `mini_server_cb.c` 中的 `xqc_mini_svr_h3_request_read_notify()` | 将上传字节交给业务回调或存储层 |
| 服务器 GET 数据源 | `xqc_mini_svr_prepare_file_response()` 与 `xqc_mini_svr_send_body()` | 从业务对象、内存缓冲区或生成器读取 |

不要在 XQUIC 回调中直接执行耗时业务逻辑。应将任务放入其他线程的队列，并尽快返回。

### 10.4 增加应用层回调

建议的下载回调接口：

```c
typedef int (*XQUIC_data_cb)(
    void *app_ctx,
    int stream_index,
    uint64_t file_offset,
    const unsigned char *data,
    size_t data_len,
    int fin);
```

每当 `xqc_mini_cli_h3_request_read_notify()` 收到一块响应体数据后，便调用该回调。除非另行约定了所有权转移规则，否则应用必须在回调返回前复制或处理完这些数据。

---

## 11. HTTP/3 应用层协议

客户端添加以下自定义头：

| 请求头 | 含义 |
|---|---|
| `x-stream-index` | 从 0 开始的分段编号 |
| `x-stream-count` | 客户端当前声明的分段流数量 |
| `x-stream-offset` | 本分段在完整对象中的字节偏移 |
| `x-total-length` | 发送端已知的完整对象长度 |
| `content-length` | 本流分段的长度 |

服务器返回：

| 响应头 | 含义 |
|---|---|
| `:status` | HTTP 响应状态 |
| `content-type` | 响应媒体类型 |
| `content-length` | 返回分段的长度 |
| `x-total-length` | 供 GET 客户端扩展后续请求的完整文件长度 |

这些头共同构成一个应用层区间传输协议。通用 HTTP/3 服务器不会自动实现相同行为。

---

## 12. 线程与回调规则

### 12.1 引擎所有权

应将 XQUIC 引擎视为由 Libevent 事件循环线程或专用引擎线程独占。

除非 XQUIC 明确说明某个操作线程安全，否则不要从任意业务线程并发调用引擎 API。

### 12.2 客户端接收线程

客户端为每条路径维护接收线程，并通过队列将 UDP 数据包转交给引擎线程。业务代码不应绕过该队列，也不应从其他线程直接调用 `xqc_engine_packet_process()`。

### 12.3 非阻塞回调

以下回调中应避免阻塞操作：

- `h3_request_read_notify`；
- `h3_request_write_notify`；
- 套接字回调；
- 定时器回调。

对于文件、编解码、推理或传感器处理，使用：

```text
XQUIC 回调 → 有界队列 → 应用工作线程
```

队列已满时应实施背压，避免无限制地分配内存。

### 12.4 从应用向传输层提供数据

对于实时数据生产者，使用：

```text
应用生产者 → 有界发送队列 → 引擎线程发送回调
```

新数据到达时，通过 `eventfd`、管道或 `event_active()` 通知事件循环，不要直接并发调用引擎。

---

## 13. 日志、统计与验证

### 13.1 有用的运行时信息

代码会输出：

- 配置路径数与活跃路径数；
- 接口绑定及本地地址；
- 路径 ID；
- 流偏移和长度；
- 各流的完成时间与有效吞吐率；
- 聚合上传/下载有效吞吐率；
- 服务器路径状态、SRTT、发送字节数与接收字节数。

### 13.2 性能测试模式

进行有效吞吐率测试时，建议设置：

```ini
use_zlog=0
```

仅在短时诊断运行中启用详细日志。

### 13.3 嵌入式设备系统监控

```bash
mpstat -P ALL 1
pidstat -t -p "$(pidof mini_client)" 1
ip -s link show rmnet_data0
ip -s link show rmnet_data1
nstat -az
cat /proc/net/softnet_stat
ss -u -m -i
```

### 13.4 每次实验应记录的内容

```text
软件提交号/构建版本
client_config.txt
server_config.txt
5G 接口与运营商映射
传输方向
数据发送端的调度器
数据发送端的拥塞控制
stream_cnt
stream_range_size
单路径 A 速率
单路径 B 速率
双路径有效吞吐率
各 CPU 核使用率
各路径 RTT 与字节数
UDP 与接口丢包
文件哈希
```

---

## 14. 故障排查

### 客户端只使用一条路径

检查：

```bash
ip -br addr
ip route get <server-ip> oif <interface-A>
ip route get <server-ip> oif <interface-B>
```

同时确认：

- `interfaces=` 中包含两个接口名；
- 两个接口均有 IPv4 地址；
- 进程具有 `SO_BINDTODEVICE` 所需权限；
- 启动输出报告两个不同的文件描述符；
- XQUIC 报告两条活跃路径。

### 修改配置后看不到效果

- 从配置文件所在目录运行程序。
- 客户端与服务器 `main` 文件中的命令行解析当前均被禁用。
- 服务器配置解析器当前不解析 `cc=`。
- 虽然解析器接受 `balanced` 和 `rap`，服务器选择器并未映射它们。

### 服务器启动失败

检查：

- `server_config.txt` 是否存在；
- `server.crt` 和 `server.key` 是否存在且可读；
- UDP 端口是否已被占用；
- 监听 IP 是否属于服务器；
- zlog 已关闭，或 `zlog.conf` 配置有效。

### GET 返回 404

检查实际解析路径：

```text
<data_dir>/<去除开头斜杠后的 download_target>
```

例如：

```ini
data_dir=/srv/XQUIC
```

以及：

```ini
download_target=/server_data.bin
```

要求文件存在于：

```text
/srv/XQUIC/server_data.bin
```

### 双路径下载反而更慢

应区分传输层瓶颈与接收端瓶颈：

1. 对比各接口的入站线路速率与 HTTP/3 实际交付的有效吞吐率；
2. 暂时关闭文件写入，仅统计字节；
3. 关闭 zlog/qlog；
4. 在相同 BBR 设置下比较服务器 `minrtt` 与 `bw`；
5. 监控各 CPU 核利用率与 UDP 丢包；
6. 比较 `stream_cnt=1` 与 `stream_cnt=2`；
7. 确认请求启动前两条路径均已活跃。

### 上传可以聚合，但下载不能聚合

注意数据发送端已经改变：

- 上传由客户端调度器/拥塞控制控制；
- 下载由服务器调度器/拥塞控制控制。

进行 GET 下载时，嵌入式设备会成为高吞吐接收端，因此 CPU、套接字接收、数据重组和存储写入都可能限制有效吞吐率。

---

## 15. 建议的生产级 API

当前函数会暴露内部结构。面向应用开发者提供接口时，建议增加一层轻量、稳定的封装，避免业务代码直接访问 `xqc_mini_cli_args_t` 和 `xqc_mini_svr_args_t`。

建议接口如下：

```c
typedef struct XQUIC_client XQUIC_client_t;
typedef struct XQUIC_server XQUIC_server_t;

typedef struct {
    const char *server_addr;
    uint16_t server_port;
    const char *interfaces[2];
    size_t interface_count;
    const char *scheduler;
    const char *congestion_control;
    size_t stream_concurrency;
    size_t stream_range_size;
} XQUIC_client_config_t;

typedef struct {
    void (*on_connected)(void *ctx);
    void (*on_path_state)(void *ctx, uint64_t path_id, int active);
    int  (*on_data)(void *ctx, uint64_t offset,
                    const unsigned char *data, size_t len, int fin);
    void (*on_complete)(void *ctx, int status, uint64_t bytes);
    void (*on_error)(void *ctx, int error_code, const char *message);
    void *ctx;
} XQUIC_callbacks_t;

int XQUIC_client_create(
    const XQUIC_client_config_t *config,
    const XQUIC_callbacks_t *callbacks,
    XQUIC_client_t **client);

int XQUIC_client_get(
    XQUIC_client_t *client,
    const char *remote_path);

int XQUIC_client_post(
    XQUIC_client_t *client,
    uint64_t total_length,
    int (*read_cb)(void *ctx, uint64_t offset,
                   unsigned char *buf, size_t capacity, size_t *read_len),
    void *read_ctx);

int XQUIC_client_run(XQUIC_client_t *client);
void XQUIC_client_stop(XQUIC_client_t *client);
void XQUIC_client_destroy(XQUIC_client_t *client);
```

该封装层应当：

- 隐藏 XQUIC 与 Libevent 内部结构；
- 验证配置合法性；
- 明确定义回调数据所有权；
- 提供异步停止和完成事件；
- 在不暴露内部内存的情况下提供各路径指标；
- 保持引擎线程所有权规则；
- 同时支持基于文件和基于回调的数据源/接收端。

---

## 快速启动检查表

### 服务器

```text
[ ] 使用 Release 模式构建 mini_server
[ ] 安装 server.crt 与 server.key
[ ] 设置 listen_addr/listen_port
[ ] 设置 data_dir/default_send_file 或 upload_path
[ ] 为 GET 选择服务器 mp_sched
[ ] 性能测试时关闭 zlog
[ ] 开放 UDP 防火墙端口
[ ] 运行 ./mini_server
```

### 客户端

```text
[ ] 为嵌入式 ARM 目标构建 mini_client
[ ] 检查两张 5G 网卡及其路由
[ ] 设置 server_addr/server_port
[ ] 设置 GET 或 POST
[ ] 设置 interfaces=A,B
[ ] 设置 stream_cnt 与 stream_range_size
[ ] 设置 upload_path，或 download_target/download_path
[ ] 为 POST 选择客户端调度器/拥塞控制
[ ] 性能测试时关闭 zlog
[ ] 使用具备 SO_BINDTODEVICE 权限的方式运行
[ ] 传输前确认两条路径均已激活
[ ] 校验输出文件哈希
```
