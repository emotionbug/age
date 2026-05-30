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
--        -v run_warm_pressure=0 \
--        -f tools/age_adjacency_baseline.sql
--
-- Optional large warm-pressure mode:
--
--   psql -d postgres \
--        -v chain_nodes=4096 \
--        -v noise_per_vertex=64 \
--        -v repeats=3 \
--        -v run_warm_pressure=1 \
--        -v warm_pressure_repeats=3 \
--        -v label_pressure_labels=0 \
--        -v label_pressure_per_vertex=64 \
--        -v edge_property_padding_bytes=0 \
--        -f tools/age_adjacency_baseline.sql
--
-- Optional fixed-provider-only repeat mode:
--
--   psql -d postgres \
--        -v run_fixed_provider_pressure=1 \
--        -v fixed_provider_repeats=10 \
--        -v run_adjacency_guard_matrix=1 \
--        -v provider_function_cost=1 \
--        -v provider_function_rows=1 \
--        -v edge_property_padding_bytes=512 \
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
\if :{?directory_probe_keys}
\else
    \set directory_probe_keys 4096
\endif
\if :{?run_warm_pressure}
\else
    \set run_warm_pressure 0
\endif
\if :{?warm_pressure_repeats}
\else
    \set warm_pressure_repeats 3
\endif
\if :{?run_fixed_provider_pressure}
\else
    \set run_fixed_provider_pressure 0
\endif
\if :{?fixed_provider_repeats}
\else
    \set fixed_provider_repeats :repeats
\endif
\if :{?run_adjacency_guard_matrix}
\else
    \set run_adjacency_guard_matrix 0
\endif
\if :{?label_pressure_labels}
\else
    \set label_pressure_labels 0
\endif
\if :{?label_pressure_per_vertex}
\else
    \set label_pressure_per_vertex :noise_per_vertex
\endif
\if :{?edge_property_padding_bytes}
\else
    \set edge_property_padding_bytes 0
\endif
\if :{?provider_function_cost}
\else
    \set provider_function_cost 0
\endif
\if :{?provider_function_rows}
\else
    \set provider_function_rows 0
\endif

\timing on

CREATE EXTENSION IF NOT EXISTS age;
LOAD 'age';
SET search_path TO ag_catalog, public;

CREATE TEMP TABLE age_adjacency_provider_function_config AS
SELECT p.procost AS original_cost,
       p.prorows AS original_rows,
       :provider_function_cost::real AS target_cost,
       :provider_function_rows::real AS target_rows
FROM pg_proc p
WHERE p.oid = 'ag_catalog.age_adjacency_candidate_edge_rows(regclass, graphid, boolean)'::regprocedure;

DO $$
DECLARE
    provider_function regprocedure;
    target_cost real;
    target_rows real;
BEGIN
    provider_function :=
        'ag_catalog.age_adjacency_candidate_edge_rows(regclass, graphid, boolean)'::regprocedure;

    SELECT c.target_cost, c.target_rows
    INTO target_cost, target_rows
    FROM age_adjacency_provider_function_config c;

    IF target_cost > 0 THEN
        EXECUTE format('ALTER FUNCTION %s COST %s',
                       provider_function, target_cost);
    END IF;

    IF target_rows > 0 THEN
        EXECUTE format('ALTER FUNCTION %s ROWS %s',
                       provider_function, target_rows);
    END IF;
END
$$;

CREATE TEMP TABLE age_adjacency_provider_function_settings AS
SELECT c.original_cost,
       c.original_rows,
       c.target_cost,
       c.target_rows,
       p.procost AS effective_cost,
       p.prorows AS effective_rows
FROM age_adjacency_provider_function_config c
CROSS JOIN pg_proc p
WHERE p.oid = 'ag_catalog.age_adjacency_candidate_edge_rows(regclass, graphid, boolean)'::regprocedure;

CREATE TEMP TABLE age_adjacency_baseline_config AS
SELECT :'graph'::name AS graph_name,
       :chain_nodes::int AS chain_nodes,
       :noise_per_vertex::int AS noise_per_vertex,
       :label_pressure_labels::int AS label_pressure_labels,
       :label_pressure_per_vertex::int AS label_pressure_per_vertex,
       :edge_property_padding_bytes::int AS edge_property_padding_bytes,
       :provider_function_cost::real AS provider_function_cost,
       :provider_function_rows::real AS provider_function_rows,
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
    label_no int;
BEGIN
    SELECT c.graph_name INTO graph_name
    FROM age_adjacency_baseline_config c;

    EXECUTE format('SELECT create_vlabel(%L, %L)', graph_name, 'N');
    EXECUTE format('SELECT create_vlabel(%L, %L)', graph_name, 'Sink');
    EXECUTE format('SELECT create_elabel(%L, %L)', graph_name, 'Keep');
    EXECUTE format('SELECT create_elabel(%L, %L)', graph_name, 'Noise');

    FOR label_no IN
        SELECT generate_series(1, (SELECT c.label_pressure_labels
                                   FROM age_adjacency_baseline_config c))
    LOOP
        EXECUTE format('SELECT create_elabel(%L, %L)',
                       graph_name, format('NoiseExtra%s', label_no));
    END LOOP;
END
$$;

DO $$
DECLARE
    graph_name name;
    chain_nodes int;
    noise_per_vertex int;
    sink_per_vertex int;
    label_pressure_labels int;
    label_pressure_per_vertex int;
    edge_property_padding_bytes int;
    n_label_id int;
    sink_label_id int;
    keep_label_id int;
    noise_label_id int;
    extra_label_id int;
    label_no int;
