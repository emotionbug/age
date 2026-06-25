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
#include "catalog/ag_label.h"
#include "commands/explain_format.h"
#include "commands/explain_state.h"
#include "executor/cypher_wcoj_join.h"
#include "executor/executor.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "nodes/pg_list.h"
#include "utils/ag_guc.h"
#include "utils/datum.h"
#include "utils/graphid.h"
#include "utils/memutils.h"

#define AGE_WCOJ_PROGRESSIVE_DENSE_SAMPLE 1024
#define AGE_WCOJ_PROGRESSIVE_DENSE_NUMERATOR 3
#define AGE_WCOJ_PROGRESSIVE_DENSE_DENOMINATOR 4
#define AGE_WCOJ_SURVIVOR_BLOCK_MIN 16
#define AGE_WCOJ_SURVIVOR_BLOCK_MAX 4096
#define AGE_WCOJ_SURVIVOR_BLOCK_BUDGET_DIVISOR 4

/*
 * One duplicate group per input stream.  Rows are copied because every child
 * PlanState reuses its result slot on the next ExecProcNode() call.
 */
typedef struct AgeWCOJTupleGroup
{
    MinimalTuple *tuples;
    graphid *edge_ids;
    bool *edge_id_valid;
    int count;
    int capacity;
} AgeWCOJTupleGroup;

typedef struct AgeWCOJJoinScanState AgeWCOJJoinScanState;
typedef struct AgeWCOJPostingProvider AgeWCOJPostingProvider;

typedef struct AgeWCOJSourceRow
{
    graphid key;
    MinimalTuple tuple;
} AgeWCOJSourceRow;

typedef struct AgeWCOJSourceKeyBag
{
    graphid key;
    int first_row;
    int row_count;
} AgeWCOJSourceKeyBag;

typedef struct AgeWCOJBatchPayload
{
    AgeAdjacencyPayload payload;
    int survivor_index;
    int source_bag_index;
} AgeWCOJBatchPayload;

typedef struct AgeWCOJBatchPayloadRange
{
    int first_payload;
    int payload_count;
} AgeWCOJBatchPayloadRange;

typedef enum AgeWCOJAdjacencyBuildResult
{
    AGE_WCOJ_ADJACENCY_BUILD_EMPTY = 0,
    AGE_WCOJ_ADJACENCY_BUILD_READY,
    AGE_WCOJ_ADJACENCY_BUILD_OVERSIZE
} AgeWCOJAdjacencyBuildResult;

typedef struct AgeWCOJPostingProviderOps
{
    bool (*first_distinct)(AgeWCOJPostingProvider *provider);
    bool (*next_distinct)(AgeWCOJPostingProvider *provider);
    bool (*seek_ge)(AgeWCOJPostingProvider *provider, graphid target);
    bool (*materialize_group)(AgeWCOJPostingProvider *provider,
                              graphid terminal);
    void (*rescan)(AgeWCOJPostingProvider *provider);
    void (*close)(AgeWCOJPostingProvider *provider);
} AgeWCOJPostingProviderOps;

/*
 * The first provider implementation deliberately wraps the already-planned,
 * sorted child PlanState.  It gives the intersection engines one contract and
 * remains the correctness fallback when a direct adjacency provider is not
 * available.  seek_ge() is an emulated seek over the sorted stream; a future
 * adjacency provider can replace it with an index-level seek without changing
 * the survivor or bag-restoration code.
 */
struct AgeWCOJPostingProvider
{
    const AgeWCOJPostingProviderOps *ops;
    AgeWCOJProviderKind kind;
    AgeWCOJJoinScanState *owner;
    PlanState *plan_state;
    AttrNumber key_attno;
    TupleTableSlot *current_slot;
    graphid current_key;
    graphid last_key;
    bool has_current;
    bool has_last_key;
    bool exhausted;
    bool seekable;
    int source_index;
    Oid index_oid;
    bool outgoing;
    int32 terminal_label_id;
    AttrNumber source_key_attno;
    int *output_map;
    int output_width;
    int output_offset;
    AttrNumber edge_id_output_attno;
    ExprState *local_qual;
    TupleTableSlot *source_slot;
    TupleTableSlot *output_slot;
    MemoryContext provider_context;
    AgeWCOJSourceRow *source_rows;
    int source_row_count;
    int source_row_capacity;
    AgeWCOJSourceKeyBag *source_bags;
    int source_bag_count;
    graphid *terminals;
    int terminal_count;
    int terminal_capacity;
    int terminal_index;
    AgeAdjacencyVisiblePayloadScan *terminal_scan;
    AgeAdjacencyVisiblePayloadScan *payload_scan;
    AgeWCOJTupleGroup *batch_groups;
    AgeWCOJBatchPayload *batch_payloads;
    AgeWCOJBatchPayloadRange *batch_payload_ranges;
    int batch_payload_count;
    int batch_payload_capacity;
    bool fetch_properties;
    bool built;
    int64 rows_scanned;
};

struct AgeWCOJJoinScanState
{
    CustomScanState css;
    int arity;
    double *estimated_postings;
    int *source_order;
    Bitmapset **uniqueness_groups;
    int uniqueness_group_count;
    AgeWCOJPostingProvider *providers;
    TupleTableSlot **group_slots;
    AgeWCOJTupleGroup *groups;
    int *combination_indexes;
    int *enumeration_order;
    graphid *selected_edge_ids;
    bool *selected_edge_id_valid;
    bool enumeration_started;
    int64 *stage_survivors;
    MemoryContext group_context;
    MemoryContext batch_context;
    graphid *survivor_block;
    int survivor_block_count;
    int survivor_block_index;
    int survivor_block_capacity;
    int active_survivor_index;
    Size batch_memory_budget;
    AgeWCOJEngineKind requested_engine;
    AgeWCOJEngineKind planned_engine;
    AgeWCOJEngineKind actual_engine;
    const char *fallback_reason;
    int seed_index;
    bool group_active;
    bool exhausted;
    bool batch_payload_enabled;
    bool survivor_input_exhausted;
    int64 groups_matched;
    int64 candidate_combinations;
    int64 cursor_advances;
    int64 seek_calls;
    int64 posting_rows_scanned;
    int64 payload_rows_scanned;
    int64 payload_rows_matched;
    int64 payload_rows_fetched;
    int64 payload_scan_batches;
    int64 payload_source_keys_scanned;
    int64 payload_scan_restarts_avoided;
    int64 survivor_blocks;
    int64 uniqueness_rejects;
    int64 rows_emitted;
    int64 rescans;
    int64 progressive_probes;
    int64 progressive_matches;
    int64 source_rows_scanned;
    int64 distinct_source_keys;
    int64 intersection_builds;
    int64 local_predicate_rejects;
    int64 exact_set_filters;
    int64 exact_set_candidates_tested;
    bool progressive_setup_done;
    Size peak_memory;
};

static inline void
increment_wcoj_counter(int64 *counter)
{
    if (*counter < PG_INT64_MAX)
        (*counter)++;
}

static inline void
add_wcoj_counter(int64 *counter, int64 increment)
{
    if (increment <= 0)
        return;
    if (*counter > PG_INT64_MAX - increment)
        *counter = PG_INT64_MAX;
    else
        *counter += increment;
}

static Node *create_age_wcoj_join_scan_state(CustomScan *cscan);
static void begin_age_wcoj_join_scan(CustomScanState *node, EState *estate,
                                     int eflags);
static TupleTableSlot *exec_age_wcoj_join_scan(CustomScanState *node);
static TupleTableSlot *access_age_wcoj_join_scan(ScanState *node);
static bool recheck_age_wcoj_join_scan(ScanState *node,
                                       TupleTableSlot *slot);
static void end_age_wcoj_join_scan(CustomScanState *node);
static void rescan_age_wcoj_join_scan(CustomScanState *node);
static void explain_age_wcoj_join_scan(CustomScanState *node,
                                       List *ancestors, ExplainState *es);
static bool plan_stream_first_distinct(AgeWCOJPostingProvider *provider);
static bool plan_stream_next_distinct(AgeWCOJPostingProvider *provider);
static bool plan_stream_seek_ge(AgeWCOJPostingProvider *provider,
                                graphid target);
static bool plan_stream_materialize_group(AgeWCOJPostingProvider *provider,
                                          graphid terminal);
static bool plan_stream_buffer_group(AgeWCOJPostingProvider *provider,
                                     graphid terminal,
                                     int survivor_index);
static void plan_stream_rescan(AgeWCOJPostingProvider *provider);
static void plan_stream_close(AgeWCOJPostingProvider *provider);
static bool adjacency_first_distinct(AgeWCOJPostingProvider *provider);
static bool adjacency_next_distinct(AgeWCOJPostingProvider *provider);
static bool adjacency_seek_ge(AgeWCOJPostingProvider *provider,
                              graphid target);
static bool adjacency_materialize_group(AgeWCOJPostingProvider *provider,
                                        graphid terminal);
static void adjacency_rescan(AgeWCOJPostingProvider *provider);
static void adjacency_close(AgeWCOJPostingProvider *provider);
static bool build_adjacency_provider(AgeWCOJPostingProvider *provider);
static AgeWCOJAdjacencyBuildResult build_adjacency_provider_filtered(
    AgeWCOJPostingProvider *provider, const graphid *candidates,
    int candidate_count, int posting_limit);
static int intersect_age_wcoj_graphids(graphid *candidates,
                                       int candidate_count,
                                       const graphid *matches,
                                       int match_count);
static void rescan_age_wcoj_providers(AgeWCOJJoinScanState *state);
static bool prepare_age_wcoj_progressive_exact_set(
    AgeWCOJJoinScanState *state);
static void update_age_wcoj_peak_memory(AgeWCOJJoinScanState *state);
static void reset_age_wcoj_survivor_block(AgeWCOJJoinScanState *state);
static int compare_age_wcoj_batch_payloads(const void *left,
                                           const void *right);
static int find_age_wcoj_survivor_index(AgeWCOJJoinScanState *state,
                                        graphid terminal);
static void append_age_wcoj_batch_payload(
    AgeWCOJPostingProvider *provider, int survivor_index,
    int source_bag_index, const AgeAdjacencyPayload *payload);
static void clear_age_wcoj_group(AgeWCOJJoinScanState *state);
static bool align_age_wcoj_leapfrog(AgeWCOJJoinScanState *state,
                                    graphid *matched_key);
static bool align_age_wcoj_merge(AgeWCOJJoinScanState *state,
                                 graphid *matched_key);
static bool align_age_wcoj_progressive(AgeWCOJJoinScanState *state,
                                       graphid *matched_key);
static void append_age_wcoj_group_tuple(AgeWCOJJoinScanState *state,
                                        int child_index,
                                        TupleTableSlot *slot,
                                        bool edge_id_valid,
                                        graphid edge_id);
static bool collect_age_wcoj_group(AgeWCOJJoinScanState *state,
                                   graphid matched_key);
static bool find_next_age_wcoj_survivor(AgeWCOJJoinScanState *state,
                                        graphid *matched_key);
static bool prepare_age_wcoj_survivor_block(AgeWCOJJoinScanState *state);
static bool prepare_age_wcoj_group(AgeWCOJJoinScanState *state);
static bool next_age_wcoj_combination(AgeWCOJJoinScanState *state);
static TupleTableSlot *materialize_age_wcoj_combination(
    AgeWCOJJoinScanState *state);

const CustomScanMethods age_wcoj_join_scan_methods = {
    AGE_WCOJ_JOIN_SCAN_NAME,
    create_age_wcoj_join_scan_state
};

static const CustomExecMethods age_wcoj_join_exec_methods = {
    AGE_WCOJ_JOIN_SCAN_NAME,
    begin_age_wcoj_join_scan,
    exec_age_wcoj_join_scan,
    end_age_wcoj_join_scan,
    rescan_age_wcoj_join_scan,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    explain_age_wcoj_join_scan
};

