#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
pg_config_bin=${PG_CONFIG:-pg_config}
psql_bin=${PSQL:-"$($pg_config_bin --bindir)/psql"}
fanout=${FANOUT:-3000000}
runs=${RUNS:-7}
minimum_speedup=${MIN_SPEEDUP:-50}
graph_name=${GRAPH_NAME:-hidden_age_expand_into_pair_perf}
keep_graph=${KEEP_GRAPH:-0}

if [[ ! $graph_name =~ ^[A-Za-z_][A-Za-z0-9_]*$ ]]; then
    echo "GRAPH_NAME must be an unquoted SQL identifier" >&2
    exit 2
fi
if (( fanout < 2 || runs < 3 )); then
    echo "FANOUT must be >= 2 and RUNS must be >= 3" >&2
    exit 2
fi

psql_args=(--no-psqlrc --set=ON_ERROR_STOP=1 --quiet --tuples-only --no-align)
tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/age-expand-into-pair.XXXXXX")

cleanup() {
    if [[ $keep_graph != 1 ]]; then
        "$psql_bin" "${psql_args[@]}" >/dev/null 2>&1 <<SQL || true
LOAD 'age';
SET search_path TO ag_catalog, public;
SELECT drop_graph('$graph_name', true);
SQL
    fi
    rm -rf "$tmpdir"
}
trap cleanup EXIT

"$psql_bin" "${psql_args[@]}" <<SQL >/dev/null
LOAD 'age';
SET search_path TO ag_catalog, public;
DO \$drop\$
BEGIN
    PERFORM drop_graph('$graph_name', true);
EXCEPTION
    WHEN OTHERS THEN NULL;
END
\$drop\$;
SELECT create_graph('$graph_name');
SELECT create_vlabel('$graph_name', 'N');
SELECT create_elabel('$graph_name', 'E');

-- Bulk load without maintaining either endpoint B-tree row by row.
DROP INDEX $graph_name."E_start_id_idx";
DROP INDEX $graph_name."E_end_id_idx";
DO \$setup\$
DECLARE
    n_label int := _label_id('$graph_name', 'N');
    e_label int := _label_id('$graph_name', 'E');
BEGIN
    EXECUTE format(
        'INSERT INTO $graph_name."N"(id, properties) VALUES '
        '(_graphid(%s, 1), ''{"role":"source"}''::agtype), '
        '(_graphid(%s, 2), ''{"role":"target"}''::agtype)',
        n_label, n_label);

    EXECUTE format(\$insert\$
        INSERT INTO $graph_name."E"(id, start_id, end_id, properties)
        SELECT _graphid(%s, i),
               _graphid(%s, 1),
               CASE WHEN i = 1 THEN _graphid(%s, 2)
                    ELSE _graphid(%s, i + 1000) END,
               '{}'::agtype
        FROM generate_series(1, $fanout) AS i
    \$insert\$, e_label, n_label, n_label, n_label);
END
\$setup\$;
CREATE INDEX "E_start_id_idx"
    ON $graph_name."E" USING btree(start_id, end_id);
CREATE INDEX "E_end_id_idx"
    ON $graph_name."E" USING btree(end_id, start_id);
CREATE INDEX "E_age_adj_start"
    ON $graph_name."E" USING age_adjacency(start_id, id, end_id);
ANALYZE $graph_name."N";
ANALYZE $graph_name."E";
SQL

source_id=$(
    "$psql_bin" "${psql_args[@]}" <<SQL | tail -n 1
LOAD 'age';
SET search_path TO ag_catalog, public;
SELECT id::text
FROM $graph_name."N"
WHERE properties @> '{"role":"source"}'::agtype;
SQL
)
target_id=$(
    "$psql_bin" "${psql_args[@]}" <<SQL | tail -n 1
LOAD 'age';
SET search_path TO ag_catalog, public;
SELECT id::text
FROM $graph_name."N"
WHERE properties @> '{"role":"target"}'::agtype;
SQL
)

