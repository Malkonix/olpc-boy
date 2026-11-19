/*
 * olpc-chordd: button chords for the OLPC XO-1 game pad.
 *
 *   CIRCLE + CHECK held:  UP/DOWN = backlight step up/down (auto-repeat),
 *                         RIGHT = DCON colour, LEFT = DCON monochrome.
 *   SQUARE + CROSS held:  UP/DOWN = volume step up/down (auto-repeat),
 *                         LEFT = mute, RIGHT = unmute.
 *   All four held: nothing.
 *
 * USB gamepads (EV_KEY with BTN_SELECT and a shoulder/trigger) add, with the
 * same steps, limits and repeat:
 *   SELECT + L1 = volume up      SELECT + L2 = volume down
 *   SELECT + R1 = brightness up  SELECT + R2 = brightness down
 * L2/R2 count as pressed when their key (BTN_TL2/BTN_TR2) is down or their
 * analog axis (ABS_Z/ABS_RZ) is above 1/3 of its range (released below 1/5).
 * Pad chords never touch the chord flag file (no D-pad is involved).
 *
 * Observes every /dev/input/event* that offers those keys (never EVIOCGRAB),
 * sleeps in poll() and only wakes for input, for inotify on /dev/input, and
 * for the repeat timer while UP/DOWN is held inside a chord.
 *
 * The chord state is exported as "1\n"/"0\n" in $CHORDD_STATE
 * (default /run/olpcboy/chord).
 *
 * Test mode (no devices, no root): see usage().  Paths are overridable by
 * environment variables, see the table below.
 *
 * Single file, libc only, no SSE: build with -Os -march=geode -mtune=geode.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <linux/input.h>
#include <sound/asound.h>

#define K_HOME   102
#define K_UP     103
#define K_PAGEUP 104	/* circle */
#define K_LEFT   105
#define K_RIGHT  106
#define K_END    107	/* check  */
#define K_DOWN   108
#define K_PAGEDN 109
#define K_ROTATE 153
#define BTN_SEL  0x13a	/* BTN_SELECT */
#define BTN_STA  0x13b	/* BTN_START: deliberately not part of any pad chord */
#define BTN_L1   0x136	/* BTN_TL  */
#define BTN_R1   0x137	/* BTN_TR  */
#define BTN_L2   0x138	/* BTN_TL2 */
#define BTN_R2   0x139	/* BTN_TR2 */

#define REPEAT_FIRST_MS 300	/* delay before the first auto-repeat step */
#define REPEAT_MS       150	/* auto-repeat period */
#define MAXDEV 16

/* held-key bitmask */
enum { B_CIRCLE = 1, B_CHECK = 2, B_UP = 4, B_DOWN = 8, B_LEFT = 16, B_RIGHT = 32,
       B_SQUARE = 64, B_CROSS = 128 };
#define B_BRIGHT (B_CIRCLE | B_CHECK)
#define B_VOL    (B_SQUARE | B_CROSS)
#define B_DPAD (B_UP | B_DOWN | B_LEFT | B_RIGHT)

/* gamepad pressed-button bitmask (per device) */
enum { P_SEL = 1, P_L1 = 2, P_L2 = 4, P_R1 = 8, P_R2 = 16 };
#define P_VOL    (P_L1 | P_L2)
#define P_BRIGHT (P_R1 | P_R2)

static int keybit(int code)
{
	switch (code) {
	case K_PAGEUP: return B_CIRCLE;
	case K_END:    return B_CHECK;
	case K_HOME:   return B_SQUARE;
	case K_PAGEDN: return B_CROSS;
	case K_UP:     return B_UP;
	case K_DOWN:   return B_DOWN;
	case K_LEFT:   return B_LEFT;
	case K_RIGHT:  return B_RIGHT;
	}
	return 0;
}

/* i386 struct input_event, fixed so the 64-bit test build reads the same bytes */
struct ev16 { uint32_t sec, usec; uint16_t type, code; int32_t value; };

/* ---- configuration (environment overrides) ----------------------------- */
static const char *p_input, *p_bl_glob, *p_mono, *p_state, *p_timeline, *p_snd, *p_fake;
static int test_mode;
static const char *log_src;	/* "pad: <name>" while a gamepad chord acts, else NULL */

static const char *envp(const char *n, const char *def)
{
	const char *v = getenv(n);
	return v && *v ? v : def;
}

/* ---- logging ------------------------------------------------------------- */
static void logf_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void logf_(const char *fmt, ...)
{
	char msg[256], line[300];
	double up = 0;
	FILE *f;
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(msg, sizeof msg, fmt, ap);
	va_end(ap);
	if (log_src) {
		size_t l = strlen(msg);
		snprintf(msg + l, sizeof msg - l, " [%s]", log_src);
	}
	fprintf(stderr, "olpc-chordd: %s\n", msg);
	if (test_mode && !getenv("CHORDD_TIMELINE"))
		return;
	if ((f = fopen("/proc/uptime", "r"))) {
		if (fscanf(f, "%lf", &up) != 1) up = 0;
		fclose(f);
	}
	snprintf(line, sizeof line, "%.2f chordd: %s\n", up, msg);
	/* same "<uptime> <text>" format as the shell helper tl() */
	if ((f = fopen(p_timeline, "a"))) { fputs(line, f); fclose(f); }
}

