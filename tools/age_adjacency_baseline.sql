-- AGE adjacency decision-gate benchmark.
--
-- This script measures the baseline paths that an age_adjacency index must
-- beat before it should be connected to VLE planning:
--
--   * existing btree indexes on edge label start_id/end_id columns
--   * cold and warm global graph cache VLE traversal
--   * label-constrained and unconstrained bound-endpoint VLE shapes
--
-- Run after installing AGE, for example:
--
--   psql -d postgres -f tools/age_adjacency_baseline.sql
--
-- Optional psql variables:
--
--   psql -d postgres \
--        -v graph=age_adjacency_gate \
--        -v chain_nodes=48 \
--        -v noise_per_vertex=12 \
--        -v repeats=5 \
--        -f tools/age_adjacency_baseline.sql

\set ON_ERROR_STOP on

\if :{?graph}
\else
    \set graph age_adjacency_gate
\endif
\if :{?chain_nodes}
\else
    \set chain_nodes 48
\endif
\if :{?noise_per_vertex}
\else
    \set noise_per_vertex 12
\endif
\if :{?repeats}
\else
    \set repeats 5
\endif

\timing on

CREATE EXTENSION IF NOT EXISTS age;
LOAD 'age';
SET search_path TO ag_catalog, public;

CREATE TEMP TABLE age_adjacency_baseline_config AS
SELECT :'graph'::name AS graph_name,
       :chain_nodes::int AS chain_nodes,
       :noise_per_vertex::int AS noise_per_vertex,
       :repeats::int AS repeats;

DO $$
DECLARE
    graph_name name;
BEGIN
    SELECT c.graph_name INTO graph_name
    FROM age_adjacency_baseline_config c;

    PERFORM drop_graph(graph_name, true);
EXCEPTION WHEN OTHERS THEN
    NULL;
END
$$;

SELECT create_graph(graph_name)
FROM age_adjacency_baseline_config;

DO $$
DECLARE
    graph_name name;
BEGIN
    SELECT c.graph_name INTO graph_name
    FROM age_adjacency_baseline_config c;

    EXECUTE format('SELECT create_vlabel(%L, %L)', graph_name, 'N');
    EXECUTE format('SELECT create_vlabel(%L, %L)', graph_name, 'Sink');
    EXECUTE format('SELECT create_elabel(%L, %L)', graph_name, 'Keep');
    EXECUTE format('SELECT create_elabel(%L, %L)', graph_name, 'Noise');
END
$$;

DO $$
DECLARE
    graph_name name;
    chain_nodes int;
    noise_per_vertex int;
    n_label_id int;
    sink_label_id int;
    keep_label_id int;
    noise_label_id int;
BEGIN
    SELECT c.graph_name, c.chain_nodes, c.noise_per_vertex
    INTO graph_name, chain_nodes, noise_per_vertex
    FROM age_adjacency_baseline_config c;

    n_label_id := _label_id(graph_name, 'N');
    sink_label_id := _label_id(graph_name, 'Sink');
    keep_label_id := _label_id(graph_name, 'Keep');
    noise_label_id := _label_id(graph_name, 'Noise');

    EXECUTE format(
        'INSERT INTO %I."N"(id, properties)
         SELECT ag_catalog._graphid(%s, (i + 1)::bigint),
                format(''{"i": %%s}'', i)::ag_catalog.agtype
         FROM generate_series(0, %s) AS g(i)',
        graph_name, n_label_id, chain_nodes - 1);

    EXECUTE format(
        'INSERT INTO %I."Sink"(id, properties)
         SELECT ag_catalog._graphid(%s,
                                    (i * %s + j + 1)::bigint),
                format(''{"i": %%s, "j": %%s}'', i, j)::ag_catalog.agtype
         FROM generate_series(0, %s) AS i
         CROSS JOIN generate_series(0, %s) AS j',
        graph_name, sink_label_id, noise_per_vertex, chain_nodes - 1,
        noise_per_vertex - 1);

    EXECUTE format(
        'INSERT INTO %I."Keep"(id, start_id, end_id, properties)
         SELECT ag_catalog._graphid(%s, (i + 1)::bigint),
                ag_catalog._graphid(%s, (i + 1)::bigint),
                ag_catalog._graphid(%s, (i + 2)::bigint),
                format(''{"kind": "keep", "step": %%s}'', i)::ag_catalog.agtype
         FROM generate_series(0, %s) AS g(i)',
        graph_name, keep_label_id, n_label_id, n_label_id, chain_nodes - 2);

    EXECUTE format(
        'INSERT INTO %I."Noise"(id, start_id, end_id, properties)
         SELECT ag_catalog._graphid(%s, (i * %s + j + 1)::bigint),
                ag_catalog._graphid(%s, (i + 1)::bigint),
                ag_catalog._graphid(%s, (i * %s + j + 1)::bigint),
                format(''{"kind": "noise", "i": %%s, "j": %%s}'',
                       i, j)::ag_catalog.agtype
         FROM generate_series(0, %s) AS i
         CROSS JOIN generate_series(0, %s) AS j',
        graph_name, noise_label_id, noise_per_vertex, n_label_id,
        sink_label_id, noise_per_vertex, chain_nodes - 1,
        noise_per_vertex - 1);

    EXECUTE format('ANALYZE %I."N"', graph_name);
    EXECUTE format('ANALYZE %I."Sink"', graph_name);
    EXECUTE format('ANALYZE %I."Keep"', graph_name);
    EXECUTE format('ANALYZE %I."Noise"', graph_name);
