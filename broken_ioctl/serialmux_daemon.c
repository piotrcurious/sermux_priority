// serialmux_daemon.c - Serial mux daemon with ioctl/tc* forwarding support
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
#include <sys/types.h>
#include <pthread.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <stdint.h>

#ifdef __linux__
#include <sys/uio.h>
#endif

#define SOCKET_PATH "/tmp/serialmux.sock"
#define MAX_CLIENTS 10
#define BUF_SIZE 4096

#include "serialmux.h"

typedef enum { PRIORITY_LOW = 0, PRIORITY_HIGH = 1 } Priority;

typedef struct {
    int active;
    int paused;
    pid_t pid;               // pid reported by client (text open) or 0
    Priority priority;
    int pty_master_fd;
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
volatile sig_atomic_t running = 1;
static int sig_pipe_fds[2] = { -1, -1 };

/* Logging helper */
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

/* set FD_CLOEXEC */
static int set_cloexec(int fd) {
    int flags = fcntl(fd, F_GETFD);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

/* robust write - handle partial writes and EINTR */
ssize_t robust_write(int fd, const void *buf, size_t count) {
    const unsigned char *p = buf;
    size_t left = count;
    while (left > 0) {
        ssize_t w = write(fd, p, left);
        if (w < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) { usleep(1000); continue; }
            return -1;
        }
        p += w;
        left -= (size_t)w;
    }
    return (ssize_t)count;
}

/* robust read-all helper for fixed-size reads */
static ssize_t robust_read_all(int fd, void *buf, size_t len) {
    unsigned char *p = buf;
    size_t left = len;
    while (left > 0) {
        ssize_t r = read(fd, p, left);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (r == 0) return (ssize_t)(len - left); // peer closed
        p += r;
        left -= (size_t)r;
    }
    return (ssize_t)len;
}

/* Create PTY pair */
int create_pty_pair(int *master, char *slave_name_buf, size_t slave_name_size) {
    *master = open("/dev/ptmx", O_RDWR | O_NOCTTY);
    if (*master < 0) {
        perror("open /dev/ptmx");
        return -1;
    }
    if (set_cloexec(*master) < 0) {
        log_msg("Warning: failed to FD_CLOEXEC pty master: %s\n", strerror(errno));
    }
    if (grantpt(*master) < 0 || unlockpt(*master) < 0) {
        perror("pty setup (grantpt/unlockpt)");
        close(*master);
        return -1;
    }
    if (ptsname_r(*master, slave_name_buf, slave_name_size) < 0) {
        perror("ptsname_r");
        close(*master);
        return -1;
    }
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

/* termios helpers */
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

/* client disconnect cleanup */
void handle_client_disconnect(int client_idx) {
    pthread_mutex_lock(&sp.lock);
    Client *c = &sp.clients[client_idx];
    if (!c->active) {
        pthread_mutex_unlock(&sp.lock);
        return;
    }
    log_msg("Client PID %d disconnected\n", c->pid);

    /* The PTY master FD is owned by the relay thread and closed there.
     * We just mark the client as inactive to signal the threads to exit. */
    c->active = 0;
    c->pid = -1;
    c->paused = 0;
    if (c->priority == PRIORITY_HIGH) {
        sp.num_high_priority--;
    }
    pthread_mutex_unlock(&sp.lock);
}

/* pty data relay thread */
void* pty_relay_thread(void *arg) {
    int client_idx = *(int*)arg;
    free(arg);
    Client *c = &sp.clients[client_idx];

    unsigned char buf[BUF_SIZE];
    fd_set readfds;

    log_msg("Started relay thread for client %d (PID %d, PTY_FD %d)\n",
            client_idx, c->pid, c->pty_master_fd);

    int pty_fd = c->pty_master_fd;
    if (pty_fd < 0) {
        log_msg("Relay thread for client %d started with invalid pty_fd\n", client_idx);
        handle_client_disconnect(client_idx);
        return NULL;
    }

    while (running && c->active) {
        pthread_mutex_lock(&sp.lock);
        int paused = c->paused;
        int real_fd = sp.real_fd;
        pthread_mutex_unlock(&sp.lock);

        FD_ZERO(&readfds);
        FD_SET(real_fd, &readfds);
        FD_SET(pty_fd, &readfds);
        int max_fd = (real_fd > pty_fd ? real_fd : pty_fd);

        struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
        int ret = select(max_fd + 1, &readfds, NULL, NULL, &tv);
        if (ret < 0) {
            if (errno == EINTR) continue;
            perror("select (pty_relay)");
            break;
        }
        if (ret == 0) continue;

        if (FD_ISSET(real_fd, &readfds)) {
            if (!paused) {
                ssize_t n = read(real_fd, buf, sizeof(buf));
                if (n > 0) {
                    ssize_t w = write(pty_fd, buf, n);
                    (void)w;
                } else if (n == 0) {
                    log_msg("Real serial port hung up. Terminating relay for client %d.\n", c->pid);
                    break;
                }
                else {
                    log_msg("Real serial port error (read=%zd). Terminating relay for client %d.\n", n, c->pid);
                    break;
                }
            }
        }
        if (FD_ISSET(pty_fd, &readfds)) {
            ssize_t n = read(pty_fd, buf, sizeof(buf));
            if (n > 0) {
                if (!paused) {
                    robust_write(real_fd, buf, n);
                }
            } else {
                // PTY closed
                break;
            }
        }
    }

    /* Close PTY master here, as we are the owner of this FD */
    if (pty_fd >= 0) {
        close(pty_fd);
    }
    pthread_mutex_lock(&sp.lock);
    if (c->pty_master_fd == pty_fd) {
        c->pty_master_fd = -1;
    }
    pthread_mutex_unlock(&sp.lock);

    handle_client_disconnect(client_idx);
    return NULL;
}

/* ---------- Binary control handling (SMIO protocol) ---------- */
static int handle_binary_request(int ctrl_fd) {
    struct sm_header hdr;
    if (robust_read_all(ctrl_fd, &hdr, sizeof(hdr)) != sizeof(hdr)) return -1;

    if (memcmp(hdr.magic, SM_MAGIC, sizeof(SM_MAGIC)) != 0) {
        log_msg("Invalid magic number\n");
        return -1;
    }

    struct sm_response resp = {0};
    void* payload = NULL;

    if (hdr.payload_len > 0) {
        payload = malloc(hdr.payload_len);
        if (!payload) {
            resp.rc = -1;
            resp.errno_val = ENOMEM;
            robust_write(ctrl_fd, &resp, sizeof(resp));
            return -1;
        }
        if (robust_read_all(ctrl_fd, payload, hdr.payload_len) != (ssize_t)hdr.payload_len) {
            free(payload);
            return -1;
        }
    }

    pthread_mutex_lock(&sp.lock);
    int real_fd = sp.real_fd;
    pthread_mutex_unlock(&sp.lock);

    switch (hdr.type) {
        case REQ_IOCTL: {
            struct sm_ioctl_req *req = payload;
            void *argp = NULL;
            int value;
            if (req->arg_type == ARG_BUFFER) {
                argp = (void*)((char*)payload + sizeof(struct sm_ioctl_req));
            } else if (req->arg_type == ARG_VALUE) {
                memcpy(&value, (char*)payload + sizeof(struct sm_ioctl_req), sizeof(int));
                argp = &value;
            }

            resp.rc = ioctl(real_fd, req->request, argp);
            resp.errno_val = (resp.rc < 0) ? errno : 0;

            if (resp.rc >= 0 && req->arg_type == ARG_BUFFER && (_IOC_DIR(req->request) & _IOC_READ)) {
                resp.payload_len = req->arg_len;
            }
            break;
        }
        case REQ_TCFLSH:
        case REQ_TCSENDBREAK: {
            int arg = *(int*)payload;
            if (hdr.type == REQ_TCFLSH) {
                resp.rc = tcflush(real_fd, arg);
            } else {
                resp.rc = tcsendbreak(real_fd, arg);
            }
            resp.errno_val = (resp.rc < 0) ? errno : 0;
            break;
        }
        case REQ_TCDRAIN:
            resp.rc = tcdrain(real_fd);
            resp.errno_val = (resp.rc < 0) ? errno : 0;
            break;
        default:
            resp.rc = -1;
            resp.errno_val = ENOSYS;
            break;
    }

    robust_write(ctrl_fd, &resp, sizeof(resp));
    if (resp.payload_len > 0) {
        robust_write(ctrl_fd, (void*)((char*)payload + sizeof(struct sm_ioctl_req)), resp.payload_len);
    }

    if (payload) free(payload);
    return 0;
}


/* control thread: accept either text commands (OPEN/CLOSE) or binary SMIO requests.
   Uses MSG_PEEK to detect binary magic without consuming it.
*/
void* client_control_thread(void *arg) {
    int client_fd = *(int*)arg;
    free(arg);

    // Peek to detect SMIO binary magic
    char magic_peek[4];
    ssize_t p = recv(client_fd, &magic_peek, sizeof(magic_peek), MSG_PEEK | MSG_DONTWAIT);
    if (p == (ssize_t)sizeof(magic_peek) && memcmp(magic_peek, SM_MAGIC, sizeof(SM_MAGIC)) == 0) {
        handle_binary_request(client_fd);
        close(client_fd);
        return NULL;
    }

    // Not a binary magic: treat as text control (OPEN/CLOSE)
    char buf[512];
    ssize_t n = recv(client_fd, buf, sizeof(buf) - 1, 0);
    if (n <= 0) {
        close(client_fd);
        return NULL;
    }
    buf[n] = '\0';

    if (strncmp(buf, "OPEN:", 5) == 0) {
        // Format: OPEN:device:prio:pid
        char *saveptr = NULL;
        char *device = strtok_r(buf + 5, ":\n", &saveptr);
        char *prio_s = strtok_r(NULL, ":\n", &saveptr);
        char *pid_s = strtok_r(NULL, ":\n", &saveptr);
        Priority prio = PRIORITY_LOW;
        pid_t supplied_pid = -1;
        if (prio_s) prio = (atoi(prio_s) == 1) ? PRIORITY_HIGH : PRIORITY_LOW;
        if (pid_s) supplied_pid = (pid_t)atoi(pid_s);

        pid_t peer_pid = supplied_pid;
        if (!device || strcmp(device, sp.device) != 0) {
            log_msg("Client PID %d requested wrong device '%s'\n", (int)peer_pid, device ? device : "(null)");
            write(client_fd, "ERROR:Wrong device", strlen("ERROR:Wrong device") + 1);
            close(client_fd);
            return NULL;
        }

        pthread_mutex_lock(&sp.lock);
        int idx = -1;
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (!sp.clients[i].active) { idx = i; break; }
        }
        if (idx == -1) {
            log_msg("Max clients reached, rejecting PID %d\n", (int)peer_pid);
            write(client_fd, "ERROR:Max clients", strlen("ERROR:Max clients") + 1);
            pthread_mutex_unlock(&sp.lock);
            close(client_fd);
            return NULL;
        }

        char pty_slave_name[256];
        int pty_master;
        if (create_pty_pair(&pty_master, pty_slave_name, sizeof(pty_slave_name)) < 0) {
            log_msg("Failed to create PTY for PID %d\n", (int)peer_pid);
            write(client_fd, "ERROR:PTY creation failed", strlen("ERROR:PTY creation failed") + 1);
            pthread_mutex_unlock(&sp.lock);
            close(client_fd);
            return NULL;
        }

        // send PTY slave path (NUL terminated)
        write(client_fd, pty_slave_name, strlen(pty_slave_name) + 1);

        // configure client slot
        Client *c = &sp.clients[idx];
        c->active = 1;
        c->pid = peer_pid;
        c->priority = prio;
        c->pty_master_fd = pty_master;
        c->paused = 0;

        // Apply current settings to the new PTY
        restore_termios(c->pty_master_fd, &sp.current_settings);

        pthread_mutex_unlock(&sp.lock);

        // spawn threads for this client
        pthread_t relay_tid;
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
            handle_client_disconnect(idx);
            close(c->pty_master_fd);
            c->pty_master_fd = -1;
            free(pidx);
            close(client_fd);
            return NULL;
        }
        pthread_detach(relay_tid);

    } else if (strncmp(buf, "CLOSE:", 6) == 0) {
        pid_t pid = (pid_t)atoi(buf + 6);
        pthread_mutex_lock(&sp.lock);
        int idx = -1;
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (sp.clients[i].active && sp.clients[i].pid == pid) { idx = i; break; }
        }
        pthread_mutex_unlock(&sp.lock);
        if (idx != -1) {
            log_msg("Received CLOSE from PID %d, initiating disconnect.\n", (int)pid);
            handle_client_disconnect(idx);
        } else {
            log_msg("CLOSE: unknown pid %d\n", (int)pid);
        }
    } else {
        // unknown text - ignore
        log_msg("Unknown control message: %.*s\n", (int)n, buf);
    }

    close(client_fd);
    return NULL;
}

