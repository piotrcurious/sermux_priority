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
#include <stdarg.h>
#include <sys/types.h>
#include <sys/stat.h>

#ifdef __linux__
#include <sys/uio.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <pwd.h>
#include <grp.h>
#include <sys/ucred.h>
#endif

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

static SerialPort sp;
static struct termios original_real_settings;
static volatile sig_atomic_t running = 1;
static int sig_pipe_fds[2] = { -1, -1 }; // [0]=read, [1]=write

void log_msg(const char *fmt, ...) {
    va_list args;
    time_t now = time(NULL);
    char timestr[64];
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    strftime(timestr, sizeof(timestr), "%Y-%m-%d %H:%M:%S", &tm_now);
    printf("[%s] ", timestr);
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    fflush(stdout);
}

static int set_cloexec(int fd) {
    int flags = fcntl(fd, F_GETFD);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

// robust write: handle partial writes and EINTR/EAGAIN
ssize_t robust_write(int fd, const void *buf, size_t count) {
    const unsigned char *p = buf;
    size_t left = count;
    while (left > 0) {
        ssize_t w = write(fd, p, left);
        if (w < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // brief sleep then retry (could use poll in more advanced form)
                usleep(1000);
                continue;
            }
            return -1;
        }
        p += w;
        left -= (size_t)w;
    }
    return (ssize_t)count;
}

// Create a PTY pair, return master FD and slave name
int create_pty_pair(int *master, char *slave_name_buf, size_t slave_name_size) {
    *master = open("/dev/ptmx", O_RDWR | O_NOCTTY);
    if (*master < 0) {
        perror("open /dev/ptmx");
        return -1;
    }
    if (set_cloexec(*master) < 0) {
        // non-fatal, log and continue
        log_msg("Warning: failed to set FD_CLOEXEC on pty master: %s\n", strerror(errno));
    }

    if (grantpt(*master) < 0 || unlockpt(*master) < 0) {
        perror("pty setup (grantpt/unlockpt)");
        close(*master);
        return -1;
    }

    if (ptsname_r(*master, slave_name_buf, slave_name_size) != 0) {
        perror("ptsname_r");
        close(*master);
        return -1;
    }

    // Open slave and set raw mode (so client sees passthrough)
    int slave = open(slave_name_buf, O_RDWR | O_NOCTTY);
    if (slave < 0) {
        perror("open slave pty");
        close(*master);
        return -1;
    }

    struct termios t;
    if (tcgetattr(slave, &t) < 0) {
        perror("tcgetattr (slave)");
        close(slave);
        close(*master);
        return -1;
    }
    cfmakeraw(&t);
    if (tcsetattr(slave, TCSANOW, &t) < 0) {
        perror("tcsetattr (slave)");
        close(slave);
        close(*master);
        return -1;
    }
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
    // Caller must hold sp.lock
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

// Handles an IOCTL / termios change from a client (detected by polling tcgetattr on PTY master)
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

    // Close PTY master if open
    if (c->pty_master_fd >= 0) {
        close(c->pty_master_fd);
        c->pty_master_fd = -1;
    }

    c->active = 0;
    c->pid = -1;
    c->paused = 0;

    if (c->priority == PRIORITY_HIGH) {
        sp.num_high_priority--;
        if (sp.num_high_priority <= 0) {
            sp.num_high_priority = 0;
            resume_low_priority_clients();
        }
    }

    pthread_mutex_unlock(&sp.lock);
}

