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

#ifndef AGE_VLE_SETUP_H
#define AGE_VLE_SETUP_H

#include "postgres.h"

#include "nodes/cypher_nodes.h"
#include "utils/age_global_graph.h"
#include "utils/age_vle.h"

typedef struct VLETraversalLoadPolicy
{
    bool load_edge_property_metadata;
    bool load_edge_metadata;
    bool load_vertex_metadata;
} VLETraversalLoadPolicy;

typedef struct VLETraversalSourceIndexes
{
    Oid age_adjacency_out_index_oid;
    Oid age_adjacency_in_index_oid;
    Oid edge_start_index_oid;
    Oid edge_end_index_oid;
} VLETraversalSourceIndexes;

typedef struct VLETraversalSourcePolicy
{
    VLETraversalSourceIndexes indexes;
    VLETraversalLoadPolicy load_policy;
} VLETraversalSourcePolicy;

typedef struct VLETraversalGraphLoad
{
    char *graph_name;
    int graph_name_len;
    Oid graph_oid;
    Oid edge_label_oid;
    VLETraversalSourcePolicy source_policy;
} VLETraversalGraphLoad;

typedef struct VLETraversalShape
{
    bool initial_start_valid;
    graphid initial_vsid;
    bool initial_end_valid;
    graphid initial_veid;
    int64 lower;
    int64 upper;
    bool upper_infinite;
    cypher_rel_dir direction;
} VLETraversalShape;

typedef struct VLETraversalSetup
{
    VLETraversalGraphLoad graph_load;
    agtype *edge_property_constraint;
    int edge_property_constraint_count;
    VLETraversalShape shape;
} VLETraversalSetup;

extern void init_invalid_vle_traversal_source_indexes(
    VLETraversalSourceIndexes *indexes);
extern void init_vle_traversal_source_indexes_for_label(
    VLETraversalSourceIndexes *indexes, Oid edge_label_oid);
extern void init_vle_traversal_setup(AgeVLEInput *input,
                                     int64 vle_grammar_node_id,
                                     VLETraversalSetup *setup);
extern GRAPH_global_context *load_vle_graph_context_for_traversal(
    const VLETraversalGraphLoad *graph_load);

#endif
