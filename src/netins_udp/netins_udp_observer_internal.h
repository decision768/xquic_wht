#ifndef NETINS_UDP_OBSERVER_INTERNAL_H
#define NETINS_UDP_OBSERVER_INTERNAL_H

#include <sys/socket.h>

#include "netins_udp_legacy.h"
#include <xquic/xquic.h>

/*
 * NetIns-private extended counters. They deliberately stay outside both the
 * compatibility observer structures and xqc_conn_stats_t. Array indexes are
 * aligned with the observer's stable path slots.
 */
typedef struct netins_udp_observer_extended_path_s {
    uint64_t path_id;
    uint64_t stream_payload_bytes_sent_unique;
    uint64_t stream_payload_bytes_recv_unique;
    uint64_t wire_bytes_recv;
    uint64_t retransmitted_bytes_sent;
    uint64_t cwnd_bytes;
    uint64_t inflight_bytes;
    uint64_t latest_rtt_us;
    uint64_t min_rtt_us;
    uint64_t last_received_time_us;
    uint64_t last_sent_time_us;
    uint64_t lost_packets;
    double unique_send_mbps;
    double unique_recv_mbps;
    double wire_recv_mbps;
    int cwnd_limited;
} netins_udp_observer_extended_path_t;

typedef struct netins_udp_observer_extended_snapshot_s {
    uint32_t path_count;
    uint32_t path_table_truncated;
    netins_udp_observer_extended_path_t paths[NETINS_UDP_MAX_PATHS];
} netins_udp_observer_extended_snapshot_t;

typedef struct netins_udp_observer_private_snapshot_s {
    netins_udp_snapshot_t legacy;
    netins_udp_observer_extended_snapshot_t extended;
} netins_udp_observer_private_snapshot_t;

/* Host-side lifecycle functions. Call these from the XQUIC engine thread. */
int netins_udp_observer_bind(netins_udp_observer_t *netins, xqc_engine_t *engine,
    const xqc_cid_t *cid, netins_udp_role_t role);
int netins_udp_observer_update_cid(netins_udp_observer_t *netins, const xqc_cid_t *cid);
int netins_udp_observer_refresh(netins_udp_observer_t *netins);
int netins_udp_observer_mark_handshake_finished(netins_udp_observer_t *netins);
int netins_udp_observer_mark_closing(netins_udp_observer_t *netins);
int netins_udp_observer_mark_closed(netins_udp_observer_t *netins, int conn_error);
int netins_udp_observer_mark_error(netins_udp_observer_t *netins, int error_code,
    const char *error_message);

int netins_udp_observer_set_path_metadata(netins_udp_observer_t *netins, uint64_t path_id,
    const char *interface_name, const struct sockaddr *local_addr,
    socklen_t local_addr_len, const struct sockaddr *peer_addr,
    socklen_t peer_addr_len);
int netins_udp_observer_get_extended_snapshot(
    netins_udp_observer_t *netins,
    netins_udp_observer_extended_snapshot_t *out);
int netins_udp_observer_get_private_snapshot(
    netins_udp_observer_t *netins,
    netins_udp_observer_private_snapshot_t *out);

#endif /* NETINS_UDP_OBSERVER_INTERNAL_H */
