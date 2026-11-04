#include "src/transport/xqc_observer.h"
#include "src/transport/xqc_conn.h"
#include "src/transport/xqc_stream.h"
#include "src/transport/xqc_send_ctl.h"
#include "src/transport/xqc_send_queue.h"
#include "src/transport/xqc_packet_out.h"
#include "src/transport/xqc_engine.h"
#include "src/common/xqc_time.h"
#include <xquic/xqc_observer_clock.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdarg.h>
#include <netdb.h>
#include <string.h>
#include "src/common/xqc_id_hash.h"
#include "src/transport/scheduler/xqc_scheduler_minrtt.h"

static xqc_connection_t *
observer_conn(xqc_engine_t *engine, const xqc_cid_t *cid)
{
    return engine && cid ? xqc_engine_conns_hash_find(engine, cid, 's') : NULL;
}

static xqc_stream_t *
observer_stream(xqc_connection_t *conn, uint64_t id)
{
    return conn ? xqc_id_hash_find(conn->streams_hash, id) : NULL;
}

static uint64_t
observer_now(xqc_observer_t *o)
{
    uint64_t now = xqc_observer_clock_us();
    if (!now && o) { o->invalid = 1; }
    return now;
}

/* All strings inserted into JSON below are fixed vocabulary or validated
 * attempt tokens. Addresses returned by getnameinfo use numeric forms. */
static int
observer_token(const char *s)
{
    if (!s || !*s || strlen(s) >= 192) { return 0; }
    for (; *s; ++s) {
        if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z')
            || (*s >= '0' && *s <= '9') || *s == '_' || *s == '-' || *s == '.')) { return 0; }
    }
    return 1;
}

static void
observer_emit(xqc_connection_t *conn, int event, const char *kind, const char *body)
{
    xqc_observer_t *o = conn->observer;
    if (!o || !o->sink) { return; }
    char *record = o->record;
    uint64_t now = observer_now(o), realtime = xqc_observer_realtime_us();
    if (!realtime) { o->invalid = 1; }
    uint64_t seq = event ? ++o->event_seq : ++o->seq;
    int n = snprintf(record, sizeof(o->record),
        "{\"observer_revision\":2,\"clock_domain\":\"CLOCK_MONOTONIC\",\"schema_version\":%d,\"schema_family\":\"%s\",\"sample_kind\":\"%s\","
        "\"sample_seq\":%"PRIu64",\"actual_mono_us\":%"PRIu64",\"realtime_us\":%"PRIu64","
        "\"conn_uid\":\"%s\",\"attempt_id\":\"%s\",\"transfer_id\":\"%s\","
        "\"endpoint\":\"%s\",\"direction\":\"%s\",\"phase\":\"task\","
        "\"observer_enabled\":true,\"observer_valid\":%s,\"dropped_records\":%"PRIu64",\"overflow_count\":%"PRIu64",%s}",
        event ? 1 : 2, event ? "observer_events" : "transport", kind, seq,
        now, realtime,
        o->uid, o->attempt, o->attempt, o->endpoint, o->direction,
        o->invalid || o->dropped ? "false" : "true", o->dropped, o->overflow_count, body);
    if (n < 0 || (size_t)n >= sizeof(o->record) || o->sink(o->sink_data, event, record)) {
        o->dropped++; o->invalid = 1;
    }
}

void
xqc_observer_event(xqc_connection_t *conn, const char *kind, const char *body)
{
    if (conn && conn->observer) { observer_emit(conn, 1, kind, body); }
}

int
xqc_observer_attach(xqc_engine_t *engine, const xqc_cid_t *cid,
    const char *endpoint, const char *attempt, xqc_observer_sink_pt sink, void *data)
{
    xqc_connection_t *conn = observer_conn(engine, cid);
    if (!conn || conn->observer || !sink || !endpoint
        || (strcmp(endpoint, "board") && strcmp(endpoint, "server"))
        || (attempt && *attempt && !observer_token(attempt))) { return -1; }
    xqc_observer_t *o = calloc(1, sizeof(*o));
    if (!o) { return -1; }
    static uint64_t serial;
    uint64_t unique = __atomic_add_fetch(&serial, 1, __ATOMIC_RELAXED);
    snprintf(o->uid, sizeof(o->uid), "%s-%"PRIu64"-%"PRIu64,
        endpoint, (uint64_t)xqc_realtime_timestamp(), unique);
    snprintf(o->endpoint, sizeof(o->endpoint), "%s", endpoint);
    snprintf(o->attempt, sizeof(o->attempt), "%s", attempt ? attempt : "");
    o->sink = sink; o->sink_data = data;
    o->attached_us = observer_now(o);
    xqc_list_head_t *existing;
    xqc_list_for_each(existing, &conn->conn_all_streams) {
        xqc_stream_t *v = xqc_list_entry(existing, xqc_stream_t, all_stream_list);
        if (v->stream_type == XQC_CLI_BID) {
            if (!o->has_existing_streams || v->stream_id > o->existing_stream_max) { o->existing_stream_max = v->stream_id; }
            o->has_existing_streams = 1;
        }
    }
    o->primary_id = o->secondary_id = o->first_request_id = UINT64_MAX;
    conn->observer = o;
    return 0;
}

