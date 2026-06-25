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
    AGE_GRAPH_JOIN_DESC_CONNECTOR,
    AGE_GRAPH_JOIN_DESC_BOUND,
    AGE_GRAPH_JOIN_DESC_ORDER_PROPERTY,
    AGE_GRAPH_JOIN_DESC_SOURCE_EVIDENCE,
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
    AGE_GRAPH_JOIN_DESC_NEXT_CONNECTOR,
    AGE_GRAPH_JOIN_DESC_NEXT_ORDER_PROPERTY,
    AGE_GRAPH_JOIN_DESC_NEXT_SOURCE_EVIDENCE,
    AGE_GRAPH_JOIN_DESC_NEXT_ROWS,
    AGE_GRAPH_JOIN_DESC_NEXT_TOTAL_COST,
    AGE_GRAPH_JOIN_DESC_COUNT
} AgeGraphJoinDescriptorField;

typedef struct AgeGraphJoinCandidateRequest
{
    const char *component;
    const char *connector;
    const char *bound;
    const char *order_property;
    const char *source_evidence;
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

typedef struct AgeGraphJoinComponent
{
    char *name;
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
    char *kind;
    char *bound;
    char *order_property;
    char *source_evidence;
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
} AgeGraphJoinCandidate;

typedef struct AgeGraphJoinCandidateTable
{
    List *candidates;
} AgeGraphJoinCandidateTable;

typedef struct AgeGraphJoinPathEvidence
{
    const char *connector;
    const char *order_property;
    const char *source_evidence;
    Relids solved_relids;
    Relids required_outer;
    Relids provided_relids;
    int64 candidate_count;
    double selected_total_cost;
    double next_total_cost;
    int output_width;
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
    char *component;
    Cost total_cost;
} AgeGraphJoinRelComponentCandidate;

typedef struct AgeGraphJoinRelMetadata
{
    RelOptInfo *rel;
    List *candidates;
    List *planner_candidates;
    List *path_evidence;
    List *component_candidates;
} AgeGraphJoinRelMetadata;

typedef bool (*AgeGraphJoinPathEvidenceCallback)(
    Path *path, AgeGraphJoinPathEvidence *evidence);

AgeGraphJoinCandidate *age_graph_join_make_candidate(
    const AgeGraphJoinCandidateRequest *request);
AgeGraphJoinCandidateTable *age_graph_join_make_candidate_table(void);
AgeGraphJoinCandidate *age_graph_join_table_add_candidate(
    AgeGraphJoinCandidateTable *table,
    const AgeGraphJoinCandidateRequest *request);
AgeGraphJoinCandidate *age_graph_join_table_add_path_candidate(
    AgeGraphJoinCandidateTable *table, Path *path, const char *component,
    const char *connector, const char *bound, const char *order_property,
    const char *source_evidence, bool shared_state_required);
void age_graph_join_init_path_request(
    AgeGraphJoinCandidateRequest *request, Path *path, const char *component,
    const char *connector, const char *bound, const char *order_property,
    const char *source_evidence, bool shared_state_required);
AgeGraphJoinCandidate *age_graph_join_table_add_scheduled_candidate(
    AgeGraphJoinCandidateTable *table, Path *path, const char *component,
    const char *connector, const char *bound, const char *order_property,
    const char *source_evidence, double rows, Cost startup_cost,
    Cost total_cost, bool shared_state_required);
AgeGraphJoinCandidate *age_graph_join_table_add_path_evidence_candidate(
    AgeGraphJoinCandidateTable *table, Path *path, const char *component,
    const AgeGraphJoinPathEvidence *evidence);
AgeGraphJoinCandidate *age_graph_join_table_select_cheapest(
    const AgeGraphJoinCandidateTable *table);
bool age_graph_join_apply_selected_path_cost(
    const AgeGraphJoinCandidateTable *table, Path *path);
const char *age_graph_join_component_from_evidence(
    const AgeGraphJoinPathEvidence *evidence);
bool age_graph_join_order_property_is_bound(const char *order_property);
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
void age_graph_join_register_rel_path_evidence(
    PlannerInfo *root, RelOptInfo *rel, Path *path,
    const AgeGraphJoinPathEvidence *evidence);
void age_graph_join_register_rel_candidate_table(
    PlannerInfo *root, RelOptInfo *rel, Path *path,
    const AgeGraphJoinCandidateTable *table);
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
