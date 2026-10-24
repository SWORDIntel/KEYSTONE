#!/usr/bin/env bash
#
# build_vector_engine.sh - Build the KEYSTONE Vector Engine
#
# Auto-detects target and host SIMD. Explicit KEYSTONE_ISA or
# KEYSTONE_ENABLE_* settings may select a different target CPU.
# The scalar kernel is ALWAYS compiled — it's the fallback floor.
# CUDA is built as a separate .so if nvcc is available.
# VPU support is compiled in if KEYSTONE_HAVE_VPU=1 is set.
#
# Usage:
#   ./build_vector_engine.sh                    # auto-detect everything
#   KEYSTONE_NO_CUDA=1 ./build_vector_engine.sh # skip CUDA
#   KEYSTONE_HAVE_VPU=1 ./build_vector_engine.sh # enable VPU kernel
#   KEYSTONE_ISA=avx ./build_vector_engine.sh          # AVX1 target
#   KEYSTONE_ENABLE_AVX512=1 ./build_vector_engine.sh # cross-build AVX-512
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

CC=${CC:-cc}
CFLAGS_BASE="${CFLAGS_BASE:--O2 -fPIC -std=c11 -Wall -Wextra -I../include -I.}"
OBJS=""
HAVE_CUDA=0
HAVE_VPU=0

# Check if VPU support is requested
if [ "${KEYSTONE_HAVE_VPU:-0}" = "1" ]; then
    HAVE_VPU=1
fi

# A compiler accepting -mavx2 says nothing about the CPU running this build.
# Auto mode uses host features only for a compiler targeting this host.
TARGET_ARCH="$($CC -dumpmachine | cut -d- -f1)"
HOST_ARCH="$(uname -m)"
HOST_CPU_FLAGS="${HOST_CPU_FLAGS:-$(lscpu 2>/dev/null | awk -F: '/^(Flags|Features):/{sub(/^ /, "", $2); print $2; exit}')}"
if [ "$TARGET_ARCH" != "$HOST_ARCH" ]; then
    HOST_CPU_FLAGS=""
fi

has_host_flags() {
    local flag
    for flag in "$@"; do
        case " $HOST_CPU_FLAGS " in
            *" $flag "*) ;;
            *) return 1 ;;
        esac
    done
}

try_compile_flags() {
    printf 'int main(void){return 0;}\n' |
        $CC $CFLAGS_BASE "$@" -x c -fsyntax-only - 2>/dev/null
}

WANT_SSE42=0 WANT_AVX=0 WANT_AVX2=0 WANT_AVX512=0 WANT_NEON=0
case "${KEYSTONE_ISA:-auto}" in
    auto|native)
        if [ "$TARGET_ARCH" = x86_64 ] || [[ "$TARGET_ARCH" =~ ^i[3-6]86$ ]]; then
            has_host_flags sse4_1 sse4_2 && WANT_SSE42=1
            has_host_flags avx && WANT_AVX=1
            has_host_flags avx2 fma && WANT_AVX2=1
            has_host_flags avx512f avx512dq avx512bw avx512vl fma && WANT_AVX512=1
        elif [ "$TARGET_ARCH" = aarch64 ]; then
            has_host_flags asimd && WANT_NEON=1
        fi
        ;;
    scalar) ;;
    avx) WANT_SSE42=1 WANT_AVX=1 ;;
    avx2|avx_vnni) WANT_SSE42=1 WANT_AVX=1 WANT_AVX2=1 ;;
    avx512|amx) WANT_SSE42=1 WANT_AVX=1 WANT_AVX2=1 WANT_AVX512=1 ;;
    neon|sve) WANT_NEON=1 ;;
    *) echo "Unknown KEYSTONE_ISA=${KEYSTONE_ISA}" >&2; exit 2 ;;
esac

