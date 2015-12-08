#ifndef _LINUX_LINKER_TABLES_H
#define _LINUX_LINKER_TABLES_H

#include <linux/sections.h>

/*
 * Linux linker tables
 *
 * Copyright (C) 2005-2009 Michael Brown <mcb30@ipxe.org>
 * Copyright (C) 2015-2016 Luis R. Rodriguez <mcgrof@kernel.org>
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
 * A linker table is a data structure that is stitched together from items
 * in multiple object files. Linux has historically implicitly used linker
 * tables for ages, however they were all built in an adhoc manner which
 * requires linker script modifications, per architecture. This linker table
 * solution provides a general linker table facility so that a new linker table
 * can be implemented by changing C code only.
 *
 * Linker tables help you simplify init sequences by using ELF sections, linker
 * build time selective sorting (disabled options get ignored), and can
 * optionally also be used to help you avoid code bit-rot due to #ifdery
 * collateral.
 */

/**
 * DOC: Linker table provenance and userspace testing
 *
 * The Linux implementation of linker tables is derivative of iPXE linker
 * table's solution (iPXE commit 67a10ef000cb7 [0]).  To see how this code
 * evolved or to extend and test and use this code in userspace refer to the
 * userspace linker-table tree [1].  This repository can be used for ease of
 * testing of extensions and sampling of changes prior to inclusion into Linux,
 * it is intended to be kept up to date to match Linux's solution. Contrary to
 * iPXE's solution, which strives to force compilation of everything using
 * linker tables, Linux's solution allows for developers to be selective over
 * where one wishes to force compilation, this then is just an optional feature
 * for the Linux linker table solution.
 *
 * [0] git://git.ipxe.org/ipxe.git
 *
 * [1] https://git.kernel.org/cgit/linux/kernel/git/mcgrof/linker-tables.git/
 */

/**
 * DOC: The code bit-rot problem
 *
 * Overuse of C #ifdefs can be problematic for certain types of code.  Linux
 * provides a rich array of features, but all these features take up valuable
 * space in a kernel image. The traditional solution to this problem has been
 * for each feature to have its own Kconfig entry and for the respective code
 * to be wrapped around #ifdefs, allowing the feature to be compiled in only
 * if desired.
 *
 * The problem with this is that over time it becomes very difficult and time
 * consuming to compile, let alone test, all possible versions of Linux. Code
 * that is not typically used tends to suffer from bit-rot over time. It can
 * become difficult to predict which combinations of compile-time options will
 * result in code that can compile and link correctly.
 */

/**
 * DOC: Avoiding the code bit-rot problem when desirable
 *
 * To solve the code bit-rot problem linker tables can be used on Linux, it
 * enables you to always force compiling of select features that one wishes to
 * avoid bit-rot while still enabling you to disable linking feature code into
 * the final kernel image if the features have been disabled via Kconfig.
 * Linux's linker tables allows for developers to be selective over where one
 * wishes to take advantage of the optional feature of forcing compilation and
 * only linking in enabled features.
 *
 * To use linker tables and to optionally take advantage of avoiding code
 * bit-rot, feature code should be implemented in separate C files, and should
 * be designed to always be compiled -- they should not be guarded with a C
 * code #ifdef CONFIG_FOO statements, consideration must also be taken for
 * sub-features which depend on the main CONFIG_FOO option, as they will be
 * disabled if they depend on CONFIG_FOO and therefore not compiled. To force
 * compilation and only link when features are needed a new optional target
 * table-y can be used on Makefiles, documented below.
 *
 * Currently only built-in features are supported, modular support is not
 * yet supported, however you can make use of sub-features for modules
 * if they are independent and can simply be linked into modules.
 */

