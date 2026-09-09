/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LINUX_KALLSYMS_INTERNAL_H_
#define LINUX_KALLSYMS_INTERNAL_H_

#include <linux/types.h>

extern const int kallsyms_offsets[];
extern const u8 kallsyms_names[];

extern const unsigned int kallsyms_num_syms;

extern const char kallsyms_token_table[];
extern const u16 kallsyms_token_index[];

extern const unsigned int kallsyms_markers[];
extern const u8 kallsyms_seqs_of_names[];

unsigned long kallsyms_addr_pos_bsearch(unsigned long addr);

/* Position of the last symbol whose address is <= @addr. */
static inline unsigned long kallsyms_addr_pos(unsigned long addr)
{
	return kallsyms_addr_pos_bsearch(addr);
}

#endif // LINUX_KALLSYMS_INTERNAL_H_
