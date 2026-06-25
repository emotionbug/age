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

#include "access/age_adjacency.h"
#include "access/parallel.h"
#include "catalog/ag_label.h"
#include "commands/explain_format.h"
#include "commands/explain_state.h"
#include "executor/cypher_adjacency_match.h"
#include "executor/cypher_adjacency_match_terminal.h"
#include "executor/executor.h"
#include "lib/stringinfo.h"
#include "optimizer/cypher_graph_join.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/graphid.h"
#include "utils/age_vle_source_cost.h"
#include "utils/agtype.h"
#include "utils/memutils.h"

typedef struct AgeAdjacencyMatchTuple
{
    graphid edge_id;
    graphid start_id;
    graphid end_id;
    Datum properties;
    bool properties_isnull;
} AgeAdjacencyMatchTuple;

typedef struct AgeAdjacencyMatchParallelState
{
    uint32 magic;
    int32 planned_workers;
} AgeAdjacencyMatchParallelState;

typedef enum AgeAdjacencyTerminalPrunePlan
{
    AGE_ADJACENCY_TERMINAL_PRUNE_NONE = 0,
    AGE_ADJACENCY_TERMINAL_PRUNE_SOURCE_PREFETCH,
    AGE_ADJACENCY_TERMINAL_PRUNE_ID_CACHE_SMALL,
    AGE_ADJACENCY_TERMINAL_PRUNE_VERTEX_CACHE,
    AGE_ADJACENCY_TERMINAL_PRUNE_DEFERRED
} AgeAdjacencyTerminalPrunePlan;

typedef enum AgeAdjacencyTerminalPolicyClass
{
    AGE_ADJACENCY_TERMINAL_CLASS_NONE = 0,
    AGE_ADJACENCY_TERMINAL_CLASS_COMPOSITE_PREFILTER,
    AGE_ADJACENCY_TERMINAL_CLASS_COMPOSITE_ID_CACHE,
    AGE_ADJACENCY_TERMINAL_CLASS_COMPOSITE_DEFERRED,
    AGE_ADJACENCY_TERMINAL_CLASS_COMPOSITE_RECHECK
} AgeAdjacencyTerminalPolicyClass;

typedef enum AgeAdjacencyPayloadSliceMode
{
    AGE_ADJACENCY_PAYLOAD_SLICE_SERIAL_ONLY = 0,
    AGE_ADJACENCY_PAYLOAD_SLICE_WORKER_LOCAL,
    AGE_ADJACENCY_PAYLOAD_SLICE_WORKER_LOCAL_READY,
    AGE_ADJACENCY_PAYLOAD_SLICE_SHARED_STATE_REQUIRED
} AgeAdjacencyPayloadSliceMode;

typedef struct AgeAdjacencyMatchScanState
{
    CustomScanState css;
    Oid graph_oid;
    Oid index_oid;
    bool outgoing;
    bool fetch_properties;
    bool has_edge_variable_projection;
    bool has_edge_property_predicate;
    bool has_right_label_constraint;
    bool has_right_property_predicate;
    bool edge_payload_required;
    int payload_attr_mask;
    int32 right_label_id;
    AttrNumber endpoint_attno;
    int64 estimated_endpoint_fanout;
    int64 estimated_terminal_fanout;
    int64 estimated_composite_fanout;
    int64 composite_selectivity_ppm;
    int64 estimated_terminal_label_groups;
    int64 estimated_main_blocks;
    char *fanout_source;
    AgeGraphPropertySelectivitySource composite_selectivity_source_kind;
    VLESourceValuePostingKind value_posting_source_kind;
    char *index_source;
    AgeAdjacencyMatchIndexKind index_kind_id;
    char *index_provider;
    AgeAdjacencyMatchIndexDirection index_direction_id;
    int32 index_property_count;
    bool index_metadata_backed;
    char *right_property_key;
    Oid right_property_index_oid;
    char *right_property_index_source;
    char *right_property_index_provider;
    char *right_property_index_type;
    bool right_property_index_metadata_backed;
    bool right_property_prefetch_eligible;
    AgeAdjacencyMatchValueKind right_property_value_kind;
    AgeAdjacencyMatchTerminalStrategy terminal_source_strategy;
    int64 terminal_prefetch_threshold;
    AgeAdjacencyMatchTerminalPrefetchReason terminal_prefetch_reason_kind;
    char *join_order_component;
    AgeGraphJoinConnectorKind join_order_connector_kind;
    char *join_order_bound;
    AgeGraphJoinOrderPropertyKind join_order_property_kind;
    char *join_order_source_evidence;
    char *join_order_solved_relids;
    int64 join_order_candidate_count;
    int64 join_order_declared_cover_count;
    AgeGraphJoinCoverMatchKind join_order_cover_match_kind;
    AgeGraphJoinConnectorKind join_order_next_connector_kind;
    AgeGraphJoinOrderPropertyKind join_order_next_property_kind;
    char *join_order_next_source_evidence;
    bool graph_join_parallel_safe;
    bool graph_join_parallel_aware;
    int64 graph_join_parallel_workers;
    double graph_join_gather_cost;
    bool graph_join_order_preserving;
    bool graph_join_shared_state_required;
    AgeAdjacencyPayloadSliceMode payload_slice_mode;
    int32 payload_slice_index;
    int32 payload_slice_count;
    int32 payload_parallel_workers;
    Datum right_property_value;
    bool right_property_value_isnull;
    AgeAdjacencyMatchTerminalPropertyLookup *terminal_property_lookup;
    int scan_nattrs;
    AttrNumber *scan_attnos;
    graphid current_key;
    AgeAdjacencyVisiblePayloadScan *payload_scan;
    ExprState *key_expr_state;
    ExprState *property_value_expr_state;
    MemoryContext payload_scan_context;
    MemoryContext scan_context;
    bool payload_active;
    bool scanned;
    int64 payload_candidates;
    int64 terminal_filtered;
    int64 rows_emitted;
} AgeAdjacencyMatchScanState;

static Node *create_age_adjacency_match_scan_state(CustomScan *cscan);
static void begin_age_adjacency_match_scan(CustomScanState *node,
                                           EState *estate, int eflags);
static TupleTableSlot *exec_age_adjacency_match_scan(CustomScanState *node);
static TupleTableSlot *access_age_adjacency_match_scan(ScanState *node);
static bool recheck_age_adjacency_match_scan(ScanState *node,
                                             TupleTableSlot *slot);
static void end_age_adjacency_match_scan(CustomScanState *node);
static void rescan_age_adjacency_match_scan(CustomScanState *node);
static Size estimate_age_adjacency_match_dsm(CustomScanState *node,
                                             ParallelContext *pcxt);
static void initialize_age_adjacency_match_dsm(CustomScanState *node,
                                               ParallelContext *pcxt,
                                               void *coordinate);
static void reinitialize_age_adjacency_match_dsm(CustomScanState *node,
                                                 ParallelContext *pcxt,
                                                 void *coordinate);
static void initialize_age_adjacency_match_worker(CustomScanState *node,
                                                  shm_toc *toc,
                                                  void *coordinate);
static void explain_age_adjacency_match_scan(CustomScanState *node,
                                             List *ancestors,
                                             ExplainState *es);
static void load_age_adjacency_match_descriptor(
    AgeAdjacencyMatchScanState *state, CustomScan *cscan);
static bool adjacency_match_descriptor_bool(List *descriptor, int index);
static int64 adjacency_match_descriptor_int64(List *descriptor, int index);
static double adjacency_match_descriptor_float8(List *descriptor, int index);
static char *adjacency_match_descriptor_text(List *descriptor, int index);
static char *format_age_adjacency_match_index_descriptor(
    AgeAdjacencyMatchScanState *state);
static char *format_age_adjacency_match_index_metadata(
    AgeAdjacencyMatchScanState *state);
static char *format_age_adjacency_match_pruning_descriptor(
    AgeAdjacencyMatchScanState *state);
static char *format_age_adjacency_match_cost_input(
    AgeAdjacencyMatchScanState *state);
static char *format_age_adjacency_match_join_order(
    AgeAdjacencyMatchScanState *state);
static char *format_age_adjacency_match_composite_source(
    AgeAdjacencyMatchScanState *state);
static char *format_age_adjacency_match_composite_policy(
    AgeAdjacencyMatchScanState *state, bool analyze);
static char *format_age_adjacency_match_terminal_property(
    AgeAdjacencyMatchScanState *state);
static char *format_age_adjacency_match_terminal_runtime(
    AgeAdjacencyMatchScanState *state);
static char *format_age_adjacency_match_terminal_prefetch(
    AgeAdjacencyMatchScanState *state);
static const char *age_adjacency_match_terminal_runtime_outcome(
    AgeAdjacencyMatchScanState *state);
static const char *age_adjacency_match_terminal_runtime_outcome_name(
    AgeAdjacencyTerminalPrunePlan outcome);
static AgeAdjacencyTerminalPrunePlan
age_adjacency_match_terminal_runtime_outcome_id(
    AgeAdjacencyMatchScanState *state);
static const char *age_adjacency_match_terminal_runtime_action(
    AgeAdjacencyMatchScanState *state);
static const char *age_adjacency_match_terminal_planned_prefetch(
    AgeAdjacencyMatchScanState *state);
static AgeAdjacencyTerminalPrunePlan
age_adjacency_match_terminal_planned_prefetch_id(
    AgeAdjacencyMatchScanState *state);
static const char *age_adjacency_match_terminal_policy_class(
    AgeAdjacencyMatchScanState *state);
static AgeAdjacencyTerminalPolicyClass
age_adjacency_match_terminal_policy_class_id(
    AgeAdjacencyMatchScanState *state);
static const char *age_adjacency_match_terminal_policy_recommendation(
    AgeAdjacencyMatchScanState *state);
static const char *age_adjacency_match_terminal_runtime_class(
    AgeAdjacencyMatchScanState *state);
static AgeAdjacencyTerminalPolicyClass
age_adjacency_match_terminal_runtime_class_id(
    AgeAdjacencyMatchScanState *state);
static const char *age_adjacency_match_terminal_runtime_recommendation(
    AgeAdjacencyMatchScanState *state);
static bool age_adjacency_match_terminal_class_matches(
    AgeAdjacencyMatchScanState *state);
static bool age_adjacency_match_terminal_lifecycle_matches(
    AgeAdjacencyMatchScanState *state);
static const char *age_adjacency_match_terminal_value_identity(
    AgeAdjacencyMatchScanState *state);
static char *format_age_adjacency_match_payload_columns(
    AgeAdjacencyMatchScanState *state);
static char *format_age_adjacency_match_payload_runtime(
    AgeAdjacencyMatchScanState *state);
static void append_age_adjacency_match_payload_mask(StringInfo buf,
                                                    int payload_attr_mask);
static const char *age_adjacency_match_attr_name(AttrNumber attno);
static const char *age_adjacency_match_terminal_property_prune_mode(
    AgeAdjacencyMatchScanState *state);
