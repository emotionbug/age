-- Property aggregate/index-domain benchmark harness.
--
-- Run from psql after installing AGE, for example:
--
--   psql -d postgres -v graph=age_agg_index_bench -v rows=5000 \
--        -f tools/aggregate_index_benchmark.sql
--
--   psql -d postgres -v graph=age_agg_index_bench \
--        -v threshold_rows=100,250,500 \
--        -f tools/aggregate_index_benchmark.sql

\set ON_ERROR_STOP on

\if :{?graph}
\else
    \set graph age_agg_index_bench
\endif
\if :{?rows}
\else
    \set rows 5000
\endif
\if :{?preserve_graph}
\else
    \set preserve_graph 0
\endif
\if :{?wide_text_width}
\else
    \set wide_text_width 64
\endif

\timing on

CREATE EXTENSION IF NOT EXISTS age;
LOAD 'age';
SET search_path TO ag_catalog, public;

CREATE TEMP TABLE aggregate_index_benchmark_config AS
WITH input AS (
    SELECT :'graph'::text AS graph_name,
           :rows::int AS row_count,
           :wide_text_width::int AS wide_text_width
),
row_counts AS (
\if :{?threshold_rows}
    SELECT trim(value)::int AS row_count
    FROM regexp_split_to_table(:'threshold_rows', ',') AS value
\else
    SELECT row_count
    FROM input
\endif
)
SELECT CASE WHEN row_counts.row_count = input.row_count
                 AND NOT :{?threshold_rows}
            THEN input.graph_name
            ELSE input.graph_name || '_' || row_counts.row_count::text
       END AS graph_name,
       row_counts.row_count,
       input.wide_text_width
FROM input
CROSS JOIN row_counts;

\if :preserve_graph
\else
\timing off
\o /dev/null
SET client_min_messages TO warning;
SELECT format('SELECT ag_catalog.drop_label(%L, %L, false);',
              c.graph_name, l.name)
FROM aggregate_index_benchmark_config c
JOIN ag_catalog.ag_graph g ON g.name = c.graph_name::name
JOIN ag_catalog.ag_label l ON l.graph = g.graphid
ORDER BY l.kind = 'v', l.name
\gexec
SET client_min_messages TO default;
\o
\timing on

SELECT ag_catalog.drop_graph(c.graph_name, true)
FROM aggregate_index_benchmark_config c
JOIN ag_catalog.ag_graph g ON g.name = c.graph_name::name;
\endif

SELECT create_graph(graph_name) FROM aggregate_index_benchmark_config;

DO $$
DECLARE
    graph_name text;
    row_count int;
    config record;
    i int;
BEGIN
    FOR config IN
        SELECT c.graph_name, c.row_count, c.wide_text_width
        FROM aggregate_index_benchmark_config c
        ORDER BY c.row_count
    LOOP
        graph_name := config.graph_name;
        row_count := config.row_count;

        FOR i IN 1..row_count LOOP
            EXECUTE format(
                'SELECT * FROM ag_catalog.cypher(%L, $agg_bench$%s$agg_bench$) AS (r ag_catalog.agtype)',
                graph_name,
                format('CREATE (:AggNode {payload: {a: %s, b: %s, c: "%s", d: "%s"}, bucket: %s})',
                       i, i % 1000, 'v' || (i % 32),
                       repeat('x', config.wide_text_width), i % 16));
        END LOOP;
    END LOOP;
END
$$;

DO $$
DECLARE
    graph_name text;
    config record;
BEGIN
    FOR config IN
        SELECT c.graph_name
        FROM aggregate_index_benchmark_config c
        ORDER BY c.row_count
    LOOP
        graph_name := config.graph_name;

        EXECUTE format(
            'CREATE INDEX %I ON %I.%I ((ag_catalog.agtype_object_field_agtype(ag_catalog.agtype_object_field_agtype(properties, ''"payload"''::ag_catalog.agtype), ''"a"''::ag_catalog.agtype)))',
            graph_name || '_aggnode_payload_a_idx',
            graph_name,
            'AggNode');
        EXECUTE format('ANALYZE %I.%I', graph_name, 'AggNode');
    END LOOP;
