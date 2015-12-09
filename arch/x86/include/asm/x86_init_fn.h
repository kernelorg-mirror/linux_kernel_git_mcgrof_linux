#ifndef __X86_INIT_TABLES_H
#define __X86_INIT_TABLES_H

#include <linux/types.h>
#include <linux/tables.h>
#include <linux/init.h>
#include <linux/bitops.h>

/**
 * struct x86_init_fn - x86 generic kernel init call
 *
 * Linux x86 features vary in complexity, features may require work done at
 * different levels of the full x86 init sequence. Today there are also two
 * different possible entry points for Linux on x86, one for bare metal, KVM
 * and Xen HVM, and another for Xen PV guests / dom0.  Assuming a bootloader
 * has set up 64-bit mode, roughly the x86 init sequence follows this path:
 *
 * Bare metal, KVM, Xen HVM                      Xen PV / dom0
 *       startup_64()                             startup_xen()
 *              \                                     /
 *      x86_64_start_kernel()                 xen_start_kernel()
 *                           \               /
 *                      x86_64_start_reservations()
 *                                   |
 *                              start_kernel()
 *                              [   ...        ]
 *                              [ setup_arch() ]
 *                              [   ...        ]
 *                                  init
 *
 * x86_64_start_kernel() and xen_start_kernel() are the respective first C code
 * entry starting points. The different entry points exist to enable Xen to
 * skip a lot of hardware setup already done and managed on behalf of the
 * hypervisor, we refer to this as "paravirtualization yielding". The different
 * levels of init calls on the x86 init sequence exist to account for these
 * slight differences and requirements. These different entry points also share
 * a common entry x86 specific path, x86_64_start_reservations().
 *
 * A generic x86 feature can have different initialization calls, one on each
 * of the different main x86 init sequences, but must also address both entry
 * points in order to work properly across the board on all supported x86
 * subarchitectures. Since x86 features can also have dependencies on other
 * setup code or features, x86 features can at times be subordinate to other
 * x86 features, or conditions. struct x86_init_fn enables feature developers
 * to annotate dependency relationships to ensure subsequent init calls only
 * run once a subordinate's dependencies have run. When needed custom
 * dependency requirements can also be spelled out through a custom dependency
 * checker. In order to account for the dual entry point nature of x86-64 Linux
 * for "paravirtualization yielding" and to make annotations for support for
 * these explicit each struct x86_init_fn must specify supported
 * subarchitectures. The earliest x86-64 code can read the subarchitecture
 * though is after load_idt(), as such the earliest we can currently rely on
 * subarchitecture for semantics and a common init sequences is on the shared
 * common x86_64_start_reservations().  Each struct x86_init_fn must also
 * declare a two-digit decimal number to impose an ordering relative to other
 * features when required.
 *
 * x86_init_fn enables strong semantics and dependencies to be defined and
 * implemented on the full x86 initialization sequence.
 *
 * @order_level: must be set, linker order level, this corresponds to the table
 * 	section sub-table index, we record this only for semantic validation
 * 	purposes.  Order-level is always required however you typically would
 * 	only use X86_INIT_NORMAL*() and leave ordering to be done by placement
 * 	of code in a C file and the order of objects through a Makefile. Custom
 * 	order-levels can be used when order on C file and order of objects on
 * 	Makfiles does not suffice or much further refinements are needed.
 * @supp_hardware_subarch: must be set, it represents the bitmask of supported
 *	subarchitectures.  We require each struct x86_init_fn to have this set
 *	to require developer considerations for each supported x86
 *	subarchitecture and to build strong annotations of different possible
 *	run time states particularly in consideration for the two main
 *	different entry points for x86 Linux, to account for paravirtualization
 *	yielding.
 *
 *	The subarchitecture is read by the kernel at early boot from the
 *	struct boot_params hardware_subarch. Support for the subarchitecture
 *	exists as of x86 boot protocol 2.07. The bootloader would have set up
 *	the respective hardware_subarch on the boot sector as per
 *	Documentation/x86/boot.txt.
 *
 *	What x86 entry point is used is determined at run time by the
 *	bootloader. Linux pv_ops was designed to help enable to build one Linux
 *	binary to support bare metal and different hypervisors.  pv_ops setup
 *	code however is limited in that all pv_ops setup code is run late in
 *	the x86 init sequence, during setup_arch(). In fact cpu_has_hypervisor
 *	only works after early_cpu_init() during setup_arch(). If an x86
 *	feature requires an earlier determination of what hypervisor was used,
 *	or if it needs to annotate only support for certain hypervisors, the
 *	x86 hardware_subarch should be set by the bootloader and
 *	@supp_hardware_subarch set by the x86 feature. Using hardware_subarch
 *	enables x86 features to fill the semantic gap between the Linux x86
 *	entry point used and what pv_ops has to offer through a hypervisor
 *	agnostic mechanism.
 *
 *	Each supported subarchitecture is set using the respective
 *	X86_SUBARCH_* as a bit in the bitmask. For instance if a feature
 *	is supported on PC and Xen subarchitectures only you would set this
 *	bitmask to:
 *
 *		BIT(X86_SUBARCH_PC) |
 *		BIT(X86_SUBARCH_XEN);
 *
 * @detect: optional, if set returns true if the feature has been detected to
 *	be required, it returns false if the feature has been detected to not
 *	be required.
 * @depend: optional, if set this set of init routines must be called prior to
 * 	the init routine who's respective detect routine we have set this
 * 	depends callback to. This is only used for sorting purposes given
 * 	all current init callbacks have a void return type. Sorting is
 * 	implemented via x86_init_fn_sort(), it must be called only once,
 * 	however you can delay sorting until you need it if you can ensure
 * 	only @order_level and @supp_hardware_subarch can account for proper
 * 	ordering and dependency requirements for all init sequences prior.
 *	If you do not have a depend callback set its assumed the order level
 *	(__x86_init_fn(level)) set by the init routine suffices to set the
 *	order for when the feature's respective callbacks are called with
 *	respect to other calls. Sorting of init calls with the same order level
 *	is determined by linker order, determined by order placement on C code
 *	and order listed on a Makefile. A routine that depends on another is
 *	known as being subordinate to the init routine it depends on. Routines
 *	that are subordinate must have an order-level of lower priority or
 *	equal priority than the order-level of the init sequence it depends on.
 * @early_init: required, routine which will run in x86_64_start_reservations()
 *	after we ensure boot_params.hdr.hardware_subarch is accessible and
 *	properly set. Memory is not yet available. This the earliest we can
 *	currently define a common shared callback since all callbacks need to
 *	check for boot_params.hdr.hardware_subarch and this becomes accessible
 *	on x86-64 until after load_idt().
 * @flags: optional, bitmask of enum x86_init_fn_flags
 */
