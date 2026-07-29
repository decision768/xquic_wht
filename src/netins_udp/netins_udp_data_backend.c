#define _POSIX_C_SOURCE 200809L

#include "netins_udp_data_backend.h"

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "netins_udp_v2_internal.h"

#define NETINS_UDP_DATA_LANE_COUNT          8U
#define NETINS_UDP_DATA_CLIENT_SEND_LANES   4U
#define NETINS_UDP_DATA_SERVER_SEND_LANES   8U
#define NETINS_UDP_DATA_FRAME_HEADER_SIZE   24U
#define NETINS_UDP_DATA_FRAME_PAYLOAD_SIZE  (4U * 1024U * 1024U)
#define NETINS_UDP_DATA_REORDER_LIMIT       (64U * 1024U * 1024U)
#define NETINS_UDP_DATA_FRAME_MAGIC         0x4e554432U /* "NUD2" */
#define NETINS_UDP_DATA_FRAME_VERSION       1U
#define NETINS_UDP_DATA_FRAME_FIN           0x01U
#define NETINS_UDP_DATA_PATH_PREFIX         "/netins/udp/stream/v2/"
#define NETINS_UDP_DATA_CONTENT_TYPE \
    "application/x-netins-udp-stream-v2"

_Static_assert(NETINS_UDP_DATA_REORDER_LIMIT
        >= NETINS_UDP_DATA_SERVER_SEND_LANES
            * NETINS_UDP_DATA_FRAME_PAYLOAD_SIZE,
    "reorder budget must keep every download lane active");

typedef struct netins_udp_data_segment_s {
    struct netins_udp_data_segment_s *next;
    uint64_t offset;
    size_t length;
    size_t data_offset;
    unsigned char *data;
} netins_udp_data_segment_t;

typedef struct netins_udp_data_lane_s {
    netins_udp_h3_request_t *request;

    unsigned char *tx_wire;
    size_t tx_wire_length;
    size_t tx_wire_offset;
    size_t tx_payload_length;
    int tx_fin_frame;
    int tx_fin_wire_sent;
    int tx_body_finished;

    unsigned char rx_header[NETINS_UDP_DATA_FRAME_HEADER_SIZE];
    size_t rx_header_have;
    uint64_t rx_frame_offset;
    uint32_t rx_frame_length;
    uint8_t rx_frame_flags;
    unsigned char *rx_payload;
    size_t rx_payload_have;
    int rx_peer_validated;
    int rx_protocol_finished;
} netins_udp_data_lane_t;

struct netins_udp_data_backend_s {
    pthread_mutex_t establish_lock;
    pthread_mutex_t state_lock;
    pthread_mutex_t send_lock;
    pthread_mutex_t recv_lock;

    netins_udp_t *connection;
    netins_udp_v2_notifier_t *parent_notifier;
    int role;
    int nonblock;
    uint32_t send_timeout_ms;
    uint32_t recv_timeout_ms;
    uint32_t close_timeout_ms;

    netins_udp_data_lane_t lanes[NETINS_UDP_DATA_LANE_COUNT];
    uint32_t lane_count;
    uint32_t send_lane_count;
    uint32_t recv_lane_count;
    int established;

    uint32_t next_send_lane;
    uint64_t send_offset;
    uint64_t app_bytes_sent;
    size_t send_queued_payload;
    int send_fin_requested;
    int send_finished;

    netins_udp_data_segment_t *recv_segments;
    uint64_t recv_offset;
    uint64_t app_bytes_received;
    uint64_t recv_final_offset;
    size_t recv_buffered;
    uint32_t recv_fin_count;
    int recv_final_offset_known;

    int error_code;
    char error_message[NETINS_UDP_ERROR_MSG_LEN];
};

typedef struct netins_udp_data_deadline_s {
    int enabled;
    uint64_t expires_ms;
} netins_udp_data_deadline_t;

static uint64_t
netins_udp_data_now_ms(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0;
    }
    return (uint64_t)now.tv_sec * 1000ULL
        + (uint64_t)now.tv_nsec / 1000000ULL;
}

static netins_udp_data_deadline_t
netins_udp_data_deadline(uint32_t timeout_ms)
{
    netins_udp_data_deadline_t deadline = {0, 0};
    if (timeout_ms != 0) {
        uint64_t now = netins_udp_data_now_ms();
        deadline.enabled = 1;
        deadline.expires_ms = UINT64_MAX - now < timeout_ms
            ? UINT64_MAX : now + timeout_ms;
    }
    return deadline;
}

static int
netins_udp_data_deadline_poll_ms(
    const netins_udp_data_deadline_t *deadline)
{
    if (!deadline->enabled) {
        return -1;
    }
    uint64_t now = netins_udp_data_now_ms();
    if (now >= deadline->expires_ms) {
        return 0;
    }
    uint64_t remaining = deadline->expires_ms - now;
    return remaining > (uint64_t)INT32_MAX
        ? INT32_MAX : (int)remaining;
}

static void
netins_udp_data_put_u16(unsigned char *output, uint16_t value)
{
    output[0] = (unsigned char)(value >> 8);
    output[1] = (unsigned char)value;
}

static void
netins_udp_data_put_u32(unsigned char *output, uint32_t value)
{
    output[0] = (unsigned char)(value >> 24);
    output[1] = (unsigned char)(value >> 16);
    output[2] = (unsigned char)(value >> 8);
    output[3] = (unsigned char)value;
}

static void
netins_udp_data_put_u64(unsigned char *output, uint64_t value)
{
    for (unsigned int i = 0; i < 8; ++i) {
        output[i] = (unsigned char)(value >> (56U - 8U * i));
    }
}

static uint16_t
netins_udp_data_get_u16(const unsigned char *input)
{
    return (uint16_t)((uint16_t)input[0] << 8)
        | (uint16_t)input[1];
}

static uint32_t
netins_udp_data_get_u32(const unsigned char *input)
{
    return ((uint32_t)input[0] << 24)
        | ((uint32_t)input[1] << 16)
        | ((uint32_t)input[2] << 8)
        | (uint32_t)input[3];
}

static uint64_t
netins_udp_data_get_u64(const unsigned char *input)
{
    uint64_t value = 0;
    for (unsigned int i = 0; i < 8; ++i) {
        value = (value << 8) | input[i];
    }
    return value;
}

static void
netins_udp_data_signal(netins_udp_data_backend_t *backend,
    uint32_t events)
{
    netins_udp_v2_notifier_signal(backend->parent_notifier, events);
}

