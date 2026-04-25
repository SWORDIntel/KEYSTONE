# NOT_STISLA

## Project Description
NOT_STISLA is a C project focused on performance optimization and benchmarking. It includes utilities for native builds, detailed performance analysis, and comparison of search algorithms. The project appears to be designed for high-performance computing environments, leveraging AVX2 and AVX512 instructions.

## Build Instructions

To build the native executables for x86_64 architecture, run the following script:

```bash
./scripts/build_native.sh
```

This script compiles the source files and links them to create executables for testing and benchmarking.

## Benchmarking and Results

The project includes a `benchmarks` directory containing executables for performance analysis. The results of these benchmarks are intended to be documented in files like `BENCHMARK_RESULTS.md` and `OPTIMIZATION_SUMMARY.md`.

You can run the compiled benchmarks from the `benchmarks` directory:

```bash
cd benchmarks
./dsmil_benchmark
./performance_proof
```

Please ensure you have the necessary C compiler (like GCC) and development libraries installed on your system. The build script assumes an x86_64 architecture with AVX2 and AVX512 support.

### Benchmark Outputs:

**`dsmil_benchmark` Output (100M Element Dataset):**
```
📂 Loading custom dataset: larger_dataset.bin
   - Elements: 100000000
🔥 Warming up algorithms...
🔬 Comprehensive Algorithm Comparison
=====================================
Algorithm          | Time/op | Found | Speedup vs Binary
-------------------|---------|-------|------------------
Binary Search      |  999.1 ns| 50000 | 1.00x (baseline)
NOT_STISLA Classic |  213.7 ns| 50000 | 4.67x
Enhanced Search    |  205.2 ns| 50000 | 4.87x

Enhanced Search Benchmark
=====================================
Enhanced Search:   171.54 ns/op (50000/50000 found, 100.0% success)
```

**`performance_proof` Output:**
```
PERFORMANCE COMPARISON: NOT_STISLA vs reference search
=================================================================

📊 Performance Comparison Matrix:
================================

Binary Search:     measured per target run
Reference Search:  measured per target run
NOT_STISLA:        measured per target run

🎯 CONCLUSION:
   NOT_STISLA delivered the fastest observed latency in this run
   The measured gap should be validated on target hardware and workloads

Performance proof complete.
```

## Contributing

If you wish to contribute, please fork the repository, make your changes, and submit a pull request. Ensure your changes are well-tested and follow the existing coding conventions.

## License

[Specify License Here - e.g., MIT, Apache 2.0]
_Note: License information was not found in the project context._

## Repository
This project is hosted at: https://github.com/SWORDIntel/NOT_STISLA
