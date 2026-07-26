# NetIns UDP API 架构与 ABI 说明

## 1. 公共边界

第三方交付只有一个公共头文件：

```text
include/xquic/netins_udp.h
```

公共符号只有统一对象、HTTP/3 request、状态统计、通知和错误查询接口。
仓库内部为复用 mini observer 和既有独立 client 而保留的兼容接口位于：

```text
src/netins_udp/netins_udp_legacy.h
```

该头文件不安装，其符号不进入 `libxquic.so` 的全局导出表，不构成第三方 ABI。
静态库内保留实现是为了让 mini 和统一 API 共用同一 backend，而不是维护两套
对外接口。

## 2. 分层

```text
第三方程序
    |
    v
netins_udp.h
    |
    +-- 连接对象层：client / listener / accepted connection
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

listener close：

- 停止接收新连接；
- 唤醒阻塞 accept；
- 释放 listener 自己的引用；
- 不立即销毁仍被 accepted connection 引用的共享 backend。

## 4. 数据面

公共数据面固定为 HTTP/3 request，而不是连接级原始字节流。原因是 QUIC
原生提供多 stream；如果把连接伪装为 TCP 字节流，会产生 stream 选择、
消息边界、并发和关闭语义冲突。

应用线程只操作有界队列，不直接调用 XQUIC：

- client request：headers、请求 body、响应 body；
- server request：请求 accept、请求 body、response headers、响应 body；
- send/recv 支持部分完成、超时和非阻塞；
- 服务端公开关闭 request 后，未读大 POST 进入丢弃式 drain；
- 每连接及全 server 有发送/接收总预算，防止并发 request 无界占用内存。

## 5. 通知

connection、listener、accepted connection 和 request 各自维护独立 notifier。
notifier 使用 eventfd/pipe，仅作为 readiness 唤醒，不承载网络数据。

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
- connection statistics 描述去重 stream payload、wire bytes、ACK、loss
  proxy、inflight 和区间速率；
- path statistics 描述稳定 path index、地址、接口、RTT、CWND 和逐路径流量；
- request info 单独统计 HTTP body 字节。

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
- 不发布 raw stream、通用 SOCKOPT 或旧式 client/observer 入口。

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
- README、使用说明、架构说明和统一示例

正式发布验收应包括：

- Linux x86_64 和 AArch64 编译、链接、符号表检查；
- 本机及真实两端 client/server GET、POST；
- 大于 16 MiB body、并发 request、提前响应和提前 close；
- listener 先关、accepted 先关、断网、超时、重连和重复创建销毁；
- epoll 不丢唤醒、不持续忙循环；
- 长时间运行内存、句柄、线程和无进展监控；
- mini_client/mini_server 原路径回归，确认未出现第二套 UDP 接收者。
