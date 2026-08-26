#include "mini_stream_backend.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "common.h"
#include "mini_stream_protocol.h"
#include "mini_stream_reorder.h"

#define MINI_STREAM_MAGIC 0x4e535452U
#define MINI_STREAM_LANE_MAGIC 0x4e534c4eU
#define MINI_STREAM_ALLOWED_FLAGS 1U
/* One logical stream may aggregate up to eight independent Bytestreams. */
#define MINI_STREAM_ENABLED_MAX_LANES 8U

typedef struct mini_stream_send_chunk_s mini_stream_send_chunk_t;
typedef struct mini_stream_lane_s mini_stream_lane_t;

struct mini_stream_send_chunk_s {
    mini_stream_send_chunk_t *next;
    uint64_t offset;
    size_t payload_length;
    size_t wire_length;
    size_t wire_sent;
    unsigned char wire[];
};

struct mini_stream_lane_s {
    uint32_t magic;
    struct mini_backend_stream_s *stream;
    xqc_h3_ext_bytestream_t *bs;
    uint32_t lane_index;
    int allocated;
    int lane_header_parsed;
    unsigned char lane_header[MINI_STREAM_LANE_HEADER_SIZE];
    size_t lane_header_bytes;
    size_t lane_header_sent;

    mini_stream_send_chunk_t *send_head;
    mini_stream_send_chunk_t *send_tail;

    unsigned char frame_header[MINI_STREAM_FRAME_HEADER_SIZE];
    size_t frame_header_bytes;
    mini_stream_frame_header_t incoming_frame;
    mini_stream_reorder_chunk_t *incoming_chunk;

    unsigned char fin_frame[MINI_STREAM_FRAME_HEADER_SIZE];
    size_t fin_frame_sent;
    int protocol_fin_sent;
    int send_fin_submitted;

    int protocol_fin_received;
    int transport_fin_received;
    uint64_t final_offset;

    unsigned char receipt[MINI_STREAM_RECEIPT_SIZE];
    size_t receipt_bytes;
    size_t receipt_sent;
    int receipt_valid;
    int reverse_fin_submitted;
    int reverse_fin_acked;
    int graceful_closed;
};

struct mini_backend_stream_s {
    uint32_t magic;
    xqc_engine_t *engine;
    xqc_cid_t cid;
    struct event_base *event_base;
    int role;
    uint32_t flags;
    uint32_t lane_count;
    size_t send_queue_limit;
    size_t receive_queue_limit;
    mini_backend_stream_data_fn on_data;
    void *user_data;
    void **connection_state_slot;
    void *owner;
    mini_stream_owner_detach_fn detach;

    pthread_mutex_t lock;
    pthread_cond_t state_cond;
    pthread_cond_t send_space_cond;
    pthread_cond_t delivery_cond;
    pthread_t worker;
    int worker_started;
    int external_event_driver;
    int io_stopped;
    pthread_t delivery_worker;
    int delivery_worker_started;
    int delivery_stop_requested;
    int wake_pipe[2];
    struct event *wake_event;
    int wake_pending;

    mini_stream_lane_t lanes[MINI_STREAM_ENABLED_MAX_LANES];
    uint32_t allocated_lanes;
    uint32_t next_send_lane;
    uint64_t session_id;
    uint64_t next_send_offset;
    uint64_t accepted_bytes;
    uint64_t peer_received_bytes;
    uint64_t digest;
    uint64_t start_time_us;
    uint64_t completed_time_us;
    size_t send_queue_bytes;
    size_t send_queue_high_water;
    size_t reorder_high_water;
    uint64_t records_enqueued;
    uint64_t records_submitted;
    uint64_t records_received;
    uint64_t records_delivered;
    uint64_t lane_payload_sent[MINI_STREAM_ENABLED_MAX_LANES];
    uint64_t lane_payload_received[MINI_STREAM_ENABLED_MAX_LANES];
    mini_stream_reorder_t reorder;
    mini_stream_reorder_chunk_t *delivery_head;
    mini_stream_reorder_chunk_t *delivery_tail;
    size_t delivery_queue_bytes;

    int finish_requested;
    int reset_requested;
    int stop_requested;
    int connection_closed;
    int completed;
    int error_code;
    uint64_t app_error_code;
};

static void mini_stream_wake(mini_backend_stream_t *stream);
static int mini_stream_process_locked(mini_backend_stream_t *stream);
static void *mini_stream_delivery_worker(void *arg);

static uint64_t
mini_stream_now(void)
{
    return xqc_now();
}

static int
mini_stream_valid(const mini_backend_stream_t *stream)
{
    return stream != NULL && stream->magic == MINI_STREAM_MAGIC;
}

static void
mini_stream_signal_locked(mini_backend_stream_t *stream)
{
    pthread_cond_broadcast(&stream->state_cond);
    pthread_cond_broadcast(&stream->send_space_cond);
}

static int
mini_stream_fail_locked(mini_backend_stream_t *stream, int error_code)
{
    if (stream->error_code == 0) {
        stream->error_code = error_code != 0 ? error_code : EIO;
    }
    if (!stream->completed) {
        stream->completed = 1;
        stream->completed_time_us = mini_stream_now();
    }
    mini_stream_signal_locked(stream);
    return -1;
}

static void
mini_stream_close_lanes_locked(mini_backend_stream_t *stream, int abort)
{
    uint32_t i;
    for (i = 0; i < stream->lane_count; i++) {
        mini_stream_lane_t *lane = &stream->lanes[i];
        if (lane->bs != NULL) {
            /* close_notify may run after this stream object is released. */
            xqc_h3_ext_bytestream_set_user_data(lane->bs, NULL);
            if (abort) {
                (void)xqc_h3_ext_bytestream_close(lane->bs);
            }
            lane->bs = NULL;
        }
    }
}

static int
mini_stream_set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        return -1;
    }
    flags = fcntl(fd, F_GETFD, 0);
    if (flags < 0 || fcntl(fd, F_SETFD, flags | FD_CLOEXEC) != 0) {
        return -1;
    }
    return 0;
}

static void
mini_stream_wake(mini_backend_stream_t *stream)
{
    unsigned char byte = 1;
    if (stream == NULL || stream->wake_pipe[1] < 0) {
        return;
    }
    /* All callers hold stream->lock.  One unread byte is enough to wake the
     * worker, so coalesce bursts instead of writing once per queued record. */
    if (stream->wake_pending) {
        return;
    }
    stream->wake_pending = 1;
    ssize_t written = write(stream->wake_pipe[1], &byte, 1);
    if (written < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        stream->wake_pending = 0;
    }
}

