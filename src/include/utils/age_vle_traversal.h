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

#ifndef AGE_VLE_TRAVERSAL_H
#define AGE_VLE_TRAVERSAL_H

#include "postgres.h"

#include "utils/age_global_graph.h"
#include "utils/hsearch.h"

typedef struct VLELocalEdgeState
{
    bool use_local;
    HTAB *local_edge_index;
    uint8 *flags;
    int64 size;
    int64 capacity;
} VLELocalEdgeState;

typedef struct VLETraversalFrame
{
    graphid edge_id;
    int64 edge_index;
    graphid next_vertex_id;
    vertex_entry *next_vertex_entry;
} VLETraversalFrame;

typedef struct VLETraversalFrameStack
{
    VLETraversalFrame *array;
    int64 size;
    int64 capacity;
} VLETraversalFrameStack;

typedef struct VLETraversalState
{
    VLETraversalFrameStack *frame_stack;
    GraphIdStack *path_stack;
    GraphIdStack *path_edge_index_stack;
    GraphIdStack *path_vertex_stack;
    VLELocalEdgeState edge_state;
    int64 path_depth;
} VLETraversalState;

typedef struct VLETraversalAcceptance
{
    int64 lower_bound;
    int64 upper_bound;
    bool upper_unbounded;
    bool require_terminal;
    graphid terminal_id;
    bool require_terminal_label;
    int32 terminal_label_id;
} VLETraversalAcceptance;

typedef struct VLETraversalCandidate
{
    graphid edge_id;
    int64 edge_index;
    graphid next_vertex_id;
    vertex_entry *next_vertex_entry;
} VLETraversalCandidate;

typedef struct VLETraversalStep
{
    graphid vertex_id;
    vertex_entry *vertex_entry;
    int64 path_length;
} VLETraversalStep;

#define VLE_EDGE_STATE_USED 0x01
#define VLE_EDGE_STATE_MATCH_CHECKED 0x02
#define VLE_EDGE_STATE_MATCHED 0x04

extern void age_vle_acceptance_init(VLETraversalAcceptance *acceptance,
                                    int64 lower_bound,
                                    int64 upper_bound,
                                    bool upper_unbounded);
extern void age_vle_acceptance_require_terminal(
    VLETraversalAcceptance *acceptance, graphid terminal_id);
extern void age_vle_acceptance_require_terminal_label(
    VLETraversalAcceptance *acceptance, int32 terminal_label_id);
extern bool age_vle_accepts_path(const VLETraversalAcceptance *acceptance,
                                 graphid terminal_id, int64 path_length);
extern bool age_vle_terminal_over_upper_bound(
    const VLETraversalAcceptance *acceptance, graphid terminal_id,
    int64 path_length);
extern bool age_vle_accepts_step(
    const VLETraversalAcceptance *acceptance,
    const VLETraversalStep *step);
extern bool age_vle_step_over_upper_bound(
    const VLETraversalAcceptance *acceptance,
    const VLETraversalStep *step);
extern void age_vle_traversal_state_init(VLETraversalState *state);
extern void age_vle_traversal_state_free(VLETraversalState *state);
extern void age_vle_traversal_state_reset(VLETraversalState *state,
                                          graphid start_vertex_id);
extern VLETraversalFrameStack *age_vle_frame_stack_new(void);
extern void age_vle_frame_stack_free(VLETraversalFrameStack *stack);
extern void age_vle_frame_stack_push(VLETraversalFrameStack *stack,
                                     graphid edge_id, int64 edge_index,
                                     graphid next_vertex_id,
                                     vertex_entry *next_vertex_entry);
extern VLETraversalFrame *age_vle_frame_stack_peek(
    VLETraversalFrameStack *stack);
extern void age_vle_frame_stack_pop(VLETraversalFrameStack *stack);
extern bool age_vle_frame_stack_is_empty(VLETraversalFrameStack *stack);
extern void age_vle_path_stacks_push(GraphIdStack *path_stack,
                                     GraphIdStack *path_edge_index_stack,
                                     GraphIdStack *path_vertex_stack,
                                     graphid edge_id, graphid edge_index,
                                     graphid vertex_id);
extern void age_vle_path_stacks_pop(GraphIdStack *path_stack,
                                    GraphIdStack *path_edge_index_stack,
                                    GraphIdStack *path_vertex_stack);
extern bool age_vle_path_top_edge_index_equals(
    GraphIdStack *path_edge_index_stack, int64 edge_index);
extern void age_vle_edge_state_init(VLELocalEdgeState *state,
                                    bool use_local,
                                    int64 estimated_edge_count);
extern void age_vle_edge_state_free(VLELocalEdgeState *state);
extern int64 age_vle_traversal_get_or_create_local_edge_index(
    VLETraversalState *state, graphid edge_id);
extern uint8 *age_vle_edge_state_flag_at(VLELocalEdgeState *state,
                                         int64 edge_index,
                                         const char *caller);
extern bool age_vle_traversal_candidate_needs_match_check(
    VLETraversalState *state, int64 edge_index, const char *caller);
extern void age_vle_traversal_candidate_mark_match(
    VLETraversalState *state, int64 edge_index, bool matched,
    const char *caller);
extern bool age_vle_traversal_push_candidate_if_matched(
    VLETraversalState *state, const VLETraversalCandidate *candidate,
    const char *caller);
extern bool age_vle_consume_next_frame(VLETraversalState *state,
                                       const char *caller,
                                       VLETraversalStep *step);

#endif
