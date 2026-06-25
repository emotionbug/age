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

#include "access/age_adjacency.h"
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

typedef enum AgeVLEStreamFanoutSourceKind
{
    AGE_VLE_STREAM_FANOUT_SOURCE_UNKNOWN = 0,
    AGE_VLE_STREAM_FANOUT_SOURCE_STATISTICS,
    AGE_VLE_STREAM_FANOUT_SOURCE_DIRECTORY,
    AGE_VLE_STREAM_FANOUT_SOURCE_DIRECTORY_LABEL
} AgeVLEStreamFanoutSourceKind;

typedef enum VLESourceDirectionClass
{
    VLE_SOURCE_DIRECTION_NONE = 0,
    VLE_SOURCE_DIRECTION_OUT,
    VLE_SOURCE_DIRECTION_IN,
    VLE_SOURCE_DIRECTION_BOTH,
    VLE_SOURCE_DIRECTION_MIXED
} VLESourceDirectionClass;

typedef enum VLESourcePayloadFeedbackReason
{
    VLE_SOURCE_PAYLOAD_REASON_NONE = 0,
    VLE_SOURCE_PAYLOAD_REASON_SCAN_OBSERVED,
    VLE_SOURCE_PAYLOAD_REASON_VALUE_POSTING_OBSERVED,
    VLE_SOURCE_PAYLOAD_REASON_DIRECTORY_FILTER_OBSERVED,
    VLE_SOURCE_PAYLOAD_REASON_COMPOSITE_PREFILTER_OBSERVED,
    VLE_SOURCE_PAYLOAD_REASON_MATRIX_PRESCAN_COMPACT,
    VLE_SOURCE_PAYLOAD_REASON_MATRIX_FULL_GROUP_DRAIN,
    VLE_SOURCE_PAYLOAD_REASON_MATRIX_REPLAY_SOURCE_BATCH,
    VLE_SOURCE_PAYLOAD_REASON_MATRIX_BLOCK_TAG_BATCH,
    VLE_SOURCE_PAYLOAD_REASON_MATRIX_RUN_BLOCK_STREAM,
    VLE_SOURCE_PAYLOAD_REASON_MATRIX_RAW_BLOCK_BATCH,
    VLE_SOURCE_PAYLOAD_REASON_MATRIX_RUN_BLOCK_GROUP,
    VLE_SOURCE_PAYLOAD_REASON_MATRIX_PAGE_GROUP,
    VLE_SOURCE_PAYLOAD_REASON_MATRIX_FALLBACK_REGROUP,
    VLE_SOURCE_PAYLOAD_REASON_MATRIX_ARENA_COMPACTION,
    VLE_SOURCE_PAYLOAD_REASON_MATRIX_FRONTIER_PRESSURE,
    VLE_SOURCE_PAYLOAD_REASON_REPLAY_RATIO_OBSERVED,
    VLE_SOURCE_PAYLOAD_REASON_REPLAY_OBSERVED,
    VLE_SOURCE_PAYLOAD_REASON_CACHE_SEEDED
} VLESourcePayloadFeedbackReason;

typedef enum VLESourceThresholdFeedbackReason
{
    VLE_SOURCE_THRESHOLD_REASON_NONE = 0,
    VLE_SOURCE_THRESHOLD_REASON_PLANNED_EMPTY_LIFECYCLE,
    VLE_SOURCE_THRESHOLD_REASON_ROOT_EMPTY_OBSERVED,
    VLE_SOURCE_THRESHOLD_REASON_ROOT_EMPTY_SATURATED,
    VLE_SOURCE_THRESHOLD_REASON_ROOT_EMPTY_REPEAT_OBSERVED,
    VLE_SOURCE_THRESHOLD_REASON_ROOT_EMPTY_REPEAT_SATURATED
} VLESourceThresholdFeedbackReason;

