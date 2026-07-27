#include <errno.h>
#include <stdio.h>

#include <xquic/netins_udp.h>

static void
example_logger(int level, const char *message)
{
    static const char *names[] = {"DEBUG", "INFO", "WARN", "ERROR"};
    const char *name = level >= NETINS_UDP_LOG_LEVEL_DEBUG
            && level <= NETINS_UDP_LOG_LEVEL_ERROR
        ? names[level] : "UNKNOWN";
    fprintf(stderr, "[netins-udp:%s] %s\n", name, message);
}

int
netins_udp_client_lifecycle_example(const char *host, int port,
    const char *first_interface, const char *second_interface)
{
    if (netins_udp_init() != NETINS_UDP_OK) {
        return -1;
    }

    netins_udp_t *client = netins_udp_new(
        second_interface != NULL
            ? NETINS_UDP_TYPE_MPQUIC : NETINS_UDP_TYPE_QUIC);
    if (client == NULL) {
        (void)netins_udp_cleanup();
        return -1;
    }
    if (netins_udp_set_logger(client, example_logger) != NETINS_UDP_OK) {
        goto error;
    }

    int protocol = NETINS_UDP_APP_HTTP3;
    if (netins_udp_set_opt(client, NETINS_UDP_OPT_APP_PROTOCOL,
            &protocol, sizeof(protocol)) != NETINS_UDP_OK)
    {
        goto error;
    }

    netins_udp_tls_config_t tls = NETINS_UDP_TLS_CONFIG_INITIALIZER;
    /* The project's current test server normally uses a self-signed cert. */
    tls.verify_peer = 0;
    if (netins_udp_set_opt(client, NETINS_UDP_OPT_TLS,
            &tls, sizeof(tls)) != NETINS_UDP_OK)
    {
        goto error;
    }

    if (first_interface != NULL) {
        netins_udp_interfaces_t interfaces =
            NETINS_UDP_INTERFACES_INITIALIZER;
        snprintf(interfaces.names[interfaces.interface_count++],
            NETINS_UDP_INTERFACE_NAME_LEN, "%s", first_interface);
        if (second_interface != NULL) {
            snprintf(interfaces.names[interfaces.interface_count++],
                NETINS_UDP_INTERFACE_NAME_LEN, "%s", second_interface);
        }
        if (netins_udp_set_opt(client, NETINS_UDP_OPT_INTERFACES,
                &interfaces, sizeof(interfaces)) != NETINS_UDP_OK)
        {
            goto error;
        }
    }

    if (netins_udp_connect(client, host, port) != NETINS_UDP_OK) {
        goto error;
    }

    printf("client state after connect: %d\n",
        netins_udp_get_state(client));
    netins_udp_close(client);
    return netins_udp_cleanup();

error:
    {
        int call_errno = errno;
        char message[NETINS_UDP_ERROR_MSG_LEN] = {0};
        (void)netins_udp_get_last_error_msg(client, message,
            sizeof(message));
        fprintf(stderr,
            "client lifecycle failed: errno=%d, handle_error=%d, %s\n",
            call_errno, netins_udp_get_last_error_code(client),
            message[0] != '\0' ? message : "no error message");
    }
    netins_udp_close(client);
    (void)netins_udp_cleanup();
    return -1;
}
