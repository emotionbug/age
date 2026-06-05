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

#ifndef AG_AGTYPE_VLE_H
#define AG_AGTYPE_VLE_H

#include "fmgr.h"
#include "funcapi.h"
#include "utils/agtype.h"
#include "utils/age_global_graph.h"

/*
 * We declare the VLE_path_container here, and in this way, so that it may be
 * used elsewhere. However, we keep the contents private by defining it in
 * the VLE container module.
 */
typedef struct VLE_path_container VLE_path_container;
typedef struct AgeVLEIterator AgeVLEIterator;

#define AGE_VLE_MAX_ARGS 9

typedef enum AgeVLEOutputRequirement
{
    AGE_VLE_OUTPUT_REQUIREMENT_UNKNOWN = 0,
    AGE_VLE_OUTPUT_REQUIREMENT_PATH,
    AGE_VLE_OUTPUT_REQUIREMENT_TERMINAL_VERTEX,
    AGE_VLE_OUTPUT_REQUIREMENT_TERMINAL_PROPERTIES,
    AGE_VLE_OUTPUT_REQUIREMENT_TERMINAL_PROPERTY
} AgeVLEOutputRequirement;

typedef struct AgeVLEInput
{
    int nargs;
    NullableDatum args[AGE_VLE_MAX_ARGS];
    AgeVLEOutputRequirement output_requirement;
    bool graph_name_known;
    bool graph_name_null;
    const char *graph_name_value;
    int graph_name_len;
    bool start_vertex_known;
    bool start_vertex_null;
    graphid start_vertex_id;
    bool end_vertex_known;
    bool end_vertex_null;
    graphid end_vertex_id;
    bool edge_prototype_known;
    bool edge_label_known;
    const char *edge_label_value;
    int edge_label_len;
    bool edge_properties_known;
    bool edge_properties_null;
    Datum edge_properties_value;
    int edge_property_constraint_count;
    bool lower_known;
    bool lower_null;
    int64 lower_value;
    bool upper_known;
    bool upper_null;
    int64 upper_value;
    bool direction_known;
    bool direction_null;
    int64 direction_value;
    bool grammar_node_known;
    bool grammar_node_null;
    int64 grammar_node_value;
    bool terminal_property_key_known;
    bool terminal_property_key_null;
    const char *terminal_property_key_value;
    int terminal_property_key_len;
    bool terminal_property_key_is_char;
    char terminal_property_key_char;
    bool source_policy_known;
    int source_policy_outgoing_kind;
    int source_policy_incoming_kind;
} AgeVLEInput;

typedef struct AgeVLEInputEdgePrototype
{
    bool label_known;
    char *label_name;
    int label_len;
    int property_constraint_count;
    agtype *property_constraint;
} AgeVLEInputEdgePrototype;

typedef struct AgeVLESourceStats
{
    int64 missing_vertex_attempts;
    int64 missing_vertex_source_hits;
    int64 age_adjacency_scans;
    int64 age_adjacency_candidates;
    int64 age_adjacency_payload_scans;
    int64 age_adjacency_payload_replays;
    int64 age_adjacency_payload_cache_seeds;
    int64 endpoint_btree_scans;
    int64 endpoint_btree_candidates;
    int64 packed_scans;
    int64 packed_candidates;
    int64 packed_empty_skips;
    int64 packed_policy_skips;
    int64 packed_suppress_out;
    int64 packed_suppress_in;
    int64 packed_suppress_self;
    int64 candidates_yielded;
    int64 candidates_pushed;
} AgeVLESourceStats;

AgeVLEIterator *age_vle_iterator_create_from_input(AgeVLEInput *input,
                                                   FuncCallContext *funcctx);
bool age_vle_iterator_next(AgeVLEIterator *iterator, Datum *result,
                           bool *is_null);
void age_vle_iterator_end(AgeVLEIterator *iterator);
void age_vle_iterator_get_source_stats(AgeVLEIterator *iterator,
                                       AgeVLESourceStats *stats);
bool age_vle_input_arg_is_null(AgeVLEInput *input, int argno);
agtype *age_vle_input_get_agtype(AgeVLEInput *input, int argno);
bool age_vle_input_get_vertex_or_id(AgeVLEInput *input, int argno,
                                    const char *type_error_msg,
                                    graphid *result);