BEGIN
    SELECT c.graph_name, c.chain_nodes, c.noise_per_vertex,
           c.label_pressure_labels, c.label_pressure_per_vertex,
           c.edge_property_padding_bytes
    INTO graph_name, chain_nodes, noise_per_vertex,
         label_pressure_labels, label_pressure_per_vertex,
         edge_property_padding_bytes
    FROM age_adjacency_baseline_config c;

    sink_per_vertex := GREATEST(noise_per_vertex, label_pressure_per_vertex);
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
        graph_name, sink_label_id, sink_per_vertex, chain_nodes - 1,
        sink_per_vertex - 1);

    EXECUTE format(
        'INSERT INTO %I."Keep"(id, start_id, end_id, properties)
         SELECT ag_catalog._graphid(%s, (i + 1)::bigint),
                ag_catalog._graphid(%s, (i + 1)::bigint),
                ag_catalog._graphid(%s, (i + 2)::bigint),
                format(''{"kind": "keep", "step": %%s, "pad": "%%s"}'',
                       i, repeat(''x'', %s))::ag_catalog.agtype
         FROM generate_series(0, %s) AS g(i)',
        graph_name, keep_label_id, n_label_id, n_label_id,
        edge_property_padding_bytes, chain_nodes - 2);

    EXECUTE format(
        'INSERT INTO %I."Noise"(id, start_id, end_id, properties)
         SELECT ag_catalog._graphid(%s, (i * %s + j + 1)::bigint),
                ag_catalog._graphid(%s, (i + 1)::bigint),
                ag_catalog._graphid(%s, (i * %s + j + 1)::bigint),
                format(''{"kind": "noise", "i": %%s, "j": %%s,
                         "pad": "%%s"}'',
                       i, j, repeat(''x'', %s))::ag_catalog.agtype
         FROM generate_series(0, %s) AS i
         CROSS JOIN generate_series(0, %s) AS j',
        graph_name, noise_label_id, noise_per_vertex, n_label_id,
        sink_label_id, sink_per_vertex, edge_property_padding_bytes,
        chain_nodes - 1,
        noise_per_vertex - 1);

    FOR label_no IN SELECT generate_series(1, label_pressure_labels)
    LOOP
        extra_label_id := _label_id(graph_name,
                                    format('NoiseExtra%s', label_no));

        EXECUTE format(
            'INSERT INTO %I.%I(id, start_id, end_id, properties)
             SELECT ag_catalog._graphid(%s,
                                        (i * %s + j + 1)::bigint),
                    ag_catalog._graphid(%s, (i + 1)::bigint),
                    ag_catalog._graphid(%s, (i * %s + j + 1)::bigint),
                    format(''{"kind": "noise_extra", "label": %%s,
                             "i": %%s, "j": %%s, "pad": "%%s"}'',
                           %s, i, j, repeat(''x'', %s))::ag_catalog.agtype
             FROM generate_series(0, %s) AS i
             CROSS JOIN generate_series(0, %s) AS j',
            graph_name, format('NoiseExtra%s', label_no),
            extra_label_id, label_pressure_per_vertex, n_label_id,
            sink_label_id, sink_per_vertex, label_no,
            edge_property_padding_bytes, chain_nodes - 1,
            label_pressure_per_vertex - 1);

        EXECUTE format('ANALYZE %I.%I',
                       graph_name, format('NoiseExtra%s', label_no));
    END LOOP;

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

CREATE TEMP TABLE age_adjacency_plan_costs
(
    case_name text PRIMARY KEY,
    startup_cost numeric NOT NULL,
    total_cost numeric NOT NULL,
    plan_rows bigint NOT NULL,
    plan_width int NOT NULL,
    uses_provider boolean NOT NULL,
    plan_line text NOT NULL
);

CREATE TEMP TABLE age_adjacency_plan_lines
(
    case_name text NOT NULL,
    line_no int NOT NULL,
    startup_cost numeric,
    total_cost numeric,
    plan_rows bigint,
    plan_width int,
    mentions_provider boolean NOT NULL,
    plan_line text NOT NULL,
    PRIMARY KEY (case_name, line_no)
);

CREATE TEMP TABLE age_adjacency_index_stats
(
    phase text NOT NULL,
    index_name text NOT NULL,
    num_pages bigint NOT NULL,
    postings bigint NOT NULL,
    directory_entries bigint NOT NULL,
    delta_postings bigint NOT NULL,
    delta_reindex_threshold bigint NOT NULL,
    delta_reindex_recommended boolean NOT NULL
);

CREATE TEMP TABLE age_adjacency_label_stats
(
    phase text NOT NULL,
    label_name text NOT NULL,
    reltuples real NOT NULL,
    relpages int NOT NULL,
    actual_rows bigint NOT NULL,
    distinct_start_vertices bigint NOT NULL,
    avg_out_degree numeric NOT NULL,
    max_out_degree bigint NOT NULL,
    distinct_end_vertices bigint NOT NULL,
    avg_in_degree numeric NOT NULL,
    max_in_degree bigint NOT NULL
);

CREATE TEMP TABLE age_adjacency_directory_probe_results
(
    probe_name text NOT NULL,
    found boolean NOT NULL,
    directory_pages_visited bigint NOT NULL,
    directory_entries_scanned bigint NOT NULL
);

