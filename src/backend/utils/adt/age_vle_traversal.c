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

#include "common/hashfn.h"
#include "utils/age_vle_traversal.h"
#include "utils/graphid.h"

#define VLE_FRAME_STACK_INITIAL_CAPACITY 64
#define VLE_WORKLIST_INITIAL_CAPACITY 64
#define VLE_ARENA_SEGMENT_INITIAL_CAPACITY 64
#define VLE_ARENA_COMPACTION_MIN_FRAME_COUNT 1024
/*
 * Reference discovery used to run after every closed work item.  Delay it
 * until enough newly closed arena state can amortize the scan.
 */
#define VLE_ARENA_REFERENCE_PROBE_SEGMENT_INTERVAL 64
#define VLE_ARENA_REFERENCE_PROBE_FRAME_INTERVAL 1024

typedef struct VLELocalEdgeStateEntry
{
    graphid edge_id;
    int64 edge_index;
} VLELocalEdgeStateEntry;

typedef struct VLETraversalWorkSelection
{
    VLETraversalWorkItem item;
    int64 work_index;
} VLETraversalWorkSelection;

typedef struct VLEArenaCompactionWindow
{
    int64 frame_start;
    int64 frame_count;
} VLEArenaCompactionWindow;

#define VLE_LOCAL_EDGE_STATE_INITIAL_CAPACITY_MAX 1048576

static void age_vle_frame_stack_ensure_capacity(
    VLETraversalFrameStack *stack, int64 required);
static void age_vle_arena_segments_init(
    VLETraversalArenaSegmentList *segments);
static void age_vle_arena_segments_reset(
    VLETraversalArenaSegmentList *segments);
static void age_vle_arena_segments_free(
    VLETraversalArenaSegmentList *segments);
static int64 age_vle_arena_segment_begin(
    VLETraversalState *state, int64 frame_start, int64 work_start,
    int64 path_length, int64 parent_frame_index,
    VLETraversalIteratorPolicy iterator_policy);
static void age_vle_arena_segment_record_frame_work(
    VLETraversalState *state, int64 arena_segment_index);
static void age_vle_arena_segment_record_work_consumed(
    VLETraversalState *state, int64 arena_segment_index);
static void age_vle_arena_segments_mark_referenced_chain(
    const VLETraversalState *state, bool *referenced, int64 frame_index);
static void age_vle_arena_segments_refresh_compactable_candidates(
    VLETraversalState *state);
static int64 age_vle_arena_frame_index_after_compaction_windows(
    int64 frame_index, const VLEArenaCompactionWindow *windows,
    int64 window_count);
static void age_vle_arena_segments_compact_windows(VLETraversalState *state);
static int64 age_vle_arena_segments_collect_compaction_windows(
    VLETraversalState *state, VLEArenaCompactionWindow *windows,
    int64 max_window_count, int64 *total_frame_count);
static void age_vle_arena_segments_move_frame_windows(
    VLETraversalFrameStack *stack, const VLEArenaCompactionWindow *windows,
    int64 window_count, int64 total_frame_count);
static void age_vle_arena_segments_rewrite_frame_indexes_for_windows(
    VLETraversalState *state, const VLEArenaCompactionWindow *windows,
    int64 window_count);
static void age_vle_arena_segments_ensure_capacity(
    VLETraversalArenaSegmentList *segments, int64 required);
static void age_vle_worklist_ensure_capacity(
    VLETraversalWorklist *worklist, int64 required);
static void age_vle_worklist_push(VLETraversalWorklist *worklist,
                                  int64 frame_index, int64 path_length,
                                  int64 arena_segment_index,
                                  VLETraversalWorkItemKind kind);
static bool age_vle_worklist_select_next(
    VLETraversalState *state, VLETraversalWorkSelection *selection);
static bool age_vle_worklist_select_lifo(
    VLETraversalWorklist *worklist, VLETraversalWorkSelection *selection);
static bool age_vle_worklist_select_level_batch(
    VLETraversalState *state, VLETraversalWorkSelection *selection);
static void age_vle_worklist_remove_at(VLETraversalWorklist *worklist,
                                       int64 work_index);
static bool age_vle_worklist_is_empty(VLETraversalWorklist *worklist);
static bool age_vle_traversal_uses_path_replay_iterator(
    const VLETraversalState *state);
static void age_vle_path_replay_ensure_capacity(VLETraversalState *state,
                                                int64 required);
static void age_vle_traversal_clear_active_path(VLETraversalState *state,
                                                const char *caller);
static void age_vle_traversal_switch_active_path(
    VLETraversalState *state, int64 target_frame_index, const char *caller);
static bool age_vle_traversal_iterator_policy_is_valid(
    VLETraversalIteratorPolicy iterator_policy);
static void age_vle_frontier_batch_init(
    const VLETraversalState *state, VLETraversalFrontierBatch *frontier_batch,
    const VLETraversalCandidate *candidates, const bool *accepted,
    int candidate_count, int64 path_length, int64 frame_start,
    int64 work_start, VLETraversalIteratorPolicy iterator_policy,
    int64 parent_frame_index);
static void age_vle_edge_state_ensure_capacity(VLELocalEdgeState *state,
                                               int64 required);
static int64 age_vle_edge_state_get_or_create_index(
    VLELocalEdgeState *state, graphid edge_id);

void age_vle_acceptance_init(VLETraversalAcceptance *acceptance,
                             int64 lower_bound, int64 upper_bound,
                             bool upper_unbounded)
{
    Assert(acceptance != NULL);

    acceptance->lower_bound = lower_bound;
    acceptance->upper_bound = upper_bound;
    acceptance->upper_unbounded = upper_unbounded;
    acceptance->require_terminal = false;
    acceptance->terminal_id = 0;
    acceptance->require_terminal_label = false;
    acceptance->terminal_label_id = -1;
}

void age_vle_acceptance_require_terminal(
    VLETraversalAcceptance *acceptance, graphid terminal_id)
{
    Assert(acceptance != NULL);

    acceptance->require_terminal = true;
    acceptance->terminal_id = terminal_id;
}

void age_vle_acceptance_require_terminal_label(
    VLETraversalAcceptance *acceptance, int32 terminal_label_id)
{
    Assert(acceptance != NULL);

    acceptance->require_terminal_label = true;
    acceptance->terminal_label_id = terminal_label_id;
}

static bool age_vle_path_length_in_acceptance_range(
    const VLETraversalAcceptance *acceptance, int64 path_length)
{
    Assert(acceptance != NULL);

    return path_length >= acceptance->lower_bound &&
           (acceptance->upper_unbounded ||
            path_length <= acceptance->upper_bound);
}

bool age_vle_accepts_path(const VLETraversalAcceptance *acceptance,
                          graphid terminal_id, int64 path_length)
{
    Assert(acceptance != NULL);

    if (acceptance->require_terminal &&
        terminal_id != acceptance->terminal_id)
    {
        return false;
    }
    if (acceptance->require_terminal_label &&
        get_graphid_label_id(terminal_id) != acceptance->terminal_label_id)
    {
        return false;
    }

    return age_vle_path_length_in_acceptance_range(acceptance, path_length);
}

bool age_vle_terminal_over_upper_bound(
    const VLETraversalAcceptance *acceptance, graphid terminal_id,
    int64 path_length)
{
    Assert(acceptance != NULL);

    return acceptance->require_terminal &&
           terminal_id == acceptance->terminal_id &&
           !acceptance->upper_unbounded &&
           path_length > acceptance->upper_bound;
}