static void
netins_udp_data_set_error(netins_udp_data_backend_t *backend,
    int error_code, const char *message)
{
    if (error_code == 0) {
        error_code = EIO;
    }
    pthread_mutex_lock(&backend->state_lock);
    if (backend->error_code == 0) {
        backend->error_code = error_code;
        snprintf(backend->error_message, sizeof(backend->error_message),
            "%s", message != NULL ? message : "data channel failed");
    }
    error_code = backend->error_code;
    char saved_message[NETINS_UDP_ERROR_MSG_LEN];
    snprintf(saved_message, sizeof(saved_message), "%s",
        backend->error_message);
    pthread_mutex_unlock(&backend->state_lock);

    netins_udp_v2_connection_set_error(backend->connection,
        error_code, saved_message);
    netins_udp_data_signal(backend,
        NETINS_UDP_EVENT_READABLE | NETINS_UDP_EVENT_WRITABLE);
}

static void
netins_udp_data_sync_request_error(
    netins_udp_data_backend_t *backend,
    netins_udp_h3_request_t *request, const char *fallback)
{
    int saved_errno = errno;
    int error_code = netins_udp_h3_request_get_last_error_code(request);
    char message[NETINS_UDP_ERROR_MSG_LEN] = {0};
    (void)netins_udp_h3_request_get_last_error_msg(request,
        message, sizeof(message));
    if (error_code == 0) {
        error_code = saved_errno != 0 ? saved_errno : EIO;
    }
    netins_udp_data_set_error(backend, error_code,
        message[0] != '\0' ? message : fallback);
    errno = error_code;
}

static int
netins_udp_data_current_error(netins_udp_data_backend_t *backend)
{
    pthread_mutex_lock(&backend->state_lock);
    int error_code = backend->error_code;
    pthread_mutex_unlock(&backend->state_lock);
    return error_code;
}

static int
netins_udp_data_parse_lane_path(const char *path, uint32_t *out_lane)
{
    if (path == NULL || out_lane == NULL) {
        return -1;
    }
    size_t prefix_length = strlen(NETINS_UDP_DATA_PATH_PREFIX);
    if (strncmp(path, NETINS_UDP_DATA_PATH_PREFIX, prefix_length) != 0) {
        return -1;
    }
    const char *number = path + prefix_length;
    if (number[0] == '\0' || number[1] != '\0'
        || number[0] < '0'
        || number[0] >= (char)('0' + NETINS_UDP_DATA_LANE_COUNT))
    {
        return -1;
    }
    *out_lane = (uint32_t)(number[0] - '0');
    return 0;
}

static int
netins_udp_data_establish_client_lane(
    netins_udp_data_backend_t *backend, uint32_t lane_index)
{
    netins_udp_h3_request_config_t config;
    netins_udp_h3_request_config_init(&config);
    snprintf(config.method, sizeof(config.method), "POST");
    snprintf(config.path, sizeof(config.path), "%s%u",
        NETINS_UDP_DATA_PATH_PREFIX, lane_index);
    snprintf(config.content_type, sizeof(config.content_type), "%s",
        NETINS_UDP_DATA_CONTENT_TYPE);
    config.content_length = NETINS_UDP_CONTENT_LENGTH_UNKNOWN;

    netins_udp_h3_request_t *request = NULL;
    if (netins_udp_v2_h3_request_open_internal(backend->connection,
            &config, &request) != NETINS_UDP_OK)
    {
        return NETINS_UDP_ERROR;
    }
    netins_udp_v2_h3_request_set_parent_notifier(request,
        backend->parent_notifier);
    backend->lanes[lane_index].request = request;
    backend->lane_count++;
    return NETINS_UDP_OK;
}

static int
netins_udp_data_establish_server_lane(
    netins_udp_data_backend_t *backend, int dontwait)
{
    netins_udp_h3_request_t *request = NULL;
    int accept_result = dontwait
        ? netins_udp_v2_h3_request_accept_internal_dontwait(
            backend->connection, &request)
        : netins_udp_v2_h3_request_accept_internal(
            backend->connection, &request);
    if (accept_result != NETINS_UDP_OK)
    {
        return NETINS_UDP_ERROR;
    }

    netins_udp_h3_request_info_t info =
        NETINS_UDP_H3_REQUEST_INFO_INITIALIZER;
    uint32_t lane_index = 0;
    if (netins_udp_h3_request_get_info(request, &info)
            != NETINS_UDP_OK
        || strcmp(info.method, "POST") != 0
        || strcmp(info.request_content_type,
            NETINS_UDP_DATA_CONTENT_TYPE) != 0
        || info.request_content_length
            != NETINS_UDP_CONTENT_LENGTH_UNKNOWN
        || netins_udp_data_parse_lane_path(info.path, &lane_index) != 0
        || backend->lanes[lane_index].request != NULL)
    {
        netins_udp_h3_request_close(request);
        errno = EPROTO;
        return NETINS_UDP_ERROR;
    }

    netins_udp_h3_response_config_t response;
    netins_udp_h3_response_config_init(&response);
    response.content_length = NETINS_UDP_CONTENT_LENGTH_UNKNOWN;
    snprintf(response.content_type, sizeof(response.content_type), "%s",
        NETINS_UDP_DATA_CONTENT_TYPE);
    if (netins_udp_h3_request_set_response(request, &response)
        != NETINS_UDP_OK)
    {
        netins_udp_h3_request_close(request);
        return NETINS_UDP_ERROR;
    }
    netins_udp_v2_h3_request_set_parent_notifier(request,
        backend->parent_notifier);
    backend->lanes[lane_index].request = request;
    backend->lanes[lane_index].rx_peer_validated = 1;
    backend->lane_count++;
    return NETINS_UDP_OK;
}