/* ---- sysfs actions --------------------------------------------------------- */
static int read_int(const char *path, long *out)
{
	char b[32];
	int fd = open(path, O_RDONLY | O_CLOEXEC), n;
	if (fd < 0) return -1;
	n = read(fd, b, sizeof b - 1);
	close(fd);
	if (n <= 0) return -1;
	b[n] = 0;
	*out = strtol(b, NULL, 10);
	return 0;
}

static int write_str(const char *path, const char *s)
{
	int fd = open(path, O_WRONLY | O_CLOEXEC), n;
	if (fd < 0) return -1;
	n = write(fd, s, strlen(s));
	close(fd);
	return n < 0 ? -1 : 0;
}

/* First backlight directory, resolved on every use so a late driver is found. */
static int find_backlight(char *dir, size_t sz)
{
	glob_t g;
	size_t i;
	int ok = -1;

	if (glob(p_bl_glob, GLOB_NOSORT, NULL, &g) == 0) {
		for (i = 0; i < g.gl_pathc; i++) {
			char t[300];
			snprintf(t, sizeof t, "%s/max_brightness", g.gl_pathv[i]);
			if (access(t, R_OK) == 0) {
				snprintf(dir, sz, "%s", g.gl_pathv[i]);
				ok = 0;
				break;
			}
		}
	}
	globfree(&g);
	return ok;
}

static void do_brightness(int dir, int edge)
{
	char bl[256], p[300], s[16];
	long max, cur, step, nw, back;

	if (find_backlight(bl, sizeof bl) < 0) {
		if (edge) logf_("brightness %s: no backlight device", dir > 0 ? "up" : "down");
		return;
	}
	snprintf(p, sizeof p, "%s/max_brightness", bl);
	if (read_int(p, &max) < 0) return;
	snprintf(p, sizeof p, "%s/brightness", bl);
	if (read_int(p, &cur) < 0) return;
	step = max / 8;			/* same step as olpc-dcon */
	if (step < 1) step = 1;
	nw = cur + dir * step;
	if (nw > max) nw = max;
	if (nw < 0) nw = 0;
	if (nw == cur) {
		if (edge) logf_("brightness %ld (%s limit, max %ld)", cur, dir > 0 ? "upper" : "lower", max);
		return;
	}
	snprintf(s, sizeof s, "%ld\n", nw);
	if (write_str(p, s) < 0) { logf_("brightness: write %s: %s", p, strerror(errno)); return; }
	back = -1;
	read_int(p, &back);
	logf_("brightness %ld -> %ld (max %ld, step %ld), read back %ld", cur, nw, max, step, back);
}

static void do_mono(int mono)
{
	long back = -1;

	if (write_str(p_mono, mono ? "1\n" : "0\n") < 0) {
		logf_("%s: %s: %s", mono ? "mono" : "color", p_mono, strerror(errno));
		return;
	}
	read_int(p_mono, &back);
	logf_("%s mode, read back %ld", mono ? "mono" : "color", back);
}

/* ---- mixer backend ------------------------------------------------------------
 * Real: raw ALSA control ioctls on /dev/snd/controlC0 (no alsa-lib), one open
 * per call.  Fake (CHORDD_FAKE_MIXER=file, for tests): a text file with lines
 *   <element name>=<min> <max> <count> <v0> [v1]
 * that is rewritten on every put; a missing file means "no card".
 * Both return 0, -ENODEV (no card) or -ENOENT (no such element), else -errno.
 */
struct mx_elem { int count; long min, max, v[2]; };
struct mixer_ops {
	int (*get)(const char *name, struct mx_elem *e);
	int (*put)(const char *name, const struct mx_elem *e);
};

static int snd_open(void)
{
	int fd = open(p_snd, O_RDWR | O_CLOEXEC);
	return fd < 0 ? -ENODEV : fd;
}

static void snd_id(struct snd_ctl_elem_id *id, const char *name)
{
	memset(id, 0, sizeof *id);
	id->iface = SNDRV_CTL_ELEM_IFACE_MIXER;
	snprintf((char *)id->name, sizeof id->name, "%s", name);
}

static int real_get(const char *name, struct mx_elem *e)
{
	struct snd_ctl_elem_info info;
	struct snd_ctl_elem_value val;
	int fd = snd_open(), i, r = 0;

	if (fd < 0) return fd;
	memset(&info, 0, sizeof info);
	snd_id(&info.id, name);
	if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_INFO, &info) < 0) { r = -errno; goto out; }
	if (info.type != SNDRV_CTL_ELEM_TYPE_INTEGER && info.type != SNDRV_CTL_ELEM_TYPE_BOOLEAN) {
		r = -EINVAL; goto out;
	}
	memset(&val, 0, sizeof val);
	val.id = info.id;
	if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_READ, &val) < 0) { r = -errno; goto out; }
	e->count = info.count > 2 ? 2 : (int)info.count;
	e->min = info.type == SNDRV_CTL_ELEM_TYPE_BOOLEAN ? 0 : info.value.integer.min;
	e->max = info.type == SNDRV_CTL_ELEM_TYPE_BOOLEAN ? 1 : info.value.integer.max;
	for (i = 0; i < 2; i++) e->v[i] = i < e->count ? val.value.integer.value[i] : 0;