static AgeAdjacencyTerminalPrunePlan
age_adjacency_match_terminal_property_prune_mode_id(
    AgeAdjacencyMatchScanState *state);
static const char *age_adjacency_match_terminal_property_residual_mode(
    AgeAdjacencyMatchScanState *state);
static const char *age_adjacency_match_terminal_prune_plan_name(
    AgeAdjacencyTerminalPrunePlan plan);
static const char *age_adjacency_match_terminal_policy_class_name(
    AgeAdjacencyTerminalPolicyClass policy_class);
static const char *age_adjacency_match_payload_slice_mode_name(
    AgeAdjacencyPayloadSliceMode mode);
static const char *age_adjacency_match_value_kind_name(
    AgeAdjacencyMatchValueKind kind);
static const char *age_adjacency_match_terminal_strategy_name(
    AgeAdjacencyMatchTerminalStrategy strategy);
static int age_adjacency_match_residual_predicate_count(
    AgeAdjacencyMatchScanState *state);
static int age_adjacency_match_index_solved_predicate_count(
    AgeAdjacencyMatchScanState *state);
static void store_age_adjacency_match_tuple(
    AgeAdjacencyMatchScanState *state,
    const AgeAdjacencyPayload *payload,
    AgeAdjacencyMatchTuple *tuple);
static AgeAdjacencyMatchTerminalPropertyRequest
make_age_adjacency_match_terminal_property_request(
    AgeAdjacencyMatchScanState *state);
static void configure_age_adjacency_match_payload_slice(
    AgeAdjacencyMatchScanState *state);
static void initialize_age_adjacency_match_parallel_state(
    AgeAdjacencyMatchParallelState *parallel_state, ParallelContext *pcxt);

const CustomScanMethods age_adjacency_match_scan_methods = {
    AGE_ADJACENCY_MATCH_SCAN_NAME, create_age_adjacency_match_scan_state};

#define AGE_ADJACENCY_MATCH_PARALLEL_MAGIC 0x41475053U

static const CustomExecMethods age_adjacency_match_exec_methods = {
    AGE_ADJACENCY_MATCH_SCAN_NAME,
    begin_age_adjacency_match_scan,
    exec_age_adjacency_match_scan,
    end_age_adjacency_match_scan,
    rescan_age_adjacency_match_scan,
    NULL,
    NULL,
    estimate_age_adjacency_match_dsm,
    initialize_age_adjacency_match_dsm,
    reinitialize_age_adjacency_match_dsm,
    initialize_age_adjacency_match_worker,
    NULL,
    explain_age_adjacency_match_scan};

static Node *create_age_adjacency_match_scan_state(CustomScan *cscan)
{
    AgeAdjacencyMatchScanState *state;
    Const *index_const;
    Const *outgoing_const;
    ListCell *lc;
    int nattrs = 0;

    state = palloc0(sizeof(*state));

    Assert(list_length(cscan->custom_private) == 3);

    index_const = linitial(cscan->custom_private);
    outgoing_const = lsecond(cscan->custom_private);

    state->index_oid = DatumGetObjectId(index_const->constvalue);
    state->outgoing = DatumGetBool(outgoing_const->constvalue);
    state->fetch_properties = true;
    state->css.ss.ps.type = T_CustomScanState;
    state->css.flags = cscan->flags;
    state->css.methods = &age_adjacency_match_exec_methods;
    load_age_adjacency_match_descriptor(state, cscan);

    if (cscan->custom_scan_tlist != NIL)
    {
        state->scan_nattrs = list_length(cscan->custom_scan_tlist);
        state->scan_attnos =
            palloc0(sizeof(AttrNumber) * state->scan_nattrs);

        foreach(lc, cscan->custom_scan_tlist)
        {
            TargetEntry *tle = lfirst_node(TargetEntry, lc);
            Var *var;

            Assert(IsA(tle->expr, Var));
            var = (Var *)tle->expr;
            state->scan_attnos[nattrs++] = var->varattno;
        }

        state->fetch_properties = false;
        for (nattrs = 0; nattrs < state->scan_nattrs; nattrs++)
        {
            if (state->scan_attnos[nattrs] ==
                Anum_ag_label_edge_table_properties)
            {
                state->fetch_properties = true;
                break;
            }
        }
    }

    return (Node *)state;
}

static void begin_age_adjacency_match_scan(CustomScanState *node,
                                           EState *estate, int eflags)
{
    AgeAdjacencyMatchScanState *state =
        (AgeAdjacencyMatchScanState *)node;
    CustomScan *cscan = (CustomScan *)node->ss.ps.plan;
    AgeAdjacencyCompositeTerminalFilter terminal_filter;
    MemoryContext oldcontext;

    (void) eflags;

    state->scan_context = AllocSetContextCreate(
        node->ss.ps.state->es_query_cxt,
        "AGE adjacency match scan context",
        ALLOCSET_DEFAULT_SIZES);
    state->payload_scan_context = node->ss.ps.state->es_query_cxt;
    oldcontext = MemoryContextSwitchTo(state->payload_scan_context);
    state->payload_scan = age_adjacency_begin_visible_payload_scan(
        state->index_oid, estate->es_snapshot, state->fetch_properties);
    configure_age_adjacency_match_payload_slice(state);
    memset(&terminal_filter, 0, sizeof(terminal_filter));
    terminal_filter.terminal_label_id = state->right_label_id;
    terminal_filter.source = "label";
    age_adjacency_visible_payload_scan_set_composite_terminal_filter(
        state->payload_scan, &terminal_filter);
    MemoryContextSwitchTo(oldcontext);

    if (cscan->custom_exprs != NIL)
    {
        Expr *key_expr = linitial(cscan->custom_exprs);
        Expr *property_value_expr = lsecond(cscan->custom_exprs);

        state->key_expr_state =
            ExecInitExpr(key_expr, (PlanState *)node);
        state->property_value_expr_state =
            ExecInitExpr(property_value_expr, (PlanState *)node);
    }
    {
        AgeAdjacencyMatchTerminalPropertyRequest request;

        request = make_age_adjacency_match_terminal_property_request(state);
        state->terminal_property_lookup =
            age_adjacency_match_terminal_property_begin(
                &request, node->ss.ps.state->es_query_cxt);
        if (age_adjacency_match_terminal_property_prefilter_active(
                state->terminal_property_lookup))
        {
            AgeAdjacencyCompositeTerminalFilter composite_filter;

            memset(&composite_filter, 0, sizeof(composite_filter));
            composite_filter.terminal_label_id = state->right_label_id;
            composite_filter.property_index_oid =
                age_adjacency_match_terminal_property_index_oid(
                    state->terminal_property_lookup);
            composite_filter.property_filter_id =
                age_adjacency_match_terminal_property_filter_id(
                    state->terminal_property_lookup);
            composite_filter.property_match_count =
                age_adjacency_match_terminal_property_prefetched_matches(
                    state->terminal_property_lookup);
            composite_filter.has_property_summary = true;
            if (age_adjacency_match_terminal_property_prefilter_set(
                    state->terminal_property_lookup,
                    &composite_filter.vertex_set_filter))
            {
                composite_filter.has_vertex_set_filter = true;
                composite_filter.source = "label-property-prefetch";
            }
            else
            {
                composite_filter.vertex_filter =
                    age_adjacency_match_terminal_property_prefilter_matches;
                composite_filter.vertex_filter_state =
                    state->terminal_property_lookup;
                composite_filter.has_vertex_filter = true;
                composite_filter.source = "label-property-callback";
            }
            age_adjacency_visible_payload_scan_set_composite_terminal_filter(
                state->payload_scan, &composite_filter);
        }
    }
}

static TupleTableSlot *exec_age_adjacency_match_scan(CustomScanState *node)
{
    return ExecScan(&node->ss, access_age_adjacency_match_scan,
                    recheck_age_adjacency_match_scan);
}

