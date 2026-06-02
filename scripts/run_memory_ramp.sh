#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BENCH_BIN="${KEYSTONE_BENCH_BIN:-$ROOT_DIR/scripts/compare_search_auto}"
OUT_ROOT="${KEYSTONE_MEMORY_OUT:-$ROOT_DIR/bench_results/memory_ramp_$(date -u +%Y%m%dT%H%M%SZ)}"

RAM_CAP_PCT="${KEYSTONE_RAM_CAP_PCT:-40}"
MEMORY_SIZES="${KEYSTONE_MEMORY_SIZES:-}"
FRACTIONS="${KEYSTONE_MEMORY_FRACTIONS:-5 10 25 40 60}"
QUERY_SIZES="${KEYSTONE_MEMORY_QUERIES:-200000}"
HIT_RATES="${KEYSTONE_MEMORY_HIT_RATES:-100 50 0}"
GAP_PROFILES="${KEYSTONE_MEMORY_GAPS:-dense:1:0 jitter:8:8}"
STRIDES="${KEYSTONE_MEMORY_STRIDES:-17}"
RUNS="${KEYSTONE_MEMORY_RUNS:-5}"
THREADS="${KEYSTONE_MEMORY_THREADS:-${OMP_NUM_THREADS:-16}}"
MODES="${KEYSTONE_MEMORY_MODES:-cold warm}"
WARMUP_RUNS="${KEYSTONE_MEMORY_WARMUP_RUNS:-1}"
LIMIT="${KEYSTONE_MEMORY_LIMIT:-0}"
SMOKE="${KEYSTONE_MEMORY_SMOKE:-0}"

usage() {
    cat <<EOF
Usage: $(basename "$0") [--smoke] [--out DIR]

Environment:
  KEYSTONE_RAM_CAP_PCT       RAM cap as percent of MemAvailable (default: 40)
  KEYSTONE_MEMORY_SIZES      Space-separated KEYSTONE_N values; skips derived sizes
  KEYSTONE_MEMORY_FRACTIONS  MemAvailable fractions for derived sizes (default: 5 10 25 40 60)
  KEYSTONE_MEMORY_QUERIES    Space-separated query counts (default: 200000)
  KEYSTONE_MEMORY_HIT_RATES  Space-separated hit-rate percentages (default: 100 50 0)
  KEYSTONE_MEMORY_GAPS       Space-separated name:gap:jitter profiles (default: dense:1:0 jitter:8:8)
  KEYSTONE_MEMORY_STRIDES    Space-separated query strides (default: 17)
  KEYSTONE_MEMORY_RUNS       Runs passed to compare_search_auto (default: 5)
  KEYSTONE_MEMORY_MODES      Space-separated modes: cold warm (default: cold warm)
  KEYSTONE_MEMORY_WARMUP_RUNS Warmup passes for warm mode (default: 1)
  KEYSTONE_MEMORY_LIMIT      Stop after this many cases; 0 means no limit
  KEYSTONE_MEMORY_OUT        Output directory
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --smoke)
            SMOKE=1
            shift
            ;;
        --out)
            if [[ $# -lt 2 ]]; then
                echo "--out requires a directory" >&2
                exit 2
            fi
            OUT_ROOT="$2"
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "unknown argument: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

if [[ ! -x "$BENCH_BIN" ]]; then
    echo "benchmark binary is not executable: $BENCH_BIN" >&2
    echo "build it first, for example: KEYSTONE_ENABLE_FORTRAN=1 ./scripts/build_native.sh" >&2
    exit 1
fi

if ! [[ "$RAM_CAP_PCT" =~ ^[0-9]+$ ]] || [[ "$RAM_CAP_PCT" -lt 1 || "$RAM_CAP_PCT" -gt 95 ]]; then
    echo "KEYSTONE_RAM_CAP_PCT must be an integer from 1 to 95; got '$RAM_CAP_PCT'" >&2
    exit 1
fi

if ! [[ "$WARMUP_RUNS" =~ ^[0-9]+$ ]]; then
    echo "KEYSTONE_MEMORY_WARMUP_RUNS must be a non-negative integer; got '$WARMUP_RUNS'" >&2
    exit 1
fi

for mode in $MODES; do
    case "$mode" in
        cold|warm)
            ;;
        *)
            echo "invalid KEYSTONE_MEMORY_MODES entry: '$mode'; expected cold or warm" >&2
            exit 1
            ;;
    esac