out:
	close(fd);
	return r;
}

static int real_put(const char *name, const struct mx_elem *e)
{
	struct snd_ctl_elem_info info;
	struct snd_ctl_elem_value val;
	int fd = snd_open(), i, r = 0;

	if (fd < 0) return fd;
	memset(&info, 0, sizeof info);
	snd_id(&info.id, name);
	if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_INFO, &info) < 0) { r = -errno; goto out; }
	memset(&val, 0, sizeof val);
	val.id = info.id;
	/* all channels get the same value (balanced) */
	for (i = 0; i < (int)info.count && i < 128; i++) val.value.integer.value[i] = e->v[0];
	if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_WRITE, &val) < 0) r = -errno;
out:
	close(fd);
	return r;
}

static int fake_get(const char *name, struct mx_elem *e)
{
	FILE *f = fopen(p_fake, "r");
	char line[160], key[96];
	int r = -ENOENT;

	if (!f) return -ENODEV;
	snprintf(key, sizeof key, "%s=", name);
	while (fgets(line, sizeof line, f)) {
		if (strncmp(line, key, strlen(key))) continue;
		e->v[1] = 0;
		if (sscanf(line + strlen(key), "%ld %ld %d %ld %ld", &e->min, &e->max, &e->count,
			   &e->v[0], &e->v[1]) >= 4) r = 0;
		break;
	}
	fclose(f);
	return r;
}

static int fake_put(const char *name, const struct mx_elem *e)
{
	FILE *f = fopen(p_fake, "r"), *o;
	char lines[32][160], key[96];
	int n = 0, i, hit = -1;

	if (!f) return -ENODEV;
	snprintf(key, sizeof key, "%s=", name);
	while (n < 32 && fgets(lines[n], sizeof lines[n], f)) {
		if (!strncmp(lines[n], key, strlen(key))) hit = n;
		n++;
	}
	fclose(f);
	if (hit < 0) return -ENOENT;
	snprintf(lines[hit], sizeof lines[hit], "%s%ld %ld %d %ld %ld\n", key, e->min, e->max,
		 e->count, e->v[0], e->count > 1 ? e->v[0] : 0L);
	if (!(o = fopen(p_fake, "w"))) return -errno;
	for (i = 0; i < n; i++) fputs(lines[i], o);
	fclose(o);
	return 0;
}

static const struct mixer_ops real_ops = { real_get, real_put };
static const struct mixer_ops fake_ops = { fake_get, fake_put };
static const struct mixer_ops *mx = &real_ops;

#define DEF_VOL_PCT 88		/* level applied at start if the codec comes up muted/zero */
#define DEF_PCM_PCT 75		/* PCM is not user-adjusted: keep it near 0 dB */

static int nocard_logged;	/* "no sound card" is logged once until a card shows up */
static int mx_inited;

static long pct_of(const struct mx_elem *e, int pct)
{
	return e->min + ((e->max - e->min) * pct + 50) / 100;
}

/* Unmute + default level on one "<base> Playback {Switch,Volume}" pair if it is off/zero. */
static void init_pair(const char *base, int pct)
{
	char n[64];
	struct mx_elem e;

	snprintf(n, sizeof n, "%s Playback Switch", base);
	if (mx->get(n, &e) == 0 && !e.v[0] && !e.v[1]) {
		e.v[0] = 1;
		if (mx->put(n, &e) == 0) logf_("init: %s unmuted", base);
	}
	snprintf(n, sizeof n, "%s Playback Volume", base);
	if (mx->get(n, &e) == 0 && e.v[0] == e.min && e.v[1] == e.min) {
		long nw = pct_of(&e, pct);
		e.v[0] = nw;
		if (mx->put(n, &e) == 0) logf_("init: %s volume %ld (max %ld)", base, nw, e.max);
	}
}

/* Returns 0 when the card is usable (running the one-time init), else -ENODEV. */
static int mixer_ready(void)
{
	struct mx_elem e;
	int r = mx->get("Master Playback Volume", &e);

	if (r == -ENODEV) {
		if (!nocard_logged) {
			logf_("volume: no sound card (%s)", mx == &fake_ops ? p_fake : p_snd);
			nocard_logged = 1;
		}
		return -ENODEV;
	}
	nocard_logged = 0;
	if (!mx_inited) {
		mx_inited = 1;
		/* AD1888 boots with every switch off and every level 0. Master Mono
		 * feeds the internal speaker path on AC97 laptops, so it is set too. */
		init_pair("Master", DEF_VOL_PCT);
		init_pair("PCM", DEF_PCM_PCT);
		init_pair("Master Mono", DEF_VOL_PCT);
	}
	return 0;
}

