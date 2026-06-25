#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

echo "Running KEYSTONE benchmark suite..."

cpu_has() {
    grep -m1 '^flags' /proc/cpuinfo | grep -qw "$1"
}

BASE_CFLAGS="${KEYSTONE_BASE_CFLAGS:--O3 -march=native}"
SIMD_CFLAGS=""

if [ "${KEYSTONE_FORCE_SCALAR:-0}" = "1" ]; then
    :
else
    if [ "${KEYSTONE_ENABLE_AVX2:-auto}" != "0" ] && cpu_has avx2; then
        SIMD_CFLAGS="$SIMD_CFLAGS -mavx2"
    fi
    if [ "${KEYSTONE_ENABLE_AVX512:-0}" = "1" ] && cpu_has avx512f && cpu_has avx512dq; then
        SIMD_CFLAGS="$SIMD_CFLAGS -mavx512f -mavx512dq"
    fi
fi

NATIVE_CFLAGS="${KEYSTONE_NATIVE_CFLAGS:-$BASE_CFLAGS$SIMD_CFLAGS}"

OPENMP_CFLAGS=""
OPENMP_LDFLAGS=""
if [ "${KEYSTONE_ENABLE_OPENMP:-1}" = "1" ]; then
    OPENMP_CFLAGS="-fopenmp"
    OPENMP_LDFLAGS="-fopenmp"
fi

FORTRAN_CFLAGS=""
FORTRAN_LDFLAGS=""
if [ "${KEYSTONE_ENABLE_FORTRAN:-1}" = "1" ]; then
    if [ -f fortran/libkeystone_batch.so ]; then
        FORTRAN_CFLAGS="-DKEYSTONE_ENABLE_FORTRAN"
        FORTRAN_LDFLAGS="-L./fortran -lkeystone_batch -Wl,-rpath,\$ORIGIN/fortran"
    elif command -v gfortran >/dev/null 2>&1; then
        echo "Building Fortran backend for benchmark..."
        mkdir -p fortran
        gfortran -O3 -shared -fPIC -fopenmp -Jfortran \
            fortran/keystone_batch.f90 \
            -o fortran/libkeystone_batch.so
        FORTRAN_CFLAGS="-DKEYSTONE_ENABLE_FORTRAN"
        FORTRAN_LDFLAGS="-L./fortran -lkeystone_batch -Wl,-rpath,\$ORIGIN/fortran"
    fi
fi

# Build compare_search_auto benchmark binary
echo "Compiling src/keystone.c..."
gcc $NATIVE_CFLAGS $OPENMP_CFLAGS -Wall -Wextra -Werror=implicit-function-declaration -I./include $FORTRAN_CFLAGS \
    -c src/keystone.c -o keystone.o

echo "Compiling src/keystone_avx512.c..."
gcc $NATIVE_CFLAGS -mavx512f -mavx512dq -c src/keystone_avx512.c -o keystone_avx512.o

echo "Building compare_search_auto..."
gcc $NATIVE_CFLAGS $OPENMP_CFLAGS -Wall -Wextra -I. -I./include -DKEYSTONE_BENCH_AUTO=1 $FORTRAN_CFLAGS \
    -c scripts/compare_search.c -o scripts/compare_search_auto.o

gcc -o scripts/compare_search_auto scripts/compare_search_auto.o keystone.o keystone_avx512.o \
    -lm $OPENMP_LDFLAGS -L./fortran -lkeystone_batch -Wl,-rpath,"\$ORIGIN/../fortran" || { echo "compare_search_auto linking failed"; exit 1; }

# Run benchmark profiles and collect CSV output
CSV_FILE="benchmark_results.csv"
rm -f "$CSV_FILE"

run_profile() {
    local name=$1
    shift
    echo "Running profile: $name"
    env "$@" ./scripts/compare_search_auto >> "$CSV_FILE"
    echo "" >> "$CSV_FILE"
}

run_profile "100K_dense" KEYSTONE_N=100000 KEYSTONE_QUERIES=50000 KEYSTONE_DATA_GAP=1 KEYSTONE_HIT_RATE_PCT=100 KEYSTONE_RUNS=5
run_profile "1M_dense"   KEYSTONE_N=1000000 KEYSTONE_QUERIES=200000 KEYSTONE_DATA_GAP=1 KEYSTONE_HIT_RATE_PCT=100 KEYSTONE_RUNS=5
run_profile "1M_jitter"  KEYSTONE_N=1000000 KEYSTONE_QUERIES=200000 KEYSTONE_DATA_GAP=8 KEYSTONE_DATA_GAP_JITTER=8 KEYSTONE_HIT_RATE_PCT=100 KEYSTONE_RUNS=5
run_profile "10M_dense"  KEYSTONE_N=10000000 KEYSTONE_QUERIES=200000 KEYSTONE_DATA_GAP=1 KEYSTONE_HIT_RATE_PCT=100 KEYSTONE_RUNS=3

echo "Generating benchmark_comparison.png..."
python3 scripts/generate_benchmark_chart.py "$CSV_FILE" benchmark_comparison.png

echo "Benchmark complete."
echo "  CSV: $CSV_FILE"
echo "  PNG: benchmark_comparison.png"

# Optional: run text-based benchmarks if available
if [ -f benchmarks/dsmil_benchmark ]; then
    echo ""
    echo "Running dsmil_benchmark..."
    ./benchmarks/dsmil_benchmark || true
fi

if [ -f benchmarks/performance_proof ]; then
    echo ""
    echo "Running performance_proof..."
    ./benchmarks/performance_proof || true
fi

# Cleanup temporary objects
rm -f keystone.o keystone_avx512.o scripts/compare_search_auto.o
