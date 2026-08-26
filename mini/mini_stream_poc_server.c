#include "mini_stream_poc.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mini_server.h"

typedef struct xqc_mini_stream_poc_server_state_s {
    xqc_mini_svr_user_conn_t *user_conn;
    xqc_h3_ext_bytestream_t *bs;
    uint32_t seed;
    uint32_t status;
    uint64_t expected_bytes;
    uint64_t received_bytes;
    uint64_t digest;
    uint64_t first_bad_offset;
    uint64_t start_time_us;
    uint64_t finish_time_us;
    unsigned char receipt_buffer[XQC_MINI_STREAM_POC_RECEIPT_SIZE];
    int fin_received;
    int receipt_submitted;
    int completed;
} xqc_mini_stream_poc_server_state_t;

static void
xqc_mini_stream_poc_server_publish(
    xqc_mini_stream_poc_server_state_t *state)
{
    if (state == NULL || state->user_conn == NULL || state->completed) {
        return;
    }
    state->completed = 1;
    xqc_mini_svr_user_conn_t *listener = state->user_conn->listener_conn;
    if (listener != NULL) {
        listener->stream_poc_completed = 1;
        listener->stream_poc_result =
            state->status == XQC_MINI_STREAM_POC_STATUS_OK;
        listener->stream_poc_total_bytes = state->received_bytes;
        listener->stream_poc_digest = state->digest;
    }

    uint64_t elapsed_us = state->finish_time_us > state->start_time_us
        ? state->finish_time_us - state->start_time_us : 0;
    double throughput_mbps = elapsed_us > 0
        ? ((double)state->received_bytes * 8.0) / (double)elapsed_us
        : 0.0;
    printf("[stream_poc_result] role=server success=%d status=%u"
           " bytes=%" PRIu64 " digest=%016" PRIx64
           " first_bad_offset=%" PRIu64 " elapsed_us=%" PRIu64
           " throughput_mbps=%.3f\n",
        state->status == XQC_MINI_STREAM_POC_STATUS_OK, state->status,
        state->received_bytes, state->digest, state->first_bad_offset,
        elapsed_us, throughput_mbps);
}

static int
xqc_mini_stream_poc_server_submit_receipt(
    xqc_mini_stream_poc_server_state_t *state)
{
    if (state == NULL || state->bs == NULL || state->receipt_submitted
        || !state->fin_received)
    {
        return XQC_OK;
    }

    ssize_t sent = xqc_h3_ext_bytestream_send(state->bs,
        state->receipt_buffer, sizeof(state->receipt_buffer), 1,
        XQC_DATA_QOS_HIGHEST);
    if (sent == -XQC_EAGAIN) {
        return XQC_OK;
    }
    if (sent < 0 || (size_t)sent != sizeof(state->receipt_buffer)) {
        state->status = XQC_MINI_STREAM_POC_STATUS_INTERNAL_ERROR;
        state->finish_time_us = xqc_now();
        xqc_mini_stream_poc_server_publish(state);
        printf("[stream_poc_error] role=server reason=send receipt failed\n");
        return XQC_ERROR;
    }
    state->receipt_submitted = 1;
    xqc_mini_stream_poc_server_publish(state);
    return XQC_OK;
}

int
xqc_mini_stream_poc_server_bs_create_notify(
    xqc_h3_ext_bytestream_t *bs, void *bs_user_data)
{
    (void)bs_user_data;
    xqc_h3_conn_t *h3_conn = xqc_h3_ext_bytestream_get_h3_conn(bs);
    xqc_mini_svr_user_conn_t *user_conn =
        xqc_h3_conn_get_user_data(h3_conn);
    if (user_conn == NULL || user_conn->ctx == NULL
        || user_conn->ctx->args == NULL
        || !user_conn->ctx->args->stream_poc.enabled
        || user_conn->stream_poc_state != NULL)
    {
        printf("[stream_poc_error] role=server reason=unexpected bytestream\n");
        return XQC_ERROR;
    }

    xqc_mini_stream_poc_server_state_t *state =
        calloc(1, sizeof(*state));
    if (state == NULL) {
        return XQC_ERROR;
    }
    state->user_conn = user_conn;
    state->bs = bs;
    state->seed = user_conn->ctx->args->stream_poc.seed;
    state->expected_bytes =
        user_conn->ctx->args->stream_poc.total_bytes;
    state->digest = xqc_mini_stream_poc_digest_init();
    state->first_bad_offset = XQC_MINI_STREAM_POC_NO_BAD_OFFSET;
    state->start_time_us = xqc_now();
    user_conn->stream_poc_state = state;
    xqc_h3_ext_bytestream_set_user_data(bs, state);

    printf("[stream_poc_start] role=server lanes=1 expected_bytes=%" PRIu64
           " seed=%u stream_id=%" PRIu64 "\n",
        state->expected_bytes, state->seed,
        (uint64_t)xqc_h3_ext_bytestream_id(bs));
    return XQC_OK;
}