/* Volume element base name: "Master", else "PCM"; NULL if neither exists. */
static const char *vol_base(struct mx_elem *e)
{
	if (mx->get("Master Playback Volume", e) == 0) return "Master";
	if (mx->get("PCM Playback Volume", e) == 0) return "PCM";
	return NULL;
}

/* Master Mono follows Master (same raw value, same max) so the speaker tracks it. */
static void mirror_mono(const char *base, const char *what, long v)
{
	char n[64];
	struct mx_elem e;

	if (strcmp(base, "Master")) return;
	snprintf(n, sizeof n, "Master Mono Playback %s", what);
	if (mx->get(n, &e) < 0) return;
	e.v[0] = v > e.max ? e.max : v;
	mx->put(n, &e);
}

/* Returns 0 on success/limit, -1 when the action cannot continue (stop repeat). */
static int do_volume(int dir, int edge)
{
	struct mx_elem e;
	char n[64];
	const char *base;
	long step, cur, nw, back = -1;
	int r;

	if (mixer_ready() < 0) return -1;
	if (!(base = vol_base(&e))) { if (edge) logf_("volume: no Master/PCM playback volume"); return -1; }
	step = (e.max - e.min) / 16;
	if (step < 1) step = 1;
	cur = e.v[0];
	nw = cur + dir * step;
	if (nw > e.max) nw = e.max;
	if (nw < e.min) nw = e.min;
	if (nw == cur) {
		if (edge) logf_("volume %s %ld (%s limit, max %ld)", base, cur, dir > 0 ? "upper" : "lower", e.max);
		return 0;
	}
	snprintf(n, sizeof n, "%s Playback Volume", base);
	e.v[0] = nw;
	if ((r = mx->put(n, &e)) < 0) { logf_("volume: write %s: %s", n, strerror(-r)); return -1; }
	mirror_mono(base, "Volume", nw);
	if (mx->get(n, &e) == 0) back = e.v[0];
	logf_("volume %s %ld -> %ld (max %ld), read back %ld", base, cur, nw, e.max, back);
	return 0;
}

static void do_mute(int mute)
{
	struct mx_elem e;
	char n[64];
	const char *base;
	long back = -1;
	int r;

	if (mixer_ready() < 0) return;
	if (!(base = vol_base(&e))) { logf_("volume: no Master/PCM playback volume"); return; }
	snprintf(n, sizeof n, "%s Playback Switch", base);
	if ((r = mx->get(n, &e)) < 0) { logf_("volume: %s: %s", n, strerror(-r)); return; }
	e.v[0] = mute ? 0 : 1;
	if ((r = mx->put(n, &e)) < 0) { logf_("volume: write %s: %s", n, strerror(-r)); return; }
	mirror_mono(base, "Switch", e.v[0]);
	if (mx->get(n, &e) == 0) back = e.v[0];
	logf_("volume %s %s, read back switch %ld", base, mute ? "muted" : "unmuted", back);
}

/* ---- chord state machine ---------------------------------------------------- */
static int held;		/* bitmask of currently held keys (all devices) */
static int tail;		/* chord broke while a D-pad key is still down */
static int exported = -1;	/* last value written to the state file */
static int state_fd = -1;
static int rep_dir;		/* +1 / -1 while UP / DOWN auto-repeats, else 0 */
static int rep_kind;		/* chord (B_BRIGHT / B_VOL) the repeat belongs to */
static long rep_due;		/* ms timestamp of the next auto-repeat step */
static unsigned rep_pad;	/* id of the gamepad owning the repeat, 0 = built-in buttons */
static int rep_bit;		/* pad button (P_*) that drives a pad repeat */
static char rep_tag[96];	/* log source of a pad repeat */

/* Active chord: B_BRIGHT or B_VOL; 0 for none, and 0 when all four keys are held. */
static int chord_kind(void)
{
	int b = (held & B_BRIGHT) == B_BRIGHT, v = (held & B_VOL) == B_VOL;
	return b && v ? 0 : b ? B_BRIGHT : v ? B_VOL : 0;
}

/* Flag source: either chord held, all-four included. */
static int chord_on(void)
{
	return (held & B_BRIGHT) == B_BRIGHT || (held & B_VOL) == B_VOL;
}

/*
 * Exported flag: 1 while the chord is held, and also while a D-pad key that was
 * in use is still down after the chord broke (so a consumer does not see the
 * tail of a brightness adjustment as D-pad input).
 */
static void export_state(void)
{
	int v;

	if (chord_on()) tail = 1;
	else if (!(held & B_DPAD)) tail = 0;
	v = chord_on() || tail;
	if (v == exported) return;
	exported = v;
	if (state_fd >= 0 && pwrite(state_fd, v ? "1\n" : "0\n", 2, 0) != 2)
		fprintf(stderr, "olpc-chordd: state write: %s\n", strerror(errno));
	if (test_mode) fprintf(stderr, "olpc-chordd: state file = %d\n", v);
}

