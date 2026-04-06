#!/usr/bin/env bash
set -euo pipefail

# Clean generated artifacts in fio_engine/

rm -f \
  latency.out \
  *.log \
  *.out \
  *.tmp \
  *.so \
  *.o \
  dummy

echo "Cleaned generated files."