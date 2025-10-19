// serialmux_daemon.c - Main daemon managing serial port access
#define _GNU_SOURCE
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
#include <stdarg.h> // For va_list

#define SOCKET_PATH "/tmp/serialmux.sock"
#define MAX_CLIENTS 10
#define BUF_SIZE 4096

typedef enum { PRIORITY_LOW = 0, PRIORITY_HIGH = 1 } Priority;

typedef struct {
    int active;
    int paused;
    pid_t pid;
    Priority priority;
    int pty_master_fd;
    struct termios saved_settings; // The settings this client *wants*
    struct termios current_pty_settings; // The last-seen settings on its PTY
} Client;

typedef struct {
    int real_fd;
    char device[256];
    struct termios current_settings;
    Client clients[MAX_CLIENTS];
    int num_high_priority;
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
    fflush(stdout);
}

// Create a PTY pair, return master FD and slave name
int create_pty_pair(int *master, char *slave_name_buf, size_t slave_name_size) {
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
    
    if (ptsname_r(*master, slave_name_buf, slave_name_size) < 0) {
        perror("ptsname_r");
        close(*master);
        return -1;
    }
    
    // Set slave PTY to raw mode to pass data through
    int slave = open(slave_name_buf, O_RDWR | O_NOCTTY);
    if (slave < 0) {
        perror("open slave pty");
        close(*master);
        return -1;
    }
    
    struct termios t;
    tcgetattr(slave, &t);
    cfmakeraw(&t);
    tcsetattr(slave, TCSANOW, &t);
    close(slave);
    
    return 0;
}

void save_termios(int fd, struct termios *t) {
    if (tcgetattr(fd, t) < 0) {
        perror("tcgetattr (save)");
    }
}

void restore_termios(int fd, struct termios *t) {
    if (tcsetattr(fd, TCSANOW, t) < 0) {
        perror("tcsetattr (restore)");
    }
}

// A high-priority client has applied settings.
// We must save this as the new "base" setting for low-prio clients.
void update_low_priority_saves(struct termios *new_settings) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (sp.clients[i].active && sp.clients[i].priority == PRIORITY_LOW) {
            memcpy(&sp.clients[i].saved_settings, new_settings, sizeof(struct termios));
        }
    }
    memcpy(&sp.current_settings, new_settings, sizeof(struct termios));
}

void pause_low_priority_clients() {
    // This function is called WITH sp.lock HELD
    log_msg("High priority client active. Pausing low priority clients.\n");
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (sp.clients[i].active && sp.clients[i].priority == PRIORITY_LOW) {
            sp.clients[i].paused = 1;
            log_msg("Paused low priority client PID %d\n", sp.clients[i].pid);
        }
    }
}

void resume_low_priority_clients() {
    // This function is called WITH sp.lock HELD
    log_msg("Last high priority client disconnected. Resuming low priority clients.\n");
    
    // Find the *last* active low-prio client and apply its settings
    int last_low_prio_idx = -1;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (sp.clients[i].active && sp.clients[i].priority == PRIORITY_LOW) {
            last_low_prio_idx = i;
        }
    }

    if (last_low_prio_idx != -1) {
        log_msg("Restoring settings from PID %d\n", sp.clients[last_low_prio_idx].pid);
        restore_termios(sp.real_fd, &sp.clients[last_low_prio_idx].saved_settings);
        save_termios(sp.real_fd, &sp.current_settings);
    }
    
    // Unpause all low-prio clients
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (sp.clients[i].active && sp.clients[i].priority == PRIORITY_LOW) {
            sp.clients[i].paused = 0;
            log_msg("Resumed low priority client PID %d\n", sp.clients[i].pid);
        }
    }
}

// Handles an IOCTL from a client (detected by polling tcgetattr on PTY master)
void handle_client_ioctl(int client_idx, struct termios *new_settings) {
    pthread_mutex_lock(&sp.lock);
    
    Client *c = &sp.clients[client_idx];
    if (!c->active) {
        pthread_mutex_unlock(&sp.lock);
        return;
    }
    
    if (c->priority == PRIORITY_HIGH) {
        // High priority: apply immediately
        restore_termios(sp.real_fd, new_settings);
        update_low_priority_saves(new_settings); // New settings are the baseline now
        log_msg("High priority PID %d applied new termios settings\n", c->pid);
    } else {
        // Low priority: save settings, apply if not paused
        memcpy(&c->saved_settings, new_settings, sizeof(struct termios));
        if (c->paused) {
            log_msg("Low priority PID %d termios settings saved (paused)\n", c->pid);
        } else {
            restore_termios(sp.real_fd, new_settings);
            save_termios(sp.real_fd, &sp.current_settings);
            log_msg("Low priority PID %d applied new termios settings\n", c->pid);
        }
    }
    
    pthread_mutex_unlock(&sp.lock);
}

