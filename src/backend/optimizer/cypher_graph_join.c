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

#include "postgres.h"

#include "catalog/pg_collation_d.h"
#include "catalog/pg_type_d.h"
#include "nodes/makefuncs.h"
#include "optimizer/cypher_graph_join.h"
#include "utils/builtins.h"

static char *copy_graph_join_text(const char *value,
                                  const char *fallback);
static AgeGraphJoinComponentKind graph_join_infer_component_family_kind(
    AgeGraphJoinConnectorKind connector_kind,
    AgeGraphJoinOrderPropertyKind order_property_kind);
static bool graph_join_component_kinds_match(
    AgeGraphJoinComponentKind existing, AgeGraphJoinComponentKind candidate);
static bool graph_join_order_property_kinds_match(
    AgeGraphJoinOrderPropertyKind existing,
    AgeGraphJoinOrderPropertyKind candidate);
static bool graph_join_source_kinds_match(
    AgeGraphJoinSourceKind existing, AgeGraphJoinSourceKind candidate);
static bool graph_join_connector_kinds_match(
    AgeGraphJoinConnectorKind existing, AgeGraphJoinConnectorKind candidate);
static bool graph_join_source_evidence_matches(
    AgeGraphJoinSourceEvidenceKind existing_kind,
    AgeGraphJoinSourceEvidenceKind candidate_kind);
static void graph_join_metadata_context_reset(void *arg);
static bool graph_join_metadata_identity_matches(PlannerInfo *root);
static void apply_graph_join_lowering_input(
    AgeGraphJoinCandidateRequest *request,
    const AgeGraphJoinLoweringInput *lowering_input);
static void graph_join_metadata_rebuild_component_candidates(
    AgeGraphJoinRelMetadata *metadata);
static void graph_join_metadata_append_pool_component_candidates(
    AgeGraphJoinRelMetadata *metadata);
static AgeGraphJoinRelCandidate *graph_join_metadata_pool_carrier_candidate(
    AgeGraphJoinRelMetadata *metadata,
    const AgeGraphJoinPathEvidence *evidence);
static bool graph_join_pool_evidence_can_use_carrier(
    const AgeGraphJoinRelCandidate *carrier,
    const AgeGraphJoinPathEvidence *evidence);
static void graph_join_metadata_append_lowering_candidates(
    AgeGraphJoinRelMetadata *metadata);
static void graph_join_metadata_merge_lowering_candidate(
    AgeGraphJoinRelCandidate *candidate,
    const AgeGraphJoinRelCandidate *lowering_candidate);
static int graph_join_metadata_lowering_candidate_count(
    const AgeGraphJoinRelMetadata *metadata);
static int graph_join_metadata_lowering_artifact_entry_count(
    const AgeGraphJoinRelMetadata *metadata);
static Cost graph_join_evidence_component_total_cost(
    const AgeGraphJoinRelCandidate *candidate);
static AgeGraphJoinLoweringArtifact *graph_join_metadata_merge_lowering_artifact(
    AgeGraphJoinRelMetadata *metadata,
    const AgeGraphJoinLoweringArtifact *artifact);
static AgeGraphJoinLoweringArtifact *graph_join_metadata_find_lowering_artifact(
    AgeGraphJoinRelMetadata *metadata, const char *pattern_key);
static void graph_join_metadata_update_component_candidate(
    AgeGraphJoinRelMetadata *metadata,
    const AgeGraphJoinRelCandidate *candidate);
static bool graph_join_component_candidate_keeps_existing(
    const AgeGraphJoinRelComponentCandidate *component_candidate,
    const AgeGraphJoinRelCandidate *candidate, Cost total_cost);
static bool graph_join_component_candidate_matches(
    const AgeGraphJoinRelComponentCandidate *component_candidate,
    const AgeGraphJoinPathEvidence *evidence);
static bool graph_join_component_physical_properties_match(
    const AgeGraphJoinPathEvidence *existing,
    const AgeGraphJoinPathEvidence *candidate);
static bool graph_join_component_connector_properties_match(
    const AgeGraphJoinPathEvidence *existing,
    const AgeGraphJoinPathEvidence *candidate);
static bool graph_join_text_matches(const char *existing,
                                    const char *candidate);
static bool graph_join_metadata_lookup_path_evidence(
    const AgeGraphJoinRelMetadata *metadata, Path *path,
    AgeGraphJoinPathEvidence *evidence);
static AgeGraphJoinLoweringArtifactEntry *
graph_join_lowering_artifact_find_entry(
    AgeGraphJoinLoweringArtifact *artifact,
    AgeGraphJoinSourceKind source_kind_id, bool require_empty_table);
static AgeGraphJoinCandidateTable *graph_join_make_declared_entry_table(
    RelOptInfo *rel, const AgeGraphJoinLoweringArtifact *artifact,
    const AgeGraphJoinLoweringArtifactEntry *entry);
static AgeGraphJoinConnectorKind graph_join_declared_connector_kind(
    AgeGraphJoinSourceKind source_kind_id);
static AgeGraphJoinOrderPropertyKind graph_join_declared_order_property_kind(
    AgeGraphJoinSourceKind source_kind_id);
static AgeGraphJoinSourceEvidenceKind
graph_join_declared_source_evidence_kind(
    AgeGraphJoinSourceKind source_kind_id);
static bool graph_join_lowering_candidate_matches(
    const AgeGraphJoinRelCandidate *rel_candidate,
    const AgeGraphJoinPathEvidence *evidence);
static void graph_join_merge_lowering_pool_evidence(
    AgeGraphJoinRelMetadata *metadata,
    AgeGraphJoinPathEvidence *evidence);
static void graph_join_path_evidence_from_candidate(
    Path *path, const AgeGraphJoinCandidateTable *table,
    const AgeGraphJoinCandidate *candidate,
    AgeGraphJoinPathEvidence *evidence);
static void graph_join_candidate_table_apply_defaults(
    AgeGraphJoinCandidateTable *table, AgeGraphJoinCandidate *candidate);
static bool graph_join_planner_candidate_matches(
    const AgeGraphJoinRelCandidate *rel_candidate, Path *path,
    const AgeGraphJoinPathEvidence *evidence);
static Const *make_graph_join_text_const(const char *value);
static void set_graph_join_descriptor_value(List *descriptor, int index,
                                            Node *value);

static PlannerInfo *graph_join_metadata_root = NULL;
static PlannerGlobal *graph_join_metadata_glob = NULL;
static Query *graph_join_metadata_parse = NULL;
static MemoryContext graph_join_metadata_context = NULL;
static Index graph_join_metadata_query_level = 0;
static List *graph_join_rel_metadata = NIL;

AgeGraphJoinCandidate *age_graph_join_make_candidate(
    const AgeGraphJoinCandidateRequest *request)
{
    AgeGraphJoinCandidate *candidate;

    Assert(request != NULL);

    candidate = palloc0(sizeof(*candidate));
    candidate->component.display_name = copy_graph_join_text(
        request->display_name != NULL ? request->display_name :
        age_graph_join_component_name(request->component_family_kind),
        "graph-component");
    candidate->component.family_kind = request->component_family_kind;
    candidate->component.solved_relids = bms_copy(request->solved_relids);
    candidate->component.required_outer = bms_copy(request->required_outer);
    candidate->component.provided_relids = bms_copy(request->provided_relids);
    candidate->component.estimated_rows = request->rows;
    candidate->component.output_width = request->output_width;
    candidate->component.parallel_safe = request->parallel_safe;
    candidate->component.parallel_aware = request->parallel_aware;
    candidate->component.parallel_workers = request->parallel_workers;
    candidate->component.gather_cost = request->gather_cost;
    candidate->component.order_preserving = request->order_preserving;
    candidate->component.shared_state_required =
        request->shared_state_required;

    candidate->connector.kind_id = request->connector_kind;
    candidate->connector.bound_kind = request->bound_kind;
    candidate->connector.order_property_kind = request->order_property_kind;
    candidate->connector.source_evidence_kind =
        request->source_evidence_kind;
    candidate->connector.solved_relids = bms_copy(request->solved_relids);
    candidate->connector.required_outer = bms_copy(request->required_outer);
    candidate->connector.provided_relids = bms_copy(request->provided_relids);
    candidate->connector.rows = request->rows;
    candidate->connector.startup_cost = request->startup_cost;
    candidate->connector.total_cost = request->total_cost;
    if (request->pattern_key != NULL)
        candidate->pattern_key = pstrdup(request->pattern_key);
    candidate->source_kind_id = request->source_kind_id;

    return candidate;
}

AgeGraphJoinCandidateTable *age_graph_join_make_candidate_table(void)
{
    AgeGraphJoinCandidateTable *table;

    table = palloc0(sizeof(*table));

    return table;
}

AgeGraphJoinLoweringArtifact *age_graph_join_make_pattern_lowering_artifact(
    const char *pattern_key, AgeGraphJoinSourceKind source_kind_id,
    AgeGraphJoinCandidateTable *table)
{
    AgeGraphJoinLoweringArtifact *artifact;

    artifact = palloc0(sizeof(*artifact));
    artifact->pattern_key = copy_graph_join_text(pattern_key,
                                                  "graph-pattern");
    age_graph_join_lowering_artifact_add_typed_table(
        artifact,
        source_kind_id,
        AGE_GRAPH_JOIN_COMPONENT_UNKNOWN, table);

    return artifact;
}

AgeGraphJoinLoweringArtifact *age_graph_join_make_lowering_artifact(
    const char *pattern_key, AgeGraphJoinCandidateTable *table)
{
    return age_graph_join_make_pattern_lowering_artifact(
        pattern_key, AGE_GRAPH_JOIN_SOURCE_UNKNOWN, table);
}

void age_graph_join_lowering_artifact_add_typed_table(
    AgeGraphJoinLoweringArtifact *artifact,
    AgeGraphJoinSourceKind source_kind_id,
    AgeGraphJoinComponentKind component_family_kind,
    AgeGraphJoinCandidateTable *table)
{
    AgeGraphJoinLoweringArtifactEntry *entry;
    ListCell *lc;

    if (artifact == NULL ||
        table == NULL ||
        table->candidates == NIL)
    {
        return;
    }

    entry = graph_join_lowering_artifact_find_entry(
        artifact, source_kind_id, true);
    if (entry != NULL)
    {
        entry->table = table;
        if (entry->component_family_kind == AGE_GRAPH_JOIN_COMPONENT_UNKNOWN)
            entry->component_family_kind = component_family_kind;
        goto apply_identity;
    }

    foreach(lc, artifact->entries)
    {
        AgeGraphJoinLoweringArtifactEntry *existing = lfirst(lc);

        if (existing != NULL &&
            existing->table == table &&
            graph_join_source_kinds_match(
                existing->source_kind_id, source_kind_id))
        {
            return;
        }
    }

    entry = palloc0(sizeof(*entry));
    entry->source_kind_id = source_kind_id;
    entry->component_family_kind = component_family_kind;
    entry->table = table;
    artifact->entries = lappend(artifact->entries, entry);

apply_identity:
    table->declared_entry_count = Max(table->declared_entry_count,
                                      list_length(artifact->entries));
    table->cover_match_kind = AGE_GRAPH_JOIN_COVER_MATCHED;
    table->pattern_key = copy_graph_join_text(artifact->pattern_key,
                                               "graph-pattern");
    table->source_kind_id = entry->source_kind_id;
    table->component_family_kind = entry->component_family_kind;
    foreach(lc, table->candidates)
    {
        AgeGraphJoinCandidate *candidate = lfirst(lc);

        if (candidate == NULL)
            continue;
        graph_join_candidate_table_apply_defaults(table, candidate);
    }
}

void age_graph_join_lowering_artifact_declare_typed_entry(
    AgeGraphJoinLoweringArtifact *artifact,
    AgeGraphJoinSourceKind source_kind_id,
    AgeGraphJoinComponentKind component_family_kind)
{
    AgeGraphJoinLoweringArtifactEntry *entry;

    if (artifact == NULL ||
        source_kind_id == AGE_GRAPH_JOIN_SOURCE_UNKNOWN)
    {
        return;
    }

    entry = graph_join_lowering_artifact_find_entry(
        artifact, source_kind_id, false);
    if (entry != NULL)
    {
        if (entry->component_family_kind == AGE_GRAPH_JOIN_COMPONENT_UNKNOWN)
            entry->component_family_kind = component_family_kind;
        return;
    }

    entry = palloc0(sizeof(*entry));
    entry->source_kind_id = source_kind_id;
    entry->component_family_kind = component_family_kind;
    artifact->entries = lappend(artifact->entries, entry);
}

