#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/generic-join-preserve.XXXXXX")
trap 'rm -rf "$tmpdir"' EXIT

pg_config=${PG_CONFIG:-/Users/emotionbug/IdeaProjects/postgres_proj/pg18release/bin/pg_config}
database=${PGDATABASE:-agebench}
cycle_size=${GENERIC_JOIN_PRESERVE_CYCLE_SIZE:-128}
raw_plan_log=${GENERIC_JOIN_PRESERVE_RAW_PLAN_LOG:-}

if [[ -n "$raw_plan_log" ]]; then
    mkdir -p "$(dirname -- "$raw_plan_log")"
    out=$raw_plan_log
else
    out="$tmpdir/generic-preserve.out"
fi
: > "$out"

if [[ ! -x "$pg_config" ]]; then
    echo "PG_CONFIG is not executable: $pg_config" >&2
    exit 1
fi

configure=$("$pg_config" --configure)
if [[ ${GENERIC_JOIN_PRESERVE_ALLOW_DEBUG_PG:-0} != 1 ]] &&
   [[ "$configure" == *"--enable-debug"* ||
      "$configure" == *"--enable-cassert"* ||
      "$configure" == *"-O0"* ]]; then
    echo "refusing to run Generic Join gates against a debug/cassert/O0 PostgreSQL" >&2
    echo "$configure" >&2
    exit 1
fi

psql=${PSQL:-$("$pg_config" --bindir)/psql}
psql_base=("$psql" --set=ON_ERROR_STOP=1 --dbname="$database")

"${psql_base[@]}" --tuples-only --no-align --command='SELECT 1' >/dev/null

if [[ ${GENERIC_JOIN_PRESERVE_SKIP_SETUP:-0} != 1 ]]; then
    "${psql_base[@]}" --set=cycle_size="$cycle_size" \
        --file="$script_dir/generic_join_preservation_setup.sql" >/dev/null
fi

"${psql_base[@]}" \
    --file="$script_dir/generic_join_preservation_benchmark.sql" | tee "$out"

require()
{
    local pattern=$1
    local description=$2

    if ! grep -Eq "$pattern" "$out"; then
        echo "missing Generic Join preservation gate: $description" >&2
        exit 1
    fi
}

require 'Custom Scan \(AGE Generic Multiway Join\)' 'Generic Join custom scan'
require 'Reduction Shape: cyclic-core' 'cyclic core shape'
require 'Reduction Shape: cyclic-with-tail' 'cyclic-with-tail shape'
require 'Generic Join Provider Mode: eager sorted array' 'physical provider mode'
require 'Lazy Physical Provider: false' 'lazy provider disclosure'
require 'Provider Full Materialization: true' 'provider materialization disclosure'
require 'Provider Rows Read: [1-9][0-9]*' 'provider row read telemetry'
require 'Provider Row Bytes Allocated: [1-9][0-9]* bytes' 'provider row allocation telemetry'
require 'GHD Leaf Tail Providers: 2' 'multi-tail separator count'
require 'GHD Mode: 2-core leaf-tail' '2-core leaf-tail GHD mode'
require 'GHD General Decomposition: false' 'general GHD limitation disclosure'
require 'GHD Fallback Reason: general GHD decomposition is not implemented' 'GHD fallback reason'
require 'GHD Separator Domain Keys: 2' 'multi-tail separator domains'
require 'GHD Cyclic Core Rows Removed: [1-9][0-9]*' 'cyclic core pruning'
require 'Provider Tuples Materialized: 0' 'key-only tuple skip'
require 'Prefix Range Reuses: [1-9][0-9]*' 'prefix range reuse'
require 'Rows Emitted: 1' 'tail-pruned result cardinality'

printf 'generic_join_preservation_cycle_size=%s raw_plan_log=%s\n' \
       "$cycle_size" "$out"
