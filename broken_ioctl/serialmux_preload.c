// serialmux_preload.c
// LD_PRELOAD library for serialmux (Linux) with ioctl/tc* forwarding
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

#define SOCKET_PATH "/tmp/serialmux.sock"
#define DAEMON_DEVICE_ENV "SERIALMUX_DEVICE"
#define SERIALMUX_FALLBACK_ENV "SERIALMUX_FALLBACK"
#define MAX_MAPPED_FDS 1024
#define CONNECT_TIMEOUT_MS 200

#include "serialmux.h"

typedef struct {
    int fd;
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
static int (*real_open64)(const char *, int, ...) = NULL;
static int (*real_openat)(int, const char *, int, ...) = NULL;
static int (*real_close)(int) = NULL;
static int (*real_dup)(int) = NULL;
static int (*real_dup2)(int, int) = NULL;
static int (*real_dup3)(int, int, int) = NULL;
static int (*real_fcntl)(int, int, ...) = NULL;
static FILE *(*real_fdopen)(int, const char *) = NULL;
static FILE *(*real_fopen)(const char *, const char *) = NULL;
static FILE *(*real_freopen)(const char *, const char *, FILE *) = NULL;
static int (*real_fclose)(FILE *) = NULL;
static int (*real_ioctl)(int, unsigned long, void *) = NULL;
static int (*real_tcflush)(int, int) = NULL;
static int (*real_tcsendbreak)(int, int) = NULL;
static int (*real_tcdrain)(int) = NULL;

/* debug helper */
static void debug_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "SERIALMUX_PRELOAD: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

/* mapping helpers */
static void add_mapping(int fd) {
    if (fd < 0) return;
    pthread_mutex_lock(&map_lock);
    for (int i = 0; i < num_mapped; ++i) {
        if (fd_map[i].fd == fd) { pthread_mutex_unlock(&map_lock); return; }
    }
    if (num_mapped < MAX_MAPPED_FDS) {
        fd_map[num_mapped].fd = fd;
        fd_map[num_mapped].pid = getpid();
        ++num_mapped;
    } else {
        debug_log("mapping overflow, cannot track fd %d", fd);
    }
    pthread_mutex_unlock(&map_lock);
}

static void remove_mapping(int fd) {
    if (fd < 0) return;
    pthread_mutex_lock(&map_lock);
    int idx = -1;
    for (int i = 0; i < num_mapped; ++i) {
        if (fd_map[i].fd == fd) { idx = i; break; }
    }
    if (idx != -1) {
        fd_map[idx] = fd_map[num_mapped - 1];
        --num_mapped;
    }
    pthread_mutex_unlock(&map_lock);
}

static int is_mapped(int fd) {
    if (fd < 0) return 0;
    pthread_mutex_lock(&map_lock);
    int found = 0;
    for (int i = 0; i < num_mapped; ++i) {
        if (fd_map[i].fd == fd) { found = 1; break; }
    }
    pthread_mutex_unlock(&map_lock);
    return found;
}

/* robust send/recv helpers */
static ssize_t send_all(int sock, const void *buf, size_t len) {
    const unsigned char *p = buf;
    size_t left = len;
    while (left > 0) {
        ssize_t s = send(sock, p, left, MSG_NOSIGNAL);
        if (s < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) { usleep(1000); continue; }
            return -1;
        }
        p += s;
        left -= (size_t)s;
    }
    return (ssize_t)len;
}

static ssize_t recv_all(int sock, void *buf, size_t len) {
    unsigned char *p = buf;
    size_t left = len;
    while (left > 0) {
        ssize_t r = recv(sock, p, left, 0);
        if (r < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) { usleep(1000); continue; }
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
        // connected immediately, restore flags
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
        // success
        fcntl(sock, F_SETFL, flags);
        return sock;
    }
    close(sock);
    return -1;
}

static int send_request(int req_type, int fd, unsigned long request, void *argp);