/**
 * DOC: Using target table-y and table-n
 *
 * Let's assume we want to always force compilation of feature FOO in the
 * kernel but avoid linking it. When you enable the FOO feature via Kconfig
 * you'd end up with:
 *
 *	#define CONFIG_FOO 1
 *
 * You typically would then just use this on your Makefile to selectively
 * compile and link the feature:
 *
 *	obj-$(CONFIG_FOO) += foo.o
 *
 * You could instead optionally use the new linker table object:
 *
 *	table-$(CONFIG_FOO) += foo.o
 *
 * Alternatively, this would be the equivalent of listing:
 *
 *	extra += foo.o
 *	obj-$(CONFIG_FOO) += foo.o
 *
 * Both are mechanisms which can be used to take advantage of forcing
 * compilation with linker tables, however making use of table-$(CONFIG_FOO)
 * is encouraged as it helps with annotating linker tables clearly where
 * compilation is forced.
 */

/**
 * DOC: Opting out of forcing compilation
 *
 * If you want to opt-out of forcing compilation you would use the typical
 * obj-$(CONFIG_FOO) += foo.o and foo.o will only be compiled and linked
 * in when enabled. Using both table-$(CONFIG_FOO) and obj-($CONFIG_FOO)
 * will result with the feature on your binary only if you've enabled
 * CONFIG_FOO, however using table-$(CONFIG_FOO) will always force compilation,
 * this is why avoiding code bit-rot is an optional fature for Linux linker
 * tables.
 */

/**
 * DOC: How linker tables simplify inits
 *
 * Traditionally, we would implement features in C code as follows:
 *
 *	foo_init();
 *
 * You'd then have a foo.h which would have:
 *
 *	#ifdef CONFIG_FOO
 *	#else
 *	static inline void foo(void) { }
 *	#endif
 *
 * With linker tables this is no longer necessary as your init routines would
 * be implicit, you'd instead call:
 *
 *	call_init_fns();
 *
 * call_init_fns() would call all functions present in your init table and if
 * and only if foo.o gets linked in, then its initialisation function will be
 * called, whether you use obj-$(CONFIG_FOO) or table-$(CONFIG_FOO).
 *
 * The linker script takes care of assembling the tables for us. All of our
 * table sections have names of the format SECTION_NAME*.tbl.NAME.N. Here
 * SECTION_NAME is one of the standard sections in include/linux/sections.h,
 * and NAME designates the specific use case for the linker table, the table.
 * N is a digit decimal number used to impose an "order level" upon the tables
 * if required. NN= (empty string) is reserved for the symbol indicating "table
 * start", and N=~ is reserved for the symbol indicating "table end". In order
 * for the call_init_fns() to work behind the scenes the custom linker script
 * would need to define the beginning of the table, the end of the table, and
 * in between it should use SORT() to give order-level effect. Now, typically
 * such type of requirements would require custom linker script modifications
 * but Linux's linker tables builds on top of already existing standard Linux
 * ELF sections, each with different purposes. This lets you build and add
 * new tables without needing custom linker script modifications. This is
 * also done to support all architectures. All that is needed then is to
 * ensure a respective common linker table entry is added to the shared
 * include/asm-generic/vmlinux.lds.h. There should be a respective:
 *
 *	*(SORT(SECTION_TBL_ALL(SECTION_NAME)))
 *
 * entry for each type of supported section there. If your SECTION_NAME
 * is not yet supported, consider adding support for it.
 *
 * The order-level is really only a helper, if only one order level is
 * used, the next contributing factor to order is the order of the code
 * in the C file, and the order of the objects in the Makefile. Using
 * an order level then should not really be needed in most cases, its
 * use however enables to compartamentalize code into tables where ordering
 * through C file or through the Makefile would otherwise be very difficult
 * or if one wanted to enable very specific initialization semantics.
 *
 * As an example, suppose that we want to create a "frobnicator"
 * feature framework, and allow for several independent modules to
 * provide frobnicating services. Then we would create a frob.h
 * header file containing e.g.
 *
 *	struct frobnicator {
 *		const char *name;
 *		void (*frob) (void);
 *	};
 *
 *	DECLARE_LINKTABLE_INIT(struct frobnicator, frobnicator_fns);
 *
 * Any module providing frobnicating services would look something
 * like
 *
 *	#include "frob.h"
 *	static void my_frob(void) {
 *		... Do my frobnicating
 *	}
 *	LINKTABLE(frobnicator_fns, all) my_frobnicator = {
 *		.name = "my_frob",
 *		.frob = my_frob,
 *	};
 *
 * The central frobnicator code (frob.c) would use the frobnicating
 * modules as follows
 *
 *	#include "frob.h"
 *
 *	void frob_all(void) {
 *		struct frob *frob;
 *
 *		LINKTABLE_FOR_EACH(frob, frobnicator_fns) {
 *			pr_info("Calling frobnicator \"%s\"\n", frob->name);
 *			frob->frob();
 *		}
 *	}
 */

