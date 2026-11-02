#include "netins_udp_telemetry_json.h"
#include <pthread.h>
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
struct netins_telemetry_writer_s {
    pthread_mutex_t lock;
    pthread_cond_t ready;
    pthread_t worker;
    char prefix[1400];
    char records[QUEUE_CAP][RECORD_CAP];
    int events[QUEUE_CAP];
    unsigned head, count, stop;
    uint64_t dropped, written;
    int failed;
    FILE *files[2];
    unsigned segments[2];
    size_t bytes[2];
};

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

static void *
writer_worker(void *arg)
{
    netins_telemetry_writer_t *w = arg;
    char record[RECORD_CAP];
    for (;;) {
        pthread_mutex_lock(&w->lock);
        while (!w->count && !w->stop) { pthread_cond_wait(&w->ready, &w->lock); }
        if (!w->count && w->stop) { pthread_mutex_unlock(&w->lock); break; }
        unsigned slot = w->head;
        int event = w->events[slot];
        memcpy(record, w->records[slot], RECORD_CAP);
        w->head = (w->head + 1) % QUEUE_CAP; w->count--;
        pthread_mutex_unlock(&w->lock);
        if (__atomic_load_n(&w->failed, __ATOMIC_RELAXED)) {
            __atomic_add_fetch(&w->dropped, 1, __ATOMIC_RELAXED); continue;
        }
        if (!w->files[event]) {
            char path[1536]; writer_path(w, event, 1, path, sizeof(path));
            w->files[event] = fopen(path, "wx");
        }
        size_t len = strlen(record);
        if (!w->files[event] || fwrite(record, 1, len, w->files[event]) != len
            || fputc('\n', w->files[event]) == EOF || fflush(w->files[event]) != 0)
        {
            __atomic_store_n(&w->failed, errno ? errno : EIO, __ATOMIC_RELAXED);
            __atomic_add_fetch(&w->dropped, 1, __ATOMIC_RELAXED); continue;
        }
        w->written++; w->bytes[event] += len + 1;
        if (w->bytes[event] >= SEGMENT_BYTES && writer_seal(w, event)) {
            __atomic_store_n(&w->failed, errno ? errno : EIO, __ATOMIC_RELAXED);
        }
    }
    for (int i = 0; i < 2; ++i) {
        if (writer_seal(w, i)) { __atomic_store_n(&w->failed, EIO, __ATOMIC_RELAXED); }
    }
    return NULL;
}

netins_telemetry_writer_t *
netins_telemetry_open(const char *prefix)
{
    if (!prefix || strlen(prefix) > 1200) { return NULL; }
    netins_telemetry_writer_t *w = calloc(1, sizeof(*w));
    if (!w) { return NULL; }
    struct timeval tv; gettimeofday(&tv, NULL);
    static unsigned serial;
    snprintf(w->prefix, sizeof(w->prefix), "%s.%ld.%ld.%ld.%u", prefix,
        (long)getpid(), (long)tv.tv_sec, (long)tv.tv_usec,
        __atomic_add_fetch(&serial, 1, __ATOMIC_RELAXED));
    if (pthread_mutex_init(&w->lock, NULL)) { free(w); return NULL; }
    if (pthread_cond_init(&w->ready, NULL)) { pthread_mutex_destroy(&w->lock); free(w); return NULL; }
    if (pthread_create(&w->worker, NULL, writer_worker, w)) {
        pthread_cond_destroy(&w->ready); pthread_mutex_destroy(&w->lock); free(w); return NULL;
    }
    return w;
}

int
netins_telemetry_write(void *arg, int event, const char *record)
{
    netins_telemetry_writer_t *w = arg;
    if (!w || !record || strlen(record) >= RECORD_CAP) { return -1; }
    if (__atomic_load_n(&w->failed, __ATOMIC_RELAXED) || pthread_mutex_trylock(&w->lock)) {
        __atomic_add_fetch(&w->dropped, 1, __ATOMIC_RELAXED); return -1;
    }
    if (w->count == QUEUE_CAP || w->stop) {
        pthread_mutex_unlock(&w->lock);
        __atomic_add_fetch(&w->dropped, 1, __ATOMIC_RELAXED); return -1;
    }
    unsigned slot = (w->head + w->count) % QUEUE_CAP;
    strcpy(w->records[slot], record); w->events[slot] = event != 0;
    w->count++;
    pthread_cond_signal(&w->ready); pthread_mutex_unlock(&w->lock);
    return 0;
}

void
netins_telemetry_close(netins_telemetry_writer_t *w)
{
    if (!w) { return; }
    pthread_mutex_lock(&w->lock); w->stop = 1;
    pthread_cond_signal(&w->ready); pthread_mutex_unlock(&w->lock);
    pthread_join(w->worker, NULL);
    char path[1536], boot[80] = "unknown";
    FILE *f = fopen("/proc/sys/kernel/random/boot_id", "r");
    if (f) { if (!fgets(boot, sizeof(boot), f)) { strcpy(boot, "unknown"); } fclose(f); }
    boot[strcspn(boot, "\r\n")] = 0;
    snprintf(path, sizeof(path), "%s.health.json", w->prefix);
    f = fopen(path, "w");
    if (f) {
        fprintf(f, "{\"boot_id\":\"%s\",\"written_records\":%"PRIu64","
            "\"dropped_records\":%"PRIu64",\"writer_error\":%d}\n",
            boot, w->written, __atomic_load_n(&w->dropped, __ATOMIC_RELAXED),
            __atomic_load_n(&w->failed, __ATOMIC_RELAXED));
        if (fclose(f)) { fprintf(stderr, "observer_error: health file flush failed\n"); }
    } else { fprintf(stderr, "observer_error: cannot write %s\n", path); }
    if (w->failed || w->dropped) {
        fprintf(stderr, "observer_error: writer=%d dropped=%"PRIu64" prefix=%s\n",
            w->failed, w->dropped, w->prefix);
    }
    pthread_cond_destroy(&w->ready); pthread_mutex_destroy(&w->lock); free(w);
}