AgeGraphJoinCandidateTable *
age_graph_join_lowering_artifact_typed_entry_table(
    AgeGraphJoinLoweringArtifact *artifact,
    AgeGraphJoinSourceKind source_kind_id,
    AgeGraphJoinComponentKind component_family_kind)
{
    if (artifact == NULL ||
        source_kind_id == AGE_GRAPH_JOIN_SOURCE_UNKNOWN)
    {
        return NULL;
    }

    age_graph_join_lowering_artifact_declare_typed_entry(
        artifact, source_kind_id, component_family_kind);
    return age_graph_join_lowering_artifact_declared_entry_table(
        artifact, source_kind_id, component_family_kind);
}

AgeGraphJoinCandidateTable *
age_graph_join_lowering_artifact_declared_entry_table(
    AgeGraphJoinLoweringArtifact *artifact,
    AgeGraphJoinSourceKind source_kind_id,
    AgeGraphJoinComponentKind component_family_kind)
{
    AgeGraphJoinLoweringArtifactEntry *entry;

    if (artifact == NULL ||
        source_kind_id == AGE_GRAPH_JOIN_SOURCE_UNKNOWN)
    {
        return NULL;
    }

    entry = graph_join_lowering_artifact_find_entry(
        artifact, source_kind_id, false);
    if (entry == NULL)
        return NULL;

    if (entry->table == NULL)
        entry->table = age_graph_join_make_candidate_table();
    else
        entry->table->candidates = NIL;
    if (entry->component_family_kind == AGE_GRAPH_JOIN_COMPONENT_UNKNOWN)
        entry->component_family_kind = component_family_kind;

    entry->table->declared_entry_count = Max(
        entry->table->declared_entry_count, list_length(artifact->entries));
    entry->table->cover_match_kind = AGE_GRAPH_JOIN_COVER_MATCHED;
    entry->table->pattern_key = copy_graph_join_text(
        artifact->pattern_key, "graph-pattern");
    entry->table->source_kind_id = entry->source_kind_id;
    entry->table->component_family_kind = entry->component_family_kind;

    return entry->table;
}

AgeGraphJoinCandidate *age_graph_join_table_add_candidate(
    AgeGraphJoinCandidateTable *table,
    const AgeGraphJoinCandidateRequest *request)
{
    AgeGraphJoinCandidate *candidate;

    Assert(table != NULL);
    Assert(request != NULL);

    candidate = age_graph_join_make_candidate(request);
    graph_join_candidate_table_apply_defaults(table, candidate);
    table->candidates = lappend(table->candidates, candidate);

    return candidate;
}

static void graph_join_candidate_table_apply_defaults(
    AgeGraphJoinCandidateTable *table, AgeGraphJoinCandidate *candidate)
{
    if (table == NULL || candidate == NULL)
        return;

    if (table->pattern_key != NULL)
        candidate->pattern_key = copy_graph_join_text(table->pattern_key,
                                                       "graph-pattern");
    if (table->source_kind_id != AGE_GRAPH_JOIN_SOURCE_UNKNOWN)
        candidate->source_kind_id = table->source_kind_id;
    if (table->component_family_kind != AGE_GRAPH_JOIN_COMPONENT_UNKNOWN)
        candidate->component.family_kind = table->component_family_kind;
}

void age_graph_join_init_path_request(
    AgeGraphJoinCandidateRequest *request, Path *path,
    AgeGraphJoinBoundKind bound_kind,
    AgeGraphJoinComponentKind component_family_kind,
    AgeGraphJoinConnectorKind connector_kind,
    AgeGraphJoinOrderPropertyKind order_property_kind,
    AgeGraphJoinSourceEvidenceKind source_evidence_kind,
    AgeGraphJoinSourceKind source_kind_id,
    bool shared_state_required)
{
    Relids provided_relids = NULL;
    int output_width = 0;

    Assert(request != NULL);
    Assert(path != NULL);

    memset(request, 0, sizeof(*request));
    if (path->parent != NULL)
    {
        provided_relids = path->parent->relids;
        if (path->parent->reltarget != NULL)
            output_width = path->parent->reltarget->width;
    }
    if (path->pathtarget != NULL && path->pathtarget->width > 0)
        output_width = path->pathtarget->width;

    request->component_family_kind = component_family_kind;
    request->connector_kind = connector_kind;
    request->bound_kind = bound_kind;
    request->order_property_kind = order_property_kind;
    request->source_evidence_kind = source_evidence_kind;
    request->source_kind_id = source_kind_id;
    request->solved_relids = provided_relids;
    request->required_outer = PATH_REQ_OUTER(path);
    request->provided_relids = provided_relids;
    request->rows = path->rows;
    request->startup_cost = path->startup_cost;
    request->total_cost = path->total_cost;
    request->output_width = output_width;
    request->parallel_safe = path->parallel_safe;
    request->parallel_aware = path->parallel_aware;
    request->parallel_workers = path->parallel_workers;
    request->gather_cost = 0;
    request->order_preserving = path->pathkeys != NIL;
    request->shared_state_required = shared_state_required;
}

void age_graph_join_init_lowered_path_request(
    AgeGraphJoinCandidateRequest *request, Path *path,
    const AgeGraphJoinLoweringInput *lowering_input,
    AgeGraphJoinBoundKind bound_kind,
    AgeGraphJoinComponentKind component_family_kind,
    AgeGraphJoinConnectorKind connector_kind,
    AgeGraphJoinOrderPropertyKind order_property_kind,
    AgeGraphJoinSourceEvidenceKind source_evidence_kind,
    AgeGraphJoinSourceKind source_kind_id,
    bool shared_state_required)
{
    age_graph_join_init_path_request(
        request, path, bound_kind,
        component_family_kind, connector_kind, order_property_kind,
        source_evidence_kind, source_kind_id, shared_state_required);
    apply_graph_join_lowering_input(request, lowering_input);
}

static void apply_graph_join_lowering_input(
    AgeGraphJoinCandidateRequest *request,
    const AgeGraphJoinLoweringInput *lowering_input)
{
    if (request == NULL || lowering_input == NULL)
        return;

    if (lowering_input->solved_relids != NULL)
        request->solved_relids = lowering_input->solved_relids;
    if (lowering_input->required_outer != NULL)
        request->required_outer = lowering_input->required_outer;
    if (lowering_input->provided_relids != NULL)
        request->provided_relids = lowering_input->provided_relids;
    if (lowering_input->output_width > 0)
        request->output_width = lowering_input->output_width;
}

AgeGraphJoinCandidate *age_graph_join_table_add_lowered_typed_path_candidate(
    AgeGraphJoinCandidateTable *table, Path *path,
    const AgeGraphJoinLoweringInput *lowering_input,
    AgeGraphJoinBoundKind bound_kind,
    AgeGraphJoinComponentKind component_family_kind,
    AgeGraphJoinConnectorKind connector_kind,
    AgeGraphJoinOrderPropertyKind order_property_kind,
    AgeGraphJoinSourceEvidenceKind source_evidence_kind,
    AgeGraphJoinSourceKind source_kind_id,
    bool shared_state_required)
{
    AgeGraphJoinCandidateRequest request;

    age_graph_join_init_lowered_path_request(
        &request, path, lowering_input, bound_kind,
        component_family_kind, connector_kind, order_property_kind,
        source_evidence_kind, source_kind_id, shared_state_required);

    return age_graph_join_table_add_candidate(table, &request);
}

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
    bool shared_state_required)
{
    AgeGraphJoinCandidateRequest request;

    age_graph_join_init_lowered_path_request(
        &request, path, lowering_input, bound_kind,
        component_family_kind, connector_kind, order_property_kind,
        source_evidence_kind, source_kind_id, shared_state_required);
    request.rows = rows;
    request.startup_cost = startup_cost;
    request.total_cost = total_cost;

    return age_graph_join_table_add_candidate(table, &request);
}

AgeGraphJoinCandidate *age_graph_join_table_add_path_evidence_candidate(
    AgeGraphJoinCandidateTable *table, Path *path,
    const AgeGraphJoinPathEvidence *evidence)
{
    AgeGraphJoinCandidateRequest request;
    Cost total_cost;

    Assert(path != NULL);

    if (table == NULL ||
        evidence == NULL ||
        evidence->order_property_kind == AGE_GRAPH_JOIN_ORDER_UNKNOWN ||
        evidence->component_kind == AGE_GRAPH_JOIN_COMPONENT_UNKNOWN)
    {
        return NULL;
    }

    age_graph_join_init_path_request(
        &request, path,
        evidence->bound ? AGE_GRAPH_JOIN_BOUND_BOUND :
        AGE_GRAPH_JOIN_BOUND_UNBOUND,
        evidence->component_kind, evidence->connector_kind,
        evidence->order_property_kind, evidence->source_evidence_kind,
        evidence->source_kind_id,
        false);

    total_cost = evidence->selected_total_cost > 0 ?
        evidence->selected_total_cost : path->total_cost;
    if (evidence->required_outer != NULL)
        request.required_outer = evidence->required_outer;
    if (evidence->solved_relids != NULL)
        request.solved_relids = evidence->solved_relids;
    if (evidence->provided_relids != NULL)
        request.provided_relids = evidence->provided_relids;
    if (evidence->output_width > 0)
        request.output_width = evidence->output_width;
    request.parallel_safe = evidence->parallel_safe;
    request.parallel_aware = evidence->parallel_aware;
    request.parallel_workers = evidence->parallel_workers;
    request.gather_cost = evidence->gather_cost;
    request.order_preserving = evidence->order_preserving;
    request.shared_state_required = evidence->shared_state_required;
    request.pattern_key = evidence->pattern_key;
    request.total_cost = total_cost;
    request.rows = path->rows;

    return age_graph_join_table_add_candidate(table, &request);
}

const char *age_graph_join_component_name(
    AgeGraphJoinComponentKind component_kind)
{
    switch (component_kind)
    {
        case AGE_GRAPH_JOIN_COMPONENT_NODE_PROPERTY_SEEK:
            return "node-property-seek";
        case AGE_GRAPH_JOIN_COMPONENT_VLE_EXPANSION:
            return "vle-expansion";
        case AGE_GRAPH_JOIN_COMPONENT_ADJACENCY_EXPANSION:
            return "adjacency-expansion";
        case AGE_GRAPH_JOIN_COMPONENT_VALUE_JOIN:
            return "graph-value-join";
        case AGE_GRAPH_JOIN_COMPONENT_APPLY:
            return "graph-apply";
        case AGE_GRAPH_JOIN_COMPONENT_CARTESIAN:
            return "graph-cartesian";
        case AGE_GRAPH_JOIN_COMPONENT_UNKNOWN:
            break;
    }

    return "graph-component";
}

