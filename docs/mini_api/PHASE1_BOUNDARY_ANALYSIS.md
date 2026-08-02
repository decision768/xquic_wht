# Mini API 阶段1：Mini 边界分析

## 1. 结论

现有 mini 已经具备可抽取 backend 所需的大部分上下文结构和带前缀函数。
第一轮不需要移动数据面代码，适合采用 adapter + CMake 对象库方式，把
`main()` 中的编排拆成内部生命周期入口。

不能直接把客户端和服务端源文件简单合并到一个库：当前存在进程级/文件级
共享状态、重复全局符号、资源释放缺口，以及“任务完成即关闭连接”的命令行
程序语义。阶段2、3必须先解决这些边界问题，同时保持 RX、H3 request、文件
偏移读写和路径管理调用链不变。

## 2. 当前入口与建议 backend 映射

### 2.1 客户端

| 当前入口 | 当前职责 | 建议内部入口 |
|---|---|---|
| `xqc_mini_cli_init_env()` | 默认参数、配置文件、event base、日志 | `mini_backend_client_create()` 的一部分 |
| `xqc_mini_cli_init_xquic_engine()` | 创建唯一 client engine 和 engine event | `mini_backend_client_create()` 的一部分 |
| `xqc_mini_cli_init_engine_ctx()` | 注册 H3 回调 | `mini_backend_client_create()` 的一部分 |
| `xqc_mini_cli_user_conn_create()` | connection、定时器、路径、任务初始状态 | `mini_backend_client_connect()` 的准备阶段 |
| `xqc_mini_cli_main_process()` | 调用 `xqc_h3_connect()` | `mini_backend_client_connect()` |
| `xqc_mini_cli_launch_requests()` | 启动 request 池 | `mini_backend_client_transfer()` |
| `xqc_mini_cli_try_schedule_requests()` | 完成后补充 request | 原样保留在 transfer backend |
| `event_base_dispatch()` | 阻塞运行事件循环 | backend 的阻塞等待实现 |
| `xqc_mini_cli_on_connection_finish()` | 停止任务相关 event、writer 和路径 I/O | `mini_backend_shutdown()` 的连接部分 |
| `xqc_mini_cli_free_user_conn()` | 释放 connection 与 path 状态 | `mini_backend_destroy()` 的连接部分 |
| `xqc_engine_destroy()` + `xqc_mini_cli_free_ctx()` | 释放 engine/配置/日志 | `mini_backend_destroy()` 的 client 部分 |

`mini_client_main.c` 中的 `init_args()`、`create_connection()`、`start_event()`、
`close_all()` 是命令行适配层雏形。阶段2应保留相同执行顺序，但改为调用正式
backend 入口。

### 2.2 服务端

| 当前入口 | 当前职责 | 建议内部入口 |
|---|---|---|
| `xqc_mini_svr_init_env()` | 默认参数、配置文件、event base、日志 | `mini_backend_server_create()` 的一部分 |
| `xqc_mini_svr_init_xquic_engine()` | 创建唯一 server engine 和 engine event | `mini_backend_server_create()` 的一部分 |
| `xqc_mini_svr_init_engine_ctx()` | server settings 与 H3 回调 | `mini_backend_server_create()` 的一部分 |
| `xqc_mini_svr_create_user_conn()` | 创建 listener UDP socket 和 read event | `mini_backend_server_listen()` |
| `xqc_mini_svr_h3_conn_create_notify()` | 创建实际 connection-scope 对象 | `mini_backend_server_accept()` 的生产端 |
| `xqc_mini_svr_h3_conn_close_notify()` | 释放 connection-scope 对象 | accepted connection 关闭路径 |
| `event_base_dispatch()` | 阻塞运行 listener 事件循环 | server backend 的 serve loop |
| `xqc_mini_svr_free_user_conn()` | 当前只释放地址和对象 | `mini_backend_destroy()` 的 listener 部分 |
| `xqc_engine_destroy()` + `xqc_mini_svr_free_ctx()` | 释放 engine/配置/日志 | `mini_backend_destroy()` 的 server 部分 |

`xqc_mini_svr_accept()` 当前是空回调。真正的连接级对象在
`xqc_mini_svr_h3_conn_create_notify()` 中分配。API 的 `netins_udp_accept()`
必须由该回调向 listener 的 accepted queue 发布对象并发出条件通知，不能创建
新的 UDP socket 或第二个接收线程。

