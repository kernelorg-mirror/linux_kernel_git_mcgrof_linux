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
  ./pci-peer-check.py --gpu 01:00.0 --nvme 02:00.0 --verbose
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
Without a probe, JSON contains ``transfer_tested: false``. Linux's running build option is
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

Verbose placement and fallback explanations
-------------------------------------------

``-v`` / ``--verbose`` explains the selected policies and the separate export,
placement and transfer contracts. With ``--json``, explanations are included
as an array in the report. Linux's THRU_HOST_BRIDGE map type means PCI forwarding
through the root complex, not host payload memory.

For AMD, a rejected distance helper predicts that peer2peer is cleared and
pin/map may migrate the backing BO to host TT/GTT. This is labeled **RAM FALLBACK
PREDICTED**, never observed. Even eligible topology cannot guarantee VRAM:
the importer must permit peers, other attachments can constrain placement,
and BO attributes/driver behavior matter. Observe the actual attachment,
placement/migration and exported mapping before reporting an actual RAM route.
Aggregate VRAM/GTT counters alone are insufficient allocation evidence.

A separate CPU bounce buffer stages each transfer with copies to/from GPU
memory. AMD migration instead changes the shared allocation's backing memory.
Both can put host RAM in the payload path, but they are different mechanisms.
CPU metadata or independent verification copies do not by themselves imply a
payload RAM fallback. A Vulkan allocation on AMD uses the AMD exporter policy
and does not repair peer topology rejected by that driver.

Explicit Vulkan/RM route probe
------------------------------

An optional executable helper can test the NVIDIA alternative allocation path::

  ./pci-peer-check.py --gpu 01:00.0 --nvme 02:00.0 --verbose \
      --probe-vulkan-rm /absolute/path/to/owned-route-helper \
      --scratch-dir /path/to/owned/nvme/scratch

This mode is explicitly opt-in and requires the exact 615.71.09 profile. The
default remains a topology diagnostic. The helper is separate because allocation,
tensor/FD ownership, synchronization, and version-coupled RM ABI bindings are
not provided by Python's standard library. Use a reviewed helper: it is an
executable chosen by the caller, not a sandboxed plugin. It must check GPU
availability/UUID/BDF, verify the scratch filesystem resolves to the selected
NVMe BDF, and operate only on new owned regular files with bounded extents.
No driver/ACS/IOMMU changes, raw-device writes, existing-file replacement or
automatic switch after an in-flight failure are part of this contract.

The tested allocation route is Vulkan device-local non-host-visible memory
exported as OPAQUE_FD, CUDA mapping of that same owned object, then NVIDIA RM
core DMA-BUF export (``nv_dmabuf``) for storage. Vulkan's DRM DMA_BUF export is
a different exporter. This alternative does not re-export an arbitrary existing
Torch pointer, enable cuFile/GDS, or silently use CPU payload memory. Native
CUDA export errors must be reported separately. An explicit alternative test
is not evidence of an implemented automatic application fallback.

The helper receives ``--gpu BDF --nvme BDF --scratch-dir DIRECTORY`` and emits
one JSON object to stdout, with diagnostics sent to its retained receipts or
stderr. Version 1 requires ``schema: 1``, exact ``gpu``/``nvme``,
``status`` (passed/failed/unknown/unsupported), boolean ``transfer_tested``, and
``ram_fallback`` (observed/not-observed/unknown). A pass additionally requires:

* ``route: vulkan-opaque-cuda-rm-dmabuf``;
* ``payload_memory: gpu-device-local`` and ``exporter: nv_dmabuf``;
* exact running ``driver`` and ``kernel`` identities;
* ``transfer_tested: true``, ``byte_verified: true``;
* positive integer ``writes_verified`` and ``reads_verified``;
* ``ram_fallback: not-observed`` supported by allocation/export evidence.

Exit zero means passed; every other result requires a nonzero exit. Keep
native export status, allocation/FD identities, synchronization, actual CQEs,
byte/guard verification, teardown and all failures in helper receipts. A
passing JSON object is a helper report, not an independent physical-bus audit.
Failed/unknown RAM observations must never be relabeled as a GPU-only pass.

The checker retains helper stdout in a new file under scratch and checks target
identities and both-direction byte evidence before reporting a pass. If the
helper times out it is not killed; PID/output are reported, RAM placement and
transfer progress are unknown, and no further provider is tried. A helper with
unknown GPU/I/O completion must likewise retain its owners and stop admission.
Probe failure returns 1, unknown/unsupported returns 3, and malformed results
return 2. ``--require`` still enforces the independently predicted policy and
cannot turn a failed probe into success.

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