/* Called with the global held mask before/after one key change. */
static void on_change(int before, long now)
{
	int pressed = held & ~before, released = before & ~held;
	int kind = chord_kind();

	if (kind == B_BRIGHT) {
		/* Directions act only when pressed while the chord is already held. */
		if (pressed & B_UP)    { do_brightness(+1, 1); rep_dir = +1; rep_pad = 0; rep_kind = kind; rep_due = now + REPEAT_FIRST_MS; }
		if (pressed & B_DOWN)  { do_brightness(-1, 1); rep_dir = -1; rep_pad = 0; rep_kind = kind; rep_due = now + REPEAT_FIRST_MS; }
		if (pressed & B_RIGHT) do_mono(0);
		if (pressed & B_LEFT)  do_mono(1);
	} else if (kind == B_VOL) {
		if ((pressed & B_UP) && do_volume(+1, 1) == 0)   { rep_dir = +1; rep_pad = 0; rep_kind = kind; rep_due = now + REPEAT_FIRST_MS; }
		if ((pressed & B_DOWN) && do_volume(-1, 1) == 0) { rep_dir = -1; rep_pad = 0; rep_kind = kind; rep_due = now + REPEAT_FIRST_MS; }
		if (pressed & B_RIGHT) do_mute(0);
		if (pressed & B_LEFT)  do_mute(1);
	}
	/* a repeat dies with its chord (also on a direct switch to the other chord) */
	if (rep_dir && !rep_pad && (kind != rep_kind ||
	    (rep_dir > 0 ? (released & B_UP) : (released & B_DOWN))))
		rep_dir = 0;
	export_state();
}

static void tick(long now)
{
	if (rep_dir && now >= rep_due) {
		log_src = rep_pad ? rep_tag : NULL;
		if (rep_kind == B_VOL) { if (do_volume(rep_dir, 0) < 0) { rep_dir = 0; log_src = NULL; return; } }
		else do_brightness(rep_dir, 0);
		log_src = NULL;
		rep_due += REPEAT_MS;
		if (rep_due <= now) rep_due = now + REPEAT_MS;	/* we were late: no burst */
	}
}

/* ---- devices -------------------------------------------------------------------- */
static struct dev {
	int fd; int held; char path[64];
	/* gamepad part (pad != 0): held buttons by key / by analog axis */
	int pad; unsigned id; int pkey, paxis; int abs_ok;	/* abs_ok: bit0 ABS_Z, bit1 ABS_RZ usable */
	long a_on[2], a_off[2];	/* press / release thresholds for ABS_Z, ABS_RZ */
	char tag[96];
} devs[MAXDEV];
static int ndev;
static unsigned next_id = 1;

static long mono_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static int all_held(void)
{
	int i, h = 0;
	for (i = 0; i < ndev; i++) h |= devs[i].held;
	return h;
}

#define LONGBITS (8 * sizeof(long))
static int testbit(const unsigned long *a, int bit) { return (a[bit / LONGBITS] >> (bit % LONGBITS)) & 1; }

static void dev_key(int i, int code, int value, long now)
{
	int b = keybit(code), before = held;

	if (!b || value > 1) return;	/* value 2 (autorepeat) carries no edge */
	/* value 1 on an already-held key (hardware re-send) changes nothing */
	if (value) devs[i].held |= b; else devs[i].held &= ~b;
	held = all_held();
	on_change(before, now);
}

/* ---- gamepad chords ---------------------------------------------------------- */
static int padbit(int code)
{
	switch (code) {
	case BTN_SEL: return P_SEL;
	case BTN_L1:  return P_L1;
	case BTN_L2:  return P_L2;
	case BTN_R1:  return P_R1;
	case BTN_R2:  return P_R2;
	}
	return 0;	/* includes BTN_START: it neither acts nor blocks */
}

/* Press above 1/3 of the range, release below 1/5 (integer maths; min may be non-zero). */
static void pad_set_axis(struct dev *d, int ax, long min, long max)
{
	long range = max - min;

	if (range < 5) { d->abs_ok &= ~(1 << ax); return; }
	d->a_on[ax] = min + range / 3;
	d->a_off[ax] = min + range / 5;
	d->abs_ok |= 1 << ax;
}

static void pad_start_rep(struct dev *d, int dir, int kind, int bit, long now)
{
	rep_dir = dir; rep_kind = kind; rep_pad = d->id; rep_bit = bit;
	snprintf(rep_tag, sizeof rep_tag, "%s", d->tag);
	rep_due = now + REPEAT_FIRST_MS;
}

static void pad_update(struct dev *d, int before, long now)
{
	int cur = d->pkey | d->paxis, pressed = cur & ~before, bit;
	int both = (cur & P_VOL) && (cur & P_BRIGHT);

	/* the repeat dies with its button, with SELECT, or when both groups are held */
	if (rep_dir && rep_pad == d->id && (!(cur & rep_bit) || !(cur & P_SEL) || both))
		rep_dir = 0;
	if (!(cur & P_SEL) || both) return;
	for (bit = P_L1; bit <= P_R2; bit <<= 1) {
		int dir = (bit & (P_L1 | P_R1)) ? +1 : -1, kind = (bit & P_VOL) ? B_VOL : B_BRIGHT;

		if (!(pressed & bit)) continue;
		log_src = d->tag;
		if (kind == B_VOL) {
			if (do_volume(dir, 1) == 0) pad_start_rep(d, dir, kind, bit, now);
		} else {
			do_brightness(dir, 1);
			pad_start_rep(d, dir, kind, bit, now);
		}
		log_src = NULL;
	}
}

