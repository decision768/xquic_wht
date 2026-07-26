#ifndef NETINS_UDP_TRANSPORT_METRICS_H
#define NETINS_UDP_TRANSPORT_METRICS_H

#include <stddef.h>
#include <stdint.h>

#include "netins_udp_legacy.h"
#include <xquic/xquic.h>

/*
 * Private adapter between NetIns and XQUIC transport internals.
 *
 * This structure is deliberately not installed as a public header.  New
 * diagnostic fields belong here instead of xqc_conn_stats_t so that the
 * XQUIC public ABI and xqc_conn_get_stats() semantics remain unchanged.
 * Collect only from the thread that owns/drives the XQUIC engine.
 */
typedef struct netins_udp_transport_path_metrics_s {
    uint64_t path_id;
    netins_udp_path_state_t path_state;
    netins_udp_app_path_status_t app_path_status;

    uint64_t packets_sent;
    uint64_t packets_recv;

    /* Encoded packet bytes used by the legacy diagnostics. */
    uint64_t app_packet_bytes_sent;
    uint64_t wire_bytes_sent;
    uint64_t wire_bytes_recv;
    uint64_t delivered_bytes;
    uint64_t reinjected_bytes_sent;
    uint64_t retransmitted_bytes_sent;
    uint64_t app_packet_bytes_recv;
    uint64_t reinjected_bytes_recv;

    /*
     * De-duplicated QUIC STREAM payload.  Receive bytes are counted only when
     * xqc_stream_recv() moves previously unread stream offsets upward.
     */
    uint64_t stream_payload_bytes_sent_unique;
    uint64_t stream_payload_bytes_recv_unique;
    uint64_t stream_payload_bytes_recv_unique_reinject;

    uint64_t srtt_us;
    uint64_t latest_rtt_us;
    uint64_t min_rtt_us;
    uint64_t cwnd_bytes;
    uint64_t inflight_bytes;
    uint64_t cwnd_block_count;
    uint64_t cwnd_block_duration_us;
    uint64_t last_received_time_us;
    uint64_t last_sent_time_us;
    uint64_t lost_packets;
    int cwnd_limited;
} netins_udp_transport_path_metrics_t;

typedef struct netins_udp_transport_metrics_s {
    /*
     * Legacy connection-level data and diagnostic strings.  Its embedded
     * paths retain the original xqc_conn_get_stats() meaning and are not used
     * for NetIns extended path statistics.
     */
    xqc_conn_stats_t legacy;
    size_t path_count;
    netins_udp_transport_path_metrics_t paths[XQC_MAX_PATHS_COUNT];
} netins_udp_transport_metrics_t;

int netins_udp_transport_metrics_collect(xqc_engine_t *engine,
    const xqc_cid_t *cid, netins_udp_transport_metrics_t *metrics);

#endif /* NETINS_UDP_TRANSPORT_METRICS_H */
