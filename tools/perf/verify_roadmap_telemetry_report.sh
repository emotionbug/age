#!/usr/bin/env bash
set -Eeuo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
current_step="initialization"
plan_report=${ROADMAP_TELEMETRY_PLAN_REPORT:-}
print_plan_report=${ROADMAP_TELEMETRY_PRINT_PLAN_REPORT:-1}
secondary_checks=${ROADMAP_TELEMETRY_SECONDARY_CHECKS:-1}
print_failure_log=${ROADMAP_TELEMETRY_PRINT_FAILURE_LOG:-1}

if [[ -n "${ROADMAP_TELEMETRY_LOG_DIR:-}" ]]; then
    log_dir=$ROADMAP_TELEMETRY_LOG_DIR
    mkdir -p "$log_dir"
else
    timestamp=$(date +%Y%m%d-%H%M%S)
    log_dir=$(mktemp -d "${TMPDIR:-/tmp}/roadmap-telemetry-$timestamp.XXXXXX")
fi
if [[ -z "$plan_report" ]]; then
    plan_report="$log_dir/roadmap-full-plans.md"
fi

on_error()
{
    local status=$?

    printf 'roadmap raw-plan workflow failed unexpectedly: %s (exit %s)\n' \
           "$current_step" "$status" >&2
    printf 'roadmap telemetry logs retained in %s\n' "$log_dir" >&2
    exit "$status"
}
trap on_error ERR

print_check_failure_log()
{
    local log_path=$1
    local fallback_lines=$2

    if [[ "$print_failure_log" == 1 ]]; then
        cat "$log_path" >&2 || true
    else
        tail -n "$fallback_lines" "$log_path" >&2 || true
    fi
}

run_plan_check()
{
    local label=$1
    local script_name=$2
    local log_name=$3
    shift 3
    local script_path="$script_dir/$script_name"
    local log_path="$log_dir/$log_name"
    local status

    current_step="$label"
    if [[ ! -x "$script_path" ]]; then
        printf 'roadmap telemetry check script is not executable: %s\n' \
               "$script_path" >&2
        exit 1
    fi

    printf '%s: start\n' "$label"
    if env "$@" "$script_path" >"$log_path" 2>&1; then
        printf '%s: ok\n' "$label"
    else
        status=$?
        printf '%s: failed (exit %s)\n' "$label" "$status" >&2
        printf 'log: %s\n' "$log_path" >&2
        print_check_failure_log "$log_path" 60
        exit "$status"
    fi
}

first_match()
{
    local file=$1
    local pattern=$2

    awk -v pattern="$pattern" '
    $0 ~ pattern {
        sub(/^[[:space:]]+/, "", $0)
        print
        found = 1
        exit
    }
    END {
        if (!found)
            exit 1
    }' "$file"
}

emit_metric()
{
    local category=$1
    local description=$2
    local file=$3
    local pattern=$4
    local line

    if ! line=$(first_match "$file" "$pattern"); then
        printf 'missing secondary roadmap telemetry metric: %s: %s\n' \
               "$category" "$description" >&2
        printf 'searched log: %s\n' "$file" >&2
        exit 1
    fi

    printf '  %-34s %s\n' "$description:" "$line"
}

emit_category()
{
    printf '\n%s\n' "$1"
}

capture_plan_report()
{
    local capture_script="$script_dir/capture_roadmap_plans.sh"
    local capture_log="$log_dir/capture-roadmap-plans.log"
    local raw_plan_log_dir="$log_dir/raw-plan-logs"
    local status

    current_step="primary raw plan capture"
    mkdir -p "$(dirname "$plan_report")"

    if [[ ! -x "$capture_script" ]]; then
        printf 'roadmap raw plan capture script is not executable: %s\n' \
               "$capture_script" >&2
        exit 1
    fi

    printf 'primary raw plan capture: start\n'
    if ROADMAP_PLAN_PRINT_REPORT=0 \
        ROADMAP_PLAN_LOG_DIR="$raw_plan_log_dir" \
        ROADMAP_PLAN_REPORT="$plan_report" \
        "$capture_script" >"$capture_log" 2>&1; then
        printf 'primary raw plan capture: ok\n'
    else
        status=$?
        printf 'primary raw plan capture: failed (exit %s)\n' "$status" >&2
        printf 'log: %s\n' "$capture_log" >&2
        print_check_failure_log "$capture_log" 60
        exit "$status"
    fi

    printf '\nprimary raw plan artifact=%s\n' "$plan_report"
    if [[ "$print_plan_report" == 1 ]]; then
        printf '\n'
        cat "$plan_report"
    fi
}

wcoj_log="$log_dir/wcoj-roadmap-gates.log"
semiring_log="$log_dir/wcoj-semiring-gates.log"
preserve_log="$log_dir/generic-join-preservation.log"
reduction_log="$log_dir/generic-reduction-matrix.log"

