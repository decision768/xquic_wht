# NetIns Mini API 5.4.2

本交付基于已验证的 `v5.4.2_final` mini 数据面，将原 mini_client/mini_server 的连接、双路径
RX、公平调度、HTTP/3 request 和文件分片能力封装为第三方可调用的 C API。

## 交付内容

```text
include/xquic/netins_udp.h
lib/libxquic-static.a
lib/libxquic.so
bin/netins_udp_mini_api_client
bin/netins_udp_mini_api_server
README.md
NETINS_UDP_API_使用说明.md
NETINS_UDP_编译指南.md
RELEASE_NOTES.md
VERSION
```

第三方只需包含：

```c
#include <xquic/netins_udp.h>
```

API 使用完整 GET/POST 文件任务模型。4 MiB 分片、GET 8 并发、POST 4 并发和单路径
256/双路径每路径 256 的 RX 策略固定在内部，调用方不能改变；单双路径预算仍独立选择。

快速验证：

```bash
bin/netins_udp_mini_api_server --help
bin/netins_udp_mini_api_client --help
```

两个 demo 默认保持原有压测行为：API 日志回调使用 DEBUG 阈值，zlog 关闭，服务端调度器为
adaptive。可选参数 `--log-level 0..3` 用于等级过滤；传入 `--zlog-config FILE` 才会启用
zlog；服务端可用 `--scheduler NAME` 覆盖 adaptive。未传这些新参数时，既有压测命令和
速率摘要格式不变。

当前 demo 对未知参数（包括 `--help`）打印完整 usage 并返回非 0，这是命令帮助行为，不代表
库初始化失败。

详细集成方法见根目录 `NETINS_UDP_API_使用说明.md`。该文档列出全部 20 个公开函数，
并给出客户端建连、GET/POST 文件收发、服务端 listen/accept/serve、状态查询、错误处理、
shutdown/close 以及完整 C 示例。构建和打包见 `NETINS_UDP_编译指南.md`。

目标 5G 环境的性能与断链报告必须使用仓库阶段 8/9 工具生成。源代码仓库中的
`TARGET_ENV_PENDING` 表示尚未在当前构建机上伪造或代填实测结果。
