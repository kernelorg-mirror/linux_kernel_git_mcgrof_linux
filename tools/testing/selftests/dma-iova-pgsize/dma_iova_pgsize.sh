#!/bin/bash
# SPDX-License-Identifier: GPL-2.0

set -u

KSFT_SKIP=4
MODULE=dma_iova_pgsize_selftest
PARAM_DIR=/sys/module/$MODULE/parameters

BDF=${DMA_IOVA_PGSIZE_BDF:-}
API=current
MODE=immediate
NEGATIVE=none
ORDER=9
FOLIOS_PER_MAPPING=1
ITERATIONS=1
MAX_LIVE=1
MAX_BYTES=$((64 * 1024 * 1024))
SEED=1
KERNEL_COMMIT=${DMA_IOVA_PGSIZE_KERNEL_COMMIT:-}
CONFIGURE_ONLY=0
RUN_ONLY=0
LOADED_HERE=0
KEEP_MODULE=0
JSON_OUT=

usage()
{
	cat <<EOF
Usage: $0 [options]
  --bdf DDDD:BB:SS.F       target PCI function (or DMA_IOVA_PGSIZE_BDF)
  --api current|strict
  --mode immediate|churn|ramp
  --negative none|misaligned|unsupported
  --order N
  --folios-per-mapping N
  --iterations N
  --max-live N
  --max-bytes N
  --seed N
  --kernel-commit SHA1     source commit for result metadata
  --json-out FILE
  --configure-only         load/configure, but do not trigger
  --run-only               trigger a previously configured loaded module
EOF
}

die()
{
	echo "dma-iova-pgsize: $*" >&2
	exit 1
}

cleanup()
{
	if [ "$LOADED_HERE" -eq 1 ] && [ "$KEEP_MODULE" -eq 0 ]; then
		modprobe -r "$MODULE" >/dev/null 2>&1 || true
	fi
}

trap cleanup EXIT

while [ "$#" -gt 0 ]; do
	case "$1" in
	--bdf|--api|--mode|--negative|--order|--folios-per-mapping|\
	--iterations|--max-live|--max-bytes|--seed|--kernel-commit|--json-out)
		[ "$#" -ge 2 ] || die "$1 requires a value"
		case "$1" in
		--bdf) BDF=$2 ;;
		--api) API=$2 ;;
		--mode) MODE=$2 ;;
		--negative) NEGATIVE=$2 ;;
		--order) ORDER=$2 ;;
		--folios-per-mapping) FOLIOS_PER_MAPPING=$2 ;;
		--iterations) ITERATIONS=$2 ;;
		--max-live) MAX_LIVE=$2 ;;
		--max-bytes) MAX_BYTES=$2 ;;
		--seed) SEED=$2 ;;
		--kernel-commit) KERNEL_COMMIT=$2 ;;
		--json-out) JSON_OUT=$2 ;;
		esac
		shift 2
		;;
	--configure-only)
		CONFIGURE_ONLY=1
		shift
		;;
	--run-only)
		RUN_ONLY=1
		shift
		;;
	-h|--help)
		usage
		exit 0
		;;
	*)
		die "unknown option: $1"
		;;
	esac
done

[ "$(id -u)" -eq 0 ] || {
	echo "dma-iova-pgsize: root is required" >&2
	exit "$KSFT_SKIP"
}
[ "$CONFIGURE_ONLY" -eq 0 ] || [ "$RUN_ONLY" -eq 0 ] || \
	die "--configure-only and --run-only are mutually exclusive"

if [ "$RUN_ONLY" -eq 0 ]; then
	[ -n "$BDF" ] || {
		echo "dma-iova-pgsize: set --bdf or DMA_IOVA_PGSIZE_BDF" >&2
		exit "$KSFT_SKIP"
	}
	if [ -z "$KERNEL_COMMIT" ]; then
		SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
		KERNEL_COMMIT=$(git -C "$SCRIPT_DIR/../../../.." rev-parse \
			--verify HEAD 2>/dev/null || printf '%s' unknown)
	fi
	if [ ! -d "$PARAM_DIR" ]; then
		modprobe "$MODULE" || die "cannot load $MODULE"
		LOADED_HERE=1
	fi
	[ -d "$PARAM_DIR" ] || die "module parameter directory is absent"

	# module_param_string() keeps a trailing newline written through sysfs.
	printf '%s' "$BDF" > "$PARAM_DIR/bdf" || die "cannot set bdf"
	printf '%s' "$API" > "$PARAM_DIR/api" || die "cannot set api"
	printf '%s' "$MODE" > "$PARAM_DIR/mode" || die "cannot set mode"
	printf '%s' "$NEGATIVE" > "$PARAM_DIR/negative" || die "cannot set negative"
	printf '%s' "$KERNEL_COMMIT" > "$PARAM_DIR/kernel_commit" || \
		die "cannot set kernel_commit"
	printf '%s\n' "$ORDER" > "$PARAM_DIR/order" || die "cannot set order"
	printf '%s\n' "$FOLIOS_PER_MAPPING" > "$PARAM_DIR/folios_per_mapping" || \
		die "cannot set folios_per_mapping"
	printf '%s\n' "$ITERATIONS" > "$PARAM_DIR/iterations" || die "cannot set iterations"
	printf '%s\n' "$MAX_LIVE" > "$PARAM_DIR/max_live" || die "cannot set max_live"
	printf '%s\n' "$MAX_BYTES" > "$PARAM_DIR/max_bytes" || die "cannot set max_bytes"
	printf '%s\n' "$SEED" > "$PARAM_DIR/seed" || die "cannot set seed"
fi

if [ "$CONFIGURE_ONLY" -eq 1 ]; then
	KEEP_MODULE=1
	echo "dma-iova-pgsize: configured $MODULE; trigger with --run-only" >&2
	exit 0
fi

[ -d "$PARAM_DIR" ] || die "$MODULE is not loaded; configure it first"

printf '1\n' > "$PARAM_DIR/run"
TRIGGER_STATUS=$?
RESULT=$(<"$PARAM_DIR/result") || die "cannot read result"
printf '%s\n' "$RESULT"
if [ -n "$JSON_OUT" ]; then
	printf '%s\n' "$RESULT" > "$JSON_OUT" || die "cannot write $JSON_OUT"
fi
[ "$TRIGGER_STATUS" -eq 0 ] || exit 1
