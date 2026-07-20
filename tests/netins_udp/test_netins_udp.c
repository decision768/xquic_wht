#ifdef NDEBUG
#undef NDEBUG
#endif

#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <arpa/inet.h>

#include "netins_udp_internal.h"

static xqc_conn_stats_t g_mock_stats;

xqc_conn_stats_t
xqc_conn_get_stats(xqc_engine_t *engine, const xqc_cid_t *cid)
{
    (void)engine;
    (void)cid;
    return g_mock_stats;
}

static void
prepare_mock_stats(void)
{
    memset(&g_mock_stats, 0, sizeof(g_mock_stats));
    for (size_t i = 0; i < XQC_MAX_PATHS_COUNT; ++i) {
        g_mock_stats.paths_info[i].path_id = UINT64_MAX;
    }

    snprintf(g_mock_stats.alpn, sizeof(g_mock_stats.alpn), "h3");
    snprintf(g_mock_stats.conn_info, sizeof(g_mock_stats.conn_info),
        "mock live connection");
    g_mock_stats.enable_multipath = 1;
    g_mock_stats.mp_state = 2;
    g_mock_stats.send_count = 120;
    g_mock_stats.recv_count = 140;
    g_mock_stats.lost_count = 2;
    g_mock_stats.srtt = 32000;
    g_mock_stats.min_rtt = 21000;
    g_mock_stats.inflight_bytes = 64000;

    xqc_path_metrics_t *path0 = &g_mock_stats.paths_info[0];
    path0->path_id = 0;
    path0->path_state = NETINS_UDP_PATH_ACTIVE;
    path0->path_app_status = NETINS_UDP_APP_PATH_AVAILABLE;
    path0->path_pkt_send_count = 80;
    path0->path_pkt_recv_count = 90;
    path0->path_wire_send_bytes = 80000;
    path0->path_send_bytes = 70000;
    path0->path_delivered_bytes = 65000;
    path0->path_recv_bytes = 90000;
    path0->path_recv_effective_bytes = 85000;
    path0->path_srtt = 30000;

    xqc_path_metrics_t *path1 = &g_mock_stats.paths_info[1];
    path1->path_id = 1;
    path1->path_state = NETINS_UDP_PATH_VALIDATING;
    path1->path_app_status = NETINS_UDP_APP_PATH_NONE;
    path1->path_pkt_send_count = 40;
    path1->path_pkt_recv_count = 50;
    path1->path_wire_send_bytes = 40000;
    path1->path_send_bytes = 30000;
    path1->path_delivered_bytes = 25000;
    path1->path_recv_bytes = 50000;
    path1->path_recv_effective_bytes = 45000;
    path1->path_srtt = 45000;
}

typedef struct reader_context_s {
    netins_udp_t *handle;
    int failed;
} reader_context_t;

static void *
snapshot_reader(void *argument)
{
    reader_context_t *context = argument;
    for (int i = 0; i < 2000; ++i) {
        netins_udp_snapshot_t snapshot = NETINS_UDP_SNAPSHOT_INITIALIZER;
        if (netins_udp_get_snapshot(context->handle, &snapshot)
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
    assert(netins_udp_get_api_version() == NETINS_UDP_API_VERSION);

    netins_udp_t *host = netins_udp_new();
    assert(host != NULL);

    netins_udp_info_t wrong_version = NETINS_UDP_INFO_INITIALIZER;
    wrong_version.api_version++;
    assert(netins_udp_get_info(host, &wrong_version)
        == NETINS_UDP_VERSION_MISMATCH);

    xqc_cid_t cid;
    memset(&cid, 0, sizeof(cid));
    assert(netins_udp_bind(host, (xqc_engine_t *)(uintptr_t)1, &cid,
        NETINS_UDP_ROLE_CLIENT) == NETINS_UDP_OK);
    xqc_cid_t updated_cid;
    memset(&updated_cid, 0, sizeof(updated_cid));
    assert(netins_udp_update_cid(host, &updated_cid) == NETINS_UDP_OK);

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
    assert(netins_udp_set_path_metadata(host, 0, "rmnet_data0",
        (const struct sockaddr *)&local_addr, sizeof(local_addr),
        (const struct sockaddr *)&peer_addr, sizeof(peer_addr))
        == NETINS_UDP_OK);

    netins_udp_info_t info = NETINS_UDP_INFO_INITIALIZER;
    assert(netins_udp_get_info(host, &info) == NETINS_UDP_OK);
    assert(info.connection_id != NETINS_UDP_INVALID_CONNECTION_ID);
    assert(info.role == NETINS_UDP_ROLE_CLIENT);
    assert(info.path_count == 2);
    assert(info.active_path_count == 1);
    assert(info.validating_path_count == 1);

    size_t connection_count = 0;
    assert(netins_udp_list_connections(NULL, 0, &connection_count)
        == NETINS_UDP_OK);
    assert(connection_count == 1);

    uint64_t connection_id = 0;
    assert(netins_udp_list_connections(&connection_id, 1,
        &connection_count) == NETINS_UDP_OK);
    assert(connection_id == info.connection_id);

    netins_udp_snapshot_t by_id = NETINS_UDP_SNAPSHOT_INITIALIZER;
    assert(netins_udp_get_snapshot_by_id(connection_id, &by_id)
        == NETINS_UDP_OK);
    assert(by_id.path_count == 2);
    assert(strcmp(by_id.paths[0].interface_name, "rmnet_data0") == 0);
    assert(strcmp(by_id.paths[0].local_address, "10.0.0.2:41000") == 0);
    assert(by_id.statis.bytes_wire_sent == 120000);
    assert(by_id.statis.bytes_recv_effective == 130000);

    assert(netins_udp_mark_handshake_finished(host) == NETINS_UDP_OK);
    netins_udp_t *third_party = netins_udp_open(connection_id);
    assert(third_party != NULL);

    reader_context_t reader = {third_party, 0};
    pthread_t reader_thread;
    assert(pthread_create(&reader_thread, NULL, snapshot_reader, &reader) == 0);
    assert(pthread_join(reader_thread, NULL) == 0);
    assert(reader.failed == 0);

    assert(netins_udp_mark_closed(host, 0) == NETINS_UDP_OK);
    assert(netins_udp_list_connections(NULL, 0, &connection_count)
        == NETINS_UDP_OK);
    assert(connection_count == 0);
    assert(netins_udp_open(connection_id) == NULL);

    netins_udp_snapshot_t final_snapshot = NETINS_UDP_SNAPSHOT_INITIALIZER;
    assert(netins_udp_get_snapshot(third_party, &final_snapshot)
        == NETINS_UDP_OK);
    assert(final_snapshot.info.connection_state == NETINS_UDP_STATE_CLOSED);

    netins_udp_close(third_party);
    netins_udp_close(host);
    printf("netins_udp_unit_test: PASS\n");
    return 0;
}
