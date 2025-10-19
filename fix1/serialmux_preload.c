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
#include <stdarg.h> // For va_list
#include <errno.h>

#define SOCKET_PATH "/tmp/serialmux.sock"
#define DAEMON_DEVICE_ENV "SERIALMUX_DEVICE" // e.g., /dev/ttyUSB0
#define MAX_MAPPED_FDS 256

// Stores info about FDs we've mapped to a PTY
typedef struct {
    int pty_fd;
    pid_t pid;
} FDMapping;

static FDMapping fd_map[MAX_MAPPED_FDS];
static int num_mapped = 0;
static pthread_mutex_t map_lock = PTHREAD_MUTEX_INITIALIZER;
static char *target_device = NULL;

// Real libc functions
static int (*real_open)(const char *, int, ...) = NULL;
static int (*real_close)(int) = NULL;

void init_real_funcs() {
    if (real_open) return;
    
    real_open = dlsym(RTLD_NEXT, "open");
    real_close = dlsym(RTLD_NEXT, "close");
    target_device = getenv(DAEMON_DEVICE_ENV);
    
    if (!real_open || !real_close) {
        fprintf(stderr, "SERIALMUX_PRELOAD: Failed to get real libc functions\n");
        exit(1);
    }

    if (!target_device) {
        fprintf(stderr, "SERIALMUX_PRELOAD: ERROR! %s environment variable not set.\n", DAEMON_DEVICE_ENV);
        // We can continue, but we won't intercept anything.
    }
}

// Connects to daemon, sends OPEN, and gets PTY path back
int connect_and_get_pty(const char *device, char *pty_path_buf, size_t buf_size) {
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("SERIALMUX_PRELOAD: socket");
        return -1;
    }
    
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);
    
    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "SERIALMUX_PRELOAD: Failed to connect to serialmux daemon. Is it running?\n");
        close(sock);
        return -1;
    }
    
    // Send device name and priority
    const char *priority_str = getenv("SERIALMUX_PRIORITY");
    int prio = (priority_str && strcmp(priority_str, "HIGH") == 0) ? 1 : 0;
    
    char msg[512];
    snprintf(msg, sizeof(msg), "OPEN:%s:%d:%d", device, prio, getpid());
    
    if (write(sock, msg, strlen(msg) + 1) < 0) {
        perror("SERIALMUX_PRELOAD: write to daemon");
        close(sock);
        return -1;
    }
    
    // Read PTY path response
    ssize_t n = read(sock, pty_path_buf, buf_size - 1);
    if (n <= 0) {
        fprintf(stderr, "SERIALMUX_PRELOAD: No response from daemon\n");
        close(sock);
        return -1;
    }
    
    pty_path_buf[n] = '\0';
    
    if (strncmp(pty_path_buf, "ERROR:", 6) == 0) {
        fprintf(stderr, "SERIALMUX_PRELOAD: Daemon error: %s\n", pty_path_buf + 6);
        close(sock);
        return -1;
    }

    close(sock);
    return 0;
}

// Intercept open()
int open(const char *path, int flags, ...) {
    init_real_funcs();
    
    va_list args;
    va_start(args, flags);
    int mode = va_arg(args, int);
    va_end(args);
    
    // Check if this is the device we're supposed to intercept
    if (target_device && strcmp(path, target_device) == 0) {
        
        char pty_path[256];
        if (connect_and_get_pty(path, pty_path, sizeof(pty_path)) == 0) {
            
            // Success! Open the PTY instead of the real device
            int pty_fd = real_open(pty_path, flags, mode);
            if (pty_fd < 0) {
                perror("SERIALMUX_PRELOAD: Failed to open PTY device");
                return -1;
            }

            // Store this FD for intercepting close()
            pthread_mutex_lock(&map_lock);
            if (num_mapped < MAX_MAPPED_FDS) {
                fd_map[num_mapped].pty_fd = pty_fd;
                fd_map[num_mapped].pid = getpid();
                num_mapped++;
            }
            pthread_mutex_unlock(&map_lock);
            
            return pty_fd;
        } else {
            // Failed to connect, return error
            errno = EIO;
            return -1;
        }
    }
    
    // Not our device, pass through to real open()
    return real_open(path, flags, mode);
}

// Intercept close()
int close(int fd) {
    init_real_funcs();
    
    pthread_mutex_lock(&map_lock);
    
    int found_idx = -1;
    for (int i = 0; i < num_mapped; i++) {
        if (fd_map[i].pty_fd == fd) {
            found_idx = i;
            break;
        }
    }
    
    if (found_idx != -1) {
        // This is one of our PTYs. We don't need to send a CLOSE message,
        // the daemon will detect the PTY closing.
        // We just need to remove it from our map.
        
        // Remove from mapping
        fd_map[found_idx] = fd_map[num_mapped - 1];
        num_mapped--;
    }
    
    pthread_mutex_unlock(&map_lock);
    
    // Pass through to real close()
    return real_close(fd);
}
