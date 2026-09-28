#!/usr/bin/env bash
# run_sanitizers.sh — sanitizer matrix for the KEYSTONE C test suites.
#
# Builds every test suite in an isolated temp dir (in-place objects are never
# touched) and runs them either:
#   asan     — compiled with -fsanitize=address,undefined, leak detection on
#   valgrind — plain -O1 build executed under valgrind with --error-exitcode
#
# Usage (wired from the Makefile):
#   run_sanitizers.sh <asan|valgrind> <cc> <extra-cflags> <ldflags> <src-or-test...>
#
# Sources under tests/ are treated as test mains; everything else is library
# source compiled into every binary. Fortran-backed tests are filtered out by
# the Makefile: gfortran objects are not sanitizer-instrumented, so including
# them would test nothing and could false-negative. Instrumented runs also set
# KEYSTONE_SKIP_PERF_GATES=1 (slowed executions must not flake the perf
# ceiling; the ceiling is also compiled out under ASan/UBSan). Calibration
# policy asserts are compiled out under sanitizers in the test itself.
set -euo pipefail

MODE="${1:?usage: run_sanitizers.sh <asan|valgrind> <cc> <cflags> <ldflags> <src...>}"
CC_BIN="${2:?compiler}"
EXTRA_CFLAGS="${3:?extra cflags}"
LINKFLAGS="${4:?ldflags}"
shift 4

command -v "$CC_BIN" >/dev/null 2>&1 || { echo "sanitizers: compiler '$CC_BIN' not found" >&2; exit 2; }
[ "$MODE" = "asan" ] || [ "$MODE" = "valgrind" ] || { echo "sanitizers: unknown mode '$MODE'" >&2; exit 2; }
if [ "$MODE" = "valgrind" ]; then
    command -v valgrind >/dev/null 2>&1 || { echo "sanitizers: valgrind not found" >&2; exit 2; }
fi

BUILD="$(mktemp -d)"
trap 'rm -rf "$BUILD"' EXIT

COMMON_FLAGS="-O1 -g -fno-omit-frame-pointer -Wall -Wextra -mno-avx2 -mno-avx512f"
if [ "$MODE" = "asan" ]; then
    BUILD_FLAGS="$COMMON_FLAGS -fsanitize=address,undefined"
    LINK_FLAGS="-fsanitize=address,undefined $LINKFLAGS"
else
    BUILD_FLAGS="$COMMON_FLAGS"
    LINK_FLAGS="$LINKFLAGS"
fi

LIB_OBJS=""
TESTS=()
for src in "$@"; do
    obj="$BUILD/$(basename "${src%.c}").o"
    # shellcheck disable=SC2086
    "$CC_BIN" $BUILD_FLAGS -I./include $EXTRA_CFLAGS -c "$src" -o "$obj" || { echo "sanitizers: compile failed: $src" >&2; exit 1; }
    case "$src" in
        tests/*) TESTS+=("${src%.c}") ;;
        *) LIB_OBJS="$LIB_OBJS $obj" ;;
    esac
done

if [ "${#TESTS[@]}" -eq 0 ]; then
    echo "sanitizers: no test sources provided" >&2
    exit 2
fi

FAILED=0
for t in "${TESTS[@]}"; do
    name="$(basename "$t")"
    bin="$BUILD/$name"
    # shellcheck disable=SC2086
    "$CC_BIN" -o "$bin" $LIB_OBJS "$BUILD/$name.o" $LINK_FLAGS || { echo "sanitizers: link failed: $t" >&2; FAILED=1; continue; }
    printf '==> %s (%s)\n' "$name" "$MODE"
    if [ "$MODE" = "asan" ]; then
        if ! (ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 \
              UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
              "$bin" >"$BUILD/$name.log" 2>&1); then
            echo "    FAIL — sanitizer findings or crash:" >&2
            grep -m5 -E "ERROR: AddressSanitizer|runtime error|LeakSanitizer|SUMMARY" "$BUILD/$name.log" >&2 || tail -5 "$BUILD/$name.log" >&2
            FAILED=1
        else
            echo "    clean"
        fi
    else
        if ! (KEYSTONE_SKIP_PERF_GATES=1 \
              valgrind -q --error-exitcode=99 --leak-check=full \
              --errors-for-leak-kinds=definite,indirect \
              "$bin" >"$BUILD/$name.log" 2>&1); then
            echo "    FAIL — valgrind findings:" >&2
            grep -m5 -E "Invalid|definitely lost|ERROR" "$BUILD/$name.log" >&2 || tail -5 "$BUILD/$name.log" >&2
            FAILED=1
        else
            echo "    clean"
        fi
    fi
done

if [ "$FAILED" -ne 0 ]; then
    echo "sanitizers[$MODE]: FAILURES DETECTED" >&2
    exit 1
fi
echo "sanitizers[$MODE]: all ${#TESTS[@]} suites clean"