query=$(cat <<SQL
SELECT * FROM cypher('$graph_name', \$cypher\$
  MATCH (target:N), (source:N), (source)-[:E]->(target)
  WHERE id(target) = $target_id AND id(source) = $source_id
  RETURN count(*)
\$cypher\$) AS (matches agtype)
SQL
)

result=$(
    "$psql_bin" "${psql_args[@]}" <<SQL | tail -n 1
LOAD 'age';
SET search_path TO ag_catalog, public;
$query;
SQL
)
if [[ $result != 1 ]]; then
    echo "expand-into result mismatch: expected 1, got $result" >&2
    exit 1
fi

pair_plan="$tmpdir/pair.plan"
exact_plan="$tmpdir/exact.plan"
"$psql_bin" "${psql_args[@]}" >"$pair_plan" <<SQL
LOAD 'age';
SET search_path TO ag_catalog, public;
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY ON, BUFFERS ON)
$query;
SQL
"$psql_bin" "${psql_args[@]}" >"$exact_plan" <<SQL
LOAD 'age';
SET search_path TO ag_catalog, public;
SET enable_indexscan = off;
SET enable_bitmapscan = off;
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY ON, BUFFERS ON)
$query;
SQL

if ! grep -Eq 'Index (Only )?Scan using "E_(start|end)_id_idx"' "$pair_plan" ||
   ! grep -Eq 'Index Cond:.*start_id.*end_id|Index Cond:.*end_id.*start_id' "$pair_plan" ||
   grep -q 'Custom Scan (AGE Adjacency Match)' "$pair_plan"; then
    echo "endpoint-pair plan was not selected" >&2
    cat "$pair_plan" >&2
    exit 1
fi
if ! grep -q 'Adjacency Expand Into: exact-terminal' "$exact_plan"; then
    echo "exact-terminal adjacency fallback was not selected" >&2
    cat "$exact_plan" >&2
    exit 1
fi

measure() {
    local mode=$1
    local output=$2
    local i

    : >"$output"
    for i in $(seq 1 "$runs"); do
        if [[ $mode == pair ]]; then
            "$psql_bin" "${psql_args[@]}" <<SQL |
LOAD 'age';
SET search_path TO ag_catalog, public;
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY ON)
$query;
SQL
                awk '/Execution Time:/ {print $3}' >>"$output"
        else
            "$psql_bin" "${psql_args[@]}" <<SQL |
LOAD 'age';
SET search_path TO ag_catalog, public;
SET enable_indexscan = off;
SET enable_bitmapscan = off;
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY ON)
$query;
SQL
                awk '/Execution Time:/ {print $3}' >>"$output"
        fi
    done
}

median() {
    sort -n "$1" | awk '
        { value[NR] = $1 }
        END {
            if (NR % 2) print value[(NR + 1) / 2];
            else print (value[NR / 2] + value[NR / 2 + 1]) / 2;
        }'
}

# Warm both paths before collecting medians.
cat "$pair_plan" >/dev/null
cat "$exact_plan" >/dev/null
measure pair "$tmpdir/pair.times"
measure exact "$tmpdir/exact.times"
pair_median=$(median "$tmpdir/pair.times")
exact_median=$(median "$tmpdir/exact.times")
speedup=$(awk -v old="$exact_median" -v new="$pair_median" \
    'BEGIN { printf "%.2f", old / new }')

printf 'fanout=%s runs=%s pair_median_ms=%s exact_median_ms=%s speedup=%sx\n' \
       "$fanout" "$runs" "$pair_median" "$exact_median" "$speedup"

if ! awk -v observed="$speedup" -v required="$minimum_speedup" \
    'BEGIN { exit !(observed >= required) }'; then
    printf 'expand-into speedup %sx is below required %sx\n' \
           "$speedup" "$minimum_speedup" >&2
    exit 1
fi
