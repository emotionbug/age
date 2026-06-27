#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/wcoj-roadmap-gates.XXXXXX")
trap 'rm -rf "$tmpdir"' EXIT

pg_config=${PG_CONFIG:-/Users/emotionbug/IdeaProjects/postgres_proj/pg18release/bin/pg_config}
database=${PGDATABASE:-agebench}
runs=${WCOJ_ROADMAP_RUNS:-5}
star_sources=${WCOJ_ROADMAP_STAR_SOURCES:-4}
star_fanout=${WCOJ_ROADMAP_STAR_FANOUT:-4096}
cycle_vertices=${WCOJ_ROADMAP_CYCLE_VERTICES:-1024}
cycle_fanout=${WCOJ_ROADMAP_CYCLE_FANOUT:-64}
semijoin_fanout=${WCOJ_ROADMAP_SEMIJOIN_FANOUT:-128}
semiring_speedup_fanout=${WCOJ_ROADMAP_SEMIRING_SPEEDUP_FANOUT:-80}

min_dense_speedup=${WCOJ_ROADMAP_MIN_DENSE_SPEEDUP:-100}
min_cycle_speedup=${WCOJ_ROADMAP_MIN_CYCLE_SPEEDUP:-20}
min_semijoin_speedup=${WCOJ_ROADMAP_MIN_SEMIJOIN_SPEEDUP:-100}
min_semiring_speedup=${WCOJ_ROADMAP_MIN_SEMIRING_SPEEDUP:-100}
min_100x_geomean=${WCOJ_ROADMAP_MIN_100X_GEOMEAN:-100}
max_dense_overhead=${WCOJ_ROADMAP_MAX_DENSE_OVERHEAD:-1.25}

# Reference baseline from wcoj_survivor_batch_results.md. Override this when
# comparing against a freshly measured pre-batch build on the same hardware.
dense_baseline_ms=${WCOJ_ROADMAP_DENSE_BASELINE_MS:-2738.042}

completion_raw_plan_log=${WCOJ_ROADMAP_COMPLETION_RAW_PLAN_LOG:-}
semijoin_raw_plan_log=${WCOJ_ROADMAP_SEMIJOIN_RAW_PLAN_LOG:-}
semiring_speedup_raw_plan_log=${WCOJ_ROADMAP_SEMIRING_SPEEDUP_RAW_PLAN_LOG:-}
if [[ -n "$completion_raw_plan_log" ]]; then
    mkdir -p "$(dirname -- "$completion_raw_plan_log")"
    completion_out=$completion_raw_plan_log
else
    completion_out="$tmpdir/completion.out"
fi
completion_times="$tmpdir/completion-times.tsv"
if [[ -n "$semijoin_raw_plan_log" ]]; then
    mkdir -p "$(dirname -- "$semijoin_raw_plan_log")"
    semijoin_out=$semijoin_raw_plan_log
else
    semijoin_out="$tmpdir/semijoin.out"
fi
semijoin_times="$tmpdir/semijoin-times.tsv"
if [[ -n "$semiring_speedup_raw_plan_log" ]]; then
    mkdir -p "$(dirname -- "$semiring_speedup_raw_plan_log")"
    semiring_speedup_out=$semiring_speedup_raw_plan_log
else
    semiring_speedup_out="$tmpdir/semiring-speedup.out"
fi
semiring_speedup_times="$tmpdir/semiring-speedup-times.tsv"
: > "$completion_out"
: > "$semijoin_out"
: > "$semiring_speedup_out"

if [[ ! -x "$pg_config" ]]; then
    echo "PG_CONFIG is not executable: $pg_config" >&2
    exit 1
fi

configure=$("$pg_config" --configure)
if [[ ${WCOJ_ROADMAP_ALLOW_DEBUG_PG:-0} != 1 ]] &&
   [[ "$configure" == *"--enable-debug"* ||
      "$configure" == *"--enable-cassert"* ||
      "$configure" == *"-O0"* ]]; then
    echo "refusing to run roadmap gates against a debug/cassert/O0 PostgreSQL" >&2
    echo "$configure" >&2
    exit 1
fi

psql=${PSQL:-$("$pg_config" --bindir)/psql}
psql_base=("$psql" --set=ON_ERROR_STOP=1 --dbname="$database")

"${psql_base[@]}" --tuples-only --no-align --command='SELECT 1' >/dev/null

extract_times()
{
    awk '
    /^[[:alnum:]][^:]*: / {
        label = $0
        next
    }
    /Execution Time:/ && label != "" {
        seen[label]++
        if (seen[label] > 1)
            printf "%s\t%s\n", label, $(NF - 1)
    }
    ' "$1" > "$2"
}

median_for()
{
    local label=$1
    local file=$2

    awk -F '\t' -v wanted="$label" '$1 == wanted { print $2 }' "$file" |
        sort -n |
        awk -v label="$label" '
        { values[NR] = $1 }
        END {
            if (NR == 0) {
                printf "missing benchmark samples for %s\n", label > "/dev/stderr"
                exit 2
            }
            if (NR % 2 == 1)
                printf "%.6f", values[(NR + 1) / 2]
            else
                printf "%.6f", (values[NR / 2] + values[NR / 2 + 1]) / 2
        }'
}

ratio()
{
    awk -v numerator="$1" -v denominator="$2" 'BEGIN {
        if (denominator <= 0)
            exit 2
        printf "%.6f", numerator / denominator
    }'
}

geomean()
{
    awk 'BEGIN {
        if (ARGC <= 1)
            exit 2
        for (i = 1; i < ARGC; i++) {
            value = ARGV[i] + 0
            if (value <= 0)
                exit 2
            total += log(value)
        }
        printf "%.6f", exp(total / (ARGC - 1))
    }' "$@"
}

