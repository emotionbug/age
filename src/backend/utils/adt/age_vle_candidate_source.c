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

#include "access/age_adjacency.h"
#include "utils/age_vle_candidate_source.h"
#include "utils/age_vle_matrix_frontier_prefetch.h"
#include "utils/age_vle_matrix_frontier_source.h"

typedef struct VLECandidateGraphAccess
{
    GRAPH_global_context *ggctx;
    bool carry_frame_vertex_entry;
} VLECandidateGraphAccess;

typedef struct VLECandidateSourceIdentity
{
    graphid source_vertex_id;
    bool outgoing;
    bool skip_self_loops;
} VLECandidateSourceIdentity;

typedef struct VLEAgeAdjacencyCandidateValidation
{
    VLECandidateGraphAccess graph_access;
    VLECandidateSourceIdentity source_identity;
    Oid edge_label_oid;
    int32 edge_label_id;
    bool use_local_edge_state;
    const VLEEdgePropertyMatchContext *match_context;
} VLEAgeAdjacencyCandidateValidation;

typedef struct VLEAgeAdjacencyPayloadMaterialization
{
    const VLEContextSourceCursor *source_cursor;
    const VLEAgeAdjacencyCandidateValidation *validation;
    const VLECandidateSourceIdentity *source_identity;
    VLEMatrixFrontierPayloadBatch payload_batch;
    bool source_batch_materialization;
    bool local_edge_state_candidate;
    bool local_edge_state_blocked_by_constraints;
    bool local_edge_label_check_required;
    bool next_vertex_entry_required;
} VLEAgeAdjacencyPayloadMaterialization;

typedef struct VLEPackedAdjacencyCandidateValidation
{
    VLECandidateGraphAccess graph_access;
    const VLEEdgePropertyMatchContext *match_context;
} VLEPackedAdjacencyCandidateValidation;

typedef struct VLEEndpointIndexCandidateValidation
{
    VLECandidateGraphAccess graph_access;
    VLECandidateSourceIdentity source_identity;
} VLEEndpointIndexCandidateValidation;

typedef struct VLEAgeAdjacencyScanState
{
    VLE_local_context *vlelctx;
    VLEEdgePropertyMatchContext match_context;
    VLEAgeAdjacencyCandidateValidation validation;
} VLEAgeAdjacencyScanState;

typedef struct VLEAgeAdjacencySourceScan
{
    VLEAgeAdjacencyScanState state;
    const VLEContextSourceCursor *source_cursors;
    int64 source_cursor_count;
    VLEAgeAdjacencyCandidateValidation *validations;
    VLEAgeAdjacencyPayloadMaterialization *materializations;
    VLEMatrixFrontierSourceBlock source_block;
    VLEAgeAdjacencyPayloadMaterialization cached_materialization;
    VLETraversalCandidate *candidate_buffer;
    bool *candidate_pushed;
    VLEMatrixFrontierPayloadBatchItem *payload_batch_items;
    int candidate_buffer_capacity;
    int candidate_buffer_index;
    int candidate_buffer_count;
} VLEAgeAdjacencySourceScan;

typedef struct VLEEndpointIndexCandidateSource
{
    VLEContextEndpointIndexSource *scan;
    VLEEndpointIndexCandidateValidation validation;
    VLE_local_context *vlelctx;
} VLEEndpointIndexCandidateSource;

typedef struct VLEPackedAdjacencySourceScan
{
    VLEContextPackedAdjacencySource *packed_source;
    VLEPackedAdjacencyCandidateValidation validation;
    VLE_local_context *vlelctx;
} VLEPackedAdjacencySourceScan;

typedef struct VLECandidateSource VLECandidateSource;

typedef bool (*VLECandidateSourceNext) (VLECandidateSource *source,
                                        VLETraversalCandidate *candidate);
typedef void (*VLECandidateSourceEnd) (VLECandidateSource *source);
typedef int64 (*VLECandidateSourceDrainBuffered) (
    VLECandidateSource *source, VLE_local_context *vlelctx,
    VLEMatrixFrontierPrefetchCollector *prefetch);

struct VLECandidateSource
{
    void *state;
    VLECandidateSourceNext next_candidate;
    VLECandidateSourceEnd end_source;
    VLECandidateSourceDrainBuffered drain_buffered;
    const char *trace_name;
    VLEContextSourceStatsKind stats_kind;
};

static void init_candidate_source(
    VLECandidateSource *source, void *state,
    VLECandidateSourceNext next_candidate, VLECandidateSourceEnd end_source,
    const char *trace_name, VLEContextSourceStatsKind stats_kind);
static void init_candidate_source_with_buffer_drain(
    VLECandidateSource *source, void *state,
    VLECandidateSourceNext next_candidate, VLECandidateSourceEnd end_source,
    VLECandidateSourceDrainBuffered drain_buffered, const char *trace_name,
    VLEContextSourceStatsKind stats_kind);
static int64 push_candidates_from_source(
    VLE_local_context *vlelctx, VLECandidateSource *source,
    VLEMatrixFrontierPrefetchCollector *prefetch);
static bool add_valid_vertex_edges_from_age_adjacency(
    VLE_local_context *vlelctx,
    const VLEContextSourceCursor *source_cursor,
    const VLEEdgePropertyMatchContext *match_context);
static bool add_valid_vertex_edges_from_age_adjacency_block(
    VLE_local_context *vlelctx,
    const VLEContextSourceCursor *source_cursors, int64 source_cursor_count,
    const VLEEdgePropertyMatchContext *match_context);
static bool add_valid_vertex_edges_from_edge_endpoint_index(
    VLE_local_context *vlelctx,
    const VLEContextSourceCursor *source_cursor,
    const VLEEdgePropertyMatchContext *match_context);
static void init_candidate_graph_access(
    VLECandidateGraphAccess *access, VLE_local_context *vlelctx);
static vertex_entry *candidate_graph_get_vertex_entry(
    const VLECandidateGraphAccess *access, graphid vertex_id);
static vertex_entry *candidate_graph_ensure_vertex_entry_skeleton(
    const VLECandidateGraphAccess *access, graphid vertex_id);
static edge_entry *candidate_graph_get_edge_entry(
    const VLECandidateGraphAccess *access, graphid edge_id);
static bool candidate_graph_get_edge_vle_fields_by_tid(
    const VLECandidateGraphAccess *access, const ItemPointerData *tid,
    graphid *edge_id, Oid *edge_label_table_oid, graphid *start_vertex_id,
    graphid *end_vertex_id, int64 *edge_index,
    vertex_entry **start_vertex_entry, vertex_entry **end_vertex_entry,
    edge_entry **edge);
static void init_candidate_source_identity(
    VLECandidateSourceIdentity *identity,
    const VLEContextSourceCursor *source_cursor);
static bool candidate_source_identity_accepts_edge(
    const VLECandidateSourceIdentity *identity, graphid start_vertex_id,
    graphid end_vertex_id, graphid *next_vertex_id);
static bool candidate_source_identity_accepts_next_vertex(
    const VLECandidateSourceIdentity *identity, graphid next_vertex_id);
static vertex_entry *candidate_source_identity_select_next_entry(
    const VLECandidateSourceIdentity *identity,
    vertex_entry *start_vertex_entry, vertex_entry *end_vertex_entry);
static bool init_age_adjacency_source_scan_block(
    VLEAgeAdjacencySourceScan *scan, VLE_local_context *vlelctx,
    const VLEContextSourceCursor *source_cursors, int64 source_cursor_count,
    const VLEEdgePropertyMatchContext *match_context);
static int age_adjacency_source_scan_candidate_buffer_capacity(
    VLE_local_context *vlelctx, int64 source_cursor_count);