CREATE TEMP TABLE age_adjacency_guard_matrix
(
    guard_case text NOT NULL,
    expected_provider boolean NOT NULL,
    observed_provider boolean NOT NULL,
    matched_expected boolean NOT NULL
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

CREATE FUNCTION pg_temp.record_age_adjacency_plan_cost(
    graph_name text,
    plan_case_name text,
    query text)
RETURNS void
LANGUAGE plpgsql
AS $$
DECLARE
    plan_text text;
    cost_match text[];
    first_plan_line text;
    startup_cost numeric;
    total_cost numeric;
    plan_rows bigint;
    plan_width int;
    uses_provider boolean;
    line_no int;
BEGIN
    uses_provider := false;
    line_no := 0;

    DELETE FROM age_adjacency_plan_lines
    WHERE case_name = plan_case_name;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM ag_catalog.cypher(%L, $age_adj_plan$EXPLAIN %s$age_adj_plan$)
         AS (plan ag_catalog.agtype)',
        graph_name, query)
    LOOP
        line_no := line_no + 1;

        IF plan_text LIKE '%age_adjacency_candidate_edge_rows%' THEN
            uses_provider := true;
        END IF;

        cost_match := regexp_match(
            plan_text,
            'cost=([0-9.]+)\.\.([0-9.]+) rows=([0-9]+) width=([0-9]+)');

        IF cost_match IS NOT NULL AND first_plan_line IS NULL THEN
            first_plan_line := plan_text;
            startup_cost := cost_match[1]::numeric;
            total_cost := cost_match[2]::numeric;
            plan_rows := cost_match[3]::bigint;
            plan_width := cost_match[4]::int;
        END IF;

        INSERT INTO age_adjacency_plan_lines
        VALUES
        (
            plan_case_name,
            line_no,
            CASE WHEN cost_match IS NULL THEN NULL
                 ELSE cost_match[1]::numeric END,
            CASE WHEN cost_match IS NULL THEN NULL
                 ELSE cost_match[2]::numeric END,
            CASE WHEN cost_match IS NULL THEN NULL
                 ELSE cost_match[3]::bigint END,
            CASE WHEN cost_match IS NULL THEN NULL
                 ELSE cost_match[4]::int END,
            plan_text LIKE '%age_adjacency_candidate_edge_rows%',
            plan_text
        );
    END LOOP;

    IF first_plan_line IS NULL THEN
        RAISE EXCEPTION 'could not find EXPLAIN cost line for %', plan_case_name;
    END IF;

    INSERT INTO age_adjacency_plan_costs
    VALUES
    (
        plan_case_name,
        startup_cost,
        total_cost,
        plan_rows,
        plan_width,
        uses_provider,
        first_plan_line
    )
    ON CONFLICT (case_name) DO UPDATE
    SET startup_cost = EXCLUDED.startup_cost,
        total_cost = EXCLUDED.total_cost,
        plan_rows = EXCLUDED.plan_rows,
        plan_width = EXCLUDED.plan_width,
        uses_provider = EXCLUDED.uses_provider,
        plan_line = EXCLUDED.plan_line;
END
$$;

CREATE FUNCTION pg_temp.record_age_adjacency_index_stats(
    phase text,
    index_name text,
    index_regclass text)
RETURNS void
LANGUAGE plpgsql
AS $$
BEGIN
    INSERT INTO age_adjacency_index_stats
    SELECT phase,
           index_name,
           num_pages,
           postings,
           directory_entries,
           delta_postings,
           delta_reindex_threshold,
           delta_reindex_recommended
    FROM ag_catalog.age_adjacency_debug_stats(index_regclass::regclass);
END
$$;

CREATE FUNCTION pg_temp.record_age_adjacency_label_stats(
    phase text,
    graph_name name,
    label_name name)
RETURNS void
LANGUAGE plpgsql
AS $$
DECLARE
    label_regclass regclass;
    reltuples real;
    relpages int;
    actual_rows bigint;
    distinct_start_vertices bigint;
    avg_out_degree numeric;
    max_out_degree bigint;
    distinct_end_vertices bigint;
    avg_in_degree numeric;
    max_in_degree bigint;
BEGIN
    label_regclass := format('%I.%I', graph_name, label_name)::regclass;

    SELECT c.reltuples, c.relpages
    INTO reltuples, relpages
    FROM pg_class c
    WHERE c.oid = label_regclass;

    EXECUTE format('SELECT count(*) FROM %I.%I', graph_name, label_name)
    INTO actual_rows;

    EXECUTE format(
        'SELECT count(*), COALESCE(avg(edge_count), 0),
                COALESCE(max(edge_count), 0)
         FROM (
             SELECT count(*) AS edge_count
             FROM %I.%I
             GROUP BY start_id
         ) AS fanout',
        graph_name, label_name)
    INTO distinct_start_vertices, avg_out_degree, max_out_degree;

    EXECUTE format(
        'SELECT count(*), COALESCE(avg(edge_count), 0),
                COALESCE(max(edge_count), 0)
         FROM (
             SELECT count(*) AS edge_count
             FROM %I.%I
             GROUP BY end_id
         ) AS fanin',
        graph_name, label_name)
    INTO distinct_end_vertices, avg_in_degree, max_in_degree;

    INSERT INTO age_adjacency_label_stats
    VALUES (phase, label_name, reltuples, relpages, actual_rows,
            distinct_start_vertices, round(avg_out_degree, 3), max_out_degree,
            distinct_end_vertices, round(avg_in_degree, 3), max_in_degree);
END
$$;

CREATE FUNCTION pg_temp.record_age_adjacency_directory_probe(
    probe_name text,
    index_regclass text,
    key ag_catalog.graphid)
RETURNS void
LANGUAGE plpgsql
AS $$
BEGIN
    INSERT INTO age_adjacency_directory_probe_results
    SELECT probe_name,
           probe.found,
           probe.directory_pages_visited,
           probe.directory_entries_scanned
    FROM ag_catalog.age_adjacency_debug_directory_probe(index_regclass::regclass,
                                                        key) AS probe;
END
$$;

CREATE FUNCTION pg_temp.record_age_adjacency_guard_case(
    graph_name name,
    guard_case text,
    cypher_query text,
    expected_provider boolean)
RETURNS void
LANGUAGE plpgsql
AS $$
DECLARE
    observed_provider boolean;
    plan_text text;
BEGIN
    observed_provider := false;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text AS plan_text
         FROM cypher(%L, $age_guard$%s$age_guard$) AS (plan agtype)',
        graph_name,
        'EXPLAIN (COSTS OFF) ' || cypher_query)
    LOOP
        IF plan_text LIKE '%age_adjacency_candidate_edge_rows%' THEN
            observed_provider := true;
            EXIT;
        END IF;
    END LOOP;

    INSERT INTO age_adjacency_guard_matrix
    VALUES (guard_case, expected_provider, observed_provider,
            expected_provider = observed_provider);
END
$$;

CREATE FUNCTION pg_temp.assert_age_adjacency_guard_matrix()
RETURNS void
LANGUAGE plpgsql
AS $$
DECLARE
    mismatches text;
BEGIN
    SELECT string_agg(
               format('%s expected=%s observed=%s',
                      guard_case, expected_provider, observed_provider),
               '; ' ORDER BY guard_case)
    INTO mismatches
    FROM age_adjacency_guard_matrix
    WHERE NOT matched_expected;

    IF mismatches IS NOT NULL THEN
        RAISE EXCEPTION 'age_adjacency guard matrix mismatch: %', mismatches;
    END IF;
END
$$;

