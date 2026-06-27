/*
 * Phase E: cyclic bag + leaf-tail separator reduction for Generic Join.
 */
LOAD 'age';
SET search_path = ag_catalog, public;

SELECT create_graph('generic_ghd');
SELECT create_vlabel('generic_ghd', 'A');
SELECT create_vlabel('generic_ghd', 'B');
SELECT create_vlabel('generic_ghd', 'C');
SELECT create_vlabel('generic_ghd', 'D');
SELECT create_vlabel('generic_ghd', 'X');
SELECT create_vlabel('generic_ghd', 'GPA');
SELECT create_vlabel('generic_ghd', 'GPB');
SELECT create_vlabel('generic_ghd', 'GPC');
SELECT create_vlabel('generic_ghd', 'GPD');
SELECT create_elabel('generic_ghd', 'E1');
SELECT create_elabel('generic_ghd', 'E2');
SELECT create_elabel('generic_ghd', 'E3');
SELECT create_elabel('generic_ghd', 'E4');
SELECT create_elabel('generic_ghd', 'TX');
SELECT create_elabel('generic_ghd', 'GPE1');
SELECT create_elabel('generic_ghd', 'GPE2');
SELECT create_elabel('generic_ghd', 'GPE3');
SELECT create_elabel('generic_ghd', 'GPE4');

INSERT INTO generic_ghd."A" (id, properties)
SELECT _graphid(_label_id('generic_ghd', 'A'), i), '{}'::agtype
FROM generate_series(1, 64) i;
INSERT INTO generic_ghd."B" (id, properties)
SELECT _graphid(_label_id('generic_ghd', 'B'), i), '{}'::agtype
FROM generate_series(1, 64) i;
INSERT INTO generic_ghd."C" (id, properties)
SELECT _graphid(_label_id('generic_ghd', 'C'), i), '{}'::agtype
FROM generate_series(1, 64) i;
INSERT INTO generic_ghd."D" (id, properties)
SELECT _graphid(_label_id('generic_ghd', 'D'), i), '{}'::agtype
FROM generate_series(1, 64) i;
INSERT INTO generic_ghd."X" (id, properties)
VALUES (_graphid(_label_id('generic_ghd', 'X'), 1), '{}'::agtype);

INSERT INTO generic_ghd."E1" (id, start_id, end_id, properties)
SELECT _graphid(_label_id('generic_ghd', 'E1'), i),
       _graphid(_label_id('generic_ghd', 'A'), i),
       _graphid(_label_id('generic_ghd', 'B'), i),
       format('{"score":%s}', i)::agtype
FROM generate_series(1, 64) i;
INSERT INTO generic_ghd."E2" (id, start_id, end_id, properties)
SELECT _graphid(_label_id('generic_ghd', 'E2'), i),
       _graphid(_label_id('generic_ghd', 'B'), i),
       _graphid(_label_id('generic_ghd', 'C'), i),
       '{}'::agtype
FROM generate_series(1, 64) i;
INSERT INTO generic_ghd."E3" (id, start_id, end_id, properties)
SELECT _graphid(_label_id('generic_ghd', 'E3'), i),
       _graphid(_label_id('generic_ghd', 'C'), i),
       _graphid(_label_id('generic_ghd', 'D'), i),
       '{}'::agtype
FROM generate_series(1, 64) i;
INSERT INTO generic_ghd."E4" (id, start_id, end_id, properties)
SELECT _graphid(_label_id('generic_ghd', 'E4'), i),
       _graphid(_label_id('generic_ghd', 'D'), i),
       _graphid(_label_id('generic_ghd', 'A'), i),
       '{}'::agtype
FROM generate_series(1, 64) i;
INSERT INTO generic_ghd."TX" (id, start_id, end_id, properties)
VALUES (_graphid(_label_id('generic_ghd', 'TX'), 1),
        _graphid(_label_id('generic_ghd', 'C'), 1),
        _graphid(_label_id('generic_ghd', 'X'), 1),
        '{}'::agtype);

INSERT INTO generic_ghd."GPA" (id, properties)
SELECT _graphid(_label_id('generic_ghd', 'GPA'), i), '{}'::agtype
FROM generate_series(1, 4) i;
INSERT INTO generic_ghd."GPB" (id, properties)
SELECT _graphid(_label_id('generic_ghd', 'GPB'), i), '{}'::agtype
FROM generate_series(1, 4) i;
INSERT INTO generic_ghd."GPC" (id, properties)
SELECT _graphid(_label_id('generic_ghd', 'GPC'), i), '{}'::agtype
FROM generate_series(1, 4) i;
INSERT INTO generic_ghd."GPD" (id, properties)
SELECT _graphid(_label_id('generic_ghd', 'GPD'), i), '{}'::agtype
FROM generate_series(1, 4) i;

INSERT INTO generic_ghd."GPE1" (id, start_id, end_id, properties)
SELECT _graphid(_label_id('generic_ghd', 'GPE1'), (i - 1) * 3 + j),
       _graphid(_label_id('generic_ghd', 'GPA'), i),
       _graphid(_label_id('generic_ghd', 'GPB'), i),
       format('{"score":%s}', 10 * i + j)::agtype
FROM generate_series(1, 4) i,
     generate_series(1, 3) j;
INSERT INTO generic_ghd."GPE2" (id, start_id, end_id, properties)
SELECT _graphid(_label_id('generic_ghd', 'GPE2'), i),
       _graphid(_label_id('generic_ghd', 'GPB'), i),
       _graphid(_label_id('generic_ghd', 'GPC'), i),
       '{}'::agtype
FROM generate_series(1, 4) i;
INSERT INTO generic_ghd."GPE3" (id, start_id, end_id, properties)
SELECT _graphid(_label_id('generic_ghd', 'GPE3'), i),
       _graphid(_label_id('generic_ghd', 'GPC'), i),
       _graphid(_label_id('generic_ghd', 'GPD'), i),
       '{}'::agtype
FROM generate_series(1, 4) i;
INSERT INTO generic_ghd."GPE4" (id, start_id, end_id, properties)
SELECT _graphid(_label_id('generic_ghd', 'GPE4'), i),
       _graphid(_label_id('generic_ghd', 'GPD'), i),
       _graphid(_label_id('generic_ghd', 'GPA'), i),
       '{}'::agtype
FROM generate_series(1, 4) i;

CREATE INDEX generic_ghd_e1_out
ON generic_ghd."E1" USING age_adjacency(start_id, id, end_id);
CREATE INDEX generic_ghd_e2_out
ON generic_ghd."E2" USING age_adjacency(start_id, id, end_id);
CREATE INDEX generic_ghd_e3_out
ON generic_ghd."E3" USING age_adjacency(start_id, id, end_id);
CREATE INDEX generic_ghd_e4_out
ON generic_ghd."E4" USING age_adjacency(start_id, id, end_id);
CREATE INDEX generic_ghd_tx_out
ON generic_ghd."TX" USING age_adjacency(start_id, id, end_id);
CREATE INDEX generic_ghd_gpe1_out
ON generic_ghd."GPE1" USING age_adjacency(start_id, id, end_id);
CREATE INDEX generic_ghd_gpe2_out
ON generic_ghd."GPE2" USING age_adjacency(start_id, id, end_id);
CREATE INDEX generic_ghd_gpe3_out
ON generic_ghd."GPE3" USING age_adjacency(start_id, id, end_id);
CREATE INDEX generic_ghd_gpe4_out
ON generic_ghd."GPE4" USING age_adjacency(start_id, id, end_id);

ANALYZE generic_ghd."A";
ANALYZE generic_ghd."B";
ANALYZE generic_ghd."C";
ANALYZE generic_ghd."D";
ANALYZE generic_ghd."X";
ANALYZE generic_ghd."GPA";
ANALYZE generic_ghd."GPB";
ANALYZE generic_ghd."GPC";
ANALYZE generic_ghd."GPD";
ANALYZE generic_ghd."E1";
ANALYZE generic_ghd."E2";
ANALYZE generic_ghd."E3";
ANALYZE generic_ghd."E4";
ANALYZE generic_ghd."TX";
ANALYZE generic_ghd."GPE1";
ANALYZE generic_ghd."GPE2";
ANALYZE generic_ghd."GPE3";
ANALYZE generic_ghd."GPE4";

BEGIN;
SET LOCAL age.enable_wcoj = on;
SET LOCAL enable_nestloop = off;
SET LOCAL enable_hashjoin = off;
SET LOCAL enable_mergejoin = off;
EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF)
SELECT *
FROM cypher('generic_ghd', $$
    MATCH (a:A)-[e1:E1]->(b:B)-[e2:E2]->(c:C)
          -[e3:E3]->(d:D)-[e4:E4]->(a),
          (c)-[t:TX]->(x:X)
    RETURN id(a), id(b), id(c), id(d), id(x),
           id(e1), id(e2), id(e3), id(e4), id(t)
$$) AS (a agtype, b agtype, c agtype, d agtype, x agtype,
        e1 agtype, e2 agtype, e3 agtype, e4 agtype, t agtype);
ROLLBACK;

BEGIN;
SET LOCAL age.enable_wcoj = on;
SET LOCAL enable_nestloop = off;
SET LOCAL enable_hashjoin = off;
SET LOCAL enable_mergejoin = off;
EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF)
SELECT *
FROM cypher('generic_ghd', $$
    MATCH (a:A)-[e1:E1]->(b:B)-[e2:E2]->(c:C)
          -[e3:E3]->(d:D)-[e4:E4]->(a),
          (c)-[t:TX]->(x:X)
    RETURN count(*) AS total
$$) AS (total agtype);
ROLLBACK;

