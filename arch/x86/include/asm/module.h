/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_MODULE_H
#define _ASM_X86_MODULE_H

#include <asm-generic/module.h>
#include <asm/orc_types.h>

struct its_array {
#ifdef CONFIG_MITIGATION_ITS
	void **pages;
	int num;
#endif
};

struct mod_arch_specific {
#ifdef CONFIG_UNWINDER_ORC
	unsigned int num_orcs;
	int *orc_unwind_ip;
	/* Block lookup table over the core text, see unwind_orc.c. */
	unsigned int *orc_lookup;
	unsigned int orc_lookup_first;	/* first ORC entry in the core text */
	unsigned long orc_lookup_start;	/* core text start */
	unsigned long orc_lookup_stop;
	struct orc_entry *orc_unwind;
#endif
	struct its_array its_pages;
};

#endif /* _ASM_X86_MODULE_H */
