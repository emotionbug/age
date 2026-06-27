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

#ifndef AG_CYPHER_GRAPH_JOIN_H
#define AG_CYPHER_GRAPH_JOIN_H

#include "nodes/bitmapset.h"
#include "nodes/pathnodes.h"
#include "nodes/pg_list.h"

typedef enum AgeGraphJoinDescriptorField
{
    AGE_GRAPH_JOIN_DESC_COMPONENT = 0,
    AGE_GRAPH_JOIN_DESC_BOUND,
    AGE_GRAPH_JOIN_DESC_SOURCE_EVIDENCE,
    AGE_GRAPH_JOIN_DESC_PATTERN_KEY,
    AGE_GRAPH_JOIN_DESC_COMPONENT_KIND,
    AGE_GRAPH_JOIN_DESC_CONNECTOR_KIND,
    AGE_GRAPH_JOIN_DESC_ORDER_PROPERTY_KIND,
    AGE_GRAPH_JOIN_DESC_SOURCE_EVIDENCE_KIND,
    AGE_GRAPH_JOIN_DESC_SOURCE_KIND_ID,
    AGE_GRAPH_JOIN_DESC_SOLVED_RELIDS,
    AGE_GRAPH_JOIN_DESC_REQUIRED_OUTER,
    AGE_GRAPH_JOIN_DESC_PROVIDED_RELIDS,
    AGE_GRAPH_JOIN_DESC_ROWS,
    AGE_GRAPH_JOIN_DESC_STARTUP_COST,
    AGE_GRAPH_JOIN_DESC_TOTAL_COST,
    AGE_GRAPH_JOIN_DESC_OUTPUT_WIDTH,
    AGE_GRAPH_JOIN_DESC_PARALLEL_SAFE,
    AGE_GRAPH_JOIN_DESC_PARALLEL_AWARE,
    AGE_GRAPH_JOIN_DESC_PARALLEL_WORKERS,
    AGE_GRAPH_JOIN_DESC_GATHER_COST,
    AGE_GRAPH_JOIN_DESC_ORDER_PRESERVING,
    AGE_GRAPH_JOIN_DESC_SHARED_STATE_REQUIRED,
    AGE_GRAPH_JOIN_DESC_CANDIDATE_COUNT,
    AGE_GRAPH_JOIN_DESC_DECLARED_COVER_COUNT,
    AGE_GRAPH_JOIN_DESC_COVER_MATCH_KIND,
    AGE_GRAPH_JOIN_DESC_NEXT_CONNECTOR_KIND,
    AGE_GRAPH_JOIN_DESC_NEXT_ORDER_PROPERTY_KIND,
    AGE_GRAPH_JOIN_DESC_NEXT_SOURCE_EVIDENCE,
    AGE_GRAPH_JOIN_DESC_NEXT_ROWS,
    AGE_GRAPH_JOIN_DESC_NEXT_TOTAL_COST,
    AGE_GRAPH_JOIN_DESC_COUNT
} AgeGraphJoinDescriptorField;

typedef enum AgeGraphJoinCoverMatchKind
{
    AGE_GRAPH_JOIN_COVER_UNKNOWN = 0,
    AGE_GRAPH_JOIN_COVER_MATCHED,
    AGE_GRAPH_JOIN_COVER_PATTERN_MISMATCH,
    AGE_GRAPH_JOIN_COVER_SOURCE_MISMATCH,
    AGE_GRAPH_JOIN_COVER_COMPONENT_MISMATCH
} AgeGraphJoinCoverMatchKind;

typedef enum AgeGraphJoinComponentKind
{
    AGE_GRAPH_JOIN_COMPONENT_UNKNOWN = 0,
    AGE_GRAPH_JOIN_COMPONENT_NODE_PROPERTY_SEEK,
    AGE_GRAPH_JOIN_COMPONENT_VLE_EXPANSION,
    AGE_GRAPH_JOIN_COMPONENT_ADJACENCY_EXPANSION,
    AGE_GRAPH_JOIN_COMPONENT_VALUE_JOIN,
    AGE_GRAPH_JOIN_COMPONENT_APPLY,
    AGE_GRAPH_JOIN_COMPONENT_CARTESIAN
} AgeGraphJoinComponentKind;