assert_at_least()
{
    local name=$1
    local actual=$2
    local minimum=$3

    awk -v name="$name" -v actual="$actual" -v minimum="$minimum" 'BEGIN {
        if (actual + 0 < minimum + 0) {
            printf "%s %.3fx is below %.3fx\n", name, actual, minimum > "/dev/stderr"
            exit 1
        }
    }'
}

assert_at_most()
{
    local name=$1
    local actual=$2
    local maximum=$3

    awk -v name="$name" -v actual="$actual" -v maximum="$maximum" 'BEGIN {
        if (actual + 0 > maximum + 0) {
            printf "%s %.3fx exceeds %.3fx\n", name, actual, maximum > "/dev/stderr"
            exit 1
        }
    }'
}

if [[ ${WCOJ_ROADMAP_SKIP_COMPLETION_SETUP:-0} != 1 ]]; then
    "${psql_base[@]}" \
        --set=star_sources="$star_sources" \
        --set=star_fanout="$star_fanout" \
        --set=cycle_vertices="$cycle_vertices" \
        --set=cycle_fanout="$cycle_fanout" \
        --file="$script_dir/wcoj_completion_setup.sql" >/dev/null
fi

"${psql_base[@]}" --set=runs="$runs" \
    --file="$script_dir/wcoj_completion_benchmark.sql" | tee "$completion_out"
extract_times "$completion_out" "$completion_times"

dense_merge_ms=$(median_for 'dense star: forced merge (expected count = star_fanout)' "$completion_times")
dense_auto_ms=$(median_for 'dense star: auto WCOJ' "$completion_times")
cycle_binary_ms=$(median_for 'late-rejection cycle: forced binary (expected count 0)' "$completion_times")
cycle_generic_ms=$(median_for 'late-rejection cycle: Generic Join' "$completion_times")

dense_speedup=$(ratio "$dense_baseline_ms" "$dense_merge_ms")
dense_overhead=$(ratio "$dense_auto_ms" "$dense_merge_ms")
cycle_speedup=$(ratio "$cycle_binary_ms" "$cycle_generic_ms")

assert_at_least "dense star survivor batching speedup" \
    "$dense_speedup" "$min_dense_speedup"
assert_at_most "dense star auto overhead" \
    "$dense_overhead" "$max_dense_overhead"
assert_at_least "late-rejection cycle speedup" \
    "$cycle_speedup" "$min_cycle_speedup"

if [[ ${WCOJ_ROADMAP_SKIP_SEMIJOIN_SETUP:-0} != 1 ]]; then
    "${psql_base[@]}" --set=fanout="$semijoin_fanout" \
        --file="$script_dir/wcoj_semijoin_setup.sql" >/dev/null
fi

"${psql_base[@]}" --set=runs="$runs" \
    --file="$script_dir/wcoj_semijoin_benchmark.sql" | tee "$semijoin_out"
extract_times "$semijoin_out" "$semijoin_times"

semijoin_binary_ms=$(median_for 'acyclic chain: forced binary' "$semijoin_times")
semijoin_generic_ms=$(median_for \
    'acyclic chain: auto Generic Join with semijoin reduction' \
    "$semijoin_times")
semijoin_speedup=$(ratio "$semijoin_binary_ms" "$semijoin_generic_ms")

assert_at_least "acyclic chain semijoin speedup" \
    "$semijoin_speedup" "$min_semijoin_speedup"

if [[ ${WCOJ_ROADMAP_SKIP_SEMIRING_SPEEDUP_SETUP:-0} != 1 ]]; then
    "${psql_base[@]}" --set=fanout="$semiring_speedup_fanout" \
        --file="$script_dir/wcoj_semiring_setup.sql" >/dev/null
fi

"${psql_base[@]}" --set=runs="$runs" \
    --file="$script_dir/wcoj_semiring_speedup_benchmark.sql" |
    tee "$semiring_speedup_out"
extract_times "$semiring_speedup_out" "$semiring_speedup_times"

semiring_binary_ms=$(median_for \
    'semiring count speedup: forced binary count' \
    "$semiring_speedup_times")
semiring_wcoj_ms=$(median_for \
    'semiring count speedup: WCOJ count' \
    "$semiring_speedup_times")
semiring_speedup=$(ratio "$semiring_binary_ms" "$semiring_wcoj_ms")

assert_at_least "semiring count speedup" \
    "$semiring_speedup" "$min_semiring_speedup"

speedup_100x_geomean=$(geomean "$dense_speedup" "$semijoin_speedup" \
                               "$semiring_speedup")
all_speedup_geomean=$(geomean "$dense_speedup" "$cycle_speedup" \
                              "$semijoin_speedup" "$semiring_speedup")

assert_at_least "three-workload 100x geometric mean" \
    "$speedup_100x_geomean" "$min_100x_geomean"

printf 'dense_star_speedup=%sx dense_auto_overhead=%sx cycle_speedup=%sx semijoin_speedup=%sx semiring_count_speedup=%sx three_workload_100x_geomean=%sx all_speedup_geomean=%sx completion_raw_plan_log=%s semijoin_raw_plan_log=%s semiring_speedup_raw_plan_log=%s\n' \
       "$dense_speedup" "$dense_overhead" "$cycle_speedup" \
       "$semijoin_speedup" "$semiring_speedup" "$speedup_100x_geomean" \
       "$all_speedup_geomean" "$completion_out" "$semijoin_out" \
       "$semiring_speedup_out"
