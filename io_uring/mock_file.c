// SPDX-License-Identifier: GPL-2.0
#include <linux/device.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/anon_inodes.h>
#include <linux/ktime.h>
#include <linux/hrtimer.h>
#include <linux/poll.h>
#include <linux/bvec.h>
#include <linux/fs.h>
#include <linux/highmem.h>
#include <linux/mm.h>
#include <linux/refcount.h>
#include <linux/slab.h>

#include <linux/io_uring/cmd.h>
#include <linux/io_uring_types.h>
#include <uapi/linux/io_uring/mock_file.h>

struct io_mock_iocb {
	struct kiocb		*iocb;
	struct hrtimer		timer;
	int			res;
};

/*
 * A mock provider buffer. It is jointly owned by the mock file (which reads
 * back the accounting) and by io_uring (which holds it until the registered
 * slot is detached and the last in-flight user drops). io_uring may run the
 * release callback after the mock file is closed, so this record must outlive
 * both -- hence the refcount, dropped by whichever owner is last.
 */
struct io_mock_regbuf {
	refcount_t		refs;
	atomic_t		released;
	u64			len;
	unsigned int		nr_pages;
	struct page		**pages;
};

struct io_mock_file {
	size_t			size;
	u64			rw_delay_ns;
	bool			pollable;
	struct wait_queue_head	poll_wq;
	struct io_mock_regbuf	*regbuf;
};

#define IO_VALID_COPY_CMD_FLAGS		IORING_MOCK_COPY_FROM
#define IO_VALID_PROV_FLAGS		(IORING_MOCK_PROV_F_FILL_PATTERN | \
					 IORING_MOCK_PROV_F_READ | \
					 IORING_MOCK_PROV_F_WRITE)

static int io_copy_regbuf(struct iov_iter *reg_iter, void __user *ubuf)
{
	size_t ret, copied = 0;
	size_t buflen = PAGE_SIZE;
	void *tmp_buf;

	tmp_buf = kzalloc(buflen, GFP_KERNEL);
	if (!tmp_buf)
		return -ENOMEM;

	while (iov_iter_count(reg_iter)) {
		size_t len = min(iov_iter_count(reg_iter), buflen);

		if (iov_iter_rw(reg_iter) == ITER_SOURCE) {
			ret = copy_from_iter(tmp_buf, len, reg_iter);
			if (ret <= 0)
				break;
			if (copy_to_user(ubuf, tmp_buf, ret))
				break;
		} else {
			if (copy_from_user(tmp_buf, ubuf, len))
				break;
			ret = copy_to_iter(tmp_buf, len, reg_iter);
			if (ret <= 0)
				break;
		}
		ubuf += ret;
		copied += ret;
	}

	kfree(tmp_buf);
	return copied;
}

static int io_cmd_copy_regbuf(struct io_uring_cmd *cmd, unsigned int issue_flags)
{
	const struct io_uring_sqe *sqe = cmd->sqe;
	const struct iovec __user *iovec;
	unsigned flags, iovec_len;
	struct iov_iter iter;
	void __user *ubuf;
	int dir, ret;

	ubuf = u64_to_user_ptr(READ_ONCE(sqe->addr3));
	iovec = u64_to_user_ptr(READ_ONCE(sqe->addr));
	iovec_len = READ_ONCE(sqe->len);
	flags = READ_ONCE(sqe->file_index);

	if (unlikely(sqe->ioprio || sqe->__pad1))
		return -EINVAL;
	if (flags & ~IO_VALID_COPY_CMD_FLAGS)
		return -EINVAL;

	dir = (flags & IORING_MOCK_COPY_FROM) ? ITER_SOURCE : ITER_DEST;
	ret = io_uring_cmd_import_fixed_vec(cmd, iovec, iovec_len, dir, &iter,
					    issue_flags);
	if (ret)
		return ret;
	ret = io_copy_regbuf(&iter, ubuf);
	return ret ? ret : -EFAULT;
}

static void io_mock_regbuf_put(struct io_mock_regbuf *rb)
{
	if (rb && refcount_dec_and_test(&rb->refs)) {
		kfree(rb->pages);
		kfree(rb);
	}
}

/*
 * io_uring's release callback: runs exactly once, after the registered slot
 * is detached and the last in-flight fixed-buffer user drops. Free the backing
 * pages, record that release ran, and drop io_uring's reference.
 */
static void io_mock_regbuf_release(void *data)
{
	struct io_mock_regbuf *rb = data;
	unsigned int i;

	for (i = 0; i < rb->nr_pages; i++)
		__free_page(rb->pages[i]);
	rb->nr_pages = 0;
	atomic_set(&rb->released, 1);
	io_mock_regbuf_put(rb);
}

