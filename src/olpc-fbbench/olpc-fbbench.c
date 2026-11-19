/*
 * olpc-fbbench: measurement tool for the OLPC XO-1 (Geode LX, lxfb, cs5535audio).
 *
 * Measures what a software-rendering emulator frontend needs to know:
 * framebuffer facts and bandwidth, scaled blits (several strategies), an 8 bpp
 * mode switch, audio hw params and silence playback, timer jitter.  Results go
 * to a plain key=value log (default /var/log/olpc-current/fbbench.log).
 *
 * Build (static, inside the builder container):  make -C src/olpc-fbbench
 * Host self-test of the scaling code:             make -C src/olpc-fbbench test
 *
 * Safety: the original video mode is restored on every exit path including
 * signals; a hard alarm() bounds the run.  Missing /dev/fb0 or sound card are
 * logged and skipped (exit 0).  Only MMX is used unconditionally; movntq/sfence
 * (MMX extensions) are used only when CPUID reports them, and a SIGILL guard
 * turns a wrong CPUID answer into a skipped test.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <setjmp.h>
#include <time.h>
#include <limits.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/utsname.h>
#include <linux/fb.h>
#include <linux/kd.h>
#include <linux/vt.h>
#include <cpuid.h>
#include <mmintrin.h>

#define NELEM(a) ((int)(sizeof(a) / sizeof((a)[0])))
typedef uint64_t u64;

/* ------------------------------------------------------------------ pixel formats */

/* Contribution tables: pixel = r[v>>11] + g[(v>>5)&63] + b[v&31] (fields are
 * disjoint for colour formats; for 8 bpp they hold luminance parts). */
struct pixtab {
	int B;            /* bytes per pixel: 1, 2 or 4 */
	int ident16;      /* dst is RGB565: the converted row is the source row */
	uint32_t r[32], g[64], b[32];
};

static void pixtab_init(struct pixtab *t, int bpp, int ro, int rl, int go, int gl,
			int bo, int bl)
{
	int i;

	memset(t, 0, sizeof *t);
	t->B = bpp / 8;
	for (i = 0; i < 32; i++) {
		unsigned r8 = (i << 3) | (i >> 2);
		t->r[i] = bpp == 8 ? (r8 * 299 + 500) / 1000
				   : (uint32_t)(r8 >> (8 - rl)) << ro;
		t->b[i] = bpp == 8 ? (r8 * 114 + 500) / 1000
				   : (uint32_t)(r8 >> (8 - bl)) << bo;
	}
	for (i = 0; i < 64; i++) {
		unsigned g8 = (i << 2) | (i >> 4);
		t->g[i] = bpp == 8 ? (g8 * 587 + 500) / 1000
				   : (uint32_t)(g8 >> (8 - gl)) << go;
	}
	t->ident16 = bpp == 16 && ro == 11 && rl == 5 && go == 5 && gl == 6 && bo == 0 && bl == 5;
}

static inline uint32_t conv_px(const struct pixtab *t, uint16_t v)
{
	return t->r[v >> 11] + t->g[(v >> 5) & 63] + t->b[v & 31];
}

static void store_px(uint8_t *p, int B, uint32_t v)
{
	if (B == 1) *p = (uint8_t)v;
	else if (B == 2) *(uint16_t *)p = (uint16_t)v;
	else *(uint32_t *)p = v;
}

/* RGB565 source row -> destination-format row (w pixels). */
static void conv_row(const struct pixtab *t, const uint16_t *src, int w, uint8_t *out)
{
	int i;

	if (t->ident16) { memcpy(out, src, (size_t)w * 2); return; }
	if (t->B == 1) for (i = 0; i < w; i++) out[i] = (uint8_t)conv_px(t, src[i]);
	else if (t->B == 2) for (i = 0; i < w; i++) ((uint16_t *)out)[i] = (uint16_t)conv_px(t, src[i]);
	else for (i = 0; i < w; i++) ((uint32_t *)out)[i] = conv_px(t, src[i]);
}

/* ------------------------------------------------------------------ CPUID */

struct cpuinfo {
	char vendor[13];
	uint32_t max_leaf, l1_eax, l1_ebx, l1_ecx, l1_edx;
	uint32_t max_ext, e1_edx, e1_ecx;
	char brand[49];
	int mmx, sse, mmxext, amd3dnow, amd3dnowext, cmov;
	int nt_ok;                 /* movntq + sfence may be used */
};
static struct cpuinfo g_cpu;

static void cpu_detect(struct cpuinfo *c)
{
	unsigned a, b, d, e;

	memset(c, 0, sizeof *c);
	if (!__get_cpuid(0, &a, &b, &d, &e)) return;
	c->max_leaf = a;
	memcpy(c->vendor, &b, 4); memcpy(c->vendor + 4, &e, 4); memcpy(c->vendor + 8, &d, 4);
	if (c->max_leaf >= 1) {
		__get_cpuid(1, &a, &b, &e, &d);          /* eax ebx ecx edx */
		c->l1_eax = a; c->l1_ebx = b; c->l1_ecx = e; c->l1_edx = d;
	}
	c->max_ext = __get_cpuid(0x80000000u, &a, &b, &e, &d) ? a : 0;   /* 0: no extended leaves */
	if (c->max_ext >= 0x80000001u) {
		__get_cpuid(0x80000001u, &a, &b, &e, &d);
		c->e1_ecx = e; c->e1_edx = d;
	}
	if (c->max_ext >= 0x80000004u) {
		unsigned r[12], i;
		for (i = 0; i < 3; i++) __get_cpuid(0x80000002u + i, &r[i*4], &r[i*4+1], &r[i*4+2], &r[i*4+3]);
		memcpy(c->brand, r, 48);
	}
	c->mmx = (c->l1_edx >> 23) & 1;
	c->cmov = (c->l1_edx >> 15) & 1;
	c->sse = (c->l1_edx >> 25) & 1;
	c->mmxext = (c->e1_edx >> 22) & 1;
	c->amd3dnow = (c->e1_edx >> 31) & 1;
	c->amd3dnowext = (c->e1_edx >> 30) & 1;
	c->nt_ok = c->mmx && (c->sse || c->mmxext);
}

/* SIGILL guard for the non-temporal paths. */
static sigjmp_buf g_ill_jmp __attribute__((unused));
static volatile sig_atomic_t g_ill_armed;

/* ------------------------------------------------------------------ scaling kernels */

/* The functions with "cpuid" in their name are the only ones that may contain
 * MMX-extension instructions (movntq, sfence); they are noinline so the
 * symbol of any such instruction is always a dispatch-guarded one. */

__attribute__((noinline, target("mmx")))
static void nt_copy_mmxext_cpuid(void *dst, const void *src, size_t n)
{
	uint8_t *d = dst;
	const uint8_t *s = src;

	while (n && ((uintptr_t)d & 7)) { *d++ = *s++; n--; }
	while (n >= 32) {
		__asm__ volatile(
		    "movq   (%1), %%mm0\n\t"
		    "movq  8(%1), %%mm1\n\t"
		    "movq 16(%1), %%mm2\n\t"
		    "movq 24(%1), %%mm3\n\t"
		    "movntq %%mm0,   (%0)\n\t"
		    "movntq %%mm1,  8(%0)\n\t"
		    "movntq %%mm2, 16(%0)\n\t"
		    "movntq %%mm3, 24(%0)\n\t"
		    : : "r"(d), "r"(s) : "memory", "mm0", "mm1", "mm2", "mm3");
		d += 32; s += 32; n -= 32;
	}
	while (n >= 8) {
		__asm__ volatile("movq (%1), %%mm0\n\tmovntq %%mm0, (%0)\n\t"
				 : : "r"(d), "r"(s) : "memory", "mm0");
		d += 8; s += 8; n -= 8;
	}
	while (n) { *d++ = *s++; n--; }
}

