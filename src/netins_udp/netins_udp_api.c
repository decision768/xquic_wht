#include "netins_udp_internal.h"
#include "netins_udp_builtin_tls.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif

#ifndef MFD_ALLOW_SEALING
#define MFD_ALLOW_SEALING 0x0002U
#endif

#if !defined(SYS_memfd_create) && defined(__NR_memfd_create)
#define SYS_memfd_create __NR_memfd_create
#endif

static pthread_mutex_t netins_udp_global_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned int netins_udp_init_references;
static unsigned int netins_udp_handle_count;

static int netins_udp_valid(const netins_udp_t *handle);
static void netins_udp_copy_string(char *destination,
    size_t destination_size, const char *source);

static int
netins_udp_create_memfd(const char *name)
{
#if defined(SYS_memfd_create)
    int fd;
    do {
        fd = (int)syscall(SYS_memfd_create, name,
            MFD_CLOEXEC | MFD_ALLOW_SEALING);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0 && errno == EINVAL) {
        do {
            fd = (int)syscall(SYS_memfd_create, name, MFD_CLOEXEC);
        } while (fd < 0 && errno == EINTR);
    }
    return fd;
#else
    (void)name;
    errno = ENOSYS;
    return -1;
#endif
}

static int
netins_udp_write_all(int fd, const unsigned char *data, size_t length)
{
    size_t written = 0;
    while (written < length) {
        ssize_t result = write(fd, data + written, length - written);
        if (result > 0) {
            written += (size_t)result;
            continue;
        }
        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result == 0) {
            errno = EIO;
        }
        return -1;
    }
    return 0;
}

static int
netins_udp_create_pem_fd(const char *name, const void *pem, size_t pem_len,
    char *path, size_t path_len, int *out_fd)
{
    if (name == NULL || pem == NULL || pem_len == 0
        || pem_len > NETINS_UDP_TLS_PEM_MAX_LEN || path == NULL
        || path_len == 0 || out_fd == NULL)
    {
        errno = EINVAL;
        return -1;
    }

    int fd = netins_udp_create_memfd(name);
    if (fd < 0) {
        return -1;
    }

    int error_code = 0;
    if (fchmod(fd, S_IRUSR | S_IWUSR) != 0
        || netins_udp_write_all(fd, pem, pem_len) != 0
        || lseek(fd, 0, SEEK_SET) < 0)
    {
        error_code = errno;
        goto fail;
    }

#if defined(F_ADD_SEALS) && defined(F_SEAL_SEAL) \
    && defined(F_SEAL_SHRINK) && defined(F_SEAL_GROW) && defined(F_SEAL_WRITE)
    (void)fcntl(fd, F_ADD_SEALS,
        F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE);
#endif

    int result = snprintf(path, path_len, "/proc/self/fd/%d", fd);
    if (result < 0 || (size_t)result >= path_len) {
        error_code = ENAMETOOLONG;
        goto fail;
    }
    if (access(path, R_OK) != 0) {
        error_code = errno;
        goto fail;
    }

    *out_fd = fd;
    return 0;

fail:
    (void)close(fd);
    errno = error_code != 0 ? error_code : EIO;
    return -1;
}

static void
netins_udp_close_tls_fds(netins_udp_t *handle)
{
    if (handle == NULL || !handle->owns_tls_fds) {
        return;
    }
    if (handle->tls_certificate_fd >= 0) {
        (void)close(handle->tls_certificate_fd);
    }
    if (handle->tls_private_key_fd >= 0) {
        (void)close(handle->tls_private_key_fd);
    }
    handle->tls_certificate_fd = -1;
    handle->tls_private_key_fd = -1;
    handle->owns_tls_fds = 0;
}

static int
netins_udp_install_memory_tls(netins_udp_t *handle,
    const void *certificate_pem, size_t certificate_pem_len,
    const void *private_key_pem, size_t private_key_pem_len, int source)
{
    char certificate_path[MINI_BACKEND_FILE_PATH_LEN];
    char private_key_path[MINI_BACKEND_FILE_PATH_LEN];
    int certificate_fd = -1;
    int private_key_fd = -1;

    if (netins_udp_create_pem_fd("netins-udp-certificate",
            certificate_pem, certificate_pem_len, certificate_path,
            sizeof(certificate_path), &certificate_fd) != 0)
    {
        return -1;
    }
    if (netins_udp_create_pem_fd("netins-udp-private-key",
            private_key_pem, private_key_pem_len, private_key_path,
            sizeof(private_key_path), &private_key_fd) != 0)
    {
        int error_code = errno;
        (void)close(certificate_fd);
        errno = error_code;
        return -1;
    }

    netins_udp_close_tls_fds(handle);
    handle->tls_certificate_fd = certificate_fd;
    handle->tls_private_key_fd = private_key_fd;
    handle->owns_tls_fds = 1;
    handle->tls_source = source;
    handle->backend_config.has_tls = 1;
    netins_udp_copy_string(handle->backend_config.certificate_file,
        sizeof(handle->backend_config.certificate_file), certificate_path);
    netins_udp_copy_string(handle->backend_config.private_key_file,
        sizeof(handle->backend_config.private_key_file), private_key_path);
    return 0;
}

static void
netins_udp_copy_string(char *destination, size_t destination_size,
    const char *source)
{
    if (destination == NULL || destination_size == 0) {
        return;
    }
    if (source == NULL) {
        destination[0] = '\0';
        return;
    }

    size_t length = 0;
    while (length + 1 < destination_size && source[length] != '\0') {
        length++;
    }
    if (length > 0) {
        memcpy(destination, source, length);
    }
    destination[length] = '\0';
}

static void
netins_udp_log(netins_udp_t *handle, int level, const char *format, ...)
{
    if (!netins_udp_valid(handle) || handle->logger == NULL
        || level < handle->logger_minimum_level || format == NULL)
    {
        return;
    }
    char message[NETINS_UDP_ERROR_MSG_LEN];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    handle->logger(handle->logger_user_data, level, message);
}

static void
netins_udp_set_close_reason(netins_udp_t *handle, int source,
    int system_error, int transport_error, const char *message)
{
    if (!netins_udp_valid(handle)) {
        return;
    }
    handle->close_source = source;
    handle->close_system_error = system_error;
    handle->close_transport_error = transport_error;
    snprintf(handle->close_message, sizeof(handle->close_message), "%s",
        message != NULL ? message : "");
}

static int
netins_udp_valid(const netins_udp_t *handle)
{
    return handle != NULL && handle->magic == NETINS_UDP_HANDLE_MAGIC;
}

static int
netins_udp_set_error(netins_udp_t *handle, int error_code,
    const char *format, ...)
{
    if (error_code == 0) {
        error_code = EIO;
    }
    if (netins_udp_valid(handle)) {
        handle->last_error_code = error_code;
        va_list args;
        va_start(args, format);
        vsnprintf(handle->last_error_msg, sizeof(handle->last_error_msg),
            format != NULL ? format : "NetIns UDP error", args);
        va_end(args);
        netins_udp_log(handle, NETINS_UDP_LOG_ERROR, "%s",
            handle->last_error_msg);
    }
    errno = error_code;
    return -1;
}

static void
netins_udp_clear_error(netins_udp_t *handle)
{
    handle->last_error_code = 0;
    handle->last_error_msg[0] = '\0';
}

static const char *
netins_udp_scheduler_name(int scheduler)
{
    switch (scheduler) {
    case NETINS_UDP_SCHEDULER_BALANCED:
        return "balanced";
    case NETINS_UDP_SCHEDULER_MINRTT:
        return "minrtt";
    case NETINS_UDP_SCHEDULER_BACKUP:
        return "backup";
    case NETINS_UDP_SCHEDULER_RAP:
        return "rap";
    case NETINS_UDP_SCHEDULER_ACT:
        return "act";
    case NETINS_UDP_SCHEDULER_BANDWIDTH:
        return "bw";
    case NETINS_UDP_SCHEDULER_FASTPROBE:
        return "fastprobe";
    case NETINS_UDP_SCHEDULER_ADAPTIVE:
        return "adaptive";
    case NETINS_UDP_SCHEDULER_FASTPROBE5:
        return "fastprobe5";
    case NETINS_UDP_SCHEDULER_FASTPROBE10:
        return "fastprobe10";
    case NETINS_UDP_SCHEDULER_FASTPROBE20:
        return "fastprobe20";
    case NETINS_UDP_SCHEDULER_FASTONLY:
        return "fastonly";
    default:
        return NULL;
    }
}

uint32_t
netins_udp_get_api_version(void)
{
    return NETINS_UDP_API_VERSION;
}