static AgeGraphJoinComponentKind graph_join_infer_component_family_kind(
    AgeGraphJoinConnectorKind connector_kind,
    AgeGraphJoinOrderPropertyKind order_kind)
{
    switch (connector_kind)
    {
        case AGE_GRAPH_JOIN_CONNECTOR_POSTGRES_INDEX_SEEK:
        case AGE_GRAPH_JOIN_CONNECTOR_NODE_PROPERTY_INDEX_SEEK:
            return AGE_GRAPH_JOIN_COMPONENT_NODE_PROPERTY_SEEK;
        case AGE_GRAPH_JOIN_CONNECTOR_VLE_EXPAND:
        case AGE_GRAPH_JOIN_CONNECTOR_VLE_BIDIRECTIONAL_EXPAND:
        case AGE_GRAPH_JOIN_CONNECTOR_VLE_COMPOSITE_EXPAND:
        case AGE_GRAPH_JOIN_CONNECTOR_VLE_EXPAND_INTO:
        case AGE_GRAPH_JOIN_CONNECTOR_VLE_COMPOSITE_EXPAND_INTO:
        case AGE_GRAPH_JOIN_CONNECTOR_MATRIX_FRONTIER_EXPAND:
            return AGE_GRAPH_JOIN_COMPONENT_VLE_EXPANSION;
        case AGE_GRAPH_JOIN_CONNECTOR_AGE_ADJACENCY:
        case AGE_GRAPH_JOIN_CONNECTOR_ADJACENCY_VALUE_JOIN:
        case AGE_GRAPH_JOIN_CONNECTOR_ADJACENCY_EXPAND:
        case AGE_GRAPH_JOIN_CONNECTOR_ADJACENCY_COMPOSITE_EXPAND:
            return AGE_GRAPH_JOIN_COMPONENT_ADJACENCY_EXPANSION;
        case AGE_GRAPH_JOIN_CONNECTOR_VALUE_JOIN:
            return AGE_GRAPH_JOIN_COMPONENT_VALUE_JOIN;
        case AGE_GRAPH_JOIN_CONNECTOR_APPLY:
            return AGE_GRAPH_JOIN_COMPONENT_APPLY;
        case AGE_GRAPH_JOIN_CONNECTOR_CARTESIAN:
            return AGE_GRAPH_JOIN_COMPONENT_CARTESIAN;
        case AGE_GRAPH_JOIN_CONNECTOR_UNKNOWN:
            break;
    }

    switch (order_kind)
    {
        case AGE_GRAPH_JOIN_ORDER_INDEX_ANCHORED:
            return AGE_GRAPH_JOIN_COMPONENT_NODE_PROPERTY_SEEK;
        case AGE_GRAPH_JOIN_ORDER_VLE_FRONTIER:
        case AGE_GRAPH_JOIN_ORDER_MATRIX_FRONTIER:
        case AGE_GRAPH_JOIN_ORDER_EXPAND_INTO:
            return AGE_GRAPH_JOIN_COMPONENT_VLE_EXPANSION;
        case AGE_GRAPH_JOIN_ORDER_ADJACENCY_DIRECTORY:
        case AGE_GRAPH_JOIN_ORDER_ADJACENCY:
            return AGE_GRAPH_JOIN_COMPONENT_ADJACENCY_EXPANSION;
        case AGE_GRAPH_JOIN_ORDER_VALUE:
            return AGE_GRAPH_JOIN_COMPONENT_VALUE_JOIN;
        case AGE_GRAPH_JOIN_ORDER_APPLY:
            return AGE_GRAPH_JOIN_COMPONENT_APPLY;
        case AGE_GRAPH_JOIN_ORDER_CARTESIAN:
            return AGE_GRAPH_JOIN_COMPONENT_CARTESIAN;
        case AGE_GRAPH_JOIN_ORDER_QUERY:
        case AGE_GRAPH_JOIN_ORDER_UNKNOWN:
            break;
    }

    return AGE_GRAPH_JOIN_COMPONENT_UNKNOWN;
}

static bool graph_join_source_kinds_match(
    AgeGraphJoinSourceKind existing, AgeGraphJoinSourceKind candidate)
{
    return existing != AGE_GRAPH_JOIN_SOURCE_UNKNOWN &&
           candidate != AGE_GRAPH_JOIN_SOURCE_UNKNOWN &&
           existing == candidate;
}

static bool graph_join_component_kinds_match(
    AgeGraphJoinComponentKind existing, AgeGraphJoinComponentKind candidate)
{
    return existing != AGE_GRAPH_JOIN_COMPONENT_UNKNOWN &&
        candidate != AGE_GRAPH_JOIN_COMPONENT_UNKNOWN &&
        existing == candidate;
}

static bool graph_join_order_property_kinds_match(
    AgeGraphJoinOrderPropertyKind existing,
    AgeGraphJoinOrderPropertyKind candidate)
{
    return existing != AGE_GRAPH_JOIN_ORDER_UNKNOWN &&
           candidate != AGE_GRAPH_JOIN_ORDER_UNKNOWN &&
           existing == candidate;
}

static bool graph_join_connector_kinds_match(
    AgeGraphJoinConnectorKind existing, AgeGraphJoinConnectorKind candidate)
{
    return existing != AGE_GRAPH_JOIN_CONNECTOR_UNKNOWN &&
           candidate != AGE_GRAPH_JOIN_CONNECTOR_UNKNOWN &&
           existing == candidate;
}

static bool graph_join_source_evidence_matches(
    AgeGraphJoinSourceEvidenceKind existing_kind,
    AgeGraphJoinSourceEvidenceKind candidate_kind)
{
    if (existing_kind == AGE_GRAPH_JOIN_SOURCE_EVIDENCE_UNKNOWN ||
        candidate_kind == AGE_GRAPH_JOIN_SOURCE_EVIDENCE_UNKNOWN)
        return false;

    return existing_kind == candidate_kind;
}

AgeGraphJoinCandidate *age_graph_join_table_select_cheapest(
    const AgeGraphJoinCandidateTable *table)
{
    AgeGraphJoinCandidate *selected = NULL;
    ListCell *lc;

    if (table == NULL)
        return NULL;

    foreach(lc, table->candidates)
    {
        AgeGraphJoinCandidate *candidate = lfirst(lc);

        if (selected == NULL ||
            candidate->connector.total_cost < selected->connector.total_cost)
        {
            selected = candidate;
        }
    }

    return selected;
}

bool age_graph_join_apply_selected_path_cost(
    const AgeGraphJoinCandidateTable *table, Path *path)
{
    AgeGraphJoinCandidate *selected;

    Assert(path != NULL);

    selected = age_graph_join_table_select_cheapest(table);
    if (selected == NULL)
        return false;

    path->rows = selected->connector.rows;
    path->startup_cost = selected->connector.startup_cost;
    path->total_cost = selected->connector.total_cost;

    return true;
}

const char *age_graph_join_connector_name(
    AgeGraphJoinConnectorKind connector_kind)
{
    switch (connector_kind)
    {
        case AGE_GRAPH_JOIN_CONNECTOR_POSTGRES_INDEX_SEEK:
            return "postgres-index-seek";
        case AGE_GRAPH_JOIN_CONNECTOR_NODE_PROPERTY_INDEX_SEEK:
            return "node-property-index-seek";
        case AGE_GRAPH_JOIN_CONNECTOR_VLE_EXPAND:
            return "vle-expand";
        case AGE_GRAPH_JOIN_CONNECTOR_VLE_BIDIRECTIONAL_EXPAND:
            return "vle-bidirectional-expand";
        case AGE_GRAPH_JOIN_CONNECTOR_VLE_COMPOSITE_EXPAND:
            return "vle-composite-expand";
        case AGE_GRAPH_JOIN_CONNECTOR_VLE_EXPAND_INTO:
            return "vle-expand-into";
        case AGE_GRAPH_JOIN_CONNECTOR_VLE_COMPOSITE_EXPAND_INTO:
            return "vle-composite-expand-into";
        case AGE_GRAPH_JOIN_CONNECTOR_MATRIX_FRONTIER_EXPAND:
            return "matrix-frontier-expand";
        case AGE_GRAPH_JOIN_CONNECTOR_AGE_ADJACENCY:
            return "age-adjacency";
        case AGE_GRAPH_JOIN_CONNECTOR_ADJACENCY_VALUE_JOIN:
            return "adjacency-value-join";
        case AGE_GRAPH_JOIN_CONNECTOR_ADJACENCY_EXPAND:
            return "adjacency-expand";
        case AGE_GRAPH_JOIN_CONNECTOR_ADJACENCY_COMPOSITE_EXPAND:
            return "adjacency-composite-expand";
        case AGE_GRAPH_JOIN_CONNECTOR_VALUE_JOIN:
            return "graph-value-join";
        case AGE_GRAPH_JOIN_CONNECTOR_APPLY:
            return "graph-apply";
        case AGE_GRAPH_JOIN_CONNECTOR_CARTESIAN:
            return "graph-cartesian";
        case AGE_GRAPH_JOIN_CONNECTOR_UNKNOWN:
            break;
    }

    return "graph-connector";
}

const char *age_graph_join_source_kind_name(
    AgeGraphJoinSourceKind source_kind_id)
{
    switch (source_kind_id)
    {
        case AGE_GRAPH_JOIN_SOURCE_NODE_PROPERTY_INDEX:
            return "node-property-index";
        case AGE_GRAPH_JOIN_SOURCE_VLE_MARKER:
            return "vle-marker";
        case AGE_GRAPH_JOIN_SOURCE_VLE_TERMINAL_PROPERTY:
            return "vle-terminal-property";
        case AGE_GRAPH_JOIN_SOURCE_ADJACENCY_EXPANSION:
            return "adjacency-expansion";
        case AGE_GRAPH_JOIN_SOURCE_ADJACENCY_NODE_PROPERTY:
            return "adjacency-node-property";
        case AGE_GRAPH_JOIN_SOURCE_VALUE_JOIN:
            return "graph-value-join";
        case AGE_GRAPH_JOIN_SOURCE_APPLY:
            return "graph-apply";
        case AGE_GRAPH_JOIN_SOURCE_CARTESIAN:
            return "graph-cartesian";
        case AGE_GRAPH_JOIN_SOURCE_UNKNOWN:
            break;
    }

    return "graph-source";
}

const char *age_graph_join_source_evidence_kind_name(
    AgeGraphJoinSourceEvidenceKind source_evidence_kind)
{
    switch (source_evidence_kind)
    {
        case AGE_GRAPH_JOIN_SOURCE_EVIDENCE_POSTGRES_INDEX:
            return "postgres-index-path";
        case AGE_GRAPH_JOIN_SOURCE_EVIDENCE_NODE_PROPERTY_INDEX:
            return "node-property-index";
        case AGE_GRAPH_JOIN_SOURCE_EVIDENCE_VLE_EDGE_SOURCE:
            return "vle-edge-source";
        case AGE_GRAPH_JOIN_SOURCE_EVIDENCE_VLE_MATRIX_FRONTIER:
            return "vle-matrix-frontier";
        case AGE_GRAPH_JOIN_SOURCE_EVIDENCE_ADJACENCY_DIRECTORY:
            return "adjacency-directory";
        case AGE_GRAPH_JOIN_SOURCE_EVIDENCE_ADJACENCY_VALUE:
            return "adjacency-value";
        case AGE_GRAPH_JOIN_SOURCE_EVIDENCE_ADJACENCY_PAYLOAD:
            return "adjacency-payload";
        case AGE_GRAPH_JOIN_SOURCE_EVIDENCE_GRAPH_VALUE:
            return "graph-value";
        case AGE_GRAPH_JOIN_SOURCE_EVIDENCE_GRAPH_APPLY:
            return "graph-apply";
        case AGE_GRAPH_JOIN_SOURCE_EVIDENCE_GRAPH_CARTESIAN:
            return "graph-cartesian";
        case AGE_GRAPH_JOIN_SOURCE_EVIDENCE_UNKNOWN:
            break;
    }

    return "unknown";
}

const char *age_graph_join_bound_kind_name(AgeGraphJoinBoundKind bound_kind)
{
    switch (bound_kind)
    {
        case AGE_GRAPH_JOIN_BOUND_UNBOUND:
            return "unbound";
        case AGE_GRAPH_JOIN_BOUND_BOUND:
            return "bound";
        case AGE_GRAPH_JOIN_BOUND_DECLARED_COVER:
            return "declared-cover";
        case AGE_GRAPH_JOIN_BOUND_VLE_OUT:
            return "out";
        case AGE_GRAPH_JOIN_BOUND_VLE_IN:
            return "in";
        case AGE_GRAPH_JOIN_BOUND_VLE_BOTH:
            return "both";
        case AGE_GRAPH_JOIN_BOUND_VLE_MIXED:
            return "mixed";
        case AGE_GRAPH_JOIN_BOUND_ADJACENCY_START:
            return "start-bound";
        case AGE_GRAPH_JOIN_BOUND_ADJACENCY_END:
            return "end-bound";
        case AGE_GRAPH_JOIN_BOUND_UNKNOWN:
            break;
    }

    return "unknown";
}

const char *age_graph_join_cover_match_kind_name(
    AgeGraphJoinCoverMatchKind cover_match_kind)
{
    switch (cover_match_kind)
    {
        case AGE_GRAPH_JOIN_COVER_MATCHED:
            return "matched";
        case AGE_GRAPH_JOIN_COVER_PATTERN_MISMATCH:
            return "pattern-mismatch";
        case AGE_GRAPH_JOIN_COVER_SOURCE_MISMATCH:
            return "source-mismatch";
        case AGE_GRAPH_JOIN_COVER_COMPONENT_MISMATCH:
            return "component-mismatch";
        case AGE_GRAPH_JOIN_COVER_UNKNOWN:
            break;
    }

    return "unknown";
}

