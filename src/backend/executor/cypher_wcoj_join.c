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
#include "catalog/ag_label.h"
#include "catalog/pg_type_d.h"
#include "commands/explain_format.h"
#include "commands/explain_state.h"
#include "common/int.h"
#include "executor/cypher_factorized_binding.h"
#include "executor/cypher_wcoj_join.h"
#include "executor/executor.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "nodes/pg_list.h"
#include "storage/buffile.h"
#include "utils/ag_guc.h"
#include "utils/agtype.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/graphid.h"
#include "utils/memutils.h"
#include "utils/numeric.h"

#define AGE_WCOJ_PROGRESSIVE_DENSE_SAMPLE 1024
#define AGE_WCOJ_PROGRESSIVE_DENSE_NUMERATOR 3
#define AGE_WCOJ_PROGRESSIVE_DENSE_DENOMINATOR 4
#define AGE_WCOJ_SURVIVOR_BLOCK_MIN 16
#define AGE_WCOJ_SURVIVOR_BLOCK_MAX 4096
#define AGE_WCOJ_SURVIVOR_BLOCK_BUDGET_DIVISOR 4
#define AGE_WCOJ_GALLOPING_MIN_RATIO 8

typedef struct AgeWCOJJoinScanState AgeWCOJJoinScanState;
typedef struct AgeWCOJPostingProvider AgeWCOJPostingProvider;
typedef struct AgeWCOJBatchPayload AgeWCOJBatchPayload;
typedef struct AgeWCOJPayloadBucketSpill AgeWCOJPayloadBucketSpill;

typedef enum AgeWCOJTupleGroupKind
{
    AGE_WCOJ_TUPLE_GROUP_ROWS = 0,
    AGE_WCOJ_TUPLE_GROUP_ADJACENCY
} AgeWCOJTupleGroupKind;

/*
 * One duplicate group per input stream.  Plan stream rows are copied because
 * every child PlanState reuses its result slot on the next ExecProcNode()
 * call.  Direct adjacency groups keep payload/source-bag references and expose
 * their source-row x payload product through count.
 */
typedef struct AgeWCOJTupleGroup
{
    AgeWCOJTupleGroupKind kind;
    MinimalTuple *tuples;
    graphid *edge_ids;
    bool *edge_id_valid;
    AgeWCOJPostingProvider *adjacency_provider;
    AgeWCOJBatchPayload *adjacency_payloads;
    int adjacency_payload_count;
    int adjacency_payload_capacity;
    int count;
    int capacity;
} AgeWCOJTupleGroup;

struct AgeWCOJBatchPayload
{
    AgeAdjacencyPayload payload;
    int survivor_index;
    int source_bag_index;
    int first_tuple;
    bool properties_spilled;
    int properties_spill_fileno;
    off_t properties_spill_offset;
    int32 properties_spill_size;
};

struct AgeWCOJPayloadBucketSpill
{
    bool spilled;
    int spill_fileno;
    off_t spill_offset;
    int32 spill_size;
    int payload_count;
    int logical_count;
};

typedef struct AgeWCOJPayloadRunFilterState
{
    AgeWCOJJoinScanState *state;
    int64 run_postings;
    int32 terminal_label_id;
} AgeWCOJPayloadRunFilterState;

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
    Index source_rti;
    int source_correlation_group;
    int *output_map;
    int output_width;
    int output_offset;
    AttrNumber edge_id_output_attno;
    ExprState *local_qual;
    TupleTableSlot *source_slot;
    TupleTableSlot *output_slot;
    MemoryContext provider_context;
    AgeBindingSourceRow *source_rows;
    int source_row_count;
    int source_row_capacity;
    AgeSourceBag *source_bags;
    int source_bag_count;
    int64 source_bag_bytes;
    graphid *terminals;
    int terminal_count;
    int terminal_capacity;
    int64 terminal_bytes;
    int terminal_index;
    AgeAdjacencyVisiblePayloadScan *terminal_scan;
    AgeAdjacencyVisiblePayloadScan *payload_scan;
    BufFile *payload_spill_file;
    AgeWCOJTupleGroup *batch_groups;
    AgeWCOJBatchPayload *batch_payloads;
    AgeEdgeBag *batch_payload_ranges;
    AgeWCOJPayloadBucketSpill *batch_payload_bucket_spills;
    int batch_payload_count;
    int batch_payload_capacity;
    double observed_payloads_per_survivor;
    int64 peak_batch_payload_count;
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
    int source_correlation_group_count;
    AgeWCOJPostingProvider *providers;
    TupleTableSlot **group_slots;
    AgeWCOJTupleGroup *groups;
    AgeBindingNode *active_binding_node;
    AgeBindingFlatEnumerator flat_enumerator;
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
    AgeWCOJConsumerKind consumer_kind;
    AgeWCOJRowGoalSource row_goal_source;
    Oid consumer_output_type;
    int64 row_goal;
    int64 row_goal_emitted;
    const char *fallback_reason;
    int seed_index;
    graphid active_group_key;
    bool active_group_key_valid;
    bool group_active;
    bool exhausted;
    bool count_emitted;
    bool count_executed;
    bool exists_emitted;
    bool exists_executed;
    int sum_property_provider_index;
    Datum sum_property_key;
    bool sum_property_key_isnull;
    bool sum_emitted;
    bool sum_executed;
    bool sum_has_value;
    agtype_value sum_value;
    agtype *property_aggregate_value;
    float8 avg_sum;
    bool batch_payload_enabled;
    bool survivor_input_exhausted;
    int64 groups_matched;
    int64 count_result;
    int64 sum_input_rows;
    int64 sum_null_rows;
    int64 candidate_flat_rows;
    int64 candidate_combinations;
    int64 flat_rows_materialized;
    int64 consumer_flat_rows_avoided;
    int64 active_binding_node_flat_rows_base;
    int64 binding_nodes_created;
    int64 binding_node_source_bags;
    int64 binding_node_edge_bags;
    int64 binding_node_materialized_flat_rows;
    int64 binding_node_flat_rows_avoided;
    int64 source_correlation_rejects;
    int64 peak_binding_node_memory;
    int64 cursor_advances;
    int64 seek_calls;
    int64 posting_rows_scanned;
    int64 payload_rows_scanned;
    int64 payload_main_rows_scanned;
    int64 payload_delta_rows_scanned;
    int64 payload_rows_after_survivor_filter;
    int64 payload_rows_matched;
    int64 payload_rows_fetched;
    int64 payload_scan_batches;
    int64 payload_scan_restarts;
    int64 payload_source_keys_scanned;
    int64 payload_scan_restarts_avoided;
    int64 survivor_blocks;
    int64 rows_emitted;
    int64 rescans;
    int64 progressive_probes;
    int64 progressive_matches;
    int64 source_rows_scanned;
    int64 distinct_source_keys;
    int64 source_bag_rows;
    int64 source_bag_keys;
    int64 intersection_builds;
    int64 intersection_merge_calls;
    int64 intersection_galloping_calls;
    int64 intersection_galloping_steps;
    int64 local_predicate_rejects;
    int64 exact_set_filters;
    int64 exact_set_candidates_tested;
    int64 row_goal_block_clamps;
    int64 payload_block_budget_overruns;
    int64 payload_block_capacity_clamps;
    int64 peak_payload_block_rows;
    int64 spill_bytes;
    int64 payload_rows_spilled;
    int64 payload_bucket_blocks_spilled;
    int64 payload_bucket_rows_spilled;
    int64 payload_bucket_bytes_spilled;
    int64 source_bag_memory_reserve;
    int64 peak_source_bag_bytes;
    int64 peak_factor_memory;
    bool source_correlation_enforced;
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

