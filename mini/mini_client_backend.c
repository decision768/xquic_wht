#include "mini_backend.h"

#include <errno.h>
#include <netdb.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "mini_client.h"
#include "mini_stream_backend.h"

#if MINI_BACKEND_MAX_INTERFACES > MAX_PATH_CNT
#error "Mini backend interface capacity exceeds Mini client path slots"
#endif

#if !defined(XQC_SYS_WINDOWS)
#include <unistd.h>
#endif

struct mini_backend_client_s {
    xqc_mini_cli_ctx_t ctx;
    xqc_mini_cli_args_t *args;
    xqc_mini_cli_user_conn_t *connection;
    int api_mode;
    int connect_started;
    mini_backend_stream_t *active_stream;
    pthread_mutex_t engine_lock;
};

static void
mini_backend_client_timeout_wakeup(evutil_socket_t fd, short events, void *arg)
{
    (void)fd;
    (void)events;
    (void)arg;
}

static int
mini_backend_client_arm_timeout(struct event_base *event_base,
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
        mini_backend_client_timeout_wakeup, NULL);
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
mini_backend_client_disarm_timeout(struct event **timeout_event)
{
    if (timeout_event != NULL && *timeout_event != NULL) {
        event_free(*timeout_event);
        *timeout_event = NULL;
    }
}

static void
mini_backend_client_abort_transfer(mini_backend_client_t *client,
    int error_code)
{
    if (client == NULL || client->connection == NULL) {
        return;
    }
    xqc_mini_cli_set_transfer_error(client->connection,
        error_code != 0 ? error_code : EIO);
    client->connection->transfer_active = 0;
    client->connection->transfer_finished = 1;
    client->connection->auto_start_requests = 0;
    client->connection->transfer_end_time = xqc_now();
    if (!client->connection->connection_closed && client->ctx.engine != NULL) {
        (void)xqc_h3_conn_close(client->ctx.engine,
            &client->connection->cid);
    }
    xqc_mini_cli_stop_async_writer(client->connection);
    if (client->connection->download_fd >= 0) {
        close(client->connection->download_fd);
        client->connection->download_fd = -1;
    }
}

