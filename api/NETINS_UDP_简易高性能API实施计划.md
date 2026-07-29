# NetIns UDP 简易高性能 API 实施计划

## 1. 计划目标

在当前 `v5.4-api版` 基础上，形成一套可直接交付第三方的 NetIns UDP API。第三方只需要：

- 引入 `netins_udp.h`；
- 链接 `libxquic.a` 及其依赖库；
- 按“初始化、建立连接、收发数据、关闭连接”的方式使用聚合模组；
- 不需要了解 XQUIC engine、QUIC stream、HTTP/3 request、UDP socket、Libevent 或 mini 内部实现。

最终常用调用链如下：

```c
/* 客户端 */
netins_udp_init();
netins_udp_t *conn = netins_udp_new(NETINS_UDP_TYPE_MPQUIC);
netins_udp_set_opt(conn, ...);
netins_udp_connect(conn, host, port);
netins_udp_send(conn, data, data_len, 0);
netins_udp_recv(conn, buf, buf_len, 0);
netins_udp_shutdown(conn, 0);
netins_udp_close(conn);
netins_udp_cleanup();

/* 服务端 */
netins_udp_init();
netins_udp_t *listener = netins_udp_new(NETINS_UDP_TYPE_MPQUIC);
netins_udp_set_opt(listener, ...);
netins_udp_listen(listener, host, port);
netins_udp_t *conn = netins_udp_accept(listener, NULL, NULL);
netins_udp_recv(conn, buf, buf_len, 0);
netins_udp_send(conn, data, data_len, 0);
netins_udp_shutdown(conn, 0);
netins_udp_close(conn);
netins_udp_close(listener);
netins_udp_cleanup();
```

API 实现内部可以直接调用 `xqc_*` 底层接口，也可以复用现有 NetIns 私有 backend。对外公共接口不得暴露 XQUIC 私有结构、HTTP/3 request 对象或底层 UDP socket。

## 2. 核心原则

### 2.1 外部简单，内部保留高性能实现空间

对外提供接近 TCP/MPTCP 的连接级使用方式；内部仍以 QUIC/HTTP/3 和现有双路径收发机制工作。`netins_udp_send()`、`netins_udp_recv()` 是稳定的应用数据接口，不等同于裸 UDP 数据报。

### 2.2 只封装，不改变原有传输逻辑

首轮实现不得修改：

- mini_client、mini_server 的原有业务与命令行逻辑；
- XQUIC 调度器和拥塞控制算法；
- mini 的 Libevent、UDP 收包和 RX 主路径；
- XQUIC 原有公共 ABI；
- `xqc_conn_get_stats()` 的原有语义；
- 已验证的双路径创建、ACK、定时器和路径调度逻辑。

API 只能复用同一个 XQUIC engine、UDP socket 和接收者，禁止增加第二套 UDP 接收线程或第二个 socket 消费者。

### 2.3 性能是交付门槛

不能仅以“功能可收发”为完成标准。API 数据面必须与原 mini 使用相同的关键传输能力，并通过同环境对比测试。

连接级 `send/recv` 由私有并行数据 backend 承载。client 到 server 和 server
到 client 分别保持与 mini 上传、下载方向相当的内部并发能力；第三方只看到
一个连续有序字节流，不配置 request 数、内部帧或重排缓冲区。

### 2.4 ABI 采用增量方式演进

在正式 ABI 冻结前只保留最终需要交付的连接级接口。开发阶段曾加入但不再交付
的文件专用草案接口从公共头、导出表和构建目标中移除；不复用不兼容的旧符号，
不把 legacy 私有结构安装给第三方。

## 3. 拟新增公共接口

在 `include/xquic/netins_udp.h` 中新增：

```c
NETINS_UDP_PUBLIC ssize_t
netins_udp_send(netins_udp_t *conn, const void *buf, size_t len, int flags);

NETINS_UDP_PUBLIC ssize_t
netins_udp_recv(netins_udp_t *conn, void *buf, size_t len, int flags);
```

