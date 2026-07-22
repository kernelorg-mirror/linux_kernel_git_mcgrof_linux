/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_LINUX_BLKDEV_H
#define _UAPI_LINUX_BLKDEV_H

#include <linux/ioctl.h>
#include <linux/types.h>

/*
 * io_uring block file commands, see IORING_OP_URING_CMD.
 * It's a different number space from ioctl(), reuse the block's code 0x12.
 */
#define BLOCK_URING_CMD_DISCARD			_IO(0x12, 0)

/*
 * Allocate one logical fixed buffer from the provider queue's iobuf pool and
 * install it into an existing sparse registered-buffer slot on the ring.
 *
 *   sqe->addr   = buf_index, the sparse fixed-buffer slot to fill
 *   sqe->addr3  = len, the logical buffer length in bytes
 *
 * The provider's pool determines the folio order; the buffer is bidirectional,
 * zeroed before it becomes visible, and allocated strictly from the pool (no
 * fallback). Remove it with the ordinary registered-buffer update/unregister
 * API. Errors: -EOPNOTSUPP (no pool attached), -ENOBUFS (pool exhausted),
 * -EBUSY (slot occupied), -EINVAL (bad length or index), -ENXIO (no buffer
 * table).
 */
#define BLOCK_URING_CMD_ALLOC_IOBUF		_IO(0x12, 1)

#endif
