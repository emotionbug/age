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
--        -v label_fanout_labels=800 -v label_fanout_edges=64 \
--        -v run_standard_cases=1 \
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
\if :{?label_fanout_labels}
\else
    \set label_fanout_labels 800
\endif
\if :{?label_fanout_edges}
\else
    \set label_fanout_edges 64
\endif
\if :{?run_standard_cases}
\else
    \set run_standard_cases 0
\endif
\if :{?preserve_graph}
\else
    \set preserve_graph 0
\endif

\timing on

CREATE EXTENSION IF NOT EXISTS age;
LOAD 'age';
SET search_path TO ag_catalog, public;

CREATE TEMP TABLE vle_benchmark_config AS
SELECT :'graph'::text AS graph_name,
       :sparse_nodes::int AS sparse_nodes,
       :dense_nodes::int AS dense_nodes,
       :label_fanout_labels::int AS label_fanout_labels,
       :label_fanout_edges::int AS label_fanout_edges;

\if :preserve_graph
\else
\timing off
\o /dev/null
SET client_min_messages TO warning;
SELECT format('SELECT ag_catalog.drop_label(%L, %L, false);',
              c.graph_name, l.name)
FROM vle_benchmark_config c
JOIN ag_catalog.ag_graph g ON g.name = c.graph_name::name
JOIN ag_catalog.ag_label l ON l.graph = g.graphid
ORDER BY l.kind = 'v', l.name
\gexec
SET client_min_messages TO default;
\o
\timing on

SELECT ag_catalog.drop_graph(c.graph_name, true)
FROM vle_benchmark_config c
JOIN ag_catalog.ag_graph g ON g.name = c.graph_name::name;
\endif

SELECT create_graph(graph_name) FROM vle_benchmark_config;

DO $$
DECLARE
    graph_name text;
    sparse_nodes int;
    dense_nodes int;
    label_fanout_edges int;
    i int;
    j int;
BEGIN
    SELECT c.graph_name, c.sparse_nodes, c.dense_nodes,
           c.label_fanout_edges
    INTO graph_name, sparse_nodes, dense_nodes,
         label_fanout_edges
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

    EXECUTE format(
        'SELECT * FROM ag_catalog.cypher(%L, $vle_bench$
         CREATE (:FanoutStart {i: 0})
         $vle_bench$) AS (r ag_catalog.agtype)',
        graph_name);

    FOR i IN 0..label_fanout_edges - 1 LOOP
        EXECUTE format(
            'SELECT * FROM ag_catalog.cypher(%L, $vle_bench$%s$vle_bench$) AS (r ag_catalog.agtype)',
            graph_name,
            format(
                'MATCH (a:FanoutStart {i: 0})
                 CREATE (a)-[:FanoutEdge {bucket: %s}]->(:FanoutTarget {i: %s})',
                i % 8, i));
    END LOOP;

END
$$;

\timing off
\o /dev/null
SELECT format(
    'SELECT * FROM ag_catalog.cypher(%L, $vle_bench$%s$vle_bench$) AS (r ag_catalog.agtype);',
    c.graph_name,
    format(
        'CREATE (:NoiseNode_%s {i: %s})-[:NoiseEdge_%s {i: %s}]->(:NoiseSink_%s {i: %s})',
        i, i, i, i, i, i))
FROM vle_benchmark_config c,
     generate_series(0, c.label_fanout_labels - 1) AS i
\gexec
\o
\timing on

DO $$
DECLARE
    graph_name text;
BEGIN
    SELECT c.graph_name INTO graph_name
    FROM vle_benchmark_config c;

    EXECUTE format(
        'CREATE INDEX %I ON %I.%I USING gin (properties)',
        graph_name || '_fanout_start_properties_gin_idx',
        graph_name, 'FanoutStart');
    EXECUTE format(
        'CREATE INDEX %I ON %I.%I USING btree (start_id)',
        graph_name || '_fanout_edge_start_idx',
        graph_name, 'FanoutEdge');
    EXECUTE format(
        'CREATE INDEX %I ON %I.%I USING btree (end_id)',
        graph_name || '_fanout_edge_end_idx',
        graph_name, 'FanoutEdge');
    EXECUTE format(
        'CREATE INDEX %I ON %I.%I USING age_adjacency (start_id, id, end_id)',
        graph_name || '_fanout_edge_start_adj_idx',
        graph_name, 'FanoutEdge');
    EXECUTE format(
        'CREATE INDEX %I ON %I.%I USING age_adjacency (end_id, id, start_id)',
        graph_name || '_fanout_edge_end_adj_idx',
        graph_name, 'FanoutEdge');
    EXECUTE format('ANALYZE %I.%I', graph_name, 'FanoutStart');
    EXECUTE format('ANALYZE %I.%I', graph_name, 'FanoutEdge');
END
$$;

CREATE TEMP TABLE vle_benchmark_results
(
    shape text PRIMARY KEY,
    rows_returned bigint NOT NULL,
    elapsed_ms numeric NOT NULL
);

CREATE TEMP TABLE vle_benchmark_explain
(
    shape text NOT NULL,
    line_no bigserial NOT NULL,
    plan_line text NOT NULL
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

CREATE OR REPLACE FUNCTION public.capture_vle_benchmark_explain(
    graph_name text,
    shape text,
    query text)
RETURNS void
LANGUAGE plpgsql
AS $$
DECLARE
    plan_line text;
BEGIN
    FOR plan_line IN EXECUTE format(
        'EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY OFF)
         SELECT *
         FROM ag_catalog.cypher(%L, $vle_bench$%s$vle_bench$)
              AS (result ag_catalog.agtype)',
        graph_name, query)
    LOOP
        INSERT INTO vle_benchmark_explain(shape, plan_line)
        VALUES (shape, plan_line);
    END LOOP;
END
$$;

\if :run_standard_cases
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
\endif

SELECT public.run_vle_benchmark_case(graph_name, '800-label-fanout-terminal',
    'MATCH (:FanoutStart {i: 0})-[:FanoutEdge*1..2]->(n) RETURN n.i')
FROM vle_benchmark_config;

SELECT public.capture_vle_benchmark_explain(graph_name,
    '800-label-fanout-terminal',
    'MATCH (:FanoutStart {i: 0})-[:FanoutEdge*1..2]->(n) RETURN n.i')
FROM vle_benchmark_config;

TABLE vle_benchmark_results ORDER BY shape;

SELECT shape, line_no, plan_line
FROM vle_benchmark_explain
WHERE plan_line LIKE '%VLE%'
   OR plan_line LIKE '%source%'
ORDER BY shape, line_no;

DROP FUNCTION public.capture_vle_benchmark_explain(text, text, text);
DROP FUNCTION public.run_vle_benchmark_case(text, text, text);
\if :preserve_graph
\else
\timing off
\o /dev/null
SET client_min_messages TO warning;
SELECT format('SELECT ag_catalog.drop_label(%L, %L, false);',
              c.graph_name, l.name)
FROM vle_benchmark_config c
JOIN ag_catalog.ag_graph g ON g.name = c.graph_name::name
JOIN ag_catalog.ag_label l ON l.graph = g.graphid
ORDER BY l.kind = 'v', l.name
\gexec
SET client_min_messages TO default;
\o
\timing on

SELECT ag_catalog.drop_graph(c.graph_name, true)
FROM vle_benchmark_config c
JOIN ag_catalog.ag_graph g ON g.name = c.graph_name::name;
\endif
