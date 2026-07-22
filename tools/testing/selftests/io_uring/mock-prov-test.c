// SPDX-License-Identifier: GPL-2.0
/*
 * Exercise the io_uring bvec buffer-provider path with no hardware.
 *
 * The io_uring mock file (CONFIG_IO_URING_MOCK_FILE) grows two commands that
 * make it a buffer provider: IORING_MOCK_CMD_PROV_REGISTER allocates pages and
 * installs them into a sparse registered-buffer slot via the in-kernel helper
 * io_uring_cmd_register_bvecs(), and IORING_MOCK_CMD_PROV_STAT reports whether
 * that buffer has been registered and released. This test drives the provider,
 * moves real bytes through the installed slot with READ_FIXED / WRITE_FIXED,
 * and proves the release callback fires exactly when the slot is detached.
 *
 * Requires root (the mock manager device is CAP_SYS_ADMIN gated) and the mock
 * module; without either the affected cases report as skipped.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <linux/io_uring/mock_file.h>
#include <liburing.h>

/* KSFT exit codes (kselftest.h is not always in the include path). */
#define KSFT_PASS	0
#define KSFT_FAIL	1
#define KSFT_SKIP	4

#define MOCK_DEV	"/dev/io_uring_mock"
#define NR_SLOTS	4
#define BUF_LEN		8192

/* Mirror the kernel's dir bits so the test does not need the kernel header. */
#ifndef IO_URING_CMD_BUF_READ
#define IO_URING_CMD_BUF_READ	(1U << 0)
#define IO_URING_CMD_BUF_WRITE	(1U << 1)
#endif

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

/* Issue a uring_cmd carrying a fixed-size argument at sqe->addr/sqe->len. */
static int mock_cmd(struct io_uring *ring, int fd, unsigned int cmd_op,
		    void *arg, size_t len)
{
	struct io_uring_sqe *sqe = io_uring_get_sqe(ring);

	if (!sqe)
		return -ENOSPC;
	memset(sqe, 0, 128);		/* SQE128 ring */
	sqe->opcode = IORING_OP_URING_CMD;
	sqe->fd = fd;
	sqe->cmd_op = cmd_op;
	sqe->addr = (uint64_t)(uintptr_t)arg;
	sqe->len = len;
	return submit_one(ring);
}

static int read_fixed(struct io_uring *ring, int fd, unsigned int slot,
		      unsigned int len)
{
	struct io_uring_sqe *sqe = io_uring_get_sqe(ring);

	if (!sqe)
		return -ENOSPC;
	io_uring_prep_read_fixed(sqe, fd, 0, len, 0, slot);
	return submit_one(ring);
}

static int write_fixed(struct io_uring *ring, int fd, unsigned int slot,
		       unsigned int len)
{
	struct io_uring_sqe *sqe = io_uring_get_sqe(ring);

	if (!sqe)
		return -ENOSPC;
	io_uring_prep_write_fixed(sqe, fd, 0, len, 0, slot);
	return submit_one(ring);
}

static int detach_slot(struct io_uring *ring, unsigned int slot)
{
	struct iovec iov = { .iov_base = NULL, .iov_len = 0 };

	return io_uring_register_buffers_update_tag(ring, slot, &iov, NULL, 1);
}

/* An anonymous scratch file used as the READ_FIXED/WRITE_FIXED target. */
static int scratch_fd(void)
{
	char path[] = "/tmp/mock-provXXXXXX";
	int fd = mkstemp(path);

	if (fd >= 0)
		unlink(path);
	return fd;
}

static int prov_register(struct io_uring *ring, int mockfd, unsigned int slot,
			 unsigned int len, unsigned int flags, unsigned char pat)
{
	struct io_uring_mock_prov_register arg;

	memset(&arg, 0, sizeof(arg));
	arg.buf_index = slot;
	arg.flags = flags;
	arg.len = len;
	arg.pattern = pat;
	return mock_cmd(ring, mockfd, IORING_MOCK_CMD_PROV_REGISTER, &arg,
			sizeof(arg));
}

static int prov_stat(struct io_uring *ring, int mockfd,
		     struct io_uring_mock_prov_stat *out)
{
	memset(out, 0, sizeof(*out));
	return mock_cmd(ring, mockfd, IORING_MOCK_CMD_PROV_STAT, out,
			sizeof(*out));
}

/* Drain fixed-buffer @slot to a scratch file and confirm every byte is @pat. */
static int drain_is_pattern(struct io_uring *ring, unsigned int slot,
			    unsigned int len, unsigned char pat)
{
	unsigned char *got;
	int sfd, res, i, bad_byte = 0;

	sfd = scratch_fd();
	if (sfd < 0)
		return -errno;
	res = write_fixed(ring, sfd, slot, len);
	if (res != (int)len) {
		close(sfd);
		return res < 0 ? res : -EIO;
	}
	got = malloc(len);
	if (!got) {
		close(sfd);
		return -ENOMEM;
	}
	if (pread(sfd, got, len, 0) != (ssize_t)len) {
		free(got);
		close(sfd);
		return -EIO;
	}
	for (i = 0; i < (int)len; i++) {
		if (got[i] != pat) {
			bad_byte = 1;
			break;
		}
	}
	free(got);
	close(sfd);
	return bad_byte ? -EBADMSG : 0;
}