static inline void
add_wcoj_byte_counter(int64 *counter, Size increment)
{
    if (increment == 0)
        return;
    if (increment > (Size)PG_INT64_MAX)
        *counter = PG_INT64_MAX;
    else
        add_wcoj_counter(counter, (int64)increment);
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
static int intersect_age_wcoj_graphids(AgeWCOJJoinScanState *state,
                                       graphid *candidates,
                                       int candidate_count,
                                       const graphid *matches,
                                       int match_count);
static int age_wcoj_galloping_lower_bound(AgeWCOJJoinScanState *state,
                                          const graphid *values,
                                          int value_count, int start_index,
                                          graphid target);
static void rescan_age_wcoj_providers(AgeWCOJJoinScanState *state);
static bool prepare_age_wcoj_progressive_exact_set(
    AgeWCOJJoinScanState *state);
static void update_age_wcoj_peak_memory(AgeWCOJJoinScanState *state);
static void update_age_wcoj_payload_block_budget(
    AgeWCOJJoinScanState *state);
static int64 current_age_wcoj_source_bag_bytes(
    AgeWCOJJoinScanState *state);
static int64 current_age_wcoj_binding_node_memory(
    AgeWCOJJoinScanState *state);
static int64 current_age_wcoj_factor_memory(AgeWCOJJoinScanState *state);
static void reset_age_wcoj_survivor_block(AgeWCOJJoinScanState *state);
static bool age_wcoj_payload_run_filter(
    int64 run_postings, int64 active_postings,
    AgeAdjacencyCompositeTerminalFilter *filter, bool *known_empty,
    void *callback_state);
static int compare_age_wcoj_batch_payloads(const void *left,
                                           const void *right);
static void close_age_wcoj_payload_spill(AgeWCOJPostingProvider *provider);
static bool should_spill_age_wcoj_payload_property(
    AgeWCOJPostingProvider *provider, const AgeAdjacencyPayload *payload);
static void spill_age_wcoj_payload_property(
    AgeWCOJPostingProvider *provider, AgeWCOJBatchPayload *entry);
static void spill_age_wcoj_copied_payload_property(
    AgeWCOJPostingProvider *provider, AgeWCOJBatchPayload *entry);
static void restore_age_wcoj_payload_property(
    AgeWCOJPostingProvider *provider, AgeWCOJBatchPayload *entry,
    MemoryContext context);
static void spill_age_wcoj_payload_buckets(
    AgeWCOJPostingProvider *provider);
static AgeWCOJBatchPayload *load_age_wcoj_payload_bucket(
    AgeWCOJPostingProvider *provider, int survivor_index,
    const AgeEdgeBag *range, MemoryContext context);
static int find_age_wcoj_survivor_index(AgeWCOJJoinScanState *state,
                                        graphid terminal);
static void append_age_wcoj_batch_payload(
    AgeWCOJPostingProvider *provider, int survivor_index,
    int source_bag_index, const AgeAdjacencyPayload *payload);
static void reset_age_wcoj_tuple_group(AgeWCOJTupleGroup *group);
static void set_age_wcoj_adjacency_group(
    AgeWCOJTupleGroup *group, AgeWCOJPostingProvider *provider,
    AgeWCOJBatchPayload *payloads, int payload_count, int logical_count);
static void append_age_wcoj_adjacency_group_payload(
    AgeWCOJTupleGroup *group, MemoryContext tuple_context,
    AgeWCOJPostingProvider *provider, int source_bag_index,
    const AgeAdjacencyPayload *payload);
static AgeWCOJBatchPayload *get_age_wcoj_adjacency_group_payload(
    AgeWCOJTupleGroup *group, int tuple_index, int *source_row_index);
static bool get_age_wcoj_group_edge_id(
    AgeWCOJTupleGroup *group, int tuple_index, graphid *edge_id);
static int age_wcoj_group_logical_count(void *callback_state,
                                        int source_index);
static bool age_wcoj_group_edge_id(void *callback_state, int source_index,
                                   int tuple_index, graphid *edge_id);
static AgeBindingNode *build_age_wcoj_binding_node(
    AgeWCOJJoinScanState *state, graphid matched_key);
static void finish_age_wcoj_binding_node(AgeWCOJJoinScanState *state);
static TupleTableSlot *materialize_age_wcoj_group_entry(
    AgeWCOJJoinScanState *state, int child_index, int tuple_index);
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
static void fill_age_wcoj_adjacency_output(
    AgeWCOJPostingProvider *provider, int bag_index,
    int source_row_index, const AgeAdjacencyPayload *payload);
static bool age_wcoj_adjacency_local_qual(
    AgeWCOJPostingProvider *provider);
static bool age_wcoj_adjacency_payload_passes_local_qual(
    AgeWCOJPostingProvider *provider, int bag_index,
    const AgeAdjacencyPayload *payload);
static bool collect_age_wcoj_group(AgeWCOJJoinScanState *state,
                                   graphid matched_key);
static bool find_next_age_wcoj_survivor(AgeWCOJJoinScanState *state,
                                        graphid *matched_key);
static bool prepare_age_wcoj_survivor_block(AgeWCOJJoinScanState *state);
static bool prepare_age_wcoj_group(AgeWCOJJoinScanState *state);
static bool next_age_wcoj_combination(AgeWCOJJoinScanState *state);
static TupleTableSlot *materialize_age_wcoj_combination(
    AgeWCOJJoinScanState *state);
static bool age_wcoj_uniqueness_may_reject(AgeWCOJJoinScanState *state);
static int64 count_age_wcoj_group_product(AgeWCOJJoinScanState *state);
static int64 count_age_wcoj_active_group(AgeWCOJJoinScanState *state);
static int64 count_age_wcoj_rows(AgeWCOJJoinScanState *state);
static int64 count_age_wcoj_distinct_keys(AgeWCOJJoinScanState *state);
static TupleTableSlot *access_age_wcoj_distinct_key_scan(ScanState *node);
static TupleTableSlot *store_age_wcoj_distinct_key_result(ScanState *node,
                                                          graphid key);
static TupleTableSlot *access_age_wcoj_count_scan(ScanState *node);
static TupleTableSlot *store_age_wcoj_count_result(ScanState *node,
                                                   int64 count);
static TupleTableSlot *access_age_wcoj_group_count_scan(ScanState *node);
static TupleTableSlot *store_age_wcoj_group_count_result(ScanState *node,
                                                         graphid key,
                                                         int64 count);
static TupleTableSlot *access_age_wcoj_group_count_distinct_key_scan(
    ScanState *node);
static TupleTableSlot *access_age_wcoj_sum_property_scan(ScanState *node);
static TupleTableSlot *store_age_wcoj_sum_property_result(ScanState *node);
static TupleTableSlot *access_age_wcoj_group_sum_property_scan(ScanState *node);
static TupleTableSlot *store_age_wcoj_group_sum_property_result(ScanState *node,
                                                                graphid key);
static TupleTableSlot *access_age_wcoj_exists_scan(ScanState *node);
static TupleTableSlot *store_age_wcoj_exists_result(ScanState *node);
static void reset_age_wcoj_sum_value(AgeWCOJJoinScanState *state);
static void reset_age_wcoj_sum_state(AgeWCOJJoinScanState *state);
static bool age_wcoj_property_aggregate_consumer(
    AgeWCOJConsumerKind consumer);
static bool age_wcoj_property_minmax_consumer(AgeWCOJConsumerKind consumer);
static bool age_wcoj_property_avg_consumer(AgeWCOJConsumerKind consumer);
static bool age_wcoj_group_property_aggregate_consumer(
    AgeWCOJConsumerKind consumer);
static void add_age_wcoj_sum_value(AgeWCOJJoinScanState *state,
                                   agtype_value *value,
                                   int64 multiplicity);
static void add_age_wcoj_avg_value(AgeWCOJJoinScanState *state,
                                   agtype_value *value,
                                   int64 multiplicity);
static void add_age_wcoj_minmax_value(AgeWCOJJoinScanState *state,
                                      agtype_value *value);
static void add_age_wcoj_sum_payload(AgeWCOJJoinScanState *state,
                                     AgeWCOJPostingProvider *provider,
                                     AgeWCOJBatchPayload *entry,
                                     int64 multiplicity);
static void add_age_wcoj_sum_active_group(AgeWCOJJoinScanState *state);
static void add_age_wcoj_sum_rows(AgeWCOJJoinScanState *state);
static bool age_wcoj_payload_property_value(AgeWCOJJoinScanState *state,
                                            const AgeAdjacencyPayload *payload,
                                            agtype_value *value,
                                            agtype **properties,
                                            agtype **key,
                                            bool *needs_free);
static void free_age_wcoj_agtype_value(agtype_value *value, bool needs_free);
static void free_age_wcoj_detoasted_agtype(Datum original, agtype *value);
static Datum age_wcoj_numeric_from_value(agtype_value *value);
static Datum age_wcoj_numeric_mul_int64(Datum value, int64 multiplier);
static Datum age_wcoj_numeric_add(Datum left, Datum right);
static float8 age_wcoj_float8_from_value(agtype_value *value);
static void age_wcoj_sum_promote_to_numeric(AgeWCOJJoinScanState *state);
static void age_wcoj_sum_promote_to_float(AgeWCOJJoinScanState *state);
static int64 age_wcoj_checked_int64_product(int64 left, int64 right);
static int64 age_wcoj_checked_int64_sum(int64 left, int64 right);

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

static bool
valid_wcoj_consumer(int consumer)
{
    return consumer >= AGE_WCOJ_CONSUMER_ROWS &&
        consumer <= AGE_WCOJ_CONSUMER_GROUP_AVG_PROPERTY;
}

static int
wcoj_consumer_output_offset(AgeWCOJConsumerKind consumer)
{
    switch (consumer)
    {
    case AGE_WCOJ_CONSUMER_ROWS:
        return 1;
    case AGE_WCOJ_CONSUMER_COUNT:
    case AGE_WCOJ_CONSUMER_COUNT_DISTINCT_KEY:
    case AGE_WCOJ_CONSUMER_DISTINCT_KEY:
        return 2;
    case AGE_WCOJ_CONSUMER_GROUP_COUNT:
    case AGE_WCOJ_CONSUMER_GROUP_COUNT_DISTINCT_KEY:
        return 3;
    case AGE_WCOJ_CONSUMER_SUM_PROPERTY:
    case AGE_WCOJ_CONSUMER_MIN_PROPERTY:
    case AGE_WCOJ_CONSUMER_MAX_PROPERTY:
    case AGE_WCOJ_CONSUMER_AVG_PROPERTY:
        return 2;
    case AGE_WCOJ_CONSUMER_GROUP_SUM_PROPERTY:
    case AGE_WCOJ_CONSUMER_GROUP_MIN_PROPERTY:
    case AGE_WCOJ_CONSUMER_GROUP_MAX_PROPERTY:
    case AGE_WCOJ_CONSUMER_GROUP_AVG_PROPERTY:
        return 3;
    case AGE_WCOJ_CONSUMER_EXISTS:
        return 2;
    }
    elog(ERROR, "invalid AGE WCOJ consumer kind %d", consumer);
    return 1;
}

static bool
age_wcoj_property_aggregate_consumer(AgeWCOJConsumerKind consumer)
{
    return consumer == AGE_WCOJ_CONSUMER_SUM_PROPERTY ||
        consumer == AGE_WCOJ_CONSUMER_GROUP_SUM_PROPERTY ||
        consumer == AGE_WCOJ_CONSUMER_MIN_PROPERTY ||
        consumer == AGE_WCOJ_CONSUMER_MAX_PROPERTY ||
        consumer == AGE_WCOJ_CONSUMER_GROUP_MIN_PROPERTY ||
        consumer == AGE_WCOJ_CONSUMER_GROUP_MAX_PROPERTY ||
        consumer == AGE_WCOJ_CONSUMER_AVG_PROPERTY ||
        consumer == AGE_WCOJ_CONSUMER_GROUP_AVG_PROPERTY;
}

static bool
age_wcoj_property_minmax_consumer(AgeWCOJConsumerKind consumer)
{
    return consumer == AGE_WCOJ_CONSUMER_MIN_PROPERTY ||
        consumer == AGE_WCOJ_CONSUMER_MAX_PROPERTY ||
        consumer == AGE_WCOJ_CONSUMER_GROUP_MIN_PROPERTY ||
        consumer == AGE_WCOJ_CONSUMER_GROUP_MAX_PROPERTY;
}

static bool
age_wcoj_property_avg_consumer(AgeWCOJConsumerKind consumer)
{
    return consumer == AGE_WCOJ_CONSUMER_AVG_PROPERTY ||
        consumer == AGE_WCOJ_CONSUMER_GROUP_AVG_PROPERTY;
}

static bool
age_wcoj_group_property_aggregate_consumer(AgeWCOJConsumerKind consumer)
{
    return consumer == AGE_WCOJ_CONSUMER_GROUP_SUM_PROPERTY ||
        consumer == AGE_WCOJ_CONSUMER_GROUP_MIN_PROPERTY ||
        consumer == AGE_WCOJ_CONSUMER_GROUP_MAX_PROPERTY ||
        consumer == AGE_WCOJ_CONSUMER_GROUP_AVG_PROPERTY;
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
    int consumer_kind;
    Const *output_type_const;
    Const *row_goal_const;
    List *consumer_desc;
    Oid output_type;
    int64 row_goal;
    int row_goal_source;
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
    consumer_kind = intVal(list_nth(
        cscan->custom_private, AGE_WCOJ_JOIN_PRIVATE_CONSUMER));
    output_type_const = list_nth_node(
        Const, cscan->custom_private, AGE_WCOJ_JOIN_PRIVATE_OUTPUT_TYPE);
    row_goal_const = list_nth_node(
        Const, cscan->custom_private, AGE_WCOJ_JOIN_PRIVATE_ROW_GOAL);
    row_goal_source = intVal(list_nth(
        cscan->custom_private, AGE_WCOJ_JOIN_PRIVATE_ROW_GOAL_SOURCE));
    consumer_desc = list_nth(cscan->custom_private,
                             AGE_WCOJ_JOIN_PRIVATE_CONSUMER_DESC);
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
    if (!valid_wcoj_consumer(consumer_kind) ||
        output_type_const->constisnull ||
        output_type_const->consttype != OIDOID)
    {
        elog(ERROR, "invalid AGE WCOJ consumer descriptor");
    }
    output_type = DatumGetObjectId(output_type_const->constvalue);
    if ((consumer_kind == AGE_WCOJ_CONSUMER_COUNT ||
         consumer_kind == AGE_WCOJ_CONSUMER_COUNT_DISTINCT_KEY ||
         consumer_kind == AGE_WCOJ_CONSUMER_GROUP_COUNT ||
         consumer_kind == AGE_WCOJ_CONSUMER_GROUP_COUNT_DISTINCT_KEY) &&
        output_type != INT8OID && output_type != AGTYPEOID)
    {
        elog(ERROR, "invalid AGE WCOJ count output type %u", output_type);
    }
    if (consumer_kind == AGE_WCOJ_CONSUMER_DISTINCT_KEY &&
        output_type != GRAPHIDOID && output_type != INT8OID &&
        output_type != AGTYPEOID)
    {
        elog(ERROR, "invalid AGE WCOJ distinct-key output type %u",
             output_type);
    }
    if (age_wcoj_property_aggregate_consumer(
            (AgeWCOJConsumerKind)consumer_kind))
    {
        int provider_index;
        Const *key_const;
        bool avg_consumer = age_wcoj_property_avg_consumer(
            (AgeWCOJConsumerKind)consumer_kind);

        if ((!avg_consumer && output_type != AGTYPEOID) ||
            (avg_consumer &&
             output_type != AGTYPEOID && output_type != FLOAT8OID) ||
            list_length(consumer_desc) != AGE_WCOJ_SUM_PROPERTY_COUNT)
        {
            elog(ERROR, "invalid AGE WCOJ property aggregate descriptor");
        }
        provider_index = intVal(list_nth(
            consumer_desc, AGE_WCOJ_SUM_PROPERTY_PROVIDER_INDEX));
        key_const = list_nth_node(
            Const, consumer_desc, AGE_WCOJ_SUM_PROPERTY_KEY);
        if (provider_index < 0 || provider_index >= arity ||
            key_const->constisnull || key_const->consttype != AGTYPEOID)
        {
            elog(ERROR, "invalid AGE WCOJ property aggregate key descriptor");
        }
    }
    if (consumer_kind == AGE_WCOJ_CONSUMER_EXISTS &&
        output_type != AGTYPEOID)
    {
        elog(ERROR, "invalid AGE WCOJ exists output type %u", output_type);
    }
    if (row_goal_const->constisnull || row_goal_const->consttype != INT8OID)
        elog(ERROR, "invalid AGE WCOJ row goal descriptor");
    row_goal = DatumGetInt64(row_goal_const->constvalue);
    if (row_goal < 0 ||
        (consumer_kind != AGE_WCOJ_CONSUMER_ROWS &&
         consumer_kind != AGE_WCOJ_CONSUMER_DISTINCT_KEY &&
         consumer_kind != AGE_WCOJ_CONSUMER_EXISTS && row_goal != 0) ||
        (consumer_kind == AGE_WCOJ_CONSUMER_DISTINCT_KEY &&
         row_goal > 0 && row_goal_source != AGE_WCOJ_ROW_GOAL_LIMIT) ||
        (consumer_kind == AGE_WCOJ_CONSUMER_EXISTS &&
         (row_goal <= 0 || row_goal_source != AGE_WCOJ_ROW_GOAL_EXISTS)) ||
        (row_goal == 0 && row_goal_source != AGE_WCOJ_ROW_GOAL_NONE) ||
        (row_goal > 0 &&
         row_goal_source != AGE_WCOJ_ROW_GOAL_LIMIT &&
         row_goal_source != AGE_WCOJ_ROW_GOAL_EXISTS))
    {
        elog(ERROR, "invalid AGE WCOJ row goal " INT64_FORMAT, row_goal);
    }

    state = palloc0(sizeof(*state));
    state->css.ss.ps.type = T_CustomScanState;
    state->css.flags = cscan->flags;
    state->css.methods = &age_wcoj_join_exec_methods;
    state->arity = arity;
    state->requested_engine = (AgeWCOJEngineKind)requested_engine;
    state->planned_engine = (AgeWCOJEngineKind)planned_engine;
    state->actual_engine = state->planned_engine;
    state->consumer_kind = (AgeWCOJConsumerKind)consumer_kind;
    state->row_goal_source = (AgeWCOJRowGoalSource)row_goal_source;
    state->consumer_output_type = output_type;
    state->sum_property_provider_index = -1;
    state->row_goal = row_goal;
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

    if (age_wcoj_property_aggregate_consumer(
            (AgeWCOJConsumerKind)consumer_kind))
    {
        Const *key_const = list_nth_node(
            Const, consumer_desc, AGE_WCOJ_SUM_PROPERTY_KEY);

        state->sum_property_provider_index = intVal(list_nth(
            consumer_desc, AGE_WCOJ_SUM_PROPERTY_PROVIDER_INDEX));
        state->sum_property_key = datumCopy(key_const->constvalue,
                                            key_const->constbyval,
                                            key_const->constlen);
        state->sum_property_key_isnull = false;
    }

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
    state->uniqueness_groups = palloc0(sizeof(Bitmapset *) * arity);
    state->stage_survivors = palloc0(sizeof(int64) * arity);
    age_binding_init_flat_enumerator(&state->flat_enumerator, arity,
                                     estate->es_query_cxt);
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
    int source_rti;
    int source_correlation_group;
    int output_width;

    if (list_length(desc) != AGE_WCOJ_PROVIDER_DESC_COUNT)
        elog(ERROR, "invalid AGE WCOJ provider descriptor");

    provider_kind = intVal(list_nth(desc, AGE_WCOJ_PROVIDER_DESC_KIND));
    source_rti = intVal(list_nth(desc, AGE_WCOJ_PROVIDER_DESC_SOURCE_RTI));
    source_correlation_group = intVal(list_nth(
        desc, AGE_WCOJ_PROVIDER_DESC_SOURCE_CORRELATION_GROUP));
    if (source_rti < 0 || source_correlation_group < 0 ||
        source_correlation_group > state->arity)
    {
        elog(ERROR, "invalid AGE WCOJ source correlation descriptor");
    }
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
    provider->source_rti = (Index)source_rti;
    provider->source_correlation_group = source_correlation_group;
    state->source_correlation_group_count = Max(
        state->source_correlation_group_count, source_correlation_group);
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
initialize_wcoj_source_correlation_enforcement(
    AgeWCOJJoinScanState *state)
{
    int group_id;

    state->source_correlation_enforced = false;
    for (group_id = 1; group_id <= state->source_correlation_group_count;
         group_id++)
    {
        bool has_member = false;
        bool all_adjacency = true;
        int adjacency_count = 0;
        int source_index;

        for (source_index = 0; source_index < state->arity; source_index++)
        {
            AgeWCOJPostingProvider *provider =
                &state->providers[source_index];

            if (provider->source_correlation_group != group_id)
                continue;
            has_member = true;
            if (provider->kind == AGE_WCOJ_PROVIDER_ADJACENCY)
                adjacency_count++;
            else
                all_adjacency = false;
        }
        if (has_member && all_adjacency && adjacency_count >= 2)
        {
            state->source_correlation_enforced = true;
            return;
        }
    }
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
    int output_offset = wcoj_consumer_output_offset(state->consumer_kind);

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
    initialize_wcoj_source_correlation_enforcement(state);

    /*
     * A survivor block is useful whenever at least one direct adjacency
     * provider is present.  Plan-stream providers are buffered by duplicate
     * group while adjacency providers defer payload retrieval to one tagged
     * run scan per block.
     */
    state->batch_payload_enabled = adjacency_provider_count > 0;

    if (age_wcoj_property_aggregate_consumer(state->consumer_kind))
    {
        AgeWCOJPostingProvider *sum_provider;

        if (state->sum_property_provider_index < 0 ||
            state->sum_property_provider_index >= state->arity)
        {
            elog(ERROR, "invalid AGE WCOJ property aggregate provider index");
        }
        sum_provider = &state->providers[state->sum_property_provider_index];
        if (sum_provider->kind != AGE_WCOJ_PROVIDER_ADJACENCY ||
            !sum_provider->fetch_properties)
        {
            elog(ERROR, "AGE WCOJ property aggregate provider is not direct");
        }
    }

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
    {
        increment_wcoj_counter(&state->rows_emitted);
        if (state->row_goal > 0 &&
            (state->consumer_kind == AGE_WCOJ_CONSUMER_ROWS ||
             state->consumer_kind == AGE_WCOJ_CONSUMER_DISTINCT_KEY ||
             state->consumer_kind == AGE_WCOJ_CONSUMER_EXISTS))
        {
            increment_wcoj_counter(&state->row_goal_emitted);
        }
    }

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

static int64
current_age_wcoj_source_bag_bytes(AgeWCOJJoinScanState *state)
{
    int64 bytes = 0;
    int source_index;

    for (source_index = 0; source_index < state->arity; source_index++)
    {
        add_wcoj_counter(&bytes,
                         state->providers[source_index].source_bag_bytes);
    }
    return bytes;
}

static int64
current_age_wcoj_factor_memory(AgeWCOJJoinScanState *state)
{
    int64 bytes;
    int source_index;

    bytes = current_age_wcoj_source_bag_bytes(state);
    add_wcoj_counter(&bytes, current_age_wcoj_binding_node_memory(state));
    if (state->survivor_block_capacity > 0)
    {
        add_wcoj_byte_counter(&bytes, sizeof(graphid) *
                              state->survivor_block_capacity);
    }
    for (source_index = 0; source_index < state->arity; source_index++)
    {
        AgeWCOJPostingProvider *provider = &state->providers[source_index];
        AgeWCOJTupleGroup *group = &state->groups[source_index];

        add_wcoj_counter(&bytes, provider->terminal_bytes);
        if (provider->batch_groups != NULL)
        {
            add_wcoj_byte_counter(&bytes, sizeof(AgeWCOJTupleGroup) *
                                  state->survivor_block_capacity);
        }
        if (provider->batch_payload_ranges != NULL)
        {
            add_wcoj_byte_counter(&bytes,
                                  sizeof(AgeEdgeBag) *
                                  state->survivor_block_capacity);
        }
        if (provider->batch_payload_capacity > 0)
        {
            add_wcoj_byte_counter(&bytes, sizeof(AgeWCOJBatchPayload) *
                                  provider->batch_payload_capacity);
        }
        if (group->adjacency_payload_capacity > 0)
        {
            add_wcoj_byte_counter(&bytes, sizeof(AgeWCOJBatchPayload) *
                                  group->adjacency_payload_capacity);
        }
    }
    return bytes;
}

static int64
current_age_wcoj_binding_node_memory(AgeWCOJJoinScanState *state)
{
    if (state->active_binding_node == NULL)
        return 0;
    return age_binding_node_memory_bytes(state->active_binding_node);
}

static void
update_age_wcoj_peak_memory(AgeWCOJJoinScanState *state)
{
    Size current_memory = 0;
    int64 source_bag_bytes;
    int64 binding_node_memory;
    int64 factor_memory;
    int source_index;

    source_bag_bytes = current_age_wcoj_source_bag_bytes(state);
    if (source_bag_bytes > state->peak_source_bag_bytes)
        state->peak_source_bag_bytes = source_bag_bytes;

    binding_node_memory = current_age_wcoj_binding_node_memory(state);
    if (binding_node_memory > state->peak_binding_node_memory)
        state->peak_binding_node_memory = binding_node_memory;

    factor_memory = current_age_wcoj_factor_memory(state);
    if (factor_memory > state->peak_factor_memory)
        state->peak_factor_memory = factor_memory;

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
update_age_wcoj_payload_block_budget(AgeWCOJJoinScanState *state)
{
    Size batch_memory;
    int64 payload_rows = 0;
    int source_index;

    if (state->batch_context == NULL || state->survivor_block_count <= 0)
        return;

    for (source_index = 0; source_index < state->arity; source_index++)
    {
        AgeWCOJPostingProvider *provider = &state->providers[source_index];

        if (provider->kind != AGE_WCOJ_PROVIDER_ADJACENCY)
            continue;

        add_wcoj_counter(&payload_rows, provider->batch_payload_count);
        if (provider->batch_payload_count >
            provider->peak_batch_payload_count)
        {
            provider->peak_batch_payload_count =
                provider->batch_payload_count;
        }
        if (provider->batch_payload_count > 0)
        {
            double observed =
                (double)provider->batch_payload_count /
                (double)state->survivor_block_count;

            if (observed > provider->observed_payloads_per_survivor)
                provider->observed_payloads_per_survivor = observed;
        }
    }

    if (payload_rows > state->peak_payload_block_rows)
        state->peak_payload_block_rows = payload_rows;

    batch_memory = MemoryContextMemAllocated(state->batch_context, true);
    if (batch_memory > state->batch_memory_budget)
        increment_wcoj_counter(&state->payload_block_budget_overruns);
}

static void
reset_age_wcoj_survivor_block(AgeWCOJJoinScanState *state)
{
    int source_index;

    if (state->providers != NULL)
    {
        for (source_index = 0; source_index < state->arity; source_index++)
            close_age_wcoj_payload_spill(&state->providers[source_index]);
    }
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
        provider->batch_payload_bucket_spills = NULL;
        provider->batch_payload_count = 0;
        provider->batch_payload_capacity = 0;
    }
}

static int
age_wcoj_survivor_block_capacity(AgeWCOJJoinScanState *state)
{
    double bytes_per_survivor = sizeof(graphid);
    Size effective_budget = state->batch_memory_budget;
    int64 source_bag_reserve;
    double capacity;
    int source_index;

    source_bag_reserve = Min(state->peak_source_bag_bytes,
                             (int64)(state->batch_memory_budget / 2));
    if (source_bag_reserve > 0)
    {
        if (source_bag_reserve > state->source_bag_memory_reserve)
            state->source_bag_memory_reserve = source_bag_reserve;
        effective_budget -= (Size)source_bag_reserve;
    }

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
            if (provider->observed_payloads_per_survivor > multiplicity)
                multiplicity = provider->observed_payloads_per_survivor;
            bytes_per_survivor += sizeof(AgeEdgeBag) +
                multiplicity * sizeof(AgeWCOJBatchPayload);
        }
    }

    capacity = (double)effective_budget /
        Max(bytes_per_survivor, 1.0);
    if (capacity < AGE_WCOJ_SURVIVOR_BLOCK_MIN)
    {
        if (state->payload_block_budget_overruns > 0)
        {
            increment_wcoj_counter(&state->payload_block_capacity_clamps);
            if (capacity < 1.0)
                capacity = 1.0;
        }
        else
            capacity = AGE_WCOJ_SURVIVOR_BLOCK_MIN;
    }
    if (capacity > AGE_WCOJ_SURVIVOR_BLOCK_MAX)
        capacity = AGE_WCOJ_SURVIVOR_BLOCK_MAX;
    if (state->row_goal > 0)
    {
        int64 remaining = state->row_goal - state->row_goal_emitted;

        if (remaining <= 0)
            return 1;
        if (capacity > (double)remaining)
        {
            capacity = (double)remaining;
            increment_wcoj_counter(&state->row_goal_block_clamps);
        }
    }
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
                sizeof(AgeEdgeBag) * capacity);
            provider->batch_payload_bucket_spills = palloc0(
                sizeof(AgeWCOJPayloadBucketSpill) * capacity);
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

static bool
age_wcoj_payload_run_filter(int64 run_postings, int64 active_postings,
                            AgeAdjacencyCompositeTerminalFilter *filter,
                            bool *known_empty, void *callback_state)
{
    AgeWCOJPayloadRunFilterState *filter_state = callback_state;
    AgeWCOJJoinScanState *state;

    (void)active_postings;
    if (filter_state == NULL || filter == NULL || known_empty == NULL)
        elog(ERROR, "invalid AGE WCOJ payload run filter state");

    state = filter_state->state;
    filter_state->run_postings = run_postings;
    add_wcoj_counter(&state->payload_main_rows_scanned, run_postings);
    add_wcoj_counter(&state->payload_rows_scanned, run_postings);
    *known_empty = state->survivor_block_count <= 0;
    if (*known_empty)
        return true;

    prepare_age_wcoj_survivor_filter(
        state, filter, filter_state->terminal_label_id);
    return true;
}

static void
prefetch_age_wcoj_adjacency_payloads(AgeWCOJPostingProvider *provider)
{
    AgeWCOJJoinScanState *state = provider->owner;
    AgeAdjacencyVisiblePayloadRunKey *keys;
    AgeAdjacencyVisiblePayloadRunOptions options;
    AgeWCOJPayloadRunFilterState filter_state;
    AgeAdjacencyVisiblePayloadRunScan *run_scan;
    AgeAdjacencyPayload payload;
    AgeAdjacencyVisiblePayloadRunNextBatch batch;
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

    filter_state.state = state;
    filter_state.run_postings = 0;
    filter_state.terminal_label_id = provider->terminal_label_id;
    memset(&options, 0, sizeof(options));
    options.terminal_label_id = provider->terminal_label_id;
    options.filter_callback = age_wcoj_payload_run_filter;
    options.filter_callback_state = &filter_state;
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
           age_adjacency_visible_payload_run_scan_next_tag_batch(
               run_scan, &payload, &tag, &batch))
    {
        AgeSourceBag *bag = tag;
        int survivor_index;

        if (!batch.payload_from_main)
        {
            increment_wcoj_counter(&state->payload_delta_rows_scanned);
            increment_wcoj_counter(&state->payload_rows_scanned);
        }
        else if (filter_state.run_postings <= 0)
        {
            increment_wcoj_counter(&state->payload_main_rows_scanned);
            increment_wcoj_counter(&state->payload_rows_scanned);
        }
        bag_index = age_binding_find_source_bag_index(
            provider->source_bags, provider->source_bag_count, bag);
        if (bag_index < 0)
            elog(ERROR, "invalid AGE WCOJ payload source tag");
        survivor_index = find_age_wcoj_survivor_index(
            state, payload.next_vertex_id);
        if (survivor_index < 0)
            continue;

        increment_wcoj_counter(
            &state->payload_rows_after_survivor_filter);
        increment_wcoj_counter(&state->payload_rows_matched);
        increment_wcoj_counter(&state->payload_rows_fetched);
        if (!age_wcoj_adjacency_payload_passes_local_qual(
                provider, bag_index, &payload))
        {
            continue;
        }
        append_age_wcoj_batch_payload(
            provider, survivor_index, bag_index, &payload);
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
        AgeEdgeBag *range =
            &provider->batch_payload_ranges[entry->survivor_index];
        AgeSourceBag *bag;

        if (range->payload_count == 0)
            range->first_payload = payload_index;
        bag = age_binding_get_source_bag(provider->source_bags,
                                         provider->source_bag_count,
                                         entry->source_bag_index);
        if (bag == NULL)
            elog(ERROR, "invalid AGE WCOJ payload source bag");
        entry->first_tuple =
            age_binding_edge_bag_record_payload(range, bag);
    }
    spill_age_wcoj_payload_buckets(provider);
    MemoryContextSwitchTo(oldcontext);
}