__attribute__((noinline, target("mmx")))
static void nt_fence_sfence_cpuid(void)
{
	__asm__ volatile("sfence" : : : "memory");
	_mm_empty();
}

typedef __m64 __attribute__((aligned(1))) m64u;

/* Expand each pixel of crow (w pixels, B bytes) k times into out.  Every source
 * pixel is broadcast into an MMX register and stored ceil(n/8) times, n=k*B, at
 * 8-byte steps starting at the pixel's output offset; the overshoot of one
 * pixel is overwritten by the next, so out needs 8 spare bytes at the end. */
__attribute__((target("mmx")))
static void expand_mmx(const uint8_t *crow, int w, int B, int k, uint8_t *out)
{
	size_t n = (size_t)k * B;
	int nst = (int)((n + 7) / 8), i, s;
	uint8_t *o = out;

	for (i = 0; i < w; i++, o += n) {
		__m64 v;
		if (B == 2) {
			v = _mm_cvtsi32_si64(((const uint16_t *)crow)[i]);
			v = _mm_unpacklo_pi16(v, v);
			v = _mm_unpacklo_pi32(v, v);
		} else if (B == 4) {
			v = _mm_cvtsi32_si64((int)((const uint32_t *)crow)[i]);
			v = _mm_unpacklo_pi32(v, v);
		} else {
			v = _mm_cvtsi32_si64(crow[i]);
			v = _mm_unpacklo_pi8(v, v);
			v = _mm_unpacklo_pi16(v, v);
			v = _mm_unpacklo_pi32(v, v);
		}
		for (s = 0; s < nst; s++) *(m64u *)(o + s * 8) = v;
	}
	_mm_empty();
}

static void expand_c(const uint8_t *crow, int w, int B, int k, uint8_t *out)
{
	int i, j;

	if (B == 1) {
		for (i = 0; i < w; i++) for (j = 0; j < k; j++) *out++ = crow[i];
	} else if (B == 2) {
		uint16_t *o = (uint16_t *)out;
		for (i = 0; i < w; i++) for (j = 0; j < k; j++) *o++ = ((const uint16_t *)crow)[i];
	} else {
		uint32_t *o = (uint32_t *)out;
		for (i = 0; i < w; i++) for (j = 0; j < k; j++) *o++ = ((const uint32_t *)crow)[i];
	}
}

struct blit {
	int sw, sh, k;             /* source size and integer scale */
	const struct pixtab *t;
	size_t stride;             /* destination line length in bytes */
	uint8_t *crow;             /* converted source row (sw*B) */
	uint8_t *rowbuf;           /* expanded row (sw*k*B + 16 spare) */
};

#define BM_DIRTY 1
#define BM_MMX   2
#define BM_NT    4

static int blit_alloc(struct blit *b, int sw, int sh, int k, const struct pixtab *t, size_t stride)
{
	b->sw = sw; b->sh = sh; b->k = k; b->t = t; b->stride = stride;
	b->crow = malloc((size_t)sw * 4 + 16);
	b->rowbuf = malloc((size_t)sw * k * 4 + 16);
	return b->crow && b->rowbuf ? 0 : -1;
}

static void blit_free(struct blit *b) { free(b->crow); free(b->rowbuf); b->crow = b->rowbuf = NULL; }

/* (a) naive nearest-neighbour, one pixel at a time (two divisions per pixel). */
static void blit_naive(const struct blit *b, const uint16_t *src, uint8_t *dst)
{
	int dw = b->sw * b->k, dh = b->sh * b->k, x, y;
	int B = b->t->B;

	for (y = 0; y < dh; y++) {
		int sy = y * b->sh / dh;
		uint8_t *row = dst + (size_t)y * b->stride;
		for (x = 0; x < dw; x++) {
			int sx = x * b->sw / dw;
			store_px(row + (size_t)x * B, B, conv_px(b->t, src[sy * b->sw + sx]));
		}
	}
}

/* (b) row expanded once, copied for the repeated lines.
 * (c) BM_DIRTY: skip source rows identical to prev (prev may be NULL).
 * (d) BM_MMX:   MMX row expansion.   (e) BM_NT: non-temporal line copies.
 * Returns the number of source rows written. */
static int blit_rows(const struct blit *b, const uint16_t *src, const uint16_t *prev,
		     uint8_t *dst, int mode)
{
	int B = b->t->B, k = b->k, y, j, rows = 0;
	size_t rb = (size_t)b->sw * k * B;

	for (y = 0; y < b->sh; y++) {
		const uint16_t *sr = src + (size_t)y * b->sw;
		if ((mode & BM_DIRTY) && prev &&
		    !memcmp(sr, prev + (size_t)y * b->sw, (size_t)b->sw * 2))
			continue;
		conv_row(b->t, sr, b->sw, b->crow);
		if (mode & BM_MMX) expand_mmx(b->crow, b->sw, B, k, b->rowbuf);
		else expand_c(b->crow, b->sw, B, k, b->rowbuf);
		for (j = 0; j < k; j++) {
			uint8_t *d = dst + (size_t)(y * k + j) * b->stride;
			if (mode & BM_NT) nt_copy_mmxext_cpuid(d, b->rowbuf, rb);
			else memcpy(d, b->rowbuf, rb);
		}
		rows++;
	}
	if (mode & BM_NT) nt_fence_sfence_cpuid();
	return rows;
}

/* ------------------------------------------------------------------ synthetic frames */

static uint16_t pattern_px(int x, int y, int n)
{
	return (uint16_t)(((((x + n * 2) >> 2) & 31) << 11) | ((((y + n) >> 1) & 63) << 5) |
			  (((x ^ y) + n) & 31));
}

#define NFRAMES 32
/* Frame 0 is a full pattern; frame i>0 equals frame i-1 except ~30% of the rows
 * (y*37+i*13)%10 < 3, which are redrawn with the pattern shifted by i. */
static uint16_t **frames_make(int sw, int sh)
{
	uint16_t **f = calloc(NFRAMES, sizeof *f);
	int i, x, y;

	if (!f) return NULL;
	for (i = 0; i < NFRAMES; i++) {
		f[i] = malloc((size_t)sw * sh * 2);
		if (!f[i]) return NULL;
		if (i == 0) {
			for (y = 0; y < sh; y++) for (x = 0; x < sw; x++) f[0][y * sw + x] = pattern_px(x, y, 0);
		} else {
			memcpy(f[i], f[i - 1], (size_t)sw * sh * 2);
			for (y = 0; y < sh; y++)
				if ((y * 37 + i * 13) % 10 < 3)
					for (x = 0; x < sw; x++) f[i][y * sw + x] = pattern_px(x, y, i);
		}
	}
	return f;
}

#ifndef FBBENCH_SELFTEST
/* ================================================================== benchmark */

static FILE *g_log;
static u64 g_t0;
#define BUDGET_S 55         /* no new test starts after this many seconds */
#define HARD_S   62         /* alarm(): restore and exit */

static u64 now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (u64)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}
static u64 cpu_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
	return (u64)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

static void kv(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vfprintf(g_log, fmt, ap);
	va_end(ap);
	fputc('\n', g_log);
	fflush(g_log);
}
static void log_sync(void) { fflush(g_log); fsync(fileno(g_log)); }