typedef enum VLESourcePolicyRecommendation
{
    VLE_SOURCE_RECOMMENDATION_NONE = 0,
    VLE_SOURCE_RECOMMENDATION_KEEP_LAYOUT,
    VLE_SOURCE_RECOMMENDATION_COLLECT_ENDPOINT_STATS,
    VLE_SOURCE_RECOMMENDATION_KEEP_VALUE_POSTING,
    VLE_SOURCE_RECOMMENDATION_KEEP_PROPERTY_PREFILTER,
    VLE_SOURCE_RECOMMENDATION_KEEP_AGE_ADJACENCY,
    VLE_SOURCE_RECOMMENDATION_KEEP_MATRIX_FRONTIER_BATCH,
    VLE_SOURCE_RECOMMENDATION_KEEP_EMPTY_BATCH,
    VLE_SOURCE_RECOMMENDATION_KEEP_EMPTY_LIFECYCLE,
    VLE_SOURCE_RECOMMENDATION_PREFER_AGE_ADJACENCY_DEPTH,
    VLE_SOURCE_RECOMMENDATION_PREFER_AGE_ADJACENCY_MATERIALIZATION,
    VLE_SOURCE_RECOMMENDATION_PREFER_AGE_ADJACENCY_UNDIRECTED,
    VLE_SOURCE_RECOMMENDATION_KEEP_AGE_ADJACENCY_UNDIRECTED,
    VLE_SOURCE_RECOMMENDATION_KEEP_DIRECTIONAL_SPLIT,
    VLE_SOURCE_RECOMMENDATION_KEEP_ENDPOINT_BTREE,
    VLE_SOURCE_RECOMMENDATION_KEEP_GLOBAL_METADATA,
    VLE_SOURCE_RECOMMENDATION_NO_CANDIDATES,
    VLE_SOURCE_RECOMMENDATION_KEEP_LOCAL_SOURCE,
    VLE_SOURCE_RECOMMENDATION_CHECK_AGE_ADJACENCY_DENSITY,
    VLE_SOURCE_RECOMMENDATION_KEEP_PACKED_SUPPRESSED,
    VLE_SOURCE_RECOMMENDATION_CHECK_FIXED_SOURCE_COVERAGE,
    VLE_SOURCE_RECOMMENDATION_OBSERVE
} VLESourcePolicyRecommendation;

typedef enum AgeVLEStreamCompositePlanKind
{
    AGE_VLE_STREAM_COMPOSITE_PLAN_UNKNOWN = 0,
    AGE_VLE_STREAM_COMPOSITE_PLAN_NONE,
    AGE_VLE_STREAM_COMPOSITE_PLAN_PROPERTY_PREFILTER,
    AGE_VLE_STREAM_COMPOSITE_PLAN_BELOW_THRESHOLD,
    AGE_VLE_STREAM_COMPOSITE_PLAN_METADATA_ONLY
} AgeVLEStreamCompositePlanKind;

typedef enum AgeVLEStreamCompositeSourceReason
{
    AGE_VLE_STREAM_COMPOSITE_REASON_NONE = 0,
    AGE_VLE_STREAM_COMPOSITE_REASON_MISSING_TERMINAL_LABEL,
    AGE_VLE_STREAM_COMPOSITE_REASON_PROPERTY_LABEL_UNKNOWN,
    AGE_VLE_STREAM_COMPOSITE_REASON_LABEL_MISMATCH,
    AGE_VLE_STREAM_COMPOSITE_REASON_TERMINAL_LABEL_PROPERTY,
    AGE_VLE_STREAM_COMPOSITE_REASON_ENDPOINT_LABEL_ACCEPTANCE
} AgeVLEStreamCompositeSourceReason;

typedef enum AgeGraphPropertySelectivitySource
{
    AGE_GRAPH_PROPERTY_SELECTIVITY_NONE = 0,
    AGE_GRAPH_PROPERTY_SELECTIVITY_FALLBACK,
    AGE_GRAPH_PROPERTY_SELECTIVITY_TYPED_MCV,
    AGE_GRAPH_PROPERTY_SELECTIVITY_FALLBACK_MCV_CEILING,
    AGE_GRAPH_PROPERTY_SELECTIVITY_TYPED_DISTINCT,
    AGE_GRAPH_PROPERTY_SELECTIVITY_TYPED_SPARSE,
    AGE_GRAPH_PROPERTY_SELECTIVITY_FALLBACK_CEILING
} AgeGraphPropertySelectivitySource;

typedef enum AgeVLEStreamTerminalValueKind
{
    AGE_VLE_STREAM_TERMINAL_VALUE_NONE = 0,
    AGE_VLE_STREAM_TERMINAL_VALUE_CONST,
    AGE_VLE_STREAM_TERMINAL_VALUE_NULL,
    AGE_VLE_STREAM_TERMINAL_VALUE_RUNTIME_SLOT
} AgeVLEStreamTerminalValueKind;