int
netins_udp_init(void)
{
    int error_code = 0;
    pthread_mutex_lock(&netins_udp_global_lock);
    if (netins_udp_init_references == UINT_MAX) {
        error_code = EOVERFLOW;
    } else {
        netins_udp_init_references++;
    }
    pthread_mutex_unlock(&netins_udp_global_lock);
    if (error_code != 0) {
        errno = error_code;
        return -1;
    }
    return NETINS_UDP_OK;
}

int
netins_udp_cleanup(void)
{
    int error_code = 0;
    pthread_mutex_lock(&netins_udp_global_lock);
    if (netins_udp_init_references == 0) {
        error_code = EINVAL;
    } else if (netins_udp_init_references == 1
        && netins_udp_handle_count != 0)
    {
        error_code = EBUSY;
    } else {
        netins_udp_init_references--;
    }
    pthread_mutex_unlock(&netins_udp_global_lock);
    if (error_code != 0) {
        errno = error_code;
        return -1;
    }
    return 0;
}

netins_udp_t *
netins_udp_new(int role)
{
    if (role != NETINS_UDP_ROLE_CLIENT && role != NETINS_UDP_ROLE_SERVER) {
        errno = EINVAL;
        return NULL;
    }

    netins_udp_t *handle = calloc(1, sizeof(*handle));
    if (handle == NULL) {
        errno = ENOMEM;
        return NULL;
    }

    pthread_mutex_lock(&netins_udp_global_lock);
    if (netins_udp_init_references == 0) {
        pthread_mutex_unlock(&netins_udp_global_lock);
        free(handle);
        errno = EPERM;
        return NULL;
    }
    if (netins_udp_handle_count == UINT_MAX) {
        pthread_mutex_unlock(&netins_udp_global_lock);
        free(handle);
        errno = EOVERFLOW;
        return NULL;
    }
    netins_udp_handle_count++;
    pthread_mutex_unlock(&netins_udp_global_lock);

    handle->magic = NETINS_UDP_HANDLE_MAGIC;
    handle->role = role;
    handle->state = NETINS_UDP_STATE_NEW;
    handle->connect_timeout_ms = 10000;
    handle->accept_timeout_ms = 0;
    handle->close_timeout_ms = 3000;
    handle->logger_minimum_level = NETINS_UDP_LOG_DEBUG;
    handle->tls_certificate_fd = -1;
    handle->tls_private_key_fd = -1;
    mini_backend_config_init(&handle->backend_config);
    handle->backend_config.load_config_file = 0;
    handle->backend_config.api_mode = 1;

    return handle;
}

static int
netins_udp_set_u32_option(netins_udp_t *handle, const void *value,
    size_t value_len, uint32_t *destination)
{
    if (value == NULL || value_len != sizeof(uint32_t)) {
        return netins_udp_set_error(handle, EINVAL,
            "option requires a uint32_t value");
    }
    *destination = *(const uint32_t *)value;
    return 0;
}

