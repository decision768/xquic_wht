/* Deterministic transport harness. Production scheduler/runtime and scheduling,
 * queue and GP timer function bodies are compiled unchanged by the runner.
 * Socket, congestion controller and observer are controlled boundary doubles. */
#include <stdint.h>
#include <xquic/netins_udp.h>
#define XQC_OBSERVER_CLOCK_H
static uint64_t clock_fn(void);
#define xqc_observer_clock_us clock_fn
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "src/transport/scheduler/xqc_scheduler_adaptive_v2.c"
#include "src/transport/scheduler/xqc_scheduler_adaptive.c"
ssize_t xqc_send(xqc_connection_t *,xqc_path_ctx_t *,unsigned char *,unsigned int);
#include "src/common/xqc_algorithm.h"
#include "arrival_transport_slices.c"

static uint64_t clock_now=1000000, connection_sequence;
static uint64_t clock_fn(void){return clock_now;}
xqc_timestamp_pt xqc_monotonic_timestamp=clock_fn;
static xqc_connection_t conn;
static xqc_engine_t engine;
static xqc_send_queue_t queue;
static xqc_path_ctx_t paths[2];
static xqc_send_ctl_t ctls[2];
static xqc_stream_t streams[3];
static xqc_adaptive_scheduler_t adaptive;
static xqc_observer_t observer;
static xqc_scheduler_callback_t cb;
static unsigned sent_events, log_fail, log_records, pacing_debits;
static ssize_t socket_result;
static uint64_t test_window=10000;
static uint64_t test_cwnd(void *ctx){(void)ctx;return test_window;}
static uint32_t test_rate(void *ctx){(void)ctx;return 100000;}
static uint8_t test_full(void *ctx){(void)ctx;return 1;}
static void test_bbr(void *ctx,xqc_sample_t *sample,xqc_cc_params_t params)
{(void)ctx;(void)sample;(void)params;}
const xqc_cong_ctrl_callback_t xqc_bbr_cb={.xqc_cong_ctl_init_bbr=test_bbr};
static void test_other_cc(void *ctx,xqc_sample_t *sample,xqc_cc_params_t params)
{test_bbr(ctx,sample,params);}
static xqc_bbr_info_interface_t info;
static xqc_cong_ctrl_callback_t cc;
static int log_sink(void *data,int family,const char *line)
{
    (void)data;(void)family;log_records++;
    const char *logpath=getenv("XQC_ARRIVAL_TEST_LOG");
    if(logpath) {FILE *f=fopen(logpath,"ab");assert(f);fprintf(f,"%s\n",line);fclose(f);}
    assert(line[0]=='{' && line[strlen(line)-1]=='}');
    if(strstr(line,"\"event\":\"packet_sent\""))sent_events++;
    return log_fail?-1:0;
}
xqc_stream_t *xqc_find_stream_by_id(xqc_stream_id_t id,xqc_id_hash_table_t *hash)
{(void)hash;for(unsigned i=0;i<3;i++)if(streams[i].stream_id==id)return &streams[i];return NULL;}
xqc_connection_t *xqc_engine_conns_hash_find(xqc_engine_t *e,const xqc_cid_t *cid,char type)
{(void)e;(void)cid;(void)type;return &conn;}
xqc_path_ctx_t *xqc_conn_find_path_by_path_id(xqc_connection_t *c,uint64_t id)
{(void)c;for(unsigned i=0;i<2;i++)if(paths[i].path_id==id)return &paths[i];return NULL;}
xqc_usec_t xqc_send_ctl_get_srtt(xqc_send_ctl_t *ctl){return ctl->ctl_srtt;}
uint64_t xqc_send_ctl_get_est_bw(xqc_send_ctl_t *ctl){(void)ctl;return 100000;}
uint64_t xqc_send_ctl_get_pacing_rate(xqc_send_ctl_t *ctl){(void)ctl;return 1000000;}
int xqc_send_ctl_can_send(xqc_send_ctl_t *ctl,xqc_packet_out_t *p,uint32_t z)
{return ctl->ctl_bytes_in_flight+z+p->po_used_size<=test_window;}
void xqc_observer_role(xqc_connection_t *c,int valid,uint64_t p,uint64_t s,double share,int phase)
{(void)c;(void)valid;(void)p;(void)s;(void)share;(void)phase;}
void xqc_scheduler_diag_init(xqc_sched_diag_t *d,const char *name,const xqc_scheduler_params_t *p)
{(void)p;memset(d,0,sizeof(*d));d->sched_name=name;}
xqc_sched_diag_path_t *xqc_scheduler_diag_record_evaluation(xqc_sched_diag_t *d,xqc_path_ctx_t *p,const xqc_sched_path_eval_t *e)
{(void)d;(void)p;(void)e;return NULL;}
void xqc_scheduler_diag_record_decision(xqc_sched_diag_t *d,xqc_path_ctx_t *p,size_t n,xqc_bool_t no)
{(void)d;(void)p;(void)n;(void)no;}
void xqc_scheduler_diag_maybe_tick(xqc_sched_diag_t *d,xqc_connection_t *c,xqc_usec_t now)
{(void)d;(void)c;(void)now;}
void xqc_scheduler_diag_close(xqc_sched_diag_t *d,xqc_connection_t *c){(void)d;(void)c;}
xqc_bool_t xqc_packet_out_on_specific_path(xqc_connection_t *c,xqc_packet_out_t *p,xqc_path_ctx_t **path)
{if(!p->po_path_flag)return 0;*path=xqc_conn_find_path_by_path_id(c,p->po_path_id);return 1;}
/* Included even in non-FEC builds by the existing connection loop. */
xqc_usec_t xqc_conn_get_queue_fin_timeout(xqc_connection_t *c){(void)c;return 0;}

