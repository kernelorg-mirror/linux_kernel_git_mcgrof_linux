#ifndef LINUX_IO_URING_MOCK_FILE_H
#define LINUX_IO_URING_MOCK_FILE_H

#include <linux/types.h>

enum {
	IORING_MOCK_FEAT_CMD_COPY,
	IORING_MOCK_FEAT_RW_ZERO,
	IORING_MOCK_FEAT_RW_NOWAIT,
	IORING_MOCK_FEAT_RW_ASYNC,
	IORING_MOCK_FEAT_POLL,
	IORING_MOCK_FEAT_PROV_REGBUF,

	IORING_MOCK_FEAT_END,
};

struct io_uring_mock_probe {
	__u64		features;
	__u64		__resv[9];
};

enum {
	IORING_MOCK_CREATE_F_SUPPORT_NOWAIT			= 1,
	IORING_MOCK_CREATE_F_POLL				= 2,
};

struct io_uring_mock_create {
	__u32		out_fd;
	__u32		flags;
	__u64		file_size;
	__u64		rw_delay_ns;
	__u64		__resv[13];
};

enum {
	IORING_MOCK_MGR_CMD_PROBE,
	IORING_MOCK_MGR_CMD_CREATE,
};

enum {
	IORING_MOCK_CMD_COPY_REGBUF,
	IORING_MOCK_CMD_PROV_REGISTER,
	IORING_MOCK_CMD_PROV_STAT,
};

enum {
	IORING_MOCK_COPY_FROM			= 1,
};

/*
 * Flags for IORING_MOCK_CMD_PROV_REGISTER. READ / WRITE choose the directions
 * the provider buffer may be used for (I/O destination / source); if neither
 * is set the buffer is registered bidirectional.
 */
enum {
	IORING_MOCK_PROV_F_FILL_PATTERN		= 1,
	IORING_MOCK_PROV_F_READ			= 2,
	IORING_MOCK_PROV_F_WRITE		= 4,
};

struct io_uring_mock_prov_register {
	__u32		buf_index;
	__u32		flags;
	__u64		len;
	__u32		pattern;
	__u32		__pad;
	__u64		__resv[4];
};

struct io_uring_mock_prov_stat {
	__u32		registered;
	__u32		released;
	__u64		len;
	__u64		__resv[6];
};

#endif
