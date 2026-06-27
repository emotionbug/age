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
#include "executor/cypher_generic_join.h"
#include "executor/executor.h"
#include "lib/stringinfo.h"
#include "nodes/pg_list.h"
#include "utils/graphid.h"
#include "utils/memutils.h"

#define AGE_GENERIC_GALLOPING_MIN_RATIO 8
#define AGE_GENERIC_PREFIX_RANGE_CACHE_SIZE 8
#define AGE_GENERIC_PREFIX_RANGE_DIRECTORY_MIN_MISSES 8

typedef struct AgeGenericRow
{
    graphid key1;
    graphid key2;
    graphid edge_id;
    bool edge_id_valid;
    MinimalTuple tuple;
} AgeGenericRow;

typedef struct AgeGenericPrefixRange
{
    graphid key;
    int start;
    int end;
    bool valid;
} AgeGenericPrefixRange;

typedef struct AgeGenericTrieRange
{
    int start;
    int end;
} AgeGenericTrieRange;

typedef struct AgeGenericChildRangeCacheEntry
{
    graphid key;
    int prefix_count;
    AgeGenericTrieRange range;
    bool valid;
} AgeGenericChildRangeCacheEntry;

typedef struct AgeGenericJoinState AgeGenericJoinState;
typedef struct AgeGenericProvider AgeGenericProvider;

typedef struct AgeGenericTrieOps
{
    const char *name;
    bool (*open_child_range)(AgeGenericJoinState *state,
                             AgeGenericProvider *provider,
                             const graphid *prefix_keys, int prefix_count,
                             AgeGenericTrieRange *range);
} AgeGenericTrieOps;

typedef enum AgeGenericOutputSource
{
    AGE_GENERIC_OUTPUT_SOURCE_NONE = 0,
    AGE_GENERIC_OUTPUT_SOURCE_KEY1,
    AGE_GENERIC_OUTPUT_SOURCE_KEY2,
    AGE_GENERIC_OUTPUT_SOURCE_EDGE_ID
} AgeGenericOutputSource;

struct AgeGenericProvider
{
    AgeGenericProviderKind kind;
    const AgeGenericTrieOps *trie_ops;
    int var1;
    int var2;
    AttrNumber key1_attno;
    AttrNumber key2_attno;
    AttrNumber edge_id_attno;
    PlanState *plan_state;
    TupleTableSlot *tuple_slot;
    AgeGenericOutputSource *output_sources;
    int output_width;
    bool key_only_output;
    AgeGenericRow *rows;
    int row_count;
    int row_capacity;
    int bag_start;
    int bag_count;
    AgeGenericPrefixRange *prefix_ranges;
    int prefix_range_count;
    bool prefix_ranges_valid;
    AgeGenericPrefixRange prefix_range_cache[
        AGE_GENERIC_PREFIX_RANGE_CACHE_SIZE];
    int prefix_range_cache_count;
    int prefix_range_misses;
    graphid prefix_range_cursor_key;
    int prefix_range_cursor_start;
    int prefix_range_cursor_end;
    bool prefix_range_cursor_valid;
    AgeGenericChildRangeCacheEntry child_range_cache[
        AGE_GENERIC_PREFIX_RANGE_CACHE_SIZE];
    int child_range_cache_count;
};

typedef struct AgeGenericLevel
{
    MemoryContext context;
    graphid *candidates;
    graphid *candidate_buffer;
    int candidate_capacity;
    int candidate_count;
    int next_candidate;
    bool initialized;
} AgeGenericLevel;

typedef struct AgeGenericDomainView
{
    AgeGenericProvider *provider;
    int variable;
    int start;
    int end;
    bool use_key2;
} AgeGenericDomainView;

typedef struct AgeGenericDomain
{
    graphid *keys;
    int count;
} AgeGenericDomain;

typedef struct AgeGenericDomainScratch
{
    graphid *keys;
    int capacity;
} AgeGenericDomainScratch;

typedef struct AgeGenericSeparatorDomain
{
    graphid *keys;
    int count;
    bool initialized;
} AgeGenericSeparatorDomain;

typedef struct AgeGenericGHDSeparatorPlan
{
    int provider_index;
    int separator_variable;
    int tail_variable;
    bool separator_is_key1;
} AgeGenericGHDSeparatorPlan;

typedef enum AgeGenericGHDSeparatorDescField
{
    AGE_GENERIC_GHD_SEPARATOR_DESC_ID = 0,
    AGE_GENERIC_GHD_SEPARATOR_DESC_PARENT_BAG,
    AGE_GENERIC_GHD_SEPARATOR_DESC_CHILD_BAG,
    AGE_GENERIC_GHD_SEPARATOR_DESC_SEPARATOR_VARIABLE,
    AGE_GENERIC_GHD_SEPARATOR_DESC_TAIL_VARIABLE,
    AGE_GENERIC_GHD_SEPARATOR_DESC_PROVIDER_INDEX,
    AGE_GENERIC_GHD_SEPARATOR_DESC_SEPARATOR_IS_KEY1,
    AGE_GENERIC_GHD_SEPARATOR_DESC_COUNT
} AgeGenericGHDSeparatorDescField;

struct AgeGenericJoinState
{
    CustomScanState css;
    int variable_count;
    Index *variable_rtis;
    int provider_count;
    AgeGenericProvider *providers;
    AgeGenericLevel *levels;
    graphid *bindings;
    bool search_started;
    bool binding_ready;
    int *combination_indexes;
    int *enumeration_order;
    graphid *selected_edge_ids;
    bool *selected_edge_id_valid;
    Bitmapset **uniqueness_groups;
    int uniqueness_group_count;
    AgeGenericDomainScratch reduction_scratch;
    AgeGenericReductionShape reduction_shape;
    int reduction_core_variables;
    int reduction_tail_separators;
    AgeGenericReductionOrderKind reduction_order_kind;
    AgeGenericReductionDescriptorSource reduction_descriptor_source;
    int *reduction_order_provider_indexes;
    int reduction_order_edge_count;
    int reduction_ghd_bag_count;
    int reduction_ghd_separator_count;
    AgeGenericGHDSeparatorPlan *reduction_ghd_separators;
    int component_count;
    int *variable_component_ids;
    bool reduction_order_applied;
    bool reduction_order_checked;
    bool reduction_order_valid;
    bool combination_started;
    MemoryContext data_context;
    bool materialized;
    bool exhausted;
    int64 rows_materialized;
    int64 tuples_materialized;
    int64 providers_skipped_after_empty;
    int64 semijoin_passes;
    int64 semijoin_bottom_up_passes;
    int64 semijoin_top_down_passes;
    int64 semijoin_rows_removed;
    int64 semijoin_bottom_up_rows_removed;
    int64 semijoin_top_down_rows_removed;
    int64 semijoin_provider_rows_after;
    int64 semijoin_final_domain_keys;
    int64 separator_reduction_passes;
    int64 separator_descriptor_applications;
    int64 separator_leaf_tail_providers;
    int64 separator_tail_domain_passes;
    int64 separator_tail_domain_rows_removed;
    int64 separator_domain_keys;
    int64 separator_leaf_tail_rows_removed;
    int64 separator_cyclic_core_rows_removed;
    int64 bindings_completed;
    int64 candidate_flat_rows;
    int64 candidate_combinations;
    int64 uniqueness_rejects;
    int64 rows_emitted;
    int64 rescans;
    int64 spill_bytes;
    int64 lazy_domain_views;
    int64 lazy_domain_keys_scanned;
    int64 lazy_domain_scratch_allocations;
    int64 lazy_domain_scratch_reuses;
    int64 lazy_prefix_range_builds;
    int64 lazy_prefix_range_reuses;
    int64 lazy_prefix_range_directory_searches;
    int64 lazy_prefix_range_cursor_reuses;
    int64 trie_child_range_reuses;
    int64 trie_child_range_opens;
    int64 trie_prefix_range_seeks;
    int64 trie_pair_range_opens;
    int64 reduction_scratch_allocations;
    int64 reduction_scratch_reuses;
    int64 vector_intersection_merge_calls;
    int64 vector_intersection_galloping_calls;
    int64 vector_intersection_galloping_steps;
    Size peak_memory;
};

static inline void
increment_generic_counter(int64 *counter)
{
    if (*counter < PG_INT64_MAX)
        (*counter)++;
}

static inline void
add_generic_counter(int64 *counter, int64 amount)
{
    if (amount <= 0)
        return;
    if (*counter > PG_INT64_MAX - amount)
        *counter = PG_INT64_MAX;
    else
        *counter += amount;
}

static Node *create_age_generic_join_state(CustomScan *cscan);
static void begin_age_generic_join(CustomScanState *node, EState *estate,
                                   int eflags);
static TupleTableSlot *exec_age_generic_join(CustomScanState *node);
static TupleTableSlot *access_age_generic_join(ScanState *node);
static bool recheck_age_generic_join(ScanState *node, TupleTableSlot *slot);
static void end_age_generic_join(CustomScanState *node);
static void rescan_age_generic_join(CustomScanState *node);
static void explain_age_generic_join(CustomScanState *node, List *ancestors,
                                     ExplainState *es);
static void initialize_generic_reduction_order(AgeGenericJoinState *state,
                                               List *provider_descs,
                                               List *reduction_desc);
static void initialize_generic_ghd_separators(AgeGenericJoinState *state,
                                              List *reduction_desc);
static void build_generic_provider_prefix_range_directory(
    AgeGenericJoinState *state, AgeGenericProvider *provider);
static void reduce_generic_providers(AgeGenericJoinState *state);
static void reduce_generic_leaf_tail_separators(
    AgeGenericJoinState *state, MemoryContext reduction_context);
static void initialize_generic_provider_output(
    AgeGenericProvider *provider, TupleDesc tuple_desc, EState *estate);

const CustomScanMethods age_generic_join_scan_methods = {
    AGE_GENERIC_JOIN_SCAN_NAME,
    create_age_generic_join_state
};

static const CustomExecMethods age_generic_join_exec_methods = {
    AGE_GENERIC_JOIN_SCAN_NAME,
    begin_age_generic_join,
    exec_age_generic_join,
    end_age_generic_join,
    rescan_age_generic_join,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    explain_age_generic_join
};

static int
generic_provider_desc_int(List *desc, int field)
{
    if (list_length(desc) != AGE_GENERIC_PROVIDER_DESC_COUNT)
        elog(ERROR, "invalid AGE Generic Join provider descriptor");
    return intVal(list_nth(desc, field));
}

static void
validate_generic_leaf_peel_order(AgeGenericJoinState *state,
                                 List *provider_descs)
{
    bool *edge_alive;
    bool *seen;
    int *degree;
    ListCell *lc;
    int provider_index = 0;
    int edge_count = 0;
    int expected_edge_count;
    int order_index;

    edge_alive = palloc0(sizeof(bool) * state->provider_count);
    seen = palloc0(sizeof(bool) * state->provider_count);
    degree = palloc0(sizeof(int) * state->variable_count);

    foreach(lc, provider_descs)
    {
        List *desc = lfirst_node(List, lc);
        int kind = generic_provider_desc_int(
            desc, AGE_GENERIC_PROVIDER_DESC_KIND);

        if (kind == AGE_GENERIC_PROVIDER_EDGE)
        {
            int var1 = generic_provider_desc_int(
                desc, AGE_GENERIC_PROVIDER_DESC_VAR1);
            int var2 = generic_provider_desc_int(
                desc, AGE_GENERIC_PROVIDER_DESC_VAR2);

            if (var1 < 0 || var1 >= state->variable_count ||
                var2 <= var1 || var2 >= state->variable_count)
            {
                elog(ERROR,
                     "invalid AGE Generic Join reduction order edge");
            }
            if (state->variable_component_ids[var1] !=
                state->variable_component_ids[var2])
            {
                elog(ERROR,
                     "AGE Generic Join leaf-peel edge crosses components");
            }
            edge_alive[provider_index] = true;
            degree[var1]++;
            degree[var2]++;
            edge_count++;
        }
        else if (kind != AGE_GENERIC_PROVIDER_VERTEX)
        {
            elog(ERROR, "invalid AGE Generic Join provider kind %d", kind);
        }
        provider_index++;
    }

    expected_edge_count = state->variable_count - state->component_count;
    if (edge_count != state->reduction_order_edge_count ||
        edge_count != expected_edge_count)
    {
        elog(ERROR, "invalid AGE Generic Join leaf-peel reduction order");
    }

    for (order_index = 0;
         order_index < state->reduction_order_edge_count;
         order_index++)
    {
        List *desc;
        int var1;
        int var2;

        provider_index = state->reduction_order_provider_indexes[order_index];
        if (provider_index < 0 || provider_index >= state->provider_count ||
            seen[provider_index] || !edge_alive[provider_index])
        {
            elog(ERROR, "invalid AGE Generic Join reduction order edge");
        }

        desc = list_nth_node(List, provider_descs, provider_index);
        var1 = generic_provider_desc_int(desc, AGE_GENERIC_PROVIDER_DESC_VAR1);
        var2 = generic_provider_desc_int(desc, AGE_GENERIC_PROVIDER_DESC_VAR2);
        if (degree[var1] != 1 && degree[var2] != 1)
        {
            elog(ERROR,
                 "AGE Generic Join reduction order is not leaf-peel");
        }

        seen[provider_index] = true;
        edge_alive[provider_index] = false;
        degree[var1]--;
        degree[var2]--;
    }

    for (provider_index = 0;
         provider_index < state->provider_count;
         provider_index++)
    {
        if (edge_alive[provider_index])
            elog(ERROR,
                 "AGE Generic Join reduction order did not cover an edge");
    }

    pfree(edge_alive);
    pfree(seen);
    pfree(degree);
}

