// serialmux_preload.c
// LD_PRELOAD library for serialmux (Linux) with a simplified, robust IPC mechanism.
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <termios.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdint.h>
#include <poll.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <byteswap.h>

#define DATA_SOCKET_PATH "/tmp/serialmux.sock"
#define CTRL_SOCKET_PATH "/tmp/serialmux_ctrl.sock"
#define DAEMON_DEVICE_ENV "SERIALMUX_DEVICE"
#define SERIALMUX_FALLBACK_ENV "SERIALMUX_FALLBACK"
#define MAX_MAPPED_FDS 1024
#define CONNECT_TIMEOUT_MS 200
#define MAGIC 0x534d494f

#include "serialmux.h"

typedef struct {
    int fd;
    int ioctl_sock;
    pid_t pid;
} FDMapping;

static FDMapping fd_map[MAX_MAPPED_FDS];
static int num_mapped = 0;
static pthread_mutex_t map_lock = PTHREAD_MUTEX_INITIALIZER;
static char *target_device = NULL;
static int allow_fallback = 0;

static pthread_once_t init_once = PTHREAD_ONCE_INIT;

/* libc function pointers */
static int (*real_open)(const char *, int, ...) = NULL;
static int (*real_close)(int) = NULL;
static int (*real_ioctl)(int, unsigned long, ...) = NULL;

/* debug helper */
static void debug_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "SERIALMUX_PRELOAD: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

static void resolve_symbols() {
    if (!real_open) real_open = dlsym(RTLD_NEXT, "open");
    if (!real_close) real_close = dlsym(RTLD_NEXT, "close");
    if (!real_ioctl) real_ioctl = dlsym(RTLD_NEXT, "ioctl");
}

/* mapping helpers */
static FDMapping *add_mapping(int fd) {
    if (fd < 0) return NULL;
    pthread_mutex_lock(&map_lock);
    for (int i = 0; i < num_mapped; ++i) {
        if (fd_map[i].fd == fd) {
            pthread_mutex_unlock(&map_lock);
            return &fd_map[i];
        }
    }
    if (num_mapped < MAX_MAPPED_FDS) {
        fd_map[num_mapped].fd = fd;
        fd_map[num_mapped].pid = getpid();
        fd_map[num_mapped].ioctl_sock = -1;
        int idx = num_mapped;
        ++num_mapped;
        pthread_mutex_unlock(&map_lock);
        return &fd_map[idx];
    }
    pthread_mutex_unlock(&map_lock);
    return NULL;
}

static void remove_mapping(int fd) {
    if (fd < 0) return;
    pthread_mutex_lock(&map_lock);
    int idx = -1;
    for (int i = 0; i < num_mapped; ++i) {
        if (fd_map[i].fd == fd) { idx = i; break; }
    }
    if (idx != -1) {
        if (fd_map[idx].ioctl_sock >= 0) close(fd_map[idx].ioctl_sock);
        fd_map[idx] = fd_map[num_mapped - 1];
        --num_mapped;
    }
    pthread_mutex_unlock(&map_lock);
}

static FDMapping *find_mapping(int fd) {
    if (fd < 0) return NULL;
    pthread_mutex_lock(&map_lock);
    for (int i = 0; i < num_mapped; ++i) {
        if (fd_map[i].fd == fd) {
            pthread_mutex_unlock(&map_lock);
            return &fd_map[i];
        }
    }
    pthread_mutex_unlock(&map_lock);
    return NULL;
}

/* robust send/recv helpers */
static ssize_t send_all(int sock, const void *buf, size_t len) {
    const unsigned char *p = buf;
    size_t left = len;
    while (left > 0) {
        ssize_t s = send(sock, p, left, MSG_NOSIGNAL);
        if (s < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += s;
        left -= (size_t)s;
    }
    return (ssize_t)len;
}

static ssize_t recv_all_fd(int sock, void *buf, size_t len) {
    unsigned char *p = buf;
    size_t left = len;
    while (left > 0) {
        ssize_t r = recv(sock, p, left, 0);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (r == 0) return (ssize_t)(len - left); // peer closed
        p += r;
        left -= (size_t)r;
    }
    return (ssize_t)len;
}

/* non-blocking connect with timeout (returns connected socket fd or -1) */
static int connect_with_timeout(const struct sockaddr_un *addr, socklen_t addrlen, int timeout_ms) {
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) return -1;

    int flags = fcntl(sock, F_GETFL, 0);
    if (flags < 0) { close(sock); return -1; }
    if (fcntl(sock, F_SETFL, flags | O_NONBLOCK) < 0) { close(sock); return -1; }

    int rc = connect(sock, (const struct sockaddr *)addr, addrlen);
    if (rc == 0) {
        fcntl(sock, F_SETFL, flags);
        return sock;
    }
    if (errno != EINPROGRESS) {
        close(sock);
        return -1;
    }

    struct pollfd pfd = { .fd = sock, .events = POLLOUT };
    int pret = poll(&pfd, 1, timeout_ms);
    if (pret == 1) {
        int err = 0;
        socklen_t errlen = sizeof(err);
        if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &errlen) < 0 || err != 0) {
            close(sock);
            return -1;
        }
        fcntl(sock, F_SETFL, flags);
        return sock;
    }
    close(sock);
    return -1;
}

