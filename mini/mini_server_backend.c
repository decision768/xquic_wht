#include "mini_backend.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include <event2/thread.h>

#include "mini_server.h"
#include "mini_stream_backend.h"

#if !defined(XQC_SYS_WINDOWS)
#include <unistd.h>
#endif

typedef struct mini_backend_server_stream_node_s {
    mini_backend_stream_t *stream;
    unsigned int references;
    int removing;
    struct mini_backend_server_stream_node_s *next;
} mini_backend_server_stream_node_t;

struct mini_backend_server_s {
    xqc_mini_svr_ctx_t ctx;
    xqc_mini_svr_args_t *args;
    xqc_mini_svr_user_conn_t *listener;
    int api_mode;
    unsigned int references;
    int listen_started;
    pthread_mutex_t lock;
    pthread_cond_t state_cond;
    pthread_t io_worker;
    int io_worker_started;
    int io_stop_requested;
    int wake_pipe[2];
    struct event *wake_event;
    mini_backend_server_stream_node_t *stream_head;
};

static void
mini_backend_server_timeout_wakeup(evutil_socket_t fd, short events, void *arg)
{
    (void)fd;
    (void)events;
    (void)arg;
}

static int
mini_backend_server_set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        return -1;
    }
    flags = fcntl(fd, F_GETFD, 0);
    return flags < 0 || fcntl(fd, F_SETFD,
        flags | FD_CLOEXEC) != 0 ? -1 : 0;
}

static void
mini_backend_server_wake(mini_backend_server_t *server)
{
    unsigned char byte = 1;
    if (server == NULL || server->wake_pipe[1] < 0) {
        return;
    }
    ssize_t written = write(server->wake_pipe[1], &byte, 1);
    (void)written;
}

static void
mini_backend_server_io_wakeup(evutil_socket_t fd, short events, void *arg)
{
    mini_backend_server_t *server = arg;
    unsigned char buffer[128];
    (void)events;
    while (read((int)fd, buffer, sizeof(buffer)) > 0) {
    }

    for (;;) {
        xqc_cid_t cid;
        int found = 0;
        pthread_mutex_lock(&server->lock);
        if (server->listener != NULL) {
            for (xqc_mini_svr_user_conn_t *connection =
                    server->listener->connection_head;
                connection != NULL; connection = connection->all_next)
            {
                if (connection->api_close_requested
                    && !connection->api_close_submitted
                    && !connection->connection_closed)
                {
                    connection->api_close_submitted = 1;
                    memcpy(&cid, &connection->cid, sizeof(cid));
                    found = 1;
                    break;
                }
            }
        }
        int stop = server->io_stop_requested;
        pthread_mutex_unlock(&server->lock);
        if (!found) {
            if (stop && server->ctx.eb != NULL) {
                event_base_loopbreak(server->ctx.eb);
            }
            break;
        }
        if (server->ctx.engine != NULL) {
            (void)xqc_h3_conn_close(server->ctx.engine, &cid);
        }
    }
}

static void *
mini_backend_server_io_worker(void *arg)
{
    mini_backend_server_t *server = arg;
    for (;;) {
        int ret = event_base_loop(server->ctx.eb, EVLOOP_ONCE);
        pthread_mutex_lock(&server->lock);
        mini_backend_server_stream_node_t *node = server->stream_head;
        while (node != NULL) {
            if (node->removing) {
                node = node->next;
                continue;
            }
            node->references++;
            mini_backend_stream_t *stream = node->stream;
            pthread_mutex_unlock(&server->lock);
            (void)mini_stream_backend_drive(stream);
            pthread_mutex_lock(&server->lock);
            node->references--;
            pthread_cond_broadcast(&server->state_cond);
            node = node->next;
        }
        int stop = server->io_stop_requested;
        if (ret < 0 && server->listener != NULL
            && server->listener->last_error_code == 0)
        {
            server->listener->last_error_code = EIO;
        }
        pthread_cond_broadcast(&server->state_cond);
        pthread_mutex_unlock(&server->lock);
        if (stop || ret < 0) {
            break;
        }
    }
    return NULL;
}

