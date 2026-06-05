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
    bool use_local_edge_state;
    const VLEEdgePropertyMatchContext *match_context;
} VLEAgeAdjacencyCandidateValidation;

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
    VLEContextAgeAdjacencyPayloadSource *payload_source;
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

struct VLECandidateSource
{
    void *state;
    VLECandidateSourceNext next_candidate;
    VLECandidateSourceEnd end_source;
    const char *trace_name;
    VLEContextSourceStatsKind stats_kind;
};

static void init_candidate_source(
    VLECandidateSource *source, void *state,
    VLECandidateSourceNext next_candidate, VLECandidateSourceEnd end_source,
    const char *trace_name, VLEContextSourceStatsKind stats_kind);
static void push_candidates_from_source(
    VLE_local_context *vlelctx, VLECandidateSource *source);
static bool add_valid_vertex_edges_from_age_adjacency(
    VLE_local_context *vlelctx,
    const VLEContextSourceCursor *source_cursor,
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
static bool init_age_adjacency_source_scan(
    VLEAgeAdjacencySourceScan *scan, VLE_local_context *vlelctx,
    const VLEContextSourceCursor *source_cursor,
    const VLEEdgePropertyMatchContext *match_context);
static bool age_adjacency_source_scan_next_candidate(
    VLECandidateSource *source, VLETraversalCandidate *candidate);
static void finish_age_adjacency_source_scan(
    VLECandidateSource *source);
static void init_age_adjacency_candidate_validation(
    VLEAgeAdjacencyCandidateValidation *validation, VLE_local_context *vlelctx,
    const VLEContextSourceCursor *source_cursor,
    const VLEEdgePropertyMatchContext *match_context);
static bool init_age_adjacency_payload_candidate(
    const VLEAgeAdjacencyCandidateValidation *validation,
    VLE_local_context *vlelctx, const AgeAdjacencyPayload *payload,
    graphid next_vertex_id, VLETraversalCandidate *candidate,
    VLECandidateMatchResult *match_result);
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
        age_vle_context_record_expansion_source_result(run, outgoing, false);
        return false;
    }

    used_source = push_candidates_from_source_cursor(
        vlelctx, &source_cursor, match_context);
    age_vle_context_record_expansion_source_result(run, outgoing,
                                                   used_source);
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
    push_candidates_from_source(vlelctx, &packed_source);
}

static void init_candidate_source(
    VLECandidateSource *source, void *state,
    VLECandidateSourceNext next_candidate, VLECandidateSourceEnd end_source,
    const char *trace_name, VLEContextSourceStatsKind stats_kind)
{
    Assert(source != NULL);
    Assert(state != NULL);
    Assert(next_candidate != NULL);
    Assert(trace_name != NULL);

    source->state = state;
    source->next_candidate = next_candidate;
    source->end_source = end_source;
    source->trace_name = trace_name;
    source->stats_kind = stats_kind;
}

static void push_candidates_from_source(
    VLE_local_context *vlelctx, VLECandidateSource *source)
{
    VLETraversalCandidate candidate;

    Assert(vlelctx != NULL);
    Assert(source != NULL);
    Assert(source->next_candidate != NULL);

    while (source->next_candidate(source, &candidate))
    {
        age_vle_context_record_source_candidate(vlelctx, source->stats_kind);
        if (age_vle_context_push_candidate_if_matched(
                vlelctx, &candidate, source->trace_name))
        {
            age_vle_context_record_source_push(vlelctx);
        }
    }

    if (source->end_source != NULL)
    {
        source->end_source(source);
    }
}

