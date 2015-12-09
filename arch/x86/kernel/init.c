#define pr_fmt(fmt) "x86-init: " fmt

#include <linux/bug.h>
#include <linux/kernel.h>

#include <asm/x86_init_fn.h>
#include <asm/bootparam.h>
#include <asm/boot.h>
#include <asm/setup.h>

extern struct x86_init_fn __tbl_x86_start_init_fns[], __tbl_x86_end_init_fns[];

static bool x86_init_fn_supports_subarch(struct x86_init_fn *fn)
{
	if (!fn->supp_hardware_subarch) {
		pr_err("Init sequence fails to declares any supported subarchs: %pF\n", fn->early_init);
		WARN_ON(1);
	}
	if (BIT(boot_params.hdr.hardware_subarch) & fn->supp_hardware_subarch)
		return true;
	return false;
}

void x86_init_fn_early_init(void)
{
	int ret;
	struct x86_init_fn *init_fn;
	unsigned int num_inits = table_num_entries(X86_INIT_FNS);

	if (!num_inits)
		return;

	pr_debug("Number of init entries: %d\n", num_inits);

	for_each_table_entry(init_fn, X86_INIT_FNS) {
		if (!x86_init_fn_supports_subarch(init_fn))
			continue;
		if (!init_fn->detect)
			init_fn->flags |= X86_INIT_DETECTED;
		else {
			ret = init_fn->detect();
			if (ret > 0)
				init_fn->flags |= X86_INIT_DETECTED;
		}

		if (init_fn->flags & X86_INIT_DETECTED) {
			init_fn->flags |= X86_INIT_DETECTED;
			pr_debug("Running early init %pF ...\n", init_fn->early_init);
			init_fn->early_init();
			pr_debug("Completed early init %pF\n", init_fn->early_init);
			if (init_fn->flags & X86_INIT_FINISH_IF_DETECTED)
				break;
		}
	}
}