static TupleTableSlot *access_age_adjacency_match_scan(ScanState *node)
{
    AgeAdjacencyMatchScanState *state =
        (AgeAdjacencyMatchScanState *)node;
    TupleTableSlot *slot = node->ss_ScanTupleSlot;

    if (!state->scanned)
    {
        ExprContext *econtext = node->ps.ps_ExprContext;
        MemoryContext oldcontext;
        Datum key_datum;
        Datum property_value_datum;
        bool isnull = false;
        bool property_value_isnull = true;

        state->scanned = true;
        MemoryContextReset(state->scan_context);
        state->payload_active = false;

        if (state->key_expr_state == NULL)
            return ExecClearTuple(slot);

        key_datum = ExecEvalExpr(state->key_expr_state, econtext, &isnull);
        if (state->property_value_expr_state != NULL)
            property_value_datum = ExecEvalExpr(state->property_value_expr_state,
                                                econtext,
                                                &property_value_isnull);
        else
            property_value_datum = (Datum)0;
        age_adjacency_match_terminal_property_set_value(
            state->terminal_property_lookup, property_value_datum,
            property_value_isnull);
        age_adjacency_visible_payload_scan_set_terminal_vertex_filter(
            state->payload_scan, NULL, NULL);
        if (!isnull)
        {
            state->current_key = DATUM_GET_GRAPHID(key_datum);
            oldcontext = MemoryContextSwitchTo(state->payload_scan_context);
            state->payload_active =
                age_adjacency_visible_payload_scan_begin_key(
                    state->payload_scan, state->current_key);
            MemoryContextSwitchTo(oldcontext);
            if (state->payload_active &&
                age_adjacency_match_terminal_property_prepare_prefilter(
                    state->terminal_property_lookup,
                    age_adjacency_visible_payload_scan_run_postings(
                        state->payload_scan),
                    age_adjacency_visible_payload_scan_active_postings(
                        state->payload_scan),
                    state->terminal_prefetch_threshold))
            {
                AgeAdjacencyCompositeTerminalFilter composite_filter;

                memset(&composite_filter, 0, sizeof(composite_filter));
                composite_filter.terminal_label_id = state->right_label_id;
                composite_filter.property_index_oid =
                    age_adjacency_match_terminal_property_index_oid(
                        state->terminal_property_lookup);
                composite_filter.property_filter_id =
                    age_adjacency_match_terminal_property_filter_id(
                        state->terminal_property_lookup);
                composite_filter.property_match_count =
                    age_adjacency_match_terminal_property_prefetched_matches(
                        state->terminal_property_lookup);
                composite_filter.has_property_summary = true;
                if (age_adjacency_match_terminal_property_prefilter_set(
                        state->terminal_property_lookup,
                        &composite_filter.vertex_set_filter))
                {
                    composite_filter.has_vertex_set_filter = true;
                    composite_filter.source = "label-property-prefetch";
                }
                else
                {
                    composite_filter.vertex_filter =
                        age_adjacency_match_terminal_property_prefilter_matches;
                    composite_filter.vertex_filter_state =
                        state->terminal_property_lookup;
                    composite_filter.has_vertex_filter = true;
                    composite_filter.source = "label-property-callback";
                }
                age_adjacency_visible_payload_scan_set_composite_terminal_filter(
                    state->payload_scan, &composite_filter);
            }
        }
    }

    while (state->payload_active)
    {
        AgeAdjacencyPayload payload;
        AgeAdjacencyMatchTuple tuple;
        MemoryContext oldcontext;
        bool found;
        int i;

        oldcontext = MemoryContextSwitchTo(state->payload_scan_context);
        found = age_adjacency_visible_payload_scan_next(state->payload_scan,
                                                        &payload);
        MemoryContextSwitchTo(oldcontext);

        if (!found)
        {
            state->payload_active = false;
            return ExecClearTuple(slot);
        }

        state->payload_candidates++;
        if (age_adjacency_match_terminal_property_active(
                state->terminal_property_lookup) &&
            !age_adjacency_match_terminal_property_prefilter_active(
                state->terminal_property_lookup) &&
            !age_adjacency_match_terminal_property_matches(
                state->terminal_property_lookup, payload.next_vertex_id))
        {
            state->terminal_filtered++;
            continue;
        }

        ExecClearTuple(slot);
        MemoryContextReset(state->scan_context);
        store_age_adjacency_match_tuple(state, &payload, &tuple);

        if (state->scan_nattrs > 0)
        {
            for (i = 0; i < state->scan_nattrs; i++)
            {
                switch (state->scan_attnos[i])
                {
                case Anum_ag_label_edge_table_id:
                    slot->tts_values[i] = GRAPHID_GET_DATUM(tuple.edge_id);
                    slot->tts_isnull[i] = false;
                    break;
                case Anum_ag_label_edge_table_start_id:
                    slot->tts_values[i] = GRAPHID_GET_DATUM(tuple.start_id);
                    slot->tts_isnull[i] = false;
                    break;
                case Anum_ag_label_edge_table_end_id:
                    slot->tts_values[i] = GRAPHID_GET_DATUM(tuple.end_id);
                    slot->tts_isnull[i] = false;
                    break;
                case Anum_ag_label_edge_table_properties:
                    slot->tts_values[i] = tuple.properties;
                    slot->tts_isnull[i] = tuple.properties_isnull;
                    break;
                default:
                    elog(ERROR, "unexpected AGE adjacency match attr %d",
                         state->scan_attnos[i]);
                }
            }
        }
        else
        {
            slot->tts_values[Anum_ag_label_edge_table_id - 1] =
                GRAPHID_GET_DATUM(tuple.edge_id);
            slot->tts_isnull[Anum_ag_label_edge_table_id - 1] = false;
            slot->tts_values[Anum_ag_label_edge_table_start_id - 1] =
                GRAPHID_GET_DATUM(tuple.start_id);
            slot->tts_isnull[Anum_ag_label_edge_table_start_id - 1] = false;
            slot->tts_values[Anum_ag_label_edge_table_end_id - 1] =
                GRAPHID_GET_DATUM(tuple.end_id);
            slot->tts_isnull[Anum_ag_label_edge_table_end_id - 1] = false;
            slot->tts_values[Anum_ag_label_edge_table_properties - 1] =
                tuple.properties;
            slot->tts_isnull[Anum_ag_label_edge_table_properties - 1] =
                tuple.properties_isnull;
        }

        ExecStoreVirtualTuple(slot);
        state->rows_emitted++;
        return slot;
    }

    return ExecClearTuple(slot);
}

static bool recheck_age_adjacency_match_scan(ScanState *node,
                                             TupleTableSlot *slot)
{
    (void) node;
    (void) slot;

    return true;
}

static void end_age_adjacency_match_scan(CustomScanState *node)
{
    AgeAdjacencyMatchScanState *state =
        (AgeAdjacencyMatchScanState *)node;

    if (state->payload_scan != NULL)
    {
        age_adjacency_end_visible_payload_scan(state->payload_scan);
        state->payload_scan = NULL;
    }
    if (state->scan_context != NULL)
    {
        MemoryContextDelete(state->scan_context);
        state->scan_context = NULL;
    }
    age_adjacency_match_terminal_property_end(
        state->terminal_property_lookup);
    state->terminal_property_lookup = NULL;
}

static void rescan_age_adjacency_match_scan(CustomScanState *node)
{
    AgeAdjacencyMatchScanState *state =
        (AgeAdjacencyMatchScanState *)node;

    if (state->scan_context != NULL)
        MemoryContextReset(state->scan_context);
    age_adjacency_match_terminal_property_rescan(
        state->terminal_property_lookup);
    age_adjacency_visible_payload_scan_reset_runtime(state->payload_scan);
    state->payload_active = false;
    state->scanned = false;
    state->payload_candidates = 0;
    state->terminal_filtered = 0;
    state->rows_emitted = 0;
}

static void explain_age_adjacency_match_scan(CustomScanState *node,
                                             List *ancestors,
                                             ExplainState *es)
{
    AgeAdjacencyMatchScanState *state =
        (AgeAdjacencyMatchScanState *)node;

    (void)ancestors;

    ExplainPropertyText("Adjacency Index",
                        format_age_adjacency_match_index_descriptor(state),
                        es);
    ExplainPropertyText("Adjacency Index Descriptor",
                        format_age_adjacency_match_index_metadata(state),
                        es);
    ExplainPropertyText("Adjacency Pruning",
                        format_age_adjacency_match_pruning_descriptor(state),
                        es);
    ExplainPropertyText("Adjacency Cost Input",
                        format_age_adjacency_match_cost_input(state),
                        es);
    ExplainPropertyText("Adjacency Join Order",
                        format_age_adjacency_match_join_order(state),
                        es);
    ExplainPropertyText("Adjacency Parallel",
                        psprintf("safe=%s aware=%s workers=%ld "
                                 "gather-cost=%.2f order-preserving=%s "
                                 "shared-state=%s slice=%s active=%s",
                                 state->graph_join_parallel_safe ?
                                 "true" : "false",
                                 state->graph_join_parallel_aware ?
                                 "true" : "false",
                                 (long)state->graph_join_parallel_workers,
                                 state->graph_join_gather_cost,
                                 state->graph_join_order_preserving ?
                                 "true" : "false",
                                 state->graph_join_shared_state_required ?
                                 "true" : "false",
                                 age_adjacency_match_payload_slice_mode_name(
                                     state->payload_slice_mode),
                                 state->payload_slice_count > 1 ?
                                 psprintf("%d/%d",
                                          state->payload_slice_index,
                                          state->payload_slice_count) :
                                 "serial"),
                        es);
    if (state->has_right_label_constraint ||
        state->has_right_property_predicate)
    {
        ExplainPropertyText("Adjacency Composite Source",
                            format_age_adjacency_match_composite_source(
                                state),
                            es);
        if (state->has_right_property_predicate)
            ExplainPropertyText("Adjacency Composite Policy",
                                format_age_adjacency_match_composite_policy(
                                    state, es->analyze),
                                es);
    }
    ExplainPropertyText("Adjacency Terminal Property",
                        format_age_adjacency_match_terminal_property(state),
                        es);
    if (es->analyze)
    {
        ExplainPropertyText("Adjacency Terminal Runtime",
                            format_age_adjacency_match_terminal_runtime(
                                state),
                            es);
        ExplainPropertyText("Adjacency Terminal Prefetch",
                            format_age_adjacency_match_terminal_prefetch(
                                state),
                            es);
    }
    ExplainPropertyText("Adjacency Payload Columns",
                        format_age_adjacency_match_payload_columns(state),
                        es);
    if (es->analyze)
    {
        ExplainPropertyText("Adjacency Payload Runtime",
                            format_age_adjacency_match_payload_runtime(
                                state),
                            es);
    }
}