/*
 * Act as a buffer provider: allocate pages, describe them as a bvec array, and
 * install them into an existing sparse registered-buffer slot via the generic
 * provider helper. This exercises io_uring_cmd_register_bvecs() and its release
 * path with no hardware.
 */
static int io_cmd_prov_register(struct io_uring_cmd *cmd,
				unsigned int issue_flags)
{
	const struct io_uring_sqe *sqe = cmd->sqe;
	struct io_mock_file *mf = cmd->file->private_data;
	struct io_uring_mock_prov_register arg;
	struct io_uring_mock_prov_register __user *uarg;
	struct io_uring_cmd_buf_desc desc;
	struct io_mock_regbuf *rb;
	struct bio_vec *bvecs;
	unsigned int i, nr_pages, dir;
	u64 remaining;
	int ret;

	if (sqe->ioprio || sqe->__pad1 || sqe->addr3 || sqe->file_index)
		return -EINVAL;
	uarg = u64_to_user_ptr(READ_ONCE(sqe->addr));
	if (READ_ONCE(sqe->len) != sizeof(arg))
		return -EINVAL;
	if (copy_from_user(&arg, uarg, sizeof(arg)))
		return -EFAULT;
	if (arg.__pad || !mem_is_zero(arg.__resv, sizeof(arg.__resv)))
		return -EINVAL;
	if (arg.flags & ~IO_VALID_PROV_FLAGS)
		return -EINVAL;
	if (!arg.len || arg.len > MAX_RW_COUNT)
		return -EINVAL;

	dir = ((arg.flags & IORING_MOCK_PROV_F_READ) ?
			IO_URING_CMD_BUF_READ : 0) |
	      ((arg.flags & IORING_MOCK_PROV_F_WRITE) ?
			IO_URING_CMD_BUF_WRITE : 0);
	if (!dir)
		dir = IO_URING_CMD_BUF_READ | IO_URING_CMD_BUF_WRITE;

	nr_pages = DIV_ROUND_UP_ULL(arg.len, PAGE_SIZE);

	rb = kzalloc_obj(*rb, GFP_KERNEL);
	if (!rb)
		return -ENOMEM;
	rb->pages = kmalloc_array(nr_pages, sizeof(*rb->pages), GFP_KERNEL);
	bvecs = kmalloc_array(nr_pages, sizeof(*bvecs), GFP_KERNEL);
	if (!rb->pages || !bvecs) {
		ret = -ENOMEM;
		goto err_arrays;
	}

	for (i = 0; i < nr_pages; i++) {
		rb->pages[i] = alloc_page(GFP_KERNEL);
		if (!rb->pages[i]) {
			ret = -ENOMEM;
			goto err_pages;
		}
	}

	remaining = arg.len;
	for (i = 0; i < nr_pages; i++) {
		unsigned int chunk = min_t(u64, remaining, PAGE_SIZE);

		if (arg.flags & IORING_MOCK_PROV_F_FILL_PATTERN) {
			void *va = kmap_local_page(rb->pages[i]);

			memset(va, arg.pattern, PAGE_SIZE);
			kunmap_local(va);
		}
		bvec_set_page(&bvecs[i], rb->pages[i], chunk, 0);
		remaining -= chunk;
	}

	rb->nr_pages = nr_pages;
	rb->len = arg.len;
	atomic_set(&rb->released, 0);
	refcount_set(&rb->refs, 2);	/* one for io_uring, one for the file */

	desc = (struct io_uring_cmd_buf_desc){
		.bvecs		= bvecs,
		.nr_bvecs	= nr_pages,
		.len		= arg.len,
		.dir		= dir,
		.release	= io_mock_regbuf_release,
		.release_data	= rb,
	};
	ret = io_uring_cmd_register_bvecs(cmd, arg.buf_index, &desc, issue_flags);
	kfree(bvecs);
	bvecs = NULL;
	if (ret)
		goto err_pages_all;

	/* io_uring owns one reference now; the mock file keeps the other. */
	io_mock_regbuf_put(mf->regbuf);
	mf->regbuf = rb;
	return 0;

err_pages_all:
	i = nr_pages;
err_pages:
	while (i--)
		__free_page(rb->pages[i]);
err_arrays:
	kfree(bvecs);
	kfree(rb->pages);
	kfree(rb);
	return ret;
}

/* Read back whether the last provider buffer was registered and released. */
static int io_cmd_prov_stat(struct io_uring_cmd *cmd)
{
	const struct io_uring_sqe *sqe = cmd->sqe;
	struct io_mock_file *mf = cmd->file->private_data;
	struct io_uring_mock_prov_stat arg;
	struct io_uring_mock_prov_stat __user *uarg;

	if (sqe->ioprio || sqe->__pad1 || sqe->addr3 || sqe->file_index)
		return -EINVAL;
	uarg = u64_to_user_ptr(READ_ONCE(sqe->addr));
	if (READ_ONCE(sqe->len) != sizeof(arg))
		return -EINVAL;

	memset(&arg, 0, sizeof(arg));
	if (mf->regbuf) {
		arg.registered = 1;
		arg.released = atomic_read(&mf->regbuf->released);
		arg.len = mf->regbuf->len;
	}
	if (copy_to_user(uarg, &arg, sizeof(arg)))
		return -EFAULT;
	return 0;
}

