#!/bin/bash
# Build and usage script for Serial Port Multiplexer

cat > Makefile << 'EOF'
CFLAGS = -Wall -Wextra -fPIC -I.
LDFLAGS = -lpthread

all: serialmux_daemon libserialmux_preload.so

serialmux_daemon: serialmux_daemon.c
	gcc $(CFLAGS) -o $@ $^ $(LDFLAGS)

libserialmux_preload.so: serialmux_preload.c
	gcc $(CFLAGS) -shared -o $@ $^ -ldl

clean:
	rm -f serialmux_daemon libserialmux_preload.so

install: all
	sudo cp serialmux_daemon /usr/local/bin/
	sudo cp libserialmux_preload.so /usr/local/lib/

.PHONY: all clean install
EOF

cat > serialmux_daemon.c << 'DAEMON_EOF'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/select.h>
#include <pthread.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <stdarg.h>

#define SOCKET_PATH "/tmp/serialmux.sock"
#define MAX_CLIENTS 10

typedef struct {
    int socket_fd;
    pid_t pid;
    int priority;
    int active;
    int paused;
    struct termios saved_settings;
    char device[256];
} Client;

typedef struct {
    int real_fd;
    char device[256];
    struct termios current_settings;
    Client clients[MAX_CLIENTS];
    int num_clients;
    pthread_mutex_t lock;
} SerialPort;

SerialPort sp;
volatile int running = 1;

void log_msg(const char *fmt, ...) {
    va_list args;
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char timestr[64];
    strftime(timestr, sizeof(timestr), "%H:%M:%S", tm_info);
    printf("[%s] ", timestr);
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    fflush(stdout);
}

void save_termios(int fd, struct termios *t) {
    if (tcgetattr(fd, t) < 0) {
        perror("tcgetattr");
    }
}

void restore_termios(int fd, struct termios *t) {
    if (tcsetattr(fd, TCSANOW, t) < 0) {
        perror("tcsetattr");
    }
}

void pause_low_priority() {
    pthread_mutex_lock(&sp.lock);
    for (int i = 0; i < sp.num_clients; i++) {
        if (sp.clients[i].priority == 0 && sp.clients[i].active && !sp.clients[i].paused) {
            sp.clients[i].paused = 1;
            save_termios(sp.real_fd, &sp.clients[i].saved_settings);
            log_msg("PAUSE: Low priority PID %d\n", sp.clients[i].pid);
        }
    }
    pthread_mutex_unlock(&sp.lock);
}

void resume_low_priority() {
    pthread_mutex_lock(&sp.lock);
    for (int i = 0; i < sp.num_clients; i++) {
        if (sp.clients[i].priority == 0 && sp.clients[i].active && sp.clients[i].paused) {
            sp.clients[i].paused = 0;
            restore_termios(sp.real_fd, &sp.clients[i].saved_settings);
            log_msg("RESUME: Low priority PID %d\n", sp.clients[i].pid);
        }
    }
    pthread_mutex_unlock(&sp.lock);
}

void* client_handler(void *arg) {
    int client_idx = *(int*)arg;
    free(arg);
    
    Client *c = &sp.clients[client_idx];
    unsigned char buf[4096];
    
    log_msg("Handler started for PID %d (priority %d)\n", c->pid, c->priority);
    
    while (running && c->active) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(sp.real_fd, &readfds);
        FD_SET(c->socket_fd, &readfds);
        
        int max_fd = (sp.real_fd > c->socket_fd) ? sp.real_fd : c->socket_fd;
        struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
        
        int ret = select(max_fd + 1, &readfds, NULL, NULL, &tv);
        
        if (ret < 0) {
            if (errno != EINTR) perror("select");
            break;
        }
        
        if (ret == 0) continue;
        
        // Data from serial port to client (if not paused)
        if (FD_ISSET(sp.real_fd, &readfds) && !c->paused) {
            ssize_t n = read(sp.real_fd, buf, sizeof(buf));
            if (n > 0) {
                if (write(c->socket_fd, buf, n) < 0) {
                    if (errno != EPIPE) perror("write to client");
                    break;
                }
            }
        }
        
        // Data from client to serial port (if not paused)
        if (FD_ISSET(c->socket_fd, &readfds) && !c->paused) {
            ssize_t n = read(c->socket_fd, buf, sizeof(buf));
            if (n > 0) {
                // Check for control commands
                if (n > 0 && buf[0] == 0xFF) {
                    unsigned char cmd = buf[1];
                    if (cmd == 0x01) {  // CLOSE
                        log_msg("PID %d: Close request\n", c->pid);
                        break;
                    } else if (cmd == 0x02) {  // SET_BAUD
                        speed_t baud = *(speed_t*)&buf[2];
                        struct termios t;
                        tcgetattr(sp.real_fd, &t);
                        cfsetispeed(&t, baud);
                        cfsetospeed(&t, baud);
                        tcsetattr(sp.real_fd, TCSANOW, &t);
                        tcgetattr(sp.real_fd, &sp.current_settings);
                        log_msg("PID %d: Set baud rate\n", c->pid);
                    }
                } else {
                    // Regular data
                    if (write(sp.real_fd, buf, n) < 0) {
                        perror("write to serial");
                        break;
                    }
                }
            } else if (n == 0) {
                break;
            }
        }
    }
    
    pthread_mutex_lock(&sp.lock);
    
    if (c->priority == 1) {
        resume_low_priority();
    }
    
    close(c->socket_fd);
    c->active = 0;
    
    log_msg("Handler ended for PID %d\n", c->pid);
    pthread_mutex_unlock(&sp.lock);
    
    return NULL;
}