/* Integrate the previous task state before changing one stream contribution.
 * Binding coverage is explicit; unbound streams retain their own full history. */
static void
observer_aggregate(xqc_connection_t *conn, xqc_observer_stream_t *v, uint64_t now)
{
    xqc_observer_t *o = conn->observer;
    if (!o) { return; }
    if (!o->aggregate_last_us) { o->aggregate_last_us = now; o->aggregate_from_us = now; }
    if (now < o->aggregate_last_us) { o->invalid = 1; return; }
    uint64_t dt = now - o->aggregate_last_us;
    if (o->pending_streams) { o->any_hol_us = xqc_observer_add(o->any_hol_us, dt, &o->invalid); }
    if (dt && o->sum_b > UINT64_MAX / dt) { o->invalid = 1; o->sum_b_byte_us = UINT64_MAX; }
    else { o->sum_b_byte_us = xqc_observer_add(o->sum_b_byte_us, o->sum_b * dt, &o->invalid); }
    o->aggregate_last_us = now;
    if (!v) { return; }
    if (v->counted) {
        o->sum_b -= v->counted_b;
        if (v->counted_b) { o->pending_streams--; }
    }
    v->counted = !v->state.ended && (v->bound ?
        ((v->direction == 1 && !strcmp(o->endpoint, "board")) ||
         (v->direction == 2 && !strcmp(o->endpoint, "server"))) :
        ((!strcmp(o->endpoint, "board") && strcmp(o->direction, "post")) ||
         (!strcmp(o->endpoint, "server") && strcmp(o->direction, "get"))));
    v->counted_b = v->counted ? v->state.b : 0;
    if (v->counted) {
        o->sum_b = xqc_observer_add(o->sum_b, v->counted_b, &o->invalid);
        if (v->counted_b) { o->pending_streams++; }
    }
    if (o->sum_b > o->sum_b_peak) { o->sum_b_peak = o->sum_b; }
}

static void
observer_stream_record(xqc_stream_t *stream, const char *kind)
{
    xqc_connection_t *conn = stream->stream_conn;
    xqc_observer_stream_t *v = stream->observer;
    if (!v || !stream->stream_conn->observer) { return; }
    xqc_observer_state_t *s = &v->state;
    uint64_t now = observer_now(conn->observer);
    xqc_observer_settle(s, now);
    if (s->invalid) { conn->observer->invalid = 1; }
    char body[2304];
    snprintf(body, sizeof(body),
        "\"stream_id\":%"PRIu64",\"bound\":%s,\"stream_valid\":%s,"
        "\"end_reason\":\"%s\",\"censored\":%s,"
        "\"C\":%"PRIu64",\"N\":%"PRIu64",\"reorder_pending_bytes\":%"PRIu64","
        "\"readable_unread_bytes\":%"PRIu64",\"hol_pending_us\":%"PRIu64","
        "\"hol_exposed_us\":%"PRIu64",\"reorder_byte_us\":%"PRIu64","
        "\"reorder_pending_peak_bytes\":%"PRIu64",\"interval_peak_bytes\":%"PRIu64","
        "\"hol_episode_count\":%"PRIu64",\"current_episode_age_us\":%"PRIu64","
        "\"request_index\":%"PRIu64",\"file_offset\":%"PRIu64",\"fragment_length\":%"PRIu64","
        "\"body_received_bytes\":%"PRIu64",\"first_body_us\":%"PRIu64",\"last_body_us\":%"PRIu64","
        "\"stream_send_limit_bytes\":%"PRIu64",\"stream_send_offset_bytes\":%"PRIu64","
        "\"observed_from_us\":%"PRIu64",\"interval_start_us\":%"PRIu64",\"interval_end_us\":%"PRIu64","
        "\"transport_sample_seq\":%"PRIu64","
        "\"interval_count\":%u,\"interval_capacity\":%u,\"interval_peak_count\":%u,"
        "\"interval_limit\":%u,\"interval_error\":\"%s\","
        "\"hol_bytes_basis\":\"target_request_STREAM_bytes_including_H3_framing\"",
        v->id, v->bound ? "true" : "false", s->invalid ? "false" : "true",
        v->end_reason ? v->end_reason : "", s->ended && s->b ? "true" : "false",
        s->c, s->n, s->b, s->a, s->pending_us, s->exposed_us, s->byte_us,
        s->peak, s->interval_peak, s->episodes,
        s->b && !s->ended ? now - s->episode_start : 0,
        v->request_index, v->file_offset, v->fragment_length,
        v->body_bytes, v->first_body_us, v->last_body_us,
        stream->stream_flow_ctl.fc_max_stream_data_can_send, stream->stream_send_offset,
        v->observed_from_us,
        v->interval_start_us, now, stream->stream_conn->observer->seq,
        s->count, s->capacity, s->peak_count, XQC_OBSERVER_MAX_INTERVALS,
        xqc_observer_state_error(s));
    xqc_observer_event(stream->stream_conn, kind, body);
    s->interval_peak = s->b;
    v->interval_start_us = now;
}