/* Signal handler: write to pipe to wake select and set running=0 */
void handle_signal(int sig) {
    running = 0;
    log_msg("Caught signal %d, shutting down.\n", sig);
    if (sig_pipe_fds[1] != -1) {
        const char c = 'x';
        write(sig_pipe_fds[1], &c, 1);
    }
}

/* main */
int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s /dev/ttyUSB0\n", argv[0]);
        exit(1);
    }

    signal(SIGPIPE, SIG_IGN);
    struct sigaction sa;
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    memset(&sp, 0, sizeof(sp));
    pthread_mutex_init(&sp.lock, NULL);
    sp.num_high_priority = 0;

    sp.real_fd = open(argv[1], O_RDWR | O_NOCTTY);
    if (sp.real_fd < 0) {
        perror("open serial port");
        exit(1);
    }
    if (set_cloexec(sp.real_fd) < 0) {
        log_msg("Warning: failed to set FD_CLOEXEC on real fd: %s\n", strerror(errno));
    }
    strncpy(sp.device, argv[1], sizeof(sp.device) - 1);
    save_termios(sp.real_fd, &sp.current_settings);
    save_termios(sp.real_fd, &original_real_settings);

    if (pipe(sig_pipe_fds) < 0) {
        perror("pipe");
        close(sp.real_fd);
        exit(1);
    }
    set_cloexec(sig_pipe_fds[0]);
    set_cloexec(sig_pipe_fds[1]);

    unlink(SOCKET_PATH);
    int socket_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (socket_fd < 0) {
        perror("socket");
        close(sp.real_fd);
        exit(1);
    }
    set_cloexec(socket_fd);

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (bind(socket_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(socket_fd);
        close(sp.real_fd);
        exit(1);
    }
    /* restrict socket permissions to owner read/write */
    chmod(SOCKET_PATH, S_IRUSR | S_IWUSR);

    if (listen(socket_fd, MAX_CLIENTS) < 0) {
        perror("listen");
        close(socket_fd);
        close(sp.real_fd);
        unlink(SOCKET_PATH);
        exit(1);
    }

    log_msg("Serial mux daemon started on %s\n", argv[1]);
    log_msg("Listening on %s\n", SOCKET_PATH);

    while (running) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(socket_fd, &readfds);
        FD_SET(sig_pipe_fds[0], &readfds);
        int maxfd = socket_fd > sig_pipe_fds[0] ? socket_fd : sig_pipe_fds[0];
        struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
        int ret = select(maxfd + 1, &readfds, NULL, NULL, &tv);
        if (ret < 0) {
            if (errno == EINTR) continue;
            perror("select (main)");
            break;
        }
        if (ret == 0) continue;

        if (FD_ISSET(sig_pipe_fds[0], &readfds)) {
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
            set_cloexec(client_fd);

            // spawn control thread
            pthread_t tid;
            int *pfd = malloc(sizeof(int));
            if (!pfd) {
                log_msg("malloc failed for control thread arg\n");
                close(client_fd);
                continue;
            }
            *pfd = client_fd;
            if (pthread_create(&tid, NULL, client_control_thread, pfd) != 0) {
                log_msg("pthread_create failed for control thread\n");
                free(pfd);
                close(client_fd);
                continue;
            }
            pthread_detach(tid);
        }
    }

    log_msg("Shutting down: restoring original termios\n");
    restore_termios(sp.real_fd, &original_real_settings);

    pthread_mutex_lock(&sp.lock);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (sp.clients[i].active && sp.clients[i].pty_master_fd >= 0) {
            close(sp.clients[i].pty_master_fd);
            sp.clients[i].pty_master_fd = -1;
        }
    }
    pthread_mutex_unlock(&sp.lock);

    close(sp.real_fd);
    close(socket_fd);
    unlink(SOCKET_PATH);
    if (sig_pipe_fds[0] >= 0) close(sig_pipe_fds[0]);
    if (sig_pipe_fds[1] >= 0) close(sig_pipe_fds[1]);

    log_msg("Daemon shut down\n");
    return 0;
}
