/*
 * relay.c — KVM keyboard relay daemon
 *
 * Usage: sudo ./relay <evdev-path> <ttyACM0> <ttyACM1>
 *
 * Reads keyboard events from an evdev device (exclusive grab), converts
 * to USB HID boot keyboard reports, and forwards to the active dongle via
 * CDC-ACM serial using the km_proto frame format.
 *
 * Switching: double-tap ScrollLock within 400 ms switches the active dongle.
 * A TYPE_RELEASE frame is sent to the old dongle before switching to prevent
 * stuck keys on the previously active PC.
 *
 * Both dongles stay connected to their PCs over BLE at all times. Only the
 * forwarding target changes — no reconnection overhead on switch.
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
#include <linux/input.h>
#include <libevdev/libevdev.h>

#include "km_proto.h"

/* ── evdev → USB HID keycode table ──────────────────────────────────────
 * Source: Linux kernel drivers/hid/hid-input.c hid_keyboard[] (reversed).
 * Modifiers (0xE0–0xE7) are handled separately as bitmask in hid_report[0].
 * Index = Linux KEY_* code, value = USB HID Usage ID (page 0x07).
 */
static const uint8_t evdev_to_hid[256] = {
	[KEY_RESERVED]    = 0x00,
	[KEY_ESC]         = 0x29,
	[KEY_1]           = 0x1E, [KEY_2]           = 0x1F,
	[KEY_3]           = 0x20, [KEY_4]           = 0x21,
	[KEY_5]           = 0x22, [KEY_6]           = 0x23,
	[KEY_7]           = 0x24, [KEY_8]           = 0x25,
	[KEY_9]           = 0x26, [KEY_0]           = 0x27,
	[KEY_MINUS]       = 0x2D, [KEY_EQUAL]       = 0x2E,
	[KEY_BACKSPACE]   = 0x2A,
	[KEY_TAB]         = 0x2B,
	[KEY_Q]           = 0x14, [KEY_W]           = 0x1A,
	[KEY_E]           = 0x08, [KEY_R]           = 0x15,
	[KEY_T]           = 0x17, [KEY_Y]           = 0x1C,
	[KEY_U]           = 0x18, [KEY_I]           = 0x0C,
	[KEY_O]           = 0x12, [KEY_P]           = 0x13,
	[KEY_LEFTBRACE]   = 0x2F, [KEY_RIGHTBRACE]  = 0x30,
	[KEY_ENTER]       = 0x28,
	[KEY_LEFTCTRL]    = 0xE0,
	[KEY_A]           = 0x04, [KEY_S]           = 0x16,
	[KEY_D]           = 0x07, [KEY_F]           = 0x09,
	[KEY_G]           = 0x0A, [KEY_H]           = 0x0B,
	[KEY_J]           = 0x0D, [KEY_K]           = 0x0E,
	[KEY_L]           = 0x0F,
	[KEY_SEMICOLON]   = 0x33, [KEY_APOSTROPHE]  = 0x34,
	[KEY_GRAVE]       = 0x35,
	[KEY_LEFTSHIFT]   = 0xE1,
	[KEY_BACKSLASH]   = 0x31,
	[KEY_Z]           = 0x1D, [KEY_X]           = 0x1B,
	[KEY_C]           = 0x06, [KEY_V]           = 0x19,
	[KEY_B]           = 0x05, [KEY_N]           = 0x11,
	[KEY_M]           = 0x10,
	[KEY_COMMA]       = 0x36, [KEY_DOT]         = 0x37,
	[KEY_SLASH]       = 0x38,
	[KEY_RIGHTSHIFT]  = 0xE5,
	[KEY_KPASTERISK]  = 0x55,
	[KEY_LEFTALT]     = 0xE2,
	[KEY_SPACE]       = 0x2C,
	[KEY_CAPSLOCK]    = 0x39,
	[KEY_F1]          = 0x3A, [KEY_F2]          = 0x3B,
	[KEY_F3]          = 0x3C, [KEY_F4]          = 0x3D,
	[KEY_F5]          = 0x3E, [KEY_F6]          = 0x3F,
	[KEY_F7]          = 0x40, [KEY_F8]          = 0x41,
	[KEY_F9]          = 0x42, [KEY_F10]         = 0x43,
	[KEY_NUMLOCK]     = 0x53, [KEY_SCROLLLOCK]  = 0x47,
	[KEY_KP7]         = 0x5F, [KEY_KP8]         = 0x60,
	[KEY_KP9]         = 0x61, [KEY_KPMINUS]     = 0x56,
	[KEY_KP4]         = 0x5C, [KEY_KP5]         = 0x5D,
	[KEY_KP6]         = 0x5E, [KEY_KPPLUS]      = 0x57,
	[KEY_KP1]         = 0x59, [KEY_KP2]         = 0x5A,
	[KEY_KP3]         = 0x5B, [KEY_KP0]         = 0x62,
	[KEY_KPDOT]       = 0x63,
	[KEY_F11]         = 0x44, [KEY_F12]         = 0x45,
	[KEY_KPENTER]     = 0x58,
	[KEY_RIGHTCTRL]   = 0xE4,
	[KEY_KPSLASH]     = 0x54,
	[KEY_SYSRQ]       = 0x46,
	[KEY_RIGHTALT]    = 0xE6,
	[KEY_HOME]        = 0x4A, [KEY_UP]          = 0x52,
	[KEY_PAGEUP]      = 0x4B, [KEY_LEFT]        = 0x50,
	[KEY_RIGHT]       = 0x4F, [KEY_END]         = 0x4D,
	[KEY_DOWN]        = 0x51, [KEY_PAGEDOWN]    = 0x4E,
	[KEY_INSERT]      = 0x49, [KEY_DELETE]      = 0x4C,
	[KEY_LEFTMETA]    = 0xE3, [KEY_RIGHTMETA]   = 0xE7,
	[KEY_COMPOSE]     = 0x65,
	[KEY_PAUSE]       = 0x48,
	[KEY_F13]         = 0x68, [KEY_F14]         = 0x69,
	[KEY_F15]         = 0x6A, [KEY_F16]         = 0x6B,
	[KEY_F17]         = 0x6C, [KEY_F18]         = 0x6D,
	[KEY_F19]         = 0x6E, [KEY_F20]         = 0x6F,
	[KEY_F21]         = 0x70, [KEY_F22]         = 0x71,
	[KEY_F23]         = 0x72, [KEY_F24]         = 0x73,
};

