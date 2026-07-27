#include "netins_udp_backend.h"

#include <string.h>

#include "netins_udp_observer_internal.h"

void
netins_udp_backend_rx_policy_init(netins_udp_backend_rx_policy_t *policy)
{
    if (policy == NULL) {
        return;
    }
    policy->batch_capacity = NETINS_UDP_BACKEND_RX_BATCH_CAPACITY;
    policy->responsive_packet_budget =
        NETINS_UDP_BACKEND_RX_RESPONSIVE_BUDGET;
    policy->bulk_packet_budget =
        NETINS_UDP_BACKEND_RX_BATCH_CAPACITY;
}

uint32_t
netins_udp_backend_rx_packet_limit(
    const netins_udp_backend_rx_policy_t *policy, size_t active_path_count)
{
    if (policy == NULL || policy->batch_capacity == 0) {
        return 0;
    }

    uint32_t limit = active_path_count > 1
        ? policy->bulk_packet_budget
        : policy->responsive_packet_budget;
    if (limit == 0 || limit > policy->batch_capacity) {
        limit = policy->batch_capacity;
    }
    return limit;
}

size_t
netins_udp_backend_next_path_start(size_t current, size_t path_capacity)
{
    return path_capacity == 0 ? 0 : (current + 1) % path_capacity;
}

int
netins_udp_backend_connection_bind(netins_udp_observer_t *observer,
    xqc_engine_t *engine, const xqc_cid_t *cid, netins_udp_role_t role,
    int enable_payload_metrics)
{
    return netins_udp_observer_bind(observer, engine, cid, role,
        enable_payload_metrics);
}

int
netins_udp_backend_connection_update_cid(netins_udp_observer_t *observer,
    const xqc_cid_t *cid)
{
    return netins_udp_observer_update_cid(observer, cid);
}

int
netins_udp_backend_connection_refresh(netins_udp_observer_t *observer)
{
    return netins_udp_observer_refresh(observer);
}

int
netins_udp_backend_connection_handshake_finished(
    netins_udp_observer_t *observer)
{
    return netins_udp_observer_mark_handshake_finished(observer);
}

int
netins_udp_backend_connection_closing(netins_udp_observer_t *observer)
{
    return netins_udp_observer_mark_closing(observer);
}

int
netins_udp_backend_connection_closed(netins_udp_observer_t *observer,
    int conn_error)
{
    return netins_udp_observer_mark_closed(observer, conn_error);
}

int
netins_udp_backend_connection_abandoned(netins_udp_observer_t *observer,
    int conn_error)
{
    return netins_udp_observer_abandon(observer, conn_error);
}

int
netins_udp_backend_connection_error(netins_udp_observer_t *observer,
    int error_code, const char *error_message)
{
    return netins_udp_observer_mark_error(observer, error_code, error_message);
}

int
netins_udp_backend_set_path_metadata(netins_udp_observer_t *observer,
    uint64_t path_id, const char *interface_name,
    const struct sockaddr *local_addr, socklen_t local_addr_len,
    const struct sockaddr *peer_addr, socklen_t peer_addr_len)
{
    return netins_udp_observer_set_path_metadata(observer, path_id,
        interface_name, local_addr, local_addr_len, peer_addr, peer_addr_len);
}

int
netins_udp_backend_sync_xquic_path(netins_udp_observer_t *observer,
    xqc_connection_t *conn, uint64_t path_id, const char *interface_name)
{
    if (observer == NULL || conn == NULL) {
        return NETINS_UDP_ERROR;
    }

    struct sockaddr_storage local_addr;
    struct sockaddr_storage peer_addr;
    socklen_t local_addr_len = 0;
    socklen_t peer_addr_len = 0;
    memset(&local_addr, 0, sizeof(local_addr));
    memset(&peer_addr, 0, sizeof(peer_addr));

    const struct sockaddr *local = NULL;
    const struct sockaddr *peer = NULL;
    if (xqc_path_get_local_addr(conn, path_id,
        (struct sockaddr *)&local_addr, sizeof(local_addr),
        &local_addr_len) == XQC_OK)
    {
        local = (const struct sockaddr *)&local_addr;
    }
    if (xqc_path_get_peer_addr(conn, path_id,
        (struct sockaddr *)&peer_addr, sizeof(peer_addr),
        &peer_addr_len) == XQC_OK)
    {
        peer = (const struct sockaddr *)&peer_addr;
    }

    return netins_udp_backend_set_path_metadata(observer, path_id,
        interface_name, local, local_addr_len, peer, peer_addr_len);
}
