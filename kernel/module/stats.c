// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Let's keep keep tabs on a few important module stats, useful
 * for debugging production loads and interactions between userspace
 * and kernelspace for loading modules.
 *
 * Copyright (C) 2023 Luis Chamberlain <mcgrof@kernel.org>
 */

#include <linux/module.h>
#include <linux/string.h>
#include <linux/printk.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/debugfs.h>
#include <linux/rculist.h>

#include "internal.h"

extern struct dentry *mod_debugfs_root;

/*
 * Tracks modules which failed to be loaded as they were being processed.
 * These require modulesed consumed vmalloc space for all finit_module()
 * calls as kernel_read*() is used. Then if compression is used vmap()
 * is used to allocate space for the decompressed version of what userspace
 * has on the filesystem, we vfree() the compressed data which kerne_read*()
 * fetched for us. Finally, a final module is allocated as well which we
 * use to keep around, and that *can* use vmalloc() too.
 *
 * In the worst case, when module compression is used then we use the vmap
 * space three times.
 *
 * We really should strive to get this list to be empty. This not being empty
 * is a reflection of us needing to do more work to ensure either the kernel
 * or usersapce does not do unnecessary calls to load modules which it should
 * know are already loaded or on its way to be loaded.
 */
static LIST_HEAD(failed_modules);

/* Total bytes used by all modules we've dealt with on this system */
atomic64_t total_mod_size;

/* Total .text section sizes we've dealt with on this system */
atomic64_t total_text_size;

/* Failures happen on the initial kernel_read_*() call. They use vmalloc() */
atomic64_t invalid_kread_bytes;

/* Failures happen on the module decompression path, these use use vmap(). */
atomic64_t invalid_decompress_bytes;

/*
 * The invalid_mod_becoming_bytes only keeps tabs of failures in between kread
 * success and right before we allocate the module to process it. These
 * can be failures due to:
 *
 *  o module_sig_check() - module signature checks
 *  o elf_validity_cache_copy() - ELF does not add up
 *  o early_mod_check():
 *  	- blacklist
 *  	- failed to rewrite section headers
 *  	- verion magic
 *  	- live patch requirements didn't check out
 *  	- the module was detected as being already present, this
 *  	  first check avoids a new vmalloc for the full size of
 *  	  the module.
 */
atomic64_t invalid_mod_becoming_bytes;

/*
 * These are failures after we did all the sanity checks of the
 * module userspace passed to us. This can still fail if we detect
 * the module is loaded, we do this check after we allocate space
 * for the module.
 */
atomic64_t invalid_mod_bytes;

/* How many modules we've loaded in our kernel life time */
atomic_t modcount;

/* How many modules failed due to failed kernel_read*() */
atomic_t failed_kreads;

/* How many failed decompression attempts we've had */
atomic_t failed_decompress;

/* How many modules failed once we've allocated space for our module */
atomic_t failed_load_modules;

int try_add_failed_module(const char *name)
{
	struct mod_fail_load *mod_fail;

	list_for_each_entry_rcu(mod_fail, &failed_modules, list,
				lockdep_is_held(&module_mutex)) {
                if (!strcmp(mod_fail->name, name)) {
                        atomic_inc(&mod_fail->count);
                        goto out;
                }
        }

	mod_fail = kmalloc(sizeof(*mod_fail), GFP_KERNEL);
	if (!mod_fail)
		return -ENOMEM;
	strscpy(mod_fail->name, name, MODULE_NAME_LEN);
        atomic_inc(&mod_fail->count);
        list_add_rcu(&mod_fail->list, &failed_modules);
out:
	return 0;
}