static bool is_modifier(uint8_t hid_code)
{
	return hid_code >= 0xE0 && hid_code <= 0xE7;
}

/* ── HID report state ────────────────────────────────────────────────── */
static uint8_t hid_report[KM_PAYLOAD_KB_LEN];  /* [0]=mods [1]=0 [2..7]=keycodes */

static void report_key_down(uint8_t hid)
{
	if (is_modifier(hid)) {
		hid_report[0] |= (uint8_t)(1U << (hid - 0xE0));
	} else {
		for (int i = 2; i < 8; i++) {
			if (hid_report[i] == 0) {
				hid_report[i] = hid;
				break;
			}
		}
	}
}

static void report_key_up(uint8_t hid)
{
	if (is_modifier(hid)) {
		hid_report[0] &= (uint8_t)~(1U << (hid - 0xE0));
	} else {
		for (int i = 2; i < 8; i++) {
			if (hid_report[i] == hid) {
				hid_report[i] = 0;
				break;
			}
		}
	}
}

/* ── Dongle serial ports ─────────────────────────────────────────────── */
#define NUM_DONGLES 2

struct dongle {
	const char *tty_path;
	int         fd;
};

static struct dongle dongles[NUM_DONGLES];
static int active = 0;

static int open_serial(const char *path)
{
	int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);

	if (fd < 0) {
		perror(path);
		return -1;
	}

	struct termios tty;

	memset(&tty, 0, sizeof(tty));
	cfsetispeed(&tty, B115200);
	cfsetospeed(&tty, B115200);
	tty.c_cflag = CS8 | CREAD | CLOCAL;
	tty.c_lflag = 0;
	tty.c_iflag = 0;
	tty.c_oflag = 0;
	tty.c_cc[VMIN]  = 0;
	tty.c_cc[VTIME] = 0;

	if (tcsetattr(fd, TCSANOW, &tty) < 0) {
		perror("tcsetattr");
		close(fd);
		return -1;
	}

	return fd;
}