int
netins_udp_set_opt(netins_udp_t *handle, int option,
    const void *value, size_t value_len)
{
    if (!netins_udp_valid(handle)) {
        errno = EINVAL;
        return -1;
    }
    if (handle->state != NETINS_UDP_STATE_NEW
        && option != NETINS_UDP_OPT_LOGGER)
    {
        return netins_udp_set_error(handle, EBUSY,
            "options must be set before connect or listen");
    }

    switch (option) {
    case NETINS_UDP_OPT_INTERFACES: {
        if (handle->role != NETINS_UDP_ROLE_CLIENT) {
            return netins_udp_set_error(handle, EOPNOTSUPP,
                "interfaces are only configurable on clients");
        }
        if (value == NULL || value_len < sizeof(netins_udp_interfaces_t)) {
            return netins_udp_set_error(handle, EINVAL,
                "invalid interfaces option size");
        }
        const netins_udp_interfaces_t *interfaces = value;
        if (interfaces->struct_size < sizeof(*interfaces)
            || interfaces->api_version != NETINS_UDP_API_VERSION
            || interfaces->interface_count == 0
            || interfaces->interface_count > NETINS_UDP_MAX_INTERFACES)
        {
            return netins_udp_set_error(handle, EINVAL,
                "invalid interface list");
        }
        for (uint32_t i = 0; i < interfaces->interface_count; i++) {
            if (interfaces->names[i][0] == '\0'
                || memchr(interfaces->names[i], '\0',
                    sizeof(interfaces->names[i])) == NULL)
            {
                return netins_udp_set_error(handle, EINVAL,
                    "interface name must be nonempty and terminated");
            }
        }
        handle->backend_config.has_interfaces = 1;
        handle->backend_config.interface_count =
            (int)interfaces->interface_count;
        memset(handle->backend_config.interfaces, 0,
            sizeof(handle->backend_config.interfaces));
        for (uint32_t i = 0; i < interfaces->interface_count; i++) {
            netins_udp_copy_string(handle->backend_config.interfaces[i],
                sizeof(handle->backend_config.interfaces[i]),
                interfaces->names[i]);
        }
        break;
    }
    case NETINS_UDP_OPT_SCHEDULER: {
        if (value == NULL || value_len != sizeof(int)) {
            return netins_udp_set_error(handle, EINVAL,
                "scheduler option requires an int");
        }
        int scheduler = *(const int *)value;
        if (scheduler == NETINS_UDP_SCHEDULER_DEFAULT) {
            handle->backend_config.has_scheduler = 0;
            handle->backend_config.scheduler[0] = '\0';
            break;
        }
        const char *name = netins_udp_scheduler_name(scheduler);
        if (name == NULL) {
            return netins_udp_set_error(handle, EINVAL,
                "unsupported scheduler");
        }
        handle->backend_config.has_scheduler = 1;
        netins_udp_copy_string(handle->backend_config.scheduler,
            sizeof(handle->backend_config.scheduler), name);
        break;
    }
    case NETINS_UDP_OPT_CONGESTION_CONTROL: {
        if (value == NULL || value_len != sizeof(int)) {
            return netins_udp_set_error(handle, EINVAL,
                "congestion control option requires an int");
        }
        int cc = *(const int *)value;
        if (cc != NETINS_UDP_CC_DEFAULT && cc != NETINS_UDP_CC_BBR
            && cc != NETINS_UDP_CC_CUBIC)
        {
            return netins_udp_set_error(handle, EINVAL,
                "unsupported congestion control");
        }
        handle->backend_config.has_congestion_control =
            cc != NETINS_UDP_CC_DEFAULT;
        handle->backend_config.congestion_control =
            cc == NETINS_UDP_CC_CUBIC
            ? MINI_BACKEND_CC_CUBIC : MINI_BACKEND_CC_BBR;
        break;
    }
    case NETINS_UDP_OPT_TLS: {
        if (value == NULL || value_len < sizeof(netins_udp_tls_config_t)) {
            return netins_udp_set_error(handle, EINVAL,
                "invalid TLS option size");
        }
        const netins_udp_tls_config_t *tls = value;
        if (tls->struct_size < sizeof(*tls)
            || tls->api_version != NETINS_UDP_API_VERSION)
        {
            return netins_udp_set_error(handle, EINVAL,
                "invalid TLS option version");
        }
        if (memchr(tls->server_name, '\0', sizeof(tls->server_name)) == NULL
            || memchr(tls->certificate_file, '\0',
                sizeof(tls->certificate_file)) == NULL
            || memchr(tls->private_key_file, '\0',
                sizeof(tls->private_key_file)) == NULL)
        {
            return netins_udp_set_error(handle, EINVAL,
                "TLS strings must be terminated");
        }
        if (handle->role == NETINS_UDP_ROLE_SERVER
            && (tls->certificate_file[0] == '\0'
                || tls->private_key_file[0] == '\0'))
        {
            return netins_udp_set_error(handle, EINVAL,
                "server TLS certificate and private key are required");
        }
        if (handle->role == NETINS_UDP_ROLE_SERVER
            && handle->tls_source != NETINS_UDP_TLS_SOURCE_NONE
            && handle->tls_source != NETINS_UDP_TLS_SOURCE_FILE)
        {
            return netins_udp_set_error(handle, EALREADY,
                "server TLS memory credentials are already configured");
        }
        if (handle->role == NETINS_UDP_ROLE_CLIENT
            && (tls->certificate_file[0] != '\0'
                || tls->private_key_file[0] != '\0'))
        {
            return netins_udp_set_error(handle, EOPNOTSUPP,
                "client certificates are not supported");
        }
        handle->backend_config.has_tls = 1;
        netins_udp_copy_string(handle->backend_config.tls_server_name,
            sizeof(handle->backend_config.tls_server_name), tls->server_name);
        netins_udp_copy_string(handle->backend_config.certificate_file,
            sizeof(handle->backend_config.certificate_file),
            tls->certificate_file);
        netins_udp_copy_string(handle->backend_config.private_key_file,
            sizeof(handle->backend_config.private_key_file),
            tls->private_key_file);
        if (handle->role == NETINS_UDP_ROLE_SERVER) {
            handle->tls_source = NETINS_UDP_TLS_SOURCE_FILE;
        }
        break;
    }
    case NETINS_UDP_OPT_TLS_MEMORY: {
        if (handle->role != NETINS_UDP_ROLE_SERVER) {
            return netins_udp_set_error(handle, EOPNOTSUPP,
                "TLS memory credentials are only configurable on servers");
        }
        if (value == NULL
            || value_len < sizeof(netins_udp_tls_memory_config_t))
        {
            return netins_udp_set_error(handle, EINVAL,
                "invalid TLS memory option size");
        }
        const netins_udp_tls_memory_config_t *tls_memory = value;
        if (tls_memory->struct_size < sizeof(*tls_memory)
            || tls_memory->api_version != NETINS_UDP_API_VERSION)
        {
            return netins_udp_set_error(handle, EINVAL,
                "invalid TLS memory option version");
        }
        if (tls_memory->certificate_pem == NULL
            || tls_memory->certificate_pem_len == 0
            || tls_memory->certificate_pem_len > NETINS_UDP_TLS_PEM_MAX_LEN
            || tls_memory->private_key_pem == NULL
            || tls_memory->private_key_pem_len == 0
            || tls_memory->private_key_pem_len > NETINS_UDP_TLS_PEM_MAX_LEN)
        {
            return netins_udp_set_error(handle, EINVAL,
                "invalid TLS memory certificate or private key buffer");
        }
        if (handle->tls_source != NETINS_UDP_TLS_SOURCE_NONE
            && handle->tls_source != NETINS_UDP_TLS_SOURCE_MEMORY)
        {
            return netins_udp_set_error(handle, EALREADY,
                "server TLS file credentials are already configured");
        }
        if (netins_udp_install_memory_tls(handle,
                tls_memory->certificate_pem,
                tls_memory->certificate_pem_len,
                tls_memory->private_key_pem,
                tls_memory->private_key_pem_len,
                NETINS_UDP_TLS_SOURCE_MEMORY) != 0)
        {
            int error_code = errno;
            return netins_udp_set_error(handle,
                error_code != 0 ? error_code : EIO,
                "install TLS memory credentials failed");
        }
        break;
    }
    case NETINS_UDP_OPT_CONNECT_TIMEOUT_MS: {
        if (handle->role != NETINS_UDP_ROLE_CLIENT) {
            return netins_udp_set_error(handle, EOPNOTSUPP,
                "connect timeout is only configurable on clients");
        }
        uint32_t connect_timeout_ms = 0;
        if (netins_udp_set_u32_option(handle, value, value_len,
                &connect_timeout_ms) != 0)
        {
            return -1;
        }
        if (connect_timeout_ms == 0) {
            return netins_udp_set_error(handle, EINVAL,
                "connect timeout must be nonzero");
        }
        handle->connect_timeout_ms = connect_timeout_ms;
        break;
    }
    case NETINS_UDP_OPT_ACCEPT_TIMEOUT_MS:
        if (handle->role != NETINS_UDP_ROLE_SERVER) {
            return netins_udp_set_error(handle, EOPNOTSUPP,
                "accept timeout is only configurable on servers");
        }
        if (netins_udp_set_u32_option(handle, value, value_len,
                &handle->accept_timeout_ms) != 0)
        {
            return -1;
        }
        break;
    case NETINS_UDP_OPT_CLOSE_TIMEOUT_MS:
        if (netins_udp_set_u32_option(handle, value, value_len,
                &handle->close_timeout_ms) != 0)
        {
            return -1;
        }
        break;
    case NETINS_UDP_OPT_KERNEL_SNDBUF:
    case NETINS_UDP_OPT_KERNEL_RCVBUF: {
        if (value == NULL || value_len != sizeof(int)
            || *(const int *)value <= 0)
        {
            return netins_udp_set_error(handle, EINVAL,
                "socket buffer option requires a positive int");
        }
        if (option == NETINS_UDP_OPT_KERNEL_SNDBUF) {
            handle->backend_config.has_kernel_sndbuf = 1;
            handle->backend_config.kernel_sndbuf = *(const int *)value;
        } else {
            handle->backend_config.has_kernel_rcvbuf = 1;
            handle->backend_config.kernel_rcvbuf = *(const int *)value;
        }
        break;
    }
    case NETINS_UDP_OPT_LOGGER: {
        if (value == NULL || value_len < sizeof(netins_udp_logger_config_t)) {
            return netins_udp_set_error(handle, EINVAL,
                "invalid logger option size");
        }
        const netins_udp_logger_config_t *logger = value;
        if (logger->struct_size < sizeof(*logger)
            || logger->api_version != NETINS_UDP_API_VERSION
            || logger->minimum_level < NETINS_UDP_LOG_DEBUG
            || logger->minimum_level > NETINS_UDP_LOG_ERROR)
        {
            return netins_udp_set_error(handle, EINVAL,
                "invalid logger option version or level");
        }
        handle->logger = logger->callback;
        handle->logger_user_data = logger->user_data;
        handle->logger_minimum_level = logger->minimum_level;
        handle->backend_config.logger = logger->callback;
        handle->backend_config.logger_user_data = logger->user_data;
        handle->backend_config.logger_minimum_level =
            logger->minimum_level;
        if (handle->role == NETINS_UDP_ROLE_SERVER
            && handle->server_backend != NULL)
        {
            mini_backend_server_set_logger(handle->server_backend,
                logger->callback, logger->user_data,
                logger->minimum_level);
        }
        if (handle->active_stream != NULL
            && handle->active_stream->backend_stream != NULL)
        {
            mini_backend_stream_set_logger(
                handle->active_stream->backend_stream,
                logger->callback, logger->user_data,
                logger->minimum_level);
        }
        break;
    }
    case NETINS_UDP_OPT_ZLOG: {
        if (value == NULL || value_len < sizeof(netins_udp_zlog_config_t)) {
            return netins_udp_set_error(handle, EINVAL,
                "invalid zlog option size");
        }
        const netins_udp_zlog_config_t *zlog = value;
        if (zlog->struct_size < sizeof(*zlog)
            || zlog->api_version != NETINS_UDP_API_VERSION
            || (zlog->enabled != 0 && zlog->enabled != 1)
            || zlog->minimum_level < NETINS_UDP_LOG_DEBUG
            || zlog->minimum_level > NETINS_UDP_LOG_ERROR)
        {
            return netins_udp_set_error(handle, EINVAL,
                "invalid zlog option version, enabled flag, or level");
        }
        if (zlog->enabled
            && (memchr(zlog->config_file, '\0',
                    sizeof(zlog->config_file)) == NULL
                || memchr(zlog->category, '\0',
                    sizeof(zlog->category)) == NULL
                || zlog->config_file[0] == '\0'
                || zlog->category[0] == '\0'))
        {
            return netins_udp_set_error(handle, EINVAL,
                "enabled zlog requires terminated config and category strings");
        }
        handle->backend_config.has_zlog = 1;
        handle->backend_config.zlog_enabled = zlog->enabled;
        handle->backend_config.zlog_minimum_level = zlog->minimum_level;
        handle->backend_config.zlog_config_file[0] = '\0';
        handle->backend_config.zlog_category[0] = '\0';
        if (zlog->enabled) {
            netins_udp_copy_string(handle->backend_config.zlog_config_file,
                sizeof(handle->backend_config.zlog_config_file),
                zlog->config_file);
            netins_udp_copy_string(handle->backend_config.zlog_category,
                sizeof(handle->backend_config.zlog_category),
                zlog->category);
        }
        break;
    }
    case NETINS_UDP_OPT_STREAM_MODE: {
        if (value == NULL || value_len != sizeof(int)
            || (*(const int *)value != 0 && *(const int *)value != 1))
        {
            return netins_udp_set_error(handle, EINVAL,
                "stream mode option requires an int boolean");
        }
        handle->backend_config.stream_mode = *(const int *)value;
        break;
    }
    default:
        return netins_udp_set_error(handle, ENOPROTOOPT,
            "unsupported option");
    }
    netins_udp_clear_error(handle);
    return 0;
}

