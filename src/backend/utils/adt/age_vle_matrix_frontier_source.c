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

typedef struct VLEMatrixFrontierRawRunFilterState
{
    VLE_local_context *vlelctx;
    VLEContextAgeAdjacencyPayloadSource *source;
    const AgeAdjacencyCompositeTerminalFilter *prepared_filter;
    bool prepared_filter_valid;
    bool prepared_known_empty;
} VLEMatrixFrontierRawRunFilterState;

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
static void age_vle_matrix_frontier_source_block_ensure_run_input_capacity(
    VLEMatrixFrontierSourceBlock *block, int64 capacity);
static VLEMatrixFrontierRunInput *
age_vle_matrix_frontier_source_block_append_run_input(
    VLEMatrixFrontierSourceBlock *block,
    VLEMatrixFrontierRunInputKind kind,
    VLEContextAgeAdjacencyPayloadSource *source,
    const VLEContextSourceCursor *cursor);
static VLEMatrixFrontierRunInputState *
age_vle_matrix_frontier_source_block_find_run_input_state(
    VLEMatrixFrontierSourceBlock *block,
    VLEMatrixFrontierRunInputKind kind);
static void age_vle_matrix_frontier_source_block_release_run_input_states(
    VLEMatrixFrontierSourceBlock *block);
static VLEMatrixFrontierRunInput *
age_vle_matrix_frontier_source_block_append_cursor_input(
    VLEMatrixFrontierSourceBlock *block,
    VLEContextAgeAdjacencyPayloadSource *source,
    const VLEContextSourceCursor *cursor, const AgeAdjacencyPayload *payload);
static bool age_vle_matrix_frontier_source_block_append_replay_input(
    VLEMatrixFrontierSourceBlock *block,
    VLEMatrixFrontierRunInputState **matrix_replay_state,
    VLEContextAgeAdjacencyPayloadSource *source,
    const VLEContextSourceCursor *cursor,
    VLEMatrixFrontierCacheEntry *entry);
static void age_vle_matrix_frontier_source_block_append_raw_merge_input(
    VLEMatrixFrontierSourceBlock *block,
    VLEMatrixFrontierRunInputState **raw_scan_state,
    VLEContextAgeAdjacencyPayloadSource *source,
    const VLEContextSourceCursor *cursor);
static void age_vle_matrix_frontier_source_block_prefilter_raw_merge_inputs(
    VLEMatrixFrontierSourceBlock *block, const VLEContextSourceCursor *cursor);
static void age_vle_matrix_frontier_source_block_begin_raw_merge_scan(
    VLEMatrixFrontierSourceBlock *block,
    const VLEContextSourceCursor *first_cursor,
    VLEMatrixFrontierRunInputState *raw_scan_state);
static bool age_vle_matrix_frontier_source_block_next_cursor_input(
    VLEMatrixFrontierSourceBlock *block, AgeAdjacencyPayload *payload,
    VLEContextAgeAdjacencyPayloadSource **payload_source,
    const VLEContextSourceCursor **source_cursor,
    VLEMatrixFrontierRunInput **selected_input);
static void age_vle_matrix_frontier_source_block_close_raw_scan(
    VLEMatrixFrontierSourceBlock *block);
static bool age_vle_matrix_frontier_source_block_next_replay_input(
    VLEMatrixFrontierSourceBlock *block, AgeAdjacencyPayload *payload,
    VLEContextAgeAdjacencyPayloadSource **payload_source,
    const VLEContextSourceCursor **source_cursor,
    VLEMatrixFrontierRunInput **selected_input);
static bool age_vle_matrix_frontier_source_block_next_raw_input(
    VLEMatrixFrontierSourceBlock *block, AgeAdjacencyPayload *payload,
    VLEContextAgeAdjacencyPayloadSource **payload_source,
    const VLEContextSourceCursor **source_cursor,
    VLEMatrixFrontierRunInput **selected_input);
