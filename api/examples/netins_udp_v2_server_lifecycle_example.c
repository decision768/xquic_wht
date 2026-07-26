#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "api/draft/netins_udp_v2.h"

int
netins_udp_v2_server_lifecycle_example(const char *bind_host, int port,
    const char *certificate_file, const char *private_key_file)
{
    int result = NETINS_UDP_ERROR;
    netins_udp_t *listener = NULL;
    netins_udp_t *connection = NULL;

    if (certificate_file == NULL || private_key_file == NULL) {
        errno = EINVAL;
        return NETINS_UDP_ERROR;
    }
    if (netins_udp_init() != NETINS_UDP_OK) {
        return NETINS_UDP_ERROR;
    }

    listener = netins_udp_new(NETINS_UDP_TYPE_MPQUIC);
    if (listener == NULL) {
        goto done;
    }

    int protocol = NETINS_UDP_APP_HTTP3;
    if (netins_udp_set_opt(listener, NETINS_UDP_OPT_APP_PROTOCOL,
            &protocol, sizeof(protocol)) != NETINS_UDP_OK)
    {
        goto done;
    }

    netins_udp_tls_config_t tls = NETINS_UDP_TLS_CONFIG_INITIALIZER;
    snprintf(tls.certificate_file, sizeof(tls.certificate_file), "%s",
        certificate_file);
    snprintf(tls.private_key_file, sizeof(tls.private_key_file), "%s",
        private_key_file);
    if (netins_udp_set_opt(listener, NETINS_UDP_OPT_TLS,
            &tls, sizeof(tls)) != NETINS_UDP_OK
        || netins_udp_listen(listener, bind_host, port) == NULL)
    {
        goto done;
    }

    connection = netins_udp_accept(listener, NULL, NULL);
    if (connection == NULL) {
        goto done;
    }
    if (netins_udp_get_state(connection) != NETINS_UDP_STATE_CONNECTED) {
        errno = ENOTCONN;
        goto done;
    }

    /*
     * Stage 6 proves listener/accepted-connection ownership. Stage 7 adds
     * netins_udp_h3_request_accept() for actual server request bodies.
     */
    result = netins_udp_shutdown(connection, 0);

done:
    netins_udp_close(connection);
    netins_udp_close(listener);
    if (netins_udp_cleanup() != NETINS_UDP_OK) {
        result = NETINS_UDP_ERROR;
    }
    return result;
}
