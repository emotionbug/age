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
SELECT create_elabel('generic_ghd', 'E1');
SELECT create_elabel('generic_ghd', 'E2');
SELECT create_elabel('generic_ghd', 'E3');
SELECT create_elabel('generic_ghd', 'E4');
SELECT create_elabel('generic_ghd', 'TX');

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
       '{}'::agtype
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

ANALYZE generic_ghd."A";
ANALYZE generic_ghd."B";
ANALYZE generic_ghd."C";
ANALYZE generic_ghd."D";
ANALYZE generic_ghd."X";
ANALYZE generic_ghd."E1";
ANALYZE generic_ghd."E2";
ANALYZE generic_ghd."E3";
ANALYZE generic_ghd."E4";
ANALYZE generic_ghd."TX";

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

DO $generic_ghd_count_consumer_plan$
DECLARE
    plan_text text;
    has_generic boolean := false;
    has_count_consumer boolean := false;
    has_count_result boolean := false;
    has_no_flat_rows boolean := false;
    has_consumer_avoids boolean := false;
    has_rows_emitted boolean := false;
    has_eager_provider boolean := false;
    has_full_materialization boolean := false;
    has_separator_pass boolean := false;
    has_descriptor_separators boolean := false;
BEGIN
    PERFORM set_config('age.enable_wcoj', 'on', true);
    PERFORM set_config('enable_nestloop', 'off', true);
    PERFORM set_config('enable_hashjoin', 'off', true);
    PERFORM set_config('enable_mergejoin', 'off', true);

    FOR plan_text IN EXECUTE $plan$
        EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF)
        SELECT *
        FROM cypher('generic_ghd', $cypher$
            MATCH (a:A)-[e1:E1]->(b:B)-[e2:E2]->(c:C)
                  -[e3:E3]->(d:D)-[e4:E4]->(a),
                  (c)-[t:TX]->(x:X)
            RETURN count(*) AS total
        $cypher$) AS (total agtype)
    $plan$
    LOOP
        has_generic := has_generic OR
            plan_text LIKE '%Custom Scan (AGE Generic Multiway Join)%';
        has_count_consumer := has_count_consumer OR
            plan_text LIKE '%Generic Join Consumer: count(*)%';
        has_count_result := has_count_result OR
            plan_text LIKE '%Count Result: 1%';
        has_no_flat_rows := has_no_flat_rows OR
            plan_text LIKE '%Flat Rows Materialized: 0%';
        has_consumer_avoids := has_consumer_avoids OR
            plan_text LIKE '%Consumer Flat Rows Avoided: 1%';
        has_rows_emitted := has_rows_emitted OR
            plan_text LIKE '%Rows Emitted: 1%';
        has_eager_provider := has_eager_provider OR
            plan_text LIKE '%Lazy Physical Provider: false%';
        has_full_materialization := has_full_materialization OR
            plan_text LIKE '%Provider Full Materialization: true%';
        has_separator_pass := has_separator_pass OR
            plan_text LIKE '%GHD Separator Reduction Passes: 2%';
        has_descriptor_separators := has_descriptor_separators OR
            plan_text LIKE '%GHD Descriptor Separators Applied: 2%';
    END LOOP;

    IF NOT has_generic OR NOT has_count_consumer OR
       NOT has_count_result OR NOT has_no_flat_rows OR
       NOT has_consumer_avoids OR NOT has_rows_emitted OR
       NOT has_eager_provider OR NOT has_full_materialization OR
       NOT has_separator_pass OR NOT has_descriptor_separators THEN
        RAISE EXCEPTION
            'Generic Join count consumer with GHD separator reduction was not observed';
    END IF;
    RAISE NOTICE 'generic join count consumer with GHD separator reduction verified';
END
$generic_ghd_count_consumer_plan$;

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

DO $generic_ghd_count_distinct_consumer_plan$
DECLARE
    plan_text text;
    has_generic boolean := false;
    has_distinct_consumer boolean := false;
    has_count_result boolean := false;
    has_distinct_count boolean := false;
    has_no_flat_rows boolean := false;
    has_consumer_avoids boolean := false;
    has_rows_emitted boolean := false;
