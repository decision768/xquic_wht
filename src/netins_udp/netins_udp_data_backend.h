#ifndef NETINS_UDP_DATA_BACKEND_H
#define NETINS_UDP_DATA_BACKEND_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include <xquic/netins_udp.h>

#include "netins_udp_v2_notifier.h"

typedef struct netins_udp_data_backend_s netins_udp_data_backend_t;

typedef struct netins_udp_data_backend_config_s {
    netins_udp_t *connection;
    netins_udp_v2_notifier_t *parent_notifier;
    int role;
    int nonblock;
    uint32_t send_timeout_ms;
    uint32_t recv_timeout_ms;
    uint32_t close_timeout_ms;
} netins_udp_data_backend_config_t;

int netins_udp_data_backend_create(
    const netins_udp_data_backend_config_t *config,
    netins_udp_data_backend_t **out_backend);

/*
 * Opens or accepts the private parallel HTTP/3 lanes. EAGAIN is retryable:
 * successfully established lanes remain owned by the backend.
 */
int netins_udp_data_backend_establish(
    netins_udp_data_backend_t *backend, int dontwait);

ssize_t netins_udp_data_backend_send(netins_udp_data_backend_t *backend,
    const void *data, size_t length, int flags);
ssize_t netins_udp_data_backend_recv(netins_udp_data_backend_t *backend,
    void *data, size_t length, int flags);

/* Flushes queued bytes and closes the local byte-stream send direction. */
int netins_udp_data_backend_finish_send(
    netins_udp_data_backend_t *backend);

/* Returns connection-level READABLE/WRITABLE readiness for the byte API. */
int netins_udp_data_backend_get_events(
    netins_udp_data_backend_t *backend, uint32_t *out_events);

/*
 * Returns the byte counts visible through the public connection-level API.
 * Internal channel framing is deliberately excluded.
 */
int netins_udp_data_backend_get_counters(
    netins_udp_data_backend_t *backend,
    uint64_t *out_bytes_sent, uint64_t *out_bytes_received);

int netins_udp_data_backend_get_error(
    netins_udp_data_backend_t *backend, int *out_error,
    char *message, size_t message_capacity);

void netins_udp_data_backend_destroy(
    netins_udp_data_backend_t *backend);

#endif /* NETINS_UDP_DATA_BACKEND_H */
