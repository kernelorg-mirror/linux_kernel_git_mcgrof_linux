#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Predict PCI peer policy; optionally test an owned Vulkan/RM storage route.

Default mode allocates no GPU memory and performs no storage I/O.
See pci-peer-check.rst for assumptions and the NVIDIA ABI version boundary.
"""

import argparse
import ctypes as ct
import fcntl
import gzip
import json
import os
import platform
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile


HERE = Path(__file__).resolve().parent
BDF = re.compile(r"(?:(?P<domain>[0-9a-fA-F]{4}|[0-9a-fA-F]{8}):)?"
                 r"(?P<bus>[0-9a-fA-F]{2}):(?P<slot>[0-9a-fA-F]{2})\."
                 r"(?P<function>[0-7])\Z")
ACS_REDIRECT = (1 << 2) | (1 << 3) | (1 << 5)
INTEL_SAME_HOST = {0x3c00, 0x3c01, 0x2f00, 0x2f01}
INTEL_ANY_HOST = {
    0x2030, 0x2031, 0x2032, 0x2033, 0x2020, 0x09a2,
    0x0b25, 0x0cfe, 0x11fb, 0x1212, 0x1216,
    0x4940, 0x4942, 0x4944, 0x4946, 0x4948,
}


def normalize_bdf(value):
    match = BDF.fullmatch(value)
    if not match:
        raise ValueError("invalid PCI address: " + value)
    domain = int(match['domain'] or '0', 16)
    slot = int(match['slot'], 16)
    if domain > 0xffff or slot > 31:
        raise ValueError("PCI domain or slot out of range: " + value)
    return f"{domain:04x}:{int(match['bus'], 16):02x}:{slot:02x}.{match['function']}"


def result(status, reason, **fields):
    return dict(status=status, reason=reason, **fields)


def read_text(path):
    return path.read_text().strip()


def device_info(bdf, sysfs):
    path = sysfs / 'bus/pci/devices' / bdf
    real = path.resolve(strict=True)
    chain = [name for name in reversed(real.parts) if BDF.fullmatch(name)]
    hosts = [name for name in real.parts if re.fullmatch(r'pci[0-9a-f]{4}:[0-9a-f]{2}', name)]
    if not chain or chain[0] != bdf or len(hosts) != 1:
        raise ValueError("cannot resolve PCI ancestry for " + bdf)
    info = dict(bdf=bdf, chain=chain, host=hosts[0])
    for field in ('vendor', 'device', 'class'):
        info[field] = int(read_text(path / field), 16)
    for field in ('driver', 'iommu_group'):
        link = path / field
        info[field] = link.resolve().name if link.is_symlink() else None
    return info


def acs_config(data):
    """Return True/False/None; unreadable or malformed config is unknown."""
    # No PCIe capability in conventional config means no ACS ext capability.
    if len(data) < 64:
        return None
    if not (struct.unpack_from('<H', data, 6)[0] & 0x10):
        return False
    pos = data[0x34] & ~3
    seen = set()
    pcie = False
    while pos:
        if pos < 0x40 or pos + 2 > min(len(data), 256) or pos in seen:
            return None
        seen.add(pos)
        pcie |= data[pos] == 0x10
        pos = data[pos + 1] & ~3
    if not pcie:
        return False
    pos = 0x100
    seen = set()
    while pos:
        if pos < 0x100 or pos & 3 or pos + 4 > len(data) or pos in seen:
            return None
        seen.add(pos)
        header = struct.unpack_from('<I', data, pos)[0]
        if header in (0, 0xffffffff):
            return False if header == 0 else None
        if header & 0xffff == 0x0d:
            if pos + 8 > len(data):
                return None
            control = struct.unpack_from('<H', data, pos + 6)[0]
            return bool(control & ACS_REDIRECT)
        pos = (header >> 20) & 0xfff
    return False


def get_acs(bdf, sysfs):
    try:
        return acs_config((sysfs / 'bus/pci/devices' / bdf / 'config').read_bytes())
    except OSError:
        return None


def host_identity(host, sysfs):
    # The kernel uses the first enumerated device, not an arbitrary host-class
    # device. Only model the usual root-bus 00.0 case; otherwise stay unknown.
    domain, bus = host[3:].split(':')
    bdf = f'{domain}:{bus}:00.0'
    try:
        info = device_info(bdf, sysfs)
        if info['host'] == host and len(info['chain']) == 1:
            return {key: info[key] for key in ('bdf', 'vendor', 'device')}
    except (OSError, ValueError):
        pass
    return None


def host_allowed(identity, same_host):
    if identity is None:
        return None
    vendor, device = identity['vendor'], identity['device']
    return (vendor == 0x8086 and
            (device in INTEL_ANY_HOST or same_host and device in INTEL_SAME_HOST)
            or vendor == 0x1ae0
            or vendor == 0x10de and device in {0x2f95, 0x2f96, 0x2f97, 0x2f98})


def cpu_info(proc):
    fields = {}
    try:
        first = (proc / 'cpuinfo').read_text().split('\n\n', 1)[0]
        fields = dict(line.split(':', 1) for line in first.splitlines() if ':' in line)
        fields = {key.strip(): value.strip() for key, value in fields.items()}
    except OSError:
        pass
    family = fields.get('cpu family')
    return dict(vendor=fields.get('vendor_id'), family=int(family) if family else None,
                machine=platform.machine(), hypervisor='hypervisor' in fields.get('flags', '').split())


def kernel_config(proc):
    candidates = [Path('/boot') / ('config-' + platform.release()), proc / 'config.gz']
    for path in candidates:
        try:
            data = gzip.decompress(path.read_bytes()).decode() if path.suffix == '.gz' else path.read_text()
        except (OSError, EOFError, UnicodeError):
            continue
        if re.search(r'^CONFIG_PCI_P2PDMA=y$', data, re.M):
            return dict(enabled=True, source=str(path))
        if re.search(r'^# CONFIG_PCI_P2PDMA is not set$', data, re.M):
            return dict(enabled=False, source=str(path))
    return dict(enabled=None, source=None)


def linux_policy(gpu, nvme, cpu, hosts, acs):
    a, b = gpu['chain'], nvme['chain']
    common = next((dev for dev in a if dev in b), None)
    # Kernel examines the provider side including the common device, and the
    # client side excluding it. With no common device, ACS cannot change route.
    if common:
        ia, ib = a.index(common), b.index(common)
        path = a[:ia + 1] + b[:ib]
        distance = ia + ib
        redirects = [dev for dev in path if acs[dev] is True]
        unknown = [dev for dev in path if acs[dev] is None]
        if not redirects and not unknown:
            return result('eligible', 'common PCI ancestor; no ACS redirect',
                          map_type='BUS_ADDR', distance=distance, acs_unknown=[])
    else:
        distance = len(a) + len(b)
        redirects, unknown = [], []
    zen = (cpu['machine'] in ('x86_64', 'i386', 'i686') and
           cpu['vendor'] == 'AuthenticAMD' and cpu['family'] is not None and
           cpu['family'] >= 0x17)
    same = gpu['host'] == nvme['host']
    allowed = [host_allowed(hosts[host], same) for host in {gpu['host'], nvme['host']}]
    fields = dict(distance=distance, acs_redirect=redirects, acs_unknown=unknown)
    if zen or all(value is True for value in allowed):
        return result('eligible', 'AMD Zen CPU allowance' if zen else 'host bridge allowlist',
                      map_type='UNKNOWN' if unknown and not redirects else 'THRU_HOST_BRIDGE', **fields)
    cpu_unknown = (cpu['machine'] in ('x86_64', 'i386', 'i686') and
                   (cpu['vendor'] is None or cpu['family'] is None))
    if cpu_unknown or (any(value is None for value in allowed) and
                       not any(value is False for value in allowed)):
        return result('unknown', 'CPU or root-bus identity unavailable', map_type='UNKNOWN', **fields)
    if unknown and not redirects:
        return result('unknown', 'ACS config unreadable; host route would be rejected',
                      map_type='UNKNOWN', **fields)
    return result('rejected', 'host route: CPU allowance false and host bridge not allowlisted',
                  map_type='NOT_SUPPORTED', **fields)


class Version(ct.Structure):
    _fields_ = [('cmd', ct.c_uint32), ('reply', ct.c_uint32), ('version', ct.c_char * 64)]


class Alloc(ct.Structure):
    _fields_ = [(name, ct.c_uint32) for name in ('root', 'parent', 'object', 'class_id')] + [
        ('params', ct.c_uint64), ('size', ct.c_uint32), ('status', ct.c_uint32)]


class Control(ct.Structure):
    _fields_ = [(name, ct.c_uint32) for name in ('client', 'object', 'cmd', 'flags')] + [
        ('params', ct.c_uint64), ('size', ct.c_uint32), ('status', ct.c_uint32)]


class Free(ct.Structure):
    _fields_ = [(name, ct.c_uint32) for name in ('root', 'parent', 'object', 'status')]


CHIPSET_IDS = ('vendorId', 'deviceId', 'subSysVendorId', 'subSysDeviceId',
               'HBvendorId', 'HBdeviceId', 'HBsubSysVendorId', 'HBsubSysDeviceId')


class Chipset(ct.Structure):
    _fields_ = [(name, ct.c_uint16) for name in CHIPSET_IDS] + [('sliBondId', ct.c_uint32)] + [
        (name, ct.c_char * 32) for name in ('vendorName', 'subSysVendorName', 'chipsetName', 'sliBondName')
    ] + [('flags', ct.c_uint32), ('chipsetId', ct.c_uint32)]


def rm_ioctl(fd, command, obj):
    data = bytearray(bytes(obj))
    request = (3 << 30) | (ct.sizeof(obj) << 16) | (ord('F') << 8) | command
    fcntl.ioctl(fd, request, data, True)
    ct.memmove(ct.addressof(obj), bytes(data), len(data))
    if hasattr(obj, 'status') and obj.status:
        raise OSError(f'NVIDIA RM status 0x{obj.status:x}')


def query_chipset(version):
    """Public system identity query. No device/buffer objects or CUDA context."""
    if platform.machine() != 'x86_64' or ct.sizeof(ct.c_void_p) != 8:
        raise OSError('NVIDIA query ABI supported only on x86_64')
    if [ct.sizeof(klass) for klass in (Version, Alloc, Control, Free, Chipset)] != [72, 32, 32, 16, 156]:
        raise OSError('unexpected NVIDIA ioctl structure layout')
    fd = os.open('/dev/nvidiactl', os.O_RDWR | os.O_CLOEXEC)
    client = 0
    try:
        check = Version(version=version.encode('ascii'))
        rm_ioctl(fd, 210, check)  # NV_ESC_CHECK_VERSION_STR, STRICT
        if check.reply != 1:
            raise OSError('NVIDIA RM strict version handshake failed')
        alloc = Alloc(class_id=0x41, status=0xffffffff)  # NV01_ROOT_CLIENT
        rm_ioctl(fd, 0x2b, alloc)
        client = alloc.object
        if not client:
            raise OSError('NVIDIA RM returned a null client')
        chipset = Chipset()
        control = Control(client=client, object=client, cmd=0x104,
                          params=ct.addressof(chipset), size=ct.sizeof(chipset), status=0xffffffff)
        rm_ioctl(fd, 0x2a, control)
        identity = {name: getattr(chipset, name) for name in CHIPSET_IDS + ('chipsetId',)}
        if any(identity[name] == 0xffff for name in CHIPSET_IDS):
            raise OSError('NVIDIA chipset identity incomplete; initialize selected GPU first')
        identity['chipsetName'] = chipset.chipsetName.decode('utf-8', errors='replace')
        return identity
    finally:
        try:
            if client:
                rm_ioctl(fd, 0x29, Free(root=client, object=client, status=0xffffffff))
        finally:
            os.close(fd)


def chipset_rule(identity, profile):
    fields = profile['rule_columns']
    for index, rule in enumerate(profile['nvidia_rules']):
        match = rule['match']
        if match[0] is not None and match[0] != identity['chipsetId']:
            continue
        if any(value is not None and value not in (identity[key], identity['HB' + key])
               for key, value in zip(fields[1:], match[1:])):
            continue
        return dict(index=index, **rule)
    raise ValueError('NVIDIA policy has no default rule')


def nvidia_policy(gpu, nvme, version, identity, profile, overrides=False, virtual=False):
    if gpu['driver'] != 'nvidia':
        return result('not-applicable', 'selected GPU is not bound to nvidia')
    if version != profile['nvidia_version']:
        return result('unknown', 'installed NVIDIA version has no matching policy profile')
    if overrides or virtual or platform.machine() != 'x86_64':
        return result('unknown', 'registry overrides, virtualization or coherent-platform state not modeled')
    common = next((dev for dev in gpu['chain'][1:] if dev in nvme['chain'][1:]), None)
    rule = chipset_rule(identity, profile) if identity else None
    fields = dict(common_bridge=common, chipset_rule=rule, scope='normal PCI DMA-BUF topology gate')
    if common:
        return result('eligible', 'shared upstream PCI bridge satisfies NVIDIA fallback', **fields)
    if not rule:
        return result('unknown', 'NVIDIA chipset identity unavailable', **fields)
    if rule['caps'] == 'READWRITE':
        return result('eligible', 'chipset table permits both peer reads and writes', **fields)
    return result('rejected', 'chipset lacks both peer capabilities and there is no shared bridge', **fields)


def diagnose(gpu_bdf, nvme_bdf, sysfs, proc, profile, query=True):
    gpu = device_info(gpu_bdf, sysfs)
    nvme = device_info(nvme_bdf, sysfs)
    if gpu['class'] >> 16 != 3:
        raise ValueError(gpu_bdf + ' is not a display/3D GPU')
    if nvme['class'] != 0x010802:
        raise ValueError(nvme_bdf + ' is not an NVMe controller (class 010802)')
    cpu = cpu_info(proc)
    hosts = {host: host_identity(host, sysfs) for host in (gpu['host'], nvme['host'])}
    acs = {dev: get_acs(dev, sysfs) for dev in set(gpu['chain'] + nvme['chain'])}
    report = dict(gpu=gpu, nvme=nvme, cpu=cpu, host_bridges=hosts, acs=acs,
                  kernel=platform.release(), model_basis=dict(linux=profile['linux_basis'],
                  nvidia=profile['nvidia_commit']), transfer_tested=False)
    report['linux_p2pdma'] = linux_policy(gpu, nvme, cpu, hosts, acs)
    config = kernel_config(proc)
    report['config_pci_p2pdma'] = config
    if config['enabled'] is False:
        report['linux_p2pdma'] = result('rejected', 'CONFIG_PCI_P2PDMA disabled: kernel distance helper returns -1',
                                       map_type='NOT_SUPPORTED')
    elif config['enabled'] is None and report['linux_p2pdma']['status'] == 'eligible':
        report['linux_p2pdma'] = result('unknown', 'topology eligible, but CONFIG_PCI_P2PDMA unavailable',
                                       topology_prediction=report['linux_p2pdma'])
    identity, version, query_error = None, None, None
    overrides = gpu['driver'] == 'nvidia'
    if gpu['driver'] == 'nvidia':
        try:
            version = read_text(sysfs / 'module/nvidia/version')
            params = (proc / 'driver/nvidia/params').read_text()
            keys = ('RegistryDwords', 'RegistryDwordsPerDevice', 'RegistryBinaryFilePerDevice')
            overrides = any(not re.search(r'^' + key + r':\s*""\s*$', params, re.M) for key in keys)
            # This public query describes system chipset, not per-GPU flags.
            # Require selected adapter metadata to avoid a different GPU fallback.
            info = (proc / 'driver/nvidia/gpus' / gpu_bdf / 'information').read_text()
            if not re.search(r'^Bus Location:\s*' + re.escape(gpu_bdf) + r'\s*$', info, re.M):
                raise OSError('selected NVIDIA GPU identity unavailable')
            if query and version == profile['nvidia_version']:
                identity = query_chipset(version)
        except OSError as error:
            query_error = str(error)
            overrides = True if version is None else overrides
    report['nvidia'] = dict(version=version, chipset=identity, query_error=query_error,
                           registry_overrides=overrides)
    report['nvidia_dmabuf'] = nvidia_policy(gpu, nvme, version, identity, profile,
                                         overrides=overrides, virtual=cpu['hypervisor'])
    report['amdgpu_effect'] = (
        'peer2peer cleared; attachment may use GTT host RAM' if
        gpu['driver'] == 'amdgpu' and report['linux_p2pdma']['status'] == 'rejected'
        else 'not determined; buffer placement and attachment were not tested')
    return report


def route_explanations(report):
    """Keep topology permission, possible placement and observed I/O distinct."""
    linux, nvidia = report['linux_p2pdma'], report['nvidia_dmabuf']
    explanations = [
        'Topology eligibility permits a source-modeled peer attachment; it does '
        'not prove export support, current buffer placement or byte-correct I/O.',
        'THRU_HOST_BRIDGE describes PCI forwarding through a root complex, '
        'not a copy into host RAM.',
    ]
    if report['gpu']['driver'] == 'amdgpu':
        explanations.append(
            'RAM FALLBACK PREDICTED: AMD clears peer2peer when the distance '
            'helper rejects this pair; pin/map may move the backing BO from '
            'VRAM into host TT/GTT. No live allocation was inspected.' if
            linux['status'] == 'rejected' else
            'AMD RAM fallback is NOT DETERMINED. An eligible topology can '
            'permit VRAM, but importer flags, other attachments, BO attributes '
            'and placement must be checked on the actual buffer.')
        explanations.append(
            'A Vulkan AMD allocation uses the same AMD DMA-BUF attachment '
            'policy; it does not repair rejected peer topology. The NVIDIA '
            'Vulkan/OPAQUE_FD/CUDA/RM provider does not apply to AMD.')
    else:
        explanations.append(
            'NVIDIA normal DMA-BUF topology gate: ' + nvidia['status'] + '. '
            'Its attach rejection has no AMD-style automatic GTT migration. '
            'Native CUDA export support is a separate capability.')
    explanations.extend([
        'VULKAN ALLOCATION ALTERNATIVE: allocate owned device-local Vulkan '
        'memory, export OPAQUE_FD, map the same object into CUDA, then export '
        'the final nv_dmabuf through NVIDIA RM. This is not Vulkan DRM DMA_BUF '
        'export or re-export of an existing arbitrary Torch pointer.',
        'Vulkan/RM is experimental and version guarded. It does not force '
        'topology permission, enable cuFile/GDS, or silently choose host RAM.',
        'A RAM bounce buffer is a separate host staging buffer copied to/from '
        'GPU memory. AMD BO migration changes the backing placement instead; '
        'both can involve host payload RAM. CPU metadata and verification '
        'copies alone are not a payload fallback.',
    ])
    probe = report.get('vulkan_rm_probe')
    if probe:
        explanations.append(
            'Vulkan/RM probe: ' + probe['status'] + '; RAM fallback: ' +
            probe['ram_fallback'] + '. This is an explicitly requested '
            'alternative-allocation test, not automatic runtime error recovery. '
            'Helper receipts are not a physical PCIe bus audit.')
        native = probe.get('native_cuda_export')
        if isinstance(native, dict) and native.get('results') == [801, 801]:
            explanations.append(
                'Native CUDA export returned CUDA_ERROR_NOT_SUPPORTED (801) '
                'with both default and PCIe mapping flags. Eligible topology '
                'does not override that export refusal; the Vulkan/RM test '
                'allocates and exports a different owned object.')
        if probe['status'] == 'passed':
            explanations.append(
                f"Byte verification: {probe['writes_verified']} writes and "
                f"{probe['reads_verified']} reads. The helper reports owned "
                'device-local memory and nv_dmabuf export. This qualifies '
                'its bounded transfer test, not LMCache/P/D, arbitrary '
                'concurrency, reset recovery or performance.')
    return explanations


def validate_probe(value, report):
    """Validate the external helper's versioned result, never infer success."""
    if not isinstance(value, dict) or value.get('schema') != 1:
        raise ValueError('route helper returned an unsupported result schema')
    for key in ('gpu', 'nvme'):
        if value.get(key) != report[key]['bdf']:
            raise ValueError('route helper result does not match selected ' + key)
    if value.get('status') not in ('passed', 'failed', 'unknown', 'unsupported'):
        raise ValueError('route helper returned an invalid status')
    if value.get('ram_fallback') not in ('observed', 'not-observed', 'unknown'):
        raise ValueError('route helper must report RAM fallback evidence state')
    if type(value.get('transfer_tested')) is not bool:
        raise ValueError('route helper omitted transfer_tested')
    if value['status'] == 'passed':
        if (value.get('route') != 'vulkan-opaque-cuda-rm-dmabuf' or
                value.get('payload_memory') != 'gpu-device-local' or
                value.get('exporter') != 'nv_dmabuf' or
                value.get('driver') != report['nvidia']['version'] or
                value.get('kernel') != report['kernel'] or
                not value['transfer_tested'] or
                value.get('byte_verified') is not True):
            raise ValueError('passing route helper lacks allocation/export/byte evidence')
        for key in ('writes_verified', 'reads_verified'):
            if type(value.get(key)) is not int or value[key] <= 0:
                raise ValueError('passing route helper must verify both directions')
        if value['ram_fallback'] != 'not-observed':
            raise ValueError('GPU-only route pass cannot claim observed/unknown RAM fallback')
    return value