static void
mini_stream_wake_callback(evutil_socket_t fd, short events, void *arg)
{
    mini_backend_stream_t *stream = arg;
    unsigned char buffer[128];
    (void)events;
    while (read((int)fd, buffer, sizeof(buffer)) > 0) {
    }
    if (!mini_stream_valid(stream)) {
        return;
    }
    pthread_mutex_lock(&stream->lock);
    stream->wake_pending = 0;
    (void)mini_stream_process_locked(stream);
    if (stream->stop_requested) {
        stream->io_stopped = 1;
        mini_stream_signal_locked(stream);
        if (!stream->external_event_driver && stream->event_base != NULL) {
            event_base_loopbreak(stream->event_base);
        }
    }
    pthread_mutex_unlock(&stream->lock);
}

static int
mini_stream_send_bytes(mini_stream_lane_t *lane, unsigned char *data,
    size_t data_len, size_t *sent_offset)
{
    if (lane->bs == NULL || *sent_offset >= data_len) {
        return 0;
    }
    ssize_t sent = xqc_h3_ext_bytestream_send(lane->bs,
        data + *sent_offset, data_len - *sent_offset, 0,
        XQC_DATA_QOS_HIGHEST);
    if (sent == -XQC_EAGAIN) {
        return 0;
    }
    if (sent <= 0 || (size_t)sent > data_len - *sent_offset) {
        return -1;
    }
    *sent_offset += (size_t)sent;
    return 1;
}

static int
mini_stream_finish_lane(mini_stream_lane_t *lane)
{
    if (lane->bs == NULL || lane->send_fin_submitted) {
        return 0;
    }
    ssize_t sent = xqc_h3_ext_bytestream_finish(lane->bs);
    if (sent == -XQC_EAGAIN) {
        return 0;
    }
    if (sent < 0) {
        return -1;
    }
    lane->send_fin_submitted = 1;
    return 1;
}

static int
mini_stream_pump_open_lane_locked(mini_backend_stream_t *stream,
    mini_stream_lane_t *lane)
{
    int ret;
    if (lane->bs == NULL) {
        return 0;
    }
    if (lane->lane_header_sent < MINI_STREAM_LANE_HEADER_SIZE) {
        ret = mini_stream_send_bytes(lane, lane->lane_header,
            MINI_STREAM_LANE_HEADER_SIZE, &lane->lane_header_sent);
        if (ret <= 0 || lane->lane_header_sent < MINI_STREAM_LANE_HEADER_SIZE) {
            return ret;
        }
    }

    while (lane->send_head != NULL) {
        mini_stream_send_chunk_t *chunk = lane->send_head;
        ret = mini_stream_send_bytes(lane, chunk->wire, chunk->wire_length,
            &chunk->wire_sent);
        if (ret <= 0 || chunk->wire_sent < chunk->wire_length) {
            return ret;
        }
        lane->send_head = chunk->next;
        if (lane->send_head == NULL) {
            lane->send_tail = NULL;
        }
        stream->records_submitted++;
        stream->lane_payload_sent[lane->lane_index] +=
            chunk->payload_length;
        stream->send_queue_bytes -= chunk->payload_length;
        free(chunk);
        pthread_cond_broadcast(&stream->send_space_cond);
        if (stream->send_queue_bytes == 0) {
            pthread_cond_broadcast(&stream->state_cond);
        }
    }

    if (!stream->finish_requested) {
        return 0;
    }
    if (!lane->protocol_fin_sent) {
        ret = mini_stream_send_bytes(lane, lane->fin_frame,
            MINI_STREAM_FRAME_HEADER_SIZE, &lane->fin_frame_sent);
        if (ret <= 0 || lane->fin_frame_sent < MINI_STREAM_FRAME_HEADER_SIZE) {
            return ret;
        }
        lane->protocol_fin_sent = 1;
    }
    return mini_stream_finish_lane(lane);
}

static mini_stream_lane_t *
mini_stream_logical_lane(mini_backend_stream_t *stream, uint32_t lane_index)
{
    uint32_t i;
    for (i = 0; i < stream->lane_count; i++) {
        if (stream->lanes[i].allocated
            && stream->lanes[i].lane_index == lane_index)
        {
            return &stream->lanes[i];
        }
    }
    return NULL;
}

static int
mini_stream_all_forward_finished_locked(mini_backend_stream_t *stream,
    uint64_t *total_bytes)
{
    uint32_t i;
    uint64_t total = 0;
    if (stream->allocated_lanes != stream->lane_count) {
        return 0;
    }
    for (i = 0; i < stream->lane_count; i++) {
        mini_stream_lane_t *lane = &stream->lanes[i];
        if (!lane->lane_header_parsed || !lane->protocol_fin_received
            || !lane->transport_fin_received)
        {
            return 0;
        }
        if (i == 0) {
            total = lane->final_offset;
        } else if (lane->final_offset != total) {
            return mini_stream_fail_locked(stream, EPROTO);
        }
    }
    *total_bytes = total;
    return 1;
}

static void
mini_stream_build_receipt_locked(mini_backend_stream_t *stream,
    uint32_t status, uint64_t total_bytes, uint64_t detail)
{
    mini_stream_lane_t *lane0 = mini_stream_logical_lane(stream, 0);
    mini_stream_receipt_t receipt = {
        .status = status,
        .session_id = stream->session_id,
        .total_bytes = total_bytes,
        .digest = stream->digest,
        .detail = detail,
    };
    if (lane0 != NULL && lane0->receipt_bytes == 0) {
        mini_stream_receipt_encode(&receipt, lane0->receipt);
        lane0->receipt_bytes = MINI_STREAM_RECEIPT_SIZE;
    }
}

