#define XQC_MINI_LOG_IMPLEMENTATION
#include "common.h"
#include "mini_backend.h"

#include <stdarg.h>
#include <string.h>

char method_s[][16] = {
    {"GET"}, 
    {"POST"}
};

static _Thread_local int xqc_mini_log_scope_active;
static _Thread_local xqc_mini_logger_fn xqc_mini_log_callback;
static _Thread_local void *xqc_mini_log_user_data;
static _Thread_local int xqc_mini_log_minimum_level;

void
xqc_mini_log_scope_enter(xqc_mini_logger_fn callback,
    void *user_data, int minimum_level)
{
    xqc_mini_log_scope_active = 1;
    xqc_mini_log_callback = callback;
    xqc_mini_log_user_data = user_data;
    xqc_mini_log_minimum_level = minimum_level;
}

void
xqc_mini_log_scope_leave(void)
{
    xqc_mini_log_scope_active = 0;
    xqc_mini_log_callback = NULL;
    xqc_mini_log_user_data = NULL;
    xqc_mini_log_minimum_level = 0;
}

int
xqc_mini_log_printf(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    if (!xqc_mini_log_scope_active) {
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
    if (xqc_mini_log_callback != NULL
        && level >= xqc_mini_log_minimum_level)
    {
        xqc_mini_log_callback(xqc_mini_log_user_data, level, message);
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
