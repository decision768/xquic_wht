#include <assert.h>
#include <stddef.h>

#include <xquic/netins_udp.h>

_Static_assert(NETINS_UDP_STREAM_MAX_LANES == 8,
    "stream API v1 lane limit changed");
_Static_assert(NETINS_UDP_STREAM_ENDPOINT_OPEN == 1,
    "stream endpoint ABI changed");
_Static_assert(NETINS_UDP_STREAM_ENDPOINT_ACCEPT == 2,
    "stream endpoint ABI changed");
_Static_assert(NETINS_UDP_STREAM_STATE_ACTIVE == 1,
    "stream state ABI changed");
_Static_assert(NETINS_UDP_STREAM_STATE_ERROR == 5,
    "stream state ABI changed");

int
main(void)
{
    netins_udp_stream_config_t config =
        NETINS_UDP_STREAM_CONFIG_INITIALIZER;
    netins_udp_stream_result_t result =
        NETINS_UDP_STREAM_RESULT_INITIALIZER;
    netins_udp_stream_snapshot_t snapshot =
        NETINS_UDP_STREAM_SNAPSHOT_INITIALIZER;

    assert(config.struct_size == sizeof(config));
    assert(config.api_version == NETINS_UDP_API_VERSION);
    assert(config.lane_count == 1);
    assert(config.send_queue_size == NETINS_UDP_STREAM_DEFAULT_QUEUE_SIZE);
    assert(config.receive_queue_size == NETINS_UDP_STREAM_DEFAULT_QUEUE_SIZE);
    assert(result.struct_size == sizeof(result));
    assert(result.api_version == NETINS_UDP_API_VERSION);
    assert(snapshot.struct_size == sizeof(snapshot));
    assert(snapshot.api_version == NETINS_UDP_API_VERSION);
    assert(sizeof(snapshot.lane_payload_sent) / sizeof(uint64_t)
        == NETINS_UDP_STREAM_MAX_LANES);
    assert(sizeof(snapshot.lane_payload_received) / sizeof(uint64_t)
        == NETINS_UDP_STREAM_MAX_LANES);
    assert(offsetof(netins_udp_stream_snapshot_t, app_error_code)
        > offsetof(netins_udp_stream_snapshot_t, last_error));
    return 0;
}