/* passthrough ioctl (call libc's ioctl on current fd) */
static int passthrough_ioctl(int fd, unsigned long request, void *argp) {
    if (!real_ioctl) real_ioctl = dlsym(RTLD_NEXT, "ioctl");
    if (!real_ioctl) { errno = ENOSYS; return -1; }
    return real_ioctl(fd, request, argp);
}

/* init function */
static void init_once_fn(void) {
    dlerror();
    real_open = dlsym(RTLD_NEXT, "open");
    real_open64 = dlsym(RTLD_NEXT, "open64");
    real_openat = dlsym(RTLD_NEXT, "openat");
    real_close = dlsym(RTLD_NEXT, "close");
    real_dup = dlsym(RTLD_NEXT, "dup");
    real_dup2 = dlsym(RTLD_NEXT, "dup2");
    real_dup3 = dlsym(RTLD_NEXT, "dup3");
    real_fcntl = dlsym(RTLD_NEXT, "fcntl");
    real_fdopen = dlsym(RTLD_NEXT, "fdopen");
    real_fopen = dlsym(RTLD_NEXT, "fopen");
    real_freopen = dlsym(RTLD_NEXT, "freopen");
    real_fclose = dlsym(RTLD_NEXT, "fclose");
    real_ioctl = dlsym(RTLD_NEXT, "ioctl");
    real_tcflush = dlsym(RTLD_NEXT, "tcflush");
    real_tcsendbreak = dlsym(RTLD_NEXT, "tcsendbreak");
    real_tcdrain = dlsym(RTLD_NEXT, "tcdrain");

    char *env = getenv(DAEMON_DEVICE_ENV);
    if (env) target_device = strdup(env);
    char *fb = getenv(SERIALMUX_FALLBACK_ENV);
    if (fb && strcmp(fb, "1") == 0) allow_fallback = 1;

    if (!target_device) debug_log("%s unset: preload will not intercept device unless configured", DAEMON_DEVICE_ENV);
}

/* helper: mode conversion for fopen */
static int fopen_mode_to_flags(const char *mode) {
    if (!mode) return O_RDONLY;
    int plus = strchr(mode, '+') != NULL;
    int r = strchr(mode, 'r') != NULL;
    int w = strchr(mode, 'w') != NULL;
    int a = strchr(mode, 'a') != NULL;
    if (plus) return O_RDWR;
    if (r && !w && !a) return O_RDONLY;
    if (w) return O_WRONLY | O_CREAT | O_TRUNC;
    if (a) return O_WRONLY | O_CREAT | O_APPEND;
    return O_RDONLY;
}

/* connect to daemon and request PTY path (text protocol existing in daemon) */
static int connect_and_get_pty(const char *device, char *pty_path_buf, size_t buf_size) {
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    int sock = connect_with_timeout(&addr, sizeof(addr), CONNECT_TIMEOUT_MS);
    if (sock < 0) return -1;

    const char *priority_str = getenv("SERIALMUX_PRIORITY");
    int prio = (priority_str && strcmp(priority_str, "HIGH") == 0) ? 1 : 0;

    char msg[512];
    int len = snprintf(msg, sizeof(msg), "OPEN:%s:%d:%d", device, prio, (int)getpid());
    if (len < 0 || (size_t)len >= sizeof(msg)) { close(sock); return -1; }

    if (send_all(sock, msg, (size_t)len + 1) < 0) { close(sock); return -1; }

    ssize_t n = recv(sock, pty_path_buf, buf_size - 1, 0);
    close(sock);
    if (n <= 0) return -1;
    pty_path_buf[n] = '\0';
    if (strncmp(pty_path_buf, "ERROR:", 6) == 0) return -1;
    return 0;
}

/* wrapper implementations */

/* open */
int open(const char *path, int flags, ...) {
    pthread_once(&init_once, init_once_fn);

    mode_t mode = 0;
    int need_mode = (flags & O_CREAT) != 0;
    if (need_mode) {
        va_list ap;
        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
    }

    if (!target_device || strcmp(path, target_device) != 0) {
        if (need_mode) return real_open(path, flags, mode);
        else return real_open(path, flags);
    }

    char pty_path[256];
    if (connect_and_get_pty(path, pty_path, sizeof(pty_path)) == 0) {
        int fd = need_mode ? real_open(pty_path, flags, mode) : real_open(pty_path, flags);
        if (fd >= 0) add_mapping(fd);
        return fd;
    }

    if (allow_fallback) {
        debug_log("daemon unreachable, falling back to real open(%s)", path);
        if (need_mode) return real_open(path, flags, mode);
        else return real_open(path, flags);
    }

    errno = EIO;
    return -1;
}

