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
#include "utils/age_vle_matrix_frontier_source.h"

static int age_vle_matrix_frontier_source_cursor_compare(
    const void *a, const void *b);
static void age_vle_matrix_frontier_source_block_init_order(
    VLEMatrixFrontierSourceBlock *block);
static bool age_vle_matrix_frontier_cursors_share_run(
    const VLEContextSourceCursor *first,
    const VLEContextSourceCursor *cursor);
static void age_vle_matrix_frontier_source_block_prepare_run(
    VLEMatrixFrontierSourceBlock *block);
static void age_vle_matrix_frontier_source_block_cleanup_run(
    VLEMatrixFrontierSourceBlock *block);
static void age_vle_matrix_frontier_source_block_remember_source(
    VLEMatrixFrontierSourceBlock *block,
    VLEContextAgeAdjacencyPayloadSource *source);
static void age_vle_matrix_frontier_source_block_append_run_cursor(
    VLEMatrixFrontierSourceBlock *block,
    VLEContextAgeAdjacencyPayloadSource *source,
    const VLEContextSourceCursor *cursor, const AgeAdjacencyPayload *payload);
static void age_vle_matrix_frontier_source_block_append_payload(
    VLEMatrixFrontierSourceBlock *block, const AgeAdjacencyPayload *payload,
    VLEContextAgeAdjacencyPayloadSource *source,
    const VLEContextSourceCursor *cursor);
static bool age_vle_matrix_frontier_source_block_pop_next_payload(
    VLEMatrixFrontierSourceBlock *block, VLEMatrixFrontierRunPayload *entry);
static bool age_vle_matrix_frontier_source_block_drain_run(
    VLEMatrixFrontierSourceBlock *block);
static bool age_vle_matrix_frontier_source_block_advance(
    VLEMatrixFrontierSourceBlock *block);

static int age_vle_matrix_frontier_source_cursor_compare(
    const void *a, const void *b)
{
    const VLEContextSourceCursor *left;
    const VLEContextSourceCursor *right;

    left = *((const VLEContextSourceCursor *const *) a);
    right = *((const VLEContextSourceCursor *const *) b);

    if (left->source_kind != right->source_kind)
        return left->source_kind < right->source_kind ? -1 : 1;
    if (left->index_oid != right->index_oid)
        return left->index_oid < right->index_oid ? -1 : 1;
    if (left->edge_label_oid != right->edge_label_oid)
        return left->edge_label_oid < right->edge_label_oid ? -1 : 1;
    if (left->terminal_label_id != right->terminal_label_id)
        return left->terminal_label_id < right->terminal_label_id ? -1 : 1;
    if (left->target_path_length != right->target_path_length)
        return left->target_path_length < right->target_path_length ? -1 : 1;
    if (left->outgoing != right->outgoing)
        return left->outgoing ? -1 : 1;
    if (left->has_property_constraints != right->has_property_constraints)
        return left->has_property_constraints ? 1 : -1;
    if (left->source_vertex_id != right->source_vertex_id)
        return left->source_vertex_id < right->source_vertex_id ? -1 : 1;

    return 0;
}

static void age_vle_matrix_frontier_source_block_init_order(
    VLEMatrixFrontierSourceBlock *block)
{
    int64 i;

    Assert(block != NULL);
    Assert(block->cursors != NULL);
    Assert(block->cursor_count > 0);

    block->cursor_order = palloc(sizeof(VLEContextSourceCursor *) *
                                 block->cursor_count);
    for (i = 0; i < block->cursor_count; i++)
        block->cursor_order[i] = &block->cursors[i];

    qsort(block->cursor_order, block->cursor_count,
          sizeof(VLEContextSourceCursor *),
          age_vle_matrix_frontier_source_cursor_compare);
}

