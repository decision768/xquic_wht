# Mini API 阶段 10：构建、ABI 与正式交付

阶段 10 提供：

- 公开头文件、静态库、共享库和两个 demo 的 `mini_api` component 安装规则；
- 共享库 SONAME `libxquic.so.1`，Mini API 版本 `1`；
- Linux 共享库 version script；
- public header/version script/实际动态库三方 ABI 检查；
- SIM8810 AArch64 工具链文件；
- 只收集正式文件的安装与打包脚本；
- API 使用、编译、release notes 和版本文件。

预期安装树：

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

仓库检查确认：

- 只有 `include/xquic/netins_udp.h` 一套 NetIns UDP 公共 API；
- 所有公开函数均在 `scripts/xquic.lds`；
- demo 不含 mini 私有头文件或 `xqc_*` 调用；
- public API 不含 stream count、fragment size 或 RX budget option；
- component install 不包含 tests、阶段诊断和内部 backend 头文件。

当前 Windows 工作区没有目标架构 TLS/Libevent/zlog 完整库，不能生成可运行的最终 `.a/.so`
和安装包。两个目标架构完整链接、`nm` 实库检查与打包状态为 `TARGET_ENV_PENDING`；命令已经
固化在 `NETINS_UDP_编译指南.md`。
