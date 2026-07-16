// SPDX-License-Identifier: GPL-2.0
/*
 * Block-layer per-queue iobuf pool for higher-order folios.
 *
 * Creates a bounded pool of compound folios sized to match device geometry
 * so that passthrough and DIO paths avoid huge scatter/gather lists.
 *
 * Order-selection triggers (any one is sufficient to create a pool):
 *   BLK_IOBUF_REASON_IO_MIN   - io_min > PAGE_SIZE
 *   BLK_IOBUF_REASON_SEG_GEOM - max_hw_bytes / max_segments > PAGE_SIZE
 *   BLK_IOBUF_REASON_IO_OPT   - io_opt >= PAGE_SIZE and admin preferred it
 */

#define CREATE_TRACE_POINTS
#include <trace/events/block_iobuf.h>

#include <linux/blk-iobuf.h>
#include <linux/blkdev.h>
#include <linux/export.h>
#include <linux/gfp.h>
#include <linux/log2.h>
#include <linux/mm.h>
#include <linux/moduleparam.h>
#include <linux/slab.h>

/* --- Module parameters (also used by nvme_core for nvme_core.iobuf_pool=*) */

unsigned int blk_iobuf_pool_max_order = 5;
EXPORT_SYMBOL_GPL(blk_iobuf_pool_max_order);
module_param_named(iobuf_pool_max_order, blk_iobuf_pool_max_order, uint, 0644);
MODULE_PARM_DESC(iobuf_pool_max_order,
		 "Maximum folio order for block iobuf pools (default 5 = 128 KiB)");

unsigned int blk_iobuf_pool_min_folios = 16;
EXPORT_SYMBOL_GPL(blk_iobuf_pool_min_folios);
module_param_named(iobuf_pool_min_folios, blk_iobuf_pool_min_folios, uint, 0644);
MODULE_PARM_DESC(iobuf_pool_min_folios,
		 "Minimum folios in block iobuf pool (default 16)");

bool blk_iobuf_pool_prefer_io_opt;
EXPORT_SYMBOL_GPL(blk_iobuf_pool_prefer_io_opt);
module_param_named(iobuf_pool_prefer_io_opt, blk_iobuf_pool_prefer_io_opt,
		   bool, 0644);
MODULE_PARM_DESC(iobuf_pool_prefer_io_opt,
		 "Use io_opt to increase pool folio order (default off)");

/*
 * Operator override, independent of device geometry.  Device NVMe hints like
 * io_opt are performance hints; io_opt==0 means "unspecified", not "a pool is
 * pointless".  When the benefit is host-side (io_uring registered pool buffers,
 * deterministic large I/O, large folios), the operator -- not the drive -- has
 * the reason.  -1 = auto (use geometry); 0 = no pool; >0 = force this folio
 * order (capped by iobuf_pool_max_order).  Does NOT touch queue limits.
 */
int blk_iobuf_pool_force_order = -1;
module_param_named(iobuf_pool_force_order, blk_iobuf_pool_force_order, int, 0644);
MODULE_PARM_DESC(iobuf_pool_force_order,
		 "Force pool folio order regardless of geometry (-1=auto, 0=off, >0=order)");

/* --- Core helpers --- */

bool blk_queue_iobuf_pool_enabled(struct request_queue *q)
{
	return q->iobuf_pool != NULL;
}
EXPORT_SYMBOL_GPL(blk_queue_iobuf_pool_enabled);

struct blk_iobuf_pool *blk_queue_get_iobuf_pool(struct request_queue *q)
{
	return q->iobuf_pool;
}
EXPORT_SYMBOL_GPL(blk_queue_get_iobuf_pool);

unsigned int blk_iobuf_pool_folio_size(struct request_queue *q)
{
	struct blk_iobuf_pool *pool = q->iobuf_pool;

	return pool ? pool->folio_size : 0;
}
EXPORT_SYMBOL_GPL(blk_iobuf_pool_folio_size);

void blk_iobuf_inc_fallback(struct request_queue *q)
{
	struct blk_iobuf_pool *pool = q->iobuf_pool;

	if (pool)
		atomic64_inc(&pool->fallback);
}
EXPORT_SYMBOL_GPL(blk_iobuf_inc_fallback);

/**
 * blk_iobuf_choose_order - select folio order for a queue's iobuf pool
 * @lim:         committed queue limits
 * @max_order:   hard cap on order (caller-supplied, typically from module param)
 * @reason_mask: out: set of BLK_IOBUF_REASON_* flags that drove the decision
 *
 * Returns the chosen order, or 0 if no higher-order pool is warranted.
 */
