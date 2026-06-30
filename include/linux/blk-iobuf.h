/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Block-layer queue-associated iobuf pool for higher-order folios.
 *
 * Provides a per-queue pool of compound folios sized to match device geometry
 * (io_min, optimal write size, max_hw_bytes / max_segments), so that
 * passthrough and DIO paths can avoid building huge scatter/gather lists.
 */
#ifndef _LINUX_BLK_IOBUF_H
#define _LINUX_BLK_IOBUF_H

#include <linux/atomic.h>
#include <linux/mempool.h>
#include <linux/types.h>

struct request_queue;
struct queue_limits;
struct folio;

/* Reasons a higher-order pool was created */
#define BLK_IOBUF_REASON_IO_MIN		BIT(0)	/* io_min > PAGE_SIZE */
#define BLK_IOBUF_REASON_SEG_GEOM	BIT(1)	/* max_hw_bytes/segs > PAGE_SIZE */
#define BLK_IOBUF_REASON_IO_OPT	BIT(2)	/* io_opt >= PAGE_SIZE and preferred */

/**
 * struct blk_iobuf_pool_config - admin-visible tunables for pool creation
 * @max_order:        cap on chosen folio order (0 = use global default)
 * @min_nr:           minimum folios to pre-allocate in the pool
 * @max_bytes_total:  hard cap on total pool memory in bytes (0 = use default)
 * @prefer_io_opt:    use io_opt to increase folio order when beneficial
 */
struct blk_iobuf_pool_config {
	unsigned int max_order;
	unsigned int min_nr;
	unsigned long max_bytes_total;
	bool prefer_io_opt;
};

/**
 * struct blk_iobuf_pool - per-queue folio pool
 */
struct blk_iobuf_pool {
	struct request_queue	*q;
	mempool_t		folio_pool;
	unsigned int		order;
	unsigned int		folio_size;
	unsigned int		min_nr;
	unsigned int		max_nr;
	unsigned int		reasons;
	atomic_t		in_use;
	atomic64_t		allocs;
	atomic64_t		frees;
	atomic64_t		misses;
	atomic64_t		fallback;
};

/* Global defaults (overrideable via module params) */
extern unsigned int blk_iobuf_pool_max_order;
extern unsigned int blk_iobuf_pool_min_folios;
extern bool blk_iobuf_pool_prefer_io_opt;

#ifdef CONFIG_BLK_IOBUF_POOL

bool blk_queue_iobuf_pool_enabled(struct request_queue *q);
struct blk_iobuf_pool *blk_queue_get_iobuf_pool(struct request_queue *q);
unsigned int blk_iobuf_pool_folio_size(struct request_queue *q);

unsigned int blk_iobuf_choose_order(const struct queue_limits *lim,
				    unsigned int max_order,
				    unsigned int *reason_mask);

int blk_queue_init_iobuf_pool(struct request_queue *q,
			      const struct queue_limits *lim,
			      const struct blk_iobuf_pool_config *cfg);

void blk_queue_exit_iobuf_pool(struct request_queue *q);

struct folio *blk_iobuf_alloc_folio(struct request_queue *q, gfp_t gfp);
void blk_iobuf_free_folio(struct request_queue *q, struct folio *folio);
void blk_iobuf_inc_fallback(struct request_queue *q);

#else /* !CONFIG_BLK_IOBUF_POOL */

static inline bool blk_queue_iobuf_pool_enabled(struct request_queue *q)
{
	return false;
}

static inline struct blk_iobuf_pool *
blk_queue_get_iobuf_pool(struct request_queue *q)
{
	return NULL;
}

static inline unsigned int blk_iobuf_pool_folio_size(struct request_queue *q)
{
	return 0;
}

static inline unsigned int blk_iobuf_choose_order(const struct queue_limits *lim,
						   unsigned int max_order,
						   unsigned int *reason_mask)
{
	if (reason_mask)
		*reason_mask = 0;
	return 0;
}

static inline int blk_queue_init_iobuf_pool(struct request_queue *q,
					    const struct queue_limits *lim,
					    const struct blk_iobuf_pool_config *cfg)
{
	return 0;
}

static inline void blk_queue_exit_iobuf_pool(struct request_queue *q) {}

static inline struct folio *blk_iobuf_alloc_folio(struct request_queue *q,
						   gfp_t gfp)
{
	return NULL;
}

static inline void blk_iobuf_free_folio(struct request_queue *q,
					 struct folio *folio) {}

static inline void blk_iobuf_inc_fallback(struct request_queue *q) {}

#endif /* CONFIG_BLK_IOBUF_POOL */

/* Sysfs show helpers, called from blk-sysfs.c */
#ifdef CONFIG_BLK_IOBUF_POOL
struct gendisk;
ssize_t blk_iobuf_sysfs_enabled_show(struct gendisk *disk, char *page);
ssize_t blk_iobuf_sysfs_order_show(struct gendisk *disk, char *page);
ssize_t blk_iobuf_sysfs_folio_size_show(struct gendisk *disk, char *page);
ssize_t blk_iobuf_sysfs_reasons_show(struct gendisk *disk, char *page);
ssize_t blk_iobuf_sysfs_min_folios_show(struct gendisk *disk, char *page);
ssize_t blk_iobuf_sysfs_in_use_show(struct gendisk *disk, char *page);
ssize_t blk_iobuf_sysfs_allocs_show(struct gendisk *disk, char *page);
ssize_t blk_iobuf_sysfs_misses_show(struct gendisk *disk, char *page);
ssize_t blk_iobuf_sysfs_fallbacks_show(struct gendisk *disk, char *page);
#endif /* CONFIG_BLK_IOBUF_POOL */

#endif /* _LINUX_BLK_IOBUF_H */
