#define XQC_MINI_LOG_IMPLEMENTATION
#include "common.h"
#include "mini_backend.h"

#include <stdarg.h>
#include <string.h>

char method_s[][16] = {
    {"GET"}, 
    {"POST"}
};

#define XQC_MINI_LOG_SCOPE_MAX_DEPTH 32

typedef struct xqc_mini_log_scope_s {
    xqc_mini_logger_fn callback;
    void *user_data;
    int minimum_level;
} xqc_mini_log_scope_t;

static _Thread_local xqc_mini_log_scope_t
    xqc_mini_log_scopes[XQC_MINI_LOG_SCOPE_MAX_DEPTH];
static _Thread_local unsigned int xqc_mini_log_scope_depth;
static _Thread_local unsigned int xqc_mini_log_scope_overflow;

void
xqc_mini_log_scope_enter(xqc_mini_logger_fn callback,
    void *user_data, int minimum_level)
{
    if (xqc_mini_log_scope_depth >= XQC_MINI_LOG_SCOPE_MAX_DEPTH) {
        xqc_mini_log_scope_overflow++;
        return;
    }
    unsigned int index = xqc_mini_log_scope_depth++;
    xqc_mini_log_scopes[index].callback = callback;
    xqc_mini_log_scopes[index].user_data = user_data;
    xqc_mini_log_scopes[index].minimum_level = minimum_level;
}

void
xqc_mini_log_scope_leave(void)
{
    if (xqc_mini_log_scope_overflow > 0) {
        xqc_mini_log_scope_overflow--;
        return;
    }
    if (xqc_mini_log_scope_depth == 0) {
        return;
    }
    xqc_mini_log_scope_depth--;
    memset(&xqc_mini_log_scopes[xqc_mini_log_scope_depth], 0,
        sizeof(xqc_mini_log_scopes[xqc_mini_log_scope_depth]));
}

int
xqc_mini_log_printf(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    if (xqc_mini_log_scope_depth == 0) {
        int ret = vprintf(format, args);
        va_end(args);
        return ret;
    }

    char message[2048];
    int ret = vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    int level = 1;
    if (strncmp(message, "[error]", 7) == 0) {
        level = 3;
    } else if (strncmp(message, "[warn]", 6) == 0) {
        level = 2;
    } else if (strncmp(message, "[diag", 5) == 0) {
        level = 0;
    }
    const xqc_mini_log_scope_t *scope =
        &xqc_mini_log_scopes[xqc_mini_log_scope_depth - 1];
    if (scope->callback != NULL && level >= scope->minimum_level)
    {
        scope->callback(scope->user_data, level, message);
    }
    return ret;
}

void
mini_backend_config_init(mini_backend_config_t *config)
{
    if (config == NULL) {
        return;
    }
    memset(config, 0, sizeof(*config));
    config->load_config_file = 1;
}

void
mini_backend_log_scope_enter(const mini_backend_config_t *config)
{
    xqc_mini_log_scope_enter(
        config != NULL ? config->logger : NULL,
        config != NULL ? config->logger_user_data : NULL,
        config != NULL ? config->logger_minimum_level : 0);
}

void
mini_backend_log_scope_leave(void)
{
    xqc_mini_log_scope_leave();
}

int
xqc_mini_read_file_data(char * data, size_t data_len, char *filename)
{
    int ret = 0;
    size_t total_len, read_len;
    FILE *fp = fopen(filename, "rb");
    if (fp == NULL) {
        ret = -1;
        goto end;
    }

    fseek(fp, 0 , SEEK_END);
    total_len = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (total_len > data_len) {
        ret = -1;
        goto end;
    }

    read_len = fread(data, 1, total_len, fp);
    if (read_len != total_len) {
        ret = -1;
        goto end;
    }

    ret = read_len;

end:
    if (fp) {
        fclose(fp);
    }
    return ret;
}