static const AgeWCOJPostingProviderOps plan_stream_provider_ops = {
    plan_stream_first_distinct,
    plan_stream_next_distinct,
    plan_stream_seek_ge,
    plan_stream_materialize_group,
    plan_stream_rescan,
    plan_stream_close
};

static const AgeWCOJPostingProviderOps adjacency_provider_ops = {
    adjacency_first_distinct,
    adjacency_next_distinct,
    adjacency_seek_ge,
    adjacency_materialize_group,
    adjacency_rescan,
    adjacency_close
};

static bool
valid_wcoj_engine(int engine)
{
    return engine >= AGE_WCOJ_ENGINE_AUTO &&
        engine <= AGE_WCOJ_ENGINE_MERGE;
}

static Node *
create_age_wcoj_join_scan_state(CustomScan *cscan)
{
    AgeWCOJJoinScanState *state;
    List *key_attnos;
    List *estimated_postings;
    List *provider_descs;
    List *uniqueness_group_descs;
    ListCell *lc;
    int arity;
    int requested_engine;
    int planned_engine;
    int index;

    if (list_length(cscan->custom_private) !=
        AGE_WCOJ_JOIN_PRIVATE_COUNT)
    {
        elog(ERROR, "invalid AGE WCOJ join private descriptor");
    }

    arity = intVal(list_nth(cscan->custom_private,
                            AGE_WCOJ_JOIN_PRIVATE_ARITY));
    key_attnos = list_nth(cscan->custom_private,
                          AGE_WCOJ_JOIN_PRIVATE_KEY_ATTNOS);
    requested_engine = intVal(list_nth(
        cscan->custom_private, AGE_WCOJ_JOIN_PRIVATE_REQUESTED_ENGINE));
    planned_engine = intVal(list_nth(
        cscan->custom_private, AGE_WCOJ_JOIN_PRIVATE_PLANNED_ENGINE));
    estimated_postings = list_nth(
        cscan->custom_private, AGE_WCOJ_JOIN_PRIVATE_ESTIMATED_POSTINGS);
    provider_descs = list_nth(
        cscan->custom_private, AGE_WCOJ_JOIN_PRIVATE_PROVIDER_DESCS);
    uniqueness_group_descs = list_nth(
        cscan->custom_private, AGE_WCOJ_JOIN_PRIVATE_UNIQUENESS_GROUPS);
    if (arity < 2 || list_length(key_attnos) != arity ||
        list_length(estimated_postings) != arity ||
        list_length(provider_descs) != arity ||
        list_length(uniqueness_group_descs) != arity)
    {
        elog(ERROR, "invalid AGE WCOJ join arity");
    }
    if (!valid_wcoj_engine(requested_engine) ||
        !valid_wcoj_engine(planned_engine) ||
        planned_engine == AGE_WCOJ_ENGINE_AUTO)
    {
        elog(ERROR, "invalid AGE WCOJ engine descriptor");
    }

    state = palloc0(sizeof(*state));
    state->css.ss.ps.type = T_CustomScanState;
    state->css.flags = cscan->flags;
    state->css.methods = &age_wcoj_join_exec_methods;
    state->arity = arity;
    state->requested_engine = (AgeWCOJEngineKind)requested_engine;
    state->planned_engine = (AgeWCOJEngineKind)planned_engine;
    state->actual_engine = state->planned_engine;
    state->fallback_reason = "none";
    state->estimated_postings = palloc0(sizeof(double) * arity);
    state->source_order = palloc0(sizeof(int) * arity);

    index = 0;
    foreach(lc, estimated_postings)
    {
        Node *value = lfirst(lc);
        double estimate;

        if (!IsA(value, Float))
            elog(ERROR, "invalid AGE WCOJ posting estimate");
        estimate = floatVal(value);
        if (estimate < 0)
            estimate = 0;
        state->estimated_postings[index] = estimate;
        state->source_order[index] = index;
        index++;
    }

    /* Stable insertion sort keeps equal estimates in planner child order. */
    for (index = 1; index < arity; index++)
    {
        int source = state->source_order[index];
        int position = index;

        while (position > 0 &&
               state->estimated_postings[state->source_order[position - 1]] >
                   state->estimated_postings[source])
        {
            state->source_order[position] =
                state->source_order[position - 1];
            position--;
        }
        state->source_order[position] = source;
    }
    state->seed_index = state->source_order[0];

    return (Node *)state;
}

static Oid
age_wcoj_provider_desc_oid(List *desc, int field)
{
    Const *value;

    value = castNode(Const, list_nth(desc, field));
    if (value->consttype != OIDOID || value->constisnull)
        elog(ERROR, "invalid AGE WCOJ provider OID");
    return DatumGetObjectId(value->constvalue);
}

static TupleDesc
age_wcoj_provider_output_desc(CustomScan *cscan, int output_offset,
                              int output_width)
{
    List *segment = NIL;
    int index;

    if (output_offset <= 0 || output_width <= 0 ||
        output_offset + output_width - 1 >
            list_length(cscan->custom_scan_tlist))
    {
        elog(ERROR, "invalid AGE WCOJ provider output range");
    }
    for (index = 0; index < output_width; index++)
    {
        TargetEntry *tle = copyObject(list_nth(
            cscan->custom_scan_tlist, output_offset - 1 + index));

        tle->resno = index + 1;
        segment = lappend(segment, tle);
    }

    return ExecTypeFromTL(segment);
}

static void
initialize_wcoj_runtime_state(AgeWCOJJoinScanState *state, EState *estate)
{
    int arity = state->arity;
    uint64 budget;

    state->providers = palloc0(sizeof(AgeWCOJPostingProvider) * arity);
    state->group_slots = palloc0(sizeof(TupleTableSlot *) * arity);
    state->groups = palloc0(sizeof(AgeWCOJTupleGroup) * arity);
    state->combination_indexes = palloc0(sizeof(int) * arity);
    state->enumeration_order = palloc0(sizeof(int) * arity);
    state->selected_edge_ids = palloc0(sizeof(graphid) * arity);
    state->selected_edge_id_valid = palloc0(sizeof(bool) * arity);
    state->uniqueness_groups = palloc0(sizeof(Bitmapset *) * arity);
    state->stage_survivors = palloc0(sizeof(int64) * arity);
    state->group_context = AllocSetContextCreate(
        estate->es_query_cxt, "AGE WCOJ duplicate group",
        ALLOCSET_DEFAULT_SIZES);
    state->batch_context = AllocSetContextCreate(
        estate->es_query_cxt, "AGE WCOJ survivor block",
        ALLOCSET_DEFAULT_SIZES);

    budget = ((uint64)Max(work_mem, 64) * 1024) /
        AGE_WCOJ_SURVIVOR_BLOCK_BUDGET_DIVISOR;
    budget = Max(budget, (uint64)64 * 1024);
    budget = Min(budget, (uint64)MaxAllocSize);
    state->batch_memory_budget = (Size)budget;
}

static void
initialize_wcoj_uniqueness_groups(AgeWCOJJoinScanState *state,
                                  List *group_descs)
{
    ListCell *group_desc_cell;
    int source_index = 0;

    foreach(group_desc_cell, group_descs)
    {
        List *group_ids = lfirst(group_desc_cell);
        ListCell *group_cell;

        foreach(group_cell, group_ids)
        {
            int group_id = intVal(lfirst(group_cell));

            if (group_id < 0)
                elog(ERROR, "invalid AGE WCOJ uniqueness group");
            state->uniqueness_groups[source_index] = bms_add_member(
                state->uniqueness_groups[source_index], group_id);
            state->uniqueness_group_count = Max(
                state->uniqueness_group_count, group_id + 1);
        }
        source_index++;
    }
}

static int
initialize_wcoj_provider(AgeWCOJJoinScanState *state,
                         CustomScanState *node, CustomScan *cscan,
                         EState *estate, PlanState *child_state, List *desc,
                         List *local_quals, int source_index, int key_attno,
                         int output_offset)
{
    AgeWCOJPostingProvider *provider = &state->providers[source_index];
    TupleDesc child_desc = ExecGetResultType(child_state);
    List *output_map;
    int provider_kind;
    int output_width;

    if (list_length(desc) != AGE_WCOJ_PROVIDER_DESC_COUNT)
        elog(ERROR, "invalid AGE WCOJ provider descriptor");

    provider_kind = intVal(list_nth(desc, AGE_WCOJ_PROVIDER_DESC_KIND));
    output_map = list_nth(desc, AGE_WCOJ_PROVIDER_DESC_OUTPUT_MAP);
    output_width = provider_kind == AGE_WCOJ_PROVIDER_PLAN_STREAM ?
        child_desc->natts : list_length(output_map);

    provider->kind = (AgeWCOJProviderKind)provider_kind;
    provider->owner = state;
    provider->plan_state = child_state;
    provider->source_index = source_index;
    provider->index_oid = age_wcoj_provider_desc_oid(
        desc, AGE_WCOJ_PROVIDER_DESC_INDEX_OID);
    provider->outgoing = intVal(list_nth(
        desc, AGE_WCOJ_PROVIDER_DESC_OUTGOING)) != 0;
    provider->terminal_label_id = intVal(list_nth(
        desc, AGE_WCOJ_PROVIDER_DESC_TERMINAL_LABEL_ID));
    provider->source_key_attno = (AttrNumber)intVal(list_nth(
        desc, AGE_WCOJ_PROVIDER_DESC_SOURCE_KEY_ATTNO));
    provider->output_width = output_width;
    provider->output_offset = output_offset;
    provider->edge_id_output_attno = (AttrNumber)intVal(list_nth(
        desc, AGE_WCOJ_PROVIDER_DESC_EDGE_ID_OUTPUT_ATTNO));
    provider->local_qual = local_quals != NIL ?
        ExecInitQual(local_quals, (PlanState *)node) : NULL;

    if (provider_kind == AGE_WCOJ_PROVIDER_PLAN_STREAM)
    {
        if (key_attno <= 0 || key_attno > child_desc->natts)
        {
            elog(ERROR,
                 "AGE WCOJ key attribute %d exceeds child %d width %d",
                 key_attno, source_index + 1, child_desc->natts);
        }
        provider->ops = &plan_stream_provider_ops;
        provider->key_attno = (AttrNumber)key_attno;
        provider->seekable = true;
        state->group_slots[source_index] = ExecInitExtraTupleSlot(
            estate, child_desc, &TTSOpsMinimalTuple);
        return output_width;
    }

    if (provider_kind == AGE_WCOJ_PROVIDER_ADJACENCY)
    {
        TupleDesc output_desc;
        ListCell *map_cell;
        int map_index = 0;

        if (!OidIsValid(provider->index_oid) ||
            provider->source_key_attno <= 0 ||
            provider->source_key_attno > child_desc->natts ||
            output_width <= 0)
        {
            elog(ERROR, "invalid AGE WCOJ adjacency provider");
        }

        provider->ops = &adjacency_provider_ops;
        provider->seekable = true;
        provider->fetch_properties = intVal(list_nth(
            desc, AGE_WCOJ_PROVIDER_DESC_FETCH_PROPERTIES)) != 0;
        provider->provider_context = AllocSetContextCreate(
            estate->es_query_cxt, "AGE WCOJ adjacency provider",
            ALLOCSET_DEFAULT_SIZES);
        output_desc = age_wcoj_provider_output_desc(
            cscan, output_offset, output_width);
        state->group_slots[source_index] = ExecInitExtraTupleSlot(
            estate, output_desc, &TTSOpsMinimalTuple);
        provider->output_slot = ExecInitExtraTupleSlot(
            estate, output_desc, &TTSOpsVirtual);
        provider->source_slot = ExecInitExtraTupleSlot(
            estate, child_desc, &TTSOpsMinimalTuple);
        provider->output_map = palloc(sizeof(int) * output_width);
        foreach(map_cell, output_map)
            provider->output_map[map_index++] = intVal(lfirst(map_cell));
        provider->terminal_scan = age_adjacency_begin_visible_payload_scan(
            provider->index_oid, estate->es_snapshot, false);
        provider->payload_scan = age_adjacency_begin_visible_payload_scan(
            provider->index_oid, estate->es_snapshot,
            provider->fetch_properties);
        age_adjacency_visible_payload_scan_set_terminal_label(
            provider->terminal_scan, provider->terminal_label_id);
        age_adjacency_visible_payload_scan_set_terminal_label(
            provider->payload_scan, provider->terminal_label_id);
        return output_width;
    }

    elog(ERROR, "invalid AGE WCOJ provider kind %d", provider_kind);
    return 0;
}

