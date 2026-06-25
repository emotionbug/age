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

#ifndef AGE_VLE_MATRIX_FRONTIER_PREFETCH_H
#define AGE_VLE_MATRIX_FRONTIER_PREFETCH_H

#include "postgres.h"

#include "utils/age_vle_context.h"
#include "utils/age_vle_traversal.h"

typedef struct VLEMatrixFrontierPrefetchCollector
{
    graphid *source_vertex_ids;
    int64 source_count;
    int64 source_capacity;
    int64 source_path_length;
    bool enabled;
} VLEMatrixFrontierPrefetchCollector;

typedef struct VLEAcceptedCandidateSpan
{
    const VLETraversalCandidate *candidates;
    const bool *accepted;
    int candidate_count;
} VLEAcceptedCandidateSpan;

extern void age_vle_matrix_frontier_prefetch_collector_init(
    VLEMatrixFrontierPrefetchCollector *collector,
    VLE_local_context *vlelctx, const VLEContextSourceCursor *source_cursors,
    int64 source_cursor_count);
extern void age_vle_matrix_frontier_prefetch_collector_add(
    VLEMatrixFrontierPrefetchCollector *collector,
    VLE_local_context *vlelctx,
    const VLETraversalCandidate *candidate);
extern void age_vle_matrix_frontier_prefetch_collector_add_span(
    VLEMatrixFrontierPrefetchCollector *collector,
    VLE_local_context *vlelctx,
    const VLEAcceptedCandidateSpan *span);
extern void age_vle_matrix_frontier_prefetch_collector_flush(
    VLEMatrixFrontierPrefetchCollector *collector,
    VLE_local_context *vlelctx);

#endif
