\set ON_ERROR_STOP on
\if :{?fanout}
\else
\set fanout 500
\endif

LOAD 'age';
SET search_path = ag_catalog, "$user", public;
SET client_min_messages = warning;

SELECT drop_graph('wcoj_bench_semiring', true)
WHERE EXISTS (SELECT 1 FROM ag_graph WHERE name = 'wcoj_bench_semiring');
SELECT create_graph('wcoj_bench_semiring');
SELECT create_vlabel('wcoj_bench_semiring', 'S');
SELECT create_vlabel('wcoj_bench_semiring', 'T');
SELECT create_elabel('wcoj_bench_semiring', 'E1');
SELECT create_elabel('wcoj_bench_semiring', 'E2');
SELECT create_elabel('wcoj_bench_semiring', 'E3');

INSERT INTO wcoj_bench_semiring."S" (id, properties)
SELECT _graphid(_label_id('wcoj_bench_semiring', 'S'), source_no),
       format('{"id":%s}', source_no)::agtype
FROM generate_series(1, 3) source_no;

INSERT INTO wcoj_bench_semiring."T" (id, properties)
VALUES (_graphid(_label_id('wcoj_bench_semiring', 'T'), 1),
        '{"id":1}'::agtype);

INSERT INTO wcoj_bench_semiring."E1" (id, start_id, end_id, properties)
SELECT _graphid(_label_id('wcoj_bench_semiring', 'E1'), edge_no),
       _graphid(_label_id('wcoj_bench_semiring', 'S'), 1),
       _graphid(_label_id('wcoj_bench_semiring', 'T'), 1),
       format('{"score":%s}', edge_no)::agtype
FROM generate_series(1, :fanout) edge_no;

INSERT INTO wcoj_bench_semiring."E2" (id, start_id, end_id, properties)
SELECT _graphid(_label_id('wcoj_bench_semiring', 'E2'), edge_no),
       _graphid(_label_id('wcoj_bench_semiring', 'S'), 2),
       _graphid(_label_id('wcoj_bench_semiring', 'T'), 1),
       '{}'::agtype
FROM generate_series(1, :fanout) edge_no;

INSERT INTO wcoj_bench_semiring."E3" (id, start_id, end_id, properties)
SELECT _graphid(_label_id('wcoj_bench_semiring', 'E3'), edge_no),
       _graphid(_label_id('wcoj_bench_semiring', 'S'), 3),
       _graphid(_label_id('wcoj_bench_semiring', 'T'), 1),
       '{}'::agtype
FROM generate_series(1, :fanout) edge_no;

CREATE INDEX wcoj_bench_semiring_e1_out
ON wcoj_bench_semiring."E1"
USING age_adjacency(start_id, id, end_id);
CREATE INDEX wcoj_bench_semiring_e2_out
ON wcoj_bench_semiring."E2"
USING age_adjacency(start_id, id, end_id);
CREATE INDEX wcoj_bench_semiring_e3_out
ON wcoj_bench_semiring."E3"
USING age_adjacency(start_id, id, end_id);

ANALYZE wcoj_bench_semiring."S";
ANALYZE wcoj_bench_semiring."T";
ANALYZE wcoj_bench_semiring."E1";
ANALYZE wcoj_bench_semiring."E2";
ANALYZE wcoj_bench_semiring."E3";
CHECKPOINT;
