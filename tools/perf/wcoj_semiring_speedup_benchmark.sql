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

\echo 'semiring count speedup: forced binary count'
SET age.enable_wcoj = off;
RESET enable_nestloop;
RESET enable_hashjoin;
RESET enable_mergejoin;
SELECT format($sql$
EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY ON, BUFFERS ON)
SELECT *
FROM cypher('wcoj_bench_semiring', $cypher$
    MATCH (s1:S {id:1})-[e1:E1]->(t:T),
          (s2:S {id:2})-[e2:E2]->(t),
          (s3:S {id:3})-[e3:E3]->(t)
    RETURN count(*) AS total
$cypher$) AS (total agtype)
$sql$)
FROM generate_series(1, :runs + 1)
\gexec

\echo 'semiring count speedup: WCOJ count'
SET age.enable_wcoj = on;
SET age.wcoj_engine = 'merge';
SET enable_nestloop = off;
SET enable_hashjoin = off;
SET enable_mergejoin = off;
SELECT format($sql$
EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY ON, BUFFERS ON)
SELECT *
FROM cypher('wcoj_bench_semiring', $cypher$
    MATCH (s1:S {id:1})-[e1:E1]->(t:T),
          (s2:S {id:2})-[e2:E2]->(t),
          (s3:S {id:3})-[e3:E3]->(t)
    RETURN count(*) AS total
$cypher$) AS (total agtype)
$sql$)
FROM generate_series(1, :runs + 1)
\gexec
