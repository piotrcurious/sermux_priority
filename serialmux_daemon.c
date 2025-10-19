// serialmux_daemon.c - Main daemon managing serial port access
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

#define SOCKET_PATH "/tmp/serialmux.sock"
#define MAX_CLIENTS 10
#define MAX_PTY_PAIRS 10

typedef enum { PRIORITY_LOW = 0, PRIORITY_HIGH = 1 } Priority;

typedef struct {
    int client_fd;
    int pty_slave;
    pid_t pid;
    Priority priority;
    int active;
    int paused;
    struct termios saved_settings;
} Client;

typedef struct {
    int real_fd;
    char device[256];
    struct termios current_settings;
    Client clients[MAX_CLIENTS];
    int num_clients;
    int current_owner;
    pthread_mutex_t lock;
} SerialPort;

SerialPort sp;
volatile int running = 1;

void log_msg(const char *fmt, ...) {
    va_list args;
    time_t now = time(NULL);
    char timestr[64];
    strftime(timestr, sizeof(timestr), "%Y-%m-%d %H:%M:%S", localtime(&now));
    printf("[%s] ", timestr);
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
}

int create_pty_pair(int *master, int *slave) {
    char pty_name[256];
    *master = open("/dev/ptmx", O_RDWR | O_NOCTTY);
    if (*master < 0) {
        perror("open /dev/ptmx");
        return -1;
    }
    
    if (grantpt(*master) < 0 || unlockpt(*master) < 0) {
        perror("pty setup");
        close(*master);
        return -1;
    }
    
    if (ptsname_r(*master, pty_name, sizeof(pty_name)) < 0) {
        perror("ptsname_r");
        close(*master);
        return -1;
    }
    
    *slave = open(pty_name, O_RDWR | O_NOCTTY);
    if (*slave < 0) {
        perror("open slave pty");
        close(*master);
        return -1;
    }
    
    return 0;
}

void save_termios(int fd, struct termios *t) {
    tcgetattr(fd, t);
}

void restore_termios(int fd, struct termios *t) {
    tcsetattr(fd, TCSANOW, t);
}

void handle_client_ioctl(int client_idx, unsigned long request, void *argp) {
    pthread_mutex_lock(&sp.lock);
    
    Client *c = &sp.clients[client_idx];
    
    if (c->priority == PRIORITY_HIGH) {
        // High priority: apply immediately
        if (ioctl(sp.real_fd, request, argp) < 0) {
            perror("ioctl on serial port");
        }
        tcgetattr(sp.real_fd, &sp.current_settings);
        log_msg("High priority PID %d applied ioctl 0x%lx\n", c->pid, request);
    } else {
        // Low priority: save settings if paused
        if (c->paused) {
            log_msg("Low priority PID %d ioctl 0x%lx blocked (paused)\n", c->pid, request);
        } else {
            if (ioctl(sp.real_fd, request, argp) < 0) {
                perror("ioctl on serial port");
            }
            tcgetattr(sp.real_fd, &sp.current_settings);
            save_termios(sp.real_fd, &c->saved_settings);
            log_msg("Low priority PID %d applied ioctl 0x%lx\n", c->pid, request);
        }
    }
    
    pthread_mutex_unlock(&sp.lock);
}

void pause_low_priority_clients() {
    pthread_mutex_lock(&sp.lock);
    
    for (int i = 0; i < sp.num_clients; i++) {
        if (sp.clients[i].priority == PRIORITY_LOW && sp.clients[i].active) {
            sp.clients[i].paused = 1;
            log_msg("Paused low priority client PID %d\n", sp.clients[i].pid);
        }
    }
    
    pthread_mutex_unlock(&sp.lock);
}

void resume_low_priority_clients() {
    pthread_mutex_lock(&sp.lock);
    
    for (int i = 0; i < sp.num_clients; i++) {
        if (sp.clients[i].priority == PRIORITY_LOW && sp.clients[i].active) {
            sp.clients[i].paused = 0;
            restore_termios(sp.real_fd, &sp.clients[i].saved_settings);
            log_msg("Resumed low priority client PID %d with saved settings\n", sp.clients[i].pid);
        }
    }
    
    pthread_mutex_unlock(&sp.lock);
}

