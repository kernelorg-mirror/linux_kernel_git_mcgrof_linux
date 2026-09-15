// SPDX-License-Identifier: GPL-2.0
/*
 * dma-buf backed fixed buffers on a block device, end to end, with no
 * liburing dependency (raw io_uring syscalls).
 *
 * Two buffer sources, both host memory, both registered with io_uring as
 * IO_REGBUF_TYPE_DMABUF buffers whose target is the block device opened
 * O_DIRECT, so every READ_FIXED / WRITE_FIXED against them becomes a
 * dma-map backed bio the driver issues from the mapping made at
 * registration:
 *
 *   udmabuf a memfd (optionally MFD_HUGETLB) turned into a dma-buf by
 *           /dev/udmabuf, i.e. plain pinned user memory with no pool.
 *   sysheap /dev/dma_heap/system: the dma-buf system heap's own
 *           allocation (1 MiB, 64 KiB, 4 KiB chunks upstream; 2 MiB with
 *           Davidlohr Bueso's "2MB system heap chunks" series).
 *
 * The check is data-exact: fill the source dma-buf through mmap() with a
 * pattern, WRITE_FIXED it to the device, READ_FIXED the same range into a
 * second dma-buf, compare through mmap(), then pread() the range with an
 * ordinary O_DIRECT read into anonymous memory and compare again.
 *
 * Command sizes are the point: run with the nvme_setup_cmd tracepoint
 * enabled and count how many commands, and of what length, each 8 MiB
 * transfer became.  Behind a translating IOMMU an ordinary bvec buffer
 * splits at max_hw_sectors_kb (128 KiB); a dma-buf buffer splits at
 * max_hw_dmabuf_sectors_kb (MDTS, 8 MiB on a QEMU mdts=11 controller).
 * Measured on that rig with intel_iommu=on iommu=nopt: bvec 64 x 128 KiB
 * per direction, udmabuf / udmabuf-huge / sysheap 1 x 8 MiB per direction.
 *
 * Usage: dmabuf-block-io <bdev> <udmabuf|udmabuf-huge|sysheap|bvec>
 *        [size]
 *        [offset]   size defaults to 8 MiB, offset to 1 GiB into the device.
 *        The range is overwritten: point it at a scratch namespace.
 * Exit: 0 pass, 1 fail, 4 skip (feature unavailable).
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <linux/dma-heap.h>
#include <linux/io_uring.h>
#include <linux/memfd.h>
#include <linux/udmabuf.h>

#define KSFT_PASS 0
#define KSFT_FAIL 1
#define KSFT_SKIP 4

/*
 * Private copies of the new ABI pieces, so the program builds against
 * either old or new system headers: the extended buffer-update descriptor
 * (io_uring rw-dmabuf series, where the update structure's resv field
 * became flags).
 */
struct t_rsrc_update2 {
	__u32 offset;
	__u32 flags;
	__u64 data;
	__u64 tags;
	__u32 nr;
	__u32 resv2;
};
#define T_RSRC_UPDATE_EXTENDED	(1U << 1)
#define T_REGBUF_TYPE_DMABUF	2
struct t_regbuf_desc {
	__u32 type;
	__u32 flags;
	__u64 size;
	__u64 uaddr;
	__s32 dmabuf_fd;
	__s32 target_fd;
	__u64 __resv[6];
};

/* ---- minimal io_uring ---- */
struct ring {
	int fd;
	unsigned *sq_head, *sq_tail, *sq_mask, *sq_array, *sq_flags;
	unsigned *cq_head, *cq_tail, *cq_mask;
	struct io_uring_sqe *sqes;
	struct io_uring_cqe *cqes;
	void *sq_ptr, *cq_ptr;
	size_t sq_len, cq_len;
};

static int io_uring_setup(unsigned entries, struct io_uring_params *p)
{
	return syscall(__NR_io_uring_setup, entries, p);
}
static int io_uring_enter(int fd, unsigned to_submit, unsigned min_complete,
			  unsigned flags)
{
	return syscall(__NR_io_uring_enter, fd, to_submit, min_complete, flags,
		       NULL, 0);
}
static int io_uring_register(int fd, unsigned op, void *arg, unsigned nr)
{
	return syscall(__NR_io_uring_register, fd, op, arg, nr);
}

