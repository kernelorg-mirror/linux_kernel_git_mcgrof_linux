#define pr_fmt(fmt) "debug-alpha: " fmt

#include <linux/kernel.h>
#include <asm/x86_init.h>

static void early_init_dbg_alpha(void) {
	pr_info("early_init triggered\n");
}

x86_init_early_pc_simple(early_init_dbg_alpha);