END
$$;

CREATE TEMP TABLE aggregate_index_benchmark_results
(
    shape text,
    row_count int,
    rows_returned bigint,
    elapsed_ms numeric
);

CREATE TEMP TABLE aggregate_index_benchmark_explain
(
    shape text,
    row_count int,
    plan_line text
);

CREATE TEMP TABLE aggregate_index_benchmark_slot_state
(
    shape text,
    row_count int,
    slot_descriptor text,
    slot_state text,
    aggregate_rows int
);

CREATE OR REPLACE FUNCTION public.run_aggregate_index_benchmark_case(
    graph_name text, row_count int, shape text, query text)
RETURNS void
LANGUAGE plpgsql
AS $$
DECLARE
    start_ts timestamptz;
    finish_ts timestamptz;
    rows_returned bigint;
BEGIN
    start_ts := clock_timestamp();
    EXECUTE format('SELECT count(*) FROM ag_catalog.cypher(%L, %L) AS (result ag_catalog.agtype)',
                   graph_name, query)
    INTO rows_returned;
    finish_ts := clock_timestamp();

    INSERT INTO aggregate_index_benchmark_results(shape, row_count,
                                                  rows_returned,
                                                  elapsed_ms)
    VALUES (shape, row_count, rows_returned,
            EXTRACT(MILLISECOND FROM finish_ts - start_ts) +
            EXTRACT(SECOND FROM finish_ts - start_ts) * 1000);
END
$$;

CREATE OR REPLACE FUNCTION public.capture_aggregate_index_benchmark_slot_state(
    graph_name text, row_count int, shape text, descriptor_expr text,
    query text)
RETURNS void
LANGUAGE plpgsql
AS $$
DECLARE
    slot_descriptor text;
    aggregate_rows int;
BEGIN
    EXECUTE format(
        'SELECT %s,
                array_length(array_agg(result), 1)
         FROM ag_catalog.cypher(%L, %L) AS (result ag_catalog.agtype)',
         descriptor_expr, graph_name, query)
    INTO slot_descriptor, aggregate_rows;

    INSERT INTO aggregate_index_benchmark_slot_state(shape, row_count,
                                                     slot_descriptor,
                                                     aggregate_rows)
    VALUES (shape, row_count, slot_descriptor, aggregate_rows);
END
$$;

CREATE OR REPLACE FUNCTION public.capture_aggregate_index_benchmark_slot_summary(
    p_graph_name text, p_row_count int, p_shape text, p_summary_expr text,
    p_where_clause text)
RETURNS void
LANGUAGE plpgsql
AS $$
DECLARE
    captured_slot_state text;
BEGIN
    EXECUTE format('SELECT %s FROM %I.%I n %s',
                   p_summary_expr, p_graph_name, 'AggNode', p_where_clause)
    INTO captured_slot_state;

    UPDATE aggregate_index_benchmark_slot_state s
       SET slot_state = captured_slot_state
     WHERE s.shape = p_shape
       AND s.row_count = p_row_count;
END
$$;

CREATE OR REPLACE FUNCTION public.capture_aggregate_index_benchmark_explain(
    graph_name text, row_count int, shape text, descriptor_expr text,
    query text)
RETURNS void
LANGUAGE plpgsql
AS $$
DECLARE
    line text;
BEGIN
    FOR line IN EXECUTE format(
        'EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY ON)
         SELECT %s,
                array_length(array_agg(result), 1)
         FROM ag_catalog.cypher(%L, %L) AS (result ag_catalog.agtype)',
         descriptor_expr, graph_name, query)
    LOOP
        INSERT INTO aggregate_index_benchmark_explain(shape, row_count,
                                                      plan_line)
        VALUES (shape, row_count, line);
    END LOOP;
END
$$;

SELECT public.run_aggregate_index_benchmark_case(
           graph_name,
           row_count,
           'indexed-selective-natural',
           format('MATCH (n:AggNode) WHERE n.payload.a = %s RETURN [n.payload.a]',
                  row_count))
FROM aggregate_index_benchmark_config;

