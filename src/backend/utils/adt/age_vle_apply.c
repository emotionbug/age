/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#include "postgres.h"

#include "executor/cypher_vle_stream.h"
#include "utils/age_global_graph.h"
#include "utils/age_vle_apply.h"
#include "utils/age_vle_iterator_materialization.h"
#include "utils/age_vle_source_cost.h"
#include "utils/memutils.h"

#define VLE_LOCAL_EDGE_STATE_INITIAL_CAPACITY_MAX 1048576

static AgeVLEOutputRequirement resolve_vle_output_requirement(
    const AgeVLEInput *input, int64 vle_grammar_node_id);
static bool should_carry_frame_vertex_entry_for_output(
    bool use_local_edge_state, AgeVLEOutputRequirement output_requirement);
static void init_vle_traversal_context_apply(
    VLETraversalContextApply *context_apply,
    const VLETraversalApplyInput *apply);
static void init_vle_traversal_output_apply(
    VLETraversalOutputApply *output_apply,
    const VLETraversalApplyInput *apply,
    const VLETraversalContextApply *context_apply);
static void init_vle_traversal_edge_state_apply(
    VLETraversalEdgeStateApply *edge_state_apply,
    const VLETraversalContextApply *context_apply,
    const VLETraversalRootDescriptor *root);
static int64 estimate_vle_edge_state_capacity_for_setup(
    const VLETraversalContextApply *context_apply,
    const VLETraversalRootDescriptor *root);
static void init_vle_traversal_setup_apply(
    VLETraversalSetupApply *setup_apply,
    const VLETraversalApplyInput *apply);
static void apply_vle_traversal_context_base(
    VLE_local_context *vlelctx,
    const VLETraversalContextApply *context_apply,
    const VLETraversalApplyOps *ops);
static void apply_vle_traversal_setup_apply(
    VLE_local_context *vlelctx,
    const VLETraversalSetupApply *setup_apply,
    const VLETraversalApplyOps *ops);
static bool init_vle_traversal_refresh_apply(
    VLETraversalRefreshApply *refresh_apply, VLE_local_context *vlelctx,
    const VLEContextRefreshInput *refresh);
static void apply_vle_traversal_refresh(
    VLE_local_context *vlelctx,
    const VLETraversalRefreshApply *refresh_apply);
static void init_vle_traversal_activation_apply(
    VLETraversalActivationApply *activation_apply, VLE_local_context *vlelctx,
    bool refresh_source_indexes, bool init_traversal_state,
    bool mark_dirty);
static void apply_vle_traversal_activation(
    VLE_local_context *vlelctx,
    const VLETraversalActivationApply *activation_apply,
    const VLETraversalApplyOps *ops);
static bool init_vle_traversal_cached_reuse_apply(
    VLETraversalCachedReuseApply *reuse_apply, VLE_local_context *vlelctx,
    const VLEContextRefreshInput *refresh);
static void apply_vle_traversal_cached_reuse(
    VLE_local_context *vlelctx,
    const VLETraversalCachedReuseApply *reuse_apply,
    FuncCallContext *funcctx, const VLETraversalApplyOps *ops);
void init_vle_traversal_apply_input(
    VLETraversalApplyInput *apply, AgeVLEInput *input,
    const VLETraversalSetup *setup, GRAPH_global_context *ggctx,
    bool use_cache, int64 vle_grammar_node_id)
{
    Assert(apply != NULL);
    Assert(input != NULL);
    Assert(setup != NULL);
    Assert(ggctx != NULL);

    apply->input = input;
    apply->setup = setup;
    apply->ggctx = ggctx;
    apply->use_cache = use_cache;
    apply->vle_grammar_node_id = vle_grammar_node_id;
}