char *age_vle_input_get_graph_name(AgeVLEInput *input, int *graph_name_len);
int64 age_vle_input_get_grammar_node(AgeVLEInput *input);
void age_vle_input_get_edge_prototype(AgeVLEInput *input,
                                      AgeVLEInputEdgePrototype *edge);
int64 age_vle_input_get_range_lower(AgeVLEInput *input);
void age_vle_input_get_range_upper(AgeVLEInput *input, int64 *upper,
                                   bool *is_infinite);
int64 age_vle_input_get_direction(AgeVLEInput *input);
bool age_vle_input_get_terminal_property_key(AgeVLEInput *input,
                                             agtype_value *key_value);

/*
 * Mode offsets for age_materialize_vle_slice_boundary().  The lower bits are
 * supplied by the boundary parser and describe head/last plus slice transform.
 */
#define VLE_SLICE_BOUNDARY_ID_OFFSET 8
#define VLE_SLICE_BOUNDARY_LABEL_OFFSET 16
#define VLE_SLICE_BOUNDARY_LABELS_OFFSET 24
#define VLE_SLICE_BOUNDARY_PROPERTIES_OFFSET 32
#define VLE_SLICE_BOUNDARY_START_NODE_OFFSET 40
#define VLE_SLICE_BOUNDARY_END_NODE_OFFSET 48
#define VLE_SLICE_BOUNDARY_START_ID_OFFSET 56
#define VLE_SLICE_BOUNDARY_END_ID_OFFSET 64
#define VLE_SLICE_BOUNDARY_START_LABEL_OFFSET 72
#define VLE_SLICE_BOUNDARY_END_LABEL_OFFSET 80
#define VLE_SLICE_BOUNDARY_START_LABELS_OFFSET 88
#define VLE_SLICE_BOUNDARY_END_LABELS_OFFSET 96
#define VLE_SLICE_BOUNDARY_START_PROPERTIES_OFFSET 104
#define VLE_SLICE_BOUNDARY_END_PROPERTIES_OFFSET 112
#define VLE_SLICE_BOUNDARY_REVERSE_OFFSET 120
#define VLE_SLICE_BOUNDARY_TAIL_REVERSE_OFFSET 240
#define VLE_SLICE_BOUNDARY_DOUBLE_TAIL_OFFSET 360
#define VLE_SLICE_BOUNDARY_SLICE_TAIL_OFFSET 720
#define VLE_SLICE_BOUNDARY_SLICE_REVERSE_OFFSET 840

/*
 * Function to take an AGTV_BINARY VLE_path_container and return a path as an
 * agtype.
 */
agtype *agt_materialize_vle_path(agtype *agt_arg_vpc);
/*
 * Function to take a AGTV_BINARY VLE_path_container and return a path as an
 * agtype_value.
 */
agtype_value *agtv_materialize_vle_path(agtype *agt_arg_vpc);
/*
 * Append the interior edge/vertex values of a VLE path container into an
 * existing path builder. Returns the number of appended values.
 */
int64 agt_vle_append_path_interior(agtype *agt_arg_vpc,
                                   agtype_in_state *result);
/*
 * Exposed helper function to make an agtype_value AGTV_ARRAY of edges from a
 * VLE_path_container.
 */
agtype_value *agtv_materialize_vle_edges(agtype *agt_arg_vpc);
/*
 * Exposed helper function to materialize one edge from a VLE_path_container.
 * Returns NULL when the index is out of bounds.
 */
agtype_value *agtv_materialize_vle_edge_at(agtype *agt_arg_vpc,
                                           int64 edge_index);
/*
 * Exposed helper function to materialize a normalized half-open edge slice
 * from a VLE_path_container.
 */
agtype_value *agtv_materialize_vle_edges_slice(agtype *agt_arg_vpc,
                                               int64 lower_index,
                                               int64 upper_index);
agtype_value *agtv_materialize_vle_edges_reversed(agtype *agt_arg_vpc);
agtype_value *agtv_materialize_vle_nodes(agtype *agt_arg_vpc);
agtype *agt_vle_edge_properties_at(agtype *agt_arg_vpc, int64 edge_index);
bool agt_vle_contains_edge_id(agtype *agt_arg_vpc, graphid edge_id);
int64 agtv_vle_edge_count(agtype *agt_arg_vpc);

#endif
