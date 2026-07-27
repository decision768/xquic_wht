#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include <xquic/netins_udp.h>

/* Minimal blocking QUIC/HTTP/3 connection example. */
int
netins_udp_connect_example(const char *server_address, uint16_t server_port,
    const char *server_name, const char *first_interface,
    const char *second_interface)
{
    if (server_address == NULL || server_address[0] == '\0'
        || server_port == 0)
    {
        errno = EINVAL;
        return NETINS_UDP_ERROR;
    }

    if (netins_udp_init() != NETINS_UDP_OK) {
        return NETINS_UDP_ERROR;
    }

    int result = NETINS_UDP_ERROR;
    int use_multipath = second_interface != NULL
        && second_interface[0] != '\0';
    netins_udp_t *client = netins_udp_new(use_multipath
        ? NETINS_UDP_TYPE_MPQUIC : NETINS_UDP_TYPE_QUIC);
    if (client == NULL) {
        goto done;
    }

    netins_udp_tls_config_t tls = NETINS_UDP_TLS_CONFIG_INITIALIZER;
    /*
     * Keep verification enabled for normal deployments. The supplied name
     * must match the certificate. Test deployments using a self-signed
     * certificate can explicitly set tls.verify_peer = 0.
     */
    if (server_name != NULL) {
        snprintf(tls.server_name, sizeof(tls.server_name), "%s",
            server_name);
    }
    if (netins_udp_set_opt(client, NETINS_UDP_OPT_TLS,
            &tls, sizeof(tls)) != NETINS_UDP_OK)
    {
        goto done;
    }

    if (first_interface != NULL && first_interface[0] != '\0') {
        netins_udp_interfaces_t interfaces =
            NETINS_UDP_INTERFACES_INITIALIZER;
        snprintf(interfaces.names[interfaces.interface_count++],
            NETINS_UDP_INTERFACE_NAME_LEN, "%s", first_interface);
        if (use_multipath) {
            snprintf(interfaces.names[interfaces.interface_count++],
                NETINS_UDP_INTERFACE_NAME_LEN, "%s", second_interface);
        }
        if (netins_udp_set_opt(client, NETINS_UDP_OPT_INTERFACES,
                &interfaces, sizeof(interfaces)) != NETINS_UDP_OK)
        {
            goto done;
        }
    }

    if (netins_udp_connect(client, server_address, (int)server_port)
        != NETINS_UDP_OK)
    {
        goto done;
    }

    netins_udp_snapshot_t snapshot = NETINS_UDP_SNAPSHOT_INITIALIZER;
    if (netins_udp_get_snapshot(client, &snapshot) != NETINS_UDP_OK) {
        goto done;
    }
    printf("connected id=%" PRIu64 " state=%d paths=%u active=%u"
           " multipath=%d\n",
        snapshot.info.connection_id, snapshot.info.state,
        snapshot.path_count, snapshot.info.active_path_count,
        snapshot.info.multipath_active);
    result = NETINS_UDP_OK;

done:
    if (result != NETINS_UDP_OK && client != NULL) {
        char message[NETINS_UDP_ERROR_MSG_LEN] = {0};
        (void)netins_udp_get_last_error_msg(client, message,
            sizeof(message));
        fprintf(stderr, "QUIC/HTTP3 connect failed: code=%d message=%s\n",
            netins_udp_get_last_error_code(client),
            message[0] != '\0' ? message : "no error message");
    }
    netins_udp_close(client);
    if (netins_udp_cleanup() != NETINS_UDP_OK) {
        result = NETINS_UDP_ERROR;
    }
    return result;
}
