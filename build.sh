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
