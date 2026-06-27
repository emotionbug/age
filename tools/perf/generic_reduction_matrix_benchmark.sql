\set ON_ERROR_STOP on

LOAD 'age';
SET search_path = ag_catalog, "$user", public;
SET client_min_messages = warning;
SET jit = off;
SET max_parallel_workers_per_gather = 0;
SET join_collapse_limit = 1;
SET from_collapse_limit = 1;
SET age.enable_wcoj = on;
SET age.wcoj_engine = 'auto';
SET enable_nestloop = off;
SET enable_hashjoin = off;
SET enable_mergejoin = off;

\echo 'generic reduction matrix: acyclic leaf-pruned chain'
EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY ON, BUFFERS ON)
SELECT count(*)
FROM cypher('generic_reduction_matrix', $$
    MATCH (a:A)-[h:H]->(p:P),
          (a)-[f1:F1]->(b:B)-[f2:F2]->(c:C)-[f3:F3]->(d:D)
    RETURN id(a), id(b), id(c), id(d), id(p), id(h), id(f1), id(f2), id(f3)
$$) AS (a agtype, b agtype, c agtype, d agtype, p agtype,
        h agtype, f1 agtype, f2 agtype, f3 agtype);
