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

#ifndef AG_CYPHER_VLE_STREAM_H
#define AG_CYPHER_VLE_STREAM_H

#include "nodes/extensible.h"
#include "utils/age_global_graph.h"
#include "utils/age_vle.h"

#define AGE_VLE_STREAM_SCAN_NAME "AGE VLE Stream"
#define AGE_VLE_STREAM_MARKER "age_vle_stream_anchor"

typedef enum AgeVLEStreamArgIndex
{
    AGE_VLE_STREAM_ARG_GRAPH = 0,
    AGE_VLE_STREAM_ARG_START,
    AGE_VLE_STREAM_ARG_END,
    AGE_VLE_STREAM_ARG_EDGE,
    AGE_VLE_STREAM_ARG_LOWER,
    AGE_VLE_STREAM_ARG_UPPER,
    AGE_VLE_STREAM_ARG_DIRECTION,
    AGE_VLE_STREAM_ARG_GRAMMAR_NODE,
    AGE_VLE_STREAM_ARG_TERMINAL_PROPERTY,
    AGE_VLE_STREAM_ARG_TERMINAL_LABEL,
    AGE_VLE_STREAM_ARG_COUNT
} AgeVLEStreamArgIndex;

typedef enum AgeVLEStreamPrivateIndex
{
    AGE_VLE_STREAM_PRIVATE_NARGS = 0,
    AGE_VLE_STREAM_PRIVATE_CONST_FLAGS,
    AGE_VLE_STREAM_PRIVATE_GRAPH,
    AGE_VLE_STREAM_PRIVATE_EDGE,
    AGE_VLE_STREAM_PRIVATE_RANGE_DIRECTION,
    AGE_VLE_STREAM_PRIVATE_OUTPUT,
    AGE_VLE_STREAM_PRIVATE_EDGE_SOURCE,
    AGE_VLE_STREAM_PRIVATE_TERMINAL_PROPERTY_PREDICATE_EXPR,
    AGE_VLE_STREAM_PRIVATE_GRAPH_JOIN,
    AGE_VLE_STREAM_PRIVATE_COUNT
} AgeVLEStreamPrivateIndex;

typedef enum AgeVLEStreamGraphIndex
{
    AGE_VLE_STREAM_GRAPH_KNOWN = 0,
    AGE_VLE_STREAM_GRAPH_NULL,
    AGE_VLE_STREAM_GRAPH_VALUE,
    AGE_VLE_STREAM_GRAPH_COUNT
} AgeVLEStreamGraphIndex;

typedef enum AgeVLEStreamEdgeIndex
{
    AGE_VLE_STREAM_EDGE_KNOWN = 0,
    AGE_VLE_STREAM_EDGE_LABEL_KNOWN,
    AGE_VLE_STREAM_EDGE_LABEL_VALUE,
    AGE_VLE_STREAM_EDGE_PROPERTIES_KNOWN,
    AGE_VLE_STREAM_EDGE_PROPERTIES_NULL,
    AGE_VLE_STREAM_EDGE_PROPERTIES_VALUE,
    AGE_VLE_STREAM_EDGE_PROPERTIES_COUNT,
    AGE_VLE_STREAM_EDGE_COUNT
} AgeVLEStreamEdgeIndex;

typedef enum AgeVLEStreamRangeDirectionIndex
{
    AGE_VLE_STREAM_RANGE_LOWER_KNOWN = 0,
    AGE_VLE_STREAM_RANGE_LOWER_NULL,
    AGE_VLE_STREAM_RANGE_LOWER_VALUE,
    AGE_VLE_STREAM_RANGE_UPPER_KNOWN,
    AGE_VLE_STREAM_RANGE_UPPER_NULL,
    AGE_VLE_STREAM_RANGE_UPPER_VALUE,
    AGE_VLE_STREAM_DIRECTION_KNOWN,
    AGE_VLE_STREAM_DIRECTION_NULL,
    AGE_VLE_STREAM_DIRECTION_VALUE,
    AGE_VLE_STREAM_RANGE_DIRECTION_COUNT
} AgeVLEStreamRangeDirectionIndex;