END
$$;

CREATE TEMP TABLE age_adjacency_baseline_results
(
    case_name text NOT NULL,
    run_no int NOT NULL,
    rows_returned bigint NOT NULL,
    elapsed_ms numeric NOT NULL,
    PRIMARY KEY (case_name, run_no)
);

CREATE FUNCTION pg_temp.run_age_adjacency_sql_case(
    case_name text,
    count_sql text,
    repeats int)
RETURNS void
LANGUAGE plpgsql
AS $$
DECLARE
    run_no int;
    started_at timestamptz;
    rows_returned bigint;
BEGIN
    FOR run_no IN 1..repeats LOOP
        started_at := clock_timestamp();
        EXECUTE count_sql INTO rows_returned;

        INSERT INTO age_adjacency_baseline_results
        VALUES
        (
            case_name,
            run_no,
            rows_returned,
            round((extract(epoch FROM clock_timestamp() - started_at) *
                   1000)::numeric, 3)
        );
    END LOOP;
END
$$;

CREATE FUNCTION pg_temp.run_age_adjacency_cypher_case(
    graph_name text,
    case_name text,
    query text,
    repeats int,
    clear_cache_each_run boolean)
RETURNS void
LANGUAGE plpgsql
AS $$
DECLARE
    run_no int;
    started_at timestamptz;
    rows_returned bigint;
BEGIN
    FOR run_no IN 1..repeats LOOP
        IF clear_cache_each_run THEN
            EXECUTE format(
                'SELECT * FROM ag_catalog.cypher(
                     %L,
                     $age_adj_delete$RETURN delete_global_graphs(%L)$age_adj_delete$)
                 AS (result ag_catalog.agtype)',
                graph_name, graph_name);
        END IF;

        started_at := clock_timestamp();
        EXECUTE format(
            'SELECT count(*)
             FROM ag_catalog.cypher(%L, $age_adj_gate$%s$age_adj_gate$)
             AS (result ag_catalog.agtype)',
            graph_name, query)
        INTO rows_returned;

        INSERT INTO age_adjacency_baseline_results
        VALUES
        (
            case_name,
            run_no,
            rows_returned,
            round((extract(epoch FROM clock_timestamp() - started_at) *
                   1000)::numeric, 3)
        );
    END LOOP;
END
$$;

SELECT graph_name,
       chain_nodes,
       noise_per_vertex,
       repeats,
       (chain_nodes - 1) AS keep_edges,
       (chain_nodes * noise_per_vertex) AS noise_edges
FROM age_adjacency_baseline_config;

SELECT schemaname, tablename, indexname, indexdef
FROM pg_indexes
WHERE schemaname = (SELECT graph_name::text
                    FROM age_adjacency_baseline_config)
  AND tablename IN ('Keep', 'Noise')
ORDER BY tablename, indexname;

SELECT pg_temp.run_age_adjacency_sql_case(
    'sql_keep_start_btree',
    format(
        'SELECT count(*) FROM %I."Keep"
         WHERE start_id = ag_catalog._graphid(%s, 1)',
        graph_name,
        _label_id(graph_name, 'N')),
    repeats)
FROM age_adjacency_baseline_config;

SELECT pg_temp.run_age_adjacency_sql_case(
    'sql_keep_end_btree',
    format(
        'SELECT count(*) FROM %I."Keep"
         WHERE end_id = ag_catalog._graphid(%s, %s)',
        graph_name,
        _label_id(graph_name, 'N'),
        chain_nodes),
    repeats)
FROM age_adjacency_baseline_config;

SELECT pg_temp.run_age_adjacency_sql_case(
    'sql_noise_start_btree',
    format(
        'SELECT count(*) FROM %I."Noise"
         WHERE start_id = ag_catalog._graphid(%s, 1)',
        graph_name,
        _label_id(graph_name, 'N')),
    repeats)
FROM age_adjacency_baseline_config;

SELECT pg_temp.run_age_adjacency_cypher_case(
    graph_name,
    'noidx_vle_keep_one_bound_cold',
    'MATCH p=(:N {i: 0})-[:Keep*1..8]->() RETURN p',
    repeats,
    true)
FROM age_adjacency_baseline_config;

SELECT pg_temp.run_age_adjacency_cypher_case(
    graph_name,
    'noidx_vle_keep_one_bound_warm',
    'MATCH p=(:N {i: 0})-[:Keep*1..8]->() RETURN p',
    repeats,
    false)