static bool add_valid_vertex_edges_from_age_adjacency(
    VLE_local_context *vlelctx,
    const VLEContextSourceCursor *source_cursor,
    const VLEEdgePropertyMatchContext *match_context)
{
    VLEAgeAdjacencySourceScan source_scan;
    VLECandidateSource source;

    Assert(source_cursor != NULL);
    Assert(match_context != NULL);

    if (!init_age_adjacency_source_scan(&source_scan, vlelctx, source_cursor,
                                        match_context))
        return false;

    init_candidate_source(&source, &source_scan,
                          age_adjacency_source_scan_next_candidate,
                          finish_age_adjacency_source_scan,
                          "age_adjacency_source",
                          VLE_CONTEXT_SOURCE_STATS_AGE_ADJACENCY);
    age_vle_context_record_source_scan(
        vlelctx, VLE_CONTEXT_SOURCE_STATS_AGE_ADJACENCY);
    push_candidates_from_source(vlelctx, &source);

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

static bool init_age_adjacency_source_scan(
    VLEAgeAdjacencySourceScan *scan, VLE_local_context *vlelctx,
    const VLEContextSourceCursor *source_cursor,
    const VLEEdgePropertyMatchContext *match_context)
{
    Assert(scan != NULL);
    Assert(vlelctx != NULL);
    Assert(source_cursor != NULL);
    Assert(match_context != NULL);

    memset(scan, 0, sizeof(*scan));
    scan->state.vlelctx = vlelctx;
    scan->state.match_context = *match_context;
    init_age_adjacency_candidate_validation(
        &scan->state.validation, vlelctx, source_cursor,
        &scan->state.match_context);
    scan->payload_source =
        age_vle_context_begin_age_adjacency_payload_source(
            vlelctx, source_cursor);
    return scan->payload_source != NULL;
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

    while (true)
    {
        graphid next_vertex_id;

        if (!age_vle_context_age_adjacency_payload_next(
                scan->state.vlelctx, scan->payload_source, &payload))
            return false;

        next_vertex_id = payload.next_vertex_id;
        if (!candidate_source_identity_accepts_next_vertex(
                &scan->state.validation.source_identity, next_vertex_id))
        {
            continue;
        }

        age_vle_context_init_candidate_match_result(
            &match_result, &scan->state.match_context);
        if (!init_age_adjacency_payload_candidate(
                &scan->state.validation, scan->state.vlelctx,
                &payload, next_vertex_id, candidate, &match_result))
        {
            continue;
        }

        age_vle_context_apply_candidate_match_result(
            scan->state.vlelctx, candidate, &match_result,
            source->trace_name);

        return true;
    }
}

static void finish_age_adjacency_source_scan(
    VLECandidateSource *source)
{
    VLEAgeAdjacencySourceScan *scan;

    Assert(source != NULL);
    scan = source->state;
    Assert(scan != NULL);

    age_vle_context_end_age_adjacency_payload_source(
        scan->payload_source);
    scan->payload_source = NULL;
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
    push_candidates_from_source(vlelctx, &source);
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
    validation->use_local_edge_state =
        age_vle_context_uses_local_edge_state(vlelctx);
    validation->match_context = match_context;
}

static bool init_age_adjacency_payload_candidate(
    const VLEAgeAdjacencyCandidateValidation *validation,
    VLE_local_context *vlelctx, const AgeAdjacencyPayload *payload,
    graphid next_vertex_id, VLETraversalCandidate *candidate,
    VLECandidateMatchResult *match_result)
{
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

    Assert(validation != NULL);
    Assert(vlelctx != NULL);
    Assert(payload != NULL);
    Assert(candidate != NULL);
    Assert(match_result != NULL);

    edge_id = payload->edge_id;

    if (validation->use_local_edge_state)
    {
        if (age_vle_edge_property_match_context_has_constraints(
                validation->match_context))
        {
            return false;
        }
        edge_index = age_vle_context_get_or_create_local_edge_index(
            vlelctx, edge_id);
        next_vertex_entry =
            validation->graph_access.carry_frame_vertex_entry ?
            candidate_graph_get_vertex_entry(&validation->graph_access,
                                             next_vertex_id) : NULL;
    }
    else
    {
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
    }

    candidate->edge_id = edge_id;
    candidate->edge_index = edge_index;
    candidate->next_vertex_id = next_vertex_id;
    candidate->next_vertex_entry =
        validation->graph_access.carry_frame_vertex_entry ?
        next_vertex_entry : NULL;

    return true;
}
