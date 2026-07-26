#ifndef NETINS_UDP_BACKEND_H
#define NETINS_UDP_BACKEND_H

#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

#include <xquic/netins_udp.h>
#include <xquic/xquic.h>

/*
 * Shared host adapter used by mini and by standalone NetIns objects.
 *
 * XQUIC engine execution remains owned by the host event loop.  This backend
 * centralizes the lifecycle, path metadata and bounded/fair RX policy so the
 * two hosts do not acquire different externally visible semantics.
 */
#define NETINS_UDP_BACKEND_RX_BATCH_CAPACITY 256U

typedef struct netins_udp_backend_rx_policy_s {
    uint32_t batch_capacity;
    uint32_t single_path_packets;
    uint32_t multipath_packets_per_path;
} netins_udp_backend_rx_policy_t;

void netins_udp_backend_rx_policy_init(
    netins_udp_backend_rx_policy_t *policy);
uint32_t netins_udp_backend_rx_packet_limit(
    const netins_udp_backend_rx_policy_t *policy, size_t active_path_count);
size_t netins_udp_backend_next_path_start(size_t current, size_t path_capacity);

int netins_udp_backend_connection_bind(netins_udp_observer_t *observer,
    xqc_engine_t *engine, const xqc_cid_t *cid, netins_udp_role_t role);
int netins_udp_backend_connection_update_cid(netins_udp_observer_t *observer,
    const xqc_cid_t *cid);
int netins_udp_backend_connection_refresh(netins_udp_observer_t *observer);
int netins_udp_backend_connection_handshake_finished(
    netins_udp_observer_t *observer);
int netins_udp_backend_connection_closing(netins_udp_observer_t *observer);
int netins_udp_backend_connection_closed(netins_udp_observer_t *observer,
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
