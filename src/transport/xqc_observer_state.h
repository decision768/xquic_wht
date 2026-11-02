/* Passive STREAM interval accounting. No transport decisions depend on this state. */
#ifndef XQC_OBSERVER_STATE_H
#define XQC_OBSERVER_STATE_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#define XQC_OBSERVER_MAX_INTERVALS 256U
typedef struct { uint64_t begin, end; } xqc_observer_interval_t;
typedef struct {
    uint64_t c, n, b, a, last_us, pending_us, exposed_us, byte_us;
    uint64_t peak, interval_peak, episodes, episode_start, episode_peak;
    uint64_t completed_episodes, censored_episodes;
    xqc_observer_interval_t *ranges;
    unsigned count, capacity;
    int initialized, invalid, ended;
} xqc_observer_state_t;
static inline uint64_t
xqc_observer_add(uint64_t a, uint64_t b, int *invalid)
{
    if (UINT64_MAX - a < b) { *invalid = 1; return UINT64_MAX; }
    return a + b;
}
static inline void
xqc_observer_settle(xqc_observer_state_t *s, uint64_t now)
{
    if (!s->initialized) { s->initialized = 1; s->last_us = now; }
    if (now < s->last_us) { s->invalid = 1; return; }
    if (s->ended) { return; }
    uint64_t dt = now - s->last_us;
    if (s->b) {
        s->pending_us = xqc_observer_add(s->pending_us, dt, &s->invalid);
        if (s->n == s->c) {
            s->exposed_us = xqc_observer_add(s->exposed_us, dt, &s->invalid);
        }
        if (dt && s->b > UINT64_MAX / dt) { s->invalid = 1; s->byte_us = UINT64_MAX; }
        else { s->byte_us = xqc_observer_add(s->byte_us, s->b * dt, &s->invalid); }
    }
    s->last_us = now;
}
/* Returns 1 for episode start, -1 for release, 0 otherwise. Old episode
 * metadata remains available to the caller until the next episode starts. */
static inline int
xqc_observer_update(xqc_observer_state_t *s, uint64_t begin, uint64_t end,
    uint64_t c, uint64_t n, uint64_t now)
{
    xqc_observer_settle(s, now);
    if (s->ended || s->invalid) { return 0; }
    uint64_t old_b = s->b;
    s->c = c; s->n = n; s->a = c > n ? c - n : 0;
    /* Discard intervals covered by the new continuous prefix. */
    unsigned used = 0;
    for (unsigned i = 0; i < s->count; ++i) {
        if (s->ranges[i].end > c) {
            s->ranges[used] = s->ranges[i];
            if (s->ranges[used].begin < c) { s->ranges[used].begin = c; }
            ++used;
        }
    }
    s->count = used;
    if (end > c && end > begin) {
        if (begin < c) { begin = c; }
        unsigned first = 0;
        while (first < s->count && s->ranges[first].end < begin) { ++first; }
        unsigned last = first;
        while (last < s->count && s->ranges[last].begin <= end) {
            if (s->ranges[last].begin < begin) { begin = s->ranges[last].begin; }
            if (s->ranges[last].end > end) { end = s->ranges[last].end; }
            ++last;
        }
        unsigned required = s->count - (last - first) + 1;
        if (required > s->capacity) {
            unsigned cap = s->capacity ? s->capacity * 2 : 8;
            if (cap > XQC_OBSERVER_MAX_INTERVALS) { cap = XQC_OBSERVER_MAX_INTERVALS; }
            if (required > cap) { s->invalid = 1; return 0; }
            void *p = realloc(s->ranges, cap * sizeof(*s->ranges));
            if (!p) { s->invalid = 1; return 0; }
            s->ranges = p; s->capacity = cap;
        }
        memmove(s->ranges + first + 1, s->ranges + last,
            (s->count - last) * sizeof(*s->ranges));
        s->ranges[first].begin = begin; s->ranges[first].end = end;
        s->count = required;
    }
    s->b = 0;
    for (unsigned i = 0; i < s->count; ++i) {
        s->b = xqc_observer_add(s->b,
            s->ranges[i].end - s->ranges[i].begin, &s->invalid);
    }
    if (s->b > s->peak) { s->peak = s->b; }
    if (s->b > s->interval_peak) { s->interval_peak = s->b; }
    if (!old_b && s->b) {
        s->episodes++; s->episode_start = now; s->episode_peak = s->b;
        return 1;
    }
    if (s->b > s->episode_peak) { s->episode_peak = s->b; }
    if (old_b && !s->b) { s->completed_episodes++; return -1; }
    return 0;
}
#endif