SELECT public.capture_aggregate_index_benchmark_explain(
           graph_name,
           row_count,
           'indexed-selective-natural',
           'ag_catalog.age_array_agg_slots_descriptor(NULL::ag_catalog.agtype)',
           format('MATCH (n:AggNode) WHERE n.payload.a = %s RETURN [n.payload.a]',
                  row_count))
FROM aggregate_index_benchmark_config;

SELECT public.capture_aggregate_index_benchmark_slot_state(
           graph_name,
           row_count,
           'indexed-selective-natural',
           'ag_catalog.age_array_agg_slots_descriptor(NULL::ag_catalog.agtype)',
           format('MATCH (n:AggNode) WHERE n.payload.a = %s RETURN [n.payload.a]',
                  row_count))
FROM aggregate_index_benchmark_config;

SELECT public.capture_aggregate_index_benchmark_slot_summary(
           graph_name,
           row_count,
           'indexed-selective-natural',
           'ag_catalog.age_array_agg_list_slots_summary(ag_catalog.agtype_object_field_agtype(ag_catalog.agtype_object_field_agtype(n.properties, ''"payload"''::ag_catalog.agtype), ''"a"''::ag_catalog.agtype))',
           format('WHERE ag_catalog.agtype_object_field_agtype(ag_catalog.agtype_object_field_agtype(n.properties, ''"payload"''::ag_catalog.agtype), ''"a"''::ag_catalog.agtype) = %L::ag_catalog.agtype',
                  row_count::text))
FROM aggregate_index_benchmark_config;

SET enable_seqscan = off;

SELECT public.run_aggregate_index_benchmark_case(
           graph_name,
           row_count,
           'indexed-selective-forced',
           format('MATCH (n:AggNode) WHERE n.payload.a = %s RETURN [n.payload.a]',
                  row_count))
FROM aggregate_index_benchmark_config;

SELECT public.capture_aggregate_index_benchmark_explain(
           graph_name,
           row_count,
           'indexed-selective-forced',
           'ag_catalog.age_array_agg_slots_descriptor(NULL::ag_catalog.agtype)',
           format('MATCH (n:AggNode) WHERE n.payload.a = %s RETURN [n.payload.a]',
                  row_count))
FROM aggregate_index_benchmark_config;

SELECT public.capture_aggregate_index_benchmark_slot_state(
           graph_name,
           row_count,
           'indexed-selective-forced',
           'ag_catalog.age_array_agg_slots_descriptor(NULL::ag_catalog.agtype)',
           format('MATCH (n:AggNode) WHERE n.payload.a = %s RETURN [n.payload.a]',
                  row_count))
FROM aggregate_index_benchmark_config;

SELECT public.capture_aggregate_index_benchmark_slot_summary(
           graph_name,
           row_count,
           'indexed-selective-forced',
           'ag_catalog.age_array_agg_list_slots_summary(ag_catalog.agtype_object_field_agtype(ag_catalog.agtype_object_field_agtype(n.properties, ''"payload"''::ag_catalog.agtype), ''"a"''::ag_catalog.agtype))',
           format('WHERE ag_catalog.agtype_object_field_agtype(ag_catalog.agtype_object_field_agtype(n.properties, ''"payload"''::ag_catalog.agtype), ''"a"''::ag_catalog.agtype) = %L::ag_catalog.agtype',
                  row_count::text))
FROM aggregate_index_benchmark_config;

SET enable_seqscan = on;

SELECT public.run_aggregate_index_benchmark_case(
           graph_name,
           row_count,
           'typed-list-aggregate',
           'MATCH (n:AggNode) RETURN [n.payload.a::pg_numeric, n.payload.b::pg_bigint]')
FROM aggregate_index_benchmark_config;

SELECT public.capture_aggregate_index_benchmark_explain(
           graph_name,
           row_count,
           'typed-list-aggregate',
           'ag_catalog.age_array_agg_slots_descriptor(NULL::numeric, NULL::bigint)',
           'MATCH (n:AggNode) RETURN [n.payload.a::pg_numeric, n.payload.b::pg_bigint]')