现有接口继续承担以下职责：

| 能力 | 公共接口 |
|---|---|
| 创建对象 | `netins_udp_new()` |
| 配置选项 | `netins_udp_set_opt()` / `netins_udp_get_opt()` |
| 客户端连接 | `netins_udp_connect()` |
| 服务端监听 | `netins_udp_listen()` |
| 接收连接 | `netins_udp_accept()` |
| 数据发送 | `netins_udp_send()` |
| 数据接收 | `netins_udp_recv()` |
| 查询状态 | `netins_udp_get_state()` |
| 查询多路径 | `netins_udp_is_multipath()` |
| 查询路径与统计 | `netins_udp_get_path_count()` / `netins_udp_get_snapshot()` 等 |
| 通知集成 | `netins_udp_get_fd()` / `netins_udp_get_events()` |
| 正常关闭 | `netins_udp_shutdown()` |
| 释放对象 | `netins_udp_close()` |
| 错误信息 | `netins_udp_get_last_error()` / `netins_udp_get_close_reason()` |

现有 HTTP/3 request API 暂时保留为高级接口，避免破坏已完成代码和兼容性；正式第三方入门文档只介绍连接级常用接口。

## 4. 连接级收发语义

### 4.1 数据模型

`netins_udp_send()` / `netins_udp_recv()` 提供：

- 可靠传输；
- 有序字节数据；
- 允许部分发送和部分接收；
- 不保留调用方每次 `send()` 的消息边界；
- 数据可由 MPQUIC 在内部路径间调度；
- 不向调用方暴露 HTTP headers、request、stream ID 或路径分片。

这是一套“类似 socket 字节流”的接口，不是裸 UDP 数据报接口。

### 4.2 返回值

`netins_udp_send()`：

- `> 0`：本次已被 API 接受的字节数，可能小于 `len`；
- `0`：仅在 `len == 0` 或明确规定的结束语义下返回；
- `< 0`：失败，具体原因通过稳定的 NetIns 错误码查询。

`netins_udp_recv()`：

- `> 0`：实际读取字节数；
- `0`：对端数据方向已正常结束；
- `< 0`：失败或非阻塞状态下暂时无数据。

不得承诺完整兼容 Linux `send(2)` / `recv(2)` 的所有 flags。首版只公开已经实现并测试的 flags，例如非阻塞、查看但不移除、发送结束标志。

### 4.3 对象适用范围

- 客户端连接对象：允许 `send/recv`；
- `accept()` 返回的服务端连接对象：允许 `send/recv`；
- listener 对象：不允许 `send/recv`，调用时返回明确错误；
- 已关闭或错误状态对象：返回稳定错误码；
- HTTP/3 request 对象继续使用其专用接口。

### 4.4 并发与线程安全

首版保证：

- 一个发送线程和一个接收线程可同时工作；
- 状态查询和统计快照可以与收发并行；
- 多个线程同时发送时由内部串行化，不能交错破坏字节顺序；
- 多个线程同时接收时由内部串行化，数据只交付一次；
- 回调线程、阻塞行为、超时和内存所有权写入公共文档。

### 4.5 生命周期

- `connect()` 成功表示 QUIC/H3 连接可用；内部默认数据通道在首次
  `send/recv` 时延迟建立；
- `accept()` 返回 accepted connection，不返回底层 request；
- 数据通道尚未完全就绪时，阻塞 `send/recv` 按超时等待，非阻塞调用返回“稍后重试”；
- `shutdown()` 发起正常关闭，不立即释放对象；
- `close()` 释放 NetIns 对象及其拥有的资源；
- listener 和 accepted connection 的所有权独立；
- 关闭 listener 不得破坏已经 accept 的连接；
- accepted connection 关闭不得误关共享的 server engine 或 UDP socket。

## 5. 内部技术方案

### 5.1 默认隐藏数据通道

