# sermux_priority
app allowing two applications connect to one serial device, one having higher priority 
0.1vibe by Claude 
# Serial Port Multiplexer - Usage Guide

## Overview

This tool allows two applications to share a single serial port with priority-based arbitration. When a high-priority application (like avrdude) connects, low-priority applications (like minicom) are automatically paused, and their serial port settings are saved and restored when the high-priority app disconnects.

## Components

1. **serialmux_daemon** - Central daemon managing serial port access
2. **libserialmux_preload.so** - LD_PRELOAD library that intercepts serial port calls
3. **Makefile** - Build script

## Building

```bash
bash build.sh
# or manually:
make
```

## Installation

```bash
make install
# Copies daemon to /usr/local/bin/
# Copies library to /usr/local/lib/
```

## Setup

### 1. Start the Daemon

```bash
# Start daemon for /dev/ttyUSB0
serialmux_daemon /dev/ttyUSB0

# Output should show:
# [12:34:56] Serial Mux Daemon started
# [12:34:56] Device: /dev/ttyUSB0
# [12:34:56] Socket: /tmp/serialmux.sock
```

### 2. Run Low-Priority Application

```bash
# Terminal 1: Start minicom (low priority)
LD_PRELOAD=/usr/local/lib/libserialmux_preload.so minicom -D /dev/ttyUSB0

# Or with explicit priority:
export SERIALMUX_PRIORITY=LOW
LD_PRELOAD=/usr/local/lib/libserialmux_preload.so minicom -D /dev/ttyUSB0
```

The daemon will log:
```
[12:34:58] New client: PID 1234, priority 0
[12:34:58] Handler started for PID 1234 (priority 0)
```

### 3. Run High-Priority Application

In another terminal, when you want to upload with avrdude:

```bash
# Terminal 2: Start avrdude (high priority)
export SERIALMUX_PRIORITY=HIGH
LD_PRELOAD=/usr/local/lib/libserialmux_preload.so avrdude -p m328p -c arduino -P /dev/ttyUSB0 -U flash:w:sketch.hex
```

The daemon will log:
```
[12:34:59] New client: PID 5678, priority 1
[12:34:59] PAUSE: Low priority PID 1234
[12:34:59] Handler started for PID 5678 (priority 1)
```

The minicom window will freeze while avrdude uses the port.

### 4. After High-Priority App Closes

```
[12:35:05] Handler ended for PID 5678
[12:35:05] RESUME: Low priority PID 1234
```

Minicom automatically resumes with original settings restored.

## Usage Examples

### Arduino IDE + Serial Monitor

**Terminal 1 (Daemon):**
```bash
serialmux_daemon /dev/ttyUSB0
```

**Terminal 2 (Serial Monitor - Low Priority):**
```bash
export SERIALMUX_PRIORITY=LOW
LD_PRELOAD=/usr/local/lib/libserialmux_preload.so minicom -D /dev/ttyUSB0
```

**Terminal 3 (Arduino IDE Upload - High Priority):**
Arduino IDE will use avrdude internally. If you run it from command line:
```bash
export SERIALMUX_PRIORITY=HIGH
LD_PRELOAD=/usr/local/lib/libserialmux_preload.so arduino-cli upload -p /dev/ttyUSB0
```

### Debugging with GDB + Serial Console

**Terminal 1 (Daemon):**
```bash
serialmux_daemon /dev/ttyUSB0
```

**Terminal 2 (Console - Low Priority):**
```bash
LD_PRELOAD=/usr/local/lib/libserialmux_preload.so picocom -b 115200 /dev/ttyUSB0
```

**Terminal 3 (Debugger - High Priority):**
```bash
export SERIALMUX_PRIORITY=HIGH
LD_PRELOAD=/usr/local/lib/libserialmux_preload.so gdb ./firmware.elf
```

## Environment Variables

- `SERIALMUX_PRIORITY` - Set to `HIGH` for high priority, `LOW` or unset for low priority
- `LD_PRELOAD` - Must include path to `libserialmux_preload.so`

## Troubleshooting

### "Failed to connect to serialmux daemon"

The daemon isn't running. Start it first:
```bash
serialmux_daemon /dev/ttyUSB0
```

### Daemon won't start: "Permission denied"

Your user needs read/write access to the serial port:
```bash
sudo usermod -a -G dialout $USER
# Then log out and log back in
```

Or run daemon with sudo:
```bash
sudo serialmux_daemon /dev/ttyUSB0
```

### Baud rate keeps changing

The multiplexer saves/restores baud rates when switching between apps. If you see unexpected rate changes:

1. Check the daemon logs for ioctl operations
2. Ensure each app closes properly (sends close command)
3. May need to explicitly set baud rate in your serial monitor

### High-priority app hangs

The daemon uses non-blocking I/O. If an app doesn't handle this well:

1. Try adding a small delay before opening the port
2. Some apps might need patches to handle non-blocking mode

## Architecture Notes

- The daemon accepts connections from clients via Unix domain socket (`/tmp/serialmux.sock`)
- Each client gets its own handler thread
- When high-priority client connects, all low-priority clients are paused
- Data relay uses `select()` for multiplexing
- Serial port settings are saved/restored using `tcgetattr()/tcsetattr()`
- LD_PRELOAD intercepts `open()`, `close()`, `read()`, `write()`, and `ioctl()` calls

## Limitations

- Currently supports only 2 priority levels (HIGH/LOW)
- Maximum 10 concurrent clients (configurable in source)
- Does not work with applications that directly open `/dev/ttyUSB*` by different names
- Some ioctl calls may not be fully intercepted if the app uses custom control sequences
- Works best with applications that use standard termios settings

## Stopping the Daemon

```bash
# Find the daemon process
ps aux | grep serialmux_daemon

# Kill it gracefully
kill <PID>

# Or with Ctrl+C if running in foreground
^C
```

The socket file will be cleaned up automatically.
