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
           :replay_branches::int AS replay_branches_input,
           :replay_leaves::int AS replay_leaves_input
)
SELECT graph_name,
       sparse_nodes,
       dense_nodes,
       label_fanout_labels,
       label_fanout_edges,
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
    replay_branches int;
    replay_leaves int;
    i int;
    j int;
BEGIN
    SELECT c.graph_name, c.sparse_nodes, c.dense_nodes,
           c.label_fanout_edges, c.replay_branches, c.replay_leaves
    INTO graph_name, sparse_nodes, dense_nodes,
         label_fanout_edges, replay_branches, replay_leaves
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
    EXECUTE format('ANALYZE %I.%I', graph_name, 'FanoutStart');
    EXECUTE format('ANALYZE %I.%I', graph_name, 'FanoutEnd');
    EXECUTE format('ANALYZE %I.%I', graph_name, 'FanoutEdge');
    EXECUTE format('ANALYZE %I.%I', graph_name, 'ReplayStart');
    EXECUTE format('ANALYZE %I.%I', graph_name, 'ReplayHub');
    EXECUTE format('ANALYZE %I.%I', graph_name, 'ReplayEdge');
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

TABLE vle_benchmark_results ORDER BY shape;

SELECT shape, line_no, plan_line
FROM vle_benchmark_explain
WHERE plan_line LIKE '%VLE%'
   OR plan_line LIKE '%source%'
ORDER BY shape, line_no;

