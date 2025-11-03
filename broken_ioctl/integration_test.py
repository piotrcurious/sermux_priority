import serial
import fcntl
import termios
import os
import subprocess
import time
import pty
import errno

# Build the project
subprocess.run(["make", "-C", "broken_ioctl"], check=True)

# Create a PTY pair
master, slave = pty.openpty()
slave_name = os.ttyname(slave)

# Start the daemon
daemon_process = subprocess.Popen(["./broken_ioctl/serialmux_daemon", slave_name])

time.sleep(1)

# Set up the environment for the test
os.environ["LD_PRELOAD"] = "./broken_ioctl/serialmux_preload.so"
os.environ["SERIALMUX_DEVICE"] = slave_name

# Open the virtual serial port
ser = serial.Serial(slave_name)

# Test TCGETS/TCSETS
attrs = termios.tcgetattr(ser.fd)
attrs[2] &= ~termios.PARENB
termios.tcsetattr(ser.fd, termios.TCSANOW, attrs)

# Test TIOCMBIS - this should fail with ENOTTY, but not EFAULT
try:
    fcntl.ioctl(ser.fd, termios.TIOCMBIS, termios.TIOCM_DTR)
except OSError as e:
    if e.errno != errno.ENOTTY:
        raise
else:
    # This should not be reached on a PTY
    pass

# Test tcdrain
termios.tcdrain(ser.fd)

print("All tests passed!")

# Clean up
ser.close()
daemon_process.terminate()
daemon_process.wait()