static ssize_t read_file_mod_stats(struct file *file, char __user *user_buf,
				   size_t count, loff_t *ppos)
{
	struct mod_fail_load *mod_fail;
	unsigned int len;
	const unsigned int size = 1024;
	char *buf;
	u32 live_mod_count, fkreads, floads;
	u64 total_size, text_size, ikread_bytes, idecomp_bytes, imod_bytes;

	live_mod_count = atomic_read(&modcount);
	fkreads = atomic_read(&failed_kreads);
	floads = atomic_read(&failed_load_modules);

	total_size = atomic64_read(&total_mod_size);
	text_size = atomic64_read(&total_text_size);
	ikread_bytes = atomic64_read(&invalid_mod_bytes);
	idecomp_bytes = atomic64_read(&invalid_decompress_bytes);
	imod_bytes = atomic64_read(&invalid_mod_bytes);

	buf = kzalloc(size, GFP_KERNEL);
	if (buf == NULL)
		return -ENOMEM;

	len += scnprintf(buf + len, size - len, "%25s\t%u\n", "Modules loaded", live_mod_count);
	len += scnprintf(buf + len, size - len, "%25s\t%llu\n", "Total module size", total_size);
	len += scnprintf(buf + len, size - len, "%25s\t%llu\n", "Total mod text size", text_size);

	/*
	 * Failed kmod bytes do not contain any failed kreads bytes as those
	 * failures would happen earlier on kread. Failed kread bytes are wasted
	 * vmalloc() space allocations and are indicative of invalid modules.
	 */
	len += scnprintf(buf + len, size - len, "%25s\t%llu\n", "Failed kread bytes", ikread_bytes);

	/*
	 * Failed kmod bytes are modules which for whatever reason fail to load
	 * on the load_module() effort. They are good signs the kernel or userspace
	 * is doing something stupid or that could be improved.
	 */
	len += scnprintf(buf + len, size - len, "%25s\t%llu\n", "Failed kmod bytes", imod_bytes);

	len += scnprintf(buf + len, size - len, "%25s\t%llu\n", "Invalid kread bytes", ikread_bytes);
	len += scnprintf(buf + len, size - len, "%25s\t%llu\n", "Invalid decompress bytes", idecomp_bytes);
	len += scnprintf(buf + len, size - len, "%25s\t%llu\n", "Invalid mod bytes", imod_bytes);

	if (live_mod_count && total_size) {
		len += scnprintf(buf + len, size - len, "%25s\t%llu\n", "Average mod size",
				 DIV_ROUND_UP(total_size, live_mod_count));
	}

	if (live_mod_count && text_size) {
		len += scnprintf(buf + len, size - len, "%25s\t%llu\n", "Average mod text size",
				 DIV_ROUND_UP(text_size, live_mod_count));
	}

	if (list_empty(&failed_modules))
		goto out;

	len += scnprintf(buf + len, size - len, "Failed modules:\n");
	list_for_each_entry_rcu(mod_fail, &failed_modules, list)
		len += scnprintf(buf + len, size - len, "%25s\n", mod_fail->name);
out:
        return simple_read_from_buffer(user_buf, count, ppos, buf, len);
}

static const struct file_operations fops_mod_stats = {
	.read = read_file_mod_stats,
	.open = simple_open,
	.owner = THIS_MODULE,
	.llseek = default_llseek,
};

static int __init module_stats_init(void)
{
	debugfs_create_atomic64_t("total_mod_size", 0400, mod_debugfs_root, &total_mod_size);
	debugfs_create_atomic64_t("total_text_size", 0400, mod_debugfs_root, &total_text_size);
	debugfs_create_atomic64_t("invalid_kread_bytes", 0400, mod_debugfs_root, &invalid_kread_bytes);
	debugfs_create_atomic64_t("invalid_decompress_bytes", 0400, mod_debugfs_root, &invalid_decompress_bytes);
	debugfs_create_atomic64_t("invalid_mod_bytes", 0400, mod_debugfs_root, &invalid_mod_bytes);
	debugfs_create_atomic_t("modcount", 0400, mod_debugfs_root, &modcount);
	debugfs_create_atomic_t("failed_kreads", 0400, mod_debugfs_root, &failed_kreads);
	debugfs_create_atomic_t("failed_load_modules", 0400, mod_debugfs_root, &failed_load_modules);
	debugfs_create_file("stats", 0400, mod_debugfs_root, mod_debugfs_root, &fops_mod_stats);

	return 0;
}
module_init(module_stats_init);