BEGIN;
SET LOCAL age.enable_wcoj = on;
SET LOCAL enable_nestloop = off;
SET LOCAL enable_hashjoin = off;
SET LOCAL enable_mergejoin = off;
EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF)
SELECT *
FROM cypher('generic_ghd', $$
    MATCH (a:A)-[:E1]->(b:B),
          (b)-[:E2]->(c:C),
          (c)-[:E3]->(d:D),
          (d)-[:E4]->(a)
    RETURN count(*) AS total
$$) AS (total agtype);
ROLLBACK;

-- Same-label parallel edge bags force bounded exact enumeration under
-- explicit edge uniqueness while preserving the Generic Join count shape.
BEGIN;
SET LOCAL age.enable_wcoj = on;
SET LOCAL enable_nestloop = off;
SET LOCAL enable_hashjoin = off;
SET LOCAL enable_mergejoin = off;
EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF)
SELECT *
FROM cypher('generic_ghd', $$
    MATCH (a:GPA)-[e1:GPE1]->(b:GPB),
          (a)-[e2:GPE1]->(b),
          (b)-[:GPE2]->(c:GPC),
          (c)-[:GPE3]->(d:GPD),
          (d)-[:GPE4]->(a)
    WHERE _ag_enforce_edge_uniqueness2(id(e1), id(e2))
    RETURN count(*) AS total
$$) AS (total agtype);
ROLLBACK;

BEGIN;
SET LOCAL age.enable_wcoj = on;
SET LOCAL enable_nestloop = off;
SET LOCAL enable_hashjoin = off;
SET LOCAL enable_mergejoin = off;
SET LOCAL enable_hashagg = off;
EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF)
SELECT *
FROM cypher('generic_ghd', $$
    MATCH (a:GPA)-[e1:GPE1]->(b:GPB),
          (a)-[e2:GPE1]->(b),
          (b)-[:GPE2]->(c:GPC),
          (c)-[:GPE3]->(d:GPD),
          (d)-[:GPE4]->(a)
    WHERE _ag_enforce_edge_uniqueness2(id(e1), id(e2))
    RETURN id(a) AS key, count(*) AS total
$$) AS (key agtype, total agtype);
ROLLBACK;

BEGIN;
SET LOCAL age.enable_wcoj = on;
SET LOCAL enable_nestloop = off;
SET LOCAL enable_hashjoin = off;
SET LOCAL enable_mergejoin = off;
EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF)
SELECT *
FROM cypher('generic_ghd', $$
    MATCH (a:GPA)-[e1:GPE1]->(b:GPB),
          (a)-[e2:GPE1]->(b),
          (b)-[:GPE2]->(c:GPC),
          (c)-[:GPE3]->(d:GPD),
          (d)-[:GPE4]->(a)
    WHERE _ag_enforce_edge_uniqueness2(id(e1), id(e2))
    RETURN sum(e1.score) AS total
$$) AS (total agtype);
ROLLBACK;

BEGIN;
SET LOCAL age.enable_wcoj = on;
SET LOCAL enable_nestloop = off;
SET LOCAL enable_hashjoin = off;
SET LOCAL enable_mergejoin = off;
SET LOCAL enable_hashagg = off;
EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF)
SELECT *
FROM cypher('generic_ghd', $$
    MATCH (a:GPA)-[e1:GPE1]->(b:GPB),
          (a)-[e2:GPE1]->(b),
          (b)-[:GPE2]->(c:GPC),
          (c)-[:GPE3]->(d:GPD),
          (d)-[:GPE4]->(a)
    WHERE _ag_enforce_edge_uniqueness2(id(e1), id(e2))
    RETURN id(a) AS key, sum(e1.score) AS total
$$) AS (key agtype, total agtype);
ROLLBACK;

BEGIN;
SET LOCAL age.enable_wcoj = off;
CREATE TEMP TABLE generic_ghd_binary_small_enum_count ON COMMIT DROP AS
SELECT *
FROM cypher('generic_ghd', $$
    MATCH (a:GPA)-[e1:GPE1]->(b:GPB),
          (a)-[e2:GPE1]->(b),
          (b)-[:GPE2]->(c:GPC),
          (c)-[:GPE3]->(d:GPD),
          (d)-[:GPE4]->(a)
    WHERE _ag_enforce_edge_uniqueness2(id(e1), id(e2))
    RETURN count(*) AS total
$$) AS (total agtype);
CREATE TEMP TABLE generic_ghd_binary_small_enum_group_count ON COMMIT DROP AS
SELECT *
FROM cypher('generic_ghd', $$
    MATCH (a:GPA)-[e1:GPE1]->(b:GPB),
          (a)-[e2:GPE1]->(b),
          (b)-[:GPE2]->(c:GPC),
          (c)-[:GPE3]->(d:GPD),
          (d)-[:GPE4]->(a)
    WHERE _ag_enforce_edge_uniqueness2(id(e1), id(e2))
    RETURN id(a) AS key, count(*) AS total
$$) AS (key agtype, total agtype);
CREATE TEMP TABLE generic_ghd_binary_small_enum_sum ON COMMIT DROP AS
SELECT *
FROM cypher('generic_ghd', $$
    MATCH (a:GPA)-[e1:GPE1]->(b:GPB),
          (a)-[e2:GPE1]->(b),
          (b)-[:GPE2]->(c:GPC),
          (c)-[:GPE3]->(d:GPD),
          (d)-[:GPE4]->(a)
    WHERE _ag_enforce_edge_uniqueness2(id(e1), id(e2))
    RETURN sum(e1.score) AS total
$$) AS (total agtype);
CREATE TEMP TABLE generic_ghd_binary_small_enum_group_sum ON COMMIT DROP AS
SELECT *
FROM cypher('generic_ghd', $$
    MATCH (a:GPA)-[e1:GPE1]->(b:GPB),
          (a)-[e2:GPE1]->(b),
          (b)-[:GPE2]->(c:GPC),
          (c)-[:GPE3]->(d:GPD),
          (d)-[:GPE4]->(a)
    WHERE _ag_enforce_edge_uniqueness2(id(e1), id(e2))
    RETURN id(a) AS key, sum(e1.score) AS total
$$) AS (key agtype, total agtype);

SET LOCAL age.enable_wcoj = on;
SET LOCAL enable_nestloop = off;
SET LOCAL enable_hashjoin = off;
SET LOCAL enable_mergejoin = off;
SET LOCAL enable_hashagg = off;
CREATE TEMP TABLE generic_ghd_join_small_enum_count ON COMMIT DROP AS
SELECT *
FROM cypher('generic_ghd', $$
    MATCH (a:GPA)-[e1:GPE1]->(b:GPB),
          (a)-[e2:GPE1]->(b),
          (b)-[:GPE2]->(c:GPC),
          (c)-[:GPE3]->(d:GPD),
          (d)-[:GPE4]->(a)
    WHERE _ag_enforce_edge_uniqueness2(id(e1), id(e2))
    RETURN count(*) AS total
$$) AS (total agtype);
CREATE TEMP TABLE generic_ghd_join_small_enum_group_count ON COMMIT DROP AS
SELECT *
FROM cypher('generic_ghd', $$
    MATCH (a:GPA)-[e1:GPE1]->(b:GPB),
          (a)-[e2:GPE1]->(b),
          (b)-[:GPE2]->(c:GPC),
          (c)-[:GPE3]->(d:GPD),
          (d)-[:GPE4]->(a)
    WHERE _ag_enforce_edge_uniqueness2(id(e1), id(e2))
    RETURN id(a) AS key, count(*) AS total
$$) AS (key agtype, total agtype);
CREATE TEMP TABLE generic_ghd_join_small_enum_sum ON COMMIT DROP AS
SELECT *
FROM cypher('generic_ghd', $$
    MATCH (a:GPA)-[e1:GPE1]->(b:GPB),
          (a)-[e2:GPE1]->(b),
          (b)-[:GPE2]->(c:GPC),
          (c)-[:GPE3]->(d:GPD),
          (d)-[:GPE4]->(a)
    WHERE _ag_enforce_edge_uniqueness2(id(e1), id(e2))
    RETURN sum(e1.score) AS total
$$) AS (total agtype);
CREATE TEMP TABLE generic_ghd_join_small_enum_group_sum ON COMMIT DROP AS
SELECT *
FROM cypher('generic_ghd', $$
    MATCH (a:GPA)-[e1:GPE1]->(b:GPB),
          (a)-[e2:GPE1]->(b),
          (b)-[:GPE2]->(c:GPC),
          (c)-[:GPE3]->(d:GPD),
          (d)-[:GPE4]->(a)
    WHERE _ag_enforce_edge_uniqueness2(id(e1), id(e2))
    RETURN id(a) AS key, sum(e1.score) AS total
$$) AS (key agtype, total agtype);