static int
mini_stream_pump_accept_receipt_locked(mini_backend_stream_t *stream)
{
    uint64_t total_bytes = 0;
    int ready = mini_stream_all_forward_finished_locked(stream, &total_bytes);
    if (ready <= 0) {
        return ready;
    }
    if (mini_stream_reorder_has_pending(&stream->reorder)
        || stream->reorder.next_offset != total_bytes)
    {
        return mini_stream_fail_locked(stream, EPROTO);
    }
    /* A receipt describes bytes already handed to the application, not just
     * bytes parsed from QUIC.  The delivery worker wakes the I/O driver after
     * the final callback returns. */
    if (stream->delivery_head != NULL || stream->delivery_queue_bytes != 0
        || stream->accepted_bytes != total_bytes)
    {
        return 0;
    }

    mini_stream_build_receipt_locked(stream, MINI_STREAM_RECEIPT_OK,
        total_bytes, UINT64_MAX);
    uint32_t i;
    for (i = 0; i < stream->lane_count; i++) {
        mini_stream_lane_t *lane = &stream->lanes[i];
        if (lane->lane_index == 0
            && lane->receipt_sent < lane->receipt_bytes)
        {
            int ret = mini_stream_send_bytes(lane, lane->receipt,
                lane->receipt_bytes, &lane->receipt_sent);
            if (ret < 0) {
                return mini_stream_fail_locked(stream, EIO);
            }
            if (lane->receipt_sent < lane->receipt_bytes) {
                continue;
            }
        }
        if (!lane->reverse_fin_submitted) {
            int ret = mini_stream_finish_lane(lane);
            if (ret < 0) {
                return mini_stream_fail_locked(stream, EIO);
            }
            if (lane->send_fin_submitted) {
                lane->reverse_fin_submitted = 1;
            }
        }
    }

    for (i = 0; i < stream->lane_count; i++) {
        if (!stream->lanes[i].reverse_fin_submitted) {
            return 0;
        }
    }

    /* finish() must not return while close() could still discard the receipt.
     * Wait until QUIC acknowledges every reverse FIN; the lane-0 FIN covers
     * the receipt bytes that precede it. */
    for (i = 0; i < stream->lane_count; i++) {
        mini_stream_lane_t *lane = &stream->lanes[i];
        if (!lane->reverse_fin_acked && lane->bs != NULL) {
            xqc_h3_ext_bytestream_stats_t stats =
                xqc_h3_ext_bytestream_get_stats(lane->bs);
            lane->reverse_fin_acked = stats.fin_acked_time != 0;
        }
        if (!lane->reverse_fin_acked) {
            return 0;
        }
    }
    stream->peer_received_bytes = total_bytes;
    stream->accepted_bytes = total_bytes;
    stream->completed = 1;
    stream->completed_time_us = mini_stream_now();
    mini_stream_signal_locked(stream);
    return 1;
}

static int
mini_stream_process_locked(mini_backend_stream_t *stream)
{
    if (stream->reset_requested) {
        mini_stream_close_lanes_locked(stream, 1);
        return mini_stream_fail_locked(stream, ECANCELED);
    }
    if (stream->stop_requested) {
        mini_stream_close_lanes_locked(stream,
            !stream->completed || stream->error_code != 0);
        return 0;
    }
    if (stream->error_code != 0 || stream->connection_closed) {
        return -1;
    }

    if (stream->role == MINI_STREAM_ENDPOINT_OPEN) {
        uint32_t i;
        for (i = 0; i < stream->lane_count; i++) {
            if (mini_stream_pump_open_lane_locked(stream,
                    &stream->lanes[i]) < 0)
            {
                return mini_stream_fail_locked(stream, EIO);
            }
        }
    } else {
        return mini_stream_pump_accept_receipt_locked(stream);
    }
    return 0;
}

static void *
mini_stream_worker(void *arg)
{
    mini_backend_stream_t *stream = arg;
    pthread_mutex_lock(&stream->lock);
    if (stream->role == MINI_STREAM_ENDPOINT_OPEN) {
        uint32_t i;
        for (i = 0; i < stream->lane_count; i++) {
            mini_stream_lane_t *lane = &stream->lanes[i];
            lane->bs = xqc_h3_ext_bytestream_create(stream->engine,
                &stream->cid, lane);
            if (lane->bs == NULL) {
                (void)mini_stream_fail_locked(stream, EIO);
                break;
            }
        }
        (void)mini_stream_process_locked(stream);
    }
    pthread_mutex_unlock(&stream->lock);

    for (;;) {
        pthread_mutex_lock(&stream->lock);
        int stop = stream->stop_requested;
        pthread_mutex_unlock(&stream->lock);
        if (stop) {
            break;
        }
        int ret = event_base_loop(stream->event_base, EVLOOP_ONCE);
        if (ret < 0) {
            pthread_mutex_lock(&stream->lock);
            (void)mini_stream_fail_locked(stream, EIO);
            pthread_mutex_unlock(&stream->lock);
            break;
        }
        pthread_mutex_lock(&stream->lock);
        (void)mini_stream_process_locked(stream);
        stop = stream->stop_requested;
        pthread_mutex_unlock(&stream->lock);
        if (stop) {
            break;
        }
    }
    pthread_mutex_lock(&stream->lock);
    (void)mini_stream_process_locked(stream);
    stream->io_stopped = 1;
    mini_stream_signal_locked(stream);
    pthread_mutex_unlock(&stream->lock);
    return NULL;
}

int
mini_stream_backend_drive(mini_backend_stream_t *stream)
{
    if (!mini_stream_valid(stream)) {
        return EINVAL;
    }
    pthread_mutex_lock(&stream->lock);
    int ret = mini_stream_process_locked(stream);
    if (stream->stop_requested) {
        stream->io_stopped = 1;
        mini_stream_signal_locked(stream);
    }
    pthread_mutex_unlock(&stream->lock);
    return ret < 0 ? EIO : 0;
}

static int
mini_stream_wait_locked(mini_backend_stream_t *stream,
    pthread_cond_t *condition, uint32_t timeout_ms, uint64_t start_us)
{
    if (timeout_ms == 0) {
        int ret = pthread_cond_wait(condition, &stream->lock);
        return ret == 0 ? 0 : ret;
    }
    uint64_t timeout_us = (uint64_t)timeout_ms * 1000;
    uint64_t now = mini_stream_now();
    if (now - start_us >= timeout_us) {
        return ETIMEDOUT;
    }
    uint64_t remaining_us = timeout_us - (now - start_us);
    struct timespec deadline;
    if (clock_gettime(CLOCK_REALTIME, &deadline) != 0) {
        return errno != 0 ? errno : EIO;
    }
    deadline.tv_sec += (time_t)(remaining_us / 1000000);
    deadline.tv_nsec += (long)((remaining_us % 1000000) * 1000);
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }
    int ret = pthread_cond_timedwait(condition, &stream->lock, &deadline);
    return ret == 0 ? 0 : ret;
}

static mini_stream_send_chunk_t *
mini_stream_make_send_chunk(uint64_t offset, const unsigned char *data,
    size_t data_len)
{
    size_t wire_length = MINI_STREAM_FRAME_HEADER_SIZE + data_len;
    if (wire_length < data_len) {
        return NULL;
    }
    mini_stream_send_chunk_t *chunk =
        malloc(sizeof(*chunk) + wire_length);
    if (chunk == NULL) {
        return NULL;
    }
    memset(chunk, 0, sizeof(*chunk));
    chunk->offset = offset;
    chunk->payload_length = data_len;
    chunk->wire_length = wire_length;
    mini_stream_frame_header_t header = {
        .type = MINI_STREAM_FRAME_DATA,
        .offset = offset,
        .payload_length = (uint32_t)data_len,
        .flags = 0,
    };
    mini_stream_frame_header_encode(&header, chunk->wire);
    memcpy(chunk->wire + MINI_STREAM_FRAME_HEADER_SIZE, data, data_len);
    return chunk;
}