void
xqc_observer_stream_update(xqc_stream_t *stream, uint64_t begin, uint64_t end)
{
    xqc_connection_t *conn = stream->stream_conn;
    if (!conn->observer || stream->stream_type != XQC_CLI_BID) { return; }
    if (!stream->observer) {
        if (conn->observer->stream_count >= XQC_OBSERVER_MAX_STREAMS) {
            conn->observer->invalid = 1; return;
        }
        stream->observer = calloc(1, sizeof(*stream->observer));
        if (!stream->observer) { conn->observer->invalid = 1; return; }
        stream->observer->id = stream->stream_id;
        stream->observer->observed_from_us = observer_now(conn->observer);
        if (conn->observer->has_existing_streams && stream->stream_id <= conn->observer->existing_stream_max) {
            conn->observer->aggregate_prebind_unobserved = 1;
        }
        conn->observer->stream_count++;
        conn->observer->unbound_observed++;
        stream->observer->interval_start_us = observer_now(conn->observer);
    }
    xqc_observer_state_t *s = &stream->observer->state;
    uint64_t now = observer_now(conn->observer);
    int transition = xqc_observer_update(s, begin, end,
        stream->stream_data_in.merged_offset_end,
        stream->stream_data_in.next_read_offset, now);
    observer_aggregate(conn, stream->observer, now);
    if (s->invalid && !stream->observer->invalid_reported) {
        stream->observer->invalid_reported = 1;
        conn->observer->overflow_count++;
        conn->observer->invalid = 1;
        char detail[512];
        snprintf(detail, sizeof(detail),
            "\"reason\":\"%s\",\"stream_id\":%"PRIu64",\"interval_count\":%u,"
            "\"interval_capacity\":%u,\"interval_required\":%u,\"interval_peak_count\":%u,"
            "\"interval_limit\":%u,\"C\":%"PRIu64",\"N\":%"PRIu64,
            xqc_observer_state_error(s), stream->stream_id, s->count, s->capacity,
            s->required_count, s->peak_count, XQC_OBSERVER_MAX_INTERVALS, s->c, s->n);
        xqc_observer_event(conn, "observer_error", detail);
    }
    if (transition) {
        char body[512];
        snprintf(body, sizeof(body),
            "\"stream_id\":%"PRIu64",\"start_us\":%"PRIu64",\"end_us\":%"PRIu64","
            "\"duration_us\":%"PRIu64",\"peak_bytes\":%"PRIu64",\"censored\":false",
            stream->stream_id, s->episode_start, now,
            now - s->episode_start, s->episode_peak);
        xqc_observer_event(conn, transition > 0 ? "hol_start" : "hol_episode", body);
    }
}

void
xqc_observer_stream_end(xqc_stream_t *stream, const char *reason)
{
    xqc_connection_t *conn = stream->stream_conn;
    if (!stream->observer || stream->observer->state.ended) { return; }
    xqc_observer_state_t *s = &stream->observer->state;
    uint64_t now = observer_now(conn->observer);
    xqc_observer_settle(s, now);
    if (s->b) {
        char body[512];
        snprintf(body, sizeof(body),
            "\"stream_id\":%"PRIu64",\"start_us\":%"PRIu64",\"end_us\":%"PRIu64","
            "\"duration_us\":%"PRIu64",\"peak_bytes\":%"PRIu64","
            "\"censored\":true,\"end_reason\":\"%s\"",
            stream->stream_id, s->episode_start, now,
            now - s->episode_start, s->episode_peak, reason);
        xqc_observer_event(stream->stream_conn, "hol_episode", body);
        s->censored_episodes++;
    }
    stream->observer->end_reason = reason;
    s->ended = 1;
    observer_stream_record(stream, "stream_end");
    observer_aggregate(stream->stream_conn, stream->observer, now);
}

