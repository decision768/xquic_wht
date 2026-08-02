/**
 * @copyright Copyright (c) 2022, Alibaba Group Holding Limited
 */

#ifndef TEST_COMMON_H
#define TEST_COMMON_H


#include <memory.h>
#include <stdio.h>
#include <xquic/xquic_typedef.h>
#include <zlog.h>

/* definition for connection */
#define DEFAULT_SERVER_ADDR "127.0.0.1"
#define DEFAULT_SERVER_PORT 8443

#define CIPHER_SUIT_LEN     256
#define TLS_GROUPS_LEN      64

#define PATH_LEN            1024
#define RESOURCE_LEN        1024
#define AUTHORITY_LEN       128
#define URL_LEN             1024

/* the congestion control types */
typedef enum cc_type_s {
    CC_TYPE_BBR,
    CC_TYPE_CUBIC,
    CC_TYPE_RENO,
    CC_TYPE_COPA
} CC_TYPE;



/* request method */
typedef enum request_method_e {
    REQUEST_METHOD_GET,
    REQUEST_METHOD_POST,
} REQUEST_METHOD;

extern char method_s[][16];


static size_t READ_FILE_BUF_LEN = 2 *1024 * 1024;

#define DEBUG ;
// #define DEBUG printf("%s:%d (%s)\n",__FILE__, __LINE__ ,__FUNCTION__);


#define RSP_HDR_BUF_LEN 32
typedef enum h3_hdr_type {
    /* rsp */
    H3_HDR_STATUS,
    H3_HDR_CONTENT_TYPE,
    H3_HDR_CONTENT_LENGTH,
    H3_HDR_METHOD,
    H3_HDR_SCHEME,
    H3_HDR_HOST,
    H3_HDR_PATH,
    H3_HDR_STREAM_INDEX,
    H3_HDR_STREAM_COUNT,
    H3_HDR_STREAM_OFFSET,
    H3_HDR_TOTAL_LENGTH,

    H3_HDR_CNT
} H3_HDR_TYPE;


extern long xqc_random(void);
extern xqc_usec_t xqc_now();


int xqc_mini_read_file_data(char * data, size_t data_len, char *filename);

typedef void (*xqc_mini_logger_fn)(void *user_data, int level,
    const char *message);

/*
 * API calls install a thread-local logging scope while they drive the
 * original synchronous mini callbacks. CLI calls have no scope and retain
 * their existing stdout behavior.
 */
void xqc_mini_log_scope_enter(xqc_mini_logger_fn callback,
    void *user_data, int minimum_level);
void xqc_mini_log_scope_leave(void);
int xqc_mini_log_printf(const char *format, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;

/*
 * zlog uses process-wide initialization. These helpers serialize ownership
 * and keep it alive until every Mini client/server context has released it.
 */
int xqc_mini_zlog_acquire(const char *config_file, const char *category,
    zlog_category_t **out_category);
void xqc_mini_zlog_release(void);
int xqc_mini_log_level_from_xqc(xqc_log_level_t level);
xqc_log_level_t xqc_mini_xqc_log_level_for_minimum(int minimum_level);

#if !defined(XQC_MINI_LOG_IMPLEMENTATION)
#define printf(...) xqc_mini_log_printf(__VA_ARGS__)
#endif

#endif