printf 'roadmap raw-plan workflow: PG_CONFIG=%s PGDATABASE=%s PGHOST=%s PGPORT=%s\n' \
       "${PG_CONFIG:-<script default>}" \
       "${PGDATABASE:-agebench}" \
       "${PGHOST:-<libpq default>}" \
       "${PGPORT:-<libpq default>}"
printf 'roadmap telemetry logs: %s\n' "$log_dir"

capture_plan_report

if [[ "$secondary_checks" != 1 ]]; then
    current_step="complete"
    printf '\nsecondary roadmap telemetry checks skipped\n'
    printf 'primary_raw_plan_artifact=%s\n' "$plan_report"
    exit 0
fi

run_plan_check "WCOJ completion/cycle/semijoin plan run" \
               "verify_wcoj_roadmap_gates.sh" \
               "$(basename "$wcoj_log")" \
               "WCOJ_ROADMAP_COMPLETION_RAW_PLAN_LOG=$log_dir/raw-wcoj-completion-plans.log" \
               "WCOJ_ROADMAP_SEMIJOIN_RAW_PLAN_LOG=$log_dir/raw-wcoj-semijoin-plans.log"
run_plan_check "WCOJ semiring consumer plan run" \
               "verify_wcoj_semiring_gates.sh" \
               "$(basename "$semiring_log")" \
               "WCOJ_SEMIRING_RAW_PLAN_LOG=$log_dir/raw-wcoj-semiring-plans.log"
run_plan_check "Generic Join preservation plan run" \
               "verify_generic_join_preservation.sh" \
               "$(basename "$preserve_log")" \
               "GENERIC_JOIN_PRESERVE_RAW_PLAN_LOG=$log_dir/raw-generic-join-preservation-plans.log"
run_plan_check "Generic reduction matrix plan run" \
               "verify_generic_reduction_matrix.sh" \
               "$(basename "$reduction_log")" \
               "GENERIC_REDUCTION_RAW_PLAN_LOG=$log_dir/raw-generic-reduction-plans.log"

current_step="secondary telemetry extraction"

printf '\nsecondary roadmap telemetry checks\n'
printf 'logs=%s\n' "$log_dir"
printf 'primary_raw_plan_artifact=%s\n' "$plan_report"
printf 'raw_wcoj_completion_plans=%s\n' \
       "$log_dir/raw-wcoj-completion-plans.log"
printf 'raw_wcoj_semijoin_plans=%s\n' \
       "$log_dir/raw-wcoj-semijoin-plans.log"
printf 'raw_wcoj_semiring_plans=%s\n' \
       "$log_dir/raw-wcoj-semiring-plans.log"
printf 'raw_generic_join_preservation_plans=%s\n' \
       "$log_dir/raw-generic-join-preservation-plans.log"
printf 'raw_generic_reduction_plans=%s\n' \
       "$log_dir/raw-generic-reduction-plans.log"

emit_category "survivor batching"
emit_metric "survivor batching" "batch enabled" \
            "$wcoj_log" 'Batched Payload Materialization: true'
emit_metric "survivor batching" "survivor blocks" \
            "$wcoj_log" 'Survivor Blocks: [1-9][0-9]*'
emit_metric "survivor batching" "payload scan batches" \
            "$wcoj_log" 'Payload Scan Batches: [1-9][0-9]*'
emit_metric "survivor batching" "restart avoids" \
            "$wcoj_log" 'Payload Scan Restarts Avoided: [1-9][0-9]*'
emit_metric "survivor batching" "payload rows matched" \
            "$wcoj_log" 'Payload Rows Matched: [1-9][0-9]*'
emit_metric "survivor batching" "payload block rows" \
            "$wcoj_log" 'Peak Payload Block Rows: [1-9][0-9]*'
emit_metric "survivor batching" "payload budget overruns" \
            "$wcoj_log" 'Payload Block Budget Overruns: [0-9]+'
emit_metric "survivor batching" "payload bucket blocks spilled" \
            "$wcoj_log" 'Payload Bucket Blocks Spilled: [0-9]+'
emit_metric "survivor batching" "payload bucket rows spilled" \
            "$wcoj_log" 'Payload Bucket Rows Spilled: [0-9]+'
emit_metric "survivor batching" "payload bucket bytes spilled" \
            "$wcoj_log" 'Payload Bucket Bytes Spilled: [0-9]+ bytes'

emit_category "semijoin reduction"
emit_metric "semijoin reduction" "shape" \
            "$reduction_log" 'Reduction Shape: alpha-acyclic'
emit_metric "semijoin reduction" "order" \
            "$reduction_log" 'Reduction Order: leaf-peel'
emit_metric "semijoin reduction" "step count" \
            "$reduction_log" 'Yannakakis Step Count: [1-9][0-9]*'
