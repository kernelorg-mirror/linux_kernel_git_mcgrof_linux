.. SPDX-License-Identifier: GPL-2.0

====================================================
Premapped pool buffers and the NVMe command ceiling
====================================================

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

1. ``block, io_uring: premap a pool buffer to exceed the dma_opt clamp``.
   This is the generic infrastructure:

   * The retained mapping. At registration the pool reserves one
     contiguous IOVA for the whole buffer with ``dma_iova_try_alloc()``
     and links every folio into it with ``dma_iova_link()``, so the buffer
     becomes a single contiguous device address. ``struct blk_dma_premap``
     holds that ``dma_iova_state`` and the device it belongs to; it lives
     in the io_uring registration object. ``dma_iova_try_alloc()`` returns
     false when the device is not behind a translating IOMMU -- there is no
     per-I/O IOVA to avoid and no clamp to beat -- so registration then
     falls back to an ordinary dynamically mapped buffer.

   * The higher ceiling. ``queue_limits.max_hw_premapped_sectors`` is the
     transport's real maximum with the ``dma_opt_mapping_size()`` clamp
     removed but ``NVME_MAX_BYTES`` still applied. ``blk_rq_map_user_iov()``
     gains an opts form carrying an explicit ``max_bytes`` and a ``NO_COPY``
     flag; a premapped request is mapped against ``max_hw_premapped_sectors``
     and fails rather than silently bounce-buffering. Ordinary I/O keeps
     the ``dma_opt``-clamped ``max_hw_sectors``.

   * The DMA iterator. ``blk_rq_dma_map_iter_*()`` gains a premapped branch
     keyed on ``req->dma_premap`` that emits the retained contiguous IOVA
     as one segment rather than mapping per command; NVMe's PRP/SGL builders
     consume the iterator unchanged.

2. ``nvme: issue premapped commands past the dma_opt clamp``. ``nvme-pci``
   records the MDTS before the ``dma_opt_mapping_size()`` clamp and
   publishes it as ``max_hw_premapped_sectors``; the io_uring_cmd
   passthrough path resolves a fixed buffer to its ``blk_dma_premap``,
   points ``req->dma_premap`` at it, and skips the completion unmap.

3. ``nvme: do not premap a pool buffer allocated via the multipath head``.
   A persistent mapping is controller-specific, so only the fixed
   per-controller ``/dev/ngXnY`` path premaps; the floating multipath head
   allocates an ordinary buffer.

How the request reaches the mapping
===================================

``io_uring_cmd_kbuf_priv()`` resolves the command's fixed buffer to the
provider's ``release_data`` (the ``blk_iobuf_reg``);
``blk_iobuf_fixed_buf_premap()`` returns the retained ``blk_dma_premap``
when it is one of ours premapped to the device; and ``request.dma_premap``
carries it into the request. The whole chain is ``req->buf_index ->
imu->priv -> blk_iobuf_reg -> blk_dma_premap``.

Correctness spots that must all agree
=====================================

The premapped consumer is spread across the NVMe request path and several
spots must be correct together:

* **Set up the request.** In ``nvme_map_user_request()``, after importing
  the fixed buffer, resolve it with ``io_uring_cmd_kbuf_priv()`` +
  ``blk_iobuf_fixed_buf_premap(dma_dev)``; on a hit, point
  ``req->dma_premap`` at the retained mapping (which also splits the
  request at ``max_hw_premapped_sectors``).

* **Feed the DMA iterator from the mapping.** The premapped branch of
  ``blk_rq_dma_map_iter_start()`` emits the whole buffer as one contiguous
  IOVA segment from ``blk_dma_premap.state`` rather than mapping. For the
  whole-buffer case the segment starts at the mapping's base; an arbitrary
  buffer offset needs the byte-offset bookkeeping this first version can
  defer.

* **Do not unmap what you did not map (critical).** A premapped request
  must free no DMA at completion. ``nvme_pci_prp_save_mapping()`` saves no
  ``dma_vec`` for it, the single-segment fast path is bypassed, and
  ``nvme_unmap_data()`` skips the unmap while still freeing any PRP
  descriptor list. Getting any of these wrong frees a live mapping --
  corruption -- which is why this step is boot-validated, not asserted.

* **Coherency.** The buffer is mapped once and reused with no per-command
  DMA cache maintenance, which is correct only where the device does
  cache-coherent DMA. Registration establishes the mapping only for a
  cache-coherent device (``dev_is_dma_coherent()``) and otherwise falls
  back to an ordinary buffer; coherent DMA is also where premapping is
  worthwhile, since a non-coherent host would owe per-I/O cache maintenance
  that defeats it.

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
