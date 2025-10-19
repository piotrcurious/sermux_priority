Usage:

run demon :
./serialmux_daemon /dev/ttyUSB0

low priority client:

export SERIALMUX_DEVICE="/dev/ttyUSB0"
LD_PRELOAD=./serialmux_preload.so minicom -D /dev/ttyUSB0


high priority client:

export SERIALMUX_DEVICE="/dev/ttyUSB0"
export SERIALMUX_PRIORITY="HIGH"
LD_PRELOAD=./serialmux_preload.so avrdude -c arduino -p m328p -P /dev/ttyUSB0 ...

