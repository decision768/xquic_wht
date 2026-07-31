# Mini API 阶段 3：生命周期稳定化

## 1. 对象与所有权

| 对象 | 独占资源 | 共享资源 |
|---|---|---|
| client backend | client engine、event base、engine event、路径 socket/event、连接 | 无 |
| server backend/listener | server engine、event base、engine event、listener UDP socket/event | 被 accepted handle 引用 |
| accepted connection | CID、H3 connection scope、peer/local 地址、连接级文件任务状态 | listener 的 engine、event base 和 UDP socket |
| request stream | H3 request、收发缓冲、文件 fd | 所属 accepted connection |

服务端 backend 使用引用计数。关闭 listener 后：

- 停止接受新连接；
- 主动关闭尚未交付的排队连接；
- 不关闭仍被 accepted handle 使用的 UDP socket、event base 和 engine；
- 最后一个 accepted handle 释放后才销毁共享 backend。

关闭超时时，不提前释放仍可能被 XQUIC 回调访问的 connection scope；该对象保留到回调完成或
engine 最终销毁，避免悬空 `user_data`。

## 2. 全局状态迁移

原服务端 `g_svr_file_state` 已迁入 `xqc_mini_svr_user_conn_t::file_task_state`：

- 每个 accepted connection 独立；
- 同一连接同一时间一个任务；
- 同一连接可顺序开始下一任务；
- `stream_done` 和 `stream_progress` 按任务片段数动态分配；
- 不再把 200 MiB 的 50 个片段或 1 GiB 的 256 个片段截断到旧的 16 项数组。

当前防御上限为 262144 个 4 MiB 片段。该上限不是公共可配置的分片参数。

客户端/服务端 callback 表已改为每次初始化的局部值，客户端和服务端同名的
`line_break` 已改为文件内符号，避免多实例和合并链接时共享可变状态或符号冲突。

API 模式还会禁用 CLI 的固定日志/keylog文件以及固定名称的 0-RTT
ticket/token/transport-parameter 文件，避免第三方进程的多 handle 竞争当前工作目录文件。

## 3. 事件循环模型

阶段 4 使用同步阻塞模型：

- `connect()`、`transfer()`、服务端 `accept()/serve()` 在调用线程中以
  `EVLOOP_ONCE` 阻塞推进原 Libevent 循环，事件处理完成后立即进入下一轮，
  不再使用 `EVLOOP_NONBLOCK + 1ms sleep`；独立超时事件保证 API
  仍能在调用方配置的期限内返回；
- `accept()` 在调用线程中推进同一个 server event base，直到 accepted queue 有连接或超时；
- `shutdown()` 在调用线程中推进关闭回调；
- `get_state()` 进行一次非阻塞 event poll；
- 未创建第二个 UDP 接收线程，也没有第二个 socket 接收者。

客户端 GET writer 线程仍只在原 GET 文件任务需要时创建，且只消费已复制的数据，不调用
XQUIC。

API 建连阶段不会自动启动 CLI 的 request 池。握手回调、路径重试回调和连接超时回调均识别
deferred-transfer 状态；阶段 5 提交任务后再进入原 request 调度链。

## 4. 错误与释放

已完成：

- 服务端 socket event 异常不再 `exit(1)`，改为记录错误并结束对应事件等待；
- `event_base_new()`、engine event、listener event、client timeout/path event 均检查失败；
- client/server context 释放 engine event 和 event base；
- listener 释放 UDP socket 和 socket event；
- connection/request 分配失败路径可安全回收；
- H3 正常/异常关闭记录底层错误码；
- shutdown 与 destroy 可处理 NULL 和半初始化对象。

释放顺序：

```text
停止新工作
  -> 请求 H3 connection 关闭并推进回调
  -> 停止任务 event / writer / path I/O
  -> 销毁 XQUIC engine
  -> 释放 connection/listener
  -> 释放 engine event、日志和 event base
  -> 释放参数与 backend
```

## 5. 验证状态

本机完成：

- CMake configure/generate；
- 四个共享对象目标在 Linux x86_64 Clang 21、仓库 `-Werror` 规则下编译通过；
- client/server CLI main 单独编译通过；
- 公共头文件以 C11 和 C++11 包含编译通过；
- client、server、API 对象执行 relocatable link，通过重复全局符号检查；
- `git diff --check`。

目标机仍需补跑：

- client/API 与 server/API 连续创建、握手、关闭 20 轮；
- listener 先关闭、accepted connection 后关闭；
- 正常关闭、对端异常关闭和超时；
- fd、线程和内存泄漏检查。

原因是当前 Windows 工作机没有目标证书、私钥、Linux 运行环境和双 5G 网卡。该部分状态为
`TARGET_ENV_PENDING`，不能用语法编译替代运行验收。
