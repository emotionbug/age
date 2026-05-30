-- VLE-focused benchmark harness.
--
-- Run from psql after installing AGE, for example:
--
--   psql -d postgres -f tools/vle_benchmark.sql
--
-- Optional psql variables:
--
--   psql -d postgres -v graph=age_vle_bench \
--        -v sparse_nodes=128 -v dense_nodes=24 \
--        -f tools/vle_benchmark.sql

\set ON_ERROR_STOP on

\if :{?graph}
\else
    \set graph age_vle_bench
\endif
\if :{?sparse_nodes}
\else
    \set sparse_nodes 128
\endif
\if :{?dense_nodes}
\else
    \set dense_nodes 24
\endif

\timing on

CREATE EXTENSION IF NOT EXISTS age;
LOAD 'age';
SET search_path TO ag_catalog, public;

CREATE TEMP TABLE vle_benchmark_config AS
SELECT :'graph'::text AS graph_name,
       :sparse_nodes::int AS sparse_nodes,
       :dense_nodes::int AS dense_nodes;

DO $$
DECLARE
    graph_name text;
BEGIN
    SELECT c.graph_name INTO graph_name FROM vle_benchmark_config c;
    PERFORM drop_graph(graph_name, true);
EXCEPTION WHEN OTHERS THEN
    NULL;
END
$$;

SELECT create_graph(graph_name) FROM vle_benchmark_config;

DO $$
DECLARE
    graph_name text;
    sparse_nodes int;
    dense_nodes int;
    i int;
    j int;
BEGIN
    SELECT c.graph_name, c.sparse_nodes, c.dense_nodes
    INTO graph_name, sparse_nodes, dense_nodes
    FROM vle_benchmark_config c;

    FOR i IN 0..sparse_nodes - 1 LOOP
        EXECUTE format(
            'SELECT * FROM ag_catalog.cypher(%L, $vle_bench$%s$vle_bench$) AS (r ag_catalog.agtype)',
            graph_name,
            format('CREATE (:Sparse {i: %s})', i));
    END LOOP;

    FOR i IN 0..sparse_nodes - 2 LOOP
        EXECUTE format(
            'SELECT * FROM ag_catalog.cypher(%L, $vle_bench$%s$vle_bench$) AS (r ag_catalog.agtype)',
            graph_name,
            format(
                'MATCH (a:Sparse {i: %s}), (b:Sparse {i: %s})
                 CREATE (a)-[:SparseEdge {weight: %s, parity: "%s"}]->(b)',
                i, i + 1, i % 7,
                CASE WHEN i % 2 = 0 THEN 'even' ELSE 'odd' END));
    END LOOP;

    FOR i IN 0..dense_nodes - 1 LOOP
        EXECUTE format(
            'SELECT * FROM ag_catalog.cypher(%L, $vle_bench$%s$vle_bench$) AS (r ag_catalog.agtype)',
            graph_name,
            format('CREATE (:Dense {i: %s})', i));
    END LOOP;

    FOR i IN 0..dense_nodes - 1 LOOP
        FOR j IN 0..dense_nodes - 1 LOOP
            IF i <> j AND (i + j) % 3 = 0 THEN
                EXECUTE format(
                    'SELECT * FROM ag_catalog.cypher(%L, $vle_bench$%s$vle_bench$) AS (r ag_catalog.agtype)',
                    graph_name,
                    format(
                        'MATCH (a:Dense {i: %s}), (b:Dense {i: %s})
                         CREATE (a)-[:DenseEdge {bucket: %s, hot: "%s"}]->(b)',
                        i, j, (i + j) % 5,
                        CASE WHEN (i * j) % 2 = 0 THEN 'yes' ELSE 'no' END));
            END IF;
        END LOOP;
    END LOOP;
END
$$;

CREATE TEMP TABLE vle_benchmark_results
(
    shape text PRIMARY KEY,
    rows_returned bigint NOT NULL,
    elapsed_ms numeric NOT NULL
);

CREATE OR REPLACE FUNCTION public.run_vle_benchmark_case(
    graph_name text,
    shape text,
    query text)
RETURNS void
LANGUAGE plpgsql
AS $$
DECLARE
    started_at timestamptz;
    rows_returned bigint;
BEGIN
    started_at := clock_timestamp();

    EXECUTE format(
        'SELECT count(*) FROM ag_catalog.cypher(%L, $vle_bench$%s$vle_bench$) AS (result ag_catalog.agtype)',
        graph_name, query)
    INTO rows_returned;

    INSERT INTO vle_benchmark_results(shape, rows_returned, elapsed_ms)
    VALUES
    (
        shape,
        rows_returned,
        round((extract(epoch FROM clock_timestamp() - started_at) * 1000)::numeric, 3)
    );
END
$$;

SELECT public.run_vle_benchmark_case(graph_name, 'two-bound',
    'MATCH p=(:Sparse {i: 0})-[*1..8]->(:Sparse {i: 8}) RETURN p')
FROM vle_benchmark_config;

SELECT public.run_vle_benchmark_case(graph_name, 'one-bound',
    'MATCH p=(:Sparse {i: 0})-[*1..6]->() RETURN p')
FROM vle_benchmark_config;

SELECT public.run_vle_benchmark_case(graph_name, 'all-roots',
    'MATCH p=()-[*1..3]->() RETURN p')
FROM vle_benchmark_config;

SELECT public.run_vle_benchmark_case(graph_name, 'labeled',
    'MATCH p=()-[:SparseEdge*1..6]->() RETURN p')
FROM vle_benchmark_config;

SELECT public.run_vle_benchmark_case(graph_name, 'property-filtered',
    'MATCH p=()-[:SparseEdge*1..6 {parity: "even"}]->() RETURN p')
FROM vle_benchmark_config;

SELECT public.run_vle_benchmark_case(graph_name, 'bounded-upper',
    'MATCH p=(:Sparse {i: 0})-[*1..12]->() RETURN p')
FROM vle_benchmark_config;

SELECT public.run_vle_benchmark_case(graph_name, 'unbounded-upper',
    'MATCH p=(:Sparse {i: 0})-[*]->() RETURN p')
FROM vle_benchmark_config;

SELECT public.run_vle_benchmark_case(graph_name, 'sparse-graph',
    'MATCH p=(:Sparse {i: 0})-[:SparseEdge*1..16]->() RETURN p')
FROM vle_benchmark_config;

SELECT public.run_vle_benchmark_case(graph_name, 'dense-graph',
    'MATCH p=(:Dense {i: 0})-[:DenseEdge*1..3]->() RETURN p')
FROM vle_benchmark_config;

TABLE vle_benchmark_results ORDER BY shape;

DROP FUNCTION public.run_vle_benchmark_case(text, text, text);
SELECT drop_graph(graph_name, true) FROM vle_benchmark_config;