static bool age_vle_matrix_frontier_source_block_next_run_input_kind(
    VLEMatrixFrontierSourceBlock *block,
    VLEMatrixFrontierRunInputKind kind,
    AgeAdjacencyPayload *payload,
    VLEContextAgeAdjacencyPayloadSource **payload_source,
    const VLEContextSourceCursor **source_cursor,
    VLEMatrixFrontierRunInput **selected_input);
static bool age_vle_matrix_frontier_source_block_next_run_input(
    VLEMatrixFrontierSourceBlock *block, AgeAdjacencyPayload *payload,
    VLEContextAgeAdjacencyPayloadSource **payload_source,
    const VLEContextSourceCursor **source_cursor);
static bool age_vle_matrix_frontier_source_block_prepare_raw_run_filter(
    int64 run_postings, int64 active_postings,
    AgeAdjacencyCompositeTerminalFilter *filter, bool *known_empty,
    void *callback_state);
static void age_vle_matrix_frontier_source_block_accept_raw_run_payload(
    void *tag, const AgeAdjacencyPayload *payload, void *callback_state);
static void age_vle_matrix_frontier_source_block_accept_filtered_raw_key(
    void *tag, void *callback_state);
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

    age_vle_matrix_frontier_source_block_close_raw_scan(block);
    for (i = 0; i < block->run_source_count; i++)
    {
        age_vle_context_end_age_adjacency_payload_source(
            block->vlelctx, block->run_sources[i]);
    }

    block->run_source_count = 0;
    age_vle_matrix_frontier_source_block_release_run_input_states(block);
    block->run_input_count = 0;
    block->raw_source_count = 0;
    block->raw_prefiltered_source_count = 0;
    memset(&block->raw_filter, 0, sizeof(block->raw_filter));
    block->raw_filter_prepared = false;
    block->raw_filter_known_empty = false;
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

static void age_vle_matrix_frontier_source_block_ensure_run_input_capacity(
    VLEMatrixFrontierSourceBlock *block, int64 capacity)
{
    Assert(block != NULL);
    Assert(capacity > 0);

    if (capacity <= block->run_input_capacity)
        return;

    if (block->run_input_capacity == 0)
    {
        block->run_inputs =
            palloc_array(VLEMatrixFrontierRunInput, capacity);
        block->raw_keys =
            palloc_array(AgeAdjacencyVisiblePayloadRunKey, capacity);
    }
    else
    {
        block->run_inputs = repalloc_array(
            block->run_inputs, VLEMatrixFrontierRunInput, capacity);
        block->raw_keys = repalloc_array(
            block->raw_keys, AgeAdjacencyVisiblePayloadRunKey, capacity);
    }

    block->run_input_capacity = capacity;
}

static VLEMatrixFrontierRunInput *
age_vle_matrix_frontier_source_block_append_run_input(
    VLEMatrixFrontierSourceBlock *block,
    VLEMatrixFrontierRunInputKind kind,
    VLEContextAgeAdjacencyPayloadSource *source,
    const VLEContextSourceCursor *cursor)
{
    VLEMatrixFrontierRunInput *input;

    Assert(block != NULL);
    Assert(source != NULL);
    Assert(cursor != NULL);
    Assert(block->run_input_count < block->run_input_capacity);

    input = &block->run_inputs[block->run_input_count++];
    input->kind = kind;
    input->source = source;
    input->cursor = cursor;
    input->state = NULL;
    input->payload_valid = false;
    input->state_owner = false;
    return input;
}

static VLEMatrixFrontierRunInputState *
age_vle_matrix_frontier_source_block_find_run_input_state(
    VLEMatrixFrontierSourceBlock *block,
    VLEMatrixFrontierRunInputKind kind)
{
    int64 i;

    Assert(block != NULL);

    for (i = 0; i < block->run_input_count; i++)
    {
        VLEMatrixFrontierRunInput *input;

        input = &block->run_inputs[i];
        if (input->kind == kind && input->state != NULL)
            return input->state;
    }

    return NULL;
}

