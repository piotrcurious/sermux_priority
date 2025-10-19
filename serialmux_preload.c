// serialmux_preload.c - LD_PRELOAD library for intercepting serial port access
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

#define SOCKET_PATH "/tmp/serialmux.sock"

// Stores info about opened serial ports
typedef struct {
    int original_fd;
    int proxy_fd;
    char device[256];
    int is_serial;
} FDMapping;

static FDMapping fd_map[256];
static int num_mapped = 0;
static pthread_mutex_t map_lock = PTHREAD_MUTEX_INITIALIZER;

// Real libc functions
static int (*real_open)(const char *, int, ...) = NULL;
static int (*real_close)(int) = NULL;
static ssize_t (*real_read)(int, void *, size_t) = NULL;
static ssize_t (*real_write)(int, const void *, size_t) = NULL;
static int (*real_ioctl)(int, unsigned long, ...) = NULL;

void init_real_funcs() {
    if (real_open) return;
    
    real_open = dlsym(RTLD_NEXT, "open");
    real_close = dlsym(RTLD_NEXT, "close");
    real_read = dlsym(RTLD_NEXT, "read");
    real_write = dlsym(RTLD_NEXT, "write");
    real_ioctl = dlsym(RTLD_NEXT, "ioctl");
    
    if (!real_open || !real_close || !real_read || !real_write || !real_ioctl) {
        fprintf(stderr, "Failed to get real libc functions\n");
        exit(1);
    }
}

int is_serial_device(const char *path) {
    return strstr(path, "/dev/tty") != NULL || 
           strstr(path, "/dev/cu.") != NULL;
}

int connect_to_daemon(const char *device) {
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        return -1;
    }
    
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);
    
    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "Failed to connect to serialmux daemon. Start it first!\n");
        close(sock);
        return -1;
    }
    
    // Send device name and priority
    const char *priority = getenv("SERIALMUX_PRIORITY");
    int prio = (priority && strcmp(priority, "HIGH") == 0) ? 1 : 0;
    
    char msg[512];
    snprintf(msg, sizeof(msg), "OPEN:%s:%d:%d", device, prio, getpid());
    if (write(sock, msg, strlen(msg) + 1) < 0) {
        perror("write to daemon");
        close(sock);
        return -1;
    }
    
    return sock;
}

FDMapping* find_mapping(int fd) {
    for (int i = 0; i < num_mapped; i++) {
        if (fd_map[i].original_fd == fd) {
            return &fd_map[i];
        }
    }
    return NULL;
}

int open(const char *path, int flags, ...) {
    init_real_funcs();
    
    va_list args;
    va_start(args, flags);
    int mode = va_arg(args, int);
    va_end(args);
    
    int fd = real_open(path, flags, mode);
    if (fd < 0) return fd;
    
    if (is_serial_device(path)) {
        int proxy_fd = connect_to_daemon(path);
        if (proxy_fd >= 0) {
            pthread_mutex_lock(&map_lock);
            
            if (num_mapped < 256) {
                fd_map[num_mapped].original_fd = fd;
                fd_map[num_mapped].proxy_fd = proxy_fd;
                strncpy(fd_map[num_mapped].device, path, sizeof(fd_map[num_mapped].device) - 1);
                fd_map[num_mapped].is_serial = 1;
                num_mapped++;
            }
            
            pthread_mutex_unlock(&map_lock);
            
            // Don't use the real fd, close it and use proxy
            real_close(fd);
            return proxy_fd;
        }
    }
    
    return fd;
}

int close(int fd) {
    init_real_funcs();
    
    pthread_mutex_lock(&map_lock);
    
    FDMapping *map = find_mapping(fd);
    if (map) {
        // Send close command to daemon
        char msg[64];
        snprintf(msg, sizeof(msg), "CLOSE");
        write(fd, msg, strlen(msg) + 1);
        
        real_close(map->original_fd);
        real_close(fd);
        
        // Remove from mapping
        for (int i = 0; i < num_mapped - 1; i++) {
            if (fd_map[i].original_fd == map->original_fd) {
                fd_map[i] = fd_map[num_mapped - 1];
                break;
            }
        }
        num_mapped--;
        
        pthread_mutex_unlock(&map_lock);
        return 0;
    }
    
    pthread_mutex_unlock(&map_lock);
    return real_close(fd);
}

ssize_t read(int fd, void *buf, size_t count) {
    init_real_funcs();
    
    pthread_mutex_lock(&map_lock);
    FDMapping *map = find_mapping(fd);
    pthread_mutex_unlock(&map_lock);
    
    if (map) {
        return real_read(fd, buf, count);
    }
    
    return real_read(fd, buf, count);
}

ssize_t write(int fd, const void *buf, size_t count) {
    init_real_funcs();
    
    pthread_mutex_lock(&map_lock);
    FDMapping *map = find_mapping(fd);
    pthread_mutex_unlock(&map_lock);
    
    if (map) {
        return real_write(fd, buf, count);
    }
    
    return real_write(fd, buf, count);
}

int ioctl(int fd, unsigned long request, ...) {
    init_real_funcs();
    
    va_list args;
    va_start(args, request);
    void *argp = va_arg(args, void *);
    va_end(args);
    
    pthread_mutex_lock(&map_lock);
    FDMapping *map = find_mapping(fd);
    pthread_mutex_unlock(&map_lock);
    
    if (map) {
        // Send ioctl request to daemon
        char msg[512];
        snprintf(msg, sizeof(msg), "IOCTL:%lu", request);
        write(fd, msg, strlen(msg) + 1);
        
        // For baud rate, also send the actual termios struct
        if (request == TCSETS || request == TCSETSW || request == TCSETSF) {
            struct termios *t = (struct termios *)argp;
            write(fd, t, sizeof(struct termios));
        }
        
        return real_ioctl(fd, request, argp);
    }
    
    return real_ioctl(fd, request, argp);
}