static void
initialize_generic_ghd_separators(AgeGenericJoinState *state,
                                  List *reduction_desc)
{
    Node *separators_node;
    List *separator_descs;
    ListCell *lc;
    int index = 0;

    if (state->reduction_ghd_separator_count <= 0)
        return;

    separators_node = list_nth(
        reduction_desc, AGE_GENERIC_REDUCTION_DESC_GHD_SEPARATORS);
    if (separators_node == NULL || !IsA(separators_node, List))
        elog(ERROR, "invalid AGE Generic Join GHD separator descriptor");

    separator_descs = (List *)separators_node;
    if (list_length(separator_descs) != state->reduction_ghd_separator_count)
        elog(ERROR, "invalid AGE Generic Join GHD separator count");

    state->reduction_ghd_separators =
        palloc0(sizeof(AgeGenericGHDSeparatorPlan) *
                state->reduction_ghd_separator_count);
    foreach(lc, separator_descs)
    {
        List *desc = lfirst(lc);
        AgeGenericGHDSeparatorPlan *plan;
        int separator_id;

        if (desc == NIL || !IsA(desc, List) ||
            list_length(desc) != AGE_GENERIC_GHD_SEPARATOR_DESC_COUNT)
        {
            elog(ERROR, "invalid AGE Generic Join GHD separator descriptor");
        }

        separator_id = intVal(list_nth(
            desc, AGE_GENERIC_GHD_SEPARATOR_DESC_ID));
        if (separator_id != index)
            elog(ERROR, "invalid AGE Generic Join GHD separator id");

        plan = &state->reduction_ghd_separators[index++];
        plan->separator_variable = intVal(list_nth(
            desc, AGE_GENERIC_GHD_SEPARATOR_DESC_SEPARATOR_VARIABLE));
        plan->tail_variable = intVal(list_nth(
            desc, AGE_GENERIC_GHD_SEPARATOR_DESC_TAIL_VARIABLE));
        plan->provider_index = intVal(list_nth(
            desc, AGE_GENERIC_GHD_SEPARATOR_DESC_PROVIDER_INDEX));
        plan->separator_is_key1 = intVal(list_nth(
            desc, AGE_GENERIC_GHD_SEPARATOR_DESC_SEPARATOR_IS_KEY1)) != 0;

        if (plan->provider_index < 0 ||
            plan->provider_index >= state->provider_count ||
            plan->separator_variable < 0 ||
            plan->separator_variable >= state->variable_count ||
            plan->tail_variable < 0 ||
            plan->tail_variable >= state->variable_count ||
            plan->separator_variable == plan->tail_variable)
        {
            elog(ERROR, "invalid AGE Generic Join GHD separator descriptor");
        }
    }
}

static void
initialize_generic_reduction_order(AgeGenericJoinState *state,
                                   List *provider_descs,
                                   List *reduction_desc)
{
    Node *reduction_order_node;
    List *reduction_order_desc;
    Node *component_ids_node;
    List *component_ids_desc;
    ListCell *lc;
    int index = 0;

    state->reduction_shape = (AgeGenericReductionShape)intVal(list_nth(
        reduction_desc, AGE_GENERIC_REDUCTION_DESC_SHAPE));
    state->reduction_core_variables = intVal(list_nth(
        reduction_desc, AGE_GENERIC_REDUCTION_DESC_CORE_VARIABLES));
    state->reduction_tail_separators = intVal(list_nth(
        reduction_desc, AGE_GENERIC_REDUCTION_DESC_TAIL_SEPARATORS));
    state->reduction_order_kind =
        (AgeGenericReductionOrderKind)intVal(list_nth(
            reduction_desc, AGE_GENERIC_REDUCTION_DESC_ORDER_KIND));
    state->reduction_descriptor_source =
        (AgeGenericReductionDescriptorSource)intVal(list_nth(
            reduction_desc, AGE_GENERIC_REDUCTION_DESC_SOURCE));
    state->reduction_ghd_bag_count = intVal(list_nth(
        reduction_desc, AGE_GENERIC_REDUCTION_DESC_GHD_BAG_COUNT));
    state->reduction_ghd_separator_count = intVal(list_nth(
        reduction_desc, AGE_GENERIC_REDUCTION_DESC_GHD_SEPARATOR_COUNT));
    initialize_generic_ghd_separators(state, reduction_desc);

    reduction_order_node = list_nth(
        reduction_desc, AGE_GENERIC_REDUCTION_DESC_ORDER_EDGES);
    if (reduction_order_node != NULL && !IsA(reduction_order_node, List))
        elog(ERROR, "invalid AGE Generic Join reduction order descriptor");
    reduction_order_desc = (List *)reduction_order_node;
    state->reduction_order_edge_count = list_length(reduction_order_desc);
    if (state->reduction_order_edge_count > 0)
    {
        state->reduction_order_provider_indexes =
            palloc(sizeof(int) * state->reduction_order_edge_count);
        foreach(lc, reduction_order_desc)
        {
            state->reduction_order_provider_indexes[index++] =
                intVal(lfirst(lc));
        }
    }

    state->component_count = intVal(list_nth(
        reduction_desc, AGE_GENERIC_REDUCTION_DESC_COMPONENT_COUNT));
    component_ids_node = list_nth(
        reduction_desc, AGE_GENERIC_REDUCTION_DESC_COMPONENT_IDS);
    if (component_ids_node != NULL && !IsA(component_ids_node, List))
        elog(ERROR, "invalid AGE Generic Join component descriptor");
    component_ids_desc = (List *)component_ids_node;
    if (state->component_count <= 0 ||
        list_length(component_ids_desc) != state->variable_count)
    {
        elog(ERROR, "invalid AGE Generic Join component descriptor");
    }
    state->variable_component_ids =
        palloc(sizeof(int) * state->variable_count);
    index = 0;
    foreach(lc, component_ids_desc)
    {
        int component_id = intVal(lfirst(lc));

        if (component_id < 0 || component_id >= state->component_count)
        {
            elog(ERROR, "invalid AGE Generic Join component id");
        }
        state->variable_component_ids[index++] = component_id;
    }

    if (state->reduction_shape < AGE_GENERIC_REDUCTION_ALPHA_ACYCLIC ||
        state->reduction_shape > AGE_GENERIC_REDUCTION_CYCLIC_WITH_TAIL ||
        state->reduction_core_variables < 0 ||
        state->reduction_tail_separators < 0 ||
        state->reduction_order_kind < AGE_GENERIC_REDUCTION_ORDER_NONE ||
        state->reduction_order_kind > AGE_GENERIC_REDUCTION_ORDER_LEAF_PEEL ||
        state->reduction_descriptor_source <
        AGE_GENERIC_REDUCTION_SOURCE_LOCAL ||
        state->reduction_descriptor_source >
        AGE_GENERIC_REDUCTION_SOURCE_GRAPH_JOIN_MATCH_IR ||
        state->reduction_ghd_bag_count < 0 ||
        state->reduction_ghd_separator_count < 0)
    {
        elog(ERROR, "invalid AGE Generic Join reduction descriptor");
    }

    if (state->component_count != 1 &&
        state->reduction_shape != AGE_GENERIC_REDUCTION_ALPHA_ACYCLIC)
    {
        elog(ERROR,
             "AGE Generic Join multi-component reduction requires alpha-acyclic shape");
    }

    if (state->reduction_shape == AGE_GENERIC_REDUCTION_ALPHA_ACYCLIC)
    {
        if (state->reduction_tail_separators != 0 ||
            state->reduction_order_kind !=
            AGE_GENERIC_REDUCTION_ORDER_LEAF_PEEL)
        {
            elog(ERROR,
                 "invalid AGE Generic Join alpha-acyclic reduction order");
        }
        validate_generic_leaf_peel_order(state, provider_descs);
    }
    else if (state->reduction_order_kind != AGE_GENERIC_REDUCTION_ORDER_NONE ||
             state->reduction_order_edge_count != 0)
    {
        elog(ERROR, "invalid AGE Generic Join cyclic reduction order");
    }
}

static Node *
create_age_generic_join_state(CustomScan *cscan)
{
    AgeGenericJoinState *state;
    List *variable_rtis;
    List *provider_descs;
    List *uniqueness_group_descs;
    List *reduction_desc;
    ListCell *lc;
    int index = 0;

    if (list_length(cscan->custom_private) !=
        AGE_GENERIC_JOIN_PRIVATE_COUNT)
    {
        elog(ERROR, "invalid AGE Generic Join private descriptor");
    }

    state = palloc0(sizeof(*state));
    state->css.ss.ps.type = T_CustomScanState;
    state->css.flags = cscan->flags;
    state->css.methods = &age_generic_join_exec_methods;
    state->variable_count = intVal(list_nth(
        cscan->custom_private, AGE_GENERIC_JOIN_PRIVATE_VARIABLE_COUNT));
    variable_rtis = list_nth(
        cscan->custom_private, AGE_GENERIC_JOIN_PRIVATE_VARIABLE_RTIS);
    provider_descs = list_nth(
        cscan->custom_private, AGE_GENERIC_JOIN_PRIVATE_PROVIDER_DESCS);
    uniqueness_group_descs = list_nth(
        cscan->custom_private,
        AGE_GENERIC_JOIN_PRIVATE_UNIQUENESS_GROUPS);
    reduction_desc = list_nth(
        cscan->custom_private,
        AGE_GENERIC_JOIN_PRIVATE_REDUCTION_DESC);
    if (state->variable_count < 2 ||
        list_length(variable_rtis) != state->variable_count ||
        provider_descs == NIL ||
        list_length(uniqueness_group_descs) != list_length(provider_descs) ||
        list_length(reduction_desc) != AGE_GENERIC_REDUCTION_DESC_COUNT)
    {
        elog(ERROR, "invalid AGE Generic Join arity");
    }

    state->variable_rtis = palloc0(sizeof(Index) * state->variable_count);
    foreach(lc, variable_rtis)
        state->variable_rtis[index++] = (Index)intVal(lfirst(lc));
    state->provider_count = list_length(provider_descs);
    initialize_generic_reduction_order(state, provider_descs,
                                       reduction_desc);

    return (Node *)state;
}

static int
compare_generic_rows(const void *left, const void *right)
{
    const AgeGenericRow *a = left;
    const AgeGenericRow *b = right;

    if (a->key1 < b->key1)
        return -1;
    if (a->key1 > b->key1)
        return 1;
    if (a->key2 < b->key2)
        return -1;
    if (a->key2 > b->key2)
        return 1;
    if (a->edge_id_valid && b->edge_id_valid)
    {
        if (a->edge_id < b->edge_id)
            return -1;
        if (a->edge_id > b->edge_id)
            return 1;
    }
    return 0;
}

static void
append_generic_row(AgeGenericJoinState *state, AgeGenericProvider *provider,
                   graphid key1, graphid key2, graphid edge_id,
                   bool edge_id_valid, TupleTableSlot *slot)
{
    MemoryContext oldcontext;

    oldcontext = MemoryContextSwitchTo(state->data_context);
    if (provider->row_count == provider->row_capacity)
    {
        int new_capacity = provider->row_capacity == 0 ? 128 :
            provider->row_capacity * 2;

        if (new_capacity <= provider->row_capacity ||
            (Size)new_capacity > MaxAllocSize / sizeof(AgeGenericRow))
        {
            ereport(ERROR,
                    (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                     errmsg("AGE Generic Join provider is too large")));
        }
        if (provider->rows == NULL)
        {
            provider->rows = palloc(sizeof(AgeGenericRow) * new_capacity);
        }
        else
        {
            provider->rows = repalloc(provider->rows,
                                      sizeof(AgeGenericRow) * new_capacity);
        }
        provider->row_capacity = new_capacity;
    }
    provider->rows[provider->row_count].key1 = key1;
    provider->rows[provider->row_count].key2 = key2;
    provider->rows[provider->row_count].edge_id = edge_id;
    provider->rows[provider->row_count].edge_id_valid = edge_id_valid;
    if (provider->key_only_output)
    {
        provider->rows[provider->row_count].tuple = NULL;
    }
    else
    {
        provider->rows[provider->row_count].tuple =
            ExecCopySlotMinimalTuple(slot);
        increment_generic_counter(&state->tuples_materialized);
    }
    provider->row_count++;
    MemoryContextSwitchTo(oldcontext);

    increment_generic_counter(&state->rows_materialized);
}

static void
materialize_generic_providers(AgeGenericJoinState *state)
{
    int provider_index;

    if (state->materialized)
        return;

    for (provider_index = 0;
         provider_index < state->provider_count;
         provider_index++)
    {
        AgeGenericProvider *provider = &state->providers[provider_index];
        TupleTableSlot *slot;

        for (;;)
        {
            Datum value;
            graphid key1;
            graphid key2 = 0;
            graphid edge_id = 0;
            bool isnull;
            bool edge_id_valid = false;

            slot = ExecProcNode(provider->plan_state);
            if (TupIsNull(slot))
                break;
            value = slot_getattr(slot, provider->key1_attno, &isnull);
            if (isnull)
                continue;
            key1 = DATUM_GET_GRAPHID(value);
            if (provider->kind == AGE_GENERIC_PROVIDER_EDGE)
            {
                value = slot_getattr(slot, provider->key2_attno, &isnull);
                if (isnull)
                    continue;
                key2 = DATUM_GET_GRAPHID(value);
                value = slot_getattr(slot, provider->edge_id_attno, &isnull);
                if (!isnull)
                {
                    edge_id = DATUM_GET_GRAPHID(value);
                    edge_id_valid = true;
                }
            }
            append_generic_row(state, provider, key1, key2, edge_id,
                               edge_id_valid, slot);
        }

        if (provider->row_count > 1)
        {
            qsort(provider->rows, provider->row_count,
                  sizeof(AgeGenericRow), compare_generic_rows);
        }
        if (provider->row_count == 0)
        {
            state->exhausted = true;
            add_generic_counter(&state->providers_skipped_after_empty,
                                state->provider_count - provider_index - 1);
            break;
        }
    }
    if (!state->exhausted)
        reduce_generic_providers(state);
    state->materialized = true;
    state->peak_memory = Max(state->peak_memory,
                             MemoryContextMemAllocated(state->data_context,
                                                       true));
}

