#ifndef NETINS_UDP_BACKEND_H
#define NETINS_UDP_BACKEND_H

#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

#include "netins_udp_legacy.h"
#include <xquic/xquic.h>

/*
 * Shared host adapter used by mini and by standalone NetIns objects.
 *
 * XQUIC engine/socket/connection execution remains owned by each host. This
 * adapter centralizes observer lifecycle calls, path metadata helpers and the
 * bounded/fair RX policy; it does not replace either host's event loop.
 */
/*
 * Internal receive-service limits. They bound work performed in one event-loop
 * dispatch to balance feedback latency with batched receive efficiency. These
 * values are implementation details and are not part of the public API/ABI.
 */
#define NETINS_UDP_BACKEND_RX_BATCH_CAPACITY     256U
#define NETINS_UDP_BACKEND_RX_RESPONSIVE_BUDGET   40U

typedef struct netins_udp_backend_rx_policy_s {
    uint32_t batch_capacity;
    uint32_t responsive_packet_budget;
    uint32_t bulk_packet_budget;
} netins_udp_backend_rx_policy_t;

void netins_udp_backend_rx_policy_init(
    netins_udp_backend_rx_policy_t *policy);
uint32_t netins_udp_backend_rx_packet_limit(
    const netins_udp_backend_rx_policy_t *policy, size_t active_path_count);
size_t netins_udp_backend_next_path_start(size_t current, size_t path_capacity);

int netins_udp_backend_connection_bind(netins_udp_observer_t *observer,
    xqc_engine_t *engine, const xqc_cid_t *cid, netins_udp_role_t role,
    int enable_payload_metrics);
int netins_udp_backend_connection_update_cid(netins_udp_observer_t *observer,
    const xqc_cid_t *cid);
int netins_udp_backend_connection_refresh(netins_udp_observer_t *observer);
int netins_udp_backend_connection_handshake_finished(
    netins_udp_observer_t *observer);
int netins_udp_backend_connection_closing(netins_udp_observer_t *observer);
int netins_udp_backend_connection_closed(netins_udp_observer_t *observer,
    int conn_error);
int netins_udp_backend_connection_abandoned(netins_udp_observer_t *observer,
    int conn_error);
int netins_udp_backend_connection_error(netins_udp_observer_t *observer,
    int error_code, const char *error_message);

int netins_udp_backend_set_path_metadata(netins_udp_observer_t *observer,
    uint64_t path_id, const char *interface_name,
    const struct sockaddr *local_addr, socklen_t local_addr_len,
    const struct sockaddr *peer_addr, socklen_t peer_addr_len);
int netins_udp_backend_sync_xquic_path(netins_udp_observer_t *observer,
    xqc_connection_t *conn, uint64_t path_id, const char *interface_name);

#endif /* NETINS_UDP_BACKEND_H */