static void
age_vle_matrix_frontier_source_block_release_run_input_states(
    VLEMatrixFrontierSourceBlock *block)
{
    int64 i;

    Assert(block != NULL);

    for (i = 0; i < block->run_input_count; i++)
    {
        VLEMatrixFrontierRunInput *input;

        input = &block->run_inputs[i];
        if (input->state_owner && input->state != NULL)
            pfree(input->state);
        input->state = NULL;
        input->state_owner = false;
    }
}

static VLEMatrixFrontierRunInput *
age_vle_matrix_frontier_source_block_append_cursor_input(
    VLEMatrixFrontierSourceBlock *block,
    VLEContextAgeAdjacencyPayloadSource *source,
    const VLEContextSourceCursor *cursor, const AgeAdjacencyPayload *payload)
{
    VLEMatrixFrontierRunInput *input;

    Assert(block != NULL);
    Assert(source != NULL);
    Assert(cursor != NULL);
    Assert(payload != NULL);

    input = age_vle_matrix_frontier_source_block_append_run_input(
        block, VLE_MATRIX_FRONTIER_RUN_INPUT_CURSOR, source, cursor);
    input->payload = *payload;
    input->payload_valid = true;
    return input;
}

static bool age_vle_matrix_frontier_source_block_append_replay_input(
    VLEMatrixFrontierSourceBlock *block,
    VLEMatrixFrontierRunInputState **matrix_replay_state,
    VLEContextAgeAdjacencyPayloadSource *source,
    const VLEContextSourceCursor *cursor,
    VLEMatrixFrontierCacheEntry *entry)
{
    VLEMatrixFrontierRunInput *input;
    bool state_owner;

    Assert(block != NULL);
    Assert(matrix_replay_state != NULL);
    Assert(source != NULL);
    Assert(cursor != NULL);
    Assert(entry != NULL);

    if (*matrix_replay_state != NULL &&
        (*matrix_replay_state)->matrix_replay_entry != entry)
        return false;

    state_owner = false;
    if (*matrix_replay_state == NULL)
    {
        *matrix_replay_state = palloc0(sizeof(VLEMatrixFrontierRunInputState));
        (*matrix_replay_state)->matrix_replay_entry = entry;
        state_owner = true;
    }

    input = age_vle_matrix_frontier_source_block_append_run_input(
        block, VLE_MATRIX_FRONTIER_RUN_INPUT_REPLAY, source, cursor);
    input->state = *matrix_replay_state;
    input->state_owner = state_owner;
    return true;
}

static void age_vle_matrix_frontier_source_block_append_raw_merge_input(
    VLEMatrixFrontierSourceBlock *block,
    VLEMatrixFrontierRunInputState **raw_scan_state,
    VLEContextAgeAdjacencyPayloadSource *source,
    const VLEContextSourceCursor *cursor)
{
    VLEMatrixFrontierRunInput *input;

    Assert(block != NULL);
    Assert(raw_scan_state != NULL);
    Assert(source != NULL);
    Assert(cursor != NULL);

    if (*raw_scan_state == NULL)
        *raw_scan_state = palloc0(sizeof(VLEMatrixFrontierRunInputState));

    input = age_vle_matrix_frontier_source_block_append_run_input(
        block, VLE_MATRIX_FRONTIER_RUN_INPUT_RAW, source, cursor);
    input->state = *raw_scan_state;
    input->state_owner = block->raw_source_count == 0;
    block->raw_keys[block->raw_source_count].key = cursor->source_vertex_id;
    block->raw_keys[block->raw_source_count].tag = input;
    block->raw_source_count++;
}

