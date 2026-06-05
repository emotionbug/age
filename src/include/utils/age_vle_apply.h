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

#ifndef AGE_VLE_APPLY_H
#define AGE_VLE_APPLY_H

#include "postgres.h"

#include "funcapi.h"
#include "utils/age_vle_context.h"

typedef struct VLETraversalApplyOps
{
    void (*refresh_source_indexes)(VLE_local_context *vlelctx);
    void (*load_initial_stacks)(VLE_local_context *vlelctx);
} VLETraversalApplyOps;

typedef struct VLETraversalContextCacheOps
{
    VLE_local_context *(*get_cached)(int64 vle_grammar_node_id);
    void (*cache)(VLE_local_context *vlelctx);
} VLETraversalContextCacheOps;

extern void init_vle_traversal_apply_input(
    VLETraversalApplyInput *apply, AgeVLEInput *input,
    const VLETraversalSetup *setup, GRAPH_global_context *ggctx,
    bool use_cache, int64 vle_grammar_node_id);
extern VLE_local_context *build_vle_local_context_for_input(
    AgeVLEInput *input, FuncCallContext *funcctx,
    const VLETraversalApplyOps *apply_ops,
    const VLETraversalContextCacheOps *cache_ops);
extern void apply_vle_traversal_setup(
    VLE_local_context *vlelctx, const VLETraversalApplyInput *apply,
    const VLETraversalApplyOps *ops);
extern void init_vle_context_refresh_input(AgeVLEInput *input,
                                           VLEContextRefreshInput *refresh);
extern bool apply_cached_vle_context_refresh(
    VLE_local_context *vlelctx, const VLEContextRefreshInput *refresh,
    FuncCallContext *funcctx, const VLETraversalApplyOps *ops);
extern void apply_new_vle_context_activation(
    VLE_local_context *vlelctx, const VLETraversalApplyOps *ops);
extern void apply_vle_start_vertex_activation(
    VLE_local_context *vlelctx, const VLETraversalApplyOps *ops);
extern void refresh_vle_traversal_source_layout(VLE_local_context *vlelctx);

#endif