static int io_mock_cmd(struct io_uring_cmd *cmd, unsigned int issue_flags)
{
	switch (cmd->cmd_op) {
	case IORING_MOCK_CMD_COPY_REGBUF:
		return io_cmd_copy_regbuf(cmd, issue_flags);
	case IORING_MOCK_CMD_PROV_REGISTER:
		return io_cmd_prov_register(cmd, issue_flags);
	case IORING_MOCK_CMD_PROV_STAT:
		return io_cmd_prov_stat(cmd);
	}
	return -ENOTSUPP;
}

static enum hrtimer_restart io_mock_rw_timer_expired(struct hrtimer *timer)
{
	struct io_mock_iocb *mio = container_of(timer, struct io_mock_iocb, timer);
	struct kiocb *iocb = mio->iocb;

	WRITE_ONCE(iocb->private, NULL);
	iocb->ki_complete(iocb, mio->res);
	kfree(mio);
	return HRTIMER_NORESTART;
}

static ssize_t io_mock_delay_rw(struct kiocb *iocb, size_t len)
{
	struct io_mock_file *mf = iocb->ki_filp->private_data;
	struct io_mock_iocb *mio;

	mio = kzalloc_obj(*mio);
	if (!mio)
		return -ENOMEM;

	mio->iocb = iocb;
	mio->res = len;
	hrtimer_setup(&mio->timer, io_mock_rw_timer_expired,
		      CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	hrtimer_start(&mio->timer, ns_to_ktime(mf->rw_delay_ns),
		      HRTIMER_MODE_REL);
	return -EIOCBQUEUED;
}

static ssize_t io_mock_read_iter(struct kiocb *iocb, struct iov_iter *to)
{
	struct io_mock_file *mf = iocb->ki_filp->private_data;
	size_t len = iov_iter_count(to);
	size_t nr_zeroed;

	if (iocb->ki_pos + len > mf->size)
		return -EINVAL;
	nr_zeroed = iov_iter_zero(len, to);
	if (!mf->rw_delay_ns || nr_zeroed != len)
		return nr_zeroed;

	return io_mock_delay_rw(iocb, len);
}

static ssize_t io_mock_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
	struct io_mock_file *mf = iocb->ki_filp->private_data;
	size_t len = iov_iter_count(from);

	if (iocb->ki_pos + len > mf->size)
		return -EINVAL;
	if (!mf->rw_delay_ns) {
		iov_iter_advance(from, len);
		return len;
	}

	return io_mock_delay_rw(iocb, len);
}

static loff_t io_mock_llseek(struct file *file, loff_t offset, int whence)
{
	struct io_mock_file *mf = file->private_data;

	return fixed_size_llseek(file, offset, whence, mf->size);
}

static __poll_t io_mock_poll(struct file *file, struct poll_table_struct *pt)
{
	struct io_mock_file *mf = file->private_data;
	__poll_t mask = 0;

	poll_wait(file, &mf->poll_wq, pt);

	mask |= EPOLLOUT | EPOLLWRNORM;
	mask |= EPOLLIN | EPOLLRDNORM;
	return mask;
}

static int io_mock_release(struct inode *inode, struct file *file)
{
	struct io_mock_file *mf = file->private_data;

	io_mock_regbuf_put(mf->regbuf);
	kfree(mf);
	return 0;
}

static const struct file_operations io_mock_fops = {
	.owner		= THIS_MODULE,
	.release	= io_mock_release,
	.uring_cmd	= io_mock_cmd,
	.read_iter	= io_mock_read_iter,
	.write_iter	= io_mock_write_iter,
	.llseek		= io_mock_llseek,
};

static const struct file_operations io_mock_poll_fops = {
	.owner		= THIS_MODULE,
	.release	= io_mock_release,
	.uring_cmd	= io_mock_cmd,
	.read_iter	= io_mock_read_iter,
	.write_iter	= io_mock_write_iter,
	.llseek		= io_mock_llseek,
	.poll		= io_mock_poll,
};

#define IO_VALID_CREATE_FLAGS (IORING_MOCK_CREATE_F_SUPPORT_NOWAIT | \
				IORING_MOCK_CREATE_F_POLL)

