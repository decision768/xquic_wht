#include "netins_udp_telemetry_json.h"
#include <xquic/xqc_observer_clock.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <inttypes.h>
#include <sys/time.h>
#include <unistd.h>

#define QUEUE_CAP 64U
#define RECORD_CAP 32768U
#define SEGMENT_BYTES (4U * 1024U * 1024U)
#define FLUSH_BYTES (256U * 1024U)
#define FLUSH_US UINT64_C(100000)
enum { WRITER_TRANSPORT, WRITER_EVENT, WRITER_CHECKPOINT, WRITER_CLOSE };
struct netins_telemetry_writer_s {
    sem_t ready;
    pthread_t worker;
    char prefix[1400];
    char records[QUEUE_CAP][RECORD_CAP];
    size_t lengths[QUEUE_CAP];
    unsigned char kinds[QUEUE_CAP];
    unsigned head, tail, stop, peak;
    uint64_t dropped, queue_full, oversize, rejected, write_errors;
    uint64_t written, family_written[2], last_flush_us;
    int failed;
    FILE *files[2];
    unsigned segments[2], checkpoints;
    size_t bytes[2], unflushed;
};

static void
writer_fail(netins_telemetry_writer_t *w, int error_code)
{
    int expected = 0;
    __atomic_compare_exchange_n(&w->failed, &expected, error_code ? error_code : EIO,
        0, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
    __atomic_add_fetch(&w->write_errors, 1, __ATOMIC_RELAXED);
}

static void
writer_path(netins_telemetry_writer_t *w, int event, int active, char *out, size_t size)
{
    snprintf(out, size, "%s.%s.%06u.jsonl%s", w->prefix,
        event ? "events" : "transport", w->segments[event], active ? ".active" : "");
}

static int
writer_seal(netins_telemetry_writer_t *w, int event)
{
    if (!w->files[event]) { return 0; }
    int error_code = 0;
    if (fflush(w->files[event]) != 0) { error_code = errno ? errno : EIO; }
    if (fclose(w->files[event]) != 0 && !error_code) { error_code = errno ? errno : EIO; }
    w->files[event] = NULL;
    if (error_code) { errno = error_code; return -1; }
    char active[1536], sealed[1536];
    writer_path(w, event, 1, active, sizeof(active));
    writer_path(w, event, 0, sealed, sizeof(sealed));
    if (rename(active, sealed) != 0) { return -1; }
    w->segments[event]++; w->bytes[event] = 0;
    return 0;
}

static void
writer_flush(netins_telemetry_writer_t *w)
{
    for (int i = 0; i < 2; ++i) {
        if (w->files[i] && fflush(w->files[i]) != 0) { writer_fail(w, errno); }
    }
    w->unflushed = 0;
    w->last_flush_us = xqc_observer_clock_us();
    if (!w->last_flush_us) { writer_fail(w, EIO); }
}

static void
writer_quote(FILE *f, const char *s)
{
    fputc('"', f);
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
        if (*p == '"' || *p == '\\') { fputc('\\', f); fputc(*p, f); }
        else if (*p < 32) { fprintf(f, "\\u%04x", (unsigned)*p); }
        else { fputc(*p, f); }
    }
    fputc('"', f);
}

/* Only the consumer writes receipts. A receipt is published after both files
 * have been sealed, so its cumulative counts describe immutable segments. */
