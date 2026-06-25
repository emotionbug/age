# Fixed-MATCH performance verification

`verify_fixed_match.sh` creates a disposable graph with 64 outgoing edges per
source vertex and verifies the optimized fixed-length Cypher path. It checks:

- `WHERE id(source)=constant` selects an indexed edge path;
- anonymous terminal vertices retain correct local-ID semantics;
- terminal label and single-property semantics remain correct;
- an unbound source can be reordered around a one-row terminal property;
- sparse expression-index statistics drive one-row terminal access;
- bounded property postings can drive a local intersection, with a local-ID fallback;
- multiple terminal properties retain the ordinary vertex-scan fallback.

Run against an installed AGE build:

```sh
PGHOST=/tmp PGPORT=5432 PGDATABASE=postgres PGUSER="$USER" \
  PG_CONFIG=/path/to/pg_config tools/perf/verify_fixed_match.sh
```

Set `BENCHMARK_SECONDS=10` to append prepared-statement `pgbench` runs for both
the bound fixed-match path and the unbound sparse-terminal path. The script
intentionally reports latency rather than enforcing a machine-specific
absolute threshold; compare the latter with the pre-series result to verify the
targeted 10x improvement on the deployment hardware.

## VLE level-batch worklist kernel

`verify_vle_level_batch_worklist.sh` reproduces the executor's level-batch
worklist operations without requiring a PostgreSQL server.  It compares the
former minimum-depth scan plus front-array removal against the FIFO head-offset
implementation.  It measures both a drain-only frontier and a sustained-width
depth transition that dequeues one parent while enqueueing one child, checks
identical visit order across an interleaved two-level frontier, and rejects
either result below 10x.

```sh
tools/perf/verify_vle_level_batch_worklist.sh
```

The benchmark isolates the worklist kernel rather than claiming whole-query
latency.  The sustained-width case also guards against copying the whole live
window on every dequeue/enqueue pair.  Use it to detect accidental
reintroduction of O(frontier²) work; use the installed-extension VLE benchmark
in the patch package for the end-to-end measurement.

Override widths or the guard threshold when needed:

```sh
VLE_WORKLIST_WIDTHS="8192 16384 32768" \
VLE_WORKLIST_MIN_SPEEDUP=10 \
  tools/perf/verify_vle_level_batch_worklist.sh
```

## Exact endpoint / expand-into indexes

A Cypher closing edge such as `(a)-[:R]->(b)` has both endpoint graphids
available before the edge lookup.  The optimized planner hands that lookup to
a two-column B-tree when either `(start_id, end_id)` or `(end_id, start_id)` is
present.  New edge labels create both endpoint-pair indexes under the historical
`*_start_id_idx` and `*_end_id_idx` names.

For existing databases, choose one migration mode:

```sh
# Blocking replacement; keeps two indexes and preserves historical names.
psql --set=ON_ERROR_STOP=1 --file=tools/perf/rebuild_endpoint_pair_indexes.sql

# Online/additive; retains scalar indexes and builds auxiliary pair indexes.
psql --set=ON_ERROR_STOP=1 --file=tools/perf/add_endpoint_pair_indexes_concurrently.sql
```

The blocking script takes a write-blocking lock while each index is rebuilt.
The concurrent script requires more temporary and steady-state disk space; the
old scalar indexes can be removed after plans and write overhead are validated.
Use `EXPLAIN (ANALYZE, BUFFERS)` to confirm that exact endpoint Cypher uses an
`Index Scan` or `Index Only Scan` with both `start_id` and `end_id` in the
`Index Cond`, rather than an `AGE Adjacency Match` custom scan.

The end-to-end verifier builds a high-fanout edge label, checks both plans, and
compares the endpoint-pair path with the exact-terminal adjacency fallback:

```sh
PG_CONFIG=/path/to/pg_config FANOUT=3000000 RUNS=7 \
  tools/perf/verify_expand_into_pair_index.sh
```

`MIN_SPEEDUP` defaults to `50`; raise it to the deployment target after sizing
`FANOUT` to the production degree distribution.
