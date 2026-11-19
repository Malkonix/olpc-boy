/* Host self-test: scaler, resampler, input mapping.  No hardware needed. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include "scale.h"
#include "resample.h"
#include "input.h"
#include "audio.h"
#include "diag.h"
#include "saver.h"
#include "overlay.h"
#include <sys/stat.h>
#include <time.h>

static int g_fail, g_checks;
#define CHECK(cond, ...) do { g_checks++; if (!(cond)) { g_fail++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static uint32_t rs_ = 12345;
static uint32_t rnd(void) { rs_ = rs_ * 1664525u + 1013904223u; return rs_ >> 8; }

/* ------------------------------------------------------------------ scaler */

static void scale_case(const char *fn, const struct pixtab *t, int fmt, int sw, int sh, int k,
		       size_t pad, size_t xoff, size_t pitch_extra)
{
	struct blit b;
	int B = t->B, dw = sw * k, dh = sh * k, y, x, mode;
	size_t sbytes = fmt == SRC_XRGB8888 ? 4 : 2, pitch = (size_t)sw * sbytes + pitch_extra;
	size_t stride = (size_t)dw * B + pad + xoff * B, total = stride * (dh + 2), off = stride + xoff * B;
	uint8_t *ref = malloc(total), *got = malloc(total), *init = malloc(total);
	uint8_t *src = malloc(pitch * sh), *prev = malloc(pitch * sh), *pv = malloc(pitch * sh);

	for (y = 0; y < (int)total; y++) init[y] = (uint8_t)rnd();
	for (y = 0; y < (int)(pitch * sh); y++) src[y] = (uint8_t)rnd();
	memcpy(prev, src, pitch * sh);
	for (y = 0; y < sh; y++)                         /* ~30% of the rows differ from the old frame */
		if (rnd() % 10 < 3) for (x = 0; x < (int)pitch; x++) prev[y * pitch + x] = (uint8_t)rnd();
	blit_alloc(&b, sw, sh, k, fmt, t, stride);

	memcpy(ref, init, total);
	blit_naive(&b, src, pitch, ref + off);

	for (mode = 0; mode < 3; mode++) {                /* 0 full, 1 dirty rows over the old frame, 2 identical frame */
		int rows;
		memcpy(got, init, total);
		if (mode == 0) {
			memset(pv, 0, pitch * sh);
			rows = blit_rows(&b, src, pitch, pv, 1, got + off);
			CHECK(rows == sh, "%s full: %d rows of %d", fn, rows, sh);
		} else {
			/* the screen shows `prev` (drawn naively); pv mirrors it */
			blit_naive(&b, prev, pitch, got + off);
			memcpy(pv, prev, pitch * sh);
			rows = blit_rows(&b, src, pitch, pv, 0, got + off);
			if (mode == 2) {                           /* second call with the same frame draws nothing */
				int again = blit_rows(&b, src, pitch, pv, 0, got + off);
				CHECK(again == 0, "%s: unchanged frame drew %d rows", fn, again);
			}
		}
		CHECK(!memcmp(ref, got, total), "%s %dx%d k=%d pad=%zu xoff=%zu pitch+%zu mode=%d: output differs",
		      fn, sw, sh, k, pad, xoff, pitch_extra, mode);
		/* prev must now equal the source rows that matter */
		for (y = 0; y < sh; y++)
			CHECK(!memcmp(pv + y * pitch, src + y * pitch, (size_t)sw * sbytes), "%s: prev row %d stale", fn, y);
	}
	blit_free(&b);
	free(ref); free(got); free(init); free(src); free(prev); free(pv);
}

static uint16_t one_px(int fmt, uint32_t v)
{
	struct pixtab t;
	struct blit b;
	uint8_t src[8] = { 0 }, pv[8] = { 0 }, dst[8] = { 0 };

	pixtab_init(&t, 16, 11, 5, 5, 6, 0, 5);
	if (fmt == SRC_XRGB8888) memcpy(src, &v, 4); else { uint16_t s = (uint16_t)v; memcpy(src, &s, 2); }
	blit_alloc(&b, 1, 1, 1, fmt, &t, 2);
	blit_rows(&b, src, 4, pv, 1, dst);
	blit_free(&b);
	return (uint16_t)(dst[0] | (dst[1] << 8));
}

static void scale_tests(void)
{
	static const struct { const char *n; int bpp, ro, rl, go, gl, bo, bl; } fm[] = {
		{ "rgb565", 16, 11, 5, 5, 6, 0, 5 }, { "bgr565", 16, 0, 5, 5, 6, 11, 5 },
		{ "xrgb8888", 32, 16, 8, 8, 8, 0, 8 }, { "xbgr8888", 32, 0, 8, 8, 8, 16, 8 },
	};
	static const int sz[][3] = { {160,144,4}, {160,144,6}, {256,240,2}, {256,240,3}, {1,1,1}, {1,1,7}, {3,2,3}, {7,5,5}, {13,3,6}, {17,9,4} };
	int f, s, sf;
	struct pixtab t;

	for (sf = 0; sf < 3; sf++)
		for (f = 0; f < 4; f++) {
			pixtab_init(&t, fm[f].bpp, fm[f].ro, fm[f].rl, fm[f].go, fm[f].gl, fm[f].bo, fm[f].bl);
			for (s = 0; s < (int)(sizeof sz / sizeof sz[0]); s++) {
				scale_case(fm[f].n, &t, sf, sz[s][0], sz[s][1], sz[s][2], 0, 0, 0);
				scale_case(fm[f].n, &t, sf, sz[s][0], sz[s][1], sz[s][2], 24, 3, 8);
			}
		}
	/* source format conversions against independently written expectations */
	CHECK(one_px(SRC_XRGB8888, 0xffffff) == 0xffff, "xrgb white");
	CHECK(one_px(SRC_XRGB8888, 0xff0000) == 0xf800, "xrgb red");
	CHECK(one_px(SRC_XRGB8888, 0x00ff00) == 0x07e0, "xrgb green");
	CHECK(one_px(SRC_XRGB8888, 0x0000ff) == 0x001f, "xrgb blue");
	CHECK(one_px(SRC_XRGB8888, 0x000000) == 0, "xrgb black");
	CHECK(one_px(SRC_0RGB1555, 0x7fff) == 0xffff, "1555 white");
	CHECK(one_px(SRC_0RGB1555, 0x7c00) == 0xf800, "1555 red");
	CHECK(one_px(SRC_0RGB1555, 0x03e0) == 0x07e0, "1555 green");
	CHECK(one_px(SRC_0RGB1555, 0x001f) == 0x001f, "1555 blue");
	CHECK(one_px(SRC_RGB565, 0x1234) == 0x1234, "565 identity");
	/* scale choice */
	CHECK(scale_pick(160, 144, 1200, 900) == 6, "gb scale %d", scale_pick(160, 144, 1200, 900));
	CHECK(scale_pick(256, 240, 1200, 900) == 3, "nes scale %d", scale_pick(256, 240, 1200, 900));
	CHECK(scale_pick(2000, 10, 1200, 900) == 1, "oversize scale");
	printf("scaler: done\n");
}

