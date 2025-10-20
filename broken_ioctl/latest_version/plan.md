# Plan: Fixing `ioctl` Forwarding in Serialmux

This document outlines the plan to fix the `ioctl` forwarding mechanism in the serialmux project. The goal is to create a robust and generic solution that can handle any `ioctl` call correctly.

## 1. Protocol Redesign

The existing protocol is the primary source of the issues. We will replace it with a new, more explicit binary protocol.

### New Protocol Structure

**Request (Client -> Daemon):**

| Field           | Type      | Size (bytes) | Description                                                                                             |
| --------------- | --------- | ------------ | ------------------------------------------------------------------------------------------------------- |
| Magic           | `char[4]` | 4            | Always "SMIO" to identify the protocol.                                                                 |
| Request Type    | `uint32_t`| 4            | The type of request (e.g., `REQ_IOCTL`).                                                                |
| `ioctl` Request | `uint64_t`| 8            | The full `ioctl` request number.                                                                        |
| Argument Type   | `uint32_t`| 4            | Defines how the `ioctl`'s third argument is passed. See "Argument Types" below.                         |
| Argument Length | `uint32_t`| 4            | The length of the `Argument Data` payload in bytes.                                                     |
| Argument Data   | `char[]`  | Variable     | The actual argument data, with a length specified by the `Argument Length` field.                       |

**Argument Types:**

-   `0` (`ARG_NONE`): No third argument was passed to `ioctl`. `Argument Length` will be 0.
-   `1` (`ARG_VALUE`): The argument is an integer value passed directly. `Argument Data` will contain the raw integer. `Argument Length` will be `sizeof(int)`.
-   `2` (`ARG_BUFFER`): The argument is a pointer to a buffer. `Argument Data` contains the contents of that buffer. `Argument Length` is the size of the buffer as determined by `_IOC_SIZE(request)`.

**Response (Daemon -> Client):**

| Field              | Type       | Size (bytes) | Description                                                                        |
| ------------------ | ---------- | ------------ | ---------------------------------------------------------------------------------- |
| Return Code (`rc`) | `int32_t`  | 4            | The return value of the operation on the daemon side (-1 for error).               |
| `errno` Value      | `int32_t`  | 4            | The value of `errno` if an error occurred.                                         |
| Output Data Length | `uint32_t` | 4            | The length of the `Output Data` payload in bytes.                                  |
| Output Data        | `char[]`   | Variable     | Data to be copied back to the client's buffer (for `_IOC_READ` `ioctl`s).          |

## 2. Implementation Steps

### Step 2.1: Modify `serialmux_preload.c`

1.  **Update Protocol Constants**: Define the new `Argument Type` enums.
2.  **Rework `ioctl` Wrapper**:
    -   The core of the logic will be to determine the correct `Argument Type`.
    -   If the third argument (`argp`) is `NULL`, the type is `ARG_NONE`.
    -   If `_IOC_SIZE(request)` is `0`, but `argp` is not `NULL`, the type is `ARG_VALUE`. The value of `argp` will be sent as the data.
    -   If `_IOC_SIZE(request)` is greater than `0` and `argp` is not `NULL`, the type is `ARG_BUFFER`. The data from the buffer pointed to by `argp` will be sent.
3.  **Update `send_request_and_get_response`**:
    -   Modify the function to construct and send the new protocol message.
    -   It will need to take the `Argument Type` as a new parameter.
    -   When receiving the response, it must be able to handle a variable-sized `Output Data` payload and copy it back to the `argp` buffer if required.

### Step 2.2: Modify `serialmux_daemon.c`

1.  **Update Protocol Constants**: Define the new `Argument Type` enums to match the preload library.
2.  **Rework `handle_binary_request`**:
    -   Update the function to parse the new, extended protocol header.
    -   Remove all `ioctl`-specific `case` statements (e.g., `TCGETS`). The logic must be generic.
    -   Dynamically allocate a buffer to hold the incoming `Argument Data` based on the `Argument Length` field. This eliminates the fixed-size buffer limitation.
    -   Based on the `Argument Type`:
        -   `ARG_NONE`: Call `ioctl(real_fd, request, NULL)`.
        -   `ARG_VALUE`: Call `ioctl(real_fd, request, (int)value_from_payload)`.
        -   `ARG_BUFFER`: Call `ioctl(real_fd, request, pointer_to_allocated_buffer)`.
3.  **Construct Generic Response**:
    -   After the `ioctl` call, capture the return code and `errno`.
    -   If the `ioctl` was a `READ` operation, copy the data from the buffer into the `Output Data` field of the response.
    -   Send the complete response back to the client.
4.  **Remove Asynchronous Polling**:
    -   Delete the `pty_ioctl_thread` function entirely.
    -   Remove the code that creates and detaches this thread.
    -   Remove any related logic that checks for PTY setting changes (`memcmp`, etc.). This will simplify the daemon's logic and make `ioctl` handling the single source of truth for terminal settings.

## 3. Testing Strategy

After implementing the changes, the following tests will be performed:

1.  **Compile**: Ensure both `serialmux_preload.c` and `serialmux_daemon.c` compile without errors or warnings.
2.  **Basic Functionality**: Use `minicom` or a similar terminal emulator to verify that standard serial communication (sending and receiving data) still works as expected.
3.  **`ioctl` Forwarding**:
    -   Use `esptool.py` to attempt to flash an ESP32 device. This tool heavily relies on `ioctl` calls to manipulate serial lines (DTR/RTS) and change baud rates. Successful flashing will be a strong indicator of a correct implementation.
    -   Use `avrdude` to interact with an Arduino or similar AVR-based device. This will test a different set of `ioctl` calls.
4.  **Graceful Shutdown**: Verify that the daemon shuts down cleanly and restores the original terminal settings.