done

mem_available_kb="$(awk '/^MemAvailable:/ { print $2 }' /proc/meminfo)"
if [[ -z "${mem_available_kb:-}" || "$mem_available_kb" -le 0 ]]; then
    echo "could not read MemAvailable from /proc/meminfo" >&2
    exit 1
fi

mem_available_bytes=$((mem_available_kb * 1024))
cap_bytes=$((mem_available_bytes * RAM_CAP_PCT / 100))

# compare_search_auto allocates data plus query and batch buffers. Some search
# paths allocate an additional sorted batch, so keep query accounting conservative.
DATA_BYTES_PER_ITEM=8
QUERY_BYTES_PER_ITEM=56
SAFETY_PCT=110

estimate_bytes() {
    local n="$1"
    local queries="$2"
    local raw
    raw=$((n * DATA_BYTES_PER_ITEM + queries * QUERY_BYTES_PER_ITEM))
    echo $((raw * SAFETY_PCT / 100))
}

derive_sizes() {
    local queries="$1"
    local fraction target budget n
    for fraction in $FRACTIONS; do
        if ! [[ "$fraction" =~ ^[0-9]+$ ]] || [[ "$fraction" -lt 1 || "$fraction" -gt 95 ]]; then
            echo "invalid KEYSTONE_MEMORY_FRACTIONS entry: '$fraction'" >&2
            exit 1
        fi
        target=$((mem_available_bytes * fraction / 100))
        budget=$((target - queries * QUERY_BYTES_PER_ITEM * SAFETY_PCT / 100))
        if [[ "$budget" -le 0 ]]; then
            continue
        fi
        n=$((budget * 100 / (DATA_BYTES_PER_ITEM * SAFETY_PCT)))
        if [[ "$n" -gt 0 ]]; then
            echo "$n"
        fi
    done | awk '!seen[$0]++'
}

parse_time_metric() {
    local label="$1"
    local file="$2"
    awk -F': ' -v label="$label" '$1 ~ label { gsub(/^[ \t]+|[ \t]+$/, "", $2); print $2; exit }' "$file"
}

if [[ "$SMOKE" == "1" ]]; then
    MEMORY_SIZES="${KEYSTONE_MEMORY_SIZES:-4096}"
    QUERY_SIZES="${KEYSTONE_MEMORY_QUERIES:-1024}"
    HIT_RATES="${KEYSTONE_MEMORY_HIT_RATES:-100}"
    GAP_PROFILES="${KEYSTONE_MEMORY_GAPS:-dense:1:0}"
    STRIDES="${KEYSTONE_MEMORY_STRIDES:-17}"
    RUNS="${KEYSTONE_MEMORY_RUNS:-1}"
    MODES="${KEYSTONE_MEMORY_MODES:-cold}"
    LIMIT="${KEYSTONE_MEMORY_LIMIT:-1}"
fi

mkdir -p "$OUT_ROOT"
SUMMARY_CSV="$OUT_ROOT/memory_ramp_summary.csv"
RUN_LOG="$OUT_ROOT/memory_ramp.log"
ANALYSIS_MD="$OUT_ROOT/analysis_summary.md"

printf 'case_id,profile,n,queries,runs,bench_mode,warmup_runs,hit_rate_pct,data_gap,data_gap_jitter,query_stride,threads,estimated_bytes,estimated_gib,ram_cap_pct,cap_bytes,mem_available_bytes,benchmark_csv,time_metrics_path,max_rss_kb,major_faults,minor_faults,elapsed,status\n' > "$SUMMARY_CSV"
: > "$RUN_LOG"