unsigned int blk_iobuf_choose_order(const struct queue_limits *lim,
				    unsigned int max_order,
				    unsigned int *reason_mask)
{
	unsigned long max_hw_bytes, seg_geom, target;
	unsigned int reasons = 0;

	if (reason_mask)
		*reason_mask = 0;

	if (!max_order)
		max_order = blk_iobuf_pool_max_order;

	/*
	 * Operator override wins over geometry.  This is how a plain 4 KiB-IU
	 * NVMe (io_min=4096, io_opt=0, small seg_geom) can still get a pool for
	 * io_uring registered large-I/O buffers: the reason is the workload, not
	 * a device hint.
	 */
	if (blk_iobuf_pool_force_order >= 0) {
		unsigned int forced = blk_iobuf_pool_force_order;

		if (!forced)
			return 0;
		forced = min_t(unsigned int, forced, max_order);
		if (reason_mask)
			*reason_mask = BLK_IOBUF_REASON_FORCE;
		return forced;
	}

	max_hw_bytes = (unsigned long)lim->max_hw_sectors << SECTOR_SHIFT;
	if (!max_hw_bytes || !lim->max_segments)
		return 0;

	seg_geom = DIV_ROUND_UP(max_hw_bytes,
				max_t(unsigned int, lim->max_segments, 1));

	target = PAGE_SIZE;

	if (lim->io_min > PAGE_SIZE) {
		target = max(target, rounddown_pow_of_two(lim->io_min));
		reasons |= BLK_IOBUF_REASON_IO_MIN;
	}

	if (seg_geom > PAGE_SIZE) {
		target = max(target, roundup_pow_of_two(seg_geom));
		reasons |= BLK_IOBUF_REASON_SEG_GEOM;
	}

	if (lim->io_opt >= PAGE_SIZE && blk_iobuf_pool_prefer_io_opt) {
		target = max(target, rounddown_pow_of_two(lim->io_opt));
		reasons |= BLK_IOBUF_REASON_IO_OPT;
	}

	if (!reasons)
		return 0;

	target = clamp(target, (unsigned long)PAGE_SIZE,
		       (unsigned long)(PAGE_SIZE << max_order));

	if (reason_mask)
		*reason_mask = reasons;

	return get_order(target);
}
EXPORT_SYMBOL_GPL(blk_iobuf_choose_order);

/**
 * blk_queue_init_iobuf_pool - create and attach an iobuf pool to @q
 * @q:   request queue
 * @lim: current queue limits (after commit)
 * @cfg: pool configuration; NULL uses global defaults
 *
 * Idempotent: if a pool already exists with the same order, it is kept.
 * If it would need a different order, the old pool is destroyed first.
 *
 * Returns 0 on success.  A failure in "auto" mode should not abort I/O;
 * callers that want permissive behaviour should ignore a non-zero return.
 */
int blk_queue_init_iobuf_pool(struct request_queue *q,
			      const struct queue_limits *lim,
			      const struct blk_iobuf_pool_config *cfg)
{
	static const struct blk_iobuf_pool_config default_cfg = {};
	struct blk_iobuf_pool *pool;
	unsigned int order, reasons;
	unsigned int max_order, min_nr;
	int ret;

	if (!cfg)
		cfg = &default_cfg;

	max_order = cfg->max_order ? cfg->max_order : blk_iobuf_pool_max_order;
	min_nr    = cfg->min_nr    ? cfg->min_nr    : blk_iobuf_pool_min_folios;

	order = blk_iobuf_choose_order(lim, max_order, &reasons);
	if (!order)
		return 0;

	/* Reuse existing pool if order matches */
	if (q->iobuf_pool && q->iobuf_pool->order == order)
		return 0;

	/* Tear down any mismatched pool */
	if (q->iobuf_pool) {
		blk_queue_exit_iobuf_pool(q);
		q->iobuf_pool = NULL;
	}

	pool = kzalloc(sizeof(*pool), GFP_KERNEL);
	if (!pool)
		return -ENOMEM;

	ret = mempool_init_folio_pool(&pool->folio_pool, min_nr, order);
	if (ret) {
		kfree(pool);
		return ret;
	}

	pool->q          = q;
	pool->order      = order;
	pool->folio_size = PAGE_SIZE << order;
	pool->min_nr     = min_nr;
	pool->max_nr     = min_nr;	/* fixed for now; grow on demand later */
	pool->reasons    = reasons;
	atomic_set(&pool->in_use, 0);
	atomic64_set(&pool->allocs, 0);
	atomic64_set(&pool->frees, 0);
	atomic64_set(&pool->misses, 0);
	atomic64_set(&pool->fallback, 0);

	q->iobuf_pool = pool;
	/* Bump generation so io_uring buffers can detect stale associations */
	q->limits_gen++;

	trace_block_iobuf_pool_create(q, order, pool->folio_size, reasons,
				      min_nr);
	return 0;
}
EXPORT_SYMBOL_GPL(blk_queue_init_iobuf_pool);