typedef enum VLESourcePolicyClass
{
    VLE_SOURCE_POLICY_CLASS_NONE = 0,
    VLE_SOURCE_POLICY_CLASS_ADJACENCY_WORK,
    VLE_SOURCE_POLICY_CLASS_ADJACENCY_FEEDBACK,
    VLE_SOURCE_POLICY_CLASS_ADJACENCY_CACHE_SEEDED,
    VLE_SOURCE_POLICY_CLASS_ADJACENCY_EMPTY_LIFECYCLE,
    VLE_SOURCE_POLICY_CLASS_ADJACENCY_EMPTY_BATCH,
    VLE_SOURCE_POLICY_CLASS_ADJACENCY_REPLAY,
    VLE_SOURCE_POLICY_CLASS_ADJACENCY_REPLAY_OBSERVED,
    VLE_SOURCE_POLICY_CLASS_ADJACENCY_PAYLOAD_SCAN,
    VLE_SOURCE_POLICY_CLASS_ADJACENCY_PAYLOAD,
    VLE_SOURCE_POLICY_CLASS_ADJACENCY_COMPOSITE_PREFILTER,
    VLE_SOURCE_POLICY_CLASS_ADJACENCY_COMPOSITE_VALUE_POSTING,
    VLE_SOURCE_POLICY_CLASS_MATRIX_FRONTIER_PRE_SCAN,
    VLE_SOURCE_POLICY_CLASS_LAYOUT,
    VLE_SOURCE_POLICY_CLASS_ADJACENCY_STREAM,
    VLE_SOURCE_POLICY_CLASS_ADJACENCY_MATERIALIZED_TIE,
    VLE_SOURCE_POLICY_CLASS_ADJACENCY_WORK_TIE,
    VLE_SOURCE_POLICY_CLASS_ADJACENCY_COMBINED_WORK_TIE,
    VLE_SOURCE_POLICY_CLASS_ADJACENCY_COMBINED_WORK,
    VLE_SOURCE_POLICY_CLASS_ADJACENCY_ONLY,
    VLE_SOURCE_POLICY_CLASS_ENDPOINT_DIRECT,
    VLE_SOURCE_POLICY_CLASS_NO_SOURCE,
    VLE_SOURCE_POLICY_CLASS_IDLE,
    VLE_SOURCE_POLICY_CLASS_MISSING_VERTEX_SOURCE,
    VLE_SOURCE_POLICY_CLASS_PACKED_POLICY_SUPPRESSED,
    VLE_SOURCE_POLICY_CLASS_PACKED_FALLBACK
} VLESourcePolicyClass;