int
mini_stream_backend_create(xqc_engine_t *engine, const xqc_cid_t *cid,
    struct event_base *event_base, int role,
    const mini_backend_stream_config_t *config,
    void **connection_state_slot, void *owner,
    mini_stream_owner_detach_fn detach,
    mini_backend_stream_t **out_stream)
{
    if (engine == NULL || cid == NULL || event_base == NULL || config == NULL
        || connection_state_slot == NULL || *connection_state_slot != NULL
        || out_stream == NULL
        || (role != MINI_STREAM_ENDPOINT_OPEN
            && role != MINI_STREAM_ENDPOINT_ACCEPT)
        || config->lane_count == 0
        || config->lane_count > MINI_STREAM_ENABLED_MAX_LANES
        || (config->flags & ~MINI_STREAM_ALLOWED_FLAGS) != 0
        || config->send_queue_size < MINI_STREAM_MAX_FRAME_PAYLOAD
        || config->receive_queue_size < MINI_STREAM_MAX_FRAME_PAYLOAD
        || (role == MINI_STREAM_ENDPOINT_ACCEPT && config->on_data == NULL))
    {
        return EINVAL;
    }
    *out_stream = NULL;
    mini_backend_stream_t *stream = calloc(1, sizeof(*stream));
    if (stream == NULL) {
        return ENOMEM;
    }
    stream->wake_pipe[0] = -1;
    stream->wake_pipe[1] = -1;
    stream->magic = MINI_STREAM_MAGIC;
    stream->engine = engine;
    memcpy(&stream->cid, cid, sizeof(*cid));
    stream->event_base = event_base;
    stream->role = role;
    stream->flags = config->flags;
    stream->lane_count = config->lane_count;
    stream->send_queue_limit = config->send_queue_size;
    stream->receive_queue_limit = config->receive_queue_size;
    stream->external_event_driver = config->external_event_driver != 0;
    stream->on_data = config->on_data;
    stream->user_data = config->user_data;
    stream->connection_state_slot = connection_state_slot;
    stream->owner = owner;
    stream->detach = detach;
    stream->digest = mini_stream_digest_init();
    mini_stream_reorder_init(&stream->reorder,
        stream->receive_queue_limit);
    if (role == MINI_STREAM_ENDPOINT_OPEN) {
        stream->session_id = mini_stream_now()
            ^ (uint64_t)(uintptr_t)stream ^ ((uint64_t)getpid() << 32);
    }

    int ret = pthread_mutex_init(&stream->lock, NULL);
    if (ret != 0) {
        free(stream);
        return ret;
    }
    ret = pthread_cond_init(&stream->state_cond, NULL);
    if (ret != 0) {
        pthread_mutex_destroy(&stream->lock);
        free(stream);
        return ret;
    }
    ret = pthread_cond_init(&stream->send_space_cond, NULL);
    if (ret != 0) {
        pthread_cond_destroy(&stream->state_cond);
        pthread_mutex_destroy(&stream->lock);
        free(stream);
        return ret;
    }
    ret = pthread_cond_init(&stream->delivery_cond, NULL);
    if (ret != 0) {
        pthread_cond_destroy(&stream->send_space_cond);
        pthread_cond_destroy(&stream->state_cond);
        pthread_mutex_destroy(&stream->lock);
        free(stream);
        return ret;
    }
    if (pipe(stream->wake_pipe) != 0
        || mini_stream_set_nonblocking(stream->wake_pipe[0]) != 0
        || mini_stream_set_nonblocking(stream->wake_pipe[1]) != 0)
    {
        ret = errno != 0 ? errno : EIO;
        goto fail;
    }
    stream->wake_event = event_new(event_base, stream->wake_pipe[0],
        EV_READ | EV_PERSIST, mini_stream_wake_callback, stream);
    if (stream->wake_event == NULL || event_add(stream->wake_event, NULL) != 0) {
        ret = ENOMEM;
        goto fail;
    }

    uint32_t i;
    for (i = 0; i < stream->lane_count; i++) {
        mini_stream_lane_t *lane = &stream->lanes[i];
        lane->magic = MINI_STREAM_LANE_MAGIC;
        lane->stream = stream;
        lane->lane_index = role == MINI_STREAM_ENDPOINT_OPEN ? i : UINT32_MAX;
        if (role == MINI_STREAM_ENDPOINT_OPEN) {
            mini_stream_lane_header_t lane_header = {
                .session_id = stream->session_id,
                .lane_index = i,
                .lane_count = stream->lane_count,
                .max_frame_payload = MINI_STREAM_MAX_FRAME_PAYLOAD,
            };
            mini_stream_lane_header_encode(&lane_header, lane->lane_header);
            mini_stream_frame_header_t fin_header = {
                .type = MINI_STREAM_FRAME_FIN,
                .offset = 0,
                .payload_length = 0,
                .flags = 0,
            };
            mini_stream_frame_header_encode(&fin_header, lane->fin_frame);
            lane->allocated = 1;
            lane->lane_header_parsed = 1;
            stream->allocated_lanes++;
        }
    }

    if (role == MINI_STREAM_ENDPOINT_ACCEPT) {
        ret = pthread_create(&stream->delivery_worker, NULL,
            mini_stream_delivery_worker, stream);
        if (ret != 0) {
            goto fail;
        }
        stream->delivery_worker_started = 1;
    }

    *connection_state_slot = stream;
    if (!stream->external_event_driver) {
        ret = pthread_create(&stream->worker, NULL,
            mini_stream_worker, stream);
        if (ret != 0) {
            *connection_state_slot = NULL;
            goto fail;
        }
        stream->worker_started = 1;
    }
    *out_stream = stream;
    return 0;

fail:
    if (stream->delivery_worker_started) {
        pthread_mutex_lock(&stream->lock);
        stream->delivery_stop_requested = 1;
        pthread_cond_broadcast(&stream->delivery_cond);
        pthread_mutex_unlock(&stream->lock);
        (void)pthread_join(stream->delivery_worker, NULL);
    }
    if (stream->wake_event != NULL) {
        event_free(stream->wake_event);
    }
    if (stream->wake_pipe[0] >= 0) {
        close(stream->wake_pipe[0]);
    }
    if (stream->wake_pipe[1] >= 0) {
        close(stream->wake_pipe[1]);
    }
    pthread_cond_destroy(&stream->delivery_cond);
    pthread_cond_destroy(&stream->send_space_cond);
    pthread_cond_destroy(&stream->state_cond);
    pthread_mutex_destroy(&stream->lock);
    stream->magic = 0;
    free(stream);
    return ret;
}

