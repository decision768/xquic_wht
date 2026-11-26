/* Production lifecycle functions are included verbatim by the Python runner.
 * TLS, sockets, disk writes and the event loop use deterministic doubles. */
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define XQC_OK 0
#define H3_NO_ERROR 0
#define EVLOOP_ONCE 1
#define MINI_BACKEND_TRANSFER_GET 1
#define MINI_BACKEND_TRANSFER_POST 2
#define XQC_MINI_DEFAULT_GET_CONCURRENCY 8
#define XQC_MINI_DEFAULT_POST_CONCURRENCY 8
#define XQC_MINI_PATH_RETRY_INTERVAL_USEC 1000
typedef uint64_t xqc_usec_t;
typedef int REQUEST_METHOD;
enum { REQUEST_METHOD_GET = 1, REQUEST_METHOD_POST = 2 };
typedef struct { int value; } xqc_cid_t;
typedef struct { int value; } xqc_h3_conn_t;
typedef struct { int value; } xqc_engine_t;
struct event { int value; };
struct event_base { int value; };
struct timeval { long tv_sec, tv_usec; };
typedef struct { int enabled; char attempt_id[192]; } netins_udp_observer_config_t;
typedef struct { netins_udp_observer_config_t observer_config; int defer_transfer_setup;
    struct { REQUEST_METHOD method; } req_cfg; struct { int enabled; } stream_poc;
} xqc_mini_cli_args_t;
typedef struct { xqc_engine_t *engine; struct event_base *eb; xqc_mini_cli_args_t *args; } xqc_mini_cli_ctx_t;
typedef struct mini_observer_s {
    xqc_engine_t *engine; xqc_cid_t cid; struct event *timer; void *writer;
    uint64_t interval_us, due_us; int finished, finish_error;
    void (*extra)(void *); void *extra_data;
} mini_observer_t;
typedef struct {
    xqc_mini_cli_ctx_t *ctx; xqc_cid_t cid; xqc_h3_conn_t *h3_conn;
    mini_observer_t *observer; void *stream_api_state;
    char get_task_id[192]; int get_task_needs_confirmation, get_confirmation_result;
    int handshake_finished, connection_closed, last_error_code;
    int transfer_active, transfer_finished, transfer_io_complete, transfer_error_code;
    int auto_start_requests, completed_requests, target_requests, active_requests, download_fd;
    xqc_usec_t transfer_start_time, transfer_end_time;
    uint64_t transfer_observer_start_us;
    int transfer_observer_terminal_recorded, transfer_observer_checkpoint_result;
    size_t download_total_bytes, download_received_bytes, download_expected_bytes, upload_total_bytes;
    xqc_usec_t download_first_body_time, download_last_body_time, upload_start_time, upload_end_time;
    uint64_t download_first_get_submit_us, download_last_body_us;
} xqc_mini_cli_user_conn_t;
typedef struct {
    xqc_mini_cli_ctx_t ctx; xqc_mini_cli_args_t *args;
    xqc_mini_cli_user_conn_t *connection; int api_mode, connect_started;
} mini_backend_client_t;
typedef struct {
    size_t total_bytes; uint32_t total_fragments, completed_fragments;
    uint64_t elapsed_us, first_get_submit_us, last_body_us, get_received_bytes, get_expected_bytes;
} mini_backend_transfer_result_t;

static uint64_t now_us = 1000;
static int attached, sealed, confirmed, failed, checkpoints, observed_stream_ends;
static int streams_at_checkpoint, h3_error, checkpoint_error, arm_error, launch_error;
static int synchronous_abort_close, stream_notifications, scenario;
static char trace[128][40]; static unsigned trace_count;
static mini_backend_client_t *running;
enum { NORMAL, CLOSE_SAME_TURN, PREMATURE_CLOSE, IO_ERROR, TRANSPORT_ERROR, LOOP_ERROR, CLOSE_COMPLETE, CLOSE_NO_IO };

