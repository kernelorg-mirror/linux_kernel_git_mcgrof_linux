#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Build an IOMMU leaf-size histogram from ``perf script`` output."""

import argparse
import contextlib
import csv
import json
import re
import sys
from collections import defaultdict


EVENT_RE = re.compile(r"(?:iommu:)?iommu_map_leaf:\s+(?P<payload>.*)$")
FIELD_RE = re.compile(
    r"\b(domain|iova|paddr|leaf_size|leaves|mapped_bytes|status)=([^\s]+)"
)
REQUIRED_FIELDS = {
    "domain",
    "iova",
    "paddr",
    "leaf_size",
    "leaves",
    "mapped_bytes",
    "status",
}


def auto_int(value):
    """Parse a decimal or 0x-prefixed integer for argparse."""
    try:
        return int(value, 0)
    except ValueError as error:
        raise argparse.ArgumentTypeError(str(error)) from error


def parse_event(line, line_number):
    """Return one iommu_map_leaf event, or None for an unrelated line."""
    match = EVENT_RE.search(line)
    if not match:
        return None

    fields = dict(FIELD_RE.findall(match.group("payload")))
    missing = REQUIRED_FIELDS - fields.keys()
    if missing:
        names = ", ".join(sorted(missing))
        raise ValueError(f"line {line_number}: missing fields: {names}")

    event = {"domain": fields["domain"]}
    try:
        event.update(
            {
                name: int(fields[name], 0)
                for name in REQUIRED_FIELDS - {"domain"}
            }
        )
    except ValueError as error:
        raise ValueError(
            f"line {line_number}: invalid numeric field: {error}"
        ) from error

    if event["leaf_size"] <= 0 or event["leaves"] <= 0:
        raise ValueError(
            f"line {line_number}: leaf_size and leaves must be positive"
        )
    if event["mapped_bytes"] != event["leaf_size"] * event["leaves"]:
        raise ValueError(
            f"line {line_number}: mapped_bytes does not match "
            "leaf_size * leaves"
        )
    return event


def build_histogram(
    lines, iova_start, iova_end, domain, include_nonzero_status=False
):
    """Aggregate events which overlap the half-open IOVA filter range."""
    if iova_start < 0 or iova_end <= iova_start:
        raise ValueError("the IOVA range must be nonempty and nonnegative")
    if not domain:
        raise ValueError("an exact domain token is required")

    histogram = defaultdict(
        lambda: {"mapping_count": 0, "leaf_count": 0, "mapped_bytes": 0}
    )
    stats = {
        "events_seen": 0,
        "events_matched": 0,
        "events_other_domain": 0,
        "events_outside_range": 0,
        "events_nonzero_status": 0,
    }

    for line_number, line in enumerate(lines, 1):
        event = parse_event(line, line_number)
        if event is None:
            continue
        stats["events_seen"] += 1

        if event["domain"] != domain:
            stats["events_other_domain"] += 1
            continue

        if event["status"] != 0 and not include_nonzero_status:
            stats["events_nonzero_status"] += 1
            continue

        event_start = event["iova"]
        event_end = event_start + event["mapped_bytes"]
        overlap_start = max(event_start, iova_start)
        overlap_end = min(event_end, iova_end)
        if overlap_start >= overlap_end:
            stats["events_outside_range"] += 1
            continue

        leaf_size = event["leaf_size"]
        first_leaf = (overlap_start - event_start) // leaf_size
        last_leaf = (
            overlap_end - event_start + leaf_size - 1
        ) // leaf_size
        row = histogram[leaf_size]
        row["mapping_count"] += 1
        row["leaf_count"] += last_leaf - first_leaf
        row["mapped_bytes"] += overlap_end - overlap_start
        stats["events_matched"] += 1

    rows = []
    for leaf_size, counts in sorted(histogram.items()):
        rows.append({"leaf_size": leaf_size, **counts})
    return rows, stats


def write_json(output, args, rows, stats):
    result = {
        "iova_start": args.iova_start,
        "iova_end": args.iova_end,
        "domain_token": args.domain,
        "expected_mapped_bytes": args.expected_mapped_bytes,
        "include_nonzero_status": args.include_nonzero_status,
        **stats,
        "histogram": rows,
    }
    json.dump(result, output, indent=2, sort_keys=True)
    output.write("\n")


def write_csv(output, rows):
    fieldnames = ["leaf_size", "mapping_count", "leaf_count", "mapped_bytes"]
    writer = csv.DictWriter(output, fieldnames=fieldnames, lineterminator="\n")
    writer.writeheader()
    writer.writerows(rows)


def validate_expected_mapped_bytes(rows, expected_mapped_bytes):
    """Reject incomplete or rolled-back leaf streams when expected is known."""
    mapped_bytes = sum(row["mapped_bytes"] for row in rows)
    if expected_mapped_bytes is not None and mapped_bytes != expected_mapped_bytes:
        raise ValueError(
            "filtered mapped bytes "
            f"{mapped_bytes} do not match expected {expected_mapped_bytes}; "
            "trace may include a rolled-back or incomplete mapping"
        )


def parse_args(argv):
    parser = argparse.ArgumentParser(
        description=(
            "filter iommu:iommu_map_leaf perf-script records by a half-open "
            "IOVA range and summarize their installed leaf sizes"
        )
    )
    parser.add_argument(
        "input",
        nargs="?",
        default="-",
        help="perf script output (default: stdin)",
    )
    parser.add_argument("--iova-start", required=True, type=auto_int)
    parser.add_argument(
        "--domain",
        required=True,
        help="exact domain_token reported by the DMA-IOVA selftest JSON",
    )
    end_group = parser.add_mutually_exclusive_group(required=True)
    end_group.add_argument("--iova-end", type=auto_int)
    end_group.add_argument("--iova-size", type=auto_int)
    parser.add_argument(
        "--include-nonzero-status",
        action="store_true",
        help="include partial mappings reported by failed map operations",
    )
    parser.add_argument(
        "--expected-mapped-bytes",
        type=auto_int,
        help=(
            "require the filtered total to match an independently reported "
            "successful-map byte count"
        ),
    )
    parser.add_argument(
        "--format", choices=("json", "csv"), default="json"
    )
    parser.add_argument(
        "--output", default="-", help="output path (default: stdout)"
    )
    args = parser.parse_args(argv)
    if args.iova_size is not None:
        args.iova_end = args.iova_start + args.iova_size
    if args.iova_start < 0 or args.iova_end <= args.iova_start:
        parser.error("the IOVA range must be nonempty and nonnegative")
    return args


def main(argv=None):
    args = parse_args(argv)
    try:
        with contextlib.ExitStack() as stack:
            input_file = sys.stdin if args.input == "-" else stack.enter_context(
                open(args.input, "r", encoding="utf-8")
            )
            output_file = sys.stdout if args.output == "-" else stack.enter_context(
                open(args.output, "w", encoding="utf-8", newline="")
            )
            rows, stats = build_histogram(
                input_file,
                args.iova_start,
                args.iova_end,
                args.domain,
                args.include_nonzero_status,
            )
            validate_expected_mapped_bytes(rows, args.expected_mapped_bytes)
            if args.format == "json":
                write_json(output_file, args, rows, stats)
            else:
                write_csv(output_file, rows)
    except (OSError, ValueError) as error:
        print(f"leaf_hist.py: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