static bool age_vle_matrix_frontier_cursors_share_run(
    const VLEContextSourceCursor *first,
    const VLEContextSourceCursor *cursor)
{
    Assert(first != NULL);
    Assert(cursor != NULL);

    return first->source_kind == VLE_TRAVERSAL_SOURCE_AGE_ADJACENCY &&
           cursor->source_kind == VLE_TRAVERSAL_SOURCE_AGE_ADJACENCY &&
           OidIsValid(first->index_oid) &&
           OidIsValid(cursor->index_oid) &&
           !first->has_property_constraints &&
           !cursor->has_property_constraints &&
           first->index_oid == cursor->index_oid &&
           first->edge_label_oid == cursor->edge_label_oid &&
           first->terminal_label_id == cursor->terminal_label_id &&
           first->target_path_length == cursor->target_path_length &&
           first->outgoing == cursor->outgoing;
}

static void age_vle_matrix_frontier_source_block_prepare_run(
    VLEMatrixFrontierSourceBlock *block)
{
    const VLEContextSourceCursor *first;
    int64 run_start;
    int64 run_end;
    int64 run_count;

    Assert(block != NULL);
    Assert(block->vlelctx != NULL);
    Assert(block->cursor_index < block->cursor_count);
    Assert(block->cursor_order != NULL);

    run_start = block->cursor_index;
    first = block->cursor_order[run_start];
    run_end = run_start + 1;
    while (run_end < block->cursor_count &&
           age_vle_matrix_frontier_cursors_share_run(
               first, block->cursor_order[run_end]))
    {
        run_end++;
    }

    block->run_start_index = run_start;
    block->run_end_index = run_end;
    block->matrix_key_counted = false;
    run_count = run_end - run_start;
    block->matrix_key_valid =
        age_vle_context_init_matrix_frontier_cursor_array_key(
            block->vlelctx, &block->cursor_order[run_start], run_count,
            &block->matrix_key);
    if (block->matrix_key_valid)
        age_vle_context_record_matrix_frontier_source_run(block->vlelctx,
                                                          run_count);
}

static void age_vle_matrix_frontier_source_block_cleanup_run(
    VLEMatrixFrontierSourceBlock *block)
{
    int64 i;

    Assert(block != NULL);

    for (i = 0; i < block->run_source_count; i++)
    {
        age_vle_context_end_age_adjacency_payload_source(
            block->vlelctx, block->run_sources[i]);
    }

    block->run_source_count = 0;
    block->run_cursor_count = 0;
    block->run_payload_count = 0;
    block->run_payload_index = 0;
    block->run_batch_active = false;
}

static void age_vle_matrix_frontier_source_block_remember_source(
    VLEMatrixFrontierSourceBlock *block,
    VLEContextAgeAdjacencyPayloadSource *source)
{
    Assert(block != NULL);
    Assert(source != NULL);

    if (block->run_source_count >= block->run_source_capacity)
    {
        int64 new_capacity;

        new_capacity = block->run_source_capacity == 0 ?
                       8 : block->run_source_capacity * 2;
        if (block->run_sources == NULL)
            block->run_sources = palloc_array(
                VLEContextAgeAdjacencyPayloadSource *, new_capacity);
        else
            block->run_sources = repalloc_array(
                block->run_sources, VLEContextAgeAdjacencyPayloadSource *,
                new_capacity);
        block->run_source_capacity = new_capacity;
    }

    block->run_sources[block->run_source_count++] = source;
}

static void age_vle_matrix_frontier_source_block_append_run_cursor(
    VLEMatrixFrontierSourceBlock *block,
    VLEContextAgeAdjacencyPayloadSource *source,
    const VLEContextSourceCursor *cursor, const AgeAdjacencyPayload *payload)
{
    VLEMatrixFrontierRunCursor *entry;

    Assert(block != NULL);
    Assert(source != NULL);
    Assert(cursor != NULL);
    Assert(payload != NULL);

    if (block->run_cursor_count >= block->run_cursor_capacity)
    {
        int64 new_capacity;

        new_capacity = block->run_cursor_capacity == 0 ?
                       8 : block->run_cursor_capacity * 2;
        if (block->run_cursors == NULL)
            block->run_cursors = palloc_array(VLEMatrixFrontierRunCursor,
                                              new_capacity);
        else
            block->run_cursors = repalloc_array(
                block->run_cursors, VLEMatrixFrontierRunCursor,
                new_capacity);
        block->run_cursor_capacity = new_capacity;
    }

    entry = &block->run_cursors[block->run_cursor_count++];
    entry->source = source;
    entry->cursor = cursor;
    entry->payload = *payload;
    entry->payload_valid = true;
}