VLE_local_context *build_vle_local_context_for_input(
    AgeVLEInput *input, FuncCallContext *funcctx,
    const VLETraversalApplyOps *apply_ops,
    const VLETraversalContextCacheOps *cache_ops)
{
    MemoryContext oldctx = NULL;
    GRAPH_global_context *ggctx = NULL;
    VLE_local_context *vlelctx = NULL;
    VLETraversalApplyInput apply;
    VLETraversalSetup setup;
    VLEContextRefreshInput refresh;
    int64 vle_grammar_node_id = 0;
    bool use_cache = false;

    Assert(input != NULL);
    Assert(funcctx != NULL);
    Assert(apply_ops != NULL);
    Assert(cache_ops != NULL);
    Assert(cache_ops->get_cached != NULL);
    Assert(cache_ops->cache != NULL);

    if (input->nargs >= 8)
    {
        vle_grammar_node_id = age_vle_input_get_grammar_node(input);
        use_cache = true;
    }

    if (use_cache)
        vlelctx = cache_ops->get_cached(vle_grammar_node_id);

    if (use_cache && vlelctx != NULL)
    {
        age_vle_context_reset_source_stats(vlelctx);
        init_vle_context_refresh_input(input, &refresh);
        if (!apply_cached_vle_context_refresh(vlelctx, &refresh, funcctx,
                                              apply_ops))
            return NULL;

        return vlelctx;
    }

    if (use_cache)
        oldctx = MemoryContextSwitchTo(TopMemoryContext);
    else
        oldctx = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

    init_vle_traversal_setup(input, vle_grammar_node_id, &setup);
    ggctx = load_vle_graph_context_for_traversal(&setup.graph_load);

    vlelctx = palloc0(sizeof(VLE_local_context));
    init_vle_traversal_apply_input(&apply, input, &setup, ggctx, use_cache,
                                   vle_grammar_node_id);
    apply_vle_traversal_setup(vlelctx, &apply, apply_ops);
    apply_new_vle_context_activation(vlelctx, apply_ops);

    vlelctx->next = NULL;

    if (use_cache)
        cache_ops->cache(vlelctx);

    MemoryContextSwitchTo(oldctx);

    return vlelctx;
}

static AgeVLEOutputRequirement resolve_vle_output_requirement(
    const AgeVLEInput *input, int64 vle_grammar_node_id)
{
    Assert(input != NULL);

    if (input->output_requirement != AGE_VLE_OUTPUT_REQUIREMENT_UNKNOWN)
        return input->output_requirement;
    if (input->nargs == AGE_VLE_STREAM_ARG_TERMINAL_PROPERTY + 1 ||
        input->nargs == AGE_VLE_STREAM_ARG_TERMINAL_LABEL + 1)
        return AGE_VLE_OUTPUT_REQUIREMENT_TERMINAL_PROPERTY;
    if (vle_grammar_node_id < 0)
        return AGE_VLE_OUTPUT_REQUIREMENT_TERMINAL_VERTEX;

    return AGE_VLE_OUTPUT_REQUIREMENT_PATH;
}

static bool should_carry_frame_vertex_entry_for_output(
    bool use_local_edge_state, AgeVLEOutputRequirement output_requirement)
{
    if (!use_local_edge_state)
        return true;

    return age_vle_output_requirement_is_terminal_only(output_requirement);
}

