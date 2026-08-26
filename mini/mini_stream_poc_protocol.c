#include "mini_stream_poc_protocol.h"

#include <string.h>

#define XQC_MINI_STREAM_POC_FNV_OFFSET 14695981039346656037ULL
#define XQC_MINI_STREAM_POC_FNV_PRIME 1099511628211ULL

static const unsigned char xqc_mini_stream_poc_receipt_magic[8] = {
    'N', 'S', 'B', 'S', 'A', 'C', 'K', '1'
};

static uint64_t
xqc_mini_stream_poc_pattern_word(uint32_t seed, uint64_t word_index)
{
    uint64_t value = word_index
        + (((uint64_t)seed << 32) | seed)
        + 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}

void
xqc_mini_stream_poc_fill(uint32_t seed, uint64_t offset,
    unsigned char *data, size_t data_len)
{
    if (data == NULL) {
        return;
    }
    size_t i = 0;
    while (i < data_len) {
        uint64_t logical_offset = offset + i;
        size_t byte_in_word = (size_t)(logical_offset % 8);
        size_t count = 8 - byte_in_word;
        if (count > data_len - i) {
            count = data_len - i;
        }
        uint64_t word = xqc_mini_stream_poc_pattern_word(seed,
            logical_offset / 8);
        for (size_t j = 0; j < count; j++) {
            data[i + j] = (unsigned char)(word
                >> ((byte_in_word + j) * 8));
        }
        i += count;
    }
}

int
xqc_mini_stream_poc_verify(uint32_t seed, uint64_t offset,
    const unsigned char *data, size_t data_len, uint64_t *bad_offset)
{
    if (data == NULL && data_len != 0) {
        return -1;
    }
    size_t i = 0;
    while (i < data_len) {
        uint64_t logical_offset = offset + i;
        size_t byte_in_word = (size_t)(logical_offset % 8);
        size_t count = 8 - byte_in_word;
        if (count > data_len - i) {
            count = data_len - i;
        }
        uint64_t word = xqc_mini_stream_poc_pattern_word(seed,
            logical_offset / 8);
        for (size_t j = 0; j < count; j++) {
            unsigned char expected = (unsigned char)(word
                >> ((byte_in_word + j) * 8));
            if (data[i + j] == expected) {
                continue;
            }
            if (bad_offset != NULL) {
                *bad_offset = logical_offset + j;
            }
            return -1;
        }
        i += count;
    }
    return 0;
}

uint64_t
xqc_mini_stream_poc_digest_init(void)
{
    return XQC_MINI_STREAM_POC_FNV_OFFSET;
}

uint64_t
xqc_mini_stream_poc_digest_update(uint64_t digest,
    const unsigned char *data, size_t data_len)
{
    if (data == NULL) {
        return digest;
    }
    for (size_t i = 0; i < data_len; i++) {
        digest ^= data[i];
        digest *= XQC_MINI_STREAM_POC_FNV_PRIME;
    }
    return digest;
}

static void
xqc_mini_stream_poc_write_u32(unsigned char *out, uint32_t value)
{
    out[0] = (unsigned char)(value >> 24);
    out[1] = (unsigned char)(value >> 16);
    out[2] = (unsigned char)(value >> 8);
    out[3] = (unsigned char)value;
}

static void
xqc_mini_stream_poc_write_u64(unsigned char *out, uint64_t value)
{
    for (size_t i = 0; i < 8; i++) {
        out[i] = (unsigned char)(value >> (56 - i * 8));
    }
}

static uint32_t
xqc_mini_stream_poc_read_u32(const unsigned char *data)
{
    return ((uint32_t)data[0] << 24)
        | ((uint32_t)data[1] << 16)
        | ((uint32_t)data[2] << 8)
        | (uint32_t)data[3];
}

static uint64_t
xqc_mini_stream_poc_read_u64(const unsigned char *data)
{
    uint64_t value = 0;
    for (size_t i = 0; i < 8; i++) {
        value = (value << 8) | data[i];
    }
    return value;
}

void
xqc_mini_stream_poc_receipt_encode(
    const xqc_mini_stream_poc_receipt_t *receipt,
    unsigned char out[XQC_MINI_STREAM_POC_RECEIPT_SIZE])
{
    memcpy(out, xqc_mini_stream_poc_receipt_magic,
        sizeof(xqc_mini_stream_poc_receipt_magic));
    xqc_mini_stream_poc_write_u32(out + 8,
        XQC_MINI_STREAM_POC_RECEIPT_VERSION);
    xqc_mini_stream_poc_write_u32(out + 12, receipt->status);
    xqc_mini_stream_poc_write_u64(out + 16, receipt->total_bytes);
    xqc_mini_stream_poc_write_u64(out + 24, receipt->digest);
    xqc_mini_stream_poc_write_u64(out + 32, receipt->first_bad_offset);
}

int
xqc_mini_stream_poc_receipt_decode(
    const unsigned char data[XQC_MINI_STREAM_POC_RECEIPT_SIZE],
    xqc_mini_stream_poc_receipt_t *receipt)
{
    if (data == NULL || receipt == NULL
        || memcmp(data, xqc_mini_stream_poc_receipt_magic,
            sizeof(xqc_mini_stream_poc_receipt_magic)) != 0
        || xqc_mini_stream_poc_read_u32(data + 8)
            != XQC_MINI_STREAM_POC_RECEIPT_VERSION)
    {
        return -1;
    }
    receipt->status = xqc_mini_stream_poc_read_u32(data + 12);
    receipt->total_bytes = xqc_mini_stream_poc_read_u64(data + 16);
    receipt->digest = xqc_mini_stream_poc_read_u64(data + 24);
    receipt->first_bad_offset = xqc_mini_stream_poc_read_u64(data + 32);
    return 0;
}
