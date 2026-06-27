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
    has_component_count boolean := false;
    has_component_ids boolean := false;
    has_reduction_shape boolean := false;
    has_descriptor_source boolean := false;
    has_ghd_bag_count boolean := false;
    has_ghd_separator_count boolean := false;
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
        has_descriptor_source := has_descriptor_source OR
            plan_text LIKE
            '%Reduction Descriptor Source: graph-join-match-ir%';
        has_ghd_bag_count := has_ghd_bag_count OR
            plan_text LIKE '%GHD Bag Count: 2%';
        has_ghd_separator_count := has_ghd_separator_count OR
            plan_text LIKE '%GHD Separator Count: 1%';
        has_ghd_descriptor_source := has_ghd_descriptor_source OR
            plan_text LIKE '%GHD Descriptor Source: graph-join-match-ir%';
        has_reduction_core := has_reduction_core OR
            plan_text LIKE '%Reduction Core Variables: 4%';
        has_reduction_tail := has_reduction_tail OR
            plan_text LIKE '%Reduction Tail Separators: 1%';
        has_separator_pass := has_separator_pass OR
            plan_text LIKE '%GHD Separator Reduction Passes: 1%';
        has_leaf_tail_provider := has_leaf_tail_provider OR
            plan_text LIKE '%GHD Leaf Tail Providers: 1%';
        has_descriptor_separators := has_descriptor_separators OR
            plan_text LIKE '%GHD Descriptor Separators Applied: 1%';
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

    IF NOT has_generic OR NOT has_component_count OR
       NOT has_component_ids OR NOT has_reduction_shape OR
       NOT has_descriptor_source OR NOT has_reduction_core OR
       NOT has_ghd_bag_count OR NOT has_ghd_separator_count OR
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

DO $generic_ghd_multi_tail_plan$
DECLARE
    plan_text text;
    has_generic boolean := false;
    has_component_count boolean := false;
    has_component_ids boolean := false;
    has_reduction_shape boolean := false;
    has_descriptor_source boolean := false;
    has_ghd_bag_count boolean := false;
    has_ghd_separator_count boolean := false;
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
        has_descriptor_source := has_descriptor_source OR
            plan_text LIKE
            '%Reduction Descriptor Source: graph-join-match-ir%';
        has_ghd_bag_count := has_ghd_bag_count OR
            plan_text LIKE '%GHD Bag Count: 3%';
        has_ghd_separator_count := has_ghd_separator_count OR
            plan_text LIKE '%GHD Separator Count: 2%';
        has_ghd_descriptor_source := has_ghd_descriptor_source OR
            plan_text LIKE '%GHD Descriptor Source: graph-join-match-ir%';
        has_reduction_core := has_reduction_core OR
            plan_text LIKE '%Reduction Core Variables: 4%';
        has_reduction_tail := has_reduction_tail OR
            plan_text LIKE '%Reduction Tail Separators: 2%';
        has_separator_pass := has_separator_pass OR
            plan_text LIKE '%GHD Separator Reduction Passes: 1%';
        has_leaf_tail_providers := has_leaf_tail_providers OR
            plan_text LIKE '%GHD Leaf Tail Providers: 2%';
        has_descriptor_separators := has_descriptor_separators OR
            plan_text LIKE '%GHD Descriptor Separators Applied: 2%';
        has_separator_domain := has_separator_domain OR
            plan_text LIKE '%GHD Separator Domain Keys: 2%';
        has_core_pruning := has_core_pruning OR
            plan_text LIKE '%GHD Cyclic Core Rows Removed: 378%';
        has_one_row := has_one_row OR
            plan_text LIKE '%Rows Emitted: 1%';
    END LOOP;

    IF NOT has_generic OR NOT has_component_count OR
       NOT has_component_ids OR NOT has_reduction_shape OR
       NOT has_descriptor_source OR NOT has_ghd_bag_count OR
       NOT has_ghd_separator_count OR NOT has_ghd_descriptor_source OR
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

    IF NOT has_generic OR NOT has_rows OR
       prefix_builds IS NULL OR prefix_reuses IS NULL OR
       prefix_cursor_reuses IS NULL OR prefix_builds > 40 OR
       prefix_reuses < 300 OR prefix_cursor_reuses <= 0 OR
       trie_child_range_opens IS NULL OR trie_prefix_range_seeks IS NULL OR
       trie_pair_range_opens IS NULL OR trie_child_range_opens <= 0 OR
       trie_prefix_range_seeks <= 0 OR trie_pair_range_opens <= 0 THEN
        RAISE EXCEPTION
            'Generic Join prefix range trie ops/directory/cursor/direct pair reuse was not observed (builds %, reuses %, cursor reuses %, child opens %, prefix seeks %, pair opens %)',
            prefix_builds, prefix_reuses, prefix_cursor_reuses,
            trie_child_range_opens, trie_prefix_range_seeks,
            trie_pair_range_opens;
    END IF;
    RAISE NOTICE 'generic join prefix range trie ops, directory, cursor, and direct pair reuse verified';
END
$generic_ghd_prefix_range_plan$;

SELECT drop_graph('generic_ghd', true);
-- End cypher_generic_join_ghd.
