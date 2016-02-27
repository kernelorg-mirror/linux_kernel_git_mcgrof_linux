#ifndef _LINUX_SECTIONS_H
#define _LINUX_SECTIONS_H
/*
 * Linux ELF sections
 *
 * Copyright (C) 2016 Luis R. Rodriguez <mcgrof@kernel.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Due to this file being licensed under the GPL there is controversy over
 * whether this permits you to write a module that #includes this file
 * without placing your module under the GPL.  Please consult a lawyer for
 * advice before doing this.
 */

/**
 * DOC: Introduction
 *
 * The Linux vmlinux binary uses a custom linker script which adds
 * some custom specialized ELF sections. This aims to document those
 * sections. Each section must document the goal of the section, and
 * address concurrency considerations when applicable.
 */

/**
 * DOC: Core Linux kernel sections
 *
 * These are the core Linux kernel sections.
 */

/**
 * SECTION_RODATA - read only data
 *
 * Macro name for code which must be protected from write access.
 */
#define SECTION_RODATA			.rodata

/**
 * SECTION_TEXT - kernel code execution section, read-only
 *
 * Macro name used to annotate code (functions) used during regular
 * kernel run time. This is combined with SECTION_RODATA, only this
 * section also gets execution allowed.
 *
 */
#define SECTION_TEXT			.text

/**
 * SECTION_DATA - for read-write data
 *
 * Macro name for read-write data.
 */
#define SECTION_DATA			.data

/**
 * DOC: Linux init sections
 *
 * These sections are used for code and data structures used during boot or
 * module initialization. On architectures that support it (x86, x86_64), all
 * this code is freed up by the kernel right before the fist userspace init
 * process is called when built-in to the kernel, and if modular it is freed
 * after module initialization. Since the code is freed so early, in theory
 * there should be no races against freeing this code with other CPUs. Init
 * section code and data structures should never be exported with
 * EXPORT_SYMBOL*() as the code will quickly become unavailable to the kernel
 * after bootup.
 */

/**
 * SECTION_INIT - boot initialization code
 *
 * Macro name used to annotate code (functions) used only during boot or driver
 * initialization.
 *
 */
#define SECTION_INIT			.init.text

/**
 * SECTION_INIT_DATA - boot initialization data
 *
 * Macro name used to annotate data structures used only during boot or driver
 * initialization.
 */
#define SECTION_INIT_DATA		.init.data

/**
 * SECTION_INIT_RODATA - boot read-only initialization data
 *
 * Macro name used to annotate read-only code (functions) used only during boot
 * or driver initialization.
 */
#define SECTION_INIT_RODATA		.init.rodata

/**
 * SECTION_INIT_CALL - special init call
 *
 * Special macro name used to annotate subsystem init call. These calls are
 * are now grouped by functionality into separate subsections. Ordering inside
 * the subsections is determined by link order.
 */
#define SECTION_INIT_CALL		.initcall

/**
 * DOC: Linux exit sections
 *
 * These sections are used to declare a functions and data structures which
 * are only required on exit, the function or data structure will be dropped
 * if the code declaring this section is not compiled as a module on
 * architectures that support this (x86, x86_64). There is no special case
 * handling for this code when built-in to the kernel.
 */

/**
 * SECTION_EXIT - module exit code
 *
 * Macro name used to annotate code (functions) used only during module
 * unload.
 */
#define SECTION_EXIT			.exit.text

/**
 * SECTION_EXIT_DATA - module exit data structures
 *
 * Macro name used to annotate data structures used only during module
 * unload.
 */
#define SECTION_EXIT_DATA		.exit.data

/**
 * SECTION_EXIT_CALL - special exit call
 *
 * Special macro name used to annotate an exit exit routine, order
 * is important and maintained by link order.
 */
#define SECTION_EXIT_CALL		.exitcall.exit

/**
 * DOC: Linux references to init sections
 *
 * These sections are used to teach modpost to not warn about possible
 * misuses of init section code from other sections. If you use this
 * your use case should document why you are certain such use of init
 * sectioned code is valid. For more details refer to include/linux/init.h
 * __ref, __refdata, and __refconst documentation.
 */

/**
 * SECTION_REF - code referencing init is valid
 *
 * Macro name used to annotate that code (functions) declared with this section
 * has been vetteed as valid for its reference or use of other code (functions)
 * or data structures which are part of the init sections.
 */
#define SECTION_REF			.ref.text

/**
 * SECTION_REF_DATA - reference data structure are valid
 *
 * Macro name used to annotate data structures declared with this section have
 * been vetteed for its reference or use of other code (functions) or data
 * structures part of the init sections.
 */
#define SECTION_REF_DATA		.ref.data

/**
 * SECTION_REF_RODATA - const code or data structure referencing init is valid
 *
 * Macro name used to annotate const code (functions) const data structures which
 * has been vetteed for its reference or use of other code (functions) or data
 * structures part of the init sections.
 */
