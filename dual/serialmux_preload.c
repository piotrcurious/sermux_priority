// serialmux_preload.c
// LD_PRELOAD library for serialmux (Linux)
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

#include "serialmux.h"

#define MAX_MAPPED_FDS 1024
#define CONNECT_TIMEOUT_MS 200

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
        if (fd_map[idx].ioctl_sock >= 0) real_close(fd_map[idx].ioctl_sock);
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
        if (r == 0) return (ssize_t)(len - left); 
        p += r;
        left -= (size_t)r;
    }
    return (ssize_t)len;
}

static int connect_with_timeout(const struct sockaddr_un *addr, socklen_t addrlen, int timeout_ms) {
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) return -1;

    int flags = fcntl(sock, F_GETFL, 0);
    if (flags < 0) { real_close(sock); return -1; }
    if (fcntl(sock, F_SETFL, flags | O_NONBLOCK) < 0) { real_close(sock); return -1; }

    int rc = connect(sock, (const struct sockaddr *)addr, addrlen);
    if (rc == 0) {
        fcntl(sock, F_SETFL, flags);
        return sock;
    }
    if (errno != EINPROGRESS) {
        real_close(sock);
        return -1;
    }

    struct pollfd pfd = { .fd = sock, .events = POLLOUT };
    int pret = poll(&pfd, 1, timeout_ms);
    if (pret == 1) {
        int err = 0;
        socklen_t errlen = sizeof(err);
        if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &errlen) < 0 || err != 0) {
            real_close(sock);
            return -1;
        }
        fcntl(sock, F_SETFL, flags);
        return sock;
    }
    real_close(sock);
    return -1;
}

static void init_once_fn(void) {
    resolve_symbols();
    char *env = getenv(DAEMON_DEVICE_ENV);
    if (env) target_device = strdup(env);
    char *fb = getenv(SERIALMUX_FALLBACK_ENV);
    if (fb && strcmp(fb, "1") == 0) allow_fallback = 1;
}

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
    if (!mapping) {
        real_close(sock);
        errno = ENOMEM;
        return -1;
    }

    struct sockaddr_un ctrl_addr;
    memset(&ctrl_addr, 0, sizeof(ctrl_addr));
    ctrl_addr.sun_family = AF_UNIX;
    strncpy(ctrl_addr.sun_path, CTRL_SOCKET_PATH, sizeof(ctrl_addr.sun_path) - 1);
    mapping->ioctl_sock = connect_with_timeout((struct sockaddr_un *)&ctrl_addr, sizeof(ctrl_addr), CONNECT_TIMEOUT_MS);

    return sock;
}

int close(int fd) {
    resolve_symbols();
    FDMapping *c = find_mapping(fd);
    if (c) {
        remove_mapping(fd);
        // Note: The main FD is the socket, real_close cleans it up.
        // remove_mapping cleans up the ioctl_sock.
    }
    return real_close(fd);
}

int ioctl(int fd, unsigned long request, ...) {
    resolve_symbols();
    va_list ap;
    va_start(ap, request);
    // Grab the argument. It might be a pointer or an integer/long.
    // va_arg grabs the width of a pointer/long on the stack.
    void *argp = va_arg(ap, void *);
    va_end(ap);

    FDMapping *c = find_mapping(fd);
    if (!c || c->ioctl_sock < 0) {
        return real_ioctl(fd, request, argp);
    }

    static uint32_t next_req = 1;
    uint32_t req_id = __sync_fetch_and_add(&next_req, 1);

    // Determine if this is a pointer ioctl or a value ioctl
    size_t arg_size = ioctl_arg_size(request);
    
    // If arg_size is 0, we treat 'argp' as the VALUE (e.g., TCFLSH uses the value directly)
    // If arg_size > 0, we treat 'argp' as a POINTER to data
    
    uint64_t arg_value_raw = (uint64_t)(uintptr_t)argp;

    uint32_t net_magic = htonl(MAGIC);
    uint32_t net_reqid = htonl(req_id);
    uint64_t ioc_be = htobe64((uint64_t)request);
    uint64_t arg_val_be = htobe64(arg_value_raw);
    uint32_t net_arg_len = htonl((uint32_t)arg_size);

    if (send_all(c->ioctl_sock, &net_magic, 4) < 0) { errno = EIO; return -1; }
    if (send_all(c->ioctl_sock, &net_reqid, 4) < 0) { errno = EIO; return -1; }
    if (send_all(c->ioctl_sock, &ioc_be, 8) < 0) { errno = EIO; return -1; }
    if (send_all(c->ioctl_sock, &arg_val_be, 8) < 0) { errno = EIO; return -1; }
    if (send_all(c->ioctl_sock, &net_arg_len, 4) < 0) { errno = EIO; return -1; }

    // If it's a pointer type ioctl, send the data it points to
    if (arg_size > 0 && argp != NULL) {
        if (send_all(c->ioctl_sock, argp, arg_size) < 0) { errno = EIO; return -1; }
    }

    // Wait for response
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

    // Read back output data if any
    if (resp_outlen > 0 && argp) {
        // We read into a temp buffer then copy to argp to avoid partial reads corrupting user mem
        char *outbuf = malloc(resp_outlen);
        if (!outbuf) { errno = ENOMEM; return -1; }
        
        if (recv_all_fd(c->ioctl_sock, outbuf, resp_outlen) < 0) {
            free(outbuf); errno = EIO; return -1;
        }
        
        // Only copy back if we really expect data (double safety)
        if (arg_size > 0) {
             memcpy(argp, outbuf, (resp_outlen < arg_size) ? resp_outlen : arg_size);
        }
        free(outbuf);
    }

    if (resp_ret == -1) {
        errno = resp_errno;
        return -1;
    }
    return resp_ret;
}
