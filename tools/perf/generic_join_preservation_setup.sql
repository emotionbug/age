\set ON_ERROR_STOP on
\if :{?cycle_size}
\else
\set cycle_size 128
\endif

LOAD 'age';
SET search_path = ag_catalog, "$user", public;
SET client_min_messages = warning;

SELECT drop_graph('generic_join_preserve', true)
WHERE EXISTS (SELECT 1 FROM ag_graph WHERE name = 'generic_join_preserve');
SELECT create_graph('generic_join_preserve');
SELECT create_vlabel('generic_join_preserve', 'A');
SELECT create_vlabel('generic_join_preserve', 'B');
SELECT create_vlabel('generic_join_preserve', 'C');
SELECT create_vlabel('generic_join_preserve', 'D');
SELECT create_vlabel('generic_join_preserve', 'X');
SELECT create_vlabel('generic_join_preserve', 'Y');
SELECT create_elabel('generic_join_preserve', 'E1');
SELECT create_elabel('generic_join_preserve', 'E2');
SELECT create_elabel('generic_join_preserve', 'E3');
SELECT create_elabel('generic_join_preserve', 'E4');
SELECT create_elabel('generic_join_preserve', 'TX');
SELECT create_elabel('generic_join_preserve', 'TY');

INSERT INTO generic_join_preserve."A" (id, properties)
SELECT _graphid(_label_id('generic_join_preserve', 'A'), i), '{}'::agtype
FROM generate_series(1, :cycle_size) i;
INSERT INTO generic_join_preserve."B" (id, properties)
SELECT _graphid(_label_id('generic_join_preserve', 'B'), i), '{}'::agtype
FROM generate_series(1, :cycle_size) i;
INSERT INTO generic_join_preserve."C" (id, properties)
SELECT _graphid(_label_id('generic_join_preserve', 'C'), i), '{}'::agtype
FROM generate_series(1, :cycle_size) i;
INSERT INTO generic_join_preserve."D" (id, properties)
SELECT _graphid(_label_id('generic_join_preserve', 'D'), i), '{}'::agtype
FROM generate_series(1, :cycle_size) i;
INSERT INTO generic_join_preserve."X" (id, properties)
VALUES (_graphid(_label_id('generic_join_preserve', 'X'), 1), '{}'::agtype);
INSERT INTO generic_join_preserve."Y" (id, properties)
VALUES (_graphid(_label_id('generic_join_preserve', 'Y'), 1), '{}'::agtype);

INSERT INTO generic_join_preserve."E1" (id, start_id, end_id, properties)
SELECT _graphid(_label_id('generic_join_preserve', 'E1'), i),
       _graphid(_label_id('generic_join_preserve', 'A'), i),
       _graphid(_label_id('generic_join_preserve', 'B'), i),
       '{}'::agtype
FROM generate_series(1, :cycle_size) i;
INSERT INTO generic_join_preserve."E2" (id, start_id, end_id, properties)
SELECT _graphid(_label_id('generic_join_preserve', 'E2'), i),
       _graphid(_label_id('generic_join_preserve', 'B'), i),
       _graphid(_label_id('generic_join_preserve', 'C'), i),
       '{}'::agtype
FROM generate_series(1, :cycle_size) i;
INSERT INTO generic_join_preserve."E3" (id, start_id, end_id, properties)
SELECT _graphid(_label_id('generic_join_preserve', 'E3'), i),
       _graphid(_label_id('generic_join_preserve', 'C'), i),
       _graphid(_label_id('generic_join_preserve', 'D'), i),
       '{}'::agtype
FROM generate_series(1, :cycle_size) i;
INSERT INTO generic_join_preserve."E4" (id, start_id, end_id, properties)
SELECT _graphid(_label_id('generic_join_preserve', 'E4'), i),
       _graphid(_label_id('generic_join_preserve', 'D'), i),
       _graphid(_label_id('generic_join_preserve', 'A'), i),
       '{}'::agtype
FROM generate_series(1, :cycle_size) i;

INSERT INTO generic_join_preserve."TX" (id, start_id, end_id, properties)
VALUES (_graphid(_label_id('generic_join_preserve', 'TX'), 1),
        _graphid(_label_id('generic_join_preserve', 'C'), 1),
        _graphid(_label_id('generic_join_preserve', 'X'), 1),
        '{}'::agtype);
INSERT INTO generic_join_preserve."TY" (id, start_id, end_id, properties)
VALUES (_graphid(_label_id('generic_join_preserve', 'TY'), 1),
        _graphid(_label_id('generic_join_preserve', 'A'), 1),
        _graphid(_label_id('generic_join_preserve', 'Y'), 1),
        '{}'::agtype);

CREATE INDEX generic_join_preserve_e1_out
ON generic_join_preserve."E1" USING age_adjacency(start_id, id, end_id);
CREATE INDEX generic_join_preserve_e2_out
ON generic_join_preserve."E2" USING age_adjacency(start_id, id, end_id);
CREATE INDEX generic_join_preserve_e3_out
ON generic_join_preserve."E3" USING age_adjacency(start_id, id, end_id);
CREATE INDEX generic_join_preserve_e4_out
ON generic_join_preserve."E4" USING age_adjacency(start_id, id, end_id);
CREATE INDEX generic_join_preserve_tx_out
ON generic_join_preserve."TX" USING age_adjacency(start_id, id, end_id);
CREATE INDEX generic_join_preserve_ty_out
ON generic_join_preserve."TY" USING age_adjacency(start_id, id, end_id);

ANALYZE generic_join_preserve."A";
ANALYZE generic_join_preserve."B";
ANALYZE generic_join_preserve."C";
ANALYZE generic_join_preserve."D";
ANALYZE generic_join_preserve."X";
ANALYZE generic_join_preserve."Y";
ANALYZE generic_join_preserve."E1";
ANALYZE generic_join_preserve."E2";
ANALYZE generic_join_preserve."E3";
ANALYZE generic_join_preserve."E4";
ANALYZE generic_join_preserve."TX";
ANALYZE generic_join_preserve."TY";
CHECKPOINT;
