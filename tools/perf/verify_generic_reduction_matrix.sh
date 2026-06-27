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

require_in_section()
{
    local section=$1
    local pattern=$2
    local description=$3

    if ! awk -v section="$section" -v pattern="$pattern" '
        $0 == section {
            in_section = 1
            next
        }
        in_section && /^generic reduction matrix:/ {
            exit
        }
        in_section && $0 ~ pattern {
            found = 1
            exit
        }
        END {
            exit found ? 0 : 1
        }
    ' "$out"; then
        echo "missing Generic reduction gate in $section: $description" >&2
        exit 1
    fi
}

metric_in_section()
{
    local section=$1
    local pattern=$2

    awk -v section="$section" -v pattern="$pattern" '
        $0 == section {
            in_section = 1
            next
        }
        in_section && /^generic reduction matrix:/ {
            exit
        }
        in_section && $0 ~ pattern {
            print $NF
            found = 1
            exit
        }
        END {
            if (!found)
                exit 1
        }
    ' "$out"
}

row_section='generic reduction matrix: acyclic leaf-pruned chain'
lazy_section='generic reduction matrix: lazy physical acyclic count'

provider_rows_after=$(metric_in_section "$row_section" \
                      'Semijoin Provider Rows After:')
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

lazy_provider_rows_after=$(metric_in_section "$lazy_section" \
                           'Semijoin Provider Rows After:')
if [[ -z "${lazy_provider_rows_after:-}" ]]; then
    echo "missing Generic reduction gate: lazy provider rows after" >&2
    exit 1
fi
if (( lazy_provider_rows_after > max_retained )); then
    echo "lazy retained provider rows $lazy_provider_rows_after exceed 1% gate $max_retained of $original_rows" >&2
    exit 1
fi

require 'Custom Scan \(AGE Generic Multiway Join\)' 'Generic Join custom scan'
require_in_section "$row_section" \
        'Generic Join Consumer: rows' 'row-producing consumer'
require_in_section "$row_section" \
        'Reduction Shape: alpha-acyclic' 'alpha-acyclic shape'
require_in_section "$row_section" \
        'Reduction Order: leaf-peel' 'leaf-peel order'
require_in_section "$row_section" \
        'Reduction Order Applied: true' 'ordered semijoin application'
require_in_section "$row_section" \
        'Yannakakis Step Count: [1-9][0-9]*' 'Yannakakis step count'
require_in_section "$row_section" \
        'Yannakakis Step Plan: bottom-up:' 'Yannakakis step plan'
require_in_section "$row_section" \
        'Yannakakis Step Filter Mode: planned step-domain provider filter' \
        'Yannakakis step filter mode'
require_in_section "$row_section" \
        'Yannakakis Steps Applied: [1-9][0-9]*' \
        'Yannakakis applied steps'
require_in_section "$row_section" \
        'Semijoin Reduction Passes: 2' 'two semijoin passes'
require_in_section "$row_section" \
        'Semijoin Rows Removed: [1-9][0-9]*' 'rows removed'
require_in_section "$row_section" \
        'Rows Emitted: 1' 'single final binding'
require_in_section "$row_section" \
        'Peak Generic Join Memory: [1-9][0-9]* bytes' 'memory telemetry'
require "$lazy_section" \
        'lazy physical count plan section'
require_in_section "$lazy_section" \
        'Generic Join Consumer: count[(][*][)]' \
        'lazy physical count consumer'
require_in_section "$lazy_section" \
        'Generic Join Provider Mode: lazy adjacency edge provider .* eager sorted array' \
        'lazy physical provider mode'
require_in_section "$lazy_section" \
        'Lazy Physical Provider: true' 'lazy physical provider flag'
require_in_section "$lazy_section" \
        'Provider Full Materialization: false' \
        'lazy physical providers stay non-materialized'
require_in_section "$lazy_section" \
        'Yannakakis Step Filter Mode: planned step-domain provider filter' \
        'lazy physical planned step-domain filter'
require_in_section "$lazy_section" \
        'Yannakakis Steps Applied: [1-9][0-9]*' \
        'lazy physical applied steps'
require_in_section "$lazy_section" \
        'Semijoin Rows Removed: [1-9][0-9]*' \
        'lazy physical rows removed'
require_in_section "$lazy_section" \
        'Lazy Physical Providers: [1-9][0-9]*' \
        'lazy physical provider count'
require_in_section "$lazy_section" \
        'Lazy Physical Reduction Domain Builds: [1-9][0-9]*' \
        'lazy physical reduction domain builds'
require_in_section "$lazy_section" \
        'Lazy Physical Reduction Source Keys: [1-9][0-9]*' \
        'lazy physical reduction source keys'
require_in_section "$lazy_section" \
        'Lazy Physical Reduction Rows Scanned: [1-9][0-9]*' \
        'lazy physical reduction rows scanned'
require_in_section "$lazy_section" \
        'Lazy Physical Reduction Keys Produced: [1-9][0-9]*' \
        'lazy physical reduction keys produced'
require_in_section "$lazy_section" \
        'Count Result: [1-9][0-9]*' 'lazy physical count result'

printf 'generic_reduction_original_rows=%s retained_rows=%s lazy_retained_rows=%s fanout=%s raw_plan_log=%s\n' \
       "$original_rows" "$provider_rows_after" "$lazy_provider_rows_after" \
       "$fanout" "$out"