static void
reset_age_wcoj_tuple_group(AgeWCOJTupleGroup *group)
{
    memset(group, 0, sizeof(*group));
    group->kind = AGE_WCOJ_TUPLE_GROUP_ROWS;
}

static void
set_age_wcoj_adjacency_group(AgeWCOJTupleGroup *group,
                             AgeWCOJPostingProvider *provider,
                             AgeWCOJBatchPayload *payloads,
                             int payload_count, int logical_count)
{
    if (payload_count <= 0 || logical_count <= 0)
        elog(ERROR, "invalid AGE WCOJ factorized adjacency group");

    reset_age_wcoj_tuple_group(group);
    group->kind = AGE_WCOJ_TUPLE_GROUP_ADJACENCY;
    group->adjacency_provider = provider;
    group->adjacency_payloads = payloads;
    group->adjacency_payload_count = payload_count;
    group->count = logical_count;
}

static void
append_age_wcoj_adjacency_group_payload(
    AgeWCOJTupleGroup *group, MemoryContext tuple_context,
    AgeWCOJPostingProvider *provider, int source_bag_index,
    const AgeAdjacencyPayload *payload)
{
    AgeSourceBag *bag;
    AgeWCOJBatchPayload *entry;
    MemoryContext oldcontext;

    bag = age_binding_get_source_bag(provider->source_bags,
                                     provider->source_bag_count,
                                     source_bag_index);
    if (bag == NULL)
        elog(ERROR, "invalid AGE WCOJ payload source bag");
    if (group->kind != AGE_WCOJ_TUPLE_GROUP_ADJACENCY)
    {
        if (group->count != 0 || group->tuples != NULL)
            elog(ERROR, "invalid AGE WCOJ tuple group state");
        group->kind = AGE_WCOJ_TUPLE_GROUP_ADJACENCY;
        group->adjacency_provider = provider;
    }
    else if (group->adjacency_provider != provider)
    {
        elog(ERROR, "AGE WCOJ adjacency group provider mismatch");
    }

    oldcontext = MemoryContextSwitchTo(tuple_context);
    if (group->adjacency_payload_count ==
        group->adjacency_payload_capacity)
    {
        int new_capacity = group->adjacency_payload_capacity == 0 ? 8 :
            group->adjacency_payload_capacity * 2;

        if (new_capacity <= group->adjacency_payload_capacity ||
            (Size)new_capacity > MaxAllocSize / sizeof(AgeWCOJBatchPayload))
        {
            ereport(ERROR,
                    (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                     errmsg("AGE WCOJ factorized group is too large")));
        }
        if (group->adjacency_payloads == NULL)
        {
            group->adjacency_payloads = palloc(
                sizeof(AgeWCOJBatchPayload) * new_capacity);
        }
        else
        {
            group->adjacency_payloads = repalloc(
                group->adjacency_payloads,
                sizeof(AgeWCOJBatchPayload) * new_capacity);
        }
        group->adjacency_payload_capacity = new_capacity;
    }

    entry = &group->adjacency_payloads[group->adjacency_payload_count++];
    entry->payload = *payload;
    if (provider->fetch_properties && !payload->properties_isnull)
        entry->payload.properties = datumCopy(payload->properties, false, -1);
    entry->survivor_index = -1;
    entry->source_bag_index = source_bag_index;
    entry->first_tuple =
        age_binding_add_source_multiplicity(&group->count, bag);
    MemoryContextSwitchTo(oldcontext);
}

static AgeWCOJBatchPayload *
get_age_wcoj_adjacency_group_payload(AgeWCOJTupleGroup *group,
                                     int tuple_index,
                                     int *source_row_index)
{
    AgeWCOJPostingProvider *provider = group->adjacency_provider;
    AgeWCOJBatchPayload *entry;
    AgeSourceBag *bag;
    int payload_index;
    int low = 0;
    int high = group->adjacency_payload_count;
    int row_offset;

    if (group->kind != AGE_WCOJ_TUPLE_GROUP_ADJACENCY ||
        provider == NULL ||
        tuple_index < 0 || tuple_index >= group->count)
    {
        elog(ERROR, "invalid AGE WCOJ factorized tuple index");
    }

    while (low < high)
    {
        int middle = low + (high - low) / 2;

        if (group->adjacency_payloads[middle].first_tuple <= tuple_index)
            low = middle + 1;
        else
            high = middle;
    }
    payload_index = low - 1;
    if (payload_index < 0)
        elog(ERROR, "invalid AGE WCOJ factorized payload offset");

    entry = &group->adjacency_payloads[payload_index];
    bag = age_binding_get_source_bag(provider->source_bags,
                                     provider->source_bag_count,
                                     entry->source_bag_index);
    if (bag == NULL)
        elog(ERROR, "invalid AGE WCOJ payload source bag");
    row_offset = tuple_index - entry->first_tuple;
    if (row_offset < 0 || row_offset >= bag->row_count)
        elog(ERROR, "invalid AGE WCOJ factorized source row offset");
    if (source_row_index != NULL)
        *source_row_index = bag->first_row + row_offset;
    return entry;
}

static bool
get_age_wcoj_group_edge_id(AgeWCOJTupleGroup *group, int tuple_index,
                           graphid *edge_id)
{
    if (tuple_index < 0 || tuple_index >= group->count)
        elog(ERROR, "invalid AGE WCOJ tuple group index");

    if (group->kind == AGE_WCOJ_TUPLE_GROUP_ADJACENCY)
    {
        AgeWCOJBatchPayload *entry =
            get_age_wcoj_adjacency_group_payload(group, tuple_index, NULL);

        *edge_id = entry->payload.edge_id;
        return true;
    }

    if (group->kind != AGE_WCOJ_TUPLE_GROUP_ROWS)
        elog(ERROR, "invalid AGE WCOJ tuple group kind");
    if (group->edge_id_valid == NULL || !group->edge_id_valid[tuple_index])
        return false;
    *edge_id = group->edge_ids[tuple_index];
    return true;
}

static int
age_wcoj_group_logical_count(void *callback_state, int source_index)
{
    AgeWCOJJoinScanState *state = callback_state;

    if (state == NULL || source_index < 0 || source_index >= state->arity)
        elog(ERROR, "invalid AGE WCOJ factorized source index");
    return state->groups[source_index].count;
}

static bool
age_wcoj_group_edge_id(void *callback_state, int source_index,
                       int tuple_index, graphid *edge_id)
{
    AgeWCOJJoinScanState *state = callback_state;

    if (state == NULL || source_index < 0 || source_index >= state->arity)
        elog(ERROR, "invalid AGE WCOJ factorized source index");
    return get_age_wcoj_group_edge_id(
        &state->groups[source_index], tuple_index, edge_id);
}

