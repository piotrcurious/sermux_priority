#ifndef SERIALMUX_H
#define SERIALMUX_H

#include <stdint.h>

#define SM_MAGIC 0x534d494f // "SMIO"

enum {
    REQ_IOCTL = 1,
    REQ_TCFLSH = 2,
    REQ_TCSENDBREAK = 3,
    REQ_TCDRAIN = 4
};

enum {
    ARG_NONE = 0,
    ARG_VALUE = 1,
    ARG_BUFFER = 2
};

struct sm_header {
    uint32_t magic;
    uint32_t type;
    uint32_t payload_len;
};

struct sm_ioctl_req {
    uint64_t request;
    uint32_t arg_type;
    uint32_t arg_len;
};

struct sm_response {
    int32_t rc;
    int32_t errno_val;
    uint32_t payload_len;
};

#endif // SERIALMUX_H
