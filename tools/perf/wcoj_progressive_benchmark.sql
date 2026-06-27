\set ON_ERROR_STOP on
\if :{?runs}
\else
\set runs 5
\endif

LOAD 'age';
SET search_path = ag_catalog, "$user", public;
SET jit = off;
SET max_parallel_workers_per_gather = 0;

\echo 'Sparse high-arity intersection (expected count 0)'
SELECT count(*)
FROM age_wcoj_progressive_sparse_args args
CROSS JOIN LATERAL age_adjacency_multiway_intersect(
    'age_wcoj_progressive_sparse', 'E', args.source_ids) result;

SELECT format($sql$
EXPLAIN (ANALYZE, TIMING OFF, SUMMARY ON, BUFFERS ON)
SELECT count(*)
FROM age_wcoj_progressive_sparse_args args
CROSS JOIN LATERAL age_adjacency_multiway_intersect(
    'age_wcoj_progressive_sparse', 'E', args.source_ids) result
$sql$)
FROM generate_series(1, :runs)
\gexec

\echo 'Dense high-arity intersection (expected count = fanout)'
SELECT count(*)
FROM age_wcoj_progressive_dense_args args
CROSS JOIN LATERAL age_adjacency_multiway_intersect(
    'age_wcoj_progressive_dense', 'E', args.source_ids) result;

SELECT format($sql$
EXPLAIN (ANALYZE, TIMING OFF, SUMMARY ON, BUFFERS ON)
SELECT count(*)
FROM age_wcoj_progressive_dense_args args
CROSS JOIN LATERAL age_adjacency_multiway_intersect(
    'age_wcoj_progressive_dense', 'E', args.source_ids) result
$sql$)
FROM generate_series(1, :runs)
\gexec
