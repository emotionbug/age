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

COPY (
SELECT amname || ':' || amtype::text
FROM pg_am
WHERE amname = 'age_adjacency'
) TO STDOUT;

COPY (
SELECT opc.opcname || ':' || t.typname || ':' || am.amname || ':' ||
       opc.opcdefault
FROM pg_opclass opc
JOIN pg_am am ON am.oid = opc.opcmethod
JOIN pg_type t ON t.oid = opc.opcintype
WHERE am.amname = 'age_adjacency'
ORDER BY 1
) TO STDOUT;

CREATE TEMP TABLE age_adjacency_smoke
(
    id graphid NOT NULL,
    start_id graphid NOT NULL,
    end_id graphid NOT NULL
);

INSERT INTO age_adjacency_smoke VALUES
(_graphid(2, 1), _graphid(1, 1), _graphid(1, 2)),
(_graphid(2, 2), _graphid(1, 1), _graphid(1, 3)),
(_graphid(2, 3), _graphid(1, 4), _graphid(1, 5));

CREATE INDEX age_adjacency_smoke_bad_idx
ON age_adjacency_smoke USING age_adjacency (start_id);

CREATE INDEX age_adjacency_smoke_start_idx
ON age_adjacency_smoke USING age_adjacency (start_id, id, end_id);

INSERT INTO age_adjacency_smoke VALUES
(_graphid(2, 4), _graphid(1, 1), _graphid(1, 6));

SET enable_seqscan = off;

COPY (
SELECT count(*)::text
FROM age_adjacency_smoke
WHERE start_id = _graphid(1, 1)
) TO STDOUT;

RESET enable_seqscan;

COPY (
SELECT edge_id::text || ':' || next_vertex_id::text
FROM age_adjacency_debug_payload('age_adjacency_smoke_start_idx'::regclass,
                                 _graphid(1, 1))
ORDER BY edge_id
) TO STDOUT;

DELETE FROM age_adjacency_smoke
WHERE id = _graphid(2, 2);

COPY (
SELECT edge_id::text || ':' || next_vertex_id::text
FROM age_adjacency_debug_payload('age_adjacency_smoke_start_idx'::regclass,
                                 _graphid(1, 1))
ORDER BY edge_id
) TO STDOUT;

COPY (
SELECT postings::text
FROM age_adjacency_debug_stats('age_adjacency_smoke_start_idx'::regclass)
) TO STDOUT;

DO $$
BEGIN
    PERFORM create_graph('age_adj_vle');
    PERFORM create_vlabel('age_adj_vle', 'N');
    PERFORM create_elabel('age_adj_vle', 'R');
END
$$;

INSERT INTO age_adj_vle."N"(id, properties) VALUES
(_graphid(_label_id('age_adj_vle', 'N'), 1), '{"i": 0}'::agtype),
(_graphid(_label_id('age_adj_vle', 'N'), 2), '{"i": 1}'::agtype),
(_graphid(_label_id('age_adj_vle', 'N'), 3), '{"i": 2}'::agtype);

INSERT INTO age_adj_vle."R"(id, start_id, end_id, properties) VALUES
(_graphid(_label_id('age_adj_vle', 'R'), 1),
 _graphid(_label_id('age_adj_vle', 'N'), 1),
 _graphid(_label_id('age_adj_vle', 'N'), 2),
 '{"kind": "forward"}'::agtype),
(_graphid(_label_id('age_adj_vle', 'R'), 2),
 _graphid(_label_id('age_adj_vle', 'N'), 2),
 _graphid(_label_id('age_adj_vle', 'N'), 3),
 '{"kind": "forward"}'::agtype),
(_graphid(_label_id('age_adj_vle', 'R'), 3),
 _graphid(_label_id('age_adj_vle', 'N'), 2),
 _graphid(_label_id('age_adj_vle', 'N'), 2),
 '{"kind": "self"}'::agtype);

CREATE INDEX age_adj_vle_r_start_payload_idx
ON age_adj_vle."R" USING age_adjacency (start_id, id, end_id);

CREATE INDEX age_adj_vle_r_end_payload_idx
ON age_adj_vle."R" USING age_adjacency (end_id, id, start_id);

COPY (
SELECT count(*)::text
FROM cypher('age_adj_vle',
            $$MATCH p=(:N {i: 0})-[:R*1..2]->() RETURN p$$)
AS (p agtype)
) TO STDOUT;

COPY (
SELECT count(*)::text
FROM cypher('age_adj_vle',
            $$MATCH p=()-[:R*1..2]->(:N {i: 2}) RETURN p$$)
AS (p agtype)
) TO STDOUT;

COPY (
SELECT count(*)::text
FROM cypher('age_adj_vle',
            $$MATCH p=(:N {i: 1})-[:R*1..1]-() RETURN p$$)
AS (p agtype)
) TO STDOUT;

DO $$
BEGIN
    PERFORM drop_graph('age_adj_vle', true);
END
$$;

DO $age_custom_path$
DECLARE
    graph_name text := 'age_adj_custom_path';
    n_label_id int;
    r_label_id int;
    plan_text text;
    first_cost_line text;
    cost_match text[];
    has_disabled_custom_scan boolean := false;
    has_custom_scan boolean := false;
    has_left_direction_custom_scan boolean := false;
    has_right_label_custom_scan boolean := false;
    has_excluded_custom_scan boolean := false;
    has_join_build_vertex boolean := false;
    has_direct_id_qual boolean := false;
    has_direct_edge_id_qual boolean := false;
    has_direct_edge_start_id_qual boolean := false;
    has_direct_edge_end_id_qual boolean := false;
    has_direct_edge_properties_qual boolean := false;
    has_direct_edge_label_expr boolean := false;
    has_direct_edge_id_projection boolean := false;
    has_direct_edge_start_id_projection boolean := false;
    has_direct_edge_end_id_projection boolean := false;
    has_direct_edge_properties_projection boolean := false;
    has_direct_edge_property_indirection boolean := false;
    has_direct_edge_keys_projection boolean := false;
    has_direct_edge_map_projection boolean := false;
    has_direct_edge_start_properties_projection boolean := false;
    has_direct_edge_end_keys_projection boolean := false;
    has_direct_edge_start_property_access boolean := false;
    has_direct_edge_end_property_access boolean := false;
    has_direct_edge_start_id_function boolean := false;
    has_direct_edge_end_id_function boolean := false;
    has_direct_edge_start_node_projection boolean := false;
    has_direct_edge_end_node_projection boolean := false;
    has_direct_edge_start_label_projection boolean := false;
    has_direct_edge_end_labels_projection boolean := false;
    has_direct_edge_start_null_test boolean := false;
    has_direct_edge_end_null_test boolean := false;
    has_direct_edge_start_id_null_test boolean := false;
    has_direct_edge_end_id_null_test boolean := false;
    has_direct_previous_edge_start_properties boolean := false;
    has_lazy_edge_return_projection boolean := false;
    has_delayed_named_edge_surface boolean := false;
    has_direct_edge_null_test_projection boolean := false;
    has_direct_vertex_keys_qual boolean := false;
    has_direct_vertex_labels_qual boolean := false;
    has_direct_previous_vertex_keys_qual boolean := false;
    has_direct_previous_vertex_labels_qual boolean := false;
    has_direct_vertex_map_projection boolean := false;
    has_direct_vertex_null_test_projection boolean := false;
    has_direct_fixed_path_length_projection boolean := false;
    has_direct_fixed_path_relationships_projection boolean := false;
    has_direct_fixed_path_nodes_projection boolean := false;
    has_direct_fixed_path_head_node_projection boolean := false;
    has_direct_fixed_path_last_node_projection boolean := false;
    has_direct_fixed_path_head_edge_projection boolean := false;
    has_direct_fixed_path_last_edge_projection boolean := false;
    has_direct_fixed_path_head_node_id boolean := false;
    has_direct_fixed_path_last_node_properties boolean := false;
    has_direct_fixed_path_head_edge_type boolean := false;
    has_direct_fixed_path_last_edge_start_id boolean := false;
    has_direct_fixed_path_tail_nodes_projection boolean := false;
    has_direct_fixed_path_tail_relationships_projection boolean := false;
    has_direct_fixed_path_tail_head_node_id boolean := false;
    has_direct_fixed_path_tail_last_node_properties boolean := false;
    has_direct_fixed_path_reverse_nodes_projection boolean := false;
    has_direct_fixed_path_reverse_relationships_projection boolean := false;
    has_direct_fixed_path_reverse_node_id boolean := false;
    has_direct_fixed_path_reverse_node_properties boolean := false;
    has_direct_fixed_path_reverse_edge_type boolean := false;
    has_direct_fixed_path_reverse_edge_start_id boolean := false;
    has_direct_fixed_path_nodes_size boolean := false;
    has_direct_fixed_path_tail_relationships_size boolean := false;
    has_direct_fixed_path_reverse_nodes_is_empty boolean := false;
    has_direct_fixed_path_tail_relationships_is_empty boolean := false;
    has_direct_fixed_path_node_suffix_slice boolean := false;
    has_direct_fixed_path_node_prefix_slice boolean := false;
    has_direct_fixed_path_relationship_suffix_slice boolean := false;
    has_direct_fixed_path_relationship_prefix_slice boolean := false;
    has_direct_fixed_path_node_slice_size boolean := false;
    has_direct_fixed_path_relationship_slice_is_empty boolean := false;
    has_direct_fixed_path_node_slice_head_id boolean := false;
    has_direct_fixed_path_relationship_slice_head_properties boolean := false;
    has_direct_fixed_path_node_slice_tail_projection boolean := false;
    has_direct_fixed_path_relationship_slice_tail_projection boolean := false;
    has_direct_fixed_path_node_slice_reverse_projection boolean := false;
    has_direct_fixed_path_relationship_slice_reverse_projection boolean := false;
    has_direct_fixed_path_node_slice_tail_head_id boolean := false;
    has_direct_fixed_path_rel_slice_rev_head_props boolean := false;
    has_direct_fixed_path_node_slice_tail_head_prop boolean := false;
    has_direct_fixed_path_rel_slice_rev_head_prop_access boolean := false;
    has_direct_fixed_path_rel_slice_start_node_prop boolean := false;
    has_direct_fixed_path_rel_slice_end_node_id boolean := false;
    has_direct_fixed_path_indexed_node_id boolean := false;
    has_direct_fixed_path_indexed_edge_id boolean := false;
    has_direct_fixed_path_indexed_node_properties boolean := false;
    has_direct_fixed_path_indexed_edge_properties boolean := false;
    has_direct_fixed_path_indexed_node_label boolean := false;
    has_direct_fixed_path_indexed_edge_type boolean := false;
    has_direct_fixed_path_indexed_node_labels boolean := false;
    has_direct_fixed_path_indexed_node_property_access boolean := false;
    has_direct_fixed_path_indexed_edge_property_access boolean := false;
    has_direct_fixed_path_indexed_node_keys boolean := false;
    has_direct_fixed_path_indexed_edge_keys boolean := false;
    has_direct_fixed_path_indexed_edge_start_id boolean := false;
    has_direct_fixed_path_indexed_edge_end_id boolean := false;
    has_direct_fixed_path_indexed_start_node_property_access boolean := false;
    has_direct_fixed_path_indexed_end_node_property_access boolean := false;
    has_direct_fixed_path_indexed_start_node_projection boolean := false;
    has_direct_fixed_path_indexed_end_node_projection boolean := false;
    has_direct_fixed_path_indexed_start_node_id boolean := false;
    has_direct_fixed_path_indexed_end_node_properties boolean := false;
    has_direct_fixed_path_indexed_start_node_label boolean := false;
    has_direct_fixed_path_indexed_end_node_keys boolean := false;
    has_pathless_vle_build_path boolean := false;
    has_pathless_left_vle_no_build_path boolean := false;
    has_pathless_right_vle_no_build_path boolean := false;
    has_pathless_undirected_vle_no_build_path boolean := false;
    has_pathless_named_edge_vle_no_build_path boolean := false;
    has_pathless_right_vle_terminal_id_join boolean := false;
    has_pathless_right_vle_direct_property_projection boolean := false;
    has_direct_variable_vle_length boolean := false;
    has_direct_variable_vle_length_projection boolean := false;
    has_direct_variable_vle_node_count boolean := false;
    has_direct_variable_vle_node_count_projection boolean := false;
    has_direct_variable_vle_edge_count boolean := false;
    has_direct_variable_vle_edge_count_projection boolean := false;
    has_direct_variable_vle_edge_slice_count boolean := false;
    has_direct_variable_vle_node_slice_empty boolean := false;
    has_direct_variable_vle_head_edge_id_filter boolean := false;
    has_direct_variable_vle_indexed_edge_property_filter boolean := false;
    has_direct_variable_vle_indexed_node_property_filter boolean := false;
    has_direct_variable_vle_edge_membership boolean := false;
    has_direct_variable_vle_edge_equality boolean := false;
    has_direct_variable_vle_reverse_edge_equality boolean := false;
    has_direct_variable_vle_tail_reverse_edge_equality boolean := false;
    has_direct_variable_vle_indexed_node_keys boolean := false;
    has_direct_variable_vle_indexed_node_keys_projection boolean := false;
    has_direct_variable_vle_indexed_edge_keys boolean := false;
    has_direct_variable_vle_indexed_edge_keys_projection boolean := false;
    has_direct_variable_vle_boundary_node_keys boolean := false;
    has_direct_variable_vle_boundary_node_keys_projection boolean := false;
    has_direct_variable_vle_boundary_edge_keys boolean := false;
    has_direct_variable_vle_boundary_edge_keys_projection boolean := false;
    has_direct_variable_vle_boundary_node_properties boolean := false;
    has_direct_variable_vle_boundary_node_properties_projection boolean := false;
    has_direct_variable_vle_boundary_edge_type boolean := false;
    has_direct_variable_vle_boundary_edge_type_projection boolean := false;
    has_direct_variable_vle_tail_indexed_node_properties boolean := false;
    has_direct_variable_vle_tail_indexed_node_keys boolean := false;
    has_direct_variable_vle_reverse_indexed_edge_type boolean := false;
    has_direct_variable_vle_reverse_indexed_edge_keys boolean := false;
    has_direct_variable_vle_reverse_indexed_node_keys boolean := false;
    has_direct_variable_vle_reverse_indexed_node_id boolean := false;
    has_direct_vle_tail_reverse_slice_node_id boolean := false;
    has_direct_vle_reverse_tail_slice_edge_type boolean := false;
    has_direct_vle_tail_reverse_slice_node_props boolean := false;
    has_direct_vle_reverse_tail_slice_node_id boolean := false;
    has_direct_vle_tail_reverse_slice_edge_props boolean := false;
    has_direct_vle_reverse_tail_slice_node_keys boolean := false;
    has_direct_vle_tail_reverse_slice_node_size boolean := false;
    has_direct_vle_reverse_tail_slice_edge_empty boolean := false;
    has_direct_vle_tail_reverse_slice_start_id boolean := false;
    has_direct_vle_reverse_tail_slice_end_props boolean := false;
    has_direct_vle_tail_reverse_slice_start_prop boolean := false;
    has_direct_vle_reverse_tail_slice_end_keys boolean := false;
    has_direct_vle_tail_reverse_index_start_id boolean := false;
    has_direct_vle_reverse_tail_index_end_prop boolean := false;
    has_direct_vle_tail_reverse_index_start_keys boolean := false;
    has_direct_vle_reverse_tail_index_end_keys boolean := false;
    has_direct_vle_tail_reverse_index_start_labels boolean := false;
    has_direct_vle_reverse_tail_index_end_label boolean := false;
    has_direct_variable_vle_named_indexed_edge_keys boolean := false;
    has_direct_variable_vle_named_tail_indexed_edge_keys boolean := false;
    has_direct_variable_vle_named_tail_reverse_indexed_edge_keys boolean := false;
    has_direct_variable_vle_named_reverse_tail_indexed_edge_keys boolean := false;
    has_direct_variable_vle_named_double_tail_indexed_edge_keys boolean := false;
    has_direct_variable_vle_named_double_tail_edge_id boolean := false;
    has_direct_variable_vle_named_boundary_edge_keys boolean := false;
    has_direct_variable_vle_named_reverse_indexed_edge_keys boolean := false;
    has_direct_variable_vle_named_indexed_edge_properties boolean := false;
    has_direct_variable_vle_named_nested_edge_properties boolean := false;
    has_direct_variable_vle_named_double_tail_edge_properties boolean := false;
    has_direct_variable_vle_named_indexed_edge_type boolean := false;
    has_direct_variable_vle_named_double_tail_edge_type boolean := false;
    has_direct_variable_vle_named_indexed_edge_property_access boolean := false;
    has_direct_variable_vle_named_nested_edge_property_access boolean := false;
    has_direct_variable_vle_named_double_tail_edge_property_access boolean := false;
    has_direct_variable_vle_named_double_tail_start_id boolean := false;
    has_direct_variable_vle_named_double_tail_end_properties boolean := false;
    has_direct_variable_vle_named_double_tail_start_property_access boolean := false;
    has_direct_variable_vle_named_double_tail_start_projection boolean := false;
    has_direct_variable_vle_named_double_tail_end_projection boolean := false;
    has_direct_variable_vle_named_double_tail_start_keys boolean := false;
    has_direct_variable_vle_named_double_tail_end_keys boolean := false;
    has_direct_variable_vle_named_double_tail_start_labels boolean := false;
    has_direct_variable_vle_named_double_tail_end_label boolean := false;
    has_direct_variable_vle_named_double_tail_size boolean := false;
    has_direct_variable_vle_named_double_tail_empty boolean := false;
    has_direct_variable_vle_named_double_tail_slice boolean := false;
    has_direct_variable_vle_named_double_tail_head_id boolean := false;
    has_direct_variable_vle_named_double_tail_last_property_access boolean := false;
    has_direct_variable_vle_named_double_tail_head_payload boolean := false;
    has_direct_variable_vle_named_tail_reverse_last_id boolean := false;
    has_direct_variable_vle_named_reverse_tail_head_property_access boolean := false;
    has_direct_variable_vle_named_reverse_tail_start_labels boolean := false;
    has_direct_variable_vle_named_tail_reverse_size boolean := false;
    has_direct_variable_vle_named_reverse_tail_empty boolean := false;
    has_direct_variable_vle_named_tail_reverse_slice boolean := false;
    has_direct_variable_vle_named_tail_reverse_slice_size boolean := false;
    has_direct_variable_vle_named_reverse_tail_slice_empty boolean := false;
    has_direct_variable_vle_named_tail_reverse_slice_head_id boolean := false;
    has_direct_variable_vle_named_reverse_tail_slice_last_id boolean := false;
    has_direct_vle_named_tail_reverse_slice_head_prop boolean := false;
    has_direct_variable_vle_named_boundary_edge_property_access boolean := false;
    has_direct_variable_vle_named_tail_size boolean := false;
    has_direct_variable_vle_named_reverse_empty boolean := false;
    has_direct_variable_vle_named_head_payload boolean := false;
    has_direct_variable_vle_named_tail_access_payload boolean := false;
    has_direct_variable_vle_named_slice_payload boolean := false;
    has_direct_variable_vle_named_reverse_access_payload boolean := false;
    has_direct_variable_vle_named_nested_access_payload boolean := false;
    has_direct_variable_vle_named_slice_head_payload boolean := false;
    has_direct_variable_vle_named_slice_last_payload boolean := false;
    has_direct_variable_vle_named_slice_tail_head_payload boolean := false;
    has_direct_variable_vle_named_slice_tail_last_payload boolean := false;
    has_direct_variable_vle_named_slice_reverse_head_payload boolean := false;
    has_direct_variable_vle_named_slice_reverse_last_payload boolean := false;
    has_direct_variable_vle_named_slice_tail_properties_payload boolean := false;
    has_direct_variable_vle_named_slice_reverse_type_payload boolean := false;
    has_direct_variable_vle_named_slice_tail_keys_payload boolean := false;
    has_direct_variable_vle_named_slice_reverse_keys_payload boolean := false;
    has_direct_variable_vle_named_slice_tail_start_keys boolean := false;
    has_direct_variable_vle_named_slice_reverse_end_keys boolean := false;
    has_direct_variable_vle_named_slice_tail_start_id boolean := false;
    has_direct_variable_vle_named_slice_reverse_end_properties boolean := false;
    has_direct_variable_vle_named_slice_tail_start_property_access boolean := false;
    has_direct_variable_vle_named_slice_tail_start_projection boolean := false;
    has_direct_variable_vle_named_slice_reverse_end_projection boolean := false;
    has_direct_vle_indexed_node_keys boolean := false;
    has_direct_vle_indexed_edge_keys boolean := false;
    has_direct_vle_boundary_node_keys boolean := false;
    has_direct_vle_boundary_edge_keys boolean := false;
    has_direct_vle_boundary_node_id boolean := false;
    has_direct_vle_boundary_edge_id boolean := false;
    has_direct_vle_boundary_node_properties boolean := false;
    has_direct_vle_boundary_edge_type boolean := false;
    has_direct_vle_boundary_start_node_id boolean := false;
    has_direct_vle_boundary_end_node_properties boolean := false;
    has_direct_vle_boundary_start_node_projection boolean := false;
    has_direct_vle_boundary_start_node_keys boolean := false;
    has_direct_vle_boundary_end_node_keys boolean := false;
    has_id_bound_custom_scan boolean := true;
    has_id_bound_age_id boolean := false;
    has_vertex_property_prefilter boolean := false;
    endpoint_id graphid;
    end_endpoint_id graphid;
    edge_id graphid;
    returned_edges bigint;
    payload_text text;