void* accept_thread(void *arg) {
    int server_fd = *(int*)arg;
    
    while (running) {
        struct sockaddr_un addr;
        socklen_t len = sizeof(addr);
        
        int client_fd = accept(server_fd, (struct sockaddr*)&addr, &len);
        if (client_fd < 0) {
            if (errno != EINTR) perror("accept");
            continue;
        }
        
        pthread_mutex_lock(&sp.lock);
        
        if (sp.num_clients >= MAX_CLIENTS) {
            log_msg("Max clients reached, rejecting connection\n");
            close(client_fd);
            pthread_mutex_unlock(&sp.lock);
            continue;
        }
        
        // Read initial message: "PID:priority"
        char msg[128];
        read(client_fd, msg, sizeof(msg));
        
        int priority = 0;
        pid_t pid = 0;
        sscanf(msg, "%d:%d", &pid, &priority);
        
        int idx = sp.num_clients;
        sp.clients[idx].socket_fd = client_fd;
        sp.clients[idx].pid = pid;
        sp.clients[idx].priority = priority;
        sp.clients[idx].active = 1;
        sp.clients[idx].paused = 0;
        strncpy(sp.clients[idx].device, sp.device, sizeof(sp.clients[idx].device) - 1);
        sp.num_clients++;
        
        if (priority == 1) {
            pause_low_priority();
        }
        
        pthread_mutex_unlock(&sp.lock);
        
        pthread_t tid;
        int *idx_ptr = malloc(sizeof(int));
        *idx_ptr = idx;
        pthread_create(&tid, NULL, client_handler, idx_ptr);
        pthread_detach(tid);
        
        log_msg("New client: PID %d, priority %d\n", pid, priority);
    }
    
    return NULL;
}

void handle_signal(int sig) {
    running = 0;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s /dev/ttyUSB0\n", argv[0]);
        exit(1);
    }
    
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    
    // Open real serial port
    sp.real_fd = open(argv[1], O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (sp.real_fd < 0) {
        perror("open serial port");
        exit(1);
    }
    
    strncpy(sp.device, argv[1], sizeof(sp.device) - 1);
    save_termios(sp.real_fd, &sp.current_settings);
    pthread_mutex_init(&sp.lock, NULL);
    
    // Create Unix domain socket
    unlink(SOCKET_PATH);
    int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        exit(1);
    }
    
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);
    
    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        exit(1);
    }
    
    listen(server_fd, MAX_CLIENTS);
    
    log_msg("Serial Mux Daemon started\n");
    log_msg("Device: %s\n", argv[1]);
    log_msg("Socket: %s\n", SOCKET_PATH);
    
    pthread_t accept_tid;
    pthread_create(&accept_tid, NULL, accept_thread, &server_fd);
    
    while (running) {
        sleep(1);
    }
    
    close(server_fd);
    close(sp.real_fd);
    unlink(SOCKET_PATH);
    
    log_msg("Daemon shutdown\n");
    return 0;
}
DAEMON_EOF

cat > serialmux_preload.c << 'PRELOAD_EOF'
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

#define SOCKET_PATH "/tmp/serialmux.sock"

typedef struct {
    int real_fd;
    int proxy_fd;
    char device[256];
} FDMap;

