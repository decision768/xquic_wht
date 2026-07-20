#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

int netins_udp_connect_example(const char *server_address,
    uint16_t server_port, const char *server_name,
    const char *first_interface, const char *second_interface);

int
main(int argc, char **argv)
{
    if (argc < 4 || argc > 6) {
        fprintf(stderr,
            "usage: %s <server-address> <port> <server-name> [if0] [if1]\n",
            argv[0]);
        return 2;
    }

    errno = 0;
    char *end = NULL;
    unsigned long port = strtoul(argv[2], &end, 10);
    if (errno != 0 || end == argv[2] || *end != '\0'
        || port == 0 || port > UINT16_MAX)
    {
        fprintf(stderr, "invalid UDP port: %s\n", argv[2]);
        return 2;
    }

    const char *first_interface = argc >= 5 ? argv[4] : NULL;
    const char *second_interface = argc >= 6 ? argv[5] : NULL;
    int ret = netins_udp_connect_example(argv[1], (uint16_t)port, argv[3],
        first_interface, second_interface);
    return ret == 0 ? 0 : 1;
}
