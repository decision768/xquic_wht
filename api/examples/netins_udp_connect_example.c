#include <inttypes.h>
#include <stdio.h>
#include <unistd.h>

#include <xquic/netins_udp.h>

/*
 * Minimal independent QUIC/H3 connection example. This code does not start or
 * reuse mini_client. Open netins_udp_stream_t objects after this connection
 * step when the application needs HTTP/3 GET/POST data transfer.
 */
int
netins_udp_connect_example(const char *server_address, uint16_t server_port,
    const char *server_name, const char *first_interface,
    const char *second_interface)
{
    if (server_address == NULL || server_address[0] == '\0'
        || server_port == 0)
    {
        return NETINS_UDP_ERROR;
    }
    netins_udp_client_config_t config;
    netins_udp_client_config_init(&config);
    snprintf(config.server_address, sizeof(config.server_address), "%s",
        server_address);
    config.server_port = server_port;
    if (server_name != NULL) {
        snprintf(config.server_name, sizeof(config.server_name), "%s",
            server_name);
    }
    if (first_interface != NULL && first_interface[0] != '\0') {
        snprintf(config.interface_names[0],
            sizeof(config.interface_names[0]), "%s", first_interface);
        config.interface_count = 1;
    }
    if (second_interface != NULL && second_interface[0] != '\0') {
        snprintf(config.interface_names[1],
            sizeof(config.interface_names[1]), "%s", second_interface);
        config.interface_count = 2;
        config.enable_multipath = 1;
    }

    netins_udp_client_t *client = NULL;
    int ret = netins_udp_client_create(&config, &client);
    if (ret != NETINS_UDP_OK) {
        fprintf(stderr, "netins_udp_client_create failed: %d\n", ret);
        return ret;
    }

    ret = netins_udp_client_connect(client);
    if (ret != NETINS_UDP_OK) {
        char message[NETINS_UDP_ERROR_MSG_LEN];
        (void)netins_udp_client_get_last_error_msg(client, message,
            sizeof(message));
        fprintf(stderr, "QUIC/H3 connect failed: code=%d message=%s\n",
            netins_udp_client_get_last_error_code(client), message);
        netins_udp_client_close(client);
        return ret;
    }

    netins_udp_snapshot_t snapshot = NETINS_UDP_SNAPSHOT_INITIALIZER;
    for (int attempt = 0; attempt < 50; ++attempt) {
        ret = netins_udp_client_get_snapshot(client, &snapshot);
        if (ret != NETINS_UDP_OK || !config.enable_multipath
            || snapshot.info.active_path_count >= config.interface_count)
        {
            break;
        }
        usleep(100000);
    }
    if (ret == NETINS_UDP_OK) {
        printf("connected id=%" PRIu64 " state=%s paths=%u active=%u\n",
            snapshot.info.connection_id,
            netins_udp_conn_state_name(snapshot.info.connection_state),
            snapshot.path_count,
            snapshot.info.active_path_count);
    }

    netins_udp_client_close(client);
    return ret;
}
