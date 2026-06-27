/*
 * Cyclic / fork fixed-MATCH semantics lock-in for the WCOJ-style multiway
 * intersection work (TODO item 6).  These results are the correctness contract
 * a future multiway-intersection executor MUST preserve: the binary-join plan
 * and a WCOJ plan have to return identical rows.  Scalars are returned (not
 * whole vertices) so the expected output is stable across runs.
 */
LOAD 'age';
SET search_path = ag_catalog, public;

SELECT create_graph('wcoj_semantics');
SELECT create_vlabel('wcoj_semantics', 'N');
SELECT create_elabel('wcoj_semantics', 'E');

-- Triangle 1->2->3->1, with a dangling tail 3->4 and a fork target 2->5.
SELECT * FROM cypher('wcoj_semantics', $$
    CREATE (a:N {id: 1})-[:E]->(b:N {id: 2})-[:E]->(c:N {id: 3})-[:E]->(a),
           (c)-[:E]->(:N {id: 4}),
           (b)-[:E]->(:N {id: 5})
$$) AS (z agtype);

-- Triangle: a directed 3-cycle closing back to the start variable enumerates
-- each rotation once (the cycle is matched from every vertex).
SELECT a, b, c FROM cypher('wcoj_semantics', $$
    MATCH (a:N)-[:E]->(b:N)-[:E]->(c:N)-[:E]->(a)
    RETURN a.id AS a, b.id AS b, c.id AS c
$$) AS (a agtype, b agtype, c agtype)
ORDER BY a, b, c;

-- Fork: one vertex with two outgoing edges yields the unordered pair of its
-- distinct targets (b.id < c.id de-duplicates the symmetric match).
SELECT a, b, c FROM cypher('wcoj_semantics', $$
    MATCH (b:N)<-[:E]-(a:N)-[:E]->(c:N)
    WHERE b.id < c.id
    RETURN a.id AS a, b.id AS b, c.id AS c
$$) AS (a agtype, b agtype, c agtype)
ORDER BY a, b, c;

-- Relationship uniqueness: within one MATCH the same edge may not be reused, so
-- a two-hop pattern never returns a path that traverses a single edge twice.
SELECT a, b, c FROM cypher('wcoj_semantics', $$
    MATCH (a:N)-[r1:E]->(b:N)-[r2:E]->(c:N)
    RETURN a.id AS a, b.id AS b, c.id AS c
$$) AS (a agtype, b agtype, c agtype)
ORDER BY a, b, c;

-- Diamond / two-path-between-endpoints: count the distinct intermediate fan.
SELECT total FROM cypher('wcoj_semantics', $$
    MATCH (a:N {id: 1})-[:E]->(b:N)-[:E]->(c:N)
    RETURN count(*) AS total
$$) AS (total agtype);

SELECT drop_graph('wcoj_semantics', true);

-- A second graph for the 4-cycle and diamond patterns the gate also calls out,
-- kept separate so the triangle/fork results above stay isolated.  A 4-cycle
-- 10->11->12->13->10 and a diamond where 1 reaches 4 by two distinct two-hop
-- paths (1->2->4 and 1->3->4).
SELECT create_graph('wcoj_semantics_more');
SELECT create_vlabel('wcoj_semantics_more', 'N');
SELECT create_elabel('wcoj_semantics_more', 'E');
SELECT * FROM cypher('wcoj_semantics_more', $$
    CREATE (n10:N {id: 10})-[:E]->(n11:N {id: 11})-[:E]->(n12:N {id: 12})
                          -[:E]->(n13:N {id: 13})-[:E]->(n10)
$$) AS (z agtype);
SELECT * FROM cypher('wcoj_semantics_more', $$
    CREATE (a:N {id: 1})-[:E]->(:N {id: 2}), (a)-[:E]->(:N {id: 3}),
           (:N {id: 4})
$$) AS (z agtype);
SELECT * FROM cypher('wcoj_semantics_more', $$
    MATCH (b:N), (d:N {id: 4})
    WHERE b.id IN [2, 3]
    CREATE (b)-[:E]->(d)
$$) AS (z agtype);

-- 4-cycle: a directed 4-cycle closing to the start variable enumerates each of
-- its four rotations once.
SELECT a, b, c, d FROM cypher('wcoj_semantics_more', $$
    MATCH (a:N)-[:E]->(b:N)-[:E]->(c:N)-[:E]->(e:N)-[:E]->(a)
    RETURN a.id AS a, b.id AS b, c.id AS c, e.id AS d
$$) AS (a agtype, b agtype, c agtype, d agtype)
ORDER BY a, b, c, d;

-- Diamond: two distinct two-hop paths from :id 1 to :id 4 (via 2 and via 3),
-- so the closed pattern returns both intermediates.
SELECT mid FROM cypher('wcoj_semantics_more', $$
    MATCH (a:N {id: 1})-[:E]->(m:N)-[:E]->(d:N {id: 4})
    RETURN m.id AS mid
$$) AS (mid agtype)
ORDER BY mid;

SELECT drop_graph('wcoj_semantics_more', true);

-- Planner lowering: a three-branch shared-terminal star must become one WCOJ
-- physical path automatically.  Uneven parallel-edge bags preserve Cypher bag
-- semantics, so the common terminal yields 2 * 3 * 4 = 24 rows.
SELECT create_graph('wcoj_lowering');
SELECT create_vlabel('wcoj_lowering', 'S');
SELECT create_vlabel('wcoj_lowering', 'T');
SELECT create_vlabel('wcoj_lowering', 'H');
SELECT create_elabel('wcoj_lowering', 'E');
SELECT * FROM cypher('wcoj_lowering', $$
    CREATE (s1:S {id: 1}), (s2:S {id: 2}), (s3:S {id: 3}),
           (t10:T {id: 10}),
           (:H {tag: 101, source_id: 1}),
           (:H {tag: 102, source_id: 1}),
           (:H {tag: 103, source_id: 1}),
           (s1)-[:E {keep: true, score: 10}]->(t10),
           (s1)-[:E {score: 10}]->(t10),
           (s2)-[:E {keep: true, score: 20}]->(t10),
           (s2)-[:E {keep: true, score: 20}]->(t10),
           (s2)-[:E {keep: false, score: 20}]->(t10),
           (s3)-[:E {keep: true, score: 30}]->(t10),
           (s3)-[:E {keep: true, score: 30}]->(t10),
           (s3)-[:E {keep: true, score: 30}]->(t10),
           (s3)-[:E {keep: false, score: 30}]->(t10)
$$) AS (z agtype);
CREATE INDEX wcoj_lowering_adj
ON wcoj_lowering."E" USING age_adjacency(start_id, id, end_id);
CREATE INDEX wcoj_lowering_pair
ON wcoj_lowering."E"(start_id, end_id);
ANALYZE wcoj_lowering."S";
ANALYZE wcoj_lowering."T";
ANALYZE wcoj_lowering."H";
ANALYZE wcoj_lowering."E";

DO $wcoj_lowering_plan$
DECLARE
    plan_text text;
    has_wcoj boolean := false;
    has_arity_four boolean := false;
    has_three_adjacency_providers boolean := false;
BEGIN
    FOR plan_text IN
        SELECT plan::text
        FROM cypher('wcoj_lowering', $$
            EXPLAIN (VERBOSE, COSTS OFF)
            MATCH (s1:S {id: 1})-[e1:E]->(t:T),
                  (s2:S {id: 2})-[e2:E]->(t),
                  (s3:S {id: 3})-[e3:E]->(t)
            RETURN t
        $$) AS (plan agtype)
    LOOP
        IF plan_text LIKE '%Custom Scan (AGE WCOJ Multiway Join)%' THEN
            has_wcoj := true;
        END IF;
        IF plan_text LIKE '%WCOJ Arity: 4%' THEN
            has_arity_four := true;
        END IF;
        IF plan_text LIKE '%Adjacency Providers: 3%' THEN
            has_three_adjacency_providers := true;
        END IF;
    END LOOP;

    IF NOT has_wcoj OR NOT has_arity_four OR
       NOT has_three_adjacency_providers THEN
        RAISE EXCEPTION 'expected arity-4 WCOJ with three adjacency providers';
    END IF;
END
$wcoj_lowering_plan$;

DO $wcoj_controls$
DECLARE
    plan_text text;
    engine_name text;
    has_wcoj boolean;
    has_planned boolean;
    has_actual boolean;
    has_no_fallback boolean;
BEGIN
    PERFORM set_config('age.enable_wcoj', 'off', true);
    has_wcoj := false;
    FOR plan_text IN
        SELECT plan::text
        FROM cypher('wcoj_lowering', $$
            EXPLAIN (VERBOSE, COSTS OFF)
            MATCH (s1:S {id: 1})-[e1:E]->(t:T),
                  (s2:S {id: 2})-[e2:E]->(t),
                  (s3:S {id: 3})-[e3:E]->(t)
            RETURN t
        $$) AS (plan agtype)
    LOOP
        has_wcoj := has_wcoj OR
            plan_text LIKE '%Custom Scan (AGE WCOJ Multiway Join)%';
    END LOOP;
    IF has_wcoj THEN
        RAISE EXCEPTION 'age.enable_wcoj=off still selected WCOJ';
    END IF;

    PERFORM set_config('age.enable_wcoj', 'on', true);
    FOREACH engine_name IN ARRAY ARRAY['leapfrog', 'merge', 'progressive']
    LOOP
        PERFORM set_config('age.wcoj_engine', engine_name, true);
        has_wcoj := false;
        has_planned := false;
        has_actual := false;
        has_no_fallback := false;

        /* Dynamic SQL forces replanning after each engine GUC change. */
        FOR plan_text IN EXECUTE $engine_plan$
            SELECT plan::text
            FROM cypher('wcoj_lowering', $cypher$
                EXPLAIN (VERBOSE, COSTS OFF)
                MATCH (s1:S {id: 1})-[e1:E]->(t:T),
                      (s2:S {id: 2})-[e2:E]->(t),
                      (s3:S {id: 3})-[e3:E]->(t)
                RETURN t
            $cypher$) AS (plan agtype)
        $engine_plan$
        LOOP
            has_wcoj := has_wcoj OR
                plan_text LIKE '%Custom Scan (AGE WCOJ Multiway Join)%';
            has_planned := has_planned OR
                plan_text LIKE format('%%Planned Engine: %s%%', engine_name);
            has_actual := has_actual OR
                plan_text LIKE format('%%Actual Engine: %s%%', engine_name);
            has_no_fallback := has_no_fallback OR
                plan_text LIKE '%Fallback Reason: none%';
        END LOOP;

        IF NOT has_wcoj OR NOT has_planned OR NOT has_actual OR
           NOT has_no_fallback
        THEN
            RAISE EXCEPTION 'invalid forced WCOJ engine plan for %',
                            engine_name;
        END IF;
    END LOOP;

    RAISE NOTICE 'wcoj controls and forced engines verified';
END
$wcoj_controls$;

-- Re-enter the same analyzed Cypher EXPLAIN through separate SPI portals.  The
-- graph-join lowering artifact must own its candidate strings and tables past
-- each PortalDrop; planner-local pointers here previously caused a UAF.
DO $wcoj_planner_lifetime$
DECLARE
    plan_text text;
    iteration integer;
BEGIN
    PERFORM set_config('age.enable_wcoj', 'on', true);
    PERFORM set_config('age.wcoj_engine', 'leapfrog', true);

    FOR iteration IN 1..3
    LOOP
        FOR plan_text IN
            SELECT plan::text
            FROM cypher('wcoj_lowering', $$
                EXPLAIN (VERBOSE, COSTS OFF)
                MATCH (s1:S {id: 1})-[e1:E]->(t:T),
                      (s2:S {id: 2})-[e2:E]->(t),
                      (s3:S {id: 3})-[e3:E]->(t)
                RETURN t
            $$) AS (plan agtype)
        LOOP
            NULL;
        END LOOP;
    END LOOP;

    RAISE NOTICE 'wcoj repeated planner lifetime verified';
END
$wcoj_planner_lifetime$;

-- Direct adjacency providers intersect terminal keys before fetching edge
-- payloads.  The edge-local property predicate leaves 1 * 2 * 3 combinations;
-- the source-side helper rows repeat one source graphid three times and must be
-- factorized to one intersection build while restoring all 72 bag rows.
DO $wcoj_direct_provider_telemetry$
DECLARE
    plan_text text;
    property_direct boolean := false;
    property_batched boolean := false;
    property_survivor_block boolean := false;
    property_payload_batches boolean := false;
    property_payload_qual_rejects boolean := false;
    property_rejects boolean := false;
    property_candidate_flat_rows boolean := false;
    property_combinations boolean := false;
    property_flat_rows_avoided boolean := false;
    factorized_source_rows boolean := false;
    factorized_keys boolean := false;
    factorized_source_bag_rows boolean := false;
    factorized_source_bag_keys boolean := false;
    factorized_binding_source_bags boolean := false;
    factorized_source_bag_bytes boolean := false;
    factorized_source_bag_reserve boolean := false;
    factorized_builds boolean := false;
    factorized_peak_factor_memory boolean := false;
    factorized_candidate_flat_rows boolean := false;
    factorized_combinations boolean := false;
    factorized_shared_enumerators boolean := false;
    factorized_shared_steps boolean := false;
    factorized_flat_rows_avoided boolean := false;
BEGIN
    PERFORM set_config('age.enable_wcoj', 'on', true);
    PERFORM set_config('age.wcoj_engine', 'leapfrog', true);
    PERFORM set_config('enable_nestloop', 'off', true);
    PERFORM set_config('enable_hashjoin', 'off', true);
    PERFORM set_config('enable_mergejoin', 'off', true);

    FOR plan_text IN EXECUTE $property_plan$
        EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY OFF)
        SELECT *
        FROM cypher('wcoj_lowering', $cypher$
            MATCH (s1:S {id: 1})-[e1:E]->(t:T),
                  (s2:S {id: 2})-[e2:E]->(t),
                  (s3:S {id: 3})-[e3:E]->(t)
            WHERE e1.keep = true AND e2.keep = true AND e3.keep = true
            RETURN id(e1), id(e2), id(e3), id(t)
        $cypher$) AS (eid1 agtype, eid2 agtype, eid3 agtype, tid agtype)
    $property_plan$
    LOOP
        property_direct := property_direct OR
            plan_text LIKE '%Adjacency Providers: 3%';
        property_batched := property_batched OR
            plan_text LIKE '%Batched Payload Materialization: true%';
        property_survivor_block := property_survivor_block OR
            plan_text LIKE '%Survivor Blocks: 1%';
        property_payload_batches := property_payload_batches OR
            plan_text LIKE '%Payload Scan Batches: 3%';
        property_payload_qual_rejects := property_payload_qual_rejects OR
            plan_text LIKE '%Payload Rows Rejected by Local Qual: 3%';
        property_rejects := property_rejects OR
            plan_text LIKE '%Local Predicate Rejects: 3%';
        property_candidate_flat_rows := property_candidate_flat_rows OR
            plan_text LIKE '%Candidate Flat Rows: 6%';
        property_combinations := property_combinations OR
            plan_text LIKE '%Candidate Bag Combinations: 6%';
        property_flat_rows_avoided := property_flat_rows_avoided OR
            plan_text LIKE '%Flat Rows Avoided: 0%';
    END LOOP;

    FOR plan_text IN EXECUTE $factorized_plan$
        EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY OFF)
        SELECT *
        FROM cypher('wcoj_lowering', $cypher$
            MATCH (h:H),
                  (s1:S)-[e1:E]->(t:T),
                  (s2:S {id: 2})-[e2:E]->(t),
                  (s3:S {id: 3})-[e3:E]->(t)
            WHERE h.source_id = s1.id
            RETURN h.tag, id(e1), id(e2), id(e3), id(t)
        $cypher$) AS (tag agtype, eid1 agtype, eid2 agtype,
                      eid3 agtype, tid agtype)
    $factorized_plan$
    LOOP
        factorized_source_rows := factorized_source_rows OR
            plan_text LIKE '%Source Rows Scanned: 5%';
        factorized_keys := factorized_keys OR
            plan_text LIKE '%Distinct Source Keys: 3%';
        factorized_source_bag_rows := factorized_source_bag_rows OR
            plan_text LIKE '%Source Bag Rows: 5%';
        factorized_source_bag_keys := factorized_source_bag_keys OR
            plan_text LIKE '%Source Bag Keys: 3%';
        factorized_binding_source_bags :=
            factorized_binding_source_bags OR
            plan_text LIKE '%Factorized Binding Source Bags: 3%';
        factorized_source_bag_bytes := factorized_source_bag_bytes OR
            plan_text ~ 'Source Bag Bytes: [1-9][0-9]* bytes';
        factorized_source_bag_reserve := factorized_source_bag_reserve OR
            plan_text ~ 'Source Bag Memory Reserve: [1-9][0-9]* bytes';
        factorized_builds := factorized_builds OR
            plan_text LIKE '%Intersection Builds: 3%';
        factorized_peak_factor_memory := factorized_peak_factor_memory OR
            plan_text ~ 'Peak Factor Memory: [1-9][0-9]* bytes';
        factorized_candidate_flat_rows := factorized_candidate_flat_rows OR
            plan_text LIKE '%Candidate Flat Rows: 72%';
        factorized_combinations := factorized_combinations OR
            plan_text LIKE '%Candidate Bag Combinations: 72%';
        factorized_shared_enumerators := factorized_shared_enumerators OR
            plan_text ~ 'Factorized Binding Enumerators: [1-9][0-9]*';
        factorized_shared_steps := factorized_shared_steps OR
            plan_text ~ 'Shared Factor Enumerator Steps: [1-9][0-9]*';
        factorized_flat_rows_avoided := factorized_flat_rows_avoided OR
            plan_text LIKE '%Flat Rows Avoided: 0%';
    END LOOP;

    IF NOT property_direct OR NOT property_batched OR
       NOT property_survivor_block OR NOT property_payload_batches OR
       NOT property_payload_qual_rejects OR NOT property_rejects OR
       NOT property_candidate_flat_rows OR NOT property_combinations OR
       NOT property_flat_rows_avoided THEN
        RAISE EXCEPTION 'direct provider did not filter edge payloads early';
    END IF;
    IF NOT factorized_source_rows OR NOT factorized_keys OR
       NOT factorized_source_bag_rows OR NOT factorized_source_bag_keys OR
       NOT factorized_binding_source_bags OR NOT factorized_source_bag_bytes OR
       NOT factorized_source_bag_reserve OR NOT factorized_builds OR
       NOT factorized_peak_factor_memory OR
       NOT factorized_candidate_flat_rows OR
       NOT factorized_combinations OR NOT factorized_shared_enumerators OR
       NOT factorized_shared_steps OR NOT factorized_flat_rows_avoided THEN
        RAISE EXCEPTION 'duplicate source-key factorization was not observed';
    END IF;

    RAISE NOTICE 'wcoj direct provider telemetry verified';
