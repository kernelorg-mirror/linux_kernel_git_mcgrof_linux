/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Common infrastructure for supporing dma-buf in the I/O path.
 *
 * Copyright (C) 2026 Pavel Begunkov <asml.silence@gmail.com>
 */
#include <linux/dma-buf-io.h>
#include <linux/dma-resv.h>
#include <linux/wait_bit.h>

static void dma_buf_io_map_release(struct dma_buf_io_map *map)
{
	struct dma_buf_io_ctx *ctx = map->ctx;

	dma_resv_assert_held(ctx->dmabuf->resv);

	percpu_ref_exit(&map->refs);
	ctx->dev_ops->unmap(ctx, map);
}

static void dma_buf_io_map_refs_cb(struct percpu_ref *ref)
{
	struct dma_buf_io_map *map = container_of(ref, struct dma_buf_io_map, refs);

	/* Context shutdown and an invalidation may both be waiting. */
	complete_all(&map->drained);
}

static void dma_buf_io_kill_map(struct dma_buf_io_ctx *ctx)
{
	struct dma_buf_io_map *map;

	dma_resv_assert_held(ctx->dmabuf->resv);

	/*
	 * Context shutdown may have unpublished a map and dropped the lock
	 * to wait for it. Make sure no I/O still uses it before the exporter
	 * moves the buffer. The shutdown path releases it.
	 */
	if (ctx->dying_map)
		wait_for_completion(&ctx->dying_map->drained);

	map = rcu_dereference_protected(ctx->map,
				dma_resv_held(ctx->dmabuf->resv));
	if (!map)
		return;

	rcu_assign_pointer(ctx->map, NULL);
	percpu_ref_kill(&map->refs);
	/* make sure the map is not visible via ctx->map */
	synchronize_rcu_expedited();
	wait_for_completion(&map->drained);
	dma_buf_io_map_release(map);
}

/**
 * dma_buf_io_init_map - Initialise the map.
 * @ctx:	[in]	dma-buf I/O context the map is created for
 * @map:	[in]	dma-buf I/O map
 * @sgt:	[in]	the backing scatter-gather table
 *
 * Importer's helper that initialises @map for the provided I/O context
 * and scatter-gather table. The importer should call it once for each map
 * on creation.
 *
 * Return: 0 on success, or a negative errno code otherwise.
 */
int dma_buf_io_init_map(struct dma_buf_io_ctx *ctx, struct dma_buf_io_map *map,
			struct sg_table *sgt)
{
	unsigned seg_shift = ~0U;
	struct scatterlist *sg;
	unsigned long tmp;
	int ret;

	for_each_sgtable_dma_sg(sgt, sg, tmp)
		seg_shift = min(seg_shift, __ffs(sg_dma_len(sg)));

	ret = percpu_ref_init(&map->refs, dma_buf_io_map_refs_cb, 0, GFP_KERNEL);
	if (ret)
		return ret;
	init_completion(&map->drained);
	map->min_seg_shift = seg_shift;
	map->ctx = ctx;
	return 0;
}
EXPORT_SYMBOL_NS_GPL(dma_buf_io_init_map, "DMA_BUF");

static struct dma_buf_io_map *__dma_buf_io_create_map(struct dma_buf_io_ctx *ctx)
{
	struct dma_buf *dmabuf = ctx->dmabuf;
	struct dma_buf_io_map *map;
	long ret;

	dma_resv_assert_held(dmabuf->resv);

	if (ctx->is_dead)
		return ERR_PTR(-ENOENT);

	map = __dma_buf_io_get_map(ctx);
	if (map)
		return map;

	map = ctx->dev_ops->map(ctx);
	if (IS_ERR(map))
		return map;

	if (WARN_ON_ONCE(!map->min_seg_shift))
		return ERR_PTR(-EFAULT);

	ret = dma_resv_wait_timeout(dmabuf->resv, DMA_RESV_USAGE_KERNEL,
				    true, MAX_SCHEDULE_TIMEOUT);
	if (ret <= 0) {
		if (!ret)
			ret = -EAGAIN;
		dma_buf_io_map_release(map);
		return ERR_PTR(ret);
	}

	/* get a reference for the caller */
	percpu_ref_get(&map->refs);
	rcu_assign_pointer(ctx->map, map);
	return map;
}

/**
 * dma_buf_io_create_map - Create a new or reference an existing map.
 * @ctx:	[in]	dma-buf I/O context
 *
 * A slow path part of dma_buf_io_get_map() and shouldn't be used directly.
 * Behaves in the same way but is allowed to block.
 *
 * Returns a map on success, or ERR_PTR encoded errno otherwise.
 */
struct dma_buf_io_map *dma_buf_io_create_map(struct dma_buf_io_ctx *ctx)
{
	struct dma_buf_io_map *map;
	int ret;

	ret = dma_resv_lock_interruptible(ctx->dmabuf->resv, NULL);
	if (ret)
		return ERR_PTR(ret);

	map = __dma_buf_io_create_map(ctx);
	dma_resv_unlock(ctx->dmabuf->resv);
	return map;
}