static int
mini_backend_server_start_io_worker(mini_backend_server_t *server)
{
    if (!server->args->stream_api_enabled || server->io_worker_started) {
        return 0;
    }
    if (pipe(server->wake_pipe) != 0
        || mini_backend_server_set_nonblocking(server->wake_pipe[0]) != 0
        || mini_backend_server_set_nonblocking(server->wake_pipe[1]) != 0)
    {
        return errno != 0 ? errno : EIO;
    }
    server->wake_event = event_new(server->ctx.eb, server->wake_pipe[0],
        EV_READ | EV_PERSIST, mini_backend_server_io_wakeup, server);
    if (server->wake_event == NULL
        || event_add(server->wake_event, NULL) != 0)
    {
        return ENOMEM;
    }
    int ret = pthread_create(&server->io_worker, NULL,
        mini_backend_server_io_worker, server);
    if (ret != 0) {
        return ret;
    }
    server->io_worker_started = 1;
    return 0;
}

static int
mini_backend_server_arm_timeout(struct event_base *event_base,
    xqc_usec_t start, uint32_t timeout_ms, struct event **timeout_event)
{
    *timeout_event = NULL;
    if (timeout_ms == 0) {
        return 0;
    }

    xqc_usec_t timeout_us = (xqc_usec_t)timeout_ms * 1000;
    xqc_usec_t now = xqc_now();
    if (now - start >= timeout_us) {
        return ETIMEDOUT;
    }

    xqc_usec_t remaining_us = timeout_us - (now - start);
    struct timeval remaining = {
        .tv_sec = (long)(remaining_us / 1000000),
        .tv_usec = (long)(remaining_us % 1000000),
    };
    struct event *event = event_new(event_base, -1, EV_TIMEOUT,
        mini_backend_server_timeout_wakeup, NULL);
    if (event == NULL) {
        return ENOMEM;
    }
    if (event_add(event, &remaining) != 0) {
        event_free(event);
        return EIO;
    }
    *timeout_event = event;
    return 0;
}

static void
mini_backend_server_disarm_timeout(struct event **timeout_event)
{
    if (timeout_event != NULL && *timeout_event != NULL) {
        event_free(*timeout_event);
        *timeout_event = NULL;
    }
}

static void
mini_backend_server_apply_config(xqc_mini_svr_args_t *args,
    const mini_backend_config_t *config)
{
    if (config == NULL) {
        return;
    }
    if (config->api_mode) {
        args->env_cfg.use_zlog = 0;
        args->env_cfg.disable_file_logging = 1;
    }
    args->stream_api_enabled = config->stream_mode != 0;
    if (config->has_zlog) {
        args->env_cfg.use_zlog = config->zlog_enabled;
        args->env_cfg.zlog_minimum_level = config->zlog_minimum_level;
        args->env_cfg.zlog_level_filter_enabled = 1;
        if (config->zlog_enabled) {
            strncpy(args->env_cfg.zlog_conf, config->zlog_config_file,
                sizeof(args->env_cfg.zlog_conf) - 1);
            args->env_cfg.zlog_conf[sizeof(args->env_cfg.zlog_conf) - 1] =
                '\0';
            strncpy(args->env_cfg.zlog_category, config->zlog_category,
                sizeof(args->env_cfg.zlog_category) - 1);
            args->env_cfg.zlog_category[
                sizeof(args->env_cfg.zlog_category) - 1] = '\0';
        }
    }
    if (config->has_scheduler) {
        strncpy(args->quic_cfg.mp_sched, config->scheduler,
            sizeof(args->quic_cfg.mp_sched) - 1);
        args->quic_cfg.mp_sched[sizeof(args->quic_cfg.mp_sched) - 1] = '\0';
    }
    if (config->has_congestion_control) {
        args->quic_cfg.cc = config->congestion_control == MINI_BACKEND_CC_CUBIC
            ? CC_TYPE_CUBIC : CC_TYPE_BBR;
    }
    if (config->has_kernel_sndbuf) {
        args->net_cfg.kernel_sndbuf = config->kernel_sndbuf;
    }
    if (config->has_kernel_rcvbuf) {
        args->net_cfg.kernel_revbuf = config->kernel_rcvbuf;
    }
    if (config->has_tls) {
        if (config->certificate_file[0] != '\0') {
            strncpy(args->env_cfg.cert_file, config->certificate_file,
                sizeof(args->env_cfg.cert_file) - 1);
            args->env_cfg.cert_file[sizeof(args->env_cfg.cert_file) - 1] =
                '\0';
        }
        if (config->private_key_file[0] != '\0') {
            strncpy(args->env_cfg.private_key_file, config->private_key_file,
                sizeof(args->env_cfg.private_key_file) - 1);
            args->env_cfg.private_key_file[
                sizeof(args->env_cfg.private_key_file) - 1] = '\0';
        }
    }
}

