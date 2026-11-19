/* Streaming linear resampler for interleaved S16 stereo. */
#ifndef OLPC_RUN_RESAMPLE_H
#define OLPC_RUN_RESAMPLE_H
#include <stddef.h>
#include <stdint.h>

struct resampler {
	uint64_t step;        /* input frames per output frame, 32.32 */
	uint64_t pos;         /* position of the next output, in input frames after prev, 32.32 */
	int16_t pl, pr;       /* last input frame of the previous call */
};

void   rs_init(struct resampler *r, unsigned in_rate, unsigned out_rate);
/* Output capacity (frames) that is always enough for nin input frames. */
size_t rs_max_out(const struct resampler *r, size_t nin);
/* Consumes nin frames, writes at most outcap frames, returns the number written. */
size_t rs_process(struct resampler *r, const int16_t *in, size_t nin, int16_t *out, size_t outcap);
#endif