const char *age_graph_join_order_property_name(
    AgeGraphJoinOrderPropertyKind order_property_kind)
{
    switch (order_property_kind)
    {
        case AGE_GRAPH_JOIN_ORDER_QUERY:
            return "query-order";
        case AGE_GRAPH_JOIN_ORDER_INDEX_ANCHORED:
            return "index-anchored";
        case AGE_GRAPH_JOIN_ORDER_VLE_FRONTIER:
            return "vle-frontier-anchored";
        case AGE_GRAPH_JOIN_ORDER_MATRIX_FRONTIER:
            return "matrix-frontier-anchored";
        case AGE_GRAPH_JOIN_ORDER_EXPAND_INTO:
            return "expand-into-verification";
        case AGE_GRAPH_JOIN_ORDER_ADJACENCY_DIRECTORY:
            return "adjacency-directory-anchored";
        case AGE_GRAPH_JOIN_ORDER_ADJACENCY:
            return "adjacency-anchored";
        case AGE_GRAPH_JOIN_ORDER_VALUE:
            return "value-anchored";
        case AGE_GRAPH_JOIN_ORDER_APPLY:
            return "apply-anchored";
        case AGE_GRAPH_JOIN_ORDER_CARTESIAN:
            return "cartesian";
        case AGE_GRAPH_JOIN_ORDER_UNKNOWN:
            break;
    }

    return "query-order";
}

bool age_graph_join_order_property_kind_is_bound(
    AgeGraphJoinOrderPropertyKind kind)
{
    switch (kind)
    {
        case AGE_GRAPH_JOIN_ORDER_INDEX_ANCHORED:
        case AGE_GRAPH_JOIN_ORDER_VLE_FRONTIER:
        case AGE_GRAPH_JOIN_ORDER_MATRIX_FRONTIER:
        case AGE_GRAPH_JOIN_ORDER_EXPAND_INTO:
        case AGE_GRAPH_JOIN_ORDER_ADJACENCY_DIRECTORY:
        case AGE_GRAPH_JOIN_ORDER_ADJACENCY:
        case AGE_GRAPH_JOIN_ORDER_VALUE:
        case AGE_GRAPH_JOIN_ORDER_APPLY:
            return true;
        case AGE_GRAPH_JOIN_ORDER_QUERY:
        case AGE_GRAPH_JOIN_ORDER_CARTESIAN:
        case AGE_GRAPH_JOIN_ORDER_UNKNOWN:
            return false;
    }

    return false;
}

void age_graph_join_init_path_evidence(
    AgeGraphJoinPathEvidence *evidence)
{
    Assert(evidence != NULL);

    memset(evidence, 0, sizeof(*evidence));
    evidence->candidate_count = 1;
    evidence->cover_match_kind = AGE_GRAPH_JOIN_COVER_UNKNOWN;
}

void age_graph_join_complete_path_evidence(
    Path *path, AgeGraphJoinPathEvidence *evidence)
{
    RelOptInfo *rel;

    if (path == NULL || evidence == NULL)
        return;

    if (evidence->component_kind == AGE_GRAPH_JOIN_COMPONENT_UNKNOWN)
    {
        evidence->component_kind = graph_join_infer_component_family_kind(
            evidence->connector_kind, evidence->order_property_kind);
    }
    rel = path->parent;
    if (evidence->solved_relids == NULL && rel != NULL)
        evidence->solved_relids = rel->relids;
    if (evidence->required_outer == NULL)
        evidence->required_outer = PATH_REQ_OUTER(path);
    if (evidence->provided_relids == NULL && rel != NULL)
        evidence->provided_relids = rel->relids;
    if (evidence->output_width <= 0)
    {
        if (path->pathtarget != NULL && path->pathtarget->width > 0)
            evidence->output_width = path->pathtarget->width;
        else if (rel != NULL && rel->reltarget != NULL)
            evidence->output_width = rel->reltarget->width;
    }
    if (!evidence->physical_properties_known)
    {
        evidence->parallel_safe = path->parallel_safe;
        evidence->parallel_aware = path->parallel_aware;
        evidence->parallel_workers = path->parallel_workers;
        evidence->order_preserving = path->pathkeys != NIL;
        evidence->physical_properties_known = true;
    }
}

void age_graph_join_metadata_begin(PlannerInfo *root)
{
    if (root == NULL)
        return;

    if (graph_join_metadata_identity_matches(root))
        return;

    graph_join_metadata_root = root;
    graph_join_metadata_glob = root->glob;
    graph_join_metadata_parse = root->parse;
    graph_join_metadata_context = root->planner_cxt;
    graph_join_metadata_query_level = root->query_level;
    graph_join_rel_metadata = NIL;
    if (graph_join_metadata_context != NULL)
    {
        MemoryContextCallback *context_callback;

        context_callback = MemoryContextAlloc(graph_join_metadata_context,
                                              sizeof(*context_callback));
        context_callback->func = graph_join_metadata_context_reset;
        context_callback->arg = graph_join_metadata_context;
        MemoryContextRegisterResetCallback(
            graph_join_metadata_context, context_callback);
    }
}

bool age_graph_join_metadata_matches_root(PlannerInfo *root)
{
    return graph_join_metadata_identity_matches(root);
}

static void graph_join_metadata_context_reset(void *arg)
{
    if (arg != graph_join_metadata_context)
        return;

    graph_join_metadata_root = NULL;
    graph_join_metadata_glob = NULL;
    graph_join_metadata_parse = NULL;
    graph_join_metadata_context = NULL;
    graph_join_metadata_query_level = 0;
    graph_join_rel_metadata = NIL;
}

static bool graph_join_metadata_identity_matches(PlannerInfo *root)
{
    return root != NULL &&
           graph_join_metadata_root == root &&
           graph_join_metadata_glob == root->glob &&
           graph_join_metadata_parse == root->parse &&
           graph_join_metadata_context == root->planner_cxt &&
           graph_join_metadata_query_level == root->query_level;
}

AgeGraphJoinRelMetadata *age_graph_join_get_rel_metadata(RelOptInfo *rel,
                                                         bool create)
{
    ListCell *lc;
    AgeGraphJoinRelMetadata *metadata;

    if (rel == NULL)
        return NULL;

    foreach(lc, graph_join_rel_metadata)
    {
        metadata = lfirst(lc);

        if (metadata->rel == rel)
            return metadata;
    }

    if (!create)
        return NULL;

    metadata = palloc0(sizeof(*metadata));
    metadata->rel = rel;
    graph_join_rel_metadata = lappend(graph_join_rel_metadata, metadata);

    return metadata;
}

void age_graph_join_refresh_rel_metadata(
    PlannerInfo *root, RelOptInfo *rel,
    AgeGraphJoinPathEvidenceCallback evidence_callback)
{
    AgeGraphJoinRelMetadata *metadata;
    ListCell *lc;

    if (rel == NULL)
        return;

    if (!age_graph_join_metadata_matches_root(root))
        age_graph_join_metadata_begin(root);
    metadata = age_graph_join_get_rel_metadata(rel, true);
    metadata->candidates = NIL;

    foreach(lc, rel->pathlist)
    {
        Path *path = lfirst(lc);
        AgeGraphJoinRelCandidate *candidate;
        AgeGraphJoinPathEvidence evidence;
        bool has_evidence = false;

        age_graph_join_init_path_evidence(&evidence);
        if (evidence_callback != NULL)
            has_evidence = evidence_callback(path, &evidence);
        if (!has_evidence)
            has_evidence = graph_join_metadata_lookup_path_evidence(
                metadata, path, &evidence);

        candidate = palloc0(sizeof(*candidate));
        candidate->path = path;
        if (has_evidence)
        {
            age_graph_join_complete_path_evidence(path, &evidence);
            graph_join_merge_lowering_pool_evidence(metadata, &evidence);
            candidate->evidence = evidence;
        }
        else
        {
            age_graph_join_init_path_evidence(&candidate->evidence);
            age_graph_join_complete_path_evidence(path,
                                                  &candidate->evidence);
        }
        metadata->candidates = lappend(metadata->candidates, candidate);
    }
    graph_join_metadata_append_lowering_candidates(metadata);

    if (metadata->candidates != NIL)
    {
        graph_join_metadata_rebuild_component_candidates(metadata);
        ereport(DEBUG2,
                (errmsg_internal("AGE graph join rel metadata refreshed: "
                                 "relids=%s candidates=%d components=%d "
                                 "lowering=%d artifacts=%d pool=%d paths=%d",
                                 bmsToString(rel->relids),
                                 list_length(metadata->candidates),
                                 list_length(metadata->component_candidates),
                                 graph_join_metadata_lowering_candidate_count(
                                     metadata),
                                 graph_join_metadata_lowering_artifact_entry_count(
                                     metadata),
                                 list_length(metadata->lowering_pool),
                                 list_length(rel->pathlist))));
    }
}

static void graph_join_metadata_append_lowering_candidates(
    AgeGraphJoinRelMetadata *metadata)
{
    ListCell *lowering_lc;

    if (metadata == NULL ||
        metadata->lowering_candidates == NIL)
    {
        return;
    }

    foreach(lowering_lc, metadata->lowering_candidates)
    {
        AgeGraphJoinRelCandidate *lowering_candidate = lfirst(lowering_lc);
        ListCell *candidate_lc;
        bool already_present = false;

        foreach(candidate_lc, metadata->candidates)
        {
            AgeGraphJoinRelCandidate *candidate = lfirst(candidate_lc);

            if (graph_join_planner_candidate_matches(
                    candidate, lowering_candidate->path,
                    &lowering_candidate->evidence))
            {
                graph_join_metadata_merge_lowering_candidate(
                    candidate, lowering_candidate);
                already_present = true;
                break;
            }
        }
        if (!already_present)
            metadata->candidates = lappend(metadata->candidates,
                                           lowering_candidate);
    }
}

static void graph_join_metadata_merge_lowering_candidate(
    AgeGraphJoinRelCandidate *candidate,
    const AgeGraphJoinRelCandidate *lowering_candidate)
{
    if (candidate == NULL || lowering_candidate == NULL)
        return;

    if (lowering_candidate->lowering_input)
    {
        candidate->evidence = lowering_candidate->evidence;
        candidate->lowering_input = true;
    }
}

static int graph_join_metadata_lowering_candidate_count(
    const AgeGraphJoinRelMetadata *metadata)
{
    ListCell *lc;
    int count = 0;

    if (metadata == NULL)
        return 0;

    foreach(lc, metadata->candidates)
    {
        AgeGraphJoinRelCandidate *candidate = lfirst(lc);

        if (candidate != NULL && candidate->lowering_input)
            count++;
    }

    return count;
}

static int graph_join_metadata_lowering_artifact_entry_count(
    const AgeGraphJoinRelMetadata *metadata)
{
    ListCell *lc;
    int count = 0;

    if (metadata == NULL)
        return 0;

    foreach(lc, metadata->lowering_artifacts)
    {
        AgeGraphJoinLoweringArtifact *artifact = lfirst(lc);

        if (artifact != NULL)
            count += list_length(artifact->entries);
    }

    return count;
}

static AgeGraphJoinLoweringArtifact *graph_join_metadata_merge_lowering_artifact(
    AgeGraphJoinRelMetadata *metadata,
    const AgeGraphJoinLoweringArtifact *artifact)
{
    AgeGraphJoinLoweringArtifact *shared_artifact;
    ListCell *lc;

    if (metadata == NULL ||
        artifact == NULL ||
        artifact->entries == NIL)
    {
        return NULL;
    }

    shared_artifact = graph_join_metadata_find_lowering_artifact(
        metadata, artifact->pattern_key);
    if (shared_artifact == NULL)
    {
        shared_artifact = age_graph_join_make_lowering_artifact(
            artifact->pattern_key, NULL);
        metadata->lowering_artifacts = lappend(metadata->lowering_artifacts,
                                               shared_artifact);
    }

    foreach(lc, artifact->entries)
    {
        AgeGraphJoinLoweringArtifactEntry *entry = lfirst(lc);

        if (entry == NULL)
            continue;
        if (entry->table != NULL)
            age_graph_join_lowering_artifact_add_typed_table(
                shared_artifact,
                entry->source_kind_id,
                entry->component_family_kind, entry->table);
        else
            age_graph_join_lowering_artifact_declare_typed_entry(
                shared_artifact,
                entry->source_kind_id,
                entry->component_family_kind);
    }

    return shared_artifact;
}

static AgeGraphJoinLoweringArtifact *graph_join_metadata_find_lowering_artifact(
    AgeGraphJoinRelMetadata *metadata, const char *pattern_key)
{
    ListCell *lc;

    if (metadata == NULL ||
        pattern_key == NULL)
    {
        return NULL;
    }

    foreach(lc, metadata->lowering_artifacts)
    {
        AgeGraphJoinLoweringArtifact *artifact = lfirst(lc);

        if (artifact != NULL &&
            artifact->pattern_key != NULL &&
            strcmp(artifact->pattern_key, pattern_key) == 0)
        {
            return artifact;
        }
    }

    return NULL;
}

