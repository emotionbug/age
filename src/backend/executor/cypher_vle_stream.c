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

#include "commands/explain_format.h"
#include "commands/explain_state.h"
#include "executor/cypher_vle_stream.h"
#include "executor/executor.h"
#include "funcapi.h"
#include "nodes/execnodes.h"
#include "utils/age_vle.h"
#include "utils/age_vle_source_cost.h"
#include "utils/agtype.h"
#include "utils/memutils.h"

typedef struct AgeVLEStreamScanState
{
    CustomScanState css;
    ExprState *argstates[AGE_VLE_STREAM_ARG_COUNT];
    Datum const_arg_values[AGE_VLE_STREAM_ARG_COUNT];
    bool const_arg_nulls[AGE_VLE_STREAM_ARG_COUNT];
    bool const_arg_valid[AGE_VLE_STREAM_ARG_COUNT];
    AgeVLEInput input;
    FuncCallContext *funcctx;
    AgeVLEIterator *iterator;
    int nargs;
    AgeVLEStreamGraph graph;
    AgeVLEStreamEdge edge;
    AgeVLEStreamRangeDirection range_direction;
    AgeVLEStreamOutput output;
    AgeVLEStreamEdgeSource edge_source;
    AgeVLESourceStats source_stats;
    AgeVLESourceStats current_source_stats;
    AgeVLESourceStats total_source_stats;
    MemoryContext scancontext;
    MemoryContext argcontext;
    MemoryContext multi_call_context;
    bool exhausted;
    bool source_stats_accumulated;
} AgeVLEStreamScanState;

static Node *create_age_vle_stream_scan_state(CustomScan *cscan);
static void begin_age_vle_stream_scan(CustomScanState *node, EState *estate,
                                      int eflags);
static TupleTableSlot *exec_age_vle_stream_scan(CustomScanState *node);
static TupleTableSlot *access_age_vle_stream_scan(ScanState *node);
static bool recheck_age_vle_stream_scan(ScanState *node, TupleTableSlot *slot);
static void end_age_vle_stream_scan(CustomScanState *node);
static void rescan_age_vle_stream_scan(CustomScanState *node);
static void explain_age_vle_stream_scan(CustomScanState *node,
                                        List *ancestors,
                                        ExplainState *es);
static void initialize_age_vle_stream_descriptor(AgeVLEStreamScanState *state,
                                                CustomScan *cscan,
                                                PlanState *parent);
static void initialize_age_vle_stream_input_descriptor(
    AgeVLEStreamScanState *state);
static int age_vle_stream_source_kind_to_input(
    AgeVLEStreamDirectedSourceKind kind);
static void initialize_age_vle_stream_iterator(AgeVLEStreamScanState *state,
                                              ExprContext *econtext);
static void evaluate_age_vle_stream_args(AgeVLEStreamScanState *state,
                                         ExprContext *econtext);
static void evaluate_age_vle_stream_endpoint_args(
    AgeVLEStreamScanState *state);
static void evaluate_age_vle_stream_endpoint_arg(AgeVLEStreamScanState *state,
                                                int argno,
                                                const char *type_error_msg);
static void reset_age_vle_stream_iterator(AgeVLEStreamScanState *state);
static void snapshot_age_vle_stream_source_stats(
    AgeVLEStreamScanState *state);
static void accumulate_age_vle_stream_source_stats(
    AgeVLESourceStats *total, const AgeVLESourceStats *current);
static void finalize_age_vle_stream_source_stats(
    AgeVLEStreamScanState *state);
const CustomScanMethods age_vle_stream_scan_methods = {
    AGE_VLE_STREAM_SCAN_NAME,
    create_age_vle_stream_scan_state};

static const CustomExecMethods age_vle_stream_exec_methods = {
    AGE_VLE_STREAM_SCAN_NAME,
    begin_age_vle_stream_scan,
    exec_age_vle_stream_scan,
    end_age_vle_stream_scan,
    rescan_age_vle_stream_scan,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    explain_age_vle_stream_scan};

static Node *create_age_vle_stream_scan_state(CustomScan *cscan)
{
    AgeVLEStreamScanState *state;

    state = palloc0(sizeof(*state));
    state->css.ss.ps.type = T_CustomScanState;
    state->css.flags = cscan->flags;
    state->css.methods = &age_vle_stream_exec_methods;

    return (Node *)state;
}