bool age_vle_accepts_step(const VLETraversalAcceptance *acceptance,
                          const VLETraversalStep *step)
{
    Assert(step != NULL);

    return age_vle_accepts_path(acceptance, step->vertex_id,
                                step->path_length);
}

bool age_vle_step_over_upper_bound(const VLETraversalAcceptance *acceptance,
                                   const VLETraversalStep *step)
{
    Assert(step != NULL);

    return age_vle_terminal_over_upper_bound(acceptance, step->vertex_id,
                                             step->path_length);
}

void age_vle_traversal_state_init(VLETraversalState *state,
                                  bool store_path_edges)
{
    Assert(state != NULL);

    state->frame_stack = age_vle_frame_stack_new();
    state->worklist = age_vle_worklist_new();
    age_vle_arena_segments_init(&state->arena_segments);
    state->path_stack = store_path_edges ? new_gid_stack() : NULL;
    state->path_edge_index_stack = new_gid_stack();
    state->path_vertex_stack = new_gid_stack();
    state->path_replay_frame_indexes = NULL;
    state->path_replay_frame_capacity = 0;
    state->store_path_edges = store_path_edges;
    state->path_depth = 0;
    state->active_frame_index = -1;
}

void age_vle_traversal_state_set_iterator_policy(
    VLETraversalState *state, VLETraversalIteratorPolicy iterator_policy)
{
    Assert(state != NULL);
    Assert(state->worklist != NULL);
    if (!age_vle_traversal_iterator_policy_is_valid(iterator_policy))
        elog(ERROR, "invalid VLE traversal iterator policy");
    Assert(state->worklist->size == 0);
    Assert(state->path_depth == 0);
    Assert(state->active_frame_index == -1);

    state->worklist->iterator_policy = iterator_policy;
}

void age_vle_traversal_state_free(VLETraversalState *state)
{
    Assert(state != NULL);

    age_vle_edge_state_free(&state->edge_state);
    if (state->frame_stack != NULL)
    {
        age_vle_frame_stack_free(state->frame_stack);
    }
    if (state->worklist != NULL)
    {
        age_vle_worklist_free(state->worklist);
    }
    age_vle_arena_segments_free(&state->arena_segments);
    if (state->path_stack != NULL)
    {
        free_gid_stack(state->path_stack);
    }
    if (state->path_edge_index_stack != NULL)
    {
        free_gid_stack(state->path_edge_index_stack);
    }
    if (state->path_vertex_stack != NULL)
    {
        free_gid_stack(state->path_vertex_stack);
    }
    pfree_if_not_null(state->path_replay_frame_indexes);

    memset(state, 0, sizeof(*state));
}

void age_vle_traversal_state_reset(VLETraversalState *state,
                                   graphid start_vertex_id)
{
    Assert(state != NULL);
    Assert(state->frame_stack != NULL);
    Assert(state->worklist != NULL);
    Assert(state->path_edge_index_stack != NULL);
    Assert(state->path_vertex_stack != NULL);

    age_vle_traversal_clear_active_path(state,
                                        "age_vle_traversal_state_reset");
    state->frame_stack->size = 0;
    state->worklist->head = 0;
    state->worklist->size = 0;
    state->worklist->next_work_ordinal = 0;
    age_vle_arena_segments_reset(&state->arena_segments);
    if (state->path_stack != NULL)
        state->path_stack->size = 0;
    state->path_edge_index_stack->size = 0;
    state->path_vertex_stack->size = 0;
    state->path_depth = 0;
    state->active_frame_index = -1;
    gid_stack_push(state->path_vertex_stack, start_vertex_id);
}

VLETraversalFrameStack *age_vle_frame_stack_new(void)
{
    VLETraversalFrameStack *stack = palloc(sizeof(*stack));

    stack->array = palloc(sizeof(VLETraversalFrame) *
                          VLE_FRAME_STACK_INITIAL_CAPACITY);
    stack->size = 0;
    stack->capacity = VLE_FRAME_STACK_INITIAL_CAPACITY;

    return stack;
}

void age_vle_frame_stack_free(VLETraversalFrameStack *stack)
{
    if (stack == NULL)
    {
        return;
    }

    pfree_if_not_null(stack->array);
    pfree(stack);
}

static void age_vle_frame_stack_ensure_capacity(
    VLETraversalFrameStack *stack, int64 required)
{
    Assert(stack != NULL);

    if (required <= stack->capacity)
    {
        return;
    }

    while (stack->capacity < required)
    {
        stack->capacity *= 2;
    }
    stack->array = repalloc(stack->array,
                            sizeof(VLETraversalFrame) * stack->capacity);
}

int64 age_vle_frame_stack_push(VLETraversalFrameStack *stack,
                               graphid edge_id, int64 edge_index,
                               int64 path_length,
                               int64 parent_frame_index,
                               int64 arena_segment_index,
                               graphid next_vertex_id,
                               vertex_entry *next_vertex_entry)
{
    VLETraversalFrame *frame = NULL;
    int64 frame_index;

    Assert(stack != NULL);
    Assert(path_length > 0);
    Assert(parent_frame_index < stack->size);

    age_vle_frame_stack_ensure_capacity(stack, stack->size + 1);

    frame_index = stack->size;
    frame = &stack->array[stack->size++];
    frame->edge_id = edge_id;
    frame->edge_index = edge_index;
    frame->path_length = path_length;
    frame->parent_frame_index = parent_frame_index;
    frame->arena_segment_index = arena_segment_index;
    frame->next_vertex_id = next_vertex_id;
    frame->next_vertex_entry = next_vertex_entry;

    return frame_index;
}

static void age_vle_arena_segments_init(
    VLETraversalArenaSegmentList *segments)
{
    Assert(segments != NULL);

    segments->array = palloc(sizeof(VLETraversalArenaSegment) *
                             VLE_ARENA_SEGMENT_INITIAL_CAPACITY);
    segments->size = 0;
    segments->capacity = VLE_ARENA_SEGMENT_INITIAL_CAPACITY;
    segments->work_closed_count = 0;
    segments->unprobed_closed_segment_count = 0;
    segments->unprobed_closed_frame_count = 0;
    segments->compactable_candidate_count = 0;
    segments->compactable_frame_count = 0;
    segments->compacted_segment_count = 0;
    segments->compacted_frame_count = 0;
}

static void age_vle_arena_segments_reset(
    VLETraversalArenaSegmentList *segments)
{
    Assert(segments != NULL);

    segments->size = 0;
    segments->work_closed_count = 0;
    segments->unprobed_closed_segment_count = 0;
    segments->unprobed_closed_frame_count = 0;
    segments->compactable_candidate_count = 0;
    segments->compactable_frame_count = 0;
    segments->compacted_segment_count = 0;
    segments->compacted_frame_count = 0;
}

static void age_vle_arena_segments_free(
    VLETraversalArenaSegmentList *segments)
{
    Assert(segments != NULL);

    pfree_if_not_null(segments->array);
    segments->array = NULL;
    segments->size = 0;
    segments->capacity = 0;
    segments->work_closed_count = 0;
    segments->unprobed_closed_segment_count = 0;
    segments->unprobed_closed_frame_count = 0;
    segments->compactable_candidate_count = 0;
    segments->compactable_frame_count = 0;
    segments->compacted_segment_count = 0;
    segments->compacted_frame_count = 0;
}

