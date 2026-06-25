/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

LOAD 'age';
SET search_path TO ag_catalog;

SELECT p.proname || ':' ||
       a.aggcombinefn::regproc::text || ':' ||
       a.aggserialfn::regproc::text || ':' ||
       a.aggdeserialfn::regproc::text AS slots_aggregate_parallel_contract
FROM pg_aggregate a
JOIN pg_proc p ON p.oid = a.aggfnoid
WHERE p.proname IN ('age_array_agg_map_slots',
                    'age_array_agg_list_slots')
ORDER BY p.proname;

BEGIN;
SET LOCAL max_parallel_workers_per_gather = 4;
SET LOCAL min_parallel_table_scan_size = 0;
SET LOCAL parallel_setup_cost = 0;
SET LOCAL parallel_tuple_cost = 0;

CREATE TABLE age_slots_parallel_scalar AS
SELECT CASE WHEN i % 3 = 0 THEN NULL::agtype ELSE '1'::agtype END AS v
FROM generate_series(1, 20000) AS g(i);

CREATE TABLE age_slots_parallel_object AS
SELECT '{"a": 1}'::agtype AS v
FROM generate_series(1, 20000) AS g(i);

CREATE TABLE age_slots_parallel_typed AS
SELECT i::bigint AS i, (i + 0.5)::numeric AS n, ('slot-' || i)::text AS t
FROM generate_series(1, 20000) AS g(i);

ALTER TABLE age_slots_parallel_scalar SET (parallel_workers = 4);
ALTER TABLE age_slots_parallel_object SET (parallel_workers = 4);
ALTER TABLE age_slots_parallel_typed SET (parallel_workers = 4);

EXPLAIN (COSTS OFF)
SELECT array_length(age_array_agg_list_slots(v), 1)
FROM age_slots_parallel_scalar;

SELECT array_length(age_array_agg_list_slots(v), 1) AS scalar_slots,
       count(*) AS scalar_rows
FROM age_slots_parallel_scalar;

SELECT age_array_agg_list_slots_summary(v) AS scalar_slot_state
FROM age_slots_parallel_scalar;

SELECT array_length(age_array_agg_list_slots(v), 1) AS object_slots,
       count(*) AS object_rows
FROM age_slots_parallel_object;

EXPLAIN (COSTS OFF)
SELECT array_length(age_array_agg_list_slots(i, n, t), 1)
FROM age_slots_parallel_typed;

SELECT array_length(age_array_agg_list_slots(i, n, t), 1) AS typed_slots,
       count(*) AS typed_rows
FROM age_slots_parallel_typed;

SELECT age_array_agg_list_slots(1::bigint, 2.5::numeric, 'slot'::text)
       AS typed_list_slots;

SELECT age_array_agg_list_slots_summary(i, n, t) AS typed_list_slot_state
FROM age_slots_parallel_typed;

SELECT age_array_agg_list_slots_summary(i, i) AS repeated_source_slot_state
FROM age_slots_parallel_typed;

SELECT age_array_agg_map_slots_summary('i'::text, i, 'n'::text, n,
                                       't'::text, 'slot'::text)
       AS typed_map_slot_state
FROM age_slots_parallel_typed;

ROLLBACK;

SELECT create_graph('cypher_match');

SELECT * FROM cypher('cypher_match', $$CREATE (:v)$$) AS (a agtype);
SELECT * FROM cypher('cypher_match', $$CREATE (:v {i: 0})$$) AS (a agtype);
SELECT * FROM cypher('cypher_match', $$CREATE (:v {i: 1})$$) AS (a agtype);

SELECT * FROM cypher('cypher_match', $$MATCH (n:v) RETURN n$$) AS (n agtype);
SELECT * FROM cypher('cypher_match', $$MATCH (n:v) RETURN n.i$$) AS (i agtype);

EXPLAIN (VERBOSE, COSTS OFF)
SELECT *
FROM cypher('cypher_match',
            $$MATCH (n:v) RETURN n.i$$) AS (i agtype);

EXPLAIN (VERBOSE, COSTS OFF)
SELECT *
FROM cypher('cypher_match',
            $$MATCH (n:v) RETURN n.i::pg_bigint$$) AS (i bigint);

EXPLAIN (VERBOSE, COSTS OFF)
SELECT *
FROM cypher('cypher_match',
            $$MATCH (n:v) RETURN n.i::pg_bigint, n.i::pg_float8$$)
     AS (i bigint, f float8);

SELECT *
FROM cypher('cypher_match',
            $$MATCH (n:v) RETURN n.i::pg_bigint, n.i::pg_float8$$)
     AS (i bigint, f float8)
ORDER BY i NULLS FIRST;

SELECT *
FROM cypher('cypher_match',
            $$MATCH (n:v) RETURN n.i::pg_bigint$$) AS (i bigint)
ORDER BY i NULLS FIRST;

SELECT * FROM cypher('cypher_match', $$
MATCH (n:v) WHERE n.i > 0
RETURN n.i
$$) AS (i agtype);

SELECT create_graph('cypher_match_plan');

SELECT * FROM cypher('cypher_match_plan', $$
CREATE (:join_v {i: 3})-[:order_e {i: 2}]->(:join_v),
       (:join_v {i: 1})-[:order_e {i: 1}]->(:join_v)
$$) AS (a agtype);

SELECT create_graph('cypher_match_numeric_path');

SELECT * FROM cypher('cypher_match_numeric_path', $$
CREATE (:NumericPath {payload: {a: 1, b: 11, c: 101}}),
       (:NumericPath {payload: {a: 2, b: 12, c: 102}})
$$) AS (a agtype);

CREATE FUNCTION cypher_match_normalized_explain(graph_name text,
                                                cypher_query text)
RETURNS SETOF text
LANGUAGE plpgsql
AS $normalized_explain$
DECLARE
    plan_line text;
BEGIN
    FOR plan_line IN EXECUTE
        format('EXPLAIN (VERBOSE, COSTS OFF)
                SELECT *
                FROM cypher(%L, %L) AS (value agtype)',
               graph_name, cypher_query)
    LOOP
        plan_line := regexp_replace(
            plan_line,
            'agtype_ctid_property_field_agtype\(''[0-9]+''::oid',
            'agtype_ctid_property_field_agtype(''<relid>''::oid',
            'g');
        RETURN NEXT regexp_replace(
            plan_line,
            'agtype_id_property_field_agtype\(''[0-9]+''::oid',
            'agtype_id_property_field_agtype(''<relid>''::oid',
            'g');
    END LOOP;
END
$normalized_explain$;

