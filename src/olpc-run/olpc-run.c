/*
 * olpc-run: minimal libretro frontend for the OLPC XO-1 (Geode LX, no SSE).
 *
 *   olpc-run [options] <gb|gbc|nes|core.so> <rom | rom.zip>
 *
 * Video: integer-scaled blit into /dev/fb0 (see scale.c).  Audio: raw ALSA ioctl
 * PCM; blocking on the sound card paces the emulator (audio.c).  Input: evdev
 * nodes read directly (input.c).  Statistics for the hardware tests are written
 * to --stats.  See rootfs/README.md for the user-visible behaviour.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <linux/fb.h>
#include <linux/kd.h>
#include <linux/vt.h>
#include "libretro.h"
#include "scale.h"
#include "audio.h"
#include "input.h"
#include "diag.h"
#include "saver.h"

typedef uint64_t u64;

#define FB_W 1200
#define FB_H 900
#define MAX_ROM (16u << 20)

/* ------------------------------------------------------------------ options */

static const char *o_core;            /* --core override (name or path) */
static const char *o_libdir = "/usr/lib/libretro";
static const char *o_savedir, *o_sysdir = "/usr/share/olpcboy/system", *o_stats;
static const char *o_audiodev;
static const char *o_stats_live;      /* --stats-live FILE: periodic copy of the statistics, meant for tmpfs */
static unsigned long o_stall_at[8];   /* --test-stall-at F[,F2...] with --test-stall-ms M */
static int o_nstall, o_stall_ms;
static unsigned o_save_period_s = 60; /* --test-save-period-s */
static int o_save_delay_ms;           /* --test-save-delay-ms: the save helper sleeps before each write */
static const char *o_chord = "/run/olpcboy/chord";
static const char *o_inputdir = "/dev/input";
static unsigned long o_frames;        /* 0 = unlimited */
static int o_noaudio, o_novideo, o_maxskip = 4, o_maxsec;
static unsigned o_target;             /* --audio-target frames; 0 = derive from o_latency_ms */
static double o_period_ms = 4.0, o_latency_ms = 45.0;
static int o_nofill;                  /* --no-fill-query: pace with poll() only, never DELAY/STATUS */
static long o_xrun_debug = -1;        /* --xrun-debug N: written to the PCM's /proc xrun_debug; -1 = leave */
static int g_xrun_debug_ok = -1;      /* -1 not requested, 0 failed, 1 written */
static int o_nort;                    /* --no-rt: stay SCHED_OTHER, no mlockall */
static const char *o_running = "/run/olpcboy/running";
static int o_blit_delay_ms;           /* test hook: busy-wait inside the blit */
static int o_restore;
static int o_maxscale;                  /* 0 = largest that fits */
static int o_poke_save;               /* test hook: flip one save RAM byte at frame 100 */

/* ------------------------------------------------------------------ time, log */

static u64 now_ns(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (u64)t.tv_sec * 1000000000ull + (u64)t.tv_nsec;
}

static void logf_(const char *fmt, ...)
{
	va_list ap;

	fputs("olpc-run: ", stderr);
	va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
	fputc('\n', stderr);
}

/* ------------------------------------------------------------------ console / fb */

static uint8_t *g_map;
static size_t g_maplen;
static int g_fbfd = -1, g_tty = -1, g_fake;
static volatile sig_atomic_t g_kd_graphics;
static int g_kd_saved = KD_TEXT;

static void console_text(void)
{
	if (g_kd_graphics && g_tty >= 0) {
		ioctl(g_tty, KDSETMODE, g_kd_saved);
		g_kd_graphics = 0;
	}
}

/* back to KD_TEXT, then bounce through another VT so fbcon repaints */
static void console_redraw(void)
{
	struct vt_stat st;

	console_text();
	if (g_tty >= 0 && ioctl(g_tty, VT_GETSTATE, &st) == 0 && st.v_active >= 1 && st.v_active < 63) {
		int cur = st.v_active, other = cur == 1 ? 2 : 1;
		if (ioctl(g_tty, VT_ACTIVATE, other) == 0) ioctl(g_tty, VT_WAITACTIVE, other);
		if (ioctl(g_tty, VT_ACTIVATE, cur) == 0) ioctl(g_tty, VT_WAITACTIVE, cur);
	}
}

static void tty_open(void)
{
	g_tty = open("/dev/tty0", O_RDWR | O_NOCTTY | O_CLOEXEC);
	if (g_tty < 0) g_tty = open("/dev/tty1", O_RDWR | O_NOCTTY | O_CLOEXEC);
}

static struct pixtab g_tab;
static size_t g_stride;
static int g_fbw, g_fbh;

