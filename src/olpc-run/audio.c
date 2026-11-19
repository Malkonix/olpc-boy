#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sound/asound.h>
#include "audio.h"
#include "diag.h"

#define NELEM(a) ((int)(sizeof(a) / sizeof((a)[0])))

static uint64_t now_ns(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

/* ---- raw hw_params helpers (same approach as olpc-fbbench) ---- */
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
static unsigned clampu(unsigned v, unsigned lo, unsigned hi) { return v < lo ? lo : v > hi ? hi : v; }

static unsigned round16(double v)
{
	unsigned r = (unsigned)(v / 16.0 + 0.5) * 16;

	return r < 16 ? 16 : r;
}

void audio_plan_hw(struct audio_plan *p, unsigned rate, double period_ms, double latency_ms,
		   unsigned target_frames, const struct audio_hw *hw)
{
	unsigned want_buf;

	memset(p, 0, sizeof *p);
	p->period = clampu(round16(rate * period_ms / 1000.0), hw->period_min, hw->period_max);
	if (!p->period) p->period = 16;
	p->target = target_frames ? target_frames : (unsigned)(rate * latency_ms / 1000.0 + 0.5);
	p->target = (p->target + p->period - 1) / p->period * p->period;
	if (!p->target) p->target = p->period;
	want_buf = p->target * 2 > 4096 ? p->target * 2 : 4096;
	p->periods = clampu((want_buf + p->period - 1) / p->period, hw->periods_min, hw->periods_max);
	if (hw->buffer_max && p->periods * p->period > hw->buffer_max) p->periods = hw->buffer_max / p->period;
	if (p->periods < hw->periods_min) p->periods = hw->periods_min;
	if (p->periods < 2) p->periods = 2;
	p->bufsz = p->periods * p->period;
	audio_plan_sw(p, p->period, p->bufsz);
}

void audio_plan_sw(struct audio_plan *p, unsigned period, unsigned bufsz)
{
	p->period = period; p->bufsz = bufsz;
	/* the target must leave room for one more period, and hold at least one */
	if (p->target > bufsz - (bufsz > period ? period : 0)) p->target = bufsz > period ? bufsz - period : bufsz;
	if (p->target < period) p->target = period < bufsz ? period : bufsz;
	/* poll() wakes when avail (= bufsz - fill) >= avail_min, i.e. when the fill is down to
	 * the target.  The kernel re-checks that on every period interrupt, so the wake-up
	 * granularity is one period; playback starts once the fill has reached the target
	 * (a start threshold above it would deadlock: poll would never wake first). */
	p->avail_min = bufsz - p->target;
	p->start_threshold = p->target;
}

/* Configure fd for S16 stereo at rate.  Returns 0 and fills a->rate/period/bufsz,
 * or -1 when the hardware refuses (the fd stays usable for another attempt). */
static int alsa_setup(struct audio *a, unsigned rate, double period_ms, double latency_ms, unsigned target_frames)
{
	struct snd_pcm_hw_params p, q;
	struct snd_pcm_sw_params sw;
	struct audio_hw hw;
	struct audio_plan pl;

	hw_any(&p);
	hw_set_mask(&p, SNDRV_PCM_HW_PARAM_ACCESS, SNDRV_PCM_ACCESS_RW_INTERLEAVED);
	hw_set_mask(&p, SNDRV_PCM_HW_PARAM_FORMAT, SNDRV_PCM_FORMAT_S16_LE);
	hw_set_mask(&p, SNDRV_PCM_HW_PARAM_SUBFORMAT, SNDRV_PCM_SUBFORMAT_STD);
	hw_set_iv(&p, SNDRV_PCM_HW_PARAM_CHANNELS, 2, 2);
	hw_set_iv(&p, SNDRV_PCM_HW_PARAM_RATE, rate, rate);
	if (ioctl(a->fd, SNDRV_PCM_IOCTL_HW_REFINE, &p) < 0) return -1;
	q = p;
	hw.period_min = hw_iv(&p, SNDRV_PCM_HW_PARAM_PERIOD_SIZE)->min;
	hw.period_max = hw_iv(&p, SNDRV_PCM_HW_PARAM_PERIOD_SIZE)->max;
	hw.periods_min = hw_iv(&p, SNDRV_PCM_HW_PARAM_PERIODS)->min;
	hw.periods_max = hw_iv(&p, SNDRV_PCM_HW_PARAM_PERIODS)->max;
	hw.buffer_min = hw_iv(&p, SNDRV_PCM_HW_PARAM_BUFFER_SIZE)->min;
	hw.buffer_max = hw_iv(&p, SNDRV_PCM_HW_PARAM_BUFFER_SIZE)->max;
	audio_plan_hw(&pl, rate, period_ms, latency_ms, target_frames, &hw);
	hw_set_iv(&q, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, pl.period, pl.period);
	hw_set_iv(&q, SNDRV_PCM_HW_PARAM_PERIODS, pl.periods, pl.periods);
	q.rmask = ~0u; q.cmask = 0;
	if (ioctl(a->fd, SNDRV_PCM_IOCTL_HW_REFINE, &q) < 0) q = p;   /* refused: let the driver choose */
	q.rmask = ~0u; q.cmask = 0;
	if (ioctl(a->fd, SNDRV_PCM_IOCTL_HW_PARAMS, &q) < 0) return -1;
	a->rate = hw_iv(&q, SNDRV_PCM_HW_PARAM_RATE)->min;
	audio_plan_sw(&pl, hw_iv(&q, SNDRV_PCM_HW_PARAM_PERIOD_SIZE)->min, hw_iv(&q, SNDRV_PCM_HW_PARAM_BUFFER_SIZE)->min);
	a->period = pl.period; a->bufsz = pl.bufsz; a->target = pl.target;
	a->avail_min = pl.avail_min; a->start_threshold = pl.start_threshold;

	memset(&sw, 0, sizeof sw);
	sw.proto = SNDRV_PCM_VERSION;
	sw.period_step = 1;
	sw.xfer_align = 1;
	sw.avail_min = pl.avail_min;
	sw.start_threshold = pl.start_threshold;
	sw.stop_threshold = pl.bufsz;
	if (ioctl(a->fd, SNDRV_PCM_IOCTL_SW_PARAMS, &sw) < 0) return -1;
	if (ioctl(a->fd, SNDRV_PCM_IOCTL_PREPARE) < 0) return -1;
	return 0;
}

static void queue_reserve(struct audio *a, size_t more)
{
	if (a->qn + more > a->qcap) {
		size_t nc = (a->qn + more) * 2 + 1024;
		int16_t *nq = realloc(a->q, nc * 4);
		if (!nq) return;
		a->q = nq; a->qcap = nc;
	}
}

int audio_open(struct audio *a, unsigned core_rate, const char *dev, int mode_null,
	       double period_ms, double latency_ms, unsigned target_frames, volatile int *quit)
{
	static const char *probe[] = { "/dev/snd/pcmC0D0p", "/dev/snd/pcmC1D0p" };
	int i;

	memset(a, 0, sizeof *a);
	a->fd = -1; a->core_rate = core_rate; a->rate = core_rate; a->quit = quit;
	a->fill_min = UINT_MAX;
	a->last_fill = -1;
	a->req_period_ms = period_ms; a->req_latency_ms = latency_ms; a->req_target = target_frames;
	a->target = target_frames ? target_frames : (unsigned)(core_rate * latency_ms / 1000.0 + 0.5);
	if (mode_null) {
		a->mode = AUD_NULL;
		fprintf(stderr, "olpc-run: audio: null sink (--no-audio), core rate %u Hz\n", core_rate);
		return 0;
	}
	for (i = 0; i < (dev ? 1 : NELEM(probe)) && a->fd < 0; i++) {
		const char *p = dev ? dev : probe[i];
		a->fd = open(p, O_RDWR | O_CLOEXEC);
		if (a->fd >= 0) { snprintf(a->dev, sizeof a->dev, "%s", p); fprintf(stderr, "olpc-run: audio: device %s\n", p); }
	}
	if (a->fd >= 0) {
		/* the card's clock is the master: ask for the core's own rate first, else 48000 + resample */
		if (alsa_setup(a, core_rate, period_ms, latency_ms, target_frames) == 0) {
			a->resample = 0;
		} else {
			fprintf(stderr, "olpc-run: audio: hw_params refused %u Hz, trying 48000 with resampling\n", core_rate);
			ioctl(a->fd, SNDRV_PCM_IOCTL_HW_FREE);
			if (alsa_setup(a, 48000, period_ms, latency_ms, target_frames) < 0) {
				fprintf(stderr, "olpc-run: audio: 48000 Hz refused too (errno %d), using the clock fallback\n", errno);
				close(a->fd); a->fd = -1;
			}
		}
	} else {
		fprintf(stderr, "olpc-run: audio: no PCM device (%s), using the clock fallback\n", strerror(errno));
	}
	if (a->fd >= 0) {
		a->mode = AUD_ALSA;
		a->zeros = calloc(a->bufsz, 4);        /* NULL: the restart after an underrun just skips the silence */
		a->resample = a->rate != core_rate;
	} else {
		a->mode = AUD_CLOCK;
		a->rate = core_rate;
		a->resample = 0;
		a->bufsz = a->target * 2;
		a->period = a->target / 2;
		a->avail_min = a->start_threshold = 0;
		a->t0 = now_ns();
	}
	if (a->resample) rs_init(&a->rs, core_rate, a->rate);
	queue_reserve(a, 8192);                /* no allocation in the loop (a malloc can take the mmap lock) */
	fprintf(stderr, "olpc-run: audio: mode=%s core_rate=%u device_rate=%u resample=%d period=%u buffer=%u target_fill=%u avail_min=%u start_threshold=%u (%.2f ms period, %.1f ms target)\n",
		a->mode == AUD_ALSA ? "alsa" : "clock", core_rate, a->rate, a->resample, a->period, a->bufsz, a->target,
		a->avail_min, a->start_threshold, 1000.0 * a->period / a->rate, 1000.0 * a->target / a->rate);
	return 0;
}

void audio_push(struct audio *a, const int16_t *frames, size_t n)
{
	if (a->mode == AUD_NULL || !n) return;
	if (a->resample) {
		queue_reserve(a, rs_max_out(&a->rs, n));
		if (a->qn + rs_max_out(&a->rs, n) > a->qcap) return;
		a->qn += rs_process(&a->rs, frames, n, a->q + 2 * a->qn, a->qcap - a->qn);
	} else {
		queue_reserve(a, n);
		if (a->qn + n > a->qcap) return;
		memcpy(a->q + 2 * a->qn, frames, n * 4);
		a->qn += n;
	}
}

/* silence up to the target fill: what the caller still has queued goes on top of it */
static void prime_silence(struct audio *a)
{
	size_t n = a->target > a->qn ? a->target - a->qn : 0;

	if (n > a->bufsz) n = a->bufsz;
	if (n && a->zeros) {
		struct snd_xferi x;

		x.result = 0; x.buf = a->zeros; x.frames = (snd_pcm_uframes_t)n;
		if (ioctl(a->fd, SNDRV_PCM_IOCTL_WRITEI_FRAMES, &x) < 0) a->recover_fail++;   /* not fatal: the data follows */
	}
}

/* Close and reopen the PCM: a full hw_free/hw_params, which also rebuilds the driver's
 * descriptor list.  The device rate must come back the same (the resampler depends on it). */
static int audio_reopen(struct audio *a)
{
	unsigned rate = a->rate;
	int16_t *z;

	ioctl(a->fd, SNDRV_PCM_IOCTL_DROP);
	ioctl(a->fd, SNDRV_PCM_IOCTL_HW_FREE);
	close(a->fd);
	a->fd = open(a->dev, O_RDWR | O_CLOEXEC);
	if (a->fd < 0) return -1;
	if (alsa_setup(a, rate, a->req_period_ms, a->req_latency_ms, a->req_target) < 0 || a->rate != rate) return -1;
	z = realloc(a->zeros, (size_t)a->bufsz * 4);
	if (!z) return -1;
	a->zeros = z;
	memset(a->zeros, 0, (size_t)a->bufsz * 4);
	prime_silence(a);
	return 0;
}

/* Underrun.  The policy (diag.c recov_underrun): the first one in an incident gets DROP +
 * PREPARE + silence up to the target, a second one while the incident is open reopens the
 * PCM, a third gives up on the card.  0 on success, -1 when the card must be abandoned. */
static int xrun_recover(struct audio *a, int via)
{
	int act;

	a->underruns++;
	if (via == XR_VIA_POLL) a->underruns_poll++; else a->underruns_write++;
	a->last_via = via;
	if (!a->rc.level) a->incident_no++;
	act = recov_underrun(&a->rc, a->nflush, now_ns());
	a->last_action = act;
	a->n_action[act]++;
	if (act == RA_CLOCK) return -1;
	if (act == RA_REOPEN) {
		dlog("olpc-run: audio: underruns in a row at frame %lu: reopening the PCM\n", a->nflush);
		if (audio_reopen(a) < 0) { a->reopen_failed++; a->recover_fail++; return -1; }
		return 0;
	}
	ioctl(a->fd, SNDRV_PCM_IOCTL_DROP);
	if (ioctl(a->fd, SNDRV_PCM_IOCTL_PREPARE) < 0) { a->recover_fail++; return -1; }
	prime_silence(a);
	return 0;
}

long audio_fill(struct audio *a)
{
	if (a->mode == AUD_ALSA) {
		snd_pcm_sframes_t d = 0;
		if (a->no_query) return -1;
		if (ioctl(a->fd, SNDRV_PCM_IOCTL_DELAY, &d) < 0) d = 0;   /* XRUN: empty */
		a->last_fill = d < 0 ? 0 : (long)d;
		return a->last_fill;
	}
	if (a->mode == AUD_CLOCK) {
		long long played = (long long)((now_ns() - a->t0) * a->rate / 1000000000ull);
		long long fill = (long long)a->written - played;
		return fill < 0 ? 0 : (long)fill;
	}
	return -1;
}

static unsigned fill_now(struct audio *a)
{
	long f = audio_fill(a);

	return f < 0 ? 0 : (unsigned)f;
}

int audio_xrun_debug(struct audio *a, unsigned n)
{
	char val[16];
	const char *b = strrchr(a->dev, '/');
	int card, dev, fd, len, ok;

	a->xd_errno = 0; a->xd_path[0] = 0;
	if (a->mode != AUD_ALSA || !b || sscanf(b + 1, "pcmC%dD%dp", &card, &dev) != 2) { a->xd_errno = ENODEV; return -1; }
	snprintf(a->xd_path, sizeof a->xd_path, "/proc/asound/card%d/pcm%dp/xrun_debug", card, dev);
	fd = open(a->xd_path, O_WRONLY | O_CLOEXEC);
	if (fd < 0) {
		a->xd_errno = errno;
		fprintf(stderr, "olpc-run: audio: %s: %s\n", a->xd_path, strerror(errno));
		return -1;
	}
	len = snprintf(val, sizeof val, "%u\n", n);
	ok = write(fd, val, (size_t)len) == len;
	if (!ok) a->xd_errno = errno ? errno : EIO;
	close(fd);                                  /* the text entry parses its buffer on close */
	fprintf(stderr, "olpc-run: audio: xrun_debug=%u %s (%s)\n", n, ok ? "set" : "refused", a->xd_path);
	return ok ? 0 : -1;
}

/* the PCM is unusable: pace against the clock instead */
static void to_clock(struct audio *a, const char *why)
{
	dlog("olpc-run: audio: %s, switching to the clock fallback\n", why);
	close(a->fd); a->fd = -1;
	a->mode = AUD_CLOCK; a->t0 = now_ns(); a->written = 0;
}

/* Recovery pacing: never run frames back to back while an incident is open.  Sleep until
 * the frame deadline (nominal frame period after the previous one). */
static void pace_wait(struct audio *a)
{
	uint64_t until = pace_deadline(&a->pace_next, now_ns(), a->frame_ns);
	struct timespec ts = { (time_t)(until / 1000000000ull), (long)(until % 1000000000ull) };

	a->paced_frames++;
	while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR && !*a->quit) {}
}

