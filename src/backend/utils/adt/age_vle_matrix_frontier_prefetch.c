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
#include "utils/age_vle_matrix_frontier_prefetch.h"
#include "utils/age_vle_matrix_frontier_source.h"

static bool matrix_frontier_prefetch_sources_for_direction(
    VLEMatrixFrontierPrefetchCollector *collector,
    VLE_local_context *vlelctx, bool outgoing);
static bool matrix_frontier_prefetch_label_sources_for_direction(
    VLEMatrixFrontierPrefetchCollector *collector,
    VLE_local_context *vlelctx, bool outgoing);
static void matrix_frontier_prefetch_age_adjacency_cursors(
    VLE_local_context *vlelctx, VLEContextSourceCursor *source_cursors,
    int64 source_cursor_count);

void age_vle_matrix_frontier_prefetch_collector_init(
    VLEMatrixFrontierPrefetchCollector *collector,
    VLE_local_context *vlelctx, const VLEContextSourceCursor *source_cursors,
    int64 source_cursor_count)
{
    int64 source_path_length;
    int64 i;

    Assert(collector != NULL);
    Assert(vlelctx != NULL);
    Assert(source_cursors != NULL);
    Assert(source_cursor_count > 0);

    memset(collector, 0, sizeof(*collector));

    if (!vlelctx->matrix_frontier_policy_known ||
        !vlelctx->matrix_frontier_eligible ||
        vlelctx->matrix_frontier_batch_size <= 1)
    {
        return;
    }

    source_path_length = source_cursors[0].target_path_length;
    if (age_vle_context_reached_upper_bound(vlelctx, source_path_length))
        return;

    for (i = 1; i < source_cursor_count; i++)
    {
        if (source_cursors[i].target_path_length != source_path_length)
            return;
    }

    collector->source_capacity = vlelctx->matrix_frontier_batch_size;
    collector->source_vertex_ids = palloc_array(graphid,
                                                collector->source_capacity);
    collector->source_path_length = source_path_length;
    collector->enabled = true;
}

void age_vle_matrix_frontier_prefetch_collector_add(
    VLEMatrixFrontierPrefetchCollector *collector,
    const VLETraversalCandidate *candidate)
{
    int64 i;

    if (collector == NULL || !collector->enabled || candidate == NULL)
        return;

    for (i = 0; i < collector->source_count; i++)
    {
        if (collector->source_vertex_ids[i] == candidate->next_vertex_id)
            return;
    }

    if (collector->source_count >= collector->source_capacity)
        return;

    collector->source_vertex_ids[collector->source_count++] =
        candidate->next_vertex_id;
}

void age_vle_matrix_frontier_prefetch_collector_flush(
    VLEMatrixFrontierPrefetchCollector *collector,
    VLE_local_context *vlelctx)
{
    if (collector == NULL || !collector->enabled)
        return;

    if (collector->source_count > 1)
    {
        (void) matrix_frontier_prefetch_sources_for_direction(
            collector, vlelctx, true);
        (void) matrix_frontier_prefetch_sources_for_direction(
            collector, vlelctx, false);
        (void) matrix_frontier_prefetch_label_sources_for_direction(
            collector, vlelctx, true);
        (void) matrix_frontier_prefetch_label_sources_for_direction(
            collector, vlelctx, false);
    }

    pfree_if_not_null(collector->source_vertex_ids);
    memset(collector, 0, sizeof(*collector));
}

static bool matrix_frontier_prefetch_sources_for_direction(
    VLEMatrixFrontierPrefetchCollector *collector,
    VLE_local_context *vlelctx, bool outgoing)
{
    VLEContextSourceCursor *source_cursors;
    int64 source_cursor_count = 0;
    int64 i;
    bool used_source = false;

    Assert(collector != NULL);
    Assert(vlelctx != NULL);

    if (!age_vle_context_has_edge_label(vlelctx))
        return false;

    source_cursors = palloc0(sizeof(VLEContextSourceCursor) *
                             collector->source_count);
    for (i = 0; i < collector->source_count; i++)
    {
        VLEContextExpansionSourceRun run;
        VLEContextSourceCursor *source_cursor;

        age_vle_context_init_expansion_source_run(
            &run, collector->source_vertex_ids[i]);
        run.source_path_length = collector->source_path_length;
        run.used_out_source = !outgoing &&
            age_vle_context_edge_direction(vlelctx) == CYPHER_REL_DIR_NONE;

        source_cursor = &source_cursors[source_cursor_count];
        if (!age_vle_context_init_expansion_source_cursor(
                vlelctx, &run, source_cursor, outgoing))
        {
            continue;
        }

        source_cursor_count++;
    }

    if (source_cursor_count > 0)
    {
        matrix_frontier_prefetch_age_adjacency_cursors(
            vlelctx, source_cursors, source_cursor_count);
        used_source = true;
    }

    pfree(source_cursors);
    return used_source;
}

