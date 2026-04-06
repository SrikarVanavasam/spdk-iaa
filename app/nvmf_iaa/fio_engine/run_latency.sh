#!/usr/bin/env bash
set -euo pipefail

FIO_BIN="${FIO_BIN:-/home/xuanboj2/spdk-iaa/app/nvmf_iaa/fio/fio}"
ENGINE_PATH="${ENGINE_PATH:-./iaanic.so}"

BS="${1:-128k}"
SIZE="${2:-$BS}"

OUT_FILE="${OUT_FILE:-${BS}_latency.out}"

SNIC_IP="${SNIC_IP:-192.168.200.11}"
TARGET_IP="${TARGET_IP:-192.168.200.20}"
TARGET_PORT="${TARGET_PORT:-4420}"
WQ_PATH="${WQ_PATH:-/dev/iax/wq1.0}"
LBA_SHIFT="${LBA_SHIFT:-9}"
MAX_XFER_SIZE="${MAX_XFER_SIZE:-2097152}"
POLL_USLEEP="${POLL_USLEEP:-1000}"
VERBOSE="${VERBOSE:-0}"

WORKDIR="$(mktemp -d)"
trap 'rm -rf "$WORKDIR"' EXIT

WRITE_FIO="$WORKDIR/write_once.fio"
READ_FIO="$WORKDIR/read_once.fio"

cat > "$WRITE_FIO" <<EOF
[global]
ioengine=external:${ENGINE_PATH}
thread=1
time_based=1
direct=1
iodepth=1

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

[job0]
rw=write
EOF

cat > "$READ_FIO" <<EOF
[global]
ioengine=external:${ENGINE_PATH}
thread=1
time_based=1
direct=1
iodepth=1

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

[job0]
rw=read
EOF

{
  echo "=== run_latency_pair.sh ==="
  echo "timestamp: $(date '+%Y-%m-%d %H:%M:%S')"
  echo "bs: ${BS}"
  echo "size: ${SIZE}"
  echo "fio: ${FIO_BIN}"
  echo "engine: ${ENGINE_PATH}"
  echo

  echo "=== WRITE ==="
} > "$OUT_FILE"

sudo "$FIO_BIN" "$WRITE_FIO" >> "$OUT_FILE" 2>&1

{
  echo
  echo "=== READ ==="
} >> "$OUT_FILE"

sudo "$FIO_BIN" "$READ_FIO" >> "$OUT_FILE" 2>&1

echo "Saved results to ${OUT_FILE}"