#ifndef NETINS_UDP_CLIENT_INTERNAL_H
#define NETINS_UDP_CLIENT_INTERNAL_H

#include <stdint.h>

#include <xquic/netins_udp.h>
#include <xquic/xquic.h>

/*
 * Private controls used by the V2 lifecycle adapter.  The public V1 client
 * continues to use the same defaults and blocking connect behavior.
 */
int netins_udp_client_configure_transport(netins_udp_client_t *client,
    const xqc_cong_ctrl_callback_t *congestion_control,
    const xqc_scheduler_callback_t *scheduler,
    uint32_t idle_timeout_ms, uint32_t close_timeout_ms);
void netins_udp_client_set_close_timeout(netins_udp_client_t *client,
    uint32_t close_timeout_ms);
int netins_udp_client_start_async(netins_udp_client_t *client);
void netins_udp_client_request_close(netins_udp_client_t *client);

#endif /* NETINS_UDP_CLIENT_INTERNAL_H */