/* mode: 1 real /dev/fb0 (also switches the console to graphics), 0 failure */
static int fb_open(int graphics)
{
	const char *fake = getenv("OLPC_RUN_FAKEFB");

	if (fake && *fake) {
		struct stat st;
		g_fbfd = open(fake, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
		if (g_fbfd < 0) { logf_("video: fake fb %s: %s", fake, strerror(errno)); return 0; }
		g_maplen = (size_t)FB_W * FB_H * 2;
		if (fstat(g_fbfd, &st) == 0 && (size_t)st.st_size < g_maplen && ftruncate(g_fbfd, (off_t)g_maplen) < 0) return 0;
		g_fbw = FB_W; g_fbh = FB_H; g_stride = FB_W * 2;
		pixtab_init(&g_tab, 16, 11, 5, 5, 6, 0, 5);
		g_fake = 1;
		logf_("video: FAKE framebuffer %s (%dx%d rgb565)", fake, g_fbw, g_fbh);
	} else {
		struct fb_var_screeninfo v;
		struct fb_fix_screeninfo f;
		g_fbfd = open("/dev/fb0", O_RDWR | O_CLOEXEC);
		if (g_fbfd < 0) { logf_("video: /dev/fb0: %s", strerror(errno)); return 0; }
		if (ioctl(g_fbfd, FBIOGET_VSCREENINFO, &v) < 0 || ioctl(g_fbfd, FBIOGET_FSCREENINFO, &f) < 0) {
			logf_("video: fb ioctl failed: %s", strerror(errno));
			close(g_fbfd); g_fbfd = -1;
			return 0;
		}
		if (v.bits_per_pixel != 16 && v.bits_per_pixel != 32) {
			logf_("video: unsupported fb depth %u bpp", v.bits_per_pixel);
			close(g_fbfd); g_fbfd = -1;
			return 0;
		}
		g_fbw = (int)v.xres; g_fbh = (int)v.yres; g_stride = f.line_length;
		g_maplen = f.smem_len;
		pixtab_init(&g_tab, (int)v.bits_per_pixel, (int)v.red.offset, (int)v.red.length,
			    (int)v.green.offset, (int)v.green.length, (int)v.blue.offset, (int)v.blue.length);
		logf_("video: /dev/fb0 %dx%d %u bpp stride %zu", g_fbw, g_fbh, v.bits_per_pixel, g_stride);
	}
	g_map = mmap(NULL, g_maplen, PROT_READ | PROT_WRITE, MAP_SHARED, g_fbfd, 0);
	if (g_map == MAP_FAILED) {
		logf_("video: mmap failed: %s", strerror(errno));
		g_map = NULL; close(g_fbfd); g_fbfd = -1;
		return 0;
	}
	if (!g_fake && graphics) {
		tty_open();
		if (g_tty >= 0) {
			if (ioctl(g_tty, KDGETMODE, &g_kd_saved) < 0) g_kd_saved = KD_TEXT;
			if (ioctl(g_tty, KDSETMODE, KD_GRAPHICS) == 0) g_kd_graphics = 1;
		}
		logf_("video: console KD_GRAPHICS=%d", (int)g_kd_graphics);
	}
	return 1;
}

static void fb_clear(void)
{
	if (g_map) memset(g_map, 0, g_stride * (size_t)g_fbh);
}

static void fb_close(void)
{
	if (g_map) {
		if (!g_fake) fb_clear();      /* the fake fb keeps its last frame for the tests */
		munmap(g_map, g_maplen);
		g_map = NULL;
	}
	if (!g_fake) console_redraw();
	if (g_fbfd >= 0) close(g_fbfd);
	g_fbfd = -1;
	if (g_tty >= 0) close(g_tty);
	g_tty = -1;
}

static volatile sig_atomic_t g_quit, g_sig, g_timeout;

static void on_term(int sig) { g_quit = 1; g_sig = sig; }
static void on_alarm(int sig) { (void)sig; g_quit = 1; g_timeout = 1; }

/* crash: leave the console usable, then die with the signal's status */
static void on_crash(int sig)
{
	static const char m[] = "olpc-run: fatal signal, restoring the console\n";

	if (write(2, m, sizeof m - 1) < 0) {}
	if (g_map && !g_fake) memset(g_map, 0, g_stride * (size_t)g_fbh);
	console_text();
	if (o_running) unlink(o_running);
	_exit(128 + sig);
}

static void install_signals(void)
{
	struct sigaction sa;
	static const int term[] = { SIGINT, SIGTERM, SIGHUP };
	static const int crash[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT };
	unsigned i;

	memset(&sa, 0, sizeof sa);
	sigemptyset(&sa.sa_mask);
	sa.sa_handler = on_term;              /* no SA_RESTART: blocking waits must return */
	for (i = 0; i < sizeof term / sizeof term[0]; i++) sigaction(term[i], &sa, NULL);
	sa.sa_handler = on_alarm;
	sigaction(SIGALRM, &sa, NULL);
	sa.sa_handler = on_crash;
	for (i = 0; i < sizeof crash / sizeof crash[0]; i++) sigaction(crash[i], &sa, NULL);
	signal(SIGPIPE, SIG_IGN);
}

/* ------------------------------------------------------------------ video */

static struct {
	int ok;                     /* geometry configured */
	unsigned w, h, fmt;
	int k, x0, y0;
	struct blit b;
	uint8_t *prev;
	size_t pitch;
	int full;
} V;

static unsigned g_pixfmt = RETRO_PIXEL_FORMAT_0RGB1555;
static int g_video_null;                                 /* --no-video or no framebuffer */
static unsigned long st_drawn, st_skipped, st_dupes;
static u64 st_rows_drawn, st_rows_total;
static u64 g_blit_ns;                                    /* blit time of the current frame */
static volatile int g_skip_now;
static int g_last_k, g_last_x0, g_last_y0, g_last_dw, g_last_dh;

static int video_configure(unsigned w, unsigned h, unsigned fmt, size_t pitch)
{
	size_t sb = fmt == RETRO_PIXEL_FORMAT_XRGB8888 ? 4 : 2;

	if (V.ok) { blit_free(&V.b); free(V.prev); V.ok = 0; }
	V.k = scale_pick((int)w, (int)h, g_fbw, g_fbh);
	if (o_maxscale > 0 && V.k > o_maxscale) V.k = o_maxscale;
	if ((int)w * V.k > g_fbw || (int)h * V.k > g_fbh) {
		logf_("video: source %ux%u does not fit %dx%d", w, h, g_fbw, g_fbh);
		return -1;
	}
	V.x0 = (g_fbw - (int)w * V.k) / 2;
	V.y0 = (g_fbh - (int)h * V.k) / 2;
	V.w = w; V.h = h; V.fmt = fmt; V.pitch = pitch;
	if (blit_alloc(&V.b, (int)w, (int)h, V.k, (int)fmt, &g_tab, g_stride) < 0) return -1;
	V.prev = calloc(1, pitch * h + sb);
	if (!V.prev) return -1;
	V.full = 1;
	fb_clear();                                          /* borders cleared once */
	V.ok = 1;
	g_last_k = V.k; g_last_x0 = V.x0; g_last_y0 = V.y0;
	g_last_dw = (int)w * V.k; g_last_dh = (int)h * V.k;
	logf_("video: source %ux%u fmt %u scale %dx -> %dx%d at %d,%d", w, h, fmt, V.k,
	      g_last_dw, g_last_dh, V.x0, V.y0);
	return 0;
}

static void video_cb(const void *data, unsigned w, unsigned h, size_t pitch)
{
	u64 t0;

	if (!data) { st_dupes++; return; }                   /* RETRO_ENVIRONMENT_GET_CAN_DUPE */
	if (g_skip_now) { st_skipped++; return; }
	t0 = now_ns();
	if (!g_video_null) {
		int rows;
		if (!V.ok || V.w != w || V.h != h || V.fmt != g_pixfmt || V.pitch != pitch)
			if (video_configure(w, h, g_pixfmt, pitch) < 0) { g_video_null = 1; return; }
		rows = blit_rows(&V.b, data, pitch, V.prev, V.full,
				 g_map + (size_t)V.y0 * g_stride + (size_t)V.x0 * (size_t)g_tab.B);
		V.full = 0;
		st_rows_drawn += (u64)rows; st_rows_total += h;
	}
	if (o_blit_delay_ms) { u64 e = t0 + (u64)o_blit_delay_ms * 1000000ull; while (now_ns() < e) {} }
	st_drawn++;
	g_blit_ns += now_ns() - t0;
}

/* ------------------------------------------------------------------ audio, input callbacks */

static struct audio A;
static struct input IN;

static void audio_cb(int16_t l, int16_t r)
{
	int16_t f[2] = { l, r };

	audio_push(&A, f, 1);
}
static size_t audio_batch_cb(const int16_t *d, size_t n) { audio_push(&A, d, n); return n; }
static void poll_cb(void) {}                              /* input is polled once per frame by the main loop */

static int16_t input_cb(unsigned port, unsigned dev, unsigned idx, unsigned id)
{
	uint16_t m;

	(void)idx;
	if (port != 0 || dev != RETRO_DEVICE_JOYPAD) return 0;
	m = input_mask(&IN);
	if (id == RETRO_DEVICE_ID_JOYPAD_MASK) return (int16_t)m;
	return id < 16 ? (m >> id) & 1 : 0;
}

/* ------------------------------------------------------------------ environment */

static const char *g_savedir_env = "/tmp";

static void log_cb(enum retro_log_level lvl, const char *fmt, ...)
{
	va_list ap;

	if (lvl < RETRO_LOG_WARN) return;
	dlog("olpc-run: core: ");
	va_start(ap, fmt); dlogv(fmt, ap); va_end(ap);
}

static bool env_cb(unsigned cmd, void *data)
{
	switch (cmd) {
	case RETRO_ENVIRONMENT_GET_CAN_DUPE: *(bool *)data = true; return true;
	case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: {
		unsigned f = (unsigned)*(const enum retro_pixel_format *)data;
		if (f != RETRO_PIXEL_FORMAT_RGB565 && f != RETRO_PIXEL_FORMAT_XRGB8888 &&
		    f != RETRO_PIXEL_FORMAT_0RGB1555) return false;
		g_pixfmt = f;
		return true;
	}
	case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY: *(const char **)data = o_sysdir; return true;
	case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY: *(const char **)data = g_savedir_env; return true;
	case RETRO_ENVIRONMENT_GET_VARIABLE: ((struct retro_variable *)data)->value = NULL; return false;
	case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE: *(bool *)data = false; return true;
	case RETRO_ENVIRONMENT_GET_LOG_INTERFACE: ((struct retro_log_callback *)data)->log = log_cb; return true;
	case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION: *(unsigned *)data = 0; return true;
	case RETRO_ENVIRONMENT_SET_VARIABLES:
	case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
	case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
	case RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL:
	case RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS:
	case RETRO_ENVIRONMENT_SET_ROTATION:
	case RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK:
		return true;
	default: return false;
	}
}

/* ------------------------------------------------------------------ ROM loading */

static const char *rom_exts[] = { "gb", "gbc", "nes", "sgb", NULL };

static int has_rom_ext(const char *name)
{
	const char *dot = strrchr(name, '.');
	int i;

	if (!dot) return 0;
	for (i = 0; rom_exts[i]; i++) if (!strcasecmp(dot + 1, rom_exts[i])) return 1;
	return 0;
}

/* Run argv (busybox applets are found through PATH), collect stdout into *out. */
static int run_collect(char *const argv[], uint8_t **out, size_t *len, size_t cap)
{
	int p[2];
	pid_t pid;
	size_t n = 0, alloc = 65536;
	uint8_t *buf = malloc(alloc + 1);
	int st, over = 0;

	if (!buf || pipe(p) < 0) { free(buf); return -1; }
	pid = fork();
	if (pid < 0) { close(p[0]); close(p[1]); free(buf); return -1; }
	if (pid == 0) {
		int nul = open("/dev/null", O_WRONLY);
		dup2(p[1], 1);
		if (nul >= 0) dup2(nul, 2);
		close(p[0]); close(p[1]);
		execvp(argv[0], argv);
		_exit(127);
	}
	close(p[1]);
	for (;;) {
		ssize_t r;
		if (n == alloc) {
			uint8_t *nb;
			if (alloc >= cap) { over = 1; break; }
			nb = realloc(buf, (alloc *= 2) + 1);
			if (!nb) { over = 1; break; }
			buf = nb;
		}
		r = read(p[0], buf + n, alloc - n);
		if (r < 0 && errno == EINTR) continue;
		if (r <= 0) break;
		n += (size_t)r;
	}
	close(p[0]);
	if (over) kill(pid, SIGKILL);
	while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
	if (over || !WIFEXITED(st) || WEXITSTATUS(st) != 0) { free(buf); return -1; }
	buf[n] = 0;
	*out = buf; *len = n;
	return 0;
}

/* first entry of the archive with a ROM extension, via `unzip -l`; name copied to out */
static int zip_first_rom(const char *zip, char *out, size_t outsz)
{
	char *const argv[] = { "unzip", "-l", (char *)zip, NULL };
	uint8_t *buf;
	size_t len;
	char *p, *e;
	int past_hdr = 0;

	if (run_collect(argv, &buf, &len, 1 << 20) < 0) return -1;
	for (p = (char *)buf; p && *p; p = e ? e + 1 : NULL) {
		char *q;
		int field;
		e = strchr(p, '\n');
		if (e) *e = 0;
		if (!strncmp(p, "-----", 5)) { if (past_hdr) break; past_hdr = 1; continue; }
		if (!past_hdr) continue;
		/* "  Length  Date  Time  Name": skip three whitespace-separated fields */
		q = p;
		for (field = 0; field < 3; field++) {
			while (*q == ' ') q++;
			while (*q && *q != ' ') q++;
		}
		while (*q == ' ') q++;
		if (*q && has_rom_ext(q)) {
			snprintf(out, outsz, "%s", q);
			free(buf);
			return 0;
		}
	}
	free(buf);
	return -1;
}

static int zip_extract(const char *zip, const char *name, uint8_t **data, size_t *len)
{
	char esc[PATH_MAX * 2], *o = esc;
	const char *s;
	char *const argv[] = { "unzip", "-p", (char *)zip, esc, NULL };

	/* the entry name is matched as a glob: escape the metacharacters */
	for (s = name; *s && o < esc + sizeof esc - 2; s++) {
		if (strchr("[]*?\\", *s)) *o++ = '\\';
		*o++ = *s;
	}
	*o = 0;
	return run_collect(argv, data, len, MAX_ROM);
}

static int read_file(const char *path, uint8_t **data, size_t *len, size_t cap)
{
	FILE *f = fopen(path, "rb");
	long n;
	uint8_t *b;

	if (!f) return -1;
	if (fseek(f, 0, SEEK_END) < 0 || (n = ftell(f)) < 0 || (size_t)n > cap) { fclose(f); return -1; }
	rewind(f);
	b = malloc((size_t)n + 1);
	if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); fclose(f); return -1; }
	fclose(f);
	*data = b; *len = (size_t)n;
	return 0;
}

