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
    int64 path_length;
    int64 parent_frame_index;
    int64 arena_segment_index;
    graphid next_vertex_id;
    vertex_entry *next_vertex_entry;
} VLETraversalFrame;

typedef struct VLETraversalFrameStack
{
    VLETraversalFrame *array;
    int64 size;
    int64 capacity;
} VLETraversalFrameStack;

typedef enum VLETraversalIteratorPolicy
{
    VLE_TRAVERSAL_ITERATOR_LIFO = 0,
    VLE_TRAVERSAL_ITERATOR_LEVEL_BATCH
} VLETraversalIteratorPolicy;

typedef enum VLETraversalWorkItemKind
{
    VLE_TRAVERSAL_WORK_VISIT = 0,
    VLE_TRAVERSAL_WORK_BACKTRACK
} VLETraversalWorkItemKind;

typedef struct VLETraversalWorkItem
{
    int64 frame_index;
    int64 path_length;
    int64 work_ordinal;
    int64 arena_segment_index;
    VLETraversalWorkItemKind kind;
} VLETraversalWorkItem;

typedef struct VLETraversalWorklist
{
    VLETraversalWorkItem *items;
    int64 size;
    int64 capacity;
    int64 next_work_ordinal;
    VLETraversalIteratorPolicy iterator_policy;
} VLETraversalWorklist;

typedef struct VLETraversalArenaSegment
{
    int64 frame_start;
    int64 frame_count;
    int64 work_start;
    int64 work_count;
    int64 path_length;
    int64 parent_frame_index;
    int64 remaining_work_count;
    int64 consumed_work_count;
    VLETraversalIteratorPolicy iterator_policy;
    bool work_closed;
    bool compactable_candidate;
    bool frames_compacted;
} VLETraversalArenaSegment;

typedef struct VLETraversalArenaSegmentList
{
    VLETraversalArenaSegment *array;
    int64 size;
    int64 capacity;
    int64 work_closed_count;
    int64 compactable_candidate_count;
    int64 compactable_frame_count;
    int64 compacted_segment_count;
    int64 compacted_frame_count;
} VLETraversalArenaSegmentList;

typedef struct VLETraversalCompactionEvidence
{
    int64 arena_segment_count;
    int64 work_closed_segment_count;
    int64 compactable_candidate_count;
    int64 compactable_frame_count;
    int64 compacted_segment_count;
    int64 compacted_frame_count;
} VLETraversalCompactionEvidence;

typedef struct VLETraversalFrontierPressureEvidence
{
    int64 handoff_count;
    int64 candidate_count;
    int64 accepted_count;
    int64 unique_source_count;
    int64 max_unique_source_count;
    int64 frame_count;
    int64 work_count;
    int64 work_closed_segment_delta;
    int64 compactable_frame_delta;
    int64 compacted_frame_delta;
} VLETraversalFrontierPressureEvidence;

typedef struct VLETraversalState
{
    VLETraversalFrameStack *frame_stack;
    VLETraversalWorklist *worklist;
    VLETraversalArenaSegmentList arena_segments;
    GraphIdStack *path_stack;
    GraphIdStack *path_edge_index_stack;
    GraphIdStack *path_vertex_stack;
    VLELocalEdgeState edge_state;
    int64 *path_replay_frame_indexes;
    int64 path_replay_frame_capacity;
    bool store_path_edges;
    int64 path_depth;
    int64 active_frame_index;
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
    int64 path_length;
    graphid next_vertex_id;
    vertex_entry *next_vertex_entry;
} VLETraversalCandidate;

typedef struct VLETraversalFrontierBatch
{
    const VLETraversalCandidate *candidates;
    const bool *accepted;
    int candidate_count;
    int64 path_length;
    int64 frame_start;
    int64 frame_count;
    int64 work_start;
    int64 work_count;
    int64 arena_segment_index;
    VLETraversalIteratorPolicy iterator_policy;
    int64 parent_frame_index;
    VLETraversalCompactionEvidence compaction_evidence;
    bool single_accepted;
} VLETraversalFrontierBatch;

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
extern void age_vle_traversal_state_init(VLETraversalState *state,
                                         bool store_path_edges);
extern void age_vle_traversal_state_set_iterator_policy(
    VLETraversalState *state, VLETraversalIteratorPolicy iterator_policy);
extern void age_vle_traversal_state_free(VLETraversalState *state);
extern void age_vle_traversal_state_reset(VLETraversalState *state,
                                          graphid start_vertex_id);
extern VLETraversalFrameStack *age_vle_frame_stack_new(void);
extern void age_vle_frame_stack_free(VLETraversalFrameStack *stack);
extern int64 age_vle_frame_stack_push(VLETraversalFrameStack *stack,
                                      graphid edge_id, int64 edge_index,
                                      int64 path_length,
                                      int64 parent_frame_index,
                                      int64 arena_segment_index,
                                      graphid next_vertex_id,
                                      vertex_entry *next_vertex_entry);
extern VLETraversalWorklist *age_vle_worklist_new(void);
extern void age_vle_worklist_free(VLETraversalWorklist *worklist);
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
extern void age_vle_traversal_compaction_evidence(
    const VLETraversalState *state, VLETraversalCompactionEvidence *evidence);
extern bool age_vle_traversal_push_candidate_if_matched(
    VLETraversalState *state, const VLETraversalCandidate *candidate,
    const char *caller, VLETraversalFrontierBatch *frontier_batch);
extern int64 age_vle_traversal_push_candidate_batch_if_matched(
    VLETraversalState *state, const VLETraversalCandidate *candidates,
    int candidate_count, int32 target_label_id, const char *caller,
    bool *pushed, VLETraversalFrontierBatch *frontier_batch);
extern bool age_vle_consume_next_frame(VLETraversalState *state,
                                       const char *caller,
                                       VLETraversalStep *step);

#endif
