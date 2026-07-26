#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <xquic/netins_udp.h>

int
netins_udp_server_lifecycle_example(const char *bind_host, int port,
    const char *certificate_file, const char *private_key_file)
{
    int result = NETINS_UDP_ERROR;
    netins_udp_t *listener = NULL;
    netins_udp_t *connection = NULL;
    netins_udp_h3_request_t *request = NULL;

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

    if (netins_udp_h3_request_accept(connection, &request)
        != NETINS_UDP_OK)
    {
        goto done;
    }

    unsigned char request_body[4096];
    for (;;) {
        ssize_t received = netins_udp_h3_request_recv(request,
            request_body, sizeof(request_body), 0);
        if (received < 0) {
            goto done;
        }
        if (received == 0) {
            break;
        }
        /* Consume or persist this request-body chunk here. */
    }

    static const char response_body[] = "OK";
    netins_udp_h3_response_config_t response;
    netins_udp_h3_response_config_init(&response);
    response.status = 200;
    response.content_length = sizeof(response_body) - 1;
    snprintf(response.content_type, sizeof(response.content_type),
        "text/plain");
    if (netins_udp_h3_request_set_response(request, &response)
        != NETINS_UDP_OK)
    {
        goto done;
    }

    size_t offset = 0;
    while (offset < sizeof(response_body) - 1) {
        ssize_t sent = netins_udp_h3_request_send(request,
            response_body + offset, sizeof(response_body) - 1 - offset,
            NETINS_UDP_MSG_FIN);
        if (sent <= 0) {
            if (sent == 0) {
                errno = EIO;
            }
            goto done;
        }
        offset += (size_t)sent;
    }
    netins_udp_h3_request_close(request);
    request = NULL;
    result = netins_udp_shutdown(connection, 0);

done:
    netins_udp_h3_request_close(request);
    netins_udp_close(connection);
    netins_udp_close(listener);
    if (netins_udp_cleanup() != NETINS_UDP_OK) {
        result = NETINS_UDP_ERROR;
    }
    return result;
}
