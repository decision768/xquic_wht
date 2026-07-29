#include <assert.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>

#include "netins_udp_v2_notifier.h"
#include "netins_udp_v2_transport.h"

static int
is_readable(int fd)
{
    struct pollfd descriptor = {
        .fd = fd,
        .events = POLLIN,
    };
    return poll(&descriptor, 1, 0) == 1
        && (descriptor.revents & POLLIN) != 0;
}

int
main(void)
{
    netins_udp_v2_notifier_t first;
    netins_udp_v2_notifier_t second;
    assert(netins_udp_v2_notifier_init(&first) == 0);
    assert(netins_udp_v2_notifier_init(&second) == 0);
    int first_fd = netins_udp_v2_notifier_get_fd(&first);
    int second_fd = netins_udp_v2_notifier_get_fd(&second);
    assert(first_fd >= 0);
    assert(second_fd >= 0);
    assert(first_fd != second_fd);

    /* Level notifications wake the fd but are never returned stale. */
    netins_udp_v2_notifier_signal(&first, NETINS_UDP_V2_EVENT_READABLE);
    assert(is_readable(first_fd));
    uint32_t events = UINT32_MAX;
    uint64_t generation =
        netins_udp_v2_notifier_begin_level_query(&first);
    assert(netins_udp_v2_notifier_consume(&first, generation, 0,
        &events) == 0);
    assert(events == 0);
    assert(!is_readable(first_fd));

    /* Transition events are latched and coalesced until get_events. */
    netins_udp_v2_notifier_signal(&first, NETINS_UDP_V2_EVENT_CONNECTED);
    netins_udp_v2_notifier_signal(&first,
        NETINS_UDP_V2_EVENT_CONNECTED | NETINS_UDP_V2_EVENT_PATH_CHANGED);
    assert(is_readable(first_fd));
    generation = netins_udp_v2_notifier_begin_level_query(&first);
    assert(netins_udp_v2_notifier_consume(&first, generation, 0,
        &events) == 0);
    assert(events == (NETINS_UDP_V2_EVENT_CONNECTED
        | NETINS_UDP_V2_EVENT_PATH_CHANGED));
    assert(!is_readable(first_fd));

    /*
     * WRITABLE is reported as a level, but does not continuously rearm the
     * shared notification fd and spin receive-only event loops.
     */
    netins_udp_v2_notifier_signal(&first, NETINS_UDP_V2_EVENT_WRITABLE);
    generation = netins_udp_v2_notifier_begin_level_query(&first);
    assert(netins_udp_v2_notifier_consume(&first, generation,
        NETINS_UDP_V2_EVENT_WRITABLE, &events) == 0);
    assert(events == NETINS_UDP_V2_EVENT_WRITABLE);
    assert(!is_readable(first_fd));

    /* A live readable condition remains armed until it is drained. */
    netins_udp_v2_notifier_signal(&first, NETINS_UDP_V2_EVENT_READABLE);
    generation = netins_udp_v2_notifier_begin_level_query(&first);
    assert(netins_udp_v2_notifier_consume(&first, generation,
        NETINS_UDP_V2_EVENT_READABLE,
        &events) == 0);
    assert(events == NETINS_UDP_V2_EVENT_READABLE);
    assert(is_readable(first_fd));
    generation = netins_udp_v2_notifier_begin_level_query(&first);
    assert(netins_udp_v2_notifier_consume(&first, generation, 0,
        &events) == 0);
    assert(events == 0);
    assert(!is_readable(first_fd));

    /*
     * A level signal racing between backend sampling and pipe consumption
     * must leave one wakeup armed for a fresh readiness query.
     */
    generation = netins_udp_v2_notifier_begin_level_query(&first);
    netins_udp_v2_notifier_signal(&first,
        NETINS_UDP_V2_EVENT_READABLE);
    assert(netins_udp_v2_notifier_consume(&first, generation, 0,
        &events) == 0);
    assert(events == 0);
    assert(is_readable(first_fd));
    generation = netins_udp_v2_notifier_begin_level_query(&first);
    assert(netins_udp_v2_notifier_consume(&first, generation, 0,
        &events) == 0);
    assert(events == 0);
    assert(!is_readable(first_fd));

    assert(!is_readable(second_fd));
    netins_udp_v2_notifier_destroy(&second);
    netins_udp_v2_notifier_destroy(&first);
    printf("netins_udp_v2_notifier_unit_test: PASS\n");
    return 0;
}