typedef enum AgeVLEStreamOutputIndex
{
    AGE_VLE_STREAM_OUTPUT_GRAMMAR_KNOWN = 0,
    AGE_VLE_STREAM_OUTPUT_GRAMMAR_NULL,
    AGE_VLE_STREAM_OUTPUT_GRAMMAR_VALUE,
    AGE_VLE_STREAM_OUTPUT_REQUIREMENT,
    AGE_VLE_STREAM_OUTPUT_TERMINAL_KEY_KNOWN,
    AGE_VLE_STREAM_OUTPUT_TERMINAL_KEY_NULL,
    AGE_VLE_STREAM_OUTPUT_TERMINAL_KEY_VALUE,
    AGE_VLE_STREAM_OUTPUT_TERMINAL_KEY_LEN,
    AGE_VLE_STREAM_OUTPUT_TERMINAL_KEY_IS_CHAR,
    AGE_VLE_STREAM_OUTPUT_TERMINAL_LABEL_KNOWN,
    AGE_VLE_STREAM_OUTPUT_TERMINAL_LABEL_ID,
    AGE_VLE_STREAM_OUTPUT_TERMINAL_LABEL_MODE,
    AGE_VLE_STREAM_OUTPUT_MATERIALIZER_VERTEX_PREFETCH,
    AGE_VLE_STREAM_OUTPUT_MATERIALIZER_PREFETCH_MIN_REL_CANDIDATES,
    AGE_VLE_STREAM_OUTPUT_COUNT
} AgeVLEStreamOutputIndex;

typedef enum AgeVLEStreamEdgeSourceKind
{
    AGE_VLE_STREAM_EDGE_SOURCE_GLOBAL_METADATA = 0,
    AGE_VLE_STREAM_EDGE_SOURCE_LOCAL_INDEX_CANDIDATE,
    AGE_VLE_STREAM_EDGE_SOURCE_DYNAMIC
} AgeVLEStreamEdgeSourceKind;

typedef enum AgeVLEStreamDirectedSourceKind
{
    AGE_VLE_STREAM_DIRECTED_SOURCE_NONE = 0,
    AGE_VLE_STREAM_DIRECTED_SOURCE_AGE_ADJACENCY,
    AGE_VLE_STREAM_DIRECTED_SOURCE_ENDPOINT_BTREE
} AgeVLEStreamDirectedSourceKind;