int
netins_udp_data_backend_create(
    const netins_udp_data_backend_config_t *config,
    netins_udp_data_backend_t **out_backend)
{
    if (config == NULL || out_backend == NULL
        || config->connection == NULL || config->parent_notifier == NULL
        || (config->role != NETINS_UDP_ROLE_CLIENT
            && config->role != NETINS_UDP_ROLE_ACCEPTED))
    {
        errno = EINVAL;
        return NETINS_UDP_ERROR;
    }
    *out_backend = NULL;

    netins_udp_data_backend_t *backend = calloc(1, sizeof(*backend));
    if (backend == NULL) {
        errno = ENOMEM;
        return NETINS_UDP_ERROR;
    }
    int initialized = 0;
    int error_code = pthread_mutex_init(&backend->establish_lock, NULL);
    if (error_code == 0) {
        initialized = 1;
        error_code = pthread_mutex_init(&backend->state_lock, NULL);
    }
    if (error_code == 0) {
        initialized = 2;
        error_code = pthread_mutex_init(&backend->send_lock, NULL);
    }
    if (error_code == 0) {
        initialized = 3;
        error_code = pthread_mutex_init(&backend->recv_lock, NULL);
    }
    if (error_code != 0) {
        if (initialized >= 3) {
            pthread_mutex_destroy(&backend->send_lock);
        }
        if (initialized >= 2) {
            pthread_mutex_destroy(&backend->state_lock);
        }
        if (initialized >= 1) {
            pthread_mutex_destroy(&backend->establish_lock);
        }
        free(backend);
        errno = error_code;
        return NETINS_UDP_ERROR;
    }

    backend->connection = config->connection;
    backend->parent_notifier = config->parent_notifier;
    backend->role = config->role;
    backend->nonblock = config->nonblock;
    backend->send_timeout_ms = config->send_timeout_ms;
    backend->recv_timeout_ms = config->recv_timeout_ms;
    backend->close_timeout_ms = config->close_timeout_ms;
    backend->send_lane_count = config->role == NETINS_UDP_ROLE_CLIENT
        ? NETINS_UDP_DATA_CLIENT_SEND_LANES
        : NETINS_UDP_DATA_SERVER_SEND_LANES;
    backend->recv_lane_count = config->role == NETINS_UDP_ROLE_CLIENT
        ? NETINS_UDP_DATA_SERVER_SEND_LANES
        : NETINS_UDP_DATA_CLIENT_SEND_LANES;
    *out_backend = backend;
    return NETINS_UDP_OK;
}

int
netins_udp_data_backend_establish(netins_udp_data_backend_t *backend,
    int dontwait)
{
    if (backend == NULL) {
        errno = EINVAL;
        return NETINS_UDP_ERROR;
    }
    pthread_mutex_lock(&backend->establish_lock);
    if (backend->established) {
        pthread_mutex_unlock(&backend->establish_lock);
        return NETINS_UDP_OK;
    }
    if (netins_udp_data_current_error(backend) != 0) {
        int error_code = netins_udp_data_current_error(backend);
        pthread_mutex_unlock(&backend->establish_lock);
        errno = error_code;
        return NETINS_UDP_ERROR;
    }

    while (backend->lane_count < NETINS_UDP_DATA_LANE_COUNT) {
        int result;
        if (backend->role == NETINS_UDP_ROLE_CLIENT) {
            uint32_t lane_index = backend->lane_count;
            result = netins_udp_data_establish_client_lane(backend,
                lane_index);
        } else {
            result = netins_udp_data_establish_server_lane(backend,
                backend->nonblock || dontwait);
        }
        if (result != NETINS_UDP_OK) {
            int error_code = errno != 0 ? errno : EIO;
            if (error_code != EAGAIN && error_code != ETIMEDOUT) {
                netins_udp_data_set_error(backend, error_code,
                    "failed to establish private parallel data channels");
            }
            pthread_mutex_unlock(&backend->establish_lock);
            errno = error_code;
            return NETINS_UDP_ERROR;
        }
    }
    backend->established = 1;
    pthread_mutex_unlock(&backend->establish_lock);
    netins_udp_data_signal(backend,
        NETINS_UDP_EVENT_READABLE | NETINS_UDP_EVENT_WRITABLE);
    return NETINS_UDP_OK;
}

static void
netins_udp_data_build_header(unsigned char *header, uint32_t lane,
    uint8_t flags, uint64_t offset, uint32_t length)
{
    netins_udp_data_put_u32(header, NETINS_UDP_DATA_FRAME_MAGIC);
    header[4] = NETINS_UDP_DATA_FRAME_VERSION;
    header[5] = flags;
    netins_udp_data_put_u16(header + 6, (uint16_t)lane);
    netins_udp_data_put_u64(header + 8, offset);
    netins_udp_data_put_u32(header + 16, length);
    netins_udp_data_put_u32(header + 20, 0);
}

static int
netins_udp_data_queue_frame(netins_udp_data_backend_t *backend,
    uint32_t lane_index, const unsigned char *data, size_t length,
    uint64_t offset, int fin)
{
    netins_udp_data_lane_t *lane = &backend->lanes[lane_index];
    if (lane->tx_wire != NULL
        || length > NETINS_UDP_DATA_FRAME_PAYLOAD_SIZE
        || length > UINT32_MAX)
    {
        errno = EBUSY;
        return NETINS_UDP_ERROR;
    }
    size_t wire_length = NETINS_UDP_DATA_FRAME_HEADER_SIZE + length;
    unsigned char *wire = malloc(wire_length);
    if (wire == NULL) {
        errno = ENOMEM;
        return NETINS_UDP_ERROR;
    }
    netins_udp_data_build_header(wire, lane_index,
        fin ? NETINS_UDP_DATA_FRAME_FIN : 0, offset, (uint32_t)length);
    if (length != 0) {
        memcpy(wire + NETINS_UDP_DATA_FRAME_HEADER_SIZE, data, length);
    }
    lane->tx_wire = wire;
    lane->tx_wire_length = wire_length;
    lane->tx_wire_offset = 0;
    lane->tx_payload_length = length;
    lane->tx_fin_frame = fin;
    backend->send_queued_payload += length;
    return NETINS_UDP_OK;
}