typedef enum AgeGraphJoinOrderPropertyKind
{
    AGE_GRAPH_JOIN_ORDER_UNKNOWN = 0,
    AGE_GRAPH_JOIN_ORDER_QUERY,
    AGE_GRAPH_JOIN_ORDER_INDEX_ANCHORED,
    AGE_GRAPH_JOIN_ORDER_VLE_FRONTIER,
    AGE_GRAPH_JOIN_ORDER_MATRIX_FRONTIER,
    AGE_GRAPH_JOIN_ORDER_EXPAND_INTO,
    AGE_GRAPH_JOIN_ORDER_ADJACENCY_DIRECTORY,
    AGE_GRAPH_JOIN_ORDER_ADJACENCY,
    AGE_GRAPH_JOIN_ORDER_VALUE,
    AGE_GRAPH_JOIN_ORDER_APPLY,
    AGE_GRAPH_JOIN_ORDER_CARTESIAN
} AgeGraphJoinOrderPropertyKind;

typedef enum AgeGraphJoinConnectorKind
{
    AGE_GRAPH_JOIN_CONNECTOR_UNKNOWN = 0,
    AGE_GRAPH_JOIN_CONNECTOR_POSTGRES_INDEX_SEEK,
    AGE_GRAPH_JOIN_CONNECTOR_NODE_PROPERTY_INDEX_SEEK,
    AGE_GRAPH_JOIN_CONNECTOR_VLE_EXPAND,
    AGE_GRAPH_JOIN_CONNECTOR_VLE_BIDIRECTIONAL_EXPAND,
    AGE_GRAPH_JOIN_CONNECTOR_VLE_COMPOSITE_EXPAND,
    AGE_GRAPH_JOIN_CONNECTOR_VLE_EXPAND_INTO,
    AGE_GRAPH_JOIN_CONNECTOR_VLE_COMPOSITE_EXPAND_INTO,
    AGE_GRAPH_JOIN_CONNECTOR_MATRIX_FRONTIER_EXPAND,
    AGE_GRAPH_JOIN_CONNECTOR_AGE_ADJACENCY,
    AGE_GRAPH_JOIN_CONNECTOR_ADJACENCY_VALUE_JOIN,
    AGE_GRAPH_JOIN_CONNECTOR_ADJACENCY_EXPAND,
    AGE_GRAPH_JOIN_CONNECTOR_ADJACENCY_COMPOSITE_EXPAND,
    AGE_GRAPH_JOIN_CONNECTOR_VALUE_JOIN,
    AGE_GRAPH_JOIN_CONNECTOR_APPLY,
    AGE_GRAPH_JOIN_CONNECTOR_CARTESIAN
} AgeGraphJoinConnectorKind;

typedef enum AgeGraphJoinSourceEvidenceKind
{
    AGE_GRAPH_JOIN_SOURCE_EVIDENCE_UNKNOWN = 0,
    AGE_GRAPH_JOIN_SOURCE_EVIDENCE_POSTGRES_INDEX,
    AGE_GRAPH_JOIN_SOURCE_EVIDENCE_NODE_PROPERTY_INDEX,
    AGE_GRAPH_JOIN_SOURCE_EVIDENCE_VLE_EDGE_SOURCE,
    AGE_GRAPH_JOIN_SOURCE_EVIDENCE_VLE_MATRIX_FRONTIER,
    AGE_GRAPH_JOIN_SOURCE_EVIDENCE_ADJACENCY_DIRECTORY,
    AGE_GRAPH_JOIN_SOURCE_EVIDENCE_ADJACENCY_VALUE,
    AGE_GRAPH_JOIN_SOURCE_EVIDENCE_ADJACENCY_PAYLOAD,
    AGE_GRAPH_JOIN_SOURCE_EVIDENCE_GRAPH_VALUE,
    AGE_GRAPH_JOIN_SOURCE_EVIDENCE_GRAPH_APPLY,
    AGE_GRAPH_JOIN_SOURCE_EVIDENCE_GRAPH_CARTESIAN
} AgeGraphJoinSourceEvidenceKind;

typedef enum AgeGraphJoinBoundKind
{
    AGE_GRAPH_JOIN_BOUND_UNKNOWN = 0,
    AGE_GRAPH_JOIN_BOUND_UNBOUND,
    AGE_GRAPH_JOIN_BOUND_BOUND,
    AGE_GRAPH_JOIN_BOUND_DECLARED_COVER,
    AGE_GRAPH_JOIN_BOUND_VLE_OUT,
    AGE_GRAPH_JOIN_BOUND_VLE_IN,
    AGE_GRAPH_JOIN_BOUND_VLE_BOTH,
    AGE_GRAPH_JOIN_BOUND_VLE_MIXED,
    AGE_GRAPH_JOIN_BOUND_ADJACENCY_START,
    AGE_GRAPH_JOIN_BOUND_ADJACENCY_END
} AgeGraphJoinBoundKind;