int
xqc_mini_stream_poc_server_bs_close_notify(
    xqc_h3_ext_bytestream_t *bs, void *bs_user_data)
{
    xqc_mini_stream_poc_server_state_t *state = bs_user_data;
    if (state != NULL) {
        xqc_h3_ext_bytestream_stats_t stats =
            xqc_h3_ext_bytestream_get_stats(bs);
        printf("[stream_poc_bs_closed] role=server sent=%zu received=%zu"
               " stream_error=%d\n",
            stats.bytes_sent, stats.bytes_rcvd, stats.stream_err);
        state->bs = NULL;
    }
    return XQC_OK;
}

int
xqc_mini_stream_poc_server_bs_read_notify(
    xqc_h3_ext_bytestream_t *bs, const void *data, size_t data_len,
    uint8_t fin, void *bs_user_data, uint64_t data_recv_time)
{
    (void)bs;
    (void)data_recv_time;
    xqc_mini_stream_poc_server_state_t *state = bs_user_data;
    if (state == NULL || state->fin_received) {
        return XQC_ERROR;
    }

    if (data_len > 0) {
        uint64_t bad_offset = XQC_MINI_STREAM_POC_NO_BAD_OFFSET;
        if (state->status == XQC_MINI_STREAM_POC_STATUS_OK
            && xqc_mini_stream_poc_verify(state->seed,
                state->received_bytes, data, data_len, &bad_offset) != 0)
        {
            state->status = XQC_MINI_STREAM_POC_STATUS_PATTERN_MISMATCH;
            state->first_bad_offset = bad_offset;
        }
        state->digest = xqc_mini_stream_poc_digest_update(state->digest,
            data, data_len);
        state->received_bytes += data_len;
        if (state->expected_bytes > 0
            && state->received_bytes > state->expected_bytes
            && state->status == XQC_MINI_STREAM_POC_STATUS_OK)
        {
            state->status = XQC_MINI_STREAM_POC_STATUS_LENGTH_MISMATCH;
            state->first_bad_offset = state->expected_bytes;
        }
    }

    if (!fin) {
        return XQC_OK;
    }
    state->fin_received = 1;
    state->finish_time_us = xqc_now();
    if (state->expected_bytes > 0
        && state->received_bytes != state->expected_bytes
        && state->status == XQC_MINI_STREAM_POC_STATUS_OK)
    {
        state->status = XQC_MINI_STREAM_POC_STATUS_LENGTH_MISMATCH;
        state->first_bad_offset = state->received_bytes;
    }

    xqc_mini_stream_poc_receipt_t receipt = {
        .status = state->status,
        .total_bytes = state->received_bytes,
        .digest = state->digest,
        .first_bad_offset = state->first_bad_offset,
    };
    xqc_mini_stream_poc_receipt_encode(&receipt, state->receipt_buffer);
    printf("[stream_poc_receive_finished] bytes=%" PRIu64
           " digest=%016" PRIx64 " status=%u\n",
        state->received_bytes, state->digest, state->status);
    return xqc_mini_stream_poc_server_submit_receipt(state);
}

int
xqc_mini_stream_poc_server_bs_write_notify(
    xqc_h3_ext_bytestream_t *bs, void *bs_user_data)
{
    (void)bs;
    return xqc_mini_stream_poc_server_submit_receipt(bs_user_data);
}

void
xqc_mini_stream_poc_server_on_connection_close(
    xqc_mini_svr_user_conn_t *user_conn)
{
    if (user_conn == NULL) {
        return;
    }
    xqc_mini_stream_poc_server_state_t *state =
        user_conn->stream_poc_state;
    if (state != NULL) {
        state->bs = NULL;
        if (!state->completed) {
            state->status = XQC_MINI_STREAM_POC_STATUS_PROTOCOL_ERROR;
            state->finish_time_us = xqc_now();
            xqc_mini_stream_poc_server_publish(state);
            printf("[stream_poc_error] role=server reason=connection closed"
                   " before stream completion\n");
        }
    }

    xqc_mini_svr_user_conn_t *listener = user_conn->listener_conn;
    if (listener != NULL && listener->ctx != NULL
        && listener->ctx->args != NULL
        && listener->ctx->args->stream_poc.once
        && listener->stream_poc_completed
        && listener->ctx->eb != NULL)
    {
        event_base_loopbreak(listener->ctx->eb);
    }
}

void
xqc_mini_stream_poc_server_release(xqc_mini_svr_user_conn_t *user_conn)
{
    if (user_conn == NULL || user_conn->stream_poc_state == NULL) {
        return;
    }
    xqc_mini_stream_poc_server_state_t *state =
        user_conn->stream_poc_state;
    user_conn->stream_poc_state = NULL;
    free(state);
}

int
xqc_mini_stream_poc_server_succeeded(
    const xqc_mini_svr_user_conn_t *listener_conn)
{
    return listener_conn != NULL && listener_conn->stream_poc_completed
        && listener_conn->stream_poc_result;
}