static void age_vle_arena_segments_ensure_capacity(
    VLETraversalArenaSegmentList *segments, int64 required)
{
    Assert(segments != NULL);

    if (required <= segments->capacity)
        return;

    while (segments->capacity < required)
        segments->capacity *= 2;
    segments->array = repalloc(segments->array,
                               sizeof(VLETraversalArenaSegment) *
                               segments->capacity);
}

static int64 age_vle_arena_segment_begin(
    VLETraversalState *state, int64 frame_start, int64 work_start,
    int64 path_length, int64 parent_frame_index,
    VLETraversalIteratorPolicy iterator_policy)
{
    VLETraversalArenaSegmentList *segments;
    VLETraversalArenaSegment *segment;
    int64 arena_segment_index;

    Assert(state != NULL);
    Assert(frame_start >= 0);
    Assert(work_start >= 0);
    Assert(path_length > 0);
    Assert(parent_frame_index < frame_start);
    if (!age_vle_traversal_iterator_policy_is_valid(iterator_policy))
        elog(ERROR, "invalid VLE traversal iterator policy");

    segments = &state->arena_segments;
    age_vle_arena_segments_ensure_capacity(segments, segments->size + 1);
    arena_segment_index = segments->size;
    segment = &segments->array[segments->size++];
    segment->frame_start = frame_start;
    segment->frame_count = 0;
    segment->work_start = work_start;
    segment->work_count = 0;
    segment->path_length = path_length;
    segment->parent_frame_index = parent_frame_index;
    segment->remaining_work_count = 0;
    segment->consumed_work_count = 0;
    segment->iterator_policy = iterator_policy;
    segment->work_closed = false;
    segment->compactable_candidate = false;
    segment->frames_compacted = false;

    return arena_segment_index;
}

static void age_vle_arena_segment_record_frame_work(
    VLETraversalState *state, int64 arena_segment_index)
{
    VLETraversalArenaSegment *segment;

    Assert(state != NULL);
    Assert(arena_segment_index >= 0);
    Assert(arena_segment_index < state->arena_segments.size);

    segment = &state->arena_segments.array[arena_segment_index];
    Assert(!segment->work_closed);
    segment->frame_count++;
    segment->work_count++;
    segment->remaining_work_count++;
}

static void age_vle_arena_segment_record_work_consumed(
    VLETraversalState *state, int64 arena_segment_index)
{
    VLETraversalArenaSegment *segment;

    Assert(state != NULL);

    if (arena_segment_index < 0)
        return;

    Assert(arena_segment_index < state->arena_segments.size);
    segment = &state->arena_segments.array[arena_segment_index];
    Assert(segment->remaining_work_count > 0);
    Assert(segment->consumed_work_count < segment->work_count);

    segment->remaining_work_count--;
    segment->consumed_work_count++;
    if (segment->remaining_work_count == 0)
    {
        Assert(!segment->work_closed);
        segment->work_closed = true;
        state->arena_segments.work_closed_count++;
        state->arena_segments.unprobed_closed_segment_count++;
        state->arena_segments.unprobed_closed_frame_count +=
            segment->frame_count;
    }
}

/*
 * All frames produced by one arena segment share the segment's parent frame.
 * Marking by segment therefore visits each referenced ancestry segment at
 * most once per probe, even when many pending work items reconverge on it.
 */
static void age_vle_arena_segments_mark_referenced_chain(
    const VLETraversalState *state, bool *referenced, int64 frame_index)
{
    const VLETraversalArenaSegmentList *segments;

    Assert(state != NULL);
    Assert(state->frame_stack != NULL);
    Assert(referenced != NULL);

    segments = &state->arena_segments;
    while (frame_index >= 0)
    {
        const VLETraversalArenaSegment *segment;
        const VLETraversalFrame *frame;
        int64 arena_segment_index;

        Assert(frame_index < state->frame_stack->size);
        frame = &state->frame_stack->array[frame_index];
        arena_segment_index = frame->arena_segment_index;
        Assert(arena_segment_index >= 0);
        Assert(arena_segment_index < segments->size);
        if (referenced[arena_segment_index])
            return;

        referenced[arena_segment_index] = true;
        segment = &segments->array[arena_segment_index];
        frame_index = segment->parent_frame_index;
    }
}

static void age_vle_arena_segments_refresh_compactable_candidates(
    VLETraversalState *state)
{
    VLETraversalArenaSegmentList *segments;
    bool *referenced;
    bool force_probe;
    int64 i;

    Assert(state != NULL);
    Assert(state->worklist != NULL);

    segments = &state->arena_segments;
    if (segments->work_closed_count != segments->compactable_candidate_count)
    {
        /*
         * Probing each closed segment made reconvergent traversals quadratic:
         * every completion rescanned all pending work and each parent chain.
         * Batch probes while work remains, but force one before traversal
         * quiescence so every reclaimable segment is eventually discovered.
         */
        force_probe = state->worklist->size == 0;
        if (!force_probe &&
            segments->unprobed_closed_segment_count <
                VLE_ARENA_REFERENCE_PROBE_SEGMENT_INTERVAL &&
            segments->unprobed_closed_frame_count <
                VLE_ARENA_REFERENCE_PROBE_FRAME_INTERVAL)
            goto compact_ready_frames;

        referenced = palloc0(sizeof(bool) * segments->size);
        age_vle_arena_segments_mark_referenced_chain(
            state, referenced, state->active_frame_index);
        for (i = state->worklist->head;
             i < state->worklist->head + state->worklist->size; i++)
        {
            age_vle_arena_segments_mark_referenced_chain(
                state, referenced, state->worklist->items[i].frame_index);
        }

        for (i = 0; i < segments->size; i++)
        {
            VLETraversalArenaSegment *segment;

            segment = &segments->array[i];
            if (!segment->work_closed || segment->compactable_candidate)
                continue;

            if (referenced[i])
                continue;

            segment->compactable_candidate = true;
            segments->compactable_candidate_count++;
            segments->compactable_frame_count += segment->frame_count;
        }

        pfree(referenced);
        segments->unprobed_closed_segment_count = 0;
        segments->unprobed_closed_frame_count = 0;
    }

compact_ready_frames:
    if (segments->compactable_candidate_count !=
        segments->compacted_segment_count &&
        (segments->compactable_frame_count - segments->compacted_frame_count >=
         VLE_ARENA_COMPACTION_MIN_FRAME_COUNT ||
         state->worklist->size == 0))
    {
        age_vle_arena_segments_compact_windows(state);
    }
}

static int64 age_vle_arena_frame_index_after_compaction_windows(
    int64 frame_index, const VLEArenaCompactionWindow *windows,
    int64 window_count)
{
    int64 removed_frame_count = 0;
    int64 i;

    if (frame_index < 0)
        return -1;

    Assert(windows != NULL);
    Assert(window_count > 0);

    for (i = 0; i < window_count; i++)
    {
        int64 frame_start;
        int64 frame_end;

        frame_start = windows[i].frame_start;
        frame_end = frame_start + windows[i].frame_count;
        Assert(frame_start >= 0);
        Assert(windows[i].frame_count > 0);
        if (frame_index < frame_start)
            return frame_index - removed_frame_count;
        if (frame_index < frame_end)
            return -1;

        removed_frame_count += windows[i].frame_count;
    }

    return frame_index - removed_frame_count;
}