END
$wcoj_direct_provider_telemetry$;

BEGIN;
SET LOCAL age.enable_wcoj = off;
CREATE TEMP TABLE wcoj_property_binary ON COMMIT DROP AS
SELECT eid1, eid2, eid3, tid
FROM cypher('wcoj_lowering', $$
    MATCH (s1:S {id: 1})-[e1:E]->(t:T),
          (s2:S {id: 2})-[e2:E]->(t),
          (s3:S {id: 3})-[e3:E]->(t)
    WHERE e1.keep = true AND e2.keep = true AND e3.keep = true
    RETURN id(e1), id(e2), id(e3), id(t)
$$) AS (eid1 agtype, eid2 agtype, eid3 agtype, tid agtype);

SET LOCAL age.enable_wcoj = on;
SET LOCAL age.wcoj_engine = 'leapfrog';
SET LOCAL enable_nestloop = off;
SET LOCAL enable_hashjoin = off;
SET LOCAL enable_mergejoin = off;
CREATE TEMP TABLE wcoj_property_direct ON COMMIT DROP AS
SELECT eid1, eid2, eid3, tid
FROM cypher('wcoj_lowering', $$
    MATCH (s1:S {id: 1})-[e1:E]->(t:T),
          (s2:S {id: 2})-[e2:E]->(t),
          (s3:S {id: 3})-[e3:E]->(t)
    WHERE e1.keep = true AND e2.keep = true AND e3.keep = true
    RETURN id(e1), id(e2), id(e3), id(t)
$$) AS (eid1 agtype, eid2 agtype, eid3 agtype, tid agtype);

SELECT (SELECT count(*) FROM wcoj_property_binary) AS binary_rows,
       (SELECT count(*) FROM wcoj_property_direct) AS direct_rows,
       (SELECT count(*)
        FROM (
            (TABLE wcoj_property_binary
             EXCEPT ALL TABLE wcoj_property_direct)
            UNION ALL
            (TABLE wcoj_property_direct
             EXCEPT ALL TABLE wcoj_property_binary)
        ) diff) AS diff_rows;
ROLLBACK;

BEGIN;
SET LOCAL age.enable_wcoj = off;
CREATE TEMP TABLE wcoj_factor_binary ON COMMIT DROP AS
SELECT tag, eid1, eid2, eid3, tid
FROM cypher('wcoj_lowering', $$
    MATCH (h:H),
          (s1:S)-[e1:E]->(t:T),
          (s2:S {id: 2})-[e2:E]->(t),
          (s3:S {id: 3})-[e3:E]->(t)
    WHERE h.source_id = s1.id
    RETURN h.tag, id(e1), id(e2), id(e3), id(t)
$$) AS (tag agtype, eid1 agtype, eid2 agtype, eid3 agtype, tid agtype);

SET LOCAL age.enable_wcoj = on;
SET LOCAL age.wcoj_engine = 'leapfrog';
SET LOCAL enable_nestloop = off;
SET LOCAL enable_hashjoin = off;
SET LOCAL enable_mergejoin = off;
CREATE TEMP TABLE wcoj_factor_direct ON COMMIT DROP AS
SELECT tag, eid1, eid2, eid3, tid
FROM cypher('wcoj_lowering', $$
    MATCH (h:H),
          (s1:S)-[e1:E]->(t:T),
          (s2:S {id: 2})-[e2:E]->(t),
          (s3:S {id: 3})-[e3:E]->(t)
    WHERE h.source_id = s1.id
    RETURN h.tag, id(e1), id(e2), id(e3), id(t)
$$) AS (tag agtype, eid1 agtype, eid2 agtype, eid3 agtype, tid agtype);

SELECT (SELECT count(*) FROM wcoj_factor_binary) AS binary_rows,
       (SELECT count(*) FROM wcoj_factor_direct) AS direct_rows,
       (SELECT count(*)
        FROM (
            (TABLE wcoj_factor_binary EXCEPT ALL TABLE wcoj_factor_direct)
            UNION ALL
            (TABLE wcoj_factor_direct EXCEPT ALL TABLE wcoj_factor_binary)
        ) diff) AS diff_rows,
       (SELECT count(DISTINCT tag) FROM wcoj_factor_direct) AS source_bag_rows;
ROLLBACK;

BEGIN;
SET LOCAL age.enable_wcoj = off;
CREATE TEMP TABLE wcoj_binary_result ON COMMIT DROP AS
SELECT sid1, eid1, sid2, eid2, sid3, eid3, tid
FROM cypher('wcoj_lowering', $$
    MATCH (s1:S {id: 1})-[e1:E]->(t:T),
          (s2:S {id: 2})-[e2:E]->(t),
          (s3:S {id: 3})-[e3:E]->(t)
    RETURN id(s1), id(e1), id(s2), id(e2), id(s3), id(e3), id(t)
$$) AS (sid1 agtype, eid1 agtype, sid2 agtype, eid2 agtype,
        sid3 agtype, eid3 agtype, tid agtype);

SET LOCAL age.enable_wcoj = on;
SET LOCAL age.wcoj_engine = 'leapfrog';
CREATE TEMP TABLE wcoj_leapfrog_result ON COMMIT DROP AS
SELECT sid1, eid1, sid2, eid2, sid3, eid3, tid
FROM cypher('wcoj_lowering', $$
    MATCH (s1:S {id: 1})-[e1:E]->(t:T),
          (s2:S {id: 2})-[e2:E]->(t),
          (s3:S {id: 3})-[e3:E]->(t)
    RETURN id(s1), id(e1), id(s2), id(e2), id(s3), id(e3), id(t)
$$) AS (sid1 agtype, eid1 agtype, sid2 agtype, eid2 agtype,
        sid3 agtype, eid3 agtype, tid agtype);

SET LOCAL age.wcoj_engine = 'merge';
CREATE TEMP TABLE wcoj_merge_result ON COMMIT DROP AS
SELECT sid1, eid1, sid2, eid2, sid3, eid3, tid
FROM cypher('wcoj_lowering', $$
    MATCH (s1:S {id: 1})-[e1:E]->(t:T),
          (s2:S {id: 2})-[e2:E]->(t),
          (s3:S {id: 3})-[e3:E]->(t)
    RETURN id(s1), id(e1), id(s2), id(e2), id(s3), id(e3), id(t)
$$) AS (sid1 agtype, eid1 agtype, sid2 agtype, eid2 agtype,
        sid3 agtype, eid3 agtype, tid agtype);

SET LOCAL age.wcoj_engine = 'progressive';
CREATE TEMP TABLE wcoj_progressive_result ON COMMIT DROP AS
SELECT sid1, eid1, sid2, eid2, sid3, eid3, tid
FROM cypher('wcoj_lowering', $$
    MATCH (s1:S {id: 1})-[e1:E]->(t:T),
          (s2:S {id: 2})-[e2:E]->(t),
          (s3:S {id: 3})-[e3:E]->(t)
    RETURN id(s1), id(e1), id(s2), id(e2), id(s3), id(e3), id(t)
$$) AS (sid1 agtype, eid1 agtype, sid2 agtype, eid2 agtype,
        sid3 agtype, eid3 agtype, tid agtype);

SELECT 'leapfrog'::text AS engine,
       (SELECT count(*)
        FROM (
            (TABLE wcoj_binary_result EXCEPT ALL TABLE wcoj_leapfrog_result)
            UNION ALL
            (TABLE wcoj_leapfrog_result EXCEPT ALL TABLE wcoj_binary_result)
        ) diff) AS diff_rows
UNION ALL
SELECT 'merge'::text,
       (SELECT count(*)
        FROM (
            (TABLE wcoj_binary_result EXCEPT ALL TABLE wcoj_merge_result)
            UNION ALL
            (TABLE wcoj_merge_result EXCEPT ALL TABLE wcoj_binary_result)
        ) diff)
UNION ALL
SELECT 'progressive'::text,
       (SELECT count(*)
        FROM (
            (TABLE wcoj_binary_result EXCEPT ALL TABLE wcoj_progressive_result)
            UNION ALL
            (TABLE wcoj_progressive_result EXCEPT ALL TABLE wcoj_binary_result)
        ) diff)
ORDER BY engine;
ROLLBACK;

SELECT total FROM cypher('wcoj_lowering', $$
    MATCH (s1:S {id: 1})-[e1:E]->(t:T),
          (s2:S {id: 2})-[e2:E]->(t),
          (s3:S {id: 3})-[e3:E]->(t)
    RETURN count(*) AS total
$$) AS (total agtype);

DO $wcoj_count_consumer_telemetry$
DECLARE
    plan_text text;
    has_wcoj boolean := false;
    has_arity_four boolean := false;
    has_three_adjacency_providers boolean := false;
    has_count_consumer boolean := false;
    has_count_result boolean := false;
    has_flat_rows_avoided boolean := false;
    has_one_row_emitted boolean := false;
BEGIN
    PERFORM set_config('age.enable_wcoj', 'on', true);
    PERFORM set_config('age.wcoj_engine', 'merge', true);
    PERFORM set_config('enable_nestloop', 'off', true);
    PERFORM set_config('enable_hashjoin', 'off', true);
    PERFORM set_config('enable_mergejoin', 'off', true);

    FOR plan_text IN
        SELECT plan::text
        FROM cypher('wcoj_lowering', $$
            EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY OFF)
            MATCH (s1:S {id: 1})-[e1:E]->(t:T),
                  (s2:S {id: 2})-[e2:E]->(t),
                  (s3:S {id: 3})-[e3:E]->(t)
            RETURN count(*) AS total
        $$) AS (plan agtype)
    LOOP
        has_wcoj := has_wcoj OR
            plan_text LIKE '%Custom Scan (AGE WCOJ Multiway Join)%';
        has_arity_four := has_arity_four OR
            plan_text LIKE '%WCOJ Arity: 4%';
        has_three_adjacency_providers :=
            has_three_adjacency_providers OR
            plan_text LIKE '%Adjacency Providers: 3%';
        has_count_consumer := has_count_consumer OR
            plan_text LIKE '%WCOJ Consumer: count(*)%';
        has_count_result := has_count_result OR
            plan_text LIKE '%Count Result: 24%';
        has_flat_rows_avoided := has_flat_rows_avoided OR
            plan_text LIKE '%Consumer Flat Rows Avoided: 24%';
        has_one_row_emitted := has_one_row_emitted OR
            plan_text LIKE '%Rows Emitted: 1%';
    END LOOP;

    IF NOT has_wcoj OR NOT has_arity_four OR
       NOT has_three_adjacency_providers OR NOT has_count_consumer OR
       NOT has_count_result OR NOT has_flat_rows_avoided OR
       NOT has_one_row_emitted THEN
        RAISE EXCEPTION 'WCOJ count consumer telemetry was not observed';
    END IF;
    RAISE NOTICE 'wcoj count consumer telemetry verified';
END
$wcoj_count_consumer_telemetry$;

SELECT total FROM cypher('wcoj_lowering', $$
    MATCH (s1:S {id: 1})-[e1:E]->(t:T),
          (s2:S {id: 2})-[e2:E]->(t),
          (s3:S {id: 3})-[e3:E]->(t)
    RETURN sum(e1.score) AS total
$$) AS (total agtype);

DO $wcoj_sum_property_consumer_telemetry$
DECLARE
    plan_text text;
    has_wcoj boolean := false;
    has_sum_consumer boolean := false;
    has_batched_payload boolean := false;
    has_flat_rows_avoided boolean := false;
    has_sum_input_rows boolean := false;
    has_one_row_emitted boolean := false;
BEGIN
    PERFORM set_config('age.enable_wcoj', 'on', true);
    PERFORM set_config('age.wcoj_engine', 'merge', true);
    PERFORM set_config('enable_nestloop', 'off', true);
    PERFORM set_config('enable_hashjoin', 'off', true);
    PERFORM set_config('enable_mergejoin', 'off', true);

    FOR plan_text IN
        SELECT plan::text
        FROM cypher('wcoj_lowering', $$
            EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY OFF)
            MATCH (s1:S {id: 1})-[e1:E]->(t:T),
                  (s2:S {id: 2})-[e2:E]->(t),
                  (s3:S {id: 3})-[e3:E]->(t)
            RETURN sum(e1.score) AS total
        $$) AS (plan agtype)
    LOOP
        has_wcoj := has_wcoj OR
            plan_text LIKE '%Custom Scan (AGE WCOJ Multiway Join)%';
        has_sum_consumer := has_sum_consumer OR
            plan_text LIKE '%WCOJ Consumer: sum(property)%';
        has_batched_payload := has_batched_payload OR
            plan_text LIKE '%Batched Payload Materialization: true%';
        has_flat_rows_avoided := has_flat_rows_avoided OR
            plan_text LIKE '%Consumer Flat Rows Avoided: 24%';
        has_sum_input_rows := has_sum_input_rows OR
            plan_text LIKE '%Sum Property Input Rows: 24%';
        has_one_row_emitted := has_one_row_emitted OR
            plan_text LIKE '%Rows Emitted: 1%';
    END LOOP;

    IF NOT has_wcoj OR NOT has_sum_consumer OR
       NOT has_batched_payload OR NOT has_flat_rows_avoided OR
       NOT has_sum_input_rows OR NOT has_one_row_emitted THEN
        RAISE EXCEPTION
            'WCOJ sum-property consumer telemetry was not observed';
    END IF;
    RAISE NOTICE 'wcoj sum-property consumer telemetry verified';
END
$wcoj_sum_property_consumer_telemetry$;

BEGIN;
SET LOCAL age.enable_wcoj = off;
CREATE TEMP TABLE wcoj_sum_property_binary ON COMMIT DROP AS
SELECT total
FROM cypher('wcoj_lowering', $$
    MATCH (s1:S {id: 1})-[e1:E]->(t:T),
          (s2:S {id: 2})-[e2:E]->(t),
          (s3:S {id: 3})-[e3:E]->(t)
    RETURN sum(e1.score) AS total
$$) AS (total agtype);

SET LOCAL age.enable_wcoj = on;
SET LOCAL age.wcoj_engine = 'merge';
SET LOCAL enable_nestloop = off;
SET LOCAL enable_hashjoin = off;
SET LOCAL enable_mergejoin = off;
CREATE TEMP TABLE wcoj_sum_property_direct ON COMMIT DROP AS
SELECT total
FROM cypher('wcoj_lowering', $$
    MATCH (s1:S {id: 1})-[e1:E]->(t:T),
          (s2:S {id: 2})-[e2:E]->(t),
          (s3:S {id: 3})-[e3:E]->(t)
    RETURN sum(e1.score) AS total
$$) AS (total agtype);

SELECT (SELECT total FROM wcoj_sum_property_binary) AS binary_total,
       (SELECT total FROM wcoj_sum_property_direct) AS direct_total,
       (SELECT count(*)
        FROM (
            (TABLE wcoj_sum_property_binary
             EXCEPT ALL TABLE wcoj_sum_property_direct)
            UNION ALL
            (TABLE wcoj_sum_property_direct
             EXCEPT ALL TABLE wcoj_sum_property_binary)
        ) diff) AS diff_rows;
ROLLBACK;

SELECT key, total FROM cypher('wcoj_lowering', $$
    MATCH (s1:S {id: 1})-[e1:E]->(t:T),
          (s2:S {id: 2})-[e2:E]->(t),
          (s3:S {id: 3})-[e3:E]->(t)
    RETURN id(t) AS key, sum(e1.score) AS total
$$) AS (key agtype, total agtype)
ORDER BY key;

DO $wcoj_group_sum_property_consumer_telemetry$
DECLARE
    plan_text text;
    has_wcoj boolean := false;
    has_group_sum_consumer boolean := false;
    has_batched_payload boolean := false;
    has_flat_rows_avoided boolean := false;
    has_sum_provider boolean := false;
    has_sum_input_rows boolean := false;
    has_one_row_emitted boolean := false;
