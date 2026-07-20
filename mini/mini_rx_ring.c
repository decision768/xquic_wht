#include "mini_rx_ring.h"

#include <stdlib.h>
#include <string.h>

int
xqc_mini_spsc_ring_init(xqc_mini_spsc_ring_t *ring, uint32_t capacity)
{
    if (ring == NULL || capacity == 0) {
        return -1;
    }

    memset(ring, 0, sizeof(*ring));
    ring->slots = calloc(capacity, sizeof(*ring->slots));
    if (ring->slots == NULL) {
        return -1;
    }

    ring->capacity = capacity;
    atomic_init(&ring->producer, 0);
    atomic_init(&ring->consumer, 0);
    atomic_init(&ring->stopping, 0);
    return 0;
}

void
xqc_mini_spsc_ring_destroy(xqc_mini_spsc_ring_t *ring)
{
    if (ring == NULL) {
        return;
    }

    free(ring->slots);
    memset(ring, 0, sizeof(*ring));
}

void
xqc_mini_spsc_ring_stop(xqc_mini_spsc_ring_t *ring)
{
    if (ring != NULL && ring->slots != NULL) {
        atomic_store_explicit(&ring->stopping, 1, memory_order_release);
    }
}

uint32_t
xqc_mini_spsc_ring_size(const xqc_mini_spsc_ring_t *ring)
{
    if (ring == NULL || ring->slots == NULL) {
        return 0;
    }

    uint32_t producer = atomic_load_explicit(&ring->producer,
                                              memory_order_acquire);
    uint32_t consumer = atomic_load_explicit(&ring->consumer,
                                              memory_order_acquire);
    return producer - consumer;
}

uint32_t
xqc_mini_spsc_ring_free(const xqc_mini_spsc_ring_t *ring)
{
    uint32_t used = xqc_mini_spsc_ring_size(ring);
    return ring != NULL && ring->capacity > used ? ring->capacity - used : 0;
}

int
xqc_mini_spsc_ring_push(xqc_mini_spsc_ring_t *ring,
    uint64_t path_id, uint64_t path_generation, xqc_usec_t recv_time,
    const unsigned char *data, size_t len, int *was_empty)
{
    if (ring == NULL || ring->slots == NULL || data == NULL
        || len == 0 || len > XQC_MAX_PACKET_IN_LEN
        || atomic_load_explicit(&ring->stopping, memory_order_acquire))
    {
        return -1;
    }

    uint32_t producer = atomic_load_explicit(&ring->producer,
                                              memory_order_relaxed);
    uint32_t consumer = atomic_load_explicit(&ring->consumer,
                                              memory_order_acquire);
    if (producer - consumer >= ring->capacity) {
        return 0;
    }

    xqc_mini_rx_slot_t *slot = &ring->slots[producer % ring->capacity];
    slot->path_id = path_id;
    slot->path_generation = path_generation;
    slot->recv_time = recv_time;
    slot->len = (uint32_t)len;
    memcpy(slot->data, data, len);

    if (was_empty != NULL) {
        *was_empty = producer == consumer;
    }
    atomic_store_explicit(&ring->producer, producer + 1,
                          memory_order_release);
    return 1;
}

const xqc_mini_rx_slot_t *
xqc_mini_spsc_ring_peek(const xqc_mini_spsc_ring_t *ring)
{
    if (ring == NULL || ring->slots == NULL) {
        return NULL;
    }

    uint32_t consumer = atomic_load_explicit(&ring->consumer,
                                              memory_order_relaxed);
    uint32_t producer = atomic_load_explicit(&ring->producer,
                                              memory_order_acquire);
    if (consumer == producer) {
        return NULL;
    }

    return &ring->slots[consumer % ring->capacity];
}

void
xqc_mini_spsc_ring_consume(xqc_mini_spsc_ring_t *ring)
{
    if (ring == NULL || ring->slots == NULL) {
        return;
    }

    uint32_t consumer = atomic_load_explicit(&ring->consumer,
                                              memory_order_relaxed);
    atomic_store_explicit(&ring->consumer, consumer + 1,
                          memory_order_release);
}