/* ------------------------------------------------------------------ saves */

static struct saver SVR;
static struct { unsigned id; const char *ext; } const SVK[] = { { RETRO_MEMORY_SAVE_RAM, ".srm" }, { RETRO_MEMORY_RTC, ".rtc" } };
static uint8_t *g_sram;               /* save RAM, for the --test-poke-save hook */
static size_t g_sram_size;
static int g_saves_ok;
static char g_tmprom[PATH_MAX];

/* Load the battery files into the core's memory and register them with the save helper.
 * The helper thread is started by the caller afterwards (sv_start), before SCHED_FIFO. */
static void saves_init(void *core, const char *savedir, const char *base)
{
	void *(*get_data)(unsigned) = (void *(*)(unsigned))dlsym(core, "retro_get_memory_data");
	size_t (*get_size)(unsigned) = (size_t (*)(unsigned))dlsym(core, "retro_get_memory_size");
	unsigned i;

	if (!get_data || !get_size) return;
	for (i = 0; i < sizeof SVK / sizeof SVK[0]; i++) {
		uint8_t *d = NULL, *ptr = get_data(SVK[i].id);
		size_t n = 0, size = get_size(SVK[i].id);
		char path[PATH_MAX];

		if (!size || !ptr) continue;
		if (mkdir(savedir, 0755) < 0 && errno != EEXIST) {
			logf_("saves: cannot create %s (%s): saving disabled", savedir, strerror(errno));
			return;
		}
		snprintf(path, sizeof path, "%s/%s%s", savedir, base, SVK[i].ext);
		if (read_file(path, &d, &n, 64u << 20) == 0) {
			memcpy(ptr, d, n < size ? n : size);
			if (n != size) logf_("saves: %s is %zu bytes, core wants %zu", path, n, size);
			free(d);
			logf_("saves: loaded %s (%zu bytes)", path, n);
		} else {
			logf_("saves: no %s yet", path);
		}
		if (sv_register(&SVR, path, ptr, size) == 0) g_saves_ok = 1;      /* "unchanged" baseline = what was loaded */
		if (SVK[i].id == RETRO_MEMORY_SAVE_RAM) { g_sram = ptr; g_sram_size = size; }
	}
}

