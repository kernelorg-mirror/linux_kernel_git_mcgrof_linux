// SPDX-License-Identifier: GPL-2.0
/*
 * Block-layer bounded iobuf folio pool.
 *
 * The pool preallocates exactly @capacity folios of exactly @order and hands
 * out only that inventory. Checkout is strict -- it draws from the
 * preallocated reserve via mempool_alloc_preallocated() and never falls back
 * to the page allocator -- so exhaustion is a deterministic -ENOBUFS rather
 * than an opportunistic high-order allocation under pressure. A logical buffer
 * may need several folios, so checkout is all-or-nothing under a pool lock:
 * either every folio is handed out or none is.
 *
 * Lifetime: the pool is refcounted. A folio is returned to the exact pool it
 * came from (callers pass the pool to the free path, there is no lookup), and
 * a holder of checked-out folios keeps a reference until it has returned them,
 * so the object outlives its outstanding inventory. The final free is deferred
 * an RCU grace period so a lookup that fetched the pointer under rcu_read_lock
 * can run kref_get_unless_zero() against it safely; the queue attachment that
 * relies on that is added separately.
 */
#include <linux/blk-iobuf.h>
#include <linux/blkdev.h>
#include <linux/blk-mq.h>
#include <linux/blk-mq-dma.h>
#include <linux/bvec.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/dma-map-ops.h>
#include <linux/gfp.h>
#include <linux/highmem.h>
#include <linux/io_uring/cmd.h>
#include <linux/kref.h>
#include <linux/mempool.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/overflow.h>
#include <linux/rcupdate.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>

struct blk_iobuf_pool {
	struct kref		ref;
	mempool_t		folio_pool;
	spinlock_t		lock;		/* serializes all-or-nothing checkout */
	unsigned int		order;
	unsigned int		folio_size;
	unsigned int		capacity;	/* preallocated inventory, fixed */
	atomic_t		in_use;
	atomic_long_t		high_water;
	atomic64_t		alloc_failures;
	struct rcu_head		rcu;
};

/* Cap the per-pool byte total so a bad order/count cannot reserve absurd RAM. */
#define BLK_IOBUF_POOL_MAX_BYTES	SZ_1G		/* per pool */

static void *blk_iobuf_folio_alloc_cb(gfp_t gfp, void *pool_data)
{
	unsigned int order = (unsigned int)(unsigned long)pool_data;

	return folio_alloc(gfp, order);
}

static void blk_iobuf_folio_free_cb(void *element, void *pool_data)
{
	folio_put((struct folio *)element);
}

/**
 * blk_iobuf_pool_create - preallocate a bounded higher-order folio pool
 * @order:     folio order every allocation returns (0..MAX_PAGE_ORDER)
 * @nr_folios: exact number of folios to preallocate (the capacity)
 *
 * Returns a pool with one reference held by the caller, or an ERR_PTR:
 * -EINVAL for a zero capacity, an impossible order, or a byte total past the
 * per-pool cap; -ENOMEM if the inventory could not be preallocated.
 */
struct blk_iobuf_pool *blk_iobuf_pool_create(unsigned int order,
					     unsigned int nr_folios)
{
	struct blk_iobuf_pool *pool;
	size_t folio_bytes, total_bytes;

	if (!nr_folios || order > MAX_PAGE_ORDER)
		return ERR_PTR(-EINVAL);

	folio_bytes = (size_t)PAGE_SIZE << order;
	if (check_mul_overflow(folio_bytes, (size_t)nr_folios, &total_bytes) ||
	    total_bytes > BLK_IOBUF_POOL_MAX_BYTES)
		return ERR_PTR(-EINVAL);

	pool = kzalloc(sizeof(*pool), GFP_KERNEL);
	if (!pool)
		return ERR_PTR(-ENOMEM);

	kref_init(&pool->ref);
	spin_lock_init(&pool->lock);
	pool->order = order;
	pool->folio_size = PAGE_SIZE << order;
	pool->capacity = nr_folios;
	atomic_set(&pool->in_use, 0);
	atomic_long_set(&pool->high_water, 0);

	/*
	 * A mempool with these callbacks preallocates nr_folios folios into its
	 * reserve; mempool_alloc_preallocated() then serves strictly from that
	 * reserve and never invokes the callback (buddy) allocator.
	 */
	if (mempool_init(&pool->folio_pool, nr_folios, blk_iobuf_folio_alloc_cb,
			 blk_iobuf_folio_free_cb,
			 (void *)(unsigned long)order)) {
		kfree(pool);
		return ERR_PTR(-ENOMEM);
	}
	return pool;
}
EXPORT_SYMBOL_GPL(blk_iobuf_pool_create);