static void
writer_receipt(netins_telemetry_writer_t *w, const char *metadata)
{
    for (int i = 0; i < 2; ++i) {
        if (writer_seal(w, i)) { writer_fail(w, errno); }
    }
    w->unflushed = 0;
    char path[1536], active[1544], boot[80] = "unknown";
    if (metadata) {
        snprintf(path, sizeof(path), "%s.checkpoint.%06u.json", w->prefix, w->checkpoints++);
    } else {
        snprintf(path, sizeof(path), "%s.health.json", w->prefix);
    }
    snprintf(active, sizeof(active), "%s.active", path);
    FILE *f = fopen("/proc/sys/kernel/random/boot_id", "r");
    if (f) { if (!fgets(boot, sizeof(boot), f)) { strcpy(boot, "unknown"); } fclose(f); }
    boot[strcspn(boot, "\r\n")] = 0;
    f = fopen(active, "wx");
    if (!f) { writer_fail(w, errno); return; }
    fprintf(f, "{\"observer_revision\":2,\"clock_domain\":\"CLOCK_MONOTONIC\",\"boot_id\":");
    writer_quote(f, boot);
    fprintf(f, ",\"writer_prefix\":"); writer_quote(f, w->prefix);
    fprintf(f, ",\"written_records\":%"PRIu64",\"transport_records\":%"PRIu64
        ",\"event_records\":%"PRIu64",\"dropped_records\":%"PRIu64
        ",\"queue_full_drops\":%"PRIu64",\"oversize_drops\":%"PRIu64
        ",\"rejected_records\":%"PRIu64",\"write_errors\":%"PRIu64
        ",\"queue_peak\":%u,\"writer_error\":%d,\"sealed_transport_segments\":%u"
        ",\"sealed_event_segments\":%u%s%s}\n",
        w->written, w->family_written[0], w->family_written[1],
        __atomic_load_n(&w->dropped, __ATOMIC_RELAXED),
        __atomic_load_n(&w->queue_full, __ATOMIC_RELAXED),
        __atomic_load_n(&w->oversize, __ATOMIC_RELAXED),
        __atomic_load_n(&w->rejected, __ATOMIC_RELAXED),
        __atomic_load_n(&w->write_errors, __ATOMIC_RELAXED),
        __atomic_load_n(&w->peak, __ATOMIC_RELAXED),
        __atomic_load_n(&w->failed, __ATOMIC_RELAXED), w->segments[0], w->segments[1],
        metadata ? "," : "", metadata ? metadata : "");
    int error_code = ferror(f) ? EIO : 0;
    if (fclose(f) != 0 && !error_code) { error_code = errno ? errno : EIO; }
    if (error_code || rename(active, path) != 0) { writer_fail(w, error_code ? error_code : errno); }
}

static void *
writer_worker(void *arg)
{
    netins_telemetry_writer_t *w = arg;
    char record[RECORD_CAP];
    w->last_flush_us = xqc_observer_clock_us();
    if (!w->last_flush_us) { writer_fail(w, EIO); }
    for (;;) {
        unsigned head = __atomic_load_n(&w->head, __ATOMIC_RELAXED);
        unsigned tail = __atomic_load_n(&w->tail, __ATOMIC_ACQUIRE);
        if (head == tail) {
            if (__atomic_load_n(&w->stop, __ATOMIC_ACQUIRE)) {
                if (head == __atomic_load_n(&w->tail, __ATOMIC_ACQUIRE)) { break; }
                continue;
            }
            struct timespec deadline;
            if (clock_gettime(CLOCK_REALTIME, &deadline)) { writer_fail(w, errno); break; }
            deadline.tv_nsec += 100000000L;
            if (deadline.tv_nsec >= 1000000000L) { deadline.tv_nsec -= 1000000000L; deadline.tv_sec++; }
            while (sem_timedwait(&w->ready, &deadline) && errno == EINTR) {}
        } else {
            unsigned slot = head % QUEUE_CAP;
            unsigned kind = w->kinds[slot];
            size_t len = w->lengths[slot];
            memcpy(record, w->records[slot], len + 1);
            __atomic_store_n(&w->head, head + 1, __ATOMIC_RELEASE);
            /* Drain the corresponding wake token when available. */
            (void)sem_trywait(&w->ready);
            if (kind == WRITER_CLOSE) { break; }
            if (kind == WRITER_CHECKPOINT) { writer_receipt(w, record); continue; }
            if (__atomic_load_n(&w->failed, __ATOMIC_RELAXED)) {
                __atomic_add_fetch(&w->dropped, 1, __ATOMIC_RELAXED); continue;
            }
            int event = kind == WRITER_EVENT;
            if (!w->files[event]) {
                char path[1536]; writer_path(w, event, 1, path, sizeof(path));
                w->files[event] = fopen(path, "wx");
            }
            if (!w->files[event] || fwrite(record, 1, len, w->files[event]) != len
                || fputc('\n', w->files[event]) == EOF) {
                writer_fail(w, errno); __atomic_add_fetch(&w->dropped, 1, __ATOMIC_RELAXED); continue;
            }
            w->written++; w->family_written[event]++;
            w->bytes[event] += len + 1; w->unflushed += len + 1;
            if (w->bytes[event] >= SEGMENT_BYTES && writer_seal(w, event)) { writer_fail(w, errno); }
        }
        uint64_t now = xqc_observer_clock_us();
        if (!now) { writer_fail(w, EIO); }
        if (w->unflushed && (w->unflushed >= FLUSH_BYTES || now - w->last_flush_us >= FLUSH_US)) {
            writer_flush(w);
        }
    }
    writer_receipt(w, NULL);
    return NULL;
}

