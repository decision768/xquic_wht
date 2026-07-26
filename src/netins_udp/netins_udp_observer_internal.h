#ifndef NETINS_UDP_OBSERVER_INTERNAL_H
#define NETINS_UDP_OBSERVER_INTERNAL_H

#include <sys/socket.h>

#include <xquic/netins_udp.h>
#include <xquic/xquic.h>

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

#endif /* NETINS_UDP_OBSERVER_INTERNAL_H */
