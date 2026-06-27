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

The raw plans include budget and spill telemetry for batched payload blocks:
`Payload Block Budget Overruns`, `Payload Block Capacity Clamps`,
`Peak Payload Block Rows`, `Payload Bucket Blocks Spilled`,
`Payload Bucket Rows Spilled`, and `Payload Bucket Bytes Spilled`.
The budget counters show when a completed block exceeded the in-memory payload
budget and whether later survivor blocks were reduced from observed payload
fanout.  The bucket-spill counters show compact survivor payload buckets written
to temp storage and loaded back on demand.

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
telemetry, and the multiset correctness contract.  The regression and release
plans print the full Generic Join plan first; the Yannakakis step count, bottom-
up/top-down step plan, filter mode, applied-step counters, and retained provider
rows are visible in that plan, with grep checks kept as secondary guards.

## WCOJ roadmap release gates

`verify_wcoj_roadmap_gates.sh` runs the release-server gates for the current
WCOJ roadmap: dense 4-way survivor batching, dense auto-overhead, late-rejection
cycle Generic Join, and acyclic-chain semijoin reduction.  It rejects
debug/cassert/O0 PostgreSQL builds by default, runs the existing setup and
benchmark SQL, discards the first sample, computes medians, and fails when a
threshold is missed.

```sh
PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18release/bin/pg_config \
PGHOST=/tmp PGPORT=55432 PGDATABASE=agebench \
  tools/perf/verify_wcoj_roadmap_gates.sh
```

Default thresholds are:

- dense survivor batching speedup: `WCOJ_ROADMAP_MIN_DENSE_SPEEDUP=100`
- dense auto overhead: `WCOJ_ROADMAP_MAX_DENSE_OVERHEAD=1.25`
- late-rejection cycle speedup: `WCOJ_ROADMAP_MIN_CYCLE_SPEEDUP=20`
- acyclic-chain semijoin speedup: `WCOJ_ROADMAP_MIN_SEMIJOIN_SPEEDUP=100`

The dense-star speedup compares the current median with the recorded
pre-batching baseline from `wcoj_survivor_batch_results.md`.  Override
`WCOJ_ROADMAP_DENSE_BASELINE_MS` when measuring a fresh baseline on the same
hardware.  Use `WCOJ_ROADMAP_SKIP_COMPLETION_SETUP=1` or
`WCOJ_ROADMAP_SKIP_SEMIJOIN_SETUP=1` to reuse existing benchmark graphs.
Set `WCOJ_ROADMAP_COMPLETION_RAW_PLAN_LOG=/path/to/completion-plans.log` and
`WCOJ_ROADMAP_SEMIJOIN_RAW_PLAN_LOG=/path/to/semijoin-plans.log` when the
threshold gate should also preserve named full raw plan artifacts.

## WCOJ/Generic Join roadmap gate umbrella

`verify_wcoj_generic_join_roadmap_gates.sh` runs the bounded roadmap gate set
with one command: WCOJ roadmap gates, WCOJ semiring consumers, Generic Join
preservation, and the Generic reduction matrix.  It delegates to the individual
scripts in that order, prints short start/ok lines with each gate's final
summary, and fails fast at the first missed gate.

```sh
PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18release/bin/pg_config \
PGHOST=/tmp PGPORT=55432 PGDATABASE=agebench \
  tools/perf/verify_wcoj_generic_join_roadmap_gates.sh
```

The umbrella does not replace the existing knobs.  Keep using the individual
environment overrides for `PG_CONFIG`, `PGPORT`, fanout/size values, skip-setup
flags, and debug-PG allow flags such as `WCOJ_ROADMAP_ALLOW_DEBUG_PG=1`,
`WCOJ_SEMIRING_ALLOW_DEBUG_PG=1`, `GENERIC_JOIN_PRESERVE_ALLOW_DEBUG_PG=1`, and
`GENERIC_REDUCTION_ALLOW_DEBUG_PG=1`.  Full per-gate output is captured under
`ROADMAP_GATES_LOG_DIR` when set; otherwise the temporary log directory is
cleaned up after success and retained on failure.

