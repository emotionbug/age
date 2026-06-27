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

#include "access/detoast.h"
#include "access/age_adjacency.h"
#include "catalog/pg_type_d.h"
#include "commands/explain_format.h"
#include "commands/explain_state.h"
#include "common/int.h"
#include "executor/cypher_factorized_binding.h"
#include "executor/cypher_generic_join.h"
#include "executor/executor.h"
#include "lib/stringinfo.h"
#include "nodes/pg_list.h"
#include "utils/agtype.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/graphid.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/numeric.h"

#define AGE_GENERIC_GALLOPING_MIN_RATIO 8
#define AGE_GENERIC_PREFIX_RANGE_CACHE_SIZE 8
#define AGE_GENERIC_PREFIX_RANGE_DIRECTORY_MIN_MISSES 8
#define AGE_GENERIC_UNIQUENESS_EXACT_PAIR_LIMIT 4096
#define AGE_GENERIC_SMALL_BAG_ENUMERATOR_LIMIT 4096

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
    AttrNumber properties_attno;
    AgeGenericProviderPhysicalKind physical_kind;
    Oid adjacency_index_oid;
    int32 adjacency_terminal_label_id;
    AgeAdjacencyVisiblePayloadScan *payload_scan;
    PlanState *plan_state;
    TupleTableSlot *tuple_slot;
    AgeGenericOutputSource *output_sources;
    int output_width;
    bool key_only_output;
    AgeGenericRow *rows;
    int row_count;
    int row_capacity;
    AgeGenericRow *prefix_rows;
    int prefix_row_count;
    int prefix_row_capacity;
    graphid prefix_key;
    bool prefix_valid;
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
    AgeGenericRow *rows;
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

typedef struct AgeGenericGraphidPair
{
    graphid key1;
    graphid key2;
} AgeGenericGraphidPair;

typedef struct AgeGenericGHDSeparatorPlan
{
    AgeGenericGHDSeparatorKind kind;
    int parent_bag_id;
    int child_bag_id;
    int provider_index;
    int separator_variable;
    int tail_variable;
    bool separator_is_key1;
    int *separator_variables;
    int separator_variable_count;
} AgeGenericGHDSeparatorPlan;

typedef struct AgeGenericGHDBagPlan
{
    int id;
    int kind;
    int *variables;
    int variable_count;
    int *provider_indexes;
    int provider_count;
} AgeGenericGHDBagPlan;

typedef struct AgeGenericGraphidHashEntry
{
    graphid key;
} AgeGenericGraphidHashEntry;

typedef struct AgeGenericSemijoinStepPlan
{
    AgeGenericSemijoinStepPhase phase;
    int provider_index;
    int from_variable;
    int to_variable;
    int key_variable;
    bool key_is_key1;
} AgeGenericSemijoinStepPlan;

typedef enum AgeGenericGHDBagDescField
{
    AGE_GENERIC_GHD_BAG_DESC_ID = 0,
    AGE_GENERIC_GHD_BAG_DESC_KIND,
    AGE_GENERIC_GHD_BAG_DESC_VARIABLES,
    AGE_GENERIC_GHD_BAG_DESC_PROVIDERS,
    AGE_GENERIC_GHD_BAG_DESC_COUNT
} AgeGenericGHDBagDescField;

typedef enum AgeGenericGHDBagKind
{
    AGE_GENERIC_GHD_BAG_CYCLIC_CORE = 0,
    AGE_GENERIC_GHD_BAG_LEAF_TAIL
} AgeGenericGHDBagKind;

