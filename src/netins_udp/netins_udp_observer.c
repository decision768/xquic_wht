#define _POSIX_C_SOURCE 200809L

#include "netins_udp_observer_internal.h"
#include "netins_udp_transport_metrics.h"

#include <errno.h>
#include <netdb.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

_Static_assert((unsigned)NETINS_UDP_MAX_PATHS
        == (unsigned)XQC_MAX_PATHS_COUNT,
    "netins UDP and XQUIC path limits must match");
_Static_assert((int)NETINS_UDP_APP_PATH_STANDBY
        == (int)XQC_APP_PATH_STATUS_STANDBY,
    "netins UDP application path values must match XQUIC");
_Static_assert((int)NETINS_UDP_APP_PATH_AVAILABLE
        == (int)XQC_APP_PATH_STATUS_AVAILABLE,
    "netins UDP application path values must match XQUIC");
_Static_assert((int)NETINS_UDP_APP_PATH_FROZEN
        == (int)XQC_APP_PATH_STATUS_FROZEN,
    "netins UDP application path values must match XQUIC");

typedef struct netins_udp_observer_path_metadata_s {
    int valid;
    uint64_t path_id;
    char interface_name[NETINS_UDP_INTERFACE_NAME_LEN];
    char local_address[NETINS_UDP_ADDRESS_LEN];
    char peer_address[NETINS_UDP_ADDRESS_LEN];
} netins_udp_observer_path_metadata_t;

struct netins_udp_observer_s {
    pthread_mutex_t lock;
    size_t reference_count;

    xqc_engine_t *engine;
    xqc_cid_t cid;
    int bound;
    int registered;
    struct netins_udp_observer_s *registry_next;

    netins_udp_snapshot_t snapshot;
    netins_udp_observer_extended_snapshot_t extended_snapshot;
    netins_udp_observer_path_metadata_t path_metadata[NETINS_UDP_MAX_PATHS];

    netins_udp_logger_cb logger;
    void *logger_user_data;
    int last_error_code;
    char last_error_msg[NETINS_UDP_ERROR_MSG_LEN];
};

static pthread_mutex_t g_netins_udp_observer_registry_lock = PTHREAD_MUTEX_INITIALIZER;
static netins_udp_observer_t *g_netins_udp_observer_registry_head;
static uint64_t g_netins_udp_observer_next_connection_id = 1;

static uint64_t
netins_udp_observer_now_us(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (uint64_t)ts.tv_sec * 1000000ULL
        + (uint64_t)ts.tv_nsec / 1000ULL;
}

static void
netins_udp_observer_copy_string(char *dst, size_t dst_len, const char *src,
    size_t src_len)
{
    if (dst == NULL || dst_len == 0) {
        return;
    }

    size_t copy_len = 0;
    if (src != NULL) {
        while (copy_len < src_len && src[copy_len] != '\0') {
            copy_len++;
        }
        if (copy_len >= dst_len) {
            copy_len = dst_len - 1;
        }
        if (copy_len > 0) {
            memcpy(dst, src, copy_len);
        }
    }
    dst[copy_len] = '\0';
}

static void
netins_udp_observer_init_info(netins_udp_info_t *info)
{
    memset(info, 0, sizeof(*info));
    info->struct_size = sizeof(*info);
    info->api_version = NETINS_UDP_API_VERSION;
    info->connection_id = NETINS_UDP_INVALID_CONNECTION_ID;
    info->use_udp = 1;
    info->use_quic = 1;
}

static void
netins_udp_observer_init_statis(netins_udp_statis_t *statis)
{
    memset(statis, 0, sizeof(*statis));
    statis->struct_size = sizeof(*statis);
    statis->api_version = NETINS_UDP_API_VERSION;
}

static void
netins_udp_observer_init_path_statis(netins_udp_path_statis_t *path)
{
    memset(path, 0, sizeof(*path));
    path->struct_size = sizeof(*path);
    path->api_version = NETINS_UDP_API_VERSION;
    path->path_id = UINT64_MAX;
}

static void
netins_udp_observer_init_snapshot(netins_udp_snapshot_t *snapshot)
{
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->struct_size = sizeof(*snapshot);
    snapshot->api_version = NETINS_UDP_API_VERSION;
    netins_udp_observer_init_info(&snapshot->info);
    netins_udp_observer_init_statis(&snapshot->statis);
    for (size_t i = 0; i < NETINS_UDP_MAX_PATHS; ++i) {
        netins_udp_observer_init_path_statis(&snapshot->paths[i]);
    }
}

static void
netins_udp_observer_init_extended_snapshot(
    netins_udp_observer_extended_snapshot_t *snapshot)
{
    memset(snapshot, 0, sizeof(*snapshot));
    for (size_t i = 0; i < NETINS_UDP_MAX_PATHS; ++i) {
        snapshot->paths[i].path_id = UINT64_MAX;
    }
}

static void
netins_udp_observer_set_error_locked(netins_udp_observer_t *netins, int error_code,
    const char *error_message)
{
    netins->last_error_code = error_code;
    if (error_message == NULL) {
        netins->last_error_msg[0] = '\0';
        return;
    }
    snprintf(netins->last_error_msg, sizeof(netins->last_error_msg), "%s",
        error_message);
}

static void
netins_udp_observer_set_error(netins_udp_observer_t *netins, int error_code,
    const char *error_message)
{
    if (netins == NULL) {
        return;
    }
    pthread_mutex_lock(&netins->lock);
    netins_udp_observer_set_error_locked(netins, error_code, error_message);
    pthread_mutex_unlock(&netins->lock);
}

