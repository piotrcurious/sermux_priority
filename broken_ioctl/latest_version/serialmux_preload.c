// serialmux_preload.c
// LD_PRELOAD library for serialmux (Linux) with ioctl/tc* forwarding
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <termios.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdint.h>
#include <poll.h>
#include <sys/time.h>
#include <limits.h>
#include <sys/sysmacros.h>   /* major/minor */
#include <sys/syscall.h>     /* SYS_ioctl */
#include <sys/uio.h>

#define SOCKET_PATH "/tmp/serialmux.sock"
#define DAEMON_DEVICE_ENV "SERIALMUX_DEVICE"
#define SERIALMUX_FALLBACK_ENV "SERIALMUX_FALLBACK"
#define SERIALMUX_DEBUG_ENV "SERIALMUX_DEBUG"
#define MAX_MAPPED_FDS 1024
#define CONNECT_TIMEOUT_MS 200
#define SM_MAGIC "SMIO"
#define SM_MAGIC_LEN 4

/* Request types */
enum {
    REQ_IOCTL = 1,
    REQ_TCFLUSH,
    REQ_TCSENDBREAK,
    REQ_TCDRAIN
};

/* ioctl argument types */
typedef enum {
    ARG_NONE = 0, /* no third argument */
    ARG_VALUE,    /* integer value */
    ARG_BUFFER    /* pointer to buffer */
} ArgType;

typedef struct {
    int fd;
    pid_t pid;
} FDMapping;

static FDMapping fd_map[MAX_MAPPED_FDS];
static int num_mapped = 0;
static pthread_mutex_t map_lock = PTHREAD_MUTEX_INITIALIZER;
static char *target_device = NULL;     /* from env */
static char resolved_target[PATH_MAX]; /* resolved path for stat */
static dev_t target_rdev = 0;
static int allow_fallback = 0;
static int debug_enabled = 0;

static pthread_once_t init_once = PTHREAD_ONCE_INIT;

/* libc function pointers (kept for wrappers where needed) */
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