SELECT shape,
       substring(plan_line FROM 'feedback=dominant=([^ ]+)') AS dominant_source,
       substring(plan_line FROM 'planned=([^ ]+)') AS planned_source,
       substring(plan_line FROM 'source-match=([^ ]+)') AS runtime_source_match,
       substring(plan_line FROM ' class=([^ ]+)') AS source_class,
       substring(plan_line FROM 'planned-class=([^ ]+)') AS planned_class,
       substring(plan_line FROM 'class-match=([^ ]+)') AS runtime_class_match,
       substring(plan_line FROM ' recommendation=([^ ]+)') AS recommendation,
       substring(plan_line FROM 'planned-recommendation=([^ ]+)') AS planned_recommendation,
       substring(plan_line FROM 'empty=age_adjacency:([^/]+)') AS age_adjacency_empty_scans,
       substring(plan_line FROM 'endpoint-btree:([^,]+), empty-suppressed') AS endpoint_btree_empty_scans,
       substring(plan_line FROM 'empty-suppressed=age_adjacency:([^/]+)') AS age_adjacency_empty_source_skips,
       substring(plan_line FROM '/out:([^/]+)') AS age_adjacency_empty_source_skip_out,
       substring(plan_line FROM '/in:([^,]+), empty-cache') AS age_adjacency_empty_source_skip_in,
       substring(plan_line FROM 'empty-cache=age_adjacency:([^/]+)') AS age_adjacency_empty_source_cache_hits,
       substring(plan_line FROM 'empty-cache=age_adjacency:[^/]+/out:([^/]+)') AS age_adjacency_empty_source_cache_hit_out,
       substring(plan_line FROM 'empty-cache=age_adjacency:[^/]+/out:[^/]+/in:([^,]+)') AS age_adjacency_empty_source_cache_hit_in,
       substring(plan_line FROM 'empty-frontier=age_adjacency:([^/]+)') AS age_adjacency_empty_source_frontier_marks,
       substring(plan_line FROM 'empty-frontier=age_adjacency:[^/]+/out:([^/]+)') AS age_adjacency_empty_source_frontier_mark_out,
       substring(plan_line FROM 'empty-frontier=age_adjacency:[^/]+/out:[^/]+/in:([^,]+)') AS age_adjacency_empty_source_frontier_mark_in,
       substring(plan_line FROM 'empty-frontier-batch=flushes:([^/]+)') AS empty_frontier_batch_flushes,
       substring(plan_line FROM 'empty-frontier-batch=[^/]+/out:([^/]+)') AS empty_frontier_batch_out,
       substring(plan_line FROM 'empty-frontier-batch=[^/]+/out:[^/]+/in:([^/]+)') AS empty_frontier_batch_in,
       substring(plan_line FROM 'empty-frontier-batch=[^/]+/out:[^/]+/in:[^/]+/keys:([^/]+)') AS empty_frontier_batch_keys,
       substring(plan_line FROM 'empty-frontier-batch=[^/]+/out:[^/]+/in:[^/]+/keys:[^/]+/max:([^,]+)') AS empty_frontier_batch_max,
       substring(plan_line FROM 'empty-run=age_adjacency:([^/]+)') AS age_adjacency_empty_source_run_skips,
       substring(plan_line FROM 'empty-run=age_adjacency:[^/]+/out:([^/]+)') AS age_adjacency_empty_source_run_skip_out,
       substring(plan_line FROM 'empty-run=age_adjacency:[^/]+/out:[^/]+/in:([^,]+)') AS age_adjacency_empty_source_run_skip_in,
       substring(plan_line FROM 'payload-cache=runs:scan:([^/]+)') AS payload_scan_runs,
       substring(plan_line FROM 'payload-cache=[^/]+/replay:([^/]+)') AS payload_replay_runs,
       substring(plan_line FROM 'payload-cache=[^/]+/replay:[^/]+/seed:([^/]+)') AS payload_seed_runs,
       substring(plan_line FROM 'payload-cache=[^/]+/replay:[^/]+/seed:[^/]+/tuples:scan:([^/]+)') AS payload_scan_tuples,
       substring(plan_line FROM 'payload-cache=[^/]+/replay:[^/]+/seed:[^/]+/tuples:scan:[^/]+/replay:([^/]+)') AS payload_replay_tuples,
       substring(plan_line FROM 'payload-cache=[^/]+/replay:[^/]+/seed:[^/]+/tuples:scan:[^/]+/replay:[^/]+/seeds:([^,]+)') AS payload_seed_events,
       substring(plan_line FROM 'empty-plan=([^/]+)') AS empty_plan,
       substring(plan_line FROM 'empty-plan=[^/]+/depth:([^ ]+)') AS empty_plan_depth,
       substring(plan_line FROM 'empty-plan=[^ ]+ match=([^,]+)') AS runtime_empty_plan_match,
       substring(plan_line FROM 'empty-context=([^/]+)') AS empty_context,
       substring(plan_line FROM 'empty-context=[^/]+/depth:([^/]+)') AS empty_context_depth,
       substring(plan_line FROM 'empty-context=[^ ]+ match=([^,]+)') AS runtime_empty_context_match,
       substring(plan_line FROM 'empty-batch=([^/]+)') AS empty_batch,
       substring(plan_line FROM 'empty-batch=[^/]+/size:([^/]+)') AS empty_batch_size,
       substring(plan_line FROM 'empty-batch=[^/]+/size:[^/]+/capacity:([^ ]+)') AS empty_batch_capacity,
       substring(plan_line FROM 'empty-batch=[^ ]+ match=([^,]+)') AS runtime_empty_batch_match,
       substring(plan_line FROM 'empty-summary=completion:([^/]+)') AS empty_completion_count,
       substring(plan_line FROM 'empty-summary=[^/]+/batch:([^/]+)') AS empty_summary_batch,
       substring(plan_line FROM 'empty-summary=[^/]+/batch:[^/]+/saturated:([^,]+)') AS empty_batch_saturated,
       substring(plan_line FROM 'root-empty=completion:([^/]+)') AS root_empty_completion_count,
       substring(plan_line FROM 'root-empty=[^/]+/out:([^/]+)') AS root_empty_completion_out,
       substring(plan_line FROM 'root-empty=[^/]+/out:[^/]+/in:([^/]+)') AS root_empty_completion_in,
       substring(plan_line FROM 'root-empty=[^/]+/out:[^/]+/in:[^/]+/batch:([^/]+)') AS root_empty_batch_capacity,
       substring(plan_line FROM 'root-empty=[^/]+/out:[^/]+/in:[^/]+/batch:[^/]+/saturated-roots:([^,]+)') AS root_empty_saturated_count,
       substring(plan_line FROM 'threshold-feedback=([^/]+)') AS threshold_feedback,
       substring(plan_line FROM 'threshold-feedback=[^/]+/headroom:([^/]+)') AS threshold_feedback_headroom,
       substring(plan_line FROM 'threshold-feedback=[^/]+/headroom:[^/]+/batch:([^/]+)') AS threshold_feedback_batch,
       substring(plan_line FROM 'threshold-feedback=[^/]+/headroom:[^/]+/batch:[^/]+/source:([^/]+)') AS threshold_feedback_source,
       substring(plan_line FROM 'threshold-feedback=[^/]+/headroom:[^/]+/batch:[^/]+/source:[^/]+/reason:([^,]+)') AS threshold_feedback_reason,
       substring(plan_line FROM 'empty-evidence=([^,]+)') AS empty_evidence,
       substring(plan_line FROM 'suppressed-source=([^ ]+)') AS suppressed_source,
       substring(plan_line FROM 'suppression-match=([^,]+)') AS suppression_match,
       substring(plan_line FROM 'pressure=([^ ]+)') AS runtime_pressure,
       substring(plan_line FROM 'action=([^ ]+)') AS runtime_action