static int
lower_bound_key1(AgeGenericProvider *provider, graphid key)
{
    int low = 0;
    int high = provider->row_count;

    while (low < high)
    {
        int middle = low + (high - low) / 2;

        if (provider->rows[middle].key1 < key)
            low = middle + 1;
        else
            high = middle;
    }
    return low;
}

static int
upper_bound_key1(AgeGenericProvider *provider, graphid key)
{
    int low = 0;
    int high = provider->row_count;

    while (low < high)
    {
        int middle = low + (high - low) / 2;

        if (provider->rows[middle].key1 <= key)
            low = middle + 1;
        else
            high = middle;
    }
    return low;
}

static void
invalidate_generic_provider_prefix_ranges(AgeGenericProvider *provider)
{
    int cache_index;

    provider->prefix_range_count = 0;
    provider->prefix_ranges_valid = false;
    for (cache_index = 0; cache_index < provider->prefix_range_cache_count;
         cache_index++)
    {
        provider->prefix_range_cache[cache_index].valid = false;
    }
    provider->prefix_range_cache_count = 0;
    provider->prefix_range_misses = 0;
    provider->prefix_range_cursor_valid = false;
    for (cache_index = 0; cache_index < provider->child_range_cache_count;
         cache_index++)
    {
        provider->child_range_cache[cache_index].valid = false;
    }
    provider->child_range_cache_count = 0;
}

static bool
generic_provider_prefix_directory_lookup(AgeGenericJoinState *state,
                                         AgeGenericProvider *provider,
                                         graphid key, int *start, int *end)
{
    int low = 0;
    int high = provider->prefix_range_count;

    if (!provider->prefix_ranges_valid)
        return false;

    if (provider->prefix_range_cursor_valid &&
        provider->prefix_range_cursor_key == key)
    {
        *start = provider->prefix_range_cursor_start;
        *end = provider->prefix_range_cursor_end;
        increment_generic_counter(&state->lazy_prefix_range_reuses);
        increment_generic_counter(&state->lazy_prefix_range_cursor_reuses);
        return true;
    }

    increment_generic_counter(&state->lazy_prefix_range_directory_searches);
    while (low < high)
    {
        int middle = low + (high - low) / 2;

        if (provider->prefix_ranges[middle].key < key)
            low = middle + 1;
        else
            high = middle;
    }

    increment_generic_counter(&state->lazy_prefix_range_reuses);
    if (low < provider->prefix_range_count &&
        provider->prefix_ranges[low].key == key)
    {
        *start = provider->prefix_ranges[low].start;
        *end = provider->prefix_ranges[low].end;
    }
    else if (low < provider->prefix_range_count)
    {
        *start = provider->prefix_ranges[low].start;
        *end = provider->prefix_ranges[low].start;
    }
    else
    {
        *start = provider->row_count;
        *end = provider->row_count;
    }
    provider->prefix_range_cursor_key = key;
    provider->prefix_range_cursor_start = *start;
    provider->prefix_range_cursor_end = *end;
    provider->prefix_range_cursor_valid = true;
    return true;
}

static bool
generic_provider_prefix_cache_lookup(AgeGenericProvider *provider,
                                     graphid key, int *start, int *end)
{
    int cache_index;

    for (cache_index = 0; cache_index < provider->prefix_range_cache_count;
         cache_index++)
    {
        AgeGenericPrefixRange *entry =
            &provider->prefix_range_cache[cache_index];

        if (!entry->valid)
            continue;
        if (entry->key == key)
        {
            AgeGenericPrefixRange hit = *entry;

            while (cache_index > 0)
            {
                provider->prefix_range_cache[cache_index] =
                    provider->prefix_range_cache[cache_index - 1];
                cache_index--;
            }
            provider->prefix_range_cache[0] = hit;
            *start = hit.start;
            *end = hit.end;
            return true;
        }
    }
    return false;
}

static void
generic_provider_prefix_cache_store(AgeGenericProvider *provider, graphid key,
                                    int start, int end)
{
    int cache_index;
    int cache_count = provider->prefix_range_cache_count;

    if (cache_count < AGE_GENERIC_PREFIX_RANGE_CACHE_SIZE)
        provider->prefix_range_cache_count++;
    else
        cache_count = AGE_GENERIC_PREFIX_RANGE_CACHE_SIZE - 1;

    for (cache_index = cache_count; cache_index > 0; cache_index--)
        provider->prefix_range_cache[cache_index] =
            provider->prefix_range_cache[cache_index - 1];

    provider->prefix_range_cache[0].key = key;
    provider->prefix_range_cache[0].start = start;
    provider->prefix_range_cache[0].end = end;
    provider->prefix_range_cache[0].valid = true;
}

static bool
generic_provider_child_range_cache_lookup(AgeGenericJoinState *state,
                                          AgeGenericProvider *provider,
                                          const graphid *prefix_keys,
                                          int prefix_count,
                                          AgeGenericTrieRange *range)
{
    graphid key = 0;
    int cache_index;

    if (prefix_count < 0 || prefix_count > 1)
        return false;
    if (prefix_count == 1)
    {
        if (prefix_keys == NULL)
            return false;
        key = prefix_keys[0];
    }

    for (cache_index = 0; cache_index < provider->child_range_cache_count;
         cache_index++)
    {
        AgeGenericChildRangeCacheEntry *entry =
            &provider->child_range_cache[cache_index];

        if (!entry->valid || entry->prefix_count != prefix_count)
            continue;
        if (prefix_count == 1 && entry->key != key)
            continue;
        {
            AgeGenericChildRangeCacheEntry hit = *entry;

            while (cache_index > 0)
            {
                provider->child_range_cache[cache_index] =
                    provider->child_range_cache[cache_index - 1];
                cache_index--;
            }
            provider->child_range_cache[0] = hit;
            *range = hit.range;
            increment_generic_counter(&state->trie_child_range_reuses);
            return true;
        }
    }
    return false;
}

static void
generic_provider_child_range_cache_store(AgeGenericProvider *provider,
                                         const graphid *prefix_keys,
                                         int prefix_count,
                                         AgeGenericTrieRange *range)
{
    int cache_index;
    int cache_count = provider->child_range_cache_count;

    if (prefix_count < 0 || prefix_count > 1)
        return;
    if (prefix_count == 1 && prefix_keys == NULL)
        return;

    if (cache_count < AGE_GENERIC_PREFIX_RANGE_CACHE_SIZE)
        provider->child_range_cache_count++;
    else
        cache_count = AGE_GENERIC_PREFIX_RANGE_CACHE_SIZE - 1;

    for (cache_index = cache_count; cache_index > 0; cache_index--)
        provider->child_range_cache[cache_index] =
            provider->child_range_cache[cache_index - 1];

    provider->child_range_cache[0].key =
        prefix_count == 1 ? prefix_keys[0] : 0;
    provider->child_range_cache[0].prefix_count = prefix_count;
    provider->child_range_cache[0].range = *range;
    provider->child_range_cache[0].valid = true;
}

static void
sorted_array_provider_key1_range(AgeGenericJoinState *state,
                                 AgeGenericProvider *provider, graphid key,
                                 int *start, int *end)
{
    if (generic_provider_prefix_directory_lookup(state, provider, key, start,
                                                 end))
    {
        return;
    }

    if (generic_provider_prefix_cache_lookup(provider, key, start, end))
    {
        increment_generic_counter(&state->lazy_prefix_range_reuses);
        return;
    }

    provider->prefix_range_misses++;
    if (provider->prefix_range_misses >=
        AGE_GENERIC_PREFIX_RANGE_DIRECTORY_MIN_MISSES &&
        provider->row_count > AGE_GENERIC_PREFIX_RANGE_CACHE_SIZE)
    {
        build_generic_provider_prefix_range_directory(state, provider);
        if (generic_provider_prefix_directory_lookup(state, provider, key,
                                                     start, end))
        {
            return;
        }
    }

    *start = lower_bound_key1(provider, key);
    *end = upper_bound_key1(provider, key);
    generic_provider_prefix_cache_store(provider, key, *start, *end);
    increment_generic_counter(&state->lazy_prefix_range_builds);
}

static bool
sorted_array_trie_open_child_range(AgeGenericJoinState *state,
                                   AgeGenericProvider *provider,
                                   const graphid *prefix_keys,
                                   int prefix_count,
                                   AgeGenericTrieRange *range)
{
    if (prefix_count < 0 || prefix_count > 1)
        elog(ERROR, "invalid AGE Generic Join trie prefix depth %d",
             prefix_count);
    if (prefix_count > 0 && prefix_keys == NULL)
        elog(ERROR, "invalid AGE Generic Join trie prefix keys");

    increment_generic_counter(&state->trie_child_range_opens);
    if (prefix_count == 0)
    {
        range->start = 0;
        range->end = provider->row_count;
        return true;
    }

    increment_generic_counter(&state->trie_prefix_range_seeks);
    sorted_array_provider_key1_range(state, provider, prefix_keys[0],
                                     &range->start, &range->end);
    return true;
}

static const AgeGenericTrieOps sorted_array_trie_ops = {
    "lazy sorted arrays with on-demand prefix directories",
    sorted_array_trie_open_child_range
};

static void
generic_provider_open_child_range(AgeGenericJoinState *state,
                                  AgeGenericProvider *provider,
                                  const graphid *prefix_keys,
                                  int prefix_count,
                                  AgeGenericTrieRange *range)
{
    if (provider->trie_ops == NULL ||
        provider->trie_ops->open_child_range == NULL)
    {
        elog(ERROR, "AGE Generic Join provider has no trie ops");
    }

    if (generic_provider_child_range_cache_lookup(
            state, provider, prefix_keys, prefix_count, range))
    {
        return;
    }

    provider->trie_ops->open_child_range(state, provider, prefix_keys,
                                         prefix_count, range);
    generic_provider_child_range_cache_store(provider, prefix_keys,
                                             prefix_count, range);
}

static void
generic_provider_key1_range(AgeGenericJoinState *state,
                            AgeGenericProvider *provider, graphid key,
                            int *start, int *end)
{
    AgeGenericTrieRange range;
    graphid prefix_key = key;

    generic_provider_open_child_range(state, provider, &prefix_key, 1,
                                      &range);
    *start = range.start;
    *end = range.end;
}

static void
build_generic_provider_prefix_range_directory(AgeGenericJoinState *state,
                                              AgeGenericProvider *provider)
{
    MemoryContext oldcontext;
    graphid current_key;
    int current_start;
    int row_index;

    if (provider->prefix_ranges_valid)
        return;
    oldcontext = MemoryContextSwitchTo(state->data_context);
    invalidate_generic_provider_prefix_ranges(provider);
    if (provider->row_count <= 0)
    {
        provider->prefix_ranges_valid = true;
        MemoryContextSwitchTo(oldcontext);
        return;
    }
    if ((Size)provider->row_count >
        MaxAllocSize / sizeof(AgeGenericPrefixRange))
    {
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("AGE Generic Join prefix range directory is too "
                        "large")));
    }

    provider->prefix_ranges =
        palloc(sizeof(AgeGenericPrefixRange) * provider->row_count);
    current_key = provider->rows[0].key1;
    current_start = 0;
    for (row_index = 1; row_index <= provider->row_count; row_index++)
    {
        if (row_index < provider->row_count &&
            provider->rows[row_index].key1 == current_key)
        {
            continue;
        }

        provider->prefix_ranges[provider->prefix_range_count].key =
            current_key;
        provider->prefix_ranges[provider->prefix_range_count].start =
            current_start;
        provider->prefix_ranges[provider->prefix_range_count].end =
            row_index;
        provider->prefix_ranges[provider->prefix_range_count].valid = true;
        provider->prefix_range_count++;
        if (row_index < provider->row_count)
        {
            current_key = provider->rows[row_index].key1;
            current_start = row_index;
        }
    }
    provider->prefix_ranges_valid = true;
    add_generic_counter(&state->lazy_prefix_range_builds,
                        provider->prefix_range_count);
    MemoryContextSwitchTo(oldcontext);
    state->peak_memory = Max(state->peak_memory,
                             MemoryContextMemAllocated(state->data_context,
                                                       true));
}

static int
lower_bound_key2(AgeGenericProvider *provider, int low, int high,
                 graphid key2)
{
    while (low < high)
    {
        int middle = low + (high - low) / 2;

        if (provider->rows[middle].key2 < key2)
            low = middle + 1;
        else
            high = middle;
    }
    return low;
}

static int
upper_bound_key2(AgeGenericProvider *provider, int low, int high,
                 graphid key2)
{
    while (low < high)
    {
        int middle = low + (high - low) / 2;

        if (provider->rows[middle].key2 <= key2)
            low = middle + 1;
        else
            high = middle;
    }
    return low;
}

static void
generic_provider_pair_range(AgeGenericJoinState *state,
                            AgeGenericProvider *provider,
                            graphid key1, graphid key2,
                            int *start, int *end)
{
    AgeGenericTrieRange range;
    graphid prefix_key = key1;

    generic_provider_open_child_range(state, provider, &prefix_key, 1,
                                      &range);
    *start = lower_bound_key2(provider, range.start, range.end, key2);
    *end = upper_bound_key2(provider, *start, range.end, key2);
    increment_generic_counter(&state->trie_pair_range_opens);
}

static void
ensure_generic_level_capacity(AgeGenericJoinState *state,
                              AgeGenericLevel *level, int required)
{
    MemoryContext oldcontext;
    int new_capacity;

    if (required <= 0)
        return;
    if ((Size)required > MaxAllocSize / sizeof(graphid))
    {
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("AGE Generic Join domain is too large")));
    }
    if (level->candidate_capacity >= required)
    {
        increment_generic_counter(&state->lazy_domain_scratch_reuses);
        return;
    }

    new_capacity = Max(level->candidate_capacity, 128);
    while (new_capacity < required)
    {
        if (new_capacity > MaxAllocSize / (sizeof(graphid) * 2))
            new_capacity = required;
        else
            new_capacity *= 2;
    }

    oldcontext = MemoryContextSwitchTo(state->data_context);
    if (level->candidate_buffer == NULL)
        level->candidate_buffer = palloc(sizeof(graphid) * new_capacity);
    else
        level->candidate_buffer = repalloc(
            level->candidate_buffer, sizeof(graphid) * new_capacity);
    MemoryContextSwitchTo(oldcontext);
    level->candidate_capacity = new_capacity;
    increment_generic_counter(&state->lazy_domain_scratch_allocations);
    state->peak_memory = Max(state->peak_memory,
                             MemoryContextMemAllocated(state->data_context,
                                                       true));
}