/* ------------------------------------------------------------------ resampler */

static void resample_tests(void)
{
	static const unsigned rates[] = { 32768, 44100, 22050, 48000, 8000 };
	unsigned r;

	for (r = 0; r < sizeof rates / sizeof rates[0]; r++) {
		unsigned in = rates[r], out = 48000;
		size_t n = in * 3, i, no = 0, nref = 0, cap;
		int16_t *x = malloc(n * 4), *y, *yref, *chunk;
		struct resampler a, b;
		double maxerr = 0;
		size_t pos = 0;

		/* 440 Hz sine, both channels (right inverted) */
		for (i = 0; i < n; i++) {
			x[2 * i] = (int16_t)(12000 * sin(2 * M_PI * 440.0 * i / in));
			x[2 * i + 1] = (int16_t)-x[2 * i];
		}
		rs_init(&a, in, out); rs_init(&b, in, out);
		cap = rs_max_out(&a, n);
		yref = malloc(cap * 4); y = malloc(cap * 4); chunk = malloc(rs_max_out(&a, 4096) * 4);
		nref = rs_process(&a, x, n, yref, cap);
		/* the same stream cut into irregular chunks must give identical output */
		while (pos < n) {
			size_t c = 1 + rnd() % 1500, m;
			if (pos + c > n) c = n - pos;
			m = rs_process(&b, x + 2 * pos, c, chunk, rs_max_out(&b, c));
			memcpy(y + 2 * no, chunk, m * 4);
			no += m; pos += c;
		}
		CHECK(no == nref && !memcmp(y, yref, nref * 4), "resample %u: chunked output differs (%zu vs %zu)", in, no, nref);
		/* length: 3 s of input = 3*out frames within a couple of frames (32.32 step) */
		CHECK(llabs((long long)nref - (long long)out * 3) <= 3 + out * 3 / 20000, "resample %u->%u: %zu frames for 3 s", in, out, nref);
		/* signal: output k is at input position k*in/out - 1 (one input frame of delay: the interpolator starts at the previous frame) */
		for (i = 100; i < nref - 100; i++) {
			double ideal = 12000 * sin(2 * M_PI * 440.0 * ((double)i * in / out - 1.0) / in), e = fabs(yref[2 * i] - ideal);
			if (e > maxerr) maxerr = e;
			CHECK(yref[2 * i + 1] == -yref[2 * i] || abs(yref[2 * i + 1] + yref[2 * i]) <= 1, "resample %u: channels mixed at %zu", in, i);
		}
		/* linear interpolation error of a 440 Hz sine at >= 8 kHz is bounded (~1.5% of 12000 at 8 kHz) */
		CHECK(maxerr < 12000 * 0.03, "resample %u: max error %.0f", in, maxerr);
		printf("resample %u -> %u: %zu frames, max error vs ideal sine %.1f (of 12000)\n", in, out, nref, maxerr);
		free(x); free(y); free(yref); free(chunk);
	}
	/* identical rates: pass-through delayed by one frame */
	{
		struct resampler a; int16_t x[20], y[40]; size_t i, m;
		for (i = 0; i < 20; i++) x[i] = (int16_t)(i * 100);
		rs_init(&a, 48000, 48000);
		m = rs_process(&a, x, 10, y, 40);
		CHECK(m == 10, "1:1 length %zu", m);
		for (i = 0; i < m; i++) CHECK(y[2 * i] == (i ? x[2 * (i - 1)] : 0), "1:1 value %zu", i);
	}
	printf("resampler: done\n");
}

/* ------------------------------------------------------------------ input */

static struct input_event ev(int type, int code, int value)
{
	struct input_event e;

	memset(&e, 0, sizeof e);
	e.type = (uint16_t)type; e.code = (uint16_t)code; e.value = value;
	return e;
}

static void pump(struct inp_dev *d, int wfd, struct input_event e)
{
	if (write(wfd, &e, sizeof e) != (ssize_t)sizeof e) { CHECK(0, "pipe write"); return; }
	CHECK(inp_dev_drain(d) == 0, "drain");
}

#define JB(n) (1u << (n))