static void age_vle_arena_segments_compact_windows(VLETraversalState *state)
{
    VLETraversalArenaSegmentList *segments;
    VLEArenaCompactionWindow *windows;
    VLETraversalFrameStack *stack;
    int64 window_count;
    int64 total_frame_count = 0;

    Assert(state != NULL);
    Assert(state->frame_stack != NULL);

    segments = &state->arena_segments;
    stack = state->frame_stack;
    if (stack->size == 0)
        return;

    windows = palloc(sizeof(VLEArenaCompactionWindow) * segments->size);
    window_count = age_vle_arena_segments_collect_compaction_windows(
        state, windows, segments->size, &total_frame_count);
    if (window_count == 0)
    {
        pfree(windows);
        return;
    }

    age_vle_arena_segments_rewrite_frame_indexes_for_windows(
        state, windows, window_count);
    age_vle_arena_segments_move_frame_windows(stack, windows, window_count,
                                              total_frame_count);
    pfree(windows);
}

static int64 age_vle_arena_segments_collect_compaction_windows(
    VLETraversalState *state, VLEArenaCompactionWindow *windows,
    int64 max_window_count, int64 *total_frame_count)
{
    VLETraversalArenaSegmentList *segments;
    int64 window_index = 0;
    int64 i;

    Assert(state != NULL);
    Assert(windows != NULL);
    Assert(max_window_count >= 0);
    Assert(total_frame_count != NULL);

    *total_frame_count = 0;

    segments = &state->arena_segments;
    for (i = 0; i < segments->size; i++)
    {
        VLETraversalArenaSegment *segment;
        int64 window_start;
        int64 window_frame_count;
        int64 window_end;
        int64 j;

        segment = &segments->array[i];
        if (segment->frames_compacted || segment->frame_count == 0 ||
            !segment->compactable_candidate)
            continue;

        window_start = segment->frame_start;
        window_frame_count = segment->frame_count;
        window_end = window_start + window_frame_count;
        Assert(window_start >= 0);
        for (j = i + 1; j < segments->size; j++)
        {
            VLETraversalArenaSegment *next_segment;

            next_segment = &segments->array[j];
            if (next_segment->frames_compacted ||
                next_segment->frame_count == 0)
                continue;
            if (!next_segment->compactable_candidate ||
                next_segment->frame_start != window_end)
                break;

            window_frame_count += next_segment->frame_count;
            window_end += next_segment->frame_count;
        }

        Assert(window_index < max_window_count);
        windows[window_index].frame_start = window_start;
        windows[window_index].frame_count = window_frame_count;
        *total_frame_count += window_frame_count;
        window_index++;
        i = j - 1;
    }

    return window_index;
}

static void age_vle_arena_segments_move_frame_windows(
    VLETraversalFrameStack *stack, const VLEArenaCompactionWindow *windows,
    int64 window_count, int64 total_frame_count)
{
    int64 dst_index;
    int64 src_index;
    int64 i;

    Assert(stack != NULL);
    Assert(windows != NULL);
    Assert(window_count > 0);
    Assert(total_frame_count > 0);

    dst_index = windows[0].frame_start;
    src_index = dst_index;
    for (i = 0; i < window_count; i++)
    {
        int64 live_count;

        Assert(windows[i].frame_start >= src_index);
        live_count = windows[i].frame_start - src_index;
        if (live_count > 0 && dst_index != src_index)
        {
            memmove(&stack->array[dst_index], &stack->array[src_index],
                    sizeof(VLETraversalFrame) * live_count);
        }
        dst_index += live_count;
        src_index = windows[i].frame_start + windows[i].frame_count;
    }

    Assert(src_index <= stack->size);
    if (src_index < stack->size)
    {
        int64 live_count;

        live_count = stack->size - src_index;
        if (dst_index != src_index)
        {
            memmove(&stack->array[dst_index], &stack->array[src_index],
                    sizeof(VLETraversalFrame) * live_count);
        }
    }

    stack->size -= total_frame_count;
}

static void age_vle_arena_segments_rewrite_frame_indexes_for_windows(
    VLETraversalState *state, const VLEArenaCompactionWindow *windows,
    int64 window_count)
{
    VLETraversalArenaSegmentList *segments;
    VLETraversalFrameStack *stack;
    int64 rewritten_frame_index;
    int64 i;

    Assert(state != NULL);
    Assert(state->frame_stack != NULL);
    Assert(state->worklist != NULL);
    Assert(windows != NULL);
    Assert(window_count > 0);

    segments = &state->arena_segments;
    stack = state->frame_stack;

    rewritten_frame_index =
        age_vle_arena_frame_index_after_compaction_windows(
            state->active_frame_index, windows, window_count);
    Assert(state->active_frame_index < 0 || rewritten_frame_index >= 0);
    state->active_frame_index = rewritten_frame_index;

    for (i = state->worklist->head;
         i < state->worklist->head + state->worklist->size; i++)
    {
        VLETraversalWorkItem *item;

        item = &state->worklist->items[i];
        rewritten_frame_index =
            age_vle_arena_frame_index_after_compaction_windows(
                item->frame_index, windows, window_count);
        Assert(rewritten_frame_index >= 0);
        item->frame_index = rewritten_frame_index;
    }

    for (i = 0; i < stack->size; i++)
    {
        VLETraversalFrame *frame;

        if (age_vle_arena_frame_index_after_compaction_windows(
                i, windows, window_count) < 0)
            continue;

        frame = &stack->array[i];
        frame->parent_frame_index =
            age_vle_arena_frame_index_after_compaction_windows(
                frame->parent_frame_index, windows, window_count);
    }

    for (i = 0; i < segments->size; i++)
    {
        VLETraversalArenaSegment *segment;

        segment = &segments->array[i];
        if (segment->frames_compacted)
            continue;

        segment->parent_frame_index =
            age_vle_arena_frame_index_after_compaction_windows(
                segment->parent_frame_index, windows, window_count);
        rewritten_frame_index =
            age_vle_arena_frame_index_after_compaction_windows(
                segment->frame_start, windows, window_count);
        if (rewritten_frame_index < 0)
        {
            Assert(segment->compactable_candidate);
            Assert(age_vle_arena_frame_index_after_compaction_windows(
                       segment->frame_start + segment->frame_count - 1,
                       windows, window_count) < 0);
            segments->compacted_segment_count++;
            segments->compacted_frame_count += segment->frame_count;
            segment->frame_start = -1;
            segment->frame_count = 0;
            segment->frames_compacted = true;
            continue;
        }

        segment->frame_start = rewritten_frame_index;
        Assert(segment->frame_start >= 0);
    }
}

VLETraversalWorklist *age_vle_worklist_new(void)
{
    VLETraversalWorklist *worklist = palloc(sizeof(*worklist));

    worklist->items = palloc(sizeof(VLETraversalWorkItem) *
                             VLE_WORKLIST_INITIAL_CAPACITY);
    worklist->head = 0;
    worklist->size = 0;
    worklist->capacity = VLE_WORKLIST_INITIAL_CAPACITY;
    worklist->next_work_ordinal = 0;
    worklist->iterator_policy = VLE_TRAVERSAL_ITERATOR_LIFO;

    return worklist;
}

void age_vle_worklist_free(VLETraversalWorklist *worklist)
{
    if (worklist == NULL)
        return;

    pfree_if_not_null(worklist->items);
    pfree(worklist);
}