static VLEAgeAdjacencyPayloadMaterialization *
age_adjacency_source_scan_payload_materialization(
    VLEAgeAdjacencySourceScan *scan,
    const VLEContextSourceCursor *payload_cursor,
    const VLEMatrixFrontierPayloadBatch *payload_batch);
static void age_adjacency_payload_materialization_refresh_batch(
    VLEAgeAdjacencyPayloadMaterialization *materialization,
    const VLEMatrixFrontierPayloadBatch *payload_batch);
static void age_adjacency_payload_materialization_init_profile(
    VLEAgeAdjacencyPayloadMaterialization *materialization);
static bool age_adjacency_source_scan_next_candidate(
    VLECandidateSource *source, VLETraversalCandidate *candidate);
static bool age_adjacency_source_scan_pop_buffered_candidate(
    VLEAgeAdjacencySourceScan *scan, VLETraversalCandidate *candidate);
static int64 age_adjacency_source_scan_drain_buffered_candidates(
    VLECandidateSource *source, VLE_local_context *vlelctx,
    VLEMatrixFrontierPrefetchCollector *prefetch);
static bool age_adjacency_source_scan_read_payload(
    VLEAgeAdjacencySourceScan *scan, AgeAdjacencyPayload *payload,
    VLEContextAgeAdjacencyPayloadSource **payload_source,
    const VLEContextSourceCursor **payload_cursor,
    VLEMatrixFrontierPayloadBatch *payload_batch);
static bool age_adjacency_source_scan_buffer_candidate(
    VLECandidateSource *source,
    const VLEAgeAdjacencyPayloadMaterialization *materialization,
    const AgeAdjacencyPayload *payload, graphid next_vertex_id);
static void age_adjacency_source_scan_fill_candidate_batch(
    VLECandidateSource *source,
    const VLEAgeAdjacencyPayloadMaterialization *seed_materialization);
static void age_adjacency_source_scan_fill_candidate_subrun(
    VLECandidateSource *source,
    VLEAgeAdjacencyPayloadMaterialization *materialization,
    int start_index, int end_index);
static void age_adjacency_source_scan_fill_local_candidate_subrun(
    VLECandidateSource *source,
    VLEAgeAdjacencyPayloadMaterialization *materialization,
    int start_index, int end_index);
static void finish_age_adjacency_source_scan(
    VLECandidateSource *source);
static void init_age_adjacency_candidate_validation(
    VLEAgeAdjacencyCandidateValidation *validation, VLE_local_context *vlelctx,
    const VLEContextSourceCursor *source_cursor,
    const VLEEdgePropertyMatchContext *match_context);
static bool init_age_adjacency_payload_candidate(
    const VLEAgeAdjacencyPayloadMaterialization *materialization,
    VLE_local_context *vlelctx, const AgeAdjacencyPayload *payload,
    graphid next_vertex_id, VLETraversalCandidate *candidate,
    VLECandidateMatchResult *match_result);
static bool init_age_adjacency_local_payload_candidate(
    const VLEAgeAdjacencyPayloadMaterialization *materialization,
    VLE_local_context *vlelctx, const AgeAdjacencyPayload *payload,
    graphid next_vertex_id, VLETraversalCandidate *candidate);
static void init_endpoint_index_candidate_validation(
    VLEEndpointIndexCandidateValidation *validation,
    VLE_local_context *vlelctx,
    const VLEContextSourceCursor *source_cursor);
static bool endpoint_index_source_scan_next_candidate(
    VLECandidateSource *source, VLETraversalCandidate *candidate);
static bool init_packed_adjacency_source_scan(
    VLEPackedAdjacencySourceScan *scan, VLE_local_context *vlelctx,
    VLEContextPackedAdjacencySource *packed_source,
    const VLEEdgePropertyMatchContext *match_context);
static bool packed_adjacency_source_scan_next_candidate(
    VLECandidateSource *source, VLETraversalCandidate *candidate);
static void init_packed_adjacency_candidate_validation(
    VLEPackedAdjacencyCandidateValidation *validation,
    VLE_local_context *vlelctx,
    const VLEEdgePropertyMatchContext *match_context);
static void finish_packed_adjacency_source_scan(
    VLECandidateSource *source);
static bool init_packed_adjacency_candidate(
    const VLEPackedAdjacencyCandidateValidation *validation,
    GraphEdgeAdjEntry *adj_entry, VLETraversalCandidate *candidate,
    VLECandidateMatchResult *match_result);
static bool push_candidates_from_source_cursor(
    VLE_local_context *vlelctx, const VLEContextSourceCursor *source_cursor,
    const VLEEdgePropertyMatchContext *match_context);
static bool push_candidates_from_edge_label_source_candidates(
    VLE_local_context *vlelctx, VLEContextExpansionSourceRun *run,
    bool outgoing, const VLEEdgePropertyMatchContext *match_context);
static bool push_candidates_from_expansion_context_source(
    VLE_local_context *vlelctx, VLEContextExpansionSourceRun *run,
    bool outgoing, const VLEEdgePropertyMatchContext *match_context);
static void push_candidates_from_packed_adjacency(
    VLE_local_context *vlelctx, vertex_entry *entry,
    const VLEEdgePropertyMatchContext *match_context,
    const VLEContextExpansionSourceRun *run);

bool age_vle_push_candidates_from_missing_vertex_source(
    VLE_local_context *vlelctx, graphid source_vertex_id)
{
    VLEEdgePropertyMatchContext match_context;
    VLEContextExpansionSourceRun source_run;

    Assert(vlelctx != NULL);
    age_vle_context_record_missing_vertex_attempt(vlelctx);

    age_vle_context_init_missing_vertex_source_run(vlelctx, &source_run,
                                                   source_vertex_id);
    if (!age_vle_context_expansion_source_run_is_eligible(&source_run))
    {
        return false;
    }

    age_vle_context_init_edge_property_match(vlelctx, &match_context);
    (void) push_candidates_from_expansion_context_source(
        vlelctx, &source_run, true, &match_context);
    (void) push_candidates_from_expansion_context_source(
        vlelctx, &source_run, false, &match_context);
    if (source_run.used_out_source || source_run.used_in_source)
        age_vle_context_record_missing_vertex_hit(vlelctx);

    return source_run.used_out_source || source_run.used_in_source;
}

void age_vle_push_candidates_from_vertex_entry(
    VLE_local_context *vlelctx, vertex_entry *entry)
{
    VLEEdgePropertyMatchContext match_context;
    VLEContextExpansionSourceRun source_run;
    graphid source_vertex_id;

    Assert(vlelctx != NULL);
    Assert(entry != NULL);

    age_vle_context_init_edge_property_match(vlelctx, &match_context);
    source_vertex_id = get_vertex_entry_id(entry);
    age_vle_context_init_expansion_source_run(&source_run, source_vertex_id);
    source_run.source_path_length = vlelctx->traversal.path_depth;

    (void) push_candidates_from_expansion_context_source(
        vlelctx, &source_run, true, &match_context);
    (void) push_candidates_from_expansion_context_source(
        vlelctx, &source_run, false, &match_context);

    push_candidates_from_packed_adjacency(
        vlelctx, entry, &match_context, &source_run);
}

static bool push_candidates_from_source_cursor(
    VLE_local_context *vlelctx, const VLEContextSourceCursor *source_cursor,
    const VLEEdgePropertyMatchContext *match_context)
{
    Assert(vlelctx != NULL);
    Assert(source_cursor != NULL);

    switch (source_cursor->source_kind)
    {
        case VLE_TRAVERSAL_SOURCE_AGE_ADJACENCY:
            return add_valid_vertex_edges_from_age_adjacency(
                vlelctx, source_cursor, match_context);

        case VLE_TRAVERSAL_SOURCE_ENDPOINT_BTREE:
            return add_valid_vertex_edges_from_edge_endpoint_index(
                vlelctx, source_cursor, match_context);

        case VLE_TRAVERSAL_SOURCE_NONE:
            break;
    }

    return false;
}