void handle_client_disconnect(int client_idx) {
    pthread_mutex_lock(&sp.lock);
    
    Client *c = &sp.clients[client_idx];
    log_msg("Client PID %d disconnected\n", c->pid);
    
    if (c->priority == PRIORITY_HIGH) {
        // High priority client disconnected, resume low priority ones
        resume_low_priority_clients();
    }
    
    close(c->client_fd);
    c->active = 0;
    
    pthread_mutex_unlock(&sp.lock);
}

void* data_relay_thread(void *arg) {
    int client_idx = *(int*)arg;
    free(arg);
    
    Client *c = &sp.clients[client_idx];
    unsigned char buf[4096];
    fd_set readfds;
    int max_fd = (sp.real_fd > c->client_fd) ? sp.real_fd : c->client_fd;
    
    log_msg("Started relay thread for client %d (PID %d)\n", client_idx, c->pid);
    
    while (running && c->active) {
        FD_ZERO(&readfds);
        FD_SET(sp.real_fd, &readfds);
        FD_SET(c->client_fd, &readfds);
        
        struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
        int ret = select(max_fd + 1, &readfds, NULL, NULL, &tv);
        
        if (ret < 0) {
            if (errno != EINTR) perror("select");
            break;
        }
        
        if (ret == 0) continue;
        
        // Data from real serial port to client
        if (FD_ISSET(sp.real_fd, &readfds) && !c->paused) {
            ssize_t n = read(sp.real_fd, buf, sizeof(buf));
            if (n > 0) {
                write(c->client_fd, buf, n);
            }
        }
        
        // Data from client to real serial port
        if (FD_ISSET(c->client_fd, &readfds) && !c->paused) {
            ssize_t n = read(c->client_fd, buf, sizeof(buf));
            if (n > 0) {
                write(sp.real_fd, buf, n);
            } else if (n == 0) {
                break;
            }
        }
    }
    
    handle_client_disconnect(client_idx);
    return NULL;
}

void* accept_connections_thread(void *arg) {
    while (running) {
        int client_fd = accept(*(int*)arg, NULL, NULL);
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
        
        int idx = sp.num_clients;
        sp.clients[idx].client_fd = client_fd;
        sp.clients[idx].pid = -1;
        sp.clients[idx].active = 1;
        sp.clients[idx].paused = 0;
        sp.num_clients++;
        
        pthread_mutex_unlock(&sp.lock);
        
        pthread_t tid;
        int *pidx = malloc(sizeof(int));
        *pidx = idx;
        pthread_create(&tid, NULL, data_relay_thread, pidx);
        pthread_detach(tid);
    }
    
    return NULL;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s /dev/ttyUSB0\n", argv[0]);
        exit(1);
    }
    
    signal(SIGPIPE, SIG_IGN);
    
    // Open real serial port
    sp.real_fd = open(argv[1], O_RDWR | O_NOCTTY);
    if (sp.real_fd < 0) {
        perror("open serial port");
        exit(1);
    }
    
    strncpy(sp.device, argv[1], sizeof(sp.device) - 1);
    save_termios(sp.real_fd, &sp.current_settings);
    pthread_mutex_init(&sp.lock, NULL);
    
    // Create Unix socket
    unlink(SOCKET_PATH);
    int socket_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (socket_fd < 0) {
        perror("socket");
        exit(1);
    }
    
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);
    
    if (bind(socket_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        exit(1);
    }
    
    listen(socket_fd, MAX_CLIENTS);
    log_msg("Serial mux daemon started on %s\n", argv[1]);
    log_msg("Listening on %s\n", SOCKET_PATH);
    
    // Accept connections
    pthread_t accept_tid;
    pthread_create(&accept_tid, NULL, accept_connections_thread, &socket_fd);
    
    // Main loop
    while (running) {
        sleep(1);
    }
    
    close(socket_fd);
    close(sp.real_fd);
    unlink(SOCKET_PATH);
    
    return 0;
}
