#define XQC_MINI_LOG_IMPLEMENTATION
#include "common.h"
#include "mini_backend.h"

#include <errno.h>
#include <pthread.h>
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

static pthread_mutex_t xqc_mini_zlog_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned int xqc_mini_zlog_references;
static char xqc_mini_zlog_config_file[PATH_LEN];

int
xqc_mini_zlog_acquire(const char *config_file, const char *category,
    zlog_category_t **out_category)
{
    if (config_file == NULL || config_file[0] == '\0'
        || category == NULL || category[0] == '\0'
        || out_category == NULL)
    {
        return EINVAL;
    }
    *out_category = NULL;

    int lock_error = pthread_mutex_lock(&xqc_mini_zlog_lock);
    if (lock_error != 0) {
        return lock_error;
    }

    int result = 0;
    int initialized_here = 0;
    if (xqc_mini_zlog_references == 0) {
        if (zlog_init(config_file) != 0) {
            result = EIO;
            goto done;
        }
        initialized_here = 1;
        snprintf(xqc_mini_zlog_config_file,
            sizeof(xqc_mini_zlog_config_file), "%s", config_file);
    } else if (strcmp(xqc_mini_zlog_config_file, config_file) != 0) {
        result = EBUSY;
        goto done;
    }

    zlog_category_t *resolved = zlog_get_category(category);
    if (resolved == NULL) {
        if (initialized_here) {
            zlog_fini();
            xqc_mini_zlog_config_file[0] = '\0';
        }
        result = ENOENT;
        goto done;
    }

    xqc_mini_zlog_references++;
    *out_category = resolved;

done:
    (void)pthread_mutex_unlock(&xqc_mini_zlog_lock);
    return result;
}

void
xqc_mini_zlog_release(void)
{
    if (pthread_mutex_lock(&xqc_mini_zlog_lock) != 0) {
        return;
    }
    if (xqc_mini_zlog_references > 0) {
        xqc_mini_zlog_references--;
        if (xqc_mini_zlog_references == 0) {
            zlog_fini();
            xqc_mini_zlog_config_file[0] = '\0';
        }
    }
    (void)pthread_mutex_unlock(&xqc_mini_zlog_lock);
}

int
xqc_mini_log_level_from_xqc(xqc_log_level_t level)
{
    switch (level) {
    case XQC_LOG_DEBUG:
        return 0;
    case XQC_LOG_WARN:
        return 2;
    case XQC_LOG_ERROR:
    case XQC_LOG_FATAL:
        return 3;
    case XQC_LOG_REPORT:
    case XQC_LOG_STATS:
    case XQC_LOG_INFO:
    default:
        return 1;
    }
}

xqc_log_level_t
xqc_mini_xqc_log_level_for_minimum(int minimum_level)
{
    switch (minimum_level) {
    case 3:
        return XQC_LOG_ERROR;
    case 2:
        return XQC_LOG_WARN;
    case 1:
        return XQC_LOG_INFO;
    case 0:
    default:
        return XQC_LOG_DEBUG;
    }
}

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