/* Runtime debug helper (enabled by SERIALMUX_DEBUG=1) */
static void debug_log(const char *fmt, ...) {
    if (!debug_enabled) return;
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
        debug_log("add_mapping(fd=%d) total=%d", fd, num_mapped);
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
        debug_log("remove_mapping(fd=%d) total=%d", fd, num_mapped);
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

/* send request and read response. returns 0 on IO success, sets out_errno/out_rc. */
static int send_request_and_get_response(int req_type,
                                         uint64_t ioctl_req, ArgType arg_type,
                                         const void *arg_data, uint32_t arglen,
                                         int *out_errno, int *out_rc,
                                         void *out_buf, uint32_t out_buf_capacity,
                                         uint32_t *out_arglen) {
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    int sock = connect_with_timeout(&addr, sizeof(addr), CONNECT_TIMEOUT_MS);
    if (sock < 0) return -1;

    // Send magic and request type
    if (send_all(sock, SM_MAGIC, SM_MAGIC_LEN) < 0) { close(sock); return -1; }
    uint32_t t = (uint32_t)req_type;
    if (send_all(sock, &t, sizeof(t)) < 0) { close(sock); return -1; }

    // Send payload based on request type
    if (req_type == REQ_IOCTL) {
        if (send_all(sock, &ioctl_req, sizeof(ioctl_req)) < 0) { close(sock); return -1; }
        uint32_t at = (uint32_t)arg_type;
        if (send_all(sock, &at, sizeof(at)) < 0) { close(sock); return -1; }
        if (send_all(sock, &arglen, sizeof(arglen)) < 0) { close(sock); return -1; }
        if (arglen > 0) {
            if (send_all(sock, arg_data, arglen) < 0) { close(sock); return -1; }
        }
    } else if (req_type == REQ_TCFLUSH || req_type == REQ_TCSENDBREAK) {
        // These requests send a single integer argument
        if (send_all(sock, arg_data, sizeof(int32_t)) < 0) { close(sock); return -1; }
    } else if (req_type == REQ_TCDRAIN) {
        // No payload
    }

    int32_t rc = 0;
    int32_t err_no = 0;
    uint32_t returned_len = 0;

    if (recv_all(sock, &rc, sizeof(rc)) < 0) { close(sock); return -1; }
    if (recv_all(sock, &err_no, sizeof(err_no)) < 0) { close(sock); return -1; }
    if (recv_all(sock, &returned_len, sizeof(returned_len)) < 0) { close(sock); return -1; }

    // read returned bytes (if any)
    if (returned_len > 0) {
        if (out_buf && out_buf_capacity > 0) {
            uint32_t to_copy = (returned_len > out_buf_capacity) ? out_buf_capacity : returned_len;
            if (recv_all(sock, out_buf, to_copy) < 0) { close(sock); return -1; }
            // drain remainder if our buffer was too small
            if (returned_len > to_copy) {
                uint32_t left = returned_len - to_copy;
                char drain[256];
                while (left > 0) {
                    uint32_t chunk = left > sizeof(drain) ? sizeof(drain) : left;
                    if (recv_all(sock, drain, chunk) < 0) { close(sock); return -1; }
                    left -= chunk;
                }
            }
        } else {
            // no buffer provided: drain and discard all returned data
            uint32_t left = returned_len;
            char drain[256];
            while (left > 0) {
                uint32_t chunk = left > sizeof(drain) ? sizeof(drain) : left;
                if (recv_all(sock, drain, chunk) < 0) { close(sock); return -1; }
                left -= chunk;
            }
        }
    }

    close(sock);
    if (out_errno) *out_errno = (int)err_no;
    if (out_rc) *out_rc = (int)rc;
    if (out_arglen) *out_arglen = returned_len;
    return 0;
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
    if (env) {
        target_device = strdup(env);
        /* resolve symlink if possible */
        if (realpath(target_device, resolved_target) == NULL) {
            /* realpath failed: copy raw path */
            strncpy(resolved_target, target_device, sizeof(resolved_target) - 1);
            resolved_target[sizeof(resolved_target) - 1] = '\0';
        }
        struct stat st;
        if (stat(resolved_target, &st) == 0 && S_ISCHR(st.st_mode)) {
            target_rdev = st.st_rdev;
            debug_log("Resolved SERIALMUX_DEVICE=%s rdev=%u:%u", resolved_target, (unsigned)major(target_rdev), (unsigned)minor(target_rdev));
        } else {
            debug_log("SERIALMUX_DEVICE %s is not a character device or stat failed", resolved_target);
            target_rdev = 0;
        }
    }
    char *fb = getenv(SERIALMUX_FALLBACK_ENV);
    if (fb && strcmp(fb, "1") == 0) allow_fallback = 1;

    /* optional runtime debug env */
    if (getenv(SERIALMUX_DEBUG_ENV)) debug_enabled = 1;

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

/* passthrough ioctl (call kernel directly via syscall for robustness) */
static int passthrough_ioctl(int fd, unsigned long request, void *argp) {
    /* syscall invokes kernel ioctl directly, avoiding dlsym symbol-resolution edge cases */
    long rc = syscall(SYS_ioctl, fd, request, argp);
    if (rc == -1) {
        /* errno set by syscall */
        return -1;
    }
    return (int)rc;
}

/* Open PTY with retry on ENOENT, for up to ~200ms */
static int open_pty_with_retry(const char *pty_path, int flags, int need_mode, mode_t mode) {
    int fd = -1;
    for (int i = 0; i < 20; i++) {
        fd = need_mode ? real_open(pty_path, flags, mode) : real_open(pty_path, flags);
        if (fd >= 0) {
            break;
        }
        if (errno != ENOENT) {
            break;
        }
        usleep(10000);  // 10ms
    }
    return fd;
}

/* wrapper implementations (open/open64/openat/fopen/creat/etc) */
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
        int fd = open_pty_with_retry(pty_path, flags, need_mode, mode);
        if (fd >= 0) {
            add_mapping(fd);
        }
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
        int fd = open_pty_with_retry(pty_path, flags, need_mode, mode);
        if (fd >= 0) {
            add_mapping(fd);
        }
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
        int fd = open_pty_with_retry(pty_path, flags, need_mode, mode);
        if (fd >= 0) {
            add_mapping(fd);
        }
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
        int fd = open_pty_with_retry(pty_path, flags, (flags & O_CREAT) != 0, 0666);
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
    int oldfd = fileno(stream);
    if (oldfd >= 0) remove_mapping(oldfd);
    if (real_fclose) real_fclose(stream);
    else fclose(stream);
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


/* ioctl wrapper: forward all ioctls for our managed FDs to the daemon */
int ioctl(int fd, unsigned long request, ...) {
    pthread_once(&init_once, init_once_fn);

    void *argp = NULL;
    va_list ap;
    va_start(ap, request);
    argp = va_arg(ap, void *);
    va_end(ap);

    if (!is_mapped(fd)) {
        return passthrough_ioctl(fd, request, argp);
    }
    /* The check 'isatty' is important because it prevents us from hijacking
     * ioctls on non-terminal devices that might share the same fd number,
     * such as network sockets. This was a significant issue in a previous
     * version of this software.
     */
    if (!isatty(fd)) {
        debug_log("passthrough ioctl on non-tty fd=%d req=0x%lx", fd, request);
        return passthrough_ioctl(fd, request, argp);
    }

    debug_log("forwarding ioctl fd=%d req=0x%lx", fd, request);

    ArgType arg_type;
    const void *arg_data = NULL;
    uint32_t arglen = 0;
    size_t ioctl_size = _IOC_SIZE(request);

    unsigned long dir = _IOC_DIR(request);

    if (argp == NULL) {
        arg_type = ARG_NONE;
        arg_data = NULL;
        arglen = 0;
    } else {
        // Default to ARG_BUFFER as it's the most common and safest case for TTY ioctls.
        // The fatal error is misinterpreting a pointer as a value.
        arg_type = ARG_BUFFER;
        arg_data = argp;
        arglen = (uint32_t)ioctl_size;

        // For old-style ioctls (like TIOCMBIS) or new ioctls with pointer
        // types, the size can be encoded as 0. We default to sizeof(int)
        // as this is correct for most modem/control line ioctls.
        if (arglen == 0 && dir != _IOC_NONE) {
            arglen = sizeof(int);
        } else if (dir == _IOC_NONE) {
            // This case handles legacy ioctls like FIONREAD (0x541B) where
            // direction info is missing but a pointer argument is expected.
            arglen = sizeof(int);
        }
    }

    // Output buffer for READ ioctls
    unsigned char outbuf[4096];
    uint32_t got_len = 0;
    int daemon_errno = 0, out_rc = 0;

    // For read-only ioctls, arg_data points to user memory but may contain
    // stale data. We send a zeroed buffer to the daemon to prevent issues.
    unsigned char dummy_buf[arglen];
    if (dir == _IOC_READ && arglen > 0) {
        memset(dummy_buf, 0, arglen);
        arg_data = dummy_buf;
    }

    if (send_request_and_get_response(REQ_IOCTL, (uint64_t)request, arg_type, arg_data, arglen,
                                      &daemon_errno, &out_rc, outbuf, sizeof(outbuf), &got_len) < 0) {
        if (allow_fallback) {
            debug_log("daemon call failed, falling back to real ioctl fd=%d", fd);
            return passthrough_ioctl(fd, request, argp);
        }
        errno = EIO;
        return -1;
    }

    if (out_rc < 0) {
        errno = daemon_errno;
        return -1;
    }

    if (argp && got_len > 0 && (_IOC_DIR(request) & _IOC_READ)) {
        size_t to_copy = (got_len > ioctl_size) ? ioctl_size : got_len;
        memcpy(argp, outbuf, to_copy);
    }

    return out_rc;
}

/* End of file */
/* tcflush wrapper */
int tcflush(int fd, int queue_selector) {
    pthread_once(&init_once, init_once_fn);
    if (!is_mapped(fd)) {
        return real_tcflush(fd, queue_selector);
    }

    debug_log("forwarding tcflush(fd=%d, sel=%d)\n", fd, queue_selector);
    int arg = queue_selector;
    int daemon_errno = 0, out_rc = 0;

    if (send_request_and_get_response(REQ_TCFLUSH, 0, ARG_NONE, &arg, sizeof(arg), &daemon_errno, &out_rc, NULL, 0, NULL) < 0) {
        if (allow_fallback) {
            return real_tcflush(fd, queue_selector);
        }
        errno = EIO;
        return -1;
    }
    if (out_rc < 0) {
        errno = daemon_errno;
        return -1;
    }
    return out_rc;
}

/* tcsendbreak wrapper */
int tcsendbreak(int fd, int duration) {
    pthread_once(&init_once, init_once_fn);
    if (!is_mapped(fd)) {
        return real_tcsendbreak(fd, duration);
    }

    debug_log("forwarding tcsendbreak(fd=%d, dur=%d)\n", fd, duration);
    int arg = duration;
    int daemon_errno = 0, out_rc = 0;

    if (send_request_and_get_response(REQ_TCSENDBREAK, 0, ARG_NONE, &arg, sizeof(arg), &daemon_errno, &out_rc, NULL, 0, NULL) < 0) {
        if (allow_fallback) {
            return real_tcsendbreak(fd, duration);
        }
        errno = EIO;
        return -1;
    }
    if (out_rc < 0) {
        errno = daemon_errno;
        return -1;
    }
    return out_rc;
}

/* tcdrain wrapper */
int tcdrain(int fd) {
    pthread_once(&init_once, init_once_fn);
    if (!is_mapped(fd)) {
        return real_tcdrain(fd);
    }

    debug_log("forwarding tcdrain(fd=%d)\n", fd);
    int daemon_errno = 0, out_rc = 0;

    if (send_request_and_get_response(REQ_TCDRAIN, 0, ARG_NONE, NULL, 0, &daemon_errno, &out_rc, NULL, 0, NULL) < 0) {
        if (allow_fallback) {
            return real_tcdrain(fd);
        }
        errno = EIO;
        return -1;
    }
    if (out_rc < 0) {
        errno = daemon_errno;
        return -1;
    }
    return out_rc;
}