static bool push_candidates_from_expansion_context_source(
    VLE_local_context *vlelctx, VLEContextExpansionSourceRun *run,
    bool outgoing, const VLEEdgePropertyMatchContext *match_context)
{
    VLEContextSourceCursor source_cursor;
    bool used_source = false;

    Assert(vlelctx != NULL);
    Assert(run != NULL);

    if (!age_vle_context_init_expansion_source_cursor(
            vlelctx, run, &source_cursor, outgoing))
    {
        used_source = push_candidates_from_edge_label_source_candidates(
            vlelctx, run, outgoing, match_context);
        if (used_source)
        {
            age_vle_context_record_expansion_source_result(run, outgoing,
                                                           true);
            return true;
        }
        age_vle_context_record_expansion_source_result(run, outgoing, false);
        return false;
    }

    if (age_vle_context_expansion_source_cursor_known_empty(
            vlelctx, &source_cursor))
    {
        age_vle_context_record_expansion_source_result(run, outgoing, true);
        return true;
    }

    used_source = push_candidates_from_source_cursor(
        vlelctx, &source_cursor, match_context);
    age_vle_context_record_expansion_source_result(run, outgoing,
                                                   used_source);
    return used_source;
}

static bool push_candidates_from_edge_label_source_candidates(
    VLE_local_context *vlelctx, VLEContextExpansionSourceRun *run,
    bool outgoing, const VLEEdgePropertyMatchContext *match_context)
{
    GraphEdgeLabelSourceCandidate *candidates;
    VLEContextSourceCursor *source_cursors;
    int candidate_count;
    int64 source_cursor_count = 0;
    int i;
    bool used_source = false;
    bool skip_self_loops;
    cypher_rel_dir direction;

    Assert(vlelctx != NULL);
    Assert(run != NULL);
    Assert(match_context != NULL);

    direction = age_vle_context_edge_direction(vlelctx);
    if ((outgoing && direction == CYPHER_REL_DIR_LEFT) ||
        (!outgoing && direction == CYPHER_REL_DIR_RIGHT))
    {
        return false;
    }

    if (!age_vle_context_expansion_source_run_is_eligible(run) ||
        !age_vle_context_uses_local_edge_state(vlelctx) ||
        age_vle_context_has_edge_label(vlelctx) ||
        age_vle_context_has_edge_property_constraints(vlelctx) ||
        vlelctx->root.uidx_infinite ||
        vlelctx->root.uidx > 1)
    {
        return false;
    }

    candidate_count = get_graph_edge_label_source_candidates(vlelctx->ggctx,
                                                             &candidates);
    if (candidate_count <= 0)
    {
        return false;
    }
    source_cursors =
        palloc0(sizeof(VLEContextSourceCursor) * candidate_count);

    if (run->missing_vertex_fallback)
    {
        skip_self_loops = !outgoing &&
            age_vle_context_edge_direction(vlelctx) == CYPHER_REL_DIR_NONE;
    }
    else
    {
        skip_self_loops = !outgoing && run->used_out_source;
    }

    for (i = 0; i < candidate_count; i++)
    {
        VLEContextSourceCursor *source_cursor;
        Oid index_oid;

        index_oid = outgoing ?
            candidates[i].age_adjacency_out_index_oid :
            candidates[i].age_adjacency_in_index_oid;
        if (!OidIsValid(index_oid))
        {
            continue;
        }

        source_cursor = &source_cursors[source_cursor_count];
        source_cursor->source_vertex_id = run->source_vertex_id;
        source_cursor->source_kind = VLE_TRAVERSAL_SOURCE_AGE_ADJACENCY;
        source_cursor->index_oid = index_oid;
        source_cursor->edge_label_oid = candidates[i].edge_label_oid;
        source_cursor->edge_label_id = candidates[i].label_id;
        source_cursor->terminal_label_id = INVALID_LABEL_ID;
        source_cursor->target_path_length = run->source_path_length + 1;
        source_cursor->outgoing = outgoing;
        source_cursor->skip_self_loops = skip_self_loops;
        source_cursor->has_property_constraints = false;

        if (age_vle_context_expansion_source_cursor_known_empty(
                vlelctx, source_cursor))
        {
            used_source = true;
            continue;
        }

        source_cursor_count++;
    }

    if (source_cursor_count > 0)
    {
        used_source = add_valid_vertex_edges_from_age_adjacency_block(
            vlelctx, source_cursors, source_cursor_count, match_context) ||
            used_source;
    }

    pfree(source_cursors);
    pfree(candidates);
    return used_source;
}

static void push_candidates_from_packed_adjacency(
    VLE_local_context *vlelctx, vertex_entry *entry,
    const VLEEdgePropertyMatchContext *match_context,
    const VLEContextExpansionSourceRun *run)
{
    VLEContextPackedAdjacencySource *packed_context_source;
    VLEPackedAdjacencySourceScan packed_scan;
    VLECandidateSource packed_source;

    Assert(vlelctx != NULL);
    Assert(entry != NULL);
    Assert(match_context != NULL);
    Assert(run != NULL);

    packed_context_source =
        age_vle_context_begin_packed_adjacency_source_from_run(
            vlelctx, entry, run);
    if (packed_context_source == NULL)
        return;

    init_packed_adjacency_source_scan(
        &packed_scan, vlelctx, packed_context_source, match_context);
    init_candidate_source(&packed_source, &packed_scan,
                          packed_adjacency_source_scan_next_candidate,
                          finish_packed_adjacency_source_scan,
                          "packed_adjacency_source",
                          VLE_CONTEXT_SOURCE_STATS_PACKED_ADJACENCY);
    age_vle_context_record_source_scan(
        vlelctx, VLE_CONTEXT_SOURCE_STATS_PACKED_ADJACENCY);
    push_candidates_from_source(vlelctx, &packed_source, NULL);
}

static void init_candidate_source(
    VLECandidateSource *source, void *state,
    VLECandidateSourceNext next_candidate, VLECandidateSourceEnd end_source,
    const char *trace_name, VLEContextSourceStatsKind stats_kind)
{
    init_candidate_source_with_buffer_drain(
        source, state, next_candidate, end_source, NULL, trace_name,
        stats_kind);
}

static void init_candidate_source_with_buffer_drain(
    VLECandidateSource *source, void *state,
    VLECandidateSourceNext next_candidate, VLECandidateSourceEnd end_source,
    VLECandidateSourceDrainBuffered drain_buffered, const char *trace_name,
    VLEContextSourceStatsKind stats_kind)
{
    Assert(source != NULL);
    Assert(state != NULL);
    Assert(next_candidate != NULL);
    Assert(trace_name != NULL);

    source->state = state;
    source->next_candidate = next_candidate;
    source->end_source = end_source;
    source->drain_buffered = drain_buffered;
    source->trace_name = trace_name;
    source->stats_kind = stats_kind;
}