if [ "${KEYSTONE_ISA:-auto}" != scalar ]; then
    WANT_SSE42="${KEYSTONE_ENABLE_SSE42:-$WANT_SSE42}"
    WANT_AVX="${KEYSTONE_ENABLE_AVX:-$WANT_AVX}"
    WANT_AVX2="${KEYSTONE_ENABLE_AVX2:-$WANT_AVX2}"
    WANT_AVX512="${KEYSTONE_ENABLE_AVX512:-$WANT_AVX512}"
    WANT_NEON="${KEYSTONE_ENABLE_NEON:-$WANT_NEON}"
fi

for wanted in "$WANT_SSE42" "$WANT_AVX" "$WANT_AVX2" "$WANT_AVX512" "$WANT_NEON"; do
    case "$wanted" in 0|1) ;; *) echo "ISA switches must be 0 or 1" >&2; exit 2 ;; esac
done
if [ "$WANT_NEON" = 1 ] && [ "$TARGET_ARCH" != aarch64 ]; then
    echo "NEON requires an AArch64 compiler" >&2; exit 2
fi
if { [ "$WANT_SSE42" = 1 ] || [ "$WANT_AVX" = 1 ] ||
     [ "$WANT_AVX2" = 1 ] || [ "$WANT_AVX512" = 1 ]; } &&
   [ "$TARGET_ARCH" != x86_64 ] && ! [[ "$TARGET_ARCH" =~ ^i[3-6]86$ ]]; then
    echo "x86 SIMD requires an x86 compiler" >&2; exit 2
fi

echo "=== KEYSTONE Vector Engine Build ==="
echo "Compiler: $CC"
echo "Base flags: $CFLAGS_BASE"
echo "Target: $TARGET_ARCH  ISA: ${KEYSTONE_ISA:-auto}"

# ---- Level 8: SCALAR — ALWAYS COMPILED, NO CONDITIONS ----
echo "[1/8] Compiling scalar kernel (always)..."
$CC $CFLAGS_BASE -c kernels_scalar.c -o kernels_scalar.o
OBJS="$OBJS kernels_scalar.o"

# ---- Level 5: SSE4.2 ----
if [ "$WANT_SSE42" = 1 ]; then
    try_compile_flags -msse4.2 || { echo "Compiler lacks SSE4.2 support" >&2; exit 2; }
    echo "[2/8] Compiling SSE4.2 kernel..."
    $CC $CFLAGS_BASE -msse4.2 -c kernels_sse42.c -o kernels_sse42.o
    OBJS="$OBJS kernels_sse42.o"
else
    echo "[2/8] SSE4.2 not selected — skipping"
fi

# ---- Level 4: AVX (float-only, no AVX2 needed) ----
if [ "$WANT_AVX" = 1 ]; then
    try_compile_flags -mavx || { echo "Compiler lacks AVX support" >&2; exit 2; }
    echo "[3/8] Compiling AVX kernel..."
    $CC $CFLAGS_BASE -mavx -c kernels_avx.c -o kernels_avx.o
    OBJS="$OBJS kernels_avx.o"
else
    echo "[3/8] AVX not selected — skipping"
fi

# ---- Level 3: AVX2 + FMA ----
if [ "$WANT_AVX2" = 1 ]; then
    try_compile_flags -mavx2 -mfma || { echo "Compiler lacks AVX2/FMA support" >&2; exit 2; }
    echo "[4/8] Compiling AVX2 kernel..."
    $CC $CFLAGS_BASE -mavx2 -mfma -c kernels_avx2.c -o kernels_avx2.o
    OBJS="$OBJS kernels_avx2.o"
else
    echo "[4/8] AVX2/FMA not selected — skipping"
fi

# ---- Level 2: AVX-512 ----
if [ "$WANT_AVX512" = 1 ]; then
    try_compile_flags -mavx512f -mavx512dq -mavx512bw -mavx512vl -mfma || {
        echo "Compiler lacks the AVX-512 feature group" >&2; exit 2;
    }
    echo "[5/8] Compiling AVX-512 kernel..."
    $CC $CFLAGS_BASE -mavx512f -mavx512dq -mavx512bw -mavx512vl -mfma -c kernels_avx512.c -o kernels_avx512.o
    OBJS="$OBJS kernels_avx512.o"
