#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include "saver.h"
#include "diag.h"

#define LD(p) __atomic_load_n(&(p), __ATOMIC_ACQUIRE)
#define ST(p, v) __atomic_store_n(&(p), (v), __ATOMIC_RELEASE)

int sv_write_atomic(const char *path, const uint8_t *d, size_t n)
{
	char tmp[PATH_MAX + 8], dir[PATH_MAX];
	int fd, dfd;
	size_t off = 0;

	snprintf(tmp, sizeof tmp, "%s.tmp", path);
	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd < 0) return -1;
	while (off < n) {
		ssize_t w = write(fd, d + off, n - off);
		if (w < 0 && errno == EINTR) continue;
		if (w <= 0) { close(fd); unlink(tmp); return -1; }
		off += (size_t)w;
	}
	if (fsync(fd) < 0 || close(fd) < 0 || rename(tmp, path) < 0) { unlink(tmp); return -1; }
	snprintf(dir, sizeof dir, "%s", path);
	dfd = open(dirname(dir), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (dfd >= 0) { fsync(dfd); close(dfd); }
	return 0;
}


/* Wait on cv until pred-like timeout; absolute CLOCK_REALTIME deadline for the condvar. */
static void abs_after_ms(struct timespec *ts, unsigned ms)
{
	clock_gettime(CLOCK_REALTIME, ts);
	ts->tv_sec += ms / 1000;
	ts->tv_nsec += (long)(ms % 1000) * 1000000L;
	if (ts->tv_nsec >= 1000000000L) { ts->tv_sec++; ts->tv_nsec -= 1000000000L; }
}

static void *worker(void *arg)
{
	struct saver *s = arg;
	struct sched_param sp;
	int i;

	/* never SCHED_FIFO, whatever the creator was: a raw syscall (musl's wrapper is a stub) */
	memset(&sp, 0, sizeof sp);
	syscall(SYS_sched_setscheduler, 0, SCHED_OTHER, &sp);
	setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 10);

	pthread_mutex_lock(&s->mu);
	for (;;) {
		struct sv_slot *job = NULL;

		for (i = 0; i < s->n; i++) if (LD(s->slot[i].state) == SV_QUEUED) { job = &s->slot[i]; break; }
		if (!job) {
			if (s->stop) break;
			pthread_cond_wait(&s->wake, &s->mu);
			continue;
		}
		ST(job->state, SV_WRITING);
		pthread_mutex_unlock(&s->mu);
		if (s->test_write_delay_ms) usleep((useconds_t)s->test_write_delay_ms * 1000);
		if (sv_write_atomic(job->path, job->snap, job->size) == 0) {
			job->written++;
			fprintf(stderr, "olpc-run: saves: wrote %s (%s)\n", job->path, job->why ? job->why : "?");
			ST(job->state, SV_OK);
		} else {
			int e = errno;
			job->failed++;
			fprintf(stderr, "olpc-run: saves: write of %s failed: %s\n", job->path, strerror(e));
			ST(job->state, SV_FAIL);
		}
		pthread_mutex_lock(&s->mu);
		pthread_cond_broadcast(&s->done);
	}
	pthread_mutex_unlock(&s->mu);
	return NULL;
}

int sv_start(struct saver *s)
{
	pthread_attr_t at;
	sigset_t all, old;
	int r;

	pthread_mutex_init(&s->mu, NULL);
	pthread_cond_init(&s->wake, NULL);
	pthread_cond_init(&s->done, NULL);
	pthread_attr_init(&at);
	pthread_attr_setstacksize(&at, 128 * 1024);
	pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
	sigfillset(&all);
	pthread_sigmask(SIG_BLOCK, &all, &old);          /* the helper must not take SIGALRM/SIGTERM */
	r = pthread_create(&s->th, &at, worker, s);
	pthread_sigmask(SIG_SETMASK, &old, NULL);
	pthread_attr_destroy(&at);
	s->started = r == 0;
	return r == 0 ? 0 : -1;
}

int sv_register(struct saver *s, const char *path, uint8_t *ptr, size_t size)
{
	struct sv_slot *v;

	if (s->n >= SV_MAX || !ptr || !size) return -1;
	v = &s->slot[s->n];
	memset(v, 0, sizeof *v);
	snprintf(v->path, sizeof v->path, "%s", path);
	v->ptr = ptr; v->size = size;
	v->last = malloc(size); v->snap = malloc(size); v->fin = malloc(size);
	if (!v->last || !v->snap || !v->fin) { free(v->last); free(v->snap); free(v->fin); return -1; }
	memcpy(v->last, ptr, size);
	v->state = SV_IDLE;
	s->n++;
	return 0;
}

/* fold a finished write into the main thread's view */
static void reap(struct sv_slot *v)
{
	int st = LD(v->state);

	if (st == SV_OK) { memcpy(v->last, v->snap, v->size); ST(v->state, SV_IDLE); }
	else if (st == SV_FAIL) ST(v->state, SV_IDLE);          /* last stays: the next check retries */
}

int sv_check(struct saver *s, const char *why)
{
	int i, left = 0;

	for (i = 0; i < s->n; i++) {
		struct sv_slot *v = &s->slot[i];

		reap(v);
		if (LD(v->state) != SV_IDLE) { left++; continue; }
		if (!memcmp(v->last, v->ptr, v->size)) continue;
		if (pthread_mutex_trylock(&s->mu) != 0) { left++; continue; }
		memcpy(v->snap, v->ptr, v->size);
		v->why = why;
		ST(v->state, SV_QUEUED);
		pthread_cond_signal(&s->wake);
		pthread_mutex_unlock(&s->mu);
	}
	return left;
}

void sv_capture_final(struct saver *s)
{
	int i;

	for (i = 0; i < s->n; i++) {
		memcpy(s->slot[i].fin, s->slot[i].ptr, s->slot[i].size);
		s->slot[i].fin_valid = 1;
	}
}

/* wait (mu held) until the slot is not queued/writing, or the deadline passes; 1 when settled */
static int settle(struct saver *s, struct sv_slot *v, const struct timespec *dl)
{
	while (LD(v->state) == SV_QUEUED || LD(v->state) == SV_WRITING)
		if (pthread_cond_timedwait(&s->done, &s->mu, dl) != 0) break;
	return LD(v->state) != SV_QUEUED && LD(v->state) != SV_WRITING;
}

int sv_wait(struct saver *s, unsigned timeout_ms)
{
	struct timespec dl;
	int i, unfinished = 0;

	abs_after_ms(&dl, timeout_ms);
	for (i = 0; i < s->n; i++) {
		struct sv_slot *v = &s->slot[i];

		/* the helper holds mu only briefly, never across I/O: a plain lock is fine here */
		pthread_mutex_lock(&s->mu);
		if (!settle(s, v, &dl)) { pthread_mutex_unlock(&s->mu); unfinished++; continue; }
		reap(v);
		if (v->fin_valid && memcmp(v->last, v->fin, v->size)) {
			memcpy(v->snap, v->fin, v->size);
			v->why = "exit";
			ST(v->state, SV_QUEUED);
			pthread_cond_signal(&s->wake);
			if (!settle(s, v, &dl)) { pthread_mutex_unlock(&s->mu); unfinished++; continue; }
			if (LD(v->state) == SV_FAIL) unfinished++;
			reap(v);
		} else if (!v->fin_valid && memcmp(v->last, v->ptr, v->size)) {
			unfinished++;              /* never captured: nothing safe to write */
		}
		pthread_mutex_unlock(&s->mu);
	}
	return unfinished;
}
