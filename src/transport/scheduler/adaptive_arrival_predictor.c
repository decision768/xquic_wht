#include "adaptive_arrival_predictor.h"
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <limits.h>

#ifndef XQC_ARRIVAL_BUILD_ID
#define XQC_ARRIVAL_BUILD_ID "adaptive-arrival-v1-unverified-build-" __DATE__ "-" __TIME__
#endif
const char *xqc_arrival_source_build_id(void) { return XQC_ARRIVAL_BUILD_ID; }

void xqc_arrival_config_init(xqc_arrival_config_t *c)
{
    memset(c, 0, sizeof(*c)); c->struct_size = sizeof(*c); c->version = 1;
    c->predict_horizon_us = 20000; c->max_defer_episode_us = 5000;
    c->max_defer_stream_us = 20000; c->recheck_interval_us = 1000;
    c->gain_margin_floor_us = 2000; c->gain_margin_rtt_ratio = .10;
    c->min_valid_rate_samples = 8; c->rate_sample_capacity = 16;
    c->ack_batch_capacity = 16; c->min_ack_batches = 4;
    c->max_sim_events_per_path = 64; c->max_waiting_streams = 16;
    c->schedule_scan_budget = 256; c->prediction_log_limit_per_s = 100;
}
int xqc_arrival_config_validate(const xqc_arrival_config_t *c)
{
    return c && c->struct_size == sizeof(*c) && c->version == 1 && c->mode <= 2
        && c->predict_horizon_us && c->predict_horizon_us <= 1000000
        && c->max_defer_episode_us && c->max_defer_episode_us <= c->predict_horizon_us
        && c->max_defer_stream_us && c->recheck_interval_us
        && isfinite(c->gain_margin_rtt_ratio) && c->gain_margin_rtt_ratio >= 0
        && c->gain_margin_rtt_ratio <= 10
        && c->min_valid_rate_samples >= 2 && c->rate_sample_capacity <= 16
        && c->min_valid_rate_samples <= c->rate_sample_capacity
        && c->min_ack_batches >= 4 && c->ack_batch_capacity <= 16
        && c->min_ack_batches <= c->ack_batch_capacity
        && c->max_sim_events_per_path && c->max_sim_events_per_path <= 4096
        && c->max_waiting_streams && c->max_waiting_streams <= 16
        && c->schedule_scan_budget > c->max_waiting_streams && c->schedule_scan_budget <= 65536
        && memchr(c->output_prefix, 0, sizeof(c->output_prefix)) ? 0 : -1;
}
const char *xqc_arrival_mode_name(unsigned n)
{ return n == 0 ? "off" : n == 1 ? "shadow" : n == 2 ? "enforce" : "invalid"; }
int xqc_arrival_config_option(xqc_arrival_config_t *c, const char *key, const char *s)
{
    if (!c || !key || !s) return -1;
    if (!strcmp(key,"mode") || !strcmp(key,"adaptive_arrival_mode")) {
        for (unsigned i=0;i<3;i++) if (!strcmp(s,xqc_arrival_mode_name(i))) { c->mode=i; return 0; }
        return -1;
    }
    if (!strcmp(key,"arrival_model_version")) return strcmp(s,XQC_ARRIVAL_MODEL) ? -1 : 0;
    if (!strcmp(key,"output_prefix")) {
        if (strlen(s)>=sizeof(c->output_prefix)) return -1;
        strcpy(c->output_prefix,s); return 0;
    }
    char *end; errno=0;
    if (!strcmp(key,"gain_margin_rtt_ratio")) {
        double v=strtod(s,&end);
        if (errno || end==s || *end || !isfinite(v) || v<0 || v>10) return -1;
        c->gain_margin_rtt_ratio=v; return 0;
    }
    unsigned long long v=strtoull(s,&end,10);
    if (errno || end==s || *end || *s=='-' || v>UINT32_MAX) return -1;
#define OPT(f) if (!strcmp(key,#f)) { c->f=(uint32_t)v; return 0; }
    OPT(predict_horizon_us) OPT(max_defer_episode_us) OPT(max_defer_stream_us)
    OPT(recheck_interval_us) OPT(gain_margin_floor_us) OPT(min_valid_rate_samples)
    OPT(rate_sample_capacity) OPT(ack_batch_capacity) OPT(min_ack_batches)
    OPT(max_sim_events_per_path) OPT(max_waiting_streams) OPT(schedule_scan_budget)
    OPT(prediction_log_limit_per_s)
#undef OPT
    return -1;
}
const char *xqc_arrival_invalid_name(unsigned r)
{
    static const char *names[]={"OK","INPUT","OVERFLOW","RATE_SAMPLES","RATE_STALE",
      "RATE_SPAN","CC_UNSUPPORTED","ACK_SAMPLES","ACK_JITTER","ACK_FORECAST_OVERDUE",
      "ACK_HORIZON_UNSUPPORTED","HORIZON","EVENT_LIMIT","PRIORITY_PENDING","SOCKET_BLOCKED"};
    return r<AR_REASON_COUNT ? names[r] : "UNKNOWN";
}
void xqc_arrival_history_rate(xqc_arrival_history_t *h, const xqc_arrival_config_t *c,
    uint64_t t, uint64_t r, uint64_t interval, int limited, unsigned phase)
{
    if (!r || !interval || limited) return;
    if (h->nr && h->rates[h->nr-1].seq==h->ack_seq) return;
    unsigned cap=c->rate_sample_capacity;
    if (h->nr==cap) { memmove(h->rates,h->rates+1,(cap-1)*sizeof(h->rates[0])); h->nr--; }
    h->rates[h->nr++]=(xqc_arrival_rate_t){t,r,interval,h->ack_seq,phase};
}
void xqc_arrival_history_ack(xqc_arrival_history_t *h, const xqc_arrival_config_t *c,
    uint64_t t, uint64_t freed)
{
    h->ack_seq++;
    if (!freed) return;
    if (h->na && h->acks[h->na-1].time_us==t) {
        uint64_t old=h->acks[h->na-1].bytes;
        h->acks[h->na-1].bytes=freed>UINT64_MAX-old ? UINT64_MAX : old+freed;
        h->acks[h->na-1].seq=h->ack_seq;
        return;
    }
    if (h->na && h->acks[h->na-1].time_us>t) { h->na=0; }
    unsigned cap=c->ack_batch_capacity;
    if (h->na==cap) { memmove(h->acks,h->acks+1,(cap-1)*sizeof(h->acks[0])); h->na--; }
    h->acks[h->na++]=(xqc_arrival_ack_t){t,freed,h->ack_seq};
}
static void sort(uint64_t *a, unsigned n)
{ for (unsigned i=1;i<n;i++) { uint64_t v=a[i]; unsigned j=i; while(j && a[j-1]>v) {a[j]=a[j-1];j--;} a[j]=v; } }
static uint64_t median(uint64_t *a,unsigned n)
{ sort(a,n); return n%2 ? a[n/2] : a[n/2-1]+(a[n/2]-a[n/2-1])/2; }
static int add(uint64_t a,uint64_t b,uint64_t *out)
{ if (b>UINT64_MAX-a) return 0; *out=a+b; return 1; }
static int muldiv(uint64_t a,uint64_t b,uint64_t d,int ceil_result,uint64_t *out)
{
    if (!d || (b && a>UINT64_MAX/b)) return 0;
    uint64_t v=a*b; *out=v/d+(ceil_result && v%d!=0); return 1;
}
/* Entire simulation uses values. No transport objects, timers or queue pointers. */
xqc_arrival_prediction_t xqc_arrival_predict(const xqc_arrival_config_t *c,
    const xqc_arrival_history_t *h,const xqc_arrival_snapshot_t *s)
{
    xqc_arrival_prediction_t p; memset(&p,0,sizeof(p)); p.snapshot=*s;
#define FAIL(r) do { p.reason=(r); return p; } while(0)
    if (h->nr > 16 || h->na > 16 || !c->min_valid_rate_samples || !c->min_ack_batches) FAIL(AR_INPUT);
    p.rate_count=h->nr; p.ack_count=h->na; p.network_us=s->srtt_us/2;
    if(h->nr) {
        if(s->now_us<h->rates[h->nr-1].time_us || h->rates[h->nr-1].time_us<h->rates[0].time_us) FAIL(AR_INPUT);
        p.rate_age=s->now_us-h->rates[h->nr-1].time_us;
        p.rate_span=h->rates[h->nr-1].time_us-h->rates[0].time_us;p.rate_metadata_known=1;
    }
    if(h->na && s->now_us>=h->acks[h->na-1].time_us) {
        p.ack_age=s->now_us-h->acks[h->na-1].time_us;p.ack_age_known=1;
    }
    if (!s->L || !s->C || !s->srtt_us || !s->packet_quantum) FAIL(AR_INPUT);
    if (s->socket_blocked) FAIL(AR_SOCKET_BLOCKED);
    if (s->priority_pending) FAIL(AR_PRIORITY);
    if (!s->bbr_full) FAIL(AR_CC_UNSUPPORTED);
    p.rate_count=h->nr; p.ack_count=h->na;
    if (h->nr<c->min_valid_rate_samples) FAIL(AR_RATE_SAMPLES);
    uint64_t stale=s->srtt_us>UINT64_MAX/3 ? UINT64_MAX : s->srtt_us*3;
    if (stale<200000) stale=200000;
    if (p.rate_age>stale) FAIL(AR_RATE_STALE);
    if (p.rate_span<s->srtt_us) FAIL(AR_RATE_SPAN);
    uint64_t a[16]; for(unsigned i=0;i<h->nr;i++) a[i]=h->rates[i].rate;
    p.r_hat=median(a,h->nr);
    if (!p.r_hat) FAIL(AR_INPUT);
    if (!muldiv(s->L,1000000,p.r_hat,1,&p.service_us)) FAIL(AR_OVERFLOW);
    p.service_known=1;
    p.network_us=s->srtt_us/2;
    uint64_t end,occupied;
    if (!add(s->now_us,c->predict_horizon_us,&end) || !add(s->I,s->Z,&occupied)) FAIL(AR_OVERFLOW);
    if (s->L>s->C) FAIL(AR_ACK_HORIZON_UNSUPPORTED);
    uint64_t t=s->now_us, I=s->I, Z=s->Z, reclaim=s->I, budget=s->budget;
    uint64_t pending=s->pending, last=s->last_sent_us, timer=s->pacing_expire_us;
    uint64_t next_ack=0; int admitted=occupied<=s->C-s->L;
    if (!admitted) {
        if (h->na<c->min_ack_batches) FAIL(AR_ACK_SAMPLES);
        unsigned n=0;
        for(unsigned i=1;i<h->na;i++) {
            if(h->acks[i].time_us<=h->acks[i-1].time_us) FAIL(AR_ACK_SAMPLES);
            a[n++]=h->acks[i].time_us-h->acks[i-1].time_us;
        }
        p.ack_gap=median(a,n);
        /* Nearest-rank Q90/Q10, including small histories (at least 3 gaps). */
        unsigned q90=(9*n+9)/10-1, q10=(n+9)/10-1;
        uint64_t spread=a[q90]-a[q10];
        if (spread>(p.ack_gap>2000?p.ack_gap:2000)) FAIL(AR_ACK_JITTER);
        for(unsigned i=0;i<h->na;i++) a[i]=h->acks[i].bytes;
        p.ack_bytes=median(a,h->na);
        if (!p.ack_gap || !p.ack_bytes || s->now_us<h->acks[h->na-1].time_us) FAIL(AR_ACK_SAMPLES);
        p.ack_age=s->now_us-h->acks[h->na-1].time_us;
        if (!add(h->acks[h->na-1].time_us,p.ack_gap,&next_ack)) FAIL(AR_OVERFLOW);
        if (next_ack<t) FAIL(AR_ACK_FORECAST_OVERDUE);
    }
    if (s->pacing_on && (!s->pacing_Bps || !s->max_burst || last>t)) FAIL(AR_INPUT);
    if (s->pacing_on && s->max_burst>UINT32_MAX) FAIL(AR_OVERFLOW);
    for (;;) {
        if (p.events>=c->max_sim_events_per_path) FAIL(AR_EVENT_LIMIT);
        p.events++;
        if (t>end) FAIL(AR_HORIZON);
        if (next_ack && next_ack<=t) {
            uint64_t freed=p.ack_bytes<reclaim?p.ack_bytes:reclaim;
            I-=freed; reclaim-=freed;
            if (!add(next_ack,p.ack_gap,&next_ack)) FAIL(AR_OVERFLOW);
        }
        if (timer && timer<=t) {
            uint64_t earned,calc,minimum;
            if (!muldiv(t-last,s->pacing_Bps,1000000,0,&earned)
                || !add(budget,earned,&calc) || !add(budget,pending,&minimum)) FAIL(AR_OVERFLOW);
            if (!last) calc=s->max_burst;
            if (calc>s->max_burst) calc=s->max_burst;
            budget=calc>minimum?calc:minimum; pending=0; last=t; timer=0;
        }
        if (!add(I,Z,&occupied)) FAIL(AR_OVERFLOW);
        if (!admitted && occupied<=s->C-s->L) admitted=1;
        uint64_t bytes=Z ? (Z<s->packet_quantum?Z:s->packet_quantum) : s->L;
        int window_ok=I<=s->C && bytes<=s->C-I;
        if ((Z || admitted) && window_ok) {
            if (s->pacing_on && !timer && budget<bytes) {
                uint64_t delay;
                if (!muldiv(bytes-budget,1000000,s->pacing_Bps,0,&delay)) FAIL(AR_OVERFLOW);
                if (delay<1000) delay=1000;
                if (!add(t,delay,&timer)) FAIL(AR_OVERFLOW);
                pending=bytes-budget;
            }
            if (!s->pacing_on || !timer) {
                if (!Z) {
                    p.W_us=t-s->now_us;
                    if (!add(p.W_us,p.service_us,&p.cost_us) || !add(p.cost_us,p.network_us,&p.cost_us)) FAIL(AR_OVERFLOW);
                    if (p.cost_us>INT64_MAX) FAIL(AR_OVERFLOW);
                    p.valid=1; return p;
                }
                if (s->pacing_on) {
                    uint64_t earned,calc;
                    if (!muldiv(t-last,s->pacing_Bps,1000000,0,&earned) || !add(budget,earned,&calc)) FAIL(AR_OVERFLOW);
                    if (!last || calc>s->max_burst) calc=s->max_burst;
                    budget=calc>bytes?calc-bytes:0; last=t;
                }
                I+=bytes; Z-=bytes; continue; /* I+Z conserved; no new ACKs invented. */
            }
        }
        if ((!admitted || !window_ok) && !reclaim) FAIL(AR_ACK_HORIZON_UNSUPPORTED);
        uint64_t next=UINT64_MAX;
        if (next_ack && reclaim && next_ack>t) next=next_ack;
        if (timer>t && timer<next) next=timer;
        if (next==UINT64_MAX) FAIL(AR_ACK_HORIZON_UNSUPPORTED);
        if (!p.next_event_us) p.next_event_us=next;
        t=next;
    }
#undef FAIL
}
int xqc_arrival_compare(const xqc_arrival_config_t *c,const xqc_arrival_prediction_t *p,
    const xqc_arrival_prediction_t *s,uint64_t episode,uint64_t stream,int64_t *gain,uint64_t *margin)
{
    *gain=INT64_MIN; *margin=c->gain_margin_floor_us;
    uint64_t r=p->snapshot.srtt_us>s->snapshot.srtt_us?p->snapshot.srtt_us:s->snapshot.srtt_us;
    double m=ceil(c->gain_margin_rtt_ratio*(double)r);
    if (m>=(double)INT64_MAX) { *margin=UINT64_MAX; return 0; }
    if (m>(double)*margin) *margin=(uint64_t)m;
    if (!p->valid || !s->valid || p->cost_us>INT64_MAX || s->cost_us>INT64_MAX) return 0;
    *gain=(int64_t)s->cost_us-(int64_t)p->cost_us;
    return p->W_us>0 && p->W_us<=episode && p->W_us<=stream
        && *gain>0 && (uint64_t)*gain>*margin;
}