static int
netins_udp_data_pump_send_lane(netins_udp_data_backend_t *backend,
    uint32_t lane_index, int *made_progress)
{
    netins_udp_data_lane_t *lane = &backend->lanes[lane_index];
    if (lane->request == NULL || lane->tx_body_finished) {
        return NETINS_UDP_OK;
    }

    if (backend->send_fin_requested && lane->tx_wire == NULL
        && !lane->tx_fin_wire_sent
        && lane_index < backend->send_lane_count)
    {
        if (netins_udp_data_queue_frame(backend, lane_index, NULL, 0,
                backend->send_offset, 1) != NETINS_UDP_OK)
        {
            return NETINS_UDP_ERROR;
        }
    }

    if (lane->tx_wire != NULL) {
        size_t remaining = lane->tx_wire_length - lane->tx_wire_offset;
        ssize_t sent = netins_udp_h3_request_send(lane->request,
            lane->tx_wire + lane->tx_wire_offset, remaining,
            NETINS_UDP_MSG_DONTWAIT);
        if (sent < 0) {
            if (errno == EAGAIN || errno == ETIMEDOUT) {
                return NETINS_UDP_OK;
            }
            netins_udp_data_sync_request_error(backend, lane->request,
                "parallel data channel send failed");
            return NETINS_UDP_ERROR;
        }
        if (sent > 0) {
            lane->tx_wire_offset += (size_t)sent;
            *made_progress = 1;
        }
        if (lane->tx_wire_offset == lane->tx_wire_length) {
            if (backend->send_queued_payload
                >= lane->tx_payload_length)
            {
                backend->send_queued_payload -=
                    lane->tx_payload_length;
            }
            if (lane->tx_fin_frame) {
                lane->tx_fin_wire_sent = 1;
            }
            free(lane->tx_wire);
            lane->tx_wire = NULL;
            lane->tx_wire_length = 0;
            lane->tx_wire_offset = 0;
            lane->tx_payload_length = 0;
            lane->tx_fin_frame = 0;
            *made_progress = 1;
        }
    }

    int should_finish_body = backend->send_fin_requested
        && lane->tx_wire == NULL
        && (lane_index >= backend->send_lane_count
            || lane->tx_fin_wire_sent);
    if (should_finish_body && !lane->tx_body_finished) {
        ssize_t result = netins_udp_h3_request_send(lane->request,
            NULL, 0, NETINS_UDP_MSG_DONTWAIT | NETINS_UDP_MSG_FIN);
        if (result < 0) {
            if (errno == EAGAIN || errno == ETIMEDOUT) {
                return NETINS_UDP_OK;
            }
            netins_udp_data_sync_request_error(backend, lane->request,
                "failed to finish a parallel data channel");
            return NETINS_UDP_ERROR;
        }
        lane->tx_body_finished = 1;
        *made_progress = 1;
    }
    return NETINS_UDP_OK;
}

static int
netins_udp_data_pump_send(netins_udp_data_backend_t *backend,
    int *made_progress)
{
    *made_progress = 0;
    for (uint32_t i = 0; i < NETINS_UDP_DATA_LANE_COUNT; ++i) {
        if (netins_udp_data_pump_send_lane(backend, i,
                made_progress) != NETINS_UDP_OK)
        {
            return NETINS_UDP_ERROR;
        }
    }
    if (backend->send_fin_requested) {
        int complete = 1;
        for (uint32_t i = 0; i < NETINS_UDP_DATA_LANE_COUNT; ++i) {
            if (!backend->lanes[i].tx_body_finished) {
                complete = 0;
                break;
            }
        }
        if (complete) {
            backend->send_finished = 1;
        }
    }
    return NETINS_UDP_OK;
}

static int
netins_udp_data_wait(netins_udp_data_backend_t *backend,
    const netins_udp_data_deadline_t *deadline)
{
    struct pollfd descriptors[NETINS_UDP_DATA_LANE_COUNT];
    nfds_t count = 0;
    for (uint32_t i = 0; i < NETINS_UDP_DATA_LANE_COUNT; ++i) {
        if (backend->lanes[i].request == NULL) {
            continue;
        }
        int fd =
            netins_udp_h3_request_get_fd(backend->lanes[i].request);
        if (fd < 0) {
            netins_udp_data_sync_request_error(backend,
                backend->lanes[i].request,
                "data channel notification descriptor failed");
            return NETINS_UDP_ERROR;
        }
        descriptors[count].fd = fd;
        descriptors[count].events = POLLIN;
        descriptors[count].revents = 0;
        count++;
    }
    if (count == 0) {
        errno = ENOTCONN;
        return NETINS_UDP_ERROR;
    }

    int timeout_ms = netins_udp_data_deadline_poll_ms(deadline);
    if (timeout_ms == 0) {
        errno = ETIMEDOUT;
        return NETINS_UDP_ERROR;
    }
    int result;
    do {
        result = poll(descriptors, count, timeout_ms);
    } while (result < 0 && errno == EINTR);
    if (result == 0) {
        errno = ETIMEDOUT;
        return NETINS_UDP_ERROR;
    }
    if (result < 0) {
        return NETINS_UDP_ERROR;
    }

    for (uint32_t i = 0; i < NETINS_UDP_DATA_LANE_COUNT; ++i) {
        if (backend->lanes[i].request == NULL) {
            continue;
        }
        uint32_t events = 0;
        if (netins_udp_h3_request_get_events(
                backend->lanes[i].request, &events) != NETINS_UDP_OK)
        {
            netins_udp_data_sync_request_error(backend,
                backend->lanes[i].request,
                "failed to query a parallel data channel");
            return NETINS_UDP_ERROR;
        }
        if ((events & NETINS_UDP_EVENT_ERROR) != 0) {
            netins_udp_data_sync_request_error(backend,
                backend->lanes[i].request,
                "parallel data channel failed");
            return NETINS_UDP_ERROR;
        }
    }
    return NETINS_UDP_OK;
}