SELECT (SELECT total FROM generic_ghd_binary_small_enum_count) AS binary_count,
       (SELECT total FROM generic_ghd_join_small_enum_count) AS generic_count,
       (SELECT count(*) FROM (
            (SELECT * FROM generic_ghd_binary_small_enum_count
             EXCEPT ALL
             SELECT * FROM generic_ghd_join_small_enum_count)
            UNION ALL
            (SELECT * FROM generic_ghd_join_small_enum_count
             EXCEPT ALL
             SELECT * FROM generic_ghd_binary_small_enum_count)
        ) diff) AS count_diff_rows,
       (SELECT count(*) FROM generic_ghd_binary_small_enum_group_count) AS binary_group_count_rows,
       (SELECT count(*) FROM generic_ghd_join_small_enum_group_count) AS generic_group_count_rows,
       (SELECT count(*) FROM (
            (SELECT * FROM generic_ghd_binary_small_enum_group_count
             EXCEPT ALL
             SELECT * FROM generic_ghd_join_small_enum_group_count)
            UNION ALL
            (SELECT * FROM generic_ghd_join_small_enum_group_count
             EXCEPT ALL
             SELECT * FROM generic_ghd_binary_small_enum_group_count)
        ) diff) AS group_count_diff_rows,
       (SELECT total FROM generic_ghd_binary_small_enum_sum) AS binary_sum,
       (SELECT total FROM generic_ghd_join_small_enum_sum) AS generic_sum,
       (SELECT count(*) FROM (
            (SELECT * FROM generic_ghd_binary_small_enum_sum
             EXCEPT ALL
             SELECT * FROM generic_ghd_join_small_enum_sum)
            UNION ALL
            (SELECT * FROM generic_ghd_join_small_enum_sum
             EXCEPT ALL
             SELECT * FROM generic_ghd_binary_small_enum_sum)
        ) diff) AS sum_diff_rows,
       (SELECT count(*) FROM generic_ghd_binary_small_enum_group_sum) AS binary_group_sum_rows,
       (SELECT count(*) FROM generic_ghd_join_small_enum_group_sum) AS generic_group_sum_rows,
       (SELECT count(*) FROM (
            (SELECT * FROM generic_ghd_binary_small_enum_group_sum
             EXCEPT ALL
             SELECT * FROM generic_ghd_join_small_enum_group_sum)
            UNION ALL
            (SELECT * FROM generic_ghd_join_small_enum_group_sum
             EXCEPT ALL
             SELECT * FROM generic_ghd_binary_small_enum_group_sum)
        ) diff) AS group_sum_diff_rows;
ROLLBACK;

BEGIN;
SET LOCAL age.enable_wcoj = on;
SET LOCAL enable_nestloop = off;
SET LOCAL enable_hashjoin = off;
SET LOCAL enable_mergejoin = off;
EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF)
SELECT *
FROM cypher('generic_ghd', $$
    MATCH (a:A)-[e1:E1]->(b:B),
          (b)-[:E2]->(c:C),
          (c)-[:E3]->(d:D),
          (d)-[:E4]->(a)
    RETURN sum(e1.score) AS total
$$) AS (total agtype);
ROLLBACK;

BEGIN;
SET LOCAL age.enable_wcoj = on;
SET LOCAL enable_nestloop = off;
SET LOCAL enable_hashjoin = off;
SET LOCAL enable_mergejoin = off;
EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF)
SELECT *
FROM cypher('generic_ghd', $$
    MATCH (a:A)-[e1:E1]->(b:B),
          (b)-[:E2]->(c:C),
          (c)-[:E3]->(d:D),
          (d)-[:E4]->(a)
    RETURN avg(e1.score) AS value
$$) AS (value agtype);
ROLLBACK;

BEGIN;
SET LOCAL age.enable_wcoj = on;
SET LOCAL enable_nestloop = off;
SET LOCAL enable_hashjoin = off;
SET LOCAL enable_mergejoin = off;
EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF)
SELECT *
FROM cypher('generic_ghd', $$
    MATCH (a:A)-[e1:E1]->(b:B),
          (b)-[:E2]->(c:C),
          (c)-[:E3]->(d:D),
          (d)-[:E4]->(a)
    RETURN min(e1.score) AS value
$$) AS (value agtype);
ROLLBACK;

BEGIN;
SET LOCAL age.enable_wcoj = on;
SET LOCAL enable_nestloop = off;
SET LOCAL enable_hashjoin = off;
SET LOCAL enable_mergejoin = off;
EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF)
SELECT *
FROM cypher('generic_ghd', $$
    MATCH (a:A)-[e1:E1]->(b:B),
          (b)-[:E2]->(c:C),
          (c)-[:E3]->(d:D),
          (d)-[:E4]->(a)
    RETURN max(e1.score) AS value
$$) AS (value agtype);
ROLLBACK;

BEGIN;
SET LOCAL age.enable_wcoj = off;
CREATE TEMP TABLE generic_ghd_binary_sum_property ON COMMIT DROP AS
SELECT *
FROM cypher('generic_ghd', $$
    MATCH (a:A)-[e1:E1]->(b:B),
          (b)-[:E2]->(c:C),
          (c)-[:E3]->(d:D),
          (d)-[:E4]->(a)
    RETURN sum(e1.score) AS total
$$) AS (total agtype);

SET LOCAL age.enable_wcoj = on;
SET LOCAL enable_nestloop = off;
SET LOCAL enable_hashjoin = off;
SET LOCAL enable_mergejoin = off;
CREATE TEMP TABLE generic_ghd_join_sum_property ON COMMIT DROP AS
SELECT *
FROM cypher('generic_ghd', $$
    MATCH (a:A)-[e1:E1]->(b:B),
          (b)-[:E2]->(c:C),
          (c)-[:E3]->(d:D),
          (d)-[:E4]->(a)
    RETURN sum(e1.score) AS total
$$) AS (total agtype);

SELECT (SELECT total FROM generic_ghd_binary_sum_property) AS binary_total,
       (SELECT total FROM generic_ghd_join_sum_property) AS generic_total,
       (SELECT count(*) FROM (
            (SELECT * FROM generic_ghd_binary_sum_property
             EXCEPT ALL
             SELECT * FROM generic_ghd_join_sum_property)
            UNION ALL
            (SELECT * FROM generic_ghd_join_sum_property
             EXCEPT ALL
             SELECT * FROM generic_ghd_binary_sum_property)
        ) diff) AS diff_rows;
ROLLBACK;

BEGIN;
SET LOCAL age.enable_wcoj = off;
CREATE TEMP TABLE generic_ghd_binary_avg_property ON COMMIT DROP AS
SELECT *
FROM cypher('generic_ghd', $$
    MATCH (a:A)-[e1:E1]->(b:B),
          (b)-[:E2]->(c:C),
          (c)-[:E3]->(d:D),
          (d)-[:E4]->(a)
    RETURN avg(e1.score) AS value
$$) AS (value agtype);
CREATE TEMP TABLE generic_ghd_binary_min_property ON COMMIT DROP AS
SELECT *
FROM cypher('generic_ghd', $$
    MATCH (a:A)-[e1:E1]->(b:B),
          (b)-[:E2]->(c:C),
          (c)-[:E3]->(d:D),
          (d)-[:E4]->(a)
    RETURN min(e1.score) AS value
$$) AS (value agtype);
CREATE TEMP TABLE generic_ghd_binary_max_property ON COMMIT DROP AS
SELECT *
FROM cypher('generic_ghd', $$
    MATCH (a:A)-[e1:E1]->(b:B),
          (b)-[:E2]->(c:C),
          (c)-[:E3]->(d:D),
          (d)-[:E4]->(a)
    RETURN max(e1.score) AS value
$$) AS (value agtype);

SET LOCAL age.enable_wcoj = on;
SET LOCAL enable_nestloop = off;
SET LOCAL enable_hashjoin = off;
SET LOCAL enable_mergejoin = off;
CREATE TEMP TABLE generic_ghd_join_avg_property ON COMMIT DROP AS
SELECT *
FROM cypher('generic_ghd', $$
    MATCH (a:A)-[e1:E1]->(b:B),
          (b)-[:E2]->(c:C),
          (c)-[:E3]->(d:D),
          (d)-[:E4]->(a)
    RETURN avg(e1.score) AS value
$$) AS (value agtype);
CREATE TEMP TABLE generic_ghd_join_min_property ON COMMIT DROP AS
SELECT *
FROM cypher('generic_ghd', $$
    MATCH (a:A)-[e1:E1]->(b:B),
          (b)-[:E2]->(c:C),
          (c)-[:E3]->(d:D),
          (d)-[:E4]->(a)
    RETURN min(e1.score) AS value
$$) AS (value agtype);
CREATE TEMP TABLE generic_ghd_join_max_property ON COMMIT DROP AS
SELECT *
FROM cypher('generic_ghd', $$
    MATCH (a:A)-[e1:E1]->(b:B),
          (b)-[:E2]->(c:C),
          (c)-[:E3]->(d:D),
          (d)-[:E4]->(a)
    RETURN max(e1.score) AS value
$$) AS (value agtype);

SELECT (SELECT value FROM generic_ghd_binary_avg_property) AS binary_avg,
       (SELECT value FROM generic_ghd_join_avg_property) AS generic_avg,
       (SELECT count(*) FROM (
            (SELECT * FROM generic_ghd_binary_avg_property
             EXCEPT ALL
             SELECT * FROM generic_ghd_join_avg_property)
            UNION ALL
            (SELECT * FROM generic_ghd_join_avg_property
             EXCEPT ALL
             SELECT * FROM generic_ghd_binary_avg_property)
        ) diff) AS avg_diff_rows,
       (SELECT value FROM generic_ghd_binary_min_property) AS binary_min,
       (SELECT value FROM generic_ghd_join_min_property) AS generic_min,
       (SELECT count(*) FROM (
            (SELECT * FROM generic_ghd_binary_min_property
             EXCEPT ALL
             SELECT * FROM generic_ghd_join_min_property)
            UNION ALL
            (SELECT * FROM generic_ghd_join_min_property
             EXCEPT ALL
             SELECT * FROM generic_ghd_binary_min_property)
        ) diff) AS min_diff_rows,
       (SELECT value FROM generic_ghd_binary_max_property) AS binary_max,
       (SELECT value FROM generic_ghd_join_max_property) AS generic_max,
       (SELECT count(*) FROM (
            (SELECT * FROM generic_ghd_binary_max_property
             EXCEPT ALL
             SELECT * FROM generic_ghd_join_max_property)
            UNION ALL
            (SELECT * FROM generic_ghd_join_max_property
             EXCEPT ALL
             SELECT * FROM generic_ghd_binary_max_property)
        ) diff) AS max_diff_rows;