/**
 * DOC: Linker table helpers
 *
 * These are helpers for linker tables.
 */

/**
 * LINKTABLE_ALIGNMENT - get linker table alignment
 *
 * @name: linker table
 *
 * Gives you the linker table alignment.
 */
#define LINKTABLE_ALIGNMENT(name)	__alignof__(__typeof__(name[0]))

/**
 * LINKTABLE_SIZE - get number of entries in linker table
 *
 * @name: linker table
 *
 * This gives you the number of entries in linker table.
 * Example usage:
 *
 *   unsigned int num_frobs = LINKTABLE_SIZE(frobnicator_fns);
 */
#define LINKTABLE_SIZE(name)	((name##__end) - (name))

/**
 * LINKTABLE_EMPTY - check if link table is empty
 *
 * @name: linker table
 *
 * Returns true if link table is emtpy.
 *
 *   bool is_empty = LINKTABLE_EMPTY(frobnicator_fns);
 */
#define LINKTABLE_EMPTY(name)	(LINKTABLE_SIZE(name) == 0)

/**
 * LINKTABLE_START - get address of start of linker table.
 *
 * @tbl: linker table
 *
 * This gives you the start address of the linker table.
 * This should give you the address of the first entry.
 *
 */
#define LINKTABLE_START(tbl)	tbl

/**
 * LINKTABLE_END - get address of endd of linker table.
 *
 * @tbl: linker table
 *
 * This gives you the end address of the linker table.
 * This should give you the address of the end of the
 * linker table. This will match the start address if the
 * linker table is empty.
 */
#define LINKTABLE_END(tbl)	tbl##__end

/**
 * LINKTABLE_ADDR_WITHIN - returns true if address is in range
 *
 * @tbl: linker table
 * @address: address to query for
 *
 * Returns true if the address is part of the linker table.
 */
#define LINKTABLE_ADDR_WITHIN(tbl, addr)				\
	 (addr >= (unsigned long) LINKTABLE_START(tbl) &&		\
          addr < (unsigned long) LINKTABLE_END(tbl))

/**
 * DOC: Constructing linker tables
 *
 * Linker tables constructors are used to build an entry into a linker table.
 * Linker table constructors exist for each type of supported section.
 *
 * You have weak and regular type of link table entry constructors.
 */

/**
 * DOC: Weak linker tables constructors
 *
 * The weak attribute is desirable if you want an entry you can replace at
 * link time. A very special use case for linker tables is the first entry.
 * A weak attribute is used for the first entry to ensure that this entry's
 * address matches the end address of the table when the linker table is
 * emtpy, but will also point to the first real entry of the table once not
 * empty. When the first entry is linked in, it takes place of the first entry.
 */

/**
 * LINKTABLE_DATA_WEAK - Constructs a weak linker table entry for data
 *
 * @name: linker table name
 * @level: order level
 *
 * Constructs a weak linker table which we data.
 */
#define LINKTABLE_DATA_WEAK(name, level)				\
	      __typeof__(name[0])					\
	      __attribute__((used,					\
			     weak,					\
			     __aligned__(LINKTABLE_ALIGNMENT(name)),	\
			     section(SECTION_TBL(SECTION_DATA, name, level))))

/**
 * LINKTABLE_TEXT_WEAK - Constructs a weak linker table entry for execution
 *
 * @name: linker table name
 * @level: order level
 *
 * Constructs a weak linker table which we for execution. These will be
 * read-only.
 */