void
xqc_observer_bind(xqc_engine_t *engine, const xqc_cid_t *cid, uint64_t id,
    const char *attempt, int direction, uint64_t index, uint64_t offset,
    uint64_t length, uint64_t concurrency, uint64_t range_bytes)
{
    xqc_connection_t *conn = observer_conn(engine, cid);
    if (!conn || !conn->observer) { return; }
    xqc_observer_t *o = conn->observer;
    if (!observer_token(attempt) || (direction != 1 && direction != 2)) {
        o->invalid = 1;
        xqc_observer_event(conn, "observer_error", "\"reason\":\"attempt_binding_conflict\"");
        return;
    }
    int task_start = !*o->direction || strcmp(o->attempt, attempt);
    xqc_stream_t *stream = observer_stream(conn, id);
    if (!stream) {
        o->invalid = 1;
        xqc_observer_event(conn, "observer_error", "\"reason\":\"binding_stream_not_found\"");
        return;
    }
    xqc_observer_stream_t *v = stream->observer;
    const char *conflict = NULL;
    if (!task_start && strcmp(o->direction, direction == 1 ? "get" : "post")) {
        conflict = "direction";
    }
    if (v && v->bound) {
        if (task_start) { conflict = "attempt_id"; }
        else if (v->direction != direction) { conflict = "direction"; }
        else if (v->request_index != index) { conflict = "request_index"; }
        else if (v->file_offset != offset) { conflict = "file_offset"; }
        else if (v->fragment_length != length
            && !(direction == 1 && index == 0 && offset == 0
                 && v->fragment_length == 0 && length > 0 && !v->state.ended
                 && !v->body_bytes && !v->first_body_us && !v->last_body_us)) {
            conflict = "fragment_length";
        }
    }
    if (conflict) {
        char detail[1024];
        snprintf(detail, sizeof(detail),
            "\"reason\":\"stream_binding_conflict_%s\",\"stream_id\":%"PRIu64","
            "\"requested_attempt_id\":\"%s\",\"requested_direction\":%d,"
            "\"requested_index\":%"PRIu64",\"requested_offset\":%"PRIu64","
            "\"requested_length\":%"PRIu64",\"previous_direction\":%d,"
            "\"previous_index\":%"PRIu64",\"previous_offset\":%"PRIu64","
            "\"previous_length\":%"PRIu64,
            conflict, id, attempt, direction, index, offset, length,
            v ? v->direction : 0, v ? v->request_index : 0,
            v ? v->file_offset : 0, v ? v->fragment_length : 0);
        o->invalid = 1;
        xqc_observer_event(conn, "observer_error", detail);
        return;
    }
    if (task_start && *o->direction) {
        xqc_list_head_t *pos;
        xqc_list_for_each(pos, &conn->conn_all_streams) {
            xqc_stream_t *old = xqc_list_entry(pos, xqc_stream_t, all_stream_list);
            xqc_observer_stream_t *previous = old->observer;
            if (!previous || !previous->bound || previous->state.ended || old->stream_id == id) { continue; }
            int receiver = (previous->direction == 1 && !strcmp(o->endpoint, "board")) ||
                           (previous->direction == 2 && !strcmp(o->endpoint, "server"));
            if (previous->state.b || (receiver ? old->stream_state_recv < XQC_RECV_STREAM_ST_DATA_RECVD
                                              : old->stream_state_send < XQC_SEND_STREAM_ST_DATA_SENT)) {
                o->invalid = 1;
                xqc_observer_event(conn, "observer_error", "\"reason\":\"overlapping_attempts\"");
                return;
            }
        }
        o->aggregate_prebind_unobserved = 1;
        o->any_hol_us = o->sum_b_peak = o->sum_b_byte_us = 0;
        o->aggregate_last_us = o->aggregate_from_us = observer_now(conn->observer);
        o->write_cost_us = o->write_samples = o->write_bytes = 0;
    }
    if (!task_start && index == 0 && o->first_request_id != UINT64_MAX && o->first_request_id != id) {
        o->invalid = 1;
        xqc_observer_event(conn, "observer_error", "\"reason\":\"attempt_reused_for_multiple_transfers\"");
        return;
    }
    if (task_start) { o->first_request_id = UINT64_MAX; }
    if (index == 0) { o->first_request_id = id; }
    snprintf(o->attempt, sizeof(o->attempt), "%s", attempt);
    snprintf(o->direction, sizeof(o->direction), "%s", direction == 1 ? "get" : "post");
    xqc_observer_stream_update(stream, 0, 0);
    v = stream->observer;
    if (!v) { return; }
    if (v->bound && v->fragment_length == length && v->file_offset == offset) { return; }
    if (!v->bound && o->unbound_observed) { o->unbound_observed--; }
    v->bound = 1; v->direction = direction; v->request_index = index;
    v->file_offset = offset; v->fragment_length = length;
    v->bind_us = observer_now(conn->observer);
    observer_aggregate(conn, v, v->bind_us);
    char body[640], concurrent[32] = "null";
    if (concurrency) { snprintf(concurrent, sizeof(concurrent), "%"PRIu64, concurrency); }
    snprintf(body, sizeof(body),
        "\"stream_id\":%"PRIu64",\"request_index\":%"PRIu64",\"file_offset\":%"PRIu64","
        "\"fragment_length\":%"PRIu64",\"actual_concurrency\":%s,\"range_bytes\":%"PRIu64","
        "\"bind_time_us\":%"PRIu64",\"payload_kind\":\"file\",\"prebind_unobserved\":%s",
        id, index, offset, length, concurrent, range_bytes, v->bind_us,
        o->has_existing_streams && id <= o->existing_stream_max ? "true" : "false");
    xqc_observer_event(conn, "stream_bind", body);
    if (task_start) { xqc_observer_sample(engine, cid, "start", v->bind_us, 0, 0); }
}

