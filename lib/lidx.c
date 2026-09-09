// SPDX-License-Identifier: GPL-2.0-only
/*
 * Construction of a predicted-position index. See linux/lidx.h.
 */
#include <linux/lidx.h>
#include <linux/errno.h>
#include <linux/export.h>
#include <linux/log2.h>
#include <linux/minmax.h>

static void lidx_fit_endpoint(struct lidx_bucket *b, unsigned long bstart,
			      unsigned long cnt, lidx_key_fn key,
			      const void *ctx)
{
	unsigned long x0 = key(b->idx0, ctx) - bstart;
	unsigned long x1 = key(b->idx0 + cnt - 1, ctx) - bstart;

	b->slope = 0;
	b->icept = 0;
	if (cnt < 2 || x1 <= x0)
		return;
	b->slope = div64_u64((u64)(cnt - 1) << 32, x1 - x0);
	b->icept = -(s32)mul_u64_u64_shr(x0, b->slope, 32);
}

#ifdef CONFIG_ARCH_SUPPORTS_INT128
/* Integer least squares over the bucket's keys; false if degenerate. */
static bool lidx_fit_lsq(struct lidx_bucket *b, unsigned long bstart,
			 unsigned long cnt, lidx_key_fn key, const void *ctx)
{
	unsigned __int128 sxx = 0, den;
	__int128 sxy = 0, num;
	u64 sx = 0, sy = 0, x, slope;
	unsigned long i;
	s64 icept;

	if (cnt < 2)
		return false;
	for (i = 0; i < cnt; i++) {
		x = key(b->idx0 + i, ctx) - bstart;
		sx += x;
		sy += i;
		sxx += (unsigned __int128)x * x;
		sxy += (__int128)x * i;
	}
	num = (__int128)cnt * sxy - (__int128)sx * sy;
	den = (unsigned __int128)cnt * sxx - (unsigned __int128)sx * sx;
	if (num <= 0 || !den)
		return false;
	/* Scale both into 62 bits; only the ratio matters. */
	while (num >= ((__int128)1 << 62) || den >= ((unsigned __int128)1 << 62)) {
		num >>= 1;
		den >>= 1;
	}
	if (!den)
		return false;
	slope = mul_u64_u64_div_u64((u64)num, 1ULL << 32, (u64)den);
	icept = div_s64((s64)sy - (s64)mul_u64_u64_shr(sx, slope, 32),
			(s64)cnt);
	if (icept < S32_MIN || icept > S32_MAX)
		return false;
	b->slope = slope;
	b->icept = icept;
	return true;
}
#else
static bool lidx_fit_lsq(struct lidx_bucket *b, unsigned long bstart,
			 unsigned long cnt, lidx_key_fn key, const void *ctx)
{
	return false;
}
#endif

/*
 * Largest |prediction - position| over the bucket's keys and its two
 * edges. The lower edge maps to lo and the upper edge to hi, the answers
 * for a key before the first or after the last key in the bucket.
 */
static u32 lidx_bucket_err(const struct lidx_bucket *b, unsigned long bstart,
			   unsigned long bend, unsigned long lo,
			   unsigned long hi, lidx_key_fn key, const void *ctx)
{
	unsigned long i, p, x, err = 0, e;

	if (b->idx0) {
		p = lidx_predict(b, 0, lo, hi);
		e = p > lo ? p - lo : lo - p;
		err = max(err, e);
	}
	for (i = b->idx0; i <= hi; i++) {
		x = key(i, ctx) - bstart;
		p = lidx_predict(b, x, lo, hi);
		e = p > i ? p - i : i - p;
		err = max(err, e);
	}
	p = lidx_predict(b, bend - bstart, lo, hi);
	e = p > hi ? p - hi : hi - p;
	err = max(err, e);
	return min_t(unsigned long, err, U32_MAX);
}

/**
 * lidx_build - build a predicted-position index over a sorted table
 * @m: the model to fill in
 * @buckets: storage for @nbuckets + 1 buckets
 * @nbuckets: number of buckets, a power of two
 * @n: number of table positions, at least 1
 * @key: returns the key at a position; keys must be non-decreasing
 * @ctx: passed to @key
 *
 * Returns 0, or -EINVAL for an empty table, a non-power-of-two bucket
 * count or a table whose last key is below its first.
 */
int lidx_build(struct lidx_model *m, struct lidx_bucket *buckets,
	       unsigned int nbuckets, unsigned long n, lidx_key_fn key,
	       const void *ctx)
{
	unsigned long base, last, span, i, s, bstart, bend, lo, hi, cnt;
	unsigned int shift = 0;
	struct lidx_bucket *b;

	if (!n || !nbuckets || !is_power_of_2(nbuckets))
		return -EINVAL;
	base = key(0, ctx);
	last = key(n - 1, ctx);
	if (last < base)
		return -EINVAL;
	span = last - base;
	while ((span >> shift) >= nbuckets)
		shift++;

	m->base = base;
	m->n = n;
	m->shift = shift;
	m->nbuckets = nbuckets;
	m->b = buckets;
	m->max_err = 0;

	i = 0;
	for (s = 0; s < nbuckets; s++) {
		bstart = base + (s << shift);
		while (i < n && key(i, ctx) < bstart)
			i++;
		buckets[s].idx0 = i;
	}
	buckets[nbuckets].idx0 = n;

	for (s = 0; s < nbuckets; s++) {
		b = &buckets[s];
		bstart = base + (s << shift);
		bend = bstart + (1UL << shift) - 1;
		lo = b->idx0 ? b->idx0 - 1 : 0;
		hi = buckets[s + 1].idx0 - 1;
		cnt = buckets[s + 1].idx0 - b->idx0;
		b->slope = 0;
		b->icept = 0;
		b->err = 0;
		if (!cnt)
			continue;
		if (!lidx_fit_lsq(b, bstart, cnt, key, ctx))
			lidx_fit_endpoint(b, bstart, cnt, key, ctx);
		b->err = lidx_bucket_err(b, bstart, bend, lo, hi, key, ctx);
		m->max_err = max(m->max_err, b->err);
	}
	return 0;
}
EXPORT_SYMBOL_GPL(lidx_build);