static void
netins_udp_observer_log(netins_udp_observer_t *netins, int level, const char *message)
{
    netins_udp_logger_cb logger = NULL;
    void *user_data = NULL;

    if (netins == NULL || message == NULL) {
        return;
    }

    pthread_mutex_lock(&netins->lock);
    logger = netins->logger;
    user_data = netins->logger_user_data;
    pthread_mutex_unlock(&netins->lock);

    if (logger != NULL) {
        logger(level, message, user_data);
    }
}

static int
netins_udp_observer_output_valid(uint32_t struct_size, uint32_t api_version,
    size_t required_size)
{
    if (api_version != NETINS_UDP_API_VERSION) {
        return NETINS_UDP_VERSION_MISMATCH;
    }
    if ((size_t)struct_size < required_size) {
        return NETINS_UDP_BUFFER_TOO_SMALL;
    }
    return NETINS_UDP_OK;
}

static double
netins_udp_observer_rate_mbps(uint64_t current, uint64_t previous,
    uint64_t interval_us)
{
    if (interval_us == 0 || current < previous) {
        return 0.0;
    }
    return (double)(current - previous) * 8.0 / (double)interval_us;
}

static const netins_udp_path_statis_t *
netins_udp_observer_find_previous_path(const netins_udp_snapshot_t *snapshot,
    uint64_t path_id)
{
    for (size_t i = 0; i < snapshot->path_count; ++i) {
        if (snapshot->paths[i].path_id == path_id) {
            return &snapshot->paths[i];
        }
    }
    return NULL;
}

static const netins_udp_observer_path_metadata_t *
netins_udp_observer_find_metadata(const netins_udp_observer_path_metadata_t *metadata,
    uint64_t path_id)
{
    for (size_t i = 0; i < NETINS_UDP_MAX_PATHS; ++i) {
        if (metadata[i].valid && metadata[i].path_id == path_id) {
            return &metadata[i];
        }
    }
    return NULL;
}

static void
netins_udp_observer_destroy(netins_udp_observer_t *netins)
{
    pthread_mutex_destroy(&netins->lock);
    free(netins);
}

static void
netins_udp_observer_retain_locked(netins_udp_observer_t *netins)
{
    netins->reference_count++;
}

static void
netins_udp_observer_release(netins_udp_observer_t *netins)
{
    int destroy = 0;
    pthread_mutex_lock(&netins->lock);
    if (netins->reference_count > 0) {
        netins->reference_count--;
    }
    destroy = netins->reference_count == 0;
    pthread_mutex_unlock(&netins->lock);
    if (destroy) {
        netins_udp_observer_destroy(netins);
    }
}

static int
netins_udp_observer_register(netins_udp_observer_t *netins)
{
    pthread_mutex_lock(&g_netins_udp_observer_registry_lock);
    pthread_mutex_lock(&netins->lock);

    if (!netins->registered) {
        if (g_netins_udp_observer_next_connection_id == NETINS_UDP_INVALID_CONNECTION_ID) {
            pthread_mutex_unlock(&netins->lock);
            pthread_mutex_unlock(&g_netins_udp_observer_registry_lock);
            netins_udp_observer_set_error(netins, EOVERFLOW,
                "connection ID space exhausted");
            return NETINS_UDP_ERROR;
        }
        netins->snapshot.info.connection_id =
            g_netins_udp_observer_next_connection_id++;
        netins->registry_next = g_netins_udp_observer_registry_head;
        g_netins_udp_observer_registry_head = netins;
        netins->registered = 1;
        netins_udp_observer_retain_locked(netins); /* registry reference */
    }

    pthread_mutex_unlock(&netins->lock);
    pthread_mutex_unlock(&g_netins_udp_observer_registry_lock);
    return NETINS_UDP_OK;
}

static void
netins_udp_observer_unregister(netins_udp_observer_t *netins)
{
    int release_registry_reference = 0;

    pthread_mutex_lock(&g_netins_udp_observer_registry_lock);
    netins_udp_observer_t **cursor = &g_netins_udp_observer_registry_head;
    while (*cursor != NULL && *cursor != netins) {
        cursor = &(*cursor)->registry_next;
    }
    if (*cursor == netins) {
        *cursor = netins->registry_next;
        pthread_mutex_lock(&netins->lock);
        netins->registry_next = NULL;
        netins->registered = 0;
        pthread_mutex_unlock(&netins->lock);
        release_registry_reference = 1;
    }
    pthread_mutex_unlock(&g_netins_udp_observer_registry_lock);

    if (release_registry_reference) {
        netins_udp_observer_release(netins);
    }
}

netins_udp_observer_t *
netins_udp_observer_new(void)
{
    netins_udp_observer_t *netins = calloc(1, sizeof(*netins));
    if (netins == NULL) {
        return NULL;
    }
    if (pthread_mutex_init(&netins->lock, NULL) != 0) {
        free(netins);
        return NULL;
    }

    netins->reference_count = 1;
    netins_udp_observer_init_snapshot(&netins->snapshot);
    netins_udp_observer_init_extended_snapshot(&netins->extended_snapshot);
    netins->snapshot.info.connection_state = NETINS_UDP_STATE_NEW;
    return netins;
}

void
netins_udp_observer_close(netins_udp_observer_t *netins)
{
    if (netins != NULL) {
        netins_udp_observer_release(netins);
    }
}

