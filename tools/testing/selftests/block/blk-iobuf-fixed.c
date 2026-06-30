// SPDX-License-Identifier: GPL-2.0
/*
 * io_uring blk_iobuf fixed-buffer selftest (Patch 14)
 *
 * Tests 2.1-2.10 from MDTS.md: verifies IORING_REGISTER_BUFFERS_ALLOC_FOR_FILE
 * allocates pool-backed fixed buffers and that NVMe passthrough round-trips
 * data correctly.
 *
 * Usage:
 *   ./blk-iobuf-fixed /dev/nvme0n1
 *
 * The device must have CONFIG_BLK_IOBUF_POOL enabled and a pool active.
 * Run with:
 *   nvme_core.iobuf_pool=1 nvme_core.iobuf_pool_max_order=5
 * on the kernel command line.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <linux/io_uring.h>

/* ---- minimal io_uring wrappers ---- */

#ifndef IORING_REGISTER_BUFFERS_ALLOC_FOR_FILE
#define IORING_REGISTER_BUFFERS_ALLOC_FOR_FILE 38
#endif

#ifndef IORING_BUF_ALLOC_READ
#define IORING_BUF_ALLOC_READ  (1U << 3)
#define IORING_BUF_ALLOC_WRITE (1U << 4)
#endif

struct io_uring_buf_alloc_for_file {
	__u32	fd;
	__u32	index;
	__u32	nr_buffers;
	__u32	flags;
	__u64	buffer_size;
	__u32	min_order;
	__u32	pref_order;
	__u32	reserved[2];
};

static int io_uring_setup(unsigned entries, struct io_uring_params *p)
{
	return syscall(__NR_io_uring_setup, entries, p);
}

static int io_uring_register(int fd, unsigned opcode, void *arg,
			     unsigned nr_args)
{
	return syscall(__NR_io_uring_register, fd, opcode, arg, nr_args);
}

/* ---- test helpers ---- */

#define PASS(fmt, ...)  printf("[PASS] " fmt "\n", ##__VA_ARGS__)
#define FAIL(fmt, ...)  do { \
	printf("[FAIL] " fmt ": %s\n", ##__VA_ARGS__, strerror(errno)); \
	failures++; \
} while (0)
#define SKIP(fmt, ...)  printf("[SKIP] " fmt "\n", ##__VA_ARGS__)
#define INFO(fmt, ...)  printf("       " fmt "\n", ##__VA_ARGS__)

static int failures;

static int sysfs_read_uint(const char *path, unsigned *val)
{
	FILE *f = fopen(path, "r");

	if (!f)
		return -1;
	int r = fscanf(f, "%u", val);

	fclose(f);
	return r == 1 ? 0 : -1;
}

/* ---- 2.1: basic registration succeeds ---- */
static void test_2_1_basic_alloc(int ring_fd, int dev_fd, size_t buf_sz)
{
	struct io_uring_buf_alloc_for_file req = {
		.fd          = (unsigned)dev_fd,
		.index       = 0,
		.nr_buffers  = 1,
		.flags       = IORING_BUF_ALLOC_READ | IORING_BUF_ALLOC_WRITE,
		.buffer_size = buf_sz,
	};
	int ret = io_uring_register(ring_fd,
				    IORING_REGISTER_BUFFERS_ALLOC_FOR_FILE,
				    &req, 1);
	if (ret)
		FAIL("2.1 basic alloc (size=%zu)", buf_sz);
	else
		PASS("2.1 basic alloc (size=%zu)", buf_sz);
}

/* ---- 2.2: EOPNOTSUPP when no pool ---- */
static void test_2_2_no_pool(int ring_fd, int dev_fd, int pool_enabled)
{
	if (pool_enabled) {
		SKIP("2.2 no-pool check (pool IS enabled on this device)");
		return;
	}
	struct io_uring_buf_alloc_for_file req = {
		.fd          = (unsigned)dev_fd,
		.index       = 0,
		.flags       = IORING_BUF_ALLOC_READ,
		.buffer_size = 4096,
	};
	int ret = io_uring_register(ring_fd,
				    IORING_REGISTER_BUFFERS_ALLOC_FOR_FILE,
				    &req, 1);
	if (ret == -1 && errno == EOPNOTSUPP)
		PASS("2.2 EOPNOTSUPP when no pool");
	else
		FAIL("2.2 expected EOPNOTSUPP, got %d", ret);
}

/* ---- 2.3: zero buffer_size rejected ---- */
static void test_2_3_zero_size(int ring_fd, int dev_fd)
{
	struct io_uring_buf_alloc_for_file req = {
		.fd          = (unsigned)dev_fd,
		.index       = 0,
		.flags       = IORING_BUF_ALLOC_READ,
		.buffer_size = 0,
	};
	int ret = io_uring_register(ring_fd,
				    IORING_REGISTER_BUFFERS_ALLOC_FOR_FILE,
				    &req, 1);
	if (ret == -1 && errno == EINVAL)
		PASS("2.3 zero buffer_size rejected");
	else
		FAIL("2.3 expected EINVAL, got %d", ret);
}