ssize_t
netins_udp_data_backend_send(netins_udp_data_backend_t *backend,
    const void *data, size_t length, int flags)
{
    if (backend == NULL
        || (length != 0 && data == NULL)
        || (flags & ~(NETINS_UDP_MSG_DONTWAIT
            | NETINS_UDP_MSG_FIN)) != 0)
    {
        errno = EINVAL;
        return -1;
    }
    if (netins_udp_data_backend_establish(backend,
            (flags & NETINS_UDP_MSG_DONTWAIT) != 0)
        != NETINS_UDP_OK)
    {
        return -1;
    }

    int dontwait = backend->nonblock
        || (flags & NETINS_UDP_MSG_DONTWAIT) != 0;
    netins_udp_data_deadline_t deadline =
        netins_udp_data_deadline(backend->send_timeout_ms);
    const unsigned char *bytes = data;
    size_t target_length = length;
#ifdef SSIZE_MAX
    if (target_length > (size_t)SSIZE_MAX) {
        target_length = (size_t)SSIZE_MAX;
    }
#endif
    size_t accepted = 0;

    pthread_mutex_lock(&backend->send_lock);
    int error_code = netins_udp_data_current_error(backend);
    if (error_code != 0) {
        pthread_mutex_unlock(&backend->send_lock);
        errno = error_code;
        return -1;
    }
    if (backend->send_fin_requested) {
        pthread_mutex_unlock(&backend->send_lock);
        errno = EPIPE;
        return -1;
    }
    if (length == 0 && (flags & NETINS_UDP_MSG_FIN) == 0) {
        pthread_mutex_unlock(&backend->send_lock);
        return 0;
    }

    while (accepted < target_length) {
        for (uint32_t attempt = 0;
                attempt < backend->send_lane_count
                    && accepted < target_length;
                ++attempt)
        {
            uint32_t lane_index = backend->next_send_lane
                % backend->send_lane_count;
            backend->next_send_lane =
                (lane_index + 1) % backend->send_lane_count;
            netins_udp_data_lane_t *lane =
                &backend->lanes[lane_index];
            if (lane->tx_wire != NULL) {
                continue;
            }
            size_t amount = target_length - accepted;
            if (amount > NETINS_UDP_DATA_FRAME_PAYLOAD_SIZE) {
                amount = NETINS_UDP_DATA_FRAME_PAYLOAD_SIZE;
            }
            if (UINT64_MAX - backend->send_offset < amount) {
                netins_udp_data_set_error(backend, EOVERFLOW,
                    "connection byte offset overflow");
                break;
            }
            if (netins_udp_data_queue_frame(backend, lane_index,
                    bytes + accepted, amount, backend->send_offset, 0)
                != NETINS_UDP_OK)
            {
                if (accepted == 0) {
                    error_code = errno != 0 ? errno : ENOMEM;
                }
                break;
            }
            backend->send_offset += amount;
            accepted += amount;
        }

        int made_progress = 0;
        if (netins_udp_data_pump_send(backend, &made_progress)
            != NETINS_UDP_OK)
        {
            error_code = netins_udp_data_current_error(backend);
            break;
        }
        int backend_error = netins_udp_data_current_error(backend);
        if (backend_error != 0) {
            error_code = backend_error;
            break;
        }
        if (error_code != 0) {
            break;
        }
        if (accepted == target_length) {
            break;
        }
        if (made_progress) {
            continue;
        }
        if (accepted != 0) {
            break;
        }
        if (dontwait) {
            error_code = EAGAIN;
            break;
        }
        if (netins_udp_data_wait(backend, &deadline)
            != NETINS_UDP_OK)
        {
            error_code = errno != 0 ? errno : EIO;
            break;
        }
    }

    if (accepted == length && (flags & NETINS_UDP_MSG_FIN) != 0) {
        backend->send_fin_requested = 1;
        int made_progress = 0;
        if (netins_udp_data_pump_send(backend, &made_progress)
            != NETINS_UDP_OK
            && accepted == 0)
        {
            error_code = netins_udp_data_current_error(backend);
        }
    }
    if (!dontwait && error_code == 0
        && accepted == target_length)
    {
        int need_fin = accepted == length
            && (flags & NETINS_UDP_MSG_FIN) != 0;
        while (backend->send_queued_payload != 0
            || (need_fin && !backend->send_finished))
        {
            int made_progress = 0;
            if (netins_udp_data_pump_send(backend, &made_progress)
                != NETINS_UDP_OK)
            {
                error_code = netins_udp_data_current_error(backend);
                break;
            }
            if (backend->send_queued_payload == 0
                && (!need_fin || backend->send_finished))
            {
                break;
            }
            if (!made_progress
                && netins_udp_data_wait(backend, &deadline)
                    != NETINS_UDP_OK)
            {
                error_code = errno != 0 ? errno : ETIMEDOUT;
                break;
            }
        }
    }
    pthread_mutex_lock(&backend->state_lock);
    backend->app_bytes_sent = backend->send_offset;
    pthread_mutex_unlock(&backend->state_lock);
    pthread_mutex_unlock(&backend->send_lock);

    if (accepted != 0 || (length == 0
            && (flags & NETINS_UDP_MSG_FIN) != 0
            && error_code == 0))
    {
        netins_udp_data_signal(backend, NETINS_UDP_EVENT_WRITABLE);
        return (ssize_t)accepted;
    }
    errno = error_code != 0 ? error_code : EAGAIN;
    return -1;
}

static int
netins_udp_data_insert_segment(netins_udp_data_backend_t *backend,
    uint64_t offset, unsigned char *payload, size_t length)
{
    if (length == 0 || payload == NULL
        || UINT64_MAX - offset < length)
    {
        errno = EPROTO;
        return NETINS_UDP_ERROR;
    }
    netins_udp_data_segment_t **cursor = &backend->recv_segments;
    netins_udp_data_segment_t *previous = NULL;
    while (*cursor != NULL && (*cursor)->offset < offset) {
        previous = *cursor;
        cursor = &(*cursor)->next;
    }
    if ((previous != NULL
            && previous->offset + previous->length > offset)
        || (*cursor != NULL
            && offset + length > (*cursor)->offset)
        || offset < backend->recv_offset
        || (backend->recv_final_offset_known
            && offset + length > backend->recv_final_offset))
    {
        errno = EPROTO;
        return NETINS_UDP_ERROR;
    }

    netins_udp_data_segment_t *segment = malloc(sizeof(*segment));
    if (segment == NULL) {
        errno = ENOMEM;
        return NETINS_UDP_ERROR;
    }
    segment->offset = offset;
    segment->length = length;
    segment->data_offset = 0;
    segment->data = payload;
    segment->next = *cursor;
    *cursor = segment;
    backend->recv_buffered += length;
    if (offset == backend->recv_offset) {
        netins_udp_data_signal(backend, NETINS_UDP_EVENT_READABLE);
    }
    return NETINS_UDP_OK;
}

