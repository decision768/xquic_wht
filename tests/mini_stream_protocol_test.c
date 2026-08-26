#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "mini_stream_protocol.h"

int
main(void)
{
    unsigned char lane_wire[MINI_STREAM_LANE_HEADER_SIZE];
    mini_stream_lane_header_t lane = {
        .session_id = 0x0102030405060708ULL,
        .lane_index = 3,
        .lane_count = 4,
        .max_frame_payload = MINI_STREAM_MAX_FRAME_PAYLOAD,
    };
    mini_stream_lane_header_t lane_decoded = {0};
    mini_stream_lane_header_encode(&lane, lane_wire);
    assert(mini_stream_lane_header_decode(lane_wire, &lane_decoded) == 0);
    assert(lane_decoded.session_id == lane.session_id);
    assert(lane_decoded.lane_index == lane.lane_index);
    assert(lane_decoded.lane_count == lane.lane_count);
    assert(lane_decoded.max_frame_payload == lane.max_frame_payload);
    lane_wire[0] ^= 1;
    assert(mini_stream_lane_header_decode(lane_wire, &lane_decoded) != 0);

    unsigned char frame_wire[MINI_STREAM_FRAME_HEADER_SIZE];
    mini_stream_frame_header_t frame = {
        .type = MINI_STREAM_FRAME_DATA,
        .offset = 9876543210ULL,
        .payload_length = 65535,
        .flags = 7,
    };
    mini_stream_frame_header_t frame_decoded = {0};
    mini_stream_frame_header_encode(&frame, frame_wire);
    assert(mini_stream_frame_header_decode(frame_wire, &frame_decoded) == 0);
    assert(frame_decoded.type == frame.type);
    assert(frame_decoded.offset == frame.offset);
    assert(frame_decoded.payload_length == frame.payload_length);
    assert(frame_decoded.flags == frame.flags);

    unsigned char receipt_wire[MINI_STREAM_RECEIPT_SIZE];
    mini_stream_receipt_t receipt = {
        .status = MINI_STREAM_RECEIPT_OK,
        .session_id = lane.session_id,
        .total_bytes = 123456789,
        .digest = 0xfedcba9876543210ULL,
        .detail = UINT64_MAX,
    };
    mini_stream_receipt_t receipt_decoded = {0};
    mini_stream_receipt_encode(&receipt, receipt_wire);
    assert(mini_stream_receipt_decode(receipt_wire, &receipt_decoded) == 0);
    assert(receipt_decoded.status == receipt.status);
    assert(receipt_decoded.session_id == receipt.session_id);
    assert(receipt_decoded.total_bytes == receipt.total_bytes);
    assert(receipt_decoded.digest == receipt.digest);
    assert(receipt_decoded.detail == receipt.detail);

    const char sample[] = "stream-api-protocol";
    uint64_t digest = mini_stream_digest_init();
    digest = mini_stream_digest_update(digest, sample, 7);
    digest = mini_stream_digest_update(digest, sample + 7,
        sizeof(sample) - 1 - 7);
    assert(digest == mini_stream_digest_update(mini_stream_digest_init(),
        sample, sizeof(sample) - 1));

    puts("mini stream protocol tests passed");
    return 0;
}