netins_telemetry_writer_t *
netins_telemetry_open(const char *prefix)
{
    if (!prefix || strlen(prefix) > 1200) { return NULL; }
    netins_telemetry_writer_t *w = calloc(1, sizeof(*w));
    if (!w) { return NULL; }
    struct timeval tv;
    if (gettimeofday(&tv, NULL)) { free(w); return NULL; }
    static unsigned serial;
    snprintf(w->prefix, sizeof(w->prefix), "%s.%ld.%ld.%ld.%u", prefix,
        (long)getpid(), (long)tv.tv_sec, (long)tv.tv_usec,
        __atomic_add_fetch(&serial, 1, __ATOMIC_RELAXED));
    if (sem_init(&w->ready, 0, 0)) { free(w); return NULL; }
    if (pthread_create(&w->worker, NULL, writer_worker, w)) {
        sem_destroy(&w->ready); free(w); return NULL;
    }
    return w;
}

static int
writer_enqueue(netins_telemetry_writer_t *w, unsigned kind, const char *record)
{
    if (!w || !record) { return -1; }
    size_t len = strlen(record);
    if (len >= RECORD_CAP) {
        __atomic_add_fetch(&w->oversize, 1, __ATOMIC_RELAXED);
        if (kind < WRITER_CHECKPOINT) { __atomic_add_fetch(&w->dropped, 1, __ATOMIC_RELAXED); }
        return -1;
    }
    if (kind < WRITER_CHECKPOINT && (__atomic_load_n(&w->failed, __ATOMIC_RELAXED)
        || __atomic_load_n(&w->stop, __ATOMIC_ACQUIRE))) {
        __atomic_add_fetch(&w->rejected, 1, __ATOMIC_RELAXED);
        __atomic_add_fetch(&w->dropped, 1, __ATOMIC_RELAXED); return -1;
    }
    unsigned tail = __atomic_load_n(&w->tail, __ATOMIC_RELAXED);
    unsigned count = tail - __atomic_load_n(&w->head, __ATOMIC_ACQUIRE);
    unsigned limit = kind < WRITER_CHECKPOINT ? QUEUE_CAP - 2 : kind == WRITER_CHECKPOINT ? QUEUE_CAP - 1 : QUEUE_CAP;
    if (count >= limit) {
        __atomic_add_fetch(&w->queue_full, 1, __ATOMIC_RELAXED);
        if (kind < WRITER_CHECKPOINT) { __atomic_add_fetch(&w->dropped, 1, __ATOMIC_RELAXED); }
        else { writer_fail(w, ENOBUFS); }
        return -1;
    }
    unsigned slot = tail % QUEUE_CAP;
    memcpy(w->records[slot], record, len + 1);
    w->lengths[slot] = len; w->kinds[slot] = (unsigned char)kind;
    if (count + 1 > __atomic_load_n(&w->peak, __ATOMIC_RELAXED)) {
        __atomic_store_n(&w->peak, count + 1, __ATOMIC_RELAXED);
    }
    __atomic_store_n(&w->tail, tail + 1, __ATOMIC_RELEASE);
    if (sem_post(&w->ready)) { writer_fail(w, errno); }
    return 0;
}

int
netins_telemetry_write(void *arg, int event, const char *record)
{
    return writer_enqueue(arg, event ? WRITER_EVENT : WRITER_TRANSPORT, record);
}

int
netins_telemetry_checkpoint(netins_telemetry_writer_t *w, const char *metadata)
{
    return writer_enqueue(w, WRITER_CHECKPOINT, metadata);
}

void
netins_telemetry_close(netins_telemetry_writer_t *w)
{
    if (!w) { return; }
    (void)writer_enqueue(w, WRITER_CLOSE, "");
    __atomic_store_n(&w->stop, 1, __ATOMIC_RELEASE);
    (void)sem_post(&w->ready);
    pthread_join(w->worker, NULL);
    if (__atomic_load_n(&w->failed, __ATOMIC_RELAXED) || __atomic_load_n(&w->dropped, __ATOMIC_RELAXED)) {
        fprintf(stderr, "observer_error: writer=%d dropped=%"PRIu64" prefix=%s\n",
            __atomic_load_n(&w->failed, __ATOMIC_RELAXED),
            __atomic_load_n(&w->dropped, __ATOMIC_RELAXED), w->prefix);
    }
    sem_destroy(&w->ready); free(w);
}
