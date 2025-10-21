CC = gcc
CFLAGS = -Wall -Wextra -pthread -fPIC
LDFLAGS = -lpthread
LDLIBS = -ldl

all: serialmux_daemon serialmux_preload.so

serialmux_daemon: serialmux_daemon.c serialmux.h
	$(CC) $(CFLAGS) -o serialmux_daemon serialmux_daemon.c $(LDFLAGS)

serialmux_preload.so: serialmux_preload.c serialmux.h
	$(CC) $(CFLAGS) -shared -o serialmux_preload.so serialmux_preload.c $(LDLIBS)

clean:
	rm -f serialmux_daemon serialmux_preload.so