static void age_vle_matrix_frontier_source_block_prefilter_raw_merge_inputs(
    VLEMatrixFrontierSourceBlock *block, const VLEContextSourceCursor *cursor)
{
    AgeAdjacencyTerminalLabelPostingEstimate *estimates;
    int64 kept_count;
    int64 run_postings;
    int64 terminal_postings;
    int64 i;

    Assert(block != NULL);
    Assert(cursor != NULL);

    if (block->raw_source_count <= 0 ||
        !label_id_is_valid(cursor->terminal_label_id))
    {
        return;
    }

    estimates = palloc_array(AgeAdjacencyTerminalLabelPostingEstimate,
                             block->raw_source_count);
    for (i = 0; i < block->raw_source_count; i++)
        estimates[i].key = block->raw_keys[i].key;

    if (!age_adjacency_estimate_terminal_label_postings_batch(
            cursor->index_oid, cursor->terminal_label_id, estimates,
            block->raw_source_count))
    {
        pfree(estimates);
        return;
    }

    kept_count = 0;
    run_postings = 0;
    terminal_postings = 0;
    for (i = 0; i < block->raw_source_count; i++)
    {
        VLEMatrixFrontierRunInput *input;

        input = block->raw_keys[i].tag;
        Assert(input != NULL);
        if (estimates[i].found && estimates[i].terminal_postings <= 0)
        {
            age_vle_context_age_adjacency_payload_source_mark_empty_no_matrix(
                block->vlelctx, input->source);
            block->raw_prefiltered_source_count++;
            continue;
        }

        if (kept_count != i)
            block->raw_keys[kept_count] = block->raw_keys[i];
        run_postings += estimates[i].run_postings;
        terminal_postings += estimates[i].terminal_postings;
        kept_count++;
    }

    block->raw_source_count = kept_count;
    if (block->raw_source_count > 0)
    {
        VLEMatrixFrontierRunInput *first_input;

        first_input = block->raw_keys[0].tag;
        Assert(first_input != NULL);
        block->raw_filter_prepared =
            age_vle_context_prepare_age_adjacency_payload_source_run_filter(
                block->vlelctx, first_input->source, run_postings,
                terminal_postings, &block->raw_filter,
                &block->raw_filter_known_empty);
    }
    if (block->raw_filter_prepared && block->raw_filter_known_empty)
    {
        for (i = 0; i < block->raw_source_count; i++)
        {
            VLEMatrixFrontierRunInput *input;

            input = block->raw_keys[i].tag;
            Assert(input != NULL);
            age_vle_context_age_adjacency_payload_source_mark_empty_no_matrix(
                block->vlelctx, input->source);
        }
        block->raw_prefiltered_source_count += block->raw_source_count;
        block->raw_source_count = 0;
    }
    else if (block->raw_filter_prepared)
    {
        for (i = 0; i < block->raw_source_count; i++)
            estimates[i].key = block->raw_keys[i].key;
        if (age_adjacency_estimate_composite_terminal_postings_batch(
                cursor->index_oid, &block->raw_filter, estimates,
                block->raw_source_count))
        {
            kept_count = 0;
            for (i = 0; i < block->raw_source_count; i++)
            {
                VLEMatrixFrontierRunInput *input;

                input = block->raw_keys[i].tag;
                Assert(input != NULL);
                if (estimates[i].found && !estimates[i].composite_matches)
                {
                    age_vle_context_age_adjacency_payload_source_mark_empty_no_matrix(
                        block->vlelctx, input->source);
                    block->raw_prefiltered_source_count++;
                    continue;
                }

                if (kept_count != i)
                    block->raw_keys[kept_count] = block->raw_keys[i];
                kept_count++;
            }
            block->raw_source_count = kept_count;
        }
    }
    pfree(estimates);
}