static void age_vle_worklist_ensure_capacity(
    VLETraversalWorklist *worklist, int64 required)
{
    int64 new_capacity;

    Assert(worklist != NULL);
    Assert(required >= worklist->size);

    if (worklist->head + required <= worklist->capacity)
        return;

    /*
     * LEVEL_BATCH removes from the front.  A nearly full queue can dequeue
     * one item and enqueue one child repeatedly; compacting the live window
     * for every such append would recreate quadratic copying.  Give dense
     * queues geometric tail room, and compact in-place only when at least
     * half of the allocation is already dead prefix.
     */
    new_capacity = worklist->capacity;
    if (worklist->head > 0 && required > worklist->capacity / 2)
        new_capacity *= 2;
    while (new_capacity < required)
        new_capacity *= 2;

    if (new_capacity != worklist->capacity)
    {
        worklist->items = repalloc(worklist->items,
                                   sizeof(VLETraversalWorkItem) *
                                   new_capacity);
        worklist->capacity = new_capacity;
    }

    if (worklist->head > 0)
    {
        if (worklist->size > 0)
        {
            memmove(worklist->items,
                    &worklist->items[worklist->head],
                    sizeof(VLETraversalWorkItem) * worklist->size);
        }
        worklist->head = 0;
    }

    Assert(required <= worklist->capacity);
}

static void age_vle_worklist_push(VLETraversalWorklist *worklist,
                                  int64 frame_index, int64 path_length,
                                  int64 arena_segment_index,
                                  VLETraversalWorkItemKind kind)
{
    VLETraversalWorkItem *item;

    Assert(worklist != NULL);
    Assert(frame_index >= 0);
    Assert(path_length > 0);

    age_vle_worklist_ensure_capacity(worklist, worklist->size + 1);
    item = &worklist->items[worklist->head + worklist->size++];
    item->frame_index = frame_index;
    item->path_length = path_length;
    item->work_ordinal = worklist->next_work_ordinal++;
    item->arena_segment_index = arena_segment_index;
    item->kind = kind;
}

static bool age_vle_worklist_select_next(
    VLETraversalState *state, VLETraversalWorkSelection *selection)
{
    Assert(state != NULL);
    Assert(state->worklist != NULL);
    Assert(selection != NULL);

    switch (state->worklist->iterator_policy)
    {
        case VLE_TRAVERSAL_ITERATOR_LIFO:
            return age_vle_worklist_select_lifo(state->worklist, selection);

        case VLE_TRAVERSAL_ITERATOR_LEVEL_BATCH:
            return age_vle_worklist_select_level_batch(state, selection);
    }

    elog(ERROR, "unsupported VLE traversal iterator policy: %d",
         (int) state->worklist->iterator_policy);
    return false;
}

static bool age_vle_worklist_select_lifo(
    VLETraversalWorklist *worklist, VLETraversalWorkSelection *selection)
{
    Assert(worklist != NULL);
    Assert(selection != NULL);

    if (worklist->size == 0)
        return false;

    selection->work_index = worklist->head + worklist->size - 1;
    selection->item = worklist->items[selection->work_index];
    return true;
}

static bool age_vle_worklist_select_level_batch(
    VLETraversalState *state, VLETraversalWorkSelection *selection)
{
    VLETraversalWorklist *worklist;
    VLETraversalWorkItem *item;
    int64 selected_index = -1;
    int64 selected_path_length = 0;
    int64 i;

    Assert(state != NULL);
    Assert(state->frame_stack != NULL);
    Assert(state->worklist != NULL);
    Assert(selection != NULL);

    worklist = state->worklist;

    if (worklist->size == 0)
        return false;

    /*
     * A level-batch traversal drains all visits at depth N before it appends
     * depth N+1 work.  Therefore insertion order is already breadth-first
     * order, and the oldest live visit is the minimum-depth visit.  Pop that
     * queue head directly instead of rescanning the whole frontier.
     */
    item = &worklist->items[worklist->head];
    if (item->kind == VLE_TRAVERSAL_WORK_VISIT)
    {
        Assert(item->frame_index >= 0);
        Assert(item->frame_index < state->frame_stack->size);
        selection->work_index = worklist->head;
        selection->item = *item;
        return true;
    }

    /*
     * LEVEL_BATCH does not normally enqueue backtracks.  Keep the historical
     * mixed-item fallback for defensive compatibility rather than changing
     * its semantics if a future caller does so.
     */
    for (i = worklist->head;
         i < worklist->head + worklist->size; i++)
    {
        item = &worklist->items[i];
        if (item->kind != VLE_TRAVERSAL_WORK_VISIT)
            continue;
        Assert(item->frame_index >= 0);
        Assert(item->frame_index < state->frame_stack->size);
        if (selected_index >= 0 &&
            item->path_length >= selected_path_length)
            continue;

        selected_index = i;
        selected_path_length = item->path_length;
    }

    if (selected_index >= 0)
    {
        selection->work_index = selected_index;
        selection->item = worklist->items[selected_index];
        return true;
    }

    return age_vle_worklist_select_lifo(worklist, selection);
}

static void age_vle_worklist_remove_at(VLETraversalWorklist *worklist,
                                       int64 work_index)
{
    int64 work_end;

    Assert(worklist != NULL);
    Assert(work_index >= worklist->head);
    Assert(work_index < worklist->head + worklist->size);

    if (work_index == worklist->head)
    {
        worklist->head++;
        worklist->size--;
        if (worklist->size == 0)
            worklist->head = 0;
        return;
    }

    work_end = worklist->head + worklist->size;
    if (work_index + 1 < work_end)
    {
        memmove(&worklist->items[work_index],
                &worklist->items[work_index + 1],
                sizeof(VLETraversalWorkItem) *
                (work_end - work_index - 1));
    }
    worklist->size--;
}

static bool age_vle_worklist_is_empty(VLETraversalWorklist *worklist)
{
    Assert(worklist != NULL);

    return worklist->size == 0;
}

static bool age_vle_traversal_uses_path_replay_iterator(
    const VLETraversalState *state)
{
    Assert(state != NULL);
    Assert(state->worklist != NULL);

    return state->worklist->iterator_policy ==
           VLE_TRAVERSAL_ITERATOR_LEVEL_BATCH;
}

static void age_vle_path_replay_ensure_capacity(VLETraversalState *state,
                                                int64 required)
{
    int64 new_capacity;

    Assert(state != NULL);
    Assert(required >= 0);

    if (required <= state->path_replay_frame_capacity)
        return;

    new_capacity = state->path_replay_frame_capacity > 0 ?
        state->path_replay_frame_capacity : VLE_FRAME_STACK_INITIAL_CAPACITY;
    while (new_capacity < required)
        new_capacity *= 2;

    if (state->path_replay_frame_indexes == NULL)
        state->path_replay_frame_indexes = palloc(sizeof(int64) *
                                                  new_capacity);
    else
        state->path_replay_frame_indexes = repalloc(
            state->path_replay_frame_indexes, sizeof(int64) * new_capacity);
    state->path_replay_frame_capacity = new_capacity;
}