static int64 push_candidates_from_source(
    VLE_local_context *vlelctx, VLECandidateSource *source,
    VLEMatrixFrontierPrefetchCollector *prefetch)
{
    VLETraversalCandidate candidate;
    int64 yielded = 0;

    Assert(vlelctx != NULL);
    Assert(source != NULL);
    Assert(source->next_candidate != NULL);

    while (source->next_candidate(source, &candidate))
    {
        yielded++;
        age_vle_context_record_source_candidate(vlelctx, source->stats_kind);
        if (age_vle_context_push_candidate_if_matched(
                vlelctx, &candidate, source->trace_name))
        {
            age_vle_context_record_source_push(vlelctx);
            age_vle_matrix_frontier_prefetch_collector_add(prefetch, vlelctx,
                                                           &candidate);
        }
        if (source->drain_buffered != NULL)
            yielded += source->drain_buffered(source, vlelctx, prefetch);
    }

    if (source->end_source != NULL)
    {
        source->end_source(source);
    }

    return yielded;
}

static bool add_valid_vertex_edges_from_age_adjacency(
    VLE_local_context *vlelctx,
    const VLEContextSourceCursor *source_cursor,
    const VLEEdgePropertyMatchContext *match_context)
{
    Assert(source_cursor != NULL);

    return add_valid_vertex_edges_from_age_adjacency_block(
        vlelctx, source_cursor, 1, match_context);
}

static bool add_valid_vertex_edges_from_age_adjacency_block(
    VLE_local_context *vlelctx,
    const VLEContextSourceCursor *source_cursors, int64 source_cursor_count,
    const VLEEdgePropertyMatchContext *match_context)
{
    VLEAgeAdjacencySourceScan source_scan;
    VLEMatrixFrontierPrefetchCollector prefetch;
    VLECandidateSource source;
    int64 before_directory_filtered;
    int64 after_directory_filtered;
    int64 yielded;
    int64 i;

    Assert(vlelctx != NULL);
    Assert(source_cursors != NULL);
    Assert(source_cursor_count > 0);
    Assert(match_context != NULL);

    if (!init_age_adjacency_source_scan_block(&source_scan, vlelctx,
                                              source_cursors,
                                              source_cursor_count,
                                              match_context))
        return false;

    if (source_cursor_count == 1 &&
        age_vle_matrix_frontier_source_block_empty_suppressed(
            &source_scan.source_block))
    {
        age_vle_matrix_frontier_source_block_end(
            &source_scan.source_block);
        return true;
    }

    init_candidate_source_with_buffer_drain(
        &source, &source_scan, age_adjacency_source_scan_next_candidate,
        finish_age_adjacency_source_scan,
        age_adjacency_source_scan_drain_buffered_candidates,
        "age_adjacency_source", VLE_CONTEXT_SOURCE_STATS_AGE_ADJACENCY);
    age_vle_matrix_frontier_prefetch_collector_init(
        &prefetch, vlelctx, source_cursors, source_cursor_count);
    for (i = 0; i < source_cursor_count; i++)
        age_vle_context_record_source_scan(
            vlelctx, VLE_CONTEXT_SOURCE_STATS_AGE_ADJACENCY);
    before_directory_filtered =
        age_vle_context_age_adjacency_directory_filtered(vlelctx);
    yielded = push_candidates_from_source(vlelctx, &source, &prefetch);
    age_vle_matrix_frontier_prefetch_collector_flush(&prefetch, vlelctx);
    if (yielded == 0)
    {
        after_directory_filtered =
            age_vle_context_age_adjacency_directory_filtered(vlelctx);
        if (after_directory_filtered > before_directory_filtered)
        {
            age_vle_context_record_age_adjacency_directory_filtered_empty_scan(
                vlelctx);
        }
        else
        {
            age_vle_context_record_source_empty_scan(
                vlelctx, VLE_CONTEXT_SOURCE_STATS_AGE_ADJACENCY);
        }
    }

    return true;
}

static void init_candidate_graph_access(
    VLECandidateGraphAccess *access, VLE_local_context *vlelctx)
{
    Assert(access != NULL);
    Assert(vlelctx != NULL);

    access->ggctx = vlelctx->ggctx;
    access->carry_frame_vertex_entry =
        age_vle_context_carries_frame_vertex_entry(vlelctx);
}

static vertex_entry *candidate_graph_get_vertex_entry(
    const VLECandidateGraphAccess *access, graphid vertex_id)
{
    Assert(access != NULL);
    Assert(access->ggctx != NULL);

    return get_vertex_entry(access->ggctx, vertex_id);
}

static vertex_entry *candidate_graph_ensure_vertex_entry_skeleton(
    const VLECandidateGraphAccess *access, graphid vertex_id)
{
    Assert(access != NULL);
    Assert(access->ggctx != NULL);

    return ensure_vertex_entry_skeleton(access->ggctx, vertex_id);
}

static edge_entry *candidate_graph_get_edge_entry(
    const VLECandidateGraphAccess *access, graphid edge_id)
{
    Assert(access != NULL);
    Assert(access->ggctx != NULL);

    return get_edge_entry(access->ggctx, edge_id);
}

static bool candidate_graph_get_edge_vle_fields_by_tid(
    const VLECandidateGraphAccess *access, const ItemPointerData *tid,
    graphid *edge_id, Oid *edge_label_table_oid, graphid *start_vertex_id,
    graphid *end_vertex_id, int64 *edge_index,
    vertex_entry **start_vertex_entry, vertex_entry **end_vertex_entry,
    edge_entry **edge)
{
    Assert(access != NULL);
    Assert(access->ggctx != NULL);

    return get_edge_entry_vle_fields_by_tid(
        access->ggctx, tid, edge_id, edge_label_table_oid, start_vertex_id,
        end_vertex_id, edge_index, start_vertex_entry, end_vertex_entry, edge);
}

static void init_candidate_source_identity(
    VLECandidateSourceIdentity *identity,
    const VLEContextSourceCursor *source_cursor)
{
    Assert(identity != NULL);
    Assert(source_cursor != NULL);

    identity->source_vertex_id = source_cursor->source_vertex_id;
    identity->outgoing = source_cursor->outgoing;
    identity->skip_self_loops = source_cursor->skip_self_loops;
}

static bool candidate_source_identity_accepts_edge(
    const VLECandidateSourceIdentity *identity, graphid start_vertex_id,
    graphid end_vertex_id, graphid *next_vertex_id)
{
    Assert(identity != NULL);
    Assert(next_vertex_id != NULL);

    if (identity->outgoing)
    {
        if (start_vertex_id != identity->source_vertex_id)
            return false;
        *next_vertex_id = end_vertex_id;
    }
    else
    {
        if (end_vertex_id != identity->source_vertex_id)
            return false;
        *next_vertex_id = start_vertex_id;
    }

    return candidate_source_identity_accepts_next_vertex(identity,
                                                         *next_vertex_id);
}

static bool candidate_source_identity_accepts_next_vertex(
    const VLECandidateSourceIdentity *identity, graphid next_vertex_id)
{
    Assert(identity != NULL);

    return !identity->skip_self_loops ||
           next_vertex_id != identity->source_vertex_id;
}

static vertex_entry *candidate_source_identity_select_next_entry(
    const VLECandidateSourceIdentity *identity,
    vertex_entry *start_vertex_entry, vertex_entry *end_vertex_entry)
{
    Assert(identity != NULL);

    return identity->outgoing ? end_vertex_entry : start_vertex_entry;
}

