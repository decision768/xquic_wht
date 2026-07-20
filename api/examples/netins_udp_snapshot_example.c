#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#include <xquic/netins_udp.h>

/*
 * Call this function from a third-party component loaded into mini_client or
 * mini_server. The registry is process-local; running this as another process
 * would inspect a different registry.
 */
void
third_party_poll_udp_connections(void)
{
    size_t count = 0;
    if (netins_udp_list_connections(NULL, 0, &count) != NETINS_UDP_OK
        || count == 0)
    {
        return;
    }

    uint64_t *ids = calloc(count, sizeof(*ids));
    if (ids == NULL) {
        return;
    }

    size_t listed = count;
    int result = netins_udp_list_connections(ids, count, &listed);
    if (result != NETINS_UDP_OK) {
        free(ids);
        return;
    }

    for (size_t i = 0; i < listed; ++i) {
        netins_udp_snapshot_t snapshot = NETINS_UDP_SNAPSHOT_INITIALIZER;
        if (netins_udp_get_snapshot_by_id(ids[i], &snapshot)
            != NETINS_UDP_OK)
        {
            continue;
        }

        printf("udp connection=%" PRIu64 " state=%s paths=%u"
               " tx=%.3fMbps rx=%.3fMbps\n",
            snapshot.info.connection_id,
            netins_udp_conn_state_name(snapshot.info.connection_state),
            snapshot.path_count,
            snapshot.statis.wire_send_mbps,
            snapshot.statis.effective_recv_mbps);

        for (size_t path_index = 0;
             path_index < snapshot.path_count; ++path_index)
        {
            const netins_udp_path_statis_t *path =
                &snapshot.paths[path_index];
            printf("  path=%" PRIu64 " state=%s if=%s local=%s peer=%s"
                   " delivered=%" PRIu64 " effective_rx=%" PRIu64 "\n",
                path->path_id,
                netins_udp_path_state_name(path->transport_state),
                path->interface_name,
                path->local_address,
                path->peer_address,
                path->bytes_delivered,
                path->bytes_recv_effective);
        }
    }

    free(ids);
}
