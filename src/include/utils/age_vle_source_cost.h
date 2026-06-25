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

#ifndef AGE_VLE_SOURCE_COST_H
#define AGE_VLE_SOURCE_COST_H

#include "postgres.h"

#include "access/attnum.h"
#include "executor/cypher_vle_stream.h"
#include "utils/age_vle_root.h"
#include "utils/age_vle.h"

typedef struct VLESourceFanoutEvidence
{
    Oid edge_label_oid;
    bool relation_tuples_known;
    bool start_fanout_known;
    bool end_fanout_known;
    double reltuples;
    double start_fanout;
    double end_fanout;
    AgeVLEStreamFanoutSourceKind start_fanout_source_kind;
    AgeVLEStreamFanoutSourceKind end_fanout_source_kind;
    VLESourceValuePostingKind start_value_posting_source_kind;
    VLESourceValuePostingKind end_value_posting_source_kind;
} VLESourceFanoutEvidence;

typedef struct VLEStreamSourceCostInput
{
    AgeVLEStreamEdgeSourceKind source_kind;
    AgeVLEStreamDirectedSourceKind outgoing_kind;
    AgeVLEStreamDirectedSourceKind incoming_kind;
    const char *graph_name;
    const char *label_name;
    const VLESourceFanoutEvidence *evidence;
    int64 upper;
    bool upper_infinite;
    cypher_rel_dir direction;
    AgeVLEOutputRequirement output_requirement;
    bool has_property_constraints;
    bool endpoint_start;
    bool endpoint_end;
    bool age_adjacency_out;
    bool age_adjacency_in;
    bool start_fanout_known;
    bool end_fanout_known;
    bool composite_prefilter_planned;
    int32 terminal_label_id;
    Oid terminal_property_index_oid;
    uint32 terminal_property_filter_id;
    int64 composite_candidate_fanout;
    int64 composite_fanout;
} VLEStreamSourceCostInput;

typedef struct VLEStreamSourceCostDecision
{
    AgeVLEStreamDirectedSourceKind outgoing_kind;
    AgeVLEStreamDirectedSourceKind incoming_kind;
    char *policy_text;
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
    int64 threshold_input_out_observed_count;
    int64 threshold_input_in_observed_count;
    int64 threshold_input_out_saturated_count;
    int64 threshold_input_in_saturated_count;
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
    bool composite_prefilter_planned;
    int64 composite_candidate_fanout;
    int64 composite_fanout;
} VLEStreamSourceCostDecision;

typedef struct VLESourceRuntimeThresholdFeedback
{
    bool eligible;
    bool saturated;
    int64 endpoint_headroom_percent;
    int64 empty_lifecycle_batch_size;
    int64 root_empty_completion_count;
    int64 root_empty_completion_out;
    int64 root_empty_completion_in;
    int64 observed_count;
    int64 saturated_count;
    int64 relaxed_count;
    int64 out_observed_count;
    int64 in_observed_count;
    int64 out_saturated_count;
    int64 in_saturated_count;
    int64 scan_runs;
    int64 replay_runs;
    int64 seed_runs;
    int64 replay_percent;
    int64 seed_percent;
    int64 payload_observed_count;
    int64 payload_endpoint_headroom_percent;
    int64 payload_value_posting_observed_count;
    int64 payload_matrix_active_percent;
    int64 payload_matrix_filter_percent;
    int64 payload_matrix_prescan_percent;
    int64 payload_matrix_replay_source_percent;
    int64 payload_matrix_run_block_percent;
    int64 payload_matrix_regroup_percent;
    int64 payload_matrix_compaction_percent;
    int64 payload_matrix_dense_percent;
    int64 payload_matrix_duplicate_percent;
    int64 payload_matrix_compaction_pressure_percent;
    int64 payload_matrix_residency_pressure_percent;
    int64 payload_matrix_replay_cutover_percent;
    int64 payload_matrix_prefetch_cutover_percent;
    int64 payload_matrix_payload_coverage_percent;
    int64 payload_matrix_source_overlap_percent;
    int64 payload_matrix_reuse_proximity_percent;
    VLESourceDirectionClass source_direction_id;
    VLESourceThresholdFeedbackReason reason_id;
    VLESourcePolicyClass feedback_class_id;
    VLESourcePayloadFeedbackReason payload_reason_id;
    VLESourcePolicyClass payload_class_id;
    VLESourceValuePostingKind payload_value_posting_source_kind;
} VLESourceRuntimeThresholdFeedback;

