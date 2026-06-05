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
--        -v value_posting_edges=38 \
--        -v replay_branches=0 -v replay_leaves=0 \
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
\if :{?value_posting_edges}
\else
    \set value_posting_edges 38
\endif
\if :{?replay_branches}
\else
    \set replay_branches 0
\endif
\if :{?replay_leaves}
\else
    \set replay_leaves 0
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
WITH input AS (
    SELECT :'graph'::text AS graph_name,
           :sparse_nodes::int AS sparse_nodes,
           :dense_nodes::int AS dense_nodes,
           :label_fanout_labels::int AS label_fanout_labels,
           :label_fanout_edges::int AS label_fanout_edges,
           :value_posting_edges::int AS value_posting_edges,
           :replay_branches::int AS replay_branches_input,
           :replay_leaves::int AS replay_leaves_input
)
SELECT graph_name,
       sparse_nodes,
       dense_nodes,
       label_fanout_labels,
       label_fanout_edges,
       value_posting_edges,
       GREATEST((GREATEST(value_posting_edges, 2) + 255) / 256, 1)
           AS value_posting_root_count,
       (GREATEST(value_posting_edges, 2) +
        GREATEST((GREATEST(value_posting_edges, 2) + 255) / 256, 1) - 1) /
        GREATEST((GREATEST(value_posting_edges, 2) + 255) / 256, 1)
           AS value_posting_edges_per_root,
       replay_branches_input,
       replay_leaves_input,
       CASE WHEN replay_branches_input > 0
            THEN replay_branches_input
            ELSE GREATEST(label_fanout_edges, 4)
       END AS replay_branches,
       CASE WHEN replay_leaves_input > 0
            THEN replay_leaves_input
            ELSE LEAST(GREATEST(label_fanout_edges / 2, 4), 16)
       END AS replay_leaves
FROM input;

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
    value_posting_edges int;
    value_posting_root_count int;
    value_posting_edges_per_root int;
    replay_branches int;
    replay_leaves int;
    i int;
    j int;
