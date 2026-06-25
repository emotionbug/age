\set ON_ERROR_STOP on
LOAD 'age';
SET search_path TO ag_catalog, public;

DO $$
BEGIN
    PERFORM drop_graph('hidden_age_perf_regression', true);
EXCEPTION
    WHEN OTHERS THEN NULL;
END
$$;

SELECT create_graph('hidden_age_perf_regression');
SELECT create_vlabel('hidden_age_perf_regression', 'N');
SELECT create_vlabel('hidden_age_perf_regression', 'Sink');
SELECT create_elabel('hidden_age_perf_regression', 'Noise');

DO $$
DECLARE
    n_id int := _label_id('hidden_age_perf_regression', 'N');
    sink_id int := _label_id('hidden_age_perf_regression', 'Sink');
    noise_id int := _label_id('hidden_age_perf_regression', 'Noise');
BEGIN
    EXECUTE format($query$
        INSERT INTO hidden_age_perf_regression."N"(id, properties)
        SELECT _graphid(%s, i),
               format('{"i": %%s}', i)::agtype
        FROM generate_series(1, 512) AS i
    $query$, n_id);

    EXECUTE format($query$
        INSERT INTO hidden_age_perf_regression."Sink"(id, properties)
        SELECT _graphid(%s, (i - 1) * 64 + j),
               CASE WHEN i = 1 AND j = 1 THEN
                   format('{"i": %%s, "j": %%s, "bucket": %%s, '
                          '"needle": 1, '
                          '"pad": "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"}',
                          i, j, j %% 16)::agtype
               ELSE
                   format('{"i": %%s, "j": %%s, "bucket": %%s, '
                          '"pad": "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"}',
                          i, j, j %% 16)::agtype
               END
        FROM generate_series(1, 512) AS i
        CROSS JOIN generate_series(1, 64) AS j
    $query$, sink_id);

    EXECUTE format($query$
        INSERT INTO hidden_age_perf_regression."Noise"
                    (id, start_id, end_id, properties)
        SELECT _graphid(%s, (i - 1) * 64 + j),
               _graphid(%s, i),
               _graphid(%s, (i - 1) * 64 + j),
               format('{"j": %%s, "bucket": %%s}', j, j %% 16)::agtype
        FROM generate_series(1, 512) AS i
        CROSS JOIN generate_series(1, 64) AS j
    $query$, noise_id, n_id, sink_id);
END
$$;

CREATE INDEX "Noise_age_adj_start"
    ON hidden_age_perf_regression."Noise"
    USING age_adjacency(start_id, id, end_id);

CREATE INDEX "Noise_age_adj_end"
    ON hidden_age_perf_regression."Noise"
    USING age_adjacency(end_id, id, start_id);

SELECT create_property_source_index(
    'hidden_age_perf_regression', 'Sink', 'bucket');
SELECT create_property_source_index(
    'hidden_age_perf_regression', 'Sink', 'needle');

ANALYZE hidden_age_perf_regression."N";
ANALYZE hidden_age_perf_regression."Sink";
ANALYZE hidden_age_perf_regression."Noise";