BEGIN
    PERFORM set_config('age.enable_wcoj', 'on', true);
    PERFORM set_config('enable_nestloop', 'off', true);
    PERFORM set_config('enable_hashjoin', 'off', true);
    PERFORM set_config('enable_mergejoin', 'off', true);
    PERFORM set_config('enable_hashagg', 'off', true);

    FOR plan_text IN EXECUTE $plan$
        EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF)
        SELECT *
        FROM cypher('generic_ghd', $cypher$
            MATCH (a:A)-[e1:E1]->(b:B)-[e2:E2]->(c:C)
                  -[e3:E3]->(d:D)-[e4:E4]->(a)
            RETURN count(DISTINCT id(a)) AS total
        $cypher$) AS (total agtype)
    $plan$
    LOOP
        has_generic := has_generic OR
            plan_text LIKE '%Custom Scan (AGE Generic Multiway Join)%';
        has_distinct_consumer := has_distinct_consumer OR
            plan_text LIKE '%Generic Join Consumer: count(distinct key)%';
        has_count_result := has_count_result OR
            plan_text LIKE '%Count Result: 64%';
        has_distinct_count := has_distinct_count OR
            plan_text LIKE '%Distinct Key Count: 64%';
        has_no_flat_rows := has_no_flat_rows OR
            plan_text LIKE '%Flat Rows Materialized: 0%';
        has_consumer_avoids := has_consumer_avoids OR
            plan_text LIKE '%Consumer Flat Rows Avoided: 64%';
        has_rows_emitted := has_rows_emitted OR
            plan_text LIKE '%Rows Emitted: 1%';
    END LOOP;

    IF NOT has_generic OR NOT has_distinct_consumer OR
       NOT has_count_result OR NOT has_distinct_count OR
       NOT has_no_flat_rows OR NOT has_consumer_avoids OR
       NOT has_rows_emitted THEN
        RAISE EXCEPTION
            'Generic Join count distinct-key consumer was not observed';
    END IF;
    RAISE NOTICE 'generic join count distinct-key consumer verified';
END
$generic_ghd_count_distinct_consumer_plan$;

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

DO $generic_ghd_exists_consumer_plan$
DECLARE
    plan_text text;
    has_generic boolean := false;
    has_exists_consumer boolean := false;
    has_row_goal boolean := false;
    has_row_goal_source boolean := false;
    has_exists_result boolean := false;
    has_no_flat_rows boolean := false;
    has_consumer_avoids boolean := false;
    has_rows_emitted boolean := false;
BEGIN
    PERFORM set_config('age.enable_wcoj', 'on', true);
    PERFORM set_config('enable_nestloop', 'off', true);
    PERFORM set_config('enable_hashjoin', 'off', true);
    PERFORM set_config('enable_mergejoin', 'off', true);

    FOR plan_text IN EXECUTE $plan$
        EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF)
        SELECT EXISTS (
            SELECT 1
            FROM cypher('generic_ghd', $cypher$
                MATCH (a:A)-[e1:E1]->(b:B)-[e2:E2]->(c:C)
                      -[e3:E3]->(d:D)-[e4:E4]->(a),
                      (c)-[t:TX]->(x:X)
                RETURN id(a) AS a
            $cypher$) AS (a agtype)
        ) AS exists_match
    $plan$
    LOOP
        has_generic := has_generic OR
            plan_text LIKE '%Custom Scan (AGE Generic Multiway Join)%';
        has_exists_consumer := has_exists_consumer OR
            plan_text LIKE '%Generic Join Consumer: exists%';
        has_row_goal := has_row_goal OR
            plan_text LIKE '%Generic Join Row Goal: 1%';
        has_row_goal_source := has_row_goal_source OR
            plan_text LIKE '%Generic Join Row Goal Source: exists%';
        has_exists_result := has_exists_result OR
            plan_text LIKE '%Exists Result: true%';
        has_no_flat_rows := has_no_flat_rows OR
            plan_text LIKE '%Flat Rows Materialized: 0%';
        has_consumer_avoids := has_consumer_avoids OR
            plan_text LIKE '%Consumer Flat Rows Avoided: 1%';
        has_rows_emitted := has_rows_emitted OR
            plan_text LIKE '%Rows Emitted: 1%';
    END LOOP;

    IF NOT has_generic OR NOT has_exists_consumer OR
       NOT has_row_goal OR NOT has_row_goal_source OR
       NOT has_exists_result OR NOT has_no_flat_rows OR
       NOT has_consumer_avoids OR NOT has_rows_emitted THEN
        RAISE EXCEPTION
            'Generic Join exists consumer was not observed';
    END IF;
    RAISE NOTICE 'generic join exists consumer verified';