static void dma_buf_io_ctx_shutdown(struct dma_buf_io_ctx *ctx)
{
	struct dma_resv *resv = ctx->dmabuf->resv;
	struct dma_buf_io_map *map;

	dma_resv_lock(resv, NULL);
	ctx->is_dead = true;
	map = rcu_dereference_protected(ctx->map, dma_resv_held(resv));
	if (map) {
		rcu_assign_pointer(ctx->map, NULL);
		percpu_ref_kill(&map->refs);
		ctx->dying_map = map;
	}
	dma_resv_unlock(resv);
	if (!map) {
		/*
		 * Another shutdown, such as a release racing with device
		 * removal, may still be draining the map. Do not return
		 * before it has been unmapped.
		 */
		wait_var_event(&ctx->dying_map, !READ_ONCE(ctx->dying_map));
		return;
	}

	/*
	 * The reference drains only after an RCU grace period. Wait for it
	 * without the reservation lock, so the exporter is not blocked from
	 * unmapping or freeing the buffer meanwhile. The context is dead, so
	 * no new map can be created, and an invalidation that comes in
	 * between waits for this map in dma_buf_io_kill_map().
	 */
	synchronize_rcu_expedited();
	wait_for_completion(&map->drained);

	dma_resv_lock(resv, NULL);
	dma_buf_io_map_release(map);
	WRITE_ONCE(ctx->dying_map, NULL);
	dma_resv_unlock(resv);
	wake_up_var(&ctx->dying_map);
}

/*
 * dma_buf_io_ctx_create - Create a new dma-buf I/O context for the given file.
 * @file: 	[in]	file for which the context is created
 * @dmabuf:	[in]	buffer to create a context from
 * @dir:	[in]	direction of DMA transfers
 * @out_ctx:	[out]	resulting ctx
 *
 * Create a new I/O context from the given DMA buffer for the given file.
 * It only works with files implementing ->init_dma_buf_io_ctx and fails
 * with -EOPNOTSUPP otherwise. Once created, the user can create a map
 * and use it to issue I/O, such as read/write requests, to @file.
 */
int dma_buf_io_ctx_create(struct file *file,
			   struct dma_buf *dmabuf,
			   enum dma_data_direction dir,
			   struct dma_buf_io_ctx **out_ctx)
{
	struct dma_buf_io_ctx *ctx;
	int ret;

	if (!file->f_op->init_dma_buf_io_ctx)
		return -EOPNOTSUPP;

	ctx = kzalloc_obj(*ctx);
	if (!ctx)
		return -ENOMEM;
	ctx->dir = dir;
	ctx->dmabuf = dmabuf;
	get_dma_buf(dmabuf);

	ret = file->f_op->init_dma_buf_io_ctx(file, ctx);
	if (ret) {
		kfree(ctx);
		dma_buf_put(dmabuf);
		return ret;
	}

	if (WARN_ON_ONCE(!ctx->dev_ops ||
			 !ctx->dev_ops->map ||
			 !ctx->dev_ops->unmap ||
			 !ctx->dev_ops->release))
		return -EINVAL;

	*out_ctx = ctx;
	return 0;
}

/**
 * dma_buf_io_ctx_release - Destroy the I/O context
 * @ctx:	[in]	dma-buf I/O context
 *
 * Destroy the context. It makes sure all maps are waited on and destroyed,
 * and can block for I/O to complete. It detaches from the importer and
 * destroys all its resources via the ->release callback, and frees the
 * context.
 */
void dma_buf_io_ctx_release(struct dma_buf_io_ctx *ctx)
{
	dma_buf_io_ctx_shutdown(ctx);

	if (WARN_ON_ONCE(rcu_dereference_protected(ctx->map, true)))
		return;

	ctx->dev_ops->release(ctx);
	dma_buf_put(ctx->dmabuf);
	kfree(ctx);
}

/**
 * dma_buf_io_invalidate_mappings - Invalidate the current map
 * @ctx: 	[in]	dma-buf I/O context
 *
 * Importer's helper for invalidating the current map. Waits for all I/O
 * using the map to complete and destroys the map. Called with the
 * dma-buf reservation lock held.
 */
void dma_buf_io_invalidate_mappings(struct dma_buf_io_ctx *ctx)
{
	dma_buf_io_kill_map(ctx);
}
EXPORT_SYMBOL_NS_GPL(dma_buf_io_invalidate_mappings, "DMA_BUF");

/**
 * dma_buf_io_detach - Detach the @ctx from the importer.
 * @ctx:	[in]	dma-buf I/O context
 *
 * Importer's helper for removing the I/O context. It kills all maps and
 * prevents creation of new ones. Upon return, it's safe for the importer
 * to release all its resources such as dma-buf attachments.
 */
void dma_buf_io_detach(struct dma_buf_io_ctx *ctx)
{
	dma_buf_io_ctx_shutdown(ctx);
}
EXPORT_SYMBOL_NS_GPL(dma_buf_io_detach, "DMA_BUF");