static bool
age_wcoj_source_correlation_group_is_enforced(
    AgeWCOJJoinScanState *state, int group_id)
{
    bool has_member = false;
    bool all_adjacency = true;
    int adjacency_count = 0;
    int source_index;

    if (!state->source_correlation_enforced || group_id <= 0)
        return false;

    for (source_index = 0; source_index < state->arity; source_index++)
    {
        AgeWCOJPostingProvider *provider = &state->providers[source_index];

        if (provider->source_correlation_group != group_id)
            continue;
        has_member = true;
        if (provider->kind == AGE_WCOJ_PROVIDER_ADJACENCY)
            adjacency_count++;
        else
            all_adjacency = false;
    }

    return has_member && all_adjacency && adjacency_count >= 2;
}

static bool
age_wcoj_group_source_position(AgeWCOJJoinScanState *state,
                               int source_index, int tuple_index,
                               graphid *source_key, int *source_row_offset)
{
    AgeWCOJTupleGroup *group;
    AgeWCOJPostingProvider *provider;
    AgeWCOJBatchPayload *entry;
    AgeSourceBag *bag;
    int source_row_index;

    if (source_index < 0 || source_index >= state->arity)
        elog(ERROR, "invalid AGE WCOJ source correlation factor");

    group = &state->groups[source_index];
    if (group->kind != AGE_WCOJ_TUPLE_GROUP_ADJACENCY ||
        group->adjacency_provider == NULL)
    {
        return false;
    }

    provider = group->adjacency_provider;
    entry = get_age_wcoj_adjacency_group_payload(
        group, tuple_index, &source_row_index);
    bag = age_binding_get_source_bag(provider->source_bags,
                                     provider->source_bag_count,
                                     entry->source_bag_index);
    if (bag == NULL)
        elog(ERROR, "invalid AGE WCOJ correlated source bag");
    if (source_row_index < bag->first_row ||
        source_row_index >= bag->first_row + bag->row_count)
    {
        elog(ERROR, "invalid AGE WCOJ correlated source row");
    }

    *source_key = bag->key;
    *source_row_offset = source_row_index - bag->first_row;
    return true;
}

static bool
age_wcoj_source_correlation_accept(void *callback_state, int source_index,
                                   int tuple_index)
{
    AgeWCOJJoinScanState *state = callback_state;
    AgeWCOJPostingProvider *provider;
    graphid source_key;
    int source_row_offset;
    int group_id;
    int other_index;

    if (state == NULL || source_index < 0 || source_index >= state->arity)
        elog(ERROR, "invalid AGE WCOJ source correlation callback");

    provider = &state->providers[source_index];
    group_id = provider->source_correlation_group;
    if (!age_wcoj_source_correlation_group_is_enforced(state, group_id))
        return true;
    if (!age_wcoj_group_source_position(
            state, source_index, tuple_index, &source_key,
            &source_row_offset))
    {
        increment_wcoj_counter(&state->source_correlation_rejects);
        return false;
    }

    for (other_index = 0; other_index < state->arity; other_index++)
    {
        AgeWCOJPostingProvider *other_provider;
        graphid other_source_key;
        int other_source_row_offset;
        int other_tuple_index;

        if (other_index == source_index)
            continue;
        other_provider = &state->providers[other_index];
        if (other_provider->source_correlation_group != group_id)
            continue;

        other_tuple_index = age_binding_flat_enumerator_index(
            &state->flat_enumerator, other_index);
        if (other_tuple_index < 0)
            continue;
        if (!age_wcoj_group_source_position(
                state, other_index, other_tuple_index, &other_source_key,
                &other_source_row_offset) ||
            other_source_key != source_key ||
            other_source_row_offset != source_row_offset)
        {
            increment_wcoj_counter(&state->source_correlation_rejects);
            return false;
        }
    }

    return true;
}

static AgeBindingNode *
build_age_wcoj_binding_node(AgeWCOJJoinScanState *state, graphid matched_key)
{
    AgeBindingNode *node;
    int source_index;

    node = age_binding_create_node(state->group_context, matched_key, true);
    increment_wcoj_counter(&state->binding_nodes_created);

    for (source_index = 0; source_index < state->arity; source_index++)
    {
        AgeWCOJTupleGroup *group = &state->groups[source_index];

        if (group->count <= 0)
            elog(ERROR, "invalid AGE WCOJ binding node factor");

        if (group->kind == AGE_WCOJ_TUPLE_GROUP_ADJACENCY)
        {
            if (group->adjacency_payload_count <= 0)
            {
                elog(ERROR,
                     "invalid AGE WCOJ binding node adjacency factor");
            }
            age_binding_node_add_edge_bag(
                node, -1, 0, group->adjacency_payload_count,
                group->count, group->adjacency_payloads, 0);
            increment_wcoj_counter(&state->binding_node_edge_bags);
        }
        else if (group->kind == AGE_WCOJ_TUPLE_GROUP_ROWS)
        {
            age_binding_node_add_source_bag(
                node, matched_key, 0, group->count, group->tuples, 0);
            increment_wcoj_counter(&state->binding_node_source_bags);
        }
        else
        {
            elog(ERROR, "invalid AGE WCOJ binding node tuple group kind");
        }
    }

    (void)age_binding_node_flat_cardinality(node);
    return node;
}

static void
finish_age_wcoj_binding_node(AgeWCOJJoinScanState *state)
{
    AgeBindingNode *node = state->active_binding_node;
    int64 materialized_rows;

    if (node == NULL)
        return;

    materialized_rows = state->flat_rows_materialized -
        state->active_binding_node_flat_rows_base;
    if (materialized_rows < 0)
        materialized_rows = 0;
    age_binding_node_note_flat_enumeration(node, materialized_rows);
    add_wcoj_counter(&state->binding_node_materialized_flat_rows,
                     age_binding_node_materialized_flat_rows(node));
    add_wcoj_counter(&state->binding_node_flat_rows_avoided,
                     age_binding_node_flat_rows_avoided(node));
    update_age_wcoj_peak_memory(state);
    state->active_binding_node = NULL;
    state->active_binding_node_flat_rows_base = 0;
}

static void
clear_age_wcoj_group(AgeWCOJJoinScanState *state)
{
    int child_index;

    if (state->group_context == NULL)
        return;

    finish_age_wcoj_binding_node(state);
    for (child_index = 0; child_index < state->arity; child_index++)
    {
        if (state->group_slots[child_index] != NULL)
            ExecClearTuple(state->group_slots[child_index]);
        reset_age_wcoj_tuple_group(&state->groups[child_index]);
    }
    age_binding_reset_flat_enumerator(&state->flat_enumerator);
    MemoryContextReset(state->group_context);
    state->group_active = false;
    state->active_group_key = 0;
    state->active_group_key_valid = false;
}

static void
append_age_wcoj_tuple_group(AgeWCOJTupleGroup *group,
                            MemoryContext tuple_context,
                            TupleTableSlot *slot, bool edge_id_valid,
                            graphid edge_id)
{
    MemoryContext old_context;

    if (group->kind != AGE_WCOJ_TUPLE_GROUP_ROWS)
        elog(ERROR, "invalid AGE WCOJ tuple group kind");

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

static void
close_age_wcoj_payload_spill(AgeWCOJPostingProvider *provider)
{
    if (provider->payload_spill_file != NULL)
    {
        BufFileClose(provider->payload_spill_file);
        provider->payload_spill_file = NULL;
    }
}

static bool
should_spill_age_wcoj_payload_property(AgeWCOJPostingProvider *provider,
                                       const AgeAdjacencyPayload *payload)
{
    AgeWCOJJoinScanState *state = provider->owner;
    Size property_size;
    Size current_memory;
    Size large_property_threshold;

    if (!provider->fetch_properties || payload->properties_isnull)
        return false;

    property_size = toast_raw_datum_size(payload->properties);
    large_property_threshold = Max((Size)8192,
                                   state->batch_memory_budget / 8);
    if (property_size >= large_property_threshold)
        return true;

    current_memory = MemoryContextMemAllocated(state->batch_context, true);
    return current_memory + property_size + sizeof(AgeWCOJBatchPayload) >
        state->batch_memory_budget;
}

static void
spill_age_wcoj_payload_property_internal(AgeWCOJPostingProvider *provider,
                                         AgeWCOJBatchPayload *entry,
                                         bool free_existing_property)
{
    AgeWCOJJoinScanState *state = provider->owner;
    Pointer original_property;
    struct varlena *value;
    Size raw_value_size;
    int32 value_size;

    if (entry->payload.properties_isnull)
        return;

    if (provider->payload_spill_file == NULL)
        provider->payload_spill_file = BufFileCreateTemp(false);

    original_property = DatumGetPointer(entry->payload.properties);
    BufFileTell(provider->payload_spill_file,
                &entry->properties_spill_fileno,
                &entry->properties_spill_offset);
    value = PG_DETOAST_DATUM_COPY(entry->payload.properties);
    raw_value_size = VARSIZE_ANY(value);
    if (raw_value_size > PG_INT32_MAX)
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("AGE WCOJ payload property is too large to spill")));
    value_size = (int32)raw_value_size;
    BufFileWrite(provider->payload_spill_file, &value_size,
                 sizeof(value_size));
    BufFileWrite(provider->payload_spill_file, value, value_size);
    entry->properties_spilled = true;
    entry->properties_spill_size = value_size;
    entry->payload.properties = (Datum)0;
    add_wcoj_counter(&state->spill_bytes,
                     sizeof(value_size) + value_size);
    increment_wcoj_counter(&state->payload_rows_spilled);
    if (free_existing_property)
        pfree(original_property);
    pfree(value);
}

static void
spill_age_wcoj_payload_property(AgeWCOJPostingProvider *provider,
                                AgeWCOJBatchPayload *entry)
{
    spill_age_wcoj_payload_property_internal(provider, entry, false);
}

static void
spill_age_wcoj_copied_payload_property(AgeWCOJPostingProvider *provider,
                                       AgeWCOJBatchPayload *entry)
{
    spill_age_wcoj_payload_property_internal(provider, entry, true);
}

static void
restore_age_wcoj_payload_property(AgeWCOJPostingProvider *provider,
                                  AgeWCOJBatchPayload *entry,
                                  MemoryContext context)
{
    struct varlena *value;
    int32 value_size;
    MemoryContext oldcontext;

    if (!entry->properties_spilled)
        return;
    if (provider->payload_spill_file == NULL)
        elog(ERROR, "AGE WCOJ payload spill file is not available");
    if (BufFileSeek(provider->payload_spill_file,
                    entry->properties_spill_fileno,
                    entry->properties_spill_offset, SEEK_SET) != 0)
    {
        elog(ERROR, "could not seek AGE WCOJ payload spill file");
    }
    BufFileReadExact(provider->payload_spill_file, &value_size,
                     sizeof(value_size));
    if (value_size <= 0 || value_size != entry->properties_spill_size)
        elog(ERROR, "invalid AGE WCOJ payload spill record");

    oldcontext = MemoryContextSwitchTo(context);
    value = palloc(value_size);
    MemoryContextSwitchTo(oldcontext);
    BufFileReadExact(provider->payload_spill_file, value, value_size);
    entry->payload.properties = PointerGetDatum(value);
    entry->properties_spilled = false;
}

static bool
should_spill_age_wcoj_payload_buckets(AgeWCOJPostingProvider *provider)
{
    AgeWCOJJoinScanState *state = provider->owner;
    Size current_memory;

    if (provider->batch_payload_count <= 0 ||
        provider->batch_payloads == NULL ||
        provider->batch_payload_bucket_spills == NULL)
    {
        return false;
    }

    current_memory = MemoryContextMemAllocated(state->batch_context, true);
    return current_memory > state->batch_memory_budget;
}

static void
spill_age_wcoj_payload_bucket(AgeWCOJPostingProvider *provider,
                              int survivor_index, AgeEdgeBag *range)
{
    AgeWCOJJoinScanState *state = provider->owner;
    AgeWCOJPayloadBucketSpill *spill;
    AgeWCOJBatchPayload *payloads;
    Size raw_payload_bytes;
    int32 payload_bytes;
    int32 record_size;
    int payload_index;

    if (range->payload_count <= 0)
        return;
    if (provider->batch_payload_bucket_spills == NULL)
        elog(ERROR, "missing AGE WCOJ payload bucket spill descriptors");

    spill = &provider->batch_payload_bucket_spills[survivor_index];
    if (spill->spilled)
        return;

    payloads = &provider->batch_payloads[range->first_payload];
    if ((Size)range->payload_count >
        (MaxAllocSize / sizeof(AgeWCOJBatchPayload)))
    {
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("AGE WCOJ payload bucket is too large to spill")));
    }
    raw_payload_bytes = sizeof(AgeWCOJBatchPayload) *
        (Size)range->payload_count;
    if (raw_payload_bytes == 0 ||
        raw_payload_bytes > (Size)(PG_INT32_MAX - (int32)sizeof(int32)))
    {
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("AGE WCOJ payload bucket spill record is too large")));
    }
    payload_bytes = (int32)raw_payload_bytes;
    record_size = payload_bytes + (int32)sizeof(int32);

    for (payload_index = 0;
         payload_index < range->payload_count;
         payload_index++)
    {
        AgeWCOJBatchPayload *entry = &payloads[payload_index];

        if (provider->fetch_properties &&
            !entry->payload.properties_isnull &&
            !entry->properties_spilled)
        {
            spill_age_wcoj_copied_payload_property(provider, entry);
        }
    }

    if (provider->payload_spill_file == NULL)
        provider->payload_spill_file = BufFileCreateTemp(false);

    BufFileTell(provider->payload_spill_file, &spill->spill_fileno,
                &spill->spill_offset);
    BufFileWrite(provider->payload_spill_file, &range->payload_count,
                 sizeof(range->payload_count));
    BufFileWrite(provider->payload_spill_file, payloads, payload_bytes);

    spill->spilled = true;
    spill->spill_size = record_size;
    spill->payload_count = range->payload_count;
    spill->logical_count = range->logical_count;
    increment_wcoj_counter(&state->payload_bucket_blocks_spilled);
    add_wcoj_counter(&state->payload_bucket_rows_spilled,
                     range->payload_count);
    add_wcoj_counter(&state->payload_bucket_bytes_spilled, record_size);
    add_wcoj_counter(&state->spill_bytes, record_size);
}

static void
spill_age_wcoj_payload_buckets(AgeWCOJPostingProvider *provider)
{
    AgeWCOJJoinScanState *state = provider->owner;
    int survivor_index;

    if (!should_spill_age_wcoj_payload_buckets(provider))
        return;

    for (survivor_index = 0;
         survivor_index < state->survivor_block_count;
         survivor_index++)
    {
        AgeEdgeBag *range = &provider->batch_payload_ranges[survivor_index];

        spill_age_wcoj_payload_bucket(provider, survivor_index, range);
    }

    pfree(provider->batch_payloads);
    provider->batch_payloads = NULL;
    provider->batch_payload_capacity = 0;
}

