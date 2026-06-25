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

#ifndef AG_CYPHER_PATHS_H
#define AG_CYPHER_PATHS_H

#include "nodes/nodes.h"
#include "nodes/parsenodes.h"
#include "nodes/pathnodes.h"
#include "nodes/pg_list.h"
#include "access/age_adjacency.h"
#include "executor/cypher_adjacency_match.h"
#include "executor/cypher_vle_stream.h"
#include "optimizer/cypher_graph_join.h"
#include "utils/relcache.h"

typedef struct CypherAdjacencyMatchCandidate
{
    Oid graph_oid;
    Oid edge_label_oid;
    Oid index_oid;
    char *edge_alias;
    char *graph_pattern_key;
    char *bound_endpoint_alias;
    Node *bound_endpoint_expr;
    char *bound_terminal_alias;
    Node *bound_terminal_expr;
    char *terminal_alias;
    char *index_source;
    AgeAdjacencyMatchIndexKind index_kind_id;
    char *index_provider;
    AgeAdjacencyMatchIndexDirection index_direction_id;
    int32 index_property_count;
    bool index_metadata_backed;
    char *right_property_key;
    Oid right_property_index_oid;
    char *right_property_index_source;
    char *right_property_index_provider;
    char *right_property_index_type;
    bool right_property_index_metadata_backed;
    bool right_property_prefetch_eligible;
    bool right_property_prefetch_cost_rejected;
    AgeAdjacencyMatchValueKind right_property_value_kind_id;
    AgeAdjacencyMatchTerminalStrategy terminal_source_strategy_id;
    Const *right_property_value;
    Node *right_property_value_expr;
    Index edge_rti;
    Index bound_endpoint_rti;
    Index bound_terminal_rti;
    Relids solved_relids;
    Relids required_outer;
    bool exact_terminal_bound;
    bool outgoing;
    bool has_edge_variable_projection;
    bool has_edge_property_predicate;
    bool has_right_label_constraint;
    bool has_right_property_predicate;
    bool terminal_elided;
    int32 right_label_id;
    AttrNumber endpoint_attno;
    double estimated_endpoint_fanout;
    double estimated_terminal_fanout;
    double estimated_composite_fanout;
    AgeAdjacencyMatchSourceHandoff source_handoff_kind_id;
    double estimated_source_fanout;
    double estimated_source_blocks;
    double estimated_property_source_matches;
    double estimated_composite_selectivity;
    AgeGraphPropertySelectivitySource
        estimated_composite_selectivity_source_kind;
    VLESourceValuePostingKind estimated_value_posting_source_kind;
    double estimated_terminal_label_groups;
    double estimated_main_blocks;
    bool estimated_fanout_from_directory;
    AgeGraphJoinLoweringArtifact *graph_join_artifact;
    Relids graph_join_lowering_required_outer;
} CypherAdjacencyMatchCandidate;

void set_rel_pathlist_init(void);
void set_rel_pathlist_fini(void);
void cypher_rewrite_property_index_surfaces(Query *parse);
void cypher_clear_adjacency_match_candidates(void);
void cypher_register_graph_pattern_handoff(const char *graph_pattern_key);
void cypher_declare_graph_pattern_source(
    const char *graph_pattern_key, AgeGraphJoinSourceKind source_kind_id,
    AgeGraphJoinComponentKind component_family_kind);
void cypher_register_vle_pattern_handoff(const char *marker_alias,
                                         const char *graph_pattern_key);
void cypher_register_node_pattern_handoff(const char *node_alias,
                                          const char *graph_pattern_key);
double cypher_estimate_graph_property_index_matches(
    Oid property_index_oid, Const *property_value,
    double fallback_selectivity,
    AgeGraphPropertySelectivitySource *source_kind);
void cypher_register_adjacency_match_candidate(Oid edge_label_oid,
                                               Oid index_oid,
                                               Oid graph_oid,
                                               const char *edge_alias,
                                               const char *graph_pattern_key,
                                               const char *bound_endpoint_alias,
                                               Node *bound_endpoint_expr,
                                               const char *bound_terminal_alias,
                                               Node *bound_terminal_expr,
                                               const char *terminal_alias,
                                               const char *index_source,
                                               AgeAdjacencyMatchIndexKind index_kind_id,
                                               const char *index_provider,
                                               AgeAdjacencyMatchIndexDirection index_direction_id,
                                               int32 index_property_count,
                                               bool index_metadata_backed,
                                               const char *right_property_key,
                                               Oid right_property_index_oid,
                                               const char *right_property_index_source,
                                               const char *right_property_index_provider,
                                               const char *right_property_index_type,
                                               bool right_property_index_metadata_backed,
                                               Const *right_property_value,
                                               Node *right_property_value_expr,
                                               bool outgoing,
                                               bool has_edge_variable_projection,
                                               bool has_edge_property_predicate,
                                               bool has_right_label_constraint,
                                               bool has_right_property_predicate,
                                               bool terminal_elided,
                                               int32 right_label_id,
                                               AttrNumber endpoint_attno);

#endif
