/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Common infrastructure for supporing dma-buf in the I/O path.
 *
 * Copyright (C) 2026 Pavel Begunkov <asml.silence@gmail.com>
 */
#include <linux/dma-buf-io.h>
#include <linux/dma-resv.h>

static void dma_buf_io_put_ctx(struct dma_buf_io_ctx *ctx)
{
	might_sleep();

	if (!refcount_dec_and_test(&ctx->refs))
		return;
	if (WARN_ON_ONCE(rcu_dereference_protected(ctx->map, true)))
		return;

	ctx->dev_ops->release(ctx);
	dma_buf_put(ctx->dmabuf);
	kfree(ctx);
}

static void dma_buf_io_map_release_work(struct work_struct *work)
{
	struct dma_buf_io_map *map = container_of(work, struct dma_buf_io_map,
						  release_work);
	struct dma_buf_io_ctx *ctx = map->ctx;
	struct dma_buf *dmabuf = ctx->dmabuf;

	dma_resv_lock(dmabuf->resv, NULL);
	ctx->dev_ops->unmap(ctx, map);
	dma_resv_unlock(dmabuf->resv);

	percpu_ref_exit(&map->refs);
	kfree(map);

	dma_buf_io_put_ctx(ctx);
}

static void dma_buf_io_map_refs_release(struct percpu_ref *ref)
{
	struct dma_buf_io_map *map = container_of(ref, struct dma_buf_io_map, refs);
	struct dma_buf_io_ctx *ctx = map->ctx;

	/* There are no more requests using the map. */
	atomic_dec(&ctx->active_maps);
	wake_up(&ctx->active_maps_wq);

	/* might sleep, use a worker */
	INIT_WORK(&map->release_work, dma_buf_io_map_release_work);
	queue_work(system_wq, &map->release_work);
}

static void dma_buf_io_wait_maps(struct dma_buf_io_ctx *ctx)
{
	wait_event(ctx->active_maps_wq, atomic_read(&ctx->active_maps) == 0);
}

int dma_buf_io_init_map(struct dma_buf_io_ctx *ctx, struct dma_buf_io_map *map)
{
	int ret;

	ret = percpu_ref_init(&map->refs, dma_buf_io_map_refs_release, 0,
			      GFP_KERNEL);
	if (ret)
		return ret;
	map->ctx = ctx;
	return 0;
}
EXPORT_SYMBOL_NS_GPL(dma_buf_io_init_map, "DMA_BUF");

struct dma_buf_io_map *dma_buf_io_create_map(struct dma_buf_io_ctx *ctx)
{
	struct dma_buf *dmabuf = ctx->dmabuf;
	struct dma_buf_io_map *map;
	long ret;

	guard(mutex)(&ctx->map_create_mutex);

	/* recheck under the lock in case it has already been re-created */
	map = dma_buf_io_get_map(ctx);
	if (map)
		return map;
	dma_buf_io_wait_maps(ctx);

	ret = dma_resv_lock_interruptible(dmabuf->resv, NULL);
	if (ret)
		return ERR_PTR(ret);

	ret = dma_resv_wait_timeout(dmabuf->resv, DMA_RESV_USAGE_KERNEL,
				    true, MAX_SCHEDULE_TIMEOUT);
	if (!ret)
		ret = -EAGAIN;
	if (ret < 0) {
		dma_resv_unlock(dmabuf->resv);
		return ERR_PTR(ret);
	}

	map = ctx->dev_ops->map(ctx);
	dma_resv_unlock(dmabuf->resv);

	if (IS_ERR(map))
		return map;
	if (WARN_ON_ONCE(!map->seg_shift))
		return ERR_PTR(-EFAULT);

	atomic_inc(&ctx->active_maps);
	refcount_inc(&ctx->refs);
	/* get a reference for the caller */
	percpu_ref_get(&map->refs);

	scoped_guard(mutex, &ctx->map_mutex)
		rcu_assign_pointer(ctx->map, map);
	return map;
}

void dma_buf_io_invalidate_mappings(struct dma_buf_io_ctx *ctx)
{
	struct dma_buf_io_map *map;

	scoped_guard(mutex, &ctx->map_mutex) {
		map = rcu_dereference_protected(ctx->map,
					lockdep_is_held(&ctx->map_mutex));
		if (!map)
			return;
		rcu_assign_pointer(ctx->map, NULL);
		percpu_ref_kill(&map->refs);
	}
	dma_buf_io_wait_maps(ctx);
}
EXPORT_SYMBOL_NS_GPL(dma_buf_io_invalidate_mappings, "DMA_BUF");

void dma_buf_io_ctx_release(struct dma_buf_io_ctx *ctx)
{
	/* Remove and wait for the last map, there should be no new ones. */
	dma_buf_io_invalidate_mappings(ctx);
	dma_buf_io_put_ctx(ctx);
}

int dma_buf_io_ctx_create(struct file *file,
			   struct dma_buf *dmabuf,
			   enum dma_data_direction dir,
			   struct dma_buf_io_ctx **out_ctx)
{
	struct dma_buf_io_ctx *ctx;
	int ret;

	if (!file->f_op->init_dma_buf_io_ctx)
		return -EOPNOTSUPP;

	ctx = kmalloc_obj(*ctx);
	if (!ctx)
		return -ENOMEM;

	memset(ctx, 0, sizeof(*ctx));
	ctx->dir = dir;
	ctx->dmabuf = dmabuf;
	get_dma_buf(dmabuf);
	refcount_set(&ctx->refs, 1);
	mutex_init(&ctx->map_mutex);
	mutex_init(&ctx->map_create_mutex);
	atomic_set(&ctx->active_maps, 0);
	init_waitqueue_head(&ctx->active_maps_wq);

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