/* open64 */
int open64(const char *path, int flags, ...) {
    pthread_once(&init_once, init_once_fn);

    mode_t mode = 0;
    int need_mode = (flags & O_CREAT) != 0;
    if (need_mode) {
        va_list ap; va_start(ap, flags); mode = (mode_t)va_arg(ap, int); va_end(ap);
    }

    if (!target_device || strcmp(path, target_device) != 0) {
        if (real_open64) {
            if (need_mode) return real_open64(path, flags, mode);
            else return real_open64(path, flags);
        } else {
            if (need_mode) return real_open(path, flags, mode);
            else return real_open(path, flags);
        }
    }

    char pty_path[256];
    if (connect_and_get_pty(path, pty_path, sizeof(pty_path)) == 0) {
        int fd = need_mode ? real_open64(pty_path, flags, mode) : real_open(pty_path, flags);
        if (fd >= 0) add_mapping(fd);
        return fd;
    }

    if (allow_fallback) {
        if (real_open64) {
            if (need_mode) return real_open64(path, flags, mode);
            else return real_open64(path, flags);
        } else {
            if (need_mode) return real_open(path, flags, mode);
            else return real_open(path, flags);
        }
    }

    errno = EIO;
    return -1;
}

/* openat */
int openat(int dirfd, const char *path, int flags, ...) {
    pthread_once(&init_once, init_once_fn);

    mode_t mode = 0;
    int need_mode = (flags & O_CREAT) != 0;
    if (need_mode) {
        va_list ap; va_start(ap, flags); mode = (mode_t)va_arg(ap, int); va_end(ap);
    }

    if (!target_device || strcmp(path, target_device) != 0) {
        if (real_openat) {
            if (need_mode) return real_openat(dirfd, path, flags, mode);
            else return real_openat(dirfd, path, flags);
        } else {
            if (need_mode) return real_open(path, flags, mode);
            else return real_open(path, flags);
        }
    }

    char pty_path[256];
    if (connect_and_get_pty(path, pty_path, sizeof(pty_path)) == 0) {
        int fd = need_mode ? real_open(pty_path, flags, mode) : real_open(pty_path, flags);
        if (fd >= 0) add_mapping(fd);
        return fd;
    }

    if (allow_fallback) {
        if (real_openat) {
            if (need_mode) return real_openat(dirfd, path, flags, mode);
            else return real_openat(dirfd, path, flags);
        } else {
            if (need_mode) return real_open(path, flags, mode);
            else return real_open(path, flags);
        }
    }

    errno = EIO;
    return -1;
}

/* creat */
int creat(const char *path, mode_t mode) {
    pthread_once(&init_once, init_once_fn);
    return open(path, O_CREAT | O_WRONLY | O_TRUNC, mode);
}

/* fopen */
FILE *fopen(const char *path, const char *mode_str) {
    pthread_once(&init_once, init_once_fn);

    if (!target_device || strcmp(path, target_device) != 0) {
        if (real_fopen) return real_fopen(path, mode_str);
        int fd = real_open(path, fopen_mode_to_flags(mode_str), 0666);
        if (fd < 0) return NULL;
        FILE *f = real_fdopen(fd, mode_str);
        if (!f) { real_close(fd); return NULL; }
        return f;
    }

    char pty_path[256];
    if (connect_and_get_pty(path, pty_path, sizeof(pty_path)) == 0) {
        int flags = fopen_mode_to_flags(mode_str);
        int fd = real_open(pty_path, flags, 0666);
        if (fd < 0) {
            if (allow_fallback) {
                int fd2 = real_open(path, flags, 0666);
                if (fd2 < 0) return NULL;
                FILE *f2 = real_fdopen(fd2, mode_str);
                if (!f2) { real_close(fd2); return NULL; }
                return f2;
            }
            return NULL;
        }
        add_mapping(fd);
        FILE *f = real_fdopen(fd, mode_str);
        if (!f) { remove_mapping(fd); real_close(fd); return NULL; }
        return f;
    }

    if (allow_fallback) {
        if (real_fopen) return real_fopen(path, mode_str);
        int fd = real_open(path, fopen_mode_to_flags(mode_str), 0666);
        if (fd < 0) return NULL;
        FILE *f = real_fdopen(fd, mode_str);
        if (!f) { real_close(fd); return NULL; }
        return f;
    }

    errno = EIO;
    return NULL;
}

