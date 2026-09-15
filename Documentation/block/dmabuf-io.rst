.. SPDX-License-Identifier: GPL-2.0

=======================================
Map-once buffers for block I/O: dma-buf
=======================================

This document describes how a user space buffer can be mapped to a block
device once and then used for many reads and writes without a DMA mapping
per command, why that matters, and what the two ways of exceeding the
per-command mapping clamp trade off.

The problem
===========

Every ordinary direct I/O maps its data buffer for DMA when the request is
issued and unmaps it at completion.  On a host with a translating IOMMU the
mapping allocates an IOVA range per command, and the IOVA allocator keeps a
per-CPU range cache that only covers sizes up to ``iova_rcache_range()``.
NVMe therefore folds ``dma_opt_mapping_size()`` into ``max_hw_sectors`` so
that every command's IOVA allocation stays inside that cache.  On x86 that
pins every NVMe command at 128 KiB regardless of the device's MDTS.  A
2 MiB or 8 MiB transfer that the device could take as one command reaches
it as 16 or 64 commands.

The clamp is a cost of mapping per command.  Remove the per-command
mapping and the clamp has nothing to protect.

The mechanism: dma-buf backed bios
==================================

A dma-buf can be registered with io_uring as a fixed buffer of type
``IO_REGBUF_TYPE_DMABUF`` whose target is a block device file opened
``O_DIRECT``.  At registration the block device asks its driver to attach
to the dma-buf; NVMe attaches its PCI device and, on first use, maps the
buffer once.  The mapping stays for as long as the buffer is registered.

Each ``READ_FIXED`` or ``WRITE_FIXED`` against such a buffer produces a
dma-map backed bio (``REQ_DMABUF``): instead of a bvec array the bio
carries the device's map object and a byte offset into it, and the driver
builds the command's PRP or SGL directly from the retained mapping.  No
``dma_map``, no ``dma_unmap``, no IOVA allocation per command.

Because such a bio allocates no IOVA per command, it is bounded by its own
queue limit, ``max_hw_dmabuf_sectors``, rather than ``max_sectors``.  A
driver sets it to the device's true command ceiling; NVMe sets it to MDTS
with the transport's descriptor limit folded in.  It is exposed read-only
as ``/sys/block/<disk>/queue/max_hw_dmabuf_sectors_kb`` and is 0 when the
driver cannot attach a dma-buf, in which case dma-buf bios follow
``max_sectors`` like any other bio.

Where the buffer comes from
===========================

Any dma-buf exporter works.  Two host-memory exporters are relevant to
storage:

udmabuf
  ``/dev/udmabuf`` turns a memfd into a dma-buf.  With ``MFD_HUGETLB`` the
  memory is 2 MiB folios; without it, 4 KiB pages.  This is plain pinned
  user memory, and it is enough to get map-once
  behaviour for an application's own staging buffers.

the dma-buf system heap
  ``/dev/dma_heap/system`` allocates kernel pages and exports them
  directly.  Upstream it allocates in 1 MiB, 64 KiB and 4 KiB chunks; with
  the 2 MiB allocation order proposed for it, each chunk matches an IOMMU
  superpage, which makes the one-time map and unmap cheap and keeps IOTLB
  pressure low during I/O.  No memfd and no hugetlb reservation.

A GPU driver's dma-buf export of device memory is the third source and
the reason the path is shaped this way: the same registration, the same
bio type and the same driver path serve host and device memory alike.

What was measured
=================

QEMU NVMe controller with ``mdts=11`` (8 MiB) behind a translating IOMMU
(``intel_iommu=on iommu=nopt``), 512-byte logical blocks,
``max_hw_sectors_kb`` 128 and ``max_hw_dmabuf_sectors_kb`` 8192.  One
8 MiB ``WRITE_FIXED`` followed by one 8 MiB ``READ_FIXED``, counted at the
``nvme_setup_cmd`` tracepoint (``tools/testing/selftests/dmabuf-heaps/
dmabuf-block-io``):

============  ==================  ==================
buffer        write commands      read commands
============  ==================  ==================
bvec          64 x 128 KiB        64 x 128 KiB
udmabuf       1 x 8 MiB           1 x 8 MiB
udmabuf-huge  1 x 8 MiB           1 x 8 MiB
sysheap       1 x 8 MiB           1 x 8 MiB
============  ==================  ==================

All four read back exact.  A RAM-backed emulated controller says nothing
about a real drive's throughput; the measurement is the command geometry
the host issues, which is the same on real hardware.

Interfaces
==========

``max_hw_dmabuf_sectors``
  Queue limit set by the driver, validated to stay at or above
  ``max_sectors`` and at or below ``max_dev_sectors``, stacked with
  ``min_not_zero``.

``nvme.lift_dma_opt_clamp``
  Module parameter that raises ``max_hw_sectors`` itself to the true DMA
  ceiling for every command, dma-buf or not, so that ``max_sectors_kb``
  sizes the request.  It trades the IOVA range cache for command size on
  all I/O, which is the right trade on a host whose IOMMU is in
  passthrough or whose workload is dominated by large sequential I/O,
  and the wrong one for a mixed workload behind a translating IOMMU.
  The dma-buf path needs no such trade: it reaches MDTS for the buffers
  that were registered, and leaves every other command under the clamp.