typedef enum AgeVLEStreamEdgeSourceIndex
{
    AGE_VLE_STREAM_EDGE_SOURCE_KIND = 0,
    AGE_VLE_STREAM_EDGE_SOURCE_ADJACENCY_OUT,
    AGE_VLE_STREAM_EDGE_SOURCE_ADJACENCY_IN,
    AGE_VLE_STREAM_EDGE_SOURCE_ENDPOINT_START,
    AGE_VLE_STREAM_EDGE_SOURCE_ENDPOINT_END,
    AGE_VLE_STREAM_EDGE_SOURCE_LOCAL_EDGE_STATE,
    AGE_VLE_STREAM_EDGE_SOURCE_EDGE_LABEL_OID,
    AGE_VLE_STREAM_EDGE_SOURCE_OUTGOING_KIND,
    AGE_VLE_STREAM_EDGE_SOURCE_INCOMING_KIND,
    AGE_VLE_STREAM_EDGE_SOURCE_RELATION_TUPLES,
    AGE_VLE_STREAM_EDGE_SOURCE_START_FANOUT,
    AGE_VLE_STREAM_EDGE_SOURCE_END_FANOUT,
    AGE_VLE_STREAM_EDGE_SOURCE_RELATION_TUPLES_KNOWN,
    AGE_VLE_STREAM_EDGE_SOURCE_START_FANOUT_KNOWN,
    AGE_VLE_STREAM_EDGE_SOURCE_END_FANOUT_KNOWN,
    AGE_VLE_STREAM_EDGE_SOURCE_START_FANOUT_SOURCE,
    AGE_VLE_STREAM_EDGE_SOURCE_END_FANOUT_SOURCE,
    AGE_VLE_STREAM_EDGE_SOURCE_START_VALUE_POSTING_SOURCE,
    AGE_VLE_STREAM_EDGE_SOURCE_END_VALUE_POSTING_SOURCE,
    AGE_VLE_STREAM_EDGE_SOURCE_COST_POLICY,
    AGE_VLE_STREAM_EDGE_SOURCE_POLICY_OUTGOING_KIND,
    AGE_VLE_STREAM_EDGE_SOURCE_POLICY_INCOMING_KIND,
    AGE_VLE_STREAM_EDGE_SOURCE_POLICY_CONSUMER,
    AGE_VLE_STREAM_EDGE_SOURCE_POLICY_CONSUMER_CLASS,
    AGE_VLE_STREAM_EDGE_SOURCE_POLICY_ACTIVE_DIRECTION,
    AGE_VLE_STREAM_EDGE_SOURCE_POLICY_FANOUT_BUDGET,
    AGE_VLE_STREAM_EDGE_SOURCE_POLICY_MATERIALIZATION_WEIGHT,
    AGE_VLE_STREAM_EDGE_SOURCE_POLICY_CLASS,
    AGE_VLE_STREAM_EDGE_SOURCE_POLICY_RECOMMENDATION,
    AGE_VLE_STREAM_EDGE_SOURCE_CACHE_SEED_ELIGIBLE,
    AGE_VLE_STREAM_EDGE_SOURCE_ENDPOINT_HEADROOM_PERCENT,
    AGE_VLE_STREAM_EDGE_SOURCE_EMPTY_LIFECYCLE_ELIGIBLE,
    AGE_VLE_STREAM_EDGE_SOURCE_EMPTY_LIFECYCLE_DEPTH,
    AGE_VLE_STREAM_EDGE_SOURCE_EMPTY_LIFECYCLE_BATCH_SIZE,
    AGE_VLE_STREAM_EDGE_SOURCE_THRESHOLD_INPUT_KNOWN,
    AGE_VLE_STREAM_EDGE_SOURCE_THRESHOLD_INPUT_HEADROOM_PERCENT,
    AGE_VLE_STREAM_EDGE_SOURCE_THRESHOLD_INPUT_BATCH_SIZE,
    AGE_VLE_STREAM_EDGE_SOURCE_THRESHOLD_INPUT_OBSERVED_COUNT,
    AGE_VLE_STREAM_EDGE_SOURCE_THRESHOLD_INPUT_SATURATED_COUNT,
    AGE_VLE_STREAM_EDGE_SOURCE_THRESHOLD_INPUT_RELAXED_COUNT,
    AGE_VLE_STREAM_EDGE_SOURCE_THRESHOLD_INPUT_SOURCE,
    AGE_VLE_STREAM_EDGE_SOURCE_THRESHOLD_INPUT_REASON,
    AGE_VLE_STREAM_EDGE_SOURCE_THRESHOLD_INPUT_CLASS,
    AGE_VLE_STREAM_EDGE_SOURCE_PAYLOAD_INPUT_KNOWN,
    AGE_VLE_STREAM_EDGE_SOURCE_PAYLOAD_INPUT_HEADROOM_PERCENT,
    AGE_VLE_STREAM_EDGE_SOURCE_PAYLOAD_INPUT_SCAN_RUNS,
    AGE_VLE_STREAM_EDGE_SOURCE_PAYLOAD_INPUT_REPLAY_RUNS,
    AGE_VLE_STREAM_EDGE_SOURCE_PAYLOAD_INPUT_SEED_RUNS,
    AGE_VLE_STREAM_EDGE_SOURCE_PAYLOAD_INPUT_REPLAY_PERCENT,
    AGE_VLE_STREAM_EDGE_SOURCE_PAYLOAD_INPUT_SEED_PERCENT,
    AGE_VLE_STREAM_EDGE_SOURCE_PAYLOAD_INPUT_OBSERVED_COUNT,
    AGE_VLE_STREAM_EDGE_SOURCE_PAYLOAD_INPUT_VALUE_POSTING_OBSERVED_COUNT,
    AGE_VLE_STREAM_EDGE_SOURCE_PAYLOAD_INPUT_MATRIX_ACTIVE_PERCENT,
    AGE_VLE_STREAM_EDGE_SOURCE_PAYLOAD_INPUT_MATRIX_FILTER_PERCENT,
    AGE_VLE_STREAM_EDGE_SOURCE_PAYLOAD_INPUT_MATRIX_PRESCAN_PERCENT,
    AGE_VLE_STREAM_EDGE_SOURCE_PAYLOAD_INPUT_MATRIX_REPLAY_SOURCE_PERCENT,
    AGE_VLE_STREAM_EDGE_SOURCE_PAYLOAD_INPUT_MATRIX_RUN_BLOCK_PERCENT,
    AGE_VLE_STREAM_EDGE_SOURCE_PAYLOAD_INPUT_MATRIX_REGROUP_PERCENT,
    AGE_VLE_STREAM_EDGE_SOURCE_PAYLOAD_INPUT_REASON,
    AGE_VLE_STREAM_EDGE_SOURCE_PAYLOAD_INPUT_CLASS,
    AGE_VLE_STREAM_EDGE_SOURCE_PAYLOAD_INPUT_VALUE_POSTING_SOURCE,
    AGE_VLE_STREAM_EDGE_SOURCE_TERMINAL_PROPERTY_SOURCE_KNOWN,
    AGE_VLE_STREAM_EDGE_SOURCE_TERMINAL_LABEL_ID,
    AGE_VLE_STREAM_EDGE_SOURCE_TERMINAL_PROPERTY_INDEX_OID,
    AGE_VLE_STREAM_EDGE_SOURCE_TERMINAL_PROPERTY_FILTER_ID,
    AGE_VLE_STREAM_EDGE_SOURCE_TERMINAL_PROPERTY_LABEL,
    AGE_VLE_STREAM_EDGE_SOURCE_TERMINAL_PROPERTY_SOURCE,
    AGE_VLE_STREAM_EDGE_SOURCE_TERMINAL_PROPERTY_PROVIDER,
    AGE_VLE_STREAM_EDGE_SOURCE_TERMINAL_PROPERTY_TYPE,
    AGE_VLE_STREAM_EDGE_SOURCE_TERMINAL_PROPERTY_MATCH_COUNT,
    AGE_VLE_STREAM_EDGE_SOURCE_COMPOSITE_SOURCE_KNOWN,
    AGE_VLE_STREAM_EDGE_SOURCE_COMPOSITE_SOURCE_STATUS,
    AGE_VLE_STREAM_EDGE_SOURCE_COMPOSITE_SOURCE_REASON,
    AGE_VLE_STREAM_EDGE_SOURCE_COMPOSITE_SOURCE_PROPERTY_TUPLES,
    AGE_VLE_STREAM_EDGE_SOURCE_COMPOSITE_SOURCE_CANDIDATE_FANOUT,
    AGE_VLE_STREAM_EDGE_SOURCE_COMPOSITE_SOURCE_FANOUT,
    AGE_VLE_STREAM_EDGE_SOURCE_COMPOSITE_SOURCE_SELECTIVITY_PPM,
    AGE_VLE_STREAM_EDGE_SOURCE_COMPOSITE_SOURCE_SELECTIVITY_SOURCE,
    AGE_VLE_STREAM_EDGE_SOURCE_COMPOSITE_SOURCE_PLANNED,
    AGE_VLE_STREAM_EDGE_SOURCE_TERMINAL_PROPERTY_PREDICATE_KNOWN,
    AGE_VLE_STREAM_EDGE_SOURCE_TERMINAL_PROPERTY_PREDICATE_KEY,
    AGE_VLE_STREAM_EDGE_SOURCE_TERMINAL_PROPERTY_PREDICATE_NULL,
    AGE_VLE_STREAM_EDGE_SOURCE_TERMINAL_PROPERTY_PREDICATE_VALUE,
    AGE_VLE_STREAM_EDGE_SOURCE_TERMINAL_PROPERTY_VALUE_KIND,
    AGE_VLE_STREAM_EDGE_SOURCE_TERMINAL_PROPERTY_PREFILTER_ELIGIBLE,
    AGE_VLE_STREAM_EDGE_SOURCE_TERMINAL_PROPERTY_PREFETCH_THRESHOLD,
    AGE_VLE_STREAM_EDGE_SOURCE_COUNT
} AgeVLEStreamEdgeSourceIndex;