static AgeWCOJBatchPayload *
load_age_wcoj_payload_bucket(AgeWCOJPostingProvider *provider,
                             int survivor_index, const AgeEdgeBag *range,
                             MemoryContext context)
{
    AgeWCOJPayloadBucketSpill *spill;
    AgeWCOJBatchPayload *payloads;
    int payload_count;
    Size raw_payload_bytes;
    int32 payload_bytes;
    MemoryContext oldcontext;

    if (provider->batch_payload_bucket_spills == NULL ||
        survivor_index < 0 ||
        survivor_index >= provider->owner->survivor_block_count)
    {
        elog(ERROR, "invalid AGE WCOJ payload bucket spill lookup");
    }

    spill = &provider->batch_payload_bucket_spills[survivor_index];
    if (!spill->spilled)
    {
        if (provider->batch_payloads == NULL)
            elog(ERROR, "AGE WCOJ payload bucket is neither memory nor spill");
        return &provider->batch_payloads[range->first_payload];
    }
    if (provider->payload_spill_file == NULL)
        elog(ERROR, "AGE WCOJ payload bucket spill file is not available");
    if (spill->payload_count != range->payload_count ||
        spill->logical_count != range->logical_count)
    {
        elog(ERROR, "invalid AGE WCOJ payload bucket spill descriptor");
    }
    if (BufFileSeek(provider->payload_spill_file, spill->spill_fileno,
                    spill->spill_offset, SEEK_SET) != 0)
    {
        elog(ERROR, "could not seek AGE WCOJ payload bucket spill file");
    }
    BufFileReadExact(provider->payload_spill_file, &payload_count,
                     sizeof(payload_count));
    if (payload_count <= 0 || payload_count != spill->payload_count)
        elog(ERROR, "invalid AGE WCOJ payload bucket spill record");
    if ((Size)payload_count > MaxAllocSize / sizeof(AgeWCOJBatchPayload))
    {
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("AGE WCOJ payload bucket spill record is too large")));
    }
    raw_payload_bytes = sizeof(AgeWCOJBatchPayload) * (Size)payload_count;
    if (raw_payload_bytes == 0 || raw_payload_bytes > (Size)PG_INT32_MAX)
    {
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("AGE WCOJ payload bucket spill record is too large")));
    }
    payload_bytes = (int32)raw_payload_bytes;
    if (spill->spill_size != payload_bytes + (int32)sizeof(int32))
        elog(ERROR, "invalid AGE WCOJ payload bucket spill size");

    oldcontext = MemoryContextSwitchTo(context);
    payloads = palloc((Size)payload_bytes);
    MemoryContextSwitchTo(oldcontext);
    BufFileReadExact(provider->payload_spill_file, payloads, payload_bytes);
    return payloads;
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
    entry->properties_spilled = false;
    entry->properties_spill_fileno = 0;
    entry->properties_spill_offset = 0;
    entry->properties_spill_size = 0;
    if (provider->fetch_properties && !payload->properties_isnull)
    {
        if (should_spill_age_wcoj_payload_property(provider, payload))
            spill_age_wcoj_payload_property(provider, entry);
        else
            entry->payload.properties =
                datumCopy(payload->properties, false, -1);
    }
    entry->survivor_index = survivor_index;
    entry->source_bag_index = source_bag_index;
    MemoryContextSwitchTo(oldcontext);
}

static void
append_age_wcoj_source_row(AgeWCOJPostingProvider *provider, graphid key,
                           TupleTableSlot *slot)
{
    AgeWCOJJoinScanState *state = provider->owner;

    age_binding_append_source_row(&provider->source_rows,
                                  &provider->source_row_count,
                                  &provider->source_row_capacity,
                                  &provider->source_bag_bytes,
                                  provider->provider_context,
                                  key, slot);
    increment_wcoj_counter(&state->source_bag_rows);
}

