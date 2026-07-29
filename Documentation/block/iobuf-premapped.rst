.. SPDX-License-Identifier: GPL-2.0

==================================================
Premapped pool buffers and the NVMe command ceiling
==================================================

Why the kernel NVMe path is capped at 128 KiB
=============================================

On a host with a translating IOMMU the per-command size the kernel NVMe
driver advertises is not the drive's MDTS. ``nvme-pci`` sets
``max_hw_sectors`` to ``min(NVME_MAX_BYTES, dma_opt_mapping_size())``, and
``dma_opt_mapping_size()`` returns the IOVA rcache range -- 128 KiB on
4 KiB pages. The reason is scalability, not hardware: every command
allocates and frees an IOVA, and allocating IOVAs larger than the rcache
ceiling falls into the RB-tree path that does not scale under
``iova_rbtree_lock`` on high-core-count hosts. So the clamp protects the
common, per-I/O-mapped path.

SPDK sidesteps the whole thing. Its driver runs in userspace over VFIO,
DMA-maps its buffer pool **once** at startup, and submits commands by
offset into that persistent mapping. No IOVA is allocated per command,
so nothing forces the 128 KiB ceiling, and on the same drive SPDK issues
the full 512 KiB MDTS. Measured on a Micron 7450 (MDTS 512 KiB) behind
an AMD IOMMU: kernel path 128 KiB, SPDK path 512 KiB, both at ~3.56 GB/s
(the drive is bandwidth-bound at 128 KiB, but the asymmetry is real for
IOPS-bound and CPU-bound workloads and for the per-command overhead).

The two ceilings the kernel conflates
=====================================

``max_hw_sectors`` today answers two different questions with one number:

* the largest command a **dynamically mapped** buffer may use -- correctly
  128 KiB, because it allocates an IOVA per I/O; and
* the largest command the **hardware** can accept -- the MDTS, 512 KiB.

A buffer that is DMA-mapped once and kept mapped belongs to the second
category but is charged the first. The kernel analogue of SPDK's model is
to give such a buffer a persistent mapping and split it at a separate,
higher ceiling.

The pieces in this series
=========================

1. ``block: iobuf: retain a persistent DMA mapping for pool-backed
   buffers``. ``struct blk_iobuf_reg`` gains an ``sg_table`` and a
   ``dma_dev``; when a pool-backed fixed buffer is allocated with a
   non-NULL ``dma_dev`` its folios are ``dma_map_sgtable()``-mapped once
   and retained until release. This is the "map once" substrate.

2. ``block: split premapped I/O at a separate, higher command ceiling``.
   A ``BIO_PREMAPPED`` flag and ``queue_limits.max_premapped_sectors``;
   ``get_max_io_size()`` splits a premapped bio at that ceiling instead of
   ``max_sectors``. Ordinary I/O is unchanged.

3. ``nvme: publish the un-clamped MDTS as the premapped command
   ceiling``. ``nvme-pci`` records the MDTS before the
   ``dma_opt_mapping_size()`` clamp and publishes it as
   ``max_premapped_sectors``.

The plumbing that reaches the mapping
=====================================

``block, io_uring: plumbing to reach a premapped buffer's DMA mapping``
adds the accessors the consumer needs, still without a consumer:
``io_uring_cmd_kbuf_priv()`` resolves the command's fixed buffer to the
provider's ``release_data`` (the ``blk_iobuf_reg``);
``blk_iobuf_fixed_buf_sgt()`` turns that into the retained ``sg_table``
when it is one of ours premapped to the device; and
``request.premap_sgt`` carries it into the request. The whole chain is
``req->buf_index -> imu->priv -> blk_iobuf_reg -> sg_table``.

What remains: the NVMe premapped-PRP consumer
=============================================

The patches above are the substrate and its plumbing; they change no I/O
on their own, because nothing yet flags a bio ``BIO_PREMAPPED`` or reuses
the retained mapping. The consumer that closes the loop is in the NVMe
request path, and it is the piece that must be validated on hardware
because several spots must all be correct together:

* **Set up the request.** In ``nvme_map_user_request()``, after importing
  the fixed buffer, call ``io_uring_cmd_kbuf_priv()`` +
  ``blk_iobuf_fixed_buf_sgt(dma_dev)``; on a hit, set ``req->premap_sgt``
  and flag the bio ``BIO_PREMAPPED`` (so it splits at
  ``max_premapped_sectors``).

* **Feed the DMA iterator from the mapping.** Add a premapped branch to
  ``blk_rq_dma_map_iter_start()``/``_next()`` keyed on
  ``req->premap_sgt`` and ``BIO_PREMAPPED``: emit ``blk_dma_iter``
  segments from the retained ``sg_dma_address()``/``sg_dma_len()`` rather
  than mapping. NVMe's PRP/SGL builders consume the iterator unchanged.
  For the whole-buffer case the walk starts at the first sg entry; an
  arbitrary buffer offset needs the byte-offset bookkeeping this first
  version can defer.

* **Do not unmap what you did not map (critical).** A premapped request
  must free no DMA at completion. ``nvme_pci_prp_save_mapping()`` must
  save no ``dma_vec`` for it, the single-segment fast path
  (``nvme_pci_setup_data_simple()``, which ``dma_map_page()``s directly)
  must be bypassed, and ``nvme_unmap_data()`` must skip the unmap while
  still freeing any PRP descriptor list. Getting any of these wrong frees
  a live mapping -- corruption -- which is why this step is
  boot-validated, not asserted.

* **Scope it.** Single-path ``/dev/ngXnY`` only; a persistent mapping is
  controller-specific, so reject a multipath-head registration (or keep
  one mapping per controller and remap on failover). mmap for CPU
  consumption and dma-buf export for GPU consumption are separate
  follow-ups.

Validation plan
===============

The point is a four-arm comparison on one drive whose MDTS exceeds
128 KiB, behind a translating IOMMU:

1. kernel, dynamically mapped, stock -- 128 KiB;
2. kernel, dynamically mapped, IOVA rcache raised -- 512 KiB, but the
   per-I/O IOVA allocation (and its contention) is back;
3. kernel, **premapped pool buffer** -- 512 KiB with no per-I/O IOVA
   allocation and full IOMMU translation retained;
4. SPDK/VFIO -- 512 KiB, the userspace reference.

Arm 3 is the goal: SPDK's command size and isolation from inside the
kernel driver. Measure not just bandwidth (the reference drive is
bandwidth-bound at 128 KiB) but CPU cycles per GiB, system CPU, achieved
command rate, and ``iova_rbtree_lock`` contention, which is where arm 3
should separate cleanly from arm 2.
