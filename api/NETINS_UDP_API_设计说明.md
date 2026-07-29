# NetIns UDP API 架构与 ABI 说明

## 1. 公共边界

第三方交付只有一个公共头文件：

```text
include/xquic/netins_udp.h
```

公共符号包含统一对象、连接级 send/recv、HTTP/3 request 扩展、状态统计、
通知和错误查询接口。
仓库内部为复用 mini observer 和既有独立 client 而保留的兼容接口位于：

```text
src/netins_udp/netins_udp_legacy.h
```

该头文件不安装，其符号不进入 `libxquic.so` 的全局导出表，不构成第三方 ABI。
静态库内保留实现是为了让 mini 和统一 API 共享 observer 生命周期适配、路径
元数据和有界/公平 RX 策略，而不是维护两套对外接口。mini 与独立 API backend
仍分别拥有各自既有的 engine、socket、event loop 和连接/request 生命周期；
公共 adapter 不接管这些对象，也不会为 mini 增加第二个 UDP 接收者。

## 2. 分层

```text
第三方程序
    |
    v
netins_udp.h
    |
    +-- 连接对象层：client / listener / accepted connection
    +-- 连接级可靠有序数据：send / recv
    +-- HTTP/3 request 对象层
    +-- snapshot / path statistics / close reason
    +-- 独立 eventfd/pipe 通知
    |
    v
NetIns backend adapters
    |
    +-- 既有 client engine/thread/socket
    +-- server shared engine/socket + accepted connection refs
    +-- 私有 observer
    |
    v
原 XQUIC engine / scheduler / congestion control / UDP I/O
```

这一分层不修改 XQUIC 的拥塞控制、调度器、ACK、定时器和 multipath 核心
逻辑，也不向第三方开放 UDP socket。

## 3. 对象和所有权

`netins_udp_t` 是不透明对象，根据生命周期拥有三种角色：

- client：独占 client transport；
- listener：拥有共享 server backend 的主引用；
- accepted connection：持有共享 server backend 引用，同时独立拥有 QUIC
  connection close 生命周期。

`netins_udp_h3_request_t` 是不透明子对象。每个 request 持有自己的 backend
引用，因此父连接开始关闭后仍可安全查询并 close。父连接 close 会中止并
detach 子对象，但不会替应用释放子对象地址。

连接级 `send/recv` 在每个 client 或 accepted connection 内部拥有一组隐藏的
并行 HTTP/3 数据通道。数据由私有 backend 自动分发，并按全局字节偏移重排成
一个连续有序字节流。这些通道由 connection 自动创建、绑定和释放，不暴露给
调用方，也不要求调用方处理 headers。显式创建的高级 request 仍由应用独立
释放；同一 connection 不允许混用两种数据使用方式。

连接级运行时选项使用明确的快照边界：request 在 open/accept 时复制
`NONBLOCK` 和 send/recv/close timeout，后续父对象修改不追溯到既有 request；
accepted connection 在 accept 时继承 listener 选项，此后与 listener 独立。
这样避免并发中的半次调用改变阻塞策略，也不要求父对象反向持有公开子句柄。

日志回调同样属于连接对象，而不属于 XQUIC engine。默认回调为空，因此公共
API 不会主动输出诊断文本。accepted connection 在 accept 时复制 listener 的
logger 和日志级别，之后独立修改。回调执行计数纳入对象关闭同步：
`set_logger(NULL)` 和 `close()` 都会等待已进入的回调结束。回调消息只包含低频
生命周期、路径可用性变化和异常；稳定统计继续由私有 observer 快照提供，不把
逐包诊断或内部策略参数变成公共日志 ABI。

公共 API 创建的 XQUIC connection 保持 `mp_diag_enabled=0`，因此旧的 scheduler、
ACK 和路径诊断 `printf` 不会绕过 logger 直接输出。mini 测试程序仍可通过自己
原有的诊断开关启用这些内部报告；该开关不属于第三方 API。

端点建立可能在底层 transport 分配前失败。此时公共对象仍保留 ERROR 状态和
配置元数据，info/snapshot 返回空 transport 统计，保证调用方始终能够完成
“查询失败原因—记录状态—close”的生命周期。

listener close：

- 停止接收新连接；
- 唤醒阻塞 accept；
- 释放 listener 自己的引用；
- 不立即销毁仍被 accepted connection 引用的共享 backend。

## 4. 数据面

普通应用的数据面是可靠、有序的连接级字节数据，内部由多条持续的 HTTP/3
request/response 通道并行承载。发送端给每个内部数据帧分配全局字节偏移，
接收端只向应用交付连续前缀，从而在多个内部通道完成顺序不同时仍保持 socket
式字节流语义。该接口不保留每次 send 的消息边界，也不声称兼容裸 UDP 数据报
或 Linux socket 的全部 flags。

client 到 server 的发送方向保持最多 4 条活动数据通道，server 到 client 的
发送方向保持最多 8 条活动数据通道，对应既有 mini 上传、下载方向的并发能力。
这些数量、内部帧格式、重排预算和 HTTP/3 对象均属于私有实现，不进入公共 ABI。
文件内容与普通内存数据统一通过 `send/recv` 传输，不再交付独立的公共文件 API。

应用线程通过连接级接口提交或取得数据：