TIME_BIN=""
if [[ -x /usr/bin/time ]]; then
    TIME_BIN="/usr/bin/time"
fi

{
    echo "# Memory Ramp Analysis Summary"
    echo
    echo "- started_utc: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "- benchmark: $BENCH_BIN"
    echo "- mem_available_bytes: $mem_available_bytes"
    echo "- ram_cap_pct: $RAM_CAP_PCT"
    echo "- cap_bytes: $cap_bytes"
    echo "- smoke: $SMOKE"
    echo "- modes: $MODES"
    echo "- warmup_runs: $WARMUP_RUNS"
    if [[ -n "$TIME_BIN" ]]; then
        echo "- time_metrics: $TIME_BIN -v"
    else
        echo "- time_metrics: unavailable"
    fi
    echo
    echo "## Cases"
} > "$ANALYSIS_MD"

case_id=0
completed=0

for queries in $QUERY_SIZES; do
    if ! [[ "$queries" =~ ^[0-9]+$ ]] || [[ "$queries" -le 0 ]]; then
        echo "invalid query count: '$queries'" >&2
        exit 1
    fi

    if [[ -n "$MEMORY_SIZES" ]]; then
        data_sizes="$MEMORY_SIZES"
    else
        data_sizes="$(derive_sizes "$queries")"
    fi

    if [[ -z "${data_sizes// }" ]]; then
        echo "no data sizes available for queries=$queries under current memory settings" >&2
        exit 1
    fi

    for n in $data_sizes; do
        if ! [[ "$n" =~ ^[0-9]+$ ]] || [[ "$n" -le 0 ]]; then
            echo "invalid KEYSTONE_N value: '$n'" >&2
            exit 1
        fi

        estimated_bytes="$(estimate_bytes "$n" "$queries")"
        estimated_gib="$(awk -v b="$estimated_bytes" 'BEGIN { printf "%.3f", b / 1024 / 1024 / 1024 }')"
        if [[ "$estimated_bytes" -gt "$cap_bytes" ]]; then
            {
                printf '[%s] skip n=%s queries=%s estimated_bytes=%s exceeds cap_bytes=%s\n' \
                    "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$n" "$queries" "$estimated_bytes" "$cap_bytes"
            } >> "$RUN_LOG"
            continue
        fi

        for hit_rate in $HIT_RATES; do
            for gap_profile in $GAP_PROFILES; do
                IFS=':' read -r gap_name data_gap data_gap_jitter <<< "$gap_profile"
                if [[ -z "${gap_name:-}" || -z "${data_gap:-}" || -z "${data_gap_jitter:-}" ]]; then
                    echo "invalid gap profile '$gap_profile'; expected name:gap:jitter" >&2
                    exit 1
                fi

                for stride in $STRIDES; do
                    for mode in $MODES; do
                        case_id=$((case_id + 1))
                        if [[ "$LIMIT" -gt 0 && "$completed" -ge "$LIMIT" ]]; then
                            echo "limit reached: $completed cases"
                            echo "summary_csv=$SUMMARY_CSV"
                            echo "analysis_summary=$ANALYSIS_MD"
                            echo "run_log=$RUN_LOG"
                            exit 0
                        fi

                        mode_warmup_runs=0
                        if [[ "$mode" == "warm" ]]; then
                            mode_warmup_runs="$WARMUP_RUNS"
                        fi

                        profile="mem_n${n}_q${queries}_h${hit_rate}_${gap_name}_g${data_gap}_j${data_gap_jitter}_s${stride}_t${THREADS}_m${mode}"
                        benchmark_csv="$OUT_ROOT/${case_id}_${profile}.csv"
                        time_metrics_path="$OUT_ROOT/${case_id}_${profile}.time.txt"
                        status="ok"

                        {
                            printf '[%s] case=%d profile=%s estimated_gib=%s mode=%s warmup_runs=%s\n' \
                                "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$case_id" "$profile" "$estimated_gib" "$mode" "$mode_warmup_runs"
                            printf '  benchmark_csv=%s\n' "$benchmark_csv"
                            printf '  time_metrics=%s\n' "$time_metrics_path"
                        } >> "$RUN_LOG"

                        if [[ -n "$TIME_BIN" ]]; then
                            if ! OMP_NUM_THREADS="$THREADS" \
                                KEYSTONE_PROFILE="$profile" \
                                KEYSTONE_BENCH_MODE="$mode" \
                                KEYSTONE_WARMUP_RUNS="$mode_warmup_runs" \
                                KEYSTONE_N="$n" \
                                KEYSTONE_QUERIES="$queries" \
                                KEYSTONE_RUNS="$RUNS" \
                                KEYSTONE_HIT_RATE_PCT="$hit_rate" \
                                KEYSTONE_DATA_GAP="$data_gap" \
                                KEYSTONE_DATA_GAP_JITTER="$data_gap_jitter" \
                                KEYSTONE_QUERY_STRIDE="$stride" \
                                "$TIME_BIN" -v -o "$time_metrics_path" "$BENCH_BIN" > "$benchmark_csv" 2>> "$RUN_LOG"; then
                                status="failed"
                            fi
                        else
                            : > "$time_metrics_path"
                            if ! OMP_NUM_THREADS="$THREADS" \
                                KEYSTONE_PROFILE="$profile" \
                                KEYSTONE_BENCH_MODE="$mode" \
                                KEYSTONE_WARMUP_RUNS="$mode_warmup_runs" \
                                KEYSTONE_N="$n" \
                                KEYSTONE_QUERIES="$queries" \
                                KEYSTONE_RUNS="$RUNS" \
                                KEYSTONE_HIT_RATE_PCT="$hit_rate" \
                                KEYSTONE_DATA_GAP="$data_gap" \
                                KEYSTONE_DATA_GAP_JITTER="$data_gap_jitter" \
                                KEYSTONE_QUERY_STRIDE="$stride" \
                                "$BENCH_BIN" > "$benchmark_csv" 2>> "$RUN_LOG"; then
                                status="failed"
                            fi
                        fi

                        max_rss_kb="$(parse_time_metric 'Maximum resident set size' "$time_metrics_path" || true)"
                        major_faults="$(parse_time_metric 'Major .*page faults' "$time_metrics_path" || true)"
                        minor_faults="$(parse_time_metric 'Minor .*page faults' "$time_metrics_path" || true)"
                        elapsed="$(parse_time_metric 'Elapsed .*wall clock' "$time_metrics_path" || true)"

                        printf '%d,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
                            "$case_id" "$profile" "$n" "$queries" "$RUNS" "$mode" "$mode_warmup_runs" "$hit_rate" \
                            "$data_gap" "$data_gap_jitter" "$stride" "$THREADS" "$estimated_bytes" \
                            "$estimated_gib" "$RAM_CAP_PCT" "$cap_bytes" "$mem_available_bytes" \
                            "$benchmark_csv" "$time_metrics_path" "${max_rss_kb:-}" "${major_faults:-}" \
                            "${minor_faults:-}" "${elapsed:-}" "$status" >> "$SUMMARY_CSV"

                        {
                            echo "- case $case_id: status=$status, mode=$mode, warmup_runs=$mode_warmup_runs, n=$n, queries=$queries, estimated_gib=$estimated_gib, benchmark_csv=$benchmark_csv, time_metrics=$time_metrics_path"
                        } >> "$ANALYSIS_MD"

                        completed=$((completed + 1))
                        echo "case $case_id $status: $benchmark_csv"

                        if [[ "$status" != "ok" ]]; then
                            echo "benchmark failed; see $RUN_LOG" >&2
                            exit 1
                        fi
                    done
                done
            done
        done
    done
done

echo "completed cases: $completed"
echo "summary_csv=$SUMMARY_CSV"
echo "analysis_summary=$ANALYSIS_MD"
echo "run_log=$RUN_LOG"