static void age_vle_matrix_frontier_source_block_append_payload(
    VLEMatrixFrontierSourceBlock *block, const AgeAdjacencyPayload *payload,
    VLEContextAgeAdjacencyPayloadSource *source,
    const VLEContextSourceCursor *cursor)
{
    VLEMatrixFrontierRunPayload *entry;

    Assert(block != NULL);
    Assert(payload != NULL);
    Assert(source != NULL);
    Assert(cursor != NULL);

    if (block->run_payload_count >= block->run_payload_capacity)
    {
        int64 new_capacity;

        new_capacity = block->run_payload_capacity == 0 ?
                       32 : block->run_payload_capacity * 2;
        if (block->run_payloads == NULL)
            block->run_payloads = palloc_array(VLEMatrixFrontierRunPayload,
                                               new_capacity);
        else
            block->run_payloads = repalloc_array(
                block->run_payloads, VLEMatrixFrontierRunPayload,
                new_capacity);
        block->run_payload_capacity = new_capacity;
    }

    entry = &block->run_payloads[block->run_payload_count++];
    entry->payload = *payload;
    entry->source = source;
    entry->cursor = cursor;
}

static bool age_vle_matrix_frontier_source_block_pop_next_payload(
    VLEMatrixFrontierSourceBlock *block, VLEMatrixFrontierRunPayload *entry)
{
    VLEMatrixFrontierRunCursor *selected;
    int64 selected_index;
    int64 i;

    Assert(block != NULL);
    Assert(entry != NULL);

    selected = NULL;
    selected_index = -1;
    for (i = 0; i < block->run_cursor_count; i++)
    {
        VLEMatrixFrontierRunCursor *candidate;

        candidate = &block->run_cursors[i];
        if (!candidate->payload_valid)
            continue;
        if (selected == NULL ||
            candidate->payload.next_vertex_id <
            selected->payload.next_vertex_id ||
            (candidate->payload.next_vertex_id ==
             selected->payload.next_vertex_id &&
             candidate->payload.edge_id < selected->payload.edge_id))
        {
            selected = candidate;
            selected_index = i;
        }
    }

    if (selected == NULL)
        return false;

    entry->payload = selected->payload;
    entry->source = selected->source;
    entry->cursor = selected->cursor;
    selected->payload_valid =
        age_vle_context_age_adjacency_payload_next(
            block->vlelctx, selected->source, &selected->payload);
    if (!selected->payload_valid && selected_index >= 0)
    {
        while (block->run_cursor_count > 0 &&
               !block->run_cursors[block->run_cursor_count - 1].payload_valid)
        {
            block->run_cursor_count--;
        }
    }

    return true;
}