每个可收发的 `netins_udp_t` 内部持有一组不可见的并行数据通道：

```text
第三方 send/recv
        │
        ▼
NetIns 连接级数据 backend
        │
        ▼
内部 HTTP/3 并行逻辑通道
        │
        ▼
XQUIC engine / MPQUIC path scheduler
        │
        ▼
现有 UDP socket 与 RX 主路径
```

第三方不创建、accept 或关闭 HTTP/3 request。默认通道由 API 自动管理；
内部数据带有全局字节偏移，接收端完成去重、排序并只交付连续前缀。

### 5.2 客户端行为

客户端在首次连接级 `send/recv` 时自动创建内部并行数据 request，并携带私有
协议标识和版本。request body 与 response body 分别作为双向数据面。

客户端 `send()` 写入 request body，`recv()` 读取 response body。内部通道采用未知总长度的持续传输语义，允许应用连续多次收发。

### 5.3 服务端行为

服务端识别同一连接的 NetIns 内部数据 request，将其按私有通道编号绑定到对应
accepted connection，并自动设置成功响应。

服务端 `recv()` 读取 request body，`send()` 写入 response body。业务调用方不需要处理 URL、headers、status code 或 request 生命周期。

### 5.4 底层 XQUIC 调用原则

NetIns 内部允许直接调用以下类型的 XQUIC 能力：

- engine 创建、驱动、定时器和销毁；
- QUIC/H3 建连与关闭；
- H3 request 创建、收发和结束；
- MPQUIC 路径创建、关闭和状态查询；
- NetIns 私有 observer 和路径统计。

调用应集中在私有 transport/backend 层，公共 API 文件负责参数校验、对象生命周期和稳定语义。不得让调用方保存或操作 `xqc_engine_t`、`xqc_connection_t`、`xqc_h3_request_t` 等内部对象。

### 5.5 通知机制

连接对象现有 notifier 继续作为第三方事件入口：

- 内部通道可读时，连接对象产生 `READABLE`；
- 内部通道可写时，连接对象产生 `WRITABLE`；
- 连接建立产生 `CONNECTED`；
- listener 有连接可接收时产生 `ACCEPTABLE`；
- 路径变化产生 `PATH_CHANGED`；
- 连接关闭或错误产生 `CLOSED` / `ERROR`。

`netins_udp_get_fd()` 返回的是 NetIns 通知 fd，不是底层 UDP socket。第三方不得读取底层网络包，也不会成为第二个 UDP 接收者。

### 5.6 队列与背压

继续使用有界队列和连接级总预算：

- 发送队列满时，阻塞调用等待、非阻塞调用返回稍后重试；
- 接收队列满时通过 XQUIC/H3 流控施加背压；
- 不能为了提高吞吐建立无界缓存；
- API 队列预算不能挤占 engine 的 ACK、定时器和路径反馈处理；
- 日志和统计不得进入每包热路径。

具体内部队列大小属于实现细节，不写入公共 ABI，也不作为第三方必须配置的参数。

## 6. 性能实现策略

### 6.1 已完成的首轮原型：单隐藏通道

先用一个持续的内部 H3 数据通道实现连接级字节流，直接复用现有 engine、socket、RX 和 MPQUIC 调度能力。

该方案改动最小、顺序语义清晰、生命周期容易验证。XQUIC 可以在一个连接的数据包层使用多条路径，但其最终吞吐必须实测，不能仅凭功能判断。

### 6.2 性能判定门

在相同设备、链路、文件、方向和测试参数下对比：

- 原 mini 单链路 GET/POST；
- API 单链路 GET/POST；
- 原 mini Agg GET/POST；
- API Agg GET/POST。

如果 API 聚合吞吐低于 mini 超过 5%，先定位：

- 应用发送粒度和队列预算；
- H3 body 生产速度；
- 接收回调与 ACK/定时器调度；
- notifier 唤醒和锁竞争；
- 文件 I/O 是否进入协议线程；
- 单通道流控或 CWND 是否成为瓶颈。

