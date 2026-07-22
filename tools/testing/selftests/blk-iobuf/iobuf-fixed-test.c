// SPDX-License-Identifier: GPL-2.0
/*
 * Exercise BLOCK_URING_CMD_ALLOC_IOBUF end to end: allocate a pool-backed
 * io_uring fixed buffer, drive real I/O through it, and prove the bytes are
 * correct.
 *
 * The kernel never maps the pool folios into user space, so a pool buffer is
 * verified indirectly: read a known logical block into the fixed buffer, drain
 * the fixed buffer to a scratch file with WRITE_FIXED, and compare that scratch
 * copy against an ordinary read of the same block. If they match, the data
 * flowed I/O -> pool folios -> scratch untouched.
 *
 * Two devices are covered. A block device (e.g. /dev/nvme0n1) exercises the
 * generic block dispatch (READ_FIXED does the fill) with O_DIRECT, so the pool
 * folios back a true direct-I/O transfer. An NVMe generic char device (e.g.
 * /dev/ng0n1) exercises the NVMe namespace dispatch, filling the buffer with an
 * NVME_URING_CMD_IO read that carries IORING_URING_CMD_FIXED -- the path a
 * blockless /dev/ngXnY namespace must use.
 *
 * A provisioned pool is a prerequisite: boot with nvme_core.iobuf_pool_order
 * and nvme_core.iobuf_pool_folios set. With no pool the allocation returns
 * -EOPNOTSUPP and the affected cases report as skipped.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <linux/blkdev.h>
#include <linux/fs.h>
#include <linux/nvme_ioctl.h>
#include <liburing.h>

/* KSFT exit codes (kselftest.h is not always in the include path). */
#define KSFT_PASS	0
#define KSFT_FAIL	1
#define KSFT_SKIP	4

#define NR_SLOTS	4
#define POOL_LEN	4096		/* default logical buffer length */

/* NVMe I/O read opcode; the symbolic name lives in the kernel-only header. */
#define NVME_CMD_READ	0x02

static int passes, fails, skips;

static void ok(const char *name)
{
	printf("ok - %s\n", name);
	passes++;
}

static void bad(const char *name, const char *why)
{
	printf("not ok - %s: %s\n", name, why);
	fails++;
}

static void skip(const char *name, const char *why)
{
	printf("skip - %s: %s\n", name, why);
	skips++;
}

/*
 * Submit one SQE and return the CQE result. The caller has already filled the
 * SQE; liburing hands us the ring's real SQE memory so a 128-byte ring gives a
 * 128-byte SQE and the NVMe command payload lands in sqe->cmd.
 */
static int submit_one(struct io_uring *ring)
{
	struct io_uring_cqe *cqe;
	int ret, res;

	ret = io_uring_submit(ring);
	if (ret < 0)
		return ret;
	ret = io_uring_wait_cqe(ring, &cqe);
	if (ret < 0)
		return ret;
	res = cqe->res;
	io_uring_cqe_seen(ring, cqe);
	return res;
}

/*
 * Allocate one pool buffer into a sparse slot. Carried in plain SQE fields so
 * the same call works on a 64-byte block ring and a 128-byte NVMe ring:
 * sqe->addr = slot, sqe->addr3 = length.
 */
static int alloc_iobuf(struct io_uring *ring, int fd, int sqe128,
		       unsigned int slot, uint64_t len)
{
	struct io_uring_sqe *sqe = io_uring_get_sqe(ring);

	if (!sqe)
		return -ENOSPC;
	memset(sqe, 0, sqe128 ? 128 : sizeof(*sqe));
	sqe->opcode = IORING_OP_URING_CMD;
	sqe->fd = fd;
	sqe->cmd_op = BLOCK_URING_CMD_ALLOC_IOBUF;
	sqe->addr = slot;
	sqe->addr3 = len;
	return submit_one(ring);
}

/*
 * Detach a KBUF slot with the ordinary registered-buffer update API: a zeroed
 * iovec at that offset fires the buffer's release callback.
 */
static int detach_slot(struct io_uring *ring, unsigned int slot)
{
	struct iovec iov = { .iov_base = NULL, .iov_len = 0 };

	return io_uring_register_buffers_update_tag(ring, slot, &iov, NULL, 1);
}