static void send_frame_to(int dongle_idx, uint8_t type, const uint8_t *payload)
{
	int fd = dongles[dongle_idx].fd;

	if (fd < 0) {
		return;
	}

	uint8_t frame[KM_FRAME_KB_LEN];

	km_build_frame(frame, type, payload);

	fprintf(stderr, "TX dongle%d type=0x%02x mods=0x%02x keys=%02x%02x%02x%02x%02x%02x\n",
		dongle_idx, type,
		payload[0], payload[2], payload[3], payload[4], payload[5], payload[6], payload[7]);

	ssize_t n = write(fd, frame, KM_FRAME_KB_LEN);

	if (n < 0 && errno != EAGAIN) {
		fprintf(stderr, "write %s: %s\n",
			dongles[dongle_idx].tty_path, strerror(errno));
	}
}

static void send_release_to(int dongle_idx)
{
	static const uint8_t zero[KM_PAYLOAD_KB_LEN] = {0};

	send_frame_to(dongle_idx, KM_TYPE_RELEASE, zero);
}

/* ── Hotkey FSM (ScrollLock double-tap) ──────────────────────────────── */
#define HOTKEY_CODE          KEY_SCROLLLOCK
#define HOTKEY_DOUBLE_TAP_MS 400

typedef enum {
	HK_IDLE,
	HK_FIRST_DOWN,
	HK_FIRST_UP,
} hotkey_state_t;

static hotkey_state_t hk_state = HK_IDLE;
static struct timespec hk_ts;

static long elapsed_ms(const struct timespec *start)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (now.tv_sec  - start->tv_sec)  * 1000L
	     + (now.tv_nsec - start->tv_nsec) / 1000000L;
}

/*
 * Returns true if a switch was triggered.
 * Caller must NOT forward the ScrollLock key.
 */
static bool hotkey_update(uint16_t code, int32_t value)
{
	if (code != HOTKEY_CODE) {
		hk_state = HK_IDLE;
		return false;
	}

	long ms = elapsed_ms(&hk_ts);

	switch (hk_state) {
	case HK_IDLE:
		if (value == 1) {
			clock_gettime(CLOCK_MONOTONIC, &hk_ts);
			hk_state = HK_FIRST_DOWN;
		}
		break;

	case HK_FIRST_DOWN:
		if (value == 0) {
			hk_state = HK_FIRST_UP;
		} else if (ms > HOTKEY_DOUBLE_TAP_MS) {
			hk_state = HK_IDLE;
		}
		break;

	case HK_FIRST_UP:
		if (value == 1 && ms < HOTKEY_DOUBLE_TAP_MS) {
			hk_state = HK_IDLE;

			int old = active;

			send_release_to(old);
			memset(hid_report, 0, sizeof(hid_report));

			active ^= 1;
			printf("Switched to dongle %d (%s)\n",
			       active, dongles[active].tty_path);
			fflush(stdout);
			return true;
		}
		if (ms > HOTKEY_DOUBLE_TAP_MS) {
			hk_state = HK_IDLE;
		}
		break;
	}

	return false;
}

/* ── Signal handling ─────────────────────────────────────────────────── */
static volatile bool running = true;

static void sig_handler(int sig)
{
	(void)sig;
	running = false;
}

