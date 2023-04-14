// SPDX-License-Identifier: GPL-2.0-only
#include <linux/fs.h>
#include <linux/fs_struct.h>
#include <linux/kernel_read_file.h>
#include <linux/security.h>
#include <linux/vmalloc.h>
#include <linux/fdtable.h>

/**
 * kernel_read_file() - read file contents into a kernel buffer
 *
 * @file	file to read from
 * @offset	where to start reading from (see below).
 * @buf		pointer to a "void *" buffer for reading into (if
 *		*@buf is NULL, a buffer will be allocated, and
 *		@buf_size will be ignored)
 * @buf_size	size of buf, if already allocated. If @buf not
 *		allocated, this is the largest size to allocate.
 * @file_size	if non-NULL, the full size of @file will be
 *		written here.
 * @id		the kernel_read_file_id identifying the type of
 *		file contents being read (for LSMs to examine)
 *
 * @offset must be 0 unless both @buf and @file_size are non-NULL
 * (i.e. the caller must be expecting to read partial file contents
 * via an already-allocated @buf, in at most @buf_size chunks, and
 * will be able to determine when the entire file was read by
 * checking @file_size). This isn't a recommended way to read a
 * file, though, since it is possible that the contents might
 * change between calls to kernel_read_file().
 *
 * Returns number of bytes read (no single read will be bigger
 * than SSIZE_MAX), or negative on error.
 *
 */
ssize_t kernel_read_file(struct file *file, loff_t offset, void **buf,
			 size_t buf_size, size_t *file_size,
			 enum kernel_read_file_id id)
{
	loff_t i_size, pos;
	ssize_t copied;
	void *allocated = NULL;
	bool whole_file;
	int ret;

	if (offset != 0 && (!*buf || !file_size))
		return -EINVAL;

	if (!S_ISREG(file_inode(file)->i_mode))
		return -EINVAL;

	ret = deny_write_access(file);
	if (ret)
		return ret;

	i_size = i_size_read(file_inode(file));
	if (i_size <= 0) {
		ret = -EINVAL;
		goto out;
	}
	/* The file is too big for sane activities. */
	if (i_size > SSIZE_MAX) {
		ret = -EFBIG;
		goto out;
	}
	/* The entire file cannot be read in one buffer. */
	if (!file_size && offset == 0 && i_size > buf_size) {
		ret = -EFBIG;
		goto out;
	}

	whole_file = (offset == 0 && i_size <= buf_size);
	ret = security_kernel_read_file(file, id, whole_file);
	if (ret)
		goto out;

	if (file_size)
		*file_size = i_size;

	if (!*buf)
		*buf = allocated = vmalloc(i_size);
	if (!*buf) {
		ret = -ENOMEM;
		goto out;
	}

	pos = offset;
	copied = 0;
	while (copied < buf_size) {
		ssize_t bytes;
		size_t wanted = min_t(size_t, buf_size - copied,
					      i_size - pos);

		bytes = kernel_read(file, *buf + copied, wanted, &pos);
		if (bytes < 0) {
			ret = bytes;
			goto out_free;
		}

		if (bytes == 0)
			break;
		copied += bytes;
	}

	if (whole_file) {
		if (pos != i_size) {
			ret = -EIO;
			goto out_free;
		}

		ret = security_kernel_post_read_file(file, *buf, i_size, id);
	}

out_free:
	if (ret < 0) {
		if (allocated) {
			vfree(*buf);
			*buf = NULL;
		}
	}

out:
	allow_write_access(file);
	return ret == 0 ? copied : ret;
}
EXPORT_SYMBOL_GPL(kernel_read_file);

ssize_t kernel_read_file_from_path(const char *path, loff_t offset, void **buf,
				   size_t buf_size, size_t *file_size,
				   enum kernel_read_file_id id)
{
	struct file *file;
	ssize_t ret;

	if (!path || !*path)
		return -EINVAL;

	file = filp_open(path, O_RDONLY, 0);
	if (IS_ERR(file))
		return PTR_ERR(file);

	ret = kernel_read_file(file, offset, buf, buf_size, file_size, id);
	fput(file);
	return ret;
}
EXPORT_SYMBOL_GPL(kernel_read_file_from_path);

ssize_t kernel_read_file_from_path_initns(const char *path, loff_t offset,
					  void **buf, size_t buf_size,
					  size_t *file_size,
					  enum kernel_read_file_id id)
{
	struct file *file;
	struct path root;
	ssize_t ret;

	if (!path || !*path)
		return -EINVAL;

	task_lock(&init_task);
	get_fs_root(init_task.fs, &root);
	task_unlock(&init_task);

	file = file_open_root(&root, path, O_RDONLY, 0);
	path_put(&root);
	if (IS_ERR(file))
		return PTR_ERR(file);

	ret = kernel_read_file(file, offset, buf, buf_size, file_size, id);
	fput(file);
	return ret;
}
EXPORT_SYMBOL_GPL(kernel_read_file_from_path_initns);

