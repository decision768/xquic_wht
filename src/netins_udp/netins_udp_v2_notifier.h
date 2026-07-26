#ifndef NETINS_UDP_V2_NOTIFIER_H
#define NETINS_UDP_V2_NOTIFIER_H

#include <pthread.h>
#include <stdint.h>

typedef struct netins_udp_v2_notifier_s {
    pthread_mutex_t lock;
    int read_fd;
    int write_fd;
    uint32_t latched_events;
    uint64_t level_generation;
    int signaled;
    int initialized;
} netins_udp_v2_notifier_t;

int netins_udp_v2_notifier_init(netins_udp_v2_notifier_t *notifier);
void netins_udp_v2_notifier_destroy(netins_udp_v2_notifier_t *notifier);
int netins_udp_v2_notifier_get_fd(netins_udp_v2_notifier_t *notifier);
void netins_udp_v2_notifier_signal(netins_udp_v2_notifier_t *notifier,
    uint32_t events);
uint64_t netins_udp_v2_notifier_begin_level_query(
    netins_udp_v2_notifier_t *notifier);
int netins_udp_v2_notifier_consume(netins_udp_v2_notifier_t *notifier,
    uint64_t level_generation, uint32_t level_events, uint32_t *out_events);

#endif /* NETINS_UDP_V2_NOTIFIER_H */