BEGIN
    PERFORM set_config('age.enable_wcoj', 'on', true);
    PERFORM set_config('age.wcoj_engine', 'merge', true);
    PERFORM set_config('enable_nestloop', 'off', true);
    PERFORM set_config('enable_hashjoin', 'off', true);
    PERFORM set_config('enable_mergejoin', 'off', true);

    FOR plan_text IN EXECUTE $plan$
        EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY OFF)
        SELECT *
        FROM cypher('wcoj_lowering', $cypher$
            MATCH (s1:S {id: 1})-[e1:E]->(t:T),
                  (s2:S {id: 2})-[e2:E]->(t),
                  (s3:S {id: 3})-[e3:E]->(t)
            RETURN id(t) AS key, sum(e1.score) AS total
        $cypher$) AS (key agtype, total agtype)
    $plan$
    LOOP
        has_wcoj := has_wcoj OR
            plan_text LIKE '%Custom Scan (AGE WCOJ Multiway Join)%';
        has_group_sum_consumer := has_group_sum_consumer OR
            plan_text LIKE '%WCOJ Consumer: group sum(property)%';
        has_batched_payload := has_batched_payload OR
            plan_text LIKE '%Batched Payload Materialization: true%';
        has_flat_rows_avoided := has_flat_rows_avoided OR
            plan_text LIKE '%Consumer Flat Rows Avoided: 24%';
        has_sum_provider := has_sum_provider OR
            plan_text LIKE '%Sum Property Provider: 1%';
        has_sum_input_rows := has_sum_input_rows OR
            plan_text LIKE '%Sum Property Input Rows: 24%';
        has_one_row_emitted := has_one_row_emitted OR
            plan_text LIKE '%Rows Emitted: 1%';
    END LOOP;

    IF NOT has_wcoj OR NOT has_group_sum_consumer OR
       NOT has_batched_payload OR NOT has_flat_rows_avoided OR
       NOT has_sum_provider OR NOT has_sum_input_rows OR
       NOT has_one_row_emitted THEN
        RAISE EXCEPTION
            'WCOJ group sum-property consumer telemetry was not observed';
    END IF;
    RAISE NOTICE 'wcoj group sum-property consumer telemetry verified';
END
$wcoj_group_sum_property_consumer_telemetry$;

BEGIN;
SET LOCAL age.enable_wcoj = off;
CREATE TEMP TABLE wcoj_group_sum_property_binary ON COMMIT DROP AS
SELECT key, total
FROM cypher('wcoj_lowering', $$
    MATCH (s1:S {id: 1})-[e1:E]->(t:T),
          (s2:S {id: 2})-[e2:E]->(t),
          (s3:S {id: 3})-[e3:E]->(t)
    RETURN id(t) AS key, sum(e1.score) AS total
$$) AS (key agtype, total agtype);

SET LOCAL age.enable_wcoj = on;
SET LOCAL age.wcoj_engine = 'merge';
SET LOCAL enable_nestloop = off;
SET LOCAL enable_hashjoin = off;
SET LOCAL enable_mergejoin = off;
CREATE TEMP TABLE wcoj_group_sum_property_direct ON COMMIT DROP AS
SELECT key, total
FROM cypher('wcoj_lowering', $$
    MATCH (s1:S {id: 1})-[e1:E]->(t:T),
          (s2:S {id: 2})-[e2:E]->(t),
          (s3:S {id: 3})-[e3:E]->(t)
    RETURN id(t) AS key, sum(e1.score) AS total
$$) AS (key agtype, total agtype);

SELECT (SELECT count(*) FROM wcoj_group_sum_property_binary) AS binary_rows,
       (SELECT count(*) FROM wcoj_group_sum_property_direct) AS direct_rows,
       (SELECT count(*)
        FROM (
            (TABLE wcoj_group_sum_property_binary
             EXCEPT ALL TABLE wcoj_group_sum_property_direct)
            UNION ALL
            (TABLE wcoj_group_sum_property_direct
             EXCEPT ALL TABLE wcoj_group_sum_property_binary)
        ) diff) AS diff_rows;
ROLLBACK;

DO $wcoj_group_sum_property_filter_fallback$
DECLARE
    plan_text text;
    has_group_sum_consumer boolean := false;
BEGIN
    PERFORM set_config('age.enable_wcoj', 'on', true);
    PERFORM set_config('age.wcoj_engine', 'merge', true);
    PERFORM set_config('enable_nestloop', 'off', true);
    PERFORM set_config('enable_hashjoin', 'off', true);
    PERFORM set_config('enable_mergejoin', 'off', true);

    FOR plan_text IN EXECUTE $plan$
        EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY OFF)
        SELECT *
        FROM cypher('wcoj_lowering', $cypher$
            MATCH (s1:S {id: 1})-[e1:E]->(t:T),
                  (s2:S {id: 2})-[e2:E]->(t),
                  (s3:S {id: 3})-[e3:E]->(t)
            WHERE e1.score >= 10
            RETURN id(t) AS key, sum(e1.score) AS total
        $cypher$) AS (key agtype, total agtype)
    $plan$
    LOOP
        has_group_sum_consumer := has_group_sum_consumer OR
            plan_text LIKE '%WCOJ Consumer: group sum(property)%';
    END LOOP;

    IF has_group_sum_consumer THEN
        RAISE EXCEPTION
            'WCOJ group sum-property consumer was selected for filtered input';
    END IF;
    RAISE NOTICE 'wcoj group sum-property filtered fallback verified';
END
$wcoj_group_sum_property_filter_fallback$;

SELECT total FROM cypher('wcoj_lowering', $$
    MATCH (s1:S {id: 1})-[e1:E]->(t:T),
          (s2:S {id: 2})-[e2:E]->(t),
          (s3:S {id: 3})-[e3:E]->(t)
    RETURN count(DISTINCT id(t)) AS total
$$) AS (total agtype);

SELECT key, total FROM cypher('wcoj_lowering', $$
    MATCH (s1:S {id: 1})-[e1:E]->(t:T),
          (s2:S {id: 2})-[e2:E]->(t),
          (s3:S {id: 3})-[e3:E]->(t)
    RETURN id(t) AS key, count(*) AS total
$$) AS (key agtype, total agtype)
ORDER BY key;

DO $wcoj_group_count_consumer_telemetry$
DECLARE
    plan_text text;
    has_wcoj boolean := false;
    has_group_consumer boolean := false;
    has_count_result boolean := false;
    has_flat_rows_avoided boolean := false;
    has_one_row_emitted boolean := false;
BEGIN
    PERFORM set_config('age.enable_wcoj', 'on', true);
    PERFORM set_config('age.wcoj_engine', 'merge', true);
    PERFORM set_config('enable_nestloop', 'off', true);
    PERFORM set_config('enable_hashjoin', 'off', true);
    PERFORM set_config('enable_mergejoin', 'off', true);

    FOR plan_text IN EXECUTE $plan$
        EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY OFF)
        SELECT *
        FROM cypher('wcoj_lowering', $cypher$
            MATCH (s1:S {id: 1})-[e1:E]->(t:T),
                  (s2:S {id: 2})-[e2:E]->(t),
                  (s3:S {id: 3})-[e3:E]->(t)
            RETURN id(t) AS key, count(*) AS total
        $cypher$) AS (key agtype, total agtype)
    $plan$
    LOOP
        has_wcoj := has_wcoj OR
            plan_text LIKE '%Custom Scan (AGE WCOJ Multiway Join)%';
        has_group_consumer := has_group_consumer OR
            plan_text LIKE '%WCOJ Consumer: group count(key)%';
        has_count_result := has_count_result OR
            plan_text LIKE '%Count Result: 24%';
        has_flat_rows_avoided := has_flat_rows_avoided OR
            plan_text LIKE '%Consumer Flat Rows Avoided: 24%';
        has_one_row_emitted := has_one_row_emitted OR
            plan_text LIKE '%Rows Emitted: 1%';
    END LOOP;

    IF NOT has_wcoj OR NOT has_group_consumer OR
       NOT has_count_result OR NOT has_flat_rows_avoided OR
       NOT has_one_row_emitted THEN
        RAISE EXCEPTION
            'WCOJ group-count consumer telemetry was not observed';
    END IF;
    RAISE NOTICE 'wcoj group-count consumer telemetry verified';
END
$wcoj_group_count_consumer_telemetry$;

DO $wcoj_count_distinct_key_consumer_telemetry$
DECLARE
    plan_text text;
    has_wcoj boolean := false;
    has_distinct_consumer boolean := false;
    has_count_result boolean := false;
    has_flat_rows_avoided boolean := false;
    has_one_row_emitted boolean := false;
BEGIN
    PERFORM set_config('age.enable_wcoj', 'on', true);
    PERFORM set_config('age.wcoj_engine', 'merge', true);
    PERFORM set_config('enable_nestloop', 'off', true);
    PERFORM set_config('enable_hashjoin', 'off', true);
    PERFORM set_config('enable_mergejoin', 'off', true);

    FOR plan_text IN
        SELECT plan::text
        FROM cypher('wcoj_lowering', $$
            EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY OFF)
            MATCH (s1:S {id: 1})-[e1:E]->(t:T),
                  (s2:S {id: 2})-[e2:E]->(t),
                  (s3:S {id: 3})-[e3:E]->(t)
            RETURN count(DISTINCT id(t)) AS total
        $$) AS (plan agtype)
    LOOP
        has_wcoj := has_wcoj OR
            plan_text LIKE '%Custom Scan (AGE WCOJ Multiway Join)%';
        has_distinct_consumer := has_distinct_consumer OR
            plan_text LIKE '%WCOJ Consumer: count(distinct key)%';
        has_count_result := has_count_result OR
            plan_text LIKE '%Count Result: 1%';
        has_flat_rows_avoided := has_flat_rows_avoided OR
            plan_text LIKE '%Consumer Flat Rows Avoided: 24%';
        has_one_row_emitted := has_one_row_emitted OR
            plan_text LIKE '%Rows Emitted: 1%';
    END LOOP;

    IF NOT has_wcoj OR NOT has_distinct_consumer OR
       NOT has_count_result OR NOT has_flat_rows_avoided OR
       NOT has_one_row_emitted THEN
        RAISE EXCEPTION
            'WCOJ count distinct-key consumer telemetry was not observed';
    END IF;
    RAISE NOTICE 'wcoj count distinct-key consumer telemetry verified';
END
$wcoj_count_distinct_key_consumer_telemetry$;

DO $wcoj_exists_factorized_row_goal_telemetry$
DECLARE
    plan_text text;
    has_result boolean := false;
    has_initplan boolean := false;
    has_wcoj boolean := false;
    has_exists_consumer boolean := false;
    has_row_goal boolean := false;
    has_row_goal_source boolean := false;
    has_candidate_flat_rows boolean := false;
    has_candidate_combinations boolean := false;
    has_row_goal_rows_emitted boolean := false;
    has_row_goal_flat_rows_avoided boolean := false;
    has_exists_result boolean := false;
    has_goal_reached boolean := false;
BEGIN
    PERFORM set_config('age.enable_wcoj', 'on', true);
    PERFORM set_config('age.wcoj_engine', 'merge', true);
    PERFORM set_config('enable_nestloop', 'off', true);
    PERFORM set_config('enable_hashjoin', 'off', true);
    PERFORM set_config('enable_mergejoin', 'off', true);

    FOR plan_text IN EXECUTE $plan$
        EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY OFF)
        SELECT *
        FROM cypher('wcoj_lowering', $cypher$
            RETURN EXISTS {
                MATCH (s1:S {id: 1})-[e1:E]->(t:T),
                      (s2:S {id: 2})-[e2:E]->(t),
                      (s3:S {id: 3})-[e3:E]->(t)
                RETURN t
            } AS ok
        $cypher$) AS (ok agtype)
    $plan$
    LOOP
        has_result := has_result OR plan_text LIKE '%Result%';
        has_initplan := has_initplan OR plan_text LIKE '%InitPlan%';
        has_wcoj := has_wcoj OR
            plan_text LIKE '%Custom Scan (AGE WCOJ Multiway Join)%';
        has_exists_consumer := has_exists_consumer OR
            plan_text LIKE '%WCOJ Consumer: exists%';
        has_row_goal := has_row_goal OR
            plan_text LIKE '%WCOJ Row Goal: 1%';
        has_row_goal_source := has_row_goal_source OR
            plan_text LIKE '%WCOJ Row Goal Source: exists%';
        has_candidate_flat_rows := has_candidate_flat_rows OR
            plan_text LIKE '%Candidate Flat Rows: 24%';
        has_candidate_combinations := has_candidate_combinations OR
            plan_text LIKE '%Candidate Bag Combinations: 1%';
        has_row_goal_rows_emitted := has_row_goal_rows_emitted OR
            plan_text LIKE '%Row Goal Rows Emitted: 1%';
        has_row_goal_flat_rows_avoided :=
            has_row_goal_flat_rows_avoided OR
            plan_text LIKE '%Row Goal Flat Rows Avoided: 23%';
        has_exists_result := has_exists_result OR
            plan_text LIKE '%Exists Result: true%';
        has_goal_reached := has_goal_reached OR
            plan_text LIKE '%Row Goal Reached: true%';
    END LOOP;

    IF NOT has_result OR NOT has_initplan OR NOT has_wcoj OR
       NOT has_exists_consumer OR NOT has_row_goal OR
       NOT has_row_goal_source OR NOT has_candidate_flat_rows OR
       NOT has_candidate_combinations OR NOT has_row_goal_rows_emitted OR
       NOT has_row_goal_flat_rows_avoided OR NOT has_exists_result OR
       NOT has_goal_reached THEN
        RAISE EXCEPTION
            'WCOJ factorized exists row-goal telemetry was not observed';
    END IF;
    RAISE NOTICE 'wcoj factorized exists row-goal telemetry verified';
END
$wcoj_exists_factorized_row_goal_telemetry$;

-- Add a second star after the automatic-plan assertion.  Its first two
-- branches meet at one terminal while the third reaches another.
SELECT * FROM cypher('wcoj_lowering', $$
    CREATE (s4:S {id: 4}), (s5:S {id: 5}), (s6:S {id: 6}),
           (t11:T {id: 11}), (t12:T {id: 12}),
           (s4)-[:E]->(t11), (s4)-[:E]->(t11),
           (s5)-[:E]->(t11), (s5)-[:E]->(t11),
           (s6)-[:E]->(t12), (s6)-[:E]->(t12)
$$) AS (z agtype);
ANALYZE wcoj_lowering."S";
ANALYZE wcoj_lowering."T";
ANALYZE wcoj_lowering."E";

-- The third stream is disjoint.  The delayed bag product must therefore emit
-- no combinations even though the first two branches share a terminal.
SELECT total FROM cypher('wcoj_lowering', $$
    MATCH (s4:S {id: 4})-[e4:E]->(t:T),
          (s5:S {id: 5})-[e5:E]->(t),
          (s6:S {id: 6})-[e6:E]->(t)
    RETURN count(*) AS total
$$) AS (total agtype);

-- A dense direct-provider star exercises survivor-block payload retrieval with
-- more than one terminal.  Three source postings contain the same 17
-- terminals, so batching replaces 51 survivor-local scan restarts with three
-- tagged source-key scans while preserving one output row per terminal.
INSERT INTO wcoj_lowering."S" (id, properties)
SELECT _graphid(_label_id('wcoj_lowering', 'S'), source_no),
       format('{"id":%s}', source_no)::agtype
FROM generate_series(7, 9) source_no;
INSERT INTO wcoj_lowering."T" (id, properties)
SELECT _graphid(_label_id('wcoj_lowering', 'T'), target_no), '{}'::agtype
FROM generate_series(100, 116) target_no;
INSERT INTO wcoj_lowering."E" (id, start_id, end_id, properties)
SELECT _graphid(_label_id('wcoj_lowering', 'E'),
                10000 + (source_no - 7) * 17 + (target_no - 100)),
       _graphid(_label_id('wcoj_lowering', 'S'), source_no),
       _graphid(_label_id('wcoj_lowering', 'T'), target_no),
       '{}'::agtype
FROM generate_series(7, 9) source_no,
     generate_series(100, 116) target_no;
ANALYZE wcoj_lowering."S";
ANALYZE wcoj_lowering."T";
ANALYZE wcoj_lowering."E";

SELECT key, total FROM cypher('wcoj_lowering', $$
    MATCH (s7:S {id: 7})-[e7:E]->(t:T),
          (s8:S {id: 8})-[e8:E]->(t),
          (s9:S {id: 9})-[e9:E]->(t)
    RETURN id(t) AS key, count(*) AS total
$$) AS (key agtype, total agtype)
ORDER BY key;

DO $wcoj_survivor_batch_telemetry$
DECLARE
    plan_text text;
    has_one_block boolean := false;
    has_three_batches boolean := false;
    has_zero_restarts boolean := false;
    has_three_source_keys boolean := false;
    has_three_distinct_source_keys boolean := false;
    has_forty_eight_restarts_avoided boolean := false;
    has_fifty_one_payload_rows_scanned boolean := false;
    has_fifty_one_payload_rows boolean := false;
    has_seventeen_candidate_flat_rows boolean := false;
    has_zero_flat_rows_avoided boolean := false;
