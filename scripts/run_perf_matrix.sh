#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BENCH_BIN="${NOT_STISLA_BENCH_BIN:-$ROOT_DIR/scripts/compare_search_auto}"
OUT_ROOT="${NOT_STISLA_MATRIX_OUT:-$ROOT_DIR/bench_results/perf_matrix_$(date -u +%Y%m%dT%H%M%SZ)}"

DATA_SIZES="${NOT_STISLA_MATRIX_N:-1000000}"
QUERY_SIZES="${NOT_STISLA_MATRIX_QUERIES:-512 8192 32768 200000}"
HIT_RATES="${NOT_STISLA_MATRIX_HIT_RATES:-100 75 50 25 0}"
GAP_PROFILES="${NOT_STISLA_MATRIX_GAPS:-dense:1:0 sparse:16:0 jitter:8:8}"
STRIDES="${NOT_STISLA_MATRIX_STRIDES:-1 17 257}"
RUNS="${NOT_STISLA_MATRIX_RUNS:-7}"
THREADS="${NOT_STISLA_MATRIX_THREADS:-${OMP_NUM_THREADS:-16}}"
LIMIT="${NOT_STISLA_MATRIX_LIMIT:-0}"

if [[ ! -x "$BENCH_BIN" ]]; then
    echo "benchmark binary is not executable: $BENCH_BIN" >&2
    echo "build it first, for example: NOT_STISLA_ENABLE_FORTRAN=1 ./scripts/build_native.sh" >&2
    exit 1
fi

mkdir -p "$OUT_ROOT"
SUMMARY_CSV="$OUT_ROOT/matrix_summary.csv"
RUN_LOG="$OUT_ROOT/matrix.log"

printf 'case_id,profile,n,queries,runs,hit_rate_pct,data_gap,data_gap_jitter,query_stride,threads,csv_path,status\n' > "$SUMMARY_CSV"
: > "$RUN_LOG"

case_id=0
completed=0

for n in $DATA_SIZES; do
    for queries in $QUERY_SIZES; do
        for hit_rate in $HIT_RATES; do
            for gap_profile in $GAP_PROFILES; do
                IFS=':' read -r gap_name data_gap data_gap_jitter <<< "$gap_profile"
                if [[ -z "${gap_name:-}" || -z "${data_gap:-}" || -z "${data_gap_jitter:-}" ]]; then
                    echo "invalid gap profile '$gap_profile'; expected name:gap:jitter" >&2
                    exit 1
                fi

                for stride in $STRIDES; do
                    case_id=$((case_id + 1))
                    if [[ "$LIMIT" -gt 0 && "$completed" -ge "$LIMIT" ]]; then
                        echo "limit reached: $completed cases"
                        echo "summary_csv=$SUMMARY_CSV"
                        echo "run_log=$RUN_LOG"
                        exit 0
                    fi

                    profile="n${n}_q${queries}_h${hit_rate}_${gap_name}_g${data_gap}_j${data_gap_jitter}_s${stride}_t${THREADS}"
                    csv_path="$OUT_ROOT/${case_id}_${profile}.csv"
                    status="ok"

                    {
                        printf '[%s] case=%d profile=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$case_id" "$profile"
                        printf '  csv=%s\n' "$csv_path"
                    } >> "$RUN_LOG"

                    if ! OMP_NUM_THREADS="$THREADS" \
                        NOT_STISLA_PROFILE="$profile" \
                        NOT_STISLA_N="$n" \
                        NOT_STISLA_QUERIES="$queries" \
                        NOT_STISLA_RUNS="$RUNS" \
                        NOT_STISLA_HIT_RATE_PCT="$hit_rate" \
                        NOT_STISLA_DATA_GAP="$data_gap" \
                        NOT_STISLA_DATA_GAP_JITTER="$data_gap_jitter" \
                        NOT_STISLA_QUERY_STRIDE="$stride" \
                        "$BENCH_BIN" > "$csv_path" 2>> "$RUN_LOG"; then
                        status="failed"
                    fi

                    printf '%d,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
                        "$case_id" "$profile" "$n" "$queries" "$RUNS" "$hit_rate" \
                        "$data_gap" "$data_gap_jitter" "$stride" "$THREADS" "$csv_path" "$status" \
                        >> "$SUMMARY_CSV"

                    completed=$((completed + 1))
                    echo "case $case_id $status: $csv_path"

                    if [[ "$status" != "ok" ]]; then
                        echo "benchmark failed; see $RUN_LOG" >&2
                        exit 1
                    fi
                done
            done
        done
    done
done

echo "completed cases: $completed"
echo "summary_csv=$SUMMARY_CSV"
echo "run_log=$RUN_LOG"