int
netins_udp_connect(netins_udp_t *client, const char *host, uint16_t port)
{
    struct in_addr ipv4;
    if (!netins_udp_valid(client) || client->role != NETINS_UDP_ROLE_CLIENT
        || client->state != NETINS_UDP_STATE_NEW || host == NULL
        || host[0] == '\0' || port == 0
        || inet_pton(AF_INET, host, &ipv4) != 1)
    {
        return netins_udp_set_error(client, EINVAL,
            "invalid client, host, port, or state");
    }

    client->state = NETINS_UDP_STATE_CONNECTING;
    int backend_created = 0;
    mini_backend_log_scope_enter(&client->backend_config);
    int ret = mini_backend_client_create(&client->backend_config,
        &client->client_backend);
    if (ret == 0) {
        backend_created = 1;
        ret = mini_backend_client_connect(client->client_backend, host, port,
            client->connect_timeout_ms);
    }
    mini_backend_log_scope_leave();
    if (ret != 0) {
        client->state = NETINS_UDP_STATE_ERROR;
        int close_source = !backend_created
            ? NETINS_UDP_CLOSE_INTERNAL
            : (ret == ETIMEDOUT ? NETINS_UDP_CLOSE_TIMEOUT
                                : NETINS_UDP_CLOSE_TRANSPORT);
        netins_udp_set_close_reason(client, close_source,
            ret > 0 ? ret : EIO, ret, "Mini client connection failed");
        return netins_udp_set_error(client, ret > 0 ? ret : EIO,
            "Mini backend client connection failed");
    }
    client->state = NETINS_UDP_STATE_CONNECTED;
    netins_udp_clear_error(client);
    netins_udp_log(client, NETINS_UDP_LOG_INFO,
        "client connected to %s:%u", host, (unsigned)port);
    return 0;
}

int
netins_udp_listen(netins_udp_t *server, const char *host, uint16_t port)
{
    struct in_addr ipv4;
    if (!netins_udp_valid(server) || server->role != NETINS_UDP_ROLE_SERVER
        || server->state != NETINS_UDP_STATE_NEW || host == NULL
        || host[0] == '\0' || port == 0
        || inet_pton(AF_INET, host, &ipv4) != 1)
    {
        return netins_udp_set_error(server, EINVAL,
            "invalid server, host, port, or state");
    }
    netins_udp_log(server, NETINS_UDP_LOG_INFO,
        "Mini API server version=%s\n", NETINS_UDP_API_RELEASE_VERSION);
    if (server->tls_source == NETINS_UDP_TLS_SOURCE_NONE) {
        if (netins_udp_install_memory_tls(server,
                netins_udp_builtin_certificate_pem,
                netins_udp_builtin_certificate_pem_len,
                netins_udp_builtin_private_key_pem,
                netins_udp_builtin_private_key_pem_len,
                NETINS_UDP_TLS_SOURCE_BUILTIN) != 0)
        {
            int error_code = errno;
            return netins_udp_set_error(server,
                error_code != 0 ? error_code : EIO,
                "install built-in TLS credentials failed");
        }
        netins_udp_log(server, NETINS_UDP_LOG_INFO,
            "using built-in TLS certificate and private key\n");
    }
    if (!server->backend_config.has_tls) {
        return netins_udp_set_error(server, EINVAL,
            "server TLS credentials are unavailable");
    }

    mini_backend_log_scope_enter(&server->backend_config);
    int ret = mini_backend_server_create(&server->backend_config,
        &server->server_backend);
    if (ret == 0) {
        ret = mini_backend_server_listen(server->server_backend, host, port);
    }
    mini_backend_log_scope_leave();
    if (ret != 0) {
        server->state = NETINS_UDP_STATE_ERROR;
        netins_udp_set_close_reason(server, NETINS_UDP_CLOSE_INTERNAL,
            ret > 0 ? ret : EIO, ret, "Mini server listen failed");
        return netins_udp_set_error(server, ret > 0 ? ret : EIO,
            "Mini backend server listen failed");
    }
    server->state = NETINS_UDP_STATE_LISTENING;
    netins_udp_clear_error(server);
    netins_udp_log(server, NETINS_UDP_LOG_INFO,
        "server listening on %s:%u", host, (unsigned)port);
    return 0;
}

netins_udp_t *
netins_udp_accept(netins_udp_t *server)
{
    if (!netins_udp_valid(server) || server->role != NETINS_UDP_ROLE_SERVER
        || server->state != NETINS_UDP_STATE_LISTENING
        || server->server_backend == NULL)
    {
        netins_udp_set_error(server, EINVAL,
            "handle is not a listening Mini server");
        return NULL;
    }

    mini_backend_server_connection_t *connection = NULL;
    mini_backend_log_scope_enter(&server->backend_config);
    int ret = mini_backend_server_accept(server->server_backend,
        server->accept_timeout_ms, &connection);
    mini_backend_log_scope_leave();
    if (ret != 0) {
        netins_udp_set_error(server, ret > 0 ? ret : EIO,
            "Mini backend accept failed");
        return NULL;
    }

    netins_udp_t *accepted = calloc(1, sizeof(*accepted));
    if (accepted == NULL) {
        mini_backend_log_scope_enter(&server->backend_config);
        (void)mini_backend_server_connection_shutdown(server->server_backend,
            connection, server->close_timeout_ms);
        mini_backend_server_connection_release(server->server_backend,
            connection);
        mini_backend_log_scope_leave();
        netins_udp_set_error(server, ENOMEM,
            "allocate accepted connection failed");
        return NULL;
    }

    int handle_limit_reached = 0;
    pthread_mutex_lock(&netins_udp_global_lock);
    if (netins_udp_handle_count == UINT_MAX) {
        handle_limit_reached = 1;
    } else {
        netins_udp_handle_count++;
    }
    pthread_mutex_unlock(&netins_udp_global_lock);
    if (handle_limit_reached) {
        free(accepted);
        mini_backend_log_scope_enter(&server->backend_config);
        (void)mini_backend_server_connection_shutdown(server->server_backend,
            connection, server->close_timeout_ms);
        mini_backend_server_connection_release(server->server_backend,
            connection);
        mini_backend_log_scope_leave();
        netins_udp_set_error(server, EOVERFLOW,
            "Mini API handle limit reached");
        return NULL;
    }

    accepted->magic = NETINS_UDP_HANDLE_MAGIC;
    accepted->role = NETINS_UDP_ROLE_ACCEPTED;
    accepted->state = NETINS_UDP_STATE_CONNECTED;
    accepted->connect_timeout_ms = server->connect_timeout_ms;
    accepted->accept_timeout_ms = server->accept_timeout_ms;
    accepted->close_timeout_ms = server->close_timeout_ms;
    accepted->tls_certificate_fd = -1;
    accepted->tls_private_key_fd = -1;
    accepted->backend_config = server->backend_config;
    if (server->backend_config.stream_mode) {
        /* The stream listener's callback user_data belongs to the listener.
         * Stream-mode accepted handles start with an independent empty logger
         * so listener close never leaves a copied callback pointer behind. */
        accepted->backend_config.logger = NULL;
        accepted->backend_config.logger_user_data = NULL;
        accepted->backend_config.logger_minimum_level = NETINS_UDP_LOG_DEBUG;
        accepted->logger = NULL;
        accepted->logger_user_data = NULL;
        accepted->logger_minimum_level = NETINS_UDP_LOG_DEBUG;
    } else {
        /* Preserve the established file-API logger inheritance contract. */
        accepted->logger = server->logger;
        accepted->logger_user_data = server->logger_user_data;
        accepted->logger_minimum_level = server->logger_minimum_level;
    }
    accepted->server_backend = server->server_backend;
    accepted->server_connection = connection;
    mini_backend_server_retain(accepted->server_backend);

    netins_udp_clear_error(server);
    if (server->backend_config.stream_mode) {
        netins_udp_log(server, NETINS_UDP_LOG_INFO,
            "accepted Mini stream connection");
    } else {
        netins_udp_log(accepted, NETINS_UDP_LOG_INFO,
            "accepted Mini connection");
    }
    return accepted;
}

