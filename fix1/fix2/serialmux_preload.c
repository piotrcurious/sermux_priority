// serialmux_preload.c - LD_PRELOAD library for intercepting serial port access
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

#define SOCKET_PATH "/tmp/serialmux.sock"
#define DAEMON_DEVICE_ENV "SERIALMUX_DEVICE" // e.g., /dev/ttyUSB0
#define MAX_MAPPED_FDS 512

typedef struct {
    int fd;      // intercepted fd in this process (key)
    pid_t pid;   // pid of our process (for debugging / future use)
} FDMapping;

static FDMapping fd_map[MAX_MAPPED_FDS];
static int num_mapped = 0;
static pthread_mutex_t map_lock = PTHREAD_MUTEX_INITIALIZER;
static char *target_device = NULL;
static int allow_fallback = 0; // controlled by env SERIALMUX_FALLBACK

// real libc functions (populate via dlsym)
static int (*real_open)(const char *, int, ...) = NULL;
static int (*real_open64)(const char *, int, ...) = NULL;
static int (*real_openat)(int, const char *, int, ...) = NULL;
static int (*real_close)(int) = NULL;
static int (*real_dup)(int) = NULL;
static int (*real_dup2)(int, int) = NULL;
static int (*real_dup3)(int, int, int) = NULL;
static int (*real_fcntl)(int, int, ...) = NULL;

// initialization guard
static pthread_once_t init_once = PTHREAD_ONCE_INIT;

static void debug_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "SERIALMUX_PRELOAD: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

static void add_mapping(int fd) {
    if (fd < 0) return;
    pthread_mutex_lock(&map_lock);
    // avoid duplicates
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

// safe send all bytes (MSG_NOSIGNAL to avoid SIGPIPE)
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

// simple recv - read up to buf_size-1 and terminate
static ssize_t recv_some(int sock, char *buf, size_t buf_size) {
    ssize_t n = recv(sock, buf, buf_size - 1, 0);
    if (n > 0) buf[n] = '\0';
    return n;
}

static void init_real_funcs_once(void) {
    dlerror();
    real_open = dlsym(RTLD_NEXT, "open");
    real_open64 = dlsym(RTLD_NEXT, "open64");
    real_openat = dlsym(RTLD_NEXT, "openat");
    real_close = dlsym(RTLD_NEXT, "close");
    real_dup = dlsym(RTLD_NEXT, "dup");
    real_dup2 = dlsym(RTLD_NEXT, "dup2");
    real_dup3 = dlsym(RTLD_NEXT, "dup3");
    real_fcntl = dlsym(RTLD_NEXT, "fcntl");

    // read env vars for configuration
    char *env = getenv(DAEMON_DEVICE_ENV);
    if (env) target_device = strdup(env);
    char *fb = getenv("SERIALMUX_FALLBACK");
    if (fb && strcmp(fb, "1") == 0) allow_fallback = 1;

    if (!real_open || !real_close || !real_dup || !real_dup2 || !real_fcntl) {
        debug_log("dlsym failed to find required libc functions: %s", dlerror());
        // not fatal here; calls will segfault later — but better to notify
    }
}

// Connects to daemon, sends OPEN, gets PTY path back
static int connect_and_get_pty(const char *device, char *pty_path_buf, size_t buf_size) {
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        // daemon likely not running
        close(sock);
        return -1;
    }

    // priority from env
    const char *priority_str = getenv("SERIALMUX_PRIORITY");
    int prio = (priority_str && strcmp(priority_str, "HIGH") == 0) ? 1 : 0;

    char msg[512];
    int len = snprintf(msg, sizeof(msg), "OPEN:%s:%d:%d", device, prio, (int)getpid());
    if (len < 0 || (size_t)len >= sizeof(msg)) {
        close(sock);
        return -1;
    }

    if (send_all(sock, msg, (size_t)len + 1) < 0) { // include terminating NUL to match daemon expectation
        close(sock);
        return -1;
    }

    ssize_t n = recv_some(sock, pty_path_buf, buf_size);
    close(sock);
    if (n <= 0) return -1;
    if (strncmp(pty_path_buf, "ERROR:", 6) == 0) return -1;
    return 0;
}

/* --- wrappers --- */

