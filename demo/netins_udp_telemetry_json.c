#include <xquic/netins_udp.h>

#ifdef NETINS_UDP_TELEMETRY_CONFIG_INITIALIZER
#include "netins_udp_telemetry_json.h"

#include <errno.h>
#include <inttypes.h>
#include <string.h>

static int
write_json_string(FILE *file, const char *value)
{
    if (fputc('"', file) == EOF) {
        return -1;
    }
    const unsigned char *p = (const unsigned char *)(value != NULL ? value : "");
    for (; *p != '\0'; p++) {
        if (*p == '"' || *p == '\\') {
            if (fputc('\\', file) == EOF || fputc(*p, file) == EOF) {
                return -1;
            }
        } else if (*p < 0x20) {
            if (fprintf(file, "\\u%04x", (unsigned)*p) < 0) {
                return -1;
            }
        } else if (fputc(*p, file) == EOF) {
            return -1;
        }
    }
    return fputc('"', file) == EOF ? -1 : 0;
}

int
netins_udp_telemetry_json_open(netins_udp_telemetry_json_t *writer,
    const char *path, int append)
{
    if (writer == NULL || path == NULL || path[0] == '\0') {
        errno = EINVAL;
        return -1;
    }
    memset(writer, 0, sizeof(*writer));
    writer->file = fopen(path, append ? "a" : "w");
    return writer->file != NULL ? 0 : -1;
}

int
netins_udp_telemetry_json_write(void *user_data,
    const netins_udp_telemetry_sample_t *sample)
{
    netins_udp_telemetry_json_t *writer = user_data;
    if (writer == NULL || writer->file == NULL || sample == NULL) {
        return -1;
    }
    FILE *file = writer->file;
    if (fprintf(file, "{\"schema_version\":1,\"attempt_id\":") < 0
        || write_json_string(file, sample->attempt_id) != 0
        || fprintf(file,
            ",\"role\":\"%s\",\"direction\":\"%s\","
            "\"sample_kind\":\"%s\",\"sample_seq\":%" PRIu32
            ",\"scheduled_mono_us\":%" PRIu64
            ",\"actual_mono_us\":%" PRIu64
            ",\"realtime_us\":%" PRIu64
            ",\"lateness_us\":%" PRIu64
            ",\"missed_intervals\":%" PRIu32
            ",\"connection_credit_bytes\":%" PRIu64
            ",\"active_stream_credit_bytes\":%" PRIu64
            ",\"effective_swnd_bytes\":%" PRIu64 ",\"paths\":[",
            sample->role == NETINS_UDP_TELEMETRY_ROLE_SERVER
                ? "server" : "client",
            sample->direction == NETINS_UDP_TRANSFER_GET ? "get" : "post",
            sample->sample_kind == NETINS_UDP_TELEMETRY_START ? "start"
                : sample->sample_kind == NETINS_UDP_TELEMETRY_END
                    ? "end" : "periodic",
            sample->sample_seq, sample->scheduled_mono_us,
            sample->actual_mono_us, sample->realtime_us,
            sample->lateness_us, sample->missed_intervals,
            sample->connection_credit_bytes,
            sample->active_stream_credit_bytes,
            sample->effective_swnd_bytes) < 0)
    {
        return -1;
    }
    for (uint32_t i = 0; i < sample->path_count; i++) {
        const netins_udp_path_telemetry_t *path = &sample->paths[i];
        if ((i > 0 && fputc(',', file) == EOF)
            || fprintf(file,
                "{\"path_id\":%" PRIu64 ",\"path_index\":%" PRIu32
                ",\"path_state\":%d,\"app_status\":%d,"
                "\"interface_name\":",
                path->path_id, path->path_index, path->path_state,
                path->app_status) < 0
            || write_json_string(file, path->interface_name) != 0
            || fprintf(file, ",\"local_address\":") < 0
            || write_json_string(file, path->local_address) != 0
            || fprintf(file,
                ",\"cwnd_bytes\":%" PRIu64
                ",\"effective_cwnd_bytes\":%" PRIu64
                ",\"inflight_packets\":%" PRIu64
                ",\"inflight_bytes\":%" PRIu64
                ",\"srtt_us\":%" PRIu64
                ",\"sent_bytes\":%" PRIu64 "}",
                path->cwnd_bytes, path->effective_cwnd_bytes,
                path->inflight_packets, path->inflight_bytes,
                path->srtt_us, path->sent_bytes) < 0)
        {
            return -1;
        }
    }
    if (fputs("]}\n", file) == EOF || fflush(file) != 0) {
        return -1;
    }
    return 0;
}

void
netins_udp_telemetry_json_close(netins_udp_telemetry_json_t *writer)
{
    if (writer != NULL && writer->file != NULL) {
        (void)fflush(writer->file);
        (void)fclose(writer->file);
        writer->file = NULL;
    }
}
#endif