static int
netins_udp_data_complete_rx_frame(
    netins_udp_data_backend_t *backend, uint32_t lane_index)
{
    netins_udp_data_lane_t *lane = &backend->lanes[lane_index];
    int result = NETINS_UDP_OK;
    if ((lane->rx_frame_flags & NETINS_UDP_DATA_FRAME_FIN) != 0) {
        if (lane->rx_frame_length != 0 || lane->rx_protocol_finished) {
            errno = EPROTO;
            result = NETINS_UDP_ERROR;
        } else if (lane->rx_frame_offset < backend->recv_offset) {
            errno = EPROTO;
            result = NETINS_UDP_ERROR;
        } else if (backend->recv_final_offset_known
            && lane->rx_frame_offset != backend->recv_final_offset)
        {
            errno = EPROTO;
            result = NETINS_UDP_ERROR;
        } else {
            for (netins_udp_data_segment_t *segment =
                    backend->recv_segments;
                    segment != NULL; segment = segment->next)
            {
                if (segment->offset + segment->length
                    > lane->rx_frame_offset)
                {
                    errno = EPROTO;
                    result = NETINS_UDP_ERROR;
                    break;
                }
            }
        }
        if (result == NETINS_UDP_OK) {
            backend->recv_final_offset = lane->rx_frame_offset;
            backend->recv_final_offset_known = 1;
            backend->recv_fin_count++;
            lane->rx_protocol_finished = 1;
        }
    } else if (lane->rx_protocol_finished
        || lane->rx_frame_length == 0)
    {
        errno = EPROTO;
        result = NETINS_UDP_ERROR;
    } else {
        result = netins_udp_data_insert_segment(backend,
            lane->rx_frame_offset, lane->rx_payload,
            lane->rx_frame_length);
        if (result == NETINS_UDP_OK) {
            lane->rx_payload = NULL;
        }
    }

    free(lane->rx_payload);
    lane->rx_payload = NULL;
    lane->rx_payload_have = 0;
    lane->rx_header_have = 0;
    lane->rx_frame_offset = 0;
    lane->rx_frame_length = 0;
    lane->rx_frame_flags = 0;
    if (result != NETINS_UDP_OK) {
        netins_udp_data_set_error(backend,
            errno != 0 ? errno : EPROTO,
            "peer sent an invalid ordered data frame");
    } else if (backend->recv_fin_count == backend->recv_lane_count) {
        netins_udp_data_signal(backend, NETINS_UDP_EVENT_READABLE);
    }
    return result;
}

static int
netins_udp_data_parse_rx_header(netins_udp_data_backend_t *backend,
    uint32_t lane_index)
{
    netins_udp_data_lane_t *lane = &backend->lanes[lane_index];
    const unsigned char *header = lane->rx_header;
    uint32_t magic = netins_udp_data_get_u32(header);
    uint8_t version = header[4];
    uint8_t flags = header[5];
    uint16_t encoded_lane = netins_udp_data_get_u16(header + 6);
    uint64_t offset = netins_udp_data_get_u64(header + 8);
    uint32_t length = netins_udp_data_get_u32(header + 16);
    uint32_t reserved = netins_udp_data_get_u32(header + 20);
    if (magic != NETINS_UDP_DATA_FRAME_MAGIC
        || version != NETINS_UDP_DATA_FRAME_VERSION
        || (flags & ~NETINS_UDP_DATA_FRAME_FIN) != 0
        || encoded_lane != lane_index || reserved != 0
        || length > NETINS_UDP_DATA_FRAME_PAYLOAD_SIZE
        || ((flags & NETINS_UDP_DATA_FRAME_FIN) != 0 && length != 0))
    {
        errno = EPROTO;
        netins_udp_data_set_error(backend, EPROTO,
            "peer sent an incompatible ordered data frame");
        return NETINS_UDP_ERROR;
    }
    lane->rx_frame_offset = offset;
    lane->rx_frame_length = length;
    lane->rx_frame_flags = flags;
    if (length == 0) {
        return netins_udp_data_complete_rx_frame(backend, lane_index);
    }
    lane->rx_payload = malloc(length);
    if (lane->rx_payload == NULL) {
        netins_udp_data_set_error(backend, ENOMEM,
            "failed to allocate a receive frame");
        errno = ENOMEM;
        return NETINS_UDP_ERROR;
    }
    return NETINS_UDP_OK;
}

static int
netins_udp_data_pump_recv_lane(netins_udp_data_backend_t *backend,
    uint32_t lane_index, int *made_progress)
{
    netins_udp_data_lane_t *lane = &backend->lanes[lane_index];
    if (lane->request == NULL || lane->rx_protocol_finished) {
        return NETINS_UDP_OK;
    }
    for (unsigned int operation = 0; operation < 8; ++operation) {
        void *output;
        size_t wanted;
        if (lane->rx_header_have
            < NETINS_UDP_DATA_FRAME_HEADER_SIZE)
        {
            output = lane->rx_header + lane->rx_header_have;
            wanted = NETINS_UDP_DATA_FRAME_HEADER_SIZE
                - lane->rx_header_have;
        } else {
            if (lane->rx_payload == NULL) {
                if (netins_udp_data_parse_rx_header(backend,
                        lane_index) != NETINS_UDP_OK)
                {
                    return NETINS_UDP_ERROR;
                }
                if (lane->rx_header_have == 0) {
                    *made_progress = 1;
                    continue;
                }
            }
            /*
             * Once the reorder budget is full, keep reading the exact next
             * frame only. This guarantees that a fast lane cannot permanently
             * hide the missing prefix behind buffered out-of-order data.
             */
            if (backend->recv_buffered + lane->rx_frame_length
                    > NETINS_UDP_DATA_REORDER_LIMIT
                && lane->rx_frame_offset != backend->recv_offset)
            {
                return NETINS_UDP_OK;
            }
            output = lane->rx_payload + lane->rx_payload_have;
            wanted = lane->rx_frame_length - lane->rx_payload_have;
        }

        ssize_t received = netins_udp_h3_request_recv(lane->request,
            output, wanted, NETINS_UDP_MSG_DONTWAIT);
        if (received < 0) {
            if (errno == EAGAIN || errno == ETIMEDOUT) {
                return NETINS_UDP_OK;
            }
            netins_udp_data_sync_request_error(backend, lane->request,
                "parallel data channel receive failed");
            return NETINS_UDP_ERROR;
        }
        if (received == 0) {
            netins_udp_data_set_error(backend, EPROTO,
                "parallel data channel ended before its final frame");
            errno = EPROTO;
            return NETINS_UDP_ERROR;
        }
        if (!lane->rx_peer_validated) {
            netins_udp_h3_request_info_t info =
                NETINS_UDP_H3_REQUEST_INFO_INITIALIZER;
            if (netins_udp_h3_request_get_info(lane->request, &info)
                    != NETINS_UDP_OK
                || !info.response_headers_ready
                || info.response_status < 200
                || info.response_status >= 300
                || info.response_content_length
                    != NETINS_UDP_CONTENT_LENGTH_UNKNOWN
                || strcmp(info.response_content_type,
                    NETINS_UDP_DATA_CONTENT_TYPE) != 0)
            {
                netins_udp_data_set_error(backend, EPROTO,
                    "peer returned an incompatible data-channel response");
                errno = EPROTO;
                return NETINS_UDP_ERROR;
            }
            lane->rx_peer_validated = 1;
        }
        *made_progress = 1;
        if (lane->rx_header_have
            < NETINS_UDP_DATA_FRAME_HEADER_SIZE)
        {
            lane->rx_header_have += (size_t)received;
            if (lane->rx_header_have
                == NETINS_UDP_DATA_FRAME_HEADER_SIZE)
            {
                if (netins_udp_data_parse_rx_header(backend,
                        lane_index) != NETINS_UDP_OK)
                {
                    return NETINS_UDP_ERROR;
                }
            }
        } else {
            lane->rx_payload_have += (size_t)received;
            if (lane->rx_payload_have == lane->rx_frame_length) {
                if (netins_udp_data_complete_rx_frame(backend,
                        lane_index) != NETINS_UDP_OK)
                {
                    return NETINS_UDP_ERROR;
                }
            }
        }
    }
    return NETINS_UDP_OK;
}