END
$generic_ghd_exists_consumer_plan$;

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

DO $generic_ghd_limit_consumer_plan$
DECLARE
    plan_text text;
    has_generic boolean := false;
    has_limit_consumer boolean := false;
    has_row_goal boolean := false;
    has_row_goal_source boolean := false;
    has_row_goal_reached boolean := false;
    has_flat_row boolean := false;
    has_consumer_avoids boolean := false;
    has_rows_emitted boolean := false;
BEGIN
    PERFORM set_config('age.enable_wcoj', 'on', true);
    PERFORM set_config('enable_nestloop', 'off', true);
    PERFORM set_config('enable_hashjoin', 'off', true);
    PERFORM set_config('enable_mergejoin', 'off', true);

    FOR plan_text IN EXECUTE $plan$
        EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF)
        SELECT *
        FROM cypher('generic_ghd', $cypher$
            MATCH (a:A)-[e1:E1]->(b:B)-[e2:E2]->(c:C)
                  -[e3:E3]->(d:D)-[e4:E4]->(a)
            RETURN 1 AS one
            LIMIT 1
        $cypher$) AS (one agtype)
    $plan$
    LOOP
        has_generic := has_generic OR
            plan_text LIKE '%Custom Scan (AGE Generic Multiway Join)%';
        has_limit_consumer := has_limit_consumer OR
            plan_text LIKE '%Generic Join Consumer: limit%';
        has_row_goal := has_row_goal OR
            plan_text LIKE '%Generic Join Row Goal: 1%';
        has_row_goal_source := has_row_goal_source OR
            plan_text LIKE '%Generic Join Row Goal Source: limit%';
        has_row_goal_reached := has_row_goal_reached OR
            plan_text LIKE '%Row Goal Reached: true%';
        has_flat_row := has_flat_row OR
            plan_text LIKE '%Flat Rows Materialized: 1%';
        has_consumer_avoids := has_consumer_avoids OR
            plan_text LIKE '%Consumer Flat Rows Avoided:%';
        has_rows_emitted := has_rows_emitted OR
            plan_text LIKE '%Rows Emitted: 1%';
    END LOOP;

    IF NOT has_generic OR NOT has_limit_consumer OR
       NOT has_row_goal OR NOT has_row_goal_source OR
       NOT has_row_goal_reached OR NOT has_flat_row OR
       NOT has_consumer_avoids OR NOT has_rows_emitted THEN
        RAISE EXCEPTION
            'Generic Join limit consumer was not observed';
    END IF;
    RAISE NOTICE 'generic join limit consumer verified';
END
$generic_ghd_limit_consumer_plan$;

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

DO $generic_ghd_separator_plan$
DECLARE
    plan_text text;
    has_generic boolean := false;
    has_component_count boolean := false;
    has_component_ids boolean := false;
    has_reduction_shape boolean := false;
    has_descriptor_source boolean := false;
    has_ghd_mode boolean := false;
    has_ghd_general boolean := false;
    has_ghd_fallback boolean := false;
    has_ghd_bag_count boolean := false;
    has_ghd_bag_details boolean := false;
    has_ghd_separator_count boolean := false;
    has_ghd_separator_details boolean := false;
    has_ghd_descriptor_source boolean := false;
    has_reduction_core boolean := false;
    has_reduction_tail boolean := false;
    has_separator_pass boolean := false;
    has_leaf_tail_provider boolean := false;
    has_descriptor_separators boolean := false;
    has_separator_domain boolean := false;
    has_core_pruning boolean := false;
    has_provider_rows boolean := false;
    has_key_only_tuple_skip boolean := false;
    has_reduction_scratch_allocation boolean := false;
    has_reduction_scratch_reuse boolean := false;
    has_one_row boolean := false;