void
xqc_observer_body(xqc_engine_t *engine, const xqc_cid_t *cid, uint64_t id, uint64_t bytes)
{
    xqc_connection_t *conn = observer_conn(engine, cid);
    xqc_stream_t *stream = observer_stream(conn, id);
    if (!stream || !stream->observer || !stream->observer->bound || !bytes) { return; }
    xqc_observer_stream_t *v = stream->observer;
    uint64_t now = observer_now(conn->observer);
    if (!v->first_body_us) { v->first_body_us = now; }
    v->last_body_us = now;
    v->body_bytes = xqc_observer_add(v->body_bytes, bytes, &v->state.invalid);
}

static void
observer_addr(const unsigned char *address, socklen_t len, char *out, size_t cap)
{
    char host[NI_MAXHOST], service[NI_MAXSERV];
    if (!cap) { return; }
    out[0] = 0;
    if (len && !getnameinfo((const struct sockaddr *)address, len, host, sizeof(host),
        service, sizeof(service), NI_NUMERICHOST | NI_NUMERICSERV)) {
        size_t host_len = strlen(host), service_len = strlen(service);
        if (host_len + service_len + 4 > cap) { return; }
        out[0] = '[';
        memcpy(out + 1, host, host_len);
        out[host_len + 1] = ']';
        out[host_len + 2] = ':';
        memcpy(out + host_len + 3, service, service_len + 1);
    }
}

void
xqc_observer_path_interface(xqc_engine_t *engine, const xqc_cid_t *cid, uint64_t id, const char *name)
{
    xqc_connection_t *conn = observer_conn(engine, cid);
    if (!conn || !conn->observer || id == UINT64_MAX || !name || strlen(name) >= 64) { return; }
    for (const char *c = name; *c; ++c) {
        if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') ||
            (*c >= '0' && *c <= '9') || *c == '_' || *c == '-' || *c == '.')) { return; }
    }
    xqc_observer_t *o = conn->observer;
    unsigned slot = 8;
    for (unsigned i = 0; i < 8; ++i) {
        if (o->mappings[i].used && o->mappings[i].id == id) { slot = i; break; }
        if (!o->mappings[i].used && slot == 8) { slot = i; }
    }
    if (slot == 8) { o->invalid = 1; return; }
    o->mappings[slot].used = 1; o->mappings[slot].id = id;
    if (strcmp(o->mappings[slot].interface_name, name)) {
        snprintf(o->mappings[slot].interface_name, sizeof(o->mappings[slot].interface_name), "%s", name);
        o->mappings[slot].generation++;
    }
}

static void
observer_inflight_packets(xqc_connection_t *conn, uint64_t id, char *out, size_t cap)
{
    uint64_t count = 0, visited = 0;
    for (unsigned pns = 0; pns < XQC_PNS_N; ++pns) {
        xqc_list_head_t *pos;
        xqc_list_for_each(pos, &conn->conn_send_queue->sndq_unacked_packets[pns]) {
            const xqc_packet_out_t *packet = xqc_list_entry(pos, xqc_packet_out_t, po_list);
            if (++visited > 262144) { snprintf(out, cap, "null"); return; }
            if (packet->po_path_id == id && (packet->po_flag & XQC_POF_IN_FLIGHT)) { count++; }
        }
    }
    snprintf(out, cap, "%"PRIu64, count);
}

