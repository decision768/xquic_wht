#include "mini_observer.h"
#include "../demo/netins_udp_telemetry_json.h"
#include <xquic/xqc_observer_clock.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

struct mini_observer_s {
    xqc_engine_t *engine;
    xqc_cid_t cid;
    struct event *timer;
    netins_telemetry_writer_t *writer;
    uint64_t interval_us, due_us;
    int finished;
    void (*extra)(void *);
    void *extra_data;
};

static uint64_t
mini_observer_now(mini_observer_t *o)
{
    uint64_t now = xqc_observer_clock_us();
    if (!now) { xqc_observer_phase(o->engine, &o->cid, "observer_clock_failed", 0, 0, UINT64_MAX); }
    return now;
}

static void
mini_observer_tick(evutil_socket_t fd, short events, void *arg)
{
    (void)fd; (void)events;
    mini_observer_t *o = arg;
    uint64_t now = mini_observer_now(o);
    uint64_t missed = now > o->due_us ? (now - o->due_us) / o->interval_us : 0;
    if (o->extra) { o->extra(o->extra_data); }
    xqc_observer_sample(o->engine, &o->cid, "periodic", o->due_us, o->interval_us, missed);
    o->due_us += (missed + 1) * o->interval_us;
    now = mini_observer_now(o);
    uint64_t delay = o->due_us > now ? o->due_us - now : 1;
    struct timeval tv = {(long)(delay / 1000000), (long)(delay % 1000000)};
    if (event_add(o->timer, &tv)) {
        fprintf(stderr, "observer_error: cannot rearm timer\n");
    }
}

mini_observer_t *
mini_observer_start(xqc_engine_t *engine, const xqc_cid_t *cid,
    struct event_base *base, const netins_udp_observer_config_t *config,
    const char *endpoint, void (*extra)(void *), void *extra_data)
{
    if (!config || !config->enabled) { return NULL; }
    mini_observer_t *o = calloc(1, sizeof(*o));
    if (!o) { fprintf(stderr, "observer_error: allocation failed\n"); return NULL; }
    o->engine = engine; o->cid = *cid; o->interval_us = (uint64_t)config->interval_ms * 1000;
    o->extra = extra; o->extra_data = extra_data;
    o->writer = netins_telemetry_open(config->output_prefix);
    if (!o->writer || xqc_observer_attach(engine, cid, endpoint, config->attempt_id,
        netins_telemetry_write, o->writer))
    {
        fprintf(stderr, "observer_error: writer or attach failed\n");
        netins_telemetry_close(o->writer); free(o); return NULL;
    }
    o->timer = event_new(base, -1, EV_TIMEOUT, mini_observer_tick, o);
    if (!o->timer) { mini_observer_stop(&o); return NULL; }
    o->due_us = mini_observer_now(o);
    if (o->extra) { o->extra(o->extra_data); }
    xqc_observer_sample(engine, cid, "start", o->due_us, o->interval_us, 0);
    o->due_us += o->interval_us;
    struct timeval tv = {(long)(o->interval_us / 1000000), (long)(o->interval_us % 1000000)};
    if (event_add(o->timer, &tv)) { mini_observer_stop(&o); return NULL; }
    return o;
}

void
mini_observer_finish(mini_observer_t *o)
{
    if (!o || o->finished) { return; }
    if (o->extra) { o->extra(o->extra_data); }
    xqc_observer_phase(o->engine, &o->cid, "task_end", 0, 0, 0);
    xqc_observer_sample(o->engine, &o->cid, "end", mini_observer_now(o), o->interval_us, 0);
    char metadata[768];
    if (xqc_observer_checkpoint_metadata(o->engine, &o->cid, metadata, sizeof(metadata))
        || netins_telemetry_checkpoint(o->writer, metadata)) {
        fprintf(stderr, "observer_error: cannot publish task checkpoint\n");
    }
    o->finished = 1;
    if (o->timer) { event_del(o->timer); }
}

void
mini_observer_stop(mini_observer_t **address)
{
    if (!address || !*address) { return; }
    mini_observer_t *o = *address;
    mini_observer_finish(o);
    if (o->timer) { event_free(o->timer); }
    xqc_observer_detach(o->engine, &o->cid);
    netins_telemetry_close(o->writer);
    free(o); *address = NULL;
}

void
mini_observer_resume(mini_observer_t *o)
{
    if (!o || !o->finished) { return; }
    o->finished = 0; o->due_us = mini_observer_now(o) + o->interval_us;
    struct timeval tv = {(long)(o->interval_us / 1000000), (long)(o->interval_us % 1000000)};
    if (event_add(o->timer, &tv)) { fprintf(stderr, "observer_error: cannot resume timer\n"); }
}

void
mini_observer_update_cid(mini_observer_t *observer, const xqc_cid_t *cid)
{
    if (observer && cid) { observer->cid = *cid; }
}