static int budget_ok(const char *what)
{
	if ((now_ns() - g_t0) / 1000000000ull < BUDGET_S) return 1;
	kv("skipped.%s=time_budget", what);
	return 0;
}

/* ---- console + mode restore (also used from the signal handler) ---- */

static int g_fd = -1, g_tty = -1;
static struct fb_var_screeninfo g_var0;
static struct fb_fix_screeninfo g_fix0;
static volatile sig_atomic_t g_mode_changed, g_kd_graphics;
static int g_kd_saved = KD_TEXT;

static void restore_mode_raw(void)
{
	if (g_mode_changed && g_fd >= 0) {
		struct fb_var_screeninfo v = g_var0;
		v.activate = FB_ACTIVATE_NOW | FB_ACTIVATE_FORCE;
		if (ioctl(g_fd, FBIOPUT_VSCREENINFO, &v) == 0) g_mode_changed = 0;
	}
}

static void console_text(void)
{
	if (g_kd_graphics && g_tty >= 0) {
		ioctl(g_tty, KDSETMODE, g_kd_saved);
		g_kd_graphics = 0;
	}
}

static void on_signal(int sig)
{
	if (sig == SIGILL && g_ill_armed) siglongjmp(g_ill_jmp, 1);
	restore_mode_raw();
	console_text();
	if (g_log) {
		char m[48];
		int n = snprintf(m, sizeof m, "aborted_by_signal=%d\n", sig);
		if (write(fileno(g_log), m, n) < 0) {}
	}
	_exit(128 + sig);
}

/* Force the text console to repaint: back to KD_TEXT, then bounce through
 * another VT and return. */
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

/* ---- test harness ---- */

static double g_dur = 1.0;         /* seconds for scaled-blit tests */
#define DUR_RAW 1.5

typedef u64 (*frame_fn)(void *ctx, u64 i);   /* returns bytes written to the fb */

static void report(const char *name, u64 frames, u64 bytes, u64 wall, u64 cpu)
{
	double ws = wall / 1e9, cs = cpu / 1e9;
	kv("%s.frames=%llu", name, (unsigned long long)frames);
	kv("%s.wall_ms=%.1f", name, ws * 1e3);
	kv("%s.cpu_ms=%.1f", name, cs * 1e3);
	kv("%s.cpu_pct=%.1f", name, ws > 0 ? cs / ws * 100 : 0.0);
	kv("%s.fps=%.2f", name, ws > 0 ? frames / ws : 0.0);
	kv("%s.mbs=%.2f", name, ws > 0 ? bytes / ws / 1e6 : 0.0);
}

static void run_timed(const char *name, double dur, frame_fn fn, void *ctx)
{
	u64 w0 = now_ns(), c0 = cpu_ns(), end = w0 + (u64)(dur * 1e9), fr = 0, by = 0, w, c;

	do { by += fn(ctx, fr); fr++; } while (now_ns() < end);
	w = now_ns() - w0; c = cpu_ns() - c0;
	report(name, fr, by, w, c);
}

static u64 slurp(const char *path, char *buf, size_t n)
{
	int fd = open(path, O_RDONLY);
	ssize_t r;
	if (fd < 0) { buf[0] = 0; return 0; }
	r = read(fd, buf, n - 1);
	close(fd);
	if (r < 0) r = 0;
	buf[r] = 0;
	while (r > 0 && (buf[r - 1] == '\n' || buf[r - 1] == ' ')) buf[--r] = 0;
	return (u64)r;
}

static void kv_file(const char *key, const char *path)
{
	char b[512];
	slurp(path, b, sizeof b);
	if (b[0]) { char *nl; while ((nl = strchr(b, '\n'))) *nl = ' '; kv("%s=%s", key, b); }
	else kv("%s=unavailable", key);
}

/* ---- section: system / cpu ---- */

static void sec_system(void)
{
	struct utsname u;
	char b[2048], *p;
	kv("fbbench.version=1");
	kv("fbbench.units=mbs is 1e6 bytes/s of framebuffer writes; cpu_pct is process CPU time / wall time");
	if (uname(&u) == 0) { kv("kernel.release=%s", u.release); kv("kernel.machine=%s", u.machine); }
	kv_file("loadavg.start", "/proc/loadavg");
	slurp("/proc/cpuinfo", b, sizeof b);
	if ((p = strstr(b, "cpu MHz"))) { char *e = strchr(p, '\n'); if (e) *e = 0; kv("cpuinfo.%s", p); }
	cpu_detect(&g_cpu);
	kv("cpuid.vendor=%s", g_cpu.vendor);
	kv("cpuid.max_leaf=%u", g_cpu.max_leaf);
	kv("cpuid.l1_eax=0x%08x", g_cpu.l1_eax);
	kv("cpuid.l1_edx=0x%08x", g_cpu.l1_edx);
	kv("cpuid.l1_ecx=0x%08x", g_cpu.l1_ecx);
	kv("cpuid.max_ext=0x%08x", g_cpu.max_ext);
	kv("cpuid.e1_edx=0x%08x", g_cpu.e1_edx);
	kv("cpuid.e1_ecx=0x%08x", g_cpu.e1_ecx);
	kv("cpuid.brand=%s", g_cpu.brand);
	kv("cpuid.mmx=%d", g_cpu.mmx);
	kv("cpuid.cmov=%d", g_cpu.cmov);
	kv("cpuid.sse=%d", g_cpu.sse);
	kv("cpuid.mmxext=%d", g_cpu.mmxext);
	kv("cpuid.3dnow=%d", g_cpu.amd3dnow);
	kv("cpuid.3dnowext=%d", g_cpu.amd3dnowext);
	kv("cpuid.nt_stores_used=%d", g_cpu.nt_ok);
	kv("mmx_path=%d", g_cpu.mmx);
	{ /* is the background inventory competing for the CPU? */
		int n = 0, pid;
		char path[64], comm[64];
		for (pid = 1; pid < 400; pid++) {
			snprintf(path, sizeof path, "/proc/%d/comm", pid);
			slurp(path, comm, sizeof comm);
			if (!strcmp(comm, "olpc-inventory") || !strcmp(comm, "olpc-selftest")) n++;
		}
		kv("competing_olpc_jobs=%d", n);
	}
}

/* ---- section: timer quality ---- */

static int cmp_u64(const void *a, const void *b)
{
	u64 x = *(const u64 *)a, y = *(const u64 *)b;
	return x < y ? -1 : x > y;
}