static size_t
observer_paths(xqc_connection_t *conn, char *out, size_t cap)
{
    size_t used = 0;
    xqc_list_head_t *pos;
    unsigned index = 0;
    xqc_list_for_each(pos, &conn->conn_paths_list) {
        xqc_path_ctx_t *p = xqc_list_entry(pos, xqc_path_ctx_t, path_list);
        xqc_send_ctl_t *ctl = p->path_send_ctl;
        if (!ctl) { continue; }
        uint64_t raw = ctl->ctl_cong_callback->xqc_cong_ctl_get_cwnd(ctl->ctl_cong);
        uint64_t eff = xqc_send_ctl_calc_effective_cwnd(raw, conn->conn_settings.so_sndbuf > 0 ? (uint64_t)conn->conn_settings.so_sndbuf : 0);
        uint64_t z = p->path_schedule_bytes, i = ctl->ctl_bytes_in_flight;
        uint64_t occupied = xqc_observer_add(i, z, &conn->observer->invalid);
        char local[160], peer[160];
        observer_addr(p->local_addr, p->local_addrlen, local, sizeof(local));
        observer_addr(p->peer_addr, p->peer_addrlen, peer, sizeof(peer));
        xqc_observer_t *o = conn->observer;
        unsigned slot = 8;
        for (unsigned m = 0; m < 8; ++m) {
            if (o->mappings[m].used && o->mappings[m].id == p->path_id) { slot = m; break; }
        }
        if (slot == 8) {
            for (unsigned m = 0; m < 8; ++m) { if (!o->mappings[m].used) { slot = m; break; } }
            if (slot != 8) { o->mappings[slot].used = 1; o->mappings[slot].id = p->path_id; }
        }
        uint64_t generation = 0;
        const char *interface_name = "";
        if (slot != 8) {
            if (strcmp(local, o->mappings[slot].local) || strcmp(peer, o->mappings[slot].peer)) {
                o->mappings[slot].generation++;
                snprintf(o->mappings[slot].local, sizeof(o->mappings[slot].local), "%s", local);
                snprintf(o->mappings[slot].peer, sizeof(o->mappings[slot].peer), "%s", peer);
                char event[640];
                snprintf(event, sizeof(event), "\"path_id\":%"PRIu64",\"mapping_generation\":%"PRIu64","
                    "\"interface_name\":\"%s\",\"local_address\":\"%s\",\"peer_address\":\"%s\"",
                    p->path_id, o->mappings[slot].generation, o->mappings[slot].interface_name, local, peer);
                xqc_observer_event(conn, "path_map", event);
            }
            generation = o->mappings[slot].generation;
            interface_name = o->mappings[slot].interface_name;
        } else { o->invalid = 1; }
        char packet_count[32];
        observer_inflight_packets(conn, p->path_id, packet_count, sizeof(packet_count));
        char selected[2048] = {0};
        xqc_adaptive_observer_path_json(conn, p->path_id, selected, sizeof(selected));
        int n = snprintf(out + used, cap - used,
            "%s{\"path_id\":%"PRIu64",\"mapping_generation\":%"PRIu64",\"path_index\":%u,"
            "\"interface_name\":\"%s\",\"inflight_packets\":%s,\"inflight_packets_reason\":%s,"
            "\"local_address\":\"%s\",\"peer_address\":\"%s\",\"path_state\":%d,\"app_status\":%d,"
            "\"cwnd_bytes\":%"PRIu64",\"effective_cwnd_bytes\":%"PRIu64","
            "\"inflight_bytes\":%"PRIu64",\"scheduled_bytes\":%"PRIu64","
            "\"occupied_bytes\":%"PRIu64",\"headroom_bytes\":%"PRIu64",\"srtt_us\":%"PRIu64","
            "\"wire_sent_bytes\":%"PRIu64",\"stream_packet_sent_bytes\":%"PRIu64","
            "\"retrans_sent_bytes\":%"PRIu64",\"reinject_sent_bytes\":%"PRIu64","
            "\"delivered_quic_bytes\":%"PRIu64",\"reference_packet_bytes\":1200,\"cwnd_eligible_ref\":%s%s}",
            index ? "," : "", p->path_id, generation, index, interface_name, packet_count,
            !strcmp(packet_count, "null") ? "\"scan_limit\"" : "null", local, peer, p->path_state, p->app_path_status,
            raw, eff, i, z, occupied, eff > occupied ? eff - occupied : 0,
            (uint64_t)ctl->ctl_srtt, ctl->ctl_bytes_send, ctl->ctl_app_bytes_send,
            ctl->ctl_retrans_bytes_send, ctl->ctl_reinjected_bytes_send, ctl->ctl_delivered,
            eff >= occupied && eff - occupied >= 1200 ? "true" : "false", selected);
        if (n < 0 || (size_t)n >= cap - used) { conn->observer->invalid = 1; break; }
        used += (size_t)n; ++index;
    }
    return used;
}