int
mini_backend_stream_write(mini_backend_stream_t *stream, const void *data,
    size_t data_len, uint32_t timeout_ms, size_t *accepted)
{
    if (!mini_stream_valid(stream) || accepted == NULL
        || (data == NULL && data_len != 0)
        || stream->role != MINI_STREAM_ENDPOINT_OPEN)
    {
        return EINVAL;
    }
    *accepted = 0;
    if (data_len == 0) {
        return 0;
    }
    const unsigned char *bytes = data;
    uint64_t start = mini_stream_now();
    int result = 0;
    pthread_mutex_lock(&stream->lock);
    while (*accepted < data_len) {
        if (stream->error_code != 0 || stream->connection_closed) {
            result = stream->error_code != 0
                ? stream->error_code : ECONNRESET;
            break;
        }
        if (stream->reset_requested) {
            result = ECANCELED;
            break;
        }
        if (stream->finish_requested) {
            result = EPIPE;
            break;
        }
        size_t available = stream->send_queue_limit
            - stream->send_queue_bytes;
        if (available == 0) {
            if ((stream->flags & 1U) != 0) {
                result = EAGAIN;
                break;
            }
            result = mini_stream_wait_locked(stream,
                &stream->send_space_cond, timeout_ms, start);
            if (result != 0) {
                break;
            }
            continue;
        }
        size_t count = data_len - *accepted;
        if (count > available) {
            count = available;
        }
        if (count > MINI_STREAM_MAX_FRAME_PAYLOAD) {
            count = MINI_STREAM_MAX_FRAME_PAYLOAD;
        }
        mini_stream_send_chunk_t *chunk = mini_stream_make_send_chunk(
            stream->next_send_offset, bytes + *accepted, count);
        if (chunk == NULL) {
            result = ENOMEM;
            break;
        }
        mini_stream_lane_t *lane =
            &stream->lanes[stream->next_send_lane];
        stream->next_send_lane =
            (stream->next_send_lane + 1) % stream->lane_count;
        if (lane->send_tail == NULL) {
            lane->send_head = chunk;
        } else {
            lane->send_tail->next = chunk;
        }
        lane->send_tail = chunk;
        stream->send_queue_bytes += count;
        if (stream->send_queue_bytes > stream->send_queue_high_water) {
            stream->send_queue_high_water = stream->send_queue_bytes;
        }
        stream->records_enqueued++;
        stream->digest = mini_stream_digest_update(stream->digest,
            bytes + *accepted, count);
        stream->next_send_offset += count;
        stream->accepted_bytes += count;
        *accepted += count;
        if (stream->start_time_us == 0) {
            stream->start_time_us = mini_stream_now();
        }
        mini_stream_wake(stream);
    }
    pthread_mutex_unlock(&stream->lock);
    return result;
}

int
mini_backend_stream_flush(mini_backend_stream_t *stream,
    uint32_t timeout_ms)
{
    if (!mini_stream_valid(stream)
        || stream->role != MINI_STREAM_ENDPOINT_OPEN)
    {
        return EINVAL;
    }
    uint64_t start = mini_stream_now();
    int result = 0;
    pthread_mutex_lock(&stream->lock);
    mini_stream_wake(stream);
    while (stream->send_queue_bytes != 0 && stream->error_code == 0
        && !stream->reset_requested && !stream->connection_closed)
    {
        result = mini_stream_wait_locked(stream, &stream->state_cond,
            timeout_ms, start);
        if (result != 0) {
            break;
        }
    }
    if (result == 0 && stream->error_code != 0) {
        result = stream->error_code;
    } else if (result == 0 && stream->reset_requested) {
        result = ECANCELED;
    } else if (result == 0 && stream->connection_closed) {
        result = ECONNRESET;
    }
    pthread_mutex_unlock(&stream->lock);
    return result;
}

int
mini_backend_stream_finish(mini_backend_stream_t *stream,
    uint32_t timeout_ms, mini_backend_stream_result_t *result)
{
    if (!mini_stream_valid(stream) || result == NULL) {
        return EINVAL;
    }
    memset(result, 0, sizeof(*result));
    uint64_t start = mini_stream_now();
    int wait_result = 0;
    pthread_mutex_lock(&stream->lock);
    if (stream->start_time_us == 0) {
        stream->start_time_us = start;
    }
    if (stream->reset_requested) {
        wait_result = ECANCELED;
    } else if (stream->role == MINI_STREAM_ENDPOINT_OPEN
        && !stream->finish_requested)
    {
        uint32_t i;
        stream->finish_requested = 1;
        for (i = 0; i < stream->lane_count; i++) {
            mini_stream_frame_header_t header = {
                .type = MINI_STREAM_FRAME_FIN,
                .offset = stream->accepted_bytes,
                .payload_length = 0,
                .flags = 0,
            };
            mini_stream_frame_header_encode(&header,
                stream->lanes[i].fin_frame);
        }
        mini_stream_wake(stream);
    }
    while (wait_result == 0 && !stream->completed
        && stream->error_code == 0)
    {
        wait_result = mini_stream_wait_locked(stream, &stream->state_cond,
            timeout_ms, start);
        if (wait_result != 0) {
            break;
        }
    }
    result->accepted_bytes = stream->accepted_bytes;
    result->peer_received_bytes = stream->peer_received_bytes;
    if (stream->completed_time_us > stream->start_time_us) {
        result->elapsed_us =
            stream->completed_time_us - stream->start_time_us;
    }
    if (wait_result == 0 && stream->error_code != 0) {
        wait_result = stream->error_code;
    }
    pthread_mutex_unlock(&stream->lock);
    return wait_result;
}

int
mini_backend_stream_reset(mini_backend_stream_t *stream,
    uint64_t error_code)
{
    if (!mini_stream_valid(stream)) {
        return EINVAL;
    }
    pthread_mutex_lock(&stream->lock);
    if (!stream->completed) {
        stream->app_error_code = error_code;
        stream->reset_requested = 1;
        mini_stream_wake(stream);
    }
    pthread_mutex_unlock(&stream->lock);
    return 0;
}