static void load_age_adjacency_match_descriptor(
    AgeAdjacencyMatchScanState *state, CustomScan *cscan)
{
    List *descriptor;

    descriptor = lthird(cscan->custom_private);
    Assert(list_length(descriptor) == AGE_ADJACENCY_MATCH_DESC_COUNT);

    state->graph_oid = (Oid)adjacency_match_descriptor_int64(
        descriptor, AGE_ADJACENCY_MATCH_DESC_GRAPH_OID);
    state->has_edge_variable_projection =
        adjacency_match_descriptor_bool(
            descriptor, AGE_ADJACENCY_MATCH_DESC_EDGE_VARIABLE);
    state->has_edge_property_predicate =
        adjacency_match_descriptor_bool(
            descriptor, AGE_ADJACENCY_MATCH_DESC_EDGE_PROPERTY_PREDICATE);
    state->has_right_label_constraint =
        adjacency_match_descriptor_bool(
            descriptor, AGE_ADJACENCY_MATCH_DESC_RIGHT_LABEL_CONSTRAINT);
    state->has_right_property_predicate =
        adjacency_match_descriptor_bool(
            descriptor, AGE_ADJACENCY_MATCH_DESC_RIGHT_PROPERTY_PREDICATE);
    state->edge_payload_required =
        adjacency_match_descriptor_bool(
            descriptor, AGE_ADJACENCY_MATCH_DESC_EDGE_PAYLOAD_REQUIRED);
    state->payload_attr_mask = (int)adjacency_match_descriptor_int64(
        descriptor, AGE_ADJACENCY_MATCH_DESC_PAYLOAD_ATTR_MASK);
    state->right_label_id = (int32)adjacency_match_descriptor_int64(
        descriptor, AGE_ADJACENCY_MATCH_DESC_RIGHT_LABEL_ID);
    state->endpoint_attno = (AttrNumber)adjacency_match_descriptor_int64(
        descriptor, AGE_ADJACENCY_MATCH_DESC_ENDPOINT_ATTNO);
    state->estimated_endpoint_fanout =
        adjacency_match_descriptor_int64(
            descriptor, AGE_ADJACENCY_MATCH_DESC_ESTIMATED_FANOUT);
    state->estimated_terminal_fanout =
        adjacency_match_descriptor_int64(
            descriptor, AGE_ADJACENCY_MATCH_DESC_ESTIMATED_TERMINAL_FANOUT);
    state->estimated_composite_fanout =
        adjacency_match_descriptor_int64(
            descriptor, AGE_ADJACENCY_MATCH_DESC_ESTIMATED_COMPOSITE_FANOUT);
    state->composite_selectivity_ppm =
        adjacency_match_descriptor_int64(
            descriptor,
            AGE_ADJACENCY_MATCH_DESC_COMPOSITE_SELECTIVITY_PPM);
    state->composite_selectivity_source_kind =
        (AgeGraphPropertySelectivitySource)adjacency_match_descriptor_int64(
            descriptor,
            AGE_ADJACENCY_MATCH_DESC_COMPOSITE_SELECTIVITY_SOURCE_KIND);
    state->value_posting_source_kind =
        (VLESourceValuePostingKind)adjacency_match_descriptor_int64(
            descriptor,
            AGE_ADJACENCY_MATCH_DESC_VALUE_POSTING_SOURCE_KIND);
    state->estimated_terminal_label_groups =
        adjacency_match_descriptor_int64(
            descriptor, AGE_ADJACENCY_MATCH_DESC_ESTIMATED_LABEL_GROUPS);
    state->estimated_main_blocks =
        adjacency_match_descriptor_int64(
            descriptor, AGE_ADJACENCY_MATCH_DESC_ESTIMATED_MAIN_BLOCKS);
    state->fanout_source = adjacency_match_descriptor_text(
        descriptor, AGE_ADJACENCY_MATCH_DESC_FANOUT_SOURCE);
    state->index_source = adjacency_match_descriptor_text(
        descriptor, AGE_ADJACENCY_MATCH_DESC_INDEX_SOURCE);
    state->index_kind_id =
        (AgeAdjacencyMatchIndexKind)adjacency_match_descriptor_int64(
            descriptor, AGE_ADJACENCY_MATCH_DESC_INDEX_KIND_ID);
    state->index_provider = adjacency_match_descriptor_text(
        descriptor, AGE_ADJACENCY_MATCH_DESC_INDEX_PROVIDER);
    state->index_direction_id =
        (AgeAdjacencyMatchIndexDirection)adjacency_match_descriptor_int64(
            descriptor, AGE_ADJACENCY_MATCH_DESC_INDEX_DIRECTION_KIND);
    state->index_property_count = (int32)adjacency_match_descriptor_int64(
        descriptor, AGE_ADJACENCY_MATCH_DESC_INDEX_PROPERTY_COUNT);
    state->index_metadata_backed =
        adjacency_match_descriptor_bool(
            descriptor, AGE_ADJACENCY_MATCH_DESC_INDEX_METADATA_BACKED);
    state->right_property_key = adjacency_match_descriptor_text(
        descriptor, AGE_ADJACENCY_MATCH_DESC_RIGHT_PROPERTY_KEY);
    if (!state->has_right_property_predicate)
    {
        pfree(state->right_property_key);
        state->right_property_key = NULL;
    }
    state->right_property_index_oid =
        (Oid)adjacency_match_descriptor_int64(
            descriptor, AGE_ADJACENCY_MATCH_DESC_RIGHT_PROPERTY_INDEX_OID);
    state->right_property_index_source = adjacency_match_descriptor_text(
        descriptor, AGE_ADJACENCY_MATCH_DESC_RIGHT_PROPERTY_INDEX_SOURCE);
    state->right_property_index_provider = adjacency_match_descriptor_text(
        descriptor, AGE_ADJACENCY_MATCH_DESC_RIGHT_PROPERTY_INDEX_PROVIDER);
    state->right_property_index_type = adjacency_match_descriptor_text(
        descriptor, AGE_ADJACENCY_MATCH_DESC_RIGHT_PROPERTY_INDEX_TYPE);
    state->right_property_index_metadata_backed =
        adjacency_match_descriptor_bool(
            descriptor,
            AGE_ADJACENCY_MATCH_DESC_RIGHT_PROPERTY_INDEX_METADATA_BACKED);
    state->right_property_prefetch_eligible =
        adjacency_match_descriptor_bool(
            descriptor,
            AGE_ADJACENCY_MATCH_DESC_RIGHT_PROPERTY_PREFETCH_ELIGIBLE);
    state->right_property_value_kind =
        (AgeAdjacencyMatchValueKind)adjacency_match_descriptor_int64(
            descriptor, AGE_ADJACENCY_MATCH_DESC_RIGHT_PROPERTY_VALUE_KIND);
    state->terminal_source_strategy =
        (AgeAdjacencyMatchTerminalStrategy)adjacency_match_descriptor_int64(
            descriptor, AGE_ADJACENCY_MATCH_DESC_TERMINAL_SOURCE_STRATEGY);
    state->terminal_prefetch_threshold =
        adjacency_match_descriptor_int64(
            descriptor, AGE_ADJACENCY_MATCH_DESC_TERMINAL_PREFETCH_THRESHOLD);
    state->terminal_prefetch_reason_kind =
        (AgeAdjacencyMatchTerminalPrefetchReason)
        adjacency_match_descriptor_int64(
            descriptor,
            AGE_ADJACENCY_MATCH_DESC_TERMINAL_PREFETCH_REASON_KIND);
    state->join_order_component = adjacency_match_descriptor_text(
        descriptor, AGE_ADJACENCY_MATCH_DESC_JOIN_ORDER_COMPONENT);
    state->join_order_connector_kind =
        (AgeGraphJoinConnectorKind)adjacency_match_descriptor_int64(
            descriptor,
            AGE_ADJACENCY_MATCH_DESC_JOIN_ORDER_CONNECTOR_KIND);
    state->join_order_bound = adjacency_match_descriptor_text(
        descriptor, AGE_ADJACENCY_MATCH_DESC_JOIN_ORDER_BOUND);
    state->join_order_property_kind =
        (AgeGraphJoinOrderPropertyKind)adjacency_match_descriptor_int64(
            descriptor,
            AGE_ADJACENCY_MATCH_DESC_JOIN_ORDER_PROPERTY_KIND);
    state->join_order_source_evidence = adjacency_match_descriptor_text(
        descriptor, AGE_ADJACENCY_MATCH_DESC_JOIN_ORDER_SOURCE_EVIDENCE);
    state->join_order_solved_relids = adjacency_match_descriptor_text(
        descriptor, AGE_ADJACENCY_MATCH_DESC_JOIN_ORDER_SOLVED_RELIDS);
    state->join_order_candidate_count =
        adjacency_match_descriptor_int64(
            descriptor,
            AGE_ADJACENCY_MATCH_DESC_JOIN_ORDER_CANDIDATE_COUNT);
    state->join_order_declared_cover_count =
        adjacency_match_descriptor_int64(
            descriptor,
            AGE_ADJACENCY_MATCH_DESC_JOIN_ORDER_DECLARED_COVER_COUNT);
    state->join_order_cover_match_kind =
        (AgeGraphJoinCoverMatchKind)adjacency_match_descriptor_int64(
            descriptor,
            AGE_ADJACENCY_MATCH_DESC_JOIN_ORDER_COVER_MATCH_KIND);
    state->join_order_next_connector_kind =
        (AgeGraphJoinConnectorKind)adjacency_match_descriptor_int64(
            descriptor,
            AGE_ADJACENCY_MATCH_DESC_JOIN_ORDER_NEXT_CONNECTOR_KIND);
    state->join_order_next_property_kind =
        (AgeGraphJoinOrderPropertyKind)adjacency_match_descriptor_int64(
            descriptor,
            AGE_ADJACENCY_MATCH_DESC_JOIN_ORDER_NEXT_PROPERTY_KIND);
    state->join_order_next_source_evidence = adjacency_match_descriptor_text(
        descriptor,
        AGE_ADJACENCY_MATCH_DESC_JOIN_ORDER_NEXT_SOURCE_EVIDENCE);
    state->graph_join_parallel_safe =
        adjacency_match_descriptor_bool(
            descriptor, AGE_ADJACENCY_MATCH_DESC_PARALLEL_SAFE);
    state->graph_join_parallel_aware =
        adjacency_match_descriptor_bool(
            descriptor, AGE_ADJACENCY_MATCH_DESC_PARALLEL_AWARE);
    state->graph_join_parallel_workers =
        adjacency_match_descriptor_int64(
            descriptor, AGE_ADJACENCY_MATCH_DESC_PARALLEL_WORKERS);
    state->graph_join_gather_cost =
        adjacency_match_descriptor_float8(
            descriptor, AGE_ADJACENCY_MATCH_DESC_PARALLEL_GATHER_COST);
    state->graph_join_order_preserving =
        adjacency_match_descriptor_bool(
            descriptor, AGE_ADJACENCY_MATCH_DESC_ORDER_PRESERVING);
    state->graph_join_shared_state_required =
        adjacency_match_descriptor_bool(
            descriptor, AGE_ADJACENCY_MATCH_DESC_SHARED_STATE_REQUIRED);
    state->payload_slice_mode = AGE_ADJACENCY_PAYLOAD_SLICE_SERIAL_ONLY;
    state->payload_slice_index = 0;
    state->payload_slice_count = 0;
    state->payload_parallel_workers = 0;
    {
        Const *value_const;

        value_const = list_nth_node(
            Const, descriptor,
            AGE_ADJACENCY_MATCH_DESC_RIGHT_PROPERTY_VALUE);
        Assert(value_const->consttype == AGTYPEOID);
        state->right_property_value = value_const->constvalue;
        state->right_property_value_isnull = value_const->constisnull;
    }
}

static Size estimate_age_adjacency_match_dsm(CustomScanState *node,
                                             ParallelContext *pcxt)
{
    (void)node;
    (void)pcxt;

    return MAXALIGN(sizeof(AgeAdjacencyMatchParallelState));
}

static void initialize_age_adjacency_match_dsm(CustomScanState *node,
                                               ParallelContext *pcxt,
                                               void *coordinate)
{
    AgeAdjacencyMatchScanState *state =
        (AgeAdjacencyMatchScanState *)node;
    AgeAdjacencyMatchParallelState *parallel_state = coordinate;

    Assert(parallel_state != NULL);
    initialize_age_adjacency_match_parallel_state(parallel_state, pcxt);
    state->payload_parallel_workers = parallel_state->planned_workers;
}

static void reinitialize_age_adjacency_match_dsm(CustomScanState *node,
                                                 ParallelContext *pcxt,
                                                 void *coordinate)
{
    initialize_age_adjacency_match_dsm(node, pcxt, coordinate);
}

static void initialize_age_adjacency_match_worker(CustomScanState *node,
                                                  shm_toc *toc,
                                                  void *coordinate)
{
    AgeAdjacencyMatchScanState *state =
        (AgeAdjacencyMatchScanState *)node;
    AgeAdjacencyMatchParallelState *parallel_state = coordinate;

    (void)toc;

    if (parallel_state == NULL ||
        parallel_state->magic != AGE_ADJACENCY_MATCH_PARALLEL_MAGIC)
    {
        elog(ERROR, "invalid AGE adjacency match parallel state");
    }

    state->payload_parallel_workers = parallel_state->planned_workers;
    if (state->payload_scan != NULL)
        configure_age_adjacency_match_payload_slice(state);
}

static void initialize_age_adjacency_match_parallel_state(
    AgeAdjacencyMatchParallelState *parallel_state, ParallelContext *pcxt)
{
    int planned_workers;

    Assert(parallel_state != NULL);
    Assert(pcxt != NULL);

    planned_workers = pcxt->nworkers_to_launch > 0 ?
        pcxt->nworkers_to_launch : pcxt->nworkers;
    parallel_state->magic = AGE_ADJACENCY_MATCH_PARALLEL_MAGIC;
    parallel_state->planned_workers = planned_workers > 0 ?
        planned_workers : 0;
}

