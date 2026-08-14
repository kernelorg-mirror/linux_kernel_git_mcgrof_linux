#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0

import io
import unittest

import leaf_hist


class LeafHistogramTest(unittest.TestCase):
    def test_perf_script_histogram(self):
        events = io.StringIO(
            "task 1 [000] 1.000: iommu:iommu_map_leaf: "
            "domain=ffff iova=0x100000 paddr=0x200000 "
            "leaf_size=4096 leaves=2 mapped_bytes=8192 status=0\n"
            "task 1 [000] 1.001: iommu:iommu_map_leaf: "
            "domain=ffff iova=0x200000 paddr=0x400000 "
            "leaf_size=2097152 leaves=1 mapped_bytes=2097152 status=0\n"
        )
        rows, stats = leaf_hist.build_histogram(
            events, 0x100000, 0x400000, "ffff"
        )

        self.assertEqual(
            rows,
            [
                {
                    "leaf_size": 4096,
                    "mapping_count": 1,
                    "leaf_count": 2,
                    "mapped_bytes": 8192,
                },
                {
                    "leaf_size": 2097152,
                    "mapping_count": 1,
                    "leaf_count": 1,
                    "mapped_bytes": 2097152,
                },
            ],
        )
        self.assertEqual(stats["events_seen"], 2)
        self.assertEqual(stats["events_matched"], 2)

    def test_range_is_clipped(self):
        events = [
            "iommu_map_leaf: domain=x iova=0x1000 paddr=0x2000 "
            "leaf_size=4096 leaves=2 mapped_bytes=8192 status=0\n"
        ]
        rows, _ = leaf_hist.build_histogram(events, 0x1800, 0x2800, "x")

        self.assertEqual(rows[0]["leaf_count"], 2)
        self.assertEqual(rows[0]["mapped_bytes"], 4096)

    def test_nonzero_status_is_optional(self):
        events = [
            "iommu_map_leaf: domain=x iova=0x1000 paddr=0x2000 "
            "leaf_size=4096 leaves=1 mapped_bytes=4096 status=-12\n"
        ]
        rows, stats = leaf_hist.build_histogram(
            events, 0x1000, 0x2000, "x"
        )
        self.assertEqual(rows, [])
        self.assertEqual(stats["events_nonzero_status"], 1)

        rows, _ = leaf_hist.build_histogram(
            events,
            0x1000,
            0x2000,
            "x",
            include_nonzero_status=True,
        )
        self.assertEqual(rows[0]["mapped_bytes"], 4096)

    def test_inconsistent_event_is_rejected(self):
        events = [
            "iommu_map_leaf: domain=x iova=0 paddr=0 "
            "leaf_size=4096 leaves=2 mapped_bytes=4096 status=0\n"
        ]
        with self.assertRaisesRegex(ValueError, "mapped_bytes"):
            leaf_hist.build_histogram(events, 0, 4096, "x")

    def test_expected_bytes_reject_rolled_back_prefix(self):
        events = [
            "iommu_map_leaf: domain=x iova=0x200000 paddr=0x400000 "
            "leaf_size=2097152 leaves=1 mapped_bytes=2097152 status=0\n"
        ]
        rows, _ = leaf_hist.build_histogram(
            events, 0x200000, 0x400000, "x"
        )

        with self.assertRaisesRegex(ValueError, "rolled-back"):
            leaf_hist.validate_expected_mapped_bytes(rows, 0)

    def test_domain_filter_is_exact(self):
        events = [
            "iommu_map_leaf: domain=target iova=0x1000 paddr=0x2000 "
            "leaf_size=4096 leaves=1 mapped_bytes=4096 status=0\n",
            "iommu_map_leaf: domain=other iova=0x1000 paddr=0x3000 "
            "leaf_size=4096 leaves=1 mapped_bytes=4096 status=0\n",
        ]

        rows, stats = leaf_hist.build_histogram(
            events, 0x1000, 0x2000, "target"
        )

        self.assertEqual(rows[0]["mapped_bytes"], 4096)
        self.assertEqual(stats["events_seen"], 2)
        self.assertEqual(stats["events_matched"], 1)
        self.assertEqual(stats["events_other_domain"], 1)

    def test_empty_domain_filter_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "domain token"):
            leaf_hist.build_histogram([], 0x1000, 0x2000, "")

    def test_event_without_domain_is_rejected(self):
        events = [
            "iommu_map_leaf: iova=0x1000 paddr=0x2000 "
            "leaf_size=4096 leaves=1 mapped_bytes=4096 status=0\n"
        ]

        with self.assertRaisesRegex(ValueError, "domain"):
            leaf_hist.build_histogram(events, 0x1000, 0x2000, "target")


if __name__ == "__main__":
    unittest.main()
