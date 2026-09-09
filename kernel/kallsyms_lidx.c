// SPDX-License-Identifier: GPL-2.0-only
/*
 * kallsyms_lidx.c: predicted-position search of the kallsyms address table.
 *
 * The kallsyms address table is sorted and immutable once the kernel is
 * linked, so the position of the last symbol at or below an address is a
 * monotone function of the address. A small model of that function predicts
 * a position, which is then proved correct against the neighbouring table
 * entries or corrected with a bounded search. Nothing here changes what a
 * lookup returns; it only changes how many table entries are read on the
 * way to the answer.
 *
 * The model is a directory of equal-width address buckets. Each bucket
 * records the first symbol at or above its start, a fixed-point line
 * predicting the position from the offset into the bucket, and the largest
 * error that line makes on any symbol in the bucket or at its edges. The
 * line is monotone, so any address in the bucket has its predecessor within
 * that error of the prediction, and a binary search over that window is
 * exact. The window edges are checked before they are trusted; a window
 * that does not bracket the answer falls back to the original binary
 * search, so a wrong model cannot change a result.
 *
 * All arithmetic is integer. The model is built once at early init from the
 * live table into static storage, so the lookup path allocates nothing,
 * takes no locks and is usable from NMI context like the binary search.
 */
#define pr_fmt(fmt) "kallsyms_lidx: " fmt

#include <linux/kallsyms.h>
#include <linux/lidx.h>
#include <linux/init.h>
#include <linux/jump_label.h>
#include <linux/minmax.h>
#include <linux/moduleparam.h>

#include "kallsyms_internal.h"

/* Parameters live under kallsyms., not under the object name. */
#ifdef MODULE_PARAM_PREFIX
#undef MODULE_PARAM_PREFIX
#endif
#define MODULE_PARAM_PREFIX "kallsyms."

/*
 * Last position in [lo, hi] whose address is <= @addr, given that the
 * address at @lo is <= @addr or @lo is 0. This is the original binary
 * search restricted to a range.
 */
static __always_inline unsigned long
lidx_bsearch_range(unsigned long lo, unsigned long hi, unsigned long addr,
		   unsigned int *steps)
{
	unsigned long low = lo, high = hi + 1, mid;

	while (high - low > 1) {
		mid = low + (high - low) / 2;
		if (steps)
			(*steps)++;
		if (__kallsyms_sym_address(mid) <= addr)
			low = mid;
		else
			high = mid;
	}

	return low;
}

/*
 * The predecessor of @addr. Every position in the bucket's [lo, hi] is a
 * valid candidate: the symbol at lo is below the bucket start (or lo is 0)
 * and the symbol at hi + 1, if any, is at or above the next bucket start.
 */
static __always_inline unsigned long
lidx_search(const struct lidx_model *m, unsigned long addr,
	    struct kallsyms_lidx_result *r, bool count)
{
	const struct lidx_bucket *b;
	unsigned long bstart, lo, hi, g, wlo, whi, n = kallsyms_num_syms;
	unsigned int *steps = count ? &r->steps : NULL;

	if (addr < m->base)
		return 0;
	b = lidx_bucket(m, addr, &bstart, &lo, &hi);
	g = lidx_predict(b, addr - bstart, lo, hi);

	if (steps)
		*steps += 2;
	if (__kallsyms_sym_address(g) <= addr &&
	    (g == n - 1 || addr < __kallsyms_sym_address(g + 1))) {
		r->outcome = KALLSYMS_LIDX_HIT;
		return g;
	}

	wlo = g > lo + b->err + 1 ? g - b->err - 1 : lo;
	whi = min(hi, g + b->err + 1);
	if (steps)
		*steps += 2;
	if (__kallsyms_sym_address(wlo) > addr && wlo)
		goto fallback;
	if (whi < n - 1 && __kallsyms_sym_address(whi + 1) <= addr)
		goto fallback;
	r->outcome = KALLSYMS_LIDX_WINDOW;
	return lidx_bsearch_range(wlo, whi, addr, steps);

fallback:
	r->outcome = KALLSYMS_LIDX_FALLBACK;
	return lidx_bsearch_range(0, n - 1, addr, steps);
}

/* Test entry point: the same search, reporting the outcome and reads. */
unsigned long kallsyms_lidx_test_search(const struct lidx_model *m,
					unsigned long addr,
					struct kallsyms_lidx_result *r)
{
	r->outcome = KALLSYMS_LIDX_HIT;
	r->steps = 0;
	return lidx_search(m, addr, r, true);
}

/*
 * Model construction.
 */
static unsigned long kallsyms_lidx_key(unsigned long pos, const void *ctx)
{
	return __kallsyms_sym_address(pos);
}

int kallsyms_lidx_build(struct lidx_model *m, struct lidx_bucket *buckets)
{
	return lidx_build(m, buckets, KALLSYMS_LIDX_BUCKETS, kallsyms_num_syms,
			  kallsyms_lidx_key, NULL);
}

/*
 * The production model and dispatch.
 */
static struct lidx_bucket lidx_buckets[KALLSYMS_LIDX_BUCKETS + 1];
static struct lidx_model lidx_model __ro_after_init;
static DEFINE_STATIC_KEY_FALSE(lidx_ready);

static bool lidx_enable __initdata = true;
module_param_named(learned_index, lidx_enable, bool, 0);
MODULE_PARM_DESC(learned_index,
		 "Use the predicted-position search for address lookups (default on)");

unsigned long kallsyms_addr_pos(unsigned long addr)
{
	struct kallsyms_lidx_result r;

	if (static_branch_likely(&lidx_ready))
		return lidx_search(&lidx_model, addr, &r, false);
	return kallsyms_addr_pos_bsearch(addr);
}

static int __init kallsyms_lidx_init(void)
{
	int ret;

	if (!lidx_enable)
		return 0;
	ret = kallsyms_lidx_build(&lidx_model, lidx_buckets);
	if (ret) {
		pr_warn("model build failed (%d), keeping the binary search\n",
			ret);
		return 0;
	}
	pr_info("%u buckets of %lu bytes, max error %u, %zu bytes over %u symbols\n",
		KALLSYMS_LIDX_BUCKETS, 1UL << lidx_model.shift,
		lidx_model.max_err, sizeof(lidx_buckets), kallsyms_num_syms);

	/*
	 * Patching the branch after the model is written publishes it: a
	 * CPU that runs the patched instruction has passed the patching
	 * synchronization and sees the stores made before it.
	 */
	static_branch_enable(&lidx_ready);
	return 0;
}
early_initcall(kallsyms_lidx_init);