DEFINE_MUTEX(kread_dup_mutex);
static LIST_HEAD(kread_dup_reqs);

struct kread_dup_req {
	struct list_head list;
	char name[PATH_MAX];
	struct completion first_req_done;
	struct work_struct complete_work;
	struct delayed_work delete_work;
	int dup_ret;
};

static struct kread_dup_req *kread_dup_request_lookup(char *name)
{
	struct kread_dup_req *kread_req;

	list_for_each_entry_rcu(kread_req, &kread_dup_reqs, list,
				lockdep_is_held(&kread_dup_mutex)) {
		if (strlen(kread_req->name) == strlen(name) &&
		    !memcmp(kread_req->name, name, strlen(name))) {
			return kread_req;
                }
        }

	return NULL;
}

static void kread_dup_request_delete(struct work_struct *work)
{
	struct kread_dup_req *kread_req;
	kread_req = container_of(to_delayed_work(work), struct kread_dup_req, delete_work);

	mutex_lock(&kread_dup_mutex);
	list_del_rcu(&kread_req->list);
	synchronize_rcu();
	mutex_unlock(&kread_dup_mutex);
	kfree(kread_req);
}

static void kread_dup_request_complete(struct work_struct *work)
{
	struct kread_dup_req *kread_req;

	kread_req = container_of(work, struct kread_dup_req, complete_work);

	complete_all(&kread_req->first_req_done);
	queue_delayed_work(system_wq, &kread_req->delete_work, 60 * HZ);
}

static bool kread_dup_request_exists_wait(char *name, int *dup_ret)
{
	struct kread_dup_req *kread_req, *new_kread_req;
	int ret;

	/*
	 * Pre-allocate the entry in case we have to use it later
	 * to avoid contention with the mutex.
	 */
	new_kread_req = kzalloc(sizeof(*new_kread_req), GFP_KERNEL);
	if (!new_kread_req)
		return false;

	memcpy(new_kread_req->name, name, strlen(name));
	INIT_WORK(&new_kread_req->complete_work, kread_dup_request_complete);
	INIT_DELAYED_WORK(&new_kread_req->delete_work, kread_dup_request_delete);
	init_completion(&new_kread_req->first_req_done);

	mutex_lock(&kread_dup_mutex);

	kread_req = kread_dup_request_lookup(name);
	if (!kread_req) {
		/*
		 * There was no duplicate, just add the request so we can
		 * keep tab on duplicates later.
		 */
		//pr_info("New kread request for %s\n", name);
		list_add_rcu(&new_kread_req->list, &kread_dup_reqs);
		mutex_unlock(&kread_dup_mutex);
		return false;
	}
	mutex_unlock(&kread_dup_mutex);

	/* We are dealing with a duplicate request now */

	kfree(new_kread_req);

	//pr_warn("kread: duplicate request for file %s\n", name);

	ret = wait_for_completion_state(&kread_req->first_req_done,
					TASK_UNINTERRUPTIBLE | TASK_KILLABLE);
	if (ret) {
		*dup_ret = ret;
		return true;
	}

	/* breath */
	schedule_timeout(2*HZ);

	*dup_ret = kread_req->dup_ret;

	return true;
}

void kread_dup_request_announce(char *name, int ret)
{
	struct kread_dup_req *kread_req;

	mutex_lock(&kread_dup_mutex);

	kread_req = kread_dup_request_lookup(name);
	if (!kread_req)
		goto out;

	kread_req->dup_ret = ret;

	/*
	 * If we complete() here we may allow duplicate threads
	 * to continue before the first one that submitted the
	 * request. We're in no rush but avoid boot delays caused
	 * by these threads waiting too long.
	 */
	queue_work(system_wq, &kread_req->complete_work);

out:
	mutex_unlock(&kread_dup_mutex);
}

ssize_t kernel_read_file_from_fd(int fd, loff_t offset, void **buf,
				 size_t buf_size, size_t *file_size,
				 enum kernel_read_file_id id)
{
	struct fd f = fdget(fd);
	ssize_t ret = -EBADF;
	char *name, *path;
	int dup_ret;

	if (!f.file || !(f.file->f_mode & FMODE_READ))
		goto out;

	path = kzalloc(PATH_MAX, GFP_KERNEL);
	if (!path)
		return -ENOMEM;

	name = file_path(f.file, path, PATH_MAX);
	if (IS_ERR(name)) {
		ret = PTR_ERR(name);
		goto out_mem;
	}

	if (kread_dup_request_exists_wait(name, &dup_ret)) {
		ret = -EBUSY;
		goto out_mem;
	}

	ret = kernel_read_file(f.file, offset, buf, buf_size, file_size, id);

	kread_dup_request_announce(name, ret);

out_mem:
	kfree(path);
out:
	fdput(f);
	return ret;
}
EXPORT_SYMBOL_GPL(kernel_read_file_from_fd);