BEGIN
    PERFORM set_config('age.enable_wcoj', 'on', true);
    PERFORM set_config('age.wcoj_engine', 'merge', true);
    PERFORM set_config('enable_nestloop', 'off', true);
    PERFORM set_config('enable_hashjoin', 'off', true);
    PERFORM set_config('enable_mergejoin', 'off', true);

    FOR plan_text IN EXECUTE $plan$
        EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY OFF)
        SELECT *
        FROM cypher('wcoj_lowering', $cypher$
            MATCH (s7:S {id: 7})-[e7:E]->(t:T),
                  (s8:S {id: 8})-[e8:E]->(t),
                  (s9:S {id: 9})-[e9:E]->(t)
            RETURN id(e7), id(e8), id(e9), id(t)
        $cypher$) AS (eid7 agtype, eid8 agtype, eid9 agtype, tid agtype)
    $plan$
    LOOP
        has_one_block := has_one_block OR
            plan_text LIKE '%Survivor Blocks: 1%';
        has_three_batches := has_three_batches OR
            plan_text LIKE '%Payload Scan Batches: 3%';
        has_zero_restarts := has_zero_restarts OR
            plan_text LIKE '%Payload Scan Restarts: 0%';
        has_three_source_keys := has_three_source_keys OR
            plan_text LIKE '%Payload Source Keys Scanned: 3%';
        has_three_distinct_source_keys := has_three_distinct_source_keys OR
            plan_text LIKE '%Distinct Source Keys Scanned: 3%';
        has_forty_eight_restarts_avoided :=
            has_forty_eight_restarts_avoided OR
            plan_text LIKE '%Payload Scan Restarts Avoided: 48%';
        has_fifty_one_payload_rows_scanned :=
            has_fifty_one_payload_rows_scanned OR
            plan_text LIKE '%Payload Rows Scanned: 51%';
        has_fifty_one_payload_rows := has_fifty_one_payload_rows OR
            plan_text LIKE '%Payload Rows Matched: 51%';
        has_seventeen_candidate_flat_rows :=
            has_seventeen_candidate_flat_rows OR
            plan_text LIKE '%Candidate Flat Rows: 17%';
        has_zero_flat_rows_avoided := has_zero_flat_rows_avoided OR
            plan_text LIKE '%Flat Rows Avoided: 0%';
    END LOOP;

    IF NOT has_one_block OR NOT has_three_batches OR
       NOT has_zero_restarts OR
       NOT has_three_source_keys OR NOT has_three_distinct_source_keys OR
       NOT has_forty_eight_restarts_avoided OR
       NOT has_fifty_one_payload_rows_scanned OR
       NOT has_fifty_one_payload_rows OR
       NOT has_seventeen_candidate_flat_rows OR
       NOT has_zero_flat_rows_avoided THEN
        RAISE EXCEPTION 'survivor payload batching telemetry was not observed';
    END IF;
    RAISE NOTICE 'wcoj survivor payload batching verified';
END
$wcoj_survivor_batch_telemetry$;

DO $wcoj_limit_row_goal_telemetry$
DECLARE
    plan_text text;
    has_limit boolean := false;
    has_wcoj boolean := false;
    has_row_goal boolean := false;
    has_row_goal_source boolean := false;
    has_rows_emitted boolean := false;
    has_row_goal_rows_emitted boolean := false;
    has_row_goal_flat_rows_avoided boolean := false;
    has_block_clamp boolean := false;
    has_goal_reached boolean := false;
    has_spill_bytes boolean := false;
BEGIN
    PERFORM set_config('age.enable_wcoj', 'on', true);
    PERFORM set_config('age.wcoj_engine', 'merge', true);
    PERFORM set_config('enable_nestloop', 'off', true);
    PERFORM set_config('enable_hashjoin', 'off', true);
    PERFORM set_config('enable_mergejoin', 'off', true);

    FOR plan_text IN EXECUTE $plan$
        EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY OFF)
        SELECT *
        FROM cypher('wcoj_lowering', $cypher$
            MATCH (s7:S {id: 7})-[e7:E]->(t:T),
                  (s8:S {id: 8})-[e8:E]->(t),
                  (s9:S {id: 9})-[e9:E]->(t)
            RETURN id(e7), id(e8), id(e9), id(t)
            LIMIT 1
        $cypher$) AS (eid7 agtype, eid8 agtype, eid9 agtype, tid agtype)
    $plan$
    LOOP
        has_limit := has_limit OR plan_text LIKE '%Limit%';
        has_wcoj := has_wcoj OR
            plan_text LIKE '%Custom Scan (AGE WCOJ Multiway Join)%';
        has_row_goal := has_row_goal OR
            plan_text LIKE '%WCOJ Row Goal: 1%';
        has_row_goal_source := has_row_goal_source OR
            plan_text LIKE '%WCOJ Row Goal Source: limit%';
        has_rows_emitted := has_rows_emitted OR
            plan_text LIKE '%Rows Emitted: 1%';
        has_row_goal_rows_emitted := has_row_goal_rows_emitted OR
            plan_text LIKE '%Row Goal Rows Emitted: 1%';
        has_row_goal_flat_rows_avoided :=
            has_row_goal_flat_rows_avoided OR
            plan_text LIKE '%Row Goal Flat Rows Avoided: 0%';
        has_block_clamp := has_block_clamp OR
            plan_text LIKE '%Row Goal Survivor Blocks Clamped: 1%';
        has_goal_reached := has_goal_reached OR
            plan_text LIKE '%Row Goal Reached: true%';
        has_spill_bytes := has_spill_bytes OR
            plan_text LIKE '%Spill Bytes: 0 bytes%';
    END LOOP;

    IF NOT has_limit OR NOT has_wcoj OR NOT has_row_goal OR
       NOT has_row_goal_source OR
       NOT has_rows_emitted OR NOT has_row_goal_rows_emitted OR
       NOT has_row_goal_flat_rows_avoided OR NOT has_block_clamp OR
       NOT has_goal_reached OR NOT has_spill_bytes THEN
        RAISE EXCEPTION 'WCOJ limit row-goal telemetry was not observed';
    END IF;
    RAISE NOTICE 'wcoj limit row-goal telemetry verified';
END
$wcoj_limit_row_goal_telemetry$;

SELECT ok FROM cypher('wcoj_lowering', $$
    RETURN EXISTS {
        MATCH (s7:S {id: 7})-[e7:E]->(t:T),
              (s8:S {id: 8})-[e8:E]->(t),
              (s9:S {id: 9})-[e9:E]->(t)
        RETURN t
    } AS ok
$$) AS (ok agtype);

DO $wcoj_exists_row_goal_telemetry$
DECLARE
    plan_text text;
    has_result boolean := false;
    has_initplan boolean := false;
    has_wcoj boolean := false;
    has_exists_consumer boolean := false;
    has_row_goal boolean := false;
    has_row_goal_source boolean := false;
    has_rows_emitted boolean := false;
    has_row_goal_rows_emitted boolean := false;
    has_row_goal_flat_rows_avoided boolean := false;
    has_block_clamp boolean := false;
    has_exists_result boolean := false;
    has_goal_reached boolean := false;
BEGIN
    PERFORM set_config('age.enable_wcoj', 'on', true);
    PERFORM set_config('age.wcoj_engine', 'merge', true);
    PERFORM set_config('enable_nestloop', 'off', true);
    PERFORM set_config('enable_hashjoin', 'off', true);
    PERFORM set_config('enable_mergejoin', 'off', true);

    FOR plan_text IN EXECUTE $plan$
        EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY OFF)
        SELECT *
        FROM cypher('wcoj_lowering', $cypher$
            RETURN EXISTS {
                MATCH (s7:S {id: 7})-[e7:E]->(t:T),
                      (s8:S {id: 8})-[e8:E]->(t),
                      (s9:S {id: 9})-[e9:E]->(t)
                RETURN t
            } AS ok
        $cypher$) AS (ok agtype)
    $plan$
    LOOP
        has_result := has_result OR plan_text LIKE '%Result%';
        has_initplan := has_initplan OR plan_text LIKE '%InitPlan%';
        has_wcoj := has_wcoj OR
            plan_text LIKE '%Custom Scan (AGE WCOJ Multiway Join)%';
        has_exists_consumer := has_exists_consumer OR
            plan_text LIKE '%WCOJ Consumer: exists%';
        has_row_goal := has_row_goal OR
            plan_text LIKE '%WCOJ Row Goal: 1%';
        has_row_goal_source := has_row_goal_source OR
            plan_text LIKE '%WCOJ Row Goal Source: exists%';
        has_rows_emitted := has_rows_emitted OR
            plan_text LIKE '%Rows Emitted: 1%';
        has_row_goal_rows_emitted := has_row_goal_rows_emitted OR
            plan_text LIKE '%Row Goal Rows Emitted: 1%';
        has_row_goal_flat_rows_avoided :=
            has_row_goal_flat_rows_avoided OR
            plan_text LIKE '%Row Goal Flat Rows Avoided: 0%';
        has_block_clamp := has_block_clamp OR
            plan_text LIKE '%Row Goal Survivor Blocks Clamped: 1%';
        has_exists_result := has_exists_result OR
            plan_text LIKE '%Exists Result: true%';
        has_goal_reached := has_goal_reached OR
            plan_text LIKE '%Row Goal Reached: true%';
    END LOOP;

    IF NOT has_result OR NOT has_initplan OR NOT has_wcoj OR
       NOT has_exists_consumer OR NOT has_row_goal OR
       NOT has_row_goal_source OR NOT has_rows_emitted OR
       NOT has_row_goal_rows_emitted OR
       NOT has_row_goal_flat_rows_avoided OR NOT has_block_clamp OR
       NOT has_exists_result OR
       NOT has_goal_reached THEN
        RAISE EXCEPTION 'WCOJ exists row-goal telemetry was not observed';
    END IF;
    RAISE NOTICE 'wcoj exists row-goal telemetry verified';
END
$wcoj_exists_row_goal_telemetry$;

-- Sparse overlap in progressive exact-set filtering should skip long runs in
-- the larger candidate array instead of merge-walking every key.  The first
-- branch has eighty terminals; the middle branch has three terminals; the
-- final branch has eighty terminals.
INSERT INTO wcoj_lowering."S" (id, properties)
SELECT _graphid(_label_id('wcoj_lowering', 'S'), source_no),
       format('{"id":%s}', source_no)::agtype
FROM generate_series(20, 22) source_no;
INSERT INTO wcoj_lowering."T" (id, properties)
SELECT _graphid(_label_id('wcoj_lowering', 'T'), target_no),
       format('{"id":%s}', target_no)::agtype
FROM generate_series(300, 379) target_no;
INSERT INTO wcoj_lowering."E" (id, start_id, end_id, properties)
SELECT _graphid(_label_id('wcoj_lowering', 'E'),
                30000 + target_no - 300),
       _graphid(_label_id('wcoj_lowering', 'S'), 20),
       _graphid(_label_id('wcoj_lowering', 'T'), target_no),
       '{}'::agtype
FROM generate_series(300, 379) target_no;
INSERT INTO wcoj_lowering."E" (id, start_id, end_id, properties)
SELECT _graphid(_label_id('wcoj_lowering', 'E'),
                30100 + target_no - 300),
       _graphid(_label_id('wcoj_lowering', 'S'), 21),
       _graphid(_label_id('wcoj_lowering', 'T'), target_no),
       '{}'::agtype
FROM (VALUES (300), (340), (379)) target(target_no);
INSERT INTO wcoj_lowering."E" (id, start_id, end_id, properties)
SELECT _graphid(_label_id('wcoj_lowering', 'E'),
                30200 + target_no - 300),
       _graphid(_label_id('wcoj_lowering', 'S'), 22),
       _graphid(_label_id('wcoj_lowering', 'T'), target_no),
       '{}'::agtype
FROM generate_series(300, 379) target_no;

DO $wcoj_galloping_intersection_telemetry$
DECLARE
    plan_text text;
    has_wcoj boolean := false;
    has_progressive_plan boolean := false;
    has_galloping_calls boolean := false;
    has_galloping_steps boolean := false;
    has_three_rows boolean := false;
BEGIN
    PERFORM set_config('age.enable_wcoj', 'on', true);
    PERFORM set_config('age.wcoj_engine', 'progressive', true);
    PERFORM set_config('enable_nestloop', 'off', true);
    PERFORM set_config('enable_hashjoin', 'off', true);
    PERFORM set_config('enable_mergejoin', 'off', true);

    FOR plan_text IN EXECUTE $plan$
        EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY OFF)
        SELECT *
        FROM cypher('wcoj_lowering', $cypher$
            MATCH (s20:S {id: 20})-[e20:E]->(t:T),
                  (s21:S {id: 21})-[e21:E]->(t),
                  (s22:S {id: 22})-[e22:E]->(t)
            RETURN id(e20), id(e21), id(e22), id(t)
        $cypher$) AS (eid20 agtype, eid21 agtype,
                      eid22 agtype, tid agtype)
    $plan$
    LOOP
        has_wcoj := has_wcoj OR
            plan_text LIKE '%Custom Scan (AGE WCOJ Multiway Join)%';
        has_progressive_plan := has_progressive_plan OR
            plan_text LIKE '%Planned Engine: progressive%';
        has_galloping_calls := has_galloping_calls OR
            plan_text ~ 'Intersection Galloping Calls: [1-9][0-9]*';
        has_galloping_steps := has_galloping_steps OR
            plan_text ~ 'Intersection Galloping Steps: [1-9][0-9]*';
        has_three_rows := has_three_rows OR
            plan_text LIKE '%Rows Emitted: 3%';
    END LOOP;

    IF NOT has_wcoj OR NOT has_progressive_plan OR
       NOT has_galloping_calls OR NOT has_galloping_steps OR
       NOT has_three_rows THEN
        RAISE EXCEPTION 'WCOJ galloping intersection telemetry was not observed';
    END IF;
    RAISE NOTICE 'wcoj galloping intersection telemetry verified';
END
$wcoj_galloping_intersection_telemetry$;

BEGIN;
SET LOCAL age.enable_wcoj = off;
CREATE TEMP TABLE wcoj_survivor_batch_binary ON COMMIT DROP AS
SELECT eid7, eid8, eid9, tid
FROM cypher('wcoj_lowering', $$
    MATCH (s7:S {id: 7})-[e7:E]->(t:T),
          (s8:S {id: 8})-[e8:E]->(t),
          (s9:S {id: 9})-[e9:E]->(t)
    RETURN id(e7), id(e8), id(e9), id(t)
$$) AS (eid7 agtype, eid8 agtype, eid9 agtype, tid agtype);

SET LOCAL age.enable_wcoj = on;
SET LOCAL age.wcoj_engine = 'merge';
SET LOCAL enable_nestloop = off;
SET LOCAL enable_hashjoin = off;
SET LOCAL enable_mergejoin = off;
CREATE TEMP TABLE wcoj_survivor_batch_direct ON COMMIT DROP AS
SELECT eid7, eid8, eid9, tid
FROM cypher('wcoj_lowering', $$
    MATCH (s7:S {id: 7})-[e7:E]->(t:T),
          (s8:S {id: 8})-[e8:E]->(t),
          (s9:S {id: 9})-[e9:E]->(t)
    RETURN id(e7), id(e8), id(e9), id(t)
$$) AS (eid7 agtype, eid8 agtype, eid9 agtype, tid agtype);

SELECT (SELECT count(*) FROM wcoj_survivor_batch_binary) AS binary_rows,
       (SELECT count(*) FROM wcoj_survivor_batch_direct) AS direct_rows,
       (SELECT count(*) FROM (
           (TABLE wcoj_survivor_batch_binary
            EXCEPT ALL TABLE wcoj_survivor_batch_direct)
           UNION ALL
           (TABLE wcoj_survivor_batch_direct
            EXCEPT ALL TABLE wcoj_survivor_batch_binary)
       ) diff) AS diff_rows;
ROLLBACK;

SELECT drop_graph('wcoj_lowering', true);

-- Mixed main/delta payload scans must charge both origins.  Two common targets
-- are present when the adjacency index is built, and one is inserted after the
-- index so the three source-key scans return 6 main payloads plus 3 delta
-- payloads.
SELECT create_graph('wcoj_payload_mixed');
SELECT create_vlabel('wcoj_payload_mixed', 'S');
SELECT create_vlabel('wcoj_payload_mixed', 'T');
SELECT create_elabel('wcoj_payload_mixed', 'E');
INSERT INTO wcoj_payload_mixed."S" (id, properties)
SELECT _graphid(_label_id('wcoj_payload_mixed', 'S'), source_no),
       format('{"id":%s}', source_no)::agtype
FROM generate_series(1, 3) source_no;
INSERT INTO wcoj_payload_mixed."T" (id, properties)
SELECT _graphid(_label_id('wcoj_payload_mixed', 'T'), target_no),
       format('{"id":%s}', target_no)::agtype
FROM generate_series(200, 201) target_no;
INSERT INTO wcoj_payload_mixed."E" (id, start_id, end_id, properties)
SELECT _graphid(_label_id('wcoj_payload_mixed', 'E'),
                20000 + (source_no - 1) * 2 + (target_no - 200)),
       _graphid(_label_id('wcoj_payload_mixed', 'S'), source_no),
       _graphid(_label_id('wcoj_payload_mixed', 'T'), target_no),
       '{}'::agtype
FROM generate_series(1, 3) source_no,
     generate_series(200, 201) target_no;