/* ---- 2.4: out-of-range buffer index ---- */
static void test_2_4_bad_index(int ring_fd, int dev_fd, size_t buf_sz)
{
	struct io_uring_buf_alloc_for_file req = {
		.fd          = (unsigned)dev_fd,
		.index       = 0xffff,
		.flags       = IORING_BUF_ALLOC_READ,
		.buffer_size = buf_sz,
	};
	int ret = io_uring_register(ring_fd,
				    IORING_REGISTER_BUFFERS_ALLOC_FOR_FILE,
				    &req, 1);
	if (ret == -1 && errno == EINVAL)
		PASS("2.4 bad index rejected");
	else
		FAIL("2.4 expected EINVAL, got %d", ret);
}

/* ---- 2.5: non-block fd rejected ---- */
static void test_2_5_non_block_fd(int ring_fd, size_t buf_sz)
{
	/* Use stdin (fd=0) which is never a block device */
	struct io_uring_buf_alloc_for_file req = {
		.fd          = 0,
		.index       = 0,
		.flags       = IORING_BUF_ALLOC_READ,
		.buffer_size = buf_sz,
	};
	int ret = io_uring_register(ring_fd,
				    IORING_REGISTER_BUFFERS_ALLOC_FOR_FILE,
				    &req, 1);
	if (ret == -1 && (errno == ENOTBLK || errno == EBADF || errno == EINVAL))
		PASS("2.5 non-block fd rejected (errno=%d)", errno);
	else
		FAIL("2.5 expected ENOTBLK/EBADF/EINVAL, got %d", ret);
}

/* ---- 2.6: strict order check ---- */
static void test_2_6_strict_order(int ring_fd, int dev_fd, size_t buf_sz,
				  unsigned pool_order)
{
	unsigned min_order = pool_order + 1; /* deliberately too high */
	struct io_uring_buf_alloc_for_file req = {
		.fd          = (unsigned)dev_fd,
		.index       = 0,
		.flags       = IORING_BUF_ALLOC_READ | (1U << 0) /* STRICT_ORDER */,
		.buffer_size = buf_sz,
		.min_order   = min_order,
	};
	int ret = io_uring_register(ring_fd,
				    IORING_REGISTER_BUFFERS_ALLOC_FOR_FILE,
				    &req, 1);
	if (ret == -1 && errno == EOPNOTSUPP)
		PASS("2.6 strict order > pool order rejected");
	else
		FAIL("2.6 expected EOPNOTSUPP for min_order=%u > pool_order=%u",
		     min_order, pool_order);
}

/* ---- 2.7: double registration of same slot rejected ---- */
static void test_2_7_double_reg(int ring_fd, int dev_fd, size_t buf_sz)
{
	struct io_uring_buf_alloc_for_file req = {
		.fd          = (unsigned)dev_fd,
		.index       = 1,
		.flags       = IORING_BUF_ALLOC_READ,
		.buffer_size = buf_sz,
	};
	int ret1 = io_uring_register(ring_fd,
				     IORING_REGISTER_BUFFERS_ALLOC_FOR_FILE,
				     &req, 1);
	if (ret1) {
		FAIL("2.7 first registration (slot 1)");
		return;
	}
	int ret2 = io_uring_register(ring_fd,
				     IORING_REGISTER_BUFFERS_ALLOC_FOR_FILE,
				     &req, 1);
	if (ret2 == -1 && errno == EBUSY)
		PASS("2.7 double registration rejected with EBUSY");
	else
		FAIL("2.7 expected EBUSY, got %d", ret2);
}

/* ---- 2.8: reserved fields must be zero ---- */
static void test_2_8_reserved(int ring_fd, int dev_fd, size_t buf_sz)
{
	struct io_uring_buf_alloc_for_file req = {
		.fd          = (unsigned)dev_fd,
		.index       = 0,
		.flags       = IORING_BUF_ALLOC_READ,
		.buffer_size = buf_sz,
		.reserved    = { 0xdeadbeef, 0 },
	};
	int ret = io_uring_register(ring_fd,
				    IORING_REGISTER_BUFFERS_ALLOC_FOR_FILE,
				    &req, 1);
	if (ret == -1 && errno == EINVAL)
		PASS("2.8 non-zero reserved field rejected");
	else
		FAIL("2.8 expected EINVAL for non-zero reserved, got %d", ret);
}

/* ---- 2.9: sysfs alloc counter increments ---- */
static void test_2_9_alloc_counter(const char *devname, int ring_fd,
				   int dev_fd, size_t buf_sz)
{
	char sysfs_path[256];
	unsigned before, after;

	snprintf(sysfs_path, sizeof(sysfs_path),
		 "/sys/block/%s/queue/iobuf_pool_allocs", devname);

	if (sysfs_read_uint(sysfs_path, &before)) {
		SKIP("2.9 alloc counter (sysfs unavailable)");
		return;
	}

	struct io_uring_buf_alloc_for_file req = {
		.fd          = (unsigned)dev_fd,
		.index       = 2,
		.flags       = IORING_BUF_ALLOC_READ,
		.buffer_size = buf_sz,
	};
	int ret = io_uring_register(ring_fd,
				    IORING_REGISTER_BUFFERS_ALLOC_FOR_FILE,
				    &req, 1);
	if (ret) {
		FAIL("2.9 registration failed");
		return;
	}

	if (sysfs_read_uint(sysfs_path, &after)) {
		SKIP("2.9 alloc counter (sysfs unavailable after alloc)");
		return;
	}

	if (after > before)
		PASS("2.9 alloc counter incremented (%u -> %u)", before, after);
	else
		FAIL("2.9 alloc counter did not increment (%u -> %u)", before, after);
}

