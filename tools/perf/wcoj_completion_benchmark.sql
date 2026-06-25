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

-- Each generated EXPLAIN is a measured sample. Run the setup script first,
-- discard the first sample as warm-up, and report the median Execution Time.
\echo 'sparse star: forced binary (expected count 1)'
SET age.enable_wcoj = off;
RESET enable_nestloop;
RESET enable_hashjoin;
RESET enable_mergejoin;
SELECT format($sql$
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY ON, BUFFERS OFF)
SELECT count(*)
FROM cypher('wcoj_bench_sparse', $cypher$
    MATCH (s1:S {id:1})-[:E]->(t:T),
          (s2:S {id:2})-[:E]->(t),
          (s3:S {id:3})-[:E]->(t),
          (s4:S {id:4})-[:E]->(t)
    RETURN id(t)
$cypher$) AS (tid agtype)
$sql$)
FROM generate_series(1, :runs + 1)
\gexec

\echo 'sparse star: auto WCOJ'
SET age.enable_wcoj = on;
SET age.wcoj_engine = 'auto';
SET enable_nestloop = off;
SET enable_hashjoin = off;
SET enable_mergejoin = off;
SELECT format($sql$
EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY ON, BUFFERS OFF)
SELECT count(*)
FROM cypher('wcoj_bench_sparse', $cypher$
    MATCH (s1:S {id:1})-[:E]->(t:T),
          (s2:S {id:2})-[:E]->(t),
          (s3:S {id:3})-[:E]->(t),
          (s4:S {id:4})-[:E]->(t)
    RETURN id(t)
$cypher$) AS (tid agtype)
$sql$)
FROM generate_series(1, :runs + 1)
\gexec

\echo 'dense star: forced merge (expected count = star_fanout)'
SET age.enable_wcoj = on;
SET age.wcoj_engine = 'merge';
SET enable_nestloop = off;
SET enable_hashjoin = off;
SET enable_mergejoin = off;
SELECT format($sql$
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY ON, BUFFERS OFF)
SELECT count(*)
FROM cypher('wcoj_bench_dense', $cypher$
    MATCH (s1:S {id:1})-[:E]->(t:T),
          (s2:S {id:2})-[:E]->(t),
          (s3:S {id:3})-[:E]->(t),
          (s4:S {id:4})-[:E]->(t)
    RETURN id(t)
$cypher$) AS (tid agtype)
$sql$)
FROM generate_series(1, :runs + 1)
\gexec

\echo 'dense star: auto WCOJ'
SET age.enable_wcoj = on;
SET age.wcoj_engine = 'auto';
SET enable_nestloop = off;
SET enable_hashjoin = off;
SET enable_mergejoin = off;
SELECT format($sql$
EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY ON, BUFFERS OFF)
SELECT count(*)
FROM cypher('wcoj_bench_dense', $cypher$
    MATCH (s1:S {id:1})-[:E]->(t:T),
          (s2:S {id:2})-[:E]->(t),
          (s3:S {id:3})-[:E]->(t),
          (s4:S {id:4})-[:E]->(t)
    RETURN id(t)
$cypher$) AS (tid agtype)
$sql$)
FROM generate_series(1, :runs + 1)
\gexec

\echo 'late-rejection cycle: forced binary (expected count 0)'
SET age.enable_wcoj = off;
RESET enable_nestloop;
RESET enable_hashjoin;
RESET enable_mergejoin;
SELECT format($sql$
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY ON, BUFFERS OFF)
SELECT count(*)
FROM cypher('wcoj_bench_cycle', $cypher$
    MATCH (a:A)-[:E1]->(b:B)-[:E2]->(c:C)-[:E3]->(a)
    RETURN id(a), id(b), id(c)
$cypher$) AS (aid agtype, bid agtype, cid agtype)
$sql$)
FROM generate_series(1, :runs + 1)
\gexec

\echo 'late-rejection cycle: Generic Join'
SET age.enable_wcoj = on;
SET age.wcoj_engine = 'auto';
SET enable_nestloop = off;
SET enable_hashjoin = off;
SET enable_mergejoin = off;
SELECT format($sql$
EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY ON, BUFFERS OFF)
SELECT count(*)
FROM cypher('wcoj_bench_cycle', $cypher$
    MATCH (a:A)-[:E1]->(b:B)-[:E2]->(c:C)-[:E3]->(a)
    RETURN id(a), id(b), id(c)
$cypher$) AS (aid agtype, bid agtype, cid agtype)
$sql$)
FROM generate_series(1, :runs + 1)
\gexec
