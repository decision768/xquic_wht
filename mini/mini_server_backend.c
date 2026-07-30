#include "mini_backend.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "mini_server.h"

#if !defined(XQC_SYS_WINDOWS)
#include <unistd.h>
#endif

struct mini_backend_server_s {
    xqc_mini_svr_ctx_t ctx;
    xqc_mini_svr_args_t *args;
    xqc_mini_svr_user_conn_t *listener;
    int api_mode;
    unsigned int references;
    int listen_started;
};

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

    server->args = args;
    server->api_mode = config != NULL && config->api_mode;
    server->references = 1;
    xqc_mini_svr_init_args(args);
    if (config == NULL || config->load_config_file) {
        (void)xqc_mini_svr_load_config_file(args, MINI_SERVER_CONFIG_FILE);
    }
    mini_backend_server_apply_config(args, config);

    if (xqc_mini_svr_init_ctx(&server->ctx, args) != XQC_OK) {
        xqc_mini_svr_free_ctx(&server->ctx);
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
    server->listener->accepting = 1;
    server->listen_started = 1;
    return 0;
}

int
mini_backend_server_run(mini_backend_server_t *server)
{
    if (server == NULL || server->ctx.eb == NULL
        || !server->listen_started)
    {
        return EINVAL;
    }
    return event_base_dispatch(server->ctx.eb) < 0 ? EIO : 0;
}

int
mini_backend_server_poll(mini_backend_server_t *server)
{
    if (server == NULL || server->ctx.eb == NULL) {
        return EINVAL;
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
    int ret = event_base_loop(server->ctx.eb, EVLOOP_NONBLOCK);
    if (ret < 0) {
        return EIO;
    }
#if !defined(XQC_SYS_WINDOWS)
    usleep(1000);
#endif
    return 0;
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

    for (;;) {
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
                continue;
            }
            connection->delivered = 1;
            *out_connection = connection;
            return 0;
        }
        if (!server->listener->accepting) {
            return ECANCELED;
        }
        if (server->listener->last_error_code != 0) {
            return server->listener->last_error_code;
        }
        if (timeout_ms > 0 && xqc_now() - start >= timeout_us) {
            return ETIMEDOUT;
        }
        int ret = mini_backend_server_pump(server);
        if (ret != 0) {
            return ret;
        }
    }
}

void
mini_backend_server_retain(mini_backend_server_t *server)
{
    if (server != NULL) {
        server->references++;
    }
}

void
mini_backend_server_shutdown(mini_backend_server_t *server)
{
    if (server == NULL || server->listener == NULL) {
        return;
    }
    server->listener->accepting = 0;
    if (server->ctx.engine == NULL) {
        return;
    }

    for (xqc_mini_svr_user_conn_t *connection =
            server->listener->connection_head;
        connection != NULL; connection = connection->all_next)
    {
        if (!connection->delivered && !connection->connection_closed) {
            (void)xqc_h3_conn_close(server->ctx.engine, &connection->cid);
        }
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
    int ret = xqc_h3_conn_close(server->ctx.engine, &connection->cid);
    if (ret != XQC_OK) {
        return EIO;
    }

    xqc_usec_t start = xqc_now();
    xqc_usec_t timeout_us = (xqc_usec_t)timeout_ms * 1000;
    while (!connection->connection_closed) {
        if (timeout_ms > 0 && xqc_now() - start >= timeout_us) {
            return ETIMEDOUT;
        }
        ret = mini_backend_server_pump(server);
        if (ret != 0) {
            return ret;
        }
    }
    return 0;
}

void
mini_backend_server_connection_release(mini_backend_server_t *server,
    mini_backend_server_connection_t *connection)
{
    if (server == NULL || connection == NULL || server->listener == NULL) {
        return;
    }
    if (connection->connection_closed) {
        mini_backend_server_remove_connection(server, connection);
        xqc_mini_svr_free_user_conn(connection);
        return;
    }

    /*
     * A timed-out shutdown can still have callbacks pending in xquic. Keep the
     * scope attached to the backend until engine destruction instead of
     * leaving callbacks with a dangling user-data pointer.
     */
    connection->delivered = 0;
}

static void
mini_backend_server_destroy(mini_backend_server_t *server)
{
    if (server == NULL) {
        return;
    }
    mini_backend_server_shutdown(server);
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
    free(server);
}

void
mini_backend_server_release(mini_backend_server_t *server)
{
    if (server == NULL) {
        return;
    }
    server->references--;
    if (server->references == 0) {
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