static graphid *
ensure_generic_reduction_scratch_capacity(AgeGenericJoinState *state,
                                          int required)
{
    MemoryContext oldcontext;
    int new_capacity;

    if (required <= 0)
        return NULL;
    if ((Size)required > MaxAllocSize / sizeof(graphid))
    {
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("AGE Generic Join reduction scratch is too large")));
    }
    if (state->reduction_scratch.capacity >= required)
    {
        increment_generic_counter(&state->reduction_scratch_reuses);
        return state->reduction_scratch.keys;
    }

    new_capacity = Max(state->reduction_scratch.capacity, 128);
    while (new_capacity < required)
    {
        if (new_capacity > MaxAllocSize / (sizeof(graphid) * 2))
            new_capacity = required;
        else
            new_capacity *= 2;
    }

    oldcontext = MemoryContextSwitchTo(state->data_context);
    if (state->reduction_scratch.keys == NULL)
        state->reduction_scratch.keys = palloc(sizeof(graphid) * new_capacity);
    else
        state->reduction_scratch.keys = repalloc(
            state->reduction_scratch.keys, sizeof(graphid) * new_capacity);
    MemoryContextSwitchTo(oldcontext);
    state->reduction_scratch.capacity = new_capacity;
    increment_generic_counter(&state->reduction_scratch_allocations);
    state->peak_memory = Max(state->peak_memory,
                             MemoryContextMemAllocated(state->data_context,
                                                       true));
    return state->reduction_scratch.keys;
}

static graphid
generic_domain_view_key(const AgeGenericDomainView *view, int row_index)
{
    AgeGenericRow *row = &view->provider->rows[row_index];

    return view->use_key2 ? row->key2 : row->key1;
}

static bool
build_generic_domain_view(AgeGenericJoinState *state,
                          AgeGenericProvider *provider, int variable,
                          AgeGenericDomainView *view)
{
    int start = 0;
    int end = provider->row_count;
    bool use_key2 = false;

    if (provider->row_count == 0)
        return false;
    if (provider->kind == AGE_GENERIC_PROVIDER_EDGE &&
        provider->var2 == variable)
    {
        generic_provider_key1_range(state, provider,
                                    state->bindings[provider->var1],
                                    &start, &end);
        use_key2 = true;
    }
    else if (provider->var1 != variable)
    {
        return false;
    }
    if (start >= end)
        return false;

    view->provider = provider;
    view->variable = variable;
    view->start = start;
    view->end = end;
    view->use_key2 = use_key2;
    increment_generic_counter(&state->lazy_domain_views);
    return true;
}

static int
copy_generic_domain_view(AgeGenericJoinState *state, AgeGenericLevel *level,
                         const AgeGenericDomainView *view)
{
    int row_index;
    int count = 0;
    graphid previous = 0;
    bool have_previous = false;

    ensure_generic_level_capacity(state, level, view->end - view->start);
    for (row_index = view->start; row_index < view->end; row_index++)
    {
        graphid value = generic_domain_view_key(view, row_index);

        if (!have_previous || value != previous)
        {
            level->candidate_buffer[count++] = value;
            previous = value;
            have_previous = true;
        }
    }
    add_generic_counter(&state->lazy_domain_keys_scanned,
                        view->end - view->start);
    return count;
}

static bool
generic_domain_view_next_distinct(AgeGenericJoinState *state,
                                  const AgeGenericDomainView *view,
                                  int *row_index, graphid *key)
{
    graphid value;
    int start;

    if (*row_index >= view->end)
        return false;
    start = *row_index;
    value = generic_domain_view_key(view, *row_index);
    do
    {
        (*row_index)++;
    } while (*row_index < view->end &&
             generic_domain_view_key(view, *row_index) == value);
    add_generic_counter(&state->lazy_domain_keys_scanned,
                        *row_index - start);
    *key = value;
    return true;
}

static int
generic_graphid_galloping_lower_bound(AgeGenericJoinState *state,
                                      const graphid *values, int value_count,
                                      int start_index, graphid target)
{
    int low;
    int high;
    int bound;

    if (start_index >= value_count)
        return value_count;

    increment_generic_counter(&state->vector_intersection_galloping_steps);
    if (values[start_index] >= target)
        return start_index;

    bound = 1;
    while (bound < value_count - start_index)
    {
        int probe = start_index + bound;

        increment_generic_counter(
            &state->vector_intersection_galloping_steps);
        if (values[probe] >= target)
            break;
        if (bound > (value_count - start_index) / 2)
        {
            bound = value_count - start_index;
            break;
        }
        bound *= 2;
    }

    low = start_index + bound / 2 + 1;
    high = Min(start_index + bound + 1, value_count);
    while (low < high)
    {
        int middle = low + (high - low) / 2;

        increment_generic_counter(
            &state->vector_intersection_galloping_steps);
        if (values[middle] < target)
            low = middle + 1;
        else
            high = middle;
    }
    return low;
}

static int
generic_domain_view_galloping_lower_bound(AgeGenericJoinState *state,
                                          const AgeGenericDomainView *view,
                                          int start_index, graphid target)
{
    int low;
    int high;
    int bound;

    if (start_index >= view->end)
        return view->end;

    increment_generic_counter(&state->vector_intersection_galloping_steps);
    if (generic_domain_view_key(view, start_index) >= target)
        return start_index;

    bound = 1;
    while (bound < view->end - start_index)
    {
        int probe = start_index + bound;

        increment_generic_counter(
            &state->vector_intersection_galloping_steps);
        if (generic_domain_view_key(view, probe) >= target)
            break;
        if (bound > (view->end - start_index) / 2)
        {
            bound = view->end - start_index;
            break;
        }
        bound *= 2;
    }

    low = start_index + bound / 2 + 1;
    high = Min(start_index + bound + 1, view->end);
    while (low < high)
    {
        int middle = low + (high - low) / 2;

        increment_generic_counter(
            &state->vector_intersection_galloping_steps);
        if (generic_domain_view_key(view, middle) < target)
            low = middle + 1;
        else
            high = middle;
    }
    return low;
}

static int
intersect_generic_candidates_with_view(AgeGenericJoinState *state,
                                       graphid *candidates,
                                       int candidate_count,
                                       const AgeGenericDomainView *view)
{
    int candidate_index = 0;
    int domain_index = view->start;
    int output_count = 0;
    int view_row_count = view->end - view->start;

    if (candidate_count <= 0 || view_row_count <= 0)
        return 0;

    if ((int64)view_row_count >=
        (int64)candidate_count * AGE_GENERIC_GALLOPING_MIN_RATIO)
    {
        increment_generic_counter(
            &state->vector_intersection_galloping_calls);
        while (candidate_index < candidate_count && domain_index < view->end)
        {
            graphid candidate = candidates[candidate_index];

            domain_index = generic_domain_view_galloping_lower_bound(
                state, view, domain_index, candidate);
            if (domain_index >= view->end)
                break;
            if (generic_domain_view_key(view, domain_index) == candidate)
            {
                candidates[output_count++] = candidate;
                domain_index++;
            }
            candidate_index++;
        }
        return output_count;
    }

    if ((int64)candidate_count >=
        (int64)view_row_count * AGE_GENERIC_GALLOPING_MIN_RATIO)
    {
        graphid domain_key;

        increment_generic_counter(
            &state->vector_intersection_galloping_calls);
        while (candidate_index < candidate_count &&
               generic_domain_view_next_distinct(state, view, &domain_index,
                                                 &domain_key))
        {
            candidate_index = generic_graphid_galloping_lower_bound(
                state, candidates, candidate_count, candidate_index,
                domain_key);
            if (candidate_index >= candidate_count)
                break;
            if (candidates[candidate_index] == domain_key)
            {
                candidates[output_count++] = domain_key;
                candidate_index++;
            }
        }
        return output_count;
    }

    increment_generic_counter(&state->vector_intersection_merge_calls);
    {
        graphid domain_key;
        bool have_domain = generic_domain_view_next_distinct(
            state, view, &domain_index, &domain_key);

        while (candidate_index < candidate_count && have_domain)
        {
            graphid candidate = candidates[candidate_index];

            if (candidate < domain_key)
            {
                candidate_index++;
            }
            else if (candidate > domain_key)
            {
                have_domain = generic_domain_view_next_distinct(
                    state, view, &domain_index, &domain_key);
            }
            else
            {
                candidates[output_count++] = candidate;
                candidate_index++;
                have_domain = generic_domain_view_next_distinct(
                    state, view, &domain_index, &domain_key);
            }
        }
    }
    return output_count;
}

static int
intersect_generic_domains(AgeGenericJoinState *state,
                          graphid *left, int left_count,
                          const graphid *right, int right_count)
{
    int left_index = 0;
    int right_index = 0;
    int output_count = 0;

    if (left_count <= 0 || right_count <= 0)
        return 0;

    if ((int64)right_count >=
        (int64)left_count * AGE_GENERIC_GALLOPING_MIN_RATIO)
    {
        increment_generic_counter(
            &state->vector_intersection_galloping_calls);
        while (left_index < left_count && right_index < right_count)
        {
            graphid left_key = left[left_index];

            right_index = generic_graphid_galloping_lower_bound(
                state, right, right_count, right_index, left_key);
            if (right_index >= right_count)
                break;
            if (right[right_index] == left_key)
            {
                left[output_count++] = left_key;
                right_index++;
            }
            left_index++;
        }
        return output_count;
    }

    if ((int64)left_count >=
        (int64)right_count * AGE_GENERIC_GALLOPING_MIN_RATIO)
    {
        increment_generic_counter(
            &state->vector_intersection_galloping_calls);
        while (left_index < left_count && right_index < right_count)
        {
            graphid right_key = right[right_index];

            left_index = generic_graphid_galloping_lower_bound(
                state, left, left_count, left_index, right_key);
            if (left_index >= left_count)
                break;
            if (left[left_index] == right_key)
            {
                left[output_count++] = right_key;
                left_index++;
            }
            right_index++;
        }
        return output_count;
    }

    increment_generic_counter(&state->vector_intersection_merge_calls);
    while (left_index < left_count && right_index < right_count)
    {
        if (left[left_index] < right[right_index])
            left_index++;
        else if (left[left_index] > right[right_index])
            right_index++;
        else
        {
            left[output_count++] = left[left_index];
            left_index++;
            right_index++;
        }
    }
    return output_count;
}

static int
compare_generic_graphids(const void *left, const void *right)
{
    graphid a = *(const graphid *)left;
    graphid b = *(const graphid *)right;

    if (a < b)
        return -1;
    if (a > b)
        return 1;
    return 0;
}

/*
 * Return a sorted distinct domain for one unbound provider endpoint.  key1
 * is already ordered by the provider sort.  key2 is ordered only within a
 * key1 prefix, so it needs a compact scratch sort for query-wide reduction.
 */
static graphid *
generic_provider_unbound_domain(AgeGenericJoinState *state,
                                AgeGenericProvider *provider, int variable,
                                MemoryContext context, bool use_scratch,
                                int *domain_count)
{
    graphid *domain;
    bool use_key2;
    int row_index;
    int count = 0;
    MemoryContext oldcontext;

    *domain_count = 0;
    if (provider->row_count <= 0)
        return NULL;
    if (provider->var1 == variable)
        use_key2 = false;
    else if (provider->kind == AGE_GENERIC_PROVIDER_EDGE &&
             provider->var2 == variable)
        use_key2 = true;
    else
        return NULL;

    if (use_scratch)
    {
        domain = ensure_generic_reduction_scratch_capacity(
            state, provider->row_count);
    }
    else
    {
        oldcontext = MemoryContextSwitchTo(context);
        domain = palloc(sizeof(graphid) * provider->row_count);
        MemoryContextSwitchTo(oldcontext);
    }
    for (row_index = 0; row_index < provider->row_count; row_index++)
    {
        domain[row_index] = use_key2 ? provider->rows[row_index].key2 :
            provider->rows[row_index].key1;
    }
    if (use_key2 && provider->row_count > 1)
    {
        qsort(domain, provider->row_count, sizeof(graphid),
              compare_generic_graphids);
    }
    for (row_index = 0; row_index < provider->row_count; row_index++)
    {
        if (count == 0 || domain[row_index] != domain[count - 1])
            domain[count++] = domain[row_index];
    }
    *domain_count = count;
    return domain;
}

static bool
build_generic_reduction_domains(AgeGenericJoinState *state,
                                MemoryContext context,
                                AgeGenericDomain *domains)
{
    bool *initialized;
    int provider_index;
    int variable;

    initialized = MemoryContextAllocZero(
        context, sizeof(bool) * state->variable_count);
    for (provider_index = 0;
         provider_index < state->provider_count;
         provider_index++)
    {
        AgeGenericProvider *provider = &state->providers[provider_index];
        int endpoint_count = provider->kind == AGE_GENERIC_PROVIDER_EDGE ?
            2 : 1;
        int endpoint;

        for (endpoint = 0; endpoint < endpoint_count; endpoint++)
        {
            graphid *provider_domain;
            int provider_domain_count;

            variable = endpoint == 0 ? provider->var1 : provider->var2;
            provider_domain = generic_provider_unbound_domain(
                state, provider, variable, context, initialized[variable],
                &provider_domain_count);
            if (provider_domain_count <= 0)
                return false;
            if (!initialized[variable])
            {
                domains[variable].keys = provider_domain;
                domains[variable].count = provider_domain_count;
                initialized[variable] = true;
            }
            else
            {
                domains[variable].count = intersect_generic_domains(
                    state, domains[variable].keys, domains[variable].count,
                    provider_domain, provider_domain_count);
                if (domains[variable].count <= 0)
                    return false;
            }
        }
    }

    for (variable = 0; variable < state->variable_count; variable++)
    {
        if (!initialized[variable] || domains[variable].count <= 0)
            return false;
    }
    return true;
}