static void
mini_backend_client_apply_config(xqc_mini_cli_args_t *args,
    const mini_backend_config_t *config)
{
    if (config == NULL) {
        return;
    }

    if (config->api_mode) {
        args->env_cfg.use_zlog = 0;
        args->env_cfg.disable_file_logging = 1;
        args->defer_transfer_setup = 1;
        args->disable_0rtt_storage = 1;
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
    if (config->has_interfaces) {
        args->net_cfg.multi_interface_cnt = 0;
        memset(args->net_cfg.multi_interface, 0,
            sizeof(args->net_cfg.multi_interface));
        int count = config->interface_count;
        if (count > MAX_PATH_CNT) {
            count = MAX_PATH_CNT;
        }
        for (int i = 0; i < count; i++) {
            strncpy(args->net_cfg.multi_interface[i], config->interfaces[i],
                sizeof(args->net_cfg.multi_interface[i]) - 1);
            if (args->net_cfg.multi_interface[i][0] != '\0') {
                args->net_cfg.multi_interface_cnt++;
            }
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
    if (config->has_tls && config->tls_server_name[0] != '\0') {
        strncpy(args->req_cfg.host, config->tls_server_name,
            sizeof(args->req_cfg.host) - 1);
        args->req_cfg.host[sizeof(args->req_cfg.host) - 1] = '\0';
    }
}

int
mini_backend_client_create(const mini_backend_config_t *config,
    mini_backend_client_t **out_client)
{
    if (out_client == NULL) {
        return EINVAL;
    }
    *out_client = NULL;

    mini_backend_client_t *client = calloc(1, sizeof(*client));
    xqc_mini_cli_args_t *args = calloc(1, sizeof(*args));
    if (client == NULL || args == NULL) {
        free(args);
        free(client);
        return ENOMEM;
    }
    int sync_ret = pthread_mutex_init(&client->engine_lock, NULL);
    if (sync_ret != 0) {
        free(args);
        free(client);
        return sync_ret;
    }

    client->args = args;
    client->api_mode = config != NULL && config->api_mode;
    xqc_mini_cli_init_args(args);
    if (config == NULL || config->load_config_file) {
        (void)xqc_mini_cli_load_config_file(args, MINI_CONFIG_FILE);
    }
    mini_backend_client_apply_config(args, config);
    if (client->api_mode) {
        printf("[resolved-event-loop] role=client mode=blocking_once"
               " fixed_sleep_us=0\n");
        int configured_interfaces = args->net_cfg.multi_interface_cnt;
        int target_paths = configured_interfaces > 0
            ? configured_interfaces : XQC_MINI_DEFAULT_PATH_CNT;
        if (target_paths > MAX_PATH_CNT) {
            target_paths = MAX_PATH_CNT;
        }
        printf("[resolved-interface-config] configured_interfaces=%d"
               " target_paths=%d multipath=%d default_auto_dual=%d\n",
               configured_interfaces, target_paths,
               args->quic_cfg.multipath,
               configured_interfaces == 0 && target_paths >= 2);
    }

    if (xqc_mini_cli_init_ctx(&client->ctx, args) != XQC_OK) {
        xqc_mini_cli_free_ctx(&client->ctx);
        pthread_mutex_destroy(&client->engine_lock);
        free(client);
        return EIO;
    }
    if (xqc_mini_cli_init_xquic_engine(&client->ctx, args) != XQC_OK
        || xqc_mini_cli_init_engine_ctx(&client->ctx) != XQC_OK)
    {
        if (client->ctx.engine != NULL) {
            xqc_engine_destroy(client->ctx.engine);
            client->ctx.engine = NULL;
        }
        xqc_mini_cli_free_ctx(&client->ctx);
        pthread_mutex_destroy(&client->engine_lock);
        free(client);
        return EIO;
    }

    *out_client = client;
    return 0;
}

static int
mini_backend_client_pump_until(mini_backend_client_t *client,
    int wait_for_close, uint32_t timeout_ms)
{
    xqc_usec_t start = xqc_now();
    xqc_usec_t timeout_us = (xqc_usec_t)timeout_ms * 1000;
    struct event *timeout_event = NULL;
    int result = mini_backend_client_arm_timeout(client->ctx.eb, start,
        timeout_ms, &timeout_event);
    if (result != 0) {
        return result;
    }

    for (;;) {
        int reached = wait_for_close
            ? client->connection->connection_closed
            : client->connection->handshake_finished;
        if (reached) {
            result = 0;
            break;
        }
        if (!wait_for_close && client->connection->connection_closed) {
            result = ECONNRESET;
            break;
        }
        if (timeout_ms > 0 && xqc_now() - start >= timeout_us) {
            result = ETIMEDOUT;
            break;
        }

        int ret = event_base_loop(client->ctx.eb, EVLOOP_ONCE);
        if (ret != 0) {
            result = EIO;
            break;
        }
    }

    mini_backend_client_disarm_timeout(&timeout_event);
    return result;
}

int
mini_backend_client_connect(mini_backend_client_t *client,
    const char *host, uint16_t port, uint32_t timeout_ms)
{
    if (client == NULL || client->connection != NULL
        || client->connect_started)
    {
        return EINVAL;
    }

    if (host != NULL && host[0] != '\0') {
        strncpy(client->args->net_cfg.server_addr, host,
            sizeof(client->args->net_cfg.server_addr) - 1);
        client->args->net_cfg.server_addr[
            sizeof(client->args->net_cfg.server_addr) - 1] = '\0';
        if (client->args->req_cfg.host[0] == '\0'
            || strcmp(client->args->req_cfg.host, DEFAULT_HOST) == 0)
        {
            strncpy(client->args->req_cfg.host, host,
                sizeof(client->args->req_cfg.host) - 1);
            client->args->req_cfg.host[
                sizeof(client->args->req_cfg.host) - 1] = '\0';
        }
    }
    if (port != 0) {
        client->args->net_cfg.server_port = port;
    }

    client->connection = xqc_mini_cli_user_conn_create(&client->ctx);
    if (client->connection == NULL) {
        return EIO;
    }
    client->connection->auto_start_requests = client->api_mode ? 0 : 1;
    if (xqc_mini_cli_main_process(client->connection, &client->ctx)
        != XQC_OK)
    {
        return ECONNREFUSED;
    }
    client->connect_started = 1;
    if (!client->api_mode) {
        return 0;
    }
    return mini_backend_client_pump_until(client, 0, timeout_ms);
}

int
mini_backend_client_run(mini_backend_client_t *client)
{
    if (client == NULL || client->ctx.eb == NULL) {
        return EINVAL;
    }
    return event_base_dispatch(client->ctx.eb) < 0 ? EIO : 0;
}

int
mini_backend_client_poll(mini_backend_client_t *client)
{
    if (client == NULL || client->ctx.eb == NULL) {
        return EINVAL;
    }
    if (client->active_stream != NULL) {
        return 0;
    }
    pthread_mutex_lock(&client->engine_lock);
    int ret = event_base_loop(client->ctx.eb, EVLOOP_NONBLOCK) < 0 ? EIO : 0;
    pthread_mutex_unlock(&client->engine_lock);
    return ret;
}

int
mini_backend_client_transfer(mini_backend_client_t *client,
    int direction, const char *local_path, const char *remote_path,
    uint32_t timeout_ms, mini_backend_transfer_result_t *result)
{
    if (client == NULL || client->connection == NULL || result == NULL
        || !client->api_mode || !client->connect_started
        || !client->connection->handshake_finished
        || client->connection->connection_closed)
    {
        return EINVAL;
    }
    if (direction != MINI_BACKEND_TRANSFER_GET
        && direction != MINI_BACKEND_TRANSFER_POST)
    {
        return EINVAL;
    }

    memset(result, 0, sizeof(*result));
    REQUEST_METHOD method = direction == MINI_BACKEND_TRANSFER_GET
        ? REQUEST_METHOD_GET : REQUEST_METHOD_POST;
    int concurrency = direction == MINI_BACKEND_TRANSFER_GET
        ? XQC_MINI_DEFAULT_GET_CONCURRENCY
        : XQC_MINI_DEFAULT_POST_CONCURRENCY;
    if (xqc_mini_cli_prepare_transfer(client->connection, method,
            local_path, remote_path, concurrency) != XQC_OK)
    {
        return client->connection->transfer_error_code != 0
            ? client->connection->transfer_error_code : EIO;
    }

    int ret = XQC_OK;
    if (method == REQUEST_METHOD_GET
        && !xqc_mini_cli_paths_ready_for_requests(client->connection))
    {
        xqc_mini_cli_schedule_path_retry(client->connection,
            XQC_MINI_PATH_RETRY_INTERVAL_USEC);
    } else {
        ret = xqc_mini_cli_launch_requests(client->connection);
    }
    if (ret != XQC_OK) {
        mini_backend_client_abort_transfer(client, EIO);
        return EIO;
    }

    xqc_usec_t start = client->connection->transfer_start_time;
    xqc_usec_t timeout_us = (xqc_usec_t)timeout_ms * 1000;
    struct event *timeout_event = NULL;
    int timeout_ret = mini_backend_client_arm_timeout(client->ctx.eb, start,
        timeout_ms, &timeout_event);
    if (timeout_ret != 0) {
        ret = timeout_ret;
        mini_backend_client_abort_transfer(client, ret);
    }

    while (ret == XQC_OK && !client->connection->transfer_finished) {
        if (client->connection->transfer_error_code != 0) {
            ret = client->connection->transfer_error_code;
            mini_backend_client_abort_transfer(client, ret);
            break;
        }
        if (client->connection->connection_closed) {
            ret = client->connection->last_error_code != 0
                ? client->connection->last_error_code : ECONNRESET;
            xqc_mini_cli_set_transfer_error(client->connection, ret);
            break;
        }
        if (timeout_ms > 0 && xqc_now() - start >= timeout_us) {
            ret = ETIMEDOUT;
            mini_backend_client_abort_transfer(client, ret);
            break;
        }
        int loop_ret = event_base_loop(client->ctx.eb, EVLOOP_ONCE);
        if (loop_ret != 0) {
            ret = EIO;
            mini_backend_client_abort_transfer(client, ret);
            break;
        }
    }
    mini_backend_client_disarm_timeout(&timeout_event);

    if (client->connection->transfer_end_time == 0) {
        client->connection->transfer_end_time = xqc_now();
    }
    result->total_bytes = method == REQUEST_METHOD_GET
        ? client->connection->download_total_bytes
        : client->connection->upload_total_bytes;
    result->total_fragments =
        (uint32_t)client->connection->target_requests;
    result->completed_fragments =
        (uint32_t)client->connection->completed_requests;
    /*
     * Keep API completion semantics (wait for the whole task above), but
     * report throughput with the same timing boundaries as the original
     * Mini client:
     *   GET  - first response-body byte to last response-body byte;
     *   POST - first body submission to completion of all body submissions.
     */
    if (method == REQUEST_METHOD_GET) {
        result->elapsed_us =
            client->connection->download_first_body_time > 0
            && client->connection->download_last_body_time
                > client->connection->download_first_body_time
            ? client->connection->download_last_body_time
                - client->connection->download_first_body_time
            : 0;
    } else {
        result->elapsed_us = client->connection->upload_start_time > 0
            && client->connection->upload_end_time
                >= client->connection->upload_start_time
            ? client->connection->upload_end_time
                - client->connection->upload_start_time
            : 0;
    }

    if (ret == XQC_OK && client->connection->transfer_error_code != 0) {
        ret = client->connection->transfer_error_code;
    }
    if (ret != XQC_OK) {
        mini_backend_client_abort_transfer(client, ret);
    }
    return ret == XQC_OK ? 0 : ret;
}

int
mini_backend_client_shutdown(mini_backend_client_t *client,
    uint32_t timeout_ms)
{
    if (client == NULL) {
        return EINVAL;
    }
    if (client->connection == NULL || !client->connect_started
        || client->connection->connection_closed)
    {
        return 0;
    }

    int ret = xqc_h3_conn_close(client->ctx.engine,
        &client->connection->cid);
    if (ret != XQC_OK) {
        return EIO;
    }
    return mini_backend_client_pump_until(client, 1, timeout_ms);
}

void
mini_backend_client_destroy(mini_backend_client_t *client)
{
    if (client == NULL) {
        return;
    }
    if (client->active_stream != NULL) {
        mini_backend_stream_destroy(client->active_stream);
    }
    if (client->connection != NULL) {
        (void)mini_backend_client_shutdown(client, 1000);
        xqc_mini_cli_on_connection_finish(client->connection);
    }
    if (client->ctx.engine != NULL) {
        xqc_engine_destroy(client->ctx.engine);
        client->ctx.engine = NULL;
    }
    if (client->connection != NULL) {
        xqc_mini_cli_free_user_conn(client->connection);
        client->connection = NULL;
    }
    xqc_mini_cli_free_ctx(&client->ctx);
    client->args = NULL;
    pthread_mutex_destroy(&client->engine_lock);
    free(client);
}

static void
mini_backend_client_stream_detach(void *owner,
    mini_backend_stream_t *stream)
{
    mini_backend_client_t *client = owner;
    if (client != NULL && client->active_stream == stream) {
        client->active_stream = NULL;
    }
}

static void
mini_backend_client_wake_stream(mini_backend_client_t *client)
{
    if (client != NULL && client->active_stream != NULL) {
        mini_stream_backend_request_wakeup(client->active_stream);
    }
}

int
mini_backend_client_stream_open(mini_backend_client_t *client,
    const mini_backend_stream_config_t *config,
    mini_backend_stream_t **out_stream)
{
    if (client == NULL || config == NULL || out_stream == NULL
        || !client->api_mode || !client->args->stream_api_enabled
        || !client->connect_started || client->connection == NULL
        || !client->connection->handshake_finished
        || client->connection->connection_closed
        || client->active_stream != NULL
        || client->connection->transfer_active)
    {
        return EINVAL;
    }
    int ret = mini_stream_backend_create(client->ctx.engine,
        &client->connection->cid, client->ctx.eb,
        MINI_STREAM_ENDPOINT_OPEN, config,
        &client->connection->stream_api_state, &client->engine_lock, client,
        mini_backend_client_stream_detach, out_stream);
    if (ret == 0) {
        client->active_stream = *out_stream;
    }
    return ret;
}

int
mini_backend_client_is_connected(mini_backend_client_t *client)
{
    if (client == NULL) {
        return 0;
    }
    mini_backend_client_wake_stream(client);
    pthread_mutex_lock(&client->engine_lock);
    int connected = client->connection != NULL
        && client->connection->handshake_finished
        && !client->connection->connection_closed;
    pthread_mutex_unlock(&client->engine_lock);
    return connected;
}

int
mini_backend_client_is_closed(mini_backend_client_t *client)
{
    if (client == NULL) {
        return 1;
    }
    mini_backend_client_wake_stream(client);
    pthread_mutex_lock(&client->engine_lock);
    int closed = client->connection == NULL
        || client->connection->connection_closed;
    pthread_mutex_unlock(&client->engine_lock);
    return closed;
}

int
mini_backend_client_last_error(mini_backend_client_t *client)
{
    if (client == NULL) {
        return 0;
    }
    mini_backend_client_wake_stream(client);
    pthread_mutex_lock(&client->engine_lock);
    int error_code = client->connection != NULL
        ? client->connection->last_error_code : 0;
    pthread_mutex_unlock(&client->engine_lock);
    return error_code;
}

int
mini_backend_client_active_path_count(mini_backend_client_t *client)
{
    if (client == NULL) {
        return 0;
    }
    mini_backend_client_wake_stream(client);
    pthread_mutex_lock(&client->engine_lock);
    int path_count = client->connection != NULL
        ? client->connection->active_path_cnt : 0;
    pthread_mutex_unlock(&client->engine_lock);
    return path_count;
}

static void
mini_backend_client_format_address(const struct sockaddr *address,
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
mini_backend_client_get_snapshot(mini_backend_client_t *client,
    mini_backend_snapshot_t *snapshot)
{
    if (client == NULL || snapshot == NULL || client->connection == NULL
        || client->ctx.engine == NULL)
    {
        return EINVAL;
    }

    mini_backend_client_wake_stream(client);
    pthread_mutex_lock(&client->engine_lock);
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->snapshot_time_us = xqc_now();
    snapshot->handshake_finished =
        client->connection->handshake_finished;
    xqc_conn_stats_t stats = xqc_conn_get_stats(client->ctx.engine,
        &client->connection->cid);
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

        for (int slot = 0; slot < MAX_PATH_CNT; slot++) {
            const xqc_mini_cli_user_path_t *user_path =
                &client->connection->paths[slot];
            if (user_path->path_id != source->path_id) {
                continue;
            }
            strncpy(path->interface_name, user_path->interface_name,
                sizeof(path->interface_name) - 1);
            mini_backend_client_format_address(user_path->local_addr,
                user_path->local_addrlen, path->local_address,
                sizeof(path->local_address));
            mini_backend_client_format_address(user_path->peer_addr,
                user_path->peer_addrlen, path->peer_address,
                sizeof(path->peer_address));
            break;
        }
        if (path->path_state == XQC_MINI_TRANSPORT_PATH_STATE_ACTIVE) {
            snapshot->active_path_count++;
        }
        snapshot->path_count++;
    }
    pthread_mutex_unlock(&client->engine_lock);
    return 0;
}