static void begin_age_vle_stream_scan(CustomScanState *node, EState *estate,
                                      int eflags)
{
    AgeVLEStreamScanState *state = (AgeVLEStreamScanState *)node;
    CustomScan *cscan = (CustomScan *)node->ss.ps.plan;
    Integer *nargs_value;

    (void) estate;
    (void) eflags;

    Assert(list_length(cscan->custom_private) ==
           AGE_VLE_STREAM_PRIVATE_COUNT);
    nargs_value = list_nth_node(Integer, cscan->custom_private,
                                AGE_VLE_STREAM_PRIVATE_NARGS);
    state->nargs = intVal(nargs_value);

    state->input.nargs = state->nargs;
    read_age_vle_stream_graph(cscan, &state->graph);
    read_age_vle_stream_edge(cscan, &state->edge);
    read_age_vle_stream_range_direction(cscan, &state->range_direction);
    read_age_vle_stream_output(cscan, &state->output);
    read_age_vle_stream_edge_source(cscan, &state->edge_source);
    initialize_age_vle_stream_input_descriptor(state);
    initialize_age_vle_stream_descriptor(state, cscan, &node->ss.ps);
    state->argcontext = AllocSetContextCreate(CurrentMemoryContext,
                                              "AGE VLE stream arguments",
                                              ALLOCSET_DEFAULT_SIZES);
    state->scancontext = CurrentMemoryContext;
    state->exhausted = false;
    state->source_stats_accumulated = false;
}

static void initialize_age_vle_stream_input_descriptor(
    AgeVLEStreamScanState *state)
{
    state->input.graph_name_known = state->graph.graph_known;
    state->input.graph_name_null = state->graph.graph_null;
    state->input.graph_name_value = state->graph.graph_name;
    state->input.graph_name_len = state->graph.graph_name == NULL ?
        0 : strlen(state->graph.graph_name);
    state->input.edge_prototype_known = state->edge.edge_known;
    state->input.edge_label_known = state->edge.label_known;
    state->input.edge_label_value = state->edge.label_name;
    state->input.edge_label_len = state->edge.label_name == NULL ?
        0 : strlen(state->edge.label_name);
    state->input.edge_properties_known = state->edge.properties_known;
    state->input.edge_properties_null = state->edge.properties_null;
    state->input.edge_properties_value = state->edge.properties_value;
    state->input.edge_property_constraint_count =
        state->edge.properties_count;
    state->input.lower_known = state->range_direction.lower_known;
    state->input.lower_null = state->range_direction.lower_null;
    state->input.lower_value = state->range_direction.lower_value;
    state->input.upper_known = state->range_direction.upper_known;
    state->input.upper_null = state->range_direction.upper_null;
    state->input.upper_value = state->range_direction.upper_value;
    state->input.direction_known = state->range_direction.direction_known;
    state->input.direction_null = state->range_direction.direction_null;
    state->input.direction_value = state->range_direction.direction_value;
    state->input.grammar_node_known = state->output.grammar_known;
    state->input.grammar_node_null = state->output.grammar_null;
    state->input.grammar_node_value = state->output.grammar_value;
    state->input.output_requirement = state->output.requirement;
    state->input.terminal_property_key_known =
        state->output.terminal_key_known;
    state->input.terminal_property_key_null =
        state->output.terminal_key_null;
    state->input.terminal_property_key_value =
        state->output.terminal_key_value;
    state->input.terminal_property_key_len =
        state->output.terminal_key_len;
    state->input.terminal_property_key_is_char =
        state->output.terminal_key_is_char;
    state->input.terminal_property_key_char =
        state->output.terminal_key_is_char ?
        state->output.terminal_key_value[0] : '\0';
    state->input.source_policy_known =
        state->edge_source.kind ==
        AGE_VLE_STREAM_EDGE_SOURCE_LOCAL_INDEX_CANDIDATE &&
        state->edge_source.local_edge_state;
    state->input.source_policy_outgoing_kind =
        age_vle_stream_source_kind_to_input(
            state->edge_source.policy_outgoing_kind);
    state->input.source_policy_incoming_kind =
        age_vle_stream_source_kind_to_input(
            state->edge_source.policy_incoming_kind);
    state->input.empty_lifecycle_policy_known =
        state->edge_source.kind ==
        AGE_VLE_STREAM_EDGE_SOURCE_LOCAL_INDEX_CANDIDATE;
    state->input.empty_lifecycle_eligible =
        state->edge_source.empty_lifecycle_eligible;
    state->input.empty_lifecycle_depth =
        state->edge_source.empty_lifecycle_depth;
    state->input.empty_lifecycle_batch_size =
        state->edge_source.empty_lifecycle_batch_size;
}