/* ------------------------------------------------------------------ real-time priority */

#define RT_PRIO 10
#define RT_NOWAIT_NS 1000000ull        /* an audio wait shorter than this did not really block */
#define RT_NOWAIT_FRAMES 90            /* ~1.5 s of frames that never blocked: give the CPU back */
static const char *g_rt = "off";       /* fifo | demoted | failed | no-rt | n/a (reported in the stats) */
static int g_mlock = -1;               /* -1 not tried, 0 failed, 1 locked */
static int g_rt_policy = -1;           /* sched_getscheduler() read back after the change, -1 = not read */
static int g_rt_errno;                 /* errno of a failed sched_setscheduler */

/* musl's sched_setscheduler()/sched_getscheduler() are stubs that fail with ENOSYS, so go
 * through the raw system calls. */
static int sched_set(int policy, int prio)
{
	struct sched_param sp;

	memset(&sp, 0, sizeof sp);
	sp.sched_priority = prio;
	return (int)syscall(SYS_sched_setscheduler, 0, policy, &sp);
}
static int sched_get(void) { return (int)syscall(SYS_sched_getscheduler, 0); }

/* Pagefaults and a 10 ms scheduler tick (HZ=100, voluntary preemption) are what can
 * make one frame run long, so lock the memory and run above the background shell
 * jobs.  SCHED_FIFO is only safe when the loop sleeps on the sound card every frame:
 * poll() on the PCM blocks for about 10 of every 16.6 ms, which leaves every normal
 * task (olpc-chordd, the log sync loop, the console) the rest.  So it is only taken
 * with a real PCM, and the audio wait is watched (rt_watch): if frames stop blocking
 * (the core is slower than the card, the buffer is always empty) it falls back to
 * SCHED_OTHER instead of starving the system.  Failures are logged and ignored; the
 * policy is read back so the stats say whether it really took effect. */
static void rt_start(const struct audio *a)
{
	if (o_nort) { g_rt = "no-rt"; logf_("rt: disabled (--no-rt)"); return; }
	g_mlock = mlockall(MCL_CURRENT | MCL_FUTURE) == 0;
	if (!g_mlock) logf_("rt: mlockall failed: %s (continuing)", strerror(errno));
	if (a->mode != AUD_ALSA) { g_rt = "n/a"; logf_("rt: SCHED_FIFO not used without a sound card pacing the loop"); return; }
	if (sched_set(SCHED_FIFO, RT_PRIO) == 0) {
		g_rt_policy = sched_get();
		g_rt = g_rt_policy == SCHED_FIFO ? "fifo" : "failed";
		logf_("rt: SCHED_FIFO priority %d, policy read back %d, mlockall=%d", RT_PRIO, g_rt_policy, g_mlock);
	} else {
		g_rt_errno = errno;
		g_rt = "failed";
		logf_("rt: SCHED_FIFO failed: %s (continuing)", strerror(errno));
	}
}

/* called once per frame with the time spent waiting for the audio device */
static void rt_watch(u64 wait_ns)
{
	static int nowait;

	if (strcmp(g_rt, "fifo")) return;
	nowait = wait_ns < RT_NOWAIT_NS ? nowait + 1 : 0;
	if (nowait < RT_NOWAIT_FRAMES) return;
	sched_set(SCHED_OTHER, 0);
	g_rt_policy = sched_get();
	g_rt = "demoted";
	dlog("olpc-run: rt: %d frames without waiting on the audio device (core too slow): back to SCHED_OTHER\n", nowait);
}

/* ------------------------------------------------------------------ statistics */

struct acc { u64 sum, max; };
static struct acc a_core, a_blit, a_audio, a_work, a_frame;
static unsigned long st_frames;
/* frame time histogram (t3 - t0): <14, 14-18, 18-22, 22-30, 30-40, >40 ms */
static unsigned long st_hist[6];
static const char *const hist_name[6] = { "lt14", "14_18", "18_22", "22_30", "30_40", "gt40" };
static unsigned long st_core12, st_blit12, st_wait12;     /* frames where that phase took more than 12 ms */
#define OVER_NS 12000000ull

static void hist_add(u64 ns)
{
	static const u64 lim[5] = { 14000000ull, 18000000ull, 22000000ull, 30000000ull, 40000000ull };
	int i = 0;

	while (i < 5 && ns >= lim[i]) i++;
	st_hist[i]++;
}
static u64 g_t_start;

static void acc_add(struct acc *a, u64 v) { a->sum += v; if (v > a->max) a->max = v; }

static const char *g_core_name = "", *g_rom_name = "";
static struct xrun_ring g_xr;
#define STATS_BUF (XR_MAX * 260 + 6000)