static void configure_age_adjacency_match_payload_slice(
    AgeAdjacencyMatchScanState *state)
{
    int32 slice_index = 0;
    int32 slice_count = state->payload_parallel_workers;

    Assert(state != NULL);
    Assert(state->payload_scan != NULL);

    if (state->graph_join_parallel_aware &&
        slice_count > 1 &&
        !state->graph_join_shared_state_required &&
        IsParallelWorker())
    {
        slice_index = ParallelWorkerNumber % slice_count;
        state->payload_slice_mode = AGE_ADJACENCY_PAYLOAD_SLICE_WORKER_LOCAL;
    }
    else if (state->graph_join_parallel_safe &&
             !state->graph_join_shared_state_required)
    {
        state->payload_slice_mode =
            AGE_ADJACENCY_PAYLOAD_SLICE_WORKER_LOCAL_READY;
    }
    else if (state->graph_join_shared_state_required)
    {
        state->payload_slice_mode =
            AGE_ADJACENCY_PAYLOAD_SLICE_SHARED_STATE_REQUIRED;
    }
    else
    {
        state->payload_slice_mode = AGE_ADJACENCY_PAYLOAD_SLICE_SERIAL_ONLY;
    }

    state->payload_slice_index = slice_index;
    state->payload_slice_count = slice_count;
    age_adjacency_visible_payload_scan_set_parallel_slice(
        state->payload_scan, slice_index, slice_count);
}

static bool adjacency_match_descriptor_bool(List *descriptor, int index)
{
    Const *value;

    value = list_nth_node(Const, descriptor, index);
    Assert(value->consttype == BOOLOID && !value->constisnull);

    return DatumGetBool(value->constvalue);
}

static int64 adjacency_match_descriptor_int64(List *descriptor, int index)
{
    Const *value;

    value = list_nth_node(Const, descriptor, index);
    Assert(!value->constisnull);

    if (value->consttype == INT4OID)
        return (int64)DatumGetInt32(value->constvalue);
    if (value->consttype == OIDOID)
        return (int64)DatumGetObjectId(value->constvalue);

    Assert(value->consttype == INT8OID);
    return DatumGetInt64(value->constvalue);
}

static double adjacency_match_descriptor_float8(List *descriptor, int index)
{
    Const *value;

    value = list_nth_node(Const, descriptor, index);
    Assert(value->consttype == FLOAT8OID && !value->constisnull);

    return DatumGetFloat8(value->constvalue);
}

static char *adjacency_match_descriptor_text(List *descriptor, int index)
{
    Const *value;

    value = list_nth_node(Const, descriptor, index);
    Assert(value->consttype == TEXTOID && !value->constisnull);

    return TextDatumGetCString(value->constvalue);
}

static char *format_age_adjacency_match_index_descriptor(
    AgeAdjacencyMatchScanState *state)
{
    StringInfoData buf;

    initStringInfo(&buf);
    appendStringInfo(&buf,
                     "direction=%s endpoint=%s fanout=%ld "
                     "terminal-fanout=%ld composite-fanout=%ld",
                     state->outgoing ? "outgoing" : "incoming",
                     age_adjacency_match_attr_name(state->endpoint_attno),
                     (long)state->estimated_endpoint_fanout,
                     (long)state->estimated_terminal_fanout,
                     (long)state->estimated_composite_fanout);
    if (state->composite_selectivity_ppm > 0)
        appendStringInfo(&buf,
                         " composite-selectivity=%.6f "
                         "selectivity-source=%s",
                         (double)state->composite_selectivity_ppm /
                         1000000.0,
                         age_graph_property_selectivity_source_name(
                             state->composite_selectivity_source_kind));
    appendStringInfo(&buf,
                     " label-groups=%ld main-blocks=%ld "
                     "fanout-source=%s source=%s payload=%s",
                     (long)state->estimated_terminal_label_groups,
                     (long)state->estimated_main_blocks,
                     state->fanout_source != NULL ?
                     state->fanout_source : "unknown",
                     state->index_source != NULL ?
                     state->index_source : "unknown",
                     state->edge_payload_required ? "edge-row" : "id-only");

    return buf.data;
}

static char *format_age_adjacency_match_index_metadata(
    AgeAdjacencyMatchScanState *state)
{
    StringInfoData buf;

    initStringInfo(&buf);
    appendStringInfo(&buf,
                     "kind=%s provider=%s direction=%s properties=%d "
                     "metadata=%s",
                     age_adjacency_match_index_kind_name(
                         state->index_kind_id),
                     state->index_provider != NULL ?
                     state->index_provider : "unknown",
                     age_adjacency_match_index_direction_name(
                         state->index_direction_id),
                     state->index_property_count,
                     state->index_metadata_backed ? "yes" : "no");

    return buf.data;
}

static char *format_age_adjacency_match_pruning_descriptor(
    AgeAdjacencyMatchScanState *state)
{
    StringInfoData buf;

    initStringInfo(&buf);
    appendStringInfoString(&buf, "required=");
    append_age_adjacency_match_payload_mask(&buf, state->payload_attr_mask);
    appendStringInfo(&buf,
                     " terminal-source=%s right-prune=label:%s/props:%s "
                     "index-solved=%d residual-count=%d "
                     "residual=edge:%s/right-label:%s/right-props:%s "
                     "edge-var:%s",
                     age_adjacency_match_terminal_strategy_name(
                         state->terminal_source_strategy),
                     label_id_is_valid(state->right_label_id) ?
                     "yes" : "no",
                     age_adjacency_match_terminal_property_prune_mode(state),
                     age_adjacency_match_index_solved_predicate_count(state),
                     age_adjacency_match_residual_predicate_count(state),
                     state->has_edge_property_predicate ? "yes" : "no",
                     state->has_right_label_constraint ? "yes" : "no",
                     age_adjacency_match_terminal_property_residual_mode(
                         state),
                     state->has_edge_variable_projection ? "yes" : "no");

    return buf.data;
}

static char *format_age_adjacency_match_cost_input(
    AgeAdjacencyMatchScanState *state)
{
    int residual_count;
    int index_solved_count;
    int residual_weight_percent;
    int index_credit_percent;

    residual_count = age_adjacency_match_residual_predicate_count(state);
    index_solved_count = age_adjacency_match_index_solved_predicate_count(
        state);
    residual_weight_percent = 100 + (35 * residual_count);
    index_credit_percent = Max(70, 100 - (10 * index_solved_count));

    return psprintf("fanout=%ld terminal-fanout=%ld composite-fanout=%ld "
                    "residual=%d "
                    "label-groups=%ld main-blocks=%ld fanout-source=%s "
                    "value-posting=%s "
                    "index-solved=%d residual-weight=%d%% "
                    "index-credit=%d%% prefetch-threshold=%ld "
                    "reason=%s payload=%s",
                    (long)state->estimated_endpoint_fanout,
                    (long)state->estimated_terminal_fanout,
                    (long)state->estimated_composite_fanout,
                    residual_count,
                    (long)state->estimated_terminal_label_groups,
                    (long)state->estimated_main_blocks,
                    state->fanout_source != NULL ?
                    state->fanout_source : "unknown",
                    age_vle_value_posting_source_name(
                        state->value_posting_source_kind),
                    index_solved_count,
                    residual_weight_percent, index_credit_percent,
                    (long)state->terminal_prefetch_threshold,
                    age_adjacency_match_terminal_prefetch_reason_name(
                        state->terminal_prefetch_reason_kind),
                    state->edge_payload_required ? "edge-row" : "id-only");
}

static char *format_age_adjacency_match_join_order(
    AgeAdjacencyMatchScanState *state)
{
    return psprintf("component=%s solved=%s connector=%s bound=%s property=%s "
                    "rows=%ld fanout=%ld terminal-fanout=%ld "
                    "composite-fanout=%ld source=%s candidates=%ld "
                    "declared-cover=%ld cover=%s "
                    "next=%s/%s/%s",
                    state->join_order_component != NULL ?
                    state->join_order_component : "edge",
                    state->join_order_solved_relids != NULL ?
                    state->join_order_solved_relids : "(b)",
                    age_graph_join_connector_name(
                        state->join_order_connector_kind),
                    state->join_order_bound != NULL ?
                    state->join_order_bound : "unknown",
                    age_graph_join_order_property_name(
                        state->join_order_property_kind),
                    (long)state->css.ss.ps.plan->plan_rows,
                    (long)state->estimated_endpoint_fanout,
                    (long)state->estimated_terminal_fanout,
                    (long)state->estimated_composite_fanout,
                    state->join_order_source_evidence != NULL ?
                    state->join_order_source_evidence :
                    (state->fanout_source != NULL ?
                     state->fanout_source : "unknown"),
                    (long)Max(state->join_order_candidate_count, 1),
                    (long)state->join_order_declared_cover_count,
                    age_graph_join_cover_match_kind_name(
                        state->join_order_cover_match_kind),
                    state->join_order_next_connector_kind !=
                    AGE_GRAPH_JOIN_CONNECTOR_UNKNOWN ?
                    age_graph_join_connector_name(
                        state->join_order_next_connector_kind) : "none",
                    state->join_order_next_property_kind !=
                    AGE_GRAPH_JOIN_ORDER_UNKNOWN ?
                    age_graph_join_order_property_name(
                        state->join_order_next_property_kind) : "none",
                    state->join_order_next_source_evidence != NULL ?
                    state->join_order_next_source_evidence : "none");
}

static char *format_age_adjacency_match_composite_source(
    AgeAdjacencyMatchScanState *state)
{
    return psprintf("strategy=%s label=%s property=%s value=%s "
                    "candidate-fanout=%ld composite-fanout=%ld "
                    "value-posting=%s planned=%s threshold=%ld reason=%s",
                    age_adjacency_match_terminal_strategy_name(
                        state->terminal_source_strategy),
                    label_id_is_valid(state->right_label_id) ?
                    "yes" : "no",
                    state->right_property_prefetch_eligible ?
                    "source" :
                    (state->has_right_property_predicate ?
                     "recheck" : "none"),
                    age_adjacency_match_value_kind_name(
                        state->right_property_value_kind),
                    (long)state->estimated_terminal_fanout,
                    (long)state->estimated_composite_fanout,
                    age_vle_value_posting_source_name(
                        state->value_posting_source_kind),
                    age_adjacency_match_terminal_planned_prefetch(state),
                    (long)state->terminal_prefetch_threshold,
                    age_adjacency_match_terminal_prefetch_reason_name(
                        state->terminal_prefetch_reason_kind));
}

static char *format_age_adjacency_match_composite_policy(
    AgeAdjacencyMatchScanState *state, bool analyze)
{
    if (state == NULL || !state->has_right_property_predicate)
        return pstrdup("none");

    if (analyze)
    {
        return psprintf("class=%s recommendation=%s runtime-class=%s "
                        "runtime-recommendation=%s class-match=%s",
                        age_adjacency_match_terminal_policy_class(state),
                        age_adjacency_match_terminal_policy_recommendation(
                            state),
                        age_adjacency_match_terminal_runtime_class(state),
                        age_adjacency_match_terminal_runtime_recommendation(
                            state),
                        age_adjacency_match_terminal_class_matches(state) ?
                        "true" : "false");
    }

    return psprintf("class=%s recommendation=%s",
                    age_adjacency_match_terminal_policy_class(state),
                    age_adjacency_match_terminal_policy_recommendation(state));
}