static int age_vle_stream_source_kind_to_input(
    AgeVLEStreamDirectedSourceKind kind)
{
    switch (kind)
    {
        case AGE_VLE_STREAM_DIRECTED_SOURCE_NONE:
            return VLE_TRAVERSAL_SOURCE_NONE;
        case AGE_VLE_STREAM_DIRECTED_SOURCE_AGE_ADJACENCY:
            return VLE_TRAVERSAL_SOURCE_AGE_ADJACENCY;
        case AGE_VLE_STREAM_DIRECTED_SOURCE_ENDPOINT_BTREE:
            return VLE_TRAVERSAL_SOURCE_ENDPOINT_BTREE;
    }

    return VLE_TRAVERSAL_SOURCE_NONE;
}

static void initialize_age_vle_stream_descriptor(AgeVLEStreamScanState *state,
                                                CustomScan *cscan,
                                                PlanState *parent)
{
    int argno;

    Assert(state->nargs == AGE_VLE_STREAM_ARG_GRAMMAR_NODE + 1 ||
           state->nargs == AGE_VLE_STREAM_ARG_TERMINAL_PROPERTY + 1);
    Assert(list_length(cscan->custom_exprs) == state->nargs);
    Assert(list_length(list_nth_node(List, cscan->custom_private,
                                     AGE_VLE_STREAM_PRIVATE_CONST_FLAGS)) ==
           state->nargs);

    for (argno = 0; argno < state->nargs; argno++)
    {
        Expr *arg = list_nth(cscan->custom_exprs, argno);
        List *const_flags = list_nth_node(List, cscan->custom_private,
                                          AGE_VLE_STREAM_PRIVATE_CONST_FLAGS);
        Integer *const_flag = list_nth_node(Integer, const_flags, argno);

        if (intVal(const_flag) != 0)
        {
            Const *const_arg;

            Assert(IsA(arg, Const));
            const_arg = (Const *)arg;
            state->const_arg_values[argno] = const_arg->constvalue;
            state->const_arg_nulls[argno] = const_arg->constisnull;
            state->const_arg_valid[argno] = true;
            state->input.args[argno].value = const_arg->constvalue;
            state->input.args[argno].isnull = const_arg->constisnull;
        }
        else
        {
            state->argstates[argno] = ExecInitExpr(arg, parent);
        }
    }
}

static TupleTableSlot *exec_age_vle_stream_scan(CustomScanState *node)
{
    return ExecScan(&node->ss, access_age_vle_stream_scan,
                    recheck_age_vle_stream_scan);
}

static TupleTableSlot *access_age_vle_stream_scan(ScanState *node)
{
    AgeVLEStreamScanState *state = (AgeVLEStreamScanState *)node;
    ExprContext *econtext = node->ps.ps_ExprContext;
    TupleTableSlot *slot = node->ss_ScanTupleSlot;
    Datum result;
    bool isnull;

    if (state->exhausted)
        return ExecClearTuple(slot);

    ExecClearTuple(slot);
    ResetExprContext(econtext);

    if (state->iterator == NULL)
        initialize_age_vle_stream_iterator(state, econtext);

    if (!age_vle_iterator_next(state->iterator, &result, &isnull))
    {
        state->exhausted = true;
        finalize_age_vle_stream_source_stats(state);
        return ExecClearTuple(slot);
    }
    snapshot_age_vle_stream_source_stats(state);

    if (slot->tts_tupleDescriptor->natts > 0)
    {
        slot->tts_values[0] = result;
        slot->tts_isnull[0] = isnull;
    }
    ExecStoreVirtualTuple(slot);

    return slot;
}

static bool recheck_age_vle_stream_scan(ScanState *node, TupleTableSlot *slot)
{
    (void)node;
    (void)slot;

    return true;
}

static void end_age_vle_stream_scan(CustomScanState *node)
{
    AgeVLEStreamScanState *state = (AgeVLEStreamScanState *)node;

    ReScanExprContext(node->ss.ps.ps_ExprContext);
    reset_age_vle_stream_iterator(state);
}

