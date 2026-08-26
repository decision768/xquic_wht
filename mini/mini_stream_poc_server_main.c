#include "mini_server.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void
xqc_mini_stream_poc_server_usage(const char *program)
{
    printf("Usage: %s [options]\n"
           "  --listen ADDR        listen IPv4 address\n"
           "  --port PORT          listen UDP port\n"
           "  --expect-bytes N[K|M|G] require final length (0 accepts unknown)\n"
           "  --seed N             deterministic data seed\n"
           "  --keep-running       accept another connection after completion\n"
           "  --no-zlog            use stdout/file logging only\n"
           "  --help               show this help\n",
        program);
}

static int
xqc_mini_stream_poc_parse_u64(const char *text, uint64_t *value)
{
    if (text == NULL || value == NULL || text[0] == '\0') {
        return -1;
    }
    errno = 0;
    char *end = NULL;
    unsigned long long parsed = strtoull(text, &end, 10);
    if (errno != 0 || end == text) {
        return -1;
    }
    uint64_t multiplier = 1;
    if (*end != '\0') {
        if (end[1] != '\0') {
            return -1;
        }
        switch (*end) {
        case 'k': case 'K': multiplier = 1024ULL; break;
        case 'm': case 'M': multiplier = 1024ULL * 1024ULL; break;
        case 'g': case 'G': multiplier = 1024ULL * 1024ULL * 1024ULL; break;
        default: return -1;
        }
    }
    if ((uint64_t)parsed > UINT64_MAX / multiplier) {
        return -1;
    }
    *value = (uint64_t)parsed * multiplier;
    return 0;
}

static int
xqc_mini_stream_poc_server_parse(xqc_mini_svr_args_t *args,
    int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        const char *option = argv[i];
        if (strcmp(option, "--help") == 0) {
            return 1;
        }
        if (strcmp(option, "--keep-running") == 0) {
            args->stream_poc.once = 0;
            continue;
        }
        if (strcmp(option, "--no-zlog") == 0) {
            args->env_cfg.use_zlog = 0;
            continue;
        }
        if (i + 1 >= argc) {
            fprintf(stderr, "[error] missing value for %s\n", option);
            return -1;
        }
        const char *value = argv[++i];
        uint64_t parsed = 0;
        if (strcmp(option, "--listen") == 0) {
            strncpy(args->net_cfg.ip, value, sizeof(args->net_cfg.ip) - 1);
        } else if (strcmp(option, "--port") == 0) {
            if (xqc_mini_stream_poc_parse_u64(value, &parsed) != 0
                || parsed == 0 || parsed > UINT16_MAX)
            {
                return -1;
            }
            args->net_cfg.port = (unsigned short)parsed;
        } else if (strcmp(option, "--expect-bytes") == 0) {
            if (xqc_mini_stream_poc_parse_u64(value, &parsed) != 0) {
                return -1;
            }
            args->stream_poc.total_bytes = parsed;
        } else if (strcmp(option, "--seed") == 0) {
            if (xqc_mini_stream_poc_parse_u64(value, &parsed) != 0
                || parsed > UINT32_MAX)
            {
                return -1;
            }
            args->stream_poc.seed = (uint32_t)parsed;
        } else {
            fprintf(stderr, "[error] unknown option: %s\n", option);
            return -1;
        }
    }
    return 0;
}

int
main(int argc, char **argv)
{
    int exit_code = 1;
    int ctx_initialized = 0;
    xqc_mini_svr_ctx_t ctx = {0};
    xqc_mini_svr_args_t *args = calloc(1, sizeof(*args));
    xqc_mini_svr_user_conn_t *listener = NULL;
    if (args == NULL) {
        fprintf(stderr, "[error] allocate server arguments failed\n");
        return 1;
    }

    xqc_platform_init_env();
    xqc_mini_svr_init_args(args);
    (void)xqc_mini_svr_load_config_file(args, MINI_SERVER_CONFIG_FILE);
    args->stream_poc.enabled = 1;
    args->env_cfg.disable_file_logging = 1;

    int parse_result = xqc_mini_stream_poc_server_parse(args, argc, argv);
    if (parse_result != 0) {
        if (parse_result > 0) {
            xqc_mini_stream_poc_server_usage(argv[0]);
            exit_code = 0;
        } else {
            xqc_mini_stream_poc_server_usage(argv[0]);
        }
        goto finish;
    }

    if (xqc_mini_svr_init_ctx(&ctx, args) != XQC_OK) {
        fprintf(stderr, "[error] initialize server context failed\n");
        ctx_initialized = 1;
        goto finish;
    }
    ctx_initialized = 1;
    if (xqc_mini_svr_init_xquic_engine(&ctx, args) != XQC_OK
        || xqc_mini_svr_init_engine_ctx(&ctx, args) != XQC_OK)
    {
        fprintf(stderr, "[error] initialize h3-ext server engine failed\n");
        goto finish;
    }
    listener = xqc_mini_svr_create_user_conn(&ctx);
    if (listener == NULL) {
        fprintf(stderr, "[error] create server listener failed\n");
        goto finish;
    }

    printf("[stream_poc_listen] address=%s port=%u once=%d seed=%u"
           " expected_bytes=%" PRIu64 "\n",
        args->net_cfg.ip, args->net_cfg.port, args->stream_poc.once,
        args->stream_poc.seed, args->stream_poc.total_bytes);
    if (event_base_dispatch(ctx.eb) < 0) {
        fprintf(stderr, "[error] server event loop failed\n");
        goto finish;
    }
    exit_code = args->stream_poc.once
        ? (xqc_mini_stream_poc_server_succeeded(listener) ? 0 : 1)
        : 0;

finish:
    if (listener != NULL) {
        xqc_mini_svr_on_connection_finish(listener);
    }
    if (ctx.engine != NULL) {
        xqc_engine_destroy(ctx.engine);
        ctx.engine = NULL;
    }
    if (listener != NULL) {
        xqc_mini_svr_free_user_conn(listener);
    }
    if (ctx_initialized) {
        xqc_mini_svr_free_ctx(&ctx);
    } else {
        free(args);
    }
    return exit_code;
}