static bool age_vle_matrix_frontier_source_block_drain_run(
    VLEMatrixFrontierSourceBlock *block)
{
    const VLEContextSourceCursor *first_cursor;
    const VLEContextSourceCursor **raw_cursors;
    VLEContextAgeAdjacencyPayloadSource **raw_sources;
    graphid *raw_keys;
    bool *raw_seen;
    int64 i;
    int64 raw_source_count;
    VLEMatrixFrontierRunPayload entry;

    Assert(block != NULL);
    Assert(block->matrix_key_valid);
    Assert(block->run_end_index > block->run_start_index);

    first_cursor = block->cursor_order[block->run_start_index];
    raw_cursors = palloc_array(const VLEContextSourceCursor *,
                               block->run_end_index -
                               block->run_start_index);
    raw_sources = palloc_array(VLEContextAgeAdjacencyPayloadSource *,
                               block->run_end_index -
                               block->run_start_index);
    raw_keys = palloc_array(graphid,
                            block->run_end_index - block->run_start_index);
    raw_seen = palloc0_array(bool,
                             block->run_end_index - block->run_start_index);
    raw_source_count = 0;

    age_vle_matrix_frontier_source_block_cleanup_run(block);
    for (i = block->run_start_index; i < block->run_end_index; i++)
    {
        const VLEContextSourceCursor *cursor;
        VLEContextAgeAdjacencyPayloadSource *source;
        AgeAdjacencyPayload payload;
        bool record_matrix_block;

        cursor = block->cursor_order[i];
        record_matrix_block =
            block->matrix_key_valid && !block->matrix_key_counted;
        source = age_vle_context_begin_age_adjacency_payload_source_batch(
            block->vlelctx, cursor, &block->matrix_key, record_matrix_block);
        if (source == NULL)
            continue;

        if (record_matrix_block)
            block->matrix_key_counted = true;
        age_vle_matrix_frontier_source_block_remember_source(block, source);

        if (age_vle_context_age_adjacency_payload_source_empty_suppressed(
                source))
            continue;

        if (age_vle_context_age_adjacency_payload_source_uses_visible_scan(
                source))
        {
            raw_cursors[raw_source_count] = cursor;
            raw_sources[raw_source_count] = source;
            raw_keys[raw_source_count] = cursor->source_vertex_id;
            raw_source_count++;
            continue;
        }

        if (age_vle_context_age_adjacency_payload_next(block->vlelctx, source,
                                                       &payload))
            age_vle_matrix_frontier_source_block_append_run_cursor(
                block, source, cursor, &payload);
    }

    if (raw_source_count > 0)
    {
        AgeAdjacencyVisiblePayloadRunScan *run_scan;
        AgeAdjacencyPayload payload;
        int64 key_index;

        run_scan = age_adjacency_begin_visible_payload_run_scan(
            first_cursor->index_oid, NULL, false, first_cursor->terminal_label_id,
            raw_keys, raw_source_count);
        while (age_adjacency_visible_payload_run_scan_next(
                   run_scan, &payload, &key_index))
        {
            VLEContextAgeAdjacencyPayloadSource *source;

            Assert(key_index >= 0);
            Assert(key_index < raw_source_count);

            source = raw_sources[key_index];
            raw_seen[key_index] = true;
            age_vle_context_age_adjacency_payload_source_accept_scanned_payload(
                block->vlelctx, source, &payload);
            age_vle_matrix_frontier_source_block_append_payload(
                block, &payload, source, raw_cursors[key_index]);
        }
        age_adjacency_end_visible_payload_run_scan(run_scan);

        for (i = 0; i < raw_source_count; i++)
        {
            if (!raw_seen[i])
                age_vle_context_age_adjacency_payload_source_mark_empty(
                    block->vlelctx, raw_sources[i]);
        }
    }

    while (age_vle_matrix_frontier_source_block_pop_next_payload(block,
                                                                 &entry))
    {
        age_vle_matrix_frontier_source_block_append_payload(
            block, &entry.payload, entry.source, entry.cursor);
    }

    block->cursor_index = block->run_end_index;
    block->run_batch_active = block->run_source_count > 0;
    pfree(raw_cursors);
    pfree(raw_sources);
    pfree(raw_keys);
    pfree(raw_seen);
    return block->run_batch_active;
}