static void rescan_age_vle_stream_scan(CustomScanState *node)
{
    AgeVLEStreamScanState *state = (AgeVLEStreamScanState *)node;

    ReScanExprContext(node->ss.ps.ps_ExprContext);
    reset_age_vle_stream_iterator(state);
    if (state->argcontext != NULL)
        MemoryContextReset(state->argcontext);
    state->exhausted = false;
}

static void explain_age_vle_stream_scan(CustomScanState *node,
                                        List *ancestors,
                                        ExplainState *es)
{
    CustomScan *cscan = (CustomScan *)node->ss.ps.plan;
    AgeVLEStreamScanState *state = (AgeVLEStreamScanState *)node;
    int nargs = state->nargs;

    (void)ancestors;

    if (!es->verbose)
        return;

    ExplainPropertyText("VLE Shape",
                        age_vle_stream_shape_name(&state->output, nargs),
                        es);
    ExplainPropertyInteger("VLE Arguments", NULL, nargs, es);
    ExplainPropertyList("VLE Argument Slots",
                        make_age_vle_stream_slot_descriptions(cscan, nargs),
                        es);
    ExplainPropertyText("VLE Graph",
                        format_age_vle_stream_graph(&state->graph),
                        es);
    ExplainPropertyText("VLE Edge",
                        format_age_vle_stream_edge(&state->edge),
                        es);
    ExplainPropertyText("VLE Edge Source",
                        format_vle_stream_edge_source_evidence(
                            &state->edge_source),
                        es);
    ExplainPropertyText("VLE Endpoints",
                        format_age_vle_stream_endpoints(cscan),
                        es);
    ExplainPropertyText("VLE Range",
                        format_age_vle_stream_range(&state->range_direction),
                        es);
    ExplainPropertyText("VLE Direction",
                        format_age_vle_stream_direction(
                            &state->range_direction),
                        es);
    ExplainPropertyText("VLE Output",
                        format_age_vle_stream_output(&state->output, nargs),
                        es);
    ExplainPropertyText("VLE Materialization",
                        format_age_vle_stream_materialization(
                            &state->output, &state->edge_source),
                        es);
    if (nargs > AGE_VLE_STREAM_ARG_TERMINAL_PROPERTY)
    {
        ExplainPropertyText("VLE Terminal Output Slot",
                            format_age_vle_stream_terminal_slot(
                                cscan, &state->output, nargs),
                            es);
    }
    if (es->analyze)
    {
        ExplainPropertyText("VLE Source Runtime",
                            format_vle_source_runtime_evidence(
                                &state->source_stats, &state->edge_source),
                            es);
    }
}

static void initialize_age_vle_stream_iterator(AgeVLEStreamScanState *state,
                                              ExprContext *econtext)
{
    MemoryContext oldcontext;

    MemoryContextReset(state->argcontext);
    oldcontext = MemoryContextSwitchTo(state->argcontext);
    evaluate_age_vle_stream_args(state, econtext);
    MemoryContextSwitchTo(oldcontext);

    state->multi_call_context =
        AllocSetContextCreate(state->scancontext,
                              "AGE VLE stream multi-call context",
                              ALLOCSET_SMALL_SIZES);
    state->funcctx =
        MemoryContextAllocZero(state->multi_call_context,
                               sizeof(*state->funcctx));
    state->funcctx->multi_call_memory_ctx = state->multi_call_context;
    state->iterator =
        age_vle_iterator_create_from_input(&state->input, state->funcctx);
}

static void evaluate_age_vle_stream_args(AgeVLEStreamScanState *state,
                                         ExprContext *econtext)
{
    int argno;

    for (argno = 0; argno < state->nargs; argno++)
    {
        if (state->const_arg_valid[argno])
        {
            state->input.args[argno].value = state->const_arg_values[argno];
            state->input.args[argno].isnull = state->const_arg_nulls[argno];
            continue;
        }

        state->input.args[argno].value =
            ExecEvalExpr(state->argstates[argno], econtext,
                         &state->input.args[argno].isnull);
    }
    evaluate_age_vle_stream_endpoint_args(state);
}

static void evaluate_age_vle_stream_endpoint_args(
    AgeVLEStreamScanState *state)
{
    evaluate_age_vle_stream_endpoint_arg(
        state, AGE_VLE_STREAM_ARG_START,
        "start vertex argument must be a vertex or the integer id");
    evaluate_age_vle_stream_endpoint_arg(
        state, AGE_VLE_STREAM_ARG_END,
        "end vertex argument must be a vertex or the integer id");
}