ROLLBACK;

BEGIN;
SET LOCAL age.enable_wcoj = on;
SET LOCAL enable_nestloop = off;
SET LOCAL enable_hashjoin = off;
SET LOCAL enable_mergejoin = off;
SET LOCAL enable_hashagg = off;
EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF)
SELECT *
FROM cypher('generic_ghd', $$
    MATCH (a:GPA)-[e1:GPE1]->(b:GPB),
          (b)-[:GPE2]->(c:GPC),
          (c)-[:GPE3]->(d:GPD),
          (d)-[:GPE4]->(a)
    RETURN id(a) AS key, sum(e1.score) AS total
$$) AS (key agtype, total agtype);
ROLLBACK;

BEGIN;
SET LOCAL age.enable_wcoj = on;
SET LOCAL enable_nestloop = off;
SET LOCAL enable_hashjoin = off;
SET LOCAL enable_mergejoin = off;
SET LOCAL enable_hashagg = off;
EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF)
SELECT *
FROM cypher('generic_ghd', $$
    MATCH (a:GPA)-[e1:GPE1]->(b:GPB),
          (b)-[:GPE2]->(c:GPC),
          (c)-[:GPE3]->(d:GPD),
          (d)-[:GPE4]->(a)
    RETURN id(a) AS key, avg(e1.score) AS value
$$) AS (key agtype, value agtype);
ROLLBACK;

BEGIN;
SET LOCAL age.enable_wcoj = on;
SET LOCAL enable_nestloop = off;
SET LOCAL enable_hashjoin = off;
SET LOCAL enable_mergejoin = off;
SET LOCAL enable_hashagg = off;
EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF)
SELECT *
FROM cypher('generic_ghd', $$
    MATCH (a:GPA)-[e1:GPE1]->(b:GPB),
          (b)-[:GPE2]->(c:GPC),
          (c)-[:GPE3]->(d:GPD),
          (d)-[:GPE4]->(a)
    RETURN id(a) AS key, min(e1.score) AS value
$$) AS (key agtype, value agtype);
ROLLBACK;

BEGIN;
SET LOCAL age.enable_wcoj = on;
SET LOCAL enable_nestloop = off;
SET LOCAL enable_hashjoin = off;
SET LOCAL enable_mergejoin = off;
SET LOCAL enable_hashagg = off;
EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF)
SELECT *
FROM cypher('generic_ghd', $$
    MATCH (a:GPA)-[e1:GPE1]->(b:GPB),
          (b)-[:GPE2]->(c:GPC),
          (c)-[:GPE3]->(d:GPD),
          (d)-[:GPE4]->(a)
    RETURN id(a) AS key, max(e1.score) AS value
$$) AS (key agtype, value agtype);
ROLLBACK;

BEGIN;
SET LOCAL age.enable_wcoj = off;
CREATE TEMP TABLE generic_ghd_binary_group_sum_property ON COMMIT DROP AS
SELECT *
FROM cypher('generic_ghd', $$
    MATCH (a:GPA)-[e1:GPE1]->(b:GPB),
          (b)-[:GPE2]->(c:GPC),
          (c)-[:GPE3]->(d:GPD),
          (d)-[:GPE4]->(a)
    RETURN id(a) AS key, sum(e1.score) AS total
$$) AS (key agtype, total agtype);
CREATE TEMP TABLE generic_ghd_binary_group_avg_property ON COMMIT DROP AS
SELECT *
FROM cypher('generic_ghd', $$
    MATCH (a:GPA)-[e1:GPE1]->(b:GPB),
          (b)-[:GPE2]->(c:GPC),
          (c)-[:GPE3]->(d:GPD),
          (d)-[:GPE4]->(a)
    RETURN id(a) AS key, avg(e1.score) AS value
$$) AS (key agtype, value agtype);
CREATE TEMP TABLE generic_ghd_binary_group_min_property ON COMMIT DROP AS
SELECT *
FROM cypher('generic_ghd', $$
    MATCH (a:GPA)-[e1:GPE1]->(b:GPB),
          (b)-[:GPE2]->(c:GPC),
          (c)-[:GPE3]->(d:GPD),
          (d)-[:GPE4]->(a)
    RETURN id(a) AS key, min(e1.score) AS value
$$) AS (key agtype, value agtype);
CREATE TEMP TABLE generic_ghd_binary_group_max_property ON COMMIT DROP AS
SELECT *
FROM cypher('generic_ghd', $$
    MATCH (a:GPA)-[e1:GPE1]->(b:GPB),
          (b)-[:GPE2]->(c:GPC),
          (c)-[:GPE3]->(d:GPD),
          (d)-[:GPE4]->(a)
    RETURN id(a) AS key, max(e1.score) AS value
$$) AS (key agtype, value agtype);

SET LOCAL age.enable_wcoj = on;
SET LOCAL enable_nestloop = off;
SET LOCAL enable_hashjoin = off;
SET LOCAL enable_mergejoin = off;
SET LOCAL enable_hashagg = off;
CREATE TEMP TABLE generic_ghd_join_group_sum_property ON COMMIT DROP AS
SELECT *
FROM cypher('generic_ghd', $$
    MATCH (a:GPA)-[e1:GPE1]->(b:GPB),
          (b)-[:GPE2]->(c:GPC),
          (c)-[:GPE3]->(d:GPD),
          (d)-[:GPE4]->(a)
    RETURN id(a) AS key, sum(e1.score) AS total
$$) AS (key agtype, total agtype);
CREATE TEMP TABLE generic_ghd_join_group_avg_property ON COMMIT DROP AS
SELECT *
FROM cypher('generic_ghd', $$
    MATCH (a:GPA)-[e1:GPE1]->(b:GPB),
          (b)-[:GPE2]->(c:GPC),
          (c)-[:GPE3]->(d:GPD),
          (d)-[:GPE4]->(a)
    RETURN id(a) AS key, avg(e1.score) AS value
$$) AS (key agtype, value agtype);
CREATE TEMP TABLE generic_ghd_join_group_min_property ON COMMIT DROP AS
SELECT *
FROM cypher('generic_ghd', $$
    MATCH (a:GPA)-[e1:GPE1]->(b:GPB),
          (b)-[:GPE2]->(c:GPC),
          (c)-[:GPE3]->(d:GPD),
          (d)-[:GPE4]->(a)
    RETURN id(a) AS key, min(e1.score) AS value
$$) AS (key agtype, value agtype);
CREATE TEMP TABLE generic_ghd_join_group_max_property ON COMMIT DROP AS
SELECT *
FROM cypher('generic_ghd', $$
    MATCH (a:GPA)-[e1:GPE1]->(b:GPB),
          (b)-[:GPE2]->(c:GPC),
          (c)-[:GPE3]->(d:GPD),
          (d)-[:GPE4]->(a)
    RETURN id(a) AS key, max(e1.score) AS value
$$) AS (key agtype, value agtype);

SELECT (SELECT count(*) FROM generic_ghd_binary_group_sum_property) AS binary_sum_rows,
       (SELECT count(*) FROM generic_ghd_join_group_sum_property) AS generic_sum_rows,
       (SELECT count(*) FROM (
            (SELECT * FROM generic_ghd_binary_group_sum_property
             EXCEPT ALL
             SELECT * FROM generic_ghd_join_group_sum_property)
            UNION ALL
            (SELECT * FROM generic_ghd_join_group_sum_property
             EXCEPT ALL
             SELECT * FROM generic_ghd_binary_group_sum_property)
        ) diff) AS sum_diff_rows,
       (SELECT count(*) FROM generic_ghd_binary_group_avg_property) AS binary_avg_rows,
       (SELECT count(*) FROM generic_ghd_join_group_avg_property) AS generic_avg_rows,
       (SELECT count(*) FROM (
            (SELECT * FROM generic_ghd_binary_group_avg_property
             EXCEPT ALL
             SELECT * FROM generic_ghd_join_group_avg_property)
            UNION ALL
            (SELECT * FROM generic_ghd_join_group_avg_property
             EXCEPT ALL
             SELECT * FROM generic_ghd_binary_group_avg_property)
        ) diff) AS avg_diff_rows,
       (SELECT count(*) FROM generic_ghd_binary_group_min_property) AS binary_min_rows,
       (SELECT count(*) FROM generic_ghd_join_group_min_property) AS generic_min_rows,
       (SELECT count(*) FROM (
            (SELECT * FROM generic_ghd_binary_group_min_property
             EXCEPT ALL
             SELECT * FROM generic_ghd_join_group_min_property)
            UNION ALL
            (SELECT * FROM generic_ghd_join_group_min_property
             EXCEPT ALL
             SELECT * FROM generic_ghd_binary_group_min_property)
        ) diff) AS min_diff_rows,
       (SELECT count(*) FROM generic_ghd_binary_group_max_property) AS binary_max_rows,
       (SELECT count(*) FROM generic_ghd_join_group_max_property) AS generic_max_rows,
       (SELECT count(*) FROM (
            (SELECT * FROM generic_ghd_binary_group_max_property
             EXCEPT ALL
             SELECT * FROM generic_ghd_join_group_max_property)
            UNION ALL
            (SELECT * FROM generic_ghd_join_group_max_property
             EXCEPT ALL
             SELECT * FROM generic_ghd_binary_group_max_property)
        ) diff) AS max_diff_rows;
ROLLBACK;