#define SECTION_REF_RODATA		.ref.rodata

/**
 * DOC: Custom Linux sections
 *
 * These are very custom Linux sections.
 */

#ifndef __ASSEMBLY__

/**
 * DOC: Section helpers
 *
 * These are helpers for section areas.
 */

/**
 * SECTION_ALIGNMENT - get section area alignment
 *
 * @name: section area name
 *
 * Gives you alignment for the the section area.
 */
#define SECTION_RANGE_ALIGNMENT(name)	__alignof__(__typeof__(name[0]))

/**
 * SECTION_SIZE - get number of entries in section area
 *
 * @name: section area name
 *
 * This gives you the number of entries in the section area.
 * Example usage:
 *
 *   unsigned int num_entries = SECTION_SIZE(section_name);
 */
#define SECTION_RANGE_SIZE(name)	((name##__end) - (name))

/**
 * SECTION_EMPTY - check if section area is empty
 *
 * @name: section area name
 *
 * Returns true if there are no entires in the section area.
 *
 *   bool is_empty = SECTION_EMPTY(section_name);
 */
#define SECTION_RANGE_EMPTY(name)	(SECTION_RANGE_SIZE(name) == 0)

/**
 * SECTION_START - get address of start of the section area
 *
 * @name: section area name
 *
 * This gives you the start address of the section area.
 * This should give you the address of the first entry in the
 * section area as well.
 *
 */
#define SECTION_RANGE_START(name)	name

/**
 * SECTION_END - get address of end of the section area
 *
 * @name: section area name
 *
 * This gives you the end address of the section area.
 * This will match the start address if the section area
 * is empty.
 */
#define SECTION_RANGE_END(name)	name##__end

/**
 * SECTION_ADDR_IN_RANGE - returns true if address is in section range
 *
 * @name: section range name
 * @address: address to query for in sectoin range
 *
 * Returns true if the address is within the section range.
 */
#define SECTION_ADDR_IN_RANGE(name, addr)				\
	 (addr >= (unsigned long) SECTION_RANGE_START(name) &&		\
          addr < (unsigned long) SECTION_RANGE_END(name))

#define SECTION_RANGE(name, section)					\
	#section "." #name

/**
 * DOC: Declaring section areas
 *
 * Declarers are used to help code access the section areas. Typically
 * header files for subsystems would declare the section areas to enable
 * easy access to add new entries, and to iterate over the list of table.
 */

/**
 * DECLARE_SECTION_TYPE - Declares section range of specific type
 *
 * @name: section range name
 * @type: data type
 *
 * Declares a section range of the specified data type.
 */
#define DECLARE_SECTION_RANGE_TYPE(type, name)				\
	 extern type name[], name##__end[];

/**
 * DECLARE_SECTION_RANGE_TYPE_RO - Declares a read-only type section range
 *
 * @name: section area name
 * @type: data type
 *
 * Declares a read only section range of the specified data type.
 * This should be used for section ranges including read only data or
 * a collection of executable code.
 */
#define DECLARE_SECTION_RANGE_TYPE_RO(type, name)			\
	 extern const type name[], name##__end[];

/**
 * DECLARE_SECTION_RANGE - Declares section range used for exectuion
 *
 * @name: section range name
 *
 * Declares a section range, to be used for execution.
 */
#define DECLARE_SECTION_RANGE(name)					\
	 DECLARE_SECTION_RANGE_TYPE_RO(char, name)

/**
 * DECLARE_SECTION_RANGE_DATA - Declares a data section area
 *
 * @name: table name
 *
 * Declares a data linker table entry.
 */
#define DECLARE_SECTION_RANGE_DATA(type, name)				\
	 DECLARE_SECTION_RANGE_TYPE(char, name)

/*
 * Without this you end up with the section macro
 * as part of the name
 */
#define __SECTION_TBL(section, name, level)				\
	#section ".tbl." #name "." #level

/**
 * SECTION_TBL - Linux linker table section
 *
 * @section: respective section
 * @name: used to describe the use case
 * @level: the order-level for the linker table
 *
 * Macro name used to annotate a linker table. For more details refer to
 * include/linux/tables.h. Linker tables use standard Linux sections defined
 * in this file.
 */
#define SECTION_TBL(section, name, level)                         	\
	__SECTION_TBL(section, name, level)

#endif /* __ASSEMBLY__ */

/*
 * For use on linker scripts and helpers
 */
#define ___SECTION_TBL(section, name)					\
	section##.tbl.##name
/**
 * SECTION_TBL_ALL - glob to capture all linker table uses for this section
 *
 * @section: respective section
 *
 * Macro name used by linker script to capture all linker tables uses for
 * the given section. This is used by include/asm-generic/vmlinux.lds.h
 */
#define SECTION_TBL_ALL(section)					\
	___SECTION_TBL(section,*)

#endif /* _LINUX_SECTIONS_H */