static bool init_age_adjacency_source_scan_block(
    VLEAgeAdjacencySourceScan *scan, VLE_local_context *vlelctx,
    const VLEContextSourceCursor *source_cursors, int64 source_cursor_count,
    const VLEEdgePropertyMatchContext *match_context)
{
    int64 i;

    Assert(scan != NULL);
    Assert(vlelctx != NULL);
    Assert(source_cursors != NULL);
    Assert(source_cursor_count > 0);
    Assert(match_context != NULL);

    memset(scan, 0, sizeof(*scan));
    scan->state.vlelctx = vlelctx;
    scan->state.match_context = *match_context;
    scan->source_cursors = source_cursors;
    scan->source_cursor_count = source_cursor_count;
    scan->candidate_buffer_capacity =
        age_adjacency_source_scan_candidate_buffer_capacity(
            vlelctx, source_cursor_count);
    scan->candidate_buffer = palloc_array(
        VLETraversalCandidate, scan->candidate_buffer_capacity);
    scan->candidate_pushed = palloc_array(
        bool, scan->candidate_buffer_capacity);
    scan->payload_batch_items = palloc_array(
        VLEMatrixFrontierPayloadBatchItem, scan->candidate_buffer_capacity);
    if (source_cursor_count == 1)
    {
        scan->validations = &scan->state.validation;
        scan->materializations = &scan->cached_materialization;
    }
    else
    {
        scan->validations = palloc0(
            sizeof(VLEAgeAdjacencyCandidateValidation) * source_cursor_count);
        scan->materializations = palloc0(
            sizeof(VLEAgeAdjacencyPayloadMaterialization) *
            source_cursor_count);
    }

    for (i = 0; i < source_cursor_count; i++)
    {
        init_age_adjacency_candidate_validation(
            &scan->validations[i], vlelctx, &source_cursors[i],
            &scan->state.match_context);
        scan->materializations[i].source_cursor = &source_cursors[i];
        scan->materializations[i].validation = &scan->validations[i];
        scan->materializations[i].source_identity =
            &scan->validations[i].source_identity;
        age_adjacency_payload_materialization_init_profile(
            &scan->materializations[i]);
    }

    if (!age_vle_matrix_frontier_source_block_begin(
            &scan->source_block, vlelctx, source_cursors,
            source_cursor_count))
    {
        if (scan->validations != NULL &&
            scan->validations != &scan->state.validation)
        {
            pfree(scan->validations);
            scan->validations = NULL;
        }
        if (scan->materializations != NULL &&
            scan->materializations != &scan->cached_materialization)
        {
            pfree(scan->materializations);
            scan->materializations = NULL;
        }
        pfree(scan->candidate_buffer);
        scan->candidate_buffer = NULL;
        pfree(scan->candidate_pushed);
        scan->candidate_pushed = NULL;
        pfree(scan->payload_batch_items);
        scan->payload_batch_items = NULL;
        scan->candidate_buffer_capacity = 0;
        return false;
    }

    return true;
}

static int
age_adjacency_source_scan_candidate_buffer_capacity(
    VLE_local_context *vlelctx, int64 source_cursor_count)
{
    int capacity;
    int64 planned_batch_size;

    Assert(vlelctx != NULL);
    Assert(source_cursor_count > 0);

    capacity = 64;
    planned_batch_size = Max(source_cursor_count,
                             vlelctx->matrix_frontier_batch_size);
    while (capacity < 1024 && capacity < planned_batch_size)
        capacity *= 2;

    return capacity;
}

static VLEAgeAdjacencyPayloadMaterialization *
age_adjacency_source_scan_payload_materialization(
    VLEAgeAdjacencySourceScan *scan,
    const VLEContextSourceCursor *payload_cursor,
    const VLEMatrixFrontierPayloadBatch *payload_batch)
{
    VLEAgeAdjacencyPayloadMaterialization *materialization;
    int64 cursor_index;

    Assert(scan != NULL);
    Assert(payload_cursor != NULL);
    Assert(scan->source_cursors != NULL);
    Assert(scan->validations != NULL);
    Assert(scan->materializations != NULL);

    cursor_index = payload_cursor - scan->source_cursors;
    Assert(cursor_index >= 0);
    Assert(cursor_index < scan->source_cursor_count);

    materialization = &scan->materializations[cursor_index];
    Assert(materialization->source_cursor == payload_cursor);
    age_adjacency_payload_materialization_refresh_batch(
        materialization, payload_batch);

    return materialization;
}

static void age_adjacency_payload_materialization_refresh_batch(
    VLEAgeAdjacencyPayloadMaterialization *materialization,
    const VLEMatrixFrontierPayloadBatch *payload_batch)
{
    Assert(materialization != NULL);

    if (payload_batch != NULL)
        materialization->payload_batch = *payload_batch;
    else
        memset(&materialization->payload_batch, 0,
               sizeof(materialization->payload_batch));

    materialization->source_batch_materialization =
        materialization->payload_batch.valid &&
        materialization->payload_batch.batch_count > 1 &&
        (materialization->payload_batch.kind ==
         VLE_MATRIX_FRONTIER_PAYLOAD_BATCH_REPLAY_SOURCE ||
         materialization->payload_batch.kind ==
         VLE_MATRIX_FRONTIER_PAYLOAD_BATCH_RAW_RUN_BLOCK);
}

static void age_adjacency_payload_materialization_init_profile(
    VLEAgeAdjacencyPayloadMaterialization *materialization)
{
    const VLEAgeAdjacencyCandidateValidation *validation;

    Assert(materialization != NULL);
    Assert(materialization->validation != NULL);

    validation = materialization->validation;
    materialization->local_edge_state_candidate =
        validation->use_local_edge_state;
    materialization->local_edge_state_blocked_by_constraints =
        validation->use_local_edge_state &&
        age_vle_edge_property_match_context_has_constraints(
            validation->match_context);
    materialization->local_edge_label_check_required =
        validation->use_local_edge_state &&
        label_id_is_valid(validation->edge_label_id);
    materialization->next_vertex_entry_required =
        validation->graph_access.carry_frame_vertex_entry;
}

static bool age_adjacency_source_scan_next_candidate(
    VLECandidateSource *source, VLETraversalCandidate *candidate)
{
    VLEAgeAdjacencySourceScan *scan;
    AgeAdjacencyPayload payload;
    VLECandidateMatchResult match_result;

    Assert(source != NULL);
    Assert(candidate != NULL);

    scan = source->state;
    Assert(scan != NULL);

    if (age_adjacency_source_scan_pop_buffered_candidate(scan, candidate))
        return true;

    while (true)
    {
        graphid next_vertex_id;
        VLEContextAgeAdjacencyPayloadSource *payload_source;
        const VLEContextSourceCursor *payload_cursor;
        VLEMatrixFrontierPayloadBatch payload_batch;
        const VLEAgeAdjacencyPayloadMaterialization *materialization;

        if (!age_adjacency_source_scan_read_payload(
                scan, &payload, &payload_source, &payload_cursor,
                &payload_batch))
            return false;

        materialization = age_adjacency_source_scan_payload_materialization(
            scan, payload_cursor, &payload_batch);
        next_vertex_id = payload.next_vertex_id;
        age_vle_context_maybe_mark_age_adjacency_frontier_empty(
            scan->state.vlelctx, payload_source, next_vertex_id);
        if (!candidate_source_identity_accepts_next_vertex(
                materialization->source_identity, next_vertex_id))
        {
            continue;
        }

        age_vle_context_init_candidate_match_result(
            &match_result, &scan->state.match_context);
        if (!init_age_adjacency_payload_candidate(
                materialization, scan->state.vlelctx, &payload, next_vertex_id,
                candidate, &match_result))
        {
            continue;
        }

        age_vle_context_apply_candidate_match_result(
            scan->state.vlelctx, candidate, &match_result,
            source->trace_name);

        age_adjacency_source_scan_fill_candidate_batch(source,
                                                       materialization);
        return true;
    }
}

static bool age_adjacency_source_scan_pop_buffered_candidate(
    VLEAgeAdjacencySourceScan *scan, VLETraversalCandidate *candidate)
{
    Assert(scan != NULL);
    Assert(candidate != NULL);

    if (scan->candidate_buffer_index >= scan->candidate_buffer_count)
    {
        scan->candidate_buffer_index = 0;
        scan->candidate_buffer_count = 0;
        return false;
    }

    *candidate = scan->candidate_buffer[scan->candidate_buffer_index++];
    return true;
}

