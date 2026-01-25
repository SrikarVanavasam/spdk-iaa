#!/bin/bash

# --- CONFIGURATION ---
# Change this variable if you move your SPDK installation
SPDK_DIR="/fast-lab-share/srikarv2/spdk-iaa-x86"

# Network Config (Based on your show_gids)
TARGET_IP="192.168.200.20"
TARGET_PORT="4420"
TRANSPORT="RDMA" # Using RDMA for Mellanox
# ---------------------

# path helpers
NVMF_TGT="$SPDK_DIR/build/bin/nvmf_tgt"
RPC_PY="$SPDK_DIR/scripts/rpc.py"
SETUP_SH="$SPDK_DIR/scripts/setup.sh"

# 1. CLEANUP TRAP (Handles Ctrl+C)
cleanup() {
  echo ""
  echo "--------------------------------------------------"
  echo "Caught Ctrl+C! Shutting down SPDK Target..."

  if [ -n "$TARGET_PID" ]; then
    sudo kill $TARGET_PID
    wait $TARGET_PID 2>/dev/null
    echo "Target process (PID $TARGET_PID) killed."
  fi

  # Optional: Reset hugepages or bindings if you want a complete scrub
  # sudo "$SETUP_SH" reset

  echo "Shutdown Complete."
  exit 0
}

# Register the trap for SIGINT (Ctrl+C)
trap cleanup SIGINT

# 2. PRE-FLIGHT CHECKS
if [ ! -f "$NVMF_TGT" ]; then
  echo "Error: Could not find nvmf_tgt at $NVMF_TGT"
  echo "Check your SPDK_DIR path."
  exit 1
fi

echo "Setting up hugepages..."
sudo "$SETUP_SH"

# 3. START TARGET
echo "Starting nvmf_tgt..."
sudo "$NVMF_TGT" -m 0x1 &
TARGET_PID=$!

# Wait for the RPC socket to be ready
echo "Waiting for SPDK to initialize (PID: $TARGET_PID)..."
# We loop until the RPC socket is responsive (up to 10s)
for i in {1..10}; do
  if sudo "$RPC_PY" spdk_get_version &>/dev/null; then
    echo "SPDK is ready."
    break
  fi
  sleep 1
  if [ $i -eq 10 ]; then
    echo "Error: SPDK timed out initializing."
    kill $TARGET_PID
    exit 1
  fi
done

# 4. CONFIGURE TARGET (RPC)
echo "Configuring NVMe-oF Target..."

# Create Transport
sudo "$RPC_PY" nvmf_create_transport -t $TRANSPORT -u 8192 -m 32 -c 8192

# Create Ramdisk (Malloc)
sudo "$RPC_PY" bdev_malloc_create -b Malloc0 512 512

# Create Subsystem
sudo "$RPC_PY" nvmf_create_subsystem nqn.2016-06.io.spdk:cnode1 -a -s SPDK00000000000001

# Add Namespace
sudo "$RPC_PY" nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 Malloc0

# Add Listener
sudo "$RPC_PY" nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 \
  -t $TRANSPORT \
  -a $TARGET_IP \
  -s $TARGET_PORT

# 5. RUN LOOP
echo "--------------------------------------------------"
echo "TARGET IS RUNNING"
echo "IP: $TARGET_IP | Transport: $TRANSPORT"
echo "Press Ctrl+C to stop the target."
echo "--------------------------------------------------"

# Wait indefinitely for the background process
wait $TARGET_PID
