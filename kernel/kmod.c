/*
 * kmod - the kernel module loader
 */
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/sched/task.h>
#include <linux/binfmts.h>
#include <linux/syscalls.h>
#include <linux/unistd.h>
#include <linux/kmod.h>
#include <linux/slab.h>
#include <linux/completion.h>
#include <linux/cred.h>
#include <linux/file.h>
#include <linux/fdtable.h>
#include <linux/workqueue.h>
#include <linux/security.h>
#include <linux/mount.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/resource.h>
#include <linux/notifier.h>
#include <linux/suspend.h>
#include <linux/rwsem.h>
#include <linux/ptrace.h>
#include <linux/async.h>
#include <linux/uaccess.h>

#include <trace/events/module.h>

/*
 * Assuming:
 *
 * threads = div64_u64((u64) totalram_pages * (u64) PAGE_SIZE,
 *		       (u64) THREAD_SIZE * 8UL);
 *
 * If you need less than 50 threads would mean we're dealing with systems
 * smaller than 3200 pages. This assuems you are capable of having ~13M memory,
 * and this would only be an be an upper limit, after which the OOM killer
 * would take effect. Systems like these are very unlikely if modules are
 * enabled.
 */
#define MAX_KMOD_CONCURRENT 50
static atomic_t kmod_concurrent_max = ATOMIC_INIT(MAX_KMOD_CONCURRENT);
static DECLARE_WAIT_QUEUE_HEAD(kmod_wq);

bool finished_loading(const char *name);
int module_wait_until_finished(const char *name);
struct module *find_module_all(const char *name, size_t len,
			       bool even_unformed);

/*
 * This is a restriction on having *all* MAX_KMOD_CONCURRENT threads
 * running at the same time without returning. When this happens we
 * believe you've somehow ended up with a recursive module dependency
 * creating a loop.
 *
 * We have no option but to fail.
 *
 * Userspace should proactively try to detect and prevent these.
 */
#define MAX_KMOD_ALL_BUSY_TIMEOUT 5

/*
	modprobe_path is set via /proc/sys.
*/
char modprobe_path[KMOD_PATH_LEN] = "/sbin/modprobe";

static void free_modprobe_argv(struct subprocess_info *info)
{
	kfree(info->argv[3]); /* check call_modprobe() */
	kfree(info->argv);
}

static int call_modprobe(char *module_name, int wait)
{
	struct subprocess_info *info;
	static char *envp[] = {
		"HOME=/",
		"TERM=linux",
		"PATH=/sbin:/usr/sbin:/bin:/usr/bin",
		NULL
	};

	char **argv = kmalloc(sizeof(char *[5]), GFP_KERNEL);
	if (!argv)
		goto out;

	module_name = kstrdup(module_name, GFP_KERNEL);
	if (!module_name)
		goto free_argv;

	argv[0] = modprobe_path;
	argv[1] = "-q";
	argv[2] = "--";
	argv[3] = module_name;	/* check free_modprobe_argv() */
	argv[4] = NULL;

	info = call_usermodehelper_setup(modprobe_path, argv, envp, GFP_KERNEL,
					 NULL, free_modprobe_argv, NULL);
	if (!info)
		goto free_module_name;

	return call_usermodehelper_exec(info, wait | UMH_KILLABLE);

free_module_name:
	kfree(module_name);
free_argv:
	kfree(argv);
out:
	return -ENOMEM;
}

static bool kmod_exists(char *name)
{
	struct module *mod;

	mutex_lock(&module_mutex);
	mod = find_module_all(name, strlen(name), true);
	mutex_unlock(&module_mutex);

	if (mod)
		return true;

	return false;
}

/*
 * The assumption is this must be a module, it could still not be live though
 * since kmod <= 19 returns 0 even if it was not ready yet.  Allow for force
 * wait check in case you are stuck on old userspace.
 */
static int wait_for_kmod(char *name)
{
	int ret = 0;

	if (!finished_loading(name))
		ret = module_wait_until_finished(name);

	return ret;
}