FROM vle_benchmark_explain
WHERE plan_line LIKE '%VLE Source Runtime%'
ORDER BY shape, line_no;

SELECT shape,
       substring(plan_line FROM 'profile=consumer:([^/]+)') AS consumer,
       substring(plan_line FROM 'profile=[^ ]*/class:([^/]+)') AS consumer_class,
       substring(plan_line FROM 'profile=[^ ]*/active:([^/]+)') AS active_direction,
       substring(plan_line FROM 'profile=[^ ]*/budget:([^/ ]+)') AS fanout_budget,
       substring(plan_line FROM 'profile=[^ ]*/weight:([^ ]+)') AS materialization_weight,
       substring(plan_line FROM 'policy=([^ ]+)') AS planner_policy,
       substring(plan_line FROM ' reason=([^ ]+)') AS policy_reason,
       substring(plan_line FROM 'combined-work=([^ ]+)') AS combined_work,
       substring(plan_line FROM 'cache-seed=([^ ]+)') AS cache_seed,
       substring(plan_line FROM 'endpoint-headroom=([^ ]+)') AS endpoint_headroom,
       substring(plan_line FROM 'empty-lifecycle=([^/]+)') AS empty_lifecycle,
       substring(plan_line FROM 'empty-lifecycle=[^/]+/depth:([^ ]+)') AS empty_lifecycle_depth,
       substring(plan_line FROM 'empty-batch=([^/]+)') AS empty_batch,
       substring(plan_line FROM 'empty-batch=[^/]+/size:([^ ]+)') AS empty_batch_size,
       substring(plan_line FROM 'threshold-input=([^/]+)') AS threshold_input,
       substring(plan_line FROM 'threshold-input=[^/]+/headroom:([^/]+)') AS threshold_input_headroom,
       substring(plan_line FROM 'threshold-input=[^/]+/headroom:[^/]+/batch:([^/]+)') AS threshold_input_batch,
       substring(plan_line FROM 'threshold-input=[^/]+/headroom:[^/]+/batch:[^/]+/source:([^/]+)') AS threshold_input_source,
       substring(plan_line FROM 'threshold-input=[^/]+/headroom:[^/]+/batch:[^/]+/source:[^/]+/reason:([^ ]+)') AS threshold_input_reason,
       substring(plan_line FROM 'threshold-cache=observed:([^/]+)') AS threshold_cache_observed,
       substring(plan_line FROM 'threshold-cache=[^/]+/saturated:([^/]+)') AS threshold_cache_saturated,
       substring(plan_line FROM 'threshold-cache=[^/]+/saturated:[^/]+/relaxed:([^ ]+)') AS threshold_cache_relaxed,
       substring(plan_line FROM 'payload-input=([^/]+)') AS payload_input,
       substring(plan_line FROM 'payload-input=[^/]+/headroom:([^/]+)') AS payload_input_headroom,
       substring(plan_line FROM 'payload-input=[^/]+/headroom:[^/]+/scan-runs:([^/]+)') AS payload_input_scan_runs,
       substring(plan_line FROM 'payload-input=[^/]+/headroom:[^/]+/scan-runs:[^/]+/replay-runs:([^/]+)') AS payload_input_replay_runs,
       substring(plan_line FROM 'payload-input=[^/]+/headroom:[^/]+/scan-runs:[^/]+/replay-runs:[^/]+/seed-runs:([^/]+)') AS payload_input_seed_runs,
       substring(plan_line FROM 'payload-input=[^/]+/headroom:[^/]+/scan-runs:[^/]+/replay-runs:[^/]+/seed-runs:[^/]+/replay-percent:([^/]+)') AS payload_input_replay_percent,
       substring(plan_line FROM 'payload-input=[^/]+/headroom:[^/]+/scan-runs:[^/]+/replay-runs:[^/]+/seed-runs:[^/]+/replay-percent:[^/]+/seed-percent:([^/]+)') AS payload_input_seed_percent,
       substring(plan_line FROM 'payload-input=[^/]+/headroom:[^/]+/scan-runs:[^/]+/replay-runs:[^/]+/seed-runs:[^/]+/replay-percent:[^/]+/seed-percent:[^/]+/observed:([^/]+)') AS payload_input_observed,
       substring(plan_line FROM 'payload-input=[^/]+/headroom:[^/]+/scan-runs:[^/]+/replay-runs:[^/]+/seed-runs:[^/]+/replay-percent:[^/]+/seed-percent:[^/]+/observed:[^/]+/reason:([^ ]+)') AS payload_input_reason,
       substring(plan_line FROM ' class=([^ ]+)') AS policy_class,
       substring(plan_line FROM ' recommendation=([^ ]+)') AS recommendation
