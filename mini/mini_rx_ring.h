#ifndef XQC_MINI_RX_RING_H
#define XQC_MINI_RX_RING_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#include "src/transport/xqc_packet_in.h"

typedef struct xqc_mini_rx_slot_s {
    uint64_t        path_id;
    uint64_t        path_generation;
    xqc_usec_t      recv_time;
    uint32_t        len;
    unsigned char   data[XQC_MAX_PACKET_IN_LEN];
} xqc_mini_rx_slot_t;

typedef struct xqc_mini_spsc_ring_s {
    xqc_mini_rx_slot_t *slots;
    uint32_t             capacity;
    _Atomic uint32_t     producer;
    _Atomic uint32_t     consumer;
    _Atomic int          stopping;
} xqc_mini_spsc_ring_t;

int xqc_mini_spsc_ring_init(xqc_mini_spsc_ring_t *ring, uint32_t capacity);
void xqc_mini_spsc_ring_destroy(xqc_mini_spsc_ring_t *ring);
void xqc_mini_spsc_ring_stop(xqc_mini_spsc_ring_t *ring);
uint32_t xqc_mini_spsc_ring_size(const xqc_mini_spsc_ring_t *ring);
uint32_t xqc_mini_spsc_ring_free(const xqc_mini_spsc_ring_t *ring);
int xqc_mini_spsc_ring_push(xqc_mini_spsc_ring_t *ring,
    uint64_t path_id, uint64_t path_generation, xqc_usec_t recv_time,
    const unsigned char *data, size_t len, int *was_empty);
const xqc_mini_rx_slot_t *xqc_mini_spsc_ring_peek(
    const xqc_mini_spsc_ring_t *ring);
void xqc_mini_spsc_ring_consume(xqc_mini_spsc_ring_t *ring);

#endif
