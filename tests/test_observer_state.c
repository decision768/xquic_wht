#include "src/transport/xqc_observer_state.h"
#include <assert.h>
#include <stdio.h>

static void clear(xqc_observer_state_t *s) { free(s->ranges); memset(s, 0, sizeof(*s)); }
int main(void)
{
    xqc_observer_state_t s = {0};
    xqc_observer_update(&s, 0, 1000, 1000, 0, 0);
    xqc_observer_update(&s, 1000, 2000, 2000, 0, 20000);
    assert(s.b == 0 && s.a == 2000 && s.pending_us == 0);
    clear(&s);
    assert(xqc_observer_update(&s, 1000, 2000, 0, 0, 0) == 1);
    assert(xqc_observer_update(&s, 0, 1000, 2000, 0, 20000) == -1);
    assert(s.pending_us == 20000 && s.exposed_us == 20000);
    assert(s.byte_us == 20000000 && s.peak == 1000 && s.episodes == 1);
    clear(&s);
    xqc_observer_update(&s, 0, 1000, 1000, 0, 0);
    xqc_observer_update(&s, 2000, 3000, 1000, 0, 0);
    xqc_observer_update(&s, 0, 0, 1000, 1000, 5000);
    xqc_observer_update(&s, 1000, 2000, 3000, 1000, 20000);
    assert(s.pending_us == 20000 && s.exposed_us == 15000);
    clear(&s);
    xqc_observer_update(&s, 1000, 2000, 0, 0, 0);
    xqc_observer_update(&s, 1500, 2500, 0, 0, 1000);
    xqc_observer_update(&s, 1000, 2000, 0, 0, 2000);
    assert(s.b == 1500 && s.count == 1 && s.episodes == 1);
    xqc_observer_update(&s, 3000, 4000, 0, 0, 3000);
    xqc_observer_update(&s, 0, 1000, 2500, 0, 4000);
    assert(s.b == 1000 && s.episodes == 1 && s.pending_us == 4000);
    xqc_observer_update(&s, 2500, 3000, 4000, 0, 10000);
    xqc_observer_settle(&s, 100000);
    assert(s.pending_us == 10000 && s.peak == 2500 && s.completed_episodes == 1);
    clear(&s);
    /* Resource overflow invalidates telemetry, never touches business data. */
    for (unsigned i = 0; i <= XQC_OBSERVER_MAX_INTERVALS; ++i) {
        xqc_observer_update(&s, 2*i+1, 2*i+2, 0, 0, i);
    }
    assert(s.invalid);
    clear(&s);
    xqc_observer_update(&s, 1, UINT64_MAX, 0, 0, 0);
    xqc_observer_settle(&s, 2);
    assert(s.invalid && s.byte_us == UINT64_MAX);
    clear(&s);
    /* Independent byte bitmap oracle checks overlap, gaps and time integrals. */
    for (unsigned trial = 0; trial < 20; ++trial) {
        unsigned char received[2048] = {0};
        uint64_t c = 0, n = 0, b = 0, pending = 0, exposed = 0, area = 0;
        for (unsigned step = 0; step < 500; ++step) {
            unsigned begin = (unsigned)rand() % 1500;
            unsigned end = begin + (unsigned)rand() % 40;
            uint64_t dt = (unsigned)rand() % 1000;
            uint64_t now = s.last_us + dt;
            if (b) { pending += dt; if (n == c) { exposed += dt; } area += b * dt; }
            for (unsigned offset = begin; offset < end; ++offset) { received[offset] = 1; }
            while (c < sizeof(received) && received[c]) { ++c; }
            if (rand() % 3 == 0) { n = c; }
            b = 0;
            for (unsigned offset = (unsigned)c; offset < sizeof(received); ++offset) { b += received[offset]; }
            xqc_observer_update(&s, begin, end, c, n, now);
            assert(!s.invalid && s.b == b && s.a == c-n);
            assert(s.pending_us == pending && s.exposed_us == exposed && s.byte_us == area);
        }
        clear(&s);
    }
    puts("observer interval tests passed");
    return 0;
}
