/*
 * evtest.c — 简单 evdev 按键测试，不 grab，只打印事件
 * Usage: ./evtest /dev/input/eventX
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <libevdev/libevdev.h>

int main(int argc, char *argv[])
{
	if (argc != 2) {
		fprintf(stderr, "Usage: %s /dev/input/eventX\n", argv[0]);
		return 1;
	}

	int fd = open(argv[1], O_RDONLY | O_NONBLOCK);
	if (fd < 0) { perror(argv[1]); return 1; }

	struct libevdev *dev = NULL;
	int rc = libevdev_new_from_fd(fd, &dev);
	if (rc < 0) {
		fprintf(stderr, "libevdev_new_from_fd: %s\n", strerror(-rc));
		return 1;
	}

	printf("Device: %s\n", libevdev_get_name(dev));
	printf("Listening (no grab) — press keys, Ctrl+C to stop\n\n");
	fflush(stdout);

	for (;;) {
		struct input_event ev;
		rc = libevdev_next_event(dev,
			LIBEVDEV_READ_FLAG_NORMAL | LIBEVDEV_READ_FLAG_BLOCKING, &ev);

		if (rc == LIBEVDEV_READ_STATUS_SYNC) {
			while (rc == LIBEVDEV_READ_STATUS_SYNC)
				rc = libevdev_next_event(dev, LIBEVDEV_READ_FLAG_SYNC, &ev);
			continue;
		}
		if (rc == -EAGAIN) continue;
		if (rc < 0) { fprintf(stderr, "read error: %s\n", strerror(-rc)); break; }

		if (ev.type != EV_KEY) continue;

		const char *action = ev.value == 1 ? "DOWN" :
		                     ev.value == 0 ? "UP  " : "RPT ";
		printf("%s  code=%-4u  name=%s\n",
		       action, ev.code,
		       libevdev_event_code_get_name(EV_KEY, ev.code));
		fflush(stdout);
	}

	libevdev_free(dev);
	close(fd);
	return 0;
}