static uint64_t xqc_now(void) { return ++now_us; }
static uint64_t xqc_observer_clock_us(void) { return ++now_us; }
static uint64_t xqc_observer_elapsed_us(uint64_t a, uint64_t b) { return b >= a ? b-a : 0; }
static void note(const char *name) { assert(trace_count < 128); snprintf(trace[trace_count++], 40, "%s", name); }
static void xqc_observer_phase(xqc_engine_t *e, const xqc_cid_t *id,
    const char *name, uint64_t bytes, unsigned depth, uint64_t cost)
{
    (void)e; (void)id; (void)bytes; (void)depth; (void)cost;
    if (!attached || sealed) return;
    note(name); confirmed += !strcmp(name,"client_confirmed"); failed += !strcmp(name,"client_failed");
}
static void xqc_observer_sample(xqc_engine_t *e, const xqc_cid_t *id,
    const char *kind, uint64_t a, uint64_t b, uint64_t c)
{ (void)e;(void)id;(void)kind;(void)a;(void)b;(void)c; }
static int xqc_observer_checkpoint_metadata(xqc_engine_t *e, const xqc_cid_t *id, char *b, size_t n)
{ (void)e;(void)id; assert(attached); sealed=1; snprintf(b,n,"fixture"); return 0; }
static int netins_telemetry_checkpoint(void *writer, const char *metadata)
{
    (void)writer;(void)metadata; note("checkpoint"); checkpoints++;
    streams_at_checkpoint=observed_stream_ends; return checkpoint_error;
}
static void netins_telemetry_close(void *writer) { (void)writer; note("writer_close"); }
static void xqc_observer_detach(xqc_engine_t *e, const xqc_cid_t *id)
{ (void)e;(void)id; attached=0; note("detach"); }
static void event_free(struct event *e) { (void)e; }
static int event_del(struct event *e) { (void)e; return 0; }
static int event_add(struct event *e, const struct timeval *tv) { (void)e;(void)tv; return 0; }
static int event_base_loopbreak(struct event_base *e) { (void)e; return 0; }
static int xqc_h3_conn_get_errno(xqc_h3_conn_t *e) { (void)e; return h3_error; }
static void xqc_mini_cli_diag_maybe_log(xqc_mini_cli_user_conn_t *c,const char *why,int force)
{ (void)c;(void)why;(void)force; }
static void xqc_mini_stream_poc_client_on_connection_close(xqc_mini_cli_user_conn_t *c) { (void)c; }
static void mini_stream_backend_on_connection_close(void *c,int err) { (void)c;(void)err; stream_notifications++; }
static void xqc_mini_cli_stop_async_writer(xqc_mini_cli_user_conn_t *c) { (void)c; }
static int close(int fd) { (void)fd; return 0; }
static void xqc_mini_cli_observer_extra(void *c) { (void)c; }
static int mini_get_task_id_valid(const char *id) { return id && *id; }
static int mini_backend_client_arm_timeout(struct event_base *b,xqc_usec_t a,uint32_t n,struct event **e)
{ (void)b;(void)a;(void)n;*e=NULL;return arm_error; }
static void mini_backend_client_disarm_timeout(struct event **e) { (void)e; }
static int xqc_mini_cli_paths_ready_for_requests(xqc_mini_cli_user_conn_t *c) { (void)c;return 1; }
static void xqc_mini_cli_schedule_path_retry(xqc_mini_cli_user_conn_t *c,xqc_usec_t d) { (void)c;(void)d; }

int mini_observer_finish(mini_observer_t *);
void mini_observer_stop(mini_observer_t **);
void mini_observer_resume(mini_observer_t *);
void xqc_mini_cli_maybe_finish_transfer(xqc_mini_cli_user_conn_t *);
void xqc_mini_cli_set_transfer_error(xqc_mini_cli_user_conn_t *,int);
int xqc_mini_cli_finish_transfer_observation(xqc_mini_cli_user_conn_t *,int);
int xqc_mini_cli_h3_conn_close_notify(xqc_h3_conn_t *,const xqc_cid_t *,void *);