static void flush_alsa(struct audio *a)
{
	struct pollfd pfd = { a->fd, POLLOUT, 0 };
	size_t off = 0;
	int recov = 0;
	unsigned long long xr0 = a->underruns;
	uint64_t t_poll;

	a->nflush++;
	if (a->frame_ns && recov_pacing(&a->rc)) pace_wait(a);
	else a->pace_next = 0;
	/* pacing: sleep (in the kernel) until the fill is down to the target */
	t_poll = now_ns();
	for (;;) {
		int r = poll(&pfd, 1, 250);
		a->poll_ns = now_ns() - t_poll;
		if (*a->quit) { a->qn = 0; return; }
		if (r < 0) { if (errno == EINTR) continue; break; }
		if (r == 0) { a->stalls++; break; }
		if ((pfd.revents & POLLERR) && xrun_recover(a, XR_VIA_POLL) < 0) { to_clock(a, "PREPARE failed"); return; }
		break;
	}
	if (a->nflush > 30 && !a->no_query) {   /* the first frames fill an empty buffer */
		unsigned f = fill_now(a);
		if (f < a->fill_min) a->fill_min = f;
	}
	while (off < a->qn) {
		struct snd_xferi x;

		x.result = 0; x.buf = a->q + 2 * off; x.frames = (snd_pcm_uframes_t)(a->qn - off);
		if (ioctl(a->fd, SNDRV_PCM_IOCTL_WRITEI_FRAMES, &x) < 0) {
			if (x.result > 0) { off += (size_t)x.result; a->written += (unsigned long long)x.result; }
			if (errno == EINTR) { if (*a->quit) break; continue; }
			if (errno == EPIPE && ++recov <= 3) {      /* a device that keeps failing must not loop here */
				if (xrun_recover(a, XR_VIA_WRITE) == 0) continue;
				to_clock(a, "PREPARE failed");
				break;
			}
			{
				char why[80];
				snprintf(why, sizeof why, "write failed (%s)", strerror(errno));
				to_clock(a, why);
			}
			break;
		}
		off += (size_t)x.result;
		a->written += (unsigned long long)x.result;
	}
	a->qn = 0;
	if (a->mode == AUD_ALSA) {
		int uf = a->underruns != xr0;
		if (recov_frame_end(&a->rc, a->nflush, now_ns(), uf, !uf && a->poll_ns >= RC_WAIT_NS)) {
			a->incident_done++;
			a->inc_frames = (long)a->rc.done_frames;
			a->inc_ms = a->rc.done_ms;
		}
	}
	if (a->mode == AUD_ALSA && a->nflush > 30 && !a->no_query) {
		unsigned f = fill_now(a);
		if (f > a->fill_max) a->fill_max = f;
	}
}