int
mini_backend_server_create(const mini_backend_config_t *config,
    mini_backend_server_t **out_server)
{
    if (out_server == NULL) {
        return EINVAL;
    }
    *out_server = NULL;

    mini_backend_server_t *server = calloc(1, sizeof(*server));
    xqc_mini_svr_args_t *args = calloc(1, sizeof(*args));
    if (server == NULL || args == NULL) {
        free(args);
        free(server);
        return ENOMEM;
    }

    server->wake_pipe[0] = -1;
    server->wake_pipe[1] = -1;
    int sync_ret = pthread_mutex_init(&server->lock, NULL);
    if (sync_ret != 0) {
        free(args);
        free(server);
        return sync_ret;
    }
    sync_ret = pthread_cond_init(&server->state_cond, NULL);
    if (sync_ret != 0) {
        pthread_mutex_destroy(&server->lock);
        free(args);
        free(server);
        return sync_ret;
    }
    if (evthread_use_pthreads() != 0) {
        pthread_cond_destroy(&server->state_cond);
        pthread_mutex_destroy(&server->lock);
        free(args);
        free(server);
        return EIO;
    }

    server->args = args;
    server->api_mode = config != NULL && config->api_mode;
    server->references = 1;
    xqc_mini_svr_init_args(args);
    if (config == NULL || config->load_config_file) {
        (void)xqc_mini_svr_load_config_file(args, MINI_SERVER_CONFIG_FILE);
    }
    mini_backend_server_apply_config(args, config);
    if (server->api_mode) {
        printf("[resolved-event-loop] role=server mode=blocking_once"
               " fixed_sleep_us=0\n");
    }

    if (xqc_mini_svr_init_ctx(&server->ctx, args) != XQC_OK) {
        xqc_mini_svr_free_ctx(&server->ctx);
        pthread_cond_destroy(&server->state_cond);
        pthread_mutex_destroy(&server->lock);
        free(server);
        return EIO;
    }
    if (xqc_mini_svr_init_xquic_engine(&server->ctx, args) != XQC_OK
        || xqc_mini_svr_init_engine_ctx(&server->ctx, args) != XQC_OK)
    {
        if (server->ctx.engine != NULL) {
            xqc_engine_destroy(server->ctx.engine);
            server->ctx.engine = NULL;
        }
        xqc_mini_svr_free_ctx(&server->ctx);
        pthread_cond_destroy(&server->state_cond);
        pthread_mutex_destroy(&server->lock);
        free(server);
        return EIO;
    }

    *out_server = server;
    return 0;
}

int
mini_backend_server_listen(mini_backend_server_t *server,
    const char *host, uint16_t port)
{
    if (server == NULL || server->listen_started)
    {
        return EINVAL;
    }
    if (host != NULL && host[0] != '\0') {
        strncpy(server->args->net_cfg.ip, host,
            sizeof(server->args->net_cfg.ip) - 1);
        server->args->net_cfg.ip[sizeof(server->args->net_cfg.ip) - 1] = '\0';
    }
    if (port != 0) {
        server->args->net_cfg.port = port;
    }

    server->listener = xqc_mini_svr_create_user_conn(&server->ctx);
    if (server->listener == NULL) {
        return EADDRNOTAVAIL;
    }
    server->listener->api_listener_mode = server->api_mode;
    server->listener->api_backend = server;
    server->listener->accepting = 1;
    server->listen_started = 1;
    return mini_backend_server_start_io_worker(server);
}

int
mini_backend_server_run(mini_backend_server_t *server)
{
    if (server == NULL || server->ctx.eb == NULL
        || !server->listen_started)
    {
        return EINVAL;
    }
    if (server->io_worker_started) {
        return EBUSY;
    }
    return event_base_dispatch(server->ctx.eb) < 0 ? EIO : 0;
}

int
mini_backend_server_poll(mini_backend_server_t *server)
{
    if (server == NULL || server->ctx.eb == NULL) {
        return EINVAL;
    }
    if (server->io_worker_started) {
        return 0;
    }
    return event_base_loop(server->ctx.eb, EVLOOP_NONBLOCK) < 0 ? EIO : 0;
}

int
mini_backend_server_last_error(const mini_backend_server_t *server)
{
    return server != NULL && server->listener != NULL
        ? server->listener->last_error_code : 0;
}

static void
mini_backend_server_remove_connection(mini_backend_server_t *server,
    xqc_mini_svr_user_conn_t *connection)
{
    xqc_mini_svr_user_conn_t **cursor =
        &server->listener->connection_head;
    while (*cursor != NULL) {
        if (*cursor == connection) {
            *cursor = connection->all_next;
            connection->all_next = NULL;
            return;
        }
        cursor = &(*cursor)->all_next;
    }
}