static char *format_age_adjacency_match_terminal_property(
    AgeAdjacencyMatchScanState *state)
{
    AgeAdjacencyMatchTerminalPropertyMode mode;

    if (!state->has_right_property_predicate)
        return pstrdup("none");

    mode = age_adjacency_match_terminal_property_mode_id(
        state->terminal_property_lookup);
    if (!state->scanned && state->right_property_prefetch_eligible)
        mode = AGE_ADJACENCY_TERMINAL_PROPERTY_DEFERRED_PREFETCH;

    return psprintf("key=%s index=%s provider=%s domain=%s metadata=%s value=%s "
                    "prefetch=%s precheck=%s mode=%s",
                    state->right_property_key != NULL ?
                    state->right_property_key : "unknown",
                    state->right_property_index_metadata_backed ?
                    state->right_property_index_source : "none",
                    state->right_property_index_metadata_backed ?
                    state->right_property_index_provider : "none",
                    state->right_property_index_metadata_backed ?
                    state->right_property_index_type : "none",
                    state->right_property_index_metadata_backed ?
                    "yes" : "no",
                    age_adjacency_match_value_kind_name(
                        state->right_property_value_kind),
                    state->right_property_prefetch_eligible ?
                    "eligible" : "ineligible",
                    age_adjacency_match_terminal_property_active(
                        state->terminal_property_lookup) ? "yes" : "no",
                    age_adjacency_match_terminal_property_mode_name(mode));
}

static char *format_age_adjacency_match_terminal_runtime(
    AgeAdjacencyMatchScanState *state)
{
    if (!state->has_right_property_predicate)
        return pstrdup("none");

    return psprintf("outcome=%s action=%s lifecycle-match=%s "
                    "value-identity=%s "
                    "prefetch-matches=%lld payload-candidates=%lld "
                    "terminal-filtered=%lld emitted=%lld cache-hits=%lld "
                    "index-lookups=%lld",
                    age_adjacency_match_terminal_runtime_outcome(state),
                    age_adjacency_match_terminal_runtime_action(state),
                    age_adjacency_match_terminal_lifecycle_matches(state) ?
                    "true" : "false",
                    age_adjacency_match_terminal_value_identity(state),
                    (long long)
                    age_adjacency_match_terminal_property_prefetched_matches(
                        state->terminal_property_lookup),
                    (long long)state->payload_candidates,
                    (long long)state->terminal_filtered,
                    (long long)state->rows_emitted,
                    (long long)
                    age_adjacency_match_terminal_property_cache_hits(
                        state->terminal_property_lookup),
                    (long long)
                    age_adjacency_match_terminal_property_index_lookups(
                        state->terminal_property_lookup));
}

static const char *
age_adjacency_match_terminal_runtime_outcome(
    AgeAdjacencyMatchScanState *state)
{
    return age_adjacency_match_terminal_runtime_outcome_name(
        age_adjacency_match_terminal_runtime_outcome_id(state));
}

static AgeAdjacencyTerminalPrunePlan
age_adjacency_match_terminal_runtime_outcome_id(
    AgeAdjacencyMatchScanState *state)
{
    if (state == NULL || !state->has_right_property_predicate)
        return AGE_ADJACENCY_TERMINAL_PRUNE_NONE;

    if (age_adjacency_match_terminal_property_mode_id(
            state->terminal_property_lookup) ==
        AGE_ADJACENCY_TERMINAL_PROPERTY_SOURCE_PREFETCH)
        return AGE_ADJACENCY_TERMINAL_PRUNE_SOURCE_PREFETCH;
    if (age_adjacency_match_terminal_property_prefetch_skipped_small(
            state->terminal_property_lookup) > 0)
    {
        return AGE_ADJACENCY_TERMINAL_PRUNE_ID_CACHE_SMALL;
    }
    if (age_adjacency_match_terminal_property_mode_id(
            state->terminal_property_lookup) ==
        AGE_ADJACENCY_TERMINAL_PROPERTY_ID_CACHE ||
        age_adjacency_match_terminal_property_mode_id(
            state->terminal_property_lookup) ==
        AGE_ADJACENCY_TERMINAL_PROPERTY_ID_BTREE)
    {
        return AGE_ADJACENCY_TERMINAL_PRUNE_VERTEX_CACHE;
    }
    if (age_adjacency_match_terminal_property_mode_id(
            state->terminal_property_lookup) ==
        AGE_ADJACENCY_TERMINAL_PROPERTY_DEFERRED_PREFETCH)
        return AGE_ADJACENCY_TERMINAL_PRUNE_DEFERRED;

    return AGE_ADJACENCY_TERMINAL_PRUNE_NONE;
}

static const char *
age_adjacency_match_terminal_runtime_action(
    AgeAdjacencyMatchScanState *state)
{
    switch (age_adjacency_match_terminal_runtime_outcome_id(state))
    {
        case AGE_ADJACENCY_TERMINAL_PRUNE_SOURCE_PREFETCH:
            return "use-property-source";
        case AGE_ADJACENCY_TERMINAL_PRUNE_ID_CACHE_SMALL:
            return "keep-id-cache";
        case AGE_ADJACENCY_TERMINAL_PRUNE_VERTEX_CACHE:
            return "verify-by-id";
        case AGE_ADJACENCY_TERMINAL_PRUNE_DEFERRED:
            return "await-runtime-value";
        case AGE_ADJACENCY_TERMINAL_PRUNE_NONE:
            break;
    }

    return "none";
}

static const char *
age_adjacency_match_terminal_policy_class(
    AgeAdjacencyMatchScanState *state)
{
    return age_adjacency_match_terminal_policy_class_name(
        age_adjacency_match_terminal_policy_class_id(state));
}

static AgeAdjacencyTerminalPolicyClass
age_adjacency_match_terminal_policy_class_id(
    AgeAdjacencyMatchScanState *state)
{
    if (state == NULL || !state->has_right_property_predicate)
        return AGE_ADJACENCY_TERMINAL_CLASS_NONE;

    switch (age_adjacency_match_terminal_planned_prefetch_id(state))
    {
        case AGE_ADJACENCY_TERMINAL_PRUNE_SOURCE_PREFETCH:
            return AGE_ADJACENCY_TERMINAL_CLASS_COMPOSITE_PREFILTER;
        case AGE_ADJACENCY_TERMINAL_PRUNE_ID_CACHE_SMALL:
            return AGE_ADJACENCY_TERMINAL_CLASS_COMPOSITE_ID_CACHE;
        case AGE_ADJACENCY_TERMINAL_PRUNE_NONE:
        case AGE_ADJACENCY_TERMINAL_PRUNE_VERTEX_CACHE:
        case AGE_ADJACENCY_TERMINAL_PRUNE_DEFERRED:
            break;
    }
    if (state->right_property_prefetch_eligible)
        return AGE_ADJACENCY_TERMINAL_CLASS_COMPOSITE_DEFERRED;

    return AGE_ADJACENCY_TERMINAL_CLASS_COMPOSITE_RECHECK;
}

static const char *
age_adjacency_match_terminal_policy_recommendation(
    AgeAdjacencyMatchScanState *state)
{
    if (state == NULL || !state->has_right_property_predicate)
        return "none";

    switch (age_adjacency_match_terminal_planned_prefetch_id(state))
    {
        case AGE_ADJACENCY_TERMINAL_PRUNE_SOURCE_PREFETCH:
            return "keep-property-prefilter";
        case AGE_ADJACENCY_TERMINAL_PRUNE_ID_CACHE_SMALL:
            return "keep-id-cache";
        case AGE_ADJACENCY_TERMINAL_PRUNE_NONE:
        case AGE_ADJACENCY_TERMINAL_PRUNE_VERTEX_CACHE:
        case AGE_ADJACENCY_TERMINAL_PRUNE_DEFERRED:
            break;
    }
    if (state->right_property_prefetch_eligible)
        return "await-runtime-value";

    return "keep-recheck";
}

static const char *
age_adjacency_match_terminal_runtime_class(
    AgeAdjacencyMatchScanState *state)
{
    return age_adjacency_match_terminal_policy_class_name(
        age_adjacency_match_terminal_runtime_class_id(state));
}

static AgeAdjacencyTerminalPolicyClass
age_adjacency_match_terminal_runtime_class_id(
    AgeAdjacencyMatchScanState *state)
{
    if (state == NULL || !state->has_right_property_predicate)
        return AGE_ADJACENCY_TERMINAL_CLASS_NONE;

    switch (age_adjacency_match_terminal_runtime_outcome_id(state))
    {
        case AGE_ADJACENCY_TERMINAL_PRUNE_SOURCE_PREFETCH:
            return AGE_ADJACENCY_TERMINAL_CLASS_COMPOSITE_PREFILTER;
        case AGE_ADJACENCY_TERMINAL_PRUNE_ID_CACHE_SMALL:
        case AGE_ADJACENCY_TERMINAL_PRUNE_VERTEX_CACHE:
            return AGE_ADJACENCY_TERMINAL_CLASS_COMPOSITE_ID_CACHE;
        case AGE_ADJACENCY_TERMINAL_PRUNE_DEFERRED:
            return AGE_ADJACENCY_TERMINAL_CLASS_COMPOSITE_DEFERRED;
        case AGE_ADJACENCY_TERMINAL_PRUNE_NONE:
            break;
    }
    if (!state->right_property_prefetch_eligible)
        return AGE_ADJACENCY_TERMINAL_CLASS_COMPOSITE_RECHECK;

    return AGE_ADJACENCY_TERMINAL_CLASS_NONE;
}

static const char *
age_adjacency_match_terminal_runtime_recommendation(
    AgeAdjacencyMatchScanState *state)
{
    if (state == NULL || !state->has_right_property_predicate)
        return "none";

    switch (age_adjacency_match_terminal_runtime_outcome_id(state))
    {
        case AGE_ADJACENCY_TERMINAL_PRUNE_SOURCE_PREFETCH:
            return "keep-property-prefilter";
        case AGE_ADJACENCY_TERMINAL_PRUNE_ID_CACHE_SMALL:
            return "keep-id-cache";
        case AGE_ADJACENCY_TERMINAL_PRUNE_VERTEX_CACHE:
            return "verify-by-id";
        case AGE_ADJACENCY_TERMINAL_PRUNE_DEFERRED:
            return "await-runtime-value";
        case AGE_ADJACENCY_TERMINAL_PRUNE_NONE:
            break;
    }
    if (!state->right_property_prefetch_eligible)
        return "keep-recheck";

    return "none";
}