BEGIN
    PERFORM create_graph(graph_name);
    EXECUTE format('SELECT create_vlabel(%L, %L)', graph_name, 'N');
    EXECUTE format('SELECT create_elabel(%L, %L)', graph_name, 'R');

    n_label_id := _label_id(graph_name, 'N');
    r_label_id := _label_id(graph_name, 'R');
    endpoint_id := _graphid(n_label_id, 0);
    end_endpoint_id := _graphid(n_label_id, 1);
    edge_id := _graphid(r_label_id, 1);

    EXECUTE format(
        'INSERT INTO %I."N"(id, properties)
         SELECT ag_catalog._graphid(%s, i::bigint),
                format(''{"i": %%s}'', i)::ag_catalog.agtype
         FROM generate_series(0, 127) AS g(i)',
        graph_name, n_label_id);

    EXECUTE format(
        'INSERT INTO %I."R"(id, start_id, end_id, properties)
         VALUES (ag_catalog._graphid(%s, 1),
                 ag_catalog._graphid(%s, 0),
                 ag_catalog._graphid(%s, 1),
                 ''{"kind": "forward"}''::ag_catalog.agtype),
                (ag_catalog._graphid(%s, 2),
                 ag_catalog._graphid(%s, 1),
                 ag_catalog._graphid(%s, 2),
                 ''{"kind": "chain"}''::ag_catalog.agtype)',
        graph_name, r_label_id, n_label_id, n_label_id,
        r_label_id, n_label_id, n_label_id);

    EXECUTE format('CREATE INDEX ON %I."R" USING age_adjacency ' ||
                   '(start_id, id, end_id)', graph_name);
    EXECUTE format('CREATE INDEX ON %I."R" USING age_adjacency ' ||
                   '(end_id, id, start_id)', graph_name);
    EXECUTE format('CREATE INDEX ON %I."N" USING gin (properties)',
                   graph_name);
    EXECUTE format('ANALYZE %I."N"', graph_name);
    EXECUTE format('ANALYZE %I."R"', graph_name);

    SET LOCAL age.enable_adjacency_match = off;
    SET LOCAL age.enable_adjacency_match_custom_path = off;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN MATCH (:N {i: 0})-[:R]->(n) RETURN n.i$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%Custom Scan (AGE Adjacency Match)%' THEN
            has_disabled_custom_scan := true;
        END IF;
        IF plan_text LIKE '%_agtype_build_vertex%' OR
           plan_text LIKE '%age_id(%' THEN
            has_join_build_vertex := true;
        END IF;
        IF plan_text LIKE '%properties @> ''{"i": 0}''::agtype%' THEN
            has_vertex_property_prefilter := true;
        END IF;
    END LOOP;

    IF has_disabled_custom_scan THEN
        RAISE EXCEPTION 'unexpected AGE Adjacency Match Custom Scan while disabled';
    END IF;
    IF has_join_build_vertex THEN
        RAISE EXCEPTION 'expected simple edge join quals to avoid vertex build';
    END IF;
    IF NOT has_vertex_property_prefilter THEN
        RAISE EXCEPTION 'expected ordinary vertex map literal to expose properties containment prefilter';
    END IF;

    SET LOCAL age.enable_adjacency_match_custom_path = on;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN MATCH (:N {i: 0})-[:R]->(n) RETURN n.i$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF first_cost_line IS NULL AND plan_text LIKE '%cost=%' THEN
            first_cost_line := plan_text;
        END IF;
        IF plan_text LIKE '%Custom Scan (AGE Adjacency Match)%' THEN
            has_custom_scan := true;
        END IF;
    END LOOP;

    IF NOT has_custom_scan THEN
        RAISE EXCEPTION 'expected AGE Adjacency Match Custom Scan';
    END IF;

    cost_match := regexp_match(first_cost_line, 'rows=([0-9]+)');
    IF cost_match IS NULL OR cost_match[1]::int <> 1 THEN
        RAISE EXCEPTION 'expected CustomPath top-level rows=1, got %',
                        first_cost_line;
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN MATCH (:N {i: 1})<-[:R]-(n) RETURN n.i$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%Custom Scan (AGE Adjacency Match)%' THEN
            has_left_direction_custom_scan := true;
        END IF;
    END LOOP;

    IF NOT has_left_direction_custom_scan THEN
        RAISE EXCEPTION 'expected AGE Adjacency Match Custom Scan for left direction shape';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN MATCH (:N {i: 0})-[:R]->(n:N) RETURN n.i$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%Custom Scan (AGE Adjacency Match)%' THEN
            has_right_label_custom_scan := true;
        END IF;
    END LOOP;

    IF NOT has_right_label_custom_scan THEN
        RAISE EXCEPTION 'expected AGE Adjacency Match Custom Scan for right label shape';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN MATCH (:N {i: 0})-[:R {kind: "keep"}]->(n) RETURN n.i$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%Custom Scan (AGE Adjacency Match)%' THEN
            has_excluded_custom_scan := true;
        END IF;
    END LOOP;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN MATCH (:N {i: 0})-[e:R]->(n) RETURN e$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%Custom Scan (AGE Adjacency Match)%' THEN
            has_excluded_custom_scan := true;
        END IF;
    END LOOP;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN MATCH (:N {i: 0})-[:R]->(n {i: 1}) RETURN n.i$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%Custom Scan (AGE Adjacency Match)%' THEN
            has_excluded_custom_scan := true;
        END IF;
    END LOOP;

    IF has_excluded_custom_scan THEN
        RAISE EXCEPTION 'unexpected AGE Adjacency Match Custom Scan for excluded shape';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN MATCH (n:N) WHERE id(n) = %s RETURN n$cypher$)
         AS (plan agtype)',
        graph_name, endpoint_id)
    LOOP
        IF (plan_text LIKE '%Index Cond: (id = %' OR
            plan_text LIKE '%Filter: (id = %') AND
           plan_text NOT LIKE '%_agtype_build_vertex(%' AND
           plan_text NOT LIKE '%age_id(%' THEN
            has_direct_id_qual := true;
        END IF;
    END LOOP;

    IF NOT has_direct_id_qual THEN
        RAISE EXCEPTION 'expected id(n) equality to lower to direct id qual';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH (n:N) WHERE keys(n) IS NOT NULL RETURN n.i$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_keys(n.properties)%' AND
           plan_text NOT LIKE '%_agtype_build_vertex(%' THEN
            has_direct_vertex_keys_qual := true;
        END IF;
    END LOOP;

    IF NOT has_direct_vertex_keys_qual THEN
        RAISE EXCEPTION 'expected keys(n) qual to lower through direct vertex properties';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH (n:N) WHERE labels(n) IS NOT NULL RETURN n.i$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%_label_name(%' AND
           plan_text LIKE '%agtype_build_list(%' AND
           plan_text NOT LIKE '%_agtype_build_vertex(%' THEN
            has_direct_vertex_labels_qual := true;
        END IF;
    END LOOP;

    IF NOT has_direct_vertex_labels_qual THEN
        RAISE EXCEPTION 'expected labels(n) qual to lower through direct vertex id';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH (s:N {i: 0}) MATCH (s)-[:R]->(n) WHERE keys(s) IS NOT NULL RETURN n.i$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_keys(%properties)%' AND
           plan_text NOT LIKE '%_agtype_build_vertex(%' THEN
            has_direct_previous_vertex_keys_qual := true;
        END IF;
    END LOOP;

    IF NOT has_direct_previous_vertex_keys_qual THEN
        RAISE EXCEPTION 'expected previous vertex keys(s) qual to lower through hidden raw properties';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH (s:N {i: 0}) MATCH (s)-[:R]->(n) WHERE labels(s) IS NOT NULL RETURN n.i$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%_label_name(%' AND
           plan_text LIKE '%agtype_build_list(%' AND
           plan_text NOT LIKE '%_agtype_build_vertex(%' THEN
            has_direct_previous_vertex_labels_qual := true;
        END IF;
    END LOOP;

    IF NOT has_direct_previous_vertex_labels_qual THEN
        RAISE EXCEPTION 'expected previous vertex labels(s) qual to lower through hidden raw id';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH (:N {i: 0})-[*1..2]->(n) RETURN n.i$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%_agtype_build_path%' THEN
            has_pathless_vle_build_path := true;
        END IF;
        IF plan_text NOT LIKE '%_agtype_build_path%' THEN
            has_pathless_left_vle_no_build_path := true;
        END IF;
        IF plan_text LIKE '%age_vle_terminal_id(%' AND
           plan_text NOT LIKE '%age_match_vle_terminal_edge%' THEN
            has_pathless_right_vle_terminal_id_join := true;
        END IF;
        IF plan_text LIKE '%Custom Scan (AGE VLE Stream)%' THEN
            has_pathless_right_vle_terminal_id_join := true;
        END IF;
        IF plan_text LIKE '%agtype_access_operator%' AND
           plan_text LIKE '%n.properties%' AND
           plan_text NOT LIKE '%_agtype_build_vertex(%' THEN
            has_pathless_right_vle_direct_property_projection := true;
        END IF;
        IF plan_text LIKE '%Custom Scan (AGE VLE Stream)%' THEN
            has_pathless_right_vle_direct_property_projection := true;
        END IF;
    END LOOP;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH (s)-[*1..2]->(:N {i: 2}) RETURN s.i$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%_agtype_build_path%' THEN
            has_pathless_vle_build_path := true;
        END IF;
        IF plan_text NOT LIKE '%_agtype_build_path%' THEN
            has_pathless_right_vle_no_build_path := true;
        END IF;
    END LOOP;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH (:N {i: 1})-[*1..1]-(n) RETURN n.i$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%_agtype_build_path%' THEN
            has_pathless_vle_build_path := true;
        END IF;
        IF plan_text NOT LIKE '%_agtype_build_path%' THEN
            has_pathless_undirected_vle_no_build_path := true;
        END IF;
    END LOOP;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH (:N {i: 0})-[e*1..2]->(n) RETURN n.i$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%_agtype_build_path%' THEN
            has_pathless_vle_build_path := true;
        END IF;
        IF plan_text NOT LIKE '%_agtype_build_path%' THEN
            has_pathless_named_edge_vle_no_build_path := true;
        END IF;
    END LOOP;

    IF has_pathless_vle_build_path OR
       NOT has_pathless_left_vle_no_build_path OR
       NOT has_pathless_right_vle_no_build_path OR
       NOT has_pathless_undirected_vle_no_build_path OR
       NOT has_pathless_named_edge_vle_no_build_path THEN
        RAISE EXCEPTION 'unexpected _agtype_build_path in pathless variable-length VLE plans';
    END IF;

    IF NOT has_pathless_right_vle_terminal_id_join THEN
        RAISE EXCEPTION 'expected pathless right-directed VLE endpoint join to use terminal id helper';
    END IF;

    IF NOT has_pathless_right_vle_direct_property_projection THEN
        RAISE EXCEPTION 'expected pathless right-directed VLE endpoint property projection to use raw properties';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(:N {i: 0})-[*1..2]->(n) WHERE length(p) > 0 RETURN n.i$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_vle_path_length(%' AND
           plan_text NOT LIKE '%_agtype_build_path%' THEN
            has_direct_variable_vle_length := true;
        END IF;
    END LOOP;

    IF NOT has_direct_variable_vle_length THEN
        RAISE EXCEPTION 'expected WHERE length(p) for variable VLE to use path length helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(:N {i: 0})-[*1..2]->(n) RETURN length(p)$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_vle_path_length(%' AND
           plan_text NOT LIKE '%_agtype_build_path%' THEN
            has_direct_variable_vle_length_projection := true;
        END IF;
    END LOOP;

    IF NOT has_direct_variable_vle_length_projection THEN
        RAISE EXCEPTION 'expected RETURN length(p) for variable VLE to use path length helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(:N {i: 0})-[*1..2]->(n) WHERE size(nodes(p)) > 0 RETURN n.i$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_vle_path_node_count(%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_vertex(%' THEN
            has_direct_variable_vle_node_count := true;
        END IF;
    END LOOP;

    IF NOT has_direct_variable_vle_node_count THEN
        RAISE EXCEPTION 'expected WHERE size(nodes(p)) for variable VLE to use node count helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(:N {i: 0})-[*1..2]->(n) RETURN size(nodes(p))$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_vle_path_node_count(%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_vertex(%' THEN
            has_direct_variable_vle_node_count_projection := true;
        END IF;
    END LOOP;

    IF NOT has_direct_variable_vle_node_count_projection THEN
        RAISE EXCEPTION 'expected RETURN size(nodes(p)) for variable VLE to use node count helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(:N {i: 0})-[*1..2]->(n) WHERE size(relationships(p)) > 0 RETURN n.i$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_vle_path_length(%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' THEN
            has_direct_variable_vle_edge_count := true;
        END IF;
    END LOOP;

    IF NOT has_direct_variable_vle_edge_count THEN
        RAISE EXCEPTION 'expected WHERE size(relationships(p)) for variable VLE to use path length helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(:N {i: 0})-[*1..2]->(n) RETURN size(relationships(p))$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_vle_path_length(%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' THEN
            has_direct_variable_vle_edge_count_projection := true;
        END IF;
    END LOOP;

    IF NOT has_direct_variable_vle_edge_count_projection THEN
        RAISE EXCEPTION 'expected RETURN size(relationships(p)) for variable VLE to use path length helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(:N {i: 0})-[*1..2]->(n) RETURN size(relationships(p)[1..])$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_vle_list_slice_count(%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' THEN
            has_direct_variable_vle_edge_slice_count := true;
        END IF;
    END LOOP;

    IF NOT has_direct_variable_vle_edge_slice_count THEN
        RAISE EXCEPTION 'expected RETURN size(relationships(p)[1..]) for variable VLE to use slice count helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(:N {i: 0})-[*1..2]->(n) RETURN isEmpty(nodes(p)[3..])$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_vle_list_slice_is_empty(%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_vertex(%' THEN
            has_direct_variable_vle_node_slice_empty := true;
        END IF;
    END LOOP;

    IF NOT has_direct_variable_vle_node_slice_empty THEN
        RAISE EXCEPTION 'expected RETURN isEmpty(nodes(p)[3..]) for variable VLE to use slice empty helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(:N {i: 0})-[*1..2]->(n) WHERE id(head(relationships(p))) IS NOT NULL RETURN n.i$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_vle_edge_id_at(%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%age_id(%' THEN
            has_direct_variable_vle_head_edge_id_filter := true;
        END IF;
    END LOOP;

    IF NOT has_direct_variable_vle_head_edge_id_filter THEN
        RAISE EXCEPTION 'expected WHERE id(head(relationships(p))) for variable VLE to use indexed edge id helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(:N {i: 0})-[*1..2]->(n) WHERE relationships(p)[0].kind IS NOT NULL RETURN n.i$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_vle_edge_properties_at(%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' THEN
            has_direct_variable_vle_indexed_edge_property_filter := true;
        END IF;
    END LOOP;

    IF NOT has_direct_variable_vle_indexed_edge_property_filter THEN
        RAISE EXCEPTION 'expected WHERE relationships(p)[0].kind for variable VLE to use indexed edge properties helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(:N {i: 0})-[*1..2]->(n) WHERE nodes(p)[0].i IS NOT NULL RETURN n.i$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_vle_node_properties_at(%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_vertex(%' THEN
            has_direct_variable_vle_indexed_node_property_filter := true;
        END IF;
    END LOOP;

    IF NOT has_direct_variable_vle_indexed_node_property_filter THEN
        RAISE EXCEPTION 'expected WHERE nodes(p)[0].i for variable VLE to use indexed node properties helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(:N {i: 0})-[*1..2]->(n) WHERE relationships(p)[0] IN relationships(p) RETURN n.i$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_vle_edge_index_exists(%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' THEN
            has_direct_variable_vle_edge_membership := true;
        END IF;
    END LOOP;

    IF NOT has_direct_variable_vle_edge_membership THEN
        RAISE EXCEPTION 'expected relationships(p)[0] IN relationships(p) for variable VLE to use edge index exists helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(:N {i: 0})-[*1..2]->(n) WHERE relationships(p)[0] = head(relationships(p)) RETURN n.i$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_vle_edge_indices_equal(%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' THEN
            has_direct_variable_vle_edge_equality := true;
        END IF;
    END LOOP;

    IF NOT has_direct_variable_vle_edge_equality THEN
        RAISE EXCEPTION 'expected relationships(p)[0] = head(relationships(p)) for variable VLE to use edge index equality helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(:N {i: 0})-[*1..2]->(n) WHERE reverse(relationships(p))[0] = last(relationships(p)) RETURN n.i$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_vle_edge_reversed_index_equal(%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' THEN
            has_direct_variable_vle_reverse_edge_equality := true;
        END IF;
        IF plan_text LIKE '%age_materialize_vle_edge_at(%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%age_materialize_vle_edges(%' THEN
            has_direct_variable_vle_reverse_edge_equality := true;
        END IF;
        IF plan_text LIKE '%age_vle_edge_indices_equal(%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' THEN
            has_direct_variable_vle_reverse_edge_equality := true;
        END IF;
    END LOOP;

    IF NOT has_direct_variable_vle_reverse_edge_equality THEN
        RAISE EXCEPTION 'expected reverse(relationships(p))[0] = last(relationships(p)) for variable VLE to use reversed edge equality helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(:N {i: 0})-[*1..2]->(n) WHERE tail(reverse(relationships(p)))[0] = relationships(p)[0] RETURN n.i$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_vle_edge_reversed_index_equal(%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%age_tail(%' AND
           plan_text NOT LIKE '%age_reverse(%' THEN
            has_direct_variable_vle_tail_reverse_edge_equality := true;
        END IF;
        IF plan_text LIKE '%age_materialize_vle_edge_at(%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%age_materialize_vle_edges(%' AND
           plan_text NOT LIKE '%age_tail(%' AND
           plan_text NOT LIKE '%age_reverse(%' THEN
            has_direct_variable_vle_tail_reverse_edge_equality := true;
        END IF;
        IF plan_text LIKE '%age_vle_edge_indices_equal(%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%age_tail(%' AND
           plan_text NOT LIKE '%age_reverse(%' THEN
            has_direct_variable_vle_tail_reverse_edge_equality := true;
        END IF;
    END LOOP;

    IF NOT has_direct_variable_vle_tail_reverse_edge_equality THEN
        RAISE EXCEPTION 'expected tail(reverse(relationships(p)))[0] = relationships(p)[0] for variable VLE to use reversed edge equality helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(:N {i: 0})-[*1..1]->(n) WHERE keys(nodes(p)[0]) IS NOT NULL RETURN n.i$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_keys(age_vle_node_properties_at(%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_vertex(%' THEN
            has_direct_variable_vle_indexed_node_keys := true;
        END IF;
    END LOOP;

    IF NOT has_direct_variable_vle_indexed_node_keys THEN
        RAISE EXCEPTION 'expected WHERE keys(nodes(p)[0]) for variable VLE to use node properties helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(:N {i: 0})-[*1..1]->(n) RETURN keys(nodes(p)[0])$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_keys(age_vle_node_properties_at(%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_vertex(%' THEN
            has_direct_variable_vle_indexed_node_keys_projection := true;
        END IF;
    END LOOP;

    IF NOT has_direct_variable_vle_indexed_node_keys_projection THEN
        RAISE EXCEPTION 'expected RETURN keys(nodes(p)[0]) for variable VLE to use node properties helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(:N {i: 0})-[*1..1]->(n) WHERE keys(relationships(p)[0]) IS NOT NULL RETURN n.i$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_keys(age_vle_edge_properties_at(%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' THEN
            has_direct_variable_vle_indexed_edge_keys := true;
        END IF;
    END LOOP;

    IF NOT has_direct_variable_vle_indexed_edge_keys THEN
        RAISE EXCEPTION 'expected WHERE keys(relationships(p)[0]) for variable VLE to use edge properties helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(:N {i: 0})-[*1..1]->(n) RETURN keys(relationships(p)[0])$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_keys(age_vle_edge_properties_at(%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' THEN
            has_direct_variable_vle_indexed_edge_keys_projection := true;
        END IF;
    END LOOP;

    IF NOT has_direct_variable_vle_indexed_edge_keys_projection THEN
        RAISE EXCEPTION 'expected RETURN keys(relationships(p)[0]) for variable VLE to use edge properties helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(:N {i: 0})-[*1..1]->(n) WHERE keys(head(nodes(p))) IS NOT NULL RETURN n.i$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_keys(age_vle_node_properties_at(%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_vertex(%' THEN
            has_direct_variable_vle_boundary_node_keys := true;
        END IF;
    END LOOP;

    IF NOT has_direct_variable_vle_boundary_node_keys THEN
        RAISE EXCEPTION 'expected WHERE keys(head(nodes(p))) for variable VLE to use node properties helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(:N {i: 0})-[*1..1]->(n) RETURN keys(head(nodes(p)))$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_keys(age_vle_node_properties_at(%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_vertex(%' THEN
            has_direct_variable_vle_boundary_node_keys_projection := true;
        END IF;
    END LOOP;

    IF NOT has_direct_variable_vle_boundary_node_keys_projection THEN
        RAISE EXCEPTION 'expected RETURN keys(head(nodes(p))) for variable VLE to use node properties helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(:N {i: 0})-[*1..1]->(n) WHERE keys(last(relationships(p))) IS NOT NULL RETURN n.i$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_keys(age_vle_edge_properties_at(%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' THEN
            has_direct_variable_vle_boundary_edge_keys := true;
        END IF;
    END LOOP;

    IF NOT has_direct_variable_vle_boundary_edge_keys THEN
        RAISE EXCEPTION 'expected WHERE keys(last(relationships(p))) for variable VLE to use edge properties helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(:N {i: 0})-[*1..1]->(n) RETURN keys(last(relationships(p)))$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_keys(age_vle_edge_properties_at(%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' THEN
            has_direct_variable_vle_boundary_edge_keys_projection := true;
        END IF;
    END LOOP;

    IF NOT has_direct_variable_vle_boundary_edge_keys_projection THEN
        RAISE EXCEPTION 'expected RETURN keys(last(relationships(p))) for variable VLE to use edge properties helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(:N {i: 0})-[*1..1]->(n) WHERE properties(head(nodes(p))) IS NOT NULL RETURN n.i$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_vle_node_properties_at(%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_vertex(%' AND
           plan_text NOT LIKE '%age_properties(%' THEN
            has_direct_variable_vle_boundary_node_properties := true;
        END IF;
    END LOOP;

    IF NOT has_direct_variable_vle_boundary_node_properties THEN
        RAISE EXCEPTION 'expected WHERE properties(head(nodes(p))) for variable VLE to use node properties helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(:N {i: 0})-[*1..1]->(n) RETURN properties(head(nodes(p)))$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_vle_node_properties_at(%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_vertex(%' AND
           plan_text NOT LIKE '%age_properties(%' THEN
            has_direct_variable_vle_boundary_node_properties_projection := true;
        END IF;
    END LOOP;

    IF NOT has_direct_variable_vle_boundary_node_properties_projection THEN
        RAISE EXCEPTION 'expected RETURN properties(head(nodes(p))) for variable VLE to use node properties helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(:N {i: 0})-[*1..1]->(n) WHERE type(last(relationships(p))) IS NOT NULL RETURN n.i$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_vle_edge_label_at(%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%age_type(%' THEN
            has_direct_variable_vle_boundary_edge_type := true;
        END IF;
    END LOOP;

    IF NOT has_direct_variable_vle_boundary_edge_type THEN
        RAISE EXCEPTION 'expected WHERE type(last(relationships(p))) for variable VLE to use edge label helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(:N {i: 0})-[*1..1]->(n) RETURN type(last(relationships(p)))$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_vle_edge_label_at(%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%age_type(%' THEN
            has_direct_variable_vle_boundary_edge_type_projection := true;
        END IF;
    END LOOP;

    IF NOT has_direct_variable_vle_boundary_edge_type_projection THEN
        RAISE EXCEPTION 'expected RETURN type(last(relationships(p))) for variable VLE to use edge label helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(:N {i: 0})-[*1..1]->(n) WHERE properties(tail(nodes(p))[0]) IS NOT NULL RETURN n.i$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_vle_node_properties_at(%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_vertex(%' AND
           plan_text NOT LIKE '%age_properties(%' THEN
            has_direct_variable_vle_tail_indexed_node_properties := true;
        END IF;
    END LOOP;

    IF NOT has_direct_variable_vle_tail_indexed_node_properties THEN
        RAISE EXCEPTION 'expected WHERE properties(tail(nodes(p))[0]) for variable VLE to use node properties helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(:N {i: 0})-[*1..1]->(n) WHERE keys(tail(nodes(p))[0]) IS NOT NULL RETURN n.i$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_keys(age_vle_node_properties_at(%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_vertex(%' THEN
            has_direct_variable_vle_tail_indexed_node_keys := true;
        END IF;
    END LOOP;

    IF NOT has_direct_variable_vle_tail_indexed_node_keys THEN
        RAISE EXCEPTION 'expected WHERE keys(tail(nodes(p))[0]) for variable VLE to use node properties helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(:N {i: 0})-[*1..1]->(n) WHERE type(reverse(relationships(p))[0]) IS NOT NULL RETURN n.i$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_vle_edge_label_at(%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%age_type(%' THEN
            has_direct_variable_vle_reverse_indexed_edge_type := true;
        END IF;
    END LOOP;

    IF NOT has_direct_variable_vle_reverse_indexed_edge_type THEN
        RAISE EXCEPTION 'expected WHERE type(reverse(relationships(p))[0]) for variable VLE to use edge label helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(:N {i: 0})-[*1..1]->(n) WHERE keys(reverse(relationships(p))[0]) IS NOT NULL RETURN n.i$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_keys(age_vle_edge_properties_at(%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' THEN
            has_direct_variable_vle_reverse_indexed_edge_keys := true;
        END IF;
    END LOOP;

    IF NOT has_direct_variable_vle_reverse_indexed_edge_keys THEN
        RAISE EXCEPTION 'expected WHERE keys(reverse(relationships(p))[0]) for variable VLE to use edge properties helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(:N {i: 0})-[*1..1]->(n) WHERE keys(reverse(nodes(p))[0]) IS NOT NULL RETURN n.i$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_keys(age_vle_node_properties_at(%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_vertex(%' THEN
            has_direct_variable_vle_reverse_indexed_node_keys := true;
        END IF;
    END LOOP;

    IF NOT has_direct_variable_vle_reverse_indexed_node_keys THEN
        RAISE EXCEPTION 'expected WHERE keys(reverse(nodes(p))[0]) for variable VLE to use node properties helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(:N {i: 0})-[*1..1]->(n) WHERE id(reverse(nodes(p))[0]) IS NOT NULL RETURN n.i$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_vle_node_id_at(%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_vertex(%' AND
           plan_text NOT LIKE '%age_id(%' THEN
            has_direct_variable_vle_reverse_indexed_node_id := true;
        END IF;
    END LOOP;

    IF NOT has_direct_variable_vle_reverse_indexed_node_id THEN
        RAISE EXCEPTION 'expected WHERE id(reverse(nodes(p))[0]) for variable VLE to use node id helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(:N {i: 0})-[*1..1]->(n) WHERE id(head(tail(reverse(nodes(p)))[0..1])) IS NOT NULL RETURN n.i$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_materialize_vle_slice_boundary(%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_vertex(%' AND
           plan_text NOT LIKE '%age_id(%' AND
           plan_text NOT LIKE '%age_tail(%' AND
           plan_text NOT LIKE '%age_reverse(%' THEN
            has_direct_vle_tail_reverse_slice_node_id := true;
        END IF;
    END LOOP;

    IF NOT has_direct_vle_tail_reverse_slice_node_id THEN
        RAISE EXCEPTION 'expected WHERE id(head(tail(reverse(nodes(p)))[0..1])) for variable VLE to use direct slice boundary node id helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(:N {i: 0})-[*1..1]->(n) WHERE type(last(reverse(tail(relationships(p)))[0..1])) IS NOT NULL RETURN n.i$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_materialize_vle_slice_boundary(%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%age_type(%' AND
           plan_text NOT LIKE '%age_tail(%' AND
           plan_text NOT LIKE '%age_reverse(%' THEN
            has_direct_vle_reverse_tail_slice_edge_type := true;
        END IF;
    END LOOP;

    IF NOT has_direct_vle_reverse_tail_slice_edge_type THEN
        RAISE EXCEPTION 'expected WHERE type(last(reverse(tail(relationships(p)))[0..1])) for variable VLE to use direct slice boundary edge label helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(:N {i: 0})-[*1..1]->(n) WHERE properties(head(tail(reverse(nodes(p)))[0..1])) IS NOT NULL RETURN n.i$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_materialize_vle_slice_boundary(%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_vertex(%' AND
           plan_text NOT LIKE '%age_properties(%' AND
           plan_text NOT LIKE '%age_tail(%' AND
           plan_text NOT LIKE '%age_reverse(%' THEN
            has_direct_vle_tail_reverse_slice_node_props := true;
        END IF;
    END LOOP;

    IF NOT has_direct_vle_tail_reverse_slice_node_props THEN
        RAISE EXCEPTION 'expected WHERE properties(head(tail(reverse(nodes(p)))[0..1])) for variable VLE to use direct slice boundary node properties helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(:N {i: 0})-[*1..1]->(n) WHERE id(last(reverse(tail(nodes(p)))[0..1])) IS NOT NULL RETURN n.i$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_materialize_vle_slice_boundary(%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_vertex(%' AND
           plan_text NOT LIKE '%age_id(%' AND
           plan_text NOT LIKE '%age_tail(%' AND
           plan_text NOT LIKE '%age_reverse(%' THEN
            has_direct_vle_reverse_tail_slice_node_id := true;
        END IF;
    END LOOP;

    IF NOT has_direct_vle_reverse_tail_slice_node_id THEN
        RAISE EXCEPTION 'expected WHERE id(last(reverse(tail(nodes(p)))[0..1])) for variable VLE to use direct slice boundary node id helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(:N {i: 0})-[*1..1]->(n) WHERE properties(head(tail(reverse(relationships(p)))[0..1])) IS NOT NULL RETURN n.i$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_materialize_vle_slice_boundary(%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%age_properties(%' AND
           plan_text NOT LIKE '%age_tail(%' AND
           plan_text NOT LIKE '%age_reverse(%' THEN
            has_direct_vle_tail_reverse_slice_edge_props := true;
        END IF;
    END LOOP;

    IF NOT has_direct_vle_tail_reverse_slice_edge_props THEN
        RAISE EXCEPTION 'expected WHERE properties(head(tail(reverse(relationships(p)))[0..1])) for variable VLE to use direct slice boundary edge properties helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(:N {i: 0})-[*1..1]->(n) WHERE keys(last(reverse(tail(nodes(p)))[0..1])) IS NOT NULL RETURN n.i$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_keys(age_materialize_vle_slice_boundary(%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_vertex(%' AND
           plan_text NOT LIKE '%age_properties(%' AND
           plan_text NOT LIKE '%age_tail(%' AND
           plan_text NOT LIKE '%age_reverse(%' THEN
            has_direct_vle_reverse_tail_slice_node_keys := true;
        END IF;
    END LOOP;

    IF NOT has_direct_vle_reverse_tail_slice_node_keys THEN
        RAISE EXCEPTION 'expected WHERE keys(last(reverse(tail(nodes(p)))[0..1])) for variable VLE to use direct slice boundary node keys helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(:N {i: 0})-[*1..2]->(n) WHERE size(tail(reverse(nodes(p)))[0..1]) >= 0 RETURN n.i$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_vle_list_slice_count(%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_vertex(%' AND
           plan_text NOT LIKE '%age_size(%' AND
           plan_text NOT LIKE '%age_tail(%' AND
           plan_text NOT LIKE '%age_reverse(%' THEN
            has_direct_vle_tail_reverse_slice_node_size := true;
        END IF;
    END LOOP;

    IF NOT has_direct_vle_tail_reverse_slice_node_size THEN
        RAISE EXCEPTION 'expected WHERE size(tail(reverse(nodes(p)))[0..1]) for variable VLE to use direct slice count helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(:N {i: 0})-[*1..2]->(n) WHERE NOT isEmpty(reverse(tail(relationships(p)))[0..1]) RETURN n.i$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_vle_list_slice_is_empty(%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%age_tail(%' AND
           plan_text NOT LIKE '%age_reverse(%' THEN
            has_direct_vle_reverse_tail_slice_edge_empty := true;
        END IF;
    END LOOP;

    IF NOT has_direct_vle_reverse_tail_slice_edge_empty THEN
        RAISE EXCEPTION 'expected WHERE isEmpty(reverse(tail(relationships(p)))[0..1]) for variable VLE to use direct slice empty helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(:N {i: 0})-[*1..1]->(n) WHERE id(startNode(head(tail(reverse(relationships(p)))[0..1]))) IS NOT NULL RETURN n.i$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_materialize_vle_slice_boundary(%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%age_id(%' AND
           plan_text NOT LIKE '%age_startnode(%' AND
           plan_text NOT LIKE '%age_tail(%' AND
           plan_text NOT LIKE '%age_reverse(%' THEN
            has_direct_vle_tail_reverse_slice_start_id := true;
        END IF;
    END LOOP;

    IF NOT has_direct_vle_tail_reverse_slice_start_id THEN
        RAISE EXCEPTION 'expected WHERE id(startNode(head(tail(reverse(relationships(p)))[0..1]))) for variable VLE to use direct endpoint id helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(:N {i: 0})-[*1..1]->(n) WHERE properties(endNode(last(reverse(tail(relationships(p)))[0..1]))) IS NOT NULL RETURN n.i$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_materialize_vle_slice_boundary(%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%age_properties(%' AND
           plan_text NOT LIKE '%age_endnode(%' AND
           plan_text NOT LIKE '%age_tail(%' AND
           plan_text NOT LIKE '%age_reverse(%' THEN
            has_direct_vle_reverse_tail_slice_end_props := true;
        END IF;
    END LOOP;

    IF NOT has_direct_vle_reverse_tail_slice_end_props THEN
        RAISE EXCEPTION 'expected WHERE properties(endNode(last(reverse(tail(relationships(p)))[0..1]))) for variable VLE to use direct endpoint properties helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(:N {i: 0})-[*1..1]->(n) WHERE startNode(head(tail(reverse(relationships(p)))[0..1])).missing IS NULL RETURN n.i$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_materialize_vle_slice_boundary(%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%age_startnode(%' AND
           plan_text NOT LIKE '%age_tail(%' AND
           plan_text NOT LIKE '%age_reverse(%' THEN
            has_direct_vle_tail_reverse_slice_start_prop := true;
        END IF;
    END LOOP;

    IF NOT has_direct_vle_tail_reverse_slice_start_prop THEN
        RAISE EXCEPTION 'expected WHERE startNode(head(tail(reverse(relationships(p)))[0..1])).missing for variable VLE to use direct endpoint projection helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(:N {i: 0})-[*1..1]->(n) WHERE keys(endNode(last(reverse(tail(relationships(p)))[0..1]))) IS NOT NULL RETURN n.i$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_keys(age_materialize_vle_slice_boundary(%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%age_properties(%' AND
           plan_text NOT LIKE '%age_endnode(%' AND
           plan_text NOT LIKE '%age_tail(%' AND
           plan_text NOT LIKE '%age_reverse(%' THEN
            has_direct_vle_reverse_tail_slice_end_keys := true;
        END IF;
    END LOOP;

    IF NOT has_direct_vle_reverse_tail_slice_end_keys THEN
        RAISE EXCEPTION 'expected WHERE keys(endNode(last(reverse(tail(relationships(p)))[0..1]))) for variable VLE to use direct endpoint keys helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(:N {i: 0})-[*1..1]->(n) WHERE id(startNode(tail(reverse(relationships(p)))[0])) IS NOT NULL RETURN n.i$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_materialize_vle_slice_boundary(%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%age_id(%' AND
           plan_text NOT LIKE '%age_startnode(%' AND
           plan_text NOT LIKE '%age_tail(%' AND
           plan_text NOT LIKE '%age_reverse(%' THEN
            has_direct_vle_tail_reverse_index_start_id := true;
        END IF;
    END LOOP;

    IF NOT has_direct_vle_tail_reverse_index_start_id THEN
        RAISE EXCEPTION 'expected WHERE id(startNode(tail(reverse(relationships(p)))[0])) for variable VLE to use direct nested endpoint id helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(:N {i: 0})-[*1..1]->(n) WHERE endNode(reverse(tail(relationships(p)))[0]).missing IS NULL RETURN n.i$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_materialize_vle_slice_boundary(%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%age_endnode(%' AND
           plan_text NOT LIKE '%age_tail(%' AND
           plan_text NOT LIKE '%age_reverse(%' THEN
            has_direct_vle_reverse_tail_index_end_prop := true;
        END IF;
    END LOOP;

    IF NOT has_direct_vle_reverse_tail_index_end_prop THEN
        RAISE EXCEPTION 'expected WHERE endNode(reverse(tail(relationships(p)))[0]).missing for variable VLE to use direct nested endpoint projection helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(:N {i: 0})-[*1..1]->(n) WHERE keys(startNode(tail(reverse(relationships(p)))[0])) IS NOT NULL RETURN n.i$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_keys(age_materialize_vle_slice_boundary(%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%age_properties(%' AND
           plan_text NOT LIKE '%age_startnode(%' AND
           plan_text NOT LIKE '%age_tail(%' AND
           plan_text NOT LIKE '%age_reverse(%' THEN
            has_direct_vle_tail_reverse_index_start_keys := true;
        END IF;
    END LOOP;

    IF NOT has_direct_vle_tail_reverse_index_start_keys THEN
        RAISE EXCEPTION 'expected WHERE keys(startNode(tail(reverse(relationships(p)))[0])) for variable VLE to use direct nested endpoint keys helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(:N {i: 0})-[*1..1]->(n) WHERE keys(endNode(reverse(tail(relationships(p)))[0])) IS NOT NULL RETURN n.i$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_keys(age_materialize_vle_slice_boundary(%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%age_properties(%' AND
           plan_text NOT LIKE '%age_endnode(%' AND
           plan_text NOT LIKE '%age_tail(%' AND
           plan_text NOT LIKE '%age_reverse(%' THEN
            has_direct_vle_reverse_tail_index_end_keys := true;
        END IF;
    END LOOP;

    IF NOT has_direct_vle_reverse_tail_index_end_keys THEN
        RAISE EXCEPTION 'expected WHERE keys(endNode(reverse(tail(relationships(p)))[0])) for variable VLE to use direct nested endpoint keys helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(:N {i: 0})-[*1..1]->(n) WHERE labels(startNode(tail(reverse(relationships(p)))[0])) IS NOT NULL RETURN n.i$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_materialize_vle_slice_boundary(%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%age_labels(%' AND
           plan_text NOT LIKE '%age_startnode(%' AND
           plan_text NOT LIKE '%age_tail(%' AND
           plan_text NOT LIKE '%age_reverse(%' THEN
            has_direct_vle_tail_reverse_index_start_labels := true;
        END IF;
    END LOOP;

    IF NOT has_direct_vle_tail_reverse_index_start_labels THEN
        RAISE EXCEPTION 'expected WHERE labels(startNode(tail(reverse(relationships(p)))[0])) for variable VLE to use direct nested endpoint labels helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(:N {i: 0})-[*1..1]->(n) WHERE label(endNode(reverse(tail(relationships(p)))[0])) IS NOT NULL RETURN n.i$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_materialize_vle_slice_boundary(%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%age_label(%' AND
           plan_text NOT LIKE '%age_endnode(%' AND
           plan_text NOT LIKE '%age_tail(%' AND
           plan_text NOT LIKE '%age_reverse(%' THEN
            has_direct_vle_reverse_tail_index_end_label := true;
        END IF;
    END LOOP;

    IF NOT has_direct_vle_reverse_tail_index_end_label THEN
        RAISE EXCEPTION 'expected WHERE label(endNode(reverse(tail(relationships(p)))[0])) for variable VLE to use direct nested endpoint label helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH (:N {i: 0})-[e*1..2]->(n) WHERE keys(e[0]) IS NOT NULL RETURN n.i$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_keys(age_vle_edge_properties_at(%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' THEN
            has_direct_variable_vle_named_indexed_edge_keys := true;
        END IF;
    END LOOP;

    IF NOT has_direct_variable_vle_named_indexed_edge_keys THEN
        RAISE EXCEPTION 'expected WHERE keys(e[0]) for variable VLE edge-list to use edge properties helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH (:N {i: 0})-[e*1..2]->(n) WHERE keys(tail(e)[0]) IS NOT NULL RETURN n.i$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_keys(age_vle_edge_properties_at(%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' THEN
            has_direct_variable_vle_named_tail_indexed_edge_keys := true;
        END IF;
    END LOOP;

    IF NOT has_direct_variable_vle_named_tail_indexed_edge_keys THEN
        RAISE EXCEPTION 'expected WHERE keys(tail(e)[0]) for variable VLE edge-list to use edge properties helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH (:N {i: 0})-[e*1..2]->(n) WHERE keys(tail(reverse(e))[0]) IS NOT NULL RETURN n.i$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_keys(%' AND
           (plan_text LIKE '%age_materialize_vle_slice_boundary(%' OR
            plan_text LIKE '%age_vle_edge_properties_at(%') AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%age_properties(%' THEN
            has_direct_variable_vle_named_tail_reverse_indexed_edge_keys := true;
        END IF;
    END LOOP;

    IF NOT has_direct_variable_vle_named_tail_reverse_indexed_edge_keys THEN
        RAISE EXCEPTION 'expected WHERE keys(tail(reverse(e))[0]) for variable VLE edge-list to use direct properties helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH (:N {i: 0})-[e*1..2]->(n) WHERE keys(reverse(tail(e))[0]) IS NOT NULL RETURN n.i$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_keys(%' AND
           (plan_text LIKE '%age_materialize_vle_slice_boundary(%' OR
            plan_text LIKE '%age_vle_edge_properties_at(%') AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%age_properties(%' THEN
            has_direct_variable_vle_named_reverse_tail_indexed_edge_keys := true;
        END IF;
    END LOOP;

    IF NOT has_direct_variable_vle_named_reverse_tail_indexed_edge_keys THEN
        RAISE EXCEPTION 'expected WHERE keys(reverse(tail(e))[0]) for variable VLE edge-list to use direct properties helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH (:N {i: 0})-[e*1..2]->(n) WHERE keys(tail(tail(e))[0]) IS NOT NULL RETURN n.i$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_keys(%' AND
           (plan_text LIKE '%age_materialize_vle_slice_boundary(%' OR
            plan_text LIKE '%age_vle_edge_properties_at(%') AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%age_properties(%' THEN
            has_direct_variable_vle_named_double_tail_indexed_edge_keys := true;
        END IF;
    END LOOP;

    IF NOT has_direct_variable_vle_named_double_tail_indexed_edge_keys THEN
        RAISE EXCEPTION 'expected WHERE keys(tail(tail(e))[0]) for variable VLE edge-list to use direct properties helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH (:N {i: 0})-[e*1..2]->(n) WHERE id(tail(tail(e))[0]) IS NOT NULL RETURN n.i$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF (plan_text LIKE '%age_materialize_vle_slice_boundary(%' OR
            plan_text LIKE '%age_vle_edge_id_at(%') AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%age_id(%' THEN
            has_direct_variable_vle_named_double_tail_edge_id := true;
        END IF;
    END LOOP;

    IF NOT has_direct_variable_vle_named_double_tail_edge_id THEN
        RAISE EXCEPTION 'expected WHERE id(tail(tail(e))[0]) for variable VLE edge-list to use direct id helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH (:N {i: 0})-[e*1..2]->(n) WHERE keys(reverse(e)[0]) IS NOT NULL RETURN n.i$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_keys(age_vle_edge_properties_at(%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' THEN
            has_direct_variable_vle_named_reverse_indexed_edge_keys := true;
        END IF;
    END LOOP;

    IF NOT has_direct_variable_vle_named_reverse_indexed_edge_keys THEN
        RAISE EXCEPTION 'expected WHERE keys(reverse(e)[0]) for variable VLE edge-list to use edge properties helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH (:N {i: 0})-[e*1..2]->(n) WHERE keys(last(e)) IS NOT NULL RETURN n.i$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_keys(age_vle_edge_properties_at(%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' THEN
            has_direct_variable_vle_named_boundary_edge_keys := true;
        END IF;
    END LOOP;

    IF NOT has_direct_variable_vle_named_boundary_edge_keys THEN
        RAISE EXCEPTION 'expected WHERE keys(last(e)) for variable VLE edge-list to use edge properties helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH (:N {i: 0})-[e*1..2]->(n) WHERE properties(e[0]) IS NOT NULL RETURN n.i$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_vle_edge_properties_at(%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' THEN
            has_direct_variable_vle_named_indexed_edge_properties := true;
        END IF;
    END LOOP;

    IF NOT has_direct_variable_vle_named_indexed_edge_properties THEN
        RAISE EXCEPTION 'expected WHERE properties(e[0]) for variable VLE edge-list to use edge properties helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH (:N {i: 0})-[e*1..2]->(n) WHERE properties(reverse(tail(e))[0]) IS NOT NULL RETURN n.i$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_materialize_vle_slice_boundary(%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%age_properties(%' THEN
            has_direct_variable_vle_named_nested_edge_properties := true;
        END IF;
    END LOOP;

    IF NOT has_direct_variable_vle_named_nested_edge_properties THEN
        RAISE EXCEPTION 'expected WHERE properties(reverse(tail(e))[0]) for variable VLE edge-list to use direct properties helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH (:N {i: 0})-[e*1..2]->(n) WHERE properties(tail(tail(e))[0]) IS NOT NULL RETURN n.i$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_materialize_vle_slice_boundary(%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%age_properties(%' THEN
            has_direct_variable_vle_named_double_tail_edge_properties := true;
        END IF;
    END LOOP;

    IF NOT has_direct_variable_vle_named_double_tail_edge_properties THEN
        RAISE EXCEPTION 'expected WHERE properties(tail(tail(e))[0]) for variable VLE edge-list to use direct properties helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH (:N {i: 0})-[e*1..2]->(n) WHERE type(e[0]) IS NOT NULL RETURN n.i$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_vle_edge_label_at(%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' THEN
            has_direct_variable_vle_named_indexed_edge_type := true;
        END IF;
    END LOOP;

    IF NOT has_direct_variable_vle_named_indexed_edge_type THEN
        RAISE EXCEPTION 'expected WHERE type(e[0]) for variable VLE edge-list to use edge label helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH (:N {i: 0})-[e*1..2]->(n) WHERE type(tail(tail(e))[0]) IS NOT NULL RETURN n.i$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_materialize_vle_slice_boundary(%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%age_type(%' THEN
            has_direct_variable_vle_named_double_tail_edge_type := true;
        END IF;
    END LOOP;

    IF NOT has_direct_variable_vle_named_double_tail_edge_type THEN
        RAISE EXCEPTION 'expected WHERE type(tail(tail(e))[0]) for variable VLE edge-list to use direct label helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH (:N {i: 0})-[e*1..2]->(n) WHERE e[0].missing IS NULL RETURN n.i$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_vle_edge_properties_at(%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' THEN
            has_direct_variable_vle_named_indexed_edge_property_access := true;
        END IF;
    END LOOP;

    IF NOT has_direct_variable_vle_named_indexed_edge_property_access THEN
        RAISE EXCEPTION 'expected WHERE e[0].missing for variable VLE edge-list to use edge properties helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH (:N {i: 0})-[e*1..2]->(n) WHERE tail(reverse(e))[0].missing IS NULL RETURN n.i$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_materialize_vle_slice_boundary(%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%age_properties(%' THEN
            has_direct_variable_vle_named_nested_edge_property_access := true;
        END IF;
    END LOOP;

    IF NOT has_direct_variable_vle_named_nested_edge_property_access THEN
        RAISE EXCEPTION 'expected WHERE tail(reverse(e))[0].missing for variable VLE edge-list to use direct properties helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH (:N {i: 0})-[e*1..2]->(n) WHERE tail(tail(e))[0].missing IS NULL RETURN n.i$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_materialize_vle_slice_boundary(%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%age_properties(%' THEN
            has_direct_variable_vle_named_double_tail_edge_property_access := true;
        END IF;
    END LOOP;

    IF NOT has_direct_variable_vle_named_double_tail_edge_property_access THEN
        RAISE EXCEPTION 'expected WHERE tail(tail(e))[0].missing for variable VLE edge-list to use direct properties helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH (:N {i: 0})-[e*1..2]->(n) WHERE id(startNode(tail(tail(e))[0])) IS NOT NULL RETURN n.i$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_materialize_vle_slice_boundary(%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%age_id(%' AND
           plan_text NOT LIKE '%age_startnode(%' THEN
            has_direct_variable_vle_named_double_tail_start_id := true;
        END IF;
    END LOOP;

    IF NOT has_direct_variable_vle_named_double_tail_start_id THEN
        RAISE EXCEPTION 'expected WHERE id(startNode(tail(tail(e))[0])) for variable VLE edge-list to use direct endpoint id helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH (:N {i: 0})-[e*1..2]->(n) WHERE properties(endNode(tail(tail(e))[0])) IS NOT NULL RETURN n.i$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_materialize_vle_slice_boundary(%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%age_properties(%' AND
           plan_text NOT LIKE '%age_endnode(%' THEN
            has_direct_variable_vle_named_double_tail_end_properties := true;
        END IF;
    END LOOP;

    IF NOT has_direct_variable_vle_named_double_tail_end_properties THEN
        RAISE EXCEPTION 'expected WHERE properties(endNode(tail(tail(e))[0])) for variable VLE edge-list to use direct endpoint properties helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH (:N {i: 0})-[e*1..2]->(n) WHERE startNode(tail(tail(e))[0]).missing IS NULL RETURN n.i$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_materialize_vle_slice_boundary(%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%age_properties(%' AND
           plan_text NOT LIKE '%age_startnode(%' THEN
            has_direct_variable_vle_named_double_tail_start_property_access := true;
        END IF;
    END LOOP;

    IF NOT has_direct_variable_vle_named_double_tail_start_property_access THEN
        RAISE EXCEPTION 'expected WHERE startNode(tail(tail(e))[0]).missing for variable VLE edge-list to use direct endpoint properties helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH (:N {i: 0})-[e*1..2]->(n) RETURN startNode(tail(tail(e))[0])$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_materialize_vle_slice_boundary(%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%age_startnode(%' THEN
            has_direct_variable_vle_named_double_tail_start_projection := true;
        END IF;
    END LOOP;

    IF NOT has_direct_variable_vle_named_double_tail_start_projection THEN
        RAISE EXCEPTION 'expected RETURN startNode(tail(tail(e))[0]) for variable VLE edge-list to use direct endpoint projection helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH (:N {i: 0})-[e*1..2]->(n) RETURN endNode(tail(tail(e))[0])$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_materialize_vle_slice_boundary(%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%age_endnode(%' THEN
            has_direct_variable_vle_named_double_tail_end_projection := true;
        END IF;
    END LOOP;

    IF NOT has_direct_variable_vle_named_double_tail_end_projection THEN
        RAISE EXCEPTION 'expected RETURN endNode(tail(tail(e))[0]) for variable VLE edge-list to use direct endpoint projection helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH (:N {i: 0})-[e*1..2]->(n) RETURN keys(startNode(tail(tail(e))[0]))$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_keys(age_materialize_vle_slice_boundary(%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%age_properties(%' AND
           plan_text NOT LIKE '%age_startnode(%' THEN
            has_direct_variable_vle_named_double_tail_start_keys := true;
        END IF;
    END LOOP;

    IF NOT has_direct_variable_vle_named_double_tail_start_keys THEN
        RAISE EXCEPTION 'expected RETURN keys(startNode(tail(tail(e))[0])) for variable VLE edge-list to use direct endpoint properties helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH (:N {i: 0})-[e*1..2]->(n) RETURN keys(endNode(tail(tail(e))[0]))$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_keys(age_materialize_vle_slice_boundary(%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%age_properties(%' AND
           plan_text NOT LIKE '%age_endnode(%' THEN
            has_direct_variable_vle_named_double_tail_end_keys := true;
        END IF;
    END LOOP;

    IF NOT has_direct_variable_vle_named_double_tail_end_keys THEN
        RAISE EXCEPTION 'expected RETURN keys(endNode(tail(tail(e))[0])) for variable VLE edge-list to use direct endpoint properties helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH (:N {i: 0})-[e*1..2]->(n) RETURN labels(startNode(tail(tail(e))[0]))$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_materialize_vle_slice_boundary(%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%age_labels(%' AND
           plan_text NOT LIKE '%age_startnode(%' THEN
            has_direct_variable_vle_named_double_tail_start_labels := true;
        END IF;
    END LOOP;

    IF NOT has_direct_variable_vle_named_double_tail_start_labels THEN
        RAISE EXCEPTION 'expected RETURN labels(startNode(tail(tail(e))[0])) for variable VLE edge-list to use direct endpoint labels helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH (:N {i: 0})-[e*1..2]->(n) RETURN label(endNode(tail(tail(e))[0]))$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_materialize_vle_slice_boundary(%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%age_label(%' AND
           plan_text NOT LIKE '%age_endnode(%' THEN
            has_direct_variable_vle_named_double_tail_end_label := true;
        END IF;
    END LOOP;

    IF NOT has_direct_variable_vle_named_double_tail_end_label THEN
        RAISE EXCEPTION 'expected RETURN label(endNode(tail(tail(e))[0])) for variable VLE edge-list to use direct endpoint label helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH (:N {i: 0})-[e*1..2]->(n) WHERE size(tail(tail(e))) >= 0 RETURN n.i$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_vle_list_slice_count(%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' THEN
            has_direct_variable_vle_named_double_tail_size := true;
        END IF;
    END LOOP;

    IF NOT has_direct_variable_vle_named_double_tail_size THEN
        RAISE EXCEPTION 'expected WHERE size(tail(tail(e))) for variable VLE edge-list to use slice count helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH (:N {i: 0})-[e*1..2]->(n) WHERE NOT isEmpty(tail(tail(e))) RETURN n.i$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_vle_list_slice_is_empty(%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' THEN
            has_direct_variable_vle_named_double_tail_empty := true;
        END IF;
    END LOOP;

    IF NOT has_direct_variable_vle_named_double_tail_empty THEN
        RAISE EXCEPTION 'expected WHERE isEmpty(tail(tail(e))) for variable VLE edge-list to use slice empty helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH (:N {i: 0})-[e*1..2]->(n) RETURN tail(tail(e))[0..1]$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_materialize_vle_list_slice(%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' THEN
            has_direct_variable_vle_named_double_tail_slice := true;
        END IF;
    END LOOP;

    IF NOT has_direct_variable_vle_named_double_tail_slice THEN
        RAISE EXCEPTION 'expected RETURN tail(tail(e))[0..1] for variable VLE edge-list to use direct slice helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH (:N {i: 0})-[e*1..2]->(n) RETURN id(head(tail(tail(e))))$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_materialize_vle_slice_boundary(%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%age_id(%' THEN
            has_direct_variable_vle_named_double_tail_head_id := true;
        END IF;
    END LOOP;

    IF NOT has_direct_variable_vle_named_double_tail_head_id THEN
        RAISE EXCEPTION 'expected RETURN id(head(tail(tail(e)))) for variable VLE edge-list to use direct boundary id helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH (:N {i: 0})-[e*1..2]->(n) RETURN last(tail(tail(e))).missing$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_materialize_vle_slice_boundary(%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%age_properties(%' THEN
            has_direct_variable_vle_named_double_tail_last_property_access := true;
        END IF;
    END LOOP;

    IF NOT has_direct_variable_vle_named_double_tail_last_property_access THEN
        RAISE EXCEPTION 'expected RETURN last(tail(tail(e))).missing for variable VLE edge-list to use direct properties helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH (:N {i: 0})-[e*1..2]->(n) RETURN head(tail(tail(e)))$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_materialize_vle_slice_boundary(%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' THEN
            has_direct_variable_vle_named_double_tail_head_payload := true;
        END IF;
    END LOOP;

    IF NOT has_direct_variable_vle_named_double_tail_head_payload THEN
        RAISE EXCEPTION 'expected RETURN head(tail(tail(e))) for variable VLE edge-list to use direct boundary helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH (:N {i: 0})-[e*1..2]->(n) RETURN id(last(tail(reverse(e))))$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_materialize_vle_slice_boundary(%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%age_id(%' THEN
            has_direct_variable_vle_named_tail_reverse_last_id := true;
        END IF;
    END LOOP;

    IF NOT has_direct_variable_vle_named_tail_reverse_last_id THEN
        RAISE EXCEPTION 'expected RETURN id(last(tail(reverse(e)))) for variable VLE edge-list to use direct boundary id helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH (:N {i: 0})-[e*1..2]->(n) RETURN head(reverse(tail(e))).missing$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_materialize_vle_slice_boundary(%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%age_properties(%' THEN
            has_direct_variable_vle_named_reverse_tail_head_property_access := true;
        END IF;
    END LOOP;

    IF NOT has_direct_variable_vle_named_reverse_tail_head_property_access THEN
        RAISE EXCEPTION 'expected RETURN head(reverse(tail(e))).missing for variable VLE edge-list to use direct properties helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH (:N {i: 0})-[e*1..2]->(n) RETURN labels(startNode(head(reverse(tail(e)))))$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_materialize_vle_slice_boundary(%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%age_labels(%' AND
           plan_text NOT LIKE '%age_startnode(%' THEN
            has_direct_variable_vle_named_reverse_tail_start_labels := true;
        END IF;
    END LOOP;

    IF NOT has_direct_variable_vle_named_reverse_tail_start_labels THEN
        RAISE EXCEPTION 'expected RETURN labels(startNode(head(reverse(tail(e))))) for variable VLE edge-list to use direct endpoint labels helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH (:N {i: 0})-[e*1..2]->(n) WHERE size(tail(reverse(e))) >= 0 RETURN n.i$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_vle_edge_tail_count(%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%age_tail(%' AND
           plan_text NOT LIKE '%age_reverse(%' THEN
            has_direct_variable_vle_named_tail_reverse_size := true;
        END IF;
    END LOOP;

    IF NOT has_direct_variable_vle_named_tail_reverse_size THEN
        RAISE EXCEPTION 'expected WHERE size(tail(reverse(e))) for variable VLE edge-list to use direct count helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH (:N {i: 0})-[e*1..2]->(n) WHERE NOT isEmpty(reverse(tail(e))) RETURN n.i$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_vle_list_is_empty(%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%age_tail(%' AND
           plan_text NOT LIKE '%age_reverse(%' THEN
            has_direct_variable_vle_named_reverse_tail_empty := true;
        END IF;
    END LOOP;

    IF NOT has_direct_variable_vle_named_reverse_tail_empty THEN
        RAISE EXCEPTION 'expected WHERE isEmpty(reverse(tail(e))) for variable VLE edge-list to use direct empty helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH (:N {i: 0})-[e*1..2]->(n) RETURN tail(reverse(e))[0..1]$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_materialize_vle_list_slice(%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%age_tail(%' AND
           plan_text NOT LIKE '%age_reverse(%' THEN
            has_direct_variable_vle_named_tail_reverse_slice := true;
        END IF;
    END LOOP;

    IF NOT has_direct_variable_vle_named_tail_reverse_slice THEN
        RAISE EXCEPTION 'expected RETURN tail(reverse(e))[0..1] for variable VLE edge-list to use direct slice helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH (:N {i: 0})-[e*1..2]->(n) WHERE size(tail(reverse(e))[0..1]) >= 0 RETURN n.i$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_vle_list_slice_count(%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%age_tail(%' AND
           plan_text NOT LIKE '%age_reverse(%' THEN
            has_direct_variable_vle_named_tail_reverse_slice_size := true;
        END IF;
    END LOOP;

    IF NOT has_direct_variable_vle_named_tail_reverse_slice_size THEN
        RAISE EXCEPTION 'expected WHERE size(tail(reverse(e))[0..1]) for variable VLE edge-list to use direct slice count helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH (:N {i: 0})-[e*1..2]->(n) WHERE NOT isEmpty(reverse(tail(e))[0..1]) RETURN n.i$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_vle_list_slice_is_empty(%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%age_tail(%' AND
           plan_text NOT LIKE '%age_reverse(%' THEN
            has_direct_variable_vle_named_reverse_tail_slice_empty := true;
        END IF;
    END LOOP;

    IF NOT has_direct_variable_vle_named_reverse_tail_slice_empty THEN
        RAISE EXCEPTION 'expected WHERE isEmpty(reverse(tail(e))[0..1]) for variable VLE edge-list to use direct slice empty helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH (:N {i: 0})-[e*1..2]->(n) RETURN id(head(tail(reverse(e))[0..1]))$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_materialize_vle_slice_boundary(%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%age_id(%' AND
           plan_text NOT LIKE '%age_tail(%' AND
           plan_text NOT LIKE '%age_reverse(%' THEN
            has_direct_variable_vle_named_tail_reverse_slice_head_id := true;
        END IF;
    END LOOP;

    IF NOT has_direct_variable_vle_named_tail_reverse_slice_head_id THEN
        RAISE EXCEPTION 'expected RETURN id(head(tail(reverse(e))[0..1])) for variable VLE edge-list to use direct slice boundary id helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH (:N {i: 0})-[e*1..2]->(n) RETURN id(last(reverse(tail(e))[0..1]))$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_materialize_vle_slice_boundary(%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%age_id(%' AND
           plan_text NOT LIKE '%age_tail(%' AND
           plan_text NOT LIKE '%age_reverse(%' THEN
            has_direct_variable_vle_named_reverse_tail_slice_last_id := true;
        END IF;
    END LOOP;

    IF NOT has_direct_variable_vle_named_reverse_tail_slice_last_id THEN
        RAISE EXCEPTION 'expected RETURN id(last(reverse(tail(e))[0..1])) for variable VLE edge-list to use direct slice boundary id helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH (:N {i: 0})-[e*1..2]->(n) RETURN head(tail(reverse(e))[0..1]).missing$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_materialize_vle_slice_boundary(%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%age_properties(%' AND
           plan_text NOT LIKE '%age_tail(%' AND
           plan_text NOT LIKE '%age_reverse(%' THEN
            has_direct_vle_named_tail_reverse_slice_head_prop := true;
        END IF;
    END LOOP;

    IF NOT has_direct_vle_named_tail_reverse_slice_head_prop THEN
        RAISE EXCEPTION 'expected RETURN head(tail(reverse(e))[0..1]).missing for variable VLE edge-list to use direct slice boundary properties helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH (:N {i: 0})-[e*1..2]->(n) WHERE last(e).missing IS NULL RETURN n.i$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF (plan_text LIKE '%age_vle_edge_properties_at(%' OR
            plan_text LIKE '%age_vle_edge_property_at(%') AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' THEN
            has_direct_variable_vle_named_boundary_edge_property_access := true;
        END IF;
    END LOOP;

    IF NOT has_direct_variable_vle_named_boundary_edge_property_access THEN
        RAISE EXCEPTION 'expected WHERE last(e).missing for variable VLE edge-list to use edge properties helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH (:N {i: 0})-[e*1..2]->(n) WHERE size(tail(e)) > 0 RETURN n.i$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_vle_edge_tail_count(%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' THEN
            has_direct_variable_vle_named_tail_size := true;
        END IF;
    END LOOP;

    IF NOT has_direct_variable_vle_named_tail_size THEN
        RAISE EXCEPTION 'expected WHERE size(tail(e)) for variable VLE edge-list to use tail count helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH (:N {i: 0})-[e*1..2]->(n) WHERE NOT isEmpty(reverse(e)) RETURN n.i$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_vle_list_is_empty(%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' THEN
            has_direct_variable_vle_named_reverse_empty := true;
        END IF;
    END LOOP;

    IF NOT has_direct_variable_vle_named_reverse_empty THEN
        RAISE EXCEPTION 'expected WHERE isEmpty(reverse(e)) for variable VLE edge-list to use list empty helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH (:N {i: 0})-[e*1..2]->(n) RETURN head(e)$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_materialize_vle_edge_at(%' AND
           plan_text NOT LIKE '%age_head(%' AND
           plan_text NOT LIKE '%age_materialize_vle_edges(%' THEN
            has_direct_variable_vle_named_head_payload := true;
        END IF;
    END LOOP;

    IF NOT has_direct_variable_vle_named_head_payload THEN
        RAISE EXCEPTION 'expected RETURN head(e) for variable VLE edge-list to use edge-at helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH (:N {i: 0})-[e*1..2]->(n) RETURN tail(e)[0]$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_materialize_vle_edge_at(%' AND
           plan_text NOT LIKE '%age_tail(%' AND
           plan_text NOT LIKE '%age_materialize_vle_edges(%' THEN
            has_direct_variable_vle_named_tail_access_payload := true;
        END IF;
    END LOOP;

    IF NOT has_direct_variable_vle_named_tail_access_payload THEN
        RAISE EXCEPTION 'expected RETURN tail(e)[0] for variable VLE edge-list to use edge-at helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH (:N {i: 0})-[e*1..2]->(n) RETURN e[0..1]$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_materialize_vle_list_slice(%' AND
           plan_text NOT LIKE '%age_materialize_vle_edges(%' THEN
            has_direct_variable_vle_named_slice_payload := true;
        END IF;
    END LOOP;

    IF NOT has_direct_variable_vle_named_slice_payload THEN
        RAISE EXCEPTION 'expected RETURN e[0..1] for variable VLE edge-list to use list slice helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH (:N {i: 0})-[e*1..2]->(n) RETURN reverse(e)[0]$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF (plan_text LIKE '%age_materialize_vle_edge_at(%' OR
            plan_text LIKE '%age_materialize_vle_edge_reversed_at(%') AND
           plan_text NOT LIKE '%age_reverse(%' AND
           plan_text NOT LIKE '%age_materialize_vle_edges(%' THEN
            has_direct_variable_vle_named_reverse_access_payload := true;
        END IF;
    END LOOP;

    IF NOT has_direct_variable_vle_named_reverse_access_payload THEN
        RAISE EXCEPTION 'expected RETURN reverse(e)[0] for variable VLE edge-list to use direct edge helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH (:N {i: 0})-[e*1..2]->(n) RETURN tail(reverse(e))[0]$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_materialize_vle_edge_at(%' AND
           plan_text NOT LIKE '%age_tail(%' AND
           plan_text NOT LIKE '%age_reverse(%' AND
           plan_text NOT LIKE '%age_materialize_vle_edges(%' THEN
            has_direct_variable_vle_named_nested_access_payload := true;
        END IF;
    END LOOP;

    IF NOT has_direct_variable_vle_named_nested_access_payload THEN
        RAISE EXCEPTION 'expected RETURN tail(reverse(e))[0] for variable VLE edge-list to use edge-at helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH (:N {i: 0})-[e*1..2]->(n) RETURN head(e[0..2])$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_materialize_vle_slice_boundary(%' AND
           plan_text NOT LIKE '%age_head(%' AND
           plan_text NOT LIKE '%age_materialize_vle_edges(%' THEN
            has_direct_variable_vle_named_slice_head_payload := true;
        END IF;
    END LOOP;

    IF NOT has_direct_variable_vle_named_slice_head_payload THEN
        RAISE EXCEPTION 'expected RETURN head(e[0..2]) for variable VLE edge-list to use slice-boundary helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH (:N {i: 0})-[e*1..2]->(n) RETURN last(e[0..2])$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_materialize_vle_slice_boundary(%' AND
           plan_text NOT LIKE '%age_last(%' AND
           plan_text NOT LIKE '%age_materialize_vle_edges(%' THEN
            has_direct_variable_vle_named_slice_last_payload := true;
        END IF;
    END LOOP;

    IF NOT has_direct_variable_vle_named_slice_last_payload THEN
        RAISE EXCEPTION 'expected RETURN last(e[0..2]) for variable VLE edge-list to use slice-boundary helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH (:N {i: 0})-[e*1..2]->(n) RETURN head(tail(e[0..2]))$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_materialize_vle_slice_boundary(%' AND
           plan_text NOT LIKE '%age_head(%' AND
           plan_text NOT LIKE '%age_tail(%' AND
           plan_text NOT LIKE '%age_materialize_vle_edges(%' THEN
            has_direct_variable_vle_named_slice_tail_head_payload := true;
        END IF;
    END LOOP;

    IF NOT has_direct_variable_vle_named_slice_tail_head_payload THEN
        RAISE EXCEPTION 'expected RETURN head(tail(e[0..2])) for variable VLE edge-list to use slice-boundary helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH (:N {i: 0})-[e*1..2]->(n) RETURN last(tail(e[0..2]))$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_materialize_vle_slice_boundary(%' AND
           plan_text NOT LIKE '%age_last(%' AND
           plan_text NOT LIKE '%age_tail(%' AND
           plan_text NOT LIKE '%age_materialize_vle_edges(%' THEN
            has_direct_variable_vle_named_slice_tail_last_payload := true;
        END IF;
    END LOOP;

    IF NOT has_direct_variable_vle_named_slice_tail_last_payload THEN
        RAISE EXCEPTION 'expected RETURN last(tail(e[0..2])) for variable VLE edge-list to use slice-boundary helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH (:N {i: 0})-[e*1..2]->(n) RETURN head(reverse(e[0..2]))$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_materialize_vle_slice_boundary(%' AND
           plan_text NOT LIKE '%age_head(%' AND
           plan_text NOT LIKE '%age_reverse(%' AND
           plan_text NOT LIKE '%age_materialize_vle_edges(%' THEN
            has_direct_variable_vle_named_slice_reverse_head_payload := true;
        END IF;
    END LOOP;

    IF NOT has_direct_variable_vle_named_slice_reverse_head_payload THEN
        RAISE EXCEPTION 'expected RETURN head(reverse(e[0..2])) for variable VLE edge-list to use slice-boundary helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH (:N {i: 0})-[e*1..2]->(n) RETURN last(reverse(e[0..2]))$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_materialize_vle_slice_boundary(%' AND
           plan_text NOT LIKE '%age_last(%' AND
           plan_text NOT LIKE '%age_reverse(%' AND
           plan_text NOT LIKE '%age_materialize_vle_edges(%' THEN
            has_direct_variable_vle_named_slice_reverse_last_payload := true;
        END IF;
    END LOOP;

    IF NOT has_direct_variable_vle_named_slice_reverse_last_payload THEN
        RAISE EXCEPTION 'expected RETURN last(reverse(e[0..2])) for variable VLE edge-list to use slice-boundary helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH (:N {i: 0})-[e*1..2]->(n) RETURN properties(head(tail(e[0..2])))$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_materialize_vle_slice_boundary(%' AND
           plan_text NOT LIKE '%age_properties(%' AND
           plan_text NOT LIKE '%age_head(%' AND
           plan_text NOT LIKE '%age_tail(%' AND
           plan_text NOT LIKE '%age_materialize_vle_edges(%' THEN
            has_direct_variable_vle_named_slice_tail_properties_payload := true;
        END IF;
    END LOOP;

    IF NOT has_direct_variable_vle_named_slice_tail_properties_payload THEN
        RAISE EXCEPTION 'expected RETURN properties(head(tail(e[0..2]))) for variable VLE edge-list to use slice-boundary properties helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH (:N {i: 0})-[e*1..2]->(n) RETURN type(head(reverse(e[0..2])))$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_materialize_vle_slice_boundary(%' AND
           plan_text NOT LIKE '%age_type(%' AND
           plan_text NOT LIKE '%age_head(%' AND
           plan_text NOT LIKE '%age_reverse(%' AND
           plan_text NOT LIKE '%age_materialize_vle_edges(%' THEN
            has_direct_variable_vle_named_slice_reverse_type_payload := true;
        END IF;
    END LOOP;

    IF NOT has_direct_variable_vle_named_slice_reverse_type_payload THEN
        RAISE EXCEPTION 'expected RETURN type(head(reverse(e[0..2]))) for variable VLE edge-list to use slice-boundary type helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH (:N {i: 0})-[e*1..2]->(n) RETURN keys(head(tail(e[0..2])))$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_keys(age_materialize_vle_slice_boundary(%' AND
           plan_text NOT LIKE '%age_properties(%' AND
           plan_text NOT LIKE '%age_head(%' AND
           plan_text NOT LIKE '%age_tail(%' AND
           plan_text NOT LIKE '%age_materialize_vle_edges(%' THEN
            has_direct_variable_vle_named_slice_tail_keys_payload := true;
        END IF;
    END LOOP;

    IF NOT has_direct_variable_vle_named_slice_tail_keys_payload THEN
        RAISE EXCEPTION 'expected RETURN keys(head(tail(e[0..2]))) for variable VLE edge-list to use slice-boundary properties helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH (:N {i: 0})-[e*1..2]->(n) RETURN keys(last(reverse(e[0..2])))$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_keys(age_materialize_vle_slice_boundary(%' AND
           plan_text NOT LIKE '%age_properties(%' AND
           plan_text NOT LIKE '%age_last(%' AND
           plan_text NOT LIKE '%age_reverse(%' AND
           plan_text NOT LIKE '%age_materialize_vle_edges(%' THEN
            has_direct_variable_vle_named_slice_reverse_keys_payload := true;
        END IF;
    END LOOP;

    IF NOT has_direct_variable_vle_named_slice_reverse_keys_payload THEN
        RAISE EXCEPTION 'expected RETURN keys(last(reverse(e[0..2]))) for variable VLE edge-list to use slice-boundary properties helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH (:N {i: 0})-[e*1..2]->(n) RETURN keys(startNode(head(tail(e[0..2]))))$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_keys(age_materialize_vle_slice_boundary(%' AND
           plan_text NOT LIKE '%age_startnode(%' AND
           plan_text NOT LIKE '%age_head(%' AND
           plan_text NOT LIKE '%age_tail(%' AND
           plan_text NOT LIKE '%age_materialize_vle_edges(%' THEN
            has_direct_variable_vle_named_slice_tail_start_keys := true;
        END IF;
    END LOOP;

    IF NOT has_direct_variable_vle_named_slice_tail_start_keys THEN
        RAISE EXCEPTION 'expected RETURN keys(startNode(head(tail(e[0..2])))) for variable VLE edge-list to use endpoint properties helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH (:N {i: 0})-[e*1..2]->(n) RETURN keys(endNode(last(reverse(e[0..2]))))$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_keys(age_materialize_vle_slice_boundary(%' AND
           plan_text NOT LIKE '%age_endnode(%' AND
           plan_text NOT LIKE '%age_last(%' AND
           plan_text NOT LIKE '%age_reverse(%' AND
           plan_text NOT LIKE '%age_materialize_vle_edges(%' THEN
            has_direct_variable_vle_named_slice_reverse_end_keys := true;
        END IF;
    END LOOP;

    IF NOT has_direct_variable_vle_named_slice_reverse_end_keys THEN
        RAISE EXCEPTION 'expected RETURN keys(endNode(last(reverse(e[0..2])))) for variable VLE edge-list to use endpoint properties helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH (:N {i: 0})-[e*1..2]->(n) RETURN id(startNode(head(tail(e[0..2]))))$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_materialize_vle_slice_boundary(%' AND
           plan_text NOT LIKE '%age_id(%' AND
           plan_text NOT LIKE '%age_startnode(%' AND
           plan_text NOT LIKE '%age_head(%' AND
           plan_text NOT LIKE '%age_tail(%' AND
           plan_text NOT LIKE '%age_materialize_vle_edges(%' THEN
            has_direct_variable_vle_named_slice_tail_start_id := true;
        END IF;
    END LOOP;

    IF NOT has_direct_variable_vle_named_slice_tail_start_id THEN
        RAISE EXCEPTION 'expected RETURN id(startNode(head(tail(e[0..2])))) for variable VLE edge-list to use endpoint id helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH (:N {i: 0})-[e*1..2]->(n) RETURN properties(endNode(last(reverse(e[0..2]))))$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_materialize_vle_slice_boundary(%' AND
           plan_text NOT LIKE '%age_properties(%' AND
           plan_text NOT LIKE '%age_endnode(%' AND
           plan_text NOT LIKE '%age_last(%' AND
           plan_text NOT LIKE '%age_reverse(%' AND
           plan_text NOT LIKE '%age_materialize_vle_edges(%' THEN
            has_direct_variable_vle_named_slice_reverse_end_properties := true;
        END IF;
    END LOOP;

    IF NOT has_direct_variable_vle_named_slice_reverse_end_properties THEN
        RAISE EXCEPTION 'expected RETURN properties(endNode(last(reverse(e[0..2])))) for variable VLE edge-list to use endpoint properties helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH (:N {i: 0})-[e*1..2]->(n) RETURN startNode(head(tail(e[0..2]))).i$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_materialize_vle_slice_boundary(%' AND
           plan_text NOT LIKE '%age_startnode(%' AND
           plan_text NOT LIKE '%age_head(%' AND
           plan_text NOT LIKE '%age_tail(%' AND
           plan_text NOT LIKE '%age_materialize_vle_edges(%' THEN
            has_direct_variable_vle_named_slice_tail_start_property_access :=
                true;
        END IF;
    END LOOP;

    IF NOT has_direct_variable_vle_named_slice_tail_start_property_access THEN
        RAISE EXCEPTION 'expected RETURN startNode(head(tail(e[0..2]))).i for variable VLE edge-list to use endpoint properties helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH (:N {i: 0})-[e*1..2]->(n) RETURN startNode(head(tail(e[0..2])))$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_materialize_vle_slice_boundary(%' AND
           plan_text NOT LIKE '%age_startnode(%' AND
           plan_text NOT LIKE '%age_head(%' AND
           plan_text NOT LIKE '%age_tail(%' AND
           plan_text NOT LIKE '%age_materialize_vle_edges(%' THEN
            has_direct_variable_vle_named_slice_tail_start_projection := true;
        END IF;
    END LOOP;

    IF NOT has_direct_variable_vle_named_slice_tail_start_projection THEN
        RAISE EXCEPTION 'expected RETURN startNode(head(tail(e[0..2]))) for variable VLE edge-list to use endpoint projection helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH (:N {i: 0})-[e*1..2]->(n) RETURN endNode(last(reverse(e[0..2])))$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_materialize_vle_slice_boundary(%' AND
           plan_text NOT LIKE '%age_endnode(%' AND
           plan_text NOT LIKE '%age_last(%' AND
           plan_text NOT LIKE '%age_reverse(%' AND
           plan_text NOT LIKE '%age_materialize_vle_edges(%' THEN
            has_direct_variable_vle_named_slice_reverse_end_projection := true;
        END IF;
    END LOOP;

    IF NOT has_direct_variable_vle_named_slice_reverse_end_projection THEN
        RAISE EXCEPTION 'expected RETURN endNode(last(reverse(e[0..2]))) for variable VLE edge-list to use endpoint projection helper';
    END IF;

    EXECUTE format(
        'SELECT h::text || '':'' || t::text || '':'' || r::text || '':'' ||
                tr::text || '':'' || sh::text || '':'' || sl::text || '':'' ||
                sth::text || '':'' || stl::text || '':'' || srh::text || '':'' ||
                srl::text
         FROM cypher(%L,
                     $cypher$MATCH (:N {i: 0})-[e*2..2]->()
                              RETURN head(e).kind, tail(e)[0].kind,
                                     reverse(e)[0].kind,
                                     tail(reverse(e))[0].kind,
                                     head(e[0..2]).kind,
                                     last(e[0..2]).kind,
                                     head(tail(e[0..2])).kind,
                                     last(tail(e[0..2])).kind,
                                     head(reverse(e[0..2])).kind,
                                     last(reverse(e[0..2])).kind$cypher$)
         AS (h agtype, t agtype, r agtype, tr agtype, sh agtype, sl agtype,
             sth agtype, stl agtype, srh agtype, srl agtype)',
        graph_name)
    INTO payload_text;

    IF payload_text <> 'forward:chain:chain:forward:forward:chain:chain:chain:chain:forward' THEN
        RAISE EXCEPTION 'unexpected VLE edge-list payload smoke result: %',
                        payload_text;
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(:N {i: 0})-[*1..1]->(n) RETURN keys(nodes(p)[0])$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_keys(age_vle_node_properties_at(%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_vertex(%' THEN
            has_direct_vle_indexed_node_keys := true;
        END IF;
    END LOOP;

    IF NOT has_direct_vle_indexed_node_keys THEN
        RAISE EXCEPTION 'expected keys(nodes(p)[0]) for VLE to use node properties helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(:N {i: 0})-[*1..1]->(n) RETURN keys(relationships(p)[0])$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_keys(age_vle_edge_properties_at(%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' THEN
            has_direct_vle_indexed_edge_keys := true;
        END IF;
    END LOOP;

    IF NOT has_direct_vle_indexed_edge_keys THEN
        RAISE EXCEPTION 'expected keys(relationships(p)[0]) for VLE to use edge properties helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(:N {i: 0})-[*1..1]->(n) RETURN keys(head(nodes(p)))$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_keys(age_vle_node_properties_at(%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_vertex(%' THEN
            has_direct_vle_boundary_node_keys := true;
        END IF;
    END LOOP;

    IF NOT has_direct_vle_boundary_node_keys THEN
        RAISE EXCEPTION 'expected keys(head(nodes(p))) for VLE to use node properties helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(:N {i: 0})-[*1..1]->(n) RETURN keys(last(relationships(p)))$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_keys(age_vle_edge_properties_at(%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' THEN
            has_direct_vle_boundary_edge_keys := true;
        END IF;
    END LOOP;

    IF NOT has_direct_vle_boundary_edge_keys THEN
        RAISE EXCEPTION 'expected keys(last(relationships(p))) for VLE to use edge properties helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(:N {i: 0})-[*1..1]->(n) RETURN id(head(nodes(p)))$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_vle_node_id_at(%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_vertex(%' THEN
            has_direct_vle_boundary_node_id := true;
        END IF;
    END LOOP;

    IF NOT has_direct_vle_boundary_node_id THEN
        RAISE EXCEPTION 'expected id(head(nodes(p))) for VLE to use node id helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(:N {i: 0})-[*1..1]->(n) RETURN id(last(relationships(p)))$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_vle_edge_id_at(%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' THEN
            has_direct_vle_boundary_edge_id := true;
        END IF;
    END LOOP;

    IF NOT has_direct_vle_boundary_edge_id THEN
        RAISE EXCEPTION 'expected id(last(relationships(p))) for VLE to use edge id helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(:N {i: 0})-[*1..1]->(n) RETURN properties(head(nodes(p)))$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_vle_node_properties_at(%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_vertex(%' THEN
            has_direct_vle_boundary_node_properties := true;
        END IF;
    END LOOP;

    IF NOT has_direct_vle_boundary_node_properties THEN
        RAISE EXCEPTION 'expected properties(head(nodes(p))) for VLE to use node properties helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(:N {i: 0})-[*1..1]->(n) RETURN type(last(relationships(p)))$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_vle_edge_label_at(%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' THEN
            has_direct_vle_boundary_edge_type := true;
        END IF;
    END LOOP;

    IF NOT has_direct_vle_boundary_edge_type THEN
        RAISE EXCEPTION 'expected type(last(relationships(p))) for VLE to use edge label helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(:N {i: 0})-[*1..1]->(n) RETURN id(startNode(last(relationships(p))))$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_vle_edge_start_id_at(%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_vertex(%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' THEN
            has_direct_vle_boundary_start_node_id := true;
        END IF;
    END LOOP;

    IF NOT has_direct_vle_boundary_start_node_id THEN
        RAISE EXCEPTION 'expected id(startNode(last(relationships(p)))) for VLE to use edge start id helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(:N {i: 0})-[*1..1]->(n) RETURN properties(endNode(last(relationships(p))))$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_vle_edge_endpoint_field_at(%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_vertex(%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' THEN
            has_direct_vle_boundary_end_node_properties := true;
        END IF;
    END LOOP;

    IF NOT has_direct_vle_boundary_end_node_properties THEN
        RAISE EXCEPTION 'expected properties(endNode(last(relationships(p)))) for VLE to use endpoint field helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(:N {i: 0})-[*1..1]->(n) RETURN startNode(last(relationships(p)))$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_vle_edge_start_node_at(%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' THEN
            has_direct_vle_boundary_start_node_projection := true;
        END IF;
    END LOOP;

    IF NOT has_direct_vle_boundary_start_node_projection THEN
        RAISE EXCEPTION 'expected startNode(last(relationships(p))) for VLE to use endpoint projection helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(:N {i: 0})-[*1..1]->(n) RETURN keys(startNode(head(relationships(p))))$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_keys(age_vle_edge_endpoint_field_at(%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%age_startnode(%' AND
           plan_text NOT LIKE '%age_properties(%' THEN
            has_direct_vle_boundary_start_node_keys := true;
        END IF;
    END LOOP;

    IF NOT has_direct_vle_boundary_start_node_keys THEN
        RAISE EXCEPTION 'expected keys(startNode(head(relationships(p)))) for VLE to use endpoint properties helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(:N {i: 0})-[*1..1]->(n) RETURN keys(endNode(last(relationships(p))))$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_keys(age_vle_edge_endpoint_field_at(%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%age_endnode(%' AND
           plan_text NOT LIKE '%age_properties(%' THEN
            has_direct_vle_boundary_end_node_keys := true;
        END IF;
    END LOOP;

    IF NOT has_direct_vle_boundary_end_node_keys THEN
        RAISE EXCEPTION 'expected keys(endNode(last(relationships(p)))) for VLE to use endpoint properties helper';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH (n:N) RETURN n{.i}$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%n.properties%' AND
           plan_text NOT LIKE '%_agtype_build_vertex(%' AND
           plan_text NOT LIKE '%age_properties(%' THEN
            has_direct_vertex_map_projection := true;
        END IF;
    END LOOP;

    IF NOT has_direct_vertex_map_projection THEN
        RAISE EXCEPTION 'expected vertex map projection to lower through hidden raw properties';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH (s:N {i: 0}) MATCH (s)-[:R]->(n) RETURN s IS NOT NULL$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%s.id IS NOT NULL%' AND
           plan_text NOT LIKE '%_agtype_build_vertex(%' THEN
            has_direct_vertex_null_test_projection := true;
        END IF;
    END LOOP;

    IF NOT has_direct_vertex_null_test_projection THEN
        RAISE EXCEPTION 'expected vertex null test to lower through raw vertex id';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(:N {i: 0})-[e:R]->(n) RETURN length(p)$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%CASE WHEN (e.id IS NOT NULL)%' AND
           plan_text LIKE '%THEN ''1''::agtype%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_vertex(%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' THEN
            has_direct_fixed_path_length_projection := true;
        END IF;
    END LOOP;

    IF NOT has_direct_fixed_path_length_projection THEN
        RAISE EXCEPTION 'expected fixed path length to lower through raw edge id';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(:N {i: 0})-[e:R]->(n) RETURN relationships(p)$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%agtype_build_list(%_agtype_build_edge_label(%::oid, e.id, e.start_id, e.end_id%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_vertex(%' THEN
            has_direct_fixed_path_relationships_projection := true;
        END IF;
    END LOOP;

    IF NOT has_direct_fixed_path_relationships_projection THEN
        RAISE EXCEPTION 'expected fixed path relationships to lower through raw edge columns';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(s:N {i: 0})-[e:R]->(n) RETURN nodes(p)$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%agtype_build_list(%_agtype_build_vertex_label(%::oid, s.id%' AND
           plan_text LIKE '%_agtype_build_vertex_label(%::oid, n.id%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' THEN
            has_direct_fixed_path_nodes_projection := true;
        END IF;
    END LOOP;

    IF NOT has_direct_fixed_path_nodes_projection THEN
        RAISE EXCEPTION 'expected fixed path nodes to lower through raw vertex columns';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(s:N {i: 0})-[e:R]->(n) RETURN nodes(p)[1..]$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%agtype_build_list(%_agtype_build_vertex_label(%::oid, n.id%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%agtype_access_slice%' THEN
            has_direct_fixed_path_node_suffix_slice := true;
        END IF;
    END LOOP;

    IF NOT has_direct_fixed_path_node_suffix_slice THEN
        RAISE EXCEPTION 'expected nodes(p)[1..] to build final end vertex list from raw columns';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(s:N {i: 0})-[e:R]->(n) RETURN nodes(p)[0..1]$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%agtype_build_list(%_agtype_build_vertex_label(%::oid, s.id%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%agtype_access_slice%' THEN
            has_direct_fixed_path_node_prefix_slice := true;
        END IF;
    END LOOP;

    IF NOT has_direct_fixed_path_node_prefix_slice THEN
        RAISE EXCEPTION 'expected nodes(p)[0..1] to build final start vertex list from raw columns';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(s:N {i: 0})-[e:R]->(n) RETURN relationships(p)[1..]$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%Output:%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_vertex(%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%agtype_access_slice%' THEN
            has_direct_fixed_path_relationship_suffix_slice := true;
        END IF;
    END LOOP;

    IF NOT has_direct_fixed_path_relationship_suffix_slice THEN
        RAISE EXCEPTION 'expected relationships(p)[1..] to lower to raw guarded empty list';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(s:N {i: 0})-[e:R]->(n) RETURN relationships(p)[0..1]$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%agtype_build_list(%_agtype_build_edge_label(%::oid, e.id, e.start_id, e.end_id%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_vertex(%' AND
           plan_text NOT LIKE '%agtype_access_slice%' THEN
            has_direct_fixed_path_relationship_prefix_slice := true;
        END IF;
    END LOOP;

    IF NOT has_direct_fixed_path_relationship_prefix_slice THEN
        RAISE EXCEPTION 'expected relationships(p)[0..1] to build final edge list from raw columns';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(s:N {i: 0})-[e:R]->(n) RETURN size(nodes(p)[1..])$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%e.id IS NOT NULL%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_vertex(%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%agtype_access_slice%' AND
           plan_text NOT LIKE '%age_size(%' THEN
            has_direct_fixed_path_node_slice_size := true;
        END IF;
    END LOOP;

    IF NOT has_direct_fixed_path_node_slice_size THEN
        RAISE EXCEPTION 'expected size(nodes(p)[1..]) to lower to raw guarded constant';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(s:N {i: 0})-[e:R]->(n) RETURN isEmpty(relationships(p)[1..])$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%e.id IS NOT NULL%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_vertex(%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%agtype_access_slice%' AND
           plan_text NOT LIKE '%age_is_empty(%' THEN
            has_direct_fixed_path_relationship_slice_is_empty := true;
        END IF;
    END LOOP;

    IF NOT has_direct_fixed_path_relationship_slice_is_empty THEN
        RAISE EXCEPTION 'expected isEmpty(relationships(p)[1..]) to lower to raw guarded true';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(s:N {i: 0})-[e:R]->(n) RETURN id(head(nodes(p)[1..]))$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%THEN (n.id)::agtype%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_vertex(%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%agtype_access_slice%' AND
           plan_text NOT LIKE '%age_head%' THEN
            has_direct_fixed_path_node_slice_head_id := true;
        END IF;
    END LOOP;

    IF NOT has_direct_fixed_path_node_slice_head_id THEN
        RAISE EXCEPTION 'expected id(head(nodes(p)[1..])) to lower through raw end vertex id';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(s:N {i: 0})-[e:R]->(n) RETURN properties(head(relationships(p)[0..1]))$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%THEN e.properties%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_vertex(%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%agtype_access_slice%' AND
           plan_text NOT LIKE '%age_head%' THEN
            has_direct_fixed_path_relationship_slice_head_properties := true;
        END IF;
    END LOOP;

    IF NOT has_direct_fixed_path_relationship_slice_head_properties THEN
        RAISE EXCEPTION 'expected properties(head(relationships(p)[0..1])) to lower through raw edge properties';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(s:N {i: 0})-[e:R]->(n) RETURN tail(nodes(p)[0..])$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%agtype_build_list(%_agtype_build_vertex_label(%::oid, n.id%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%agtype_access_slice%' AND
           plan_text NOT LIKE '%age_tail%' THEN
            has_direct_fixed_path_node_slice_tail_projection := true;
        END IF;
    END LOOP;

    IF NOT has_direct_fixed_path_node_slice_tail_projection THEN
        RAISE EXCEPTION 'expected tail(nodes(p)[0..]) to lower to raw end vertex list';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(s:N {i: 0})-[e:R]->(n) RETURN tail(relationships(p)[0..1])$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%Output:%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_vertex(%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%agtype_access_slice%' AND
           plan_text NOT LIKE '%age_tail%' THEN
            has_direct_fixed_path_relationship_slice_tail_projection := true;
        END IF;
    END LOOP;

    IF NOT has_direct_fixed_path_relationship_slice_tail_projection THEN
        RAISE EXCEPTION 'expected tail(relationships(p)[0..1]) to lower to raw guarded empty list';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(s:N {i: 0})-[e:R]->(n) RETURN reverse(nodes(p)[0..])$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%agtype_build_list(%_agtype_build_vertex_label(%::oid, n.id%' AND
           plan_text LIKE '%_agtype_build_vertex_label(%::oid, s.id%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%agtype_access_slice%' AND
           plan_text NOT LIKE '%age_reverse%' THEN
            has_direct_fixed_path_node_slice_reverse_projection := true;
        END IF;
    END LOOP;

    IF NOT has_direct_fixed_path_node_slice_reverse_projection THEN
        RAISE EXCEPTION 'expected reverse(nodes(p)[0..]) to lower to reversed raw vertex list';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(s:N {i: 0})-[e:R]->(n) RETURN reverse(relationships(p)[1..])$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%Output:%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_vertex(%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%agtype_access_slice%' AND
           plan_text NOT LIKE '%age_reverse%' THEN
            has_direct_fixed_path_relationship_slice_reverse_projection := true;
        END IF;
    END LOOP;

    IF NOT has_direct_fixed_path_relationship_slice_reverse_projection THEN
        RAISE EXCEPTION 'expected reverse(relationships(p)[1..]) to lower to raw guarded empty list';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(s:N {i: 0})-[e:R]->(n) RETURN id(head(tail(nodes(p)[0..])))$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%THEN (n.id)::agtype%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_vertex(%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%agtype_access_slice%' AND
           plan_text NOT LIKE '%age_tail%' AND
           plan_text NOT LIKE '%age_head%' THEN
            has_direct_fixed_path_node_slice_tail_head_id := true;
        END IF;
    END LOOP;

    IF NOT has_direct_fixed_path_node_slice_tail_head_id THEN
        RAISE EXCEPTION 'expected id(head(tail(nodes(p)[0..]))) to lower through raw end vertex id';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(s:N {i: 0})-[e:R]->(n) RETURN properties(head(reverse(relationships(p)[0..1])))$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%THEN e.properties%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_vertex(%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%agtype_access_slice%' AND
           plan_text NOT LIKE '%age_reverse%' AND
           plan_text NOT LIKE '%age_head%' THEN
            has_direct_fixed_path_rel_slice_rev_head_props := true;
        END IF;
    END LOOP;

    IF NOT has_direct_fixed_path_rel_slice_rev_head_props THEN
        RAISE EXCEPTION 'expected properties(head(reverse(relationships(p)[0..1]))) to lower through raw edge properties';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(s:N {i: 0})-[e:R]->(n) RETURN head(tail(nodes(p)[0..])).i$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%n.properties%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_vertex(%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%agtype_access_slice%' AND
           plan_text NOT LIKE '%age_tail%' AND
           plan_text NOT LIKE '%age_head%' THEN
            has_direct_fixed_path_node_slice_tail_head_prop := true;
        END IF;
    END LOOP;

    IF NOT has_direct_fixed_path_node_slice_tail_head_prop THEN
        RAISE EXCEPTION 'expected head(tail(nodes(p)[0..])).i to lower through raw end vertex properties';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(s:N {i: 0})-[e:R]->(n) RETURN head(reverse(relationships(p)[0..1])).kind$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%e.properties%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_vertex(%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%agtype_access_slice%' AND
           plan_text NOT LIKE '%age_reverse%' AND
           plan_text NOT LIKE '%age_head%' THEN
            has_direct_fixed_path_rel_slice_rev_head_prop_access := true;
        END IF;
    END LOOP;

    IF NOT has_direct_fixed_path_rel_slice_rev_head_prop_access THEN
        RAISE EXCEPTION 'expected head(reverse(relationships(p)[0..1])).kind to lower through raw edge properties';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(s:N {i: 0})-[e:R]->(n) RETURN startNode(head(reverse(relationships(p)[0..1]))).i$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%s.properties%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_vertex(%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%agtype_access_slice%' AND
           plan_text NOT LIKE '%age_reverse%' AND
           plan_text NOT LIKE '%age_head%' AND
           plan_text NOT LIKE '%age_start_node%' THEN
            has_direct_fixed_path_rel_slice_start_node_prop := true;
        END IF;
    END LOOP;

    IF NOT has_direct_fixed_path_rel_slice_start_node_prop THEN
        RAISE EXCEPTION 'expected startNode(head(reverse(relationships(p)[0..1]))).i to lower through raw start vertex properties';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(s:N {i: 0})-[e:R]->(n) RETURN id(endNode(head(relationships(p)[0..1])))$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%THEN (n.id)::agtype%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_vertex(%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%agtype_access_slice%' AND
           plan_text NOT LIKE '%age_head%' AND
           plan_text NOT LIKE '%age_end_node%' THEN
            has_direct_fixed_path_rel_slice_end_node_id := true;
        END IF;
    END LOOP;

    IF NOT has_direct_fixed_path_rel_slice_end_node_id THEN
        RAISE EXCEPTION 'expected id(endNode(head(relationships(p)[0..1]))) to lower through raw end vertex id';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(s:N {i: 0})-[e:R]->(n) RETURN head(nodes(p))$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%_agtype_build_vertex_label(%::oid, s.id%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' THEN
            has_direct_fixed_path_head_node_projection := true;
        END IF;
    END LOOP;

    IF NOT has_direct_fixed_path_head_node_projection THEN
        RAISE EXCEPTION 'expected head(nodes(p)) to build final start vertex from raw columns';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(s:N {i: 0})-[e:R]->(n) RETURN last(nodes(p))$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%_agtype_build_vertex_label(%::oid, n.id%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' THEN
            has_direct_fixed_path_last_node_projection := true;
        END IF;
    END LOOP;

    IF NOT has_direct_fixed_path_last_node_projection THEN
        RAISE EXCEPTION 'expected last(nodes(p)) to build final end vertex from raw columns';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(s:N {i: 0})-[e:R]->(n) RETURN head(relationships(p))$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%_agtype_build_edge_label(%::oid, e.id, e.start_id, e.end_id%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_vertex(%' THEN
            has_direct_fixed_path_head_edge_projection := true;
        END IF;
    END LOOP;

    IF NOT has_direct_fixed_path_head_edge_projection THEN
        RAISE EXCEPTION 'expected head(relationships(p)) to build final edge from raw columns';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(s:N {i: 0})-[e:R]->(n) RETURN last(relationships(p))$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%_agtype_build_edge_label(%::oid, e.id, e.start_id, e.end_id%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_vertex(%' THEN
            has_direct_fixed_path_last_edge_projection := true;
        END IF;
    END LOOP;

    IF NOT has_direct_fixed_path_last_edge_projection THEN
        RAISE EXCEPTION 'expected last(relationships(p)) to build final edge from raw columns';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(s:N {i: 0})-[e:R]->(n) RETURN id(head(nodes(p)))$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%THEN (s.id)::agtype%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_vertex(%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%age_id(%' THEN
            has_direct_fixed_path_head_node_id := true;
        END IF;
    END LOOP;

    IF NOT has_direct_fixed_path_head_node_id THEN
        RAISE EXCEPTION 'expected id(head(nodes(p))) to lower through raw start vertex id';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(s:N {i: 0})-[e:R]->(n) RETURN properties(last(nodes(p)))$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%THEN n.properties%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_vertex(%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%age_properties(%' THEN
            has_direct_fixed_path_last_node_properties := true;
        END IF;
    END LOOP;

    IF NOT has_direct_fixed_path_last_node_properties THEN
        RAISE EXCEPTION 'expected properties(last(nodes(p))) to lower through raw end vertex properties';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(s:N {i: 0})-[e:R]->(n) RETURN type(head(relationships(p)))$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%_label_name(%' AND
           plan_text LIKE '%e.id%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_vertex(%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%age_type(%' THEN
            has_direct_fixed_path_head_edge_type := true;
        END IF;
    END LOOP;

    IF NOT has_direct_fixed_path_head_edge_type THEN
        RAISE EXCEPTION 'expected type(head(relationships(p))) to lower through raw edge id';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(s:N {i: 0})-[e:R]->(n) RETURN start_id(last(relationships(p)))$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%THEN (e.start_id)::agtype%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_vertex(%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%age_start_id(%' THEN
            has_direct_fixed_path_last_edge_start_id := true;
        END IF;
    END LOOP;

    IF NOT has_direct_fixed_path_last_edge_start_id THEN
        RAISE EXCEPTION 'expected start_id(last(relationships(p))) to lower through raw edge start id';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(s:N {i: 0})-[e:R]->(n) RETURN tail(nodes(p))$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%agtype_build_list(%_agtype_build_vertex_label(%::oid, n.id%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' THEN
            has_direct_fixed_path_tail_nodes_projection := true;
        END IF;
    END LOOP;

    IF NOT has_direct_fixed_path_tail_nodes_projection THEN
        RAISE EXCEPTION 'expected tail(nodes(p)) to build final end vertex list from raw columns';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(s:N {i: 0})-[e:R]->(n) RETURN tail(relationships(p))$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%Output:%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_vertex(%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%age_tail(%' THEN
            has_direct_fixed_path_tail_relationships_projection := true;
        END IF;
    END LOOP;

    IF NOT has_direct_fixed_path_tail_relationships_projection THEN
        RAISE EXCEPTION 'expected tail(relationships(p)) to lower to empty list without entity builds';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(s:N {i: 0})-[e:R]->(n) RETURN id(head(tail(nodes(p))))$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%THEN (n.id)::agtype%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_vertex(%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%age_id(%' THEN
            has_direct_fixed_path_tail_head_node_id := true;
        END IF;
    END LOOP;

    IF NOT has_direct_fixed_path_tail_head_node_id THEN
        RAISE EXCEPTION 'expected id(head(tail(nodes(p)))) to lower through raw end vertex id';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(s:N {i: 0})-[e:R]->(n) RETURN properties(last(tail(nodes(p))))$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%THEN n.properties%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_vertex(%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%age_properties(%' THEN
            has_direct_fixed_path_tail_last_node_properties := true;
        END IF;
    END LOOP;

    IF NOT has_direct_fixed_path_tail_last_node_properties THEN
        RAISE EXCEPTION 'expected properties(last(tail(nodes(p)))) to lower through raw end vertex properties';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(s:N {i: 0})-[e:R]->(n) RETURN reverse(nodes(p))$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%agtype_build_list(%_agtype_build_vertex_label(%::oid, n.id%' AND
           plan_text LIKE '%_agtype_build_vertex_label(%::oid, s.id%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' THEN
            has_direct_fixed_path_reverse_nodes_projection := true;
        END IF;
    END LOOP;

    IF NOT has_direct_fixed_path_reverse_nodes_projection THEN
        RAISE EXCEPTION 'expected reverse(nodes(p)) to build final reversed vertex list from raw columns';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(s:N {i: 0})-[e:R]->(n) RETURN reverse(relationships(p))$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%agtype_build_list(%_agtype_build_edge_label(%::oid, e.id, e.start_id, e.end_id%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_vertex(%' THEN
            has_direct_fixed_path_reverse_relationships_projection := true;
        END IF;
    END LOOP;

    IF NOT has_direct_fixed_path_reverse_relationships_projection THEN
        RAISE EXCEPTION 'expected reverse(relationships(p)) to build final edge list from raw columns';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(s:N {i: 0})-[e:R]->(n) RETURN id(reverse(nodes(p))[0])$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%THEN (n.id)::agtype%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_vertex(%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%age_id(%' THEN
            has_direct_fixed_path_reverse_node_id := true;
        END IF;
    END LOOP;

    IF NOT has_direct_fixed_path_reverse_node_id THEN
        RAISE EXCEPTION 'expected id(reverse(nodes(p))[0]) to lower through raw end vertex id';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(s:N {i: 0})-[e:R]->(n) RETURN properties(reverse(nodes(p))[1])$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%THEN s.properties%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_vertex(%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%age_properties(%' THEN
            has_direct_fixed_path_reverse_node_properties := true;
        END IF;
    END LOOP;

    IF NOT has_direct_fixed_path_reverse_node_properties THEN
        RAISE EXCEPTION 'expected properties(reverse(nodes(p))[1]) to lower through raw start vertex properties';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(s:N {i: 0})-[e:R]->(n) RETURN type(reverse(relationships(p))[0])$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%_label_name(%' AND
           plan_text LIKE '%e.id%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_vertex(%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%age_type(%' THEN
            has_direct_fixed_path_reverse_edge_type := true;
        END IF;
    END LOOP;

    IF NOT has_direct_fixed_path_reverse_edge_type THEN
        RAISE EXCEPTION 'expected type(reverse(relationships(p))[0]) to lower through raw edge id';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(s:N {i: 0})-[e:R]->(n) RETURN start_id(reverse(relationships(p))[0])$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%THEN (e.start_id)::agtype%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_vertex(%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%age_start_id(%' THEN
            has_direct_fixed_path_reverse_edge_start_id := true;
        END IF;
    END LOOP;

    IF NOT has_direct_fixed_path_reverse_edge_start_id THEN
        RAISE EXCEPTION 'expected start_id(reverse(relationships(p))[0]) to lower through raw edge start id';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(s:N {i: 0})-[e:R]->(n) RETURN size(nodes(p))$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%e.id IS NOT NULL%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_vertex(%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%age_size(%' THEN
            has_direct_fixed_path_nodes_size := true;
        END IF;
    END LOOP;

    IF NOT has_direct_fixed_path_nodes_size THEN
        RAISE EXCEPTION 'expected size(nodes(p)) to lower to raw guarded constant';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(s:N {i: 0})-[e:R]->(n) RETURN size(tail(relationships(p)))$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%e.id IS NOT NULL%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_vertex(%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%age_size(%' THEN
            has_direct_fixed_path_tail_relationships_size := true;
        END IF;
    END LOOP;

    IF NOT has_direct_fixed_path_tail_relationships_size THEN
        RAISE EXCEPTION 'expected size(tail(relationships(p))) to lower to raw guarded zero';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(s:N {i: 0})-[e:R]->(n) RETURN isEmpty(reverse(nodes(p)))$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%e.id IS NOT NULL%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_vertex(%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%age_is_empty(%' THEN
            has_direct_fixed_path_reverse_nodes_is_empty := true;
        END IF;
    END LOOP;

    IF NOT has_direct_fixed_path_reverse_nodes_is_empty THEN
        RAISE EXCEPTION 'expected isEmpty(reverse(nodes(p))) to lower to raw guarded false';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(s:N {i: 0})-[e:R]->(n) RETURN isEmpty(tail(relationships(p)))$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%e.id IS NOT NULL%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_vertex(%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%age_is_empty(%' THEN
            has_direct_fixed_path_tail_relationships_is_empty := true;
        END IF;
    END LOOP;

    IF NOT has_direct_fixed_path_tail_relationships_is_empty THEN
        RAISE EXCEPTION 'expected isEmpty(tail(relationships(p))) to lower to raw guarded true';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(s:N {i: 0})-[e:R]->(n) RETURN id(nodes(p)[0])$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%THEN (s.id)::agtype%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_vertex(%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%age_id(%' THEN
            has_direct_fixed_path_indexed_node_id := true;
        END IF;
    END LOOP;

    IF NOT has_direct_fixed_path_indexed_node_id THEN
        RAISE EXCEPTION 'expected id(nodes(p)[0]) to lower through raw vertex id';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(s:N {i: 0})-[e:R]->(n) RETURN id(relationships(p)[0])$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%THEN (e.id)::agtype%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_vertex(%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%age_id(%' THEN
            has_direct_fixed_path_indexed_edge_id := true;
        END IF;
    END LOOP;

    IF NOT has_direct_fixed_path_indexed_edge_id THEN
        RAISE EXCEPTION 'expected id(relationships(p)[0]) to lower through raw edge id';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(s:N {i: 0})-[e:R]->(n) RETURN properties(nodes(p)[0])$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%THEN s.properties%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_vertex(%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%age_properties(%' THEN
            has_direct_fixed_path_indexed_node_properties := true;
        END IF;
    END LOOP;

    IF NOT has_direct_fixed_path_indexed_node_properties THEN
        RAISE EXCEPTION 'expected properties(nodes(p)[0]) to lower through raw vertex properties';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(s:N {i: 0})-[e:R]->(n) RETURN properties(relationships(p)[0])$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%THEN e.properties%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_vertex(%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%age_properties(%' THEN
            has_direct_fixed_path_indexed_edge_properties := true;
        END IF;
    END LOOP;

    IF NOT has_direct_fixed_path_indexed_edge_properties THEN
        RAISE EXCEPTION 'expected properties(relationships(p)[0]) to lower through raw edge properties';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(s:N {i: 0})-[e:R]->(n) RETURN label(nodes(p)[0])$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%_label_name(%' AND
           plan_text LIKE '%s.id%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_vertex(%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%age_label(%' THEN
            has_direct_fixed_path_indexed_node_label := true;
        END IF;
    END LOOP;

    IF NOT has_direct_fixed_path_indexed_node_label THEN
        RAISE EXCEPTION 'expected label(nodes(p)[0]) to lower through raw vertex id';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(s:N {i: 0})-[e:R]->(n) RETURN type(relationships(p)[0])$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%_label_name(%' AND
           plan_text LIKE '%e.id%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_vertex(%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%age_type(%' THEN
            has_direct_fixed_path_indexed_edge_type := true;
        END IF;
    END LOOP;

    IF NOT has_direct_fixed_path_indexed_edge_type THEN
        RAISE EXCEPTION 'expected type(relationships(p)[0]) to lower through raw edge id';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(s:N {i: 0})-[e:R]->(n) RETURN labels(nodes(p)[0])$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%agtype_build_list(%' AND
           plan_text LIKE '%_label_name(%' AND
           plan_text LIKE '%s.id%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_vertex(%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%age_labels(%' THEN
            has_direct_fixed_path_indexed_node_labels := true;
        END IF;
    END LOOP;

    IF NOT has_direct_fixed_path_indexed_node_labels THEN
        RAISE EXCEPTION 'expected labels(nodes(p)[0]) to lower through raw vertex id';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(s:N {i: 0})-[e:R]->(n) RETURN nodes(p)[0].i$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%s.properties%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_vertex(%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%age_properties(%' THEN
            has_direct_fixed_path_indexed_node_property_access := true;
        END IF;
    END LOOP;

    IF NOT has_direct_fixed_path_indexed_node_property_access THEN
        RAISE EXCEPTION 'expected nodes(p)[0].i to lower through raw vertex properties';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(s:N {i: 0})-[e:R]->(n) RETURN relationships(p)[0].kind$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%e.properties%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_vertex(%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%age_properties(%' THEN
            has_direct_fixed_path_indexed_edge_property_access := true;
        END IF;
    END LOOP;

    IF NOT has_direct_fixed_path_indexed_edge_property_access THEN
        RAISE EXCEPTION 'expected relationships(p)[0].kind to lower through raw edge properties';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(s:N {i: 0})-[e:R]->(n) RETURN keys(nodes(p)[0])$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_keys(s.properties)%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_vertex(%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' THEN
            has_direct_fixed_path_indexed_node_keys := true;
        END IF;
    END LOOP;

    IF NOT has_direct_fixed_path_indexed_node_keys THEN
        RAISE EXCEPTION 'expected keys(nodes(p)[0]) to lower through raw vertex properties';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(s:N {i: 0})-[e:R]->(n) RETURN keys(relationships(p)[0])$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_keys(e.properties)%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_vertex(%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' THEN
            has_direct_fixed_path_indexed_edge_keys := true;
        END IF;
    END LOOP;

    IF NOT has_direct_fixed_path_indexed_edge_keys THEN
        RAISE EXCEPTION 'expected keys(relationships(p)[0]) to lower through raw edge properties';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(s:N {i: 0})-[e:R]->(n) RETURN start_id(relationships(p)[0])$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%THEN (e.start_id)::agtype%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_vertex(%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%age_start_id(%' THEN
            has_direct_fixed_path_indexed_edge_start_id := true;
        END IF;
    END LOOP;

    IF NOT has_direct_fixed_path_indexed_edge_start_id THEN
        RAISE EXCEPTION 'expected start_id(relationships(p)[0]) to lower through raw edge start id';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(s:N {i: 0})-[e:R]->(n) RETURN end_id(relationships(p)[0])$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%THEN (e.end_id)::agtype%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_vertex(%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%age_end_id(%' THEN
            has_direct_fixed_path_indexed_edge_end_id := true;
        END IF;
    END LOOP;

    IF NOT has_direct_fixed_path_indexed_edge_end_id THEN
        RAISE EXCEPTION 'expected end_id(relationships(p)[0]) to lower through raw edge end id';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(s:N {i: 0})-[e:R]->(n) RETURN startNode(relationships(p)[0]).i$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%s.properties%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_vertex(%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%age_start_node(%' THEN
            has_direct_fixed_path_indexed_start_node_property_access := true;
        END IF;
    END LOOP;

    IF NOT has_direct_fixed_path_indexed_start_node_property_access THEN
        RAISE EXCEPTION 'expected startNode(relationships(p)[0]).i to lower through raw start vertex properties';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(s:N {i: 0})-[e:R]->(n) RETURN endNode(relationships(p)[0]).i$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%n.properties%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_vertex(%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%age_end_node(%' THEN
            has_direct_fixed_path_indexed_end_node_property_access := true;
        END IF;
    END LOOP;

    IF NOT has_direct_fixed_path_indexed_end_node_property_access THEN
        RAISE EXCEPTION 'expected endNode(relationships(p)[0]).i to lower through raw end vertex properties';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(s:N {i: 0})-[e:R]->(n) RETURN startNode(relationships(p)[0])$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%_agtype_build_vertex_label(%::oid, s.id%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%age_start_node(%' THEN
            has_direct_fixed_path_indexed_start_node_projection := true;
        END IF;
    END LOOP;

    IF NOT has_direct_fixed_path_indexed_start_node_projection THEN
        RAISE EXCEPTION 'expected startNode(relationships(p)[0]) to build final vertex from raw start vertex columns';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(s:N {i: 0})-[e:R]->(n) RETURN endNode(relationships(p)[0])$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%_agtype_build_vertex_label(%::oid, n.id%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%age_end_node(%' THEN
            has_direct_fixed_path_indexed_end_node_projection := true;
        END IF;
    END LOOP;

    IF NOT has_direct_fixed_path_indexed_end_node_projection THEN
        RAISE EXCEPTION 'expected endNode(relationships(p)[0]) to build final vertex from raw end vertex columns';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(s:N {i: 0})-[e:R]->(n) RETURN id(startNode(relationships(p)[0]))$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%THEN (s.id)::agtype%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_vertex(%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%age_id(%' THEN
            has_direct_fixed_path_indexed_start_node_id := true;
        END IF;
    END LOOP;

    IF NOT has_direct_fixed_path_indexed_start_node_id THEN
        RAISE EXCEPTION 'expected id(startNode(relationships(p)[0])) to lower through raw start vertex id';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(s:N {i: 0})-[e:R]->(n) RETURN properties(endNode(relationships(p)[0]))$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%THEN n.properties%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_vertex(%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%age_properties(%' THEN
            has_direct_fixed_path_indexed_end_node_properties := true;
        END IF;
    END LOOP;

    IF NOT has_direct_fixed_path_indexed_end_node_properties THEN
        RAISE EXCEPTION 'expected properties(endNode(relationships(p)[0])) to lower through raw end vertex properties';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(s:N {i: 0})-[e:R]->(n) RETURN label(startNode(relationships(p)[0]))$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%_label_name(%' AND
           plan_text LIKE '%s.id%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_vertex(%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%age_label(%' THEN
            has_direct_fixed_path_indexed_start_node_label := true;
        END IF;
    END LOOP;

    IF NOT has_direct_fixed_path_indexed_start_node_label THEN
        RAISE EXCEPTION 'expected label(startNode(relationships(p)[0])) to lower through raw start vertex id';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(s:N {i: 0})-[e:R]->(n) RETURN keys(endNode(relationships(p)[0]))$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_keys(n.properties)%' AND
           plan_text NOT LIKE '%_agtype_build_path%' AND
           plan_text NOT LIKE '%_agtype_build_vertex(%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' THEN
            has_direct_fixed_path_indexed_end_node_keys := true;
        END IF;
    END LOOP;

    IF NOT has_direct_fixed_path_indexed_end_node_keys THEN
        RAISE EXCEPTION 'expected keys(endNode(relationships(p)[0])) to lower through raw end vertex properties';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN MATCH (:N {i: 0})-[e:R]->(n) WHERE id(e) = %s RETURN n.i$cypher$)
         AS (plan agtype)',
        graph_name, edge_id)
    LOOP
        IF (plan_text LIKE '%Index Cond: (id = %' OR
            plan_text LIKE '%Filter: (id = %') AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%age_id(%' THEN
            has_direct_edge_id_qual := true;
        END IF;
    END LOOP;

    IF NOT has_direct_edge_id_qual THEN
        RAISE EXCEPTION 'expected id(e) equality to lower to direct edge id qual';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN MATCH (:N {i: 0})-[e:R]->(n) WHERE start_id(e) = %s RETURN n.i$cypher$)
         AS (plan agtype)',
        graph_name, endpoint_id)
    LOOP
        IF (plan_text LIKE '%Index Cond: (start_id = %' OR
            plan_text LIKE '%Filter: (start_id = %') AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%age_start_id(%' THEN
            has_direct_edge_start_id_qual := true;
        END IF;
    END LOOP;

    IF NOT has_direct_edge_start_id_qual THEN
        RAISE EXCEPTION 'expected start_id(e) equality to lower to direct start_id qual';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN MATCH (:N {i: 0})-[e:R]->(n) WHERE end_id(e) = %s RETURN n.i$cypher$)
         AS (plan agtype)',
        graph_name, end_endpoint_id)
    LOOP
        IF (plan_text LIKE '%Index Cond: (end_id = %' OR
            plan_text LIKE '%Filter: (end_id = %') AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%age_end_id(%' THEN
            has_direct_edge_end_id_qual := true;
        END IF;
    END LOOP;

    IF NOT has_direct_edge_end_id_qual THEN
        RAISE EXCEPTION 'expected end_id(e) equality to lower to direct end_id qual';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN MATCH (:N {i: 0})-[e:R]->(n) WHERE properties(e) = {} RETURN n.i$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%Filter: (properties = %' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%age_properties(%' THEN
            has_direct_edge_properties_qual := true;
        END IF;
    END LOOP;

    IF NOT has_direct_edge_properties_qual THEN
        RAISE EXCEPTION 'expected properties(e) equality to lower to direct properties qual';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH (:N {i: 0})-[e:R]->(n) RETURN type(e)$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%text_to_agtype((_label_name(%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%age_type(%' THEN
            has_direct_edge_label_expr := true;
        END IF;
    END LOOP;

    IF NOT has_direct_edge_label_expr THEN
        RAISE EXCEPTION 'expected type(e) to lower to direct label-name expression';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH (:N {i: 0})-[e:R]->(n) RETURN id(e)$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%Output:%::agtype%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%age_id(%' THEN
            has_direct_edge_id_projection := true;
        END IF;
    END LOOP;

    IF NOT has_direct_edge_id_projection THEN
        RAISE EXCEPTION 'expected id(e) projection to lower to direct edge id expression';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH (:N {i: 0})-[e:R]->(n) RETURN start_id(e)$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%e.start_id%' AND
           plan_text LIKE '%::agtype%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%age_start_id(%' THEN
            has_direct_edge_start_id_projection := true;
        END IF;
    END LOOP;

    IF NOT has_direct_edge_start_id_projection THEN
        RAISE EXCEPTION 'expected start_id(e) projection to lower to direct edge start_id expression';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH (:N {i: 0})-[e:R]->(n) RETURN end_id(e)$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%e.end_id%' AND
           plan_text LIKE '%::agtype%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%age_end_id(%' THEN
            has_direct_edge_end_id_projection := true;
        END IF;
    END LOOP;

    IF NOT has_direct_edge_end_id_projection THEN
        RAISE EXCEPTION 'expected end_id(e) projection to lower to direct edge end_id expression';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH (:N {i: 0})-[e:R]->(n) RETURN properties(e)$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%Output: e.properties%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%age_properties(%' THEN
            has_direct_edge_properties_projection := true;
        END IF;
    END LOOP;

    IF NOT has_direct_edge_properties_projection THEN
        RAISE EXCEPTION 'expected properties(e) projection to lower to direct edge properties expression';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH (:N {i: 0})-[e:R]->(n) RETURN e.kind$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%e.properties%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' THEN
            has_direct_edge_property_indirection := true;
        END IF;
    END LOOP;

    IF NOT has_direct_edge_property_indirection THEN
        RAISE EXCEPTION 'expected e.kind to lower through direct edge properties';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH (:N {i: 0})-[e:R]->(n) RETURN keys(e)$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_keys(e.properties)%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' THEN
            has_direct_edge_keys_projection := true;
        END IF;
    END LOOP;

    IF NOT has_direct_edge_keys_projection THEN
        RAISE EXCEPTION 'expected keys(e) to lower through direct edge properties';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH (:N {i: 0})-[e:R]->(n) RETURN e{.kind}$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%e.properties%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%age_properties(%' THEN
            has_direct_edge_map_projection := true;
        END IF;
    END LOOP;

    IF NOT has_direct_edge_map_projection THEN
        RAISE EXCEPTION 'expected e map projection to lower through direct edge properties';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH (s:N {i: 0})-[e:R]->(n) RETURN properties(startNode(e))$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%s.properties%' AND
           plan_text NOT LIKE '%_agtype_build_vertex(%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%age_properties(%' THEN
            has_direct_edge_start_properties_projection := true;
        END IF;
    END LOOP;

    IF NOT has_direct_edge_start_properties_projection THEN
        RAISE EXCEPTION 'expected properties(startNode(e)) to lower through direct start vertex properties';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH (s:N {i: 0})-[e:R]->(n) RETURN keys(endNode(e))$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%age_keys(n.properties)%' AND
           plan_text NOT LIKE '%_agtype_build_vertex(%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' THEN
            has_direct_edge_end_keys_projection := true;
        END IF;
    END LOOP;

    IF NOT has_direct_edge_end_keys_projection THEN
        RAISE EXCEPTION 'expected keys(endNode(e)) to lower through direct end vertex properties';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH (s:N {i: 0})-[e:R]->(n) RETURN startNode(e).i$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%s.properties%' AND
           plan_text NOT LIKE '%_agtype_build_vertex(%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' THEN
            has_direct_edge_start_property_access := true;
        END IF;
    END LOOP;

    IF NOT has_direct_edge_start_property_access THEN
        RAISE EXCEPTION 'expected startNode(e).i to lower through direct start vertex properties';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH (s:N {i: 0})-[e:R]->(n) RETURN endNode(e).i$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%n.properties%' AND
           plan_text NOT LIKE '%_agtype_build_vertex(%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' THEN
            has_direct_edge_end_property_access := true;
        END IF;
    END LOOP;

    IF NOT has_direct_edge_end_property_access THEN
        RAISE EXCEPTION 'expected endNode(e).i to lower through direct end vertex properties';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH (s:N {i: 0})-[e:R]->(n) RETURN id(startNode(e))$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%THEN (s.id)::agtype%' AND
           plan_text NOT LIKE '%_agtype_build_vertex(%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%age_id(%' THEN
            has_direct_edge_start_id_function := true;
        END IF;
    END LOOP;

    IF NOT has_direct_edge_start_id_function THEN
        RAISE EXCEPTION 'expected id(startNode(e)) to lower through direct start vertex id';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH (s:N {i: 0})-[e:R]->(n) RETURN id(endNode(e))$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%THEN (n.id)::agtype%' AND
           plan_text NOT LIKE '%_agtype_build_vertex(%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%age_id(%' THEN
            has_direct_edge_end_id_function := true;
        END IF;
    END LOOP;

    IF NOT has_direct_edge_end_id_function THEN
        RAISE EXCEPTION 'expected id(endNode(e)) to lower through direct end vertex id';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH (s:N {i: 0})-[e:R]->(n) RETURN startNode(e)$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%_agtype_build_vertex_label(%::oid, s.id%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%age_start_node(%' THEN
            has_direct_edge_start_node_projection := true;
        END IF;
    END LOOP;

    IF NOT has_direct_edge_start_node_projection THEN
        RAISE EXCEPTION 'expected startNode(e) projection to build final vertex from direct start vertex columns';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH (s:N {i: 0})-[e:R]->(n) RETURN endNode(e)$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%_agtype_build_vertex_label(%::oid, n.id%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%age_end_node(%' THEN
            has_direct_edge_end_node_projection := true;
        END IF;
    END LOOP;

    IF NOT has_direct_edge_end_node_projection THEN
        RAISE EXCEPTION 'expected endNode(e) projection to build final vertex from direct end vertex columns';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH (s:N {i: 0})-[e:R]->(n) RETURN label(startNode(e))$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%_label_name(%' AND
           plan_text LIKE '%s.id%' AND
           plan_text NOT LIKE '%_agtype_build_vertex(%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%age_label(%' THEN
            has_direct_edge_start_label_projection := true;
        END IF;
    END LOOP;

    IF NOT has_direct_edge_start_label_projection THEN
        RAISE EXCEPTION 'expected label(startNode(e)) to lower through direct start vertex id';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH (s:N {i: 0})-[e:R]->(n) RETURN labels(endNode(e))$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%_label_name(%' AND
           plan_text LIKE '%n.id%' AND
           plan_text NOT LIKE '%_agtype_build_vertex(%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%age_labels(%' THEN
            has_direct_edge_end_labels_projection := true;
        END IF;
    END LOOP;

    IF NOT has_direct_edge_end_labels_projection THEN
        RAISE EXCEPTION 'expected labels(endNode(e)) to lower through direct end vertex id';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH (s:N {i: 0})-[e:R]->(n) RETURN startNode(e) IS NOT NULL$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%e.id IS NOT NULL%' AND
           plan_text NOT LIKE '%_agtype_build_vertex(%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' THEN
            has_direct_edge_start_null_test := true;
        END IF;
    END LOOP;

    IF NOT has_direct_edge_start_null_test THEN
        RAISE EXCEPTION 'expected startNode(e) null test to lower through direct edge id';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH (s:N {i: 0})-[e:R]->(n) RETURN endNode(e) IS NOT NULL$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%e.id IS NOT NULL%' AND
           plan_text NOT LIKE '%_agtype_build_vertex(%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' THEN
            has_direct_edge_end_null_test := true;
        END IF;
    END LOOP;

    IF NOT has_direct_edge_end_null_test THEN
        RAISE EXCEPTION 'expected endNode(e) null test to lower through direct edge id';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH (s:N {i: 0})-[e:R]->(n) RETURN start_id(e) IS NOT NULL$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%e.start_id IS NOT NULL%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%age_start_id(%' THEN
            has_direct_edge_start_id_null_test := true;
        END IF;
    END LOOP;

    IF NOT has_direct_edge_start_id_null_test THEN
        RAISE EXCEPTION 'expected start_id(e) null test to lower through direct edge start_id';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH (s:N {i: 0})-[e:R]->(n) RETURN end_id(e) IS NOT NULL$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%e.end_id IS NOT NULL%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%age_end_id(%' THEN
            has_direct_edge_end_id_null_test := true;
        END IF;
    END LOOP;

    IF NOT has_direct_edge_end_id_null_test THEN
        RAISE EXCEPTION 'expected end_id(e) null test to lower through direct edge end_id';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH (s:N {i: 0})-[e:R]->(n) MATCH (n) RETURN properties(startNode(e))$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%s.properties%' AND
           plan_text NOT LIKE '%_agtype_build_vertex(%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' AND
           plan_text NOT LIKE '%age_properties(%' THEN
            has_direct_previous_edge_start_properties := true;
        END IF;
    END LOOP;

    IF NOT has_direct_previous_edge_start_properties THEN
        RAISE EXCEPTION 'expected previous edge properties(startNode(e)) to lower through raw start vertex properties';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH (:N {i: 0})-[e:R]->(n) RETURN n.i$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text NOT LIKE '%_agtype_build_edge(%' THEN
            has_delayed_named_edge_surface := true;
        END IF;
    END LOOP;

    IF NOT has_delayed_named_edge_surface THEN
        RAISE EXCEPTION 'expected non-returned named edge to avoid surface edge build';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH (:N {i: 0})-[e:R]->(n) RETURN e$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%CASE WHEN (e.id IS NOT NULL)%' AND
           plan_text LIKE '%_agtype_build_edge_label(%::oid, e.id, e.start_id, e.end_id%' AND
           plan_text LIKE '%e.properties%' THEN
            has_lazy_edge_return_projection := true;
        END IF;
    END LOOP;

    IF NOT has_lazy_edge_return_projection THEN
        RAISE EXCEPTION 'expected RETURN e to lazy-build from raw edge columns';
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH (:N {i: 0})-[e:R]->(n) MATCH (n) RETURN e IS NOT NULL$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%e.id IS NOT NULL%' AND
           plan_text NOT LIKE '%_agtype_build_edge(%' THEN
            has_direct_edge_null_test_projection := true;
        END IF;
    END LOOP;

    IF NOT has_direct_edge_null_test_projection THEN
        RAISE EXCEPTION 'expected edge null test to lower through raw edge id';
    END IF;

    EXECUTE format(
        'SELECT count(*)
         FROM cypher(%L,
                     $cypher$MATCH (:N {i: 0})-[e:R]->(n) RETURN e$cypher$)
         AS (e agtype)',
        graph_name)
    INTO returned_edges;
    IF returned_edges <> 1 THEN
        RAISE EXCEPTION 'expected RETURN e to lazily materialize one edge, got %',
                        returned_edges;
    END IF;

    EXECUTE format(
        'SELECT count(*)
         FROM cypher(%L,
                     $cypher$MATCH (:N {i: 0})-[e:R]->(n) RETURN *$cypher$)
         AS (e agtype, n agtype)',
        graph_name)
    INTO returned_edges;
    IF returned_edges <> 1 THEN
        RAISE EXCEPTION 'expected RETURN * to synthesize one edge, got %',
                        returned_edges;
    END IF;

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN MATCH (s:N) WHERE id(s) = %s MATCH (s)-[:R]->(n) RETURN n.i$cypher$)
         AS (plan agtype)',
        graph_name, endpoint_id)
    LOOP
        IF plan_text LIKE '%Custom Scan (AGE Adjacency Match)%' THEN
            has_id_bound_custom_scan := true;
        END IF;
        IF plan_text LIKE '%age_id(%' THEN
            has_id_bound_age_id := true;
        END IF;
    END LOOP;

    IF NOT has_id_bound_custom_scan THEN
        RAISE EXCEPTION 'expected previous-clause id-bound endpoint to use CustomPath';
    END IF;
    IF has_id_bound_age_id THEN
        RAISE EXCEPTION 'expected previous-clause id-bound endpoint to avoid age_id accessor';
    END IF;

    PERFORM drop_graph(graph_name, true);
END
$age_custom_path$;

DO $age_vle_payload_cache$
DECLARE
    graph_name text := 'age_adj_vle_payload_cache';
    n_label_id int;
    r_label_id int;
    plan_text text;
    returned_rows int;
    has_terminal_property_vle boolean := false;
BEGIN
    PERFORM create_graph(graph_name);
    EXECUTE format('SELECT create_vlabel(%L, %L)', graph_name, 'N');
    EXECUTE format('SELECT create_elabel(%L, %L)', graph_name, 'R');

    n_label_id := _label_id(graph_name, 'N');
    r_label_id := _label_id(graph_name, 'R');

    EXECUTE format(
        'INSERT INTO %I."N"(id, properties)
         SELECT ag_catalog._graphid(%s, i::bigint),
                format(''{"i": %%s}'', i)::ag_catalog.agtype
         FROM generate_series(0, 4) AS g(i)',
        graph_name, n_label_id);

    EXECUTE format(
        'INSERT INTO %I."R"(id, start_id, end_id, properties)
         VALUES (ag_catalog._graphid(%s, 1),
                 ag_catalog._graphid(%s, 0),
                 ag_catalog._graphid(%s, 1),
                 ''{}''::ag_catalog.agtype),
                (ag_catalog._graphid(%s, 2),
                 ag_catalog._graphid(%s, 0),
                 ag_catalog._graphid(%s, 2),
                 ''{}''::ag_catalog.agtype),
                (ag_catalog._graphid(%s, 3),
                 ag_catalog._graphid(%s, 1),
                 ag_catalog._graphid(%s, 3),
                 ''{}''::ag_catalog.agtype),
                (ag_catalog._graphid(%s, 4),
                 ag_catalog._graphid(%s, 1),
                 ag_catalog._graphid(%s, 4),
                 ''{}''::ag_catalog.agtype),
                (ag_catalog._graphid(%s, 5),
                 ag_catalog._graphid(%s, 2),
                 ag_catalog._graphid(%s, 3),
                 ''{}''::ag_catalog.agtype),
                (ag_catalog._graphid(%s, 6),
                 ag_catalog._graphid(%s, 2),
                 ag_catalog._graphid(%s, 4),
                 ''{}''::ag_catalog.agtype)',
        graph_name,
        r_label_id, n_label_id, n_label_id,
        r_label_id, n_label_id, n_label_id,
        r_label_id, n_label_id, n_label_id,
        r_label_id, n_label_id, n_label_id,
        r_label_id, n_label_id, n_label_id,
        r_label_id, n_label_id, n_label_id);

    EXECUTE format('CREATE INDEX ON %I."N" USING gin (properties)',
                   graph_name);
    EXECUTE format('CREATE INDEX ON %I."R" USING age_adjacency ' ||
                   '(start_id, id, end_id)', graph_name);
    EXECUTE format('CREATE INDEX ON %I."R" USING age_adjacency ' ||
                   '(end_id, id, start_id)', graph_name);
    EXECUTE format('ANALYZE %I."N"', graph_name);
    EXECUTE format('ANALYZE %I."R"', graph_name);

    FOR plan_text IN EXECUTE format(
        'SELECT plan::text
         FROM cypher(%L,
                     $cypher$EXPLAIN (VERBOSE, COSTS OFF) MATCH (:N {i: 0})-[:R*1..2]->(n) RETURN n.i$cypher$)
         AS (plan agtype)',
        graph_name)
    LOOP
        IF plan_text LIKE '%Custom Scan (AGE VLE Stream)%' THEN
            has_terminal_property_vle := true;
        END IF;
    END LOOP;

    IF NOT has_terminal_property_vle THEN
        RAISE EXCEPTION 'expected VLE terminal property function call';
    END IF;

    EXECUTE format(
        'SELECT count(*)
         FROM cypher(%L,
                     $cypher$MATCH (:N {i: 0})-[:R*1..2]->(n) RETURN n.i$cypher$)
         AS (i agtype)',
        graph_name)
    INTO returned_rows;

    IF returned_rows <> 6 THEN
        RAISE EXCEPTION 'expected fan-out VLE to return 6 rows, got %',
                        returned_rows;
    END IF;

    PERFORM drop_graph(graph_name, true);
END
$age_vle_payload_cache$;

VACUUM age_adjacency_smoke;

COPY (
SELECT postings::text
FROM age_adjacency_debug_stats('age_adjacency_smoke_start_idx'::regclass)
) TO STDOUT;
