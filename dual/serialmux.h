#ifndef SERIALMUX_H
#define SERIALMUX_H

#include <sys/ioctl.h>

#define DATA_SOCKET_PATH "/tmp/serialmux.sock"
#define CTRL_SOCKET_PATH "/tmp/serialmux_ctrl.sock"
#define DAEMON_DEVICE_ENV "SERIALMUX_DEVICE"
#define SERIALMUX_FALLBACK_ENV "SERIALMUX_FALLBACK"

#define MAGIC 0x534d494f

// Helper to determine size of ioctl argument
static inline size_t ioctl_arg_size(unsigned long request) {
    return _IOC_SIZE(request);
}

#endif // SERIALMUX_H
