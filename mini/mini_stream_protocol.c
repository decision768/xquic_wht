#include "mini_stream_protocol.h"

#include <string.h>

#define MINI_STREAM_FRAME_MAGIC 0x4e534652U
#define MINI_STREAM_FNV_OFFSET 14695981039346656037ULL
#define MINI_STREAM_FNV_PRIME 1099511628211ULL

static const unsigned char mini_stream_lane_magic[8] = {
    'N', 'S', 'L', 'A', 'N', 'E', '0', '1'
};
static const unsigned char mini_stream_receipt_magic[8] = {
    'N', 'S', 'S', 'T', 'R', 'A', 'C', 'K'
};

static void
mini_stream_write_u32(unsigned char *out, uint32_t value)
{
    out[0] = (unsigned char)(value >> 24);
    out[1] = (unsigned char)(value >> 16);
    out[2] = (unsigned char)(value >> 8);
    out[3] = (unsigned char)value;
}

static void
mini_stream_write_u64(unsigned char *out, uint64_t value)
{
    size_t i;
    for (i = 0; i < 8; i++) {
        out[i] = (unsigned char)(value >> (56 - i * 8));
    }
}

static uint32_t
mini_stream_read_u32(const unsigned char *data)
{
    return ((uint32_t)data[0] << 24)
        | ((uint32_t)data[1] << 16)
        | ((uint32_t)data[2] << 8)
        | (uint32_t)data[3];
}

static uint64_t
mini_stream_read_u64(const unsigned char *data)
{
    uint64_t value = 0;
    size_t i;
    for (i = 0; i < 8; i++) {
        value = (value << 8) | data[i];
    }
    return value;
}

void
mini_stream_lane_header_encode(const mini_stream_lane_header_t *header,
    unsigned char out[MINI_STREAM_LANE_HEADER_SIZE])
{
    memcpy(out, mini_stream_lane_magic, sizeof(mini_stream_lane_magic));
    mini_stream_write_u32(out + 8, MINI_STREAM_PROTOCOL_VERSION);
    mini_stream_write_u32(out + 12, MINI_STREAM_LANE_HEADER_SIZE);
    mini_stream_write_u64(out + 16, header->session_id);
    mini_stream_write_u32(out + 24, header->lane_index);
    mini_stream_write_u32(out + 28, header->lane_count);
    mini_stream_write_u32(out + 32, header->max_frame_payload);
    mini_stream_write_u32(out + 36, 0);
}

int
mini_stream_lane_header_decode(
    const unsigned char data[MINI_STREAM_LANE_HEADER_SIZE],
    mini_stream_lane_header_t *header)
{
    if (data == NULL || header == NULL
        || memcmp(data, mini_stream_lane_magic,
            sizeof(mini_stream_lane_magic)) != 0
        || mini_stream_read_u32(data + 8) != MINI_STREAM_PROTOCOL_VERSION
        || mini_stream_read_u32(data + 12) != MINI_STREAM_LANE_HEADER_SIZE
        || mini_stream_read_u32(data + 36) != 0)
    {
        return -1;
    }
    header->session_id = mini_stream_read_u64(data + 16);
    header->lane_index = mini_stream_read_u32(data + 24);
    header->lane_count = mini_stream_read_u32(data + 28);
    header->max_frame_payload = mini_stream_read_u32(data + 32);
    return 0;
}

void
mini_stream_frame_header_encode(const mini_stream_frame_header_t *header,
    unsigned char out[MINI_STREAM_FRAME_HEADER_SIZE])
{
    mini_stream_write_u32(out, MINI_STREAM_FRAME_MAGIC);
    mini_stream_write_u32(out + 4, header->type);
    mini_stream_write_u64(out + 8, header->offset);
    mini_stream_write_u32(out + 16, header->payload_length);
    mini_stream_write_u32(out + 20, header->flags);
}

int
mini_stream_frame_header_decode(
    const unsigned char data[MINI_STREAM_FRAME_HEADER_SIZE],
    mini_stream_frame_header_t *header)
{
    if (data == NULL || header == NULL
        || mini_stream_read_u32(data) != MINI_STREAM_FRAME_MAGIC)
    {
        return -1;
    }
    header->type = mini_stream_read_u32(data + 4);
    header->offset = mini_stream_read_u64(data + 8);
    header->payload_length = mini_stream_read_u32(data + 16);
    header->flags = mini_stream_read_u32(data + 20);
    return 0;
}

void
mini_stream_receipt_encode(const mini_stream_receipt_t *receipt,
    unsigned char out[MINI_STREAM_RECEIPT_SIZE])
{
    memcpy(out, mini_stream_receipt_magic,
        sizeof(mini_stream_receipt_magic));
    mini_stream_write_u32(out + 8, MINI_STREAM_PROTOCOL_VERSION);
    mini_stream_write_u32(out + 12, receipt->status);
    mini_stream_write_u64(out + 16, receipt->session_id);
    mini_stream_write_u64(out + 24, receipt->total_bytes);
    mini_stream_write_u64(out + 32, receipt->digest);
    mini_stream_write_u64(out + 40, receipt->detail);
}

int
mini_stream_receipt_decode(
    const unsigned char data[MINI_STREAM_RECEIPT_SIZE],
    mini_stream_receipt_t *receipt)
{
    if (data == NULL || receipt == NULL
        || memcmp(data, mini_stream_receipt_magic,
            sizeof(mini_stream_receipt_magic)) != 0
        || mini_stream_read_u32(data + 8) != MINI_STREAM_PROTOCOL_VERSION)
    {
        return -1;
    }
    receipt->status = mini_stream_read_u32(data + 12);
    receipt->session_id = mini_stream_read_u64(data + 16);
    receipt->total_bytes = mini_stream_read_u64(data + 24);
    receipt->digest = mini_stream_read_u64(data + 32);
    receipt->detail = mini_stream_read_u64(data + 40);
    return 0;
}

uint64_t
mini_stream_digest_init(void)
{
    return MINI_STREAM_FNV_OFFSET;
}

uint64_t
mini_stream_digest_update(uint64_t digest, const void *data, size_t data_len)
{
    const unsigned char *bytes = data;
    size_t i;
    if (bytes == NULL) {
        return digest;
    }
    for (i = 0; i < data_len; i++) {
        digest ^= bytes[i];
        digest *= MINI_STREAM_FNV_PRIME;
    }
    return digest;
}