static FDMap fd_map[256];
static int map_count = 0;
static pthread_mutex_t map_lock = PTHREAD_MUTEX_INITIALIZER;

typedef int (*open_t)(const char *, int, ...);
typedef int (*close_t)(int);
typedef ssize_t (*read_t)(int, void *, size_t);
typedef ssize_t (*write_t)(int, const void *, size_t);
typedef int (*ioctl_t)(int, unsigned long, ...);

static open_t real_open = NULL;
static close_t real_close = NULL;
static read_t real_read = NULL;
static write_t real_write = NULL;
static ioctl_t real_ioctl = NULL;

void init_funcs() {
    if (real_open) return;
    real_open = dlsym(RTLD_NEXT, "open");
    real_close = dlsym(RTLD_NEXT, "close");
    real_read = dlsym(RTLD_NEXT, "read");
    real_write = dlsym(RTLD_NEXT, "write");
    real_ioctl = dlsym(RTLD_NEXT, "ioctl");
}

int is_serial_device(const char *path) {
    return strstr(path, "/dev/tty") != NULL;
}

int connect_daemon(const char *device) {
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) return -1;
    
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);
    
    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(sock);
        return -1;
    }
    
    const char *prio_str = getenv("SERIALMUX_PRIORITY");
    int priority = (prio_str && strcmp(prio_str, "HIGH") == 0) ? 1 : 0;
    
    char msg[128];
    snprintf(msg, sizeof(msg), "%d:%d", getpid(), priority);
    write(sock, msg, strlen(msg) + 1);
    
    return sock;
}

FDMap* find_mapping(int fd) {
    for (int i = 0; i < map_count; i++) {
        if (fd_map[i].real_fd == fd) {
            return &fd_map[i];
        }
    }
    return NULL;
}

int open(const char *path, int flags, ...) {
    init_funcs();
    
    va_list args;
    va_start(args, flags);
    mode_t mode = va_arg(args, mode_t);
    va_end(args);
    
    int fd = real_open(path, flags, mode);
    if (fd < 0) return fd;
    
    if (is_serial_device(path)) {
        int proxy_fd = connect_daemon(path);
        if (proxy_fd >= 0) {
            pthread_mutex_lock(&map_lock);
            fd_map[map_count].real_fd = fd;
            fd_map[map_count].proxy_fd = proxy_fd;
            strncpy(fd_map[map_count].device, path, sizeof(fd_map[map_count].device) - 1);
            map_count++;
            pthread_mutex_unlock(&map_lock);
            
            real_close(fd);
            return proxy_fd;
        }
    }
    
    return fd;
}

int close(int fd) {
    init_funcs();
    
    pthread_mutex_lock(&map_lock);
    FDMap *map = find_mapping(fd);
    if (map) {
        unsigned char cmd[2] = {0xFF, 0x01};
        write(fd, cmd, 2);
        real_close(map->real_fd);
        real_close(fd);
        
        for (int i = 0; i < map_count - 1; i++) {
            if (fd_map[i].real_fd == map->real_fd) {
                fd_map[i] = fd_map[map_count - 1];
                break;
            }
        }
        map_count--;
        pthread_mutex_unlock(&map_lock);
        return 0;
    }
    pthread_mutex_unlock(&map_lock);
    
    return real_close(fd);
}

ssize_t read(int fd, void *buf, size_t count) {
    init_funcs();
    pthread_mutex_lock(&map_lock);
    int is_mapped = (find_mapping(fd) != NULL);
    pthread_mutex_unlock(&map_lock);
    
    return real_read(fd, buf, count);
}

ssize_t write(int fd, const void *buf, size_t count) {
    init_funcs();
    pthread_mutex_lock(&map_lock);
    int is_mapped = (find_mapping(fd) != NULL);
    pthread_mutex_unlock(&map_lock);
    
    return real_write(fd, buf, count);
}

int ioctl(int fd, unsigned long request, ...) {
    init_funcs();
    
    va_list args;
    va_start(args, request);
    void *argp = va_arg(args, void *);
    va_end(args);
    
    pthread_mutex_lock(&map_lock);
    int is_mapped = (find_mapping(fd) != NULL);
    pthread_mutex_unlock(&map_lock);
    
    if (is_mapped) {
        return real_ioctl(fd, request, argp);
    }
    
    return real_ioctl(fd, request, argp);
}
PRELOAD_EOF

make