typedef enum AgeGraphJoinSourceKind
{
    AGE_GRAPH_JOIN_SOURCE_UNKNOWN = 0,
    AGE_GRAPH_JOIN_SOURCE_NODE_PROPERTY_INDEX,
    AGE_GRAPH_JOIN_SOURCE_VLE_MARKER,
    AGE_GRAPH_JOIN_SOURCE_VLE_TERMINAL_PROPERTY,
    AGE_GRAPH_JOIN_SOURCE_ADJACENCY_EXPANSION,
    AGE_GRAPH_JOIN_SOURCE_ADJACENCY_NODE_PROPERTY,
    AGE_GRAPH_JOIN_SOURCE_VALUE_JOIN,
    AGE_GRAPH_JOIN_SOURCE_APPLY,
    AGE_GRAPH_JOIN_SOURCE_CARTESIAN
} AgeGraphJoinSourceKind;

typedef enum AgeGraphJoinMatchComponentShape
{
    AGE_GRAPH_JOIN_MATCH_COMPONENT_ALPHA_ACYCLIC = 0,
    AGE_GRAPH_JOIN_MATCH_COMPONENT_CYCLIC_CORE,
    AGE_GRAPH_JOIN_MATCH_COMPONENT_CYCLIC_WITH_TAIL
} AgeGraphJoinMatchComponentShape;

typedef enum AgeGraphJoinMatchDescriptorSource
{
    AGE_GRAPH_JOIN_MATCH_DESCRIPTOR_SOURCE_UNKNOWN = 0,
    AGE_GRAPH_JOIN_MATCH_DESCRIPTOR_SOURCE_GRAPH_JOIN_MATCH_IR
} AgeGraphJoinMatchDescriptorSource;

typedef enum AgeGraphJoinMatchReductionOrderKind
{
    AGE_GRAPH_JOIN_MATCH_REDUCTION_ORDER_NONE = 0,
    AGE_GRAPH_JOIN_MATCH_REDUCTION_ORDER_LEAF_PEEL
} AgeGraphJoinMatchReductionOrderKind;

typedef enum AgeGraphJoinGHDBagKind
{
    AGE_GRAPH_JOIN_GHD_BAG_CYCLIC_CORE = 0,
    AGE_GRAPH_JOIN_GHD_BAG_LEAF_TAIL
} AgeGraphJoinGHDBagKind;

typedef struct AgeGraphJoinCandidateRequest
{
    const char *display_name;
    AgeGraphJoinComponentKind component_family_kind;
    AgeGraphJoinConnectorKind connector_kind;
    AgeGraphJoinBoundKind bound_kind;
    AgeGraphJoinOrderPropertyKind order_property_kind;
    AgeGraphJoinSourceEvidenceKind source_evidence_kind;
    const char *pattern_key;
    AgeGraphJoinSourceKind source_kind_id;
    Relids solved_relids;
    Relids required_outer;
    Relids provided_relids;
    double rows;
    Cost startup_cost;
    Cost total_cost;
    int output_width;
    bool parallel_safe;
    bool parallel_aware;
    int parallel_workers;
    Cost gather_cost;
    bool order_preserving;
    bool shared_state_required;
} AgeGraphJoinCandidateRequest;

typedef struct AgeGraphJoinMatchComponent
{
    Relids relids;
    List *variable_rtis;
    List *component_ids;
    int variable_count;
    int edge_count;
    int component_count;
    int core_variable_count;
    int tail_separator_count;
    int reduction_order_edge_count;
    int ghd_bag_count;
    int ghd_separator_count;
    AgeGraphJoinMatchComponentShape shape;
    AgeGraphJoinMatchDescriptorSource descriptor_source;
    AgeGraphJoinMatchReductionOrderKind reduction_order_kind;
    List *reduction_order_edges;
    List *ghd_bags;
    List *ghd_separators;
    bool connected;
    bool cyclic;
    bool star;
} AgeGraphJoinMatchComponent;

typedef struct AgeGraphJoinLoweringInput
{
    Relids solved_relids;
    Relids required_outer;
    Relids provided_relids;
    int output_width;
} AgeGraphJoinLoweringInput;

