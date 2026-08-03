/**
 * @copyright Copyright (c) 2022, Alibaba Group Holding Limited
 */

#ifndef XQC_MINI_PLATFORM_H
#define XQC_MINI_PLATFORM_H

#if defined(_WIN64) || defined(WIN64) || defined(_WIN32) || defined(WIN32)
#define XQC_SYS_WINDOWS
#endif

#ifdef XQC_SYS_WINDOWS
# define EAGAIN WSAEWOULDBLOCK
# define EINTR WSAEINTR
#endif

static inline int
get_sys_errno(void)
{
#ifdef XQC_SYS_WINDOWS
    return WSAGetLastError();
#else
    return errno;
#endif
}

static inline void
set_sys_errno(int err)
{
#ifdef XQC_SYS_WINDOWS
    WSASetLastError(err);
#else
    errno = err;
#endif
}

static inline void
xqc_platform_init_env(void)
{
#ifdef XQC_SYS_WINDOWS
    WSADATA wsa_data;
    int result = WSAStartup(MAKEWORD(2, 2), &wsa_data);
    if (result != 0) {
        printf("WSAStartup failed with error %d\n", result);
        exit(1);
    }
#endif
}

#endif
