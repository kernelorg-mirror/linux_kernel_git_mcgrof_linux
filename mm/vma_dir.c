// SPDX-License-Identifier: GPL-2.0-only
/*
 * Per-mm VMA directory. See linux/vma_dir.h.
 *
 * The directory is a snapshot: the VMAs of the address space in start
 * order, their start addresses in a separate array, and a lidx model over
 * the starts. It records the mmap lock sequence it was built under. A
 * lookup first checks that no writer holds or has held the mmap lock
 * since, exactly as the speculative fault path does; if so the snapshot
 * still describes the address space and the predicted VMA is handed to
 * the caller, which locks it and checks its bounds as it does for a
 * Maple Tree result. Otherwise the caller walks the tree. The directory
 * is rebuilt, under the mmap read lock, after enough stale lookups.
 */
#include <linux/vma_dir.h>
#include <linux/lidx.h>
#include <linux/mm.h>
#include <linux/mmap_lock.h>
#include <linux/rcupdate.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/percpu.h>
#include <linux/moduleparam.h>
#include <linux/kstrtox.h>
#include <linux/sysfs.h>
#include <linux/bitops.h>

DEFINE_STATIC_KEY_TRUE(vma_dir_enabled);

static int vma_dir_enable_set(const char *val, const struct kernel_param *kp)
{
	bool on;
	int ret;

	ret = kstrtobool(val, &on);
	if (ret)
		return ret;
	if (on)
		static_branch_enable(&vma_dir_enabled);
	else
		static_branch_disable(&vma_dir_enabled);
	return 0;
}

static int vma_dir_enable_get(char *buf, const struct kernel_param *kp)
{
	return sysfs_emit(buf, "%c\n",
			  static_branch_likely(&vma_dir_enabled) ? 'Y' : 'N');
}

static const struct kernel_param_ops vma_dir_enable_ops = {
	.set = vma_dir_enable_set,
	.get = vma_dir_enable_get,
};
module_param_cb(enable, &vma_dir_enable_ops, NULL, 0644);
MODULE_PARM_DESC(enable, "Use the per-mm VMA directory in the fault path");

struct vma_dir {
	struct rcu_head rcu;
	unsigned int seq;		/* mm_lock_seq the snapshot is valid for */
	unsigned long built;		/* jiffies, to judge how long it lived */
	unsigned long n;
	unsigned long *starts;		/* n */
	struct vm_area_struct **vmas;	/* n */
	struct lidx_model model;
	struct lidx_bucket *buckets;
};

struct vma_dir_stats {
	u64 lookups;	/* directory consulted */
	u64 answered;	/* candidate returned */
	u64 hits;	/* prediction exact */
	u64 windows;	/* corrected within the error window */
	u64 stale;	/* directory absent or out of date */
	u64 busy;	/* a writer held the mmap lock */
	u64 rebuilds;
	u64 skipped;	/* rebuilds withheld by the backoff */
};

static DEFINE_PER_CPU(struct vma_dir_stats, vma_dir_stats);
#define vd_stat(f) this_cpu_inc(vma_dir_stats.f)

static __always_inline unsigned long
vd_bsearch(const unsigned long *starts, unsigned long lo, unsigned long hi,
	   unsigned long addr)
{
	unsigned long low = lo, high = hi + 1, mid;

	while (high - low > 1) {
		mid = low + (high - low) / 2;
		if (starts[mid] <= addr)
			low = mid;
		else
			high = mid;
	}
	return low;
}

/*
 * Returns the VMA whose start is the last one at or below @addr, or NULL
 * when the directory cannot be used. The caller validates the VMA.
 * Runs under rcu_read_lock().
 */
struct vm_area_struct *__vma_dir_lookup(struct mm_struct *mm,
					unsigned long addr)
{
	struct vma_dir *dir;
	const struct lidx_bucket *b;
	unsigned long bstart, lo, hi, g, wlo, whi, n;
	unsigned int seq;

	dir = rcu_dereference(mm->vma_dir);
	if (!dir) {
		/*
		 * Small address spaces never get a directory; count stale
		 * lookups only where a rebuild could happen, and only every
		 * so often, so a fault does not write a shared line.
		 */
		if (READ_ONCE(mm->map_count) >= VMA_DIR_MIN_VMAS)
			WRITE_ONCE(mm->vma_dir_stale, READ_ONCE(mm->vma_dir_stale) + 1);
		return NULL;
	}
	vd_stat(lookups);
	if (!mmap_lock_speculate_try_begin(mm, &seq)) {
		vd_stat(busy);
		return NULL;
	}
	if (dir->seq != seq) {
		vd_stat(stale);
		WRITE_ONCE(mm->vma_dir_stale, READ_ONCE(mm->vma_dir_stale) + 1);
		return NULL;
	}
	if (addr < dir->model.base)
		return NULL;
	n = dir->n;
	b = lidx_bucket(&dir->model, addr, &bstart, &lo, &hi);
	g = lidx_predict(b, addr - bstart, lo, hi);
	if (dir->starts[g] <= addr && (g == n - 1 || addr < dir->starts[g + 1])) {
		vd_stat(hits);
	} else {
		wlo = g > lo + b->err + 1 ? g - b->err - 1 : lo;
		whi = min(hi, g + b->err + 1);
		if ((wlo && dir->starts[wlo] > addr) ||
		    (whi < n - 1 && dir->starts[whi + 1] <= addr))
			return NULL;
		g = vd_bsearch(dir->starts, wlo, whi, addr);
		vd_stat(windows);
	}
	vd_stat(answered);
	return dir->vmas[g];
}

static unsigned long vd_key(unsigned long pos, const void *ctx)
{
	const struct vma_dir *dir = ctx;

	return dir->starts[pos];
}