typedef struct AgeGraphJoinComponent
{
    char *display_name;
    AgeGraphJoinComponentKind family_kind;
    Relids solved_relids;
    Relids required_outer;
    Relids provided_relids;
    double estimated_rows;
    int output_width;
    bool parallel_safe;
    bool parallel_aware;
    int parallel_workers;
    Cost gather_cost;
    bool order_preserving;
    bool shared_state_required;
} AgeGraphJoinComponent;

typedef struct AgeGraphJoinConnector
{
    AgeGraphJoinConnectorKind kind_id;
    AgeGraphJoinBoundKind bound_kind;
    AgeGraphJoinOrderPropertyKind order_property_kind;
    AgeGraphJoinSourceEvidenceKind source_evidence_kind;
    Relids solved_relids;
    Relids required_outer;
    Relids provided_relids;
    double rows;
    Cost startup_cost;
    Cost total_cost;
} AgeGraphJoinConnector;

typedef struct AgeGraphJoinCandidate
{
    AgeGraphJoinComponent component;
    AgeGraphJoinConnector connector;
    char *pattern_key;
    AgeGraphJoinSourceKind source_kind_id;
} AgeGraphJoinCandidate;

typedef struct AgeGraphJoinCandidateTable
{
    List *candidates;
    int declared_entry_count;
    AgeGraphJoinCoverMatchKind cover_match_kind;
    char *pattern_key;
    AgeGraphJoinSourceKind source_kind_id;
    AgeGraphJoinComponentKind component_family_kind;
} AgeGraphJoinCandidateTable;

typedef struct AgeGraphJoinLoweringArtifactEntry
{
    AgeGraphJoinSourceKind source_kind_id;
    AgeGraphJoinComponentKind component_family_kind;
    AgeGraphJoinCandidateTable *table;
} AgeGraphJoinLoweringArtifactEntry;

typedef struct AgeGraphJoinLoweringArtifact
{
    char *pattern_key;
    List *entries;
} AgeGraphJoinLoweringArtifact;

typedef struct AgeGraphJoinPathEvidence
{
    AgeGraphJoinComponentKind component_kind;
    AgeGraphJoinConnectorKind connector_kind;
    AgeGraphJoinOrderPropertyKind order_property_kind;
    AgeGraphJoinSourceEvidenceKind source_evidence_kind;
    const char *pattern_key;
    AgeGraphJoinSourceKind source_kind_id;
    Relids solved_relids;
    Relids required_outer;
    Relids provided_relids;
    int64 candidate_count;
    int64 declared_cover_count;
    AgeGraphJoinCoverMatchKind cover_match_kind;
    double selected_total_cost;
    double next_total_cost;
    int output_width;
    bool physical_properties_known;
    bool parallel_safe;
    bool parallel_aware;
    int parallel_workers;
    Cost gather_cost;
    bool order_preserving;
    bool shared_state_required;
    bool bound;
} AgeGraphJoinPathEvidence;

typedef struct AgeGraphJoinRelCandidate
{
    Path *path;
    AgeGraphJoinPathEvidence evidence;
    bool lowering_input;
} AgeGraphJoinRelCandidate;

typedef struct AgeGraphJoinRelPathEvidence
{
    Path *path;
    AgeGraphJoinPathEvidence evidence;
} AgeGraphJoinRelPathEvidence;

typedef struct AgeGraphJoinRelComponentCandidate
{
    Path *path;
    AgeGraphJoinPathEvidence evidence;
    AgeGraphJoinComponentKind component_kind;
    Cost total_cost;
    bool lowering_input;
} AgeGraphJoinRelComponentCandidate;

typedef struct AgeGraphJoinRelMetadata
{
    RelOptInfo *rel;
    List *candidates;
    List *lowering_artifacts;
    List *lowering_pool;
    List *lowering_candidates;
    List *path_evidence;
    List *component_candidates;
    List *match_components;
} AgeGraphJoinRelMetadata;

typedef bool (*AgeGraphJoinPathEvidenceCallback)(
    Path *path, AgeGraphJoinPathEvidence *evidence);

AgeGraphJoinCandidate *age_graph_join_make_candidate(
    const AgeGraphJoinCandidateRequest *request);
AgeGraphJoinCandidateTable *age_graph_join_make_candidate_table(void);
AgeGraphJoinLoweringArtifact *age_graph_join_make_pattern_lowering_artifact(
    const char *pattern_key, AgeGraphJoinSourceKind source_kind_id,
    AgeGraphJoinCandidateTable *table);