BEGIN
    PERFORM set_config('age.enable_wcoj', 'on', true);
    PERFORM set_config('enable_nestloop', 'off', true);
    PERFORM set_config('enable_hashjoin', 'off', true);
    PERFORM set_config('enable_mergejoin', 'off', true);

    FOR plan_text IN EXECUTE $plan$
        EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY OFF)
        SELECT *
        FROM cypher('generic_ghd', $cypher$
            MATCH (a:A)-[e1:E1]->(b:B)-[e2:E2]->(c:C)
                  -[e3:E3]->(d:D)-[e4:E4]->(a),
                  (c)-[t:TX]->(x:X)
            RETURN id(a), id(b), id(c), id(d), id(x),
                   id(e1), id(e2), id(e3), id(e4), id(t)
        $cypher$) AS (a agtype, b agtype, c agtype, d agtype, x agtype,
                      e1 agtype, e2 agtype, e3 agtype, e4 agtype,
                      t agtype)
    $plan$
    LOOP
        has_generic := has_generic OR
            plan_text LIKE '%Custom Scan (AGE Generic Multiway Join)%';
        has_component_count := has_component_count OR
            plan_text LIKE '%Component Count: 1%';
        has_component_ids := has_component_ids OR
            plan_text LIKE '%Component IDs: 1, 1, 1, 1, 1%';
        has_reduction_shape := has_reduction_shape OR
            plan_text LIKE '%Reduction Shape: cyclic-with-tail%';
        has_ghd_mode := has_ghd_mode OR
            plan_text LIKE '%GHD Mode: general GHD%';
        has_ghd_general := has_ghd_general OR
            plan_text LIKE '%GHD General Decomposition: true%';
        has_ghd_fallback := has_ghd_fallback OR
            plan_text LIKE '%GHD Fallback Reason: none%';
        has_descriptor_source := has_descriptor_source OR
            plan_text LIKE
            '%Reduction Descriptor Source: graph-join-match-ir%';
        has_ghd_bag_count := has_ghd_bag_count OR
            plan_text LIKE '%GHD Bag Count: 3%';
        has_ghd_bag_details := has_ghd_bag_details OR
            plan_text LIKE
            '%GHD Bags: bag 1 cyclic-core:%bag 2 cyclic-core:%bag 3 leaf-tail:%';
        has_ghd_separator_count := has_ghd_separator_count OR
            plan_text LIKE '%GHD Separator Count: 2%';
        has_ghd_separator_details := has_ghd_separator_details OR
            plan_text LIKE '%GHD Separators:%pair v%provider %';
        has_ghd_descriptor_source := has_ghd_descriptor_source OR
            plan_text LIKE '%GHD Descriptor Source: graph-join-match-ir%';
        has_reduction_core := has_reduction_core OR
            plan_text LIKE '%Reduction Core Variables: 4%';
        has_reduction_tail := has_reduction_tail OR
            plan_text LIKE '%Reduction Tail Separators: 1%';
        has_separator_pass := has_separator_pass OR
            plan_text LIKE '%GHD Separator Reduction Passes: 2%';
        has_leaf_tail_provider := has_leaf_tail_provider OR
            plan_text LIKE '%GHD Leaf Tail Providers: 1%';
        has_descriptor_separators := has_descriptor_separators OR
            plan_text LIKE '%GHD Descriptor Separators Applied: 2%';
        has_separator_domain := has_separator_domain OR
            plan_text LIKE '%GHD Separator Domain Keys: 3%';
        has_core_pruning := has_core_pruning OR
            plan_text LIKE '%GHD Cyclic Core Rows Removed: 315%';
        has_provider_rows := has_provider_rows OR
            plan_text ~ 'Provider Rows Materialized: [1-9][0-9]*';
        has_key_only_tuple_skip := has_key_only_tuple_skip OR
            plan_text LIKE '%Provider Tuples Materialized: 0%';
        has_reduction_scratch_allocation :=
            has_reduction_scratch_allocation OR
            plan_text ~ 'Reduction Scratch Allocations: [1-9][0-9]*';
        has_reduction_scratch_reuse := has_reduction_scratch_reuse OR
            plan_text ~ 'Reduction Scratch Reuses: [1-9][0-9]*';
        has_one_row := has_one_row OR
            plan_text LIKE '%Rows Emitted: 1%';
    END LOOP;

    IF NOT has_generic OR NOT has_component_count OR
       NOT has_component_ids OR NOT has_reduction_shape OR
       NOT has_ghd_mode OR NOT has_ghd_general OR
       NOT has_ghd_fallback OR
       NOT has_descriptor_source OR NOT has_reduction_core OR
       NOT has_ghd_bag_count OR NOT has_ghd_bag_details OR
       NOT has_ghd_separator_count OR NOT has_ghd_separator_details OR
       NOT has_ghd_descriptor_source OR
       NOT has_reduction_tail OR
       NOT has_separator_pass OR
       NOT has_leaf_tail_provider OR
       NOT has_descriptor_separators OR NOT has_separator_domain OR
       NOT has_core_pruning OR NOT has_provider_rows OR
       NOT has_key_only_tuple_skip OR
       NOT has_reduction_scratch_allocation OR
       NOT has_reduction_scratch_reuse OR NOT has_one_row THEN
        RAISE EXCEPTION
            'Generic Join GHD separator reduction/key-only/scratch reuse was not observed';
    END IF;
    RAISE NOTICE 'generic join GHD separator reduction, key-only materialization, and scratch reuse verified';