static int64 age_adjacency_source_scan_drain_buffered_candidates(
    VLECandidateSource *source, VLE_local_context *vlelctx,
    VLEMatrixFrontierPrefetchCollector *prefetch)
{
    VLEAgeAdjacencySourceScan *scan;
    int64 yielded = 0;

    Assert(source != NULL);
    Assert(vlelctx != NULL);

    scan = source->state;
    Assert(scan != NULL);

    if (scan->candidate_buffer_index < scan->candidate_buffer_count)
    {
        VLEAcceptedCandidateSpan accepted_span;
        int batch_start;
        int batch_count;
        int64 pushed_count;

        batch_start = scan->candidate_buffer_index;
        batch_count = scan->candidate_buffer_count - batch_start;
        pushed_count = age_vle_context_push_candidate_batch_if_matched(
            vlelctx, &scan->candidate_buffer[batch_start], batch_count,
            source->trace_name, scan->candidate_pushed);
        yielded += batch_count;
        age_vle_context_record_source_candidate_batch(
            vlelctx, source->stats_kind, batch_count);
        age_vle_context_record_source_push_batch(vlelctx, pushed_count);

        accepted_span.candidates = &scan->candidate_buffer[batch_start];
        accepted_span.accepted = scan->candidate_pushed;
        accepted_span.candidate_count = batch_count;
        age_vle_matrix_frontier_prefetch_collector_add_span(
            prefetch, vlelctx, &accepted_span);
    }

    scan->candidate_buffer_index = 0;
    scan->candidate_buffer_count = 0;
    return yielded;
}

static bool age_adjacency_source_scan_read_payload(
    VLEAgeAdjacencySourceScan *scan, AgeAdjacencyPayload *payload,
    VLEContextAgeAdjacencyPayloadSource **payload_source,
    const VLEContextSourceCursor **payload_cursor,
    VLEMatrixFrontierPayloadBatch *payload_batch)
{
    Assert(scan != NULL);
    Assert(payload != NULL);
    Assert(payload_source != NULL);
    Assert(payload_cursor != NULL);
    Assert(payload_batch != NULL);

    return age_vle_matrix_frontier_source_block_next(
        &scan->source_block, payload, payload_source, payload_cursor,
        payload_batch);
}

static bool age_adjacency_source_scan_buffer_candidate(
    VLECandidateSource *source,
    const VLEAgeAdjacencyPayloadMaterialization *materialization,
    const AgeAdjacencyPayload *payload, graphid next_vertex_id)
{
    VLEAgeAdjacencySourceScan *scan;
    VLECandidateMatchResult match_result;
    VLETraversalCandidate candidate;

    Assert(source != NULL);
    Assert(materialization != NULL);
    Assert(payload != NULL);

    scan = source->state;
    Assert(scan != NULL);
    Assert(scan->candidate_buffer_count < scan->candidate_buffer_capacity);

    age_vle_context_init_candidate_match_result(
        &match_result, &scan->state.match_context);
    if (!init_age_adjacency_payload_candidate(
            materialization, scan->state.vlelctx, payload, next_vertex_id,
            &candidate, &match_result))
        return false;

    age_vle_context_apply_candidate_match_result(
        scan->state.vlelctx, &candidate, &match_result, source->trace_name);
    scan->candidate_buffer[scan->candidate_buffer_count++] = candidate;
    return true;
}

static void age_adjacency_source_scan_fill_candidate_batch(
    VLECandidateSource *source,
    const VLEAgeAdjacencyPayloadMaterialization *seed_materialization)
{
    VLEAgeAdjacencySourceScan *scan;
    int item_capacity;
    int item_count;
    int i;

    Assert(source != NULL);
    Assert(seed_materialization != NULL);

    scan = source->state;
    Assert(scan != NULL);

    if (!seed_materialization->source_batch_materialization ||
        !seed_materialization->local_edge_state_candidate)
        return;
    if (scan->candidate_buffer_count >= scan->candidate_buffer_capacity)
        return;

    item_capacity = scan->candidate_buffer_capacity -
        scan->candidate_buffer_count;
    item_count = age_vle_matrix_frontier_source_block_next_payload_batch(
        &scan->source_block, &seed_materialization->payload_batch,
        scan->payload_batch_items, item_capacity);
    i = 0;
    while (i < item_count)
    {
        VLEMatrixFrontierPayloadBatchItem *item;
        VLEAgeAdjacencyPayloadMaterialization *materialization;
        int end_index;

        item = &scan->payload_batch_items[i];
        materialization = age_adjacency_source_scan_payload_materialization(
            scan, item->source_cursor, NULL);

        end_index = i + 1;
        while (end_index < item_count &&
               scan->payload_batch_items[end_index].source_cursor ==
               item->source_cursor)
        {
            end_index++;
        }

        age_adjacency_source_scan_fill_candidate_subrun(
            source, materialization, i, end_index);
        i = end_index;
    }
}

static void age_adjacency_source_scan_fill_candidate_subrun(
    VLECandidateSource *source,
    VLEAgeAdjacencyPayloadMaterialization *materialization,
    int start_index, int end_index)
{
    VLEAgeAdjacencySourceScan *scan;
    int i;

    Assert(source != NULL);
    Assert(materialization != NULL);
    Assert(start_index >= 0);
    Assert(end_index >= start_index);

    scan = source->state;
    Assert(scan != NULL);

    if (materialization->local_edge_state_candidate)
    {
        age_adjacency_source_scan_fill_local_candidate_subrun(
            source, materialization, start_index, end_index);
        return;
    }

    for (i = start_index; i < end_index; i++)
    {
        VLEMatrixFrontierPayloadBatchItem *item;
        graphid next_vertex_id;

        item = &scan->payload_batch_items[i];
        Assert(item->source_cursor == materialization->source_cursor);

        age_adjacency_payload_materialization_refresh_batch(
            materialization, &item->payload_batch);
        next_vertex_id = item->payload.next_vertex_id;
        age_vle_context_maybe_mark_age_adjacency_frontier_empty(
            scan->state.vlelctx, item->payload_source, next_vertex_id);
        if (!candidate_source_identity_accepts_next_vertex(
                materialization->source_identity, next_vertex_id))
            continue;

        (void) age_adjacency_source_scan_buffer_candidate(
            source, materialization, &item->payload, next_vertex_id);
    }
}