netins_udp_observer_t *
netins_udp_observer_open(uint64_t connection_id)
{
    netins_udp_observer_t *result = NULL;
    pthread_mutex_lock(&g_netins_udp_observer_registry_lock);
    for (netins_udp_observer_t *item = g_netins_udp_observer_registry_head;
         item != NULL; item = item->registry_next)
    {
        pthread_mutex_lock(&item->lock);
        if (item->registered
            && item->snapshot.info.connection_id == connection_id)
        {
            netins_udp_observer_retain_locked(item);
            result = item;
            pthread_mutex_unlock(&item->lock);
            break;
        }
        pthread_mutex_unlock(&item->lock);
    }
    pthread_mutex_unlock(&g_netins_udp_observer_registry_lock);
    return result;
}

int
netins_udp_observer_list_connections(uint64_t *connection_ids, size_t capacity,
    size_t *connection_count)
{
    if (connection_count == NULL || (connection_ids == NULL && capacity != 0)) {
        return NETINS_UDP_ERROR;
    }

    size_t total = 0;
    pthread_mutex_lock(&g_netins_udp_observer_registry_lock);
    for (netins_udp_observer_t *item = g_netins_udp_observer_registry_head;
         item != NULL; item = item->registry_next)
    {
        pthread_mutex_lock(&item->lock);
        if (item->registered) {
            if (connection_ids != NULL && total < capacity) {
                connection_ids[total] = item->snapshot.info.connection_id;
            }
            total++;
        }
        pthread_mutex_unlock(&item->lock);
    }
    pthread_mutex_unlock(&g_netins_udp_observer_registry_lock);

    *connection_count = total;
    if (connection_ids == NULL) {
        return NETINS_UDP_OK;
    }
    return capacity < total ? NETINS_UDP_BUFFER_TOO_SMALL : NETINS_UDP_OK;
}

int
netins_udp_observer_bind(netins_udp_observer_t *netins, xqc_engine_t *engine,
    const xqc_cid_t *cid, netins_udp_role_t role)
{
    if (netins == NULL || engine == NULL || cid == NULL
        || (role != NETINS_UDP_ROLE_CLIENT && role != NETINS_UDP_ROLE_SERVER))
    {
        if (netins != NULL) {
            netins_udp_observer_set_error(netins, EINVAL,
                "invalid engine, CID, or connection role");
        }
        return NETINS_UDP_ERROR;
    }

    pthread_mutex_lock(&netins->lock);
    if (netins->bound || netins->registered) {
        netins_udp_observer_set_error_locked(netins, EALREADY,
            "observer is already bound");
        pthread_mutex_unlock(&netins->lock);
        return NETINS_UDP_ERROR;
    }
    netins->engine = engine;
    memcpy(&netins->cid, cid, sizeof(netins->cid));
    netins->bound = 1;
    netins->snapshot.info.role = role;
    netins->snapshot.info.connection_state = NETINS_UDP_STATE_CONNECTING;
    netins_udp_observer_set_error_locked(netins, 0, NULL);
    pthread_mutex_unlock(&netins->lock);

    if (netins_udp_observer_refresh(netins) != NETINS_UDP_OK) {
        pthread_mutex_lock(&netins->lock);
        netins->engine = NULL;
        memset(&netins->cid, 0, sizeof(netins->cid));
        netins->bound = 0;
        netins->snapshot.info.connection_state = NETINS_UDP_STATE_NEW;
        pthread_mutex_unlock(&netins->lock);
        return NETINS_UDP_ERROR;
    }

    if (netins_udp_observer_register(netins) != NETINS_UDP_OK) {
        pthread_mutex_lock(&netins->lock);
        netins->engine = NULL;
        netins->bound = 0;
        netins->snapshot.info.connection_state = NETINS_UDP_STATE_NEW;
        pthread_mutex_unlock(&netins->lock);
        return NETINS_UDP_ERROR;
    }

    netins_udp_observer_log(netins, NETINS_UDP_LOG_LEVEL_INFO,
        "netins UDP observer bound to XQUIC connection");
    return NETINS_UDP_OK;
}

int
netins_udp_observer_update_cid(netins_udp_observer_t *netins, const xqc_cid_t *cid)
{
    if (netins == NULL || cid == NULL) {
        return NETINS_UDP_ERROR;
    }

    pthread_mutex_lock(&netins->lock);
    if (!netins->bound || netins->engine == NULL) {
        netins_udp_observer_set_error_locked(netins, ENOTCONN,
            "observer is not bound to a live connection");
        pthread_mutex_unlock(&netins->lock);
        return NETINS_UDP_ERROR;
    }
    memcpy(&netins->cid, cid, sizeof(netins->cid));
    pthread_mutex_unlock(&netins->lock);
    return NETINS_UDP_OK;
}