static void init_vle_traversal_context_apply(
    VLETraversalContextApply *context_apply,
    const VLETraversalApplyInput *apply)
{
    const VLETraversalSetup *setup;
    const VLETraversalShape *shape;

    Assert(context_apply != NULL);
    Assert(apply != NULL);
    Assert(apply->setup != NULL);
    Assert(apply->ggctx != NULL);

    setup = apply->setup;
    shape = &setup->shape;

    context_apply->graph_name = setup->graph_load.graph_name;
    context_apply->graph_oid = setup->graph_load.graph_oid;
    context_apply->ggctx = apply->ggctx;
    context_apply->use_cache = apply->use_cache;
    context_apply->vle_grammar_node_id = apply->vle_grammar_node_id;
    context_apply->use_local_edge_state = apply->input->source_policy_known ||
        !graph_global_context_has_edge_metadata(apply->ggctx);
    context_apply->edge_property_constraint =
        setup->edge_property_constraint;
    context_apply->edge_property_constraint_count =
        setup->edge_property_constraint_count;
    context_apply->edge_label_oid =
        OidIsValid(setup->graph_load.edge_label_oid) ?
        setup->graph_load.edge_label_oid : InvalidOid;
    context_apply->terminal_label_id =
        apply->input->terminal_label_known &&
        apply->input->terminal_label_mode == AGE_VLE_TERMINAL_LABEL_ALL_DEPTH ?
        apply->input->terminal_label_id : INVALID_LABEL_ID;
    context_apply->terminal_endpoint_label_id =
        apply->input->terminal_label_known &&
        apply->input->terminal_label_mode ==
        AGE_VLE_TERMINAL_LABEL_ENDPOINT_ONLY ?
        apply->input->terminal_label_id : INVALID_LABEL_ID;
    context_apply->source_indexes =
        setup->graph_load.source_policy.indexes;
    context_apply->source_policy_known = apply->input->source_policy_known;
    context_apply->source_policy_outgoing_kind =
        apply->input->source_policy_outgoing_kind;
    context_apply->source_policy_incoming_kind =
        apply->input->source_policy_incoming_kind;
    context_apply->empty_lifecycle_policy_known =
        apply->input->empty_lifecycle_policy_known;
    context_apply->empty_lifecycle_eligible =
        apply->input->empty_lifecycle_eligible;
    context_apply->empty_lifecycle_depth =
        apply->input->empty_lifecycle_depth;
    context_apply->empty_lifecycle_batch_size =
        apply->input->empty_lifecycle_batch_size;
    context_apply->matrix_frontier_policy_known =
        apply->input->matrix_frontier_policy_known;
    context_apply->matrix_frontier_eligible =
        apply->input->matrix_frontier_eligible;
    context_apply->matrix_frontier_depth =
        apply->input->matrix_frontier_depth;
    context_apply->matrix_frontier_batch_size =
        apply->input->matrix_frontier_batch_size;
    context_apply->lower = shape->lower;
    context_apply->upper = shape->upper;
    context_apply->upper_infinite = shape->upper_infinite;
    context_apply->terminal_property_prefetch_budget =
        shape->upper_infinite ? -1 : shape->upper + 1;
    context_apply->terminal_property_prefilter_eligible =
        apply->input->terminal_property_prefilter_eligible &&
        apply->input->terminal_property_predicate_known &&
        apply->input->terminal_label_known &&
        apply->input->terminal_label_mode == AGE_VLE_TERMINAL_LABEL_ALL_DEPTH;
    context_apply->terminal_property_index_oid =
        apply->input->terminal_property_index_oid;
    context_apply->terminal_property_predicate_known =
        apply->input->terminal_property_predicate_known;
    context_apply->terminal_property_predicate_key_known =
        apply->input->terminal_property_predicate_key_known;
    context_apply->terminal_property_predicate_key_value =
        apply->input->terminal_property_predicate_key_value;
    context_apply->terminal_property_predicate_key_len =
        apply->input->terminal_property_predicate_key_len;
    context_apply->terminal_property_predicate_key_is_char =
        apply->input->terminal_property_predicate_key_is_char;
    context_apply->terminal_property_predicate_key_char =
        apply->input->terminal_property_predicate_key_char;
    context_apply->terminal_property_predicate_value =
        apply->input->terminal_property_predicate_value;
    context_apply->terminal_property_predicate_null =
        apply->input->terminal_property_predicate_null;
    context_apply->terminal_property_source_prefetch_threshold =
        apply->input->terminal_property_prefetch_threshold;
}