static bool
generic_domain_contains(const AgeGenericDomain *domain, graphid key)
{
    int low = 0;
    int high = domain->count;

    while (low < high)
    {
        int middle = low + (high - low) / 2;

        if (domain->keys[middle] < key)
            low = middle + 1;
        else
            high = middle;
    }
    return low < domain->count && domain->keys[low] == key;
}

static int64
generic_provider_row_total(AgeGenericJoinState *state)
{
    int64 total = 0;
    int provider_index;

    for (provider_index = 0;
         provider_index < state->provider_count;
         provider_index++)
    {
        add_generic_counter(&total, state->providers[provider_index].row_count);
    }
    return total;
}

static int64
generic_domain_key_total(AgeGenericJoinState *state,
                         const AgeGenericDomain *domains)
{
    int64 total = 0;
    int variable;

    for (variable = 0; variable < state->variable_count; variable++)
        add_generic_counter(&total, domains[variable].count);
    return total;
}

static int64
filter_generic_provider(AgeGenericProvider *provider,
                        const AgeGenericDomain *domains)
{
    int read_index;
    int write_index = 0;
    int old_count = provider->row_count;

    for (read_index = 0; read_index < old_count; read_index++)
    {
        AgeGenericRow *row = &provider->rows[read_index];

        if (!generic_domain_contains(&domains[provider->var1], row->key1))
            continue;
        if (provider->kind == AGE_GENERIC_PROVIDER_EDGE &&
            !generic_domain_contains(&domains[provider->var2], row->key2))
        {
            continue;
        }
        if (write_index != read_index)
            provider->rows[write_index] = *row;
        write_index++;
    }
    provider->row_count = write_index;
    invalidate_generic_provider_prefix_ranges(provider);
    return (int64)old_count - write_index;
}

static bool
generic_provider_has_variable(AgeGenericProvider *provider, int variable)
{
    if (provider->var1 == variable)
        return true;
    return provider->kind == AGE_GENERIC_PROVIDER_EDGE &&
        provider->var2 == variable;
}

static int64
filter_generic_provider_on_variable(AgeGenericProvider *provider,
                                    int variable,
                                    const AgeGenericDomain *domain)
{
    int read_index;
    int write_index = 0;
    int old_count = provider->row_count;
    bool use_key2 = false;

    if (provider->var1 == variable)
        use_key2 = false;
    else if (provider->kind == AGE_GENERIC_PROVIDER_EDGE &&
             provider->var2 == variable)
        use_key2 = true;
    else
        return 0;

    for (read_index = 0; read_index < old_count; read_index++)
    {
        AgeGenericRow *row = &provider->rows[read_index];
        graphid key = use_key2 ? row->key2 : row->key1;

        if (!generic_domain_contains(domain, key))
            continue;
        if (write_index != read_index)
            provider->rows[write_index] = *row;
        write_index++;
    }
    provider->row_count = write_index;
    invalidate_generic_provider_prefix_ranges(provider);
    return (int64)old_count - write_index;
}

static bool
generic_reduction_order_applicable(AgeGenericJoinState *state)
{
    bool *seen;
    int edge_count = 0;
    int order_index;
    int provider_index;

    if (state->reduction_order_checked)
        return state->reduction_order_valid;

    state->reduction_order_checked = true;
    if (state->reduction_shape != AGE_GENERIC_REDUCTION_ALPHA_ACYCLIC ||
        state->reduction_order_kind != AGE_GENERIC_REDUCTION_ORDER_LEAF_PEEL ||
        state->reduction_order_provider_indexes == NULL ||
        state->reduction_order_edge_count <= 0)
    {
        return false;
    }

    for (provider_index = 0;
         provider_index < state->provider_count;
         provider_index++)
    {
        if (state->providers[provider_index].kind == AGE_GENERIC_PROVIDER_EDGE)
            edge_count++;
    }
    if (edge_count != state->reduction_order_edge_count)
        return false;

    seen = palloc0(sizeof(bool) * state->provider_count);
    for (order_index = 0;
         order_index < state->reduction_order_edge_count;
         order_index++)
    {
        provider_index =
            state->reduction_order_provider_indexes[order_index];
        if (provider_index < 0 || provider_index >= state->provider_count ||
            seen[provider_index] ||
            state->providers[provider_index].kind !=
            AGE_GENERIC_PROVIDER_EDGE)
        {
            pfree(seen);
            return false;
        }
        seen[provider_index] = true;
    }

    pfree(seen);
    state->reduction_order_valid = true;
    return true;
}

static int64
filter_generic_provider_for_reduction(AgeGenericJoinState *state,
                                      int provider_index,
                                      const AgeGenericDomain *domains)
{
    AgeGenericProvider *provider;
    int64 rows_removed;

    provider = &state->providers[provider_index];
    rows_removed = filter_generic_provider(provider, domains);
    if (provider->row_count <= 0)
        state->exhausted = true;
    return rows_removed;
}

static int64
filter_generic_providers_in_reduction_order(AgeGenericJoinState *state,
                                            const AgeGenericDomain *domains,
                                            bool reverse)
{
    int64 rows_removed = 0;
    int order_index;
    int provider_index;

    state->reduction_order_applied = true;
    for (order_index = 0;
         order_index < state->reduction_order_edge_count;
         order_index++)
    {
        int ordered_index = reverse ?
            state->reduction_order_edge_count - order_index - 1 :
            order_index;

        provider_index =
            state->reduction_order_provider_indexes[ordered_index];
        rows_removed += filter_generic_provider_for_reduction(
            state, provider_index, domains);
        if (state->exhausted)
            return rows_removed;
    }

    for (provider_index = 0;
         provider_index < state->provider_count;
         provider_index++)
    {
        AgeGenericProvider *provider = &state->providers[provider_index];

        if (provider->kind != AGE_GENERIC_PROVIDER_VERTEX)
            continue;

        rows_removed += filter_generic_provider(provider, domains);
        if (provider->row_count <= 0)
        {
            state->exhausted = true;
            return rows_removed;
        }
    }
    return rows_removed;
}

static int64
filter_generic_providers_in_default_order(AgeGenericJoinState *state,
                                          const AgeGenericDomain *domains)
{
    int64 rows_removed = 0;
    int provider_index;

    for (provider_index = 0;
         provider_index < state->provider_count;
         provider_index++)
    {
        AgeGenericProvider *provider = &state->providers[provider_index];

        rows_removed += filter_generic_provider(provider, domains);
        if (provider->row_count <= 0)
        {
            state->exhausted = true;
            break;
        }
    }
    return rows_removed;
}

static bool *
build_generic_cycle_core_variables(AgeGenericJoinState *state,
                                   MemoryContext context, bool *has_core)
{
    bool *core_variables;
    bool *edge_alive;
    int *degree;
    bool changed;
    int provider_index;
    int variable;

    core_variables = MemoryContextAllocZero(
        context, sizeof(bool) * state->variable_count);
    edge_alive = MemoryContextAllocZero(
        context, sizeof(bool) * state->provider_count);
    degree = MemoryContextAllocZero(
        context, sizeof(int) * state->variable_count);

    for (provider_index = 0;
         provider_index < state->provider_count;
         provider_index++)
    {
        AgeGenericProvider *provider = &state->providers[provider_index];

        if (provider->kind != AGE_GENERIC_PROVIDER_EDGE)
            continue;
        edge_alive[provider_index] = true;
        degree[provider->var1]++;
        degree[provider->var2]++;
    }

    do
    {
        changed = false;
        for (provider_index = 0;
             provider_index < state->provider_count;
             provider_index++)
        {
            AgeGenericProvider *provider = &state->providers[provider_index];

            if (!edge_alive[provider_index])
                continue;
            if (degree[provider->var1] > 1 && degree[provider->var2] > 1)
                continue;
            edge_alive[provider_index] = false;
            degree[provider->var1]--;
            degree[provider->var2]--;
            changed = true;
        }
    } while (changed);

    *has_core = false;
    for (variable = 0; variable < state->variable_count; variable++)
    {
        if (degree[variable] >= 2)
        {
            core_variables[variable] = true;
            *has_core = true;
        }
    }
    return core_variables;
}

static bool
generic_leaf_tail_edge_info(AgeGenericProvider *provider,
                            const bool *core_variables,
                            int *separator_variable, int *tail_variable,
                            bool *separator_is_key1)
{
    bool key1_is_core;
    bool key2_is_core;

    if (provider->kind != AGE_GENERIC_PROVIDER_EDGE)
        return false;

    key1_is_core = core_variables[provider->var1];
    key2_is_core = core_variables[provider->var2];
    if (key1_is_core == key2_is_core)
        return false;

    if (key1_is_core)
    {
        *separator_variable = provider->var1;
        *tail_variable = provider->var2;
        *separator_is_key1 = true;
    }
    else
    {
        *separator_variable = provider->var2;
        *tail_variable = provider->var1;
        *separator_is_key1 = false;
    }
    return true;
}

static bool
generic_provider_is_planned_leaf_tail(AgeGenericJoinState *state,
                                      int provider_index)
{
    int separator_index;

    if (state->reduction_ghd_separators == NULL ||
        state->reduction_ghd_separator_count <= 0)
    {
        return false;
    }

    for (separator_index = 0;
         separator_index < state->reduction_ghd_separator_count;
         separator_index++)
    {
        if (state->reduction_ghd_separators[separator_index].provider_index ==
            provider_index)
        {
            return true;
        }
    }
    return false;
}

static bool
generic_provider_is_cyclic_core(AgeGenericProvider *provider,
                                const bool *core_variables)
{
    if (provider->kind == AGE_GENERIC_PROVIDER_VERTEX)
        return core_variables[provider->var1];
    return core_variables[provider->var1] && core_variables[provider->var2];
}

static bool
generic_has_leaf_tail_separator(AgeGenericJoinState *state,
                                const bool *core_variables)
{
    int provider_index;

    if (state->reduction_ghd_separators != NULL &&
        state->reduction_ghd_separator_count > 0)
    {
        return true;
    }

    for (provider_index = 0;
         provider_index < state->provider_count;
         provider_index++)
    {
        int separator_variable;
        int tail_variable;
        bool separator_is_key1;

        if (generic_leaf_tail_edge_info(&state->providers[provider_index],
                                        core_variables, &separator_variable,
                                        &tail_variable,
                                        &separator_is_key1))
        {
            return true;
        }
    }
    return false;
}

static graphid *
build_generic_tail_separator_domain(AgeGenericJoinState *state,
                                    AgeGenericProvider *provider,
                                    int tail_variable,
                                    bool separator_is_key1,
                                    const AgeGenericDomain *local_domains,
                                    MemoryContext context, bool use_scratch,
                                    int *domain_count)
{
    graphid *domain;
    int row_index;
    int count = 0;
    MemoryContext oldcontext;

    *domain_count = 0;
    if (provider->row_count <= 0 ||
        local_domains[tail_variable].count <= 0)
    {
        return NULL;
    }

    if (use_scratch)
    {
        domain = ensure_generic_reduction_scratch_capacity(
            state, provider->row_count);
    }
    else
    {
        oldcontext = MemoryContextSwitchTo(context);
        domain = palloc(sizeof(graphid) * provider->row_count);
        MemoryContextSwitchTo(oldcontext);
    }
    for (row_index = 0; row_index < provider->row_count; row_index++)
    {
        AgeGenericRow *row = &provider->rows[row_index];
        graphid separator_key = separator_is_key1 ? row->key1 : row->key2;
        graphid tail_key = separator_is_key1 ? row->key2 : row->key1;

        if (generic_domain_contains(&local_domains[tail_variable], tail_key))
            domain[count++] = separator_key;
    }
    if (count <= 0)
        return NULL;
    if (count > 1)
        qsort(domain, count, sizeof(graphid), compare_generic_graphids);
    for (row_index = 0; row_index < count; row_index++)
    {
        if (*domain_count == 0 ||
            domain[row_index] != domain[*domain_count - 1])
        {
            domain[(*domain_count)++] = domain[row_index];
        }
    }
    return domain;
}

static AgeGenericDomain *
build_generic_domains_in_context(AgeGenericJoinState *state,
                                 MemoryContext context)
{
    AgeGenericDomain *domains;

    domains = MemoryContextAllocZero(
        context, sizeof(AgeGenericDomain) * state->variable_count);
    if (!build_generic_reduction_domains(state, context, domains))
        return NULL;
    return domains;
}

static AgeGenericDomain *
reduce_generic_tail_domains(AgeGenericJoinState *state,
                            const bool *core_variables,
                            MemoryContext domain_context)
{
    AgeGenericDomain *local_domains;
    int max_passes = Max(state->variable_count, 1);
    int pass;

    local_domains = build_generic_domains_in_context(state, domain_context);
    if (local_domains == NULL)
        return NULL;

    for (pass = 0; pass < max_passes; pass++)
    {
        int64 rows_removed = 0;
        int provider_index;

        for (provider_index = 0;
             provider_index < state->provider_count;
             provider_index++)
        {
            AgeGenericProvider *provider = &state->providers[provider_index];

            if (generic_provider_is_cyclic_core(provider, core_variables))
                continue;

            rows_removed += filter_generic_provider(provider, local_domains);
            if (provider->row_count <= 0)
            {
                state->exhausted = true;
                return NULL;
            }
        }

        if (rows_removed == 0)
            break;

        increment_generic_counter(&state->separator_tail_domain_passes);
        add_generic_counter(&state->separator_tail_domain_rows_removed,
                            rows_removed);
        add_generic_counter(&state->separator_leaf_tail_rows_removed,
                            rows_removed);
        add_generic_counter(&state->semijoin_rows_removed, rows_removed);

        MemoryContextReset(domain_context);
        local_domains = build_generic_domains_in_context(state,
                                                         domain_context);
        if (local_domains == NULL)
        {
            state->exhausted = true;
            return NULL;
        }
    }
    return local_domains;
}

