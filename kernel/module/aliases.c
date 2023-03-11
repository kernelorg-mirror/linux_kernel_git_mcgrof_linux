// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Optional module in-kernel alias processing support.
 *
 * Copyright (C) 2023 Luis Chamberlain <mcgrof@kernel.org>
 */

#include <linux/module.h>
#include "internal.h"

void free_mod_aliases(struct module *mod)
{
	unsigned int i;

	if (!mod->num_aliases)
		return;

	for (i=0; i < mod->num_aliases; i++) {
		kfree(mod->aliases[i]);
		mod->aliases[i] = NULL;
	}

	kfree(mod->aliases);
	mod->aliases = NULL;
}

static int get_modinfo_tags(struct load_info *info,
			    const char *tag,
			    unsigned int *num_entries)
{
	char *p;
	unsigned int taglen = strlen(tag);
	Elf_Shdr *infosec = &info->sechdrs[info->index.info];
	unsigned long size = infosec->sh_size;
	const char *value;
	unsigned int len, tags_size = 0;

	for (p = (char *)infosec->sh_addr; p; p = module_next_tag_pair(p, &size)) {
		if (strncmp(p, tag, taglen) == 0 && p[taglen] == '=') {
			value = p + taglen + 1;
			len = strlen(value);
			if (len >=0 && len <= PAGE_SIZE) {
				(*num_entries)++;
				tags_size+=len;
			}
		}
	}

	return tags_size;
}

int module_process_aliases(struct module *mod, struct load_info *info)
{
	unsigned int size, i = 0, num_entries = 0;
	char *alias;

	size = get_modinfo_tags(info, "alias", &num_entries);
	if (WARN_ON(!size))
		return 0;

	mod->aliases = kzalloc(num_entries * sizeof(char *), GFP_KERNEL);
	if (!mod->aliases)
		return -ENOMEM;

	pr_debug("module %s num_aliases: %u\n", mod->name, num_entries);

	for_each_modinfo_entry(alias, info, "alias") {
		pr_debug("alias[%u] = %s\n", i, alias);
		mod->aliases[i] = kasprintf(GFP_KERNEL, "%s", alias);
		if (!mod->aliases[i])
			goto err_free;
		i++;
	}

	WARN_ON(i != num_entries);

	mod->num_aliases = num_entries;

	return 0;

err_free:
	while (i!=0) {
		i--;
		kfree(mod->aliases[i]);
		mod->aliases[i] = NULL;
	}

	kfree(mod->aliases);
	mod->aliases = NULL;

	return -ENOMEM;
}

bool module_name_match_aliases(struct module *mod, const char *name, size_t len)
{
	unsigned int i;
	const char *alias;

	for (i=0; i < mod->num_aliases; i++) {
		alias = mod->aliases[i];
		if (strlen(alias) == len && !memcmp(alias, name, len)) {
			pr_debug("module %s alias matched: alias[%u] = %s\n",
				 mod->name, i, alias);
			return true;
		}
	}

	return false;
}