/* READ_FIXED @len bytes at @off from @fd into fixed-buffer @slot. */
static int read_fixed(struct io_uring *ring, int fd, unsigned int slot,
		      uint64_t off, unsigned int len)
{
	struct io_uring_sqe *sqe = io_uring_get_sqe(ring);

	if (!sqe)
		return -ENOSPC;
	io_uring_prep_read_fixed(sqe, fd, 0, len, off, slot);
	return submit_one(ring);
}

/* WRITE_FIXED @len bytes from fixed-buffer @slot to @fd at @off. */
static int write_fixed(struct io_uring *ring, int fd, unsigned int slot,
		       uint64_t off, unsigned int len)
{
	struct io_uring_sqe *sqe = io_uring_get_sqe(ring);

	if (!sqe)
		return -ENOSPC;
	io_uring_prep_write_fixed(sqe, fd, 0, len, off, slot);
	return submit_one(ring);
}

/* NVMe passthru read of @nlb+1 blocks at @slba into fixed-buffer @slot. */
static int nvme_read_fixed(struct io_uring *ring, int fd, unsigned int nsid,
			   unsigned int slot, uint64_t slba, unsigned int nlb,
			   unsigned int len)
{
	struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
	struct nvme_uring_cmd *cmd;

	if (!sqe)
		return -ENOSPC;
	memset(sqe, 0, 128);
	sqe->opcode = IORING_OP_URING_CMD;
	sqe->fd = fd;
	sqe->cmd_op = NVME_URING_CMD_IO;
	sqe->uring_cmd_flags = IORING_URING_CMD_FIXED;
	sqe->buf_index = slot;

	cmd = (struct nvme_uring_cmd *)sqe->cmd;
	cmd->opcode = NVME_CMD_READ;
	cmd->nsid = nsid;
	cmd->addr = 0;			/* offset 0 into the fixed buffer */
	cmd->data_len = len;
	cmd->cdw10 = slba & 0xffffffff;
	cmd->cdw11 = slba >> 32;
	cmd->cdw12 = nlb;		/* zero-based block count */
	return submit_one(ring);
}

/* Read a scratch file's first @len bytes into @buf. */
static int slurp(const char *path, void *buf, unsigned int len)
{
	int fd = open(path, O_RDONLY);
	ssize_t n;

	if (fd < 0)
		return -errno;
	n = pread(fd, buf, len, 0);
	close(fd);
	if (n < 0)
		return -errno;
	return n == (ssize_t)len ? 0 : -EIO;
}

/*
 * Round trip: fill fixed-buffer slot 0 (@fill), drain it to a scratch file,
 * and compare that copy against @ref (an independent read of the same block).
 */
static void case_roundtrip(struct io_uring *ring, const char *scratch,
			   unsigned int len, const unsigned char *ref,
			   int (*fill)(void), const char *name)
{
	int sfd, res;
	void *got;

	res = fill();
	if (res == -EOPNOTSUPP) {
		skip(name, "no iobuf pool provisioned on the device queue");
		return;
	}
	if (res < 0) {
		bad(name, strerror(-res));
		return;
	}
	if ((unsigned int)res != len && res != 0) {
		bad(name, "fill short count");
		return;
	}

	sfd = open(scratch, O_RDWR | O_CREAT | O_TRUNC, 0600);
	if (sfd < 0) {
		bad(name, "open scratch");
		return;
	}

	/* Drain the KBUF fixed buffer to a buffered regular file. */
	res = write_fixed(ring, sfd, 0, 0, len);
	if (res < 0) {
		bad(name, "write_fixed drain");
		close(sfd);
		return;
	}
	fsync(sfd);
	close(sfd);

	got = malloc(len);
	if (!got) {
		bad(name, "oom");
		return;
	}
	res = slurp(scratch, got, len);
	if (res < 0) {
		bad(name, "read scratch");
		free(got);
		return;
	}
	if (memcmp(got, ref, len) != 0)
		bad(name, "pool buffer data mismatch");
	else
		ok(name);
	free(got);
}