static void blk_iobuf_pool_release(struct kref *ref)
{
	struct blk_iobuf_pool *pool =
		container_of(ref, struct blk_iobuf_pool, ref);

	/*
	 * The last reference must not drop while folios are still checked out;
	 * a holder returns its folios before dropping its reference.
	 */
	WARN_ON_ONCE(atomic_read(&pool->in_use) != 0);
	mempool_exit(&pool->folio_pool);
	kfree_rcu(pool, rcu);
}

/**
 * blk_iobuf_pool_get - take an additional reference on a pool
 * @pool: pool to reference; the caller must already hold a reference
 *
 * Returns @pool for chaining.
 */
struct blk_iobuf_pool *blk_iobuf_pool_get(struct blk_iobuf_pool *pool)
{
	kref_get(&pool->ref);
	return pool;
}
EXPORT_SYMBOL_GPL(blk_iobuf_pool_get);

/**
 * blk_iobuf_pool_put - drop a reference; the last put destroys the pool
 * @pool: pool to release (may be NULL)
 */
void blk_iobuf_pool_put(struct blk_iobuf_pool *pool)
{
	if (pool)
		kref_put(&pool->ref, blk_iobuf_pool_release);
}
EXPORT_SYMBOL_GPL(blk_iobuf_pool_put);

static void blk_iobuf_note_high_water(struct blk_iobuf_pool *pool, int in_use)
{
	long hw = atomic_long_read(&pool->high_water);

	while (in_use > hw &&
	       !atomic_long_try_cmpxchg_relaxed(&pool->high_water, &hw, in_use))
		;
}

/**
 * blk_iobuf_pool_alloc_batch - strict all-or-nothing checkout
 * @pool:      pool to draw from
 * @folios:    array of at least @nr_folios pointers, filled on success
 * @nr_folios: number of folios to check out
 *
 * Draws @nr_folios folios from the preallocated inventory under the pool lock,
 * so two concurrent checkouts cannot each grab a partial set. Returns 0 with
 * @folios filled, or -ENOBUFS with every entry of @folios set to NULL and every
 * partially-drawn folio returned. Never allocates from the page allocator.
 */
int blk_iobuf_pool_alloc_batch(struct blk_iobuf_pool *pool,
			       struct folio **folios, unsigned int nr_folios)
{
	unsigned int i;
	int in_use;

	if (!nr_folios)
		return 0;

	spin_lock(&pool->lock);
	for (i = 0; i < nr_folios; i++) {
		folios[i] = mempool_alloc_preallocated(&pool->folio_pool);
		if (!folios[i]) {
			unsigned int j;

			for (j = 0; j < i; j++)
				mempool_free(folios[j], &pool->folio_pool);
			spin_unlock(&pool->lock);
			memset(folios, 0, array_size(nr_folios, sizeof(*folios)));
			atomic64_inc(&pool->alloc_failures);
			return -ENOBUFS;
		}
	}
	in_use = atomic_add_return(nr_folios, &pool->in_use);
	spin_unlock(&pool->lock);

	blk_iobuf_note_high_water(pool, in_use);
	return 0;
}
EXPORT_SYMBOL_GPL(blk_iobuf_pool_alloc_batch);

/**
 * blk_iobuf_pool_free_batch - return folios to the exact pool that produced them
 * @pool:      pool the folios were checked out of
 * @folios:    array of @nr_folios folios to return
 * @nr_folios: number of folios
 */
void blk_iobuf_pool_free_batch(struct blk_iobuf_pool *pool,
			       struct folio **folios, unsigned int nr_folios)
{
	unsigned int i;

	if (!nr_folios)
		return;
	for (i = 0; i < nr_folios; i++)
		mempool_free(folios[i], &pool->folio_pool);
	atomic_sub(nr_folios, &pool->in_use);
}
EXPORT_SYMBOL_GPL(blk_iobuf_pool_free_batch);

unsigned int blk_iobuf_pool_order(const struct blk_iobuf_pool *pool)
{
	return pool->order;
}
EXPORT_SYMBOL_GPL(blk_iobuf_pool_order);

unsigned int blk_iobuf_pool_folio_size(const struct blk_iobuf_pool *pool)
{
	return pool->folio_size;
}
EXPORT_SYMBOL_GPL(blk_iobuf_pool_folio_size);

unsigned int blk_iobuf_pool_capacity(const struct blk_iobuf_pool *pool)
{
	return pool->capacity;
}
EXPORT_SYMBOL_GPL(blk_iobuf_pool_capacity);

