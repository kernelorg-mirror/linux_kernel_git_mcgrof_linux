#define pr_fmt(fmt) "debug-gamma: " fmt

#include <linux/kernel.h>
#include <asm/x86_init.h>

bool x86_dbg_detect_gamma(void)
{
	return true;
}

static void early_init_dbg_gamma(void)
{
	pr_info("early_init triggered\n");
}

x86_init_early_pc(x86_dbg_detect_gamma,
		  NULL,
		  early_init_dbg_gamma);
