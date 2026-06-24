# KEYSTONE Accelerator Backend Contract

This document defines the strict requirements for implementing and adopting new accelerator backends (GPU, NPU, TPU, etc.) in the KEYSTONE search engine.

## 1. Memory Ownership and Transfer Policy
- **Host-Centric Ownership**: KEYSTONE assumes the host (CPU) owns the primary memory buffers. Accelerators MUST NOT mandate that data is exclusively allocated in device memory.
- **Explicit Transfer Cost Accounting**: Every benchmark or calibration routine MUST account for PCIe/CXL transfer times when offloading to an accelerator. "Zero-copy" unified memory paths must be verified in benchmarks before they are trusted.
- **Asynchronous Transfers**: Large batch requests should be streamed asynchronously if the device supports overlapping compute and transfer operations.

## 2. Batch-Size Thresholds
- Accelerators (e.g., GPUs) generally exhibit high dispatch latency. An accelerator backend MUST define a `min_batch_size` below which requests are strictly routed to the scalar or SIMD C paths.
- The `auto_backend` router will bypass accelerator evaluation for workloads with query counts below this threshold.

## 3. Fallback Paths
- Every accelerator backend MUST gracefully handle initialization failures, out-of-memory errors on the device, or unsupported payload shapes by returning `KEYSTONE_BACKEND_UNAVAILABLE` or explicitly failing over.
- In the event of a failover, the system MUST instantly fall back to `KEYSTONE_BACKEND_C_SIMD` (or scalar).

## 4. Benchmark Fields
To maintain honest benchmarking, any accelerator backend added to KEYSTONE must extend the benchmark logs with the following explicit fields:
- `xfer_to_device_ns`: Time spent transferring query keys to the device.
- `xfer_from_device_ns`: Time spent transferring results back to the host.
- `kernel_compute_ns`: Pure device-side execution time.
Total search time must equal `xfer_to_device_ns + kernel_compute_ns + xfer_from_device_ns`.

## 5. Correctness
- Accelerator paths must execute the standard `keystone_search_batch_auto` test suites and strictly guarantee identical results (including identical fallback behavior for `KEYSTONE_NOT_FOUND`) as the scalar path.