ssize_t xqc_send(xqc_connection_t *c,xqc_path_ctx_t *path,unsigned char *data,unsigned int len)
{(void)c;(void)path;(void)data;return socket_result<0?socket_result:(ssize_t)len;}
void xqc_send_ctl_on_packet_sent(xqc_send_ctl_t *ctl,xqc_pn_ctl_t *pn,xqc_packet_out_t *p,xqc_usec_t now)
{(void)pn;xqc_arrival_sent(ctl,p,now);}
int xqc_pacing_is_on(xqc_pacing_t *p){return p->pacing_on;}
void xqc_pacing_on_packet_sent(xqc_pacing_t *p,uint32_t n)
{pacing_debits++;p->bytes_budget=p->bytes_budget>n?p->bytes_budget-n:0;}
void xqc_send_queue_insert_unacked(xqc_packet_out_t *p,xqc_list_head_t *head,xqc_send_queue_t *q)
{(void)q;xqc_list_add_tail(&p->po_list,head);}
void xqc_send_queue_insert_free(xqc_packet_out_t *p,xqc_list_head_t *head,xqc_send_queue_t *q)
{xqc_arrival_packet_gone(q->sndq_conn,p);xqc_list_add_tail(&p->po_list,head);}

static void setup(unsigned mode,int old)
{
    memset(&conn,0,sizeof(conn));memset(&engine,0,sizeof(engine));memset(&queue,0,sizeof(queue));
    memset(paths,0,sizeof(paths));memset(ctls,0,sizeof(ctls));memset(streams,0,sizeof(streams));
    memset(&cc,0,sizeof(cc));memset(&info,0,sizeof(info));memset(&observer,0,sizeof(observer));
    clock_now=1000000;test_window=10000;log_fail=sent_events=log_records=pacing_debits=0;socket_result=0;
    cc.xqc_cong_ctl_get_cwnd=test_cwnd;cc.xqc_cong_ctl_get_bandwidth_estimate=test_rate;
    cc.xqc_cong_ctl_info_cb=&info;info.full_bw_reached=test_full;
    /* Non-NULL BBR initialization marks the same eligibility as production. */
    cc.xqc_cong_ctl_init_bbr=test_bbr;
    xqc_arrival_config_init(&engine.arrival_config);engine.arrival_config.mode=mode;engine.eng_type=XQC_ENGINE_SERVER;
    conn.engine=&engine;conn.conn_create_time=++connection_sequence;conn.pkt_out_size=1000;conn.conn_send_queue=&queue;queue.sndq_conn=&conn;
    xqc_init_list_head(&conn.conn_paths_list);xqc_init_list_head(&conn.conn_timer_manager.gp_timer_list);
    xqc_init_list_head(&queue.sndq_send_packets);xqc_init_list_head(&queue.sndq_send_packets_high_pri);
    xqc_init_list_head(&queue.sndq_free_packets);
    for(unsigned j=0;j<XQC_PNS_N;j++)xqc_init_list_head(&queue.sndq_unacked_packets[j]);
    xqc_init_list_head(&queue.sndq_lost_packets);xqc_init_list_head(&queue.sndq_pto_probe_packets);
    cb=old?xqc_adaptive_scheduler_cb:xqc_adaptive_v2_scheduler_cb;conn.scheduler_callback=&cb;conn.scheduler=&adaptive;conn.observer=&observer;
    cb.xqc_scheduler_init(&adaptive,NULL,&conn.conn_settings.scheduler_params);
    xqc_arrival_conn_init(&conn);if(conn.arrival){conn.arrival->sink=log_sink;config_event(conn.arrival);}
    for(unsigned i=0;i<2;i++) {
        paths[i].path_id=10+i*13;paths[i].parent_conn=&conn;paths[i].path_state=XQC_PATH_STATE_ACTIVE;
        paths[i].app_path_status=XQC_APP_PATH_STATUS_AVAILABLE;paths[i].path_send_ctl=&ctls[i];
        ctls[i].ctl_conn=&conn;ctls[i].ctl_path=&paths[i];ctls[i].ctl_cong_callback=&cc;
        ctls[i].ctl_cong=(void *)1;ctls[i].ctl_srtt=i?120000:20000;ctls[i].ctl_first_rtt_sample_time=1;
        xqc_init_list_head(&ctls[i].path_timer_manager.gp_timer_list);
        for(unsigned j=0;j<XQC_SEND_TYPE_N;j++)xqc_init_list_head(&paths[i].path_schedule_buf[j]);
        xqc_list_add_tail(&paths[i].path_list,&conn.conn_paths_list);
        if(conn.arrival && mode) {
            xqc_arrival_history_t *h=history(conn.arrival,paths[i].path_id);
            for(unsigned j=0;j<8;j++) {
                xqc_arrival_history_ack(h,&conn.arrival->config,700000+j*40000,2000);
                xqc_arrival_history_rate(h,&conn.arrival->config,700000+j*40000,100000,40000,0,2);
            }
            /* A second ACK history with 3 ms forecast, independent of rate span. */
            h->na=0;
            for(unsigned j=0;j<4;j++)xqc_arrival_history_ack(h,&conn.arrival->config,973000+j*6000,2000);
        }
    }
    /* First ACK = 997000; make latest batch 997000, next at 1003000. */
    if(conn.arrival && mode)for(unsigned i=0;i<2;i++) {
        xqc_arrival_history_t *h=history(conn.arrival,paths[i].path_id);
        for(unsigned j=0;j<h->na;j++)h->acks[j].time_us+=6000;
    }
    for(unsigned i=0;i<3;i++) {
        streams[i].stream_id=i*4;streams[i].stream_conn=&conn;
        xqc_arrival_register_get(&engine,&conn.scid_set.user_scid,streams[i].stream_id,"transfer",i,0,10000000);
    }
    adaptive.phase=XQC_ADAPTIVE_PHASE_BULK;adaptive.primary_valid=1;
    adaptive.primary_path_id=paths[0].path_id;adaptive.adjust_start_us=clock_now;
    ctls[0].ctl_bytes_in_flight=10000;
}
static void teardown(void)
{
    xqc_arrival_conn_close(&conn);
    for(unsigned i=0;i<3;i++){free(streams[i].arrival);streams[i].arrival=NULL;}
}
static void packet(xqc_packet_out_t *p,unsigned stream)
{
    memset(p,0,sizeof(*p));xqc_init_list_head(&p->po_list);p->po_arrival_owner=&conn;
    p->po_arrival_uid=++conn.arrival_next_packet_uid;p->po_used_size=1000;
    p->po_pkt.pkt_type=XQC_PTYPE_SHORT_HEADER;p->po_frame_types=XQC_FRAME_BIT_STREAM;
    p->po_stream_frames[0].ps_is_used=1;p->po_stream_frames[0].ps_arrival_body=1;
    p->po_stream_frames[0].ps_stream_id=streams[stream].stream_id;p->po_stream_frames[0].ps_length=950;
}
static int decide(xqc_packet_out_t *p)
{xqc_arrival_clear_hint(&conn);return xqc_arrival_fallback(&conn,p,&paths[0],&paths[1],2,0,clock_now);}
static void pass(void){xqc_conn_schedule_packets(&conn,&queue.sndq_send_packets,1,XQC_SEND_TYPE_NORMAL);}
int main(void)
{
    xqc_packet_out_t p;setup(XQC_ARRIVAL_ENFORCE,0);packet(&p,0);
    assert(decide(&p));assert(conn.arrival->waiting==1 && xqc_arrival_is_defer(&conn,&p));
    uint64_t deadline=waitfor(conn.arrival,0)->deadline;
    assert(deadline==clock_now+5000 && xqc_conn_next_wakeup_time(&conn)==clock_now+1000);
    clock_now+=1000;assert(decide(&p));assert(waitfor(conn.arrival,0)->deadline==deadline);
    assert(streams[0].arrival->spent_us==1000);
    clock_now+=1000;assert(decide(&p));assert(streams[0].arrival->spent_us==2000);
    puts("PASS T09/T17: enforce, fixed deadline and elapsed-time accounting");
    clock_now=deadline+700;xqc_timer_expire(&conn.conn_timer_manager,clock_now);
    assert(conn.arrival->timer_wakes==1 && conn.arrival->lateness>0);
    assert(!decide(&p) && p.po_arrival_legacy && conn.arrival->waiting==0);
    assert(!decide(&p));assert(streams[0].arrival->spent_us==5700);
    assert(xqc_conn_next_wakeup_time(&conn)==0);
    puts("PASS T16/T18: no-input GP timer, lateness, exit and legacy-once");teardown();

    setup(XQC_ARRIVAL_SHADOW,0);packet(&p,0);
    xqc_pacing_t pacer=ctls[0].ctl_pacing;uint64_t I=ctls[0].ctl_bytes_in_flight;
    assert(!decide(&p) && conn.arrival->would_wait==1 && !conn.arrival->waiting);
    assert(!memcmp(&pacer,&ctls[0].ctl_pacing,sizeof(pacer)) && ctls[0].ctl_bytes_in_flight==I);
    assert(xqc_conn_next_wakeup_time(&conn)==0);puts("PASS T02: shadow keeps transport and timers unchanged");teardown();

    /* Run exactly the same input stream through the two actual callback objects. */
    uint64_t choices[3][40],fallbacks[3],no_sendable[3];
    for(unsigned which=0;which<3;which++) {
        setup(which==2?XQC_ARRIVAL_SHADOW:XQC_ARRIVAL_OFF,which==1);packet(&p,0);
        for(unsigned i=0;i<40;i++) {
            ctls[0].ctl_bytes_in_flight=(i%4<2)?10000:0;
            ctls[1].ctl_bytes_in_flight=(i%4==1)?10000:0;
            xqc_bool_t blocked; xqc_path_ctx_t *picked=cb.xqc_scheduler_get_path(&adaptive,&conn,&p,1,0,&blocked);
            choices[which][i]=picked?picked->path_id:UINT64_MAX;
        }
        fallbacks[which]=adaptive.fallback_real_inflight;no_sendable[which]=adaptive.no_sendable_count;
        assert(xqc_conn_next_wakeup_time(&conn)==0);teardown();
    }
    assert(!memcmp(choices[0],choices[1],sizeof(choices[0])) && fallbacks[0]==fallbacks[1] && no_sendable[0]==no_sendable[1]);
    assert(!memcmp(choices[0],choices[2],sizeof(choices[0])) && fallbacks[0]==fallbacks[2] && no_sendable[0]==no_sendable[2]);
    puts("PASS T01/T02/T26/T29: actual off/shadow/original choices and old counters match");

    setup(XQC_ARRIVAL_ENFORCE,0);packet(&p,0);
    xqc_list_add_tail(&p.po_list,&queue.sndq_send_packets);
    pass();assert(conn.arrival->waiting==1 && conn.sched_cc_blocked==0 && adaptive.no_sendable_count==0);
    assert(paths[0].path_schedule_bytes==0 && paths[1].path_schedule_bytes==0 && ctls[0].ctl_bytes_in_flight==10000);
    puts("PASS T13: DEFER survives all fallbacks without changing I/Z or blocking counts");
    xqc_packet_out_t *many=calloc(514,sizeof(*many));assert(many);
    for(unsigned i=0;i<514;i++){packet(&many[i],i==513?1:0);many[i].po_stream_frames[0].ps_offset=950*(i+1);xqc_list_add_tail(&many[i].po_list,&queue.sndq_send_packets);}
    many[513].po_arrival_legacy=1; /* B has already used its wait opportunity. */
    pass();assert(conn.arrival->scan_hits==1);
    for(unsigned i=0;i<4 && !(many[513].po_flag&XQC_POF_IN_PATH_BUF_LIST);i++) {
        uint64_t wake=xqc_conn_next_wakeup_time(&conn);assert(wake);clock_now=wake;
        xqc_timer_expire(&conn.conn_timer_manager,clock_now);pass();
    }
    assert(many[513].po_flag&XQC_POF_IN_PATH_BUF_LIST);
    for(unsigned i=0;i<513;i++)assert(!(many[i].po_flag&XQC_POF_IN_PATH_BUF_LIST));
    assert(conn.arrival->progress && conn.arrival->skips>512);
    puts("PASS T14/T15: another stream progresses beyond 512 skipped packets");
    /* Releasing the head must preserve original same-stream packet order. */
    ctls[0].ctl_bytes_in_flight=0;clock_now+=1000;pass();
    assert(p.po_flag&XQC_POF_IN_PATH_BUF_LIST);
    assert(p.po_list.next==&many[0].po_list);
    for(unsigned i=1;i<8;i++)assert(many[i-1].po_list.next==&many[i].po_list);
    puts("PASS T14/T17: recovery restarts ordered scanning before later offsets");
    teardown();free(many);

    setup(XQC_ARRIVAL_ENFORCE,0);packet(&p,0);assert(decide(&p));
    xqc_arrival_packet_gone(&conn,&p);assert(!conn.arrival->waiting && xqc_conn_next_wakeup_time(&conn)==0);
    packet(&p,0);assert(decide(&p));xqc_arrival_stream_close(&conn,0);assert(!conn.arrival->waiting);
    packet(&p,1);assert(decide(&p));xqc_arrival_path_reset(&conn,paths[0].path_id);assert(!conn.arrival->waiting);
    assert(history(conn.arrival,paths[0].path_id)->nr==0);
    puts("PASS T19/T23: packet reuse, stream cancellation and path reset clean up waits");teardown();

    setup(XQC_ARRIVAL_ENFORCE,0);packet(&p,0);p.po_flag=XQC_POF_TLP;assert(!decide(&p));
    p.po_flag=0;p.po_path_flag=1;assert(!decide(&p));p.po_path_flag=0;
    p.po_stream_frames[0].ps_arrival_body=0;assert(!decide(&p));
    p.po_stream_frames[0].ps_arrival_body=1;p.po_frame_types|=XQC_FRAME_BIT_ACK;assert(!decide(&p));
    puts("PASS T20/T21/T25: mixed, framing, specified-path and recovery bypass, no observer dependency");
    p.po_frame_types=XQC_FRAME_BIT_STREAM;conn.observer=NULL;assert(decide(&p));
    p.po_pkt.pkt_num=7;p.po_pkt.pkt_pns=XQC_PNS_APP_DATA;
    assert(!sent_events);xqc_arrival_sent(&ctls[0],&p,clock_now+3000);assert(sent_events==1);
    log_fail=1;clock_now+=1000;decide(&p);assert(conn.arrival->lifecycle_dropped>0);
    puts("PASS T28: unsampled totals distinguish lifecycle drops");teardown();

    setup(XQC_ARRIVAL_ENFORCE,0);packet(&p,0);assert(decide(&p));
    xqc_arrival_roles(&conn,paths[1].path_id,paths[0].path_id,1,clock_now);
    assert(!conn.arrival->waiting && p.po_arrival_legacy);
    puts("PASS T12/T19: physical role change cancels the old episode");teardown();
    setup(XQC_ARRIVAL_ENFORCE,0);packet(&p,0);assert(decide(&p));
    assert(xqc_arrival_set_mode(&engine,&conn.scid_set.user_scid,XQC_ARRIVAL_OFF)==0);
    assert(!conn.arrival->waiting && xqc_conn_next_wakeup_time(&conn)==0);
    puts("PASS T23/T29: switching off cancels waits and active timers");teardown();
    setup(XQC_ARRIVAL_ENFORCE,0);packet(&p,0);
    conn.conn_timer_manager.next_gp_timer_id=XQC_GP_TIMER_ID_MAX;
    assert(!decide(&p) && !conn.arrival->waiting);
    puts("PASS T19: timer allocation failure takes the legacy path");teardown();
    setup(XQC_ARRIVAL_ENFORCE,0);packet(&p,0);
    conn.arrival->config.max_waiting_streams=1;assert(decide(&p));
    xqc_packet_out_t second;packet(&second,1);assert(!decide(&second) && conn.arrival->waiting==1);
    xqc_list_add_tail(&p.po_list,&queue.sndq_send_packets);
    packet(&second,0);second.po_frame_types|=XQC_FRAME_BIT_ACK;
    xqc_list_add_tail(&second.po_list,&queue.sndq_send_packets);
    pass();assert(!conn.arrival->waiting && (p.po_flag&XQC_POF_IN_PATH_BUF_LIST) && (second.po_flag&XQC_POF_IN_PATH_BUF_LIST));
    assert(paths[1].path_schedule_buf[0].next==&p.po_list && p.po_list.next==&second.po_list);
    puts("PASS T19/T20: full table falls back; mixed packets cancel and retry head first");teardown();
    setup(XQC_ARRIVAL_ENFORCE,0);packet(&p,0);
    streams[0].arrival->spent_us=20000;assert(!decide(&p));
    packet(&p,1);assert(decide(&p));
    assert(history(conn.arrival,paths[0].path_id)->nr==8 && streams[2].arrival->spent_us==0);
    puts("PASS T18/T22: exhausted stream does not spend another transfer's budget; rate history persists");teardown();
    setup(XQC_ARRIVAL_SHADOW,0);packet(&p,0);assert(!decide(&p));
    conn.enc_pkt_len=1000;socket_result=-XQC_EAGAIN;
    assert(xqc_send_packet_with_pn(&conn,&paths[1],&p)==-XQC_EAGAIN && sent_events==0);
    socket_result=0;assert(xqc_send_packet_with_pn(&conn,&paths[1],&p)==1000 && sent_events==1);
    packet(&p,0);p.po_arrival_trace=1;xqc_path_send_buffer_append(&paths[1],&p,&paths[1].path_schedule_buf[0]);
    xqc_on_packets_send_burst(&conn,&paths[1],-XQC_EAGAIN,clock_now,0);
    assert(sent_events==1 && paths[1].path_schedule_bytes==1000);
    xqc_on_packets_send_burst(&conn,&paths[1],1,clock_now,0);
    assert(sent_events==2 && paths[1].path_schedule_bytes==0);
    puts("PASS T24: real single/batch completion paths log only successful sends");
    packet(&p,0);p.po_frame_types=XQC_FRAME_BIT_ACK;ctls[1].ctl_pacing.pacing_on=1;
    xqc_path_send_buffer_append(&paths[1],&p,&paths[1].path_schedule_buf[0]);
    xqc_on_packets_send_burst(&conn,&paths[1],1,clock_now,0);assert(pacing_debits==0);
    puts("PASS shared pacing regression: ACK-only batch does not debit the pacer");teardown();
    setup(XQC_ARRIVAL_ENFORCE,0);packet(&p,0);
    streams[0].stream_send_offset=0;p.po_stream_frames[0].ps_offset=0;p.po_stream_frames[0].ps_length=10;
    xqc_arrival_frame_written(&streams[0],&p,10);assert(!p.po_stream_frames[0].ps_arrival_body);
    streams[0].stream_send_offset=10;streams[0].arrival_writing_body=1;p.po_stream_frames[0].ps_length=100;
    xqc_arrival_frame_written(&streams[0],&p,90);assert(!p.po_stream_frames[0].ps_arrival_body);
    streams[0].stream_send_offset=100;p.po_stream_frames[0].ps_offset=100;p.po_stream_frames[0].ps_length=100;
    xqc_arrival_frame_written(&streams[0],&p,100);assert(p.po_stream_frames[0].ps_arrival_body);
    puts("PASS T20/T25: HTTP/3 framing merged into STREAM data is never marked body-only");
    xqc_packet_out_t *first=xqc_packet_out_get(&queue),*copy=xqc_packet_out_get(&queue);
    assert(first && copy && first->po_arrival_uid!=copy->po_arrival_uid);
    uint64_t copy_uid=copy->po_arrival_uid,old_uid=first->po_arrival_uid;
    first->po_arrival_trace=1;first->po_arrival_legacy=1;
    xqc_packet_out_copy(copy,first);assert(copy->po_arrival_uid==copy_uid && !copy->po_arrival_trace && !copy->po_arrival_legacy);
    xqc_packet_out_destroy(copy);xqc_list_add_tail(&first->po_list,&queue.sndq_free_packets);queue.sndq_packets_free=1;
    xqc_packet_out_t *reused=xqc_packet_out_get(&queue);assert(reused==first && reused->po_arrival_uid!=old_uid);
    xqc_packet_out_destroy(reused);
    puts("PASS T23: production packet allocation/copy/reuse assigns independent stable UIDs");
    packet(&p,0);assert(decide(&p));p.po_stream_frames[0].ps_length++;
    assert(!xqc_arrival_skip(&conn,&p,clock_now) && !conn.arrival->waiting && p.po_arrival_legacy);
    puts("PASS T08/T23: modified head ranges invalidate an existing wait");teardown();
    setup(XQC_ARRIVAL_SHADOW,0);packet(&p,0);conn.arrival->config.prediction_log_limit_per_s=1;
    assert(!decide(&p));assert(!decide(&p));assert(!decide(&p));
    assert(conn.arrival->eligible==3 && conn.arrival->would_wait==3 && conn.arrival->sampled_out==2);
    puts("PASS T28: prediction sampling preserves unsampled counters");teardown();
    setup(XQC_ARRIVAL_ENFORCE,0);packet(&p,0);p.po_path_id=paths[0].path_id;p.po_pkt.pkt_pns=XQC_PNS_APP_DATA;
    p.po_flag=XQC_POF_IN_FLIGHT;xqc_arrival_history_t *hh=history(conn.arrival,paths[0].path_id);
    unsigned old_batches=hh->na;
    xqc_send_ctl_decrease_inflight(&conn,&p); /* Loss/discard context must not create ACK batches. */
    assert(hh->na==old_batches);
    uint64_t prior=ctls[0].ctl_bytes_in_flight;xqc_send_ctl_decrease_inflight(&conn,&p);
    xqc_arrival_ack(&ctls[0],clock_now,prior-ctls[0].ctl_bytes_in_flight);assert(hh->na==old_batches);
    p.po_flag=XQC_POF_IN_FLIGHT;prior=ctls[0].ctl_bytes_in_flight;xqc_send_ctl_decrease_inflight(&conn,&p);
    xqc_arrival_ack(&ctls[0],clock_now,prior-ctls[0].ctl_bytes_in_flight);assert(hh->na==old_batches+1);
    prior=ctls[0].ctl_bytes_in_flight;xqc_send_ctl_decrease_inflight(&conn,&p);
    xqc_arrival_ack(&ctls[0],clock_now,prior-ctls[0].ctl_bytes_in_flight);assert(hh->na==old_batches+1);
    puts("PASS T06: actual in-flight flag prevents duplicate or late ACK window release");teardown();
    int parsed;
    assert(!parse_scheduler("adaptive",&parsed) && parsed==NETINS_UDP_SCHEDULER_ADAPTIVE);
    assert(!parse_scheduler("adaptive-v2",&parsed) && parsed==NETINS_UDP_SCHEDULER_ADAPTIVE_V2);
    assert(!strcmp(netins_udp_scheduler_name(parsed),"adaptive-v2"));
    assert(parse_scheduler("adaptive_v2",&parsed)<0);
    puts("PASS T29: public CLI parsing and API name resolution retain the hyphenated name");
    setup(XQC_ARRIVAL_ENFORCE,0);packet(&p,0);xqc_list_add_tail(&p.po_list,&queue.sndq_send_packets);pass();
    uint64_t checks=waitfor(conn.arrival,0)->checks;clock_now+=100;pass();
    assert(waitfor(conn.arrival,0)->checks==checks); /* Before recheck interval, do not poll. */
    ctls[0].ctl_bytes_in_flight=8000;xqc_arrival_ack(&ctls[0],clock_now,2000);pass();
    assert(p.po_flag&XQC_POF_IN_PATH_BUF_LIST);assert(p.po_path_id==paths[0].path_id);
    assert(!conn.arrival->waiting && conn.arrival->ack_wakes==1);
    puts("PASS T16: an early positive ACK rechecks a head immediately without busy polling");teardown();
    setup(XQC_ARRIVAL_ENFORCE,0);packet(&p,0);packet(&second,0);
    xqc_list_add_tail(&p.po_list,&queue.sndq_send_packets);xqc_list_add_tail(&second.po_list,&queue.sndq_send_packets);
    assert(decide(&p));second.po_frame_types|=XQC_FRAME_BIT_ACK;
    xqc_send_queue_move_to_high_pri(&second.po_list,&queue);
    assert(!conn.arrival->waiting && queue.sndq_send_packets_high_pri.next==&p.po_list && p.po_list.next==&second.po_list);
    xqc_conn_schedule_packets(&conn,&queue.sndq_send_packets_high_pri,1,XQC_SEND_TYPE_NORMAL_HIGH_PRI);
    assert(p.po_flag&XQC_POF_IN_PATH_BUF_LIST);assert(second.po_flag&XQC_POF_IN_PATH_BUF_LIST);
    puts("PASS T20/T21: priority promotion cancels waits and queues the old head before control data");teardown();
    setup(XQC_ARRIVAL_ENFORCE,0);packet(&p,0);
    adaptive.primary_path_id=paths[1].path_id;ctls[1].ctl_bytes_in_flight=10000;ctls[0].ctl_bytes_in_flight=0;
    xqc_bool_t blocked=0;xqc_path_ctx_t *selected=cb.xqc_scheduler_get_path(&adaptive,&conn,&p,1,0,&blocked);
    assert(selected==&paths[0] && !conn.arrival->waiting);
    assert(!xqc_arrival_fallback(&conn,&p,&paths[1],&paths[0],2,1,clock_now));
    puts("PASS T12/T21: reverse physical roles still fall back to the faster secondary; reinjection bypasses waiting");
    printf("runtime_context_bytes=%zu stream_metadata_bytes=%zu packet_out_bytes=%zu\n",sizeof(xqc_arrival_t),sizeof(xqc_arrival_stream_t),sizeof(xqc_packet_out_t));
    teardown();
    setup(XQC_ARRIVAL_ENFORCE,0);packet(&p,0);
    xqc_arrival_socket_result(&conn,&paths[0],NULL,-XQC_EAGAIN,1,clock_now,1);
    assert(!decide(&p) && conn.arrival->invalid[AR_SOCKET_BLOCKED]==1 && !conn.arrival->waiting);
    xqc_arrival_socket_result(&conn,&paths[0],NULL,1,1,clock_now,1);
    assert(decide(&p));
    puts("PASS T24: known socket backpressure invalidates prediction until a successful write");teardown();
    setup(XQC_ARRIVAL_ENFORCE,0);packet(&p,0);cc.xqc_cong_ctl_init_bbr=test_other_cc;
    assert(!decide(&p) && conn.arrival->invalid[AR_CC_UNSUPPORTED]==2);
    puts("PASS T07: a BBR-like callback does not silently enable an unsupported CC model");teardown();
    setup(XQC_ARRIVAL_ENFORCE,0);test_window=1000000;
    many=calloc(700,sizeof(*many));assert(many);
    for(unsigned i=0;i<700;i++){packet(&many[i],0);many[i].po_stream_frames[0].ps_offset=950*i;xqc_list_add_tail(&many[i].po_list,&queue.sndq_send_packets);}
    pass();
    for(unsigned i=0;i<700;i++)assert(many[i].po_flag&XQC_POF_IN_PATH_BUF_LIST);
    assert(!conn.arrival->waiting && !conn.arrival->scan_hits && !xqc_conn_next_wakeup_time(&conn));
    puts("PASS T01/T13: enforce without a qualifying wait preserves the original unthrottled send loop");teardown();free(many);
    return 0;
}
