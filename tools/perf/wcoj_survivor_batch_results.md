# WCOJ survivor-batched payload benchmark evidence

Measured on 2026-06-25 against PostgreSQL 18.4.  The extension was built with
`COPT=-Werror`; JIT and parallel query were disabled.  The comparison used the
same PostgreSQL installation, data directory, graph fixture, GUCs, and warm-up
policy for both builds.

- baseline: `cdeffd9` (`Simplify multiway join planner and executor`)
- candidate: `c3a043b` (`Batch WCOJ survivor payload scans`)
- fixture: 4 source vertices, 4,096 common terminals, one edge per
  source/terminal pair
- query: dense 4-way star, forced WCOJ merge engine, `count(*)`
- policy: discard the first execution; report the median of the next five

Create the fixture and run the benchmark with:

```sh
psql --set=ON_ERROR_STOP=1 \
  --set=star_sources=4 --set=star_fanout=4096 \
  --set=cycle_vertices=1024 --set=cycle_fanout=64 \
  --file=tools/perf/wcoj_completion_setup.sql

psql --set=ON_ERROR_STOP=1 --set=runs=5 \
  --file=tools/perf/wcoj_completion_benchmark.sql
```

Relevant session settings:

```sql
SET jit = off;
SET max_parallel_workers_per_gather = 0;
SET age.enable_wcoj = on;
SET age.wcoj_engine = 'merge';
SET enable_nestloop = off;
SET enable_hashjoin = off;
SET enable_mergejoin = off;
```

## Result

| Build | Measured samples after warm-up | Median |
|---|---|---:|
| baseline `cdeffd9` | 2679.074, 2738.042, 2773.953, 2899.898, 2715.537 ms | 2738.042 ms |
| candidate `c3a043b` | 19.499, 20.038, 20.988, 19.908, 20.693 ms | 20.038 ms |

```text
speedup = 2738.042 / 20.038 = 136.64x
```

The result comes from eliminating survivor-local payload rescans.  The baseline
reopened each source posting for every survivor.  The candidate collects a
bounded sorted survivor block, applies it as an exact-set filter, and executes
one tagged multi-key payload scan per provider and block.  Source-row and
parallel-edge multiplicities are restored from factorized bags only after the
payload scan.

Candidate `EXPLAIN (ANALYZE, VERBOSE)` counters for the dense fixture:

```text
Batched Payload Materialization: true
Survivor Blocks: 2
Payload Scan Batches: 8
Payload Source Keys Scanned: 8
Payload Scan Restarts Avoided: 16376
Payload Rows Scanned: 16384
Payload Rows Matched: 16384
Payload Rows Fetched: 20480
Peak WCOJ Memory: 2298544 bytes
```

`Payload Rows Fetched` includes 4,096 terminal plan-stream rows in addition to
16,384 direct adjacency payload rows.  The scan-restart counter is the number
of survivor/source-key scan starts avoided relative to the former scalar
materialization path.

## Correctness and regression checks

- dense 3-source, 17-survivor regression: binary 17 rows, direct 17 rows,
  bidirectional `EXCEPT ALL` difference 0
- parallel-edge and duplicate-source multiplicity tests remain in
  `cypher_wcoj_semantics`
- focused regression: `age_adjacency`, `cypher_wcoj_semantics`,
  `cypher_match`, and `cypher_vle` all pass
- complete 39-test run: 33 pass in one accumulated cluster; the six tests
  affected by the existing 128-graph counter ceiling all pass together in a
  fresh instance

The 136.64x result is a dense-star end-to-end result, not a claim that every
query is 100x faster.  Sparse and output-bound shapes remain separate guard
workloads.
