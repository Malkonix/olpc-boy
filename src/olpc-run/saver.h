/* Battery saves written by a helper thread, so the emulation loop never blocks on storage.
 *
 * The main thread owns the live core memory and a private copy of what is on disk ("last").
 * When the live memory differs it copies it into a snapshot buffer and queues it; the helper
 * thread writes the snapshot (temp file, fsync, rename, fsync of the directory) at normal
 * priority.  The main thread never takes a lock that is held across I/O and never waits
 * (sv_check uses a try-lock); only sv_wait, at exit, waits, and only up to a timeout. */
#ifndef OLPC_RUN_SAVER_H
#define OLPC_RUN_SAVER_H
#include <limits.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#define SV_MAX 4

enum { SV_IDLE, SV_QUEUED, SV_WRITING, SV_OK, SV_FAIL };

struct sv_slot {
	char path[PATH_MAX];
	uint8_t *ptr;              /* live memory (core), read by the main thread only */
	size_t size;
	uint8_t *last;             /* contents known to be on disk; main thread only */
	uint8_t *snap;             /* handed to the helper while QUEUED/WRITING */
	uint8_t *fin;              /* copy taken at shutdown, before the core is unloaded */
	int fin_valid;
	int state;                 /* SV_*, accessed with __atomic builtins */
	const char *why;
	unsigned long written, failed;
};

struct saver {
	struct sv_slot slot[SV_MAX];
	int n;
	pthread_t th;
	pthread_mutex_t mu;
	pthread_cond_t wake, done;
	int started, stop;
	int test_write_delay_ms;   /* test hook: the helper sleeps this long before each write */
};

/* Start the helper thread (normal priority, all signals blocked, detached). 0 on success. */
int sv_start(struct saver *s);
/* Watch size bytes at ptr; the file is path.  Takes the current contents as the on-disk state. */
int sv_register(struct saver *s, const char *path, uint8_t *ptr, size_t size);
/* Periodic check: queue the slots whose memory changed.  Never blocks.  Returns the number of
 * slots that had to be left for a later call (helper busy, or its lock taken). */
int sv_check(struct saver *s, const char *why);
/* Shutdown, step 1 (core still loaded): private copy of the live memory. */
void sv_capture_final(struct saver *s);
/* Shutdown, step 2: write what changed and wait for the helper, at most timeout_ms in total.
 * Returns the number of slots whose last write did not finish (0 = everything is on disk or
 * nothing changed).  The helper is left behind if it is stuck; exit() ends it. */
int sv_wait(struct saver *s, unsigned timeout_ms);

/* temp file + fsync + rename; 0 on success */
int sv_write_atomic(const char *path, const uint8_t *d, size_t n);
#endif