static mini_observer_t *mini_observer_start(xqc_engine_t *e,const xqc_cid_t *cid,
    struct event_base *b,const netins_udp_observer_config_t *config,const char *endpoint,
    void (*extra)(void *),void *data)
{
    (void)b;(void)endpoint;
    if (!config->enabled) return NULL;
    mini_observer_t *o=calloc(1,sizeof(*o)); assert(o);
    o->engine=e;o->cid=*cid;o->extra=extra;o->extra_data=data;o->interval_us=100000;
    attached=1;sealed=0;observed_stream_ends=0;return o;
}
static int xqc_mini_cli_prepare_transfer(xqc_mini_cli_user_conn_t *c,REQUEST_METHOD method,
    const char *local,const char *remote,int concurrency)
{
    (void)local;(void)remote;(void)concurrency;
    c->ctx->args->req_cfg.method=method;c->transfer_active=1;c->transfer_finished=0;
    c->transfer_error_code=0;c->transfer_io_complete=0;c->transfer_start_time=xqc_now();
    c->transfer_end_time=0;c->target_requests=25;c->active_requests=8;c->completed_requests=0;
    c->download_expected_bytes=104857600;c->download_total_bytes=c->download_received_bytes=0;
    return 0;
}
static int xqc_mini_cli_launch_requests(xqc_mini_cli_user_conn_t *c)
{
    if (c->observer && c->observer->finished) {
        sealed=0;observed_stream_ends=0;mini_observer_resume(c->observer);
    }
    return launch_error;
}
static int xqc_h3_conn_close(xqc_engine_t *e,const xqc_cid_t *cid)
{
    (void)e;
    if (synchronous_abort_close && !running->connection->connection_closed)
        xqc_mini_cli_h3_conn_close_notify(NULL,cid,running->connection);
    return 0;
}
static int event_base_loop(struct event_base *base,int flags)
{
    (void)base;(void)flags;
    xqc_mini_cli_user_conn_t *c=running->connection;
    if (scenario==LOOP_ERROR) return 1;
    if (scenario==PREMATURE_CLOSE) {
        xqc_mini_cli_h3_conn_close_notify(NULL,&c->cid,c);return 0;
    }
    c->completed_requests=c->target_requests;c->active_requests=0;
    c->download_total_bytes=c->download_received_bytes=c->download_expected_bytes;
    c->download_first_body_time=c->download_first_get_submit_us=100;
    c->download_last_body_time=c->download_last_body_us=500;
    c->upload_total_bytes=104857600;c->upload_start_time=100;c->upload_end_time=500;
    c->transfer_io_complete=scenario!=IO_ERROR && scenario!=CLOSE_NO_IO;
    if (scenario==IO_ERROR) xqc_mini_cli_set_transfer_error(c,EIO);
    if (scenario==TRANSPORT_ERROR) h3_error=71;
    /* Real xqc_stream_destroy calls the application close callback first,
     * then emits stream_end. Finishing the checkpoint in maybe_finish would
     * lose this last record, so verify that ordering too. */
    if (scenario!=TRANSPORT_ERROR && scenario!=CLOSE_COMPLETE)
        xqc_mini_cli_maybe_finish_transfer(c);
    if (attached && !sealed) { observed_stream_ends=c->target_requests; note("stream_end"); }
    if (scenario==CLOSE_SAME_TURN || scenario==TRANSPORT_ERROR || scenario==CLOSE_COMPLETE || scenario==CLOSE_NO_IO)
        xqc_mini_cli_h3_conn_close_notify(NULL,&c->cid,c);
    return 0;
}

#include "client_observer_lifecycle_slices.c"