### 6.3 当前实现：连接级并行通道聚合

高性能能力直接进入 `send/recv` 私有 backend：

- client 发送方向使用最多 4 条持续数据通道；
- server 发送方向使用最多 8 条持续数据通道；
- 每段内部数据携带全局字节偏移和版本化帧头；
- 接收端处理跨通道乱序，只向调用方交付连续有序字节；
- FIN 在全部内部通道上闭环，连接级 `recv()` 只在完整前缀交付后返回 EOF；
- 普通内存数据和文件内容使用同一公共接口。

并发数、内部帧大小和调度细节不作为调用方配置，防止后续性能调优破坏第三方
兼容性。

## 7. 分阶段执行计划

### 阶段 0：冻结现状与建立对照

工作内容：

- 记录当前分支、提交、公共头文件和导出符号；
- 保存现有 API 单元测试结果；
- 保存当前 mini 的构建方式和可执行文件；
- 准备后续性能测试命令与结果表格；
- 确认所有新增改动均可通过单独提交回退。

阶段产物：

- ABI/符号基线；
- 构建基线；
- 功能与性能测试表模板。

在当前无法测试真实链路时，可以先完成代码阶段，但性能阶段不得以推测数据验收。

### 阶段 1：冻结连接级 API 语义

工作内容：

- 在公共头文件声明 `netins_udp_send/recv`；
- 固定返回值、错误码、flags、EOF、阻塞和线程安全语义；
- 明确 listener 不可收发；
- 更新导出符号检查；
- 先添加编译级和非法参数测试。

验收：

- 不修改原函数签名；
- 新符号只解析到新实现；
- 静态库和共享库符号一致；
- 现有 API 测试不回退。

### 阶段 2：实现客户端默认数据通道

工作内容：

- 在 `netins_udp_t` 中增加私有默认通道状态；
- 首次连接级收发时自动发起内部数据 request；
- 将客户端 `send()` 映射到 request body；
- 将客户端 `recv()` 映射到 response body；
- 复用现有超时、非阻塞、队列预算和错误转换；
- 清理连接失败、对端关闭和重复关闭时的通道资源。

验收：

- 客户端对象可连续多次 send/recv；
- 支持部分发送、部分接收和 EOF；
- 不暴露 H3 request 对象；
- 不增加第二个 engine、socket 或 RX 消费者。

### 阶段 3：实现服务端默认数据通道

工作内容：

- server backend 识别内部数据 request；
- 自动绑定到 accepted connection；
- 自动建立响应数据方向；
- 将服务端 `recv()` 映射到 request body；
- 将服务端 `send()` 映射到 response body；
- 处理 listener 先关闭、connection 先关闭和共享资源所有权。

验收：

- `listen/accept/recv/send/shutdown/close` 完整闭环；
- 服务端可以先发送数据，调用方无需 accept request；
- listener 关闭不影响已建立连接；
- accepted connection 不销毁共享 server engine/socket。

### 阶段 4：通知、错误与生命周期闭环

工作内容：

- 将默认通道事件折叠到连接 notifier；
- 补齐 `READABLE/WRITABLE/CLOSED/ERROR`；
- 固定关闭原因和错误码映射；
- 验证阻塞超时、非阻塞重试、对端异常断开；
- 验证重复 new/connect/close 和失败路径内存释放。

验收：

- epoll 不丢唤醒、不忙循环；
- 不需要第三方轮询内部 H3 request；
- 重复关闭不崩溃、不重复释放；
- 错误信息可通过公共接口稳定获取。

### 阶段 5：功能与大数据正确性测试

测试内容：

- 本机回环客户端/服务端；
- 单向上传、单向下载和双向交互；
- 空数据、小数据、跨多次调用的数据；
- 大文件和连续长时间收发；
- 并发连接；
- 半途关闭发送方向；
- 对端提前关闭；
- listener 先关闭；
- 网络超时和异常断网；
- 路径断开、路径恢复和重连；
- 内存、句柄、队列预算和数据校验。