CREATE INDEX wcoj_payload_mixed_adj
ON wcoj_payload_mixed."E" USING age_adjacency(start_id, id, end_id);
INSERT INTO wcoj_payload_mixed."T" (id, properties)
VALUES (_graphid(_label_id('wcoj_payload_mixed', 'T'), 202),
        '{"id":202}'::agtype);
INSERT INTO wcoj_payload_mixed."E" (id, start_id, end_id, properties)
SELECT _graphid(_label_id('wcoj_payload_mixed', 'E'),
                20010 + source_no),
       _graphid(_label_id('wcoj_payload_mixed', 'S'), source_no),
       _graphid(_label_id('wcoj_payload_mixed', 'T'), 202),
       '{}'::agtype
FROM generate_series(1, 3) source_no;
ANALYZE wcoj_payload_mixed."S";
ANALYZE wcoj_payload_mixed."T";
ANALYZE wcoj_payload_mixed."E";

DO $wcoj_payload_mixed_telemetry$
DECLARE
    plan_text text;
    has_wcoj boolean := false;
    has_three_batches boolean := false;
    has_three_source_keys boolean := false;
    has_three_distinct_source_keys boolean := false;
    has_nine_payload_rows_scanned boolean := false;
    has_nine_payload_rows boolean := false;
    has_three_candidate_flat_rows boolean := false;
BEGIN
    PERFORM set_config('age.enable_wcoj', 'on', true);
    PERFORM set_config('age.wcoj_engine', 'merge', true);
    PERFORM set_config('enable_nestloop', 'off', true);
    PERFORM set_config('enable_hashjoin', 'off', true);
    PERFORM set_config('enable_mergejoin', 'off', true);

    FOR plan_text IN EXECUTE $plan$
        EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY OFF)
        SELECT *
        FROM cypher('wcoj_payload_mixed', $cypher$
            MATCH (s1:S {id: 1})-[e1:E]->(t:T),
                  (s2:S {id: 2})-[e2:E]->(t),
                  (s3:S {id: 3})-[e3:E]->(t)
            RETURN id(e1), id(e2), id(e3), id(t)
        $cypher$) AS (eid1 agtype, eid2 agtype, eid3 agtype, tid agtype)
    $plan$
    LOOP
        has_wcoj := has_wcoj OR
            plan_text LIKE '%Custom Scan (AGE WCOJ Multiway Join)%';
        has_three_batches := has_three_batches OR
            plan_text LIKE '%Payload Scan Batches: 3%';
        has_three_source_keys := has_three_source_keys OR
            plan_text LIKE '%Payload Source Keys Scanned: 3%';
        has_three_distinct_source_keys := has_three_distinct_source_keys OR
            plan_text LIKE '%Distinct Source Keys Scanned: 3%';
        has_nine_payload_rows_scanned :=
            has_nine_payload_rows_scanned OR
            plan_text LIKE '%Payload Rows Scanned: 9%';
        has_nine_payload_rows := has_nine_payload_rows OR
            plan_text LIKE '%Payload Rows Matched: 9%';
        has_three_candidate_flat_rows :=
            has_three_candidate_flat_rows OR
            plan_text LIKE '%Candidate Flat Rows: 3%';
    END LOOP;

    IF NOT has_wcoj OR NOT has_three_batches OR
       NOT has_three_source_keys OR NOT has_three_distinct_source_keys OR
       NOT has_nine_payload_rows_scanned OR NOT has_nine_payload_rows OR
       NOT has_three_candidate_flat_rows THEN
        RAISE EXCEPTION 'mixed main/delta payload telemetry was not observed';
    END IF;
    RAISE NOTICE 'wcoj mixed main/delta payload telemetry verified';
END
$wcoj_payload_mixed_telemetry$;

SELECT drop_graph('wcoj_payload_mixed', true);

-- Large edge property payloads should spill out of the survivor block instead
-- of growing batch_context without bound.  Count stays factorized and does not
-- need to read the spilled properties back, but edge-local quals still force
-- the direct providers to fetch and spill them.
SELECT create_graph('wcoj_payload_spill');
SELECT create_vlabel('wcoj_payload_spill', 'S');
SELECT create_vlabel('wcoj_payload_spill', 'T');
SELECT create_elabel('wcoj_payload_spill', 'E');
INSERT INTO wcoj_payload_spill."S" (id, properties)
SELECT _graphid(_label_id('wcoj_payload_spill', 'S'), source_no),
       format('{"id":%s}', source_no)::agtype
FROM generate_series(1, 3) source_no;
INSERT INTO wcoj_payload_spill."T" (id, properties)
SELECT _graphid(_label_id('wcoj_payload_spill', 'T'), target_no),
       format('{"id":%s}', target_no)::agtype
FROM generate_series(1, 8) target_no;
INSERT INTO wcoj_payload_spill."E" (id, start_id, end_id, properties)
SELECT _graphid(_label_id('wcoj_payload_spill', 'E'),
                30000 + (source_no - 1) * 8 + target_no),
       _graphid(_label_id('wcoj_payload_spill', 'S'), source_no),
       _graphid(_label_id('wcoj_payload_spill', 'T'), target_no),
       format('{"keep":true,"pad":"%s","w":%s}',
              repeat('x', 20000), target_no)::agtype
FROM generate_series(1, 3) source_no,
     generate_series(1, 8) target_no;
CREATE INDEX wcoj_payload_spill_adj
ON wcoj_payload_spill."E" USING age_adjacency(start_id, id, end_id);
ANALYZE wcoj_payload_spill."S";
ANALYZE wcoj_payload_spill."T";
ANALYZE wcoj_payload_spill."E";

DO $wcoj_payload_spill_telemetry$
DECLARE
    plan_text text;
    has_wcoj boolean := false;
    has_count_consumer boolean := false;
    has_batched boolean := false;
    has_payload_spill_rows boolean := false;
    has_spill_bytes boolean := false;
    has_count_result boolean := false;
BEGIN
    PERFORM set_config('work_mem', '64kB', true);
    PERFORM set_config('age.enable_wcoj', 'on', true);
    PERFORM set_config('age.wcoj_engine', 'merge', true);
    PERFORM set_config('enable_nestloop', 'off', true);
    PERFORM set_config('enable_hashjoin', 'off', true);
    PERFORM set_config('enable_mergejoin', 'off', true);

    FOR plan_text IN EXECUTE $plan$
        EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY OFF)
        SELECT *
        FROM cypher('wcoj_payload_spill', $cypher$
            MATCH (s1:S {id: 1})-[e1:E]->(t:T),
                  (s2:S {id: 2})-[e2:E]->(t),
                  (s3:S {id: 3})-[e3:E]->(t)
            WHERE e1.keep = true AND e2.keep = true AND e3.keep = true
            RETURN count(*) AS total
        $cypher$) AS (total agtype)
    $plan$
    LOOP
        has_wcoj := has_wcoj OR
            plan_text LIKE '%Custom Scan (AGE WCOJ Multiway Join)%';
        has_count_consumer := has_count_consumer OR
            plan_text LIKE '%WCOJ Consumer: count(*)%';
        has_batched := has_batched OR
            plan_text LIKE '%Batched Payload Materialization: true%';
        has_payload_spill_rows := has_payload_spill_rows OR
            plan_text ~ 'Payload Rows Spilled: [1-9][0-9]*';
        has_spill_bytes := has_spill_bytes OR
            plan_text ~ 'Spill Bytes: [1-9][0-9]* bytes';
        has_count_result := has_count_result OR
            plan_text LIKE '%Count Result: 8%';
    END LOOP;

    IF NOT has_wcoj OR NOT has_count_consumer OR NOT has_batched OR
       NOT has_payload_spill_rows OR NOT has_spill_bytes OR
       NOT has_count_result THEN
        RAISE EXCEPTION 'WCOJ payload spill telemetry was not observed';
    END IF;
    RAISE NOTICE 'wcoj payload spill telemetry verified';
END
$wcoj_payload_spill_telemetry$;

DO $wcoj_payload_spill_restore$
DECLARE
    plan_text text;
    has_wcoj boolean := false;
    has_payload_spill_rows boolean := false;
    has_spill_bytes boolean := false;
    has_rows_emitted boolean := false;
BEGIN
    PERFORM set_config('work_mem', '64kB', true);
    PERFORM set_config('age.enable_wcoj', 'on', true);
    PERFORM set_config('age.wcoj_engine', 'merge', true);
    PERFORM set_config('enable_nestloop', 'off', true);
    PERFORM set_config('enable_hashjoin', 'off', true);
    PERFORM set_config('enable_mergejoin', 'off', true);

    FOR plan_text IN EXECUTE $plan$
        EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY OFF)
        SELECT *
        FROM cypher('wcoj_payload_spill', $cypher$
            MATCH (s1:S {id: 1})-[e1:E]->(t:T),
                  (s2:S {id: 2})-[e2:E]->(t),
                  (s3:S {id: 3})-[e3:E]->(t)
            WHERE e1.keep = true AND e2.keep = true AND e3.keep = true
            RETURN e1, id(t)
            LIMIT 1
        $cypher$) AS (e1 agtype, tid agtype)
    $plan$
    LOOP
        has_wcoj := has_wcoj OR
            plan_text LIKE '%Custom Scan (AGE WCOJ Multiway Join)%';
        has_payload_spill_rows := has_payload_spill_rows OR
            plan_text ~ 'Payload Rows Spilled: [1-9][0-9]*';
        has_spill_bytes := has_spill_bytes OR
            plan_text ~ 'Spill Bytes: [1-9][0-9]* bytes';
        has_rows_emitted := has_rows_emitted OR
            plan_text LIKE '%Rows Emitted: 1%';
    END LOOP;

    IF NOT has_wcoj OR NOT has_payload_spill_rows OR
       NOT has_spill_bytes OR NOT has_rows_emitted THEN
        RAISE EXCEPTION 'WCOJ payload spill restore path was not observed';
    END IF;
    RAISE NOTICE 'wcoj payload spill restore verified';
END
$wcoj_payload_spill_restore$;

BEGIN;
SET LOCAL age.enable_wcoj = off;
CREATE TEMP TABLE wcoj_payload_spill_binary ON COMMIT DROP AS
SELECT total
FROM cypher('wcoj_payload_spill', $$
    MATCH (s1:S {id: 1})-[e1:E]->(t:T),
          (s2:S {id: 2})-[e2:E]->(t),
          (s3:S {id: 3})-[e3:E]->(t)
    WHERE e1.keep = true AND e2.keep = true AND e3.keep = true
    RETURN count(*) AS total
$$) AS (total agtype);

SET LOCAL work_mem = '64kB';
SET LOCAL age.enable_wcoj = on;
SET LOCAL age.wcoj_engine = 'merge';
SET LOCAL enable_nestloop = off;
SET LOCAL enable_hashjoin = off;
SET LOCAL enable_mergejoin = off;
CREATE TEMP TABLE wcoj_payload_spill_direct ON COMMIT DROP AS
SELECT total
FROM cypher('wcoj_payload_spill', $$
    MATCH (s1:S {id: 1})-[e1:E]->(t:T),
          (s2:S {id: 2})-[e2:E]->(t),
          (s3:S {id: 3})-[e3:E]->(t)
    WHERE e1.keep = true AND e2.keep = true AND e3.keep = true
    RETURN count(*) AS total
$$) AS (total agtype);

SELECT (SELECT count(*) FROM wcoj_payload_spill_binary) AS binary_rows,
       (SELECT count(*) FROM wcoj_payload_spill_direct) AS direct_rows,
       (SELECT count(*) FROM (
           (TABLE wcoj_payload_spill_binary
            EXCEPT ALL TABLE wcoj_payload_spill_direct)
           UNION ALL
           (TABLE wcoj_payload_spill_direct
            EXCEPT ALL TABLE wcoj_payload_spill_binary)
       ) diff) AS diff_rows;
ROLLBACK;

SELECT drop_graph('wcoj_payload_spill', true);

-- Direct providers are independent per pattern descriptor.  This star mixes
-- three edge labels and an incoming middle branch while preserving 2 * 3 * 4
-- edge-bag multiplicity.
SELECT create_graph('wcoj_mixed');
SELECT create_vlabel('wcoj_mixed', 'S');
SELECT create_vlabel('wcoj_mixed', 'T');
SELECT create_elabel('wcoj_mixed', 'E1');
SELECT create_elabel('wcoj_mixed', 'E2');
SELECT create_elabel('wcoj_mixed', 'E3');
SELECT * FROM cypher('wcoj_mixed', $$
    CREATE (s1:S {id: 1}), (s2:S {id: 2}), (s3:S {id: 3}),
           (t:T {id: 10}),
           (s1)-[:E1]->(t), (s1)-[:E1]->(t),
           (t)-[:E2]->(s2), (t)-[:E2]->(s2), (t)-[:E2]->(s2),
           (s3)-[:E3]->(t), (s3)-[:E3]->(t),
           (s3)-[:E3]->(t), (s3)-[:E3]->(t)
$$) AS (z agtype);
CREATE INDEX wcoj_mixed_e1_out
ON wcoj_mixed."E1" USING age_adjacency(start_id, id, end_id);
CREATE INDEX wcoj_mixed_e2_in
ON wcoj_mixed."E2" USING age_adjacency(end_id, id, start_id);
CREATE INDEX wcoj_mixed_e3_out
ON wcoj_mixed."E3" USING age_adjacency(start_id, id, end_id);
ANALYZE wcoj_mixed."S";
ANALYZE wcoj_mixed."T";
ANALYZE wcoj_mixed."E1";
ANALYZE wcoj_mixed."E2";
ANALYZE wcoj_mixed."E3";

DO $wcoj_mixed_plan$
DECLARE
    plan_text text;
    has_wcoj boolean := false;
    has_three_adjacency_providers boolean := false;
BEGIN
    PERFORM set_config('age.enable_wcoj', 'on', true);
    PERFORM set_config('age.wcoj_engine', 'leapfrog', true);
    PERFORM set_config('enable_nestloop', 'off', true);
    PERFORM set_config('enable_hashjoin', 'off', true);
    PERFORM set_config('enable_mergejoin', 'off', true);

    FOR plan_text IN EXECUTE $plan$
        EXPLAIN (VERBOSE, COSTS OFF)
        SELECT *
        FROM cypher('wcoj_mixed', $cypher$
            MATCH (s1:S {id: 1})-[e1:E1]->(t:T),
                  (s2:S {id: 2})<-[e2:E2]-(t),
                  (s3:S {id: 3})-[e3:E3]->(t)
            RETURN id(e1), id(e2), id(e3), id(t)
        $cypher$) AS (eid1 agtype, eid2 agtype, eid3 agtype, tid agtype)
    $plan$
    LOOP
        has_wcoj := has_wcoj OR
            plan_text LIKE '%Custom Scan (AGE WCOJ Multiway Join)%';
        has_three_adjacency_providers := has_three_adjacency_providers OR
            plan_text LIKE '%Adjacency Providers: 3%';
    END LOOP;

    IF NOT has_wcoj OR NOT has_three_adjacency_providers THEN
        RAISE EXCEPTION 'mixed label/direction star did not use direct WCOJ';
    END IF;
    RAISE NOTICE 'wcoj mixed label and direction providers verified';
END
$wcoj_mixed_plan$;

BEGIN;
SET LOCAL age.enable_wcoj = off;
CREATE TEMP TABLE wcoj_mixed_binary ON COMMIT DROP AS
SELECT eid1, eid2, eid3, tid
FROM cypher('wcoj_mixed', $$
    MATCH (s1:S {id: 1})-[e1:E1]->(t:T),
          (s2:S {id: 2})<-[e2:E2]-(t),
          (s3:S {id: 3})-[e3:E3]->(t)
    RETURN id(e1), id(e2), id(e3), id(t)
$$) AS (eid1 agtype, eid2 agtype, eid3 agtype, tid agtype);

SET LOCAL age.enable_wcoj = on;
SET LOCAL age.wcoj_engine = 'leapfrog';
SET LOCAL enable_nestloop = off;
SET LOCAL enable_hashjoin = off;
SET LOCAL enable_mergejoin = off;
CREATE TEMP TABLE wcoj_mixed_direct ON COMMIT DROP AS
SELECT eid1, eid2, eid3, tid
FROM cypher('wcoj_mixed', $$
    MATCH (s1:S {id: 1})-[e1:E1]->(t:T),
          (s2:S {id: 2})<-[e2:E2]-(t),
          (s3:S {id: 3})-[e3:E3]->(t)
    RETURN id(e1), id(e2), id(e3), id(t)
$$) AS (eid1 agtype, eid2 agtype, eid3 agtype, tid agtype);

SELECT (SELECT count(*) FROM wcoj_mixed_binary) AS binary_rows,
       (SELECT count(*) FROM wcoj_mixed_direct) AS direct_rows,
       (SELECT count(*)
        FROM (
            (TABLE wcoj_mixed_binary EXCEPT ALL TABLE wcoj_mixed_direct)
            UNION ALL
            (TABLE wcoj_mixed_direct EXCEPT ALL TABLE wcoj_mixed_binary)
        ) diff) AS diff_rows;
ROLLBACK;

SELECT drop_graph('wcoj_mixed', true);

-- Cyclic components use the Generic Join logical IR.  One graph covers
-- triangle, diamond, four-cycle, parallel-edge, uniqueness, property, and
-- mixed label/direction semantics so every comparison is a multiset check.
SELECT create_graph('wcoj_generic');
SELECT create_vlabel('wcoj_generic', 'N');
SELECT create_elabel('wcoj_generic', 'E');
SELECT create_elabel('wcoj_generic', 'E1');
SELECT create_elabel('wcoj_generic', 'E2');
SELECT create_elabel('wcoj_generic', 'E3');