int
mini_backend_stream_get_snapshot(mini_backend_stream_t *stream,
    mini_backend_stream_snapshot_t *snapshot)
{
    if (!mini_stream_valid(stream) || snapshot == NULL) {
        return EINVAL;
    }
    pthread_mutex_lock(&stream->lock);
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->snapshot_time_us = mini_stream_now();
    snapshot->endpoint = stream->role;
    if (stream->reset_requested) {
        snapshot->state = MINI_BACKEND_STREAM_STATE_RESET;
    } else if (stream->error_code != 0) {
        snapshot->state = MINI_BACKEND_STREAM_STATE_ERROR;
    } else if (stream->completed) {
        snapshot->state = MINI_BACKEND_STREAM_STATE_COMPLETED;
    } else if (stream->connection_closed) {
        snapshot->state = MINI_BACKEND_STREAM_STATE_ERROR;
    } else if (stream->finish_requested) {
        snapshot->state = MINI_BACKEND_STREAM_STATE_FINISHING;
    } else {
        snapshot->state = MINI_BACKEND_STREAM_STATE_ACTIVE;
    }
    snapshot->lane_count = stream->lane_count;
    snapshot->flags = stream->flags;
    snapshot->accepted_bytes = stream->accepted_bytes;
    snapshot->peer_received_bytes = stream->peer_received_bytes;
    snapshot->send_queue_bytes = stream->send_queue_bytes;
    snapshot->send_queue_capacity = stream->send_queue_limit;
    snapshot->send_queue_high_water = stream->send_queue_high_water;
    snapshot->reorder_bytes = stream->reorder.buffered_bytes
        + stream->delivery_queue_bytes;
    snapshot->reorder_capacity = stream->receive_queue_limit;
    snapshot->reorder_high_water = stream->reorder_high_water;
    snapshot->records_enqueued = stream->records_enqueued;
    snapshot->records_submitted = stream->records_submitted;
    snapshot->records_received = stream->records_received;
    snapshot->records_delivered = stream->records_delivered;
    memcpy(snapshot->lane_payload_sent, stream->lane_payload_sent,
        sizeof(snapshot->lane_payload_sent));
    memcpy(snapshot->lane_payload_received, stream->lane_payload_received,
        sizeof(snapshot->lane_payload_received));
    snapshot->last_error = stream->error_code != 0
        ? stream->error_code
        : (stream->connection_closed && !stream->completed
            ? ECONNRESET : 0);
    snapshot->app_error_code = stream->app_error_code;
    pthread_mutex_unlock(&stream->lock);
    return 0;
}

static void
mini_stream_free_queues(mini_backend_stream_t *stream)
{
    uint32_t i;
    for (i = 0; i < stream->lane_count; i++) {
        mini_stream_lane_t *lane = &stream->lanes[i];
        mini_stream_send_chunk_t *send = lane->send_head;
        while (send != NULL) {
            mini_stream_send_chunk_t *next = send->next;
            free(send);
            send = next;
        }
        mini_stream_reorder_discard(&stream->reorder,
            lane->incoming_chunk);
        lane->incoming_chunk = NULL;
    }
    mini_stream_reorder_release(&stream->reorder);
    mini_stream_reorder_chunk_t *delivery = stream->delivery_head;
    while (delivery != NULL) {
        mini_stream_reorder_chunk_t *next = delivery->next;
        free(delivery);
        delivery = next;
    }
    stream->delivery_head = NULL;
    stream->delivery_tail = NULL;
    stream->delivery_queue_bytes = 0;
}

void
mini_backend_stream_destroy(mini_backend_stream_t *stream)
{
    if (!mini_stream_valid(stream)) {
        return;
    }
    pthread_mutex_lock(&stream->lock);
    stream->stop_requested = 1;
    stream->delivery_stop_requested = 1;
    pthread_cond_broadcast(&stream->delivery_cond);
    mini_stream_wake(stream);
    while (stream->external_event_driver && !stream->io_stopped) {
        (void)pthread_cond_wait(&stream->state_cond, &stream->lock);
    }
    pthread_mutex_unlock(&stream->lock);
    if (stream->worker_started
        && !pthread_equal(pthread_self(), stream->worker))
    {
        (void)pthread_join(stream->worker, NULL);
    }
    if (stream->delivery_worker_started
        && !pthread_equal(pthread_self(), stream->delivery_worker))
    {
        (void)pthread_join(stream->delivery_worker, NULL);
    }
    if (stream->wake_event != NULL) {
        event_free(stream->wake_event);
        stream->wake_event = NULL;
    }
    if (stream->connection_state_slot != NULL
        && *stream->connection_state_slot == stream)
    {
        *stream->connection_state_slot = NULL;
    }
    if (stream->detach != NULL) {
        stream->detach(stream->owner, stream);
    }
    mini_stream_free_queues(stream);
    if (stream->wake_pipe[0] >= 0) {
        close(stream->wake_pipe[0]);
    }
    if (stream->wake_pipe[1] >= 0) {
        close(stream->wake_pipe[1]);
    }
    pthread_cond_destroy(&stream->delivery_cond);
    pthread_cond_destroy(&stream->send_space_cond);
    pthread_cond_destroy(&stream->state_cond);
    pthread_mutex_destroy(&stream->lock);
    stream->magic = 0;
    free(stream);
}

void
mini_stream_backend_on_connection_close(mini_backend_stream_t *stream,
    int error_code)
{
    if (!mini_stream_valid(stream)) {
        return;
    }
    pthread_mutex_lock(&stream->lock);
    stream->connection_closed = 1;
    if (!stream->completed) {
        (void)mini_stream_fail_locked(stream,
            error_code != 0 ? error_code : ECONNRESET);
    }
    pthread_mutex_unlock(&stream->lock);
}

static void *
mini_stream_delivery_worker(void *arg)
{
    mini_backend_stream_t *stream = arg;
    pthread_mutex_lock(&stream->lock);
    for (;;) {
        while (stream->delivery_head == NULL
            && !stream->delivery_stop_requested)
        {
            (void)pthread_cond_wait(&stream->delivery_cond, &stream->lock);
        }
        if (stream->delivery_stop_requested) {
            break;
        }
        mini_stream_reorder_chunk_t *chunk = stream->delivery_head;
        stream->delivery_head = chunk->next;
        if (stream->delivery_head == NULL) {
            stream->delivery_tail = NULL;
        }
        chunk->next = NULL;
        uint64_t digest = stream->digest;
        mini_backend_stream_data_fn callback = stream->on_data;
        void *callback_data = stream->user_data;
        pthread_mutex_unlock(&stream->lock);

        digest = mini_stream_digest_update(digest,
            chunk->data, chunk->length);
        if (callback != NULL) {
            callback(callback_data, chunk->data, chunk->length,
                chunk->offset);
        }

        pthread_mutex_lock(&stream->lock);
        stream->digest = digest;
        stream->accepted_bytes = chunk->offset + chunk->length;
        stream->records_delivered++;
        stream->delivery_queue_bytes -= chunk->length;
        free(chunk);
        mini_stream_signal_locked(stream);
        mini_stream_wake(stream);
    }
    pthread_mutex_unlock(&stream->lock);
    return NULL;
}

