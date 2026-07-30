#include "mini_backend.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "mini_client.h"

#if !defined(XQC_SYS_WINDOWS)
#include <unistd.h>
#endif

struct mini_backend_client_s {
    xqc_mini_cli_ctx_t ctx;
    xqc_mini_cli_args_t *args;
    xqc_mini_cli_user_conn_t *connection;
    int api_mode;
    int connect_started;
};

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

    client->args = args;
    client->api_mode = config != NULL && config->api_mode;
    xqc_mini_cli_init_args(args);
    if (config == NULL || config->load_config_file) {
        (void)xqc_mini_cli_load_config_file(args, MINI_CONFIG_FILE);
    }
    mini_backend_client_apply_config(args, config);

    if (xqc_mini_cli_init_ctx(&client->ctx, args) != XQC_OK) {
        xqc_mini_cli_free_ctx(&client->ctx);
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
    for (;;) {
        int reached = wait_for_close
            ? client->connection->connection_closed
            : client->connection->handshake_finished;
        if (reached) {
            return 0;
        }
        if (!wait_for_close && client->connection->connection_closed) {
            return ECONNRESET;
        }
        if (timeout_ms > 0 && xqc_now() - start >= timeout_us) {
            return ETIMEDOUT;
        }

        int ret = event_base_loop(client->ctx.eb, EVLOOP_NONBLOCK);
        if (ret < 0) {
            return EIO;
        }
#if !defined(XQC_SYS_WINDOWS)
        usleep(1000);
#endif
    }
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
    return event_base_loop(client->ctx.eb, EVLOOP_NONBLOCK) < 0 ? EIO : 0;
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
    free(client);
}

int
mini_backend_client_is_connected(const mini_backend_client_t *client)
{
    return client != NULL && client->connection != NULL
        && client->connection->handshake_finished
        && !client->connection->connection_closed;
}

int
mini_backend_client_is_closed(const mini_backend_client_t *client)
{
    return client == NULL || client->connection == NULL
        || client->connection->connection_closed;
}

int
mini_backend_client_last_error(const mini_backend_client_t *client)
{
    return client != NULL && client->connection != NULL
        ? client->connection->last_error_code : 0;
}

int
mini_backend_client_active_path_count(const mini_backend_client_t *client)
{
    return client != NULL && client->connection != NULL
        ? client->connection->active_path_cnt : 0;
}
