# mini_client / mini_server 构建链路与业务修改入口

本文说明当前项目中 `build/mini/mini_client` 和 `build/mini/mini_server` 的生成方式、直接源码、间接依赖和业务逻辑修改入口。结论基于当前工作区 `C:\Users\DELL\Desktop\xquic_4.1` 的源码和已有 `build/` 记录。

## 1. 核心结论

`mini_client` 和 `mini_server` 不是只由 `mini/` 目录单独编译出来的。它们的生成链路是：

```text
CMakeLists.txt
  -> add_subdirectory(mini)
  -> mini/CMakeLists.txt
  -> add_executable(mini_client / mini_server)
  -> 链接 xquic-static
  -> xquic-static 聚合 src/http3、src/transport、src/tls、src/common、src/congestion_control 等源码
  -> 再链接 libevent、zlog、Tongsuo/BabaSSL、系统库和 scripts/xquic.lds
```

也就是说，最终挂载使用的二进制文件由四类输入共同产生：

| 类别 | 主要内容 | 作用 |
| --- | --- | --- |
| mini 示例层 | `mini/*.c` | 应用侧 main、配置解析、socket、HTTP/3 请求/响应、统计输出 |
| demo HQ 封装层 | `demo/xqc_hq_*.c` | HTTP/3/HQ 请求上下文封装 |
| XQUIC 静态库 | `src/*` | QUIC、HTTP/3、TLS、拥塞控制、多路径调度等核心协议栈 |
| 第三方和链接脚本 | `third_party/*`、`scripts/xquic.lds` | TLS、事件循环、日志、符号导出和系统依赖 |

当前 Windows 工作区下 `demo/` 目录不存在，`Test-Path demo` 返回 `False`。但 `build/` 中的 `link.txt` 和 `DependInfo.cmake` 记录来自 Linux 路径 `/home/admin123/xquic_test`，说明现有构建产物来自另一套完整构建环境。重新 clean build 前需要确认真实构建机源码树里存在 `demo/xqc_hq_*.c`。

## 2. CMake 构建入口

顶层构建入口在 `CMakeLists.txt`：

| 位置 | 证据 | 含义 |
| --- | --- | --- |
| `CMakeLists.txt:167` | `if(XQC_ENABLE_TESTING)` | 只有打开测试构建时才进入 `mini/` |
| `CMakeLists.txt:168` | `add_subdirectory(mini)` | 把 `mini/CMakeLists.txt` 加入构建 |
| `build/CMakeCache.txt:240` | `XQC_ENABLE_TESTING:UNINITIALIZED=1` | 当前已有 build 记录中 mini 被启用 |

`mini/CMakeLists.txt` 定义两个可执行目标：

| 位置 | 证据 | 含义 |
| --- | --- | --- |
| `mini/CMakeLists.txt:50` | `add_executable(mini_server ${MINI_SERVER_SOURCES})` | 生成 `mini_server` |
| `mini/CMakeLists.txt:51` | `add_executable(mini_client ${MINI_CLIENT_SOURCES})` | 生成 `mini_client` |
| `mini/CMakeLists.txt:56` | `target_link_libraries(mini_server ... xquic-static ...)` | 服务端链接 `xquic-static` |
| `mini/CMakeLists.txt:57` | `target_link_libraries(mini_client ... xquic-static ...)` | 客户端链接 `xquic-static` |

## 3. 两个可执行文件的直接源码

### mini_client

`mini_client` 直接编译对象来自 `build/mini/CMakeFiles/mini_client.dir/link.txt`：

```text
demo/xqc_hq_ctx.c
demo/xqc_hq_conn.c
demo/xqc_hq_request.c
mini/common.c
mini/mini_client.c
mini/mini_client_cb.c
mini/mini_client_main.c
```

这些文件的职责如下：