unsigned int blk_iobuf_pool_in_use(const struct blk_iobuf_pool *pool)
{
	return atomic_read(&pool->in_use);
}
EXPORT_SYMBOL_GPL(blk_iobuf_pool_in_use);

unsigned int blk_iobuf_pool_available(const struct blk_iobuf_pool *pool)
{
	int avail = (int)pool->capacity - atomic_read(&pool->in_use);

	return avail > 0 ? avail : 0;
}
EXPORT_SYMBOL_GPL(blk_iobuf_pool_available);

unsigned int blk_iobuf_pool_high_water(const struct blk_iobuf_pool *pool)
{
	return atomic_long_read(&pool->high_water);
}
EXPORT_SYMBOL_GPL(blk_iobuf_pool_high_water);

u64 blk_iobuf_pool_alloc_failures(const struct blk_iobuf_pool *pool)
{
	return atomic64_read(&pool->alloc_failures);
}
EXPORT_SYMBOL_GPL(blk_iobuf_pool_alloc_failures);

/*
 * Queue attachment.
 *
 * q->iobuf_pool is RCU-published and the queue owns one reference on the pool
 * it points at. q->iobuf_pool_lock serializes writers (set and clear) for real
 * -- it is the lockdep condition on the rcu_replace_pointer below, not a bare
 * "true". Lookups are lock-free.
 */

/**
 * blk_queue_set_iobuf_pool - attach or replace a queue's iobuf pool
 * @q:    request queue
 * @pool: pool to attach
 *
 * Takes its own reference on @pool; the caller keeps its reference and must
 * drop it. Drops the queue's reference to any previously attached pool after
 * publishing the new one, so an outstanding provider buffer that still holds a
 * reference to the old pool keeps it -- and its folios -- alive. Returns 0.
 */
int blk_queue_set_iobuf_pool(struct request_queue *q,
			     struct blk_iobuf_pool *pool)
{
	struct blk_iobuf_pool *old;

	blk_iobuf_pool_get(pool);
	mutex_lock(&q->iobuf_pool_lock);
	old = rcu_replace_pointer(q->iobuf_pool, pool,
				  lockdep_is_held(&q->iobuf_pool_lock));
	mutex_unlock(&q->iobuf_pool_lock);
	if (old)
		blk_iobuf_pool_put(old);
	return 0;
}
EXPORT_SYMBOL_GPL(blk_queue_set_iobuf_pool);

/**
 * blk_queue_clear_iobuf_pool - detach and drop a queue's iobuf pool
 * @q: request queue
 */
void blk_queue_clear_iobuf_pool(struct request_queue *q)
{
	struct blk_iobuf_pool *old;

	mutex_lock(&q->iobuf_pool_lock);
	old = rcu_replace_pointer(q->iobuf_pool, NULL,
				  lockdep_is_held(&q->iobuf_pool_lock));
	mutex_unlock(&q->iobuf_pool_lock);
	if (old)
		blk_iobuf_pool_put(old);
}
EXPORT_SYMBOL_GPL(blk_queue_clear_iobuf_pool);

/**
 * blk_queue_get_iobuf_pool - take a reference to a queue's iobuf pool
 * @q: request queue
 *
 * Returns a referenced pool the caller must release with
 * blk_iobuf_pool_put(), or NULL if the queue has no pool. Safe against
 * concurrent replacement and teardown: the pointer is fetched under RCU and
 * the reference taken with kref_get_unless_zero(), and the pool's final free
 * is RCU-deferred.
 */
struct blk_iobuf_pool *blk_queue_get_iobuf_pool(struct request_queue *q)
{
	struct blk_iobuf_pool *pool;

	rcu_read_lock();
	pool = rcu_dereference(q->iobuf_pool);
	if (pool && !kref_get_unless_zero(&pool->ref))
		pool = NULL;
	rcu_read_unlock();
	return pool;
}
EXPORT_SYMBOL_GPL(blk_queue_get_iobuf_pool);

/*
 * Provider glue: allocate a logical fixed buffer from a queue's pool and
 * install it into an io_uring sparse fixed-buffer slot.
 */

/*
 * @dma_dev/@premap hold an optional persistent DMA mapping of the pool folios
 * to one device, established once at registration and torn down at release.
 * This is the kernel analogue of SPDK's map-once model: while the buffer is
 * registered its device DMA addresses are stable, so the NVMe request path can
 * build PRP/SGL lists from them without allocating an IOVA per I/O -- the
 * per-I/O IOVA allocation being what forces the 128 KiB command clamp on
 * translating-IOMMU hosts.  @dma_dev is NULL for an ordinary (dynamically
 * mapped) buffer, in which case @premap is unused and behaviour is unchanged.
 */