static int ring_init(struct ring *r, unsigned entries)
{
	struct io_uring_params p;

	memset(&p, 0, sizeof(p));
	r->fd = io_uring_setup(entries, &p);
	if (r->fd < 0)
		return -errno;
	r->sq_len = p.sq_off.array + p.sq_entries * sizeof(unsigned);
	r->cq_len = p.cq_off.cqes + p.cq_entries * sizeof(struct io_uring_cqe);
	r->sq_ptr = mmap(NULL, r->sq_len, PROT_READ | PROT_WRITE,
			 MAP_SHARED | MAP_POPULATE, r->fd, IORING_OFF_SQ_RING);
	r->cq_ptr = mmap(NULL, r->cq_len, PROT_READ | PROT_WRITE,
			 MAP_SHARED | MAP_POPULATE, r->fd, IORING_OFF_CQ_RING);
	r->sqes = mmap(NULL, p.sq_entries * sizeof(struct io_uring_sqe),
		       PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, r->fd,
		       IORING_OFF_SQES);
	if (r->sq_ptr == MAP_FAILED || r->cq_ptr == MAP_FAILED ||
	    r->sqes == MAP_FAILED)
		return -ENOMEM;
	r->sq_head = r->sq_ptr + p.sq_off.head;
	r->sq_tail = r->sq_ptr + p.sq_off.tail;
	r->sq_mask = r->sq_ptr + p.sq_off.ring_mask;
	r->sq_array = r->sq_ptr + p.sq_off.array;
	r->sq_flags = r->sq_ptr + p.sq_off.flags;
	r->cq_head = r->cq_ptr + p.cq_off.head;
	r->cq_tail = r->cq_ptr + p.cq_off.tail;
	r->cq_mask = r->cq_ptr + p.cq_off.ring_mask;
	r->cqes = r->cq_ptr + p.cq_off.cqes;
	return 0;
}

/* One fixed read/write, synchronous: returns the CQE result. */
static int ring_rw_fixed(struct ring *r, int op, int fd, unsigned buf_index,
			 uint64_t buf_addr, unsigned len, uint64_t off)
{
	unsigned tail = *r->sq_tail, idx = tail & *r->sq_mask;
	struct io_uring_sqe *sqe = &r->sqes[idx];
	struct io_uring_cqe *cqe;
	int ret;

	memset(sqe, 0, sizeof(*sqe));
	sqe->opcode = op;
	sqe->fd = fd;
	sqe->addr = buf_addr;
	sqe->len = len;
	sqe->off = off;
	sqe->buf_index = buf_index;
	sqe->user_data = 1;
	r->sq_array[idx] = idx;
	atomic_store_explicit((_Atomic unsigned *)r->sq_tail, tail + 1,
			      memory_order_release);
	ret = io_uring_enter(r->fd, 1, 1, IORING_ENTER_GETEVENTS);
	if (ret < 0)
		return -errno;
	while (atomic_load_explicit((_Atomic unsigned *)r->cq_tail,
				    memory_order_acquire) == *r->cq_head)
		io_uring_enter(r->fd, 0, 1, IORING_ENTER_GETEVENTS);
	cqe = &r->cqes[*r->cq_head & *r->cq_mask];
	ret = cqe->res;
	atomic_store_explicit((_Atomic unsigned *)r->cq_head, *r->cq_head + 1,
			      memory_order_release);
	return ret;
}

static int register_dmabuf(struct ring *r, unsigned slot, int dmabuf_fd,
			   int target_fd)
{
	struct t_regbuf_desc desc;
	struct t_rsrc_update2 up;

	memset(&desc, 0, sizeof(desc));
	desc.type = T_REGBUF_TYPE_DMABUF;
	desc.dmabuf_fd = dmabuf_fd;
	desc.target_fd = target_fd;
	memset(&up, 0, sizeof(up));
	up.offset = slot;
	up.flags = T_RSRC_UPDATE_EXTENDED;
	up.data = (uint64_t)(uintptr_t)&desc;
	up.nr = 1;
	if (io_uring_register(r->fd, IORING_REGISTER_BUFFERS_UPDATE, &up,
			      sizeof(up)) < 0)
		return -errno;
	return 0;
}

static int register_uaddr(struct ring *r, unsigned slot, void *p, size_t len)
{
	struct iovec iov = { .iov_base = p, .iov_len = len };
	struct t_rsrc_update2 up;

	memset(&up, 0, sizeof(up));
	up.offset = slot;
	up.data = (uint64_t)(uintptr_t)&iov;
	up.nr = 1;
	if (io_uring_register(r->fd, IORING_REGISTER_BUFFERS_UPDATE, &up,
			      sizeof(up)) < 0)
		return -errno;
	return 0;
}