static void input_tests(void)
{
	int p[2];
	struct inp_dev d;
	struct input in;

	CHECK(pipe(p) == 0, "pipe");
	fcntl(p[0], F_SETFL, O_NONBLOCK);

	/* built-in buttons */
	memset(&d, 0, sizeof d); d.fd = p[0]; d.cls = IN_BUILTIN;
	pump(&d, p[1], ev(EV_KEY, KEY_END, 1));      CHECK(inp_dev_mask(&d) == JB(8), "check = A: %x", inp_dev_mask(&d));
	pump(&d, p[1], ev(EV_KEY, KEY_PAGEDOWN, 1)); CHECK(inp_dev_mask(&d) == (JB(8) | JB(0)), "cross = B: %x", inp_dev_mask(&d));
	pump(&d, p[1], ev(EV_KEY, KEY_HOME, 1));     CHECK(inp_dev_mask(&d) == (JB(8) | JB(0) | JB(2)), "square = SELECT: %x", inp_dev_mask(&d));
	pump(&d, p[1], ev(EV_KEY, KEY_PAGEUP, 1));   CHECK(inp_dev_mask(&d) == (JB(8) | JB(0) | JB(2) | JB(3)), "circle = START: %x", inp_dev_mask(&d));
	pump(&d, p[1], ev(EV_KEY, KEY_UP, 1));       CHECK(inp_dev_mask(&d) & JB(4), "up");
	pump(&d, p[1], ev(EV_KEY, KEY_LEFT, 1));     CHECK(inp_dev_mask(&d) & JB(6), "left");
	pump(&d, p[1], ev(EV_KEY, KEY_END, 0));      CHECK(!(inp_dev_mask(&d) & JB(8)), "A released");
	pump(&d, p[1], ev(EV_KEY, 153, 1));          CHECK(d.rotate == 1, "rotate down");
	pump(&d, p[1], ev(EV_KEY, 153, 2));          CHECK(d.rotate == 1, "rotate repeat keeps it down");
	pump(&d, p[1], ev(EV_KEY, 153, 0));          CHECK(d.rotate == 0, "rotate up");
	CHECK(inp_dev_mask(&d) & JB(0), "cross still held");

	/* USB pad: xpad layout */
	memset(&d, 0, sizeof d); d.fd = p[0]; d.cls = IN_PAD;
	d.absmin[0] = d.absmin[1] = -32768; d.absmax[0] = d.absmax[1] = 32767;
	pump(&d, p[1], ev(EV_KEY, 0x130, 1)); CHECK(inp_dev_mask(&d) == JB(0), "BTN_SOUTH = B");
	pump(&d, p[1], ev(EV_KEY, 0x130, 0));
	pump(&d, p[1], ev(EV_KEY, 0x131, 1)); CHECK(inp_dev_mask(&d) == JB(8), "BTN_EAST = A");
	pump(&d, p[1], ev(EV_KEY, 0x131, 0));
	pump(&d, p[1], ev(EV_KEY, 0x13a, 1)); CHECK(inp_dev_mask(&d) == JB(2), "select");
	pump(&d, p[1], ev(EV_KEY, 0x13b, 1)); CHECK(inp_dev_mask(&d) == (JB(2) | JB(3)), "start");
	pump(&d, p[1], ev(EV_KEY, 0x13a, 0)); pump(&d, p[1], ev(EV_KEY, 0x13b, 0));
	pump(&d, p[1], ev(EV_ABS, ABS_HAT0X, -1)); CHECK(inp_dev_mask(&d) == JB(6), "hat left");
	pump(&d, p[1], ev(EV_ABS, ABS_HAT0X, 1));  CHECK(inp_dev_mask(&d) == JB(7), "hat right");
	pump(&d, p[1], ev(EV_ABS, ABS_HAT0X, 0));  CHECK(inp_dev_mask(&d) == 0, "hat centre");
	pump(&d, p[1], ev(EV_ABS, ABS_HAT0Y, -1)); CHECK(inp_dev_mask(&d) == JB(4), "hat up");
	pump(&d, p[1], ev(EV_ABS, ABS_HAT0Y, 1));  CHECK(inp_dev_mask(&d) == JB(5), "hat down");
	pump(&d, p[1], ev(EV_ABS, ABS_HAT0Y, 0));
	pump(&d, p[1], ev(EV_ABS, ABS_X, 20000));  CHECK(inp_dev_mask(&d) == JB(7), "stick right");
	pump(&d, p[1], ev(EV_ABS, ABS_X, 3000));   CHECK(inp_dev_mask(&d) == 0, "stick dead zone");
	pump(&d, p[1], ev(EV_ABS, ABS_Y, -20000)); CHECK(inp_dev_mask(&d) == JB(4), "stick up");
	pump(&d, p[1], ev(EV_ABS, ABS_Y, 0));
	pump(&d, p[1], ev(EV_KEY, 0x221, 1));      CHECK(inp_dev_mask(&d) == JB(5), "BTN_DPAD_DOWN");
	pump(&d, p[1], ev(EV_KEY, 0x221, 0));
	pump(&d, p[1], ev(EV_KEY, 0x120, 1));      CHECK(inp_dev_mask(&d) == JB(0), "clone BTN_TRIGGER = B");
	/* hat and stick can coexist: releasing one must not drop the other */
	pump(&d, p[1], ev(EV_KEY, 0x120, 0));
	pump(&d, p[1], ev(EV_ABS, ABS_HAT0X, 1)); pump(&d, p[1], ev(EV_ABS, ABS_X, 20000));
	pump(&d, p[1], ev(EV_ABS, ABS_HAT0X, 0)); CHECK(inp_dev_mask(&d) == JB(7), "stick survives hat release");

	/* manager: chord suppression and hold-to-exit timing */
	memset(&in, 0, sizeof in);
	in.ino = -1; in.rescan = -1; in.chord_file = "/nonexistent"; in.chord_fd = -1;
	in.ndev = 2;
	memset(in.dev, 0, sizeof in.dev);
	in.dev[0].cls = IN_BUILTIN; in.dev[0].fd = -1;
	in.dev[1].cls = IN_PAD; in.dev[1].fd = -1;
	in.dev[0].btn = JB(4) | JB(8);                       /* built-in up + A */
	in.dev[1].btn = JB(6);                               /* pad left */
	CHECK(input_mask(&in) == (JB(4) | JB(8) | JB(6)), "mask OR");
	in.chord = 1;
	CHECK(input_mask(&in) == (JB(8) | JB(6)), "chord drops the built-in D-pad only: %x", input_mask(&in));
	in.chord = 0;
	/* rotate held: exit only after 1 s; a release resets the timer.  Devices with fd -1 are
	 * dropped by the poll (read error), so drive the timers by hand. */
	in.rot_since = 0;
	in.rot_since = 1000000000ull | 1;
	CHECK(!input_exit(&in, 1500000000ull, 1000000000ull), "rotate 0.5 s: no exit");
	CHECK(input_exit(&in, 2100000000ull, 1000000000ull), "rotate 1.1 s: exit");
	in.rot_since = 0; in.ss_since = 5000000000ull | 1;
	CHECK(!input_exit(&in, 5900000000ull, 1000000000ull), "select+start 0.9 s: no exit");
	CHECK(input_exit(&in, 6100000000ull, 1000000000ull), "select+start 1.1 s: exit");
	in.ss_since = 0;
	CHECK(!input_exit(&in, 9000000000ull, 1000000000ull), "nothing held: no exit");

	/* poll with real pipes: press both pad buttons, time starts; release, it resets */
	{
		int q[2];
		struct input_event e;
		CHECK(pipe(q) == 0, "pipe2");
		fcntl(q[0], F_SETFL, O_NONBLOCK);
		memset(&in, 0, sizeof in);
		in.ino = -1; in.rescan = -1; in.chord_file = NULL; in.chord_fd = -1; in.ndev = 1;
		in.dev[0].fd = q[0]; in.dev[0].cls = IN_PAD;
		e = ev(EV_KEY, 0x13a, 1); if (write(q[1], &e, sizeof e) < 0) {}
		e = ev(EV_KEY, 0x13b, 1); if (write(q[1], &e, sizeof e) < 0) {}
		input_poll(&in, 1000000000ull);
		CHECK(!input_exit(&in, 1900000000ull, 1000000000ull), "poll: 0.9 s no exit");
		input_poll(&in, 2200000000ull);
		CHECK(input_exit(&in, 2200000000ull, 1000000000ull), "poll: 1.2 s exit");
		e = ev(EV_KEY, 0x13b, 0); if (write(q[1], &e, sizeof e) < 0) {}
		input_poll(&in, 2300000000ull);
		CHECK(!input_exit(&in, 9000000000ull, 1000000000ull), "poll: released resets");
		close(q[0]); close(q[1]);
	}
	printf("input: done\n");
}