static void sec_timer(void)
{
	enum { MAXS = 400 };
	static u64 late[MAXS];
	struct timespec next, res;
	const long period = 16666667L;
	int n = 0, i, over2 = 0, over5 = 0;
	u64 end, sum = 0;
	char b[256];

	kv("timer.sysconf_clk_tck=%ld", sysconf(_SC_CLK_TCK));
	if (clock_getres(CLOCK_MONOTONIC, &res) == 0) {
		kv("timer.clock_getres_ns=%ld", res.tv_nsec + res.tv_sec * 1000000000L);
		/* 1 ns = hrtimers active; otherwise the resolution is 1/HZ */
		if (res.tv_sec == 0 && res.tv_nsec >= 1000000L) kv("timer.kernel_hz=%ld", 1000000000L / res.tv_nsec);
		else kv("timer.kernel_hz=unknown_highres_active (CONFIG_HZ=100 per kernel/config)");
	}
	kv_file("timer.clocksource", "/sys/devices/system/clocksource/clocksource0/current_clocksource");
	kv_file("timer.clocksource_available", "/sys/devices/system/clocksource/clocksource0/available_clocksource");
	slurp("/proc/timer_list", b, sizeof b);
	{
		char *p = strstr(b, "hres_active");
		if (p) { char *e = strchr(p, '\n'); if (e) *e = 0; kv("timer.%s", p); }
	}
	clock_gettime(CLOCK_MONOTONIC, &next);
	end = now_ns() + 2000000000ull;
	while (now_ns() < end && n < MAXS) {
		struct timespec t;
		u64 target, got;
		next.tv_nsec += period;
		if (next.tv_nsec >= 1000000000L) { next.tv_nsec -= 1000000000L; next.tv_sec++; }
		target = (u64)next.tv_sec * 1000000000ull + next.tv_nsec;
		while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL) == EINTR) {}
		clock_gettime(CLOCK_MONOTONIC, &t);
		got = (u64)t.tv_sec * 1000000000ull + t.tv_nsec;
		late[n++] = got > target ? got - target : 0;
	}
	for (i = 0; i < n; i++) {
		sum += late[i];
		if (late[i] > 2000000) over2++;
		if (late[i] > 5000000) over5++;
	}
	qsort(late, n, sizeof late[0], cmp_u64);
	kv("timer.period_us=%ld", period / 1000);
	kv("timer.samples=%d", n);
	if (n) {
		kv("timer.late_min_us=%.1f", late[0] / 1e3);
		kv("timer.late_avg_us=%.1f", sum / 1e3 / n);
		kv("timer.late_p50_us=%.1f", late[n / 2] / 1e3);
		kv("timer.late_p95_us=%.1f", late[n * 95 / 100] / 1e3);
		kv("timer.late_max_us=%.1f", late[n - 1] / 1e3);
		kv("timer.late_over_2ms=%d", over2);
		kv("timer.late_over_5ms=%d", over5);
	}
	log_sync();
}

/* ---- section: framebuffer ---- */

struct fbi {
	struct fb_var_screeninfo v;
	struct fb_fix_screeninfo f;
	uint8_t *map;
	size_t maplen;
	struct pixtab t;
};

static void fb_tab(struct fbi *fb)
{
	pixtab_init(&fb->t, fb->v.bits_per_pixel, fb->v.red.offset, fb->v.red.length,
		    fb->v.green.offset, fb->v.green.length, fb->v.blue.offset, fb->v.blue.length);
}

static void fb_facts(const char *pfx, const struct fbi *fb)
{
	const struct fb_var_screeninfo *v = &fb->v;
	const struct fb_fix_screeninfo *f = &fb->f;

	kv("%s.id=%.16s", pfx, f->id);
	kv("%s.xres=%u", pfx, v->xres);
	kv("%s.yres=%u", pfx, v->yres);
	kv("%s.xres_virtual=%u", pfx, v->xres_virtual);
	kv("%s.yres_virtual=%u", pfx, v->yres_virtual);
	kv("%s.xoffset=%u", pfx, v->xoffset);
	kv("%s.yoffset=%u", pfx, v->yoffset);
	kv("%s.bpp=%u", pfx, v->bits_per_pixel);
	kv("%s.grayscale=%u", pfx, v->grayscale);
	kv("%s.red=%u/%u", pfx, v->red.offset, v->red.length);
	kv("%s.green=%u/%u", pfx, v->green.offset, v->green.length);
	kv("%s.blue=%u/%u", pfx, v->blue.offset, v->blue.length);
	kv("%s.transp=%u/%u", pfx, v->transp.offset, v->transp.length);
	kv("%s.line_length=%u", pfx, f->line_length);
	kv("%s.smem_len=%u", pfx, f->smem_len);
	kv("%s.smem_start=0x%lx", pfx, f->smem_start);
	kv("%s.type=%u visual=%u", pfx, f->type, f->visual);
	kv("%s.accel_flags=%u", pfx, v->accel_flags);
	kv("%s.fix_accel=%u", pfx, f->accel);
	kv("%s.capabilities=0x%x", pfx, f->capabilities);
	kv("%s.xpanstep=%u ypanstep=%u ywrapstep=%u", pfx, f->xpanstep, f->ypanstep, f->ywrapstep);
	kv("%s.pixclock_ps=%u", pfx, v->pixclock);
	kv("%s.vmode=0x%x nonstd=%u", pfx, v->vmode, v->nonstd);
	kv("%s.plane_bytes=%u", pfx, f->line_length * v->yres);
	kv("%s.second_page_fits=%d", pfx,
	   (u64)f->line_length * v->yres * 2 <= f->smem_len);
	kv("%s.pages_in_smem=%u", pfx, v->yres ? f->smem_len / (f->line_length * v->yres) : 0);
}

/* FBIO_WAITFORVSYNC probe in a child process (its own alarm), so a driver
 * that never signals vsync cannot stall the benchmark. */
static void fb_vsync_probe(int fd)
{
	int pp[2];
	pid_t pid;
	char buf[128];
	ssize_t r;
	int st;

	fflush(g_log);
	if (pipe(pp)) { kv("fb.vsync=probe_failed(pipe)"); return; }
	pid = fork();
	if (pid < 0) { kv("fb.vsync=probe_failed(fork)"); close(pp[0]); close(pp[1]); return; }
	if (pid == 0) {
		uint32_t arg = 0;
		u64 t0;
		int i, n = 0;
		char m[128];
		signal(SIGALRM, SIG_DFL);
		alarm(2);
		close(pp[0]);
		if (ioctl(fd, FBIO_WAITFORVSYNC, &arg) < 0) {
			snprintf(m, sizeof m, "unsupported errno=%d(%s)", errno, strerror(errno));
		} else {
			t0 = now_ns();
			for (i = 0; i < 20; i++) { arg = 0; if (ioctl(fd, FBIO_WAITFORVSYNC, &arg) < 0) break; n++; }
			snprintf(m, sizeof m, "ok n=%d hz=%.2f", n, n ? n * 1e9 / (now_ns() - t0) : 0.0);
		}
		if (write(pp[1], m, strlen(m)) < 0) {}
		_exit(0);
	}
	close(pp[1]);
	r = read(pp[0], buf, sizeof buf - 1);
	close(pp[0]);
	waitpid(pid, &st, 0);
	if (r > 0) { buf[r] = 0; kv("fb.waitforvsync=%s", buf); }
	else kv("fb.waitforvsync=hangs_or_failed(child killed by alarm after 2 s)");
}

/* raw bandwidth callbacks */
struct rawctx { uint8_t *fb; uint8_t *ram1, *ram2; size_t n; };

static u64 raw_fill(void *c, u64 i)
{
	struct rawctx *r = c;
	uint32_t *p = (uint32_t *)r->fb, v = 0x0f0f0f0fu ^ (uint32_t)(i * 0x01010101u);
	size_t k, w = r->n / 4;
	for (k = 0; k < w; k++) p[k] = v + (uint32_t)k;
	return r->n;
}
static u64 raw_memcpy(void *c, u64 i) { struct rawctx *r = c; (void)i; memcpy(r->fb, r->ram1, r->n); return r->n; }
static u64 raw_nt(void *c, u64 i)
{
	struct rawctx *r = c;
	(void)i;
	nt_copy_mmxext_cpuid(r->fb, r->ram1, r->n);
	nt_fence_sfence_cpuid();
	return r->n;
}