static void graph_join_metadata_rebuild_component_candidates(
    AgeGraphJoinRelMetadata *metadata)
{
    ListCell *lc;

    if (metadata == NULL)
        return;

    metadata->component_candidates = NIL;
    graph_join_metadata_append_pool_component_candidates(metadata);
    foreach(lc, metadata->candidates)
    {
        AgeGraphJoinRelCandidate *candidate = lfirst(lc);

        graph_join_metadata_update_component_candidate(metadata, candidate);
    }
}

static void graph_join_metadata_append_pool_component_candidates(
    AgeGraphJoinRelMetadata *metadata)
{
    ListCell *lc;

    if (metadata == NULL ||
        metadata->lowering_pool == NIL)
    {
        return;
    }

    foreach(lc, metadata->lowering_pool)
    {
        AgeGraphJoinRelCandidate *pool_candidate = lfirst(lc);
        AgeGraphJoinRelCandidate *carrier_candidate;
        AgeGraphJoinRelCandidate component_seed;

        if (pool_candidate == NULL ||
            pool_candidate->path != NULL ||
            pool_candidate->evidence.order_property_kind ==
            AGE_GRAPH_JOIN_ORDER_UNKNOWN)
        {
            continue;
        }

        carrier_candidate = graph_join_metadata_pool_carrier_candidate(
            metadata, &pool_candidate->evidence);
        if (carrier_candidate == NULL)
            continue;

        component_seed = *pool_candidate;
        component_seed.path = carrier_candidate->path;
        component_seed.lowering_input = true;
        graph_join_metadata_update_component_candidate(metadata,
                                                       &component_seed);
    }
}

static AgeGraphJoinRelCandidate *graph_join_metadata_pool_carrier_candidate(
    AgeGraphJoinRelMetadata *metadata,
    const AgeGraphJoinPathEvidence *evidence)
{
    AgeGraphJoinRelCandidate *selected = NULL;
    Cost selected_cost = 0;
    ListCell *lc;

    if (metadata == NULL ||
        evidence == NULL ||
        evidence->order_property_kind == AGE_GRAPH_JOIN_ORDER_UNKNOWN)
    {
        return NULL;
    }

    foreach(lc, metadata->candidates)
    {
        AgeGraphJoinRelCandidate *candidate = lfirst(lc);
        Cost candidate_cost;

        if (candidate == NULL ||
            candidate->path == NULL)
        {
            continue;
        }

        if (graph_join_lowering_candidate_matches(candidate, evidence))
            return candidate;
        if (!graph_join_pool_evidence_can_use_carrier(candidate, evidence))
            continue;

        candidate_cost = graph_join_evidence_component_total_cost(candidate);
        if (selected == NULL || candidate_cost < selected_cost)
        {
            selected = candidate;
            selected_cost = candidate_cost;
        }
    }

    return selected;
}

static bool graph_join_pool_evidence_can_use_carrier(
    const AgeGraphJoinRelCandidate *carrier,
    const AgeGraphJoinPathEvidence *evidence)
{
    const AgeGraphJoinPathEvidence *carrier_evidence;

    if (carrier == NULL ||
        carrier->path == NULL ||
        evidence == NULL ||
        evidence->order_property_kind == AGE_GRAPH_JOIN_ORDER_UNKNOWN)
    {
        return false;
    }

    carrier_evidence = &carrier->evidence;
    if (!graph_join_component_kinds_match(carrier_evidence->component_kind,
                                          evidence->component_kind))
    {
        return false;
    }
    if (!graph_join_text_matches(carrier_evidence->pattern_key,
                                 evidence->pattern_key))
    {
        return false;
    }
    if (carrier_evidence->provided_relids != NULL &&
        evidence->provided_relids != NULL &&
        !bms_is_subset(evidence->provided_relids,
                       carrier_evidence->provided_relids))
    {
        return false;
    }
    if (carrier_evidence->solved_relids != NULL &&
        evidence->solved_relids != NULL &&
        !bms_is_subset(evidence->solved_relids,
                       carrier_evidence->solved_relids))
    {
        return false;
    }

    return true;
}

static void graph_join_metadata_update_component_candidate(
    AgeGraphJoinRelMetadata *metadata,
    const AgeGraphJoinRelCandidate *candidate)
{
    Cost total_cost;
    ListCell *lc;
    AgeGraphJoinRelComponentCandidate *component_candidate;

    if (metadata == NULL ||
        candidate == NULL ||
        candidate->path == NULL ||
        candidate->evidence.order_property_kind == AGE_GRAPH_JOIN_ORDER_UNKNOWN ||
        candidate->evidence.component_kind == AGE_GRAPH_JOIN_COMPONENT_UNKNOWN)
    {
        return;
    }

    total_cost = graph_join_evidence_component_total_cost(candidate);

    foreach(lc, metadata->component_candidates)
    {
        component_candidate = lfirst(lc);

        if (graph_join_component_candidate_matches(
                component_candidate, &candidate->evidence))
        {
            if (graph_join_component_candidate_keeps_existing(
                    component_candidate, candidate, total_cost))
            {
                return;
            }

            component_candidate->path = candidate->path;
            component_candidate->evidence = candidate->evidence;
            component_candidate->component_kind =
                candidate->evidence.component_kind;
            component_candidate->total_cost = total_cost;
            component_candidate->lowering_input = candidate->lowering_input;
            return;
        }
    }

    component_candidate = palloc0(sizeof(*component_candidate));
    component_candidate->path = candidate->path;
    component_candidate->evidence = candidate->evidence;
    component_candidate->component_kind = candidate->evidence.component_kind;
    component_candidate->total_cost = total_cost;
    component_candidate->lowering_input = candidate->lowering_input;
    metadata->component_candidates = lappend(metadata->component_candidates,
                                             component_candidate);
}

static bool graph_join_component_candidate_keeps_existing(
    const AgeGraphJoinRelComponentCandidate *component_candidate,
    const AgeGraphJoinRelCandidate *candidate, Cost total_cost)
{
    if (component_candidate == NULL || candidate == NULL)
        return true;

    if (component_candidate->lowering_input && !candidate->lowering_input &&
        component_candidate->total_cost <= total_cost)
    {
        return true;
    }
    if (!component_candidate->lowering_input && candidate->lowering_input &&
        total_cost <= component_candidate->total_cost)
    {
        return false;
    }

    return component_candidate->total_cost <= total_cost;
}

static Cost graph_join_evidence_component_total_cost(
    const AgeGraphJoinRelCandidate *candidate)
{
    Cost startup_cost;
    Cost total_cost;

    if (candidate == NULL ||
        candidate->path == NULL)
    {
        return 0;
    }

    startup_cost = candidate->path->startup_cost;
    total_cost = candidate->evidence.selected_total_cost > 0 ?
        candidate->evidence.selected_total_cost :
        candidate->path->total_cost;

    return age_graph_join_apply_evidence_alternative_credit(
        startup_cost, total_cost,
        age_graph_join_evidence_alternative_credit(&candidate->evidence));
}

static bool graph_join_component_candidate_matches(
    const AgeGraphJoinRelComponentCandidate *component_candidate,
    const AgeGraphJoinPathEvidence *evidence)
{
    const AgeGraphJoinPathEvidence *existing;

    if (component_candidate == NULL ||
        evidence == NULL)
    {
        return false;
    }
    if (!graph_join_component_kinds_match(
            component_candidate->component_kind,
            evidence->component_kind))
    {
        return false;
    }

    existing = &component_candidate->evidence;

    return bms_equal(existing->solved_relids, evidence->solved_relids) &&
           bms_equal(existing->provided_relids, evidence->provided_relids) &&
           bms_equal(existing->required_outer, evidence->required_outer) &&
           graph_join_component_connector_properties_match(existing,
                                                           evidence) &&
           graph_join_component_physical_properties_match(existing,
                                                          evidence);
}

static bool graph_join_component_physical_properties_match(
    const AgeGraphJoinPathEvidence *existing,
    const AgeGraphJoinPathEvidence *candidate)
{
    if (existing == NULL || candidate == NULL)
        return false;

    return existing->parallel_safe == candidate->parallel_safe &&
           existing->parallel_aware == candidate->parallel_aware &&
           existing->parallel_workers == candidate->parallel_workers &&
           existing->gather_cost == candidate->gather_cost &&
           existing->output_width == candidate->output_width &&
           existing->order_preserving == candidate->order_preserving &&
           existing->shared_state_required ==
           candidate->shared_state_required;
}

static bool graph_join_component_connector_properties_match(
    const AgeGraphJoinPathEvidence *existing,
    const AgeGraphJoinPathEvidence *candidate)
{
    if (existing == NULL || candidate == NULL)
        return false;

    if (!graph_join_connector_kinds_match(existing->connector_kind,
                                          candidate->connector_kind))
        return false;
    if (!graph_join_order_property_kinds_match(
            existing->order_property_kind,
            candidate->order_property_kind))
    {
        return false;
    }

    if (!graph_join_source_evidence_matches(existing->source_evidence_kind,
                                            candidate->source_evidence_kind))
        return false;

    return
           graph_join_text_matches(existing->pattern_key,
                                   candidate->pattern_key) &&
           graph_join_source_kinds_match(existing->source_kind_id,
                                         candidate->source_kind_id);
}

static bool graph_join_text_matches(const char *existing,
                                    const char *candidate)
{
    if (existing == NULL || candidate == NULL)
        return existing == candidate;

    return strcmp(existing, candidate) == 0;
}

static AgeGraphJoinLoweringArtifactEntry *
graph_join_lowering_artifact_find_entry(
    AgeGraphJoinLoweringArtifact *artifact,
    AgeGraphJoinSourceKind source_kind_id, bool require_empty_table)
{
    ListCell *lc;

    if (artifact == NULL ||
        source_kind_id == AGE_GRAPH_JOIN_SOURCE_UNKNOWN)
    {
        return NULL;
    }

    foreach(lc, artifact->entries)
    {
        AgeGraphJoinLoweringArtifactEntry *entry = lfirst(lc);

        if (entry == NULL ||
            !graph_join_source_kinds_match(entry->source_kind_id,
                                           source_kind_id))
        {
            continue;
        }
        if (require_empty_table && entry->table != NULL)
            continue;
        return entry;
    }

    return NULL;
}

static AgeGraphJoinCandidateTable *graph_join_make_declared_entry_table(
    RelOptInfo *rel, const AgeGraphJoinLoweringArtifact *artifact,
    const AgeGraphJoinLoweringArtifactEntry *entry)
{
    AgeGraphJoinCandidateTable *table;
    AgeGraphJoinCandidateRequest request;
    double rows = 1.0;
    int output_width = 0;

    if (rel == NULL ||
        artifact == NULL ||
        entry == NULL)
    {
        return NULL;
    }

    table = age_graph_join_make_candidate_table();
    table->declared_entry_count = Max(1, list_length(artifact->entries));
    table->cover_match_kind = AGE_GRAPH_JOIN_COVER_MATCHED;
    table->pattern_key = copy_graph_join_text(artifact->pattern_key,
                                               "graph-pattern");
    table->source_kind_id = entry->source_kind_id;
    table->component_family_kind = entry->component_family_kind;

    if (rel->rows > 0)
        rows = rel->rows;
    if (rel->reltarget != NULL && rel->reltarget->width > 0)
        output_width = rel->reltarget->width;

    memset(&request, 0, sizeof(request));
    request.component_family_kind = entry->component_family_kind;
    request.connector_kind =
        graph_join_declared_connector_kind(entry->source_kind_id);
    request.bound_kind = AGE_GRAPH_JOIN_BOUND_DECLARED_COVER;
    request.order_property_kind =
        graph_join_declared_order_property_kind(entry->source_kind_id);
    request.source_evidence_kind =
        graph_join_declared_source_evidence_kind(entry->source_kind_id);
    request.pattern_key = artifact->pattern_key;
    request.source_kind_id = entry->source_kind_id;
    request.solved_relids = rel->relids;
    request.provided_relids = rel->relids;
    request.rows = rows;
    request.startup_cost = 0;
    request.total_cost = rows;
    request.output_width = output_width;
    request.parallel_safe = rel->consider_parallel;
    request.parallel_aware = false;
    request.parallel_workers = 0;
    request.gather_cost = 0;
    request.order_preserving = false;
    request.shared_state_required = false;

    (void) age_graph_join_table_add_candidate(table, &request);

    return table;
}

