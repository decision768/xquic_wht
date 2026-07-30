#include "mini_backend.h"

#include <stdio.h>

int
main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    int exit_code = 1;
    mini_backend_config_t config;
    mini_backend_server_t *server = NULL;
    mini_backend_config_init(&config);

    int ret = mini_backend_server_create(&config, &server);
    if (ret != 0) {
        fprintf(stderr, "[error] create mini server backend failed: %d\n",
            ret);
        goto finish;
    }
    ret = mini_backend_server_listen(server, NULL, 0);
    if (ret != 0) {
        fprintf(stderr, "[error] listen mini server backend failed: %d\n",
            ret);
        goto finish;
    }
    ret = mini_backend_server_run(server);
    if (ret != 0) {
        fprintf(stderr, "[error] mini server event loop failed: %d\n", ret);
        goto finish;
    }
    exit_code = 0;

finish:
    mini_backend_server_release(server);
    return exit_code;
}