/* ------------------------------------------------------------------ audio plan */

static void audio_tests(void)
{
	/* limits of the CS5535 PCM as reported on the XO-1: period 16..16380, 1..128 periods, buffer up to 16384 */
	struct audio_hw hw = { 16, 16380, 1, 128, 16, 16384 };
	struct audio_plan p;

	audio_plan_hw(&p, 32768, 4.0, 45.0, 0, &hw);
	printf("audio plan 32768 Hz: period %u x %u = %u, target %u, avail_min %u, start %u\n",
	       p.period, p.periods, p.bufsz, p.target, p.avail_min, p.start_threshold);
	CHECK(p.period == 128, "32768 Hz 4 ms: period %u", p.period);
	CHECK(p.bufsz == p.period * p.periods && p.bufsz >= 4096, "buffer %u", p.bufsz);
	CHECK(p.target % p.period == 0 && p.target >= 1474 && p.target < 1474 + p.period, "target %u", p.target);
	CHECK(p.avail_min == p.bufsz - p.target && p.start_threshold == p.target, "sw params");

	audio_plan_hw(&p, 48000, 4.0, 45.0, 0, &hw);
	printf("audio plan 48000 Hz: period %u x %u = %u, target %u, avail_min %u, start %u\n",
	       p.period, p.periods, p.bufsz, p.target, p.avail_min, p.start_threshold);
	CHECK(p.period == 192, "48000 Hz 4 ms: period %u", p.period);
	CHECK(p.bufsz == p.period * p.periods && p.bufsz >= 4096, "buffer %u", p.bufsz);
	CHECK(p.target % p.period == 0 && p.target >= 2160 && p.target < 2160 + p.period, "target %u", p.target);

	/* the old settings: 1024-frame periods, 2048 target, 4 periods */
	audio_plan_hw(&p, 32768, 31.25, 62.5, 0, &hw);
	CHECK(p.period == 1024 && p.periods == 4 && p.bufsz == 4096 && p.target == 2048 && p.avail_min == 2048,
	      "old settings: %u x %u, target %u", p.period, p.periods, p.target);

	/* an explicit frame target wins over the latency */
	audio_plan_hw(&p, 32768, 4.0, 45.0, 2048, &hw);
	CHECK(p.target == 2048, "explicit target %u", p.target);

	/* limits: period clamped up and down, periods clamped, buffer cap */
	{
		struct audio_hw tight = { 256, 512, 2, 8, 512, 4096 };
		audio_plan_hw(&p, 32768, 1.0, 45.0, 0, &tight);
		CHECK(p.period == 256, "period clamped up: %u", p.period);
		CHECK(p.periods <= 8 && p.periods >= 2 && p.bufsz <= 4096, "buffer within limits: %u x %u", p.periods, p.period);
		CHECK(p.target <= p.bufsz - p.period, "target %u leaves a period free in %u", p.target, p.bufsz);
		audio_plan_hw(&p, 48000, 100.0, 45.0, 0, &tight);
		CHECK(p.period == 512, "period clamped down: %u", p.period);
	}
	/* a huge latency request cannot exceed the buffer */
	audio_plan_hw(&p, 32768, 4.0, 2000.0, 0, &hw);
	CHECK(p.target <= p.bufsz - p.period && p.avail_min >= p.period, "huge latency: target %u buf %u", p.target, p.bufsz);
	/* the hardware grants something else: sw params follow the granted values */
	audio_plan_hw(&p, 32768, 4.0, 45.0, 0, &hw);
	audio_plan_sw(&p, 1024, 4096);
	CHECK(p.period == 1024 && p.bufsz == 4096 && p.target <= 4096 - 1024 && p.avail_min == 4096 - p.target &&
	      p.start_threshold == p.target, "re-derived: target %u avail_min %u", p.target, p.avail_min);
	printf("audio: done\n");
}

/* ------------------------------------------------------------------ diagnostics */


/* ------------------------------------------------------------------ recovery, saver, misc */

/* A fake stream drives the recovery policy: underrun_at() says whether frame f underruns; the
 * sim feeds the policy exactly the way audio.c does (one recov_underrun per underrun, one
 * recov_frame_end per frame, waited = poll really blocked). */
struct fake_dev { int bad_restart; int fixed; int stall_frame; };

static int fake_underrun(const struct fake_dev *d, unsigned long f)
{
	if (f == (unsigned long)d->stall_frame) return 1;
	/* a restart that does not hold: every frame underruns again until the device is reopened */
	return d->bad_restart && !d->fixed && f > (unsigned long)d->stall_frame;
}

static int sim(struct recov *rc, struct fake_dev *d, int *acts, int maxacts, unsigned long frames, int *paced_frames)
{
	unsigned long f;
	int n = 0;

	for (f = 1; f <= frames; f++) {
		uint64_t now = f * 16666667ull;
		int uf = fake_underrun(d, f);
		if (recov_pacing(rc)) (*paced_frames)++;
		if (uf) {
			int a = recov_underrun(rc, f, now);
			if (n < maxacts) acts[n++] = a;
			if (a == RA_REOPEN) d->fixed = 1;
			if (a == RA_CLOCK) break;
		}
		/* poll blocks for real from the second frame after a restart (the first one finds the fill at the target) */
		recov_frame_end(rc, f, now, uf, !uf && f >= (unsigned long)d->stall_frame + 2);
	}
	return n;
}