AgeGraphJoinLoweringArtifact *age_graph_join_make_lowering_artifact(
    const char *pattern_key, AgeGraphJoinCandidateTable *table);
void age_graph_join_lowering_artifact_add_typed_table(
    AgeGraphJoinLoweringArtifact *artifact,
    AgeGraphJoinSourceKind source_kind_id,
    AgeGraphJoinComponentKind component_family_kind,
    AgeGraphJoinCandidateTable *table);
void age_graph_join_lowering_artifact_declare_typed_entry(
    AgeGraphJoinLoweringArtifact *artifact,
    AgeGraphJoinSourceKind source_kind_id,
    AgeGraphJoinComponentKind component_family_kind);
AgeGraphJoinCandidateTable *
age_graph_join_lowering_artifact_typed_entry_table(
    AgeGraphJoinLoweringArtifact *artifact,
    AgeGraphJoinSourceKind source_kind_id,
    AgeGraphJoinComponentKind component_family_kind);
AgeGraphJoinCandidateTable *
age_graph_join_lowering_artifact_declared_entry_table(
    AgeGraphJoinLoweringArtifact *artifact,
    AgeGraphJoinSourceKind source_kind_id,
    AgeGraphJoinComponentKind component_family_kind);
AgeGraphJoinCandidate *age_graph_join_table_add_candidate(
    AgeGraphJoinCandidateTable *table,
    const AgeGraphJoinCandidateRequest *request);
void age_graph_join_init_path_request(
    AgeGraphJoinCandidateRequest *request, Path *path,
    AgeGraphJoinBoundKind bound_kind,
    AgeGraphJoinComponentKind component_family_kind,
    AgeGraphJoinConnectorKind connector_kind,
    AgeGraphJoinOrderPropertyKind order_property_kind,
    AgeGraphJoinSourceEvidenceKind source_evidence_kind,
    AgeGraphJoinSourceKind source_kind_id,
    bool shared_state_required);
void age_graph_join_init_lowered_path_request(
    AgeGraphJoinCandidateRequest *request, Path *path,
    const AgeGraphJoinLoweringInput *lowering_input,
    AgeGraphJoinBoundKind bound_kind,
    AgeGraphJoinComponentKind component_family_kind,
    AgeGraphJoinConnectorKind connector_kind,
    AgeGraphJoinOrderPropertyKind order_property_kind,
    AgeGraphJoinSourceEvidenceKind source_evidence_kind,
    AgeGraphJoinSourceKind source_kind_id,
    bool shared_state_required);
AgeGraphJoinCandidate *age_graph_join_table_add_lowered_typed_path_candidate(
    AgeGraphJoinCandidateTable *table, Path *path,
    const AgeGraphJoinLoweringInput *lowering_input,
    AgeGraphJoinBoundKind bound_kind,
    AgeGraphJoinComponentKind component_family_kind,
    AgeGraphJoinConnectorKind connector_kind,
    AgeGraphJoinOrderPropertyKind order_property_kind,
    AgeGraphJoinSourceEvidenceKind source_evidence_kind,
    AgeGraphJoinSourceKind source_kind_id,
    bool shared_state_required);
AgeGraphJoinCandidate *
age_graph_join_table_add_lowered_typed_scheduled_candidate(
    AgeGraphJoinCandidateTable *table, Path *path,
    const AgeGraphJoinLoweringInput *lowering_input,
    AgeGraphJoinBoundKind bound_kind,
    AgeGraphJoinComponentKind component_family_kind,
    AgeGraphJoinConnectorKind connector_kind,
    AgeGraphJoinOrderPropertyKind order_property_kind,
    AgeGraphJoinSourceEvidenceKind source_evidence_kind,
    AgeGraphJoinSourceKind source_kind_id,
    double rows, Cost startup_cost, Cost total_cost,
    bool shared_state_required);
AgeGraphJoinCandidate *age_graph_join_table_add_path_evidence_candidate(
    AgeGraphJoinCandidateTable *table, Path *path,
    const AgeGraphJoinPathEvidence *evidence);
AgeGraphJoinCandidate *age_graph_join_table_select_cheapest(
    const AgeGraphJoinCandidateTable *table);
bool age_graph_join_apply_selected_path_cost(
    const AgeGraphJoinCandidateTable *table, Path *path);
const char *age_graph_join_component_name(
    AgeGraphJoinComponentKind component_kind);