/*
 * kmod <= 19 will tell us modprobe returned 0 even if the module
 * is not ready yet, it does this because it checks the /sys/module/mod-name
 * directory and if its created but the /sys/module/mod-name/initstate is not
 * created it assumes you have a built-in driver. At this point the module
 * is still unformed, and telling the kernel at any point via request_module()
 * will cause issues given a lot of places in the kernel assert that the driver
 * will be present and ready. We need to account for this.
 *
 * If we had a module and even if buggy modprobe returned 0, we know we'd at
 * least have a dangling kmod entry we could fetch.
 *
 * If modprobe returned 0 and we cannot find a kmod entry this is a good
 * indicator your by userspace and kernel space that what you have is built-in.
 *
 * If modprobe returned 0 and we can find a kmod entry we should air on the
 * side of caution and wait for the module to become ready or going.
 *
 * In the worst case, for built-in, we have to check on the module list for
 * as many aliases possible the kernel gives the module, if that is n, that
 * n traversals on the module list.
 */
static int finished_kmod_load(char *name, bool wait)
{
	int ret = -ENOENT;

	if (kmod_exists(name)) {
		if (finished_loading(name))
			return 0;
		else if (wait)
			ret = wait_for_kmod(name);
		else
			ret = -EINPROGRESS;
	}

	return ret;
}

/**
 * __request_module - try to load a kernel module
 * @wait: wait (or not) for the operation to complete
 * @fmt: printf style format string for the name of the module
 * @...: arguments as specified in the format string
 *
 * Load a module using the user mode module loader. The function returns
 * zero on success or a negative errno code or positive exit code from
 * "modprobe" on failure. Note that a successful module load does not mean
 * the module did not then unload and exit on an error of its own. Callers
 * must check that the service they requested is now available not blindly
 * invoke it.
 *
 * If module auto-loading support is disabled then this function
 * becomes a no-operation.
 */
int __request_module(bool wait, const char *fmt, ...)
{
	va_list args;
	char module_name[MODULE_NAME_LEN];
	int ret;

	/*
	 * We don't allow synchronous module loading from async.  Module
	 * init may invoke async_synchronize_full() which will end up
	 * waiting for this task which already is waiting for the module
	 * loading to complete, leading to a deadlock.
	 */
	WARN_ON_ONCE(wait && current_is_async());

	if (!modprobe_path[0])
		return 0;

	va_start(args, fmt);
	ret = vsnprintf(module_name, MODULE_NAME_LEN, fmt, args);
	va_end(args);
	if (ret >= MODULE_NAME_LEN)
		return -ENAMETOOLONG;

	ret = security_kernel_module_request(module_name);
	if (ret)
		return ret;

	ret = finished_kmod_load(module_name, wait);
	if (!ret) {
		pr_debug_ratelimited("request_module: module %s already loaded\n", module_name);
		return 0;
	}

	if (atomic_dec_if_positive(&kmod_concurrent_max) < 0) {
		pr_warn_ratelimited("request_module: kmod_concurrent_max (%u) close to 0 (max_modprobes: %u), for module %s, throttling...",
				    atomic_read(&kmod_concurrent_max),
				    MAX_KMOD_CONCURRENT, module_name);
		ret = wait_event_killable_timeout(kmod_wq,
						  atomic_dec_if_positive(&kmod_concurrent_max) >= 0,
						  MAX_KMOD_ALL_BUSY_TIMEOUT * HZ);
		if (!ret) {
			pr_warn_ratelimited("request_module: modprobe %s cannot be processed, kmod busy with %d threads for more than %d seconds now",
					    module_name, MAX_KMOD_CONCURRENT, MAX_KMOD_ALL_BUSY_TIMEOUT);
			return -ETIME;
		} else if (ret == -ERESTARTSYS) {
			pr_warn_ratelimited("request_module: sigkill sent for modprobe %s, giving up", module_name);
			return ret;
		}
	}

	trace_module_request(module_name, wait, _RET_IP_);

	ret = call_modprobe(module_name, wait ? UMH_WAIT_PROC : UMH_WAIT_EXEC);
	if (!ret)
		ret = finished_kmod_load(module_name, wait);

	atomic_inc(&kmod_concurrent_max);
	wake_up(&kmod_wq);

	return ret;
}
EXPORT_SYMBOL(__request_module);