static void recov_tests(void)
{
	struct recov rc;
	struct fake_dev d;
	int acts[8], n, paced;
	uint64_t next = 0, t;
	unsigned long fl[9];
	char *big;

	/* one stall: PREPARE only, closed on the second clean frame that really waited */
	memset(&rc, 0, sizeof rc); memset(&d, 0, sizeof d); d.stall_frame = 10; paced = 0;
	n = sim(&rc, &d, acts, 8, 40, &paced);
	CHECK(n == 1 && acts[0] == RA_PREPARE, "single stall -> one PREPARE (n=%d)", n);
	CHECK(rc.incidents == 1 && rc.level == 0, "incident closed");
	CHECK(rc.done_frames == 2, "stable 2 frames after the underrun: %lu", rc.done_frames);
	CHECK(rc.done_ms > 33.0 && rc.done_ms < 34.0, "ms to stable %.2f", rc.done_ms);
	CHECK(paced == 2, "paced only while the incident was open: %d", paced);

	/* a restart that does not hold: PREPARE, then REOPEN, which fixes it; at most 2 underruns in a row */
	memset(&rc, 0, sizeof rc); memset(&d, 0, sizeof d); d.stall_frame = 10; d.bad_restart = 1; paced = 0;
	n = sim(&rc, &d, acts, 8, 60, &paced);
	CHECK(n == 2 && acts[0] == RA_PREPARE && acts[1] == RA_REOPEN, "bad restart -> PREPARE then REOPEN (n=%d)", n);
	CHECK(rc.incidents == 1 && rc.level == 0, "incident closed after the reopen");
	CHECK(paced >= 2, "loop paced during the whole incident: %d", paced);

	/* third underrun in the same incident: give up on the card */
	memset(&rc, 0, sizeof rc);
	CHECK(recov_underrun(&rc, 5, 1) == RA_PREPARE, "1st");
	CHECK(recov_underrun(&rc, 6, 2) == RA_REOPEN, "2nd");
	CHECK(recov_underrun(&rc, 7, 3) == RA_CLOCK, "3rd");
	CHECK(recov_underrun(&rc, 8, 4) == RA_CLOCK, "stays clock");

	/* two stalls far apart are two incidents, each answered with PREPARE only */
	memset(&rc, 0, sizeof rc);
	CHECK(recov_underrun(&rc, 10, 1) == RA_PREPARE, "stall 1");
	CHECK(!recov_frame_end(&rc, 11, 2, 0, 0), "first clean frame, not waited: still open");
	CHECK(recov_frame_end(&rc, 12, 3, 0, 1), "second clean frame that waited: closed");
	CHECK(recov_underrun(&rc, 400, 100) == RA_PREPARE, "stall 2 starts a fresh incident");
	/* poll never blocks: RC_CLEAN_FRAMES clean frames close it anyway */
	{
		int i, closed = 0;
		for (i = 1; i <= RC_CLEAN_FRAMES; i++) closed = recov_frame_end(&rc, 400 + i, 100 + i, 0, 0);
		CHECK(closed && rc.level == 0, "closed after %d clean frames without a wait", RC_CLEAN_FRAMES);
	}
	CHECK(!recov_pacing(&rc), "no pacing outside an incident");
	CHECK(!recov_frame_end(&rc, 1, 1, 0, 1), "no incident: nothing to close");

	/* clock pacing: one frame period apart, resync when far behind */
	t = pace_deadline(&next, 1000, 100);
	CHECK(t == 1000 && next == 1100, "first deadline is now");
	t = pace_deadline(&next, 1050, 100);
	CHECK(t == 1100 && next == 1200, "next deadline one period later");
	t = pace_deadline(&next, 1190, 100);
	CHECK(t == 1200 && next == 1300, "on schedule");
	t = pace_deadline(&next, 5000, 100);
	CHECK(t == 5000 && next == 5100, "resync after a long stall: no catch-up burst");

	/* --test-stall-at */
	CHECK(parse_frame_list("300,700", fl, 8) == 2 && fl[0] == 300 && fl[1] == 700, "list of two");
	CHECK(parse_frame_list("5", fl, 8) == 1 && fl[0] == 5, "single");
	CHECK(parse_frame_list("", fl, 8) < 0, "empty");
	CHECK(parse_frame_list("1,", fl, 8) < 0, "trailing comma");
	CHECK(parse_frame_list(",1", fl, 8) < 0, "leading comma");
	CHECK(parse_frame_list("1,x", fl, 8) < 0, "junk");
	CHECK(parse_frame_list("1 ,2", fl, 8) < 0, "blank");
	CHECK(parse_frame_list("1,2,3", fl, 2) < 0, "too many");
	CHECK(parse_frame_list(NULL, fl, 8) < 0, "NULL");

	/* diagnostic log: held in memory while buffering, nothing reaches stderr until the flush */
	dlog_buffering(1);
	dlog("olpc-run: test %d\n", 1);
	CHECK(dlog_pending() > 0, "message buffered");
	big = malloc(20000); memset(big, 'x', 19999); big[19999] = 0;
	dlog("%s", big);
	dlog("%s", big);
	CHECK(dlog_pending() < 8192, "buffer bounded: %zu", dlog_pending());
	free(big);
	dlog_buffering(0);
	CHECK(dlog_pending() == 0, "flushed");
	printf("recovery: done\n");
}

static char *slurp(const char *p, size_t *n)
{
	FILE *f = fopen(p, "rb");
	char *b = malloc(1 << 17);

	*n = 0;
	if (!f) { free(b); return NULL; }
	*n = fread(b, 1, 1 << 17, f);
	fclose(f);
	return b;
}

static uint64_t mono_ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}

#define LD_STATE(s) __atomic_load_n(&(s)->slot[0].state, __ATOMIC_ACQUIRE)

static int has_tmp(const char *dir)
{
	char cmd[300];
	snprintf(cmd, sizeof cmd, "ls '%s' | grep -q '\\.tmp$'", dir);
	return system(cmd) == 0;
}