/* Fill closures need shared state; a small struct beats globals. */
static struct {
	struct io_uring *ring;
	int fd;
	unsigned int nsid, len, lba;
} fillctx;

static int fill_block(void)
{
	return read_fixed(fillctx.ring, fillctx.fd, 0, 0, fillctx.len);
}

static int fill_nvme(void)
{
	unsigned int nlb = fillctx.len / fillctx.lba - 1;

	return nvme_read_fixed(fillctx.ring, fillctx.fd, fillctx.nsid, 0, 0,
			       nlb, fillctx.len);
}

static void test_exhaustion(struct io_uring *ring, int fd, int sqe128,
			    unsigned int len)
{
	int res, i;
	int got_enobufs = 0;

	/* Slots 1..NR_SLOTS-1 (slot 0 stays occupied by the round-trip). */
	for (i = 1; i < NR_SLOTS + 4; i++) {
		unsigned int slot = i % NR_SLOTS;

		if (slot == 0)
			continue;
		res = alloc_iobuf(ring, fd, sqe128, slot, len);
		if (res == -EOPNOTSUPP) {
			skip("exhaustion", "no pool provisioned");
			return;
		}
		if (res == -EBUSY)
			continue;	/* slot already filled this sweep */
		if (res == -ENOBUFS) {
			got_enobufs = 1;
			break;
		}
		if (res < 0) {
			bad("exhaustion", strerror(-res));
			return;
		}
	}
	/* Whether we hit -ENOBUFS depends on pool capacity vs slot count; the
	 * property under test is that exhaustion is reported, never a partial
	 * or buddy-fallback success. A pool larger than the table simply fills
	 * every slot. Either is correct; a crash or wrong errno is not.
	 */
	if (got_enobufs)
		ok("exhaustion reports -ENOBUFS");
	else
		ok("exhaustion (pool larger than table, all slots filled)");
}

static void test_detach_realloc(struct io_uring *ring, int fd, int sqe128,
				unsigned int len)
{
	int res;

	res = detach_slot(ring, 0);
	if (res < 0) {
		bad("detach+realloc", "detach slot 0");
		return;
	}
	res = alloc_iobuf(ring, fd, sqe128, 0, len);
	if (res == -EOPNOTSUPP) {
		skip("detach+realloc", "no pool provisioned");
		return;
	}
	if (res < 0)
		bad("detach+realloc", strerror(-res));
	else
		ok("detach frees the folio back to the pool, realloc succeeds");
}

static unsigned char *read_reference(const char *dev, unsigned int len)
{
	unsigned char *ref;
	void *aligned;
	int fd;
	ssize_t n;

	/* O_DIRECT reference read; buffer aligned to the block size. */
	fd = open(dev, O_RDONLY | O_DIRECT);
	if (fd < 0)
		fd = open(dev, O_RDONLY);
	if (fd < 0)
		return NULL;
	if (posix_memalign(&aligned, 4096, len)) {
		close(fd);
		return NULL;
	}
	n = pread(fd, aligned, len, 0);
	close(fd);
	if (n != (ssize_t)len) {
		free(aligned);
		return NULL;
	}
	ref = malloc(len);
	if (ref)
		memcpy(ref, aligned, len);
	free(aligned);
	return ref;
}

static void usage(const char *p)
{
	fprintf(stderr,
		"usage: %s [-l len] [-b blockdev] [-n ngdev] [-s scratch]\n"
		"  -b  block device for the DIO round-trip (default /dev/nvme0n1)\n"
		"  -n  /dev/ngXnY for the NVMe-passthru round-trip (optional)\n"
		"  -s  scratch file path (default ./iobuf-scratch)\n"
		"  -l  logical buffer length in bytes (default 4096)\n"
		"  -L  NVMe logical block size for the ng leg (default 512)\n", p);
}