static void evaluate_age_vle_stream_endpoint_arg(AgeVLEStreamScanState *state,
                                                int argno,
                                                const char *type_error_msg)
{
    AgeVLEInput *input = &state->input;
    agtype *agt_arg;
    agtype_value agtv_value;
    agtype_value *id;
    graphid vertex_id = 0;
    bool value_needs_free = false;
    bool is_null = true;

    if (argno >= input->nargs || input->args[argno].isnull)
        goto finish;

    agt_arg = DATUM_GET_AGTYPE_P(input->args[argno].value);
    if (!AGTYPE_CONTAINER_IS_SCALAR(&agt_arg->root))
    {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("age_vle: agtype argument must be a scalar")));
    }

    (void)get_ith_agtype_value_from_container_no_copy(&agt_arg->root, 0,
                                                      &agtv_value,
                                                      &value_needs_free);
    if (agtv_value.type == AGTV_NULL)
        goto cleanup;

    if (agtv_value.type == AGTV_VERTEX)
    {
        id = AGTYPE_VERTEX_GET_ID(&agtv_value);
    }
    else if (agtv_value.type == AGTV_INTEGER)
    {
        id = &agtv_value;
    }
    else
    {
        if (value_needs_free)
            pfree_agtype_value_content(&agtv_value);
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("%s", type_error_msg)));
    }

    vertex_id = id->val.int_value;
    is_null = false;

cleanup:
    if (value_needs_free)
        pfree_agtype_value_content(&agtv_value);

finish:
    if (argno == AGE_VLE_STREAM_ARG_START)
    {
        input->start_vertex_known = true;
        input->start_vertex_null = is_null;
        input->start_vertex_id = vertex_id;
    }
    else
    {
        input->end_vertex_known = true;
        input->end_vertex_null = is_null;
        input->end_vertex_id = vertex_id;
    }
}

static void reset_age_vle_stream_iterator(AgeVLEStreamScanState *state)
{
    if (state->iterator != NULL)
    {
        finalize_age_vle_stream_source_stats(state);
        age_vle_iterator_end(state->iterator);
    }

    state->iterator = NULL;
    state->funcctx = NULL;
    memset(&state->current_source_stats, 0,
           sizeof(state->current_source_stats));
    state->source_stats = state->total_source_stats;
    state->source_stats_accumulated = false;
    if (state->multi_call_context != NULL)
    {
        MemoryContextDelete(state->multi_call_context);
        state->multi_call_context = NULL;
    }
}

static void snapshot_age_vle_stream_source_stats(
    AgeVLEStreamScanState *state)
{
    Assert(state != NULL);

    if (state->iterator == NULL)
        return;

    age_vle_iterator_get_source_stats(state->iterator,
                                      &state->current_source_stats);
    state->source_stats = state->total_source_stats;
    accumulate_age_vle_stream_source_stats(&state->source_stats,
                                           &state->current_source_stats);
}

static void finalize_age_vle_stream_source_stats(
    AgeVLEStreamScanState *state)
{
    Assert(state != NULL);

    if (state->source_stats_accumulated)
        return;

    snapshot_age_vle_stream_source_stats(state);
    accumulate_age_vle_stream_source_stats(&state->total_source_stats,
                                           &state->current_source_stats);
    state->source_stats = state->total_source_stats;
    state->source_stats_accumulated = true;
    record_vle_source_runtime_threshold_feedback(state->graph.graph_name,
                                                 state->edge.label_name,
                                                 &state->source_stats,
                                                 &state->edge_source);
}

