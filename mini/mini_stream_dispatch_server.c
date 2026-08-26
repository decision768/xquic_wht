#include "mini_stream_backend.h"

#include "mini_server.h"
#include "mini_stream_poc.h"

static xqc_mini_svr_user_conn_t *
mini_stream_server_connection(xqc_h3_ext_bytestream_t *bs)
{
    xqc_h3_conn_t *h3_conn = xqc_h3_ext_bytestream_get_h3_conn(bs);
    return h3_conn != NULL ? xqc_h3_conn_get_user_data(h3_conn) : NULL;
}

static int
mini_stream_server_is_poc(xqc_h3_ext_bytestream_t *bs)
{
    xqc_mini_svr_user_conn_t *connection = mini_stream_server_connection(bs);
    return connection != NULL && connection->ctx != NULL
        && connection->ctx->args != NULL
        && connection->ctx->args->stream_poc.enabled;
}

int
mini_stream_backend_server_bs_create_notify(xqc_h3_ext_bytestream_t *bs,
    void *bs_user_data)
{
    if (mini_stream_server_is_poc(bs)) {
        return xqc_mini_stream_poc_server_bs_create_notify(bs, bs_user_data);
    }
    xqc_mini_svr_user_conn_t *connection = mini_stream_server_connection(bs);
    if (connection == NULL || connection->stream_api_state == NULL) {
        return XQC_ERROR;
    }
    return mini_stream_backend_passive_bs_create(
        connection->stream_api_state, bs);
}

int
mini_stream_backend_server_bs_close_notify(xqc_h3_ext_bytestream_t *bs,
    void *bs_user_data)
{
    return mini_stream_server_is_poc(bs)
        ? xqc_mini_stream_poc_server_bs_close_notify(bs, bs_user_data)
        : mini_stream_backend_bs_close(bs, bs_user_data);
}

int
mini_stream_backend_server_bs_read_notify(xqc_h3_ext_bytestream_t *bs,
    const void *data, size_t data_len, uint8_t fin, void *bs_user_data,
    uint64_t data_recv_time)
{
    return mini_stream_server_is_poc(bs)
        ? xqc_mini_stream_poc_server_bs_read_notify(bs, data, data_len, fin,
            bs_user_data, data_recv_time)
        : mini_stream_backend_bs_read(bs, data, data_len, fin, bs_user_data);
}

int
mini_stream_backend_server_bs_write_notify(xqc_h3_ext_bytestream_t *bs,
    void *bs_user_data)
{
    return mini_stream_server_is_poc(bs)
        ? xqc_mini_stream_poc_server_bs_write_notify(bs, bs_user_data)
        : mini_stream_backend_bs_write(bs, bs_user_data);
}
