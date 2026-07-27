#ifdef NDEBUG
#undef NDEBUG
#endif

#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <arpa/inet.h>

#include "netins_udp_observer_internal.h"
#include "netins_udp_transport_metrics.h"

static netins_udp_transport_metrics_t g_mock_metrics;
static int g_payload_metrics_enabled;
static unsigned g_payload_metrics_set_calls;

int
netins_udp_transport_payload_metrics_set_enabled(xqc_engine_t *engine,
    const xqc_cid_t *cid, int enabled)
{
    (void)engine;
    (void)cid;
    g_payload_metrics_enabled = enabled != 0;
    g_payload_metrics_set_calls++;
    return 0;
}

int
netins_udp_transport_metrics_collect(xqc_engine_t *engine,
    const xqc_cid_t *cid, netins_udp_transport_metrics_t *metrics)
{
    (void)engine;
    (void)cid;
    *metrics = g_mock_metrics;
    return 0;
}

static void
prepare_mock_stats(void)
{
    memset(&g_mock_metrics, 0, sizeof(g_mock_metrics));
    for (size_t i = 0; i < XQC_MAX_PATHS_COUNT; ++i) {
        g_mock_metrics.legacy.paths_info[i].path_id = UINT64_MAX;
        g_mock_metrics.paths[i].path_id = UINT64_MAX;
    }

    snprintf(g_mock_metrics.legacy.alpn, sizeof(g_mock_metrics.legacy.alpn),
        "h3");
    snprintf(g_mock_metrics.legacy.conn_info,
        sizeof(g_mock_metrics.legacy.conn_info),
        "mock live connection");
    g_mock_metrics.legacy.enable_multipath = 1;
    g_mock_metrics.legacy.mp_state = 2;
    g_mock_metrics.legacy.send_count = 120;
    g_mock_metrics.legacy.recv_count = 140;
    g_mock_metrics.legacy.lost_count = 2;
    g_mock_metrics.legacy.srtt = 32000;
    g_mock_metrics.legacy.min_rtt = 21000;
    g_mock_metrics.legacy.inflight_bytes = 64000;
    g_mock_metrics.path_count = 2;

    netins_udp_transport_path_metrics_t *path0 = &g_mock_metrics.paths[0];
    path0->path_id = 0;
    path0->path_state = NETINS_UDP_PATH_ACTIVE;
    path0->app_path_status = NETINS_UDP_APP_PATH_AVAILABLE;
    path0->packets_sent = 80;
    path0->packets_recv = 90;
    path0->wire_bytes_sent = 80000;
    path0->app_packet_bytes_sent = 70000;
    path0->delivered_bytes = 65000;
    path0->app_packet_bytes_recv = 90000;
    path0->wire_bytes_recv = 95000;
    path0->stream_payload_bytes_sent_unique = 60000;
    path0->stream_payload_bytes_recv_unique = 85000;
    path0->retransmitted_bytes_sent = 1200;
    path0->srtt_us = 30000;
    path0->latest_rtt_us = 31000;
    path0->min_rtt_us = 20000;
    path0->cwnd_bytes = 128000;
    path0->inflight_bytes = 32000;
    path0->lost_packets = 1;

    netins_udp_transport_path_metrics_t *path1 = &g_mock_metrics.paths[1];
    path1->path_id = 1;
    path1->path_state = NETINS_UDP_PATH_VALIDATING;
    path1->app_path_status = NETINS_UDP_APP_PATH_NONE;
    path1->packets_sent = 40;
    path1->packets_recv = 50;
    path1->wire_bytes_sent = 40000;
    path1->app_packet_bytes_sent = 30000;
    path1->delivered_bytes = 25000;
    path1->app_packet_bytes_recv = 50000;
    path1->wire_bytes_recv = 55000;
    path1->stream_payload_bytes_sent_unique = 20000;
    path1->stream_payload_bytes_recv_unique = 45000;
    path1->retransmitted_bytes_sent = 800;
    path1->srtt_us = 45000;
    path1->latest_rtt_us = 47000;
    path1->min_rtt_us = 35000;
    path1->cwnd_bytes = 96000;
    path1->inflight_bytes = 16000;
    path1->lost_packets = 1;
}