FROM vle_benchmark_explain
WHERE plan_line LIKE '%VLE Edge Source:%'
  AND plan_line LIKE '%policy=%'
ORDER BY shape, line_no;

WITH planner AS (
    SELECT DISTINCT ON (shape)
           shape,
           substring(plan_line FROM 'profile=consumer:([^/]+)') AS consumer,
           substring(plan_line FROM 'profile=[^ ]*/class:([^/]+)') AS consumer_class,
           substring(plan_line FROM 'profile=[^ ]*/active:([^/]+)') AS active_direction,
           substring(plan_line FROM 'profile=[^ ]*/budget:([^/ ]+)') AS fanout_budget,
           substring(plan_line FROM 'profile=[^ ]*/weight:([^ ]+)') AS materialization_weight,
           substring(plan_line FROM 'policy=([^ ]+)') AS planner_policy,
           substring(plan_line FROM 'policy=out=([^/ ]+)') AS planner_out_source,
           substring(plan_line FROM 'policy=[^ ]+/in=([^ ]+)') AS planner_in_source,
           substring(plan_line FROM 'cache-seed=([^ ]+)') AS cache_seed,
           substring(plan_line FROM 'endpoint-headroom=([^ ]+)') AS endpoint_headroom,
           substring(plan_line FROM 'empty-lifecycle=([^/]+)') AS empty_lifecycle,
           substring(plan_line FROM 'empty-lifecycle=[^/]+/depth:([^ ]+)') AS empty_lifecycle_depth,
           substring(plan_line FROM 'empty-batch=([^/]+)') AS empty_batch,
           substring(plan_line FROM 'empty-batch=[^/]+/size:([^ ]+)') AS empty_batch_size,
           substring(plan_line FROM 'threshold-input=([^/]+)') AS threshold_input,
           substring(plan_line FROM 'threshold-input=[^/]+/headroom:([^/]+)') AS threshold_input_headroom,
           substring(plan_line FROM 'threshold-input=[^/]+/headroom:[^/]+/batch:([^/]+)') AS threshold_input_batch,
           substring(plan_line FROM 'threshold-input=[^/]+/headroom:[^/]+/batch:[^/]+/source:([^/]+)') AS threshold_input_source,
           substring(plan_line FROM 'threshold-input=[^/]+/headroom:[^/]+/batch:[^/]+/source:[^/]+/reason:([^ ]+)') AS threshold_input_reason,
           substring(plan_line FROM 'threshold-cache=observed:([^/]+)') AS threshold_cache_observed,
           substring(plan_line FROM 'threshold-cache=[^/]+/saturated:([^/]+)') AS threshold_cache_saturated,
           substring(plan_line FROM 'threshold-cache=[^/]+/saturated:[^/]+/relaxed:([^ ]+)') AS threshold_cache_relaxed,
           substring(plan_line FROM 'payload-input=([^/]+)') AS payload_input,
           substring(plan_line FROM 'payload-input=[^/]+/headroom:([^/]+)') AS payload_input_headroom,
           substring(plan_line FROM 'payload-input=[^/]+/headroom:[^/]+/scan-runs:([^/]+)') AS payload_input_scan_runs,
           substring(plan_line FROM 'payload-input=[^/]+/headroom:[^/]+/scan-runs:[^/]+/replay-runs:([^/]+)') AS payload_input_replay_runs,
           substring(plan_line FROM 'payload-input=[^/]+/headroom:[^/]+/scan-runs:[^/]+/replay-runs:[^/]+/seed-runs:([^/]+)') AS payload_input_seed_runs,
           substring(plan_line FROM 'payload-input=[^/]+/headroom:[^/]+/scan-runs:[^/]+/replay-runs:[^/]+/seed-runs:[^/]+/replay-percent:([^/]+)') AS payload_input_replay_percent,
           substring(plan_line FROM 'payload-input=[^/]+/headroom:[^/]+/scan-runs:[^/]+/replay-runs:[^/]+/seed-runs:[^/]+/replay-percent:[^/]+/seed-percent:([^/]+)') AS payload_input_seed_percent,
           substring(plan_line FROM 'payload-input=[^/]+/headroom:[^/]+/scan-runs:[^/]+/replay-runs:[^/]+/seed-runs:[^/]+/replay-percent:[^/]+/seed-percent:[^/]+/observed:([^/]+)') AS payload_input_observed,
           substring(plan_line FROM 'payload-input=[^/]+/headroom:[^/]+/scan-runs:[^/]+/replay-runs:[^/]+/seed-runs:[^/]+/replay-percent:[^/]+/seed-percent:[^/]+/observed:[^/]+/reason:([^ ]+)') AS payload_input_reason,
           substring(plan_line FROM ' class=([^ ]+)') AS planner_class,
           substring(plan_line FROM ' recommendation=([^ ]+)') AS planner_recommendation
    FROM vle_benchmark_explain
    WHERE plan_line LIKE '%VLE Edge Source:%'
      AND plan_line LIKE '%policy=%'
    ORDER BY shape, line_no
),
runtime AS (
    SELECT DISTINCT ON (shape)
           shape,
           substring(plan_line FROM 'feedback=dominant=([^ ]+)') AS dominant_source,
           substring(plan_line FROM 'planned=([^ ]+)') AS planned_source,
           substring(plan_line FROM 'planned=out:([^/ ]+)') AS planned_out_source,
           substring(plan_line FROM 'planned=[^ ]+/in:([^ ]+)') AS planned_in_source,
           substring(plan_line FROM 'source-match=([^ ]+)') AS runtime_source_match,
           substring(plan_line FROM ' class=([^ ]+)') AS runtime_class,
           substring(plan_line FROM 'planned-class=([^ ]+)') AS planned_class,
           substring(plan_line FROM 'class-match=([^ ]+)') AS runtime_class_match,
           substring(plan_line FROM ' recommendation=([^ ]+)') AS runtime_recommendation,
           substring(plan_line FROM 'planned-recommendation=([^ ]+)') AS planned_recommendation,
           substring(plan_line FROM 'density=age_adjacency:([^,]+)') AS age_adjacency_density,
           substring(plan_line FROM
                     'density=age_adjacency:[^,]+,endpoint-btree:([^,]+)') AS endpoint_btree_density,
           substring(plan_line FROM 'packed:([^ ]+)') AS packed_density,
           substring(plan_line FROM 'empty=age_adjacency:([^/]+)') AS age_adjacency_empty_scans,
           substring(plan_line FROM 'endpoint-btree:([^,]+), empty-suppressed') AS endpoint_btree_empty_scans,
           substring(plan_line FROM 'empty-suppressed=age_adjacency:([^/]+)') AS age_adjacency_empty_source_skips,
           substring(plan_line FROM '/out:([^/]+)') AS age_adjacency_empty_source_skip_out,
           substring(plan_line FROM '/in:([^,]+), empty-cache') AS age_adjacency_empty_source_skip_in,
           substring(plan_line FROM 'empty-cache=age_adjacency:([^/]+)') AS age_adjacency_empty_source_cache_hits,
           substring(plan_line FROM 'empty-cache=age_adjacency:[^/]+/out:([^/]+)') AS age_adjacency_empty_source_cache_hit_out,
           substring(plan_line FROM 'empty-cache=age_adjacency:[^/]+/out:[^/]+/in:([^,]+)') AS age_adjacency_empty_source_cache_hit_in,
           substring(plan_line FROM 'empty-frontier=age_adjacency:([^/]+)') AS age_adjacency_empty_source_frontier_marks,
           substring(plan_line FROM 'empty-frontier=age_adjacency:[^/]+/out:([^/]+)') AS age_adjacency_empty_source_frontier_mark_out,
           substring(plan_line FROM 'empty-frontier=age_adjacency:[^/]+/out:[^/]+/in:([^,]+)') AS age_adjacency_empty_source_frontier_mark_in,
           substring(plan_line FROM 'empty-frontier-batch=flushes:([^/]+)') AS empty_frontier_batch_flushes,
           substring(plan_line FROM 'empty-frontier-batch=[^/]+/out:([^/]+)') AS empty_frontier_batch_out,
           substring(plan_line FROM 'empty-frontier-batch=[^/]+/out:[^/]+/in:([^/]+)') AS empty_frontier_batch_in,
           substring(plan_line FROM 'empty-frontier-batch=[^/]+/out:[^/]+/in:[^/]+/keys:([^/]+)') AS empty_frontier_batch_keys,
           substring(plan_line FROM 'empty-frontier-batch=[^/]+/out:[^/]+/in:[^/]+/keys:[^/]+/max:([^,]+)') AS empty_frontier_batch_max,
           substring(plan_line FROM 'empty-run=age_adjacency:([^/]+)') AS age_adjacency_empty_source_run_skips,
           substring(plan_line FROM 'empty-run=age_adjacency:[^/]+/out:([^/]+)') AS age_adjacency_empty_source_run_skip_out,
           substring(plan_line FROM 'empty-run=age_adjacency:[^/]+/out:[^/]+/in:([^,]+)') AS age_adjacency_empty_source_run_skip_in,
           substring(plan_line FROM 'payload-cache=runs:scan:([^/]+)') AS payload_scan_runs,
           substring(plan_line FROM 'payload-cache=[^/]+/replay:([^/]+)') AS payload_replay_runs,
           substring(plan_line FROM 'payload-cache=[^/]+/replay:[^/]+/seed:([^/]+)') AS payload_seed_runs,
           substring(plan_line FROM 'payload-cache=[^/]+/replay:[^/]+/seed:[^/]+/tuples:scan:([^/]+)') AS payload_scan_tuples,
           substring(plan_line FROM 'payload-cache=[^/]+/replay:[^/]+/seed:[^/]+/tuples:scan:[^/]+/replay:([^/]+)') AS payload_replay_tuples,
           substring(plan_line FROM 'payload-cache=[^/]+/replay:[^/]+/seed:[^/]+/tuples:scan:[^/]+/replay:[^/]+/seeds:([^,]+)') AS payload_seed_events,
           substring(plan_line FROM 'empty-plan=([^/]+)') AS empty_plan,
           substring(plan_line FROM 'empty-plan=[^/]+/depth:([^ ]+)') AS empty_plan_depth,
           substring(plan_line FROM 'empty-plan=[^ ]+ match=([^,]+)') AS runtime_empty_plan_match,
           substring(plan_line FROM 'empty-context=([^/]+)') AS empty_context,
           substring(plan_line FROM 'empty-context=[^/]+/depth:([^/]+)') AS empty_context_depth,
           substring(plan_line FROM 'empty-context=[^ ]+ match=([^,]+)') AS runtime_empty_context_match,
           substring(plan_line FROM 'empty-batch=([^/]+)') AS empty_batch,
           substring(plan_line FROM 'empty-batch=[^/]+/size:([^/]+)') AS empty_batch_size,
           substring(plan_line FROM 'empty-batch=[^/]+/size:[^/]+/capacity:([^ ]+)') AS empty_batch_capacity,
           substring(plan_line FROM 'empty-batch=[^ ]+ match=([^,]+)') AS runtime_empty_batch_match,
           substring(plan_line FROM 'empty-summary=completion:([^/]+)') AS empty_completion_count,
           substring(plan_line FROM 'empty-summary=[^/]+/batch:([^/]+)') AS empty_summary_batch,
           substring(plan_line FROM 'empty-summary=[^/]+/batch:[^/]+/saturated:([^,]+)') AS empty_batch_saturated,
           substring(plan_line FROM 'root-empty=completion:([^/]+)') AS root_empty_completion_count,
           substring(plan_line FROM 'root-empty=[^/]+/out:([^/]+)') AS root_empty_completion_out,
           substring(plan_line FROM 'root-empty=[^/]+/out:[^/]+/in:([^/]+)') AS root_empty_completion_in,
           substring(plan_line FROM 'root-empty=[^/]+/out:[^/]+/in:[^/]+/batch:([^/]+)') AS root_empty_batch_capacity,
           substring(plan_line FROM 'root-empty=[^/]+/out:[^/]+/in:[^/]+/batch:[^/]+/saturated-roots:([^,]+)') AS root_empty_saturated_count,
           substring(plan_line FROM 'threshold-feedback=([^/]+)') AS threshold_feedback,
           substring(plan_line FROM 'threshold-feedback=[^/]+/headroom:([^/]+)') AS threshold_feedback_headroom,
           substring(plan_line FROM 'threshold-feedback=[^/]+/headroom:[^/]+/batch:([^/]+)') AS threshold_feedback_batch,
           substring(plan_line FROM 'threshold-feedback=[^/]+/headroom:[^/]+/batch:[^/]+/source:([^/]+)') AS threshold_feedback_source,
           substring(plan_line FROM 'threshold-feedback=[^/]+/headroom:[^/]+/batch:[^/]+/source:[^/]+/reason:([^,]+)') AS threshold_feedback_reason,
           substring(plan_line FROM 'empty-evidence=([^,]+)') AS empty_evidence,
           substring(plan_line FROM 'suppressed-source=([^ ]+)') AS suppressed_source,
           substring(plan_line FROM 'suppression-match=([^,]+)') AS suppression_match,
           substring(plan_line FROM 'pressure=([^ ]+)') AS runtime_pressure,
           substring(plan_line FROM 'action=([^ ]+)') AS runtime_action
    FROM vle_benchmark_explain
    WHERE plan_line LIKE '%VLE Source Runtime%'
    ORDER BY shape, line_no
)
SELECT p.shape,
       b.rows_returned,
       b.elapsed_ms,
       c.label_fanout_edges,
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
       p.payload_input_reason,
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
