#include "mini_backend.h"

#include <stdio.h>

int
main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    int exit_code = 1;
    mini_backend_config_t config;
    mini_backend_client_t *client = NULL;
    mini_backend_config_init(&config);

    int ret = mini_backend_client_create(&config, &client);
    if (ret != 0) {
        fprintf(stderr, "[error] create mini client backend failed: %d\n",
            ret);
        goto finish;
    }
    ret = mini_backend_client_connect(client, NULL, 0, 0);
    if (ret != 0) {
        fprintf(stderr, "[error] connect mini client backend failed: %d\n",
            ret);
        goto finish;
    }
    ret = mini_backend_client_run(client);
    if (ret != 0) {
        fprintf(stderr, "[error] mini client event loop failed: %d\n", ret);
        goto finish;
    }
    exit_code = 0;

finish:
    mini_backend_client_destroy(client);
    return exit_code;
}