struct AgeGenericJoinState
{
    CustomScanState css;
    int variable_count;
    Index *variable_rtis;
    int provider_count;
    AgeGenericConsumerKind consumer_kind;
    AgeGenericRowGoalSource row_goal_source;
    Oid consumer_output_type;
    int64 row_goal;
    int64 row_goal_emitted;
    int distinct_variable;
    HTAB *distinct_key_hash;
    bool has_lazy_physical_provider;
    int lazy_physical_provider_count;
    int64 lazy_physical_prefix_loads;
    int64 lazy_physical_rows_read;
    int64 lazy_physical_row_bytes_allocated;
    AgeGenericProvider *providers;
    AgeGenericLevel *levels;
    graphid *bindings;
    bool search_started;
    bool binding_ready;
    int *combination_indexes;
    int *enumeration_order;
    graphid *selected_edge_ids;
    bool *selected_edge_id_valid;
    AgeBindingFlatEnumerator small_bag_enumerator;
    int64 *small_bag_row_multiplicities;
    int small_bag_row_multiplicity_capacity;
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
    AgeGenericGHDBagPlan *reduction_ghd_bags;
    int reduction_ghd_separator_count;
    AgeGenericGHDSeparatorPlan *reduction_ghd_separators;
    int reduction_semijoin_step_count;
    int reduction_semijoin_bottom_up_step_count;
    int reduction_semijoin_top_down_step_count;
    AgeGenericSemijoinStepPlan *reduction_semijoin_steps;
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
    int64 provider_rows_read;
    int64 provider_row_bytes_allocated;
    int64 provider_tuple_bytes_copied;
    int64 providers_skipped_after_empty;
    int64 semijoin_passes;
    int64 semijoin_bottom_up_passes;
    int64 semijoin_top_down_passes;
    int64 semijoin_steps_applied;
    int64 semijoin_bottom_up_steps_applied;
    int64 semijoin_top_down_steps_applied;
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
    int64 reduction_domain_builds;
    int64 reduction_domain_rows_scanned;
    int64 reduction_domain_keys_produced;
    int64 reduction_domain_sorts;
    int64 reduction_domain_sort_rows;
    int64 bindings_completed;
    int64 candidate_flat_rows;
    int64 candidate_combinations;
    int64 flat_rows_materialized;
    int64 consumer_flat_rows_avoided;
    bool count_executed;
    bool count_emitted;
    int64 count_result;
    bool count_multiplicity_fast_path;
    bool count_multiplicity_product_path_used;
    bool count_multiplicity_enumeration_used;
    bool count_multiplicity_small_bag_enumerator_used;
    int64 count_multiplicity_bindings;
    int64 count_multiplicity_rows;
    bool small_bag_enumerator_used;
    int64 small_bag_enumerator_bindings;
    int64 small_bag_enumerator_candidate_rows;
    int64 small_bag_enumerator_rows;
    int64 distinct_key_count;
    int property_aggregate_provider_index;
    AttrNumber property_aggregate_attno;
    Datum property_aggregate_key;
    bool property_aggregate_key_isnull;
    bool property_aggregate_executed;
    bool property_aggregate_emitted;
    bool property_aggregate_has_value;
    agtype_value property_aggregate_sum_value;
    agtype *property_aggregate_minmax_value;
    float8 property_aggregate_avg_sum;
    int64 property_aggregate_value_input_rows;
    int64 property_aggregate_input_rows;
    int64 property_aggregate_null_rows;
    bool exists_executed;
    bool exists_emitted;
    int64 uniqueness_rejects;
    int64 rows_emitted;
    int64 rescans;
    int64 spill_bytes;
    int64 lazy_domain_views;
    int64 lazy_domain_keys_scanned;
    int64 lazy_domain_scratch_allocations;
    int64 lazy_domain_scratch_reuses;
    int64 runtime_domain_builds;
    int64 runtime_domain_rows_scanned;
    int64 lazy_prefix_range_builds;
    int64 lazy_prefix_range_reuses;
    int64 lazy_prefix_range_directory_searches;
    int64 lazy_prefix_range_cursor_reuses;
    int64 prefix_range_cache_hits;
    int64 prefix_range_cache_misses;
    int64 child_range_cache_hits;
    int64 child_range_cache_misses;
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

static int64
add_generic_count_result(int64 count, int64 increment)
{
    int64 result;

    if (increment <= 0)
        return count;
    if (pg_add_s64_overflow(count, increment, &result))
    {
        ereport(ERROR,
                (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                 errmsg("bigint out of range")));
    }
    return result;
}

static int64
generic_checked_int64_product(int64 left, int64 right)
{
    int64 result;

    if (left < 0 || right < 0 ||
        pg_mul_s64_overflow(left, right, &result))
    {
        ereport(ERROR,
                (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                 errmsg("bigint out of range")));
    }
    return result;
}

static int64
generic_checked_int64_sum(int64 left, int64 right)
{
    int64 result;

    if (pg_add_s64_overflow(left, right, &result))
    {
        ereport(ERROR,
                (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                 errmsg("bigint out of range")));
    }
    return result;
}

static Node *create_age_generic_join_state(CustomScan *cscan);
static void begin_age_generic_join(CustomScanState *node, EState *estate,
                                   int eflags);
static TupleTableSlot *exec_age_generic_join(CustomScanState *node);
static TupleTableSlot *access_age_generic_join(ScanState *node);
static TupleTableSlot *access_age_generic_count_scan(ScanState *node);
static TupleTableSlot *access_age_generic_group_count_scan(ScanState *node);
static TupleTableSlot *access_age_generic_distinct_key_scan(ScanState *node);
static TupleTableSlot *access_age_generic_sum_property_scan(ScanState *node);
static TupleTableSlot *store_age_generic_sum_property_result(ScanState *node);
static TupleTableSlot *access_age_generic_group_sum_property_scan(
    ScanState *node);
static TupleTableSlot *store_age_generic_group_sum_property_result(
    ScanState *node, graphid key);
static TupleTableSlot *access_age_generic_exists_scan(ScanState *node);
static bool recheck_age_generic_join(ScanState *node, TupleTableSlot *slot);
static void end_age_generic_join(CustomScanState *node);
static void rescan_age_generic_join(CustomScanState *node);
static void explain_age_generic_join(CustomScanState *node, List *ancestors,
                                     ExplainState *es);
static void initialize_generic_reduction_order(AgeGenericJoinState *state,
                                               List *provider_descs,
                                               List *reduction_desc);
static void initialize_generic_ghd_bags(AgeGenericJoinState *state,
                                        List *reduction_desc);
static void initialize_generic_ghd_separators(AgeGenericJoinState *state,
                                              List *reduction_desc);
static void initialize_generic_semijoin_steps(AgeGenericJoinState *state,
                                              List *provider_descs,
                                              List *reduction_desc);
static void build_generic_provider_prefix_range_directory(
    AgeGenericJoinState *state, AgeGenericProvider *provider);
static bool generic_provider_is_lazy_physical(AgeGenericProvider *provider);
static bool load_generic_physical_prefix(AgeGenericJoinState *state,
                                         AgeGenericProvider *provider,
                                         graphid key1);
static int lower_bound_key2_rows(AgeGenericRow *rows, int low, int high,
                                 graphid key2);
static int upper_bound_key2_rows(AgeGenericRow *rows, int low, int high,
                                 graphid key2);
static void generic_physical_provider_pair_range(AgeGenericJoinState *state,
                                                 AgeGenericProvider *provider,
                                                 graphid key1, graphid key2,
                                                 int *start, int *end);
static AgeGenericRow *generic_provider_bag_row(AgeGenericProvider *provider,
                                               int local_index);
static void reduce_generic_providers(AgeGenericJoinState *state);
static int64 reduce_generic_semijoin_steps(
    AgeGenericJoinState *state, MemoryContext reduction_context);
static void reduce_generic_leaf_tail_separators(
    AgeGenericJoinState *state, MemoryContext reduction_context);
static void initialize_generic_provider_output(
    AgeGenericProvider *provider, TupleDesc tuple_desc, EState *estate);
static const char *generic_consumer_name(AgeGenericConsumerKind consumer);
static bool age_generic_property_aggregate_consumer(
    AgeGenericConsumerKind consumer);
static bool age_generic_property_minmax_consumer(
    AgeGenericConsumerKind consumer);
static bool age_generic_property_avg_consumer(
    AgeGenericConsumerKind consumer);
static bool age_generic_group_property_aggregate_consumer(
    AgeGenericConsumerKind consumer);
static void reset_age_generic_property_aggregate_value(
    AgeGenericJoinState *state);
static void reset_age_generic_property_aggregate_state(
    AgeGenericJoinState *state);
static void free_age_generic_agtype_value(agtype_value *value,
                                          bool needs_free);
static void free_age_generic_detoasted_agtype(Datum original,
                                              agtype *value);

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

static Oid
generic_provider_desc_oid(List *desc, int field)
{
    Const *constant;

    if (list_length(desc) != AGE_GENERIC_PROVIDER_DESC_COUNT)
        elog(ERROR, "invalid AGE Generic Join provider descriptor");
    constant = list_nth_node(Const, desc, field);
    if (constant->constisnull || constant->consttype != OIDOID)
        elog(ERROR, "invalid AGE Generic Join provider OID descriptor");
    return DatumGetObjectId(constant->constvalue);
}

static bool
age_generic_property_aggregate_consumer(AgeGenericConsumerKind consumer)
{
    return consumer == AGE_GENERIC_CONSUMER_SUM_PROPERTY ||
        consumer == AGE_GENERIC_CONSUMER_MIN_PROPERTY ||
        consumer == AGE_GENERIC_CONSUMER_MAX_PROPERTY ||
        consumer == AGE_GENERIC_CONSUMER_AVG_PROPERTY ||
        consumer == AGE_GENERIC_CONSUMER_GROUP_SUM_PROPERTY ||
        consumer == AGE_GENERIC_CONSUMER_GROUP_MIN_PROPERTY ||
        consumer == AGE_GENERIC_CONSUMER_GROUP_MAX_PROPERTY ||
        consumer == AGE_GENERIC_CONSUMER_GROUP_AVG_PROPERTY;
}

static bool
age_generic_property_minmax_consumer(AgeGenericConsumerKind consumer)
{
    return consumer == AGE_GENERIC_CONSUMER_MIN_PROPERTY ||
        consumer == AGE_GENERIC_CONSUMER_MAX_PROPERTY ||
        consumer == AGE_GENERIC_CONSUMER_GROUP_MIN_PROPERTY ||
        consumer == AGE_GENERIC_CONSUMER_GROUP_MAX_PROPERTY;
}

static bool
age_generic_property_avg_consumer(AgeGenericConsumerKind consumer)
{
    return consumer == AGE_GENERIC_CONSUMER_AVG_PROPERTY ||
        consumer == AGE_GENERIC_CONSUMER_GROUP_AVG_PROPERTY;
}

static bool
age_generic_group_property_aggregate_consumer(AgeGenericConsumerKind consumer)
{
    return consumer == AGE_GENERIC_CONSUMER_GROUP_SUM_PROPERTY ||
        consumer == AGE_GENERIC_CONSUMER_GROUP_MIN_PROPERTY ||
        consumer == AGE_GENERIC_CONSUMER_GROUP_MAX_PROPERTY ||
        consumer == AGE_GENERIC_CONSUMER_GROUP_AVG_PROPERTY;
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
initialize_generic_ghd_bags(AgeGenericJoinState *state, List *reduction_desc)
{
    Node *bags_node;
    List *bag_descs;
    ListCell *lc;
    int index = 0;

    bags_node = list_nth(reduction_desc, AGE_GENERIC_REDUCTION_DESC_GHD_BAGS);
    if (bags_node != NULL && !IsA(bags_node, List))
        elog(ERROR, "invalid AGE Generic Join GHD bag descriptor");
    bag_descs = (List *)bags_node;

    if (state->reduction_ghd_bag_count <= 0)
    {
        if (list_length(bag_descs) != 0)
            elog(ERROR, "invalid AGE Generic Join GHD bag count");
        return;
    }
    if (list_length(bag_descs) != state->reduction_ghd_bag_count)
        elog(ERROR, "invalid AGE Generic Join GHD bag count");

    state->reduction_ghd_bags =
        palloc0(sizeof(AgeGenericGHDBagPlan) *
                state->reduction_ghd_bag_count);
    foreach(lc, bag_descs)
    {
        List *desc = lfirst(lc);
        Node *variables_node;
        Node *providers_node;
        List *variables;
        List *providers;
        ListCell *value_cell;
        AgeGenericGHDBagPlan *bag;
        int bag_id;
        int item_index = 0;

        if (desc == NIL || !IsA(desc, List) ||
            list_length(desc) != AGE_GENERIC_GHD_BAG_DESC_COUNT)
        {
            elog(ERROR, "invalid AGE Generic Join GHD bag descriptor");
        }

        bag_id = intVal(list_nth(desc, AGE_GENERIC_GHD_BAG_DESC_ID));
        if (bag_id != index)
            elog(ERROR, "invalid AGE Generic Join GHD bag id");

        bag = &state->reduction_ghd_bags[index++];
        bag->id = bag_id;
        bag->kind = intVal(list_nth(desc, AGE_GENERIC_GHD_BAG_DESC_KIND));
        if (bag->kind < AGE_GENERIC_GHD_BAG_CYCLIC_CORE ||
            bag->kind > AGE_GENERIC_GHD_BAG_LEAF_TAIL)
        {
            elog(ERROR, "invalid AGE Generic Join GHD bag kind");
        }

        variables_node = list_nth(desc,
                                  AGE_GENERIC_GHD_BAG_DESC_VARIABLES);
        providers_node = list_nth(desc,
                                  AGE_GENERIC_GHD_BAG_DESC_PROVIDERS);
        if (variables_node == NULL || !IsA(variables_node, List) ||
            providers_node == NULL || !IsA(providers_node, List))
        {
            elog(ERROR, "invalid AGE Generic Join GHD bag descriptor");
        }
        variables = (List *)variables_node;
        providers = (List *)providers_node;
        if (list_length(variables) <= 0 || list_length(providers) <= 0)
            elog(ERROR, "invalid AGE Generic Join GHD empty bag");

        bag->variable_count = list_length(variables);
        bag->provider_count = list_length(providers);
        bag->variables = palloc(sizeof(int) * bag->variable_count);
        bag->provider_indexes = palloc(sizeof(int) * bag->provider_count);

        item_index = 0;
        foreach(value_cell, variables)
        {
            int variable = intVal(lfirst(value_cell));

            if (variable < 0 || variable >= state->variable_count)
                elog(ERROR, "invalid AGE Generic Join GHD bag variable");
            bag->variables[item_index++] = variable;
        }

        item_index = 0;
        foreach(value_cell, providers)
        {
            int provider_index = intVal(lfirst(value_cell));

            if (provider_index < 0 ||
                provider_index >= state->provider_count)
            {
                elog(ERROR, "invalid AGE Generic Join GHD bag provider");
            }
            bag->provider_indexes[item_index++] = provider_index;
        }
    }
}

static bool
generic_int_array_contains(const int *values, int count, int target)
{
    int index;

    for (index = 0; index < count; index++)
    {
        if (values[index] == target)
            return true;
    }
    return false;
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
        Node *separator_variables_node;
        List *separator_variables;
        ListCell *separator_variable_cell;
        AgeGenericGHDSeparatorPlan *plan;
        int separator_id;
        int item_index = 0;

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
        plan->parent_bag_id = intVal(list_nth(
            desc, AGE_GENERIC_GHD_SEPARATOR_DESC_PARENT_BAG));
        plan->child_bag_id = intVal(list_nth(
            desc, AGE_GENERIC_GHD_SEPARATOR_DESC_CHILD_BAG));
        plan->separator_variable = intVal(list_nth(
            desc, AGE_GENERIC_GHD_SEPARATOR_DESC_SEPARATOR_VARIABLE));
        plan->tail_variable = intVal(list_nth(
            desc, AGE_GENERIC_GHD_SEPARATOR_DESC_TAIL_VARIABLE));
        plan->provider_index = intVal(list_nth(
            desc, AGE_GENERIC_GHD_SEPARATOR_DESC_PROVIDER_INDEX));
        plan->separator_is_key1 = intVal(list_nth(
            desc, AGE_GENERIC_GHD_SEPARATOR_DESC_SEPARATOR_IS_KEY1)) != 0;
        plan->kind = (AgeGenericGHDSeparatorKind)intVal(list_nth(
            desc, AGE_GENERIC_GHD_SEPARATOR_DESC_KIND));
        separator_variables_node = list_nth(
            desc, AGE_GENERIC_GHD_SEPARATOR_DESC_SEPARATOR_VARIABLES);
        if (separator_variables_node == NULL ||
            !IsA(separator_variables_node, List))
        {
            elog(ERROR, "invalid AGE Generic Join GHD separator descriptor");
        }
        separator_variables = (List *)separator_variables_node;
        plan->separator_variable_count = list_length(separator_variables);
        if (plan->separator_variable_count <= 0)
            elog(ERROR, "invalid AGE Generic Join GHD separator descriptor");
        plan->separator_variables =
            palloc(sizeof(int) * plan->separator_variable_count);
        foreach(separator_variable_cell, separator_variables)
        {
            int variable = intVal(lfirst(separator_variable_cell));

            if (variable < 0 || variable >= state->variable_count)
                elog(ERROR,
                     "invalid AGE Generic Join GHD separator variable");
            plan->separator_variables[item_index++] = variable;
        }

        if (plan->parent_bag_id < 0 ||
            plan->parent_bag_id >= state->reduction_ghd_bag_count ||
            plan->child_bag_id < 0 ||
            plan->child_bag_id >= state->reduction_ghd_bag_count ||
            plan->parent_bag_id == plan->child_bag_id ||
            plan->kind < AGE_GENERIC_GHD_SEPARATOR_LEAF_TAIL ||
            plan->kind > AGE_GENERIC_GHD_SEPARATOR_PAIR ||
            plan->provider_index < 0 ||
            plan->provider_index >= state->provider_count ||
            plan->separator_variable < 0 ||
            plan->separator_variable >= state->variable_count ||
            plan->tail_variable < 0 ||
            plan->tail_variable >= state->variable_count ||
            plan->separator_variable == plan->tail_variable)
        {
            elog(ERROR, "invalid AGE Generic Join GHD separator descriptor");
        }
        if ((plan->kind == AGE_GENERIC_GHD_SEPARATOR_LEAF_TAIL &&
             (plan->separator_variable_count != 1 ||
              plan->separator_variables[0] != plan->separator_variable)) ||
            (plan->kind == AGE_GENERIC_GHD_SEPARATOR_PAIR &&
             plan->separator_variable_count != 2))
        {
            elog(ERROR, "invalid AGE Generic Join GHD separator descriptor");
        }

        if (state->reduction_ghd_bags != NULL)
        {
            AgeGenericGHDBagPlan *parent_bag =
                &state->reduction_ghd_bags[plan->parent_bag_id];
            AgeGenericGHDBagPlan *child_bag =
                &state->reduction_ghd_bags[plan->child_bag_id];
            int separator_index;

            for (separator_index = 0;
                 separator_index < plan->separator_variable_count;
                 separator_index++)
            {
                int separator_variable =
                    plan->separator_variables[separator_index];

                if (!generic_int_array_contains(
                        parent_bag->variables, parent_bag->variable_count,
                        separator_variable) ||
                    !generic_int_array_contains(
                        child_bag->variables, child_bag->variable_count,
                        separator_variable))
                {
                    elog(ERROR,
                         "invalid AGE Generic Join GHD separator bag linkage");
                }
            }

            if (plan->kind == AGE_GENERIC_GHD_SEPARATOR_LEAF_TAIL &&
                (!generic_int_array_contains(
                     child_bag->variables, child_bag->variable_count,
                     plan->tail_variable) ||
                 !generic_int_array_contains(
                     child_bag->provider_indexes, child_bag->provider_count,
                     plan->provider_index)))
            {
                elog(ERROR,
                     "invalid AGE Generic Join GHD separator bag linkage");
            }
        }
    }
}

static void
initialize_generic_semijoin_steps(AgeGenericJoinState *state,
                                  List *provider_descs,
                                  List *reduction_desc)
{
    Node *steps_node;
    List *step_descs;
    ListCell *lc;
    int index = 0;

    steps_node = list_nth(
        reduction_desc, AGE_GENERIC_REDUCTION_DESC_SEMIJOIN_STEPS);
    if (steps_node != NULL && !IsA(steps_node, List))
        elog(ERROR, "invalid AGE Generic Join semijoin step descriptor");

    step_descs = (List *)steps_node;
    state->reduction_semijoin_step_count = list_length(step_descs);
    if (state->reduction_semijoin_step_count <= 0)
        return;

    state->reduction_semijoin_steps =
        palloc0(sizeof(AgeGenericSemijoinStepPlan) *
                state->reduction_semijoin_step_count);
    foreach(lc, step_descs)
    {
        List *desc = lfirst(lc);
        List *provider_desc;
        AgeGenericSemijoinStepPlan *step;
        int provider_kind;
        int provider_var1;
        int provider_var2;
        int step_id;

        if (desc == NIL || !IsA(desc, List) ||
            list_length(desc) != AGE_GENERIC_SEMIJOIN_STEP_DESC_COUNT)
        {
            elog(ERROR, "invalid AGE Generic Join semijoin step descriptor");
        }

        step_id = intVal(list_nth(desc, AGE_GENERIC_SEMIJOIN_STEP_DESC_ID));
        if (step_id != index)
            elog(ERROR, "invalid AGE Generic Join semijoin step id");

        step = &state->reduction_semijoin_steps[index++];
        step->phase = (AgeGenericSemijoinStepPhase)intVal(list_nth(
            desc, AGE_GENERIC_SEMIJOIN_STEP_DESC_PHASE));
        step->provider_index = intVal(list_nth(
            desc, AGE_GENERIC_SEMIJOIN_STEP_DESC_PROVIDER_INDEX));
        step->from_variable = intVal(list_nth(
            desc, AGE_GENERIC_SEMIJOIN_STEP_DESC_FROM_VARIABLE));
        step->to_variable = intVal(list_nth(
            desc, AGE_GENERIC_SEMIJOIN_STEP_DESC_TO_VARIABLE));
        step->key_variable = intVal(list_nth(
            desc, AGE_GENERIC_SEMIJOIN_STEP_DESC_KEY_VARIABLE));
        step->key_is_key1 = intVal(list_nth(
            desc, AGE_GENERIC_SEMIJOIN_STEP_DESC_KEY_IS_KEY1)) != 0;

        if (step->phase < AGE_GENERIC_SEMIJOIN_STEP_BOTTOM_UP ||
            step->phase > AGE_GENERIC_SEMIJOIN_STEP_TOP_DOWN ||
            step->provider_index < 0 ||
            step->provider_index >= state->provider_count ||
            step->from_variable < 0 ||
            step->from_variable >= state->variable_count ||
            step->to_variable < 0 ||
            step->to_variable >= state->variable_count ||
            step->key_variable < 0 ||
            step->key_variable >= state->variable_count ||
            step->from_variable == step->to_variable)
        {
            elog(ERROR, "invalid AGE Generic Join semijoin step descriptor");
        }

        provider_desc = list_nth_node(List, provider_descs,
                                      step->provider_index);
        provider_kind = generic_provider_desc_int(
            provider_desc, AGE_GENERIC_PROVIDER_DESC_KIND);
        provider_var1 = generic_provider_desc_int(
            provider_desc, AGE_GENERIC_PROVIDER_DESC_VAR1);
        provider_var2 = generic_provider_desc_int(
            provider_desc, AGE_GENERIC_PROVIDER_DESC_VAR2);
        if (provider_kind != AGE_GENERIC_PROVIDER_EDGE ||
            (provider_var1 != step->from_variable &&
             provider_var2 != step->from_variable) ||
            (provider_var1 != step->to_variable &&
             provider_var2 != step->to_variable) ||
            (provider_var1 != step->key_variable &&
             provider_var2 != step->key_variable) ||
            (step->key_is_key1 && provider_var1 != step->key_variable) ||
            (!step->key_is_key1 && provider_var2 != step->key_variable))
        {
            elog(ERROR, "invalid AGE Generic Join semijoin step provider");
        }

        if (step->phase == AGE_GENERIC_SEMIJOIN_STEP_BOTTOM_UP)
            state->reduction_semijoin_bottom_up_step_count++;
        else
            state->reduction_semijoin_top_down_step_count++;
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
    initialize_generic_ghd_bags(state, reduction_desc);
    state->reduction_ghd_separator_count = intVal(list_nth(
        reduction_desc, AGE_GENERIC_REDUCTION_DESC_GHD_SEPARATOR_COUNT));
    initialize_generic_ghd_separators(state, reduction_desc);
    initialize_generic_semijoin_steps(state, provider_descs, reduction_desc);

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
            AGE_GENERIC_REDUCTION_ORDER_LEAF_PEEL ||
            state->reduction_semijoin_step_count !=
            state->reduction_order_edge_count * 2 ||
            state->reduction_semijoin_bottom_up_step_count !=
            state->reduction_order_edge_count ||
            state->reduction_semijoin_top_down_step_count !=
            state->reduction_order_edge_count)
        {
            elog(ERROR,
                 "invalid AGE Generic Join alpha-acyclic reduction order");
        }
        validate_generic_leaf_peel_order(state, provider_descs);
    }
    else
    {
        if (state->reduction_order_kind != AGE_GENERIC_REDUCTION_ORDER_NONE ||
            state->reduction_order_edge_count != 0)
        {
            elog(ERROR, "invalid AGE Generic Join cyclic reduction order");
        }
        if (state->reduction_semijoin_step_count != 0)
        {
            if (state->reduction_shape !=
                AGE_GENERIC_REDUCTION_CYCLIC_WITH_TAIL ||
                state->reduction_semijoin_step_count !=
                state->reduction_tail_separators * 2 ||
                state->reduction_semijoin_bottom_up_step_count !=
                state->reduction_tail_separators ||
                state->reduction_semijoin_top_down_step_count !=
                state->reduction_tail_separators)
            {
                elog(ERROR,
                     "invalid AGE Generic Join cyclic semijoin steps");
            }
        }
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
    Const *output_type_const;
    Const *row_goal_const;
    List *consumer_desc;
    int consumer_kind;
    int64 row_goal;
    int row_goal_source;
    int distinct_variable;
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
    consumer_kind = intVal(list_nth(
        cscan->custom_private, AGE_GENERIC_JOIN_PRIVATE_CONSUMER));
    output_type_const = list_nth_node(
        Const, cscan->custom_private,
        AGE_GENERIC_JOIN_PRIVATE_OUTPUT_TYPE);
    row_goal_const = list_nth_node(
        Const, cscan->custom_private,
        AGE_GENERIC_JOIN_PRIVATE_ROW_GOAL);
    row_goal_source = intVal(list_nth(
        cscan->custom_private, AGE_GENERIC_JOIN_PRIVATE_ROW_GOAL_SOURCE));
    distinct_variable = intVal(list_nth(
        cscan->custom_private, AGE_GENERIC_JOIN_PRIVATE_DISTINCT_VARIABLE));
    consumer_desc = list_nth(
        cscan->custom_private, AGE_GENERIC_JOIN_PRIVATE_CONSUMER_DESC);
    if (state->variable_count < 2 ||
        list_length(variable_rtis) != state->variable_count ||
        provider_descs == NIL ||
        list_length(uniqueness_group_descs) != list_length(provider_descs) ||
        list_length(reduction_desc) != AGE_GENERIC_REDUCTION_DESC_COUNT ||
        (consumer_kind != AGE_GENERIC_CONSUMER_ROWS &&
         consumer_kind != AGE_GENERIC_CONSUMER_COUNT &&
         consumer_kind != AGE_GENERIC_CONSUMER_COUNT_DISTINCT_KEY &&
         consumer_kind != AGE_GENERIC_CONSUMER_DISTINCT_KEY &&
         consumer_kind != AGE_GENERIC_CONSUMER_GROUP_COUNT &&
         consumer_kind != AGE_GENERIC_CONSUMER_GROUP_COUNT_DISTINCT_KEY &&
         consumer_kind != AGE_GENERIC_CONSUMER_SUM_PROPERTY &&
         consumer_kind != AGE_GENERIC_CONSUMER_MIN_PROPERTY &&
         consumer_kind != AGE_GENERIC_CONSUMER_MAX_PROPERTY &&
         consumer_kind != AGE_GENERIC_CONSUMER_AVG_PROPERTY &&
         consumer_kind != AGE_GENERIC_CONSUMER_GROUP_SUM_PROPERTY &&
         consumer_kind != AGE_GENERIC_CONSUMER_GROUP_MIN_PROPERTY &&
         consumer_kind != AGE_GENERIC_CONSUMER_GROUP_MAX_PROPERTY &&
         consumer_kind != AGE_GENERIC_CONSUMER_GROUP_AVG_PROPERTY &&
         consumer_kind != AGE_GENERIC_CONSUMER_EXISTS &&
         consumer_kind != AGE_GENERIC_CONSUMER_LIMIT) ||
        output_type_const->constisnull ||
        output_type_const->consttype != OIDOID ||
        row_goal_const->constisnull ||
        row_goal_const->consttype != INT8OID)
    {
        elog(ERROR, "invalid AGE Generic Join arity");
    }
    row_goal = DatumGetInt64(row_goal_const->constvalue);
    state->consumer_kind = (AgeGenericConsumerKind)consumer_kind;
    state->row_goal_source = (AgeGenericRowGoalSource)row_goal_source;
    state->consumer_output_type =
        DatumGetObjectId(output_type_const->constvalue);
    state->row_goal = row_goal;
    state->distinct_variable = distinct_variable;
    state->property_aggregate_provider_index = -1;
    if (state->consumer_kind == AGE_GENERIC_CONSUMER_ROWS ||
        state->consumer_kind == AGE_GENERIC_CONSUMER_LIMIT)
    {
        if (OidIsValid(state->consumer_output_type))
            elog(ERROR, "invalid AGE Generic Join row consumer descriptor");
    }
    else if ((state->consumer_kind == AGE_GENERIC_CONSUMER_COUNT ||
              state->consumer_kind ==
              AGE_GENERIC_CONSUMER_COUNT_DISTINCT_KEY ||
              state->consumer_kind == AGE_GENERIC_CONSUMER_GROUP_COUNT ||
              state->consumer_kind ==
              AGE_GENERIC_CONSUMER_GROUP_COUNT_DISTINCT_KEY) &&
             state->consumer_output_type != INT8OID &&
             state->consumer_output_type != AGTYPEOID)
    {
        elog(ERROR, "invalid AGE Generic Join count output type %u",
             state->consumer_output_type);
    }
    else if (state->consumer_kind == AGE_GENERIC_CONSUMER_DISTINCT_KEY &&
             state->consumer_output_type != GRAPHIDOID &&
             state->consumer_output_type != INT8OID &&
             state->consumer_output_type != AGTYPEOID)
    {
        elog(ERROR, "invalid AGE Generic Join distinct-key output type %u",
             state->consumer_output_type);
    }
    else if (age_generic_property_aggregate_consumer(
                 state->consumer_kind))
    {
        int provider_index;
        AttrNumber properties_attno;
        Const *key_const;
        bool avg_consumer =
            age_generic_property_avg_consumer(state->consumer_kind);

        if ((!avg_consumer && state->consumer_output_type != AGTYPEOID) ||
            (avg_consumer &&
             state->consumer_output_type != AGTYPEOID &&
             state->consumer_output_type != FLOAT8OID) ||
            list_length(consumer_desc) != AGE_GENERIC_SUM_PROPERTY_COUNT)
        {
            elog(ERROR,
                 "invalid AGE Generic Join property aggregate descriptor");
        }
        provider_index = intVal(list_nth(
            consumer_desc, AGE_GENERIC_SUM_PROPERTY_PROVIDER_INDEX));
        key_const = list_nth_node(
            Const, consumer_desc, AGE_GENERIC_SUM_PROPERTY_KEY);
        properties_attno = (AttrNumber)intVal(list_nth(
            consumer_desc,
            AGE_GENERIC_SUM_PROPERTY_PROPERTIES_ATTNO));
        if (provider_index < 0 ||
            provider_index >= list_length(provider_descs) ||
            key_const->constisnull ||
            key_const->consttype != AGTYPEOID ||
            properties_attno <= 0)
        {
            elog(ERROR,
                 "invalid AGE Generic Join property aggregate key "
                 "descriptor");
        }

        state->property_aggregate_provider_index = provider_index;
        state->property_aggregate_attno = properties_attno;
        state->property_aggregate_key =
            datumCopy(key_const->constvalue, key_const->constbyval,
                      key_const->constlen);
        state->property_aggregate_key_isnull = false;
    }
    else if (state->consumer_kind == AGE_GENERIC_CONSUMER_EXISTS &&
             state->consumer_output_type != AGTYPEOID)
    {
        elog(ERROR, "invalid AGE Generic Join exists output type %u",
             state->consumer_output_type);
    }
    if (row_goal < 0 ||
        (state->consumer_kind != AGE_GENERIC_CONSUMER_LIMIT &&
         state->consumer_kind != AGE_GENERIC_CONSUMER_EXISTS &&
         row_goal != 0) ||
        (state->consumer_kind == AGE_GENERIC_CONSUMER_LIMIT &&
         (row_goal <= 0 || row_goal_source != AGE_GENERIC_ROW_GOAL_LIMIT)) ||
        (state->consumer_kind == AGE_GENERIC_CONSUMER_EXISTS &&
         (row_goal <= 0 || row_goal_source != AGE_GENERIC_ROW_GOAL_EXISTS)) ||
        (row_goal == 0 && row_goal_source != AGE_GENERIC_ROW_GOAL_NONE) ||
        (row_goal > 0 &&
         row_goal_source != AGE_GENERIC_ROW_GOAL_LIMIT &&
         row_goal_source != AGE_GENERIC_ROW_GOAL_EXISTS))
    {
        elog(ERROR, "invalid AGE Generic Join row goal " INT64_FORMAT,
             row_goal);
    }
    if (((state->consumer_kind ==
          AGE_GENERIC_CONSUMER_COUNT_DISTINCT_KEY ||
          state->consumer_kind == AGE_GENERIC_CONSUMER_DISTINCT_KEY ||
          state->consumer_kind == AGE_GENERIC_CONSUMER_GROUP_COUNT ||
          state->consumer_kind ==
          AGE_GENERIC_CONSUMER_GROUP_COUNT_DISTINCT_KEY ||
          age_generic_group_property_aggregate_consumer(
              state->consumer_kind)) &&
         (distinct_variable < 0 ||
          distinct_variable >= state->variable_count)) ||
        (state->consumer_kind !=
         AGE_GENERIC_CONSUMER_COUNT_DISTINCT_KEY &&
         state->consumer_kind != AGE_GENERIC_CONSUMER_DISTINCT_KEY &&
         state->consumer_kind != AGE_GENERIC_CONSUMER_GROUP_COUNT &&
         state->consumer_kind !=
         AGE_GENERIC_CONSUMER_GROUP_COUNT_DISTINCT_KEY &&
         !age_generic_group_property_aggregate_consumer(
             state->consumer_kind) &&
         distinct_variable != -1))
    {
        elog(ERROR, "invalid AGE Generic Join distinct variable %d",
             distinct_variable);
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
        int old_capacity = provider->row_capacity;
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
        add_generic_counter(&state->provider_row_bytes_allocated,
                            (int64)(new_capacity - old_capacity) *
                            sizeof(AgeGenericRow));
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
        MinimalTuple tuple = ExecCopySlotMinimalTuple(slot);

        provider->rows[provider->row_count].tuple = tuple;
        add_generic_counter(&state->provider_tuple_bytes_copied,
                            tuple->t_len);
        increment_generic_counter(&state->tuples_materialized);
    }
    provider->row_count++;
    MemoryContextSwitchTo(oldcontext);

    increment_generic_counter(&state->provider_rows_read);
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

        if (generic_provider_is_lazy_physical(provider))
            continue;

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
lower_bound_key1_rows(AgeGenericRow *rows, int row_count, graphid key)
{
    int low = 0;
    int high = row_count;

    while (low < high)
    {
        int middle = low + (high - low) / 2;

        if (rows[middle].key1 < key)
            low = middle + 1;
        else
            high = middle;
    }
    return low;
}

static int
upper_bound_key1_rows(AgeGenericRow *rows, int row_count, graphid key)
{
    int low = 0;
    int high = row_count;

    while (low < high)
    {
        int middle = low + (high - low) / 2;

        if (rows[middle].key1 <= key)
            low = middle + 1;
        else
            high = middle;
    }
    return low;
}

static int
lower_bound_key1(AgeGenericProvider *provider, graphid key)
{
    return lower_bound_key1_rows(provider->rows, provider->row_count, key);
}

static int
upper_bound_key1(AgeGenericProvider *provider, graphid key)
{
    return upper_bound_key1_rows(provider->rows, provider->row_count, key);
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
        increment_generic_counter(&state->prefix_range_cache_hits);
        return;
    }

    if (generic_provider_prefix_cache_lookup(provider, key, start, end))
    {
        increment_generic_counter(&state->prefix_range_cache_hits);
        increment_generic_counter(&state->lazy_prefix_range_reuses);
        return;
    }

    increment_generic_counter(&state->prefix_range_cache_misses);
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

static bool
generic_provider_is_lazy_physical(AgeGenericProvider *provider)
{
    return provider->physical_kind ==
        AGE_GENERIC_PROVIDER_PHYSICAL_ADJACENCY_EDGE;
}

static bool
generic_provider_is_reduction_materialized(AgeGenericProvider *provider)
{
    return !generic_provider_is_lazy_physical(provider);
}

static void
ensure_generic_physical_prefix_capacity(AgeGenericJoinState *state,
                                        AgeGenericProvider *provider,
                                        int required)
{
    MemoryContext oldcontext;
    int new_capacity;

    if (required <= provider->prefix_row_capacity)
        return;
    new_capacity = provider->prefix_row_capacity == 0 ? 16 :
        provider->prefix_row_capacity * 2;
    while (new_capacity < required)
    {
        if (new_capacity > PG_INT32_MAX / 2)
        {
            ereport(ERROR,
                    (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                     errmsg("AGE Generic Join physical prefix is too large")));
        }
        new_capacity *= 2;
    }
    if ((Size)new_capacity > MaxAllocSize / sizeof(AgeGenericRow))
    {
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("AGE Generic Join physical prefix is too large")));
    }