FROM aggregate_index_benchmark_config;

SELECT public.capture_aggregate_index_benchmark_slot_state(
           graph_name,
           row_count,
           'typed-list-aggregate',
           'ag_catalog.age_array_agg_slots_descriptor(NULL::numeric, NULL::bigint)',
           'MATCH (n:AggNode) RETURN [n.payload.a::pg_numeric, n.payload.b::pg_bigint]')
FROM aggregate_index_benchmark_config;

SELECT public.capture_aggregate_index_benchmark_slot_summary(
           graph_name,
           row_count,
           'typed-list-aggregate',
           'ag_catalog.age_array_agg_list_slots_summary(ag_catalog.agtype_object_field_numeric(ag_catalog.agtype_object_field_agtype(n.properties, ''"payload"''::ag_catalog.agtype), ''"a"''::ag_catalog.agtype), ag_catalog.agtype_object_field_int8(ag_catalog.agtype_object_field_agtype(n.properties, ''"payload"''::ag_catalog.agtype), ''"b"''::ag_catalog.agtype))',
           '')
FROM aggregate_index_benchmark_config;

SELECT public.run_aggregate_index_benchmark_case(
           graph_name,
           row_count,
           'typed-wide-text-aggregate',
           'MATCH (n:AggNode) RETURN [n.payload.a::pg_numeric, n.payload.d::pg_text]')
FROM aggregate_index_benchmark_config;

SELECT public.capture_aggregate_index_benchmark_explain(
           graph_name,
           row_count,
           'typed-wide-text-aggregate',
           'ag_catalog.age_array_agg_slots_descriptor(NULL::numeric, NULL::text)',
           'MATCH (n:AggNode) RETURN [n.payload.a::pg_numeric, n.payload.d::pg_text]')
FROM aggregate_index_benchmark_config;

SELECT public.capture_aggregate_index_benchmark_slot_state(
           graph_name,
           row_count,
           'typed-wide-text-aggregate',
           'ag_catalog.age_array_agg_slots_descriptor(NULL::numeric, NULL::text)',
           'MATCH (n:AggNode) RETURN [n.payload.a::pg_numeric, n.payload.d::pg_text]')
FROM aggregate_index_benchmark_config;

SELECT public.capture_aggregate_index_benchmark_slot_summary(
           graph_name,
           row_count,
           'typed-wide-text-aggregate',
           'ag_catalog.age_array_agg_list_slots_summary(ag_catalog.agtype_object_field_numeric(ag_catalog.agtype_object_field_agtype(n.properties, ''"payload"''::ag_catalog.agtype), ''"a"''::ag_catalog.agtype), ag_catalog.agtype_object_field_text_agtype(ag_catalog.agtype_object_field_agtype(n.properties, ''"payload"''::ag_catalog.agtype), ''"d"''::ag_catalog.agtype))',
           '')
FROM aggregate_index_benchmark_config;

TABLE aggregate_index_benchmark_results ORDER BY row_count, shape;