static void pad_event(struct dev *d, int type, int code, int value, long now)
{
	int before = d->pkey | d->paxis, b;

	if (type == EV_KEY) {
		if (!(b = padbit(code)) || value > 1) return;
		if (value) d->pkey |= b; else d->pkey &= ~b;
	} else if (type == EV_ABS) {
		int ax = code == ABS_Z ? 0 : code == ABS_RZ ? 1 : -1;

		if (ax < 0 || !(d->abs_ok & (1 << ax))) return;
		b = ax ? P_R2 : P_L2;
		if (d->paxis & b) { if (value < d->a_off[ax]) d->paxis &= ~b; }
		else if (value > d->a_on[ax]) d->paxis |= b;
	} else
		return;
	if ((d->pkey | d->paxis) != before) pad_update(d, before, now);
}

static void dev_close(int i, long now)
{
	int before = held;

	if (devs[i].pad && rep_pad == devs[i].id) rep_dir = 0;
	close(devs[i].fd);
	devs[i] = devs[--ndev];
	held = all_held();
	on_change(before, now);
}

static void scan(long now)
{
	char path[64], name[256];
	unsigned long keys[(KEY_MAX + LONGBITS) / LONGBITS];
	int i, n, fd, before = held;

	for (n = 0; n < 64; n++) {
		snprintf(path, sizeof path, "%s/event%d", p_input, n);
		if (access(path, F_OK) < 0) continue;
		for (i = 0; i < ndev; i++) if (!strcmp(devs[i].path, path)) break;
		if (i < ndev || ndev >= MAXDEV) continue;
		fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (fd < 0) continue;
		memset(keys, 0, sizeof keys);
		if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keys), keys) < 0) { close(fd); continue; }
		memset(&devs[ndev], 0, sizeof devs[ndev]);
		if (!(testbit(keys, K_PAGEUP) && testbit(keys, K_END) &&
		      testbit(keys, K_UP) && testbit(keys, K_DOWN) &&
		      testbit(keys, K_LEFT) && testbit(keys, K_RIGHT))) {
			/* not the built-in buttons: a gamepad needs SELECT and a shoulder/trigger */
			unsigned long abits[(ABS_MAX + LONGBITS) / LONGBITS];
			struct input_absinfo ai;
			struct dev *d = &devs[ndev];
			int ax, tr;

			memset(abits, 0, sizeof abits);
			ioctl(fd, EVIOCGBIT(EV_ABS, sizeof abits), abits);
			for (ax = 0; ax < 2; ax++) {
				if (!testbit(abits, ax ? ABS_RZ : ABS_Z)) continue;
				if (ioctl(fd, EVIOCGABS(ax ? ABS_RZ : ABS_Z), &ai) == 0)
					pad_set_axis(d, ax, ai.minimum, ai.maximum);
			}
			tr = testbit(keys, BTN_L1) || testbit(keys, BTN_R1) ||
			     testbit(keys, BTN_L2) || testbit(keys, BTN_R2) || d->abs_ok;
			if (!testbit(keys, BTN_SEL) || !tr) { close(fd); continue; }
			d->pad = 1;
			d->id = next_id++;
			/* start from the real state: anything already held is not a new press */
			memset(keys, 0, sizeof keys);
			if (ioctl(fd, EVIOCGKEY(sizeof keys), keys) >= 0) {
				static const int codes[] = { BTN_SEL, BTN_L1, BTN_L2, BTN_R1, BTN_R2 };
				for (i = 0; i < 5; i++)
					if (testbit(keys, codes[i])) d->pkey |= padbit(codes[i]);
			}
			for (ax = 0; ax < 2; ax++)
				if ((d->abs_ok & (1 << ax)) &&
				    ioctl(fd, EVIOCGABS(ax ? ABS_RZ : ABS_Z), &ai) == 0 &&
				    ai.value > d->a_on[ax])
					d->paxis |= ax ? P_R2 : P_L2;
			strcpy(name, "?");
			ioctl(fd, EVIOCGNAME(sizeof name), name);
			snprintf(d->tag, sizeof d->tag, "pad: %.80s", name);
			strcpy(d->path, path);
			d->fd = fd;
			logf_("watching %s (%s) as gamepad%s%s", path, name,
			      d->abs_ok & 1 ? ", analog L2" : "", d->abs_ok & 2 ? ", analog R2" : "");
			ndev++;
			continue;
		}
		strcpy(devs[ndev].path, path);
		devs[ndev].fd = fd;
		/* start from the real key state (a key may already be down) */
		memset(keys, 0, sizeof keys);
		if (ioctl(fd, EVIOCGKEY(sizeof keys), keys) >= 0) {
			static const int codes[] = { K_PAGEUP, K_END, K_UP, K_DOWN, K_LEFT, K_RIGHT, K_HOME, K_PAGEDN };
			for (i = 0; i < 8; i++)
				if (testbit(keys, codes[i])) devs[ndev].held |= keybit(codes[i]);
		}
		strcpy(name, "?");
		ioctl(fd, EVIOCGNAME(sizeof name), name);
		logf_("watching %s (%s)", path, name);
		ndev++;
	}
	held = all_held();
	on_change(before, now);
}


