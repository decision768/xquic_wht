#include "mini_client.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void
xqc_mini_stream_poc_client_usage(const char *program)
{
    printf("Usage: %s [options]\n"
           "  --host ADDR          server IPv4 address\n"
           "  --port PORT          server UDP port\n"
           "  --bytes N[K|M|G]     generated stream length\n"
           "  --chunk N[K|M]       producer write size (max 16M)\n"
           "  --seed N             deterministic data seed\n"
           "  --timeout SEC        idle timeout\n"
           "  --single-path        disable QUIC multipath\n"
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
xqc_mini_stream_poc_client_parse(xqc_mini_cli_args_t *args,
    int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        const char *option = argv[i];
        if (strcmp(option, "--help") == 0) {
            return 1;
        }
        if (strcmp(option, "--single-path") == 0) {
            args->quic_cfg.multipath = 0;
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
        if (strcmp(option, "--host") == 0) {
            strncpy(args->net_cfg.server_addr, value,
                sizeof(args->net_cfg.server_addr) - 1);
        } else if (strcmp(option, "--port") == 0) {
            if (xqc_mini_stream_poc_parse_u64(value, &parsed) != 0
                || parsed == 0 || parsed > UINT16_MAX)
            {
                return -1;
            }
            args->net_cfg.server_port = (unsigned short)parsed;
        } else if (strcmp(option, "--bytes") == 0) {
            if (xqc_mini_stream_poc_parse_u64(value, &parsed) != 0) {
                return -1;
            }
            args->stream_poc.total_bytes = parsed;
        } else if (strcmp(option, "--chunk") == 0) {
            if (xqc_mini_stream_poc_parse_u64(value, &parsed) != 0
                || parsed == 0 || parsed > 16ULL * 1024ULL * 1024ULL
                || parsed > SIZE_MAX)
            {
                return -1;
            }
            args->stream_poc.chunk_size = (size_t)parsed;
        } else if (strcmp(option, "--seed") == 0) {
            if (xqc_mini_stream_poc_parse_u64(value, &parsed) != 0
                || parsed > UINT32_MAX)
            {
                return -1;
            }
            args->stream_poc.seed = (uint32_t)parsed;
        } else if (strcmp(option, "--timeout") == 0) {
            if (xqc_mini_stream_poc_parse_u64(value, &parsed) != 0
                || parsed == 0 || parsed > INT_MAX)
            {
                return -1;
            }
            args->net_cfg.conn_timeout = (int)parsed;
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
    xqc_mini_cli_ctx_t ctx = {0};
    xqc_mini_cli_args_t *args = calloc(1, sizeof(*args));
    xqc_mini_cli_user_conn_t *user_conn = NULL;
    if (args == NULL) {
        fprintf(stderr, "[error] allocate client arguments failed\n");
        return 1;
    }

    xqc_platform_init_env();
    xqc_mini_cli_init_args(args);
    (void)xqc_mini_cli_load_config_file(args, MINI_CONFIG_FILE);
    args->stream_poc.enabled = 1;
    args->disable_0rtt_storage = 1;
    args->env_cfg.disable_file_logging = 1;

    int parse_result = xqc_mini_stream_poc_client_parse(args, argc, argv);
    if (parse_result != 0) {
        if (parse_result > 0) {
            xqc_mini_stream_poc_client_usage(argv[0]);
            exit_code = 0;
        } else {
            xqc_mini_stream_poc_client_usage(argv[0]);
        }
        goto finish;
    }

    if (xqc_mini_cli_init_ctx(&ctx, args) != XQC_OK) {
        fprintf(stderr, "[error] initialize client context failed\n");
        ctx_initialized = 1;
        goto finish;
    }
    ctx_initialized = 1;
    if (xqc_mini_cli_init_xquic_engine(&ctx, args) != XQC_OK
        || xqc_mini_cli_init_engine_ctx(&ctx) != XQC_OK)
    {
        fprintf(stderr, "[error] initialize h3-ext client engine failed\n");
        goto finish;
    }

    user_conn = xqc_mini_cli_user_conn_create(&ctx);
    if (user_conn == NULL) {
        fprintf(stderr, "[error] create client connection scope failed\n");
        goto finish;
    }
    /* Handshake callback starts the POC instead of launching file requests. */
    user_conn->auto_start_requests = 1;
    if (xqc_mini_cli_main_process(user_conn, &ctx) != XQC_OK) {
        fprintf(stderr, "[error] start client connection failed\n");
        goto finish;
    }
    if (event_base_dispatch(ctx.eb) < 0) {
        fprintf(stderr, "[error] client event loop failed\n");
        goto finish;
    }
    exit_code = xqc_mini_stream_poc_client_succeeded(user_conn) ? 0 : 1;

finish:
    if (user_conn != NULL) {
        xqc_mini_cli_on_connection_finish(user_conn);
    }
    if (ctx.engine != NULL) {
        xqc_engine_destroy(ctx.engine);
        ctx.engine = NULL;
    }
    if (user_conn != NULL) {
        xqc_mini_cli_free_user_conn(user_conn);
    }
    if (ctx_initialized) {
        xqc_mini_cli_free_ctx(&ctx);
    } else {
        free(args);
    }
    return exit_code;
}