static void age_vle_traversal_clear_active_path(VLETraversalState *state,
                                                const char *caller)
{
    int64 i;

    Assert(state != NULL);

    if (state->path_edge_index_stack != NULL)
    {
        for (i = 0; i < state->path_edge_index_stack->size; i++)
        {
            int64 edge_index;
            uint8 *edge_flags;

            edge_index = state->path_edge_index_stack->array[i];
            edge_flags = age_vle_edge_state_flag_at(&state->edge_state,
                                                    edge_index, caller);
            *edge_flags &= ~VLE_EDGE_STATE_USED;
        }
        state->path_edge_index_stack->size = 0;
    }

    if (state->path_stack != NULL)
        state->path_stack->size = 0;
    if (state->path_vertex_stack != NULL &&
        state->path_vertex_stack->size > 1)
    {
        state->path_vertex_stack->size = 1;
    }
    state->path_depth = 0;
    state->active_frame_index = -1;
}

static void age_vle_traversal_switch_active_path(
    VLETraversalState *state, int64 target_frame_index, const char *caller)
{
    int64 frame_index;
    int64 frame_count = 0;
    int64 i;

    Assert(state != NULL);
    Assert(state->frame_stack != NULL);
    Assert(state->path_edge_index_stack != NULL);
    Assert(state->path_vertex_stack != NULL);
    Assert(target_frame_index < state->frame_stack->size);

    if (state->active_frame_index == target_frame_index)
        return;

    age_vle_traversal_clear_active_path(state, caller);
    if (target_frame_index < 0)
        return;

    frame_index = target_frame_index;
    while (frame_index >= 0)
    {
        VLETraversalFrame *frame;

        Assert(frame_index < state->frame_stack->size);
        age_vle_path_replay_ensure_capacity(state, frame_count + 1);
        state->path_replay_frame_indexes[frame_count++] = frame_index;

        frame = &state->frame_stack->array[frame_index];
        frame_index = frame->parent_frame_index;
    }

    for (i = frame_count - 1; i >= 0; i--)
    {
        VLETraversalFrame *frame;
        uint8 *edge_flags;

        frame_index = state->path_replay_frame_indexes[i];
        Assert(frame_index >= 0);
        Assert(frame_index < state->frame_stack->size);
        frame = &state->frame_stack->array[frame_index];
        Assert(frame->path_length == state->path_depth + 1);
        Assert(frame->parent_frame_index == state->active_frame_index);

        edge_flags = age_vle_edge_state_flag_at(&state->edge_state,
                                                frame->edge_index, caller);
        Assert((*edge_flags & VLE_EDGE_STATE_USED) == 0);
        *edge_flags |= VLE_EDGE_STATE_USED;
        age_vle_path_stacks_push(state->path_stack,
                                 state->path_edge_index_stack,
                                 state->path_vertex_stack, frame->edge_id,
                                 frame->edge_index, frame->next_vertex_id);
        state->path_depth = frame->path_length;
        state->active_frame_index = frame_index;
    }

    Assert(state->active_frame_index == target_frame_index);
    Assert(state->path_depth == frame_count);
}

static bool age_vle_traversal_iterator_policy_is_valid(
    VLETraversalIteratorPolicy iterator_policy)
{
    switch (iterator_policy)
    {
        case VLE_TRAVERSAL_ITERATOR_LIFO:
        case VLE_TRAVERSAL_ITERATOR_LEVEL_BATCH:
            return true;
    }

    return false;
}

void age_vle_path_stacks_push(GraphIdStack *path_stack,
                              GraphIdStack *path_edge_index_stack,
                              GraphIdStack *path_vertex_stack,
                              graphid edge_id, graphid edge_index,
                              graphid vertex_id)
{
    Assert(path_edge_index_stack != NULL);
    Assert(path_vertex_stack != NULL);

    if (path_stack != NULL && path_stack->size >= path_stack->capacity)
    {
        path_stack->capacity *= 2;
        path_stack->array = repalloc(path_stack->array,
                                     sizeof(graphid) *
                                     path_stack->capacity);
        path_edge_index_stack->capacity = path_stack->capacity;
        path_edge_index_stack->array = repalloc(
            path_edge_index_stack->array,
            sizeof(graphid) * path_edge_index_stack->capacity);
    }
    else if (path_stack == NULL &&
             path_edge_index_stack->size >= path_edge_index_stack->capacity)
    {
        path_edge_index_stack->capacity *= 2;
        path_edge_index_stack->array = repalloc(
            path_edge_index_stack->array,
            sizeof(graphid) * path_edge_index_stack->capacity);
    }
    if (path_vertex_stack->size >= path_vertex_stack->capacity)
    {
        path_vertex_stack->capacity *= 2;
        path_vertex_stack->array = repalloc(path_vertex_stack->array,
                                           sizeof(graphid) *
                                           path_vertex_stack->capacity);
    }

    if (path_stack != NULL)
        path_stack->array[path_stack->size++] = edge_id;
    path_edge_index_stack->array[path_edge_index_stack->size++] = edge_index;
    path_vertex_stack->array[path_vertex_stack->size++] = vertex_id;
}

void age_vle_path_stacks_pop(GraphIdStack *path_stack,
                             GraphIdStack *path_edge_index_stack,
                             GraphIdStack *path_vertex_stack)
{
    Assert(path_edge_index_stack != NULL);
    Assert(path_edge_index_stack->size > 0);
    Assert(path_vertex_stack != NULL);
    Assert(path_vertex_stack->size > 0);

    if (path_stack != NULL)
    {
        Assert(path_stack->size > 0);
        path_stack->size--;
    }
    path_edge_index_stack->size--;
    path_vertex_stack->size--;
}

bool age_vle_path_top_edge_index_equals(GraphIdStack *path_edge_index_stack,
                                        int64 edge_index)
{
    Assert(path_edge_index_stack != NULL);
    Assert(path_edge_index_stack->size > 0);

    return path_edge_index_stack->array[path_edge_index_stack->size - 1] ==
           edge_index;
}

void age_vle_edge_state_init(VLELocalEdgeState *state, bool use_local,
                             int64 estimated_edge_count)
{
    HASHCTL hash_ctl;
    int64 initial_capacity;

    Assert(state != NULL);

    memset(state, 0, sizeof(*state));
    state->use_local = use_local;
    if (use_local)
    {
        initial_capacity = estimated_edge_count > 0 ?
            estimated_edge_count : 1024;
        initial_capacity = Max(initial_capacity, 16);
        initial_capacity = Min(
            initial_capacity, VLE_LOCAL_EDGE_STATE_INITIAL_CAPACITY_MAX);

        memset(&hash_ctl, 0, sizeof(hash_ctl));
        hash_ctl.keysize = sizeof(graphid);
        hash_ctl.entrysize = sizeof(VLELocalEdgeStateEntry);
        hash_ctl.hash = tag_hash;
        state->local_edge_index =
            hash_create("VLE local edge index", initial_capacity, &hash_ctl,
                        HASH_ELEM | HASH_FUNCTION);
        state->capacity = initial_capacity;
        state->flags = palloc0(sizeof(uint8) * state->capacity);
        return;
    }

    state->size = estimated_edge_count;
    state->capacity = estimated_edge_count;
    if (state->size > 0)
    {
        state->flags = palloc0(sizeof(uint8) * state->size);
    }
}

void age_vle_edge_state_free(VLELocalEdgeState *state)
{
    Assert(state != NULL);

    pfree_if_not_null(state->flags);
    state->flags = NULL;
    state->size = 0;
    state->capacity = 0;
    if (state->local_edge_index != NULL)
    {
        hash_destroy(state->local_edge_index);
        state->local_edge_index = NULL;
    }
    state->use_local = false;
}