END
$generic_ghd_separator_plan$;

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

DO $generic_ghd_multi_tail_plan$
DECLARE
    plan_text text;
    has_generic boolean := false;
    has_component_count boolean := false;
    has_component_ids boolean := false;
    has_reduction_shape boolean := false;
    has_descriptor_source boolean := false;
    has_ghd_mode boolean := false;
    has_ghd_general boolean := false;
    has_ghd_fallback boolean := false;
    has_ghd_bag_count boolean := false;
    has_ghd_bag_details boolean := false;
    has_ghd_separator_count boolean := false;
    has_ghd_separator_details boolean := false;
    has_ghd_descriptor_source boolean := false;
    has_reduction_core boolean := false;
    has_reduction_tail boolean := false;
    has_separator_pass boolean := false;
    has_leaf_tail_providers boolean := false;
    has_descriptor_separators boolean := false;
    has_separator_domain boolean := false;
    has_core_pruning boolean := false;
    has_one_row boolean := false;
BEGIN
    PERFORM set_config('age.enable_wcoj', 'on', true);
    PERFORM set_config('enable_nestloop', 'off', true);
    PERFORM set_config('enable_hashjoin', 'off', true);
    PERFORM set_config('enable_mergejoin', 'off', true);

    FOR plan_text IN EXECUTE $plan$
        EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY OFF)
        SELECT *
        FROM cypher('generic_ghd', $cypher$
            MATCH (a:A)-[e1:E1]->(b:B)-[e2:E2]->(c:C)
                  -[e3:E3]->(d:D)-[e4:E4]->(a),
                  (c)-[tx:TX]->(x:X),
                  (a)-[ty:TY]->(y:Y)
            RETURN id(a), id(b), id(c), id(d), id(x), id(y),
                   id(e1), id(e2), id(e3), id(e4), id(tx), id(ty)
        $cypher$) AS (a agtype, b agtype, c agtype, d agtype,
                      x agtype, y agtype, e1 agtype, e2 agtype,
                      e3 agtype, e4 agtype, tx agtype, ty agtype)
    $plan$
    LOOP
        has_generic := has_generic OR
            plan_text LIKE '%Custom Scan (AGE Generic Multiway Join)%';
        has_component_count := has_component_count OR
            plan_text LIKE '%Component Count: 1%';
        has_component_ids := has_component_ids OR
            plan_text LIKE '%Component IDs: 1, 1, 1, 1, 1, 1%';
        has_reduction_shape := has_reduction_shape OR
            plan_text LIKE '%Reduction Shape: cyclic-with-tail%';
        has_ghd_mode := has_ghd_mode OR
            plan_text LIKE '%GHD Mode: general GHD%';
        has_ghd_general := has_ghd_general OR
            plan_text LIKE '%GHD General Decomposition: true%';
        has_ghd_fallback := has_ghd_fallback OR
            plan_text LIKE '%GHD Fallback Reason: none%';
        has_descriptor_source := has_descriptor_source OR
            plan_text LIKE
            '%Reduction Descriptor Source: graph-join-match-ir%';
        has_ghd_bag_count := has_ghd_bag_count OR
            plan_text LIKE '%GHD Bag Count: 4%';
        has_ghd_bag_details := has_ghd_bag_details OR
            plan_text LIKE
            '%GHD Bags: bag 1 cyclic-core:%bag 2 cyclic-core:%bag 3 leaf-tail:%bag 4 leaf-tail:%';
        has_ghd_separator_count := has_ghd_separator_count OR
            plan_text LIKE '%GHD Separator Count: 3%';
        has_ghd_separator_details := has_ghd_separator_details OR
            plan_text LIKE '%GHD Separators:%pair v%provider %provider %';
        has_ghd_descriptor_source := has_ghd_descriptor_source OR
            plan_text LIKE '%GHD Descriptor Source: graph-join-match-ir%';
        has_reduction_core := has_reduction_core OR
            plan_text LIKE '%Reduction Core Variables: 4%';
        has_reduction_tail := has_reduction_tail OR
            plan_text LIKE '%Reduction Tail Separators: 2%';
        has_separator_pass := has_separator_pass OR
            plan_text LIKE '%GHD Separator Reduction Passes: 2%';
        has_leaf_tail_providers := has_leaf_tail_providers OR
            plan_text LIKE '%GHD Leaf Tail Providers: 2%';
        has_descriptor_separators := has_descriptor_separators OR
            plan_text LIKE '%GHD Descriptor Separators Applied: 3%';
        has_separator_domain := has_separator_domain OR
            plan_text LIKE '%GHD Separator Domain Keys: 4%';
        has_core_pruning := has_core_pruning OR
            plan_text LIKE '%GHD Cyclic Core Rows Removed: 378%';
        has_one_row := has_one_row OR
            plan_text LIKE '%Rows Emitted: 1%';
    END LOOP;

    IF NOT has_generic OR NOT has_component_count OR
       NOT has_component_ids OR NOT has_reduction_shape OR
       NOT has_ghd_mode OR NOT has_ghd_general OR
       NOT has_ghd_fallback OR
       NOT has_descriptor_source OR NOT has_ghd_bag_count OR
       NOT has_ghd_bag_details OR NOT has_ghd_separator_count OR
       NOT has_ghd_separator_details OR NOT has_ghd_descriptor_source OR
       NOT has_reduction_core OR NOT has_reduction_tail OR
       NOT has_separator_pass OR NOT has_leaf_tail_providers OR
       NOT has_descriptor_separators OR NOT has_separator_domain OR
       NOT has_core_pruning OR
       NOT has_one_row THEN
        RAISE EXCEPTION
            'Generic Join multi-tail GHD separator reduction was not observed';
    END IF;
    RAISE NOTICE 'generic join multi-tail GHD separator reduction verified';
