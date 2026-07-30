# NetIns UDP Mini API 使用说明

## 1. 接入边界

应用只包含 `<xquic/netins_udp.h>`，不要包含 mini 私有头文件，也不要直接操作 XQUIC engine、
CID、HTTP/3 request、Libevent 或 stream count。

API 是阻塞式完整文件任务接口：

```text
init -> new -> set_opt -> connect/listen/accept
     -> transfer/serve -> snapshot -> shutdown -> close -> cleanup
```

一个 handle 不能被多个线程并发调用；共享同一 server backend 的 listener 和 accepted handle
也必须由调用方串行访问。

## 2. 客户端最小流程

```c
netins_udp_t *client;
netins_udp_transfer_t task = NETINS_UDP_TRANSFER_INITIALIZER;
netins_udp_transfer_result_t result =
    NETINS_UDP_TRANSFER_RESULT_INITIALIZER;

netins_udp_init();
client = netins_udp_new(NETINS_UDP_ROLE_CLIENT);
netins_udp_connect(client, "10.12.54.143", 8443);

task.direction = NETINS_UDP_TRANSFER_GET;
task.remote_path = "/payload.bin";
task.local_path = "./payload.bin";
task.timeout_ms = 300000;
netins_udp_transfer(client, &task, &result);

netins_udp_shutdown(client);
netins_udp_close(client);
netins_udp_cleanup();
```

生产代码必须检查每个返回值。失败返回 `-1` 并设置 `errno`；连接级详情使用
`netins_udp_get_last_error_code()`、`netins_udp_get_last_error_msg()` 和
`netins_udp_get_close_reason()`。

## 3. 服务端最小流程

server 在 listen 前必须通过 `NETINS_UDP_OPT_TLS` 设置证书和私钥。`accept()` 返回独立
accepted handle，随后调用：

```c
netins_udp_serve_config_t serve = NETINS_UDP_SERVE_CONFIG_INITIALIZER;
serve.root_path = "/srv/mini-api";
serve.timeout_ms = 0;
netins_udp_serve(connection, &serve);
```

远端 URL path 被限制在 `root_path` 下；含 `..` 或反斜杠的路径被拒绝。

## 4. 配置

所有配置结构体必须使用对应 `*_INITIALIZER`，保留正确的 `struct_size` 和 `api_version`。
除 logger 外，配置仅能在 `NEW` 状态设置。

| option | 类型 | 作用 |
|---|---|---|
| `NETINS_UDP_OPT_INTERFACES` | `netins_udp_interfaces_t` | client 的 1～2 个网卡 |
| `NETINS_UDP_OPT_SCHEDULER` | `int` | 原 mini 调度器 |
| `NETINS_UDP_OPT_CONGESTION_CONTROL` | `int` | default/CUBIC/BBR |
| `NETINS_UDP_OPT_TLS` | `netins_udp_tls_config_t` | client SNI 或 server 证书/私钥 |
| `NETINS_UDP_OPT_CONNECT_TIMEOUT_MS` | `uint32_t` | 建连超时 |
| `NETINS_UDP_OPT_ACCEPT_TIMEOUT_MS` | `uint32_t` | accept 超时 |
| `NETINS_UDP_OPT_CLOSE_TIMEOUT_MS` | `uint32_t` | 关闭超时 |
| `NETINS_UDP_OPT_KERNEL_SNDBUF` | `int` | UDP 发送缓冲 |
| `NETINS_UDP_OPT_KERNEL_RCVBUF` | `int` | UDP 接收缓冲 |
| `NETINS_UDP_OPT_LOGGER` | `netins_udp_logger_config_t` | 同步日志回调 |

logger 回调中的 message 只在当前回调期间有效，且不能重入同一 handle。空 callback 会抑制
API 驱动期间原 mini 的 `printf()`；原 CLI 的 stdout 行为不变。

## 5. 固定传输语义

- 分片固定 4 MiB；
- GET 固定最多 8 个并发 request；
- POST 固定最多 4 个并发 request；
- GET 先获取 `x-total-length`；
- 使用 offset `pread()`/`pwrite()`；
- 全部 request 完成后任务才返回，GET 还等待异步写队列和 `fsync()`；
- 同一连接可顺序执行多个任务，不能同时执行多个任务；
- 任务超时会关闭连接，防止迟到 request 继续写文件；
- 0 字节文件合法。

## 6. 状态与路径

`netins_udp_get_snapshot()` 返回连接/握手、多路径状态、收发包、RTT、连接级 inflight、应用
字节以及最多 8 条路径。数据来自当前 mini engine/CID 的 `xqc_conn_get_stats()`，不解析日志。

当前 XQUIC 公共 path metrics 不提供每路径 CWND 和 inflight；API v1 的对应预留字段返回 0，
连接级 `snapshot.bytes_inflight` 正常返回。

## 7. 所有权

- `netins_udp_init()/cleanup()` 进程级引用计数配对；
- `netins_udp_new()` 和 `netins_udp_accept()` 返回的 handle 均由调用方 `close()` 一次；
- `set_opt()` 复制配置与字符串，调用返回后可释放输入；
- 最后一次 `cleanup()` 时仍存在 handle 会返回 `EBUSY`；
- listener 关闭不会使已接受 connection 的共享 backend 失效。

完整参考程序位于交付包 `bin/`，对应源码为仓库 `demo/netins_udp_mini_api_client.c` 和
`demo/netins_udp_mini_api_server.c`。
