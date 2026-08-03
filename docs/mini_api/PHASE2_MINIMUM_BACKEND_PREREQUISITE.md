# Mini API 阶段 2 最小前置抽取

## 1. 范围

阶段 3 和阶段 4 不能直接从 `main()` 管理私有上下文，因此本轮先完成了它们所需的最小
阶段 2 前置工作，但没有提前实现阶段 5 的 GET/POST 任务 API。

当前结构为：

```text
mini_client_main.c ─┐
                    ├─ mini_client_backend_obj ─┐
NetIns client API ──┘                           │
                                                ├─ 原 mini / XQUIC 数据面
mini_server_main.c ─┐                           │
                    ├─ mini_server_backend_obj ─┘
NetIns server API ──┘
```

公共对象目标：

- `mini_common_backend_obj`
- `mini_client_backend_obj`
- `mini_server_backend_obj`
- `netins_udp_mini_api_obj`

原 `mini_client`、`mini_server` 与公共 API 均链接这些对象，不存在第二份 RX、H3 或文件传输
实现。

## 2. 已建立的内部入口

客户端：

```c
mini_backend_client_create()
mini_backend_client_connect()
mini_backend_client_run()
mini_backend_client_poll()
mini_backend_client_shutdown()
mini_backend_client_destroy()
```

服务端：

```c
mini_backend_server_create()
mini_backend_server_listen()
mini_backend_server_run()
mini_backend_server_poll()
mini_backend_server_accept()
mini_backend_server_shutdown()
mini_backend_server_release()
mini_backend_server_connection_shutdown()
mini_backend_server_connection_release()
```

`mini_client_main.c` 和 `mini_server_main.c` 已改为调用上述 backend。配置文件读取仍保留，
因此 CLI 的运行配置入口没有被移除。

## 3. 保持不变的数据面

本次没有复制或替换下列调用链：

- 客户端 Libevent UDP RX 和单路径/双路径/多路径独立预算的公平收包（当前每路径均为 256）；
- 单个 XQUIC engine；
- H3 request 回调；
- 4 MiB 分片；
- `pread()` / `pwrite()` 偏移读写；
- request 完成后的自动补充；
- 原路径创建、断链和恢复逻辑；
- 原调度器和拥塞控制回调。

公共 backend 当前只负责创建、连接、监听、接受、事件推进和销毁。传输任务入口将在阶段 5
把现有 request 池接入，不在本轮增加临时数据协议。

## 4. 构建入口

当前定制 AArch64 根构建默认启用：

```text
XQC_ENABLE_MINI_API=ON
```

`LIBEVENT_*`、`ZLOG_INCLUDE_DIR` 和 `ZLOG_STATIC_LIB` 已改为 CMake cache 变量，原硬编码路径仍是
默认值。标准 `xqc_build.sh` 使用的 `cmake/CMakeLists.txt` 也支持显式打开
`XQC_ENABLE_MINI_API`，但必须传入上述依赖路径。

完整安装、双架构工具链和交付目录仍属于阶段 10。