END
$generic_ghd_multi_tail_plan$;

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
 * Dense acyclic chains pass through the planner-owned graph-join match IR
 * summary before the Generic Join reduction descriptor reaches the executor.
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
SELECT _graphid(_label_id('generic_ir', 'T_CD'), (i - 1) * 80 + j),
       _graphid(_label_id('generic_ir', 'T_C'), i),
       _graphid(_label_id('generic_ir', 'T_D'), j),
       '{}'::agtype
FROM generate_series(1, 80) i,
     generate_series(1, 80) j;

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

DO $generic_ghd_acyclic_match_ir_plan$
DECLARE
    plan_text text;
    has_generic boolean := false;
    has_reduction_shape boolean := false;
    has_reduction_order boolean := false;
    has_descriptor_source boolean := false;
BEGIN
    PERFORM set_config('age.enable_wcoj', 'on', true);
    PERFORM set_config('enable_nestloop', 'off', true);
    PERFORM set_config('enable_hashjoin', 'off', true);
    PERFORM set_config('enable_mergejoin', 'off', true);

    FOR plan_text IN EXECUTE $plan$
        EXPLAIN (VERBOSE, COSTS OFF)
        SELECT *
        FROM cypher('generic_ir', $cypher$
            MATCH (a:T_A)-[ab:T_AB]->(b:T_B)-[bc:T_BC]->(c:T_C)
                  -[cd:T_CD]->(d:T_D)
            RETURN id(a), id(b), id(c), id(d), id(ab), id(bc), id(cd)
        $cypher$) AS (a agtype, b agtype, c agtype, d agtype,
                      ab agtype, bc agtype, cd agtype)
    $plan$
    LOOP
        has_generic := has_generic OR
            plan_text LIKE '%Custom Scan (AGE Generic Multiway Join)%';
        has_reduction_shape := has_reduction_shape OR
            plan_text LIKE '%Reduction Shape: alpha-acyclic%';
        has_reduction_order := has_reduction_order OR
            plan_text LIKE '%Reduction Order: leaf-peel%';
        has_descriptor_source := has_descriptor_source OR
            plan_text LIKE
            '%Reduction Descriptor Source: graph-join-match-ir%';
    END LOOP;

    IF NOT has_generic OR NOT has_reduction_shape OR
       NOT has_reduction_order OR NOT has_descriptor_source THEN
        RAISE EXCEPTION
            'Generic Join acyclic graph-join match IR descriptor source was not observed';
    END IF;
    RAISE NOTICE 'generic join acyclic graph-join match IR descriptor source verified';
