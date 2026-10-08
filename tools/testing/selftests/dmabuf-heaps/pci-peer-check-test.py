#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Policy and input tests: python3 pci-peer-check-test.py (no hardware needed)."""

import importlib.util
import json
from pathlib import Path
import struct
import tempfile
import unittest
import subprocess
from unittest.mock import patch

HERE = Path(__file__).resolve().parent
SPEC = importlib.util.spec_from_file_location('peer', HERE / 'pci-peer-check.py')
peer = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(peer)
PROFILE = json.loads((HERE / 'pci-peer-policy.json').read_text())
INTEL = dict(machine='x86_64', vendor='GenuineIntel', family=6)
ZEN = dict(machine='x86_64', vendor='AuthenticAMD', family=26)


def pair(common=False):
    gpu = dict(driver='nvidia', host='pci0000:00', chain=['0000:01:00.0', '0000:00:01.0'])
    nvme = dict(host='pci0000:00', chain=['0000:02:00.0', '0000:00:01.0' if common else '0000:00:02.0'])
    return gpu, nvme


def chipset(chipset_id, vendor=0x8086, device=0xa700, subvendor=0x1462, subdevice=0):
    fields = dict(vendorId=vendor, deviceId=device, subSysVendorId=subvendor, subSysDeviceId=subdevice)
    return dict(chipsetId=chipset_id, **fields, **{'HB' + key: value for key, value in fields.items()})


def config(redirect=False):
    data = bytearray(4096)
    struct.pack_into('<H', data, 6, 0x10)
    data[0x34] = 0x40
    data[0x40] = 0x10
    struct.pack_into('<I', data, 0x100, 0x1000d)
    struct.pack_into('<H', data, 0x106, 4 if redirect else 0)
    return data