extern void estimate_vle_source_fanout_evidence(
    VLESourceFanoutEvidence *evidence, Oid edge_label_oid);
extern double select_vle_source_fanout_for_direction(
    const VLESourceFanoutEvidence *evidence,
    const VLETraversalSourceIndexes *indexes, cypher_rel_dir direction);
extern double select_vle_source_fanout_for_endpoint(
    const VLESourceFanoutEvidence *evidence, AttrNumber endpoint_attno);
extern int64 round_vle_source_cost_evidence(double value);
extern void choose_vle_stream_source_cost_decision(
    VLEStreamSourceCostDecision *decision,
    const VLEStreamSourceCostInput *input);
extern void derive_vle_source_runtime_threshold_feedback(
    VLESourceRuntimeThresholdFeedback *feedback,
    const AgeVLESourceStats *stats, const AgeVLEStreamEdgeSource *source);
extern void record_vle_source_runtime_threshold_feedback(
    const char *graph_name, const char *label_name,
    const AgeVLESourceStats *stats, const AgeVLEStreamEdgeSource *source);
extern double estimate_vle_edge_endpoint_fanout(
    Oid edge_label_oid, AttrNumber endpoint_attno, double reltuples);
extern double get_vle_relation_estimated_tuples(Oid relation_oid);
extern const char *age_vle_output_requirement_name(
    AgeVLEOutputRequirement requirement);
extern const char *age_vle_value_posting_source_name(
    VLESourceValuePostingKind source);
extern const char *age_vle_source_policy_class_name(
    VLESourcePolicyClass source_class);
extern const char *age_vle_source_consumer_class_name(
    VLESourceConsumerClass consumer_class);
extern const char *age_vle_source_direction_name(
    VLESourceDirectionClass direction);
extern const char *age_vle_threshold_feedback_reason_name(
    VLESourceThresholdFeedbackReason reason);
extern const char *age_vle_payload_feedback_reason_name(
    VLESourcePayloadFeedbackReason reason);
extern const char *age_vle_source_policy_recommendation_name(
    VLESourcePolicyRecommendation recommendation);
extern const char *age_vle_stream_composite_source_reason_name(
    AgeVLEStreamCompositeSourceReason reason);
extern bool age_vle_stream_composite_source_reason_is_eligible(
    AgeVLEStreamCompositeSourceReason reason);
extern const char *age_graph_property_selectivity_source_name(
    AgeGraphPropertySelectivitySource source);
extern char *format_vle_stream_edge_source_evidence(
    AgeVLEStreamEdgeSource *source);
extern char *format_vle_stream_edge_source_cost(
    AgeVLEStreamEdgeSource *source);
extern char *format_vle_stream_edge_terminal_property_source(
    AgeVLEStreamEdgeSource *source);
extern char *format_vle_stream_edge_composite_source(
    AgeVLEStreamEdgeSource *source);
extern char *format_vle_stream_edge_composite_fanout(
    AgeVLEStreamEdgeSource *source);
extern char *format_vle_stream_edge_source_profile(
    AgeVLEStreamEdgeSource *source);
extern char *format_vle_stream_edge_source_threshold_input(
    AgeVLEStreamEdgeSource *source);
extern char *format_vle_stream_edge_source_payload_input(
    AgeVLEStreamEdgeSource *source);
extern char *format_vle_stream_edge_source_policy(
    AgeVLEStreamEdgeSource *source);
extern char *format_vle_source_runtime_evidence(
    const AgeVLESourceStats *stats, const AgeVLEStreamEdgeSource *source);
extern char *format_vle_source_runtime_plan(
    const AgeVLESourceStats *stats, const AgeVLEStreamEdgeSource *source);
extern char *format_vle_source_runtime_counters(
    const AgeVLESourceStats *stats, const AgeVLEStreamEdgeSource *source);
extern char *format_vle_source_runtime_payload(
    const AgeVLESourceStats *stats, const AgeVLEStreamEdgeSource *source);
extern char *format_vle_source_runtime_empty_evidence(
    const AgeVLESourceStats *stats, const AgeVLEStreamEdgeSource *source);
extern char *format_vle_source_runtime_empty_lifecycle(
    const AgeVLESourceStats *stats, const AgeVLEStreamEdgeSource *source);
extern char *format_vle_source_runtime_feedback(
    const AgeVLESourceStats *stats, const AgeVLEStreamEdgeSource *source);

#endif