FROM age_adjacency_baseline_config;

SELECT pg_temp.run_age_adjacency_cypher_case(
    graph_name,
    'noidx_vle_keep_to_bound_warm',
    'MATCH p=()-[:Keep*1..8]->(:N {i: 8}) RETURN p',
    repeats,
    false)
FROM age_adjacency_baseline_config;

SELECT pg_temp.run_age_adjacency_cypher_case(
    graph_name,
    'noidx_vle_unlabeled_one_bound_warm',
    'MATCH p=(:N {i: 0})-[*1..2]->() RETURN p',
    repeats,
    false)
FROM age_adjacency_baseline_config;

SELECT pg_temp.run_age_adjacency_cypher_case(
    graph_name,
    'noidx_vle_keep_label_roots_warm',
    'MATCH p=(:N)-[:Keep*1..2]->() RETURN p',
    repeats,
    false)
FROM age_adjacency_baseline_config;

DO $$
DECLARE
    graph_name name;
BEGIN
    SELECT c.graph_name INTO graph_name
    FROM age_adjacency_baseline_config c;

    EXECUTE format(
        'CREATE INDEX %I ON %I."Keep"
         USING age_adjacency (start_id, id, end_id)',
        'Keep_age_adjacency_start_payload_idx', graph_name);
    EXECUTE format(
        'CREATE INDEX %I ON %I."Keep"
         USING age_adjacency (end_id, id, start_id)',
        'Keep_age_adjacency_end_payload_idx', graph_name);
    EXECUTE format(
        'CREATE INDEX %I ON %I."Noise"
         USING age_adjacency (start_id, id, end_id)',
        'Noise_age_adjacency_start_payload_idx', graph_name);
END
$$;

SELECT pg_temp.run_age_adjacency_sql_case(
    'sql_keep_start_age_adjacency_payload',
    format(
        'SELECT count(*)
         FROM ag_catalog.age_adjacency_debug_payload(%L::regclass,
              ag_catalog._graphid(%s, 1))',
        format('%I.%I', graph_name,
               'Keep_age_adjacency_start_payload_idx'),
        _label_id(graph_name, 'N')),
    repeats)
FROM age_adjacency_baseline_config;

SELECT pg_temp.run_age_adjacency_sql_case(
    'sql_noise_start_age_adjacency_payload',
    format(
        'SELECT count(*)
         FROM ag_catalog.age_adjacency_debug_payload(%L::regclass,
              ag_catalog._graphid(%s, 1))',
        format('%I.%I', graph_name,
               'Noise_age_adjacency_start_payload_idx'),
        _label_id(graph_name, 'N')),
    repeats)
FROM age_adjacency_baseline_config;

SELECT pg_temp.run_age_adjacency_cypher_case(
    graph_name,
    'idx_vle_keep_one_bound_cold',
    'MATCH p=(:N {i: 0})-[:Keep*1..8]->() RETURN p',
    repeats,
    true)
FROM age_adjacency_baseline_config;

SELECT pg_temp.run_age_adjacency_cypher_case(
    graph_name,
    'idx_vle_keep_one_bound_warm',
    'MATCH p=(:N {i: 0})-[:Keep*1..8]->() RETURN p',
    repeats,
    false)
FROM age_adjacency_baseline_config;

SELECT pg_temp.run_age_adjacency_cypher_case(
    graph_name,
    'idx_vle_keep_to_bound_warm',
    'MATCH p=()-[:Keep*1..8]->(:N {i: 8}) RETURN p',
    repeats,
    false)
FROM age_adjacency_baseline_config;

SELECT pg_temp.run_age_adjacency_cypher_case(
    graph_name,
    'idx_vle_unlabeled_one_bound_warm',
    'MATCH p=(:N {i: 0})-[*1..2]->() RETURN p',
    repeats,
    false)
FROM age_adjacency_baseline_config;

SELECT pg_temp.run_age_adjacency_cypher_case(
    graph_name,
    'idx_vle_keep_label_roots_warm',
    'MATCH p=(:N)-[:Keep*1..2]->() RETURN p',
    repeats,
    false)
FROM age_adjacency_baseline_config;

TABLE age_adjacency_baseline_results
ORDER BY case_name, run_no;

SELECT case_name,
       min(rows_returned) AS rows_returned,
       round(min(elapsed_ms), 3) AS min_ms,
       round(avg(elapsed_ms), 3) AS avg_ms,
       round(max(elapsed_ms), 3) AS max_ms
FROM age_adjacency_baseline_results
GROUP BY case_name
ORDER BY case_name;

SELECT 'decision_gate' AS note,
       'Keep age_adjacency as an opt-in VLE candidate source. Do not make it '
       || 'the default label path until it consistently beats the endpoint '
       || 'btree/global-cache baseline on representative workloads.' AS guidance;

SELECT drop_graph(graph_name, true)
FROM age_adjacency_baseline_config;