int
netins_udp_observer_refresh(netins_udp_observer_t *netins)
{
    xqc_engine_t *engine = NULL;
    xqc_cid_t cid;
    netins_udp_snapshot_t previous;
    netins_udp_observer_extended_snapshot_t previous_extended;
    netins_udp_observer_path_metadata_t metadata[NETINS_UDP_MAX_PATHS];

    if (netins == NULL) {
        return NETINS_UDP_ERROR;
    }

    pthread_mutex_lock(&netins->lock);
    if (!netins->bound || netins->engine == NULL) {
        netins_udp_observer_set_error_locked(netins, ENOTCONN,
            "observer is not bound to a live connection");
        pthread_mutex_unlock(&netins->lock);
        return NETINS_UDP_ERROR;
    }
    engine = netins->engine;
    memcpy(&cid, &netins->cid, sizeof(cid));
    memcpy(&previous, &netins->snapshot, sizeof(previous));
    memcpy(&previous_extended, &netins->extended_snapshot,
        sizeof(previous_extended));
    memcpy(metadata, netins->path_metadata, sizeof(metadata));
    pthread_mutex_unlock(&netins->lock);

    netins_udp_transport_metrics_t transport;
    if (netins_udp_transport_metrics_collect(engine, &cid, &transport) != 0) {
        netins_udp_observer_set_error(netins, ENOENT,
            "XQUIC connection not found for bound CID");
        return NETINS_UDP_NOT_FOUND;
    }
    const xqc_conn_stats_t *raw = &transport.legacy;

    netins_udp_snapshot_t next;
    netins_udp_observer_init_snapshot(&next);
    netins_udp_observer_extended_snapshot_t next_extended;
    memcpy(&next_extended, &previous_extended, sizeof(next_extended));
    next.info.connection_id = previous.info.connection_id;
    next.info.role = previous.info.role;
    next.info.connection_state = previous.info.connection_state;
    next.info.handshake_finished = previous.info.handshake_finished;
    next.info.multipath_mode = raw->enable_multipath;
    next.info.mp_state = raw->mp_state;
    next.info.conn_error = raw->conn_err;
    netins_udp_observer_copy_string(next.info.alpn, sizeof(next.info.alpn), raw->alpn,
        sizeof(raw->alpn));
    netins_udp_observer_copy_string(next.info.conn_info, sizeof(next.info.conn_info),
        raw->conn_info, sizeof(raw->conn_info));
    netins_udp_observer_copy_string(next.info.extern_conn_info,
        sizeof(next.info.extern_conn_info), raw->extern_conn_info,
        sizeof(raw->extern_conn_info));

    next.statis.snapshot_time_us = netins_udp_observer_now_us();
    if (previous.statis.snapshot_time_us > 0
        && next.statis.snapshot_time_us >= previous.statis.snapshot_time_us)
    {
        next.statis.sample_interval_us = next.statis.snapshot_time_us
            - previous.statis.snapshot_time_us;
    }
    next.statis.packets_sent = raw->send_count;
    next.statis.packets_recv = raw->recv_count;
    next.statis.packets_lost = raw->lost_count;
    next.statis.packets_spurious_lost = raw->spurious_loss_count;
    next.statis.datagrams_lost = raw->lost_dgram_count;
    next.statis.srtt_us = raw->srtt;
    next.statis.min_rtt_us = raw->min_rtt;
    next.statis.inflight_bytes = raw->inflight_bytes;
    next.statis.loss_rate = raw->send_count > 0
        ? (double)raw->lost_count / (double)raw->send_count : 0.0;

    /*
     * A path's array slot is its NetIns path_index for the life of this
     * connection.  Retain disappeared paths as CLOSED so later removals never
     * shift the indexes already observed by callers.
     */
    next.path_count = previous.path_count;
    next_extended.path_count = (uint32_t)next.path_count;
    for (size_t i = 0; i < next.path_count; ++i) {
        memcpy(&next.paths[i], &previous.paths[i], sizeof(next.paths[i]));
        next.paths[i].transport_state = NETINS_UDP_PATH_CLOSED;
        next.paths[i].send_mbps = 0.0;
        next.paths[i].wire_send_mbps = 0.0;
        next.paths[i].delivery_mbps = 0.0;
        next.paths[i].recv_mbps = 0.0;
        next.paths[i].effective_recv_mbps = 0.0;
        next_extended.paths[i].unique_send_mbps = 0.0;
        next_extended.paths[i].unique_recv_mbps = 0.0;
        next_extended.paths[i].wire_recv_mbps = 0.0;
    }

    for (size_t i = 0; i < transport.path_count; ++i) {
        const netins_udp_transport_path_metrics_t *src = &transport.paths[i];
        netins_udp_path_statis_t *dst = NULL;
        for (size_t slot = 0; slot < next.path_count; ++slot) {
            if (next.paths[slot].path_id == src->path_id) {
                dst = &next.paths[slot];
                break;
            }
        }
        if (dst == NULL) {
            if (next.path_count >= NETINS_UDP_MAX_PATHS) {
                next_extended.path_table_truncated = 1;
                continue;
            }
            dst = &next.paths[next.path_count++];
            netins_udp_observer_init_path_statis(dst);
        }
        size_t path_index = (size_t)(dst - next.paths);
        netins_udp_observer_extended_path_t *extended =
            &next_extended.paths[path_index];
        const netins_udp_observer_extended_path_t *old_extended =
            path_index < previous_extended.path_count
                && previous_extended.paths[path_index].path_id == src->path_id
            ? &previous_extended.paths[path_index] : NULL;

        dst->path_id = src->path_id;
        dst->transport_state = src->path_state;
        dst->app_status = src->app_path_status;
        dst->packets_sent = src->packets_sent;
        dst->packets_recv = src->packets_recv;
        dst->bytes_sent = src->app_packet_bytes_sent;
        dst->bytes_wire_sent = src->wire_bytes_sent;
        dst->bytes_delivered = src->delivered_bytes;
        dst->bytes_sent_reinject = src->reinjected_bytes_sent;
        dst->bytes_sent_retransmitted = src->retransmitted_bytes_sent;
        dst->bytes_recv = src->app_packet_bytes_recv;
        dst->bytes_recv_reinject = src->reinjected_bytes_recv;
        dst->bytes_recv_effective = src->stream_payload_bytes_recv_unique;
        dst->bytes_recv_effective_reinject =
            src->stream_payload_bytes_recv_unique_reinject;
        dst->srtt_us = src->srtt_us;
        extended->path_id = src->path_id;
        extended->stream_payload_bytes_sent_unique =
            src->stream_payload_bytes_sent_unique;
        extended->stream_payload_bytes_recv_unique =
            src->stream_payload_bytes_recv_unique;
        extended->wire_bytes_recv = src->wire_bytes_recv;
        extended->retransmitted_bytes_sent =
            src->retransmitted_bytes_sent;
        extended->cwnd_bytes = src->cwnd_bytes;
        extended->inflight_bytes = src->inflight_bytes;
        extended->latest_rtt_us = src->latest_rtt_us;
        extended->min_rtt_us = src->min_rtt_us;
        extended->last_received_time_us = src->last_received_time_us;
        extended->last_sent_time_us = src->last_sent_time_us;
        extended->lost_packets = src->lost_packets;
        extended->cwnd_limited = src->cwnd_limited;
        if (old_extended != NULL) {
            extended->unique_send_mbps = netins_udp_observer_rate_mbps(
                extended->stream_payload_bytes_sent_unique,
                old_extended->stream_payload_bytes_sent_unique,
                next.statis.sample_interval_us);
            extended->unique_recv_mbps = netins_udp_observer_rate_mbps(
                extended->stream_payload_bytes_recv_unique,
                old_extended->stream_payload_bytes_recv_unique,
                next.statis.sample_interval_us);
            extended->wire_recv_mbps = netins_udp_observer_rate_mbps(
                extended->wire_bytes_recv, old_extended->wire_bytes_recv,
                next.statis.sample_interval_us);
        }

        const netins_udp_observer_path_metadata_t *path_metadata =
            netins_udp_observer_find_metadata(metadata, dst->path_id);
        if (path_metadata != NULL) {
            netins_udp_observer_copy_string(dst->interface_name,
                sizeof(dst->interface_name), path_metadata->interface_name,
                sizeof(path_metadata->interface_name));
            netins_udp_observer_copy_string(dst->local_address,
                sizeof(dst->local_address), path_metadata->local_address,
                sizeof(path_metadata->local_address));
            netins_udp_observer_copy_string(dst->peer_address,
                sizeof(dst->peer_address), path_metadata->peer_address,
                sizeof(path_metadata->peer_address));
        }

        const netins_udp_path_statis_t *old_path =
            netins_udp_observer_find_previous_path(&previous, dst->path_id);
        if (old_path != NULL) {
            if (dst->interface_name[0] == '\0') {
                netins_udp_observer_copy_string(dst->interface_name,
                    sizeof(dst->interface_name), old_path->interface_name,
                    sizeof(old_path->interface_name));
            }
            if (dst->local_address[0] == '\0') {
                netins_udp_observer_copy_string(dst->local_address,
                    sizeof(dst->local_address), old_path->local_address,
                    sizeof(old_path->local_address));
            }
            if (dst->peer_address[0] == '\0') {
                netins_udp_observer_copy_string(dst->peer_address,
                    sizeof(dst->peer_address), old_path->peer_address,
                    sizeof(old_path->peer_address));
            }
            dst->send_mbps = netins_udp_observer_rate_mbps(dst->bytes_sent,
                old_path->bytes_sent, next.statis.sample_interval_us);
            dst->wire_send_mbps = netins_udp_observer_rate_mbps(dst->bytes_wire_sent,
                old_path->bytes_wire_sent, next.statis.sample_interval_us);
            dst->delivery_mbps = netins_udp_observer_rate_mbps(dst->bytes_delivered,
                old_path->bytes_delivered, next.statis.sample_interval_us);
            dst->recv_mbps = netins_udp_observer_rate_mbps(dst->bytes_recv,
                old_path->bytes_recv, next.statis.sample_interval_us);
            dst->effective_recv_mbps = netins_udp_observer_rate_mbps(
                dst->bytes_recv_effective, old_path->bytes_recv_effective,
                next.statis.sample_interval_us);
        }

    }

    for (size_t i = 0; i < next.path_count; ++i) {
        const netins_udp_path_statis_t *dst = &next.paths[i];
        next.statis.bytes_sent += dst->bytes_sent;
        next.statis.bytes_wire_sent += dst->bytes_wire_sent;
        next.statis.bytes_delivered += dst->bytes_delivered;
        next.statis.bytes_retransmitted += dst->bytes_sent_retransmitted;
        next.statis.bytes_recv += dst->bytes_recv;
        next.statis.bytes_recv_effective += dst->bytes_recv_effective;
        next.statis.bytes_sent_reinject += dst->bytes_sent_reinject;
        next.statis.bytes_recv_reinject += dst->bytes_recv_reinject;
        next.statis.bytes_recv_effective_reinject +=
            dst->bytes_recv_effective_reinject;

        if (dst->transport_state == NETINS_UDP_PATH_ACTIVE) {
            next.info.active_path_count++;
        } else if (dst->transport_state == NETINS_UDP_PATH_VALIDATING) {
            next.info.validating_path_count++;
        }
        if (dst->transport_state != NETINS_UDP_PATH_CLOSED) {
            if (dst->app_status == NETINS_UDP_APP_PATH_AVAILABLE) {
                next.info.available_path_count++;
            } else if (dst->app_status == NETINS_UDP_APP_PATH_STANDBY) {
                next.info.standby_path_count++;
            } else if (dst->app_status == NETINS_UDP_APP_PATH_FROZEN) {
                next.info.frozen_path_count++;
            }
        }
    }

    next.info.path_count = next.path_count;
    next_extended.path_count = (uint32_t)next.path_count;
    next.statis.send_mbps = netins_udp_observer_rate_mbps(next.statis.bytes_sent,
        previous.statis.bytes_sent, next.statis.sample_interval_us);
    next.statis.wire_send_mbps = netins_udp_observer_rate_mbps(
        next.statis.bytes_wire_sent, previous.statis.bytes_wire_sent,
        next.statis.sample_interval_us);
    next.statis.delivery_mbps = netins_udp_observer_rate_mbps(
        next.statis.bytes_delivered, previous.statis.bytes_delivered,
        next.statis.sample_interval_us);
    next.statis.recv_mbps = netins_udp_observer_rate_mbps(next.statis.bytes_recv,
        previous.statis.bytes_recv, next.statis.sample_interval_us);
    next.statis.effective_recv_mbps = netins_udp_observer_rate_mbps(
        next.statis.bytes_recv_effective,
        previous.statis.bytes_recv_effective,
        next.statis.sample_interval_us);

    pthread_mutex_lock(&netins->lock);
    if (!netins->bound || netins->engine != engine) {
        pthread_mutex_unlock(&netins->lock);
        return NETINS_UDP_NOT_FOUND;
    }
    memcpy(&netins->snapshot, &next, sizeof(next));
    memcpy(&netins->extended_snapshot, &next_extended,
        sizeof(next_extended));
    netins_udp_observer_set_error_locked(netins, 0, NULL);
    pthread_mutex_unlock(&netins->lock);
    return NETINS_UDP_OK;
}

