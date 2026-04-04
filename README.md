# NOT_STISLA

## Project Description
NOT_STISLA is a C project focused on performance optimization and benchmarking. It includes utilities for native builds, detailed performance analysis, and comparison of search algorithms. The project appears to be designed for high-performance computing environments, leveraging AVX2 and AVX512 instructions.

## Build Instructions

To build the native executables for x86_64 architecture, run the following script:

```bash
./build_native.sh
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

## Contributing

If you wish to contribute, please fork the repository, make your changes, and submit a pull request. Ensure your changes are well-tested and follow the existing coding conventions.

## License

[Specify License Here - e.g., MIT, Apache 2.0]
_Note: License information was not found in the project context._

## Repository
This project is hosted at: https://github.com/SWORDIntel/NOT_STISLA
