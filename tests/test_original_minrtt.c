/*
 * Standalone scheduler regression test; no sockets or TLS connection required.
 * From the repository root on Linux (with OpenSSL development headers):
 * cc -std=gnu11 -DXQC_DISABLE_LOG -ffunction-sections -fdata-sections -I. -Iinclude \
 *   tests/test_original_minrtt.c src/transport/scheduler/xqc_scheduler_original_minrtt.c \
 *   src/transport/scheduler/xqc_scheduler_minrtt.c \
 *   src/transport/scheduler/xqc_scheduler_common.c src/transport/xqc_multipath.c \
 *   -Wl,--gc-sections -o /tmp/test_original_minrtt
 * /tmp/test_original_minrtt
 *
 * Use the real schedulers, quality classifier and extra-probe implementation.
 * Stub only the clock, probe sender and send-controller inputs.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "src/transport/scheduler/xqc_scheduler_original_minrtt.h"
#include "src/transport/scheduler/xqc_scheduler_common.h"
#include "src/transport/xqc_conn.h"
#include "src/transport/xqc_send_ctl.h"

static xqc_usec_t test_now = 1000000;
static unsigned test_probes;

static xqc_usec_t
test_clock(void)
{
    return test_now;
}

xqc_timestamp_pt xqc_monotonic_timestamp = test_clock;

xqc_int_t
xqc_path_send_ping_to_probe(xqc_path_ctx_t *path, xqc_pkt_num_space_t pns,
    xqc_path_specified_flag_t flag)
{
    assert(path->path_state == XQC_PATH_STATE_ACTIVE);
    assert(pns == XQC_PNS_APP_DATA);
    assert(flag == XQC_PATH_SPECIFIED_BY_PQP);
    test_probes++;
    return XQC_OK;
}

xqc_usec_t
xqc_send_ctl_get_srtt(xqc_send_ctl_t *ctl)
{
    return ctl->ctl_srtt;
}

uint64_t
xqc_send_ctl_get_est_bw(xqc_send_ctl_t *ctl)
{
    (void)ctl;
    return 1000000;
}

xqc_usec_t
xqc_conn_get_min_srtt(xqc_connection_t *conn, xqc_bool_t available_only)
{
    (void)conn;
    (void)available_only;
    return 20000;
}

int
xqc_send_ctl_can_send(xqc_send_ctl_t *ctl, xqc_packet_out_t *packet,
    uint32_t scheduled)
{
    (void)packet;
    (void)scheduled;
    return !ctl->ctl_is_cwnd_limited;
}

xqc_bool_t
xqc_send_packet_cwnd_allows(xqc_send_ctl_t *ctl, xqc_packet_out_t *packet,
    uint32_t scheduled, xqc_usec_t now)
{
    (void)now;
    return xqc_send_ctl_can_send(ctl, packet, scheduled);
}

unsigned
xqc_send_ctl_calc_effective_cwnd(uint64_t raw, uint64_t sndbuf)
{
    (void)sndbuf;
    return (unsigned)raw;
}

typedef struct {
    xqc_connection_t conn;
    xqc_path_ctx_t paths[2];
    xqc_send_ctl_t controls[2];
    xqc_packet_out_t packet;
} fixture_t;

static void
setup(fixture_t *f)
{
    memset(f, 0, sizeof(*f));
    f->conn.enable_multipath = 1;
    f->conn.conn_state = XQC_CONN_STATE_ESTABED;
    f->conn.scheduler_callback = &xqc_original_minrtt_scheduler_cb;
    f->conn.conn_settings.scheduler_callback = xqc_original_minrtt_scheduler_cb;
    f->conn.conn_settings.scheduler_params.rtt_us_thr_high = 2000000;
    f->conn.conn_settings.scheduler_params.rtt_us_thr_low = 500000;
    f->conn.conn_settings.scheduler_params.bw_Bps_thr = 375000;
    f->conn.conn_settings.scheduler_params.loss_percent_thr_high = 30;
    f->conn.conn_settings.scheduler_params.loss_percent_thr_low = 10;
    xqc_original_minrtt_apply_settings(&f->conn.conn_settings);
    xqc_init_list_head(&f->conn.conn_paths_list);
    for (unsigned i = 0; i < 2; i++) {
        xqc_path_ctx_t *path = &f->paths[i];
        path->parent_conn = &f->conn;
        path->path_id = i;
        path->path_state = XQC_PATH_STATE_ACTIVE;
        path->app_path_status = XQC_APP_PATH_STATUS_AVAILABLE;
        path->path_send_ctl = &f->controls[i];
        f->controls[i].ctl_conn = &f->conn;
        f->controls[i].ctl_path = path;
        f->controls[i].ctl_srtt = i ? 40000 : 20000;
        xqc_list_add_tail(&path->path_list, &f->conn.conn_paths_list);
    }
    f->packet.po_frame_types = XQC_FRAME_BIT_STREAM;
    f->packet.po_used_size = 1200;
}

static xqc_path_ctx_t *
pick(fixture_t *f, int check_cwnd, int reinject, xqc_bool_t *blocked)
{
    return f->conn.scheduler_callback->xqc_scheduler_get_path(NULL,
        &f->conn, &f->packet, check_cwnd, reinject, blocked);
}

static void
test_baseline_settings(void)
{
    xqc_conn_settings_t settings = {0};
    settings.scheduler_callback = xqc_original_minrtt_scheduler_cb;
    settings.initial_rtt = 25000;
    settings.scheduler_params.pto_cnt_thr = 3;
    settings.standby_path_probe_timeout = 1000;
    xqc_original_minrtt_apply_settings(&settings);
    assert(settings.initial_rtt == 250000);
    assert(settings.scheduler_params.pto_cnt_thr == 2);
    assert(settings.standby_path_probe_timeout == 0);

    settings.scheduler_callback = xqc_minrtt_scheduler_cb;
    settings.initial_rtt = 25000;
    settings.scheduler_params.pto_cnt_thr = 3;
    settings.standby_path_probe_timeout = 1000;
    xqc_conn_settings_t before = settings;
    xqc_original_minrtt_apply_settings(&settings);
    assert(memcmp(&before, &settings, sizeof(settings)) == 0);
}

static void
test_selection(void)
{
    fixture_t f;
    xqc_bool_t blocked = XQC_TRUE;
    setup(&f);
    assert(pick(&f, 1, 0, &blocked) == &f.paths[0]);
    assert(!blocked);

    f.controls[0].ctl_pto_count = 2;
    assert(pick(&f, 1, 0, &blocked) == &f.paths[1]);
    f.conn.scheduler_callback = &xqc_minrtt_scheduler_cb;
    f.conn.conn_settings.scheduler_params.pto_cnt_thr = 3;
    assert(pick(&f, 1, 0, &blocked) == &f.paths[0]);

    setup(&f);
    f.paths[0].app_path_status = XQC_APP_PATH_STATUS_FROZEN;
    assert(pick(&f, 1, 0, &blocked) == &f.paths[1]);
    f.paths[0].app_path_status = XQC_APP_PATH_STATUS_AVAILABLE;
    f.paths[0].path_state = XQC_PATH_STATE_INIT;
    assert(pick(&f, 1, 0, &blocked) == &f.paths[1]);

    setup(&f);
    f.packet.po_path_id = 0;
    assert(pick(&f, 1, 1, &blocked) == &f.paths[1]);
    f.controls[0].ctl_is_cwnd_limited = 1;
    assert(pick(&f, 1, 0, &blocked) == &f.paths[1]);
    f.controls[1].ctl_is_cwnd_limited = 1;
    assert(pick(&f, 1, 0, &blocked) == NULL);
    assert(blocked);
    assert(pick(&f, 0, 0, &blocked) == &f.paths[0]);
    assert(!blocked);

    setup(&f);
    f.controls[1].ctl_srtt = 20000;
    assert(pick(&f, 1, 0, NULL) == &f.paths[0]);
}

static void
test_extra_probe_isolation(void)
{
    fixture_t f;
    setup(&f);
    test_probes = 0;
    /* No bootstrap probe for original_minrtt, even without any RTT sample. */
    xqc_scheduler_handle_path_probe(&f.conn);
    assert(test_probes == 0);
    f.conn.conn_settings.standby_path_probe_timeout = 1000;
    xqc_scheduler_handle_path_probe(&f.conn);
    assert(test_probes == 0);

    /* The existing scheduler keeps its bootstrap and idle probe behavior. */
    f.conn.scheduler_callback = &xqc_minrtt_scheduler_cb;
    xqc_scheduler_handle_path_probe(&f.conn);
    assert(test_probes == 2);
    for (unsigned i = 0; i < 2; i++) {
        f.controls[i].ctl_first_rtt_sample_time = 1;
    }
    test_now += 199999;
    xqc_scheduler_handle_path_probe(&f.conn);
    assert(test_probes == 2);
    test_now++;
    xqc_scheduler_handle_path_probe(&f.conn);
    assert(test_probes == 4);
}

int
main(void)
{
    test_baseline_settings();
    test_selection();
    test_extra_probe_isolation();
    puts("original_minrtt: baseline, selection and probe isolation PASS");
    return 0;
}