struct x86_init_fn {
	__u32 order_level;
	__u32 supp_hardware_subarch;
	bool (*detect)(void);
	bool (*depend)(void);
	void (*early_init)(void);
	__u32 flags;
};

/**
 * enum x86_init_fn_flags: flags for init sequences
 *
 * X86_INIT_FINISH_IF_DETECTED: tells the core that once this init sequence
 *	has completed it can break out of the loop for init sequences on
 *	its own level.
 * X86_INIT_DETECTED: private flag. Used by the x86 core to annotate that this
 * 	init sequence has been detected and it all of its callbacks
 * 	must be run during initialization.
 */
enum x86_init_fn_flags {
	X86_INIT_FINISH_IF_DETECTED = BIT(0),
	X86_INIT_DETECTED = BIT(1),
};

/* The x86 initialisation function table */
#define X86_INIT_FNS __table(struct x86_init_fn, "x86_init_fns")

/* Used to declares an x86 initialization table */
#define __x86_init_fn(order_level) __table_entry(X86_INIT_FNS, order_level)

/* Init order levels, we can start at 01 but reserve 01-09 for now */
#define X86_INIT_ORDER_EARLY	10
#define X86_INIT_ORDER_NORMAL	30
#define X86_INIT_ORDER_LATE	50

/*
 * Use LTO_REFERENCE_INITCALL just in case of issues with old versions of gcc.
 * This might not be needed for linker tables due to how we compartamentalize
 * sections and then order them at linker time, but just in case.
 */

