// serialmux_daemon.c
// Main daemon for serialmux. Manages the physical serial port and handles client connections.
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
#include <pthread.h>
#include <signal.h>
#include <errno.h>
#include <sys/epoll.h>
#include <byteswap.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <stdint.h>
#include "serialmux.h"

#define DATA_SOCKET_PATH "/tmp/serialmux.sock"
#define CTRL_SOCKET_PATH "/tmp/serialmux_ctrl.sock"
#define DAEMON_DEVICE_ENV "SERIALMUX_DEVICE"
#define MAX_CLIENTS 10
#define MAX_EPOLL_EVENTS (MAX_CLIENTS + 1)
#define MAGIC 0x534d494f

static int running = 1;
static int pty_fd = -1;

void handle_signal(int sig) {
    running = 0;
    (void)sig; // Unused parameter
}

/* robust send/recv helpers */
static ssize_t send_all(int sock, const void *buf, size_t len) {
    const unsigned char *p = buf;
    size_t left = len;
    while (left > 0) {
        ssize_t s = send(sock, p, left, MSG_NOSIGNAL);
        if (s < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += s;
        left -= (size_t)s;
    }
    return (ssize_t)len;
}

static ssize_t recv_all(int sock, void *buf, size_t len) {
    unsigned char *p = buf;
    size_t left = len;
    while (left > 0) {
        ssize_t r = recv(sock, p, left, 0);
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

void *ioctl_thread(void *arg) {
    int sock = (int)(intptr_t)arg;

    while (running) {
        uint32_t magic_n, reqid_n, arg_len_n;
        uint64_t ioc_be;
        if (recv_all(sock, &magic_n, 4) != 4) break;
        if (ntohl(magic_n) != MAGIC) break;
        if (recv_all(sock, &reqid_n, 4) != 4) break;
        if (recv_all(sock, &ioc_be, 8) != 8) break;
        if (recv_all(sock, &arg_len_n, 4) != 4) break;

        uint32_t arg_len = ntohl(arg_len_n);
        unsigned long request = (unsigned long)be64toh(ioc_be);

        char arg_buf[4096];
        void *argp = NULL;
        if (arg_len > 0) {
            if (arg_len > sizeof(arg_buf)) {
                fprintf(stderr, "DAEMON: ioctl arg too big %u\n", arg_len);
                break;
            }
            if (recv_all(sock, arg_buf, arg_len) != (ssize_t)arg_len) break;
            argp = arg_buf;
        }

        int ret = ioctl(pty_fd, request, argp);
        int err = errno;

        uint32_t resp_outlen = 0;
        if (ret == 0 && (_IOC_DIR(request) & _IOC_READ)) {
            resp_outlen = ioctl_arg_size(request);
        }

        int32_t net_ret = htonl(ret);
        int32_t net_errno = htonl(err);
        uint32_t net_resp_outlen = htonl(resp_outlen);

        if (send_all(sock, &reqid_n, 4) < 0) break;
        if (send_all(sock, &net_ret, 4) < 0) break;
        if (send_all(sock, &net_errno, 4) < 0) break;
        if (send_all(sock, &net_resp_outlen, 4) < 0) break;
        if (resp_outlen > 0) {
            if (send_all(sock, argp, resp_outlen) < 0) break;
        }
    }

    close(sock);
    return NULL;
}

void *accept_thread(void *arg) {
    int sock = (int)(intptr_t)arg;
    while(running) {
        int client_fd = accept(sock, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            perror("accept(ctrl_sock)");
            break;
        }
        pthread_t tid;
        pthread_create(&tid, NULL, ioctl_thread, (void*)(intptr_t)client_fd);
        pthread_detach(tid);
    }
    return NULL;
}

int create_unix_socket(const char *path) {
    struct sockaddr_un addr;
    int fd;

    if ((fd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
        perror("socket error");
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path)-1);
    unlink(path);

    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
        perror("bind error");
        close(fd);
        return -1;
    }

    if (listen(fd, 5) == -1) {
        perror("listen error");
        close(fd);
        return -1;
    }
    return fd;
}

int main(void) {
    char *device = getenv(DAEMON_DEVICE_ENV);
    if (!device) {
        fprintf(stderr, "DAEMON: %s not set\n", DAEMON_DEVICE_ENV);
        return 1;
    }

    pty_fd = open(device, O_RDWR | O_NOCTTY);
    if (pty_fd < 0) {
        perror("DAEMON: Failed to open pty device");
        return 1;
    }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    int data_sock = create_unix_socket(DATA_SOCKET_PATH);
    if (data_sock < 0) return 1;
    int ctrl_sock = create_unix_socket(CTRL_SOCKET_PATH);
    if (ctrl_sock < 0) { close(data_sock); return 1; }

    pthread_t ioctl_tid;

    int epoll_fd = epoll_create1(0);
    if (epoll_fd < 0) {
        perror("epoll_create1");
        return 1;
    }

    struct epoll_event ev, events[MAX_EPOLL_EVENTS];
    ev.events = EPOLLIN;
    ev.data.fd = data_sock;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, data_sock, &ev) < 0) {
        perror("epoll_ctl: data_sock");
        return 1;
    }

    ev.data.fd = pty_fd;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, pty_fd, &ev) < 0) {
        perror("epoll_ctl: pty_fd");
        return 1;
    }

    int client_fds[MAX_CLIENTS] = {0};
    int num_clients = 0;

    // Control connection handling
    pthread_create(&ioctl_tid, NULL, accept_thread, (void*)(intptr_t)ctrl_sock);

    char buf[4096];
    while(running) {
        int n = epoll_wait(epoll_fd, events, MAX_EPOLL_EVENTS, 100);
        for (int i=0; i<n; ++i) {
            if (events[i].data.fd == data_sock) { // New client
                int client_fd = accept(data_sock, NULL, NULL);
                if (client_fd >= 0 && num_clients < MAX_CLIENTS) {
                    ev.events = EPOLLIN;
                    ev.data.fd = client_fd;
                    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &ev);
                    client_fds[num_clients++] = client_fd;
                } else if (client_fd >= 0) {
                    close(client_fd);
                }
            } else if (events[i].data.fd == pty_fd) { // Data from PTY
                ssize_t bytes = read(pty_fd, buf, sizeof(buf));
                if (bytes > 0) {
                    for(int j=0; j<num_clients; ++j) {
                        send_all(client_fds[j], buf, bytes);
                    }
                }
            } else { // Data from a client
                int client_fd = events[i].data.fd;
                ssize_t bytes = read(client_fd, buf, sizeof(buf));
                if (bytes <= 0) {
                    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client_fd, NULL);
                    close(client_fd);
                    for(int j=0; j<num_clients; ++j) {
                        if (client_fds[j] == client_fd) {
                            client_fds[j] = client_fds[num_clients-1];
                            num_clients--;
                            break;
                        }
                    }
                } else {
                    send_all(pty_fd, buf, bytes);
                }
            }
        }
    }

    for(int i=0; i<num_clients; ++i) close(client_fds[i]);
    close(epoll_fd);
    close(data_sock);
    close(ctrl_sock);
    close(pty_fd);
    unlink(DATA_SOCKET_PATH);
    unlink(CTRL_SOCKET_PATH);

    return 0;
}