// This thread polls the PTY master for termios changes
void* pty_ioctl_thread(void *arg) {
    int client_idx = *(int*)arg;
    free(arg); // thread owns its arg - free immediately to avoid leaks
    Client *c = &sp.clients[client_idx];

    while (running) {
        // snapshot fields under lock
        pthread_mutex_lock(&sp.lock);
        int active = c->active;
        int pty_fd = c->pty_master_fd;
        pthread_mutex_unlock(&sp.lock);

        if (!active || pty_fd < 0) break;

        struct termios new_settings;
        if (tcgetattr(pty_fd, &new_settings) < 0) {
            // PTY closed or error
            if (errno != EIO && errno != EBADF) {
                // EIO/EBADF likely indicate closure; other errors logged
                // but don't spam on every iteration
            }
            break;
        }

        pthread_mutex_lock(&sp.lock);
        if (memcmp(&c->current_pty_settings, &new_settings, sizeof(new_settings)) != 0) {
            memcpy(&c->current_pty_settings, &new_settings, sizeof(new_settings));
            pthread_mutex_unlock(&sp.lock);
            handle_client_ioctl(client_idx, &new_settings);
        } else {
            pthread_mutex_unlock(&sp.lock);
        }

        // sleep 50ms as before
        struct timespec ts = {0, 50000 * 1000}; // 50ms
        nanosleep(&ts, NULL);
    }

    // Ensure disconnection cleaning is performed by one responsible thread
    handle_client_disconnect(client_idx);
    return NULL;
}

// This thread relays data between the PTY master and the real serial port
void* pty_relay_thread(void *arg) {
    int client_idx = *(int*)arg;
    free(arg); // thread owns its arg
    Client *c = &sp.clients[client_idx];

    unsigned char buf[BUF_SIZE];
    fd_set readfds;

    // We will snapshot pty_fd and paused/active inside loop (under lock)
    log_msg("Started relay thread for client %d (PID %d, initial PTY_FD %d)\n",
            client_idx, c->pid, c->pty_master_fd);

    while (running) {
        pthread_mutex_lock(&sp.lock);
        int active = c->active;
        int paused = c->paused;
        int pty_fd = c->pty_master_fd;
        int local_real_fd = sp.real_fd; // real_fd shouldn't change, but snapshot
        pthread_mutex_unlock(&sp.lock);

        if (!active || pty_fd < 0) break;

        FD_ZERO(&readfds);
        FD_SET(local_real_fd, &readfds);
        FD_SET(pty_fd, &readfds);

        int max_fd = (local_real_fd > pty_fd) ? local_real_fd : pty_fd;

        struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
        int ret = select(max_fd + 1, &readfds, NULL, NULL, &tv);

        if (ret < 0) {
            if (errno == EINTR) continue;
            perror("select (pty_relay)");
            break;
        }
        if (ret == 0) continue;

        // Data from real serial port to client's PTY
        if (FD_ISSET(local_real_fd, &readfds)) {
            // snapshot paused state again before acting
            pthread_mutex_lock(&sp.lock);
            paused = c->paused;
            pthread_mutex_unlock(&sp.lock);

            if (!paused) {
                ssize_t n = read(local_real_fd, buf, sizeof(buf));
                if (n > 0) {
                    if (robust_write(pty_fd, buf, (size_t)n) < 0) {
                        log_msg("Failed to write to PTY (fd=%d): %s\n", pty_fd, strerror(errno));
                        break;
                    }
                } else if (n <= 0) {
                    log_msg("Real serial port error/closed (read returned %zd).\n", n);
                    running = 0; // Stop daemon
                    break;
                }
            }
        }

        // Data from client's PTY to real serial port
        if (FD_ISSET(pty_fd, &readfds)) {
            ssize_t n = read(pty_fd, buf, sizeof(buf));
            if (n > 0) {
                pthread_mutex_lock(&sp.lock);
                int paused2 = c->paused;
                pthread_mutex_unlock(&sp.lock);

                if (!paused2) {
                    if (robust_write(local_real_fd, buf, (size_t)n) < 0) {
                        log_msg("Failed to write to real serial port (fd=%d): %s\n", local_real_fd, strerror(errno));
                        break;
                    }
                }
            } else if (n <= 0) {
                // Client closed its PTY or error -> exit loop and let cleanup happen
                break;
            }
        }
    }

    handle_client_disconnect(client_idx);
    return NULL;
}

// read a line from client_fd (simple, not blocking parse)
static ssize_t read_control_message(int fd, char *buf, size_t bufsz) {
    ssize_t n = read(fd, buf, bufsz - 1);
    if (n > 0) {
        buf[n] = '\0';
    }
    return n;
}

