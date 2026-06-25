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

## WCOJ progressive multiway intersection

`verify_wcoj_progressive_intersection.sh` isolates the high-arity intersection
algorithm without requiring PostgreSQL.  It compares the former global merge
with full per-destination bitmap clears, the sparse dirty-word reset, and the
progressive exact-set algorithm.  The sparse fixture gives every source private
destinations, so the progressive path can prove the result empty after the
second source.  A dense fixture verifies that retaining more than 75% of the
seed falls back to the streaming global merge without excessive overhead.

```sh
tools/perf/verify_wcoj_progressive_intersection.sh
```

The default guard checks 1,024, 4,096, and 8,192 sources at fanout 32, requires
at least 10x algorithmic speedup, and limits dense fallback overhead to 1.25x.
Override the dimensions or thresholds as follows:

```sh
WCOJ_PROGRESSIVE_SOURCES="2048 8192" \
WCOJ_PROGRESSIVE_FANOUT=64 \
WCOJ_PROGRESSIVE_MIN_SPEEDUP=10 \
WCOJ_PROGRESSIVE_MAX_DENSE_OVERHEAD=1.25 \
  tools/perf/verify_wcoj_progressive_intersection.sh
```

The C result is a complexity/regression guard, not a whole-query latency claim.
For an installed release AGE build, create matched sparse and dense graph data
and collect five `EXPLAIN ANALYZE` samples per shape:

```sh
psql --set=ON_ERROR_STOP=1 --set=sources=8192 --set=fanout=32 \
  --file=tools/perf/wcoj_progressive_setup.sql
psql --set=ON_ERROR_STOP=1 --set=runs=5 \
  --file=tools/perf/wcoj_progressive_benchmark.sql
```

For a before/after comparison, run the benchmark against separately installed
baseline and candidate extension builds with the same PostgreSQL release,
configuration, graph dimensions, and warmup policy.  The progressive kernel is
currently exposed through `age_adjacency_multiway_intersect`; automatic
cyclic/fork Cypher lowering remains a separate planner task.

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

## WCOJ survivor-batched payload materialization

`wcoj_completion_setup.sql` and `wcoj_completion_benchmark.sql` include a dense
4-way star that exposes repeated survivor-local payload scans.  The batched
executor collects a bounded survivor block and performs one exact-set tagged
payload run scan per adjacency provider and block, preserving source and edge
bag multiplicity without materializing the posting product.

```sh
psql --set=ON_ERROR_STOP=1 \
  --set=star_sources=4 --set=star_fanout=4096 \
  --set=cycle_vertices=1024 --set=cycle_fanout=64 \
  --file=tools/perf/wcoj_completion_setup.sql
psql --set=ON_ERROR_STOP=1 --set=runs=5 \
  --file=tools/perf/wcoj_completion_benchmark.sql
```

Run the same fixture against separately installed baseline and candidate
builds.  Discard the first sample and compare the median of the next five.
`wcoj_survivor_batch_results.md` records the reference environment, raw
samples, telemetry, and correctness checks.

## Query-wide semijoin reduction for acyclic multiway joins

`wcoj_semijoin_setup.sql` creates a high-pressure three-edge chain whose two C
endpoint domains overlap at one key.  `wcoj_semijoin_benchmark.sql` compares the
written-order binary plan with auto Generic Join while leaving all PostgreSQL
join methods enabled.  Generic Join performs repeated endpoint semijoins and
compacts provider rows before variable DFS.

```sh
psql --set=ON_ERROR_STOP=1 --set=fanout=128 \
  --file=tools/perf/wcoj_semijoin_setup.sql
psql --set=ON_ERROR_STOP=1 --set=runs=5 \
  --file=tools/perf/wcoj_semijoin_benchmark.sql
```

The benchmark fixes `join_collapse_limit` and `from_collapse_limit` to one so
that the binary control retains the written chain order; `age.enable_wcoj` is
the only path switch.  See `wcoj_semijoin_results.md` for raw samples,
telemetry, and the multiset correctness contract.