验收：

- 数据内容和总字节数完全一致；
- 无死锁、越界、重复交付或静默丢数据；
- 长时间运行内存和句柄稳定；
- 原有 H3 request API 测试继续通过。

### 阶段 6：与 mini 的性能对照

工作内容：

- 使用相同文件、链路和脚本对比 mini 与 API；
- 分别测试 5G1、5G2、Agg；
- 分别测试 GET 与 POST；
- 记录吞吐、CPU、内存、路径流量、RTT、CWND 和 inflight；
- 检查 API 是否引入锁竞争、额外拷贝或日志热路径。

目标门槛：

- API 单链路吞吐不低于 mini 的 95%；
- API 聚合吞吐不低于 mini 的 95%；
- 聚合效率相对 mini 下降不超过 3 个百分点；
- CPU 使用率相对增加不超过 10%；
- Agg GET 仍满足 `Agg > max(5G1, 5G2)`；
- 后续目标环境中继续以 `Agg >= 205 Mbps` 为业务目标。

若未达到门槛，不进入正式交付，先定位并优化；确认单通道是结构性瓶颈后再进入阶段 6.5。

### 阶段 6.5：连接级并行 backend 收口

本阶段已经进入实现，不再以单隐藏通道作为性能等价性的依据。

工作内容：

- 实现 client 发送 4 通道、server 发送 8 通道；
- 实现全局字节偏移、跨通道重排、重叠拒绝和完整 EOF 判断；
- 保持内部帧参数、H3 对象和 XQUIC 调用位于私有 backend；
- 从公共 ABI 和交付包移除文件专用接口；
- 重新执行阶段 4、5、6 的全部测试。

验收标准仍使用阶段 6 的性能门槛，对外 API 和 ABI 不变化。

### 阶段 7：跨架构构建与 SDK 包

工作内容：

- Linux x86_64 构建；
- Linux AArch64 交叉编译或目标机编译；
- 检查 `libxquic.a` 中公共符号；
- 验证第三方程序只包含公共头文件即可编译；
- 固定所需系统库、TLS 库和链接顺序；
- 检查安装规则不包含 draft、legacy 和私有头文件。

交付目录建议：

```text
netins_udp_sdk/
├── include/
│   └── netins_udp.h
├── lib/
│   ├── linux-x86_64/
│   │   └── libxquic.a
│   └── linux-aarch64/
│       └── libxquic.a
├── examples/
│   ├── client.c
│   └── server.c
├── scripts/
│   └── run_demo.sh
├── docs/
│   ├── NetIns_UDP_API_使用说明.md
│   └── 版本说明.md
└── README.md
```

### 阶段 8：正式使用说明和交付验收

使用说明按甲方关注点编写：

- 如何初始化；
- 如何配置服务地址、证书和双网卡；
- 如何建立连接；
- 如何监听和接收连接；
- 如何发送与接收数据；
- 如何判断连接和多路径状态；
- 如何使用 epoll；
- 如何正常断开；
- 如何处理错误和重连；
- 如何编译和链接；
- 完整客户端、服务端示例。

正式使用说明不要求第三方理解内部 H3 request、mini 或 XQUIC 实现。mini 仅作为现有聚合模组和性能参考程序保留。

## 8. 测试矩阵