// Handles short-lived control connections on the Unix socket
void* client_control_thread(void *arg) {
    int client_fd = *(int*)arg;
    free(arg);

    char buf[512];
    ssize_t n = read_control_message(client_fd, buf, sizeof(buf));
    if (n <= 0) {
        close(client_fd);
        return NULL;
    }

    // expected formats:
    // OPEN:device:prio:pid
    // CLOSE:pid
    if (strncmp(buf, "OPEN:", 5) == 0) {
        char *saveptr = NULL;
        char *device = strtok_r(buf + 5, ":\n", &saveptr);
        char *prio_s = strtok_r(NULL, ":\n", &saveptr);
        char *pid_s = strtok_r(NULL, ":\n", &saveptr);

        Priority prio = PRIORITY_LOW;
        pid_t supplied_pid = -1;
        if (prio_s) prio = (atoi(prio_s) == 1) ? PRIORITY_HIGH : PRIORITY_LOW;
        if (pid_s) supplied_pid = (pid_t)atoi(pid_s);

        // Validate device requested
        if (!device || strcmp(device, sp.device) != 0) {
            log_msg("Client requested wrong device '%s'\n", device ? device : "(null)");
            robust_write(client_fd, "ERROR:Wrong device", strlen("ERROR:Wrong device") + 1);
            close(client_fd);
            return NULL;
        }

        // Determine peer PID from socket (SO_PEERCRED) on Linux if possible.
        pid_t peer_pid = -1;
#ifdef SO_PEERCRED
        struct ucred cred;
        socklen_t cred_len = sizeof(cred);
        if (getsockopt(client_fd, SOL_SOCKET, SO_PEERCRED, &cred, &cred_len) == 0) {
            peer_pid = (pid_t)cred.pid;
            if (supplied_pid != -1 && supplied_pid != peer_pid) {
                log_msg("Client supplied pid %d but SO_PEERCRED reports %d - using %d\n",
                        supplied_pid, peer_pid, peer_pid);
            }
        } else {
            // getsockopt failed; fall back to supplied pid
            peer_pid = supplied_pid;
        }
#else
        peer_pid = supplied_pid;
#endif

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
            log_msg("Max clients reached, rejecting PID %d\n", (int)peer_pid);
            robust_write(client_fd, "ERROR:Max clients", strlen("ERROR:Max clients") + 1);
            pthread_mutex_unlock(&sp.lock);
            close(client_fd);
            return NULL;
        }

        // Create PTY for this client
        char pty_slave_name[256];
        int pty_master = -1;
        if (create_pty_pair(&pty_master, pty_slave_name, sizeof(pty_slave_name)) < 0) {
            log_msg("Failed to create PTY for PID %d\n", (int)peer_pid);
            robust_write(client_fd, "ERROR:PTY creation failed", strlen("ERROR:PTY creation failed") + 1);
            pthread_mutex_unlock(&sp.lock);
            close(client_fd);
            return NULL;
        }

        // make PTY master CLOEXEC
        if (set_cloexec(pty_master) < 0) {
            log_msg("Warning: failed to set FD_CLOEXEC on pty master: %s\n", strerror(errno));
        }

        // Send PTY slave path back to client (include terminating NUL)
        if (robust_write(client_fd, pty_slave_name, strlen(pty_slave_name) + 1) < 0) {
            log_msg("Failed to send PTY name to client PID %d: %s\n", (int)peer_pid, strerror(errno));
            close(pty_master);
            pthread_mutex_unlock(&sp.lock);
            close(client_fd);
            return NULL;
        }

        // Configure client slot
        Client *c = &sp.clients[idx];
        c->active = 1;
        c->pid = peer_pid;
        c->priority = prio;
        c->pty_master_fd = pty_master;
        c->paused = 0;

        // Save current PTY settings (if possible)
        save_termios(c->pty_master_fd, &c->current_pty_settings);

        if (prio == PRIORITY_HIGH) {
            sp.num_high_priority++;
            pause_low_priority_clients();
            // High prio client's settings: copy current real port settings to its PTY
            save_termios(sp.real_fd, &c->saved_settings);
            restore_termios(c->pty_master_fd, &c->saved_settings);
            memcpy(&c->current_pty_settings, &c->saved_settings, sizeof(struct termios));
        } else {
            // Low prio client
            // Save current port settings as its starting point
            save_termios(sp.real_fd, &c->saved_settings);

            if (sp.num_high_priority > 0) {
                c->paused = 1; // Pause immediately if high-prio is active
                log_msg("Low priority client PID %d connected (paused)\n", (int)c->pid);
            }
        }

        pthread_mutex_unlock(&sp.lock);

        // Spawn relay and ioctl threads
        pthread_t relay_tid, ioctl_tid;
        int *pidx = malloc(sizeof(int));
        if (!pidx) {
            log_msg("malloc failed for pidx\n");
            handle_client_disconnect(idx);
            close(client_fd);
            return NULL;
        }
        *pidx = idx;
        if (pthread_create(&relay_tid, NULL, pty_relay_thread, pidx) != 0) {
            log_msg("pthread_create relay failed\n");
            free(pidx);
            handle_client_disconnect(idx);
            close(client_fd);
            return NULL;
        }
        if (pthread_detach(relay_tid) != 0) {
            log_msg("pthread_detach relay failed\n");
        }

        int *pidx2 = malloc(sizeof(int));
        if (!pidx2) {
            log_msg("malloc failed for pidx2\n");
            // We won't leak pidx because it's given to the relay thread which frees it.
            handle_client_disconnect(idx);
            close(client_fd);
            return NULL;
        }
        *pidx2 = idx;
        if (pthread_create(&ioctl_tid, NULL, pty_ioctl_thread, pidx2) != 0) {
            log_msg("pthread_create ioctl failed\n");
            free(pidx2);
            handle_client_disconnect(idx);
            close(client_fd);
            return NULL;
        }
        if (pthread_detach(ioctl_tid) != 0) {
            log_msg("pthread_detach ioctl failed\n");
        }

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
            // Ask relay thread to clean up by closing PTY master
            log_msg("Received CLOSE from PID %d, initiating disconnect.\n", (int)pid);
            handle_client_disconnect(idx);
        } else {
            log_msg("CLOSE: unknown pid %d\n", (int)pid);
        }
    } else {
        log_msg("Unknown control message: %s\n", buf);
    }

    close(client_fd);
    return NULL;
}