- connection send/recv：私有并行 request body 与 response body；
- client request：headers、请求 body、响应 body；
- server request：请求 accept、请求 body、response headers、响应 body；
- 连接级数据 backend 使用自己的版本化内部帧，不把内部 headers、通道编号或
  字节偏移暴露给应用；空 authority 按 HTTPS authority 规则补全主机、IPv6
  方括号和非默认端口；
- send/recv 支持部分完成、超时和非阻塞；
- 服务端公开关闭 request 后，未读大 POST 进入丢弃式 drain；
- client 以每 request 预留量和每连接发送/接收总预算共同限制并发队列；
- server 以每 request、每 accepted connection 和全 server 三级发送/接收
  总预算限制并发队列，防止无界占用内存。

## 5. 通知

connection、listener、accepted connection 和 request 各自维护独立 notifier。
notifier 使用 eventfd/pipe，仅作为 readiness 唤醒，不承载网络数据。

隐藏数据通道的 READABLE/WRITABLE 会映射到所属 connection notifier。
隐藏通道的 CLOSED/ERROR 只转换为一次数据面唤醒，使后续 `send/recv`
返回通道错误；它们不会伪装成 connection 的 CLOSED/ERROR。连接终止事件只由
QUIC transport 状态产生。客户端建连后即报告连接级 WRITABLE；accepted
connection 检测到默认数据 request 后即报告 READABLE/WRITABLE，因此不要求
调用方先试探性调用一次 `send/recv`。

状态变化事件被锁存；readable/writable/acceptable 等为 level 条件。
`get_events()` 负责原子消费、重新计算 level 并在必要时重置唤醒，从而避免：

- 丢失状态变化；
- 多对象共用一个通知状态；
- 应用直接读取 UDP socket；
- level 条件仍成立却丢唤醒。

## 6. 统计

统一 API 使用私有 observer 生成连接快照，不扩展或改变
`xqc_conn_get_stats()`：

- connection info 描述协商状态和路径数量；
- connection statistics 在简易 `send/recv` 模式下以调用方实际提交和取得的
  字节作为应用字节，不计私有通道帧头；wire bytes、ACK、loss proxy、
  inflight 和区间速率仍来自传输 observer；
- path statistics 描述稳定 path index、地址、接口、RTT、CWND 和逐路径流量；
- request info 单独统计 HTTP body 字节。

逐路径唯一发送字节归属于首次发送路径，逐路径唯一接收字节归属于提供新消费
stream offset 的保留副本所在路径。关闭原因采用首次终止原因冻结语义，后续
只能补充尚未记录的原始 transport error，不能改写已经公开的来源。

path index 在连接生命周期内只分配、不复用，避免路径关闭和重建后第三方把同一
数组位置误认为另一条路径。超过公开上限时通过 `path_table_truncated` 报告。

## 7. ABI 规则

- 唯一公开命名前缀为 `netins_udp_`。
- ABI 兼容标识由 `NETINS_UDP_API_VERSION` 和
  `netins_udp_get_api_version()` 共同校验。
- 跨 ABI 的结构体包含 `struct_size`、`api_version` 和 reserved 字段。
- 调用方必须使用 initializer；库校验兼容标识和最小结构大小。
- 枚举值、event bit、option id 和 path index 语义冻结。
- 新字段优先消耗 reserved 空间；破坏布局或语义的改变必须更新 ABI 兼容标识。
- 内部文件名中可能保留开发阶段标识，但不出现在安装头、安装示例和动态库
  导出契约中。

静态守卫 `tests/netins_udp/verify_netins_udp_api.py` 检查：

- 只安装最终头文件；
- 公共声明都有实现和动态库导出；
- 兼容 backend 声明未泄露、符号未导出；
- 不发布 raw XQUIC stream、通用 SOCKOPT 或旧式 client/observer 入口；
- 公共文件传输 demo 和数据回环测试只使用连接级 `send/recv` API；
- 已移除的文件专用接口不会重新出现在安装头、导出表或构建目标中。

## 8. 线程模型

- process init/cleanup 使用全局引用计数和锁；
- 每个公开对象保护自身状态、错误、active-call 计数和关闭同步；
- close 阻止新调用，取消 backend 等待，并等待已进入调用退出后释放；
- 应用必须保证 close 返回后不再使用指针；
- XQUIC engine 的协议处理仍在原 worker/Libevent 线程中串行执行。

因此 API 层可以安全地跨不同对象并发，但不会通过改变 engine 并发模型来提高
吞吐，也不会引入第二个 UDP 接收线程。

## 9. 构建和交付

安装规则交付：

- `netins_udp.h`
- `libxquic.so`
- `libxquic.a`
- README、使用说明、架构说明、公共 client/server demo 和压力脚本

正式发布验收应包括：

- Linux x86_64 和 AArch64 编译、链接、符号表检查；
- 本机及真实两端 client/server GET、POST；
- 大于 16 MiB body、并发 request、提前响应和提前 close；
- listener 先关、accepted 先关、断网、超时、重连和重复创建销毁；
- epoll 不丢唤醒、不持续忙循环；
- 长时间运行内存、句柄、线程和无进展监控；
- mini_client/mini_server 原路径回归，确认未出现第二套 UDP 接收者。