static void age_vle_edge_state_ensure_capacity(VLELocalEdgeState *state,
                                               int64 required)
{
    int64 new_capacity;

    Assert(state != NULL);

    if (required <= state->capacity)
    {
        return;
    }

    new_capacity = state->capacity > 0 ? state->capacity : 1024;
    while (new_capacity < required)
    {
        new_capacity *= 2;
    }

    if (state->flags == NULL)
    {
        state->flags = palloc0(sizeof(uint8) * new_capacity);
    }
    else
    {
        state->flags = repalloc0(state->flags,
                                 sizeof(uint8) * state->capacity,
                                 sizeof(uint8) * new_capacity);
    }
    state->capacity = new_capacity;
}

static int64 age_vle_edge_state_get_or_create_index(
    VLELocalEdgeState *state, graphid edge_id)
{
    VLELocalEdgeStateEntry *entry;
    bool found;

    Assert(state != NULL);
    Assert(state->use_local);
    Assert(state->local_edge_index != NULL);

    entry = hash_search(state->local_edge_index, &edge_id, HASH_ENTER,
                        &found);
    if (!found)
    {
        entry->edge_index = state->size++;
        age_vle_edge_state_ensure_capacity(state, state->size);
    }

    return entry->edge_index;
}

int64 age_vle_traversal_get_or_create_local_edge_index(
    VLETraversalState *state, graphid edge_id)
{
    Assert(state != NULL);

    return age_vle_edge_state_get_or_create_index(&state->edge_state, edge_id);
}

uint8 *age_vle_edge_state_flag_at(VLELocalEdgeState *state,
                                  int64 edge_index, const char *caller)
{
    Assert(state != NULL);

    if (edge_index < 0 || edge_index >= state->size || state->flags == NULL)
    {
        elog(ERROR, "%s: invalid edge index", caller);
    }

    return &state->flags[edge_index];
}

bool age_vle_traversal_candidate_needs_match_check(
    VLETraversalState *state, int64 edge_index, const char *caller)
{
    uint8 *edge_flags;

    Assert(state != NULL);

    edge_flags = age_vle_edge_state_flag_at(&state->edge_state, edge_index,
                                            caller);
    if ((*edge_flags & VLE_EDGE_STATE_USED) != 0)
        return false;

    return (*edge_flags & VLE_EDGE_STATE_MATCH_CHECKED) == 0;
}

void age_vle_traversal_candidate_mark_match(
    VLETraversalState *state, int64 edge_index, bool matched,
    const char *caller)
{
    uint8 *edge_flags;

    Assert(state != NULL);

    edge_flags = age_vle_edge_state_flag_at(&state->edge_state, edge_index,
                                            caller);
    if ((*edge_flags & VLE_EDGE_STATE_USED) != 0)
        return;

    if (matched)
        *edge_flags |= VLE_EDGE_STATE_MATCHED;
    else
        *edge_flags &= ~VLE_EDGE_STATE_MATCHED;
    *edge_flags |= VLE_EDGE_STATE_MATCH_CHECKED;
}

static void age_vle_frontier_batch_init(
    const VLETraversalState *state, VLETraversalFrontierBatch *frontier_batch,
    const VLETraversalCandidate *candidates, const bool *accepted,
    int candidate_count, int64 path_length, int64 frame_start,
    int64 work_start, VLETraversalIteratorPolicy iterator_policy,
    int64 parent_frame_index)
{
    if (frontier_batch == NULL)
        return;

    frontier_batch->candidates = candidates;
    frontier_batch->accepted = accepted;
    frontier_batch->candidate_count = candidate_count;
    frontier_batch->path_length = path_length;
    frontier_batch->frame_start = frame_start;
    frontier_batch->frame_count = 0;
    frontier_batch->work_start = work_start;
    frontier_batch->work_count = 0;
    frontier_batch->arena_segment_index = -1;
    frontier_batch->iterator_policy = iterator_policy;
    frontier_batch->parent_frame_index = parent_frame_index;
    age_vle_traversal_compaction_evidence(
        state, &frontier_batch->compaction_evidence);
    frontier_batch->single_accepted = false;
}

void age_vle_traversal_compaction_evidence(
    const VLETraversalState *state, VLETraversalCompactionEvidence *evidence)
{
    const VLETraversalArenaSegmentList *segments;

    Assert(evidence != NULL);

    memset(evidence, 0, sizeof(*evidence));
    if (state == NULL)
        return;

    segments = &state->arena_segments;
    evidence->arena_segment_count = segments->size;
    evidence->work_closed_segment_count = segments->work_closed_count;
    evidence->compactable_candidate_count =
        segments->compactable_candidate_count;
    evidence->compactable_frame_count = segments->compactable_frame_count;
    evidence->compacted_segment_count = segments->compacted_segment_count;
    evidence->compacted_frame_count = segments->compacted_frame_count;
}

bool age_vle_traversal_push_candidate_if_matched(
    VLETraversalState *state, const VLETraversalCandidate *candidate,
    const char *caller, VLETraversalFrontierBatch *frontier_batch)
{
    uint8 *edge_flags;
    int64 frame_start;
    int64 work_start;
    int64 parent_frame_index;
    int64 frame_index;
    int64 arena_segment_index;

    Assert(state != NULL);
    Assert(state->frame_stack != NULL);
    Assert(state->worklist != NULL);
    Assert(candidate != NULL);
    Assert(candidate->path_length > 0);

    frame_start = state->frame_stack->size;
    work_start = state->worklist->next_work_ordinal;
    parent_frame_index = state->active_frame_index;
    Assert(parent_frame_index < frame_start);
    age_vle_frontier_batch_init(state, frontier_batch, candidate,
                                frontier_batch != NULL ?
                                &frontier_batch->single_accepted : NULL,
                                1, candidate->path_length, frame_start,
                                work_start, state->worklist->iterator_policy,
                                parent_frame_index);
    edge_flags = age_vle_edge_state_flag_at(&state->edge_state,
                                            candidate->edge_index, caller);
    if ((*edge_flags & VLE_EDGE_STATE_USED) != 0 ||
        (*edge_flags & VLE_EDGE_STATE_MATCHED) == 0)
    {
        return false;
    }

    arena_segment_index = age_vle_arena_segment_begin(
        state, frame_start, work_start, candidate->path_length,
        parent_frame_index, state->worklist->iterator_policy);
    frame_index = age_vle_frame_stack_push(
        state->frame_stack, candidate->edge_id, candidate->edge_index,
        candidate->path_length, parent_frame_index, arena_segment_index,
        candidate->next_vertex_id, candidate->next_vertex_entry);
    age_vle_worklist_push(state->worklist, frame_index,
                          candidate->path_length, arena_segment_index,
                          VLE_TRAVERSAL_WORK_VISIT);
    age_vle_arena_segment_record_frame_work(state, arena_segment_index);
    if (frontier_batch != NULL)
    {
        frontier_batch->arena_segment_index = arena_segment_index;
        age_vle_traversal_compaction_evidence(
            state, &frontier_batch->compaction_evidence);
        frontier_batch->single_accepted = true;
        frontier_batch->frame_count = 1;
        frontier_batch->work_count = 1;
    }
    return true;
}