// async-signal-safe wakeup: write a byte to the pipe
void handle_signal(int sig) {
    running = 0;
    if (sig_pipe_fds[1] != -1) {
        const char c = 'x';
        ssize_t r = write(sig_pipe_fds[1], &c, 1);
        (void)r;
    }
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s /dev/ttyUSB0\n", argv[0]);
        exit(1);
    }

    // Set signals (do not call non-async-safe functions from handler)
    struct sigaction sa;
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    // Init serial port struct
    memset(&sp, 0, sizeof(sp));
    pthread_mutex_init(&sp.lock, NULL);
    sp.num_high_priority = 0;

    // Open real serial port
    sp.real_fd = open(argv[1], O_RDWR | O_NOCTTY);
    if (sp.real_fd < 0) {
        perror("open serial port");
        exit(1);
    }
    if (set_cloexec(sp.real_fd) < 0) {
        log_msg("Warning: failed to set FD_CLOEXEC on serial fd: %s\n", strerror(errno));
    }

    strncpy(sp.device, argv[1], sizeof(sp.device) - 1);
    save_termios(sp.real_fd, &sp.current_settings);
    // Save original to restore on exit
    save_termios(sp.real_fd, &original_real_settings);

    // Set up signal pipe for safe wakeup of main loop
    if (pipe(sig_pipe_fds) < 0) {
        perror("pipe");
        close(sp.real_fd);
        exit(1);
    }
    if (set_cloexec(sig_pipe_fds[0]) < 0 || set_cloexec(sig_pipe_fds[1]) < 0) {
        log_msg("Warning: failed to set FD_CLOEXEC on sig pipe\n");
    }

    // Create Unix socket
    unlink(SOCKET_PATH);
    int socket_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (socket_fd < 0) {
        perror("socket");
        restore_termios(sp.real_fd, &original_real_settings);
        close(sp.real_fd);
        exit(1);
    }
    if (set_cloexec(socket_fd) < 0) {
        log_msg("Warning: failed to set FD_CLOEXEC on socket fd\n");
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (bind(socket_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        restore_termios(sp.real_fd, &original_real_settings);
        close(socket_fd);
        close(sp.real_fd);
        exit(1);
    }

    // Restrict socket file permissions
    if (chmod(SOCKET_PATH, S_IRUSR | S_IWUSR) < 0) {
        log_msg("Warning: chmod on socket failed: %s\n", strerror(errno));
    }

    if (listen(socket_fd, MAX_CLIENTS) < 0) {
        perror("listen");
        restore_termios(sp.real_fd, &original_real_settings);
        close(socket_fd);
        close(sp.real_fd);
        unlink(SOCKET_PATH);
        exit(1);
    }

    log_msg("Serial mux daemon started on %s\n", argv[1]);
    log_msg("Listening on %s\n", SOCKET_PATH);

    // Main accept loop
    while (running) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(socket_fd, &readfds);
        FD_SET(sig_pipe_fds[0], &readfds);

        int maxfd = socket_fd;
        if (sig_pipe_fds[0] > maxfd) maxfd = sig_pipe_fds[0];

        struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
        int ret = select(maxfd + 1, &readfds, NULL, NULL, &tv);

        if (ret < 0) {
            if (errno == EINTR) continue;
            perror("select (main)");
            break;
        }

        if (ret == 0) continue;

        if (FD_ISSET(sig_pipe_fds[0], &readfds)) {
            // drain pipe
            char drain[64];
            while (read(sig_pipe_fds[0], drain, sizeof(drain)) > 0) {}
            if (!running) break;
        }

        if (FD_ISSET(socket_fd, &readfds)) {
            int client_fd = accept(socket_fd, NULL, NULL);
            if (client_fd < 0) {
                if (errno == EINTR) continue;
                perror("accept");
                continue;
            }

            // set CLOEXEC on client fd as well
            if (set_cloexec(client_fd) < 0) {
                log_msg("Warning: failed to set FD_CLOEXEC on client fd\n");
            }

            pthread_t tid;
            int *p_client_fd = malloc(sizeof(int));
            if (!p_client_fd) {
                log_msg("malloc failed for client fd arg\n");
                close(client_fd);
                continue;
            }
            *p_client_fd = client_fd;
            if (pthread_create(&tid, NULL, client_control_thread, p_client_fd) != 0) {
                log_msg("pthread_create failed for control thread\n");
                free(p_client_fd);
                close(client_fd);
                continue;
            }
            if (pthread_detach(tid) != 0) {
                log_msg("pthread_detach failed for control thread\n");
            }
        }
    }

    // Begin shutdown: no more accepts/relays; restore termios
    log_msg("Shutting down: restoring original termios and cleaning up\n");
    restore_termios(sp.real_fd, &original_real_settings);

    // Close active clients and their PTYs
    pthread_mutex_lock(&sp.lock);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (sp.clients[i].active) {
            if (sp.clients[i].pty_master_fd >= 0) {
                close(sp.clients[i].pty_master_fd);
                sp.clients[i].pty_master_fd = -1;
            }
            sp.clients[i].active = 0;
        }
    }
    pthread_mutex_unlock(&sp.lock);

    // Close socket and serial port, unlink socket path and close pipe fds
    close(socket_fd);
    close(sp.real_fd);
    unlink(SOCKET_PATH);
    if (sig_pipe_fds[0] != -1) close(sig_pipe_fds[0]);
    if (sig_pipe_fds[1] != -1) close(sig_pipe_fds[1]);

    log_msg("Daemon shut down.\n");
    return 0;
}
