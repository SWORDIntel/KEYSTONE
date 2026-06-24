#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BENCH_BIN="${KEYSTONE_BENCH_BIN:-$ROOT_DIR/benchmarks/dsmil_benchmark}"
ARCHIVE_DIR="$ROOT_DIR/bench_results/large_archive"
ARCHIVE_PATH="$ARCHIVE_DIR/large_benchmark_data.tar.zst"

if [[ ! -x "$BENCH_BIN" ]]; then
    echo "benchmark binary is not executable: $BENCH_BIN" >&2
    echo "build it first, for example: KEYSTONE_ENABLE_FORTRAN=1 KEYSTONE_ENABLE_TAR_ZST=1 ./scripts/build_native.sh" >&2
    exit 1
fi

mkdir -p "$ARCHIVE_DIR"

if [[ ! -f "$ARCHIVE_PATH" ]]; then
    echo "Generating large benchmark archive at $ARCHIVE_PATH..."
    
    LARGE_CSV="$ARCHIVE_DIR/telemetry_large.csv"
    echo "Writing 1,000,000 rows to $LARGE_CSV..."
    echo "timestamp,device,value" > "$LARGE_CSV"
    
    # Use awk for fast generation
    awk 'BEGIN { for(i=1; i<=1000000; i++) printf "%d,dev_01,%.2f\n", i*10, rand()*100 }' > "$LARGE_CSV"
    
    echo "Compressing into .tar.zst..."
    if command -v tar >/dev/null 2>&1 && tar --zstd -cf /dev/null /dev/null 2>/dev/null; then
        tar --zstd -cf "$ARCHIVE_PATH" -C "$ARCHIVE_DIR" telemetry_large.csv
    elif command -v zstd >/dev/null 2>&1; then
        TAR_TMP="$ARCHIVE_DIR/tmp.tar"
        tar -cf "$TAR_TMP" -C "$ARCHIVE_DIR" telemetry_large.csv
        zstd -f -19 "$TAR_TMP" -o "$ARCHIVE_PATH"
        rm -f "$TAR_TMP"
    else
        echo "Error: neither tar --zstd nor zstd command available" >&2
        exit 1
    fi
    
    # Clean up raw csv to save space
    rm "$LARGE_CSV"
fi

echo "Running large archive benchmark..."
"$BENCH_BIN" --tar-zst "$ARCHIVE_PATH" --tar-zst-member "telemetry_large.csv"

echo "Archive benchmark completed."
