#!/bin/bash

echo "Building KEYSTONE for native x86_64 architecture..."

FORTRAN_CFLAGS=""
ROOT_FORTRAN_LDFLAGS=""
BENCHMARK_FORTRAN_LDFLAGS=""
OPENMP_CFLAGS=""
OPENMP_LDFLAGS=""
BASE_CFLAGS="${KEYSTONE_BASE_CFLAGS:--O3 -march=native}"
SIMD_CFLAGS=""

cpu_has() {
    grep -m1 '^flags' /proc/cpuinfo | grep -qw "$1"
}

if [ "${KEYSTONE_FORCE_SCALAR:-0}" = "1" ]; then
    echo "Forcing scalar/SSE-safe build."
else
    if [ "${KEYSTONE_ENABLE_AVX2:-auto}" != "0" ] && cpu_has avx2; then
        SIMD_CFLAGS="$SIMD_CFLAGS -mavx2"
    fi

    if [ "${KEYSTONE_ENABLE_AVX512:-0}" = "1" ] && cpu_has avx512f && cpu_has avx512dq; then
        SIMD_CFLAGS="$SIMD_CFLAGS -mavx512f -mavx512dq"
    fi
fi

NATIVE_CFLAGS="${KEYSTONE_NATIVE_CFLAGS:-$BASE_CFLAGS$SIMD_CFLAGS}"
echo "Using native CFLAGS: $NATIVE_CFLAGS"

if [ "${KEYSTONE_ENABLE_OPENMP:-1}" = "1" ]; then
    OPENMP_CFLAGS="-fopenmp"
    OPENMP_LDFLAGS="-fopenmp"
fi

if [ "${KEYSTONE_ENABLE_FORTRAN:-1}" = "1" ]; then
    if ! command -v gfortran >/dev/null 2>&1; then
        echo "KEYSTONE_ENABLE_FORTRAN=1 requires gfortran"
        exit 1
    fi

    echo "Building optional Fortran batch backend..."
    mkdir -p fortran
    gfortran -O3 -shared -fPIC -fopenmp -Jfortran \
        fortran/keystone_batch.f90 \
        -o fortran/libkeystone_batch.so || { echo "Fortran backend build failed"; exit 1; }

    FORTRAN_CFLAGS="-DKEYSTONE_ENABLE_FORTRAN"
    ROOT_FORTRAN_LDFLAGS="-L./fortran -lkeystone_batch -Wl,-rpath,\$ORIGIN/fortran"
    BIN_FORTRAN_LDFLAGS="-L./fortran -lkeystone_batch -Wl,-rpath,\$ORIGIN/../fortran"
    BENCHMARK_FORTRAN_LDFLAGS="-L./fortran -lkeystone_batch -Wl,-rpath,\$ORIGIN/../fortran"
fi

TAR_ZST_CFLAGS=""
TAR_ZST_LDFLAGS=""
if [ "${KEYSTONE_ENABLE_TAR_ZST:-auto}" != "0" ]; then
    if pkg-config --exists libarchive libzstd 2>/dev/null; then
        echo "tar.zst support detected (libarchive + libzstd)"
        TAR_ZST_CFLAGS="-DKEYSTONE_ENABLE_TAR_ZST"
        TAR_ZST_LDFLAGS="$(pkg-config --libs libarchive libzstd)"
    fi
fi

mkdir -p bin

# Clean up previous build artifacts
rm -f *.o bin/test_* benchmarks/dsmil_benchmark benchmarks/performance_proof scripts/compare_search_auto

# Compile source files for tests and benchmarks
echo "Compiling src/keystone.c..."
gcc $NATIVE_CFLAGS $OPENMP_CFLAGS -Wall -Wextra -Werror=implicit-function-declaration -I./include $FORTRAN_CFLAGS -c src/keystone.c || { echo "keystone.c compilation failed"; exit 1; }

echo "Compiling src/dsmil_keystone_wrapper.c..."
gcc $NATIVE_CFLAGS -Wall -Wextra -I./include $TAR_ZST_CFLAGS -c src/dsmil_keystone_wrapper.c || { echo "dsmil_keystone_wrapper.c compilation failed"; exit 1; }

echo "Compiling src/dsmil_telemetry_processor.c..."
gcc $NATIVE_CFLAGS -Wall -Wextra -I./include $TAR_ZST_CFLAGS -c src/dsmil_telemetry_processor.c || { echo "dsmil_telemetry_processor.c compilation failed"; exit 1; }

echo "Compiling tests/dsmil_integration_test.c..."
gcc $NATIVE_CFLAGS -Wall -Wextra -I./include -c tests/dsmil_integration_test.c || { echo "dsmil_integration_test.c compilation failed"; exit 1; }

echo "Compiling tests/test_core_native.c..."
gcc $NATIVE_CFLAGS -Wall -Wextra -I./include -c tests/test_core_native.c || { echo "test_core_native.c compilation failed"; exit 1; }

echo "Compiling tests/test_fortran_backend.c..."
gcc $NATIVE_CFLAGS -Wall -Wextra -I./include $FORTRAN_CFLAGS -c tests/test_fortran_backend.c || { echo "test_fortran_backend.c compilation failed"; exit 1; }

echo "Compiling tests/test_auto_backend.c..."
gcc $NATIVE_CFLAGS $OPENMP_CFLAGS -Wall -Wextra -I./include $FORTRAN_CFLAGS -c tests/test_auto_backend.c || { echo "test_auto_backend.c compilation failed"; exit 1; }