static void init_vle_traversal_output_apply(
    VLETraversalOutputApply *output_apply,
    const VLETraversalApplyInput *apply,
    const VLETraversalContextApply *context_apply)
{
    AgeVLEInput *input;
    AgeVLEOutputRequirement output_requirement;

    Assert(output_apply != NULL);
    Assert(apply != NULL);
    Assert(apply->input != NULL);
    Assert(context_apply != NULL);

    input = apply->input;
    output_requirement = resolve_vle_output_requirement(
        input, apply->vle_grammar_node_id);

    output_apply->output_requirement = output_requirement;
    output_apply->emit_terminal_property = false;
    output_apply->emit_terminal_only =
        age_vle_output_requirement_is_terminal_only(output_requirement);
    output_apply->carry_frame_vertex_entry =
        should_carry_frame_vertex_entry_for_output(
            context_apply->use_local_edge_state, output_requirement);
    output_apply->has_terminal_property_key = false;
    output_apply->terminal_property_key_is_char = false;
    output_apply->terminal_property_key_char = '\0';
    output_apply->terminal_label_known = input->terminal_label_known;
    output_apply->terminal_label_id = input->terminal_label_known ?
        input->terminal_label_id : INVALID_LABEL_ID;
    output_apply->terminal_label_mode = input->terminal_label_mode;

    if (output_requirement == AGE_VLE_OUTPUT_REQUIREMENT_TERMINAL_PROPERTY)
    {
        agtype_value key_value;

        if (age_vle_input_get_terminal_property_key(input, &key_value))
        {
            output_apply->has_terminal_property_key = true;
            output_apply->terminal_property_key = key_value;
            if (input->terminal_property_key_known)
            {
                output_apply->terminal_property_key_is_char =
                    input->terminal_property_key_is_char;
                output_apply->terminal_property_key_char =
                    input->terminal_property_key_char;
            }
            else if (key_value.val.string.len == 1)
            {
                output_apply->terminal_property_key_is_char = true;
                output_apply->terminal_property_key_char =
                    key_value.val.string.val[0];
            }
            output_apply->emit_terminal_property = true;
            output_apply->emit_terminal_only = true;
            output_apply->carry_frame_vertex_entry =
                should_carry_frame_vertex_entry_for_output(
                    context_apply->use_local_edge_state,
                    AGE_VLE_OUTPUT_REQUIREMENT_TERMINAL_PROPERTY);
        }
    }
}

static void init_vle_traversal_edge_state_apply(
    VLETraversalEdgeStateApply *edge_state_apply,
    const VLETraversalContextApply *context_apply,
    const VLETraversalRootDescriptor *root)
{
    bool empty_length_range;
    bool zero_length_only;

    Assert(edge_state_apply != NULL);
    Assert(context_apply != NULL);
    Assert(root != NULL);

    empty_length_range = !context_apply->upper_infinite &&
                         context_apply->lower > context_apply->upper;
    zero_length_only = context_apply->lower == 0 &&
                       !context_apply->upper_infinite &&
                       context_apply->upper == 0;

    edge_state_apply->initialize =
        ((!empty_length_range && !zero_length_only) ||
         context_apply->use_cache);
    edge_state_apply->use_local = context_apply->use_local_edge_state;
    edge_state_apply->capacity = edge_state_apply->initialize ?
        estimate_vle_edge_state_capacity_for_setup(context_apply, root) : 0;
}

static int64 estimate_vle_edge_state_capacity_for_setup(
    const VLETraversalContextApply *context_apply,
    const VLETraversalRootDescriptor *root)
{
    VLESourceFanoutEvidence fanout_evidence;
    double fanout;
    double estimate = 0.0;
    int64 depth;
    int64 step;

    Assert(context_apply != NULL);
    Assert(root != NULL);
    Assert(context_apply->ggctx != NULL);

    if (!context_apply->use_local_edge_state)
        return get_graph_num_loaded_edges(context_apply->ggctx);

    estimate_vle_source_fanout_evidence(&fanout_evidence,
                                        context_apply->edge_label_oid);
    if (fanout_evidence.reltuples <= 0)
        return 1024;

    fanout = select_vle_source_fanout_for_direction(
        &fanout_evidence, &context_apply->source_indexes, root->direction);
    if (fanout <= 0)
        return 1024;

    if (context_apply->upper_infinite)
    {
        return (int64) Min(
            fanout_evidence.reltuples,
            (double) VLE_LOCAL_EDGE_STATE_INITIAL_CAPACITY_MAX);
    }

    depth = Max(context_apply->upper, (int64) 1);
    estimate = 1.0;
    for (step = 0; step < depth; step++)
    {
        estimate *= fanout;
        if (estimate >= fanout_evidence.reltuples)
        {
            estimate = fanout_evidence.reltuples;
            break;
        }
    }

    estimate = Max(estimate, fanout);
    estimate = Min(estimate, fanout_evidence.reltuples);
    estimate = Min(estimate,
                   (double) VLE_LOCAL_EDGE_STATE_INITIAL_CAPACITY_MAX);

    return (int64) Max(estimate, 16.0);
}

