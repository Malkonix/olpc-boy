#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "diag.h"

unsigned long xr_add(struct xrun_ring *r, const struct xrun_ev *e)
{
	unsigned long seq = r->total;

	r->ev[r->total % XR_MAX] = *e;
	r->total++;
	return seq;
}

void xr_set_recovery(struct xrun_ring *r, unsigned long seq, long frames, double ms)
{
	if (seq >= r->total || r->total - seq > XR_MAX) return;
	r->ev[seq % XR_MAX].rec_frames = frames;
	r->ev[seq % XR_MAX].rec_ms = ms;
}

unsigned xr_kept(const struct xrun_ring *r)
{
	return r->total < XR_MAX ? (unsigned)r->total : XR_MAX;
}

size_t xr_format(const struct xrun_ring *r, char *out, size_t cap)
{
	static const char *const via_name[] = { "none", "poll", "write" };
	static const char *const act_name[] = { "none", "prepare", "reopen", "clock" };
	unsigned kept = xr_kept(r), i;
	size_t n = 0;
	int w;

	if (!cap) return 0;
	out[0] = 0;
	w = snprintf(out, cap, "xrun_events_total=%lu\nxrun_events_kept=%u\n", r->total, kept);
	if (w < 0 || (size_t)w >= cap) { out[0] = 0; return 0; }
	n = (size_t)w;
	for (i = 0; i < kept; i++) {
		const struct xrun_ev *e = &r->ev[(r->total - kept + i) % XR_MAX];
		char line[240];

		w = snprintf(line, sizeof line, "xrun_event_%u=frame=%lu since_prev_ms=%.1f work_ms=%.1f,%.1f,%.1f fill=%ld via=%s action=%s rec_frames=%ld rec_ms=%.1f\n",
			     i + 1, e->frame, e->since_prev_ms, e->work_ms[0], e->work_ms[1], e->work_ms[2], e->fill,
			     via_name[e->via >= 0 && e->via <= XR_VIA_WRITE ? e->via : 0],
			     act_name[e->action >= 0 && e->action <= RA_CLOCK ? e->action : 0], e->rec_frames, e->rec_ms);
		if (w < 0 || (size_t)w >= sizeof line || n + (size_t)w >= cap) break;
		memcpy(out + n, line, (size_t)w + 1);
		n += (size_t)w;
	}
	return n;
}

int skip_by_time(uint64_t prev_work_ns, uint64_t period_ns)
{
	return period_ns && prev_work_ns > period_ns;
}

int parse_uint(const char *s, unsigned long max, unsigned long *out)
{
	char *end;
	unsigned long v;

	if (!s || *s < '0' || *s > '9') return -1;
	errno = 0;
	v = strtoul(s, &end, 10);
	if (errno || *end || v > max) return -1;
	*out = v;
	return 0;
}

int recov_underrun(struct recov *r, unsigned long frame, uint64_t now_ns)
{
	if (!r->level) { r->start_frame = frame; r->start_ns = now_ns; }
	r->level++;
	r->clean = 0;
	return r->level == 1 ? RA_PREPARE : r->level == 2 ? RA_REOPEN : RA_CLOCK;
}

int recov_pacing(const struct recov *r)
{
	return r->level > 0;
}

int recov_frame_end(struct recov *r, unsigned long frame, uint64_t now_ns, int underrun, int waited)
{
	if (!r->level) return 0;
	if (underrun) { r->clean = 0; return 0; }
	r->clean++;
	if (!waited && r->clean < RC_CLEAN_FRAMES) return 0;
	r->done_frames = frame - r->start_frame;
	r->done_ms = (now_ns - r->start_ns) / 1e6;
	r->level = 0;
	r->clean = 0;
	r->incidents++;
	return 1;
}

uint64_t pace_deadline(uint64_t *next, uint64_t now_ns, uint64_t frame_ns)
{
	uint64_t until;

	if (!*next || now_ns > *next + 4 * frame_ns) *next = now_ns;
	until = *next;
	*next += frame_ns;
	return until;
}

int parse_frame_list(const char *s, unsigned long *out, unsigned max)
{
	unsigned n = 0;

	if (!s || !*s) return -1;
	for (;;) {
		char *end;
		unsigned long v;

		if (*s < '0' || *s > '9' || n >= max) return -1;
		errno = 0;
		v = strtoul(s, &end, 10);
		if (errno) return -1;
		out[n++] = v;
		if (!*end) return (int)n;
		if (*end != ',') return -1;
		s = end + 1;
	}
}

#define DLOG_CAP 8192
static char g_dlog[DLOG_CAP];
static size_t g_dlog_n;
static int g_dlog_buf;

void dlog_buffering(int on)
{
	g_dlog_buf = on;
	if (!on) dlog_flush();
}

void dlogv(const char *fmt, va_list ap)
{
	int w;

	if (!g_dlog_buf) { vfprintf(stderr, fmt, ap); return; }
	if (g_dlog_n >= DLOG_CAP - 1) return;           /* full: drop, never touch the disk */
	w = vsnprintf(g_dlog + g_dlog_n, DLOG_CAP - g_dlog_n, fmt, ap);
	if (w > 0) g_dlog_n = g_dlog_n + (size_t)w < DLOG_CAP ? g_dlog_n + (size_t)w : DLOG_CAP - 1;
}

void dlog(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	dlogv(fmt, ap);
	va_end(ap);
}

void dlog_flush(void)
{
	if (g_dlog_n) { fwrite(g_dlog, 1, g_dlog_n, stderr); fflush(stderr); }
	g_dlog_n = 0;
}

size_t dlog_pending(void) { return g_dlog_n; }