static int
mini_backend_server_pump(mini_backend_server_t *server)
{
    return event_base_loop(server->ctx.eb, EVLOOP_ONCE) == 0 ? 0 : EIO;
}

int
mini_backend_server_accept(mini_backend_server_t *server,
    uint32_t timeout_ms, mini_backend_server_connection_t **out_connection)
{
    if (server == NULL || out_connection == NULL || server->listener == NULL
        || !server->listener->accepting)
    {
        return EINVAL;
    }
    *out_connection = NULL;
    xqc_usec_t start = xqc_now();
    xqc_usec_t timeout_us = (xqc_usec_t)timeout_ms * 1000;
    struct event *timeout_event = NULL;
    int result = 0;
    if (!server->io_worker_started) {
        result = mini_backend_server_arm_timeout(server->ctx.eb, start,
            timeout_ms, &timeout_event);
        if (result != 0) {
            return result;
        }
    }

    for (;;) {
        pthread_mutex_lock(&server->lock);
        xqc_mini_svr_user_conn_t *connection =
            server->listener->accept_head;
        if (connection != NULL) {
            server->listener->accept_head = connection->accept_next;
            if (server->listener->accept_head == NULL) {
                server->listener->accept_tail = NULL;
            }
            connection->accept_next = NULL;
            connection->queued = 0;
            if (connection->connection_closed) {
                mini_backend_server_remove_connection(server, connection);
                xqc_mini_svr_free_user_conn(connection);
                pthread_mutex_unlock(&server->lock);
                continue;
            }
            connection->delivered = 1;
            *out_connection = connection;
            result = 0;
            pthread_mutex_unlock(&server->lock);
            break;
        }
        if (!server->listener->accepting) {
            result = ECANCELED;
            pthread_mutex_unlock(&server->lock);
            break;
        }
        if (server->listener->last_error_code != 0) {
            result = server->listener->last_error_code;
            pthread_mutex_unlock(&server->lock);
            break;
        }
        if (timeout_ms > 0 && xqc_now() - start >= timeout_us) {
            result = ETIMEDOUT;
            pthread_mutex_unlock(&server->lock);
            break;
        }
        if (server->io_worker_started) {
            if (timeout_ms == 0) {
                result = pthread_cond_wait(&server->state_cond,
                    &server->lock);
            } else {
                uint64_t remaining_us = timeout_us - (xqc_now() - start);
                struct timespec deadline;
                if (clock_gettime(CLOCK_REALTIME, &deadline) != 0) {
                    result = errno != 0 ? errno : EIO;
                } else {
                    deadline.tv_sec += (time_t)(remaining_us / 1000000);
                    deadline.tv_nsec +=
                        (long)((remaining_us % 1000000) * 1000);
                    if (deadline.tv_nsec >= 1000000000L) {
                        deadline.tv_sec++;
                        deadline.tv_nsec -= 1000000000L;
                    }
                    result = pthread_cond_timedwait(&server->state_cond,
                        &server->lock, &deadline);
                }
            }
            pthread_mutex_unlock(&server->lock);
            if (result != 0 && result != EINTR) {
                break;
            }
            result = 0;
            continue;
        }
        pthread_mutex_unlock(&server->lock);
        int ret = mini_backend_server_pump(server);
        if (ret != 0) {
            result = ret;
            break;
        }
    }

    mini_backend_server_disarm_timeout(&timeout_event);
    return result;
}

void
mini_backend_server_retain(mini_backend_server_t *server)
{
    if (server != NULL) {
        pthread_mutex_lock(&server->lock);
        server->references++;
        pthread_mutex_unlock(&server->lock);
    }
}

void
mini_backend_server_shutdown(mini_backend_server_t *server)
{
    if (server == NULL || server->listener == NULL) {
        return;
    }
    pthread_mutex_lock(&server->lock);
    server->listener->accepting = 0;
    pthread_cond_broadcast(&server->state_cond);
    if (server->ctx.engine == NULL) {
        pthread_mutex_unlock(&server->lock);
        return;
    }

    if (server->io_worker_started) {
        for (xqc_mini_svr_user_conn_t *connection =
                server->listener->connection_head;
            connection != NULL; connection = connection->all_next)
        {
            if (!connection->delivered && !connection->connection_closed) {
                connection->api_close_requested = 1;
            }
        }
        pthread_mutex_unlock(&server->lock);
        mini_backend_server_wake(server);
        return;
    }
    pthread_mutex_unlock(&server->lock);

    /*
     * File mode has no background event worker, so the connection list cannot
     * change until one of these close calls enters xquic. Do not hold the
     * backend mutex across that call: a synchronous close callback may need
     * the same mutex to unlink the connection.
     */
    for (;;) {
        xqc_mini_svr_user_conn_t *connection =
            server->listener->connection_head;
        while (connection != NULL
            && (connection->delivered || connection->connection_closed
                || connection->api_close_submitted))
        {
            connection = connection->all_next;
        }
        if (connection == NULL) {
            break;
        }
        connection->api_close_submitted = 1;
        (void)xqc_h3_conn_close(server->ctx.engine, &connection->cid);
    }
}

