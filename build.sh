#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

echo "Building KEYSTONE library..."

cpu_has() {
    grep -m1 '^flags' /proc/cpuinfo | grep -qw "$1"
}

BASE_CFLAGS="${KEYSTONE_BASE_CFLAGS:--O3 -march=native}"
SIMD_CFLAGS=""

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

OPENMP_CFLAGS=""
OPENMP_LDFLAGS=""
if [ "${KEYSTONE_ENABLE_OPENMP:-1}" = "1" ]; then
    OPENMP_CFLAGS="-fopenmp"
    OPENMP_LDFLAGS="-fopenmp"
fi

FORTRAN_CFLAGS=""
FORTRAN_LDFLAGS=""
if [ "${KEYSTONE_ENABLE_FORTRAN:-1}" = "1" ]; then
    if command -v gfortran >/dev/null 2>&1; then
        echo "Building optional Fortran batch backend..."
        mkdir -p fortran
        gfortran -O3 -shared -fPIC -fopenmp -Jfortran \
            fortran/keystone_batch.f90 \
            -o fortran/libkeystone_batch.so
        FORTRAN_CFLAGS="-DKEYSTONE_ENABLE_FORTRAN"
        FORTRAN_LDFLAGS="-L./fortran -lkeystone_batch -Wl,-rpath,\$ORIGIN/fortran"
    else
        echo "gfortran not found; skipping Fortran backend."
    fi
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

mkdir -p .build_lib

# Compile library sources with -fPIC
echo "Compiling src/keystone.c..."
gcc $NATIVE_CFLAGS $OPENMP_CFLAGS -Wall -Wextra -fPIC -I./include $FORTRAN_CFLAGS \
    -c src/keystone.c -o .build_lib/keystone.o

echo "Compiling src/dsmil_keystone_wrapper.c..."
gcc $NATIVE_CFLAGS $OPENMP_CFLAGS -Wall -Wextra -fPIC -I./include $TAR_ZST_CFLAGS \
    -c src/dsmil_keystone_wrapper.c -o .build_lib/dsmil_keystone_wrapper.o

echo "Compiling src/dsmil_telemetry_processor.c..."
gcc $NATIVE_CFLAGS $OPENMP_CFLAGS -Wall -Wextra -fPIC -I./include $TAR_ZST_CFLAGS \
    -c src/dsmil_telemetry_processor.c -o .build_lib/dsmil_telemetry_processor.o

LIB_OBJS=".build_lib/keystone.o .build_lib/dsmil_keystone_wrapper.o .build_lib/dsmil_telemetry_processor.o"

if [ -n "$TAR_ZST_CFLAGS" ]; then
    echo "Compiling src/keystone_tar_zst.c..."
    gcc $NATIVE_CFLAGS -Wall -Wextra -fPIC -I./include $TAR_ZST_CFLAGS \
        -c src/keystone_tar_zst.c -o .build_lib/keystone_tar_zst.o
    LIB_OBJS="$LIB_OBJS .build_lib/keystone_tar_zst.o"
fi

echo "Linking libkeystone.so..."
gcc -shared -fPIC -o libkeystone.so $LIB_OBJS -lm $OPENMP_LDFLAGS $FORTRAN_LDFLAGS $TAR_ZST_LDFLAGS

echo "Library build complete: $(pwd)/libkeystone.so"

# Clean up temporary build objects
rm -rf .build_lib
