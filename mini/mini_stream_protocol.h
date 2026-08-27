#ifndef XQC_MINI_STREAM_PROTOCOL_H
#define XQC_MINI_STREAM_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#define MINI_STREAM_PROTOCOL_VERSION 2U
#define MINI_STREAM_LANE_HEADER_SIZE 40U
#define MINI_STREAM_FRAME_HEADER_SIZE 24U
#define MINI_STREAM_RECEIPT_SIZE 48U
#define MINI_STREAM_MAX_FRAME_PAYLOAD (64U * 1024U)

typedef enum mini_stream_frame_type_e {
    MINI_STREAM_FRAME_DATA = 1,
    MINI_STREAM_FRAME_FIN = 2
} mini_stream_frame_type_t;

typedef enum mini_stream_receipt_status_e {
    MINI_STREAM_RECEIPT_OK = 0,
    MINI_STREAM_RECEIPT_PROTOCOL_ERROR = 1,
    MINI_STREAM_RECEIPT_LENGTH_ERROR = 2,
    MINI_STREAM_RECEIPT_DIGEST_ERROR = 3,
    MINI_STREAM_RECEIPT_RESOURCE_ERROR = 4,
    /* A cumulative sender credit carried on lane 0 before the final receipt. */
    MINI_STREAM_RECEIPT_CREDIT = 5
} mini_stream_receipt_status_t;

typedef struct mini_stream_lane_header_s {
    uint64_t session_id;
    uint32_t lane_index;
    uint32_t lane_count;
    uint32_t max_frame_payload;
} mini_stream_lane_header_t;

typedef struct mini_stream_frame_header_s {
    uint32_t type;
    uint64_t offset;
    uint32_t payload_length;
    uint32_t flags;
} mini_stream_frame_header_t;

typedef struct mini_stream_receipt_s {
    uint32_t status;
    uint64_t session_id;
    uint64_t total_bytes;
    uint64_t digest;
    uint64_t detail;
} mini_stream_receipt_t;

void mini_stream_lane_header_encode(const mini_stream_lane_header_t *header,
    unsigned char out[MINI_STREAM_LANE_HEADER_SIZE]);
int mini_stream_lane_header_decode(
    const unsigned char data[MINI_STREAM_LANE_HEADER_SIZE],
    mini_stream_lane_header_t *header);

void mini_stream_frame_header_encode(
    const mini_stream_frame_header_t *header,
    unsigned char out[MINI_STREAM_FRAME_HEADER_SIZE]);
int mini_stream_frame_header_decode(
    const unsigned char data[MINI_STREAM_FRAME_HEADER_SIZE],
    mini_stream_frame_header_t *header);

void mini_stream_receipt_encode(const mini_stream_receipt_t *receipt,
    unsigned char out[MINI_STREAM_RECEIPT_SIZE]);
int mini_stream_receipt_decode(
    const unsigned char data[MINI_STREAM_RECEIPT_SIZE],
    mini_stream_receipt_t *receipt);

uint64_t mini_stream_digest_init(void);
uint64_t mini_stream_digest_update(uint64_t digest,
    const void *data, size_t data_len);

#endif