static void
append_age_wcoj_terminal(AgeWCOJPostingProvider *provider, graphid terminal)
{
    MemoryContext oldcontext;

    oldcontext = MemoryContextSwitchTo(provider->provider_context);
    if (provider->terminal_count == provider->terminal_capacity)
    {
        int old_capacity = provider->terminal_capacity;
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
        add_wcoj_byte_counter(&provider->terminal_bytes,
                              sizeof(graphid) *
                              (new_capacity - old_capacity));
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
    TupleTableSlot *slot;

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

    provider->source_bags = age_binding_build_source_bags(
        provider->source_rows, provider->source_row_count,
        &provider->source_bag_count, &provider->source_bag_bytes,
        provider->provider_context);
    add_wcoj_counter(&state->distinct_source_keys,
                     provider->source_bag_count);
    add_wcoj_counter(&state->source_bag_keys, provider->source_bag_count);
    update_age_wcoj_peak_memory(state);
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
age_wcoj_galloping_lower_bound(AgeWCOJJoinScanState *state,
                               const graphid *values, int value_count,
                               int start_index, graphid target)
{
    int low;
    int high;
    int bound;

    if (start_index >= value_count)
        return value_count;

    increment_wcoj_counter(&state->intersection_galloping_steps);
    if (values[start_index] >= target)
        return start_index;

    bound = 1;
    while (bound < value_count - start_index)
    {
        int probe = start_index + bound;

        increment_wcoj_counter(&state->intersection_galloping_steps);
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

        increment_wcoj_counter(&state->intersection_galloping_steps);
        if (values[middle] < target)
            low = middle + 1;
        else
            high = middle;
    }
    return low;
}

static int
intersect_age_wcoj_graphids(AgeWCOJJoinScanState *state,
                            graphid *candidates, int candidate_count,
                            const graphid *matches, int match_count)
{
    int candidate_index = 0;
    int match_index = 0;
    int output_count = 0;

    if (state == NULL)
        elog(ERROR, "invalid AGE WCOJ intersection state");
    if (candidate_count <= 0 || match_count <= 0)
        return 0;

    if ((int64)match_count >=
        (int64)candidate_count * AGE_WCOJ_GALLOPING_MIN_RATIO)
    {
        increment_wcoj_counter(&state->intersection_galloping_calls);
        while (candidate_index < candidate_count &&
               match_index < match_count)
        {
            graphid candidate = candidates[candidate_index];

            match_index = age_wcoj_galloping_lower_bound(
                state, matches, match_count, match_index, candidate);
            if (match_index >= match_count)
                break;
            if (matches[match_index] == candidate)
            {
                candidates[output_count++] = candidate;
                match_index++;
            }
            candidate_index++;
        }
        return output_count;
    }

    if ((int64)candidate_count >=
        (int64)match_count * AGE_WCOJ_GALLOPING_MIN_RATIO)
    {
        increment_wcoj_counter(&state->intersection_galloping_calls);
        while (candidate_index < candidate_count &&
               match_index < match_count)
        {
            graphid match = matches[match_index];

            candidate_index = age_wcoj_galloping_lower_bound(
                state, candidates, candidate_count, candidate_index, match);
            if (candidate_index >= candidate_count)
                break;
            if (candidates[candidate_index] == match)
            {
                candidates[output_count++] = match;
                candidate_index++;
            }
            match_index++;
        }
        return output_count;
    }

    increment_wcoj_counter(&state->intersection_merge_calls);
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
                state, seed->terminals, candidate_count,
                provider->terminals, provider->terminal_count);
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
    AgeSourceBag *bag;
    int index;

    bag = age_binding_get_source_bag(provider->source_bags,
                                     provider->source_bag_count,
                                     bag_index);
    if (bag == NULL)
        elog(ERROR, "invalid AGE WCOJ payload source bag");

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

static TupleTableSlot *
materialize_age_wcoj_group_entry(AgeWCOJJoinScanState *state,
                                 int child_index, int tuple_index)
{
    AgeWCOJTupleGroup *group = &state->groups[child_index];

    if (tuple_index < 0 || tuple_index >= group->count)
        elog(ERROR, "invalid AGE WCOJ tuple group index");

    if (group->kind == AGE_WCOJ_TUPLE_GROUP_ADJACENCY)
    {
        AgeWCOJBatchPayload *entry;
        int source_row_index;

        entry = get_age_wcoj_adjacency_group_payload(
            group, tuple_index, &source_row_index);
        restore_age_wcoj_payload_property(group->adjacency_provider, entry,
                                          state->group_context);
        fill_age_wcoj_adjacency_output(
            group->adjacency_provider, entry->source_bag_index,
            source_row_index, &entry->payload);
        return group->adjacency_provider->output_slot;
    }

    if (group->kind == AGE_WCOJ_TUPLE_GROUP_ROWS)
    {
        TupleTableSlot *group_slot = state->group_slots[child_index];
        MinimalTuple tuple;

        if (group_slot == NULL || group->tuples == NULL)
            elog(ERROR, "invalid AGE WCOJ tuple group");
        tuple = group->tuples[tuple_index];
        ExecStoreMinimalTuple(tuple, group_slot, false);
        return group_slot;
    }

    elog(ERROR, "invalid AGE WCOJ tuple group kind");
    return NULL;
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

static bool
age_wcoj_adjacency_payload_passes_local_qual(
    AgeWCOJPostingProvider *provider, int bag_index,
    const AgeAdjacencyPayload *payload)
{
    AgeWCOJJoinScanState *state = provider->owner;
    AgeSourceBag *bag;

    if (provider->local_qual == NULL)
        return true;

    bag = age_binding_get_source_bag(provider->source_bags,
                                     provider->source_bag_count,
                                     bag_index);
    if (bag == NULL)
        elog(ERROR, "invalid AGE WCOJ payload source bag");
    fill_age_wcoj_adjacency_output(provider, bag_index, bag->first_row,
                                   payload);
    if (age_wcoj_adjacency_local_qual(provider))
        return true;

    increment_wcoj_counter(&state->local_predicate_rejects);
    return false;
}

static void
append_age_wcoj_adjacency_payload_group(
    AgeWCOJPostingProvider *provider, int bag_index,
    const AgeAdjacencyPayload *payload, bool check_local_qual)
{
    AgeWCOJJoinScanState *state = provider->owner;

    if (check_local_qual &&
        !age_wcoj_adjacency_payload_passes_local_qual(
            provider, bag_index, payload))
    {
        return;
    }

    append_age_wcoj_adjacency_group_payload(
        &state->groups[provider->source_index], state->group_context,
        provider, bag_index, payload);
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
        AgeEdgeBag *range;

        if (provider->batch_payload_ranges == NULL ||
            state->active_survivor_index < 0 ||
            state->active_survivor_index >= state->survivor_block_count)
        {
            elog(ERROR, "invalid AGE WCOJ buffered adjacency group");
        }
        range = &provider->batch_payload_ranges[
            state->active_survivor_index];
        if (range->payload_count <= 0)
            return false;
        if (range->logical_count <= 0)
        {
            elog(ERROR, "invalid AGE WCOJ buffered adjacency group");
        }
        set_age_wcoj_adjacency_group(
            &state->groups[source_index], provider,
            load_age_wcoj_payload_bucket(
                provider, state->active_survivor_index, range,
                state->group_context),
            range->payload_count, range->logical_count);
        return state->groups[source_index].count > 0;
    }

    if (!adjacency_first_distinct(provider) ||
        provider->current_key != terminal)
    {
        return false;
    }

    for (bag_index = 0; bag_index < provider->source_bag_count; bag_index++)
    {
        AgeSourceBag *bag = &provider->source_bags[bag_index];
        AgeAdjacencyPayload payload;
        bool active;

        active = age_adjacency_visible_payload_scan_begin_key(
            provider->payload_scan, bag->key);
        increment_wcoj_counter(&state->payload_scan_batches);
        increment_wcoj_counter(&state->payload_scan_restarts);
        increment_wcoj_counter(&state->payload_source_keys_scanned);
        while (active && age_adjacency_visible_payload_scan_next(
                             provider->payload_scan, &payload))
        {
            increment_wcoj_counter(&state->payload_rows_scanned);
            if (payload.next_vertex_id != terminal)
                continue;
            increment_wcoj_counter(&state->payload_rows_matched);
            increment_wcoj_counter(&state->payload_rows_fetched);
            append_age_wcoj_adjacency_payload_group(
                provider, bag_index, &payload, true);
        }
    }

    (void)adjacency_next_distinct(provider);
    return state->groups[source_index].count > 0;
}

static void
adjacency_rescan(AgeWCOJPostingProvider *provider)
{
    ExecReScan(provider->plan_state);
    close_age_wcoj_payload_spill(provider);
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
    provider->source_bag_bytes = 0;
    provider->terminals = NULL;
    provider->terminal_count = 0;
    provider->terminal_capacity = 0;
    provider->terminal_bytes = 0;
    provider->terminal_index = 0;
    provider->has_current = false;
    provider->exhausted = false;
    provider->built = false;
}

static void
adjacency_close(AgeWCOJPostingProvider *provider)
{
    close_age_wcoj_payload_spill(provider);
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
    int64 flat_rows;

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
    state->active_binding_node = build_age_wcoj_binding_node(
        state, matched_key);
    state->active_binding_node_flat_rows_base =
        state->flat_rows_materialized;
    flat_rows = age_binding_begin_flat_enumerator(
        &state->flat_enumerator, age_wcoj_group_logical_count,
        state->source_correlation_enforced ?
        age_wcoj_source_correlation_accept : NULL,
        age_wcoj_uniqueness_may_reject(state) ? age_wcoj_group_edge_id : NULL,
        state, state->uniqueness_groups);
    if (flat_rows != age_binding_node_candidate_flat_rows(
            state->active_binding_node))
    {
        elog(ERROR, "AGE WCOJ binding node cardinality mismatch");
    }
    add_wcoj_counter(&state->candidate_flat_rows, flat_rows);

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
    update_age_wcoj_payload_block_budget(state);
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
        {
            state->active_group_key = matched_key;
            state->active_group_key_valid = true;
            return true;
        }
    }
}

static bool
next_age_wcoj_combination(AgeWCOJJoinScanState *state)
{
    if (!state->group_active)
        return false;

    if (!age_binding_flat_enumerator_next(&state->flat_enumerator))
    {
        state->group_active = false;
        return false;
    }

    increment_wcoj_counter(&state->candidate_combinations);
    return true;
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
        TupleTableSlot *group_slot;
        int attr_index;

        group_slot = materialize_age_wcoj_group_entry(
            state, child_index,
            age_binding_flat_enumerator_index(
                &state->flat_enumerator, child_index));
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
    increment_wcoj_counter(&state->flat_rows_materialized);

    return raw_slot;
}

static bool
age_wcoj_uniqueness_may_reject(AgeWCOJJoinScanState *state)
{
    int left;
    int right;

    if (state->uniqueness_group_count <= 0)
        return false;

    for (left = 0; left < state->arity; left++)
    {
        for (right = left + 1; right < state->arity; right++)
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

static int64
add_age_wcoj_count(int64 count, int64 increment)
{
    int64 result;

    if (increment < 0 ||
        pg_add_s64_overflow(count, increment, &result))
    {
        ereport(ERROR,
                (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                 errmsg("bigint out of range")));
    }
    return result;
}

static int64
age_wcoj_checked_int64_product(int64 left, int64 right)
{
    int64 result;

    if (pg_mul_s64_overflow(left, right, &result))
    {
        ereport(ERROR,
                (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                 errmsg("bigint out of range")));
    }
    return result;
}

static int64
age_wcoj_checked_int64_sum(int64 left, int64 right)
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

static void
free_age_wcoj_agtype_value(agtype_value *value, bool needs_free)
{
    if (needs_free)
        pfree_agtype_value_content(value);
}

static void
free_age_wcoj_detoasted_agtype(Datum original, agtype *value)
{
    if (value != NULL && (Pointer)value != DatumGetPointer(original))
        pfree(value);
}

static Datum
age_wcoj_numeric_from_value(agtype_value *value)
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
age_wcoj_numeric_mul_int64(Datum value, int64 multiplier)
{
    Datum multiplier_numeric;

    if (multiplier == 1)
        return NumericGetDatum(DatumGetNumericCopy(value));
    multiplier_numeric = DirectFunctionCall1(int8_numeric,
                                             Int64GetDatum(multiplier));
    return DirectFunctionCall2(numeric_mul, value, multiplier_numeric);
}

static Datum
age_wcoj_numeric_add(Datum left, Datum right)
{
    return DirectFunctionCall2(numeric_add, left, right);
}

static float8
age_wcoj_float8_from_value(agtype_value *value)
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
age_wcoj_sum_promote_to_numeric(AgeWCOJJoinScanState *state)
{
    Datum numeric;

    if (!state->sum_has_value || state->sum_value.type == AGTV_NUMERIC)
        return;

    if (state->sum_value.type == AGTV_INTEGER)
    {
        numeric = DirectFunctionCall1(
            int8_numeric, Int64GetDatum(state->sum_value.val.int_value));
    }
    else if (state->sum_value.type == AGTV_FLOAT)
    {
        numeric = DirectFunctionCall1(
            float8_numeric, Float8GetDatum(state->sum_value.val.float_value));
    }
    else
    {
        elog(ERROR, "invalid AGE WCOJ sum state");
        numeric = (Datum)0;
    }

    state->sum_value.type = AGTV_NUMERIC;
    state->sum_value.val.numeric = DatumGetNumeric(numeric);
}

static void
age_wcoj_sum_promote_to_float(AgeWCOJJoinScanState *state)
{
    if (!state->sum_has_value || state->sum_value.type == AGTV_FLOAT)
        return;
    if (state->sum_value.type != AGTV_INTEGER)
        elog(ERROR, "invalid AGE WCOJ sum state");

    state->sum_value.type = AGTV_FLOAT;
    state->sum_value.val.float_value =
        (float8)state->sum_value.val.int_value;
}

static void
add_age_wcoj_avg_value(AgeWCOJJoinScanState *state, agtype_value *value,
                       int64 multiplicity)
{
    float8 addend;

    if (multiplicity <= 0)
        return;
    addend = DatumGetFloat8(DirectFunctionCall2(
        float8mul, Float8GetDatum(age_wcoj_float8_from_value(value)),
        Float8GetDatum((float8)multiplicity)));
    if (!state->sum_has_value)
    {
        state->avg_sum = addend;
        state->sum_has_value = true;
    }
    else
    {
        state->avg_sum = DatumGetFloat8(DirectFunctionCall2(
            float8pl, Float8GetDatum(state->avg_sum),
            Float8GetDatum(addend)));
    }
}

static void
add_age_wcoj_sum_value(AgeWCOJJoinScanState *state, agtype_value *value,
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

        age_wcoj_sum_promote_to_numeric(state);
        addend = age_wcoj_numeric_mul_int64(
            age_wcoj_numeric_from_value(value), multiplicity);
        if (!state->sum_has_value)
        {
            state->sum_value.type = AGTV_NUMERIC;
            state->sum_value.val.numeric = DatumGetNumeric(addend);
            state->sum_has_value = true;
        }
        else
        {
            state->sum_value.val.numeric = DatumGetNumeric(
                age_wcoj_numeric_add(NumericGetDatum(
                                         state->sum_value.val.numeric),
                                     addend));
        }
    }
    else if (value->type == AGTV_FLOAT)
    {
        float8 addend;

        if (state->sum_has_value && state->sum_value.type == AGTV_NUMERIC)
        {
            Datum numeric_addend = age_wcoj_numeric_mul_int64(
                age_wcoj_numeric_from_value(value), multiplicity);

            state->sum_value.val.numeric = DatumGetNumeric(
                age_wcoj_numeric_add(NumericGetDatum(
                                         state->sum_value.val.numeric),
                                     numeric_addend));
            MemoryContextSwitchTo(oldcontext);
            return;
        }

        age_wcoj_sum_promote_to_float(state);
        addend = DatumGetFloat8(DirectFunctionCall2(
            float8mul, Float8GetDatum(value->val.float_value),
            Float8GetDatum((float8)multiplicity)));
        if (!state->sum_has_value)
        {
            state->sum_value.type = AGTV_FLOAT;
            state->sum_value.val.float_value = addend;
            state->sum_has_value = true;
        }
        else
        {
            state->sum_value.val.float_value = DatumGetFloat8(
                DirectFunctionCall2(
                    float8pl,
                    Float8GetDatum(state->sum_value.val.float_value),
                    Float8GetDatum(addend)));
        }
    }
    else
    {
        int64 addend = age_wcoj_checked_int64_product(
            value->val.int_value, multiplicity);

        if (state->sum_has_value && state->sum_value.type == AGTV_NUMERIC)
        {
            Datum numeric_addend = age_wcoj_numeric_mul_int64(
                age_wcoj_numeric_from_value(value), multiplicity);

            state->sum_value.val.numeric = DatumGetNumeric(
                age_wcoj_numeric_add(NumericGetDatum(
                                         state->sum_value.val.numeric),
                                     numeric_addend));
        }
        else if (state->sum_has_value && state->sum_value.type == AGTV_FLOAT)
        {
            state->sum_value.val.float_value = DatumGetFloat8(
                DirectFunctionCall2(
                    float8pl,
                    Float8GetDatum(state->sum_value.val.float_value),
                    Float8GetDatum((float8)addend)));
        }
        else if (!state->sum_has_value)
        {
            state->sum_value.type = AGTV_INTEGER;
            state->sum_value.val.int_value = addend;
            state->sum_has_value = true;
        }
        else
        {
            state->sum_value.val.int_value = age_wcoj_checked_int64_sum(
                state->sum_value.val.int_value, addend);
        }
    }
    MemoryContextSwitchTo(oldcontext);
}

static void
add_age_wcoj_minmax_value(AgeWCOJJoinScanState *state, agtype_value *value)
{
    agtype *candidate;
    bool replace_value;

    candidate = agtype_value_to_agtype(value);
    if (!state->sum_has_value)
    {
        replace_value = true;
    }
    else
    {
        int cmp = compare_agtype_containers_orderability(
            &candidate->root, &state->property_aggregate_value->root);

        replace_value =
            (state->consumer_kind == AGE_WCOJ_CONSUMER_MAX_PROPERTY ||
             state->consumer_kind == AGE_WCOJ_CONSUMER_GROUP_MAX_PROPERTY) ?
            cmp > 0 : cmp < 0;
    }

    if (replace_value)
    {
        MemoryContext oldcontext;

        oldcontext = MemoryContextSwitchTo(
            state->css.ss.ps.state->es_query_cxt);
        pfree_if_not_null(state->property_aggregate_value);
        state->property_aggregate_value = (agtype *)DatumGetPointer(
            datumCopy(PointerGetDatum(candidate), false, -1));
        state->sum_has_value = true;
        MemoryContextSwitchTo(oldcontext);
    }
    pfree(candidate);
}

static bool
age_wcoj_payload_property_value(AgeWCOJJoinScanState *state,
                                const AgeAdjacencyPayload *payload,
                                agtype_value *value,
                                agtype **properties,
                                agtype **key,
                                bool *needs_free)
{
    agtype_value key_value;
    bool key_needs_free = false;
    bool found;

    *needs_free = false;
    *properties = NULL;
    *key = NULL;
    if (payload->properties_isnull || state->sum_property_key_isnull)
        return false;

    *properties = DATUM_GET_AGTYPE_P(payload->properties);
    *key = DATUM_GET_AGTYPE_P(state->sum_property_key);
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
        free_age_wcoj_agtype_value(&key_value, key_needs_free);
        return false;
    }

    found = find_agtype_value_from_container_no_copy(
        &(*properties)->root, AGT_FOBJECT, &key_value, value, needs_free);
    free_age_wcoj_agtype_value(&key_value, key_needs_free);
    if (!found || value->type == AGTV_NULL)
    {
        if (found)
            free_age_wcoj_agtype_value(value, *needs_free);
        *needs_free = false;
        return false;
    }
    return true;
}

static void
add_age_wcoj_sum_payload(AgeWCOJJoinScanState *state,
                         AgeWCOJPostingProvider *provider,
                         AgeWCOJBatchPayload *entry,
                         int64 multiplicity)
{
    agtype_value value;
    agtype *properties;
    agtype *key;
    bool needs_free = false;
    bool found;

    restore_age_wcoj_payload_property(provider, entry,
                                      state->group_context);
    found = age_wcoj_payload_property_value(state, &entry->payload,
                                            &value, &properties, &key,
                                            &needs_free);
    if (!found)
    {
        add_wcoj_counter(&state->sum_null_rows, multiplicity);
    }
    else
    {
        state->sum_input_rows = add_age_wcoj_count(state->sum_input_rows,
                                                   multiplicity);
        if (age_wcoj_property_avg_consumer(state->consumer_kind))
            add_age_wcoj_avg_value(state, &value, multiplicity);
        else if (age_wcoj_property_minmax_consumer(state->consumer_kind))
            add_age_wcoj_minmax_value(state, &value);
        else
            add_age_wcoj_sum_value(state, &value, multiplicity);
        free_age_wcoj_agtype_value(&value, needs_free);
    }

    free_age_wcoj_detoasted_agtype(entry->payload.properties, properties);
    free_age_wcoj_detoasted_agtype(state->sum_property_key, key);
}

static void
add_age_wcoj_sum_active_group(AgeWCOJJoinScanState *state)
{
    int sum_index = state->sum_property_provider_index;
    AgeWCOJTupleGroup *sum_group;

    if (!state->group_active ||
        sum_index < 0 || sum_index >= state->arity)
    {
        return;
    }

    sum_group = &state->groups[sum_index];
    if (sum_group->kind != AGE_WCOJ_TUPLE_GROUP_ADJACENCY ||
        sum_group->adjacency_provider == NULL)
    {
        elog(ERROR, "AGE WCOJ sum-property provider is not direct adjacency");
    }

    if (!age_wcoj_uniqueness_may_reject(state) &&
        !state->source_correlation_enforced)
    {
        int64 other_product = 1;
        int source_index;
        int payload_index;
        int64 group_count = count_age_wcoj_group_product(state);

        for (source_index = 0; source_index < state->arity; source_index++)
        {
            if (source_index == sum_index)
                continue;
            other_product = age_wcoj_checked_int64_product(
                other_product, state->groups[source_index].count);
        }

        add_wcoj_counter(&state->candidate_combinations, group_count);
        add_wcoj_counter(&state->consumer_flat_rows_avoided, group_count);
        for (payload_index = 0;
             payload_index < sum_group->adjacency_payload_count;
             payload_index++)
        {
            AgeWCOJBatchPayload *entry =
                &sum_group->adjacency_payloads[payload_index];
            AgeWCOJPostingProvider *provider =
                sum_group->adjacency_provider;
            AgeSourceBag *bag;
            int64 multiplicity;

            bag = age_binding_get_source_bag(provider->source_bags,
                                             provider->source_bag_count,
                                             entry->source_bag_index);
            if (bag == NULL)
                elog(ERROR, "invalid AGE WCOJ sum-property source bag");
            multiplicity = age_wcoj_checked_int64_product(
                other_product, age_binding_source_bag_row_count(bag));
            add_age_wcoj_sum_payload(state, provider, entry, multiplicity);
        }
        return;
    }

    while (next_age_wcoj_combination(state))
    {
        AgeWCOJBatchPayload *entry;

        entry = get_age_wcoj_adjacency_group_payload(
            sum_group,
            age_binding_flat_enumerator_index(
                &state->flat_enumerator, sum_index),
            NULL);
        increment_wcoj_counter(&state->consumer_flat_rows_avoided);
        add_age_wcoj_sum_payload(state, sum_group->adjacency_provider,
                                 entry, 1);
    }
}

static void
add_age_wcoj_sum_rows(AgeWCOJJoinScanState *state)
{
    for (;;)
    {
        if (!state->group_active && !prepare_age_wcoj_group(state))
            break;

        add_age_wcoj_sum_active_group(state);
        clear_age_wcoj_group(state);
    }
}

static void
reset_age_wcoj_sum_value(AgeWCOJJoinScanState *state)
{
    state->sum_has_value = false;
    state->sum_value.type = AGTV_NULL;
    state->avg_sum = 0.0;
    pfree_if_not_null(state->property_aggregate_value);
    state->property_aggregate_value = NULL;
}

static void
reset_age_wcoj_sum_state(AgeWCOJJoinScanState *state)
{
    state->sum_emitted = false;
    state->sum_executed = false;
    reset_age_wcoj_sum_value(state);
    state->sum_input_rows = 0;
    state->sum_null_rows = 0;
}

static int64
count_age_wcoj_group_product(AgeWCOJJoinScanState *state)
{
    int64 product = 1;
    int source_index;

    for (source_index = 0; source_index < state->arity; source_index++)
    {
        int64 count = state->groups[source_index].count;
        int64 next_product;

        if (count <= 0)
            return 0;
        if (pg_mul_s64_overflow(product, count, &next_product))
        {
            ereport(ERROR,
                    (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                     errmsg("bigint out of range")));
        }
        product = next_product;
    }
    return product;
}

static int64
count_age_wcoj_active_group(AgeWCOJJoinScanState *state)
{
    int64 count = 0;

    if (!state->group_active)
        return 0;

    if (!age_wcoj_uniqueness_may_reject(state) &&
        !state->source_correlation_enforced)
    {
        count = count_age_wcoj_group_product(state);
        add_wcoj_counter(&state->candidate_combinations, count);
        add_wcoj_counter(&state->consumer_flat_rows_avoided, count);
        return count;
    }

    while (next_age_wcoj_combination(state))
    {
        count = add_age_wcoj_count(count, 1);
        increment_wcoj_counter(&state->consumer_flat_rows_avoided);
    }
    return count;
}

static int64
count_age_wcoj_rows(AgeWCOJJoinScanState *state)
{
    int64 count = 0;
    bool use_group_product = !age_wcoj_uniqueness_may_reject(state) &&
        !state->source_correlation_enforced;

    for (;;)
    {
        if (!state->group_active && !prepare_age_wcoj_group(state))
            break;

        if (use_group_product)
        {
            int64 group_count = count_age_wcoj_group_product(state);

            count = add_age_wcoj_count(count, group_count);
            add_wcoj_counter(&state->candidate_combinations, group_count);
            add_wcoj_counter(&state->consumer_flat_rows_avoided, group_count);
            clear_age_wcoj_group(state);
            continue;
        }

        while (next_age_wcoj_combination(state))
        {
            count = add_age_wcoj_count(count, 1);
            increment_wcoj_counter(&state->consumer_flat_rows_avoided);
        }
    }

    return count;
}

static int64
count_age_wcoj_distinct_keys(AgeWCOJJoinScanState *state)
{
    int64 count = 0;
    bool use_group_presence = !age_wcoj_uniqueness_may_reject(state) &&
        !state->source_correlation_enforced;

    for (;;)
    {
        int64 group_count;

        if (!state->group_active && !prepare_age_wcoj_group(state))
            break;

        group_count = count_age_wcoj_group_product(state);
        if (group_count <= 0)
        {
            clear_age_wcoj_group(state);
            continue;
        }

        if (use_group_presence)
        {
            count = add_age_wcoj_count(count, 1);
            add_wcoj_counter(&state->consumer_flat_rows_avoided,
                             group_count);
            clear_age_wcoj_group(state);
            continue;
        }

        if (next_age_wcoj_combination(state))
        {
            count = add_age_wcoj_count(count, 1);
            if (group_count > 1)
            {
                add_wcoj_counter(&state->consumer_flat_rows_avoided,
                                 group_count - 1);
            }
            clear_age_wcoj_group(state);
        }
    }

    return count;
}

static TupleTableSlot *
store_age_wcoj_distinct_key_result(ScanState *node, graphid key)
{
    AgeWCOJJoinScanState *state = (AgeWCOJJoinScanState *)node;
    TupleTableSlot *slot = node->ss_ScanTupleSlot;

    ExecClearTuple(slot);
    if (slot->tts_tupleDescriptor->natts < 1)
        elog(ERROR, "AGE WCOJ distinct-key scan expected an output column");
    memset(slot->tts_values, 0,
           sizeof(Datum) * slot->tts_tupleDescriptor->natts);
    memset(slot->tts_isnull, true,
           sizeof(bool) * slot->tts_tupleDescriptor->natts);

    if (state->consumer_output_type == GRAPHIDOID)
        slot->tts_values[0] = GRAPHID_GET_DATUM(key);
    else if (state->consumer_output_type == INT8OID)
        slot->tts_values[0] = Int64GetDatum((int64)key);
    else if (state->consumer_output_type == AGTYPEOID)
        slot->tts_values[0] = PointerGetDatum(agtype_integer_to_agtype(key));
    else
    {
        ereport(ERROR,
                (errcode(ERRCODE_DATATYPE_MISMATCH),
                 errmsg("AGE WCOJ distinct-key scan cannot produce type %s",
                        format_type_be(state->consumer_output_type))));
    }
    slot->tts_isnull[0] = false;

    ExecStoreVirtualTuple(slot);
    return slot;
}

static TupleTableSlot *
access_age_wcoj_distinct_key_scan(ScanState *node)
{
    AgeWCOJJoinScanState *state = (AgeWCOJJoinScanState *)node;
    bool use_group_presence = !age_wcoj_uniqueness_may_reject(state) &&
        !state->source_correlation_enforced;

    if (state->row_goal > 0 && state->row_goal_emitted >= state->row_goal)
        return ExecClearTuple(node->ss_ScanTupleSlot);

    for (;;)
    {
        graphid key;
        int64 group_count;

        if (!state->group_active && !prepare_age_wcoj_group(state))
            return ExecClearTuple(node->ss_ScanTupleSlot);
        if (!state->active_group_key_valid)
            elog(ERROR, "AGE WCOJ distinct-key scan missing active group key");

        key = state->active_group_key;
        group_count = count_age_wcoj_group_product(state);
        if (group_count <= 0)
        {
            clear_age_wcoj_group(state);
            continue;
        }

        if (use_group_presence)
        {
            add_wcoj_counter(&state->consumer_flat_rows_avoided,
                             group_count);
            clear_age_wcoj_group(state);
            return store_age_wcoj_distinct_key_result(node, key);
        }

        if (next_age_wcoj_combination(state))
        {
            if (group_count > 1)
            {
                add_wcoj_counter(&state->consumer_flat_rows_avoided,
                                 group_count - 1);
            }
            clear_age_wcoj_group(state);
            return store_age_wcoj_distinct_key_result(node, key);
        }
    }
}

static TupleTableSlot *
store_age_wcoj_group_count_result(ScanState *node, graphid key, int64 count)
{
    TupleTableSlot *slot = node->ss_ScanTupleSlot;
    TupleDesc tupdesc = slot->tts_tupleDescriptor;
    Oid key_type;
    Oid count_type;

    ExecClearTuple(slot);
    if (tupdesc->natts < 2)
        elog(ERROR, "AGE WCOJ group count scan expected two output columns");
    memset(slot->tts_values, 0, sizeof(Datum) * tupdesc->natts);
    memset(slot->tts_isnull, true, sizeof(bool) * tupdesc->natts);

    key_type = TupleDescAttr(tupdesc, 0)->atttypid;
    if (key_type == GRAPHIDOID)
        slot->tts_values[0] = GRAPHID_GET_DATUM(key);
    else if (key_type == INT8OID)
        slot->tts_values[0] = Int64GetDatum((int64)key);
    else if (key_type == AGTYPEOID)
        slot->tts_values[0] = PointerGetDatum(agtype_integer_to_agtype(key));
    else
    {
        ereport(ERROR,
                (errcode(ERRCODE_DATATYPE_MISMATCH),
                 errmsg("AGE WCOJ group count scan cannot produce group type %s",
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
                 errmsg("AGE WCOJ group count scan cannot produce count type %s",
                        format_type_be(count_type))));
    }
    slot->tts_isnull[1] = false;

    ExecStoreVirtualTuple(slot);
    return slot;
}

static TupleTableSlot *
access_age_wcoj_group_count_scan(ScanState *node)
{
    AgeWCOJJoinScanState *state = (AgeWCOJJoinScanState *)node;

    state->count_executed = true;
    for (;;)
    {
        graphid key;
        int64 group_count;

        if (!state->group_active && !prepare_age_wcoj_group(state))
            return ExecClearTuple(node->ss_ScanTupleSlot);
        if (!state->active_group_key_valid)
            elog(ERROR, "AGE WCOJ group count missing active group key");

        key = state->active_group_key;
        group_count = count_age_wcoj_active_group(state);
        clear_age_wcoj_group(state);
        if (group_count <= 0)
            continue;

        state->count_result =
            add_age_wcoj_count(state->count_result, group_count);
        return store_age_wcoj_group_count_result(node, key, group_count);
    }
}

static TupleTableSlot *
access_age_wcoj_group_count_distinct_key_scan(ScanState *node)
{
    AgeWCOJJoinScanState *state = (AgeWCOJJoinScanState *)node;
    bool use_group_presence = !age_wcoj_uniqueness_may_reject(state) &&
        !state->source_correlation_enforced;

    state->count_executed = true;
    for (;;)
    {
        graphid key;
        int64 group_count;
        bool has_binding = false;

        if (!state->group_active && !prepare_age_wcoj_group(state))
            return ExecClearTuple(node->ss_ScanTupleSlot);
        if (!state->active_group_key_valid)
        {
            elog(ERROR,
                 "AGE WCOJ group count-distinct missing active group key");
        }

        key = state->active_group_key;
        group_count = count_age_wcoj_group_product(state);
        if (group_count <= 0)
        {
            clear_age_wcoj_group(state);
            continue;
        }

        if (use_group_presence)
        {
            has_binding = true;
            add_wcoj_counter(&state->consumer_flat_rows_avoided,
                             group_count);
        }
        else if (next_age_wcoj_combination(state))
        {
            has_binding = true;
            if (group_count > 1)
            {
                add_wcoj_counter(&state->consumer_flat_rows_avoided,
                                 group_count - 1);
            }
        }

        clear_age_wcoj_group(state);
        if (!has_binding)
            continue;

        state->count_result = add_age_wcoj_count(state->count_result, 1);
        return store_age_wcoj_group_count_result(node, key, 1);
    }
}

static TupleTableSlot *
store_age_wcoj_count_result(ScanState *node, int64 count)
{
    AgeWCOJJoinScanState *state = (AgeWCOJJoinScanState *)node;
    TupleTableSlot *slot = node->ss_ScanTupleSlot;

    ExecClearTuple(slot);
    if (slot->tts_tupleDescriptor->natts < 1)
        elog(ERROR, "AGE WCOJ count scan expected an output column");
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
                 errmsg("AGE WCOJ count scan cannot produce type %s",
                        format_type_be(state->consumer_output_type))));
    }
    slot->tts_isnull[0] = false;
    ExecStoreVirtualTuple(slot);
    return slot;
}

