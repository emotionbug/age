\set ON_ERROR_STOP on
\if :{?fanout}
\else
\set fanout 500
\endif

LOAD 'age';
SET search_path = ag_catalog, "$user", public;
SET client_min_messages = warning;
SET jit = off;
SET max_parallel_workers_per_gather = 0;
SET age.enable_wcoj = on;
SET age.wcoj_engine = 'merge';
SET enable_nestloop = off;
SET enable_hashjoin = off;
SET enable_mergejoin = off;
SET enable_hashagg = off;

\echo 'semiring count: fanout^3 flat rows'
EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY ON, BUFFERS OFF)
SELECT *
FROM cypher('wcoj_bench_semiring', $$
    MATCH (s1:S {id:1})-[e1:E1]->(t:T),
          (s2:S {id:2})-[e2:E2]->(t),
          (s3:S {id:3})-[e3:E3]->(t)
    RETURN count(*) AS total
$$) AS (total agtype);

\echo 'semiring count distinct key: one terminal'
EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY ON, BUFFERS OFF)
SELECT *
FROM cypher('wcoj_bench_semiring', $$
    MATCH (s1:S {id:1})-[e1:E1]->(t:T),
          (s2:S {id:2})-[e2:E2]->(t),
          (s3:S {id:3})-[e3:E3]->(t)
    RETURN count(DISTINCT id(t)) AS total
$$) AS (total agtype);

\echo 'semiring distinct key: one terminal row'
EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY ON, BUFFERS OFF)
SELECT *
FROM cypher('wcoj_bench_semiring', $$
    MATCH (s1:S {id:1})-[e1:E1]->(t:T),
          (s2:S {id:2})-[e2:E2]->(t),
          (s3:S {id:3})-[e3:E3]->(t)
    RETURN DISTINCT id(t) AS key
$$) AS (key agtype);

\echo 'semiring distinct key limit: one demanded key'
EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY ON, BUFFERS OFF)
SELECT *
FROM cypher('wcoj_bench_semiring', $$
    MATCH (s1:S {id:1})-[e1:E1]->(t:T),
          (s2:S {id:2})-[e2:E2]->(t),
          (s3:S {id:3})-[e3:E3]->(t)
    RETURN DISTINCT id(t) AS key
    LIMIT 1
$$) AS (key agtype);

\echo 'semiring group count distinct key: terminal key'
EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY ON, BUFFERS OFF)
SELECT *
FROM cypher('wcoj_bench_semiring', $$
    MATCH (s1:S {id:1})-[e1:E1]->(t:T),
          (s2:S {id:2})-[e2:E2]->(t),
          (s3:S {id:3})-[e3:E3]->(t)
    RETURN id(t) AS key, count(DISTINCT id(t)) AS total
$$) AS (key agtype, total agtype);

\echo 'semiring sum property: first edge score'
EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY ON, BUFFERS OFF)
SELECT *
FROM cypher('wcoj_bench_semiring', $$
    MATCH (s1:S {id:1})-[e1:E1]->(t:T),
          (s2:S {id:2})-[e2:E2]->(t),
          (s3:S {id:3})-[e3:E3]->(t)
    RETURN sum(e1.score) AS total
$$) AS (total agtype);

\echo 'semiring group sum property: terminal key'
EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY ON, BUFFERS OFF)
SELECT *
FROM cypher('wcoj_bench_semiring', $$
    MATCH (s1:S {id:1})-[e1:E1]->(t:T),
          (s2:S {id:2})-[e2:E2]->(t),
          (s3:S {id:3})-[e3:E3]->(t)
    RETURN id(t) AS key, sum(e1.score) AS total
$$) AS (key agtype, total agtype);

\echo 'semiring limit: one demanded row'
EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY ON, BUFFERS OFF)
SELECT *
FROM cypher('wcoj_bench_semiring', $$
    MATCH (s1:S {id:1})-[e1:E1]->(t:T),
          (s2:S {id:2})-[e2:E2]->(t),
          (s3:S {id:3})-[e3:E3]->(t)
    RETURN id(e1), id(e2), id(e3), id(t)
    LIMIT 1
$$) AS (e1 agtype, e2 agtype, e3 agtype, tid agtype);

\echo 'semiring exists: one demanded row'
EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY ON, BUFFERS OFF)
SELECT *
FROM cypher('wcoj_bench_semiring', $$
    RETURN EXISTS {
        MATCH (s1:S {id:1})-[e1:E1]->(t:T),
              (s2:S {id:2})-[e2:E2]->(t),
              (s3:S {id:3})-[e3:E3]->(t)
        RETURN t
    } AS ok
$$) AS (ok agtype);