static void age_vle_matrix_frontier_source_block_begin_raw_merge_scan(
    VLEMatrixFrontierSourceBlock *block,
    const VLEContextSourceCursor *first_cursor,
    VLEMatrixFrontierRunInputState *raw_scan_state)
{
    AgeAdjacencyVisiblePayloadRunOptions options;
    VLEMatrixFrontierCacheEntry *matrix_entry;
    VLEMatrixFrontierRawRunFilterState filter_state;
    int64 i;

    Assert(block != NULL);
    Assert(first_cursor != NULL);
    Assert(raw_scan_state != NULL);
    Assert(block->raw_source_count > 0);

    matrix_entry =
        age_vle_context_prepare_age_adjacency_matrix_seed_entry(
            block->vlelctx,
            ((VLEMatrixFrontierRunInput *) block->raw_keys[0].tag)->source);
    for (i = 1; i < block->raw_source_count; i++)
    {
        VLEMatrixFrontierRunInput *input;

        input = block->raw_keys[i].tag;
        Assert(input != NULL);
        age_vle_context_bind_age_adjacency_matrix_seed_entry(
            input->source, matrix_entry);
    }

    memset(&options, 0, sizeof(options));
    filter_state.vlelctx = block->vlelctx;
    filter_state.source =
        ((VLEMatrixFrontierRunInput *) block->raw_keys[0].tag)->source;
    filter_state.prepared_filter = &block->raw_filter;
    filter_state.prepared_filter_valid = block->raw_filter_prepared;
    filter_state.prepared_known_empty = block->raw_filter_known_empty;
    options.terminal_label_id = first_cursor->terminal_label_id;
    options.filter_callback =
        age_vle_matrix_frontier_source_block_prepare_raw_run_filter;
    options.filter_callback_state = &filter_state;
    options.payload_callback =
        age_vle_matrix_frontier_source_block_accept_raw_run_payload;
    options.payload_callback_state = block->vlelctx;
    options.filtered_key_callback =
        age_vle_matrix_frontier_source_block_accept_filtered_raw_key;
    options.filtered_key_callback_state = block->vlelctx;
    raw_scan_state->raw_run_scan =
        age_adjacency_begin_visible_payload_run_scan_with_options(
            first_cursor->index_oid, NULL, false, &options,
            block->raw_keys, block->raw_source_count);
}

static bool age_vle_matrix_frontier_source_block_next_cursor_input(
    VLEMatrixFrontierSourceBlock *block, AgeAdjacencyPayload *payload,
    VLEContextAgeAdjacencyPayloadSource **payload_source,
    const VLEContextSourceCursor **source_cursor,
    VLEMatrixFrontierRunInput **selected_input)
{
    VLEMatrixFrontierRunInput *selected;
    int64 i;

    Assert(block != NULL);
    Assert(payload != NULL);
    Assert(payload_source != NULL);
    Assert(source_cursor != NULL);
    Assert(selected_input != NULL);

    selected = NULL;
    for (i = 0; i < block->run_input_count; i++)
    {
        VLEMatrixFrontierRunInput *candidate;

        candidate = &block->run_inputs[i];
        if (candidate->kind != VLE_MATRIX_FRONTIER_RUN_INPUT_CURSOR)
            continue;
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
        }
    }

    if (selected == NULL)
        return false;

    *payload = selected->payload;
    *payload_source = selected->source;
    *source_cursor = selected->cursor;
    *selected_input = selected;
    selected->payload_valid =
        age_vle_context_age_adjacency_payload_next(
            block->vlelctx, selected->source, &selected->payload);

    return true;
}

