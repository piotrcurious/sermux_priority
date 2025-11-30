// serialmux_daemon.c
// Main daemon for serialmux.
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

#define MAX_CLIENTS 32
#define MAX_EPOLL_EVENTS (MAX_CLIENTS + 2)

static int running = 1;
static int pty_fd = -1;

void handle_signal(int sig) {
    running = 0;
    (void)sig; 
}

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
        if (r == 0) return (ssize_t)(len - left); 
        p += r;
        left -= (size_t)r;
    }
    return (ssize_t)len;
}

void *ioctl_thread(void *arg) {
    int sock = (int)(intptr_t)arg;

    while (running) {
        uint32_t magic_n, reqid_n, arg_len_n;
        uint64_t ioc_be, arg_val_be;
        
        // 1. Receive Header
        if (recv_all(sock, &magic_n, 4) != 4) break;
        if (ntohl(magic_n) != MAGIC) break;
        if (recv_all(sock, &reqid_n, 4) != 4) break;
        if (recv_all(sock, &ioc_be, 8) != 8) break;
        if (recv_all(sock, &arg_val_be, 8) != 8) break; // New: Raw Value
        if (recv_all(sock, &arg_len_n, 4) != 4) break;

        uint32_t arg_len = ntohl(arg_len_n);
        unsigned long request = (unsigned long)be64toh(ioc_be);
        uint64_t arg_raw_val = be64toh(arg_val_be);

        // 2. Receive Payload (if any)
        char *arg_buf = NULL;
        if (arg_len > 0) {
            arg_buf = malloc(arg_len);
            if (!arg_buf) {
                // OOM, try to drain socket then break or send error?
                break;
            }
            if (recv_all(sock, arg_buf, arg_len) != (ssize_t)arg_len) {
                free(arg_buf);
                break;
            }
        }

        // 3. Execute IOCTL
        int ret;
        void *real_arg;

        if (arg_len > 0) {
            // It's a pointer type ioctl (like TCSETS), use the buffer
            real_arg = arg_buf;
        } else {
            // It's a value type ioctl (like TCFLSH), use the raw value
            // We cast to uintptr_t first to satisfy pointer sizing, then void*
            real_arg = (void*)(uintptr_t)arg_raw_val;
        }

        ret = ioctl(pty_fd, request, real_arg);
        int err = errno;

        // 4. Prepare Response
        uint32_t resp_outlen = 0;
        
        // If it was a READ ioctl (like TCGETS), we need to send data back
        if (ret == 0 && arg_len > 0 && (_IOC_DIR(request) & _IOC_READ)) {
            resp_outlen = arg_len; // Assuming size matches request size
        }

        int32_t net_ret = htonl(ret);
        int32_t net_errno = htonl(err);
        uint32_t net_resp_outlen = htonl(resp_outlen);

        // 5. Send Response
        if (send_all(sock, &reqid_n, 4) < 0) { if(arg_buf) free(arg_buf); break; }
        if (send_all(sock, &net_ret, 4) < 0) { if(arg_buf) free(arg_buf); break; }
        if (send_all(sock, &net_errno, 4) < 0) { if(arg_buf) free(arg_buf); break; }
        if (send_all(sock, &net_resp_outlen, 4) < 0) { if(arg_buf) free(arg_buf); break; }
        
        if (resp_outlen > 0 && arg_buf) {
            if (send_all(sock, arg_buf, resp_outlen) < 0) { free(arg_buf); break; }
        }

        if (arg_buf) free(arg_buf);
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
            perror("DAEMON: accept(ctrl_sock)");
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
    signal(SIGPIPE, SIG_IGN); // Ignore SIGPIPE to handle disconnects gracefully

    int data_sock = create_unix_socket(DATA_SOCKET_PATH);
    if (data_sock < 0) return 1;
    int ctrl_sock = create_unix_socket(CTRL_SOCKET_PATH);
    if (ctrl_sock < 0) { close(data_sock); return 1; }

    pthread_t ioctl_tid;
    pthread_create(&ioctl_tid, NULL, accept_thread, (void*)(intptr_t)ctrl_sock);

    int epoll_fd = epoll_create1(0);
    struct epoll_event ev, events[MAX_EPOLL_EVENTS];

    ev.events = EPOLLIN;
    ev.data.fd = data_sock;
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, data_sock, &ev);

    ev.data.fd = pty_fd;
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, pty_fd, &ev);

    int client_fds[MAX_CLIENTS] = {0};
    int num_clients = 0;

    char buf[4096];
    while(running) {
        int n = epoll_wait(epoll_fd, events, MAX_EPOLL_EVENTS, 100);
        for (int i=0; i<n; ++i) {
            
            // 1. New Data Connection
            if (events[i].data.fd == data_sock) {
                int client_fd = accept(data_sock, NULL, NULL);
                if (client_fd >= 0) {
                    if (num_clients < MAX_CLIENTS) {
                        ev.events = EPOLLIN;
                        ev.data.fd = client_fd;
                        epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &ev);
                        client_fds[num_clients++] = client_fd;
                    } else {
                        close(client_fd); // Too many clients
                    }
                }
            } 
            // 2. Data from Physical Device -> Broadcast to all clients
            else if (events[i].data.fd == pty_fd) {
                ssize_t bytes = read(pty_fd, buf, sizeof(buf));
                if (bytes > 0) {
                    for(int j=0; j<num_clients; ++j) {
                        // Note: A blocking send here is risky in production. 
                        // If one client stalls, everyone stalls. 
                        send_all(client_fds[j], buf, bytes);
                    }
                }
            } 
            // 3. Data from Client -> Write to Physical Device
            else {
                int client_fd = events[i].data.fd;
                ssize_t bytes = read(client_fd, buf, sizeof(buf));
                if (bytes <= 0) {
                    // Client Disconnected
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