| 文件 | 职责 |
| --- | --- |
| `mini/mini_client_main.c` | 客户端进程入口，初始化参数、创建连接、进入事件循环 |
| `mini/mini_client.c` | 客户端主体，加载 `client_config.txt`，创建 engine/connection/socket，发起 H3 请求，处理多路径 socket 绑定 |
| `mini/mini_client_cb.c` | H3 连接和请求回调，处理响应读取、POST body 写入、连接关闭和统计 |
| `mini/common.c` | mini 示例层通用工具 |
| `demo/xqc_hq_*.c` | HQ/H3 请求上下文封装 |

### mini_server

`mini_server` 直接编译对象来自 `build/mini/CMakeFiles/mini_server.dir/link.txt`：

```text
demo/xqc_hq_ctx.c
demo/xqc_hq_conn.c
demo/xqc_hq_request.c
mini/common.c
mini/mini_server.c
mini/mini_server_cb.c
mini/mini_server_main.c
```

这些文件的职责如下：

| 文件 | 职责 |
| --- | --- |
| `mini/mini_server_main.c` | 服务端进程入口，初始化参数、创建监听 socket、进入事件循环 |
| `mini/mini_server.c` | 服务端主体，加载 `server_config.txt`，创建 engine/socket，接收 UDP 包并交给 XQUIC |
| `mini/mini_server_cb.c` | H3 请求回调，接收 POST body，发送响应 headers/body，输出统计 |
| `mini/common.c` | mini 示例层通用工具 |
| `demo/xqc_hq_*.c` | HQ/H3 请求上下文封装 |

## 4. 链接和封装

`mini_client` 与 `mini_server` 的链接结构基本一致。已有 `link.txt` 显示两者都使用 aarch64 交叉编译器，并链接以下关键依赖：

| 依赖 | 证据 | 作用 |
| --- | --- | --- |
| `../libxquic-static.a` | `build/mini/CMakeFiles/*/link.txt` | XQUIC 协议栈静态库 |
| `libevent.a` | `third_party/libevent_aarch64/.../.libs/libevent.a` | 事件循环 |
| `libevent_pthreads.a` | `third_party/libevent_aarch64/.../.libs/libevent_pthreads.a` | libevent 线程支持 |
| `libzlog.a` | `third_party/zlog-1.2.18/build_aarch64/lib/libzlog.a` | 日志 |
| `libssl.a`、`libcrypto.a` | `third_party/Tongsuo-8.3.0` | TLS/加密 |
| `scripts/xquic.lds` | link 选项 `--version-script` | 符号导出控制 |
| `-ldl -lpthread -lm` | link 选项 | 系统库 |

`xquic-static` 由顶层 `CMakeLists.txt` 的 `add_library(xquic-static STATIC ${XQC_SOURCE})` 生成。已有 `build/CMakeFiles/xquic-static.dir/link.txt` 显示它当前包含：

| 模块 | 典型文件/目录 | 作用 |
| --- | --- | --- |
| HTTP/3 | `src/http3/`、`src/http3/qpack/` | HTTP/3 连接、请求、QPACK、扩展 datagram/bytestream |
| 传输层 | `src/transport/` | engine、connection、stream、packet、frame、timer、datagram、多路径 |
| 调度器 | `src/transport/scheduler/` | `minrtt`、`backup`、`balanced`、`rap`、`act`、`bw` 等路径选择 |
| 重注入控制 | `src/transport/reinjection_control/` | 多路径丢包/未 ACK 数据重注入策略 |
| TLS | `src/tls/`、`src/tls/babassl/` | TLS 适配、HKDF、crypto 接口 |
| 通用工具 | `src/common/` | 日志、时间、随机数、ring buffer、变长整数等 |
| 拥塞控制 | `src/congestion_control/` | BBR、CUBIC、采样、窗口过滤 |

当前 `build/CMakeCache.txt` 中 BBR2、Reno、Copa、Unlimited、FEC、MP interop 都是 `OFF`，所以当前已有静态库只包含默认编译进来的能力。

## 5. 业务逻辑修改入口

### 客户端主流程

建议先按这个调用链阅读：

```text
mini/mini_client_main.c
  -> 初始化 xqc_mini_cli_args_t
  -> mini/mini_client.c 加载 client_config.txt
  -> 创建 XQUIC client engine
  -> 创建 socket 和 connection
  -> 发起 HTTP/3 请求
  -> mini/mini_client_cb.c 处理读写回调和统计
```

