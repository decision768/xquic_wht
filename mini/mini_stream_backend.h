#ifndef XQC_MINI_STREAM_BACKEND_H
#define XQC_MINI_STREAM_BACKEND_H

#include <event2/event.h>
#include <xquic/xqc_http3.h>

#include "mini_backend.h"

typedef enum mini_stream_endpoint_role_e {
    MINI_STREAM_ENDPOINT_OPEN = 1,
    MINI_STREAM_ENDPOINT_ACCEPT = 2
} mini_stream_endpoint_role_t;

typedef void (*mini_stream_owner_detach_fn)(void *owner,
    mini_backend_stream_t *stream);

int mini_stream_backend_create(xqc_engine_t *engine, const xqc_cid_t *cid,
    struct event_base *event_base, int role,
    const mini_backend_stream_config_t *config,
    void **connection_state_slot, void *owner,
    mini_stream_owner_detach_fn detach,
    mini_backend_stream_t **out_stream);

void mini_stream_backend_on_connection_close(
    mini_backend_stream_t *stream, int error_code);

/* Drive completion checks from a shared server event-loop thread. */
int mini_stream_backend_drive(mini_backend_stream_t *stream);

int mini_stream_backend_active_bs_create(
    xqc_h3_ext_bytestream_t *bs, void *bs_user_data);
int mini_stream_backend_passive_bs_create(mini_backend_stream_t *stream,
    xqc_h3_ext_bytestream_t *bs);
int mini_stream_backend_bs_close(xqc_h3_ext_bytestream_t *bs,
    void *bs_user_data);
int mini_stream_backend_bs_read(xqc_h3_ext_bytestream_t *bs,
    const void *data, size_t data_len, uint8_t fin, void *bs_user_data);
int mini_stream_backend_bs_write(xqc_h3_ext_bytestream_t *bs,
    void *bs_user_data);

int mini_stream_backend_client_bs_create_notify(
    xqc_h3_ext_bytestream_t *bs, void *bs_user_data);
int mini_stream_backend_client_bs_close_notify(
    xqc_h3_ext_bytestream_t *bs, void *bs_user_data);
int mini_stream_backend_client_bs_read_notify(
    xqc_h3_ext_bytestream_t *bs, const void *data, size_t data_len,
    uint8_t fin, void *bs_user_data, uint64_t data_recv_time);
int mini_stream_backend_client_bs_write_notify(
    xqc_h3_ext_bytestream_t *bs, void *bs_user_data);

int mini_stream_backend_server_bs_create_notify(
    xqc_h3_ext_bytestream_t *bs, void *bs_user_data);
int mini_stream_backend_server_bs_close_notify(
    xqc_h3_ext_bytestream_t *bs, void *bs_user_data);
int mini_stream_backend_server_bs_read_notify(
    xqc_h3_ext_bytestream_t *bs, const void *data, size_t data_len,
    uint8_t fin, void *bs_user_data, uint64_t data_recv_time);
int mini_stream_backend_server_bs_write_notify(
    xqc_h3_ext_bytestream_t *bs, void *bs_user_data);

#endif