static void age_adjacency_source_scan_fill_local_candidate_subrun(
    VLECandidateSource *source,
    VLEAgeAdjacencyPayloadMaterialization *materialization,
    int start_index, int end_index)
{
    VLEAgeAdjacencySourceScan *scan;
    const VLEAgeAdjacencyCandidateValidation *validation;
    VLECandidateMatchResult match_result;
    vertex_entry *cached_next_vertex_entry = NULL;
    graphid cached_next_vertex_id = 0;
    bool cached_next_vertex_valid = false;
    int i;

    Assert(source != NULL);
    Assert(materialization != NULL);
    Assert(materialization->local_edge_state_candidate);
    Assert(start_index >= 0);
    Assert(end_index >= start_index);

    scan = source->state;
    Assert(scan != NULL);
    validation = materialization->validation;
    Assert(validation != NULL);

    if (materialization->local_edge_state_blocked_by_constraints)
        return;

    age_vle_context_init_candidate_match_result(
        &match_result, &scan->state.match_context);
    for (i = start_index; i < end_index; i++)
    {
        VLEMatrixFrontierPayloadBatchItem *item;
        VLETraversalCandidate candidate;
        graphid edge_id;
        graphid next_vertex_id;

        item = &scan->payload_batch_items[i];
        Assert(item->source_cursor == materialization->source_cursor);

        age_adjacency_payload_materialization_refresh_batch(
            materialization, &item->payload_batch);
        next_vertex_id = item->payload.next_vertex_id;
        age_vle_context_maybe_mark_age_adjacency_frontier_empty(
            scan->state.vlelctx, item->payload_source, next_vertex_id);
        if (!candidate_source_identity_accepts_next_vertex(
                materialization->source_identity, next_vertex_id))
            continue;

        edge_id = item->payload.edge_id;
        if (materialization->local_edge_label_check_required &&
            get_graphid_label_id(edge_id) != validation->edge_label_id)
            continue;

        candidate.edge_id = edge_id;
        candidate.edge_index = age_vle_context_get_or_create_local_edge_index(
            scan->state.vlelctx, edge_id);
        candidate.next_vertex_id = next_vertex_id;
        if (materialization->next_vertex_entry_required)
        {
            if (!cached_next_vertex_valid ||
                cached_next_vertex_id != next_vertex_id)
            {
                cached_next_vertex_id = next_vertex_id;
                cached_next_vertex_entry =
                    candidate_graph_ensure_vertex_entry_skeleton(
                        &validation->graph_access, next_vertex_id);
                cached_next_vertex_valid = true;
            }
            candidate.next_vertex_entry = cached_next_vertex_entry;
        }
        else
        {
            candidate.next_vertex_entry = NULL;
        }
        age_vle_context_apply_candidate_match_result(
            scan->state.vlelctx, &candidate, &match_result,
            source->trace_name);
        Assert(scan->candidate_buffer_count < scan->candidate_buffer_capacity);
        scan->candidate_buffer[scan->candidate_buffer_count++] = candidate;
    }
}

static void finish_age_adjacency_source_scan(
    VLECandidateSource *source)
{
    VLEAgeAdjacencySourceScan *scan;

    Assert(source != NULL);
    scan = source->state;
    Assert(scan != NULL);

    age_vle_matrix_frontier_source_block_end(&scan->source_block);
    pfree_if_not_null(scan->candidate_buffer);
    scan->candidate_buffer = NULL;
    pfree_if_not_null(scan->candidate_pushed);
    scan->candidate_pushed = NULL;
    pfree_if_not_null(scan->payload_batch_items);
    scan->payload_batch_items = NULL;
    scan->candidate_buffer_capacity = 0;
    if (scan->validations != NULL &&
        scan->validations != &scan->state.validation)
    {
        pfree(scan->validations);
        scan->validations = NULL;
    }
    if (scan->materializations != NULL &&
        scan->materializations != &scan->cached_materialization)
    {
        pfree(scan->materializations);
        scan->materializations = NULL;
    }
}

static bool add_valid_vertex_edges_from_edge_endpoint_index(
    VLE_local_context *vlelctx,
    const VLEContextSourceCursor *source_cursor,
    const VLEEdgePropertyMatchContext *match_context)
{
    VLEEndpointIndexCandidateSource candidate_source;
    VLECandidateSource source;

    (void) match_context;

    Assert(vlelctx != NULL);
    Assert(source_cursor != NULL);

    memset(&candidate_source, 0, sizeof(candidate_source));
    candidate_source.vlelctx = vlelctx;
    init_endpoint_index_candidate_validation(&candidate_source.validation,
                                             vlelctx,
                                             source_cursor);
    candidate_source.scan =
        age_vle_context_begin_endpoint_index_source(source_cursor);
    if (candidate_source.scan == NULL)
        return false;

    init_candidate_source(&source, &candidate_source,
                          endpoint_index_source_scan_next_candidate,
                          NULL, "endpoint_index_source",
                          VLE_CONTEXT_SOURCE_STATS_ENDPOINT_BTREE);
    age_vle_context_record_source_scan(
        vlelctx, VLE_CONTEXT_SOURCE_STATS_ENDPOINT_BTREE);
    if (push_candidates_from_source(vlelctx, &source, NULL) == 0)
    {
        age_vle_context_record_source_empty_scan(
            vlelctx, VLE_CONTEXT_SOURCE_STATS_ENDPOINT_BTREE);
    }
    age_vle_context_end_endpoint_index_source(candidate_source.scan);

    return true;
}

static void init_endpoint_index_candidate_validation(
    VLEEndpointIndexCandidateValidation *validation,
    VLE_local_context *vlelctx,
    const VLEContextSourceCursor *source_cursor)
{
    Assert(validation != NULL);
    Assert(vlelctx != NULL);
    Assert(source_cursor != NULL);

    init_candidate_graph_access(&validation->graph_access, vlelctx);
    init_candidate_source_identity(&validation->source_identity,
                                   source_cursor);
}

static bool endpoint_index_source_scan_next_candidate(
    VLECandidateSource *source, VLETraversalCandidate *candidate)
{
    VLEEndpointIndexCandidateSource *candidate_source;
    VLEContextEndpointIndexSource *scan;
    const VLEEndpointIndexCandidateValidation *validation;
    VLE_local_context *vlelctx;
    VLEEdgePropertyMatchContext no_match_context;

    Assert(source != NULL);
    Assert(candidate != NULL);
    candidate_source = source->state;
    Assert(candidate_source != NULL);
    scan = candidate_source->scan;
    validation = &candidate_source->validation;
    vlelctx = candidate_source->vlelctx;
    Assert(scan != NULL);
    Assert(validation != NULL);
    Assert(vlelctx != NULL);
    memset(&no_match_context, 0, sizeof(no_match_context));

    while (true)
    {
        VLEContextEndpointIndexTuple tuple_data;
        VLECandidateMatchResult match_result;
        graphid edge_id;
        graphid next_vertex_id;
        int64 edge_index;
        vertex_entry *next_vertex_entry;

        if (!age_vle_context_endpoint_index_source_next(scan, &tuple_data))
            return false;
        edge_id = tuple_data.edge_id;
        if (!candidate_source_identity_accepts_edge(
                &validation->source_identity, tuple_data.start_vertex_id,
                tuple_data.end_vertex_id,
                &next_vertex_id))
        {
            continue;
        }

        if (validation->graph_access.carry_frame_vertex_entry)
        {
            (void) candidate_graph_ensure_vertex_entry_skeleton(
                &validation->graph_access,
                validation->source_identity.source_vertex_id);
            next_vertex_entry = candidate_graph_ensure_vertex_entry_skeleton(
                &validation->graph_access, next_vertex_id);
        }
        else
        {
            next_vertex_entry = NULL;
        }
        edge_index = age_vle_context_get_or_create_local_edge_index(
            vlelctx, edge_id);

        candidate->edge_id = edge_id;
        candidate->edge_index = edge_index;
        candidate->next_vertex_id = next_vertex_id;
        candidate->next_vertex_entry = next_vertex_entry;
        age_vle_context_init_candidate_match_result(&match_result,
                                                    &no_match_context);
        age_vle_context_apply_candidate_match_result(
            vlelctx, candidate, &match_result, source->trace_name);

        return true;
    }
}

static bool init_packed_adjacency_source_scan(
    VLEPackedAdjacencySourceScan *scan, VLE_local_context *vlelctx,
    VLEContextPackedAdjacencySource *packed_source,
    const VLEEdgePropertyMatchContext *match_context)
{
    Assert(scan != NULL);
    Assert(vlelctx != NULL);
    Assert(packed_source != NULL);
    Assert(match_context != NULL);

    memset(scan, 0, sizeof(*scan));
    scan->packed_source = packed_source;
    init_packed_adjacency_candidate_validation(&scan->validation, vlelctx,
                                               match_context);
    scan->vlelctx = vlelctx;

    return true;
}

