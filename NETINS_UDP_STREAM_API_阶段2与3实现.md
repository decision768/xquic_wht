# NetIns UDP 数据流 API：阶段 2 与阶段 3 实现说明

## 1. 已实现范围

阶段 2 提供正式的单向逻辑数据流 API：

- `netins_udp_stream_open()` / `netins_udp_stream_accept()`；
- `write()`、`flush()`、`finish()`、`reset()`、`close()`；
- 应用线程到 XQUIC IO 线程的有界发送队列；
- 阻塞、超时、非阻塞和部分接收语义；
- 接收端连续 offset 回调；
- FIN、长度校验、FNV-1a 摘要和完成回执。

阶段 3 在同一套 API 上加入 1～8 个 lane：

- 连续输入按不超过 64 KiB 的 DATA record 切分，并轮询放入各 lane 的独立发送队列；
- 每个 record 携带逻辑 offset，lane 内保持 QUIC 原生可靠有序；
- lane 之间允许乱序，接收端通过有界重排缓存只向应用交付连续数据；
- 每个 lane 都提交相同最终长度的 FIN record；
- lane 0 返回统一完成回执，其余 lane 关闭反向发送方向；
- 阶段 2 曾用 `lane_count=1` 验证相同协议的单 Bytestream 退化路径；当前交付公共 API 已固定为 4 lane。

阶段 2 的独立提交为 `82d13c24`。阶段 3 不改变原文件 API 的公开函数和普通 `h3`
连接行为。

## 2. 调用流程

客户端和服务端都必须在建立连接前启用流模式：

```c
int enabled = 1;
netins_udp_set_opt(handle, NETINS_UDP_OPT_STREAM_MODE,
    &enabled, sizeof(enabled));
```

发送端：

```c
netins_udp_connect(client, server_ip, port);

netins_udp_stream_config_t cfg = NETINS_UDP_STREAM_CONFIG_INITIALIZER;

netins_udp_stream_t *stream = NULL;
netins_udp_stream_open(client, &cfg, &stream);

while (producer_has_data()) {
    size_t accepted = 0;
    netins_udp_stream_write(stream, data, data_len, 30000, &accepted);
    /* 如果返回超时或 EAGAIN，先处理 accepted，再决定是否重试剩余数据。 */
}

netins_udp_stream_result_t result =
    NETINS_UDP_STREAM_RESULT_INITIALIZER;
netins_udp_stream_finish(stream, 60000, &result);
netins_udp_stream_close(stream);
```

接收端：

```c
netins_udp_t *connection = netins_udp_accept(server);

netins_udp_stream_config_t cfg = NETINS_UDP_STREAM_CONFIG_INITIALIZER;
cfg.on_data = on_stream_data;
cfg.user_data = app_context;

netins_udp_stream_t *stream = NULL;
netins_udp_stream_accept(connection, &cfg, &stream);

netins_udp_stream_result_t result =
    NETINS_UDP_STREAM_RESULT_INITIALIZER;
netins_udp_stream_finish(stream, 60000, &result);
netins_udp_stream_close(stream);
```

`open()` 端是数据生产者，`accept()` 端是数据消费者。API version 1 不把完成回执方向暴露
成第二条应用数据流。

## 3. 线程与背压

- 客户端活动逻辑流由一个内部 I/O 线程独占对应的 XQUIC/libevent event base；
- 流模式服务端由一个共享 I/O 线程驱动所有 accepted connection，每条连接各自维护
  `stream_api_state`，因此可以同时推进多条连接；
- 应用 `write()` 只复制数据到有界队列，不直接从应用线程调用 XQUIC；
- pipe event 唤醒 IO 线程，没有固定间隔 sleep；
- 队列满时，阻塞模式等待空间，非阻塞模式返回 `EAGAIN`；超时可能已经接收部分数据，
  调用方必须读取 `accepted`；
- `flush()` 只证明本地队列已经交给 XQUIC；`finish()` 才等待对端长度和摘要回执；
- `on_data()` 在每条接收流的有界 delivery worker 执行，不持有 stream mutex，也不会阻塞
  XQUIC I/O；内存只在回调期间有效，过慢会耗尽 `receive_queue_size`；
- 当前一个连接只允许一个活动逻辑流，但同一个服务端 backend 可以同时驱动多条连接；
- 活动流关闭后才能关闭连接，也不能与文件传输在同一连接中并发执行。

## 4. 多 lane 线上格式

每个 Bytestream 先发送 40 字节 lane header：

| 字段 | 作用 |
|---|---|
| magic / version / header size | 协议识别与版本校验 |
| session ID | 把多个 lane 绑定到同一逻辑流 |
| lane index / lane count | lane 唯一性与数量校验 |
| max frame payload | 当前固定为 64 KiB |

随后是 24 字节 record header 和可选 payload：

| 类型 | offset | payload |
|---|---:|---|
| DATA | 数据在逻辑流中的起始位置 | 1～64 KiB |
| FIN | 整个逻辑流最终字节数 | 0 |

接收端拒绝重复 lane、session 不一致、DATA offset 重叠、越界帧、不同 lane 最终长度不一致
以及超过 `receive_queue_size` 的乱序缓存。全部数据连续交付后，lane 0 返回 48 字节统一
回执，包含状态、session、总字节数和摘要。

## 5. 构建与基础测试

在原 Linux/AARCH64 CMake 参数上启用 Mini API：

```bash
-DXQC_ENABLE_MINI_API=ON
```

构建：

```bash
cmake --build BUILD_DIR --target \
  mini_stream_protocol_test \
  mini_stream_reorder_test \
  netins_udp_stream_server \
  netins_udp_stream_client \
  --parallel "$(nproc)"
```

纯协议和重排测试：

```bash
BUILD_DIR/mini/mini_stream_protocol_test
BUILD_DIR/mini/mini_stream_reorder_test
```

Linux 回环测试，示例为 64 MiB、固定 4 lane、seed 7。lane 已在两个 Demo 内部固定，
命令行不再传入 lane 数：

```bash
BUILD_DIR/mini/netins_udp_stream_server \
  0.0.0.0 8443 67108864 7

BUILD_DIR/mini/netins_udp_stream_client \
  127.0.0.1 8443 67108864 7
```

成功时双方输出：

```text
[stream_api_result] ... success=1 ...
```

## 6. 尚未完成的目标环境验收

当前完成的是代码实现、严格源码编译、CMake 对象目标构建、ABI 导出检查以及纯协议/重排
单测。由于本机不直接控制主板和云端，以下项目必须在目标 Linux/AARCH64 环境补做：

- 使用真实 Tongsuo、Libevent 和 zlog 完整链接；
- 固定 4 lane 的回环传输和逐 lane 统计；
- 主板到云端的双网卡/多路径传输；
- 丢包、乱序、lane 中断、生产速度高于网络速度时的背压；
- 文件 API 回归以及流 API 长时间稳定性和吞吐对比。