void
xqc_observer_sample(xqc_engine_t *engine, const xqc_cid_t *cid, const char *kind,
    uint64_t scheduled, uint64_t interval, uint64_t missed)
{
    xqc_connection_t *conn = observer_conn(engine, cid);
    if (!conn || !conn->observer) { return; }
    char *paths = conn->observer->paths_record, *scheduler = conn->observer->scheduler_record;
    char *body = conn->observer->body_record;
    paths[0] = scheduler[0] = body[0] = 0;
    observer_paths(conn, paths, sizeof(conn->observer->paths_record));
    if (!xqc_adaptive_observer_json(conn, scheduler, sizeof(conn->observer->scheduler_record))) {
        snprintf(scheduler, sizeof(conn->observer->scheduler_record), "\"scheduler_name\":\"other\",\"primary_valid\":false,"
            "\"primary_path_id\":null,\"secondary_path_id\":null,\"target_secondary_pct\":null,"
            "\"adaptive_phase\":\"not_applicable\",\"role_epoch\":null");
    }
    if (conn->scheduler_callback && conn->scheduler_callback->xqc_scheduler_get_path == xqc_minrtt_scheduler_cb.xqc_scheduler_get_path) {
        char *name = strstr(scheduler, "other");
        if (name) { /* same-sized JSON rebuild avoids overwriting adjacent text */
            snprintf(scheduler, sizeof(conn->observer->scheduler_record), "\"scheduler_name\":\"minrtt\",\"primary_valid\":false,"
                "\"primary_path_id\":null,\"secondary_path_id\":null,\"target_secondary_pct\":null,"
                "\"adaptive_phase\":\"not_applicable\",\"role_epoch\":null");
        }
    }
    uint64_t now = observer_now(conn->observer);
    snprintf(body, sizeof(conn->observer->body_record),
        "\"scheduled_mono_us\":%"PRIu64",\"interval_us\":%"PRIu64",\"lateness_us\":%"PRIu64","
        "\"missed_intervals\":%"PRIu64",\"connection_send_limit_bytes\":%"PRIu64","
        "\"connection_sent_offset_bytes\":%"PRIu64",\"paths\":[%s],%s,"
        "\"sent_bytes_basis\":\"encoded_QUIC_packet_bytes\",\"selected_bytes_basis\":\"scheduler_packet_size\"",
        scheduled, interval, now > scheduled ? now - scheduled : 0, missed,
        conn->conn_flow_ctl.fc_max_data_can_send, conn->conn_flow_ctl.fc_data_sent, paths, scheduler);
    size_t length = strlen(body);
    snprintf(body + length, sizeof(conn->observer->body_record) - length,
        ",\"post_write_us\":%"PRIu64",\"post_write_samples\":%"PRIu64","
        "\"post_write_bytes\":%"PRIu64",\"write_queue_bytes\":%"PRIu64",\"write_queue_depth\":%"PRIu64,
        conn->observer->write_cost_us, conn->observer->write_samples, conn->observer->write_bytes,
        conn->observer->queue_bytes, conn->observer->queue_depth);
    observer_aggregate(conn, NULL, now);
    length = strlen(body);
    snprintf(body + length, sizeof(conn->observer->body_record) - length,
        ",\"sum_B\":%"PRIu64",\"sum_B_peak_bytes\":%"PRIu64",\"sum_B_byte_us\":%"PRIu64","
        "\"any_stream_hol_us\":%"PRIu64",\"aggregate_from_us\":%"PRIu64","
        "\"aggregate_prebind_unobserved\":%s,\"aggregate_includes_unbound\":%s,\"aggregate_valid\":%s",
        conn->observer->sum_b, conn->observer->sum_b_peak, conn->observer->sum_b_byte_us,
        conn->observer->any_hol_us, conn->observer->aggregate_from_us,
        conn->observer->aggregate_prebind_unobserved ? "true" : "false",
        conn->observer->unbound_observed ? "true" : "false",
        conn->observer->unbound_observed || conn->observer->invalid || conn->observer->aggregate_prebind_unobserved ? "false" : "true");
    observer_emit(conn, 0, kind, body);
    xqc_list_head_t *pos;
    xqc_list_for_each(pos, &conn->conn_all_streams) {
        xqc_stream_t *stream = xqc_list_entry(pos, xqc_stream_t, all_stream_list);
        if (stream->observer && !stream->observer->state.ended) { observer_stream_record(stream, "stream_snapshot"); }
    }
}

