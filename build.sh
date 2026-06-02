#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

echo "Building StiSorter library..."

cpu_has() {
    grep -m1 '^flags' /proc/cpuinfo | grep -qw "$1"
}

BASE_CFLAGS="${NOT_STISLA_BASE_CFLAGS:--O3 -march=native}"
SIMD_CFLAGS=""

if [ "${NOT_STISLA_FORCE_SCALAR:-0}" = "1" ]; then
    echo "Forcing scalar/SSE-safe build."
else
    if [ "${NOT_STISLA_ENABLE_AVX2:-auto}" != "0" ] && cpu_has avx2; then
        SIMD_CFLAGS="$SIMD_CFLAGS -mavx2"
    fi
    if [ "${NOT_STISLA_ENABLE_AVX512:-0}" = "1" ] && cpu_has avx512f && cpu_has avx512dq; then
        SIMD_CFLAGS="$SIMD_CFLAGS -mavx512f -mavx512dq"
    fi
fi

NATIVE_CFLAGS="${NOT_STISLA_NATIVE_CFLAGS:-$BASE_CFLAGS$SIMD_CFLAGS}"
echo "Using native CFLAGS: $NATIVE_CFLAGS"

OPENMP_CFLAGS=""
OPENMP_LDFLAGS=""
if [ "${NOT_STISLA_ENABLE_OPENMP:-1}" = "1" ]; then
    OPENMP_CFLAGS="-fopenmp"
    OPENMP_LDFLAGS="-fopenmp"
fi

FORTRAN_CFLAGS=""
FORTRAN_LDFLAGS=""
if [ "${NOT_STISLA_ENABLE_FORTRAN:-1}" = "1" ]; then
    if command -v gfortran >/dev/null 2>&1; then
        echo "Building optional Fortran batch backend..."
        mkdir -p fortran
        gfortran -O3 -shared -fPIC -fopenmp -Jfortran \
            fortran/not_stisla_batch.f90 \
            -o fortran/libnot_stisla_batch.so
        FORTRAN_CFLAGS="-DNOT_STISLA_ENABLE_FORTRAN"
        FORTRAN_LDFLAGS="-L./fortran -lnot_stisla_batch -Wl,-rpath,\$ORIGIN/fortran"
    else
        echo "gfortran not found; skipping Fortran backend."
    fi
fi

TAR_ZST_CFLAGS=""
TAR_ZST_LDFLAGS=""
if [ "${NOT_STISLA_ENABLE_TAR_ZST:-auto}" != "0" ]; then
    if pkg-config --exists libarchive libzstd 2>/dev/null; then
        echo "tar.zst support detected (libarchive + libzstd)"
        TAR_ZST_CFLAGS="-DNOT_STISLA_ENABLE_TAR_ZST"
        TAR_ZST_LDFLAGS="$(pkg-config --libs libarchive libzstd)"
    fi
fi

mkdir -p .build_lib

# Compile library sources with -fPIC
echo "Compiling src/not_stisla.c..."
gcc $NATIVE_CFLAGS $OPENMP_CFLAGS -Wall -Wextra -fPIC -I./include $FORTRAN_CFLAGS \
    -c src/not_stisla.c -o .build_lib/not_stisla.o

echo "Compiling src/dsmil_not_stisla_wrapper.c..."
gcc $NATIVE_CFLAGS $OPENMP_CFLAGS -Wall -Wextra -fPIC -I./include $TAR_ZST_CFLAGS \
    -c src/dsmil_not_stisla_wrapper.c -o .build_lib/dsmil_not_stisla_wrapper.o

echo "Compiling src/dsmil_telemetry_processor.c..."
gcc $NATIVE_CFLAGS $OPENMP_CFLAGS -Wall -Wextra -fPIC -I./include $TAR_ZST_CFLAGS \
    -c src/dsmil_telemetry_processor.c -o .build_lib/dsmil_telemetry_processor.o

LIB_OBJS=".build_lib/not_stisla.o .build_lib/dsmil_not_stisla_wrapper.o .build_lib/dsmil_telemetry_processor.o"

if [ -n "$TAR_ZST_CFLAGS" ]; then
    echo "Compiling src/not_stisla_tar_zst.c..."
    gcc $NATIVE_CFLAGS -Wall -Wextra -fPIC -I./include $TAR_ZST_CFLAGS \
        -c src/not_stisla_tar_zst.c -o .build_lib/not_stisla_tar_zst.o
    LIB_OBJS="$LIB_OBJS .build_lib/not_stisla_tar_zst.o"
fi

echo "Linking libstisorter.so..."
gcc -shared -fPIC -o libstisorter.so $LIB_OBJS -lm $OPENMP_LDFLAGS $FORTRAN_LDFLAGS $TAR_ZST_LDFLAGS

echo "Library build complete: $(pwd)/libstisorter.so"

# Clean up temporary build objects
rm -rf .build_lib