static AgeGraphJoinConnectorKind graph_join_declared_connector_kind(
    AgeGraphJoinSourceKind source_kind_id)
{
    switch (source_kind_id)
    {
        case AGE_GRAPH_JOIN_SOURCE_NODE_PROPERTY_INDEX:
            return AGE_GRAPH_JOIN_CONNECTOR_NODE_PROPERTY_INDEX_SEEK;
        case AGE_GRAPH_JOIN_SOURCE_VLE_MARKER:
            return AGE_GRAPH_JOIN_CONNECTOR_VLE_EXPAND;
        case AGE_GRAPH_JOIN_SOURCE_VLE_TERMINAL_PROPERTY:
            return AGE_GRAPH_JOIN_CONNECTOR_VLE_COMPOSITE_EXPAND;
        case AGE_GRAPH_JOIN_SOURCE_ADJACENCY_EXPANSION:
            return AGE_GRAPH_JOIN_CONNECTOR_ADJACENCY_EXPAND;
        case AGE_GRAPH_JOIN_SOURCE_ADJACENCY_NODE_PROPERTY:
            return AGE_GRAPH_JOIN_CONNECTOR_ADJACENCY_VALUE_JOIN;
        case AGE_GRAPH_JOIN_SOURCE_VALUE_JOIN:
            return AGE_GRAPH_JOIN_CONNECTOR_VALUE_JOIN;
        case AGE_GRAPH_JOIN_SOURCE_APPLY:
            return AGE_GRAPH_JOIN_CONNECTOR_APPLY;
        case AGE_GRAPH_JOIN_SOURCE_CARTESIAN:
            return AGE_GRAPH_JOIN_CONNECTOR_CARTESIAN;
        case AGE_GRAPH_JOIN_SOURCE_UNKNOWN:
            break;
    }

    return AGE_GRAPH_JOIN_CONNECTOR_UNKNOWN;
}

static AgeGraphJoinOrderPropertyKind graph_join_declared_order_property_kind(
    AgeGraphJoinSourceKind source_kind_id)
{
    switch (source_kind_id)
    {
        case AGE_GRAPH_JOIN_SOURCE_NODE_PROPERTY_INDEX:
        case AGE_GRAPH_JOIN_SOURCE_ADJACENCY_NODE_PROPERTY:
            return AGE_GRAPH_JOIN_ORDER_INDEX_ANCHORED;
        case AGE_GRAPH_JOIN_SOURCE_VLE_MARKER:
        case AGE_GRAPH_JOIN_SOURCE_VLE_TERMINAL_PROPERTY:
            return AGE_GRAPH_JOIN_ORDER_VLE_FRONTIER;
        case AGE_GRAPH_JOIN_SOURCE_ADJACENCY_EXPANSION:
            return AGE_GRAPH_JOIN_ORDER_ADJACENCY_DIRECTORY;
        case AGE_GRAPH_JOIN_SOURCE_VALUE_JOIN:
            return AGE_GRAPH_JOIN_ORDER_VALUE;
        case AGE_GRAPH_JOIN_SOURCE_APPLY:
            return AGE_GRAPH_JOIN_ORDER_APPLY;
        case AGE_GRAPH_JOIN_SOURCE_CARTESIAN:
            return AGE_GRAPH_JOIN_ORDER_CARTESIAN;
        case AGE_GRAPH_JOIN_SOURCE_UNKNOWN:
            break;
    }

    return AGE_GRAPH_JOIN_ORDER_QUERY;
}

static AgeGraphJoinSourceEvidenceKind
graph_join_declared_source_evidence_kind(AgeGraphJoinSourceKind source_kind_id)
{
    switch (source_kind_id)
    {
        case AGE_GRAPH_JOIN_SOURCE_NODE_PROPERTY_INDEX:
            return AGE_GRAPH_JOIN_SOURCE_EVIDENCE_NODE_PROPERTY_INDEX;
        case AGE_GRAPH_JOIN_SOURCE_VLE_MARKER:
        case AGE_GRAPH_JOIN_SOURCE_VLE_TERMINAL_PROPERTY:
            return AGE_GRAPH_JOIN_SOURCE_EVIDENCE_VLE_EDGE_SOURCE;
        case AGE_GRAPH_JOIN_SOURCE_ADJACENCY_EXPANSION:
            return AGE_GRAPH_JOIN_SOURCE_EVIDENCE_ADJACENCY_DIRECTORY;
        case AGE_GRAPH_JOIN_SOURCE_ADJACENCY_NODE_PROPERTY:
            return AGE_GRAPH_JOIN_SOURCE_EVIDENCE_ADJACENCY_VALUE;
        case AGE_GRAPH_JOIN_SOURCE_VALUE_JOIN:
            return AGE_GRAPH_JOIN_SOURCE_EVIDENCE_GRAPH_VALUE;
        case AGE_GRAPH_JOIN_SOURCE_APPLY:
            return AGE_GRAPH_JOIN_SOURCE_EVIDENCE_GRAPH_APPLY;
        case AGE_GRAPH_JOIN_SOURCE_CARTESIAN:
            return AGE_GRAPH_JOIN_SOURCE_EVIDENCE_GRAPH_CARTESIAN;
        case AGE_GRAPH_JOIN_SOURCE_UNKNOWN:
            break;
    }

    return AGE_GRAPH_JOIN_SOURCE_EVIDENCE_UNKNOWN;
}

void age_graph_join_register_rel_candidate_table(
    PlannerInfo *root, RelOptInfo *rel, Path *path,
    const AgeGraphJoinCandidateTable *table)
{
    age_graph_join_register_rel_lowering_table(root, rel, path, table);
}

void age_graph_join_register_rel_lowering_pool(
    PlannerInfo *root, RelOptInfo *rel,
    const AgeGraphJoinCandidateTable *table)
{
    AgeGraphJoinRelMetadata *metadata;
    ListCell *lc;

    if (root == NULL ||
        rel == NULL ||
        table == NULL ||
        table->candidates == NIL)
    {
        return;
    }

    if (!age_graph_join_metadata_matches_root(root))
        age_graph_join_metadata_begin(root);
    metadata = age_graph_join_get_rel_metadata(rel, true);

    foreach(lc, table->candidates)
    {
        AgeGraphJoinCandidate *graph_candidate = lfirst(lc);
        AgeGraphJoinRelCandidate *pool_candidate;
        ListCell *candidate_lc;
        bool replaced = false;

        pool_candidate = palloc0(sizeof(*pool_candidate));
        pool_candidate->path = NULL;
        pool_candidate->lowering_input = true;
        graph_join_path_evidence_from_candidate(NULL, table, graph_candidate,
                                                &pool_candidate->evidence);

        foreach(candidate_lc, metadata->lowering_pool)
        {
            AgeGraphJoinRelCandidate *existing = lfirst(candidate_lc);

            if (graph_join_lowering_candidate_matches(
                    existing, &pool_candidate->evidence))
            {
                existing->evidence = pool_candidate->evidence;
                existing->lowering_input = true;
                replaced = true;
                break;
            }
        }
        if (!replaced)
            metadata->lowering_pool = lappend(metadata->lowering_pool,
                                              pool_candidate);
    }
}

AgeGraphJoinLoweringArtifact *age_graph_join_register_rel_lowering_artifact_pool(
    PlannerInfo *root, RelOptInfo *rel,
    const AgeGraphJoinLoweringArtifact *artifact)
{
    AgeGraphJoinRelMetadata *metadata;
    AgeGraphJoinLoweringArtifact *shared_artifact;
    ListCell *lc;

    if (root == NULL ||
        rel == NULL ||
        artifact == NULL ||
        artifact->entries == NIL)
    {
        return NULL;
    }

    if (!age_graph_join_metadata_matches_root(root))
        age_graph_join_metadata_begin(root);
    metadata = age_graph_join_get_rel_metadata(rel, true);
    shared_artifact = graph_join_metadata_merge_lowering_artifact(metadata,
                                                                  artifact);

    foreach(lc, shared_artifact->entries)
    {
        AgeGraphJoinLoweringArtifactEntry *entry = lfirst(lc);
        AgeGraphJoinCandidateTable *declared_table = NULL;

        if (entry == NULL)
            continue;
        if (entry->table != NULL && entry->table->candidates != NIL)
            age_graph_join_register_rel_lowering_pool(root, rel, entry->table);
        else
        {
            declared_table = graph_join_make_declared_entry_table(
                rel, shared_artifact, entry);
            age_graph_join_register_rel_lowering_pool(root, rel,
                                                     declared_table);
        }
    }

    return shared_artifact;
}

AgeGraphJoinCandidateTable *age_graph_join_bind_lowering_pool_to_path(
    Path *path, const AgeGraphJoinCandidateTable *table)
{
    AgeGraphJoinCandidateTable *bound_table;
    ListCell *lc;

    if (path == NULL ||
        table == NULL ||
        table->candidates == NIL)
    {
        return NULL;
    }

    bound_table = age_graph_join_make_candidate_table();
    bound_table->declared_entry_count = table->declared_entry_count;
    bound_table->cover_match_kind = table->cover_match_kind;
    foreach(lc, table->candidates)
    {
        AgeGraphJoinCandidate *candidate = lfirst(lc);
        AgeGraphJoinCandidateRequest request;
        Cost run_cost;

        memset(&request, 0, sizeof(request));
        request.display_name = candidate->component.display_name;
        request.connector_kind = candidate->connector.kind_id;
        request.bound_kind = candidate->connector.bound_kind;
        request.order_property_kind = candidate->connector.order_property_kind;
        request.source_evidence_kind =
            candidate->connector.source_evidence_kind;
        request.pattern_key = candidate->pattern_key;
        request.source_kind_id = candidate->source_kind_id;
        request.component_family_kind = candidate->component.family_kind;
        request.solved_relids = candidate->connector.solved_relids;
        request.required_outer = candidate->connector.required_outer;
        request.provided_relids = candidate->connector.provided_relids;
        request.rows = candidate->connector.rows;
        request.startup_cost = path->startup_cost;
        run_cost = Max(candidate->connector.total_cost -
                       candidate->connector.startup_cost, 0.0);
        request.total_cost = path->startup_cost + run_cost;
        request.output_width = path->pathtarget != NULL &&
            path->pathtarget->width > 0 ?
            path->pathtarget->width : candidate->component.output_width;
        request.parallel_safe = path->parallel_safe;
        request.parallel_aware = path->parallel_aware;
        request.parallel_workers = path->parallel_workers;
        request.gather_cost = 0;
        request.order_preserving = path->pathkeys != NIL;
        request.shared_state_required =
            candidate->component.shared_state_required;

        (void) age_graph_join_table_add_candidate(bound_table, &request);
    }

    return bound_table;
}

AgeGraphJoinCandidateTable *age_graph_join_bind_lowering_artifact_to_path(
    Path *path, const AgeGraphJoinLoweringArtifact *artifact)
{
    AgeGraphJoinCandidateTable *bound_table;
    ListCell *lc;

    if (path == NULL ||
        artifact == NULL ||
        artifact->entries == NIL)
    {
        return NULL;
    }

    bound_table = age_graph_join_make_candidate_table();
    bound_table->declared_entry_count = list_length(artifact->entries);
    bound_table->cover_match_kind = AGE_GRAPH_JOIN_COVER_MATCHED;
    foreach(lc, artifact->entries)
    {
        AgeGraphJoinLoweringArtifactEntry *entry = lfirst(lc);
        AgeGraphJoinCandidateTable *entry_table;

        entry_table = age_graph_join_bind_lowering_pool_to_path(path,
                                                                entry->table);
        if (entry_table != NULL)
            bound_table->candidates = list_concat(bound_table->candidates,
                                                  entry_table->candidates);
    }

    if (bound_table->candidates == NIL)
        return NULL;

    return bound_table;
}