typedef enum VLESourceConsumerClass
{
    VLE_SOURCE_CONSUMER_UNKNOWN = 0,
    VLE_SOURCE_CONSUMER_TERMINAL_SCALAR,
    VLE_SOURCE_CONSUMER_TERMINAL_OBJECT,
    VLE_SOURCE_CONSUMER_PATH_MATERIALIZED,
    VLE_SOURCE_CONSUMER_MATERIALIZED
} VLESourceConsumerClass;

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
    AGE_VLE_STREAM_EDGE_SOURCE_START_FANOUT_SOURCE_KIND,
    AGE_VLE_STREAM_EDGE_SOURCE_END_FANOUT_SOURCE_KIND,
    AGE_VLE_STREAM_EDGE_SOURCE_START_VALUE_POSTING_SOURCE_KIND,
    AGE_VLE_STREAM_EDGE_SOURCE_END_VALUE_POSTING_SOURCE_KIND,
    AGE_VLE_STREAM_EDGE_SOURCE_COST_POLICY,
    AGE_VLE_STREAM_EDGE_SOURCE_POLICY_OUTGOING_KIND,
    AGE_VLE_STREAM_EDGE_SOURCE_POLICY_INCOMING_KIND,
    AGE_VLE_STREAM_EDGE_SOURCE_POLICY_OUTPUT_REQUIREMENT,
    AGE_VLE_STREAM_EDGE_SOURCE_POLICY_CONSUMER_CLASS_KIND,
    AGE_VLE_STREAM_EDGE_SOURCE_POLICY_ACTIVE_DIRECTION_KIND,
    AGE_VLE_STREAM_EDGE_SOURCE_POLICY_FANOUT_BUDGET,
    AGE_VLE_STREAM_EDGE_SOURCE_POLICY_MATERIALIZATION_WEIGHT,
    AGE_VLE_STREAM_EDGE_SOURCE_POLICY_CLASS_KIND,
    AGE_VLE_STREAM_EDGE_SOURCE_POLICY_RECOMMENDATION_KIND,
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
    AGE_VLE_STREAM_EDGE_SOURCE_THRESHOLD_INPUT_DIRECTION_KIND,
    AGE_VLE_STREAM_EDGE_SOURCE_THRESHOLD_INPUT_REASON_KIND,
    AGE_VLE_STREAM_EDGE_SOURCE_THRESHOLD_INPUT_CLASS_KIND,
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
    AGE_VLE_STREAM_EDGE_SOURCE_PAYLOAD_INPUT_MATRIX_COMPACTION_PERCENT,
    AGE_VLE_STREAM_EDGE_SOURCE_PAYLOAD_INPUT_MATRIX_DENSE_PERCENT,
    AGE_VLE_STREAM_EDGE_SOURCE_PAYLOAD_INPUT_MATRIX_DUPLICATE_PERCENT,
    AGE_VLE_STREAM_EDGE_SOURCE_PAYLOAD_INPUT_MATRIX_COMPACTION_PRESSURE_PERCENT,
    AGE_VLE_STREAM_EDGE_SOURCE_PAYLOAD_INPUT_MATRIX_RESIDENCY_PRESSURE_PERCENT,
    AGE_VLE_STREAM_EDGE_SOURCE_PAYLOAD_INPUT_MATRIX_REPLAY_CUTOVER_PERCENT,
    AGE_VLE_STREAM_EDGE_SOURCE_PAYLOAD_INPUT_MATRIX_PREFETCH_CUTOVER_PERCENT,
    AGE_VLE_STREAM_EDGE_SOURCE_PAYLOAD_INPUT_MATRIX_PAYLOAD_COVERAGE_PERCENT,
    AGE_VLE_STREAM_EDGE_SOURCE_PAYLOAD_INPUT_MATRIX_SOURCE_OVERLAP_PERCENT,
    AGE_VLE_STREAM_EDGE_SOURCE_PAYLOAD_INPUT_MATRIX_REUSE_PROXIMITY_PERCENT,
    AGE_VLE_STREAM_EDGE_SOURCE_OUT_MATRIX_DENSE_PERCENT,
    AGE_VLE_STREAM_EDGE_SOURCE_OUT_MATRIX_DUPLICATE_PERCENT,
    AGE_VLE_STREAM_EDGE_SOURCE_OUT_MATRIX_COMPACTION_PRESSURE_PERCENT,
    AGE_VLE_STREAM_EDGE_SOURCE_OUT_MATRIX_RESIDENCY_PRESSURE_PERCENT,
    AGE_VLE_STREAM_EDGE_SOURCE_OUT_MATRIX_REPLAY_PERCENT,
    AGE_VLE_STREAM_EDGE_SOURCE_OUT_MATRIX_PREFETCH_PERCENT,
    AGE_VLE_STREAM_EDGE_SOURCE_OUT_MATRIX_PAYLOAD_COVERAGE_PERCENT,
    AGE_VLE_STREAM_EDGE_SOURCE_OUT_MATRIX_SOURCE_OVERLAP_PERCENT,
    AGE_VLE_STREAM_EDGE_SOURCE_OUT_MATRIX_REUSE_PROXIMITY_PERCENT,
    AGE_VLE_STREAM_EDGE_SOURCE_IN_MATRIX_DENSE_PERCENT,
    AGE_VLE_STREAM_EDGE_SOURCE_IN_MATRIX_DUPLICATE_PERCENT,
    AGE_VLE_STREAM_EDGE_SOURCE_IN_MATRIX_COMPACTION_PRESSURE_PERCENT,
    AGE_VLE_STREAM_EDGE_SOURCE_IN_MATRIX_RESIDENCY_PRESSURE_PERCENT,
    AGE_VLE_STREAM_EDGE_SOURCE_IN_MATRIX_REPLAY_PERCENT,
    AGE_VLE_STREAM_EDGE_SOURCE_IN_MATRIX_PREFETCH_PERCENT,
    AGE_VLE_STREAM_EDGE_SOURCE_IN_MATRIX_PAYLOAD_COVERAGE_PERCENT,
    AGE_VLE_STREAM_EDGE_SOURCE_IN_MATRIX_SOURCE_OVERLAP_PERCENT,
    AGE_VLE_STREAM_EDGE_SOURCE_IN_MATRIX_REUSE_PROXIMITY_PERCENT,
    AGE_VLE_STREAM_EDGE_SOURCE_PAYLOAD_INPUT_REASON_KIND,
    AGE_VLE_STREAM_EDGE_SOURCE_PAYLOAD_INPUT_CLASS_KIND,
    AGE_VLE_STREAM_EDGE_SOURCE_PAYLOAD_INPUT_VALUE_POSTING_SOURCE_KIND,
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
    AGE_VLE_STREAM_EDGE_SOURCE_COMPOSITE_SOURCE_REASON_KIND,
    AGE_VLE_STREAM_EDGE_SOURCE_COMPOSITE_SOURCE_PROPERTY_TUPLES,
    AGE_VLE_STREAM_EDGE_SOURCE_COMPOSITE_SOURCE_CANDIDATE_FANOUT,
    AGE_VLE_STREAM_EDGE_SOURCE_COMPOSITE_SOURCE_FANOUT,
    AGE_VLE_STREAM_EDGE_SOURCE_COMPOSITE_SOURCE_SELECTIVITY_PPM,
    AGE_VLE_STREAM_EDGE_SOURCE_COMPOSITE_SOURCE_SELECTIVITY_SOURCE_KIND,
    AGE_VLE_STREAM_EDGE_SOURCE_COMPOSITE_SOURCE_PLANNED_KIND,
    AGE_VLE_STREAM_EDGE_SOURCE_TERMINAL_PROPERTY_PREDICATE_KNOWN,
    AGE_VLE_STREAM_EDGE_SOURCE_TERMINAL_PROPERTY_PREDICATE_KEY,
    AGE_VLE_STREAM_EDGE_SOURCE_TERMINAL_PROPERTY_PREDICATE_NULL,
    AGE_VLE_STREAM_EDGE_SOURCE_TERMINAL_PROPERTY_PREDICATE_VALUE,
    AGE_VLE_STREAM_EDGE_SOURCE_TERMINAL_PROPERTY_VALUE_KIND_ID,
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
    AgeVLEStreamFanoutSourceKind start_fanout_source_kind;
    AgeVLEStreamFanoutSourceKind end_fanout_source_kind;
    VLESourceValuePostingKind start_value_posting_source_kind;
    VLESourceValuePostingKind end_value_posting_source_kind;
    char *cost_policy;
    AgeVLEStreamDirectedSourceKind policy_outgoing_kind;
    AgeVLEStreamDirectedSourceKind policy_incoming_kind;
    AgeVLEOutputRequirement policy_output_requirement;
    VLESourceConsumerClass policy_consumer_class_id;
    VLESourceDirectionClass policy_active_direction_id;
    int64 policy_fanout_budget;
    int64 policy_materialization_weight;
    VLESourcePolicyClass policy_class_id;
    VLESourcePolicyRecommendation policy_recommendation_kind;
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
    VLESourceDirectionClass threshold_input_direction_id;
    VLESourceThresholdFeedbackReason threshold_input_reason_id;
    VLESourcePolicyClass threshold_input_class_id;
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
    int64 payload_input_matrix_compaction_percent;
    int64 payload_input_matrix_dense_percent;
    int64 payload_input_matrix_duplicate_percent;
    int64 payload_input_matrix_compaction_pressure_percent;
    int64 payload_input_matrix_residency_pressure_percent;
    int64 payload_input_matrix_replay_cutover_percent;
    int64 payload_input_matrix_prefetch_cutover_percent;
    int64 payload_input_matrix_payload_coverage_percent;
    int64 payload_input_matrix_source_overlap_percent;
    int64 payload_input_matrix_reuse_proximity_percent;
    AgeVLEMatrixFrontierCursorPolicy outgoing_matrix_cursor_policy;
    AgeVLEMatrixFrontierCursorPolicy incoming_matrix_cursor_policy;
    VLESourcePayloadFeedbackReason payload_input_reason_id;
    VLESourcePolicyClass payload_input_class_id;
    VLESourceValuePostingKind payload_input_value_posting_source_kind;
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
    AgeVLEStreamCompositeSourceReason composite_source_reason_kind;
    int64 composite_source_property_tuples;
    int64 composite_source_candidate_fanout;
    int64 composite_source_fanout;
    int64 composite_source_selectivity_ppm;
    AgeGraphPropertySelectivitySource composite_source_selectivity_source_kind;
    AgeVLEStreamCompositePlanKind composite_source_planned_kind;
    bool terminal_property_predicate_known;
    char *terminal_property_predicate_key;
    bool terminal_property_predicate_null;
    Datum terminal_property_predicate_value;
    AgeVLEStreamTerminalValueKind terminal_property_value_kind_id;
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
const char *age_vle_stream_fanout_source_name(
    AgeVLEStreamFanoutSourceKind source);
bool age_vle_stream_fanout_source_is_directory_label(
    AgeVLEStreamFanoutSourceKind source);
const char *age_vle_stream_composite_plan_name(
    AgeVLEStreamCompositePlanKind plan);
bool age_vle_stream_composite_plan_is_property_prefilter(
    AgeVLEStreamCompositePlanKind plan);
const char *age_vle_stream_terminal_value_kind_name(
    AgeVLEStreamTerminalValueKind kind);
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