SELECT e.shape,
       e.row_count,
       s.slot_descriptor,
       s.slot_state,
       substring(s.slot_descriptor from 'slots=([0-9]+)')::int AS slot_count,
       substring(s.slot_descriptor from 'typed=([0-9]+)')::int AS slot_typed_count,
       substring(s.slot_descriptor from 'agtype=([0-9]+)')::int AS slot_agtype_count,
       substring(s.slot_descriptor from 'source-groups=([0-9]+)')::int AS slot_source_group_count,
       substring(s.slot_descriptor from 'reused-slots=([0-9]+)')::int AS slot_reused_count,
       substring(s.slot_descriptor from 'payload-weight=([0-9]+)')::int AS slot_payload_weight,
       substring(s.slot_descriptor from 'final-weight=([0-9]+)')::int AS slot_final_weight,
       substring(s.slot_descriptor from 'materialization-weight=([0-9]+)')::int AS slot_materialization_weight,
       substring(s.slot_descriptor from 'serialized-bytes=([0-9]+)')::int AS slot_serialized_bytes,
       substring(s.slot_descriptor from 'null-bitmap-bytes=([0-9]+)')::int AS slot_null_bitmap_bytes,
       substring(s.slot_descriptor from 'value-bytes=([0-9]+)')::int AS slot_value_bytes,
       substring(s.slot_descriptor from 'estimated-wire-width=([0-9]+)')::int AS slot_estimated_wire_width,
       substring(s.slot_descriptor from 'estimated-state-width-weight=([0-9]+)')::int AS slot_estimated_state_width_weight,
       substring(s.slot_state from 'value-bytes=([0-9]+)')::int AS slot_state_value_bytes,
       substring(s.slot_state from 'serialized-bytes=([0-9]+)')::int AS slot_state_serialized_bytes,
       CASE WHEN substring(s.slot_state from 'value-bytes=([0-9]+)')::numeric > 0
            THEN round(
                substring(s.slot_state from 'value-bytes=([0-9]+)')::numeric /
                GREATEST(s.aggregate_rows, 1),
                2)
            ELSE NULL END AS slot_state_value_bytes_per_row,
       CASE WHEN substring(s.slot_descriptor from 'estimated-wire-width=([0-9]+)')::numeric > 0
              AND substring(s.slot_state from 'value-bytes=([0-9]+)')::numeric > 0
            THEN round(
                (substring(s.slot_state from 'value-bytes=([0-9]+)')::numeric /
                 GREATEST(s.aggregate_rows, 1)) /
                substring(s.slot_descriptor from 'estimated-wire-width=([0-9]+)')::numeric,
                2)
            ELSE NULL END AS slot_value_estimate_ratio,
       substring(s.slot_descriptor from 'types=([^ ]+)') AS slot_types,
       s.aggregate_rows,
       max(e.plan_line) FILTER (WHERE e.plan_line LIKE '%Output:%') AS aggregate_output,
       max(e.plan_line) FILTER (
           WHERE e.plan_line LIKE '%age_array_agg%_slots(%') AS aggregate_slot_output,
       CASE WHEN count(*) FILTER (
                    WHERE e.plan_line LIKE '%age_array_agg%_slots(%') > 0
            THEN true ELSE false END AS aggregate_uses_slot_vector,
       max(e.plan_line) FILTER (WHERE e.plan_line LIKE '%Index Scan%') AS index_scan,
       CASE WHEN count(*) FILTER (WHERE e.plan_line LIKE '%Index Scan%') > 0
            THEN true ELSE false END AS child_uses_index,
       max(e.plan_line) FILTER (WHERE e.plan_line LIKE '%Execution Time:%') AS execution_time
FROM aggregate_index_benchmark_explain e
LEFT JOIN aggregate_index_benchmark_slot_state s
  ON s.shape = e.shape AND s.row_count = e.row_count
GROUP BY e.shape, e.row_count, s.slot_descriptor, s.slot_state,
         s.aggregate_rows
ORDER BY e.row_count, e.shape;

WITH plan_summary AS (
    SELECT shape,
           row_count,
           CASE WHEN count(*) FILTER (WHERE plan_line LIKE '%Index Scan%') > 0
                THEN true ELSE false END AS child_uses_index
    FROM aggregate_index_benchmark_explain
    GROUP BY shape, row_count
)
SELECT max(row_count) FILTER (
           WHERE shape = 'indexed-selective-natural'
             AND NOT child_uses_index) AS last_natural_child_seqscan_rows,
       min(row_count) FILTER (
           WHERE shape = 'indexed-selective-natural'
             AND child_uses_index) AS first_natural_child_index_rows,
       min(row_count) FILTER (
           WHERE shape = 'indexed-selective-forced'
             AND child_uses_index) AS first_forced_child_index_rows
FROM plan_summary;

DROP FUNCTION public.capture_aggregate_index_benchmark_explain(text, int, text, text, text);
DROP FUNCTION public.capture_aggregate_index_benchmark_slot_summary(text, int, text, text, text);
DROP FUNCTION public.capture_aggregate_index_benchmark_slot_state(text, int, text, text, text);
DROP FUNCTION public.run_aggregate_index_benchmark_case(text, int, text, text);