int
netins_udp_observer_mark_handshake_finished(netins_udp_observer_t *netins)
{
    if (netins == NULL) {
        return NETINS_UDP_ERROR;
    }
    int refresh_result = netins_udp_observer_refresh(netins);
    pthread_mutex_lock(&netins->lock);
    if (!netins->bound) {
        pthread_mutex_unlock(&netins->lock);
        return NETINS_UDP_ERROR;
    }
    netins->snapshot.info.handshake_finished = 1;
    netins->snapshot.info.connection_state = NETINS_UDP_STATE_CONNECTED;
    pthread_mutex_unlock(&netins->lock);
    return refresh_result == NETINS_UDP_OK
        ? NETINS_UDP_OK : NETINS_UDP_ERROR;
}

int
netins_udp_observer_mark_closing(netins_udp_observer_t *netins)
{
    if (netins == NULL) {
        return NETINS_UDP_ERROR;
    }
    pthread_mutex_lock(&netins->lock);
    if (netins->snapshot.info.connection_state != NETINS_UDP_STATE_CLOSED) {
        netins->snapshot.info.connection_state = NETINS_UDP_STATE_CLOSING;
    }
    pthread_mutex_unlock(&netins->lock);
    return NETINS_UDP_OK;
}

int
netins_udp_observer_mark_closed(netins_udp_observer_t *netins, int conn_error)
{
    if (netins == NULL) {
        return NETINS_UDP_ERROR;
    }

    pthread_mutex_lock(&netins->lock);
    int already_detached = !netins->bound;
    pthread_mutex_unlock(&netins->lock);
    if (already_detached) {
        netins_udp_observer_unregister(netins);
        return NETINS_UDP_OK;
    }

    (void)netins_udp_observer_refresh(netins);

    pthread_mutex_lock(&netins->lock);
    int keep_error = netins->snapshot.info.connection_state
        == NETINS_UDP_STATE_ERROR
        || netins->snapshot.info.conn_error != 0;
    if (conn_error != 0) {
        netins->snapshot.info.conn_error = conn_error;
        keep_error = 1;
    }
    netins->snapshot.info.connection_state = keep_error
        ? NETINS_UDP_STATE_ERROR : NETINS_UDP_STATE_CLOSED;
    if (keep_error) {
        int final_error = netins->snapshot.info.conn_error;
        char message[128];
        snprintf(message, sizeof(message),
            "XQUIC connection closed with error: %d", final_error);
        netins_udp_observer_set_error_locked(netins, final_error, message);
    } else {
        netins_udp_observer_set_error_locked(netins, 0, NULL);
    }
    netins->bound = 0;
    netins->engine = NULL;
    memset(&netins->cid, 0, sizeof(netins->cid));
    pthread_mutex_unlock(&netins->lock);

    netins_udp_observer_unregister(netins);
    return NETINS_UDP_OK;
}

