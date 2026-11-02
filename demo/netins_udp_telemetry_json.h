#ifndef NETINS_UDP_TELEMETRY_JSON_H
#define NETINS_UDP_TELEMETRY_JSON_H
typedef struct netins_telemetry_writer_s netins_telemetry_writer_t;
netins_telemetry_writer_t *netins_telemetry_open(const char *);
int netins_telemetry_write(void *, int, const char *);
void netins_telemetry_close(netins_telemetry_writer_t *);
#endif