static void flush_clock(struct audio *a)
{
	/* the virtual device plays one frame every 1/rate s; stay `target` frames ahead of it */
	uint64_t ahead_ns, now = now_ns();

	a->written += a->qn;
	a->qn = 0;
	ahead_ns = (uint64_t)a->written * 1000000000ull / a->rate;
	if (ahead_ns > now - a->t0 + (uint64_t)a->target * 1000000000ull / a->rate) {
		uint64_t until = a->t0 + ahead_ns - (uint64_t)a->target * 1000000000ull / a->rate;
		struct timespec ts = { (time_t)(until / 1000000000ull), (long)(until % 1000000000ull) };
		/* the 100 Hz kernel tick makes this late by a few ms on the XO-1; this
		 * path is only a fallback when there is no sound card */
		while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR && !*a->quit) {}
	} else if (now - a->t0 > ahead_ns + 500000000ull) {
		/* more than 0.5 s behind: resynchronise instead of racing to catch up */
		a->t0 = now - ahead_ns;
	}
}

void audio_flush(struct audio *a)
{
	if (a->mode == AUD_ALSA) flush_alsa(a);
	else if (a->mode == AUD_CLOCK) flush_clock(a);
	else a->qn = 0;
}

void audio_close(struct audio *a)
{
	if (a->mode == AUD_ALSA && a->fd >= 0) {
		ioctl(a->fd, SNDRV_PCM_IOCTL_DROP);
		ioctl(a->fd, SNDRV_PCM_IOCTL_HW_FREE);
		close(a->fd);
	}
	free(a->q);
	free(a->zeros);
	a->q = NULL; a->zeros = NULL; a->fd = -1;
}