static bool matrix_frontier_prefetch_label_sources_for_direction(
    VLEMatrixFrontierPrefetchCollector *collector,
    VLE_local_context *vlelctx, bool outgoing)
{
    GraphEdgeLabelSourceCandidate *candidates;
    VLEContextSourceCursor *source_cursors;
    int candidate_count;
    int64 source_cursor_count = 0;
    int64 source_cursor_capacity;
    int64 source_index;
    int candidate_index;
    bool used_source = false;
    bool skip_self_loops;
    cypher_rel_dir direction;

    Assert(collector != NULL);
    Assert(vlelctx != NULL);

    direction = age_vle_context_edge_direction(vlelctx);
    if ((outgoing && direction == CYPHER_REL_DIR_LEFT) ||
        (!outgoing && direction == CYPHER_REL_DIR_RIGHT) ||
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
        return false;

    source_cursor_capacity = collector->source_count * candidate_count;
    source_cursors =
        palloc0(sizeof(VLEContextSourceCursor) * source_cursor_capacity);
    skip_self_loops = !outgoing && direction == CYPHER_REL_DIR_NONE;

    for (source_index = 0; source_index < collector->source_count;
         source_index++)
    {
        for (candidate_index = 0; candidate_index < candidate_count;
             candidate_index++)
        {
            VLEContextSourceCursor *source_cursor;
            Oid index_oid;

            index_oid = outgoing ?
                candidates[candidate_index].age_adjacency_out_index_oid :
                candidates[candidate_index].age_adjacency_in_index_oid;
            if (!OidIsValid(index_oid))
                continue;

            source_cursor = &source_cursors[source_cursor_count];
            source_cursor->source_vertex_id =
                collector->source_vertex_ids[source_index];
            source_cursor->source_kind = VLE_TRAVERSAL_SOURCE_AGE_ADJACENCY;
            source_cursor->index_oid = index_oid;
            source_cursor->edge_label_oid =
                candidates[candidate_index].edge_label_oid;
            source_cursor->edge_label_id = candidates[candidate_index].label_id;
            source_cursor->terminal_label_id = INVALID_LABEL_ID;
            source_cursor->target_path_length =
                collector->source_path_length + 1;
            source_cursor->outgoing = outgoing;
            source_cursor->skip_self_loops = skip_self_loops;
            source_cursor->has_property_constraints = false;

            source_cursor_count++;
        }
    }

    if (source_cursor_count > 0)
    {
        matrix_frontier_prefetch_age_adjacency_cursors(
            vlelctx, source_cursors, source_cursor_count);
        used_source = true;
    }

    pfree(source_cursors);
    pfree(candidates);
    return used_source;
}

static void matrix_frontier_prefetch_age_adjacency_cursors(
    VLE_local_context *vlelctx, VLEContextSourceCursor *source_cursors,
    int64 source_cursor_count)
{
    VLEMatrixFrontierSourceBlock source_block;
    AgeAdjacencyPayload payload;
    VLEContextAgeAdjacencyPayloadSource *payload_source;
    const VLEContextSourceCursor *payload_cursor;

    Assert(vlelctx != NULL);
    Assert(source_cursors != NULL);
    Assert(source_cursor_count > 0);

    if (!age_vle_matrix_frontier_source_block_begin(
            &source_block, vlelctx, source_cursors, source_cursor_count))
    {
        return;
    }

    while (age_vle_matrix_frontier_source_block_next(
               &source_block, &payload, &payload_source, &payload_cursor))
    {
        (void) payload_cursor;
        age_vle_context_maybe_mark_age_adjacency_frontier_empty(
            vlelctx, payload_source, payload.next_vertex_id);
    }

    age_vle_matrix_frontier_source_block_end(&source_block);
}
