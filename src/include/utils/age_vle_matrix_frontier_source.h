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

#ifndef AGE_VLE_MATRIX_FRONTIER_SOURCE_H
#define AGE_VLE_MATRIX_FRONTIER_SOURCE_H

#include "postgres.h"

#include "access/age_adjacency.h"
#include "utils/age_vle_context.h"

typedef enum VLEMatrixFrontierRunInputKind
{
    VLE_MATRIX_FRONTIER_RUN_INPUT_REPLAY,
    VLE_MATRIX_FRONTIER_RUN_INPUT_RAW,
    VLE_MATRIX_FRONTIER_RUN_INPUT_CURSOR
} VLEMatrixFrontierRunInputKind;

typedef struct VLEMatrixFrontierRunInputState
{
    VLEMatrixFrontierCacheEntry *matrix_replay_entry;
    AgeAdjacencyVisiblePayloadRunScan *raw_run_scan;
    int64 matrix_payload_index;
} VLEMatrixFrontierRunInputState;

typedef struct VLEMatrixFrontierRunInput
{
    VLEMatrixFrontierRunInputKind kind;
    VLEContextAgeAdjacencyPayloadSource *source;
    const VLEContextSourceCursor *cursor;
    VLEMatrixFrontierRunInputState *state;
    AgeAdjacencyPayload payload;
    bool payload_valid;
    bool state_owner;
} VLEMatrixFrontierRunInput;

typedef struct VLEMatrixFrontierSourceBlock
{
    VLE_local_context *vlelctx;
    const VLEContextSourceCursor *cursors;
    const VLEContextSourceCursor **cursor_order;
    int64 cursor_count;
    int64 cursor_index;
    int64 run_start_index;
    int64 run_end_index;
    const VLEContextSourceCursor *active_cursor;
    VLEContextAgeAdjacencyPayloadSource *active_source;
    VLEContextAgeAdjacencyPayloadSource **run_sources;
    VLEMatrixFrontierRunInput *run_inputs;
    AgeAdjacencyVisiblePayloadRunKey *raw_keys;
    int64 run_source_count;
    int64 run_source_capacity;
    int64 run_input_count;
    int64 run_input_capacity;
    int64 raw_source_count;
    VLEMatrixFrontierCacheKey matrix_key;
    bool matrix_key_valid;
    bool matrix_key_counted;
    bool active_empty_suppressed;
    bool run_batch_active;
} VLEMatrixFrontierSourceBlock;

extern bool age_vle_matrix_frontier_source_block_begin(
    VLEMatrixFrontierSourceBlock *block, VLE_local_context *vlelctx,
    const VLEContextSourceCursor *cursors, int64 cursor_count);
extern bool age_vle_matrix_frontier_source_block_empty_suppressed(
    const VLEMatrixFrontierSourceBlock *block);
extern bool age_vle_matrix_frontier_source_block_next(
    VLEMatrixFrontierSourceBlock *block, AgeAdjacencyPayload *payload,
    VLEContextAgeAdjacencyPayloadSource **payload_source,
    const VLEContextSourceCursor **source_cursor);
extern void age_vle_matrix_frontier_source_block_end(
    VLEMatrixFrontierSourceBlock *block);

#endif
