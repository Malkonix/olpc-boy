/* Audio sink.  Modes:
 *   ALSA  raw ioctl PCM (no alsa-lib); the sound card clock paces the emulator
 *   CLOCK no device: a virtual buffer drained against CLOCK_MONOTONIC paces it
 *   NULL  --no-audio: samples are dropped, nothing paces (tests)
 */
#ifndef OLPC_RUN_AUDIO_H
#define OLPC_RUN_AUDIO_H
#include <stddef.h>
#include <stdint.h>
#include "resample.h"
#include "diag.h"

enum { AUD_NULL, AUD_CLOCK, AUD_ALSA };

struct audio {
	int mode;
	int fd;
	unsigned core_rate;       /* what the core produces */
	unsigned rate;            /* what the device plays */
	unsigned period, bufsz;   /* frames */
	unsigned target;          /* wanted fill in frames: pacing keeps the device buffer near this */
	unsigned avail_min, start_threshold;   /* software params actually set (frames) */
	unsigned fill_min, fill_max;           /* fill seen after startup: when poll() woke / after the write */
	unsigned long nflush;
	int no_query;             /* never ask the driver for the fill (DELAY/STATUS): pace with poll() only */
	long last_fill;           /* last fill read from the driver, frames; -1 = none yet / not queried */
	unsigned long long underruns_poll, underruns_write;   /* detected via POLLERR / EPIPE (sum = underruns) */
	unsigned long long recover_fail;                      /* PREPARE or the restart write failed */
	int last_via;             /* XR_VIA_* of the latest underrun */
	char dev[64];             /* the PCM node in use */
	int16_t *zeros;           /* bufsz frames of silence for the restart after an underrun */
	int resample;
	struct resampler rs;
	int16_t *q;               /* queued output frames (device rate), interleaved */
	size_t qn, qcap;
	unsigned long long underruns, stalls, written;
	uint64_t t0;              /* CLOCK mode: start of the virtual stream */
	volatile int *quit;
	/* what is needed to reopen the PCM after repeated underruns */
	double req_period_ms, req_latency_ms;
	unsigned req_target;
	/* recovery (policy in diag.c) */
	struct recov rc;
	int last_action;          /* RA_* of the latest underrun */
	unsigned long n_action[4];            /* underruns answered with RA_NONE/PREPARE/REOPEN/CLOCK */
	unsigned long reopen_failed;
	unsigned long incident_no;            /* incidents started */
	unsigned long incident_done;          /* incidents closed (stream stable again) */
	long inc_frames; double inc_ms;       /* of the incident closed last */
	uint64_t frame_ns;        /* nominal frame period: pacing during recovery; 0 = no pacing */
	uint64_t pace_next;
	unsigned long paced_frames;
	uint64_t poll_ns;         /* time the latest flush spent blocked in poll() */
	/* xrun_debug write: what was tried */
	char xd_path[96];
	int xd_errno;
};

/* Hardware limits of a PCM (from HW_REFINE with only the rate fixed). */
struct audio_hw {
	unsigned period_min, period_max;      /* frames */
	unsigned periods_min, periods_max;
	unsigned buffer_min, buffer_max;      /* frames */
};

/* Buffer layout derived from rate and the requested times; pure, unit-tested. */
struct audio_plan {
	unsigned period, periods, bufsz;      /* hw_params request, frames */
	unsigned target;                      /* wanted fill, a multiple of period */
	unsigned avail_min, start_threshold;  /* sw_params */
};

/* hw_params request: period of period_ms rounded to 16 frames, target fill of
 * latency_ms (or target_frames when non-zero) rounded up to whole periods, buffer of
 * at least 4096 frames and twice the target, all clamped to hw. */
void audio_plan_hw(struct audio_plan *p, unsigned rate, double period_ms, double latency_ms,
		   unsigned target_frames, const struct audio_hw *hw);
/* sw_params from the period/buffer the hardware really granted (it may differ from
 * the request): re-clamps the target and derives avail_min/start_threshold. */
void audio_plan_sw(struct audio_plan *p, unsigned period, unsigned bufsz);

/* dev: path or NULL to probe /dev/snd/pcmC0D0p and pcmC1D0p.  mode_null != 0
 * selects the null sink.  Logs the outcome on stderr. */
int  audio_open(struct audio *a, unsigned core_rate, const char *dev, int mode_null,
		double period_ms, double latency_ms, unsigned target_frames, volatile int *quit);
/* Write n to the PCM's /proc xrun_debug file (needs CONFIG_SND_PCM_XRUN_DEBUG); 0 on success.
 * The path tried and the errno are kept in a->xd_path / a->xd_errno. */
int  audio_xrun_debug(struct audio *a, unsigned n);
void audio_push(struct audio *a, const int16_t *frames, size_t n);   /* never blocks */
void audio_flush(struct audio *a);          /* pacing point: waits, then hands the queue to the sink */
long audio_fill(struct audio *a);           /* frames queued in the sink, -1 when unknown (null) */
void audio_close(struct audio *a);
#endif