static void sec_raw(struct fbi *fb)
{
	struct rawctx r;
	size_t n = (size_t)fb->f.line_length * fb->v.yres;

	r.fb = fb->map; r.n = n;
	r.ram1 = malloc(n + 64); r.ram2 = malloc(n + 64);
	if (!r.ram1 || !r.ram2) { kv("raw=nomem"); return; }
	memset(r.ram1, 0x5a, n); memset(r.ram2, 0, n);
	kv("raw.bytes=%zu", n);
	run_timed("raw.fb_fill", DUR_RAW, raw_fill, &r);
	run_timed("raw.fb_memcpy", DUR_RAW, raw_memcpy, &r);
	{
		/* RAM->RAM copy of the same size, for comparison */
		u64 w0 = now_ns(), c0 = cpu_ns(), fr = 0;
		do { memcpy(r.ram2, r.ram1, n); fr++; } while (now_ns() - w0 < (u64)(DUR_RAW * 1e9));
		report("raw.ram_memcpy", fr, fr * n, now_ns() - w0, cpu_ns() - c0);
	}
	{
		u64 w0 = now_ns(), c0 = cpu_ns(), fr = 0;
		do { memcpy(r.ram2, r.fb, n); fr++; } while (now_ns() - w0 < (u64)(DUR_RAW * 1e9));
		report("raw.fb_read_memcpy", fr, fr * n, now_ns() - w0, cpu_ns() - c0);
	}
	if (g_cpu.nt_ok) {
		if (sigsetjmp(g_ill_jmp, 1) == 0) {
			g_ill_armed = 1;
			run_timed("raw.fb_memcpy_movntq", DUR_RAW, raw_nt, &r);
		} else kv("raw.fb_memcpy_movntq=SIGILL");
		g_ill_armed = 0;
	} else kv("raw.fb_memcpy_movntq=skipped_cpuid");
	free(r.ram1); free(r.ram2);
	log_sync();
}

struct bctx {
	struct blit b;
	uint16_t **fr;
	uint8_t *dst;
	int mode, naive;
	u64 rows, dbytes;     /* source rows written, bytes per source row */
};

static u64 bt_frame(void *c, u64 i)
{
	struct bctx *x = c;
	const uint16_t *cur = x->fr[i % NFRAMES], *prev = x->fr[(i + NFRAMES - 1) % NFRAMES];
	if (x->naive) {
		blit_naive(&x->b, cur, x->dst);
		x->rows += x->b.sh;
	} else {
		x->rows += blit_rows(&x->b, cur, prev, x->dst, x->mode);
	}
	return 0;       /* bytes derived from rows afterwards */
}

static void sec_blit_one(const char *pfx, struct fbi *fb, uint16_t **fr, int sw, int sh, int k)
{
	static const struct { const char *name; int mode, naive, needs_nt; } var[] = {
		{ "naive", 0, 1, 0 },
		{ "rowcopy", 0, 0, 0 },
		{ "dirty", BM_DIRTY, 0, 0 },
		{ "mmx", BM_MMX, 0, 0 },
		{ "mmx_movntq", BM_MMX | BM_NT, 0, 1 },
	};
	int B = fb->t.B, dw = sw * k, dh = sh * k;
	volatile int vi;
	size_t ox = (fb->v.xres - dw) / 2, oy = (fb->v.yres - dh) / 2;
	struct bctx x;

	if (dw > (int)fb->v.xres || dh > (int)fb->v.yres) { kv("%s.skipped=too_big", pfx); return; }
	memset(&x, 0, sizeof x);
	if (blit_alloc(&x.b, sw, sh, k, &fb->t, fb->f.line_length)) { kv("%s.skipped=nomem", pfx); return; }
	x.fr = fr;
	x.dst = fb->map + oy * fb->f.line_length + ox * B;
	kv("%s.src=%dx%d scale=%d dst=%dx%d at %zu,%zu bpp=%d", pfx, sw, sh, k, dw, dh, ox, oy, B * 8);
	for (vi = 0; vi < NELEM(var); vi++) {
		char name[96];
		u64 w0, c0, w, c;
		volatile u64 fr_n = 0;
		if (!budget_ok(pfx)) break;
		snprintf(name, sizeof name, "%s.%s", pfx, var[vi].name);
		if (var[vi].needs_nt && !g_cpu.nt_ok) { kv("%s=skipped_cpuid", name); continue; }
		x.mode = var[vi].mode; x.naive = var[vi].naive; x.rows = 0;
		/* put frame NFRAMES-1 on screen so the dirty-row test starts coherent */
		blit_rows(&x.b, fr[NFRAMES - 1], NULL, x.dst, 0);
		if (sigsetjmp(g_ill_jmp, 1) != 0) { g_ill_armed = 0; kv("%s=SIGILL", name); continue; }
		g_ill_armed = var[vi].needs_nt;
		w0 = now_ns(); c0 = cpu_ns();
		do { bt_frame(&x, fr_n); fr_n++; } while (now_ns() - w0 < (u64)(g_dur * 1e9));
		w = now_ns() - w0; c = cpu_ns() - c0;
		g_ill_armed = 0;
		report(name, fr_n, x.rows * (u64)k * dw * B, w, c);
		if (x.mode & BM_DIRTY)
			kv("%s.rows_written_pct=%.1f", name, 100.0 * x.rows / ((double)fr_n * sh));
	}
	blit_free(&x.b);
	log_sync();
}

static void sec_blits(const char *pfx, struct fbi *fb, int only_hi)
{
	static uint16_t **gb, **nes;
	char p[64];

	if (!gb) gb = frames_make(160, 144);
	if (!nes) nes = frames_make(256, 240);
	if (!gb || !nes) { kv("%s.skipped=nomem", pfx); return; }
	if (fb->v.bits_per_pixel != 8 && fb->v.bits_per_pixel != 16 && fb->v.bits_per_pixel != 32) {
		kv("%s.skipped=unsupported_bpp_%u", pfx, fb->v.bits_per_pixel);
		return;
	}
	if (!only_hi) {
		snprintf(p, sizeof p, "%s.gb4", pfx); sec_blit_one(p, fb, gb, 160, 144, 4);
		snprintf(p, sizeof p, "%s.gb5", pfx); sec_blit_one(p, fb, gb, 160, 144, 5);
	}
	snprintf(p, sizeof p, "%s.gb6", pfx); sec_blit_one(p, fb, gb, 160, 144, 6);
	if (!only_hi) {
		snprintf(p, sizeof p, "%s.nes2", pfx); sec_blit_one(p, fb, nes, 256, 240, 2);
	}
	snprintf(p, sizeof p, "%s.nes3", pfx); sec_blit_one(p, fb, nes, 256, 240, 3);
}

static int var_same(const struct fb_var_screeninfo *a, const struct fb_var_screeninfo *b)
{
	return a->xres == b->xres && a->yres == b->yres && a->xres_virtual == b->xres_virtual &&
	       a->yres_virtual == b->yres_virtual && a->xoffset == b->xoffset &&
	       a->yoffset == b->yoffset && a->bits_per_pixel == b->bits_per_pixel &&
	       a->grayscale == b->grayscale && a->pixclock == b->pixclock &&
	       !memcmp(&a->red, &b->red, sizeof a->red) && !memcmp(&a->green, &b->green, sizeof a->green) &&
	       !memcmp(&a->blue, &b->blue, sizeof a->blue) && !memcmp(&a->transp, &b->transp, sizeof a->transp);
}