static void dev_event(int i, int type, int code, int value, long now)
{
	if (devs[i].pad) pad_event(&devs[i], type, code, value, now);
	else if (type == EV_KEY) dev_key(i, code, value, now);
}

/* ---- test / replay ------------------------------------------------------------------ */
static int name_to_code(const char *s)
{
	static const struct { const char *n; int c; } t[] = {
		{"UP", K_UP}, {"DOWN", K_DOWN}, {"LEFT", K_LEFT}, {"RIGHT", K_RIGHT},
		{"CIRCLE", K_PAGEUP}, {"CHECK", K_END}, {"CROSS", K_PAGEDN},
		{"SQUARE", K_HOME}, {"ROTATE", K_ROTATE},
		{"SELECT", BTN_SEL}, {"START", BTN_STA}, {"L1", BTN_L1}, {"R1", BTN_R1},
		{"L2", BTN_L2}, {"R2", BTN_R2},
	};
	size_t i;
	for (i = 0; i < sizeof t / sizeof *t; i++)
		if (!strcmp(s, t[i].n)) return t[i].c;
	return -1;
}

/*
 * Replay with a virtual clock taken from the event stream.
 * text:   lines "<ms> <NAME> <0|1|2>" (# comments), ms absolute and ascending
 * binary: 16-byte i386 input_event records, clock = sec*1000 + usec/1000
 * After the last event the clock runs on for CHORDD_TAIL_MS (default 0).
 */
static int replay(FILE *in, int text)
{
	long now = 0, end = 0;
	char line[128], nm[32];
	long amin = 0, amax = 255;
	int ax;

	/* devs[0] = built-in buttons, devs[1] = pseudo gamepad (range: "PAD <min> <max>") */
	ndev = 2;
	devs[0].fd = devs[1].fd = -1;
	devs[1].pad = 1;
	devs[1].id = next_id++;
	snprintf(devs[1].tag, sizeof devs[1].tag, "pad: Test Pad");
	for (ax = 0; ax < 2; ax++) pad_set_axis(&devs[1], ax, amin, amax);

	for (;;) {
		long t;
		int code, type = EV_KEY, val, unplug = 0;

		if (text) {
			int v;
			if (!fgets(line, sizeof line, in)) break;
			if (line[0] == '#' || line[0] == '\n') continue;
			if (sscanf(line, "PAD %ld %ld", &amin, &amax) == 2) {
				for (ax = 0; ax < 2; ax++) pad_set_axis(&devs[1], ax, amin, amax);
				continue;
			}
			if (sscanf(line, "%ld %31s %d", &t, nm, &v) != 3) {
				fprintf(stderr, "bad line: %s", line);
				return 2;
			}
			if (!strcmp(nm, "ABS_Z") || !strcmp(nm, "ABS_RZ")) {
				type = EV_ABS; code = nm[5] ? ABS_RZ : ABS_Z;
			} else if (!strcmp(nm, "UNPLUG")) {
				unplug = 1; code = 0;
			} else if ((code = name_to_code(nm)) < 0) {
				fprintf(stderr, "bad line: %s", line);
				return 2;
			}
			val = v;
		} else {
			struct ev16 e;
			if (fread(&e, sizeof e, 1, in) != 1) break;
			if (e.type != EV_KEY && e.type != EV_ABS) continue;
			t = (long)e.sec * 1000 + e.usec / 1000;
			type = e.type; code = e.code; val = e.value;
		}
		while (rep_dir && rep_due <= t) { now = rep_due; tick(now); }
		now = t;
		if (unplug) {
			fprintf(stderr, "[%6ld ms] pad unplugged\n", t);
			if (ndev > 1) { logf_("pad gone"); dev_close(1, now); }
			continue;
		}
		fprintf(stderr, "[%6ld ms] %s %d value %d\n", t, type == EV_ABS ? "abs" : "key", code, val);
		if (type == EV_ABS || padbit(code) || code == BTN_STA) {
			if (ndev > 1) dev_event(1, type, code, val, now);
		} else
			dev_event(0, type, code, val, now);
	}
	end = now + atol(envp("CHORDD_TAIL_MS", "0"));
	while (rep_dir && rep_due <= end) { now = rep_due; tick(now); }
	return 0;
}

/* ---- main loop ------------------------------------------------------------------------ */
static void usage(void)
{
	fputs("usage: olpc-chordd                run the daemon\n"
	      "       olpc-chordd --test        replay text events from stdin\n"
	      "                                 (\"<ms> <UP|DOWN|LEFT|RIGHT|CIRCLE|CHECK|...> <0|1|2>\";\n"
	      "                                 pad: SELECT START L1 R1 L2 R2, \"<ms> ABS_Z|ABS_RZ <value>\",\n"
	      "                                 \"PAD <min> <max>\" sets the trigger range, \"<ms> UNPLUG 1\")\n"
	      "       olpc-chordd --replay      replay raw 16-byte input_event records from stdin\n"
	      "environment: CHORDD_INPUT_DIR (/dev/input), CHORDD_BACKLIGHT_GLOB (/sys/class/backlight/*),\n"
	      "  CHORDD_MONO (/sys/devices/platform/dcon/monochrome), CHORDD_STATE (/run/olpcboy/chord),\n"
	      "  CHORDD_TIMELINE (/var/log/olpc-current/timeline.log; unset in test mode = not written),\n"
	      "  CHORDD_TAIL_MS (test: keep the clock running after the last event),\n"
	      "  CHORDD_SND_CTL (/dev/snd/controlC0), CHORDD_FAKE_MIXER (test: file-backed fake mixer)\n", stderr);
}

