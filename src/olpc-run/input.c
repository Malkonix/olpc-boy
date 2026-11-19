#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include "input.h"
#include "diag.h"

#define J_B 0
#define J_SELECT 2
#define J_START 3
#define J_UP 4
#define J_DOWN 5
#define J_LEFT 6
#define J_RIGHT 7
#define J_A 8
#define BIT(n) ((uint16_t)(1u << (n)))
#define KEY_ROTATE 153     /* KEY_DIRECTION */

/* ---------------------------------------------------------------------------
 * Button tables.  The ONLY place that decides which physical button is A or B.
 * ------------------------------------------------------------------------- */
struct keymap { int code; int bit; };

/* XO-1 built-in game buttons ("AT Translated Set 2 keyboard") */
static const struct keymap map_builtin[] = {
	{ KEY_UP, J_UP }, { KEY_DOWN, J_DOWN }, { KEY_LEFT, J_LEFT }, { KEY_RIGHT, J_RIGHT },
	{ KEY_PAGEDOWN, J_B },        /* cross, bottom */
	{ KEY_END,      J_A },        /* check, top */
	{ KEY_PAGEUP,   J_START },    /* circle, right of the diamond */
	{ KEY_HOME,     J_SELECT },   /* square, left */
};

/* USB pads, Nintendo-style layout: the east button is A, the south button is B. */
static const struct keymap map_pad[] = {
	{ BTN_SOUTH, J_B }, { BTN_EAST, J_A },
	{ BTN_SELECT, J_SELECT }, { BTN_START, J_START },
	{ BTN_DPAD_UP, J_UP }, { BTN_DPAD_DOWN, J_DOWN }, { BTN_DPAD_LEFT, J_LEFT }, { BTN_DPAD_RIGHT, J_RIGHT },
	/* generic clones (joystick range): button 1 = TRIGGER, 2 = THUMB, 9/10 = BASE3/BASE4 */
	{ BTN_TRIGGER, J_B }, { BTN_THUMB, J_A },
	{ BTN_BASE3, J_SELECT }, { BTN_BASE4, J_START },
};

static int lookup(const struct keymap *m, int n, int code)
{
	int i;

	for (i = 0; i < n; i++) if (m[i].code == code) return m[i].bit;
	return -1;
}
#define LOOKUP(m, c) lookup(m, (int)(sizeof(m) / sizeof((m)[0])), c)

/* ------------------------------------------------------------------------- */

static uint16_t stick_dirs(int v, int mn, int mx, int bneg, int bpos)
{
	int centre = (mn + mx) / 2, dz = (mx - mn) / 4;   /* pressed beyond half deflection */

	if (v < centre - dz) return BIT(bneg);
	if (v > centre + dz) return BIT(bpos);
	return 0;
}

void inp_dev_event(struct inp_dev *d, const struct input_event *e)
{
	if (e->type == EV_KEY) {
		int bit;
		if (d->cls == IN_BUILTIN) {
			if (e->code == KEY_ROTATE) { d->rotate = e->value != 0; return; }
			bit = LOOKUP(map_builtin, e->code);
		} else {
			bit = LOOKUP(map_pad, e->code);
		}
		if (bit < 0) return;
		if (e->value) d->btn |= BIT(bit); else d->btn &= (uint16_t)~BIT(bit);
	} else if (e->type == EV_ABS && d->cls == IN_PAD) {
		if (e->code == ABS_HAT0X)
			d->hat = (uint16_t)((d->hat & ~(BIT(J_LEFT) | BIT(J_RIGHT))) |
					    (e->value < 0 ? BIT(J_LEFT) : e->value > 0 ? BIT(J_RIGHT) : 0));
		else if (e->code == ABS_HAT0Y)
			d->hat = (uint16_t)((d->hat & ~(BIT(J_UP) | BIT(J_DOWN))) |
					    (e->value < 0 ? BIT(J_UP) : e->value > 0 ? BIT(J_DOWN) : 0));
		else if (e->code == ABS_X)
			d->stick = (uint16_t)((d->stick & ~(BIT(J_LEFT) | BIT(J_RIGHT))) |
					      stick_dirs(e->value, d->absmin[0], d->absmax[0], J_LEFT, J_RIGHT));
		else if (e->code == ABS_Y)
			d->stick = (uint16_t)((d->stick & ~(BIT(J_UP) | BIT(J_DOWN))) |
					      stick_dirs(e->value, d->absmin[1], d->absmax[1], J_UP, J_DOWN));
	}
}

int inp_dev_drain(struct inp_dev *d)
{
	struct input_event ev[32];

	for (;;) {
		ssize_t n = read(d->fd, ev, sizeof ev);
		int i;

		if (n < 0) return errno == EAGAIN || errno == EINTR ? 0 : -1;
		if (n == 0) return -1;
		for (i = 0; i < (int)(n / (ssize_t)sizeof ev[0]); i++) inp_dev_event(d, &ev[i]);
	}
}

uint16_t inp_dev_mask(const struct inp_dev *d) { return d->btn | d->hat | d->stick; }

#define LONG_BITS (8 * sizeof(unsigned long))
static int test_bit(const unsigned long *a, int n) { return (a[n / LONG_BITS] >> (n % LONG_BITS)) & 1; }

static int is_open(const struct input *in, const char *path)
{
	int i;

	for (i = 0; i < in->ndev; i++) if (!strcmp(in->dev[i].path, path)) return 1;
	return 0;
}