else
    echo "[5/8] AVX-512 not selected — skipping"
fi

# ---- Level 6: NEON (ARM only) ----
if [ "$WANT_NEON" = 1 ]; then
    try_compile_flags -march=armv8-a+simd || { echo "Compiler lacks NEON support" >&2; exit 2; }
    echo "[6/8] Compiling NEON kernel..."
    $CC $CFLAGS_BASE -march=armv8-a+simd -c kernels_neon.c -o kernels_neon.o
    OBJS="$OBJS kernels_neon.o"
else
    echo "[6/8] NEON not selected — skipping"
fi

# ---- Level 7: VPU (Myriad X, guarded) ----
if [ "$HAVE_VPU" = "1" ]; then
    echo "[7/8] Compiling VPU kernel (KEYSTONE_HAVE_VPU=1)..."
    $CC $CFLAGS_BASE -DKEYSTONE_HAVE_VPU=1 -c kernels_vpu.c -o kernels_vpu.o
    VPU_FLAG="-DKEYSTONE_HAVE_VPU=1"
    OBJS="$OBJS kernels_vpu.o"
else
    echo "[7/8] VPU support not requested (set KEYSTONE_HAVE_VPU=1 to enable)"
    VPU_FLAG=""
fi

# ---- Core files (compiled with NO ISA flags) ----
echo "[8/8] Compiling core engine files..."
$CC $CFLAGS_BASE $VPU_FLAG -c lsh.c -o lsh.o
$CC $CFLAGS_BASE $VPU_FLAG -c keystone_engine.c -o keystone_engine.o
$CC $CFLAGS_BASE $VPU_FLAG -c persist.c -o persist.o
OBJS="$OBJS lsh.o keystone_engine.o persist.o"

# ---- OpenMP (optional) ----
OMP_LIBS=""
OMP_FLAGS=""
if try_compile_flags -fopenmp; then
    OMP_LIBS="-fopenmp"
    OMP_FLAGS="-fopenmp"
    echo "  OpenMP: enabled (-fopenmp)"
elif pkg-config --exists omp 2>/dev/null; then
    OMP_LIBS="$(pkg-config --libs omp)"
    OMP_FLAGS="$(pkg-config --cflags omp)"
    echo "  OpenMP: enabled (pkg-config)"
else
    echo "  OpenMP: not available — serial mode"
fi

# Recompile engine with OpenMP if available
if [ -n "${OMP_FLAGS:-}" ]; then
    $CC $CFLAGS_BASE $VPU_FLAG $OMP_FLAGS -c keystone_engine.c -o keystone_engine.o
fi

# ---- Link core library ----
echo "Linking libkeystone_vector.so..."
$CC $CFLAGS_BASE -shared -o libkeystone_vector.so $OBJS -lm $OMP_LIBS -ldl

# ---- Level 1: CUDA (optional separate .so) ----
if [ "${KEYSTONE_NO_CUDA:-0}" != "1" ] && command -v nvcc >/dev/null 2>&1; then
    echo "Building CUDA module (libkeystone_cuda.so)..."
    if nvcc -O3 -shared -Xcompiler -fPIC keystone_cuda.cu -o libkeystone_cuda.so 2>/dev/null; then
        HAVE_CUDA=1
        echo "  CUDA module built successfully"
    else
        echo "  CUDA build failed — building CPU-only (OK)"
    fi
else
    echo "CUDA: skipped (no nvcc or KEYSTONE_NO_CUDA=1)"
fi

echo ""
echo "=== Build Complete ==="
echo "Library: $SCRIPT_DIR/libkeystone_vector.so"
echo "Backends compiled: scalar(always)$(echo "$OBJS" | grep -o 'avx512\|avx2\|avx\|sse42\|neon\|vpu' | sort -u | tr '\n' ' ')cuda=${HAVE_CUDA}"
echo ""
echo "Runtime dispatch will select the best available backend."
