#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

echo "Running StiSorter benchmark suite..."

cpu_has() {
    grep -m1 '^flags' /proc/cpuinfo | grep -qw "$1"
}

BASE_CFLAGS="${NOT_STISLA_BASE_CFLAGS:--O3 -march=native}"
SIMD_CFLAGS=""

if [ "${NOT_STISLA_FORCE_SCALAR:-0}" = "1" ]; then
    :
else
    if [ "${NOT_STISLA_ENABLE_AVX2:-auto}" != "0" ] && cpu_has avx2; then
        SIMD_CFLAGS="$SIMD_CFLAGS -mavx2"
    fi
    if [ "${NOT_STISLA_ENABLE_AVX512:-0}" = "1" ] && cpu_has avx512f && cpu_has avx512dq; then
        SIMD_CFLAGS="$SIMD_CFLAGS -mavx512f -mavx512dq"
    fi
fi

NATIVE_CFLAGS="${NOT_STISLA_NATIVE_CFLAGS:-$BASE_CFLAGS$SIMD_CFLAGS}"

OPENMP_CFLAGS=""
OPENMP_LDFLAGS=""
if [ "${NOT_STISLA_ENABLE_OPENMP:-1}" = "1" ]; then
    OPENMP_CFLAGS="-fopenmp"
    OPENMP_LDFLAGS="-fopenmp"
fi

FORTRAN_CFLAGS=""
FORTRAN_LDFLAGS=""
if [ "${NOT_STISLA_ENABLE_FORTRAN:-1}" = "1" ]; then
    if [ -f fortran/libnot_stisla_batch.so ]; then
        FORTRAN_CFLAGS="-DNOT_STISLA_ENABLE_FORTRAN"
        FORTRAN_LDFLAGS="-L./fortran -lnot_stisla_batch -Wl,-rpath,\$ORIGIN/fortran"
    elif command -v gfortran >/dev/null 2>&1; then
        echo "Building Fortran backend for benchmark..."
        mkdir -p fortran
        gfortran -O3 -shared -fPIC -fopenmp -Jfortran \
            fortran/not_stisla_batch.f90 \
            -o fortran/libnot_stisla_batch.so
        FORTRAN_CFLAGS="-DNOT_STISLA_ENABLE_FORTRAN"
        FORTRAN_LDFLAGS="-L./fortran -lnot_stisla_batch -Wl,-rpath,\$ORIGIN/fortran"
    fi
fi

# Build compare_search_auto benchmark binary
echo "Compiling src/not_stisla.c..."
gcc $NATIVE_CFLAGS $OPENMP_CFLAGS -Wall -Wextra -Werror=implicit-function-declaration -I./include $FORTRAN_CFLAGS \
    -c src/not_stisla.c -o not_stisla.o

echo "Building compare_search_auto..."
gcc $NATIVE_CFLAGS $OPENMP_CFLAGS -Wall -Wextra -I. -I./include -DNOT_STISLA_BENCH_AUTO=1 $FORTRAN_CFLAGS \
    -c scripts/compare_search.c -o scripts/compare_search_auto.o

gcc -o scripts/compare_search_auto scripts/compare_search_auto.o not_stisla.o \
    -lm $OPENMP_LDFLAGS -L./fortran -lnot_stisla_batch -Wl,-rpath,"\$ORIGIN/../fortran" || { echo "compare_search_auto linking failed"; exit 1; }

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

run_profile "100K_dense" NOT_STISLA_N=100000 NOT_STISLA_QUERIES=50000 NOT_STISLA_DATA_GAP=1 NOT_STISLA_HIT_RATE_PCT=100 NOT_STISLA_RUNS=5
run_profile "1M_dense"   NOT_STISLA_N=1000000 NOT_STISLA_QUERIES=200000 NOT_STISLA_DATA_GAP=1 NOT_STISLA_HIT_RATE_PCT=100 NOT_STISLA_RUNS=5
run_profile "1M_jitter"  NOT_STISLA_N=1000000 NOT_STISLA_QUERIES=200000 NOT_STISLA_DATA_GAP=8 NOT_STISLA_DATA_GAP_JITTER=8 NOT_STISLA_HIT_RATE_PCT=100 NOT_STISLA_RUNS=5
run_profile "10M_dense"  NOT_STISLA_N=10000000 NOT_STISLA_QUERIES=200000 NOT_STISLA_DATA_GAP=1 NOT_STISLA_HIT_RATE_PCT=100 NOT_STISLA_RUNS=3

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
rm -f not_stisla.o scripts/compare_search_auto.o