#define LINKTABLE_TEXT_WEAK(name, level)				\
	      __typeof__(name[0])					\
	      __attribute__((used,					\
			     weak,					\
			     __aligned__(LINKTABLE_ALIGNMENT(name)),	\
			     section(SECTION_TBL(SECTION_TEXT, name, level))))


/**
 * LINKTABLE_RO_WEAK - Constructs a weak read-only linker table entry
 *
 * @name: linker table name
 * @level: order level
 *
 * Constructs a weak linker table which we know only requires read-only access.
 */
#define LINKTABLE_RO_WEAK(name, level)					\
	const __typeof__(name[0])					\
	      __attribute__((used,					\
			     weak,					\
			     __aligned__(LINKTABLE_ALIGNMENT(name)),	\
			     section(SECTION_TBL(SECTION_RODATA, name, level))))

/**
 * LINKTABLE_INIT_WEAK - Constructs a weak linker table entry for init code
 *
 * @name: linker table name
 * @level: order level
 *
 * Constructs a weak linker table which will need at init for execution.
 */
#define LINKTABLE_INIT_WEAK(name, level)					\
	      __typeof__(name[0])					\
	      __attribute__((used,					\
			     weak,					\
			     __aligned__(LINKTABLE_ALIGNMENT(name)),	\
			     section(SECTION_TBL(SECTION_INIT, name, level))))

/**
 * LINKTABLE_INIT_DATA_WEAK - Constructs a weak linker table entry for initdata
 *
 * @name: linker table name
 * @level: order level
 *
 * Constructs a weak linker table which will need at init for data.
 */
#define LINKTABLE_INIT_DATA_WEAK(name, level)				\
	      __typeof__(name[0])					\
	      __attribute__((used,					\
			     weak,					\
			     __aligned__(LINKTABLE_ALIGNMENT(name)),	\
			     section(SECTION_TBL(SECTION_INIT_DATA, name, level))))

/**
 * DOC: Regular linker linker table constructors
 *
 * Regular constructors are expected to be used for valid linker table entries.
 * Valid uses of weak entries other than the beginning and is currently
 * untested but should in theory work.
 */

/**
 * LINKTABLE_TEXT - Declares a linker table entry for execution
 *
 * @name: linker table name
 * @level: order level
 *
 * Declares a linker table to be used for execution.
 */
#define LINKTABLE_TEXT(name, level)					\
	      __typeof__(name[0])					\
	      __attribute__((used,					\
			     __aligned__(LINKTABLE_ALIGNMENT(name)),	\
			     section(SECTION_TBL(SECTION_TEXT, name, level))))

/**
 * LINKTABLE_DATA - Declares a data linker table entry
 *
 * @name: linker table name
 * @level: order level
 *
 * Declares a data linker table entry. These are read-write.
 */
#define LINKTABLE_DATA(name, level)					\
	      __typeof__(name[0])					\
	      __attribute__((used,					\
			     __aligned__(LINKTABLE_ALIGNMENT(name)),	\
			     section(SECTION_TBL(SECTION_DATA, name, level))))
/**
 * LINKTABLE_RO - Declares a read-only linker table entry.
 *
 * @name: linker table name
 * @level: order level
 *
 * Declares a linker table which we know only requires read-only access.
 */
#define LINKTABLE_RO(name, level)					\
	const __typeof__(name[0])					\
	      __attribute__((used,					\
			     __aligned__(LINKTABLE_ALIGNMENT(name)),	\
			     section(SECTION_TBL(SECTION_RODATA, name, level))))

/**
 * LINKTABLE_INIT - Declares a linker table entry to be used on init.
 *
 * @name: linker table name
 * @level: order level
 *
 * Declares a linker table entry which we will use during init for execution.
 */
#define LINKTABLE_INIT(name, level)					\
	      __typeof__(name[0])					\
	      __attribute__((used,					\
			     __aligned__(LINKTABLE_ALIGNMENT(name)),	\
			     section(SECTION_TBL(SECTION_INIT, name, level))))
/**
 * LINKTABLE_INIT_DATA - Declares a linker table entry to be used on init data.
 *
 * @name: linker table name
 * @level: order level
 *
 * Declares a linker table entry which we will use during init for data.
 */
