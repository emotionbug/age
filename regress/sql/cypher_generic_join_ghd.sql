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

DO $generic_ghd_separator_plan$
DECLARE
    plan_text text;
    has_generic boolean := false;
    has_separator_pass boolean := false;
    has_leaf_tail_provider boolean := false;
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
        has_separator_pass := has_separator_pass OR
            plan_text LIKE '%GHD Separator Reduction Passes: 1%';
        has_leaf_tail_provider := has_leaf_tail_provider OR
            plan_text LIKE '%GHD Leaf Tail Providers: 1%';
        has_separator_domain := has_separator_domain OR
            plan_text LIKE '%GHD Separator Domain Keys: 1%';
        has_core_pruning := has_core_pruning OR
            plan_text LIKE '%GHD Cyclic Core Rows Removed: 189%';
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

    IF NOT has_generic OR NOT has_separator_pass OR
       NOT has_leaf_tail_provider OR NOT has_separator_domain OR
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

SELECT drop_graph('generic_ghd', true);
-- End cypher_generic_join_ghd.
