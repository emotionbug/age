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
static const char *graph_join_infer_component_family(const char *component,
                                                     const char *connector,
                                                     const char *order_property);
static void graph_join_metadata_context_reset(void *arg);
static bool graph_join_metadata_identity_matches(PlannerInfo *root);
static void apply_graph_join_lowering_input(
    AgeGraphJoinCandidateRequest *request,
    const AgeGraphJoinLoweringInput *lowering_input);
static void graph_join_metadata_rebuild_component_candidates(
    AgeGraphJoinRelMetadata *metadata);
static void graph_join_metadata_append_lowering_candidates(
    AgeGraphJoinRelMetadata *metadata);
static void graph_join_metadata_merge_lowering_candidate(
    AgeGraphJoinRelCandidate *candidate,
    const AgeGraphJoinRelCandidate *lowering_candidate);
static int graph_join_metadata_lowering_candidate_count(
    const AgeGraphJoinRelMetadata *metadata);
static int graph_join_metadata_lowering_artifact_entry_count(
    const AgeGraphJoinRelMetadata *metadata);
static AgeGraphJoinLoweringArtifact *graph_join_metadata_merge_lowering_artifact(
    AgeGraphJoinRelMetadata *metadata,
    const AgeGraphJoinLoweringArtifact *artifact);
static AgeGraphJoinLoweringArtifact *graph_join_metadata_find_lowering_artifact(
    AgeGraphJoinRelMetadata *metadata, const char *pattern_kind);
static void graph_join_metadata_update_component_candidate(
    AgeGraphJoinRelMetadata *metadata,
    const AgeGraphJoinRelCandidate *candidate);
static bool graph_join_component_candidate_keeps_existing(
    const AgeGraphJoinRelComponentCandidate *component_candidate,
    const AgeGraphJoinRelCandidate *candidate, Cost total_cost);
static bool graph_join_component_candidate_matches(
    const AgeGraphJoinRelComponentCandidate *component_candidate,
    const char *component, const AgeGraphJoinPathEvidence *evidence);
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
    candidate->component.name =
        copy_graph_join_text(request->component, "unknown");
    candidate->component.family =
        copy_graph_join_text(
            request->component_family != NULL ? request->component_family :
            graph_join_infer_component_family(request->component,
                                              request->connector,
                                              request->order_property),
            "graph-component");
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

    candidate->connector.kind =
        copy_graph_join_text(request->connector, "unknown");
    candidate->connector.bound =
        copy_graph_join_text(request->bound, "unknown");
    candidate->connector.order_property =
        copy_graph_join_text(request->order_property, "query-order");
    candidate->connector.source_evidence =
        copy_graph_join_text(request->source_evidence, "unknown");
    candidate->connector.solved_relids = bms_copy(request->solved_relids);
    candidate->connector.required_outer = bms_copy(request->required_outer);
    candidate->connector.provided_relids = bms_copy(request->provided_relids);
    candidate->connector.rows = request->rows;
    candidate->connector.startup_cost = request->startup_cost;
    candidate->connector.total_cost = request->total_cost;

    return candidate;
}

AgeGraphJoinCandidateTable *age_graph_join_make_candidate_table(void)
{
    AgeGraphJoinCandidateTable *table;

    table = palloc0(sizeof(*table));

    return table;
}

AgeGraphJoinLoweringArtifact *age_graph_join_make_pattern_lowering_artifact(
    const char *pattern_kind, const char *source_kind,
    AgeGraphJoinCandidateTable *table)
{
    AgeGraphJoinLoweringArtifact *artifact;

    artifact = palloc0(sizeof(*artifact));
    artifact->pattern_kind = copy_graph_join_text(pattern_kind,
                                                  "graph-pattern");
    age_graph_join_lowering_artifact_add_table(artifact, source_kind, table);

    return artifact;
}

