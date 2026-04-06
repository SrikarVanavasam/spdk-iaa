#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJ_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

FIO_DIR="/home/xuanboj2/spdk-iaa/app/nvmf_iaa/fio"
SPDK_DIR="/fast-lab-share/srikarv2/spdk-iaa-x86"

SRC="$SCRIPT_DIR/fio_iaanic.c"
CLIENT_LIB="$PROJ_ROOT/client/snic_client_lib.c"
OUT_SO="$SCRIPT_DIR/iaanic.so"

export PKG_CONFIG_PATH="$SPDK_DIR/build/lib/pkgconfig:${PKG_CONFIG_PATH:-}"

echo "[INFO] FIO_DIR    = $FIO_DIR"
echo "[INFO] SPDK_DIR   = $SPDK_DIR"
echo "[INFO] PROJ_ROOT  = $PROJ_ROOT"
echo "[INFO] SRC        = $SRC"
echo "[INFO] CLIENT_LIB = $CLIENT_LIB"
echo "[INFO] OUT_SO     = $OUT_SO"

test -f "$FIO_DIR/fio.h" || { echo "missing: $FIO_DIR/fio.h"; exit 1; }
test -f "$FIO_DIR/config-host.h" || { echo "missing: $FIO_DIR/config-host.h"; exit 1; }
test -f "$SRC" || { echo "missing: $SRC"; exit 1; }
test -f "$CLIENT_LIB" || { echo "missing: $CLIENT_LIB"; exit 1; }

SPDK_CFLAGS="$(pkg-config --cflags spdk_nvme spdk_env_dpdk spdk_rdma_provider)"
SPDK_LIBS="$(pkg-config --libs spdk_nvme spdk_env_dpdk spdk_rdma_provider)"

echo "[INFO] SPDK_CFLAGS = $SPDK_CFLAGS"
echo "[INFO] SPDK_LIBS   = $SPDK_LIBS"

gcc -Wall -Wextra -O2 -g \
  -shared -rdynamic -fPIC \
  -D_GNU_SOURCE \
  -include "$FIO_DIR/config-host.h" \
  -I"$FIO_DIR" \
  -I"$FIO_DIR/os" \
  -I"$PROJ_ROOT" \
  -I"$PROJ_ROOT/client" \
  -I"$SPDK_DIR/include" \
  $SPDK_CFLAGS \
  -o "$OUT_SO" \
  "$SRC" \
  "$CLIENT_LIB" \
  -Wl,-rpath,"$SPDK_DIR/build/lib" \
  -Wl,-rpath,"$SPDK_DIR/dpdk/build/lib" \
  -Wl,--no-as-needed \
  $SPDK_LIBS \
  -lrdmacm \
  -libverbs \
  -luuid \
  -lcrypto \
  -lssl \
  -ldl \
  -lpthread \
  -lnuma \
  -lm

echo
echo "[OK] Built: $OUT_SO"
echo
echo "[INFO] Runtime check:"
ldd -r "$OUT_SO" || true