static bool packed_adjacency_source_scan_next_candidate(
    VLECandidateSource *source, VLETraversalCandidate *candidate)
{
    VLEPackedAdjacencySourceScan *scan;
    GraphEdgeAdjEntry *adj_entry = NULL;
    VLECandidateMatchResult match_result;

    Assert(source != NULL);
    Assert(candidate != NULL);

    scan = source->state;
    Assert(scan != NULL);

    while (age_vle_context_packed_adjacency_source_next(
               scan->packed_source, &adj_entry))
    {
        age_vle_context_init_candidate_match_result(
            &match_result, scan->validation.match_context);
        if (!init_packed_adjacency_candidate(&scan->validation, adj_entry,
                                             candidate, &match_result))
        {
            continue;
        }

        age_vle_context_apply_candidate_match_result(
            scan->vlelctx, candidate, &match_result, source->trace_name);

        return true;
    }

    return false;
}

static void finish_packed_adjacency_source_scan(
    VLECandidateSource *source)
{
    VLEPackedAdjacencySourceScan *scan;

    Assert(source != NULL);
    scan = source->state;
    Assert(scan != NULL);

    age_vle_context_end_packed_adjacency_source(scan->packed_source);
    scan->packed_source = NULL;
}

static void init_packed_adjacency_candidate_validation(
    VLEPackedAdjacencyCandidateValidation *validation,
    VLE_local_context *vlelctx,
    const VLEEdgePropertyMatchContext *match_context)
{
    Assert(validation != NULL);
    Assert(vlelctx != NULL);
    Assert(match_context != NULL);

    init_candidate_graph_access(&validation->graph_access, vlelctx);
    validation->match_context = match_context;
}

static bool init_packed_adjacency_candidate(
    const VLEPackedAdjacencyCandidateValidation *validation,
    GraphEdgeAdjEntry *adj_entry, VLETraversalCandidate *candidate,
    VLECandidateMatchResult *match_result)
{
    edge_entry *ee = NULL;

    Assert(validation != NULL);
    Assert(adj_entry != NULL);
    Assert(candidate != NULL);
    Assert(match_result != NULL);

    if (age_vle_edge_property_match_context_has_constraints(
            validation->match_context))
    {
        ee = candidate_graph_get_edge_entry(&validation->graph_access,
                                            adj_entry->edge_id);
        if (ee == NULL)
        {
            elog(ERROR, "add_valid_vertex_edges: no edge found");
        }
        match_result->edge_for_match = ee;
    }

    candidate->edge_id = adj_entry->edge_id;
    candidate->edge_index = adj_entry->edge_index;
    candidate->next_vertex_id = adj_entry->next_vertex_id;
    candidate->next_vertex_entry =
        validation->graph_access.carry_frame_vertex_entry ?
        adj_entry->next_vertex_entry : NULL;

    return true;
}

static void init_age_adjacency_candidate_validation(
    VLEAgeAdjacencyCandidateValidation *validation, VLE_local_context *vlelctx,
    const VLEContextSourceCursor *source_cursor,
    const VLEEdgePropertyMatchContext *match_context)
{
    Assert(validation != NULL);
    Assert(vlelctx != NULL);
    Assert(source_cursor != NULL);
    Assert(match_context != NULL);

    init_candidate_graph_access(&validation->graph_access, vlelctx);
    init_candidate_source_identity(&validation->source_identity,
                                   source_cursor);
    validation->edge_label_oid = source_cursor->edge_label_oid;
    validation->edge_label_id = source_cursor->edge_label_id;
    validation->use_local_edge_state =
        age_vle_context_uses_local_edge_state(vlelctx);
    validation->match_context = match_context;
}

static bool init_age_adjacency_payload_candidate(
    const VLEAgeAdjacencyPayloadMaterialization *materialization,
    VLE_local_context *vlelctx, const AgeAdjacencyPayload *payload,
    graphid next_vertex_id, VLETraversalCandidate *candidate,
    VLECandidateMatchResult *match_result)
{
    const VLEAgeAdjacencyCandidateValidation *validation;
    edge_entry *ee = NULL;
    vertex_entry *next_vertex_entry = NULL;
    vertex_entry *start_vertex_entry = NULL;
    vertex_entry *end_vertex_entry = NULL;
    graphid edge_id;
    graphid loaded_edge_id;
    graphid start_vertex_id;
    graphid end_vertex_id;
    Oid edge_label_table_oid;
    int64 edge_index;

    Assert(materialization != NULL);
    Assert(materialization->validation != NULL);
    Assert(vlelctx != NULL);
    Assert(payload != NULL);
    Assert(candidate != NULL);
    Assert(match_result != NULL);

    validation = materialization->validation;
    edge_id = payload->edge_id;

    if (materialization->local_edge_state_candidate)
        return init_age_adjacency_local_payload_candidate(
            materialization, vlelctx, payload, next_vertex_id, candidate);

    if (!candidate_graph_get_edge_vle_fields_by_tid(
            &validation->graph_access, &payload->heap_tid,
            &loaded_edge_id, &edge_label_table_oid, &start_vertex_id,
            &end_vertex_id, &edge_index, &start_vertex_entry,
            &end_vertex_entry, &ee))
    {
        return false;
    }

    if (loaded_edge_id != edge_id ||
        edge_label_table_oid != validation->edge_label_oid)
    {
        return false;
    }

    if (!candidate_source_identity_accepts_edge(
            &validation->source_identity, start_vertex_id, end_vertex_id,
            &next_vertex_id) ||
        next_vertex_id != payload->next_vertex_id)
    {
        return false;
    }
    next_vertex_entry = candidate_source_identity_select_next_entry(
        &validation->source_identity, start_vertex_entry,
        end_vertex_entry);

    if (next_vertex_entry == NULL)
    {
        next_vertex_entry = candidate_graph_get_vertex_entry(
            &validation->graph_access, next_vertex_id);
    }
    if (next_vertex_entry == NULL)
    {
        return false;
    }

    match_result->edge_for_match = ee;

    candidate->edge_id = edge_id;
    candidate->edge_index = edge_index;
    candidate->next_vertex_id = next_vertex_id;
    candidate->next_vertex_entry =
        materialization->next_vertex_entry_required ? next_vertex_entry : NULL;

    return true;
}

static bool init_age_adjacency_local_payload_candidate(
    const VLEAgeAdjacencyPayloadMaterialization *materialization,
    VLE_local_context *vlelctx, const AgeAdjacencyPayload *payload,
    graphid next_vertex_id, VLETraversalCandidate *candidate)
{
    const VLEAgeAdjacencyCandidateValidation *validation;
    graphid edge_id;

    Assert(materialization != NULL);
    Assert(materialization->validation != NULL);
    Assert(vlelctx != NULL);
    Assert(payload != NULL);
    Assert(candidate != NULL);

    validation = materialization->validation;
    edge_id = payload->edge_id;

    if (materialization->local_edge_state_blocked_by_constraints)
        return false;
    if (materialization->local_edge_label_check_required &&
        get_graphid_label_id(edge_id) != validation->edge_label_id)
        return false;

    candidate->edge_id = edge_id;
    candidate->edge_index =
        age_vle_context_get_or_create_local_edge_index(vlelctx, edge_id);
    candidate->next_vertex_id = next_vertex_id;
    candidate->next_vertex_entry =
        materialization->next_vertex_entry_required ?
        candidate_graph_ensure_vertex_entry_skeleton(
            &validation->graph_access, next_vertex_id) : NULL;

    return true;
}