BEGIN;
SET LOCAL age.enable_wcoj = on;
SET LOCAL enable_nestloop = off;
SET LOCAL enable_hashjoin = off;
SET LOCAL enable_mergejoin = off;
SET LOCAL enable_hashagg = off;
EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF)
SELECT *
FROM cypher('generic_ghd', $$
    MATCH (a:A)-[:E1]->(b:B),
          (b)-[:E2]->(c:C),
          (c)-[:E3]->(d:D),
          (d)-[:E4]->(a)
    RETURN id(a) AS key, count(*) AS total
$$) AS (key agtype, total agtype);
ROLLBACK;

BEGIN;
SET LOCAL age.enable_wcoj = on;
SET LOCAL enable_nestloop = off;
SET LOCAL enable_hashjoin = off;
SET LOCAL enable_mergejoin = off;
SET LOCAL enable_hashagg = off;
EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF)
SELECT *
FROM cypher('generic_ghd', $$
    MATCH (a:A)-[:E1]->(b:B),
          (b)-[:E2]->(c:C),
          (c)-[:E3]->(d:D),
          (d)-[:E4]->(a)
    RETURN id(a) AS key, count(DISTINCT id(a)) AS total
$$) AS (key agtype, total agtype);
ROLLBACK;

BEGIN;
SET LOCAL age.enable_wcoj = off;
CREATE TEMP TABLE generic_ghd_binary_group_count ON COMMIT DROP AS
SELECT *
FROM cypher('generic_ghd', $$
    MATCH (a:A)-[:E1]->(b:B),
          (b)-[:E2]->(c:C),
          (c)-[:E3]->(d:D),
          (d)-[:E4]->(a)
    RETURN id(a) AS key, count(*) AS total
$$) AS (key agtype, total agtype);

SET LOCAL age.enable_wcoj = on;
SET LOCAL enable_nestloop = off;
SET LOCAL enable_hashjoin = off;
SET LOCAL enable_mergejoin = off;
SET LOCAL enable_hashagg = off;
CREATE TEMP TABLE generic_ghd_join_group_count ON COMMIT DROP AS
SELECT *
FROM cypher('generic_ghd', $$
    MATCH (a:A)-[:E1]->(b:B),
          (b)-[:E2]->(c:C),
          (c)-[:E3]->(d:D),
          (d)-[:E4]->(a)
    RETURN id(a) AS key, count(*) AS total
$$) AS (key agtype, total agtype);

SELECT (SELECT count(*) FROM generic_ghd_binary_group_count) AS binary_rows,
       (SELECT count(*) FROM generic_ghd_join_group_count) AS generic_rows,
       (SELECT count(*) FROM (
            (SELECT * FROM generic_ghd_binary_group_count
             EXCEPT ALL
             SELECT * FROM generic_ghd_join_group_count)
            UNION ALL
            (SELECT * FROM generic_ghd_join_group_count
             EXCEPT ALL
             SELECT * FROM generic_ghd_binary_group_count)
        ) diff) AS diff_rows;
ROLLBACK;

BEGIN;
SET LOCAL age.enable_wcoj = off;
CREATE TEMP TABLE generic_ghd_binary_group_count_distinct ON COMMIT DROP AS
SELECT *
FROM cypher('generic_ghd', $$
    MATCH (a:A)-[:E1]->(b:B),
          (b)-[:E2]->(c:C),
          (c)-[:E3]->(d:D),
          (d)-[:E4]->(a)
    RETURN id(a) AS key, count(DISTINCT id(a)) AS total
$$) AS (key agtype, total agtype);

SET LOCAL age.enable_wcoj = on;
SET LOCAL enable_nestloop = off;
SET LOCAL enable_hashjoin = off;
SET LOCAL enable_mergejoin = off;
SET LOCAL enable_hashagg = off;
CREATE TEMP TABLE generic_ghd_join_group_count_distinct ON COMMIT DROP AS
SELECT *
FROM cypher('generic_ghd', $$
    MATCH (a:A)-[:E1]->(b:B),
          (b)-[:E2]->(c:C),
          (c)-[:E3]->(d:D),
          (d)-[:E4]->(a)
    RETURN id(a) AS key, count(DISTINCT id(a)) AS total
$$) AS (key agtype, total agtype);

SELECT (SELECT count(*) FROM generic_ghd_binary_group_count_distinct) AS binary_rows,
       (SELECT count(*) FROM generic_ghd_join_group_count_distinct) AS generic_rows,
       (SELECT count(*) FROM (
            (SELECT * FROM generic_ghd_binary_group_count_distinct
             EXCEPT ALL
             SELECT * FROM generic_ghd_join_group_count_distinct)
            UNION ALL
            (SELECT * FROM generic_ghd_join_group_count_distinct
             EXCEPT ALL
             SELECT * FROM generic_ghd_binary_group_count_distinct)
        ) diff) AS diff_rows;
ROLLBACK;

BEGIN;
SET LOCAL age.enable_wcoj = off;
CREATE TEMP TABLE generic_ghd_binary_count ON COMMIT DROP AS
SELECT *
FROM cypher('generic_ghd', $$
    MATCH (a:A)-[e1:E1]->(b:B)-[e2:E2]->(c:C)
          -[e3:E3]->(d:D)-[e4:E4]->(a),
          (c)-[t:TX]->(x:X)
    RETURN count(*) AS total
$$) AS (total agtype);

SET LOCAL age.enable_wcoj = on;
SET LOCAL enable_nestloop = off;
SET LOCAL enable_hashjoin = off;
SET LOCAL enable_mergejoin = off;
CREATE TEMP TABLE generic_ghd_join_count ON COMMIT DROP AS
SELECT *
FROM cypher('generic_ghd', $$
    MATCH (a:A)-[e1:E1]->(b:B)-[e2:E2]->(c:C)
          -[e3:E3]->(d:D)-[e4:E4]->(a),
          (c)-[t:TX]->(x:X)
    RETURN count(*) AS total
$$) AS (total agtype);

SELECT (SELECT total FROM generic_ghd_binary_count) AS binary_total,
       (SELECT total FROM generic_ghd_join_count) AS generic_total,
       (SELECT count(*) FROM (
            (SELECT * FROM generic_ghd_binary_count
             EXCEPT ALL
             SELECT * FROM generic_ghd_join_count)
            UNION ALL
            (SELECT * FROM generic_ghd_join_count
             EXCEPT ALL
             SELECT * FROM generic_ghd_binary_count)
        ) diff) AS diff_rows;
ROLLBACK;

BEGIN;
SET LOCAL age.enable_wcoj = on;
SET LOCAL enable_nestloop = off;
SET LOCAL enable_hashjoin = off;
SET LOCAL enable_mergejoin = off;
SET LOCAL enable_hashagg = off;
EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF)
SELECT *
FROM cypher('generic_ghd', $$
    MATCH (a:A)-[e1:E1]->(b:B)-[e2:E2]->(c:C)
          -[e3:E3]->(d:D)-[e4:E4]->(a)
    RETURN count(DISTINCT id(a)) AS total
$$) AS (total agtype);
ROLLBACK;

BEGIN;
SET LOCAL age.enable_wcoj = off;
CREATE TEMP TABLE generic_ghd_binary_count_distinct ON COMMIT DROP AS
SELECT *
FROM cypher('generic_ghd', $$
    MATCH (a:A)-[e1:E1]->(b:B)-[e2:E2]->(c:C)
          -[e3:E3]->(d:D)-[e4:E4]->(a)
    RETURN count(DISTINCT id(a)) AS total
$$) AS (total agtype);

SET LOCAL age.enable_wcoj = on;
SET LOCAL enable_nestloop = off;
SET LOCAL enable_hashjoin = off;
SET LOCAL enable_mergejoin = off;
SET LOCAL enable_hashagg = off;
CREATE TEMP TABLE generic_ghd_join_count_distinct ON COMMIT DROP AS
SELECT *
FROM cypher('generic_ghd', $$
    MATCH (a:A)-[e1:E1]->(b:B)-[e2:E2]->(c:C)
          -[e3:E3]->(d:D)-[e4:E4]->(a)
    RETURN count(DISTINCT id(a)) AS total
$$) AS (total agtype);

SELECT (SELECT total FROM generic_ghd_binary_count_distinct) AS binary_total,
       (SELECT total FROM generic_ghd_join_count_distinct) AS generic_total,
       (SELECT count(*) FROM (
            (SELECT * FROM generic_ghd_binary_count_distinct
             EXCEPT ALL
             SELECT * FROM generic_ghd_join_count_distinct)
            UNION ALL
            (SELECT * FROM generic_ghd_join_count_distinct
             EXCEPT ALL
             SELECT * FROM generic_ghd_binary_count_distinct)
        ) diff) AS diff_rows;
ROLLBACK;

BEGIN;
SET LOCAL age.enable_wcoj = on;
SET LOCAL enable_nestloop = off;
SET LOCAL enable_hashjoin = off;
SET LOCAL enable_mergejoin = off;
SET LOCAL enable_hashagg = off;
EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF)
SELECT *
FROM cypher('generic_ghd', $$
    MATCH (a:A)-[e1:E1]->(b:B)-[e2:E2]->(c:C)
          -[e3:E3]->(d:D)-[e4:E4]->(a)
    RETURN DISTINCT id(a) AS key
$$) AS (key agtype);
ROLLBACK;

BEGIN;
SET LOCAL age.enable_wcoj = off;
CREATE TEMP TABLE generic_ghd_binary_distinct_key ON COMMIT DROP AS
SELECT *
FROM cypher('generic_ghd', $$
    MATCH (a:A)-[e1:E1]->(b:B)-[e2:E2]->(c:C)
          -[e3:E3]->(d:D)-[e4:E4]->(a)
    RETURN DISTINCT id(a) AS key
$$) AS (key agtype);

