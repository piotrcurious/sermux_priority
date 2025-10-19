#!/bin/bash
gcc -o serialmux_daemon serialmux_daemon.c -pthread
gcc -o serialmux_preload.so serialmux_preload.c -shared -fPIC -ldl -pthread
