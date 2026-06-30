#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# Manual test recipe for blk_iobuf_pool on hardware where io_min > PAGE_SIZE.
#
# Usage:
#   blk-iobuf-test.sh <device>          e.g. /dev/nvme0n1
#
# Boot parameters to set beforehand:
#   nvme_core.iobuf_pool=1              (auto mode)
#   nvme_core.iobuf_pool_max_order=5
#
# Or set at runtime (if module is loaded):
#   echo 5 > /sys/module/block/parameters/iobuf_pool_max_order
#

set -e

DEV="${1:-/dev/nvme0n1}"
SYSFS_Q="/sys/block/$(basename $DEV)/queue"

die() { echo "FAIL: $*" >&2; exit 1; }

# ---- Step 1: Check limits ----
echo "=== Device limits ==="
for attr in minimum_io_size optimal_io_size max_hw_sectors_kb max_segments \
            iobuf_pool_enabled iobuf_pool_order iobuf_pool_reasons \
            iobuf_pool_min_folios iobuf_pool_in_use iobuf_pool_allocs \
            iobuf_pool_misses iobuf_pool_fallbacks; do
    f="$SYSFS_Q/$attr"
    [ -f "$f" ] && printf "  %-30s %s\n" "$attr" "$(cat $f)" \
               || printf "  %-30s (not present)\n" "$attr"
done

# ---- Step 2: Verify pool was created when io_min > PAGE_SIZE ----
io_min=$(cat "$SYSFS_Q/minimum_io_size" 2>/dev/null || echo 0)
pool_enabled=$(cat "$SYSFS_Q/iobuf_pool_enabled" 2>/dev/null || echo 0)

page_size=$(getconf PAGESIZE)

echo ""
echo "=== Pool eligibility check ==="
if [ "$io_min" -gt "$page_size" ]; then
    echo "  io_min=$io_min > PAGE_SIZE=$page_size: pool should be enabled"
    [ "$pool_enabled" -eq 1 ] || die "Pool not enabled despite io_min > PAGE_SIZE"
    echo "  PASS: pool is enabled"
    reasons=$(cat "$SYSFS_Q/iobuf_pool_reasons" 2>/dev/null || echo "0x0")
    echo "  reasons=$reasons (expect bit 0 = BLK_IOBUF_REASON_IO_MIN)"
else
    echo "  io_min=$io_min <= PAGE_SIZE=$page_size: pool may not be needed"
fi

# ---- Step 3: Record baseline counters ----
allocs_before=$(cat "$SYSFS_Q/iobuf_pool_allocs" 2>/dev/null || echo 0)
fallbacks_before=$(cat "$SYSFS_Q/iobuf_pool_fallbacks" 2>/dev/null || echo 0)

# ---- Step 4: Run a passthrough or DIO workload ----
echo ""
echo "=== Running workload ==="
BS="8M"
if command -v fio >/dev/null 2>&1; then
    tmpfile=$(mktemp)
    fio --name=iobuf_write \
        --filename="$DEV" \
        --rw=write \
        --bs="$BS" \
        --direct=1 \
        --ioengine=libaio \
        --iodepth=1 \
        --size=64M \
        --output-format=terse \
        --output="$tmpfile" \
        --time_based=0 2>/dev/null || true
    echo "  fio write done"
    rm -f "$tmpfile"
else
    echo "  fio not found, skipping workload"
fi

# ---- Step 5: Check counters moved ----
allocs_after=$(cat "$SYSFS_Q/iobuf_pool_allocs" 2>/dev/null || echo 0)
fallbacks_after=$(cat "$SYSFS_Q/iobuf_pool_fallbacks" 2>/dev/null || echo 0)

echo ""
echo "=== Counter changes ==="
echo "  allocs:    $allocs_before -> $allocs_after"
echo "  fallbacks: $fallbacks_before -> $fallbacks_after"

if [ "$pool_enabled" -eq 1 ] && [ "$allocs_after" -gt "$allocs_before" ]; then
    echo "  PASS: pool allocs incremented"
fi

if [ "$fallbacks_after" -gt "$fallbacks_before" ]; then
    echo "  NOTE: fallback counter incremented (pool capacity may be low)"
fi

echo ""
echo "=== Test complete ==="