/* freopen (path != NULL simplified handling) */
FILE *freopen(const char *path, const char *mode_str, FILE *stream) {
    pthread_once(&init_once, init_once_fn);
    if (!path) {
        if (real_freopen) return real_freopen(path, mode_str, stream);
        errno = EINVAL;
        return NULL;
    }
    #pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Wnonnull-compare"
    if (stream) {
        int oldfd = fileno(stream);
        if (oldfd >= 0) remove_mapping(oldfd);
        if (real_fclose) real_fclose(stream);
        else fclose(stream);
    }
    #pragma GCC diagnostic pop
    return fopen(path, mode_str);
}

/* close */
int close(int fd) {
    pthread_once(&init_once, init_once_fn);
    remove_mapping(fd);
    return real_close(fd);
}

/* dup */
int dup(int oldfd) {
    pthread_once(&init_once, init_once_fn);
    int newfd = real_dup(oldfd);
    if (newfd >= 0 && is_mapped(oldfd)) add_mapping(newfd);
    return newfd;
}

/* dup2 */
int dup2(int oldfd, int newfd) {
    pthread_once(&init_once, init_once_fn);
    int r = real_dup2(oldfd, newfd);
    if (r >= 0) {
        if (is_mapped(oldfd)) add_mapping(newfd);
        else remove_mapping(newfd);
    }
    return r;
}

/* dup3 */
int dup3(int oldfd, int newfd, int flags) {
    pthread_once(&init_once, init_once_fn);
    int r = -1;
    if (real_dup3) r = real_dup3(oldfd, newfd, flags);
    else r = real_dup2(oldfd, newfd);
    if (r >= 0) {
        if (is_mapped(oldfd)) add_mapping(newfd);
        else remove_mapping(newfd);
    }
    return r;
}

/* fcntl (handle dup operations) */
int fcntl(int fd, int cmd, ...) {
    pthread_once(&init_once, init_once_fn);
    va_list ap;
    va_start(ap, cmd);
    int arg = va_arg(ap, int);
    va_end(ap);

    if (cmd == F_DUPFD || cmd == F_DUPFD_CLOEXEC) {
        int newfd = real_fcntl(fd, cmd, arg);
        if (newfd >= 0 && is_mapped(fd)) add_mapping(newfd);
        return newfd;
    }

    // pass-through for other commands (note: does not handle pointer args)
    return real_fcntl(fd, cmd, arg);
}

/* ioctl wrapper: forward ioctls to daemon when fd mapped */
int ioctl(int fd, unsigned long request, ...) {
    pthread_once(&init_once, init_once_fn);
    va_list ap;
    va_start(ap, request);
    void *argp = va_arg(ap, void*);
    va_end(ap);

    return send_request(REQ_IOCTL, fd, request, argp);
}

/* tcflush wrapper */
int tcflush(int fd, int queue_selector) {
    pthread_once(&init_once, init_once_fn);
    return send_request(REQ_TCFLSH, fd, 0, &queue_selector);
}

/* tcsendbreak wrapper */
int tcsendbreak(int fd, int duration) {
    pthread_once(&init_once, init_once_fn);
    return send_request(REQ_TCSENDBREAK, fd, 0, &duration);
}

/* tcdrain wrapper */
int tcdrain(int fd) {
    pthread_once(&init_once, init_once_fn);
    return send_request(REQ_TCDRAIN, fd, 0, NULL);
}

