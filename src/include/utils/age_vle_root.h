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

#ifndef AGE_VLE_ROOT_H
#define AGE_VLE_ROOT_H

#include "postgres.h"

#include "nodes/cypher_nodes.h"
#include "utils/age_global_graph.h"
#include "utils/age_graphid_ds.h"
#include "utils/age_vle_setup.h"

typedef enum VLE_path_function
{                                  /* Given a path (u)-[e]-(v)                */
    VLE_FUNCTION_PATHS_FROM,       /* Paths from a (u) without a provided (v) */
    VLE_FUNCTION_PATHS_TO,         /* Paths to a (v) without a provided (u)   */
    VLE_FUNCTION_PATHS_BETWEEN,    /* Paths between a (u) and a provided (v)  */
    VLE_FUNCTION_PATHS_ALL,        /* All paths without a provided (u) or (v) */
    VLE_FUNCTION_NONE
} VLE_path_function;

typedef enum VLETraversalSourceKind
{
    VLE_TRAVERSAL_SOURCE_NONE,
    VLE_TRAVERSAL_SOURCE_AGE_ADJACENCY,
    VLE_TRAVERSAL_SOURCE_ENDPOINT_BTREE
} VLETraversalSourceKind;

typedef struct VLETraversalDirectedSource
{
    VLETraversalSourceKind kind;
    Oid index_oid;
} VLETraversalDirectedSource;

typedef struct VLETraversalSourceLayout
{
    VLETraversalDirectedSource outgoing_source;
    VLETraversalDirectedSource incoming_source;
    cypher_rel_dir direction;
    bool use_local_edge_state;
    bool label_constrained;
    bool has_property_constraints;
} VLETraversalSourceLayout;

typedef struct VLETraversalSourceLayoutInput
{
    const VLETraversalSourceIndexes *indexes;
    int64 upper;
    bool upper_infinite;
    bool use_local_edge_state;
    bool label_constrained;
    bool has_property_constraints;
    bool preferred_source_known;
    VLETraversalSourceKind preferred_outgoing_kind;
    VLETraversalSourceKind preferred_incoming_kind;
} VLETraversalSourceLayoutInput;

typedef struct VLETraversalSourceCandidates
{
    bool age_adjacency_out;
    bool age_adjacency_in;
    bool endpoint_start;
    bool endpoint_end;
} VLETraversalSourceCandidates;

typedef struct VLETraversalSourceLayoutDecision
{
    VLETraversalSourceKind outgoing_kind;
    VLETraversalSourceKind incoming_kind;
    bool prefer_endpoint_btree;
} VLETraversalSourceLayoutDecision;

typedef struct VLETraversalRootSelectionInput
{
    GRAPH_global_context *ggctx;
    Oid edge_label_oid;
    bool empty_length_range;
    bool zero_length_only;
    int64 empty_lifecycle_batch_size;
} VLETraversalRootSelectionInput;

typedef struct VLETraversalEmptyCompletionSummary
{
    int64 completion_count;
    int64 out_count;
    int64 in_count;
    int64 batch_capacity;
    bool saturated;
} VLETraversalEmptyCompletionSummary;

typedef struct VLEContextRefreshInput
{
    bool start_valid;
    graphid start_vertex_id;
    bool end_valid;
    graphid end_vertex_id;
    int32 terminal_label_id;
    int32 terminal_endpoint_label_id;
    bool source_policy_known;
    VLETraversalSourceKind source_policy_outgoing_kind;
    VLETraversalSourceKind source_policy_incoming_kind;
    bool empty_lifecycle_policy_known;
    bool empty_lifecycle_eligible;
    int64 empty_lifecycle_depth;
    int64 empty_lifecycle_batch_size;
    bool matrix_frontier_policy_known;
    bool matrix_frontier_eligible;
    int64 matrix_frontier_depth;
    int64 matrix_frontier_batch_size;
} VLEContextRefreshInput;

typedef struct VLETraversalRootDescriptor
{
    VLE_path_function path_function;
    graphid start_vertex_id;
    graphid end_vertex_id;
    cypher_rel_dir direction;
    GraphIdNode *next_vertex;
    bool reverse_paths_to;
    bool reverse_output_path;
    VLETraversalSourceLayout source_layout;
    VLETraversalEmptyCompletionSummary empty_completion;
} VLETraversalRootDescriptor;

extern cypher_rel_dir reverse_vle_edge_direction(
    cypher_rel_dir edge_direction);
extern void init_vle_root_descriptor_from_setup(
    VLETraversalRootDescriptor *root,
    const VLETraversalSetup *setup, GRAPH_global_context *ggctx,
    const VLETraversalRootSelectionInput *selection_input,
    const VLETraversalSourceLayoutInput *source_layout_input);
extern bool init_vle_root_descriptor_from_refresh(
    VLETraversalRootDescriptor *root,
    const VLETraversalRootDescriptor *current_root,
    const VLEContextRefreshInput *refresh,
    const VLETraversalRootSelectionInput *selection_input,
    const VLETraversalSourceLayoutInput *source_layout_input);
extern void init_vle_traversal_source_layout(
    VLETraversalSourceLayout *layout,
    const VLETraversalSourceLayoutInput *input,
    cypher_rel_dir direction);
extern void select_vle_traversal_source_layout(
    VLETraversalSourceLayoutDecision *decision,
    const VLETraversalSourceLayoutInput *input,
    const VLETraversalSourceCandidates *candidates);
extern int64 get_vle_initial_edge_count(
    const VLETraversalRootSelectionInput *input, graphid vertex_id,
    cypher_rel_dir edge_direction);

#endif