static void init_once_fn(void) {
    resolve_symbols();
    char *env = getenv(DAEMON_DEVICE_ENV);
    if (env) target_device = strdup(env);
    char *fb = getenv(SERIALMUX_FALLBACK_ENV);
    if (fb && strcmp(fb, "1") == 0) allow_fallback = 1;

    if (!target_device) debug_log("%s unset: preload will not intercept device unless configured", DAEMON_DEVICE_ENV);
}

/* wrapper implementations */
int open(const char *path, int flags, ...) {
    pthread_once(&init_once, init_once_fn);

    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, mode_t);
        va_end(ap);
    }

    if (!target_device || strcmp(path, target_device) != 0) {
        return real_open(path, flags, mode);
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, DATA_SOCKET_PATH, sizeof(addr.sun_path) - 1);

    int sock = connect_with_timeout((struct sockaddr_un *)&addr, sizeof(addr), CONNECT_TIMEOUT_MS);
    if (sock < 0) {
        if (allow_fallback) return real_open(path, flags, mode);
        errno = EIO;
        return -1;
    }

    FDMapping *mapping = add_mapping(sock);

    struct sockaddr_un ctrl_addr;
    memset(&ctrl_addr, 0, sizeof(ctrl_addr));
    ctrl_addr.sun_family = AF_UNIX;
    strncpy(ctrl_addr.sun_path, CTRL_SOCKET_PATH, sizeof(ctrl_addr.sun_path) - 1);
    mapping->ioctl_sock = connect_with_timeout((struct sockaddr_un *)&ctrl_addr, sizeof(ctrl_addr), CONNECT_TIMEOUT_MS);

    return sock;
}

int close(int fd) {
    resolve_symbols();
    remove_mapping(fd);
    return real_close(fd);
}

int ioctl(int fd, unsigned long request, ...) {
    resolve_symbols();
    va_list ap;
    va_start(ap, request);
    void *argp = va_arg(ap, void *);
    va_end(ap);

    FDMapping *c = find_mapping(fd);
    if (!c) {
        return real_ioctl(fd, request, argp);
    }

    static uint32_t next_req = 1;
    uint32_t req_id = __sync_fetch_and_add(&next_req, 1);

    uint32_t net_magic = htonl(MAGIC);
    uint32_t net_reqid = htonl(req_id);
    uint64_t ioc = (uint64_t)request;
    uint64_t ioc_be = htobe64(ioc);

    size_t arg_len = ioctl_arg_size(request);
    char tmpbuf[128];
    void *payload = NULL;

    if (argp && arg_len > 0) {
        if (arg_len > sizeof(tmpbuf)) payload = malloc(arg_len);
        else payload = tmpbuf;
        memcpy(payload, argp, arg_len);
    }

    uint32_t arg_len32 = (uint32_t)arg_len;
    uint32_t net_arg_len = htonl(arg_len32);

    if (send_all(c->ioctl_sock, &net_magic, 4) < 0) { if (payload != tmpbuf) free(payload); errno = EIO; return -1; }
    if (send_all(c->ioctl_sock, &net_reqid, 4) < 0) { if (payload != tmpbuf) free(payload); errno = EIO; return -1; }
    if (send_all(c->ioctl_sock, &ioc_be, 8) < 0) { if (payload != tmpbuf) free(payload); errno = EIO; return -1; }
    if (send_all(c->ioctl_sock, &net_arg_len, 4) < 0) { if (payload != tmpbuf) free(payload); errno = EIO; return -1; }
    if (arg_len) {
        if (send_all(c->ioctl_sock, payload, arg_len) < 0) { if (payload != tmpbuf) free(payload); errno = EIO; return -1; }
    }
    if (payload != tmpbuf && payload) free(payload);

    uint32_t resp_reqid_n;
    int32_t resp_ret_n, resp_errno_n;
    uint32_t resp_outlen_n;
    if (recv_all_fd(c->ioctl_sock, &resp_reqid_n, 4) < 0) { errno = EIO; return -1; }
    if (recv_all_fd(c->ioctl_sock, &resp_ret_n, 4) < 0) { errno = EIO; return -1; }
    if (recv_all_fd(c->ioctl_sock, &resp_errno_n, 4) < 0) { errno = EIO; return -1; }
    if (recv_all_fd(c->ioctl_sock, &resp_outlen_n, 4) < 0) { errno = EIO; return -1; }

    uint32_t resp_reqid = ntohl(resp_reqid_n);
    int32_t resp_ret = ntohl(resp_ret_n);
    int32_t resp_errno = ntohl(resp_errno_n);
    uint32_t resp_outlen = ntohl(resp_outlen_n);

    if (resp_reqid != req_id) {
        errno = EIO; return -1;
    }

    if (resp_outlen > 0 && argp) {
        void *outbuf = malloc(resp_outlen);
        if (!outbuf) { errno = ENOMEM; return -1; }
        if (recv_all_fd(c->ioctl_sock, outbuf, resp_outlen) < 0) { free(outbuf); errno = EIO; return -1; }
        memcpy(argp, outbuf, resp_outlen);
        free(outbuf);
    }

    if (resp_ret == -1) {
        errno = resp_errno;
        return -1;
    }
    return resp_ret;
}
