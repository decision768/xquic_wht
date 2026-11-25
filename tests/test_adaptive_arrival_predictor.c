#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "src/transport/scheduler/adaptive_arrival_predictor.h"

static xqc_arrival_config_t c;
static xqc_arrival_history_t h;
static xqc_arrival_snapshot_t s;
static void fixture(void)
{
    xqc_arrival_config_init(&c); memset(&h,0,sizeof(h)); memset(&s,0,sizeof(s));
    s.now_us=1000000;s.C=10000;s.I=10000;s.L=1000;s.srtt_us=20000;
    s.packet_quantum=1000;s.bbr_full=1;
    for(unsigned i=0;i<8;i++) {
        xqc_arrival_history_ack(&h,&c,970000+i*4000,2000);
        xqc_arrival_history_rate(&h,&c,970000+i*4000,100000,4000,0,2);
    }
}
int main(void)
{
    fixture();assert(xqc_arrival_config_validate(&c)==0);
    assert(xqc_arrival_config_option(&c,"mode","enforce")==0 && c.mode==2);
    assert(xqc_arrival_config_option(&c,"mode","yes")==-1);
    assert(xqc_arrival_config_option(&c,"rate_sample_capacity","-1")==-1);
    assert(xqc_arrival_config_option(&c,"gain_margin_rtt_ratio","nan")==-1);
    c.version=2;assert(xqc_arrival_config_validate(&c)==-1);fixture();
    xqc_arrival_history_t before=h;xqc_arrival_snapshot_t ss=s;
    xqc_arrival_prediction_t p=xqc_arrival_predict(&c,&h,&s);
    assert(p.valid && p.W_us==2000 && p.service_us==10000 && p.cost_us==22000);
    assert(!memcmp(&before,&h,sizeof(h)) && !memcmp(&ss,&s,sizeof(s)));
    xqc_arrival_prediction_t again=xqc_arrival_predict(&c,&h,&s);assert(!memcmp(&p,&again,sizeof(p)));
    puts("PASS T02/T27: readonly repeatable prediction");
    s.L=0;assert(xqc_arrival_predict(&c,&h,&s).reason==AR_INPUT);fixture();
    s.I=UINT64_MAX;s.Z=2;assert(xqc_arrival_predict(&c,&h,&s).reason==AR_OVERFLOW);fixture();
    for(unsigned i=0;i<h.nr;i++)h.rates[i].rate=0;
    assert(xqc_arrival_predict(&c,&h,&s).reason==AR_INPUT);fixture();
    s.L=UINT64_MAX/2;s.C=UINT64_MAX;assert(xqc_arrival_predict(&c,&h,&s).reason==AR_OVERFLOW);fixture();
    fixture();s.now_us=UINT64_MAX-20000;s.srtt_us=s.now_us;s.I=0;
    h.rates[0].time_us=0;h.rates[h.nr-1].time_us=s.now_us;
    for(unsigned i=0;i<h.nr;i++)h.rates[i].rate=1;
    assert(xqc_arrival_predict(&c,&h,&s).reason==AR_OVERFLOW);fixture();
    puts("PASS T03: checked arithmetic and zero inputs");
    /* No initial I means no snapshot ACKs may be invented for the queued Z. */
    s.I=0;s.Z=10000;h.na=0;
    assert(xqc_arrival_predict(&c,&h,&s).reason==AR_ACK_SAMPLES);
    fixture();s.I=1000;s.Z=10000;s.C=10000;
    assert(xqc_arrival_predict(&c,&h,&s).reason==AR_ACK_HORIZON_UNSUPPORTED);
    puts("PASS T04: draining Z does not free occupied window");
    fixture();s.I=8000;s.Z=2000;s.pacing_on=1;s.pacing_Bps=1000000;
    s.budget=1000;s.last_sent_us=s.now_us;s.max_burst=10000;
    p=xqc_arrival_predict(&c,&h,&s);
    assert(p.valid && p.W_us==3000); /* Z drains before first ACK; waits overlap. */
    puts("PASS T05: Z draining overlaps ACK wait");
    fixture();unsigned na=h.na;uint64_t seq=h.ack_seq;
    xqc_arrival_history_ack(&h,&c,999000,0);assert(h.na==na && h.ack_seq==seq+1);
    xqc_arrival_history_ack(&h,&c,999000,700);xqc_arrival_history_ack(&h,&c,999000,300);
    assert(h.na==na+1 && h.acks[h.na-1].bytes==1000);
    unsigned nr=h.nr;xqc_arrival_history_rate(&h,&c,999000,42,1000,1,2);assert(h.nr==nr);
    xqc_arrival_history_rate(&h,&c,999000,42,1000,0,2);
    xqc_arrival_history_rate(&h,&c,999000,42,1000,0,2);assert(h.nr==nr+1);
    puts("PASS T06: positive batches, same-time merge, one valid sample per ACK");
    fixture();s.now_us+=3000;assert(xqc_arrival_predict(&c,&h,&s).reason==AR_ACK_FORECAST_OVERDUE);
    fixture();h.nr=7;assert(xqc_arrival_predict(&c,&h,&s).reason==AR_RATE_SAMPLES);
    fixture();s.now_us+=300000;assert(xqc_arrival_predict(&c,&h,&s).reason==AR_RATE_STALE);
    fixture();s.bbr_full=0;assert(xqc_arrival_predict(&c,&h,&s).reason==AR_CC_UNSUPPORTED);
    fixture();c.max_sim_events_per_path=1;assert(xqc_arrival_predict(&c,&h,&s).reason==AR_EVENT_LIMIT);
    fixture();c.predict_horizon_us=c.max_defer_episode_us=1000;
    assert(xqc_arrival_predict(&c,&h,&s).reason==AR_HORIZON);
    fixture();h.acks[0].time_us=900000;h.acks[1].time_us=901000;
    h.acks[2].time_us=910000;h.acks[3].time_us=911000;
    h.acks[4].time_us=920000;h.acks[5].time_us=921000;
    h.acks[6].time_us=930000;h.acks[7].time_us=999000;
    assert(xqc_arrival_predict(&c,&h,&s).reason==AR_ACK_JITTER);
    puts("PASS T07: invalid data, overdue ACK and event bound");
    fixture();s.I=0;s.pacing_on=1;s.pacing_Bps=1000000;s.budget=5000;s.pending=1000;
    s.last_sent_us=s.now_us;s.pacing_expire_us=s.now_us+3000;s.max_burst=10000;
    ss=s;p=xqc_arrival_predict(&c,&h,&s);assert(p.valid && p.W_us==3000 && !memcmp(&ss,&s,sizeof(s)));
    s.pacing_on=0;p=xqc_arrival_predict(&c,&h,&s);assert(p.valid && p.W_us==0);
    s.pacing_on=1;s.pacing_expire_us=0;s.budget=0;s.pacing_Bps=10000000;
    p=xqc_arrival_predict(&c,&h,&s);assert(p.valid && p.W_us==1000);
    puts("PASS T08: timer precedence, pending budget, disabled pacing and 1 ms minimum");
    fixture();p=xqc_arrival_predict(&c,&h,&s);p.W_us=3000;p.cost_us=23000;
    xqc_arrival_prediction_t q=p;q.cost_us=71000;q.snapshot.srtt_us=120000;
    int64_t gain;uint64_t margin;
    assert(xqc_arrival_compare(&c,&p,&q,5000,20000,&gain,&margin) && gain==48000 && margin==12000);
    p.cost_us=90000;q.cost_us=30000;
    assert(!xqc_arrival_compare(&c,&p,&q,5000,20000,&gain,&margin) && gain==-60000);
    p.cost_us=30000;q.cost_us=42000;assert(!xqc_arrival_compare(&c,&p,&q,5000,20000,&gain,&margin));
    q.cost_us++;assert(xqc_arrival_compare(&c,&p,&q,5000,20000,&gain,&margin));
    assert(!xqc_arrival_compare(&c,&p,&q,2999,20000,&gain,&margin));
    assert(!xqc_arrival_compare(&c,&p,&q,5000,2999,&gain,&margin));
    p.snapshot.srtt_us=UINT64_MAX;c.gain_margin_rtt_ratio=10;
    assert(!xqc_arrival_compare(&c,&p,&q,5000,20000,&gain,&margin));
    assert(gain==INT64_MIN && margin==UINT64_MAX);
    puts("PASS T09/T10/T11/T18: strict signed gain and remaining budgets");
    fixture();s.I=0;s.Z=60000;s.C=100000;s.packet_quantum=1;
    assert(xqc_arrival_predict(&c,&h,&s).reason==AR_EVENT_LIMIT);
    puts("PASS T27: bounded queue simulation");
    return 0;
}