#define BLK_IOBUF_REG_MAGIC	0x42494f42u	/* "BIOB" */

struct blk_iobuf_reg {
	unsigned int		magic;
	struct blk_iobuf_pool	*pool;
	struct folio		**folios;
	struct bio_vec		*bvecs;
	unsigned int		nr_folios;
	struct device		*dma_dev;
	struct blk_dma_premap	premap;
};

/*
 * Recover the retained DMA mapping of a pool-backed fixed buffer from the
 * opaque provider data io_uring hands back for a kernel buffer (see
 * io_uring_cmd_kbuf_priv()).  Returns the retained mapping only when @kbuf_priv
 * is one of our registrations premapped to @dma_dev; NULL otherwise.  The magic
 * guards against being handed some other provider's kernel buffer.
 */
struct blk_dma_premap *blk_iobuf_fixed_buf_premap(void *kbuf_priv,
						  struct device *dma_dev)
{
	struct blk_iobuf_reg *reg = kbuf_priv;

	if (!reg || reg->magic != BLK_IOBUF_REG_MAGIC)
		return NULL;
	if (!reg->dma_dev || reg->dma_dev != dma_dev)
		return NULL;
	return &reg->premap;
}
EXPORT_SYMBOL_GPL(blk_iobuf_fixed_buf_premap);

/*
 * Map the checked-out folios to @dma_dev once, retaining the mapping for the
 * lifetime of the registration.  The whole buffer is reserved as one contiguous
 * IOVA and each folio (physically contiguous) is linked into it;
 * DMA_BIDIRECTIONAL because a fixed buffer serves both reads and writes.  On
 * success @reg->dma_dev is set and the caller must pair this with
 * blk_iobuf_reg_dma_unmap().
 */
static int blk_iobuf_reg_dma_map(struct blk_iobuf_reg *reg,
				 struct device *dma_dev)
{
	unsigned int folio_size = blk_iobuf_pool_folio_size(reg->pool);
	size_t total = (size_t)reg->nr_folios * folio_size;
	size_t mapped = 0;
	unsigned int i;
	int ret;

	/*
	 * A premapped buffer is mapped once and reused for every command with no
	 * per-command DMA cache maintenance, which is correct only where the
	 * device does cache-coherent DMA.  On a non-coherent device fall back to
	 * an ordinary dynamically mapped buffer: it pays the dma_opt clamp but
	 * stays correct, and premapping there would owe per-I/O cache maintenance
	 * that defeats its purpose anyway.
	 */
	if (!dev_is_dma_coherent(dma_dev))
		return -EOPNOTSUPP;

	/*
	 * Reserve one contiguous IOVA for the whole buffer and link each folio
	 * into it -- the two-step dma_iova_*() API the nvme request path uses.
	 * dma_iova_try_alloc() fails when the device is not behind a translating
	 * IOMMU; the caller then registers an ordinary dynamically mapped buffer
	 * (which pays no dma_opt clamp there anyway).
	 */
	if (!dma_iova_try_alloc(dma_dev, &reg->premap.state,
			(phys_addr_t)folio_pfn(reg->folios[0]) << PAGE_SHIFT,
			total))
		return -EOPNOTSUPP;

	for (i = 0; i < reg->nr_folios; i++) {
		ret = dma_iova_link(dma_dev, &reg->premap.state,
				(phys_addr_t)folio_pfn(reg->folios[i]) << PAGE_SHIFT,
				mapped, folio_size, DMA_BIDIRECTIONAL, 0);
		if (ret)
			goto destroy;
		mapped += folio_size;
	}
	ret = dma_iova_sync(dma_dev, &reg->premap.state, 0, mapped);
	if (ret)
		goto destroy;

	reg->premap.dma_dev = dma_dev;
	reg->premap.len = total;
	reg->dma_dev = dma_dev;
	return 0;
destroy:
	dma_iova_destroy(dma_dev, &reg->premap.state, mapped, DMA_BIDIRECTIONAL, 0);
	return ret;
}

static void blk_iobuf_reg_dma_unmap(struct blk_iobuf_reg *reg)
{
	if (!reg->dma_dev)
		return;
	dma_iova_destroy(reg->dma_dev, &reg->premap.state, reg->premap.len,
			 DMA_BIDIRECTIONAL, 0);
	reg->dma_dev = NULL;
}