| 类别 | 测试项 | 预期 |
|---|---|---|
| 构建 | x86_64 / AArch64 | 均可编译链接 |
| ABI | 头文件、导出符号、结构版本 | 无冲突、无私有符号误暴露 |
| 连接 | connect/listen/accept | 正常建立，状态正确 |
| 收发 | 小包、大文件、连续调用 | 数据一致，支持部分收发 |
| 双向 | 客户端和服务端同时收发 | 不死锁，顺序正确 |
| 通知 | epoll READABLE/WRITABLE | 不丢唤醒，不忙循环 |
| 生命周期 | shutdown/close/重复关闭 | 不崩溃、不泄漏 |
| 异常 | 断网、超时、对端关闭 | 错误可查询，可重建 |
| 多路径 | 单路径、双路径、路径切换 | 状态与流量统计正确 |
| 性能 | mini 与 API 同条件对比 | 达到阶段 6 门槛 |
| 稳定性 | 长时间、并发连接、循环创建销毁 | 内存与句柄稳定 |

## 9. 主要风险与控制措施

### 风险 1：连接级字节流与 HTTP/3 request 生命周期不完全一致

控制措施：定义 NetIns 私有持续数据协议，由 API 自动创建 request 和 response；把 headers、FIN 和 request close 全部封装在 backend 内。

### 风险 2：单隐藏通道达不到 mini 多 request 性能

控制措施：先用数据证明瓶颈；优先处理流控、队列、发送粒度和线程调度。只有确认结构性瓶颈后，才增加内部多 lane，避免过早引入重组复杂度。

### 风险 3：API 数据队列拖慢 ACK 和定时器

控制措施：继续使用有界批处理和总预算；文件 I/O 不进入协议主循环；通知和日志不进入每包热路径。

### 风险 4：server 共享资源被 accepted connection 误释放

控制措施：明确 listener、server engine/socket、accepted connection、default channel 的引用和销毁顺序，并为各种先后关闭组合增加测试。

### 风险 5：为了“像 socket”而误承诺 Linux socket 的全部语义

控制措施：文档明确这是可靠有序的 NetIns 数据接口，只承诺已实现的 flags、阻塞、超时和线程模型。

### 风险 6：底层 XQUIC 接口变化影响第三方 ABI

控制措施：所有 `xqc_*` 类型和调用留在私有 backend；公共头文件只暴露 NetIns 类型、固定宽度字段和版本化结构。

## 10. 交付完成定义

以下条件全部满足后，才能称为“可交付的简易高性能 API”：

1. 第三方只使用 `netins_udp.h + libxquic.a` 即可编写客户端和服务端；
2. 建连、监听、accept、收发、状态查询和关闭调用链完整；
3. 使用连接级 `send/recv` 时无需操作 HTTP/3 request；
4. 数据正确性、异常断网、路径切换、重连和长时间测试通过；
5. x86_64 与 AArch64 构建验证通过；
6. API 与 mini 同环境性能达到阶段 6 门槛；
7. 未改变 mini 主路径、调度器、拥塞控制和 XQUIC 公共 ABI；
8. 使用说明、示例、脚本、库文件和版本说明齐全。

## 11. 实施决策点

按以下决策点逐阶段确认：

- 决策点 A：阶段 1 完成后，确认公共 `send/recv` 语义，再进入实现；
- 决策点 B：阶段 5 完成后，确认功能闭环，再进行真实性能测试；
- 决策点 C：阶段 6 完成后，根据实测决定是否需要阶段 6.5；
- 决策点 D：双架构和交付包验证通过后，冻结首个正式 ABI。

任何阶段发现必须修改 mini、调度器、拥塞控制或 XQUIC 公共 ABI 才能继续时，应暂停并单独评审，不在本计划中直接扩大改动范围。

## 12. 工作量预估

在不实施内部多 lane 的情况下，预计总工作量约为：

- API 语义、头文件和 ABI：10%；
- 客户端默认通道：20%；
- 服务端默认通道：25%；
- 通知、错误和生命周期：15%；
- 测试与问题修复：20%；
- 文档、示例和交付包：10%。

若实测需要内部多 lane，整体工作量预计增加 30%～50%，主要增加在私有协议、有序重组、背压、异常恢复和性能回归测试。

执行时每个阶段独立 review、独立验证、独立提交，前一阶段未通过验收时不宣布进入下一阶段。
