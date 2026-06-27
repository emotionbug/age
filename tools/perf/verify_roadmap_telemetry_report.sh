#!/usr/bin/env bash
set -Eeuo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
current_gate="initialization"

if [[ -n "${ROADMAP_TELEMETRY_LOG_DIR:-}" ]]; then
    log_dir=$ROADMAP_TELEMETRY_LOG_DIR
    mkdir -p "$log_dir"
else
    timestamp=$(date +%Y%m%d-%H%M%S)
    log_dir=$(mktemp -d "${TMPDIR:-/tmp}/roadmap-telemetry-$timestamp.XXXXXX")
fi

on_error()
{
    local status=$?

    printf 'roadmap telemetry gate failed unexpectedly: %s (exit %s)\n' \
           "$current_gate" "$status" >&2
    printf 'roadmap telemetry logs retained in %s\n' "$log_dir" >&2
    exit "$status"
}
trap on_error ERR

run_gate()
{
    local label=$1
    local script_name=$2
    local log_name=$3
    local script_path="$script_dir/$script_name"
    local log_path="$log_dir/$log_name"
    local status

    current_gate="$label"
    if [[ ! -x "$script_path" ]]; then
        printf 'roadmap telemetry gate script is not executable: %s\n' \
               "$script_path" >&2
        exit 1
    fi

    printf '%s: start\n' "$label"
    if "$script_path" >"$log_path" 2>&1; then
        printf '%s: ok\n' "$label"
    else
        status=$?
        printf '%s: failed (exit %s)\n' "$label" "$status" >&2
        printf 'log: %s\n' "$log_path" >&2
        tail -n 60 "$log_path" >&2 || true
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
        printf 'missing roadmap telemetry metric: %s: %s\n' \
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

wcoj_log="$log_dir/wcoj-roadmap-gates.log"
semiring_log="$log_dir/wcoj-semiring-gates.log"
preserve_log="$log_dir/generic-join-preservation.log"
reduction_log="$log_dir/generic-reduction-matrix.log"

printf 'roadmap telemetry gate: PG_CONFIG=%s PGDATABASE=%s PGHOST=%s PGPORT=%s\n' \
       "${PG_CONFIG:-<gate default>}" \
       "${PGDATABASE:-agebench}" \
       "${PGHOST:-<libpq default>}" \
       "${PGPORT:-<libpq default>}"
printf 'roadmap telemetry logs: %s\n' "$log_dir"

run_gate "WCOJ roadmap gates" \
         "verify_wcoj_roadmap_gates.sh" \
         "$(basename "$wcoj_log")"
run_gate "WCOJ semiring consumer gates" \
         "verify_wcoj_semiring_gates.sh" \
         "$(basename "$semiring_log")"
run_gate "Generic Join preservation gates" \
         "verify_generic_join_preservation.sh" \
         "$(basename "$preserve_log")"
run_gate "Generic reduction matrix gate" \
         "verify_generic_reduction_matrix.sh" \
         "$(basename "$reduction_log")"

current_gate="telemetry extraction"

printf '\nroadmap telemetry report\n'
printf 'logs=%s\n' "$log_dir"

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

emit_category "semijoin reduction"
emit_metric "semijoin reduction" "shape" \
            "$reduction_log" 'Reduction Shape: alpha-acyclic'
emit_metric "semijoin reduction" "order" \
            "$reduction_log" 'Reduction Order: leaf-peel'
emit_metric "semijoin reduction" "passes" \
            "$reduction_log" 'Semijoin Reduction Passes: [1-9][0-9]*'
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
emit_metric "factorized binding" "peak factor memory" \
            "$semiring_log" 'Peak Factor Memory: [1-9][0-9]* bytes'

emit_category "trie ops"
emit_metric "trie ops" "provider ops" \
            "$preserve_log" 'Provider Trie Ops: sorted-array'
emit_metric "trie ops" "prefix builds" \
            "$preserve_log" 'Prefix Range Builds: [1-9][0-9]*'
emit_metric "trie ops" "prefix reuses" \
            "$preserve_log" 'Prefix Range Reuses: [1-9][0-9]*'
emit_metric "trie ops" "cursor reuses" \
            "$preserve_log" 'Prefix Range Cursor Reuses: [1-9][0-9]*'
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
emit_metric "semiring consumers" "sum property" \
            "$semiring_log" 'WCOJ Consumer: sum\(property\)'
emit_metric "semiring consumers" "group sum" \
            "$semiring_log" 'WCOJ Consumer: group sum\(property\)'
emit_metric "semiring consumers" "exists" \
            "$semiring_log" 'WCOJ Consumer: exists'
emit_metric "semiring consumers" "consumer avoids" \
            "$semiring_log" 'Consumer Flat Rows Avoided: [1-9][0-9]*'
emit_metric "semiring consumers" "count result" \
            "$semiring_log" 'Count Result: [1-9][0-9]*'
emit_metric "semiring consumers" "sum input rows" \
            "$semiring_log" 'Sum Property Input Rows: [1-9][0-9]*'
emit_metric "semiring consumers" "exists result" \
            "$semiring_log" 'Exists Result: true'

emit_category "ghd separator"
emit_metric "ghd separator" "shape" \
            "$preserve_log" 'Reduction Shape: cyclic-with-tail'
emit_metric "ghd separator" "passes" \
            "$preserve_log" 'GHD Separator Reduction Passes: [1-9][0-9]*'
emit_metric "ghd separator" "leaf tail providers" \
            "$preserve_log" 'GHD Leaf Tail Providers: [1-9][0-9]*'
emit_metric "ghd separator" "domain keys" \
            "$preserve_log" 'GHD Separator Domain Keys: [1-9][0-9]*'
emit_metric "ghd separator" "core rows removed" \
            "$preserve_log" 'GHD Cyclic Core Rows Removed: [1-9][0-9]*'

current_gate="complete"
printf '\nroadmap telemetry report passed\n'
