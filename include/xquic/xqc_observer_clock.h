#ifndef XQC_OBSERVER_CLOCK_H
#define XQC_OBSERVER_CLOCK_H
#include <stdint.h>
#include <time.h>
#include <sys/time.h>

#define XQC_OBSERVER_REVISION 2
#define XQC_OBSERVER_CLOCK_DOMAIN "CLOCK_MONOTONIC"

/* Observer-only clock: never replace the engine's existing clock callbacks. */
static inline uint64_t
xqc_observer_clock_us(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0 || ts.tv_sec < 0) { return 0; }
    return (uint64_t)ts.tv_sec * UINT64_C(1000000) + (uint64_t)ts.tv_nsec / 1000;
}

static inline uint64_t
xqc_observer_realtime_us(void)
{
    struct timeval tv;
    if (gettimeofday(&tv, NULL) != 0 || tv.tv_sec < 0) { return 0; }
    return (uint64_t)tv.tv_sec * UINT64_C(1000000) + (uint64_t)tv.tv_usec;
}

static inline uint64_t
xqc_observer_elapsed_us(uint64_t start, uint64_t end)
{
    return !start || !end || end < start ? UINT64_MAX : end - start;
}
#endif