static void sec_8bpp(int fd, struct fbi *fb)
{
	struct fb_var_screeninfo nv = fb->v;
	struct fbi f8 = *fb;
	uint16_t grey[256];
	struct fb_cmap cm;
	int i;

	if (!budget_ok("bpp8")) return;
	nv.bits_per_pixel = 8;
	nv.red.offset = nv.green.offset = nv.blue.offset = 0;
	nv.red.length = nv.green.length = nv.blue.length = 8;
	nv.transp.offset = nv.transp.length = 0;
	nv.grayscale = 0;
	nv.activate = FB_ACTIVATE_NOW | FB_ACTIVATE_FORCE;
	g_mode_changed = 1;            /* from here a restore is mandatory */
	if (ioctl(fd, FBIOPUT_VSCREENINFO, &nv) < 0) {
		kv("bpp8.switch=failed errno=%d(%s)", errno, strerror(errno));
		goto restore;
	}
	if (ioctl(fd, FBIOGET_VSCREENINFO, &f8.v) < 0 || ioctl(fd, FBIOGET_FSCREENINFO, &f8.f) < 0 ||
	    f8.v.bits_per_pixel != 8) {
		kv("bpp8.switch=not_applied (bpp now %u)", f8.v.bits_per_pixel);
		goto restore;
	}
	kv("bpp8.switch=ok");
	fb_facts("bpp8.fb", &f8);
	for (i = 0; i < 256; i++) grey[i] = (uint16_t)(i * 257);
	memset(&cm, 0, sizeof cm);
	cm.start = 0; cm.len = 256; cm.red = cm.green = cm.blue = grey;
	if (ioctl(fd, FBIOPUTCMAP, &cm) < 0) kv("bpp8.putcmap=failed errno=%d(%s)", errno, strerror(errno));
	else kv("bpp8.putcmap=ok");
	if (f8.f.smem_len < (u64)f8.f.line_length * f8.v.yres) { kv("bpp8.skipped=smem_too_small"); goto restore; }
	fb_tab(&f8);
	f8.map = fb->map; f8.maplen = fb->maplen;
	memset(f8.map, 0, (size_t)f8.f.line_length * f8.v.yres);
	sec_blits("bpp8", &f8, 1);
restore:
	memset(fb->map, 0, (size_t)fb->f.line_length * fb->v.yres);   /* black while switching back */
	restore_mode_raw();
	{
		struct fb_var_screeninfo cur;
		struct fb_fix_screeninfo cf;
		if (ioctl(fd, FBIOGET_VSCREENINFO, &cur) == 0 && ioctl(fd, FBIOGET_FSCREENINFO, &cf) == 0) {
			int ok = var_same(&cur, &g_var0) && cf.line_length == g_fix0.line_length;
			kv("bpp8.restore=%s", ok ? "ok" : "MISMATCH");
			if (!ok) fb_facts("bpp8.restore_now", &(struct fbi){ .v = cur, .f = cf });
		} else kv("bpp8.restore=cannot_verify");
		if (g_mode_changed) kv("bpp8.restore=FAILED");
	}
	log_sync();
}

