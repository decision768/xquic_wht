#include "mini_stream_poc_protocol.h"

#include <stdio.h>
#include <string.h>

static int
test_pattern_and_digest(void)
{
    unsigned char first[257];
    unsigned char second[257];
    xqc_mini_stream_poc_fill(7, 0, first, sizeof(first));
    xqc_mini_stream_poc_fill(7, 113, second, sizeof(second));
    if (memcmp(first + 113, second, sizeof(first) - 113) != 0) {
        fprintf(stderr, "pattern is not stable across chunk boundaries\n");
        return -1;
    }

    uint64_t bad_offset = XQC_MINI_STREAM_POC_NO_BAD_OFFSET;
    if (xqc_mini_stream_poc_verify(7, 113, second, sizeof(second),
            &bad_offset) != 0)
    {
        fprintf(stderr, "valid generated pattern was rejected\n");
        return -1;
    }
    second[91] ^= 0x80;
    if (xqc_mini_stream_poc_verify(7, 113, second, sizeof(second),
            &bad_offset) == 0 || bad_offset != 204)
    {
        fprintf(stderr, "corruption offset was not detected\n");
        return -1;
    }
    second[91] ^= 0x80;

    uint64_t whole = xqc_mini_stream_poc_digest_update(
        xqc_mini_stream_poc_digest_init(), second, sizeof(second));
    uint64_t split = xqc_mini_stream_poc_digest_init();
    split = xqc_mini_stream_poc_digest_update(split, second, 31);
    split = xqc_mini_stream_poc_digest_update(split, second + 31,
        sizeof(second) - 31);
    if (whole != split) {
        fprintf(stderr, "digest depends on callback chunk boundaries\n");
        return -1;
    }
    return 0;
}

static int
test_receipt_codec(void)
{
    xqc_mini_stream_poc_receipt_t input = {
        .status = XQC_MINI_STREAM_POC_STATUS_PATTERN_MISMATCH,
        .total_bytes = 0x0102030405060708ULL,
        .digest = 0x1122334455667788ULL,
        .first_bad_offset = 0x8877665544332211ULL,
    };
    unsigned char encoded[XQC_MINI_STREAM_POC_RECEIPT_SIZE];
    xqc_mini_stream_poc_receipt_encode(&input, encoded);
    if (memcmp(encoded, "NSBSACK1", 8) != 0
        || encoded[8] != 0 || encoded[9] != 0
        || encoded[10] != 0 || encoded[11] != 1
        || encoded[16] != 0x01 || encoded[23] != 0x08)
    {
        fprintf(stderr, "receipt is not encoded in canonical network order\n");
        return -1;
    }

    xqc_mini_stream_poc_receipt_t output = {0};
    if (xqc_mini_stream_poc_receipt_decode(encoded, &output) != 0
        || output.status != input.status
        || output.total_bytes != input.total_bytes
        || output.digest != input.digest
        || output.first_bad_offset != input.first_bad_offset)
    {
        fprintf(stderr, "receipt codec round trip failed\n");
        return -1;
    }
    encoded[0] ^= 1;
    if (xqc_mini_stream_poc_receipt_decode(encoded, &output) == 0) {
        fprintf(stderr, "invalid receipt magic was accepted\n");
        return -1;
    }
    return 0;
}

int
main(void)
{
    if (test_pattern_and_digest() != 0 || test_receipt_codec() != 0) {
        return 1;
    }
    printf("mini stream POC protocol tests passed\n");
    return 0;
}