static void try_open(struct input *in, const char *path)
{
	unsigned long keys[(KEY_MAX + LONG_BITS) / LONG_BITS];
	char name[128] = "";
	struct inp_dev *d;
	int fd, cls;

	if (in->ndev >= IN_MAXDEV || is_open(in, path)) return;
	fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0) return;
	memset(keys, 0, sizeof keys);
	ioctl(fd, EVIOCGNAME(sizeof name - 1), name);
	ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keys), keys);
	if (strstr(name, "AT Translated Set 2")) cls = IN_BUILTIN;
	else if (test_bit(keys, BTN_GAMEPAD) || test_bit(keys, BTN_JOYSTICK)) cls = IN_PAD;
	else { close(fd); return; }
	d = &in->dev[in->ndev++];
	memset(d, 0, sizeof *d);
	d->fd = fd; d->cls = cls;
	snprintf(d->path, sizeof d->path, "%s", path);
	d->absmin[0] = d->absmin[1] = -32768;
	d->absmax[0] = d->absmax[1] = 32767;
	if (cls == IN_PAD) {
		struct input_absinfo ai;
		if (ioctl(fd, EVIOCGABS(ABS_X), &ai) == 0) { d->absmin[0] = ai.minimum; d->absmax[0] = ai.maximum; }
		if (ioctl(fd, EVIOCGABS(ABS_Y), &ai) == 0) { d->absmin[1] = ai.minimum; d->absmax[1] = ai.maximum; }
	}
	dlog("olpc-run: input: %s \"%s\" as %s\n", path, name, cls == IN_PAD ? "pad" : "built-in buttons");
}

void input_scan(struct input *in)
{
	DIR *d = opendir(in->dir);
	struct dirent *e;
	char p[96];

	if (!d) return;
	while ((e = readdir(d)))
		if (!strncmp(e->d_name, "event", 5)) {
			snprintf(p, sizeof p, "%s/%s", in->dir, e->d_name);
			try_open(in, p);
		}
	closedir(d);
}

void input_init(struct input *in, const char *dir, const char *chord_file)
{
	memset(in, 0, sizeof *in);
	in->dir = dir; in->chord_file = chord_file;
	in->chord_fd = -1;
	in->rescan = -1;
	in->ino = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (in->ino >= 0 && inotify_add_watch(in->ino, dir, IN_CREATE) < 0) { close(in->ino); in->ino = -1; }
	input_scan(in);
	if (!in->ndev) fprintf(stderr, "olpc-run: input: no usable device in %s\n", dir);
}

/* One pread() on a descriptor that stays open (the file is on tmpfs and olpc-chordd rewrites
 * it in place); while the file is missing, retry the open only every 60 polls. */
static void read_chord(struct input *in)
{
	char c = '0';

	if (in->chord_fd < 0 && in->chord_file && in->chord_retry-- <= 0) {
		in->chord_fd = open(in->chord_file, O_RDONLY | O_CLOEXEC);
		in->chord_retry = 60;
	}
	if (in->chord_fd >= 0 && pread(in->chord_fd, &c, 1, 0) != 1) {
		close(in->chord_fd);
		in->chord_fd = -1;
		c = '0';
	}
	in->chord = c == '1';
}

void input_poll(struct input *in, uint64_t now_ns)
{
	int i, rot = 0, ss = 0;

	if (in->ino >= 0) {
		char b[512];
		if (read(in->ino, b, sizeof b) > 0) in->rescan = 30;   /* let mdev settle the node */
	}
	if (in->rescan > 0 && --in->rescan == 0) { input_scan(in); in->rescan = -1; }
	for (i = 0; i < in->ndev; ) {
		if (inp_dev_drain(&in->dev[i]) < 0) {
			dlog("olpc-run: input: %s gone\n", in->dev[i].path);
			close(in->dev[i].fd);
			in->dev[i] = in->dev[--in->ndev];
			continue;
		}
		i++;
	}
	if (in->chord_tick-- <= 0) { read_chord(in); in->chord_tick = 2; }
	for (i = 0; i < in->ndev; i++) {
		const struct inp_dev *d = &in->dev[i];
		if (d->rotate) rot = 1;
		if (d->cls == IN_PAD && (d->btn & BIT(J_SELECT)) && (d->btn & BIT(J_START))) ss = 1;
	}
	if (!rot) in->rot_since = 0; else if (!in->rot_since) in->rot_since = now_ns | 1;
	if (!ss) in->ss_since = 0; else if (!in->ss_since) in->ss_since = now_ns | 1;
}

uint16_t input_mask(const struct input *in)
{
	uint16_t m = 0;
	int i;

	for (i = 0; i < in->ndev; i++) {
		const struct inp_dev *d = &in->dev[i];
		uint16_t dm = inp_dev_mask(d);
		/* while a chord is active the built-in D-pad belongs to olpc-chordd */
		if (in->chord && d->cls == IN_BUILTIN)
			dm &= (uint16_t)~(BIT(J_UP) | BIT(J_DOWN) | BIT(J_LEFT) | BIT(J_RIGHT));
		m |= dm;
	}
	return m;
}

int input_exit(const struct input *in, uint64_t now_ns, uint64_t hold_ns)
{
	return (in->rot_since && now_ns - in->rot_since >= hold_ns) ||
	       (in->ss_since && now_ns - in->ss_since >= hold_ns);
}
