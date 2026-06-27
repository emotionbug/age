#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/wcoj-semiring-gates.XXXXXX")
trap 'rm -rf "$tmpdir"' EXIT

pg_config=${PG_CONFIG:-/Users/emotionbug/IdeaProjects/postgres_proj/pg18release/bin/pg_config}
database=${PGDATABASE:-agebench}
fanout=${WCOJ_SEMIRING_FANOUT:-500}
flat_rows=$((fanout * fanout * fanout))
flat_rows_minus_one=$((flat_rows - 1))

if [[ ! -x "$pg_config" ]]; then
    echo "PG_CONFIG is not executable: $pg_config" >&2
    exit 1
fi

configure=$("$pg_config" --configure)
if [[ ${WCOJ_SEMIRING_ALLOW_DEBUG_PG:-0} != 1 ]] &&
   [[ "$configure" == *"--enable-debug"* ||
      "$configure" == *"--enable-cassert"* ||
      "$configure" == *"-O0"* ]]; then
    echo "refusing to run semiring gates against a debug/cassert/O0 PostgreSQL" >&2
    echo "$configure" >&2
    exit 1
fi

psql=${PSQL:-$("$pg_config" --bindir)/psql}
psql_base=("$psql" --set=ON_ERROR_STOP=1 --dbname="$database")
out="$tmpdir/semiring.out"

"${psql_base[@]}" --tuples-only --no-align --command='SELECT 1' >/dev/null

if [[ ${WCOJ_SEMIRING_SKIP_SETUP:-0} != 1 ]]; then
    "${psql_base[@]}" --set=fanout="$fanout" \
        --file="$script_dir/wcoj_semiring_setup.sql" >/dev/null
fi

"${psql_base[@]}" --set=fanout="$fanout" \
    --file="$script_dir/wcoj_semiring_benchmark.sql" | tee "$out"

require()
{
    local pattern=$1
    local description=$2

    if ! grep -Eq "$pattern" "$out"; then
        echo "missing semiring gate: $description" >&2
        exit 1
    fi
}

require 'WCOJ Consumer: count\(\*\)' 'count consumer'
require "Count Result: $flat_rows" 'count arithmetic result'
require "Consumer Flat Rows Avoided: $flat_rows" 'count avoids flat rows'
require 'WCOJ Consumer: count\(distinct key\)' 'count distinct consumer'
require 'Count Result: 1' 'count distinct one-key result'
require 'WCOJ Consumer: sum\(property\)' 'sum property consumer'
require "Sum Property Input Rows: $flat_rows" 'sum property multiplicity'
require 'WCOJ Consumer: group sum\(property\)' 'group sum property consumer'
require "Consumer Flat Rows Avoided: $flat_rows" 'group sum avoids flat rows'
require "Flat Rows Avoided: $flat_rows_minus_one" 'limit/exists avoid flat rows after one demand'
require 'WCOJ Consumer: exists' 'exists consumer'
require 'WCOJ Row Goal: 1' 'exists row goal'
require 'WCOJ Row Goal Source: exists' 'exists row goal source'
require 'Row Goal Rows Emitted: 1' 'row goal emitted one row'
require "Row Goal Flat Rows Avoided: $flat_rows_minus_one" 'row goal avoids flat rows after one demand'
require 'Exists Result: true' 'exists result'
require 'Row Goal Reached: true' 'exists short circuit'
require 'Rows Emitted: 1' 'single-row consumers'
require 'Source Bag Rows: [1-9][0-9]*' 'source bag row telemetry'
require 'Source Bag Bytes: [1-9][0-9]* bytes' 'source bag memory telemetry'
require 'Peak Factor Memory: [1-9][0-9]* bytes' 'factor memory telemetry'

printf 'wcoj_semiring_flat_rows=%s fanout=%s\n' "$flat_rows" "$fanout"