void age_graph_join_register_rel_lowering_table(
    PlannerInfo *root, RelOptInfo *rel, Path *path,
    const AgeGraphJoinCandidateTable *table)
{
    AgeGraphJoinRelMetadata *metadata;
    ListCell *lc;

    if (root == NULL ||
        rel == NULL ||
        path == NULL ||
        table == NULL ||
        table->candidates == NIL)
    {
        return;
    }

    if (!age_graph_join_metadata_matches_root(root))
        age_graph_join_metadata_begin(root);
    metadata = age_graph_join_get_rel_metadata(rel, true);

    foreach(lc, table->candidates)
    {
        AgeGraphJoinCandidate *graph_candidate = lfirst(lc);
        AgeGraphJoinRelCandidate *rel_candidate;
        ListCell *candidate_lc;
        bool replaced = false;

        rel_candidate = palloc0(sizeof(*rel_candidate));
        rel_candidate->path = path;
        rel_candidate->lowering_input = true;
        graph_join_path_evidence_from_candidate(path, table, graph_candidate,
                                                &rel_candidate->evidence);
        graph_join_merge_lowering_pool_evidence(metadata,
                                                &rel_candidate->evidence);
        foreach(candidate_lc, metadata->lowering_candidates)
        {
            AgeGraphJoinRelCandidate *existing = lfirst(candidate_lc);

            if (graph_join_planner_candidate_matches(
                    existing, path, &rel_candidate->evidence))
            {
                existing->evidence = rel_candidate->evidence;
                existing->lowering_input = true;
                graph_join_metadata_update_component_candidate(metadata,
                                                               existing);
                replaced = true;
                break;
            }
        }
        if (replaced)
            continue;
        metadata->lowering_candidates = lappend(metadata->lowering_candidates,
                                                rel_candidate);
        graph_join_metadata_update_component_candidate(metadata,
                                                       rel_candidate);
    }
}

AgeGraphJoinCandidateTable *age_graph_join_register_rel_lowering_artifact_path(
    PlannerInfo *root, RelOptInfo *rel, Path *path,
    const AgeGraphJoinLoweringArtifact *artifact)
{
    AgeGraphJoinCandidateTable *bound_table;

    if (artifact == NULL)
        return NULL;

    bound_table = age_graph_join_bind_lowering_artifact_to_path(path,
                                                                artifact);
    if (bound_table == NULL)
        return NULL;

    age_graph_join_register_rel_lowering_table(root, rel, path, bound_table);

    return bound_table;
}

void age_graph_join_register_rel_path_evidence(
    PlannerInfo *root, RelOptInfo *rel, Path *path,
    const AgeGraphJoinPathEvidence *evidence)
{
    AgeGraphJoinRelMetadata *metadata;
    AgeGraphJoinRelPathEvidence *path_evidence;
    ListCell *lc;

    if (root == NULL ||
        rel == NULL ||
        path == NULL ||
        evidence == NULL ||
        evidence->order_property_kind == AGE_GRAPH_JOIN_ORDER_UNKNOWN)
    {
        return;
    }

    if (!age_graph_join_metadata_matches_root(root))
        age_graph_join_metadata_begin(root);
    metadata = age_graph_join_get_rel_metadata(rel, true);

    foreach(lc, metadata->path_evidence)
    {
        path_evidence = lfirst(lc);

        if (path_evidence->path == path)
        {
            path_evidence->evidence = *evidence;
            return;
        }
    }

    path_evidence = palloc0(sizeof(*path_evidence));
    path_evidence->path = path;
    path_evidence->evidence = *evidence;
    metadata->path_evidence = lappend(metadata->path_evidence,
                                      path_evidence);
}

static void graph_join_path_evidence_from_candidate(
    Path *path, const AgeGraphJoinCandidateTable *table,
    const AgeGraphJoinCandidate *candidate,
    AgeGraphJoinPathEvidence *evidence)
{
    AgeGraphJoinCandidate *next_best;

    Assert(evidence != NULL);

    age_graph_join_init_path_evidence(evidence);
    if (candidate == NULL)
        return;

    evidence->component_kind = candidate->component.family_kind;
    evidence->connector_kind = candidate->connector.kind_id;
    evidence->order_property_kind =
        candidate->connector.order_property_kind;
    evidence->source_evidence_kind =
        candidate->connector.source_evidence_kind;
    evidence->pattern_key = candidate->pattern_key;
    evidence->source_kind_id = candidate->source_kind_id;
    evidence->solved_relids = candidate->connector.solved_relids;
    evidence->required_outer = candidate->connector.required_outer;
    evidence->provided_relids = candidate->connector.provided_relids;
    evidence->candidate_count = age_graph_join_table_candidate_count(table);
    evidence->declared_cover_count = table != NULL ?
        table->declared_entry_count : 0;
    evidence->cover_match_kind = table != NULL ?
        table->cover_match_kind : AGE_GRAPH_JOIN_COVER_UNKNOWN;
    evidence->selected_total_cost = candidate->connector.total_cost;
    next_best = age_graph_join_table_select_next_best(table, candidate);
    evidence->next_total_cost = next_best != NULL ?
        next_best->connector.total_cost : 0;
    evidence->output_width = candidate->component.output_width;
    evidence->parallel_safe = candidate->component.parallel_safe;
    evidence->parallel_aware = candidate->component.parallel_aware;
    evidence->parallel_workers = candidate->component.parallel_workers;
    evidence->gather_cost = candidate->component.gather_cost;
    evidence->order_preserving = candidate->component.order_preserving;
    evidence->shared_state_required =
        candidate->component.shared_state_required;
    evidence->physical_properties_known = true;
    evidence->bound = age_graph_join_order_property_kind_is_bound(
        evidence->order_property_kind);
    age_graph_join_complete_path_evidence(path, evidence);
}

static bool graph_join_planner_candidate_matches(
    const AgeGraphJoinRelCandidate *rel_candidate, Path *path,
    const AgeGraphJoinPathEvidence *evidence)
{
    const AgeGraphJoinPathEvidence *existing;

    if (rel_candidate == NULL ||
        rel_candidate->path != path ||
        evidence == NULL)
    {
        return false;
    }

    existing = &rel_candidate->evidence;
    if (!graph_join_component_kinds_match(
            existing->component_kind,
            evidence->component_kind))
    {
        return false;
    }
    if (!graph_join_connector_kinds_match(existing->connector_kind,
                                          evidence->connector_kind))
    {
        return false;
    }
    if (!graph_join_order_property_kinds_match(
            existing->order_property_kind,
            evidence->order_property_kind))
    {
        return false;
    }
    if (!graph_join_source_evidence_matches(existing->source_evidence_kind,
                                            evidence->source_evidence_kind))
        return false;
    if (!graph_join_text_matches(existing->pattern_key,
                                 evidence->pattern_key))
    {
        return false;
    }
    if (!graph_join_source_kinds_match(existing->source_kind_id,
                                       evidence->source_kind_id))
    {
        return false;
    }

    return bms_equal(existing->solved_relids, evidence->solved_relids) &&
           bms_equal(existing->required_outer, evidence->required_outer) &&
           bms_equal(existing->provided_relids, evidence->provided_relids);
}

static bool graph_join_lowering_candidate_matches(
    const AgeGraphJoinRelCandidate *rel_candidate,
    const AgeGraphJoinPathEvidence *evidence)
{
    const AgeGraphJoinPathEvidence *existing;

    if (rel_candidate == NULL ||
        evidence == NULL)
    {
        return false;
    }

    existing = &rel_candidate->evidence;
    if (!graph_join_component_kinds_match(
            existing->component_kind,
            evidence->component_kind))
    {
        return false;
    }
    if (!graph_join_connector_kinds_match(existing->connector_kind,
                                          evidence->connector_kind))
    {
        return false;
    }
    if (!graph_join_order_property_kinds_match(
            existing->order_property_kind,
            evidence->order_property_kind))
    {
        return false;
    }
    if (!graph_join_source_evidence_matches(existing->source_evidence_kind,
                                            evidence->source_evidence_kind))
        return false;
    if (!graph_join_text_matches(existing->pattern_key,
                                 evidence->pattern_key))
    {
        return false;
    }
    if (!graph_join_source_kinds_match(existing->source_kind_id,
                                       evidence->source_kind_id))
    {
        return false;
    }

    return bms_equal(existing->solved_relids, evidence->solved_relids) &&
           bms_equal(existing->required_outer, evidence->required_outer) &&
           bms_equal(existing->provided_relids, evidence->provided_relids);
}

static void graph_join_merge_lowering_pool_evidence(
    AgeGraphJoinRelMetadata *metadata,
    AgeGraphJoinPathEvidence *evidence)
{
    ListCell *lc;
    AgeGraphJoinCoverMatchKind mismatch_kind = AGE_GRAPH_JOIN_COVER_UNKNOWN;
    int64 declared_cover_count = 0;

    if (metadata == NULL ||
        metadata->lowering_pool == NIL ||
        evidence == NULL)
    {
        return;
    }

    foreach(lc, metadata->lowering_pool)
    {
        AgeGraphJoinRelCandidate *pool_candidate = lfirst(lc);

        declared_cover_count = Max(
            declared_cover_count,
            pool_candidate->evidence.declared_cover_count);

        if (graph_join_lowering_candidate_matches(pool_candidate, evidence))
        {
            AgeGraphJoinPathEvidence path_evidence = *evidence;

            *evidence = pool_candidate->evidence;
            evidence->selected_total_cost = path_evidence.selected_total_cost;
            evidence->next_total_cost = path_evidence.next_total_cost;
            evidence->output_width = path_evidence.output_width;
            evidence->parallel_safe = path_evidence.parallel_safe;
            evidence->parallel_aware = path_evidence.parallel_aware;
            evidence->parallel_workers = path_evidence.parallel_workers;
            evidence->gather_cost = path_evidence.gather_cost;
            evidence->order_preserving = path_evidence.order_preserving;
            evidence->shared_state_required =
                path_evidence.shared_state_required;
            evidence->physical_properties_known =
                path_evidence.physical_properties_known;
            evidence->declared_cover_count = Max(
                pool_candidate->evidence.declared_cover_count,
                path_evidence.declared_cover_count);
            evidence->cover_match_kind = AGE_GRAPH_JOIN_COVER_MATCHED;
            return;
        }

        if (!graph_join_text_matches(pool_candidate->evidence.pattern_key,
                                     evidence->pattern_key))
        {
            if (mismatch_kind == AGE_GRAPH_JOIN_COVER_UNKNOWN)
                mismatch_kind = AGE_GRAPH_JOIN_COVER_PATTERN_MISMATCH;
            continue;
        }
        if (!graph_join_component_kinds_match(
                pool_candidate->evidence.component_kind,
                evidence->component_kind))
        {
            mismatch_kind = AGE_GRAPH_JOIN_COVER_COMPONENT_MISMATCH;
            continue;
        }
        if (!graph_join_source_kinds_match(
                pool_candidate->evidence.source_kind_id,
                evidence->source_kind_id))
        {
            mismatch_kind = AGE_GRAPH_JOIN_COVER_SOURCE_MISMATCH;
            continue;
        }

        evidence->declared_cover_count = Max(
            evidence->declared_cover_count,
            pool_candidate->evidence.declared_cover_count);
        evidence->cover_match_kind = AGE_GRAPH_JOIN_COVER_MATCHED;
        return;
    }

    evidence->declared_cover_count = Max(evidence->declared_cover_count,
                                         declared_cover_count);
    evidence->cover_match_kind = mismatch_kind;
}

static bool graph_join_metadata_lookup_path_evidence(
    const AgeGraphJoinRelMetadata *metadata, Path *path,
    AgeGraphJoinPathEvidence *evidence)
{
    ListCell *lc;

    if (metadata == NULL ||
        path == NULL ||
        evidence == NULL)
    {
        return false;
    }

    foreach(lc, metadata->path_evidence)
    {
        AgeGraphJoinRelPathEvidence *path_evidence = lfirst(lc);

        if (path_evidence->path == path)
        {
            *evidence = path_evidence->evidence;
            return evidence->order_property_kind !=
                   AGE_GRAPH_JOIN_ORDER_UNKNOWN;
        }
    }

    return false;
}

double age_graph_join_evidence_alternative_credit(
    const AgeGraphJoinPathEvidence *evidence)
{
    if (evidence == NULL ||
        evidence->candidate_count <= 1 ||
        evidence->selected_total_cost <= 0 ||
        evidence->next_total_cost <= evidence->selected_total_cost)
    {
        return 1.0;
    }

    return Max(0.25,
               evidence->selected_total_cost / evidence->next_total_cost);
}

Cost age_graph_join_apply_evidence_alternative_credit(
    Cost startup_cost, Cost total_cost, double credit)
{
    Cost run_cost;

    if (credit >= 1.0)
        return total_cost;

    run_cost = Max(total_cost - startup_cost, 0.0);

    return startup_cost + run_cost * credit;
}