常见修改点：

| 目标 | 优先文件 | 说明 |
| --- | --- | --- |
| 修改 server 地址、方法、stream 数、调度器配置解析 | `mini/mini_client.c` | `xqc_mini_cli_load_config_file` 负责解析 `client_config.txt` |
| 修改 GET/POST 请求构造 | `mini/mini_client.c` | 请求 headers、body 发送入口在客户端主体逻辑 |
| 修改 POST body 连续发送逻辑 | `mini/mini_client_cb.c` | 写回调继续推动 request body |
| 修改响应读取和统计 | `mini/mini_client_cb.c` | 读回调和 request close 统计 |
| 修改多网卡/多路径 socket 绑定 | `mini/mini_client.c` | 查 `interfaces`、`SO_BINDTODEVICE`、path binding 相关逻辑 |

### 服务端主流程

建议先按这个调用链阅读：

```text
mini/mini_server_main.c
  -> 初始化 xqc_mini_svr_args_t
  -> mini/mini_server.c 加载 server_config.txt
  -> 创建 XQUIC server engine
  -> 创建并 bind UDP socket
  -> recvfrom 后调用 xqc_engine_packet_process
  -> mini/mini_server_cb.c 处理请求并发送响应
```

常见修改点：

| 目标 | 优先文件 | 说明 |
| --- | --- | --- |
| 修改监听地址、端口、默认文件目录 | `mini/mini_server.c` | `xqc_mini_svr_load_config_file` 负责解析 `server_config.txt` |
| 修改 socket bind 行为 | `mini/mini_server.c` | `xqc_mini_svr_create_socket` 决定绑定地址 |
| 修改 UDP 收包处理 | `mini/mini_server.c` | socket read handler 调用 `xqc_engine_packet_process` |
| 修改请求读取和响应内容 | `mini/mini_server_cb.c` | H3 request read/write 回调 |
| 修改服务端 GET 下载文件逻辑 | `mini/mini_server_cb.c` | 响应 body 发送逻辑 |

### 协议栈和算法修改入口

| 修改目标 | 入口目录/文件 |
| --- | --- |
| QUIC engine 和 packet process | `src/transport/xqc_engine.c` |
| 连接状态、路径管理、调度调用 | `src/transport/xqc_conn.c` |
| stream 发送/接收和流控 | `src/transport/xqc_stream.c` |
| 拥塞窗口、RTT、inflight、ACK/LOSS | `src/transport/xqc_send_ctl.c` |
| 多路径调度算法 | `src/transport/scheduler/xqc_scheduler_*.c` |
| 重注入策略 | `src/transport/reinjection_control/`、`src/transport/xqc_reinjection.c` |
| 拥塞控制算法 | `src/congestion_control/` |

## 6. 建议验证命令

在 Windows 工作区只读核查：

```powershell
cd C:\Users\DELL\Desktop\xquic_4.1
rg -n "add_subdirectory\(mini\)|add_executable|target_link_libraries|xquic-static" CMakeLists.txt mini\CMakeLists.txt
Get-Content build\mini\CMakeFiles\mini_client.dir\link.txt
Get-Content build\mini\CMakeFiles\mini_server.dir\link.txt
Get-Content build\CMakeFiles\xquic-static.dir\link.txt
Get-Content build\mini\CMakeFiles\mini_client.dir\DependInfo.cmake
Get-Content build\mini\CMakeFiles\mini_server.dir\DependInfo.cmake
Test-Path demo
```

在 Linux 构建机验证：

```bash
cd /home/admin123/xquic_test
cmake -S . -B build -DXQC_ENABLE_TESTING=1
cmake --build build --target mini_client mini_server -j
file build/mini/mini_client build/mini/mini_server
ldd build/mini/mini_client || true
```

如果构建耗时过长，建议只让目标机器执行 `cmake --build build --target mini_client mini_server -j`，再回收 `link.txt`、`CMakeCache.txt` 和报错日志继续分析。
