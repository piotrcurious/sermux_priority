# serialmux - README

Usage

Run the daemon:

```sh
./serialmux_daemon /dev/ttyUSB0
```

Low-priority client

```sh
export SERIALMUX_DEVICE="/dev/ttyUSB0"
LD_PRELOAD=./serialmux_preload.so minicom -D /dev/ttyUSB0
```

High-priority client

```sh
export SERIALMUX_DEVICE="/dev/ttyUSB0"
export SERIALMUX_PRIORITY="HIGH"
LD_PRELOAD=./serialmux_preload.so avrdude -c arduino -p m328p -P /dev/ttyUSB0 ...
```

Notes

- Set SERIALMUX_DEVICE to the device used by the daemon.
- Set SERIALMUX_PRIORITY to "HIGH" for high-priority clients (default is low priority if unset).
- LD_PRELOAD should point to the provided serialmux_preload.so so clients talk to the daemon instead of the device directly.