static void sec_fb(void)
{
	struct fbi fb;
	int fd = open("/dev/fb0", O_RDWR);
	char b[1024];

	if (fd < 0) { kv("fb=missing errno=%d(%s)", errno, strerror(errno)); return; }
	memset(&fb, 0, sizeof fb);
	if (ioctl(fd, FBIOGET_VSCREENINFO, &fb.v) < 0 || ioctl(fd, FBIOGET_FSCREENINFO, &fb.f) < 0) {
		kv("fb=missing (ioctl failed errno=%d)", errno);
		close(fd);
		return;
	}
	kv("fb=present");
	g_fd = fd; g_var0 = fb.v; g_fix0 = fb.f;
	fb_facts("fb", &fb);
	kv_file("fb.sysfs_name", "/sys/class/graphics/fb0/name");
	kv_file("fb.sysfs_stride", "/sys/class/graphics/fb0/stride");
	kv_file("fb.sysfs_virtual_size", "/sys/class/graphics/fb0/virtual_size");
	slurp("/sys/class/graphics/fb0/modes", b, sizeof b);
	{
		char *p = b, *nl;
		int n = 0;
		while (*p) {
			nl = strchr(p, '\n');
			if (nl) *nl = 0;
			if (*p) kv("fb.mode%d=%s", n++, p);
			if (!nl) break;
			p = nl + 1;
		}
		kv("fb.modes_listed=%d", n);
	}
	{
		struct fb_var_screeninfo pv = fb.v;
		pv.activate = FB_ACTIVATE_NOW;
		if (ioctl(fd, FBIOPAN_DISPLAY, &pv) == 0) kv("fb.pan_display=ok");
		else kv("fb.pan_display=fails errno=%d(%s)", errno, strerror(errno));
	}
	fb_vsync_probe(fd);
	fb.maplen = fb.f.smem_len;
	fb.map = mmap(NULL, fb.maplen, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (fb.map == MAP_FAILED) { kv("fb.mmap=failed errno=%d(%s)", errno, strerror(errno)); close(fd); g_fd = -1; return; }
	kv("fb.mmap=ok");
	if (fb.f.smem_len < (u64)fb.f.line_length * fb.v.yres) { kv("fb.tests=skipped smem_len<plane"); goto out; }
	fb_tab(&fb);
	kv("fb.pixfmt_is_rgb565=%d", fb.t.ident16);

	g_tty = open("/dev/tty0", O_RDWR | O_NOCTTY);
	if (g_tty < 0) g_tty = open("/dev/tty1", O_RDWR | O_NOCTTY);
	if (g_tty >= 0) {
		if (ioctl(g_tty, KDGETMODE, &g_kd_saved) < 0) g_kd_saved = KD_TEXT;
		if (ioctl(g_tty, KDSETMODE, KD_GRAPHICS) == 0) g_kd_graphics = 1;
	}
	kv("console.kd_graphics=%d", g_kd_graphics);

	if (budget_ok("raw")) sec_raw(&fb);
	sec_blits("blit", &fb, 0);
	if (fb.v.bits_per_pixel == 16) sec_8bpp(fd, &fb);
	else kv("bpp8.skipped=fb_is_%ubpp_not_16", fb.v.bits_per_pixel);

	memset(fb.map, 0, (size_t)fb.f.line_length * fb.v.yres);
	console_redraw();
out:
	munmap(fb.map, fb.maplen);
	close(fd);
	g_fd = -1;
	if (g_tty >= 0) { close(g_tty); g_tty = -1; }
	log_sync();
}

/* ---- section: audio via the raw ALSA ioctl interface ---- */
#include <sound/asound.h>

static struct snd_mask *hw_mask(struct snd_pcm_hw_params *p, int i)
{ return &p->masks[i - SNDRV_PCM_HW_PARAM_FIRST_MASK]; }
static struct snd_interval *hw_iv(struct snd_pcm_hw_params *p, int i)
{ return &p->intervals[i - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL]; }

static void hw_any(struct snd_pcm_hw_params *p)
{
	int i;
	memset(p, 0, sizeof *p);
	for (i = 0; i < NELEM(p->masks); i++) memset(p->masks[i].bits, 0xff, sizeof p->masks[i].bits);
	for (i = 0; i < NELEM(p->intervals); i++) { p->intervals[i].min = 0; p->intervals[i].max = UINT_MAX; }
	p->rmask = ~0u;
	p->info = ~0u;
}
static void hw_set_mask(struct snd_pcm_hw_params *p, int i, unsigned bit)
{
	struct snd_mask *m = hw_mask(p, i);
	memset(m->bits, 0, sizeof m->bits);
	m->bits[bit / 32] = 1u << (bit % 32);
}
static void hw_set_iv(struct snd_pcm_hw_params *p, int i, unsigned lo, unsigned hi)
{
	struct snd_interval *v = hw_iv(p, i);
	v->min = lo; v->max = hi; v->openmin = v->openmax = 0; v->integer = 1; v->empty = 0;
}
static void hw_log(const char *pfx, struct snd_pcm_hw_params *p)
{
	static const struct { const char *n; int id; } iv[] = {
		{ "channels", SNDRV_PCM_HW_PARAM_CHANNELS }, { "rate", SNDRV_PCM_HW_PARAM_RATE },
		{ "period_size", SNDRV_PCM_HW_PARAM_PERIOD_SIZE }, { "periods", SNDRV_PCM_HW_PARAM_PERIODS },
		{ "buffer_size", SNDRV_PCM_HW_PARAM_BUFFER_SIZE }, { "period_bytes", SNDRV_PCM_HW_PARAM_PERIOD_BYTES },
		{ "period_time_us", SNDRV_PCM_HW_PARAM_PERIOD_TIME }, { "buffer_time_us", SNDRV_PCM_HW_PARAM_BUFFER_TIME },
	};
	int i;
	for (i = 0; i < NELEM(iv); i++) {
		struct snd_interval *v = hw_iv(p, iv[i].id);
		kv("%s.%s_min=%u", pfx, iv[i].n, v->min);
		kv("%s.%s_max=%u", pfx, iv[i].n, v->max);
	}
	kv("%s.format_mask=0x%08x%08x", pfx, hw_mask(p, SNDRV_PCM_HW_PARAM_FORMAT)->bits[1],
	   hw_mask(p, SNDRV_PCM_HW_PARAM_FORMAT)->bits[0]);
	kv("%s.access_mask=0x%08x", pfx, hw_mask(p, SNDRV_PCM_HW_PARAM_ACCESS)->bits[0]);
}

static unsigned clampu(unsigned v, unsigned lo, unsigned hi) { return v < lo ? lo : v > hi ? hi : v; }

static void audio_rate(int fd, unsigned rate)
{
	char pfx[48];
	struct snd_pcm_hw_params p, q;
	struct snd_pcm_sw_params sw;
	struct snd_xferi x;
	unsigned period, periods, bufsz;
	int16_t *silence;
	u64 w0, c0, left, under = 0, wr = 0, werr = 0;

	snprintf(pfx, sizeof pfx, "audio.r%u", rate);
	hw_any(&p);
	hw_set_mask(&p, SNDRV_PCM_HW_PARAM_ACCESS, SNDRV_PCM_ACCESS_RW_INTERLEAVED);
	hw_set_mask(&p, SNDRV_PCM_HW_PARAM_FORMAT, SNDRV_PCM_FORMAT_S16_LE);
	hw_set_mask(&p, SNDRV_PCM_HW_PARAM_SUBFORMAT, SNDRV_PCM_SUBFORMAT_STD);
	hw_set_iv(&p, SNDRV_PCM_HW_PARAM_CHANNELS, 2, 2);
	hw_set_iv(&p, SNDRV_PCM_HW_PARAM_RATE, rate, rate);
	if (ioctl(fd, SNDRV_PCM_IOCTL_HW_REFINE, &p) < 0) {
		kv("%s.accepted=0 errno=%d(%s)", pfx, errno, strerror(errno));
		return;
	}
	kv("%s.accepted=1", pfx);
	hw_log(pfx, &p);
	/* pick ~1024-frame periods, 4 per buffer when the hardware allows */
	q = p;
	period = clampu(1024, hw_iv(&p, SNDRV_PCM_HW_PARAM_PERIOD_SIZE)->min, hw_iv(&p, SNDRV_PCM_HW_PARAM_PERIOD_SIZE)->max);
	periods = clampu(4, hw_iv(&p, SNDRV_PCM_HW_PARAM_PERIODS)->min, hw_iv(&p, SNDRV_PCM_HW_PARAM_PERIODS)->max);
	hw_set_iv(&q, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, period, period);
	hw_set_iv(&q, SNDRV_PCM_HW_PARAM_PERIODS, periods, periods);
	q.rmask = ~0u; q.cmask = 0;
	if (ioctl(fd, SNDRV_PCM_IOCTL_HW_REFINE, &q) < 0) q = p;   /* fall back to the kernel's choice */
	q.rmask = ~0u; q.cmask = 0;
	if (ioctl(fd, SNDRV_PCM_IOCTL_HW_PARAMS, &q) < 0) {
		kv("%s.hw_params=failed errno=%d(%s)", pfx, errno, strerror(errno));
		return;
	}
	period = hw_iv(&q, SNDRV_PCM_HW_PARAM_PERIOD_SIZE)->min;
	bufsz = hw_iv(&q, SNDRV_PCM_HW_PARAM_BUFFER_SIZE)->min;
	kv("%s.chosen_period_size=%u", pfx, period);
	kv("%s.chosen_buffer_size=%u", pfx, bufsz);
	memset(&sw, 0, sizeof sw);
	sw.proto = SNDRV_PCM_VERSION;
	sw.period_step = 1;
	sw.avail_min = period;
	sw.xfer_align = 1;
	sw.start_threshold = bufsz;
	sw.stop_threshold = bufsz;
	if (ioctl(fd, SNDRV_PCM_IOCTL_SW_PARAMS, &sw) < 0) {
		kv("%s.sw_params=failed errno=%d(%s)", pfx, errno, strerror(errno));
		goto free_hw;
	}
	if (ioctl(fd, SNDRV_PCM_IOCTL_PREPARE) < 0) { kv("%s.prepare=failed errno=%d", pfx, errno); goto free_hw; }
	silence = calloc(period, 4);
	if (!silence) goto free_hw;
	left = rate;
	w0 = now_ns(); c0 = cpu_ns();
	while (left && now_ns() - w0 < 4000000000ull) {
		u64 n = left < period ? left : period;
		x.result = 0; x.buf = silence; x.frames = (snd_pcm_uframes_t)n;
		if (ioctl(fd, SNDRV_PCM_IOCTL_WRITEI_FRAMES, &x) < 0) {
			if (errno == EINTR) continue;
			if (errno == EPIPE) { under++; ioctl(fd, SNDRV_PCM_IOCTL_PREPARE); continue; }
			werr = errno; break;
		}
		left -= x.result; wr += x.result;
	}
	ioctl(fd, SNDRV_PCM_IOCTL_DRAIN);
	kv("%s.play_frames=%llu", pfx, (unsigned long long)wr);
	kv("%s.play_wall_ms=%.1f", pfx, (now_ns() - w0) / 1e6);
	kv("%s.play_cpu_ms=%.1f", pfx, (cpu_ns() - c0) / 1e6);
	kv("%s.underruns=%llu", pfx, (unsigned long long)under);
	if (werr) kv("%s.write_error=%d(%s)", pfx, (int)werr, strerror((int)werr));
	free(silence);
free_hw:
	ioctl(fd, SNDRV_PCM_IOCTL_HW_FREE);
	log_sync();
}

static void sec_audio(void)
{
	const char *paths[] = { "/dev/snd/pcmC0D0p", "/dev/snd/pcmC1D0p" };
	struct snd_pcm_hw_params p;
	int fd = -1, i;

	kv("audio.api=raw_alsa_ioctl (no alsa-lib: static binary, nothing to add to rootfs/packages)");
	kv_file("audio.proc_cards", "/proc/asound/cards");
	for (i = 0; i < NELEM(paths) && fd < 0; i++) {
		fd = open(paths[i], O_RDWR | O_NONBLOCK);
		if (fd >= 0) kv("audio.device=%s", paths[i]);
	}
	if (fd < 0) { kv("audio=missing errno=%d(%s)", errno, strerror(errno)); return; }
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);
	kv("audio=present");
	hw_any(&p);
	if (ioctl(fd, SNDRV_PCM_IOCTL_HW_REFINE, &p) == 0) hw_log("audio.any", &p);
	else kv("audio.any=refine_failed errno=%d", errno);
	if (budget_ok("audio")) audio_rate(fd, 44100);
	if (budget_ok("audio")) audio_rate(fd, 48000);
	close(fd);
}