emit_metric "semijoin reduction" "step plan" \
            "$reduction_log" 'Yannakakis Step Plan: bottom-up:'
emit_metric "semijoin reduction" "step filter mode" \
            "$reduction_log" \
            'Yannakakis Step Filter Mode: planned step-domain provider filter'
emit_metric "semijoin reduction" "passes" \
            "$reduction_log" 'Semijoin Reduction Passes: [1-9][0-9]*'
emit_metric "semijoin reduction" "steps applied" \
            "$reduction_log" 'Yannakakis Steps Applied: [1-9][0-9]*'
emit_metric "semijoin reduction" "rows removed" \
            "$reduction_log" 'Semijoin Rows Removed: [1-9][0-9]*'
emit_metric "semijoin reduction" "provider rows after" \
            "$reduction_log" 'Semijoin Provider Rows After: [1-9][0-9]*'
emit_metric "semijoin reduction" "final domain keys" \
            "$reduction_log" 'Semijoin Final Domain Keys: [1-9][0-9]*'

emit_category "factorized binding"
emit_metric "factorized binding" "source bag rows" \
            "$semiring_log" 'Source Bag Rows: [1-9][0-9]*'
emit_metric "factorized binding" "source bag keys" \
            "$semiring_log" 'Source Bag Keys: [1-9][0-9]*'
emit_metric "factorized binding" "shared source bags" \
            "$semiring_log" 'Factorized Binding Source Bags: [1-9][0-9]*'
emit_metric "factorized binding" "enumerators" \
            "$semiring_log" 'Factorized Binding Enumerators: [1-9][0-9]*'
emit_metric "factorized binding" "enumerator steps" \
            "$semiring_log" 'Shared Factor Enumerator Steps: [1-9][0-9]*'
emit_metric "factorized binding" "flat rows materialized" \
            "$semiring_log" 'Flat Rows Materialized: [0-9]+'
emit_metric "factorized binding" "peak factor memory" \
            "$semiring_log" 'Peak Factor Memory: [1-9][0-9]* bytes'

emit_category "trie ops"
emit_metric "trie ops" "provider mode" \
            "$preserve_log" \
            'Generic Join Provider Mode: eager sorted array'
emit_metric "trie ops" "lazy physical provider" \
            "$preserve_log" 'Lazy Physical Provider: false'
emit_metric "trie ops" "full materialization" \
            "$preserve_log" 'Provider Full Materialization: true'
emit_metric "trie ops" "provider rows read" \
            "$preserve_log" 'Provider Rows Read: [1-9][0-9]*'
emit_metric "trie ops" "provider row bytes" \
            "$preserve_log" 'Provider Row Bytes Allocated: [1-9][0-9]* bytes'
emit_metric "trie ops" "provider tuple bytes" \
            "$preserve_log" 'Provider Tuple Bytes Copied: [0-9]+ bytes'
emit_metric "trie ops" "provider ops" \
            "$preserve_log" \
            'Provider Trie Ops: lazy sorted arrays with on-demand prefix directories'
emit_metric "trie ops" "reduction domain builds" \
            "$preserve_log" 'Reduction Domain Builds: [0-9]+'
emit_metric "trie ops" "reduction domain rows" \
            "$preserve_log" 'Reduction Domain Rows Scanned: [0-9]+'
emit_metric "trie ops" "reduction domain keys" \
            "$preserve_log" 'Reduction Domain Keys Produced: [0-9]+'
emit_metric "trie ops" "reduction domain sorts" \
            "$preserve_log" 'Reduction Domain Sorts: [0-9]+'
emit_metric "trie ops" "runtime domain builds" \
            "$preserve_log" 'Runtime Domain Builds: [0-9]+'
emit_metric "trie ops" "runtime domain rows" \
            "$preserve_log" 'Runtime Domain Rows Scanned: [0-9]+'
emit_metric "trie ops" "prefix builds" \
            "$preserve_log" 'Prefix Range Builds: [1-9][0-9]*'
emit_metric "trie ops" "prefix reuses" \
            "$preserve_log" 'Prefix Range Reuses: [1-9][0-9]*'
emit_metric "trie ops" "cursor reuses" \
            "$preserve_log" 'Prefix Range Cursor Reuses: [1-9][0-9]*'
emit_metric "trie ops" "prefix cache hits" \
            "$preserve_log" 'Prefix Range Cache Hits: [0-9]+'
emit_metric "trie ops" "prefix cache misses" \
            "$preserve_log" 'Prefix Range Cache Misses: [0-9]+'
emit_metric "trie ops" "child cache hits" \
            "$preserve_log" 'Child Range Cache Hits: [0-9]+'
emit_metric "trie ops" "child cache misses" \
            "$preserve_log" 'Child Range Cache Misses: [0-9]+'