static TupleTableSlot *
access_age_wcoj_count_scan(ScanState *node)
{
    AgeWCOJJoinScanState *state = (AgeWCOJJoinScanState *)node;

    if (state->count_emitted)
        return ExecClearTuple(node->ss_ScanTupleSlot);

    state->count_emitted = true;
    state->count_executed = true;
    if (state->consumer_kind == AGE_WCOJ_CONSUMER_COUNT_DISTINCT_KEY)
        state->count_result = count_age_wcoj_distinct_keys(state);
    else
        state->count_result = count_age_wcoj_rows(state);
    return store_age_wcoj_count_result(node, state->count_result);
}

static TupleTableSlot *
store_age_wcoj_sum_property_result(ScanState *node)
{
    AgeWCOJJoinScanState *state = (AgeWCOJJoinScanState *)node;
    TupleTableSlot *slot = node->ss_ScanTupleSlot;

    ExecClearTuple(slot);
    if (slot->tts_tupleDescriptor->natts < 1)
        elog(ERROR, "AGE WCOJ sum-property scan expected an output column");
    memset(slot->tts_values, 0,
           sizeof(Datum) * slot->tts_tupleDescriptor->natts);
    memset(slot->tts_isnull, true,
           sizeof(bool) * slot->tts_tupleDescriptor->natts);

    if (age_wcoj_property_avg_consumer(state->consumer_kind))
    {
        float8 avg_value;

        if (state->consumer_output_type != AGTYPEOID &&
            state->consumer_output_type != FLOAT8OID)
        {
            ereport(ERROR,
                    (errcode(ERRCODE_DATATYPE_MISMATCH),
                     errmsg("AGE WCOJ sum-property scan cannot produce type %s",
                            format_type_be(state->consumer_output_type))));
        }
        if (!state->sum_has_value)
        {
            ExecStoreVirtualTuple(slot);
            return slot;
        }

        avg_value = DatumGetFloat8(DirectFunctionCall2(
            float8div, Float8GetDatum(state->avg_sum),
            Float8GetDatum((float8)state->sum_input_rows)));
        if (state->consumer_output_type == FLOAT8OID)
            slot->tts_values[0] = Float8GetDatum(avg_value);
        else
        {
            agtype_value value;

            value.type = AGTV_FLOAT;
            value.val.float_value = avg_value;
            slot->tts_values[0] = PointerGetDatum(
                agtype_value_to_agtype(&value));
        }
        slot->tts_isnull[0] = false;
        ExecStoreVirtualTuple(slot);
        return slot;
    }

    if (state->consumer_output_type != AGTYPEOID)
    {
        ereport(ERROR,
                (errcode(ERRCODE_DATATYPE_MISMATCH),
                 errmsg("AGE WCOJ sum-property scan cannot produce type %s",
                        format_type_be(state->consumer_output_type))));
    }

    if (state->sum_has_value && age_wcoj_property_minmax_consumer(
            state->consumer_kind))
    {
        slot->tts_values[0] = PointerGetDatum(
            state->property_aggregate_value);
        slot->tts_isnull[0] = false;
    }
    else if (state->sum_has_value)
    {
        slot->tts_values[0] =
            PointerGetDatum(agtype_value_to_agtype(&state->sum_value));
        slot->tts_isnull[0] = false;
    }

    ExecStoreVirtualTuple(slot);
    return slot;
}

static TupleTableSlot *
store_age_wcoj_group_sum_property_result(ScanState *node, graphid key)
{
    AgeWCOJJoinScanState *state = (AgeWCOJJoinScanState *)node;
    TupleTableSlot *slot = node->ss_ScanTupleSlot;
    TupleDesc tupdesc = slot->tts_tupleDescriptor;
    Oid key_type;
    Oid sum_type;

    ExecClearTuple(slot);
    if (tupdesc->natts < 2)
    {
        elog(ERROR,
             "AGE WCOJ group sum-property scan expected two output columns");
    }
    memset(slot->tts_values, 0, sizeof(Datum) * tupdesc->natts);
    memset(slot->tts_isnull, true, sizeof(bool) * tupdesc->natts);

    key_type = TupleDescAttr(tupdesc, 0)->atttypid;
    if (key_type == GRAPHIDOID)
        slot->tts_values[0] = GRAPHID_GET_DATUM(key);
    else if (key_type == INT8OID)
        slot->tts_values[0] = Int64GetDatum((int64)key);
    else if (key_type == AGTYPEOID)
        slot->tts_values[0] = PointerGetDatum(agtype_integer_to_agtype(key));
    else
    {
        ereport(ERROR,
                (errcode(ERRCODE_DATATYPE_MISMATCH),
                 errmsg("AGE WCOJ group sum-property scan cannot produce group type %s",
                        format_type_be(key_type))));
    }
    slot->tts_isnull[0] = false;

    sum_type = TupleDescAttr(tupdesc, 1)->atttypid;
    if (age_wcoj_property_avg_consumer(state->consumer_kind))
    {
        float8 avg_value;

        if (sum_type != AGTYPEOID && sum_type != FLOAT8OID)
        {
            ereport(ERROR,
                    (errcode(ERRCODE_DATATYPE_MISMATCH),
                     errmsg("AGE WCOJ group sum-property scan cannot produce sum type %s",
                            format_type_be(sum_type))));
        }
        if (!state->sum_has_value)
        {
            ExecStoreVirtualTuple(slot);
            return slot;
        }

        avg_value = DatumGetFloat8(DirectFunctionCall2(
            float8div, Float8GetDatum(state->avg_sum),
            Float8GetDatum((float8)state->sum_input_rows)));
        if (sum_type == FLOAT8OID)
            slot->tts_values[1] = Float8GetDatum(avg_value);
        else
        {
            agtype_value value;

            value.type = AGTV_FLOAT;
            value.val.float_value = avg_value;
            slot->tts_values[1] = PointerGetDatum(
                agtype_value_to_agtype(&value));
        }
        slot->tts_isnull[1] = false;
        ExecStoreVirtualTuple(slot);
        return slot;
    }

    if (sum_type != AGTYPEOID)
    {
        ereport(ERROR,
                (errcode(ERRCODE_DATATYPE_MISMATCH),
                 errmsg("AGE WCOJ group sum-property scan cannot produce sum type %s",
                        format_type_be(sum_type))));
    }
    if (state->sum_has_value && age_wcoj_property_minmax_consumer(
            state->consumer_kind))
    {
        slot->tts_values[1] = PointerGetDatum(
            state->property_aggregate_value);
        slot->tts_isnull[1] = false;
    }
    else if (state->sum_has_value)
    {
        slot->tts_values[1] =
            PointerGetDatum(agtype_value_to_agtype(&state->sum_value));
        slot->tts_isnull[1] = false;
    }

    ExecStoreVirtualTuple(slot);
    return slot;
}

static TupleTableSlot *
access_age_wcoj_group_sum_property_scan(ScanState *node)
{
    AgeWCOJJoinScanState *state = (AgeWCOJJoinScanState *)node;

    state->sum_executed = true;
    for (;;)
    {
        graphid key;

        if (!state->group_active && !prepare_age_wcoj_group(state))
            return ExecClearTuple(node->ss_ScanTupleSlot);
        if (!state->active_group_key_valid)
            elog(ERROR, "AGE WCOJ group sum-property missing active group key");

        key = state->active_group_key;
        reset_age_wcoj_sum_value(state);
        add_age_wcoj_sum_active_group(state);
        clear_age_wcoj_group(state);
        return store_age_wcoj_group_sum_property_result(node, key);
    }
}

static TupleTableSlot *
access_age_wcoj_sum_property_scan(ScanState *node)
{
    AgeWCOJJoinScanState *state = (AgeWCOJJoinScanState *)node;

    if (state->sum_emitted)
        return ExecClearTuple(node->ss_ScanTupleSlot);

    state->sum_emitted = true;
    state->sum_executed = true;
    add_age_wcoj_sum_rows(state);
    return store_age_wcoj_sum_property_result(node);
}

static TupleTableSlot *
store_age_wcoj_exists_result(ScanState *node)
{
    AgeWCOJJoinScanState *state = (AgeWCOJJoinScanState *)node;
    TupleTableSlot *slot = node->ss_ScanTupleSlot;

    ExecClearTuple(slot);
    if (slot->tts_tupleDescriptor->natts < 1)
        elog(ERROR, "AGE WCOJ exists scan expected an output column");
    memset(slot->tts_values, 0,
           sizeof(Datum) * slot->tts_tupleDescriptor->natts);
    memset(slot->tts_isnull, true,
           sizeof(bool) * slot->tts_tupleDescriptor->natts);

    if (state->consumer_output_type != AGTYPEOID)
    {
        ereport(ERROR,
                (errcode(ERRCODE_DATATYPE_MISMATCH),
                 errmsg("AGE WCOJ exists scan cannot produce type %s",
                        format_type_be(state->consumer_output_type))));
    }

    slot->tts_values[0] = boolean_to_agtype(true);
    slot->tts_isnull[0] = false;
    ExecStoreVirtualTuple(slot);
    return slot;
}

static TupleTableSlot *
access_age_wcoj_exists_scan(ScanState *node)
{
    AgeWCOJJoinScanState *state = (AgeWCOJJoinScanState *)node;

    state->exists_executed = true;
    if (state->exists_emitted ||
        (state->row_goal > 0 && state->row_goal_emitted >= state->row_goal))
    {
        return ExecClearTuple(node->ss_ScanTupleSlot);
    }

    for (;;)
    {
        if (!state->group_active && !prepare_age_wcoj_group(state))
            return ExecClearTuple(node->ss_ScanTupleSlot);
        if (next_age_wcoj_combination(state))
        {
            state->exists_emitted = true;
            return store_age_wcoj_exists_result(node);
        }
    }
}