int64 age_vle_traversal_push_candidate_batch_if_matched(
    VLETraversalState *state, const VLETraversalCandidate *candidates,
    int candidate_count, int32 target_label_id, const char *caller,
    bool *pushed, VLETraversalFrontierBatch *frontier_batch)
{
    VLETraversalFrameStack *stack;
    VLETraversalWorklist *worklist;
    bool target_label_valid;
    int64 frame_start;
    int64 work_start;
    int64 parent_frame_index;
    int64 path_length;
    int64 arena_segment_index = -1;
    int64 pushed_count = 0;
    int i;

    Assert(state != NULL);
    Assert(state->frame_stack != NULL);
    Assert(state->worklist != NULL);
    Assert(candidates != NULL);
    Assert(candidate_count >= 0);
    Assert(pushed != NULL);

    stack = state->frame_stack;
    worklist = state->worklist;
    frame_start = stack->size;
    work_start = worklist->next_work_ordinal;
    parent_frame_index = state->active_frame_index;
    Assert(parent_frame_index < frame_start);
    path_length = candidate_count > 0 ? candidates[0].path_length : 0;
    age_vle_frontier_batch_init(state, frontier_batch, candidates, pushed,
                                candidate_count, path_length, frame_start,
                                work_start, worklist->iterator_policy,
                                parent_frame_index);
    target_label_valid = label_id_is_valid(target_label_id);
    age_vle_frame_stack_ensure_capacity(stack, stack->size + candidate_count);
    age_vle_worklist_ensure_capacity(worklist,
                                     worklist->size + candidate_count);

    for (i = 0; i < candidate_count; i++)
    {
        const VLETraversalCandidate *candidate;
        VLETraversalFrame *frame;
        uint8 *edge_flags;
        int64 frame_index;

        candidate = &candidates[i];
        pushed[i] = false;
        Assert(candidate->path_length > 0);
        Assert(candidate->path_length == candidates[0].path_length);
        if (target_label_valid &&
            get_graphid_label_id(candidate->next_vertex_id) !=
            target_label_id)
        {
            continue;
        }

        edge_flags = age_vle_edge_state_flag_at(&state->edge_state,
                                                candidate->edge_index,
                                                caller);
        if ((*edge_flags & VLE_EDGE_STATE_USED) != 0 ||
            (*edge_flags & VLE_EDGE_STATE_MATCHED) == 0)
        {
            continue;
        }

        if (arena_segment_index < 0)
        {
            arena_segment_index = age_vle_arena_segment_begin(
                state, frame_start, work_start, candidate->path_length,
                parent_frame_index, worklist->iterator_policy);
            if (frontier_batch != NULL)
                frontier_batch->arena_segment_index = arena_segment_index;
        }

        frame_index = stack->size;
        frame = &stack->array[stack->size++];
        frame->edge_id = candidate->edge_id;
        frame->edge_index = candidate->edge_index;
        frame->path_length = candidate->path_length;
        frame->parent_frame_index = parent_frame_index;
        frame->arena_segment_index = arena_segment_index;
        frame->next_vertex_id = candidate->next_vertex_id;
        frame->next_vertex_entry = candidate->next_vertex_entry;
        age_vle_worklist_push(worklist, frame_index, candidate->path_length,
                              arena_segment_index,
                              VLE_TRAVERSAL_WORK_VISIT);
        age_vle_arena_segment_record_frame_work(state, arena_segment_index);
        pushed[i] = true;
        pushed_count++;
        if (frontier_batch != NULL)
        {
            frontier_batch->frame_count++;
            frontier_batch->work_count++;
        }
    }

    if (frontier_batch != NULL && pushed_count > 0)
        age_vle_traversal_compaction_evidence(
            state, &frontier_batch->compaction_evidence);

    return pushed_count;
}

bool age_vle_consume_next_frame(VLETraversalState *state, const char *caller,
                                VLETraversalStep *step)
{
    Assert(state != NULL);
    Assert(state->frame_stack != NULL);
    Assert(state->worklist != NULL);
    Assert(state->path_edge_index_stack != NULL);
    Assert(state->path_vertex_stack != NULL);
    Assert(step != NULL);

    while (!age_vle_worklist_is_empty(state->worklist))
    {
        VLETraversalFrame *frame;
        VLETraversalWorkSelection selection;
        VLETraversalWorkItemKind item_kind;
        bool replay_iterator;
        uint8 *edge_flags;
        int64 frame_index;

        replay_iterator =
            age_vle_traversal_uses_path_replay_iterator(state);
        if (!age_vle_worklist_select_next(state, &selection))
            return false;

        frame_index = selection.item.frame_index;
        item_kind = selection.item.kind;
        Assert(frame_index >= 0);
        Assert(frame_index < state->frame_stack->size);
        frame = &state->frame_stack->array[frame_index];
        Assert(selection.item.path_length == frame->path_length);
        edge_flags = age_vle_edge_state_flag_at(&state->edge_state,
                                                frame->edge_index, caller);

        if (item_kind == VLE_TRAVERSAL_WORK_BACKTRACK)
        {
            if (replay_iterator)
            {
                age_vle_worklist_remove_at(state->worklist,
                                           selection.work_index);
                age_vle_arena_segments_refresh_compactable_candidates(state);
                continue;
            }
            Assert((*edge_flags & VLE_EDGE_STATE_USED) != 0);
            Assert(age_vle_path_top_edge_index_equals(
                       state->path_edge_index_stack, frame->edge_index));
            Assert(state->active_frame_index == frame_index);
            age_vle_path_stacks_pop(state->path_stack,
                                    state->path_edge_index_stack,
                                    state->path_vertex_stack);
            Assert(state->path_depth > 0);
            state->path_depth--;
            state->active_frame_index = frame->parent_frame_index;
            *edge_flags &= ~VLE_EDGE_STATE_USED;
            age_vle_worklist_remove_at(state->worklist,
                                       selection.work_index);
            age_vle_arena_segments_refresh_compactable_candidates(state);
            continue;
        }
        Assert(item_kind == VLE_TRAVERSAL_WORK_VISIT);
        if (replay_iterator)
        {
            age_vle_traversal_switch_active_path(
                state, frame->parent_frame_index, caller);
            edge_flags = age_vle_edge_state_flag_at(&state->edge_state,
                                                    frame->edge_index,
                                                    caller);
        }
        age_vle_worklist_remove_at(state->worklist, selection.work_index);
        age_vle_arena_segment_record_work_consumed(
            state, selection.item.arena_segment_index);

        if ((*edge_flags & VLE_EDGE_STATE_USED) != 0)
        {
            age_vle_arena_segments_refresh_compactable_candidates(state);
            continue;
        }

        *edge_flags |= VLE_EDGE_STATE_USED;
        Assert(frame->path_length == state->path_depth + 1);
        Assert(frame->parent_frame_index == state->active_frame_index);
        if (!replay_iterator)
        {
            age_vle_worklist_push(state->worklist, frame_index,
                                  frame->path_length, -1,
                                  VLE_TRAVERSAL_WORK_BACKTRACK);
        }
        step->path_length = frame->path_length;
        step->vertex_id = frame->next_vertex_id;
        step->vertex_entry = frame->next_vertex_entry;
        age_vle_path_stacks_push(state->path_stack,
                                 state->path_edge_index_stack,
                                 state->path_vertex_stack, frame->edge_id,
                                 frame->edge_index, step->vertex_id);
        state->path_depth = step->path_length;
        state->active_frame_index = frame_index;
        age_vle_arena_segments_refresh_compactable_candidates(state);

        return true;
    }

    if (age_vle_traversal_uses_path_replay_iterator(state))
        age_vle_traversal_clear_active_path(state, caller);
    age_vle_arena_segments_refresh_compactable_candidates(state);

    return false;
}