AgeGraphJoinLoweringArtifact *age_graph_join_make_lowering_artifact(
    const char *source_kind, AgeGraphJoinCandidateTable *table)
{
    return age_graph_join_make_pattern_lowering_artifact(
        source_kind, source_kind, table);
}

void age_graph_join_lowering_artifact_add_table(
    AgeGraphJoinLoweringArtifact *artifact, const char *source_kind,
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

    foreach(lc, artifact->entries)
    {
        AgeGraphJoinLoweringArtifactEntry *existing = lfirst(lc);

        if (existing != NULL &&
            existing->table == table &&
            existing->source_kind != NULL &&
            source_kind != NULL &&
            strcmp(existing->source_kind, source_kind) == 0)
        {
            return;
        }
    }

    entry = palloc0(sizeof(*entry));
    entry->source_kind = copy_graph_join_text(source_kind,
                                              artifact->pattern_kind);
    entry->table = table;
    artifact->entries = lappend(artifact->entries, entry);
}

AgeGraphJoinCandidate *age_graph_join_table_add_candidate(
    AgeGraphJoinCandidateTable *table,
    const AgeGraphJoinCandidateRequest *request)
{
    AgeGraphJoinCandidate *candidate;

    Assert(table != NULL);

    candidate = age_graph_join_make_candidate(request);
    table->candidates = lappend(table->candidates, candidate);

    return candidate;
}

AgeGraphJoinCandidate *age_graph_join_table_add_path_candidate(
    AgeGraphJoinCandidateTable *table, Path *path, const char *component,
    const char *component_family, const char *connector, const char *bound,
    const char *order_property, const char *source_evidence,
    bool shared_state_required)
{
    AgeGraphJoinCandidateRequest request;

    age_graph_join_init_path_request(&request, path, component,
                                     component_family, connector, bound,
                                     order_property, source_evidence,
                                     shared_state_required);

    return age_graph_join_table_add_candidate(table, &request);
}