static xqc_mini_cli_args_t args;
static xqc_mini_cli_user_conn_t connection;
static mini_backend_client_t client;
static void setup(int mode,int observer)
{
    memset(&args,0,sizeof(args));memset(&connection,0,sizeof(connection));memset(&client,0,sizeof(client));
    attached=sealed=confirmed=failed=checkpoints=observed_stream_ends=streams_at_checkpoint=0;
    h3_error=checkpoint_error=arm_error=launch_error=stream_notifications=0;
    synchronous_abort_close=1;trace_count=0;scenario=mode;
    args.defer_transfer_setup=1;args.observer_config.enabled=observer;
    client.args=&args;client.ctx.args=&args;client.ctx.engine=(void *)1;client.ctx.eb=(void *)1;
    client.connection=&connection;client.api_mode=client.connect_started=1;
    connection.ctx=&client.ctx;connection.handshake_finished=1;connection.download_fd=-1;running=&client;
}
static int transfer(const char *id,int direction)
{
    mini_backend_transfer_result_t result;
    return mini_backend_client_transfer(&client,direction,"local","/file",1000,&result,id);
}
static int index_of(const char *name)
{ for (unsigned i=0;i<trace_count;i++) if (!strcmp(trace[i],name)) return (int)i;return -1; }
static void check_success(const char *name)
{
    assert(confirmed==1 && failed==0 && checkpoints==1);
    assert(streams_at_checkpoint==25);
    assert(index_of("stream_end")<index_of("client_confirmed"));
    assert(index_of("client_confirmed")<index_of("task_end"));
    assert(index_of("task_end")<index_of("checkpoint"));
    mini_observer_stop(&connection.observer);puts(name);
}
int main(void)
{
    setup(NORMAL,1);assert(transfer(NULL,MINI_BACKEND_TRANSFER_GET)==0);
    xqc_mini_cli_h3_conn_close_notify(NULL,&connection.cid,&connection);
    check_success("PASS normal completion then close: one terminal event and checkpoint");
    setup(CLOSE_SAME_TURN,1);assert(transfer(NULL,MINI_BACKEND_TRANSFER_GET)==0);
    check_success("PASS completion and connection close in one engine turn retain stream_end and client_confirmed");
    setup(CLOSE_COMPLETE,1);assert(transfer(NULL,MINI_BACKEND_TRANSFER_GET)==0);
    check_success("PASS complete I/O and requests are recognized before a normal close");
    setup(PREMATURE_CLOSE,1);assert(transfer(NULL,MINI_BACKEND_TRANSFER_GET)==ECONNRESET);
    assert(failed==1 && confirmed==0 && checkpoints==1);assert(index_of("client_failed")<index_of("task_end"));
    puts("PASS premature close is a recorded failure, never a success");
    setup(TRANSPORT_ERROR,1);assert(transfer(NULL,MINI_BACKEND_TRANSFER_GET)==71);
    assert(failed==1 && confirmed==0 && checkpoints==1);
    puts("PASS transport error is not promoted to success even with a complete payload");
    setup(IO_ERROR,1);assert(transfer(NULL,MINI_BACKEND_TRANSFER_GET)==EIO);
    assert(failed==1 && confirmed==0 && checkpoints==1);mini_observer_stop(&connection.observer);
    puts("PASS file I/O failure remains a failure");
    setup(CLOSE_NO_IO,1);assert(transfer(NULL,MINI_BACKEND_TRANSFER_GET)==ECONNRESET);
    assert(failed==1 && confirmed==0 && checkpoints==1);
    puts("PASS closed requests without completed file I/O are never a success");
    setup(NORMAL,1);launch_error=-1;assert(transfer(NULL,MINI_BACKEND_TRANSFER_GET)==EIO);
    assert(failed==1 && confirmed==0 && checkpoints==1);puts("PASS synchronous close during launch failure retains client_failed");
    setup(NORMAL,1);launch_error=-1;synchronous_abort_close=0;
    assert(transfer(NULL,MINI_BACKEND_TRANSFER_GET)==EIO);
    assert(failed==1 && confirmed==0 && checkpoints==1);mini_observer_stop(&connection.observer);
    puts("PASS asynchronous launch failure records its terminal result before API return");
    setup(NORMAL,1);arm_error=ETIMEDOUT;assert(transfer(NULL,MINI_BACKEND_TRANSFER_GET)==ETIMEDOUT);
    assert(failed==1 && confirmed==0 && checkpoints==1);puts("PASS timeout records exactly one failure before observer shutdown");
    setup(LOOP_ERROR,1);assert(transfer(NULL,MINI_BACKEND_TRANSFER_GET)==EIO);
    assert(failed==1 && confirmed==0 && checkpoints==1);puts("PASS event-loop failure records exactly one failure");
    setup(NORMAL,1);
    for (int i=0;i<3;i++) {
        char id[16];snprintf(id,sizeof(id),"task-%d",i);
        assert(transfer(id,MINI_BACKEND_TRANSFER_GET)==0);
        assert(connection.get_confirmation_result==EINPROGRESS);
        connection.get_task_needs_confirmation=0;connection.get_confirmation_result=0;
    }
    assert(confirmed==3 && failed==0 && checkpoints==3);mini_observer_stop(&connection.observer);
    puts("PASS three transfers on one connection reset terminal state per task");
    setup(NORMAL,0);assert(transfer(NULL,MINI_BACKEND_TRANSFER_GET)==0);
    assert(!confirmed && !failed && !checkpoints);puts("PASS disabled observation preserves successful transfer behavior");
    setup(NORMAL,1);checkpoint_error=1;assert(transfer("bad-checkpoint",MINI_BACKEND_TRANSFER_GET)==0);
    assert(connection.get_confirmation_result==EIO && connection.transfer_observer_checkpoint_result!=0);
    assert(checkpoints==1);xqc_mini_cli_h3_conn_close_notify(NULL,&connection.cid,&connection);
    assert(checkpoints==1);puts("PASS checkpoint errors are cached and never silently retried as success");
    setup(NORMAL,1);assert(transfer(NULL,MINI_BACKEND_TRANSFER_POST)==0);
    check_success("PASS POST completion uses the same terminal ordering");
    setup(NORMAL,0);connection.stream_api_state=(void *)1;
    xqc_mini_cli_h3_conn_close_notify(NULL,&connection.cid,&connection);
    assert(stream_notifications==1 && !confirmed && !failed);
    puts("PASS stream API close does not create a file-transfer terminal event");
    return 0;
}
