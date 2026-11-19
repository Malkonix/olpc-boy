/* Pure helpers of the audio diagnostics: underrun event ring, time-based skip
 * criterion, strict option parsing.  Unit-tested on the host (selftest.c). */
#ifndef OLPC_RUN_DIAG_H
#define OLPC_RUN_DIAG_H
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#define XR_MAX 64                     /* events kept: the last XR_MAX */
enum { XR_VIA_NONE, XR_VIA_POLL, XR_VIA_WRITE };

struct xrun_ev {
	unsigned long frame;          /* frame number (1-based) in whose audio flush it was detected */
	double since_prev_ms;         /* start of this frame minus start of the previous one */
	double work_ms[3];            /* core + blit time of the previous three frames, latest first */
	long fill;                    /* last fill read before it, frames; -1 = not queried */
	int via;                      /* XR_VIA_*: POLLERR in poll() or EPIPE from the write */
	int action;                   /* RA_*: what the recovery did about it */
	long rec_frames;              /* first event of an incident: frames until the stream was stable again; -1 = n/a or not yet */
	double rec_ms;                /* ... and milliseconds; -1 = n/a or not yet */
};

struct xrun_ring {
	struct xrun_ev ev[XR_MAX];
	unsigned long total;          /* events ever added; ev[] holds the last min(total, XR_MAX) */
};

/* Adds an event and returns its sequence number (r->total before the call). */
unsigned long xr_add(struct xrun_ring *r, const struct xrun_ev *e);
/* Fill in rec_frames/rec_ms of the event with sequence number seq, if it is still kept. */
void xr_set_recovery(struct xrun_ring *r, unsigned long seq, long frames, double ms);
unsigned xr_kept(const struct xrun_ring *r);
/* "xrun_events_total=N", "xrun_events_kept=K", then one "xrun_event_<i>=..." line per kept event,
 * oldest first.  NUL-terminated, truncated at a line boundary; returns the length. */
size_t xr_format(const struct xrun_ring *r, char *out, size_t cap);

/* Without a fill query: skip the next blit when the previous frame's work did not fit in a frame. */
int skip_by_time(uint64_t prev_work_ns, uint64_t period_ns);

/* Whole decimal number 0..max, nothing else; returns 0 and sets *out, else -1. */
int parse_uint(const char *s, unsigned long max, unsigned long *out);

/* ---- underrun recovery policy (pure; audio.c performs the actions)
 *
 * An incident starts with an underrun and lasts until the stream is stable again.  The first
 * underrun is answered with DROP + PREPARE + silence up to the target.  A further underrun
 * while the incident is still open means that was not enough: close and reopen the PCM
 * (full hw_free/hw_params), and as the last resort give up on the card and pace by the clock.
 * While an incident is open the loop is paced by the clock instead of running free. */
enum { RA_NONE, RA_PREPARE, RA_REOPEN, RA_CLOCK };
#define RC_CLEAN_FRAMES 6             /* clean frames that close an incident even if poll() never blocked */
#define RC_WAIT_NS 1000000ull         /* a poll() that slept this long really waited on the card */

struct recov {
	unsigned level;               /* 0 = no incident, else underruns so far in the incident */
	unsigned long start_frame;    /* frame of the incident's first underrun */
	uint64_t start_ns;
	unsigned clean;               /* clean frames since the last underrun */
	unsigned long incidents;      /* incidents closed */
	unsigned long done_frames;    /* of the incident closed last */
	double done_ms;
};

/* An underrun was found in `frame`: returns the RA_* to perform. */
int recov_underrun(struct recov *r, unsigned long frame, uint64_t now_ns);
/* 1 while the loop must be paced by the clock. */
int recov_pacing(const struct recov *r);
/* End of an audio flush: `underrun` = this frame had one, `waited` = poll() really blocked.
 * Returns 1 when this frame closed the incident (done_frames/done_ms are set). */
int recov_frame_end(struct recov *r, unsigned long frame, uint64_t now_ns, int underrun, int waited);

/* Clock pacing: *next is the deadline of the next frame (0 = none yet).  Returns the absolute
 * time to sleep until and advances *next by frame_ns; resynchronises when far behind. */
uint64_t pace_deadline(uint64_t *next, uint64_t now_ns, uint64_t frame_ns);

/* "300,700,1200": up to max ascending-or-not frame numbers; returns the count or -1. */
int parse_frame_list(const char *s, unsigned long *out, unsigned max);

/* Diagnostic log.  While buffering is on (the main loop) messages are kept in memory instead of
 * being written to stderr, which is a file on the USB stick; dlog_flush() writes them out. */
void dlog_buffering(int on);
void dlog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void dlog_flush(void);
void dlogv(const char *fmt, va_list ap);
/* test access */
size_t dlog_pending(void);

#endif