void handle_client_disconnect(int client_idx) {
    pthread_mutex_lock(&sp.lock);
    
    Client *c = &sp.clients[client_idx];
    if (!c->active) {
        pthread_mutex_unlock(&sp.lock);
        return;
    }
    
    log_msg("Client PID %d disconnected\n", c->pid);
    
    close(c->pty_master_fd);
    c->active = 0;
    c->pid = -1;
    c->pty_master_fd = -1;
    
    if (c->priority == PRIORITY_HIGH) {
        sp.num_high_priority--;
        if (sp.num_high_priority == 0) {
            resume_low_priority_clients();
        }
    }
    
    pthread_mutex_unlock(&sp.lock);
}

// This thread polls the PTY master for termios changes
void* pty_ioctl_thread(void *arg) {
    int client_idx = *(int*)arg;
    Client *c = &sp.clients[client_idx];
    
    while (running && c->active) {
        struct termios new_settings;
        if (tcgetattr(c->pty_master_fd, &new_settings) < 0) {
            // PTY has been closed
            break; 
        }

        if (memcmp(&c->current_pty_settings, &new_settings, sizeof(new_settings)) != 0) {
            // Settings changed!
            memcpy(&c->current_pty_settings, &new_settings, sizeof(new_settings));
            handle_client_ioctl(client_idx, &new_settings);
        }
        
        usleep(50000); // Poll every 50ms
    }
    return NULL;
}

// This thread relays data between the PTY master and the real serial port
void* pty_relay_thread(void *arg) {
    int client_idx = *(int*)arg;
    Client *c = &sp.clients[client_idx];
    
    unsigned char buf[BUF_SIZE];
    fd_set readfds;
    int pty_fd = c->pty_master_fd;
    int max_fd = (sp.real_fd > pty_fd) ? sp.real_fd : pty_fd;
    
    log_msg("Started relay thread for client %d (PID %d, PTY_FD %d)\n", client_idx, c->pid, pty_fd);
    
    while (running && c->active) {
        FD_ZERO(&readfds);
        FD_SET(sp.real_fd, &readfds);
        FD_SET(pty_fd, &readfds);
        
        struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
        int ret = select(max_fd + 1, &readfds, NULL, NULL, &tv);
        
        if (ret < 0) {
            if (errno != EINTR) perror("select");
            break;
        }
        if (ret == 0) continue;
        
        // Data from real serial port to client's PTY
        if (FD_ISSET(sp.real_fd, &readfds)) {
            if (!c->paused) {
                ssize_t n = read(sp.real_fd, buf, sizeof(buf));
                if (n > 0) {
                    write(pty_fd, buf, n);
                } else if (n <= 0) {
                    log_msg("Real serial port error/closed.\n");
                    running = 0; // Stop daemon
                    break;
                }
            }
        }
        
        // Data from client's PTY to real serial port
        if (FD_ISSET(pty_fd, &readfds)) {
            ssize_t n = read(pty_fd, buf, sizeof(buf));
            if (n > 0) {
                if (!c->paused) {
                    write(sp.real_fd, buf, n);
                }
            } else if (n <= 0) {
                // Client closed its PTY
                break;
            }
        }
    }
    
    handle_client_disconnect(client_idx);
    free(arg);
    return NULL;
}