SELECT * FROM cypher('wcoj_generic', $$
    CREATE (t1:N {case:'tri', id:1}),
           (t2:N {case:'tri', id:2}),
           (t3:N {case:'tri', id:3}),
           (q1:N {case:'c4', id:10}),
           (q2:N {case:'c4', id:11}),
           (q3:N {case:'c4', id:12}),
           (q4:N {case:'c4', id:13}),
           (d1:N {case:'dia', id:20}),
           (d2:N {case:'dia', id:21}),
           (d3:N {case:'dia', id:22}),
           (d4:N {case:'dia', id:23}),
           (p1:N {case:'par', id:30}),
           (p2:N {case:'par', id:31}),
           (p3:N {case:'par', id:32}),
           (s:N {case:'self', id:40}),
           (r1:N {case:'prop', id:50}),
           (r2:N {case:'prop', id:51}),
           (r3:N {case:'prop', id:52}),
           (m1:N {case:'mixed', id:60}),
           (m2:N {case:'mixed', id:61}),
           (m3:N {case:'mixed', id:62})
$$) AS (z agtype);

SELECT * FROM cypher('wcoj_generic', $$
    MATCH (t1:N {case:'tri',id:1}),
          (t2:N {case:'tri',id:2}),
          (t3:N {case:'tri',id:3}),
          (q1:N {case:'c4',id:10}),
          (q2:N {case:'c4',id:11}),
          (q3:N {case:'c4',id:12}),
          (q4:N {case:'c4',id:13}),
          (d1:N {case:'dia',id:20}),
          (d2:N {case:'dia',id:21}),
          (d3:N {case:'dia',id:22}),
          (d4:N {case:'dia',id:23}),
          (p1:N {case:'par',id:30}),
          (p2:N {case:'par',id:31}),
          (p3:N {case:'par',id:32}),
          (s:N {case:'self',id:40}),
          (r1:N {case:'prop',id:50}),
          (r2:N {case:'prop',id:51}),
          (r3:N {case:'prop',id:52}),
          (m1:N {case:'mixed',id:60}),
          (m2:N {case:'mixed',id:61}),
          (m3:N {case:'mixed',id:62})
    CREATE (t1)-[:E {case:'tri'}]->(t2),
           (t2)-[:E {case:'tri'}]->(t3),
           (t3)-[:E {case:'tri'}]->(t1),
           (q1)-[:E {case:'c4'}]->(q2),
           (q2)-[:E {case:'c4'}]->(q3),
           (q3)-[:E {case:'c4'}]->(q4),
           (q4)-[:E {case:'c4'}]->(q1),
           (d1)-[:E {case:'dia'}]->(d2),
           (d1)-[:E {case:'dia'}]->(d3),
           (d2)-[:E {case:'dia'}]->(d4),
           (d3)-[:E {case:'dia'}]->(d4),
           (p1)-[:E {case:'par'}]->(p2),
           (p1)-[:E {case:'par'}]->(p2),
           (p2)-[:E {case:'par'}]->(p3),
           (p2)-[:E {case:'par'}]->(p3),
           (p2)-[:E {case:'par'}]->(p3),
           (p3)-[:E {case:'par'}]->(p1),
           (p3)-[:E {case:'par'}]->(p1),
           (p3)-[:E {case:'par'}]->(p1),
           (p3)-[:E {case:'par'}]->(p1),
           (s)-[:E {case:'self'}]->(s),
           (r1)-[:E {case:'prop',keep:true}]->(r2),
           (r2)-[:E {case:'prop',keep:true}]->(r3),
           (r3)-[:E {case:'prop',keep:true}]->(r1),
           (r1)-[:E {case:'prop',keep:false}]->(r2),
           (m1)-[:E1 {case:'mixed'}]->(m2),
           (m3)-[:E2 {case:'mixed'}]->(m2),
           (m3)-[:E3 {case:'mixed'}]->(m1)
$$) AS (z agtype);

CREATE INDEX wcoj_generic_e_out
ON wcoj_generic."E" USING age_adjacency(start_id, id, end_id);
CREATE INDEX wcoj_generic_e_in
ON wcoj_generic."E" USING age_adjacency(end_id, id, start_id);
CREATE INDEX wcoj_generic_e1_out
ON wcoj_generic."E1" USING age_adjacency(start_id, id, end_id);
CREATE INDEX wcoj_generic_e2_in
ON wcoj_generic."E2" USING age_adjacency(end_id, id, start_id);
CREATE INDEX wcoj_generic_e3_out
ON wcoj_generic."E3" USING age_adjacency(start_id, id, end_id);
ANALYZE wcoj_generic."N";
ANALYZE wcoj_generic."E";
ANALYZE wcoj_generic."E1";
ANALYZE wcoj_generic."E2";
ANALYZE wcoj_generic."E3";

DO $wcoj_generic_plan$
DECLARE
    plan_text text;
    has_generic boolean := false;
    has_variable_count boolean := false;
    has_provider_count boolean := false;
BEGIN
    PERFORM set_config('age.enable_wcoj', 'on', true);
    PERFORM set_config('enable_nestloop', 'off', true);
    PERFORM set_config('enable_hashjoin', 'off', true);
    PERFORM set_config('enable_mergejoin', 'off', true);

    FOR plan_text IN EXECUTE $plan$
        EXPLAIN (VERBOSE, COSTS OFF)
        SELECT *
        FROM cypher('wcoj_generic', $cypher$
            MATCH (a:N {case:'tri'})-[e1:E {case:'tri'}]->(b:N)
                  -[e2:E {case:'tri'}]->(c:N)
                  -[e3:E {case:'tri'}]->(a)
            RETURN id(a), id(b), id(c), id(e1), id(e2), id(e3)
        $cypher$) AS (a agtype, b agtype, c agtype,
                      e1 agtype, e2 agtype, e3 agtype)
    $plan$
    LOOP
        has_generic := has_generic OR
            plan_text LIKE '%Custom Scan (AGE Generic Multiway Join)%';
        has_variable_count := has_variable_count OR
            plan_text LIKE '%Variable Count: 3%';
        has_provider_count := has_provider_count OR
            plan_text LIKE '%Provider Count: 6%';
    END LOOP;

    IF NOT has_generic OR NOT has_variable_count OR NOT has_provider_count THEN
        RAISE EXCEPTION 'cyclic component did not use AGE Generic Join';
    END IF;
    RAISE NOTICE 'wcoj generic join logical IR verified';
END
$wcoj_generic_plan$;

DO $wcoj_generic_factor_telemetry$
DECLARE
    plan_text text;
    has_generic boolean := false;
    has_candidate_flat_rows boolean := false;
    has_candidate_combinations boolean := false;
    has_flat_rows_avoided boolean := false;
BEGIN
    PERFORM set_config('age.enable_wcoj', 'on', true);
    PERFORM set_config('enable_nestloop', 'off', true);
    PERFORM set_config('enable_hashjoin', 'off', true);
    PERFORM set_config('enable_mergejoin', 'off', true);

    FOR plan_text IN EXECUTE $plan$
        EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY OFF)
        SELECT *
        FROM cypher('wcoj_generic', $cypher$
            MATCH (a:N {case:'self'})-[:E {case:'self'}]->(b:N)
                  -[:E {case:'self'}]->(c:N)
                  -[:E {case:'self'}]->(a)
            RETURN id(a), id(b), id(c)
        $cypher$) AS (a agtype, b agtype, c agtype)
    $plan$
    LOOP
        has_generic := has_generic OR
            plan_text LIKE '%Custom Scan (AGE Generic Multiway Join)%';
        has_candidate_flat_rows := has_candidate_flat_rows OR
            plan_text LIKE '%Candidate Flat Rows: 1%';
        has_candidate_combinations := has_candidate_combinations OR
            plan_text LIKE '%Candidate Bag Combinations: 0%';
        has_flat_rows_avoided := has_flat_rows_avoided OR
            plan_text LIKE '%Flat Rows Avoided: 1%';
    END LOOP;

    IF NOT has_generic OR NOT has_candidate_flat_rows OR
       NOT has_candidate_combinations OR NOT has_flat_rows_avoided THEN
        RAISE EXCEPTION 'Generic Join factorized telemetry was not observed';
    END IF;
    RAISE NOTICE 'wcoj generic join factorized telemetry verified';
END
$wcoj_generic_factor_telemetry$;

BEGIN;
CREATE TEMP TABLE wcoj_generic_summary (
    case_name text,
    binary_rows bigint,
    generic_rows bigint,
    diff_rows bigint
) ON COMMIT DROP;

SET LOCAL age.enable_wcoj = off;
CREATE TEMP TABLE generic_binary_triangle ON COMMIT DROP AS
SELECT * FROM cypher('wcoj_generic', $$
    MATCH (a:N {case:'tri'})-[e1:E {case:'tri'}]->(b:N)
          -[e2:E {case:'tri'}]->(c:N)
          -[e3:E {case:'tri'}]->(a)
    RETURN id(a), id(b), id(c), id(e1), id(e2), id(e3)
$$) AS (a agtype, b agtype, c agtype,
        e1 agtype, e2 agtype, e3 agtype);
CREATE TEMP TABLE generic_binary_four_cycle ON COMMIT DROP AS
SELECT * FROM cypher('wcoj_generic', $$
    MATCH (a:N {case:'c4'})-[e1:E {case:'c4'}]->(b:N)
          -[e2:E {case:'c4'}]->(c:N)
          -[e3:E {case:'c4'}]->(d:N)
          -[e4:E {case:'c4'}]->(a)
    RETURN id(a), id(b), id(c), id(d),
           id(e1), id(e2), id(e3), id(e4)
$$) AS (a agtype, b agtype, c agtype, d agtype,
        e1 agtype, e2 agtype, e3 agtype, e4 agtype);
CREATE TEMP TABLE generic_binary_diamond ON COMMIT DROP AS
SELECT * FROM cypher('wcoj_generic', $$
    MATCH (a:N {case:'dia',id:20})-[e1:E {case:'dia'}]->(b:N)
          -[e2:E {case:'dia'}]->(d:N {case:'dia',id:23}),
          (a)-[e3:E {case:'dia'}]->(c:N)
          -[e4:E {case:'dia'}]->(d)
    RETURN id(a), id(b), id(c), id(d),
           id(e1), id(e2), id(e3), id(e4)
$$) AS (a agtype, b agtype, c agtype, d agtype,
        e1 agtype, e2 agtype, e3 agtype, e4 agtype);
CREATE TEMP TABLE generic_binary_parallel ON COMMIT DROP AS
SELECT * FROM cypher('wcoj_generic', $$
    MATCH (a:N {case:'par',id:30})-[e1:E {case:'par'}]->(b:N)
          -[e2:E {case:'par'}]->(c:N)
          -[e3:E {case:'par'}]->(a)
    RETURN id(e1), id(e2), id(e3)
$$) AS (e1 agtype, e2 agtype, e3 agtype);
-- Anonymous relationship occurrences still require hidden edge IDs for
-- uniqueness-aware DFS; no projected edge column may be used as a shortcut.
CREATE TEMP TABLE generic_binary_self_unique ON COMMIT DROP AS
SELECT * FROM cypher('wcoj_generic', $$
    MATCH (a:N {case:'self'})-[:E {case:'self'}]->(b:N)
          -[:E {case:'self'}]->(c:N)
          -[:E {case:'self'}]->(a)
    RETURN id(a), id(b), id(c)
$$) AS (a agtype, b agtype, c agtype);
CREATE TEMP TABLE generic_binary_property ON COMMIT DROP AS
SELECT * FROM cypher('wcoj_generic', $$
    MATCH (a:N {case:'prop'})-[e1:E {case:'prop',keep:true}]->(b:N)
          -[e2:E {case:'prop',keep:true}]->(c:N)
          -[e3:E {case:'prop',keep:true}]->(a)
    RETURN id(e1), id(e2), id(e3), e1.keep, e2.keep, e3.keep
$$) AS (e1 agtype, e2 agtype, e3 agtype,
        p1 agtype, p2 agtype, p3 agtype);
CREATE TEMP TABLE generic_binary_mixed ON COMMIT DROP AS
SELECT * FROM cypher('wcoj_generic', $$
    MATCH (a:N {case:'mixed'})-[e1:E1]->(b:N)<-[e2:E2]-(c:N)
          -[e3:E3]->(a)
    RETURN id(a), id(b), id(c), id(e1), id(e2), id(e3)
$$) AS (a agtype, b agtype, c agtype,
        e1 agtype, e2 agtype, e3 agtype);

SET LOCAL age.enable_wcoj = on;
SET LOCAL enable_nestloop = off;
SET LOCAL enable_hashjoin = off;
SET LOCAL enable_mergejoin = off;
CREATE TEMP TABLE generic_join_triangle ON COMMIT DROP AS
TABLE generic_binary_triangle WITH NO DATA;
INSERT INTO generic_join_triangle
SELECT * FROM cypher('wcoj_generic', $$
    MATCH (a:N {case:'tri'})-[e1:E {case:'tri'}]->(b:N)
          -[e2:E {case:'tri'}]->(c:N)
          -[e3:E {case:'tri'}]->(a)
    RETURN id(a), id(b), id(c), id(e1), id(e2), id(e3)
$$) AS (a agtype, b agtype, c agtype,
        e1 agtype, e2 agtype, e3 agtype);
CREATE TEMP TABLE generic_join_four_cycle ON COMMIT DROP AS
TABLE generic_binary_four_cycle WITH NO DATA;
INSERT INTO generic_join_four_cycle
SELECT * FROM cypher('wcoj_generic', $$
    MATCH (a:N {case:'c4'})-[e1:E {case:'c4'}]->(b:N)
          -[e2:E {case:'c4'}]->(c:N)
          -[e3:E {case:'c4'}]->(d:N)
          -[e4:E {case:'c4'}]->(a)
    RETURN id(a), id(b), id(c), id(d),
           id(e1), id(e2), id(e3), id(e4)
$$) AS (a agtype, b agtype, c agtype, d agtype,
        e1 agtype, e2 agtype, e3 agtype, e4 agtype);
CREATE TEMP TABLE generic_join_diamond ON COMMIT DROP AS
TABLE generic_binary_diamond WITH NO DATA;
INSERT INTO generic_join_diamond
SELECT * FROM cypher('wcoj_generic', $$
    MATCH (a:N {case:'dia',id:20})-[e1:E {case:'dia'}]->(b:N)
          -[e2:E {case:'dia'}]->(d:N {case:'dia',id:23}),
          (a)-[e3:E {case:'dia'}]->(c:N)
          -[e4:E {case:'dia'}]->(d)
    RETURN id(a), id(b), id(c), id(d),
           id(e1), id(e2), id(e3), id(e4)
$$) AS (a agtype, b agtype, c agtype, d agtype,
        e1 agtype, e2 agtype, e3 agtype, e4 agtype);
CREATE TEMP TABLE generic_join_parallel ON COMMIT DROP AS
TABLE generic_binary_parallel WITH NO DATA;
INSERT INTO generic_join_parallel
SELECT * FROM cypher('wcoj_generic', $$
    MATCH (a:N {case:'par',id:30})-[e1:E {case:'par'}]->(b:N)
          -[e2:E {case:'par'}]->(c:N)
          -[e3:E {case:'par'}]->(a)
    RETURN id(e1), id(e2), id(e3)
$$) AS (e1 agtype, e2 agtype, e3 agtype);
CREATE TEMP TABLE generic_join_self_unique ON COMMIT DROP AS
TABLE generic_binary_self_unique WITH NO DATA;
INSERT INTO generic_join_self_unique
SELECT * FROM cypher('wcoj_generic', $$
    MATCH (a:N {case:'self'})-[:E {case:'self'}]->(b:N)
          -[:E {case:'self'}]->(c:N)
          -[:E {case:'self'}]->(a)
    RETURN id(a), id(b), id(c)
$$) AS (a agtype, b agtype, c agtype);
CREATE TEMP TABLE generic_join_property ON COMMIT DROP AS
TABLE generic_binary_property WITH NO DATA;
INSERT INTO generic_join_property
SELECT * FROM cypher('wcoj_generic', $$
    MATCH (a:N {case:'prop'})-[e1:E {case:'prop',keep:true}]->(b:N)
          -[e2:E {case:'prop',keep:true}]->(c:N)
          -[e3:E {case:'prop',keep:true}]->(a)
    RETURN id(e1), id(e2), id(e3), e1.keep, e2.keep, e3.keep
$$) AS (e1 agtype, e2 agtype, e3 agtype,
        p1 agtype, p2 agtype, p3 agtype);
CREATE TEMP TABLE generic_join_mixed ON COMMIT DROP AS
TABLE generic_binary_mixed WITH NO DATA;
INSERT INTO generic_join_mixed
SELECT * FROM cypher('wcoj_generic', $$
    MATCH (a:N {case:'mixed'})-[e1:E1]->(b:N)<-[e2:E2]-(c:N)
          -[e3:E3]->(a)
    RETURN id(a), id(b), id(c), id(e1), id(e2), id(e3)
$$) AS (a agtype, b agtype, c agtype,
        e1 agtype, e2 agtype, e3 agtype);