static bool
age_adjacency_match_terminal_class_matches(
    AgeAdjacencyMatchScanState *state)
{
    return age_adjacency_match_terminal_policy_class_id(state) ==
           age_adjacency_match_terminal_runtime_class_id(state);
}

static bool
age_adjacency_match_terminal_lifecycle_matches(
    AgeAdjacencyMatchScanState *state)
{
    AgeAdjacencyTerminalPrunePlan planned;
    AgeAdjacencyTerminalPrunePlan outcome;

    planned = age_adjacency_match_terminal_planned_prefetch_id(state);
    outcome = age_adjacency_match_terminal_runtime_outcome_id(state);
    if (planned == outcome)
        return true;

    return false;
}

static const char *
age_adjacency_match_terminal_value_identity(
    AgeAdjacencyMatchScanState *state)
{
    if (state == NULL || !state->has_right_property_predicate)
        return "none";
    if (age_adjacency_match_terminal_property_filter_id(
            state->terminal_property_lookup) == 0)
    {
        return "none";
    }

    return "present";
}

static char *format_age_adjacency_match_terminal_prefetch(
    AgeAdjacencyMatchScanState *state)
{
    if (!state->has_right_property_predicate)
        return pstrdup("none");

    return psprintf("planned=%s run-count=%lld candidate-count=%lld "
                    "threshold=%lld reason=%s skipped-small=%lld",
                    age_adjacency_match_terminal_planned_prefetch(state),
                    (long long)
                    age_adjacency_match_terminal_property_prefetch_run_count(
                        state->terminal_property_lookup),
                    (long long)
                    age_adjacency_match_terminal_property_prefetch_candidate_count(
                        state->terminal_property_lookup),
                    (long long)
                    age_adjacency_match_terminal_property_prefetch_threshold(
                        state->terminal_property_lookup),
                    age_adjacency_match_terminal_prefetch_reason_name(
                        state->terminal_prefetch_reason_kind),
                    (long long)
                    age_adjacency_match_terminal_property_prefetch_skipped_small(
                        state->terminal_property_lookup));
}

static const char *
age_adjacency_match_terminal_planned_prefetch(
    AgeAdjacencyMatchScanState *state)
{
    return age_adjacency_match_terminal_prune_plan_name(
        age_adjacency_match_terminal_planned_prefetch_id(state));
}

static AgeAdjacencyTerminalPrunePlan
age_adjacency_match_terminal_planned_prefetch_id(
    AgeAdjacencyMatchScanState *state)
{
    if (state == NULL || !state->right_property_prefetch_eligible ||
        state->terminal_prefetch_threshold <= 0)
    {
        return AGE_ADJACENCY_TERMINAL_PRUNE_NONE;
    }

    if (state->estimated_terminal_fanout < state->terminal_prefetch_threshold)
        return AGE_ADJACENCY_TERMINAL_PRUNE_ID_CACHE_SMALL;

    return AGE_ADJACENCY_TERMINAL_PRUNE_SOURCE_PREFETCH;
}

static void append_age_adjacency_match_payload_mask(StringInfo buf,
                                                    int payload_attr_mask)
{
    AttrNumber attno;
    bool first = true;

    if (payload_attr_mask == 0)
    {
        appendStringInfoString(buf, "all");
        return;
    }

    for (attno = Anum_ag_label_edge_table_id;
         attno <= Anum_ag_label_edge_table_properties;
         attno++)
    {
        if ((payload_attr_mask & (1 << (attno - 1))) == 0)
            continue;

        if (!first)
            appendStringInfoChar(buf, ',');
        appendStringInfoString(buf, age_adjacency_match_attr_name(attno));
        first = false;
    }
}

static char *format_age_adjacency_match_payload_columns(
    AgeAdjacencyMatchScanState *state)
{
    StringInfoData buf;
    int i;

    initStringInfo(&buf);
    appendStringInfo(&buf, "mode=%s columns=",
                     state->fetch_properties ? "edge-row" : "id-only");

    if (state->scan_nattrs == 0)
    {
        appendStringInfoString(&buf, "all");
        return buf.data;
    }

    for (i = 0; i < state->scan_nattrs; i++)
    {
        if (i > 0)
            appendStringInfoChar(&buf, ',');
        appendStringInfoString(&buf,
                               age_adjacency_match_attr_name(
                                   state->scan_attnos[i]));
    }

    return buf.data;
}

static char *format_age_adjacency_match_payload_runtime(
    AgeAdjacencyMatchScanState *state)
{
    StringInfoData buf;
    int64 label_filtered;
    int64 directory_label_filtered;
    int64 property_filtered;
    int64 cache_filtered;
    int64 cache_label_filtered;
    int64 cache_property_filtered;
    int64 range_filtered;
    int64 sorted_filtered;
    int64 block_filtered;
    int64 block_value_filtered;
    int64 block_value_posting_filtered;
    int64 block_compressed_filtered;
    int64 block_posting_filtered;
    int64 directory_filtered;
    int64 directory_range_filtered;
    int64 directory_exact_filtered;
    int64 directory_label_bloom_filtered;
    int64 directory_compressed_filtered;
    int64 directory_wide_bloom_filtered;
    int64 directory_value_filtered;
    int64 directory_value_posting_filtered;
    int64 composite_requests;
    int64 composite_block_filtered;
    int64 composite_directory_filtered;
    int64 composite_directory_estimated;

    label_filtered = age_adjacency_visible_payload_scan_label_filtered(
        state->payload_scan);
    directory_label_filtered =
        age_adjacency_visible_payload_scan_directory_label_filtered(
            state->payload_scan);
    property_filtered = age_adjacency_visible_payload_scan_property_filtered(
        state->payload_scan);
    cache_filtered = age_adjacency_visible_payload_scan_cache_filtered(
        state->payload_scan);
    cache_label_filtered =
        age_adjacency_visible_payload_scan_cache_label_filtered(
            state->payload_scan);
    cache_property_filtered =
        age_adjacency_visible_payload_scan_cache_property_filtered(
            state->payload_scan);
    range_filtered =
        age_adjacency_visible_payload_scan_vertex_set_range_filtered(
            state->payload_scan);
    sorted_filtered =
        age_adjacency_visible_payload_scan_vertex_set_sorted_filtered(
            state->payload_scan);
    block_filtered =
        age_adjacency_visible_payload_scan_vertex_set_block_filtered(
            state->payload_scan);
    block_value_filtered =
        age_adjacency_visible_payload_scan_vertex_set_block_value_filtered(
            state->payload_scan);
    block_value_posting_filtered =
        age_adjacency_visible_payload_scan_vertex_set_block_value_posting_filtered(
            state->payload_scan);
    block_compressed_filtered =
        age_adjacency_visible_payload_scan_vertex_set_block_compressed_filtered(
            state->payload_scan);
    block_posting_filtered =
        age_adjacency_visible_payload_scan_vertex_set_block_posting_filtered(
            state->payload_scan);
    directory_filtered =
        age_adjacency_visible_payload_scan_vertex_set_directory_filtered(
            state->payload_scan);
    directory_range_filtered =
        age_adjacency_visible_payload_scan_vertex_set_directory_range_filtered(
            state->payload_scan);
    directory_exact_filtered =
        age_adjacency_visible_payload_scan_vertex_set_directory_exact_filtered(
            state->payload_scan);
    directory_label_bloom_filtered =
        age_adjacency_visible_payload_scan_vertex_set_directory_label_bloom_filtered(
            state->payload_scan);
    directory_compressed_filtered =
        age_adjacency_visible_payload_scan_vertex_set_directory_compressed_filtered(
            state->payload_scan);
    directory_wide_bloom_filtered =
        age_adjacency_visible_payload_scan_vertex_set_directory_wide_bloom_filtered(
            state->payload_scan);
    directory_value_filtered =
        age_adjacency_visible_payload_scan_vertex_set_directory_value_filtered(
            state->payload_scan);
    directory_value_posting_filtered =
        age_adjacency_visible_payload_scan_vertex_set_directory_value_posting_filtered(
            state->payload_scan);
    composite_requests =
        age_adjacency_visible_payload_scan_composite_requests(
            state->payload_scan);
    composite_block_filtered =
        age_adjacency_visible_payload_scan_composite_block_filtered(
            state->payload_scan);
    composite_directory_filtered =
        age_adjacency_visible_payload_scan_composite_directory_filtered(
            state->payload_scan);
    composite_directory_estimated =
        age_adjacency_visible_payload_scan_composite_directory_estimated(
            state->payload_scan);

    initStringInfo(&buf);
    appendStringInfo(&buf,
                     "visible=%lld label-filtered=%lld "
                     "directory-label=%lld "
                     "property-filtered=%lld cache-filtered=%lld "
                     "cache-label=%lld cache-property=%lld",
                     (long long)state->payload_candidates,
                     (long long)label_filtered,
                     (long long)directory_label_filtered,
                     (long long)property_filtered,
                     (long long)cache_filtered,
                     (long long)cache_label_filtered,
                     (long long)cache_property_filtered);
    if (range_filtered > 0)
        appendStringInfo(&buf, " set-range-filter=%lld",
                         (long long)range_filtered);
    if (sorted_filtered > 0)
        appendStringInfo(&buf, " set-sorted-filter=%lld",
                         (long long)sorted_filtered);
    if (block_filtered > 0)
        appendStringInfo(&buf, " set-block-filter=%lld",
                         (long long)block_filtered);
    if (block_value_filtered > 0)
        appendStringInfo(&buf, "/value-summary:%lld",
                         (long long)block_value_filtered);
    if (block_value_posting_filtered > 0)
        appendStringInfo(&buf, "/value-posting:%lld",
                         (long long)block_value_posting_filtered);
    if (block_compressed_filtered > 0)
        appendStringInfo(&buf, "/compressed:%lld",
                         (long long)block_compressed_filtered);
    if (block_posting_filtered > 0)
        appendStringInfo(&buf, "/posting:%lld",
                         (long long)block_posting_filtered);
    if (directory_filtered > 0)
        appendStringInfo(&buf, " set-directory-filter=%lld",
                         (long long)directory_filtered);
    if (directory_range_filtered > 0)
        appendStringInfo(&buf, "/range:%lld",
                         (long long)directory_range_filtered);
    if (directory_exact_filtered > 0)
        appendStringInfo(&buf, "/exact:%lld",
                         (long long)directory_exact_filtered);
    if (directory_label_bloom_filtered > 0)
        appendStringInfo(&buf, "/label-bloom:%lld",
                         (long long)directory_label_bloom_filtered);
    if (directory_compressed_filtered > 0)
        appendStringInfo(&buf, "/compressed:%lld",
                         (long long)directory_compressed_filtered);
    if (directory_value_filtered > 0)
        appendStringInfo(&buf, "/value-summary:%lld",
                         (long long)directory_value_filtered);
    if (directory_value_posting_filtered > 0)
        appendStringInfo(&buf, "/value-posting:%lld",
                         (long long)directory_value_posting_filtered);
    if (directory_wide_bloom_filtered > 0)
        appendStringInfo(&buf, "/wide-bloom:%lld",
                         (long long)directory_wide_bloom_filtered);
    if (composite_requests > 0)
    {
        appendStringInfo(&buf, " composite=request:%lld",
                         (long long)composite_requests);
        if (composite_block_filtered > 0)
            appendStringInfo(&buf, "/block-filter:%lld",
                             (long long)composite_block_filtered);
        if (composite_directory_filtered > 0)
            appendStringInfo(&buf, "/dir-filter:%lld",
                             (long long)composite_directory_filtered);
        if (composite_directory_estimated > 0)
            appendStringInfo(&buf, "/dir-estimate:%lld",
                             (long long)composite_directory_estimated);
    }
    appendStringInfo(&buf, " emitted=%lld",
                     (long long)state->rows_emitted);

    return buf.data;
}

