/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LINUX_KALLSYMS_INTERNAL_H_
#define LINUX_KALLSYMS_INTERNAL_H_

#include <linux/types.h>
#include <linux/compiler.h>
#include <linux/lidx.h>

extern const int kallsyms_offsets[];
extern const u8 kallsyms_names[];

extern const unsigned int kallsyms_num_syms;

extern const char kallsyms_token_table[];
extern const u16 kallsyms_token_index[];

extern const unsigned int kallsyms_markers[];
extern const u8 kallsyms_seqs_of_names[];

/*
 * Address of the symbol at table position @idx, for callers inside
 * kallsyms that read the table in a loop and must not pay a call per read.
 */
static __always_inline unsigned long __kallsyms_sym_address(int idx)
{
	/* non-relocatable 32-bit kernels just embed the value directly */
	if (!IS_ENABLED(CONFIG_64BIT) && !IS_ENABLED(CONFIG_RELOCATABLE))
		return (u32)kallsyms_offsets[idx];
	return (unsigned long)offset_to_ptr(kallsyms_offsets + idx);
}

unsigned long kallsyms_addr_pos_bsearch(unsigned long addr);

#ifdef CONFIG_KALLSYMS_LEARNED_INDEX

#define KALLSYMS_LIDX_BUCKETS	1024

enum kallsyms_lidx_outcome {
	KALLSYMS_LIDX_HIT,	/* the predicted position was the answer */
	KALLSYMS_LIDX_WINDOW,	/* found by the bounded correction search */
	KALLSYMS_LIDX_FALLBACK,	/* the bound was wrong; full binary search */
};

struct kallsyms_lidx_result {
	unsigned int outcome;	/* enum kallsyms_lidx_outcome */
	unsigned int steps;	/* table entries read */
};

unsigned long kallsyms_addr_pos(unsigned long addr);
int kallsyms_lidx_build(struct lidx_model *m, struct lidx_bucket *buckets);
unsigned long kallsyms_lidx_test_search(const struct lidx_model *m,
					unsigned long addr,
					struct kallsyms_lidx_result *r);

#else /* !CONFIG_KALLSYMS_LEARNED_INDEX */

/* Position of the last symbol whose address is <= @addr. */
static inline unsigned long kallsyms_addr_pos(unsigned long addr)
{
	return kallsyms_addr_pos_bsearch(addr);
}

#endif /* CONFIG_KALLSYMS_LEARNED_INDEX */

#endif // LINUX_KALLSYMS_INTERNAL_H_
