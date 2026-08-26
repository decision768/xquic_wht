#include "mini_stream_poc.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mini_client.h"

typedef struct xqc_mini_stream_poc_client_state_s {
    xqc_mini_cli_user_conn_t *user_conn;
    xqc_h3_ext_bytestream_t *bs;
    unsigned char *send_buffer;
    size_t chunk_size;
    uint32_t seed;
    uint64_t total_bytes;
    uint64_t accepted_bytes;
    uint64_t digest;
    uint64_t start_time_us;
    uint64_t finish_time_us;
    unsigned char receipt_buffer[XQC_MINI_STREAM_POC_RECEIPT_SIZE];
    size_t receipt_bytes;
    int fin_submitted;
    int completed;
    int succeeded;
    int close_requested;
} xqc_mini_stream_poc_client_state_t;

static void
xqc_mini_stream_poc_client_close(
    xqc_mini_stream_poc_client_state_t *state)
{
    if (state == NULL || state->close_requested || state->user_conn == NULL
        || state->user_conn->ctx == NULL
        || state->user_conn->ctx->engine == NULL)
    {
        return;
    }
    state->close_requested = 1;
    (void)xqc_h3_conn_close(state->user_conn->ctx->engine,
        &state->user_conn->cid);
}

static int
xqc_mini_stream_poc_client_fail(
    xqc_mini_stream_poc_client_state_t *state, const char *message,
    int close_connection)
{
    if (state != NULL) {
        state->completed = 1;
        state->succeeded = 0;
    }
    printf("[stream_poc_error] role=client reason=%s\n",
        message != NULL ? message : "unknown");
    if (close_connection) {
        xqc_mini_stream_poc_client_close(state);
    }
    return -1;
}

static int
xqc_mini_stream_poc_client_pump(
    xqc_mini_stream_poc_client_state_t *state)
{
    if (state == NULL || state->bs == NULL || state->completed
        || state->fin_submitted)
    {
        return XQC_OK;
    }

    while (state->accepted_bytes < state->total_bytes) {
        uint64_t remaining = state->total_bytes - state->accepted_bytes;
        size_t data_len = remaining < state->chunk_size
            ? (size_t)remaining : state->chunk_size;
        xqc_mini_stream_poc_fill(state->seed, state->accepted_bytes,
            state->send_buffer, data_len);

        int final_chunk = data_len == remaining;
        ssize_t sent = xqc_h3_ext_bytestream_send(state->bs,
            state->send_buffer, data_len, final_chunk,
            XQC_DATA_QOS_HIGHEST);
        if (sent == -XQC_EAGAIN) {
            return XQC_OK;
        }
        if (sent < 0 || (size_t)sent != data_len) {
            return xqc_mini_stream_poc_client_fail(state,
                "bytestream send failed", 1);
        }

        state->digest = xqc_mini_stream_poc_digest_update(state->digest,
            state->send_buffer, data_len);
        state->accepted_bytes += data_len;
        if (final_chunk) {
            state->fin_submitted = 1;
            break;
        }
    }

    if (state->total_bytes == 0 && !state->fin_submitted) {
        ssize_t sent = xqc_h3_ext_bytestream_finish(state->bs);
        if (sent == -XQC_EAGAIN) {
            return XQC_OK;
        }
        if (sent < 0) {
            return xqc_mini_stream_poc_client_fail(state,
                "bytestream finish failed", 1);
        }
        state->fin_submitted = 1;
    }

    if (state->fin_submitted) {
        printf("[stream_poc_send_finished] bytes=%" PRIu64
               " digest=%016" PRIx64 "\n",
            state->accepted_bytes, state->digest);
    }
    return XQC_OK;
}

int
xqc_mini_stream_poc_client_start(xqc_mini_cli_user_conn_t *user_conn)
{
    if (user_conn == NULL || user_conn->ctx == NULL
        || user_conn->ctx->args == NULL
        || !user_conn->ctx->args->stream_poc.enabled
        || user_conn->stream_poc_state != NULL)
    {
        return XQC_ERROR;
    }

    xqc_mini_stream_poc_config_t *config =
        &user_conn->ctx->args->stream_poc;
    if (config->chunk_size == 0) {
        return XQC_ERROR;
    }

    xqc_mini_stream_poc_client_state_t *state =
        calloc(1, sizeof(*state));
    if (state == NULL) {
        return XQC_ERROR;
    }
    state->send_buffer = malloc(config->chunk_size);
    if (state->send_buffer == NULL) {
        free(state);
        return XQC_ERROR;
    }
    state->user_conn = user_conn;
    state->chunk_size = config->chunk_size;
    state->seed = config->seed;
    state->total_bytes = config->total_bytes;
    state->digest = xqc_mini_stream_poc_digest_init();
    state->start_time_us = xqc_now();
    user_conn->stream_poc_state = state;

    state->bs = xqc_h3_ext_bytestream_create(user_conn->ctx->engine,
        &user_conn->cid, state);
    if (state->bs == NULL) {
        xqc_mini_stream_poc_client_fail(state,
            "create H3 extended bytestream failed", 1);
        return XQC_ERROR;
    }

    printf("[stream_poc_start] role=client lanes=1 bytes=%" PRIu64
           " chunk=%zu seed=%u stream_id=%" PRIu64 "\n",
        state->total_bytes, state->chunk_size, state->seed,
        (uint64_t)xqc_h3_ext_bytestream_id(state->bs));
    return xqc_mini_stream_poc_client_pump(state);
}

