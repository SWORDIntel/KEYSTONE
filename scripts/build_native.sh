#!/bin/bash

echo "Building NOT_STISLA for native x86_64 architecture..."

FORTRAN_CFLAGS=""
ROOT_FORTRAN_LDFLAGS=""
BENCHMARK_FORTRAN_LDFLAGS=""
NATIVE_CFLAGS="-O3 -march=native -mavx2"
OPENMP_CFLAGS=""
OPENMP_LDFLAGS=""

if [ "${NOT_STISLA_ENABLE_AVX512:-0}" = "1" ]; then
    NATIVE_CFLAGS="$NATIVE_CFLAGS -mavx512f -mavx512dq"
fi

if [ "${NOT_STISLA_ENABLE_OPENMP:-1}" = "1" ]; then
    OPENMP_CFLAGS="-fopenmp"
    OPENMP_LDFLAGS="-fopenmp"
fi

if [ "${NOT_STISLA_ENABLE_FORTRAN:-0}" = "1" ]; then
    if ! command -v gfortran >/dev/null 2>&1; then
        echo "NOT_STISLA_ENABLE_FORTRAN=1 requires gfortran"
        exit 1
    fi

    echo "Building optional Fortran batch backend..."
    gfortran -O3 -shared -fPIC -fopenmp -Jfortran \
        fortran/not_stisla_batch.f90 \
        -o fortran/libnot_stisla_batch.so || { echo "Fortran backend build failed"; exit 1; }

    FORTRAN_CFLAGS="-DNOT_STISLA_ENABLE_FORTRAN"
    ROOT_FORTRAN_LDFLAGS="-L./fortran -lnot_stisla_batch -Wl,-rpath,\$ORIGIN/fortran"
    BENCHMARK_FORTRAN_LDFLAGS="-L./fortran -lnot_stisla_batch -Wl,-rpath,\$ORIGIN/../fortran"
fi

# Clean up previous build artifacts
rm -f *.o test_enhanced test_fortran_backend test_auto_backend test_telemetry_processor_perf benchmarks/dsmil_benchmark benchmarks/performance_proof scripts/compare_search_auto

# Compile source files for test_enhanced
echo "Compiling src/not_stisla.c..."
gcc $NATIVE_CFLAGS $OPENMP_CFLAGS -Wall -Wextra -Werror=implicit-function-declaration -I./include $FORTRAN_CFLAGS -c src/not_stisla.c || { echo "not_stisla.c compilation failed"; exit 1; }

echo "Compiling src/dsmil_not_stisla_wrapper.c..."
gcc -O3 -march=native -mavx2 -Wall -Wextra -I./include -c src/dsmil_not_stisla_wrapper.c || { echo "dsmil_not_stisla_wrapper.c compilation failed"; exit 1; }

echo "Compiling src/dsmil_telemetry_processor.c..."
gcc -O3 -march=native -mavx2 -Wall -Wextra -I./include -c src/dsmil_telemetry_processor.c || { echo "dsmil_telemetry_processor.c compilation failed"; exit 1; }

echo "Compiling tests/dsmil_integration_test.c..."
gcc -O3 -march=native -mavx2 -Wall -Wextra -I./include -c tests/dsmil_integration_test.c || { echo "dsmil_integration_test.c compilation failed"; exit 1; }

echo "Compiling tests/test_fortran_backend.c..."
gcc -O3 -march=native -Wall -Wextra -I./include $FORTRAN_CFLAGS -c tests/test_fortran_backend.c || { echo "test_fortran_backend.c compilation failed"; exit 1; }

echo "Compiling tests/test_auto_backend.c..."
gcc -O3 -march=native $OPENMP_CFLAGS -Wall -Wextra -I./include $FORTRAN_CFLAGS -c tests/test_auto_backend.c || { echo "test_auto_backend.c compilation failed"; exit 1; }

echo "Compiling tests/test_telemetry_processor_perf.c..."
gcc -O3 -march=native -mavx2 -Wall -Wextra -I./include -c tests/test_telemetry_processor_perf.c || { echo "test_telemetry_processor_perf.c compilation failed"; exit 1; }

# Link test_enhanced
echo "Linking test_enhanced..."
gcc -o test_enhanced not_stisla.o dsmil_not_stisla_wrapper.o dsmil_telemetry_processor.o dsmil_integration_test.o -lm $OPENMP_LDFLAGS $ROOT_FORTRAN_LDFLAGS || { echo "test_enhanced linking failed"; exit 1; }

echo "Linking test_fortran_backend..."
gcc -o test_fortran_backend not_stisla.o test_fortran_backend.o -lm $OPENMP_LDFLAGS $ROOT_FORTRAN_LDFLAGS || { echo "test_fortran_backend linking failed"; exit 1; }

echo "Linking test_auto_backend..."
gcc -o test_auto_backend not_stisla.o test_auto_backend.o -lm $OPENMP_LDFLAGS $ROOT_FORTRAN_LDFLAGS || { echo "test_auto_backend linking failed"; exit 1; }

echo "Linking test_telemetry_processor_perf..."
gcc -o test_telemetry_processor_perf not_stisla.o dsmil_not_stisla_wrapper.o dsmil_telemetry_processor.o test_telemetry_processor_perf.o -lm $OPENMP_LDFLAGS $ROOT_FORTRAN_LDFLAGS || { echo "test_telemetry_processor_perf linking failed"; exit 1; }

# Compile source files for benchmarks
echo "Compiling benchmarks/dsmil_benchmark.c..."
gcc -O3 -march=native -mavx2 -Wall -Wextra -I./include -c benchmarks/dsmil_benchmark.c || { echo "dsmil_benchmark.c compilation failed"; exit 1; }

echo "Compiling benchmarks/performance_proof.c..."
gcc -O3 -march=native -mavx2 -Wall -Wextra -I./include -c benchmarks/performance_proof.c || { echo "performance_proof.c compilation failed"; exit 1; }

# Link benchmarks
echo "Linking benchmarks/dsmil_benchmark..."
gcc -o benchmarks/dsmil_benchmark dsmil_benchmark.o not_stisla.o dsmil_not_stisla_wrapper.o dsmil_telemetry_processor.o -lm $OPENMP_LDFLAGS $BENCHMARK_FORTRAN_LDFLAGS || { echo "dsmil_benchmark linking failed"; exit 1; }

echo "Linking benchmarks/performance_proof..."
gcc -o benchmarks/performance_proof performance_proof.o not_stisla.o dsmil_not_stisla_wrapper.o dsmil_telemetry_processor.o -lm $OPENMP_LDFLAGS $BENCHMARK_FORTRAN_LDFLAGS || { echo "performance_proof linking failed"; exit 1; }

echo "Building scripts/compare_search_auto..."
gcc $NATIVE_CFLAGS $OPENMP_CFLAGS -Wall -Wextra -I. -I./include -DNOT_STISLA_BENCH_AUTO=1 $FORTRAN_CFLAGS \
    scripts/compare_search.c not_stisla.o \
    -lm $OPENMP_LDFLAGS $BENCHMARK_FORTRAN_LDFLAGS \
    -o scripts/compare_search_auto || { echo "compare_search_auto build failed"; exit 1; }

echo "Native x86_64 build completed successfully!"

# Clean up .o files (optional, but good practice for a clean build script)
rm -f *.o