int
netins_udp_observer_mark_error(netins_udp_observer_t *netins, int error_code,
    const char *error_message)
{
    if (netins == NULL) {
        return NETINS_UDP_ERROR;
    }
    pthread_mutex_lock(&netins->lock);
    netins->snapshot.info.connection_state = NETINS_UDP_STATE_ERROR;
    netins->snapshot.info.conn_error = error_code;
    netins_udp_observer_set_error_locked(netins, error_code, error_message);
    pthread_mutex_unlock(&netins->lock);
    netins_udp_observer_log(netins, NETINS_UDP_LOG_LEVEL_ERROR,
        error_message != NULL ? error_message : "XQUIC connection error");
    return NETINS_UDP_OK;
}

static int
netins_udp_observer_format_address(const struct sockaddr *addr, socklen_t addr_len,
    char *output, size_t output_len)
{
    if (addr == NULL || addr_len == 0 || output == NULL || output_len == 0) {
        return NETINS_UDP_ERROR;
    }

    char host[128];
    char service[32];
    int result = getnameinfo(addr, addr_len, host, sizeof(host), service,
        sizeof(service), NI_NUMERICHOST | NI_NUMERICSERV);
    if (result != 0) {
        return NETINS_UDP_ERROR;
    }

    size_t host_len = strnlen(host, sizeof(host));
    size_t service_len = strnlen(service, sizeof(service));
    size_t required = host_len + service_len + 2;
    if (addr->sa_family == AF_INET6) {
        required += 2;
    }
    if (required > output_len) {
        return NETINS_UDP_BUFFER_TOO_SMALL;
    }

    size_t offset = 0;
    if (addr->sa_family == AF_INET6) {
        output[offset++] = '[';
    }
    memcpy(output + offset, host, host_len);
    offset += host_len;
    if (addr->sa_family == AF_INET6) {
        output[offset++] = ']';
    }
    output[offset++] = ':';
    memcpy(output + offset, service, service_len);
    offset += service_len;
    output[offset] = '\0';
    return NETINS_UDP_OK;
}