BEGIN
    SELECT c.graph_name, c.sparse_nodes, c.dense_nodes,
           c.label_fanout_edges, c.value_posting_edges,
           c.value_posting_root_count, c.value_posting_edges_per_root,
           c.replay_branches, c.replay_leaves
    INTO graph_name, sparse_nodes, dense_nodes,
         label_fanout_edges, value_posting_edges,
         value_posting_root_count, value_posting_edges_per_root,
         replay_branches, replay_leaves
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
         CREATE (:FanoutStart {i: 0}), (:FanoutEnd {i: 0})
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
        EXECUTE format(
            'SELECT * FROM ag_catalog.cypher(%L, $vle_bench$%s$vle_bench$) AS (r ag_catalog.agtype)',
            graph_name,
            format(
                'MATCH (b:FanoutEnd {i: 0})
                 CREATE (:FanoutSource {i: %s})-[:FanoutEdge {bucket: %s}]->(b)',
                i, i % 8));
    END LOOP;

    EXECUTE format(
        'SELECT * FROM ag_catalog.cypher(%L, $vle_bench$
         CREATE (:ReplayStart {i: 0}), (:ReplayHub {i: 0})
         $vle_bench$) AS (r ag_catalog.agtype)',
        graph_name);

    FOR i IN 0..replay_branches - 1 LOOP
        EXECUTE format(
            'SELECT * FROM ag_catalog.cypher(%L, $vle_bench$%s$vle_bench$) AS (r ag_catalog.agtype)',
            graph_name,
            format(
                'MATCH (s:ReplayStart {i: 0}), (h:ReplayHub {i: 0})
                 CREATE (s)-[:ReplayEdge {bucket: %s}]->(:ReplayMid {i: %s})-[:ReplayEdge {bucket: %s}]->(h)',
                i % 8, i, i % 8));
    END LOOP;

    FOR i IN 0..replay_leaves - 1 LOOP
        EXECUTE format(
            'SELECT * FROM ag_catalog.cypher(%L, $vle_bench$%s$vle_bench$) AS (r ag_catalog.agtype)',
            graph_name,
            format(
                'MATCH (h:ReplayHub {i: 0})
                 CREATE (h)-[:ReplayEdge {bucket: %s}]->(:ReplayLeaf {i: %s})',
                i % 8, i));
    END LOOP;

    PERFORM ag_catalog.create_vlabel(graph_name::cstring,
                                     'ValuePostingNode'::cstring);
    PERFORM ag_catalog.create_vlabel(graph_name::cstring,
                                     'ValuePostingOther'::cstring);
    PERFORM ag_catalog.create_elabel(graph_name::cstring,
                                     'ValuePostingEdge'::cstring);

    EXECUTE format(
        'INSERT INTO %I.%I(id, properties)
         WITH roots AS (
             SELECT generate_series(0, %s - 1) AS root
         ),
         endpoints AS (
             SELECT 0 AS root, 1 AS g
             UNION ALL
             SELECT r.root, 220 + r.root * %s + endpoint_offset AS g
             FROM roots r,
                  generate_series(
                      0,
                      %s - CASE WHEN r.root = 0 THEN 2 ELSE 1 END
                  ) AS endpoint_offset
         )
         SELECT ag_catalog._graphid(ag_catalog._label_id(%L, %L),
                                    199000 + r.root),
                (''{"i": 199, "role": "vp_start", "bucket": '' ||
                 r.root || ''}'')::ag_catalog.agtype
         FROM roots r
         UNION ALL
         SELECT ag_catalog._graphid(ag_catalog._label_id(%L, %L), 62),
                ''{"i": 59, "role": "vp_candidate"}''::ag_catalog.agtype
         UNION ALL
         SELECT ag_catalog._graphid(ag_catalog._label_id(%L, %L), e.g),
                (''{"i": '' || e.g || '', "role": "vp_endpoint"}'')::ag_catalog.agtype
         FROM endpoints e',
        graph_name, 'ValuePostingNode',
        value_posting_root_count,
        value_posting_edges_per_root,
        value_posting_edges_per_root,
        graph_name, 'ValuePostingNode',
        graph_name, 'ValuePostingNode',
        graph_name, 'ValuePostingNode');

    EXECUTE format(
        'INSERT INTO %I.%I(id, properties)
         WITH roots AS (
             SELECT generate_series(0, %s - 1) AS root
         ),
         mids AS (
             SELECT r.root,
                    500000 + r.root * %s + mid_offset AS g
             FROM roots r,
                  generate_series(0, %s - 1) AS mid_offset
         ),
         leaves AS (
             SELECT 800000 + leaf_offset AS g
             FROM generate_series(0, %s - 1) AS leaf_offset
         )
         SELECT ag_catalog._graphid(ag_catalog._label_id(%L, %L), m.g),
                (''{"i": '' || m.g || '', "role": "vp_replay_mid"}'')::ag_catalog.agtype
         FROM mids m
         UNION ALL
         SELECT ag_catalog._graphid(ag_catalog._label_id(%L, %L),
                                    299000 + r.root),
                (''{"i": 299, "role": "vp_replay_start", "bucket": '' ||
                 r.root || ''}'')::ag_catalog.agtype
         FROM roots r
         UNION ALL
         SELECT ag_catalog._graphid(ag_catalog._label_id(%L, %L), 700000),
                ''{"i": 700000, "role": "vp_replay_hub"}''::ag_catalog.agtype
         UNION ALL
         SELECT ag_catalog._graphid(ag_catalog._label_id(%L, %L), l.g),
                (''{"i": '' || l.g || '', "role": "vp_replay_leaf"}'')::ag_catalog.agtype
         FROM leaves l',
        graph_name, 'ValuePostingNode',
        value_posting_root_count,
        value_posting_edges_per_root,
        value_posting_edges_per_root,
        value_posting_edges_per_root,
        graph_name, 'ValuePostingNode',
        graph_name, 'ValuePostingNode',
        graph_name, 'ValuePostingNode',
        graph_name, 'ValuePostingNode');

    EXECUTE format(
        'INSERT INTO %I.%I(id, properties)
         SELECT ag_catalog._graphid(ag_catalog._label_id(%L, %L),
                                    r.root * 1000 + g),
                ''{}''::ag_catalog.agtype
         FROM generate_series(0, %s - 1) r(root),
              generate_series(1, GREATEST(%s / 8, 4)) g',
        graph_name, 'ValuePostingOther',
        graph_name, 'ValuePostingOther',
        value_posting_root_count,
        value_posting_edges_per_root);

    EXECUTE format(
        'INSERT INTO %I.%I(id, start_id, end_id, properties)
         WITH roots AS (
             SELECT generate_series(0, %s - 1) AS root
         ),
         endpoints AS (
             SELECT 0 AS root, 1 AS g
             UNION ALL
             SELECT r.root, 220 + r.root * %s + endpoint_offset AS g
             FROM roots r,
                  generate_series(
                      0,
                      %s - CASE WHEN r.root = 0 THEN 2 ELSE 1 END
                  ) AS endpoint_offset
         )
         SELECT ag_catalog._graphid(ag_catalog._label_id(%L, %L),
                                    e.root * 1000000 + e.g),
                ag_catalog._graphid(ag_catalog._label_id(%L, %L),
                                    199000 + e.root),
                ag_catalog._graphid(ag_catalog._label_id(%L, %L), e.g),
                ''{}''::ag_catalog.agtype
         FROM endpoints e
         UNION ALL
         SELECT ag_catalog._graphid(ag_catalog._label_id(%L, %L),
                                    r.root * 1000000 + 500000 + g),
                ag_catalog._graphid(ag_catalog._label_id(%L, %L),
                                    199000 + r.root),
                ag_catalog._graphid(ag_catalog._label_id(%L, %L),
                                    r.root * 1000 + g),
                ''{}''::ag_catalog.agtype
         FROM roots r,
              generate_series(1, GREATEST(%s / 8, 4)) g',
        graph_name, 'ValuePostingEdge',
        value_posting_root_count,
        value_posting_edges_per_root,
        value_posting_edges_per_root,
        graph_name, 'ValuePostingEdge',
        graph_name, 'ValuePostingNode',
        graph_name, 'ValuePostingNode',
        graph_name, 'ValuePostingEdge',
        graph_name, 'ValuePostingNode',
        graph_name, 'ValuePostingOther',
        value_posting_edges_per_root);

    EXECUTE format(
        'INSERT INTO %I.%I(id, start_id, end_id, properties)
         WITH roots AS (
             SELECT generate_series(0, %s - 1) AS root
         ),
         mids AS (
             SELECT r.root,
                    500000 + r.root * %s + mid_offset AS g,
                    mid_offset
             FROM roots r,
                  generate_series(0, %s - 1) AS mid_offset
         ),
         leaves AS (
             SELECT 800000 + leaf_offset AS g,
                    leaf_offset
             FROM generate_series(0, %s - 1) AS leaf_offset
         )
         SELECT ag_catalog._graphid(ag_catalog._label_id(%L, %L),
                                    2000000000 + m.root * %s + m.mid_offset),
                ag_catalog._graphid(ag_catalog._label_id(%L, %L),
                                    299000 + m.root),
                ag_catalog._graphid(ag_catalog._label_id(%L, %L), m.g),
                ''{}''::ag_catalog.agtype
         FROM mids m
         UNION ALL
         SELECT ag_catalog._graphid(ag_catalog._label_id(%L, %L),
                                    2100000000 + m.root * %s + m.mid_offset),
                ag_catalog._graphid(ag_catalog._label_id(%L, %L), m.g),
                ag_catalog._graphid(ag_catalog._label_id(%L, %L), 700000),
                ''{}''::ag_catalog.agtype
         FROM mids m
         UNION ALL
         SELECT ag_catalog._graphid(ag_catalog._label_id(%L, %L),
                                    2200000000 + l.leaf_offset),
                ag_catalog._graphid(ag_catalog._label_id(%L, %L), 700000),
                ag_catalog._graphid(ag_catalog._label_id(%L, %L), l.g),
                ''{}''::ag_catalog.agtype
         FROM leaves l',
        graph_name, 'ValuePostingEdge',
        value_posting_root_count,
        value_posting_edges_per_root,
        value_posting_edges_per_root,
        value_posting_edges_per_root,
        graph_name, 'ValuePostingEdge',
        value_posting_edges_per_root,
        graph_name, 'ValuePostingNode',
        graph_name, 'ValuePostingNode',
        graph_name, 'ValuePostingEdge',
        value_posting_edges_per_root,
        graph_name, 'ValuePostingNode',
        graph_name, 'ValuePostingNode',
        graph_name, 'ValuePostingEdge',
        graph_name, 'ValuePostingNode',
        graph_name, 'ValuePostingNode');

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
        'CREATE INDEX %I ON %I.%I USING gin (properties)',
        graph_name || '_fanout_end_properties_gin_idx',
        graph_name, 'FanoutEnd');
    EXECUTE format(
        'CREATE INDEX %I ON %I.%I USING gin (properties)',
        graph_name || '_replay_start_properties_gin_idx',
        graph_name, 'ReplayStart');
    EXECUTE format(
        'CREATE INDEX %I ON %I.%I USING gin (properties)',
        graph_name || '_replay_hub_properties_gin_idx',
        graph_name, 'ReplayHub');
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
    EXECUTE format(
        'CREATE INDEX %I ON %I.%I USING btree (start_id)',
        graph_name || '_replay_edge_start_idx',
        graph_name, 'ReplayEdge');
    EXECUTE format(
        'CREATE INDEX %I ON %I.%I USING btree (end_id)',
        graph_name || '_replay_edge_end_idx',
        graph_name, 'ReplayEdge');
    EXECUTE format(
        'CREATE INDEX %I ON %I.%I USING age_adjacency (start_id, id, end_id)',
        graph_name || '_replay_edge_start_adj_idx',
        graph_name, 'ReplayEdge');
    EXECUTE format(
        'CREATE INDEX %I ON %I.%I USING age_adjacency (end_id, id, start_id)',
        graph_name || '_replay_edge_end_adj_idx',
        graph_name, 'ReplayEdge');
    EXECUTE format(
        'SELECT * FROM ag_catalog.cypher(%L, $vle_bench$
         CREATE INDEX value_posting_node_i_source FOR (n:ValuePostingNode) ON (n.i)
         $vle_bench$) AS (create_index text)',
        graph_name);
    EXECUTE format(
        'CREATE INDEX %I ON %I.%I USING age_adjacency (start_id, id, end_id)',
        graph_name || '_value_posting_edge_start_adj_idx',
        graph_name, 'ValuePostingEdge');
    EXECUTE format(
        'CREATE INDEX %I ON %I.%I USING age_adjacency (end_id, id, start_id)',
        graph_name || '_value_posting_edge_end_adj_idx',
        graph_name, 'ValuePostingEdge');
    EXECUTE format('ANALYZE %I.%I', graph_name, 'FanoutStart');
    EXECUTE format('ANALYZE %I.%I', graph_name, 'FanoutEnd');
    EXECUTE format('ANALYZE %I.%I', graph_name, 'FanoutEdge');
    EXECUTE format('ANALYZE %I.%I', graph_name, 'ReplayStart');
    EXECUTE format('ANALYZE %I.%I', graph_name, 'ReplayHub');
    EXECUTE format('ANALYZE %I.%I', graph_name, 'ReplayEdge');
    EXECUTE format('ANALYZE %I.%I', graph_name, 'ValuePostingNode');
    EXECUTE format('ANALYZE %I.%I', graph_name, 'ValuePostingOther');
    EXECUTE format('ANALYZE %I.%I', graph_name, 'ValuePostingEdge');
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

