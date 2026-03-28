CC      = gcc
CFLAGS  = -Wall -Wextra -O2 $(shell pkg-config --cflags libevdev 2>/dev/null || echo -I/usr/include/libevdev-1.0)
LDFLAGS = $(shell pkg-config --libs libevdev 2>/dev/null || echo -levdev)

TARGETS = relay relay_hidraw

.PHONY: all clean

all: $(TARGETS)

relay: relay.c km_proto.h
	$(CC) $(CFLAGS) -o $@ relay.c $(LDFLAGS)

relay_hidraw: relay_hidraw.c km_proto.h
	$(CC) $(CFLAGS) -o $@ relay_hidraw.c

clean:
	rm -f $(TARGETS)