int
netins_udp_observer_set_path_metadata(netins_udp_observer_t *netins, uint64_t path_id,
    const char *interface_name, const struct sockaddr *local_addr,
    socklen_t local_addr_len, const struct sockaddr *peer_addr,
    socklen_t peer_addr_len)
{
    if (netins == NULL || path_id == UINT64_MAX) {
        return NETINS_UDP_ERROR;
    }

    char local_text[NETINS_UDP_ADDRESS_LEN] = {0};
    char peer_text[NETINS_UDP_ADDRESS_LEN] = {0};
    (void)netins_udp_observer_format_address(local_addr, local_addr_len, local_text,
        sizeof(local_text));
    (void)netins_udp_observer_format_address(peer_addr, peer_addr_len, peer_text,
        sizeof(peer_text));

    pthread_mutex_lock(&netins->lock);
    netins_udp_observer_path_metadata_t *slot = NULL;
    for (size_t i = 0; i < NETINS_UDP_MAX_PATHS; ++i) {
        if (netins->path_metadata[i].valid
            && netins->path_metadata[i].path_id == path_id)
        {
            slot = &netins->path_metadata[i];
            break;
        }
        if (slot == NULL && !netins->path_metadata[i].valid) {
            slot = &netins->path_metadata[i];
        }
    }
    if (slot == NULL) {
        netins_udp_observer_set_error_locked(netins, ENOSPC,
            "path metadata table is full");
        pthread_mutex_unlock(&netins->lock);
        return NETINS_UDP_BUFFER_TOO_SMALL;
    }

    slot->valid = 1;
    slot->path_id = path_id;
    if (interface_name != NULL) {
        netins_udp_observer_copy_string(slot->interface_name,
            sizeof(slot->interface_name), interface_name,
            strlen(interface_name));
    }
    if (local_text[0] != '\0') {
        netins_udp_observer_copy_string(slot->local_address,
            sizeof(slot->local_address), local_text, strlen(local_text));
    }
    if (peer_text[0] != '\0') {
        netins_udp_observer_copy_string(slot->peer_address,
            sizeof(slot->peer_address), peer_text, strlen(peer_text));
    }

    for (size_t i = 0; i < netins->snapshot.path_count; ++i) {
        netins_udp_path_statis_t *path = &netins->snapshot.paths[i];
        if (path->path_id != path_id) {
            continue;
        }
        netins_udp_observer_copy_string(path->interface_name,
            sizeof(path->interface_name), slot->interface_name,
            sizeof(slot->interface_name));
        netins_udp_observer_copy_string(path->local_address,
            sizeof(path->local_address), slot->local_address,
            sizeof(slot->local_address));
        netins_udp_observer_copy_string(path->peer_address,
            sizeof(path->peer_address), slot->peer_address,
            sizeof(slot->peer_address));
        break;
    }
    pthread_mutex_unlock(&netins->lock);
    return NETINS_UDP_OK;
}

int
netins_udp_observer_get_snapshot(netins_udp_observer_t *netins, netins_udp_snapshot_t *out)
{
    if (netins == NULL || out == NULL) {
        return NETINS_UDP_ERROR;
    }
    int validation = netins_udp_observer_output_valid(out->struct_size,
        out->api_version, sizeof(*out));
    if (validation != NETINS_UDP_OK) {
        netins_udp_observer_set_error(netins, EPROTO,
            "snapshot API version or structure size mismatch");
        return validation;
    }
    pthread_mutex_lock(&netins->lock);
    memcpy(out, &netins->snapshot, sizeof(*out));
    pthread_mutex_unlock(&netins->lock);
    return NETINS_UDP_OK;
}

int
netins_udp_observer_get_extended_snapshot(
    netins_udp_observer_t *netins,
    netins_udp_observer_extended_snapshot_t *out)
{
    if (netins == NULL || out == NULL) {
        return NETINS_UDP_ERROR;
    }
    pthread_mutex_lock(&netins->lock);
    memcpy(out, &netins->extended_snapshot, sizeof(*out));
    pthread_mutex_unlock(&netins->lock);
    return NETINS_UDP_OK;
}

int
netins_udp_observer_get_private_snapshot(
    netins_udp_observer_t *netins,
    netins_udp_observer_private_snapshot_t *out)
{
    if (netins == NULL || out == NULL) {
        return NETINS_UDP_ERROR;
    }
    pthread_mutex_lock(&netins->lock);
    memcpy(&out->legacy, &netins->snapshot, sizeof(out->legacy));
    memcpy(&out->extended, &netins->extended_snapshot,
        sizeof(out->extended));
    pthread_mutex_unlock(&netins->lock);
    return NETINS_UDP_OK;
}

