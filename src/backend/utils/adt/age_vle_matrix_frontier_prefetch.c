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
static void matrix_frontier_prefetch_collector_drain(
    VLEMatrixFrontierPrefetchCollector *collector, VLE_local_context *vlelctx);
static void matrix_frontier_prefetch_collector_record_handoff(
    VLEMatrixFrontierPrefetchCollector *collector,
    const VLETraversalFrontierBatch *frontier_batch,
    int accepted_source_count, int unique_source_count);
static int matrix_frontier_prefetch_graphid_cmp(const void *left,
                                                const void *right);
static bool matrix_frontier_prefetch_source_insert_position(
    const graphid *values, int64 value_count, graphid value,
    int64 *insert_index);
static void matrix_frontier_prefetch_collector_add_source(
    VLEMatrixFrontierPrefetchCollector *collector, VLE_local_context *vlelctx,
    graphid source_vertex_id, int64 frontier_path_length);
static void matrix_frontier_prefetch_age_adjacency_cursors(
    VLE_local_context *vlelctx, VLEContextSourceCursor *source_cursors,
    int64 source_cursor_count);

void age_vle_matrix_frontier_prefetch_collector_init(
    VLEMatrixFrontierPrefetchCollector *collector,
    VLE_local_context *vlelctx, const VLEContextSourceCursor *source_cursors,
    int64 source_cursor_count)
{
    int64 frontier_path_length;
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

    frontier_path_length = source_cursors[0].target_path_length;
    if (age_vle_context_reached_upper_bound(vlelctx, frontier_path_length))
        return;

    for (i = 1; i < source_cursor_count; i++)
    {
        if (source_cursors[i].target_path_length != frontier_path_length)
            return;
    }

    collector->source_capacity = vlelctx->matrix_frontier_batch_size;
    collector->source_vertex_ids = palloc_array(graphid,
                                                collector->source_capacity);
    collector->frontier_path_length = frontier_path_length;
    collector->enabled = true;
}

void age_vle_matrix_frontier_prefetch_collector_add_batch(
    VLEMatrixFrontierPrefetchCollector *collector,
    VLE_local_context *vlelctx,
    const VLETraversalFrontierBatch *frontier_batch)
{
    graphid *accepted_source_ids;
    int accepted_source_count = 0;
    int unique_source_count = 0;
    int i;

    if (collector == NULL || !collector->enabled || vlelctx == NULL ||
        frontier_batch == NULL || frontier_batch->candidate_count <= 0 ||
        frontier_batch->frame_count <= 0 || frontier_batch->work_count <= 0)
    {
        return;
    }

    Assert(frontier_batch->accepted != NULL);
    Assert(frontier_batch->candidates != NULL);
    Assert(frontier_batch->path_length == collector->frontier_path_length);
    Assert(frontier_batch->frame_start >= 0);
    Assert(frontier_batch->work_start >= 0);
    Assert(frontier_batch->work_count == frontier_batch->frame_count);
    Assert(frontier_batch->arena_segment_index >= 0);
    Assert(frontier_batch->compaction_evidence.arena_segment_count >
           frontier_batch->arena_segment_index);
    Assert(frontier_batch->iterator_policy == VLE_TRAVERSAL_ITERATOR_LIFO ||
           frontier_batch->iterator_policy ==
           VLE_TRAVERSAL_ITERATOR_LEVEL_BATCH);
    Assert(frontier_batch->parent_frame_index < frontier_batch->frame_start);

    accepted_source_ids = palloc_array(graphid,
                                       frontier_batch->candidate_count);
    for (i = 0; i < frontier_batch->candidate_count; i++)
    {
        if (!frontier_batch->accepted[i])
            continue;

        Assert(frontier_batch->path_length ==
               frontier_batch->candidates[i].path_length);
        accepted_source_ids[accepted_source_count++] =
            frontier_batch->candidates[i].next_vertex_id;
    }

    if (accepted_source_count == 0)
    {
        pfree(accepted_source_ids);
        return;
    }

    qsort(accepted_source_ids, accepted_source_count, sizeof(graphid),
          matrix_frontier_prefetch_graphid_cmp);

    for (i = 0; i < accepted_source_count; i++)
    {
        if (i == 0 || accepted_source_ids[i - 1] != accepted_source_ids[i])
            unique_source_count++;
    }

    if (collector->source_count > 0 &&
        collector->source_count + unique_source_count >
        collector->source_capacity)
    {
        matrix_frontier_prefetch_collector_drain(collector, vlelctx);
    }

    matrix_frontier_prefetch_collector_record_handoff(
        collector, frontier_batch, accepted_source_count,
        unique_source_count);

    for (i = 0; i < accepted_source_count; i++)
    {
        graphid source_vertex_id;

        source_vertex_id = accepted_source_ids[i];
        if (i > 0 && accepted_source_ids[i - 1] == source_vertex_id)
            continue;
        matrix_frontier_prefetch_collector_add_source(
            collector, vlelctx, source_vertex_id,
            frontier_batch->path_length);
    }

    pfree(accepted_source_ids);
}

