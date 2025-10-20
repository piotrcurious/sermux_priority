# Serial Mux

This project consists of two components: a daemon (`serialmux_daemon`) and a preload library (`serialmux_preload.so`). Together, they provide a mechanism for multiple applications to share a single serial port.

## Purpose

The primary goal of this project is to allow multiple applications to access a single serial port in a controlled manner. A common use case is for a primary application to have high-priority access to a serial device, while allowing other, lower-priority applications (like monitoring or debugging tools) to access the same device when the primary application is not actively using it.

## How it Works

### `serialmux_daemon`

The daemon is the central component of the system. It opens the real serial port and creates a Unix domain socket (`/tmp/serialmux.sock`) to listen for client connections. When a client connects, the daemon creates a pseudo-terminal (PTY) for it. The daemon then relays data between the client's PTY and the real serial port.

The daemon implements a priority system. A client can connect with either high or low priority. If a high-priority client is connected, all low-priority clients are "paused," meaning their I/O is temporarily suspended. When the last high-priority client disconnects, the low-priority clients are resumed.

The daemon also forwards certain `ioctl` calls and termios settings from the clients to the real serial port. This allows clients to configure the serial port as if they were accessing it directly.

### `serialmux_preload.so`

This is a shared library that is loaded into an application's address space using the `LD_PRELOAD` mechanism. It intercepts standard C library functions related to file I/O, such as `open`, `close`, `ioctl`, `tcflush`, etc.

When an application tries to open the serial device that is being managed by the daemon, the preload library intercepts the `open` call. Instead of opening the real device, it connects to the `serialmux_daemon` via the Unix domain socket and requests a new PTY. The daemon responds with the path to the slave side of the PTY, which the preload library then opens and returns the file descriptor to the application.

From the application's perspective, it has a valid file descriptor for the serial port. However, all of its subsequent operations on that file descriptor are actually being performed on the PTY. The preload library continues to intercept relevant function calls (like `ioctl`) and forwards them to the daemon to be applied to the real serial port.

## Techniques and Methods

*   **Daemonization:** The `serialmux_daemon` is a long-running background process.
*   **Unix Domain Sockets:** Used for inter-process communication between the preload library and the daemon.
*   **Pseudo-terminals (PTYs):** A PTY is created for each client, providing them with a file descriptor that behaves like a terminal.
*   **`LD_PRELOAD`:** The `serialmux_preload.so` library uses this mechanism to intercept and wrap standard C library functions.
*   **`dlsym`:** Used by the preload library to find the addresses of the real C library functions.
*   **pthreads:** The daemon uses multiple threads to handle I/O for each client concurrently.
*   **`select`:** Used for multiplexing I/O from the real serial port, the client PTYs, and the main listening socket.
*   **`termios` and `ioctl`:** The daemon and preload library work together to forward terminal control operations to the real serial port.
