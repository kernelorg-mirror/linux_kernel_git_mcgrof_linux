#define pr_fmt(fmt) "debug-beta: " fmt

#include <linux/kernel.h>
#include <asm/x86_init.h>

#include "gamma.h"

static bool x86_dbg_detect_beta(void)
{
	return true;
}

static void early_init_dbg_beta(void) {
	pr_info("early_init triggered\n");
}
x86_init_early_pc(x86_dbg_detect_beta,
		  x86_dbg_detect_gamma,
		  early_init_dbg_beta);