class PolicyTest(unittest.TestCase):
    def linux(self, common=False, cpu=INTEL, redirect=False, unknown=False, host=None):
        gpu, nvme = pair(common)
        acs = {dev: False for dev in gpu['chain'] + nvme['chain']}
        acs[gpu['chain'][-1]] = None if unknown else redirect
        hosts = {'pci0000:00': host or dict(vendor=0x8086, device=0xa700)}
        return peer.linux_policy(gpu, nvme, cpu, hosts, acs)

    def test_intel_unlisted_host_rejection_distance_is_not_a_limit(self):
        gpu, nvme = pair()
        gpu['chain'].insert(1, '0000:03:00.0')
        gpu['chain'].insert(1, '0000:04:00.0')
        value = peer.linux_policy(gpu, nvme, INTEL, {'pci0000:00': dict(vendor=0x8086, device=0xa700)}, {})
        self.assertEqual((value['status'], value['distance']), ('rejected', 6))

    def test_common_bridge_direct(self):
        self.assertEqual(self.linux(common=True)['map_type'], 'BUS_ADDR')

    def test_common_bridge_acs_redirect_needs_host_policy(self):
        self.assertEqual(self.linux(common=True, redirect=True)['status'], 'rejected')
        self.assertEqual(self.linux(common=True, cpu=ZEN, redirect=True)['map_type'], 'THRU_HOST_BRIDGE')

    def test_common_bridge_unreadable_acs_is_unknown(self):
        self.assertEqual(self.linux(common=True, unknown=True)['status'], 'unknown')
        self.assertEqual(self.linux(common=True, cpu=ZEN, unknown=True)['status'], 'eligible')
        self.assertEqual(self.linux(common=True, cpu=ZEN, unknown=True)['map_type'], 'UNKNOWN')

    def test_amd_zen_host_bridge_route(self):
        self.assertEqual(self.linux(cpu=ZEN)['map_type'], 'THRU_HOST_BRIDGE')

    def test_missing_cpu_information(self):
        cpu = dict(machine='x86_64', vendor=None, family=None)
        self.assertEqual(self.linux(cpu=cpu)['status'], 'unknown')

    def test_host_allowlist_and_same_host_requirement(self):
        self.assertEqual(self.linux(host=dict(vendor=0x8086, device=0x3c00))['status'], 'eligible')
        gpu, nvme = pair()
        nvme['host'] = 'pci0001:00'
        hosts = {name: dict(vendor=0x8086, device=0x3c00) for name in (gpu['host'], nvme['host'])}
        self.assertEqual(peer.linux_policy(gpu, nvme, INTEL, hosts, {})['status'], 'rejected')
        self.assertEqual(peer.linux_policy(gpu, nvme, ZEN, hosts, {})['status'], 'eligible')

    def test_nvidia_x370_not_raphael_hostbridge(self):
        gpu, nvme = pair()
        identity = chipset(4163, vendor=0x1022, device=0x790e)
        identity['HBdeviceId'] = 0x14d8
        value = peer.nvidia_policy(gpu, nvme, '615.71.09', identity, PROFILE)
        self.assertEqual(value['status'], 'eligible')
        self.assertEqual(value['chipset_rule']['name'], 'CS_AMD_X370')

    def test_nvidia_deny_precedes_chipset_allow(self):
        rule = PROFILE['nvidia_rules'][0]
        identity = chipset(rule['match'][0], vendor=0x8086, device=0xa1c1, subvendor=0x105b, subdevice=0x7270)
        self.assertEqual(peer.chipset_rule(identity, PROFILE)['caps'], 'NONE')
        identity['subSysDeviceId'] = identity['HBsubSysDeviceId'] = 0
        self.assertEqual(peer.chipset_rule(identity, PROFILE)['caps'], 'READWRITE')

    def test_nvidia_read_only_and_default_reject(self):
        gpu, nvme = pair()
        for rule in PROFILE['nvidia_rules']:
            if rule['caps'] in ('READ', 'WRITE'):
                self.assertEqual(peer.nvidia_policy(gpu, nvme, '615.71.09', chipset(rule['match'][0]), PROFILE)['status'], 'rejected')
        self.assertEqual(peer.nvidia_policy(gpu, nvme, '615.71.09', chipset(0xfeed), PROFILE)['status'], 'rejected')

    def test_nvidia_shared_root_port_not_just_hostbridge(self):
        gpu, nvme = pair(True)
        self.assertEqual(peer.nvidia_policy(gpu, nvme, '615.71.09', None, PROFILE)['status'], 'eligible')
        gpu, nvme = pair(False)
        self.assertEqual(peer.nvidia_policy(gpu, nvme, '615.71.09', None, PROFILE)['status'], 'unknown')

    def test_nvidia_unsupported_version_or_overrides(self):
        gpu, nvme = pair(True)
        self.assertEqual(peer.nvidia_policy(gpu, nvme, '999.00', None, PROFILE)['status'], 'unknown')
        self.assertEqual(peer.nvidia_policy(gpu, nvme, '615.71.09', None, PROFILE, overrides=True)['status'], 'unknown')
        self.assertEqual(peer.nvidia_policy(gpu, nvme, '615.71.09', None, PROFILE, virtual=True)['status'], 'unknown')

    def test_bdf_input(self):
        for value in ('01:00.0', '0000:01:00.0', '00000000:01:00.0'):
            self.assertEqual(peer.normalize_bdf(value), '0000:01:00.0')
        for value in ('01:20.0', '01:00.8', '00010000:01:00.0', '01:00.0x', '../01:00.0'):
            with self.assertRaises(ValueError):
                peer.normalize_bdf(value)

    def test_config_chain_and_permissions(self):
        self.assertFalse(peer.acs_config(config()))
        self.assertTrue(peer.acs_config(config(True)))
        self.assertIsNone(peer.acs_config(config()[:64]))
        malformed = config()
        struct.pack_into('<I', malformed, 0x100, (0x100 << 20) | 1)
        self.assertIsNone(peer.acs_config(malformed))
        malformed = config()
        malformed[0x41] = 0x40
        self.assertIsNone(peer.acs_config(malformed))

    def test_exact_target_and_device_role_validation(self):
        with tempfile.TemporaryDirectory() as directory:
            sysfs = Path(directory) / 'sys'
            bus = sysfs / 'bus/pci/devices'
            bus.mkdir(parents=True)
            for bdf, cls in (('0000:01:00.0', 0x030000), ('0000:02:00.0', 0x010802), ('0000:02:00.1', 0x020000)):
                path = sysfs / 'devices/pci0000:00/0000:00:01.0' / bdf
                path.mkdir(parents=True)
                (bus / bdf).symlink_to(path)
                for field, value in dict(vendor=0x10de, device=0x2684, **{'class': cls}).items():
                    (path / field).write_text(hex(value))
            with self.assertRaises(FileNotFoundError):
                peer.device_info('0000:09:00.0', sysfs)
            with self.assertRaisesRegex(ValueError, 'not an NVMe'):
                peer.diagnose('0000:01:00.0', '0000:02:00.1', sysfs, Path(directory), PROFILE, query=False)
            with self.assertRaisesRegex(ValueError, 'not a display'):
                peer.diagnose('0000:02:00.0', '0000:02:00.0', sysfs, Path(directory), PROFILE, query=False)
            for enabled, expected in ((False, 'rejected'), (None, 'unknown'), (True, 'eligible')):
                with patch.object(peer, 'cpu_info', return_value=dict(ZEN, hypervisor=False)), \
                        patch.object(peer, 'kernel_config', return_value=dict(enabled=enabled, source=None)):
                    value = peer.diagnose('0000:01:00.0', '0000:02:00.0', sysfs, Path(directory), PROFILE, query=False)
                    self.assertEqual(value['linux_p2pdma']['status'], expected)

    def test_failed_rm_query_frees_client_and_closes_fd(self):
        commands = []

        def ioctl(fd, command, obj):
            commands.append(command)
            if command == 210:
                obj.reply = 1
            elif command == 0x2b:
                obj.object = 123
            elif command == 0x2a:
                raise OSError('denied')
        with patch.object(peer.platform, 'machine', return_value='x86_64'), \
                patch.object(peer.os, 'open', return_value=9), \
                patch.object(peer.os, 'close') as close, \
                patch.object(peer, 'rm_ioctl', side_effect=ioctl):
            with self.assertRaisesRegex(OSError, 'denied'):
                peer.query_chipset('615.71.09')
            close.assert_called_once_with(9)
            self.assertEqual(commands, [210, 0x2b, 0x2a, 0x29])

    def test_version_handshake_failure_allocates_nothing(self):
        with patch.object(peer.platform, 'machine', return_value='x86_64'), \
                patch.object(peer.os, 'open', return_value=9), \
                patch.object(peer.os, 'close') as close, \
                patch.object(peer, 'rm_ioctl') as ioctl:
            with self.assertRaisesRegex(OSError, 'handshake'):
                peer.query_chipset('615.71.09')
            self.assertEqual(ioctl.call_count, 1)
            close.assert_called_once_with(9)