/* ---- buffer sources ---- */
static int udmabuf_dmabuf(size_t size, int huge)
{
	struct udmabuf_create c;
	unsigned flags = MFD_ALLOW_SEALING;
	int memfd, dev, fd, ret;

	if (huge)
		flags |= MFD_HUGETLB | MFD_HUGE_2MB;
	memfd = memfd_create("kvbuf", flags);
	if (memfd < 0)
		return -errno;
	if (ftruncate(memfd, size) < 0) {
		ret = -errno;
		close(memfd);
		return ret;
	}
	/* udmabuf requires the size seal (F_SEAL_SHRINK) */
	if (fcntl(memfd, F_ADD_SEALS, F_SEAL_SHRINK) < 0) {
		ret = -errno;
		close(memfd);
		return ret;
	}
	dev = open("/dev/udmabuf", O_RDWR | O_CLOEXEC);
	if (dev < 0) {
		ret = -errno;
		close(memfd);
		return ret;
	}
	memset(&c, 0, sizeof(c));
	c.memfd = memfd;
	c.flags = UDMABUF_FLAGS_CLOEXEC;
	c.size = size;
	fd = ioctl(dev, UDMABUF_CREATE, &c);
	ret = fd < 0 ? -errno : fd;
	close(dev);
	close(memfd);
	return ret;
}

static int sysheap_dmabuf(size_t size)
{
	struct dma_heap_allocation_data a;
	int dev, ret;

	dev = open("/dev/dma_heap/system", O_RDONLY | O_CLOEXEC);
	if (dev < 0)
		return -errno;
	memset(&a, 0, sizeof(a));
	a.len = size;
	a.fd_flags = O_RDWR | O_CLOEXEC;
	ret = ioctl(dev, DMA_HEAP_IOCTL_ALLOC, &a);
	ret = ret < 0 ? -errno : (int)a.fd;
	close(dev);
	return ret;
}

static void fill(uint8_t *p, size_t n, uint32_t seed)
{
	uint32_t x = seed;
	size_t i;

	for (i = 0; i < n; i += 4) {
		x = x * 1664525u + 1013904223u;
		memcpy(p + i, &x, 4);
	}
}

static size_t first_mismatch(const uint8_t *a, const uint8_t *b, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++)
		if (a[i] != b[i])
			return i;
	return n;
}

