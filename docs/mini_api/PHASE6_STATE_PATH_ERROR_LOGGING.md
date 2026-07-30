# Mini API 阶段 6：状态、路径、错误与日志

## 1. 公共查询接口

阶段 6 新增：

```c
int netins_udp_is_multipath(netins_udp_t *handle);
int netins_udp_get_path_count(netins_udp_t *handle, uint32_t *out_count);
int netins_udp_get_path_statis(netins_udp_t *handle,
    size_t path_index, netins_udp_path_statis_t *out);
int netins_udp_get_snapshot(netins_udp_t *handle,
    netins_udp_snapshot_t *out);
int netins_udp_get_close_reason(netins_udp_t *handle,
    netins_udp_close_reason_t *out);
```

所有结构体继续使用 `struct_size` 和 `api_version`。查询 client 或 accepted connection 时，
backend 使用该对象真实的 engine 和 CID 调用：

```c
xqc_conn_get_stats(engine, &cid);
```

不解析日志，不注册第二套 observer 数据面，也不修改 `xqc_conn_get_stats()` 或其他 XQUIC
公共 ABI。

## 2. Snapshot 内容

连接级 snapshot 包含：

- API role/state 和握手状态；
- 协商后的 multipath mode 与 MP state；
- 当前路径数和 ACTIVE 路径数；
- 收发包、丢包和伪丢包计数；
- SRTT、min RTT 和连接级 inflight bytes；
- XQUIC 累计 application bytes 和 standby-path application bytes；
- 最多 8 条路径统计；
- 关闭来源、errno 风格错误和 XQUIC transport error。

路径级统计包含：

- path ID、稳定的本次 snapshot 索引；
- transport path state 和 application path status；
- 客户端接口名；
- 可获得的本地/对端数字地址；
- 收发包和收发字节；
- reinjection 字节；
- effective receive bytes；
- path SRTT。

当前 XQUIC 公共 `xqc_path_metrics_t` 不提供路径级 CWND 和 inflight。公共结构预留了
`cwnd_bytes`/`bytes_inflight`，API version 1 返回 0；连接级 inflight 正常返回。不能通过
解析 `conn_info` 或私有 transport 结构伪造这两个值，否则会违反“不改 XQUIC 公共 ABI”
和“不解析日志”的要求。

## 3. 关闭原因

当前关闭来源：

```text
NONE / LOCAL / PEER / TRANSPORT / TIMEOUT / INTERNAL
```

- 主动 `shutdown()` 记录 LOCAL；
- 正常对端关闭记录 PEER；
- XQUIC 非零关闭码记录 TRANSPORT；
- 任务或关闭超时记录 TIMEOUT；
- Libevent/backend 处理失败记录 INTERNAL。

错误查询不会依赖进程退出或日志文本。`get_state()`、`get_snapshot()` 和
`get_close_reason()` 会推进一次非阻塞事件轮询，以便观察最近的关闭状态。

## 4. 可选日志回调

`NETINS_UDP_OPT_LOGGER` 接受 `netins_udp_logger_config_t`，可在创建前或运行中替换：

- `callback == NULL` 时，API 调用期间抑制原 mini 的 `printf()` 诊断；
- 配置 callback 后，原 mini 同步回调中的 `printf()` 被路由到调用方；
- `[diag...]`、普通信息、`[warn]`、`[error]` 分别映射为 DEBUG/INFO/WARN/ERROR；
- logger 还接收公共生命周期和错误消息；
- CLI 未安装 API 日志 scope，保持原 stdout 行为；
- 回调只在当前 API 调用驱动事件循环的线程中同步执行。

logger 回调必须快速返回，并且不能重入同一 handle。`perror()` 和第三方依赖直接写入
stderr 的内容不属于 `printf()` 路由范围。

## 5. 验证状态

本机已完成：

- client/server snapshot adapter 和公共 API 在仓库 `-Werror` 下编译；
- C11/C++11 公共结构初始化器编译；
- logger 的级别过滤、API 静默 scope 和 CLI stdout fallback 静态/链接检查；
- API、client、server 对象合并链接检查。

目标机仍需验证：

- 单路径报告 1 条 ACTIVE 路径；
- 双路径报告 2 条 ACTIVE 路径；
- 断开/恢复路径后数量和状态更新；
- 实际流量、RTT、inflight 与目标机诊断对照；
- 正常关闭、对端异常关闭、超时的 close reason。

状态为 `TARGET_ENV_PENDING`。