static void age_vle_matrix_frontier_source_block_close_raw_scan(
    VLEMatrixFrontierSourceBlock *block)
{
    VLEMatrixFrontierRunInputState *state;
    int64 i;

    Assert(block != NULL);

    state = age_vle_matrix_frontier_source_block_find_run_input_state(
        block, VLE_MATRIX_FRONTIER_RUN_INPUT_RAW);
    if (state == NULL || state->raw_run_scan == NULL)
        return;

    {
        int64 active_keys;

        active_keys = age_adjacency_visible_payload_run_scan_active_keys(
            state->raw_run_scan);
        age_vle_context_record_matrix_frontier_source_run_active_keys(
            block->vlelctx, active_keys);
        age_vle_context_record_matrix_frontier_source_run_filtered_keys(
            block->vlelctx, block->raw_prefiltered_source_count +
            block->raw_source_count - active_keys);
        age_vle_context_record_matrix_frontier_source_run_prefiltered_keys(
            block->vlelctx, block->raw_prefiltered_source_count);
    }

    for (i = 0; i < block->raw_source_count; i++)
    {
        VLEMatrixFrontierRunInput *input;
        AgeAdjacencyVisiblePayloadRunKeyEvidence evidence;

        input = block->raw_keys[i].tag;
        Assert(input != NULL);
        if (age_adjacency_visible_payload_run_scan_key_evidence(
                state->raw_run_scan, i, &evidence))
        {
            age_vle_context_record_matrix_frontier_source_run_evidence(
                block->vlelctx, evidence.run_postings,
                evidence.terminal_postings);
        }
        if (!age_adjacency_visible_payload_run_scan_key_seen(
                state->raw_run_scan, i))
            age_vle_context_age_adjacency_payload_source_mark_empty_no_matrix(
                block->vlelctx, input->source);
    }
    age_adjacency_end_visible_payload_run_scan(state->raw_run_scan);
    state->raw_run_scan = NULL;
}

static bool age_vle_matrix_frontier_source_block_next_replay_input(
    VLEMatrixFrontierSourceBlock *block, AgeAdjacencyPayload *payload,
    VLEContextAgeAdjacencyPayloadSource **payload_source,
    const VLEContextSourceCursor **source_cursor,
    VLEMatrixFrontierRunInput **selected_input)
{
    VLEMatrixFrontierRunInputState *state;

    Assert(block != NULL);
    Assert(payload != NULL);
    Assert(payload_source != NULL);
    Assert(source_cursor != NULL);
    Assert(selected_input != NULL);

    state = age_vle_matrix_frontier_source_block_find_run_input_state(
        block, VLE_MATRIX_FRONTIER_RUN_INPUT_REPLAY);
    if (state == NULL || state->matrix_replay_entry == NULL)
        return false;

    while (state->matrix_payload_index < state->matrix_replay_entry->count)
    {
        VLEMatrixFrontierPayload *matrix_payload;
        int64 input_index;

        matrix_payload =
            &state->matrix_replay_entry->payloads[
                state->matrix_payload_index++];
        for (input_index = 0; input_index < block->run_input_count;
             input_index++)
        {
            VLEMatrixFrontierRunInput *input;

            input = &block->run_inputs[input_index];
            if (input->kind != VLE_MATRIX_FRONTIER_RUN_INPUT_REPLAY)
                continue;
            if (matrix_payload->source_vertex_id !=
                input->cursor->source_vertex_id)
                continue;

            age_vle_context_age_adjacency_payload_source_accept_matrix_replay(
                block->vlelctx, input->source);
            *payload = matrix_payload->payload;
            *payload_source = input->source;
            *source_cursor = input->cursor;
            *selected_input = input;
            return true;
        }
    }

    return false;
}

static bool age_vle_matrix_frontier_source_block_next_raw_input(
    VLEMatrixFrontierSourceBlock *block, AgeAdjacencyPayload *payload,
    VLEContextAgeAdjacencyPayloadSource **payload_source,
    const VLEContextSourceCursor **source_cursor,
    VLEMatrixFrontierRunInput **selected_input)
{
    VLEMatrixFrontierRunInputState *state;
    VLEMatrixFrontierRunInput *raw_source;
    void *tag;

    Assert(block != NULL);
    Assert(payload != NULL);
    Assert(payload_source != NULL);
    Assert(source_cursor != NULL);
    Assert(selected_input != NULL);

    state = age_vle_matrix_frontier_source_block_find_run_input_state(
        block, VLE_MATRIX_FRONTIER_RUN_INPUT_RAW);
    if (state == NULL || state->raw_run_scan == NULL)
        return false;

    if (age_adjacency_visible_payload_run_scan_next_tag(state->raw_run_scan,
                                                        payload, &tag))
    {
        raw_source = tag;
        Assert(raw_source != NULL);

        *payload_source = raw_source->source;
        *source_cursor = raw_source->cursor;
        *selected_input = raw_source;
        return true;
    }

    age_vle_matrix_frontier_source_block_close_raw_scan(block);
    return false;
}

