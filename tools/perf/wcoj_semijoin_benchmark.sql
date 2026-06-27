\set ON_ERROR_STOP on
\if :{?runs}
\else
\set runs 5
\endif

LOAD 'age';
SET search_path = ag_catalog, "$user", public;
SET client_min_messages = warning;
SET jit = off;
SET max_parallel_workers_per_gather = 0;

-- Preserve the written chain order so the binary control materializes the
-- high-pressure adjacent-edge join.  All join methods remain enabled for both
-- cases; age.enable_wcoj is the only execution-path switch.
SET join_collapse_limit = 1;
SET from_collapse_limit = 1;
RESET enable_nestloop;
RESET enable_hashjoin;
RESET enable_mergejoin;

\echo 'acyclic chain: forced binary'
SET age.enable_wcoj = off;
SELECT format($sql$
EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY ON, BUFFERS ON)
SELECT count(*)
FROM cypher('wcoj_bench_semijoin', $cypher$
    MATCH (a:A)-[f1:F1]->(b:B)-[f2:F2]->(c:C)-[f3:F3]->(d:D)
    RETURN id(a), id(b), id(c), id(d), id(f1), id(f2), id(f3)
$cypher$) AS (a agtype, b agtype, c agtype, d agtype,
              f1 agtype, f2 agtype, f3 agtype)
$sql$)
FROM generate_series(1, :runs + 1)
\gexec

\echo 'acyclic chain: auto Generic Join with semijoin reduction'
SET age.enable_wcoj = on;
SELECT format($sql$
EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY ON, BUFFERS ON)
SELECT count(*)
FROM cypher('wcoj_bench_semijoin', $cypher$
    MATCH (a:A)-[f1:F1]->(b:B)-[f2:F2]->(c:C)-[f3:F3]->(d:D)
    RETURN id(a), id(b), id(c), id(d), id(f1), id(f2), id(f3)
$cypher$) AS (a agtype, b agtype, c agtype, d agtype,
              f1 agtype, f2 agtype, f3 agtype)
$sql$)
FROM generate_series(1, :runs + 1)
\gexec