/* Runs once, when io_uring drops the registered buffer. */
static void blk_iobuf_reg_release(void *data)
{
	struct blk_iobuf_reg *reg = data;

	blk_iobuf_reg_dma_unmap(reg);
	blk_iobuf_pool_free_batch(reg->pool, reg->folios, reg->nr_folios);
	blk_iobuf_pool_put(reg->pool);
	kfree(reg->folios);
	kfree(reg->bvecs);
	kfree(reg);
}

/**
 * blk_uring_cmd_alloc_iobuf - install a pool-backed fixed buffer
 * @cmd:         provider uring_cmd
 * @pool:        the queue's iobuf pool (a referenced pointer the caller owns)
 * @dma_dev:     device to persistently DMA-map the buffer to, or NULL for an
 *               ordinary dynamically mapped buffer (unchanged behaviour)
 * @buf_index:   sparse fixed-buffer slot to fill
 * @len:         logical buffer length
 * @issue_flags: uring_cmd issue flags
 *
 * Checks out ceil(len / folio_size) folios strictly from @pool, zeroes them,
 * builds one bvec per folio (the last truncated to the remainder), and
 * registers them as a kernel-owned fixed buffer. On success the provider
 * object owns a reference to @pool and is handed to the io_uring release
 * callback; on failure every folio is returned and no reference is leaked.
 *
 * When @dma_dev is non-NULL the folios are additionally DMA-mapped to that
 * device once, and the mapping is retained until the buffer is released, so a
 * device request path can reuse the persistent DMA addresses instead of
 * mapping per I/O.
 */
int blk_uring_cmd_alloc_iobuf(struct io_uring_cmd *cmd,
			      struct blk_iobuf_pool *pool,
			      struct device *dma_dev,
			      u64 buf_index, u64 len, unsigned int issue_flags)
{
	unsigned int folio_size = blk_iobuf_pool_folio_size(pool);
	struct io_uring_cmd_buf_desc desc;
	struct blk_iobuf_reg *reg;
	unsigned int nr_folios, i;
	size_t remaining;
	int ret;

	if (!len || len > MAX_RW_COUNT || buf_index > UINT_MAX)
		return -EINVAL;
	nr_folios = DIV_ROUND_UP(len, folio_size);

	reg = kzalloc(sizeof(*reg), GFP_KERNEL);
	if (!reg)
		return -ENOMEM;
	reg->folios = kmalloc_array(nr_folios, sizeof(*reg->folios), GFP_KERNEL);
	reg->bvecs = kmalloc_array(nr_folios, sizeof(*reg->bvecs), GFP_KERNEL);
	if (!reg->folios || !reg->bvecs) {
		ret = -ENOMEM;
		goto err;
	}
	reg->magic = BLK_IOBUF_REG_MAGIC;
	reg->pool = blk_iobuf_pool_get(pool);
	reg->nr_folios = nr_folios;

	ret = blk_iobuf_pool_alloc_batch(pool, reg->folios, nr_folios);
	if (ret)
		goto err;

	remaining = len;
	for (i = 0; i < nr_folios; i++) {
		unsigned int chunk = min_t(size_t, remaining, folio_size);

		folio_zero_range(reg->folios[i], 0, folio_size);
		bvec_set_folio(&reg->bvecs[i], reg->folios[i], chunk, 0);
		remaining -= chunk;
	}

	if (dma_dev) {
		ret = blk_iobuf_reg_dma_map(reg, dma_dev);
		/* No translating IOMMU: no IOVA to premap, no clamp to beat. */
		if (ret && ret != -EOPNOTSUPP)
			goto err_free_batch;
	}

	desc = (struct io_uring_cmd_buf_desc){
		.bvecs		= reg->bvecs,
		.nr_bvecs	= nr_folios,
		.len		= len,
		.dir		= IO_URING_CMD_BUF_READ | IO_URING_CMD_BUF_WRITE,
		.release	= blk_iobuf_reg_release,
		.release_data	= reg,
	};
	ret = io_uring_cmd_register_bvecs(cmd, buf_index, &desc, issue_flags);
	if (ret) {
		blk_iobuf_reg_dma_unmap(reg);
		blk_iobuf_pool_free_batch(pool, reg->folios, nr_folios);
		goto err;
	}
	return 0;	/* io_uring owns reg via the release callback */

err_free_batch:
	blk_iobuf_pool_free_batch(pool, reg->folios, nr_folios);
err:
	blk_iobuf_pool_put(reg->pool);	/* NULL-safe if never taken */
	kfree(reg->folios);
	kfree(reg->bvecs);
	kfree(reg);
	return ret;
}
EXPORT_SYMBOL_GPL(blk_uring_cmd_alloc_iobuf);