int main(int argc, char **argv)
{
	const char *dev, *mode;
	size_t size = 8u << 20, map_size;
	uint64_t off = 1ull << 30;
	int bdev, src_fd = -1, dst_fd = -1, ret, rc = KSFT_FAIL;
	uint8_t *src, *dst, *plain;
	struct ring r;
	int is_dmabuf;

	if (argc < 3) {
		fprintf(stderr, "usage: %s <bdev> <udmabuf|udmabuf-huge|sysheap|bvec> [size] [offset]\n",
			argv[0]);
		return KSFT_FAIL;
	}
	dev = argv[1];
	mode = argv[2];
	if (argc > 3)
		size = strtoull(argv[3], NULL, 0);
	if (argc > 4)
		off = strtoull(argv[4], NULL, 0);
	is_dmabuf = strcmp(mode, "bvec") != 0;

	bdev = open(dev, O_RDWR | O_DIRECT | O_CLOEXEC);
	if (bdev < 0) {
		perror("open bdev");
		return KSFT_FAIL;
	}
	ret = ring_init(&r, 8);
	if (ret) {
		fprintf(stderr, "io_uring_setup: %s\n", strerror(-ret));
		return KSFT_FAIL;
	}
	/* two sparse slots: 0 = source, 1 = destination */
	{
		struct io_uring_rsrc_register reg;

		memset(&reg, 0, sizeof(reg));
		reg.nr = 2;
		reg.flags = IORING_RSRC_REGISTER_SPARSE;
		if (io_uring_register(r.fd, IORING_REGISTER_BUFFERS2, &reg,
				      sizeof(reg)) < 0) {
			perror("register sparse buffers");
			return KSFT_FAIL;
		}
	}

	map_size = size;
	if (!strncmp(mode, "udmabuf", 7)) {
		int huge = !strcmp(mode, "udmabuf-huge");

		src_fd = udmabuf_dmabuf(size, huge);
		dst_fd = udmabuf_dmabuf(size, huge);
		if (src_fd < 0 || dst_fd < 0) {
			ret = src_fd < 0 ? src_fd : dst_fd;
			fprintf(stderr, "udmabuf: %s\n", strerror(-ret));
			return ret == -ENOENT || ret == -ENODEV ? KSFT_SKIP :
				KSFT_FAIL;
		}
	} else if (!strcmp(mode, "sysheap")) {
		src_fd = sysheap_dmabuf(size);
		dst_fd = sysheap_dmabuf(size);
		if (src_fd < 0 || dst_fd < 0) {
			ret = src_fd < 0 ? src_fd : dst_fd;
			fprintf(stderr, "dma_heap system: %s\n", strerror(-ret));
			return ret == -ENOENT ? KSFT_SKIP : KSFT_FAIL;
		}
	} else if (strcmp(mode, "bvec")) {
		fprintf(stderr, "unknown mode %s\n", mode);
		return KSFT_FAIL;
	}

	if (is_dmabuf) {
		src = mmap(NULL, map_size, PROT_READ | PROT_WRITE, MAP_SHARED,
			   src_fd, 0);
		dst = mmap(NULL, map_size, PROT_READ | PROT_WRITE, MAP_SHARED,
			   dst_fd, 0);
		if (src == MAP_FAILED || dst == MAP_FAILED) {
			perror("mmap dma-buf");
			return KSFT_FAIL;
		}
		ret = register_dmabuf(&r, 0, src_fd, bdev);
		if (!ret)
			ret = register_dmabuf(&r, 1, dst_fd, bdev);
		if (ret) {
			fprintf(stderr, "register dma-buf: %s\n",
				strerror(-ret));
			return ret == -EOPNOTSUPP || ret == -EINVAL ?
				KSFT_SKIP : KSFT_FAIL;
		}
	} else {
		src = aligned_alloc(2u << 20, size);
		dst = aligned_alloc(2u << 20, size);
		madvise(src, size, MADV_HUGEPAGE);
		madvise(dst, size, MADV_HUGEPAGE);
		ret = register_uaddr(&r, 0, src, size);
		if (!ret)
			ret = register_uaddr(&r, 1, dst, size);
		if (ret) {
			fprintf(stderr, "register buffers: %s\n",
				strerror(-ret));
			return KSFT_FAIL;
		}
	}

	fill(src, size, 0xC0FFEE ^ (uint32_t)off);
	memset(dst, 0, size);

	/*
	 * For a dma-buf fixed buffer sqe->addr is the byte offset into the
	 * buffer (there is no user address); for a bvec one it is the user
	 * address inside the registered range.
	 */
	ret = ring_rw_fixed(&r, IORING_OP_WRITE_FIXED, bdev, 0,
			    is_dmabuf ? 0 : (uint64_t)(uintptr_t)src, size, off);
	if (ret != (int)size) {
		fprintf(stderr, "WRITE_FIXED: %d (%s)\n", ret,
			ret < 0 ? strerror(-ret) : "short");
		goto out;
	}
	ret = ring_rw_fixed(&r, IORING_OP_READ_FIXED, bdev, 1,
			    is_dmabuf ? 0 : (uint64_t)(uintptr_t)dst, size, off);
	if (ret != (int)size) {
		fprintf(stderr, "READ_FIXED: %d (%s)\n", ret,
			ret < 0 ? strerror(-ret) : "short");
		goto out;
	}
	{
		size_t m = first_mismatch(src, dst, size);

		if (m != size) {
			fprintf(stderr, "dma-buf readback mismatch at byte %zu\n", m);
			goto out;
		}
	}
	/* independent witness: an ordinary O_DIRECT read of the same range */
	plain = mmap(NULL, size, PROT_READ | PROT_WRITE,
		     MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
	if (plain == MAP_FAILED) {
		perror("mmap witness");
		goto out;
	}
	if (pread(bdev, plain, size, off) != (ssize_t)size) {
		perror("pread witness");
		goto out;
	}
	{
		size_t m = first_mismatch(src, plain, size);

		if (m != size) {
			fprintf(stderr, "plain pread mismatch at byte %zu\n", m);
			goto out;
		}
	}
	printf("ok - %s %s: %zu bytes written and read back exact at offset %llu\n",
	       dev, mode, size, (unsigned long long)off);
	rc = KSFT_PASS;
out:
	return rc;
}
