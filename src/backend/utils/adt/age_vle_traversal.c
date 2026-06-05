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

#define VLE_FRAME_STACK_INITIAL_CAPACITY 64

typedef struct VLELocalEdgeStateEntry
{
    graphid edge_id;
    int64 edge_index;
} VLELocalEdgeStateEntry;

#define VLE_LOCAL_EDGE_STATE_INITIAL_CAPACITY_MAX 1048576

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

void age_vle_traversal_state_init(VLETraversalState *state)
{
    Assert(state != NULL);

    state->frame_stack = age_vle_frame_stack_new();
    state->path_stack = new_gid_stack();
    state->path_edge_index_stack = new_gid_stack();
    state->path_vertex_stack = new_gid_stack();
    state->path_depth = 0;
}

void age_vle_traversal_state_free(VLETraversalState *state)
{
    Assert(state != NULL);

    age_vle_edge_state_free(&state->edge_state);
    if (state->frame_stack != NULL)
    {
        age_vle_frame_stack_free(state->frame_stack);
    }
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

    memset(state, 0, sizeof(*state));
}

void age_vle_traversal_state_reset(VLETraversalState *state,
                                   graphid start_vertex_id)
{
    Assert(state != NULL);
    Assert(state->frame_stack != NULL);
    Assert(state->path_stack != NULL);
    Assert(state->path_edge_index_stack != NULL);
    Assert(state->path_vertex_stack != NULL);

    state->frame_stack->size = 0;
    state->path_stack->size = 0;
    state->path_edge_index_stack->size = 0;
    state->path_vertex_stack->size = 0;
    state->path_depth = 0;
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

void age_vle_frame_stack_push(VLETraversalFrameStack *stack,
                              graphid edge_id, int64 edge_index,
                              graphid next_vertex_id,
                              vertex_entry *next_vertex_entry)
{
    VLETraversalFrame *frame = NULL;

    Assert(stack != NULL);

    if (stack->size >= stack->capacity)
    {
        stack->capacity *= 2;
        stack->array = repalloc(stack->array,
                                sizeof(VLETraversalFrame) *
                                stack->capacity);
    }

    frame = &stack->array[stack->size++];
    frame->edge_id = edge_id;
    frame->edge_index = edge_index;
    frame->next_vertex_id = next_vertex_id;
    frame->next_vertex_entry = next_vertex_entry;
}

VLETraversalFrame *age_vle_frame_stack_peek(VLETraversalFrameStack *stack)
{
    Assert(stack != NULL);
    Assert(stack->size > 0);

    return &stack->array[stack->size - 1];
}

void age_vle_frame_stack_pop(VLETraversalFrameStack *stack)
{
    Assert(stack != NULL);
    Assert(stack->size > 0);

    stack->size--;
}

bool age_vle_frame_stack_is_empty(VLETraversalFrameStack *stack)
{
    return stack->size == 0;
}

void age_vle_path_stacks_push(GraphIdStack *path_stack,
                              GraphIdStack *path_edge_index_stack,
                              GraphIdStack *path_vertex_stack,
                              graphid edge_id, graphid edge_index,
                              graphid vertex_id)
{
    Assert(path_stack != NULL);
    Assert(path_edge_index_stack != NULL);
    Assert(path_vertex_stack != NULL);

    if (path_stack->size >= path_stack->capacity)
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
    if (path_vertex_stack->size >= path_vertex_stack->capacity)
    {
        path_vertex_stack->capacity *= 2;
        path_vertex_stack->array = repalloc(path_vertex_stack->array,
                                           sizeof(graphid) *
                                           path_vertex_stack->capacity);
    }

    path_stack->array[path_stack->size++] = edge_id;
    path_edge_index_stack->array[path_edge_index_stack->size++] = edge_index;
    path_vertex_stack->array[path_vertex_stack->size++] = vertex_id;
}

void age_vle_path_stacks_pop(GraphIdStack *path_stack,
                             GraphIdStack *path_edge_index_stack,
                             GraphIdStack *path_vertex_stack)
{
    Assert(path_stack != NULL);
    Assert(path_stack->size > 0);
    Assert(path_edge_index_stack != NULL);
    Assert(path_edge_index_stack->size > 0);
    Assert(path_vertex_stack != NULL);
    Assert(path_vertex_stack->size > 0);

    path_stack->size--;
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

bool age_vle_traversal_push_candidate_if_matched(
    VLETraversalState *state, const VLETraversalCandidate *candidate,
    const char *caller)
{
    uint8 *edge_flags;

    Assert(state != NULL);
    Assert(state->frame_stack != NULL);
    Assert(candidate != NULL);

    edge_flags = age_vle_edge_state_flag_at(&state->edge_state,
                                            candidate->edge_index, caller);
    if ((*edge_flags & VLE_EDGE_STATE_USED) != 0 ||
        (*edge_flags & VLE_EDGE_STATE_MATCHED) == 0)
    {
        return false;
    }

    age_vle_frame_stack_push(state->frame_stack, candidate->edge_id,
                             candidate->edge_index,
                             candidate->next_vertex_id,
                             candidate->next_vertex_entry);
    return true;
}

bool age_vle_consume_next_frame(VLETraversalState *state, const char *caller,
                                VLETraversalStep *step)
{
    Assert(state != NULL);
    Assert(state->frame_stack != NULL);
    Assert(state->path_stack != NULL);
    Assert(state->path_edge_index_stack != NULL);
    Assert(state->path_vertex_stack != NULL);
    Assert(step != NULL);

    while (!age_vle_frame_stack_is_empty(state->frame_stack))
    {
        VLETraversalFrame *frame;
        uint8 *edge_flags;

        frame = age_vle_frame_stack_peek(state->frame_stack);
        edge_flags = age_vle_edge_state_flag_at(&state->edge_state,
                                                frame->edge_index, caller);

        /*
         * A used top frame is either the path tail being backed out, or an
         * interior edge that would form a repeated-edge path.
         */
        if ((*edge_flags & VLE_EDGE_STATE_USED) != 0)
        {
            if (age_vle_path_top_edge_index_equals(
                    state->path_edge_index_stack, frame->edge_index))
            {
                age_vle_path_stacks_pop(state->path_stack,
                                        state->path_edge_index_stack,
                                        state->path_vertex_stack);
                Assert(state->path_depth > 0);
                state->path_depth--;
                *edge_flags &= ~VLE_EDGE_STATE_USED;
            }
            age_vle_frame_stack_pop(state->frame_stack);
            continue;
        }

        *edge_flags |= VLE_EDGE_STATE_USED;
        step->path_length = state->path_depth + 1;
        step->vertex_id = frame->next_vertex_id;
        step->vertex_entry = frame->next_vertex_entry;
        age_vle_path_stacks_push(state->path_stack,
                                 state->path_edge_index_stack,
                                 state->path_vertex_stack, frame->edge_id,
                                 frame->edge_index, step->vertex_id);
        state->path_depth = step->path_length;

        return true;
    }

    return false;
}