static int
netins_udp_data_pump_recv(netins_udp_data_backend_t *backend,
    int *made_progress)
{
    *made_progress = 0;
    for (uint32_t i = 0; i < backend->recv_lane_count; ++i) {
        if (netins_udp_data_pump_recv_lane(backend, i,
                made_progress) != NETINS_UDP_OK)
        {
            return NETINS_UDP_ERROR;
        }
    }
    return NETINS_UDP_OK;
}

static size_t
netins_udp_data_copy_received(netins_udp_data_backend_t *backend,
    unsigned char *output, size_t capacity, int peek)
{
    size_t copied = 0;
    uint64_t expected = backend->recv_offset;
    netins_udp_data_segment_t *segment = backend->recv_segments;
    while (segment != NULL && segment->offset == expected
        && copied < capacity)
    {
        size_t amount = segment->length;
        if (amount > capacity - copied) {
            amount = capacity - copied;
        }
        memcpy(output + copied,
            segment->data + segment->data_offset, amount);
        copied += amount;
        expected += amount;
        if (amount != segment->length) {
            break;
        }
        segment = segment->next;
    }
    if (peek || copied == 0) {
        return copied;
    }

    size_t remaining = copied;
    while (remaining != 0) {
        segment = backend->recv_segments;
        size_t amount = segment->length;
        if (amount <= remaining) {
            backend->recv_segments = segment->next;
            backend->recv_offset += amount;
            backend->recv_buffered -= amount;
            remaining -= amount;
            free(segment->data);
            free(segment);
        } else {
            segment->offset += remaining;
            segment->length -= remaining;
            segment->data_offset += remaining;
            backend->recv_offset += remaining;
            backend->recv_buffered -= remaining;
            remaining = 0;
        }
    }
    pthread_mutex_lock(&backend->state_lock);
    backend->app_bytes_received = backend->recv_offset;
    pthread_mutex_unlock(&backend->state_lock);
    return copied;
}

static void
netins_udp_data_pump_pending_send_from_recv(
    netins_udp_data_backend_t *backend)
{
    if (pthread_mutex_trylock(&backend->send_lock) != 0) {
        return;
    }
    int made_progress = 0;
    (void)netins_udp_data_pump_send(backend, &made_progress);
    pthread_mutex_unlock(&backend->send_lock);
}

ssize_t
netins_udp_data_backend_recv(netins_udp_data_backend_t *backend,
    void *data, size_t length, int flags)
{
    if (backend == NULL
        || (length != 0 && data == NULL)
        || (flags & ~(NETINS_UDP_MSG_DONTWAIT
            | NETINS_UDP_MSG_PEEK)) != 0)
    {
        errno = EINVAL;
        return -1;
    }
    if (netins_udp_data_backend_establish(backend,
            (flags & NETINS_UDP_MSG_DONTWAIT) != 0)
        != NETINS_UDP_OK)
    {
        return -1;
    }
    if (length == 0) {
        return 0;
    }
    size_t target_length = length;
#ifdef SSIZE_MAX
    if (target_length > (size_t)SSIZE_MAX) {
        target_length = (size_t)SSIZE_MAX;
    }
#endif

    int dontwait = backend->nonblock
        || (flags & NETINS_UDP_MSG_DONTWAIT) != 0;
    int peek = (flags & NETINS_UDP_MSG_PEEK) != 0;
    netins_udp_data_deadline_t deadline =
        netins_udp_data_deadline(backend->recv_timeout_ms);

    pthread_mutex_lock(&backend->recv_lock);
    for (;;) {
        size_t copied = netins_udp_data_copy_received(backend,
            data, target_length, peek);
        if (copied != 0) {
            pthread_mutex_unlock(&backend->recv_lock);
            return (ssize_t)copied;
        }
        if (backend->recv_final_offset_known
            && backend->recv_fin_count == backend->recv_lane_count
            && backend->recv_offset == backend->recv_final_offset)
        {
            pthread_mutex_unlock(&backend->recv_lock);
            return 0;
        }
        if (backend->recv_final_offset_known
            && backend->recv_fin_count == backend->recv_lane_count)
        {
            netins_udp_data_set_error(backend, EPROTO,
                "ordered data stream ended with a missing byte range");
        }
        int error_code = netins_udp_data_current_error(backend);
        if (error_code != 0) {
            pthread_mutex_unlock(&backend->recv_lock);
            errno = error_code;
            return -1;
        }

        netins_udp_data_pump_pending_send_from_recv(backend);
        int made_progress = 0;
        if (netins_udp_data_pump_recv(backend, &made_progress)
            != NETINS_UDP_OK)
        {
            error_code = netins_udp_data_current_error(backend);
            pthread_mutex_unlock(&backend->recv_lock);
            errno = error_code != 0 ? error_code : EIO;
            return -1;
        }
        if (made_progress) {
            continue;
        }
        if (dontwait) {
            pthread_mutex_unlock(&backend->recv_lock);
            errno = EAGAIN;
            return -1;
        }
        if (netins_udp_data_wait(backend, &deadline)
            != NETINS_UDP_OK)
        {
            error_code = errno != 0 ? errno : EIO;
            pthread_mutex_unlock(&backend->recv_lock);
            errno = error_code;
            return -1;
        }
    }
}

