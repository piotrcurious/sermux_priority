// serialmux_daemon.c - Serial mux daemon with ioctl and serial-control forwarding support
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
#ifdef __linux__
#include <sys/uio.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/types.h>
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
    struct termios saved_settings;      // The settings this client *wants*
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
volatile sig_atomic_t running = 1;
static int sig_pipe_fds[2] = { -1, -1 };

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
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

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
        perror("pty setup");
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

void update_low_priority_saves(struct termios *new_settings) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (sp.clients[i].active && sp.clients[i].priority == PRIORITY_LOW) {
            memcpy(&sp.clients[i].saved_settings, new_settings, sizeof(struct termios));
        }
    }
    memcpy(&sp.current_settings, new_settings, sizeof(struct termios));
}

void pause_low_priority_clients() {
    log_msg("High priority client active. Pausing low priority clients.\n");
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (sp.clients[i].active && sp.clients[i].priority == PRIORITY_LOW) {
            sp.clients[i].paused = 1;
            log_msg("Paused low priority client PID %d\n", sp.clients[i].pid);
        }
    }
}

void resume_low_priority_clients() {
    log_msg("Last high priority client disconnected. Resuming low priority clients.\n");
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
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (sp.clients[i].active && sp.clients[i].priority == PRIORITY_LOW) {
            sp.clients[i].paused = 0;
            log_msg("Resumed low priority client PID %d\n", sp.clients[i].pid);
        }
    }
}