static bool age_vle_matrix_frontier_source_block_next_run_input_kind(
    VLEMatrixFrontierSourceBlock *block,
    VLEMatrixFrontierRunInputKind kind,
    AgeAdjacencyPayload *payload,
    VLEContextAgeAdjacencyPayloadSource **payload_source,
    const VLEContextSourceCursor **source_cursor,
    VLEMatrixFrontierRunInput **selected_input)
{
    Assert(block != NULL);
    Assert(payload != NULL);
    Assert(payload_source != NULL);
    Assert(source_cursor != NULL);
    Assert(selected_input != NULL);

    switch (kind)
    {
        case VLE_MATRIX_FRONTIER_RUN_INPUT_REPLAY:
            return age_vle_matrix_frontier_source_block_next_replay_input(
                block, payload, payload_source, source_cursor,
                selected_input);
        case VLE_MATRIX_FRONTIER_RUN_INPUT_RAW:
            return age_vle_matrix_frontier_source_block_next_raw_input(
                block, payload, payload_source, source_cursor,
                selected_input);
        case VLE_MATRIX_FRONTIER_RUN_INPUT_CURSOR:
            return age_vle_matrix_frontier_source_block_next_cursor_input(
                block, payload, payload_source, source_cursor,
                selected_input);
    }

    return false;
}

static bool age_vle_matrix_frontier_source_block_next_run_input(
    VLEMatrixFrontierSourceBlock *block, AgeAdjacencyPayload *payload,
    VLEContextAgeAdjacencyPayloadSource **payload_source,
    const VLEContextSourceCursor **source_cursor)
{
    VLEMatrixFrontierRunInput *selected_input;
    VLEMatrixFrontierRunInputKind kinds[] = {
        VLE_MATRIX_FRONTIER_RUN_INPUT_REPLAY,
        VLE_MATRIX_FRONTIER_RUN_INPUT_RAW,
        VLE_MATRIX_FRONTIER_RUN_INPUT_CURSOR
    };
    int i;

    Assert(block != NULL);
    Assert(payload != NULL);
    Assert(payload_source != NULL);
    Assert(source_cursor != NULL);

    selected_input = NULL;
    for (i = 0; i < lengthof(kinds); i++)
    {
        if (age_vle_matrix_frontier_source_block_next_run_input_kind(
                block, kinds[i], payload, payload_source, source_cursor,
                &selected_input))
            return true;
    }

    return false;
}

static bool age_vle_matrix_frontier_source_block_prepare_raw_run_filter(
    int64 run_postings, int64 active_postings,
    AgeAdjacencyCompositeTerminalFilter *filter, bool *known_empty,
    void *callback_state)
{
    VLEMatrixFrontierRawRunFilterState *state;

    Assert(filter != NULL);
    Assert(known_empty != NULL);
    Assert(callback_state != NULL);

    state = callback_state;
    if (state->prepared_filter_valid)
    {
        *known_empty = state->prepared_known_empty;
        if (!state->prepared_known_empty)
            *filter = *state->prepared_filter;
        return true;
    }

    return age_vle_context_prepare_age_adjacency_payload_source_run_filter(
        state->vlelctx, state->source, run_postings, active_postings,
        filter, known_empty);
}