static bool age_vle_matrix_frontier_source_block_advance(
    VLEMatrixFrontierSourceBlock *block)
{
    Assert(block != NULL);
    Assert(block->vlelctx != NULL);

    if (block->active_source != NULL)
    {
        age_vle_context_end_age_adjacency_payload_source(
            block->vlelctx, block->active_source);
        block->active_source = NULL;
        block->active_cursor = NULL;
        block->active_empty_suppressed = false;
    }
    if (block->run_batch_active)
        age_vle_matrix_frontier_source_block_cleanup_run(block);

    while (block->cursor_index < block->cursor_count)
    {
        const VLEContextSourceCursor *cursor;
        bool record_matrix_block;

        if (block->cursor_index >= block->run_end_index)
            age_vle_matrix_frontier_source_block_prepare_run(block);

        if (block->matrix_key_valid &&
            block->run_end_index - block->run_start_index > 1)
        {
            if (age_vle_matrix_frontier_source_block_drain_run(block))
                return true;
            continue;
        }

        cursor = block->cursor_order[block->cursor_index++];
        record_matrix_block =
            block->matrix_key_valid && !block->matrix_key_counted;
        block->active_source =
            age_vle_context_begin_age_adjacency_payload_source_with_matrix_key(
                block->vlelctx, cursor,
                block->matrix_key_valid ? &block->matrix_key : NULL,
                !block->matrix_key_valid || record_matrix_block);
        if (block->active_source == NULL)
            continue;

        if (record_matrix_block)
            block->matrix_key_counted = true;
        block->active_cursor = cursor;
        block->active_empty_suppressed =
            age_vle_context_age_adjacency_payload_source_empty_suppressed(
                block->active_source);
        return true;
    }

    return false;
}

bool age_vle_matrix_frontier_source_block_begin(
    VLEMatrixFrontierSourceBlock *block, VLE_local_context *vlelctx,
    const VLEContextSourceCursor *cursors, int64 cursor_count)
{
    Assert(block != NULL);
    Assert(vlelctx != NULL);
    Assert(cursors != NULL);
    Assert(cursor_count > 0);

    memset(block, 0, sizeof(*block));
    block->vlelctx = vlelctx;
    block->cursors = cursors;
    block->cursor_count = cursor_count;
    age_vle_matrix_frontier_source_block_init_order(block);

    if (!age_vle_matrix_frontier_source_block_advance(block))
    {
        pfree_if_not_null(block->cursor_order);
        block->cursor_order = NULL;
        return false;
    }

    return true;
}

bool age_vle_matrix_frontier_source_block_empty_suppressed(
    const VLEMatrixFrontierSourceBlock *block)
{
    Assert(block != NULL);

    return block->active_source != NULL && block->active_empty_suppressed;
}

bool age_vle_matrix_frontier_source_block_next(
    VLEMatrixFrontierSourceBlock *block, AgeAdjacencyPayload *payload,
    VLEContextAgeAdjacencyPayloadSource **payload_source,
    const VLEContextSourceCursor **source_cursor)
{
    Assert(block != NULL);
    Assert(payload != NULL);
    Assert(payload_source != NULL);
    Assert(source_cursor != NULL);

    while (block->run_batch_active)
    {
        if (block->run_payload_index < block->run_payload_count)
        {
            VLEMatrixFrontierRunPayload *entry;

            entry = &block->run_payloads[block->run_payload_index++];
            *payload = entry->payload;
            *payload_source = entry->source;
            *source_cursor = entry->cursor;
            return true;
        }

        if (!age_vle_matrix_frontier_source_block_advance(block))
            break;
    }

    while (block->active_source != NULL)
    {
        if (!block->active_empty_suppressed &&
            age_vle_context_age_adjacency_payload_next(
                block->vlelctx, block->active_source, payload))
        {
            *payload_source = block->active_source;
            *source_cursor = block->active_cursor;
            return true;
        }

        if (!age_vle_matrix_frontier_source_block_advance(block))
            break;
    }

    return false;
}

void age_vle_matrix_frontier_source_block_end(
    VLEMatrixFrontierSourceBlock *block)
{
    Assert(block != NULL);

    if (block->active_source != NULL)
    {
        age_vle_context_end_age_adjacency_payload_source(
            block->vlelctx, block->active_source);
        block->active_source = NULL;
        block->active_cursor = NULL;
    }
    age_vle_matrix_frontier_source_block_cleanup_run(block);
    pfree_if_not_null(block->cursor_order);
    pfree_if_not_null(block->run_sources);
    pfree_if_not_null(block->run_cursors);
    pfree_if_not_null(block->run_payloads);
    block->cursor_order = NULL;
    block->run_sources = NULL;
    block->run_cursors = NULL;
    block->run_payloads = NULL;
}