static const char *age_adjacency_match_attr_name(AttrNumber attno)
{
    switch (attno)
    {
        case Anum_ag_label_edge_table_id:
            return "id";
        case Anum_ag_label_edge_table_start_id:
            return "start_id";
        case Anum_ag_label_edge_table_end_id:
            return "end_id";
        case Anum_ag_label_edge_table_properties:
            return "properties";
        default:
            return "unknown";
    }
}

static const char *age_adjacency_match_terminal_property_prune_mode(
    AgeAdjacencyMatchScanState *state)
{
    return age_adjacency_match_terminal_prune_plan_name(
        age_adjacency_match_terminal_property_prune_mode_id(state));
}

static AgeAdjacencyTerminalPrunePlan
age_adjacency_match_terminal_property_prune_mode_id(
    AgeAdjacencyMatchScanState *state)
{
    if (!state->has_right_property_predicate)
        return AGE_ADJACENCY_TERMINAL_PRUNE_NONE;
    if (state->right_property_prefetch_eligible)
        return AGE_ADJACENCY_TERMINAL_PRUNE_SOURCE_PREFETCH;
    if (age_adjacency_match_terminal_property_prefilter_active(
            state->terminal_property_lookup))
        return AGE_ADJACENCY_TERMINAL_PRUNE_SOURCE_PREFETCH;
    if (age_adjacency_match_terminal_property_active(
            state->terminal_property_lookup))
        return AGE_ADJACENCY_TERMINAL_PRUNE_VERTEX_CACHE;

    return AGE_ADJACENCY_TERMINAL_PRUNE_DEFERRED;
}

static const char *age_adjacency_match_terminal_prune_plan_name(
    AgeAdjacencyTerminalPrunePlan plan)
{
    switch (plan)
    {
        case AGE_ADJACENCY_TERMINAL_PRUNE_SOURCE_PREFETCH:
            return "source-prefetch";
        case AGE_ADJACENCY_TERMINAL_PRUNE_ID_CACHE_SMALL:
            return "id-cache-small";
        case AGE_ADJACENCY_TERMINAL_PRUNE_VERTEX_CACHE:
            return "vertex-cache";
        case AGE_ADJACENCY_TERMINAL_PRUNE_DEFERRED:
            return "deferred";
        case AGE_ADJACENCY_TERMINAL_PRUNE_NONE:
            break;
    }

    return "none";
}

const char *age_adjacency_match_index_kind_name(
    AgeAdjacencyMatchIndexKind kind)
{
    switch (kind)
    {
        case AGE_ADJACENCY_MATCH_INDEX_ADJACENCY:
            return "ADJACENCY";
        case AGE_ADJACENCY_MATCH_INDEX_PROPERTY:
            return "PROPERTY";
        case AGE_ADJACENCY_MATCH_INDEX_UNKNOWN:
            break;
    }

    return "unknown";
}

const char *age_adjacency_match_index_direction_name(
    AgeAdjacencyMatchIndexDirection direction)
{
    switch (direction)
    {
        case AGE_ADJACENCY_MATCH_INDEX_DIRECTION_OUT:
            return "out";
        case AGE_ADJACENCY_MATCH_INDEX_DIRECTION_IN:
            return "in";
        case AGE_ADJACENCY_MATCH_INDEX_DIRECTION_NONE:
            break;
    }

    return "unknown";
}

static const char *age_adjacency_match_terminal_runtime_outcome_name(
    AgeAdjacencyTerminalPrunePlan outcome)
{
    if (outcome == AGE_ADJACENCY_TERMINAL_PRUNE_VERTEX_CACHE)
        return "id-cache";

    return age_adjacency_match_terminal_prune_plan_name(outcome);
}

static const char *age_adjacency_match_terminal_policy_class_name(
    AgeAdjacencyTerminalPolicyClass policy_class)
{
    switch (policy_class)
    {
        case AGE_ADJACENCY_TERMINAL_CLASS_COMPOSITE_PREFILTER:
            return "adjacency-composite-prefilter";
        case AGE_ADJACENCY_TERMINAL_CLASS_COMPOSITE_ID_CACHE:
            return "adjacency-composite-id-cache";
        case AGE_ADJACENCY_TERMINAL_CLASS_COMPOSITE_DEFERRED:
            return "adjacency-composite-deferred";
        case AGE_ADJACENCY_TERMINAL_CLASS_COMPOSITE_RECHECK:
            return "adjacency-composite-recheck";
        case AGE_ADJACENCY_TERMINAL_CLASS_NONE:
            break;
    }

    return "none";
}

static const char *age_adjacency_match_payload_slice_mode_name(
    AgeAdjacencyPayloadSliceMode mode)
{
    switch (mode)
    {
        case AGE_ADJACENCY_PAYLOAD_SLICE_WORKER_LOCAL:
            return "worker-local";
        case AGE_ADJACENCY_PAYLOAD_SLICE_WORKER_LOCAL_READY:
            return "worker-local-ready";
        case AGE_ADJACENCY_PAYLOAD_SLICE_SHARED_STATE_REQUIRED:
            return "shared-state-required";
        case AGE_ADJACENCY_PAYLOAD_SLICE_SERIAL_ONLY:
            break;
    }

    return "serial-only";
}

const char *age_adjacency_match_terminal_prefetch_reason_name(
    AgeAdjacencyMatchTerminalPrefetchReason reason)
{
    switch (reason)
    {
        case AGE_ADJACENCY_MATCH_PREFETCH_REASON_NOT_INDEXABLE:
            return "not-indexable";
        case AGE_ADJACENCY_MATCH_PREFETCH_REASON_EDGE_PAYLOAD_REQUIRED:
            return "edge-payload-required";
        case AGE_ADJACENCY_MATCH_PREFETCH_REASON_LARGE_TERMINAL_FANOUT:
            return "large-terminal-fanout";
        case AGE_ADJACENCY_MATCH_PREFETCH_REASON_SMALL_TERMINAL_FANOUT:
            return "small-terminal-fanout";
        case AGE_ADJACENCY_MATCH_PREFETCH_REASON_NONE:
            break;
    }

    return "none";
}

static const char *age_adjacency_match_value_kind_name(
    AgeAdjacencyMatchValueKind kind)
{
    switch (kind)
    {
        case AGE_ADJACENCY_MATCH_VALUE_CONST:
            return "const";
        case AGE_ADJACENCY_MATCH_VALUE_RUNTIME_SLOT:
            return "runtime-slot";
        case AGE_ADJACENCY_MATCH_VALUE_NONE:
            break;
    }

    return "none";
}

static const char *age_adjacency_match_terminal_strategy_name(
    AgeAdjacencyMatchTerminalStrategy strategy)
{
    switch (strategy)
    {
        case AGE_ADJACENCY_MATCH_TERMINAL_STRATEGY_LABEL_PROPERTY:
            return "label-block+property-source";
        case AGE_ADJACENCY_MATCH_TERMINAL_STRATEGY_LABEL_BLOCK:
            return "label-block";
        case AGE_ADJACENCY_MATCH_TERMINAL_STRATEGY_PROPERTY_SOURCE:
            return "property-source";
        case AGE_ADJACENCY_MATCH_TERMINAL_STRATEGY_PROPERTY_RECHECK:
            return "property-recheck";
        case AGE_ADJACENCY_MATCH_TERMINAL_STRATEGY_NONE:
            break;
    }

    return "none";
}

static int
age_adjacency_match_residual_predicate_count(
    AgeAdjacencyMatchScanState *state)
{
    int count = 0;

    if (state->has_edge_property_predicate)
        count++;
    if (state->has_right_label_constraint)
        count++;
    if (state->has_right_property_predicate)
        count++;

    return count;
}

static int
age_adjacency_match_index_solved_predicate_count(
    AgeAdjacencyMatchScanState *state)
{
    int count = 0;

    if (label_id_is_valid(state->right_label_id))
        count++;
    if (age_adjacency_match_terminal_property_prune_mode_id(state) ==
        AGE_ADJACENCY_TERMINAL_PRUNE_SOURCE_PREFETCH)
    {
        count++;
    }

    return count;
}

static const char *age_adjacency_match_terminal_property_residual_mode(
    AgeAdjacencyMatchScanState *state)
{
    if (!state->has_right_property_predicate)
        return "no";
    if (state->right_property_prefetch_eligible)
        return "join-verify";
    if (age_adjacency_match_terminal_property_prefilter_active(
            state->terminal_property_lookup))
        return "join-verify";
    if (age_adjacency_match_terminal_property_active(
            state->terminal_property_lookup))
        return "prechecked";

    return "yes";
}

static void store_age_adjacency_match_tuple(
    AgeAdjacencyMatchScanState *state,
    const AgeAdjacencyPayload *payload,
    AgeAdjacencyMatchTuple *tuple)
{
    MemoryContext oldcontext;

    oldcontext = MemoryContextSwitchTo(state->scan_context);
    memset(tuple, 0, sizeof(*tuple));

    tuple->edge_id = payload->edge_id;
    if (state->outgoing)
    {
        tuple->start_id = state->current_key;
        tuple->end_id = payload->next_vertex_id;
    }
    else
    {
        tuple->start_id = payload->next_vertex_id;
        tuple->end_id = state->current_key;
    }
    tuple->properties_isnull = payload->properties_isnull;
    if (!payload->properties_isnull)
        tuple->properties = datumCopy(payload->properties, false, -1);
    MemoryContextSwitchTo(oldcontext);
}

static AgeAdjacencyMatchTerminalPropertyRequest
make_age_adjacency_match_terminal_property_request(
    AgeAdjacencyMatchScanState *state)
{
    AgeAdjacencyMatchTerminalPropertyRequest request;

    memset(&request, 0, sizeof(request));
    request.graph_oid = state->graph_oid;
    request.right_label_id = state->right_label_id;
    request.has_property_predicate = state->has_right_property_predicate;
    request.metadata_backed = state->right_property_index_metadata_backed;
    request.property_index_oid = state->right_property_index_oid;
    request.property_key = state->right_property_key;
    request.property_value = state->right_property_value;
    request.property_value_isnull = state->right_property_value_isnull;

    return request;
}
