/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Per-mm VMA directory: a sorted snapshot of the address space with a
 * predicted-position index over VMA start addresses, valid while the mmap
 * lock sequence is unchanged. Answers the lockless fault-path VMA lookup
 * in a few cache lines for large, read-mostly address spaces; a stale
 * directory falls back to the Maple Tree walk and is rebuilt lazily.
 */
#ifndef _LINUX_VMA_DIR_H
#define _LINUX_VMA_DIR_H

#include <linux/jiffies.h>
#include <linux/jump_label.h>
#include <linux/mm_types.h>

#ifdef CONFIG_VMA_DIRECTORY
/* Address spaces with fewer VMAs than this never get a directory. */
#define VMA_DIR_MIN_VMAS	256
/* Stale lookups before a directory is rebuilt. */
#define VMA_DIR_STALE_LIMIT	1024
/* A directory stale sooner than this after its build was not worth it. */
#define VMA_DIR_SHORT_LIFE	(HZ / 10)
/* Longest backoff: rebuild after (1 + skip) * VMA_DIR_STALE_LIMIT lookups. */
#define VMA_DIR_MAX_SKIP	1023

DECLARE_STATIC_KEY_TRUE(vma_dir_enabled);

struct vm_area_struct *__vma_dir_lookup(struct mm_struct *mm,
					unsigned long addr);
void __vma_dir_maybe_rebuild(struct mm_struct *mm);
void vma_dir_free(struct mm_struct *mm);
void vma_dir_init(struct mm_struct *mm);

/*
 * The fault path calls these on every fault; with the directory switched
 * off they are a patched-out branch and nothing else.
 */
static inline struct vm_area_struct *vma_dir_lookup(struct mm_struct *mm,
						    unsigned long addr)
{
	if (!static_branch_likely(&vma_dir_enabled))
		return NULL;
	return __vma_dir_lookup(mm, addr);
}

static inline void vma_dir_maybe_rebuild(struct mm_struct *mm)
{
	if (static_branch_likely(&vma_dir_enabled) &&
	    READ_ONCE(mm->vma_dir_stale) >= VMA_DIR_STALE_LIMIT)
		__vma_dir_maybe_rebuild(mm);
}
#else
static inline struct vm_area_struct *vma_dir_lookup(struct mm_struct *mm,
						    unsigned long addr)
{
	return NULL;
}

static inline void vma_dir_maybe_rebuild(struct mm_struct *mm) { }
static inline void vma_dir_free(struct mm_struct *mm) { }
static inline void vma_dir_init(struct mm_struct *mm) { }
#endif

#endif /* _LINUX_VMA_DIR_H */