int
mini_backend_server_connection_shutdown(mini_backend_server_t *server,
    mini_backend_server_connection_t *connection, uint32_t timeout_ms)
{
    if (server == NULL || connection == NULL) {
        return EINVAL;
    }
    if (connection->connection_closed) {
        return 0;
    }
    if (server->io_worker_started) {
        xqc_usec_t start = xqc_now();
        xqc_usec_t timeout_us = (xqc_usec_t)timeout_ms * 1000;
        pthread_mutex_lock(&server->lock);
        connection->api_close_requested = 1;
        mini_backend_server_wake(server);
        int result = 0;
        while (!connection->connection_closed && result == 0) {
            if (timeout_ms == 0) {
                result = pthread_cond_wait(&server->state_cond,
                    &server->lock);
                continue;
            }
            xqc_usec_t elapsed = xqc_now() - start;
            if (elapsed >= timeout_us) {
                result = ETIMEDOUT;
                break;
            }
            uint64_t remaining_us = timeout_us - elapsed;
            struct timespec deadline;
            if (clock_gettime(CLOCK_REALTIME, &deadline) != 0) {
                result = errno != 0 ? errno : EIO;
                break;
            }
            deadline.tv_sec += (time_t)(remaining_us / 1000000);
            deadline.tv_nsec += (long)((remaining_us % 1000000) * 1000);
            if (deadline.tv_nsec >= 1000000000L) {
                deadline.tv_sec++;
                deadline.tv_nsec -= 1000000000L;
            }
            result = pthread_cond_timedwait(&server->state_cond,
                &server->lock, &deadline);
        }
        pthread_mutex_unlock(&server->lock);
        return result;
    }
    int ret = xqc_h3_conn_close(server->ctx.engine, &connection->cid);
    if (ret != XQC_OK) {
        return EIO;
    }

    xqc_usec_t start = xqc_now();
    xqc_usec_t timeout_us = (xqc_usec_t)timeout_ms * 1000;
    struct event *timeout_event = NULL;
    ret = mini_backend_server_arm_timeout(server->ctx.eb, start,
        timeout_ms, &timeout_event);
    if (ret != 0) {
        return ret;
    }

    while (!connection->connection_closed) {
        if (timeout_ms > 0 && xqc_now() - start >= timeout_us) {
            ret = ETIMEDOUT;
            break;
        }
        ret = mini_backend_server_pump(server);
        if (ret != 0) {
            break;
        }
    }
    mini_backend_server_disarm_timeout(&timeout_event);
    return ret;
}