static void
begin_age_wcoj_join_scan(CustomScanState *node, EState *estate, int eflags)
{
    AgeWCOJJoinScanState *state = (AgeWCOJJoinScanState *)node;
    CustomScan *cscan = (CustomScan *)node->ss.ps.plan;
    List *key_attnos = list_nth(
        cscan->custom_private, AGE_WCOJ_JOIN_PRIVATE_KEY_ATTNOS);
    List *provider_descs = list_nth(
        cscan->custom_private, AGE_WCOJ_JOIN_PRIVATE_PROVIDER_DESCS);
    List *uniqueness_group_descs = list_nth(
        cscan->custom_private, AGE_WCOJ_JOIN_PRIVATE_UNIQUENESS_GROUPS);
    ListCell *plan_cell;
    ListCell *desc_cell;
    ListCell *qual_cell;
    int child_index;
    int adjacency_provider_count = 0;
    int output_offset = 1;

    foreach(plan_cell, cscan->custom_plans)
    {
        Plan *subplan = (Plan *)lfirst(plan_cell);

        node->custom_ps = lappend(node->custom_ps,
                                  ExecInitNode(subplan, estate, eflags));
    }
    if (list_length(node->custom_ps) != state->arity ||
        list_length(key_attnos) != state->arity ||
        list_length(provider_descs) != state->arity ||
        list_length(uniqueness_group_descs) != state->arity ||
        list_length(cscan->custom_exprs) != state->arity)
    {
        elog(ERROR, "AGE WCOJ join child count does not match arity");
    }

    initialize_wcoj_runtime_state(state, estate);
    initialize_wcoj_uniqueness_groups(state, uniqueness_group_descs);

    child_index = 0;
    forthree(plan_cell, node->custom_ps,
             desc_cell, provider_descs,
             qual_cell, cscan->custom_exprs)
    {
        PlanState *child_state = (PlanState *)lfirst(plan_cell);
        List *desc = (List *)lfirst(desc_cell);
        List *local_quals = (List *)lfirst(qual_cell);
        int key_attno = intVal(list_nth(key_attnos, child_index));

        output_offset += initialize_wcoj_provider(
            state, node, cscan, estate, child_state, desc, local_quals,
            child_index, key_attno, output_offset);
        if (state->providers[child_index].kind ==
            AGE_WCOJ_PROVIDER_ADJACENCY)
        {
            adjacency_provider_count++;
        }
        child_index++;
    }

    /*
     * A survivor block is useful whenever at least one direct adjacency
     * provider is present.  Plan-stream providers are buffered by duplicate
     * group while adjacency providers defer payload retrieval to one tagged
     * run scan per block.
     */
    state->batch_payload_enabled = adjacency_provider_count > 0;

    if (state->actual_engine == AGE_WCOJ_ENGINE_LEAPFROG)
    {
        for (child_index = 0; child_index < state->arity; child_index++)
        {
            if (!state->providers[child_index].seekable)
            {
                state->actual_engine = AGE_WCOJ_ENGINE_MERGE;
                state->fallback_reason = "provider does not support seek_ge";
                break;
            }
        }
    }
}

static TupleTableSlot *
exec_age_wcoj_join_scan(CustomScanState *node)
{
    AgeWCOJJoinScanState *state = (AgeWCOJJoinScanState *)node;
    TupleTableSlot *slot;

    slot = ExecScan(&node->ss, access_age_wcoj_join_scan,
                    recheck_age_wcoj_join_scan);
    if (!TupIsNull(slot))
        increment_wcoj_counter(&state->rows_emitted);

    return slot;
}

static bool
plan_stream_fetch_row(AgeWCOJPostingProvider *provider)
{
    AgeWCOJJoinScanState *state = provider->owner;
    TupleTableSlot *slot;
    Datum value;
    graphid key;
    bool isnull;

    for (;;)
    {
        slot = ExecProcNode(provider->plan_state);
        if (TupIsNull(slot))
        {
            provider->current_slot = NULL;
            provider->has_current = false;
            provider->exhausted = true;
            return false;
        }

        increment_wcoj_counter(&provider->rows_scanned);
        increment_wcoj_counter(&state->posting_rows_scanned);
        /*
         * PLAN_STREAM_PROVIDER receives an already materialized child row.
         * Count it as a payload fetch even when its terminal is rejected so
         * EXPLAIN does not imply that this fallback provides true late edge
         * materialization.  A direct adjacency provider can instead increment
         * this counter only while materializing survivor groups.
         */
        increment_wcoj_counter(&state->payload_rows_fetched);

        value = slot_getattr(slot, provider->key_attno, &isnull);
        if (isnull)
            continue;
        key = DATUM_GET_GRAPHID(value);
        if (provider->has_last_key && key < provider->last_key)
        {
            elog(ERROR,
                 "AGE WCOJ plan-stream provider received unsorted graphids");
        }
        provider->last_key = key;
        provider->has_last_key = true;
        provider->current_slot = slot;
        provider->current_key = key;
        provider->has_current = true;
        return true;
    }
}

static bool
plan_stream_first_distinct(AgeWCOJPostingProvider *provider)
{
    if (provider->has_current)
        return true;
    if (provider->exhausted)
        return false;
    return plan_stream_fetch_row(provider);
}

static void
record_provider_advance(AgeWCOJPostingProvider *provider)
{
    increment_wcoj_counter(&provider->owner->cursor_advances);
}

static bool
plan_stream_next_distinct(AgeWCOJPostingProvider *provider)
{
    graphid previous_key;

    if (!plan_stream_first_distinct(provider))
        return false;
    previous_key = provider->current_key;
    for (;;)
    {
        if (!plan_stream_fetch_row(provider))
        {
            record_provider_advance(provider);
            return false;
        }
        if (provider->current_key != previous_key)
        {
            record_provider_advance(provider);
            return true;
        }
    }
}

static bool
plan_stream_seek_ge(AgeWCOJPostingProvider *provider, graphid target)
{
    increment_wcoj_counter(&provider->owner->seek_calls);

    if (!plan_stream_first_distinct(provider))
        return false;
    while (provider->current_key < target)
    {
        if (!plan_stream_next_distinct(provider))
            return false;
    }
    return true;
}

static void
update_age_wcoj_peak_memory(AgeWCOJJoinScanState *state)
{
    Size current_memory = 0;
    int source_index;

    if (state->group_context != NULL)
    {
        current_memory += MemoryContextMemAllocated(state->group_context,
                                                    true);
    }
    if (state->batch_context != NULL)
    {
        current_memory += MemoryContextMemAllocated(state->batch_context,
                                                    true);
    }
    for (source_index = 0; source_index < state->arity; source_index++)
    {
        MemoryContext provider_context =
            state->providers[source_index].provider_context;

        if (provider_context != NULL)
        {
            current_memory += MemoryContextMemAllocated(provider_context,
                                                        true);
        }
    }
    if (current_memory > state->peak_memory)
        state->peak_memory = current_memory;
}

static void
reset_age_wcoj_survivor_block(AgeWCOJJoinScanState *state)
{
    int source_index;

    if (state->batch_context != NULL)
        MemoryContextReset(state->batch_context);
    state->survivor_block = NULL;
    state->survivor_block_count = 0;
    state->survivor_block_index = 0;
    state->survivor_block_capacity = 0;
    state->active_survivor_index = -1;

    if (state->providers == NULL)
        return;
    for (source_index = 0; source_index < state->arity; source_index++)
    {
        AgeWCOJPostingProvider *provider = &state->providers[source_index];

        provider->batch_groups = NULL;
        provider->batch_payloads = NULL;
        provider->batch_payload_ranges = NULL;
        provider->batch_payload_count = 0;
        provider->batch_payload_capacity = 0;
    }
}

static int
age_wcoj_survivor_block_capacity(AgeWCOJJoinScanState *state)
{
    double bytes_per_survivor = sizeof(graphid);
    double capacity;
    int source_index;

    for (source_index = 0; source_index < state->arity; source_index++)
    {
        AgeWCOJPostingProvider *provider = &state->providers[source_index];

        if (provider->kind == AGE_WCOJ_PROVIDER_PLAN_STREAM)
        {
            bytes_per_survivor += sizeof(AgeWCOJTupleGroup) + 128.0;
        }
        else
        {
            double multiplicity = 1.0;

            if (provider->terminal_count > 0 && provider->rows_scanned > 0)
            {
                multiplicity = Max(
                    1.0,
                    (double)provider->rows_scanned /
                        (double)provider->terminal_count);
            }
            bytes_per_survivor += sizeof(AgeWCOJBatchPayloadRange) +
                multiplicity * sizeof(AgeWCOJBatchPayload);
        }
    }

    capacity = (double)state->batch_memory_budget /
        Max(bytes_per_survivor, 1.0);
    if (capacity < AGE_WCOJ_SURVIVOR_BLOCK_MIN)
        capacity = AGE_WCOJ_SURVIVOR_BLOCK_MIN;
    if (capacity > AGE_WCOJ_SURVIVOR_BLOCK_MAX)
        capacity = AGE_WCOJ_SURVIVOR_BLOCK_MAX;
    return Max((int)capacity, 1);
}

static void
allocate_age_wcoj_survivor_block(AgeWCOJJoinScanState *state, int capacity)
{
    MemoryContext oldcontext;
    int source_index;

    if (capacity <= 0)
        elog(ERROR, "invalid AGE WCOJ survivor block capacity");

    oldcontext = MemoryContextSwitchTo(state->batch_context);
    state->survivor_block = palloc(sizeof(graphid) * capacity);
    state->survivor_block_capacity = capacity;
    for (source_index = 0; source_index < state->arity; source_index++)
    {
        AgeWCOJPostingProvider *provider = &state->providers[source_index];

        if (provider->kind == AGE_WCOJ_PROVIDER_PLAN_STREAM)
        {
            provider->batch_groups = palloc0(
                sizeof(AgeWCOJTupleGroup) * capacity);
        }
        else
        {
            provider->batch_payload_ranges = palloc0(
                sizeof(AgeWCOJBatchPayloadRange) * capacity);
        }
    }
    MemoryContextSwitchTo(oldcontext);
}

static void
prepare_age_wcoj_survivor_filter(
    AgeWCOJJoinScanState *state, AgeAdjacencyCompositeTerminalFilter *filter,
    int32 terminal_label_id)
{
    memset(filter, 0, sizeof(*filter));
    filter->terminal_label_id = terminal_label_id;
    filter->source = "wcoj-survivor-block";
    filter->has_vertex_set_filter = true;
    filter->vertex_set_filter.sorted_vertex_ids = state->survivor_block;
    filter->vertex_set_filter.source = "wcoj-survivor-block";
    filter->vertex_set_filter.matches = state->survivor_block_count;
    filter->vertex_set_filter.sorted_vertex_count =
        state->survivor_block_count;
    filter->vertex_set_filter.min_vertex_id = state->survivor_block[0];
    filter->vertex_set_filter.max_vertex_id =
        state->survivor_block[state->survivor_block_count - 1];
    filter->vertex_set_filter.has_range = true;
    filter->vertex_set_filter.has_sorted_vertex_ids = true;
}