int
netins_udp_transfer(netins_udp_t *client,
    const netins_udp_transfer_t *task,
    netins_udp_transfer_result_t *result)
{
    if (!netins_udp_valid(client) || client->role != NETINS_UDP_ROLE_CLIENT
        || client->state != NETINS_UDP_STATE_CONNECTED
        || client->client_backend == NULL || task == NULL || result == NULL)
    {
        return netins_udp_set_error(client, EINVAL,
            "transfer requires a connected client, task and result");
    }
    if (client->backend_config.stream_mode) {
        return netins_udp_set_error(client, EOPNOTSUPP,
            "file transfer is unavailable on a stream-mode connection");
    }
    if (client->active_stream != NULL) {
        return netins_udp_set_error(client, EBUSY,
            "file transfer is unavailable while a data stream is active");
    }
    if (task->struct_size < sizeof(*task)
        || task->api_version != NETINS_UDP_API_VERSION
        || result->struct_size < sizeof(*result)
        || result->api_version != NETINS_UDP_API_VERSION
        || (task->direction != NETINS_UDP_TRANSFER_GET
            && task->direction != NETINS_UDP_TRANSFER_POST)
        || task->local_path == NULL || task->local_path[0] == '\0'
        || task->remote_path == NULL || task->remote_path[0] != '/'
        || task->remote_path[1] == '\0'
        || strlen(task->local_path) >= NETINS_UDP_FILE_PATH_LEN
        || strlen(task->remote_path) >= NETINS_UDP_REMOTE_PATH_LEN)
    {
        return netins_udp_set_error(client, EINVAL,
            "invalid transfer version, direction, path, or result");
    }

    uint32_t result_size = result->struct_size;
    memset(result, 0, sizeof(*result));
    result->struct_size = result_size;
    result->api_version = NETINS_UDP_API_VERSION;

    mini_backend_transfer_result_t backend_result = {0};
    int backend_direction =
        task->direction == NETINS_UDP_TRANSFER_GET
        ? MINI_BACKEND_TRANSFER_GET : MINI_BACKEND_TRANSFER_POST;
    mini_backend_log_scope_enter(&client->backend_config);
    int ret = mini_backend_client_transfer(client->client_backend,
        backend_direction, task->local_path, task->remote_path,
        task->timeout_ms, &backend_result);
    mini_backend_log_scope_leave();

    result->total_bytes = backend_result.total_bytes;
    result->total_fragments = backend_result.total_fragments;
    result->completed_fragments = backend_result.completed_fragments;
    result->elapsed_us = backend_result.elapsed_us;
    result->throughput_mbps = result->elapsed_us > 0
        ? (double)result->total_bytes * 8.0 / (double)result->elapsed_us
        : 0.0;

    if (ret != 0) {
        if (ret == ETIMEDOUT || ret == EIO || ret == EPROTO
            || mini_backend_client_is_closed(client->client_backend))
        {
            client->state = NETINS_UDP_STATE_ERROR;
            netins_udp_set_close_reason(client,
                ret == ETIMEDOUT ? NETINS_UDP_CLOSE_TIMEOUT
                                 : NETINS_UDP_CLOSE_TRANSPORT,
                ret > 0 ? ret : EIO, ret, "Mini transfer failed");
        }
        int error_code = ret > 0 ? ret : EIO;
        return netins_udp_set_error(client, error_code,
            "Mini transfer failed with backend code %d", ret);
    }
    netins_udp_clear_error(client);
    netins_udp_log(client, NETINS_UDP_LOG_INFO,
        "transfer complete: bytes=%llu fragments=%u elapsed_us=%llu metric=%s",
        (unsigned long long)result->total_bytes,
        result->completed_fragments,
        (unsigned long long)result->elapsed_us,
        task->direction == NETINS_UDP_TRANSFER_GET
            ? "first_body_to_last_body"
            : "app_submit_not_remote_receive");
    return 0;
}

int
netins_udp_serve(netins_udp_t *connection,
    const netins_udp_serve_config_t *config)
{
    if (!netins_udp_valid(connection)
        || connection->role != NETINS_UDP_ROLE_ACCEPTED
        || connection->state != NETINS_UDP_STATE_CONNECTED
        || connection->server_backend == NULL
        || connection->server_connection == NULL || config == NULL)
    {
        return netins_udp_set_error(connection, EINVAL,
            "serve requires a connected accepted handle");
    }
    if (connection->backend_config.stream_mode) {
        return netins_udp_set_error(connection, EOPNOTSUPP,
            "file serving is unavailable on a stream-mode connection");
    }
    if (connection->active_stream != NULL) {
        return netins_udp_set_error(connection, EBUSY,
            "file serving is unavailable while a data stream is active");
    }
    if (config->struct_size < sizeof(*config)
        || config->api_version != NETINS_UDP_API_VERSION
        || config->root_path == NULL || config->root_path[0] == '\0'
        || strlen(config->root_path) >= NETINS_UDP_FILE_PATH_LEN)
    {
        return netins_udp_set_error(connection, EINVAL,
            "invalid serve configuration or root path");
    }

    mini_backend_log_scope_enter(&connection->backend_config);
    int ret = mini_backend_server_connection_serve(
        connection->server_backend, connection->server_connection,
        config->root_path, config->timeout_ms);
    mini_backend_log_scope_leave();
    if (ret != 0) {
        if (ret == ETIMEDOUT) {
            mini_backend_log_scope_enter(&connection->backend_config);
            (void)mini_backend_server_connection_shutdown(
                connection->server_backend,
                connection->server_connection,
                connection->close_timeout_ms);
            mini_backend_log_scope_leave();
            connection->state = NETINS_UDP_STATE_ERROR;
            netins_udp_set_close_reason(connection,
                NETINS_UDP_CLOSE_TIMEOUT, ETIMEDOUT, ret,
                "Mini serve loop timed out");
        } else if (mini_backend_server_connection_is_closed(
                connection->server_backend,
                connection->server_connection))
        {
            connection->state = NETINS_UDP_STATE_ERROR;
            netins_udp_set_close_reason(connection,
                NETINS_UDP_CLOSE_TRANSPORT, ret > 0 ? ret : EIO, ret,
                "Mini accepted connection closed with an error");
        }
        int error_code = ret > 0 ? ret : EIO;
        return netins_udp_set_error(connection, error_code,
            "Mini serve loop failed with backend code %d", ret);
    }

    connection->state = NETINS_UDP_STATE_CLOSED;
    netins_udp_set_close_reason(connection, NETINS_UDP_CLOSE_PEER,
        0, 0, "peer closed the accepted connection");
    netins_udp_clear_error(connection);
    netins_udp_log(connection, NETINS_UDP_LOG_INFO,
        "serve loop ended after peer close");
    return 0;
}

static int
netins_udp_stream_valid(const netins_udp_stream_t *stream)
{
    return stream != NULL && stream->magic == NETINS_UDP_STREAM_MAGIC
        && netins_udp_valid(stream->connection)
        && stream->connection->active_stream == stream
        && stream->backend_stream != NULL;
}

static void
netins_udp_stream_deliver(void *user_data, const void *data,
    size_t data_len, uint64_t offset)
{
    netins_udp_stream_t *stream = user_data;
    if (stream != NULL && stream->magic == NETINS_UDP_STREAM_MAGIC
        && stream->on_data != NULL)
    {
        stream->on_data(stream, data, data_len, offset, stream->user_data);
    }
}

static int
netins_udp_stream_configure(netins_udp_t *connection,
    const netins_udp_stream_config_t *config,
    mini_backend_stream_config_t *backend_config)
{
    if (config == NULL || config->struct_size < sizeof(*config)
        || config->api_version != NETINS_UDP_API_VERSION
        || (config->flags & ~NETINS_UDP_STREAM_FLAG_NONBLOCK) != 0)
    {
        return netins_udp_set_error(connection, EINVAL,
            "invalid stream configuration");
    }
    uint32_t direction = config->direction;
    if (direction == 0) {
        direction = NETINS_UDP_STREAM_DIRECTION_CLIENT_TO_SERVER;
    }
    if (direction != NETINS_UDP_STREAM_DIRECTION_CLIENT_TO_SERVER
        && direction != NETINS_UDP_STREAM_DIRECTION_SERVER_TO_CLIENT)
    {
        return netins_udp_set_error(connection, EINVAL,
            "invalid stream direction %u", config->direction);
    }
    memset(backend_config, 0, sizeof(*backend_config));
    backend_config->direction = direction;
    backend_config->flags = config->flags;
    backend_config->send_queue_size = config->send_queue_size != 0
        ? config->send_queue_size : NETINS_UDP_STREAM_DEFAULT_QUEUE_SIZE;
    backend_config->receive_queue_size = config->receive_queue_size != 0
        ? config->receive_queue_size : NETINS_UDP_STREAM_DEFAULT_QUEUE_SIZE;
    backend_config->logger = connection->backend_config.logger;
    backend_config->logger_user_data =
        connection->backend_config.logger_user_data;
    backend_config->logger_minimum_level =
        connection->backend_config.logger_minimum_level;
    return 0;
}