int
mini_backend_server_connection_serve(mini_backend_server_t *server,
    mini_backend_server_connection_t *connection, const char *root_path,
    uint32_t timeout_ms)
{
    if (server == NULL || connection == NULL || root_path == NULL
        || root_path[0] == '\0' || connection->connection_closed
        || strlen(root_path) >= sizeof(connection->serve_root_path))
    {
        return EINVAL;
    }
    if (server->io_worker_started) {
        return EOPNOTSUPP;
    }

#if defined(XQC_SYS_WINDOWS)
    return EOPNOTSUPP;
#else
    /*
     * Do not pass a PATH_LEN-sized buffer to realpath().  On glibc builds
     * with _FORTIFY_SOURCE enabled, realpath requires a PATH_MAX-sized
     * destination.  PATH_LEN is intentionally smaller than PATH_MAX on
     * Linux, so the fortified wrapper aborts the process before serving the
     * first request.  Passing NULL lets realpath allocate the required size.
     */
    char *canonical_root = realpath(root_path, NULL);
    if (canonical_root == NULL) {
        return errno != 0 ? errno : ENOENT;
    }
    size_t canonical_root_len = strlen(canonical_root);
    if (canonical_root_len >= sizeof(connection->serve_root_path)) {
        free(canonical_root);
        return ENAMETOOLONG;
    }
    int root_fd = open(canonical_root,
        O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (root_fd < 0) {
        int saved_errno = errno;
        free(canonical_root);
        return saved_errno != 0 ? saved_errno : ENOENT;
    }
    struct stat root_stat;
    if (fstat(root_fd, &root_stat) != 0) {
        int saved_errno = errno;
        close(root_fd);
        free(canonical_root);
        return saved_errno != 0 ? saved_errno : EIO;
    }
    if (!S_ISDIR(root_stat.st_mode)) {
        close(root_fd);
        free(canonical_root);
        return ENOTDIR;
    }
    if (connection->serve_root_fd >= 0) {
        close(connection->serve_root_fd);
    }
    connection->serve_root_fd = root_fd;
    memcpy(connection->serve_root_path, canonical_root,
        canonical_root_len + 1);
    free(canonical_root);
    connection->serve_cancelled = 0;
    connection->serve_configured = 1;
    int resume_ret = xqc_mini_svr_resume_pending_requests(connection);
    if (resume_ret < 0) {
        connection->last_error_code = EPROTO;
        connection->serve_cancelled = 1;
        connection->serve_configured = 0;
        (void)xqc_h3_conn_close(server->ctx.engine, &connection->cid);
        return EPROTO;
    }

    xqc_usec_t start = xqc_now();
    xqc_usec_t timeout_us = (xqc_usec_t)timeout_ms * 1000;
    struct event *timeout_event = NULL;
    int result = mini_backend_server_arm_timeout(server->ctx.eb, start,
        timeout_ms, &timeout_event);
    if (result != 0) {
        connection->serve_cancelled = 1;
        connection->serve_configured = 0;
        return result;
    }

    while (!connection->connection_closed) {
        if (timeout_ms > 0 && xqc_now() - start >= timeout_us) {
            connection->serve_cancelled = 1;
            connection->serve_configured = 0;
            result = ETIMEDOUT;
            break;
        }
        int ret = mini_backend_server_pump(server);
        if (ret != 0) {
            connection->serve_cancelled = 1;
            connection->serve_configured = 0;
            result = ret;
            break;
        }
    }
    mini_backend_server_disarm_timeout(&timeout_event);
    if (result != 0) {
        return result;
    }
    return connection->last_error_code != 0
        ? connection->last_error_code : 0;
#endif
}

void
mini_backend_server_connection_release(mini_backend_server_t *server,
    mini_backend_server_connection_t *connection)
{
    if (server == NULL || connection == NULL || server->listener == NULL) {
        return;
    }
    pthread_mutex_lock(&server->lock);
    if (connection->connection_closed) {
        mini_backend_server_remove_connection(server, connection);
        pthread_mutex_unlock(&server->lock);
        xqc_mini_svr_free_user_conn(connection);
        return;
    }

    /*
     * A timed-out shutdown can still have callbacks pending in xquic. Keep the
     * scope attached to the backend until engine destruction instead of
     * leaving callbacks with a dangling user-data pointer.
     */
    connection->delivered = 0;
    pthread_mutex_unlock(&server->lock);
}

static void
mini_backend_server_destroy(mini_backend_server_t *server)
{
    if (server == NULL) {
        return;
    }
    for (;;) {
        pthread_mutex_lock(&server->lock);
        mini_backend_stream_t *stream = server->stream_head != NULL
            ? server->stream_head->stream : NULL;
        pthread_mutex_unlock(&server->lock);
        if (stream == NULL) {
            break;
        }
        mini_backend_stream_destroy(stream);
    }
    mini_backend_server_shutdown(server);
    if (server->io_worker_started) {
        pthread_mutex_lock(&server->lock);
        server->io_stop_requested = 1;
        pthread_mutex_unlock(&server->lock);
        mini_backend_server_wake(server);
        (void)pthread_join(server->io_worker, NULL);
        server->io_worker_started = 0;
    }
    if (server->wake_event != NULL) {
        event_free(server->wake_event);
        server->wake_event = NULL;
    }
    if (server->wake_pipe[0] >= 0) {
        close(server->wake_pipe[0]);
    }
    if (server->wake_pipe[1] >= 0) {
        close(server->wake_pipe[1]);
    }
    if (server->listener != NULL) {
        xqc_mini_svr_on_connection_finish(server->listener);
    }
    if (server->ctx.engine != NULL) {
        xqc_engine_destroy(server->ctx.engine);
        server->ctx.engine = NULL;
    }
    if (server->listener != NULL) {
        xqc_mini_svr_user_conn_t *connection =
            server->listener->connection_head;
        while (connection != NULL) {
            xqc_mini_svr_user_conn_t *next = connection->all_next;
            xqc_mini_svr_free_user_conn(connection);
            connection = next;
        }
        server->listener->connection_head = NULL;
        xqc_mini_svr_free_user_conn(server->listener);
        server->listener = NULL;
    }
    xqc_mini_svr_free_ctx(&server->ctx);
    server->args = NULL;
    pthread_cond_destroy(&server->state_cond);
    pthread_mutex_destroy(&server->lock);
    free(server);
}

static void
mini_backend_server_stream_detach(void *owner,
    mini_backend_stream_t *stream)
{
    mini_backend_server_t *server = owner;
    if (server == NULL) {
        return;
    }
    pthread_mutex_lock(&server->lock);
    mini_backend_server_stream_node_t **cursor = &server->stream_head;
    while (*cursor != NULL) {
        if ((*cursor)->stream == stream) {
            mini_backend_server_stream_node_t *node = *cursor;
            node->removing = 1;
            while (node->references != 0) {
                (void)pthread_cond_wait(&server->state_cond,
                    &server->lock);
            }
            *cursor = node->next;
            free(node);
            break;
        }
        cursor = &(*cursor)->next;
    }
    pthread_mutex_unlock(&server->lock);
}

void
mini_backend_server_on_connection_created(void *backend,
    mini_backend_server_connection_t *connection)
{
    mini_backend_server_t *server = backend;
    if (server == NULL || connection == NULL || server->listener == NULL) {
        return;
    }
    pthread_mutex_lock(&server->lock);
    connection->api_backend = server;
    connection->all_next = server->listener->connection_head;
    server->listener->connection_head = connection;
    pthread_cond_broadcast(&server->state_cond);
    pthread_mutex_unlock(&server->lock);
}

void
mini_backend_server_on_handshake_finished(void *backend,
    mini_backend_server_connection_t *connection)
{
    mini_backend_server_t *server = backend;
    if (server == NULL || connection == NULL || server->listener == NULL) {
        return;
    }
    pthread_mutex_lock(&server->lock);
    connection->handshake_finished = 1;
    if (!connection->queued && !connection->delivered
        && !connection->connection_closed && server->listener->accepting)
    {
        connection->queued = 1;
        server->listener->accept_yield_requested = 1;
        connection->accept_next = NULL;
        if (server->listener->accept_tail == NULL) {
            server->listener->accept_head = connection;
        } else {
            server->listener->accept_tail->accept_next = connection;
        }
        server->listener->accept_tail = connection;
    }
    pthread_cond_broadcast(&server->state_cond);
    pthread_mutex_unlock(&server->lock);
}

void
mini_backend_server_on_connection_closed(void *backend,
    mini_backend_server_connection_t *connection)
{
    mini_backend_server_t *server = backend;
    if (server == NULL || connection == NULL || server->listener == NULL) {
        return;
    }
    int release = 0;
    pthread_mutex_lock(&server->lock);
    connection->connection_closed = 1;
    connection->serve_cancelled = 1;
    connection->serve_configured = 0;
    if (!connection->queued && !connection->delivered) {
        mini_backend_server_remove_connection(server, connection);
        release = 1;
    }
    pthread_cond_broadcast(&server->state_cond);
    pthread_mutex_unlock(&server->lock);
    if (release) {
        xqc_mini_svr_free_user_conn(connection);
    }
}

int
mini_backend_server_stream_accept(mini_backend_server_t *server,
    mini_backend_server_connection_t *connection,
    const mini_backend_stream_config_t *config,
    mini_backend_stream_t **out_stream)
{
    if (server == NULL || connection == NULL || config == NULL
        || out_stream == NULL || !server->api_mode
        || !server->args->stream_api_enabled
        || !connection->handshake_finished || connection->connection_closed
        || connection->stream_api_state != NULL
        || !server->io_worker_started)
    {
        return EINVAL;
    }
    mini_backend_server_stream_node_t *node = calloc(1, sizeof(*node));
    if (node == NULL) {
        return ENOMEM;
    }
    mini_backend_stream_config_t stream_config = *config;
    stream_config.external_event_driver = 1;
    int ret = mini_stream_backend_create(server->ctx.engine,
        &connection->cid, server->ctx.eb, MINI_STREAM_ENDPOINT_ACCEPT,
        &stream_config, &connection->stream_api_state, server,
        mini_backend_server_stream_detach, out_stream);
    if (ret == 0) {
        node->stream = *out_stream;
        pthread_mutex_lock(&server->lock);
        node->next = server->stream_head;
        server->stream_head = node;
        pthread_mutex_unlock(&server->lock);
        mini_backend_server_wake(server);
    } else {
        free(node);
    }
    return ret;
}

void
mini_backend_server_release(mini_backend_server_t *server)
{
    if (server == NULL) {
        return;
    }
    int destroy = 0;
    pthread_mutex_lock(&server->lock);
    server->references--;
    destroy = server->references == 0;
    pthread_mutex_unlock(&server->lock);
    if (destroy) {
        mini_backend_server_destroy(server);
    }
}

int
mini_backend_server_connection_is_connected(
    const mini_backend_server_connection_t *connection)
{
    return connection != NULL && connection->handshake_finished
        && !connection->connection_closed;
}

int
mini_backend_server_connection_is_closed(
    const mini_backend_server_connection_t *connection)
{
    return connection == NULL || connection->connection_closed;
}

int
mini_backend_server_connection_last_error(
    const mini_backend_server_connection_t *connection)
{
    return connection != NULL ? connection->last_error_code : 0;
}

static void
mini_backend_server_format_address(const struct sockaddr *address,
    socklen_t address_len, char *output, size_t output_len)
{
    if (output == NULL || output_len == 0) {
        return;
    }
    output[0] = '\0';
    if (address == NULL || address_len == 0) {
        return;
    }

    char host[NI_MAXHOST] = {0};
    char service[16] = {0};
    if (getnameinfo(address, address_len, host, sizeof(host),
            service, sizeof(service), NI_NUMERICHOST | NI_NUMERICSERV) != 0)
    {
        return;
    }
    if (address->sa_family == AF_INET6) {
        snprintf(output, output_len, "[%s]:%s", host, service);
    } else {
        snprintf(output, output_len, "%s:%s", host, service);
    }
}

int
mini_backend_server_connection_get_snapshot(
    const mini_backend_server_t *server,
    const mini_backend_server_connection_t *connection,
    mini_backend_snapshot_t *snapshot)
{
    if (server == NULL || connection == NULL || snapshot == NULL
        || server->ctx.engine == NULL)
    {
        return EINVAL;
    }

    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->snapshot_time_us = xqc_now();
    snapshot->handshake_finished = connection->handshake_finished;
    xqc_conn_stats_t stats = xqc_conn_get_stats(server->ctx.engine,
        &connection->cid);
    snapshot->multipath_mode = stats.enable_multipath;
    snapshot->multipath_state = stats.mp_state;
    snapshot->packets_sent = stats.send_count;
    snapshot->packets_received = stats.recv_count;
    snapshot->packets_lost = stats.lost_count;
    snapshot->spurious_packets_lost = stats.spurious_loss_count;
    snapshot->srtt_us = stats.srtt;
    snapshot->min_rtt_us = stats.min_rtt;
    snapshot->bytes_inflight = stats.inflight_bytes;
    snapshot->total_app_bytes = stats.total_app_bytes;
    snapshot->standby_path_app_bytes = stats.standby_path_app_bytes;

    for (size_t i = 0; i < XQC_MAX_PATHS_COUNT
        && snapshot->path_count < MINI_BACKEND_MAX_PATHS; i++)
    {
        const xqc_path_metrics_t *source = &stats.paths_info[i];
        if (source->path_id == UINT64_MAX) {
            continue;
        }
        mini_backend_path_snapshot_t *path =
            &snapshot->paths[snapshot->path_count];
        path->path_id = source->path_id;
        path->path_index = snapshot->path_count;
        path->path_state = source->path_state;
        path->app_status = source->path_app_status;
        path->packets_sent = source->path_pkt_send_count;
        path->packets_received = source->path_pkt_recv_count;
        path->bytes_sent = source->path_send_bytes;
        path->bytes_received = source->path_recv_bytes;
        path->reinjected_bytes_sent =
            source->path_send_reinject_bytes;
        path->reinjected_bytes_received =
            source->path_recv_reinject_bytes;
        path->effective_bytes_received =
            source->path_recv_effective_bytes;
        path->srtt_us = source->path_srtt;
        mini_backend_server_format_address(
            (const struct sockaddr *)connection->local_addr,
            connection->local_addrlen, path->local_address,
            sizeof(path->local_address));
        mini_backend_server_format_address(
            (const struct sockaddr *)connection->peer_addr,
            connection->peer_addrlen, path->peer_address,
            sizeof(path->peer_address));
        if (path->path_state == 2) {
            snapshot->active_path_count++;
        }
        snapshot->path_count++;
    }
    return 0;
}
