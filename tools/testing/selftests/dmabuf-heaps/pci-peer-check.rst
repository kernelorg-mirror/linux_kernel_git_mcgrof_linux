.. SPDX-License-Identifier: GPL-2.0

PCI peer policy diagnostic
==========================

``pci-peer-check.py`` predicts the topology gates relevant to a GPU DMA-BUF
exporter and an NVMe importer. It accepts exact PCI addresses; it never chooses
GPU index zero or substitutes another controller. It uses Python 3's standard
library. Keep ``pci-peer-policy.json`` beside the script.

Examples::

  ./pci-peer-check.py --gpu 0000:01:00.0 --nvme 0000:02:00.0
  sudo ./pci-peer-check.py --gpu 01:00.0 --nvme 02:00.0 --json
  ./pci-peer-check.py --gpu 01:00.0 --nvme 02:00.0 --require nvidia
  python3 pci-peer-check-test.py

Four-digit domains, zero-padded eight-digit domains from nvidia-smi, and omitted
domain (0000) are accepted. The GPU must have display/3D class 03xxxx; the NVMe
controller must have class 010802. Privilege may be needed to read extended PCI
configuration space. A short read is unknown ACS state, never "ACS disabled".

Default exit status is zero for a completed diagnostic, including rejection.
Invalid input, missing device, or missing policy file returns 2. ``--require``
selects a policy to enforce: eligible=0, rejected=1, unknown/not-applicable=3.

What is checked
---------------

The Linux model follows ``calc_map_type_and_dist()`` in drivers/pci/p2pdma.c at
the kernel revision recorded in the JSON profile. It checks shared PCI ancestry,
ACS request/completion/egress redirection, the x86 AMD family >= 0x17 allowance,
and the host bridge allowlist including the same-host restrictions. A distance
is a hop count, not an eligibility threshold. A host-bridge route does not itself
mean a copy through RAM: a PCI transaction can traverse the root complex without
landing in host memory. An AMD DMA-BUF exporter may clear peer2peer after a
negative distance result and then migrate VRAM to GTT; the diagnostic reports
this consequence without attaching a buffer or observing its placement.

The NVIDIA model is pinned to the stock open driver 615.71.09, commit
61dcc93722ecb418bb5f2e00923f05b4b8051dd1. Source paths and SHA256 digests are in
the profile. All 45 chipset rules are included in first-match order; platform
denials precede generic allowances. NVIDIA's system chipset identity is queried
through NV0000_CTRL_CMD_SYSTEM_GET_CHIPSET_INFO. In a tested AMD configuration,
the host bridge is Raphael 1022:14d8, while RM selects the X370 chipset family
using 1022:790e. Guessing the chipset from the CPU host bridge gives a wrong
answer. The returned chipset flags concern BAR behavior, not peer capability;
they are deliberately not treated as a peer flag.

``nv_dma_buf_attach()`` calls ``nv_grdma_pci_topology_supported()`` for the normal
mapping type. ``_checkP2pChipsetSupport()`` marks a chipset unsupported when either
peer reads or writes is unavailable. On a normal noncoherent bare-metal system,
an unsupported chipset then needs a shared upstream PCI bridge. This includes
a shared root port: a shared PCI host bridge alone is insufficient. Unlike the
Linux model, that NVIDIA fallback function does not inspect ACS bits. Failure
returns ENOTSUPP from attachment; it does not implement AMD's VRAM-to-GTT fallback.

The metadata ioctl is restricted to x86_64 and the exact profiled driver version,
with a strict version handshake and checked C structure sizes. It creates and
frees an ephemeral RM root client. It creates no device, memory, or CUDA context,
imports no foreign handle, and issues no storage command. ``--no-nvidia-query``
skips it. If chipset identity cannot be obtained, an otherwise unsupported
pairing is unknown rather than guessed from the host bridge.

Limits
------

These are source-based topology predictions, not observations of the live
attachment flag or a guarantee of DMA-BUF export, successful DMA mapping,
physical PCI forwarding, absence of staging, or byte-correct storage I/O.
JSON always contains ``transfer_tested: false``. Linux's running build option is
read from /boot/config-<release> or /proc/config.gz; a disabled option rejects,
and an unavailable option makes otherwise eligible topology unknown.

The usual first-root-bus-device 00.0 identity is modeled. Platforms whose kernel
allowlist uses another first enumerated root-port device remain unknown. PCI
enumeration order, kernel capability quirks, cached map decisions, vendor kernel
patches, and locally modified drivers can differ from the recorded sources.
The installed version cannot prove that a driver is unmodified. IOMMU groups
are reported as evidence and do not alone determine eligibility for normal
mapping. No IOMMU or ACS settings are changed.

NVIDIA registry overrides, detected virtualization, other driver versions and
non-x86 platforms remain unknown. Internal coherent/passthrough flags are not
queried. The model assumes normal bare-metal x86 memory and normal PCI mapping;
FORCE_PCIE, skip_p2p_check, Tegra/C2C, importer peer2peer/MMIO support, buffer
attributes and GPU-export API support are outside its scope. FORCE_PCIE uses a
different coherent-platform/IOMMU-group check. An eligible normal gate does not
imply that a CUDA DMA-BUF export API is supported by the selected GPU.

Relationship to gdscheck
------------------------

NVIDIA's gdscheck checks the cuFile/GDS environment. ``-p`` verifies the platform;
``-t`` in gdscheck.py shows optimal GPU/storage pairings; ``-e`` exports topology.
These are related diagnostics, but do not expose this exact Linux-versus-NVIDIA
DMA-BUF policy model for an explicit GPU/NVMe PCI pair. Its file/latency modes
perform additional work and are not invoked by this tool.

On a tested RTX 4090 system with an AMD Raphael host bridge, gdscheck 1.19.1.55
reports NVMe compatibility mode, use_pci_p2pdma=false,
use_compat_mode=true and GDR unsupported for the RTX 4090. Both policy models here
admit 0000:01:00.0 paired with 0000:02:00.0, 0000:05:00.0 and 0000:0c:00.0. This
admits a topology gate for the experimental kernel DMA-BUF route, not the entire
cuFile stack. A W7900 0000:03:00.0 paired with 0000:04:00.0 on an Intel
8086:a700 host-bridge system is rejected by Linux's host bridge policy, matching
the earlier live attach trace.

References:

* https://github.com/NVIDIA/open-gpu-kernel-modules/tree/615.71.09
* https://docs.nvidia.com/gpudirect-storage/troubleshooting-guide/

The JSON chipset table is derived from factual IDs/capabilities in NVIDIA's
MIT/GPL dual-licensed open driver; it preserves the original rule order and
records the unmodified source digests for auditing.
