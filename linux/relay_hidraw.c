/*
 * relay_hidraw.c — KVM keyboard relay via hidraw (no evdev, no grab needed)
 *
 * Usage: sudo ./relay_hidraw <hidraw-path> <ttyACM0> <ttyACM1>
 *
 * Reads raw HID keyboard reports directly from /dev/hidrawN.
 * hidraw is not grabbed by Wayland, so this works even on a running desktop.
 *
 * The keyboard HID report is 8 bytes (boot protocol):
 *   [0] modifier bitmask
 *   [1] reserved
 *   [2..7] keycodes (up to 6KRO)
 *
 * This is exactly the km_proto payload format, so reports are forwarded
 * directly without translation.
 *
 * Switching: double-tap ScrollLock (HID keycode 0x47) within 400 ms.
 * ScrollLock key itself is never forwarded.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <termios.h>

#include "km_proto.h"

#define HID_REPORT_LEN  8
#define HID_KEY_SCROLLLOCK  0x47

#define NUM_DONGLES 2

/* ── Serial port ─────────────────────────────────────────────────────── */
static int open_serial(const char *path)
{
	int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
	if (fd < 0) {
		fprintf(stderr, "open(%s): %s\n", path, strerror(errno));
		return -1;
	}
	struct termios tty;
	tcgetattr(fd, &tty);
	cfsetispeed(&tty, B115200);
	cfsetospeed(&tty, B115200);
	cfmakeraw(&tty);
	tty.c_cc[VMIN]  = 0;
	tty.c_cc[VTIME] = 0;
	tcsetattr(fd, TCSANOW, &tty);
	return fd;
}

/* ── Dongle state ────────────────────────────────────────────────────── */
static struct {
	const char *tty_path;
	int         fd;
} dongles[NUM_DONGLES];

static int active = 0;

static void send_frame_to(int idx, uint8_t type, const uint8_t *payload)
{
	if (dongles[idx].fd < 0) {
		return;
	}
	uint8_t frame[KM_FRAME_KB_LEN];
	km_build_frame(frame, type, payload);
	write(dongles[idx].fd, frame, sizeof(frame));
}

static void send_release_to(int idx)
{
	static const uint8_t zeros[KM_PAYLOAD_KB_LEN] = {0};
	send_frame_to(idx, KM_TYPE_RELEASE, zeros);
}

/* ── ScrollLock double-tap hotkey ────────────────────────────────────── */
#define HOTKEY_WINDOW_MS  400

static bool     sl_pressed_last;
static bool     sl_first_tap;
static struct timespec sl_first_time;

static long ms_since(const struct timespec *t)
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (now.tv_sec  - t->tv_sec)  * 1000L
	     + (now.tv_nsec - t->tv_nsec) / 1000000L;
}

/* Returns true if a switch occurred */
static bool hotkey_update(bool sl_down)
{
	if (sl_down == sl_pressed_last) {
		return false;
	}
	sl_pressed_last = sl_down;

	if (!sl_down) {
		/* key-up: not a trigger */
		return false;
	}

	/* key-down */
	if (!sl_first_tap) {
		sl_first_tap = true;
		clock_gettime(CLOCK_MONOTONIC, &sl_first_time);
		return false;
	}

	/* second tap — check timing */
	if (ms_since(&sl_first_time) <= HOTKEY_WINDOW_MS) {
		sl_first_tap = false;

		int old = active;
		active ^= 1;
		send_release_to(old);
		printf("Switched to dongle %d (%s)\n",
			active, dongles[active].tty_path);
		fflush(stdout);
		return true;
	}

	/* too slow — start fresh */
	clock_gettime(CLOCK_MONOTONIC, &sl_first_time);
	return false;
}

/* ── Signal handler ──────────────────────────────────────────────────── */
static volatile sig_atomic_t running = 1;
static void sig_handler(int s) { (void)s; running = 0; }

/* ── main ────────────────────────────────────────────────────────────── */
int main(int argc, char *argv[])
{
	if (argc != 4) {
		fprintf(stderr, "Usage: %s <hidraw> <ttyACM0> <ttyACM1>\n", argv[0]);
		return EXIT_FAILURE;
	}

	const char *hidraw_path = argv[1];
	dongles[0].tty_path = argv[2];
	dongles[1].tty_path = argv[3];

	/* Open hidraw */
	int hidfd = open(hidraw_path, O_RDONLY);
	if (hidfd < 0) {
		fprintf(stderr, "open(%s): %s\n", hidraw_path, strerror(errno));
		return EXIT_FAILURE;
	}

	/* Open serial ports */
	for (int i = 0; i < NUM_DONGLES; i++) {
		dongles[i].fd = open_serial(dongles[i].tty_path);
		if (dongles[i].fd < 0) {
			return EXIT_FAILURE;
		}
	}

	printf("KVM relay started (hidraw mode)\n");
	printf("  Keyboard : %s\n", hidraw_path);
	printf("  Dongle 0 : %s\n", dongles[0].tty_path);
	printf("  Dongle 1 : %s\n", dongles[1].tty_path);
	printf("  Active   : dongle %d\n", active);
	printf("  Hotkey   : double-tap ScrollLock to switch\n");
	fflush(stdout);

	signal(SIGINT,  sig_handler);
	signal(SIGTERM, sig_handler);

	uint8_t prev[HID_REPORT_LEN] = {0};

	while (running) {
		uint8_t report[HID_REPORT_LEN + 1];  /* +1 for optional report ID */
		ssize_t n = read(hidfd, report, HID_REPORT_LEN + 1);

		if (n < 0) {
			if (errno == EINTR) {
				continue;
			}
			fprintf(stderr, "hidraw read: %s\n", strerror(errno));
			break;
		}

		if (n != HID_REPORT_LEN) {
			/* Some keyboards send a report ID prefix — skip first byte */
			if (n == HID_REPORT_LEN + 1) {
				uint8_t tmp[HID_REPORT_LEN];
				memcpy(tmp, report + 1, HID_REPORT_LEN);
				memcpy(report, tmp, HID_REPORT_LEN);
			} else {
				continue;
			}
		}

		if (memcmp(report, prev, HID_REPORT_LEN) == 0) {
			continue;
		}
		memcpy(prev, report, HID_REPORT_LEN);

		/* Check ScrollLock in current report (keycode 0x47 in bytes 2-7) */
		bool sl_now = false;
		for (int i = 2; i < HID_REPORT_LEN; i++) {
			if (report[i] == HID_KEY_SCROLLLOCK) {
				sl_now = true;
				break;
			}
		}

		bool switched = hotkey_update(sl_now);
		(void)switched;

		/* Remove ScrollLock from report before forwarding */
		uint8_t fwd[HID_REPORT_LEN];
		memcpy(fwd, report, HID_REPORT_LEN);
		for (int i = 2; i < HID_REPORT_LEN; i++) {
			if (fwd[i] == HID_KEY_SCROLLLOCK) {
				fwd[i] = 0x00;
			}
		}

		/* Check if report is all-zero → send release frame */
		bool all_zero = true;
		for (int i = 0; i < HID_REPORT_LEN; i++) {
			if (fwd[i] != 0) {
				all_zero = false;
				break;
			}
		}

		if (all_zero) {
			send_release_to(active);
		} else {
			send_frame_to(active, KM_TYPE_KB, fwd);
		}
	}

	send_release_to(active);
	close(hidfd);
	for (int i = 0; i < NUM_DONGLES; i++) {
		if (dongles[i].fd >= 0) {
			close(dongles[i].fd);
		}
	}

	printf("\nRelay stopped.\n");
	return EXIT_SUCCESS;
}
