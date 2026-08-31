# NetIns UDP Mini API

本仓库是 NetIns UDP Mini API 的源码交付版本，基于已验证的 XQUIC Mini 数据面，提供文件传输和 4-lane 半双工数据流 API、AARCH64 客户端 Demo、Linux x86_64 服务端 Demo，以及自动化压测脚本。

## 交付内容

```text
cmake/                         CMake 安装规则和 AARCH64 工具链
demo/                          文件 API 与 Stream API 客户端、服务端 Demo
include/xquic/netins_udp.h     第三方公开 C API
mini/                          Mini 客户端、服务端 backend
src/                           XQUIC 和 Mini API 实现
scripts/                       ABI/隔离检查和导出符号表
tests/                         文件与数据流 GET/POST 自动化压测及资源监控
```

第三方应用只需包含：

```c
#include <xquic/netins_udp.h>
```

第三方不应直接包含 `mini/` 或 `src/` 中的内部头文件。

## 文档入口

- [`文档/NETINS_UDP_API_使用说明.md`](文档/NETINS_UDP_API_使用说明.md)：公开结构体、配置项、生命周期、文件/数据流语义和 Demo；
- [`文档/NETINS_UDP_编译指南.md`](文档/NETINS_UDP_编译指南.md)：AARCH64 客户端和 Linux x86_64 服务端编译与可选安装；
- [`文档/STREAM_DEMO_数据流编译测试说明.md`](文档/STREAM_DEMO_数据流编译测试说明.md)：复用原编译环境，通过文件覆盖构建并测试文件 Demo 与数据流 Demo；
- [`文档/MINI_API_测试使用说明.md`](文档/MINI_API_测试使用说明.md)：文件自动化压测环境、文件完整性和资源监控。

## 构建依赖

源码构建需要目标架构对应的：

- Tongsuo/BabaSSL；
- Libevent 及 `libevent_pthreads`；
- zlog；
- AARCH64 构建还需要 SIM8810 SDK 和交叉编译工具链。

依赖库、外部覆盖证书、测试大文件及本地构建目录不包含在源码仓库中。Mini API 服务端包含
一套封闭环境兼容用的内置证书，部署时可不携带外部证书文件。具体 CMake 参数以
`文档/NETINS_UDP_编译指南.md` 为准。

## 主要产物

启用 `XQC_ENABLE_MINI_API=ON` 后生成：

```text
libxquic-static.a
libxquic.so
mini/netins_udp_mini_api_client
mini/netins_udp_mini_api_server
mini/netins_udp_stream_client
mini/netins_udp_stream_server
```

可选使用以下命令安装公开头文件、库、四个 Demo、`VERSION` 和检查脚本：

```bash
cmake --install BUILD_DIR --prefix INSTALL_DIR --component mini_api
```

正式交付物为完整源码仓库，不依赖额外打包脚本。文档和 `tests/stream_api_benchmark.py`
直接从源码树使用，不纳入 CMake 安装规则。

## 默认兼容行为

- Demo 未传 zlog 配置时不启用 zlog；
- 文件及数据流 GET 使用 `adaptive`，POST 客户端默认 `balanced` 并可选择调度器；
- GET 默认并发为 8，POST 默认并发为 4；
- 数据流 Demo 使用 4 lane，支持 `--get`/`--post` 半双工方向；客户端支持重复
  `--interface`，POST 支持 `--scheduler`；
- 单路径和双路径 RX 预算使用独立配置分支；
- 未传日志和调度参数时使用上述默认策略，速率汇总格式保持不变。

当前 Mini API 交付版本为 `6.5`，Stream 内部协议为 v4，项目版本也可见 `VERSION`。