int
netins_udp_observer_get_snapshot_by_id(uint64_t connection_id,
    netins_udp_snapshot_t *out)
{
    netins_udp_observer_t *netins = netins_udp_observer_open(connection_id);
    if (netins == NULL) {
        return NETINS_UDP_NOT_FOUND;
    }
    int result = netins_udp_observer_get_snapshot(netins, out);
    netins_udp_observer_close(netins);
    return result;
}

int
netins_udp_observer_get_info(netins_udp_observer_t *netins, netins_udp_info_t *out)
{
    if (netins == NULL || out == NULL) {
        return NETINS_UDP_ERROR;
    }
    int validation = netins_udp_observer_output_valid(out->struct_size,
        out->api_version, sizeof(*out));
    if (validation != NETINS_UDP_OK) {
        netins_udp_observer_set_error(netins, EPROTO,
            "info API version or structure size mismatch");
        return validation;
    }
    pthread_mutex_lock(&netins->lock);
    memcpy(out, &netins->snapshot.info, sizeof(*out));
    pthread_mutex_unlock(&netins->lock);
    return NETINS_UDP_OK;
}

int
netins_udp_observer_get_statis(netins_udp_observer_t *netins, netins_udp_statis_t *out)
{
    if (netins == NULL || out == NULL) {
        return NETINS_UDP_ERROR;
    }
    int validation = netins_udp_observer_output_valid(out->struct_size,
        out->api_version, sizeof(*out));
    if (validation != NETINS_UDP_OK) {
        netins_udp_observer_set_error(netins, EPROTO,
            "statistics API version or structure size mismatch");
        return validation;
    }
    pthread_mutex_lock(&netins->lock);
    memcpy(out, &netins->snapshot.statis, sizeof(*out));
    pthread_mutex_unlock(&netins->lock);
    return NETINS_UDP_OK;
}

int
netins_udp_observer_get_path_statis(netins_udp_observer_t *netins, size_t path_index,
    netins_udp_path_statis_t *out)
{
    if (netins == NULL || out == NULL) {
        return NETINS_UDP_ERROR;
    }
    int validation = netins_udp_observer_output_valid(out->struct_size,
        out->api_version, sizeof(*out));
    if (validation != NETINS_UDP_OK) {
        netins_udp_observer_set_error(netins, EPROTO,
            "path statistics API version or structure size mismatch");
        return validation;
    }
    pthread_mutex_lock(&netins->lock);
    if (path_index >= netins->snapshot.path_count) {
        netins_udp_observer_set_error_locked(netins, ENOENT,
            "path index is outside the current snapshot");
        pthread_mutex_unlock(&netins->lock);
        return NETINS_UDP_NOT_FOUND;
    }
    memcpy(out, &netins->snapshot.paths[path_index], sizeof(*out));
    pthread_mutex_unlock(&netins->lock);
    return NETINS_UDP_OK;
}

int
netins_udp_observer_set_logger(netins_udp_observer_t *netins, netins_udp_logger_cb logger,
    void *user_data)
{
    if (netins == NULL) {
        return NETINS_UDP_ERROR;
    }
    pthread_mutex_lock(&netins->lock);
    netins->logger = logger;
    netins->logger_user_data = user_data;
    pthread_mutex_unlock(&netins->lock);
    return NETINS_UDP_OK;
}

int
netins_udp_observer_get_last_error_code(netins_udp_observer_t *netins)
{
    if (netins == NULL) {
        return EINVAL;
    }
    pthread_mutex_lock(&netins->lock);
    int error_code = netins->last_error_code;
    pthread_mutex_unlock(&netins->lock);
    return error_code;
}

int
netins_udp_observer_get_last_error_msg(netins_udp_observer_t *netins, char *buffer,
    size_t buffer_len)
{
    if (netins == NULL || buffer == NULL || buffer_len == 0) {
        return NETINS_UDP_ERROR;
    }
    pthread_mutex_lock(&netins->lock);
    snprintf(buffer, buffer_len, "%s",
        netins->last_error_msg[0] != '\0'
            ? netins->last_error_msg : "No error");
    pthread_mutex_unlock(&netins->lock);
    return NETINS_UDP_OK;
}

const char *
netins_udp_conn_state_name(int state)
{
    switch (state) {
    case NETINS_UDP_STATE_NEW: return "NEW";
    case NETINS_UDP_STATE_CONNECTING: return "CONNECTING";
    case NETINS_UDP_STATE_CONNECTED: return "CONNECTED";
    case NETINS_UDP_STATE_CLOSING: return "CLOSING";
    case NETINS_UDP_STATE_CLOSED: return "CLOSED";
    case NETINS_UDP_STATE_ERROR: return "ERROR";
    default: return "UNKNOWN";
    }
}

const char *
netins_udp_path_state_name(int state)
{
    switch (state) {
    case NETINS_UDP_PATH_INIT: return "INIT";
    case NETINS_UDP_PATH_VALIDATING: return "VALIDATING";
    case NETINS_UDP_PATH_ACTIVE: return "ACTIVE";
    case NETINS_UDP_PATH_CLOSING: return "CLOSING";
    case NETINS_UDP_PATH_CLOSED: return "CLOSED";
    default: return "UNKNOWN";
    }
}

const char *
netins_udp_app_path_status_name(int status)
{
    switch (status) {
    case NETINS_UDP_APP_PATH_NONE: return "NONE";
    case NETINS_UDP_APP_PATH_STANDBY: return "STANDBY";
    case NETINS_UDP_APP_PATH_AVAILABLE: return "AVAILABLE";
    case NETINS_UDP_APP_PATH_FROZEN: return "FROZEN";
    default: return "UNKNOWN";
    }
}
