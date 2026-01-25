# SNIC Storage Offload with IAA Acceleration

This project implements a Storage Offload engine running on a SmartNIC (BlueField-3), exposing an NVMe-oF target to a Host Client while accelerating data path operations using Intel IAA (In-Memory Analytics Accelerator) via the `idxd` driver.

## Architecture

The system consists of two main components:

1.  **SNIC Server (`snic/`)**: Runs on the SmartNIC (ARM cores).
    *   Connects to the backend NVMe Storage Target.
    *   Manages the IAA Hardware Portal (`/dev/iax/wqX.Y`).
    *   Exposes a custom RDMA Control Plane to the Host.
    *   Polls for requests and offloads data movement/compression to IAA hardware.

2.  **Host Client (`client/`)**: Runs on the Compute Host (x86).
    *   Connects to the SNIC via RDMA.
    *   Uses a Shared Memory Ring Buffer (RDMA Registered Memory) to submit IO requests.
    *   Polls for completions asynchronously.

## Current Workflow (Async Ring Buffer)

1.  **Setup**: Client connects to SNIC via RDMA. SNIC exposes the IAA Portal address and Remote Keys (RKeys) for RDMA ops.
2.  **Submission**: Client writes a Request structure into a ring buffer slot and updates the tail pointer (simulated via RDMA Send).
3.  **Processing**:
    *   SNIC detects the new request.
    *   **Write Operation**:
        *   SNIC initiates **IAA Memmove** (via RDMA Write to Portal) to copy data from Host Memory (Source) to SNIC Staging Buffer.
        *   Upon IAA completion, SNIC submits the **NVMe Write** command to the backing storage.
    *   **Read Operation**:
        *   SNIC submits **NVMe Read** command to fetch data into Staging Buffer.
        *   Upon NVMe completion, SNIC initiates **IAA Memmove** (future: Decompression) to copy data from Staging Buffer to Host Memory (Dest).
4.  **Completion**: SNIC writes a Completion Record back to the Host's completion queue via RDMA Write.

## Future Roadmap

The current implementation establishes the control plane and data path infrastructure. The following key optimizations are planned:

### 1. Enable Real Compression/Decompression
Currently, the IAA descriptor is configured for `IAX_OPCODE_MEMMOVE` (Copy).
**Update Needed**:
*   Modify `snic.c` to use `IAX_OPCODE_COMPRESS` and `IAX_OPCODE_DECOMPRESS`.
*   Handle variable output sizes (Codec) correctly in the completion path.

### 2. BAR-Based Submission 
Replace the TCP/RDMA Connection setup with a native PCIe interface.
**Update Needed**:
*   Map a **PCIe BAR (Base Address Register)** region that the Host can write to directly.
*   Host submits commands by writing 64-byte descriptors directly to this BAR (Doorbell mechanism), eliminating the RDMA Send interaction for submission.
*   This significantly reduces submission latency (< 1us).

### 3. Local IAA Polling via BAR
**Update Needed**:
*   Configure the IAA hardware to write Completion Records directly to the emulated BAR / Memory Window visible to the SNIC.
*   This allows the SNIC to poll for hardware completions locally without DMA traversals.
