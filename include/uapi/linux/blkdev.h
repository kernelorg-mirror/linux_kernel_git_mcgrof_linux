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
 *   sqe->addr  = buf_index, the sparse fixed-buffer slot to fill
 *   sqe->addr3 = len, the logical buffer length in bytes
 *   sqe->len   = BLOCK_URING_CMD_ALLOC_IOBUF_F_* flags
 *
 * The provider's pool determines the folio order; the buffer is bidirectional,
 * zeroed before it becomes visible, and allocated strictly from the pool (no
 * fallback). Remove it with the ordinary registered-buffer update/unregister
 * API. Errors include -EOPNOTSUPP (no pool attached or a strict premap is not
 * available), -ENOBUFS (pool exhausted), -EBUSY (slot occupied), and -EINVAL
 * (bad flags, length, index, or no registered-buffer table).  Strict mode may
 * also return the underlying DMA-IOMMU allocation, alignment, mapping, or sync
 * error instead of registering a dynamically mapped fallback buffer.
 */
#define BLOCK_URING_CMD_ALLOC_IOBUF		_IO(0x12, 1)

/* Fail registration unless every premap leaf is at least the pool folio size. */
#define BLOCK_URING_CMD_ALLOC_IOBUF_F_STRICT_PGSIZE	(1U << 0)

#endif
