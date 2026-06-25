#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
pg_config_bin=${PG_CONFIG:-pg_config}
psql_bin=${PSQL:-"$($pg_config_bin --bindir)/psql"}
pgbench_bin=${PGBENCH:-"$($pg_config_bin --bindir)/pgbench"}
benchmark_seconds=${BENCHMARK_SECONDS:-0}

psql_args=(--no-psqlrc --set=ON_ERROR_STOP=1 --quiet --tuples-only --no-align)

"$psql_bin" "${psql_args[@]}" --file="$script_dir/fixed_match_setup.sql" >/dev/null

source_id=$(
    "$psql_bin" "${psql_args[@]}" <<'SQL' | tail -n 1
LOAD 'age';
SET search_path TO ag_catalog, public;
SELECT id::text
FROM hidden_age_perf_regression."N"
ORDER BY id
LIMIT 1;
SQL
)

if [[ -z "$source_id" ]]; then
    echo "failed to resolve the fixed-match source graphid" >&2
    exit 1
fi

run_count() {
    local pattern=$1
    local result

    result=$(
        "$psql_bin" "${psql_args[@]}" <<SQL | tail -n 1
LOAD 'age';
SET search_path TO ag_catalog, public;
SELECT result::text
FROM cypher('hidden_age_perf_regression', \$cypher\$
${pattern}
\$cypher\$) AS (result agtype);
SQL
    )
    printf '%s' "$result"
}

assert_count() {
    local name=$1
    local pattern=$2
    local expected=$3
    local actual

    actual=$(run_count "$pattern")
    if [[ "$actual" != "$expected" ]]; then
        printf '%s: expected %s, got %s\n' "$name" "$expected" "$actual" >&2
        exit 1
    fi
    printf 'ok - %s = %s\n' "$name" "$actual"
}

explain_pattern() {
    local pattern=$1

    "$psql_bin" "${psql_args[@]}" <<SQL
LOAD 'age';
SET search_path TO ag_catalog, public;
SET max_parallel_workers_per_gather = 0;
EXPLAIN (COSTS OFF)
SELECT *
FROM cypher('hidden_age_perf_regression', \$cypher\$
${pattern}
\$cypher\$) AS (result agtype);
SQL
}

assert_count \
    "anonymous terminal" \
    "MATCH (a:N)-[:Noise]->() WHERE id(a)=${source_id} RETURN count(*)" \
    64
assert_count \
    "anonymous labeled terminal" \
    "MATCH (a:N)-[:Noise]->(:Sink) WHERE id(a)=${source_id} RETURN count(*)" \
    64
assert_count \
    "owned terminal property" \
    "MATCH (a:N)-[:Noise]->(:Sink {bucket: 1}) WHERE id(a)=${source_id} RETURN count(*)" \
    4
assert_count \
    "wrong terminal label" \
    "MATCH (a:N)-[:Noise]->(:N) WHERE id(a)=${source_id} RETURN count(*)" \
    0
assert_count \
    "multi-property fallback" \
    "MATCH (a:N)-[:Noise]->(:Sink {bucket: 1, j: 1}) WHERE id(a)=${source_id} RETURN count(*)" \
    1
assert_count \
    "unbound sparse terminal" \
    "MATCH (a:N)-[:Noise]->(:Sink {needle: 1}) RETURN count(*)" \
    1
assert_count \
    "unbound absent terminal" \
    "MATCH (a:N)-[:Noise]->(:Sink {needle: 2}) RETURN count(*)" \
    0

plain_plan=$(explain_pattern \
    "MATCH (a:N)-[:Noise]->() WHERE id(a)=${source_id} RETURN count(*)")
property_plan=$(explain_pattern \
    "MATCH (a:N)-[:Noise]->(:Sink {bucket: 1}) WHERE id(a)=${source_id} RETURN count(*)")
multi_property_plan=$(explain_pattern \
    "MATCH (a:N)-[:Noise]->(:Sink {bucket: 1, j: 1}) WHERE id(a)=${source_id} RETURN count(*)")
sparse_unbound_plan=$(explain_pattern \
    "MATCH (a:N)-[:Noise]->(:Sink {needle: 1}) RETURN count(*)")

if ! grep -Eq 'Custom Scan \(AGE Adjacency Match\)|Bitmap Index Scan on "Noise_start_id_idx"|Index Scan using "Noise_age_adj_start"' <<<"$plain_plan"; then
    echo "fixed MATCH did not select an indexed edge path" >&2
    exit 1
fi
if grep -q 'Sink_bucket_property_idx' <<<"$property_plan"; then
    if ! grep -q 'planned=source-prefetch' <<<"$property_plan"; then
        echo "bounded terminal posting index was selected without source prefetch" >&2
        exit 1
    fi
    if grep -q 'reason=property-source-too-broad' <<<"$property_plan"; then
        echo "bounded terminal posting index was both selected and rejected" >&2
        exit 1
    fi
elif ! grep -Eq 'reason=property-source-too-broad|Index Scan using "Sink_pkey"' <<<"$property_plan"; then
    echo "terminal property has neither bounded prefetch nor a local-ID fallback" >&2
    exit 1
fi
if ! grep -q 'Index Scan using "Sink_pkey"' <<<"$multi_property_plan"; then
    echo "multi-property terminal should retain its semantic fallback scan" >&2
    exit 1
fi
if ! grep -q 'Custom Scan (AGE Adjacency Match)' <<<"$sparse_unbound_plan"; then
    echo "unbound sparse terminal did not select AGE Adjacency Match" >&2
    exit 1
fi
if ! grep -q 'Sink_needle_property_idx' <<<"$sparse_unbound_plan"; then
    echo "unbound sparse terminal did not use its property-source index" >&2
    exit 1
fi
if grep -q 'direction=incoming' <<<"$sparse_unbound_plan"; then
    : # the outer one-row property index binds the terminal before expansion
elif grep -q 'planned=source-prefetch' <<<"$sparse_unbound_plan"; then
    if ! grep -Eq 'property-source-matches=1([^0-9]|$)' <<<"$sparse_unbound_plan"; then
        echo "sparse terminal property was not estimated as one posting" >&2
        exit 1
    fi
    if grep -q 'reason=property-source-too-broad' <<<"$sparse_unbound_plan"; then
        echo "sparse terminal property was incorrectly rejected" >&2
        exit 1
    fi
else
    echo "unbound sparse terminal has neither prefetch nor reverse expansion" >&2
    exit 1
fi
printf 'ok - indexed fixed match, direction, and property-source guards\n'

if (( benchmark_seconds > 0 )); then
    if [[ ! -x "$pgbench_bin" ]]; then
        echo "pgbench not found: $pgbench_bin" >&2
        exit 1
    fi
    benchmark_sql=$(mktemp)
    trap 'rm -f "$benchmark_sql"' EXIT
    sed "s/@SOURCE_ID@/${source_id}/g" \
        "$script_dir/fixed_match_pgbench.sql.in" >"$benchmark_sql"
    echo "# bound fixed-match benchmark"
    PGOPTIONS='-c session_preload_libraries=age -c search_path=ag_catalog,public' \
        "$pgbench_bin" --no-vacuum --protocol=prepared --client=1 --jobs=1 \
        --time="$benchmark_seconds" --file="$benchmark_sql"
    echo "# unbound sparse-terminal benchmark"
    PGOPTIONS='-c session_preload_libraries=age -c search_path=ag_catalog,public' \
        "$pgbench_bin" --no-vacuum --protocol=prepared --client=1 --jobs=1 \
        --time="$benchmark_seconds" \
        --file="$script_dir/fixed_match_unbound_pgbench.sql.in"
fi