static void
reduce_generic_leaf_tail_separators(AgeGenericJoinState *state,
                                    MemoryContext reduction_context)
{
    bool has_core = false;
    bool found_leaf_tail = false;
    bool *core_variables;
    AgeGenericDomain *local_domains;
    AgeGenericSeparatorDomain *separator_domains;
    MemoryContext tail_domain_context;
    int provider_index;
    int variable;

    core_variables = build_generic_cycle_core_variables(
        state, reduction_context, &has_core);
    if (!has_core)
        return;
    if (!generic_has_leaf_tail_separator(state, core_variables))
        return;

    tail_domain_context = AllocSetContextCreate(
        reduction_context,
        "AGE Generic Join tail separator domains", ALLOCSET_SMALL_SIZES);
    local_domains = reduce_generic_tail_domains(state, core_variables,
                                                tail_domain_context);
    if (local_domains == NULL)
    {
        state->exhausted = true;
        return;
    }

    separator_domains = MemoryContextAllocZero(
        reduction_context,
        sizeof(AgeGenericSeparatorDomain) * state->variable_count);
    if (state->reduction_ghd_separators != NULL &&
        state->reduction_ghd_separator_count > 0)
    {
        int separator_index;

        for (separator_index = 0;
             separator_index < state->reduction_ghd_separator_count;
             separator_index++)
        {
            AgeGenericGHDSeparatorPlan *plan =
                &state->reduction_ghd_separators[separator_index];
            AgeGenericProvider *provider;
            graphid *separator_domain;
            int separator_domain_count;

            provider = &state->providers[plan->provider_index];
            if (provider->kind != AGE_GENERIC_PROVIDER_EDGE ||
                !generic_provider_has_variable(provider,
                                               plan->separator_variable) ||
                !generic_provider_has_variable(provider,
                                               plan->tail_variable) ||
                (plan->separator_is_key1 &&
                 (provider->var1 != plan->separator_variable ||
                  provider->var2 != plan->tail_variable)) ||
                (!plan->separator_is_key1 &&
                 (provider->var2 != plan->separator_variable ||
                  provider->var1 != plan->tail_variable)))
            {
                elog(ERROR,
                     "invalid AGE Generic Join GHD separator provider");
            }

            found_leaf_tail = true;
            increment_generic_counter(
                &state->separator_leaf_tail_providers);
            increment_generic_counter(
                &state->separator_descriptor_applications);
            separator_domain = build_generic_tail_separator_domain(
                state, provider, plan->tail_variable,
                plan->separator_is_key1, local_domains, reduction_context,
                separator_domains[plan->separator_variable].initialized,
                &separator_domain_count);
            if (separator_domain_count <= 0)
            {
                state->exhausted = true;
                break;
            }

            if (!separator_domains[plan->separator_variable].initialized)
            {
                separator_domains[plan->separator_variable].keys =
                    separator_domain;
                separator_domains[plan->separator_variable].count =
                    separator_domain_count;
                separator_domains[plan->separator_variable].initialized =
                    true;
            }
            else
            {
                separator_domains[plan->separator_variable].count =
                    intersect_generic_domains(
                        state,
                        separator_domains[plan->separator_variable].keys,
                        separator_domains[plan->separator_variable].count,
                        separator_domain, separator_domain_count);
                if (separator_domains[plan->separator_variable].count <= 0)
                {
                    state->exhausted = true;
                    break;
                }
            }
        }
    }
    else
    {
        for (provider_index = 0;
             provider_index < state->provider_count;
             provider_index++)
        {
            AgeGenericProvider *provider = &state->providers[provider_index];
            graphid *separator_domain;
            int separator_domain_count;
            int separator_variable;
            int tail_variable;
            bool separator_is_key1;

            if (!generic_leaf_tail_edge_info(provider, core_variables,
                                             &separator_variable,
                                             &tail_variable,
                                             &separator_is_key1))
            {
                continue;
            }

            found_leaf_tail = true;
            increment_generic_counter(
                &state->separator_leaf_tail_providers);
            separator_domain = build_generic_tail_separator_domain(
                state, provider, tail_variable, separator_is_key1,
                local_domains, reduction_context,
                separator_domains[separator_variable].initialized,
                &separator_domain_count);
            if (separator_domain_count <= 0)
            {
                state->exhausted = true;
                break;
            }

            if (!separator_domains[separator_variable].initialized)
            {
                separator_domains[separator_variable].keys = separator_domain;
                separator_domains[separator_variable].count =
                    separator_domain_count;
                separator_domains[separator_variable].initialized = true;
            }
            else
            {
                separator_domains[separator_variable].count =
                    intersect_generic_domains(
                        state,
                        separator_domains[separator_variable].keys,
                        separator_domains[separator_variable].count,
                        separator_domain, separator_domain_count);
                if (separator_domains[separator_variable].count <= 0)
                {
                    state->exhausted = true;
                    break;
                }
            }
        }
    }

    if (state->exhausted)
    {
        if (!found_leaf_tail)
            return;
    }

    if (!found_leaf_tail)
        return;

    increment_generic_counter(&state->separator_reduction_passes);
    if (state->exhausted)
        return;

    for (variable = 0; variable < state->variable_count; variable++)
    {
        AgeGenericDomain separator_domain;

        if (!separator_domains[variable].initialized)
            continue;
        separator_domain.keys = separator_domains[variable].keys;
        separator_domain.count = separator_domains[variable].count;
        add_generic_counter(&state->separator_domain_keys,
                            separator_domain.count);

        for (provider_index = 0;
             provider_index < state->provider_count;
             provider_index++)
        {
            AgeGenericProvider *provider = &state->providers[provider_index];
            int64 rows_removed;
            int separator_variable;
            int tail_variable;
            bool separator_is_key1;

            if (!generic_provider_has_variable(provider, variable))
                continue;

            rows_removed = filter_generic_provider_on_variable(
                provider, variable, &separator_domain);
            if (generic_provider_is_cyclic_core(provider, core_variables))
            {
                add_generic_counter(
                    &state->separator_cyclic_core_rows_removed,
                    rows_removed);
            }
            else if (generic_provider_is_planned_leaf_tail(
                         state, provider_index) ||
                     generic_leaf_tail_edge_info(provider, core_variables,
                                                 &separator_variable,
                                                 &tail_variable,
                                                 &separator_is_key1))
            {
                add_generic_counter(
                    &state->separator_leaf_tail_rows_removed,
                    rows_removed);
            }
            add_generic_counter(&state->semijoin_rows_removed,
                                rows_removed);
            if (provider->row_count <= 0)
            {
                state->exhausted = true;
                return;
            }
        }
    }
}

/*
 * Enforce query-wide endpoint consistency before variable DFS.  Acyclic
 * leaf-peel metadata gives a Yannakakis-style bottom-up pass followed by a
 * top-down pass.  Other shapes keep bounded fixed-point pruning; stopping
 * after the budget affects only pruning strength, never result correctness.
 */
static int
generic_semijoin_pass_budget(AgeGenericJoinState *state)
{
    if (state->reduction_order_kind == AGE_GENERIC_REDUCTION_ORDER_LEAF_PEEL &&
        state->reduction_order_edge_count > 0)
    {
        return 2;
    }
    return Max(state->variable_count * 2, 1);
}

static void
reduce_generic_providers(AgeGenericJoinState *state)
{
    MemoryContext reduction_context;
    int max_passes = generic_semijoin_pass_budget(state);
    int pass;

    reduction_context = AllocSetContextCreate(
        state->css.ss.ps.state->es_query_cxt,
        "AGE Generic Join semijoin reduction", ALLOCSET_SMALL_SIZES);
    state->semijoin_provider_rows_after = 0;
    state->semijoin_final_domain_keys = 0;
    reduce_generic_leaf_tail_separators(state, reduction_context);
    state->peak_memory = Max(
        state->peak_memory,
        MemoryContextMemAllocated(state->data_context, true) +
        MemoryContextMemAllocated(reduction_context, true));
    if (state->exhausted)
    {
        state->semijoin_provider_rows_after =
            generic_provider_row_total(state);
        state->semijoin_final_domain_keys = 0;
        MemoryContextDelete(reduction_context);
        return;
    }
    for (pass = 0; pass < max_passes; pass++)
    {
        AgeGenericDomain *domains;
        int64 rows_removed = 0;
        bool ordered_reduction_applicable;

        MemoryContextReset(reduction_context);
        domains = MemoryContextAllocZero(
            reduction_context,
            sizeof(AgeGenericDomain) * state->variable_count);
        increment_generic_counter(&state->semijoin_passes);
        if (!build_generic_reduction_domains(state, reduction_context,
                                             domains))
        {
            state->peak_memory = Max(
                state->peak_memory,
                MemoryContextMemAllocated(state->data_context, true) +
                MemoryContextMemAllocated(reduction_context, true));
            state->semijoin_provider_rows_after =
                generic_provider_row_total(state);
            state->semijoin_final_domain_keys = 0;
            state->exhausted = true;
            break;
        }

        state->peak_memory = Max(
            state->peak_memory,
            MemoryContextMemAllocated(state->data_context, true) +
            MemoryContextMemAllocated(reduction_context, true));

        ordered_reduction_applicable = generic_reduction_order_applicable(
            state);
        if (ordered_reduction_applicable)
        {
            bool bottom_up = pass == 0;

            rows_removed = filter_generic_providers_in_reduction_order(
                state, domains, bottom_up);
            if (bottom_up)
            {
                increment_generic_counter(
                    &state->semijoin_bottom_up_passes);
                add_generic_counter(
                    &state->semijoin_bottom_up_rows_removed,
                    rows_removed);
            }
            else
            {
                increment_generic_counter(
                    &state->semijoin_top_down_passes);
                add_generic_counter(
                    &state->semijoin_top_down_rows_removed,
                    rows_removed);
            }
        }
        else
        {
            rows_removed =
                filter_generic_providers_in_default_order(state, domains);
        }

        add_generic_counter(&state->semijoin_rows_removed, rows_removed);
        state->semijoin_provider_rows_after =
            generic_provider_row_total(state);
        state->semijoin_final_domain_keys =
            state->exhausted ? 0 : generic_domain_key_total(state, domains);
        if (state->exhausted ||
            (!ordered_reduction_applicable && rows_removed == 0))
            break;
    }
    MemoryContextDelete(reduction_context);
}

static void
build_generic_level(AgeGenericJoinState *state, int depth)
{
    AgeGenericLevel *level = &state->levels[depth];
    int candidate_count = 0;
    int provider_index;
    bool found_provider = false;

    MemoryContextReset(level->context);
    level->candidates = NULL;
    for (provider_index = 0;
         provider_index < state->provider_count;
         provider_index++)
    {
        AgeGenericProvider *provider = &state->providers[provider_index];
        AgeGenericDomainView view;

        if (provider->var1 != depth && provider->var2 != depth)
            continue;
        found_provider = true;
        if (!build_generic_domain_view(state, provider, depth, &view))
        {
            candidate_count = 0;
            break;
        }
        if (level->candidates == NULL)
        {
            candidate_count = copy_generic_domain_view(state, level, &view);
            level->candidates = level->candidate_buffer;
        }
        else
        {
            candidate_count = intersect_generic_candidates_with_view(
                state, level->candidates, candidate_count, &view);
            if (candidate_count == 0)
                break;
        }
    }

    if (!found_provider)
        level->candidates = NULL;
    level->candidate_count = found_provider ? candidate_count : 0;
    level->next_candidate = 0;
    level->initialized = true;
}

static void
reset_generic_level(AgeGenericJoinState *state, int depth)
{
    AgeGenericLevel *level = &state->levels[depth];

    MemoryContextReset(level->context);
    level->candidates = NULL;
    level->candidate_count = 0;
    level->next_candidate = 0;
    level->initialized = false;
}

static bool
prepare_generic_bags(AgeGenericJoinState *state)
{
    int provider_index;
    int64 flat_rows = 1;

    for (provider_index = 0;
         provider_index < state->provider_count;
         provider_index++)
    {
        AgeGenericProvider *provider = &state->providers[provider_index];
        graphid key1 = state->bindings[provider->var1];
        int start;
        int end;

        if (provider->kind == AGE_GENERIC_PROVIDER_VERTEX)
        {
            generic_provider_key1_range(state, provider, key1, &start, &end);
        }
        else
        {
            graphid key2 = state->bindings[provider->var2];

            generic_provider_pair_range(state, provider, key1, key2, &start,
                                        &end);
        }
        provider->bag_start = start;
        provider->bag_count = end - start;
        if (provider->bag_count <= 0)
            return false;
        if (flat_rows > PG_INT64_MAX / provider->bag_count)
            flat_rows = PG_INT64_MAX;
        else
            flat_rows *= provider->bag_count;
        state->enumeration_order[provider_index] = provider_index;
        state->combination_indexes[provider_index] = -1;
        state->selected_edge_id_valid[provider_index] = false;
    }

    for (provider_index = 1;
         provider_index < state->provider_count;
         provider_index++)
    {
        int selected = state->enumeration_order[provider_index];
        int position = provider_index;

        while (position > 0 &&
               state->providers[state->enumeration_order[position - 1]].
                   bag_count > state->providers[selected].bag_count)
        {
            state->enumeration_order[position] =
                state->enumeration_order[position - 1];
            position--;
        }
        state->enumeration_order[position] = selected;
    }
    state->combination_started = false;
    state->binding_ready = true;
    increment_generic_counter(&state->bindings_completed);
    add_generic_counter(&state->candidate_flat_rows, flat_rows);
    return true;
}

static bool
next_generic_binding(AgeGenericJoinState *state)
{
    int depth;

    if (state->exhausted)
        return false;
    materialize_generic_providers(state);
    if (state->exhausted)
        return false;

    depth = state->search_started ? state->variable_count - 1 : 0;
    state->search_started = true;
    while (depth >= 0)
    {
        AgeGenericLevel *level = &state->levels[depth];

        if (!level->initialized)
            build_generic_level(state, depth);
        if (level->next_candidate < level->candidate_count)
        {
            state->bindings[depth] =
                level->candidates[level->next_candidate++];
            if (depth == state->variable_count - 1)
            {
                if (prepare_generic_bags(state))
                    return true;
                continue;
            }
            depth++;
            reset_generic_level(state, depth);
            continue;
        }

        reset_generic_level(state, depth);
        depth--;
    }

    state->exhausted = true;
    return false;
}