    oldcontext = MemoryContextSwitchTo(state->data_context);
    if (provider->prefix_rows == NULL)
        provider->prefix_rows = palloc(sizeof(AgeGenericRow) * new_capacity);
    else
        provider->prefix_rows = repalloc(
            provider->prefix_rows, sizeof(AgeGenericRow) * new_capacity);
    MemoryContextSwitchTo(oldcontext);

    add_generic_counter(
        &state->lazy_physical_row_bytes_allocated,
        (int64)(new_capacity - provider->prefix_row_capacity) *
        sizeof(AgeGenericRow));
    provider->prefix_row_capacity = new_capacity;
}

static bool
load_generic_physical_prefix(AgeGenericJoinState *state,
                             AgeGenericProvider *provider, graphid key1)
{
    AgeAdjacencyPayload payload;
    bool active;

    if (!generic_provider_is_lazy_physical(provider))
        elog(ERROR, "AGE Generic Join provider is not physical");
    if (provider->payload_scan == NULL)
        elog(ERROR, "AGE Generic Join physical provider has no scan");
    if (provider->prefix_valid && provider->prefix_key == key1)
        return provider->prefix_row_count > 0;

    provider->prefix_key = key1;
    provider->prefix_row_count = 0;
    provider->prefix_valid = false;
    invalidate_generic_provider_prefix_ranges(provider);

    active = age_adjacency_visible_payload_scan_begin_key(
        provider->payload_scan, key1);
    while (active &&
           age_adjacency_visible_payload_scan_next(provider->payload_scan,
                                                   &payload))
    {
        AgeGenericRow *row;

        ensure_generic_physical_prefix_capacity(
            state, provider, provider->prefix_row_count + 1);
        row = &provider->prefix_rows[provider->prefix_row_count++];
        row->key1 = key1;
        row->key2 = payload.next_vertex_id;
        row->edge_id = payload.edge_id;
        row->edge_id_valid = true;
        row->tuple = NULL;
        increment_generic_counter(&state->provider_rows_read);
        increment_generic_counter(&state->lazy_physical_rows_read);
    }

    if (provider->prefix_row_count > 1)
    {
        qsort(provider->prefix_rows, provider->prefix_row_count,
              sizeof(AgeGenericRow), compare_generic_rows);
    }
    provider->prefix_valid = true;
    increment_generic_counter(&state->lazy_physical_prefix_loads);
    state->peak_memory = Max(state->peak_memory,
                             MemoryContextMemAllocated(state->data_context,
                                                       true));
    return provider->prefix_row_count > 0;
}

static void
generic_physical_provider_pair_range(AgeGenericJoinState *state,
                                     AgeGenericProvider *provider,
                                     graphid key1, graphid key2,
                                     int *start, int *end)
{
    if (!load_generic_physical_prefix(state, provider, key1))
    {
        *start = 0;
        *end = 0;
        return;
    }
    *start = lower_bound_key2_rows(provider->prefix_rows, 0,
                                   provider->prefix_row_count, key2);
    *end = upper_bound_key2_rows(provider->prefix_rows, *start,
                                 provider->prefix_row_count, key2);
    increment_generic_counter(&state->trie_pair_range_opens);
}

static AgeGenericRow *
generic_provider_bag_row(AgeGenericProvider *provider, int local_index)
{
    if (local_index < 0 || local_index >= provider->bag_count)
        elog(ERROR, "invalid AGE Generic Join bag row index");
    if (generic_provider_is_lazy_physical(provider))
        return &provider->prefix_rows[provider->bag_start + local_index];
    return &provider->rows[provider->bag_start + local_index];
}

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
        increment_generic_counter(&state->child_range_cache_hits);
        return;
    }

    increment_generic_counter(&state->child_range_cache_misses);
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
lower_bound_key2_rows(AgeGenericRow *rows, int low, int high, graphid key2)
{
    while (low < high)
    {
        int middle = low + (high - low) / 2;

        if (rows[middle].key2 < key2)
            low = middle + 1;
        else
            high = middle;
    }
    return low;
}

static int
upper_bound_key2_rows(AgeGenericRow *rows, int low, int high, graphid key2)
{
    while (low < high)
    {
        int middle = low + (high - low) / 2;

        if (rows[middle].key2 <= key2)
            low = middle + 1;
        else
            high = middle;
    }
    return low;
}

static int
lower_bound_key2(AgeGenericProvider *provider, int low, int high,
                 graphid key2)
{
    return lower_bound_key2_rows(provider->rows, low, high, key2);
}