static void accumulate_age_vle_stream_source_stats(
    AgeVLESourceStats *total, const AgeVLESourceStats *current)
{
    Assert(total != NULL);
    Assert(current != NULL);

    total->missing_vertex_attempts += current->missing_vertex_attempts;
    total->missing_vertex_source_hits += current->missing_vertex_source_hits;
    total->age_adjacency_scans += current->age_adjacency_scans;
    total->age_adjacency_candidates += current->age_adjacency_candidates;
    total->age_adjacency_empty_scans +=
        current->age_adjacency_empty_scans;
    total->age_adjacency_empty_source_skips +=
        current->age_adjacency_empty_source_skips;
    total->age_adjacency_empty_source_skip_out +=
        current->age_adjacency_empty_source_skip_out;
    total->age_adjacency_empty_source_skip_in +=
        current->age_adjacency_empty_source_skip_in;
    total->age_adjacency_empty_source_cache_hits +=
        current->age_adjacency_empty_source_cache_hits;
    total->age_adjacency_empty_source_cache_hit_out +=
        current->age_adjacency_empty_source_cache_hit_out;
    total->age_adjacency_empty_source_cache_hit_in +=
        current->age_adjacency_empty_source_cache_hit_in;
    total->age_adjacency_empty_source_frontier_marks +=
        current->age_adjacency_empty_source_frontier_marks;
    total->age_adjacency_empty_source_frontier_mark_out +=
        current->age_adjacency_empty_source_frontier_mark_out;
    total->age_adjacency_empty_source_frontier_mark_in +=
        current->age_adjacency_empty_source_frontier_mark_in;
    total->age_adjacency_empty_source_frontier_batch_flushes +=
        current->age_adjacency_empty_source_frontier_batch_flushes;
    total->age_adjacency_empty_source_frontier_batch_out +=
        current->age_adjacency_empty_source_frontier_batch_out;
    total->age_adjacency_empty_source_frontier_batch_in +=
        current->age_adjacency_empty_source_frontier_batch_in;
    total->age_adjacency_empty_source_frontier_batch_keys +=
        current->age_adjacency_empty_source_frontier_batch_keys;
    total->age_adjacency_empty_source_frontier_batch_max =
        Max(total->age_adjacency_empty_source_frontier_batch_max,
            current->age_adjacency_empty_source_frontier_batch_max);
    total->age_adjacency_empty_source_run_skips +=
        current->age_adjacency_empty_source_run_skips;
    total->age_adjacency_empty_source_run_skip_out +=
        current->age_adjacency_empty_source_run_skip_out;
    total->age_adjacency_empty_source_run_skip_in +=
        current->age_adjacency_empty_source_run_skip_in;
    total->age_adjacency_payload_scan_runs +=
        current->age_adjacency_payload_scan_runs;
    total->age_adjacency_payload_replay_runs +=
        current->age_adjacency_payload_replay_runs;
    total->age_adjacency_payload_cache_seed_runs +=
        current->age_adjacency_payload_cache_seed_runs;
    total->age_adjacency_payload_scans +=
        current->age_adjacency_payload_scans;
    total->age_adjacency_payload_replays +=
        current->age_adjacency_payload_replays;
    total->age_adjacency_payload_cache_seeds +=
        current->age_adjacency_payload_cache_seeds;
    total->endpoint_btree_scans += current->endpoint_btree_scans;
    total->endpoint_btree_candidates += current->endpoint_btree_candidates;
    total->endpoint_btree_empty_scans +=
        current->endpoint_btree_empty_scans;
    total->packed_scans += current->packed_scans;
    total->packed_candidates += current->packed_candidates;
    total->packed_empty_skips += current->packed_empty_skips;
    total->packed_policy_skips += current->packed_policy_skips;
    total->packed_suppress_out += current->packed_suppress_out;
    total->packed_suppress_in += current->packed_suppress_in;
    total->packed_suppress_self += current->packed_suppress_self;
    total->candidates_yielded += current->candidates_yielded;
    total->candidates_pushed += current->candidates_pushed;
    total->empty_lifecycle_context_runs +=
        current->empty_lifecycle_context_runs;
    total->empty_lifecycle_context_eligible_runs +=
        current->empty_lifecycle_context_eligible_runs;
    total->empty_lifecycle_context_depth =
        Max(total->empty_lifecycle_context_depth,
            current->empty_lifecycle_context_depth);
    total->empty_lifecycle_batch_capacity =
        Max(total->empty_lifecycle_batch_capacity,
            current->empty_lifecycle_batch_capacity);
    total->root_empty_completion_count +=
        current->root_empty_completion_count;
    total->root_empty_completion_out +=
        current->root_empty_completion_out;
    total->root_empty_completion_in +=
        current->root_empty_completion_in;
    total->root_empty_batch_capacity =
        Max(total->root_empty_batch_capacity,
            current->root_empty_batch_capacity);
    total->root_empty_saturated_count +=
        current->root_empty_saturated_count;
}
