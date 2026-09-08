#ifndef NETINS_UDP_TELEMETRY_JSON_H
#define NETINS_UDP_TELEMETRY_JSON_H

#include <stdio.h>
#include <xquic/netins_udp.h>

typedef struct netins_udp_telemetry_json_s {
    FILE *file;
} netins_udp_telemetry_json_t;

int netins_udp_telemetry_json_open(netins_udp_telemetry_json_t *writer,
    const char *path, int append);
int netins_udp_telemetry_json_write(void *user_data,
    const netins_udp_telemetry_sample_t *sample);
void netins_udp_telemetry_json_close(netins_udp_telemetry_json_t *writer);

#endif
