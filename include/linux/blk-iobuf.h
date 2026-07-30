/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Block-layer bounded iobuf folio pool.
 *
 * A blk_iobuf_pool is a refcounted, pre-provisioned inventory of exactly
 * @capacity higher-order folios, all of the same @order. It hands out that
 * fixed inventory and nothing more: a checkout that cannot be satisfied from
 * the preallocated folios fails deterministically rather than falling back to
 * the page allocator. It exists so that suitable higher-order folios are
 * predictably available at buffer-registration time instead of being allocated
 * opportunistically under memory pressure.
 *
 * A folio is always returned to the exact pool object that produced it, and a
 * holder of checked-out folios keeps a reference to that pool until every one
 * of them has been returned, so the pool cannot be freed out from under its
 * outstanding inventory.
 */
#ifndef _LINUX_BLK_IOBUF_H
#define _LINUX_BLK_IOBUF_H

#include <linux/types.h>

struct folio;
struct blk_iobuf_pool;
struct request_queue;
struct io_uring_cmd;
struct device;

#ifdef CONFIG_BLK_IOBUF_POOL

struct blk_iobuf_pool *blk_iobuf_pool_create(unsigned int order,
					     unsigned int nr_folios);
struct blk_iobuf_pool *blk_iobuf_pool_get(struct blk_iobuf_pool *pool);
void blk_iobuf_pool_put(struct blk_iobuf_pool *pool);

/*
 * Strict, all-or-nothing checkout of @nr_folios folios from the pre-provisioned
 * inventory. Returns 0 with @folios filled, or -ENOBUFS with every
 * entry of @folios set to NULL and the pool unchanged. Never touches the page allocator.
 */
int blk_iobuf_pool_alloc_batch(struct blk_iobuf_pool *pool,
			       struct folio **folios, unsigned int nr_folios);
void blk_iobuf_pool_free_batch(struct blk_iobuf_pool *pool,
			       struct folio **folios, unsigned int nr_folios);

int blk_queue_set_iobuf_pool(struct request_queue *q,
			     struct blk_iobuf_pool *pool);
struct blk_iobuf_pool *blk_queue_get_iobuf_pool(struct request_queue *q);
void blk_queue_clear_iobuf_pool(struct request_queue *q);
int blk_uring_cmd_alloc_iobuf(struct io_uring_cmd *cmd,
			      struct blk_iobuf_pool *pool,
			      struct device *dma_dev,
			      u64 buf_index, u64 len, unsigned int issue_flags);
struct blk_dma_premap;
struct blk_dma_premap *blk_iobuf_fixed_buf_premap(void *kbuf_priv,
						  struct device *dma_dev);

unsigned int blk_iobuf_pool_order(const struct blk_iobuf_pool *pool);
unsigned int blk_iobuf_pool_folio_size(const struct blk_iobuf_pool *pool);
unsigned int blk_iobuf_pool_capacity(const struct blk_iobuf_pool *pool);
unsigned int blk_iobuf_pool_in_use(const struct blk_iobuf_pool *pool);
unsigned int blk_iobuf_pool_available(const struct blk_iobuf_pool *pool);
unsigned int blk_iobuf_pool_high_water(const struct blk_iobuf_pool *pool);
u64 blk_iobuf_pool_alloc_failures(const struct blk_iobuf_pool *pool);

#else /* !CONFIG_BLK_IOBUF_POOL */

static inline struct blk_iobuf_pool *
blk_iobuf_pool_create(unsigned int order, unsigned int nr_folios)
{
	return NULL;
}

static inline struct blk_iobuf_pool *
blk_iobuf_pool_get(struct blk_iobuf_pool *pool)
{
	return pool;
}

static inline void blk_iobuf_pool_put(struct blk_iobuf_pool *pool) {}

static inline int
blk_iobuf_pool_alloc_batch(struct blk_iobuf_pool *pool,
			   struct folio **folios, unsigned int nr_folios)
{
	return -ENOBUFS;
}

static inline void
blk_iobuf_pool_free_batch(struct blk_iobuf_pool *pool,
			  struct folio **folios, unsigned int nr_folios) {}

static inline int blk_queue_set_iobuf_pool(struct request_queue *q,
					   struct blk_iobuf_pool *pool)
{
	return 0;
}

static inline struct blk_iobuf_pool *
blk_queue_get_iobuf_pool(struct request_queue *q)
{
	return NULL;
}

static inline void blk_queue_clear_iobuf_pool(struct request_queue *q) {}

static inline unsigned int blk_iobuf_pool_order(const struct blk_iobuf_pool *p)
{
	return 0;
}

static inline unsigned int
blk_iobuf_pool_folio_size(const struct blk_iobuf_pool *p)
{
	return 0;
}

static inline unsigned int
blk_iobuf_pool_capacity(const struct blk_iobuf_pool *p)
{
	return 0;
}

static inline unsigned int blk_iobuf_pool_in_use(const struct blk_iobuf_pool *p)
{
	return 0;
}

static inline unsigned int
blk_iobuf_pool_available(const struct blk_iobuf_pool *p)
{
	return 0;
}

static inline unsigned int
blk_iobuf_pool_high_water(const struct blk_iobuf_pool *p)
{
	return 0;
}

static inline u64 blk_iobuf_pool_alloc_failures(const struct blk_iobuf_pool *p)
{
	return 0;
}

#endif /* CONFIG_BLK_IOBUF_POOL */

#endif /* _LINUX_BLK_IOBUF_H */
