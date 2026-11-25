#include "xqc_scheduler_adaptive_v2.h"
#include "xqc_scheduler_adaptive.h"
#include "xqc_scheduler_common.h"
#include "src/congestion_control/xqc_bbr_common.h"
#include "src/transport/xqc_conn.h"
#include "src/transport/xqc_engine.h"
#include "src/transport/xqc_stream.h"
#include "src/transport/xqc_send_ctl.h"
#include "src/common/xqc_malloc.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

typedef struct {
    uint64_t stream_id, packet_uid, first, accounted, deadline, wake, epoch, episode, checks;
    xqc_packet_out_t *packet; /* guarded by UID and packet_gone before recycling */
    uint64_t offset;
    unsigned length, packet_size;
    int used, begun, legacy;
    uint64_t end_us;
} arrival_wait_t;
typedef struct {
    uint64_t path_id;
    xqc_arrival_history_t history;
    int used, socket_blocked;
    uint64_t socket_attempt;
} arrival_path_t;
struct xqc_arrival_s {
    xqc_arrival_config_t config;
    arrival_path_t paths[XQC_MAX_PATHS_COUNT];
    arrival_wait_t waits[16];
    xqc_gp_timer_id_t timer;
    uint64_t conn_uid, primary, secondary, epoch, decision, episode, hint_uid;
    uint64_t continuation, timer_due, log_second;
    unsigned log_count, waiting, peak, round;
    xqc_list_head_t *cursor_head;
    xqc_packet_out_t *cursor;
    uint64_t cursor_uid;
    xqc_packet_out_t *resume, *urgent, *urgent_resume;
    uint64_t resume_uid;
    unsigned priority_mask;
    int priority_phase, urgent_phase, restart_scan;
    xqc_arrival_sink_pt sink;
    void *sink_data;
    uint64_t eligible, pairs, invalid[AR_REASON_COUNT], would_wait, shadow_wait, episodes;
    uint64_t wait_us, max_wait, recovered, legacy, exhausted, invalidated, skips;
    uint64_t progress, scan_hits, timer_wakes, ack_wakes, lateness, calls, events, cpu_us;
    uint64_t sampled_out, dropped, lifecycle_dropped;
};
static int enforcing(xqc_connection_t *c)
{ return c->arrival && c->arrival->config.mode==XQC_ARRIVAL_ENFORCE; }
static void emit(xqc_arrival_t *a,const char *s,int lifecycle)
{
    if (!a->sink || a->sink(a->sink_data,0,s)) {
        a->dropped++; if(lifecycle) a->lifecycle_dropped++;
    }
}
static void config_event(xqc_arrival_t *a)
{
    char b[4096]; const xqc_arrival_config_t *c=&a->config;
    snprintf(b,sizeof(b),"{\"family\":\"adaptive_arrival_events\",\"schema_version\":1,"
      "\"event\":\"config\",\"conn_uid\":%"PRIu64",\"scheduler_name\":\"adaptive-v2\","
      "\"endpoint\":\"server\",\"mode\":\"%s\",\"model_version\":\"%s\","
      "\"queue_model\":\"grouped_bytes_v1\",\"source_build_id\":\"%s\","
      "\"margin_is_statistical_confidence\":false,\"predict_horizon_us\":%u,"
      "\"max_defer_episode_us\":%u,\"max_defer_stream_us\":%u,\"recheck_interval_us\":%u,"
      "\"gain_margin_floor_us\":%u,\"gain_margin_rtt_ratio\":%.9g,"
      "\"min_valid_rate_samples\":%u,\"rate_sample_capacity\":%u,\"ack_batch_capacity\":%u,"
      "\"min_ack_batches\":%u,\"max_sim_events_per_path\":%u,\"max_waiting_streams\":%u,"
      "\"schedule_scan_budget\":%u,\"prediction_log_limit_per_s\":%u}",
      a->conn_uid,xqc_arrival_mode_name(c->mode),XQC_ARRIVAL_MODEL,xqc_arrival_source_build_id(),
      c->predict_horizon_us,c->max_defer_episode_us,c->max_defer_stream_us,c->recheck_interval_us,
      c->gain_margin_floor_us,c->gain_margin_rtt_ratio,c->min_valid_rate_samples,
      c->rate_sample_capacity,c->ack_batch_capacity,c->min_ack_batches,c->max_sim_events_per_path,
      c->max_waiting_streams,c->schedule_scan_budget,c->prediction_log_limit_per_s);
    emit(a,b,1);
}
static xqc_stream_t *getstream(xqc_connection_t *c,uint64_t id)
{ return xqc_find_stream_by_id(id,c->streams_hash); }
static arrival_wait_t *waitfor(xqc_arrival_t *a,uint64_t id)
{ for(unsigned i=0;i<16;i++) if(a->waits[i].used && a->waits[i].stream_id==id) return &a->waits[i]; return NULL; }
static void account(xqc_connection_t *c,arrival_wait_t *w,uint64_t now)
{
    if(now<w->accounted) return;
    uint64_t dt=now-w->accounted; w->accounted=now;
    xqc_stream_t *s=getstream(c,w->stream_id);
    if(s && s->arrival) s->arrival->spent_us+=dt;
    c->arrival->wait_us+=dt;
}
static void wait_event(xqc_connection_t *c,arrival_wait_t *w,const char *event,
    const char *reason,uint64_t now,uint64_t path,int queued)
{
    xqc_arrival_t *a=c->arrival; xqc_stream_t *s=getstream(c,w->stream_id);
    char pathstr[32],b[2200];
    if(path==UINT64_MAX) strcpy(pathstr,"null"); else snprintf(pathstr,sizeof(pathstr),"%"PRIu64,path);
    snprintf(b,sizeof(b),"{\"family\":\"adaptive_arrival_events\",\"schema_version\":1,"
      "\"event\":\"%s\",\"conn_uid\":%"PRIu64",\"transfer_id\":\"%s\","
      "\"stream_id\":%"PRIu64",\"offset\":%"PRIu64",\"length\":%u,\"request_index\":%u,\"packet_uid\":%"PRIu64",\"episode_id\":%"PRIu64","
      "\"actual_mono_us\":%"PRIu64",\"first_defer_us\":%"PRIu64",\"deadline_us\":%"PRIu64","
      "\"wake_at_us\":%"PRIu64",\"role_epoch\":%"PRIu64",\"rechecks\":%"PRIu64","
      "\"actual_wait_us\":%"PRIu64",\"stream_wait_us\":%"PRIu64",\"reason\":\"%s\","
      "\"final_path_id\":%s,\"queued\":%s}",event,a->conn_uid,
      s&&s->arrival?s->arrival->transfer_id:"",w->stream_id,w->offset,w->length,s&&s->arrival?s->arrival->request_index:0,w->packet_uid,w->episode,now,w->first,
      w->deadline,w->wake,w->epoch,w->checks,(w->end_us?w->end_us:now)-w->first,s&&s->arrival?s->arrival->spent_us:0,
      reason,pathstr,queued?"true":"false"); emit(a,b,1);
}
static void finish(xqc_connection_t *c,arrival_wait_t *w,const char *why,uint64_t now,
    uint64_t path,int queued,int cancel)
{
    xqc_arrival_t *a=c->arrival;
    if(w->legacy) {
        if(queued)wait_event(c,w,"legacy_queued",why,now,path,1);
        w->used=0;w->packet=NULL;
        if(queued)a->restart_scan=1;
        return;
    }
    account(c,w,now);w->end_us=now;
    uint64_t elapsed=now-w->first;
    if(elapsed>a->max_wait)a->max_wait=elapsed;
    if(w->packet && w->packet->po_arrival_uid==w->packet_uid)w->packet->po_arrival_legacy=1;
    wait_event(c,w,!w->begun?"wait_rejected":cancel?"wait_cancel":"wait_end",why,now,path,queued);
    a->waiting--;
    if(!cancel && !queued) {
        /* Keep same-stream order until the required original attempt is queued.
         * This guard consumes no further active-wait time and has no wait timer. */
        w->legacy=1;w->wake=now;
    } else {w->used=0;w->packet=NULL;}
    if(queued)a->restart_scan=1;
}
static void timeout(xqc_gp_timer_id_t id,xqc_usec_t now,void *data)
{
    xqc_connection_t *c=data; xqc_arrival_t *a=c->arrival; (void)id;
    if(!a) return;
    a->timer_wakes++; if(now>a->timer_due)a->lateness+=now-a->timer_due;
    a->timer_due=0; /* engine timer expiry already makes this connection runnable */
}
static int arm(xqc_connection_t *c,uint64_t now)
{
    xqc_arrival_t *a=c->arrival;
    if(!enforcing(c))return 0;
    uint64_t due=a->continuation;
    for(unsigned i=0;i<16;i++) if(a->waits[i].used && !a->waits[i].legacy && (!due || a->waits[i].wake<due)) due=a->waits[i].wake;
    if(!due) { if(a->timer>=0)xqc_timer_gp_timer_unset(&c->conn_timer_manager,a->timer); a->timer_due=0; return 0; }
    if(a->timer<0)a->timer=xqc_timer_register_gp_timer(&c->conn_timer_manager,"adaptive_arrival_wait",timeout,c);
    if(a->timer<0)return -1;
    if(due<=now)due=now+1;
    a->timer_due=due;
    return xqc_timer_gp_timer_set(&c->conn_timer_manager,a->timer,due);
}
void xqc_arrival_conn_init(xqc_connection_t *c)
{
    if(c->scheduler_callback->xqc_scheduler_get_path!=xqc_adaptive_v2_scheduler_cb.xqc_scheduler_get_path) return;
    xqc_arrival_t *a=xqc_calloc(1,sizeof(*a)); if(!a)return;
    a->timer=-1; a->primary=a->secondary=UINT64_MAX;
    if(xqc_arrival_config_validate(&c->engine->arrival_config)==0) a->config=c->engine->arrival_config;
    else xqc_arrival_config_init(&a->config);
    /* No global counter or wall clock; connection-local monotonic identity plus SCID hash. */
    a->conn_uid=c->conn_create_time;
    for(unsigned i=0;i<c->scid_set.user_scid.cid_len;i++) a->conn_uid=(a->conn_uid^c->scid_set.user_scid.cid_buf[i])*1099511628211ULL;
    c->arrival=a;
}
static void summary(xqc_arrival_t *a)
{
    char b[3000],inv[800]; size_t n=0;
    for(unsigned i=1;i<AR_REASON_COUNT;i++) n+=(size_t)snprintf(inv+n,sizeof(inv)-n,"%s\"%s\":%"PRIu64,i==1?"":",",xqc_arrival_invalid_name(i),a->invalid[i]);
    snprintf(b,sizeof(b),"{\"family\":\"adaptive_arrival_events\",\"schema_version\":1,\"event\":\"summary\","
      "\"conn_uid\":%"PRIu64",\"eligible_fallback_calls\":%"PRIu64",\"predict_valid_pairs\":%"PRIu64","
      "\"invalid_reasons\":{%s},\"would_wait_calls\":%"PRIu64",\"shadow_would_wait\":%"PRIu64",\"actual_wait_episodes\":%"PRIu64","
      "\"wait_actual_us\":%"PRIu64",\"wait_max_us\":%"PRIu64",\"primary_recovered_send\":%"PRIu64","
      "\"secondary_legacy_send\":%"PRIu64",\"wait_budget_exhausted\":%"PRIu64",\"wait_invalidated\":%"PRIu64","
      "\"deferred_stream_count_peak\":%u,\"same_stream_skips\":%"PRIu64",\"other_stream_progress_while_waiting\":%"PRIu64","
      "\"scan_budget_hits\":%"PRIu64",\"timer_wakes\":%"PRIu64",\"ACK_early_wakes\":%"PRIu64","
      "\"timer_lateness_us\":%"PRIu64",\"prediction_calls\":%"PRIu64",\"sim_events\":%"PRIu64","
      "\"prediction_cpu_us\":%"PRIu64",\"sampled_out\":%"PRIu64",\"dropped_records\":%"PRIu64","
      "\"lifecycle_dropped\":%"PRIu64",\"send_counter_stage\":\"path_queue_handoff\",\"mechanism_complete\":%s}",a->conn_uid,a->eligible,a->pairs,inv,a->would_wait,a->shadow_wait,
      a->episodes,a->wait_us,a->max_wait,a->recovered,a->legacy,a->exhausted,a->invalidated,a->peak,a->skips,
      a->progress,a->scan_hits,a->timer_wakes,a->ack_wakes,a->lateness,a->calls,a->events,a->cpu_us,
      a->sampled_out,a->dropped,a->lifecycle_dropped,a->lifecycle_dropped?"false":"true"); emit(a,b,1);
}
void xqc_arrival_conn_close(xqc_connection_t *c)
{
    xqc_arrival_t *a=c->arrival; if(!a)return;
    uint64_t now=xqc_monotonic_timestamp();
    for(unsigned i=0;i<16;i++)if(a->waits[i].used)finish(c,&a->waits[i],"connection_close",now,UINT64_MAX,0,1);
    summary(a);
    if(a->timer>=0)xqc_timer_unregister_gp_timer(&c->conn_timer_manager,a->timer);
    xqc_free(a); c->arrival=NULL;
}
void xqc_arrival_stream_close(xqc_connection_t *c,uint64_t id)
{
    if(!c->arrival)return;
    xqc_stream_t *stream=getstream(c,id);if(stream && stream->arrival)stream->arrival->cancelled=1;
    arrival_wait_t *w=waitfor(c->arrival,id);
    if(w)finish(c,w,"stream_close",xqc_monotonic_timestamp(),UINT64_MAX,0,1);
    arm(c,xqc_monotonic_timestamp());
}
void xqc_arrival_packet_gone(xqc_connection_t *c,xqc_packet_out_t *p)
{
    xqc_arrival_t *a=c->arrival; if(!a)return;
    if(a->cursor==p) {a->cursor=NULL;a->cursor_uid=0;}
    if(a->resume==p)a->resume=NULL;
    if(a->urgent==p)a->urgent=NULL;
    if(a->urgent_resume==p)a->urgent_resume=NULL;
    for(unsigned i=0;i<16;i++)if(a->waits[i].used && a->waits[i].packet_uid==p->po_arrival_uid)
        finish(c,&a->waits[i],"packet_removed",xqc_monotonic_timestamp(),UINT64_MAX,0,1);
    arm(c,xqc_monotonic_timestamp());
}
void xqc_arrival_priority_promote(xqc_connection_t *c,xqc_packet_out_t *p,xqc_list_head_t *high)
{
    xqc_arrival_t *a=c->arrival;if(!a)return;
    for(unsigned i=0;i<XQC_MAX_STREAM_FRAME_IN_PO;i++)if(p->po_stream_frames[i].ps_is_used) {
        arrival_wait_t *w=waitfor(a,p->po_stream_frames[i].ps_stream_id);if(!w)continue;
        xqc_packet_out_t *first=w->packet;
        uint64_t uid=w->packet_uid;
        finish(c,w,"priority_promoted",xqc_monotonic_timestamp(),UINT64_MAX,0,1);
        if(first && first!=p && first->po_arrival_uid==uid) {
            xqc_arrival_packet_gone(c,first);
            xqc_list_del_init(&first->po_list);xqc_list_add_tail(&first->po_list,high);
        }
    }
    xqc_arrival_packet_gone(c,p);arm(c,xqc_monotonic_timestamp());
}
static xqc_arrival_history_t *history(xqc_arrival_t *a,uint64_t id)
{
    arrival_path_t *empty=NULL;
    for(unsigned i=0;i<XQC_MAX_PATHS_COUNT;i++) { if(a->paths[i].used && a->paths[i].path_id==id)return &a->paths[i].history; if(!a->paths[i].used)empty=&a->paths[i]; }
    if(!empty)return NULL; empty->used=1; empty->path_id=id; return &empty->history;
}
void xqc_arrival_path_reset(xqc_connection_t *c,uint64_t id)
{
    xqc_arrival_t *a=c->arrival; if(!a)return;
    for(unsigned i=0;i<XQC_MAX_PATHS_COUNT;i++)if(a->paths[i].used && a->paths[i].path_id==id)memset(&a->paths[i],0,sizeof(a->paths[i]));
    for(unsigned i=0;i<16;i++)if(a->waits[i].used)finish(c,&a->waits[i],"path_reset",xqc_monotonic_timestamp(),UINT64_MAX,0,0);
    arm(c,xqc_monotonic_timestamp());
}
static int wake_wait_heads(xqc_send_ctl_t *ctl,uint64_t now)
{
    xqc_connection_t *c=ctl->ctl_conn;xqc_arrival_t *a=c->arrival;
    if(!enforcing(c) || !a->waiting || (ctl->ctl_path->path_id!=a->primary && ctl->ctl_path->path_id!=a->secondary))return 0;
    for(unsigned i=0;i<16;i++)if(a->waits[i].used && a->waits[i].wake>now)a->waits[i].wake=now;
    arm(c,now);return 1;
}
void xqc_arrival_pacing_wake(xqc_send_ctl_t *ctl,uint64_t now) {wake_wait_heads(ctl,now);}
void xqc_arrival_ack(xqc_send_ctl_t *ctl,uint64_t now,uint64_t freed)
{
    xqc_arrival_t *a=ctl->ctl_conn->arrival; if(!a || !a->config.mode)return;
    xqc_arrival_history_t *h=history(a,ctl->ctl_path->path_id);
    if(h)xqc_arrival_history_ack(h,&a->config,now,freed);
    if(freed && wake_wait_heads(ctl,now))a->ack_wakes++;
}
void xqc_arrival_rate(xqc_send_ctl_t *ctl,uint64_t now,int valid)
{
    xqc_arrival_t *a=ctl->ctl_conn->arrival; if(!a || !a->config.mode || !valid)return;
    xqc_arrival_history_t *h=history(a,ctl->ctl_path->path_id);
    xqc_bbr_info_interface_t *info=ctl->ctl_cong_callback->xqc_cong_ctl_info_cb;
    if(h)xqc_arrival_history_rate(h,&a->config,now,ctl->sampler.delivery_rate,ctl->sampler.interval,
        ctl->sampler.is_app_limited,info&&info->mode?info->mode(ctl->ctl_cong):0);
}
void xqc_arrival_frame_written(xqc_stream_t *stream,xqc_packet_out_t *packet_out,size_t written)
{
    /* Merged STREAM frames remain body-only only if all appended bytes are body. */
    for (unsigned arrival_i = 0; arrival_i < XQC_MAX_STREAM_FRAME_IN_PO; ++arrival_i) {
        xqc_po_stream_frame_t *f = &packet_out->po_stream_frames[arrival_i];
        if (f->ps_is_used && f->ps_stream_id == stream->stream_id
            && f->ps_offset + f->ps_length == stream->stream_send_offset + written)
        {
            int body = stream->arrival && stream->arrival_writing_body && written;
            f->ps_arrival_body = f->ps_offset < stream->stream_send_offset
                ? (f->ps_arrival_body && body) : body;
        }
    }
}
static int pure(xqc_connection_t *c,xqc_packet_out_t *p,xqc_stream_t **out)
{
    if(p->po_pkt.pkt_type!=XQC_PTYPE_SHORT_HEADER || p->po_origin || p->po_path_flag
        || (p->po_flag & (XQC_POF_LOST|XQC_POF_TLP|XQC_POF_RETRANSED|XQC_POF_RESEND
            |XQC_POF_REINJECTED_ORIGIN|XQC_POF_REINJECTED_REPLICA|XQC_POF_REINJECT_DIFF_PATH
            |XQC_POF_PMTUD_PROBING|XQC_POF_QOS_PROBING|XQC_POF_USE_FEC))
        || (p->po_frame_types & ~(XQC_FRAME_BIT_STREAM|XQC_FRAME_BIT_PADDING))
        || !(p->po_frame_types & XQC_FRAME_BIT_STREAM))return 0;
    xqc_po_stream_frame_t *f=&p->po_stream_frames[0];
    if(!f->ps_is_used || !f->ps_length || f->ps_is_reset || !f->ps_arrival_body || p->po_stream_frames[1].ps_is_used)return 0;
    xqc_stream_t *s=getstream(c,f->ps_stream_id); if(!s || !s->arrival || s->arrival->cancelled || s->stream_priority==XQC_STREAM_PRI_HIGH)return 0;
    if(out)*out=s; return 1;
}
static xqc_arrival_prediction_t predict(xqc_connection_t *c,xqc_path_ctx_t *path,xqc_packet_out_t *p,uint64_t now)
{
    xqc_send_ctl_t *ctl=path->path_send_ctl; xqc_sched_path_eval_t e;
    memset(&e,0,sizeof(e));
    e.effective_cwnd=xqc_send_ctl_calc_effective_cwnd(xqc_scheduler_get_cwnd(path),
        c->conn_settings.so_sndbuf>0 ? (uint64_t)c->conn_settings.so_sndbuf : 0);
    e.bytes_in_flight=ctl->ctl_bytes_in_flight;e.path_schedule_bytes=path->path_schedule_bytes;
    xqc_pacing_t *pace=&ctl->ctl_pacing;
    xqc_bbr_info_interface_t *info=ctl->ctl_cong_callback->xqc_cong_ctl_info_cb;
    xqc_arrival_snapshot_t s={0};
    s.path_id=path->path_id;s.now_us=now;s.C=e.effective_cwnd;s.I=e.bytes_in_flight;s.Z=e.path_schedule_bytes;s.L=p->po_used_size;
    s.srtt_us=ctl->ctl_first_rtt_sample_time?xqc_send_ctl_get_srtt(ctl):0;
    s.raw_bw=xqc_send_ctl_get_est_bw(ctl);s.pacing_Bps=xqc_send_ctl_get_pacing_rate(ctl);
    s.budget=pace->bytes_budget;s.pending=pace->pending_budget;s.last_sent_us=pace->last_sent_time;
    s.pacing_on=pace->pacing_on;
    if(xqc_timer_is_set(&ctl->path_timer_manager,XQC_TIMER_PACING))s.pacing_expire_us=ctl->path_timer_manager.timer[XQC_TIMER_PACING].expire_time;
    s.max_burst=s.pacing_Bps<=UINT64_MAX/2000?s.pacing_Bps*2000/1000000:UINT64_MAX;
    if(s.max_burst<10*XQC_MSS)s.max_burst=10*XQC_MSS;
    s.packet_quantum=c->pkt_out_size;
    s.bbr_full=xqc_bbr_cb.xqc_cong_ctl_init_bbr
        && ctl->ctl_cong_callback->xqc_cong_ctl_init_bbr==xqc_bbr_cb.xqc_cong_ctl_init_bbr
        && info && info->full_bw_reached && info->full_bw_reached(ctl->ctl_cong);
    if(info && info->recovery_mode && info->recovery_mode(ctl->ctl_cong))s.priority_pending=1;
    if(c->pkt_filter_cb || (path->path_flag & XQC_PATH_FLAG_SOCKET_ERROR)
        || xqc_timer_is_set(&ctl->path_timer_manager,XQC_TIMER_NAT_REBINDING))s.priority_pending=1;
    for(unsigned i=0;i<XQC_MAX_PATHS_COUNT;i++)if(c->arrival->paths[i].used && c->arrival->paths[i].path_id==path->path_id)
        s.socket_blocked=c->arrival->paths[i].socket_blocked;
    for(unsigned i=XQC_SEND_TYPE_NORMAL+1;i<XQC_SEND_TYPE_N;i++)if(!xqc_list_empty(&path->path_schedule_buf[i]))s.priority_pending=1;
    if(!xqc_list_empty(&c->conn_send_queue->sndq_lost_packets) || !xqc_list_empty(&c->conn_send_queue->sndq_pto_probe_packets))s.priority_pending=1;
    xqc_arrival_history_t empty={0},*h=history(c->arrival,path->path_id);
    return xqc_arrival_predict(&c->arrival->config,h?h:&empty,&s);
}
static const char *number(char *b,size_t size,uint64_t value,int known)
{
    if(known)snprintf(b,size,"%"PRIu64,value);else snprintf(b,size,"null");return b;
}
static void prediction_json(char *b,size_t len,const xqc_arrival_prediction_t *p)
{
    const xqc_arrival_snapshot_t *s=&p->snapshot;
    char n[12][32];
    int rate_known=p->r_hat>0,ack_known=p->ack_gap>0;
    uint64_t pacing_remaining=s->pacing_expire_us>s->now_us?s->pacing_expire_us-s->now_us:0;
    snprintf(b,len,"{\"path_id\":%"PRIu64",\"snapshot_us\":%"PRIu64",\"C\":%"PRIu64",\"I\":%"PRIu64",\"Z\":%"PRIu64",\"L\":%"PRIu64","
      "\"raw_bw_Bps\":%"PRIu64",\"r_hat_Bps\":%s,\"pacing_Bps\":%"PRIu64",\"srtt_us\":%s,"
      "\"budget\":%"PRIu64",\"pending_budget\":%"PRIu64",\"last_sent_us\":%"PRIu64",\"pacing_expire_us\":%"PRIu64","
      "\"pacing_on\":%s,\"pacing_timer_remaining_us\":%"PRIu64",\"max_burst\":%"PRIu64",\"packet_quantum\":%u,"
      "\"block_reason\":%d,\"rate_count\":%u,\"rate_age_us\":%s,\"rate_span_us\":%s,\"full_bw\":%s,"
      "\"ack_count\":%u,\"last_ACK_age_us\":%s,\"g_us\":%s,\"b_B\":%s,"
      "\"valid\":%s,\"invalid_reason\":\"%s\",\"W_hat_us\":%s,\"packet_service_us\":%s,"
      "\"network_delay_proxy_us\":%s,\"arrival_cost_us\":%s,\"next_useful_event_us\":%s,\"simulated_events\":%u}",
      s->path_id,s->now_us,s->C,s->I,s->Z,s->L,s->raw_bw,
      number(n[0],32,p->r_hat,rate_known),s->pacing_Bps,number(n[1],32,s->srtt_us,s->srtt_us>0),
      s->budget,s->pending,s->last_sent_us,s->pacing_expire_us,s->pacing_on?"true":"false",pacing_remaining,
      s->max_burst,s->packet_quantum,xqc_scheduler_classify_cwnd_block(s->C,s->I,s->Z,(uint32_t)s->L,1),
      p->rate_count,number(n[2],32,p->rate_age,p->rate_metadata_known),number(n[3],32,p->rate_span,p->rate_metadata_known),
      s->bbr_full?"true":"false",p->ack_count,number(n[4],32,p->ack_age,p->ack_age_known),
      number(n[5],32,p->ack_gap,ack_known),number(n[6],32,p->ack_bytes,p->ack_bytes>0),p->valid?"true":"false",
      xqc_arrival_invalid_name(p->reason),number(n[7],32,p->W_us,p->valid),
      number(n[8],32,p->service_us,p->service_known),number(n[9],32,p->network_us,s->srtt_us>0),
      number(n[10],32,p->cost_us,p->valid),number(n[11],32,p->next_event_us,p->next_event_us>0),p->events);
}
void xqc_arrival_roles(xqc_connection_t *c,uint64_t primary,uint64_t secondary,int valid,uint64_t now)
{
    xqc_arrival_t *a=c->arrival;if(!a || !a->config.mode)return;
    if(!valid)primary=secondary=UINT64_MAX;
    if(a->primary==primary && a->secondary==secondary)return;
    a->primary=primary;a->secondary=secondary;a->epoch++;
    for(unsigned i=0;i<16;i++)if(a->waits[i].used && !a->waits[i].legacy) {
        a->invalidated++;finish(c,&a->waits[i],"role_or_path_changed",now,UINT64_MAX,0,0);
    }
    arm(c,now);
}
int xqc_arrival_set_mode(xqc_engine_t *e,const xqc_cid_t *cid,unsigned mode)
{
    xqc_connection_t *c=e&&cid?xqc_engine_conns_hash_find(e,cid,'s'):NULL;
    if(!c || !c->arrival || mode>XQC_ARRIVAL_ENFORCE)return -1;
    xqc_arrival_t *a=c->arrival;
    if(mode!=a->config.mode) {
        uint64_t now=xqc_monotonic_timestamp();
        for(unsigned i=0;i<16;i++)if(a->waits[i].used)finish(c,&a->waits[i],"mode_changed",now,UINT64_MAX,0,1);
        a->continuation=0;a->cursor=a->resume=a->urgent=a->urgent_resume=NULL;
        a->priority_phase=a->urgent_phase=a->restart_scan=0;a->hint_uid=0;
        if(a->timer>=0)xqc_timer_gp_timer_unset(&c->conn_timer_manager,a->timer);
        a->timer_due=0;a->config.mode=mode;config_event(a);
    }
    return 0;
}
void xqc_arrival_clear_hint(xqc_connection_t *c)
{ if(c->arrival)c->arrival->hint_uid=0; }
int xqc_arrival_is_defer(xqc_connection_t *c,xqc_packet_out_t *p)
{ return c->arrival && c->arrival->hint_uid && c->arrival->hint_uid==p->po_arrival_uid; }
int xqc_arrival_fallback(xqc_connection_t *c,xqc_packet_out_t *p,xqc_path_ctx_t *P,
    xqc_path_ctx_t *S,size_t count,int reinject,uint64_t now)
{
    xqc_arrival_t *a=c->arrival; xqc_stream_t *stream;
    if(!a || !a->config.mode || count!=2 || reinject || !p->po_arrival_uid || p->po_arrival_legacy || !pure(c,p,&stream))return 0;
    if(c->engine->eng_type!=XQC_ENGINE_SERVER || c->conn_settings.enable_encode_fec)return 0;
    if(!xqc_list_empty(&c->conn_send_queue->sndq_send_packets_high_pri))return 0;
    a->eligible++;p->po_arrival_eligible=1;p->po_arrival_candidate_secondary=S->path_id;
    if(a->primary!=P->path_id || a->secondary!=S->path_id) {a->primary=P->path_id;a->secondary=S->path_id;a->epoch++;}
    arrival_wait_t *w=waitfor(a,stream->stream_id);
    if(w && w->legacy)return 0;
    if(w) {
        account(c,w,now);w->checks++;
        if(w->epoch!=a->epoch || now>=w->deadline || stream->arrival->spent_us>=a->config.max_defer_stream_us) {
            a->exhausted++;finish(c,w,"deadline_budget_or_role",now,UINT64_MAX,0,0);arm(c,now);return 0;
        }
    }
    uint64_t before=xqc_monotonic_timestamp();
    xqc_arrival_prediction_t pp=predict(c,P,p,now),sp=predict(c,S,p,now);
    a->calls+=2;a->events+=pp.events+sp.events;a->cpu_us+=xqc_monotonic_timestamp()-before;
    if(pp.valid && sp.valid)a->pairs++;
    if(!pp.valid)a->invalid[pp.reason]++;if(!sp.valid)a->invalid[sp.reason]++;
    uint64_t left=stream->arrival->spent_us>=a->config.max_defer_stream_us?0:a->config.max_defer_stream_us-stream->arrival->spent_us;
    uint64_t episode=w?w->deadline-now:a->config.max_defer_episode_us,margin;int64_t gain;
    int would=xqc_arrival_compare(&a->config,&pp,&sp,episode,left,&gain,&margin);
    if(would){a->would_wait++;if(a->config.mode==XQC_ARRIVAL_SHADOW)a->shadow_wait++;}
    int defer=would && a->config.mode==XQC_ARRIVAL_ENFORCE;
    if(defer && !w) {
        if(a->waiting>=a->config.max_waiting_streams)defer=0;
        else for(unsigned i=0;i<16;i++)if(!a->waits[i].used){w=&a->waits[i];memset(w,0,sizeof(*w));w->used=1;w->stream_id=stream->stream_id;
            w->packet=p;w->packet_uid=p->po_arrival_uid;w->first=w->accounted=now;w->deadline=now+a->config.max_defer_episode_us;
            w->offset=p->po_stream_frames[0].ps_offset;w->length=p->po_stream_frames[0].ps_length;w->packet_size=p->po_used_size;
            w->epoch=a->epoch;w->episode=++a->episode;a->waiting++;if(a->waiting>a->peak)a->peak=a->waiting;break;}
        if(!w)defer=0;
    }
    if(defer) {
        w->wake=now+a->config.recheck_interval_us;
        if(pp.next_event_us>now && pp.next_event_us<w->wake)w->wake=pp.next_event_us;
        if(w->deadline<w->wake)w->wake=w->deadline;
        if(arm(c,now)<0) {finish(c,w,"timer_failure",now,UINT64_MAX,0,0);w=NULL;defer=0;}
        else {
            a->hint_uid=p->po_arrival_uid;
            if(!w->begun){w->begun=1;a->episodes++;wait_event(c,w,"wait_begin","arrival_gain",now,UINT64_MAX,0);}
            else wait_event(c,w,"wait_recheck","arrival_gain",now,UINT64_MAX,0);
        }
    } else if(w) {a->invalidated++;finish(c,w,"prediction_or_gain_invalid",now,UINT64_MAX,0,0);arm(c,now);}
    uint64_t second=now/1000000;
    if(second!=a->log_second){a->log_second=second;a->log_count=0;}
    a->decision++;
    if(a->log_count++<a->config.prediction_log_limit_per_s) {
        char pj[2400],sj[2400],b[8000];prediction_json(pj,sizeof(pj),&pp);prediction_json(sj,sizeof(sj),&sp);
        xqc_po_stream_frame_t *f=&p->po_stream_frames[0];
        char gainstr[32],marginstr[32],waitctx[400];
        if(pp.valid && sp.valid && gain!=INT64_MIN)snprintf(gainstr,sizeof(gainstr),"%"PRId64,gain);else strcpy(gainstr,"null");
        number(marginstr,sizeof(marginstr),margin,margin!=UINT64_MAX);
        if(w && w->used)snprintf(waitctx,sizeof(waitctx),"\"episode_id\":%"PRIu64",\"first_defer_us\":%"PRIu64",\"deadline_us\":%"PRIu64",\"wake_at_us\":%"PRIu64",",w->episode,w->first,w->deadline,w->wake);
        else strcpy(waitctx,"\"episode_id\":null,\"first_defer_us\":null,\"deadline_us\":null,\"wake_at_us\":null,");
        snprintf(b,sizeof(b),"{\"family\":\"adaptive_arrival_events\",\"schema_version\":1,\"event\":\"prediction\","
          "\"conn_uid\":%"PRIu64",\"transfer_id\":\"%s\",\"request_index\":%u,\"stream_id\":%"PRIu64","
          "\"offset\":%"PRIu64",\"length\":%u,\"packet_uid\":%"PRIu64",\"decision_id\":%"PRIu64","
          "\"endpoint\":\"server\",\"direction\":\"get\",\"payload_role\":\"GET_FILE_BODY\","
          "\"scheduler_name\":\"adaptive-v2\",\"mode\":\"%s\",\"model_version\":\"%s\",\"actual_mono_us\":%"PRIu64","
          "\"role_epoch\":%"PRIu64",\"original_pick_reason\":\"CWND_FALLBACK\",\"original_path_id\":%"PRIu64","
          "\"P\":%s,\"S\":%s,%s\"stream_wait_us\":%"PRIu64",\"gain_us\":%s,\"margin_us\":%s,\"comparison_invalid_reason\":\"%s\",\"would_wait\":%s,\"action\":\"%s\"}",
          a->conn_uid,stream->arrival->transfer_id,stream->arrival->request_index,stream->stream_id,f->ps_offset,f->ps_length,
          p->po_arrival_uid,a->decision,xqc_arrival_mode_name(a->config.mode),XQC_ARRIVAL_MODEL,now,a->epoch,S->path_id,pj,sj,waitctx,stream->arrival->spent_us,gainstr,marginstr,margin==UINT64_MAX?"MARGIN_OVERFLOW":(!pp.valid||!sp.valid)?"PREDICTION_INVALID":"OK",would?"true":"false",defer?"DEFER":"SEND");emit(a,b,0);
        p->po_arrival_trace=1;p->po_arrival_decision_id=a->decision;p->po_arrival_decision_us=now;p->po_arrival_prediction_path=defer?P->path_id:S->path_id;
        p->po_arrival_prediction_W=defer?pp.W_us:sp.W_us;p->po_arrival_prediction_valid=defer?pp.valid:sp.valid;
    } else a->sampled_out++;
    if(defer && !p->po_arrival_trace) {
        p->po_arrival_trace=1;p->po_arrival_decision_id=a->decision;p->po_arrival_decision_us=now;
        p->po_arrival_prediction_path=P->path_id;p->po_arrival_prediction_W=pp.W_us;p->po_arrival_prediction_valid=pp.valid;
    }
    return defer;
}
int xqc_arrival_skip(xqc_connection_t *c,xqc_packet_out_t *p,uint64_t now)
{
    if(!enforcing(c))return 0;
    xqc_arrival_t *a=c->arrival;
    for(unsigned j=0;j<XQC_MAX_STREAM_FRAME_IN_PO;j++)if(p->po_stream_frames[j].ps_is_used) {
        arrival_wait_t *w=waitfor(a,p->po_stream_frames[j].ps_stream_id);if(!w)continue;
        xqc_stream_t *s=NULL;
        int changed=p->po_arrival_uid==w->packet_uid
            && (p->po_stream_frames[0].ps_offset!=w->offset || p->po_stream_frames[0].ps_length!=w->length || p->po_used_size!=w->packet_size);
        if(!pure(c,p,&s) || changed) {
            xqc_packet_out_t *first=w->packet;
            finish(c,w,changed?"packet_range_changed":"mixed_or_recovery_packet",now,UINT64_MAX,0,1);arm(c,now);
            if(first && first!=p) {
                a->urgent=first;a->urgent_resume=p;return 2;
            }
            return 0;
        }
        if(p->po_arrival_uid!=w->packet_uid) {a->skips++;return 1;}
        if(w->legacy)return 0;
        account(c,w,now);
        if(now>=w->deadline || !s || s->arrival->spent_us>=a->config.max_defer_stream_us) {
            a->exhausted++;finish(c,w,"deadline_or_stream_budget",now,UINT64_MAX,0,0);arm(c,now);return 0;
        }
        if(now<w->wake)return 1; /* No busy rechecks without an earlier useful event. */
    }
    return 0;
}
void xqc_arrival_queued(xqc_connection_t *c,xqc_packet_out_t *p,xqc_path_ctx_t *path,uint64_t now)
{
    xqc_arrival_t *a=c->arrival;if(!a)return;
    int own=0;
    if(a->urgent==p)a->restart_scan=1;
    for(unsigned i=0;i<16;i++)if(a->waits[i].used && a->waits[i].packet_uid==p->po_arrival_uid) {
        own=1;if(!a->waits[i].legacy && path->path_id==a->primary)a->recovered++;
        finish(c,&a->waits[i],"queued",now,path->path_id,1,0);
    }
    if(p->po_arrival_eligible && path->path_id==p->po_arrival_candidate_secondary)a->legacy++;
    p->po_arrival_eligible=0;
    if(a->waiting && !own)a->progress++;
    arm(c,now);
}
/* Due heads are visited out of order without moving list nodes. Thus a resumable
 * scan cannot reorder later data when an episode exits into NO_SENDABLE. */