void
xqc_observer_role(xqc_connection_t *conn, int valid, uint64_t primary,
    uint64_t secondary, double target, int phase)
{
    xqc_observer_t *o = conn->observer;
    if (!o) { return; }
    if (!o->role_initialized || o->role_valid != valid || o->primary_id != primary
        || o->secondary_id != secondary || o->target_pct != target || o->phase != phase) {
        int role_changed = !o->role_initialized || o->role_valid != valid
            || o->primary_id != primary || o->secondary_id != secondary;
        if (role_changed) { o->role_epoch++; }
        char primary_text[32] = "null", secondary_text[32] = "null", old_text[32] = "null";
        if (valid) { snprintf(primary_text, sizeof(primary_text), "%"PRIu64, primary); }
        if (valid && secondary != UINT64_MAX) { snprintf(secondary_text, sizeof(secondary_text), "%"PRIu64, secondary); }
        if (o->role_valid) { snprintf(old_text, sizeof(old_text), "%"PRIu64, o->primary_id); }
        char *paths = o->paths_record, *body = o->body_record;
        paths[0] = body[0] = 0;
        observer_paths(conn, paths, sizeof(o->paths_record));
        snprintf(body, sizeof(o->body_record),
            "\"role_epoch\":%"PRIu64",\"primary_valid\":%s,\"old_primary_path_id\":%s,"
            "\"primary_path_id\":%s,\"secondary_path_id\":%s,"
            "\"target_secondary_pct\":%.6f,\"adaptive_phase_code\":%d,"
            "\"reason\":\"scheduler_state_change\",\"paths\":[%s]",
            o->role_epoch, valid ? "true" : "false", old_text, primary_text,
            secondary_text, target, phase, paths);
        xqc_observer_event(conn, role_changed ? "role_change" : "control_update", body);
        o->role_initialized = 1; o->role_valid = valid; o->primary_id = primary;
        o->secondary_id = secondary; o->target_pct = target; o->phase = phase;
    }
}

void
xqc_observer_phase(xqc_engine_t *engine, const xqc_cid_t *cid, const char *phase,
    uint64_t bytes, uint64_t depth, uint64_t cost)
{
    xqc_connection_t *conn = observer_conn(engine, cid);
    if (!conn || !conn->observer) { return; }
    xqc_observer_t *o = conn->observer;
    if (cost == UINT64_MAX) { o->invalid = 1; cost = 0; }
    if (!strcmp(phase, "task_end")) {
        xqc_list_head_t *pos;
        xqc_list_for_each(pos, &conn->conn_all_streams) {
            xqc_stream_t *stream = xqc_list_entry(pos, xqc_stream_t, all_stream_list);
            xqc_observer_stream_end(stream, "task_end");
        }
    }
    if (!strcmp(phase, "post_write")) {
        if (!bytes) { return; }
        o->write_cost_us = xqc_observer_add(o->write_cost_us, cost, &o->invalid);
        o->write_bytes = xqc_observer_add(o->write_bytes, bytes, &o->invalid);
        o->write_samples++; return;
    }
    if (!strcmp(phase, "write_queue")) { o->queue_bytes = bytes; o->queue_depth = depth; return; }
    char body[512];
    snprintf(body, sizeof(body), "\"phase_name\":\"%s\",\"bytes\":%"PRIu64","
        "\"queue_depth\":%"PRIu64",\"cost_us\":%"PRIu64, phase, bytes, depth, cost);
    xqc_observer_event(conn, "phase_end", body);
}

void
xqc_observer_destroy(xqc_connection_t *conn)
{
    if (!conn || !conn->observer) { return; }
    xqc_list_head_t *pos;
    xqc_list_for_each(pos, &conn->conn_all_streams) {
        xqc_stream_t *stream = xqc_list_entry(pos, xqc_stream_t, all_stream_list);
        xqc_observer_stream_end(stream, "observer_end");
        if (stream->observer) {
            free(stream->observer->state.ranges); free(stream->observer); stream->observer = NULL;
        }
    }
    free(conn->observer); conn->observer = NULL;
}

void
xqc_observer_detach(xqc_engine_t *engine, const xqc_cid_t *cid)
{
    xqc_observer_destroy(observer_conn(engine, cid));
}

int
xqc_observer_checkpoint_metadata(xqc_engine_t *engine, const xqc_cid_t *cid,
    char *out, size_t capacity)
{
    xqc_connection_t *conn = observer_conn(engine, cid);
    if (!conn || !conn->observer || !out || !capacity) { return -1; }
    xqc_observer_t *o = conn->observer;
    int n = snprintf(out, capacity,
        "\"attempt_id\":\"%s\",\"conn_uid\":\"%s\",\"endpoint\":\"%s\","
        "\"transport_seq\":%"PRIu64",\"event_seq\":%"PRIu64",\"observer_valid\":%s,\"task_complete\":true",
        o->attempt, o->uid, o->endpoint, o->seq, o->event_seq,
        o->invalid || o->dropped ? "false" : "true");
    return n < 0 || (size_t)n >= capacity ? -1 : 0;
}