class RouteTest(unittest.TestCase):
    def report(self, driver='nvidia', linux='eligible'):
        return dict(gpu=dict(bdf='0000:01:00.0', driver=driver),
                    nvme=dict(bdf='0000:02:00.0'), kernel='test-kernel',
                    nvidia=dict(version='615.71.09'),
                    linux_p2pdma=dict(status=linux), nvidia_dmabuf=dict(status='eligible'))

    def receipt(self):
        return dict(schema=1, gpu='0000:01:00.0', nvme='0000:02:00.0',
                    status='passed', driver='615.71.09', kernel='test-kernel',
                    route='vulkan-opaque-cuda-rm-dmabuf', payload_memory='gpu-device-local',
                    exporter='nv_dmabuf', transfer_tested=True, ram_fallback='not-observed',
                    byte_verified=True, writes_verified=9, reads_verified=1)

    def test_explanations_distinguish_predicted_migration_from_observation(self):
        text = '\n'.join(peer.route_explanations(self.report(driver='amdgpu', linux='rejected')))
        self.assertIn('RAM FALLBACK PREDICTED', text)
        self.assertIn('No live allocation was inspected', text)
        self.assertIn('same AMD DMA-BUF attachment policy', text)
        self.assertIn('not a copy into host RAM', text)

    def test_eligible_amd_does_not_promise_no_ram(self):
        self.assertIn('NOT DETERMINED', '\n'.join(peer.route_explanations(self.report(driver='amdgpu'))))

    def test_receipt_exact_device_and_both_directions(self):
        self.assertEqual(peer.validate_probe(self.receipt(), self.report())['status'], 'passed')
        for key, value in (('gpu', '0000:03:00.0'), ('nvme', '0000:04:00.0'),
                           ('driver', '999.00'), ('kernel', 'other-kernel'),
                           ('writes_verified', 0), ('reads_verified', 0),
                           ('byte_verified', False), ('transfer_tested', False),
                           ('exporter', 'drm'), ('payload_memory', 'host-gtt'),
                           ('ram_fallback', 'observed')):
            receipt = dict(self.receipt(), **{key: value})
            with self.subTest(key=key), self.assertRaises(ValueError):
                peer.validate_probe(receipt, self.report())

    def test_failed_ram_observation_does_not_become_gpu_only_pass(self):
        receipt = dict(self.receipt(), status='failed', ram_fallback='observed',
                       payload_memory='host-gtt')
        report = dict(self.report(), vulkan_rm_probe=peer.validate_probe(receipt, self.report()))
        self.assertIn('RAM fallback: observed', '\n'.join(peer.route_explanations(report)))

    def test_helper_version_guard_before_launch(self):
        report = self.report()
        report['nvidia']['version'] = '999.00'
        with patch.object(peer.subprocess, 'Popen') as launch, self.assertRaises(ValueError):
            peer.probe_vulkan_rm(report, Path('/missing'), Path('/missing'))
        launch.assert_not_called()

    def test_helper_protocol_and_exit_status(self):
        with tempfile.TemporaryDirectory() as directory:
            helper = Path(directory) / 'helper'
            helper.write_text('#!/usr/bin/env python3\nimport json\nprint(' +
                              repr(json.dumps(self.receipt())) + ')\n')
            helper.chmod(0o700)
            receipt = peer.probe_vulkan_rm(self.report(), helper, Path(directory))
            self.assertEqual(receipt['status'], 'passed')
            self.assertTrue(Path(receipt['helper_output']).exists())
            helper.write_text(helper.read_text() + 'raise SystemExit(1)\n')
            with self.assertRaisesRegex(ValueError, 'exit status disagrees'):
                peer.probe_vulkan_rm(self.report(), helper, Path(directory))

    def test_timeout_keeps_process_and_reports_uncertain_transfer(self):
        with tempfile.TemporaryDirectory() as directory:
            helper = Path(directory) / 'helper'
            helper.write_text('unused')
            helper.chmod(0o700)
            with patch.object(peer.subprocess, 'Popen') as launch:
                process = launch.return_value
                process.pid = 123
                process.wait.side_effect = subprocess.TimeoutExpired('helper', 60)
                receipt = peer.probe_vulkan_rm(self.report(), helper, Path(directory))
                self.assertEqual(receipt['status'], 'unknown')
                self.assertIsNone(receipt['transfer_tested'])
                self.assertEqual(receipt['retained_pid'], 123)
                process.kill.assert_not_called()
                process.terminate.assert_not_called()


if __name__ == '__main__':
    unittest.main()