#define x86_init(__level,						\
		 __supp_hardware_subarch,				\
		 __detect,						\
		 __depend,						\
		 __early_init)						\
	static struct x86_init_fn __x86_init_fn_##__early_init __used	\
		__x86_init_fn(__level) = {				\
		.order_level = __level,					\
		.supp_hardware_subarch = __supp_hardware_subarch,	\
		.detect = __detect,					\
		.depend = __depend,					\
		.early_init = __early_init,				\
	};								\
	LTO_REFERENCE_INITCALL(__x86_init_fn_##__early_init);

#define x86_init_early(__supp_hardware_subarch,				\
		       __detect,					\
		       __depend,					\
		       __early_init)					\
	x86_init(X86_INIT_ORDER_EARLY, __supp_hardware_subarch,		\
		 __detect, __depend,					\
		 __early_init);

#define x86_init_normal(__supp_hardware_subarch,			\
		       __detect,					\
		       __depend,					\
		       __early_init)					\
	x86_init(__name, X86_INIT_ORDER_NORMAL, __supp_hardware_subarch,\
		 __detect, __depend,					\
		 __early_init);

#define x86_init_early_all(__detect,					\
			   __depend,					\
			   __early_init)				\
	x86_init_early(__name, X86_SUBARCH_ALL_SUBARCHS,		\
		       __detect, __depend,				\
		       __early_init);

#define x86_init_early_pc(__detect,					\
			  __depend,					\
			  __early_init)					\
	x86_init_early(BIT(X86_SUBARCH_PC),				\
		       __detect, __depend,				\
		       __early_init);

#define x86_init_early_pc_simple(__early_init)				\
	x86_init_early((BIT(X86_SUBARCH_PC)), NULL, NULL,		\
		       __early_init);

#define x86_init_normal_all(__detect,					\
			    __depend,					\
			    __early_init)				\
	x86_init_normal(X86_SUBARCH_ALL_SUBARCHS,			\
		        __detect, __depend,				\
		        __early_init);

#define x86_init_normal_pc(__detect,					\
			   __depend,					\
			   __early_init)				\
	x86_init_normal((BIT(X86_SUBARCH_PC)),				\
		        __detect, __depend,				\
		        __early_init);


#define x86_init_normal_xen(__detect,					\
			    __depend,					\
			    __early_init)				\
	x86_init_normal((BIT(X86_SUBARCH_XEN)),				\
		        __detect, __depend,				\
		        __early_init);

/**
 * x86_init_fn_early_init: call all early_init() callbacks
 *
 * This calls all early_init() callbacks on the x86_init_fns linker table.
 */
void x86_init_fn_early_init(void);

/**
 * x86_init_fn_init_tables - sort and check x86 linker table
 *
 * This sorts struct x86_init_fn init sequences in the x86_init_fns linker
 * table by ensuring that init sequences that depend on other init sequences
 * are placed later in the linker table. Init sequences that do not have
 * dependencies are left in place. Circular dependencies are not allowed.
 * The order-level of subordinate init sequences, that is of init sequences
 * that depend on other init sequences, must have an order-level of lower
 * or equal priority to the init sequence it depends on.
 *
 * This also validates semantics of all struct x86_init_fn init sequences
 * on the x86_init_fns linker table.
 */
void x86_init_fn_init_tables(void);

#endif /* __X86_INIT_TABLES_H */