static int send_request(int req_type, int fd, unsigned long request, void *argp) {
    if (!is_mapped(fd) || !isatty(fd)) {
        if (req_type == REQ_IOCTL) return passthrough_ioctl(fd, request, argp);
        if (req_type == REQ_TCFLSH) return real_tcflush(fd, *(int*)argp);
        if (req_type == REQ_TCSENDBREAK) return real_tcsendbreak(fd, *(int*)argp);
        if (req_type == REQ_TCDRAIN) return real_tcdrain(fd);
        errno = EINVAL;
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    int sock = connect_with_timeout(&addr, sizeof(addr), CONNECT_TIMEOUT_MS);
    if (sock < 0) {
        if (allow_fallback) {
            if (req_type == REQ_IOCTL) return passthrough_ioctl(fd, request, argp);
            if (req_type == REQ_TCFLSH) return real_tcflush(fd, *(int*)argp);
            if (req_type == REQ_TCSENDBREAK) return real_tcsendbreak(fd, *(int*)argp);
            if (req_type == REQ_TCDRAIN) return real_tcdrain(fd);
        }
        errno = EIO;
        return -1;
    }

    struct sm_header hdr;
    memcpy(hdr.magic, SM_MAGIC, sizeof(SM_MAGIC));
    hdr.type = req_type;
    hdr.payload_len = 0;

    struct sm_ioctl_req ioctl_req;
    if (req_type == REQ_IOCTL) {
        ioctl_req.request = request;
        size_t arg_size = _IOC_SIZE(request);
        int dir = _IOC_DIR(request);

        switch (request) {
            case TIOCMBIS:
            case TIOCMBIC:
            case TIOCMSET:
                ioctl_req.arg_type = ARG_VALUE;
                ioctl_req.arg_len = sizeof(int);
                hdr.payload_len = sizeof(ioctl_req) + sizeof(int);
                break;
            default:
                if (argp && (dir & _IOC_WRITE)) {
                    ioctl_req.arg_type = ARG_BUFFER;
                    ioctl_req.arg_len = arg_size;
                    hdr.payload_len = sizeof(ioctl_req) + ioctl_req.arg_len;
                } else {
                    ioctl_req.arg_type = ARG_NONE;
                    ioctl_req.arg_len = 0;
                    hdr.payload_len = sizeof(ioctl_req);
                }
                break;
        }
    }

    if (send_all(sock, &hdr, sizeof(hdr)) != sizeof(hdr)) {
        close(sock);
        errno = EIO;
        return -1;
    }

    if (req_type == REQ_IOCTL) {
        if (send_all(sock, &ioctl_req, sizeof(ioctl_req)) != sizeof(ioctl_req)) {
            close(sock);
            errno = EIO;
            return -1;
        }
        if (ioctl_req.arg_type == ARG_BUFFER && ioctl_req.arg_len > 0) {
            if (send_all(sock, argp, ioctl_req.arg_len) != (ssize_t)ioctl_req.arg_len) {
                close(sock);
                errno = EIO;
                return -1;
            }
        } else if (ioctl_req.arg_type == ARG_VALUE) {
            int val = (int)(intptr_t)argp;
            if (send_all(sock, &val, sizeof(int)) != sizeof(int)) {
                close(sock);
                errno = EIO;
                return -1;
            }
        }
    } else {
        if (argp != NULL) {
             if (send_all(sock, argp, sizeof(int)) != sizeof(int)) {
                close(sock);
                errno = EIO;
                return -1;
            }
        }
    }

    struct sm_response resp;
    if (recv_all(sock, &resp, sizeof(resp)) != sizeof(resp)) {
        close(sock);
        errno = EIO;
        return -1;
    }

    if (resp.rc < 0) {
        errno = resp.errno_val;
        close(sock);
        return -1;
    }

    if (resp.payload_len > 0 && argp) {
        if (recv_all(sock, argp, resp.payload_len) != (ssize_t)resp.payload_len) {
            close(sock);
            errno = EIO;
            return -1;
        }
    }

    close(sock);
    return resp.rc;
}
