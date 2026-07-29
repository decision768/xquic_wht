#define _POSIX_C_SOURCE 200809L

#include "netins_udp_v2_notifier.h"

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>

#define NETINS_UDP_V2_NOTIFIER_TRANSITION_EVENTS \
    ((1U << 0) | (1U << 4) | (1U << 5) | (1U << 6))
#define NETINS_UDP_V2_NOTIFIER_LEVEL_EVENTS \
    ((1U << 1) | (1U << 2) | (1U << 3) | (1U << 8))
#define NETINS_UDP_V2_NOTIFIER_REARM_EVENTS \
    ((1U << 1) | (1U << 2) | (1U << 8))

static int
netins_udp_v2_notifier_nonblocking_cloexec(int fd)
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
netins_udp_v2_notifier_write_locked(netins_udp_v2_notifier_t *notifier)
{
    if (notifier->signaled || notifier->write_fd < 0) {
        return;
    }
    unsigned char signal_byte = 1;
    ssize_t result;
    do {
        result = write(notifier->write_fd, &signal_byte,
            sizeof(signal_byte));
    } while (result < 0 && errno == EINTR);
    if (result == (ssize_t)sizeof(signal_byte)
        || (result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)))
    {
        notifier->signaled = 1;
    }
}

int
netins_udp_v2_notifier_init(netins_udp_v2_notifier_t *notifier)
{
    if (notifier == NULL) {
        errno = EINVAL;
        return -1;
    }
    memset(notifier, 0, sizeof(*notifier));
    notifier->read_fd = -1;
    notifier->write_fd = -1;
    int init_error = pthread_mutex_init(&notifier->lock, NULL);
    if (init_error != 0) {
        errno = init_error;
        return -1;
    }
    int descriptors[2] = {-1, -1};
    if (pipe(descriptors) != 0
        || netins_udp_v2_notifier_nonblocking_cloexec(descriptors[0]) != 0
        || netins_udp_v2_notifier_nonblocking_cloexec(descriptors[1]) != 0)
    {
        int error_code = errno != 0 ? errno : EIO;
        if (descriptors[0] >= 0) {
            close(descriptors[0]);
        }
        if (descriptors[1] >= 0) {
            close(descriptors[1]);
        }
        pthread_mutex_destroy(&notifier->lock);
        errno = error_code;
        return -1;
    }
    notifier->read_fd = descriptors[0];
    notifier->write_fd = descriptors[1];
    notifier->initialized = 1;
    return 0;
}

void
netins_udp_v2_notifier_destroy(netins_udp_v2_notifier_t *notifier)
{
    if (notifier == NULL || !notifier->initialized) {
        return;
    }
    if (notifier->read_fd >= 0) {
        close(notifier->read_fd);
    }
    if (notifier->write_fd >= 0) {
        close(notifier->write_fd);
    }
    notifier->read_fd = -1;
    notifier->write_fd = -1;
    notifier->initialized = 0;
    pthread_mutex_destroy(&notifier->lock);
}

int
netins_udp_v2_notifier_get_fd(netins_udp_v2_notifier_t *notifier)
{
    if (notifier == NULL || !notifier->initialized) {
        errno = EBADF;
        return -1;
    }
    return notifier->read_fd;
}

void
netins_udp_v2_notifier_signal(netins_udp_v2_notifier_t *notifier,
    uint32_t events)
{
    if (notifier == NULL || !notifier->initialized || events == 0) {
        return;
    }
    pthread_mutex_lock(&notifier->lock);
    notifier->latched_events |=
        events & NETINS_UDP_V2_NOTIFIER_TRANSITION_EVENTS;
    if ((events & NETINS_UDP_V2_NOTIFIER_LEVEL_EVENTS) != 0) {
        notifier->level_generation++;
    }
    netins_udp_v2_notifier_write_locked(notifier);
    pthread_mutex_unlock(&notifier->lock);
}

uint64_t
netins_udp_v2_notifier_begin_level_query(
    netins_udp_v2_notifier_t *notifier)
{
    if (notifier == NULL || !notifier->initialized) {
        return 0;
    }
    pthread_mutex_lock(&notifier->lock);
    uint64_t generation = notifier->level_generation;
    pthread_mutex_unlock(&notifier->lock);
    return generation;
}

int
netins_udp_v2_notifier_consume(netins_udp_v2_notifier_t *notifier,
    uint64_t level_generation, uint32_t level_events, uint32_t *out_events)
{
    if (notifier == NULL || !notifier->initialized || out_events == NULL) {
        errno = EINVAL;
        return -1;
    }
    pthread_mutex_lock(&notifier->lock);
    unsigned char buffer[64];
    for (;;) {
        ssize_t result = read(notifier->read_fd, buffer, sizeof(buffer));
        if (result > 0) {
            continue;
        }
        if (result < 0 && errno == EINTR) {
            continue;
        }
        break;
    }
    *out_events = notifier->latched_events | level_events;
    notifier->latched_events = 0;
    notifier->signaled = 0;
    /*
     * A level notification can arrive after the caller sampled backend
     * readiness but before this pipe is drained. Re-arm in that case so the
     * caller performs one fresh readiness query instead of losing the wakeup.
     */
    /*
     * Keep readable/acceptable conditions armed until the application drains
     * them. WRITABLE is still returned as a level condition, but is not
     * continuously rearmed: doing so makes a receive-only epoll loop spin
     * merely because the send queue has space. A later writable transition
     * still calls signal() and wakes the descriptor.
     */
    if ((level_events & NETINS_UDP_V2_NOTIFIER_REARM_EVENTS) != 0
        || notifier->level_generation != level_generation)
    {
        netins_udp_v2_notifier_write_locked(notifier);
    }
    pthread_mutex_unlock(&notifier->lock);
    return 0;
}