static void init_vle_traversal_setup_apply(
    VLETraversalSetupApply *setup_apply,
    const VLETraversalApplyInput *apply)
{
    VLETraversalRootSelectionInput selection_input;
    VLETraversalSourceLayoutInput source_layout_input;

    Assert(setup_apply != NULL);
    Assert(apply != NULL);
    Assert(apply->setup != NULL);
    Assert(apply->ggctx != NULL);

    init_vle_traversal_context_apply(&setup_apply->context, apply);
    age_vle_context_init_root_selection_input_from_apply(
        &selection_input, &setup_apply->context);
    age_vle_context_init_source_layout_input_from_apply(
        &source_layout_input, &setup_apply->context);
    init_vle_root_descriptor_from_setup(
        &setup_apply->root, apply->setup, apply->ggctx,
        &selection_input, &source_layout_input);
    init_vle_traversal_output_apply(
        &setup_apply->output, apply, &setup_apply->context);
    init_vle_traversal_edge_state_apply(
        &setup_apply->edge_state, &setup_apply->context,
        &setup_apply->root);
}

static void apply_vle_traversal_context_base(
    VLE_local_context *vlelctx,
    const VLETraversalContextApply *context_apply,
    const VLETraversalApplyOps *ops)
{
    Assert(vlelctx != NULL);
    Assert(context_apply != NULL);
    Assert(ops != NULL);

    age_vle_context_apply_base(vlelctx, context_apply);
}

static void apply_vle_traversal_setup_apply(
    VLE_local_context *vlelctx,
    const VLETraversalSetupApply *setup_apply,
    const VLETraversalApplyOps *ops)
{
    Assert(vlelctx != NULL);
    Assert(setup_apply != NULL);

    apply_vle_traversal_context_base(vlelctx, &setup_apply->context, ops);
    age_vle_context_apply_root(vlelctx, &setup_apply->root);
    age_vle_context_apply_output(vlelctx, &setup_apply->output);
    age_vle_context_apply_edge_state(vlelctx, &setup_apply->edge_state);
}

void apply_vle_traversal_setup(
    VLE_local_context *vlelctx, const VLETraversalApplyInput *apply,
    const VLETraversalApplyOps *ops)
{
    VLETraversalSetupApply setup_apply;

    Assert(vlelctx != NULL);
    Assert(apply != NULL);
    Assert(apply->setup != NULL);
    Assert(apply->ggctx != NULL);

    init_vle_traversal_setup_apply(&setup_apply, apply);
    apply_vle_traversal_setup_apply(vlelctx, &setup_apply, ops);
}

static bool init_vle_traversal_refresh_apply(
    VLETraversalRefreshApply *refresh_apply, VLE_local_context *vlelctx,
    const VLEContextRefreshInput *refresh)
{
    VLETraversalRootApplyInput root_apply;
    VLETraversalRootDescriptor *root;

    Assert(refresh_apply != NULL);
    Assert(vlelctx != NULL);
    Assert(refresh != NULL);

    root = &refresh_apply->root;
    age_vle_context_init_root_apply_input(&root_apply, vlelctx);
    if (!init_vle_root_descriptor_from_refresh(root,
                                               &root_apply.current_root,
                                               refresh,
                                               &root_apply.selection,
                                               &root_apply.source_layout))
        return false;

    refresh_apply->terminal_property_prefetch_budget =
        age_vle_context_terminal_prefetch_budget(vlelctx);
    refresh_apply->mark_dirty = true;

    return true;
}