emit_metric "trie ops" "child range opens" \
            "$preserve_log" 'Trie Child Range Opens: [1-9][0-9]*'
emit_metric "trie ops" "prefix seeks" \
            "$preserve_log" 'Trie Prefix Range Seeks: [1-9][0-9]*'

emit_category "row goals"
emit_metric "row goals" "limit row goal" \
            "$semiring_log" 'WCOJ Row Goal Source: limit'
emit_metric "row goals" "exists row goal" \
            "$semiring_log" 'WCOJ Row Goal Source: exists'
emit_metric "row goals" "rows emitted" \
            "$semiring_log" 'Row Goal Rows Emitted: 1'
emit_metric "row goals" "flat rows avoided" \
            "$semiring_log" 'Row Goal Flat Rows Avoided: [1-9][0-9]*'
emit_metric "row goals" "goal reached" \
            "$semiring_log" 'Row Goal Reached: true'

emit_category "semiring consumers"
emit_metric "semiring consumers" "count" \
            "$semiring_log" 'WCOJ Consumer: count\(\*\)'
emit_metric "semiring consumers" "count distinct" \
            "$semiring_log" 'WCOJ Consumer: count\(distinct key\)'
emit_metric "semiring consumers" "group count" \
            "$semiring_log" 'WCOJ Consumer: group count\(key\)'
emit_metric "semiring consumers" "group count distinct" \
            "$semiring_log" 'WCOJ Consumer: group count\(distinct key\)'
emit_metric "semiring consumers" "sum property" \
            "$semiring_log" 'WCOJ Consumer: sum\(property\)'
emit_metric "semiring consumers" "avg property" \
            "$semiring_log" 'WCOJ Consumer: avg\(property\)'
emit_metric "semiring consumers" "min property" \
            "$semiring_log" 'WCOJ Consumer: min\(property\)'
emit_metric "semiring consumers" "max property" \
            "$semiring_log" 'WCOJ Consumer: max\(property\)'
emit_metric "semiring consumers" "group sum" \
            "$semiring_log" 'WCOJ Consumer: group sum\(property\)'
emit_metric "semiring consumers" "group avg" \
            "$semiring_log" 'WCOJ Consumer: group avg\(property\)'
emit_metric "semiring consumers" "group min" \
            "$semiring_log" 'WCOJ Consumer: group min\(property\)'
emit_metric "semiring consumers" "group max" \
            "$semiring_log" 'WCOJ Consumer: group max\(property\)'
emit_metric "semiring consumers" "exists" \
            "$semiring_log" 'WCOJ Consumer: exists'
emit_metric "semiring consumers" "consumer avoids" \
            "$semiring_log" 'Consumer Flat Rows Avoided: [1-9][0-9]*'
emit_metric "semiring consumers" "count result" \
            "$semiring_log" 'Count Result: [1-9][0-9]*'
emit_metric "semiring consumers" "sum input rows" \
            "$semiring_log" 'Sum Property Input Rows: [1-9][0-9]*'
emit_metric "semiring consumers" "avg input rows" \
            "$semiring_log" 'Avg Property Input Rows: [1-9][0-9]*'
emit_metric "semiring consumers" "min input rows" \
            "$semiring_log" 'Min Property Input Rows: [1-9][0-9]*'
emit_metric "semiring consumers" "max input rows" \
            "$semiring_log" 'Max Property Input Rows: [1-9][0-9]*'
emit_metric "semiring consumers" "exists result" \
            "$semiring_log" 'Exists Result: true'

emit_category "ghd separator"
emit_metric "ghd separator" "shape" \
            "$preserve_log" 'Reduction Shape: cyclic-with-tail'
emit_metric "ghd separator" "mode" \
            "$preserve_log" 'GHD Mode: general GHD'
emit_metric "ghd separator" "general decomposition" \
            "$preserve_log" 'GHD General Decomposition: true'
emit_metric "ghd separator" "fallback reason" \
            "$preserve_log" 'GHD Fallback Reason: none'
emit_metric "ghd separator" "pair separator" \
            "$preserve_log" 'GHD Separators: .*pair v'
emit_metric "ghd separator" "passes" \
            "$preserve_log" 'GHD Separator Reduction Passes: [1-9][0-9]*'
emit_metric "ghd separator" "leaf tail providers" \
            "$preserve_log" 'GHD Leaf Tail Providers: [1-9][0-9]*'
emit_metric "ghd separator" "domain keys" \
            "$preserve_log" 'GHD Separator Domain Keys: [1-9][0-9]*'
emit_metric "ghd separator" "core rows removed" \
            "$preserve_log" 'GHD Cyclic Core Rows Removed: [1-9][0-9]*'

current_step="complete"
printf '\nsecondary roadmap telemetry checks passed\n'
printf 'primary raw plan artifact: %s\n' "$plan_report"