def probe_vulkan_rm(report, helper, scratch):
    """An opt-in executable helper owns ABI, allocation, extents and teardown."""
    if (report['gpu']['driver'] != 'nvidia' or
            report['nvidia']['version'] != '615.71.09'):
        raise ValueError('Vulkan/RM probe requires NVIDIA driver profile 615.71.09')
    helper = helper.resolve(strict=True)
    scratch = scratch.resolve(strict=True)
    if not helper.is_file() or not os.access(helper, os.X_OK) or not scratch.is_dir():
        raise ValueError('probe helper must be executable; scratch must be a directory')
    # File-backed stdout remains available if a helper retains live DMA owners
    # on timeout. Never kill it or leave a pipe whose closure causes SIGPIPE.
    fd, name = tempfile.mkstemp(prefix='pci-peer-vulkan-rm-', suffix='.json', dir=scratch)
    with os.fdopen(fd, 'w') as output:
        process = subprocess.Popen(
            [str(helper), '--gpu', report['gpu']['bdf'], '--nvme', report['nvme']['bdf'],
             '--scratch-dir', str(scratch)], stdout=output, start_new_session=True)
        try:
            code = process.wait(timeout=60)
        except subprocess.TimeoutExpired:
            return dict(schema=1, gpu=report['gpu']['bdf'], nvme=report['nvme']['bdf'],
                        status='unknown', transfer_tested=None, ram_fallback='unknown',
                        reason='helper timeout; completion unknown, process retained',
                        retained_pid=process.pid, helper_output=name)
    try:
        value = validate_probe(json.loads(Path(name).read_text()), report)
    except (OSError, ValueError) as error:
        raise ValueError(f'invalid route helper output (retained at {name}): {error}') from error
    if (code == 0) != (value['status'] == 'passed'):
        raise ValueError('route helper exit status disagrees with result; output: ' + name)
    value['helper_output'] = name
    return value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--gpu', required=True, help='GPU PCI address, e.g. 0000:01:00.0')
    parser.add_argument('--nvme', required=True, help='NVMe controller PCI address')
    parser.add_argument('--json', action='store_true', help='emit full machine-readable evidence')
    parser.add_argument('-v', '--verbose', action='store_true',
                        help='explain topology permission, Vulkan alternative and RAM fallback')
    parser.add_argument('--probe-vulkan-rm', type=Path, metavar='HELPER',
                        help='explicitly run an experimental owned-allocation R/W helper')
    parser.add_argument('--scratch-dir', type=Path,
                        help='existing directory for new probe files; required with --probe-vulkan-rm')
    parser.add_argument('--no-nvidia-query', action='store_true', help='skip read-only RM chipset query')
    parser.add_argument('--require', choices=('linux', 'nvidia'), help='exit 0 eligible, 1 rejected, 3 unknown/not-applicable')
    args = parser.parse_args()
    if bool(args.probe_vulkan_rm) != bool(args.scratch_dir):
        parser.error('--probe-vulkan-rm and --scratch-dir must be supplied together')
    try:
        profile = json.loads((HERE / 'pci-peer-policy.json').read_text())
        report = diagnose(normalize_bdf(args.gpu), normalize_bdf(args.nvme),
                          Path('/sys'), Path('/proc'), profile, query=not args.no_nvidia_query)
        if args.probe_vulkan_rm:
            report['vulkan_rm_probe'] = probe_vulkan_rm(report, args.probe_vulkan_rm, args.scratch_dir)
            report['transfer_tested'] = report['vulkan_rm_probe']['transfer_tested']
        if args.verbose:
            report['explanations'] = route_explanations(report)
    except (OSError, ValueError) as error:
        parser.exit(2, f'{parser.prog}: {error}\n')
    if args.json:
        print(json.dumps(report, indent=2, sort_keys=True))
    else:
        print(f"GPU {report['gpu']['bdf']} ({report['gpu']['driver']}) -> NVMe {report['nvme']['bdf']}")
        for key, label in (('linux_p2pdma', 'Linux P2PDMA'), ('nvidia_dmabuf', 'NVIDIA DMA-BUF')):
            value = report[key]
            print(f"{label}: {value['status'].upper()} — {value['reason']}")
        print('AMD exporter: ' + report['amdgpu_effect'])
        if report['nvidia']['query_error']:
            print('NVIDIA query: ' + report['nvidia']['query_error'])
        if 'vulkan_rm_probe' in report:
            probe = report['vulkan_rm_probe']
            print('VULKAN FALLBACK ROUTE TEST: ' + probe['status'].upper() + ' (explicitly selected)')
            print('RAM payload fallback: ' + probe['ram_fallback'].upper())
            print('Native CUDA export: ' + str(probe.get('native_cuda_export', 'not tested')))
            print('Route receipts: ' + probe['helper_output'])
        else:
            print('Vulkan alternative: NOT TESTED. RAM payload placement: NOT TESTED.')
            print('Policy prediction only; no GPU allocation, attachment or storage transfer tested.')
        if args.verbose:
            for explanation in report['explanations']:
                print('\n' + explanation)
        print('Use --json for ancestry, ACS, chipset rule and policy source revisions.')
    if 'vulkan_rm_probe' in report and report['vulkan_rm_probe']['status'] != 'passed':
        return 1 if report['vulkan_rm_probe']['status'] == 'failed' else 3
    if args.require:
        status = report['linux_p2pdma' if args.require == 'linux' else 'nvidia_dmabuf']['status']
        return {'eligible': 0, 'rejected': 1}.get(status, 3)
    return 0


if __name__ == '__main__':
    sys.exit(main())