## 3. 状态清单

### 3.1 进程级或隐式共享状态

| 状态 | 位置 | 当前问题 | 目标归属 |
|---|---|---|---|
| `g_svr_file_state` | `mini_server_cb.c:42` | 所有连接和 POST 任务共享；新连接还会重置它 | accepted connection 或 transfer task |
| client `callback`/`transport_cbs` | `mini_client.c:746,769` | 函数内 `static`，回调表会按日志配置修改 | 只读模板或每 engine 局部值 |
| server `callback`/`transport_cbs` | `mini_server.c:381,408` | 同上 | 只读模板或每 engine 局部值 |
| `line_break` | client/server callback 文件 | 两边各定义一次同名外部符号 | 改为文件内 `static const` 或删除 |
| `READ_FILE_BUF_LEN` | `mini/common.h:44` | 头文件中的可变 `static`，每个翻译单元各一份 | 宏或 `static const` |
| zlog 全局运行时 | `zlog_init()/zlog_fini()` | client/server context 都可能独立初始化和结束进程级库 | `netins_udp_init/cleanup` 引用计数 |
| `session_ticket`、`transport_params`、`token` 文件 | 客户端固定文件名 | 多连接/多实例会竞争同一工作目录文件 | handle 配置或进程级受控存储 |

另外，客户端的 `net_cfg.last_socket_time` 是运行时状态，却存放在配置结构体中。
它应迁入 connection/task 状态，避免多个连接共享和污染配置。

### 3.2 客户端对象状态

`xqc_mini_cli_ctx_t`：

- `event_base`
- 单个 XQUIC client engine
- engine timer event
- 参数对象
- log/keylog fd
- zlog category

`xqc_mini_cli_user_conn_t`：

- CID 与 H3 connection
- 最多2条 path 的 socket、event、地址、RX batch 和诊断
- request 池：目标数、最大并发、next/active/completed
- POST 文件大小、路径和汇总状态
- GET 输出 fd、异步写线程、队列、mutex/condition、进度和耗时
- connection timeout 与 path retry event
- RX 仲裁、延迟释放和单双路径独立预算模式状态（当前均为 256）

`xqc_mini_cli_user_stream_t`：

- 所属 connection 和 stream index
- 当前4 MiB 逻辑片段的 offset/length
- POST 输入 `FILE *`、发送 buffer 和发送进度
- GET 响应 offset、长度和状态
- H3 request、请求头及头字段存储

### 3.3 服务端对象状态

`xqc_mini_svr_ctx_t`：

- `event_base`
- 单个 XQUIC server engine
- engine timer event
- 参数、日志和 zlog
- `current_fd`；当前 write callback 通过它选择 listener socket

listener `xqc_mini_svr_user_conn_t`：

- 唯一 UDP socket 和持久 read event
- listener 本地、最近 peer 地址
- backend context

accepted connection-scope `xqc_mini_svr_user_conn_t`：

- 指向 listener 的 `listener_conn`
- 与 listener 共用同一个 fd、engine 和 context
- 独立 CID、本地/peer 地址
- 连接级路径和 GET 提交诊断

`xqc_mini_svr_user_stream_t`：

- H3 request 与解析后的 GET/POST metadata
- 输入/输出文件 fd
- request 接收 buffer
- GET `pread()` offset、发送 cache 和进度
- POST `pwrite()` offset、完成和耗时状态

## 4. 资源创建与释放顺序

### 4.1 客户端当前顺序

```text
main栈上创建 ctx
  -> calloc(args)
  -> event_base_new()
  -> 日志/zlog/keylog
  -> xqc_engine_create()
  -> event_new(ev_engine)
  -> calloc(user_conn)
  -> mutex/condition
  -> timeout/path-retry events
  -> path socket/address/RX batch/socket event
  -> xqc_h3_connect()
  -> event_base_dispatch()
  -> H3/request/path/timer callbacks
  -> GET异步pwrite线程（仅需要时创建）
  -> request完成后 xqc_h3_conn_close()
  -> H3 close callback执行 event_base_loopbreak()
  -> 停writer、释放path和connection events
  -> xqc_engine_destroy()
  -> 关闭日志并释放args
```

当前缺口：

- `xqc_mini_cli_free_ctx()` 没有 `event_free(ev_engine)` 和
  `event_base_free(eb)`；