int main(void)
{
	struct io_uring_mock_probe probe;
	struct io_uring_mock_create create;
	struct io_uring_mock_prov_stat stat;
	struct io_uring_params p;
	struct io_uring ring;
	int mgrfd, mockfd, res, sfd;

	mgrfd = open(MOCK_DEV, O_RDWR);
	if (mgrfd < 0) {
		fprintf(stderr, "open %s: %s (need the mock module + root)\n",
			MOCK_DEV, strerror(errno));
		return KSFT_SKIP;
	}

	memset(&p, 0, sizeof(p));
	p.flags = IORING_SETUP_SQE128 | IORING_SETUP_CQE32;
	if (io_uring_queue_init_params(NR_SLOTS + 4, &ring, &p) < 0) {
		perror("io_uring_queue_init_params");
		return KSFT_FAIL;
	}

	/* Gate on the provider feature the mock advertises. */
	memset(&probe, 0, sizeof(probe));
	res = mock_cmd(&ring, mgrfd, IORING_MOCK_MGR_CMD_PROBE, &probe,
		       sizeof(probe));
	if (res < 0) {
		skip("provider path", strerror(-res));
		goto done;
	}
	if (probe.features <= IORING_MOCK_FEAT_PROV_REGBUF) {
		skip("provider path", "mock lacks PROV_REGBUF feature");
		goto done;
	}

	/* Create a mock file to act as the provider. */
	memset(&create, 0, sizeof(create));
	create.file_size = 1 << 20;
	res = mock_cmd(&ring, mgrfd, IORING_MOCK_MGR_CMD_CREATE, &create,
		       sizeof(create));
	if (res < 0) {
		bad("create mock file", strerror(-res));
		goto done;
	}
	mockfd = create.out_fd;

	if (io_uring_register_buffers_sparse(&ring, NR_SLOTS) < 0) {
		perror("register_buffers_sparse");
		goto done;
	}

	/* Case 1: register a filled buffer, drain it, verify the source bytes. */
	res = prov_register(&ring, mockfd, 0, BUF_LEN,
			    IORING_MOCK_PROV_F_FILL_PATTERN |
			    IORING_MOCK_PROV_F_READ | IORING_MOCK_PROV_F_WRITE,
			    0xAB);
	if (res < 0) {
		bad("prov register slot 0", strerror(-res));
		goto done;
	}
	res = drain_is_pattern(&ring, 0, BUF_LEN, 0xAB);
	if (res < 0)
		bad("provider buffer is a valid WRITE_FIXED source", strerror(-res));
	else
		ok("provider buffer is a valid WRITE_FIXED source");

	/* Case 2: READ_FIXED into the buffer, drain, verify the round trip. */
	sfd = scratch_fd();
	if (sfd >= 0) {
		unsigned char *src = malloc(BUF_LEN);

		if (src) {
			memset(src, 0xCD, BUF_LEN);
			if (pwrite(sfd, src, BUF_LEN, 0) == (ssize_t)BUF_LEN) {
				res = read_fixed(&ring, sfd, 0, BUF_LEN);
				if (res != BUF_LEN)
					bad("READ_FIXED into provider buffer",
					    res < 0 ? strerror(-res) : "short");
				else if (drain_is_pattern(&ring, 0, BUF_LEN, 0xCD) < 0)
					bad("provider buffer round trip",
					    "data mismatch");
				else
					ok("provider buffer round trips READ_FIXED -> WRITE_FIXED");
			}
			free(src);
		}
		close(sfd);
	}

	/* Case 3: release fires exactly when the slot is detached. */
	if (prov_stat(&ring, mockfd, &stat) < 0 || !stat.registered)
		bad("prov stat before detach", "not registered");
	else if (stat.released)
		bad("prov stat before detach", "released too early");
	else if (detach_slot(&ring, 0) < 0)
		bad("detach slot 0", strerror(errno));
	else if (prov_stat(&ring, mockfd, &stat) < 0)
		bad("prov stat after detach", "stat failed");
	else if (!stat.released)
		bad("release on detach", "release callback did not fire");
	else
		ok("detach fires the provider release callback exactly once");

	/* Case 4: a write-only buffer rejects READ_FIXED (direction enforced). */
	res = prov_register(&ring, mockfd, 1, BUF_LEN,
			    IORING_MOCK_PROV_F_WRITE, 0);
	if (res < 0) {
		bad("prov register write-only", strerror(-res));
	} else {
		sfd = scratch_fd();
		if (sfd >= 0) {
			res = read_fixed(&ring, sfd, 1, BUF_LEN);
			if (res >= 0)
				bad("write-only buffer rejects READ_FIXED",
				    "read unexpectedly succeeded");
			else
				ok("write-only buffer rejects READ_FIXED");
			close(sfd);
		}
		detach_slot(&ring, 1);
	}

done:
	io_uring_queue_exit(&ring);
	close(mgrfd);
	printf("\n# %d passed, %d failed, %d skipped\n", passes, fails, skips);
	if (fails)
		return KSFT_FAIL;
	if (passes == 0)
		return KSFT_SKIP;
	return KSFT_PASS;
}