SELECT * FROM cypher_match_normalized_explain(
    'cypher_match',
    'MATCH (n:v)
     RETURN n.payload.a
     ORDER BY n.payload.a::pg_bigint
     LIMIT 1');

SELECT * FROM cypher_match_normalized_explain(
    'cypher_match',
    'MATCH (n:v)
     RETURN n.payload.a::pg_bigint
     ORDER BY n.payload.a::pg_bigint
     LIMIT 1');

SELECT * FROM cypher_match_normalized_explain(
    'cypher_match_plan',
    'MATCH (:join_v)-[r:order_e]->(:join_v)
     RETURN r.i ORDER BY r.i::pg_bigint LIMIT 1');

SELECT * FROM cypher_match_normalized_explain(
    'cypher_match_numeric_path',
    'MATCH (n:NumericPath)
     RETURN collect(DISTINCT n.payload.a::pg_bigint)');

SELECT * FROM cypher_match_normalized_explain(
    'cypher_match_numeric_path',
    'MATCH (n:NumericPath)
     RETURN n.payload.a
     ORDER BY n.payload.a::pg_numeric
     LIMIT 1');

EXPLAIN (VERBOSE, COSTS OFF)
SELECT *
FROM cypher('cypher_match_numeric_path',
            $$MATCH (n:NumericPath)
              RETURN n.payload.a::pg_numeric$$) AS (i numeric);

SELECT *
FROM cypher('cypher_match_numeric_path',
            $$MATCH (n:NumericPath)
              RETURN n.payload.a::pg_numeric$$) AS (i numeric)
ORDER BY i;

SELECT * FROM cypher_match_normalized_explain(
    'cypher_match_numeric_path',
    'MATCH (n:NumericPath)
     RETURN collect(DISTINCT n.payload.a::pg_numeric)');

SELECT * FROM cypher_match_normalized_explain(
    'cypher_match_numeric_path',
    'MATCH (n:NumericPath)
     RETURN collect(n.payload.a::pg_numeric)');

EXPLAIN (VERBOSE, COSTS OFF)
SELECT *
FROM cypher('cypher_match_numeric_path',
            $$MATCH (n:NumericPath)
              RETURN n.payload.b::pg_bigint, collect(n.payload.a::pg_numeric)
              ORDER BY n.payload.b::pg_bigint$$)
     AS (b bigint, vals agtype);

EXPLAIN (VERBOSE, COSTS OFF)
SELECT *
FROM cypher('cypher_match_numeric_path',
            $$MATCH (n:NumericPath)
              RETURN collect(n.payload.a::pg_numeric),
                     collect(n.payload.b::pg_bigint)$$)
     AS (a agtype, b agtype);

SELECT * FROM cypher_match_normalized_explain(
    'cypher_match_numeric_path',
    'MATCH (n:NumericPath)
     RETURN collect(n.payload.a::numeric)');

EXPLAIN (VERBOSE, COSTS OFF)
SELECT *
FROM cypher('cypher_match_numeric_path',
            $$MATCH (n:NumericPath)
              RETURN n.payload.a::pg_numeric, count(*)
              ORDER BY n.payload.a::pg_numeric$$)
     AS (i numeric, c agtype);

EXPLAIN (VERBOSE, COSTS OFF)
SELECT *
FROM cypher('cypher_match_numeric_path',
            $$MATCH (n:NumericPath)
              WHERE n.payload.a::pg_numeric >= 1::pg_numeric
              RETURN n.payload.a::pg_numeric$$)
     AS (i numeric);

EXPLAIN (VERBOSE, COSTS OFF)
SELECT array_length(array_agg(v), 1)
FROM cypher('cypher_match_numeric_path',
            $$MATCH (n:NumericPath) RETURN n.payload.a$$) AS (v agtype);

EXPLAIN (VERBOSE, COSTS OFF)
SELECT array_length(array_agg(m), 1)
FROM cypher('cypher_match_numeric_path',
            $$MATCH (n:NumericPath) RETURN {a: n.payload.a, b: n.payload.b}$$)
     AS (m agtype);

EXPLAIN (VERBOSE, COSTS OFF)
SELECT array_length(array_agg(m), 1)
FROM cypher('cypher_match_numeric_path',
            $$MATCH (n:NumericPath)
              RETURN {a: n.payload.a, b: n.payload.b, c: n.payload.c}$$)
     AS (m agtype);

EXPLAIN (VERBOSE, COSTS OFF)
SELECT array_length(array_agg(l), 1)
FROM cypher('cypher_match_numeric_path',
            $$MATCH (n:NumericPath) RETURN [n.payload.a, n.payload.b]$$)
     AS (l agtype);

EXPLAIN (VERBOSE, COSTS OFF)
SELECT array_length(array_agg(l), 1)
FROM cypher('cypher_match_numeric_path',
            $$MATCH (n:NumericPath)
              RETURN [n.payload.a::pg_numeric, n.payload.b::pg_bigint]$$)
     AS (l agtype);

EXPLAIN (VERBOSE, COSTS OFF)
SELECT age_array_agg_list_slots_summary(a, b) AS typed_property_slot_state
FROM cypher('cypher_match_numeric_path',
            $$MATCH (n:NumericPath)
              RETURN n.payload.a::pg_numeric, n.payload.b::pg_bigint$$)
     AS (a numeric, b bigint);

SELECT age_array_agg_list_slots_summary(a, b) AS typed_property_slot_state
FROM cypher('cypher_match_numeric_path',
            $$MATCH (n:NumericPath)
              RETURN n.payload.a::pg_numeric, n.payload.b::pg_bigint$$)
     AS (a numeric, b bigint);

SELECT age_array_agg_slots_descriptor(NULL::numeric, NULL::bigint)
       AS typed_property_slot_descriptor,
       age_array_agg_list_slots_summary(a, b) AS typed_property_slot_state
FROM cypher('cypher_match_numeric_path',
            $$MATCH (n:NumericPath)
              RETURN n.payload.a::pg_numeric, n.payload.b::pg_bigint$$)
     AS (a numeric, b bigint);

EXPLAIN (VERBOSE, COSTS OFF)
SELECT array_length(array_agg(m), 1)
FROM cypher('cypher_match_numeric_path',
            $$MATCH (n:NumericPath)
              RETURN {a: n.payload.a::pg_numeric,
                      b: n.payload.b::pg_bigint}$$)
     AS (m agtype);

EXPLAIN (VERBOSE, COSTS OFF)
SELECT array_length(array_agg(m), 1)
FROM cypher('cypher_match_numeric_path',
            $$MATCH (n:NumericPath)
              RETURN {a: n.payload.a::pg_numeric,
                      b: n.payload.b::pg_bigint,
                      c: n.payload.c::pg_text}$$)
     AS (m agtype);

EXPLAIN (VERBOSE, COSTS OFF)
SELECT array_length(array_agg(m), 1)
FROM cypher('cypher_match_numeric_path',
            $$MATCH (n:NumericPath)
              RETURN {a: n.payload.a, again: n.payload.a}$$)
     AS (m agtype);

EXPLAIN (VERBOSE, COSTS OFF)
SELECT array_length(array_agg(l), 1)
FROM cypher('cypher_match_numeric_path',
            $$MATCH (n:NumericPath) RETURN [n.payload.a, n.payload.a]$$)
     AS (l agtype);

-- Bare list/cast projection (no outer aggregate) lowers onto the AGE Property
-- Projection custom scan instead of the generic agtype_build_list seq scan, and
-- the built list (including typed casts and missing -> null elements) matches
-- the generic path value-for-value.
SELECT * FROM cypher_match_normalized_explain(
    'cypher_match_numeric_path',
    'MATCH (n:NumericPath) RETURN [n.payload.a, n.payload.b]');

SELECT * FROM cypher('cypher_match_numeric_path',
            $$MATCH (n:NumericPath) RETURN [n.payload.a, n.payload.b]$$)
     AS (l agtype)
ORDER BY l;

SELECT * FROM cypher_match_normalized_explain(
    'cypher_match_numeric_path',
    'MATCH (n:NumericPath)
     RETURN [n.payload.a::pg_numeric, n.payload.b::pg_bigint, n.payload.z::pg_text]');

SELECT * FROM cypher('cypher_match_numeric_path',
            $$MATCH (n:NumericPath)
              RETURN [n.payload.a::pg_numeric, n.payload.b::pg_bigint,
                      n.payload.z::pg_text]$$)
     AS (l agtype)
ORDER BY l;

-- The custom-scan list must equal the generic agtype_build_list path; a second
-- non-property projection column disqualifies the simple-projection custom scan
-- and forces the generic path, so the EXCEPT must be empty.
SELECT count(*) AS list_projection_divergences FROM (
  (SELECT row_number() OVER () AS rn, l
   FROM cypher('cypher_match_numeric_path',
               $$MATCH (n:NumericPath)
                 RETURN [n.payload.a, n.payload.b::pg_bigint,
                         n.payload.c::pg_numeric, n.payload.z]$$)
        AS (l agtype))
  EXCEPT
  (SELECT row_number() OVER () AS rn, l
   FROM cypher('cypher_match_numeric_path',
               $$MATCH (n:NumericPath)
                 RETURN [n.payload.a, n.payload.b::pg_bigint,
                         n.payload.c::pg_numeric, n.payload.z],
                        n.payload.a + 1$$)
        AS (l agtype, junk agtype))
) d;

DROP FUNCTION cypher_match_normalized_explain(text, text);

-- Typed GROUP BY/DISTINCT keys must hash/sort on the scalar physical value
-- (agtype_object_field_int8/numeric/text result) rather than wrapping the key
-- back into agtype and paying generic agtype hash/equality.  Lock in the scalar
-- key plan shape (HashAggregate/GroupAggregate Group Key + Sort Key) and prove
-- the grouping/ordering/count semantics match the untyped agtype key.
SELECT create_graph('cypher_match_typed_key');
SELECT * FROM cypher('cypher_match_typed_key', $$
  CREATE (:K {payload: {n: 1, t: 'a'}}), (:K {payload: {n: 1, t: 'a'}}),
         (:K {payload: {n: 2, t: 'b'}}), (:K {payload: {n: 2, t: 'b'}}),
         (:K {payload: {n: 3, t: 'a'}})
$$) AS (r agtype);

-- typed GROUP BY: HashAggregate Group Key is the scalar int8 field accessor.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM cypher('cypher_match_typed_key',
            $$MATCH (n:K) RETURN n.payload.n::pg_bigint, count(*)$$)
     AS (k agtype, c agtype);

-- typed GROUP BY + ORDER BY: GroupAggregate Sort Key and Group Key both scalar.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM cypher('cypher_match_typed_key',
            $$MATCH (n:K) RETURN n.payload.n::pg_bigint, count(*)
              ORDER BY n.payload.n::pg_bigint$$)
     AS (k agtype, c agtype);

-- typed DISTINCT (int8 and text): HashAggregate Group Key is the scalar key.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM cypher('cypher_match_typed_key',
            $$MATCH (n:K) RETURN DISTINCT n.payload.n::pg_bigint$$)
     AS (k agtype);

EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM cypher('cypher_match_typed_key',
            $$MATCH (n:K) RETURN DISTINCT n.payload.t::pg_text$$)
     AS (k agtype);

-- Result semantics: typed GROUP BY counts match, ordered by the scalar key.
SELECT * FROM cypher('cypher_match_typed_key',
            $$MATCH (n:K) RETURN n.payload.n::pg_bigint AS k, count(*) AS c
              ORDER BY n.payload.n::pg_bigint$$) AS (k bigint, c agtype);

-- Equivalence vs the untyped agtype key: the typed int8 grouping must produce
-- the same (key, count) set as the agtype-keyed grouping, so the EXCEPT is empty.
SELECT count(*) AS typed_vs_agtype_group_divergences FROM (
  (SELECT k::text, c::text FROM cypher('cypher_match_typed_key',
            $$MATCH (n:K) RETURN n.payload.n::pg_bigint AS k, count(*) AS c$$)
       AS (k bigint, c agtype))
  EXCEPT
  (SELECT (k::text)::numeric::text, c::text FROM cypher('cypher_match_typed_key',
            $$MATCH (n:K) RETURN n.payload.n AS k, count(*) AS c$$)
       AS (k agtype, c agtype))
) d;

SELECT drop_graph('cypher_match_typed_key', true);

-- bool as a typed physical type (item 22): n.flag::pg_bool projects a native
-- boolean through the agtype_object_field_bool accessor, and collect() of a
-- typed bool routes to the age_collect_bool aggregate.
SELECT create_graph('cypher_match_bool');
SELECT * FROM cypher('cypher_match_bool', $$
  CREATE (:B {name: 'a', flag: true}),
         (:B {name: 'b', flag: false}),
         (:B {name: 'c', flag: true})
$$) AS (r agtype);

-- typed bool projection: a native boolean column via agtype_object_field_bool.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM cypher('cypher_match_bool',
            $$MATCH (n:B) RETURN n.flag::pg_bool$$) AS (flag boolean);

SELECT * FROM cypher('cypher_match_bool',
            $$MATCH (n:B) RETURN n.name, n.flag::pg_bool$$)
     AS (name agtype, flag boolean)
ORDER BY name;

-- typed bool collect: rewritten to age_collect_bool, returns an agtype array.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM cypher('cypher_match_bool',
            $$MATCH (n:B) RETURN collect(n.flag::pg_bool)$$) AS (flags agtype);

SELECT * FROM cypher('cypher_match_bool',
            $$MATCH (n:B) RETURN collect(n.flag::pg_bool)$$) AS (flags agtype);

-- typed DISTINCT and GROUP BY on the bool scalar key.
SELECT * FROM cypher('cypher_match_bool',
            $$MATCH (n:B) RETURN DISTINCT n.flag::pg_bool$$) AS (flag boolean)
ORDER BY flag;

SELECT * FROM cypher('cypher_match_bool',
            $$MATCH (n:B) RETURN n.flag::pg_bool AS flag, count(*) AS c
              ORDER BY n.flag::pg_bool$$) AS (flag boolean, c agtype);

-- Equivalence: the typed bool grouping must produce the same (flag, count) set
-- as the untyped agtype-keyed grouping, so the EXCEPT is empty.
SELECT count(*) AS typed_vs_agtype_bool_divergences FROM (
  (SELECT flag::text, c::text FROM cypher('cypher_match_bool',
            $$MATCH (n:B) RETURN n.flag::pg_bool AS flag, count(*) AS c$$)
       AS (flag boolean, c agtype))
  EXCEPT
  (SELECT (k::text)::boolean::text, c::text FROM cypher('cypher_match_bool',
            $$MATCH (n:B) RETURN n.flag AS k, count(*) AS c$$)
       AS (k agtype, c agtype))
) d;

SELECT drop_graph('cypher_match_bool', true);

SELECT * FROM cypher('cypher_match_numeric_path', $$
    MATCH (n:NumericPath)
    RETURN collect(n.payload.a::numeric)
$$) AS (vals agtype);

SELECT *
FROM cypher('cypher_match_numeric_path', $$
    MATCH (n:NumericPath)
    RETURN n.payload.b::pg_bigint, collect(n.payload.a::pg_numeric)
    ORDER BY n.payload.b::pg_bigint
$$) AS (b bigint, vals agtype);

SELECT *
FROM cypher('cypher_match_numeric_path', $$
    MATCH (n:NumericPath)
    RETURN collect(n.payload.a::pg_numeric), collect(n.payload.b::pg_bigint)
$$) AS (a agtype, b agtype);

SELECT * FROM cypher('cypher_match_numeric_path', $$
    MATCH (n:NumericPath)
    RETURN collect(DISTINCT n.payload.a::pg_numeric)
$$) AS (vals agtype);

SELECT array_agg(v)
FROM cypher('cypher_match_numeric_path',
            $$MATCH (n:NumericPath) RETURN n.payload.a$$) AS (v agtype);

SELECT array_agg(m)
FROM cypher('cypher_match_numeric_path',
            $$MATCH (n:NumericPath) RETURN {a: n.payload.a, b: n.payload.b}$$)
     AS (m agtype);

SELECT array_agg(l)
FROM cypher('cypher_match_numeric_path',
            $$MATCH (n:NumericPath) RETURN [n.payload.a, n.payload.b]$$)
     AS (l agtype);

SELECT array_agg(l)
FROM cypher('cypher_match_numeric_path',
            $$MATCH (n:NumericPath)
              RETURN [n.payload.a::pg_numeric, n.payload.b::pg_bigint]$$)
     AS (l agtype);

SELECT array_agg(m)
FROM cypher('cypher_match_numeric_path',
            $$MATCH (n:NumericPath)
              RETURN {a: n.payload.a::pg_numeric,
                      b: n.payload.b::pg_bigint}$$)
     AS (m agtype);

SELECT array_agg(m)
FROM cypher('cypher_match_numeric_path',
            $$MATCH (n:NumericPath)
              RETURN {a: n.payload.a::pg_numeric,
                      b: n.payload.b::pg_bigint,
                      c: n.payload.c::pg_text}$$)
     AS (m agtype);

EXPLAIN (VERBOSE, COSTS OFF)
SELECT *
FROM cypher('cypher_match',
            $$MATCH (n:v) RETURN count(n.i), count(n.i)$$)
     AS (a agtype, b agtype);

SELECT *
FROM cypher('cypher_match',
            $$MATCH (n:v) RETURN count(n.i), count(n.i)$$)
     AS (a agtype, b agtype);

EXPLAIN (VERBOSE, COSTS OFF)
SELECT *
FROM cypher('cypher_match',
            $$MATCH (n:v)
              RETURN n.i::pg_bigint, count(*), count(*)
              ORDER BY n.i::pg_bigint$$)
     AS (i agtype, a agtype, b agtype);

EXPLAIN (VERBOSE, COSTS OFF)
SELECT int8_to_agtype(
           agtype_object_field_int8(
               agtype_object_field_agtype(n.properties, '"payload"'::agtype),
               '"a"'::agtype)),
       int8_to_agtype(
           agtype_object_field_int8(
               agtype_access_operator(VARIADIC ARRAY[n.properties,
                                                     '"payload"'::agtype]),
               '"a"'::agtype))
FROM cypher_match_numeric_path."NumericPath" n;

DO $property_projection_plan$
DECLARE
    plan_text text;
    has_full_custom_scan boolean := false;
    has_count_custom_scan boolean := false;
    has_count_width_zero_scan boolean := false;
    has_count_arg_nonnull_probe boolean := false;
    has_count_arg_materialized_access boolean := false;
    has_limit_custom_scan boolean := false;
    has_limit_node boolean := false;
    has_order_by_direct_access boolean := false;
    has_order_by_materialized_access boolean := false;
    has_nested_count_arg_nonnull_probe boolean := false;
    has_nested_count_arg_materialized_access boolean := false;
    has_nested_count_cypher_nonnull_probe boolean := false;
    has_nested_count_cypher_materialized_access boolean := false;
    has_pg_bigint_property_helper boolean := false;
    has_pg_bigint_materialized_cast boolean := false;
    has_pg_float8_property_helper boolean := false;
    has_pg_float8_materialized_cast boolean := false;
    has_numeric_property_helper boolean := false;
    has_numeric_materialized_cast boolean := false;
    has_deferred_order_output boolean := false;
    has_deferred_order_limit_input boolean := false;
    has_deferred_float_order_output boolean := false;
    has_deferred_float_order_limit_input boolean := false;
    has_deferred_text_order_output boolean := false;
    has_deferred_text_order_limit_input boolean := false;
    has_deferred_numeric_order_output boolean := false;
    has_deferred_numeric_order_limit_input boolean := false;
    has_deferred_edge_order_output boolean := false;
    has_deferred_edge_order_limit_input boolean := false;
    has_deferred_join_order_output boolean := false;
    has_deferred_join_order_limit_input boolean := false;
    has_group_direct_access boolean := false;
    has_group_variadic_access boolean := false;
    has_nested_group_direct_access boolean := false;
    has_nested_group_variadic_access boolean := false;
    has_group_count_final_output boolean := false;
    has_group_count_limit_input boolean := false;
    has_group_count_plain_final_output boolean := false;
    has_group_count_plain_sort_input boolean := false;
    has_float_group_count_final_output boolean := false;
    has_float_group_count_limit_input boolean := false;
    has_float_group_count_plain_final_output boolean := false;
    has_float_group_count_plain_sort_input boolean := false;
    has_text_group_count_final_output boolean := false;
    has_text_group_count_limit_input boolean := false;
    has_text_group_count_plain_final_output boolean := false;
    has_text_group_count_plain_sort_input boolean := false;
    has_numeric_group_count_final_output boolean := false;
    has_numeric_group_count_limit_input boolean := false;
    has_numeric_group_count_plain_final_output boolean := false;
    has_numeric_group_count_plain_sort_input boolean := false;
    has_distinct_order_access boolean := false;
    has_distinct_deferred_order_output boolean := false;
    has_nested_distinct_order_access boolean := false;
    has_nested_distinct_variadic_access boolean := false;
BEGIN
    PERFORM set_config('enable_seqscan', 'on', false);
    PERFORM set_config('enable_indexscan', 'on', false);
    PERFORM set_config('enable_bitmapscan', 'on', false);

    FOR plan_text IN EXECUTE
        'EXPLAIN (VERBOSE, COSTS OFF)
         SELECT *
         FROM cypher(''cypher_match'',
                     $$MATCH (n:v) RETURN n.i$$) AS (i agtype)'
    LOOP
        IF plan_text LIKE '%Custom Scan (AGE Property Projection)%' THEN
            has_full_custom_scan := true;
        END IF;
    END LOOP;

    IF NOT has_full_custom_scan THEN
        RAISE EXCEPTION 'expected simple property projection to use AGE Property Projection CustomScan';
    END IF;

    FOR plan_text IN EXECUTE
        'EXPLAIN (VERBOSE, COSTS ON)
         SELECT count(*)
         FROM cypher(''cypher_match'',
                     $$MATCH (n:v) RETURN n.i$$) AS (i agtype)'
    LOOP
        IF plan_text LIKE '%Custom Scan (AGE Property Projection)%' THEN
            has_count_custom_scan := true;
        END IF;
        IF plan_text LIKE '%Seq Scan on cypher_match.v n%' AND
           plan_text LIKE '%width=0%' THEN
            has_count_width_zero_scan := true;
        END IF;
    END LOOP;

    IF has_count_custom_scan THEN
        RAISE EXCEPTION 'unexpected AGE Property Projection CustomScan for count wrapper';
    END IF;
    IF NOT has_count_width_zero_scan THEN
        RAISE EXCEPTION 'expected count wrapper to use width=0 plain scan';
    END IF;

    FOR plan_text IN EXECUTE
        'EXPLAIN (VERBOSE, COSTS OFF)
         SELECT count(i)
         FROM cypher(''cypher_match'',
                     $$MATCH (n:v) RETURN n.i$$) AS (i agtype)'
    LOOP
        IF plan_text LIKE '%agtype_object_field_exists_nonnull%' THEN
            has_count_arg_nonnull_probe := true;
        END IF;
        IF plan_text LIKE '%agtype_access_operator%' THEN
            has_count_arg_materialized_access := true;
        END IF;
    END LOOP;

    IF NOT has_count_arg_nonnull_probe THEN
        RAISE EXCEPTION 'expected count(i) wrapper to use non-null property probe';
    END IF;
    IF has_count_arg_materialized_access THEN
        RAISE EXCEPTION 'unexpected materialized property access in count(i) wrapper';
    END IF;

    FOR plan_text IN EXECUTE
        'EXPLAIN (VERBOSE, COSTS OFF)
         SELECT count(i)
         FROM cypher(''cypher_match'',
                     $$MATCH (n:v) RETURN n.payload.a$$) AS (i agtype)'
    LOOP
        IF plan_text LIKE '%agtype_object_field_exists_nonnull(agtype_object_field_agtype(n.properties, ''"payload"''::agtype), ''"a"''::agtype)%' THEN
            has_nested_count_arg_nonnull_probe := true;
        END IF;
        IF plan_text LIKE '%count(agtype_object_field_agtype(agtype_object_field_agtype%' OR
           plan_text LIKE '%count(agtype_access_operator%' THEN
            has_nested_count_arg_materialized_access := true;
        END IF;
    END LOOP;

    IF NOT has_nested_count_arg_nonnull_probe THEN
        RAISE EXCEPTION 'expected nested count(i) wrapper to use terminal non-null property probe';
    END IF;
    IF has_nested_count_arg_materialized_access THEN
        RAISE EXCEPTION 'unexpected materialized nested property access in count(i) wrapper';
    END IF;

    FOR plan_text IN EXECUTE
        'EXPLAIN (VERBOSE, COSTS OFF)
         SELECT *
         FROM cypher(''cypher_match'',
                     $$MATCH (n:v) RETURN count(n.payload.a)$$) AS (c agtype)'
    LOOP
        IF plan_text LIKE '%agtype_object_field_exists_nonnull(%agtype_access_operator(ARRAY[n.properties, ''"payload"''::agtype]), ''"a"''::agtype)%' OR
           plan_text LIKE '%agtype_object_field_exists_nonnull(%agtype_access_operator(VARIADIC ARRAY[n.properties, ''"payload"''::agtype]), ''"a"''::agtype)%' OR
           plan_text LIKE '%agtype_object_field_exists_nonnull(agtype_object_field_agtype(n.properties, ''"payload"''::agtype), ''"a"''::agtype)%' THEN
            has_nested_count_cypher_nonnull_probe := true;
        END IF;
        IF plan_text LIKE '%count(agtype_access_operator(VARIADIC ARRAY[n.properties, ''"payload"''::agtype, ''"a"''::agtype])%' OR
           plan_text LIKE '%count(agtype_object_field_agtype(agtype_object_field_agtype%' THEN
            has_nested_count_cypher_materialized_access := true;
        END IF;
    END LOOP;

    IF NOT has_nested_count_cypher_nonnull_probe THEN
        RAISE EXCEPTION 'expected nested Cypher count to use terminal non-null property probe';
    END IF;
    IF has_nested_count_cypher_materialized_access THEN
        RAISE EXCEPTION 'unexpected materialized nested property access in Cypher count';
    END IF;

    FOR plan_text IN EXECUTE
        'EXPLAIN (VERBOSE, COSTS OFF)
         SELECT *
         FROM cypher(''cypher_match'',
                     $$MATCH (n:v) RETURN n.i LIMIT 1$$) AS (i agtype)'
    LOOP
        IF plan_text LIKE '%Custom Scan (AGE Property Projection)%' THEN
            has_limit_custom_scan := true;
        END IF;
        IF plan_text LIKE '%Limit%' THEN
            has_limit_node := true;
        END IF;
    END LOOP;

    IF NOT has_limit_custom_scan THEN
        RAISE EXCEPTION 'expected LIMIT property projection to use AGE Property Projection CustomScan';
    END IF;
    IF NOT has_limit_node THEN
        RAISE EXCEPTION 'expected LIMIT property projection to keep Limit node';
    END IF;

    FOR plan_text IN EXECUTE
        'EXPLAIN (VERBOSE, COSTS OFF)
         SELECT *
         FROM cypher(''cypher_match'',
                     $$MATCH (n:v) RETURN n.i ORDER BY n.i LIMIT 1$$) AS (i agtype)'
    LOOP
        IF plan_text LIKE '%Sort Key:%agtype_object_field_agtype(n.properties, ''"i"''::agtype)%' THEN
            has_order_by_direct_access := true;
        END IF;
        IF plan_text LIKE '%agtype_access_operator(VARIADIC ARRAY[n.properties, ''"i"''::agtype])%' THEN
            has_order_by_materialized_access := true;
        END IF;
    END LOOP;

    IF NOT has_order_by_direct_access THEN
        RAISE EXCEPTION 'expected ORDER BY property projection to use direct object field target';
    END IF;
    IF has_order_by_materialized_access THEN
        RAISE EXCEPTION 'unexpected duplicate variadic property access in ORDER BY projection';
    END IF;

    FOR plan_text IN EXECUTE
        'EXPLAIN (VERBOSE, COSTS OFF)
         SELECT *
         FROM cypher(''cypher_match'',
                     $$MATCH (n:v) RETURN n.i::pg_bigint ORDER BY n.i::pg_bigint LIMIT 1$$) AS (i bigint)'
    LOOP
        IF plan_text LIKE '%agtype_object_field_int8(n.properties, ''"i"''::agtype)%' THEN
            has_pg_bigint_property_helper := true;
        END IF;
        IF plan_text LIKE '%agtype_to_int8%' OR
           plan_text LIKE '%agtype_access_operator%' THEN
            has_pg_bigint_materialized_cast := true;
        END IF;
    END LOOP;

    IF NOT has_pg_bigint_property_helper THEN
        RAISE EXCEPTION 'expected pg_bigint property projection to use typed field helper';
    END IF;
    IF has_pg_bigint_materialized_cast THEN
        RAISE EXCEPTION 'unexpected materialized property access in pg_bigint projection';
    END IF;

    FOR plan_text IN EXECUTE
        'EXPLAIN (VERBOSE, COSTS OFF)
         SELECT *
         FROM cypher(''cypher_match'',
                     $$MATCH (n:v) RETURN n.i::pg_float8 ORDER BY n.i::pg_float8 LIMIT 1$$) AS (i float8)'
    LOOP
        IF plan_text LIKE '%agtype_object_field_float8(n.properties, ''"i"''::agtype)%' THEN
            has_pg_float8_property_helper := true;
        END IF;
        IF plan_text LIKE '%agtype_to_float8%' OR
           plan_text LIKE '%agtype_access_operator%' THEN
            has_pg_float8_materialized_cast := true;
        END IF;
    END LOOP;

    IF NOT has_pg_float8_property_helper THEN
        RAISE EXCEPTION 'expected pg_float8 property projection to use typed field helper';
    END IF;
    IF has_pg_float8_materialized_cast THEN
        RAISE EXCEPTION 'unexpected materialized property access in pg_float8 projection';
    END IF;

    FOR plan_text IN EXECUTE
        'EXPLAIN (VERBOSE, COSTS OFF)
         SELECT *
         FROM cypher(''cypher_match'',
                     $$MATCH (n:v) RETURN n.i::numeric ORDER BY n.i::numeric LIMIT 1$$) AS (i agtype)'
    LOOP
        IF plan_text LIKE '%agtype_object_field_numeric_agtype(n.properties, ''"i"''::agtype)%' THEN
            has_numeric_property_helper := true;
        END IF;
        IF plan_text LIKE '%agtype_typecast_numeric%' OR
           plan_text LIKE '%agtype_access_operator%' THEN
            has_numeric_materialized_cast := true;
        END IF;
    END LOOP;

    IF NOT has_numeric_property_helper THEN
        RAISE EXCEPTION 'expected numeric property projection to use typed field helper';
    END IF;
    IF has_numeric_materialized_cast THEN
        RAISE EXCEPTION 'unexpected materialized property access in numeric projection';
    END IF;

    FOR plan_text IN EXECUTE
        'EXPLAIN (VERBOSE, COSTS OFF)
         SELECT *
         FROM cypher(''cypher_match'',
                     $$MATCH (n:v) RETURN n.i ORDER BY n.i::pg_bigint LIMIT 1$$) AS (i agtype)'
    LOOP
        IF plan_text LIKE '%Output: agtype_ctid_property_field_agtype(%n.ctid, 2, ''"i"''::agtype), (agtype_object_field_int8(n.properties, ''"i"''::agtype))%' THEN
            has_deferred_order_output := true;
        END IF;
        IF plan_text LIKE '%Output: n.ctid, (agtype_object_field_int8(n.properties, ''"i"''::agtype))%' THEN
            has_deferred_order_limit_input := true;
        END IF;
    END LOOP;

    IF NOT has_deferred_order_output THEN
        RAISE EXCEPTION 'expected ordered property projection to materialize output above LIMIT';
    END IF;
    IF NOT has_deferred_order_limit_input THEN
        RAISE EXCEPTION 'expected ordered property projection lower path to carry ctid and typed sort key';
    END IF;

    FOR plan_text IN EXECUTE
        'EXPLAIN (VERBOSE, COSTS OFF)
         SELECT *
         FROM cypher(''cypher_match'',
                     $$MATCH (n:v) RETURN n.i ORDER BY n.i::pg_float8 LIMIT 1$$) AS (i agtype)'
    LOOP
        IF plan_text LIKE '%Output: agtype_ctid_property_field_agtype(%n.ctid, 2, ''"i"''::agtype), (agtype_object_field_float8(n.properties, ''"i"''::agtype))%' THEN
            has_deferred_float_order_output := true;
        END IF;
        IF plan_text LIKE '%Output: n.ctid, (agtype_object_field_float8(n.properties, ''"i"''::agtype))%' THEN
            has_deferred_float_order_limit_input := true;
        END IF;
    END LOOP;

    IF NOT has_deferred_float_order_output THEN
        RAISE EXCEPTION 'expected float ordered property projection to materialize output above LIMIT';
    END IF;
    IF NOT has_deferred_float_order_limit_input THEN
        RAISE EXCEPTION 'expected float ordered property projection lower path to carry ctid and typed sort key';
    END IF;

    FOR plan_text IN EXECUTE
        'EXPLAIN (VERBOSE, COSTS OFF)
         SELECT *
         FROM cypher(''cypher_match'',
                     $$MATCH (n:v) RETURN n.i ORDER BY n.i::pg_text LIMIT 1$$) AS (i agtype)'
    LOOP
        IF plan_text LIKE '%Output: agtype_ctid_property_field_agtype(%n.ctid, 2, ''"i"''::agtype), (agtype_object_field_text_agtype(n.properties, ''"i"''::agtype))%' THEN
            has_deferred_text_order_output := true;
        END IF;
        IF plan_text LIKE '%Output: n.ctid, (agtype_object_field_text_agtype(n.properties, ''"i"''::agtype))%' THEN
            has_deferred_text_order_limit_input := true;
        END IF;
    END LOOP;

    IF NOT has_deferred_text_order_output THEN
        RAISE EXCEPTION 'expected text ordered property projection to materialize output above LIMIT';
    END IF;
    IF NOT has_deferred_text_order_limit_input THEN
        RAISE EXCEPTION 'expected text ordered property projection lower path to carry ctid and typed sort key';
    END IF;

    FOR plan_text IN EXECUTE
        'EXPLAIN (VERBOSE, COSTS OFF)
         SELECT *
         FROM cypher(''cypher_match'',
                     $$MATCH (n:v) RETURN n.i ORDER BY n.i::numeric LIMIT 1$$) AS (i agtype)'
    LOOP
        IF plan_text LIKE '%Output: agtype_ctid_property_field_agtype(%n.ctid, 2, ''"i"''::agtype), (agtype_object_field_numeric_agtype(n.properties, ''"i"''::agtype))%' THEN
            has_deferred_numeric_order_output := true;
        END IF;
        IF plan_text LIKE '%Output: n.ctid, (agtype_object_field_numeric_agtype(n.properties, ''"i"''::agtype))%' THEN
            has_deferred_numeric_order_limit_input := true;
        END IF;
    END LOOP;

    IF NOT has_deferred_numeric_order_output THEN
        RAISE EXCEPTION 'expected numeric ordered property projection to materialize output above LIMIT';
    END IF;
    IF NOT has_deferred_numeric_order_limit_input THEN
        RAISE EXCEPTION 'expected numeric ordered property projection lower path to carry ctid and typed sort key';
    END IF;

    FOR plan_text IN EXECUTE
        'EXPLAIN (VERBOSE, COSTS OFF)
         SELECT *
         FROM cypher(''cypher_match_plan'',
                     $$MATCH ()-[r:order_e]->() RETURN r.i ORDER BY r.i::pg_bigint LIMIT 1$$) AS (i agtype)'
    LOOP
        IF plan_text LIKE '%Output: agtype_id_property_field_agtype(%r.id, 4, ''"i"''::agtype), (agtype_object_field_int8(r.properties, ''"i"''::agtype))%' THEN
            has_deferred_edge_order_output := true;
        END IF;
        IF plan_text LIKE '%Output: r.id, (agtype_object_field_int8(r.properties, ''"i"''::agtype))%' THEN
            has_deferred_edge_order_limit_input := true;
        END IF;
    END LOOP;

    IF NOT has_deferred_edge_order_output THEN
        RAISE EXCEPTION 'expected edge ordered property projection to refetch output by graphid above LIMIT';
    END IF;
    IF NOT has_deferred_edge_order_limit_input THEN
        RAISE EXCEPTION 'expected edge ordered property projection lower path to carry id and typed sort key';
    END IF;

    FOR plan_text IN EXECUTE
        'EXPLAIN (VERBOSE, COSTS OFF)
         SELECT *
         FROM cypher(''cypher_match_plan'',
                     $$MATCH (a:join_v)-[:order_e]->(:join_v) RETURN a.i ORDER BY a.i::pg_bigint LIMIT 1$$) AS (i agtype)'
    LOOP
        IF plan_text LIKE '%Output: agtype_ctid_property_field_agtype(%a.ctid, 2, ''"i"''::agtype), (agtype_object_field_int8(a.properties, ''"i"''::agtype))%' THEN
            has_deferred_join_order_output := true;
        END IF;
        IF plan_text LIKE '%Output: a.ctid, (agtype_object_field_int8(a.properties, ''"i"''::agtype))%' THEN
            has_deferred_join_order_limit_input := true;
        END IF;
    END LOOP;

    IF NOT has_deferred_join_order_output THEN
        RAISE EXCEPTION 'expected join ordered property projection to refetch output above LIMIT';
    END IF;
    IF NOT has_deferred_join_order_limit_input THEN
        RAISE EXCEPTION 'expected join ordered property projection lower path to carry lookup key and typed sort key';
    END IF;

    FOR plan_text IN EXECUTE
        'EXPLAIN (VERBOSE, COSTS OFF)
         SELECT *
         FROM cypher(''cypher_match'',
                     $$MATCH (n:v) RETURN DISTINCT n.i ORDER BY n.i LIMIT 1$$) AS (i agtype)'
    LOOP
        IF plan_text LIKE '%agtype_object_field_agtype(n.properties, ''"i"''::agtype)%' THEN
            has_distinct_order_access := true;
        END IF;
        IF plan_text LIKE '%agtype_ctid_property_field_agtype%' THEN
            has_distinct_deferred_order_output := true;
        END IF;
    END LOOP;

    IF NOT has_distinct_order_access THEN
        RAISE EXCEPTION 'expected DISTINCT ordered property target to use direct object field access';
    END IF;
    IF has_distinct_deferred_order_output THEN
        RAISE EXCEPTION 'unexpected deferred ctid refetch across DISTINCT property target';
    END IF;

    FOR plan_text IN EXECUTE
        'EXPLAIN (VERBOSE, COSTS OFF)
         SELECT *
         FROM cypher(''cypher_match'',
                     $$MATCH (n:v) RETURN DISTINCT n.payload.a ORDER BY n.payload.a LIMIT 1$$) AS (i agtype)'
    LOOP
        IF plan_text LIKE '%agtype_object_field_agtype(agtype_object_field_agtype(n.properties, ''"payload"''::agtype), ''"a"''::agtype)%' THEN
            has_nested_distinct_order_access := true;
        END IF;
        IF plan_text LIKE '%agtype_access_operator(VARIADIC ARRAY[n.properties, ''"payload"''::agtype, ''"a"''::agtype])%' THEN
            has_nested_distinct_variadic_access := true;
        END IF;
    END LOOP;

    IF NOT has_nested_distinct_order_access THEN
        RAISE EXCEPTION 'expected nested DISTINCT property target to use chained object field access';
    END IF;
    IF has_nested_distinct_variadic_access THEN
        RAISE EXCEPTION 'unexpected variadic access in nested DISTINCT property target';
    END IF;

    FOR plan_text IN EXECUTE
        'EXPLAIN (VERBOSE, COSTS OFF)
         SELECT *
         FROM cypher(''cypher_match'',
                     $$MATCH (n:v) RETURN n.i, count(*) ORDER BY n.i LIMIT 1$$)
              AS (i agtype, c agtype)'
    LOOP
        IF plan_text LIKE '%Group Key: agtype_object_field_agtype(n.properties, ''"i"''::agtype)%' THEN
            has_group_direct_access := true;
        END IF;
        IF plan_text LIKE '%agtype_access_operator(VARIADIC ARRAY[n.properties, ''"i"''::agtype])%' THEN
            has_group_variadic_access := true;
        END IF;
    END LOOP;

    IF NOT has_group_direct_access THEN
        RAISE EXCEPTION 'expected grouped property target to use direct object field access';
    END IF;
    IF has_group_variadic_access THEN
        RAISE EXCEPTION 'unexpected variadic property access in grouped property target';
    END IF;

    FOR plan_text IN EXECUTE
        'EXPLAIN (VERBOSE, COSTS OFF)
         SELECT *
         FROM cypher(''cypher_match'',
                     $$MATCH (n:v) RETURN n.payload.a, count(*) ORDER BY n.payload.a LIMIT 1$$)
              AS (i agtype, c agtype)'
    LOOP
        IF plan_text LIKE '%Group Key: agtype_object_field_agtype(agtype_object_field_agtype(n.properties, ''"payload"''::agtype), ''"a"''::agtype)%' THEN
            has_nested_group_direct_access := true;
        END IF;
        IF plan_text LIKE '%agtype_access_operator(VARIADIC ARRAY[n.properties, ''"payload"''::agtype, ''"a"''::agtype])%' THEN
            has_nested_group_variadic_access := true;
        END IF;
    END LOOP;

    IF NOT has_nested_group_direct_access THEN
        RAISE EXCEPTION 'expected nested grouped property target to use chained object field access';
    END IF;
    IF has_nested_group_variadic_access THEN
        RAISE EXCEPTION 'unexpected variadic access in nested grouped property target';
    END IF;

    FOR plan_text IN EXECUTE
        'EXPLAIN (VERBOSE, COSTS OFF)
         SELECT *
         FROM cypher(''cypher_match'',
                     $$MATCH (n:v) RETURN n.i::pg_bigint, count(*) ORDER BY n.i::pg_bigint LIMIT 1$$)
              AS (i bigint, c agtype)'
    LOOP
        IF plan_text LIKE '%Output: (agtype_object_field_int8(n.properties, ''"i"''::agtype)), ((count(*)))::agtype%' THEN
            has_group_count_final_output := true;
        END IF;
        IF plan_text LIKE '%Output: (agtype_object_field_int8(n.properties, ''"i"''::agtype)), (count(*))%' THEN
            has_group_count_limit_input := true;
        END IF;
    END LOOP;

    IF NOT has_group_count_final_output THEN
        RAISE EXCEPTION 'expected grouped count agtype projection above LIMIT';
    END IF;
    IF NOT has_group_count_limit_input THEN
        RAISE EXCEPTION 'expected grouped count lower path to carry raw count';
    END IF;

    FOR plan_text IN EXECUTE
        'EXPLAIN (VERBOSE, COSTS OFF)
         SELECT *
         FROM cypher(''cypher_match'',
                     $$MATCH (n:v) RETURN n.i::pg_bigint, count(*) ORDER BY n.i::pg_bigint$$)
              AS (i bigint, c agtype)'
    LOOP
        IF plan_text LIKE '%Output: (agtype_object_field_int8(n.properties, ''"i"''::agtype)), ((count(*)))::agtype%' THEN
            has_group_count_plain_final_output := true;
        END IF;
        IF plan_text LIKE '%Output: (agtype_object_field_int8(n.properties, ''"i"''::agtype)), (count(*))%' THEN
            has_group_count_plain_sort_input := true;
        END IF;
    END LOOP;

    IF NOT has_group_count_plain_final_output THEN
        RAISE EXCEPTION 'expected non-limit grouped count final projection above aggregate';
    END IF;
    IF NOT has_group_count_plain_sort_input THEN
        RAISE EXCEPTION 'expected non-limit grouped count lower path to carry raw count';
    END IF;

    FOR plan_text IN EXECUTE
        'EXPLAIN (VERBOSE, COSTS OFF)
         SELECT *
         FROM cypher(''cypher_match'',
                     $$MATCH (n:v) RETURN n.i::pg_float8, count(*) ORDER BY n.i::pg_float8 LIMIT 1$$)
              AS (i float8, c agtype)'
    LOOP
        IF plan_text LIKE '%Output: (agtype_object_field_float8(n.properties, ''"i"''::agtype)), ((count(*)))::agtype%' THEN
            has_float_group_count_final_output := true;
        END IF;
        IF plan_text LIKE '%Output: (agtype_object_field_float8(n.properties, ''"i"''::agtype)), (count(*))%' THEN
            has_float_group_count_limit_input := true;
        END IF;
    END LOOP;

    IF NOT has_float_group_count_final_output THEN
        RAISE EXCEPTION 'expected float grouped count agtype projection above LIMIT';
    END IF;
    IF NOT has_float_group_count_limit_input THEN
        RAISE EXCEPTION 'expected float grouped count lower path to carry raw count';
    END IF;

    FOR plan_text IN EXECUTE
        'EXPLAIN (VERBOSE, COSTS OFF)
         SELECT *
         FROM cypher(''cypher_match'',
                     $$MATCH (n:v) RETURN n.i::pg_float8, count(*) ORDER BY n.i::pg_float8$$)
              AS (i float8, c agtype)'
    LOOP
        IF plan_text LIKE '%Output: (agtype_object_field_float8(n.properties, ''"i"''::agtype)), ((count(*)))::agtype%' THEN
            has_float_group_count_plain_final_output := true;
        END IF;
        IF plan_text LIKE '%Output: (agtype_object_field_float8(n.properties, ''"i"''::agtype)), (count(*))%' THEN
            has_float_group_count_plain_sort_input := true;
        END IF;
    END LOOP;

    IF NOT has_float_group_count_plain_final_output THEN
        RAISE EXCEPTION 'expected non-limit float grouped count final projection above aggregate';
    END IF;
    IF NOT has_float_group_count_plain_sort_input THEN
        RAISE EXCEPTION 'expected non-limit float grouped count lower path to carry raw count';
    END IF;

    FOR plan_text IN EXECUTE
        'EXPLAIN (VERBOSE, COSTS OFF)
         SELECT *
         FROM cypher(''cypher_match'',
                     $$MATCH (n:v) RETURN n.i::pg_text, count(*) ORDER BY n.i::pg_text LIMIT 1$$)
              AS (i text, c agtype)'
    LOOP
        IF plan_text LIKE '%Output: (agtype_object_field_text_agtype(n.properties, ''"i"''::agtype)), ((count(*)))::agtype%' THEN
            has_text_group_count_final_output := true;
        END IF;
        IF plan_text LIKE '%Output: (agtype_object_field_text_agtype(n.properties, ''"i"''::agtype)), (count(*))%' THEN
            has_text_group_count_limit_input := true;
        END IF;
    END LOOP;

    IF NOT has_text_group_count_final_output THEN
        RAISE EXCEPTION 'expected text grouped count agtype projection above LIMIT';
    END IF;
    IF NOT has_text_group_count_limit_input THEN
        RAISE EXCEPTION 'expected text grouped count lower path to carry raw count';
    END IF;

    FOR plan_text IN EXECUTE
        'EXPLAIN (VERBOSE, COSTS OFF)
         SELECT *
         FROM cypher(''cypher_match'',
                     $$MATCH (n:v) RETURN n.i::pg_text, count(*) ORDER BY n.i::pg_text$$)
              AS (i text, c agtype)'
    LOOP
        IF plan_text LIKE '%Output: (agtype_object_field_text_agtype(n.properties, ''"i"''::agtype)), ((count(*)))::agtype%' THEN
            has_text_group_count_plain_final_output := true;
        END IF;
        IF plan_text LIKE '%Output: (agtype_object_field_text_agtype(n.properties, ''"i"''::agtype)), (count(*))%' THEN
            has_text_group_count_plain_sort_input := true;
        END IF;
    END LOOP;

    IF NOT has_text_group_count_plain_final_output THEN
        RAISE EXCEPTION 'expected non-limit text grouped count final projection above aggregate';
    END IF;
    IF NOT has_text_group_count_plain_sort_input THEN
        RAISE EXCEPTION 'expected non-limit text grouped count lower path to carry raw count';
    END IF;

    FOR plan_text IN EXECUTE
        'EXPLAIN (VERBOSE, COSTS OFF)
         SELECT *
         FROM cypher(''cypher_match'',
                     $$MATCH (n:v) RETURN n.i::numeric, count(*) ORDER BY n.i::numeric LIMIT 1$$)
              AS (i agtype, c agtype)'
    LOOP
        IF plan_text LIKE '%Output: (agtype_object_field_numeric_agtype(n.properties, ''"i"''::agtype)), ((count(*)))::agtype%' THEN
            has_numeric_group_count_final_output := true;
        END IF;
        IF plan_text LIKE '%Output: (agtype_object_field_numeric_agtype(n.properties, ''"i"''::agtype)), (count(*))%' THEN
            has_numeric_group_count_limit_input := true;
        END IF;
    END LOOP;

    IF NOT has_numeric_group_count_final_output THEN
        RAISE EXCEPTION 'expected numeric grouped count agtype projection above LIMIT';
    END IF;
    IF NOT has_numeric_group_count_limit_input THEN
        RAISE EXCEPTION 'expected numeric grouped count lower path to carry raw count';
    END IF;

    FOR plan_text IN EXECUTE
        'EXPLAIN (VERBOSE, COSTS OFF)
         SELECT *
         FROM cypher(''cypher_match'',
                     $$MATCH (n:v) RETURN n.i::numeric, count(*) ORDER BY n.i::numeric$$)
              AS (i agtype, c agtype)'
    LOOP
        IF plan_text LIKE '%Output: (agtype_object_field_numeric_agtype(n.properties, ''"i"''::agtype)), ((count(*)))::agtype%' THEN
            has_numeric_group_count_plain_final_output := true;
        END IF;
        IF plan_text LIKE '%Output: (agtype_object_field_numeric_agtype(n.properties, ''"i"''::agtype)), (count(*))%' THEN
            has_numeric_group_count_plain_sort_input := true;
        END IF;
    END LOOP;

    IF NOT has_numeric_group_count_plain_final_output THEN
        RAISE EXCEPTION 'expected non-limit numeric grouped count final projection above aggregate';
    END IF;
    IF NOT has_numeric_group_count_plain_sort_input THEN
        RAISE EXCEPTION 'expected non-limit numeric grouped count lower path to carry raw count';
    END IF;
END
$property_projection_plan$;

DO $property_count_plan$
DECLARE
    plan_text text;
    has_property_probe boolean := false;
    has_materialized_access boolean := false;
BEGIN
    FOR plan_text IN EXECUTE
        'EXPLAIN (VERBOSE, COSTS OFF)
         SELECT *
         FROM cypher(''cypher_match'',
                     $$MATCH (n:v) RETURN count(n.i)$$) AS (i agtype)'
    LOOP
        IF plan_text LIKE '%agtype_object_field_exists_nonnull%' THEN
            has_property_probe := true;
        END IF;
        IF plan_text LIKE '%agtype_access_operator%' THEN
            has_materialized_access := true;
        END IF;
    END LOOP;

    IF NOT has_property_probe THEN
        RAISE EXCEPTION 'expected count property projection to use non-null property probe';
    END IF;
    IF has_materialized_access THEN
        RAISE EXCEPTION 'unexpected materialized property access in count property projection';
    END IF;
END
$property_count_plan$;

DO $typed_distinct_collect_plan$
DECLARE
    plan_text text;
    has_collect_distinct_float8 boolean := false;
    has_collect_distinct_int8 boolean := false;
    has_collect_distinct_text boolean := false;
    has_distinct_properties_carry boolean := false;
    has_materialized_access boolean := false;
BEGIN
    FOR plan_text IN EXECUTE
        'EXPLAIN (VERBOSE, COSTS OFF)
         SELECT *
         FROM cypher(''cypher_match'',
                     $$MATCH (n:v) RETURN collect(DISTINCT n.i::pg_float8)$$) AS (xs agtype)'
    LOOP
        IF plan_text LIKE '%age_collect_float8(DISTINCT%' THEN
            has_collect_distinct_float8 := true;
        END IF;
        IF plan_text LIKE '%age_collect(DISTINCT%' OR
           plan_text LIKE '%agtype_access_operator%' THEN
            has_materialized_access := true;
        END IF;
        IF plan_text LIKE '%Output: n.properties,%' THEN
            has_distinct_properties_carry := true;
        END IF;
    END LOOP;

    IF NOT has_collect_distinct_float8 THEN
        RAISE EXCEPTION 'expected typed distinct float8 collect to use typed transition aggregate';
    END IF;
    IF has_materialized_access THEN
        RAISE EXCEPTION 'unexpected materialized property access in typed distinct float8 collect';
    END IF;
    IF has_distinct_properties_carry THEN
        RAISE EXCEPTION 'unexpected properties carry in typed distinct float8 collect';
    END IF;

    has_materialized_access := false;
    has_distinct_properties_carry := false;
    FOR plan_text IN EXECUTE
        'EXPLAIN (VERBOSE, COSTS OFF)
         SELECT *
         FROM cypher(''cypher_match'',
                     $$MATCH (n:v) RETURN collect(DISTINCT n.i::pg_bigint)$$) AS (xs agtype)'
    LOOP
        IF plan_text LIKE '%age_collect_int8(DISTINCT%' THEN
            has_collect_distinct_int8 := true;
        END IF;
        IF plan_text LIKE '%age_collect(DISTINCT%' OR
           plan_text LIKE '%agtype_access_operator%' THEN
            has_materialized_access := true;
        END IF;
        IF plan_text LIKE '%Output: n.properties,%' THEN
            has_distinct_properties_carry := true;
        END IF;
    END LOOP;

    IF NOT has_collect_distinct_int8 THEN
        RAISE EXCEPTION 'expected typed distinct int8 collect to use typed transition aggregate';
    END IF;
    IF has_materialized_access THEN
        RAISE EXCEPTION 'unexpected materialized property access in typed distinct int8 collect';
    END IF;
    IF has_distinct_properties_carry THEN
        RAISE EXCEPTION 'unexpected properties carry in typed distinct int8 collect';
    END IF;

    has_materialized_access := false;
    has_distinct_properties_carry := false;
    FOR plan_text IN EXECUTE
        'EXPLAIN (VERBOSE, COSTS OFF)
         SELECT *
         FROM cypher(''cypher_match'',
                     $$MATCH (n:v) RETURN collect(DISTINCT n.i::pg_text)$$) AS (xs agtype)'
    LOOP
        IF plan_text LIKE '%age_collect_text(DISTINCT%' THEN
            has_collect_distinct_text := true;
        END IF;
        IF plan_text LIKE '%age_collect(DISTINCT%' OR
           plan_text LIKE '%agtype_access_operator%' THEN
            has_materialized_access := true;
        END IF;
        IF plan_text LIKE '%Output: n.properties,%' THEN
            has_distinct_properties_carry := true;
        END IF;
    END LOOP;

    IF NOT has_collect_distinct_text THEN
        RAISE EXCEPTION 'expected typed distinct text collect to use typed transition aggregate';
    END IF;
    IF has_materialized_access THEN
        RAISE EXCEPTION 'unexpected materialized property access in typed distinct text collect';
    END IF;
    IF has_distinct_properties_carry THEN
        RAISE EXCEPTION 'unexpected properties carry in typed distinct text collect';
    END IF;
END
$typed_distinct_collect_plan$;

DO $numeric_property_collect_plan$
DECLARE
    plan_text text;
    has_collect_numeric_property boolean := false;
    has_materialized_access boolean := false;
BEGIN
    FOR plan_text IN EXECUTE
        'EXPLAIN (VERBOSE, COSTS OFF)
         SELECT *
         FROM cypher(''cypher_match'',
                     $$MATCH (n:v) RETURN collect(n.i::numeric)$$) AS (xs agtype)'
    LOOP
        IF plan_text LIKE '%age_collect_numeric_property%' THEN
            has_collect_numeric_property := true;
        END IF;
        IF plan_text LIKE '%age_collect(%' OR
           plan_text LIKE '%agtype_object_field_numeric_agtype%' OR
           plan_text LIKE '%agtype_access_operator%' THEN
            has_materialized_access := true;
        END IF;
    END LOOP;

    IF NOT has_collect_numeric_property THEN
        RAISE EXCEPTION 'expected numeric property collect to use descriptor aggregate';
    END IF;
    IF has_materialized_access THEN
        RAISE EXCEPTION 'unexpected materialized property access in numeric property collect';
    END IF;
END
$numeric_property_collect_plan$;

SET enable_seqscan = on;
SET enable_indexscan = on;
SET enable_bitmapscan = on;

EXPLAIN (VERBOSE, COSTS OFF)
SELECT *
FROM cypher('cypher_match',
            $$MATCH (n:v) WHERE n.i = 1 RETURN n.i$$) AS (i agtype);

EXPLAIN (VERBOSE, COSTS OFF)
SELECT *
FROM cypher('cypher_match',
            $$MATCH (n:v) WHERE n.i > 0 RETURN n.i$$) AS (i agtype);

EXPLAIN (VERBOSE, COSTS OFF)
SELECT *
FROM cypher('cypher_match',
            $$MATCH (n:v) WHERE n.payload.a = 1 RETURN n.payload.a$$) AS (i agtype);

EXPLAIN (VERBOSE, COSTS OFF)
SELECT *
FROM cypher('cypher_match',
            $$MATCH (n:v) WHERE n.payload.a > 0 RETURN n.payload.a$$) AS (i agtype);

EXPLAIN (VERBOSE, COSTS OFF)
SELECT *
FROM cypher('cypher_match',
            $$MATCH (n:v) WHERE 0 < n.i RETURN n.i$$) AS (i agtype);

CREATE INDEX cypher_match_v_i_access_idx ON cypher_match.v
((ag_catalog.agtype_access_operator(VARIADIC
    ARRAY[properties, '"i"'::ag_catalog.agtype])));

SET enable_seqscan = off;

EXPLAIN (VERBOSE, COSTS OFF)
SELECT *
FROM cypher('cypher_match',
            $$MATCH (n:v) WHERE n.i = 1 RETURN n.i$$) AS (i agtype);

EXPLAIN (VERBOSE, COSTS OFF)
SELECT *
FROM cypher('cypher_match',
            $$MATCH (n:v) WHERE n.i > 0 RETURN n.i$$) AS (i agtype);

SET enable_seqscan = on;

DROP INDEX cypher_match.cypher_match_v_i_access_idx;

CREATE INDEX cypher_match_v_i_direct_idx ON cypher_match.v
((ag_catalog.agtype_object_field_agtype(
    properties, '"i"'::ag_catalog.agtype)));

SET enable_seqscan = off;

EXPLAIN (VERBOSE, COSTS OFF)
SELECT *
FROM cypher('cypher_match',
            $$MATCH (n:v) WHERE n.i = 1 RETURN n.i$$) AS (i agtype);

SET enable_seqscan = on;

DROP INDEX cypher_match.cypher_match_v_i_direct_idx;

CREATE INDEX cypher_match_v_i_partial_direct_idx ON cypher_match.v
((ag_catalog.agtype_object_field_agtype(
    properties, '"i"'::ag_catalog.agtype)))
WHERE ag_catalog.agtype_object_field_agtype(
          properties, '"i"'::ag_catalog.agtype) IS NOT NULL;

SET enable_seqscan = off;

EXPLAIN (VERBOSE, COSTS OFF)
SELECT *
FROM cypher('cypher_match',
            $$MATCH (n:v) WHERE n.i = 1 RETURN n.i$$) AS (i agtype);

SET enable_seqscan = on;

DROP INDEX cypher_match.cypher_match_v_i_partial_direct_idx;

CREATE INDEX cypher_match_v_payload_a_direct_idx ON cypher_match.v
((ag_catalog.agtype_object_field_agtype(
    ag_catalog.agtype_object_field_agtype(
        properties, '"payload"'::ag_catalog.agtype),
    '"a"'::ag_catalog.agtype)));

SET enable_seqscan = off;

EXPLAIN (VERBOSE, COSTS OFF)
SELECT *
FROM cypher('cypher_match',
            $$MATCH (n:v) WHERE n.payload.a = 1 RETURN n.payload.a$$) AS (i agtype);

EXPLAIN (VERBOSE, COSTS OFF)
SELECT age_array_agg_slots_descriptor(NULL::agtype)
       AS index_backed_slot_descriptor,
       array_length(array_agg(v), 1)
FROM cypher('cypher_match',
            $$MATCH (n:v) WHERE n.payload.a = 1 RETURN [n.payload.a]$$)
     AS (v agtype);

SET enable_seqscan = on;

DROP INDEX cypher_match.cypher_match_v_payload_a_direct_idx;

CREATE INDEX cypher_match_v_payload_a_access_idx ON cypher_match.v
((ag_catalog.agtype_access_operator(VARIADIC
    ARRAY[properties,
          '"payload"'::ag_catalog.agtype,
          '"a"'::ag_catalog.agtype])));

SET enable_seqscan = off;

EXPLAIN (VERBOSE, COSTS OFF)
SELECT *
FROM cypher('cypher_match',
            $$MATCH (n:v) WHERE n.payload.a = 1 RETURN n.payload.a$$) AS (i agtype);

SET enable_seqscan = on;

DROP INDEX cypher_match.cypher_match_v_payload_a_access_idx;

--Directed Paths
SELECT * FROM cypher('cypher_match', $$
	CREATE (:v1 {id:'initial'})-[:e1]->(:v1 {id:'middle'})-[:e1]->(:v1 {id:'end'})
$$) AS (a agtype);

CREATE FUNCTION pg_temp.normalized_verbose_explain(query text)
RETURNS TABLE(plan text)
LANGUAGE plpgsql
AS $$
DECLARE
    plan_text text;
BEGIN
    FOR plan_text IN EXECUTE 'EXPLAIN (VERBOSE, COSTS OFF) ' || query
    LOOP
        plan := regexp_replace(plan_text, '''[0-9]+''::oid',
                               '''<graph_oid>''::oid', 'g');
        RETURN NEXT;
    END LOOP;
END;
$$;

SELECT * FROM pg_temp.normalized_verbose_explain($plan$
SELECT *
FROM cypher('cypher_match',
            $$MATCH p=(:v1)-[:e1]->(:v1)-[:e1]->(:v1)
              RETURN count(p)$$) AS (c agtype)
$plan$);

SELECT * FROM pg_temp.normalized_verbose_explain($plan$
SELECT *
FROM cypher('cypher_match',
            $$MATCH p=(a:v1)-[r:e1]->(b:v1)
              RETURN relationships(p)[0].payload.a, count(*)
              ORDER BY relationships(p)[0].payload.a$$)
     AS (payload agtype, count agtype)
$plan$);

SELECT * FROM pg_temp.normalized_verbose_explain($plan$
SELECT *
FROM cypher('cypher_match',
            $$MATCH p=(a:v1)-[r:e1]->(b:v1)
              RETURN startNode(relationships(p)[0]).payload.a, count(*)
              ORDER BY startNode(relationships(p)[0]).payload.a$$)
     AS (payload agtype, count agtype)
$plan$);

SELECT * FROM pg_temp.normalized_verbose_explain($plan$
SELECT *
FROM cypher('cypher_match',
            $$MATCH p=(:v1)-[:e1]->(:v1)-[:e1]->(:v1)-[:e1]->(:v1)
              RETURN p$$) AS (p agtype)
$plan$);

SELECT * FROM pg_temp.normalized_verbose_explain($plan$
SELECT *
FROM cypher('cypher_match',
            $$MATCH p=(:v1)-[:e1]->(:v1)-[:e1]->(:v1)-[:e1]->(:v1)-[:e1]->(:v1)
              RETURN p$$) AS (p agtype)
$plan$);

SELECT * FROM pg_temp.normalized_verbose_explain($plan$
SELECT *
FROM cypher('cypher_match',
            $$MATCH p=(:v1)-[:e1]->(:v1)-[:e1]->(:v1)-[:e1]->(:v1)-[:e1]->(:v1)-[:e1]->(:v1)
              RETURN size(nodes(p)), size(relationships(p))$$)
     AS (nodes agtype, rels agtype)
$plan$);

SELECT * FROM pg_temp.normalized_verbose_explain($plan$
SELECT *
FROM cypher('cypher_match',
            $$MATCH p=(:v1)-[:e1]->(:v1)-[:e1]->(:v1)-[:e1]->(:v1)-[:e1]->(:v1)-[:e1]->(:v1)
              RETURN nodes(p), relationships(p)$$)
     AS (nodes agtype, rels agtype)
$plan$);

SELECT * FROM pg_temp.normalized_verbose_explain($plan$
SELECT *
FROM cypher('cypher_match',
            $$MATCH p=(:v1)-[:e1]->(:v1)-[:e1]->(:v1)-[:e1]->(:v1)-[:e1]->(:v1)-[:e1]->(:v1)
              RETURN p$$) AS (p agtype)
$plan$);

SELECT * FROM pg_temp.normalized_verbose_explain($plan$
SELECT *
FROM cypher('cypher_match',
            $$MATCH p=(:v1)-[:e1*1..2]->(:v1)
              RETURN relationships(p)$$) AS (r agtype)
$plan$);

SELECT * FROM pg_temp.normalized_verbose_explain($plan$
SELECT *
FROM cypher('cypher_match',
            $$MATCH p=(a:v1)-[r:e1*1..2]->(b:v1)
              RETURN last(relationships(p)).payload.a, count(*)
              ORDER BY last(relationships(p)).payload.a$$)
     AS (payload agtype, count agtype)
$plan$);

SELECT * FROM pg_temp.normalized_verbose_explain($plan$
SELECT *
FROM cypher('cypher_match',
            $$MATCH p=(a:v1)-[r:e1*1..2]->(b:v1)
              RETURN last(nodes(p)).payload.a, count(*)
              ORDER BY last(nodes(p)).payload.a$$)
     AS (payload agtype, count agtype)
$plan$);

SELECT * FROM pg_temp.normalized_verbose_explain($plan$
SELECT *
FROM cypher('cypher_match',
            $$MATCH p=(:v1)-[:e1*1..2]->(:v1)
              RETURN p$$) AS (p agtype)
$plan$);

--Undirected Path Tests
SELECT * FROM cypher('cypher_match', $$
	MATCH p=(:v1)-[:e1]-(:v1)-[:e1]-(:v1) RETURN p
$$) AS (a agtype);

SELECT * FROM cypher('cypher_match', $$
	MATCH p=(a:v1)-[]-()-[]-() RETURN a
$$) AS (a agtype);

SELECT * FROM cypher('cypher_match', $$
	MATCH ()-[]-()-[]-(a:v1) RETURN a
$$) AS (a agtype);

SELECT * FROM cypher('cypher_match', $$
	MATCH ()-[]-(a:v1)-[]-() RETURN a
$$) AS (a agtype);

SELECT * FROM cypher('cypher_match', $$
	MATCH ()-[b:e1]-()-[]-() RETURN b
$$) AS (a agtype);

SELECT * FROM cypher('cypher_match', $$
	MATCH (a:v1)-[]->(), ()-[]->(a) RETURN a
$$) AS (a agtype);

SELECT * FROM cypher('cypher_match', $$
    MATCH p=()-[e]-() RETURN e
$$) AS (a agtype);

SELECT * FROM cypher('cypher_match', $$
    MATCH (:v1)-[e]-() RETURN e
$$) AS (a agtype);

SELECT * FROM cypher('cypher_match', $$
    MATCH (:v1)-[e]-(:v1) RETURN e
$$) AS (a agtype);

SELECT * FROM cypher('cypher_match', $$
    MATCH ()-[]-()-[e]-(:v1) RETURN e
$$) AS (a agtype);

SELECT * FROM cypher('cypher_match', $$
    MATCH (a)-[]-()-[]-(:v1) RETURN a
$$) AS (a agtype);

-- Right Path Test
SELECT * FROM cypher('cypher_match', $$
	MATCH (a:v1)-[:e1]->(b:v1)-[:e1]->(c:v1) RETURN a, b, c
$$) AS (a agtype, b agtype, c agtype);

SELECT * FROM cypher('cypher_match', $$
	MATCH p=(a:v1)-[]-()-[]->() RETURN a
$$) AS (a agtype);

SELECT * FROM cypher('cypher_match', $$
	MATCH p=(a:v1)-[]->()-[]-() RETURN a
$$) AS (a agtype);

SELECT * FROM cypher('cypher_match', $$
	MATCH ()-[]-()-[]->(a:v1) RETURN a
$$) AS (a agtype);

SELECT * FROM cypher('cypher_match', $$
	MATCH ()-[]-(a:v1)-[]->() RETURN a
$$) AS (a agtype);

SELECT * FROM cypher('cypher_match', $$
	MATCH ()-[b:e1]-()-[]->() RETURN b
$$) AS (a agtype);

SELECT * FROM cypher('cypher_match', $$
    MATCH (:v1)-[e]->() RETURN e
$$) AS (a agtype);

SELECT * FROM cypher('cypher_match', $$
    MATCH ()-[e]->(:v1) RETURN e
$$) AS (a agtype);

SELECT * FROM cypher('cypher_match', $$
    MATCH (:v1)-[e]->(:v1) RETURN e
$$) AS (a agtype);

--Left Path Test
SELECT * FROM cypher('cypher_match', $$
	MATCH (a:v1)<-[:e1]-(b:v1)<-[:e1]-(c:v1) RETURN a, b, c
$$) AS (a agtype, b agtype, c agtype);

SELECT * FROM cypher('cypher_match', $$
	MATCH p=(a:v1)<-[]-()-[]-() RETURN a
$$) AS (a agtype);

SELECT * FROM cypher('cypher_match', $$
	MATCH p=(a:v1)-[]-()<-[]-() RETURN a
$$) AS (a agtype);

SELECT * FROM cypher('cypher_match', $$
	MATCH ()<-[]-()-[]-(a:v1) RETURN a
$$) AS (a agtype);

SELECT * FROM cypher('cypher_match', $$
	MATCH ()<-[]-(a:v1)-[]-() RETURN a
$$) AS (a agtype);

SELECT * FROM cypher('cypher_match', $$
	MATCH ()<-[b:e1]-()-[]-() RETURN b
$$) AS (a agtype);

SELECT * FROM cypher('cypher_match', $$
    MATCH (:v1)<-[e]-(:v1) RETURN e
$$) AS (a agtype);

--Divergent Path Tests
SELECT * FROM cypher('cypher_match', $$
	CREATE (:v2 {id:'initial'})<-[:e2]-(:v2 {id:'middle'})-[:e2]->(:v2 {id:'end'})
$$) AS (a agtype);

SELECT * FROM cypher('cypher_match', $$
	MATCH ()<-[]-(n:v2)-[]->()
	MATCH p=()-[]->(n)
	RETURN p
$$) AS (i agtype);

SELECT * FROM cypher('cypher_match', $$
	MATCH ()<-[]-(n:v2)-[]->()
	MATCH p=(n)-[]->()
	RETURN p
$$) AS (i agtype);

SELECT * FROM cypher('cypher_match', $$
	MATCH ()-[]-(n:v2)
	RETURN n
$$) AS (i agtype);

SELECT * FROM cypher('cypher_match', $$
    MATCH (:v2)<-[]-(:v2)-[]->(:v2)
    MATCH p=()-[]->()
    RETURN p
$$) AS (i agtype);

SELECT * FROM cypher('cypher_match', $$
    MATCH ()<-[]-(:v2)-[]->()
	MATCH p=()-[]->()
    RETURN p
$$) AS (i agtype);

--Convergent Path Tests
SELECT * FROM cypher('cypher_match', $$
	CREATE (:v3 {id:'initial'})-[:e3]->(:v3 {id:'middle'})<-[:e3]-(:v3 {id:'end'})
$$) AS (a agtype);

SELECT * FROM cypher('cypher_match', $$
	MATCH ()-[b:e1]->()
	RETURN b
$$) AS (i agtype);

SELECT * FROM cypher('cypher_match', $$
    MATCH (:v3)-[b:e3]->()
    RETURN b
$$) AS (i agtype);

SELECT * FROM cypher('cypher_match', $$
	MATCH ()-[]->(n:v1)<-[]-()
	MATCH p=(n)<-[]-()
	RETURN p
$$) AS (i agtype);

SELECT * FROM cypher('cypher_match', $$
	MATCH ()-[]->(n:v1)<-[]-()
	MATCH p=()-[]->(n)
	RETURN p
$$) AS (i agtype);

SELECT * FROM cypher('cypher_match', $$
	MATCH ()-[]->(n:v1)<-[]-()
	MATCH p=(n)-[]->()
	RETURN p
$$) AS (i agtype);

SELECT * FROM cypher('cypher_match', $$
	MATCH con_path=(a)-[]->()<-[]-()
	where a.id = 'initial'
	RETURN con_path
$$) AS (con_path agtype);

SELECT * FROM cypher('cypher_match', $$
	MATCH div_path=(b)<-[]-()-[]->()
	where b.id = 'initial'
	RETURN div_path
$$) AS (div_path agtype);

SELECT * FROM cypher('cypher_match', $$
	MATCH (a)-[]->(:v3)<-[]-(b)
	where a.id = 'initial'
	RETURN b       
$$) AS (con_path agtype);

--Patterns
SELECT * FROM cypher('cypher_match', $$
	MATCH (a:v1), p=(a)-[]-()-[]-()
	where a.id = 'initial'
	RETURN p
$$) AS (p agtype);

SELECT * FROM cypher('cypher_match', $$
	MATCH con_path=(a)-[]->()<-[]-(), div_path=(b)<-[]-()-[]->()
	where a.id = 'initial'
	and b.id = 'initial'
	RETURN con_path, div_path
$$) AS (con_path agtype, div_path agtype);

SELECT * FROM cypher('cypher_match', $$
	MATCH (a:v), p=()-[]->()-[]->()
	RETURN a.i, p
$$) AS (i agtype, p agtype);

--Multiple Match Clauses
SELECT * FROM cypher('cypher_match', $$
	MATCH (a:v1)
	where a.id = 'initial'
	MATCH p=(a)-[]-()-[]-()
	RETURN p
$$) AS (p agtype);

SELECT * FROM cypher('cypher_match', $$
	MATCH (a:v)
	MATCH p=()-[]->()-[]->()
	RETURN a.i, p
$$) AS (i agtype, p agtype);

SELECT * FROM cypher('cypher_match', $$
	MATCH (a:v)
	MATCH (b:v1)-[]-(c)
	RETURN a.i, b.id, c.id
$$) AS (i agtype, b agtype, c agtype);

SELECT * FROM cypher('cypher_match', $$
	MATCH (a:v)
	MATCH (:v1)-[]-(c)
	RETURN a.i, c.id
$$) AS (i agtype,  c agtype);

--
-- Property constraints
--
SELECT * FROM cypher('cypher_match',
 $$CREATE ({string_key: "test", int_key: 1, float_key: 3.14, map_key: {key: "value"}, list_key: [1, 2, 3]}) $$)
AS (p agtype);

SELECT * FROM cypher('cypher_match',
 $$CREATE ({lst: [1, NULL, 3.14, "string", {key: "value"}, []]}) $$)
AS (p agtype);

SELECT * FROM cypher('cypher_match',
 $$MATCH (n  {string_key: NULL}) RETURN n $$)
AS (n agtype);

SELECT * FROM cypher('cypher_match',
 $$MATCH (n  {string_key: "wrong value"}) RETURN n $$)
AS (n agtype);


SELECT * FROM cypher('cypher_match', $$
    MATCH (n {string_key: "test", int_key: 1, float_key: 3.14, map_key: {key: "value"}, list_key: [1, 2, 3]})
    RETURN n $$)
AS (p agtype);

SELECT * FROM cypher('cypher_match',
 $$MATCH (n {string_key: "test"}) RETURN n $$)
AS (p agtype);

SELECT * FROM cypher('cypher_match',
 $$MATCH (n {lst: [1, NULL, 3.14, "string", {key: "value"}, []]})  RETURN n $$)
AS (p agtype);

SELECT * FROM cypher('cypher_match',
 $$MATCH (n {lst: [1, NULL, 3.14, "string", {key: "value"}, [], "extra value"]})  RETURN n $$)
AS (p agtype);


--
-- Prepared Statement Property Constraint
--
PREPARE property_ps(agtype) AS SELECT * FROM cypher('cypher_match',
 $$MATCH (n $props) RETURN n $$, $1)
AS (p agtype);

EXECUTE property_ps(agtype_build_map('props',
                                     agtype_build_map('string_key', 'test')));

-- need a following RETURN clause (should fail)
SELECT * FROM cypher('cypher_match', $$MATCH (n:v)$$) AS (a agtype);

--invalid variable reuse, these should fail
SELECT * FROM cypher('cypher_match', $$
	MATCH (a)-[]-()-[]-(a:v1) RETURN a
$$) AS (a agtype);
SELECT * FROM cypher('cypher_match', $$
	MATCH (a)-[]-(a:v2)-[]-(a) RETURN a
$$) AS (a agtype);
SELECT * FROM cypher('cypher_match', $$
	MATCH (a)-[]-(a:v1) RETURN a
$$) AS (a agtype);
SELECT * FROM cypher('cypher_match', $$
	MATCH (a)-[]-(a)-[]-(a:v1) RETURN a
$$) AS (a agtype);
SELECT * FROM cypher('cypher_match', $$
	MATCH (a)-[]-(a)-[]-(a:invalid_label) RETURN a
$$) AS (a agtype);
SELECT * FROM cypher('cypher_match', $$
	MATCH (a) MATCH (a:v1) RETURN a
$$) AS (a agtype);
SELECT * FROM cypher('cypher_match', $$
	MATCH (a) MATCH (a:invalid_label) RETURN a
$$) AS (a agtype);
SELECT * FROM cypher('cypher_match', $$
	MATCH (a:v1)-[]-()-[a]-() RETURN a
$$) AS (a agtype);

-- valid variable reuse for edge labels across clauses
SELECT * FROM cypher('cypher_match', $$
        MATCH ()-[r0]->() MATCH ()-[r0]->() RETURN r0
$$) AS (r0 agtype);
SELECT * FROM cypher('cypher_match', $$
        MATCH ()-[r0:e1]->() MATCH ()-[r0:e1]->() RETURN r0
$$) AS (r0 agtype);
SELECT * FROM cypher('cypher_match', $$
        MATCH ()-[r0:e2]->() MATCH ()-[r0:e2]->() RETURN r0
$$) AS (r0 agtype);
SELECT * FROM cypher('cypher_match', $$
        MATCH ()-[r0:e1]->()-[r1]->() RETURN r0,r1
$$) AS (r0 agtype, r1 agtype);
SELECT * FROM cypher('cypher_match', $$
        MATCH p0=()-[:e1]->() MATCH p1=()-[:e2]->() RETURN p1
$$) AS (p1 agtype);
SELECT * FROM cypher('cypher_match', $$
        MATCH ()-[r0:e1]->()-[r1]->() MATCH ()-[r0:e1]->()-[r1]->() RETURN r0,r1
$$) AS (r0 agtype, r1 agtype);
SELECT * FROM cypher('cypher_match', $$
        MATCH ()-[]->() MATCH ()-[r1:e2]->() RETURN r1
$$) AS (r1 agtype);
SELECT * FROM cypher('cypher_match', $$
        MATCH ()-[r0:e1]->() MATCH ()-[r1:e2]->() RETURN r0,r1
$$) AS (r0 agtype, r1 agtype);

-- valid variable reuse for vertex labels across clauses
SELECT * FROM cypher('cypher_match', $$
         MATCH (r1:invalid), (r1:invalid) return r1
$$) AS (r1 agtype);
SELECT * FROM cypher('cypher_match', $$
         MATCH (r1), (r1) return r1
$$) AS (r1 agtype);
SELECT * FROM cypher('cypher_match', $$
         MATCH (r1:invalid), (r1) return r1
$$) AS (r1 agtype);
SELECT * FROM cypher('cypher_match', $$
         MATCH (r1:invalid), (r1), (r1), (r1:invalid) return r1
$$) AS (r1 agtype);
SELECT * FROM cypher('cypher_match', $$
         MATCH (r1:invalid)-[]->(r1)-[]->(r1:invalid)-[]->(r1) return r1
$$) AS (r1 agtype);
SELECT * FROM cypher('cypher_match', $$
         MATCH (r1:invalid)-[]->()-[]->()-[]->(r1:invalid) return r1
$$) AS (r1 agtype);
SELECT * FROM cypher('cypher_match', $$
        MATCH ()-[r0:e1]->()-[r1]->() MATCH ()-[r0:e1]->()-[r0]->() RETURN r0
$$) AS (r0 agtype);
SELECT * FROM cypher('cypher_match', $$
        MATCH ()-[r0:e1]->() MATCH ()-[r0]->() RETURN r0
$$) AS (r0 agtype);

-- invalid variable reuse for vertex
SELECT * FROM cypher('cypher_match', $$
         MATCH (r1:invalid)-[]->(r1)-[]->(r1)-[]->(r1:invalids) return r1
$$) AS (r1 agtype);
SELECT * FROM cypher('cypher_match', $$
         MATCH (r1:invalid)-[]->(r1)-[]->(r1)-[]->(r1)-[r1]->() return r1
$$) AS (r1 agtype);

-- invalid variable reuse for labels across clauses
SELECT * FROM cypher('cypher_match', $$
         MATCH (r1:e1), (r1:e2) return r1
$$) AS (r1 agtype);
SELECT * FROM cypher('cypher_match', $$
         MATCH (r1:invalid), (r1:e2) return r1
$$) AS (r1 agtype);
SELECT * FROM cypher('cypher_match', $$
         MATCH (r1:e1), (r1:invalid) return r1
$$) AS (r1 agtype);
SELECT * FROM cypher('cypher_match', $$
         MATCH (r1:e1), (r1), (r1:invalid) return r1
$$) AS (r1 agtype);
SELECT * FROM cypher('cypher_match', $$
        MATCH (r0)-[r0]->() MATCH ()-[]->() RETURN r0
$$) AS (r0 agtype);
SELECT * FROM cypher('cypher_match', $$
        MATCH (r0)-[]->() MATCH ()-[r0]->() RETURN r0
$$) AS (r0 agtype);
SELECT * FROM cypher('cypher_match', $$
        MATCH ()-[r0]->() MATCH ()-[]->(r0) RETURN r0
$$) AS (r0 agtype);
SELECT * FROM cypher('cypher_match', $$
        MATCH ()-[r0:e1]->() MATCH ()-[r0:e2]->() RETURN r0
$$) AS (r0 agtype);
SELECT * FROM cypher('cypher_match', $$
        MATCH ()-[r0]->() MATCH ()-[r0:e2]->() RETURN r0
$$) AS (r0 agtype);
SELECT * FROM cypher('cypher_match', $$
        MATCH ()-[r0:e1]->() MATCH ()-[r0:e2]->() RETURN r0
$$) AS (r0 agtype);
SELECT * FROM cypher('cypher_match', $$
        MATCH ()-[r0:e1]->()-[r0]->() MATCH ()-[r0:e2]->() RETURN r0
$$) AS (r0 agtype);
SELECT * FROM cypher('cypher_match', $$
        MATCH ()-[r0:e1]->()-[r1]->() MATCH ()-[r1:e1]->()-[r0]->() RETURN r0
$$) AS (r0 agtype);

-- Labels that don't exist but do match
SELECT * FROM cypher('cypher_match', $$
        MATCH (r0)-[r1:related]->() MATCH ()-[r1:related]->() RETURN r0
$$) AS (r0 agtype);

-- Labels that don't exist and don't match
SELECT * FROM cypher('cypher_match', $$
        MATCH (r0)-[r1]->() MATCH ()-[r1:related]->() RETURN r0
$$) AS (r0 agtype);
SELECT * FROM cypher('cypher_match', $$
        MATCH (r0)-[r1:related]->() MATCH ()-[r1:relateds]->() RETURN r0
$$) AS (r0 agtype);

--Valid variable reuse, although why would you want to do it this way?
SELECT * FROM cypher('cypher_match', $$
	MATCH (a:v1)-[]-()-[]-(a {id:'will_not_fail'}) RETURN a
$$) AS (a agtype);

--Incorrect Labels
SELECT * FROM cypher('cypher_match', $$MATCH (n)-[:v]-() RETURN n$$) AS (n agtype);

SELECT * FROM cypher('cypher_match', $$MATCH (n)-[:emissing]-() RETURN n$$) AS (n agtype);

SELECT * FROM cypher('cypher_match', $$MATCH (n:e1)-[]-() RETURN n$$) AS (n agtype);

SELECT * FROM cypher('cypher_match', $$MATCH (n:vmissing)-[]-() RETURN n$$) AS (n agtype);

SELECT * FROM cypher('cypher_match', $$MATCH (:e1)-[r]-() RETURN r$$) AS (r agtype);

SELECT * FROM cypher('cypher_match', $$MATCH (:vmissing)-[r]-() RETURN r$$) AS (r agtype);

SELECT * FROM cypher('cypher_match', $$MATCH (n),(:e1) RETURN n$$) AS (n agtype);

SELECT * FROM cypher('cypher_match', $$MATCH (n),()-[:v]-() RETURN n$$) AS (n agtype);

--
-- Path of one vertex. This should select 14
--
SELECT * FROM cypher('cypher_match', $$
       MATCH p=() RETURN p
$$) AS (p agtype);

--
-- MATCH with WHERE EXISTS(pattern)
--
SELECT * FROM cypher('cypher_match',
 $$MATCH (u)-[e]->(v) RETURN u, e, v $$) AS (u agtype, e agtype, v agtype);

SELECT * FROM cypher('cypher_match',
 $$MATCH (u)-[e]->(v) WHERE EXISTS((u)-[e]->(v)) RETURN u, e, v $$)
AS (u agtype, e agtype, v agtype);


-- Property Constraint in EXISTS
SELECT * FROM cypher('cypher_match',
 $$MATCH (u) WHERE EXISTS((u)-[]->({id: "middle"})) RETURN u $$)
AS (u agtype);

SELECT * FROM cypher('cypher_match',
 $$MATCH (u) WHERE EXISTS((u)-[]->({id: "not a valid id"})) RETURN u $$)
AS (u agtype);

SELECT * FROM cypher('cypher_match',
 $$MATCH (u) WHERE EXISTS((u)-[]->({id: NULL})) RETURN u $$)
AS (u agtype);

-- Exists checks for a loop. There shouldn't be any.
SELECT * FROM cypher('cypher_match',
 $$MATCH (u)-[e]->(v) WHERE EXISTS((u)-[e]->(u)) RETURN u, e, v $$)
AS (u agtype, e agtype, v agtype);

-- Create a loop
SELECT * FROM cypher('cypher_match', $$
        CREATE (u:loop {id:'initial'})-[:self]->(u)
$$) AS (a agtype);

-- dump paths
SELECT * FROM cypher('cypher_match',
 $$MATCH (u)-[e]->(v) WHERE EXISTS((u)-[e]->(v)) RETURN u, e, v $$)
AS (u agtype, e agtype, v agtype);

-- Exists checks for a loop. There should be one.
SELECT * FROM cypher('cypher_match',
 $$MATCH (u)-[e]->(v) WHERE EXISTS((u)-[e]->(u)) RETURN u, e, v $$)
AS (u agtype, e agtype, v agtype);

-- Exists checks for a loop. There should be one.
SELECT * FROM cypher('cypher_match',
 $$MATCH (u)-[e]->(v) WHERE EXISTS((v)-[e]->(v)) RETURN u, e, v $$)
AS (u agtype, e agtype, v agtype);

-- Multiple exists
SELECT * FROM cypher('cypher_match',
 $$MATCH (u)-[e]->(v) WHERE EXISTS((u)) AND EXISTS((v)) RETURN u, e, v $$)
AS (u agtype, e agtype, v agtype);

SELECT * FROM cypher('cypher_match',
 $$MATCH (u)-[e]->(v) WHERE EXISTS((u)-[e]->(u)) AND EXISTS((v)-[e]->(v)) RETURN u, e, v $$)
AS (u agtype, e agtype, v agtype);

-- Return exists(pattern)

SELECT * FROM cypher('cypher_match',
 $$MATCH (u) RETURN EXISTS((u)-[]->()) $$)
AS (exists agtype);

SELECT * FROM cypher('cypher_match',
 $$MATCH (u)-[e]->(v) RETURN EXISTS((u)-[e]->(v)-[e]->(u))$$)
AS (exists agtype);

-- These should error
-- Bad pattern
SELECT * FROM cypher('cypher_match',
 $$MATCH (u)-[e]->(v) WHERE EXISTS((u)) AND EXISTS([e]) AND EXISTS((v)) RETURN u, e, v $$)
AS (u agtype, e agtype, v agtype);

-- variable creation error
SELECT * FROM cypher('cypher_match',
 $$MATCH (u)-[e]->(v) WHERE EXISTS((u)-[e]->(x)) RETURN u, e, v $$)
AS (u agtype, e agtype, v agtype);

-- path variable not allowed in EXISTS
SELECT * FROM cypher('cypher_match',
 $$MATCH p=(u)-[e]->(v) RETURN EXISTS((p)) $$)
AS (exists agtype);

--
-- Tests for EXISTS(property)
--

-- dump all vertices
SELECT * FROM cypher('cypher_match', $$MATCH (u) RETURN u $$) AS (u agtype);

-- select vertices with id as a property
SELECT * FROM cypher('cypher_match',
 $$MATCH (u) WHERE EXISTS(u.id) RETURN u $$)
AS (u agtype);

-- select vertices without id as a property
SELECT * FROM cypher('cypher_match',
 $$MATCH (u) WHERE NOT EXISTS(u.id) RETURN u $$)
AS (u agtype);

-- select vertices without id as a property but with a property i
SELECT * FROM cypher('cypher_match',
 $$MATCH (u) WHERE NOT EXISTS(u.id) AND EXISTS(u.i) RETURN u $$)
AS (u agtype);

-- select vertices with id as a property and have a self loop
SELECT * FROM cypher('cypher_match',
 $$MATCH (u) WHERE EXISTS(u.id) AND EXISTS((u)-[]->(u)) RETURN u$$)
AS (u agtype);

-- Return exists(property)
SELECT * FROM cypher('cypher_match',
 $$MATCH (u) RETURN EXISTS(u.id), properties(u) $$)
AS (exists agtype, properties agtype);

SELECT * FROM cypher('cypher_match',
 $$MATCH (u) RETURN EXISTS(u.name), properties(u) $$)
AS (exists agtype, properties agtype);

-- should give an error
SELECT * FROM cypher('cypher_match',
 $$MATCH (u) WHERE EXISTS(u) RETURN u$$)
AS (u agtype);

--
-- MATCH with WHERE isEmpty(property)
--

SELECT create_graph('for_isEmpty');

-- Create vertices

SELECT * FROM cypher('for_isEmpty',
 $$CREATE (u:for_pred {id:1, type: "empty", list: [], map: {}, string: ""}),
		  (v:for_pred {id:2, type: "filled", list: [1], map: {a:1}, string: "a"}),
		  (w:for_pred)$$)
AS (a agtype);

-- Match vertices with empty properties

SELECT * FROM cypher('for_isEmpty',
 $$MATCH (u:for_pred) WHERE isEmpty(u.list) RETURN properties(u) $$)
AS (u agtype);

SELECT * FROM cypher('for_isEmpty',
 $$MATCH (u:for_pred) WHERE isEmpty(u.map) RETURN properties(u) $$)
AS (u agtype);

SELECT * FROM cypher('for_isEmpty',
 $$MATCH (u:for_pred) WHERE isEmpty(u.string) RETURN properties(u) $$)
AS (u agtype);

-- Match vertices with non-empty properties

SELECT * FROM cypher('for_isEmpty',
 $$MATCH (u:for_pred) WHERE NOT isEmpty(u.list) RETURN properties(u) $$)
AS (u agtype);

SELECT * FROM cypher('for_isEmpty',
 $$MATCH (u:for_pred) WHERE NOT isEmpty(u.map) RETURN properties(u) $$)
AS (u agtype);

SELECT * FROM cypher('for_isEmpty',
 $$MATCH (u:for_pred) WHERE NOT isEmpty(u.string) RETURN properties(u) $$)
AS (u agtype);

-- Match vertices with no properties

SELECT * FROM cypher('for_isEmpty',
 $$MATCH (u:for_pred) WHERE isEmpty(properties(u)) RETURN properties(u) $$)
AS (u agtype);

-- Match vertices with properties

SELECT * FROM cypher('for_isEmpty',
 $$MATCH (u:for_pred) WHERE NOT isEmpty(properties(u)) RETURN properties(u) $$)
AS (u agtype);

-- Match vertices with null property (should return nothing since WHERE null)

SELECT * FROM cypher('for_isEmpty',
 $$MATCH (u:for_pred) WHERE isEmpty(u.tree) RETURN properties(u) $$)
AS (u agtype);

-- Match and Return bool

SELECT * FROM cypher('for_isEmpty',
 $$MATCH (u:for_pred) WHERE isEmpty(u.list) RETURN isEmpty(u.list), u.type $$)
AS (b agtype, type agtype);

SELECT * FROM cypher('for_isEmpty',
 $$MATCH (u:for_pred) WHERE NOT isEmpty(u.list) RETURN isEmpty(u.list), u.type $$)
AS (b agtype, type agtype);

-- Return null on null

SELECT * FROM cypher('for_isEmpty',
 $$MATCH (u:for_pred) RETURN isEmpty(u.tree) $$)
AS (b agtype);

-- Should give an error

SELECT * FROM cypher('for_isEmpty',
 $$MATCH (u:for_pred) WHERE isEmpty(u) RETURN properties(u) $$)
AS (u agtype);

SELECT * FROM cypher('for_isEmpty',
 $$MATCH (u:for_pred) WHERE isEmpty(1) RETURN properties(u) $$)
AS (u agtype);

SELECT * FROM cypher('for_isEmpty',
 $$MATCH (u:for_pred) WHERE isEmpty(1,2,3) RETURN properties(u) $$)
AS (u agtype);

SELECT * FROM cypher('for_isEmpty',
 $$MATCH (u:for_pred) WHERE isEmpty() RETURN properties(u) $$)
AS (u agtype);

-- clean up
SELECT drop_graph('for_isEmpty', true);

--
--Distinct
--
SELECT * FROM cypher('cypher_match', $$
	MATCH (u)
	RETURN DISTINCT u.id
$$) AS (i agtype) ORDER BY i;

SELECT * FROM cypher('cypher_match', $$
	CREATE (u:duplicate)-[:dup_edge {id:1 }]->(:other_v)
$$) AS (a agtype);

SELECT * FROM cypher('cypher_match', $$
	MATCH (u:duplicate)
	CREATE (u)-[:dup_edge {id:2 }]->(:other_v)
$$) AS (a agtype);

SELECT * FROM cypher('cypher_match', $$
	MATCH (u:duplicate)-[]-(:other_v)
	RETURN DISTINCT u
$$) AS (i agtype);

SELECT * FROM cypher('cypher_match', $$
	MATCH p=(:duplicate)-[]-(:other_v)
	RETURN DISTINCT p
$$) AS (i agtype) ORDER BY i;

--
-- Limit
--
SELECT * FROM cypher('cypher_match', $$
	MATCH (u)
	RETURN u
$$) AS (i agtype);

SELECT * FROM cypher('cypher_match', $$
	MATCH (u)
	RETURN u LIMIT 3
$$) AS (i agtype);

--
-- Skip
--
SELECT * FROM cypher('cypher_match', $$
	MATCH (u)
	RETURN u SKIP 7
$$) AS (i agtype);

SELECT * FROM cypher('cypher_match', $$
	MATCH (u)
	RETURN u SKIP 7 LIMIT 3
$$) AS (i agtype);


--
-- Optional Match
--
SELECT * FROM cypher('cypher_match', $$
    CREATE (:opt_match_v {name: 'someone'})-[:opt_match_e]->(:opt_match_v {name: 'somebody'}),
           (:opt_match_v {name: 'anybody'})-[:opt_match_e]->(:opt_match_v {name: 'nobody'})
$$) AS (u agtype);

SELECT * FROM cypher('cypher_match', $$
    MATCH (u:opt_match_v)
    OPTIONAL MATCH (u)-[m]-(l)
    RETURN u.name as u, type(m), l.name as l
    ORDER BY u, m, l
$$) AS (u agtype, m agtype, l agtype);

SELECT * FROM cypher('cypher_match', $$
    OPTIONAL MATCH (n:opt_match_v)-[r]->(p), (m:opt_match_v)-[s]->(q)
    WHERE id(n) <> id(m)
    RETURN n.name as n, type(r) AS r, p.name as p,
           m.name AS m, type(s) AS s, q.name AS q
    ORDER BY n, p, m, q
$$) AS (n agtype, r agtype, p agtype, m agtype, s agtype, q agtype);

SELECT * FROM cypher('cypher_match', $$
    MATCH (n:opt_match_v), (m:opt_match_v)
    WHERE id(n) <> id(m)
    OPTIONAL MATCH (n)-[r]->(p), (m)-[s]->(q)
    RETURN n.name AS n, type(r) AS r, p.name AS p,
           m.name AS m, type(s) AS s, q.name AS q
    ORDER BY n, p, m, q
 $$) AS (n agtype, r agtype, p agtype, m agtype, s agtype, q agtype);

-- Tests to catch match following optional match logic
-- this syntax is invalid in cypher
SELECT * FROM cypher('cypher_match', $$
	OPTIONAL MATCH (n)
    MATCH (m)
    RETURN n,m
 $$) AS (n agtype, m agtype);

SELECT * FROM cypher('cypher_match', $$
	MATCH (n)
	OPTIONAL MATCH (m)
    MATCH (o)
    RETURN n,m
 $$) AS (n agtype, m agtype);

--
-- Tests retrieving Var from some parent's cpstate during transformation
--
SELECT create_graph('test_retrieve_var');
SELECT * FROM cypher('test_retrieve_var', $$ CREATE (:A)-[:incs]->(:C) $$) as (a agtype);

-- Tests with node Var
-- both queries should return the same result
-- first query does not retrieve any variable from any parent's cpstate
-- second query retrieves variable 'a', inside WHERE, from parent's parent's cpstate
SELECT * FROM cypher('test_retrieve_var', $$
    MATCH (a:A) WITH a
    OPTIONAL MATCH (a)-[:incs]->(c)
    WHERE EXISTS((c)<-[:incs]-())
    RETURN a, c
$$) AS (a agtype, c agtype);

SELECT * FROM cypher('test_retrieve_var', $$
    MATCH (a:A) WITH a
    OPTIONAL MATCH (a)-[:incs]->(c)
    WHERE EXISTS((c)<-[:incs]-(a))
    RETURN a, c
$$) AS (a agtype, c agtype);

-- Tests with edge Var
-- both queries should return the same result
-- first query does not retrieve any variable from any parent's cpstate
-- second query retrieves variable 'r', inside WHERE, from parent's parent's cpstate
SELECT * FROM cypher('test_retrieve_var', $$
    MATCH (a:A)-[r:incs]->() WITH a, r
    OPTIONAL MATCH (a)-[r]->(c)
    WHERE EXISTS(()<-[]-(c))
    RETURN a, r
$$) AS (a agtype, r agtype);

SELECT * FROM cypher('test_retrieve_var', $$
    MATCH (a:A)-[r:incs]->() WITH a, r
    OPTIONAL MATCH (a)-[r]->(c)
    WHERE EXISTS((:A)<-[]-(c))
    RETURN a, r
$$) AS (a agtype, r agtype);

SELECT * FROM cypher('test_retrieve_var', $$
    MATCH (a:A)-[r:incs]->() WITH a, r
    OPTIONAL MATCH (a)-[r]->(c)
    WHERE EXISTS((c)<-[]-(:A))
    RETURN a, r
$$) AS (a agtype, r agtype);

SELECT * FROM cypher('test_retrieve_var', $$
    MATCH (a:A)-[r:incs]->() WITH a, r
    OPTIONAL MATCH (a)-[r]->(c)
    WHERE EXISTS((:C)<-[]-(:A))
    RETURN a, r
$$) AS (a agtype, r agtype);

SELECT * FROM cypher('test_retrieve_var', $$
    MATCH (a:A)-[r:incs]->() WITH a, r
    OPTIONAL MATCH (a)-[r]->(c)
    WHERE EXISTS(()<-[r]-(c))
    RETURN a, r
$$) AS (a agtype, r agtype);

--
-- JIRA: AGE2-544
--

-- Clean up
SELECT DISTINCT * FROM cypher('cypher_match', $$
    MATCH (u) DETACH DELETE (u)
$$) AS (i agtype);

-- Prepare
SELECT * FROM cypher('cypher_match', $$
    CREATE (u {name: "orphan"})
    CREATE (u1 {name: "F"})-[u2:e1]->(u3 {name: "T"})
    RETURN u1, u2, u3
$$) as (u1 agtype, u2 agtype, u3 agtype);

-- Querying NOT EXISTS syntax
SELECT * FROM cypher('cypher_match', $$
     MATCH (f),(t)
     WHERE NOT EXISTS((f)-[]->(t))
     RETURN f.name, t.name
 $$) as (f agtype, t agtype);

-- Querying EXISTS syntax
SELECT * FROM cypher('cypher_match', $$
    MATCH (f),(t)
    WHERE EXISTS((f)-[]->(t))
    RETURN f.name, t.name
 $$) as (f agtype, t agtype);

-- Querying ALL
SELECT * FROM cypher('cypher_match', $$
    MATCH (f),(t)
    WHERE NOT EXISTS((f)-[]->(t)) or true
    RETURN f.name, t.name
$$) as (f agtype, t agtype);

-- Querying ALL
SELECT * FROM cypher('cypher_match', $$
    MATCH (f),(t)
    RETURN f.name, t.name
$$) as (f agtype, t agtype);

--
-- Constraints and WHERE clause together
--
SELECT * FROM cypher('cypher_match', $$
    CREATE ({i: 1, j: 2, k: 3}), ({i: 1, j: 3}), ({i:2, k: 3})
$$) as (a agtype);

SELECT * FROM cypher('cypher_match', $$
    MATCH (n {j: 2})
    WHERE n.i = 1
    RETURN n
$$) as (n agtype);

--
-- Regression tests to check previous clause variable refs
--
-- set up initial state and show what we're working with
SELECT * FROM cypher('cypher_match', $$
    CREATE (a {age: 4}) RETURN a $$) as (a agtype);
SELECT * FROM cypher('cypher_match', $$
	CREATE (b {age: 6}) RETURN b $$) as (b agtype);

SELECT * FROM cypher('cypher_match', $$
    MATCH (a) RETURN a $$) as (a agtype);
SELECT * FROM cypher('cypher_match', $$
    MATCH (a) WHERE EXISTS(a.name) RETURN a $$) as (a agtype);
SELECT * FROM cypher('cypher_match', $$
    MATCH (a) WHERE EXISTS(a.name) SET a.age = 4 RETURN a $$) as (a agtype);

SELECT * FROM cypher('cypher_match', $$
	MATCH (a),(b) WHERE a.age = 4 AND a.name = "T" AND b.age = 6
	RETURN a,b $$) as (a agtype, b agtype);
SELECT * FROM cypher('cypher_match', $$
	MATCH (a),(b) WHERE a.age = 4 AND a.name = "T" AND b.age = 6 CREATE
	(a)-[:knows {relationship: "friends", years: 3}]->(b) $$) as (r agtype);
SELECT * FROM cypher('cypher_match', $$
	MATCH (a),(b) WHERE a.age = 4 AND a.name = "orphan" AND b.age = 6 CREATE
	(a)-[:knows {relationship: "enemies", years: 4}]->(b) $$) as (r agtype);
SELECT * FROM cypher('cypher_match', $$
	MATCH (a)-[r]-(b) RETURN r ORDER BY r DESC $$) as (r agtype);

-- check reuse of 'a' clause-to-clause - vertices
SELECT * FROM cypher('cypher_match', $$
    MATCH (a {age:4}) RETURN a $$) as (a agtype);
SELECT * FROM cypher('cypher_match', $$
    MATCH (a) MATCH (a {age:4}) RETURN a $$) as (a agtype);

SELECT * FROM cypher('cypher_match', $$
    MATCH (a {age:4, name: "orphan"}) RETURN a $$) as (a agtype);
SELECT * FROM cypher('cypher_match', $$
    MATCH (a) MATCH (a {age:4}) MATCH (a {name: "orphan"}) RETURN a $$) as (a agtype);
SELECT * FROM cypher('cypher_match', $$
    MATCH (a {age:4}) MATCH (a {name: "orphan"}) RETURN a $$) as (a agtype);

SELECT * FROM cypher('cypher_match', $$
    MATCH (a) MATCH (a {age:4}) MATCH (a {name: "orphan"}) SET a.age = 3 RETURN a $$) as (a agtype);

SELECT * FROM cypher('cypher_match', $$
    MATCH (a) MATCH (a {age:3}) MATCH (a {name: "orphan"}) RETURN a $$) as (a agtype);
SELECT * FROM cypher('cypher_match', $$
    MATCH (a {name: "orphan"}) MATCH (a {age:3}) RETURN a $$) as (a agtype);

SELECT * FROM cypher('cypher_match', $$
    MATCH (a) WHERE EXISTS(a.age) AND EXISTS(a.name) RETURN a $$) as (a agtype);

SELECT * FROM cypher('cypher_match', $$
    MATCH (a) WHERE EXISTS(a.age) AND NOT EXISTS(a.name) RETURN a $$) as (a agtype);

-- check reuse of 'r' clause-to-clause - edges
SELECT * FROM cypher('cypher_match', $$
	MATCH ()-[r]-() RETURN r ORDER BY r DESC $$) as (r agtype);
SELECT * FROM cypher('cypher_match', $$
	MATCH ()-[r]-() MATCH ()-[r {relationship: "friends"}]-() RETURN r $$) as (r agtype);
SELECT * FROM cypher('cypher_match', $$
	MATCH ()-[r {years:3, relationship: "friends"}]-() RETURN r $$) as (r agtype);
SELECT * FROM cypher('cypher_match', $$
	MATCH ()-[r {years:3}]-() MATCH ()-[r {relationship: "friends"}]-() RETURN r $$) as (r agtype);
--mismatch year #, should return nothing
SELECT * FROM cypher('cypher_match', $$
	MATCH ()-[r {years:2}]-() MATCH ()-[r {relationship: "friends"}]-() RETURN r $$) as (r agtype);
SELECT * FROM cypher('cypher_match', $$
	MATCH ()-[r {relationship:"enemies"}]-() MATCH ()-[r {years:4}]-() RETURN r $$) as (r agtype);
SELECT * FROM cypher('cypher_match', $$
	MATCH ()-[r {relationship:"enemies"}]-() MATCH ()-[r {relationship:"friends"}]-() RETURN r $$) as (r agtype);

-- check reuse within clause - vertices
SELECT * FROM cypher('cypher_match', $$ CREATE (u {name: "Dave"})-[:knows]->({name: "John"})-[:knows]->(u) RETURN u $$) as (u agtype);
SELECT * FROM cypher('cypher_match', $$ MATCH p=(u)-[]-()-[]-(u) RETURN p $$)as (p agtype);
SELECT * FROM cypher('cypher_match', $$ MATCH p=(u)-[]->()-[]->(u) RETURN p $$)as (p agtype);
SELECT * FROM cypher('cypher_match', $$ MATCH p=(a)-[]->()-[]->(a {name: "Dave"}) RETURN p $$)as (p agtype);
SELECT * FROM cypher('cypher_match', $$ MATCH p=(a)-[]->()-[]->(a {name: "John"}) RETURN p $$)as (p agtype);
SELECT * FROM cypher('cypher_match', $$ MATCH p=(a {name: "Dave"})-[]->()-[]->(a {name: "Dave"}) RETURN p $$)as (p agtype);
SELECT * FROM cypher('cypher_match', $$ MATCH p=(a {name: "John"})-[]->()-[]->(a {name: "John"}) RETURN p $$)as (p agtype);
SELECT * FROM cypher('cypher_match', $$ MATCH p=(a {name: "Dave"})-[]->()-[]->(a) RETURN p $$)as (p agtype);
SELECT * FROM cypher('cypher_match', $$ MATCH p=(a {name: "John"})-[]->()-[]->(a) RETURN p $$)as (p agtype);

-- these are illegal and should fail
SELECT * FROM cypher('cypher_match', $$ MATCH p=(a)-[b]->()-[b:knows]->(a) RETURN p $$)as (p agtype);
SELECT * FROM cypher('cypher_match', $$ MATCH p=(a)-[b]->()-[b]->(a) RETURN p $$)as (p agtype);
SELECT * FROM cypher('cypher_match', $$ MATCH p=(a)-[b:knows]->()-[b:knows]->(a) RETURN p $$)as (p agtype);
SELECT * FROM cypher('cypher_match', $$ MATCH p=(a)-[b:knows]->()-[b]->(a) RETURN p $$)as (p agtype);
SELECT * FROM cypher('cypher_match', $$ MATCH p=(p) RETURN p $$)as (p agtype);
SELECT * FROM cypher('cypher_match', $$ MATCH p=(p)-[]->() RETURN p $$)as (p agtype);
SELECT * FROM cypher('cypher_match', $$ MATCH p=()-[p]->() RETURN p $$)as (p agtype);
SELECT * FROM cypher('cypher_match', $$ MATCH p=() MATCH (p) RETURN p $$)as (p agtype);
SELECT * FROM cypher('cypher_match', $$ MATCH p=() MATCH (p)-[]->() RETURN p $$)as (p agtype);
SELECT * FROM cypher('cypher_match', $$ MATCH p=() MATCH ()-[p]->() RETURN p $$)as (p agtype);
SELECT * FROM cypher('cypher_match', $$ MATCH (p) MATCH p=() RETURN p $$)as (p agtype);
SELECT * FROM cypher('cypher_match', $$ MATCH ()-[p]->() MATCH p=() RETURN p $$)as (p agtype);
SELECT * FROM cypher('cypher_match', $$ MATCH ()-[p *]-()-[p]-() RETURN 0 $$)as (p agtype);
SELECT * FROM cypher('cypher_match', $$ CREATE (p) WITH p MATCH p=() RETURN p $$)as (p agtype);
SELECT * FROM cypher('cypher_match', $$ CREATE p=() WITH p MATCH (p) RETURN p $$)as (p agtype);
SELECT * FROM cypher('cypher_match', $$ CREATE ()-[p:knows]->() WITH p MATCH p=() RETURN p $$)as (p agtype);
SELECT * FROM cypher('cypher_match', $$ CREATE p=() WITH p MATCH ()-[p]->() RETURN p $$)as (p agtype);



--
-- Default alias check (issue #883)
--
SELECT * FROM cypher('cypher_match', $$ MATCH (_) RETURN _ $$) as (a agtype);
SELECT * FROM cypher('cypher_match', $$ MATCH () MATCH (_{name: "Dave"}) RETURN 0 $$) as (a agtype);
SELECT * FROM cypher('cypher_match', $$ MATCH () MATCH (_{name: "Dave"}) RETURN _ $$) as (a agtype);
SELECT * FROM cypher('cypher_match', $$ MATCH (my_age_default_{name: "Dave"}) RETURN my_age_default_$$) as (a agtype);
SELECT * FROM cypher('cypher_match', $$ MATCH () MATCH (my_age_default_{name: "Dave"}) RETURN my_age_default_$$) as (a agtype);

-- these should fail as they are prefixed with _age_default_ which is only for internal use
SELECT * FROM cypher('cypher_match', $$ MATCH (_age_default_) RETURN _age_default_ $$) as (a agtype);
SELECT * FROM cypher('cypher_match', $$ MATCH (_age_default_a) RETURN _age_default_a $$) as (a agtype);
SELECT * FROM cypher('cypher_match', $$ MATCH (_age_default_whatever) RETURN 0 $$) as (a agtype);

-- issue 876
SELECT * FROM cypher('cypher_match', $$ MATCH ({name: "Dave"}) MATCH ({name: "Dave"}) MATCH ({name: "Dave"}) RETURN 0 $$) as (a agtype);
SELECT * FROM cypher('cypher_match', $$MATCH ({n0:0}) MATCH ()-[]->() MATCH ({n1:0})-[]-() RETURN 0 AS n2$$) as (a agtype);

--
-- self referencing property constraints (issue #898)
--
SELECT * FROM cypher('cypher_match', $$ MATCH (a {name:a.name}) RETURN a $$) as (a agtype);
SELECT * FROM cypher('cypher_match', $$ MATCH (a {name:a.name, age:a.age}) RETURN a $$) as (a agtype);
SELECT * FROM cypher('cypher_match', $$ MATCH (a {name:a.name}) MATCH (a {age:a.age}) RETURN a $$) as (a agtype);

SELECT * FROM cypher('cypher_match', $$ MATCH p=(a)-[u {relationship: u.relationship}]->(b) RETURN p $$) as (a agtype);
SELECT * FROM cypher('cypher_match', $$ MATCH p=(a)-[u {relationship: u.relationship, years: u.years}]->(b) RETURN p $$) as (a agtype);
SELECT * FROM cypher('cypher_match', $$ MATCH p=(a {name:a.name})-[u {relationship: u.relationship}]->(b {age:b.age}) RETURN p $$) as (a agtype);

SELECT * FROM cypher('cypher_match', $$ CREATE () WITH * MATCH (x{n0:x.n1}) RETURN 0 $$) as (a agtype);

-- these should fail due to multiple labels for a variable
SELECT * FROM cypher('cypher_match', $$ MATCH p=(x)-[]->(x:R) RETURN p, x $$) AS (p agtype, x agtype);
SELECT * FROM cypher('cypher_match', $$ MATCH p=(x:r)-[]->(x:R) RETURN p, x $$) AS (p agtype, x agtype);

--
-- Test per-key property constraint decomposition (MATCH {key:val} path)
--
-- Verify that map property constraints are decomposed into individual
-- per-key quals using the access operator rather than whole-document
-- containment.
--

SELECT create_graph('test_enable_containment');
SELECT * FROM cypher('test_enable_containment',
$$
    CREATE (x:Customer {
        name: 'Bob',
        school: {
            name: 'XYZ College',
            program: {
                major: 'Psyc',
                degree: 'BSc'
            }
        },
        phone: [ 123456789, 987654321, 456987123 ],
        addr: [
            {city: 'Vancouver', street: 30},
            {city: 'Toronto', street: 40}
        ]
    })
    RETURN x
$$) as (a agtype);


SELECT count(*) FROM cypher('test_enable_containment', $$ MATCH (x:Customer {addr:[{city:'Toronto'}]}) RETURN x $$) as (a agtype);
SELECT count(*) FROM cypher('test_enable_containment', $$ MATCH (x:Customer {addr:[{city:'Toronto'}, {city: 'Vancouver'}]}) RETURN x $$) as (a agtype);
SELECT count(*) FROM cypher('test_enable_containment', $$ MATCH (x:Customer {addr:[{city:'Alberta'}]}) RETURN x $$) as (a agtype);
SELECT count(*) FROM cypher('test_enable_containment', $$ MATCH (x:Customer {school:{program:{major:'Psyc'}}}) RETURN x $$) as (a agtype);
SELECT count(*) FROM cypher('test_enable_containment', $$ MATCH (x:Customer {name:'Bob',school:{program:{degree:'BSc'}}}) RETURN x $$) as (a agtype);
SELECT count(*) FROM cypher('test_enable_containment', $$ MATCH (x:Customer {school:{program:{major:'Cs'}}}) RETURN x $$) as (a agtype);
SELECT count(*) FROM cypher('test_enable_containment', $$ MATCH (x:Customer {name:'Bob',school:{program:{degree:'PHd'}}}) RETURN x $$) as (a agtype);
SELECT count(*) FROM cypher('test_enable_containment', $$ MATCH (x:Customer {phone:[987654321]}) RETURN x $$) as (a agtype);
SELECT count(*) FROM cypher('test_enable_containment', $$ MATCH (x:Customer {phone:[654765876]}) RETURN x $$) as (a agtype);
\pset format unaligned
SELECT * FROM cypher('test_enable_containment', $$ EXPLAIN (COSTS OFF) MATCH (x:Customer {school:{name:'XYZ',program:{degree:'BSc'}},phone:[987654321],parents:{}}) RETURN x $$) as (a agtype);
\pset format aligned

--
-- Issue 945
--
SELECT create_graph('issue_945');
SELECT * FROM cypher('issue_945', $$
    CREATE (a:Part {part_num: '123'}),
           (b:Part {part_num: '345'}),
           (c:Part {part_num: '456'}),
           (d:Part {part_num: '789'})
    $$) as (result agtype);

-- should match 4
SELECT * FROM cypher('issue_945', $$
    MATCH (a:Part) RETURN a
    $$) as (result agtype);

-- each should return 4
SELECT * FROM cypher('issue_945', $$ MATCH (:Part) RETURN count(*) $$) as (result agtype);
SELECT * FROM cypher('issue_945', $$ MATCH (a:Part) RETURN count(*) $$) as (result agtype);

-- each should return 4 rows of 0
SELECT * FROM cypher('issue_945', $$ MATCH (:Part) RETURN 0 $$) as (result agtype);
SELECT * FROM cypher('issue_945', $$ MATCH (a:Part) RETURN 0 $$) as (result agtype);

-- each should return 16 rows of 0
SELECT * FROM cypher('issue_945', $$ MATCH (:Part) MATCH (:Part) RETURN 0 $$) as (result agtype);
SELECT * FROM cypher('issue_945', $$ MATCH (a:Part) MATCH (:Part) RETURN 0 $$) as (result agtype);
SELECT * FROM cypher('issue_945', $$ MATCH (:Part) MATCH (b:Part) RETURN 0 $$) as (result agtype);
SELECT * FROM cypher('issue_945', $$ MATCH (a:Part) MATCH (b:Part) RETURN 0 $$) as (result agtype);

-- each should return a count of 16
SELECT * FROM cypher('issue_945', $$ MATCH (:Part) MATCH (:Part) RETURN count(*) $$) as (result agtype);
SELECT * FROM cypher('issue_945', $$ MATCH (a:Part) MATCH (:Part) RETURN count(*) $$) as (result agtype);
SELECT * FROM cypher('issue_945', $$ MATCH (:Part) MATCH (b:Part) RETURN count(*) $$) as (result agtype);
SELECT * FROM cypher('issue_945', $$ MATCH (a:Part) MATCH (b:Part) RETURN count(*) $$) as (result agtype);


--
-- Issue 1045
--
SELECT * FROM cypher('cypher_match', $$ MATCH p=()-[*]->() RETURN length(p) $$) as (length agtype);
SELECT * FROM cypher('cypher_match', $$ MATCH p=()-[*]->() WHERE length(p) > 1 RETURN length(p) $$) as (length agtype);
SELECT * FROM cypher('cypher_match', $$ MATCH p=()-[*]->() WHERE size(nodes(p)) = 3 RETURN nodes(p)[0] $$) as (nodes agtype);
SELECT * FROM cypher('cypher_match', $$ MATCH (n {name:'Dave'}) MATCH p=()-[*]->() WHERE nodes(p)[0] = n RETURN length(p) ORDER BY length(p) $$) as (length agtype);
SELECT * FROM cypher('cypher_match', $$ MATCH p1=(n {name:'Dave'})-[]->() MATCH p2=()-[*]->() WHERE p2=p1 RETURN p2=p1 $$) as (path agtype);

--
-- Issue 1399 EXISTS leads to an error if a relation label does not exists as database table
--
SELECT create_graph('issue_1399');
-- this is an empty graph so these should return 0
SELECT * FROM cypher('issue_1399', $$
  MATCH (foo)
  WHERE EXISTS((foo)-[]->())
  RETURN foo
$$) as (c agtype);
SELECT * FROM cypher('issue_1399', $$
  MATCH (foo)
  WHERE NOT EXISTS((foo)-[]->())
  RETURN foo
$$) as (c agtype);
SELECT * FROM cypher('issue_1399', $$
  MATCH (foo)
  WHERE EXISTS((foo)-[:BAR]->())
  RETURN foo
$$) as (c agtype);
SELECT * FROM cypher('issue_1399', $$
  MATCH (foo)
  WHERE NOT EXISTS((foo)-[:BAR]->())
  RETURN foo
$$) as (c agtype);
-- this is an empty graph so these should return false
SELECT * FROM cypher('issue_1399', $$
  MATCH (foo)
  WHERE EXISTS((foo)-[]->())
  RETURN count(foo) > 0
$$) as (c agtype);
SELECT * FROM cypher('issue_1399', $$
  MATCH (foo)
  WHERE NOT EXISTS((foo)-[]->())
  RETURN count(foo) > 0
$$) as (c agtype);
SELECT * FROM cypher('issue_1399', $$
  MATCH (foo)
  WHERE EXISTS((foo)-[:BAR]->())
  RETURN count(foo) > 0
$$) as (c agtype);
SELECT * FROM cypher('issue_1399', $$
  MATCH (foo)
  WHERE NOT EXISTS((foo)-[:BAR]->())
  RETURN count(foo) > 0
$$) as (c agtype);
-- create 1 path
SELECT * FROM cypher('issue_1399', $$
  CREATE (foo)-[:BAR]->() RETURN foo
$$) as (c agtype);
-- these should each return 1 row as it is a directed edge and
-- only one vertex can match.
SELECT * FROM cypher('issue_1399', $$
  MATCH (foo)
  WHERE EXISTS((foo)-[]->())
  RETURN foo
$$) as (c agtype);
SELECT * FROM cypher('issue_1399', $$
  MATCH (foo)
  WHERE NOT EXISTS((foo)-[]->())
  RETURN foo
$$) as (c agtype);
SELECT * FROM cypher('issue_1399', $$
  MATCH (foo)
  WHERE EXISTS((foo)-[:BAR]->())
  RETURN foo
$$) as (c agtype);
SELECT * FROM cypher('issue_1399', $$
  MATCH (foo)
  WHERE NOT EXISTS((foo)-[:BAR]->())
  RETURN foo
$$) as (c agtype);
-- this should return 0 rows as it can't exist - that path isn't in BAR2
SELECT * FROM cypher('issue_1399', $$
  MATCH (foo)
  WHERE EXISTS((foo)-[:BAR2]->())
  RETURN foo
$$) as (c agtype);
-- this should return 2 rows as they all exist
SELECT * FROM cypher('issue_1399', $$
  MATCH (foo)
  WHERE NOT EXISTS((foo)-[:BAR2]->())
  RETURN foo
$$) as (c agtype);

-- Issue 1393 EXISTS doesn't see previous clauses' variables
SELECT FROM create_graph('issue_1393');
SELECT * FROM cypher('issue_1393', $$
  CREATE (n1:Object) RETURN n1
$$) AS (n1 agtype);
-- vertex cases
SELECT * FROM cypher('issue_1393', $$
  MATCH (n1:Object) MATCH (n2:Object) WHERE EXISTS((n1)-[]->(n2)) RETURN n1,n2
$$) AS (n1 agtype, n2 agtype);
SELECT * FROM cypher('issue_1393', $$
  MATCH (n1:Object) MATCH (n2:Object) WHERE EXISTS((n1:Object)-[]->(n2:Object)) RETURN n1,n2
$$) AS (n1 agtype, n2 agtype);
SELECT * FROM cypher('issue_1393', $$
  MATCH (n1:Object) MATCH (n2:Object) WHERE NOT EXISTS((n1)-[]->(n2)) RETURN n1,n2
$$) AS (n1 agtype, n2 agtype);
SELECT * FROM cypher('issue_1393', $$
  MATCH (n1:Object) MATCH (n2:Object) WHERE NOT EXISTS((n1:Object)-[]->(n2:Object)) RETURN n1,n2
$$) AS (n1 agtype, n2 agtype);
SELECT * FROM cypher('issue_1393', $$
  MATCH (n1:Object) MATCH (n2:Object) WHERE EXISTS((n1)-[]->()) RETURN n1,n2
$$) AS (n1 agtype, n2 agtype);
SELECT * FROM cypher('issue_1393', $$
  MATCH (n1:Object) MATCH (n2:Object) WHERE NOT EXISTS((n1)-[]->()) RETURN n1,n2
$$) AS (n1 agtype, n2 agtype);
SELECT * FROM cypher('issue_1393', $$
  CREATE (n1:Object)-[e:knows]->(n2:Object) RETURN n1, e, n2
$$) AS (n1 agtype, e agtype, n2 agtype);
SELECT * FROM cypher('issue_1393', $$
  MATCH (n1:Object) MATCH (n2:Object) WHERE EXISTS((n1)-[]->(n2)) RETURN n1,n2
$$) AS (n1 agtype, n2 agtype);
SELECT * FROM cypher('issue_1393', $$
  MATCH (n1:Object) MATCH (n2:Object) WHERE EXISTS((n1:Object)-[]->(n2:Object)) RETURN n1,n2
$$) AS (n1 agtype, n2 agtype);
SELECT * FROM cypher('issue_1393', $$
  MATCH (n1:Object) MATCH (n2:Object) WHERE NOT EXISTS((n1)-[]->(n2)) RETURN n1,n2
$$) AS (n1 agtype, n2 agtype);
SELECT * FROM cypher('issue_1393', $$
  MATCH (n1:Object) MATCH (n2:Object) WHERE NOT EXISTS((n1:Object)-[]->(n2:Object)) RETURN n1,n2
$$) AS (n1 agtype, n2 agtype);
SELECT * FROM cypher('issue_1393', $$
  MATCH (n1:Object) MATCH (n2:Object) WHERE EXISTS((n1)-[]->()) RETURN n1,n2
$$) AS (n1 agtype, n2 agtype);
SELECT * FROM cypher('issue_1393', $$
  MATCH (n1:Object) MATCH (n2:Object) WHERE NOT EXISTS((n1)-[]->()) RETURN n1,n2
$$) AS (n1 agtype, n2 agtype);
SELECT * FROM cypher('issue_1393', $$
  MATCH (n1:Object) MATCH (n2:Object) WHERE EXISTS((n1:Object)-[]->()) RETURN n1,n2
$$) AS (n1 agtype, n2 agtype);
SELECT * FROM cypher('issue_1393', $$
  MATCH (n1:Object) MATCH (n2:Object) WHERE NOT EXISTS((n1:Object)-[]->()) RETURN n1,n2
$$) AS (n1 agtype, n2 agtype);
-- should error
SELECT * FROM cypher('issue_1393', $$
  MATCH (n1:Object) MATCH (n2:Object) WHERE EXISTS((n1:object)-[]->()) RETURN n1,n2
$$) AS (n1 agtype, n2 agtype);
SELECT * FROM cypher('issue_1393', $$
  MATCH (n1:Object) MATCH (n2:Object) WHERE NOT EXISTS((n1:object)-[]->()) RETURN n1,n2
$$) AS (n1 agtype, n2 agtype);
SELECT * FROM cypher('issue_1393', $$
  MATCH (n1:Object) MATCH (n2:Object) WHERE NOT EXISTS((n1)-[e]->()) RETURN n1,n2,e
$$) AS (n1 agtype, n2 agtype, e agtype);
-- edge cases
SELECT * FROM cypher('issue_1393', $$
  MATCH (n1:Object)-[e1:knows]->() MATCH (n2:Object) WHERE EXISTS((n1)-[e1]->(n2)) RETURN e1
$$) AS (e1 agtype);
SELECT * FROM cypher('issue_1393', $$
  MATCH (n1:Object)-[e1:knows]->() MATCH (n2:Object) WHERE EXISTS((n1:Object)-[e1:knows]->(n2:Object)) RETURN e1
$$) AS (e1 agtype);
SELECT * FROM cypher('issue_1393', $$
  MATCH (n1:Object)-[e1:knows]->() MATCH (n2:Object) WHERE NOT EXISTS((n1)-[e1]->(n2)) RETURN e1
$$) AS (e1 agtype);
SELECT * FROM cypher('issue_1393', $$
  MATCH (n1:Object)-[e1:knows]->() MATCH (n2:Object) WHERE NOT EXISTS((n1:Object)-[e1:knows]->(n2:Object)) RETURN e1
$$) AS (e1 agtype);
SELECT * FROM cypher('issue_1393', $$
  MATCH (n1:Object)-[e1:knows]->() MATCH (n2:Object) WHERE EXISTS((n1)-[e1]->()) RETURN e1
$$) AS (e1 agtype);
SELECT * FROM cypher('issue_1393', $$
  MATCH (n1:Object)-[e1:knows]->() MATCH (n2:Object) WHERE EXISTS((n1:Object)-[e1:knows]->(n2:Object)) RETURN e1
$$) AS (e1 agtype);
SELECT * FROM cypher('issue_1393', $$
  MATCH (n1:Object)-[e1:knows]->() MATCH (n2:Object) WHERE NOT EXISTS((n1)-[e1]->()) RETURN e1
$$) AS (e1 agtype);
SELECT * FROM cypher('issue_1393', $$
  MATCH (n1:Object)-[e1:knows]->() MATCH (n2:Object) WHERE NOT EXISTS((n1:Object)-[e1:knows]->(n2:Object)) RETURN e1
$$) AS (e1 agtype);
-- should error
SELECT * FROM cypher('issue_1393', $$
  MATCH (n1:Object)-[e1:knows]->() MATCH (n2:Object) WHERE EXISTS((n1:Object)-[e1:Knows]->(n2:Object)) RETURN e1
$$) AS (e1 agtype);
SELECT * FROM cypher('issue_1393', $$
  MATCH (n1:Object)-[e1:knows]->() MATCH (n2:Object) WHERE NOT EXISTS((n1:Object)-[e1:Knows]->(n2:Object)) RETURN e1
$$) AS (e1 agtype);
SELECT * FROM cypher('issue_1393', $$
  MATCH (n1:Object)-[e1:knows]->() MATCH (n2:Object) WHERE EXISTS((e1:Object)-[e1:Knows]->(n2:Object)) RETURN e1
$$) AS (e1 agtype);
SELECT * FROM cypher('issue_1393', $$
  MATCH (n1:Object)-[e1:knows]->() MATCH (n2:Object) WHERE NOT EXISTS((n1:Object)-[e1:knows]->(e1)) RETURN e1
$$) AS (e1 agtype);
SELECT * FROM cypher('issue_1393', $$
  MATCH (n1:Object)-[e1:knows]->() MATCH (n2:Object) WHERE NOT EXISTS((n1:Object)-[e1:knows]->(e2)) RETURN e1
$$) AS (e1 agtype);
SELECT * FROM cypher('issue_1393', $$
  MATCH p=(n1:Object)-[e1:knows]->() MATCH (n2:Object) WHERE EXISTS((n1:Object)-[p]->(e2)) RETURN e1
$$) AS (e1 agtype);
SELECT * FROM cypher('issue_1393', $$
  MATCH p=(n1)-[e1]->() MATCH (n2) WHERE EXISTS((n1)-[p]->(e2)) RETURN p
$$) AS (e1 agtype);
-- long cases
SELECT * FROM cypher('issue_1393', $$
  MATCH (n1) MATCH (n2) MATCH (n1)-[e1]->() MATCH (n2)<-[e2]-(n1) MATCH (n3)
  WHERE EXISTS((n3)-[e1]->(n2)) RETURN n1,n2,n3,e1
$$) AS (n1 agtype, n2 agtype, n3 agtype, e1 agtype);
SELECT * FROM cypher('issue_1393', $$
  MATCH (n1:Object) MATCH (n2:Object) MATCH (n1)-[e1:knows]->() MATCH (n2)<-[e2:knows]-(n1) MATCH (n3:Object)
  WHERE EXISTS((n3:Object)-[e1:knows]->(n2:Object)) RETURN n1,n2,n3,e1
$$) AS (n1 agtype, n2 agtype, n3 agtype, e1 agtype);

--
-- Issue 1461
--

-- Using the test_enable_containment graph for these tests
SELECT * FROM cypher('test_enable_containment', $$ CREATE p=(:Customer)-[:bought {store:'Amazon', addr:{city: 'Vancouver', street: 30}}]->(y:Product) RETURN p $$) as (a agtype);


-- Should return 0
SELECT count(*) FROM cypher('test_enable_containment', $$ MATCH (x:Customer ={addr:[{city:'Toronto'}]}) RETURN x $$) as (a agtype);
SELECT count(*) FROM cypher('test_enable_containment', $$ MATCH (x:Customer ={school:{program:{major:'Psyc'}}}) RETURN x $$) as (a agtype);
SELECT count(*) FROM cypher('test_enable_containment', $$ MATCH (x:Customer ={name:'Bob',school:{program:{degree:'BSc'}}}) RETURN x $$) as (a agtype);
SELECT count(*) FROM cypher('test_enable_containment', $$ MATCH (x:Customer ={school:{program:{major:'Cs'}}}) RETURN x $$) as (a agtype);
SELECT count(*) FROM cypher('test_enable_containment', $$ MATCH (x:Customer ={name:'Bob',school:{program:{degree:'PHd'}}}) RETURN x $$) as (a agtype);
SELECT count(*) FROM cypher('test_enable_containment', $$ MATCH (x:Customer ={phone:[987654321]}) RETURN x $$) as (a agtype);
SELECT count(*) FROM cypher('test_enable_containment', $$ MATCH (x:Customer ={phone:[654765876]}) RETURN x $$) as (a agtype);
SELECT count(*) FROM cypher('test_enable_containment', $$ MATCH (x)-[:bought ={store: 'Amazon', addr:{city: 'Vancouver'}}]->() RETURN x $$) as (a agtype);

-- Should return 1
SELECT count(*) FROM cypher('test_enable_containment', $$ MATCH (x:Customer ={addr: [{city: 'Vancouver', street: 30},{city: 'Toronto', street: 40}]}) RETURN x $$) as (a agtype);
SELECT count(*) FROM cypher('test_enable_containment', $$ MATCH (x:Customer ={school: { name: 'XYZ College',program: { major: 'Psyc', degree: 'BSc'}}}) RETURN x $$) as (a agtype);
SELECT count(*) FROM cypher('test_enable_containment', $$ MATCH (x:Customer ={phone: [ 123456789, 987654321, 456987123 ]}) RETURN x $$) as (a agtype);
SELECT count(*) FROM cypher('test_enable_containment', $$ MATCH (x:Customer ={school: { name: 'XYZ College',program: { major: 'Psyc', degree: 'BSc'} },phone: [ 123456789, 987654321, 456987123 ]}) RETURN x $$) as (a agtype);
SELECT count(*) FROM cypher('test_enable_containment', $$ MATCH p=(x:Customer)-[:bought ={store: 'Amazon'}]->() RETURN p $$) as (a agtype);
SELECT count(*) FROM cypher('test_enable_containment', $$ MATCH p=(x:Customer)-[:bought ={store: 'Amazon', addr:{city: 'Vancouver', street: 30}}]->() RETURN p $$) as (a agtype);
SELECT count(*) FROM cypher('test_enable_containment', $$ MATCH p=(x:Customer)-[:bought {store: 'Amazon', addr:{city: 'Vancouver'}}]->() RETURN p $$) as (a agtype);

\pset format unaligned
SELECT * FROM cypher('test_enable_containment', $$ EXPLAIN (costs off) MATCH (x:Customer)-[:bought ={store: 'Amazon', addr:{city: 'Vancouver', street: 30}}]->(y:Product) RETURN 0 $$) as (a agtype);
SELECT * FROM cypher('test_enable_containment', $$ EXPLAIN (costs off) MATCH (x:Customer ={school: { name: 'XYZ College',program: { major: 'Psyc', degree: 'BSc'} },phone: [ 123456789, 987654321, 456987123 ]}) RETURN 0 $$) as (a agtype);
\pset format aligned

--
-- issue 2308: MATCH after CREATE returns 0 rows
--
-- When all MATCH variables are already bound from a preceding CREATE + WITH,
-- the MATCH filter quals must evaluate after CREATE, not before.
--
SELECT create_graph('issue_2308');

-- Reporter's exact case: CREATE + WITH + MATCH + SET + RETURN
SELECT * FROM cypher('issue_2308', $$
    CREATE (a:TestB3)-[e:B3REL]->(b:TestB3)
    WITH a, e, b
    MATCH p = (a)-[e]->(b)
    SET a.something = 'something'
    RETURN a
$$) AS (a agtype);

-- Bound variables, no SET
SELECT * FROM cypher('issue_2308', $$
    CREATE (a:T2)-[e:R2]->(b:T2)
    WITH a, e, b
    MATCH (a)-[e]->(b)
    RETURN a, e, b
$$) AS (a agtype, e agtype, b agtype);

-- Reversed direction: filter should reject (0 rows expected)
SELECT * FROM cypher('issue_2308', $$
    CREATE (a:T3)-[e:R3]->(b:T3)
    WITH a, e, b
    MATCH (b)-[e]->(a)
    RETURN a
$$) AS (a agtype);

-- Node-only MATCH with bound variable
SELECT * FROM cypher('issue_2308', $$
    CREATE (a:T4 {name: 'test'})
    WITH a
    MATCH (a)
    RETURN a
$$) AS (a agtype);

-- MATCH after SET (SET is also DML, chain must be protected)
SELECT * FROM cypher('issue_2308', $$
    CREATE (a:T5 {val: 1})-[e:R5]->(b:T5 {val: 2})
$$) AS (r agtype);
SELECT * FROM cypher('issue_2308', $$
    MATCH (a:T5)-[e:R5]->(b:T5)
    SET a.val = 10
    WITH a, e, b
    MATCH (a)-[e]->(b)
    RETURN a.val
$$) AS (val agtype);

SELECT drop_graph('issue_2308', true);
-- Issue 1964
--
-- PREPARE with property parameter ($props) crashed the server when
-- transform_map_to_ind_recursive blindly cast cypher_param nodes to
-- cypher_map, accessing invalid memory.
--

SELECT create_graph('issue_1964');
SELECT * FROM cypher('issue_1964', $$
    CREATE (:Person {name: 'Alice', age: 30}),
           (:Person {name: 'Bob', age: 25})
$$) AS (result agtype);
SELECT * FROM cypher('issue_1964', $$
    CREATE (:Person {name: 'Alice'})-[:KNOWS {since: 2020}]->(:Person {name: 'Bob'})
$$) AS (result agtype);


PREPARE issue_1964_vertex(agtype) AS
    SELECT * FROM cypher('issue_1964',
        $$MATCH (n $props) RETURN n $$, $1) AS (p agtype);
EXECUTE issue_1964_vertex('{"props": {"name": "Alice"}}');
EXECUTE issue_1964_vertex('{"props": {"age": 25}}');
DEALLOCATE issue_1964_vertex;

PREPARE issue_1964_edge(agtype) AS
    SELECT * FROM cypher('issue_1964',
        $$MATCH ()-[r $props]->() RETURN r $$, $1) AS (p agtype);
EXECUTE issue_1964_edge('{"props": {"since": 2020}}');
DEALLOCATE issue_1964_edge;


PREPARE issue_1964_vertex_on(agtype) AS
    SELECT * FROM cypher('issue_1964',
        $$MATCH (n $props) RETURN n $$, $1) AS (p agtype);
EXECUTE issue_1964_vertex_on('{"props": {"name": "Alice"}}');
DEALLOCATE issue_1964_vertex_on;


PREPARE issue_1964_vertex_eq(agtype) AS
    SELECT * FROM cypher('issue_1964',
        $$MATCH (n = $props) RETURN n $$, $1) AS (p agtype);
EXECUTE issue_1964_vertex_eq('{"props": {"name": "Alice", "age": 25}}');
DEALLOCATE issue_1964_vertex_eq;

PREPARE issue_1964_edge_eq(agtype) AS
    SELECT * FROM cypher('issue_1964',
        $$MATCH ()-[r = $props]->() RETURN r $$, $1) AS (p agtype);
EXECUTE issue_1964_edge_eq('{"props": {"since": 2020}}');
DEALLOCATE issue_1964_edge_eq;


PREPARE issue_1964_vertex_eq_on(agtype) AS
    SELECT * FROM cypher('issue_1964',
        $$MATCH (n = $props) RETURN n $$, $1) AS (p agtype);
EXECUTE issue_1964_vertex_eq_on('{"props": {"name": "Alice", "age": 25}}');
DEALLOCATE issue_1964_vertex_eq_on;

--
-- Issue 2193: CREATE ... WITH ... MATCH on brand-new label returns 0 rows
-- on first execution because match_check_valid_label() runs before
-- transform_prev_cypher_clause() creates the label table.
--
SELECT create_graph('issue_2193');

-- Reporter's exact case: CREATE two Person nodes, then MATCH on Person
-- Should return 2 rows on the very first execution
SELECT * FROM cypher('issue_2193', $$
    CREATE (a:Person {name: 'Jane', livesIn: 'London'}),
           (b:Person {name: 'Tom', livesIn: 'Copenhagen'})
    WITH a, b
    MATCH (p:Person)
    RETURN p.name ORDER BY p.name
$$) AS (result agtype);

-- Single CREATE + MATCH on brand-new label
SELECT * FROM cypher('issue_2193', $$
    CREATE (a:City {name: 'Berlin'})
    WITH a
    MATCH (c:City)
    RETURN c.name ORDER BY c.name
$$) AS (result agtype);

-- MATCH on a label that now exists (second execution) still works
SELECT * FROM cypher('issue_2193', $$
    CREATE (a:City {name: 'Paris'})
    WITH a
    MATCH (c:City)
    RETURN c.name ORDER BY c.name
$$) AS (result agtype);

-- MATCH on non-existent label without DML predecessor still returns 0 rows
SELECT * FROM cypher('issue_2193', $$
    MATCH (x:NonExistentLabel)
    RETURN x
$$) AS (result agtype);

-- MATCH on non-existent label after DML predecessor still returns 0 rows
-- and MATCH-introduced variable (p) is properly registered
SELECT * FROM cypher('issue_2193', $$
    CREATE (a:Person {name: 'Alice'})
    WITH a
    MATCH (p:NonExistentLabel)
    RETURN p
$$) AS (result agtype);

-- Verify that the CREATE side effect was preserved even though MATCH
-- returned 0 rows (guards against plan-elimination regressions where
-- a constant-false predicate causes PG to skip the DML predecessor)
SELECT * FROM cypher('issue_2193', $$
    MATCH (a:Person {name: 'Alice'})
    RETURN a.name
$$) AS (result agtype);

SELECT drop_graph('issue_2193', true);

--
-- Issue 2378: OPTIONAL MATCH may incorrectly drop null-preserving outer
-- rows when its WHERE clause contains a correlated sub-pattern predicate.
--
-- Cypher OPTIONAL MATCH semantics: the WHERE applies to the optional
-- binding; when no right-hand row survives the predicate, the outer row
-- is still emitted with NULLs in the optional columns.  Before the fix,
-- a WHERE containing EXISTS { ... } or COUNT { ... } was attached as an
-- outer filter on the transformed subquery, so it ran after the LATERAL
-- LEFT JOIN produced null-preserving rows and then incorrectly dropped
-- them when the predicate evaluated NULL/false on the nulled side.
--
SELECT create_graph('issue_2378');
SELECT * FROM cypher('issue_2378', $$
    CREATE (a:Person {name: 'Alice'}),
           (b:Person {name: 'Bob'}),
           (c:Person {name: 'Charlie'}),
           (a)-[:KNOWS]->(b),
           (a)-[:KNOWS]->(c)
$$) AS (v agtype);

-- Correlated EXISTS referencing the optional variable (friend).
-- Neither Bob nor Charlie knows anyone, so for every outer p the
-- predicate fails on all optional matches; expect one row per person
-- with friend = NULL.
SELECT * FROM cypher('issue_2378', $$
    MATCH (p:Person)
    OPTIONAL MATCH (p)-[:KNOWS]->(friend:Person)
    WHERE EXISTS { (friend)-[:KNOWS]->(:Person) }
    RETURN p.name AS name, friend.name AS friend
    ORDER BY name
$$) AS (name agtype, friend agtype);

-- Correlated EXISTS referencing the outer variable (p).
-- Alice knows someone so her optional matches pass; Bob and Charlie
-- don't, so they are emitted with NULL friend.
SELECT * FROM cypher('issue_2378', $$
    MATCH (p:Person)
    OPTIONAL MATCH (p)-[:KNOWS]->(friend:Person)
    WHERE EXISTS { (p)-[:KNOWS]->(:Person) }
    RETURN p.name AS name, friend.name AS friend
    ORDER BY name, friend
$$) AS (name agtype, friend agtype);

-- Non-correlated EXISTS (was already working; kept as a regression guard).
SELECT * FROM cypher('issue_2378', $$
    MATCH (p:Person)
    OPTIONAL MATCH (p)-[:KNOWS]->(friend:Person)
    WHERE EXISTS { MATCH (x:Person) RETURN x }
    RETURN p.name AS name, friend.name AS friend
    ORDER BY name, friend
$$) AS (name agtype, friend agtype);

-- Plain scalar predicate on the optional variable (was already working).
SELECT * FROM cypher('issue_2378', $$
    MATCH (p:Person)
    OPTIONAL MATCH (p)-[:KNOWS]->(friend:Person)
    WHERE friend.name = 'Bob'
    RETURN p.name AS name, friend.name AS friend
    ORDER BY name
$$) AS (name agtype, friend agtype);

-- Constant-false WHERE on the optional side (was already working).
SELECT * FROM cypher('issue_2378', $$
    MATCH (p:Person)
    OPTIONAL MATCH (p)-[:KNOWS]->(friend:Person)
    WHERE false
    RETURN p.name AS name, friend.name AS friend
    ORDER BY name
$$) AS (name agtype, friend agtype);

SELECT drop_graph('issue_2378', true);

DO $fixed_path_variadic_payload_smoke$
DECLARE
    len text;
    node_count text;
    rel_count text;
BEGIN
    PERFORM create_graph('path_payload_smoke');
    EXECUTE
        'SELECT * FROM cypher(''path_payload_smoke'',
             $$CREATE (:vfast)-[:efast]->(:vfast)-[:efast]->(:vfast)-[:efast]->(:vfast)-[:efast]->(:vfast)-[:efast]->(:vfast)$$)
         AS (a agtype)';
    EXECUTE
        'SELECT len::text, node_count::text, rel_count::text
         FROM cypher(''path_payload_smoke'',
             $$MATCH p=(:vfast)-[:efast]->(:vfast)-[:efast]->(:vfast)-[:efast]->(:vfast)-[:efast]->(:vfast)-[:efast]->(:vfast)
               RETURN length(p), size(nodes(p)), size(relationships(p))$$)
         AS (len agtype, node_count agtype, rel_count agtype)'
    INTO len, node_count, rel_count;

    IF len <> '5' OR node_count <> '6' OR rel_count <> '5' THEN
        RAISE EXCEPTION 'unexpected 5-hop path shape: length %, nodes %, relationships %',
                        len, node_count, rel_count;
    END IF;

    PERFORM drop_graph('path_payload_smoke', true);
END
$fixed_path_variadic_payload_smoke$;

SELECT create_property_source_index_named(
    'cypher_match_numeric_path', 'NumericPath', 'payload.a',
    'numeric_path_payload_a_source', 'agtype');

EXPLAIN (VERBOSE, COSTS OFF)
SELECT array_length(array_agg(v), 1)
FROM cypher('cypher_match_numeric_path',
            $$MATCH (n:NumericPath) RETURN n.payload.a$$) AS (v agtype);

--
-- Clean up
--
SELECT drop_graph('cypher_match', true);
SELECT drop_graph('cypher_match_plan', true);
SELECT drop_graph('cypher_match_numeric_path', true);
SELECT drop_graph('test_retrieve_var', true);
SELECT drop_graph('test_enable_containment', true);
SELECT drop_graph('issue_945', true);
SELECT drop_graph('issue_1399', true);
SELECT drop_graph('issue_1393', true);
SELECT drop_graph('issue_1964', true);

--
-- End
--