static void
prefetch_age_wcoj_adjacency_payloads(AgeWCOJPostingProvider *provider)
{
    AgeWCOJJoinScanState *state = provider->owner;
    AgeAdjacencyVisiblePayloadRunKey *keys;
    AgeAdjacencyVisiblePayloadRunOptions options;
    AgeAdjacencyCompositeTerminalFilter filter;
    AgeAdjacencyVisiblePayloadRunScan *run_scan;
    AgeAdjacencyPayload payload;
    MemoryContext oldcontext;
    void *tag;
    int bag_index;
    int payload_index;
    int64 old_restart_count;

    if (state->survivor_block_count <= 0 || provider->source_bag_count <= 0)
        return;

    oldcontext = MemoryContextSwitchTo(state->batch_context);
    keys = palloc(sizeof(AgeAdjacencyVisiblePayloadRunKey) *
                   provider->source_bag_count);
    for (bag_index = 0; bag_index < provider->source_bag_count; bag_index++)
    {
        keys[bag_index].key = provider->source_bags[bag_index].key;
        keys[bag_index].tag = &provider->source_bags[bag_index];
    }

    prepare_age_wcoj_survivor_filter(state, &filter,
                                     provider->terminal_label_id);
    memset(&options, 0, sizeof(options));
    options.terminal_label_id = provider->terminal_label_id;
    options.prepared_filter = &filter;
    options.prepared_filter_valid = true;
    run_scan = age_adjacency_begin_visible_payload_run_scan_with_options(
        provider->index_oid, state->css.ss.ps.state->es_snapshot,
        provider->fetch_properties, &options, keys,
        provider->source_bag_count);
    increment_wcoj_counter(&state->payload_scan_batches);
    add_wcoj_counter(&state->payload_source_keys_scanned,
                     provider->source_bag_count);
    old_restart_count =
        (int64)state->survivor_block_count * provider->source_bag_count;
    if (old_restart_count > provider->source_bag_count)
    {
        add_wcoj_counter(&state->payload_scan_restarts_avoided,
                         old_restart_count - provider->source_bag_count);
    }

    while (run_scan != NULL &&
           age_adjacency_visible_payload_run_scan_next_tag(
               run_scan, &payload, &tag))
    {
        AgeWCOJSourceKeyBag *bag = tag;
        int survivor_index;

        increment_wcoj_counter(&state->payload_rows_scanned);
        if (bag < provider->source_bags ||
            bag >= provider->source_bags + provider->source_bag_count)
        {
            elog(ERROR, "invalid AGE WCOJ payload source tag");
        }
        survivor_index = find_age_wcoj_survivor_index(
            state, payload.next_vertex_id);
        if (survivor_index < 0)
            continue;

        increment_wcoj_counter(&state->payload_rows_matched);
        increment_wcoj_counter(&state->payload_rows_fetched);
        append_age_wcoj_batch_payload(
            provider, survivor_index, (int)(bag - provider->source_bags),
            &payload);
    }
    if (run_scan != NULL)
        age_adjacency_end_visible_payload_run_scan(run_scan);

    if (provider->batch_payload_count > 1)
    {
        qsort(provider->batch_payloads, provider->batch_payload_count,
              sizeof(AgeWCOJBatchPayload),
              compare_age_wcoj_batch_payloads);
    }
    for (payload_index = 0;
         payload_index < provider->batch_payload_count;
         payload_index++)
    {
        AgeWCOJBatchPayload *entry =
            &provider->batch_payloads[payload_index];
        AgeWCOJBatchPayloadRange *range =
            &provider->batch_payload_ranges[entry->survivor_index];

        if (range->payload_count == 0)
            range->first_payload = payload_index;
        range->payload_count++;
    }
    MemoryContextSwitchTo(oldcontext);
}

static void
clear_age_wcoj_group(AgeWCOJJoinScanState *state)
{
    int child_index;

    if (state->group_context == NULL)
        return;

    for (child_index = 0; child_index < state->arity; child_index++)
    {
        if (state->group_slots[child_index] != NULL)
            ExecClearTuple(state->group_slots[child_index]);
        state->groups[child_index].tuples = NULL;
        state->groups[child_index].edge_ids = NULL;
        state->groups[child_index].edge_id_valid = NULL;
        state->groups[child_index].count = 0;
        state->groups[child_index].capacity = 0;
        state->combination_indexes[child_index] = -1;
        state->enumeration_order[child_index] = child_index;
        state->selected_edge_ids[child_index] = 0;
        state->selected_edge_id_valid[child_index] = false;
    }
    MemoryContextReset(state->group_context);
    state->enumeration_started = false;
    state->group_active = false;
}

static void
append_age_wcoj_tuple_group(AgeWCOJTupleGroup *group,
                            MemoryContext tuple_context,
                            TupleTableSlot *slot, bool edge_id_valid,
                            graphid edge_id)
{
    MemoryContext old_context;

    old_context = MemoryContextSwitchTo(tuple_context);
    if (group->count == group->capacity)
    {
        int new_capacity;

        if (group->capacity > (MaxAllocSize / sizeof(MinimalTuple)) / 2)
            ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                            errmsg("AGE WCOJ duplicate group is too large")));
        new_capacity = group->capacity == 0 ? 8 : group->capacity * 2;
        if (group->tuples == NULL)
        {
            group->tuples = palloc(sizeof(MinimalTuple) * new_capacity);
            group->edge_ids = palloc(sizeof(graphid) * new_capacity);
            group->edge_id_valid = palloc(sizeof(bool) * new_capacity);
        }
        else
        {
            group->tuples = repalloc(group->tuples,
                                     sizeof(MinimalTuple) * new_capacity);
            group->edge_ids = repalloc(group->edge_ids,
                                       sizeof(graphid) * new_capacity);
            group->edge_id_valid = repalloc(group->edge_id_valid,
                                            sizeof(bool) * new_capacity);
        }
        group->capacity = new_capacity;
    }
    group->tuples[group->count] = ExecCopySlotMinimalTuple(slot);
    group->edge_ids[group->count] = edge_id;
    group->edge_id_valid[group->count] = edge_id_valid;
    group->count++;
    MemoryContextSwitchTo(old_context);
}

static void
append_age_wcoj_group_tuple(AgeWCOJJoinScanState *state, int child_index,
                            TupleTableSlot *slot, bool edge_id_valid,
                            graphid edge_id)
{
    append_age_wcoj_tuple_group(&state->groups[child_index],
                                state->group_context, slot,
                                edge_id_valid, edge_id);
}

static bool
plan_stream_materialize_group(AgeWCOJPostingProvider *provider,
                              graphid terminal)
{
    AgeWCOJJoinScanState *state = provider->owner;
    int source_index = provider->source_index;

    if (state->batch_payload_enabled)
    {
        AgeWCOJTupleGroup *cached_group;

        if (provider->batch_groups == NULL ||
            state->active_survivor_index < 0 ||
            state->active_survivor_index >= state->survivor_block_count)
        {
            elog(ERROR, "invalid AGE WCOJ buffered plan-stream group");
        }
        cached_group = &provider->batch_groups[
            state->active_survivor_index];
        if (cached_group->count <= 0)
            return false;
        state->groups[source_index] = *cached_group;
        return true;
    }

    if (!plan_stream_first_distinct(provider) ||
        provider->current_key != terminal)
    {
        return false;
    }

    for (;;)
    {
        graphid edge_id = 0;
        bool edge_id_valid = false;

        if (provider->edge_id_output_attno > 0)
        {
            Datum edge_value;
            bool edge_isnull;

            edge_value = slot_getattr(provider->current_slot,
                                      provider->edge_id_output_attno,
                                      &edge_isnull);
            if (!edge_isnull)
            {
                edge_id = DATUM_GET_GRAPHID(edge_value);
                edge_id_valid = true;
            }
        }
        append_age_wcoj_group_tuple(state, source_index,
                                    provider->current_slot,
                                    edge_id_valid, edge_id);
        if (!plan_stream_fetch_row(provider))
        {
            record_provider_advance(provider);
            break;
        }
        if (provider->current_key != terminal)
        {
            record_provider_advance(provider);
            break;
        }
    }

    return state->groups[source_index].count > 0;
}

static bool
plan_stream_buffer_group(AgeWCOJPostingProvider *provider,
                         graphid terminal, int survivor_index)
{
    AgeWCOJJoinScanState *state = provider->owner;
    AgeWCOJTupleGroup *group;

    if (provider->batch_groups == NULL || survivor_index < 0 ||
        survivor_index >= state->survivor_block_capacity)
    {
        elog(ERROR, "invalid AGE WCOJ survivor block group index");
    }
    if (!plan_stream_first_distinct(provider) ||
        provider->current_key != terminal)
    {
        return false;
    }

    group = &provider->batch_groups[survivor_index];
    for (;;)
    {
        graphid edge_id = 0;
        bool edge_id_valid = false;

        if (provider->edge_id_output_attno > 0)
        {
            Datum edge_value;
            bool edge_isnull;

            edge_value = slot_getattr(provider->current_slot,
                                      provider->edge_id_output_attno,
                                      &edge_isnull);
            if (!edge_isnull)
            {
                edge_id = DATUM_GET_GRAPHID(edge_value);
                edge_id_valid = true;
            }
        }
        append_age_wcoj_tuple_group(group, state->batch_context,
                                    provider->current_slot,
                                    edge_id_valid, edge_id);
        if (!plan_stream_fetch_row(provider))
        {
            record_provider_advance(provider);
            break;
        }
        if (provider->current_key != terminal)
        {
            record_provider_advance(provider);
            break;
        }
    }

    return group->count > 0;
}

static void
plan_stream_rescan(AgeWCOJPostingProvider *provider)
{
    ExecReScan(provider->plan_state);
    provider->current_slot = NULL;
    provider->has_current = false;
    provider->has_last_key = false;
    provider->exhausted = false;
}

static void
plan_stream_close(AgeWCOJPostingProvider *provider)
{
    ExecEndNode(provider->plan_state);
    provider->current_slot = NULL;
    provider->has_current = false;
    provider->exhausted = true;
}

static int
compare_age_wcoj_source_rows(const void *left, const void *right)
{
    const AgeWCOJSourceRow *a = left;
    const AgeWCOJSourceRow *b = right;

    if (a->key < b->key)
        return -1;
    if (a->key > b->key)
        return 1;
    return 0;
}

static int
compare_age_wcoj_graphids(const void *left, const void *right)
{
    graphid a = *((const graphid *)left);
    graphid b = *((const graphid *)right);

    if (a < b)
        return -1;
    if (a > b)
        return 1;
    return 0;
}

static int
compare_age_wcoj_batch_payloads(const void *left, const void *right)
{
    const AgeWCOJBatchPayload *a = left;
    const AgeWCOJBatchPayload *b = right;

    if (a->survivor_index != b->survivor_index)
        return a->survivor_index < b->survivor_index ? -1 : 1;
    if (a->source_bag_index != b->source_bag_index)
        return a->source_bag_index < b->source_bag_index ? -1 : 1;
    if (a->payload.edge_id < b->payload.edge_id)
        return -1;
    if (a->payload.edge_id > b->payload.edge_id)
        return 1;
    return 0;
}

static int
find_age_wcoj_survivor_index(AgeWCOJJoinScanState *state, graphid terminal)
{
    int low = 0;
    int high = state->survivor_block_count;

    while (low < high)
    {
        int middle = low + (high - low) / 2;
        graphid candidate = state->survivor_block[middle];

        if (candidate < terminal)
            low = middle + 1;
        else
            high = middle;
    }
    if (low < state->survivor_block_count &&
        state->survivor_block[low] == terminal)
    {
        return low;
    }
    return -1;
}