void handle_client_ioctl_settings(int client_idx, struct termios *new_settings) {
    pthread_mutex_lock(&sp.lock);
    Client *c = &sp.clients[client_idx];
    if (!c->active) {
        pthread_mutex_unlock(&sp.lock);
        return;
    }
    if (c->priority == PRIORITY_HIGH) {
        restore_termios(sp.real_fd, new_settings);
        update_low_priority_saves(new_settings);
        log_msg("High priority PID %d applied new termios settings\n", c->pid);
    } else {
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

void *pty_ioctl_thread(void *arg) {
    int client_idx = *(int*)arg;
    free(arg);
    Client *c = &sp.clients[client_idx];

    while (running && c->active) {
        struct termios new_settings;
        if (tcgetattr(c->pty_master_fd, &new_settings) < 0) {
            break;
        }
        if (memcmp(&c->current_pty_settings, &new_settings, sizeof(new_settings)) != 0) {
            memcpy(&c->current_pty_settings, &new_settings, sizeof(new_settings));
            handle_client_ioctl_settings(client_idx, &new_settings);
        }
        usleep(50000);
    }
    handle_client_disconnect(client_idx);
    return NULL;
}

void *pty_relay_thread(void *arg) {
    int client_idx = *(int*)arg;
    free(arg);
    Client *c = &sp.clients[client_idx];

    unsigned char buf[BUF_SIZE];
    fd_set readfds;

    log_msg("Started relay thread for client %d (PID %d, PTY_FD %d)\n",
            client_idx, c->pid, c->pty_master_fd);

    while (running && c->active) {
        pthread_mutex_lock(&sp.lock);
        int paused = c->paused;
        int pty_fd = c->pty_master_fd;
        int real_fd = sp.real_fd;
        pthread_mutex_unlock(&sp.lock);

        if (pty_fd < 0) break;

        FD_ZERO(&readfds);
        FD_SET(real_fd, &readfds);
        FD_SET(pty_fd, &readfds);
        int max_fd = (real_fd > pty_fd ? real_fd : pty_fd);
        struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
        int ret = select(max_fd+1, &readfds, NULL, NULL, &tv);
        if (ret < 0) {
            if (errno == EINTR) continue;
            perror("select");
            break;
        }
        if (ret == 0) continue;

        if (FD_ISSET(real_fd, &readfds)) {
            if (!paused) {
                ssize_t n = read(real_fd, buf, sizeof(buf));
                if (n > 0) {
                    write(pty_fd, buf, n);
                } else {
                    log_msg("Real serial port error/closed (read result %zd)\n", n);
                    running = 0;
                    break;
                }
            }
        }
        if (FD_ISSET(pty_fd, &readfds)) {
            ssize_t n = read(pty_fd, buf, sizeof(buf));
            if (n > 0) {
                if (!paused) {
                    write(real_fd, buf, n);
                }
            } else {
                break;
            }
        }
    }

    handle_client_disconnect(client_idx);
    return NULL;
}

/* New: handle binary control requests for ioctl/tc*/ 
static int handle_binary_request(int ctrl_fd) {
    char magic[4];
    if (read(ctrl_fd, magic, 4) != 4) return -1;
    if (memcmp(magic, "SMIO", 4) != 0) return -1;

    uint32_t u32;
    if (read(ctrl_fd, &u32, sizeof(u32)) != sizeof(u32)) return -1;
    uint32_t req_type = u32;

    int rc = -1;
    int err_no = 0;
    uint32_t out_arglen = 0;
    unsigned char outbuf[64] = {0};

    pthread_mutex_lock(&sp.lock);
    int real_fd = sp.real_fd;
    pthread_mutex_unlock(&sp.lock);

    switch (req_type) {
        case 1: { // REQ_IOCTL
            uint64_t req64 = 0;
            if (read(ctrl_fd, &req64, sizeof(req64)) != sizeof(req64)) { err_no = EIO; break; }
            uint32_t arglen = 0;
            if (read(ctrl_fd, &arglen, sizeof(arglen)) != sizeof(arglen)) { err_no = EIO; break; }
            unsigned char argbuf[64];
            if (arglen > 0) {
                if (arglen > sizeof(argbuf)) { err_no = EINVAL; break; }
                if (read(ctrl_fd, argbuf, arglen) != arglen) { err_no = EIO; break; }
            }
            unsigned long request = (unsigned long)req64;
            void *argp = NULL;
            int local_int = 0;
            if (arglen >= sizeof(int)) {
                memcpy(&local_int, argbuf, sizeof(int));
                argp = &local_int;
            }
            // perform ioctl
            rc = ioctl(real_fd, request, argp);
            if (rc < 0) {
                err_no = errno;
            } else {
                // for TIOCMGET maybe produce output
                if (request == TIOCMGET && argp) {
                    int val = local_int;
                    memcpy(outbuf, &val, sizeof(int));
                    out_arglen = sizeof(int);
                }
            }
            break;
        }
        case 2: { // REQ_TCFLSH
            int32_t sel = 0;
            if (read(ctrl_fd, &sel, sizeof(sel)) != sizeof(sel)) { err_no = EIO; break; }
            if (tcflush(real_fd, sel) < 0) {
                rc = -1; err_no = errno;
            } else {
                rc = 0; err_no = 0;
            }
            break;
        }
        case 3: { // REQ_TCSENDBREAK
            int32_t dur = 0;
            if (read(ctrl_fd, &dur, sizeof(dur)) != sizeof(dur)) { err_no = EIO; break; }
            if (tcsendbreak(real_fd, dur) < 0) {
                rc = -1; err_no = errno;
            } else {
                rc = 0; err_no = 0;
            }
            break;
        }
        case 4: { // REQ_TCDRAIN
            if (tcdrain(real_fd) < 0) {
                rc = -1; err_no = errno;
            } else {
                rc = 0; err_no = 0;
            }
            break;
        }
        default:
            err_no = ENOSYS;
            break;
    }

    // send response
    int32_t s_rc = rc;
    int32_t s_errno = err_no;
    uint32_t s_out_arglen = out_arglen;
    robust_write(ctrl_fd, &s_rc, sizeof(s_rc));
    robust_write(ctrl_fd, &s_errno, sizeof(s_errno));
    robust_write(ctrl_fd, &s_out_arglen, sizeof(s_out_arglen));
    if (out_arglen > 0) {
        robust_write(ctrl_fd, outbuf, out_arglen);
    }
    return 0;
}

/* control thread: handle both OPEN/CLOSE commands (text) and binary requests */
void *client_control_thread(void *arg) {
    int client_fd = *(int*)arg;
    free(arg);

    // get peer credentials (linux)
#ifdef SO_PEERCRED
    struct ucred cred;
    socklen_t credlen = sizeof(cred);
    if (getsockopt(client_fd, SOL_SOCKET, SO_PEERCRED, &cred, &credlen) == 0) {
        log_msg("Control connection: peer pid=%d uid=%d gid=%d\n", cred.pid, cred.uid, cred.gid);
    }
#endif

    char buf[512];
    ssize_t n = read(client_fd, buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        if (strncmp(buf, "OPEN:", 5) == 0) {
            // existing OPEN logic (unchanged)...

            char *device = strtok(buf + 5, ":\n");
            char *prio_s = strtok(NULL, ":\n");
            char *pid_s = strtok(NULL, ":\n");
            Priority prio = PRIORITY_LOW;
            pid_t pid = -1;
            if (prio_s) prio = (atoi(prio_s) == 1 ? PRIORITY_HIGH : PRIORITY_LOW);
            if (pid_s) pid = (pid_t)atoi(pid_s);

            if (!device || strcmp(device, sp.device) != 0) {
                log_msg("Client PID %d requested wrong device '%s'\n", (int)pid, device ? device : "(null)");
                write(client_fd, "ERROR:Wrong device", strlen("ERROR:Wrong device")+1);
                close(client_fd);
                return NULL;
            }

            pthread_mutex_lock(&sp.lock);
            int idx = -1;
            for (int i = 0; i < MAX_CLIENTS; i++) {
                if (!sp.clients[i].active) { idx = i; break; }
            }
            if (idx < 0) {
                log_msg("Max clients reached, rejecting PID %d\n", (int)pid);
                write(client_fd, "ERROR:Max clients", strlen("ERROR:Max clients")+1);
                pthread_mutex_unlock(&sp.lock);
                close(client_fd);
                return NULL;
            }

            char pty_slave_name[256];
            int pty_master;
            if (create_pty_pair(&pty_master, pty_slave_name, sizeof(pty_slave_name)) < 0) {
                log_msg("Failed to create PTY for PID %d\n", (int)pid);
                write(client_fd, "ERROR:PTY creation failed", strlen("ERROR:PTY creation failed")+1);
                pthread_mutex_unlock(&sp.lock);
                close(client_fd);
                return NULL;
            }

            write(client_fd, pty_slave_name, strlen(pty_slave_name)+1);

            Client *c = &sp.clients[idx];
            c->active = 1;
            c->pid = pid;
            c->priority = prio;
            c->pty_master_fd = pty_master;
            c->paused = 0;
            save_termios(c->pty_master_fd, &c->current_pty_settings);

            if (prio == PRIORITY_HIGH) {
                sp.num_high_priority++;
                pause_low_priority_clients();
                save_termios(sp.real_fd, &c->saved_settings);
                restore_termios(c->pty_master_fd, &c->saved_settings);
                memcpy(&c->current_pty_settings, &c->saved_settings, sizeof(struct termios));
            } else {
                save_termios(sp.real_fd, &c->saved_settings);
                if (sp.num_high_priority > 0) {
                    c->paused = 1;
                    log_msg("Low priority client PID %d connected (paused)\n", (int)c->pid);
                }
            }
            pthread_mutex_unlock(&sp.lock);

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
                if (sp.clients[i].active && sp.clients[i].pid == pid) { idx = i; break; }
            }
            pthread_mutex_unlock(&sp.lock);
            if (idx >= 0) {
                log_msg("Received CLOSE from PID %d, initiating disconnect.\n", (int)pid);
                handle_client_disconnect(idx);
            } else {
                log_msg("CLOSE: unknown pid %d\n", (int)pid);
            }
        } else {
            // Could be binary request for ioctl/tc* — read first four bytes to detect
            // Reset the read buffer and treat this fd as binary
            lseek(client_fd, 0, SEEK_SET); // optional
            handle_binary_request(client_fd);
        }
    } else {
        // maybe the client wants to send binary request directly
        handle_binary_request(client_fd);
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

    memset(&sp, 0, sizeof(sp));
    pthread_mutex_init(&sp.lock, NULL);

    sp.real_fd = open(argv[1], O_RDWR | O_NOCTTY);
    if (sp.real_fd < 0) {
        perror("open serial port");
        exit(1);
    }
    if (set_cloexec(sp.real_fd) < 0) {
        log_msg("Warning: failed to set FD_CLOEXEC on real fd: %s\n", strerror(errno));
    }

    strncpy(sp.device, argv[1], sizeof(sp.device)-1);
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
    memset(&addr,0,sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path)-1);

    if (bind(socket_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(socket_fd);
        close(sp.real_fd);
        exit(1);
    }
    chmod(SOCKET_PATH, S_IRUSR|S_IWUSR);

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
        struct timeval tv = {.tv_sec=1, .tv_usec=0};
        int ret = select(maxfd+1, &readfds, NULL, NULL, &tv);
        if (ret < 0) {
            if (errno == EINTR) continue;
            perror("select (main)");
            break;
        }
        if (ret == 0) continue;
        if (FD_ISSET(sig_pipe_fds[0], &readfds)) {
            char buf_d[64];
            read(sig_pipe_fds[0], buf_d, sizeof(buf_d));
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

            pthread_t tid;
            int *pfd = malloc(sizeof(int));
            *pfd = client_fd;
            pthread_create(&tid, NULL, client_control_thread, pfd);
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

    close(socket_fd);
    close(sp.real_fd);
    unlink(SOCKET_PATH);
    if (sig_pipe_fds[0] >= 0) close(sig_pipe_fds[0]);
    if (sig_pipe_fds[1] >= 0) close(sig_pipe_fds[1]);

    log_msg("Daemon shut down\n");
    return 0;
}
