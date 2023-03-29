// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2023 Luis Chamberlain <mcgrof@kernel.org>
 */

#include <linux/debugfs.h>
#include "internal.h"

struct dentry *mod_debugfs_root;

static int module_debugfs_init(void)
{
	mod_debugfs_root = debugfs_create_dir("modules", NULL);
	return 0;
}
module_init(module_debugfs_init);