SELECT graph_name,
       chain_nodes,
       noise_per_vertex,
       repeats,
       :directory_probe_keys::int AS directory_probe_keys,
       label_pressure_labels,
       label_pressure_per_vertex,
       :edge_property_padding_bytes::int AS edge_property_padding_bytes,
       (chain_nodes - 1) AS keep_edges,
       (chain_nodes * noise_per_vertex) AS noise_edges,
       (chain_nodes * label_pressure_labels *
        label_pressure_per_vertex) AS label_pressure_edges
FROM age_adjacency_baseline_config;

CREATE TEMP TABLE age_adjacency_directory_probe_data
(
    id graphid NOT NULL,
    start_id graphid NOT NULL,
    end_id graphid NOT NULL
);

INSERT INTO age_adjacency_directory_probe_data
SELECT _graphid(2, i::bigint),
       _graphid(1, i::bigint),
       _graphid(1, (i + 1)::bigint)
FROM generate_series(1, :directory_probe_keys::int) AS g(i);

CREATE INDEX age_adjacency_directory_probe_idx
ON age_adjacency_directory_probe_data
USING age_adjacency (start_id, id, end_id);

SELECT pg_temp.record_age_adjacency_directory_probe(
    'first_key',
    'age_adjacency_directory_probe_idx',
    _graphid(1, 1));

SELECT pg_temp.record_age_adjacency_directory_probe(
    'middle_key',
    'age_adjacency_directory_probe_idx',
    _graphid(1, (:directory_probe_keys::int / 2)::bigint));

SELECT pg_temp.record_age_adjacency_directory_probe(
    'last_key',
    'age_adjacency_directory_probe_idx',
    _graphid(1, :directory_probe_keys::bigint));

SELECT pg_temp.record_age_adjacency_directory_probe(
    'absent_high_key',
    'age_adjacency_directory_probe_idx',
    _graphid(1, (:directory_probe_keys::bigint + 1)));

TABLE age_adjacency_directory_probe_results
ORDER BY probe_name;

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

SELECT pg_temp.run_age_adjacency_cypher_case(
    graph_name,
    'noidx_match_keep_one_hop_bound_warm',
    'MATCH (:N {i: 0})-[:Keep]->(n) RETURN n.i',
    repeats,
    false)
FROM age_adjacency_baseline_config;

\if :run_warm_pressure

SELECT pg_temp.run_age_adjacency_cypher_case(
    graph_name,
    'pressure_noidx_vle_keep_one_bound_warm',
    'MATCH p=(:N {i: 0})-[:Keep*1..8]->() RETURN p',
    :warm_pressure_repeats::int,
    false)
FROM age_adjacency_baseline_config;

SELECT pg_temp.run_age_adjacency_cypher_case(
    graph_name,
    'pressure_noidx_vle_keep_to_bound_warm',
    'MATCH p=()-[:Keep*1..8]->(:N {i: 8}) RETURN p',
    :warm_pressure_repeats::int,
    false)
FROM age_adjacency_baseline_config;

SELECT pg_temp.run_age_adjacency_cypher_case(
    graph_name,
    'pressure_noidx_vle_keep_label_roots_warm',
    'MATCH p=(:N)-[:Keep*1..2]->() RETURN p',
    :warm_pressure_repeats::int,
    false)
FROM age_adjacency_baseline_config;

SELECT pg_temp.run_age_adjacency_cypher_case(
    graph_name,
    'pressure_noidx_vle_noise_one_bound_warm',
    'MATCH p=(:N {i: 0})-[:Noise*1..1]->() RETURN p',
    :warm_pressure_repeats::int,
    false)
FROM age_adjacency_baseline_config;

SELECT pg_temp.run_age_adjacency_cypher_case(
    graph_name,
    'pressure_noidx_vle_unlabeled_one_bound_warm',
    'MATCH p=(:N {i: 0})-[*1..1]->() RETURN p',
    :warm_pressure_repeats::int,
    false)
FROM age_adjacency_baseline_config;

\endif

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

SELECT pg_temp.record_age_adjacency_index_stats(
    'after_build',
    'Keep_start',
    format('%I.%I', graph_name, 'Keep_age_adjacency_start_payload_idx'))
FROM age_adjacency_baseline_config;

SELECT pg_temp.record_age_adjacency_index_stats(
    'after_build',
    'Keep_end',
    format('%I.%I', graph_name, 'Keep_age_adjacency_end_payload_idx'))
FROM age_adjacency_baseline_config;

SELECT pg_temp.record_age_adjacency_index_stats(
    'after_build',
    'Noise_start',
    format('%I.%I', graph_name, 'Noise_age_adjacency_start_payload_idx'))
FROM age_adjacency_baseline_config;

SELECT pg_temp.record_age_adjacency_label_stats(
    'after_build',
    graph_name,
    'Keep')
FROM age_adjacency_baseline_config;

SELECT pg_temp.record_age_adjacency_label_stats(
    'after_build',
    graph_name,
    'Noise')
FROM age_adjacency_baseline_config;

\if :run_adjacency_guard_matrix

SET age.enable_adjacency_match = off;

SELECT pg_temp.record_age_adjacency_guard_case(
    graph_name,
    'guc_off_inline_bound',
    'MATCH (:N {i: 0})-[:Keep]->(n) RETURN n.i',
    false)
FROM age_adjacency_baseline_config;

SET age.enable_adjacency_match = on;

SELECT pg_temp.record_age_adjacency_guard_case(
    graph_name,
    'inline_bound_anonymous_edge',
    'MATCH (:N {i: 0})-[:Keep]->(n) RETURN n.i',
    true)
FROM age_adjacency_baseline_config;

SELECT pg_temp.record_age_adjacency_guard_case(
    graph_name,
    'left_direction_bound',
    'MATCH (:N {i: 1})<-[:Keep]-(n) RETURN n.i',
    true)
FROM age_adjacency_baseline_config;

SELECT pg_temp.record_age_adjacency_guard_case(
    graph_name,
    'edge_property_predicate',
    'MATCH (:N {i: 0})-[:Keep {kind: "keep"}]->(n) RETURN n.i',
    true)
FROM age_adjacency_baseline_config;