static int
netins_udp_stream_create(netins_udp_t *connection,
    const netins_udp_stream_config_t *config, int accepting,
    netins_udp_stream_t **out_stream)
{
    if (!netins_udp_valid(connection) || out_stream == NULL
        || connection->state != NETINS_UDP_STATE_CONNECTED
        || connection->active_stream != NULL
        || !connection->backend_config.stream_mode
        || (accepting && connection->role != NETINS_UDP_ROLE_ACCEPTED)
        || (!accepting && connection->role != NETINS_UDP_ROLE_CLIENT))
    {
        return netins_udp_set_error(connection, EINVAL,
            "stream requires a stream-mode connected endpoint");
    }
    *out_stream = NULL;
    mini_backend_stream_config_t backend_config;
    if (netins_udp_stream_configure(connection, config,
            &backend_config) != 0)
    {
        return -1;
    }
    int receiving = accepting
        ? backend_config.direction
            == NETINS_UDP_STREAM_DIRECTION_CLIENT_TO_SERVER
        : backend_config.direction
            == NETINS_UDP_STREAM_DIRECTION_SERVER_TO_CLIENT;
    if (receiving && config->on_data == NULL) {
        return netins_udp_set_error(connection, EINVAL,
            "receiving streams require an on_data callback");
    }

    netins_udp_stream_t *stream = calloc(1, sizeof(*stream));
    if (stream == NULL) {
        return netins_udp_set_error(connection, ENOMEM,
            "allocate stream handle failed");
    }
    stream->magic = NETINS_UDP_STREAM_MAGIC;
    stream->connection = connection;
    stream->direction = backend_config.direction;
    stream->flags = config->flags;
    stream->on_data = config->on_data;
    stream->user_data = config->user_data;
    backend_config.on_data = receiving ? netins_udp_stream_deliver : NULL;
    backend_config.user_data = stream;
    connection->active_stream = stream;

    mini_backend_log_scope_enter(&connection->backend_config);
    int ret = accepting
        ? mini_backend_server_stream_accept(connection->server_backend,
            connection->server_connection, &backend_config,
            &stream->backend_stream)
        : mini_backend_client_stream_open(connection->client_backend,
            &backend_config, &stream->backend_stream);
    mini_backend_log_scope_leave();
    if (ret != 0) {
        connection->active_stream = NULL;
        stream->magic = 0;
        free(stream);
        return netins_udp_set_error(connection, ret > 0 ? ret : EIO,
            "create Mini data stream failed with backend code %d", ret);
    }
    *out_stream = stream;
    netins_udp_clear_error(connection);
    return 0;
}

int
netins_udp_stream_open(netins_udp_t *connected_client,
    const netins_udp_stream_config_t *config,
    netins_udp_stream_t **out_stream)
{
    return netins_udp_stream_create(connected_client, config, 0, out_stream);
}

int
netins_udp_stream_accept(netins_udp_t *accepted_connection,
    const netins_udp_stream_config_t *config,
    netins_udp_stream_t **out_stream)
{
    return netins_udp_stream_create(accepted_connection, config, 1,
        out_stream);
}

int
netins_udp_stream_write(netins_udp_stream_t *stream, const void *data,
    size_t data_len, uint32_t timeout_ms, size_t *accepted)
{
    if (accepted == NULL) {
        errno = EINVAL;
        return -1;
    }
    *accepted = 0;
    if (!netins_udp_stream_valid(stream)) {
        errno = EINVAL;
        return -1;
    }
    mini_backend_log_scope_enter(&stream->connection->backend_config);
    int ret = mini_backend_stream_write(stream->backend_stream, data,
        data_len, timeout_ms, accepted);
    mini_backend_log_scope_leave();
    if (ret != 0) {
        return netins_udp_set_error(stream->connection,
            ret > 0 ? ret : EIO,
            "stream write failed after accepting %zu bytes", *accepted);
    }
    netins_udp_clear_error(stream->connection);
    return 0;
}

int
netins_udp_stream_flush(netins_udp_stream_t *stream, uint32_t timeout_ms)
{
    if (!netins_udp_stream_valid(stream)) {
        errno = EINVAL;
        return -1;
    }
    mini_backend_log_scope_enter(&stream->connection->backend_config);
    int ret = mini_backend_stream_flush(stream->backend_stream, timeout_ms);
    mini_backend_log_scope_leave();
    if (ret != 0) {
        return netins_udp_set_error(stream->connection,
            ret > 0 ? ret : EIO, "stream flush failed");
    }
    netins_udp_clear_error(stream->connection);
    return 0;
}

int
netins_udp_stream_finish(netins_udp_stream_t *stream, uint32_t timeout_ms,
    netins_udp_stream_result_t *result)
{
    if (!netins_udp_stream_valid(stream) || result == NULL
        || result->struct_size < sizeof(*result)
        || result->api_version != NETINS_UDP_API_VERSION)
    {
        errno = EINVAL;
        return -1;
    }
    uint32_t result_size = result->struct_size;
    mini_backend_stream_result_t backend_result;
    mini_backend_log_scope_enter(&stream->connection->backend_config);
    int ret = mini_backend_stream_finish(stream->backend_stream, timeout_ms,
        &backend_result);
    mini_backend_log_scope_leave();
    memset(result, 0, sizeof(*result));
    result->struct_size = result_size;
    result->api_version = NETINS_UDP_API_VERSION;
    result->accepted_bytes = backend_result.accepted_bytes;
    result->peer_received_bytes = backend_result.peer_received_bytes;
    result->elapsed_us = backend_result.elapsed_us;
    if (ret != 0) {
        return netins_udp_set_error(stream->connection,
            ret > 0 ? ret : EIO, "stream finish failed");
    }
    netins_udp_clear_error(stream->connection);
    return 0;
}

int
netins_udp_stream_reset(netins_udp_stream_t *stream, uint64_t error_code)
{
    if (!netins_udp_stream_valid(stream)) {
        errno = EINVAL;
        return -1;
    }
    mini_backend_log_scope_enter(&stream->connection->backend_config);
    int ret = mini_backend_stream_reset(stream->backend_stream, error_code);
    mini_backend_log_scope_leave();
    if (ret != 0) {
        return netins_udp_set_error(stream->connection,
            ret > 0 ? ret : EIO, "stream reset failed");
    }
    netins_udp_clear_error(stream->connection);
    return 0;
}

int
netins_udp_stream_get_snapshot(netins_udp_stream_t *stream,
    netins_udp_stream_snapshot_t *snapshot)
{
    if (!netins_udp_stream_valid(stream) || snapshot == NULL
        || snapshot->struct_size < sizeof(*snapshot)
        || snapshot->api_version != NETINS_UDP_API_VERSION)
    {
        errno = EINVAL;
        return -1;
    }
    uint32_t snapshot_size = snapshot->struct_size;
    mini_backend_stream_snapshot_t backend_snapshot;
    mini_backend_log_scope_enter(&stream->connection->backend_config);
    int ret = mini_backend_stream_get_snapshot(stream->backend_stream,
        &backend_snapshot);
    mini_backend_log_scope_leave();
    if (ret != 0) {
        return netins_udp_set_error(stream->connection,
            ret > 0 ? ret : EIO, "stream snapshot failed");
    }
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->struct_size = snapshot_size;
    snapshot->api_version = NETINS_UDP_API_VERSION;
    snapshot->snapshot_time_us = backend_snapshot.snapshot_time_us;
    snapshot->endpoint = backend_snapshot.endpoint;
    snapshot->state = backend_snapshot.state;
    snapshot->lane_count = backend_snapshot.lane_count;
    snapshot->flags = backend_snapshot.flags;
    snapshot->preaccept_lane_count = backend_snapshot.preaccept_lane_count;
    snapshot->accepted_bytes = backend_snapshot.accepted_bytes;
    snapshot->peer_received_bytes = backend_snapshot.peer_received_bytes;
    snapshot->send_queue_bytes = backend_snapshot.send_queue_bytes;
    snapshot->send_queue_capacity = backend_snapshot.send_queue_capacity;
    snapshot->send_queue_high_water = backend_snapshot.send_queue_high_water;
    snapshot->reorder_bytes = backend_snapshot.reorder_bytes;
    snapshot->reorder_capacity = backend_snapshot.reorder_capacity;
    snapshot->reorder_high_water = backend_snapshot.reorder_high_water;
    snapshot->records_enqueued = backend_snapshot.records_enqueued;
    snapshot->records_submitted = backend_snapshot.records_submitted;
    snapshot->records_received = backend_snapshot.records_received;
    snapshot->records_delivered = backend_snapshot.records_delivered;
    memcpy(snapshot->lane_payload_sent, backend_snapshot.lane_payload_sent,
        sizeof(snapshot->lane_payload_sent));
    memcpy(snapshot->lane_payload_received,
        backend_snapshot.lane_payload_received,
        sizeof(snapshot->lane_payload_received));
    snapshot->last_error = backend_snapshot.last_error;
    snapshot->app_error_code = backend_snapshot.app_error_code;
    return 0;
}