#define LINKTABLE_INIT_DATA(name, level)					\
	      __typeof__(name[0])					\
	      __attribute__((used,					\
			     __aligned__(LINKTABLE_ALIGNMENT(name)),	\
			     section(SECTION_TBL(SECTION_INIT_DATA, name, level))))


/**
 * DOC: Declaring Linker tables
 *
 * Declarers are used to help code access the linker tables. Typically
 * header files for subsystems would declare the linker tables to enable
 * easy access to add new entries, and to iterate over the list of table.
 */

/**
 * DECLARE_LINKTABLE_TEXT - Declares linker table entry for exectuion
 *
 * @type: data type
 * @name: table name
 *
 * Declares a linker table entry for execution.
 */
#define DECLARE_LINKTABLE_TEXT(type, name)				\
	 extern type name[], name##__end[];

/**
 * DECLARE_LINKTABLE_DATA - Declares a data linker table entry
 *
 * @type: data type
 * @name: table name
 *
 * Declares a data linker table entry.
 */
#define DECLARE_LINKTABLE_DATA(type, name)				\
	 extern type name[], name##__end[];

/**
 * DECLARE_LINKTABLE_RO - Declares a read-only linker table entry
 *
 * @type: data type
 * @name: table name
 *
 * Declares a read-only linker table entry.
 */
#define DECLARE_LINKTABLE_RO(type, name)				\
	 extern const type name[], name##__end[];

/**
 * DECLARE_LINKTABLE_INIT - Declares a linker table entry to be used on init
 *
 * @type: data type
 * @name: table name
 *
 * Declares a linker table entry to be used on init for execution.
 */
#define DECLARE_LINKTABLE_INIT(type, name)				\
	 extern type name[], name##__end[];

/**
 * DECLARE_LINKTABLE_INIT_DATA - Declares a data init linker table entry
 *
 * @type: data type
 * @name: table name
 *
 * Declares a linker table entry to be used on init for data
 */
#define DECLARE_LINKTABLE_INIT_DATA(type, name)				\
	 extern type name[], name##__end[];


/**
 * DOC: Defining Linker tables
 *
 * Linker tables are defined in the code that takes ownership over
 * the linker table. This is typically done in the same code that is in
 * charge of iterating over the linker table as well.
 */

/**
 * DEFINE_LINKTABLE_TEXT - Defines a linker table for execution
 *
 * @type: data type
 * @name: table name
 *
 * Defines a linker table which used for execution.
 */
