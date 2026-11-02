#ifndef MINI_OBSERVER_H
#define MINI_OBSERVER_H
#include <event2/event.h>
#include <xquic/netins_udp.h>
#include <xquic/xqc_observer.h>
typedef struct mini_observer_s mini_observer_t;
mini_observer_t *mini_observer_start(xqc_engine_t *, const xqc_cid_t *,
    struct event_base *, const netins_udp_observer_config_t *, const char *,
    void (*)(void *), void *);
void mini_observer_update_cid(mini_observer_t *, const xqc_cid_t *);
void mini_observer_stop(mini_observer_t **);
void mini_observer_resume(mini_observer_t *);
void mini_observer_finish(mini_observer_t *);
#endif
