#ifndef _ASM_GENERIC_SECTIONS_H_
#define _ASM_GENERIC_SECTIONS_H_

/* References to section boundaries */

#ifndef __ASSEMBLY__
#include <linux/compiler.h>
#include <linux/types.h>
#include <linux/sections.h>

/*
 * Usage guidelines:
 * _text, _data: architecture specific, don't use them in arch-independent code
 * [_stext, _etext]: contains .text.* sections, may also contain .rodata.*
 *                   and/or .init.* sections
 * [_sdata, _edata]: contains .data.* sections, may also contain .rodata.*
 *                   and/or .init.* sections.
 * [__start_rodata, __end_rodata]: contains .rodata.* sections
 * [__init_begin, __init_end]: contains .init.* sections, but .init.text.*
 *                   may be out of this range on some architectures.
 * [_sinittext, _einittext]: contains .init.text.* sections
 * [__bss_start, __bss_stop]: contains BSS sections
 *
 * Following global variables are optional and may be unavailable on some
 * architectures and/or kernel configurations.
 *	_text, _data
 *	LINKTABLE_START(kprobes), LINKTABLE_END(kprobes)
 *	__entry_text_start, __entry_text_end
 *	__ctors_start, __ctors_end
 */
extern char _text[], _stext[], _etext[];
extern char _data[], _sdata[], _edata[];
extern char __bss_start[], __bss_stop[];
extern char __init_begin[], __init_end[];
extern char _sinittext[], _einittext[];
extern char _end[];
extern char __per_cpu_load[], __per_cpu_start[], __per_cpu_end[];
DECLARE_LINKTABLE_TEXT(char, kprobes);
extern char __entry_text_start[], __entry_text_end[];
extern char __start_rodata[], __end_rodata[];

/* Start and end of .ctors section - used for constructor calls. */
extern char __ctors_start[], __ctors_end[];

extern __visible const void __nosave_begin, __nosave_end;

/* function descriptor handling (if any).  Override
 * in asm/sections.h */
#ifndef dereference_function_descriptor
#define dereference_function_descriptor(p) (p)
#endif

/* random extra sections (if any).  Override
 * in asm/sections.h */
#ifndef arch_is_kernel_text
static inline int arch_is_kernel_text(unsigned long addr)
{
	return 0;
}
#endif

#ifndef arch_is_kernel_data
static inline int arch_is_kernel_data(unsigned long addr)
{
	return 0;
}
#endif

/**
 * memory_contains - checks if an object is contained within a memory region
 * @begin: virtual address of the beginning of the memory region
 * @end: virtual address of the end of the memory region
 * @virt: virtual address of the memory object
 * @size: size of the memory object
 *
 * Returns: true if the object specified by @virt and @size is entirely
 * contained within the memory region defined by @begin and @end, false
 * otherwise.
 */
static inline bool memory_contains(void *begin, void *end, void *virt,
				   size_t size)
{
	return virt >= begin && virt + size <= end;
}

/**
 * memory_intersects - checks if the region occupied by an object intersects
 *                     with another memory region
 * @begin: virtual address of the beginning of the memory regien
 * @end: virtual address of the end of the memory region
 * @virt: virtual address of the memory object
 * @size: size of the memory object
 *
 * Returns: true if an object's memory region, specified by @virt and @size,
 * intersects with the region specified by @begin and @end, false otherwise.
 */
static inline bool memory_intersects(void *begin, void *end, void *virt,
				     size_t size)
{
	void *vend = virt + size;

	return (virt >= begin && virt < end) || (vend >= begin && vend < end);
}

/**
 * init_section_contains - checks if an object is contained within the init
 *                         section
 * @virt: virtual address of the memory object
 * @size: size of the memory object
 *
 * Returns: true if the object specified by @virt and @size is entirely
 * contained within the init section, false otherwise.
 */
static inline bool init_section_contains(void *virt, size_t size)
{
	return memory_contains(__init_begin, __init_end, virt, size);
}

/**
 * init_section_intersects - checks if the region occupied by an object
 *                           intersects with the init section
 * @virt: virtual address of the memory object
 * @size: size of the memory object
 *
 * Returns: true if an object's memory region, specified by @virt and @size,
 * intersects with the init section, false otherwise.
 */
static inline bool init_section_intersects(void *virt, size_t size)
{
	return memory_intersects(__init_begin, __init_end, virt, size);
}

/*
 * Some architectures do not like the "\t" at the end (s39), we should be
 * able to generalize this further, but so far this covers most architectures.
 */
#define push_section_tbl(section, name, level, flags)			\
	".pushsection " SECTION_TBL(section,name,level) ",  \"" #flags "\"\n\t"
#endif

#endif /* _ASM_GENERIC_SECTIONS_H_ */