static bool
next_generic_combination(AgeGenericJoinState *state)
{
    int depth;

    if (!state->binding_ready)
        return false;
    depth = state->combination_started ? state->provider_count - 1 : 0;
    state->combination_started = true;

    while (depth >= 0)
    {
        int provider_index = state->enumeration_order[depth];
        AgeGenericProvider *provider = &state->providers[provider_index];
        int local_index = state->combination_indexes[provider_index] + 1;

        state->selected_edge_id_valid[depth] = false;
        while (local_index < provider->bag_count)
        {
            AgeGenericRow *row = &provider->rows[
                provider->bag_start + local_index];
            bool conflict = false;
            int previous_depth;

            state->combination_indexes[provider_index] = local_index;
            if (row->edge_id_valid)
            {
                for (previous_depth = 0;
                     previous_depth < depth;
                     previous_depth++)
                {
                    int previous_provider =
                        state->enumeration_order[previous_depth];

                    if (state->selected_edge_id_valid[previous_depth] &&
                        state->selected_edge_ids[previous_depth] ==
                            row->edge_id &&
                        bms_overlap(
                            state->uniqueness_groups[provider_index],
                            state->uniqueness_groups[previous_provider]))
                    {
                        conflict = true;
                        break;
                    }
                }
                if (conflict)
                {
                    increment_generic_counter(&state->uniqueness_rejects);
                    local_index++;
                    continue;
                }
                state->selected_edge_ids[depth] = row->edge_id;
                state->selected_edge_id_valid[depth] = true;
            }

            if (depth == state->provider_count - 1)
            {
                increment_generic_counter(&state->candidate_combinations);
                return true;
            }
            depth++;
            provider_index = state->enumeration_order[depth];
            state->combination_indexes[provider_index] = -1;
            state->selected_edge_id_valid[depth] = false;
            break;
        }

        if (local_index < provider->bag_count)
            continue;
        state->combination_indexes[provider_index] = -1;
        state->selected_edge_id_valid[depth] = false;
        depth--;
    }

    state->binding_ready = false;
    state->combination_started = false;
    return false;
}

static TupleTableSlot *
materialize_generic_combination(AgeGenericJoinState *state)
{
    TupleTableSlot *raw_slot = state->css.ss.ss_ScanTupleSlot;
    int raw_index = 0;
    int provider_index;

    ExecClearTuple(raw_slot);
    for (provider_index = 0;
         provider_index < state->provider_count;
         provider_index++)
    {
        AgeGenericProvider *provider = &state->providers[provider_index];
        AgeGenericRow *row = &provider->rows[
            provider->bag_start +
            state->combination_indexes[provider_index]];
        int attr_index;

        if (provider->key_only_output)
        {
            for (attr_index = 0; attr_index < provider->output_width;
                 attr_index++)
            {
                if (raw_index >= raw_slot->tts_tupleDescriptor->natts)
                    elog(ERROR, "AGE Generic Join raw tuple width overflow");
                switch (provider->output_sources[attr_index])
                {
                case AGE_GENERIC_OUTPUT_SOURCE_KEY1:
                    raw_slot->tts_values[raw_index] =
                        GRAPHID_GET_DATUM(row->key1);
                    raw_slot->tts_isnull[raw_index] = false;
                    break;
                case AGE_GENERIC_OUTPUT_SOURCE_KEY2:
                    raw_slot->tts_values[raw_index] =
                        GRAPHID_GET_DATUM(row->key2);
                    raw_slot->tts_isnull[raw_index] = false;
                    break;
                case AGE_GENERIC_OUTPUT_SOURCE_EDGE_ID:
                    if (row->edge_id_valid)
                    {
                        raw_slot->tts_values[raw_index] =
                            GRAPHID_GET_DATUM(row->edge_id);
                        raw_slot->tts_isnull[raw_index] = false;
                    }
                    else
                    {
                        raw_slot->tts_values[raw_index] = (Datum)0;
                        raw_slot->tts_isnull[raw_index] = true;
                    }
                    break;
                default:
                    elog(ERROR, "invalid AGE Generic Join key-only output");
                }
                raw_index++;
            }
        }
        else
        {
            if (row->tuple == NULL || provider->tuple_slot == NULL)
                elog(ERROR, "invalid AGE Generic Join materialized tuple");
            ExecStoreMinimalTuple(row->tuple, provider->tuple_slot, false);
            slot_getallattrs(provider->tuple_slot);
            for (attr_index = 0; attr_index < provider->output_width;
                 attr_index++)
            {
                if (raw_index >= raw_slot->tts_tupleDescriptor->natts)
                    elog(ERROR, "AGE Generic Join raw tuple width overflow");
                raw_slot->tts_values[raw_index] =
                    provider->tuple_slot->tts_values[attr_index];
                raw_slot->tts_isnull[raw_index] =
                    provider->tuple_slot->tts_isnull[attr_index];
                raw_index++;
            }
        }
    }
    if (raw_index != raw_slot->tts_tupleDescriptor->natts)
    {
        elog(ERROR, "AGE Generic Join raw tuple width mismatch: %d versus %d",
             raw_index, raw_slot->tts_tupleDescriptor->natts);
    }
    ExecStoreVirtualTuple(raw_slot);
    return raw_slot;
}

static TupleTableSlot *
access_age_generic_join(ScanState *node)
{
    AgeGenericJoinState *state = (AgeGenericJoinState *)node;

    for (;;)
    {
        if (!state->binding_ready && !next_generic_binding(state))
            return ExecClearTuple(node->ss_ScanTupleSlot);
        if (next_generic_combination(state))
            return materialize_generic_combination(state);
    }
}

static TupleTableSlot *
exec_age_generic_join(CustomScanState *node)
{
    AgeGenericJoinState *state = (AgeGenericJoinState *)node;
    TupleTableSlot *slot;

    slot = ExecScan(&node->ss, access_age_generic_join,
                    recheck_age_generic_join);
    if (!TupIsNull(slot))
        increment_generic_counter(&state->rows_emitted);
    return slot;
}

static bool
recheck_age_generic_join(ScanState *node, TupleTableSlot *slot)
{
    (void)node;
    (void)slot;
    return true;
}

static void
initialize_generic_uniqueness_groups(AgeGenericJoinState *state,
                                     List *group_descs)
{
    ListCell *group_desc_cell;
    int provider_index = 0;

    foreach(group_desc_cell, group_descs)
    {
        List *group_ids = lfirst(group_desc_cell);
        ListCell *group_cell;

        foreach(group_cell, group_ids)
        {
            int group_id = intVal(lfirst(group_cell));

            if (group_id < 0)
                elog(ERROR, "invalid AGE Generic Join uniqueness group");
            state->uniqueness_groups[provider_index] = bms_add_member(
                state->uniqueness_groups[provider_index], group_id);
            state->uniqueness_group_count = Max(
                state->uniqueness_group_count, group_id + 1);
        }
        provider_index++;
    }
}

static bool
generic_provider_output_source(AgeGenericProvider *provider, AttrNumber attno,
                               Oid typid, AgeGenericOutputSource *source)
{
    if (typid != GRAPHIDOID)
        return false;
    if (attno == provider->key1_attno)
    {
        *source = AGE_GENERIC_OUTPUT_SOURCE_KEY1;
        return true;
    }
    if (provider->kind == AGE_GENERIC_PROVIDER_EDGE &&
        attno == provider->key2_attno)
    {
        *source = AGE_GENERIC_OUTPUT_SOURCE_KEY2;
        return true;
    }
    if (provider->kind == AGE_GENERIC_PROVIDER_EDGE &&
        attno == provider->edge_id_attno)
    {
        *source = AGE_GENERIC_OUTPUT_SOURCE_EDGE_ID;
        return true;
    }
    return false;
}

static void
initialize_generic_provider_output(AgeGenericProvider *provider,
                                   TupleDesc tuple_desc, EState *estate)
{
    MemoryContext oldcontext;
    int attr_index;

    if (tuple_desc->natts <= 0)
        elog(ERROR, "AGE Generic Join provider has no output columns");
    if ((Size)tuple_desc->natts >
        MaxAllocSize / sizeof(AgeGenericOutputSource))
    {
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("AGE Generic Join provider output is too wide")));
    }

    provider->output_width = tuple_desc->natts;
    provider->key_only_output = true;

    oldcontext = MemoryContextSwitchTo(estate->es_query_cxt);
    provider->output_sources =
        palloc0(sizeof(AgeGenericOutputSource) * provider->output_width);
    MemoryContextSwitchTo(oldcontext);

    for (attr_index = 0; attr_index < provider->output_width; attr_index++)
    {
        Form_pg_attribute attr = TupleDescAttr(tuple_desc, attr_index);
        AttrNumber attno = (AttrNumber)(attr_index + 1);
        AgeGenericOutputSource source = AGE_GENERIC_OUTPUT_SOURCE_NONE;

        if (attr->attisdropped ||
            !generic_provider_output_source(provider, attno, attr->atttypid,
                                            &source))
        {
            provider->key_only_output = false;
            break;
        }
        provider->output_sources[attr_index] = source;
    }

    if (!provider->key_only_output)
    {
        provider->tuple_slot = ExecInitExtraTupleSlot(
            estate, tuple_desc, &TTSOpsMinimalTuple);
    }
}

static void
initialize_generic_provider(AgeGenericJoinState *state,
                            AgeGenericProvider *provider,
                            PlanState *plan_state, List *desc,
                            EState *estate)
{
    TupleDesc tuple_desc = ExecGetResultType(plan_state);

    if (list_length(desc) != AGE_GENERIC_PROVIDER_DESC_COUNT)
        elog(ERROR, "invalid AGE Generic Join provider descriptor");

    provider->kind = (AgeGenericProviderKind)intVal(list_nth(
        desc, AGE_GENERIC_PROVIDER_DESC_KIND));
    provider->var1 = intVal(list_nth(desc, AGE_GENERIC_PROVIDER_DESC_VAR1));
    provider->var2 = intVal(list_nth(desc, AGE_GENERIC_PROVIDER_DESC_VAR2));
    provider->key1_attno = (AttrNumber)intVal(list_nth(
        desc, AGE_GENERIC_PROVIDER_DESC_KEY1_ATTNO));
    provider->key2_attno = (AttrNumber)intVal(list_nth(
        desc, AGE_GENERIC_PROVIDER_DESC_KEY2_ATTNO));
    provider->edge_id_attno = (AttrNumber)intVal(list_nth(
        desc, AGE_GENERIC_PROVIDER_DESC_EDGE_ID_ATTNO));
    provider->plan_state = plan_state;
    provider->trie_ops = &sorted_array_trie_ops;

    if ((provider->kind != AGE_GENERIC_PROVIDER_VERTEX &&
         provider->kind != AGE_GENERIC_PROVIDER_EDGE) ||
        provider->var1 < 0 || provider->var1 >= state->variable_count ||
        provider->key1_attno <= 0 || provider->key1_attno > tuple_desc->natts)
    {
        elog(ERROR, "invalid AGE Generic Join provider keys");
    }
    if (provider->kind == AGE_GENERIC_PROVIDER_EDGE &&
        (provider->var2 <= provider->var1 ||
         provider->var2 >= state->variable_count ||
         provider->key2_attno <= 0 ||
         provider->key2_attno > tuple_desc->natts ||
         provider->edge_id_attno <= 0 ||
         provider->edge_id_attno > tuple_desc->natts))
    {
        elog(ERROR, "invalid AGE Generic Join edge provider");
    }

    initialize_generic_provider_output(provider, tuple_desc, estate);
}

static void
begin_age_generic_join(CustomScanState *node, EState *estate, int eflags)
{
    AgeGenericJoinState *state = (AgeGenericJoinState *)node;
    CustomScan *cscan = castNode(CustomScan, node->ss.ps.plan);
    List *provider_descs = list_nth(
        cscan->custom_private, AGE_GENERIC_JOIN_PRIVATE_PROVIDER_DESCS);
    List *uniqueness_group_descs = list_nth(
        cscan->custom_private, AGE_GENERIC_JOIN_PRIVATE_UNIQUENESS_GROUPS);
    ListCell *plan_cell;
    ListCell *desc_cell;
    int provider_index = 0;
    int depth;

    foreach(plan_cell, cscan->custom_plans)
    {
        Plan *subplan = (Plan *)lfirst(plan_cell);

        node->custom_ps = lappend(node->custom_ps,
                                  ExecInitNode(subplan, estate, eflags));
    }
    if (list_length(node->custom_ps) != state->provider_count ||
        list_length(provider_descs) != state->provider_count ||
        list_length(uniqueness_group_descs) != state->provider_count)
    {
        elog(ERROR, "AGE Generic Join child count does not match providers");
    }

    state->providers = palloc0(sizeof(AgeGenericProvider) *
                                state->provider_count);
    state->levels = palloc0(sizeof(AgeGenericLevel) * state->variable_count);
    state->bindings = palloc0(sizeof(graphid) * state->variable_count);
    state->combination_indexes = palloc0(sizeof(int) * state->provider_count);
    state->enumeration_order = palloc0(sizeof(int) * state->provider_count);
    state->selected_edge_ids = palloc0(sizeof(graphid) *
                                        state->provider_count);
    state->selected_edge_id_valid = palloc0(sizeof(bool) *
                                             state->provider_count);
    state->uniqueness_groups = palloc0(sizeof(Bitmapset *) *
                                        state->provider_count);
    state->data_context = AllocSetContextCreate(
        estate->es_query_cxt, "AGE Generic Join provider rows",
        ALLOCSET_DEFAULT_SIZES);

    initialize_generic_uniqueness_groups(state, uniqueness_group_descs);

    provider_index = 0;
    forboth(plan_cell, node->custom_ps, desc_cell, provider_descs)
    {
        PlanState *plan_state = lfirst(plan_cell);
        List *desc = lfirst(desc_cell);
        AgeGenericProvider *provider = &state->providers[provider_index];

        initialize_generic_provider(state, provider, plan_state, desc, estate);
        provider_index++;
    }

    for (depth = 0; depth < state->variable_count; depth++)
    {
        state->levels[depth].context = AllocSetContextCreate(
            estate->es_query_cxt, "AGE Generic Join variable domain",
            ALLOCSET_SMALL_SIZES);
    }
}