static int
upper_bound_key2(AgeGenericProvider *provider, int low, int high,
                 graphid key2)
{
    return upper_bound_key2_rows(provider->rows, low, high, key2);
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
    AgeGenericRow *row = &view->rows[row_index];

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
    AgeGenericRow *rows = provider->rows;

    if (generic_provider_is_lazy_physical(provider))
    {
        if (provider->var2 != variable)
            return false;
        if (!load_generic_physical_prefix(
                state, provider, state->bindings[provider->var1]))
        {
            return false;
        }
        start = 0;
        end = provider->prefix_row_count;
        rows = provider->prefix_rows;
        use_key2 = true;
    }
    else if (provider->row_count == 0)
    {
        return false;
    }
    else if (provider->kind == AGE_GENERIC_PROVIDER_EDGE &&
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
    view->rows = rows;
    view->variable = variable;
    view->start = start;
    view->end = end;
    view->use_key2 = use_key2;
    increment_generic_counter(&state->runtime_domain_builds);
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
    add_generic_counter(&state->runtime_domain_rows_scanned,
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
    add_generic_counter(&state->runtime_domain_rows_scanned,
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

static int
compare_generic_graphid_pairs(const void *left, const void *right)
{
    const AgeGenericGraphidPair *a = left;
    const AgeGenericGraphidPair *b = right;

    if (a->key1 < b->key1)
        return -1;
    if (a->key1 > b->key1)
        return 1;
    if (a->key2 < b->key2)
        return -1;
    if (a->key2 > b->key2)
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
    increment_generic_counter(&state->reduction_domain_builds);
    add_generic_counter(&state->reduction_domain_rows_scanned,
                        provider->row_count);
    if (use_key2 && provider->row_count > 1)
    {
        qsort(domain, provider->row_count, sizeof(graphid),
              compare_generic_graphids);
        increment_generic_counter(&state->reduction_domain_sorts);
        add_generic_counter(&state->reduction_domain_sort_rows,
                            provider->row_count);
    }
    for (row_index = 0; row_index < provider->row_count; row_index++)
    {
        if (count == 0 || domain[row_index] != domain[count - 1])
            domain[count++] = domain[row_index];
    }
    *domain_count = count;
    add_generic_counter(&state->reduction_domain_keys_produced, count);
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

            if (!generic_provider_is_reduction_materialized(provider))
                continue;
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

    if (!generic_provider_is_reduction_materialized(provider))
        return 0;
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

    if (!generic_provider_is_reduction_materialized(provider))
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
    if (!generic_provider_is_reduction_materialized(provider))
        return 0;
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
        if (!generic_provider_is_reduction_materialized(
                &state->providers[provider_index]))
        {
            continue;
        }
        rows_removed += filter_generic_provider_for_reduction(
            state, provider_index, domains);
        if (state->reduction_semijoin_step_count > 0)
        {
            increment_generic_counter(&state->semijoin_steps_applied);
            if (reverse)
                increment_generic_counter(
                    &state->semijoin_bottom_up_steps_applied);
            else
                increment_generic_counter(
                    &state->semijoin_top_down_steps_applied);
        }
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
        if (!generic_provider_is_reduction_materialized(provider))
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

        if (!generic_provider_is_reduction_materialized(provider))
            continue;
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
            provider_index &&
            state->reduction_ghd_separators[separator_index].kind ==
            AGE_GENERIC_GHD_SEPARATOR_LEAF_TAIL)
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
    if (!generic_provider_is_reduction_materialized(provider))
        return false;
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
        int separator_index;

        for (separator_index = 0;
             separator_index < state->reduction_ghd_separator_count;
             separator_index++)
        {
            if (state->reduction_ghd_separators[separator_index].kind ==
                AGE_GENERIC_GHD_SEPARATOR_LEAF_TAIL)
            {
                return true;
            }
        }
        return false;
    }

    for (provider_index = 0;
         provider_index < state->provider_count;
         provider_index++)
    {
        int separator_variable;
        int tail_variable;
        bool separator_is_key1;

        if (!generic_provider_is_reduction_materialized(
                &state->providers[provider_index]))
        {
            continue;
        }
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

            if (!generic_provider_is_reduction_materialized(provider))
                continue;
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

static bool
generic_provider_row_key_for_variable(AgeGenericProvider *provider,
                                      AgeGenericRow *row, int variable,
                                      graphid *key)
{
    if (provider->var1 == variable)
    {
        *key = row->key1;
        return true;
    }
    if (provider->kind == AGE_GENERIC_PROVIDER_EDGE &&
        provider->var2 == variable)
    {
        *key = row->key2;
        return true;
    }
    return false;
}

static graphid *
build_generic_semijoin_step_domain(AgeGenericJoinState *state,
                                   AgeGenericSemijoinStepPlan *step,
                                   const AgeGenericDomain *domains,
                                   MemoryContext context,
                                   int *domain_count)
{
    AgeGenericProvider *provider;
    graphid *domain;
    int row_index;
    int output_count = 0;
    int count = 0;
    MemoryContext oldcontext;

    *domain_count = 0;
    if (step == NULL ||
        step->provider_index < 0 ||
        step->provider_index >= state->provider_count ||
        domains[step->from_variable].count <= 0)
    {
        return NULL;
    }

    provider = &state->providers[step->provider_index];
    if (!generic_provider_is_reduction_materialized(provider) ||
        provider->row_count <= 0)
        return NULL;

    oldcontext = MemoryContextSwitchTo(context);
    domain = palloc(sizeof(graphid) * provider->row_count);
    MemoryContextSwitchTo(oldcontext);

    for (row_index = 0; row_index < provider->row_count; row_index++)
    {
        AgeGenericRow *row = &provider->rows[row_index];
        graphid from_key;
        graphid key;

        if (!generic_provider_row_key_for_variable(
                provider, row, step->from_variable, &from_key) ||
            !generic_provider_row_key_for_variable(
                provider, row, step->key_variable, &key))
        {
            elog(ERROR, "invalid AGE Generic Join semijoin step row");
        }
        if (!generic_domain_contains(&domains[step->from_variable],
                                     from_key))
        {
            continue;
        }
        domain[count++] = key;
    }

    if (count <= 0)
        return NULL;
    if (count > 1)
        qsort(domain, count, sizeof(graphid), compare_generic_graphids);
    for (row_index = 0; row_index < count; row_index++)
    {
        if (output_count == 0 ||
            domain[row_index] != domain[output_count - 1])
        {
            domain[output_count++] = domain[row_index];
        }
    }
    *domain_count = output_count;
    return domain;
}

static int64
filter_generic_providers_for_semijoin_step(
    AgeGenericJoinState *state, AgeGenericSemijoinStepPlan *step,
    const AgeGenericDomain *domains, MemoryContext context)
{
    AgeGenericDomain step_domain;
    graphid *domain_keys;
    int domain_count;
    int provider_index;
    int64 rows_removed = 0;

    domain_keys = build_generic_semijoin_step_domain(
        state, step, domains, context, &domain_count);
    if (domain_count <= 0)
    {
        state->exhausted = true;
        return 0;
    }

    step_domain.keys = domain_keys;
    step_domain.count = domain_count;
    for (provider_index = 0;
         provider_index < state->provider_count;
         provider_index++)
    {
        AgeGenericProvider *provider = &state->providers[provider_index];

        if (!generic_provider_is_reduction_materialized(provider))
            continue;
        if (!generic_provider_has_variable(provider, step->to_variable))
            continue;
        rows_removed += filter_generic_provider_on_variable(
            provider, step->to_variable, &step_domain);
        if (provider->row_count <= 0)
        {
            state->exhausted = true;
            break;
        }
    }
    return rows_removed;
}

static bool
generic_cyclic_semijoin_steps_applicable(AgeGenericJoinState *state)
{
    return state->reduction_shape == AGE_GENERIC_REDUCTION_CYCLIC_WITH_TAIL &&
        state->reduction_order_kind == AGE_GENERIC_REDUCTION_ORDER_NONE &&
        state->reduction_order_edge_count == 0 &&
        state->reduction_semijoin_steps != NULL &&
        state->reduction_semijoin_step_count ==
        state->reduction_tail_separators * 2 &&
        state->reduction_semijoin_bottom_up_step_count ==
        state->reduction_tail_separators &&
        state->reduction_semijoin_top_down_step_count ==
        state->reduction_tail_separators;
}

static int64
reduce_generic_semijoin_steps(AgeGenericJoinState *state,
                              MemoryContext reduction_context)
{
    MemoryContext step_context;
    int step_index;
    int64 total_rows_removed = 0;
    bool bottom_up_pass = false;
    bool top_down_pass = false;

    if (!generic_cyclic_semijoin_steps_applicable(state))
        return 0;

    step_context = AllocSetContextCreate(
        reduction_context,
        "AGE Generic Join semijoin step domains",
        ALLOCSET_SMALL_SIZES);
    for (step_index = 0;
         step_index < state->reduction_semijoin_step_count;
         step_index++)
    {
        AgeGenericSemijoinStepPlan *step =
            &state->reduction_semijoin_steps[step_index];
        AgeGenericProvider *provider;
        AgeGenericDomain *domains;
        int64 rows_removed;

        if (step->provider_index < 0 ||
            step->provider_index >= state->provider_count)
        {
            elog(ERROR, "invalid AGE Generic Join semijoin step provider");
        }
        provider = &state->providers[step->provider_index];
        if (!generic_provider_is_reduction_materialized(provider))
            continue;

        MemoryContextReset(step_context);
        domains = build_generic_domains_in_context(state, step_context);
        if (domains == NULL)
        {
            state->exhausted = true;
            break;
        }

        rows_removed = filter_generic_providers_for_semijoin_step(
            state, step, domains, step_context);
        increment_generic_counter(&state->semijoin_steps_applied);
        if (step->phase == AGE_GENERIC_SEMIJOIN_STEP_BOTTOM_UP)
        {
            bottom_up_pass = true;
            increment_generic_counter(
                &state->semijoin_bottom_up_steps_applied);
            add_generic_counter(&state->semijoin_bottom_up_rows_removed,
                                rows_removed);
        }
        else
        {
            top_down_pass = true;
            increment_generic_counter(
                &state->semijoin_top_down_steps_applied);
            add_generic_counter(&state->semijoin_top_down_rows_removed,
                                rows_removed);
        }
        add_generic_counter(&total_rows_removed, rows_removed);
        if (state->exhausted)
            break;
    }

    if (bottom_up_pass)
        increment_generic_counter(&state->semijoin_bottom_up_passes);
    if (top_down_pass)
        increment_generic_counter(&state->semijoin_top_down_passes);
    MemoryContextDelete(step_context);
    return total_rows_removed;
}

static int
generic_ghd_path_bag_internal_variable(AgeGenericGHDBagPlan *bag,
                                       int separator_var1,
                                       int separator_var2)
{
    int index;
    int internal_variable = -1;

    if (bag == NULL || bag->variable_count != 3 || bag->provider_count != 2)
        return -1;
    for (index = 0; index < bag->variable_count; index++)
    {
        int variable = bag->variables[index];

        if (variable == separator_var1 || variable == separator_var2)
            continue;
        if (internal_variable != -1)
            return -1;
        internal_variable = variable;
    }
    return internal_variable;
}

static bool
generic_ghd_bag_reduction_materialized(AgeGenericJoinState *state,
                                       AgeGenericGHDBagPlan *bag)
{
    int index;

    if (bag == NULL)
        return false;
    for (index = 0; index < bag->provider_count; index++)
    {
        int provider_index = bag->provider_indexes[index];

        if (provider_index < 0 || provider_index >= state->provider_count)
            return false;
        if (!generic_provider_is_reduction_materialized(
                &state->providers[provider_index]))
        {
            return false;
        }
    }
    return true;
}

static bool
generic_ghd_path_pair_from_rows(AgeGenericProvider *provider1,
                                AgeGenericRow *row1,
                                AgeGenericProvider *provider2,
                                AgeGenericRow *row2,
                                int separator_var1,
                                int separator_var2,
                                int internal_variable,
                                AgeGenericGraphidPair *pair)
{
    graphid internal_key1;
    graphid internal_key2;
    bool have_key1;
    bool have_key2;

    if (!generic_provider_row_key_for_variable(provider1, row1,
                                               internal_variable,
                                               &internal_key1) ||
        !generic_provider_row_key_for_variable(provider2, row2,
                                               internal_variable,
                                               &internal_key2) ||
        internal_key1 != internal_key2)
    {
        return false;
    }

    have_key1 = generic_provider_row_key_for_variable(
        provider1, row1, separator_var1, &pair->key1) ||
        generic_provider_row_key_for_variable(
            provider2, row2, separator_var1, &pair->key1);
    have_key2 = generic_provider_row_key_for_variable(
        provider1, row1, separator_var2, &pair->key2) ||
        generic_provider_row_key_for_variable(
            provider2, row2, separator_var2, &pair->key2);
    return have_key1 && have_key2;
}

static bool
generic_pair_domain_contains(const AgeGenericGraphidPair *pairs,
                             int pair_count,
                             const AgeGenericGraphidPair *target)
{
    int low = 0;
    int high = pair_count;

    while (low < high)
    {
        int mid = low + (high - low) / 2;
        int cmp = compare_generic_graphid_pairs(&pairs[mid], target);

        if (cmp < 0)
            low = mid + 1;
        else if (cmp > 0)
            high = mid;
        else
            return true;
    }
    return false;
}

static void
append_generic_pair(MemoryContext context, AgeGenericGraphidPair **pairs,
                    int *pair_count, int *pair_capacity,
                    AgeGenericGraphidPair pair)
{
    MemoryContext oldcontext;

    if (*pair_count == *pair_capacity)
    {
        int new_capacity = *pair_capacity == 0 ? 128 : *pair_capacity * 2;

        if (new_capacity <= *pair_capacity ||
            (Size)new_capacity >
            MaxAllocSize / sizeof(AgeGenericGraphidPair))
        {
            ereport(ERROR,
                    (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                     errmsg("AGE Generic Join GHD pair domain is too large")));
        }
        oldcontext = MemoryContextSwitchTo(context);
        if (*pairs == NULL)
            *pairs = palloc(sizeof(AgeGenericGraphidPair) * new_capacity);
        else
            *pairs = repalloc(*pairs,
                              sizeof(AgeGenericGraphidPair) * new_capacity);
        MemoryContextSwitchTo(oldcontext);
        *pair_capacity = new_capacity;
    }
    (*pairs)[(*pair_count)++] = pair;
}

static AgeGenericGraphidPair *
build_generic_ghd_path_pair_domain(AgeGenericJoinState *state,
                                   AgeGenericGHDBagPlan *bag,
                                   int separator_var1,
                                   int separator_var2,
                                   MemoryContext context,
                                   int *pair_count)
{
    AgeGenericProvider *provider1;
    AgeGenericProvider *provider2;
    AgeGenericGraphidPair *pairs = NULL;
    int pair_capacity = 0;
    int internal_variable;
    int row1_index;
    int row2_index;
    int output_count = 0;
    int index;

    *pair_count = 0;
    internal_variable = generic_ghd_path_bag_internal_variable(
        bag, separator_var1, separator_var2);
    if (internal_variable < 0)
        return NULL;

    provider1 = &state->providers[bag->provider_indexes[0]];
    provider2 = &state->providers[bag->provider_indexes[1]];
    if (provider1->kind != AGE_GENERIC_PROVIDER_EDGE ||
        provider2->kind != AGE_GENERIC_PROVIDER_EDGE ||
        !generic_provider_is_reduction_materialized(provider1) ||
        !generic_provider_is_reduction_materialized(provider2) ||
        provider1->row_count <= 0 || provider2->row_count <= 0)
    {
        return NULL;
    }

    for (row1_index = 0; row1_index < provider1->row_count; row1_index++)
    {
        for (row2_index = 0; row2_index < provider2->row_count;
             row2_index++)
        {
            AgeGenericGraphidPair pair;

            if (!generic_ghd_path_pair_from_rows(
                    provider1, &provider1->rows[row1_index],
                    provider2, &provider2->rows[row2_index],
                    separator_var1, separator_var2, internal_variable,
                    &pair))
            {
                continue;
            }
            append_generic_pair(context, &pairs, pair_count, &pair_capacity,
                                pair);
        }
    }
    if (*pair_count <= 0)
        return NULL;

    if (*pair_count > 1)
    {
        qsort(pairs, *pair_count, sizeof(AgeGenericGraphidPair),
              compare_generic_graphid_pairs);
    }
    for (index = 0; index < *pair_count; index++)
    {
        if (output_count == 0 ||
            compare_generic_graphid_pairs(&pairs[index],
                                          &pairs[output_count - 1]) != 0)
        {
            pairs[output_count++] = pairs[index];
        }
    }
    *pair_count = output_count;
    return pairs;
}

static int64
compact_generic_provider_with_bitmap(AgeGenericProvider *provider,
                                     const bool *keep)
{
    int old_count = provider->row_count;
    int read_index;
    int write_index = 0;

    for (read_index = 0; read_index < old_count; read_index++)
    {
        if (!keep[read_index])
            continue;
        if (write_index != read_index)
            provider->rows[write_index] = provider->rows[read_index];
        write_index++;
    }
    provider->row_count = write_index;
    invalidate_generic_provider_prefix_ranges(provider);
    return (int64)old_count - write_index;
}

static int64
filter_generic_ghd_path_bag_on_pair_domain(
    AgeGenericJoinState *state, AgeGenericGHDBagPlan *bag,
    int separator_var1, int separator_var2,
    const AgeGenericGraphidPair *pairs, int pair_count,
    MemoryContext context)
{
    AgeGenericProvider *provider1;
    AgeGenericProvider *provider2;
    bool *keep1;
    bool *keep2;
    int internal_variable;
    int row1_index;
    int row2_index;
    int64 rows_removed = 0;

    internal_variable = generic_ghd_path_bag_internal_variable(
        bag, separator_var1, separator_var2);
    if (internal_variable < 0 || pair_count <= 0)
        return 0;
    provider1 = &state->providers[bag->provider_indexes[0]];
    provider2 = &state->providers[bag->provider_indexes[1]];
    if (provider1->kind != AGE_GENERIC_PROVIDER_EDGE ||
        provider2->kind != AGE_GENERIC_PROVIDER_EDGE ||
        !generic_provider_is_reduction_materialized(provider1) ||
        !generic_provider_is_reduction_materialized(provider2) ||
        provider1->row_count <= 0 || provider2->row_count <= 0)
    {
        return 0;
    }

    keep1 = MemoryContextAllocZero(context,
                                   sizeof(bool) * provider1->row_count);
    keep2 = MemoryContextAllocZero(context,
                                   sizeof(bool) * provider2->row_count);
    for (row1_index = 0; row1_index < provider1->row_count; row1_index++)
    {
        for (row2_index = 0; row2_index < provider2->row_count;
             row2_index++)
        {
            AgeGenericGraphidPair pair;

            if (!generic_ghd_path_pair_from_rows(
                    provider1, &provider1->rows[row1_index],
                    provider2, &provider2->rows[row2_index],
                    separator_var1, separator_var2, internal_variable,
                    &pair))
            {
                continue;
            }
            if (!generic_pair_domain_contains(pairs, pair_count, &pair))
                continue;
            keep1[row1_index] = true;
            keep2[row2_index] = true;
        }
    }

    rows_removed += compact_generic_provider_with_bitmap(provider1, keep1);
    rows_removed += compact_generic_provider_with_bitmap(provider2, keep2);
    if (provider1->row_count <= 0 || provider2->row_count <= 0)
        state->exhausted = true;
    return rows_removed;
}

static void
reduce_generic_pair_ghd_separators(AgeGenericJoinState *state,
                                   MemoryContext reduction_context)
{
    bool found_pair_separator = false;
    int separator_index;

    if (state->reduction_ghd_bags == NULL ||
        state->reduction_ghd_separators == NULL ||
        state->reduction_ghd_separator_count <= 0)
    {
        return;
    }

    for (separator_index = 0;
         separator_index < state->reduction_ghd_separator_count;
         separator_index++)
    {
        AgeGenericGHDSeparatorPlan *plan =
            &state->reduction_ghd_separators[separator_index];
        AgeGenericGHDBagPlan *parent_bag;
        AgeGenericGHDBagPlan *child_bag;
        AgeGenericGraphidPair *pair_domain;
        int pair_domain_count;
        int64 rows_removed;

        if (plan->kind != AGE_GENERIC_GHD_SEPARATOR_PAIR)
            continue;
        parent_bag = &state->reduction_ghd_bags[plan->parent_bag_id];
        child_bag = &state->reduction_ghd_bags[plan->child_bag_id];
        if (!generic_ghd_bag_reduction_materialized(state, parent_bag) ||
            !generic_ghd_bag_reduction_materialized(state, child_bag))
        {
            continue;
        }
        found_pair_separator = true;
        pair_domain = build_generic_ghd_path_pair_domain(
            state, child_bag, plan->separator_variables[0],
            plan->separator_variables[1], reduction_context,
            &pair_domain_count);
        if (pair_domain_count <= 0)
        {
            state->exhausted = true;
            break;
        }
        increment_generic_counter(&state->separator_descriptor_applications);
        add_generic_counter(&state->separator_domain_keys,
                            pair_domain_count);
        rows_removed = filter_generic_ghd_path_bag_on_pair_domain(
            state, parent_bag, plan->separator_variables[0],
            plan->separator_variables[1], pair_domain, pair_domain_count,
            reduction_context);
        add_generic_counter(&state->separator_cyclic_core_rows_removed,
                            rows_removed);
        add_generic_counter(&state->semijoin_rows_removed, rows_removed);
        if (state->exhausted)
            break;

        pair_domain = build_generic_ghd_path_pair_domain(
            state, parent_bag, plan->separator_variables[0],
            plan->separator_variables[1], reduction_context,
            &pair_domain_count);
        if (pair_domain_count <= 0)
        {
            state->exhausted = true;
            break;
        }
        add_generic_counter(&state->separator_domain_keys,
                            pair_domain_count);
        rows_removed = filter_generic_ghd_path_bag_on_pair_domain(
            state, child_bag, plan->separator_variables[0],
            plan->separator_variables[1], pair_domain, pair_domain_count,
            reduction_context);
        add_generic_counter(&state->separator_cyclic_core_rows_removed,
                            rows_removed);
        add_generic_counter(&state->semijoin_rows_removed, rows_removed);
        if (state->exhausted)
            break;
    }

    if (found_pair_separator)
        increment_generic_counter(&state->separator_reduction_passes);
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

            if (plan->kind != AGE_GENERIC_GHD_SEPARATOR_LEAF_TAIL)
                continue;

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

            if (!generic_provider_is_reduction_materialized(provider))
                continue;
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
            if (!generic_provider_is_reduction_materialized(provider))
                continue;

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
            if (!generic_provider_is_reduction_materialized(provider))
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
    if (generic_reduction_order_applicable(state))
        return 2;
    return Max(state->variable_count * 2, 1);
}

static void
reduce_generic_providers(AgeGenericJoinState *state)
{
    MemoryContext reduction_context;
    int max_passes = generic_semijoin_pass_budget(state);
    int pass;
    int64 step_rows_removed;

    reduction_context = AllocSetContextCreate(
        state->css.ss.ps.state->es_query_cxt,
        "AGE Generic Join semijoin reduction", ALLOCSET_SMALL_SIZES);
    state->semijoin_provider_rows_after = 0;
    state->semijoin_final_domain_keys = 0;
    step_rows_removed = reduce_generic_semijoin_steps(
        state, reduction_context);
    add_generic_counter(&state->semijoin_rows_removed, step_rows_removed);
    if (state->exhausted)
    {
        state->peak_memory = Max(
            state->peak_memory,
            MemoryContextMemAllocated(state->data_context, true) +
            MemoryContextMemAllocated(reduction_context, true));
        state->semijoin_provider_rows_after =
            generic_provider_row_total(state);
        state->semijoin_final_domain_keys = 0;
        MemoryContextDelete(reduction_context);
        return;
    }
    reduce_generic_leaf_tail_separators(state, reduction_context);
    if (!state->exhausted)
        reduce_generic_pair_ghd_separators(state, reduction_context);
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
        if (generic_provider_is_lazy_physical(provider) &&
            provider->var1 == depth)
        {
            continue;
        }
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

            if (generic_provider_is_lazy_physical(provider))
            {
                generic_physical_provider_pair_range(state, provider, key1,
                                                     key2, &start, &end);
            }
            else
            {
                generic_provider_pair_range(state, provider, key1, key2,
                                            &start, &end);
            }
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
            AgeGenericRow *row = generic_provider_bag_row(provider,
                                                          local_index);
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

static bool
age_generic_uniqueness_may_reject(AgeGenericJoinState *state)
{
    int left;
    int right;

    if (state->uniqueness_group_count <= 0)
        return false;

    for (left = 0; left < state->provider_count; left++)
    {
        for (right = left + 1; right < state->provider_count; right++)
        {
            if (bms_overlap(state->uniqueness_groups[left],
                            state->uniqueness_groups[right]))
            {
                return true;
            }
        }
    }
    return false;
}

static bool
generic_provider_bag_single_edge_label(AgeGenericProvider *provider,
                                       int32 *label_id)
{
    int local_index;
    bool found = false;
    int32 found_label = INVALID_LABEL_ID;

    for (local_index = 0; local_index < provider->bag_count; local_index++)
    {
        AgeGenericRow *row = generic_provider_bag_row(provider,
                                                      local_index);
        int32 row_label;

        if (!row->edge_id_valid)
            return false;
        row_label = GET_LABEL_ID(row->edge_id);
        if (!label_id_is_valid(row_label))
            return false;
        if (!found)
        {
            found = true;
            found_label = row_label;
        }
        else if (found_label != row_label)
            return false;
    }

    if (!found)
        return false;
    *label_id = found_label;
    return true;
}

static bool
generic_provider_bag_has_common_edge_id(AgeGenericProvider *left,
                                        AgeGenericProvider *right)
{
    int left_index;

    for (left_index = 0; left_index < left->bag_count; left_index++)
    {
        AgeGenericRow *left_row = generic_provider_bag_row(left, left_index);
        int right_index;

        if (!left_row->edge_id_valid)
            continue;
        for (right_index = 0; right_index < right->bag_count; right_index++)
        {
            AgeGenericRow *right_row =
                generic_provider_bag_row(right, right_index);

            if (right_row->edge_id_valid &&
                left_row->edge_id == right_row->edge_id)
            {
                return true;
            }
        }
    }
    return false;
}

static bool
generic_provider_bags_may_share_edge_id(AgeGenericProvider *left,
                                        AgeGenericProvider *right)
{
    int32 left_label;
    int32 right_label;
    int64 pair_count;

    if (left->bag_count <= 0 || right->bag_count <= 0)
        return false;
    if (generic_provider_bag_single_edge_label(left, &left_label) &&
        generic_provider_bag_single_edge_label(right, &right_label) &&
        left_label != right_label)
    {
        return false;
    }

    if (left->bag_count >
        AGE_GENERIC_UNIQUENESS_EXACT_PAIR_LIMIT / right->bag_count)
    {
        return true;
    }
    pair_count = (int64)left->bag_count * right->bag_count;
    if (pair_count > AGE_GENERIC_UNIQUENESS_EXACT_PAIR_LIMIT)
        return true;

    return generic_provider_bag_has_common_edge_id(left, right);
}

static bool
age_generic_active_binding_uniqueness_may_reject(AgeGenericJoinState *state)
{
    int left;
    int right;

    for (left = 0; left < state->provider_count; left++)
    {
        for (right = left + 1; right < state->provider_count; right++)
        {
            if (!bms_overlap(state->uniqueness_groups[left],
                             state->uniqueness_groups[right]))
            {
                continue;
            }
            if (generic_provider_bags_may_share_edge_id(
                    &state->providers[left], &state->providers[right]))
            {
                return true;
            }
        }
    }
    return false;
}

static bool
age_generic_count_binding_product_safe(AgeGenericJoinState *state,
                                       bool uniqueness_may_reject)
{
    return !uniqueness_may_reject ||
        !age_generic_active_binding_uniqueness_may_reject(state);
}

static int64
count_age_generic_binding_product(AgeGenericJoinState *state)
{
    int64 product = 1;
    int provider_index;

    for (provider_index = 0;
         provider_index < state->provider_count;
         provider_index++)
    {
        AgeGenericProvider *provider = &state->providers[provider_index];

        if (provider->bag_count <= 0)
            return 0;
        product = generic_checked_int64_product(product,
                                                provider->bag_count);
    }
    return product;
}

static bool
age_generic_binding_product_within_limit(AgeGenericJoinState *state,
                                         int64 limit, int64 *product)
{
    int64 result = 1;
    int provider_index;

    if (limit <= 0)
        return false;
    for (provider_index = 0;
         provider_index < state->provider_count;
         provider_index++)
    {
        AgeGenericProvider *provider = &state->providers[provider_index];

        if (provider->bag_count <= 0)
            return false;
        if (provider->bag_count > limit ||
            result > limit / provider->bag_count)
        {
            return false;
        }
        result *= provider->bag_count;
    }

    *product = result;
    return true;
}

static int
age_generic_provider_bag_count(void *callback_state, int provider_index)
{
    AgeGenericJoinState *state = callback_state;

    if (state == NULL || provider_index < 0 ||
        provider_index >= state->provider_count)
    {
        elog(ERROR, "invalid AGE Generic Join small-bag provider index");
    }
    return state->providers[provider_index].bag_count;
}

static bool
age_generic_provider_bag_edge_id(void *callback_state, int provider_index,
                                 int tuple_index, graphid *edge_id)
{
    AgeGenericJoinState *state = callback_state;
    AgeGenericProvider *provider;
    AgeGenericRow *row;

    if (state == NULL || provider_index < 0 ||
        provider_index >= state->provider_count)
    {
        elog(ERROR, "invalid AGE Generic Join small-bag provider index");
    }
    provider = &state->providers[provider_index];
    if (tuple_index < 0 || tuple_index >= provider->bag_count)
        elog(ERROR, "invalid AGE Generic Join small-bag row index");

    row = generic_provider_bag_row(provider, tuple_index);
    if (!row->edge_id_valid)
        return false;
    *edge_id = row->edge_id;
    return true;
}

static bool
begin_age_generic_small_bag_enumerator(AgeGenericJoinState *state,
                                       int64 *candidate_rows,
                                       int64 *uniqueness_rejects_before)
{
    int64 product;

    if (!state->binding_ready)
        return false;
    if (!age_generic_binding_product_within_limit(
            state, AGE_GENERIC_SMALL_BAG_ENUMERATOR_LIMIT, &product))
    {
        return false;
    }

    *candidate_rows = age_binding_begin_flat_enumerator(
        &state->small_bag_enumerator, age_generic_provider_bag_count,
        age_generic_provider_bag_edge_id, state, state->uniqueness_groups);
    if (*candidate_rows != product)
        elog(ERROR, "AGE Generic Join small-bag cardinality mismatch");
    *uniqueness_rejects_before =
        age_binding_flat_enumerator_uniqueness_rejects(
            &state->small_bag_enumerator);

    state->small_bag_enumerator_used = true;
    increment_generic_counter(&state->small_bag_enumerator_bindings);
    add_generic_counter(&state->small_bag_enumerator_candidate_rows,
                        *candidate_rows);
    return true;
}

static void
finish_age_generic_small_bag_enumerator(
    AgeGenericJoinState *state, int64 accepted_rows,
    int64 uniqueness_rejects_before)
{
    int64 uniqueness_rejects_after =
        age_binding_flat_enumerator_uniqueness_rejects(
            &state->small_bag_enumerator);
    int64 uniqueness_reject_delta =
        uniqueness_rejects_after - uniqueness_rejects_before;

    add_generic_counter(&state->candidate_combinations, accepted_rows);
    add_generic_counter(&state->consumer_flat_rows_avoided, accepted_rows);
    add_generic_counter(&state->small_bag_enumerator_rows, accepted_rows);
    add_generic_counter(&state->uniqueness_rejects, uniqueness_reject_delta);
    state->binding_ready = false;
    state->combination_started = false;
}

static bool
count_age_generic_small_binding(AgeGenericJoinState *state, int64 *rows)
{
    int64 candidate_rows;
    int64 uniqueness_rejects_before;
    int64 accepted_rows = 0;

    if (!begin_age_generic_small_bag_enumerator(
            state, &candidate_rows, &uniqueness_rejects_before))
    {
        return false;
    }

    while (age_binding_flat_enumerator_next(&state->small_bag_enumerator))
        accepted_rows = add_generic_count_result(accepted_rows, 1);

    finish_age_generic_small_bag_enumerator(
        state, accepted_rows, uniqueness_rejects_before);
    *rows = accepted_rows;
    return true;
}

static void
free_age_generic_agtype_value(agtype_value *value, bool needs_free)
{
    if (needs_free)
        pfree_agtype_value_content(value);
}

static void
free_age_generic_detoasted_agtype(Datum original, agtype *value)
{
    if (value != NULL && (Pointer)value != DatumGetPointer(original))
        pfree(value);
}

static Datum
age_generic_numeric_from_value(agtype_value *value)
{
    switch (value->type)
    {
    case AGTV_INTEGER:
        return DirectFunctionCall1(int8_numeric,
                                   Int64GetDatum(value->val.int_value));
    case AGTV_FLOAT:
        return DirectFunctionCall1(float8_numeric,
                                   Float8GetDatum(value->val.float_value));
    case AGTV_NUMERIC:
        return NumericGetDatum(value->val.numeric);
    default:
        break;
    }

    ereport(ERROR,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
             errmsg("arguments must resolve to a number")));
    return (Datum)0;
}

static Datum
age_generic_numeric_mul_int64(Datum value, int64 multiplier)
{
    Datum multiplier_numeric;

    if (multiplier == 1)
        return NumericGetDatum(DatumGetNumericCopy(value));
    multiplier_numeric = DirectFunctionCall1(int8_numeric,
                                             Int64GetDatum(multiplier));
    return DirectFunctionCall2(numeric_mul, value, multiplier_numeric);
}

static Datum
age_generic_numeric_add(Datum left, Datum right)
{
    return DirectFunctionCall2(numeric_add, left, right);
}

static float8
age_generic_float8_from_value(agtype_value *value)
{
    switch (value->type)
    {
    case AGTV_INTEGER:
        return (float8)value->val.int_value;
    case AGTV_FLOAT:
        return value->val.float_value;
    case AGTV_NUMERIC:
        return DatumGetFloat8(DirectFunctionCall1(
            numeric_float8, NumericGetDatum(value->val.numeric)));
    default:
        break;
    }

    ereport(ERROR,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
             errmsg("arguments must resolve to a number")));
    return 0.0;
}

static void
age_generic_sum_promote_to_numeric(AgeGenericJoinState *state)
{
    Datum numeric;

    if (!state->property_aggregate_has_value ||
        state->property_aggregate_sum_value.type == AGTV_NUMERIC)
    {
        return;
    }

    if (state->property_aggregate_sum_value.type == AGTV_INTEGER)
    {
        numeric = DirectFunctionCall1(
            int8_numeric,
            Int64GetDatum(
                state->property_aggregate_sum_value.val.int_value));
    }
    else if (state->property_aggregate_sum_value.type == AGTV_FLOAT)
    {
        numeric = DirectFunctionCall1(
            float8_numeric,
            Float8GetDatum(
                state->property_aggregate_sum_value.val.float_value));
    }
    else
    {
        elog(ERROR, "invalid AGE Generic Join property aggregate state");
        numeric = (Datum)0;
    }

    state->property_aggregate_sum_value.type = AGTV_NUMERIC;
    state->property_aggregate_sum_value.val.numeric =
        DatumGetNumeric(numeric);
}

static void
age_generic_sum_promote_to_float(AgeGenericJoinState *state)
{
    if (!state->property_aggregate_has_value ||
        state->property_aggregate_sum_value.type == AGTV_FLOAT)
    {
        return;
    }
    if (state->property_aggregate_sum_value.type != AGTV_INTEGER)
        elog(ERROR, "invalid AGE Generic Join property aggregate state");

    state->property_aggregate_sum_value.type = AGTV_FLOAT;
    state->property_aggregate_sum_value.val.float_value =
        (float8)state->property_aggregate_sum_value.val.int_value;
}

static void
add_age_generic_avg_value(AgeGenericJoinState *state, agtype_value *value,
                          int64 multiplicity)
{
    float8 addend;

    if (multiplicity <= 0)
        return;
    addend = DatumGetFloat8(DirectFunctionCall2(
        float8mul, Float8GetDatum(age_generic_float8_from_value(value)),
        Float8GetDatum((float8)multiplicity)));
    if (!state->property_aggregate_has_value)
    {
        state->property_aggregate_avg_sum = addend;
        state->property_aggregate_has_value = true;
    }
    else
    {
        state->property_aggregate_avg_sum = DatumGetFloat8(
            DirectFunctionCall2(
                float8pl,
                Float8GetDatum(state->property_aggregate_avg_sum),
                Float8GetDatum(addend)));
    }
}

static void
add_age_generic_sum_value(AgeGenericJoinState *state, agtype_value *value,
                          int64 multiplicity)
{
    MemoryContext oldcontext;

    if (multiplicity <= 0)
        return;
    if (value->type != AGTV_INTEGER &&
        value->type != AGTV_FLOAT &&
        value->type != AGTV_NUMERIC)
    {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("arguments must resolve to a number")));
    }

    oldcontext = MemoryContextSwitchTo(state->css.ss.ps.state->es_query_cxt);
    if (value->type == AGTV_NUMERIC)
    {
        Datum addend;

        age_generic_sum_promote_to_numeric(state);
        addend = age_generic_numeric_mul_int64(
            age_generic_numeric_from_value(value), multiplicity);
        if (!state->property_aggregate_has_value)
        {
            state->property_aggregate_sum_value.type = AGTV_NUMERIC;
            state->property_aggregate_sum_value.val.numeric =
                DatumGetNumeric(addend);
            state->property_aggregate_has_value = true;
        }
        else
        {
            state->property_aggregate_sum_value.val.numeric =
                DatumGetNumeric(age_generic_numeric_add(
                    NumericGetDatum(
                        state->property_aggregate_sum_value.val.numeric),
                    addend));
        }
    }
    else if (value->type == AGTV_FLOAT)
    {
        float8 addend;

        if (state->property_aggregate_has_value &&
            state->property_aggregate_sum_value.type == AGTV_NUMERIC)
        {
            Datum numeric_addend = age_generic_numeric_mul_int64(
                age_generic_numeric_from_value(value), multiplicity);

            state->property_aggregate_sum_value.val.numeric =
                DatumGetNumeric(age_generic_numeric_add(
                    NumericGetDatum(
                        state->property_aggregate_sum_value.val.numeric),
                    numeric_addend));
            MemoryContextSwitchTo(oldcontext);
            return;
        }

        age_generic_sum_promote_to_float(state);
        addend = DatumGetFloat8(DirectFunctionCall2(
            float8mul, Float8GetDatum(value->val.float_value),
            Float8GetDatum((float8)multiplicity)));
        if (!state->property_aggregate_has_value)
        {
            state->property_aggregate_sum_value.type = AGTV_FLOAT;
            state->property_aggregate_sum_value.val.float_value = addend;
            state->property_aggregate_has_value = true;
        }
        else
        {
            state->property_aggregate_sum_value.val.float_value =
                DatumGetFloat8(DirectFunctionCall2(
                    float8pl,
                    Float8GetDatum(
                        state->property_aggregate_sum_value.val.float_value),
                    Float8GetDatum(addend)));
        }
    }
    else
    {
        int64 addend = generic_checked_int64_product(value->val.int_value,
                                                     multiplicity);

        if (state->property_aggregate_has_value &&
            state->property_aggregate_sum_value.type == AGTV_NUMERIC)
        {
            Datum numeric_addend = age_generic_numeric_mul_int64(
                age_generic_numeric_from_value(value), multiplicity);

            state->property_aggregate_sum_value.val.numeric =
                DatumGetNumeric(age_generic_numeric_add(
                    NumericGetDatum(
                        state->property_aggregate_sum_value.val.numeric),
                    numeric_addend));
        }
        else if (state->property_aggregate_has_value &&
                 state->property_aggregate_sum_value.type == AGTV_FLOAT)
        {
            state->property_aggregate_sum_value.val.float_value =
                DatumGetFloat8(DirectFunctionCall2(
                    float8pl,
                    Float8GetDatum(
                        state->property_aggregate_sum_value.val.float_value),
                    Float8GetDatum((float8)addend)));
        }
        else if (!state->property_aggregate_has_value)
        {
            state->property_aggregate_sum_value.type = AGTV_INTEGER;
            state->property_aggregate_sum_value.val.int_value = addend;
            state->property_aggregate_has_value = true;
        }
        else
        {
            state->property_aggregate_sum_value.val.int_value =
                generic_checked_int64_sum(
                    state->property_aggregate_sum_value.val.int_value,
                    addend);
        }
    }
    MemoryContextSwitchTo(oldcontext);
}

static void
add_age_generic_minmax_value(AgeGenericJoinState *state,
                             agtype_value *value)
{
    agtype *candidate;
    bool replace_value;

    candidate = agtype_value_to_agtype(value);
    if (!state->property_aggregate_has_value)
    {
        replace_value = true;
    }
    else
    {
        int cmp = compare_agtype_containers_orderability(
            &candidate->root,
            &state->property_aggregate_minmax_value->root);

        replace_value =
            (state->consumer_kind == AGE_GENERIC_CONSUMER_MAX_PROPERTY ||
             state->consumer_kind ==
             AGE_GENERIC_CONSUMER_GROUP_MAX_PROPERTY) ?
            cmp > 0 : cmp < 0;
    }

    if (replace_value)
    {
        MemoryContext oldcontext;

        oldcontext = MemoryContextSwitchTo(
            state->css.ss.ps.state->es_query_cxt);
        pfree_if_not_null(state->property_aggregate_minmax_value);
        state->property_aggregate_minmax_value =
            (agtype *)DatumGetPointer(
                datumCopy(PointerGetDatum(candidate), false, -1));
        state->property_aggregate_has_value = true;
        MemoryContextSwitchTo(oldcontext);
    }
    pfree(candidate);
}

static bool
age_generic_row_property_value(AgeGenericJoinState *state,
                               AgeGenericProvider *provider,
                               AgeGenericRow *row,
                               agtype_value *value,
                               agtype **properties,
                               agtype **key,
                               Datum *properties_datum,
                               bool *needs_free)
{
    bool isnull;
    agtype_value key_value;
    bool key_needs_free = false;
    bool found;

    *needs_free = false;
    *properties = NULL;
    *key = NULL;
    *properties_datum = (Datum)0;
    if (provider->tuple_slot == NULL ||
        row == NULL ||
        row->tuple == NULL ||
        provider->properties_attno <= 0 ||
        state->property_aggregate_key_isnull)
    {
        return false;
    }

    ExecStoreMinimalTuple(row->tuple, provider->tuple_slot, false);
    *properties_datum =
        slot_getattr(provider->tuple_slot, provider->properties_attno,
                     &isnull);
    if (isnull)
        return false;

    *properties = DATUM_GET_AGTYPE_P(*properties_datum);
    *key = DATUM_GET_AGTYPE_P(state->property_aggregate_key);
    if (!AGT_ROOT_IS_OBJECT(*properties) || !AGT_ROOT_IS_SCALAR(*key))
        return false;
    if (!get_ith_agtype_value_from_container_no_copy(&(*key)->root, 0,
                                                     &key_value,
                                                     &key_needs_free))
    {
        return false;
    }
    if (key_value.type != AGTV_STRING)
    {
        free_age_generic_agtype_value(&key_value, key_needs_free);
        return false;
    }

    found = find_agtype_value_from_container_no_copy(
        &(*properties)->root, AGT_FOBJECT, &key_value, value, needs_free);
    free_age_generic_agtype_value(&key_value, key_needs_free);
    if (!found || value->type == AGTV_NULL)
    {
        if (found)
            free_age_generic_agtype_value(value, *needs_free);
        *needs_free = false;
        return false;
    }
    return true;
}

static void
add_age_generic_property_row(AgeGenericJoinState *state,
                             AgeGenericProvider *provider,
                             AgeGenericRow *row, int64 multiplicity)
{
    agtype_value value;
    agtype *properties;
    agtype *key;
    Datum properties_datum;
    bool needs_free = false;
    bool found;

    if (multiplicity <= 0)
        return;

    found = age_generic_row_property_value(state, provider, row,
                                           &value, &properties, &key,
                                           &properties_datum, &needs_free);
    if (!found)
    {
        state->property_aggregate_null_rows = add_generic_count_result(
            state->property_aggregate_null_rows, multiplicity);
    }
    else
    {
        state->property_aggregate_input_rows = add_generic_count_result(
            state->property_aggregate_input_rows, multiplicity);
        state->property_aggregate_value_input_rows =
            add_generic_count_result(
                state->property_aggregate_value_input_rows, multiplicity);
        if (age_generic_property_avg_consumer(state->consumer_kind))
            add_age_generic_avg_value(state, &value, multiplicity);
        else if (age_generic_property_minmax_consumer(state->consumer_kind))
            add_age_generic_minmax_value(state, &value);
        else
            add_age_generic_sum_value(state, &value, multiplicity);
        free_age_generic_agtype_value(&value, needs_free);
    }

    free_age_generic_detoasted_agtype(properties_datum, properties);
    free_age_generic_detoasted_agtype(state->property_aggregate_key, key);
}

static void
ensure_age_generic_small_bag_multiplicity_capacity(
    AgeGenericJoinState *state, int capacity)
{
    MemoryContext oldcontext;

    if (capacity <= state->small_bag_row_multiplicity_capacity)
        return;
    if ((Size)capacity > MaxAllocSize / sizeof(int64))
    {
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("AGE Generic Join small-bag multiplicity scratch is "
                        "too large")));
    }

    oldcontext = MemoryContextSwitchTo(
        state->css.ss.ps.state->es_query_cxt);
    if (state->small_bag_row_multiplicities == NULL)
        state->small_bag_row_multiplicities =
            palloc0(sizeof(int64) * capacity);
    else
        state->small_bag_row_multiplicities = repalloc0(
            state->small_bag_row_multiplicities,
            sizeof(int64) * state->small_bag_row_multiplicity_capacity,
            sizeof(int64) * capacity);
    MemoryContextSwitchTo(oldcontext);
    state->small_bag_row_multiplicity_capacity = capacity;
}

