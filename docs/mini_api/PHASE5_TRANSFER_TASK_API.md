# Mini API 阶段 5：GET/POST 传输任务

## 1. 公共接口

阶段 5 新增：

```c
int netins_udp_transfer(netins_udp_t *client,
    const netins_udp_transfer_t *task,
    netins_udp_transfer_result_t *result);

int netins_udp_serve(netins_udp_t *connection,
    const netins_udp_serve_config_t *config);
```

`netins_udp_transfer()` 只接受已连接的 client handle。每次调用同步执行一个完整文件任务，
同一连接上的任务串行执行。任务完成后保留 H3 connection，可以继续提交下一任务；超时会关闭
连接，避免已超时 request 在调用返回后继续修改文件。

`netins_udp_serve()` 只接受 accepted handle。它配置该连接的服务根目录，并在调用线程中持续
推进原 server Libevent 循环，直到对端关闭或服务超时。listener 和 accepted handle 仍使用
阶段 3 定义的共享 backend 所有权。

## 2. 数据面复用

公共任务没有新的帧协议、常驻数据通道或第二个收包线程。调用链直接进入原 mini：

```text
netins_udp_transfer()
  -> mini_backend_client_transfer()
  -> xqc_mini_cli_prepare_transfer()
  -> xqc_mini_cli_launch_requests()
  -> xqc_mini_cli_try_schedule_requests()
  -> xqc_mini_cli_send_h3_req()
  -> 原 H3 request callback / pread / async pwrite
```

固定策略：

- 逻辑分片固定为 4 MiB；
- POST request 并发固定为 4；
- GET request 并发固定为 8；
- GET 先用首 request 获取 `x-total-length`，再扩展完整 request 数；
- request 完成后由原补充逻辑维持并发；
- RX 继续使用原 Libevent socket、单路径 256 和双路径每路径 256，选择分支保持独立；
- 公共 API 不提供分片、并发和 RX budget 配置项。

## 3. 文件与路径语义

客户端任务：

- GET：`remote_path` 是服务端路径，`local_path` 是下载输出文件；
- POST：`local_path` 是上传源文件，`remote_path` 是服务端输出路径；
- `remote_path` 必须是类似 `/data.bin` 的绝对 URL path；
- 0 字节任务仍使用一个 request；
- GET 响应始终携带 `x-total-length`，包括值为 0 的文件。

服务端：

- `root_path` 必须是已存在目录；
- API GET/POST 的远端路径均解析到该 accepted connection 的 `root_path`；
- 拒绝 `..` 路径分量和反斜杠，避免直接越出服务根目录；
- 原 mini_server 的 `data_dir/default_send_file/upload_path` 行为保持不变。

## 4. 完成与结果

一次任务仅在全部 request 关闭后完成。GET 还必须等待异步 `pwrite()` 队列清空和 `fsync()`。

结果包含：

- `total_bytes`；
- `total_fragments`；
- `completed_fragments`；
- `elapsed_us`；
- `throughput_mbps`。

调用仍在全部 request 完成后返回；成功结果的测速区间与原 Mini 一致：GET 为首个响应体字节
至最后一个响应体字节，POST 为首次请求体提交至全部请求体提交完成。POST 指标是应用提交速率，
不代表服务端已经完整接收。

HTTP 非 2xx、文件打开/读写错误、连接关闭和超时通过 errno 风格错误返回，同时写入连接级
错误消息。HTTP 文件不存在不会主动关闭可复用连接；任务超时会关闭连接。

## 5. 当前验证

本机已完成：

- API 与 mini backend 对象在 Linux x86_64 Clang 21、仓库 `-Werror` 下编译；
- 公共头文件通过 C11 和 C++11 初始化器编译；
- 0 字节 GET/POST 分支静态检查；
- API 固定并发和 4 MiB 分片调用链检查；
- `git diff --check`。

以下真实运行验收仍为 `TARGET_ENV_PENDING`：

- 0、1、4 MiB 边界、200 MiB、1 GiB 的 GET/POST；
- client/API ↔ server/API 及与原 mini 的交叉组合；
- SHA-256 一致性；
- 4/8 并发和单/双路径 256/256 日志观察；
- 同一连接连续任务和超时后的关闭行为。