SELECT pg_temp.record_age_adjacency_guard_case(
    graph_name,
    'new_edge_variable',
    'MATCH (:N {i: 0})-[e:Keep {kind: "keep"}]->(n) RETURN e.kind',
    true)
FROM age_adjacency_baseline_config;

SELECT pg_temp.record_age_adjacency_guard_case(
    graph_name,
    'unlabeled_no_matching_index',
    'MATCH (:N {i: 0})-[]->(n) RETURN n.i',
    false)
FROM age_adjacency_baseline_config;

SELECT pg_temp.record_age_adjacency_guard_case(
    graph_name,
    'unbound_named_endpoint',
    'MATCH (m:N)-[:Keep]->(n) RETURN n.i',
    false)
FROM age_adjacency_baseline_config;

SELECT pg_temp.record_age_adjacency_guard_case(
    graph_name,
    'unbound_label_only_endpoint',
    'MATCH (:N)-[:Keep]->(n) RETURN n.i',
    false)
FROM age_adjacency_baseline_config;

SELECT pg_temp.record_age_adjacency_guard_case(
    graph_name,
    'undirected_edge',
    'MATCH (:N {i: 0})-[:Keep]-(n) RETURN n.i',
    false)
FROM age_adjacency_baseline_config;

SELECT pg_temp.record_age_adjacency_guard_case(
    graph_name,
    'path_variable',
    'MATCH p=(:N {i: 0})-[:Keep]->(n) RETURN n.i',
    false)
FROM age_adjacency_baseline_config;

SELECT pg_temp.record_age_adjacency_guard_case(
    graph_name,
    'varlen_edge',
    'MATCH p=(:N {i: 0})-[:Keep*1..2]->(n) RETURN n.i',
    false)
FROM age_adjacency_baseline_config;

SET age.enable_adjacency_match = off;

\endif

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
    'sql_keep_one_hop_candidate_provider',
    format(
        'SELECT count(*)
         FROM ag_catalog.age_adjacency_candidate_edges(%L::regclass,
              ag_catalog._graphid(%s, 1)) AS c
         JOIN %I."N" AS n ON n.id = c.next_vertex_id',
        format('%I.%I', graph_name,
               'Keep_age_adjacency_start_payload_idx'),
        _label_id(graph_name, 'N'),
        graph_name),
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

SELECT pg_temp.run_age_adjacency_cypher_case(
    graph_name,
    'idx_match_keep_one_hop_bound_btree_warm',
    'MATCH (:N {i: 0})-[:Keep]->(n) RETURN n.i',
    repeats,
    false)
FROM age_adjacency_baseline_config;

SELECT pg_temp.run_age_adjacency_cypher_case(
    graph_name,
    'idx_match_keep_one_hop_edge_prop_btree_warm',
    'MATCH (:N {i: 0})-[:Keep {kind: "keep"}]->(n) RETURN n.i',
    repeats,
    false)
FROM age_adjacency_baseline_config;

SELECT pg_temp.run_age_adjacency_cypher_case(
    graph_name,
    'idx_match_keep_one_hop_edge_var_btree_warm',
    'MATCH (:N {i: 0})-[e:Keep {kind: "keep"}]->(n) RETURN e.kind',
    repeats,
    false)
FROM age_adjacency_baseline_config;

SET age.enable_adjacency_match = on;

SELECT pg_temp.run_age_adjacency_cypher_case(
    graph_name,
    'idx_match_keep_one_hop_bound_provider_warm',
    'MATCH (:N {i: 0})-[:Keep]->(n) RETURN n.i',
    repeats,
    false)
FROM age_adjacency_baseline_config;

SELECT pg_temp.run_age_adjacency_cypher_case(
    graph_name,
    'idx_match_keep_one_hop_edge_prop_provider_warm',
    'MATCH (:N {i: 0})-[:Keep {kind: "keep"}]->(n) RETURN n.i',
    repeats,
    false)
FROM age_adjacency_baseline_config;

SELECT pg_temp.run_age_adjacency_cypher_case(
    graph_name,
    'idx_match_keep_one_hop_edge_var_provider_warm',
    'MATCH (:N {i: 0})-[e:Keep {kind: "keep"}]->(n) RETURN e.kind',
    repeats,
    false)
FROM age_adjacency_baseline_config;

SET age.enable_adjacency_match = off;

\if :run_fixed_provider_pressure

SELECT pg_temp.record_age_adjacency_plan_cost(
    graph_name,
    'fixed_pressure_idx_match_keep_one_hop_bound_btree_warm',
    'MATCH (:N {i: 0})-[:Keep]->(n) RETURN n.i')
FROM age_adjacency_baseline_config;

SELECT pg_temp.record_age_adjacency_plan_cost(
    graph_name,
    'fixed_pressure_idx_match_keep_one_hop_edge_prop_btree_warm',
    'MATCH (:N {i: 0})-[:Keep {kind: "keep"}]->(n) RETURN n.i')
FROM age_adjacency_baseline_config;

SELECT pg_temp.record_age_adjacency_plan_cost(
    graph_name,
    'fixed_pressure_idx_match_keep_one_hop_edge_var_btree_warm',
    'MATCH (:N {i: 0})-[e:Keep {kind: "keep"}]->(n) RETURN e.kind')
FROM age_adjacency_baseline_config;

SELECT pg_temp.record_age_adjacency_plan_cost(
    graph_name,
    'fixed_pressure_idx_match_keep_one_hop_right_label_btree_warm',
    'MATCH (:N {i: 0})-[:Keep]->(n:N) RETURN n.i')
FROM age_adjacency_baseline_config;

SELECT pg_temp.run_age_adjacency_cypher_case(
    graph_name,
    'fixed_pressure_idx_match_keep_one_hop_bound_btree_warm',
    'MATCH (:N {i: 0})-[:Keep]->(n) RETURN n.i',
    :fixed_provider_repeats::int,
    false)
FROM age_adjacency_baseline_config;

SELECT pg_temp.run_age_adjacency_cypher_case(
    graph_name,
    'fixed_pressure_idx_match_keep_one_hop_edge_prop_btree_warm',
    'MATCH (:N {i: 0})-[:Keep {kind: "keep"}]->(n) RETURN n.i',
    :fixed_provider_repeats::int,
    false)
FROM age_adjacency_baseline_config;

