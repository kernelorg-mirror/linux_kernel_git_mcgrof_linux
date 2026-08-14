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
#include <getopt.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
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

#define BENCH_DEFAULT_SLOTS	256
#define BENCH_DEFAULT_QD	64
#define BENCH_DEFAULT_DURATION	30
#define BENCH_DEFAULT_BUFFER_SIZE (2U * 1024 * 1024)
#define BENCH_DEFAULT_IO_SIZE	(512U * 1024)
#define BENCH_DEFAULT_WARMUP	5
#define BENCH_MAX_SLOTS		16384	/* IORING_MAX_REG_BUFFERS */
#define BENCH_MAX_QD		4096
#define BENCH_MAX_ALLOCATED_BYTES (1024ULL * 1024 * 1024)
#define LATENCY_HIST_MAX_US	200000

#ifndef BLOCK_URING_CMD_ALLOC_IOBUF
#define BLOCK_URING_CMD_ALLOC_IOBUF		_IO(0x12, 1)
#endif

#ifndef BLOCK_URING_CMD_ALLOC_IOBUF_F_STRICT_PGSIZE
#define BLOCK_URING_CMD_ALLOC_IOBUF_F_STRICT_PGSIZE	(1U << 0)
#endif

#ifndef IORING_URING_CMD_FIXED
#define IORING_URING_CMD_FIXED	(1U << 0)
#endif