#define DEFINE_LINKTABLE_TEXT(type, name)				\
	DECLARE_LINKTABLE_TEXT(type, name);				\
	LINKTABLE_TEXT_WEAK(name, ) VMLINUX_SYMBOL(name)[0] = {};	\
	LTO_REFERENCE_INITCALL(name);					\
	LINKTABLE_TEXT(name, ~) VMLINUX_SYMBOL(name##__end)[0] = {};\
	LTO_REFERENCE_INITCALL(name##__end);

/**
 * DEFINE_LINKTABLE_DATA - Defines a linker table for data
 *
 * @type: data type
 * @name: table name
 *
 * Defines a linker table which used for data.
 */
#define DEFINE_LINKTABLE_DATA(type, name)				\
	DECLARE_LINKTABLE_DATA(type, name);				\
	LINKTABLE_DATA_WEAK(name, ) VMLINUX_SYMBOL(name)[0] = {};	\
	LTO_REFERENCE_INITCALL(name);					\
	LINKTABLE_DATA(name, ~) VMLINUX_SYMBOL(name##__end)[0] = {};\
	LTO_REFERENCE_INITCALL(name##__end);

/**
 * DEFINE_LINKTABLE_RO - Defines a read-only linker table
 *
 * @type: data type
 * @name: table name
 *
 * Defines a linker table which we know only requires read-only access.
 */
#define DEFINE_LINKTABLE_RO(type, name)					\
	DECLARE_LINKTABLE_RO(type, name);				\
	LINKTABLE_RO_WEAK(name, ) VMLINUX_SYMBOL(name)[0] = {};		\
	LTO_REFERENCE_INITCALL(name);					\
	LINKTABLE_RO(name, ~) VMLINUX_SYMBOL(name##__end)[0] = {};	\
	LTO_REFERENCE_INITCALL(name##__end);

/**
 * DEFINE_LINKTABLE_INIT - Defines an init time linker table for execution
 *
 * @type: data type
 * @name: table name
 *
 * Defines a linker table. If you are adding a new type you should
 * enable CONFIG_DEBUG_SECTION_MISMATCH and ensure routines that make
 * use of the linker tables get a respective __ref tag.
 */
#define DEFINE_LINKTABLE_INIT(type, name)					\
	DECLARE_LINKTABLE_INIT(type, name);					\
	LINKTABLE_INIT_WEAK(name, ) VMLINUX_SYMBOL(name)[0] = {};		\
	LTO_REFERENCE_INITCALL(name);						\
	LINKTABLE_INIT(name, ~) VMLINUX_SYMBOL(name##__end)[0] = {};	\
	LTO_REFERENCE_INITCALL(name##__end);

/**
 * DEFINE_LINKTABLE_INIT_DATA - Defines an init time linker table for data
 *
 * @type: data type
 * @name: table name
 *
 * Defines a linker table for init data. If you are adding a new type you
 * should enable CONFIG_DEBUG_SECTION_MISMATCH and ensure routines that make
 * use of the linker tables get a respective __ref tag.
 */
#define DEFINE_LINKTABLE_INIT_DATA(type, name)					\
	DECLARE_LINKTABLE_INIT_DATA(type, name);					\
	LINKTABLE_INIT_DATA_WEAK(name, ) VMLINUX_SYMBOL(name)[0] = {};		\
	LTO_REFERENCE_INITCALL(name);						\
	LINKTABLE_INIT_DATA(name, ~) VMLINUX_SYMBOL(name##__end)[0] = {};	\
	LTO_REFERENCE_INITCALL(name##__end);

/**
 * DOC: Iterating over Linker tables
 *
 * To make use of the linker tables you want to be able to iterate over
 * them. This section documents the different iterators available.
 */

/**
 * LINKTABLE_FOR_EACH - iterate through all entries within a linker table
 *
 * @pointer: entry pointer
 * @tbl: linker table
 *
 * Example usage:
 *
 *   struct frobnicator *frob;
 *
 *   LINKTABLE_FOR_EACH(frob, frobnicator_fns) {
 *     ...
 *   }
 */
#define LINKTABLE_FOR_EACH(pointer, tbl)				\
	for (pointer = LINKTABLE_START(tbl);				\
	     pointer < LINKTABLE_END(tbl);				\
	     pointer++)

/**
 * LINKTABLE_RUN_ALL - iterate and run through all entries on a linker table
 *
 * @tbl: linker table
 * @func: structure name for the function name we want to call.
 * @args...: arguments to pass to func
 *
 * Example usage:
 *
 *   LINKTABLE_RUN_ALL(frobnicator_fns, some_run,);
 */
#define LINKTABLE_RUN_ALL(tbl, func, args...)				\
do {									\
	size_t i;							\
	for (i = 0; i < LINKTABLE_SIZE(tbl); i++)			\
		(tbl[i]).func (args);					\
} while (0);

/**
 * LINKTABLE_RUN_ERR - run each linker table entry func and return error if any
 *
 * @tbl: linker table
 * @func: structure name for the function name we want to call.
 * @args...: arguments to pass to func
 *
 * Example usage:
 *
 *   unsigned int err = LINKTABLE_RUN_ERR(frobnicator_fns, some_run,);
 */
#define LINKTABLE_RUN_ERR(tbl, func, args...)				\
({									\
	size_t i;							\
	int err = 0;							\
	for (i = 0; !err && i < LINKTABLE_SIZE(tbl); i++)		\
		err = (tbl[i]).func (args);				\
		err; \
})

#endif /* _LINUX_LINKER_TABLES_H */