static unsigned long g_stalls_done;
static int g_saves_unfinished = -1;      /* -1 = not waited for yet */

/* The statistics as text in buf; returns the length.  Pure memory work: no file is touched. */
static size_t stats_format(char *buf, size_t cap, const char *status)
{
	double wall = (now_ns() - g_t_start) / 1e9;
	double n = st_frames ? (double)st_frames : 1, nd = st_drawn ? (double)st_drawn : 1;
	size_t o = 0;
	int i;
#define P(...) do { if (o < cap) { int w_ = snprintf(buf + o, cap - o, __VA_ARGS__); if (w_ > 0) o += (size_t)w_ < cap - o ? (size_t)w_ : cap - o - 1; } } while (0)

	P("core=%s\nrom=%s\nstatus=%s\n", g_core_name, g_rom_name, status);
	P("frames_run=%lu\nframes_drawn=%lu\nframes_skipped=%lu\nframes_dupe=%lu\n", st_frames, st_drawn, st_skipped, st_dupes);
	P("wall_s=%.3f\nfps_effective=%.2f\n", wall, wall > 0 ? st_frames / wall : 0.0);
	P("core_ms_avg=%.3f\ncore_ms_max=%.3f\n", a_core.sum / 1e6 / n, a_core.max / 1e6);
	P("blit_ms_avg=%.3f\nblit_ms_max=%.3f\n", a_blit.sum / 1e6 / nd, a_blit.max / 1e6);
	P("audio_wait_ms_avg=%.3f\naudio_wait_ms_max=%.3f\n", a_audio.sum / 1e6 / n, a_audio.max / 1e6);
	P("work_ms_avg=%.3f\nwork_ms_max=%.3f\n", a_work.sum / 1e6 / n, a_work.max / 1e6);
	P("frame_ms_avg=%.3f\nframe_ms_max=%.3f\n", a_frame.sum / 1e6 / n, a_frame.max / 1e6);
	for (i = 0; i < 6; i++) P("frame_hist_%s=%lu\n", hist_name[i], st_hist[i]);
	P("core_over12_frames=%lu\nblit_over12_frames=%lu\naudio_wait_over12_frames=%lu\n", st_core12, st_blit12, st_wait12);
	P("underruns=%llu\nunderruns_poll=%llu\nunderruns_write=%llu\nunderrun_recover_failed=%llu\naudio_stalls=%llu\n",
	  A.underruns, A.underruns_poll, A.underruns_write, A.recover_fail, A.stalls);
	P("recover_prepare=%lu\nrecover_reopen=%lu\nrecover_clock=%lu\nrecover_reopen_failed=%lu\n",
	  A.n_action[RA_PREPARE], A.n_action[RA_REOPEN], A.n_action[RA_CLOCK], A.reopen_failed);
	P("incidents=%lu\nincidents_recovered=%lu\nrecovery_paced_frames=%lu\n", A.incident_no, A.incident_done, A.paced_frames);
	P("fill_query=%d\n", !o_nofill);
	if (o_nofill) {
		P("audio_fill_min=-1\naudio_fill_max=-1\naudio_fill_min_ms=-1\naudio_fill_max_ms=-1\n");
	} else {
		P("audio_fill_min=%u\naudio_fill_max=%u\n", A.fill_min == UINT_MAX ? 0 : A.fill_min, A.fill_max);
		P("audio_fill_min_ms=%.1f\naudio_fill_max_ms=%.1f\n", A.fill_min == UINT_MAX || !A.rate ? 0.0 : 1000.0 * A.fill_min / A.rate,
		  A.rate ? 1000.0 * A.fill_max / A.rate : 0.0);
	}
	P("xrun_debug_req=%ld\nxrun_debug_applied=%d\nxrun_debug_errno=%d\nxrun_debug_path=%s\n", o_xrun_debug, g_xrun_debug_ok, A.xd_errno, A.xd_path);
	P("test_stall_ms=%d\ntest_stalls_done=%lu\n", o_stall_ms, g_stalls_done);
	P("saves_unfinished=%d\n", g_saves_unfinished);
	{
		unsigned long w = 0, f = 0;
		for (i = 0; i < SVR.n; i++) { w += SVR.slot[i].written; f += SVR.slot[i].failed; }
		P("saves_written=%lu\nsaves_failed=%lu\n", w, f);
	}
	P("audio_req_period_ms=%.2f\naudio_req_latency_ms=%.1f\naudio_req_target_frames=%u\n", o_period_ms, o_latency_ms, o_target);
	P("audio_period_ms=%.2f\naudio_target_ms=%.1f\naudio_avail_min=%u\naudio_start_threshold=%u\n",
	  A.rate ? 1000.0 * A.period / A.rate : 0.0, A.rate ? 1000.0 * A.target / A.rate : 0.0, A.avail_min, A.start_threshold);
	P("rt=%s\nrt_policy=%d\nrt_errno=%d\nmlockall=%d\n", g_rt, g_rt_policy, g_rt_errno, g_mlock);
	P("audio_mode=%s\naudio_core_rate=%u\naudio_device_rate=%u\naudio_period=%u\naudio_buffer=%u\naudio_target=%u\n",
	  A.mode == AUD_ALSA ? "alsa" : A.mode == AUD_CLOCK ? "clock" : "null", A.core_rate, A.rate, A.period, A.bufsz, A.target);
	P("video_mode=%s\nvideo_scale=%d\nvideo_rect=%dx%d+%d+%d\nrows_drawn_pct=%.1f\n",
	  g_video_null ? "null" : g_fake ? "fake" : "fb", g_last_k, g_last_dw, g_last_dh, g_last_x0, g_last_y0,
	  st_rows_total ? 100.0 * st_rows_drawn / st_rows_total : 0.0);
	if (o < cap) o += xr_format(&g_xr, buf + o, cap - o);
#undef P
	return o;
}

/* Write the statistics to path (temp file + rename).  sync: fsync the data first.  The only
 * callers are the exit path (--stats, on the stick) and the 10 s copy on tmpfs (--stats-live):
 * the emulation loop never writes to the stick. */
static void stats_write(const char *path, const char *status, int sync)
{
	static char buf[STATS_BUF];
	char tmp[PATH_MAX + 8];
	size_t n;
	int fd;
	size_t off = 0;

	if (!path) return;
	n = stats_format(buf, sizeof buf, status);
	snprintf(tmp, sizeof tmp, "%s.tmp", path);
	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd < 0) return;
	while (off < n) {
		ssize_t w = write(fd, buf + off, n - off);
		if (w < 0 && errno == EINTR) continue;
		if (w <= 0) break;
		off += (size_t)w;
	}
	if (sync) fsync(fd);
	close(fd);
	if (off != n || rename(tmp, path) < 0) unlink(tmp);
}