void age_graph_join_init_path_request(
    AgeGraphJoinCandidateRequest *request, Path *path, const char *component,
    const char *component_family, const char *connector, const char *bound,
    const char *order_property, const char *source_evidence,
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

    request->component = component;
    request->component_family = component_family;
    request->connector = connector;
    request->bound = bound;
    request->order_property = order_property;
    request->source_evidence = source_evidence;
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
    const char *component, const char *component_family,
    const char *connector, const char *bound,
    const char *order_property, const char *source_evidence,
    bool shared_state_required)
{
    age_graph_join_init_path_request(request, path, component,
                                     component_family, connector, bound,
                                     order_property, source_evidence,
                                     shared_state_required);
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

AgeGraphJoinCandidate *age_graph_join_table_add_scheduled_candidate(
    AgeGraphJoinCandidateTable *table, Path *path, const char *component,
    const char *component_family, const char *connector, const char *bound,
    const char *order_property, const char *source_evidence, double rows,
    Cost startup_cost, Cost total_cost, bool shared_state_required)
{
    AgeGraphJoinCandidateRequest request;

    age_graph_join_init_path_request(&request, path, component,
                                     component_family, connector, bound,
                                     order_property, source_evidence,
                                     shared_state_required);
    request.rows = rows;
    request.startup_cost = startup_cost;
    request.total_cost = total_cost;

    return age_graph_join_table_add_candidate(table, &request);
}

AgeGraphJoinCandidate *age_graph_join_table_add_lowered_path_candidate(
    AgeGraphJoinCandidateTable *table, Path *path,
    const AgeGraphJoinLoweringInput *lowering_input,
    const char *component, const char *component_family,
    const char *connector, const char *bound,
    const char *order_property, const char *source_evidence,
    bool shared_state_required)
{
    AgeGraphJoinCandidateRequest request;

    age_graph_join_init_lowered_path_request(
        &request, path, lowering_input, component, component_family,
        connector, bound, order_property, source_evidence,
        shared_state_required);

    return age_graph_join_table_add_candidate(table, &request);
}

AgeGraphJoinCandidate *age_graph_join_table_add_lowered_scheduled_candidate(
    AgeGraphJoinCandidateTable *table, Path *path,
    const AgeGraphJoinLoweringInput *lowering_input,
    const char *component, const char *component_family,
    const char *connector, const char *bound,
    const char *order_property, const char *source_evidence, double rows,
    Cost startup_cost, Cost total_cost, bool shared_state_required)
{
    AgeGraphJoinCandidateRequest request;

    age_graph_join_init_lowered_path_request(
        &request, path, lowering_input, component, component_family,
        connector, bound, order_property, source_evidence,
        shared_state_required);
    request.rows = rows;
    request.startup_cost = startup_cost;
    request.total_cost = total_cost;

    return age_graph_join_table_add_candidate(table, &request);
}

AgeGraphJoinCandidate *age_graph_join_table_add_path_evidence_candidate(
    AgeGraphJoinCandidateTable *table, Path *path, const char *component,
    const AgeGraphJoinPathEvidence *evidence)
{
    AgeGraphJoinCandidateRequest request;
    Cost total_cost;

    Assert(path != NULL);

    if (table == NULL ||
        evidence == NULL ||
        evidence->order_property == NULL)
    {
        return NULL;
    }

    age_graph_join_init_path_request(
        &request, path,
        component != NULL ? component :
        age_graph_join_component_from_evidence(evidence),
        age_graph_join_component_from_evidence(evidence),
        evidence->connector != NULL ? evidence->connector : "graph-connector",
        evidence->bound ? "bound" : "unbound",
        evidence->order_property,
        evidence->source_evidence != NULL ?
        evidence->source_evidence : "path-evidence",
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
    request.total_cost = total_cost;
    request.rows = path->rows;

    return age_graph_join_table_add_candidate(table, &request);
}

const char *age_graph_join_component_from_evidence(
    const AgeGraphJoinPathEvidence *evidence)
{
    if (evidence == NULL)
        return "graph-component";
    if (evidence->component_family != NULL)
        return evidence->component_family;

    return graph_join_infer_component_family(NULL, evidence->connector,
                                             evidence->order_property);
}

static const char *graph_join_infer_component_family(const char *component,
                                                     const char *connector,
                                                     const char *order_property)
{
    if (component != NULL &&
        (strcmp(component, "node-property-seek") == 0 ||
         strcmp(component, "vle-expansion") == 0 ||
         strcmp(component, "adjacency-expansion") == 0 ||
         strcmp(component, "graph-value-join") == 0 ||
         strcmp(component, "graph-apply") == 0 ||
         strcmp(component, "graph-cartesian") == 0))
    {
        return component;
    }

    if (connector != NULL &&
        (strcmp(connector, "postgres-index-seek") == 0 ||
         strcmp(connector, "node-property-index-seek") == 0))
    {
        return "node-property-seek";
    }

    if (connector != NULL &&
        (strncmp(connector, "vle-", 4) == 0 ||
         strcmp(connector, "matrix-frontier-expand") == 0))
    {
        return "vle-expansion";
    }

    if (connector != NULL &&
        (strncmp(connector, "adjacency-", 10) == 0 ||
         strcmp(connector, "age-adjacency") == 0))
    {
        return "adjacency-expansion";
    }
    if (connector != NULL &&
        strcmp(connector, "graph-value-join") == 0)
        return "graph-value-join";
    if (connector != NULL &&
        strcmp(connector, "graph-apply") == 0)
        return "graph-apply";
    if (connector != NULL &&
        strcmp(connector, "graph-cartesian") == 0)
        return "graph-cartesian";

    if (order_property != NULL &&
        strcmp(order_property, "index-anchored") == 0)
        return "node-property-seek";
    if (order_property != NULL &&
        (strcmp(order_property, "vle-frontier-anchored") == 0 ||
         strcmp(order_property, "matrix-frontier-anchored") == 0 ||
         strcmp(order_property, "expand-into-verification") == 0))
    {
        return "vle-expansion";
    }
    if (order_property != NULL &&
        (strcmp(order_property, "adjacency-directory-anchored") == 0 ||
         strcmp(order_property, "adjacency-anchored") == 0))
    {
        return "adjacency-expansion";
    }
    if (order_property != NULL &&
        strcmp(order_property, "value-anchored") == 0)
        return "graph-value-join";
    if (order_property != NULL &&
        strcmp(order_property, "apply-anchored") == 0)
        return "graph-apply";
    if (order_property != NULL &&
        strcmp(order_property, "cartesian") == 0)
        return "graph-cartesian";

    return "graph-component";
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

bool age_graph_join_order_property_is_bound(const char *order_property)
{
    if (order_property == NULL)
        return false;

    return strcmp(order_property, "index-anchored") == 0 ||
           strcmp(order_property, "adjacency-directory-anchored") == 0 ||
           strcmp(order_property, "adjacency-anchored") == 0 ||
           strcmp(order_property, "vle-frontier-anchored") == 0 ||
           strcmp(order_property, "expand-into-verification") == 0 ||
           strcmp(order_property, "matrix-frontier-anchored") == 0 ||
           strcmp(order_property, "value-anchored") == 0 ||
           strcmp(order_property, "apply-anchored") == 0;
}

void age_graph_join_init_path_evidence(
    AgeGraphJoinPathEvidence *evidence)
{
    Assert(evidence != NULL);

    memset(evidence, 0, sizeof(*evidence));
    evidence->candidate_count = 1;
}

void age_graph_join_complete_path_evidence(
    Path *path, AgeGraphJoinPathEvidence *evidence)
{
    RelOptInfo *rel;

    if (path == NULL || evidence == NULL)
        return;

    if (evidence->component_family == NULL &&
        (evidence->connector != NULL || evidence->order_property != NULL))
    {
        evidence->component_family =
            graph_join_infer_component_family(NULL, evidence->connector,
                                              evidence->order_property);
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
        metadata, artifact->pattern_kind);
    if (shared_artifact == NULL)
    {
        shared_artifact = age_graph_join_make_lowering_artifact(
            artifact->pattern_kind, NULL);
        metadata->lowering_artifacts = lappend(metadata->lowering_artifacts,
                                               shared_artifact);
    }

    foreach(lc, artifact->entries)
    {
        AgeGraphJoinLoweringArtifactEntry *entry = lfirst(lc);

        if (entry == NULL)
            continue;
        age_graph_join_lowering_artifact_add_table(
            shared_artifact, entry->source_kind, entry->table);
    }

    return shared_artifact;
}

static AgeGraphJoinLoweringArtifact *graph_join_metadata_find_lowering_artifact(
    AgeGraphJoinRelMetadata *metadata, const char *pattern_kind)
{
    ListCell *lc;

    if (metadata == NULL ||
        pattern_kind == NULL)
    {
        return NULL;
    }

    foreach(lc, metadata->lowering_artifacts)
    {
        AgeGraphJoinLoweringArtifact *artifact = lfirst(lc);

        if (artifact != NULL &&
            artifact->pattern_kind != NULL &&
            strcmp(artifact->pattern_kind, pattern_kind) == 0)
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
    foreach(lc, metadata->candidates)
    {
        AgeGraphJoinRelCandidate *candidate = lfirst(lc);

        graph_join_metadata_update_component_candidate(metadata, candidate);
    }
}

static void graph_join_metadata_update_component_candidate(
    AgeGraphJoinRelMetadata *metadata,
    const AgeGraphJoinRelCandidate *candidate)
{
    const char *component;
    Cost total_cost;
    ListCell *lc;
    AgeGraphJoinRelComponentCandidate *component_candidate;

    if (metadata == NULL ||
        candidate == NULL ||
        candidate->path == NULL ||
        candidate->evidence.order_property == NULL)
    {
        return;
    }

    component = age_graph_join_component_from_evidence(&candidate->evidence);
    total_cost = candidate->evidence.selected_total_cost > 0 ?
        candidate->evidence.selected_total_cost : candidate->path->total_cost;

    foreach(lc, metadata->component_candidates)
    {
        component_candidate = lfirst(lc);

        if (graph_join_component_candidate_matches(
                component_candidate, component, &candidate->evidence))
        {
            if (graph_join_component_candidate_keeps_existing(
                    component_candidate, candidate, total_cost))
            {
                return;
            }

            component_candidate->path = candidate->path;
            component_candidate->evidence = candidate->evidence;
            component_candidate->total_cost = total_cost;
            component_candidate->lowering_input = candidate->lowering_input;
            return;
        }
    }

    component_candidate = palloc0(sizeof(*component_candidate));
    component_candidate->path = candidate->path;
    component_candidate->evidence = candidate->evidence;
    component_candidate->component = copy_graph_join_text(component,
                                                          "graph-component");
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

static bool graph_join_component_candidate_matches(
    const AgeGraphJoinRelComponentCandidate *component_candidate,
    const char *component, const AgeGraphJoinPathEvidence *evidence)
{
    const AgeGraphJoinPathEvidence *existing;

    if (component_candidate == NULL ||
        component_candidate->component == NULL ||
        component == NULL ||
        evidence == NULL)
    {
        return false;
    }
    if (strcmp(component_candidate->component, component) != 0)
        return false;

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

    return graph_join_text_matches(existing->connector,
                                   candidate->connector) &&
           graph_join_text_matches(existing->order_property,
                                   candidate->order_property) &&
           graph_join_text_matches(existing->source_evidence,
                                   candidate->source_evidence);
}

static bool graph_join_text_matches(const char *existing,
                                    const char *candidate)
{
    if (existing == NULL || candidate == NULL)
        return existing == candidate;

    return strcmp(existing, candidate) == 0;
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

    foreach(lc, artifact->entries)
    {
        AgeGraphJoinLoweringArtifactEntry *entry = lfirst(lc);

        age_graph_join_register_rel_lowering_pool(root, rel, entry->table);
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
    foreach(lc, table->candidates)
    {
        AgeGraphJoinCandidate *candidate = lfirst(lc);
        AgeGraphJoinCandidateRequest request;
        Cost run_cost;

        memset(&request, 0, sizeof(request));
        request.component = candidate->component.name;
        request.component_family = candidate->component.family;
        request.connector = candidate->connector.kind;
        request.bound = candidate->connector.bound;
        request.order_property = candidate->connector.order_property;
        request.source_evidence = candidate->connector.source_evidence;
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

void age_graph_join_register_rel_lowering_artifact_path(
    PlannerInfo *root, RelOptInfo *rel, Path *path,
    const AgeGraphJoinLoweringArtifact *artifact)
{
    AgeGraphJoinCandidateTable *bound_table;

    if (artifact == NULL)
        return;

    bound_table = age_graph_join_bind_lowering_artifact_to_path(path,
                                                                artifact);
    if (bound_table == NULL)
        return;

    age_graph_join_register_rel_lowering_table(root, rel, path, bound_table);
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
        evidence->order_property == NULL)
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

    evidence->component_family = candidate->component.family;
    evidence->connector = candidate->connector.kind;
    evidence->order_property = candidate->connector.order_property;
    evidence->source_evidence = candidate->connector.source_evidence;
    evidence->solved_relids = candidate->connector.solved_relids;
    evidence->required_outer = candidate->connector.required_outer;
    evidence->provided_relids = candidate->connector.provided_relids;
    evidence->candidate_count = age_graph_join_table_candidate_count(table);
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
    evidence->bound = age_graph_join_order_property_is_bound(
        evidence->order_property);
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
    if (!graph_join_text_matches(age_graph_join_component_from_evidence(
                                     existing),
                                 age_graph_join_component_from_evidence(
                                     evidence)))
    {
        return false;
    }
    if (!graph_join_text_matches(existing->connector, evidence->connector))
    {
        return false;
    }
    if (!graph_join_text_matches(existing->order_property,
                                 evidence->order_property))
    {
        return false;
    }
    if (!graph_join_text_matches(existing->source_evidence,
                                 evidence->source_evidence))
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
    if (strcmp(age_graph_join_component_from_evidence(existing),
               age_graph_join_component_from_evidence(evidence)) != 0)
    {
        return false;
    }
    if (existing->connector == NULL ||
        evidence->connector == NULL ||
        strcmp(existing->connector, evidence->connector) != 0)
    {
        return false;
    }
    if (existing->order_property == NULL ||
        evidence->order_property == NULL ||
        strcmp(existing->order_property, evidence->order_property) != 0)
    {
        return false;
    }
    if (existing->source_evidence == NULL ||
        evidence->source_evidence == NULL ||
        strcmp(existing->source_evidence, evidence->source_evidence) != 0)
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

    if (metadata == NULL ||
        metadata->lowering_pool == NIL ||
        evidence == NULL)
    {
        return;
    }

    foreach(lc, metadata->lowering_pool)
    {
        AgeGraphJoinRelCandidate *pool_candidate = lfirst(lc);

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
            return;
        }
    }
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
            return evidence->order_property != NULL;
        }
    }

    return false;
}

double age_graph_join_path_evidence_credit(
    const AgeGraphJoinPathEvidence *outer_evidence,
    const AgeGraphJoinPathEvidence *inner_evidence)
{
    double credit = 1.0;

    if (outer_evidence != NULL &&
        outer_evidence->candidate_count > 1 &&
        outer_evidence->selected_total_cost > 0 &&
        outer_evidence->next_total_cost > outer_evidence->selected_total_cost)
    {
        credit = Min(credit,
                     Max(0.25,
                         outer_evidence->selected_total_cost /
                         outer_evidence->next_total_cost));
    }
    if (inner_evidence != NULL &&
        inner_evidence->candidate_count > 1 &&
        inner_evidence->selected_total_cost > 0 &&
        inner_evidence->next_total_cost > inner_evidence->selected_total_cost)
    {
        credit = Min(credit,
                     Max(0.25,
                         inner_evidence->selected_total_cost /
                         inner_evidence->next_total_cost));
    }

    return credit;
}

int age_graph_join_table_candidate_count(
    const AgeGraphJoinCandidateTable *table)
{
    if (table == NULL)
        return 0;

    return list_length(table->candidates);
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
                             candidate->component.name));
    descriptor = lappend(descriptor,
                         make_graph_join_text_const(
                             candidate->component.family));
    descriptor = lappend(descriptor,
                         make_graph_join_text_const(
                             candidate->connector.kind));
    descriptor = lappend(descriptor,
                         make_graph_join_text_const(
                             candidate->connector.bound));
    descriptor = lappend(descriptor,
                         make_graph_join_text_const(
                             candidate->connector.order_property));
    descriptor = lappend(descriptor,
                         make_graph_join_text_const(
                             candidate->connector.source_evidence));
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
    descriptor = lappend(descriptor, make_graph_join_text_const("none"));
    descriptor = lappend(descriptor, make_graph_join_text_const("none"));
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
    if (next_best != NULL)
    {
        set_graph_join_descriptor_value(
            descriptor, AGE_GRAPH_JOIN_DESC_NEXT_CONNECTOR,
            (Node *)make_graph_join_text_const(next_best->connector.kind));
        set_graph_join_descriptor_value(
            descriptor, AGE_GRAPH_JOIN_DESC_NEXT_ORDER_PROPERTY,
            (Node *)make_graph_join_text_const(
                next_best->connector.order_property));
        set_graph_join_descriptor_value(
            descriptor, AGE_GRAPH_JOIN_DESC_NEXT_SOURCE_EVIDENCE,
            (Node *)make_graph_join_text_const(
                next_best->connector.source_evidence));
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