SET LOCAL age.enable_wcoj = on;
SET LOCAL enable_nestloop = off;
SET LOCAL enable_hashjoin = off;
SET LOCAL enable_mergejoin = off;
SET LOCAL enable_hashagg = off;
CREATE TEMP TABLE generic_ghd_join_distinct_key ON COMMIT DROP AS
SELECT *
FROM cypher('generic_ghd', $$
    MATCH (a:A)-[e1:E1]->(b:B)-[e2:E2]->(c:C)
          -[e3:E3]->(d:D)-[e4:E4]->(a)
    RETURN DISTINCT id(a) AS key
$$) AS (key agtype);

SELECT (SELECT count(*) FROM generic_ghd_binary_distinct_key) AS binary_rows,
       (SELECT count(*) FROM generic_ghd_join_distinct_key) AS generic_rows,
       (SELECT count(*) FROM (
            (SELECT * FROM generic_ghd_binary_distinct_key
             EXCEPT ALL
             SELECT * FROM generic_ghd_join_distinct_key)
            UNION ALL
            (SELECT * FROM generic_ghd_join_distinct_key
             EXCEPT ALL
             SELECT * FROM generic_ghd_binary_distinct_key)
        ) diff) AS diff_rows;
ROLLBACK;

BEGIN;
SET LOCAL age.enable_wcoj = on;
SET LOCAL enable_nestloop = off;
SET LOCAL enable_hashjoin = off;
SET LOCAL enable_mergejoin = off;
EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF)
SELECT EXISTS (
    SELECT 1
    FROM cypher('generic_ghd', $$
        MATCH (a:A)-[e1:E1]->(b:B)-[e2:E2]->(c:C)
              -[e3:E3]->(d:D)-[e4:E4]->(a),
              (c)-[t:TX]->(x:X)
        RETURN id(a) AS a
    $$) AS (a agtype)
) AS exists_match;
ROLLBACK;

BEGIN;
SET LOCAL age.enable_wcoj = off;
CREATE TEMP TABLE generic_ghd_binary_exists ON COMMIT DROP AS
SELECT EXISTS (
    SELECT 1
    FROM cypher('generic_ghd', $$
        MATCH (a:A)-[e1:E1]->(b:B)-[e2:E2]->(c:C)
              -[e3:E3]->(d:D)-[e4:E4]->(a),
              (c)-[t:TX]->(x:X)
        RETURN id(a) AS a
    $$) AS (a agtype)
) AS exists_match;

SET LOCAL age.enable_wcoj = on;
SET LOCAL enable_nestloop = off;
SET LOCAL enable_hashjoin = off;
SET LOCAL enable_mergejoin = off;
CREATE TEMP TABLE generic_ghd_join_exists ON COMMIT DROP AS
SELECT EXISTS (
    SELECT 1
    FROM cypher('generic_ghd', $$
        MATCH (a:A)-[e1:E1]->(b:B)-[e2:E2]->(c:C)
              -[e3:E3]->(d:D)-[e4:E4]->(a),
              (c)-[t:TX]->(x:X)
        RETURN id(a) AS a
    $$) AS (a agtype)
) AS exists_match;

SELECT (SELECT exists_match FROM generic_ghd_binary_exists) AS binary_exists,
       (SELECT exists_match FROM generic_ghd_join_exists) AS generic_exists,
       (SELECT count(*) FROM (
            (SELECT * FROM generic_ghd_binary_exists
             EXCEPT ALL
             SELECT * FROM generic_ghd_join_exists)
            UNION ALL
            (SELECT * FROM generic_ghd_join_exists
             EXCEPT ALL
             SELECT * FROM generic_ghd_binary_exists)
        ) diff) AS diff_rows;
ROLLBACK;

BEGIN;
SET LOCAL age.enable_wcoj = on;
SET LOCAL enable_nestloop = off;
SET LOCAL enable_hashjoin = off;
SET LOCAL enable_mergejoin = off;
EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF)
SELECT *
FROM cypher('generic_ghd', $$
    MATCH (a:A)-[e1:E1]->(b:B)-[e2:E2]->(c:C)
          -[e3:E3]->(d:D)-[e4:E4]->(a)
    RETURN 1 AS one
    LIMIT 1
$$) AS (one agtype);
ROLLBACK;

BEGIN;
SET LOCAL age.enable_wcoj = off;
CREATE TEMP TABLE generic_ghd_binary_limit ON COMMIT DROP AS
SELECT *
FROM cypher('generic_ghd', $$
    MATCH (a:A)-[e1:E1]->(b:B)-[e2:E2]->(c:C)
          -[e3:E3]->(d:D)-[e4:E4]->(a)
    RETURN 1 AS one
    LIMIT 1
$$) AS (one agtype);

SET LOCAL age.enable_wcoj = on;
SET LOCAL enable_nestloop = off;
SET LOCAL enable_hashjoin = off;
SET LOCAL enable_mergejoin = off;
CREATE TEMP TABLE generic_ghd_join_limit ON COMMIT DROP AS
SELECT *
FROM cypher('generic_ghd', $$
    MATCH (a:A)-[e1:E1]->(b:B)-[e2:E2]->(c:C)
          -[e3:E3]->(d:D)-[e4:E4]->(a)
    RETURN 1 AS one
    LIMIT 1
$$) AS (one agtype);

SELECT (SELECT count(*) FROM generic_ghd_binary_limit) AS binary_rows,
       (SELECT count(*) FROM generic_ghd_join_limit) AS generic_rows,
       (SELECT count(*) FROM (
            (SELECT * FROM generic_ghd_binary_limit
             EXCEPT ALL
             SELECT * FROM generic_ghd_join_limit)
            UNION ALL
            (SELECT * FROM generic_ghd_join_limit
             EXCEPT ALL
             SELECT * FROM generic_ghd_binary_limit)
        ) diff) AS diff_rows;
ROLLBACK;

BEGIN;
SET LOCAL age.enable_wcoj = on;
SET LOCAL enable_nestloop = off;
SET LOCAL enable_hashjoin = off;
SET LOCAL enable_mergejoin = off;
EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF)
SELECT *
FROM cypher('generic_ghd', $$
    MATCH (a:A)-[e1:E1]->(b:B)-[e2:E2]->(c:C)
          -[e3:E3]->(d:D)-[e4:E4]->(a),
          (c)-[t:TX]->(x:X)
    RETURN id(a), id(b), id(c), id(d), id(x),
           id(e1), id(e2), id(e3), id(e4), id(t)
$$) AS (a agtype, b agtype, c agtype, d agtype, x agtype,
        e1 agtype, e2 agtype, e3 agtype, e4 agtype, t agtype);
ROLLBACK;

BEGIN;
SET LOCAL age.enable_wcoj = off;
CREATE TEMP TABLE generic_ghd_binary ON COMMIT DROP AS
SELECT *
FROM cypher('generic_ghd', $$
    MATCH (a:A)-[e1:E1]->(b:B)-[e2:E2]->(c:C)
          -[e3:E3]->(d:D)-[e4:E4]->(a),
          (c)-[t:TX]->(x:X)
    RETURN id(a), id(b), id(c), id(d), id(x),
           id(e1), id(e2), id(e3), id(e4), id(t)
$$) AS (a agtype, b agtype, c agtype, d agtype, x agtype,
        e1 agtype, e2 agtype, e3 agtype, e4 agtype, t agtype);

SET LOCAL age.enable_wcoj = on;
SET LOCAL enable_nestloop = off;
SET LOCAL enable_hashjoin = off;
SET LOCAL enable_mergejoin = off;
CREATE TEMP TABLE generic_ghd_join ON COMMIT DROP AS
SELECT *
FROM cypher('generic_ghd', $$
    MATCH (a:A)-[e1:E1]->(b:B)-[e2:E2]->(c:C)
          -[e3:E3]->(d:D)-[e4:E4]->(a),
          (c)-[t:TX]->(x:X)
    RETURN id(a), id(b), id(c), id(d), id(x),
           id(e1), id(e2), id(e3), id(e4), id(t)
$$) AS (a agtype, b agtype, c agtype, d agtype, x agtype,
        e1 agtype, e2 agtype, e3 agtype, e4 agtype, t agtype);

SELECT (SELECT count(*) FROM generic_ghd_binary) AS binary_rows,
       (SELECT count(*) FROM generic_ghd_join) AS generic_rows,
       (SELECT count(*) FROM (
           (TABLE generic_ghd_binary EXCEPT ALL TABLE generic_ghd_join)
           UNION ALL
           (TABLE generic_ghd_join EXCEPT ALL TABLE generic_ghd_binary)
       ) diff) AS diff_rows;
ROLLBACK;

/*
 * Multiple leaf tails on different cycle separators should build one domain
 * per separator before pruning the cyclic core.
 */
SELECT create_vlabel('generic_ghd', 'Y');
SELECT create_elabel('generic_ghd', 'TY');

INSERT INTO generic_ghd."Y" (id, properties)
VALUES (_graphid(_label_id('generic_ghd', 'Y'), 1), '{}'::agtype);
INSERT INTO generic_ghd."TY" (id, start_id, end_id, properties)
VALUES (_graphid(_label_id('generic_ghd', 'TY'), 1),
        _graphid(_label_id('generic_ghd', 'A'), 1),
        _graphid(_label_id('generic_ghd', 'Y'), 1),
        '{}'::agtype);

CREATE INDEX generic_ghd_ty_out
ON generic_ghd."TY" USING age_adjacency(start_id, id, end_id);

ANALYZE generic_ghd."Y";
ANALYZE generic_ghd."TY";