typedef struct AgeVLEStreamRangeDirection
{
    bool lower_known;
    bool lower_null;
    int64 lower_value;
    bool upper_known;
    bool upper_null;
    int64 upper_value;
    bool direction_known;
    bool direction_null;
    int64 direction_value;
} AgeVLEStreamRangeDirection;

typedef struct AgeVLEStreamGraph
{
    bool graph_known;
    bool graph_null;
    char *graph_name;
} AgeVLEStreamGraph;

typedef struct AgeVLEStreamEdge
{
    bool edge_known;
    bool label_known;
    char *label_name;
    bool properties_known;
    bool properties_null;
    Datum properties_value;
    int properties_count;
} AgeVLEStreamEdge;

typedef struct AgeVLEStreamOutput
{
    bool grammar_known;
    bool grammar_null;
    int64 grammar_value;
    AgeVLEOutputRequirement requirement;
    bool terminal_key_known;
    bool terminal_key_null;
    char *terminal_key_value;
    int terminal_key_len;
    bool terminal_key_is_char;
    bool terminal_label_known;
    int32 terminal_label_id;
    AgeVLETerminalLabelMode terminal_label_mode;
    bool materializer_vertex_prefetch;
    int materializer_prefetch_min_rel_candidates;
} AgeVLEStreamOutput;