void blk_queue_exit_iobuf_pool(struct request_queue *q)
{
	struct blk_iobuf_pool *pool = q->iobuf_pool;

	if (!pool)
		return;

	mempool_exit(&pool->folio_pool);
	kfree(pool);
	q->iobuf_pool = NULL;
}
EXPORT_SYMBOL_GPL(blk_queue_exit_iobuf_pool);

/**
 * blk_iobuf_alloc_folio - allocate a folio from the queue's iobuf pool
 * @q:   request queue with an attached iobuf pool
 * @gfp: allocation flags; pass __GFP_NOWAIT to avoid blocking
 *
 * Returns a folio on success, NULL if the pool is absent or allocation fails.
 */
struct folio *blk_iobuf_alloc_folio(struct request_queue *q, gfp_t gfp)
{
	struct blk_iobuf_pool *pool = q->iobuf_pool;
	struct folio *folio;

	if (!pool)
		return NULL;

	folio = mempool_alloc(&pool->folio_pool, gfp);
	if (!folio) {
		atomic64_inc(&pool->misses);
		trace_block_iobuf_alloc(q, pool->order, gfp, false);
		return NULL;
	}

	atomic64_inc(&pool->allocs);
	atomic_inc(&pool->in_use);
	trace_block_iobuf_alloc(q, pool->order, gfp, true);
	return folio;
}
EXPORT_SYMBOL_GPL(blk_iobuf_alloc_folio);

void blk_iobuf_free_folio(struct request_queue *q, struct folio *folio)
{
	struct blk_iobuf_pool *pool = q->iobuf_pool;

	if (WARN_ON_ONCE(!pool || !folio))
		return;

	trace_block_iobuf_free(q, pool->order);
	atomic64_inc(&pool->frees);
	atomic_dec(&pool->in_use);
	mempool_free(folio, &pool->folio_pool);
}
EXPORT_SYMBOL_GPL(blk_iobuf_free_folio);

/* --- Sysfs attribute show helpers (called from blk-sysfs.c) --- */

ssize_t blk_iobuf_sysfs_enabled_show(struct gendisk *disk, char *page)
{
	return sysfs_emit(page, "%d\n",
			  blk_queue_iobuf_pool_enabled(disk->queue) ? 1 : 0);
}

ssize_t blk_iobuf_sysfs_order_show(struct gendisk *disk, char *page)
{
	struct blk_iobuf_pool *pool = disk->queue->iobuf_pool;

	return sysfs_emit(page, "%u\n", pool ? pool->order : 0);
}

ssize_t blk_iobuf_sysfs_folio_size_show(struct gendisk *disk, char *page)
{
	struct blk_iobuf_pool *pool = disk->queue->iobuf_pool;

	return sysfs_emit(page, "%u\n", pool ? pool->folio_size : 0);
}

ssize_t blk_iobuf_sysfs_reasons_show(struct gendisk *disk, char *page)
{
	struct blk_iobuf_pool *pool = disk->queue->iobuf_pool;

	return sysfs_emit(page, "0x%x\n", pool ? pool->reasons : 0);
}

ssize_t blk_iobuf_sysfs_min_folios_show(struct gendisk *disk, char *page)
{
	struct blk_iobuf_pool *pool = disk->queue->iobuf_pool;

	return sysfs_emit(page, "%u\n", pool ? pool->min_nr : 0);
}

ssize_t blk_iobuf_sysfs_in_use_show(struct gendisk *disk, char *page)
{
	struct blk_iobuf_pool *pool = disk->queue->iobuf_pool;

	return sysfs_emit(page, "%d\n", pool ? atomic_read(&pool->in_use) : 0);
}

ssize_t blk_iobuf_sysfs_allocs_show(struct gendisk *disk, char *page)
{
	struct blk_iobuf_pool *pool = disk->queue->iobuf_pool;

	return sysfs_emit(page, "%lld\n",
			  pool ? atomic64_read(&pool->allocs) : 0LL);
}

ssize_t blk_iobuf_sysfs_misses_show(struct gendisk *disk, char *page)
{
	struct blk_iobuf_pool *pool = disk->queue->iobuf_pool;

	return sysfs_emit(page, "%lld\n",
			  pool ? atomic64_read(&pool->misses) : 0LL);
}

ssize_t blk_iobuf_sysfs_fallbacks_show(struct gendisk *disk, char *page)
{
	struct blk_iobuf_pool *pool = disk->queue->iobuf_pool;

	return sysfs_emit(page, "%lld\n",
			  pool ? atomic64_read(&pool->fallback) : 0LL);
}