static int ino_fd = -1, ino_dev_wd = -1, ino_in_wd = -1;

static void ino_arm(void)
{
	if (ino_fd < 0) return;
	if (ino_in_wd < 0)
		ino_in_wd = inotify_add_watch(ino_fd, p_input, IN_CREATE | IN_DELETE | IN_ATTRIB);
	/* /dev/input not there yet: watch /dev until it shows up */
	if (ino_in_wd < 0 && ino_dev_wd < 0)
		ino_dev_wd = inotify_add_watch(ino_fd, "/dev", IN_CREATE);
	if (ino_in_wd >= 0 && ino_dev_wd >= 0) {
		inotify_rm_watch(ino_fd, ino_dev_wd);
		ino_dev_wd = -1;
	}
}

static volatile sig_atomic_t quit;
static void on_sig(int s) { (void)s; quit = 1; }

int main(int argc, char **argv)
{
	struct pollfd pfd[MAXDEV + 1];
	int i;

	p_input    = envp("CHORDD_INPUT_DIR", "/dev/input");
	p_bl_glob  = envp("CHORDD_BACKLIGHT_GLOB", "/sys/class/backlight/*");
	p_mono     = envp("CHORDD_MONO", "/sys/devices/platform/dcon/monochrome");
	p_state    = envp("CHORDD_STATE", "/run/olpcboy/chord");
	p_snd      = envp("CHORDD_SND_CTL", "/dev/snd/controlC0");
	p_fake     = getenv("CHORDD_FAKE_MIXER");
	p_timeline = envp("CHORDD_TIMELINE", "/var/log/olpc-current/timeline.log");

	if (argc > 2 || (argc == 2 && strcmp(argv[1], "--test") && strcmp(argv[1], "--replay"))) {
		usage();
		return 2;
	}
	if (argc == 2) test_mode = 1;
	if (p_fake && *p_fake) mx = &fake_ops;
	else if (test_mode && !getenv("CHORDD_SND_CTL"))
		p_snd = "/nonexistent/controlC0";	/* tests must never touch the host card */

	{
		char dir[200], *s;
		snprintf(dir, sizeof dir, "%s", p_state);
		if ((s = strrchr(dir, '/')) && s != dir) { *s = 0; mkdir(dir, 0755); }
	}
	state_fd = open(p_state, O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
	if (state_fd < 0) fprintf(stderr, "olpc-chordd: %s: %s (state not exported)\n", p_state, strerror(errno));
	export_state();

	mixer_ready();		/* card may be absent (QEMU): logged once, retried on use */
	if (test_mode)
		return replay(stdin, !strcmp(argv[1], "--test"));

	signal(SIGTERM, on_sig);
	signal(SIGINT, on_sig);
	signal(SIGPIPE, SIG_IGN);

	ino_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	ino_arm();
	scan(mono_ms());
	logf_("started (%d device(s))", ndev);

	while (!quit) {
		long now = mono_ms(), to = -1;
		int n, nd;

		if (rep_dir) {
			to = rep_due - now;
			if (to < 0) to = 0;
		}
		nd = ndev;
		for (i = 0; i < ndev; i++) { pfd[i].fd = devs[i].fd; pfd[i].events = POLLIN; }
		pfd[nd].fd = ino_fd;		/* -1 is ignored by poll() */
		pfd[nd].events = POLLIN;
		n = poll(pfd, nd + 1, (int)to);
		now = mono_ms();
		if (n < 0) { if (errno == EINTR) continue; break; }

		for (i = nd - 1; i >= 0; i--) {
			struct ev16 e[32];
			int r, k;

			if (!(pfd[i].revents & (POLLIN | POLLERR | POLLHUP | POLLNVAL))) continue;
			r = read(devs[i].fd, e, sizeof e);
			if (r < 0 && errno != EAGAIN && errno != EINTR) {
				logf_("%s gone", devs[i].path);
				dev_close(i, now);
				continue;
			}
			for (k = 0; k < r / (int)sizeof e[0]; k++)
				if (e[k].type == EV_KEY || e[k].type == EV_ABS)
					dev_event(i, e[k].type, e[k].code, e[k].value, now);
		}
		if (ino_fd >= 0 && (pfd[nd].revents & POLLIN)) {
			char buf[512];
			while (read(ino_fd, buf, sizeof buf) > 0) ;
			ino_arm();
			scan(now);
		}
		tick(mono_ms());
	}
	logf_("exiting");
	return 0;
}
