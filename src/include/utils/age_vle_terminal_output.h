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

#ifndef AGE_VLE_TERMINAL_OUTPUT_H
#define AGE_VLE_TERMINAL_OUTPUT_H

#include "postgres.h"

#include "funcapi.h"
#include "utils/age_vle_context.h"
#include "utils/age_vle_iterator_materialization.h"

typedef bool (*VLETerminalOutputMaterializePath) (void *state);

extern bool age_vle_terminal_output_uses_direct_dfs(
    const VLETerminalOutputPolicy *policy);
extern bool age_vle_terminal_output_path_matches_predicate(
    VLE_local_context *vlelctx, const VLETraversalStep *step);
extern void age_vle_terminal_output_cache_result(
    VLE_local_context *vlelctx, const VLETerminalOutputPolicy *policy,
    const VLETraversalStep *step);
extern bool age_vle_terminal_output_emit_property(
    VLE_local_context *vlelctx, const VLETerminalOutputPolicy *policy,
    FuncCallContext *funcctx, bool is_zero_bound,
    const VLEIteratorOutputTarget *target);
extern bool age_vle_terminal_output_emit_full_properties(
    VLE_local_context *vlelctx, FuncCallContext *funcctx,
    const VLEIteratorOutputTarget *target);
extern bool age_vle_terminal_output_should_materialize_batch(
    VLE_local_context *vlelctx);
extern void age_vle_terminal_output_materialize_batch(
    VLE_local_context *vlelctx, VLETerminalOutputMaterializePath find_path,
    void *find_path_state);

#endif