static void vma_dir_release(struct rcu_head *rcu)
{
	struct vma_dir *dir = container_of(rcu, struct vma_dir, rcu);

	kvfree(dir->starts);
	kvfree(dir->vmas);
	kvfree(dir->buckets);
	kfree(dir);
}

/* Called with the mmap read lock held. */
static void vma_dir_build(struct mm_struct *mm)
{
	VMA_ITERATOR(vmi, mm, 0);
	struct vm_area_struct *vma;
	struct vma_dir *dir, *old;
	unsigned long n = 0, i = 0;
	unsigned int nb;

	mmap_assert_locked(mm);
	n = mm->map_count;
	if (n < VMA_DIR_MIN_VMAS)
		return;

	dir = kzalloc_obj(*dir);
	if (!dir)
		return;
	dir->starts = kvmalloc_array(n, sizeof(*dir->starts), GFP_KERNEL);
	dir->vmas = kvmalloc_array(n, sizeof(*dir->vmas), GFP_KERNEL);
	nb = clamp_t(unsigned int, roundup_pow_of_two(n / 16), 16, 4096);
	dir->buckets = kvmalloc_array(nb + 1, sizeof(*dir->buckets), GFP_KERNEL);
	if (!dir->starts || !dir->vmas || !dir->buckets)
		goto fail;

	for_each_vma(vmi, vma) {
		if (i == n)
			break;
		dir->starts[i] = vma->vm_start;
		dir->vmas[i] = vma;
		i++;
	}
	dir->n = i;
	if (!i || lidx_build(&dir->model, dir->buckets, nb, i, vd_key, dir))
		goto fail;
	/* No writer can be active while we hold the read lock. */
	dir->seq = raw_read_seqcount(&mm->mm_lock_seq);
	dir->built = jiffies;

	old = rcu_dereference_protected(mm->vma_dir,
					test_bit(0, &mm->vma_dir_building));
	/*
	 * A directory that went stale within VMA_DIR_SHORT_LIFE of being
	 * built paid for itself with almost nothing: the address space is
	 * changing faster than it is faulting. Space the next rebuilds out
	 * geometrically, and drop the spacing once a directory lives.
	 */
	if (old && time_before(jiffies, old->built + VMA_DIR_SHORT_LIFE))
		mm->vma_dir_skip = min(2 * mm->vma_dir_skip + 1,
				       VMA_DIR_MAX_SKIP);
	else
		mm->vma_dir_skip = 0;
	rcu_assign_pointer(mm->vma_dir, dir);
	WRITE_ONCE(mm->vma_dir_stale, 0);
	vd_stat(rebuilds);
	if (old)
		call_rcu(&old->rcu, vma_dir_release);
	return;
fail:
	kvfree(dir->starts);
	kvfree(dir->vmas);
	kvfree(dir->buckets);
	kfree(dir);
}

/*
 * Called from the fault path before the lockless lookup, without any
 * lock held and in a context that may sleep.
 */
void __vma_dir_maybe_rebuild(struct mm_struct *mm)
{
	if (READ_ONCE(mm->map_count) < VMA_DIR_MIN_VMAS) {
		WRITE_ONCE(mm->vma_dir_stale, 0);
		return;
	}
	/*
	 * Several faulting threads can reach here at once and all take the
	 * read lock; only one may replace the directory, or the old one
	 * would be queued to RCU twice.
	 */
	if (test_and_set_bit_lock(0, &mm->vma_dir_building))
		return;
	if (mm->vma_dir_skip) {
		/* Backing off: let another VMA_DIR_STALE_LIMIT lookups pass. */
		mm->vma_dir_skip--;
		WRITE_ONCE(mm->vma_dir_stale, 0);
		vd_stat(skipped);
	} else if (mmap_read_trylock(mm)) {
		vma_dir_build(mm);
		mmap_read_unlock(mm);
	}
	clear_bit_unlock(0, &mm->vma_dir_building);
}

void vma_dir_init(struct mm_struct *mm)
{
	RCU_INIT_POINTER(mm->vma_dir, NULL);
	mm->vma_dir_stale = 0;
	mm->vma_dir_skip = 0;
	mm->vma_dir_building = 0;
}

void vma_dir_free(struct mm_struct *mm)
{
	struct vma_dir *dir = rcu_dereference_protected(mm->vma_dir, true);

	if (!dir)
		return;
	RCU_INIT_POINTER(mm->vma_dir, NULL);
	call_rcu(&dir->rcu, vma_dir_release);
}

static int vma_dir_stats_show(struct seq_file *s, void *unused)
{
	struct vma_dir_stats sum = {};
	int cpu;

	for_each_possible_cpu(cpu) {
		const struct vma_dir_stats *c = per_cpu_ptr(&vma_dir_stats, cpu);

		sum.lookups += c->lookups;
		sum.answered += c->answered;
		sum.hits += c->hits;
		sum.windows += c->windows;
		sum.stale += c->stale;
		sum.busy += c->busy;
		sum.rebuilds += c->rebuilds;
		sum.skipped += c->skipped;
	}
	seq_printf(s, "lookups %llu\nanswered %llu\nhits %llu\nwindows %llu\nstale %llu\nbusy %llu\nrebuilds %llu\nskipped %llu\n",
		   sum.lookups, sum.answered, sum.hits, sum.windows, sum.stale,
		   sum.busy, sum.rebuilds, sum.skipped);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(vma_dir_stats);

static int __init vma_dir_debugfs_init(void)
{
	debugfs_create_file("vma_dir", 0444, NULL, NULL, &vma_dir_stats_fops);
	return 0;
}
late_initcall(vma_dir_debugfs_init);