static int io_create_mock_file(struct io_uring_cmd *cmd, unsigned int issue_flags)
{
	const struct file_operations *fops = &io_mock_fops;
	const struct io_uring_sqe *sqe = cmd->sqe;
	struct io_uring_mock_create mc, __user *uarg;
	struct file *file;
	struct io_mock_file *mf __free(kfree) = NULL;
	size_t uarg_size;

	/*
	 * It's a testing only driver that allows exercising edge cases
	 * that wouldn't be possible to hit otherwise.
	 */
	add_taint(TAINT_TEST, LOCKDEP_STILL_OK);

	uarg = u64_to_user_ptr(READ_ONCE(sqe->addr));
	uarg_size = READ_ONCE(sqe->len);

	if (sqe->ioprio || sqe->__pad1 || sqe->addr3 || sqe->file_index)
		return -EINVAL;
	if (uarg_size != sizeof(mc))
		return -EINVAL;

	memset(&mc, 0, sizeof(mc));
	if (copy_from_user(&mc, uarg, uarg_size))
		return -EFAULT;
	if (!mem_is_zero(mc.__resv, sizeof(mc.__resv)))
		return -EINVAL;
	if (mc.flags & ~IO_VALID_CREATE_FLAGS)
		return -EINVAL;
	if (mc.file_size > SZ_1G)
		return -EINVAL;
	if (mc.rw_delay_ns > NSEC_PER_SEC)
		return -EINVAL;

	mf = kzalloc_obj(*mf, GFP_KERNEL_ACCOUNT);
	if (!mf)
		return -ENOMEM;

	init_waitqueue_head(&mf->poll_wq);
	mf->size = mc.file_size;
	mf->rw_delay_ns = mc.rw_delay_ns;
	if (mc.flags & IORING_MOCK_CREATE_F_POLL) {
		fops = &io_mock_poll_fops;
		mf->pollable = true;
	}

	FD_PREPARE(fdf, O_RDWR | O_CLOEXEC,
		   anon_inode_create_getfile("[io_uring_mock]", fops, mf,
					     O_RDWR | O_CLOEXEC, NULL));
	if (fdf.err)
		return fdf.err;

	retain_and_null_ptr(mf);
	file = fd_prepare_file(fdf);
	file->f_mode |= FMODE_READ | FMODE_CAN_READ | FMODE_WRITE |
			FMODE_CAN_WRITE | FMODE_LSEEK;
	if (mc.flags & IORING_MOCK_CREATE_F_SUPPORT_NOWAIT)
		file->f_mode |= FMODE_NOWAIT;

	mc.out_fd = fd_prepare_fd(fdf);
	if (copy_to_user(uarg, &mc, uarg_size))
		return -EFAULT;

	fd_publish(fdf);
	return 0;
}

static int io_probe_mock(struct io_uring_cmd *cmd)
{
	const struct io_uring_sqe *sqe = cmd->sqe;
	struct io_uring_mock_probe mp, __user *uarg;
	size_t uarg_size;

	uarg = u64_to_user_ptr(READ_ONCE(sqe->addr));
	uarg_size = READ_ONCE(sqe->len);

	if (sqe->ioprio || sqe->__pad1 || sqe->addr3 || sqe->file_index ||
	    uarg_size != sizeof(mp))
		return -EINVAL;

	memset(&mp, 0, sizeof(mp));
	if (copy_from_user(&mp, uarg, uarg_size))
		return -EFAULT;
	if (!mem_is_zero(&mp, sizeof(mp)))
		return -EINVAL;

	mp.features = IORING_MOCK_FEAT_END;

	if (copy_to_user(uarg, &mp, uarg_size))
		return -EFAULT;
	return 0;
}

static int iou_mock_mgr_cmd(struct io_uring_cmd *cmd, unsigned int issue_flags)
{
	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	switch (cmd->cmd_op) {
	case IORING_MOCK_MGR_CMD_PROBE:
		return io_probe_mock(cmd);
	case IORING_MOCK_MGR_CMD_CREATE:
		return io_create_mock_file(cmd, issue_flags);
	}
	return -EOPNOTSUPP;
}

static const struct file_operations iou_mock_dev_fops = {
	.owner		= THIS_MODULE,
	.uring_cmd	= iou_mock_mgr_cmd,
};

static struct miscdevice iou_mock_miscdev = {
	.minor			= MISC_DYNAMIC_MINOR,
	.name			= "io_uring_mock",
	.fops			= &iou_mock_dev_fops,
};

static int __init io_mock_init(void)
{
	int ret;

	ret = misc_register(&iou_mock_miscdev);
	if (ret < 0) {
		pr_err("Could not initialize io_uring mock device\n");
		return ret;
	}
	return 0;
}

static void __exit io_mock_exit(void)
{
	misc_deregister(&iou_mock_miscdev);
}

module_init(io_mock_init)
module_exit(io_mock_exit)

MODULE_AUTHOR("Pavel Begunkov <asml.silence@gmail.com>");
MODULE_DESCRIPTION("io_uring mock file");
MODULE_LICENSE("GPL");