SELECT pg_temp.run_age_adjacency_cypher_case(
    graph_name,
    'fixed_pressure_idx_match_keep_one_hop_edge_var_btree_warm',
    'MATCH (:N {i: 0})-[e:Keep {kind: "keep"}]->(n) RETURN e.kind',
    :fixed_provider_repeats::int,
    false)
FROM age_adjacency_baseline_config;

SELECT pg_temp.run_age_adjacency_cypher_case(
    graph_name,
    'fixed_pressure_idx_match_keep_one_hop_right_label_btree_warm',
    'MATCH (:N {i: 0})-[:Keep]->(n:N) RETURN n.i',
    :fixed_provider_repeats::int,
    false)
FROM age_adjacency_baseline_config;

SET age.enable_adjacency_match = on;

SELECT pg_temp.record_age_adjacency_plan_cost(
    graph_name,
    'fixed_pressure_idx_match_keep_one_hop_bound_provider_warm',
    'MATCH (:N {i: 0})-[:Keep]->(n) RETURN n.i')
FROM age_adjacency_baseline_config;

SELECT pg_temp.record_age_adjacency_plan_cost(
    graph_name,
    'fixed_pressure_idx_match_keep_one_hop_edge_prop_provider_warm',
    'MATCH (:N {i: 0})-[:Keep {kind: "keep"}]->(n) RETURN n.i')
FROM age_adjacency_baseline_config;

SELECT pg_temp.record_age_adjacency_plan_cost(
    graph_name,
    'fixed_pressure_idx_match_keep_one_hop_edge_var_provider_warm',
    'MATCH (:N {i: 0})-[e:Keep {kind: "keep"}]->(n) RETURN e.kind')
FROM age_adjacency_baseline_config;

SELECT pg_temp.record_age_adjacency_plan_cost(
    graph_name,
    'fixed_pressure_idx_match_keep_one_hop_right_label_provider_warm',
    'MATCH (:N {i: 0})-[:Keep]->(n:N) RETURN n.i')
FROM age_adjacency_baseline_config;

SELECT pg_temp.run_age_adjacency_cypher_case(
    graph_name,
    'fixed_pressure_idx_match_keep_one_hop_bound_provider_warm',
    'MATCH (:N {i: 0})-[:Keep]->(n) RETURN n.i',
    :fixed_provider_repeats::int,
    false)
FROM age_adjacency_baseline_config;

SELECT pg_temp.run_age_adjacency_cypher_case(
    graph_name,
    'fixed_pressure_idx_match_keep_one_hop_edge_prop_provider_warm',
    'MATCH (:N {i: 0})-[:Keep {kind: "keep"}]->(n) RETURN n.i',
    :fixed_provider_repeats::int,
    false)
FROM age_adjacency_baseline_config;

SELECT pg_temp.run_age_adjacency_cypher_case(
    graph_name,
    'fixed_pressure_idx_match_keep_one_hop_edge_var_provider_warm',
    'MATCH (:N {i: 0})-[e:Keep {kind: "keep"}]->(n) RETURN e.kind',
    :fixed_provider_repeats::int,
    false)
FROM age_adjacency_baseline_config;

SELECT pg_temp.run_age_adjacency_cypher_case(
    graph_name,
    'fixed_pressure_idx_match_keep_one_hop_right_label_provider_warm',
    'MATCH (:N {i: 0})-[:Keep]->(n:N) RETURN n.i',
    :fixed_provider_repeats::int,
    false)
FROM age_adjacency_baseline_config;

SET age.enable_adjacency_match = off;

\endif

\if :run_warm_pressure

SELECT pg_temp.run_age_adjacency_cypher_case(
    graph_name,
    'pressure_idx_vle_keep_one_bound_warm',
    'MATCH p=(:N {i: 0})-[:Keep*1..8]->() RETURN p',
    :warm_pressure_repeats::int,
    false)
FROM age_adjacency_baseline_config;

SELECT pg_temp.run_age_adjacency_cypher_case(
    graph_name,
    'pressure_idx_vle_keep_to_bound_warm',
    'MATCH p=()-[:Keep*1..8]->(:N {i: 8}) RETURN p',
    :warm_pressure_repeats::int,
    false)
FROM age_adjacency_baseline_config;

SELECT pg_temp.run_age_adjacency_cypher_case(
    graph_name,
    'pressure_idx_vle_keep_label_roots_warm',
    'MATCH p=(:N)-[:Keep*1..2]->() RETURN p',
    :warm_pressure_repeats::int,
    false)
FROM age_adjacency_baseline_config;

SELECT pg_temp.run_age_adjacency_cypher_case(
    graph_name,
    'pressure_idx_vle_noise_one_bound_warm',
    'MATCH p=(:N {i: 0})-[:Noise*1..1]->() RETURN p',
    :warm_pressure_repeats::int,
    false)
FROM age_adjacency_baseline_config;

SELECT pg_temp.run_age_adjacency_cypher_case(
    graph_name,
    'pressure_idx_vle_unlabeled_one_bound_warm',
    'MATCH p=(:N {i: 0})-[*1..1]->() RETURN p',
    :warm_pressure_repeats::int,
    false)
FROM age_adjacency_baseline_config;

SELECT pg_temp.run_age_adjacency_cypher_case(
    graph_name,
    'pressure_idx_match_keep_one_hop_bound_btree_warm',
    'MATCH (:N {i: 0})-[:Keep]->(n) RETURN n.i',
    :warm_pressure_repeats::int,
    false)
FROM age_adjacency_baseline_config;

SELECT pg_temp.run_age_adjacency_cypher_case(
    graph_name,
    'pressure_idx_match_keep_one_hop_edge_prop_btree_warm',
    'MATCH (:N {i: 0})-[:Keep {kind: "keep"}]->(n) RETURN n.i',
    :warm_pressure_repeats::int,
    false)
FROM age_adjacency_baseline_config;

SELECT pg_temp.run_age_adjacency_cypher_case(
    graph_name,
    'pressure_idx_match_keep_one_hop_edge_var_btree_warm',
    'MATCH (:N {i: 0})-[e:Keep {kind: "keep"}]->(n) RETURN e.kind',
    :warm_pressure_repeats::int,
    false)