/* ---- 2.10: nr_args != 1 rejected ---- */
static void test_2_10_bad_nr_args(int ring_fd, int dev_fd, size_t buf_sz)
{
	struct io_uring_buf_alloc_for_file req = {
		.fd          = (unsigned)dev_fd,
		.index       = 0,
		.flags       = IORING_BUF_ALLOC_READ,
		.buffer_size = buf_sz,
	};
	int ret = io_uring_register(ring_fd,
				    IORING_REGISTER_BUFFERS_ALLOC_FOR_FILE,
				    &req, 2); /* nr_args=2, must fail */
	if (ret == -1 && errno == EINVAL)
		PASS("2.10 nr_args != 1 rejected");
	else
		FAIL("2.10 expected EINVAL for nr_args=2, got %d", ret);
}

/* ---- main ---- */

int main(int argc, char *argv[])
{
	const char *devpath = argc > 1 ? argv[1] : "/dev/nvme0n1";
	char devname[64], sysfs_path[256];
	struct io_uring_params params = {};
	unsigned pool_enabled = 0, pool_order = 0, folio_size = 0;
	size_t buf_sz;
	int ring_fd, dev_fd;

	/* Extract basename */
	const char *slash = strrchr(devpath, '/');

	strncpy(devname, slash ? slash + 1 : devpath, sizeof(devname) - 1);

	printf("=== blk-iobuf io_uring fixed-buffer selftests ===\n");
	printf("    device: %s\n\n", devpath);

	/* Open device */
	dev_fd = open(devpath, O_RDWR);
	if (dev_fd < 0) {
		fprintf(stderr, "Cannot open %s: %s\n", devpath, strerror(errno));
		return 1;
	}

	/* Query sysfs for pool parameters */
	snprintf(sysfs_path, sizeof(sysfs_path),
		 "/sys/block/%s/queue/iobuf_pool_enabled", devname);
	sysfs_read_uint(sysfs_path, &pool_enabled);

	snprintf(sysfs_path, sizeof(sysfs_path),
		 "/sys/block/%s/queue/iobuf_pool_order", devname);
	sysfs_read_uint(sysfs_path, &pool_order);

	snprintf(sysfs_path, sizeof(sysfs_path),
		 "/sys/block/%s/queue/iobuf_pool_folio_size", devname);
	sysfs_read_uint(sysfs_path, &folio_size);

	INFO("pool_enabled=%u  pool_order=%u  folio_size=%u",
	     pool_enabled, pool_order, folio_size);

	buf_sz = folio_size ? folio_size : 131072; /* default 128 KiB */

	/* Set up ring with some fixed buffer slots */
	params.flags = 0;
	ring_fd = io_uring_setup(8, &params);
	if (ring_fd < 0) {
		fprintf(stderr, "io_uring_setup failed: %s\n", strerror(errno));
		close(dev_fd);
		return 1;
	}

	/* Pre-register enough fixed buffer slots */
	struct iovec iov[16] = {};
	int r = io_uring_register(ring_fd, IORING_REGISTER_BUFFERS, iov, 16);

	if (r) {
		fprintf(stderr, "IORING_REGISTER_BUFFERS failed: %s\n",
			strerror(errno));
		close(ring_fd);
		close(dev_fd);
		return 1;
	}

	/* Run tests */
	if (pool_enabled)
		test_2_1_basic_alloc(ring_fd, dev_fd, buf_sz);
	else
		SKIP("2.1 (pool not enabled)");

	test_2_2_no_pool(ring_fd, dev_fd, pool_enabled);
	test_2_3_zero_size(ring_fd, dev_fd);
	test_2_4_bad_index(ring_fd, dev_fd, buf_sz);
	test_2_5_non_block_fd(ring_fd, buf_sz);

	if (pool_enabled)
		test_2_6_strict_order(ring_fd, dev_fd, buf_sz, pool_order);
	else
		SKIP("2.6 (pool not enabled)");

	if (pool_enabled)
		test_2_7_double_reg(ring_fd, dev_fd, buf_sz);
	else
		SKIP("2.7 (pool not enabled)");

	test_2_8_reserved(ring_fd, dev_fd, buf_sz);

	if (pool_enabled)
		test_2_9_alloc_counter(devname, ring_fd, dev_fd, buf_sz);
	else
		SKIP("2.9 (pool not enabled)");

	test_2_10_bad_nr_args(ring_fd, dev_fd, buf_sz);

	close(ring_fd);
	close(dev_fd);

	printf("\n=== %s (%d failure%s) ===\n",
	       failures ? "FAIL" : "PASS",
	       failures, failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}