void age_vle_matrix_frontier_prefetch_collector_flush(
    VLEMatrixFrontierPrefetchCollector *collector,
    VLE_local_context *vlelctx)
{
    if (collector == NULL || !collector->enabled)
        return;

    matrix_frontier_prefetch_collector_drain(collector, vlelctx);
    age_vle_context_record_matrix_frontier_handoff_evidence(
        vlelctx, &collector->compaction_evidence,
        &collector->pressure_evidence);
    pfree_if_not_null(collector->source_vertex_ids);
    memset(collector, 0, sizeof(*collector));
}

static void matrix_frontier_prefetch_collector_drain(
    VLEMatrixFrontierPrefetchCollector *collector, VLE_local_context *vlelctx)
{
    Assert(collector != NULL);
    Assert(vlelctx != NULL);

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

    collector->source_count = 0;
    memset(&collector->pending_pressure_evidence, 0,
           sizeof(collector->pending_pressure_evidence));
}

static void matrix_frontier_prefetch_collector_record_handoff(
    VLEMatrixFrontierPrefetchCollector *collector,
    const VLETraversalFrontierBatch *frontier_batch,
    int accepted_source_count, int unique_source_count)
{
    const VLETraversalCompactionEvidence *batch_evidence;
    VLETraversalCompactionEvidence *collector_evidence;
    VLETraversalFrontierPressureEvidence *pressure;
    VLETraversalFrontierPressureEvidence *pending;
    int64 work_closed_delta = 0;
    int64 compactable_delta = 0;
    int64 compacted_delta = 0;

    Assert(collector != NULL);
    Assert(frontier_batch != NULL);
    Assert(accepted_source_count >= 0);
    Assert(unique_source_count >= 0);

    batch_evidence = &frontier_batch->compaction_evidence;
    collector_evidence = &collector->compaction_evidence;
    pressure = &collector->pressure_evidence;
    pending = &collector->pending_pressure_evidence;
    collector->handoff_count++;
    pressure->handoff_count++;
    pressure->candidate_count += frontier_batch->candidate_count;
    pressure->accepted_count += accepted_source_count;
    pressure->unique_source_count += unique_source_count;
    pressure->max_unique_source_count =
        Max(pressure->max_unique_source_count, unique_source_count);
    pressure->frame_count += frontier_batch->frame_count;
    pressure->work_count += frontier_batch->work_count;
    if (collector->has_compaction_evidence)
    {
        const VLETraversalCompactionEvidence *last;

        last = &collector->last_compaction_evidence;
        work_closed_delta =
            Max(batch_evidence->work_closed_segment_count -
                last->work_closed_segment_count, (int64)0);
        compactable_delta =
            Max(batch_evidence->compactable_frame_count -
                last->compactable_frame_count, (int64)0);
        compacted_delta =
            Max(batch_evidence->compacted_frame_count -
                last->compacted_frame_count, (int64)0);
        pressure->work_closed_segment_delta += work_closed_delta;
        pressure->compactable_frame_delta += compactable_delta;
        pressure->compacted_frame_delta += compacted_delta;
    }
    collector->last_compaction_evidence = *batch_evidence;
    collector->has_compaction_evidence = true;
    collector_evidence->arena_segment_count =
        Max(collector_evidence->arena_segment_count,
            batch_evidence->arena_segment_count);
    collector_evidence->work_closed_segment_count =
        Max(collector_evidence->work_closed_segment_count,
            batch_evidence->work_closed_segment_count);
    collector_evidence->compactable_candidate_count =
        Max(collector_evidence->compactable_candidate_count,
            batch_evidence->compactable_candidate_count);
    collector_evidence->compactable_frame_count =
        Max(collector_evidence->compactable_frame_count,
            batch_evidence->compactable_frame_count);
    collector_evidence->compacted_segment_count =
        Max(collector_evidence->compacted_segment_count,
            batch_evidence->compacted_segment_count);
    collector_evidence->compacted_frame_count =
        Max(collector_evidence->compacted_frame_count,
            batch_evidence->compacted_frame_count);
    pending->handoff_count++;
    pending->candidate_count += frontier_batch->candidate_count;
    pending->accepted_count += accepted_source_count;
    pending->unique_source_count += unique_source_count;
    pending->max_unique_source_count =
        Max(pending->max_unique_source_count, unique_source_count);
    pending->frame_count += frontier_batch->frame_count;
    pending->work_count += frontier_batch->work_count;
    pending->work_closed_segment_delta += work_closed_delta;
    pending->compactable_frame_delta += compactable_delta;
    pending->compacted_frame_delta += compacted_delta;
}

