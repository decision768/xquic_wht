#ifndef NETINS_UDP_V2_INTERNAL_H
#define NETINS_UDP_V2_INTERNAL_H

#include <stdint.h>

#include <xquic/netins_udp.h>
#include "netins_udp_v2_notifier.h"
#include "netins_udp_v2_transport.h"

typedef struct netins_udp_v2_h3_parent_snapshot_s {
    netins_udp_v2_transport_client_t *transport;
    int nonblock;
    uint32_t send_timeout_ms;
    uint32_t recv_timeout_ms;
    uint32_t close_timeout_ms;
} netins_udp_v2_h3_parent_snapshot_t;

typedef struct netins_udp_v2_h3_server_parent_snapshot_s {
    netins_udp_v2_transport_server_connection_t *transport;
    int nonblock;
    uint32_t send_timeout_ms;
    uint32_t recv_timeout_ms;
    uint32_t close_timeout_ms;
} netins_udp_v2_h3_server_parent_snapshot_t;

/*
 * Holds one active connection call until connection_release(). This prevents
 * netins_udp_close() from releasing the transport while a child request is
 * being created.
 */
int netins_udp_v2_connection_acquire_h3(
    netins_udp_t *netins,
    netins_udp_v2_h3_parent_snapshot_t *out);
int netins_udp_v2_connection_acquire_h3_server(
    netins_udp_t *netins,
    netins_udp_v2_h3_server_parent_snapshot_t *out);
int netins_udp_v2_connection_select_h3(netins_udp_t *netins);
void netins_udp_v2_connection_release(netins_udp_t *netins);
void netins_udp_v2_connection_set_error(
    netins_udp_t *netins, int error_code, const char *message);

/* Private data lanes mirror readiness onto the connection notifier. */
void netins_udp_v2_h3_request_set_parent_notifier(
    netins_udp_h3_request_t *request,
    netins_udp_v2_notifier_t *parent_notifier);
int netins_udp_v2_h3_request_finish_send(
    netins_udp_h3_request_t *request);
int netins_udp_v2_h3_request_open_internal(netins_udp_t *netins,
    const netins_udp_h3_request_config_t *config,
    netins_udp_h3_request_t **out_request);
int netins_udp_v2_h3_request_accept_internal(netins_udp_t *netins,
    netins_udp_h3_request_t **out_request);
int netins_udp_v2_h3_request_accept_internal_dontwait(
    netins_udp_t *netins, netins_udp_h3_request_t **out_request);

/* Child handles participate in process cleanup's EBUSY accounting. */
int netins_udp_v2_child_handle_acquire(void);
void netins_udp_v2_child_handle_release(void);

#endif /* NETINS_UDP_V2_INTERNAL_H */