/* ------------------------------------------------------------------ main */

/* frames of audio kept in hand beyond the expected core + blit time before a blit is
 * skipped; device-rate frames, independent of the PCM period (about 4 ms at 32 kHz) */
#define SKIP_MARGIN 128
#define SAVE_WAIT_MS 8000      /* exit waits at most this long for the save helper (a dead SD card) */

static void usage(void)
{
	fputs("usage: olpc-run [options] <gb|gbc|nes|core.so> <rom|rom.zip>\n"
	      "  --core NAME|PATH   override the core (e.g. --core fceumm)\n"
	      "  --savedir DIR      battery saves (default: <rom dir>/../saves)\n"
	      "  --stats FILE       write statistics here at exit (never while playing: it is on the stick)\n"
	      "  --stats-live FILE  also write them here every 10 s (for tmpfs, so a killed run leaves something)\n"
	      "  --frames N         exit after N frames\n"
	      "  --max-seconds N    exit after N seconds\n"
	      "  --no-audio         null audio sink, no pacing\n"
	      "  --no-video         null video sink\n"
	      "  --max-scale N      cap the integer scale (default: largest that fits 1200x900)\n"
	      "  --max-skip N       max consecutive skipped blits (default 4)\n"
	      "  --audio-period-ms X  PCM period in ms (default 4); the wake-up granularity of the pacing\n"
	      "  --audio-latency-ms X wanted audio buffer fill in ms (default 45)\n"
	      "  --audio-target N   wanted fill in frames, overrides --audio-latency-ms\n"
	      "  --no-rt            no SCHED_FIFO and no mlockall\n"
	      "  --xrun-debug N     write N to /proc/asound/cardX/pcmDp/xrun_debug at start (0..31);\n"
	      "                     bit 0 = log XRUN and hw_ptr anomalies, bit 1 = + stack dump, bit 2 = jiffies check\n"
	      "  --test-stall-ms M --test-stall-at F[,F2..]  sleep M ms in the loop at those frames (recovery test)\n"
	      "  --no-fill-query    never ask the driver for the fill (DELAY): pace with poll() only\n"
	      "  --running-file F   marker created while playing (default /run/olpcboy/running)\n"
	      "  --audio-dev PATH   PCM device (default: probe /dev/snd)\n"
	      "  --libdir DIR       core directory (default /usr/lib/libretro)\n"
	      "  --sysdir DIR       libretro system directory\n"
	      "  --restore-console  only restore the text console and clear the screen\n", stderr);
}

#define SYM(n) do { *(void **)&n = dlsym(h, "retro_" #n); if (!n) { logf_("core lacks retro_" #n); return 1; } } while (0)

