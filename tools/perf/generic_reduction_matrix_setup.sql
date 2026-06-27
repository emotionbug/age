\set ON_ERROR_STOP on
\if :{?fanout}
\else
\set fanout 1024
\endif

LOAD 'age';
SET search_path = ag_catalog, "$user", public;
SET client_min_messages = warning;

SELECT drop_graph('generic_reduction_matrix', true)
WHERE EXISTS (SELECT 1 FROM ag_graph WHERE name = 'generic_reduction_matrix');
SELECT create_graph('generic_reduction_matrix');
SELECT create_vlabel('generic_reduction_matrix', 'A');
SELECT create_vlabel('generic_reduction_matrix', 'B');
SELECT create_vlabel('generic_reduction_matrix', 'C');
SELECT create_vlabel('generic_reduction_matrix', 'D');
SELECT create_vlabel('generic_reduction_matrix', 'P');
SELECT create_elabel('generic_reduction_matrix', 'F1');
SELECT create_elabel('generic_reduction_matrix', 'F2');
SELECT create_elabel('generic_reduction_matrix', 'F3');
SELECT create_elabel('generic_reduction_matrix', 'H');

INSERT INTO generic_reduction_matrix."A" (id, properties)
SELECT _graphid(_label_id('generic_reduction_matrix', 'A'), i), '{}'::agtype
FROM generate_series(1, :fanout) i;
INSERT INTO generic_reduction_matrix."B" (id, properties)
VALUES (_graphid(_label_id('generic_reduction_matrix', 'B'), 1),
        '{}'::agtype);
INSERT INTO generic_reduction_matrix."C" (id, properties)
SELECT _graphid(_label_id('generic_reduction_matrix', 'C'), i), '{}'::agtype
FROM generate_series(1, 2 * :fanout - 1) i;
INSERT INTO generic_reduction_matrix."D" (id, properties)
VALUES (_graphid(_label_id('generic_reduction_matrix', 'D'), 1),
        '{}'::agtype);
INSERT INTO generic_reduction_matrix."P" (id, properties)
VALUES (_graphid(_label_id('generic_reduction_matrix', 'P'), 1),
        '{}'::agtype);

INSERT INTO generic_reduction_matrix."F1" (id, start_id, end_id, properties)
SELECT _graphid(_label_id('generic_reduction_matrix', 'F1'), i),
       _graphid(_label_id('generic_reduction_matrix', 'A'), i),
       _graphid(_label_id('generic_reduction_matrix', 'B'), 1),
       '{}'::agtype
FROM generate_series(1, :fanout) i;
INSERT INTO generic_reduction_matrix."F2" (id, start_id, end_id, properties)
SELECT _graphid(_label_id('generic_reduction_matrix', 'F2'), i),
       _graphid(_label_id('generic_reduction_matrix', 'B'), 1),
       _graphid(_label_id('generic_reduction_matrix', 'C'), i),
       '{}'::agtype
FROM generate_series(1, :fanout) i;
INSERT INTO generic_reduction_matrix."F3" (id, start_id, end_id, properties)
SELECT _graphid(_label_id('generic_reduction_matrix', 'F3'), i),
       _graphid(_label_id('generic_reduction_matrix', 'C'), :fanout - 1 + i),
       _graphid(_label_id('generic_reduction_matrix', 'D'), 1),
       '{}'::agtype
FROM generate_series(1, :fanout) i;
INSERT INTO generic_reduction_matrix."H" (id, start_id, end_id, properties)
VALUES (_graphid(_label_id('generic_reduction_matrix', 'H'), 1),
        _graphid(_label_id('generic_reduction_matrix', 'A'), 1),
        _graphid(_label_id('generic_reduction_matrix', 'P'), 1),
        '{}'::agtype);

CREATE INDEX generic_reduction_matrix_f1_out
ON generic_reduction_matrix."F1" USING age_adjacency(start_id, id, end_id);
CREATE INDEX generic_reduction_matrix_f2_out
ON generic_reduction_matrix."F2" USING age_adjacency(start_id, id, end_id);
CREATE INDEX generic_reduction_matrix_f3_out
ON generic_reduction_matrix."F3" USING age_adjacency(start_id, id, end_id);
CREATE INDEX generic_reduction_matrix_h_out
ON generic_reduction_matrix."H" USING age_adjacency(start_id, id, end_id);

ANALYZE generic_reduction_matrix."A";
ANALYZE generic_reduction_matrix."B";
ANALYZE generic_reduction_matrix."C";
ANALYZE generic_reduction_matrix."D";
ANALYZE generic_reduction_matrix."P";
ANALYZE generic_reduction_matrix."F1";
ANALYZE generic_reduction_matrix."F2";
ANALYZE generic_reduction_matrix."F3";
ANALYZE generic_reduction_matrix."H";
CHECKPOINT;
