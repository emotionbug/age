\set ON_ERROR_STOP on

LOAD 'age';
SET search_path = ag_catalog, "$user", public;
SET client_min_messages = warning;
SET jit = off;
SET max_parallel_workers_per_gather = 0;
SET age.enable_wcoj = on;
SET age.wcoj_engine = 'auto';
SET enable_nestloop = off;
SET enable_hashjoin = off;
SET enable_mergejoin = off;

\echo 'generic preserve: four cycle'
EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY ON, BUFFERS ON)
SELECT count(*)
FROM cypher('generic_join_preserve', $$
    MATCH (a:A)-[e1:E1]->(b:B)-[e2:E2]->(c:C)
          -[e3:E3]->(d:D)-[e4:E4]->(a)
    RETURN id(a), id(b), id(c), id(d), id(e1), id(e2), id(e3), id(e4)
$$) AS (a agtype, b agtype, c agtype, d agtype,
        e1 agtype, e2 agtype, e3 agtype, e4 agtype);

\echo 'generic preserve: four cycle with two tails'
EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY ON, BUFFERS ON)
SELECT count(*)
FROM cypher('generic_join_preserve', $$
    MATCH (a:A)-[e1:E1]->(b:B)-[e2:E2]->(c:C)
          -[e3:E3]->(d:D)-[e4:E4]->(a),
          (c)-[tx:TX]->(x:X),
          (a)-[ty:TY]->(y:Y)
    RETURN id(a), id(b), id(c), id(d), id(x), id(y),
           id(e1), id(e2), id(e3), id(e4), id(tx), id(ty)
$$) AS (a agtype, b agtype, c agtype, d agtype, x agtype, y agtype,
        e1 agtype, e2 agtype, e3 agtype, e4 agtype,
        tx agtype, ty agtype);
