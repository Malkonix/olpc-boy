#include "resample.h"

void rs_init(struct resampler *r, unsigned in_rate, unsigned out_rate)
{
	r->step = ((uint64_t)in_rate << 32) / out_rate;
	r->pos = 0;
	r->pl = r->pr = 0;
}

size_t rs_max_out(const struct resampler *r, size_t nin)
{
	return (size_t)(((uint64_t)nin << 32) / r->step) + 2;
}

size_t rs_process(struct resampler *r, const int16_t *in, size_t nin, int16_t *out, size_t outcap)
{
	size_t n = 0;
	uint64_t t = r->pos, end = (uint64_t)nin << 32;

	/* output k sits at t between input[i-1] (prev when i == 0) and input[i] */
	while (t < end && n < outcap) {
		size_t i = (size_t)(t >> 32);
		int32_t f = (int32_t)((t >> 16) & 0xffff);
		int32_t al = i ? in[2 * (i - 1)] : r->pl, ar = i ? in[2 * (i - 1) + 1] : r->pr;
		int32_t bl = in[2 * i], br = in[2 * i + 1];

		out[2 * n]     = (int16_t)(al + (int32_t)(((int64_t)(bl - al) * f) >> 16));
		out[2 * n + 1] = (int16_t)(ar + (int32_t)(((int64_t)(br - ar) * f) >> 16));
		n++;
		t += r->step;
	}
	if (nin) {
		r->pl = in[2 * (nin - 1)];
		r->pr = in[2 * (nin - 1) + 1];
	}
	r->pos = t >= end ? t - end : 0;
	return n;
}
