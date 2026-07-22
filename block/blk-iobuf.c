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
#include <linux/gfp.h>
#include <linux/kref.h>
#include <linux/mempool.h>
#include <linux/mm.h>
#include <linux/overflow.h>
#include <linux/rcupdate.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

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
 * @folios filled, or -ENOBUFS with @folios untouched and every partially-drawn
 * folio returned. Never allocates from the page allocator.
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
			while (i--)
				mempool_free(folios[i], &pool->folio_pool);
			spin_unlock(&pool->lock);
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
