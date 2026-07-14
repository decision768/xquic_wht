#include "mini_server.h"
#include <stdint.h>

int init_args(xqc_mini_svr_args_t** args, xqc_mini_svr_ctx_t *ctx, int argc, char *argv[]){
    int ret;
    *args = calloc(1, sizeof(xqc_mini_svr_args_t));
    if (args == NULL) {
        printf("[error] calloc args failed\n");
        return XQC_ERROR;
    }

    /* init server environment */
    ret = xqc_mini_svr_init_env(ctx, *args);
    if (ret < 0) {
        printf("[error] init server environment failed\n");
        return XQC_ERROR;
    }

    // ret = xqc_mini_svr_parse_cmd_args(*args, argc, argv);
    // if (ret != XQC_OK) {
    //     return XQC_ERROR;
    // }
    /* create & init engine to ctx->engine */
    ret = xqc_mini_svr_init_xquic_engine(ctx, *args);
    if (ret < 0) {
        printf("[error] init xquic engine failed\n");
        return XQC_ERROR;
    }

    /* init engine ctx */
    ret = xqc_mini_svr_init_engine_ctx(ctx, *args);
    if (ret < 0) {
        printf("[error] init engine ctx failed\n");
        return XQC_ERROR;
    }
    return XQC_OK;
}
xqc_mini_svr_user_conn_t* connection_socket(xqc_mini_svr_ctx_t* ctx){
    return xqc_mini_svr_create_user_conn(ctx);
}

void start_event(xqc_mini_svr_ctx_t* ctx){
    event_base_dispatch(ctx->eb);
}

void close_all(xqc_mini_svr_ctx_t* ctx, xqc_mini_svr_user_conn_t* user_conn){
    xqc_engine_destroy(ctx->engine);
    xqc_mini_svr_free_ctx(ctx);
    if (user_conn) {
        xqc_mini_svr_free_user_conn(user_conn);
    }
}
int main(int argc, char *argv[]){
    xqc_mini_svr_ctx_t svr_ctx = {0}, *ctx = &svr_ctx;
    xqc_mini_svr_args_t *args = NULL;

    if(init_args(&args, ctx, argc, argv) ==XQC_ERROR){
        goto exit;
    }
    xqc_mini_svr_user_conn_t *user_conn = NULL;
    user_conn = connection_socket(ctx);
    if (user_conn == NULL) {
        goto exit;
    }
    start_event(ctx);
exit:

    close_all(ctx,user_conn);
    return 0;
}