// Handles short-lived control connections on the Unix socket
void* client_control_thread(void *arg) {
    int client_fd = *(int*)arg;
    free(arg);
    
    char buf[512];
    ssize_t n = read(client_fd, buf, sizeof(buf) - 1);
    
    if (n <= 0) {
        close(client_fd);
        return NULL;
    }
    buf[n] = '\0';
    
    if (strncmp(buf, "OPEN:", 5) == 0) {
        char *device = strtok(buf + 5, ":");
        Priority prio = (Priority)atoi(strtok(NULL, ":"));
        pid_t pid = (pid_t)atoi(strtok(NULL, ":"));
        
        if (!device || strcmp(device, sp.device) != 0) {
            log_msg("Client PID %d requested wrong device '%s'\n", pid, device);
            write(client_fd, "ERROR:Wrong device", 19);
            close(client_fd);
            return NULL;
        }
        
        pthread_mutex_lock(&sp.lock);
        
        // Find a free client slot
        int idx = -1;
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (!sp.clients[i].active) {
                idx = i;
                break;
            }
        }
        
        if (idx == -1) {
            log_msg("Max clients reached, rejecting PID %d\n", pid);
            write(client_fd, "ERROR:Max clients", 18);
            pthread_mutex_unlock(&sp.lock);
            close(client_fd);
            return NULL;
        }
        
        // Create PTY for this client
        char pty_slave_name[256];
        int pty_master;
        if (create_pty_pair(&pty_master, pty_slave_name, sizeof(pty_slave_name)) < 0) {
            log_msg("Failed to create PTY for PID %d\n", pid);
            write(client_fd, "ERROR:PTY creation failed", 26);
            pthread_mutex_unlock(&sp.lock);
            close(client_fd);
            return NULL;
        }

        // Send PTY path back to client
        write(client_fd, pty_slave_name, strlen(pty_slave_name) + 1);
        
        // Configure client slot
        Client *c = &sp.clients[idx];
        c->active = 1;
        c->pid = pid;
        c->priority = prio;
        c->pty_master_fd = pty_master;
        c->paused = 0;
        
        // Save current PTY settings
        save_termios(c->pty_master_fd, &c->current_pty_settings);

        if (prio == PRIORITY_HIGH) {
            sp.num_high_priority++;
            pause_low_priority_clients();
            // High prio client's settings are applied immediately
            // We just copy the current real port settings to its PTY
            save_termios(sp.real_fd, &c->saved_settings);
            restore_termios(c->pty_master_fd, &c->saved_settings);
            memcpy(&c->current_pty_settings, &c->saved_settings, sizeof(struct termios));
        } else {
            // Low prio client
            // Save current port settings as its starting point
            save_termios(sp.real_fd, &c->saved_settings);
            
            if (sp.num_high_priority > 0) {
                c->paused = 1; // Pause immediately if high-prio is active
                log_msg("Low priority client PID %d connected (paused)\n", pid);
            }
        }
        
        pthread_mutex_unlock(&sp.lock);
        
        // Spawn relay and ioctl threads
        pthread_t relay_tid, ioctl_tid;
        int *pidx = malloc(sizeof(int));
        *pidx = idx;
        pthread_create(&relay_tid, NULL, pty_relay_thread, pidx);
        pthread_detach(relay_tid);
        
        int *pidx2 = malloc(sizeof(int));
        *pidx2 = idx;
        pthread_create(&ioctl_tid, NULL, pty_ioctl_thread, pidx2);
        pthread_detach(ioctl_tid);
        
    } else if (strncmp(buf, "CLOSE:", 6) == 0) {
        pid_t pid = (pid_t)atoi(buf + 6);
        
        pthread_mutex_lock(&sp.lock);
        int idx = -1;
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (sp.clients[i].active && sp.clients[i].pid == pid) {
                idx = i;
                break;
            }
        }
        pthread_mutex_unlock(&sp.lock);
        
        if (idx != -1) {
            // The relay thread will see the PTY close and call
            // handle_client_disconnect() itself.
            log_msg("Received CLOSE from PID %d, its relay thread will clean up.\n", pid);
        }
    }
    
    close(client_fd);
    return NULL;
}

void handle_signal(int sig) {
    running = 0;
    log_msg("Caught signal %d, shutting down.\n", sig);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s /dev/ttyUSB0\n", argv[0]);
        exit(1);
    }
    
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    
    // Init serial port struct
    memset(&sp, 0, sizeof(sp));
    pthread_mutex_init(&sp.lock, NULL);
    
    // Open real serial port
    sp.real_fd = open(argv[1], O_RDWR | O_NOCTTY);
    if (sp.real_fd < 0) {
        perror("open serial port");
        exit(1);
    }
    
    strncpy(sp.device, argv[1], sizeof(sp.device) - 1);
    save_termios(sp.real_fd, &sp.current_settings);
    
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
    
    if (listen(socket_fd, MAX_CLIENTS) < 0) {
        perror("listen");
        exit(1);
    }
    
    log_msg("Serial mux daemon started on %s\n", argv[1]);
    log_msg("Listening on %s\n", SOCKET_PATH);
    
    // Main accept loop
    while (running) {
        struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(socket_fd, &readfds);
        
        int ret = select(socket_fd + 1, &readfds, NULL, NULL, &tv);
        
        if (ret < 0) {
            if (errno == EINTR) continue;
            perror("select (main)");
            break;
        }
        
        if (ret == 0) continue;
        
        if (FD_ISSET(socket_fd, &readfds)) {
            int client_fd = accept(socket_fd, NULL, NULL);
            if (client_fd < 0) {
                perror("accept");
                continue;
            }
            
            pthread_t tid;
            int *p_client_fd = malloc(sizeof(int));
            *p_client_fd = client_fd;
            pthread_create(&tid, NULL, client_control_thread, p_client_fd);
            pthread_detach(tid);
        }
    }
    
    close(socket_fd);
    close(sp.real_fd);
    unlink(SOCKET_PATH);
    log_msg("Daemon shut down.\n");
    
    return 0;
}