static void apply_vle_traversal_refresh(
    VLE_local_context *vlelctx,
    const VLETraversalRefreshApply *refresh_apply)
{
    Assert(vlelctx != NULL);
    Assert(refresh_apply != NULL);

    age_vle_context_apply_root(vlelctx, &refresh_apply->root);
    age_vle_context_reset_terminal_property_direct_result(
        vlelctx, refresh_apply->terminal_property_prefetch_budget);
    if (refresh_apply->mark_dirty)
        age_vle_context_mark_dirty(vlelctx);
}

static void init_vle_traversal_activation_apply(
    VLETraversalActivationApply *activation_apply, VLE_local_context *vlelctx,
    bool refresh_source_indexes, bool init_traversal_state,
    bool mark_dirty)
{
    Assert(activation_apply != NULL);
    Assert(vlelctx != NULL);

    activation_apply->refresh_source_indexes = refresh_source_indexes;
    activation_apply->init_traversal_state = init_traversal_state;
    activation_apply->load_initial_stacks =
        age_vle_context_should_load_initial_stacks(vlelctx);
    activation_apply->mark_dirty = mark_dirty;
}

static void apply_vle_traversal_activation(
    VLE_local_context *vlelctx,
    const VLETraversalActivationApply *activation_apply,
    const VLETraversalApplyOps *ops)
{
    Assert(vlelctx != NULL);
    Assert(activation_apply != NULL);
    Assert(ops != NULL);
    Assert(ops->refresh_source_indexes != NULL);
    Assert(ops->load_initial_stacks != NULL);

    if (activation_apply->refresh_source_indexes)
        ops->refresh_source_indexes(vlelctx);

    if (activation_apply->load_initial_stacks)
    {
        if (activation_apply->init_traversal_state)
            age_vle_context_init_traversal_state(vlelctx);

        ops->load_initial_stacks(vlelctx);
    }

    if (activation_apply->mark_dirty)
        age_vle_context_mark_dirty(vlelctx);
}

static bool init_vle_traversal_cached_reuse_apply(
    VLETraversalCachedReuseApply *reuse_apply, VLE_local_context *vlelctx,
    const VLEContextRefreshInput *refresh)
{
    Assert(reuse_apply != NULL);
    Assert(vlelctx != NULL);
    Assert(refresh != NULL);

    if (!init_vle_traversal_refresh_apply(&reuse_apply->refresh, vlelctx,
                                          refresh))
        return false;

    init_vle_traversal_activation_apply(&reuse_apply->activation, vlelctx,
                                        true, false, false);

    return true;
}

static void apply_vle_traversal_cached_reuse(
    VLE_local_context *vlelctx,
    const VLETraversalCachedReuseApply *reuse_apply,
    FuncCallContext *funcctx, const VLETraversalApplyOps *ops)
{
    MemoryContext oldctx;

    Assert(vlelctx != NULL);
    Assert(reuse_apply != NULL);
    Assert(funcctx != NULL);

    apply_vle_traversal_refresh(vlelctx, &reuse_apply->refresh);

    oldctx = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);
    apply_vle_traversal_activation(vlelctx, &reuse_apply->activation, ops);
    MemoryContextSwitchTo(oldctx);
}

void apply_new_vle_context_activation(
    VLE_local_context *vlelctx, const VLETraversalApplyOps *ops)
{
    VLETraversalActivationApply activation_apply;

    Assert(vlelctx != NULL);

    init_vle_traversal_activation_apply(&activation_apply, vlelctx,
                                        false, true, true);
    apply_vle_traversal_activation(vlelctx, &activation_apply, ops);
}

void apply_vle_start_vertex_activation(
    VLE_local_context *vlelctx, const VLETraversalApplyOps *ops)
{
    VLETraversalActivationApply activation_apply;

    Assert(vlelctx != NULL);

    init_vle_traversal_activation_apply(&activation_apply, vlelctx,
                                        false, false, false);
    apply_vle_traversal_activation(vlelctx, &activation_apply, ops);
}