static int
mini_stream_insert_receive_locked(mini_backend_stream_t *stream,
    mini_stream_reorder_chunk_t *chunk)
{
    int ret = mini_stream_reorder_insert(&stream->reorder, chunk);
    if (ret != 0) {
        mini_stream_reorder_discard(&stream->reorder, chunk);
        return mini_stream_fail_locked(stream, ret);
    }
    mini_stream_reorder_chunk_t *ready;
    while ((ready = mini_stream_reorder_pop_ready(&stream->reorder)) != NULL) {
        if (stream->start_time_us == 0) {
            stream->start_time_us = mini_stream_now();
        }
        ready->next = NULL;
        if (stream->delivery_tail == NULL) {
            stream->delivery_head = ready;
        } else {
            stream->delivery_tail->next = ready;
        }
        stream->delivery_tail = ready;
        stream->delivery_queue_bytes += ready->length;
    }
    size_t buffered = stream->reorder.buffered_bytes
        + stream->delivery_queue_bytes;
    if (buffered > stream->reorder_high_water) {
        stream->reorder_high_water = buffered;
    }
    pthread_cond_signal(&stream->delivery_cond);
    return 0;
}

static int
mini_stream_parse_lane_header_locked(mini_backend_stream_t *stream,
    mini_stream_lane_t *lane)
{
    mini_stream_lane_header_t header;
    if (mini_stream_lane_header_decode(lane->lane_header, &header) != 0
        || header.lane_count != stream->lane_count
        || header.lane_index >= stream->lane_count
        || header.max_frame_payload == 0
        || header.max_frame_payload > MINI_STREAM_MAX_FRAME_PAYLOAD)
    {
        return mini_stream_fail_locked(stream, EPROTO);
    }
    uint32_t i;
    for (i = 0; i < stream->lane_count; i++) {
        if (&stream->lanes[i] != lane
            && stream->lanes[i].lane_header_parsed
            && stream->lanes[i].lane_index == header.lane_index)
        {
            return mini_stream_fail_locked(stream, EPROTO);
        }
    }
    if (stream->session_id == 0) {
        stream->session_id = header.session_id;
    } else if (stream->session_id != header.session_id) {
        return mini_stream_fail_locked(stream, EPROTO);
    }
    lane->lane_index = header.lane_index;
    lane->lane_header_parsed = 1;
    return 0;
}

static int
mini_stream_begin_frame_locked(mini_backend_stream_t *stream,
    mini_stream_lane_t *lane)
{
    if (mini_stream_frame_header_decode(lane->frame_header,
            &lane->incoming_frame) != 0
        || lane->incoming_frame.flags != 0
        || lane->protocol_fin_received)
    {
        return mini_stream_fail_locked(stream, EPROTO);
    }
    if (lane->incoming_frame.type == MINI_STREAM_FRAME_FIN) {
        if (lane->incoming_frame.payload_length != 0) {
            return mini_stream_fail_locked(stream, EPROTO);
        }
        lane->protocol_fin_received = 1;
        lane->final_offset = lane->incoming_frame.offset;
        lane->frame_header_bytes = 0;
        return 0;
    }
    if (lane->incoming_frame.type != MINI_STREAM_FRAME_DATA
        || lane->incoming_frame.payload_length == 0
        || lane->incoming_frame.payload_length > MINI_STREAM_MAX_FRAME_PAYLOAD)
    {
        return mini_stream_fail_locked(stream, EPROTO);
    }
    size_t payload_length = lane->incoming_frame.payload_length;
    size_t buffered = stream->reorder.buffered_bytes
        + stream->delivery_queue_bytes;
    if (buffered > stream->receive_queue_limit
        || payload_length > stream->receive_queue_limit - buffered)
    {
        return mini_stream_fail_locked(stream, ENOBUFS);
    }
    int error_code = 0;
    mini_stream_reorder_chunk_t *chunk = mini_stream_reorder_allocate(
        &stream->reorder, lane->incoming_frame.offset,
        payload_length, &error_code);
    if (chunk == NULL) {
        return mini_stream_fail_locked(stream,
            error_code != 0 ? error_code : ENOMEM);
    }
    buffered = stream->reorder.buffered_bytes
        + stream->delivery_queue_bytes;
    if (buffered > stream->reorder_high_water) {
        stream->reorder_high_water = buffered;
    }
    lane->incoming_chunk = chunk;
    return 0;
}

static int
mini_stream_receive_forward_locked(mini_backend_stream_t *stream,
    mini_stream_lane_t *lane, const unsigned char *data, size_t data_len,
    uint8_t fin)
{
    size_t offset = 0;
    while (offset < data_len) {
        if (!lane->lane_header_parsed) {
            size_t count = MINI_STREAM_LANE_HEADER_SIZE
                - lane->lane_header_bytes;
            if (count > data_len - offset) {
                count = data_len - offset;
            }
            memcpy(lane->lane_header + lane->lane_header_bytes,
                data + offset, count);
            lane->lane_header_bytes += count;
            offset += count;
            if (lane->lane_header_bytes == MINI_STREAM_LANE_HEADER_SIZE
                && mini_stream_parse_lane_header_locked(stream, lane) != 0)
            {
                return -1;
            }
            continue;
        }
        if (lane->incoming_chunk == NULL) {
            size_t count = MINI_STREAM_FRAME_HEADER_SIZE
                - lane->frame_header_bytes;
            if (count > data_len - offset) {
                count = data_len - offset;
            }
            memcpy(lane->frame_header + lane->frame_header_bytes,
                data + offset, count);
            lane->frame_header_bytes += count;
            offset += count;
            if (lane->frame_header_bytes == MINI_STREAM_FRAME_HEADER_SIZE
                && mini_stream_begin_frame_locked(stream, lane) != 0)
            {
                return -1;
            }
            continue;
        }
        mini_stream_reorder_chunk_t *chunk = lane->incoming_chunk;
        size_t count = chunk->length - chunk->received;
        if (count > data_len - offset) {
            count = data_len - offset;
        }
        memcpy(chunk->data + chunk->received, data + offset, count);
        chunk->received += count;
        offset += count;
        if (chunk->received == chunk->length) {
            lane->incoming_chunk = NULL;
            lane->frame_header_bytes = 0;
            stream->records_received++;
            stream->lane_payload_received[lane->lane_index] += chunk->length;
            if (mini_stream_insert_receive_locked(stream, chunk) != 0) {
                return -1;
            }
        }
    }
    if (fin) {
        if (!lane->lane_header_parsed || !lane->protocol_fin_received
            || lane->frame_header_bytes != 0 || lane->incoming_chunk != NULL)
        {
            return mini_stream_fail_locked(stream, EPROTO);
        }
        lane->transport_fin_received = 1;
    }
    return mini_stream_pump_accept_receipt_locked(stream) < 0 ? -1 : 0;
}