BEGIN;
SET LOCAL age.enable_wcoj = on;
SET LOCAL enable_nestloop = off;
SET LOCAL enable_hashjoin = off;
SET LOCAL enable_mergejoin = off;
EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF)
SELECT *
FROM cypher('generic_ghd', $$
    MATCH (a:A)-[e1:E1]->(b:B)-[e2:E2]->(c:C)
          -[e3:E3]->(d:D)-[e4:E4]->(a),
          (c)-[tx:TX]->(x:X),
          (a)-[ty:TY]->(y:Y)
    RETURN id(a), id(b), id(c), id(d), id(x), id(y),
           id(e1), id(e2), id(e3), id(e4), id(tx), id(ty)
$$) AS (a agtype, b agtype, c agtype, d agtype,
        x agtype, y agtype, e1 agtype, e2 agtype,
        e3 agtype, e4 agtype, tx agtype, ty agtype);
ROLLBACK;

BEGIN;
SET LOCAL age.enable_wcoj = off;
CREATE TEMP TABLE generic_ghd_multi_tail_binary ON COMMIT DROP AS
SELECT *
FROM cypher('generic_ghd', $$
    MATCH (a:A)-[e1:E1]->(b:B)-[e2:E2]->(c:C)
          -[e3:E3]->(d:D)-[e4:E4]->(a),
          (c)-[tx:TX]->(x:X),
          (a)-[ty:TY]->(y:Y)
    RETURN id(a), id(b), id(c), id(d), id(x), id(y),
           id(e1), id(e2), id(e3), id(e4), id(tx), id(ty)
$$) AS (a agtype, b agtype, c agtype, d agtype,
        x agtype, y agtype, e1 agtype, e2 agtype,
        e3 agtype, e4 agtype, tx agtype, ty agtype);

SET LOCAL age.enable_wcoj = on;
SET LOCAL enable_nestloop = off;
SET LOCAL enable_hashjoin = off;
SET LOCAL enable_mergejoin = off;
CREATE TEMP TABLE generic_ghd_multi_tail_join ON COMMIT DROP AS
SELECT *
FROM cypher('generic_ghd', $$
    MATCH (a:A)-[e1:E1]->(b:B)-[e2:E2]->(c:C)
          -[e3:E3]->(d:D)-[e4:E4]->(a),
          (c)-[tx:TX]->(x:X),
          (a)-[ty:TY]->(y:Y)
    RETURN id(a), id(b), id(c), id(d), id(x), id(y),
           id(e1), id(e2), id(e3), id(e4), id(tx), id(ty)
$$) AS (a agtype, b agtype, c agtype, d agtype,
        x agtype, y agtype, e1 agtype, e2 agtype,
        e3 agtype, e4 agtype, tx agtype, ty agtype);

SELECT (SELECT count(*) FROM generic_ghd_multi_tail_binary) AS binary_rows,
       (SELECT count(*) FROM generic_ghd_multi_tail_join) AS generic_rows,
       (SELECT count(*) FROM (
           (TABLE generic_ghd_multi_tail_binary EXCEPT ALL
            TABLE generic_ghd_multi_tail_join)
           UNION ALL
           (TABLE generic_ghd_multi_tail_join EXCEPT ALL
            TABLE generic_ghd_multi_tail_binary)
       ) diff) AS diff_rows;
ROLLBACK;

/*
 * Low-pressure acyclic chains still get a costed Generic Join candidate and
 * pass the planner-owned graph-join match IR summary to the executor.
 */
SELECT create_graph('generic_ir');
SELECT create_vlabel('generic_ir', 'T_A');
SELECT create_vlabel('generic_ir', 'T_B');
SELECT create_vlabel('generic_ir', 'T_C');
SELECT create_vlabel('generic_ir', 'T_D');
SELECT create_elabel('generic_ir', 'T_AB');
SELECT create_elabel('generic_ir', 'T_BC');
SELECT create_elabel('generic_ir', 'T_CD');

INSERT INTO generic_ir."T_A" (id, properties)
VALUES (_graphid(_label_id('generic_ir', 'T_A'), 1), '{}'::agtype);
INSERT INTO generic_ir."T_B" (id, properties)
SELECT _graphid(_label_id('generic_ir', 'T_B'), i), '{}'::agtype
FROM generate_series(1, 80) i;
INSERT INTO generic_ir."T_C" (id, properties)
SELECT _graphid(_label_id('generic_ir', 'T_C'), i), '{}'::agtype
FROM generate_series(1, 80) i;
INSERT INTO generic_ir."T_D" (id, properties)
SELECT _graphid(_label_id('generic_ir', 'T_D'), i), '{}'::agtype
FROM generate_series(1, 80) i;

INSERT INTO generic_ir."T_AB" (id, start_id, end_id, properties)
SELECT _graphid(_label_id('generic_ir', 'T_AB'), i),
       _graphid(_label_id('generic_ir', 'T_A'), 1),
       _graphid(_label_id('generic_ir', 'T_B'), i),
       '{}'::agtype
FROM generate_series(1, 80) i;
INSERT INTO generic_ir."T_BC" (id, start_id, end_id, properties)
SELECT _graphid(_label_id('generic_ir', 'T_BC'), (i - 1) * 80 + j),
       _graphid(_label_id('generic_ir', 'T_B'), i),
       _graphid(_label_id('generic_ir', 'T_C'), j),
       '{}'::agtype
FROM generate_series(1, 80) i,
     generate_series(1, 80) j;
INSERT INTO generic_ir."T_CD" (id, start_id, end_id, properties)
VALUES (_graphid(_label_id('generic_ir', 'T_CD'), 1),
        _graphid(_label_id('generic_ir', 'T_C'), 1),
        _graphid(_label_id('generic_ir', 'T_D'), 1),
        '{}'::agtype);

CREATE INDEX generic_ir_tab_out
ON generic_ir."T_AB" USING age_adjacency(start_id, id, end_id);
CREATE INDEX generic_ir_tbc_out
ON generic_ir."T_BC" USING age_adjacency(start_id, id, end_id);
CREATE INDEX generic_ir_tcd_out
ON generic_ir."T_CD" USING age_adjacency(start_id, id, end_id);

ANALYZE generic_ir."T_A";
ANALYZE generic_ir."T_B";
ANALYZE generic_ir."T_C";
ANALYZE generic_ir."T_D";
ANALYZE generic_ir."T_AB";
ANALYZE generic_ir."T_BC";
ANALYZE generic_ir."T_CD";

BEGIN;
SET LOCAL age.enable_wcoj = on;
SET LOCAL enable_nestloop = off;
SET LOCAL enable_hashjoin = off;
SET LOCAL enable_mergejoin = off;
EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF)
SELECT *
FROM cypher('generic_ir', $$
    MATCH (a:T_A)-[ab:T_AB]->(b:T_B)-[bc:T_BC]->(c:T_C)
          -[cd:T_CD]->(d:T_D)
    RETURN id(a), id(b), id(c), id(d), id(ab), id(bc), id(cd)
$$) AS (a agtype, b agtype, c agtype, d agtype,
        ab agtype, bc agtype, cd agtype);
ROLLBACK;

BEGIN;
SET LOCAL age.enable_wcoj = on;
SET LOCAL enable_nestloop = off;
SET LOCAL enable_hashjoin = off;
SET LOCAL enable_mergejoin = off;
EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF)
SELECT *
FROM cypher('generic_ir', $$
    MATCH (a:T_A)-[ab:T_AB]->(b:T_B)-[bc:T_BC]->(c:T_C)
          -[cd:T_CD]->(d:T_D)
    RETURN count(*) AS total
$$) AS (total agtype);
ROLLBACK;

SELECT drop_graph('generic_ir', true);

/*
 * Repeated middle prefixes keep returning to the same P_C -> P_D ranges for
 * each P_B branch.  The executor should reuse the post-reduction prefix range
 * directory and provider cursor instead of rebuilding raw row ranges for those
 * recursive lookups.
 */
SELECT create_vlabel('generic_ghd', 'P_A');
SELECT create_vlabel('generic_ghd', 'P_B');
SELECT create_vlabel('generic_ghd', 'P_C');
SELECT create_vlabel('generic_ghd', 'P_D');
SELECT create_elabel('generic_ghd', 'PAB');
SELECT create_elabel('generic_ghd', 'PBC');
SELECT create_elabel('generic_ghd', 'PCD');
SELECT create_elabel('generic_ghd', 'PAD');

INSERT INTO generic_ghd."P_A" (id, properties)
VALUES (_graphid(_label_id('generic_ghd', 'P_A'), 1), '{}'::agtype);
INSERT INTO generic_ghd."P_B" (id, properties)
SELECT _graphid(_label_id('generic_ghd', 'P_B'), i), '{}'::agtype
FROM generate_series(1, 12) i;
INSERT INTO generic_ghd."P_C" (id, properties)
SELECT _graphid(_label_id('generic_ghd', 'P_C'), i), '{}'::agtype
FROM generate_series(1, 4) i;
INSERT INTO generic_ghd."P_D" (id, properties)
SELECT _graphid(_label_id('generic_ghd', 'P_D'), i), '{}'::agtype
FROM generate_series(1, 4) i;

INSERT INTO generic_ghd."PAB" (id, start_id, end_id, properties)
SELECT _graphid(_label_id('generic_ghd', 'PAB'), i),
       _graphid(_label_id('generic_ghd', 'P_A'), 1),
       _graphid(_label_id('generic_ghd', 'P_B'), i),
       '{}'::agtype