typedef struct AgeVLEStreamEdgeSource
{
    AgeVLEStreamEdgeSourceKind kind;
    bool adjacency_out;
    bool adjacency_in;
    bool endpoint_start;
    bool endpoint_end;
    bool local_edge_state;
    Oid edge_label_oid;
    AgeVLEStreamDirectedSourceKind outgoing_kind;
    AgeVLEStreamDirectedSourceKind incoming_kind;
    int64 relation_tuples;
    int64 start_fanout;
    int64 end_fanout;
    bool relation_tuples_known;
    bool start_fanout_known;
    bool end_fanout_known;
    char *start_fanout_source;
    char *end_fanout_source;
    char *start_value_posting_source;
    char *end_value_posting_source;
    char *cost_policy;
    AgeVLEStreamDirectedSourceKind policy_outgoing_kind;
    AgeVLEStreamDirectedSourceKind policy_incoming_kind;
    char *policy_consumer;
    char *policy_consumer_class;
    char *policy_active_direction;
    int64 policy_fanout_budget;
    int64 policy_materialization_weight;
    char *policy_class;
    char *policy_recommendation;
    bool cache_seed_eligible;
    int64 endpoint_headroom_percent;
    bool empty_lifecycle_eligible;
    int64 empty_lifecycle_depth;
    int64 empty_lifecycle_batch_size;
    bool threshold_input_known;
    int64 threshold_input_headroom_percent;
    int64 threshold_input_batch_size;
    int64 threshold_input_observed_count;
    int64 threshold_input_saturated_count;
    int64 threshold_input_relaxed_count;
    char *threshold_input_source;
    char *threshold_input_reason;
    char *threshold_input_class;
    bool payload_input_known;
    int64 payload_input_headroom_percent;
    int64 payload_input_scan_runs;
    int64 payload_input_replay_runs;
    int64 payload_input_seed_runs;
    int64 payload_input_replay_percent;
    int64 payload_input_seed_percent;
    int64 payload_input_observed_count;
    int64 payload_input_value_posting_observed_count;
    int64 payload_input_matrix_active_percent;
    int64 payload_input_matrix_filter_percent;
    int64 payload_input_matrix_prescan_percent;
    int64 payload_input_matrix_replay_source_percent;
    int64 payload_input_matrix_run_block_percent;
    int64 payload_input_matrix_regroup_percent;
    char *payload_input_reason;
    char *payload_input_class;
    char *payload_input_value_posting_source;
    bool terminal_property_source_known;
    int32 terminal_label_id;
    Oid terminal_property_index_oid;
    uint32 terminal_property_filter_id;
    char *terminal_property_label;
    char *terminal_property_source;
    char *terminal_property_provider;
    char *terminal_property_type;
    int64 terminal_property_match_count;
    bool composite_source_known;
    char *composite_source_status;
    char *composite_source_reason;
    int64 composite_source_property_tuples;
    int64 composite_source_candidate_fanout;
    int64 composite_source_fanout;
    int64 composite_source_selectivity_ppm;
    char *composite_source_selectivity_source;
    char *composite_source_planned;
    bool terminal_property_predicate_known;
    char *terminal_property_predicate_key;
    bool terminal_property_predicate_null;
    Datum terminal_property_predicate_value;
    char *terminal_property_value_kind;
    bool terminal_property_prefilter_eligible;
    int64 terminal_property_prefetch_threshold;
} AgeVLEStreamEdgeSource;

extern const CustomScanMethods age_vle_stream_scan_methods;

void read_age_vle_stream_graph(CustomScan *cscan,
                               AgeVLEStreamGraph *graph);
void read_age_vle_stream_edge(CustomScan *cscan,
                              AgeVLEStreamEdge *edge);
void read_age_vle_stream_range_direction(CustomScan *cscan,
                                         AgeVLEStreamRangeDirection *range);
void read_age_vle_stream_output(CustomScan *cscan,
                                AgeVLEStreamOutput *output);
void read_age_vle_stream_edge_source(CustomScan *cscan,
                                     AgeVLEStreamEdgeSource *source);
const char *age_vle_stream_shape_name(AgeVLEStreamOutput *output, int nargs);
List *make_age_vle_stream_slot_descriptions(CustomScan *cscan, int nargs);
char *format_age_vle_stream_graph(AgeVLEStreamGraph *graph);
char *format_age_vle_stream_edge(AgeVLEStreamEdge *edge);
char *format_age_vle_stream_endpoints(CustomScan *cscan);
char *format_age_vle_stream_range(AgeVLEStreamRangeDirection *range);
const char *format_age_vle_stream_direction(
    AgeVLEStreamRangeDirection *range);
char *format_age_vle_stream_output(AgeVLEStreamOutput *output, int nargs);
const char *format_age_vle_stream_materialization(
    AgeVLEStreamOutput *output, AgeVLEStreamEdgeSource *source);
const char *format_age_vle_stream_terminal_slot(CustomScan *cscan,
                                                AgeVLEStreamOutput *output,
                                                int nargs);

#endif