static void
append_age_wcoj_batch_payload(AgeWCOJPostingProvider *provider,
                              int survivor_index, int source_bag_index,
                              const AgeAdjacencyPayload *payload)
{
    AgeWCOJBatchPayload *entry;
    MemoryContext oldcontext;

    oldcontext = MemoryContextSwitchTo(provider->owner->batch_context);
    if (provider->batch_payload_count == provider->batch_payload_capacity)
    {
        int new_capacity = provider->batch_payload_capacity == 0 ? 64 :
            provider->batch_payload_capacity * 2;

        if (new_capacity <= provider->batch_payload_capacity ||
            (Size)new_capacity > MaxAllocSize / sizeof(AgeWCOJBatchPayload))
        {
            ereport(ERROR,
                    (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                     errmsg("AGE WCOJ payload block is too large")));
        }
        if (provider->batch_payloads == NULL)
        {
            provider->batch_payloads = palloc(
                sizeof(AgeWCOJBatchPayload) * new_capacity);
        }
        else
        {
            provider->batch_payloads = repalloc(
                provider->batch_payloads,
                sizeof(AgeWCOJBatchPayload) * new_capacity);
        }
        provider->batch_payload_capacity = new_capacity;
    }

    entry = &provider->batch_payloads[provider->batch_payload_count++];
    entry->payload = *payload;
    if (provider->fetch_properties && !payload->properties_isnull)
    {
        entry->payload.properties = datumCopy(payload->properties, false, -1);
    }
    entry->survivor_index = survivor_index;
    entry->source_bag_index = source_bag_index;
    MemoryContextSwitchTo(oldcontext);
}

static void
append_age_wcoj_source_row(AgeWCOJPostingProvider *provider, graphid key,
                           TupleTableSlot *slot)
{
    MemoryContext oldcontext;

    oldcontext = MemoryContextSwitchTo(provider->provider_context);
    if (provider->source_row_count == provider->source_row_capacity)
    {
        int new_capacity = provider->source_row_capacity == 0 ? 16 :
            provider->source_row_capacity * 2;

        if (new_capacity <= provider->source_row_capacity ||
            (Size)new_capacity >
                MaxAllocSize / sizeof(AgeWCOJSourceRow))
        {
            ereport(ERROR,
                    (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                     errmsg("AGE WCOJ source bag is too large")));
        }
        if (provider->source_rows == NULL)
        {
            provider->source_rows = palloc(
                sizeof(AgeWCOJSourceRow) * new_capacity);
        }
        else
        {
            provider->source_rows = repalloc(
                provider->source_rows,
                sizeof(AgeWCOJSourceRow) * new_capacity);
        }
        provider->source_row_capacity = new_capacity;
    }
    provider->source_rows[provider->source_row_count].key = key;
    provider->source_rows[provider->source_row_count].tuple =
        ExecCopySlotMinimalTuple(slot);
    provider->source_row_count++;
    MemoryContextSwitchTo(oldcontext);
}

static void
append_age_wcoj_terminal(AgeWCOJPostingProvider *provider, graphid terminal)
{
    MemoryContext oldcontext;

    oldcontext = MemoryContextSwitchTo(provider->provider_context);
    if (provider->terminal_count == provider->terminal_capacity)
    {
        int new_capacity = provider->terminal_capacity == 0 ? 64 :
            provider->terminal_capacity * 2;

        if (new_capacity <= provider->terminal_capacity ||
            (Size)new_capacity > MaxAllocSize / sizeof(graphid))
        {
            ereport(ERROR,
                    (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                     errmsg("AGE WCOJ terminal set is too large")));
        }
        if (provider->terminals == NULL)
            provider->terminals = palloc(sizeof(graphid) * new_capacity);
        else
            provider->terminals = repalloc(
                provider->terminals, sizeof(graphid) * new_capacity);
        provider->terminal_capacity = new_capacity;
    }
    provider->terminals[provider->terminal_count++] = terminal;
    MemoryContextSwitchTo(oldcontext);
}

static void
set_adjacency_terminal_filter(AgeWCOJPostingProvider *provider,
                              const graphid *candidates,
                              int candidate_count)
{
    AgeAdjacencyVertexSetFilter filter;

    if (candidates == NULL)
    {
        age_adjacency_visible_payload_scan_set_terminal_vertex_set_filter(
            provider->terminal_scan, NULL);
        return;
    }

    memset(&filter, 0, sizeof(filter));
    filter.sorted_vertex_ids = candidates;
    filter.source = "wcoj-progressive";
    filter.matches = candidate_count;
    filter.sorted_vertex_count = candidate_count;
    filter.min_vertex_id = candidates[0];
    filter.max_vertex_id = candidates[candidate_count - 1];
    filter.has_range = true;
    filter.has_sorted_vertex_ids = true;
    age_adjacency_visible_payload_scan_set_terminal_vertex_set_filter(
        provider->terminal_scan, &filter);
}

static bool
materialize_adjacency_source_bags(AgeWCOJPostingProvider *provider)
{
    AgeWCOJJoinScanState *state = provider->owner;
    MemoryContext oldcontext;
    TupleTableSlot *slot;
    int row_index;
    int bag_index = -1;

    for (;;)
    {
        Datum value;
        graphid key;
        bool isnull;

        slot = ExecProcNode(provider->plan_state);
        if (TupIsNull(slot))
            break;
        increment_wcoj_counter(&state->source_rows_scanned);
        value = slot_getattr(slot, provider->source_key_attno, &isnull);
        if (isnull)
            continue;
        key = DATUM_GET_GRAPHID(value);
        append_age_wcoj_source_row(provider, key, slot);
    }
    if (provider->source_row_count == 0)
        return false;

    qsort(provider->source_rows, provider->source_row_count,
          sizeof(AgeWCOJSourceRow), compare_age_wcoj_source_rows);
    oldcontext = MemoryContextSwitchTo(provider->provider_context);
    provider->source_bags = palloc0(sizeof(AgeWCOJSourceKeyBag) *
                                     provider->source_row_count);
    MemoryContextSwitchTo(oldcontext);

    for (row_index = 0; row_index < provider->source_row_count; row_index++)
    {
        if (bag_index < 0 ||
            provider->source_bags[bag_index].key !=
                provider->source_rows[row_index].key)
        {
            bag_index++;
            provider->source_bags[bag_index].key =
                provider->source_rows[row_index].key;
            provider->source_bags[bag_index].first_row = row_index;
        }
        provider->source_bags[bag_index].row_count++;
    }
    provider->source_bag_count = bag_index + 1;
    add_wcoj_counter(&state->distinct_source_keys,
                     provider->source_bag_count);
    return true;
}

static bool
collect_adjacency_terminals(AgeWCOJPostingProvider *provider,
                            int posting_limit)
{
    AgeWCOJJoinScanState *state = provider->owner;
    int bag_index;

    for (bag_index = 0; bag_index < provider->source_bag_count; bag_index++)
    {
        AgeAdjacencyPayload payload;
        bool active;

        increment_wcoj_counter(&state->intersection_builds);
        active = age_adjacency_visible_payload_scan_begin_key(
            provider->terminal_scan, provider->source_bags[bag_index].key);
        while (active && age_adjacency_visible_payload_scan_next(
                             provider->terminal_scan, &payload))
        {
            increment_wcoj_counter(&provider->rows_scanned);
            increment_wcoj_counter(&state->posting_rows_scanned);
            if (posting_limit > 0 &&
                provider->rows_scanned > posting_limit)
            {
                return false;
            }
            append_age_wcoj_terminal(provider, payload.next_vertex_id);
        }
    }
    return true;
}

static void
deduplicate_adjacency_terminals(AgeWCOJPostingProvider *provider)
{
    int read_index;
    int write_index;

    if (provider->terminal_count <= 1)
        return;

    qsort(provider->terminals, provider->terminal_count,
          sizeof(graphid), compare_age_wcoj_graphids);
    write_index = 1;
    for (read_index = 1;
         read_index < provider->terminal_count;
         read_index++)
    {
        if (provider->terminals[read_index] !=
            provider->terminals[write_index - 1])
        {
            provider->terminals[write_index++] =
                provider->terminals[read_index];
        }
    }
    provider->terminal_count = write_index;
}

static AgeWCOJAdjacencyBuildResult
build_adjacency_provider_filtered(AgeWCOJPostingProvider *provider,
                                  const graphid *candidates,
                                  int candidate_count, int posting_limit)
{
    AgeWCOJJoinScanState *state = provider->owner;

    if (provider->built)
    {
        return provider->terminal_count > 0 ?
            AGE_WCOJ_ADJACENCY_BUILD_READY :
            AGE_WCOJ_ADJACENCY_BUILD_EMPTY;
    }
    if (candidates != NULL && candidate_count <= 0)
        return AGE_WCOJ_ADJACENCY_BUILD_EMPTY;
    if (posting_limit < 0)
        elog(ERROR, "invalid AGE WCOJ posting limit");

    set_adjacency_terminal_filter(provider, candidates, candidate_count);
    if (!materialize_adjacency_source_bags(provider))
    {
        provider->built = true;
        provider->exhausted = true;
        return AGE_WCOJ_ADJACENCY_BUILD_EMPTY;
    }
    if (!collect_adjacency_terminals(provider, posting_limit))
    {
        provider->has_current = false;
        provider->exhausted = false;
        provider->built = false;
        return AGE_WCOJ_ADJACENCY_BUILD_OVERSIZE;
    }

    deduplicate_adjacency_terminals(provider);
    provider->terminal_index = 0;
    provider->built = true;
    provider->exhausted = provider->terminal_count == 0;
    update_age_wcoj_peak_memory(state);
    if (!provider->exhausted)
    {
        provider->current_key = provider->terminals[0];
        provider->has_current = true;
    }
    return provider->exhausted ? AGE_WCOJ_ADJACENCY_BUILD_EMPTY :
        AGE_WCOJ_ADJACENCY_BUILD_READY;
}

static bool
build_adjacency_provider(AgeWCOJPostingProvider *provider)
{
    return build_adjacency_provider_filtered(provider, NULL, 0, 0) ==
        AGE_WCOJ_ADJACENCY_BUILD_READY;
}

static int
intersect_age_wcoj_graphids(graphid *candidates, int candidate_count,
                            const graphid *matches, int match_count)
{
    int candidate_index = 0;
    int match_index = 0;
    int output_count = 0;

    while (candidate_index < candidate_count && match_index < match_count)
    {
        graphid candidate = candidates[candidate_index];
        graphid match = matches[match_index];

        if (candidate < match)
        {
            candidate_index++;
        }
        else if (candidate > match)
        {
            match_index++;
        }
        else
        {
            candidates[output_count++] = candidate;
            candidate_index++;
            match_index++;
        }
    }
    return output_count;
}

static void
rescan_age_wcoj_providers(AgeWCOJJoinScanState *state)
{
    int source_index;

    for (source_index = 0; source_index < state->arity; source_index++)
    {
        state->providers[source_index].ops->rescan(
            &state->providers[source_index]);
    }
}

/*
 * Build a bounded adjacency seed and push its sorted exact terminal set into
 * every other direct adjacency provider.  The seed's terminal array is
 * compacted in place after each filter, so the existing progressive cursor
 * loop only probes plan-stream providers for terminals already known to
 * survive all direct adjacency sources.
 */
