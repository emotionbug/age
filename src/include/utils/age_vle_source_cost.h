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
} VLEStreamSourceCostInput;

typedef struct VLEStreamSourceCostDecision
{
    AgeVLEStreamDirectedSourceKind outgoing_kind;
    AgeVLEStreamDirectedSourceKind incoming_kind;
    char *policy_text;
    const char *policy_consumer;
    const char *policy_consumer_class;
    const char *policy_active_direction;
    int64 policy_fanout_budget;
    int64 policy_materialization_weight;
    const char *policy_class;
    const char *policy_recommendation;
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
    const char *threshold_input_source;
    const char *threshold_input_reason;
    bool payload_input_known;
    int64 payload_input_headroom_percent;
    int64 payload_input_scan_runs;
    int64 payload_input_replay_runs;
    int64 payload_input_seed_runs;
    int64 payload_input_replay_percent;
    int64 payload_input_seed_percent;
    int64 payload_input_observed_count;
    const char *payload_input_reason;
} VLEStreamSourceCostDecision;

typedef struct VLESourceRuntimeThresholdFeedback
{
    bool eligible;
    bool saturated;
    int64 endpoint_headroom_percent;
    int64 empty_lifecycle_batch_size;
    int64 root_empty_completion_count;
    int64 observed_count;
    int64 saturated_count;
    int64 relaxed_count;
    int64 scan_runs;
    int64 replay_runs;
    int64 seed_runs;
    int64 replay_percent;
    int64 seed_percent;
    int64 payload_observed_count;
    int64 payload_endpoint_headroom_percent;
    const char *source_direction;
    const char *reason;
    const char *payload_reason;
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
extern char *format_vle_stream_edge_source_evidence(
    AgeVLEStreamEdgeSource *source);
extern char *format_vle_source_runtime_evidence(
    const AgeVLESourceStats *stats, const AgeVLEStreamEdgeSource *source);

#endif
