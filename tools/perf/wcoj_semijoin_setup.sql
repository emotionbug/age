\set ON_ERROR_STOP on
\if :{?fanout}
\else
\set fanout 128
\endif

LOAD 'age';
SET search_path = ag_catalog, "$user", public;
SET client_min_messages = warning;

SELECT drop_graph('wcoj_bench_semijoin', true)
WHERE EXISTS (SELECT 1 FROM ag_graph WHERE name = 'wcoj_bench_semijoin');
SELECT create_graph('wcoj_bench_semijoin');
SELECT create_vlabel('wcoj_bench_semijoin', 'A');
SELECT create_vlabel('wcoj_bench_semijoin', 'B');
SELECT create_vlabel('wcoj_bench_semijoin', 'C');
SELECT create_vlabel('wcoj_bench_semijoin', 'D');
SELECT create_elabel('wcoj_bench_semijoin', 'F1');
SELECT create_elabel('wcoj_bench_semijoin', 'F2');
SELECT create_elabel('wcoj_bench_semijoin', 'F3');

INSERT INTO wcoj_bench_semijoin."A" (id, properties)
SELECT _graphid(_label_id('wcoj_bench_semijoin', 'A'), i), '{}'::agtype
FROM generate_series(1, :fanout) i;
INSERT INTO wcoj_bench_semijoin."B" (id, properties)
VALUES (_graphid(_label_id('wcoj_bench_semijoin', 'B'), 1), '{}'::agtype);
INSERT INTO wcoj_bench_semijoin."C" (id, properties)
SELECT _graphid(_label_id('wcoj_bench_semijoin', 'C'), i), '{}'::agtype
FROM generate_series(1, 2 * :fanout - 1) i;
INSERT INTO wcoj_bench_semijoin."D" (id, properties)
VALUES (_graphid(_label_id('wcoj_bench_semijoin', 'D'), 1), '{}'::agtype);

INSERT INTO wcoj_bench_semijoin."F1" (id, start_id, end_id, properties)
SELECT _graphid(_label_id('wcoj_bench_semijoin', 'F1'), i),
       _graphid(_label_id('wcoj_bench_semijoin', 'A'), i),
       _graphid(_label_id('wcoj_bench_semijoin', 'B'), 1),
       '{}'::agtype
FROM generate_series(1, :fanout) i;
INSERT INTO wcoj_bench_semijoin."F2" (id, start_id, end_id, properties)
SELECT _graphid(_label_id('wcoj_bench_semijoin', 'F2'), i),
       _graphid(_label_id('wcoj_bench_semijoin', 'B'), 1),
       _graphid(_label_id('wcoj_bench_semijoin', 'C'), i),
       '{}'::agtype
FROM generate_series(1, :fanout) i;
INSERT INTO wcoj_bench_semijoin."F3" (id, start_id, end_id, properties)
SELECT _graphid(_label_id('wcoj_bench_semijoin', 'F3'), i),
       _graphid(_label_id('wcoj_bench_semijoin', 'C'), :fanout - 1 + i),
       _graphid(_label_id('wcoj_bench_semijoin', 'D'), 1),
       '{}'::agtype
FROM generate_series(1, :fanout) i;

CREATE INDEX wcoj_bench_semijoin_f1_out
ON wcoj_bench_semijoin."F1"
USING age_adjacency(start_id, id, end_id);
CREATE INDEX wcoj_bench_semijoin_f2_out
ON wcoj_bench_semijoin."F2"
USING age_adjacency(start_id, id, end_id);
CREATE INDEX wcoj_bench_semijoin_f3_out
ON wcoj_bench_semijoin."F3"
USING age_adjacency(start_id, id, end_id);

ANALYZE wcoj_bench_semijoin."A";
ANALYZE wcoj_bench_semijoin."B";
ANALYZE wcoj_bench_semijoin."C";
ANALYZE wcoj_bench_semijoin."D";
ANALYZE wcoj_bench_semijoin."F1";
ANALYZE wcoj_bench_semijoin."F2";
ANALYZE wcoj_bench_semijoin."F3";
CHECKPOINT;