static void
reset_age_generic_join_state(AgeGenericJoinState *state)
{
    int provider_index;
    int depth;

    if (state->data_context != NULL)
        MemoryContextReset(state->data_context);
    for (provider_index = 0;
         provider_index < state->provider_count;
         provider_index++)
    {
        AgeGenericProvider *provider = &state->providers[provider_index];

        provider->rows = NULL;
        provider->row_count = 0;
        provider->row_capacity = 0;
        provider->bag_start = 0;
        provider->bag_count = 0;
        provider->prefix_ranges = NULL;
        invalidate_generic_provider_prefix_ranges(provider);
        state->combination_indexes[provider_index] = -1;
        state->selected_edge_id_valid[provider_index] = false;
    }
    state->reduction_scratch.keys = NULL;
    state->reduction_scratch.capacity = 0;
    for (depth = 0; depth < state->variable_count; depth++)
    {
        state->levels[depth].candidate_buffer = NULL;
        state->levels[depth].candidate_capacity = 0;
        reset_generic_level(state, depth);
    }
    state->search_started = false;
    state->binding_ready = false;
    state->combination_started = false;
    state->materialized = false;
    state->exhausted = false;
}

static void
rescan_age_generic_join(CustomScanState *node)
{
    AgeGenericJoinState *state = (AgeGenericJoinState *)node;
    int provider_index;

    for (provider_index = 0;
         provider_index < state->provider_count;
         provider_index++)
    {
        ExecReScan(state->providers[provider_index].plan_state);
    }
    reset_age_generic_join_state(state);
    increment_generic_counter(&state->rescans);
}

static void
end_age_generic_join(CustomScanState *node)
{
    AgeGenericJoinState *state = (AgeGenericJoinState *)node;
    int provider_index;
    int depth;

    for (provider_index = 0;
         provider_index < state->provider_count;
         provider_index++)
    {
        ExecEndNode(state->providers[provider_index].plan_state);
    }
    for (depth = 0; depth < state->variable_count; depth++)
    {
        if (state->levels[depth].context != NULL)
            MemoryContextDelete(state->levels[depth].context);
    }
    if (state->data_context != NULL)
        MemoryContextDelete(state->data_context);
}

static char *
format_generic_variable_order(AgeGenericJoinState *state)
{
    StringInfoData buffer;
    int depth;

    initStringInfo(&buffer);
    for (depth = 0; depth < state->variable_count; depth++)
    {
        appendStringInfo(&buffer, "%s%d(rti=%u)", depth == 0 ? "" : ", ",
                         depth + 1, state->variable_rtis[depth]);
    }
    return buffer.data;
}

static char *
format_generic_component_ids(AgeGenericJoinState *state)
{
    StringInfoData buffer;
    int variable;

    initStringInfo(&buffer);
    for (variable = 0; variable < state->variable_count; variable++)
    {
        appendStringInfo(&buffer, "%s%d", variable == 0 ? "" : ", ",
                         state->variable_component_ids[variable] + 1);
    }
    return buffer.data;
}

static char *
format_generic_ghd_separators(AgeGenericJoinState *state)
{
    StringInfoData buffer;
    int separator_index;

    initStringInfo(&buffer);
    if (state->reduction_ghd_separators == NULL ||
        state->reduction_ghd_separator_count <= 0)
    {
        appendStringInfoString(&buffer, "none");
        return buffer.data;
    }

    for (separator_index = 0;
         separator_index < state->reduction_ghd_separator_count;
         separator_index++)
    {
        AgeGenericGHDSeparatorPlan *separator;

        separator = &state->reduction_ghd_separators[separator_index];
        appendStringInfo(&buffer,
                         "%sprovider %d: v%d->v%d via %s",
                         separator_index == 0 ? "" : ", ",
                         separator->provider_index + 1,
                         separator->separator_variable + 1,
                         separator->tail_variable + 1,
                         separator->separator_is_key1 ? "key1" : "key2");
    }

    return buffer.data;
}

static const char *
generic_reduction_shape_name(AgeGenericReductionShape shape)
{
    switch (shape)
    {
    case AGE_GENERIC_REDUCTION_ALPHA_ACYCLIC:
        return "alpha-acyclic";
    case AGE_GENERIC_REDUCTION_CYCLIC_CORE:
        return "cyclic-core";
    case AGE_GENERIC_REDUCTION_CYCLIC_WITH_TAIL:
        return "cyclic-with-tail";
    }
    return "unknown";
}

static const char *
generic_reduction_order_name(AgeGenericReductionOrderKind kind)
{
    switch (kind)
    {
    case AGE_GENERIC_REDUCTION_ORDER_NONE:
        return "none";
    case AGE_GENERIC_REDUCTION_ORDER_LEAF_PEEL:
        return "leaf-peel";
    }
    return "unknown";
}

static const char *
generic_reduction_descriptor_source_name(
    AgeGenericReductionDescriptorSource source)
{
    switch (source)
    {
    case AGE_GENERIC_REDUCTION_SOURCE_LOCAL:
        return "local-hypergraph";
    case AGE_GENERIC_REDUCTION_SOURCE_GRAPH_JOIN_MATCH_IR:
        return "graph-join-match-ir";
    }
    return "unknown";
}

static void
explain_age_generic_join(CustomScanState *node, List *ancestors,
                         ExplainState *es)
{
    AgeGenericJoinState *state = (AgeGenericJoinState *)node;
    char *variable_order = format_generic_variable_order(state);
    char *component_ids = format_generic_component_ids(state);
    char *ghd_separators = format_generic_ghd_separators(state);

    (void)ancestors;
    ExplainPropertyText("Generic Join Algorithm",
                        "lazy domain views with vectorized intersections and delayed edge bags",
                        es);
    ExplainPropertyText("Provider Trie Ops", sorted_array_trie_ops.name, es);
    ExplainPropertyText("Variable Order", variable_order, es);
    ExplainPropertyInteger("Component Count", NULL, state->component_count,
                           es);
    ExplainPropertyText("Component IDs", component_ids, es);
    ExplainPropertyText("Reduction Shape",
                        generic_reduction_shape_name(state->reduction_shape),
                        es);
    ExplainPropertyText(
        "Reduction Descriptor Source",
        generic_reduction_descriptor_source_name(
            state->reduction_descriptor_source), es);
    ExplainPropertyText(
        "Reduction Order",
        generic_reduction_order_name(state->reduction_order_kind), es);
    ExplainPropertyInteger("Reduction Order Edges", NULL,
                           state->reduction_order_edge_count, es);
    ExplainPropertyInteger("Reduction Core Variables", NULL,
                           state->reduction_core_variables, es);
    ExplainPropertyInteger("Reduction Tail Separators", NULL,
                           state->reduction_tail_separators, es);
    ExplainPropertyInteger("GHD Bag Count", NULL,
                           state->reduction_ghd_bag_count, es);
    ExplainPropertyInteger("GHD Separator Count", NULL,
                           state->reduction_ghd_separator_count, es);
    ExplainPropertyText("GHD Separators", ghd_separators, es);
    ExplainPropertyText(
        "GHD Descriptor Source",
        generic_reduction_descriptor_source_name(
            state->reduction_descriptor_source), es);
    ExplainPropertyInteger("Variable Count", NULL, state->variable_count, es);
    ExplainPropertyInteger("Provider Count", NULL, state->provider_count, es);
    if (es->analyze)
    {
        int64 flat_rows_avoided;

        flat_rows_avoided =
            state->candidate_flat_rows > state->candidate_combinations ?
            state->candidate_flat_rows - state->candidate_combinations : 0;

        ExplainPropertyInteger("Provider Rows Materialized", NULL,
                               state->rows_materialized, es);
        ExplainPropertyInteger("Provider Tuples Materialized", NULL,
                               state->tuples_materialized, es);
        if (state->providers_skipped_after_empty > 0)
        {
            ExplainPropertyInteger("Providers Skipped After Empty", NULL,
                                   state->providers_skipped_after_empty, es);
        }
        ExplainPropertyInteger("Semijoin Reduction Passes", NULL,
                               state->semijoin_passes, es);
        ExplainPropertyInteger("Yannakakis Bottom-Up Passes", NULL,
                               state->semijoin_bottom_up_passes, es);
        ExplainPropertyInteger("Yannakakis Top-Down Passes", NULL,
                               state->semijoin_top_down_passes, es);
        ExplainPropertyBool("Reduction Order Applied",
                            state->reduction_order_applied, es);
        ExplainPropertyInteger("Semijoin Rows Removed", NULL,
                               state->semijoin_rows_removed, es);
        ExplainPropertyInteger("Yannakakis Bottom-Up Rows Removed", NULL,
                               state->semijoin_bottom_up_rows_removed, es);
        ExplainPropertyInteger("Yannakakis Top-Down Rows Removed", NULL,
                               state->semijoin_top_down_rows_removed, es);
        ExplainPropertyInteger("Semijoin Provider Rows After", NULL,
                               state->semijoin_provider_rows_after, es);
        ExplainPropertyInteger("Semijoin Final Domain Keys", NULL,
                               state->semijoin_final_domain_keys, es);
        ExplainPropertyInteger("GHD Separator Reduction Passes", NULL,
                               state->separator_reduction_passes, es);
        ExplainPropertyInteger("GHD Leaf Tail Providers", NULL,
                               state->separator_leaf_tail_providers, es);
        ExplainPropertyInteger("GHD Descriptor Separators Applied", NULL,
                               state->separator_descriptor_applications, es);
        ExplainPropertyInteger("GHD Tail Domain Propagation Passes", NULL,
                               state->separator_tail_domain_passes, es);
        ExplainPropertyInteger("GHD Tail Domain Rows Removed", NULL,
                               state->separator_tail_domain_rows_removed, es);
        ExplainPropertyInteger("GHD Separator Domain Keys", NULL,
                               state->separator_domain_keys, es);
        ExplainPropertyInteger("GHD Leaf Tail Rows Removed", NULL,
                               state->separator_leaf_tail_rows_removed, es);
        ExplainPropertyInteger("GHD Cyclic Core Rows Removed", NULL,
                               state->separator_cyclic_core_rows_removed, es);
        ExplainPropertyInteger("Complete Bindings", NULL,
                               state->bindings_completed, es);
        ExplainPropertyInteger("Candidate Flat Rows", NULL,
                               state->candidate_flat_rows, es);
        ExplainPropertyInteger("Candidate Bag Combinations", NULL,
                               state->candidate_combinations, es);
        ExplainPropertyInteger("Flat Rows Avoided", NULL,
                               flat_rows_avoided, es);
        ExplainPropertyInteger("Lazy Domain Views", NULL,
                               state->lazy_domain_views, es);
        ExplainPropertyInteger("Lazy Domain Keys Scanned", NULL,
                               state->lazy_domain_keys_scanned, es);
        ExplainPropertyInteger("Domain Scratch Allocations", NULL,
                               state->lazy_domain_scratch_allocations, es);
        ExplainPropertyInteger("Domain Scratch Reuses", NULL,
                               state->lazy_domain_scratch_reuses, es);
        ExplainPropertyInteger("Prefix Range Builds", NULL,
                               state->lazy_prefix_range_builds, es);
        ExplainPropertyInteger("Prefix Range Reuses", NULL,
                               state->lazy_prefix_range_reuses, es);
        ExplainPropertyInteger("Prefix Range Directory Searches", NULL,
                               state->lazy_prefix_range_directory_searches, es);
        ExplainPropertyInteger("Prefix Range Cursor Reuses", NULL,
                               state->lazy_prefix_range_cursor_reuses, es);
        ExplainPropertyInteger("Trie Child Range Reuses", NULL,
                               state->trie_child_range_reuses, es);
        ExplainPropertyInteger("Trie Child Range Opens", NULL,
                               state->trie_child_range_opens, es);
        ExplainPropertyInteger("Trie Prefix Range Seeks", NULL,
                               state->trie_prefix_range_seeks, es);
        ExplainPropertyInteger("Trie Pair Range Opens", NULL,
                               state->trie_pair_range_opens, es);
        ExplainPropertyInteger("Reduction Scratch Allocations", NULL,
                               state->reduction_scratch_allocations, es);
        ExplainPropertyInteger("Reduction Scratch Reuses", NULL,
                               state->reduction_scratch_reuses, es);
        ExplainPropertyInteger("Vector Intersection Merge Calls", NULL,
                               state->vector_intersection_merge_calls, es);
        ExplainPropertyInteger("Vector Intersection Galloping Calls", NULL,
                               state->vector_intersection_galloping_calls,
                               es);
        ExplainPropertyInteger("Vector Intersection Galloping Steps", NULL,
                               state->vector_intersection_galloping_steps,
                               es);
        ExplainPropertyInteger("Uniqueness Constraint Groups", NULL,
                               state->uniqueness_group_count, es);
        ExplainPropertyInteger("Uniqueness Rejects", NULL,
                               state->uniqueness_rejects, es);
        ExplainPropertyInteger("Rows Emitted", NULL, state->rows_emitted, es);
        ExplainPropertyInteger("Rescans", NULL, state->rescans, es);
        ExplainPropertyInteger("Peak Generic Join Memory", "bytes",
                               (int64)state->peak_memory, es);
        ExplainPropertyInteger("Spill Bytes", "bytes",
                               state->spill_bytes, es);
    }
    pfree(variable_order);
    pfree(component_ids);
    pfree(ghd_separators);
}