FROM age_adjacency_baseline_config;

SET age.enable_adjacency_match = on;

SELECT pg_temp.run_age_adjacency_cypher_case(
    graph_name,
    'pressure_idx_match_keep_one_hop_bound_provider_warm',
    'MATCH (:N {i: 0})-[:Keep]->(n) RETURN n.i',
    :warm_pressure_repeats::int,
    false)
FROM age_adjacency_baseline_config;

SELECT pg_temp.run_age_adjacency_cypher_case(
    graph_name,
    'pressure_idx_match_keep_one_hop_edge_prop_provider_warm',
    'MATCH (:N {i: 0})-[:Keep {kind: "keep"}]->(n) RETURN n.i',
    :warm_pressure_repeats::int,
    false)
FROM age_adjacency_baseline_config;

SELECT pg_temp.run_age_adjacency_cypher_case(
    graph_name,
    'pressure_idx_match_keep_one_hop_edge_var_provider_warm',
    'MATCH (:N {i: 0})-[e:Keep {kind: "keep"}]->(n) RETURN e.kind',
    :warm_pressure_repeats::int,
    false)
FROM age_adjacency_baseline_config;

SET age.enable_adjacency_match = off;

\endif

DO $$
DECLARE
    graph_name name;
    chain_nodes int;
    noise_per_vertex int;
    edge_property_padding_bytes int;
    n_label_id int;
    sink_label_id int;
    noise_label_id int;
    first_delta_id bigint;
BEGIN
    SELECT c.graph_name, c.chain_nodes, c.noise_per_vertex,
           c.edge_property_padding_bytes
    INTO graph_name, chain_nodes, noise_per_vertex,
         edge_property_padding_bytes
    FROM age_adjacency_baseline_config c;

    n_label_id := _label_id(graph_name, 'N');
    sink_label_id := _label_id(graph_name, 'Sink');
    noise_label_id := _label_id(graph_name, 'Noise');
    first_delta_id := (chain_nodes * noise_per_vertex) + 1;

    EXECUTE format(
        'INSERT INTO %I."Noise"(id, start_id, end_id, properties)
         SELECT ag_catalog._graphid(%s, (%s + j)::bigint),
                ag_catalog._graphid(%s, 1),
                ag_catalog._graphid(%s, (j + 1)::bigint),
                format(''{"kind": "delta_noise", "j": %%s,
                         "pad": "%%s"}'',
                       j, repeat(''x'', %s))::ag_catalog.agtype
         FROM generate_series(0, %s) AS g(j)',
        graph_name, noise_label_id, first_delta_id, n_label_id,
        sink_label_id, edge_property_padding_bytes, noise_per_vertex - 1);

    EXECUTE format('ANALYZE %I."Noise"', graph_name);
END
$$;

SELECT pg_temp.record_age_adjacency_index_stats(
    'after_delta_insert',
    'Noise_start',
    format('%I.%I', graph_name, 'Noise_age_adjacency_start_payload_idx'))
FROM age_adjacency_baseline_config;

SELECT pg_temp.record_age_adjacency_label_stats(
    'after_delta_insert',
    graph_name,
    'Noise')
FROM age_adjacency_baseline_config;

SELECT pg_temp.run_age_adjacency_sql_case(
    'sql_noise_start_age_adjacency_payload_delta_heavy',
    format(
        'SELECT count(*)
         FROM ag_catalog.age_adjacency_debug_payload(%L::regclass,
              ag_catalog._graphid(%s, 1))',
        format('%I.%I', graph_name,
               'Noise_age_adjacency_start_payload_idx'),
        _label_id(graph_name, 'N')),
    repeats)
FROM age_adjacency_baseline_config;

DO $$
DECLARE
    graph_name name;
BEGIN
    SELECT c.graph_name INTO graph_name
    FROM age_adjacency_baseline_config c;

    EXECUTE format('REINDEX INDEX %I.%I',
                   graph_name, 'Noise_age_adjacency_start_payload_idx');
END
$$;

SELECT pg_temp.record_age_adjacency_index_stats(
    'after_delta_reindex',
    'Noise_start',
    format('%I.%I', graph_name, 'Noise_age_adjacency_start_payload_idx'))
FROM age_adjacency_baseline_config;

SELECT pg_temp.record_age_adjacency_label_stats(
    'after_delta_reindex',
    graph_name,
    'Noise')
FROM age_adjacency_baseline_config;

SELECT pg_temp.run_age_adjacency_sql_case(
    'sql_noise_start_age_adjacency_payload_reindexed',
    format(
        'SELECT count(*)
         FROM ag_catalog.age_adjacency_debug_payload(%L::regclass,
              ag_catalog._graphid(%s, 1))',
        format('%I.%I', graph_name,
               'Noise_age_adjacency_start_payload_idx'),
        _label_id(graph_name, 'N')),
    repeats)
FROM age_adjacency_baseline_config;

DO $$
DECLARE
    graph_name name;
    chain_nodes int;
    noise_per_vertex int;
    noise_label_id int;
    first_delta_id bigint;
BEGIN
    SELECT c.graph_name, c.chain_nodes, c.noise_per_vertex
    INTO graph_name, chain_nodes, noise_per_vertex
    FROM age_adjacency_baseline_config c;

    noise_label_id := _label_id(graph_name, 'Noise');
    first_delta_id := (chain_nodes * noise_per_vertex) + 1;

    EXECUTE format(
        'DELETE FROM %I."Noise"
         WHERE id >= ag_catalog._graphid(%s, %s)
           AND id < ag_catalog._graphid(%s, %s)',
        graph_name, noise_label_id, first_delta_id,
        noise_label_id, first_delta_id + noise_per_vertex);
END
$$;

VACUUM (INDEX_CLEANUP ON) :"graph"."Noise";

SELECT pg_temp.record_age_adjacency_index_stats(
    'after_delta_vacuum',
    'Noise_start',
    format('%I.%I', graph_name, 'Noise_age_adjacency_start_payload_idx'))
FROM age_adjacency_baseline_config;

SELECT pg_temp.record_age_adjacency_label_stats(
    'after_delta_vacuum',
    graph_name,
    'Noise')
