#ifndef XQC_MINI_STREAM_POC_PROTOCOL_H
#define XQC_MINI_STREAM_POC_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#define XQC_MINI_STREAM_POC_RECEIPT_SIZE 40U
#define XQC_MINI_STREAM_POC_RECEIPT_VERSION 1U
#define XQC_MINI_STREAM_POC_NO_BAD_OFFSET UINT64_MAX

typedef enum xqc_mini_stream_poc_status_e {
    XQC_MINI_STREAM_POC_STATUS_OK = 0,
    XQC_MINI_STREAM_POC_STATUS_PATTERN_MISMATCH = 1,
    XQC_MINI_STREAM_POC_STATUS_LENGTH_MISMATCH = 2,
    XQC_MINI_STREAM_POC_STATUS_PROTOCOL_ERROR = 3,
    XQC_MINI_STREAM_POC_STATUS_INTERNAL_ERROR = 4,
} xqc_mini_stream_poc_status_t;

typedef struct xqc_mini_stream_poc_receipt_s {
    uint32_t status;
    uint64_t total_bytes;
    uint64_t digest;
    uint64_t first_bad_offset;
} xqc_mini_stream_poc_receipt_t;

void xqc_mini_stream_poc_fill(uint32_t seed, uint64_t offset,
    unsigned char *data, size_t data_len);
int xqc_mini_stream_poc_verify(uint32_t seed, uint64_t offset,
    const unsigned char *data, size_t data_len, uint64_t *bad_offset);
uint64_t xqc_mini_stream_poc_digest_init(void);
uint64_t xqc_mini_stream_poc_digest_update(uint64_t digest,
    const unsigned char *data, size_t data_len);
void xqc_mini_stream_poc_receipt_encode(
    const xqc_mini_stream_poc_receipt_t *receipt,
    unsigned char out[XQC_MINI_STREAM_POC_RECEIPT_SIZE]);
int xqc_mini_stream_poc_receipt_decode(
    const unsigned char data[XQC_MINI_STREAM_POC_RECEIPT_SIZE],
    xqc_mini_stream_poc_receipt_t *receipt);

#endif