void
netins_udp_stream_close(netins_udp_stream_t *stream)
{
    if (!netins_udp_stream_valid(stream)) {
        return;
    }
    netins_udp_t *connection = stream->connection;
    mini_backend_log_scope_enter(&connection->backend_config);
    mini_backend_stream_destroy(stream->backend_stream);
    mini_backend_log_scope_leave();
    stream->backend_stream = NULL;
    if (connection->active_stream == stream) {
        connection->active_stream = NULL;
    }
    stream->magic = 0;
    free(stream);
}

static int
netins_udp_get_backend_snapshot(netins_udp_t *handle,
    mini_backend_snapshot_t *snapshot)
{
    memset(snapshot, 0, sizeof(*snapshot));
    if (handle->role == NETINS_UDP_ROLE_CLIENT
        && handle->client_backend != NULL)
    {
        mini_backend_log_scope_enter(&handle->backend_config);
        int ret = mini_backend_client_get_snapshot(handle->client_backend,
            snapshot);
        mini_backend_log_scope_leave();
        return ret;
    }
    if (handle->role == NETINS_UDP_ROLE_ACCEPTED
        && handle->server_backend != NULL
        && handle->server_connection != NULL)
    {
        mini_backend_log_scope_enter(&handle->backend_config);
        int ret = mini_backend_server_connection_get_snapshot(
            handle->server_backend, handle->server_connection, snapshot);
        mini_backend_log_scope_leave();
        return ret;
    }
    return 0;
}

static void
netins_udp_copy_path_statis(netins_udp_path_statis_t *destination,
    const mini_backend_path_snapshot_t *source)
{
    uint32_t struct_size = destination->struct_size;
    memset(destination, 0, sizeof(*destination));
    destination->struct_size = struct_size;
    destination->api_version = NETINS_UDP_API_VERSION;
    destination->path_id = source->path_id;
    destination->path_index = source->path_index;
    destination->path_state = source->path_state;
    destination->app_status = source->app_status;
    netins_udp_copy_string(destination->interface_name,
        sizeof(destination->interface_name), source->interface_name);
    netins_udp_copy_string(destination->local_address,
        sizeof(destination->local_address), source->local_address);
    netins_udp_copy_string(destination->peer_address,
        sizeof(destination->peer_address), source->peer_address);
    destination->packets_sent = source->packets_sent;
    destination->packets_received = source->packets_received;
    destination->bytes_sent = source->bytes_sent;
    destination->bytes_received = source->bytes_received;
    destination->reinjected_bytes_sent = source->reinjected_bytes_sent;
    destination->reinjected_bytes_received =
        source->reinjected_bytes_received;
    destination->effective_bytes_received =
        source->effective_bytes_received;
    destination->srtt_us = source->srtt_us;
}

int
netins_udp_is_multipath(netins_udp_t *handle)
{
    if (!netins_udp_valid(handle)) {
        errno = EINVAL;
        return -1;
    }
    (void)netins_udp_get_state(handle);
    mini_backend_snapshot_t snapshot;
    int ret = netins_udp_get_backend_snapshot(handle, &snapshot);
    if (ret != 0) {
        return netins_udp_set_error(handle, ret > 0 ? ret : EIO,
            "read Mini multipath state failed");
    }
    return snapshot.multipath_mode != 0
        && snapshot.active_path_count > 1;
}

int
netins_udp_get_path_count(netins_udp_t *handle, uint32_t *out_count)
{
    if (!netins_udp_valid(handle) || out_count == NULL) {
        return netins_udp_set_error(handle, EINVAL,
            "path count requires a valid handle and output");
    }
    (void)netins_udp_get_state(handle);
    mini_backend_snapshot_t snapshot;
    int ret = netins_udp_get_backend_snapshot(handle, &snapshot);
    if (ret != 0) {
        return netins_udp_set_error(handle, ret > 0 ? ret : EIO,
            "read Mini path count failed");
    }
    *out_count = snapshot.path_count;
    return 0;
}

int
netins_udp_get_path_statis(netins_udp_t *handle, size_t path_index,
    netins_udp_path_statis_t *out)
{
    if (!netins_udp_valid(handle) || out == NULL
        || out->struct_size < sizeof(*out)
        || out->api_version != NETINS_UDP_API_VERSION)
    {
        return netins_udp_set_error(handle, EINVAL,
            "invalid path statistics output");
    }
    (void)netins_udp_get_state(handle);
    mini_backend_snapshot_t snapshot;
    int ret = netins_udp_get_backend_snapshot(handle, &snapshot);
    if (ret != 0) {
        return netins_udp_set_error(handle, ret > 0 ? ret : EIO,
            "read Mini path statistics failed");
    }
    if (path_index >= snapshot.path_count) {
        return netins_udp_set_error(handle, ENOENT,
            "path index is not present");
    }
    netins_udp_copy_path_statis(out, &snapshot.paths[path_index]);
    return 0;
}

int
netins_udp_get_close_reason(netins_udp_t *handle,
    netins_udp_close_reason_t *out)
{
    if (!netins_udp_valid(handle) || out == NULL
        || out->struct_size < sizeof(*out)
        || out->api_version != NETINS_UDP_API_VERSION)
    {
        return netins_udp_set_error(handle, EINVAL,
            "invalid close reason output");
    }
    (void)netins_udp_get_state(handle);
    uint32_t struct_size = out->struct_size;
    memset(out, 0, sizeof(*out));
    out->struct_size = struct_size;
    out->api_version = NETINS_UDP_API_VERSION;
    out->source = handle->close_source;
    out->system_error = handle->close_system_error;
    out->transport_error = handle->close_transport_error;
    netins_udp_copy_string(out->message, sizeof(out->message),
        handle->close_message);
    return 0;
}

int
netins_udp_get_snapshot(netins_udp_t *handle, netins_udp_snapshot_t *out)
{
    if (!netins_udp_valid(handle) || out == NULL
        || out->struct_size < sizeof(*out)
        || out->api_version != NETINS_UDP_API_VERSION)
    {
        return netins_udp_set_error(handle, EINVAL,
            "invalid snapshot output");
    }
    (void)netins_udp_get_state(handle);
    mini_backend_snapshot_t backend;
    int ret = netins_udp_get_backend_snapshot(handle, &backend);
    if (ret != 0) {
        return netins_udp_set_error(handle, ret > 0 ? ret : EIO,
            "read Mini connection snapshot failed");
    }

    uint32_t struct_size = out->struct_size;
    memset(out, 0, sizeof(*out));
    out->struct_size = struct_size;
    out->api_version = NETINS_UDP_API_VERSION;
    out->snapshot_time_us = backend.snapshot_time_us;
    out->role = handle->role;
    out->state = handle->state;
    out->handshake_finished = backend.handshake_finished;
    out->multipath_mode = backend.multipath_mode;
    out->multipath_state = backend.multipath_state;
    out->path_count = backend.path_count;
    out->active_path_count = backend.active_path_count;
    out->packets_sent = backend.packets_sent;
    out->packets_received = backend.packets_received;
    out->packets_lost = backend.packets_lost;
    out->spurious_packets_lost = backend.spurious_packets_lost;
    out->srtt_us = backend.srtt_us;
    out->min_rtt_us = backend.min_rtt_us;
    out->bytes_inflight = backend.bytes_inflight;
    out->total_app_bytes = backend.total_app_bytes;
    out->standby_path_app_bytes = backend.standby_path_app_bytes;
    for (uint32_t i = 0; i < backend.path_count
        && i < NETINS_UDP_MAX_PATHS; i++)
    {
        out->paths[i].struct_size = sizeof(out->paths[i]);
        out->paths[i].api_version = NETINS_UDP_API_VERSION;
        netins_udp_copy_path_statis(&out->paths[i], &backend.paths[i]);
    }
    out->close_reason.struct_size = sizeof(out->close_reason);
    out->close_reason.api_version = NETINS_UDP_API_VERSION;
    out->close_reason.source = handle->close_source;
    out->close_reason.system_error = handle->close_system_error;
    out->close_reason.transport_error = handle->close_transport_error;
    netins_udp_copy_string(out->close_reason.message,
        sizeof(out->close_reason.message), handle->close_message);
    return 0;
}