FROM age_adjacency_baseline_config;

TABLE age_adjacency_index_stats
ORDER BY phase, index_name;

TABLE age_adjacency_label_stats
ORDER BY phase, label_name;

\if :run_adjacency_guard_matrix

TABLE age_adjacency_guard_matrix
ORDER BY guard_case;

SELECT pg_temp.assert_age_adjacency_guard_matrix();

\endif

TABLE age_adjacency_baseline_results
ORDER BY case_name, run_no;

\if :run_fixed_provider_pressure

TABLE age_adjacency_provider_function_settings;

TABLE age_adjacency_plan_costs
ORDER BY case_name;

TABLE age_adjacency_plan_lines
ORDER BY case_name, line_no;

SELECT case_name,
       line_no,
       startup_cost,
       total_cost,
       plan_rows,
       plan_width,
       plan_line
FROM age_adjacency_plan_lines
WHERE mentions_provider
ORDER BY case_name, line_no;

SELECT case_name,
       max(total_cost) FILTER (WHERE line_no = 2) AS bound_endpoint_total_cost,
       max(total_cost) FILTER (
           WHERE mentions_provider
       ) AS function_scan_total_cost,
       max(total_cost) FILTER (
           WHERE plan_line LIKE '  ->  Append%'
       ) AS other_endpoint_append_total_cost
FROM age_adjacency_plan_lines
WHERE case_name LIKE 'fixed_pressure%provider%'
GROUP BY case_name
ORDER BY case_name;

SELECT case_name,
       bool_or(plan_line LIKE '% Scan on "Keep"%') AS has_keep_relation_scan,
       bool_or(plan_line LIKE '%age_adjacency_candidate_edge_rows%') AS has_provider_function,
       bool_or(plan_line LIKE '%start_id%'
               AND (plan_line LIKE '%Hash Cond:%'
                    OR plan_line LIKE '%Filter:%')) AS has_start_id_join_qual,
       bool_or(plan_line LIKE '%end_id%'
               AND (plan_line LIKE '%Hash Cond:%'
                    OR plan_line LIKE '%Filter:%'
                    OR plan_line LIKE '%Index Cond:%')) AS has_end_id_join_qual,
       bool_or(plan_line LIKE '  ->  Append%') AS has_other_endpoint_append
FROM age_adjacency_plan_lines
WHERE case_name LIKE 'fixed_pressure%'
GROUP BY case_name
ORDER BY case_name;

\endif

SELECT case_name,
       min(rows_returned) AS rows_returned,
       round(min(elapsed_ms), 3) AS min_ms,
       round(avg(elapsed_ms), 3) AS avg_ms,
       round(max(elapsed_ms), 3) AS max_ms
FROM age_adjacency_baseline_results
GROUP BY case_name
ORDER BY case_name;

\if :run_fixed_provider_pressure

WITH result_summary AS (
    SELECT case_name,
           min(rows_returned) AS rows_returned,
           avg(elapsed_ms) AS avg_ms
    FROM age_adjacency_baseline_results
    GROUP BY case_name
),
shape_map(shape_name, btree_case, provider_case) AS (
    VALUES
    (
        'bound',
        'fixed_pressure_idx_match_keep_one_hop_bound_btree_warm',
        'fixed_pressure_idx_match_keep_one_hop_bound_provider_warm'
    ),
    (
        'edge_property',
        'fixed_pressure_idx_match_keep_one_hop_edge_prop_btree_warm',
        'fixed_pressure_idx_match_keep_one_hop_edge_prop_provider_warm'
    ),
    (
        'edge_variable',
        'fixed_pressure_idx_match_keep_one_hop_edge_var_btree_warm',
        'fixed_pressure_idx_match_keep_one_hop_edge_var_provider_warm'
    ),
    (
        'right_label',
        'fixed_pressure_idx_match_keep_one_hop_right_label_btree_warm',
        'fixed_pressure_idx_match_keep_one_hop_right_label_provider_warm'
    )
),
keep_stats AS (
    SELECT actual_rows,
           distinct_start_vertices,
           avg_out_degree,
           max_out_degree,
           reltuples,
           relpages
    FROM age_adjacency_label_stats
    WHERE phase = 'after_build'
      AND label_name = 'Keep'
)
SELECT m.shape_name,
       b.rows_returned,
       round(b.avg_ms, 3) AS btree_avg_ms,
       round(p.avg_ms, 3) AS provider_avg_ms,
       round((b.avg_ms / NULLIF(p.avg_ms, 0)), 3) AS provider_speedup,
       bc.total_cost AS btree_total_cost,
       pc.total_cost AS provider_total_cost,
       round((bc.total_cost / NULLIF(pc.total_cost, 0)), 3) AS planner_cost_ratio,
       pc.uses_provider,
       k.actual_rows AS keep_edges,
       k.distinct_start_vertices,
       k.avg_out_degree,
       k.max_out_degree,
       k.reltuples,
       k.relpages
FROM shape_map m
JOIN result_summary b ON b.case_name = m.btree_case
JOIN result_summary p ON p.case_name = m.provider_case
JOIN age_adjacency_plan_costs bc ON bc.case_name = m.btree_case
JOIN age_adjacency_plan_costs pc ON pc.case_name = m.provider_case
CROSS JOIN keep_stats k
ORDER BY m.shape_name;

\endif

SELECT 'decision_gate' AS note,
       'Keep age_adjacency as an opt-in VLE candidate source. Do not make it '
       || 'the default label path until it consistently beats the endpoint '
       || 'btree/global-cache baseline on representative workloads.' AS guidance;

DO $$
DECLARE
    provider_function regprocedure;
    original_cost real;
    original_rows real;
BEGIN
    provider_function :=
        'ag_catalog.age_adjacency_candidate_edge_rows(regclass, graphid, boolean)'::regprocedure;

    SELECT c.original_cost, c.original_rows
    INTO original_cost, original_rows
    FROM age_adjacency_provider_function_config c;

    EXECUTE format('ALTER FUNCTION %s COST %s ROWS %s',
                   provider_function, original_cost, original_rows);
END
$$;

SELECT drop_graph(graph_name, true)
FROM age_adjacency_baseline_config;