static void saver_tests(void)
{
	static struct saver sv[4];
	static uint8_t ram[4096];
	char dir[] = "/tmp/olpcsvXXXXXX", p[300], q[320];
	char *got;
	size_t n;
	uint64_t t0;
	int left;

	if (!mkdtemp(dir)) { CHECK(0, "mkdtemp"); return; }
	snprintf(p, sizeof p, "%s/game.srm", dir);

	/* content, atomicity, no leftover temp file */
	memset(ram, 0x11, sizeof ram);
	CHECK(sv_start(&sv[0]) == 0, "start");
	CHECK(sv_register(&sv[0], p, ram, sizeof ram) == 0, "register");
	CHECK(sv_check(&sv[0], "t") == 0, "unchanged memory: nothing queued");
	CHECK(access(p, F_OK) != 0, "unchanged memory: no file written");
	ram[10] = 0x77;
	CHECK(sv_check(&sv[0], "t") == 0, "changed memory queued");
	CHECK(sv_wait(&sv[0], 5000) == 0, "wait");
	got = slurp(p, &n);
	CHECK(got && n == sizeof ram && !memcmp(got, ram, n), "file content equals the memory");
	free(got);
	CHECK(!has_tmp(dir), "no .tmp left behind");
	CHECK(sv[0].slot[0].written == 1, "written once: %lu", sv[0].slot[0].written);
	CHECK(sv_check(&sv[0], "t") == 0 && LD_STATE(&sv[0]) == SV_IDLE, "after the write the slot is clean again");

	/* the main thread does not wait for a slow write, and the file is the snapshot, not the live memory */
	memset(ram, 0x22, sizeof ram);
	CHECK(sv_start(&sv[1]) == 0, "start 2");
	snprintf(q, sizeof q, "%s/slow.srm", dir);
	sv[1].test_write_delay_ms = 400;
	CHECK(sv_register(&sv[1], q, ram, sizeof ram) == 0, "register 2");
	ram[0] = 0x33;                                       /* version A */
	t0 = mono_ms();
	CHECK(sv_check(&sv[1], "t") == 0, "queued");
	CHECK(mono_ms() - t0 < 100, "sv_check returned at once (%llu ms) while the helper sleeps", (unsigned long long)(mono_ms() - t0));
	ram[0] = 0x44;                                       /* version B, after the snapshot */
	left = sv_check(&sv[1], "t");
	CHECK(left == 1, "slot busy: left for a later call (%d)", left);
	usleep(900 * 1000);
	got = slurp(q, &n);
	CHECK(got && n == sizeof ram && got[0] == 0x33, "file holds the snapshot A, not the later memory: %02x", got ? got[0] : -1);
	free(got);
	CHECK(sv_check(&sv[1], "t") == 0, "helper idle: version B queued now");
	sv_capture_final(&sv[1]);
	t0 = mono_ms();
	CHECK(sv_wait(&sv[1], 5000) == 0, "wait for B");
	got = slurp(q, &n);
	CHECK(got && got[0] == 0x44, "file holds B after the wait");
	free(got);
	CHECK(!has_tmp(dir), "no .tmp left behind (2)");

	/* shutdown: the final copy is taken while the core is loaded, the write happens after */
	CHECK(sv_start(&sv[2]) == 0, "start 3");
	snprintf(q, sizeof q, "%s/fin.srm", dir);
	memset(ram, 0x55, sizeof ram);
	CHECK(sv_register(&sv[2], q, ram, sizeof ram) == 0, "register 3");
	ram[5] = 0x66;
	sv_capture_final(&sv[2]);
	memset(ram, 0, sizeof ram);                          /* the core is unloaded: live memory is gone */
	CHECK(sv_wait(&sv[2], 5000) == 0, "wait final");
	got = slurp(q, &n);
	CHECK(got && n == sizeof ram && got[5] == 0x66 && got[0] == 0x55, "final copy written");
	free(got);

	/* exit with a writer that never returns (a FIFO where the temp file should be): bounded wait */
	CHECK(sv_start(&sv[3]) == 0, "start 4");
	snprintf(q, sizeof q, "%s/dead.srm", dir);
	{
		char tq[340];
		snprintf(tq, sizeof tq, "%s.tmp", q);
		CHECK(mkfifo(tq, 0600) == 0, "mkfifo");
	}
	memset(ram, 0x01, sizeof ram);
	CHECK(sv_register(&sv[3], q, ram, sizeof ram) == 0, "register 4");
	ram[1] = 0x02;
	sv_capture_final(&sv[3]);
	t0 = mono_ms();
	left = sv_wait(&sv[3], 300);
	CHECK(left == 1, "blocked writer: reported unfinished (%d)", left);
	CHECK(mono_ms() - t0 >= 250 && mono_ms() - t0 < 1500, "wait bounded by the timeout: %llu ms", (unsigned long long)(mono_ms() - t0));
	CHECK(access(q, F_OK) != 0, "the destination was not touched");

	/* plain atomic write */
	CHECK(sv_write_atomic(p, (const uint8_t *)"abc", 3) == 0, "atomic write");
	got = slurp(p, &n);
	CHECK(got && n == 3 && !memcmp(got, "abc", 3), "atomic content");
	free(got);
	snprintf(q, sizeof q, "%s/nodir/x.srm", dir);
	CHECK(sv_write_atomic(q, (const uint8_t *)"abc", 3) < 0, "failure reported");
	printf("saver: done\n");
}

static void chord_tests(void)
{
	char f[] = "/tmp/olpcchXXXXXX";
	int fd = mkstemp(f), i;
	struct input in;

	if (fd < 0) { CHECK(0, "mkstemp"); return; }
	CHECK(write(fd, "1\n", 2) == 2, "write");
	input_init(&in, "/nonexistent-input-dir", f);
	input_poll(&in, 1);
	CHECK(in.chord == 1 && in.chord_fd >= 0, "chord on, descriptor kept open");
	CHECK(pwrite(fd, "0\n", 2, 0) == 2, "pwrite");           /* olpc-chordd rewrites in place */
	for (i = 0; i < 4; i++) input_poll(&in, 2 + i);
	CHECK(in.chord == 0, "chord off after an in-place rewrite");
	CHECK(pwrite(fd, "1\n", 2, 0) == 2, "pwrite");
	for (i = 0; i < 4; i++) input_poll(&in, 10 + i);
	CHECK(in.chord == 1, "chord on again");
	close(fd);
	unlink(f);
	printf("chord: done\n");
}

