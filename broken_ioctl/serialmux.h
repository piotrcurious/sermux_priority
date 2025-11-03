#ifndef SERIALMUX_H
#define SERIALMUX_H

#include <termios.h>
#include <sys/ioctl.h>

// These definitions are copied from <asm/termbits.h> to avoid header conflicts
#ifndef _UAPI_ASM_GENERIC_TERMBITS_H
#define _UAPI_ASM_GENERIC_TERMBITS_H

#ifdef NCCS
#undef NCCS
#endif
#define NCCS 19

struct termios2 {
	tcflag_t c_iflag;	/* input mode flags */
	tcflag_t c_oflag;	/* output mode flags */
	tcflag_t c_cflag;	/* control mode flags */
	tcflag_t c_lflag;	/* local mode flags */
	cc_t	 c_line;	/* line discipline */
	cc_t	 c_cc[NCCS];	/* control characters */
	speed_t	 c_ispeed;	/* input speed */
	speed_t	 c_ospeed;	/* output speed */
};
#define TCGETS2		_IOR('T', 0x2A, struct termios2)
#define TCSETS2		_IOW('T', 0x2B, struct termios2)
#define TCSETSW2	_IOW('T', 0x2C, struct termios2)
#define TCSETSF2	_IOW('T', 0x2D, struct termios2)
#endif


// A best-effort determination of ioctl argument size based on the request number
static inline size_t ioctl_arg_size(unsigned long request) {
    switch (request) {
        // termios
        case TCGETS:
        case TCSETS:
        case TCSETSW:
        case TCSETSF:
            return sizeof(struct termios);
        // termios2
        case TCGETS2:
        case TCSETS2:
        case TCSETSW2:
        case TCSETSF2:
            return sizeof(struct termios2);
        // Modem control
        case TIOCMGET:
        case TIOCMSET:
        case TIOCMBIS:
        case TIOCMBIC:
            return sizeof(int);
        // FIONREAD
        case FIONREAD:
            return sizeof(int);
        default:
            return 0;
    }
}

#endif // SERIALMUX_H