FROM generate_series(1, 12) i;
INSERT INTO generic_ghd."PBC" (id, start_id, end_id, properties)
SELECT _graphid(_label_id('generic_ghd', 'PBC'), (i - 1) * 4 + j),
       _graphid(_label_id('generic_ghd', 'P_B'), i),
       _graphid(_label_id('generic_ghd', 'P_C'), j),
       '{}'::agtype
FROM generate_series(1, 12) i,
     generate_series(1, 4) j;
INSERT INTO generic_ghd."PCD" (id, start_id, end_id, properties)
SELECT _graphid(_label_id('generic_ghd', 'PCD'), i),
       _graphid(_label_id('generic_ghd', 'P_C'), i),
       _graphid(_label_id('generic_ghd', 'P_D'), i),
       '{}'::agtype
FROM generate_series(1, 4) i;
INSERT INTO generic_ghd."PAD" (id, start_id, end_id, properties)
SELECT _graphid(_label_id('generic_ghd', 'PAD'), i),
       _graphid(_label_id('generic_ghd', 'P_A'), 1),
       _graphid(_label_id('generic_ghd', 'P_D'), i),
       '{}'::agtype
FROM generate_series(1, 4) i;

CREATE INDEX generic_ghd_pab_out
ON generic_ghd."PAB" USING age_adjacency(start_id, id, end_id);
CREATE INDEX generic_ghd_pbc_out
ON generic_ghd."PBC" USING age_adjacency(start_id, id, end_id);
CREATE INDEX generic_ghd_pcd_out
ON generic_ghd."PCD" USING age_adjacency(start_id, id, end_id);
CREATE INDEX generic_ghd_pad_out
ON generic_ghd."PAD" USING age_adjacency(start_id, id, end_id);

ANALYZE generic_ghd."P_A";
ANALYZE generic_ghd."P_B";
ANALYZE generic_ghd."P_C";
ANALYZE generic_ghd."P_D";
ANALYZE generic_ghd."PAB";
ANALYZE generic_ghd."PBC";
ANALYZE generic_ghd."PCD";
ANALYZE generic_ghd."PAD";

BEGIN;
SET LOCAL age.enable_wcoj = on;
SET LOCAL enable_nestloop = off;
SET LOCAL enable_hashjoin = off;
SET LOCAL enable_mergejoin = off;
EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF)
SELECT *
FROM cypher('generic_ghd', $$
    MATCH (a:P_A)-[ab:PAB]->(b:P_B)-[bc:PBC]->(c:P_C)
          -[cd:PCD]->(d:P_D),
          (a)-[ad:PAD]->(d)
    RETURN id(a), id(b), id(c), id(d),
           id(ab), id(bc), id(cd), id(ad)
$$) AS (a agtype, b agtype, c agtype, d agtype,
        ab agtype, bc agtype, cd agtype, ad agtype);
ROLLBACK;

BEGIN;
SET LOCAL age.enable_wcoj = on;
SET LOCAL enable_nestloop = off;
SET LOCAL enable_hashjoin = off;
SET LOCAL enable_mergejoin = off;
EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF)
SELECT *
FROM cypher('generic_ghd', $$
    MATCH (a:A {missing: true})-[e1:E1]->(b:B)-[e2:E2]->(c:C)
          -[e3:E3]->(d:D)-[e4:E4]->(a)
    RETURN id(a), id(b), id(c), id(d), id(e1), id(e2), id(e3), id(e4)
$$) AS (a agtype, b agtype, c agtype, d agtype,
        e1 agtype, e2 agtype, e3 agtype, e4 agtype);
ROLLBACK;

/*
 * Deep acyclic tails should propagate leaf-domain pruning back to the cycle
 * separator before the cyclic core is searched.
 */
SELECT create_vlabel('generic_ghd', 'U');
SELECT create_vlabel('generic_ghd', 'V');
SELECT create_vlabel('generic_ghd', 'W');
SELECT create_elabel('generic_ghd', 'TU');
SELECT create_elabel('generic_ghd', 'TV');
SELECT create_elabel('generic_ghd', 'TW');

INSERT INTO generic_ghd."U" (id, properties)
SELECT _graphid(_label_id('generic_ghd', 'U'), i), '{}'::agtype
FROM generate_series(1, 64) i;
INSERT INTO generic_ghd."V" (id, properties)
SELECT _graphid(_label_id('generic_ghd', 'V'), i), '{}'::agtype
FROM generate_series(1, 64) i;
INSERT INTO generic_ghd."W" (id, properties)
VALUES (_graphid(_label_id('generic_ghd', 'W'), 1), '{}'::agtype);

INSERT INTO generic_ghd."TU" (id, start_id, end_id, properties)
SELECT _graphid(_label_id('generic_ghd', 'TU'), i),
       _graphid(_label_id('generic_ghd', 'C'), i),
       _graphid(_label_id('generic_ghd', 'U'), i),
       '{}'::agtype
FROM generate_series(1, 64) i;
INSERT INTO generic_ghd."TV" (id, start_id, end_id, properties)
SELECT _graphid(_label_id('generic_ghd', 'TV'), i),
       _graphid(_label_id('generic_ghd', 'U'), i),
       _graphid(_label_id('generic_ghd', 'V'), i),
       '{}'::agtype
FROM generate_series(1, 64) i;
INSERT INTO generic_ghd."TW" (id, start_id, end_id, properties)
VALUES (_graphid(_label_id('generic_ghd', 'TW'), 1),
        _graphid(_label_id('generic_ghd', 'V'), 1),
        _graphid(_label_id('generic_ghd', 'W'), 1),
        '{}'::agtype);

CREATE INDEX generic_ghd_tu_out
ON generic_ghd."TU" USING age_adjacency(start_id, id, end_id);
CREATE INDEX generic_ghd_tv_out
ON generic_ghd."TV" USING age_adjacency(start_id, id, end_id);
CREATE INDEX generic_ghd_tw_out
ON generic_ghd."TW" USING age_adjacency(start_id, id, end_id);

ANALYZE generic_ghd."U";
ANALYZE generic_ghd."V";
ANALYZE generic_ghd."W";
ANALYZE generic_ghd."TU";
ANALYZE generic_ghd."TV";
ANALYZE generic_ghd."TW";

BEGIN;
SET LOCAL age.enable_wcoj = on;
SET LOCAL enable_nestloop = off;
SET LOCAL enable_hashjoin = off;
SET LOCAL enable_mergejoin = off;
EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF)
SELECT *
FROM cypher('generic_ghd', $$
    MATCH (a:A)-[e1:E1]->(b:B)-[e2:E2]->(c:C)
          -[e3:E3]->(d:D)-[e4:E4]->(a),
          (c)-[tu:TU]->(u:U)-[tv:TV]->(v:V)-[tw:TW]->(w:W)
    RETURN id(a), id(b), id(c), id(d), id(u), id(v), id(w),
           id(e1), id(e2), id(e3), id(e4), id(tu), id(tv), id(tw)
$$) AS (a agtype, b agtype, c agtype, d agtype,
        u agtype, v agtype, w agtype, e1 agtype,
        e2 agtype, e3 agtype, e4 agtype, tu agtype,
        tv agtype, tw agtype);
ROLLBACK;

BEGIN;
SET LOCAL age.enable_wcoj = off;
CREATE TEMP TABLE generic_ghd_deep_tail_binary ON COMMIT DROP AS
SELECT *
FROM cypher('generic_ghd', $$
    MATCH (a:A)-[e1:E1]->(b:B)-[e2:E2]->(c:C)
          -[e3:E3]->(d:D)-[e4:E4]->(a),
          (c)-[tu:TU]->(u:U)-[tv:TV]->(v:V)-[tw:TW]->(w:W)
    RETURN id(a), id(b), id(c), id(d), id(u), id(v), id(w),
           id(e1), id(e2), id(e3), id(e4), id(tu), id(tv), id(tw)
$$) AS (a agtype, b agtype, c agtype, d agtype,
        u agtype, v agtype, w agtype, e1 agtype,
        e2 agtype, e3 agtype, e4 agtype, tu agtype,
        tv agtype, tw agtype);

SET LOCAL age.enable_wcoj = on;
SET LOCAL enable_nestloop = off;
SET LOCAL enable_hashjoin = off;
SET LOCAL enable_mergejoin = off;
CREATE TEMP TABLE generic_ghd_deep_tail_join ON COMMIT DROP AS
SELECT *
FROM cypher('generic_ghd', $$
    MATCH (a:A)-[e1:E1]->(b:B)-[e2:E2]->(c:C)
          -[e3:E3]->(d:D)-[e4:E4]->(a),
          (c)-[tu:TU]->(u:U)-[tv:TV]->(v:V)-[tw:TW]->(w:W)
    RETURN id(a), id(b), id(c), id(d), id(u), id(v), id(w),
           id(e1), id(e2), id(e3), id(e4), id(tu), id(tv), id(tw)
$$) AS (a agtype, b agtype, c agtype, d agtype,
        u agtype, v agtype, w agtype, e1 agtype,
        e2 agtype, e3 agtype, e4 agtype, tu agtype,
        tv agtype, tw agtype);

SELECT (SELECT count(*) FROM generic_ghd_deep_tail_binary) AS binary_rows,
       (SELECT count(*) FROM generic_ghd_deep_tail_join) AS generic_rows,
       (SELECT count(*) FROM (
           (TABLE generic_ghd_deep_tail_binary EXCEPT ALL
            TABLE generic_ghd_deep_tail_join)
           UNION ALL
           (TABLE generic_ghd_deep_tail_join EXCEPT ALL
            TABLE generic_ghd_deep_tail_binary)
       ) diff) AS diff_rows;
ROLLBACK;

SELECT drop_graph('generic_ghd', true);
-- End cypher_generic_join_ghd.