static bool
prepare_age_wcoj_progressive_exact_set(AgeWCOJJoinScanState *state)
{
    AgeWCOJPostingProvider *seed;
    AgeWCOJAdjacencyBuildResult build_result;
    int candidate_count;
    int seed_count;
    int filter_count = 0;
    int stage;

    if (state->progressive_setup_done)
        return true;
    state->progressive_setup_done = true;

    seed = &state->providers[state->seed_index];
    if (seed->kind != AGE_WCOJ_PROVIDER_ADJACENCY)
        return true;

    build_result = build_adjacency_provider_filtered(
        seed, NULL, 0, AGE_WCOJ_PROGRESSIVE_MAX_SEED_POSTINGS);
    if (build_result == AGE_WCOJ_ADJACENCY_BUILD_OVERSIZE)
    {
        rescan_age_wcoj_providers(state);
        if (state->requested_engine == AGE_WCOJ_ENGINE_AUTO)
        {
            state->actual_engine = AGE_WCOJ_ENGINE_MERGE;
            state->fallback_reason = "progressive seed exceeds exact-set cap";
        }
        return true;
    }
    if (build_result == AGE_WCOJ_ADJACENCY_BUILD_EMPTY)
        return false;

    candidate_count = seed->terminal_count;
    seed_count = candidate_count;
    for (stage = 1; stage < state->arity && candidate_count > 0; stage++)
    {
        AgeWCOJPostingProvider *provider =
            &state->providers[state->source_order[stage]];

        if (provider->kind != AGE_WCOJ_PROVIDER_ADJACENCY)
            continue;

        increment_wcoj_counter(&state->exact_set_filters);
        add_wcoj_counter(&state->exact_set_candidates_tested,
                         candidate_count);
        build_result = build_adjacency_provider_filtered(
            provider, seed->terminals, candidate_count, 0);
        filter_count++;
        if (build_result == AGE_WCOJ_ADJACENCY_BUILD_EMPTY)
        {
            candidate_count = 0;
        }
        else
        {
            candidate_count = intersect_age_wcoj_graphids(
                seed->terminals, candidate_count, provider->terminals,
                provider->terminal_count);
        }

        if (filter_count == 1 &&
            state->requested_engine == AGE_WCOJ_ENGINE_AUTO &&
            (int64)candidate_count *
                    AGE_WCOJ_PROGRESSIVE_DENSE_DENOMINATOR >=
                (int64)seed_count * AGE_WCOJ_PROGRESSIVE_DENSE_NUMERATOR)
        {
            rescan_age_wcoj_providers(state);
            state->actual_engine = AGE_WCOJ_ENGINE_MERGE;
            state->fallback_reason = "dense progressive exact-set overlap";
            return true;
        }
    }

    seed->terminal_count = candidate_count;
    seed->terminal_index = 0;
    seed->has_current = candidate_count > 0;
    seed->exhausted = candidate_count == 0;
    if (candidate_count > 0)
        seed->current_key = seed->terminals[0];
    update_age_wcoj_peak_memory(state);

    return candidate_count > 0;
}

static bool
adjacency_first_distinct(AgeWCOJPostingProvider *provider)
{
    if (!provider->built && !build_adjacency_provider(provider))
        return false;
    if (provider->exhausted || provider->terminal_index >=
        provider->terminal_count)
    {
        provider->has_current = false;
        return false;
    }
    provider->current_key = provider->terminals[provider->terminal_index];
    provider->has_current = true;
    return true;
}

static bool
adjacency_next_distinct(AgeWCOJPostingProvider *provider)
{
    if (!adjacency_first_distinct(provider))
        return false;
    provider->terminal_index++;
    record_provider_advance(provider);
    if (provider->terminal_index >= provider->terminal_count)
    {
        provider->has_current = false;
        provider->exhausted = true;
        return false;
    }
    provider->current_key = provider->terminals[provider->terminal_index];
    return true;
}

static bool
adjacency_seek_ge(AgeWCOJPostingProvider *provider, graphid target)
{
    int low;
    int high;

    increment_wcoj_counter(&provider->owner->seek_calls);
    if (!adjacency_first_distinct(provider))
        return false;
    if (provider->current_key >= target)
        return true;

    low = provider->terminal_index + 1;
    high = provider->terminal_count;
    while (low < high)
    {
        int middle = low + (high - low) / 2;

        if (provider->terminals[middle] < target)
            low = middle + 1;
        else
            high = middle;
    }
    record_provider_advance(provider);
    provider->terminal_index = low;
    if (low >= provider->terminal_count)
    {
        provider->has_current = false;
        provider->exhausted = true;
        return false;
    }
    provider->current_key = provider->terminals[low];
    return true;
}


static void
fill_age_wcoj_adjacency_output(AgeWCOJPostingProvider *provider,
                               int bag_index, int source_row_index,
                               const AgeAdjacencyPayload *payload)
{
    TupleTableSlot *slot = provider->output_slot;
    AgeWCOJSourceKeyBag *bag = &provider->source_bags[bag_index];
    int index;

    ExecClearTuple(slot);
    ExecStoreMinimalTuple(provider->source_rows[source_row_index].tuple,
                          provider->source_slot, false);
    slot_getallattrs(provider->source_slot);

    for (index = 0; index < provider->output_width; index++)
    {
        int map = provider->output_map[index];

        if (map > 0)
        {
            slot->tts_values[index] = slot_getattr(provider->source_slot,
                                                   map,
                                                   &slot->tts_isnull[index]);
        }
        else if (map < 0)
        {
            AttrNumber attno = (AttrNumber)(-map);
            graphid start_id;
            graphid end_id;

            if (provider->outgoing)
            {
                start_id = bag->key;
                end_id = payload->next_vertex_id;
            }
            else
            {
                start_id = payload->next_vertex_id;
                end_id = bag->key;
            }
            slot->tts_isnull[index] = false;
            switch (attno)
            {
            case Anum_ag_label_edge_table_id:
                slot->tts_values[index] =
                    GRAPHID_GET_DATUM(payload->edge_id);
                break;
            case Anum_ag_label_edge_table_start_id:
                slot->tts_values[index] = GRAPHID_GET_DATUM(start_id);
                break;
            case Anum_ag_label_edge_table_end_id:
                slot->tts_values[index] = GRAPHID_GET_DATUM(end_id);
                break;
            case Anum_ag_label_edge_table_properties:
                slot->tts_values[index] = payload->properties;
                slot->tts_isnull[index] = payload->properties_isnull;
                break;
            default:
                elog(ERROR, "invalid AGE WCOJ edge output attribute %d",
                     attno);
            }
        }
        else
        {
            elog(ERROR, "invalid AGE WCOJ output mapping");
        }
    }
    ExecStoreVirtualTuple(slot);
}

static bool
age_wcoj_adjacency_local_qual(AgeWCOJPostingProvider *provider)
{
    AgeWCOJJoinScanState *state = provider->owner;
    TupleTableSlot *raw_slot;
    ExprContext *econtext;
    TupleTableSlot *saved_scan_tuple;
    int index;
    bool result;

    if (provider->local_qual == NULL)
        return true;

    raw_slot = state->css.ss.ss_ScanTupleSlot;
    ExecClearTuple(raw_slot);
    memset(raw_slot->tts_values, 0,
           sizeof(Datum) * raw_slot->tts_tupleDescriptor->natts);
    memset(raw_slot->tts_isnull, true,
           sizeof(bool) * raw_slot->tts_tupleDescriptor->natts);
    slot_getallattrs(provider->output_slot);
    for (index = 0; index < provider->output_width; index++)
    {
        int raw_index = provider->output_offset - 1 + index;

        raw_slot->tts_values[raw_index] =
            provider->output_slot->tts_values[index];
        raw_slot->tts_isnull[raw_index] =
            provider->output_slot->tts_isnull[index];
    }
    ExecStoreVirtualTuple(raw_slot);

    econtext = state->css.ss.ps.ps_ExprContext;
    saved_scan_tuple = econtext->ecxt_scantuple;
    econtext->ecxt_scantuple = raw_slot;
    result = ExecQual(provider->local_qual, econtext);
    econtext->ecxt_scantuple = saved_scan_tuple;
    return result;
}

static void
materialize_age_wcoj_adjacency_payload(
    AgeWCOJPostingProvider *provider, int bag_index,
    const AgeAdjacencyPayload *payload)
{
    AgeWCOJJoinScanState *state = provider->owner;
    AgeWCOJSourceKeyBag *bag = &provider->source_bags[bag_index];
    int source_row_index = bag->first_row;

    fill_age_wcoj_adjacency_output(provider, bag_index,
                                   source_row_index, payload);
    if (!age_wcoj_adjacency_local_qual(provider))
    {
        increment_wcoj_counter(&state->local_predicate_rejects);
        return;
    }

    for (source_row_index = bag->first_row;
         source_row_index < bag->first_row + bag->row_count;
         source_row_index++)
    {
        fill_age_wcoj_adjacency_output(provider, bag_index,
                                       source_row_index, payload);
        append_age_wcoj_group_tuple(state, provider->source_index,
                                    provider->output_slot, true,
                                    payload->edge_id);
    }
}

static bool
adjacency_materialize_group(AgeWCOJPostingProvider *provider,
                            graphid terminal)
{
    AgeWCOJJoinScanState *state = provider->owner;
    int source_index = provider->source_index;
    int bag_index;

    if (state->batch_payload_enabled)
    {
        AgeWCOJBatchPayloadRange *range;
        int payload_index;

        if (provider->batch_payload_ranges == NULL ||
            state->active_survivor_index < 0 ||
            state->active_survivor_index >= state->survivor_block_count)
        {
            elog(ERROR, "invalid AGE WCOJ buffered adjacency group");
        }
        range = &provider->batch_payload_ranges[
            state->active_survivor_index];
        for (payload_index = range->first_payload;
             payload_index < range->first_payload + range->payload_count;
             payload_index++)
        {
            AgeWCOJBatchPayload *entry =
                &provider->batch_payloads[payload_index];

            materialize_age_wcoj_adjacency_payload(
                provider, entry->source_bag_index, &entry->payload);
        }
        return state->groups[source_index].count > 0;
    }

    if (!adjacency_first_distinct(provider) ||
        provider->current_key != terminal)
    {
        return false;
    }

    for (bag_index = 0; bag_index < provider->source_bag_count; bag_index++)
    {
        AgeWCOJSourceKeyBag *bag = &provider->source_bags[bag_index];
        AgeAdjacencyPayload payload;
        bool active;

        active = age_adjacency_visible_payload_scan_begin_key(
            provider->payload_scan, bag->key);
        increment_wcoj_counter(&state->payload_scan_batches);
        increment_wcoj_counter(&state->payload_source_keys_scanned);
        while (active && age_adjacency_visible_payload_scan_next(
                             provider->payload_scan, &payload))
        {
            increment_wcoj_counter(&state->payload_rows_scanned);
            if (payload.next_vertex_id != terminal)
                continue;
            increment_wcoj_counter(&state->payload_rows_matched);
            increment_wcoj_counter(&state->payload_rows_fetched);
            materialize_age_wcoj_adjacency_payload(
                provider, bag_index, &payload);
        }
    }

    (void)adjacency_next_distinct(provider);
    return state->groups[source_index].count > 0;
}

static void
adjacency_rescan(AgeWCOJPostingProvider *provider)
{
    ExecReScan(provider->plan_state);
    if (provider->terminal_scan != NULL)
    {
        age_adjacency_visible_payload_scan_set_terminal_vertex_set_filter(
            provider->terminal_scan, NULL);
        age_adjacency_visible_payload_scan_reset_runtime(
            provider->terminal_scan);
    }
    if (provider->payload_scan != NULL)
        age_adjacency_visible_payload_scan_reset_runtime(
            provider->payload_scan);
    if (provider->provider_context != NULL)
        MemoryContextReset(provider->provider_context);
    provider->source_rows = NULL;
    provider->source_row_count = 0;
    provider->source_row_capacity = 0;
    provider->source_bags = NULL;
    provider->source_bag_count = 0;
    provider->terminals = NULL;
    provider->terminal_count = 0;
    provider->terminal_capacity = 0;
    provider->terminal_index = 0;
    provider->has_current = false;
    provider->exhausted = false;
    provider->built = false;
}

