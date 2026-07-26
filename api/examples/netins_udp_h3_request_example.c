#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <xquic/netins_udp.h>

/*
 * Issues one HTTP/3 GET on an already connected client and copies the
 * response body to output. File I/O remains on the caller thread.
 */
int
netins_udp_h3_get_example(netins_udp_t *client,
    const char *path, FILE *output)
{
    if (client == NULL || path == NULL || output == NULL) {
        errno = EINVAL;
        return -1;
    }

    netins_udp_h3_request_config_t config;
    netins_udp_h3_request_config_init(&config);
    size_t path_length = strlen(path);
    if (path_length == 0 || path[0] != '/'
        || path_length >= sizeof(config.path))
    {
        errno = EINVAL;
        return -1;
    }
    memcpy(config.path, path, path_length + 1);

    netins_udp_h3_request_t *request = NULL;
    if (netins_udp_h3_request_open(client, &config, &request)
        != NETINS_UDP_OK)
    {
        return -1;
    }

    unsigned char buffer[64 * 1024];
    int result = 0;
    int call_errno = 0;
    while (1) {
        ssize_t received = netins_udp_h3_request_recv(request,
            buffer, sizeof(buffer), 0);
        if (received < 0) {
            result = -1;
            call_errno = errno;
            break;
        }
        if (received == 0) {
            break;
        }
        if (fwrite(buffer, 1, (size_t)received, output)
            != (size_t)received)
        {
            errno = EIO;
            result = -1;
            call_errno = errno;
            break;
        }
    }

    if (result != 0) {
        char message[NETINS_UDP_ERROR_MSG_LEN] = {0};
        (void)netins_udp_h3_request_get_last_error_msg(request,
            message, sizeof(message));
        fprintf(stderr, "HTTP/3 GET failed: errno=%d, %s\n",
            call_errno,
            message[0] != '\0' ? message : "no error message");
    }
    netins_udp_h3_request_close(request);
    if (result != 0) {
        errno = call_errno;
    }
    return result;
}
