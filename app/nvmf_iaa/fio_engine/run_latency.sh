#!/usr/bin/env bash
set -euo pipefail

FIO_BIN="${FIO_BIN:-/home/xuanboj2/spdk-iaa/app/nvmf_iaa/fio/fio}"
ENGINE_PATH="${ENGINE_PATH:-./iaanic.so}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJ_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
HEADER_PATH="$PROJ_ROOT/nvmf_iaa.h"

BS="${1:-128k}"
RW="${RW:-write_read}"
CQ_SIZE_DEFAULT="$(awk '/#define[[:space:]]+CQ_SIZE[[:space:]]+[0-9]+/ { print $3; exit }' "$HEADER_PATH")"
CQ_SIZE_DEFAULT="${CQ_SIZE_DEFAULT:-16}"

if [[ -z "${TOTAL_IOS:-}" ]]; then
  TOTAL_IOS=32
fi

if [[ "$RW" != "write" && "$RW" != "read" && "$RW" != "write_read" ]]; then
  echo "RW must be 'write', 'read', or 'write_read', got: $RW" >&2
  exit 1
fi

if [[ "$BS" =~ ^([0-9]+)([kKmM]?)$ ]]; then
  BS_VALUE="${BASH_REMATCH[1]}"
  BS_SUFFIX="${BASH_REMATCH[2]}"
else
  echo "BS must look like 2k, 4k, 128k, 1m, etc. Got: $BS" >&2
  exit 1
fi

case "$BS_SUFFIX" in
  "" ) BS_BYTES="$BS_VALUE" ;;
  k|K ) BS_BYTES=$((BS_VALUE * 1024)) ;;
  m|M ) BS_BYTES=$((BS_VALUE * 1024 * 1024)) ;;
  * )
    echo "Unsupported BS suffix: $BS_SUFFIX" >&2
    exit 1
    ;;
esac

SIZE="${2:-$((BS_BYTES * TOTAL_IOS))}"

OUT_FILE="${OUT_FILE:-${BS}_latency.out}"

SNIC_IP="${SNIC_IP:-192.168.200.11}"
TARGET_IP="${TARGET_IP:-192.168.200.20}"
TARGET_PORT="${TARGET_PORT:-4420}"
WQ_PATH="${WQ_PATH:-/dev/iax/wq1.0}"
LBA_SHIFT="${LBA_SHIFT:-9}"
MAX_XFER_SIZE="${MAX_XFER_SIZE:-2097152}"
POLL_USLEEP="${POLL_USLEEP:-0}"
VERBOSE="${VERBOSE:-0}"

WORKDIR="$(mktemp -d)"
trap 'rm -rf "$WORKDIR"' EXIT

JOB_FIO="$WORKDIR/single_phase.fio"
ENGINE_ABS="$(realpath "$ENGINE_PATH")"

if [[ ! -f "$ENGINE_ABS" ]]; then
  echo "Missing ioengine: $ENGINE_ABS" >&2
  echo "Build it first with: ./build.sh" >&2
  exit 1
fi

cat > "$JOB_FIO" <<EOF
[global]
ioengine=external:${ENGINE_ABS}
thread=1
direct=1
iodepth=1
buffer_pattern=0x05

bs=${BS}
size=${SIZE}

snic_ip=${SNIC_IP}
target_ip=${TARGET_IP}
target_port=${TARGET_PORT}
wq_path=${WQ_PATH}
lba_shift=${LBA_SHIFT}
max_xfer_size=${MAX_XFER_SIZE}
poll_usleep=${POLL_USLEEP}
verbose=${VERBOSE}

EOF

if [[ "$RW" == "write_read" ]]; then
cat >> "$JOB_FIO" <<EOF

[write_phase]
rw=write
stonewall

[read_phase]
rw=read
EOF
else
cat >> "$JOB_FIO" <<EOF

[job]
rw=${RW}
EOF
fi

{
  echo "=== run_latency.sh ==="
  echo "timestamp: $(date '+%Y-%m-%d %H:%M:%S')"
  echo "rw: ${RW}"
  echo "cq_size: ${CQ_SIZE_DEFAULT}"
  echo "total_ios: ${TOTAL_IOS}"
  echo "bs: ${BS}"
  echo "size: ${SIZE}"
  echo "fio: ${FIO_BIN}"
  echo "engine: ${ENGINE_ABS}"
  echo "poll_usleep: ${POLL_USLEEP}"
} > "$OUT_FILE"

sudo "$FIO_BIN" "$JOB_FIO" >> "$OUT_FILE" 2>&1

echo "Saved results to ${OUT_FILE}"