static void diag_tests(void)
{
	struct xrun_ring r;
	struct xrun_ev e;
	static char buf[XR_MAX * 260 + 100];
	char small[120];
	unsigned long v = 99;
	size_t n;
	unsigned i;

	memset(&r, 0, sizeof r);
	n = xr_format(&r, buf, sizeof buf);
	CHECK(n == strlen(buf) && !strcmp(buf, "xrun_events_total=0\nxrun_events_kept=0\n"), "empty ring: '%s'", buf);

	memset(&e, 0, sizeof e);
	e.frame = 17; e.since_prev_ms = 52.25; e.work_ms[0] = 9.5; e.work_ms[1] = 10; e.work_ms[2] = 11.04; e.fill = 123; e.via = XR_VIA_POLL; e.rec_frames = -1; e.rec_ms = -1;
	CHECK(xr_add(&r, &e) == 0, "first sequence number");
	e.frame = 40; e.fill = -1; e.via = XR_VIA_WRITE;
	xr_add(&r, &e);
	n = xr_format(&r, buf, sizeof buf);
	CHECK(n == strlen(buf), "length returned %zu, strlen %zu", n, strlen(buf));
	CHECK(strstr(buf, "xrun_events_total=2\nxrun_events_kept=2\n") == buf, "header: %s", buf);
	CHECK(strstr(buf, "xrun_event_1=frame=17 since_prev_ms=52.2 work_ms=9.5,10.0,11.0 fill=123 via=poll action=none rec_frames=-1 rec_ms=-1.0\n") != NULL ||
	      strstr(buf, "xrun_event_1=frame=17 since_prev_ms=52.3 work_ms=9.5,10.0,11.0 fill=123 via=poll action=none rec_frames=-1 rec_ms=-1.0\n") != NULL, "event 1: %s", buf);
	CHECK(strstr(buf, "xrun_event_2=frame=40 ") && strstr(buf, "fill=-1 via=write action=none"), "event 2: %s", buf);
	/* recovery info is filled in later, by sequence number */
	xr_set_recovery(&r, 1, 3, 51.25);
	xr_set_recovery(&r, 99, 1, 1.0);                          /* unknown sequence number: ignored */
	n = xr_format(&r, buf, sizeof buf);
	CHECK(strstr(buf, "action=none rec_frames=3 rec_ms=51.2\n") || strstr(buf, "action=none rec_frames=3 rec_ms=51.3\n"), "recovery fields: %s", buf);
	e.action = RA_REOPEN;
	xr_add(&r, &e);
	n = xr_format(&r, buf, sizeof buf);
	CHECK(strstr(buf, "xrun_event_3=frame=40 ") && strstr(buf, "action=reopen rec_frames=-1"), "action name: %s", buf);
	e.action = RA_NONE;
	r.total = 2;

	/* ring: only the last XR_MAX kept, oldest first, total keeps counting */
	for (i = 3; i <= XR_MAX + 10; i++) { e.frame = i; xr_add(&r, &e); }
	CHECK(r.total == XR_MAX + 10 && xr_kept(&r) == XR_MAX, "ring count %lu kept %u", r.total, xr_kept(&r));
	n = xr_format(&r, buf, sizeof buf);
	CHECK(strstr(buf, "xrun_event_1=frame=11 ") != NULL, "oldest kept is frame 11 (events 1..10 overwritten): %.120s", strstr(buf, "xrun_event_1=") ? strstr(buf, "xrun_event_1=") : "?");
	CHECK(strstr(buf, "xrun_event_64=frame=74 ") != NULL, "newest is frame 74");
	CHECK(!strstr(buf, "xrun_event_65="), "no 65th line");

	/* too small a buffer: cut at a line boundary, still terminated */
	n = xr_format(&r, small, sizeof small);
	CHECK(n == strlen(small) && n < sizeof small && (n == 0 || small[n - 1] == '\n'), "truncation: n=%zu", n);
	CHECK(xr_format(&r, small, 0) == 0, "cap 0");

	/* time-based skip criterion */
	CHECK(!skip_by_time(16000000ull, 16750000ull), "work within the frame: draw");
	CHECK(!skip_by_time(16750000ull, 16750000ull), "exactly one frame: draw");
	CHECK(skip_by_time(16750001ull, 16750000ull), "work over the frame: skip");
	CHECK(!skip_by_time(0, 16750000ull), "no history: draw");
	CHECK(!skip_by_time(30000000ull, 0), "unknown frame period: draw");

	/* option parsing */
	CHECK(parse_uint("0", 31, &v) == 0 && v == 0, "parse 0");
	CHECK(parse_uint("31", 31, &v) == 0 && v == 31, "parse 31");
	v = 99;
	CHECK(parse_uint("32", 31, &v) < 0 && v == 99, "32 over max");
	CHECK(parse_uint("", 31, &v) < 0, "empty");
	CHECK(parse_uint("-1", 31, &v) < 0, "negative");
	CHECK(parse_uint("+1", 31, &v) < 0, "plus sign");
	CHECK(parse_uint("1x", 31, &v) < 0, "trailing junk");
	CHECK(parse_uint(" 1", 31, &v) < 0, "leading blank");
	CHECK(parse_uint("99999999999999999999", 31, &v) < 0, "overflow");
	CHECK(parse_uint(NULL, 31, &v) < 0, "NULL");
	recov_tests();
	printf("diag: done\n");
}


/* ------------------------------------------------------------------ in-game indicator strip */

static void put_file(const char *path, const char *text)
{
	char tmp[600];
	FILE *f;

	snprintf(tmp, sizeof tmp, "%s.new", path);
	f = fopen(tmp, "w");
	if (f) { fputs(text, f); fclose(f); rename(tmp, path); }          /* replaced atomically, like olpc-wifi */
}

static long count_changed(const uint16_t *a, const uint16_t *b, int x0, int y0, int w, int h, int inside)
{
	long n = 0;
	int x, y;

	for (y = 0; y < 900; y++)
		for (x = 0; x < 1200; x++) {
			int in = x >= x0 && x < x0 + w && y >= y0 && y < y0 + h;
			if (in == inside && a[y * 1200 + x] != b[y * 1200 + x]) n++;
		}
	return n;
}