static void
adjacency_close(AgeWCOJPostingProvider *provider)
{
    if (provider->terminal_scan != NULL)
    {
        age_adjacency_end_visible_payload_scan(provider->terminal_scan);
        provider->terminal_scan = NULL;
    }
    if (provider->payload_scan != NULL)
    {
        age_adjacency_end_visible_payload_scan(provider->payload_scan);
        provider->payload_scan = NULL;
    }
    if (provider->plan_state != NULL)
        ExecEndNode(provider->plan_state);
    if (provider->provider_context != NULL)
    {
        MemoryContextDelete(provider->provider_context);
        provider->provider_context = NULL;
    }
    provider->has_current = false;
    provider->exhausted = true;
}

static bool
initialize_age_wcoj_providers(AgeWCOJJoinScanState *state)
{
    int source_index;

    for (source_index = 0; source_index < state->arity; source_index++)
    {
        if (!state->providers[source_index].ops->first_distinct(
                &state->providers[source_index]))
        {
            return false;
        }
    }
    return true;
}

/*
 * Leapfrog alignment over the provider contract.  PLAN_STREAM_PROVIDER
 * implements seek_ge() by consuming sorted groups; ADJACENCY_PROVIDER can
 * later implement the same call as a true index cursor seek.
 */
static bool
align_age_wcoj_leapfrog(AgeWCOJJoinScanState *state, graphid *matched_key)
{
    graphid max_key;
    bool aligned;
    int source_index;

    if (!initialize_age_wcoj_providers(state))
        return false;

    for (;;)
    {
        max_key = state->providers[0].current_key;
        for (source_index = 1; source_index < state->arity; source_index++)
        {
            if (state->providers[source_index].current_key > max_key)
                max_key = state->providers[source_index].current_key;
        }

        aligned = true;
        for (source_index = 0; source_index < state->arity; source_index++)
        {
            AgeWCOJPostingProvider *provider =
                &state->providers[source_index];

            if (provider->current_key < max_key &&
                !provider->ops->seek_ge(provider, max_key))
            {
                return false;
            }
            if (provider->current_key != max_key)
                aligned = false;
        }

        if (aligned)
        {
            *matched_key = max_key;
            return true;
        }
    }
}

/*
 * Dense streaming merge.  It never seeks ahead to a maximum key; instead it
 * discards the current minimum terminal group from every source that owns it.
 */
static bool
align_age_wcoj_merge(AgeWCOJJoinScanState *state, graphid *matched_key)
{
    graphid min_key;
    graphid max_key;
    int source_index;

    if (!initialize_age_wcoj_providers(state))
        return false;

    for (;;)
    {
        min_key = state->providers[0].current_key;
        max_key = min_key;
        for (source_index = 1; source_index < state->arity; source_index++)
        {
            graphid key = state->providers[source_index].current_key;

            if (key < min_key)
                min_key = key;
            if (key > max_key)
                max_key = key;
        }
        if (min_key == max_key)
        {
            *matched_key = min_key;
            return true;
        }

        for (source_index = 0; source_index < state->arity; source_index++)
        {
            AgeWCOJPostingProvider *provider =
                &state->providers[source_index];

            if (provider->current_key == min_key &&
                !provider->ops->next_distinct(provider))
            {
                return false;
            }
        }
    }
}

/*
 * Progressive seed-driven filtering.  The smallest estimated source supplies
 * candidate terminals and the remaining providers filter them in increasing
 * estimated-posting order.  With PLAN_STREAM_PROVIDER this is a streaming
 * exact-key probe; the same control flow can use adjacency exact-set probes
 * when that provider is introduced.
 */
static bool
align_age_wcoj_progressive(AgeWCOJJoinScanState *state,
                           graphid *matched_key)
{
    AgeWCOJPostingProvider *seed = &state->providers[state->seed_index];

    if (!prepare_age_wcoj_progressive_exact_set(state))
        return false;
    if (state->actual_engine != AGE_WCOJ_ENGINE_PROGRESSIVE)
        return align_age_wcoj_merge(state, matched_key);
    if (!initialize_age_wcoj_providers(state))
        return false;

    for (;;)
    {
        graphid candidate = seed->current_key;
        bool restart = false;
        int stage;

        increment_wcoj_counter(&state->progressive_probes);
        increment_wcoj_counter(&state->stage_survivors[0]);

        for (stage = 1; stage < state->arity; stage++)
        {
            AgeWCOJPostingProvider *provider =
                &state->providers[state->source_order[stage]];

            if (!provider->ops->seek_ge(provider, candidate))
                return false;
            if (provider->current_key > candidate)
            {
                if (!seed->ops->seek_ge(seed, provider->current_key))
                    return false;
                restart = true;
                break;
            }
            increment_wcoj_counter(&state->stage_survivors[stage]);
        }
        if (restart)
            continue;

        increment_wcoj_counter(&state->progressive_matches);
        *matched_key = candidate;
        return true;
    }
}

static bool
collect_age_wcoj_group(AgeWCOJJoinScanState *state, graphid matched_key)
{
    int source_index;
    bool all_nonempty = true;

    for (source_index = 0; source_index < state->arity; source_index++)
    {
        AgeWCOJPostingProvider *provider =
            &state->providers[source_index];

        if (!provider->ops->materialize_group(provider, matched_key))
            all_nonempty = false;
    }

    if (!all_nonempty)
        return false;

    increment_wcoj_counter(&state->groups_matched);

    /*
     * Enumerate the smallest surviving edge bag first.  This makes an edge-id
     * conflict cut off the largest possible suffix of the Cartesian product.
     * The output tuple is still assembled in provider order, so this ordering
     * is an execution detail and cannot change bag semantics.
     */
    for (source_index = 0; source_index < state->arity; source_index++)
    {
        state->enumeration_order[source_index] = source_index;
        state->combination_indexes[source_index] = -1;
        state->selected_edge_id_valid[source_index] = false;
    }
    for (source_index = 1; source_index < state->arity; source_index++)
    {
        int provider_index = state->enumeration_order[source_index];
        int position = source_index;

        while (position > 0 &&
               state->groups[state->enumeration_order[position - 1]].count >
                   state->groups[provider_index].count)
        {
            state->enumeration_order[position] =
                state->enumeration_order[position - 1];
            position--;
        }
        state->enumeration_order[position] = provider_index;
    }
    state->enumeration_started = false;
    update_age_wcoj_peak_memory(state);
    state->group_active = true;
    return true;
}

static void
maybe_fallback_from_progressive(AgeWCOJJoinScanState *state)
{
    if (state->requested_engine != AGE_WCOJ_ENGINE_AUTO ||
        state->actual_engine != AGE_WCOJ_ENGINE_PROGRESSIVE ||
        state->progressive_probes < AGE_WCOJ_PROGRESSIVE_DENSE_SAMPLE)
    {
        return;
    }
    if (state->progressive_matches *
            AGE_WCOJ_PROGRESSIVE_DENSE_DENOMINATOR >=
        state->progressive_probes * AGE_WCOJ_PROGRESSIVE_DENSE_NUMERATOR)
    {
        state->actual_engine = AGE_WCOJ_ENGINE_MERGE;
        state->fallback_reason = "dense progressive overlap";
    }
}

static bool
find_next_age_wcoj_survivor(AgeWCOJJoinScanState *state,
                            graphid *matched_key)
{
    maybe_fallback_from_progressive(state);
    switch (state->actual_engine)
    {
    case AGE_WCOJ_ENGINE_PROGRESSIVE:
        return align_age_wcoj_progressive(state, matched_key);
    case AGE_WCOJ_ENGINE_LEAPFROG:
        return align_age_wcoj_leapfrog(state, matched_key);
    case AGE_WCOJ_ENGINE_MERGE:
        return align_age_wcoj_merge(state, matched_key);
    case AGE_WCOJ_ENGINE_AUTO:
    default:
        elog(ERROR, "invalid AGE WCOJ execution engine");
        return false;
    }
}

static bool
buffer_age_wcoj_survivor(AgeWCOJJoinScanState *state, graphid matched_key,
                         int survivor_index)
{
    bool can_continue = true;
    int source_index;

    state->survivor_block[survivor_index] = matched_key;
    state->survivor_block_count = survivor_index + 1;
    for (source_index = 0; source_index < state->arity; source_index++)
    {
        AgeWCOJPostingProvider *provider = &state->providers[source_index];

        if (!provider->has_current || provider->current_key != matched_key)
        {
            elog(ERROR, "AGE WCOJ provider lost an aligned survivor");
        }
        if (provider->kind == AGE_WCOJ_PROVIDER_PLAN_STREAM)
        {
            if (!plan_stream_buffer_group(provider, matched_key,
                                          survivor_index))
            {
                elog(ERROR, "AGE WCOJ could not buffer a plan-stream group");
            }
            if (provider->exhausted)
                can_continue = false;
        }
        else if (!provider->ops->next_distinct(provider))
        {
            can_continue = false;
        }
    }
    return can_continue;
}

static bool
prepare_age_wcoj_survivor_block(AgeWCOJJoinScanState *state)
{
    graphid matched_key;
    bool can_continue;
    int capacity;
    int source_index;

    reset_age_wcoj_survivor_block(state);
    if (state->survivor_input_exhausted ||
        !find_next_age_wcoj_survivor(state, &matched_key))
    {
        state->survivor_input_exhausted = true;
        return false;
    }

    capacity = age_wcoj_survivor_block_capacity(state);
    allocate_age_wcoj_survivor_block(state, capacity);
    for (;;)
    {
        int survivor_index = state->survivor_block_count;

        can_continue = buffer_age_wcoj_survivor(
            state, matched_key, survivor_index);
        if (!can_continue)
        {
            state->survivor_input_exhausted = true;
            break;
        }
        if (state->survivor_block_count >= capacity ||
            MemoryContextMemAllocated(state->batch_context, true) >=
                state->batch_memory_budget)
        {
            break;
        }
        if (!find_next_age_wcoj_survivor(state, &matched_key))
        {
            state->survivor_input_exhausted = true;
            break;
        }
    }

    for (source_index = 0; source_index < state->arity; source_index++)
    {
        AgeWCOJPostingProvider *provider = &state->providers[source_index];

        if (provider->kind == AGE_WCOJ_PROVIDER_ADJACENCY)
            prefetch_age_wcoj_adjacency_payloads(provider);
    }
    increment_wcoj_counter(&state->survivor_blocks);
    state->survivor_block_index = 0;
    update_age_wcoj_peak_memory(state);
    return state->survivor_block_count > 0;
}

static bool
prepare_age_wcoj_group(AgeWCOJJoinScanState *state)
{
    for (;;)
    {
        graphid matched_key;

        clear_age_wcoj_group(state);
        if (state->exhausted)
            return false;

        if (state->batch_payload_enabled)
        {
            if (state->survivor_block_index >= state->survivor_block_count &&
                !prepare_age_wcoj_survivor_block(state))
            {
                state->exhausted = true;
                return false;
            }
            state->active_survivor_index = state->survivor_block_index;
            matched_key = state->survivor_block[
                state->survivor_block_index++];
        }
        else if (!find_next_age_wcoj_survivor(state, &matched_key))
        {
            state->exhausted = true;
            return false;
        }
        if (collect_age_wcoj_group(state, matched_key))
            return true;
    }
}