static void age_vle_matrix_frontier_source_block_accept_raw_run_payload(
    void *tag, const AgeAdjacencyPayload *payload, void *callback_state)
{
    VLEMatrixFrontierRunInput *raw_source;
    VLE_local_context *vlelctx;

    Assert(tag != NULL);
    Assert(payload != NULL);
    Assert(callback_state != NULL);

    raw_source = tag;
    vlelctx = callback_state;
    age_vle_context_age_adjacency_payload_source_accept_scanned_payload(
        vlelctx, raw_source->source, payload);
}

static void age_vle_matrix_frontier_source_block_accept_filtered_raw_key(
    void *tag, void *callback_state)
{
    VLEMatrixFrontierRunInput *raw_source;
    VLE_local_context *vlelctx;

    Assert(tag != NULL);
    Assert(callback_state != NULL);

    raw_source = tag;
    vlelctx = callback_state;
    age_vle_context_age_adjacency_payload_source_mark_empty_no_matrix(
        vlelctx, raw_source->source);
}

static bool age_vle_matrix_frontier_source_block_drain_run(
    VLEMatrixFrontierSourceBlock *block)
{
    const VLEContextSourceCursor *first_cursor;
    VLEMatrixFrontierRunInputState *matrix_replay_state;
    VLEMatrixFrontierRunInputState *raw_scan_state;
    int64 i;
    int64 run_count;

    Assert(block != NULL);
    Assert(block->matrix_key_valid);
    Assert(block->run_end_index > block->run_start_index);

    first_cursor = block->cursor_order[block->run_start_index];
    run_count = block->run_end_index - block->run_start_index;

    age_vle_matrix_frontier_source_block_cleanup_run(block);
    age_vle_matrix_frontier_source_block_ensure_run_input_capacity(block,
                                                                   run_count);
    matrix_replay_state = NULL;
    raw_scan_state = NULL;
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

        if (age_vle_context_age_adjacency_payload_source_replays_matrix(
                source))
        {
            VLEMatrixFrontierCacheEntry *entry;

            entry = age_vle_context_age_adjacency_payload_source_matrix_entry(
                source);
            if (age_vle_matrix_frontier_source_block_append_replay_input(
                    block, &matrix_replay_state, source, cursor, entry))
            {
                continue;
            }
        }

        if (age_vle_context_age_adjacency_payload_source_uses_visible_scan(
                source))
        {
            age_vle_matrix_frontier_source_block_append_raw_merge_input(
                block, &raw_scan_state, source, cursor);
            continue;
        }

        if (age_vle_context_age_adjacency_payload_next(block->vlelctx, source,
                                                       &payload))
            age_vle_matrix_frontier_source_block_append_cursor_input(
                block, source, cursor, &payload);
    }

    if (block->raw_source_count > 0)
        age_vle_matrix_frontier_source_block_prefilter_raw_merge_inputs(
            block, first_cursor);
    if (block->raw_source_count > 0)
        age_vle_matrix_frontier_source_block_begin_raw_merge_scan(
            block, first_cursor, raw_scan_state);
    else if (block->raw_prefiltered_source_count > 0)
    {
        age_vle_context_record_matrix_frontier_source_run_active_keys(
            block->vlelctx, 0);
        age_vle_context_record_matrix_frontier_source_run_filtered_keys(
            block->vlelctx, block->raw_prefiltered_source_count);
        age_vle_context_record_matrix_frontier_source_run_prefiltered_keys(
            block->vlelctx, block->raw_prefiltered_source_count);
    }

    block->cursor_index = block->run_end_index;
    block->run_batch_active = block->run_source_count > 0;
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
        if (age_vle_matrix_frontier_source_block_next_run_input(
                block, payload, payload_source, source_cursor))
            return true;

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
    pfree_if_not_null(block->run_inputs);
    pfree_if_not_null(block->raw_keys);
    block->cursor_order = NULL;
    block->run_sources = NULL;
    block->run_inputs = NULL;
    block->raw_keys = NULL;
}