#ifndef NVME_URING_CMD_IO_VEC
#define NVME_URING_CMD_IO_VEC	_IOWR('N', 0x81, struct nvme_uring_cmd)
#endif

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
		       unsigned int slot, uint64_t len,
		       unsigned int alloc_flags)
{
	struct io_uring_sqe *sqe = io_uring_get_sqe(ring);

	if (!sqe)
		return -ENOSPC;
	memset(sqe, 0, sqe128 ? 128 : sizeof(*sqe));
	sqe->opcode = IORING_OP_URING_CMD;
	sqe->fd = fd;
	sqe->cmd_op = BLOCK_URING_CMD_ALLOC_IOBUF;
	sqe->len = alloc_flags;
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

/*
 * Submit one fixed-buffer NVMe read command. For NVME_URING_CMD_IO, @addr is
 * the scalar fixed-buffer address/offset and @data_len is the transfer length.
 * For NVME_URING_CMD_IO_VEC, @addr points to the iovec array and @data_len is
 * its element count.
 */
static int nvme_read_fixed_cmd(struct io_uring *ring, int fd,
			       unsigned int nsid, unsigned int slot,
			       unsigned int cmd_op, uint64_t addr,
			       unsigned int data_len, uint64_t slba,
			       unsigned int nlb)
{
	struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
	struct nvme_uring_cmd *cmd;

	if (!sqe)
		return -ENOSPC;
	memset(sqe, 0, 128);
	sqe->opcode = IORING_OP_URING_CMD;
	sqe->fd = fd;
	sqe->cmd_op = cmd_op;
	sqe->uring_cmd_flags = IORING_URING_CMD_FIXED;
	sqe->buf_index = slot;

	cmd = (struct nvme_uring_cmd *)sqe->cmd;
	cmd->opcode = NVME_CMD_READ;
	cmd->nsid = nsid;
	cmd->addr = addr;
	cmd->data_len = data_len;
	cmd->cdw10 = slba & 0xffffffff;
	cmd->cdw11 = slba >> 32;
	cmd->cdw12 = nlb;		/* zero-based block count */
	return submit_one(ring);
}

/* NVMe passthru read of @nlb+1 blocks at @slba into fixed-buffer @slot. */
static int nvme_read_fixed(struct io_uring *ring, int fd, unsigned int nsid,
			   unsigned int slot, uint64_t slba, unsigned int nlb,
			   unsigned int len)
{
	return nvme_read_fixed_cmd(ring, fd, nsid, slot, NVME_URING_CMD_IO,
				   0, len, slba, nlb);
}

static int open_scratch_regular(const char *path)
{
	struct stat st;
	int fd;

	fd = open(path, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
		  0600);
	if (fd < 0)
		return -1;
	if (fstat(fd, &st) || !S_ISREG(st.st_mode)) {
		close(fd);
		errno = EINVAL;
		return -1;
	}
	/* Keep only our file descriptor; never leave or later remove user data. */
	if (unlink(path)) {
		close(fd);
		return -1;
	}
	return fd;
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

	sfd = open_scratch_regular(scratch);
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
	if (fsync(sfd)) {
		bad(name, "fsync scratch");
		close(sfd);
		return;
	}

	got = malloc(len);
	if (!got) {
		bad(name, "oom");
		close(sfd);
		return;
	}
	res = pread(sfd, got, len, 0);
	close(sfd);
	if (res != (int)len) {
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
		res = alloc_iobuf(ring, fd, sqe128, slot, len, 0);
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
	res = alloc_iobuf(ring, fd, sqe128, 0, len, 0);
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

struct bench_opts {
	unsigned int slots;
	unsigned int qd;
	unsigned int duration;
	uint64_t iterations;
	unsigned int buffer_size;
	unsigned int io_size;
	unsigned int warmup;
	const char *json;
	bool strict;
};

struct bench_slot_ctx {
	unsigned int slot;
	uint64_t submit_ns;
	bool inflight;
	bool touched;
};

struct bench_result {
	uint64_t submitted;
	uint64_t completed;
	uint64_t commands;
	uint64_t bytes;
	uint64_t errors;
	uint64_t elapsed_ns;
	uint64_t latency_samples;
	uint64_t latency_min_ns;
	uint64_t latency_max_ns;
	uint64_t latency_sum_ns;
	uint64_t p50_ns;
	uint64_t p95_ns;
	uint64_t p99_ns;
	unsigned int slots_touched;
};

struct premap_stats {
	uint64_t attempts;
	uint64_t successes;
	uint64_t fallbacks;
	uint64_t no_translating_iommu;
	uint64_t pgsize_unsupported;
	uint64_t iova_no_space;
	uint64_t iova_misaligned;
	uint64_t other_failures;
	uint64_t link_failures;
	uint64_t sync_failures;
	uint64_t strict_rejections;
};

struct premap_evidence {
	struct premap_stats delta;
	char stats_path[PATH_MAX];
	bool snapshotted;
	bool verified;
};

struct bench_validation {
	unsigned int slots_validated;
	bool premap_offset_rejected;
	bool premap_vector_rejected;
	bool user_offset_succeeded;
	bool user_vector_succeeded;
};

struct bench_timing {
	uint64_t setup_total_ns;
	uint64_t registration_ns;
	uint64_t warmup_ns;
	uint64_t teardown_total_ns;
	uint64_t unregister_ns;
};

struct premap_stat_desc {
	const char *name;
	size_t offset;
};

static const struct premap_stat_desc premap_stat_descs[] = {
	{ "attempts", offsetof(struct premap_stats, attempts) },
	{ "successes", offsetof(struct premap_stats, successes) },
	{ "fallbacks", offsetof(struct premap_stats, fallbacks) },
	{ "no_translating_iommu",
	  offsetof(struct premap_stats, no_translating_iommu) },
	{ "pgsize_unsupported", offsetof(struct premap_stats, pgsize_unsupported) },
	{ "iova_no_space", offsetof(struct premap_stats, iova_no_space) },
	{ "iova_misaligned", offsetof(struct premap_stats, iova_misaligned) },
	{ "other_failures", offsetof(struct premap_stats, other_failures) },
	{ "link_failures", offsetof(struct premap_stats, link_failures) },
	{ "sync_failures", offsetof(struct premap_stats, sync_failures) },
	{ "strict_rejections", offsetof(struct premap_stats, strict_rejections) },
};

static uint64_t monotonic_ns(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC_RAW, &ts))
		return 0;
	return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static int parse_u64(const char *arg, uint64_t *value)
{
	char *end;
	unsigned long long parsed;

	errno = 0;
	parsed = strtoull(arg, &end, 0);
	if (errno || !arg[0] || *end)
		return -EINVAL;
	*value = parsed;
	return 0;
}

static int parse_uint(const char *arg, unsigned int *value)
{
	uint64_t parsed;

	if (parse_u64(arg, &parsed) || parsed > UINT_MAX)
		return -EINVAL;
	*value = parsed;
	return 0;
}

static int sysfs_device_path(int fd, bool block, const char *suffix,
			     char *path, size_t path_len)
{
	struct stat st;
	int len;

	if (fstat(fd, &st))
		return -errno;
	if ((block && !S_ISBLK(st.st_mode)) ||
	    (!block && !S_ISCHR(st.st_mode)))
		return -ENOTBLK;
	len = snprintf(path, path_len, "/sys/dev/%s/%u:%u%s",
		       block ? "block" : "char", major(st.st_rdev),
		       minor(st.st_rdev), suffix);
	return len < 0 || (size_t)len >= path_len ? -ENAMETOOLONG : 0;
}

static int sysfs_controller_path(int fd, bool block, char *controller,
				 size_t controller_len)
{
	char path[PATH_MAX];
	char resolved[PATH_MAX];
	int ret;

	ret = sysfs_device_path(fd, block, "/device", path, sizeof(path));
	if (ret)
		return ret;
	if (!realpath(path, resolved))
		return -errno;
	if (strlen(resolved) >= controller_len)
		return -ENAMETOOLONG;
	strcpy(controller, resolved);
	return 0;
}

static int premap_stats_path(int blockfd, char *path, size_t path_len)
{
	return sysfs_device_path(blockfd, true,
		"/queue/iobuf_pool_premap_stats", path, path_len);
}

static int read_premap_stats(const char *path, struct premap_stats *stats)
{
	char line[128];
	unsigned int seen = 0;
	FILE *in;
	int ret = 0;

	in = fopen(path, "r");
	if (!in)
		return -errno;
	memset(stats, 0, sizeof(*stats));
	while (fgets(line, sizeof(line), in)) {
		uint64_t *field = NULL, value;
		char *equals;
		size_t i;

		line[strcspn(line, "\r\n")] = '\0';
		if (!strcmp(line, "disabled")) {
			ret = -EOPNOTSUPP;
			break;
		}
		equals = strchr(line, '=');
		if (!equals) {
			ret = -EPROTO;
			break;
		}
		*equals++ = '\0';
		for (i = 0; i < sizeof(premap_stat_descs) /
					      sizeof(premap_stat_descs[0]); i++) {
			if (!strcmp(line, premap_stat_descs[i].name)) {
				field = (uint64_t *)((char *)stats +
					premap_stat_descs[i].offset);
				break;
			}
		}
		if (!field)
			continue;
		if ((seen & (1U << i)) || parse_u64(equals, &value)) {
			ret = -EPROTO;
			break;
		}
		*field = value;
		seen |= 1U << i;
	}
	if (!ret && ferror(in))
		ret = -EIO;
	if (fclose(in) && !ret)
		ret = -errno;
	if (!ret && seen != (1U << (sizeof(premap_stat_descs) /
				       sizeof(premap_stat_descs[0]))) - 1)
		ret = -EPROTO;
	return ret;
}

static int premap_stats_delta(const struct premap_stats *before,
			      const struct premap_stats *after,
			      struct premap_stats *delta)
{
	size_t i;

	memset(delta, 0, sizeof(*delta));
	for (i = 0; i < sizeof(premap_stat_descs) /
				      sizeof(premap_stat_descs[0]); i++) {
		const uint64_t *before_value =
			(const uint64_t *)((const char *)before +
					  premap_stat_descs[i].offset);
		const uint64_t *after_value =
			(const uint64_t *)((const char *)after +
					  premap_stat_descs[i].offset);
		uint64_t *delta_value = (uint64_t *)((char *)delta +
						 premap_stat_descs[i].offset);

		if (*after_value < *before_value)
			return -ERANGE;
		*delta_value = *after_value - *before_value;
	}
	return 0;
}

static void print_premap_delta(FILE *out, const struct premap_stats *delta)
{
	fprintf(out,
		"attempts=%" PRIu64 " successes=%" PRIu64
		" fallbacks=%" PRIu64 " no_translating_iommu=%" PRIu64
		" pgsize_unsupported=%" PRIu64 " iova_no_space=%" PRIu64
		" iova_misaligned=%" PRIu64 " other_failures=%" PRIu64
		" link_failures=%" PRIu64 " sync_failures=%" PRIu64
		" strict_rejections=%" PRIu64,
		delta->attempts, delta->successes, delta->fallbacks,
		delta->no_translating_iommu, delta->pgsize_unsupported,
		delta->iova_no_space, delta->iova_misaligned,
		delta->other_failures, delta->link_failures,
		delta->sync_failures, delta->strict_rejections);
}

static int prep_nvme_read_fixed(struct io_uring *ring, int fd,
				unsigned int nsid, unsigned int slot,
				unsigned int nlb, unsigned int len,
				struct bench_slot_ctx *ctx)
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
	sqe->user_data = (uintptr_t)ctx;

	cmd = (struct nvme_uring_cmd *)sqe->cmd;
	cmd->opcode = NVME_CMD_READ;
	cmd->nsid = nsid;
	cmd->addr = 0;
	cmd->data_len = len;
	cmd->cdw10 = 0;
	cmd->cdw11 = 0;
	cmd->cdw12 = nlb;
	return 0;
}

static uint64_t latency_percentile(const uint64_t *hist, uint64_t samples,
				   unsigned int percentile,
				   uint64_t max_ns)
{
	uint64_t target, seen = 0;
	unsigned int i;

	if (!samples)
		return 0;
	target = (samples * percentile + 99) / 100;
	for (i = 0; i <= LATENCY_HIST_MAX_US; i++) {
		seen += hist[i];
		if (seen >= target)
			return (uint64_t)(i + 1) * 1000;
	}
	return max_ns;
}

/*
 * Keep @qd NVMe commands outstanding. A slot is not reused until its CQE has
 * been reaped; next_slot advances monotonically so every registered buffer is
 * exercised instead of hammering only the first @qd slots.
 */
static int run_async_phase(struct io_uring *ring, int fd, unsigned int nsid,
			   const struct bench_opts *opts, unsigned int seconds,
			   uint64_t iterations, unsigned int nlb, bool record,
			   struct bench_slot_ctx *ctx,
			   struct bench_result *result)
{
	uint64_t *hist = NULL;
	uint64_t start, deadline = 0, end;
	unsigned int active = 0, next_slot = 0;
	bool stop = false, io_error = false;
	int ret = 0;

	if (record) {
		hist = calloc(LATENCY_HIST_MAX_US + 2, sizeof(*hist));
		if (!hist)
			return -ENOMEM;
	}
	memset(ctx, 0, opts->slots * sizeof(*ctx));
	for (next_slot = 0; next_slot < opts->slots; next_slot++)
		ctx[next_slot].slot = next_slot;
	next_slot = 0;

	memset(result, 0, sizeof(*result));
	start = monotonic_ns();
	if (seconds)
		deadline = start + (uint64_t)seconds * 1000000000ULL;

	while (!stop || active) {
		while (!stop && active < opts->qd) {
			struct bench_slot_ctx *slot = NULL;
			unsigned int scanned;
			uint64_t now = monotonic_ns();

			if ((deadline && now >= deadline &&
			     result->submitted >= opts->slots) ||
			    (iterations && result->submitted >= iterations)) {
				stop = true;
				break;
			}
			for (scanned = 0; scanned < opts->slots; scanned++) {
				struct bench_slot_ctx *candidate = &ctx[next_slot];

				next_slot = (next_slot + 1) % opts->slots;
				if (!candidate->inflight) {
					slot = candidate;
					break;
				}
			}
			if (!slot) {
				ret = -EDEADLK;
				stop = true;
				break;
			}

			slot->submit_ns = now;
			slot->inflight = true;
			if (!slot->touched) {
				slot->touched = true;
				result->slots_touched++;
			}
			ret = prep_nvme_read_fixed(ring, fd, nsid, slot->slot,
						   nlb, opts->io_size, slot);
			if (ret) {
				slot->inflight = false;
				stop = true;
				break;
			}
			active++;
			result->submitted++;
		}

		if (active) {
			struct io_uring_cqe *cqe;
			struct bench_slot_ctx *slot;
			uint64_t complete_ns, latency_ns;
			uintptr_t data, first, last;

			if (!stop || result->submitted) {
				int submitted = io_uring_submit(ring);

				if (submitted < 0) {
					ret = submitted;
					stop = true;
				}
			}
			if (ret < 0 && active)
				break;
			ret = io_uring_wait_cqe(ring, &cqe);
			if (ret < 0) {
				stop = true;
				break;
			}

			complete_ns = monotonic_ns();
			data = (uintptr_t)cqe->user_data;
			first = (uintptr_t)&ctx[0];
			last = (uintptr_t)&ctx[opts->slots];
			if (data < first || data >= last ||
			    (data - first) % sizeof(*ctx)) {
				fprintf(stderr, "benchmark CQE has invalid context\n");
				result->errors++;
				io_error = true;
				stop = true;
				io_uring_cqe_seen(ring, cqe);
				break;
			}
			slot = (struct bench_slot_ctx *)data;
			if (!slot->inflight) {
				fprintf(stderr, "benchmark CQE reused free slot %u\n",
					slot->slot);
				result->errors++;
				io_error = true;
				stop = true;
				io_uring_cqe_seen(ring, cqe);
				break;
			}
			slot->inflight = false;
			active--;
			result->completed++;
			latency_ns = complete_ns - slot->submit_ns;
			if (cqe->res != 0) {
				if (!io_error)
					fprintf(stderr,
						"NVMe CQE failed: expected 0, got %d\n",
						cqe->res);
				result->errors++;
				io_error = true;
				stop = true;
			} else {
				unsigned int bucket;

				result->commands++;
				result->bytes += opts->io_size;
				if (record) {
					bucket = latency_ns / 1000;
					if (bucket > LATENCY_HIST_MAX_US)
						bucket = LATENCY_HIST_MAX_US + 1;
					hist[bucket]++;
					result->latency_samples++;
					result->latency_sum_ns += latency_ns;
					if (!result->latency_min_ns ||
					    latency_ns < result->latency_min_ns)
						result->latency_min_ns = latency_ns;
					if (latency_ns > result->latency_max_ns)
						result->latency_max_ns = latency_ns;
				}
			}
			io_uring_cqe_seen(ring, cqe);
		} else if (!stop) {
			ret = -EIO;
			break;
		}
	}
	end = monotonic_ns();
	result->elapsed_ns = end - start;
	if (record) {
		result->p50_ns = latency_percentile(
			hist, result->latency_samples, 50, result->latency_max_ns);
		result->p95_ns = latency_percentile(
			hist, result->latency_samples, 95, result->latency_max_ns);
		result->p99_ns = latency_percentile(
			hist, result->latency_samples, 99, result->latency_max_ns);
	}
	free(hist);
	if (ret < 0)
		return ret;
	return io_error ? -EIO : 0;
}

static int validate_nvme_slots(struct io_uring *ring, int ngfd,
			       unsigned int nsid, unsigned int first_slot,
			       unsigned int nr_slots, unsigned int len,
			       unsigned int lba, const char *scratch,
			       const unsigned char *ref,
			       unsigned int *validated)
{
	void *got;
	unsigned int slot;
	int fd, ret = 0;

	*validated = 0;
	if (!nr_slots || first_slot > UINT_MAX - nr_slots)
		return -EINVAL;
	fd = open_scratch_regular(scratch);
	if (fd < 0)
		return -errno;
	got = malloc(len);
	if (!got) {
		close(fd);
		return -ENOMEM;
	}

	for (slot = first_slot; slot < first_slot + nr_slots; slot++) {
		ret = nvme_read_fixed(ring, ngfd, nsid, slot, 0,
				      len / lba - 1, len);
		/* NVMe uring command completions return status 0. */
		if (ret) {
			ret = ret < 0 ? ret : -EIO;
			break;
		}
		ret = write_fixed(ring, fd, slot, 0, len);
		if (ret != (int)len) {
			ret = ret < 0 ? ret : -EIO;
			break;
		}
		ret = pread(fd, got, len, 0);
		if (ret != (int)len) {
			ret = ret < 0 ? -errno : -EIO;
			break;
		}
		if (memcmp(got, ref, len)) {
			ret = -EILSEQ;
			break;
		}
		(*validated)++;
	}
	if (!ret && fsync(fd))
		ret = -errno;
	if (close(fd) && !ret)
		ret = -errno;
	free(got);
	return ret;
}

static void invert_reference(unsigned char *dst, const unsigned char *ref,
			     unsigned int len)
{
	unsigned int i;

	for (i = 0; i < len; i++)
		dst[i] = ref[i] ^ 0xff;
}

/*
 * A retained DMA mapping currently represents the whole pool buffer at offset
 * zero.  Prove unsupported scalar-offset and vector shapes fail closed, then
 * prove the same shapes still work through the ordinary dynamic map path.
 * Every command is a namespace read; the inverse fill below also proves that a
 * successful control command actually replaced the requested memory range.
 */
static int run_premap_shape_regressions(struct io_uring *pool_ring, int ngfd,
					unsigned int nsid,
					unsigned int pool_slot,
					unsigned int buffer_size,
					unsigned int lba,
					const unsigned char *ref,
					struct bench_validation *validation)
{
	struct io_uring control_ring;
	struct io_uring_params p = { };
	struct iovec pool_vec = { .iov_base = NULL, .iov_len = lba };
	struct iovec control_iov, control_vec;
	unsigned char *control = NULL;
	unsigned int control_len = 2 * lba;
	bool ring_ready = false, buffer_registered = false;
	int ret, cleanup_ret;

	if (buffer_size < control_len)
		return -EINVAL;

	ret = nvme_read_fixed_cmd(pool_ring, ngfd, nsid, pool_slot,
				  NVME_URING_CMD_IO, lba, lba, 0, 0);
	if (ret != -EINVAL) {
		fprintf(stderr,
			"retained premap scalar offset: expected -EINVAL, got %d\n",
			ret);
		return ret < 0 ? ret : -EIO;
	}
	validation->premap_offset_rejected = true;

	ret = nvme_read_fixed_cmd(pool_ring, ngfd, nsid, pool_slot,
				  NVME_URING_CMD_IO_VEC,
				  (uintptr_t)&pool_vec, 1, 0, 0);
	if (ret != -EINVAL) {
		fprintf(stderr,
			"retained premap vector: expected -EINVAL, got %d\n", ret);
		return ret < 0 ? ret : -EIO;
	}
	validation->premap_vector_rejected = true;

	ret = posix_memalign((void **)&control, 4096, control_len);
	if (ret)
		return -ret;
	control_iov.iov_base = control;
	control_iov.iov_len = control_len;
	p.flags = IORING_SETUP_SQE128 | IORING_SETUP_CQE32;
	ret = io_uring_queue_init_params(8, &control_ring, &p);
	if (ret < 0)
		goto out;
	ring_ready = true;
	ret = io_uring_register_buffers(&control_ring, &control_iov, 1);
	if (ret < 0)
		goto out;
	buffer_registered = true;

	invert_reference(control + lba, ref, lba);
	ret = nvme_read_fixed_cmd(&control_ring, ngfd, nsid, 0,
				  NVME_URING_CMD_IO,
				  (uintptr_t)(control + lba), lba, 0, 0);
	if (ret) {
		fprintf(stderr,
			"ordinary fixed scalar offset control: expected 0, got %d\n",
			ret);
		ret = ret < 0 ? ret : -EIO;
		goto out;
	}
	if (memcmp(control + lba, ref, lba)) {
		fprintf(stderr,
			"ordinary fixed scalar offset control returned stale data\n");
		ret = -EILSEQ;
		goto out;
	}
	validation->user_offset_succeeded = true;

	invert_reference(control, ref, lba);
	control_vec.iov_base = control;
	control_vec.iov_len = lba;
	ret = nvme_read_fixed_cmd(&control_ring, ngfd, nsid, 0,
				  NVME_URING_CMD_IO_VEC,
				  (uintptr_t)&control_vec, 1, 0, 0);
	if (ret) {
		fprintf(stderr,
			"ordinary fixed vector control: expected 0, got %d\n", ret);
		ret = ret < 0 ? ret : -EIO;
		goto out;
	}
	if (memcmp(control, ref, lba)) {
		fprintf(stderr,
			"ordinary fixed vector control returned stale data\n");
		ret = -EILSEQ;
		goto out;
	}
	validation->user_vector_succeeded = true;
out:
	if (buffer_registered) {
		cleanup_ret = io_uring_unregister_buffers(&control_ring);
		if (cleanup_ret < 0 && !ret)
			ret = cleanup_ret;
	}
	if (ring_ready)
		io_uring_queue_exit(&control_ring);
	free(control);
	return ret;
}

static void json_string(FILE *out, const char *value)
{
	const unsigned char *p = (const unsigned char *)value;

	fputc('"', out);
	for (; *p; p++) {
		if (*p == '"' || *p == '\\')
			fputc('\\', out);
		if (*p >= 0x20)
			fputc(*p, out);
	}
	fputc('"', out);
}

static int write_bench_json(const char *path, const char *blockdev,
			    const char *ngdev, const struct bench_opts *opts,
			    const struct bench_result *result,
			    unsigned int nsid, unsigned int lba,
			    uint64_t device_bytes, const char *controller,
			    const struct bench_timing *timing,
			    const struct premap_evidence *premap,
			    const struct bench_validation *validation,
			    bool pre_valid,
			    bool post_valid)
{
	FILE *out;
	double seconds = (double)result->elapsed_ns / 1e9;
	double iops = seconds ? result->commands / seconds : 0;
	double gibps = seconds ? result->bytes / seconds / (1024.0 * 1024 * 1024) : 0;
	double mean_ns = result->latency_samples ?
		(double)result->latency_sum_ns / result->latency_samples : 0;

	out = !strcmp(path, "-") ? stdout : fopen(path, "w");
	if (!out)
		return -errno;
	fputs("{\n  \"mode\": \"blk-iobuf-nvme-read\",\n  \"block_device\": ", out);
	json_string(out, blockdev);
	fputs(",\n  \"ng_device\": ", out);
	json_string(out, ngdev);
	fputs(",\n  \"controller_sysfs\": ", out);
	json_string(out, controller);
	fputs(",\n  \"premap_stats_path\": ", out);
	json_string(out, premap->stats_path);
	fprintf(out,
		",\n  \"strict\": %s,\n"
		"  \"nsid\": %u,\n  \"lba_size\": %u,\n"
		"  \"device_bytes\": %" PRIu64 ",\n"
		"  \"slots\": %u,\n  \"slots_touched\": %u,\n"
		"  \"qd\": %u,\n  \"buffer_size\": %u,\n"
		"  \"allocated_bytes\": %" PRIu64 ",\n"
		"  \"io_size\": %u,\n  \"duration_seconds_requested\": %u,\n"
		"  \"warmup_seconds_requested\": %u,\n"
		"  \"iterations_requested\": %" PRIu64 ",\n"
		"  \"submitted\": %" PRIu64 ",\n"
		"  \"completed\": %" PRIu64 ",\n"
		"  \"commands\": %" PRIu64 ",\n"
		"  \"bytes\": %" PRIu64 ",\n"
		"  \"elapsed_ns\": %" PRIu64 ",\n"
		"  \"iops\": %.6f,\n  \"GiBps\": %.9f,\n"
		"  \"latency_ns\": {\"samples\": %" PRIu64
		", \"min\": %" PRIu64 ", \"mean\": %.3f, \"p50\": %" PRIu64
		", \"p95\": %" PRIu64 ", \"p99\": %" PRIu64
		", \"max\": %" PRIu64 "},\n"
		"  \"premap_stats_snapshotted\": %s,\n"
		"  \"premap_verified\": %s,\n"
		"  \"premap_stats_delta\": {"
		"\"attempts\": %" PRIu64 ", \"successes\": %" PRIu64
		", \"fallbacks\": %" PRIu64
		", \"no_translating_iommu\": %" PRIu64
		", \"pgsize_unsupported\": %" PRIu64
		", \"iova_no_space\": %" PRIu64
		", \"iova_misaligned\": %" PRIu64
		", \"other_failures\": %" PRIu64
		", \"link_failures\": %" PRIu64
		", \"sync_failures\": %" PRIu64
		", \"strict_rejections\": %" PRIu64 "},\n"
		"  \"timing_ns\": {"
		"\"setup_total\": %" PRIu64
		", \"buffer_registration\": %" PRIu64
		", \"warmup\": %" PRIu64
		", \"measurement\": %" PRIu64
		", \"teardown_total\": %" PRIu64
		", \"buffer_unregister\": %" PRIu64 "},\n"
		"  \"validation\": {\"slots_validated\": %u"
		", \"premap_nonzero_offset_rejected\": %s"
		", \"premap_vector_rejected\": %s"
		", \"user_fixed_nonzero_offset_succeeded\": %s"
		", \"user_fixed_vector_succeeded\": %s},\n"
		"  \"pre_validation\": %s,\n  \"post_validation\": %s,\n"
		"  \"errors\": %" PRIu64 "\n}\n",
		opts->strict ? "true" : "false", nsid, lba, device_bytes,
		opts->slots,
		result->slots_touched, opts->qd, opts->buffer_size,
		(uint64_t)opts->slots * opts->buffer_size, opts->io_size,
		opts->duration, opts->warmup, opts->iterations,
		result->submitted, result->completed, result->commands,
		result->bytes, result->elapsed_ns, iops, gibps,
		result->latency_samples, result->latency_min_ns, mean_ns,
		result->p50_ns, result->p95_ns, result->p99_ns,
		result->latency_max_ns,
		premap->snapshotted ? "true" : "false",
		premap->verified ? "true" : "false",
		premap->delta.attempts, premap->delta.successes,
		premap->delta.fallbacks, premap->delta.no_translating_iommu,
		premap->delta.pgsize_unsupported, premap->delta.iova_no_space,
		premap->delta.iova_misaligned, premap->delta.other_failures,
		premap->delta.link_failures, premap->delta.sync_failures,
		premap->delta.strict_rejections,
		timing->setup_total_ns, timing->registration_ns,
		timing->warmup_ns, result->elapsed_ns,
		timing->teardown_total_ns, timing->unregister_ns,
		validation->slots_validated,
		validation->premap_offset_rejected ? "true" : "false",
		validation->premap_vector_rejected ? "true" : "false",
		validation->user_offset_succeeded ? "true" : "false",
		validation->user_vector_succeeded ? "true" : "false",
		pre_valid ? "true" : "false", post_valid ? "true" : "false",
		result->errors);
	if (out != stdout && fclose(out))
		return -errno;
	return 0;
}

static int run_benchmark(const char *blockdev, const char *ngdev,
			 const char *scratch, unsigned int lba,
			 const struct bench_opts *opts)
{
	struct io_uring ring;
	struct io_uring_params p = { };
	struct bench_result result = { }, warm = { };
	struct bench_slot_ctx *ctx = NULL;
	struct premap_stats stats_before, stats_after;
	struct premap_evidence premap = { };
	struct bench_validation validation = { };
	struct bench_timing timing = { };
	unsigned char *ref = NULL;
	uint64_t setup_start, teardown_start = 0, registration_start;
	uint64_t device_bytes = 0, total_pool;
	unsigned int nsid = 0, logical = 0, alloc_flags = 0, slot;
	unsigned int ring_entries;
	char block_controller[PATH_MAX] = { };
	char ng_controller[PATH_MAX] = { };
	bool ring_ready = false, buffers_registered = false;
	bool pre_valid = false, post_valid = false;
	int blockfd = -1, ngfd = -1, block_nsid, ret = -EINVAL;

	if (!ngdev) {
		fprintf(stderr, "--bench requires -n/--ngdev\n");
		return KSFT_FAIL;
	}
	if (!opts->slots || opts->slots > BENCH_MAX_SLOTS || !opts->qd ||
	    opts->qd > BENCH_MAX_QD || opts->qd > opts->slots) {
		fprintf(stderr, "invalid slots/qd (1 <= qd <= slots, slots <= %u, qd <= %u)\n",
			BENCH_MAX_SLOTS, BENCH_MAX_QD);
		return KSFT_FAIL;
	}
	if (!lba || !opts->buffer_size || !opts->io_size ||
	    opts->io_size > opts->buffer_size || opts->buffer_size % lba ||
	    opts->io_size % lba || opts->io_size / lba - 1 > UINT16_MAX ||
	    (uint64_t)opts->buffer_size < (uint64_t)lba * 2 ||
	    opts->buffer_size > INT_MAX || opts->io_size > INT_MAX) {
		fprintf(stderr,
			"buffer/io sizes must be nonzero LBA multiples; "
			"io-size <= buffer-size, buffer-size >= 2 LBAs, "
			"and io-size <= 65536 LBAs\n");
		return KSFT_FAIL;
	}
	if (!opts->duration && !opts->iterations) {
		fprintf(stderr, "benchmark needs nonzero --duration or --iterations\n");
		return KSFT_FAIL;
	}
	if (opts->iterations && opts->iterations < opts->slots) {
		fprintf(stderr, "--iterations must cover at least one cycle through all slots\n");
		return KSFT_FAIL;
	}
	total_pool = (uint64_t)opts->slots * opts->buffer_size;
	if (total_pool / opts->buffer_size != opts->slots ||
	    total_pool > BENCH_MAX_ALLOCATED_BYTES) {
		fprintf(stderr, "registered buffers exceed the 1 GiB safety cap\n");
		return KSFT_FAIL;
	}

	setup_start = monotonic_ns();
	blockfd = open(blockdev, O_RDONLY | O_DIRECT);
	if (blockfd < 0)
		blockfd = open(blockdev, O_RDONLY);
	if (blockfd < 0) {
		ret = -errno;
		fprintf(stderr, "open reference device %s: %s\n", blockdev,
			strerror(errno));
		goto out;
	}
	if (ioctl(blockfd, BLKGETSIZE64, &device_bytes) < 0 ||
	    ioctl(blockfd, BLKSSZGET, &logical) < 0) {
		ret = -errno;
		fprintf(stderr, "query block geometry: %s\n", strerror(errno));
		goto out;
	}
	if (logical != lba) {
		ret = -EINVAL;
		fprintf(stderr, "configured LBA %u does not match device logical block %u\n",
			lba, logical);
		goto out;
	}
	if (opts->io_size > device_bytes) {
		ret = -ERANGE;
		fprintf(stderr, "io-size exceeds device range\n");
		goto out;
	}

	ngfd = open(ngdev, O_RDONLY);
	if (ngfd < 0) {
		ret = -errno;
		fprintf(stderr, "open %s read-only: %s\n", ngdev, strerror(errno));
		goto out;
	}
	ret = ioctl(ngfd, NVME_IOCTL_ID);
	if (ret < 0) {
		fprintf(stderr, "NVME_IOCTL_ID on %s: %s\n", ngdev,
			strerror(errno));
		goto out;
	}
	nsid = ret;
	block_nsid = ioctl(blockfd, NVME_IOCTL_ID);
	if (block_nsid < 0) {
		ret = -errno;
		fprintf(stderr, "NVME_IOCTL_ID on %s: %s\n", blockdev,
			strerror(errno));
		goto out;
	}
	if ((unsigned int)block_nsid != nsid) {
		ret = -EXDEV;
		fprintf(stderr,
			"namespace mismatch: %s has NSID %u, %s has NSID %u\n",
			blockdev, (unsigned int)block_nsid, ngdev, nsid);
		goto out;
	}
	ret = sysfs_controller_path(blockfd, true, block_controller,
				    sizeof(block_controller));
	if (ret) {
		fprintf(stderr, "resolve %s controller ancestry: %s\n",
			blockdev, strerror(-ret));
		goto out;
	}
	ret = sysfs_controller_path(ngfd, false, ng_controller,
				    sizeof(ng_controller));
	if (ret) {
		fprintf(stderr, "resolve %s controller ancestry: %s\n",
			ngdev, strerror(-ret));
		goto out;
	}
	if (strcmp(block_controller, ng_controller)) {
		ret = -EXDEV;
		fprintf(stderr,
			"controller mismatch:\n  %s -> %s\n  %s -> %s\n",
			blockdev, block_controller, ngdev, ng_controller);
		goto out;
	}

	ref = read_reference(blockdev, opts->io_size);
	if (!ref) {
		ret = -EIO;
		fprintf(stderr, "cannot read reference bytes from %s\n", blockdev);
		goto out;
	}
	p.flags = IORING_SETUP_SQE128 | IORING_SETUP_CQE32;
	ring_entries = opts->qd < 8 ? 8 : opts->qd;
	ret = io_uring_queue_init_params(ring_entries, &ring, &p);
	if (ret < 0) {
		fprintf(stderr, "queue_init sqe128: %s\n", strerror(-ret));
		goto out;
	}
	ring_ready = true;
	ret = io_uring_register_buffers_sparse(&ring, opts->slots);
	if (ret < 0) {
		fprintf(stderr, "register %u sparse buffers: %s\n", opts->slots,
			strerror(-ret));
		goto out;
	}
	buffers_registered = true;
	ctx = calloc(opts->slots, sizeof(*ctx));
	if (!ctx) {
		ret = -ENOMEM;
		fprintf(stderr, "allocate benchmark contexts: %s\n",
			strerror(-ret));
		goto out;
	}
	if (opts->strict)
		alloc_flags = BLOCK_URING_CMD_ALLOC_IOBUF_F_STRICT_PGSIZE;
	ret = premap_stats_path(blockfd, premap.stats_path,
				sizeof(premap.stats_path));
	if (ret) {
		fprintf(stderr, "resolve iobuf premap stats path: %s\n",
			strerror(-ret));
		goto out;
	}
	ret = read_premap_stats(premap.stats_path, &stats_before);
	if (ret) {
		fprintf(stderr, "read pre-allocation premap stats %s: %s\n",
			premap.stats_path, strerror(-ret));
		goto out;
	}
	registration_start = monotonic_ns();
	for (slot = 0; slot < opts->slots; slot++) {
		ret = alloc_iobuf(&ring, ngfd, 1, slot, opts->buffer_size,
				  alloc_flags);
		if (ret) {
			fprintf(stderr, "allocate slot %u/%u: %s (%d)\n", slot,
				opts->slots, ret < 0 ? strerror(-ret) : "unexpected CQE",
				ret);
			break;
		}
	}
	timing.registration_ns = monotonic_ns() - registration_start;
	{
		int alloc_ret = ret;

		ret = read_premap_stats(premap.stats_path, &stats_after);
		if (ret) {
			fprintf(stderr, "read post-allocation premap stats %s: %s\n",
				premap.stats_path, strerror(-ret));
			goto out;
		}
		ret = premap_stats_delta(&stats_before, &stats_after,
					 &premap.delta);
		if (ret) {
			fprintf(stderr,
				"premap stats counters moved backwards: %s\n",
				strerror(-ret));
			goto out;
		}
		premap.snapshotted = true;
		premap.verified = premap.delta.attempts == opts->slots &&
			premap.delta.successes == opts->slots &&
			premap.delta.fallbacks == 0;
		if (!premap.verified) {
			fprintf(stderr,
				"premap verification failed; quiet run required\n"
				"  expected: attempts=%u successes=%u fallbacks=0\n"
				"  observed: ", opts->slots, opts->slots);
			print_premap_delta(stderr, &premap.delta);
			fputs("\n  rerun with exclusive access to this queue\n",
			      stderr);
		}
		if (alloc_ret) {
			ret = alloc_ret < 0 ? alloc_ret : -EIO;
			goto out;
		}
		if (!premap.verified) {
			ret = -EAGAIN;
			goto out;
		}
	}
	ret = run_premap_shape_regressions(&ring, ngfd, nsid, 0,
					   opts->buffer_size, lba, ref,
					   &validation);
	if (ret) {
		fprintf(stderr, "premap shape regression failed: %s\n",
			strerror(-ret));
		goto out;
	}
	ret = validate_nvme_slots(&ring, ngfd, nsid, 0, opts->slots,
				  opts->io_size, lba, scratch, ref,
				  &validation.slots_validated);
	if (ret) {
		fprintf(stderr,
			"pre-benchmark data validation failed after %u/%u slots: %s\n",
			validation.slots_validated, opts->slots,
			ret < 0 ? strerror(-ret) : "unexpected CQE");
		goto out;
	}
	pre_valid = true;
	timing.setup_total_ns = monotonic_ns() - setup_start;

	if (opts->warmup) {
		ret = run_async_phase(&ring, ngfd, nsid, opts, opts->warmup, 0,
				      opts->io_size / lba - 1, false, ctx, &warm);
		timing.warmup_ns = warm.elapsed_ns;
		if (ret) {
			fprintf(stderr, "warmup failed: %s\n", strerror(-ret));
			result.errors += warm.errors ? warm.errors : 1;
			if (warm.completed != warm.submitted)
				goto out;
			goto teardown;
		}
	}

	ret = run_async_phase(&ring, ngfd, nsid, opts, opts->duration,
			      opts->iterations, opts->io_size / lba - 1,
			      true, ctx, &result);
	if (ret) {
		fprintf(stderr, "benchmark I/O failed: %s\n", strerror(-ret));
		if (!result.errors)
			result.errors++;
		if (result.completed != result.submitted)
			goto out;
	}
	if (result.slots_touched != opts->slots) {
		fprintf(stderr, "benchmark touched %u of %u registered slots\n",
			result.slots_touched, opts->slots);
		result.errors++;
	}

teardown:
	teardown_start = monotonic_ns();
	{
		int validation_ret;
		unsigned int slots_validated;

		validation_ret = validate_nvme_slots(
			&ring, ngfd, nsid, opts->slots - 1, 1, opts->io_size,
			lba, scratch, ref, &slots_validated);
		if (!validation_ret) {
			post_valid = true;
		} else {
			fprintf(stderr, "post-benchmark data validation failed: %s\n",
				strerror(-validation_ret));
			result.errors++;
		}
	}
out:
	if (!timing.setup_total_ns)
		timing.setup_total_ns = monotonic_ns() - setup_start;
	if (!teardown_start)
		teardown_start = monotonic_ns();
	if (buffers_registered) {
		uint64_t unregister_start = monotonic_ns();
		int unregister_ret;

		unregister_ret = io_uring_unregister_buffers(&ring);
		timing.unregister_ns = monotonic_ns() - unregister_start;
		if (unregister_ret < 0) {
			fprintf(stderr, "unregister sparse buffers: %s\n",
				strerror(-unregister_ret));
			result.errors++;
			if (!ret)
				ret = unregister_ret;
		} else {
			buffers_registered = false;
		}
	}
	if (ring_ready)
		io_uring_queue_exit(&ring);
	free(ctx);
	if (ngfd >= 0)
		close(ngfd);
	if (blockfd >= 0)
		close(blockfd);
	free(ref);
	timing.teardown_total_ns = monotonic_ns() - teardown_start;
	if (ret && !result.errors)
		result.errors++;

	{
		FILE *summary = opts->json && !strcmp(opts->json, "-") ?
			stderr : stdout;
		double seconds = (double)result.elapsed_ns / 1e9;
		double iops = seconds ? result.commands / seconds : 0;
		double gibps = seconds ? result.bytes / seconds /
			(1024.0 * 1024 * 1024) : 0;

		fprintf(summary, "commands=%" PRIu64 " bytes=%" PRIu64
			" iops=%.2f GiB/s=%.3f p50=%" PRIu64
			"ns p95=%" PRIu64 "ns p99=%" PRIu64
			"ns errors=%" PRIu64 "\n",
			result.commands, result.bytes, iops, gibps,
			result.p50_ns, result.p95_ns, result.p99_ns,
			result.errors);
		fprintf(summary, "premap_verified=%s premap_delta ",
			premap.verified ? "true" : "false");
		print_premap_delta(summary, &premap.delta);
		fprintf(summary,
			"\ntiming_ns setup_total=%" PRIu64
			" buffer_registration=%" PRIu64
			" warmup=%" PRIu64 " measurement=%" PRIu64
			" teardown_total=%" PRIu64
			" buffer_unregister=%" PRIu64 "\n",
			timing.setup_total_ns, timing.registration_ns,
			timing.warmup_ns, result.elapsed_ns,
			timing.teardown_total_ns, timing.unregister_ns);
		fprintf(summary,
			"validation slots=%u/%u premap_offset_rejected=%s "
			"premap_vector_rejected=%s user_offset_succeeded=%s "
			"user_vector_succeeded=%s\n",
			validation.slots_validated, opts->slots,
			validation.premap_offset_rejected ? "true" : "false",
			validation.premap_vector_rejected ? "true" : "false",
			validation.user_offset_succeeded ? "true" : "false",
			validation.user_vector_succeeded ? "true" : "false");
		if (opts->json) {
			int json_ret = write_bench_json(opts->json, blockdev, ngdev,
					opts, &result, nsid, lba, device_bytes,
					block_controller, &timing, &premap,
					&validation,
					pre_valid, post_valid);

			if (json_ret) {
				fprintf(stderr, "write JSON %s: %s\n", opts->json,
					strerror(-json_ret));
				result.errors++;
			}
		}
	}
	return !timing.setup_total_ns || ret || result.errors ||
		!premap.verified || !pre_valid || !post_valid ||
		validation.slots_validated != opts->slots ||
		!validation.premap_offset_rejected ||
		!validation.premap_vector_rejected ||
		!validation.user_offset_succeeded ||
		!validation.user_vector_succeeded ?
		KSFT_FAIL : KSFT_PASS;
}

static void usage(const char *p)
{
	fprintf(stderr,
		"usage: %s [-l len] [-b blockdev] [-n ngdev] [-s scratch]\n"
		"       %s --bench -n /dev/ngXnY [benchmark options]\n"
		"  -b  block device for the DIO round-trip (default /dev/nvme0n1)\n"
		"  -n  /dev/ngXnY for the NVMe-passthru round-trip (optional)\n"
		"  -s  scratch file path (default ./iobuf-scratch)\n"
		"  -l  logical buffer length in bytes (default 4096)\n"
		"  -L  NVMe logical block size for the ng leg (default 512)\n"
		"Benchmark options:\n"
		"      --bench              sustained NVMe fixed-buffer read mode\n"
		"      --strict             require strict IOMMU page-size mapping\n"
		"      --slots N            registered/allocated slots (default 256)\n"
		"      --qd N               outstanding I/O depth (default 64)\n"
		"      --duration SEC       measured duration (default 30); with\n"
		"                           --iterations, the first limit wins\n"
		"      --iterations N       measured commands; with --duration, the\n"
		"                           first limit wins\n"
		"      --buffer-size BYTES  allocation per slot (default 2 MiB)\n"
		"      --io-size BYTES      bytes per command (default 512 KiB)\n"
		"      --warmup SEC         unmeasured warmup (default 5)\n"
		"      --json FILE          write machine-readable result ('-' stdout)\n",
		p, p);
}

int main(int argc, char **argv)
{
	enum {
		OPT_BENCH = 256,
		OPT_STRICT,
		OPT_SLOTS,
		OPT_QD,
		OPT_DURATION,
		OPT_ITERATIONS,
		OPT_BUFFER_SIZE,
		OPT_IO_SIZE,
		OPT_WARMUP,
		OPT_JSON,
	};
	static const struct option long_opts[] = {
		{ "blockdev", required_argument, NULL, 'b' },
		{ "ngdev", required_argument, NULL, 'n' },
		{ "scratch", required_argument, NULL, 's' },
		{ "length", required_argument, NULL, 'l' },
		{ "lba", required_argument, NULL, 'L' },
		{ "bench", no_argument, NULL, OPT_BENCH },
		{ "strict", no_argument, NULL, OPT_STRICT },
		{ "slots", required_argument, NULL, OPT_SLOTS },
		{ "qd", required_argument, NULL, OPT_QD },
		{ "duration", required_argument, NULL, OPT_DURATION },
		{ "iterations", required_argument, NULL, OPT_ITERATIONS },
		{ "buffer-size", required_argument, NULL, OPT_BUFFER_SIZE },
		{ "io-size", required_argument, NULL, OPT_IO_SIZE },
		{ "warmup", required_argument, NULL, OPT_WARMUP },
		{ "json", required_argument, NULL, OPT_JSON },
		{ "help", no_argument, NULL, 'h' },
		{ }
	};
	const char *blockdev = "/dev/nvme0n1";
	const char *ngdev = NULL;
	const char *scratch = "./iobuf-scratch";
	char derived_blockdev[PATH_MAX];
	struct bench_opts bench_opts = {
		.slots = BENCH_DEFAULT_SLOTS,
		.qd = BENCH_DEFAULT_QD,
		.duration = BENCH_DEFAULT_DURATION,
		.buffer_size = BENCH_DEFAULT_BUFFER_SIZE,
		.io_size = BENCH_DEFAULT_IO_SIZE,
		.warmup = BENCH_DEFAULT_WARMUP,
	};
	unsigned int len = POOL_LEN;
	unsigned int lba = 512;
	struct io_uring ring;
	struct io_uring_params p;
	unsigned char *ref;
	bool bench = false, bench_opt_seen = false, blockdev_set = false;
	bool duration_set = false, iterations_set = false;
	int fd, res, c, sqe128;

	while ((c = getopt_long(argc, argv, "b:n:s:l:L:h", long_opts,
			      NULL)) != -1) {
		switch (c) {
		case 'b':
			blockdev = optarg;
			blockdev_set = true;
			break;
		case 'n':
			ngdev = optarg;
			break;
		case 's':
			scratch = optarg;
			break;
		case 'l':
			if (parse_uint(optarg, &len))
				goto bad_option;
			break;
		case 'L':
			if (parse_uint(optarg, &lba))
				goto bad_option;
			break;
		case OPT_BENCH:
			bench = true;
			break;
		case OPT_STRICT:
			bench_opts.strict = true;
			bench_opt_seen = true;
			break;
		case OPT_SLOTS:
			if (parse_uint(optarg, &bench_opts.slots))
				goto bad_option;
			bench_opt_seen = true;
			break;
		case OPT_QD:
			if (parse_uint(optarg, &bench_opts.qd))
				goto bad_option;
			bench_opt_seen = true;
			break;
		case OPT_DURATION:
			if (parse_uint(optarg, &bench_opts.duration))
				goto bad_option;
			duration_set = true;
			bench_opt_seen = true;
			break;
		case OPT_ITERATIONS:
			if (parse_u64(optarg, &bench_opts.iterations))
				goto bad_option;
			iterations_set = true;
			bench_opt_seen = true;
			break;
		case OPT_BUFFER_SIZE:
			if (parse_uint(optarg, &bench_opts.buffer_size))
				goto bad_option;
			bench_opt_seen = true;
			break;
		case OPT_IO_SIZE:
			if (parse_uint(optarg, &bench_opts.io_size))
				goto bad_option;
			bench_opt_seen = true;
			break;
		case OPT_WARMUP:
			if (parse_uint(optarg, &bench_opts.warmup))
				goto bad_option;
			bench_opt_seen = true;
			break;
		case OPT_JSON:
			bench_opts.json = optarg;
			bench_opt_seen = true;
			break;
		case 'h':
			usage(argv[0]);
			return KSFT_PASS;
		default:
			usage(argv[0]);
			return KSFT_FAIL;
		}
	}
	goto options_parsed;

bad_option:
	fprintf(stderr, "invalid numeric option: %s\n", optarg);
	usage(argv[0]);
	return KSFT_FAIL;

options_parsed:
	if (optind != argc) {
		usage(argv[0]);
		return KSFT_FAIL;
	}
	if (bench_opt_seen && !bench) {
		fprintf(stderr, "benchmark options require --bench\n");
		return KSFT_FAIL;
	}
	if (iterations_set && !duration_set)
		bench_opts.duration = 0;
	if (bench) {
		if (!blockdev_set && ngdev && !strncmp(ngdev, "/dev/ng", 7)) {
			if (snprintf(derived_blockdev, sizeof(derived_blockdev),
				     "/dev/nvme%s", ngdev + 7) >=
			    (int)sizeof(derived_blockdev)) {
				fprintf(stderr, "derived block device path is too long\n");
				return KSFT_FAIL;
			}
			blockdev = derived_blockdev;
		}
		return run_benchmark(blockdev, ngdev, scratch, lba, &bench_opts);
	}

	ref = read_reference(blockdev, len);
	if (!ref) {
		fprintf(stderr, "cannot read reference block from %s: %s\n",
			blockdev, strerror(errno));
		return KSFT_SKIP;
	}

	/* --- Block device leg: 64-byte SQEs, DIO fill via READ_FIXED. --- */
	fd = open(blockdev, O_RDONLY | O_DIRECT);
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

	res = alloc_iobuf(&ring, fd, sqe128, 0, len, 0);
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
		int ngfd = open(ngdev, O_RDONLY);
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
		res = alloc_iobuf(&ring, ngfd, 1, 0, len, 0);
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
	free(ref);

	printf("\n# %d passed, %d failed, %d skipped\n", passes, fails, skips);
	if (fails)
		return KSFT_FAIL;
	if (passes == 0)
		return KSFT_SKIP;
	return KSFT_PASS;
}