int main(int argc, char **argv)
{
	const char *sys = NULL, *romarg = NULL;
	char corepath[PATH_MAX], savedir[PATH_MAX], base[PATH_MAX], tmp[PATH_MAX], zipname[PATH_MAX];
	int is_zip = 0;
	int poked = 0;
	uint8_t *rom = NULL;
	size_t romlen = 0;
	void *h;
	struct retro_system_info si;
	struct retro_system_av_info av;
	struct retro_game_info gi;
	void (*set_environment)(retro_environment_t), (*set_video_refresh)(retro_video_refresh_t),
	     (*set_audio_sample)(retro_audio_sample_t), (*set_audio_sample_batch)(retro_audio_sample_batch_t),
	     (*set_input_poll)(retro_input_poll_t), (*set_input_state)(retro_input_state_t),
	     (*init)(void), (*run)(void), (*unload_game)(void), (*deinit)(void),
	     (*get_system_info)(struct retro_system_info *), (*get_system_av_info)(struct retro_system_av_info *);
	bool (*load_game)(const struct retro_game_info *);
	const char *reason = "frames";
	u64 t_stats, t_save, ema_core = 0, ema_blit = 8000000;
	u64 frame_ns, prev_t0 = 0, work_hist[3] = { 0, 0, 0 };     /* work of the last frames, latest first */
	unsigned long long seen_underruns = 0;
	unsigned long inc_seen = 0, inc_seq = 0;
	int inc_open = 0;
	int i, consec = 0;
	char *rp;

	for (i = 1; i < argc; i++) {
		const char *a = argv[i];
#define OPTV(name) (!strcmp(a, name) && i + 1 < argc ? argv[++i] : NULL)
		const char *v;
		if ((v = OPTV("--core"))) o_core = v;
		else if ((v = OPTV("--savedir"))) o_savedir = v;
		else if ((v = OPTV("--sysdir"))) o_sysdir = v;
		else if ((v = OPTV("--stats"))) o_stats = v;
		else if ((v = OPTV("--stats-live"))) o_stats_live = v;
		else if ((v = OPTV("--libdir"))) o_libdir = v;
		else if ((v = OPTV("--audio-dev"))) o_audiodev = v;
		else if ((v = OPTV("--chord-file"))) o_chord = v;
		else if ((v = OPTV("--input-dir"))) o_inputdir = v;
		else if ((v = OPTV("--frames"))) o_frames = strtoul(v, NULL, 10);
		else if ((v = OPTV("--max-seconds"))) o_maxsec = atoi(v);
		else if ((v = OPTV("--max-scale"))) o_maxscale = atoi(v);
		else if ((v = OPTV("--max-skip"))) o_maxskip = atoi(v);
		else if ((v = OPTV("--audio-target"))) o_target = (unsigned)atoi(v);
		else if ((v = OPTV("--audio-period-ms"))) o_period_ms = atof(v);
		else if ((v = OPTV("--audio-latency-ms"))) o_latency_ms = atof(v);
		else if ((v = OPTV("--running-file"))) o_running = *v ? v : NULL;
		else if (!strcmp(a, "--no-rt")) o_nort = 1;
		else if (!strcmp(a, "--no-fill-query")) o_nofill = 1;
		else if ((v = OPTV("--xrun-debug"))) {
			unsigned long xd;
			if (parse_uint(v, 31, &xd) < 0) { logf_("--xrun-debug wants a number 0..31, got '%s'", v); return 2; }
			o_xrun_debug = (long)xd;
		}
		else if ((v = OPTV("--test-blit-delay-ms"))) o_blit_delay_ms = atoi(v);
		else if ((v = OPTV("--test-stall-ms"))) {
			unsigned long ms;
			if (parse_uint(v, 10000, &ms) < 0) { logf_("--test-stall-ms wants 0..10000, got '%s'", v); return 2; }
			o_stall_ms = (int)ms;
		}
		else if ((v = OPTV("--test-stall-at"))) {
			o_nstall = parse_frame_list(v, o_stall_at, sizeof o_stall_at / sizeof o_stall_at[0]);
			if (o_nstall < 0) { logf_("--test-stall-at wants frame numbers like 300,700 (at most 8), got '%s'", v); return 2; }
		}
		else if ((v = OPTV("--test-save-period-s"))) o_save_period_s = (unsigned)atoi(v);
		else if ((v = OPTV("--test-save-delay-ms"))) o_save_delay_ms = atoi(v);
		else if (!strcmp(a, "--test-poke-save")) o_poke_save = 1;
		else if (!strcmp(a, "--no-audio")) o_noaudio = 1;
		else if (!strcmp(a, "--no-video")) o_novideo = 1;
		else if (!strcmp(a, "--restore-console")) o_restore = 1;
		else if (a[0] == '-' && a[1]) { usage(); return 2; }
		else if (!sys) sys = a;
		else if (!romarg) romarg = a;
		else { usage(); return 2; }
	}
	if (o_restore) {
		if (fb_open(1)) { g_kd_graphics = 1; fb_close(); }
		else { tty_open(); g_kd_graphics = 1; console_redraw(); }
		return 0;
	}
	if (!sys || !romarg) { usage(); return 2; }

	/* ---- which core */
	{
		const char *name = o_core ? o_core : !strcmp(sys, "gb") || !strcmp(sys, "gbc") ? "gambatte"
				 : !strcmp(sys, "nes") ? "quicknes" : sys;
		if (strchr(name, '/') || strstr(name, ".so")) snprintf(corepath, sizeof corepath, "%s", name);
		else snprintf(corepath, sizeof corepath, "%s/%s_libretro.so", o_libdir, name);
		if (!o_core && strcmp(sys, "gb") && strcmp(sys, "gbc") && strcmp(sys, "nes") &&
		    !strchr(sys, '/') && !strstr(sys, ".so")) {
			logf_("unknown system '%s' (use gb, gbc, nes or a core path)", sys);
			return 2;
		}
		g_core_name = strrchr(corepath, '/') ? strrchr(corepath, '/') + 1 : corepath;
	}

	install_signals();
	g_t_start = now_ns();
	if (o_maxsec > 0) alarm((unsigned)o_maxsec);

	/* ---- ROM (plain or zip) */
	g_rom_name = romarg;
	{
		uint8_t magic[4] = { 0 };
		FILE *f = fopen(romarg, "rb");
		if (!f) { logf_("%s: %s", romarg, strerror(errno)); return 1; }
		if (fread(magic, 1, 4, f) < 4) memset(magic, 0, 4);
		fclose(f);
		if (!memcmp(magic, "PK\3\4", 4)) {
			if (zip_first_rom(romarg, zipname, sizeof zipname) < 0) { logf_("%s: no .gb/.gbc/.nes entry in the archive", romarg); return 1; }
			if (zip_extract(romarg, zipname, &rom, &romlen) < 0) { logf_("%s: cannot extract '%s'", romarg, zipname); return 1; }
			is_zip = 1;
			logf_("rom: zip entry '%s', %zu bytes", zipname, romlen);
		} else if (read_file(romarg, &rom, &romlen, MAX_ROM) < 0) {
			logf_("%s: cannot read (or larger than %u bytes)", romarg, MAX_ROM);
			return 1;
		}
	}

	/* ---- core */
	h = dlopen(corepath, RTLD_NOW);
	if (!h) { logf_("dlopen %s: %s", corepath, dlerror()); return 1; }
	SYM(set_environment); SYM(set_video_refresh); SYM(set_audio_sample); SYM(set_audio_sample_batch);
	SYM(set_input_poll); SYM(set_input_state); SYM(init); SYM(run); SYM(unload_game); SYM(deinit);
	SYM(get_system_info); SYM(get_system_av_info); SYM(load_game);

	/* save directory: sibling of the ROM directory unless given */
	rp = realpath(romarg, NULL);
	snprintf(tmp, sizeof tmp, "%s", rp ? rp : romarg);
	free(rp);
	snprintf(base, sizeof base, "%s", basename(tmp));
	{ char *dot = strrchr(base, '.'); if (dot && dot != base) *dot = 0; }
	if (o_savedir) snprintf(savedir, sizeof savedir, "%s", o_savedir);
	else {
		char d1[PATH_MAX], d2[PATH_MAX];
		snprintf(d1, sizeof d1, "%s", tmp);
		snprintf(d2, sizeof d2, "%s", dirname(d1));
		snprintf(d1, sizeof d1, "%s", d2);
		snprintf(savedir, sizeof savedir, "%s/saves", dirname(d1));
	}
	g_savedir_env = savedir;

	set_environment(env_cb);
	init();
	set_video_refresh(video_cb); set_audio_sample(audio_cb); set_audio_sample_batch(audio_batch_cb);
	set_input_poll(poll_cb); set_input_state(input_cb);
	memset(&si, 0, sizeof si);
	get_system_info(&si);
	memset(&gi, 0, sizeof gi);
	gi.path = romarg; gi.data = rom; gi.size = romlen;
	if (si.need_fullpath && is_zip) {
		/* the core wants a real file: put the extracted ROM on tmpfs */
		const char *ext = strrchr(zipname, '.');
		snprintf(g_tmprom, sizeof g_tmprom, "%s/olpc-run-rom%s", access("/run/olpcboy", W_OK) == 0 ? "/run/olpcboy" : "/tmp", ext ? ext : "");
		if (sv_write_atomic(g_tmprom, rom, romlen) < 0) { logf_("cannot write %s", g_tmprom); return 1; }
		gi.path = g_tmprom;
	}
	if (!load_game(&gi)) { logf_("%s: load_game failed", g_core_name); return 1; }
	memset(&av, 0, sizeof av);
	get_system_av_info(&av);
	logf_("core %s %s: %ux%u @ %.3f fps, audio %.0f Hz, pixel format %u", si.library_name, si.library_version,
	      av.geometry.base_width, av.geometry.base_height, av.timing.fps, av.timing.sample_rate, g_pixfmt);
	saves_init(h, savedir, base);

	/* ---- sinks */
	if (!o_novideo && !fb_open(1)) { logf_("video: no usable framebuffer, running with a null video sink"); g_video_null = 1; }
	if (o_novideo) { g_video_null = 1; logf_("video: null sink (--no-video)"); }
	if (o_period_ms < 0.5 || o_period_ms > 200) o_period_ms = 4.0;
	if (o_latency_ms < 5 || o_latency_ms > 500) o_latency_ms = 45.0;
	audio_open(&A, (unsigned)(av.timing.sample_rate + 0.5), o_audiodev, o_noaudio, o_period_ms, o_latency_ms, o_target,
		   (volatile int *)&g_quit);
	A.no_query = o_nofill;
	if (o_xrun_debug >= 0) g_xrun_debug_ok = audio_xrun_debug(&A, (unsigned)o_xrun_debug) == 0;
	input_init(&IN, o_inputdir, o_chord);
	/* background jobs (log sync) read this to back off while a game runs; it holds our pid so
	 * a stale file left by a SIGKILL is recognisable */
	if (o_running) {
		FILE *rf = fopen(o_running, "w");
		if (rf) { fprintf(rf, "%d\n", (int)getpid()); fclose(rf); }
	}
	/* The save helper is created before SCHED_FIFO is taken, so it starts at normal priority
	 * (it also resets itself); it only ever sees private copies of the save RAM. */
	SVR.test_write_delay_ms = o_save_delay_ms;
	if (g_saves_ok && sv_start(&SVR) < 0) { logf_("saves: cannot start the helper thread: saving disabled"); g_saves_ok = 0; SVR.n = 0; }
	rt_start(&A);

	/* ---- main loop */
	frame_ns = av.timing.fps > 1.0 ? (u64)(1e9 / av.timing.fps) : 16666667ull;
	A.frame_ns = frame_ns;
	t_stats = t_save = now_ns();
	dlog_buffering(1);               /* from here on nothing is written to stderr (the stick) until exit */
	for (;;) {
		u64 t0, t1, t2, t3, core, blit, wait;

		if (o_poke_save && !poked && st_frames == 100 && g_sram_size) { g_sram[0] ^= 0xff; poked = 1; }
		if (g_quit) { reason = g_timeout ? "timeout" : "signal"; break; }
		if (o_nstall && o_stall_ms > 0) {                     /* test hook: a deliberate stall, outside the measured frame */
			int k;
			for (k = 0; k < o_nstall; k++) if (o_stall_at[k] == st_frames) {
				struct timespec ts = { o_stall_ms / 1000, (long)(o_stall_ms % 1000) * 1000000L };
				nanosleep(&ts, NULL);
				g_stalls_done++;
				break;
			}
		}
		if (o_frames && st_frames >= o_frames) { reason = "frames"; break; }
		t0 = now_ns();
		input_poll(&IN, t0);
		if (input_exit(&IN, t0, 1000000000ull)) { reason = "exit-button"; break; }

		/* behind schedule when the audio buffer will not outlast core + blit: skip the blit */
		g_skip_now = 0;
		if (st_frames >= 30 && consec < o_maxskip) {
			if (o_nofill) {
				g_skip_now = skip_by_time(work_hist[0], frame_ns);
			} else {
				long fill = audio_fill(&A);
				if (fill >= 0 && (u64)fill < (ema_core + ema_blit) * A.rate / 1000000000ull + SKIP_MARGIN) g_skip_now = 1;
			}
		}
		g_blit_ns = 0;
		t1 = now_ns();
		run();
		t2 = now_ns();
		audio_flush(&A);
		t3 = now_ns();

		blit = g_blit_ns;
		core = (t2 - t1) > blit ? (t2 - t1) - blit : 0;
		wait = t3 - t2;
		st_frames++;
		acc_add(&a_core, core); acc_add(&a_audio, wait); acc_add(&a_work, core + blit); acc_add(&a_frame, t3 - t0);
		hist_add(t3 - t0);
		if (core > OVER_NS) st_core12++;
		if (blit > OVER_NS) st_blit12++;
		if (wait > OVER_NS) st_wait12++;
		rt_watch(wait);
		while (seen_underruns < A.underruns) {                /* one event per underrun found in this flush */
			struct xrun_ev ev;
			ev.frame = st_frames;
			ev.since_prev_ms = prev_t0 ? (t0 - prev_t0) / 1e6 : 0.0;
			ev.work_ms[0] = work_hist[0] / 1e6; ev.work_ms[1] = work_hist[1] / 1e6; ev.work_ms[2] = work_hist[2] / 1e6;
			ev.fill = o_nofill ? -1 : A.last_fill;
			ev.via = A.last_via;
			ev.action = A.last_action;
			ev.rec_frames = -1; ev.rec_ms = -1.0;
			if (A.incident_no != inc_seen) { inc_seen = A.incident_no; inc_seq = xr_add(&g_xr, &ev); inc_open = 1; }
			else xr_add(&g_xr, &ev);
			seen_underruns++;
		}
		if (inc_open && A.incident_done == A.incident_no) {   /* the stream is stable again */
			xr_set_recovery(&g_xr, inc_seq, A.inc_frames, A.inc_ms);
			inc_open = 0;
		}
		prev_t0 = t0;
		work_hist[2] = work_hist[1]; work_hist[1] = work_hist[0]; work_hist[0] = core + blit;
		if (blit) { acc_add(&a_blit, blit); ema_blit += ((long long)blit - (long long)ema_blit) / 8; }
		ema_core += ((long long)core - (long long)ema_core) / 8;
		consec = g_skip_now && !blit ? consec + 1 : 0;

		if (o_stats_live && t3 - t_stats >= 10000000000ull) { stats_write(o_stats_live, "running", 0); t_stats = t3; }
		if (g_saves_ok && t3 - t_save >= (u64)o_save_period_s * 1000000000ull) {
			/* snapshot only: the helper thread does the writing.  When it was busy, ask again in 1 s */
			t_save = sv_check(&SVR, "periodic") ? t3 - (u64)o_save_period_s * 1000000000ull + 1000000000ull : t3;
		}
	}

	/* ---- shutdown: take the private copy of the save RAM while the core is still loaded, give the
	 * screen back, then wait (bounded) for the helper to put it on disk */
	if (o_running) unlink(o_running);
	if (g_saves_ok) sv_capture_final(&SVR);
	if (g_tmprom[0]) unlink(g_tmprom);
	unload_game();
	deinit();
	audio_close(&A);
	if (!g_video_null || g_map) fb_close();
	if (g_saves_ok) {
		g_saves_unfinished = sv_wait(&SVR, SAVE_WAIT_MS);
		if (g_saves_unfinished) dlog("olpc-run: saves: %d write(s) did not finish within %d ms: giving up\n", g_saves_unfinished, SAVE_WAIT_MS);
	}
	dlog_buffering(0);
	stats_write(o_stats, reason, 0);
	logf_("exit: %s after %lu frames (%lu drawn, %lu skipped, %llu underruns, %.2f fps)", reason, st_frames,
	      st_drawn, st_skipped, A.underruns, (now_ns() - g_t_start) > 0 ? st_frames / ((now_ns() - g_t_start) / 1e9) : 0.0);
	return 0;
}
