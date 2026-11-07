#include "dma-buf.h"

static void io_release_dmabuf_work(struct work_struct *work)
{
	struct io_buf_dma *bd = container_of(work, struct io_buf_dma,
					     release_work);

	dma_buf_io_ctx_release(bd->ctx);
	fput(bd->target_file);
	kfree(bd);
}

/*
 * Releasing the context waits for its map's percpu reference to drain,
 * which takes a full RCU grace period even when no I/O is in flight, as
 * the reference only reaches zero once it has switched to atomic mode.
 * Do it from a workqueue so that unregistering a buffer, or freeing the
 * last ring that holds it, does not stall the caller for that long. The
 * context holds its own dma-buf reference, and the target file is put
 * only after the context is gone, so both stay valid until the work runs.
 */
static void io_release_dmabuf(void *priv)
{
	struct io_buf_dma *bd = priv;

	INIT_WORK(&bd->release_work, io_release_dmabuf_work);
	queue_work(system_dfl_wq, &bd->release_work);
}

int __io_register_dmabuf(struct io_ring_ctx *ctx,
			struct io_uring_regbuf_desc *desc,
			struct io_mapped_ubuf *imu)
{
	struct io_buf_dma *bd = NULL;
	struct file *target_file = NULL;
	struct dma_buf *dmabuf = NULL;
	int ret;

	/*
	 * We have to drop the dma-buf map early when IO is finished. Disable
	 * IOPOLL for now as we can't wait potentially infintely for a task
	 * to do that.
	 */
	if (ctx->flags & IORING_SETUP_IOPOLL)
		return -EOPNOTSUPP;
	if (desc->uaddr || desc->size)
		return -EINVAL;

	bd = kzalloc(sizeof(*bd), GFP_KERNEL);
	if (!bd)
		return -ENOMEM;

	target_file = fget(desc->target_fd);
	if (!target_file) {
		ret = -EBADF;
		goto err;
	}
	dmabuf = dma_buf_get(desc->dmabuf_fd);
	if (IS_ERR(dmabuf)) {
		ret = PTR_ERR(dmabuf);
		dmabuf = NULL;
		goto err;
	}
	ret = io_validate_user_buf_range(0, dmabuf->size);
	if (ret)
		goto err;

	ret = dma_buf_io_ctx_create(target_file, dmabuf, DMA_BIDIRECTIONAL,
				    &bd->ctx);
	if (ret)
		goto err;

	bd->target_file = target_file;
	imu->len = dmabuf->size;
	imu->release = io_release_dmabuf;
	imu->priv = bd;
	imu->flags = IO_REGBUF_F_DMABUF;
	imu->dir = IO_BUF_DEST | IO_BUF_SOURCE;
	dma_buf_put(dmabuf);
	return 0;
err:
	kfree(bd);
	if (target_file)
		fput(target_file);
	if (dmabuf)
		dma_buf_put(dmabuf);
	return ret;
}