INSERT INTO wcoj_generic_summary
SELECT 'triangle',
       (SELECT count(*) FROM generic_binary_triangle),
       (SELECT count(*) FROM generic_join_triangle),
       (SELECT count(*) FROM (
           (TABLE generic_binary_triangle EXCEPT ALL TABLE generic_join_triangle)
           UNION ALL
           (TABLE generic_join_triangle EXCEPT ALL TABLE generic_binary_triangle)
       ) diff)
UNION ALL
SELECT 'four_cycle',
       (SELECT count(*) FROM generic_binary_four_cycle),
       (SELECT count(*) FROM generic_join_four_cycle),
       (SELECT count(*) FROM (
           (TABLE generic_binary_four_cycle EXCEPT ALL TABLE generic_join_four_cycle)
           UNION ALL
           (TABLE generic_join_four_cycle EXCEPT ALL TABLE generic_binary_four_cycle)
       ) diff)
UNION ALL
SELECT 'diamond',
       (SELECT count(*) FROM generic_binary_diamond),
       (SELECT count(*) FROM generic_join_diamond),
       (SELECT count(*) FROM (
           (TABLE generic_binary_diamond EXCEPT ALL TABLE generic_join_diamond)
           UNION ALL
           (TABLE generic_join_diamond EXCEPT ALL TABLE generic_binary_diamond)
       ) diff)
UNION ALL
SELECT 'parallel_2x3x4',
       (SELECT count(*) FROM generic_binary_parallel),
       (SELECT count(*) FROM generic_join_parallel),
       (SELECT count(*) FROM (
           (TABLE generic_binary_parallel EXCEPT ALL TABLE generic_join_parallel)
           UNION ALL
           (TABLE generic_join_parallel EXCEPT ALL TABLE generic_binary_parallel)
       ) diff)
UNION ALL
SELECT 'self_uniqueness',
       (SELECT count(*) FROM generic_binary_self_unique),
       (SELECT count(*) FROM generic_join_self_unique),
       (SELECT count(*) FROM (
           (TABLE generic_binary_self_unique EXCEPT ALL TABLE generic_join_self_unique)
           UNION ALL
           (TABLE generic_join_self_unique EXCEPT ALL TABLE generic_binary_self_unique)
       ) diff)
UNION ALL
SELECT 'edge_property',
       (SELECT count(*) FROM generic_binary_property),
       (SELECT count(*) FROM generic_join_property),
       (SELECT count(*) FROM (
           (TABLE generic_binary_property EXCEPT ALL TABLE generic_join_property)
           UNION ALL
           (TABLE generic_join_property EXCEPT ALL TABLE generic_binary_property)
       ) diff)
UNION ALL
SELECT 'mixed_label_direction',
       (SELECT count(*) FROM generic_binary_mixed),
       (SELECT count(*) FROM generic_join_mixed),
       (SELECT count(*) FROM (
           (TABLE generic_binary_mixed EXCEPT ALL TABLE generic_join_mixed)
           UNION ALL
           (TABLE generic_join_mixed EXCEPT ALL TABLE generic_binary_mixed)
       ) diff);

SELECT * FROM wcoj_generic_summary ORDER BY case_name;
ROLLBACK;

-- A high-pressure acyclic chain is admitted to Generic Join only when the
-- estimated adjacent-edge intermediate is large.  Query-wide semijoin
-- reduction keeps the single common C endpoint before any A x C binding is
-- enumerated.  The shape is deliberately not a star, which remains assigned
-- to the lower-overhead direct WCOJ executor.
SELECT create_vlabel('wcoj_generic', 'A');
SELECT create_vlabel('wcoj_generic', 'B');
SELECT create_vlabel('wcoj_generic', 'C');
SELECT create_vlabel('wcoj_generic', 'D');
SELECT create_elabel('wcoj_generic', 'F1');
SELECT create_elabel('wcoj_generic', 'F2');
SELECT create_elabel('wcoj_generic', 'F3');

INSERT INTO wcoj_generic."A" (id, properties)
SELECT _graphid(_label_id('wcoj_generic', 'A'), i), '{}'::agtype
FROM generate_series(1, 128) i;
INSERT INTO wcoj_generic."B" (id, properties)
VALUES (_graphid(_label_id('wcoj_generic', 'B'), 1), '{}'::agtype);
INSERT INTO wcoj_generic."C" (id, properties)
SELECT _graphid(_label_id('wcoj_generic', 'C'), i), '{}'::agtype
FROM generate_series(1, 255) i;
INSERT INTO wcoj_generic."D" (id, properties)
VALUES (_graphid(_label_id('wcoj_generic', 'D'), 1), '{}'::agtype);

INSERT INTO wcoj_generic."F1" (id, start_id, end_id, properties)
SELECT _graphid(_label_id('wcoj_generic', 'F1'), i),
       _graphid(_label_id('wcoj_generic', 'A'), i),
       _graphid(_label_id('wcoj_generic', 'B'), 1),
       '{}'::agtype
FROM generate_series(1, 128) i;
INSERT INTO wcoj_generic."F2" (id, start_id, end_id, properties)
SELECT _graphid(_label_id('wcoj_generic', 'F2'), i),
       _graphid(_label_id('wcoj_generic', 'B'), 1),
       _graphid(_label_id('wcoj_generic', 'C'), i),
       '{}'::agtype
FROM generate_series(1, 128) i;
INSERT INTO wcoj_generic."F3" (id, start_id, end_id, properties)
SELECT _graphid(_label_id('wcoj_generic', 'F3'), i),
       _graphid(_label_id('wcoj_generic', 'C'), 127 + i),
       _graphid(_label_id('wcoj_generic', 'D'), 1),
       '{}'::agtype
FROM generate_series(1, 128) i;

CREATE INDEX wcoj_generic_f1_out
ON wcoj_generic."F1" USING age_adjacency(start_id, id, end_id);
CREATE INDEX wcoj_generic_f2_out
ON wcoj_generic."F2" USING age_adjacency(start_id, id, end_id);
CREATE INDEX wcoj_generic_f3_out
ON wcoj_generic."F3" USING age_adjacency(start_id, id, end_id);

ANALYZE wcoj_generic."A";
ANALYZE wcoj_generic."B";
ANALYZE wcoj_generic."C";
ANALYZE wcoj_generic."D";
ANALYZE wcoj_generic."F1";
ANALYZE wcoj_generic."F2";
ANALYZE wcoj_generic."F3";

DO $wcoj_acyclic_semijoin_plan$
DECLARE
    plan_text text;
    has_generic boolean := false;
    has_reduction_shape boolean := false;
    has_reduction_order boolean := false;
    has_reduction_order_edges boolean := false;
    has_semijoin_passes boolean := false;
    has_bottom_up_pass boolean := false;
    has_top_down_pass boolean := false;
    has_reduction_order_applied boolean := false;
    has_rows_removed boolean := false;
    has_provider_rows_after boolean := false;
    has_final_domain_keys boolean := false;
    has_rows_emitted boolean := false;
    has_spill_bytes boolean := false;
BEGIN
    PERFORM set_config('age.enable_wcoj', 'on', true);
    PERFORM set_config('enable_nestloop', 'off', true);
    PERFORM set_config('enable_hashjoin', 'off', true);
    PERFORM set_config('enable_mergejoin', 'off', true);

    FOR plan_text IN EXECUTE $plan$
        EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY OFF)
        SELECT *
        FROM cypher('wcoj_generic', $cypher$
            MATCH (a:A)-[f1:F1]->(b:B)-[f2:F2]->(c:C)-[f3:F3]->(d:D)
            RETURN id(a), id(b), id(c), id(d), id(f1), id(f2), id(f3)
        $cypher$) AS (a agtype, b agtype, c agtype, d agtype,
                      f1 agtype, f2 agtype, f3 agtype)
    $plan$
    LOOP
        has_generic := has_generic OR
            plan_text LIKE '%Custom Scan (AGE Generic Multiway Join)%';
        has_reduction_shape := has_reduction_shape OR
            plan_text LIKE '%Reduction Shape: alpha-acyclic%';
        has_reduction_order := has_reduction_order OR
            plan_text LIKE '%Reduction Order: leaf-peel%';
        has_reduction_order_edges := has_reduction_order_edges OR
            plan_text LIKE '%Reduction Order Edges: 3%';
        has_semijoin_passes := has_semijoin_passes OR
            plan_text LIKE '%Semijoin Reduction Passes: 2%';
        has_bottom_up_pass := has_bottom_up_pass OR
            plan_text LIKE '%Yannakakis Bottom-Up Passes: 1%';
        has_top_down_pass := has_top_down_pass OR
            plan_text LIKE '%Yannakakis Top-Down Passes: 1%';
        has_reduction_order_applied := has_reduction_order_applied OR
            plan_text LIKE '%Reduction Order Applied: true%';
        has_rows_removed := has_rows_removed OR
            plan_text LIKE '%Semijoin Rows Removed: 508%';
        has_provider_rows_after := has_provider_rows_after OR
            plan_text LIKE '%Semijoin Provider Rows After: 261%';
        has_final_domain_keys := has_final_domain_keys OR
            plan_text LIKE '%Semijoin Final Domain Keys: 131%';
        has_rows_emitted := has_rows_emitted OR
            plan_text LIKE '%Rows Emitted: 128%';
        has_spill_bytes := has_spill_bytes OR
            plan_text LIKE '%Spill Bytes: 0 bytes%';
    END LOOP;

    IF NOT has_generic OR NOT has_reduction_shape OR
       NOT has_reduction_order OR NOT has_reduction_order_edges OR
       NOT has_semijoin_passes OR NOT has_bottom_up_pass OR
       NOT has_top_down_pass OR NOT has_reduction_order_applied OR
       NOT has_rows_removed OR NOT has_provider_rows_after OR
       NOT has_final_domain_keys OR NOT has_rows_emitted OR
       NOT has_spill_bytes THEN
        RAISE EXCEPTION 'acyclic Generic Join semijoin reduction was not observed';
    END IF;
    RAISE NOTICE 'wcoj acyclic semijoin reduction verified';
END
$wcoj_acyclic_semijoin_plan$;

BEGIN;
SET LOCAL age.enable_wcoj = off;
CREATE TEMP TABLE generic_binary_chain ON COMMIT DROP AS
SELECT * FROM cypher('wcoj_generic', $$
    MATCH (a:A)-[f1:F1]->(b:B)-[f2:F2]->(c:C)-[f3:F3]->(d:D)
    RETURN id(a), id(b), id(c), id(d), id(f1), id(f2), id(f3)
$$) AS (a agtype, b agtype, c agtype, d agtype,
        f1 agtype, f2 agtype, f3 agtype);

SET LOCAL age.enable_wcoj = on;
SET LOCAL enable_nestloop = off;
SET LOCAL enable_hashjoin = off;
SET LOCAL enable_mergejoin = off;
CREATE TEMP TABLE generic_semijoin_chain ON COMMIT DROP AS
SELECT * FROM cypher('wcoj_generic', $$
    MATCH (a:A)-[f1:F1]->(b:B)-[f2:F2]->(c:C)-[f3:F3]->(d:D)
    RETURN id(a), id(b), id(c), id(d), id(f1), id(f2), id(f3)
$$) AS (a agtype, b agtype, c agtype, d agtype,
        f1 agtype, f2 agtype, f3 agtype);

SELECT (SELECT count(*) FROM generic_binary_chain) AS binary_rows,
       (SELECT count(*) FROM generic_semijoin_chain) AS generic_rows,
       (SELECT count(*) FROM (
           (TABLE generic_binary_chain
            EXCEPT ALL TABLE generic_semijoin_chain)
           UNION ALL
           (TABLE generic_semijoin_chain
            EXCEPT ALL TABLE generic_binary_chain)
       ) diff) AS diff_rows;
ROLLBACK;

-- A branching acyclic tree is not a star: C participates in the chain and in
-- one leaf edge.  Semijoin reduction must still shrink the shared C domain
-- before enumerating the A-side multiplicity.
SELECT create_vlabel('wcoj_generic', 'Q');
SELECT create_elabel('wcoj_generic', 'G1');
SELECT create_elabel('wcoj_generic', 'G2');
SELECT create_elabel('wcoj_generic', 'G3');
SELECT create_elabel('wcoj_generic', 'G4');
INSERT INTO wcoj_generic."Q" (id, properties)
VALUES (_graphid(_label_id('wcoj_generic', 'Q'), 1), '{}'::agtype);
INSERT INTO wcoj_generic."G1" (id, start_id, end_id, properties)
SELECT _graphid(_label_id('wcoj_generic', 'G1'), i),
       _graphid(_label_id('wcoj_generic', 'A'), i),
       _graphid(_label_id('wcoj_generic', 'B'), 1),
       '{}'::agtype
FROM generate_series(1, 128) i;
INSERT INTO wcoj_generic."G2" (id, start_id, end_id, properties)
SELECT _graphid(_label_id('wcoj_generic', 'G2'), i),
       _graphid(_label_id('wcoj_generic', 'B'), 1),
       _graphid(_label_id('wcoj_generic', 'C'), i),
       '{}'::agtype
FROM generate_series(1, 128) i;
INSERT INTO wcoj_generic."G3" (id, start_id, end_id, properties)
SELECT _graphid(_label_id('wcoj_generic', 'G3'), i),
       _graphid(_label_id('wcoj_generic', 'C'), 127 + i),
       _graphid(_label_id('wcoj_generic', 'D'), 1),
       '{}'::agtype
FROM generate_series(1, 128) i;
INSERT INTO wcoj_generic."G4" (id, start_id, end_id, properties)
VALUES (_graphid(_label_id('wcoj_generic', 'G4'), 1),
        _graphid(_label_id('wcoj_generic', 'C'), 128),
        _graphid(_label_id('wcoj_generic', 'Q'), 1),
        '{}'::agtype);
CREATE INDEX wcoj_generic_g1_out
ON wcoj_generic."G1" USING age_adjacency(start_id, id, end_id);
CREATE INDEX wcoj_generic_g2_out
ON wcoj_generic."G2" USING age_adjacency(start_id, id, end_id);
CREATE INDEX wcoj_generic_g3_out
ON wcoj_generic."G3" USING age_adjacency(start_id, id, end_id);
CREATE INDEX wcoj_generic_g4_out
ON wcoj_generic."G4" USING age_adjacency(start_id, id, end_id);
ANALYZE wcoj_generic."Q";
ANALYZE wcoj_generic."G1";
ANALYZE wcoj_generic."G2";
ANALYZE wcoj_generic."G3";
ANALYZE wcoj_generic."G4";

DO $wcoj_tree_semijoin_plan$
DECLARE
    plan_text text;
    has_generic boolean := false;
    has_reduction_shape boolean := false;
    has_reduction_order boolean := false;
    has_reduction_order_edges boolean := false;
    has_semijoin_passes boolean := false;
    has_bottom_up_pass boolean := false;
    has_top_down_pass boolean := false;
    has_reduction_order_applied boolean := false;
    has_rows_removed boolean := false;
    has_provider_rows_after boolean := false;
    has_final_domain_keys boolean := false;
    has_rows_emitted boolean := false;
BEGIN
    PERFORM set_config('age.enable_wcoj', 'on', true);
    PERFORM set_config('enable_nestloop', 'off', true);
    PERFORM set_config('enable_hashjoin', 'off', true);
    PERFORM set_config('enable_mergejoin', 'off', true);

    FOR plan_text IN EXECUTE $plan$
        EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY OFF)
        SELECT *
        FROM cypher('wcoj_generic', $cypher$
            MATCH (a:A)-[g1:G1]->(b:B)-[g2:G2]->(c:C)-[g3:G3]->(d:D),
                  (c)-[g4:G4]->(q:Q)
            RETURN id(a), id(b), id(c), id(d), id(q),
                   id(g1), id(g2), id(g3), id(g4)
        $cypher$) AS (a agtype, b agtype, c agtype, d agtype, q agtype,
                      g1 agtype, g2 agtype, g3 agtype, g4 agtype)
    $plan$
    LOOP
        has_generic := has_generic OR
            plan_text LIKE '%Custom Scan (AGE Generic Multiway Join)%';
        has_reduction_shape := has_reduction_shape OR
            plan_text LIKE '%Reduction Shape: alpha-acyclic%';
        has_reduction_order := has_reduction_order OR
            plan_text LIKE '%Reduction Order: leaf-peel%';
        has_reduction_order_edges := has_reduction_order_edges OR
            plan_text LIKE '%Reduction Order Edges: 4%';
        has_semijoin_passes := has_semijoin_passes OR
            plan_text LIKE '%Semijoin Reduction Passes: 2%';
        has_bottom_up_pass := has_bottom_up_pass OR
            plan_text LIKE '%Yannakakis Bottom-Up Passes: 1%';
        has_top_down_pass := has_top_down_pass OR
            plan_text LIKE '%Yannakakis Top-Down Passes: 1%';
        has_reduction_order_applied := has_reduction_order_applied OR
            plan_text LIKE '%Reduction Order Applied: true%';
        has_rows_removed := has_rows_removed OR
            plan_text LIKE '%Semijoin Rows Removed: 508%';
        has_provider_rows_after := has_provider_rows_after OR
            plan_text LIKE '%Semijoin Provider Rows After: 263%';
        has_final_domain_keys := has_final_domain_keys OR
            plan_text LIKE '%Semijoin Final Domain Keys: 132%';
        has_rows_emitted := has_rows_emitted OR
            plan_text LIKE '%Rows Emitted: 128%';
    END LOOP;

    IF NOT has_generic OR NOT has_reduction_shape OR
       NOT has_reduction_order OR NOT has_reduction_order_edges OR
       NOT has_semijoin_passes OR NOT has_bottom_up_pass OR
       NOT has_top_down_pass OR NOT has_reduction_order_applied OR
       NOT has_rows_removed OR NOT has_provider_rows_after OR
       NOT has_final_domain_keys OR NOT has_rows_emitted THEN
        RAISE EXCEPTION 'branching Generic Join semijoin reduction was not observed';
    END IF;
    RAISE NOTICE 'wcoj branching semijoin reduction verified';