/* ---- main ---- */

int main(int argc, char **argv)
{
	const char *out = "/var/log/olpc-current/fbbench.log";
	struct sigaction sa;
	int i, sigs[] = { SIGTERM, SIGINT, SIGHUP, SIGALRM, SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGPIPE };

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-o") && i + 1 < argc) out = argv[++i];
		else if (!strcmp(argv[i], "-d") && i + 1 < argc) g_dur = atof(argv[++i]);
		else { fprintf(stderr, "usage: olpc-fbbench [-o logfile] [-d secs_per_blit_test]\n"); return 2; }
	}
	g_log = fopen(out, "a");
	if (!g_log) { g_log = stderr; }
	g_t0 = now_ns();
	memset(&sa, 0, sizeof sa);
	sa.sa_handler = on_signal;
	sigemptyset(&sa.sa_mask);
	for (i = 0; i < NELEM(sigs); i++) sigaction(sigs[i], &sa, NULL);
	alarm(HARD_S);

	kv("fbbench.begin=1");
	sec_system();
	sec_timer();
	sec_fb();
	sec_audio();
	kv("total_s=%.1f", (now_ns() - g_t0) / 1e9);
	kv_file("loadavg.end", "/proc/loadavg");
	kv("fbbench.end=1");
	log_sync();
	return 0;
}

#else /* FBBENCH_SELFTEST ========================================================= */

/* Host-side self-test of the scaling kernels: every variant must produce the
 * same destination bytes as the naive one, for random input, all pixel formats,
 * odd sizes, an unaligned destination offset and a destination stride larger
 * than the row; the destination outside the blitted rectangle must stay intact. */

static uint32_t rs = 12345;
static uint32_t rnd(void) { rs = rs * 1664525u + 1013904223u; return rs >> 8; }

static int g_fail, g_checks;

static void check_case(const char *fmtname, const struct pixtab *t, int sw, int sh, int k,
		       size_t pad, size_t xoff, int mmxext)
{
	struct blit b;
	int B = t->B, dw = sw * k, dh = sh * k, y, x;
	size_t stride = (size_t)dw * B + pad + xoff * B, total = stride * (dh + 2);
	uint8_t *ref = malloc(total), *got = malloc(total), *init = malloc(total);
	uint16_t *src = malloc((size_t)sw * sh * 2), *prev = malloc((size_t)sw * sh * 2);
	uint8_t *guard;
	size_t off = stride + xoff * B;
	int mode, modes[8], nm = 0;

	for (y = 0; y < (int)total; y++) init[y] = (uint8_t)rnd();
	for (y = 0; y < sw * sh; y++) src[y] = (uint16_t)rnd();
	memcpy(prev, src, (size_t)sw * sh * 2);
	for (y = 0; y < sh; y++)
		if (rnd() % 10 < 3) for (x = 0; x < sw; x++) prev[y * sw + x] = (uint16_t)rnd();
	blit_alloc(&b, sw, sh, k, t, stride);
	guard = b.rowbuf + (size_t)dw * B;
	memset(guard, 0xee, 16);

	memcpy(ref, init, total);
	blit_naive(&b, src, ref + off);

	modes[nm++] = 0; modes[nm++] = BM_DIRTY; modes[nm++] = BM_MMX; modes[nm++] = BM_DIRTY | BM_MMX;
	if (mmxext) { modes[nm++] = BM_MMX | BM_NT; modes[nm++] = BM_NT; }
	for (mode = 0; mode < nm; mode++) {
		int m = modes[mode], bad = 0;
		memcpy(got, init, total);
		if (m & BM_DIRTY) {
			/* screen shows prev (rendered naively); only changed rows may be redrawn */
			blit_naive(&b, prev, got + off);
			blit_rows(&b, src, prev, got + off, m);
		} else {
			blit_rows(&b, src, NULL, got + off, m);
		}
		g_checks++;
		if (memcmp(ref, got, total)) bad = 1;
		/* the MMX overshoot must stay inside rowbuf's 8 spare bytes */
		if (m & BM_MMX) for (y = 8; y < 16; y++) if (guard[y] != 0xee) bad = 2;
		memset(guard, 0xee, 16);
		if (bad) {
			g_fail++;
			printf("FAIL %s %dx%d k=%d pad=%zu xoff=%zu mode=%d (%s)\n", fmtname, sw, sh, k, pad,
			       xoff, m, bad == 2 ? "rowbuf overshoot" : "output differs");
		}
	}
	blit_free(&b);
	free(ref); free(got); free(init); free(src); free(prev);
}

int main(void)
{
	static const struct { const char *n; int bpp, ro, rl, go, gl, bo, bl; } fm[] = {
		{ "rgb565", 16, 11, 5, 5, 6, 0, 5 },
		{ "bgr565", 16, 0, 5, 5, 6, 11, 5 },
		{ "xrgb8888", 32, 16, 8, 8, 8, 0, 8 },
		{ "xbgr8888", 32, 0, 8, 8, 8, 16, 8 },
		{ "grey8", 8, 0, 8, 0, 8, 0, 8 },
	};
	static const int sz[][3] = { {160,144,4}, {160,144,5}, {160,144,6}, {256,240,2}, {256,240,3},
				     {1,1,1}, {1,1,7}, {3,2,3}, {7,5,5}, {13,3,6}, {2,2,2}, {17,9,4} };
	int f, s;
	struct pixtab t;

	cpu_detect(&g_cpu);
	printf("cpu vendor=%s mmx=%d sse=%d mmxext=%d 3dnow=%d -> mmx path tested: yes, movntq path tested: %s\n",
	       g_cpu.vendor, g_cpu.mmx, g_cpu.sse, g_cpu.mmxext, g_cpu.amd3dnow, g_cpu.nt_ok ? "yes" : "no (cpu lacks it)");
	if (!g_cpu.mmx) { printf("SKIP: cpu has no MMX\n"); return 0; }
	signal(SIGILL, SIG_DFL);
	for (f = 0; f < NELEM(fm); f++) {
		pixtab_init(&t, fm[f].bpp, fm[f].ro, fm[f].rl, fm[f].go, fm[f].gl, fm[f].bo, fm[f].bl);
		for (s = 0; s < NELEM(sz); s++) {
			check_case(fm[f].n, &t, sz[s][0], sz[s][1], sz[s][2], 0, 0, g_cpu.nt_ok);
			check_case(fm[f].n, &t, sz[s][0], sz[s][1], sz[s][2], 24, 3, g_cpu.nt_ok);
		}
	}
	/* frame generator: roughly 30% of rows change between consecutive frames */
	{
		uint16_t **fr = frames_make(160, 144);
		int i, y, ch = 0, tot = 0;
		for (i = 1; i < NFRAMES; i++)
			for (y = 0; y < 144; y++) { tot++; if (memcmp(fr[i] + y*160, fr[i-1] + y*160, 320)) ch++; }
		printf("frame generator: %.1f%% of rows change per frame (target ~30)\n", 100.0 * ch / tot);
		g_checks++;
		if (ch * 100 / tot < 20 || ch * 100 / tot > 40) { g_fail++; printf("FAIL frame generator\n"); }
	}
	printf("%d checks, %d failures\n%s\n", g_checks, g_fail, g_fail ? "FAILED" : "ALL OK");
	return g_fail ? 1 : 0;
}
#endif