int
netins_udp_data_backend_finish_send(
    netins_udp_data_backend_t *backend)
{
    if (backend == NULL) {
        errno = EINVAL;
        return NETINS_UDP_ERROR;
    }
    if (netins_udp_data_backend_establish(backend, 0)
        != NETINS_UDP_OK)
    {
        return NETINS_UDP_ERROR;
    }
    netins_udp_data_deadline_t deadline =
        netins_udp_data_deadline(backend->close_timeout_ms);

    pthread_mutex_lock(&backend->send_lock);
    backend->send_fin_requested = 1;
    while (!backend->send_finished) {
        int made_progress = 0;
        if (netins_udp_data_pump_send(backend, &made_progress)
            != NETINS_UDP_OK)
        {
            int error_code = netins_udp_data_current_error(backend);
            pthread_mutex_unlock(&backend->send_lock);
            errno = error_code != 0 ? error_code : EIO;
            return NETINS_UDP_ERROR;
        }
        if (backend->send_finished) {
            break;
        }
        if (!made_progress
            && netins_udp_data_wait(backend, &deadline)
                != NETINS_UDP_OK)
        {
            int error_code = errno != 0 ? errno : ETIMEDOUT;
            pthread_mutex_unlock(&backend->send_lock);
            errno = error_code;
            return NETINS_UDP_ERROR;
        }
    }
    pthread_mutex_unlock(&backend->send_lock);
    return NETINS_UDP_OK;
}

int
netins_udp_data_backend_get_events(
    netins_udp_data_backend_t *backend, uint32_t *out_events)
{
    if (backend == NULL || out_events == NULL) {
        errno = EINVAL;
        return NETINS_UDP_ERROR;
    }
    *out_events = 0;
    pthread_mutex_lock(&backend->establish_lock);
    int established = backend->established;
    pthread_mutex_unlock(&backend->establish_lock);
    if (!established) {
        if (backend->role == NETINS_UDP_ROLE_CLIENT) {
            *out_events = NETINS_UDP_EVENT_WRITABLE;
        }
        return NETINS_UDP_OK;
    }

    if (pthread_mutex_trylock(&backend->send_lock) == 0) {
        int made_progress = 0;
        (void)netins_udp_data_pump_send(backend, &made_progress);
        pthread_mutex_unlock(&backend->send_lock);
    }
    if (pthread_mutex_trylock(&backend->recv_lock) == 0) {
        int made_progress = 0;
        (void)netins_udp_data_pump_recv(backend, &made_progress);
        pthread_mutex_unlock(&backend->recv_lock);
    }

    for (uint32_t i = 0; i < NETINS_UDP_DATA_LANE_COUNT; ++i) {
        uint32_t request_events = 0;
        if (netins_udp_h3_request_get_events(
                backend->lanes[i].request,
                &request_events) != NETINS_UDP_OK)
        {
            netins_udp_data_sync_request_error(backend,
                backend->lanes[i].request,
                "failed to query a parallel data channel");
            break;
        }
        if ((request_events & NETINS_UDP_EVENT_ERROR) != 0) {
            netins_udp_data_sync_request_error(backend,
                backend->lanes[i].request,
                "parallel data channel failed");
            break;
        }
    }

    pthread_mutex_lock(&backend->recv_lock);
    int readable = backend->recv_segments != NULL
        && backend->recv_segments->offset == backend->recv_offset;
    int eof = backend->recv_final_offset_known
        && backend->recv_fin_count == backend->recv_lane_count
        && backend->recv_offset == backend->recv_final_offset;
    pthread_mutex_unlock(&backend->recv_lock);

    pthread_mutex_lock(&backend->send_lock);
    int writable = !backend->send_fin_requested;
    if (writable) {
        writable = 0;
        for (uint32_t i = 0; i < backend->send_lane_count; ++i) {
            if (backend->lanes[i].tx_wire == NULL) {
                writable = 1;
                break;
            }
        }
    }
    pthread_mutex_unlock(&backend->send_lock);

    int error_code = netins_udp_data_current_error(backend);
    if (readable || eof || error_code != 0) {
        *out_events |= NETINS_UDP_EVENT_READABLE;
    }
    if (writable || error_code != 0) {
        *out_events |= NETINS_UDP_EVENT_WRITABLE;
    }
    return NETINS_UDP_OK;
}

int
netins_udp_data_backend_get_error(
    netins_udp_data_backend_t *backend, int *out_error,
    char *message, size_t message_capacity)
{
    if (backend == NULL || out_error == NULL
        || (message == NULL && message_capacity != 0))
    {
        errno = EINVAL;
        return NETINS_UDP_ERROR;
    }
    pthread_mutex_lock(&backend->state_lock);
    *out_error = backend->error_code;
    if (message != NULL && message_capacity != 0) {
        snprintf(message, message_capacity, "%s",
            backend->error_message);
    }
    pthread_mutex_unlock(&backend->state_lock);
    return NETINS_UDP_OK;
}

int
netins_udp_data_backend_get_counters(
    netins_udp_data_backend_t *backend,
    uint64_t *out_bytes_sent, uint64_t *out_bytes_received)
{
    if (backend == NULL || out_bytes_sent == NULL
        || out_bytes_received == NULL)
    {
        errno = EINVAL;
        return NETINS_UDP_ERROR;
    }

    pthread_mutex_lock(&backend->state_lock);
    *out_bytes_sent = backend->app_bytes_sent;
    *out_bytes_received = backend->app_bytes_received;
    pthread_mutex_unlock(&backend->state_lock);
    return NETINS_UDP_OK;
}

void
netins_udp_data_backend_destroy(
    netins_udp_data_backend_t *backend)
{
    if (backend == NULL) {
        return;
    }
    for (uint32_t i = 0; i < NETINS_UDP_DATA_LANE_COUNT; ++i) {
        netins_udp_data_lane_t *lane = &backend->lanes[i];
        if (lane->request != NULL) {
            netins_udp_v2_h3_request_set_parent_notifier(
                lane->request, NULL);
            netins_udp_h3_request_close(lane->request);
            lane->request = NULL;
        }
        free(lane->tx_wire);
        free(lane->rx_payload);
    }
    netins_udp_data_segment_t *segment = backend->recv_segments;
    while (segment != NULL) {
        netins_udp_data_segment_t *next = segment->next;
        free(segment->data);
        free(segment);
        segment = next;
    }
    pthread_mutex_destroy(&backend->recv_lock);
    pthread_mutex_destroy(&backend->send_lock);
    pthread_mutex_destroy(&backend->state_lock);
    pthread_mutex_destroy(&backend->establish_lock);
    free(backend);
}