void init_vle_context_refresh_input(AgeVLEInput *input,
                                    VLEContextRefreshInput *refresh)
{
    Assert(input != NULL);
    Assert(refresh != NULL);

    refresh->start_valid = age_vle_input_get_vertex_or_id(
        input, 1, "start vertex argument must be a vertex or the integer id",
        &refresh->start_vertex_id);
    refresh->end_valid = age_vle_input_get_vertex_or_id(
        input, 2, "end vertex argument must be a vertex or the integer id",
        &refresh->end_vertex_id);
    refresh->terminal_label_id =
        input->terminal_label_known &&
        input->terminal_label_mode == AGE_VLE_TERMINAL_LABEL_ALL_DEPTH ?
        input->terminal_label_id : INVALID_LABEL_ID;
    refresh->terminal_endpoint_label_id =
        input->terminal_label_known &&
        input->terminal_label_mode ==
        AGE_VLE_TERMINAL_LABEL_ENDPOINT_ONLY ?
        input->terminal_label_id : INVALID_LABEL_ID;
    refresh->source_policy_known = input->source_policy_known;
    refresh->source_policy_outgoing_kind =
        input->source_policy_outgoing_kind;
    refresh->source_policy_incoming_kind =
        input->source_policy_incoming_kind;
    refresh->empty_lifecycle_policy_known =
        input->empty_lifecycle_policy_known;
    refresh->empty_lifecycle_eligible = input->empty_lifecycle_eligible;
    refresh->empty_lifecycle_depth = input->empty_lifecycle_depth;
    refresh->empty_lifecycle_batch_size =
        input->empty_lifecycle_batch_size;
    refresh->matrix_frontier_policy_known =
        input->matrix_frontier_policy_known;
    refresh->matrix_frontier_eligible = input->matrix_frontier_eligible;
    refresh->matrix_frontier_depth = input->matrix_frontier_depth;
    refresh->matrix_frontier_batch_size = input->matrix_frontier_batch_size;
}

bool apply_cached_vle_context_refresh(
    VLE_local_context *vlelctx, const VLEContextRefreshInput *refresh,
    FuncCallContext *funcctx, const VLETraversalApplyOps *ops)
{
    VLETraversalCachedReuseApply reuse_apply;

    Assert(vlelctx != NULL);
    Assert(refresh != NULL);
    Assert(funcctx != NULL);

    vlelctx->source_policy_known = refresh->source_policy_known;
    vlelctx->terminal_label_id = refresh->terminal_label_id;
    vlelctx->terminal_endpoint_label_id =
        refresh->terminal_endpoint_label_id;
    vlelctx->source_policy_outgoing_kind =
        refresh->source_policy_outgoing_kind;
    vlelctx->source_policy_incoming_kind =
        refresh->source_policy_incoming_kind;
    vlelctx->empty_lifecycle_policy_known =
        refresh->empty_lifecycle_policy_known;
    vlelctx->empty_lifecycle_eligible = refresh->empty_lifecycle_eligible;
    vlelctx->empty_lifecycle_depth = refresh->empty_lifecycle_depth;
    vlelctx->empty_lifecycle_batch_size =
        refresh->empty_lifecycle_batch_size;
    vlelctx->matrix_frontier_policy_known =
        refresh->matrix_frontier_policy_known;
    vlelctx->matrix_frontier_eligible = refresh->matrix_frontier_eligible;
    vlelctx->matrix_frontier_depth = refresh->matrix_frontier_depth;
    vlelctx->matrix_frontier_batch_size =
        refresh->matrix_frontier_batch_size;
    age_vle_context_record_empty_lifecycle_policy(vlelctx);
    age_vle_context_record_matrix_frontier_policy(vlelctx);

    if (!init_vle_traversal_cached_reuse_apply(&reuse_apply, vlelctx,
                                               refresh))
        return false;
    apply_vle_traversal_cached_reuse(vlelctx, &reuse_apply, funcctx, ops);

    return true;
}

void refresh_vle_traversal_source_layout(VLE_local_context *vlelctx)
{
    age_vle_context_refresh_source_layout(vlelctx);
}