int
xqc_mini_stream_poc_client_bs_create_notify(
    xqc_h3_ext_bytestream_t *bs, void *bs_user_data)
{
    xqc_mini_stream_poc_client_state_t *state = bs_user_data;
    if (state == NULL) {
        return XQC_ERROR;
    }
    state->bs = bs;
    return XQC_OK;
}

int
xqc_mini_stream_poc_client_bs_close_notify(
    xqc_h3_ext_bytestream_t *bs, void *bs_user_data)
{
    xqc_mini_stream_poc_client_state_t *state = bs_user_data;
    if (state != NULL) {
        xqc_h3_ext_bytestream_stats_t stats =
            xqc_h3_ext_bytestream_get_stats(bs);
        printf("[stream_poc_bs_closed] role=client sent=%zu received=%zu"
               " stream_error=%d\n",
            stats.bytes_sent, stats.bytes_rcvd, stats.stream_err);
        state->bs = NULL;
    }
    return XQC_OK;
}

int
xqc_mini_stream_poc_client_bs_read_notify(
    xqc_h3_ext_bytestream_t *bs, const void *data, size_t data_len,
    uint8_t fin, void *bs_user_data, uint64_t data_recv_time)
{
    (void)bs;
    (void)data_recv_time;
    xqc_mini_stream_poc_client_state_t *state = bs_user_data;
    if (state == NULL || state->completed) {
        return XQC_OK;
    }
    if (data_len > XQC_MINI_STREAM_POC_RECEIPT_SIZE - state->receipt_bytes) {
        return xqc_mini_stream_poc_client_fail(state,
            "receipt is larger than the fixed protocol record", 0);
    }
    if (data_len > 0) {
        memcpy(state->receipt_buffer + state->receipt_bytes, data, data_len);
        state->receipt_bytes += data_len;
    }
    if (!fin) {
        return XQC_OK;
    }
    if (state->receipt_bytes != XQC_MINI_STREAM_POC_RECEIPT_SIZE) {
        return xqc_mini_stream_poc_client_fail(state,
            "receipt FIN arrived before a complete record", 0);
    }

    xqc_mini_stream_poc_receipt_t receipt;
    if (xqc_mini_stream_poc_receipt_decode(state->receipt_buffer,
            &receipt) != 0)
    {
        return xqc_mini_stream_poc_client_fail(state,
            "invalid receipt magic or version", 0);
    }

    state->finish_time_us = xqc_now();
    state->completed = 1;
    state->succeeded = receipt.status == XQC_MINI_STREAM_POC_STATUS_OK
        && receipt.total_bytes == state->accepted_bytes
        && receipt.digest == state->digest;
    uint64_t elapsed_us = state->finish_time_us - state->start_time_us;
    double throughput_mbps = elapsed_us > 0
        ? ((double)state->accepted_bytes * 8.0) / (double)elapsed_us
        : 0.0;
    printf("[stream_poc_receipt] status=%u bytes=%" PRIu64
           " digest=%016" PRIx64 " first_bad_offset=%" PRIu64 "\n",
        receipt.status, receipt.total_bytes, receipt.digest,
        receipt.first_bad_offset);
    printf("[stream_poc_result] role=client success=%d elapsed_us=%" PRIu64
           " throughput_mbps=%.3f\n",
        state->succeeded, elapsed_us, throughput_mbps);
    xqc_mini_stream_poc_client_close(state);
    return XQC_OK;
}

int
xqc_mini_stream_poc_client_bs_write_notify(
    xqc_h3_ext_bytestream_t *bs, void *bs_user_data)
{
    (void)bs;
    return xqc_mini_stream_poc_client_pump(bs_user_data);
}

void
xqc_mini_stream_poc_client_on_connection_close(
    xqc_mini_cli_user_conn_t *user_conn)
{
    if (user_conn == NULL || user_conn->stream_poc_state == NULL) {
        return;
    }
    xqc_mini_stream_poc_client_state_t *state =
        user_conn->stream_poc_state;
    state->bs = NULL;
    if (!state->completed) {
        state->completed = 1;
        state->succeeded = 0;
        printf("[stream_poc_error] role=client reason=connection closed"
               " before a valid receipt\n");
    }
}

void
xqc_mini_stream_poc_client_release(xqc_mini_cli_user_conn_t *user_conn)
{
    if (user_conn == NULL || user_conn->stream_poc_state == NULL) {
        return;
    }
    xqc_mini_stream_poc_client_state_t *state =
        user_conn->stream_poc_state;
    user_conn->stream_poc_state = NULL;
    free(state->send_buffer);
    free(state);
}

int
xqc_mini_stream_poc_client_succeeded(
    const xqc_mini_cli_user_conn_t *user_conn)
{
    if (user_conn == NULL || user_conn->stream_poc_state == NULL) {
        return 0;
    }
    const xqc_mini_stream_poc_client_state_t *state =
        user_conn->stream_poc_state;
    return state->completed && state->succeeded;
}
