/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Predicted-position index over a sorted, immutable table of integer keys.
 *
 * The table is divided into equal-width key buckets. Each bucket records
 * the first table position whose key is at or above the bucket start, a
 * fixed-point line predicting the position from the offset into the
 * bucket, and the largest error that line makes on any key in the bucket
 * or at its edges. The line is monotone, so the predecessor of any key in
 * the bucket lies within that error of the prediction.
 *
 * lidx_build() constructs the model from a key accessor; the search is
 * written by each user, because the certify-and-correct step reads the
 * user's own table. lidx_bucket() and lidx_predict() are the shared parts
 * of that search.
 */
#ifndef _LINUX_LIDX_H
#define _LINUX_LIDX_H

#include <linux/math64.h>
#include <linux/types.h>

struct lidx_bucket {
	u32 idx0;	/* first position at or above the bucket start */
	u32 err;	/* largest prediction error within the bucket */
	s32 icept;	/* integer intercept of the fitted line */
	u32 pad;
	u64 slope;	/* positions per key unit, Q32 fixed point */
};

struct lidx_model {
	unsigned long base;	/* key of position 0 */
	unsigned long n;	/* table positions */
	unsigned int shift;	/* log2 of the bucket width in key units */
	unsigned int nbuckets;	/* power of two */
	unsigned int max_err;
	struct lidx_bucket *b;	/* nbuckets + 1 entries */
};

typedef unsigned long (*lidx_key_fn)(unsigned long pos, const void *ctx);

int lidx_build(struct lidx_model *m, struct lidx_bucket *buckets,
	       unsigned int nbuckets, unsigned long n, lidx_key_fn key,
	       const void *ctx);

static inline size_t lidx_bytes(unsigned int nbuckets)
{
	return (nbuckets + 1) * sizeof(struct lidx_bucket);
}

/*
 * The bucket of @key and the range of positions its predecessor can occupy.
 * The caller has checked key >= m->base. Every position in [lo, hi] is a
 * valid candidate: position lo is below the bucket start (or is 0) and
 * position hi + 1, if any, is at or above the next bucket start.
 */
static __always_inline const struct lidx_bucket *
lidx_bucket(const struct lidx_model *m, unsigned long key,
	    unsigned long *bstart, unsigned long *lo, unsigned long *hi)
{
	unsigned long s = (key - m->base) >> m->shift;
	const struct lidx_bucket *b;

	if (s >= m->nbuckets)
		s = m->nbuckets - 1;
	b = &m->b[s];
	*bstart = m->base + (s << m->shift);
	*lo = b->idx0 ? b->idx0 - 1 : 0;
	*hi = m->b[s + 1].idx0 - 1;
	return b;
}

static __always_inline unsigned long
lidx_predict(const struct lidx_bucket *b, unsigned long x,
	     unsigned long lo, unsigned long hi)
{
	long g;

	g = (long)b->idx0 + (long)mul_u64_u64_shr(x, b->slope, 32) + b->icept;
	if (g < (long)lo)
		return lo;
	if (g > (long)hi)
		return hi;
	return g;
}

#endif /* _LINUX_LIDX_H */