static bool
add_age_generic_small_bag_property_rows(AgeGenericJoinState *state,
                                        int sum_index)
{
    AgeGenericProvider *sum_provider = &state->providers[sum_index];
    int64 candidate_rows;
    int64 uniqueness_rejects_before;
    int64 accepted_rows = 0;
    int local_index;

    if (!begin_age_generic_small_bag_enumerator(
            state, &candidate_rows, &uniqueness_rejects_before))
    {
        return false;
    }

    ensure_age_generic_small_bag_multiplicity_capacity(
        state, sum_provider->bag_count);
    memset(state->small_bag_row_multiplicities, 0,
           sizeof(int64) * sum_provider->bag_count);

    while (age_binding_flat_enumerator_next(&state->small_bag_enumerator))
    {
        int row_index = age_binding_flat_enumerator_index(
            &state->small_bag_enumerator, sum_index);

        if (row_index < 0 || row_index >= sum_provider->bag_count)
            elog(ERROR, "invalid AGE Generic Join property aggregate row");
        state->small_bag_row_multiplicities[row_index] =
            add_generic_count_result(
                state->small_bag_row_multiplicities[row_index], 1);
        accepted_rows = add_generic_count_result(accepted_rows, 1);
    }

    finish_age_generic_small_bag_enumerator(
        state, accepted_rows, uniqueness_rejects_before);

    for (local_index = 0;
         local_index < sum_provider->bag_count;
         local_index++)
    {
        int64 multiplicity =
            state->small_bag_row_multiplicities[local_index];

        if (multiplicity > 0)
        {
            AgeGenericRow *row = generic_provider_bag_row(
                sum_provider, local_index);

            add_age_generic_property_row(state, sum_provider, row,
                                         multiplicity);
        }
    }
    return true;
}