After all gates pass, the umbrella runs `capture_roadmap_plans.sh --skip-setup`
by default, retains a Markdown artifact whose primary sections are the full raw
`EXPLAIN ANALYZE` plans, and prints that complete report to stdout.  Set
`ROADMAP_GATES_PRINT_PLANS=0` when stdout should stay compact but the artifact
should still be retained.  Set `ROADMAP_GATES_CAPTURE_PLANS=0` only when the
compact gate output is enough and the plan artifact is not needed:

```sh
ROADMAP_GATES_LOG_DIR=/tmp/hidden-age-roadmap-gates \
PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18release/bin/pg_config \
PGHOST=/tmp PGPORT=55432 PGDATABASE=agebench \
  tools/perf/verify_wcoj_generic_join_roadmap_gates.sh
```

## Roadmap raw plan report with secondary telemetry checks

`verify_roadmap_telemetry_report.sh` first delegates to
`capture_roadmap_plans.sh`, retains the full `EXPLAIN ANALYZE` output, writes
the raw-plan Markdown artifact, and prints that full artifact before any grep
or threshold checks run.  The raw plan text is the default review target.  The
PASS/FAIL telemetry summary is secondary and only checks that the logs include
survivor batching, alpha-acyclic semijoin reduction, factorized source bags and
shared enumerators, Generic Join provider materialization and trie/cache ops,
WCOJ row goals, semiring consumers, explicit GHD separator pruning counters,
and general GHD pair separators.

Regression coverage should follow the same plan-first ordering: standalone
`EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS ON)`
blocks expose the full direct payload, source-bag factorization, semiring
consumer, row-goal, and shared/local buffer plans before any grep-style
telemetry assertions run.

```sh
PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18release/bin/pg_config \
PGHOST=/tmp PGPORT=55432 PGDATABASE=agebench \
ROADMAP_TELEMETRY_LOG_DIR=/tmp/hidden-age-roadmap-telemetry \
ROADMAP_TELEMETRY_PLAN_REPORT=/tmp/hidden-age-roadmap-telemetry/full-plans.md \
  tools/perf/verify_roadmap_telemetry_report.sh
```

The script delegates data setup, timing thresholds, and debug-build rejection
to the individual verifier scripts.  Use the existing per-script environment
overrides for dimensions, skip-setup flags, baseline medians, and debug-PG
allow flags.  If `ROADMAP_TELEMETRY_LOG_DIR` is omitted, the script creates and
retains a temporary log directory and prints its path.  If
`ROADMAP_TELEMETRY_PLAN_REPORT` is omitted, the full-plan Markdown report is
written to `ROADMAP_TELEMETRY_LOG_DIR/roadmap-full-plans.md`.  Set
`ROADMAP_TELEMETRY_PRINT_PLAN_REPORT=0` only when the report should be written
without also being printed.  Set `ROADMAP_TELEMETRY_SECONDARY_CHECKS=0` to stop
after the raw plan capture and skip the secondary evidence gates.

When secondary checks are enabled, the wrapper also writes named full-output
logs beside the Markdown report:

- `raw-wcoj-completion-plans.log` for WCOJ payload batching, dense star, and
  cycle plans;
- `raw-wcoj-semijoin-plans.log` for the acyclic semijoin/Yannakakis plans;
- `raw-wcoj-semiring-plans.log` for semiring consumers and row goals;
- `raw-generic-join-preservation-plans.log` for Generic Join GHD/count
  preservation;
- `raw-generic-reduction-plans.log` for the reduction matrix and Yannakakis
  step disclosure.

## Roadmap full plan capture

`capture_roadmap_plans.sh` runs the roadmap benchmark SQL files and writes the
full raw `EXPLAIN ANALYZE` output for each workload to individual logs plus a
single Markdown report.  The report opens with complete raw plan sections and
is printed to stdout by default for direct inspection rather than grep-based
evidence gating.  It also includes an artifact index for the representative
WCOJ payload, semiring consumer, Generic Join GHD/count, and Yannakakis
reduction plan logs.  Set `ROADMAP_PLAN_PRINT_REPORT=0` to retain only the
file.