/* ── Main ────────────────────────────────────────────────────────────── */
int main(int argc, char *argv[])
{
	if (argc != 4) {
		fprintf(stderr, "Usage: %s <evdev-path> <ttyACM0> <ttyACM1>\n",
			argv[0]);
		return EXIT_FAILURE;
	}

	const char *evdev_path = argv[1];

	dongles[0].tty_path = argv[2];
	dongles[1].tty_path = argv[3];

	/* Open serial ports */
	for (int i = 0; i < NUM_DONGLES; i++) {
		dongles[i].fd = open_serial(dongles[i].tty_path);
		if (dongles[i].fd < 0) {
			fprintf(stderr, "Warning: cannot open %s — dongle %d offline\n",
				dongles[i].tty_path, i);
		}
	}

	/* Open evdev device */
	int evfd = open(evdev_path, O_RDONLY);

	if (evfd < 0) {
		perror(evdev_path);
		return EXIT_FAILURE;
	}

	struct libevdev *dev = NULL;
	int rc = libevdev_new_from_fd(evfd, &dev);

	if (rc < 0) {
		fprintf(stderr, "libevdev_new_from_fd: %s\n", strerror(-rc));
		return EXIT_FAILURE;
	}

	/* Exclusive grab: prevent events from reaching other processes */
	rc = libevdev_grab(dev, LIBEVDEV_GRAB);
	if (rc < 0) {
		fprintf(stderr, "libevdev_grab: %s\n", strerror(-rc));
		fprintf(stderr, "Hint: run as root or add user to 'input' group\n");
		return EXIT_FAILURE;
	}

	printf("KVM relay started\n");
	printf("  Keyboard : %s (%s)\n", evdev_path, libevdev_get_name(dev));
	printf("  Dongle 0 : %s\n", dongles[0].tty_path);
	printf("  Dongle 1 : %s\n", dongles[1].tty_path);
	printf("  Active   : dongle %d\n", active);
	printf("  Hotkey   : double-tap ScrollLock to switch\n");
	fflush(stdout);

	signal(SIGINT,  sig_handler);
	signal(SIGTERM, sig_handler);

	memset(hid_report, 0, sizeof(hid_report));

	/* Event loop */
	while (running) {
		struct input_event ev;

		rc = libevdev_next_event(dev,
			LIBEVDEV_READ_FLAG_NORMAL | LIBEVDEV_READ_FLAG_BLOCKING,
			&ev);

		if (rc == LIBEVDEV_READ_STATUS_SYNC) {
			/* Drain sync queue */
			while (rc == LIBEVDEV_READ_STATUS_SYNC) {
				rc = libevdev_next_event(
					dev, LIBEVDEV_READ_FLAG_SYNC, &ev);
			}
			/* Re-read in normal mode */
			continue;
		}

		if (rc == -EAGAIN) {
			continue;
		}

		if (rc != LIBEVDEV_READ_STATUS_SUCCESS) {
			if (running) {
				fprintf(stderr, "libevdev_next_event: %s\n",
					strerror(-rc));
			}
			break;
		}

		if (ev.type != EV_KEY) {
			continue;
		}

		uint16_t code  = ev.code;
		int32_t  value = ev.value;

		/* Check hotkey BEFORE consuming the key into the report.
		 * Returns true if switch occurred — discard this event entirely. */
		bool switched = hotkey_update(code, value);

		if (switched) {
			continue;
		}

		/* Never forward ScrollLock to any PC */
		if (code == KEY_SCROLLLOCK) {
			continue;
		}

		/* Suppress key repeats (value == 2); PC generates its own repeats */
		if (value == 2) {
			continue;
		}

		/* Map evdev code to HID usage */
		if (code >= 256) {
			continue;
		}
		uint8_t hid = evdev_to_hid[code];

		if (hid == 0) {
			continue;  /* unmapped key */
		}

		/* Update report state */
		if (value == 1) {
			report_key_down(hid);
		} else {
			report_key_up(hid);
		}

		/* Send to active dongle */
		send_frame_to(active, KM_TYPE_KB, hid_report);
	}

	/* Cleanup: release all keys on active dongle */
	send_release_to(active);

	libevdev_grab(dev, LIBEVDEV_UNGRAB);
	libevdev_free(dev);
	close(evfd);

	for (int i = 0; i < NUM_DONGLES; i++) {
		if (dongles[i].fd >= 0) {
			close(dongles[i].fd);
		}
	}

	printf("\nRelay stopped.\n");
	return EXIT_SUCCESS;
}