static TupleTableSlot *
access_age_wcoj_join_scan(ScanState *node)
{
    AgeWCOJJoinScanState *state = (AgeWCOJJoinScanState *)node;

    if (state->consumer_kind == AGE_WCOJ_CONSUMER_EXISTS)
        return access_age_wcoj_exists_scan(node);
    if (state->consumer_kind == AGE_WCOJ_CONSUMER_COUNT ||
        state->consumer_kind == AGE_WCOJ_CONSUMER_COUNT_DISTINCT_KEY)
        return access_age_wcoj_count_scan(node);
    if (state->consumer_kind == AGE_WCOJ_CONSUMER_DISTINCT_KEY)
        return access_age_wcoj_distinct_key_scan(node);
    if (state->consumer_kind == AGE_WCOJ_CONSUMER_GROUP_COUNT)
        return access_age_wcoj_group_count_scan(node);
    if (state->consumer_kind == AGE_WCOJ_CONSUMER_GROUP_COUNT_DISTINCT_KEY)
        return access_age_wcoj_group_count_distinct_key_scan(node);
    if (state->consumer_kind == AGE_WCOJ_CONSUMER_SUM_PROPERTY ||
        state->consumer_kind == AGE_WCOJ_CONSUMER_MIN_PROPERTY ||
        state->consumer_kind == AGE_WCOJ_CONSUMER_MAX_PROPERTY ||
        state->consumer_kind == AGE_WCOJ_CONSUMER_AVG_PROPERTY)
        return access_age_wcoj_sum_property_scan(node);
    if (age_wcoj_group_property_aggregate_consumer(state->consumer_kind))
        return access_age_wcoj_group_sum_property_scan(node);
    if (state->row_goal > 0 && state->row_goal_emitted >= state->row_goal)
        return ExecClearTuple(node->ss_ScanTupleSlot);

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
    state->count_emitted = false;
    state->count_executed = false;
    state->count_result = 0;
    state->exists_emitted = false;
    state->exists_executed = false;
    reset_age_wcoj_sum_state(state);
    state->row_goal_emitted = 0;
    state->active_group_key = 0;
    state->active_group_key_valid = false;
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

static char *
format_source_correlation_groups(AgeWCOJJoinScanState *state,
                                 int *provider_count)
{
    StringInfoData buffer;
    bool first = true;
    int source_index;

    initStringInfo(&buffer);
    *provider_count = 0;
    for (source_index = 0; source_index < state->arity; source_index++)
    {
        AgeWCOJPostingProvider *provider = &state->providers[source_index];
        int group_id = provider->source_correlation_group;
        int previous_index;
        int member_index;
        int member_count = 0;
        int plan_stream_count = 0;
        int adjacency_count = 0;

        if (group_id <= 0)
            continue;

        for (previous_index = 0; previous_index < source_index;
             previous_index++)
        {
            AgeWCOJPostingProvider *previous =
                &state->providers[previous_index];

            if (previous->source_correlation_group == group_id)
            {
                break;
            }
        }
        if (previous_index < source_index)
            continue;

        for (member_index = source_index; member_index < state->arity;
             member_index++)
        {
            AgeWCOJPostingProvider *member = &state->providers[member_index];

            if (member->source_correlation_group != group_id)
                continue;
            member_count++;
            if (member->kind == AGE_WCOJ_PROVIDER_PLAN_STREAM)
                plan_stream_count++;
            else if (member->kind == AGE_WCOJ_PROVIDER_ADJACENCY)
                adjacency_count++;
        }
        if (member_count <= 1)
            continue;

        appendStringInfo(
            &buffer,
            "%s%d=%d providers (plan-stream=%d, adjacency=%d)",
            first ? "" : ", ", group_id, member_count, plan_stream_count,
            adjacency_count);
        *provider_count += member_count;
        first = false;
    }
    if (first)
        appendStringInfoString(&buffer, "none");
    return buffer.data;
}

static const char *
age_wcoj_consumer_name(AgeWCOJConsumerKind consumer)
{
    switch (consumer)
    {
    case AGE_WCOJ_CONSUMER_ROWS:
        return "rows";
    case AGE_WCOJ_CONSUMER_COUNT:
        return "count(*)";
    case AGE_WCOJ_CONSUMER_COUNT_DISTINCT_KEY:
        return "count(distinct key)";
    case AGE_WCOJ_CONSUMER_DISTINCT_KEY:
        return "distinct key";
    case AGE_WCOJ_CONSUMER_GROUP_COUNT:
        return "group count(key)";
    case AGE_WCOJ_CONSUMER_GROUP_COUNT_DISTINCT_KEY:
        return "group count(distinct key)";
    case AGE_WCOJ_CONSUMER_SUM_PROPERTY:
        return "sum(property)";
    case AGE_WCOJ_CONSUMER_GROUP_SUM_PROPERTY:
        return "group sum(property)";
    case AGE_WCOJ_CONSUMER_MIN_PROPERTY:
        return "min(property)";
    case AGE_WCOJ_CONSUMER_MAX_PROPERTY:
        return "max(property)";
    case AGE_WCOJ_CONSUMER_GROUP_MIN_PROPERTY:
        return "group min(property)";
    case AGE_WCOJ_CONSUMER_GROUP_MAX_PROPERTY:
        return "group max(property)";
    case AGE_WCOJ_CONSUMER_AVG_PROPERTY:
        return "avg(property)";
    case AGE_WCOJ_CONSUMER_GROUP_AVG_PROPERTY:
        return "group avg(property)";
    case AGE_WCOJ_CONSUMER_EXISTS:
        return "exists";
    }
    return "unknown";
}

static const char *
age_wcoj_row_goal_source_name(AgeWCOJRowGoalSource source)
{
    switch (source)
    {
    case AGE_WCOJ_ROW_GOAL_NONE:
        return "none";
    case AGE_WCOJ_ROW_GOAL_LIMIT:
        return "limit";
    case AGE_WCOJ_ROW_GOAL_EXISTS:
        return "exists";
    }
    return "unknown";
}

static void
explain_age_wcoj_join_scan(CustomScanState *node, List *ancestors,
                           ExplainState *es)
{
    AgeWCOJJoinScanState *state = (AgeWCOJJoinScanState *)node;
    char *estimated_postings;
    char *source_correlation_groups;
    int adjacency_provider_count = 0;
    int source_correlation_provider_count;
    int source_index;

    (void)ancestors;
    for (source_index = 0; source_index < state->arity; source_index++)
    {
        if (state->providers[source_index].kind ==
            AGE_WCOJ_PROVIDER_ADJACENCY)
            adjacency_provider_count++;
    }
    estimated_postings = format_estimated_postings(state);
    source_correlation_groups = format_source_correlation_groups(
        state, &source_correlation_provider_count);
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
    if (source_correlation_provider_count > 0)
    {
        ExplainPropertyInteger("Source Correlated Providers", NULL,
                               source_correlation_provider_count, es);
        ExplainPropertyText("Source Correlation Groups",
                            source_correlation_groups, es);
        ExplainPropertyText(
            "Source Correlation Enforcement",
            state->source_correlation_enforced ? "enumerator-prune" :
            "not required", es);
    }
    ExplainPropertyText("WCOJ Consumer",
                        age_wcoj_consumer_name(state->consumer_kind), es);
    ExplainPropertyInteger("WCOJ Row Goal", NULL, state->row_goal, es);
    ExplainPropertyText("WCOJ Row Goal Source",
                        age_wcoj_row_goal_source_name(
                            state->row_goal_source), es);
    ExplainPropertyBool("Batched Payload Materialization",
                        state->batch_payload_enabled, es);
    ExplainPropertyInteger("WCOJ Arity", NULL, state->arity, es);
    ExplainPropertyInteger("Seed Source", NULL, state->seed_index + 1, es);
    ExplainPropertyText("Estimated Postings", estimated_postings, es);
    pfree(estimated_postings);
    pfree(source_correlation_groups);
    if (es->analyze)
    {
        char *observed_postings = format_observed_postings(state);
        char *stage_survivors = format_stage_survivors(state);
        int64 flat_rows_avoided;

        flat_rows_avoided =
            state->candidate_flat_rows > state->candidate_combinations ?
            state->candidate_flat_rows - state->candidate_combinations : 0;

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
        ExplainPropertyInteger("Payload Scan Restarts", NULL,
                               state->payload_scan_restarts, es);
        ExplainPropertyInteger("Payload Source Keys Scanned", NULL,
                               state->payload_source_keys_scanned, es);
        ExplainPropertyInteger("Distinct Source Keys Scanned", NULL,
                               state->payload_source_keys_scanned, es);
        ExplainPropertyInteger("Payload Scan Restarts Avoided", NULL,
                               state->payload_scan_restarts_avoided, es);
        ExplainPropertyInteger("Payload Rows Scanned", NULL,
                               state->payload_rows_scanned, es);
        ExplainPropertyInteger("Payload Main Rows Scanned", NULL,
                               state->payload_main_rows_scanned, es);
        ExplainPropertyInteger("Payload Delta Rows Scanned", NULL,
                               state->payload_delta_rows_scanned, es);
        ExplainPropertyInteger("Payload Rows After Survivor Filter", NULL,
                               state->payload_rows_after_survivor_filter,
                               es);
        ExplainPropertyInteger("Payload Rows Matched", NULL,
                               state->payload_rows_matched, es);
        ExplainPropertyInteger("Payload Rows Fetched", NULL,
                               state->payload_rows_fetched, es);
        ExplainPropertyInteger("Payload Block Budget Overruns", NULL,
                               state->payload_block_budget_overruns, es);
        ExplainPropertyInteger("Payload Block Capacity Clamps", NULL,
                               state->payload_block_capacity_clamps, es);
        ExplainPropertyInteger("Peak Payload Block Rows", NULL,
                               state->peak_payload_block_rows, es);
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
        ExplainPropertyInteger("Source Bag Rows", NULL,
                               state->source_bag_rows, es);
        ExplainPropertyInteger("Source Bag Keys", NULL,
                               state->source_bag_keys, es);
        ExplainPropertyInteger("Factorized Binding Source Bags", NULL,
                               state->source_bag_keys, es);
        ExplainPropertyInteger("Binding Nodes Created", NULL,
                               state->binding_nodes_created, es);
        ExplainPropertyInteger("Binding Node Source Bags", NULL,
                               state->binding_node_source_bags, es);
        ExplainPropertyInteger("Binding Node Edge Bags", NULL,
                               state->binding_node_edge_bags, es);
        ExplainPropertyInteger("Binding Node Memory", "bytes",
                               state->peak_binding_node_memory, es);
        ExplainPropertyInteger("Source Bag Bytes", "bytes",
                               state->peak_source_bag_bytes, es);
        ExplainPropertyInteger("Source Bag Memory Reserve", "bytes",
                               state->source_bag_memory_reserve, es);
        ExplainPropertyInteger("Intersection Builds", NULL,
                               state->intersection_builds, es);
        ExplainPropertyInteger("Intersection Merge Calls", NULL,
                               state->intersection_merge_calls, es);
        ExplainPropertyInteger("Intersection Galloping Calls", NULL,
                               state->intersection_galloping_calls, es);
        ExplainPropertyInteger("Intersection Galloping Steps", NULL,
                               state->intersection_galloping_steps, es);
        ExplainPropertyInteger("Payload Rows Rejected by Local Qual", NULL,
                               state->local_predicate_rejects, es);
        ExplainPropertyInteger("Local Predicate Rejects", NULL,
                               state->local_predicate_rejects, es);
        ExplainPropertyInteger("Matched Terminal Groups", NULL,
                               state->groups_matched, es);
        ExplainPropertyInteger("Candidate Flat Rows", NULL,
                               state->candidate_flat_rows, es);
        ExplainPropertyInteger("Candidate Bag Combinations", NULL,
                               state->candidate_combinations, es);
        ExplainPropertyInteger("Flat Rows Materialized", NULL,
                               state->flat_rows_materialized, es);
        ExplainPropertyInteger(
            "Factorized Binding Enumerators", NULL,
            age_binding_flat_enumerator_starts(&state->flat_enumerator), es);
        ExplainPropertyInteger(
            "Shared Factor Enumerator Steps", NULL,
            age_binding_flat_enumerator_steps(&state->flat_enumerator), es);
        ExplainPropertyInteger("Flat Rows Avoided", NULL,
                               flat_rows_avoided, es);
        ExplainPropertyInteger("Binding Node Flat Rows Avoided", NULL,
                               state->binding_node_flat_rows_avoided, es);
        ExplainPropertyInteger("Consumer Flat Rows Avoided", NULL,
                               state->consumer_flat_rows_avoided, es);
        if (source_correlation_provider_count > 0)
        {
            ExplainPropertyInteger("Source Correlation Rejects", NULL,
                                   state->source_correlation_rejects, es);
        }
        ExplainPropertyInteger("Row Goal Survivor Blocks Clamped", NULL,
                               state->row_goal_block_clamps, es);
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
        if ((state->consumer_kind == AGE_WCOJ_CONSUMER_COUNT ||
             state->consumer_kind == AGE_WCOJ_CONSUMER_COUNT_DISTINCT_KEY ||
             state->consumer_kind == AGE_WCOJ_CONSUMER_GROUP_COUNT ||
             state->consumer_kind ==
             AGE_WCOJ_CONSUMER_GROUP_COUNT_DISTINCT_KEY) &&
            state->count_executed)
        {
            ExplainPropertyInteger("Count Result", NULL,
                                   state->count_result, es);
        }
        if (age_wcoj_property_aggregate_consumer(state->consumer_kind) &&
            state->sum_executed)
        {
            const char *prefix = "Sum";
            char *provider_label;
            char *input_label;
            char *null_label;

            if (state->consumer_kind == AGE_WCOJ_CONSUMER_MIN_PROPERTY ||
                state->consumer_kind == AGE_WCOJ_CONSUMER_GROUP_MIN_PROPERTY)
                prefix = "Min";
            else if (state->consumer_kind == AGE_WCOJ_CONSUMER_MAX_PROPERTY ||
                     state->consumer_kind ==
                     AGE_WCOJ_CONSUMER_GROUP_MAX_PROPERTY)
                prefix = "Max";
            else if (state->consumer_kind == AGE_WCOJ_CONSUMER_AVG_PROPERTY ||
                     state->consumer_kind ==
                     AGE_WCOJ_CONSUMER_GROUP_AVG_PROPERTY)
                prefix = "Avg";

            provider_label = psprintf("%s Property Provider", prefix);
            input_label = psprintf("%s Property Input Rows", prefix);
            null_label = psprintf("%s Property Null Rows", prefix);
            ExplainPropertyInteger(provider_label, NULL,
                                   state->sum_property_provider_index + 1,
                                   es);
            ExplainPropertyInteger(input_label, NULL,
                                   state->sum_input_rows, es);
            ExplainPropertyInteger(null_label, NULL,
                                   state->sum_null_rows, es);
            pfree(provider_label);
            pfree(input_label);
            pfree(null_label);
        }
        if (state->consumer_kind == AGE_WCOJ_CONSUMER_EXISTS &&
            state->exists_executed)
        {
            ExplainPropertyBool("Exists Result", state->exists_emitted, es);
        }
        ExplainPropertyInteger("Uniqueness Constraint Groups", NULL,
                               state->uniqueness_group_count, es);
        ExplainPropertyInteger("Uniqueness Rejects", NULL,
                               age_binding_flat_enumerator_uniqueness_rejects(
                                   &state->flat_enumerator), es);
        ExplainPropertyInteger("Rows Emitted", NULL, state->rows_emitted, es);
        ExplainPropertyInteger("Rescans", NULL, state->rescans, es);
        if (state->rescans > 0)
        {
            ExplainPropertyText("Rescan Strategy", "full provider reset", es);
            ExplainPropertyBool("Outer Key Block Reduction", false, es);
        }
        ExplainPropertyInteger("Peak WCOJ Memory", "bytes",
                               (int64)state->peak_memory, es);
        ExplainPropertyInteger("Peak Factor Memory", "bytes",
                               state->peak_factor_memory, es);
        ExplainPropertyInteger("Payload Rows Spilled", NULL,
                               state->payload_rows_spilled, es);
        ExplainPropertyInteger("Payload Bucket Blocks Spilled", NULL,
                               state->payload_bucket_blocks_spilled, es);
        ExplainPropertyInteger("Payload Bucket Rows Spilled", NULL,
                               state->payload_bucket_rows_spilled, es);
        ExplainPropertyInteger("Payload Bucket Bytes Spilled", "bytes",
                               state->payload_bucket_bytes_spilled, es);
        ExplainPropertyInteger("Spill Bytes", "bytes",
                               state->spill_bytes, es);
        pfree(observed_postings);
        pfree(stage_survivors);
    }
}