static xqc_list_head_t *priority_next(xqc_arrival_t *a)
{
    for(unsigned j=0;j<16;j++) {
        unsigned i=(a->round+j)%16;
        if(a->priority_mask & (1U<<i)) {
            a->priority_mask &= ~(1U<<i);
            arrival_wait_t *w=&a->waits[i];
            if(w->used && w->packet && w->packet->po_arrival_uid==w->packet_uid)
                return &w->packet->po_list;
        }
    }
    return NULL;
}
xqc_list_head_t *xqc_arrival_scan_start(xqc_connection_t *c,xqc_list_head_t *head,uint64_t now)
{
    if(!enforcing(c))return head->next;
    xqc_arrival_t *a=c->arrival;a->continuation=0;
    xqc_list_head_t *start=head->next;
    if(a->cursor && a->cursor_head==head && a->cursor->po_arrival_uid==a->cursor_uid)start=&a->cursor->po_list;
    a->cursor=NULL;a->resume=start==head?NULL:xqc_list_entry(start,xqc_packet_out_t,po_list);
    a->resume_uid=a->resume?a->resume->po_arrival_uid:0;a->priority_mask=0;
    for(unsigned i=0;i<16;i++)if(a->waits[i].used && a->waits[i].wake<=now
        && a->waits[i].packet!=a->resume)a->priority_mask|=1U<<i;
    a->priority_phase=1;a->round=(a->round+1)%16;
    xqc_list_head_t *due=priority_next(a);
    if(due)return due;
    a->priority_phase=0;return start;
}
xqc_list_head_t *xqc_arrival_scan_next(xqc_connection_t *c,xqc_list_head_t *head,
    xqc_list_head_t *current,xqc_list_head_t *next)
{
    (void)current;
    if(!enforcing(c))return next;
    xqc_arrival_t *a=c->arrival;
    if(a->restart_scan) {
        a->restart_scan=0;a->priority_phase=a->urgent_phase=0;
        a->resume=a->urgent=a->urgent_resume=NULL;return head->next;
    }
    if(a->urgent && !a->urgent_phase) {a->urgent_phase=1;return &a->urgent->po_list;}
    if(a->urgent_phase) {
        a->urgent_phase=0;a->urgent=NULL;
        xqc_list_head_t *ret=a->urgent_resume?&a->urgent_resume->po_list:next;
        a->urgent_resume=NULL;return ret;
    }
    if(a->priority_phase) {
        xqc_list_head_t *due=priority_next(a);if(due)return due;
        a->priority_phase=0;
        return a->resume && a->resume->po_arrival_uid==a->resume_uid?&a->resume->po_list:head->next;
    }
    return next;
}
int xqc_arrival_scan_cut(xqc_connection_t *c,xqc_list_head_t *head,xqc_list_head_t *pos,unsigned n,uint64_t now)
{
    if(!enforcing(c))return 0;
    xqc_arrival_t *a=c->arrival;
    int guarded=0;
    for(unsigned i=0;i<16;i++)if(a->waits[i].used){guarded=1;break;}
    if(!guarded) {
        if(pos==head){a->cursor=NULL;a->continuation=0;arm(c,now);}
        return 0; /* No waiting: preserve the original send loop without an artificial batch delay. */
    }
    if(n<a->config.schedule_scan_budget || pos==head) {if(pos==head){a->cursor=NULL;a->continuation=0;arm(c,now);}return 0;}
    a->cursor=xqc_list_entry(pos,xqc_packet_out_t,po_list);a->cursor_uid=a->cursor->po_arrival_uid;a->cursor_head=head;
    a->continuation=now+a->config.recheck_interval_us;a->scan_hits++;arm(c,now);return 1;
}
void xqc_arrival_socket_result(xqc_connection_t *c,xqc_path_ctx_t *path,xqc_packet_out_t *p,
    int64_t result,uint64_t expected,uint64_t now,int batch)
{
    xqc_arrival_t *a=c->arrival;if(!a || !a->config.mode)return;
    history(a,path->path_id);
    arrival_path_t *state=NULL;
    for(unsigned i=0;i<XQC_MAX_PATHS_COUNT;i++)if(a->paths[i].used && a->paths[i].path_id==path->path_id)state=&a->paths[i];
    if(!state)return;
    int old=state->socket_blocked;
    int success=result>=0 && (uint64_t)result==expected;
    state->socket_blocked=!success;state->socket_attempt++;
    if(!success)wake_wait_heads(path->path_send_ctl,now);
    if(!success || old || (p && p->po_arrival_trace)) {
        char b[700],uid[32];if(p)snprintf(uid,sizeof(uid),"%"PRIu64,p->po_arrival_uid);else strcpy(uid,"null");
        snprintf(b,sizeof(b),"{\"family\":\"adaptive_arrival_events\",\"schema_version\":1,\"event\":\"socket_attempt\","
          "\"conn_uid\":%"PRIu64",\"path_id\":%"PRIu64",\"packet_uid\":%s,\"socket_attempt_id\":%"PRIu64","
          "\"actual_mono_us\":%"PRIu64",\"result\":%"PRId64",\"expected\":%"PRIu64",\"units\":\"%s\",\"success\":%s}",
          a->conn_uid,path->path_id,uid,state->socket_attempt,now,result,expected,batch?"datagrams":"bytes",success?"true":"false");
        emit(a,b,1);
    }
}
void xqc_arrival_sent(xqc_send_ctl_t *ctl, xqc_packet_out_t *p,
    uint64_t socket_start_us, uint64_t socket_return_us, int batch)
{
    xqc_arrival_t *a = ctl->ctl_conn->arrival;
    if (!a || !p->po_arrival_trace) return;

    char b[1200], error[64];
    if (p->po_arrival_prediction_valid && p->po_arrival_prediction_path == ctl->ctl_path->path_id
        && socket_start_us >= p->po_arrival_decision_us
        && socket_start_us - p->po_arrival_decision_us <= INT64_MAX
        && p->po_arrival_prediction_W <= INT64_MAX)
    {
        snprintf(error, sizeof(error), "%"PRId64,
            (int64_t)(socket_start_us - p->po_arrival_decision_us) - (int64_t)p->po_arrival_prediction_W);
    } else {
        strcpy(error, "null");
    }
    snprintf(b,sizeof(b),"{\"family\":\"adaptive_arrival_events\",\"schema_version\":1,\"event\":\"packet_sent\","
      "\"conn_uid\":%"PRIu64",\"packet_uid\":%"PRIu64",\"decision_id\":%"PRIu64",\"path_id\":%"PRIu64",\"pns\":%d,\"packet_number\":%"PRIu64","
      "\"send_time_us\":%"PRIu64",\"socket_return_us\":%"PRIu64",\"send_time_basis\":\"%s\","
      "\"stream_id\":%"PRIu64",\"offset\":%"PRIu64",\"length\":%u,\"W_error_us\":%s}",
      a->conn_uid,p->po_arrival_uid,p->po_arrival_decision_id,ctl->ctl_path->path_id,p->po_pkt.pkt_pns,p->po_pkt.pkt_num,
      socket_start_us,socket_return_us,batch?"batch_socket_call_start":"socket_call_start",
      p->po_stream_frames[0].ps_stream_id,p->po_stream_frames[0].ps_offset,p->po_stream_frames[0].ps_length,error);emit(a,b,1);
}
int xqc_engine_set_arrival_config(xqc_engine_t *e,const xqc_arrival_config_t *config)
{ if(!e || xqc_arrival_config_validate(config))return -1;e->arrival_config=*config;return 0; }
int xqc_arrival_register_get(xqc_engine_t *e,const xqc_cid_t *cid,uint64_t id,const char *transfer,
    uint32_t index,uint64_t offset,uint64_t length)
{
    xqc_connection_t *c=e&&cid?xqc_engine_conns_hash_find(e,cid,'s'):NULL;
    if(!c || !c->arrival || !c->arrival->config.mode || !length)return 0;
    xqc_stream_t *s=getstream(c,id);if(!s)return -1;
    if(s->arrival)return -1;
    s->arrival=xqc_calloc(1,sizeof(*s->arrival));if(!s->arrival)return -1;
    s->arrival->request_index=index;s->arrival->file_offset=offset;s->arrival->body_length=length;
    char fallback[64];if(!transfer || !*transfer){snprintf(fallback,sizeof(fallback),"stream-%"PRIu64,id);transfer=fallback;}
    char *out=s->arrival->transfer_id;
    for(unsigned i=0;i<190 && transfer[i];i++) {unsigned char ch=transfer[i];if(ch=='"'||ch=='\\')*out++='\\';*out++=ch<32?'_':ch;}*out=0;
    char binding[1000];
    snprintf(binding,sizeof(binding),"{\"family\":\"adaptive_arrival_events\",\"schema_version\":1,\"event\":\"stream_bind\","
      "\"conn_uid\":%"PRIu64",\"transfer_id\":\"%s\",\"request_index\":%u,\"stream_id\":%"PRIu64","
      "\"file_offset\":%"PRIu64",\"body_length\":%"PRIu64",\"direction\":\"get\",\"payload_role\":\"GET_FILE_BODY\","
      "\"actual_mono_us\":%"PRIu64"}",c->arrival->conn_uid,s->arrival->transfer_id,index,id,offset,length,xqc_monotonic_timestamp());
    emit(c->arrival,binding,1);
    return 0;
}
int xqc_arrival_attach_sink(xqc_engine_t *e,const xqc_cid_t *cid,xqc_arrival_sink_pt sink,void *data)
{
    xqc_connection_t *c=e&&cid?xqc_engine_conns_hash_find(e,cid,'s'):NULL;
    if(!c || !c->arrival)return -1;
    xqc_arrival_t *a=c->arrival;
    if(!sink){summary(a);a->sink=NULL;a->sink_data=NULL;return 0;}
    a->sink=sink;a->sink_data=data;config_event(a);return 0;
}
static size_t v2size(void) {return xqc_adaptive_scheduler_cb.xqc_scheduler_size();}
static void v2init(void *s,xqc_log_t *l,xqc_scheduler_params_t *p)
{xqc_adaptive_v2_init_core(s,l,p);}
static void v2close(void *s,xqc_connection_t *c) {if(xqc_adaptive_scheduler_cb.xqc_scheduler_conn_close_diag)xqc_adaptive_scheduler_cb.xqc_scheduler_conn_close_diag(s,c);}
static xqc_path_ctx_t *v2get(void *s,xqc_connection_t *c,xqc_packet_out_t *p,int cc,int reinject,xqc_bool_t *blocked)
{xqc_arrival_clear_hint(c);return xqc_adaptive_get_path_core(s,c,p,cc,reinject,blocked);}
const xqc_scheduler_callback_t xqc_adaptive_v2_scheduler_cb={
    .xqc_scheduler_size=v2size,.xqc_scheduler_init=v2init,.xqc_scheduler_get_path=v2get,.xqc_scheduler_conn_close_diag=v2close
};