static int matrix_frontier_prefetch_graphid_cmp(const void *left,
                                                const void *right)
{
    graphid left_value = *((const graphid *) left);
    graphid right_value = *((const graphid *) right);

    if (left_value < right_value)
        return -1;
    if (left_value > right_value)
        return 1;
    return 0;
}

static bool matrix_frontier_prefetch_source_insert_position(
    const graphid *values, int64 value_count, graphid value,
    int64 *insert_index)
{
    int64 low = 0;
    int64 high = value_count;

    Assert(insert_index != NULL);

    while (low < high)
    {
        int64 mid = low + (high - low) / 2;

        if (values[mid] == value)
        {
            *insert_index = mid;
            return true;
        }
        if (values[mid] < value)
            low = mid + 1;
        else
            high = mid;
    }

    *insert_index = low;
    return false;
}

static void matrix_frontier_prefetch_collector_add_source(
    VLEMatrixFrontierPrefetchCollector *collector, VLE_local_context *vlelctx,
    graphid source_vertex_id, int64 frontier_path_length)
{
    int64 insert_index;

    Assert(collector != NULL);
    Assert(collector->enabled);
    Assert(vlelctx != NULL);
    Assert(collector->source_vertex_ids != NULL);
    Assert(collector->source_capacity > 0);
    Assert(frontier_path_length == collector->frontier_path_length);

    if (matrix_frontier_prefetch_source_insert_position(
            collector->source_vertex_ids, collector->source_count,
            source_vertex_id, &insert_index))
    {
        return;
    }

    if (collector->source_count >= collector->source_capacity)
    {
        matrix_frontier_prefetch_collector_drain(collector, vlelctx);
        insert_index = 0;
    }

    if (insert_index < collector->source_count)
    {
        memmove(&collector->source_vertex_ids[insert_index + 1],
                &collector->source_vertex_ids[insert_index],
                sizeof(graphid) * (collector->source_count - insert_index));
    }
    collector->source_vertex_ids[insert_index] = source_vertex_id;
    collector->source_count++;
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
        run.source_path_length = collector->frontier_path_length;
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
        age_vle_context_record_matrix_frontier_direction_pressure(
            vlelctx, outgoing, source_cursor_count,
            &collector->pending_pressure_evidence);
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
                collector->frontier_path_length + 1;
            source_cursor->outgoing = outgoing;
            source_cursor->skip_self_loops = skip_self_loops;
            source_cursor->has_property_constraints = false;
            source_cursor->matrix_policy = outgoing ?
                vlelctx->matrix_frontier_out_policy :
                vlelctx->matrix_frontier_in_policy;

            source_cursor_count++;
        }
    }

    if (source_cursor_count > 0)
    {
        age_vle_context_record_matrix_frontier_direction_pressure(
            vlelctx, outgoing, source_cursor_count,
            &collector->pending_pressure_evidence);
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
               &source_block, &payload, &payload_source, &payload_cursor,
               NULL))
    {
        (void) payload_cursor;
        age_vle_context_maybe_mark_age_adjacency_frontier_empty(
            vlelctx, payload_source, payload.next_vertex_id);
    }

    age_vle_matrix_frontier_source_block_end(&source_block);
}