echo "Compiling tests/test_telemetry_processor_perf.c..."
gcc $NATIVE_CFLAGS -Wall -Wextra -I./include -c tests/test_telemetry_processor_perf.c || { echo "test_telemetry_processor_perf.c compilation failed"; exit 1; }

echo "Compiling tests/test_performance_fix.c..."
gcc $NATIVE_CFLAGS -Wall -Wextra -I./include -c tests/test_performance_fix.c || { echo "test_performance_fix.c compilation failed"; exit 1; }

echo "Compiling src/keystone_tar_zst.c..."
gcc $NATIVE_CFLAGS -Wall -Wextra -I./include $TAR_ZST_CFLAGS -c src/keystone_tar_zst.c || { echo "keystone_tar_zst.c compilation failed"; exit 1; }

echo "Compiling tests/test_tar_zst.c..."
gcc $NATIVE_CFLAGS -Wall -Wextra -I./include $TAR_ZST_CFLAGS -c tests/test_tar_zst.c || { echo "test_tar_zst.c compilation failed"; exit 1; }

# Link test binaries into bin/
echo "Linking bin/test_enhanced..."
gcc -o bin/test_enhanced keystone.o dsmil_keystone_wrapper.o dsmil_telemetry_processor.o keystone_tar_zst.o dsmil_integration_test.o -lm $OPENMP_LDFLAGS $BIN_FORTRAN_LDFLAGS $TAR_ZST_LDFLAGS || { echo "test_enhanced linking failed"; exit 1; }

echo "Linking bin/test_core_native..."
gcc -o bin/test_core_native keystone.o test_core_native.o -lm $OPENMP_LDFLAGS $BIN_FORTRAN_LDFLAGS || { echo "test_core_native linking failed"; exit 1; }

echo "Linking bin/test_fortran_backend..."
gcc -o bin/test_fortran_backend keystone.o test_fortran_backend.o -lm $OPENMP_LDFLAGS $BIN_FORTRAN_LDFLAGS || { echo "test_fortran_backend linking failed"; exit 1; }

echo "Linking bin/test_auto_backend..."
gcc -o bin/test_auto_backend keystone.o test_auto_backend.o -lm $OPENMP_LDFLAGS $BIN_FORTRAN_LDFLAGS || { echo "test_auto_backend linking failed"; exit 1; }

echo "Linking bin/test_telemetry_processor_perf..."
gcc -o bin/test_telemetry_processor_perf keystone.o dsmil_keystone_wrapper.o dsmil_telemetry_processor.o keystone_tar_zst.o test_telemetry_processor_perf.o -lm $OPENMP_LDFLAGS $BIN_FORTRAN_LDFLAGS $TAR_ZST_LDFLAGS || { echo "test_telemetry_processor_perf linking failed"; exit 1; }

echo "Linking bin/test_performance_fix..."
gcc -o bin/test_performance_fix keystone.o dsmil_keystone_wrapper.o dsmil_telemetry_processor.o keystone_tar_zst.o test_performance_fix.o -lm $OPENMP_LDFLAGS $BIN_FORTRAN_LDFLAGS $TAR_ZST_LDFLAGS || { echo "test_performance_fix linking failed"; exit 1; }

echo "Linking bin/test_tar_zst..."
gcc -o bin/test_tar_zst keystone.o dsmil_keystone_wrapper.o dsmil_telemetry_processor.o keystone_tar_zst.o test_tar_zst.o -lm $OPENMP_LDFLAGS $BIN_FORTRAN_LDFLAGS $TAR_ZST_LDFLAGS || { echo "test_tar_zst linking failed"; exit 1; }

# Compile source files for benchmarks
echo "Compiling benchmarks/dsmil_benchmark.c..."
gcc $NATIVE_CFLAGS -Wall -Wextra -I./include -c benchmarks/dsmil_benchmark.c || { echo "dsmil_benchmark.c compilation failed"; exit 1; }

echo "Compiling benchmarks/performance_proof.c..."
gcc $NATIVE_CFLAGS -Wall -Wextra -I./include -c benchmarks/performance_proof.c || { echo "performance_proof.c compilation failed"; exit 1; }

# Link benchmarks
echo "Linking benchmarks/dsmil_benchmark..."
gcc -o benchmarks/dsmil_benchmark dsmil_benchmark.o keystone.o dsmil_keystone_wrapper.o dsmil_telemetry_processor.o keystone_tar_zst.o -lm $OPENMP_LDFLAGS $BENCHMARK_FORTRAN_LDFLAGS $TAR_ZST_LDFLAGS || { echo "dsmil_benchmark linking failed"; exit 1; }

echo "Linking benchmarks/performance_proof..."
gcc -o benchmarks/performance_proof performance_proof.o keystone.o dsmil_keystone_wrapper.o dsmil_telemetry_processor.o keystone_tar_zst.o -lm $OPENMP_LDFLAGS $BENCHMARK_FORTRAN_LDFLAGS $TAR_ZST_LDFLAGS || { echo "performance_proof linking failed"; exit 1; }

echo "Building scripts/compare_search_auto..."
gcc $NATIVE_CFLAGS $OPENMP_CFLAGS -Wall -Wextra -I. -I./include -DKEYSTONE_BENCH_AUTO=1 $FORTRAN_CFLAGS \
    scripts/compare_search.c keystone.o \
    -lm $OPENMP_LDFLAGS $BENCHMARK_FORTRAN_LDFLAGS \
    -o scripts/compare_search_auto || { echo "compare_search_auto build failed"; exit 1; }

echo "Native x86_64 build completed successfully!"

# Clean up .o files (optional, but good practice for a clean build script)
rm -f *.o