```sh
ROADMAP_PLAN_LOG_DIR=/tmp/hidden-age-roadmap-plans \
  tools/perf/capture_roadmap_plans.sh \
  --report /tmp/hidden-age-roadmap-plans.md
```

The script refuses debug/cassert/O0 PostgreSQL builds by default, matching the
release benchmark policy.  Use `ROADMAP_PLAN_ALLOW_DEBUG_PG=1` only for local
plan-shape inspection, not for performance evidence.

## WCOJ semiring consumer gates

`verify_wcoj_semiring_gates.sh` builds a compact direct-WCOJ graph whose three
edge bags imply `fanout^3` flat rows.  The default `fanout=500` creates
125,000,000 candidate combinations from only 1,500 edge rows, then verifies
that count, non-null key count, count-distinct, standalone distinct-key,
grouped key count, grouped count-distinct, sum/avg/min/max property consumers,
their grouped variants, LIMIT, and EXISTS use consumer arithmetic or row goals
instead of materializing the flat product.  The gate checks both the avoided
flat product and the actual
`Flat Rows Materialized` count, plus source-bag and factor-memory telemetry.

```sh
PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18release/bin/pg_config \
PGHOST=/tmp PGPORT=55432 PGDATABASE=agebench \
  tools/perf/verify_wcoj_semiring_gates.sh
```

Override `WCOJ_SEMIRING_FANOUT` to scale the implied flat cardinality, and use
`WCOJ_SEMIRING_SKIP_SETUP=1` to reuse an existing `wcoj_bench_semiring` graph.
Set `WCOJ_SEMIRING_RAW_PLAN_LOG=/path/to/semiring-plans.log` to preserve the
complete raw plan output as a named artifact while keeping the existing grep
guards operational.

## Generic Join preservation gates

`verify_generic_join_preservation.sh` builds a four-cycle plus two independent
leaf tails and verifies that cyclic components still choose Generic Join,
preserve key-only output materialization, expose the current eager sorted-array
physical provider in the raw plan, reuse prefix ranges, and apply multi-tail
separator pruning.  The raw plan reports `GHD Mode: general GHD`,
`GHD General Decomposition: true`, `GHD Fallback Reason: none`, and the
pair-plus-leaf separator list.  The script prints the full plan first and uses
the grep checks only as secondary guards over that plan text.

```sh
PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18release/bin/pg_config \
PGHOST=/tmp PGPORT=55432 PGDATABASE=agebench \
  tools/perf/verify_generic_join_preservation.sh
```

Set `GENERIC_JOIN_PRESERVE_CYCLE_SIZE` to scale the cyclic core, or
`GENERIC_JOIN_PRESERVE_SKIP_SETUP=1` to reuse an existing
`generic_join_preserve` graph.  Set
`GENERIC_JOIN_PRESERVE_RAW_PLAN_LOG=/path/to/generic-preserve-plans.log` to
preserve the full raw plan output for review before the secondary gate summary.

## Generic reduction matrix gate

`verify_generic_reduction_matrix.sh` builds an acyclic chain with a selective
leaf on the A side.  The B endpoint remains a one-row separator, so the planner
admits Generic Join for the high-pressure chain, and the leaf-peel semijoin
order should reduce retained provider rows to at most 1% of the original
provider rows before enumeration.  The script tees the full raw plan before
checking secondary telemetry guards, including the Yannakakis step plan and
applied-step counters.

```sh
PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18release/bin/pg_config \
PGHOST=/tmp PGPORT=55432 PGDATABASE=agebench \
  tools/perf/verify_generic_reduction_matrix.sh
```

Set `GENERIC_REDUCTION_FANOUT` to scale the rejected domains, or
`GENERIC_REDUCTION_SKIP_SETUP=1` to reuse an existing
`generic_reduction_matrix` graph.  Set
`GENERIC_REDUCTION_RAW_PLAN_LOG=/path/to/generic-reduction-plans.log` to retain
the complete Yannakakis/GHD disclosure as a raw plan artifact.