/* open */
int open(const char *path, int flags, ...) {
    pthread_once(&init_once, init_real_funcs_once);

    va_list ap;
    mode_t mode = 0;
    int need_mode = (flags & O_CREAT) != 0;
    if (need_mode) {
        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
    }

    if (!target_device || strcmp(path, target_device) != 0) {
        // pass-through
        if (need_mode) return real_open(path, flags, mode);
        else return real_open(path, flags);
    }

    // try to get PTY from daemon
    char pty_path[256];
    if (connect_and_get_pty(path, pty_path, sizeof(pty_path)) == 0) {
        int fd;
        if (need_mode) fd = real_open(pty_path, flags, mode);
        else fd = real_open(pty_path, flags);
        if (fd >= 0) add_mapping(fd);
        return fd;
    }

    // failed to contact daemon: fallback or error
    if (allow_fallback) {
        debug_log("daemon unreachable, falling back to real device open(%s)", path);
        if (need_mode) return real_open(path, flags, mode);
        else return real_open(path, flags);
    }

    errno = EIO;
    return -1;
}

/* open64 (if present) */
int open64(const char *path, int flags, ...) {
    pthread_once(&init_once, init_real_funcs_once);

    va_list ap;
    mode_t mode = 0;
    int need_mode = (flags & O_CREAT) != 0;
    if (need_mode) {
        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
    }

    if (!target_device || strcmp(path, target_device) != 0) {
        if (real_open64) {
            if (need_mode) return real_open64(path, flags, mode);
            else return real_open64(path, flags);
        }
        // fallback to open()
        if (need_mode) return real_open(path, flags, mode);
        else return real_open(path, flags);
    }

    char pty_path[256];
    if (connect_and_get_pty(path, pty_path, sizeof(pty_path)) == 0) {
        int fd;
        if (need_mode && real_open64) fd = real_open64(pty_path, flags, mode);
        else if (need_mode) fd = real_open(pty_path, flags, mode);
        else fd = real_open(pty_path, flags);
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
    pthread_once(&init_once, init_real_funcs_once);

    va_list ap;
    mode_t mode = 0;
    int need_mode = (flags & O_CREAT) != 0;
    if (need_mode) {
        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
    }

    // Only intercept absolute or matching path; otherwise pass-through
    if (!target_device || strcmp(path, target_device) != 0) {
        if (real_openat) {
            if (need_mode) return real_openat(dirfd, path, flags, mode);
            else return real_openat(dirfd, path, flags);
        } else {
            // fallback: use open + ignore dirfd
            if (need_mode) return real_open(path, flags, mode);
            else return real_open(path, flags);
        }
    }

    char pty_path[256];
    if (connect_and_get_pty(path, pty_path, sizeof(pty_path)) == 0) {
        int fd;
        if (need_mode) fd = real_open(pty_path, flags, mode);
        else fd = real_open(pty_path, flags);
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

/* close */
int close(int fd) {
    pthread_once(&init_once, init_real_funcs_once);

    // remove mapping if present
    remove_mapping(fd);

    return real_close(fd);
}

/* dup */
int dup(int oldfd) {
    pthread_once(&init_once, init_real_funcs_once);
    int newfd = real_dup(oldfd);
    if (newfd >= 0 && is_mapped(oldfd)) add_mapping(newfd);
    return newfd;
}

/* dup2 */
int dup2(int oldfd, int newfd) {
    pthread_once(&init_once, init_real_funcs_once);
    int r = real_dup2(oldfd, newfd);
    if (r >= 0) {
        // if oldfd is mapped, ensure newfd is mapped too
        if (is_mapped(oldfd)) add_mapping(newfd);
        else remove_mapping(newfd); // ensure mapping cleaned up
    }
    return r;
}

/* dup3 */
int dup3(int oldfd, int newfd, int flags) {
    pthread_once(&init_once, init_real_funcs_once);
    int r = -1;
    if (real_dup3) r = real_dup3(oldfd, newfd, flags);
    else r = real_dup2(oldfd, newfd); // fallback if dup3 missing (ignores flags)
    if (r >= 0) {
        if (is_mapped(oldfd)) add_mapping(newfd);
        else remove_mapping(newfd);
    }
    return r;
}

/* fcntl - handle F_DUPFD / F_DUPFD_CLOEXEC */
int fcntl(int fd, int cmd, ...) {
    pthread_once(&init_once, init_real_funcs_once);

    va_list ap;
    va_start(ap, cmd);
    int arg = va_arg(ap, int);
    va_end(ap);

    if (cmd == F_DUPFD || cmd == F_DUPFD_CLOEXEC) {
        // call real_fcntl and if old fd mapped, add mapping for new fd
        int newfd = real_fcntl(fd, cmd, arg);
        if (newfd >= 0 && is_mapped(fd)) add_mapping(newfd);
        return newfd;
    }

    // Default pass-through: call with the provided arg
    // Note: This is a simplified pass-through. For commands with pointer args, more handling is needed.
    return real_fcntl(fd, cmd, arg);
}