const char *age_graph_join_order_property_name(
    AgeGraphJoinOrderPropertyKind order_property_kind);
const char *age_graph_join_cover_match_kind_name(
    AgeGraphJoinCoverMatchKind cover_match_kind);
bool age_graph_join_order_property_kind_is_bound(
    AgeGraphJoinOrderPropertyKind order_property_kind);
const char *age_graph_join_connector_name(
    AgeGraphJoinConnectorKind connector_kind);
const char *age_graph_join_source_kind_name(
    AgeGraphJoinSourceKind source_kind_id);
const char *age_graph_join_source_evidence_kind_name(
    AgeGraphJoinSourceEvidenceKind source_evidence_kind);
const char *age_graph_join_bound_kind_name(
    AgeGraphJoinBoundKind bound_kind);
void age_graph_join_init_path_evidence(
    AgeGraphJoinPathEvidence *evidence);
void age_graph_join_complete_path_evidence(
    Path *path, AgeGraphJoinPathEvidence *evidence);
void age_graph_join_metadata_begin(PlannerInfo *root);
bool age_graph_join_metadata_matches_root(PlannerInfo *root);
AgeGraphJoinRelMetadata *age_graph_join_get_rel_metadata(RelOptInfo *rel,
                                                         bool create);
void age_graph_join_refresh_rel_metadata(
    PlannerInfo *root, RelOptInfo *rel,
    AgeGraphJoinPathEvidenceCallback evidence_callback);
AgeGraphJoinMatchComponent *age_graph_join_register_rel_match_component(
    PlannerInfo *root, RelOptInfo *rel,
    const AgeGraphJoinMatchComponent *component);
const AgeGraphJoinMatchComponent *age_graph_join_find_rel_match_component(
    PlannerInfo *root, RelOptInfo *rel, Relids relids);
void age_graph_join_register_rel_path_evidence(
    PlannerInfo *root, RelOptInfo *rel, Path *path,
    const AgeGraphJoinPathEvidence *evidence);
void age_graph_join_register_rel_candidate_table(
    PlannerInfo *root, RelOptInfo *rel, Path *path,
    const AgeGraphJoinCandidateTable *table);
void age_graph_join_register_rel_lowering_pool(
    PlannerInfo *root, RelOptInfo *rel,
    const AgeGraphJoinCandidateTable *table);
AgeGraphJoinLoweringArtifact *age_graph_join_register_rel_lowering_artifact_pool(
    PlannerInfo *root, RelOptInfo *rel,
    const AgeGraphJoinLoweringArtifact *artifact);
AgeGraphJoinCandidateTable *age_graph_join_bind_lowering_pool_to_path(
    Path *path, const AgeGraphJoinCandidateTable *table);
AgeGraphJoinCandidateTable *age_graph_join_bind_lowering_artifact_to_path(
    Path *path, const AgeGraphJoinLoweringArtifact *artifact);
void age_graph_join_register_rel_lowering_table(
    PlannerInfo *root, RelOptInfo *rel, Path *path,
    const AgeGraphJoinCandidateTable *table);
AgeGraphJoinCandidateTable *age_graph_join_register_rel_lowering_artifact_path(
    PlannerInfo *root, RelOptInfo *rel, Path *path,
    const AgeGraphJoinLoweringArtifact *artifact);
double age_graph_join_evidence_alternative_credit(
    const AgeGraphJoinPathEvidence *evidence);
Cost age_graph_join_apply_evidence_alternative_credit(
    Cost startup_cost, Cost total_cost, double credit);
double age_graph_join_path_evidence_credit(
    const AgeGraphJoinPathEvidence *outer_evidence,
    const AgeGraphJoinPathEvidence *inner_evidence);
int age_graph_join_table_candidate_count(
    const AgeGraphJoinCandidateTable *table);
AgeGraphJoinCandidate *age_graph_join_table_select_next_best(
    const AgeGraphJoinCandidateTable *table,
    const AgeGraphJoinCandidate *selected);
List *age_graph_join_candidate_private(
    const AgeGraphJoinCandidate *candidate);
List *age_graph_join_table_selected_private(
    const AgeGraphJoinCandidateTable *table);
const char *age_graph_join_descriptor_text_field(List *descriptor,
                                                 int index);
int64 age_graph_join_descriptor_int_field(List *descriptor, int index,
                                          int64 fallback);
double age_graph_join_descriptor_float_field(List *descriptor, int index,
                                             double fallback);

#endif