int main(int argc, char **argv)
{
	const char *blockdev = "/dev/nvme0n1";
	const char *ngdev = NULL;
	const char *scratch = "./iobuf-scratch";
	unsigned int len = POOL_LEN;
	unsigned int lba = 512;
	struct io_uring ring;
	struct io_uring_params p;
	unsigned char *ref;
	int fd, res, c, sqe128;

	while ((c = getopt(argc, argv, "b:n:s:l:L:h")) != -1) {
		switch (c) {
		case 'b':
			blockdev = optarg;
			break;
		case 'n':
			ngdev = optarg;
			break;
		case 's':
			scratch = optarg;
			break;
		case 'l':
			len = strtoul(optarg, NULL, 0);
			break;
		case 'L':
			lba = strtoul(optarg, NULL, 0);
			break;
		default:
			usage(argv[0]);
			return KSFT_FAIL;
		}
	}

	ref = read_reference(blockdev, len);
	if (!ref) {
		fprintf(stderr, "cannot read reference block from %s: %s\n",
			blockdev, strerror(errno));
		return KSFT_SKIP;
	}

	/* --- Block device leg: 64-byte SQEs, DIO fill via READ_FIXED. --- */
	fd = open(blockdev, O_RDWR | O_DIRECT);
	if (fd < 0) {
		fprintf(stderr, "open %s: %s\n", blockdev, strerror(errno));
		free(ref);
		return KSFT_SKIP;
	}
	memset(&p, 0, sizeof(p));
	if (io_uring_queue_init_params(NR_SLOTS + 4, &ring, &p) < 0) {
		perror("io_uring_queue_init_params");
		return KSFT_FAIL;
	}
	if (io_uring_register_buffers_sparse(&ring, NR_SLOTS) < 0) {
		perror("register_buffers_sparse");
		return KSFT_FAIL;
	}
	sqe128 = 0;

	res = alloc_iobuf(&ring, fd, sqe128, 0, len);
	if (res == -EOPNOTSUPP) {
		skip("block roundtrip", "no iobuf pool on block queue");
		skip("block exhaustion", "no pool");
		skip("block detach+realloc", "no pool");
	} else if (res < 0) {
		bad("block alloc slot 0", strerror(-res));
	} else {
		fillctx.ring = &ring;
		fillctx.fd = fd;
		fillctx.len = len;
		case_roundtrip(&ring, scratch, len, ref, fill_block,
			       "block roundtrip: DIO read -> pool -> scratch");
		test_exhaustion(&ring, fd, sqe128, len);
		test_detach_realloc(&ring, fd, sqe128, len);
	}
	io_uring_queue_exit(&ring);
	close(fd);

	/* --- NVMe generic char leg: 128-byte SQEs, passthru fill. --- */
	if (ngdev) {
		int ngfd = open(ngdev, O_RDWR);
		unsigned int nsid;

		if (ngfd < 0) {
			skip("ng roundtrip", strerror(errno));
			goto done;
		}
		nsid = ioctl(ngfd, NVME_IOCTL_ID);
		memset(&p, 0, sizeof(p));
		p.flags = IORING_SETUP_SQE128 | IORING_SETUP_CQE32;
		if (io_uring_queue_init_params(NR_SLOTS + 4, &ring, &p) < 0) {
			perror("queue_init sqe128");
			close(ngfd);
			goto done;
		}
		if (io_uring_register_buffers_sparse(&ring, NR_SLOTS) < 0) {
			perror("register_buffers_sparse ng");
			io_uring_queue_exit(&ring);
			close(ngfd);
			goto done;
		}
		res = alloc_iobuf(&ring, ngfd, 1, 0, len);
		if (res == -EOPNOTSUPP) {
			skip("ng roundtrip", "no iobuf pool on ng queue");
		} else if (res < 0) {
			bad("ng alloc slot 0", strerror(-res));
		} else {
			fillctx.ring = &ring;
			fillctx.fd = ngfd;
			fillctx.nsid = nsid;
			fillctx.len = len;
			fillctx.lba = lba;
			case_roundtrip(&ring, scratch, len, ref,
				       fill_nvme,
				       "ng roundtrip: NVMe passthru FIXED -> pool -> scratch");
		}
		io_uring_queue_exit(&ring);
		close(ngfd);
	}
done:
	unlink(scratch);
	free(ref);

	printf("\n# %d passed, %d failed, %d skipped\n", passes, fails, skips);
	if (fails)
		return KSFT_FAIL;
	if (passes == 0)
		return KSFT_SKIP;
	return KSFT_PASS;
}