double age_graph_join_path_evidence_credit(
    const AgeGraphJoinPathEvidence *outer_evidence,
    const AgeGraphJoinPathEvidence *inner_evidence)
{
    double credit = 1.0;

    credit = Min(credit,
                 age_graph_join_evidence_alternative_credit(outer_evidence));
    credit = Min(credit,
                 age_graph_join_evidence_alternative_credit(inner_evidence));

    return credit;
}

int age_graph_join_table_candidate_count(
    const AgeGraphJoinCandidateTable *table)
{
    if (table == NULL)
        return 0;

    return Max(list_length(table->candidates), table->declared_entry_count);
}

AgeGraphJoinCandidate *age_graph_join_table_select_next_best(
    const AgeGraphJoinCandidateTable *table,
    const AgeGraphJoinCandidate *selected)
{
    AgeGraphJoinCandidate *next_best = NULL;
    ListCell *lc;

    if (table == NULL || selected == NULL)
        return NULL;

    foreach(lc, table->candidates)
    {
        AgeGraphJoinCandidate *candidate = lfirst(lc);

        if (candidate == selected)
            continue;
        if (next_best == NULL ||
            candidate->connector.total_cost < next_best->connector.total_cost)
        {
            next_best = candidate;
        }
    }

    return next_best;
}

List *age_graph_join_candidate_private(
    const AgeGraphJoinCandidate *candidate)
{
    List *descriptor = NIL;

    Assert(candidate != NULL);

    descriptor = lappend(descriptor,
                         make_graph_join_text_const(
                             candidate->component.display_name));
    descriptor = lappend(descriptor,
                         make_graph_join_text_const(
                             age_graph_join_bound_kind_name(
                                 candidate->connector.bound_kind)));
    descriptor = lappend(descriptor,
                         make_graph_join_text_const(
                             age_graph_join_source_evidence_kind_name(
                                 candidate->connector.source_evidence_kind)));
    descriptor = lappend(descriptor,
                         make_graph_join_text_const(candidate->pattern_key));
    descriptor = lappend(descriptor,
                         makeConst(INT4OID, -1, InvalidOid, sizeof(int32),
                                   Int32GetDatum(
                                       candidate->component.family_kind),
                                   false, true));
    descriptor = lappend(descriptor,
                         makeConst(INT4OID, -1, InvalidOid, sizeof(int32),
                                   Int32GetDatum(
                                       candidate->connector.kind_id),
                                   false, true));
    descriptor = lappend(descriptor,
                         makeConst(INT4OID, -1, InvalidOid, sizeof(int32),
                                   Int32GetDatum(
                                       candidate->connector.order_property_kind),
                                   false, true));
    descriptor = lappend(descriptor,
                         makeConst(INT4OID, -1, InvalidOid, sizeof(int32),
                                   Int32GetDatum(
                                       candidate->connector.source_evidence_kind),
                                   false, true));
    descriptor = lappend(descriptor,
                         makeConst(INT4OID, -1, InvalidOid, sizeof(int32),
                                   Int32GetDatum(candidate->source_kind_id),
                                   false, true));
    descriptor = lappend(descriptor,
                         make_graph_join_text_const(
                             bmsToString(
                                 candidate->connector.solved_relids)));
    descriptor = lappend(descriptor,
                         make_graph_join_text_const(
                             bmsToString(
                                 candidate->connector.required_outer)));
    descriptor = lappend(descriptor,
                         make_graph_join_text_const(
                             bmsToString(
                                 candidate->connector.provided_relids)));
    descriptor = lappend(descriptor,
                         makeConst(FLOAT8OID, -1, InvalidOid,
                                   sizeof(float8),
                                   Float8GetDatum(candidate->connector.rows),
                                   false, true));
    descriptor = lappend(descriptor,
                         makeConst(FLOAT8OID, -1, InvalidOid,
                                   sizeof(float8),
                                   Float8GetDatum(
                                       candidate->connector.startup_cost),
                                   false, true));
    descriptor = lappend(descriptor,
                         makeConst(FLOAT8OID, -1, InvalidOid,
                                   sizeof(float8),
                                   Float8GetDatum(
                                       candidate->connector.total_cost),
                                   false, true));
    descriptor = lappend(descriptor,
                         makeConst(INT4OID, -1, InvalidOid, sizeof(int32),
                                   Int32GetDatum(
                                       candidate->component.output_width),
                                   false, true));
    descriptor = lappend(descriptor,
                         makeConst(BOOLOID, -1, InvalidOid, sizeof(bool),
                                   BoolGetDatum(
                                       candidate->component.parallel_safe),
                                   false, true));
    descriptor = lappend(descriptor,
                         makeConst(BOOLOID, -1, InvalidOid, sizeof(bool),
                                   BoolGetDatum(
                                       candidate->component.parallel_aware),
                                   false, true));
    descriptor = lappend(descriptor,
                         makeConst(INT4OID, -1, InvalidOid, sizeof(int32),
                                   Int32GetDatum(
                                       candidate->component.parallel_workers),
                                   false, true));
    descriptor = lappend(descriptor,
                         makeConst(FLOAT8OID, -1, InvalidOid,
                                   sizeof(float8),
                                   Float8GetDatum(
                                       candidate->component.gather_cost),
                                   false, true));
    descriptor = lappend(descriptor,
                         makeConst(BOOLOID, -1, InvalidOid, sizeof(bool),
                                   BoolGetDatum(
                                       candidate->component.order_preserving),
                                   false, true));
    descriptor = lappend(descriptor,
                         makeConst(BOOLOID, -1, InvalidOid, sizeof(bool),
                                   BoolGetDatum(
                                       candidate->component.shared_state_required),
                                   false, true));
    descriptor = lappend(descriptor,
                         makeConst(INT4OID, -1, InvalidOid, sizeof(int32),
                                   Int32GetDatum(1), false, true));
    descriptor = lappend(descriptor,
                         makeConst(INT4OID, -1, InvalidOid, sizeof(int32),
                                   Int32GetDatum(0), false, true));
    descriptor = lappend(descriptor,
                         makeConst(INT4OID, -1, InvalidOid, sizeof(int32),
                                   Int32GetDatum(AGE_GRAPH_JOIN_COVER_UNKNOWN),
                                   false, true));
    descriptor = lappend(descriptor,
                         makeConst(INT4OID, -1, InvalidOid, sizeof(int32),
                                   Int32GetDatum(
                                       AGE_GRAPH_JOIN_CONNECTOR_UNKNOWN),
                                   false, true));
    descriptor = lappend(descriptor,
                         makeConst(INT4OID, -1, InvalidOid, sizeof(int32),
                                   Int32GetDatum(AGE_GRAPH_JOIN_ORDER_UNKNOWN),
                                   false, true));
    descriptor = lappend(descriptor, make_graph_join_text_const("none"));
    descriptor = lappend(descriptor,
                         makeConst(FLOAT8OID, -1, InvalidOid,
                                   sizeof(float8), Float8GetDatum(0),
                                   false, true));
    descriptor = lappend(descriptor,
                         makeConst(FLOAT8OID, -1, InvalidOid,
                                   sizeof(float8), Float8GetDatum(0),
                                   false, true));

    return descriptor;
}

List *age_graph_join_table_selected_private(
    const AgeGraphJoinCandidateTable *table)
{
    AgeGraphJoinCandidate *candidate;
    AgeGraphJoinCandidate *next_best;
    List *descriptor;

    candidate = age_graph_join_table_select_cheapest(table);
    if (candidate == NULL)
        return NIL;

    descriptor = age_graph_join_candidate_private(candidate);
    next_best = age_graph_join_table_select_next_best(table, candidate);

    set_graph_join_descriptor_value(
        descriptor, AGE_GRAPH_JOIN_DESC_CANDIDATE_COUNT,
        (Node *)makeConst(INT4OID, -1, InvalidOid, sizeof(int32),
                          Int32GetDatum(
                              age_graph_join_table_candidate_count(table)),
                          false, true));
    set_graph_join_descriptor_value(
        descriptor, AGE_GRAPH_JOIN_DESC_DECLARED_COVER_COUNT,
        (Node *)makeConst(INT4OID, -1, InvalidOid, sizeof(int32),
                          Int32GetDatum(table->declared_entry_count),
                          false, true));
    set_graph_join_descriptor_value(
        descriptor, AGE_GRAPH_JOIN_DESC_COVER_MATCH_KIND,
        (Node *)makeConst(INT4OID, -1, InvalidOid, sizeof(int32),
                          Int32GetDatum(table->cover_match_kind),
                          false, true));
    if (next_best != NULL)
    {
        set_graph_join_descriptor_value(
            descriptor, AGE_GRAPH_JOIN_DESC_NEXT_CONNECTOR_KIND,
            (Node *)makeConst(INT4OID, -1, InvalidOid, sizeof(int32),
                              Int32GetDatum(next_best->connector.kind_id),
                              false, true));
        set_graph_join_descriptor_value(
            descriptor, AGE_GRAPH_JOIN_DESC_NEXT_ORDER_PROPERTY_KIND,
            (Node *)makeConst(
                INT4OID, -1, InvalidOid, sizeof(int32),
                Int32GetDatum(next_best->connector.order_property_kind),
                false, true));
        set_graph_join_descriptor_value(
            descriptor, AGE_GRAPH_JOIN_DESC_NEXT_SOURCE_EVIDENCE,
            (Node *)make_graph_join_text_const(
                age_graph_join_source_evidence_kind_name(
                    next_best->connector.source_evidence_kind)));
        set_graph_join_descriptor_value(
            descriptor, AGE_GRAPH_JOIN_DESC_NEXT_ROWS,
            (Node *)makeConst(FLOAT8OID, -1, InvalidOid, sizeof(float8),
                              Float8GetDatum(next_best->connector.rows),
                              false, true));
        set_graph_join_descriptor_value(
            descriptor, AGE_GRAPH_JOIN_DESC_NEXT_TOTAL_COST,
            (Node *)makeConst(FLOAT8OID, -1, InvalidOid, sizeof(float8),
                              Float8GetDatum(
                                  next_best->connector.total_cost),
                              false, true));
    }

    return descriptor;
}

const char *age_graph_join_descriptor_text_field(List *descriptor, int index)
{
    Const *value;

    if (descriptor == NIL || list_length(descriptor) <= index)
        return NULL;

    value = list_nth_node(Const, descriptor, index);
    if (value->constisnull || value->consttype != TEXTOID)
        return NULL;

    return TextDatumGetCString(value->constvalue);
}

int64 age_graph_join_descriptor_int_field(List *descriptor, int index,
                                          int64 fallback)
{
    Const *value;

    if (descriptor == NIL || list_length(descriptor) <= index)
        return fallback;

    value = list_nth_node(Const, descriptor, index);
    if (value->constisnull)
        return fallback;
    if (value->consttype == BOOLOID)
        return DatumGetBool(value->constvalue) ? 1 : 0;
    if (value->consttype == INT4OID)
        return (int64)DatumGetInt32(value->constvalue);
    if (value->consttype == INT8OID)
        return DatumGetInt64(value->constvalue);
    if (value->consttype == OIDOID)
        return (int64)DatumGetObjectId(value->constvalue);

    return fallback;
}

double age_graph_join_descriptor_float_field(List *descriptor, int index,
                                             double fallback)
{
    Const *value;

    if (descriptor == NIL || list_length(descriptor) <= index)
        return fallback;

    value = list_nth_node(Const, descriptor, index);
    if (value->constisnull)
        return fallback;
    if (value->consttype == FLOAT8OID)
        return DatumGetFloat8(value->constvalue);
    if (value->consttype == INT4OID)
        return (double)DatumGetInt32(value->constvalue);
    if (value->consttype == INT8OID)
        return (double)DatumGetInt64(value->constvalue);

    return fallback;
}

static char *copy_graph_join_text(const char *value, const char *fallback)
{
    if (value == NULL)
        value = fallback;
    if (value == NULL)
        return NULL;

    return pstrdup(value);
}

static Const *make_graph_join_text_const(const char *value)
{
    return makeConst(TEXTOID, -1, DEFAULT_COLLATION_OID, -1,
                     CStringGetTextDatum(value != NULL ? value : "unknown"),
                     false, false);
}

static void set_graph_join_descriptor_value(List *descriptor, int index,
                                            Node *value)
{
    ListCell *cell;

    Assert(descriptor != NIL);
    Assert(value != NULL);
    Assert(list_length(descriptor) > index);

    cell = list_nth_cell(descriptor, index);
    lfirst(cell) = value;
}