SELECT public.run_vle_benchmark_case(graph_name, '800-label-fanout-path',
    'MATCH p=(:FanoutStart {i: 0})-[:FanoutEdge*1..2]->(n) RETURN p')
FROM vle_benchmark_config;

SELECT public.run_vle_benchmark_case(graph_name, '800-label-fanout-vertex',
    'MATCH (:FanoutStart {i: 0})-[:FanoutEdge*1..2]->(n) RETURN n')
FROM vle_benchmark_config;

SELECT public.run_vle_benchmark_case(graph_name, '800-label-fanout-properties',
    'MATCH (:FanoutStart {i: 0})-[:FanoutEdge*1..2]->(n) RETURN properties(n)')
FROM vle_benchmark_config;

SELECT public.run_vle_benchmark_case(graph_name, '800-label-fanout-left-terminal',
    'MATCH (:FanoutEnd {i: 0})<-[:FanoutEdge*1..2]-(n) RETURN n.i')
FROM vle_benchmark_config;

SELECT public.run_vle_benchmark_case(graph_name, '800-label-fanout-left-path',
    'MATCH p=(:FanoutEnd {i: 0})<-[:FanoutEdge*1..2]-(n) RETURN p')
FROM vle_benchmark_config;

SELECT public.run_vle_benchmark_case(graph_name, 'payload-replay-path',
    'MATCH p=(:ReplayStart {i: 0})-[:ReplayEdge*1..3]->(n) RETURN p')
FROM vle_benchmark_config;

SELECT public.run_vle_benchmark_case(graph_name, 'payload-replay-terminal',
    'MATCH (:ReplayStart {i: 0})-[:ReplayEdge*1..3]->(n) RETURN n.i')
FROM vle_benchmark_config;

SELECT public.run_vle_benchmark_case(graph_name, 'payload-replay-vertex',
    'MATCH (:ReplayStart {i: 0})-[:ReplayEdge*1..3]->(n) RETURN n')
FROM vle_benchmark_config;

SELECT public.run_vle_benchmark_case(graph_name, 'payload-replay-properties',
    'MATCH (:ReplayStart {i: 0})-[:ReplayEdge*1..3]->(n) RETURN properties(n)')
FROM vle_benchmark_config;

SELECT public.capture_vle_benchmark_explain(graph_name,
    '800-label-fanout-terminal',
    'MATCH (:FanoutStart {i: 0})-[:FanoutEdge*1..2]->(n) RETURN n.i')
FROM vle_benchmark_config;

SELECT public.capture_vle_benchmark_explain(graph_name,
    '800-label-fanout-path',
    'MATCH p=(:FanoutStart {i: 0})-[:FanoutEdge*1..2]->(n) RETURN p')
FROM vle_benchmark_config;

SELECT public.capture_vle_benchmark_explain(graph_name,
    '800-label-fanout-vertex',
    'MATCH (:FanoutStart {i: 0})-[:FanoutEdge*1..2]->(n) RETURN n')
FROM vle_benchmark_config;

SELECT public.capture_vle_benchmark_explain(graph_name,
    '800-label-fanout-properties',
    'MATCH (:FanoutStart {i: 0})-[:FanoutEdge*1..2]->(n) RETURN properties(n)')
FROM vle_benchmark_config;

SELECT public.capture_vle_benchmark_explain(graph_name,
    '800-label-fanout-left-terminal',
    'MATCH (:FanoutEnd {i: 0})<-[:FanoutEdge*1..2]-(n) RETURN n.i')
FROM vle_benchmark_config;

SELECT public.capture_vle_benchmark_explain(graph_name,
    '800-label-fanout-left-path',
    'MATCH p=(:FanoutEnd {i: 0})<-[:FanoutEdge*1..2]-(n) RETURN p')
FROM vle_benchmark_config;

SELECT public.capture_vle_benchmark_explain(graph_name,
    '800-label-fanout-family-path',
    'MATCH p=(:FanoutStart {i: 0})-[:FanoutEdge*1..2]-(n) RETURN p')
FROM vle_benchmark_config;

SELECT public.run_vle_benchmark_case(graph_name, '800-label-fanout-family-path',
    'MATCH p=(:FanoutStart {i: 0})-[:FanoutEdge*1..2]-(n) RETURN p')
FROM vle_benchmark_config;

SELECT public.capture_vle_benchmark_explain(graph_name,
    'payload-replay-path',
    'MATCH p=(:ReplayStart {i: 0})-[:ReplayEdge*1..3]->(n) RETURN p')
FROM vle_benchmark_config;

SELECT public.capture_vle_benchmark_explain(graph_name,
    'payload-replay-terminal',
    'MATCH (:ReplayStart {i: 0})-[:ReplayEdge*1..3]->(n) RETURN n.i')
FROM vle_benchmark_config;

SELECT public.capture_vle_benchmark_explain(graph_name,
    'payload-replay-vertex',
    'MATCH (:ReplayStart {i: 0})-[:ReplayEdge*1..3]->(n) RETURN n')
FROM vle_benchmark_config;

SELECT public.capture_vle_benchmark_explain(graph_name,
    'payload-replay-properties',
    'MATCH (:ReplayStart {i: 0})-[:ReplayEdge*1..3]->(n) RETURN properties(n)')
FROM vle_benchmark_config;