static void
add_age_generic_sum_active_binding(AgeGenericJoinState *state)
{
    int sum_index = state->property_aggregate_provider_index;
    AgeGenericProvider *sum_provider;

    if (!state->binding_ready ||
        sum_index < 0 || sum_index >= state->provider_count)
    {
        return;
    }

    sum_provider = &state->providers[sum_index];
    if (sum_provider->kind != AGE_GENERIC_PROVIDER_EDGE)
        elog(ERROR, "AGE Generic Join property aggregate provider is not edge");

    if (!age_generic_uniqueness_may_reject(state))
    {
        int64 other_product = 1;
        int64 binding_count = count_age_generic_binding_product(state);
        int provider_index;
        int local_index;

        for (provider_index = 0;
             provider_index < state->provider_count;
             provider_index++)
        {
            if (provider_index == sum_index)
                continue;
            other_product = generic_checked_int64_product(
                other_product, state->providers[provider_index].bag_count);
        }

        add_generic_counter(&state->candidate_combinations, binding_count);
        add_generic_counter(&state->consumer_flat_rows_avoided,
                            binding_count);
        for (local_index = 0;
             local_index < sum_provider->bag_count;
             local_index++)
        {
            AgeGenericRow *row = generic_provider_bag_row(sum_provider,
                                                          local_index);

            add_age_generic_property_row(state, sum_provider, row,
                                         other_product);
        }
        state->binding_ready = false;
        state->combination_started = false;
        return;
    }

    if (age_generic_active_binding_uniqueness_may_reject(state))
    {
        if (add_age_generic_small_bag_property_rows(state, sum_index))
            return;
    }

    while (next_generic_combination(state))
    {
        AgeGenericRow *row = generic_provider_bag_row(
            sum_provider, state->combination_indexes[sum_index]);

        increment_generic_counter(&state->consumer_flat_rows_avoided);
        add_age_generic_property_row(state, sum_provider, row, 1);
    }
}

static void
add_age_generic_sum_rows(AgeGenericJoinState *state)
{
    for (;;)
    {
        if (!state->binding_ready && !next_generic_binding(state))
            break;

        add_age_generic_sum_active_binding(state);
    }
}

static void
reset_age_generic_property_aggregate_value(AgeGenericJoinState *state)
{
    state->property_aggregate_has_value = false;
    state->property_aggregate_sum_value.type = AGTV_NULL;
    state->property_aggregate_avg_sum = 0.0;
    state->property_aggregate_value_input_rows = 0;
    pfree_if_not_null(state->property_aggregate_minmax_value);
    state->property_aggregate_minmax_value = NULL;
}

static void
reset_age_generic_property_aggregate_state(AgeGenericJoinState *state)
{
    state->property_aggregate_emitted = false;
    state->property_aggregate_executed = false;
    reset_age_generic_property_aggregate_value(state);
    state->property_aggregate_input_rows = 0;
    state->property_aggregate_null_rows = 0;
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
        AgeGenericRow *row = generic_provider_bag_row(
            provider, state->combination_indexes[provider_index]);
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
    increment_generic_counter(&state->flat_rows_materialized);
    return raw_slot;
}

static TupleTableSlot *
store_age_generic_count_result(ScanState *node, int64 count)
{
    AgeGenericJoinState *state = (AgeGenericJoinState *)node;
    TupleTableSlot *slot = node->ss_ScanTupleSlot;

    ExecClearTuple(slot);
    if (slot->tts_tupleDescriptor->natts < 1)
        elog(ERROR, "AGE Generic Join count scan expected an output column");
    memset(slot->tts_values, 0,
           sizeof(Datum) * slot->tts_tupleDescriptor->natts);
    memset(slot->tts_isnull, true,
           sizeof(bool) * slot->tts_tupleDescriptor->natts);

    if (state->consumer_output_type == INT8OID)
    {
        slot->tts_values[0] = Int64GetDatum(count);
    }
    else if (state->consumer_output_type == AGTYPEOID)
    {
        slot->tts_values[0] =
            PointerGetDatum(agtype_integer_to_agtype(count));
    }
    else
    {
        ereport(ERROR,
                (errcode(ERRCODE_DATATYPE_MISMATCH),
                 errmsg("AGE Generic Join count scan cannot produce type %s",
                        format_type_be(state->consumer_output_type))));
    }
    slot->tts_isnull[0] = false;

    ExecStoreVirtualTuple(slot);
    return slot;
}

static TupleTableSlot *
store_age_generic_group_count_result(ScanState *node, graphid key,
                                     int64 count)
{
    TupleTableSlot *slot = node->ss_ScanTupleSlot;
    TupleDesc tupdesc = slot->tts_tupleDescriptor;
    Oid key_type;
    Oid count_type;

    ExecClearTuple(slot);
    if (tupdesc->natts < 2)
        elog(ERROR,
             "AGE Generic Join group count scan expected two output columns");
    memset(slot->tts_values, 0, sizeof(Datum) * tupdesc->natts);
    memset(slot->tts_isnull, true, sizeof(bool) * tupdesc->natts);

    key_type = TupleDescAttr(tupdesc, 0)->atttypid;
    if (key_type == GRAPHIDOID)
        slot->tts_values[0] = GRAPHID_GET_DATUM(key);
    else if (key_type == INT8OID)
        slot->tts_values[0] = Int64GetDatum((int64)key);
    else if (key_type == AGTYPEOID)
        slot->tts_values[0] =
            PointerGetDatum(agtype_integer_to_agtype(key));
    else
    {
        ereport(ERROR,
                (errcode(ERRCODE_DATATYPE_MISMATCH),
                 errmsg("AGE Generic Join group count scan cannot produce "
                        "group type %s",
                        format_type_be(key_type))));
    }
    slot->tts_isnull[0] = false;

    count_type = TupleDescAttr(tupdesc, 1)->atttypid;
    if (count_type == INT8OID)
        slot->tts_values[1] = Int64GetDatum(count);
    else if (count_type == AGTYPEOID)
        slot->tts_values[1] =
            PointerGetDatum(agtype_integer_to_agtype(count));
    else
    {
        ereport(ERROR,
                (errcode(ERRCODE_DATATYPE_MISMATCH),
                 errmsg("AGE Generic Join group count scan cannot produce "
                        "count type %s",
                        format_type_be(count_type))));
    }
    slot->tts_isnull[1] = false;

    ExecStoreVirtualTuple(slot);
    return slot;
}

static TupleTableSlot *
store_age_generic_distinct_key_result(ScanState *node, graphid key)
{
    AgeGenericJoinState *state = (AgeGenericJoinState *)node;
    TupleTableSlot *slot = node->ss_ScanTupleSlot;

    ExecClearTuple(slot);
    if (slot->tts_tupleDescriptor->natts < 1)
        elog(ERROR,
             "AGE Generic Join distinct-key scan expected an output column");
    memset(slot->tts_values, 0,
           sizeof(Datum) * slot->tts_tupleDescriptor->natts);
    memset(slot->tts_isnull, true,
           sizeof(bool) * slot->tts_tupleDescriptor->natts);

    if (state->consumer_output_type == GRAPHIDOID)
        slot->tts_values[0] = GRAPHID_GET_DATUM(key);
    else if (state->consumer_output_type == INT8OID)
        slot->tts_values[0] = Int64GetDatum((int64)key);
    else if (state->consumer_output_type == AGTYPEOID)
        slot->tts_values[0] =
            PointerGetDatum(agtype_integer_to_agtype(key));
    else
    {
        ereport(ERROR,
                (errcode(ERRCODE_DATATYPE_MISMATCH),
                 errmsg("AGE Generic Join distinct-key scan cannot produce "
                        "type %s",
                        format_type_be(state->consumer_output_type))));
    }
    slot->tts_isnull[0] = false;

    ExecStoreVirtualTuple(slot);
    return slot;
}