typedef struct reader_context_s {
    netins_udp_observer_t *handle;
    int failed;
} reader_context_t;

static void *
snapshot_reader(void *argument)
{
    reader_context_t *context = argument;
    for (int i = 0; i < 2000; ++i) {
        netins_udp_snapshot_t snapshot = NETINS_UDP_SNAPSHOT_INITIALIZER;
        if (netins_udp_observer_get_snapshot(context->handle, &snapshot)
            != NETINS_UDP_OK
            || snapshot.info.connection_id
                == NETINS_UDP_INVALID_CONNECTION_ID)
        {
            context->failed = 1;
            break;
        }
    }
    return NULL;
}

int
main(void)
{
    prepare_mock_stats();
    assert(netins_udp_get_api_version() == 2U);

    netins_udp_observer_t *host = netins_udp_observer_new();
    assert(host != NULL);

    netins_udp_info_t wrong_version = NETINS_UDP_INFO_INITIALIZER;
    wrong_version.api_version++;
    assert(netins_udp_observer_get_info(host, &wrong_version)
        == NETINS_UDP_VERSION_MISMATCH);

    xqc_cid_t cid;
    memset(&cid, 0, sizeof(cid));
    assert(netins_udp_observer_bind(host, (xqc_engine_t *)(uintptr_t)1, &cid,
        NETINS_UDP_ROLE_CLIENT, 1) == NETINS_UDP_OK);
    assert(g_payload_metrics_enabled == 1);
    assert(g_payload_metrics_set_calls == 1);
    xqc_cid_t updated_cid;
    memset(&updated_cid, 0, sizeof(updated_cid));
    assert(netins_udp_observer_update_cid(host, &updated_cid) == NETINS_UDP_OK);

    struct sockaddr_in local_addr;
    struct sockaddr_in peer_addr;
    memset(&local_addr, 0, sizeof(local_addr));
    memset(&peer_addr, 0, sizeof(peer_addr));
    local_addr.sin_family = AF_INET;
    local_addr.sin_port = htons(41000);
    peer_addr.sin_family = AF_INET;
    peer_addr.sin_port = htons(8443);
    assert(inet_pton(AF_INET, "10.0.0.2", &local_addr.sin_addr) == 1);
    assert(inet_pton(AF_INET, "10.0.0.1", &peer_addr.sin_addr) == 1);
    assert(netins_udp_observer_set_path_metadata(host, 0, "rmnet_data0",
        (const struct sockaddr *)&local_addr, sizeof(local_addr),
        (const struct sockaddr *)&peer_addr, sizeof(peer_addr))
        == NETINS_UDP_OK);

    netins_udp_info_t info = NETINS_UDP_INFO_INITIALIZER;
    assert(netins_udp_observer_get_info(host, &info) == NETINS_UDP_OK);
    assert(info.connection_id != NETINS_UDP_INVALID_CONNECTION_ID);
    assert(info.role == NETINS_UDP_ROLE_CLIENT);
    assert(info.path_count == 2);
    assert(info.active_path_count == 1);
    assert(info.validating_path_count == 1);

    size_t connection_count = 0;
    assert(netins_udp_observer_list_connections(NULL, 0, &connection_count)
        == NETINS_UDP_OK);
    assert(connection_count == 1);

    uint64_t connection_id = 0;
    assert(netins_udp_observer_list_connections(&connection_id, 1,
        &connection_count) == NETINS_UDP_OK);
    assert(connection_id == info.connection_id);

    netins_udp_snapshot_t by_id = NETINS_UDP_SNAPSHOT_INITIALIZER;
    assert(netins_udp_observer_get_snapshot_by_id(connection_id, &by_id)
        == NETINS_UDP_OK);
    assert(by_id.path_count == 2);
    assert(strcmp(by_id.paths[0].interface_name, "rmnet_data0") == 0);
    assert(strcmp(by_id.paths[0].local_address, "10.0.0.2:41000") == 0);
    assert(by_id.statis.bytes_wire_sent == 120000);
    assert(by_id.statis.bytes_recv_effective == 130000);

    netins_udp_observer_private_snapshot_t private_snapshot;
    assert(netins_udp_observer_get_private_snapshot(host, &private_snapshot)
        == NETINS_UDP_OK);
    assert(private_snapshot.legacy.path_count == 2);
    assert(private_snapshot.extended.path_count == 2);
    assert(private_snapshot.extended.paths[0]
        .stream_payload_bytes_sent_unique == 60000);
    assert(private_snapshot.extended.paths[0]
        .stream_payload_bytes_recv_unique == 85000);
    assert(private_snapshot.extended.paths[0].wire_bytes_recv == 95000);
    assert(private_snapshot.extended.paths[0].cwnd_bytes == 128000);
    assert(private_snapshot.extended.paths[0].inflight_bytes == 32000);
    assert(private_snapshot.extended.paths[0].latest_rtt_us == 31000);
    assert(private_snapshot.extended.paths[0].min_rtt_us == 20000);
    assert(private_snapshot.extended.paths[0].lost_packets == 1);

    /* Collector order changes must not change the public path indexes. */
    netins_udp_transport_path_metrics_t swap = g_mock_metrics.paths[0];
    g_mock_metrics.paths[0] = g_mock_metrics.paths[1];
    g_mock_metrics.paths[1] = swap;
    assert(netins_udp_observer_refresh(host) == NETINS_UDP_OK);
    netins_udp_snapshot_t reordered = NETINS_UDP_SNAPSHOT_INITIALIZER;
    assert(netins_udp_observer_get_snapshot(host, &reordered)
        == NETINS_UDP_OK);
    assert(reordered.paths[0].path_id == 0);
    assert(reordered.paths[1].path_id == 1);

    /* Removed paths retain their slots and transition to CLOSED. */
    g_mock_metrics.path_count = 1;
    assert(netins_udp_observer_refresh(host) == NETINS_UDP_OK);
    netins_udp_snapshot_t removed = NETINS_UDP_SNAPSHOT_INITIALIZER;
    assert(netins_udp_observer_get_snapshot(host, &removed) == NETINS_UDP_OK);
    assert(removed.path_count == 2);
    assert(removed.paths[0].path_id == 0);
    assert(removed.paths[0].transport_state == NETINS_UDP_PATH_CLOSED);
    assert(removed.paths[1].path_id == 1);
    assert(removed.info.available_path_count == 0);

    assert(netins_udp_observer_mark_handshake_finished(host) == NETINS_UDP_OK);
    netins_udp_observer_t *third_party = netins_udp_observer_open(connection_id);
    assert(third_party != NULL);

    reader_context_t reader = {third_party, 0};
    pthread_t reader_thread;
    assert(pthread_create(&reader_thread, NULL, snapshot_reader, &reader) == 0);
    assert(pthread_join(reader_thread, NULL) == 0);
    assert(reader.failed == 0);

    assert(netins_udp_observer_mark_closed(host, 0) == NETINS_UDP_OK);
    assert(g_payload_metrics_enabled == 0);
    assert(g_payload_metrics_set_calls == 2);
    assert(netins_udp_observer_list_connections(NULL, 0, &connection_count)
        == NETINS_UDP_OK);
    assert(connection_count == 0);
    assert(netins_udp_observer_open(connection_id) == NULL);

    netins_udp_snapshot_t final_snapshot = NETINS_UDP_SNAPSHOT_INITIALIZER;
    assert(netins_udp_observer_get_snapshot(third_party, &final_snapshot)
        == NETINS_UDP_OK);
    assert(final_snapshot.info.connection_state == NETINS_UDP_STATE_CLOSED);

    netins_udp_observer_close(third_party);
    netins_udp_observer_close(host);
    printf("netins_udp_unit_test: PASS\n");
    return 0;
}
