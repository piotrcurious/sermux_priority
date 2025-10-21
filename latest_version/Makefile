CC = gcc
CFLAGS = -Wall -Wextra -pthread -fPIC
LDFLAGS = -lpthread -ldl

all: serialmux_daemon serialmux_preload.so

serialmux_daemon: serialmux_daemon.c
	$(CC) $(CFLAGS) -o $@ $< -lpthread

serialmux_preload.so: serialmux_preload.c
	$(CC) $(CFLAGS) -shared -o $@ $< -ldl

clean:
	rm -f serialmux_daemon serialmux_preload.so
