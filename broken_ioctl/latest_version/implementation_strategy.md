# Implementation Strategy: `ioctl` Forwarding

Based on the research, the following implementation strategy will be used to correctly handle `ioctl` forwarding.

## 1. `serialmux_preload.c`

### `ioctl` Wrapper

The `ioctl` wrapper will be simplified to always treat its third argument as a pointer. This is the safe and correct default for the vast majority of `ioctl` calls. The logic will be as follows:

1.  If the `fd` is not managed by `serialmux`, pass the call through to the real `ioctl`.
2.  If the `fd` is a managed `tty`, forward the `ioctl` to the daemon.
3.  The argument will be treated as a pointer to a buffer of size `_IOC_SIZE(request)`.
4.  If `_IOC_SIZE(request)` is 0, a default size of `sizeof(int)` will be used, as this is the case for `TIOCMBIS`, `TIOCMBIC`, etc.

### `tc*` Wrappers

The `tcflush`, `tcsendbreak`, and `tcdrain` functions will have their own wrappers. These wrappers will send specific request types to the daemon (`REQ_TCFLSH`, `REQ_TCSENDBREAK`, `REQ_TCDRAIN`), and will pass their arguments as values.

## 2. `serialmux_daemon.c`

### `handle_binary_request`

The `handle_binary_request` function will be updated to handle the different request types:

1.  **`REQ_IOCTL`**: The daemon will treat the incoming data as a buffer and will pass a pointer to this buffer to the `ioctl` call. It will not attempt to interpret the data as a value.
2.  **`REQ_TCFLSH`**, **`REQ_TCSENDBREAK`**: The daemon will read an integer value from the socket and pass this value to the corresponding `tcflush` or `tcsendbreak` call.
3.  **`REQ_TCDRAIN`**: The daemon will call `tcdrain` with no arguments.

This separation of concerns will ensure that both pointer-based and value-based `ioctl`s are handled correctly, resolving the `esptool` and `minicom` issues.
