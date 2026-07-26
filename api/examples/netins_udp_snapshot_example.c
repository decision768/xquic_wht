#include <inttypes.h>
#include <stdio.h>

#include <xquic/netins_udp.h>

/*
 * Print a coherent point-in-time snapshot for a client or accepted
 * connection owned by the caller.
 */
int
netins_udp_print_snapshot(netins_udp_t *connection)
{
    netins_udp_snapshot_t snapshot = NETINS_UDP_SNAPSHOT_INITIALIZER;
    if (netins_udp_get_snapshot(connection, &snapshot) != NETINS_UDP_OK)
    {
        return NETINS_UDP_ERROR;
    }

    printf("udp connection=%" PRIu64 " state=%d paths=%u"
           " tx=%.3fMbps rx=%.3fMbps\n",
        snapshot.info.connection_id, snapshot.info.state,
        snapshot.path_count, snapshot.statis.send_mbps,
        snapshot.statis.receive_mbps);

    for (size_t path_index = 0;
         path_index < snapshot.path_count; ++path_index)
    {
        const netins_udp_path_statis_t *path =
            &snapshot.paths[path_index];
        printf("  index=%u id=%" PRIu64 " state=%d if=%s"
               " local=%s peer=%s tx=%" PRIu64 " rx=%" PRIu64
               " srtt=%" PRIu64 "us cwnd=%" PRIu64
               " inflight=%" PRIu64 "\n",
            path->path_index, path->path_id, path->path_state,
            path->interface_name, path->local_address,
            path->peer_address, path->bytes_sent,
            path->bytes_received, path->srtt_us, path->cwnd_bytes,
            path->bytes_inflight);
    }

    return NETINS_UDP_OK;
}