END
$wcoj_tree_semijoin_plan$;

BEGIN;
SET LOCAL age.enable_wcoj = off;
CREATE TEMP TABLE generic_binary_tree ON COMMIT DROP AS
SELECT * FROM cypher('wcoj_generic', $$
    MATCH (a:A)-[g1:G1]->(b:B)-[g2:G2]->(c:C)-[g3:G3]->(d:D),
          (c)-[g4:G4]->(q:Q)
    RETURN id(a), id(b), id(c), id(d), id(q),
           id(g1), id(g2), id(g3), id(g4)
$$) AS (a agtype, b agtype, c agtype, d agtype, q agtype,
        g1 agtype, g2 agtype, g3 agtype, g4 agtype);

SET LOCAL age.enable_wcoj = on;
SET LOCAL enable_nestloop = off;
SET LOCAL enable_hashjoin = off;
SET LOCAL enable_mergejoin = off;
CREATE TEMP TABLE generic_semijoin_tree ON COMMIT DROP AS
SELECT * FROM cypher('wcoj_generic', $$
    MATCH (a:A)-[g1:G1]->(b:B)-[g2:G2]->(c:C)-[g3:G3]->(d:D),
          (c)-[g4:G4]->(q:Q)
    RETURN id(a), id(b), id(c), id(d), id(q),
           id(g1), id(g2), id(g3), id(g4)
$$) AS (a agtype, b agtype, c agtype, d agtype, q agtype,
        g1 agtype, g2 agtype, g3 agtype, g4 agtype);

SELECT (SELECT count(*) FROM generic_binary_tree) AS binary_rows,
       (SELECT count(*) FROM generic_semijoin_tree) AS generic_rows,
       (SELECT count(*) FROM (
           (TABLE generic_binary_tree
            EXCEPT ALL TABLE generic_semijoin_tree)
           UNION ALL
           (TABLE generic_semijoin_tree
            EXCEPT ALL TABLE generic_binary_tree)
       ) diff) AS diff_rows;
ROLLBACK;

-- A snowflake-shaped acyclic component has several leaves sharing the
-- reduced C separator.  This keeps the query outside the star fast path while
-- verifying that semijoin pruning propagates across more than one branch.
SELECT create_vlabel('wcoj_generic', 'R');
SELECT create_vlabel('wcoj_generic', 'S');
SELECT create_elabel('wcoj_generic', 'H1');
SELECT create_elabel('wcoj_generic', 'H2');
SELECT create_elabel('wcoj_generic', 'H3');
SELECT create_elabel('wcoj_generic', 'H4');
SELECT create_elabel('wcoj_generic', 'H5');
SELECT create_elabel('wcoj_generic', 'H6');
INSERT INTO wcoj_generic."R" (id, properties)
VALUES (_graphid(_label_id('wcoj_generic', 'R'), 1), '{}'::agtype);
INSERT INTO wcoj_generic."S" (id, properties)
VALUES (_graphid(_label_id('wcoj_generic', 'S'), 1), '{}'::agtype);
INSERT INTO wcoj_generic."H1" (id, start_id, end_id, properties)
SELECT _graphid(_label_id('wcoj_generic', 'H1'), i),
       _graphid(_label_id('wcoj_generic', 'A'), i),
       _graphid(_label_id('wcoj_generic', 'B'), 1),
       '{}'::agtype
FROM generate_series(1, 128) i;
INSERT INTO wcoj_generic."H2" (id, start_id, end_id, properties)
SELECT _graphid(_label_id('wcoj_generic', 'H2'), i),
       _graphid(_label_id('wcoj_generic', 'B'), 1),
       _graphid(_label_id('wcoj_generic', 'C'), i),
       '{}'::agtype
FROM generate_series(1, 128) i;
INSERT INTO wcoj_generic."H3" (id, start_id, end_id, properties)
SELECT _graphid(_label_id('wcoj_generic', 'H3'), i),
       _graphid(_label_id('wcoj_generic', 'C'), 127 + i),
       _graphid(_label_id('wcoj_generic', 'D'), 1),
       '{}'::agtype
FROM generate_series(1, 128) i;
INSERT INTO wcoj_generic."H4" (id, start_id, end_id, properties)
VALUES (_graphid(_label_id('wcoj_generic', 'H4'), 1),
        _graphid(_label_id('wcoj_generic', 'C'), 128),
        _graphid(_label_id('wcoj_generic', 'Q'), 1),
        '{}'::agtype);
INSERT INTO wcoj_generic."H5" (id, start_id, end_id, properties)
VALUES (_graphid(_label_id('wcoj_generic', 'H5'), 1),
        _graphid(_label_id('wcoj_generic', 'R'), 1),
        _graphid(_label_id('wcoj_generic', 'C'), 128),
        '{}'::agtype);
INSERT INTO wcoj_generic."H6" (id, start_id, end_id, properties)
VALUES (_graphid(_label_id('wcoj_generic', 'H6'), 1),
        _graphid(_label_id('wcoj_generic', 'C'), 128),
        _graphid(_label_id('wcoj_generic', 'S'), 1),
        '{}'::agtype);
CREATE INDEX wcoj_generic_h1_out
ON wcoj_generic."H1" USING age_adjacency(start_id, id, end_id);
CREATE INDEX wcoj_generic_h2_out
ON wcoj_generic."H2" USING age_adjacency(start_id, id, end_id);
CREATE INDEX wcoj_generic_h3_out
ON wcoj_generic."H3" USING age_adjacency(start_id, id, end_id);
CREATE INDEX wcoj_generic_h4_out
ON wcoj_generic."H4" USING age_adjacency(start_id, id, end_id);
CREATE INDEX wcoj_generic_h5_out
ON wcoj_generic."H5" USING age_adjacency(start_id, id, end_id);
CREATE INDEX wcoj_generic_h6_out
ON wcoj_generic."H6" USING age_adjacency(start_id, id, end_id);
ANALYZE wcoj_generic."R";
ANALYZE wcoj_generic."S";
ANALYZE wcoj_generic."H1";
ANALYZE wcoj_generic."H2";
ANALYZE wcoj_generic."H3";
ANALYZE wcoj_generic."H4";
ANALYZE wcoj_generic."H5";
ANALYZE wcoj_generic."H6";

DO $wcoj_snowflake_semijoin_plan$
DECLARE
    plan_text text;
    has_generic boolean := false;
    has_reduction_shape boolean := false;
    has_reduction_order boolean := false;
    has_reduction_order_edges boolean := false;
    has_semijoin_passes boolean := false;
    has_bottom_up_pass boolean := false;
    has_top_down_pass boolean := false;
    has_reduction_order_applied boolean := false;
    has_rows_removed boolean := false;
    has_provider_rows_after boolean := false;
    has_final_domain_keys boolean := false;
    has_rows_emitted boolean := false;
BEGIN
    PERFORM set_config('age.enable_wcoj', 'on', true);
    PERFORM set_config('enable_nestloop', 'off', true);
    PERFORM set_config('enable_hashjoin', 'off', true);
    PERFORM set_config('enable_mergejoin', 'off', true);

    FOR plan_text IN EXECUTE $plan$
        EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY OFF)
        SELECT *
        FROM cypher('wcoj_generic', $cypher$
            MATCH (a:A)-[h1:H1]->(b:B)-[h2:H2]->(c:C)-[h3:H3]->(d:D),
                  (c)-[h4:H4]->(q:Q),
                  (r:R)-[h5:H5]->(c),
                  (c)-[h6:H6]->(s:S)
            RETURN id(a), id(b), id(c), id(d), id(q), id(r), id(s),
                   id(h1), id(h2), id(h3), id(h4), id(h5), id(h6)
        $cypher$) AS (a agtype, b agtype, c agtype, d agtype, q agtype,
                      r agtype, s agtype, h1 agtype, h2 agtype, h3 agtype,
                      h4 agtype, h5 agtype, h6 agtype)
    $plan$
    LOOP
        has_generic := has_generic OR
            plan_text LIKE '%Custom Scan (AGE Generic Multiway Join)%';
        has_reduction_shape := has_reduction_shape OR
            plan_text LIKE '%Reduction Shape: alpha-acyclic%';
        has_reduction_order := has_reduction_order OR
            plan_text LIKE '%Reduction Order: leaf-peel%';
        has_reduction_order_edges := has_reduction_order_edges OR
            plan_text LIKE '%Reduction Order Edges: 6%';
        has_semijoin_passes := has_semijoin_passes OR
            plan_text LIKE '%Semijoin Reduction Passes: 2%';
        has_bottom_up_pass := has_bottom_up_pass OR
            plan_text LIKE '%Yannakakis Bottom-Up Passes: 1%';
        has_top_down_pass := has_top_down_pass OR
            plan_text LIKE '%Yannakakis Top-Down Passes: 1%';
        has_reduction_order_applied := has_reduction_order_applied OR
            plan_text LIKE '%Reduction Order Applied: true%';
        has_rows_removed := has_rows_removed OR
            plan_text LIKE '%Semijoin Rows Removed: 508%';
        has_provider_rows_after := has_provider_rows_after OR
            plan_text LIKE '%Semijoin Provider Rows After: 267%';
        has_final_domain_keys := has_final_domain_keys OR
            plan_text LIKE '%Semijoin Final Domain Keys: 134%';
        has_rows_emitted := has_rows_emitted OR
            plan_text LIKE '%Rows Emitted: 128%';
    END LOOP;

    IF NOT has_generic OR NOT has_reduction_shape OR
       NOT has_reduction_order OR NOT has_reduction_order_edges OR
       NOT has_semijoin_passes OR NOT has_bottom_up_pass OR
       NOT has_top_down_pass OR NOT has_reduction_order_applied OR
       NOT has_rows_removed OR NOT has_provider_rows_after OR
       NOT has_final_domain_keys OR NOT has_rows_emitted THEN
        RAISE EXCEPTION 'snowflake Generic Join semijoin reduction was not observed';
    END IF;
    RAISE NOTICE 'wcoj snowflake semijoin reduction verified';
END
$wcoj_snowflake_semijoin_plan$;

BEGIN;
SET LOCAL age.enable_wcoj = off;
CREATE TEMP TABLE generic_binary_snowflake ON COMMIT DROP AS
SELECT * FROM cypher('wcoj_generic', $$
    MATCH (a:A)-[h1:H1]->(b:B)-[h2:H2]->(c:C)-[h3:H3]->(d:D),
          (c)-[h4:H4]->(q:Q),
          (r:R)-[h5:H5]->(c),
          (c)-[h6:H6]->(s:S)
    RETURN id(a), id(b), id(c), id(d), id(q), id(r), id(s),
           id(h1), id(h2), id(h3), id(h4), id(h5), id(h6)
$$) AS (a agtype, b agtype, c agtype, d agtype, q agtype,
        r agtype, s agtype, h1 agtype, h2 agtype, h3 agtype,
        h4 agtype, h5 agtype, h6 agtype);

SET LOCAL age.enable_wcoj = on;
SET LOCAL enable_nestloop = off;
SET LOCAL enable_hashjoin = off;
SET LOCAL enable_mergejoin = off;
CREATE TEMP TABLE generic_semijoin_snowflake ON COMMIT DROP AS
SELECT * FROM cypher('wcoj_generic', $$
    MATCH (a:A)-[h1:H1]->(b:B)-[h2:H2]->(c:C)-[h3:H3]->(d:D),
          (c)-[h4:H4]->(q:Q),
          (r:R)-[h5:H5]->(c),
          (c)-[h6:H6]->(s:S)
    RETURN id(a), id(b), id(c), id(d), id(q), id(r), id(s),
           id(h1), id(h2), id(h3), id(h4), id(h5), id(h6)
$$) AS (a agtype, b agtype, c agtype, d agtype, q agtype,
        r agtype, s agtype, h1 agtype, h2 agtype, h3 agtype,
        h4 agtype, h5 agtype, h6 agtype);

SELECT (SELECT count(*) FROM generic_binary_snowflake) AS binary_rows,
       (SELECT count(*) FROM generic_semijoin_snowflake) AS generic_rows,
       (SELECT count(*) FROM (
           (TABLE generic_binary_snowflake
            EXCEPT ALL TABLE generic_semijoin_snowflake)
           UNION ALL
           (TABLE generic_semijoin_snowflake
            EXCEPT ALL TABLE generic_binary_snowflake)
       ) diff) AS diff_rows;
ROLLBACK;

SELECT drop_graph('wcoj_generic', true);

-- A parameterized WCOJ path must preserve required_outer through its Sort
-- wrappers and rebuild cleanly when a Nested Loop changes the source key.
SELECT create_graph('wcoj_parameterized');
SELECT create_vlabel('wcoj_parameterized', 'S');
SELECT create_vlabel('wcoj_parameterized', 'T');
SELECT create_elabel('wcoj_parameterized', 'E');
SELECT * FROM cypher('wcoj_parameterized', $$
    CREATE (s1:S {id:1}), (s2:S {id:2}), (s3:S {id:3}),
           (t10:T {id:10}), (t11:T {id:11}),
           (s1)-[:E]->(t10), (s1)-[:E]->(t11),
           (s2)-[:E]->(t10), (s2)-[:E]->(t11),
           (s3)-[:E]->(t10)
$$) AS (z agtype);
CREATE INDEX wcoj_parameterized_e_out
ON wcoj_parameterized."E" USING age_adjacency(start_id, id, end_id);
ANALYZE wcoj_parameterized."S";
ANALYZE wcoj_parameterized."T";
ANALYZE wcoj_parameterized."E";

DO $wcoj_parameterized_plan$
DECLARE
    plan_text text;
    has_parameterized_wcoj boolean := false;
    has_three_loops boolean := false;
    has_five_rows boolean := false;
    has_three_rescans boolean := false;
BEGIN
    PERFORM set_config('age.enable_wcoj', 'on', true);
    PERFORM set_config('age.wcoj_engine', 'auto', true);
    PERFORM set_config('enable_hashjoin', 'off', true);
    PERFORM set_config('enable_mergejoin', 'off', true);
    PERFORM set_config('enable_memoize', 'off', true);

    FOR plan_text IN EXECUTE $plan$
        EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY OFF)
        SELECT *
        FROM cypher('wcoj_parameterized', $cypher$
            MATCH (s:S)-[e1:E]->(t:T), (s)-[e2:E]->(t)
            RETURN id(s), id(t), id(e1), id(e2)
        $cypher$) AS (sid agtype, tid agtype, e1 agtype, e2 agtype)
    $plan$
    LOOP
        IF plan_text LIKE '%Custom Scan (AGE WCOJ Multiway Join)%' THEN
            has_parameterized_wcoj := true;
            has_three_loops := plan_text LIKE '%loops=3%';
        END IF;
        has_five_rows := has_five_rows OR
            plan_text LIKE '%Rows Emitted: 5%';
        has_three_rescans := has_three_rescans OR
            plan_text LIKE '%Rescans: 3%';
    END LOOP;

    IF NOT has_parameterized_wcoj OR NOT has_three_loops OR
       NOT has_five_rows OR NOT has_three_rescans THEN
        RAISE EXCEPTION
            'parameterized WCOJ did not preserve cumulative rescan telemetry';
    END IF;
    RAISE NOTICE 'parameterized wcoj rescan verified';
END
$wcoj_parameterized_plan$;

BEGIN;
SET LOCAL age.enable_wcoj = off;
CREATE TEMP TABLE parameterized_binary ON COMMIT DROP AS
SELECT * FROM cypher('wcoj_parameterized', $$
    MATCH (s:S)-[e1:E]->(t:T), (s)-[e2:E]->(t)
    RETURN id(s), id(t), id(e1), id(e2)
$$) AS (sid agtype, tid agtype, e1 agtype, e2 agtype);

SET LOCAL age.enable_wcoj = on;
SET LOCAL age.wcoj_engine = 'auto';
SET LOCAL enable_hashjoin = off;
SET LOCAL enable_mergejoin = off;
SET LOCAL enable_memoize = off;
CREATE TEMP TABLE parameterized_wcoj ON COMMIT DROP AS
SELECT * FROM cypher('wcoj_parameterized', $$
    MATCH (s:S)-[e1:E]->(t:T), (s)-[e2:E]->(t)
    RETURN id(s), id(t), id(e1), id(e2)
$$) AS (sid agtype, tid agtype, e1 agtype, e2 agtype);

SELECT (SELECT count(*) FROM parameterized_binary) AS binary_rows,
       (SELECT count(*) FROM parameterized_wcoj) AS wcoj_rows,
       (SELECT count(*) FROM (
           (TABLE parameterized_binary EXCEPT ALL TABLE parameterized_wcoj)
           UNION ALL
           (TABLE parameterized_wcoj EXCEPT ALL TABLE parameterized_binary)
       ) diff) AS diff_rows;
ROLLBACK;
SELECT drop_graph('wcoj_parameterized', true);
