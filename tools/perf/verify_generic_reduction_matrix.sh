#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/generic-reduction-matrix.XXXXXX")
trap 'rm -rf "$tmpdir"' EXIT

pg_config=${PG_CONFIG:-/Users/emotionbug/IdeaProjects/postgres_proj/pg18release/bin/pg_config}
database=${PGDATABASE:-agebench}
fanout=${GENERIC_REDUCTION_FANOUT:-1024}
raw_plan_log=${GENERIC_REDUCTION_RAW_PLAN_LOG:-}

if [[ -n "$raw_plan_log" ]]; then
    mkdir -p "$(dirname -- "$raw_plan_log")"
    out=$raw_plan_log
else
    out="$tmpdir/generic-reduction.out"
fi
: > "$out"

if [[ ! -x "$pg_config" ]]; then
    echo "PG_CONFIG is not executable: $pg_config" >&2
    exit 1
fi

configure=$("$pg_config" --configure)
if [[ ${GENERIC_REDUCTION_ALLOW_DEBUG_PG:-0} != 1 ]] &&
   [[ "$configure" == *"--enable-debug"* ||
      "$configure" == *"--enable-cassert"* ||
      "$configure" == *"-O0"* ]]; then
    echo "refusing to run Generic reduction gates against a debug/cassert/O0 PostgreSQL" >&2
    echo "$configure" >&2
    exit 1
fi

psql=${PSQL:-$("$pg_config" --bindir)/psql}
psql_base=("$psql" --set=ON_ERROR_STOP=1 --dbname="$database")

"${psql_base[@]}" --tuples-only --no-align --command='SELECT 1' >/dev/null

if [[ ${GENERIC_REDUCTION_SKIP_SETUP:-0} != 1 ]]; then
    "${psql_base[@]}" --set=fanout="$fanout" \
        --file="$script_dir/generic_reduction_matrix_setup.sql" >/dev/null
fi

"${psql_base[@]}" \
    --file="$script_dir/generic_reduction_matrix_benchmark.sql" | tee "$out"

require()
{
    local pattern=$1
    local description=$2

    if ! grep -Eq "$pattern" "$out"; then
        echo "missing Generic reduction gate: $description" >&2
        exit 1
    fi
}

provider_rows_after=$(awk '/Semijoin Provider Rows After:/ { print $NF; exit }' "$out")
if [[ -z "${provider_rows_after:-}" ]]; then
    echo "missing Generic reduction gate: provider rows after" >&2
    exit 1
fi

# Setup rows: A fanout, B 1, C 2*fanout-1, D 1, P 1,
# F1/F2/F3 fanout each, and H 1.
original_rows=$((6 * fanout + 3))
max_retained=$((original_rows / 100))
if (( max_retained < 1 )); then
    max_retained=1
fi
if (( provider_rows_after > max_retained )); then
    echo "retained provider rows $provider_rows_after exceed 1% gate $max_retained of $original_rows" >&2
    exit 1
fi

require 'Custom Scan \(AGE Generic Multiway Join\)' 'Generic Join custom scan'
require 'Reduction Shape: alpha-acyclic' 'alpha-acyclic shape'
require 'Reduction Order: leaf-peel' 'leaf-peel order'
require 'Reduction Order Applied: true' 'ordered semijoin application'
require 'Yannakakis Step Count: [1-9][0-9]*' 'Yannakakis step count'
require 'Yannakakis Step Plan: bottom-up:' 'Yannakakis step plan'
require 'Yannakakis Step Filter Mode: global-domain provider filter' 'Yannakakis step filter mode'
require 'Yannakakis Steps Applied: [1-9][0-9]*' 'Yannakakis applied steps'
require 'Semijoin Reduction Passes: 2' 'two semijoin passes'
require 'Semijoin Rows Removed: [1-9][0-9]*' 'rows removed'
require 'Rows Emitted: 1' 'single final binding'
require 'Peak Generic Join Memory: [1-9][0-9]* bytes' 'memory telemetry'

printf 'generic_reduction_original_rows=%s retained_rows=%s fanout=%s raw_plan_log=%s\n' \
       "$original_rows" "$provider_rows_after" "$fanout" "$out"