- 部分初始化失败后仍走统一 `close_all()`，需要保证 NULL/半初始化安全；
- connection 完成会主动关闭 H3，当前不能在同一连接顺序执行第二个任务；
- `on_connection_finish()` 与 `free_user_conn()` 有重复清理逻辑，阶段3应收敛为
  幂等、单向的 shutdown/destroy；
- RX arbiter 正在运行时，释放通过 `rx_free_pending` 延迟到公平轮次末，这一
  机制必须保留。

### 4.2 服务端当前顺序

```text
main栈上创建 ctx
  -> calloc(args)
  -> event_base_new()
  -> 日志/zlog/keylog
  -> xqc_engine_create()
  -> event_new(ev_engine)
  -> calloc(listener_conn)
  -> listener UDP socket
  -> event_new(ev_socket, EV_READ|EV_PERSIST)
  -> event_base_dispatch()
  -> socket回调把包交给同一engine
  -> H3 create回调分配accepted connection-scope
  -> request create回调分配stream
  -> request close回调释放stream资源
  -> H3 close回调释放accepted connection-scope
  -> 外部停止事件循环
  -> xqc_engine_destroy()
  -> 释放ctx和listener对象
```

当前缺口：

- listener 与 accepted connection 已有逻辑区分，但没有 accept queue、等待条件
  和独立公共 handle；
- accepted connection 共享 listener fd 和 engine。关闭 listener 时不能立即
  销毁它们，否则会破坏存量 accepted connection；
- `xqc_mini_svr_free_user_conn()` 没有关闭 UDP fd，也没有
  `event_del/event_free(ev_socket)`；
- `xqc_mini_svr_free_ctx()` 没有释放 `ev_engine` 和 `event_base`；
- server event loop 没有内部正常停止入口；
- `xqc_mini_svr_create_socket()` 使用 `if (!user_conn->fd)` 判断失败，
  无法识别 `-1`；
- `xqc_mini_svr_create_user_conn()` 在检查 `calloc()` 结果前解引用；
- `xqc_mini_svr_free_user_conn()` 在 NULL 检查前访问对象字段。

## 5. 线程和回调归属

| 执行上下文 | 内容 |
|---|---|
| 调用/event-loop线程 | Libevent engine timer、socket事件、所有 XQUIC transport/H3 回调、request 调度、路径创建/恢复、客户端 RX 仲裁、服务端 `pread/pwrite` |
| 客户端 GET writer线程 | 消费 connection 写队列，对共享下载 fd 执行偏移 `pwrite()` |
| 其他线程 | 当前没有 |

必须保持：

- UDP socket 只由 Libevent socket callback 接收；
- 客户端不新增第二个 RX 线程；
- 单个 engine 的 XQUIC 调用继续由其 event-loop 路径驱动；
- writer 线程只处理已复制到队列的数据，不直接调用 XQUIC。

阻塞 API 的建议模型是由 backend 在调用线程运行/推进 event loop，并以明确的
完成、超时或关闭条件退出。若以后支持后台事件线程，应作为后续显式设计，
不能在第一版暗中增加 transport/RX 线程。

## 6. `exit()` 与错误传播清单

实际可执行的进程退出调用只有一处：

```text
mini/mini_server.c:881  xqc_mini_svr_socket_event_callback() -> exit(1)
```

它必须改为：

- 记录 listener/connection last error；
- 请求 backend 停止对应事件循环；
- 由公共 API 返回 errno 风格错误；
- 不结束第三方进程。

`mini_client_main.c` 和 `mini_server_main.c` 中的 `exit:` 是 `goto` 标签，不是
`exit()` 调用。不过两个 `main()` 当前即使初始化或传输失败也返回0，公共
backend 必须保留真实错误码，命令行 adapter 再把错误码转换为进程返回值。

当前大量错误路径只 `printf()` 后返回通用 `XQC_ERROR`，还有少量 callback
只打印后继续。阶段3、4需要为 context/connection 增加：

- last error code；
- last error message；
- close reason；
- 完成/失败状态；
- 可选日志回调。

## 7. 文件任务边界

必须原样复用的传输函数链包括：