static bool
next_age_wcoj_combination(AgeWCOJJoinScanState *state)
{
    int depth;

    if (!state->group_active)
        return false;

    /* Resume at the leaf after an emitted row; otherwise start at the root. */
    depth = state->enumeration_started ? state->arity - 1 : 0;
    state->enumeration_started = true;

    while (depth >= 0)
    {
        int source_index = state->enumeration_order[depth];
        AgeWCOJTupleGroup *group = &state->groups[source_index];
        int tuple_index = state->combination_indexes[source_index] + 1;

        state->selected_edge_id_valid[depth] = false;
        while (tuple_index < group->count)
        {
            bool conflict = false;
            int previous_depth;

            state->combination_indexes[source_index] = tuple_index;
            if (group->edge_id_valid[tuple_index])
            {
                graphid edge_id = group->edge_ids[tuple_index];

                for (previous_depth = 0;
                     previous_depth < depth;
                     previous_depth++)
                {
                    int previous_source =
                        state->enumeration_order[previous_depth];

                    if (state->selected_edge_id_valid[previous_depth] &&
                        state->selected_edge_ids[previous_depth] == edge_id &&
                        bms_overlap(state->uniqueness_groups[source_index],
                                    state->uniqueness_groups[previous_source]))
                    {
                        conflict = true;
                        break;
                    }
                }
                if (conflict)
                {
                    increment_wcoj_counter(&state->uniqueness_rejects);
                    tuple_index++;
                    continue;
                }
                state->selected_edge_ids[depth] = edge_id;
                state->selected_edge_id_valid[depth] = true;
            }

            if (depth == state->arity - 1)
            {
                increment_wcoj_counter(&state->candidate_combinations);
                return true;
            }

            depth++;
            source_index = state->enumeration_order[depth];
            state->combination_indexes[source_index] = -1;
            state->selected_edge_id_valid[depth] = false;
            break;
        }

        if (tuple_index < group->count)
            continue;

        state->combination_indexes[source_index] = -1;
        state->selected_edge_id_valid[depth] = false;
        depth--;
    }

    state->group_active = false;
    state->enumeration_started = false;
    return false;
}

static TupleTableSlot *
materialize_age_wcoj_combination(AgeWCOJJoinScanState *state)
{
    TupleTableSlot *raw_slot = state->css.ss.ss_ScanTupleSlot;
    int raw_index;
    int child_index;

    ExecClearTuple(raw_slot);
    raw_index = 0;
    for (child_index = 0; child_index < state->arity; child_index++)
    {
        TupleTableSlot *group_slot = state->group_slots[child_index];
        MinimalTuple tuple;
        int attr_index;

        tuple = state->groups[child_index].tuples[
            state->combination_indexes[child_index]];
        ExecStoreMinimalTuple(tuple, group_slot, false);
        slot_getallattrs(group_slot);
        for (attr_index = 0;
             attr_index < group_slot->tts_tupleDescriptor->natts;
             attr_index++)
        {
            if (raw_index >= raw_slot->tts_tupleDescriptor->natts)
                elog(ERROR, "AGE WCOJ raw tuple width overflow");
            raw_slot->tts_values[raw_index] = group_slot->tts_values[attr_index];
            raw_slot->tts_isnull[raw_index] =
                group_slot->tts_isnull[attr_index];
            raw_index++;
        }
    }
    if (raw_index != raw_slot->tts_tupleDescriptor->natts)
    {
        elog(ERROR, "AGE WCOJ raw tuple width mismatch: %d versus %d",
             raw_index, raw_slot->tts_tupleDescriptor->natts);
    }
    ExecStoreVirtualTuple(raw_slot);

    return raw_slot;
}

static TupleTableSlot *
access_age_wcoj_join_scan(ScanState *node)
{
    AgeWCOJJoinScanState *state = (AgeWCOJJoinScanState *)node;

    for (;;)
    {
        if (!state->group_active && !prepare_age_wcoj_group(state))
            return ExecClearTuple(node->ss_ScanTupleSlot);
        if (next_age_wcoj_combination(state))
            return materialize_age_wcoj_combination(state);
    }
}

static bool
recheck_age_wcoj_join_scan(ScanState *node, TupleTableSlot *slot)
{
    (void)node;
    (void)slot;
    return true;
}

static void
end_age_wcoj_join_scan(CustomScanState *node)
{
    AgeWCOJJoinScanState *state = (AgeWCOJJoinScanState *)node;
    int source_index;

    clear_age_wcoj_group(state);
    /*
     * group_slots were allocated with ExecInitExtraTupleSlot(), so they are
     * owned by the estate tuple table and released by ExecutorEnd.  Dropping
     * them here would leave dangling entries for ExecResetTupleTable().
     */
    for (source_index = 0; source_index < state->arity; source_index++)
        state->providers[source_index].ops->close(
            &state->providers[source_index]);
    if (state->group_context != NULL)
    {
        MemoryContextDelete(state->group_context);
        state->group_context = NULL;
    }
    if (state->batch_context != NULL)
    {
        MemoryContextDelete(state->batch_context);
        state->batch_context = NULL;
    }
}

static void
rescan_age_wcoj_join_scan(CustomScanState *node)
{
    AgeWCOJJoinScanState *state = (AgeWCOJJoinScanState *)node;
    int source_index;

    clear_age_wcoj_group(state);
    for (source_index = 0; source_index < state->arity; source_index++)
        state->providers[source_index].ops->rescan(
            &state->providers[source_index]);
    reset_age_wcoj_survivor_block(state);
    state->actual_engine = state->planned_engine;
    state->fallback_reason = "none";
    state->exhausted = false;
    state->survivor_input_exhausted = false;
    state->progressive_setup_done = false;
    increment_wcoj_counter(&state->rescans);
}

static const char *
age_wcoj_algorithm_name(AgeWCOJEngineKind engine)
{
    switch (engine)
    {
    case AGE_WCOJ_ENGINE_PROGRESSIVE:
        return "progressive seed filtering with delayed bag product";
    case AGE_WCOJ_ENGINE_LEAPFROG:
        return "provider leapfrog intersection with delayed bag product";
    case AGE_WCOJ_ENGINE_MERGE:
        return "streaming merge intersection with delayed bag product";
    case AGE_WCOJ_ENGINE_AUTO:
    default:
        return "unknown";
    }
}

static char *
format_estimated_postings(AgeWCOJJoinScanState *state)
{
    StringInfoData buffer;
    int source_index;

    initStringInfo(&buffer);
    for (source_index = 0; source_index < state->arity; source_index++)
    {
        appendStringInfo(&buffer, "%s%d=%.0f",
                         source_index == 0 ? "" : ", ", source_index + 1,
                         state->estimated_postings[source_index]);
    }
    return buffer.data;
}

static char *
format_observed_postings(AgeWCOJJoinScanState *state)
{
    StringInfoData buffer;
    int source_index;

    initStringInfo(&buffer);
    for (source_index = 0; source_index < state->arity; source_index++)
    {
        appendStringInfo(&buffer, "%s%d=" INT64_FORMAT,
                         source_index == 0 ? "" : ", ", source_index + 1,
                         state->providers[source_index].rows_scanned);
    }
    return buffer.data;
}

static char *
format_stage_survivors(AgeWCOJJoinScanState *state)
{
    StringInfoData buffer;
    int stage;

    initStringInfo(&buffer);
    for (stage = 0; stage < state->arity; stage++)
    {
        appendStringInfo(&buffer, "%s%d=" INT64_FORMAT,
                         stage == 0 ? "" : ", ", stage + 1,
                         state->stage_survivors[stage]);
    }
    return buffer.data;
}

static void
explain_age_wcoj_join_scan(CustomScanState *node, List *ancestors,
                           ExplainState *es)
{
    AgeWCOJJoinScanState *state = (AgeWCOJJoinScanState *)node;
    char *estimated_postings;
    int adjacency_provider_count = 0;
    int source_index;

    (void)ancestors;
    for (source_index = 0; source_index < state->arity; source_index++)
    {
        if (state->providers[source_index].kind ==
            AGE_WCOJ_PROVIDER_ADJACENCY)
            adjacency_provider_count++;
    }
    estimated_postings = format_estimated_postings(state);
    ExplainPropertyText("WCOJ Algorithm",
                        age_wcoj_algorithm_name(state->actual_engine), es);
    ExplainPropertyText("Planned Engine",
                        age_wcoj_engine_name(state->planned_engine), es);
    ExplainPropertyText("Actual Engine",
                        age_wcoj_engine_name(state->actual_engine), es);
    ExplainPropertyText("Fallback Reason", state->fallback_reason, es);
    ExplainPropertyInteger("Source Count", NULL, state->arity, es);
    ExplainPropertyInteger("Adjacency Providers", NULL,
                           adjacency_provider_count, es);
    ExplainPropertyBool("Batched Payload Materialization",
                        state->batch_payload_enabled, es);
    ExplainPropertyInteger("WCOJ Arity", NULL, state->arity, es);
    ExplainPropertyInteger("Seed Source", NULL, state->seed_index + 1, es);
    ExplainPropertyText("Estimated Postings", estimated_postings, es);
    pfree(estimated_postings);
    if (es->analyze)
    {
        char *observed_postings = format_observed_postings(state);
        char *stage_survivors = format_stage_survivors(state);

        ExplainPropertyText("Observed Postings", observed_postings, es);
        ExplainPropertyText("Stage Survivors", stage_survivors, es);
        ExplainPropertyInteger("Cursor Advances", NULL,
                               state->cursor_advances, es);
        ExplainPropertyInteger("Seek Calls", NULL, state->seek_calls, es);
        ExplainPropertyInteger("Posting Rows Scanned", NULL,
                               state->posting_rows_scanned, es);
        ExplainPropertyInteger("Survivor Blocks", NULL,
                               state->survivor_blocks, es);
        ExplainPropertyInteger("Payload Scan Batches", NULL,
                               state->payload_scan_batches, es);
        ExplainPropertyInteger("Payload Source Keys Scanned", NULL,
                               state->payload_source_keys_scanned, es);
        ExplainPropertyInteger("Payload Scan Restarts Avoided", NULL,
                               state->payload_scan_restarts_avoided, es);
        ExplainPropertyInteger("Payload Rows Scanned", NULL,
                               state->payload_rows_scanned, es);
        ExplainPropertyInteger("Payload Rows Matched", NULL,
                               state->payload_rows_matched, es);
        ExplainPropertyInteger("Payload Rows Fetched", NULL,
                               state->payload_rows_fetched, es);
        ExplainPropertyInteger("Progressive Probes", NULL,
                               state->progressive_probes, es);
        ExplainPropertyInteger("Progressive Matches", NULL,
                               state->progressive_matches, es);
        ExplainPropertyInteger("Exact Set Filters", NULL,
                               state->exact_set_filters, es);
        ExplainPropertyInteger("Exact Set Candidates Tested", NULL,
                               state->exact_set_candidates_tested, es);
        ExplainPropertyInteger("Source Rows Scanned", NULL,
                               state->source_rows_scanned, es);
        ExplainPropertyInteger("Distinct Source Keys", NULL,
                               state->distinct_source_keys, es);
        ExplainPropertyInteger("Intersection Builds", NULL,
                               state->intersection_builds, es);
        ExplainPropertyInteger("Local Predicate Rejects", NULL,
                               state->local_predicate_rejects, es);
        ExplainPropertyInteger("Matched Terminal Groups", NULL,
                               state->groups_matched, es);
        ExplainPropertyInteger("Candidate Bag Combinations", NULL,
                               state->candidate_combinations, es);
        ExplainPropertyInteger("Uniqueness Constraint Groups", NULL,
                               state->uniqueness_group_count, es);
        ExplainPropertyInteger("Uniqueness Rejects", NULL,
                               state->uniqueness_rejects, es);
        ExplainPropertyInteger("Rows Emitted", NULL, state->rows_emitted, es);
        ExplainPropertyInteger("Rescans", NULL, state->rescans, es);
        ExplainPropertyInteger("Peak WCOJ Memory", "bytes",
                               (int64)state->peak_memory, es);
        pfree(observed_postings);
        pfree(stage_survivors);
    }
}