END
$generic_ghd_acyclic_match_ir_plan$;

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

DO $generic_ghd_prefix_range_plan$
DECLARE
    plan_text text;
    matches text[];
    prefix_builds integer := NULL;
    prefix_reuses integer := NULL;
    prefix_cursor_reuses integer := NULL;
    trie_child_range_opens integer := NULL;
    trie_prefix_range_seeks integer := NULL;
    trie_pair_range_opens integer := NULL;
    has_generic boolean := false;
    has_on_demand_trie_ops boolean := false;
    has_rows boolean := false;
BEGIN
    PERFORM set_config('age.enable_wcoj', 'on', true);
    PERFORM set_config('enable_nestloop', 'off', true);
    PERFORM set_config('enable_hashjoin', 'off', true);
    PERFORM set_config('enable_mergejoin', 'off', true);

    FOR plan_text IN EXECUTE $plan$
        EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY OFF)
        SELECT *
        FROM cypher('generic_ghd', $cypher$
            MATCH (a:P_A)-[ab:PAB]->(b:P_B)-[bc:PBC]->(c:P_C)
                  -[cd:PCD]->(d:P_D),
                  (a)-[ad:PAD]->(d)
            RETURN id(a), id(b), id(c), id(d),
                   id(ab), id(bc), id(cd), id(ad)
        $cypher$) AS (a agtype, b agtype, c agtype, d agtype,
                      ab agtype, bc agtype, cd agtype, ad agtype)
    $plan$
    LOOP
        has_generic := has_generic OR
            plan_text LIKE '%Custom Scan (AGE Generic Multiway Join)%';
        has_on_demand_trie_ops := has_on_demand_trie_ops OR
            plan_text LIKE
            '%Provider Trie Ops: lazy sorted arrays with on-demand prefix directories%';
        has_rows := has_rows OR plan_text LIKE '%Rows Emitted: 48%';
        matches := regexp_match(plan_text, 'Prefix Range Builds: ([0-9]+)');
        IF matches IS NOT NULL THEN
            prefix_builds := matches[1]::integer;
        END IF;
        matches := regexp_match(plan_text, 'Prefix Range Reuses: ([0-9]+)');
        IF matches IS NOT NULL THEN
            prefix_reuses := matches[1]::integer;
        END IF;
        matches := regexp_match(plan_text,
                                'Prefix Range Cursor Reuses: ([0-9]+)');
        IF matches IS NOT NULL THEN
            prefix_cursor_reuses := matches[1]::integer;
        END IF;
        matches := regexp_match(plan_text,
                                'Trie Child Range Opens: ([0-9]+)');
        IF matches IS NOT NULL THEN
            trie_child_range_opens := matches[1]::integer;
        END IF;
        matches := regexp_match(plan_text,
                                'Trie Prefix Range Seeks: ([0-9]+)');
        IF matches IS NOT NULL THEN
            trie_prefix_range_seeks := matches[1]::integer;
        END IF;
        matches := regexp_match(plan_text,
                                'Trie Pair Range Opens: ([0-9]+)');
        IF matches IS NOT NULL THEN
            trie_pair_range_opens := matches[1]::integer;
        END IF;
    END LOOP;

    IF NOT has_generic OR NOT has_on_demand_trie_ops OR NOT has_rows OR
       prefix_builds IS NULL OR prefix_reuses IS NULL OR
       prefix_cursor_reuses IS NULL OR prefix_builds > 40 OR
       prefix_reuses < prefix_builds OR
       trie_child_range_opens IS NULL OR trie_prefix_range_seeks IS NULL OR
       trie_pair_range_opens IS NULL OR trie_child_range_opens <= 0 OR
       trie_prefix_range_seeks <= 0 OR trie_pair_range_opens <= 0 THEN
        RAISE EXCEPTION
            'Generic Join on-demand prefix range trie ops/directory/direct pair reuse was not observed (builds %, reuses %, cursor reuses %, child opens %, prefix seeks %, pair opens %)',
            prefix_builds, prefix_reuses, prefix_cursor_reuses,
            trie_child_range_opens, trie_prefix_range_seeks,
            trie_pair_range_opens;
    END IF;
    RAISE NOTICE 'generic join on-demand prefix range trie ops, directory, and direct pair reuse verified';
