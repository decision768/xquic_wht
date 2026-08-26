#ifndef XQC_MINI_STREAM_POC_H
#define XQC_MINI_STREAM_POC_H

#include <stddef.h>
#include <stdint.h>

#include <xquic/xqc_http3.h>

#include "mini_stream_poc_protocol.h"

#define XQC_MINI_STREAM_POC_DEFAULT_BYTES (64ULL * 1024ULL * 1024ULL)
#define XQC_MINI_STREAM_POC_DEFAULT_CHUNK (256U * 1024U)
#define XQC_MINI_STREAM_POC_DEFAULT_SEED 1U

typedef struct xqc_mini_stream_poc_config_s {
    int enabled;
    int once;
    uint32_t seed;
    uint32_t reserved32;
    uint64_t total_bytes;
    size_t chunk_size;
} xqc_mini_stream_poc_config_t;

struct xqc_mini_cli_user_conn_s;
struct xqc_mini_svr_user_conn_s;

int xqc_mini_stream_poc_client_start(
    struct xqc_mini_cli_user_conn_s *user_conn);
void xqc_mini_stream_poc_client_on_connection_close(
    struct xqc_mini_cli_user_conn_s *user_conn);
void xqc_mini_stream_poc_client_release(
    struct xqc_mini_cli_user_conn_s *user_conn);
int xqc_mini_stream_poc_client_succeeded(
    const struct xqc_mini_cli_user_conn_s *user_conn);

int xqc_mini_stream_poc_client_bs_create_notify(
    xqc_h3_ext_bytestream_t *bs, void *bs_user_data);
int xqc_mini_stream_poc_client_bs_close_notify(
    xqc_h3_ext_bytestream_t *bs, void *bs_user_data);
int xqc_mini_stream_poc_client_bs_read_notify(
    xqc_h3_ext_bytestream_t *bs, const void *data, size_t data_len,
    uint8_t fin, void *bs_user_data, uint64_t data_recv_time);
int xqc_mini_stream_poc_client_bs_write_notify(
    xqc_h3_ext_bytestream_t *bs, void *bs_user_data);

void xqc_mini_stream_poc_server_on_connection_close(
    struct xqc_mini_svr_user_conn_s *user_conn);
void xqc_mini_stream_poc_server_release(
    struct xqc_mini_svr_user_conn_s *user_conn);
int xqc_mini_stream_poc_server_succeeded(
    const struct xqc_mini_svr_user_conn_s *listener_conn);

int xqc_mini_stream_poc_server_bs_create_notify(
    xqc_h3_ext_bytestream_t *bs, void *bs_user_data);
int xqc_mini_stream_poc_server_bs_close_notify(
    xqc_h3_ext_bytestream_t *bs, void *bs_user_data);
int xqc_mini_stream_poc_server_bs_read_notify(
    xqc_h3_ext_bytestream_t *bs, const void *data, size_t data_len,
    uint8_t fin, void *bs_user_data, uint64_t data_recv_time);
int xqc_mini_stream_poc_server_bs_write_notify(
    xqc_h3_ext_bytestream_t *bs, void *bs_user_data);

#endif