- 客户端分片：`xqc_mini_cli_compute_stream_segment()`
- request 创建：`xqc_mini_cli_send_h3_req()`
- request 池：`xqc_mini_cli_try_schedule_requests()`
- POST 文件读取和发送：`xqc_mini_cli_request_send()`
- GET 首请求扩展：`xqc_mini_cli_h3_request_read_notify()`
- GET 异步偏移写：`xqc_mini_cli_async_pwrite()`
- 服务端 GET 偏移读：`xqc_mini_svr_pread_file_chunk()`
- 服务端 POST 偏移写：`xqc_mini_svr_h3_request_read_notify()`
- 服务端响应发送：`xqc_mini_svr_send_body()`

现有协议头保持：

```text
x-stream-index
x-stream-count
x-stream-offset
x-total-length
```

### 大文件跟踪问题

客户端允许总 fragment 数超过16，16只是最大同时 request 数。例如：

```text
200 MiB / 4 MiB = 50 fragments
1 GiB / 4 MiB = 256 fragments
```

但服务端 POST 汇总状态使用固定16项数组，并把 `expected_streams` 截断到16：

- `mini_server_cb.c:915-919`
- `mini_server_cb.c:1047-1053`
- `mini_server_cb.c:1094-1096`
- `mini_server_cb.c:1126-1128`

第16项之后的数据仍可能通过 `pwrite()` 写入正确偏移，但不会进入进度、完成数
和聚合统计；服务器还可能在前16项完成时提前打印“全部完成”。这不属于要保留
的数据面语义。阶段3/5必须把任务跟踪改为按总 fragment 数动态分配或使用无
固定上限的计数结构，否则无法可信验收200 MiB和1 GiB POST。

## 8. listener 与 accepted connection 约束

当前服务端已经证明了以下所有权关系：

```text
server context / engine
        |
listener connection ---- UDP socket + Libevent read event
        |
        +---- accepted connection A ---- CID/H3/peer/task state
        |
        +---- accepted connection B ---- CID/H3/peer/task state
```

因此 API 对象映射建议：

- server handle：拥有 context、engine、listener socket、accept queue；
- accepted handle：引用 server backend，拥有 CID、H3 和单任务状态；
- server backend：引用计数或等价生命周期保护；
- listener shutdown：停止接受新连接；
- listener close：释放公共 listener handle，但 engine/socket 的最终释放必须
  等待 accepted handle 安全关闭；
- accepted close：只关闭自己的 XQUIC connection 和任务，不结束 listener。

第一版可限制每进程一个 listener、每 accepted connection 同时一个任务，但
必须显式返回 busy/state 错误。

## 9. 链接同一 backend 前必须处理的符号问题

如果一个 API 库同时链接 client 和 server 对象，当前至少有以下重复符号：

```text
line_break
xqc_mini_cli_on_connection_finish
```

第二个符号在 `mini_server.c` 中是误用的 client 前缀。两套 mini executable
分开链接时没有暴露该问题，合并到 API 库会发生冲突。

另外，当前 `MINI_CLIENT_SOURCES` 和 `MINI_SERVER_SOURCES` 都各自包含同一组
`HQ_SOURCES` 和 `common.c`。对象库拆分时应使用：

```text
mini_common_backend_obj
mini_client_backend_obj
mini_server_backend_obj
```

公共 HQ/common 对象只链接一次，client/server 对象不包含各自的 `main.c`。

## 10. 阶段2实施边界

阶段2应只完成以下动作：

1. 新建内部 backend 头文件和 context wrapper；
2. 将 `main()` 的创建、运行、关闭顺序移入 backend 入口；
3. 原 mini executable 改为调用 backend；
4. CMake 使用共享对象库，消除重复符号；
5. 为后续错误返回补充必要返回值，但不改变 RX、H3、分片、并发调度和文件 I/O；
6. 用调用链和符号检查证明 mini 与 API adapter 使用同一份实现。

以下动作留给阶段3及以后：

- accepted queue 和完整公共 `accept()` 行为；
- 全局 POST 状态迁移；
- 连接复用和顺序多任务；
- init/cleanup 引用计数；
- 公共错误、状态、日志和 observer；
- 公共 `netins_udp.h` ABI。

## 11. 阶段1验收结论

- 已列出 client/server backend 候选入口；
- 已列出全局、context、connection、path、stream 和 task 状态；
- 已梳理资源创建/释放顺序；
- 已明确 event-loop 与异步 writer 线程归属；
- 已确认唯一真实 `exit()`；
- 已明确 listener/accepted 对象映射；
- 已识别共享对象库的重复符号和大文件状态上限。

本阶段没有改动 mini 运行逻辑。