END
$generic_ghd_prefix_range_plan$;

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

DO $generic_ghd_deep_tail_plan$
DECLARE
    plan_text text;
    has_generic boolean := false;
    has_reduction_shape boolean := false;
    has_ghd_mode boolean := false;
    has_ghd_general boolean := false;
    has_ghd_fallback boolean := false;
    has_ghd_bag_count boolean := false;
    has_ghd_bag_details boolean := false;
    has_ghd_separator_count boolean := false;
    has_tail_domain_pass boolean := false;
    has_tail_domain_rows boolean := false;
    has_separator_domain boolean := false;
    has_core_pruning boolean := false;
    has_one_row boolean := false;
BEGIN
    PERFORM set_config('age.enable_wcoj', 'on', true);
    PERFORM set_config('enable_nestloop', 'off', true);
    PERFORM set_config('enable_hashjoin', 'off', true);
    PERFORM set_config('enable_mergejoin', 'off', true);

    FOR plan_text IN EXECUTE $plan$
        EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY OFF)
        SELECT *
        FROM cypher('generic_ghd', $cypher$
            MATCH (a:A)-[e1:E1]->(b:B)-[e2:E2]->(c:C)
                  -[e3:E3]->(d:D)-[e4:E4]->(a),
                  (c)-[tu:TU]->(u:U)-[tv:TV]->(v:V)-[tw:TW]->(w:W)
            RETURN id(a), id(b), id(c), id(d), id(u), id(v), id(w),
                   id(e1), id(e2), id(e3), id(e4), id(tu), id(tv), id(tw)
        $cypher$) AS (a agtype, b agtype, c agtype, d agtype,
                      u agtype, v agtype, w agtype, e1 agtype,
                      e2 agtype, e3 agtype, e4 agtype, tu agtype,
                      tv agtype, tw agtype)
    $plan$
    LOOP
        has_generic := has_generic OR
            plan_text LIKE '%Custom Scan (AGE Generic Multiway Join)%';
        has_reduction_shape := has_reduction_shape OR
            plan_text LIKE '%Reduction Shape: cyclic-with-tail%';
        has_ghd_mode := has_ghd_mode OR
            plan_text LIKE '%GHD Mode: general GHD%';
        has_ghd_general := has_ghd_general OR
            plan_text LIKE '%GHD General Decomposition: true%';
        has_ghd_fallback := has_ghd_fallback OR
            plan_text LIKE '%GHD Fallback Reason: none%';
        has_ghd_bag_count := has_ghd_bag_count OR
            plan_text LIKE '%GHD Bag Count: 3%';
        has_ghd_bag_details := has_ghd_bag_details OR
            plan_text LIKE
            '%GHD Bags: bag 1 cyclic-core:%bag 2 cyclic-core:%bag 3 leaf-tail:%';
        has_ghd_separator_count := has_ghd_separator_count OR
            plan_text LIKE '%GHD Separator Count: 2%';
        has_tail_domain_pass := has_tail_domain_pass OR
            plan_text ~ 'GHD Tail Domain Propagation Passes: [1-9][0-9]*';
        has_tail_domain_rows := has_tail_domain_rows OR
            plan_text ~ 'GHD Tail Domain Rows Removed: [1-9][0-9]*';
        has_separator_domain := has_separator_domain OR
            plan_text LIKE '%GHD Separator Domain Keys: 3%';
        has_core_pruning := has_core_pruning OR
            plan_text LIKE '%GHD Cyclic Core Rows Removed: 315%';
        has_one_row := has_one_row OR
            plan_text LIKE '%Rows Emitted: 1%';
    END LOOP;

    IF NOT has_generic OR NOT has_reduction_shape OR
       NOT has_ghd_mode OR NOT has_ghd_general OR
       NOT has_ghd_fallback OR
       NOT has_ghd_bag_count OR NOT has_ghd_bag_details OR
       NOT has_ghd_separator_count OR NOT has_tail_domain_pass OR
       NOT has_tail_domain_rows OR NOT has_separator_domain OR
       NOT has_core_pruning OR NOT has_one_row THEN
        RAISE EXCEPTION
            'Generic Join deep-tail GHD separator reduction was not observed';
    END IF;
    RAISE NOTICE 'generic join deep-tail GHD separator reduction verified';
END
$generic_ghd_deep_tail_plan$;

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