SELECT public.capture_vle_benchmark_explain(graph_name,
    'value-posting-reject-seed',
    'MATCH (:ValuePostingNode {i: 199})-[:ValuePostingEdge*1..1]->(n:ValuePostingNode)
     WHERE n.i = 59
     RETURN n.i')
FROM vle_benchmark_config;

SELECT public.capture_vle_benchmark_explain(graph_name,
    'value-posting-reject',
    'MATCH (:ValuePostingNode {i: 199})-[:ValuePostingEdge*1..1]->(n:ValuePostingNode)
     WHERE n.i = 59
     RETURN n.i')
FROM vle_benchmark_config;

SELECT public.capture_vle_benchmark_explain(graph_name,
    'value-posting-endpoint-control',
    'MATCH (:ValuePostingNode {i: 199})-[:ValuePostingEdge*1..1]->(n:ValuePostingNode)
     WHERE n.i = 1
     RETURN n.i')
FROM vle_benchmark_config;

SELECT public.capture_vle_benchmark_explain(graph_name,
    'value-posting-replay-seed',
    'MATCH (:ValuePostingNode {i: 299})-[:ValuePostingEdge*1..3]->(n:ValuePostingNode)
     WHERE n.i = 59
     RETURN n.i')
FROM vle_benchmark_config;

SELECT public.capture_vle_benchmark_explain(graph_name,
    'value-posting-replay',
    'MATCH (:ValuePostingNode {i: 299})-[:ValuePostingEdge*1..3]->(n:ValuePostingNode)
     WHERE n.i = 59
     RETURN n.i')
FROM vle_benchmark_config;

TABLE vle_benchmark_results ORDER BY shape;

SELECT shape, line_no, plan_line
FROM vle_benchmark_explain
WHERE plan_line LIKE '%VLE%'
   OR plan_line LIKE '%source%'
ORDER BY shape, line_no;

SELECT shape,
       substring(plan_line FROM 'dominant=([^ ]+)') AS dominant_source,
       substring(plan_line FROM ' class=([^ ]+)') AS source_class,
       substring(plan_line FROM 'pressure=([^ ]+)') AS runtime_pressure,
       substring(plan_line FROM 'action=([^ ]+)') AS runtime_action
FROM vle_benchmark_explain
WHERE plan_line LIKE '%VLE Source Runtime%'
ORDER BY shape, line_no;

WITH planner_profile AS (
    SELECT DISTINCT ON (shape) shape, plan_line
    FROM vle_benchmark_explain
    WHERE plan_line LIKE '%VLE Source Profile:%'
    ORDER BY shape, line_no
),
planner_threshold AS (
    SELECT DISTINCT ON (shape) shape, plan_line
    FROM vle_benchmark_explain
    WHERE plan_line LIKE '%VLE Source Threshold Input:%'
    ORDER BY shape, line_no
),
planner_payload AS (
    SELECT DISTINCT ON (shape) shape, plan_line
    FROM vle_benchmark_explain
    WHERE plan_line LIKE '%VLE Source Payload Input:%'
    ORDER BY shape, line_no
),
planner_policy AS (
    SELECT DISTINCT ON (shape) shape, plan_line
    FROM vle_benchmark_explain
    WHERE plan_line LIKE '%VLE Source Policy:%'
    ORDER BY shape, line_no
)
SELECT p.shape,
       substring(p.plan_line FROM 'consumer=([^ ]+)') AS consumer,
       substring(p.plan_line FROM ' class=([^ ]+)') AS consumer_class,
       substring(p.plan_line FROM 'active=([^ ]+)') AS active_direction,
       substring(p.plan_line FROM 'budget=([^ ]+)') AS fanout_budget,
       substring(p.plan_line FROM 'weight=([^ ]+)') AS materialization_weight,
       substring(pol.plan_line FROM 'VLE Source Policy: ([^ ]+)') AS planner_policy,
       substring(pol.plan_line FROM ' reason=([^ ]+)') AS policy_reason,
       substring(pol.plan_line FROM 'combined-work=([^ ]+)') AS combined_work,
       substring(pol.plan_line FROM 'value-posting=([^ ]+)') AS value_posting_source,
       substring(p.plan_line FROM 'cache-seed=([^ ]+)') AS cache_seed,
       substring(p.plan_line FROM 'endpoint-headroom=([^ ]+)') AS endpoint_headroom,
       substring(p.plan_line FROM 'empty-lifecycle=([^/]+)') AS empty_lifecycle,
       substring(p.plan_line FROM 'empty-lifecycle=[^/]+/depth:([^ ]+)') AS empty_lifecycle_depth,
       substring(p.plan_line FROM 'empty-batch=([^/]+)') AS empty_batch,
       substring(p.plan_line FROM 'empty-batch=[^/]+/size:([^ ]+)') AS empty_batch_size,
       substring(t.plan_line FROM 'source=([^ ]+)') AS threshold_input,
       substring(t.plan_line FROM 'headroom=([^ ]+)') AS threshold_input_headroom,
       substring(t.plan_line FROM 'batch=([^ ]+)') AS threshold_input_batch,
       substring(t.plan_line FROM 'direction=([^ ]+)') AS threshold_input_source,
       COALESCE(substring(p.plan_line FROM 'active=([^ ]+)') = 'both' AND
           substring(t.plan_line FROM 'direction=([^ ]+)') IN ('out', 'in', 'mixed'),
           false) AS threshold_directional_family,
       substring(t.plan_line FROM 'reason=([^ ]+)') AS threshold_input_reason,
       substring(t.plan_line FROM 'observed=([^ ]+)') AS threshold_cache_observed,
       substring(t.plan_line FROM 'saturated=([^ ]+)') AS threshold_cache_saturated,
       substring(t.plan_line FROM 'relaxed=([^ ]+)') AS threshold_cache_relaxed,
       substring(pay.plan_line FROM 'source=([^ ]+)') AS payload_input,
       substring(pay.plan_line FROM 'headroom=([^ ]+)') AS payload_input_headroom,
       substring(pay.plan_line FROM 'scan-runs=([^ ]+)') AS payload_input_scan_runs,
       substring(pay.plan_line FROM 'replay-runs=([^ ]+)') AS payload_input_replay_runs,
       substring(pay.plan_line FROM 'seed-runs=([^ ]+)') AS payload_input_seed_runs,
       substring(pay.plan_line FROM 'replay-percent=([^ ]+)') AS payload_input_replay_percent,
       substring(pay.plan_line FROM 'seed-percent=([^ ]+)') AS payload_input_seed_percent,
       substring(pay.plan_line FROM 'observed=([^ ]+)') AS payload_input_observed,
       substring(pay.plan_line FROM 'value-posting=([^/]+)') AS payload_input_value_posting_source,
       substring(pay.plan_line FROM 'value-posting=[^/]+/observed:([^ ]+)') AS payload_input_value_posting_observed,
       substring(pay.plan_line FROM 'reason=([^ ]+)') AS payload_input_reason,
       substring(pay.plan_line FROM ' class=([^ ]+)') AS payload_input_class,
       CASE
           WHEN substring(pay.plan_line FROM ' class=([^ ]+)') =
                'adjacency-composite-value-posting'
            AND COALESCE(NULLIF(substring(pay.plan_line FROM
                         'value-posting=[^/]+/observed:([^ ]+)'), '')::numeric,
                         0) > 0
           THEN 'identity-cache-hit'
           WHEN substring(pol.plan_line FROM 'value-posting=([^ ]+)') IS NOT NULL
            AND substring(pol.plan_line FROM 'value-posting=([^ ]+)') <> 'out:none/in:none'
           THEN 'source-available'
           ELSE 'none'
       END AS value_posting_decision,
       CASE
           WHEN substring(pol.plan_line FROM 'reason=[^ ]*composite-value-posting') IS NOT NULL
           THEN 'policy-value-posting'
           WHEN substring(pol.plan_line FROM 'value-posting=([^ ]+)') IS NOT NULL
            AND substring(pol.plan_line FROM 'value-posting=([^ ]+)') <> 'out:none/in:none'
           THEN 'source-available'
           ELSE 'none'
       END AS value_posting_policy_decision,
       CASE
           WHEN substring(pay.plan_line FROM ' class=([^ ]+)') =
                'adjacency-composite-value-posting'
           THEN CASE COALESCE(NULLIF(substring(p.plan_line FROM
                            'weight=([^ ]+)'), '')::numeric, 0)
                    WHEN 3 THEN 18
                    WHEN 2 THEN 20
                    ELSE 25
                END
           ELSE NULL::integer
       END AS value_posting_headroom_expected,
       CASE
           WHEN substring(pay.plan_line FROM ' class=([^ ]+)') =
                'adjacency-composite-value-posting'
           THEN COALESCE(NULLIF(substring(pay.plan_line FROM
                            'headroom=([^ ]+)'), '')::numeric, 0) =
                CASE COALESCE(NULLIF(substring(p.plan_line FROM
                                'weight=([^ ]+)'), '')::numeric, 0)
                    WHEN 3 THEN 18
                    WHEN 2 THEN 20
                    ELSE 25
                END
           ELSE false
       END AS value_posting_headroom_applied,
       substring(pol.plan_line FROM ' class=([^ ]+)') AS policy_class,
       substring(pol.plan_line FROM ' recommendation=([^ ]+)') AS recommendation
FROM planner_profile p
LEFT JOIN planner_threshold t USING (shape)
LEFT JOIN planner_payload pay USING (shape)
LEFT JOIN planner_policy pol USING (shape)
ORDER BY p.shape;

WITH planner_profile AS (
    SELECT DISTINCT ON (shape) shape, plan_line
    FROM vle_benchmark_explain
    WHERE plan_line LIKE '%VLE Source Profile:%'
    ORDER BY shape, line_no
),
planner_threshold AS (
    SELECT DISTINCT ON (shape) shape, plan_line
    FROM vle_benchmark_explain
    WHERE plan_line LIKE '%VLE Source Threshold Input:%'
    ORDER BY shape, line_no
),
planner_payload AS (
    SELECT DISTINCT ON (shape) shape, plan_line
    FROM vle_benchmark_explain
    WHERE plan_line LIKE '%VLE Source Payload Input:%'
    ORDER BY shape, line_no
),
planner_policy AS (
    SELECT DISTINCT ON (shape) shape, plan_line
    FROM vle_benchmark_explain
    WHERE plan_line LIKE '%VLE Source Policy:%'
    ORDER BY shape, line_no
),
planner AS (
    SELECT p.shape,
           substring(p.plan_line FROM 'consumer=([^ ]+)') AS consumer,
           substring(p.plan_line FROM ' class=([^ ]+)') AS consumer_class,
           substring(p.plan_line FROM 'active=([^ ]+)') AS active_direction,
           substring(p.plan_line FROM 'budget=([^ ]+)') AS fanout_budget,
           substring(p.plan_line FROM 'weight=([^ ]+)') AS materialization_weight,
           substring(pol.plan_line FROM 'VLE Source Policy: ([^ ]+)') AS planner_policy,
           substring(pol.plan_line FROM 'VLE Source Policy: out=([^/ ]+)') AS planner_out_source,
           substring(pol.plan_line FROM 'VLE Source Policy: [^ ]+/in=([^ ]+)') AS planner_in_source,
           substring(pol.plan_line FROM ' reason=([^ ]+)') AS policy_reason,
           substring(pol.plan_line FROM 'composite-work=([^ ]+)') AS composite_work,
           substring(pol.plan_line FROM 'composite-work=[^(]+\(out:([^,]+)') AS composite_work_out,
           substring(pol.plan_line FROM 'composite-work=[^(]+\(out:[^,]+,in:([^\)]+)') AS composite_work_in,
           substring(pol.plan_line FROM 'value-posting=([^ ]+)') AS value_posting_source,
           substring(p.plan_line FROM 'cache-seed=([^ ]+)') AS cache_seed,
           substring(p.plan_line FROM 'endpoint-headroom=([^ ]+)') AS endpoint_headroom,
           substring(p.plan_line FROM 'empty-lifecycle=([^/]+)') AS empty_lifecycle,
           substring(p.plan_line FROM 'empty-lifecycle=[^/]+/depth:([^ ]+)') AS empty_lifecycle_depth,
           substring(p.plan_line FROM 'empty-batch=([^/]+)') AS empty_batch,
           substring(p.plan_line FROM 'empty-batch=[^/]+/size:([^ ]+)') AS empty_batch_size,
           substring(t.plan_line FROM 'source=([^ ]+)') AS threshold_input,
           substring(t.plan_line FROM 'headroom=([^ ]+)') AS threshold_input_headroom,
           substring(t.plan_line FROM 'batch=([^ ]+)') AS threshold_input_batch,
           substring(t.plan_line FROM 'direction=([^ ]+)') AS threshold_input_source,
           COALESCE(substring(p.plan_line FROM 'active=([^ ]+)') = 'both' AND
               substring(t.plan_line FROM 'direction=([^ ]+)') IN ('out', 'in', 'mixed'),
               false) AS threshold_directional_family,
           substring(t.plan_line FROM 'reason=([^ ]+)') AS threshold_input_reason,
           substring(t.plan_line FROM 'observed=([^ ]+)') AS threshold_cache_observed,
           substring(t.plan_line FROM 'saturated=([^ ]+)') AS threshold_cache_saturated,
           substring(t.plan_line FROM 'relaxed=([^ ]+)') AS threshold_cache_relaxed,
           substring(pay.plan_line FROM 'source=([^ ]+)') AS payload_input,
           substring(pay.plan_line FROM 'headroom=([^ ]+)') AS payload_input_headroom,
           substring(pay.plan_line FROM 'scan-runs=([^ ]+)') AS payload_input_scan_runs,
           substring(pay.plan_line FROM 'replay-runs=([^ ]+)') AS payload_input_replay_runs,
           substring(pay.plan_line FROM 'seed-runs=([^ ]+)') AS payload_input_seed_runs,
           substring(pay.plan_line FROM 'replay-percent=([^ ]+)') AS payload_input_replay_percent,
           substring(pay.plan_line FROM 'seed-percent=([^ ]+)') AS payload_input_seed_percent,
           substring(pay.plan_line FROM 'observed=([^ ]+)') AS payload_input_observed,
           substring(pay.plan_line FROM 'value-posting=([^/]+)') AS payload_input_value_posting_source,
           substring(pay.plan_line FROM 'value-posting=[^/]+/observed:([^ ]+)') AS payload_input_value_posting_observed,
           substring(pay.plan_line FROM 'reason=([^ ]+)') AS payload_input_reason,
           substring(pay.plan_line FROM ' class=([^ ]+)') AS payload_input_class,
           CASE
               WHEN substring(pay.plan_line FROM ' class=([^ ]+)') =
                    'adjacency-composite-value-posting'
                AND COALESCE(NULLIF(substring(pay.plan_line FROM
                             'value-posting=[^/]+/observed:([^ ]+)'), '')::numeric,
                             0) > 0
               THEN 'identity-cache-hit'
               WHEN substring(pol.plan_line FROM 'value-posting=([^ ]+)') IS NOT NULL
                AND substring(pol.plan_line FROM 'value-posting=([^ ]+)') <> 'out:none/in:none'
               THEN 'source-available'
               ELSE 'none'
           END AS value_posting_decision,
           CASE
               WHEN substring(pol.plan_line FROM 'reason=[^ ]*composite-value-posting') IS NOT NULL
               THEN 'policy-value-posting'
               WHEN substring(pol.plan_line FROM 'value-posting=([^ ]+)') IS NOT NULL
                AND substring(pol.plan_line FROM 'value-posting=([^ ]+)') <> 'out:none/in:none'
               THEN 'source-available'
               ELSE 'none'
           END AS value_posting_policy_decision,
           CASE
               WHEN substring(pay.plan_line FROM ' class=([^ ]+)') =
                    'adjacency-composite-value-posting'
               THEN CASE COALESCE(NULLIF(substring(p.plan_line FROM
                                'weight=([^ ]+)'), '')::numeric, 0)
                        WHEN 3 THEN 18
                        WHEN 2 THEN 20
                        ELSE 25
                    END
               ELSE NULL::integer
           END AS value_posting_headroom_expected,
           CASE
               WHEN substring(pay.plan_line FROM ' class=([^ ]+)') =
                    'adjacency-composite-value-posting'
               THEN COALESCE(NULLIF(substring(pay.plan_line FROM
                                'headroom=([^ ]+)'), '')::numeric, 0) =
                    CASE COALESCE(NULLIF(substring(p.plan_line FROM
                                    'weight=([^ ]+)'), '')::numeric, 0)
                        WHEN 3 THEN 18
                        WHEN 2 THEN 20
                        ELSE 25
                    END
               ELSE false
           END AS value_posting_headroom_applied,
           substring(pol.plan_line FROM ' class=([^ ]+)') AS planner_class,
           substring(pol.plan_line FROM ' recommendation=([^ ]+)') AS planner_recommendation
    FROM planner_profile p
    LEFT JOIN planner_threshold t USING (shape)
    LEFT JOIN planner_payload pay USING (shape)
    LEFT JOIN planner_policy pol USING (shape)
),
runtime_summary AS (
    SELECT DISTINCT ON (shape)
           shape,
           substring(plan_line FROM 'dominant=([^ ]+)') AS dominant_source,
           substring(plan_line FROM ' class=([^ ]+)') AS runtime_class,
           substring(plan_line FROM 'pressure=([^ ]+)') AS runtime_pressure,
           substring(plan_line FROM 'action=([^ ]+)') AS runtime_action
    FROM vle_benchmark_explain
    WHERE plan_line LIKE '%VLE Source Runtime%'
    ORDER BY shape, line_no
),
runtime_plan AS (
    SELECT DISTINCT ON (shape)
           shape,
           substring(plan_line FROM 'planned=([^ ]+)') AS planned_source,
           substring(plan_line FROM 'planned=out:([^/ ]+)') AS planned_out_source,
           substring(plan_line FROM 'planned=[^ ]+/in:([^ ]+)') AS planned_in_source,
           substring(plan_line FROM 'source-match=([^ ]+)') AS runtime_source_match,
           substring(plan_line FROM 'planned-class=([^ ]+)') AS planned_class,
           substring(plan_line FROM 'class-match=([^ ]+)') AS runtime_class_match
    FROM vle_benchmark_explain
    WHERE plan_line LIKE '%VLE Source Plan%'
    ORDER BY shape, line_no
),
runtime_counters AS (
    SELECT DISTINCT ON (shape)
           shape,
           substring(plan_line FROM 'density=age_adjacency:([^/]+)') AS age_adjacency_density,
           substring(plan_line FROM 'endpoint-btree:([^/]+)') AS endpoint_btree_density,
           substring(plan_line FROM 'packed:([^ ]+)') AS packed_density
    FROM vle_benchmark_explain
    WHERE plan_line LIKE '%VLE Source Counters%'
    ORDER BY shape, line_no
),
runtime_payload AS (
    SELECT DISTINCT ON (shape)
           shape,
           substring(plan_line FROM 'runs=scan:([^/]+)') AS payload_scan_runs,
           substring(plan_line FROM 'runs=scan:[^/]+/replay:([^/]+)') AS payload_replay_runs,
           substring(plan_line FROM 'runs=scan:[^/]+/replay:[^/]+/seed:([^ ]+)') AS payload_seed_runs,
           COALESCE(substring(plan_line FROM 'value-posting:([0-9]+)'),
                    substring(plan_line FROM 'value-summary:([0-9]+)'),
                    substring(plan_line FROM 'wide-bloom:([0-9]+)'))
               AS payload_value_posting_filtered,
           substring(plan_line FROM 'tuples=scan:([^/]+)') AS payload_scan_tuples,
           substring(plan_line FROM 'tuples=scan:[^/]+/replay:([^/]+)') AS payload_replay_tuples,
           substring(plan_line FROM 'tuples=scan:[^/]+/replay:[^/]+/seeds:([^ ]+)') AS payload_seed_events
    FROM vle_benchmark_explain
    WHERE plan_line LIKE '%VLE Payload Runtime%'
    ORDER BY shape, line_no
),
runtime_empty AS (
    SELECT DISTINCT ON (shape)
           shape,
           substring(plan_line FROM 'scans=age_adjacency:([^/]+)') AS age_adjacency_empty_scans,
           substring(plan_line FROM 'endpoint-btree:([^ ]+)') AS endpoint_btree_empty_scans,
           substring(plan_line FROM 'evidence=([^ ]+)') AS empty_evidence,
           substring(plan_line FROM 'suppressed=([^ ]+)') AS suppressed_source,
           substring(plan_line FROM 'match=([^ ]+)') AS suppression_match,
           substring(plan_line FROM 'cache=([^/]+)') AS age_adjacency_empty_source_cache_hits,
           substring(plan_line FROM 'cache=[^/]+/([^/]+)') AS age_adjacency_empty_source_cache_hit_out,
           substring(plan_line FROM 'cache=[^/]+/[^/]+/([^ ]+)') AS age_adjacency_empty_source_cache_hit_in,
           substring(plan_line FROM 'frontier=([^/]+)') AS age_adjacency_empty_source_frontier_marks,
           substring(plan_line FROM 'frontier=[^/]+/([^/]+)') AS age_adjacency_empty_source_frontier_mark_out,
           substring(plan_line FROM 'frontier=[^/]+/[^/]+/([^ ]+)') AS age_adjacency_empty_source_frontier_mark_in,
           substring(plan_line FROM 'run=([^/]+)') AS age_adjacency_empty_source_run_skips,
           substring(plan_line FROM 'run=[^/]+/([^/]+)') AS age_adjacency_empty_source_run_skip_out,
           substring(plan_line FROM 'run=[^/]+/[^/]+/([^ ]+)') AS age_adjacency_empty_source_run_skip_in
    FROM vle_benchmark_explain
    WHERE plan_line LIKE '%VLE Empty Evidence%'
    ORDER BY shape, line_no
),
runtime_lifecycle AS (
    SELECT DISTINCT ON (shape)
           shape,
           substring(plan_line FROM 'plan=([^/]+)') AS empty_plan,
           substring(plan_line FROM 'plan=[^/]+/depth:([^ ]+)') AS empty_plan_depth,
           substring(plan_line FROM 'plan=[^ ]+ match=([^ ]+)') AS runtime_empty_plan_match,
           substring(plan_line FROM 'context=([^/]+)') AS empty_context,
           substring(plan_line FROM 'context=[^/]+/depth:([^/]+)') AS empty_context_depth,
           substring(plan_line FROM 'context=[^ ]+ match=([^ ]+)') AS runtime_empty_context_match,
           substring(plan_line FROM 'batch=([^/]+)') AS empty_batch,
           substring(plan_line FROM 'batch=[^/]+/size:([^/]+)') AS empty_batch_size,
           substring(plan_line FROM 'batch=[^/]+/size:[^/]+/capacity:([^ ]+)') AS empty_batch_capacity,
           substring(plan_line FROM 'batch=[^ ]+ match=([^ ]+)') AS runtime_empty_batch_match,
           substring(plan_line FROM 'frontier-batch=flushes:([^/]+)') AS empty_frontier_batch_flushes,
           substring(plan_line FROM 'frontier-batch=[^/]+/keys:([^/]+)') AS empty_frontier_batch_keys,
           substring(plan_line FROM 'frontier-batch=[^/]+/keys:[^/]+/max:([^ ]+)') AS empty_frontier_batch_max
    FROM vle_benchmark_explain
    WHERE plan_line LIKE '%VLE Empty Lifecycle%'
    ORDER BY shape, line_no
),
runtime_feedback AS (
    SELECT DISTINCT ON (shape)
           shape,
           substring(plan_line FROM 'recommendation=([^ ]+)') AS runtime_recommendation,
           substring(plan_line FROM 'planned=([^ ]+)') AS planned_recommendation,
           substring(plan_line FROM 'root-empty=completion:([^/]+)') AS root_empty_completion_count,
           substring(plan_line FROM 'root-empty=[^/]+/out:([^/]+)') AS root_empty_completion_out,
           substring(plan_line FROM 'root-empty=[^/]+/out:[^/]+/in:([^/]+)') AS root_empty_completion_in,
           substring(plan_line FROM 'root-empty=[^/]+/out:[^/]+/in:[^/]+/batch:([^/]+)') AS root_empty_batch_capacity,
           substring(plan_line FROM 'root-empty=[^/]+/out:[^/]+/in:[^/]+/batch:[^/]+/saturated-roots:([^ ]+)') AS root_empty_saturated_count,
           substring(plan_line FROM 'threshold=([^/]+)') AS threshold_feedback,
           substring(plan_line FROM 'threshold=[^/]+/headroom:([^/]+)') AS threshold_feedback_headroom,
           substring(plan_line FROM 'threshold=[^/]+/headroom:[^/]+/batch:([^/]+)') AS threshold_feedback_batch,
           substring(plan_line FROM 'threshold=[^/]+/headroom:[^/]+/batch:[^/]+/source:([^/]+)') AS threshold_feedback_source,
           substring(plan_line FROM 'threshold=[^/]+/headroom:[^/]+/batch:[^/]+/source:[^/]+/reason:([^ ]+)') AS threshold_feedback_reason
    FROM vle_benchmark_explain
    WHERE plan_line LIKE '%VLE Runtime Feedback%'
    ORDER BY shape, line_no
),
runtime AS (
    SELECT s.shape,
           s.dominant_source,
           p.planned_source,
           p.planned_out_source,
           p.planned_in_source,
           p.runtime_source_match,
           s.runtime_class,
           p.planned_class,
           p.runtime_class_match,
           f.runtime_recommendation,
           f.planned_recommendation,
           c.age_adjacency_density,
           c.endpoint_btree_density,
           c.packed_density,
           e.age_adjacency_empty_scans,
           e.endpoint_btree_empty_scans,
           NULL::text AS age_adjacency_empty_source_skips,
           NULL::text AS age_adjacency_empty_source_skip_out,
           NULL::text AS age_adjacency_empty_source_skip_in,
           e.age_adjacency_empty_source_cache_hits,
           e.age_adjacency_empty_source_cache_hit_out,
           e.age_adjacency_empty_source_cache_hit_in,
           e.age_adjacency_empty_source_frontier_marks,
           e.age_adjacency_empty_source_frontier_mark_out,
           e.age_adjacency_empty_source_frontier_mark_in,
           l.empty_frontier_batch_flushes,
           NULL::text AS empty_frontier_batch_out,
           NULL::text AS empty_frontier_batch_in,
           l.empty_frontier_batch_keys,
           l.empty_frontier_batch_max,
           e.age_adjacency_empty_source_run_skips,
           e.age_adjacency_empty_source_run_skip_out,
           e.age_adjacency_empty_source_run_skip_in,
           py.payload_scan_runs,
           py.payload_replay_runs,
           py.payload_seed_runs,
           py.payload_value_posting_filtered,
           py.payload_scan_tuples,
           py.payload_replay_tuples,
           py.payload_seed_events,
           l.empty_plan,
           l.empty_plan_depth,
           l.runtime_empty_plan_match,
           l.empty_context,
           l.empty_context_depth,
           l.runtime_empty_context_match,
           l.empty_batch,
           l.empty_batch_size,
           l.empty_batch_capacity,
           l.runtime_empty_batch_match,
           f.root_empty_completion_count AS empty_completion_count,
           f.root_empty_batch_capacity AS empty_summary_batch,
           f.root_empty_saturated_count AS empty_batch_saturated,
           f.root_empty_completion_count,
           f.root_empty_completion_out,
           f.root_empty_completion_in,
           f.root_empty_batch_capacity,
           f.root_empty_saturated_count,
           f.threshold_feedback,
           f.threshold_feedback_headroom,
           f.threshold_feedback_batch,
           f.threshold_feedback_source,
           f.threshold_feedback_reason,
           e.empty_evidence,
           e.suppressed_source,
           e.suppression_match,
           s.runtime_pressure,
           s.runtime_action
    FROM runtime_summary s
    LEFT JOIN runtime_plan p USING (shape)
    LEFT JOIN runtime_counters c USING (shape)
    LEFT JOIN runtime_payload py USING (shape)
    LEFT JOIN runtime_empty e USING (shape)
    LEFT JOIN runtime_lifecycle l USING (shape)
    LEFT JOIN runtime_feedback f USING (shape)
)
SELECT p.shape,
       b.rows_returned,
       b.elapsed_ms,
       c.label_fanout_edges,
       c.value_posting_edges,
       c.value_posting_root_count,
       c.value_posting_edges_per_root,
       c.replay_branches_input,
       c.replay_leaves_input,
       c.replay_branches,
       c.replay_leaves,
       p.consumer,
       p.consumer_class,
       p.active_direction,
       p.fanout_budget,
       p.materialization_weight,
       p.cache_seed,
       p.endpoint_headroom,
       p.empty_lifecycle,
       p.empty_lifecycle_depth,
       p.empty_batch AS planned_empty_batch,
       p.empty_batch_size AS planned_empty_batch_size,
       p.threshold_input,
       p.threshold_input_headroom,
       p.threshold_input_batch,
       p.threshold_input_source,
       p.threshold_directional_family,
       CASE WHEN p.threshold_directional_family
            THEN r.age_adjacency_density::numeric
            ELSE NULL::numeric
       END AS directional_family_productive_density,
       CASE WHEN p.threshold_directional_family
            THEN round(r.root_empty_completion_count::numeric /
                       NULLIF(r.root_empty_completion_count::numeric +
                              b.rows_returned::numeric, 0), 4)
            ELSE NULL::numeric
       END AS directional_family_empty_completion_ratio,
       CASE WHEN p.threshold_directional_family
            THEN round(r.root_empty_completion_out::numeric /
                       NULLIF(r.root_empty_completion_count::numeric, 0), 4)
            ELSE NULL::numeric
       END AS directional_family_empty_out_ratio,
       CASE WHEN p.threshold_directional_family
            THEN round(r.root_empty_completion_in::numeric /
                       NULLIF(r.root_empty_completion_count::numeric, 0), 4)
            ELSE NULL::numeric
       END AS directional_family_empty_in_ratio,
       p.threshold_input_reason,
       p.threshold_cache_observed,
       p.threshold_cache_saturated,
       p.threshold_cache_relaxed,
       p.payload_input,
       p.payload_input_headroom,
       p.payload_input_scan_runs,
       p.payload_input_replay_runs,
       p.payload_input_seed_runs,
       p.payload_input_replay_percent,
       p.payload_input_seed_percent,
       p.payload_input_observed,
       p.payload_input_value_posting_source,
       p.payload_input_value_posting_observed,
       p.payload_input_reason,
       p.payload_input_class,
       p.value_posting_decision,
       p.value_posting_policy_decision,
       p.value_posting_headroom_expected,
       p.value_posting_headroom_applied,
       p.composite_work,
       p.composite_work_out,
       p.composite_work_in,
       p.value_posting_source,
       p.policy_reason,
       p.planner_policy,
       CASE p.active_direction
           WHEN 'out' THEN p.planner_out_source
           WHEN 'in' THEN p.planner_in_source
           WHEN 'both' THEN p.planner_policy
           ELSE p.planner_policy
       END AS active_planner_source,
       r.dominant_source,
       r.planned_source,
       CASE p.active_direction
           WHEN 'out' THEN r.planned_out_source
           WHEN 'in' THEN r.planned_in_source
           WHEN 'both' THEN r.planned_source
           ELSE r.planned_source
       END AS active_planned_source,
       p.planner_class,
       r.runtime_class,
       r.planned_class,
       p.planner_recommendation,
       r.runtime_recommendation,
       r.planned_recommendation,
       CASE p.active_direction
           WHEN 'out' THEN p.planner_out_source = r.dominant_source
           WHEN 'in' THEN p.planner_in_source = r.dominant_source
           WHEN 'both' THEN p.planner_policy LIKE '%' || r.dominant_source || '%'
           ELSE p.planner_policy LIKE '%' || r.dominant_source || '%'
       END AS source_match,
       r.runtime_source_match,
       p.planner_class = r.runtime_class AS class_match,
       r.runtime_class_match,
       CASE
           WHEN COALESCE(r.runtime_source_match, 'false') <> 'true'
           THEN 'source-mismatch'
           WHEN COALESCE(r.runtime_class_match, 'false') <> 'true'
           THEN 'class-mismatch'
           WHEN p.value_posting_policy_decision = 'policy-value-posting' AND
                p.value_posting_headroom_applied
           THEN 'value-posting-headroom-applied'
           WHEN COALESCE(NULLIF(r.payload_value_posting_filtered, '')::numeric,
                         0) > 0
           THEN 'promote-value-posting-headroom'
           WHEN p.threshold_directional_family AND
                p.policy_reason LIKE '%directional-family-productive%'
           THEN 'directional-family-split-applied'
           WHEN p.threshold_directional_family AND
                COALESCE(r.root_empty_completion_count::numeric /
                         NULLIF(r.root_empty_completion_count::numeric +
                                b.rows_returned::numeric, 0), 0) >= 0.50
           THEN 'review-directional-family'
           WHEN r.runtime_pressure IN ('adjacency-empty-frontier',
                                       'adjacency-empty-run')
           THEN 'keep-empty-lifecycle'
           WHEN r.runtime_pressure = 'adjacency-payload-replay'
           THEN 'keep-payload-replay'
           ELSE 'keep-policy'
       END AS source_policy_outcome,
       CASE
           WHEN COALESCE(r.runtime_source_match, 'false') <> 'true'
           THEN 'inspect-source-handoff'
           WHEN COALESCE(r.runtime_class_match, 'false') <> 'true'
           THEN 'tune-source-policy'
           WHEN p.value_posting_policy_decision = 'policy-value-posting' AND
                p.value_posting_headroom_applied
           THEN 'keep-value-posting-headroom'
           WHEN COALESCE(NULLIF(r.payload_value_posting_filtered, '')::numeric,
                         0) > 0
           THEN 'promote-value-posting-headroom'
           WHEN p.threshold_directional_family AND
                p.policy_reason LIKE '%directional-family-productive%'
           THEN 'keep-directional-split'
           WHEN p.threshold_directional_family AND
                COALESCE(r.root_empty_completion_count::numeric /
                         NULLIF(r.root_empty_completion_count::numeric +
                                b.rows_returned::numeric, 0), 0) >= 0.50
           THEN 'measure-directional-split'
           WHEN r.runtime_pressure IN ('adjacency-empty-frontier',
                                       'adjacency-empty-run')
           THEN 'keep-empty-batch'
           WHEN r.runtime_pressure = 'adjacency-payload-replay'
           THEN 'keep-replay-headroom'
           ELSE COALESCE(r.runtime_recommendation, p.planner_recommendation)
       END AS source_policy_next_action,
       r.age_adjacency_density,
       r.endpoint_btree_density,
       r.packed_density,
       r.age_adjacency_empty_scans,
       r.endpoint_btree_empty_scans,
       r.age_adjacency_empty_source_skips,
       r.age_adjacency_empty_source_skip_out,
       r.age_adjacency_empty_source_skip_in,
       r.age_adjacency_empty_source_cache_hits,
       r.age_adjacency_empty_source_cache_hit_out,
       r.age_adjacency_empty_source_cache_hit_in,
       r.age_adjacency_empty_source_frontier_marks,
       r.age_adjacency_empty_source_frontier_mark_out,
       r.age_adjacency_empty_source_frontier_mark_in,
       r.empty_frontier_batch_flushes,
       r.empty_frontier_batch_out,
       r.empty_frontier_batch_in,
       r.empty_frontier_batch_keys,
       r.empty_frontier_batch_max,
       r.age_adjacency_empty_source_run_skips,
       r.age_adjacency_empty_source_run_skip_out,
       r.age_adjacency_empty_source_run_skip_in,
       r.payload_scan_runs,
       r.payload_replay_runs,
       r.payload_seed_runs,
       r.payload_value_posting_filtered,
       COALESCE(NULLIF(p.payload_input_value_posting_observed, '')::numeric, 0) > 0
           AS value_posting_identity_cache_hit,
       COALESCE(NULLIF(r.payload_value_posting_filtered, '')::numeric, 0) > 0
           AS value_posting_runtime_hit,
       CASE
           WHEN COALESCE(NULLIF(p.payload_input_value_posting_observed, '')::numeric, 0) > 0
           THEN 'identity-cache-hit'
           WHEN COALESCE(NULLIF(r.payload_value_posting_filtered, '')::numeric, 0) > 0
           THEN 'runtime-hit'
           WHEN p.value_posting_source IS NOT NULL
            AND p.value_posting_source <> 'out:none/in:none'
           THEN 'source-available'
           ELSE 'none'
       END AS value_posting_runtime_decision,
       r.payload_scan_tuples,
       r.payload_replay_tuples,
       r.payload_seed_events,
       r.empty_plan,
       r.empty_plan_depth,
       p.empty_lifecycle = r.empty_plan AS empty_plan_match,
       p.empty_lifecycle_depth = r.empty_plan_depth AS empty_plan_depth_match,
       r.runtime_empty_plan_match,
       r.empty_context,
       r.empty_context_depth,
       p.empty_lifecycle = r.empty_context AS empty_context_match,
       p.empty_lifecycle_depth = r.empty_context_depth AS empty_context_depth_match,
       r.runtime_empty_context_match,
       r.empty_batch,
       r.empty_batch_size,
       r.empty_batch_capacity,
       p.empty_batch = r.empty_batch AS empty_batch_match,
       p.empty_batch_size = r.empty_batch_size AS empty_batch_size_match,
       p.empty_batch_size = r.empty_batch_capacity AS empty_batch_capacity_match,
       r.runtime_empty_batch_match,
       r.empty_completion_count,
       r.empty_summary_batch,
       r.empty_batch_saturated,
       r.root_empty_completion_count,
       r.root_empty_completion_out,
       r.root_empty_completion_in,
       r.root_empty_batch_capacity,
       r.root_empty_saturated_count,
       r.threshold_feedback,
       r.threshold_feedback_headroom,
       r.threshold_feedback_batch,
       r.threshold_feedback_source,
       r.threshold_feedback_reason,
       r.empty_evidence,
       r.suppressed_source,
       r.suppression_match,
       r.runtime_pressure,
       r.runtime_action
FROM planner p
LEFT JOIN runtime r USING (shape)
LEFT JOIN vle_benchmark_results b USING (shape)
CROSS JOIN vle_benchmark_config c
ORDER BY p.shape;

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