int
netins_udp_get_state(netins_udp_t *handle)
{
    if (!netins_udp_valid(handle)) {
        errno = EINVAL;
        return NETINS_UDP_STATE_ERROR;
    }
    int poll_error = 0;
    mini_backend_log_scope_enter(&handle->backend_config);
    if (handle->role == NETINS_UDP_ROLE_CLIENT
        && handle->client_backend != NULL)
    {
        /* The stream worker drives the event base while active.  Backend
         * state queries are separately serialized with that worker. */
        if (handle->active_stream == NULL) {
            poll_error = mini_backend_client_poll(handle->client_backend);
        }
    } else if ((handle->role == NETINS_UDP_ROLE_SERVER
            || handle->role == NETINS_UDP_ROLE_ACCEPTED)
        && handle->server_backend != NULL)
    {
        if (handle->active_stream == NULL) {
            poll_error = mini_backend_server_poll(handle->server_backend);
        }
        if (poll_error == 0) {
            poll_error =
                mini_backend_server_last_error(handle->server_backend);
        }
    }
    mini_backend_log_scope_leave();
    if (poll_error != 0) {
        handle->state = NETINS_UDP_STATE_ERROR;
        netins_udp_set_close_reason(handle, NETINS_UDP_CLOSE_INTERNAL,
            poll_error > 0 ? poll_error : EIO, poll_error,
            "Mini backend event processing failed");
        (void)netins_udp_set_error(handle,
            poll_error > 0 ? poll_error : EIO,
            "Mini backend event processing failed");
        return handle->state;
    }
    if (handle->role == NETINS_UDP_ROLE_CLIENT
        && handle->client_backend != NULL
        && mini_backend_client_is_closed(handle->client_backend))
    {
        int backend_error =
            mini_backend_client_last_error(handle->client_backend);
        if (backend_error != 0) {
            handle->state = NETINS_UDP_STATE_ERROR;
            netins_udp_set_close_reason(handle, NETINS_UDP_CLOSE_TRANSPORT,
                ECONNRESET, backend_error,
                "XQUIC client connection closed with an error");
            (void)netins_udp_set_error(handle, ECONNRESET,
                "XQUIC client connection closed with code %d",
                backend_error);
        } else {
            if (handle->state != NETINS_UDP_STATE_ERROR) {
                handle->state = NETINS_UDP_STATE_CLOSED;
            }
            if (handle->close_source == NETINS_UDP_CLOSE_NONE) {
                netins_udp_set_close_reason(handle, NETINS_UDP_CLOSE_PEER,
                    0, 0, "peer closed the client connection");
            }
        }
    } else if (handle->role == NETINS_UDP_ROLE_ACCEPTED
        && handle->server_connection != NULL
        && mini_backend_server_connection_is_closed(
            handle->server_backend,
            handle->server_connection))
    {
        int backend_error = mini_backend_server_connection_last_error(
            handle->server_backend,
            handle->server_connection);
        if (backend_error != 0) {
            handle->state = NETINS_UDP_STATE_ERROR;
            netins_udp_set_close_reason(handle, NETINS_UDP_CLOSE_TRANSPORT,
                ECONNRESET, backend_error,
                "XQUIC accepted connection closed with an error");
            (void)netins_udp_set_error(handle, ECONNRESET,
                "XQUIC accepted connection closed with code %d",
                backend_error);
        } else {
            if (handle->state != NETINS_UDP_STATE_ERROR) {
                handle->state = NETINS_UDP_STATE_CLOSED;
            }
            if (handle->close_source == NETINS_UDP_CLOSE_NONE) {
                netins_udp_set_close_reason(handle, NETINS_UDP_CLOSE_PEER,
                    0, 0, "peer closed the accepted connection");
            }
        }
    }
    return handle->state;
}

int
netins_udp_shutdown(netins_udp_t *handle)
{
    if (!netins_udp_valid(handle)) {
        errno = EINVAL;
        return -1;
    }
    if (handle->state == NETINS_UDP_STATE_CLOSED) {
        return 0;
    }
    if (handle->active_stream != NULL) {
        return netins_udp_set_error(handle, EBUSY,
            "close the active data stream before shutdown");
    }

    if (handle->close_source == NETINS_UDP_CLOSE_NONE) {
        netins_udp_set_close_reason(handle, NETINS_UDP_CLOSE_LOCAL,
            0, 0, "local shutdown requested");
    }
    handle->state = NETINS_UDP_STATE_CLOSING;
    int ret = 0;
    mini_backend_log_scope_enter(&handle->backend_config);
    if (handle->role == NETINS_UDP_ROLE_CLIENT
        && handle->client_backend != NULL)
    {
        ret = mini_backend_client_shutdown(handle->client_backend,
            handle->close_timeout_ms);
    } else if (handle->role == NETINS_UDP_ROLE_SERVER
        && handle->server_backend != NULL)
    {
        mini_backend_server_shutdown(handle->server_backend);
    } else if (handle->role == NETINS_UDP_ROLE_ACCEPTED
        && handle->server_backend != NULL
        && handle->server_connection != NULL)
    {
        ret = mini_backend_server_connection_shutdown(handle->server_backend,
            handle->server_connection, handle->close_timeout_ms);
    }
    mini_backend_log_scope_leave();
    if (ret != 0) {
        handle->state = NETINS_UDP_STATE_ERROR;
        netins_udp_set_close_reason(handle,
            ret == ETIMEDOUT ? NETINS_UDP_CLOSE_TIMEOUT
                             : NETINS_UDP_CLOSE_INTERNAL,
            ret > 0 ? ret : EIO, ret, "Mini backend shutdown failed");
        return netins_udp_set_error(handle, ret > 0 ? ret : EIO,
            "Mini backend shutdown failed");
    }
    handle->state = NETINS_UDP_STATE_CLOSED;
    netins_udp_clear_error(handle);
    netins_udp_log(handle, NETINS_UDP_LOG_INFO,
        "handle shutdown complete");
    return 0;
}

void
netins_udp_close(netins_udp_t *handle)
{
    if (!netins_udp_valid(handle)) {
        return;
    }
    if (handle->active_stream != NULL) {
        netins_udp_stream_close(handle->active_stream);
    }
    if (handle->state != NETINS_UDP_STATE_CLOSED) {
        (void)netins_udp_shutdown(handle);
    }

    mini_backend_log_scope_enter(&handle->backend_config);
    if (handle->role == NETINS_UDP_ROLE_CLIENT) {
        mini_backend_client_destroy(handle->client_backend);
        handle->client_backend = NULL;
    } else if (handle->role == NETINS_UDP_ROLE_SERVER) {
        if (handle->backend_config.stream_mode) {
            /* Stream-mode accepted connections retain the shared server
             * worker.  Make it adopt an empty logger before close() returns
             * so it cannot call listener-owned callback state afterwards. */
            mini_backend_server_set_logger(handle->server_backend,
                NULL, NULL, NETINS_UDP_LOG_ERROR);
        }
        mini_backend_server_release(handle->server_backend);
        handle->server_backend = NULL;
    } else if (handle->role == NETINS_UDP_ROLE_ACCEPTED) {
        mini_backend_server_connection_release(handle->server_backend,
            handle->server_connection);
        handle->server_connection = NULL;
        mini_backend_server_release(handle->server_backend);
        handle->server_backend = NULL;
    }
    mini_backend_log_scope_leave();
    netins_udp_close_tls_fds(handle);

    handle->magic = 0;
    pthread_mutex_lock(&netins_udp_global_lock);
    if (netins_udp_handle_count > 0) {
        netins_udp_handle_count--;
    }
    pthread_mutex_unlock(&netins_udp_global_lock);
    free(handle);
}

int
netins_udp_get_last_error_code(netins_udp_t *handle)
{
    if (!netins_udp_valid(handle)) {
        errno = EINVAL;
        return EINVAL;
    }
    return handle->last_error_code;
}

int
netins_udp_get_last_error_msg(netins_udp_t *handle, char *buffer,
    size_t buffer_len)
{
    if (!netins_udp_valid(handle) || buffer == NULL || buffer_len == 0) {
        errno = EINVAL;
        return -1;
    }
    size_t length = strlen(handle->last_error_msg);
    if (length >= buffer_len) {
        errno = ENOBUFS;
        return -1;
    }
    memcpy(buffer, handle->last_error_msg, length + 1);
    return 0;
}