static int
mini_stream_receive_receipt_locked(mini_backend_stream_t *stream,
    mini_stream_lane_t *lane, const unsigned char *data, size_t data_len,
    uint8_t fin)
{
    if (lane->lane_index != 0 && data_len != 0) {
        return mini_stream_fail_locked(stream, EPROTO);
    }
    if (lane->lane_index == 0 && data_len != 0) {
        if (data_len > MINI_STREAM_RECEIPT_SIZE - lane->receipt_bytes) {
            return mini_stream_fail_locked(stream, EPROTO);
        }
        memcpy(lane->receipt + lane->receipt_bytes, data, data_len);
        lane->receipt_bytes += data_len;
    }
    if (fin) {
        lane->transport_fin_received = 1;
        if (lane->lane_index == 0) {
            mini_stream_receipt_t receipt;
            if (lane->receipt_bytes != MINI_STREAM_RECEIPT_SIZE
                || mini_stream_receipt_decode(lane->receipt, &receipt) != 0
                || receipt.status != MINI_STREAM_RECEIPT_OK
                || receipt.session_id != stream->session_id
                || receipt.total_bytes != stream->accepted_bytes
                || receipt.digest != stream->digest)
            {
                return mini_stream_fail_locked(stream, EPROTO);
            }
            lane->receipt_valid = 1;
            stream->peer_received_bytes = receipt.total_bytes;
        }
    }
    uint32_t i;
    mini_stream_lane_t *lane0 = mini_stream_logical_lane(stream, 0);
    if (lane0 == NULL || !lane0->receipt_valid) {
        return 0;
    }
    for (i = 0; i < stream->lane_count; i++) {
        if (!stream->lanes[i].transport_fin_received) {
            return 0;
        }
    }
    stream->completed = 1;
    stream->completed_time_us = mini_stream_now();
    mini_stream_signal_locked(stream);
    return 0;
}

static mini_stream_lane_t *
mini_stream_lane_from_user_data(void *user_data)
{
    mini_stream_lane_t *lane = user_data;
    return lane != NULL && lane->magic == MINI_STREAM_LANE_MAGIC
        ? lane : NULL;
}

int
mini_stream_backend_passive_bs_create(mini_backend_stream_t *stream,
    xqc_h3_ext_bytestream_t *bs)
{
    if (!mini_stream_valid(stream) || bs == NULL
        || stream->role != MINI_STREAM_ENDPOINT_ACCEPT)
    {
        return XQC_ERROR;
    }
    pthread_mutex_lock(&stream->lock);
    mini_stream_lane_t *lane = NULL;
    uint32_t i;
    for (i = 0; i < stream->lane_count; i++) {
        if (!stream->lanes[i].allocated) {
            lane = &stream->lanes[i];
            break;
        }
    }
    if (lane == NULL) {
        (void)mini_stream_fail_locked(stream, EPROTO);
        pthread_mutex_unlock(&stream->lock);
        return XQC_ERROR;
    }
    lane->allocated = 1;
    lane->bs = bs;
    stream->allocated_lanes++;
    xqc_h3_ext_bytestream_set_user_data(bs, lane);
    pthread_mutex_unlock(&stream->lock);
    return XQC_OK;
}

int
mini_stream_backend_bs_close(xqc_h3_ext_bytestream_t *bs,
    void *bs_user_data)
{
    mini_stream_lane_t *lane = mini_stream_lane_from_user_data(bs_user_data);
    if (lane == NULL || !mini_stream_valid(lane->stream)) {
        return XQC_OK;
    }
    xqc_h3_ext_bytestream_stats_t stats =
        xqc_h3_ext_bytestream_get_stats(bs);
    pthread_mutex_lock(&lane->stream->lock);
    if (stats.fin_acked_time != 0) {
        lane->reverse_fin_acked = 1;
    }
    if (lane->stream->role == MINI_STREAM_ENDPOINT_ACCEPT) {
        lane->graceful_closed = lane->protocol_fin_received
            && lane->transport_fin_received
            && lane->reverse_fin_submitted
            && lane->reverse_fin_acked;
    } else {
        lane->graceful_closed = lane->protocol_fin_sent
            && lane->send_fin_submitted
            && lane->transport_fin_received;
    }
    lane->bs = NULL;
    (void)mini_stream_process_locked(lane->stream);
    if (!lane->stream->completed && !lane->stream->stop_requested
        && !lane->stream->reset_requested && !lane->graceful_closed)
    {
        (void)mini_stream_fail_locked(lane->stream, ECONNRESET);
    }
    pthread_mutex_unlock(&lane->stream->lock);
    return XQC_OK;
}

int
mini_stream_backend_bs_read(xqc_h3_ext_bytestream_t *bs, const void *data,
    size_t data_len, uint8_t fin, void *bs_user_data)
{
    (void)bs;
    mini_stream_lane_t *lane = mini_stream_lane_from_user_data(bs_user_data);
    if (lane == NULL || !mini_stream_valid(lane->stream)
        || (data == NULL && data_len != 0))
    {
        return XQC_ERROR;
    }
    mini_backend_stream_t *stream = lane->stream;
    pthread_mutex_lock(&stream->lock);
    int ret = stream->role == MINI_STREAM_ENDPOINT_ACCEPT
        ? mini_stream_receive_forward_locked(stream, lane, data, data_len, fin)
        : mini_stream_receive_receipt_locked(stream, lane, data, data_len, fin);
    pthread_mutex_unlock(&stream->lock);
    return ret == 0 ? XQC_OK : XQC_ERROR;
}

int
mini_stream_backend_bs_write(xqc_h3_ext_bytestream_t *bs,
    void *bs_user_data)
{
    (void)bs;
    mini_stream_lane_t *lane = mini_stream_lane_from_user_data(bs_user_data);
    if (lane == NULL || !mini_stream_valid(lane->stream)) {
        return XQC_ERROR;
    }
    pthread_mutex_lock(&lane->stream->lock);
    int ret = mini_stream_process_locked(lane->stream);
    pthread_mutex_unlock(&lane->stream->lock);
    return ret < 0 ? XQC_ERROR : XQC_OK;
}

int
mini_stream_backend_active_bs_create(xqc_h3_ext_bytestream_t *bs,
    void *bs_user_data)
{
    mini_stream_lane_t *lane = mini_stream_lane_from_user_data(bs_user_data);
    if (lane == NULL) {
        return XQC_ERROR;
    }
    lane->bs = bs;
    return XQC_OK;
}