static void overlay_tests(void)
{
	static struct overlay o;
	static uint16_t fb[1200 * 900], ref[1200 * 900];
	static uint32_t fb32[1200 * 900], ref32[1200 * 900];
	struct pixtab t16, t32;
	char dir[] = "/tmp/olpc-ovl-XXXXXX", pst[300], pwf[300], l1[16], l2[16];
	int x = -1, i;
	long before;

	/* geometry: the strip lives in the right border only */
	CHECK(ovl_fits(1200, 900, 120, 18, 960, 864, &x) == 1 && x == 1080 + (120 - OVL_W) / 2, "GB 6x fits at x=%d", x);
	CHECK(ovl_fits(1200, 900, 120, 2, 960, 896, &x) == 1 && x == 1092, "NES 4x fits");
	CHECK(ovl_fits(1200, 900, 216, 90, 768, 720, &x) == 1 && x == 984 + (216 - OVL_W) / 2, "NES 3x (wide border) fits at x=%d", x);
	CHECK(ovl_fits(1200, 900, 0, 0, 1200 - (OVL_W + 2 * OVL_MARGIN) + 1, 900, &x) == 0, "border one pixel short: nothing");
	CHECK(ovl_fits(1200, 900, 0, 0, 1200 - (OVL_W + 2 * OVL_MARGIN), 900, &x) == 1 && x == 1200 - OVL_W - OVL_MARGIN, "border exactly enough: x=%d", x);
	CHECK(ovl_fits(1200, 900, 0, 0, 1200, 900, &x) == 0, "full-screen game: nothing");
	CHECK(ovl_fits(1200, OVL_Y + OVL_H - 1, 120, 0, 960, 100, &x) == 0, "screen too short: nothing");
	CHECK(ovl_fits(800, 600, 80, 0, 640, 480, &x) == 0, "800x600 with 80 px border: nothing");

	ovl_clock_lines("3:07 PM", l1, l2, sizeof l1);
	CHECK(!strcmp(l1, "3:07") && !strcmp(l2, "PM"), "12 h clock in two lines: %s / %s", l1, l2);
	ovl_clock_lines("15:07", l1, l2, sizeof l1);
	CHECK(!strcmp(l1, "15:07") && !l2[0], "24 h clock in one line");
	ovl_clock_lines("--:--", l1, l2, sizeof l1);
	CHECK(!strcmp(l1, "--:--") && !l2[0], "unset clock in one line");

	pixtab_init(&t16, 16, 11, 5, 5, 6, 0, 5);
	pixtab_init(&t32, 32, 16, 8, 8, 8, 0, 8);
	if (!mkdtemp(dir)) { CHECK(0, "mkdtemp"); return; }
	snprintf(pst, sizeof pst, "%s/status", dir);
	snprintf(pwf, sizeof pwf, "%s/wifi.status", dir);
	put_file(pst, "vol=20 volmax=31 mute=0 bri=7 brimax=15 mono=0\n");
	put_file(pwf, "state=connected ssid=Casa ip=10.0.0.2\n");

	/* paint: only the strip rectangle may change (16-bit fb) */
	for (i = 0; i < 1200 * 900; i++) fb[i] = ref[i] = 0x1234;
	ovl_init(&o);
	o.status_path = pst; o.wifi_path = pwf; o.wireless_path = "/nonexistent"; o.period = 5; o.fake_base = 1717268830; o.fake_t0 = 0;
	CHECK(ovl_place(&o, 1200, 900, 120, 18, 960, 864, (uint8_t *)fb, 2400, &t16) == 1, "place GB 6x");
	CHECK(o.x == 1092 && o.y == OVL_Y && o.redraws == OS_N && o.full_paints == 1, "first paint draws every slot: redraws=%lu", o.redraws);
	CHECK(count_changed(fb, ref, 1092, OVL_Y, OVL_W, OVL_H, 1) > 1000, "strip has pixels");
	CHECK(count_changed(fb, ref, 1092, OVL_Y, OVL_W, OVL_H, 0) == 0, "nothing outside the strip rectangle changed");

	/* quiet frames: a counter compare, nothing redrawn, nothing read */
	for (i = 0; i < 4; i++) ovl_frame(&o, (uint8_t *)fb, 2400, &t16, 0);
	CHECK(o.redraws == OS_N && o.ticks == 0, "4 frames of a 5-frame period: no tick (ticks=%lu)", o.ticks);
	ovl_frame(&o, (uint8_t *)fb, 2400, &t16, 0);
	CHECK(o.ticks == 1 && o.redraws == OS_N, "5th frame polls; nothing changed: no redraw (redraws=%lu)", o.redraws);
	/* a skipped blit defers the poll to the next frame */
	for (i = 0; i < 4; i++) ovl_frame(&o, (uint8_t *)fb, 2400, &t16, 0);
	ovl_frame(&o, (uint8_t *)fb, 2400, &t16, 1);
	CHECK(o.ticks == 1, "deferred while a blit is skipped");
	ovl_frame(&o, (uint8_t *)fb, 2400, &t16, 0);
	CHECK(o.ticks == 2, "polled on the next frame");

	/* a value changes: exactly that slot is redrawn, replaced file noticed, nothing outside the strip */
	memcpy(ref, fb, sizeof fb);
	before = (long)o.redraws;
	put_file(pst, "vol=20 volmax=31 mute=1 bri=7 brimax=15 mono=0\n");
	for (i = 0; i < 5; i++) ovl_frame(&o, (uint8_t *)fb, 2400, &t16, 0);
	CHECK((long)o.redraws == before + 1, "mute flips: one slot redrawn (%ld)", (long)o.redraws - before);
	CHECK(count_changed(fb, ref, 1092, OVL_Y, OVL_W, OVL_H, 1) > 0 && count_changed(fb, ref, 1092, OVL_Y, OVL_W, OVL_H, 0) == 0, "only the strip changed");
	memcpy(ref, fb, sizeof fb);
	before = (long)o.redraws;
	put_file(pst, "vol=20 volmax=31 mute=1 bri=3 brimax=15 mono=1\n");
	put_file(pwf, "state=off\n");
	for (i = 0; i < 5; i++) ovl_frame(&o, (uint8_t *)fb, 2400, &t16, 0);
	CHECK((long)o.redraws == before + 3, "brightness + mode + wifi: three slots (%ld)", (long)o.redraws - before);
	unlink(pst);
	for (i = 0; i < 5; i++) ovl_frame(&o, (uint8_t *)fb, 2400, &t16, 0);
	CHECK(o.st.vol == -1 && o.st.mono == -1, "status file gone: values unknown");
	put_file(pst, "vol=1 volmax=31 mute=0 bri=15 brimax=15 mono=0\n");
	for (i = 0; i < 5; i++) ovl_frame(&o, (uint8_t *)fb, 2400, &t16, 0);
	CHECK(o.st.vol == 1 && o.st.bri == 15, "status file back: re-opened");
	CHECK(o.redraw_ns_max > 0 && o.redraw_ns >= o.redraw_ns_max, "redraw time is accounted");
	ovl_close(&o);

	/* the same into a 32-bit framebuffer */
	for (i = 0; i < 1200 * 900; i++) fb32[i] = ref32[i] = 0x12345678u;
	ovl_init(&o);
	o.status_path = pst; o.wifi_path = pwf; o.wireless_path = "/nonexistent"; o.period = 5;
	CHECK(ovl_place(&o, 1200, 900, 216, 90, 768, 720, (uint8_t *)fb32, 4800, &t32) == 1 && o.x == 984 + (216 - OVL_W) / 2, "place NES 3x, 32 bpp");
	{
		long outside = 0, in = 0;
		int xx, yy;
		for (yy = 0; yy < 900; yy++)
			for (xx = 0; xx < 1200; xx++) {
				int ins = xx >= o.x && xx < o.x + OVL_W && yy >= o.y && yy < o.y + OVL_H;
				if (fb32[yy * 1200 + xx] != ref32[yy * 1200 + xx]) { if (ins) in++; else outside++; }
			}
		CHECK(in > 1000 && outside == 0, "32 bpp: strip %ld pixels, outside %ld", in, outside);
	}
	ovl_close(&o);

	/* a border that is too narrow: nothing drawn, nothing touched */
	for (i = 0; i < 1200 * 900; i++) fb[i] = ref[i] = 0x1234;
	ovl_init(&o);
	o.status_path = pst; o.wifi_path = pwf; o.period = 5;
	CHECK(ovl_place(&o, 1200, 900, 100, 0, 1000, 900, (uint8_t *)fb, 2400, &t16) == 0 && !o.on, "100 px border: not placed");
	for (i = 0; i < 20; i++) ovl_frame(&o, (uint8_t *)fb, 2400, &t16, 0);
	CHECK(!memcmp(fb, ref, sizeof fb), "narrow border: the framebuffer is untouched");
	ovl_close(&o);
	unlink(pst); unlink(pwf);
	rmdir(dir);
	printf("overlay: done\n");
}

int main(void)
{
	audio_tests();
	diag_tests();
	scale_tests();
	resample_tests();
	input_tests();
	chord_tests();
	saver_tests();
	overlay_tests();
	printf("%d checks, %d failures\n%s\n", g_checks, g_fail, g_fail ? "FAILED" : "ALL OK");
	return g_fail ? 1 : 0;
}
