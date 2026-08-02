# NetIns Mini API Release Notes

## 5.4.2-mini-api.1

首次 Mini API 交付候选版。

新增：

- 进程级初始化/清理与 client/server/accepted handle 生命周期；
- 配置网卡、调度器、拥塞控制、TLS、超时、socket buffer 和日志回调；
- 同步 GET/POST 完整文件任务；
- API 成功结果使用原 Mini 测速口径（GET 首/末响应体，POST 应用提交）；
- 固定 4 MiB 分片、GET 8 并发、POST 4 并发；
- 状态、multipath、路径统计、snapshot、错误和 close reason；
- 只依赖公开头文件的 client/server demo；
- Linux x86_64 与 SIM8810 AArch64 构建入口；
- 安装、ABI 检查、打包和阶段 8/9 目标机测试工具。

架构保持：

- API 和原 mini 共享同一组 client/server backend 对象；
- 复用原 Libevent、UDP socket、单 engine、H3 request 和 `pread/pwrite` 数据面；
- 单路径 256、双路径每路径 256 的公平 RX 机制未开放为配置，单双路径选择分支仍保留；
- 未修改 XQUIC 既有公共 ABI；
- 未引入第二个 UDP 接收者或自定义持续字节流协议。

已在当前工作区完成静态/交叉对象级验证：

- backend/API/demo 的 C11 `-Werror` 编译；
- 公共头文件 C11/C++11 编译；
- API/backend 合并可重定位链接；
- header 与 version script ABI 清单检查；
- 阶段 8/9 采集器模拟自测。

以下正式验收必须在具备 TLS、Linux x86_64/SIM8810 和双 5G 的目标环境执行：

- 四种 client/server 组合的 240 轮兼容与性能矩阵；
- 95% 性能等价、80% 聚合效率及可选 205 Mbps 指标；
- 真实 5G1/5G2 断链恢复、异常退出和长时间稳定性；
- 两个目标架构的完整链接、运行、安装包与最终 SHA-256。

这些项目当前标记为 `TARGET_ENV_PENDING`，本 release note 不宣称已经通过目标机实测。