static bool
age_generic_record_distinct_key(AgeGenericJoinState *state, graphid key)
{
    AgeGenericGraphidHashEntry *entry;
    HASHCTL hashctl;
    bool found;

    if (state->distinct_key_hash == NULL)
    {
        memset(&hashctl, 0, sizeof(hashctl));
        hashctl.keysize = sizeof(graphid);
        hashctl.entrysize = sizeof(AgeGenericGraphidHashEntry);
        hashctl.hcxt = state->data_context;
        state->distinct_key_hash = hash_create(
            "AGE Generic Join distinct keys", 128, &hashctl,
            HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
    }

    entry = hash_search(state->distinct_key_hash, &key, HASH_ENTER, &found);
    (void)entry;
    return !found;
}

static TupleTableSlot *
store_age_generic_sum_property_result(ScanState *node)
{
    AgeGenericJoinState *state = (AgeGenericJoinState *)node;
    TupleTableSlot *slot = node->ss_ScanTupleSlot;

    ExecClearTuple(slot);
    if (slot->tts_tupleDescriptor->natts < 1)
    {
        elog(ERROR,
             "AGE Generic Join property aggregate scan expected an "
             "output column");
    }
    memset(slot->tts_values, 0,
           sizeof(Datum) * slot->tts_tupleDescriptor->natts);
    memset(slot->tts_isnull, true,
           sizeof(bool) * slot->tts_tupleDescriptor->natts);

    if (age_generic_property_avg_consumer(state->consumer_kind))
    {
        float8 avg_value;

        if (state->consumer_output_type != AGTYPEOID &&
            state->consumer_output_type != FLOAT8OID)
        {
            ereport(ERROR,
                    (errcode(ERRCODE_DATATYPE_MISMATCH),
                     errmsg("AGE Generic Join property aggregate scan cannot "
                            "produce type %s",
                            format_type_be(state->consumer_output_type))));
        }
        if (!state->property_aggregate_has_value)
        {
            ExecStoreVirtualTuple(slot);
            return slot;
        }

        avg_value = DatumGetFloat8(DirectFunctionCall2(
            float8div, Float8GetDatum(state->property_aggregate_avg_sum),
            Float8GetDatum(
                (float8)state->property_aggregate_value_input_rows)));
        if (state->consumer_output_type == FLOAT8OID)
            slot->tts_values[0] = Float8GetDatum(avg_value);
        else
        {
            agtype_value value;

            value.type = AGTV_FLOAT;
            value.val.float_value = avg_value;
            slot->tts_values[0] =
                PointerGetDatum(agtype_value_to_agtype(&value));
        }
        slot->tts_isnull[0] = false;
        ExecStoreVirtualTuple(slot);
        return slot;
    }

    if (state->consumer_output_type != AGTYPEOID)
    {
        ereport(ERROR,
                (errcode(ERRCODE_DATATYPE_MISMATCH),
                 errmsg("AGE Generic Join property aggregate scan cannot "
                        "produce type %s",
                        format_type_be(state->consumer_output_type))));
    }

    if (state->property_aggregate_has_value &&
        age_generic_property_minmax_consumer(state->consumer_kind))
    {
        slot->tts_values[0] =
            PointerGetDatum(state->property_aggregate_minmax_value);
        slot->tts_isnull[0] = false;
    }
    else if (state->property_aggregate_has_value)
    {
        slot->tts_values[0] = PointerGetDatum(
            agtype_value_to_agtype(&state->property_aggregate_sum_value));
        slot->tts_isnull[0] = false;
    }

    ExecStoreVirtualTuple(slot);
    return slot;
}

static TupleTableSlot *
store_age_generic_group_sum_property_result(ScanState *node, graphid key)
{
    AgeGenericJoinState *state = (AgeGenericJoinState *)node;
    TupleTableSlot *slot = node->ss_ScanTupleSlot;
    TupleDesc tupdesc = slot->tts_tupleDescriptor;
    Oid key_type;
    Oid value_type;

    ExecClearTuple(slot);
    if (tupdesc->natts < 2)
    {
        elog(ERROR,
             "AGE Generic Join group property aggregate scan expected two "
             "output columns");
    }
    memset(slot->tts_values, 0, sizeof(Datum) * tupdesc->natts);
    memset(slot->tts_isnull, true, sizeof(bool) * tupdesc->natts);

    key_type = TupleDescAttr(tupdesc, 0)->atttypid;
    if (key_type == GRAPHIDOID)
        slot->tts_values[0] = GRAPHID_GET_DATUM(key);
    else if (key_type == INT8OID)
        slot->tts_values[0] = Int64GetDatum((int64)key);
    else if (key_type == AGTYPEOID)
        slot->tts_values[0] =
            PointerGetDatum(agtype_integer_to_agtype(key));
    else
    {
        ereport(ERROR,
                (errcode(ERRCODE_DATATYPE_MISMATCH),
                 errmsg("AGE Generic Join group property aggregate scan "
                        "cannot produce group type %s",
                        format_type_be(key_type))));
    }
    slot->tts_isnull[0] = false;

    value_type = TupleDescAttr(tupdesc, 1)->atttypid;
    if (age_generic_property_avg_consumer(state->consumer_kind))
    {
        float8 avg_value;

        if (value_type != AGTYPEOID && value_type != FLOAT8OID)
        {
            ereport(ERROR,
                    (errcode(ERRCODE_DATATYPE_MISMATCH),
                     errmsg("AGE Generic Join group property aggregate scan "
                            "cannot produce value type %s",
                            format_type_be(value_type))));
        }
        if (!state->property_aggregate_has_value)
        {
            ExecStoreVirtualTuple(slot);
            return slot;
        }

        avg_value = DatumGetFloat8(DirectFunctionCall2(
            float8div, Float8GetDatum(state->property_aggregate_avg_sum),
            Float8GetDatum(
                (float8)state->property_aggregate_value_input_rows)));
        if (value_type == FLOAT8OID)
            slot->tts_values[1] = Float8GetDatum(avg_value);
        else
        {
            agtype_value value;

            value.type = AGTV_FLOAT;
            value.val.float_value = avg_value;
            slot->tts_values[1] =
                PointerGetDatum(agtype_value_to_agtype(&value));
        }
        slot->tts_isnull[1] = false;
        ExecStoreVirtualTuple(slot);
        return slot;
    }

    if (value_type != AGTYPEOID)
    {
        ereport(ERROR,
                (errcode(ERRCODE_DATATYPE_MISMATCH),
                 errmsg("AGE Generic Join group property aggregate scan "
                        "cannot produce value type %s",
                        format_type_be(value_type))));
    }
    if (state->property_aggregate_has_value &&
        age_generic_property_minmax_consumer(state->consumer_kind))
    {
        slot->tts_values[1] =
            PointerGetDatum(state->property_aggregate_minmax_value);
        slot->tts_isnull[1] = false;
    }
    else if (state->property_aggregate_has_value)
    {
        slot->tts_values[1] = PointerGetDatum(
            agtype_value_to_agtype(&state->property_aggregate_sum_value));
        slot->tts_isnull[1] = false;
    }

    ExecStoreVirtualTuple(slot);
    return slot;
}

static TupleTableSlot *
store_age_generic_exists_result(ScanState *node)
{
    AgeGenericJoinState *state = (AgeGenericJoinState *)node;
    TupleTableSlot *slot = node->ss_ScanTupleSlot;

    ExecClearTuple(slot);
    if (slot->tts_tupleDescriptor->natts < 1)
        elog(ERROR, "AGE Generic Join exists scan expected an output column");
    memset(slot->tts_values, 0,
           sizeof(Datum) * slot->tts_tupleDescriptor->natts);
    memset(slot->tts_isnull, true,
           sizeof(bool) * slot->tts_tupleDescriptor->natts);

    if (state->consumer_output_type != AGTYPEOID)
    {
        ereport(ERROR,
                (errcode(ERRCODE_DATATYPE_MISMATCH),
                 errmsg("AGE Generic Join exists scan cannot produce type %s",
                        format_type_be(state->consumer_output_type))));
    }
    slot->tts_values[0] = boolean_to_agtype(true);
    slot->tts_isnull[0] = false;

    ExecStoreVirtualTuple(slot);
    return slot;
}

static TupleTableSlot *
access_age_generic_sum_property_scan(ScanState *node)
{
    AgeGenericJoinState *state = (AgeGenericJoinState *)node;

    if (state->property_aggregate_emitted)
        return ExecClearTuple(node->ss_ScanTupleSlot);

    state->property_aggregate_emitted = true;
    state->property_aggregate_executed = true;
    add_age_generic_sum_rows(state);
    return store_age_generic_sum_property_result(node);
}

static TupleTableSlot *
access_age_generic_group_sum_property_scan(ScanState *node)
{
    AgeGenericJoinState *state = (AgeGenericJoinState *)node;

    state->property_aggregate_executed = true;
    for (;;)
    {
        graphid key;

        if (!state->binding_ready && !next_generic_binding(state))
            return ExecClearTuple(node->ss_ScanTupleSlot);

        key = state->bindings[state->distinct_variable];
        reset_age_generic_property_aggregate_value(state);
        for (;;)
        {
            if (!state->binding_ready)
                break;
            if (state->bindings[state->distinct_variable] != key)
                break;

            add_age_generic_sum_active_binding(state);
            state->binding_ready = false;
            state->combination_started = false;
            if (!next_generic_binding(state))
                break;
        }

        return store_age_generic_group_sum_property_result(node, key);
    }
}

static TupleTableSlot *
access_age_generic_join(ScanState *node)
{
    AgeGenericJoinState *state = (AgeGenericJoinState *)node;

    if (state->row_goal > 0 && state->row_goal_emitted >= state->row_goal)
        return ExecClearTuple(node->ss_ScanTupleSlot);

    for (;;)
    {
        if (!state->binding_ready && !next_generic_binding(state))
            return ExecClearTuple(node->ss_ScanTupleSlot);
        if (next_generic_combination(state))
            return materialize_generic_combination(state);
    }
}

static TupleTableSlot *
access_age_generic_count_scan(ScanState *node)
{
    AgeGenericJoinState *state = (AgeGenericJoinState *)node;
    bool distinct_key;

    if (state->count_emitted)
        return ExecClearTuple(node->ss_ScanTupleSlot);

    distinct_key =
        state->consumer_kind == AGE_GENERIC_CONSUMER_COUNT_DISTINCT_KEY;
    state->count_executed = true;
    state->count_multiplicity_product_path_used = false;
    state->count_multiplicity_enumeration_used = false;
    state->count_multiplicity_small_bag_enumerator_used = false;
    state->count_multiplicity_fast_path =
        !distinct_key && !age_generic_uniqueness_may_reject(state);
    for (;;)
    {
        if (!state->binding_ready && !next_generic_binding(state))
            break;
        if (distinct_key)
        {
            if (next_generic_combination(state))
            {
                graphid key = state->bindings[state->distinct_variable];

                if (age_generic_record_distinct_key(state, key))
                {
                    state->count_result =
                        add_generic_count_result(state->count_result, 1);
                    increment_generic_counter(&state->distinct_key_count);
                }
                increment_generic_counter(
                    &state->consumer_flat_rows_avoided);
                state->binding_ready = false;
                state->combination_started = false;
            }
        }
        else if (state->count_multiplicity_fast_path)
        {
            int64 binding_count = count_age_generic_binding_product(state);

            state->count_multiplicity_product_path_used = true;
            state->count_result =
                add_generic_count_result(state->count_result,
                                         binding_count);
            add_generic_counter(&state->candidate_combinations,
                                binding_count);
            add_generic_counter(&state->consumer_flat_rows_avoided,
                                binding_count);
            increment_generic_counter(
                &state->count_multiplicity_bindings);
            add_generic_counter(&state->count_multiplicity_rows,
                                binding_count);
            state->binding_ready = false;
            state->combination_started = false;
        }
        else
        {
            bool uniqueness_may_reject =
                age_generic_uniqueness_may_reject(state);

            if (age_generic_count_binding_product_safe(
                    state, uniqueness_may_reject))
            {
                int64 binding_count = count_age_generic_binding_product(
                    state);

                state->count_multiplicity_product_path_used = true;
                state->count_result =
                    add_generic_count_result(state->count_result,
                                             binding_count);
                add_generic_counter(&state->candidate_combinations,
                                    binding_count);
                add_generic_counter(&state->consumer_flat_rows_avoided,
                                    binding_count);
                increment_generic_counter(
                    &state->count_multiplicity_bindings);
                add_generic_counter(&state->count_multiplicity_rows,
                                    binding_count);
                state->binding_ready = false;
                state->combination_started = false;
            }
            else
            {
                int64 enumerated_rows;

                if (count_age_generic_small_binding(state, &enumerated_rows))
                {
                    state->count_multiplicity_enumeration_used = true;
                    state->count_multiplicity_small_bag_enumerator_used =
                        true;
                    state->count_result =
                        add_generic_count_result(state->count_result,
                                                 enumerated_rows);
                    increment_generic_counter(
                        &state->count_multiplicity_bindings);
                    add_generic_counter(&state->count_multiplicity_rows,
                                        enumerated_rows);
                    continue;
                }

                state->count_multiplicity_enumeration_used = true;
                while (next_generic_combination(state))
                {
                    state->count_result =
                        add_generic_count_result(state->count_result, 1);
                    increment_generic_counter(
                        &state->consumer_flat_rows_avoided);
                }
            }
        }
    }

    state->count_emitted = true;
    return store_age_generic_count_result(node, state->count_result);
}

static TupleTableSlot *
access_age_generic_group_count_scan(ScanState *node)
{
    AgeGenericJoinState *state = (AgeGenericJoinState *)node;
    bool distinct_key =
        state->consumer_kind ==
        AGE_GENERIC_CONSUMER_GROUP_COUNT_DISTINCT_KEY;
    bool uniqueness_may_reject = age_generic_uniqueness_may_reject(state);

    state->count_executed = true;
    state->count_multiplicity_product_path_used = false;
    state->count_multiplicity_enumeration_used = false;
    state->count_multiplicity_small_bag_enumerator_used = false;
    state->count_multiplicity_fast_path =
        !distinct_key && !uniqueness_may_reject;

    for (;;)
    {
        graphid key;
        int64 group_count = 0;
        bool group_has_binding = false;

        if (!state->binding_ready && !next_generic_binding(state))
            return ExecClearTuple(node->ss_ScanTupleSlot);

        key = state->bindings[state->distinct_variable];
        for (;;)
        {
            int64 binding_count;

            if (!state->binding_ready)
                break;
            if (state->bindings[state->distinct_variable] != key)
                break;

            binding_count = count_age_generic_binding_product(state);
            if (distinct_key)
            {
                if (!uniqueness_may_reject)
                {
                    group_has_binding = true;
                    add_generic_counter(&state->consumer_flat_rows_avoided,
                                        binding_count);
                }
                else if (next_generic_combination(state))
                {
                    group_has_binding = true;
                    if (binding_count > 1)
                    {
                        add_generic_counter(
                            &state->consumer_flat_rows_avoided,
                            binding_count - 1);
                    }
                }
            }
            else if (state->count_multiplicity_fast_path)
            {
                group_count =
                    add_generic_count_result(group_count, binding_count);
                state->count_multiplicity_product_path_used = true;
                add_generic_counter(&state->candidate_combinations,
                                    binding_count);
                add_generic_counter(&state->consumer_flat_rows_avoided,
                                    binding_count);
                increment_generic_counter(
                    &state->count_multiplicity_bindings);
                add_generic_counter(&state->count_multiplicity_rows,
                                    binding_count);
            }
            else
            {
                if (age_generic_count_binding_product_safe(
                        state, uniqueness_may_reject))
                {
                    group_count =
                        add_generic_count_result(group_count, binding_count);
                    state->count_multiplicity_product_path_used = true;
                    add_generic_counter(&state->candidate_combinations,
                                        binding_count);
                    add_generic_counter(&state->consumer_flat_rows_avoided,
                                        binding_count);
                    increment_generic_counter(
                        &state->count_multiplicity_bindings);
                    add_generic_counter(&state->count_multiplicity_rows,
                                        binding_count);
                }
                else
                {
                    int64 enumerated_rows;

                    if (count_age_generic_small_binding(state,
                                                        &enumerated_rows))
                    {
                        group_count =
                            add_generic_count_result(group_count,
                                                     enumerated_rows);
                        state->count_multiplicity_enumeration_used = true;
                        state->count_multiplicity_small_bag_enumerator_used =
                            true;
                        increment_generic_counter(
                            &state->count_multiplicity_bindings);
                        add_generic_counter(
                            &state->count_multiplicity_rows,
                            enumerated_rows);
                    }
                    else
                    {
                        state->count_multiplicity_enumeration_used = true;
                        while (next_generic_combination(state))
                        {
                            group_count = add_generic_count_result(
                                group_count, 1);
                            increment_generic_counter(
                                &state->consumer_flat_rows_avoided);
                        }
                    }
                }
            }

            state->binding_ready = false;
            state->combination_started = false;
            if (!next_generic_binding(state))
                break;
        }

        if (distinct_key)
            group_count = group_has_binding ? 1 : 0;
        if (group_count <= 0)
            continue;

        state->count_result =
            add_generic_count_result(state->count_result, group_count);
        if (distinct_key)
            increment_generic_counter(&state->distinct_key_count);
        return store_age_generic_group_count_result(node, key, group_count);
    }
}

static TupleTableSlot *
access_age_generic_distinct_key_scan(ScanState *node)
{
    AgeGenericJoinState *state = (AgeGenericJoinState *)node;

    for (;;)
    {
        if (!state->binding_ready && !next_generic_binding(state))
            return ExecClearTuple(node->ss_ScanTupleSlot);
        if (next_generic_combination(state))
        {
            graphid key = state->bindings[state->distinct_variable];

            increment_generic_counter(&state->consumer_flat_rows_avoided);
            state->binding_ready = false;
            state->combination_started = false;
            if (age_generic_record_distinct_key(state, key))
            {
                increment_generic_counter(&state->distinct_key_count);
                return store_age_generic_distinct_key_result(node, key);
            }
        }
    }
}

static TupleTableSlot *
access_age_generic_exists_scan(ScanState *node)
{
    AgeGenericJoinState *state = (AgeGenericJoinState *)node;

    state->exists_executed = true;
    if (state->exists_emitted ||
        (state->row_goal > 0 && state->row_goal_emitted >= state->row_goal))
    {
        return ExecClearTuple(node->ss_ScanTupleSlot);
    }

    for (;;)
    {
        if (!state->binding_ready && !next_generic_binding(state))
            return ExecClearTuple(node->ss_ScanTupleSlot);
        if (next_generic_combination(state))
        {
            state->exists_emitted = true;
            increment_generic_counter(&state->consumer_flat_rows_avoided);
            return store_age_generic_exists_result(node);
        }
    }
}

static TupleTableSlot *
exec_age_generic_join(CustomScanState *node)
{
    AgeGenericJoinState *state = (AgeGenericJoinState *)node;
    TupleTableSlot *slot;

    if (state->consumer_kind == AGE_GENERIC_CONSUMER_EXISTS)
    {
        slot = ExecScan(&node->ss, access_age_generic_exists_scan,
                        recheck_age_generic_join);
    }
    else if (state->consumer_kind == AGE_GENERIC_CONSUMER_COUNT ||
             state->consumer_kind ==
             AGE_GENERIC_CONSUMER_COUNT_DISTINCT_KEY)
    {
        slot = ExecScan(&node->ss, access_age_generic_count_scan,
                        recheck_age_generic_join);
    }
    else if (state->consumer_kind == AGE_GENERIC_CONSUMER_GROUP_COUNT ||
             state->consumer_kind ==
             AGE_GENERIC_CONSUMER_GROUP_COUNT_DISTINCT_KEY)
    {
        slot = ExecScan(&node->ss, access_age_generic_group_count_scan,
                        recheck_age_generic_join);
    }
    else if (state->consumer_kind == AGE_GENERIC_CONSUMER_DISTINCT_KEY)
    {
        slot = ExecScan(&node->ss, access_age_generic_distinct_key_scan,
                        recheck_age_generic_join);
    }
    else if (age_generic_group_property_aggregate_consumer(
                 state->consumer_kind))
    {
        slot = ExecScan(&node->ss, access_age_generic_group_sum_property_scan,
                        recheck_age_generic_join);
    }
    else if (age_generic_property_aggregate_consumer(state->consumer_kind))
    {
        slot = ExecScan(&node->ss, access_age_generic_sum_property_scan,
                        recheck_age_generic_join);
    }
    else
    {
        slot = ExecScan(&node->ss, access_age_generic_join,
                        recheck_age_generic_join);
    }
    if (!TupIsNull(slot))
    {
        increment_generic_counter(&state->rows_emitted);
        if (state->row_goal > 0 &&
            (state->consumer_kind == AGE_GENERIC_CONSUMER_LIMIT ||
             state->consumer_kind == AGE_GENERIC_CONSUMER_EXISTS))
        {
            increment_generic_counter(&state->row_goal_emitted);
        }
    }
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
    provider->properties_attno = (AttrNumber)intVal(list_nth(
        desc, AGE_GENERIC_PROVIDER_DESC_PROPERTIES_ATTNO));
    provider->physical_kind = (AgeGenericProviderPhysicalKind)intVal(list_nth(
        desc, AGE_GENERIC_PROVIDER_DESC_PHYSICAL_KIND));
    provider->adjacency_index_oid = generic_provider_desc_oid(
        desc, AGE_GENERIC_PROVIDER_DESC_ADJ_INDEX_OID);
    provider->adjacency_terminal_label_id = (int32)intVal(list_nth(
        desc, AGE_GENERIC_PROVIDER_DESC_ADJ_TERMINAL_LABEL_ID));
    provider->plan_state = plan_state;
    provider->trie_ops = &sorted_array_trie_ops;

    if ((provider->kind != AGE_GENERIC_PROVIDER_VERTEX &&
         provider->kind != AGE_GENERIC_PROVIDER_EDGE) ||
        provider->var1 < 0 || provider->var1 >= state->variable_count ||
        provider->key1_attno <= 0 || provider->key1_attno > tuple_desc->natts)
    {
        elog(ERROR, "invalid AGE Generic Join provider keys");
    }
    if (provider->properties_attno < 0 ||
        provider->properties_attno > tuple_desc->natts)
    {
        elog(ERROR, "invalid AGE Generic Join provider properties column");
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
    if (provider->physical_kind ==
        AGE_GENERIC_PROVIDER_PHYSICAL_ADJACENCY_EDGE)
    {
        if (provider->kind != AGE_GENERIC_PROVIDER_EDGE ||
            !OidIsValid(provider->adjacency_index_oid) ||
            !provider->key_only_output)
        {
            elog(ERROR, "invalid AGE Generic Join physical provider");
        }
        provider->payload_scan = age_adjacency_begin_visible_payload_scan(
            provider->adjacency_index_oid, estate->es_snapshot, false);
        age_adjacency_visible_payload_scan_set_terminal_label(
            provider->payload_scan, provider->adjacency_terminal_label_id);
        state->has_lazy_physical_provider = true;
        state->lazy_physical_provider_count++;
    }
    else if (provider->physical_kind !=
             AGE_GENERIC_PROVIDER_PHYSICAL_EAGER_SORTED_ARRAY)
    {
        elog(ERROR, "invalid AGE Generic Join physical provider kind %d",
             provider->physical_kind);
    }
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
    age_binding_init_flat_enumerator(&state->small_bag_enumerator,
                                     state->provider_count,
                                     estate->es_query_cxt);
    state->small_bag_row_multiplicities = NULL;
    state->small_bag_row_multiplicity_capacity = 0;
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
        if (age_generic_property_aggregate_consumer(state->consumer_kind) &&
            provider_index == state->property_aggregate_provider_index)
        {
            if (provider->kind != AGE_GENERIC_PROVIDER_EDGE ||
                provider->properties_attno <= 0 ||
                provider->properties_attno !=
                    state->property_aggregate_attno ||
                provider->tuple_slot == NULL)
            {
                elog(ERROR,
                     "invalid AGE Generic Join property aggregate provider");
            }
        }
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
        provider->prefix_rows = NULL;
        provider->prefix_row_count = 0;
        provider->prefix_row_capacity = 0;
        provider->prefix_valid = false;
        provider->bag_start = 0;
        provider->bag_count = 0;
        provider->prefix_ranges = NULL;
        invalidate_generic_provider_prefix_ranges(provider);
        if (provider->payload_scan != NULL)
            age_adjacency_visible_payload_scan_reset_runtime(
                provider->payload_scan);
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
    age_binding_reset_flat_enumerator(&state->small_bag_enumerator);
    if (state->small_bag_row_multiplicities != NULL)
    {
        memset(state->small_bag_row_multiplicities, 0,
               sizeof(int64) * state->small_bag_row_multiplicity_capacity);
    }
    state->count_emitted = false;
    state->count_executed = false;
    state->count_result = 0;
    state->count_multiplicity_fast_path = false;
    state->count_multiplicity_product_path_used = false;
    state->count_multiplicity_enumeration_used = false;
    state->count_multiplicity_small_bag_enumerator_used = false;
    state->count_multiplicity_bindings = 0;
    state->count_multiplicity_rows = 0;
    state->small_bag_enumerator_used = false;
    state->small_bag_enumerator_bindings = 0;
    state->small_bag_enumerator_candidate_rows = 0;
    state->small_bag_enumerator_rows = 0;
    state->distinct_key_hash = NULL;
    state->distinct_key_count = 0;
    reset_age_generic_property_aggregate_state(state);
    state->exists_emitted = false;
    state->exists_executed = false;
    state->row_goal_emitted = 0;
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
        if (state->providers[provider_index].payload_scan != NULL)
        {
            age_adjacency_end_visible_payload_scan(
                state->providers[provider_index].payload_scan);
            state->providers[provider_index].payload_scan = NULL;
        }
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

static const char *
generic_ghd_bag_kind_name(int kind)
{
    switch (kind)
    {
    case AGE_GENERIC_GHD_BAG_CYCLIC_CORE:
        return "cyclic-core";
    case AGE_GENERIC_GHD_BAG_LEAF_TAIL:
        return "leaf-tail";
    }
    return "unknown";
}

static void
append_generic_int_array(StringInfo buffer, const int *values, int count,
                         int display_offset)
{
    int index;

    for (index = 0; index < count; index++)
    {
        appendStringInfo(buffer, "%s%d", index == 0 ? "" : ",",
                         values[index] + display_offset);
    }
}

static char *
format_generic_ghd_bags(AgeGenericJoinState *state)
{
    StringInfoData buffer;
    int bag_index;

    initStringInfo(&buffer);
    if (state->reduction_ghd_bags == NULL ||
        state->reduction_ghd_bag_count <= 0)
    {
        appendStringInfoString(&buffer, "none");
        return buffer.data;
    }

    for (bag_index = 0; bag_index < state->reduction_ghd_bag_count;
         bag_index++)
    {
        AgeGenericGHDBagPlan *bag = &state->reduction_ghd_bags[bag_index];

        appendStringInfo(&buffer, "%sbag %d %s: vars ",
                         bag_index == 0 ? "" : "; ", bag->id + 1,
                         generic_ghd_bag_kind_name(bag->kind));
        append_generic_int_array(&buffer, bag->variables,
                                 bag->variable_count, 1);
        appendStringInfoString(&buffer, " providers ");
        append_generic_int_array(&buffer, bag->provider_indexes,
                                 bag->provider_count, 1);
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
        if (separator->kind == AGE_GENERIC_GHD_SEPARATOR_PAIR)
        {
            appendStringInfo(&buffer, "%sbag %d->%d pair v%d,v%d",
                             separator_index == 0 ? "" : ", ",
                             separator->parent_bag_id + 1,
                             separator->child_bag_id + 1,
                             separator->separator_variables[0] + 1,
                             separator->separator_variables[1] + 1);
        }
        else
        {
            appendStringInfo(&buffer,
                             "%sbag %d->%d provider %d: v%d->v%d via %s",
                             separator_index == 0 ? "" : ", ",
                             separator->parent_bag_id + 1,
                             separator->child_bag_id + 1,
                             separator->provider_index + 1,
                             separator->separator_variable + 1,
                             separator->tail_variable + 1,
                             separator->separator_is_key1 ? "key1" : "key2");
        }
    }

    return buffer.data;
}

static const char *
generic_semijoin_step_phase_name(AgeGenericSemijoinStepPhase phase)
{
    switch (phase)
    {
    case AGE_GENERIC_SEMIJOIN_STEP_BOTTOM_UP:
        return "bottom-up";
    case AGE_GENERIC_SEMIJOIN_STEP_TOP_DOWN:
        return "top-down";
    }
    return "unknown";
}

static char *
format_generic_semijoin_steps(AgeGenericJoinState *state)
{
    StringInfoData buffer;
    int phase;
    bool printed_any = false;

    initStringInfo(&buffer);
    if (state->reduction_semijoin_steps == NULL ||
        state->reduction_semijoin_step_count <= 0)
    {
        appendStringInfoString(&buffer, "none");
        return buffer.data;
    }

    for (phase = AGE_GENERIC_SEMIJOIN_STEP_BOTTOM_UP;
         phase <= AGE_GENERIC_SEMIJOIN_STEP_TOP_DOWN;
         phase++)
    {
        int step_index;
        bool printed_phase = false;

        if (printed_any)
            appendStringInfoString(&buffer, "; ");
        appendStringInfo(&buffer, "%s:",
                         generic_semijoin_step_phase_name(phase));
        printed_any = true;

        for (step_index = 0;
             step_index < state->reduction_semijoin_step_count;
             step_index++)
        {
            AgeGenericSemijoinStepPlan *step =
                &state->reduction_semijoin_steps[step_index];

            if (step->phase != phase)
                continue;
            appendStringInfo(
                &buffer, "%s provider %d v%d->v%d on v%d(%s)",
                printed_phase ? "," : "",
                step->provider_index + 1,
                step->from_variable + 1,
                step->to_variable + 1,
                step->key_variable + 1,
                step->key_is_key1 ? "key1" : "key2");
            printed_phase = true;
        }
        if (!printed_phase)
            appendStringInfoString(&buffer, " none");
    }

    return buffer.data;
}

static const char *
generic_consumer_name(AgeGenericConsumerKind consumer)
{
    switch (consumer)
    {
    case AGE_GENERIC_CONSUMER_ROWS:
        return "rows";
    case AGE_GENERIC_CONSUMER_COUNT:
        return "count(*)";
    case AGE_GENERIC_CONSUMER_COUNT_DISTINCT_KEY:
        return "count(distinct key)";
    case AGE_GENERIC_CONSUMER_DISTINCT_KEY:
        return "distinct key";
    case AGE_GENERIC_CONSUMER_GROUP_COUNT:
        return "group count(key)";
    case AGE_GENERIC_CONSUMER_GROUP_COUNT_DISTINCT_KEY:
        return "group count(distinct key)";
    case AGE_GENERIC_CONSUMER_SUM_PROPERTY:
        return "sum(property)";
    case AGE_GENERIC_CONSUMER_MIN_PROPERTY:
        return "min(property)";
    case AGE_GENERIC_CONSUMER_MAX_PROPERTY:
        return "max(property)";
    case AGE_GENERIC_CONSUMER_AVG_PROPERTY:
        return "avg(property)";
    case AGE_GENERIC_CONSUMER_GROUP_SUM_PROPERTY:
        return "group sum(property)";
    case AGE_GENERIC_CONSUMER_GROUP_MIN_PROPERTY:
        return "group min(property)";
    case AGE_GENERIC_CONSUMER_GROUP_MAX_PROPERTY:
        return "group max(property)";
    case AGE_GENERIC_CONSUMER_GROUP_AVG_PROPERTY:
        return "group avg(property)";
    case AGE_GENERIC_CONSUMER_EXISTS:
        return "exists";
    case AGE_GENERIC_CONSUMER_LIMIT:
        return "limit";
    }
    return "unknown";
}

static const char *
generic_row_goal_source_name(AgeGenericRowGoalSource source)
{
    switch (source)
    {
    case AGE_GENERIC_ROW_GOAL_NONE:
        return "none";
    case AGE_GENERIC_ROW_GOAL_LIMIT:
        return "limit";
    case AGE_GENERIC_ROW_GOAL_EXISTS:
        return "exists";
    }
    return "unknown";
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
generic_ghd_mode_name(AgeGenericJoinState *state)
{
    switch (state->reduction_shape)
    {
    case AGE_GENERIC_REDUCTION_ALPHA_ACYCLIC:
        return "none";
    case AGE_GENERIC_REDUCTION_CYCLIC_CORE:
        if (state->reduction_ghd_bag_count > 0)
            return "general GHD";
        return "cyclic core only";
    case AGE_GENERIC_REDUCTION_CYCLIC_WITH_TAIL:
        if (state->reduction_ghd_bag_count > 0)
            return "general GHD";
        return "2-core leaf-tail";
    }
    return "unknown";
}

static bool
generic_ghd_general_decomposition(AgeGenericJoinState *state)
{
    return state->reduction_shape != AGE_GENERIC_REDUCTION_ALPHA_ACYCLIC &&
        state->reduction_ghd_bag_count > 0 &&
        state->reduction_ghd_bags != NULL;
}

static const char *
generic_ghd_fallback_reason(AgeGenericJoinState *state)
{
    if (generic_ghd_general_decomposition(state))
        return "none";

    switch (state->reduction_shape)
    {
    case AGE_GENERIC_REDUCTION_ALPHA_ACYCLIC:
        return "alpha-acyclic leaf-peel reduction";
    case AGE_GENERIC_REDUCTION_CYCLIC_CORE:
        return "no leaf-tail separators";
    case AGE_GENERIC_REDUCTION_CYCLIC_WITH_TAIL:
        return "general GHD decomposition is not implemented";
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
    char *ghd_bags = format_generic_ghd_bags(state);
    char *ghd_separators = format_generic_ghd_separators(state);
    char *semijoin_steps = format_generic_semijoin_steps(state);

    (void)ancestors;
    ExplainPropertyText("Generic Join Algorithm",
                        "lazy domain views with vectorized intersections and delayed edge bags",
                        es);
    ExplainPropertyText("Generic Join Consumer",
                        generic_consumer_name(state->consumer_kind), es);
    ExplainPropertyInteger("Generic Join Row Goal", NULL,
                           state->row_goal, es);
    ExplainPropertyText("Generic Join Row Goal Source",
                        generic_row_goal_source_name(
                            state->row_goal_source), es);
    ExplainPropertyText(
        "Generic Join Provider Mode",
        state->has_lazy_physical_provider ?
        "lazy adjacency edge provider + eager sorted array" :
        "eager sorted array",
        es);
    ExplainPropertyBool("Lazy Physical Provider",
                        state->has_lazy_physical_provider, es);
    ExplainPropertyText(
        "Provider Trie Ops",
        state->has_lazy_physical_provider ?
        "adjacency prefix scratch with sorted-array fallback" :
        sorted_array_trie_ops.name,
        es);
    ExplainPropertyText("Variable Order", variable_order, es);
    ExplainPropertyInteger("Component Count", NULL, state->component_count,
                           es);
    ExplainPropertyText("Component IDs", component_ids, es);
    ExplainPropertyText("Reduction Shape",
                        generic_reduction_shape_name(state->reduction_shape),
                        es);
    ExplainPropertyText("GHD Mode", generic_ghd_mode_name(state), es);
    ExplainPropertyBool("GHD General Decomposition",
                        generic_ghd_general_decomposition(state), es);
    ExplainPropertyText("GHD Fallback Reason",
                        generic_ghd_fallback_reason(state), es);
    ExplainPropertyText(
        "Reduction Descriptor Source",
        generic_reduction_descriptor_source_name(
            state->reduction_descriptor_source), es);
    ExplainPropertyText(
        "Reduction Order",
        generic_reduction_order_name(state->reduction_order_kind), es);
    ExplainPropertyInteger("Reduction Order Edges", NULL,
                           state->reduction_order_edge_count, es);
    ExplainPropertyInteger("Yannakakis Step Count", NULL,
                           state->reduction_semijoin_step_count, es);
    ExplainPropertyInteger("Yannakakis Bottom-Up Steps", NULL,
                           state->reduction_semijoin_bottom_up_step_count,
                           es);
    ExplainPropertyInteger("Yannakakis Top-Down Steps", NULL,
                           state->reduction_semijoin_top_down_step_count, es);
    ExplainPropertyText("Yannakakis Step Plan", semijoin_steps, es);
    ExplainPropertyText("Yannakakis Step Filter Mode",
                        "global-domain provider filter", es);
    ExplainPropertyInteger("Reduction Core Variables", NULL,
                           state->reduction_core_variables, es);
    ExplainPropertyInteger("Reduction Tail Separators", NULL,
                           state->reduction_tail_separators, es);
    ExplainPropertyInteger("GHD Bag Count", NULL,
                           state->reduction_ghd_bag_count, es);
    ExplainPropertyText("GHD Bags", ghd_bags, es);
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
        ExplainPropertyBool("Provider Full Materialization",
                            state->materialized &&
                            !state->has_lazy_physical_provider, es);
        ExplainPropertyInteger("Provider Rows Read", NULL,
                               state->provider_rows_read, es);
        ExplainPropertyInteger("Provider Row Bytes Allocated", "bytes",
                               state->provider_row_bytes_allocated, es);
        ExplainPropertyInteger("Provider Tuple Bytes Copied", "bytes",
                               state->provider_tuple_bytes_copied, es);
        if (state->has_lazy_physical_provider)
        {
            ExplainPropertyInteger("Lazy Physical Providers", NULL,
                                   state->lazy_physical_provider_count, es);
            ExplainPropertyInteger("Lazy Physical Prefix Loads", NULL,
                                   state->lazy_physical_prefix_loads, es);
            ExplainPropertyInteger("Lazy Physical Rows Read", NULL,
                                   state->lazy_physical_rows_read, es);
            ExplainPropertyInteger("Lazy Physical Row Bytes Allocated", "bytes",
                                   state->lazy_physical_row_bytes_allocated,
                                   es);
        }
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
        ExplainPropertyInteger("Yannakakis Steps Applied", NULL,
                               state->semijoin_steps_applied, es);
        ExplainPropertyInteger("Yannakakis Bottom-Up Steps Applied", NULL,
                               state->semijoin_bottom_up_steps_applied, es);
        ExplainPropertyInteger("Yannakakis Top-Down Steps Applied", NULL,
                               state->semijoin_top_down_steps_applied, es);
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
        ExplainPropertyInteger("Reduction Domain Builds", NULL,
                               state->reduction_domain_builds, es);
        ExplainPropertyInteger("Reduction Domain Rows Scanned", NULL,
                               state->reduction_domain_rows_scanned, es);
        ExplainPropertyInteger("Reduction Domain Keys Produced", NULL,
                               state->reduction_domain_keys_produced, es);
        ExplainPropertyInteger("Reduction Domain Sorts", NULL,
                               state->reduction_domain_sorts, es);
        ExplainPropertyInteger("Reduction Domain Sort Rows", NULL,
                               state->reduction_domain_sort_rows, es);
        ExplainPropertyInteger("Complete Bindings", NULL,
                               state->bindings_completed, es);
        ExplainPropertyInteger("Candidate Flat Rows", NULL,
                               state->candidate_flat_rows, es);
        ExplainPropertyInteger("Candidate Bag Combinations", NULL,
                               state->candidate_combinations, es);
        ExplainPropertyInteger("Flat Rows Materialized", NULL,
                               state->flat_rows_materialized, es);
        ExplainPropertyInteger("Flat Rows Avoided", NULL,
                               flat_rows_avoided, es);
        ExplainPropertyInteger("Consumer Flat Rows Avoided", NULL,
                               state->consumer_flat_rows_avoided, es);
        if (state->small_bag_enumerator_used)
        {
            ExplainPropertyBool("Generic Small Bag Enumerator", true, es);
            ExplainPropertyInteger("Generic Small Bag Enumerator Limit",
                                   NULL,
                                   AGE_GENERIC_SMALL_BAG_ENUMERATOR_LIMIT,
                                   es);
            ExplainPropertyInteger(
                "Generic Small Bag Enumerator Bindings", NULL,
                state->small_bag_enumerator_bindings, es);
            ExplainPropertyInteger(
                "Generic Small Bag Enumerator Candidate Rows", NULL,
                state->small_bag_enumerator_candidate_rows, es);
            ExplainPropertyInteger(
                "Generic Small Bag Enumerator Rows", NULL,
                state->small_bag_enumerator_rows, es);
        }
        if (state->row_goal > 0)
        {
            ExplainPropertyInteger("Row Goal Rows Emitted", NULL,
                                   state->row_goal_emitted, es);
            ExplainPropertyInteger("Row Goal Flat Rows Avoided", NULL,
                                   flat_rows_avoided, es);
        }
        ExplainPropertyBool("Row Goal Reached",
                            state->row_goal > 0 &&
                            state->row_goal_emitted >= state->row_goal, es);
        if ((state->consumer_kind == AGE_GENERIC_CONSUMER_COUNT ||
             state->consumer_kind == AGE_GENERIC_CONSUMER_GROUP_COUNT) &&
            state->count_executed)
        {
            const char *mode;
            const char *reason;
            bool small_bag_used =
                state->count_multiplicity_small_bag_enumerator_used ||
                state->small_bag_enumerator_used;

            if (state->count_multiplicity_fast_path)
            {
                mode = "binding-bag-product";
                reason = "no overlapping uniqueness groups";
            }
            else if (state->count_multiplicity_product_path_used &&
                     !state->count_multiplicity_enumeration_used)
            {
                mode = "exact-bag-product";
                reason = "active edge bags cannot collide";
            }
            else if (state->count_multiplicity_product_path_used &&
                     small_bag_used)
            {
                mode = "mixed product/small-bag-exact-enumerator";
                reason = "bounded active bags used where uniqueness "
                         "overlapped";
            }
            else if (state->count_multiplicity_product_path_used)
            {
                mode = "mixed product/enumerate";
                reason = "edge uniqueness checked per binding";
            }
            else if (small_bag_used)
            {
                mode = "small-bag-exact-enumerator";
                reason = "bounded active bags with overlapping uniqueness";
            }
            else
            {
                mode = "enumerate-combinations";
                reason = "edge uniqueness may reject";
            }

            ExplainPropertyText(
                "Generic Count Multiplicity Mode", mode, es);
            ExplainPropertyText(
                "Generic Count Multiplicity Reason", reason, es);
            ExplainPropertyInteger("Generic Count Multiplicity Bindings",
                                   NULL,
                                   state->count_multiplicity_bindings, es);
            ExplainPropertyInteger("Generic Count Multiplicity Rows", NULL,
                                   state->count_multiplicity_rows, es);
            ExplainPropertyInteger("Count Result", NULL,
                                   state->count_result, es);
        }
        if ((state->consumer_kind ==
             AGE_GENERIC_CONSUMER_COUNT_DISTINCT_KEY ||
             state->consumer_kind ==
             AGE_GENERIC_CONSUMER_GROUP_COUNT_DISTINCT_KEY) &&
            state->count_executed)
        {
            ExplainPropertyInteger("Count Result", NULL,
                                   state->count_result, es);
            ExplainPropertyInteger("Distinct Key Count", NULL,
                                   state->distinct_key_count, es);
        }
        if (state->consumer_kind == AGE_GENERIC_CONSUMER_DISTINCT_KEY)
        {
            ExplainPropertyInteger("Distinct Key Count", NULL,
                                   state->distinct_key_count, es);
        }
        if (age_generic_property_aggregate_consumer(state->consumer_kind) &&
            state->property_aggregate_executed)
        {
            const char *prefix = "Sum";
            char *provider_label;
            char *input_label;
            char *null_label;

            if (state->consumer_kind == AGE_GENERIC_CONSUMER_MIN_PROPERTY ||
                state->consumer_kind ==
                AGE_GENERIC_CONSUMER_GROUP_MIN_PROPERTY)
                prefix = "Min";
            else if (state->consumer_kind ==
                     AGE_GENERIC_CONSUMER_MAX_PROPERTY ||
                     state->consumer_kind ==
                     AGE_GENERIC_CONSUMER_GROUP_MAX_PROPERTY)
                prefix = "Max";
            else if (state->consumer_kind ==
                     AGE_GENERIC_CONSUMER_AVG_PROPERTY ||
                     state->consumer_kind ==
                     AGE_GENERIC_CONSUMER_GROUP_AVG_PROPERTY)
                prefix = "Avg";

            provider_label = psprintf("%s Property Provider", prefix);
            input_label = psprintf("%s Property Input Rows", prefix);
            null_label = psprintf("%s Property Null Rows", prefix);
            ExplainPropertyInteger(
                provider_label, NULL,
                state->property_aggregate_provider_index + 1, es);
            ExplainPropertyInteger(input_label, NULL,
                                   state->property_aggregate_input_rows, es);
            ExplainPropertyInteger(null_label, NULL,
                                   state->property_aggregate_null_rows, es);
            pfree(provider_label);
            pfree(input_label);
            pfree(null_label);
        }
        if (state->consumer_kind == AGE_GENERIC_CONSUMER_EXISTS &&
            state->exists_executed)
        {
            ExplainPropertyBool("Exists Result", state->exists_emitted, es);
        }
        ExplainPropertyInteger("Lazy Domain Views", NULL,
                               state->lazy_domain_views, es);
        ExplainPropertyInteger("Lazy Domain Keys Scanned", NULL,
                               state->lazy_domain_keys_scanned, es);
        ExplainPropertyInteger("Domain Scratch Allocations", NULL,
                               state->lazy_domain_scratch_allocations, es);
        ExplainPropertyInteger("Domain Scratch Reuses", NULL,
                               state->lazy_domain_scratch_reuses, es);
        ExplainPropertyInteger("Runtime Domain Builds", NULL,
                               state->runtime_domain_builds, es);
        ExplainPropertyInteger("Runtime Domain Rows Scanned", NULL,
                               state->runtime_domain_rows_scanned, es);
        ExplainPropertyInteger("Prefix Range Builds", NULL,
                               state->lazy_prefix_range_builds, es);
        ExplainPropertyInteger("Prefix Range Reuses", NULL,
                               state->lazy_prefix_range_reuses, es);
        ExplainPropertyInteger("Prefix Range Directory Searches", NULL,
                               state->lazy_prefix_range_directory_searches, es);
        ExplainPropertyInteger("Prefix Range Cursor Reuses", NULL,
                               state->lazy_prefix_range_cursor_reuses, es);
        ExplainPropertyInteger("Prefix Range Cache Hits", NULL,
                               state->prefix_range_cache_hits, es);
        ExplainPropertyInteger("Prefix Range Cache Misses", NULL,
                               state->prefix_range_cache_misses, es);
        ExplainPropertyInteger("Child Range Cache Hits", NULL,
                               state->child_range_cache_hits, es);
        ExplainPropertyInteger("Child Range Cache Misses", NULL,
                               state->child_range_cache_misses, es);
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
    pfree(ghd_bags);
    pfree(ghd_separators);
    pfree(semijoin_steps);
}
