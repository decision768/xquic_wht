# NetIns UDP SDK 5.4.2

NetIns UDP SDK 基于 XQUIC 1.8.3，提供可靠、有序的 QUIC/多路径 QUIC
客户端和服务端 C API。第三方程序只需包含公共头文件：

```c
#include <xquic/netins_udp.h>
```

常用调用链：

```text
客户端：init → new → set_opt → connect → send/recv → shutdown → close → cleanup
服务端：init → new → set_opt → listen → accept → recv/send → shutdown → close → cleanup
```

## 交付内容

```text
include/xquic/                 公共头文件
src/netins_udp/                NetIns UDP实现
src/                           XQUIC核心实现
demo/netins_udp_data_client.c  客户端API Demo
demo/netins_udp_data_server.c  服务端API Demo
cmake/toolchains/              x86_64与AArch64构建配置
scripts/netins_udp_api_stress.sh
NETINS_UDP_API_使用说明.md
NETINS_UDP_编译指南.md
RELEASE_NOTES.md
VERSION
```

`src/` 中的头文件属于库内部实现，不是公共API。第三方程序不得直接依赖这些
私有头文件。

## 主要能力

- 客户端连接、服务端监听和连接接受；
- 可靠、有序的 `netins_udp_send()` / `netins_udp_recv()`；
- 单路径QUIC和多路径QUIC；
- 连接状态、多路径状态和逐路径统计；
- 阻塞与非阻塞调用、超时和通知fd；
- 错误码、关闭原因和可选日志回调；
- Linux x86_64服务端与SIM8810 AArch64客户端构建。

## 快速构建

完整的依赖路径和平台命令见
[NETINS_UDP_编译指南.md](NETINS_UDP_编译指南.md)。

配置时默认构建两个UDP API Demo：

```sh
cmake -S . -B build [平台和依赖参数]
cmake --build build -j$(nproc)
```

也可以分别构建：

```sh
cmake --build build --target netins_udp_data_client -j$(nproc)
cmake --build build --target netins_udp_data_server -j$(nproc)
```

安装：

```sh
cmake --install build --prefix /opt/netins-udp
```

安装结果包括公共头文件、静态库、动态库、两个Demo程序和交付文档。

## Demo

AArch64客户端：

```sh
./netins_udp_data_client \
  SERVER_IP 18446 SERVER_NAME CA_FILE \
  SEND_FILE RECEIVE_FILE \
  rmnet_data0 usb0
```

x86_64服务端：

```sh
./netins_udp_data_server \
  '*' 18446 \
  server.crt server.key \
  RECEIVE_FILE SEND_FILE
```

Demo只使用 `netins_udp.h` 中的公共API，不调用mini或XQUIC内部接口。参数和
运行方法见[NETINS_UDP_API_使用说明.md](NETINS_UDP_API_使用说明.md)。

## API与库版本

程序启动时建议检查头文件和运行库是否匹配：

```c
if (netins_udp_get_api_version() != NETINS_UDP_API_VERSION) {
    return -1;
}
```

当前公共API版本为2。应用必须使用同一交付包中的头文件和库文件。

## 日志与统计

默认不注册日志回调时，SDK不向应用输出运行日志。需要连接生命周期或异常信息
时可注册日志回调；路径流量、RTT、CWND和inflight应通过snapshot与逐路径统计
接口查询，不应解析日志文本。

## 文档

- [NetIns UDP API使用说明](NETINS_UDP_API_使用说明.md)
- [Linux x86_64与AArch64编译指南](NETINS_UDP_编译指南.md)
- [版本说明](RELEASE_NOTES.md)
- [开源许可](LICENSE)
