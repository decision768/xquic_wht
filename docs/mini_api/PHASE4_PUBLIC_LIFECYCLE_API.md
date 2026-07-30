# Mini API 阶段 4：公共生命周期 API

## 1. 公共头文件

第三方只需包含：

```c
#include <xquic/netins_udp.h>
```

阶段 4 已实现并导出：

```c
netins_udp_get_api_version()
netins_udp_init()
netins_udp_cleanup()
netins_udp_new()
netins_udp_set_opt()
netins_udp_connect()
netins_udp_listen()
netins_udp_accept()
netins_udp_get_state()
netins_udp_shutdown()
netins_udp_close()
netins_udp_get_last_error_code()
netins_udp_get_last_error_msg()
```

公开头文件不包含 mini 私有头文件，也不向调用方暴露 `xqc_*`、HTTP/3 request 或 stream
count。阶段 5 的 `netins_udp_transfer()` / `netins_udp_serve()` 尚未加入。

## 2. ABI 与结构体

- `NETINS_UDP_API_VERSION` 当前为 `1`；
- 可扩展配置结构体带 `struct_size` 和 `api_version`；
- 结构体尾部包含保留字段；
- `set_opt()` 在调用期间复制结构体和字符串，调用方可立即回收输入；
- lifecycle 函数成功返回 0，失败返回 -1 并设置 `errno`；
- 有效 handle 同时保存连接级错误码和错误消息。

当前角色：

```text
NETINS_UDP_ROLE_CLIENT
NETINS_UDP_ROLE_SERVER
NETINS_UDP_ROLE_ACCEPTED
```

当前状态：

```text
NEW -> CONNECTING -> CONNECTED -> CLOSING -> CLOSED
NEW -> LISTENING  -> CLOSING   -> CLOSED
任意运行阶段 -> ERROR
```

## 3. 配置项

| 配置项 | 值类型 | 说明 |
|---|---|---|
| `NETINS_UDP_OPT_INTERFACES` | `netins_udp_interfaces_t` | 仅 client，1～2 个接口名 |
| `NETINS_UDP_OPT_SCHEDULER` | `int` | mini 已有调度器枚举 |
| `NETINS_UDP_OPT_CONGESTION_CONTROL` | `int` | default/BBR/CUBIC |
| `NETINS_UDP_OPT_TLS` | `netins_udp_tls_config_t` | server 必须提供证书和私钥；client 可提供 SNI |
| `NETINS_UDP_OPT_CONNECT_TIMEOUT_MS` | `uint32_t` | 仅 client，阻塞握手超时 |
| `NETINS_UDP_OPT_ACCEPT_TIMEOUT_MS` | `uint32_t` | 仅 server；0 表示无限等待 |
| `NETINS_UDP_OPT_CLOSE_TIMEOUT_MS` | `uint32_t` | 关闭等待；0 表示无限等待 |
| `NETINS_UDP_OPT_KERNEL_SNDBUF` | `int` | 正整数 |
| `NETINS_UDP_OPT_KERNEL_RCVBUF` | `int` | 正整数 |

所有配置必须在 `NEW` 状态设置。当前 mini 数据面仅支持数字 IPv4 地址；TLS SNI 通过
`tls.server_name` 单独设置。客户端证书认证本版不支持。

GET=8、POST=4 是内部默认并发，公共 API 不提供 stream count 配置项。

## 4. 客户端生命周期示例

```c
netins_udp_t *client = NULL;
netins_udp_interfaces_t ifaces = NETINS_UDP_INTERFACES_INITIALIZER;
netins_udp_tls_config_t tls = NETINS_UDP_TLS_CONFIG_INITIALIZER;

if (netins_udp_init() != 0) {
    return 1;
}

client = netins_udp_new(NETINS_UDP_ROLE_CLIENT);
if (client == NULL) {
    netins_udp_cleanup();
    return 1;
}

ifaces.interface_count = 2;
snprintf(ifaces.names[0], sizeof(ifaces.names[0]), "wwan0");
snprintf(ifaces.names[1], sizeof(ifaces.names[1]), "wwan1");
snprintf(tls.server_name, sizeof(tls.server_name), "mini.example");

if (netins_udp_set_opt(client, NETINS_UDP_OPT_INTERFACES,
        &ifaces, sizeof(ifaces)) != 0
    || netins_udp_set_opt(client, NETINS_UDP_OPT_TLS,
        &tls, sizeof(tls)) != 0
    || netins_udp_connect(client, "10.12.54.143", 8443) != 0)
{
    char message[NETINS_UDP_ERROR_MSG_LEN];
    netins_udp_get_last_error_msg(client, message, sizeof(message));
}

netins_udp_shutdown(client);
netins_udp_close(client);
netins_udp_cleanup();
```

## 5. 服务端生命周期示例

```c
netins_udp_t *server = NULL;
netins_udp_t *connection = NULL;
netins_udp_tls_config_t tls = NETINS_UDP_TLS_CONFIG_INITIALIZER;

netins_udp_init();
server = netins_udp_new(NETINS_UDP_ROLE_SERVER);

snprintf(tls.certificate_file, sizeof(tls.certificate_file),
    "/secure/server.crt");
snprintf(tls.private_key_file, sizeof(tls.private_key_file),
    "/secure/server.key");

netins_udp_set_opt(server, NETINS_UDP_OPT_TLS, &tls, sizeof(tls));
if (netins_udp_listen(server, "0.0.0.0", 8443) == 0) {
    connection = netins_udp_accept(server);
}

/*
 * listener 与 accepted connection 是独立 handle。
 * 先关闭 server 不会销毁 connection 仍需使用的共享 backend。
 */
netins_udp_close(server);
netins_udp_shutdown(connection);
netins_udp_close(connection);
netins_udp_cleanup();
```

调用方必须串行访问同一 handle，以及共享同一个 server backend 的 accepted handles。

## 6. 验收状态

已满足：

- 第三方头文件不依赖 mini；
- lifecycle API 不要求调用方调用任何 `xqc_*`；
- connect/listen/accept 直接驱动原 mini 的 engine、Libevent 和 UDP socket；
- listener/accepted ownership 分离；
- 正常关闭与底层异常关闭均有状态、errno 风格错误和连接级错误消息；
- 公共符号加入导出列表；
- C/C++ 头文件和共享对象目标编译通过。

目标 Linux 机器上的真实握手、accept、异常断链和 20 轮生命周期运行测试仍为
`TARGET_ENV_PENDING`。
