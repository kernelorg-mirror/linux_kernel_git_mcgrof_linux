/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __FIRMWARE_FALLBACK_H
#define __FIRMWARE_FALLBACK_H

#include <linux/firmware.h>
#include <linux/device.h>

#ifdef CONFIG_FW_LOADER_USER_HELPER
int fw_sysfs_fallback(struct firmware *fw, const char *name,
		      struct device *device,
		      unsigned int opt_flags,
		      int ret);
void kill_pending_fw_fallback_reqs(bool only_kill_custom);

void fw_fallback_set_cache_timeout(void);
void fw_fallback_set_default_timeout(void);

int register_sysfs_loader(void);
void unregister_sysfs_loader(void);
#else /* CONFIG_FW_LOADER_USER_HELPER */
static int fw_sysfs_fallback(struct firmware *fw, const char *name,
			     struct device *device,
			     unsigned int opt_flags,
			     int ret)
{
	/* Keep carrying over the same error */
	return ret;
}

static inline void kill_pending_fw_fallback_reqs(bool only_kill_custom) { }
static inline void fw_fallback_set_cache_timeout(void) { }
static inline void fw_fallback_set_default_timeout(void) { }

static inline int register_sysfs_loader(void)
{
	return 0;
}

static inline void unregister_sysfs_loader(void)
{
}
#endif /* CONFIG_FW_LOADER_USER_HELPER */

#endif /* __FIRMWARE_FALLBACK_H */
