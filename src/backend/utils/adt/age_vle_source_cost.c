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

#include "access/htup_details.h"
#include "catalog/ag_label.h"
#include "catalog/pg_class.h"
#include "catalog/pg_statistic.h"
#include "utils/age_vle_source_cost.h"
#include "utils/builtins.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/syscache.h"

typedef enum VLESourceRuntimeDominantKind
{
    VLE_SOURCE_RUNTIME_DOMINANT_NONE = 0,
    VLE_SOURCE_RUNTIME_DOMINANT_AGE_ADJACENCY,
    VLE_SOURCE_RUNTIME_DOMINANT_ENDPOINT_BTREE,
    VLE_SOURCE_RUNTIME_DOMINANT_PACKED
} VLESourceRuntimeDominantKind;

typedef struct VLESourceRuntimeDominant
{
    VLESourceRuntimeDominantKind kind;
    int64 scans;
    int64 candidates;
} VLESourceRuntimeDominant;

typedef enum VLESourcePolicyReason
{
    VLE_SOURCE_POLICY_REASON_LAYOUT = 0,
    VLE_SOURCE_POLICY_REASON_INACTIVE_DIRECTION,
    VLE_SOURCE_POLICY_REASON_NO_SOURCE,
    VLE_SOURCE_POLICY_REASON_COMPOSITE_PREFILTER,
    VLE_SOURCE_POLICY_REASON_COMPOSITE_VALUE_POSTING,
    VLE_SOURCE_POLICY_REASON_ENDPOINT_HEADROOM,
    VLE_SOURCE_POLICY_REASON_ENDPOINT_WORK,
    VLE_SOURCE_POLICY_REASON_ENDPOINT_ONLY,
    VLE_SOURCE_POLICY_REASON_ADJACENCY_ONLY,
    VLE_SOURCE_POLICY_REASON_UNKNOWN_FANOUT,
    VLE_SOURCE_POLICY_REASON_WORK_TIE,
    VLE_SOURCE_POLICY_REASON_EMPTY_LIFECYCLE_HEADROOM,
    VLE_SOURCE_POLICY_REASON_CACHE_SEED_HEADROOM,
    VLE_SOURCE_POLICY_REASON_WORK_EXCEEDS_LIMIT,
    VLE_SOURCE_POLICY_REASON_COMBINED_WORK_TIE,
    VLE_SOURCE_POLICY_REASON_COMBINED_WORK_EXCEEDS_LIMIT,
    VLE_SOURCE_POLICY_REASON_DIRECTIONAL_FAMILY_PRODUCTIVE
} VLESourcePolicyReason;

typedef enum VLESourceEmptyEvidenceKind
{
    VLE_SOURCE_EMPTY_EVIDENCE_NONE = 0,
    VLE_SOURCE_EMPTY_EVIDENCE_RUN,
    VLE_SOURCE_EMPTY_EVIDENCE_FRONTIER,
    VLE_SOURCE_EMPTY_EVIDENCE_CACHE,
    VLE_SOURCE_EMPTY_EVIDENCE_COMPLETE,
    VLE_SOURCE_EMPTY_EVIDENCE_DIRECTORY_FILTER,
    VLE_SOURCE_EMPTY_EVIDENCE_EMPTY_SCAN,
    VLE_SOURCE_EMPTY_EVIDENCE_ENDPOINT_EMPTY_SCAN
} VLESourceEmptyEvidenceKind;

typedef enum VLESourceRuntimePressureKind
{
    VLE_SOURCE_RUNTIME_PRESSURE_STABLE = 0,
    VLE_SOURCE_RUNTIME_PRESSURE_ADJACENCY_COMPOSITE_VALUE_POSTING,
    VLE_SOURCE_RUNTIME_PRESSURE_ADJACENCY_COMPOSITE_PREFILTER,
    VLE_SOURCE_RUNTIME_PRESSURE_IDLE,
    VLE_SOURCE_RUNTIME_PRESSURE_SOURCE_MISMATCH,
    VLE_SOURCE_RUNTIME_PRESSURE_CLASS_MISMATCH,
    VLE_SOURCE_RUNTIME_PRESSURE_MATERIALIZATION_TIE,
    VLE_SOURCE_RUNTIME_PRESSURE_ADJACENCY_PAYLOAD_REPLAY,
    VLE_SOURCE_RUNTIME_PRESSURE_ADJACENCY_EMPTY_PROBE,
    VLE_SOURCE_RUNTIME_PRESSURE_ADJACENCY_EMPTY_RUN,
    VLE_SOURCE_RUNTIME_PRESSURE_ADJACENCY_EMPTY_RUN_BATCH,
    VLE_SOURCE_RUNTIME_PRESSURE_ADJACENCY_EMPTY_FRONTIER,
    VLE_SOURCE_RUNTIME_PRESSURE_ADJACENCY_EMPTY_FRONTIER_BATCH,
    VLE_SOURCE_RUNTIME_PRESSURE_ADJACENCY_EMPTY_CACHE,
    VLE_SOURCE_RUNTIME_PRESSURE_ADJACENCY_EMPTY_COMPLETE,
    VLE_SOURCE_RUNTIME_PRESSURE_ADJACENCY_EMPTY_SUPPRESSED,
    VLE_SOURCE_RUNTIME_PRESSURE_CACHE_SEED_MISSED,
    VLE_SOURCE_RUNTIME_PRESSURE_ADJACENCY_DENSITY_LOW,
    VLE_SOURCE_RUNTIME_PRESSURE_ENDPOINT_FANOUT
} VLESourceRuntimePressureKind;

typedef struct VLESourceRuntimeFeedback
{
    VLESourcePolicyClass source_class_id;
    VLESourcePolicyRecommendation recommendation_kind;
} VLESourceRuntimeFeedback;

typedef struct VLESourceRuntimePressure
{
    VLESourceRuntimePressureKind kind;
    VLESourceDirectionClass direction_id;
} VLESourceRuntimePressure;

typedef struct VLESourceRuntimeSuppression
{
    VLESourceDirectionClass direction_id;
    bool planned_match;
} VLESourceRuntimeSuppression;

typedef struct VLESourceRuntimeExplain
{
    VLESourceRuntimeDominant dominant;
    VLESourceRuntimeFeedback feedback;
    VLESourceRuntimePressure pressure;
    VLESourceRuntimeSuppression suppression;
    VLESourceRuntimeThresholdFeedback threshold_feedback;
    double age_adjacency_density;
    double endpoint_btree_density;
    double packed_density;
    VLESourcePolicyClass planned_class_id;
    VLESourcePolicyRecommendation planned_recommendation_kind;
} VLESourceRuntimeExplain;

typedef struct VLESourcePolicyDecision
{
    AgeVLEStreamDirectedSourceKind kind;
    double endpoint_work;
    double limit_work;
    double combined_endpoint_work;
    double combined_limit_work;
    double composite_work;
    bool composite_prefilter_active;
    VLESourcePolicyReason reason;
} VLESourcePolicyDecision;

typedef struct VLESourcePolicyFeedback
{
    VLESourcePolicyClass source_class_id;
    VLESourcePolicyRecommendation recommendation_kind;
} VLESourcePolicyFeedback;

typedef struct VLESourcePolicyProfile
{
    AgeVLEOutputRequirement output_requirement;
    VLESourceConsumerClass consumer_class_id;
    VLESourceDirectionClass direction_class_id;
    double fanout_budget;
    int64 materialization_weight;
    double cache_seed_endpoint_headroom;
    double outgoing_endpoint_headroom;
    double incoming_endpoint_headroom;
    int64 depth;
    int64 empty_lifecycle_batch_size;
    bool cost_eligible;
    bool cache_seed_eligible;
    bool empty_lifecycle_eligible;
    bool outgoing_active;
    bool incoming_active;
    bool composite_prefilter_planned;
    int64 composite_candidate_fanout;
    int64 composite_fanout;
    VLESourceValuePostingKind out_value_posting_source_kind;
    VLESourceValuePostingKind in_value_posting_source_kind;
    bool threshold_input_known;
    bool threshold_directional_family;
    bool threshold_split_out;
    bool threshold_split_in;
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
    VLESourcePayloadFeedbackReason payload_input_reason_id;
    VLESourcePolicyClass payload_input_class_id;
    VLESourceValuePostingKind payload_input_value_posting_source_kind;
} VLESourcePolicyProfile;

typedef struct VLESourceRuntimePayloadFeedback
{
    bool eligible;
    int64 endpoint_headroom_percent;
    int64 scan_runs;
    int64 replay_runs;
    int64 seed_runs;
    int64 replay_percent;
    int64 seed_percent;
    int64 value_posting_observed_count;
    int64 matrix_active_percent;
    int64 matrix_filter_percent;
    int64 matrix_prescan_percent;
    int64 matrix_replay_source_percent;
    int64 matrix_run_block_percent;
    int64 matrix_regroup_percent;
    VLESourcePayloadFeedbackReason reason_id;
    VLESourcePolicyClass feedback_class_id;
    VLESourceValuePostingKind value_posting_source_kind;
} VLESourceRuntimePayloadFeedback;

typedef struct VLESourceThresholdCacheKey
{
    char graph_name[NAMEDATALEN];
    char label_name[NAMEDATALEN];
    Oid edge_label_oid;
    int32 terminal_label_id;
    Oid terminal_property_index_oid;
    uint32 terminal_property_filter_id;
    VLESourceConsumerClass consumer_class_id;
    VLESourceDirectionClass active_direction_id;
    VLESourceValuePostingKind value_posting_source_kind;
} VLESourceThresholdCacheKey;

typedef struct VLESourceThresholdCacheEntry
{
    VLESourceThresholdCacheKey key;
    int64 endpoint_headroom_percent;
    int64 empty_lifecycle_batch_size;
    int64 observed_count;
    int64 saturated_count;
    int64 relaxed_count;
    int64 payload_endpoint_headroom_percent;
    int64 payload_scan_runs;
    int64 payload_replay_runs;
    int64 payload_seed_runs;
    int64 payload_replay_percent;
    int64 payload_seed_percent;
    int64 payload_observed_count;
    int64 payload_value_posting_observed_count;
    int64 payload_matrix_active_percent;
    int64 payload_matrix_filter_percent;
    int64 payload_matrix_prescan_percent;
    int64 payload_matrix_replay_source_percent;
    int64 payload_matrix_run_block_percent;
    int64 payload_matrix_regroup_percent;
    int64 directional_empty_completion_count;
    VLESourceDirectionClass source_direction_id;
    VLESourceThresholdFeedbackReason reason_id;
    VLESourcePolicyClass feedback_class_id;
    VLESourcePayloadFeedbackReason payload_reason_id;
    VLESourcePolicyClass payload_class_id;
    VLESourceValuePostingKind payload_value_posting_source_kind;
} VLESourceThresholdCacheEntry;

#define VLE_ENDPOINT_BTREE_TERMINAL_FANOUT 2.0
#define VLE_ENDPOINT_BTREE_TERMINAL_OBJECT_FANOUT 1.0
#define VLE_ENDPOINT_BTREE_PATH_FANOUT 1.0
#define VLE_CACHE_SEED_ENDPOINT_HEADROOM 0.75
#define VLE_EMPTY_LIFECYCLE_ENDPOINT_HEADROOM 0.50
#define VLE_EMPTY_LIFECYCLE_BATCH_ENDPOINT_HEADROOM 0.35
#define VLE_EMPTY_LIFECYCLE_REPEAT_ENDPOINT_HEADROOM 0.30
#define VLE_EMPTY_LIFECYCLE_REPEAT_SATURATED_ENDPOINT_HEADROOM 0.25
#define VLE_EMPTY_LIFECYCLE_DIRECTIONAL_FAMILY_ENDPOINT_HEADROOM 0.40
#define VLE_EMPTY_LIFECYCLE_BATCH_MIN 8
#define VLE_EMPTY_LIFECYCLE_BATCH_STRONG 12
#define VLE_EMPTY_LIFECYCLE_BATCH_MAX 1024
#define VLE_EMPTY_LIFECYCLE_BATCH_RELAX_MULTIPLIER 2
#define VLE_PAYLOAD_SEED_ENDPOINT_HEADROOM 0.50
#define VLE_PAYLOAD_SEED_TERMINAL_SCALAR_ENDPOINT_HEADROOM 0.65
#define VLE_PAYLOAD_REPLAY_ENDPOINT_HEADROOM 0.35
#define VLE_PAYLOAD_REPLAY_OBJECT_STRONG_ENDPOINT_HEADROOM 0.20
#define VLE_PAYLOAD_REPLAY_PATH_STRONG_ENDPOINT_HEADROOM 0.18
#define VLE_PAYLOAD_REPLAY_STRONG_ENDPOINT_HEADROOM 0.25
#define VLE_PAYLOAD_VALUE_POSTING_OBJECT_ENDPOINT_HEADROOM 0.20
#define VLE_PAYLOAD_VALUE_POSTING_PATH_ENDPOINT_HEADROOM 0.18
#define VLE_PAYLOAD_VALUE_POSTING_ENDPOINT_HEADROOM 0.25
#define VLE_PAYLOAD_REPLAY_STRONG_PERCENT 25
#define VLE_PAYLOAD_REPLAY_TERMINAL_SCALAR_STRONG_PERCENT 40
#define VLE_MATERIALIZATION_WEIGHT_NONE 0
#define VLE_MATERIALIZATION_WEIGHT_SCALAR 1
#define VLE_MATERIALIZATION_WEIGHT_OBJECT 2
#define VLE_MATERIALIZATION_WEIGHT_PATH 3

static const char *age_vle_stream_source_kind_name(
    AgeVLEStreamEdgeSourceKind kind);
static const char *age_vle_directed_source_kind_name(
    AgeVLEStreamDirectedSourceKind kind);
static void choose_vle_source_policy_decision(
    VLESourcePolicyDecision *decision,
    AgeVLEStreamDirectedSourceKind current_kind, double fanout,
    bool fanout_known, bool endpoint_available, bool age_adjacency_available,
    bool direction_active, bool composite_prefilter_active,
    double endpoint_headroom,
    const VLESourcePolicyProfile *profile);
static void apply_vle_source_combined_policy(
    VLESourcePolicyDecision *out_policy,
    VLESourcePolicyDecision *in_policy,
    const VLEStreamSourceCostInput *input,
    const VLESourcePolicyProfile *profile);
static void apply_vle_source_directional_family_policy(
    VLESourcePolicyDecision *out_policy,
    VLESourcePolicyDecision *in_policy,
    const VLEStreamSourceCostInput *input,
    const VLESourcePolicyProfile *profile);
static double estimate_vle_source_policy_work(double fanout, int64 depth);
static int64 estimate_vle_empty_lifecycle_batch_size(
    const VLESourcePolicyProfile *profile);
static int64 select_vle_payload_replay_batch_size(
    const VLESourcePolicyProfile *profile, int64 replay_percent);
static double select_vle_empty_lifecycle_endpoint_headroom(
    const VLESourcePolicyProfile *profile);
static void build_vle_source_policy_profile(
    VLESourcePolicyProfile *profile, const VLEStreamSourceCostInput *input);
static bool estimate_vle_edge_endpoint_fanout_with_state(
    double *fanout, Oid edge_label_oid, AttrNumber endpoint_attno,
    double reltuples);
static bool vle_source_endpoint_work_within_policy(
    const VLESourcePolicyDecision *decision,
    bool age_adjacency_available,
    double endpoint_headroom,
    const VLESourcePolicyProfile *profile);
static bool vle_source_policy_prefers_endpoint_tie(
    bool age_adjacency_available, const VLESourcePolicyProfile *profile);
static bool vle_source_profile_has_value_posting_payload(
    const VLESourcePolicyProfile *profile);
static void set_vle_source_policy_reason(
    VLESourcePolicyDecision *decision, VLESourcePolicyReason reason);
static const char *vle_source_policy_reason_text(
    VLESourcePolicyReason reason);
static bool vle_source_policy_reason_is_composite(
    VLESourcePolicyReason reason);
static bool vle_source_policy_has_reason(
    const VLESourcePolicyDecision *out_policy,
    const VLESourcePolicyDecision *in_policy,
    VLESourcePolicyReason reason);
static char *format_vle_stream_source_cost_policy(
    const VLEStreamSourceCostDecision *decision,
    const VLESourcePolicyDecision *out_policy,
    const VLESourcePolicyDecision *in_policy,
    const VLESourcePolicyProfile *profile);
static void choose_vle_source_policy_feedback(
    VLESourcePolicyFeedback *feedback,
    const VLESourcePolicyDecision *out_policy,
    const VLESourcePolicyDecision *in_policy,
    const VLESourcePolicyProfile *profile);
static double calculate_vle_source_scan_density(int64 candidates,
                                                int64 scans);
static void choose_vle_source_runtime_dominant(
    const AgeVLESourceStats *stats, VLESourceRuntimeDominant *dominant);
static const char *age_vle_source_runtime_dominant_name(
    VLESourceRuntimeDominantKind kind);
static void choose_vle_source_runtime_feedback(
    const AgeVLESourceStats *stats,
    const VLESourceRuntimeDominant *dominant,
    const AgeVLEStreamEdgeSource *source,
    VLESourceRuntimeFeedback *feedback);
static void choose_vle_source_runtime_pressure(
    const AgeVLESourceStats *stats,
    const VLESourceRuntimeDominant *dominant,
    const AgeVLEStreamEdgeSource *source,
    const VLESourceRuntimeFeedback *feedback,
    VLESourceRuntimePressure *pressure);
static void choose_vle_source_runtime_suppression(
    const AgeVLESourceStats *stats,
    const AgeVLEStreamEdgeSource *source,
    VLESourceRuntimeSuppression *suppression);
static void build_vle_source_runtime_explain(
    VLESourceRuntimeExplain *explain, const AgeVLESourceStats *stats,
    const AgeVLEStreamEdgeSource *source);
static const char *age_vle_source_runtime_pressure_name(
    VLESourceRuntimePressureKind kind);
static char *age_vle_source_runtime_pressure_action(
    const VLESourceRuntimePressure *pressure);
static const char *age_vle_source_runtime_suppression_name(
    VLESourceDirectionClass direction_id);
static VLESourceEmptyEvidenceKind age_vle_empty_lifecycle_evidence_kind(
    const AgeVLESourceStats *stats);
static const char *age_vle_empty_lifecycle_evidence_name(
    VLESourceEmptyEvidenceKind evidence);
static VLESourceDirectionClass age_vle_empty_lifecycle_direction_kind(
    const AgeVLESourceStats *stats);
static int64 age_vle_empty_lifecycle_completion_count(
    const AgeVLESourceStats *stats);
static bool age_vle_empty_lifecycle_batch_saturated(
    const AgeVLESourceStats *stats);
static VLESourceDirectionClass age_vle_root_empty_completion_direction_kind(
    const AgeVLESourceStats *stats);
static bool age_vle_has_empty_source_suppression(
    const AgeVLESourceStats *stats);
static bool age_vle_empty_lifecycle_matches_plan(
    const AgeVLESourceStats *stats, const AgeVLEStreamEdgeSource *source);
static bool age_vle_empty_lifecycle_context_matches_plan(
    const AgeVLESourceStats *stats, const AgeVLEStreamEdgeSource *source);
static bool age_vle_empty_lifecycle_batch_matches_plan(
    const AgeVLESourceStats *stats, const AgeVLEStreamEdgeSource *source);
static bool vle_runtime_dominant_matches_plan(
    const VLESourceRuntimeDominant *dominant,
    const AgeVLEStreamEdgeSource *source);
static bool vle_runtime_class_matches_plan(
    const VLESourceRuntimeFeedback *feedback,
    const AgeVLEStreamEdgeSource *source);
static void set_vle_source_runtime_feedback_class(
    VLESourceRuntimeFeedback *feedback, VLESourcePolicyClass source_class);
static void copy_vle_source_runtime_feedback_class(
    VLESourceRuntimeFeedback *feedback, VLESourcePolicyClass source_class_id);
static void set_vle_source_policy_feedback_class(
    VLESourcePolicyFeedback *feedback, VLESourcePolicyClass source_class);
static bool vle_source_policy_uses_age_adjacency(
    const VLESourcePolicyDecision *out_policy,
    const VLESourcePolicyDecision *in_policy);
static int64 age_vle_runtime_value_pruning_count(
    const AgeVLESourceStats *stats);
static int64 age_vle_runtime_value_posting_pruning_count(
    const AgeVLESourceStats *stats, const AgeVLEStreamEdgeSource *source);
static HTAB *get_vle_source_threshold_cache(void);
static bool lookup_vle_source_threshold_feedback(
    const VLEStreamSourceCostInput *input,
    const VLESourcePolicyProfile *profile,
    VLESourceRuntimeThresholdFeedback *feedback);
static bool lookup_vle_source_directional_family_feedback(
    const VLEStreamSourceCostInput *input,
    const VLESourcePolicyProfile *profile,
    VLESourceRuntimeThresholdFeedback *feedback);
static bool merge_vle_source_directional_feedback_entry(
    VLESourceRuntimeThresholdFeedback *feedback,
    const VLESourcePolicyProfile *profile,
    const VLESourceThresholdCacheEntry *entry);
static bool vle_payload_feedback_class_matches_profile(
    const VLESourcePolicyProfile *profile, VLESourcePolicyClass payload_class);
const char *age_vle_output_requirement_name(
    AgeVLEOutputRequirement requirement);
const char *age_vle_source_policy_class_name(
    VLESourcePolicyClass source_class);
static int vle_source_threshold_class_rank_id(
    VLESourcePolicyClass source_class);
static int vle_source_payload_class_rank_id(
    VLESourcePolicyClass source_class);
static bool vle_source_policy_is_empty_lifecycle_class_id(
    VLESourcePolicyClass source_class);
static bool vle_source_policy_is_composite_class_id(
    VLESourcePolicyClass source_class);
static bool vle_source_direction_is_directional_family(
    VLESourceDirectionClass direction);
const char *age_vle_source_consumer_class_name(
    VLESourceConsumerClass consumer_class);
static VLESourceConsumerClass age_vle_source_policy_consumer_class_id(
    AgeVLEOutputRequirement requirement);
static bool vle_source_consumer_is_terminal_scalar(
    VLESourceConsumerClass consumer_class);
static bool vle_source_consumer_is_materialized(
    VLESourceConsumerClass consumer_class);
static void set_vle_threshold_feedback_reason(
    VLESourceRuntimeThresholdFeedback *feedback,
    VLESourceThresholdFeedbackReason reason);
static VLESourcePolicyClass classify_vle_threshold_feedback_reason_kind(
    VLESourceThresholdFeedbackReason reason);
static VLESourcePolicyClass classify_vle_payload_feedback_reason_kind(
    VLESourcePayloadFeedbackReason reason);
static void set_vle_payload_feedback_reason(
    VLESourceRuntimePayloadFeedback *feedback,
    VLESourcePayloadFeedbackReason reason);
static void derive_vle_source_runtime_payload_feedback(
    VLESourceRuntimePayloadFeedback *feedback,
    const AgeVLESourceStats *stats, const AgeVLEStreamEdgeSource *source);
static int64 select_vle_payload_replay_strong_percent(
    const AgeVLEStreamEdgeSource *source);
static int64 select_vle_payload_replay_strong_headroom_percent(
    const AgeVLEStreamEdgeSource *source);
static int64 select_vle_payload_replay_strong_headroom_percent_for_profile(
    const VLESourcePolicyProfile *profile);
static int64 select_vle_payload_feedback_headroom_percent_for_profile(
    const VLESourcePolicyProfile *profile,
    const VLESourceRuntimeThresholdFeedback *feedback);
static int64 select_vle_payload_value_posting_headroom_percent(
    const AgeVLEStreamEdgeSource *source);
static int64 select_vle_payload_value_posting_headroom_percent_for_profile(
    const VLESourcePolicyProfile *profile);
static int64 age_vle_output_materialization_weight(
    AgeVLEOutputRequirement requirement);
static int64 select_vle_payload_seed_headroom_percent(
    const AgeVLEStreamEdgeSource *source);
static int64 select_vle_payload_seed_headroom_percent_for_profile(
    const VLESourcePolicyProfile *profile);
static int64 select_vle_threshold_feedback_batch_size(
    const AgeVLESourceStats *stats);
static int64 select_vle_payload_matrix_frontier_batch_size(
    const VLESourcePolicyProfile *profile,
    const VLESourceRuntimeThresholdFeedback *feedback);
static int64 select_vle_threshold_feedback_headroom_percent(
    const VLESourcePolicyProfile *profile,
    const VLESourceRuntimeThresholdFeedback *feedback);
static bool vle_threshold_feedback_is_directional_family(
    const VLESourcePolicyProfile *profile,
    const VLESourceRuntimeThresholdFeedback *feedback);
static int64 calculate_vle_source_ratio_percent(int64 numerator,
                                                int64 denominator);
static void build_vle_source_threshold_cache_key(
    VLESourceThresholdCacheKey *key, const char *graph_name,
    const char *label_name, Oid edge_label_oid,
    VLESourceConsumerClass consumer_class_id,
    VLESourceDirectionClass active_direction_id);
static void apply_vle_source_cache_key_composite_identity(
    VLESourceThresholdCacheKey *key, int32 terminal_label_id,
    Oid terminal_property_index_oid, uint32 terminal_property_filter_id,
    VLESourceValuePostingKind value_posting_source_kind);
static void apply_vle_source_cache_key_input_identity(
    VLESourceThresholdCacheKey *key, const VLEStreamSourceCostInput *input,
    VLESourceDirectionClass source_direction_id);
static void apply_vle_source_cache_key_runtime_identity(
    VLESourceThresholdCacheKey *key,
    const AgeVLEStreamEdgeSource *source,
    VLESourceDirectionClass source_direction_id);
static VLESourceValuePostingKind
age_vle_source_value_posting_source_kind_for_direction(
    const AgeVLEStreamEdgeSource *source,
    VLESourceDirectionClass source_direction_id);
static VLESourceValuePostingKind
vle_source_input_value_posting_source_kind_for_direction(
    const VLEStreamSourceCostInput *input,
    VLESourceDirectionClass source_direction_id);
static void update_vle_source_threshold_cache_entry(
    const VLESourceThresholdCacheKey *key,
    const VLESourceRuntimeThresholdFeedback *feedback,
    const VLESourceRuntimePayloadFeedback *payload_feedback,
    VLESourceDirectionClass source_direction_id);
static void initialize_vle_source_threshold_cache_entry(
    VLESourceThresholdCacheEntry *entry,
    const VLESourceRuntimeThresholdFeedback *feedback,
    VLESourceDirectionClass source_direction_id);
static void merge_vle_source_payload_cache_entry(
    VLESourceThresholdCacheEntry *entry,
    const VLESourceRuntimePayloadFeedback *payload_feedback);
static void update_vle_source_payload_family_cache_entry(
    const VLESourceThresholdCacheKey *key,
    const VLESourceRuntimePayloadFeedback *payload_feedback);
static void update_vle_source_payload_family_cache_for_direction(
    const char *graph_name, const char *label_name,
    const AgeVLEStreamEdgeSource *source,
    const VLESourceRuntimePayloadFeedback *payload_feedback,
    VLESourceDirectionClass source_direction_id);
static bool lookup_vle_source_payload_family_feedback(
    const VLEStreamSourceCostInput *input,
    const VLESourcePolicyProfile *profile,
    VLESourceRuntimeThresholdFeedback *feedback);
static VLESourceConsumerClass vle_payload_feedback_consumer_family_id(
    VLESourceConsumerClass consumer_class);

void estimate_vle_source_fanout_evidence(
    VLESourceFanoutEvidence *evidence, Oid edge_label_oid)
{
    Assert(evidence != NULL);

    evidence->edge_label_oid = edge_label_oid;
    evidence->relation_tuples_known = false;
    evidence->start_fanout_known = false;
    evidence->end_fanout_known = false;
    evidence->reltuples = 0.0;
    evidence->start_fanout = 0.0;
    evidence->end_fanout = 0.0;
    evidence->start_fanout_source_kind =
        AGE_VLE_STREAM_FANOUT_SOURCE_STATISTICS;
    evidence->end_fanout_source_kind =
        AGE_VLE_STREAM_FANOUT_SOURCE_STATISTICS;
    evidence->start_value_posting_source_kind =
        VLE_SOURCE_VALUE_POSTING_NONE;
    evidence->end_value_posting_source_kind =
        VLE_SOURCE_VALUE_POSTING_NONE;

    if (!OidIsValid(edge_label_oid))
        return;

    evidence->reltuples = get_vle_relation_estimated_tuples(edge_label_oid);
    if (evidence->reltuples < 0)
        return;
    evidence->relation_tuples_known = true;
    if (evidence->reltuples <= 0)
        return;

    evidence->start_fanout_known = estimate_vle_edge_endpoint_fanout_with_state(
        &evidence->start_fanout,
        edge_label_oid, Anum_ag_label_edge_table_start_id,
        evidence->reltuples);
    evidence->end_fanout_known = estimate_vle_edge_endpoint_fanout_with_state(
        &evidence->end_fanout,
        edge_label_oid, Anum_ag_label_edge_table_end_id,
        evidence->reltuples);
}

double select_vle_source_fanout_for_direction(
    const VLESourceFanoutEvidence *evidence,
    const VLETraversalSourceIndexes *indexes, cypher_rel_dir direction)
{
    double fanout = 0.0;

    Assert(evidence != NULL);
    Assert(indexes != NULL);

    if (evidence->reltuples <= 0)
        return 0.0;

    if ((direction == CYPHER_REL_DIR_RIGHT ||
         direction == CYPHER_REL_DIR_NONE) &&
        (OidIsValid(indexes->age_adjacency_out_index_oid) ||
         OidIsValid(indexes->edge_start_index_oid)))
    {
        fanout += evidence->start_fanout;
    }
    if ((direction == CYPHER_REL_DIR_LEFT ||
         direction == CYPHER_REL_DIR_NONE) &&
        (OidIsValid(indexes->age_adjacency_in_index_oid) ||
         OidIsValid(indexes->edge_end_index_oid)))
    {
        fanout += evidence->end_fanout;
    }

    return fanout;
}

double select_vle_source_fanout_for_endpoint(
    const VLESourceFanoutEvidence *evidence, AttrNumber endpoint_attno)
{
    Assert(evidence != NULL);

    if (evidence->reltuples <= 0)
        return 0.0;

    if (endpoint_attno == Anum_ag_label_edge_table_start_id)
        return evidence->start_fanout;
    if (endpoint_attno == Anum_ag_label_edge_table_end_id)
        return evidence->end_fanout;

    return 0.0;
}

int64 round_vle_source_cost_evidence(double value)
{
    if (value <= 0)
        return 0;
    if (value >= (double)PG_INT64_MAX)
        return PG_INT64_MAX;

    return (int64)(value + 0.5);
}

const char *age_vle_value_posting_source_name(
    VLESourceValuePostingKind source)
{
    switch (source)
    {
        case VLE_SOURCE_VALUE_POSTING_RUN:
            return "run";
        case VLE_SOURCE_VALUE_POSTING_LABEL_SLICE:
            return "label-slice";
        case VLE_SOURCE_VALUE_POSTING_OTHER:
            return "other";
        case VLE_SOURCE_VALUE_POSTING_NONE:
            break;
    }

    return "none";
}

const char *age_vle_source_direction_name(VLESourceDirectionClass direction)
{
    switch (direction)
    {
        case VLE_SOURCE_DIRECTION_OUT:
            return "out";
        case VLE_SOURCE_DIRECTION_IN:
            return "in";
        case VLE_SOURCE_DIRECTION_BOTH:
            return "both";
        case VLE_SOURCE_DIRECTION_MIXED:
            return "mixed";
        case VLE_SOURCE_DIRECTION_NONE:
            break;
    }

    return "none";
}

void choose_vle_stream_source_cost_decision(
    VLEStreamSourceCostDecision *decision,
    const VLEStreamSourceCostInput *input)
{
    VLESourcePolicyProfile profile;
    VLESourcePolicyDecision out_policy;
    VLESourcePolicyDecision in_policy;
    VLESourcePolicyFeedback feedback;

    Assert(decision != NULL);
    Assert(input != NULL);
    Assert(input->evidence != NULL);

    decision->outgoing_kind = input->outgoing_kind;
    decision->incoming_kind = input->incoming_kind;
    decision->policy_text = NULL;
    decision->policy_output_requirement =
        AGE_VLE_OUTPUT_REQUIREMENT_UNKNOWN;
    decision->policy_consumer_class_id = VLE_SOURCE_CONSUMER_UNKNOWN;
    decision->policy_active_direction_id = VLE_SOURCE_DIRECTION_NONE;
    decision->policy_fanout_budget = 0;
    decision->policy_materialization_weight = 0;
    decision->policy_class_id = VLE_SOURCE_POLICY_CLASS_NONE;
    decision->policy_recommendation_kind = VLE_SOURCE_RECOMMENDATION_NONE;
    decision->cache_seed_eligible = false;
    decision->endpoint_headroom_percent = 0;
    decision->empty_lifecycle_eligible = false;
    decision->empty_lifecycle_depth = 0;
    decision->empty_lifecycle_batch_size = 0;
    decision->threshold_input_known = false;
    decision->threshold_input_headroom_percent = 0;
    decision->threshold_input_batch_size = 0;
    decision->threshold_input_observed_count = 0;
    decision->threshold_input_saturated_count = 0;
    decision->threshold_input_relaxed_count = 0;
    decision->threshold_input_out_observed_count = 0;
    decision->threshold_input_in_observed_count = 0;
    decision->threshold_input_out_saturated_count = 0;
    decision->threshold_input_in_saturated_count = 0;
    decision->threshold_input_direction_id = VLE_SOURCE_DIRECTION_NONE;
    decision->threshold_input_reason_id = VLE_SOURCE_THRESHOLD_REASON_NONE;
    decision->threshold_input_class_id = VLE_SOURCE_POLICY_CLASS_NONE;
    decision->payload_input_known = false;
    decision->payload_input_headroom_percent = 0;
    decision->payload_input_scan_runs = 0;
    decision->payload_input_replay_runs = 0;
    decision->payload_input_seed_runs = 0;
    decision->payload_input_replay_percent = 0;
    decision->payload_input_seed_percent = 0;
    decision->payload_input_observed_count = 0;
    decision->payload_input_value_posting_observed_count = 0;
    decision->payload_input_matrix_active_percent = 0;
    decision->payload_input_matrix_filter_percent = 0;
    decision->payload_input_matrix_prescan_percent = 0;
    decision->payload_input_matrix_replay_source_percent = 0;
    decision->payload_input_matrix_run_block_percent = 0;
    decision->payload_input_matrix_regroup_percent = 0;
    decision->payload_input_reason_id = VLE_SOURCE_PAYLOAD_REASON_NONE;
    decision->payload_input_class_id = VLE_SOURCE_POLICY_CLASS_NONE;
    decision->payload_input_value_posting_source_kind =
        VLE_SOURCE_VALUE_POSTING_NONE;
    decision->composite_prefilter_planned = false;
    decision->composite_candidate_fanout = 0;
    decision->composite_fanout = 0;

    if (input->source_kind != AGE_VLE_STREAM_EDGE_SOURCE_LOCAL_INDEX_CANDIDATE)
        return;

    build_vle_source_policy_profile(&profile, input);

    choose_vle_source_policy_decision(
        &out_policy,
        input->outgoing_kind, input->evidence->start_fanout,
        input->start_fanout_known, input->endpoint_start,
        input->age_adjacency_out, profile.outgoing_active,
        profile.composite_prefilter_planned && profile.outgoing_active,
        profile.outgoing_endpoint_headroom,
        &profile);
    choose_vle_source_policy_decision(
        &in_policy,
        input->incoming_kind, input->evidence->end_fanout,
        input->end_fanout_known, input->endpoint_end,
        input->age_adjacency_in, profile.incoming_active,
        profile.composite_prefilter_planned && profile.incoming_active,
        profile.incoming_endpoint_headroom,
        &profile);
    apply_vle_source_combined_policy(&out_policy, &in_policy, input,
                                     &profile);
    apply_vle_source_directional_family_policy(&out_policy, &in_policy,
                                               input, &profile);

    decision->outgoing_kind = out_policy.kind;
    decision->incoming_kind = in_policy.kind;
    decision->policy_text = format_vle_stream_source_cost_policy(
        decision, &out_policy, &in_policy, &profile);
    choose_vle_source_policy_feedback(&feedback, &out_policy, &in_policy,
                                      &profile);
    if (profile.payload_input_observed_count <= 0 ||
        profile.payload_input_headroom_percent <= 0)
    {
        profile.payload_input_known = false;
        profile.payload_input_headroom_percent = 0;
        profile.payload_input_scan_runs = 0;
        profile.payload_input_replay_runs = 0;
        profile.payload_input_seed_runs = 0;
        profile.payload_input_replay_percent = 0;
        profile.payload_input_seed_percent = 0;
        profile.payload_input_observed_count = 0;
        profile.payload_input_value_posting_observed_count = 0;
        profile.payload_input_matrix_active_percent = 0;
        profile.payload_input_matrix_filter_percent = 0;
        profile.payload_input_matrix_prescan_percent = 0;
        profile.payload_input_matrix_replay_source_percent = 0;
        profile.payload_input_matrix_run_block_percent = 0;
        profile.payload_input_matrix_regroup_percent = 0;
        profile.payload_input_reason_id = VLE_SOURCE_PAYLOAD_REASON_NONE;
        profile.payload_input_class_id = VLE_SOURCE_POLICY_CLASS_NONE;
        profile.payload_input_value_posting_source_kind =
            VLE_SOURCE_VALUE_POSTING_NONE;
    }
    decision->policy_output_requirement = profile.output_requirement;
    decision->policy_consumer_class_id = profile.consumer_class_id;
    decision->policy_active_direction_id = profile.direction_class_id;
    decision->policy_fanout_budget =
        round_vle_source_cost_evidence(profile.fanout_budget);
    decision->policy_materialization_weight = profile.materialization_weight;
    decision->policy_class_id = feedback.source_class_id;
    decision->policy_recommendation_kind = feedback.recommendation_kind;
    decision->cache_seed_eligible = profile.cache_seed_eligible;
    decision->endpoint_headroom_percent =
        (int64)(profile.cache_seed_endpoint_headroom * 100.0 + 0.5);
    decision->empty_lifecycle_eligible = profile.cache_seed_eligible &&
        vle_source_policy_uses_age_adjacency(&out_policy, &in_policy);
    decision->empty_lifecycle_depth =
        decision->empty_lifecycle_eligible ? profile.depth : 0;
    decision->empty_lifecycle_batch_size =
        decision->empty_lifecycle_eligible ?
        profile.empty_lifecycle_batch_size : 0;
    decision->threshold_input_known = profile.threshold_input_known;
    decision->threshold_input_headroom_percent =
        profile.threshold_input_headroom_percent;
    decision->threshold_input_batch_size = profile.threshold_input_batch_size;
    decision->threshold_input_observed_count =
        profile.threshold_input_observed_count;
    decision->threshold_input_saturated_count =
        profile.threshold_input_saturated_count;
    decision->threshold_input_relaxed_count =
        profile.threshold_input_relaxed_count;
    decision->threshold_input_out_observed_count =
        profile.threshold_input_out_observed_count;
    decision->threshold_input_in_observed_count =
        profile.threshold_input_in_observed_count;
    decision->threshold_input_out_saturated_count =
        profile.threshold_input_out_saturated_count;
    decision->threshold_input_in_saturated_count =
        profile.threshold_input_in_saturated_count;
    decision->threshold_input_direction_id =
        profile.threshold_input_direction_id;
    decision->threshold_input_reason_id = profile.threshold_input_reason_id;
    decision->threshold_input_class_id = profile.threshold_input_class_id;
    decision->payload_input_known = profile.payload_input_known;
    decision->payload_input_headroom_percent =
        profile.payload_input_headroom_percent;
    decision->payload_input_scan_runs = profile.payload_input_scan_runs;
    decision->payload_input_replay_runs = profile.payload_input_replay_runs;
    decision->payload_input_seed_runs = profile.payload_input_seed_runs;
    decision->payload_input_replay_percent =
        profile.payload_input_replay_percent;
    decision->payload_input_seed_percent =
        profile.payload_input_seed_percent;
    decision->payload_input_observed_count =
        profile.payload_input_observed_count;
    decision->payload_input_value_posting_observed_count =
        profile.payload_input_value_posting_observed_count;
    decision->payload_input_matrix_active_percent =
        profile.payload_input_matrix_active_percent;
    decision->payload_input_matrix_filter_percent =
        profile.payload_input_matrix_filter_percent;
    decision->payload_input_matrix_prescan_percent =
        profile.payload_input_matrix_prescan_percent;
    decision->payload_input_matrix_replay_source_percent =
        profile.payload_input_matrix_replay_source_percent;
    decision->payload_input_matrix_run_block_percent =
        profile.payload_input_matrix_run_block_percent;
    decision->payload_input_matrix_regroup_percent =
        profile.payload_input_matrix_regroup_percent;
    decision->payload_input_reason_id = profile.payload_input_reason_id;
    decision->payload_input_class_id = profile.payload_input_class_id;
    decision->payload_input_value_posting_source_kind =
        profile.payload_input_value_posting_source_kind;
    decision->composite_prefilter_planned =
        profile.composite_prefilter_planned;
    decision->composite_candidate_fanout =
        profile.composite_candidate_fanout;
    decision->composite_fanout = profile.composite_fanout;
}

double estimate_vle_edge_endpoint_fanout(
    Oid edge_label_oid, AttrNumber endpoint_attno, double reltuples)
{
    double fanout = 0.0;

    (void)estimate_vle_edge_endpoint_fanout_with_state(
        &fanout, edge_label_oid, endpoint_attno, reltuples);

    return fanout;
}

static bool estimate_vle_edge_endpoint_fanout_with_state(
    double *fanout, Oid edge_label_oid, AttrNumber endpoint_attno,
    double reltuples)
{
    HeapTuple stat_tuple;
    Form_pg_statistic stats;
    double distinct;

    Assert(fanout != NULL);
    *fanout = 0.0;

    if (!OidIsValid(edge_label_oid) || endpoint_attno <= 0 || reltuples <= 0)
        return false;

    stat_tuple = SearchSysCache3(STATRELATTINH,
                                 ObjectIdGetDatum(edge_label_oid),
                                 Int16GetDatum(endpoint_attno),
                                 BoolGetDatum(false));
    if (!HeapTupleIsValid(stat_tuple))
        return false;

    stats = (Form_pg_statistic) GETSTRUCT(stat_tuple);
    if (stats->stadistinct > 0)
        distinct = stats->stadistinct;
    else if (stats->stadistinct < 0)
        distinct = -stats->stadistinct * reltuples;
    else
        distinct = 0.0;

    ReleaseSysCache(stat_tuple);

    if (distinct <= 0)
        return false;

    distinct = Max(distinct, 1.0);
    distinct = Min(distinct, reltuples);

    *fanout = Max(reltuples / distinct, 1.0);
    return true;
}

double get_vle_relation_estimated_tuples(Oid relation_oid)
{
    HeapTuple rel_tuple;
    Form_pg_class rel_class;
    double reltuples;

    rel_tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relation_oid));
    if (!HeapTupleIsValid(rel_tuple))
        return 0.0;

    rel_class = (Form_pg_class) GETSTRUCT(rel_tuple);
    reltuples = rel_class->reltuples;
    ReleaseSysCache(rel_tuple);

    return reltuples;
}

char *format_vle_stream_edge_source_evidence(AgeVLEStreamEdgeSource *source)
{
    const char *state_text;

    if (source == NULL)
        return pstrdup("unknown");

    state_text = source->local_edge_state ?
        "dense-local" : "global-edge-state";

    return psprintf("%s, fixed-source=out=%s/in=%s, candidates="
                    "age_adjacency=%s/%s endpoint-btree=%s/%s, state=%s",
                    age_vle_stream_source_kind_name(source->kind),
                    age_vle_directed_source_kind_name(
                        source->outgoing_kind),
                    age_vle_directed_source_kind_name(
                        source->incoming_kind),
                    source->adjacency_out ? "out" : "-",
                    source->adjacency_in ? "in" : "-",
                    source->endpoint_start ? "start" : "-",
                    source->endpoint_end ? "end" : "-",
                    state_text);
}

char *format_vle_stream_edge_source_cost(AgeVLEStreamEdgeSource *source)
{
    if (source == NULL)
        return pstrdup("unknown");

    return psprintf("reltuples=%lld fanout=start:%lld/end:%lld "
                    "stats=rel:%s/start:%s/end:%s "
                    "source=start:%s/end:%s value-posting=start:%s/end:%s",
                    (long long)source->relation_tuples,
                    (long long)source->start_fanout,
                    (long long)source->end_fanout,
                    source->relation_tuples_known ? "known" : "unknown",
                    source->start_fanout_known ? "known" : "unknown",
                    source->end_fanout_known ? "known" : "unknown",
                    age_vle_stream_fanout_source_name(
                        source->start_fanout_source_kind),
                    age_vle_stream_fanout_source_name(
                        source->end_fanout_source_kind),
                    age_vle_value_posting_source_name(
                        source->start_value_posting_source_kind),
                    age_vle_value_posting_source_name(
                        source->end_value_posting_source_kind));
}

char *format_vle_stream_edge_terminal_property_source(
    AgeVLEStreamEdgeSource *source)
{
    if (source == NULL || !source->terminal_property_source_known)
        return pstrdup("none");

    return psprintf("label=%s source=%s provider=%s type=%s candidates=%lld",
                    source->terminal_property_label != NULL ?
                        source->terminal_property_label : "unknown",
                    source->terminal_property_source != NULL ?
                        source->terminal_property_source : "unknown",
                    source->terminal_property_provider != NULL ?
                        source->terminal_property_provider : "unknown",
                    source->terminal_property_type != NULL ?
                        source->terminal_property_type : "agtype",
                    (long long)source->terminal_property_match_count);
}

char *format_vle_stream_edge_composite_source(
    AgeVLEStreamEdgeSource *source)
{
    if (source == NULL || !source->composite_source_known)
        return pstrdup("none");

    return psprintf("status=%s reason=%s property-tuples=%lld "
                    "predicate=%s prefilter=%s threshold=%lld",
                    age_vle_stream_composite_source_reason_is_eligible(
                        source->composite_source_reason_kind) ?
                        "eligible" : "ineligible",
                    age_vle_stream_composite_source_reason_name(
                        source->composite_source_reason_kind),
                    (long long)source->composite_source_property_tuples,
                    age_vle_stream_terminal_value_kind_name(
                        source->terminal_property_value_kind_id),
                    source->terminal_property_prefilter_eligible ?
                        "eligible" : "ineligible",
                    (long long)source->terminal_property_prefetch_threshold);
}

char *format_vle_stream_edge_composite_fanout(
    AgeVLEStreamEdgeSource *source)
{
    StringInfoData buf;

    if (source == NULL || !source->composite_source_known)
        return pstrdup("none");

    initStringInfo(&buf);
    appendStringInfo(&buf,
                     "candidate=%lld composite=%lld planned=%s basis=%s",
                     (long long)source->composite_source_candidate_fanout,
                     (long long)source->composite_source_fanout,
                     age_vle_stream_composite_plan_name(
                         source->composite_source_planned_kind),
                     age_vle_stream_composite_source_reason_name(
                         source->composite_source_reason_kind));
    if (source->composite_source_selectivity_ppm > 0)
        appendStringInfo(&buf,
                         " selectivity=%.6f selectivity-source=%s",
                         (double)source->composite_source_selectivity_ppm /
                         1000000.0,
                         age_graph_property_selectivity_source_name(
                             source->composite_source_selectivity_source_kind));

    return buf.data;
}

char *format_vle_stream_edge_source_profile(AgeVLEStreamEdgeSource *source)
{
    if (source == NULL)
        return pstrdup("unknown");

    return psprintf("consumer=%s class=%s active=%s budget=%lld "
                    "weight=%lld cache-seed=%s endpoint-headroom=%.2f "
                    "empty-lifecycle=%s/depth:%lld empty-batch=%s/size:%lld",
                    age_vle_output_requirement_name(
                        source->policy_output_requirement),
                    age_vle_source_consumer_class_name(
                        source->policy_consumer_class_id),
                    age_vle_source_direction_name(
                        source->policy_active_direction_id),
                    (long long)source->policy_fanout_budget,
                    (long long)source->policy_materialization_weight,
                    source->cache_seed_eligible ? "eligible" : "ineligible",
                    (double)source->endpoint_headroom_percent / 100.0,
                    source->empty_lifecycle_eligible ?
                        "eligible" : "ineligible",
                    (long long)source->empty_lifecycle_depth,
                    source->empty_lifecycle_batch_size > 0 ?
                        "eligible" : "ineligible",
                    (long long)source->empty_lifecycle_batch_size);
}

char *format_vle_stream_edge_source_threshold_input(
    AgeVLEStreamEdgeSource *source)
{
    if (source == NULL)
        return pstrdup("unknown");

    return psprintf("source=%s headroom=%lld batch=%lld direction=%s "
                    "reason=%s class=%s observed=%lld saturated=%lld "
                    "relaxed=%lld",
                    source->threshold_input_known ?
                        "runtime-cache" : "none",
                    source->threshold_input_known ?
                        (long long)source->threshold_input_headroom_percent :
                        0LL,
                    source->threshold_input_known ?
                        (long long)source->threshold_input_batch_size :
                        0LL,
                    age_vle_source_direction_name(
                        source->threshold_input_direction_id),
                    age_vle_threshold_feedback_reason_name(
                        source->threshold_input_reason_id),
                    age_vle_source_policy_class_name(
                        source->threshold_input_class_id),
                    source->threshold_input_known ?
                        (long long)source->threshold_input_observed_count :
                        0LL,
                    source->threshold_input_known ?
                        (long long)source->threshold_input_saturated_count :
                        0LL,
                    source->threshold_input_known ?
                        (long long)source->threshold_input_relaxed_count :
                        0LL);
}

char *format_vle_stream_edge_source_payload_input(
    AgeVLEStreamEdgeSource *source)
{
    bool payload_input_known;
    StringInfoData buf;

    if (source == NULL)
        return pstrdup("unknown");

    payload_input_known = source->payload_input_known &&
        source->payload_input_observed_count > 0 &&
        source->payload_input_headroom_percent > 0 &&
        source->payload_input_reason_id != VLE_SOURCE_PAYLOAD_REASON_NONE;

    initStringInfo(&buf);
    appendStringInfo(&buf,
                     "source=%s headroom=%lld scan-runs=%lld "
                     "replay-runs=%lld seed-runs=%lld replay-percent=%lld "
                     "seed-percent=%lld observed=%lld "
                     "value-posting=%s/observed:%lld reason=%s class=%s",
                     payload_input_known ? "runtime-cache" : "none",
                     payload_input_known ?
                         (long long)source->payload_input_headroom_percent :
                         0LL,
                     payload_input_known ?
                         (long long)source->payload_input_scan_runs : 0LL,
                     payload_input_known ?
                         (long long)source->payload_input_replay_runs : 0LL,
                     payload_input_known ?
                         (long long)source->payload_input_seed_runs : 0LL,
                     payload_input_known ?
                         (long long)source->payload_input_replay_percent : 0LL,
                     payload_input_known ?
                         (long long)source->payload_input_seed_percent : 0LL,
                     payload_input_known ?
                         (long long)source->payload_input_observed_count : 0LL,
                     !payload_input_known ? "none" :
                         age_vle_value_posting_source_name(
                             source->payload_input_value_posting_source_kind),
                     payload_input_known ?
                         (long long)source->payload_input_value_posting_observed_count :
                         0LL,
                     !payload_input_known ? "none" :
                         age_vle_payload_feedback_reason_name(
                             source->payload_input_reason_id),
                     !payload_input_known ? "none" :
                         age_vle_source_policy_class_name(
                             source->payload_input_class_id));
    if (payload_input_known &&
        (source->payload_input_matrix_active_percent > 0 ||
         source->payload_input_matrix_filter_percent > 0 ||
         source->payload_input_matrix_prescan_percent > 0 ||
         source->payload_input_matrix_replay_source_percent > 0))
    {
        appendStringInfo(&buf, " mrun=active:%lld/filter:%lld/prescan:%lld",
                         (long long)
                            source->payload_input_matrix_active_percent,
                         (long long)
                            source->payload_input_matrix_filter_percent,
                         (long long)
                            source->payload_input_matrix_prescan_percent);
        if (source->payload_input_matrix_replay_source_percent > 0)
            appendStringInfo(&buf, "/replay:%lld",
                             (long long)
                                source->payload_input_matrix_replay_source_percent);
    }

    return buf.data;
}

char *format_vle_stream_edge_source_policy(AgeVLEStreamEdgeSource *source)
{
    if (source == NULL)
        return pstrdup("unknown");

    return pstrdup(source->cost_policy == NULL ? "none" : source->cost_policy);
}

char *format_vle_source_runtime_evidence(const AgeVLESourceStats *stats,
                                         const AgeVLEStreamEdgeSource *source)
{
    VLESourceRuntimeExplain explain;
    char *pressure_action;
    char *result;

    Assert(stats != NULL);

    build_vle_source_runtime_explain(&explain, stats, source);

    pressure_action = age_vle_source_runtime_pressure_action(
        &explain.pressure);
    result = psprintf("dominant=%s class=%s pressure=%s action=%s",
                      age_vle_source_runtime_dominant_name(
                          explain.dominant.kind),
                      explain.feedback.source_class_id !=
                      VLE_SOURCE_POLICY_CLASS_NONE ?
                      age_vle_source_policy_class_name(
                          explain.feedback.source_class_id) :
                      age_vle_source_runtime_dominant_name(
                          explain.dominant.kind),
                      age_vle_source_runtime_pressure_name(
                          explain.pressure.kind),
                      pressure_action);
    pfree(pressure_action);
    return result;
}

char *format_vle_source_runtime_plan(const AgeVLESourceStats *stats,
                                     const AgeVLEStreamEdgeSource *source)
{
    VLESourceRuntimeExplain explain;

    Assert(stats != NULL);

    build_vle_source_runtime_explain(&explain, stats, source);

    return psprintf("planned=out:%s/in:%s source-match=%s "
                    "planned-class=%s class-match=%s",
                    source == NULL ? "unknown" :
                        age_vle_directed_source_kind_name(
                            source->outgoing_kind),
                    source == NULL ? "unknown" :
                        age_vle_directed_source_kind_name(
                            source->incoming_kind),
                    vle_runtime_dominant_matches_plan(&explain.dominant,
                                                      source) ?
                        "true" : "false",
                    source == NULL ? "unknown" :
                    age_vle_source_policy_class_name(
                        explain.planned_class_id),
                    vle_runtime_class_matches_plan(&explain.feedback,
                                                   source) ?
                        "true" : "false");
}

char *format_vle_source_runtime_counters(
    const AgeVLESourceStats *stats, const AgeVLEStreamEdgeSource *source)
{
    VLESourceRuntimeExplain explain;

    Assert(stats != NULL);

    build_vle_source_runtime_explain(&explain, stats, source);

    return psprintf("age_adjacency=scans:%lld/candidates:%lld "
                    "endpoint-btree=scans:%lld/candidates:%lld "
                    "packed=scans:%lld/candidates:%lld/empty:%lld/"
                    "policy:%lld density=age_adjacency:%.2f/"
                    "endpoint-btree:%.2f/packed:%.2f candidates=%lld/%lld",
                    (long long)stats->age_adjacency_scans,
                    (long long)stats->age_adjacency_candidates,
                    (long long)stats->endpoint_btree_scans,
                    (long long)stats->endpoint_btree_candidates,
                    (long long)stats->packed_scans,
                    (long long)stats->packed_candidates,
                    (long long)stats->packed_empty_skips,
                    (long long)stats->packed_policy_skips,
                    explain.age_adjacency_density,
                    explain.endpoint_btree_density,
                    explain.packed_density,
                    (long long)stats->candidates_pushed,
                    (long long)stats->candidates_yielded);
}

char *format_vle_source_runtime_payload(
    const AgeVLESourceStats *stats, const AgeVLEStreamEdgeSource *source)
{
    StringInfoData buf;

    Assert(stats != NULL);
    (void)source;

    initStringInfo(&buf);
    appendStringInfo(&buf,
                     "runs=scan:%lld/replay:%lld/seed:%lld "
                     "property-prefilter=%lld/%lld/%lld",
                     (long long)stats->age_adjacency_payload_scan_runs,
                     (long long)stats->age_adjacency_payload_replay_runs,
                     (long long)stats->age_adjacency_payload_cache_seed_runs,
                     (long long)
                        stats->age_adjacency_payload_property_prefilter_runs,
                     (long long)
                        stats->age_adjacency_payload_property_prefilter_candidates,
                     (long long)
                        stats->age_adjacency_payload_property_filtered);
    if (stats->age_adjacency_payload_property_vertex_set_runs > 0)
        appendStringInfo(&buf, " vertex-set=%lld",
                         (long long)
                            stats->age_adjacency_payload_property_vertex_set_runs);
    if (stats->age_adjacency_payload_property_prefetch_matches > 0)
        appendStringInfo(&buf, " prefetch-matches=%lld",
                         (long long)
                            stats->age_adjacency_payload_property_prefetch_matches);
    if (stats->age_adjacency_payload_composite_requests > 0)
    {
        appendStringInfo(&buf, " composite=request:%lld",
                         (long long)
                            stats->age_adjacency_payload_composite_requests);
        if (stats->age_adjacency_payload_composite_block_filtered > 0)
            appendStringInfo(&buf, "/block-filter:%lld",
                             (long long)
                                stats->age_adjacency_payload_composite_block_filtered);
        if (stats->age_adjacency_payload_composite_directory_filtered > 0)
            appendStringInfo(&buf, "/dir-filter:%lld",
                             (long long)
                                stats->age_adjacency_payload_composite_directory_filtered);
        if (stats->age_adjacency_payload_composite_directory_estimated > 0)
            appendStringInfo(&buf, "/dir-estimate:%lld",
                             (long long)
                                stats->age_adjacency_payload_composite_directory_estimated);
    }
    if (stats->age_adjacency_payload_cache_filtered > 0)
        appendStringInfo(&buf, " cache-filter=%lld/%lld/%lld",
                         (long long)
                            stats->age_adjacency_payload_cache_filtered,
                         (long long)
                            stats->age_adjacency_payload_cache_label_filtered,
                         (long long)
                            stats->age_adjacency_payload_cache_property_filtered);
    if (stats->age_adjacency_payload_vertex_set_range_filtered > 0)
        appendStringInfo(&buf, " set-range-filter=%lld",
                         (long long)
                            stats->age_adjacency_payload_vertex_set_range_filtered);
    if (stats->age_adjacency_payload_vertex_set_sorted_filtered > 0)
        appendStringInfo(&buf, " set-sorted-filter=%lld",
                         (long long)
                            stats->age_adjacency_payload_vertex_set_sorted_filtered);
    if (stats->age_adjacency_payload_vertex_set_block_filtered > 0)
        appendStringInfo(&buf, " set-block-filter=%lld",
                         (long long)
                            stats->age_adjacency_payload_vertex_set_block_filtered);
    if (stats->age_adjacency_payload_vertex_set_block_value_filtered > 0)
        appendStringInfo(&buf, "/value-summary:%lld",
                         (long long)
                            stats->age_adjacency_payload_vertex_set_block_value_filtered);
    if (stats->age_adjacency_payload_vertex_set_block_value_posting_filtered > 0)
        appendStringInfo(&buf, "/value-posting:%lld",
                         (long long)
                            stats->age_adjacency_payload_vertex_set_block_value_posting_filtered);
    if (stats->age_adjacency_payload_vertex_set_block_compressed_filtered > 0)
        appendStringInfo(&buf, "/compressed:%lld",
                         (long long)
                            stats->age_adjacency_payload_vertex_set_block_compressed_filtered);
    if (stats->age_adjacency_payload_vertex_set_block_posting_filtered > 0)
        appendStringInfo(&buf, "/posting:%lld",
                         (long long)
                            stats->age_adjacency_payload_vertex_set_block_posting_filtered);
    if (stats->age_adjacency_payload_vertex_set_directory_filtered > 0)
        appendStringInfo(&buf, " set-directory-filter=%lld",
                         (long long)
                            stats->age_adjacency_payload_vertex_set_directory_filtered);
    if (stats->age_adjacency_payload_vertex_set_directory_range_filtered > 0)
        appendStringInfo(&buf, "/range:%lld",
                         (long long)
                            stats->age_adjacency_payload_vertex_set_directory_range_filtered);
    if (stats->age_adjacency_payload_vertex_set_directory_exact_filtered > 0)
        appendStringInfo(&buf, "/exact:%lld",
                         (long long)
                            stats->age_adjacency_payload_vertex_set_directory_exact_filtered);
    if (stats->age_adjacency_payload_vertex_set_directory_label_bloom_filtered > 0)
        appendStringInfo(&buf, "/label-bloom:%lld",
                         (long long)
                            stats->age_adjacency_payload_vertex_set_directory_label_bloom_filtered);
    if (stats->age_adjacency_payload_vertex_set_directory_compressed_filtered > 0)
        appendStringInfo(&buf, "/compressed:%lld",
                         (long long)
                            stats->age_adjacency_payload_vertex_set_directory_compressed_filtered);
    if (stats->age_adjacency_payload_vertex_set_directory_value_filtered > 0)
        appendStringInfo(&buf, "/value-summary:%lld",
                         (long long)
                            stats->age_adjacency_payload_vertex_set_directory_value_filtered);
    if (stats->age_adjacency_payload_vertex_set_directory_value_posting_filtered > 0)
        appendStringInfo(&buf, "/value-posting:%lld",
                         (long long)
                            stats->age_adjacency_payload_vertex_set_directory_value_posting_filtered);
    if (stats->age_adjacency_payload_vertex_set_directory_wide_bloom_filtered > 0)
        appendStringInfo(&buf, "/wide-bloom:%lld",
                         (long long)
                            stats->age_adjacency_payload_vertex_set_directory_wide_bloom_filtered);
    appendStringInfo(&buf,
                     " tuples=scan:%lld/replay:%lld/seeds:%lld "
                     "replay=%lld/%lld",
                     (long long)stats->age_adjacency_payload_scans,
                     (long long)stats->age_adjacency_payload_replays,
                     (long long)stats->age_adjacency_payload_cache_seeds,
                     (long long)stats->age_adjacency_payload_replays,
                     (long long)stats->age_adjacency_payload_scans);

    return buf.data;
}

char *format_vle_source_runtime_empty_evidence(
    const AgeVLESourceStats *stats, const AgeVLEStreamEdgeSource *source)
{
    VLESourceRuntimeExplain explain;

    Assert(stats != NULL);

    build_vle_source_runtime_explain(&explain, stats, source);

    return psprintf("scans=age_adjacency:%lld/endpoint-btree:%lld "
                    "evidence=%s missing-vertex=%lld/%lld "
                    "suppressed=%s match=%s cache=%lld/%lld/%lld "
                    "frontier=%lld/%lld/%lld run=%lld/%lld/%lld",
                    (long long)stats->age_adjacency_empty_scans,
                    (long long)stats->endpoint_btree_empty_scans,
                    age_vle_empty_lifecycle_evidence_name(
                        age_vle_empty_lifecycle_evidence_kind(stats)),
                    (long long)stats->missing_vertex_source_hits,
                    (long long)stats->missing_vertex_attempts,
                    age_vle_source_runtime_suppression_name(
                        explain.suppression.direction_id),
                    explain.suppression.planned_match ? "true" : "false",
                    (long long)stats->age_adjacency_empty_source_cache_hits,
                    (long long)stats->age_adjacency_empty_source_cache_hit_out,
                    (long long)stats->age_adjacency_empty_source_cache_hit_in,
                    (long long)stats->age_adjacency_empty_source_frontier_marks,
                    (long long)stats->age_adjacency_empty_source_frontier_mark_out,
                    (long long)stats->age_adjacency_empty_source_frontier_mark_in,
                    (long long)stats->age_adjacency_empty_source_run_skips,
                    (long long)stats->age_adjacency_empty_source_run_skip_out,
                    (long long)stats->age_adjacency_empty_source_run_skip_in);
}

char *format_vle_source_runtime_empty_lifecycle(
    const AgeVLESourceStats *stats, const AgeVLEStreamEdgeSource *source)
{
    Assert(stats != NULL);

    return psprintf("plan=%s/depth:%lld match=%s "
                    "context=%s/depth:%lld/runs:%lld match=%s "
                    "batch=%s/size:%lld/capacity:%lld match=%s "
                    "frontier-batch=flushes:%lld/keys:%lld/max:%lld "
                    "packed-suppressed=out:%lld/in:%lld/self:%lld",
                    source != NULL && source->empty_lifecycle_eligible ?
                        "eligible" : "ineligible",
                    source == NULL ? 0 :
                        (long long)source->empty_lifecycle_depth,
                    age_vle_empty_lifecycle_matches_plan(stats, source) ?
                        "true" : "false",
                    stats->empty_lifecycle_context_eligible_runs > 0 ?
                        "eligible" : "ineligible",
                    (long long)stats->empty_lifecycle_context_depth,
                    (long long)stats->empty_lifecycle_context_runs,
                    age_vle_empty_lifecycle_context_matches_plan(
                        stats, source) ? "true" : "false",
                    stats->empty_lifecycle_batch_capacity > 0 ?
                        "eligible" : "ineligible",
                    source == NULL ? 0 :
                        (long long)source->empty_lifecycle_batch_size,
                    (long long)stats->empty_lifecycle_batch_capacity,
                    age_vle_empty_lifecycle_batch_matches_plan(
                        stats, source) ? "true" : "false",
                    (long long)stats->age_adjacency_empty_source_frontier_batch_flushes,
                    (long long)
                        stats->age_adjacency_empty_source_frontier_batch_keys,
                    (long long)stats->age_adjacency_empty_source_frontier_batch_max,
                    (long long)stats->packed_suppress_out,
                    (long long)stats->packed_suppress_in,
                    (long long)stats->packed_suppress_self);
}

char *format_vle_source_runtime_feedback(
    const AgeVLESourceStats *stats, const AgeVLEStreamEdgeSource *source)
{
    StringInfoData buf;
    VLESourceRuntimeExplain explain;

    Assert(stats != NULL);

    build_vle_source_runtime_explain(&explain, stats, source);

    initStringInfo(&buf);
    appendStringInfo(&buf,
                     "recommendation=%s planned=%s "
                     "root-empty=completion:%lld/out:%lld/in:%lld/"
                     "batch:%lld/saturated-roots:%lld "
                     "threshold=%s/headroom:%lld/batch:%lld/source:%s/"
                     "reason:%s yield=%lld/%lld push=%lld/%lld",
                     age_vle_source_policy_recommendation_name(
                         explain.feedback.recommendation_kind),
                     age_vle_source_policy_recommendation_name(
                         explain.planned_recommendation_kind),
                     (long long)stats->root_empty_completion_count,
                     (long long)stats->root_empty_completion_out,
                     (long long)stats->root_empty_completion_in,
                     (long long)stats->root_empty_batch_capacity,
                     (long long)stats->root_empty_saturated_count,
                     explain.threshold_feedback.eligible ? "eligible" :
                         "ineligible",
                     (long long)
                         explain.threshold_feedback.endpoint_headroom_percent,
                     (long long)
                         explain.threshold_feedback.empty_lifecycle_batch_size,
                     age_vle_source_direction_name(
                         explain.threshold_feedback.source_direction_id),
                     age_vle_threshold_feedback_reason_name(
                         explain.threshold_feedback.reason_id),
                     (long long)explain.dominant.candidates,
                     (long long)explain.dominant.scans,
                     (long long)stats->candidates_pushed,
                     (long long)stats->candidates_yielded);
    if (stats->matrix_frontier_source_run_sources > 0)
    {
        appendStringInfo(&buf, " mrun=active:%lld/filter:%lld/prescan:%lld",
                         (long long)calculate_vle_source_ratio_percent(
                             stats->matrix_frontier_source_run_active_keys,
                             stats->matrix_frontier_source_run_sources),
                         (long long)calculate_vle_source_ratio_percent(
                             stats->matrix_frontier_source_run_filtered_keys,
                             stats->matrix_frontier_source_run_sources),
                         (long long)calculate_vle_source_ratio_percent(
                             stats->matrix_frontier_source_run_prefiltered_keys,
                             stats->matrix_frontier_source_run_sources));
    }

    return buf.data;
}

static void build_vle_source_runtime_explain(
    VLESourceRuntimeExplain *explain, const AgeVLESourceStats *stats,
    const AgeVLEStreamEdgeSource *source)
{
    Assert(explain != NULL);
    Assert(stats != NULL);

    choose_vle_source_runtime_dominant(stats, &explain->dominant);
    choose_vle_source_runtime_feedback(stats, &explain->dominant, source,
                                       &explain->feedback);
    choose_vle_source_runtime_pressure(stats, &explain->dominant, source,
                                       &explain->feedback,
                                       &explain->pressure);
    choose_vle_source_runtime_suppression(stats, source,
                                          &explain->suppression);
    derive_vle_source_runtime_threshold_feedback(
        &explain->threshold_feedback, stats, source);
    explain->planned_class_id = source == NULL ?
        VLE_SOURCE_POLICY_CLASS_NONE : source->policy_class_id;
    explain->planned_recommendation_kind = source == NULL ?
        VLE_SOURCE_RECOMMENDATION_NONE :
        source->policy_recommendation_kind;
    explain->age_adjacency_density = calculate_vle_source_scan_density(
        stats->age_adjacency_candidates, stats->age_adjacency_scans);
    explain->endpoint_btree_density = calculate_vle_source_scan_density(
        stats->endpoint_btree_candidates, stats->endpoint_btree_scans);
    explain->packed_density = calculate_vle_source_scan_density(
        stats->packed_candidates, stats->packed_scans);
}

void derive_vle_source_runtime_threshold_feedback(
    VLESourceRuntimeThresholdFeedback *feedback,
    const AgeVLESourceStats *stats, const AgeVLEStreamEdgeSource *source)
{
    Assert(feedback != NULL);
    Assert(stats != NULL);

    feedback->eligible = false;
    feedback->saturated = false;
    feedback->endpoint_headroom_percent = 0;
    feedback->empty_lifecycle_batch_size = 0;
    feedback->root_empty_completion_count = 0;
    feedback->root_empty_completion_out = 0;
    feedback->root_empty_completion_in = 0;
    feedback->observed_count = 0;
    feedback->saturated_count = 0;
    feedback->relaxed_count = 0;
    feedback->out_observed_count = 0;
    feedback->in_observed_count = 0;
    feedback->out_saturated_count = 0;
    feedback->in_saturated_count = 0;
    feedback->scan_runs = 0;
    feedback->replay_runs = 0;
    feedback->seed_runs = 0;
    feedback->replay_percent = 0;
    feedback->seed_percent = 0;
    feedback->payload_observed_count = 0;
    feedback->payload_endpoint_headroom_percent = 0;
    feedback->payload_value_posting_observed_count = 0;
    feedback->payload_matrix_active_percent = 0;
    feedback->payload_matrix_filter_percent = 0;
    feedback->payload_matrix_prescan_percent = 0;
    feedback->payload_matrix_replay_source_percent = 0;
    feedback->payload_matrix_run_block_percent = 0;
    feedback->payload_matrix_regroup_percent = 0;
    feedback->source_direction_id = VLE_SOURCE_DIRECTION_NONE;
    set_vle_threshold_feedback_reason(
        feedback, VLE_SOURCE_THRESHOLD_REASON_NONE);
    feedback->payload_reason_id = VLE_SOURCE_PAYLOAD_REASON_NONE;
    feedback->payload_class_id = VLE_SOURCE_POLICY_CLASS_NONE;
    feedback->payload_value_posting_source_kind =
        VLE_SOURCE_VALUE_POSTING_NONE;

    if (source == NULL || !source->empty_lifecycle_eligible ||
        stats->root_empty_batch_capacity <= 0)
    {
        return;
    }

    feedback->eligible = true;
    feedback->endpoint_headroom_percent = source->endpoint_headroom_percent;
    feedback->empty_lifecycle_batch_size =
        select_vle_threshold_feedback_batch_size(stats);
    feedback->root_empty_completion_count =
        stats->root_empty_completion_count;
    feedback->root_empty_completion_out = stats->root_empty_completion_out;
    feedback->root_empty_completion_in = stats->root_empty_completion_in;
    feedback->source_direction_id =
        age_vle_root_empty_completion_direction_kind(stats);
    set_vle_threshold_feedback_reason(
        feedback,
        stats->root_empty_completion_count > 0 ?
        VLE_SOURCE_THRESHOLD_REASON_ROOT_EMPTY_OBSERVED :
        VLE_SOURCE_THRESHOLD_REASON_PLANNED_EMPTY_LIFECYCLE);

    if (stats->root_empty_saturated_count > 0)
    {
        feedback->saturated = true;
        feedback->endpoint_headroom_percent =
            (int64)(VLE_EMPTY_LIFECYCLE_BATCH_ENDPOINT_HEADROOM * 100.0 +
                    0.5);
        set_vle_threshold_feedback_reason(
            feedback, VLE_SOURCE_THRESHOLD_REASON_ROOT_EMPTY_SATURATED);
    }
}

static void derive_vle_source_runtime_payload_feedback(
    VLESourceRuntimePayloadFeedback *feedback,
    const AgeVLESourceStats *stats, const AgeVLEStreamEdgeSource *source)
{
    bool composite_prefilter_policy;
    int64 value_posting_pruning_count;

    Assert(feedback != NULL);
    Assert(stats != NULL);

    feedback->eligible = false;
    feedback->endpoint_headroom_percent = 0;
    feedback->scan_runs = 0;
    feedback->replay_runs = 0;
    feedback->seed_runs = 0;
    feedback->replay_percent = 0;
    feedback->seed_percent = 0;
    set_vle_payload_feedback_reason(feedback,
                                    VLE_SOURCE_PAYLOAD_REASON_NONE);
    feedback->value_posting_observed_count = 0;
    feedback->value_posting_source_kind = VLE_SOURCE_VALUE_POSTING_NONE;
    feedback->matrix_active_percent = 0;
    feedback->matrix_filter_percent = 0;
    feedback->matrix_prescan_percent = 0;
    feedback->matrix_replay_source_percent = 0;
    feedback->matrix_run_block_percent = 0;
    feedback->matrix_regroup_percent = 0;

    composite_prefilter_policy = source != NULL &&
        source->policy_class_id ==
        VLE_SOURCE_POLICY_CLASS_ADJACENCY_COMPOSITE_PREFILTER;

    if (source == NULL ||
        (!source->cache_seed_eligible && !composite_prefilter_policy) ||
        stats->age_adjacency_payload_scan_runs <= 0)
    {
        return;
    }

    feedback->eligible = true;
    feedback->scan_runs = stats->age_adjacency_payload_scan_runs;
    feedback->replay_runs = stats->age_adjacency_payload_replay_runs;
    feedback->seed_runs = stats->age_adjacency_payload_cache_seed_runs;
    feedback->replay_percent = calculate_vle_source_ratio_percent(
        feedback->replay_runs, feedback->scan_runs);
    feedback->seed_percent = calculate_vle_source_ratio_percent(
        feedback->seed_runs, feedback->scan_runs);
    feedback->matrix_active_percent = calculate_vle_source_ratio_percent(
        stats->matrix_frontier_source_run_active_keys,
        stats->matrix_frontier_source_run_sources);
    feedback->matrix_filter_percent = calculate_vle_source_ratio_percent(
        stats->matrix_frontier_source_run_filtered_keys,
        stats->matrix_frontier_source_run_sources);
    feedback->matrix_prescan_percent = calculate_vle_source_ratio_percent(
        stats->matrix_frontier_source_run_prefiltered_keys,
        stats->matrix_frontier_source_run_sources);
    feedback->matrix_replay_source_percent =
        calculate_vle_source_ratio_percent(
            stats->matrix_frontier_source_run_replay_segment_sources,
            stats->matrix_frontier_source_run_sources);
    feedback->matrix_run_block_percent = calculate_vle_source_ratio_percent(
        stats->matrix_frontier_source_run_shared_page_run_block_full_group_drain_cursors > 0 ?
        stats->matrix_frontier_source_run_shared_page_run_block_full_group_drain_cursors :
        stats->matrix_frontier_source_run_block_tag_batch_cursors > 0 ?
        stats->matrix_frontier_source_run_block_tag_batch_cursors :
        stats->matrix_frontier_source_run_shared_page_run_block_stream_cursors > 0 ?
        stats->matrix_frontier_source_run_shared_page_run_block_stream_cursors :
        stats->matrix_frontier_source_run_raw_block_batch_cursors > 0 ?
        stats->matrix_frontier_source_run_raw_block_batch_cursors :
        stats->matrix_frontier_source_run_shared_page_run_block_cursors,
        stats->matrix_frontier_source_run_sources);
    feedback->matrix_regroup_percent = calculate_vle_source_ratio_percent(
        stats->matrix_frontier_source_run_fallback_regroup_cursors,
        stats->matrix_frontier_source_run_sources);
    feedback->endpoint_headroom_percent = source->endpoint_headroom_percent;
    set_vle_payload_feedback_reason(
        feedback, VLE_SOURCE_PAYLOAD_REASON_SCAN_OBSERVED);
    value_posting_pruning_count =
        age_vle_runtime_value_posting_pruning_count(stats, source);
    if (value_posting_pruning_count > 0)
    {
        feedback->value_posting_observed_count = value_posting_pruning_count;
        feedback->value_posting_source_kind =
            age_vle_source_value_posting_source_kind_for_direction(
                source, source->policy_active_direction_id);
    }

    if (composite_prefilter_policy &&
        feedback->value_posting_observed_count > 0)
    {
        feedback->endpoint_headroom_percent =
            select_vle_payload_value_posting_headroom_percent(source);
        set_vle_payload_feedback_reason(
            feedback, VLE_SOURCE_PAYLOAD_REASON_VALUE_POSTING_OBSERVED);
    }
    else if (composite_prefilter_policy &&
        stats->age_adjacency_directory_filtered_empty_scans > 0)
    {
        set_vle_payload_feedback_reason(
            feedback, VLE_SOURCE_PAYLOAD_REASON_DIRECTORY_FILTER_OBSERVED);
    }
    else if (composite_prefilter_policy &&
             stats->age_adjacency_payload_property_prefilter_runs > 0)
    {
        set_vle_payload_feedback_reason(
            feedback,
            VLE_SOURCE_PAYLOAD_REASON_COMPOSITE_PREFILTER_OBSERVED);
    }
    else if (stats->matrix_frontier_source_run_prefiltered_keys > 0)
    {
        feedback->endpoint_headroom_percent =
            Min(feedback->endpoint_headroom_percent,
                select_vle_payload_value_posting_headroom_percent(source));
        set_vle_payload_feedback_reason(
            feedback, VLE_SOURCE_PAYLOAD_REASON_MATRIX_PRESCAN_COMPACT);
    }
    else if (stats->matrix_frontier_source_run_shared_page_run_block_full_group_drains > 0)
    {
        feedback->endpoint_headroom_percent =
            Min(feedback->endpoint_headroom_percent,
                select_vle_payload_value_posting_headroom_percent(source));
        set_vle_payload_feedback_reason(
            feedback, VLE_SOURCE_PAYLOAD_REASON_MATRIX_FULL_GROUP_DRAIN);
    }
    else if (stats->matrix_frontier_source_run_replay_segments > 0)
    {
        feedback->endpoint_headroom_percent =
            Min(feedback->endpoint_headroom_percent,
                select_vle_payload_value_posting_headroom_percent(source));
        set_vle_payload_feedback_reason(
            feedback, VLE_SOURCE_PAYLOAD_REASON_MATRIX_REPLAY_SOURCE_BATCH);
    }
    else if (stats->matrix_frontier_source_run_block_tag_batches > 0)
    {
        feedback->endpoint_headroom_percent =
            Min(feedback->endpoint_headroom_percent,
                select_vle_payload_value_posting_headroom_percent(source));
        set_vle_payload_feedback_reason(
            feedback, VLE_SOURCE_PAYLOAD_REASON_MATRIX_BLOCK_TAG_BATCH);
    }
    else if (stats->matrix_frontier_source_run_shared_page_run_block_streams > 0)
    {
        feedback->endpoint_headroom_percent =
            Min(feedback->endpoint_headroom_percent,
                select_vle_payload_value_posting_headroom_percent(source));
        set_vle_payload_feedback_reason(
            feedback, VLE_SOURCE_PAYLOAD_REASON_MATRIX_RUN_BLOCK_STREAM);
    }
    else if (stats->matrix_frontier_source_run_raw_block_batches > 0)
    {
        feedback->endpoint_headroom_percent =
            Min(feedback->endpoint_headroom_percent,
                select_vle_payload_value_posting_headroom_percent(source));
        set_vle_payload_feedback_reason(
            feedback, VLE_SOURCE_PAYLOAD_REASON_MATRIX_RAW_BLOCK_BATCH);
    }
    else if (stats->matrix_frontier_source_run_shared_page_run_block_groups > 0)
    {
        feedback->endpoint_headroom_percent =
            Min(feedback->endpoint_headroom_percent,
                select_vle_payload_value_posting_headroom_percent(source));
        set_vle_payload_feedback_reason(
            feedback, VLE_SOURCE_PAYLOAD_REASON_MATRIX_RUN_BLOCK_GROUP);
    }
    else if (stats->matrix_frontier_source_run_shared_page_groups > 0 ||
             stats->matrix_frontier_source_run_fallback_regroups > 0)
    {
        feedback->endpoint_headroom_percent =
            Min(feedback->endpoint_headroom_percent,
                select_vle_payload_value_posting_headroom_percent(source));
        set_vle_payload_feedback_reason(
            feedback,
            stats->matrix_frontier_source_run_fallback_regroups > 0 ?
            VLE_SOURCE_PAYLOAD_REASON_MATRIX_FALLBACK_REGROUP :
            VLE_SOURCE_PAYLOAD_REASON_MATRIX_PAGE_GROUP);
    }
    else if (stats->age_adjacency_payload_replay_runs > 0)
    {
        if (feedback->replay_percent >=
            select_vle_payload_replay_strong_percent(source))
        {
            feedback->endpoint_headroom_percent =
                select_vle_payload_replay_strong_headroom_percent(source);
            set_vle_payload_feedback_reason(
                feedback, VLE_SOURCE_PAYLOAD_REASON_REPLAY_RATIO_OBSERVED);
        }
        else
        {
            feedback->endpoint_headroom_percent =
                (int64)(VLE_PAYLOAD_REPLAY_ENDPOINT_HEADROOM * 100.0 + 0.5);
            set_vle_payload_feedback_reason(
                feedback, VLE_SOURCE_PAYLOAD_REASON_REPLAY_OBSERVED);
        }
    }
    else if (stats->age_adjacency_payload_cache_seed_runs > 0)
    {
        feedback->endpoint_headroom_percent =
            select_vle_payload_seed_headroom_percent(source);
        set_vle_payload_feedback_reason(
            feedback, VLE_SOURCE_PAYLOAD_REASON_CACHE_SEEDED);
    }
}

static int64 select_vle_payload_replay_strong_percent(
    const AgeVLEStreamEdgeSource *source)
{
    if (source != NULL &&
        vle_source_consumer_is_terminal_scalar(
            source->policy_consumer_class_id))
    {
        return VLE_PAYLOAD_REPLAY_TERMINAL_SCALAR_STRONG_PERCENT;
    }

    return VLE_PAYLOAD_REPLAY_STRONG_PERCENT;
}

static int64 select_vle_payload_replay_strong_headroom_percent(
    const AgeVLEStreamEdgeSource *source)
{
    if (source != NULL &&
        source->policy_materialization_weight >= VLE_MATERIALIZATION_WEIGHT_PATH)
    {
        return (int64)(VLE_PAYLOAD_REPLAY_PATH_STRONG_ENDPOINT_HEADROOM *
                       100.0 + 0.5);
    }

    if (source != NULL &&
        source->policy_materialization_weight >=
            VLE_MATERIALIZATION_WEIGHT_OBJECT)
    {
        return (int64)(VLE_PAYLOAD_REPLAY_OBJECT_STRONG_ENDPOINT_HEADROOM *
                       100.0 + 0.5);
    }

    return (int64)(VLE_PAYLOAD_REPLAY_STRONG_ENDPOINT_HEADROOM * 100.0 + 0.5);
}

static int64 select_vle_payload_replay_strong_headroom_percent_for_profile(
    const VLESourcePolicyProfile *profile)
{
    if (profile != NULL &&
        profile->materialization_weight >= VLE_MATERIALIZATION_WEIGHT_PATH)
    {
        return (int64)(VLE_PAYLOAD_REPLAY_PATH_STRONG_ENDPOINT_HEADROOM *
                       100.0 + 0.5);
    }

    if (profile != NULL &&
        profile->materialization_weight >= VLE_MATERIALIZATION_WEIGHT_OBJECT)
    {
        return (int64)(VLE_PAYLOAD_REPLAY_OBJECT_STRONG_ENDPOINT_HEADROOM *
                       100.0 + 0.5);
    }

    return (int64)(VLE_PAYLOAD_REPLAY_STRONG_ENDPOINT_HEADROOM * 100.0 + 0.5);
}

static int64 select_vle_payload_feedback_headroom_percent_for_profile(
    const VLESourcePolicyProfile *profile,
    const VLESourceRuntimeThresholdFeedback *feedback)
{
    int64 headroom_percent;
    VLESourcePolicyClass payload_class_id;

    Assert(profile != NULL);
    Assert(feedback != NULL);

    headroom_percent = feedback->payload_endpoint_headroom_percent;
    payload_class_id = feedback->payload_class_id;
    if (payload_class_id == VLE_SOURCE_POLICY_CLASS_NONE)
        return headroom_percent;

    if (payload_class_id == VLE_SOURCE_POLICY_CLASS_ADJACENCY_REPLAY)
    {
        headroom_percent =
            select_vle_payload_replay_strong_headroom_percent_for_profile(
                profile);
    }
    else if (payload_class_id ==
             VLE_SOURCE_POLICY_CLASS_ADJACENCY_CACHE_SEEDED)
    {
        headroom_percent =
            select_vle_payload_seed_headroom_percent_for_profile(profile);
    }
    else if (payload_class_id ==
             VLE_SOURCE_POLICY_CLASS_ADJACENCY_COMPOSITE_VALUE_POSTING &&
             feedback->replay_runs > 0)
    {
        headroom_percent = Min(
            headroom_percent,
            select_vle_payload_value_posting_headroom_percent_for_profile(
                profile));
        headroom_percent = Min(
            headroom_percent,
            select_vle_payload_replay_strong_headroom_percent_for_profile(
                profile));
    }
    else if (payload_class_id ==
             VLE_SOURCE_POLICY_CLASS_ADJACENCY_COMPOSITE_VALUE_POSTING &&
             feedback->seed_runs > 0)
    {
        headroom_percent = Min(
            headroom_percent,
            select_vle_payload_value_posting_headroom_percent_for_profile(
                profile));
        headroom_percent = Min(
            headroom_percent,
            select_vle_payload_seed_headroom_percent_for_profile(profile));
    }
    else if (payload_class_id ==
             VLE_SOURCE_POLICY_CLASS_ADJACENCY_COMPOSITE_VALUE_POSTING)
    {
        headroom_percent = Min(
            headroom_percent,
            select_vle_payload_value_posting_headroom_percent_for_profile(
                profile));
    }

    return headroom_percent;
}

static int64 select_vle_payload_value_posting_headroom_percent(
    const AgeVLEStreamEdgeSource *source)
{
    if (source != NULL &&
        source->policy_materialization_weight >= VLE_MATERIALIZATION_WEIGHT_PATH)
    {
        return (int64)(VLE_PAYLOAD_VALUE_POSTING_PATH_ENDPOINT_HEADROOM *
                       100.0 + 0.5);
    }

    if (source != NULL &&
        source->policy_materialization_weight >=
            VLE_MATERIALIZATION_WEIGHT_OBJECT)
    {
        return (int64)(VLE_PAYLOAD_VALUE_POSTING_OBJECT_ENDPOINT_HEADROOM *
                       100.0 + 0.5);
    }

    return (int64)(VLE_PAYLOAD_VALUE_POSTING_ENDPOINT_HEADROOM * 100.0 + 0.5);
}

static int64 select_vle_payload_value_posting_headroom_percent_for_profile(
    const VLESourcePolicyProfile *profile)
{
    if (profile != NULL &&
        profile->materialization_weight >= VLE_MATERIALIZATION_WEIGHT_PATH)
    {
        return (int64)(VLE_PAYLOAD_VALUE_POSTING_PATH_ENDPOINT_HEADROOM *
                       100.0 + 0.5);
    }

    if (profile != NULL &&
        profile->materialization_weight >= VLE_MATERIALIZATION_WEIGHT_OBJECT)
    {
        return (int64)(VLE_PAYLOAD_VALUE_POSTING_OBJECT_ENDPOINT_HEADROOM *
                       100.0 + 0.5);
    }

    return (int64)(VLE_PAYLOAD_VALUE_POSTING_ENDPOINT_HEADROOM * 100.0 + 0.5);
}

static int64 select_vle_payload_seed_headroom_percent(
    const AgeVLEStreamEdgeSource *source)
{
    if (source != NULL &&
        vle_source_consumer_is_terminal_scalar(
            source->policy_consumer_class_id))
    {
        return (int64)(VLE_PAYLOAD_SEED_TERMINAL_SCALAR_ENDPOINT_HEADROOM *
                       100.0 + 0.5);
    }

    return (int64)(VLE_PAYLOAD_SEED_ENDPOINT_HEADROOM * 100.0 + 0.5);
}

static int64 select_vle_payload_seed_headroom_percent_for_profile(
    const VLESourcePolicyProfile *profile)
{
    if (profile != NULL &&
        vle_source_consumer_is_terminal_scalar(profile->consumer_class_id))
    {
        return (int64)(VLE_PAYLOAD_SEED_TERMINAL_SCALAR_ENDPOINT_HEADROOM *
                       100.0 + 0.5);
    }

    return (int64)(VLE_PAYLOAD_SEED_ENDPOINT_HEADROOM * 100.0 + 0.5);
}

void record_vle_source_runtime_threshold_feedback(
    const char *graph_name, const char *label_name,
    const AgeVLESourceStats *stats, const AgeVLEStreamEdgeSource *source)
{
    VLESourceRuntimeThresholdFeedback feedback;
    VLESourceRuntimePayloadFeedback payload_feedback;
    VLESourceThresholdCacheKey key;
    VLESourceDirectionClass active_direction_id;
    VLESourceConsumerClass consumer_class_id;

    Assert(stats != NULL);

    active_direction_id = source == NULL ? VLE_SOURCE_DIRECTION_NONE :
        source->policy_active_direction_id;
    derive_vle_source_runtime_threshold_feedback(&feedback, stats, source);
    derive_vle_source_runtime_payload_feedback(&payload_feedback, stats,
                                               source);
    if (((!feedback.eligible || feedback.endpoint_headroom_percent <= 0) &&
         (!payload_feedback.eligible ||
          payload_feedback.endpoint_headroom_percent <= 0)) ||
        graph_name == NULL || label_name == NULL || source == NULL ||
        !OidIsValid(source->edge_label_oid) ||
        source->policy_consumer_class_id == VLE_SOURCE_CONSUMER_UNKNOWN ||
        source->policy_active_direction_id == VLE_SOURCE_DIRECTION_NONE)
    {
        return;
    }
    consumer_class_id = source->policy_consumer_class_id;

    build_vle_source_threshold_cache_key(&key, graph_name, label_name,
                                         source->edge_label_oid,
                                         consumer_class_id,
                                         active_direction_id);
    apply_vle_source_cache_key_runtime_identity(
        &key, source, active_direction_id);
    update_vle_source_threshold_cache_entry(&key, &feedback,
                                            &payload_feedback,
                                            feedback.source_direction_id);
    if (payload_feedback.eligible)
    {
        update_vle_source_payload_family_cache_for_direction(
            graph_name, label_name, source, &payload_feedback,
            active_direction_id);
    }

    if (feedback.source_direction_id != VLE_SOURCE_DIRECTION_NONE &&
        (feedback.source_direction_id == VLE_SOURCE_DIRECTION_BOTH ||
         feedback.source_direction_id !=
         source->policy_active_direction_id))
    {
        if (feedback.source_direction_id == VLE_SOURCE_DIRECTION_BOTH)
        {
            build_vle_source_threshold_cache_key(&key, graph_name, label_name,
                                                 source->edge_label_oid,
                                                 consumer_class_id,
                                                 VLE_SOURCE_DIRECTION_OUT);
            apply_vle_source_cache_key_runtime_identity(
                &key, source, VLE_SOURCE_DIRECTION_OUT);
            update_vle_source_threshold_cache_entry(&key, &feedback,
                                                    &payload_feedback,
                                                    VLE_SOURCE_DIRECTION_OUT);
            update_vle_source_payload_family_cache_for_direction(
                graph_name, label_name, source, &payload_feedback,
                VLE_SOURCE_DIRECTION_OUT);
            build_vle_source_threshold_cache_key(&key, graph_name, label_name,
                                                 source->edge_label_oid,
                                                 consumer_class_id,
                                                 VLE_SOURCE_DIRECTION_IN);
            apply_vle_source_cache_key_runtime_identity(
                &key, source, VLE_SOURCE_DIRECTION_IN);
            update_vle_source_threshold_cache_entry(&key, &feedback,
                                                    &payload_feedback,
                                                    VLE_SOURCE_DIRECTION_IN);
            update_vle_source_payload_family_cache_for_direction(
                graph_name, label_name, source, &payload_feedback,
                VLE_SOURCE_DIRECTION_IN);
        }
        else
        {
            build_vle_source_threshold_cache_key(&key, graph_name, label_name,
                                                 source->edge_label_oid,
                                                 consumer_class_id,
                                                 feedback.source_direction_id);
            apply_vle_source_cache_key_runtime_identity(
                &key, source, feedback.source_direction_id);
            update_vle_source_threshold_cache_entry(&key, &feedback,
                                                    &payload_feedback,
                                                    feedback.source_direction_id);
            update_vle_source_payload_family_cache_for_direction(
                graph_name, label_name, source, &payload_feedback,
                feedback.source_direction_id);
        }
    }
}

static void update_vle_source_threshold_cache_entry(
    const VLESourceThresholdCacheKey *key,
    const VLESourceRuntimeThresholdFeedback *feedback,
    const VLESourceRuntimePayloadFeedback *payload_feedback,
    VLESourceDirectionClass source_direction_id)
{
    VLESourceThresholdCacheEntry *entry;
    bool found;

    Assert(key != NULL);
    Assert(feedback != NULL);
    Assert(payload_feedback != NULL);

    if (source_direction_id == VLE_SOURCE_DIRECTION_NONE)
        source_direction_id = feedback->source_direction_id;

    entry = hash_search(get_vle_source_threshold_cache(), (void *)key,
                        HASH_ENTER, &found);
    if (!found)
        initialize_vle_source_threshold_cache_entry(entry, feedback,
                                                    source_direction_id);
    if (feedback->eligible && feedback->saturated)
    {
        int64 prior_saturated_count = entry->saturated_count;

        entry->saturated_count++;
        entry->relaxed_count = 0;
        entry->endpoint_headroom_percent =
            Min(entry->endpoint_headroom_percent,
                feedback->endpoint_headroom_percent);
        entry->empty_lifecycle_batch_size =
            Max(entry->empty_lifecycle_batch_size,
                feedback->empty_lifecycle_batch_size);
        if (prior_saturated_count > 0)
        {
            VLESourceThresholdFeedbackReason reason;

            entry->endpoint_headroom_percent =
                Min(entry->endpoint_headroom_percent,
                    (int64)(VLE_EMPTY_LIFECYCLE_REPEAT_SATURATED_ENDPOINT_HEADROOM *
                            100.0 + 0.5));
            entry->empty_lifecycle_batch_size =
                Min(Max(entry->empty_lifecycle_batch_size * 2,
                        feedback->empty_lifecycle_batch_size),
                    (int64)VLE_EMPTY_LIFECYCLE_BATCH_MAX);
            reason =
                VLE_SOURCE_THRESHOLD_REASON_ROOT_EMPTY_REPEAT_SATURATED;
            entry->reason_id = reason;
            entry->feedback_class_id =
                classify_vle_threshold_feedback_reason_kind(reason);
        }
        else
        {
            entry->reason_id = feedback->reason_id;
            entry->feedback_class_id = feedback->feedback_class_id;
        }
        entry->source_direction_id = source_direction_id;
    }
    else if (feedback->eligible && feedback->root_empty_completion_count > 0)
    {
        entry->relaxed_count++;
        entry->endpoint_headroom_percent =
            feedback->endpoint_headroom_percent;
        entry->empty_lifecycle_batch_size =
            feedback->empty_lifecycle_batch_size;
        if (entry->saturated_count > 0)
        {
            VLESourceThresholdFeedbackReason reason;

            entry->endpoint_headroom_percent =
                Min(entry->endpoint_headroom_percent,
                    (int64)(VLE_EMPTY_LIFECYCLE_REPEAT_ENDPOINT_HEADROOM *
                            100.0 + 0.5));
            entry->empty_lifecycle_batch_size =
                Max(entry->empty_lifecycle_batch_size,
                    feedback->empty_lifecycle_batch_size);
            reason = VLE_SOURCE_THRESHOLD_REASON_ROOT_EMPTY_REPEAT_OBSERVED;
            entry->reason_id = reason;
            entry->feedback_class_id =
                classify_vle_threshold_feedback_reason_kind(reason);
        }
        else
        {
            entry->reason_id = feedback->reason_id;
            entry->feedback_class_id = feedback->feedback_class_id;
        }
        entry->source_direction_id = source_direction_id;
    }

    if (feedback->eligible)
    {
        switch (entry->source_direction_id)
        {
            case VLE_SOURCE_DIRECTION_OUT:
                entry->directional_empty_completion_count +=
                    feedback->root_empty_completion_out;
                break;
            case VLE_SOURCE_DIRECTION_IN:
                entry->directional_empty_completion_count +=
                    feedback->root_empty_completion_in;
                break;
            case VLE_SOURCE_DIRECTION_BOTH:
                entry->directional_empty_completion_count +=
                    feedback->root_empty_completion_count;
                break;
            case VLE_SOURCE_DIRECTION_NONE:
            case VLE_SOURCE_DIRECTION_MIXED:
                break;
        }
        entry->observed_count++;
    }

    merge_vle_source_payload_cache_entry(entry, payload_feedback);
}

static void initialize_vle_source_threshold_cache_entry(
    VLESourceThresholdCacheEntry *entry,
    const VLESourceRuntimeThresholdFeedback *feedback,
    VLESourceDirectionClass source_direction_id)
{
    Assert(entry != NULL);

    entry->endpoint_headroom_percent =
        feedback != NULL ? feedback->endpoint_headroom_percent : 0;
    entry->empty_lifecycle_batch_size =
        feedback != NULL ? feedback->empty_lifecycle_batch_size : 0;
    entry->observed_count = 0;
    entry->saturated_count = 0;
    entry->relaxed_count = 0;
    entry->payload_endpoint_headroom_percent = 0;
    entry->payload_scan_runs = 0;
    entry->payload_replay_runs = 0;
    entry->payload_seed_runs = 0;
    entry->payload_replay_percent = 0;
    entry->payload_seed_percent = 0;
    entry->payload_observed_count = 0;
    entry->payload_value_posting_observed_count = 0;
    entry->payload_matrix_active_percent = 0;
    entry->payload_matrix_filter_percent = 0;
    entry->payload_matrix_prescan_percent = 0;
    entry->payload_matrix_replay_source_percent = 0;
    entry->payload_matrix_run_block_percent = 0;
    entry->payload_matrix_regroup_percent = 0;
    entry->directional_empty_completion_count = 0;
    entry->source_direction_id = source_direction_id;
    entry->reason_id = feedback != NULL ? feedback->reason_id :
        VLE_SOURCE_THRESHOLD_REASON_NONE;
    entry->feedback_class_id = feedback != NULL ?
        feedback->feedback_class_id :
        VLE_SOURCE_POLICY_CLASS_NONE;
    entry->payload_reason_id = VLE_SOURCE_PAYLOAD_REASON_NONE;
    entry->payload_class_id = VLE_SOURCE_POLICY_CLASS_NONE;
    entry->payload_value_posting_source_kind =
        VLE_SOURCE_VALUE_POSTING_NONE;
}

static void
merge_vle_source_payload_cache_entry(
    VLESourceThresholdCacheEntry *entry,
    const VLESourceRuntimePayloadFeedback *payload_feedback)
{
    Assert(entry != NULL);
    Assert(payload_feedback != NULL);

    if (!payload_feedback->eligible)
        return;

    entry->payload_endpoint_headroom_percent =
        entry->payload_endpoint_headroom_percent <= 0 ?
        payload_feedback->endpoint_headroom_percent :
        Min(entry->payload_endpoint_headroom_percent,
            payload_feedback->endpoint_headroom_percent);
    entry->payload_scan_runs += payload_feedback->scan_runs;
    entry->payload_replay_runs += payload_feedback->replay_runs;
    entry->payload_seed_runs += payload_feedback->seed_runs;
    entry->payload_replay_percent = calculate_vle_source_ratio_percent(
        entry->payload_replay_runs, entry->payload_scan_runs);
    entry->payload_seed_percent = calculate_vle_source_ratio_percent(
        entry->payload_seed_runs, entry->payload_scan_runs);
    entry->payload_observed_count++;
    entry->payload_value_posting_observed_count +=
        payload_feedback->value_posting_observed_count;
    entry->payload_matrix_active_percent =
        Max(entry->payload_matrix_active_percent,
            payload_feedback->matrix_active_percent);
    entry->payload_matrix_filter_percent =
        Max(entry->payload_matrix_filter_percent,
            payload_feedback->matrix_filter_percent);
    entry->payload_matrix_prescan_percent =
        Max(entry->payload_matrix_prescan_percent,
            payload_feedback->matrix_prescan_percent);
    entry->payload_matrix_replay_source_percent =
        Max(entry->payload_matrix_replay_source_percent,
            payload_feedback->matrix_replay_source_percent);
    entry->payload_matrix_run_block_percent =
        Max(entry->payload_matrix_run_block_percent,
            payload_feedback->matrix_run_block_percent);
    entry->payload_matrix_regroup_percent =
        Max(entry->payload_matrix_regroup_percent,
            payload_feedback->matrix_regroup_percent);

    if (entry->payload_observed_count == 1 ||
        vle_source_payload_class_rank_id(payload_feedback->feedback_class_id) >
        vle_source_payload_class_rank_id(entry->payload_class_id))
    {
        entry->payload_reason_id = payload_feedback->reason_id;
        entry->payload_class_id = payload_feedback->feedback_class_id;
        entry->payload_value_posting_source_kind =
            payload_feedback->value_posting_source_kind;
    }
}

static void update_vle_source_payload_family_cache_entry(
    const VLESourceThresholdCacheKey *key,
    const VLESourceRuntimePayloadFeedback *payload_feedback)
{
    VLESourceThresholdCacheEntry *entry;
    bool found;

    Assert(key != NULL);
    Assert(payload_feedback != NULL);

    if (!payload_feedback->eligible)
        return;

    entry = hash_search(get_vle_source_threshold_cache(), (void *)key,
                        HASH_ENTER, &found);
    if (!found)
        initialize_vle_source_threshold_cache_entry(
            entry, NULL, VLE_SOURCE_DIRECTION_NONE);

    merge_vle_source_payload_cache_entry(entry, payload_feedback);
}

static void update_vle_source_payload_family_cache_for_direction(
    const char *graph_name, const char *label_name,
    const AgeVLEStreamEdgeSource *source,
    const VLESourceRuntimePayloadFeedback *payload_feedback,
    VLESourceDirectionClass source_direction_id)
{
    VLESourceThresholdCacheKey key;
    VLESourceConsumerClass consumer_class_id;
    VLESourceConsumerClass payload_family_id;

    Assert(source != NULL);
    Assert(payload_feedback != NULL);

    if (!payload_feedback->eligible ||
        graph_name == NULL ||
        label_name == NULL ||
        source_direction_id == VLE_SOURCE_DIRECTION_NONE ||
        source->policy_consumer_class_id == VLE_SOURCE_CONSUMER_UNKNOWN)
    {
        return;
    }

    consumer_class_id = source->policy_consumer_class_id;
    payload_family_id = vle_payload_feedback_consumer_family_id(
        consumer_class_id);
    if (payload_family_id == consumer_class_id)
    {
        return;
    }
    build_vle_source_threshold_cache_key(&key, graph_name, label_name,
                                         source->edge_label_oid,
                                         payload_family_id,
                                         source_direction_id);
    apply_vle_source_cache_key_runtime_identity(&key, source,
                                                source_direction_id);
    update_vle_source_payload_family_cache_entry(&key, payload_feedback);
}

static HTAB *get_vle_source_threshold_cache(void)
{
    static HTAB *threshold_cache = NULL;
    HASHCTL ctl;

    if (threshold_cache != NULL)
        return threshold_cache;

    memset(&ctl, 0, sizeof(ctl));
    ctl.keysize = sizeof(VLESourceThresholdCacheKey);
    ctl.entrysize = sizeof(VLESourceThresholdCacheEntry);
    ctl.hcxt = TopMemoryContext;
    threshold_cache = hash_create("AGE VLE source threshold feedback",
                                  128, &ctl,
                                  HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);

    return threshold_cache;
}

static bool lookup_vle_source_threshold_feedback(
    const VLEStreamSourceCostInput *input,
    const VLESourcePolicyProfile *profile,
    VLESourceRuntimeThresholdFeedback *feedback)
{
    VLESourceThresholdCacheKey key;
    VLESourceThresholdCacheEntry *entry;

    Assert(input != NULL);
    Assert(profile != NULL);
    Assert(feedback != NULL);

    feedback->eligible = false;
    feedback->saturated = false;
    feedback->endpoint_headroom_percent = 0;
    feedback->empty_lifecycle_batch_size = 0;
    feedback->root_empty_completion_count = 0;
    feedback->root_empty_completion_out = 0;
    feedback->root_empty_completion_in = 0;
    feedback->observed_count = 0;
    feedback->saturated_count = 0;
    feedback->relaxed_count = 0;
    feedback->out_observed_count = 0;
    feedback->in_observed_count = 0;
    feedback->out_saturated_count = 0;
    feedback->in_saturated_count = 0;
    feedback->scan_runs = 0;
    feedback->replay_runs = 0;
    feedback->seed_runs = 0;
    feedback->replay_percent = 0;
    feedback->seed_percent = 0;
    feedback->payload_observed_count = 0;
    feedback->payload_endpoint_headroom_percent = 0;
    feedback->payload_value_posting_observed_count = 0;
    feedback->payload_matrix_active_percent = 0;
    feedback->payload_matrix_filter_percent = 0;
    feedback->payload_matrix_prescan_percent = 0;
    feedback->payload_matrix_replay_source_percent = 0;
    feedback->payload_matrix_run_block_percent = 0;
    feedback->payload_matrix_regroup_percent = 0;
    feedback->source_direction_id = VLE_SOURCE_DIRECTION_NONE;
    feedback->reason_id = VLE_SOURCE_THRESHOLD_REASON_NONE;
    feedback->feedback_class_id = VLE_SOURCE_POLICY_CLASS_NONE;
    feedback->payload_reason_id = VLE_SOURCE_PAYLOAD_REASON_NONE;
    feedback->payload_class_id = VLE_SOURCE_POLICY_CLASS_NONE;
    feedback->payload_value_posting_source_kind =
        VLE_SOURCE_VALUE_POSTING_NONE;

    if ((!profile->cache_seed_eligible &&
         !profile->composite_prefilter_planned) ||
        input->graph_name == NULL || input->label_name == NULL ||
        profile->consumer_class_id == VLE_SOURCE_CONSUMER_UNKNOWN ||
        profile->direction_class_id == VLE_SOURCE_DIRECTION_NONE)
    {
        return false;
    }

    build_vle_source_threshold_cache_key(&key, input->graph_name,
                                         input->label_name,
                                         input->evidence->edge_label_oid,
                                         profile->consumer_class_id,
                                         profile->direction_class_id);
    apply_vle_source_cache_key_input_identity(&key, input,
                                              profile->direction_class_id);
    entry = hash_search(get_vle_source_threshold_cache(), &key,
                        HASH_FIND, NULL);
    if (entry == NULL)
    {
        if (lookup_vle_source_directional_family_feedback(input, profile,
                                                          feedback))
            return true;

        return lookup_vle_source_payload_family_feedback(input, profile,
                                                         feedback);
    }

    feedback->eligible = true;
    feedback->endpoint_headroom_percent = entry->endpoint_headroom_percent;
    feedback->empty_lifecycle_batch_size = entry->empty_lifecycle_batch_size;
    feedback->observed_count = entry->observed_count;
    feedback->saturated_count = entry->saturated_count;
    feedback->relaxed_count = entry->relaxed_count;
    feedback->payload_observed_count = entry->payload_observed_count;
    if (entry->payload_observed_count > 0 &&
        vle_payload_feedback_class_matches_profile(
            profile, entry->payload_class_id))
    {
        feedback->scan_runs = entry->payload_scan_runs;
        feedback->replay_runs = entry->payload_replay_runs;
        feedback->seed_runs = entry->payload_seed_runs;
        feedback->replay_percent = entry->payload_replay_percent;
        feedback->seed_percent = entry->payload_seed_percent;
        feedback->payload_endpoint_headroom_percent =
            entry->payload_endpoint_headroom_percent;
        feedback->payload_reason_id = entry->payload_reason_id;
        feedback->payload_class_id = entry->payload_class_id;
        feedback->payload_value_posting_observed_count =
            entry->payload_value_posting_observed_count;
        feedback->payload_matrix_active_percent =
            entry->payload_matrix_active_percent;
        feedback->payload_matrix_filter_percent =
            entry->payload_matrix_filter_percent;
        feedback->payload_matrix_prescan_percent =
            entry->payload_matrix_prescan_percent;
        feedback->payload_matrix_replay_source_percent =
            entry->payload_matrix_replay_source_percent;
        feedback->payload_matrix_run_block_percent =
            entry->payload_matrix_run_block_percent;
        feedback->payload_matrix_regroup_percent =
            entry->payload_matrix_regroup_percent;
        feedback->payload_value_posting_source_kind =
            entry->payload_value_posting_source_kind;
    }
    feedback->source_direction_id = entry->source_direction_id;
    feedback->reason_id = entry->reason_id;
    feedback->feedback_class_id = entry->feedback_class_id;

    if (feedback->payload_observed_count <= 0)
        (void)lookup_vle_source_payload_family_feedback(input, profile,
                                                        feedback);

    return true;
}

static bool lookup_vle_source_directional_family_feedback(
    const VLEStreamSourceCostInput *input,
    const VLESourcePolicyProfile *profile,
    VLESourceRuntimeThresholdFeedback *feedback)
{
    VLESourceThresholdCacheKey key;
    VLESourceThresholdCacheEntry *entry;
    bool found = false;

    Assert(input != NULL);
    Assert(profile != NULL);
    Assert(feedback != NULL);

    if (profile->direction_class_id != VLE_SOURCE_DIRECTION_BOTH ||
        input->graph_name == NULL ||
        input->label_name == NULL ||
        profile->consumer_class_id == VLE_SOURCE_CONSUMER_UNKNOWN)
    {
        return false;
    }

    build_vle_source_threshold_cache_key(&key, input->graph_name,
                                         input->label_name,
                                         input->evidence->edge_label_oid,
                                         profile->consumer_class_id,
                                         VLE_SOURCE_DIRECTION_OUT);
    apply_vle_source_cache_key_input_identity(
        &key, input, VLE_SOURCE_DIRECTION_OUT);
    entry = hash_search(get_vle_source_threshold_cache(), &key,
                        HASH_FIND, NULL);
    if (entry != NULL)
        found = merge_vle_source_directional_feedback_entry(feedback, profile,
                                                            entry);

    build_vle_source_threshold_cache_key(&key, input->graph_name,
                                         input->label_name,
                                         input->evidence->edge_label_oid,
                                         profile->consumer_class_id,
                                         VLE_SOURCE_DIRECTION_IN);
    apply_vle_source_cache_key_input_identity(
        &key, input, VLE_SOURCE_DIRECTION_IN);
    entry = hash_search(get_vle_source_threshold_cache(), &key,
                        HASH_FIND, NULL);
    if (entry != NULL)
        found = merge_vle_source_directional_feedback_entry(feedback, profile,
                                                            entry) || found;

    if (found && feedback->source_direction_id == VLE_SOURCE_DIRECTION_NONE)
    {
        feedback->source_direction_id = VLE_SOURCE_DIRECTION_MIXED;
    }

    return found;
}

static bool merge_vle_source_directional_feedback_entry(
    VLESourceRuntimeThresholdFeedback *feedback,
    const VLESourcePolicyProfile *profile,
    const VLESourceThresholdCacheEntry *entry)
{
    bool has_threshold;
    bool has_payload;

    Assert(feedback != NULL);
    Assert(entry != NULL);

    has_threshold = entry->observed_count > 0;
    has_payload = entry->payload_observed_count > 0 &&
        vle_payload_feedback_class_matches_profile(
            profile, entry->payload_class_id);

    if (!has_threshold && !has_payload)
        return false;

    feedback->eligible = true;

    if (has_threshold)
    {
        switch (entry->source_direction_id)
        {
            case VLE_SOURCE_DIRECTION_OUT:
                feedback->out_observed_count +=
                    entry->directional_empty_completion_count > 0 ?
                    entry->directional_empty_completion_count :
                    entry->observed_count;
                feedback->out_saturated_count += entry->saturated_count;
                break;
            case VLE_SOURCE_DIRECTION_IN:
                feedback->in_observed_count +=
                    entry->directional_empty_completion_count > 0 ?
                    entry->directional_empty_completion_count :
                    entry->observed_count;
                feedback->in_saturated_count += entry->saturated_count;
                break;
            case VLE_SOURCE_DIRECTION_BOTH:
            {
                int64 completion_count =
                    entry->directional_empty_completion_count > 0 ?
                    entry->directional_empty_completion_count :
                    entry->observed_count;

                feedback->out_observed_count += completion_count;
                feedback->in_observed_count += completion_count;
                feedback->out_saturated_count += entry->saturated_count;
                feedback->in_saturated_count += entry->saturated_count;
                break;
            }
            case VLE_SOURCE_DIRECTION_NONE:
            case VLE_SOURCE_DIRECTION_MIXED:
                break;
        }

        if (feedback->observed_count <= 0)
        {
            feedback->endpoint_headroom_percent =
                entry->endpoint_headroom_percent;
            feedback->empty_lifecycle_batch_size =
                entry->empty_lifecycle_batch_size;
            feedback->source_direction_id = entry->source_direction_id;
            feedback->reason_id = entry->reason_id;
            feedback->feedback_class_id = entry->feedback_class_id;
        }
        else
        {
            if (entry->endpoint_headroom_percent > 0)
            {
                feedback->endpoint_headroom_percent =
                    feedback->endpoint_headroom_percent <= 0 ?
                    entry->endpoint_headroom_percent :
                    Min(feedback->endpoint_headroom_percent,
                        entry->endpoint_headroom_percent);
            }
            feedback->empty_lifecycle_batch_size =
                Max(feedback->empty_lifecycle_batch_size,
                    entry->empty_lifecycle_batch_size);
            if (feedback->source_direction_id !=
                entry->source_direction_id)
            {
                feedback->source_direction_id = VLE_SOURCE_DIRECTION_MIXED;
            }
            if (vle_source_threshold_class_rank_id(
                    entry->feedback_class_id) >
                vle_source_threshold_class_rank_id(
                    feedback->feedback_class_id))
            {
                feedback->feedback_class_id = entry->feedback_class_id;
                feedback->reason_id = entry->reason_id;
            }
        }
        feedback->observed_count += entry->observed_count;
        feedback->saturated_count += entry->saturated_count;
        feedback->relaxed_count += entry->relaxed_count;
    }

    if (has_payload)
    {
        if (feedback->payload_observed_count <= 0)
        {
            feedback->payload_endpoint_headroom_percent =
                entry->payload_endpoint_headroom_percent;
            feedback->payload_reason_id = entry->payload_reason_id;
            feedback->payload_class_id = entry->payload_class_id;
            feedback->payload_value_posting_observed_count =
                entry->payload_value_posting_observed_count;
            feedback->payload_matrix_active_percent =
                entry->payload_matrix_active_percent;
            feedback->payload_matrix_filter_percent =
                entry->payload_matrix_filter_percent;
            feedback->payload_matrix_prescan_percent =
                entry->payload_matrix_prescan_percent;
            feedback->payload_matrix_replay_source_percent =
                entry->payload_matrix_replay_source_percent;
            feedback->payload_matrix_run_block_percent =
                entry->payload_matrix_run_block_percent;
            feedback->payload_matrix_regroup_percent =
                entry->payload_matrix_regroup_percent;
            feedback->payload_value_posting_source_kind =
                entry->payload_value_posting_source_kind;
        }
        else
        {
            if (entry->payload_endpoint_headroom_percent > 0)
            {
                feedback->payload_endpoint_headroom_percent =
                    feedback->payload_endpoint_headroom_percent <= 0 ?
                    entry->payload_endpoint_headroom_percent :
                    Min(feedback->payload_endpoint_headroom_percent,
                        entry->payload_endpoint_headroom_percent);
            }
            if (vle_source_payload_class_rank_id(entry->payload_class_id) >
                vle_source_payload_class_rank_id(
                    feedback->payload_class_id))
            {
                feedback->payload_class_id = entry->payload_class_id;
                feedback->payload_reason_id = entry->payload_reason_id;
                feedback->payload_value_posting_source_kind =
                    entry->payload_value_posting_source_kind;
            }
        }
        feedback->payload_value_posting_observed_count +=
            entry->payload_value_posting_observed_count;
        feedback->payload_matrix_active_percent =
            Max(feedback->payload_matrix_active_percent,
                entry->payload_matrix_active_percent);
        feedback->payload_matrix_filter_percent =
            Max(feedback->payload_matrix_filter_percent,
                entry->payload_matrix_filter_percent);
        feedback->payload_matrix_prescan_percent =
            Max(feedback->payload_matrix_prescan_percent,
                entry->payload_matrix_prescan_percent);
        feedback->payload_matrix_replay_source_percent =
            Max(feedback->payload_matrix_replay_source_percent,
                entry->payload_matrix_replay_source_percent);
        feedback->payload_matrix_run_block_percent =
            Max(feedback->payload_matrix_run_block_percent,
                entry->payload_matrix_run_block_percent);
        feedback->payload_matrix_regroup_percent =
            Max(feedback->payload_matrix_regroup_percent,
                entry->payload_matrix_regroup_percent);
        feedback->scan_runs += entry->payload_scan_runs;
        feedback->replay_runs += entry->payload_replay_runs;
        feedback->seed_runs += entry->payload_seed_runs;
        feedback->replay_percent = calculate_vle_source_ratio_percent(
            feedback->replay_runs, feedback->scan_runs);
        feedback->seed_percent = calculate_vle_source_ratio_percent(
            feedback->seed_runs, feedback->scan_runs);
        feedback->payload_observed_count += entry->payload_observed_count;
    }

    return true;
}

static bool lookup_vle_source_payload_family_feedback(
    const VLEStreamSourceCostInput *input,
    const VLESourcePolicyProfile *profile,
    VLESourceRuntimeThresholdFeedback *feedback)
{
    VLESourceThresholdCacheKey key;
    VLESourceThresholdCacheEntry *entry;
    VLESourceConsumerClass payload_family_id;

    Assert(input != NULL);
    Assert(profile != NULL);
    Assert(feedback != NULL);

    payload_family_id = vle_payload_feedback_consumer_family_id(
        profile->consumer_class_id);
    if (payload_family_id == profile->consumer_class_id ||
        input->graph_name == NULL ||
        input->label_name == NULL ||
        profile->direction_class_id == VLE_SOURCE_DIRECTION_NONE)
    {
        return false;
    }
    build_vle_source_threshold_cache_key(&key, input->graph_name,
                                         input->label_name,
                                         input->evidence->edge_label_oid,
                                         payload_family_id,
                                         profile->direction_class_id);
    apply_vle_source_cache_key_input_identity(&key, input,
                                              profile->direction_class_id);
    entry = hash_search(get_vle_source_threshold_cache(), &key,
                        HASH_FIND, NULL);
    if (entry == NULL || entry->payload_observed_count <= 0 ||
        !vle_payload_feedback_class_matches_profile(
            profile, entry->payload_class_id))
        return false;

    feedback->eligible = true;
    feedback->scan_runs = entry->payload_scan_runs;
    feedback->replay_runs = entry->payload_replay_runs;
    feedback->seed_runs = entry->payload_seed_runs;
    feedback->replay_percent = entry->payload_replay_percent;
    feedback->seed_percent = entry->payload_seed_percent;
    feedback->payload_endpoint_headroom_percent =
        entry->payload_endpoint_headroom_percent;
    feedback->payload_observed_count = entry->payload_observed_count;
    feedback->payload_reason_id = entry->payload_reason_id;
    feedback->payload_class_id = entry->payload_class_id;
    feedback->payload_value_posting_observed_count =
        entry->payload_value_posting_observed_count;
    feedback->payload_matrix_active_percent =
        entry->payload_matrix_active_percent;
    feedback->payload_matrix_filter_percent =
        entry->payload_matrix_filter_percent;
    feedback->payload_matrix_prescan_percent =
        entry->payload_matrix_prescan_percent;
    feedback->payload_matrix_replay_source_percent =
        entry->payload_matrix_replay_source_percent;
    feedback->payload_matrix_run_block_percent =
        entry->payload_matrix_run_block_percent;
    feedback->payload_matrix_regroup_percent =
        entry->payload_matrix_regroup_percent;
    feedback->payload_value_posting_source_kind =
        entry->payload_value_posting_source_kind;

    return true;
}

static bool vle_payload_feedback_class_matches_profile(
    const VLESourcePolicyProfile *profile, VLESourcePolicyClass payload_class)
{
    if (payload_class == VLE_SOURCE_POLICY_CLASS_NONE)
        return true;

    if (vle_source_policy_is_composite_class_id(payload_class))
        return profile != NULL && profile->composite_prefilter_planned;

    return true;
}

const char *age_vle_output_requirement_name(
    AgeVLEOutputRequirement requirement)
{
    switch (requirement)
    {
        case AGE_VLE_OUTPUT_REQUIREMENT_PATH:
            return "path";
        case AGE_VLE_OUTPUT_REQUIREMENT_TERMINAL_VERTEX:
            return "terminal-vertex";
        case AGE_VLE_OUTPUT_REQUIREMENT_TERMINAL_PROPERTIES:
            return "terminal-properties";
        case AGE_VLE_OUTPUT_REQUIREMENT_TERMINAL_PROPERTY:
            return "terminal-property";
        case AGE_VLE_OUTPUT_REQUIREMENT_UNKNOWN:
            break;
    }

    return "unknown";
}

const char *age_vle_source_policy_class_name(
    VLESourcePolicyClass source_class)
{
    switch (source_class)
    {
        case VLE_SOURCE_POLICY_CLASS_ADJACENCY_WORK:
            return "adjacency-work";
        case VLE_SOURCE_POLICY_CLASS_ADJACENCY_FEEDBACK:
            return "adjacency-feedback";
        case VLE_SOURCE_POLICY_CLASS_ADJACENCY_CACHE_SEEDED:
            return "adjacency-cache-seeded";
        case VLE_SOURCE_POLICY_CLASS_ADJACENCY_EMPTY_LIFECYCLE:
            return "adjacency-empty-lifecycle";
        case VLE_SOURCE_POLICY_CLASS_ADJACENCY_EMPTY_BATCH:
            return "adjacency-empty-batch";
        case VLE_SOURCE_POLICY_CLASS_ADJACENCY_REPLAY:
            return "adjacency-replay";
        case VLE_SOURCE_POLICY_CLASS_ADJACENCY_REPLAY_OBSERVED:
            return "adjacency-replay-observed";
        case VLE_SOURCE_POLICY_CLASS_ADJACENCY_PAYLOAD_SCAN:
            return "adjacency-payload-scan";
        case VLE_SOURCE_POLICY_CLASS_ADJACENCY_PAYLOAD:
            return "adjacency-payload";
        case VLE_SOURCE_POLICY_CLASS_ADJACENCY_COMPOSITE_PREFILTER:
            return "adjacency-composite-prefilter";
        case VLE_SOURCE_POLICY_CLASS_ADJACENCY_COMPOSITE_VALUE_POSTING:
            return "adjacency-composite-value-posting";
        case VLE_SOURCE_POLICY_CLASS_MATRIX_FRONTIER_PRE_SCAN:
            return "matrix-frontier-pre-scan";
        case VLE_SOURCE_POLICY_CLASS_LAYOUT:
            return "layout";
        case VLE_SOURCE_POLICY_CLASS_ADJACENCY_STREAM:
            return "adjacency-stream";
        case VLE_SOURCE_POLICY_CLASS_ADJACENCY_MATERIALIZED_TIE:
            return "adjacency-materialized-tie";
        case VLE_SOURCE_POLICY_CLASS_ADJACENCY_WORK_TIE:
            return "adjacency-work-tie";
        case VLE_SOURCE_POLICY_CLASS_ADJACENCY_COMBINED_WORK_TIE:
            return "adjacency-combined-work-tie";
        case VLE_SOURCE_POLICY_CLASS_ADJACENCY_COMBINED_WORK:
            return "adjacency-combined-work";
        case VLE_SOURCE_POLICY_CLASS_ADJACENCY_ONLY:
            return "adjacency-only";
        case VLE_SOURCE_POLICY_CLASS_ENDPOINT_DIRECT:
            return "endpoint-direct";
        case VLE_SOURCE_POLICY_CLASS_NO_SOURCE:
            return "no-source";
        case VLE_SOURCE_POLICY_CLASS_IDLE:
            return "idle";
        case VLE_SOURCE_POLICY_CLASS_MISSING_VERTEX_SOURCE:
            return "missing-vertex-source";
        case VLE_SOURCE_POLICY_CLASS_PACKED_POLICY_SUPPRESSED:
            return "packed-policy-suppressed";
        case VLE_SOURCE_POLICY_CLASS_PACKED_FALLBACK:
            return "packed-fallback";
        case VLE_SOURCE_POLICY_CLASS_NONE:
            break;
    }

    return "none";
}

static void set_vle_source_runtime_feedback_class(
    VLESourceRuntimeFeedback *feedback, VLESourcePolicyClass source_class)
{
    Assert(feedback != NULL);

    feedback->source_class_id = source_class;
}

static void copy_vle_source_runtime_feedback_class(
    VLESourceRuntimeFeedback *feedback, VLESourcePolicyClass source_class_id)
{
    Assert(feedback != NULL);

    feedback->source_class_id = source_class_id;
}

static void set_vle_source_policy_feedback_class(
    VLESourcePolicyFeedback *feedback, VLESourcePolicyClass source_class)
{
    Assert(feedback != NULL);

    feedback->source_class_id = source_class;
}

static int vle_source_threshold_class_rank_id(
    VLESourcePolicyClass source_class)
{
    switch (source_class)
    {
        case VLE_SOURCE_POLICY_CLASS_MATRIX_FRONTIER_PRE_SCAN:
            return 5;
        case VLE_SOURCE_POLICY_CLASS_ADJACENCY_EMPTY_BATCH:
            return 4;
        case VLE_SOURCE_POLICY_CLASS_ADJACENCY_EMPTY_LIFECYCLE:
            return 3;
        case VLE_SOURCE_POLICY_CLASS_ADJACENCY_CACHE_SEEDED:
            return 2;
        case VLE_SOURCE_POLICY_CLASS_ADJACENCY_FEEDBACK:
            return 1;
        default:
            return 0;
    }
}

static int vle_source_payload_class_rank_id(VLESourcePolicyClass source_class)
{
    switch (source_class)
    {
        case VLE_SOURCE_POLICY_CLASS_ADJACENCY_COMPOSITE_VALUE_POSTING:
            return 6;
        case VLE_SOURCE_POLICY_CLASS_ADJACENCY_COMPOSITE_PREFILTER:
            return 5;
        case VLE_SOURCE_POLICY_CLASS_ADJACENCY_REPLAY:
        case VLE_SOURCE_POLICY_CLASS_MATRIX_FRONTIER_PRE_SCAN:
            return 4;
        case VLE_SOURCE_POLICY_CLASS_ADJACENCY_REPLAY_OBSERVED:
            return 3;
        case VLE_SOURCE_POLICY_CLASS_ADJACENCY_CACHE_SEEDED:
            return 2;
        case VLE_SOURCE_POLICY_CLASS_ADJACENCY_PAYLOAD_SCAN:
            return 1;
        default:
            return 0;
    }
}

static bool vle_source_policy_is_empty_lifecycle_class_id(
    VLESourcePolicyClass source_class)
{
    return source_class == VLE_SOURCE_POLICY_CLASS_ADJACENCY_CACHE_SEEDED ||
           source_class ==
           VLE_SOURCE_POLICY_CLASS_ADJACENCY_EMPTY_LIFECYCLE ||
           source_class == VLE_SOURCE_POLICY_CLASS_ADJACENCY_EMPTY_BATCH ||
           source_class == VLE_SOURCE_POLICY_CLASS_MATRIX_FRONTIER_PRE_SCAN;
}

static bool vle_source_policy_is_composite_class_id(
    VLESourcePolicyClass source_class)
{
    return source_class ==
           VLE_SOURCE_POLICY_CLASS_ADJACENCY_COMPOSITE_PREFILTER ||
           source_class ==
           VLE_SOURCE_POLICY_CLASS_ADJACENCY_COMPOSITE_VALUE_POSTING;
}

static bool vle_source_direction_is_directional_family(
    VLESourceDirectionClass direction)
{
    return direction == VLE_SOURCE_DIRECTION_OUT ||
           direction == VLE_SOURCE_DIRECTION_IN ||
           direction == VLE_SOURCE_DIRECTION_MIXED;
}

const char *age_vle_source_consumer_class_name(
    VLESourceConsumerClass consumer_class)
{
    switch (consumer_class)
    {
        case VLE_SOURCE_CONSUMER_TERMINAL_SCALAR:
            return "terminal-scalar";
        case VLE_SOURCE_CONSUMER_TERMINAL_OBJECT:
            return "terminal-object";
        case VLE_SOURCE_CONSUMER_PATH_MATERIALIZED:
            return "path-materialized";
        case VLE_SOURCE_CONSUMER_MATERIALIZED:
            return "materialized";
        case VLE_SOURCE_CONSUMER_UNKNOWN:
            break;
    }

    return "unknown";
}

static bool vle_source_consumer_is_terminal_scalar(
    VLESourceConsumerClass consumer_class)
{
    return consumer_class == VLE_SOURCE_CONSUMER_TERMINAL_SCALAR;
}

static bool vle_source_consumer_is_materialized(
    VLESourceConsumerClass consumer_class)
{
    return consumer_class == VLE_SOURCE_CONSUMER_TERMINAL_OBJECT ||
           consumer_class == VLE_SOURCE_CONSUMER_PATH_MATERIALIZED ||
           consumer_class == VLE_SOURCE_CONSUMER_MATERIALIZED;
}

static void build_vle_source_threshold_cache_key(
    VLESourceThresholdCacheKey *key, const char *graph_name,
    const char *label_name, Oid edge_label_oid,
    VLESourceConsumerClass consumer_class_id,
    VLESourceDirectionClass active_direction_id)
{
    Assert(key != NULL);

    memset(key, 0, sizeof(*key));
    if (graph_name != NULL)
        strlcpy(key->graph_name, graph_name, sizeof(key->graph_name));
    if (label_name != NULL)
        strlcpy(key->label_name, label_name, sizeof(key->label_name));
    key->edge_label_oid = edge_label_oid;
    key->consumer_class_id = consumer_class_id;
    key->active_direction_id = active_direction_id;
    key->value_posting_source_kind = VLE_SOURCE_VALUE_POSTING_NONE;
}

static void apply_vle_source_cache_key_composite_identity(
    VLESourceThresholdCacheKey *key, int32 terminal_label_id,
    Oid terminal_property_index_oid, uint32 terminal_property_filter_id,
    VLESourceValuePostingKind value_posting_source_kind)
{
    Assert(key != NULL);

    if (!label_id_is_valid(terminal_label_id) ||
        !OidIsValid(terminal_property_index_oid) ||
        terminal_property_filter_id == 0)
    {
        return;
    }

    key->terminal_label_id = terminal_label_id;
    key->terminal_property_index_oid = terminal_property_index_oid;
    key->terminal_property_filter_id = terminal_property_filter_id;
    key->value_posting_source_kind = value_posting_source_kind;
}

static void apply_vle_source_cache_key_input_identity(
    VLESourceThresholdCacheKey *key, const VLEStreamSourceCostInput *input,
    VLESourceDirectionClass source_direction_id)
{
    VLESourceValuePostingKind value_posting_source_kind;

    Assert(key != NULL);
    Assert(input != NULL);

    if (!input->composite_prefilter_planned)
        return;

    value_posting_source_kind =
        vle_source_input_value_posting_source_kind_for_direction(
            input, source_direction_id);
    apply_vle_source_cache_key_composite_identity(
        key, input->terminal_label_id, input->terminal_property_index_oid,
        input->terminal_property_filter_id, value_posting_source_kind);
}

static void apply_vle_source_cache_key_runtime_identity(
    VLESourceThresholdCacheKey *key,
    const AgeVLEStreamEdgeSource *source,
    VLESourceDirectionClass source_direction_id)
{
    VLESourceValuePostingKind value_posting_source_kind;

    Assert(key != NULL);
    Assert(source != NULL);

    if (!source->terminal_property_prefilter_eligible)
        return;

    value_posting_source_kind =
        age_vle_source_value_posting_source_kind_for_direction(
            source, source_direction_id);
    apply_vle_source_cache_key_composite_identity(
        key, source->terminal_label_id, source->terminal_property_index_oid,
        source->terminal_property_filter_id, value_posting_source_kind);
}

static VLESourceConsumerClass vle_payload_feedback_consumer_family_id(
    VLESourceConsumerClass consumer_class)
{
    if (consumer_class == VLE_SOURCE_CONSUMER_PATH_MATERIALIZED ||
        consumer_class == VLE_SOURCE_CONSUMER_TERMINAL_OBJECT)
        return VLE_SOURCE_CONSUMER_MATERIALIZED;

    return consumer_class;
}

static bool vle_runtime_dominant_matches_plan(
    const VLESourceRuntimeDominant *dominant,
    const AgeVLEStreamEdgeSource *source)
{
    Assert(dominant != NULL);

    if (source == NULL ||
        dominant->kind == VLE_SOURCE_RUNTIME_DOMINANT_NONE)
        return true;

    switch (dominant->kind)
    {
        case VLE_SOURCE_RUNTIME_DOMINANT_AGE_ADJACENCY:
            return source->outgoing_kind ==
                       AGE_VLE_STREAM_DIRECTED_SOURCE_AGE_ADJACENCY ||
                   source->incoming_kind ==
                       AGE_VLE_STREAM_DIRECTED_SOURCE_AGE_ADJACENCY;
        case VLE_SOURCE_RUNTIME_DOMINANT_ENDPOINT_BTREE:
            return source->outgoing_kind ==
                       AGE_VLE_STREAM_DIRECTED_SOURCE_ENDPOINT_BTREE ||
                   source->incoming_kind ==
                       AGE_VLE_STREAM_DIRECTED_SOURCE_ENDPOINT_BTREE;
        case VLE_SOURCE_RUNTIME_DOMINANT_PACKED:
            return source->kind == AGE_VLE_STREAM_EDGE_SOURCE_GLOBAL_METADATA;
        case VLE_SOURCE_RUNTIME_DOMINANT_NONE:
            break;
    }

    return false;
}

static bool vle_runtime_class_matches_plan(
    const VLESourceRuntimeFeedback *feedback,
    const AgeVLEStreamEdgeSource *source)
{
    Assert(feedback != NULL);

    if (source == NULL)
        return true;

    if (source->policy_class_id == feedback->source_class_id)
        return true;

    /*
     * Runtime payload replay is a stronger provided property of a planned
     * age_adjacency lifecycle.  Treat it as satisfying cache-seeded and
     * composite source plans instead of reporting a source policy mismatch.
     */
    if (feedback->source_class_id == VLE_SOURCE_POLICY_CLASS_ADJACENCY_REPLAY &&
        source->cache_seed_eligible &&
        (vle_source_policy_is_empty_lifecycle_class_id(
             source->policy_class_id) ||
         vle_source_policy_is_composite_class_id(source->policy_class_id)) &&
        (source->outgoing_kind ==
         AGE_VLE_STREAM_DIRECTED_SOURCE_AGE_ADJACENCY ||
         source->incoming_kind ==
         AGE_VLE_STREAM_DIRECTED_SOURCE_AGE_ADJACENCY))
    {
        return true;
    }

    if (feedback->source_class_id ==
        VLE_SOURCE_POLICY_CLASS_ADJACENCY_COMPOSITE_VALUE_POSTING &&
        source->policy_class_id ==
        VLE_SOURCE_POLICY_CLASS_ADJACENCY_COMPOSITE_PREFILTER)
    {
        return true;
    }

    return false;
}

static double calculate_vle_source_scan_density(int64 candidates, int64 scans)
{
    if (candidates <= 0 || scans <= 0)
        return 0.0;

    return (double)candidates / (double)scans;
}

static const char *age_vle_stream_source_kind_name(
    AgeVLEStreamEdgeSourceKind kind)
{
    switch (kind)
    {
        case AGE_VLE_STREAM_EDGE_SOURCE_GLOBAL_METADATA:
            return "global-metadata";
        case AGE_VLE_STREAM_EDGE_SOURCE_LOCAL_INDEX_CANDIDATE:
            return "local-index-candidate";
        case AGE_VLE_STREAM_EDGE_SOURCE_DYNAMIC:
            return "dynamic";
    }

    return "unknown";
}

static const char *age_vle_directed_source_kind_name(
    AgeVLEStreamDirectedSourceKind kind)
{
    switch (kind)
    {
        case AGE_VLE_STREAM_DIRECTED_SOURCE_NONE:
            return "none";
        case AGE_VLE_STREAM_DIRECTED_SOURCE_AGE_ADJACENCY:
            return "age-adjacency";
        case AGE_VLE_STREAM_DIRECTED_SOURCE_ENDPOINT_BTREE:
            return "endpoint-btree";
    }

    return "unknown";
}

static void choose_vle_source_policy_decision(
    VLESourcePolicyDecision *decision,
    AgeVLEStreamDirectedSourceKind current_kind, double fanout,
    bool fanout_known, bool endpoint_available, bool age_adjacency_available,
    bool direction_active, bool composite_prefilter_active,
    double endpoint_headroom,
    const VLESourcePolicyProfile *profile)
{
    Assert(decision != NULL);
    Assert(profile != NULL);

    decision->kind = current_kind;
    decision->endpoint_work = estimate_vle_source_policy_work(
        fanout, profile->depth);
    decision->limit_work = estimate_vle_source_policy_work(
        profile->fanout_budget, profile->depth);
    decision->combined_endpoint_work = 0.0;
    decision->combined_limit_work = 0.0;
    decision->composite_work = 0.0;
    decision->composite_prefilter_active = false;
    set_vle_source_policy_reason(decision, VLE_SOURCE_POLICY_REASON_LAYOUT);

    if (!direction_active)
    {
        decision->kind = AGE_VLE_STREAM_DIRECTED_SOURCE_NONE;
        set_vle_source_policy_reason(
            decision, VLE_SOURCE_POLICY_REASON_INACTIVE_DIRECTION);
        return;
    }

    if (!endpoint_available && !age_adjacency_available)
    {
        decision->kind = AGE_VLE_STREAM_DIRECTED_SOURCE_NONE;
        set_vle_source_policy_reason(decision,
                                     VLE_SOURCE_POLICY_REASON_NO_SOURCE);
        return;
    }
    if (!profile->cost_eligible)
        return;
    if (age_adjacency_available && composite_prefilter_active)
    {
        decision->kind = AGE_VLE_STREAM_DIRECTED_SOURCE_AGE_ADJACENCY;
        decision->composite_work = estimate_vle_source_policy_work(
            (double)profile->composite_fanout, profile->depth);
        decision->composite_prefilter_active = true;
        set_vle_source_policy_reason(
            decision,
            vle_source_profile_has_value_posting_payload(profile) ?
            VLE_SOURCE_POLICY_REASON_COMPOSITE_VALUE_POSTING :
            VLE_SOURCE_POLICY_REASON_COMPOSITE_PREFILTER);
        return;
    }
    if (endpoint_available &&
        fanout_known &&
        vle_source_endpoint_work_within_policy(decision,
                                               age_adjacency_available,
                                               endpoint_headroom,
                                               profile))
    {
        decision->kind = AGE_VLE_STREAM_DIRECTED_SOURCE_ENDPOINT_BTREE;
        set_vle_source_policy_reason(
            decision,
            profile->cache_seed_eligible && age_adjacency_available ?
            VLE_SOURCE_POLICY_REASON_ENDPOINT_HEADROOM :
            VLE_SOURCE_POLICY_REASON_ENDPOINT_WORK);
        return;
    }
    if (age_adjacency_available)
    {
        decision->kind = AGE_VLE_STREAM_DIRECTED_SOURCE_AGE_ADJACENCY;
        if (!endpoint_available)
            set_vle_source_policy_reason(
                decision, VLE_SOURCE_POLICY_REASON_ADJACENCY_ONLY);
        else if (!fanout_known)
            set_vle_source_policy_reason(
                decision, VLE_SOURCE_POLICY_REASON_UNKNOWN_FANOUT);
        else if (decision->endpoint_work == decision->limit_work)
            set_vle_source_policy_reason(
                decision, VLE_SOURCE_POLICY_REASON_WORK_TIE);
        else if (profile->empty_lifecycle_eligible &&
                 decision->endpoint_work < decision->limit_work)
            set_vle_source_policy_reason(
                decision,
                VLE_SOURCE_POLICY_REASON_EMPTY_LIFECYCLE_HEADROOM);
        else if (profile->cache_seed_eligible &&
                 decision->endpoint_work < decision->limit_work)
            set_vle_source_policy_reason(
                decision, VLE_SOURCE_POLICY_REASON_CACHE_SEED_HEADROOM);
        else
            set_vle_source_policy_reason(
                decision, VLE_SOURCE_POLICY_REASON_WORK_EXCEEDS_LIMIT);
        return;
    }
    if (endpoint_available)
    {
        decision->kind = AGE_VLE_STREAM_DIRECTED_SOURCE_ENDPOINT_BTREE;
        set_vle_source_policy_reason(
            decision,
            fanout_known ? VLE_SOURCE_POLICY_REASON_ENDPOINT_ONLY :
                           VLE_SOURCE_POLICY_REASON_UNKNOWN_FANOUT);
        return;
    }

    decision->kind = AGE_VLE_STREAM_DIRECTED_SOURCE_NONE;
    set_vle_source_policy_reason(decision,
                                 VLE_SOURCE_POLICY_REASON_NO_SOURCE);
}

static void apply_vle_source_combined_policy(
    VLESourcePolicyDecision *out_policy,
    VLESourcePolicyDecision *in_policy,
    const VLEStreamSourceCostInput *input,
    const VLESourcePolicyProfile *profile)
{
    double combined_fanout;
    double combined_work;
    double combined_limit;
    VLESourcePolicyReason reason;

    Assert(out_policy != NULL);
    Assert(in_policy != NULL);
    Assert(input != NULL);
    Assert(input->evidence != NULL);
    Assert(profile != NULL);

    if (!profile->cost_eligible ||
        input->direction != CYPHER_REL_DIR_NONE ||
        !profile->outgoing_active ||
        !profile->incoming_active ||
        !input->start_fanout_known ||
        !input->end_fanout_known ||
        out_policy->kind != AGE_VLE_STREAM_DIRECTED_SOURCE_ENDPOINT_BTREE ||
        in_policy->kind != AGE_VLE_STREAM_DIRECTED_SOURCE_ENDPOINT_BTREE)
    {
        return;
    }

    combined_fanout = input->evidence->start_fanout +
        input->evidence->end_fanout;
    combined_work = estimate_vle_source_policy_work(combined_fanout,
                                                    profile->depth);
    combined_limit = estimate_vle_source_policy_work(profile->fanout_budget,
                                                     profile->depth);

    out_policy->combined_endpoint_work = combined_work;
    in_policy->combined_endpoint_work = combined_work;
    out_policy->combined_limit_work = combined_limit;
    in_policy->combined_limit_work = combined_limit;

    if (combined_work < combined_limit ||
        (profile->depth <= 1 && combined_work <= combined_limit))
    {
        return;
    }

    reason = combined_work == combined_limit ?
        VLE_SOURCE_POLICY_REASON_COMBINED_WORK_TIE :
        VLE_SOURCE_POLICY_REASON_COMBINED_WORK_EXCEEDS_LIMIT;

    if (input->age_adjacency_out)
    {
        out_policy->kind = AGE_VLE_STREAM_DIRECTED_SOURCE_AGE_ADJACENCY;
        set_vle_source_policy_reason(out_policy, reason);
    }
    if (input->age_adjacency_in)
    {
        in_policy->kind = AGE_VLE_STREAM_DIRECTED_SOURCE_AGE_ADJACENCY;
        set_vle_source_policy_reason(in_policy, reason);
    }
}

static void apply_vle_source_directional_family_policy(
    VLESourcePolicyDecision *out_policy,
    VLESourcePolicyDecision *in_policy,
    const VLEStreamSourceCostInput *input,
    const VLESourcePolicyProfile *profile)
{
    Assert(out_policy != NULL);
    Assert(in_policy != NULL);
    Assert(input != NULL);
    Assert(profile != NULL);

    if (!profile->threshold_directional_family ||
        input->direction != CYPHER_REL_DIR_NONE ||
        !profile->outgoing_active ||
        !profile->incoming_active)
    {
        return;
    }

    if (profile->threshold_split_out &&
        input->endpoint_end &&
        in_policy->kind == AGE_VLE_STREAM_DIRECTED_SOURCE_AGE_ADJACENCY)
    {
        in_policy->kind = AGE_VLE_STREAM_DIRECTED_SOURCE_ENDPOINT_BTREE;
        set_vle_source_policy_reason(
            in_policy,
            VLE_SOURCE_POLICY_REASON_DIRECTIONAL_FAMILY_PRODUCTIVE);
    }
    else if (profile->threshold_split_in &&
             input->endpoint_start &&
             out_policy->kind == AGE_VLE_STREAM_DIRECTED_SOURCE_AGE_ADJACENCY)
    {
        out_policy->kind = AGE_VLE_STREAM_DIRECTED_SOURCE_ENDPOINT_BTREE;
        set_vle_source_policy_reason(
            out_policy,
            VLE_SOURCE_POLICY_REASON_DIRECTIONAL_FAMILY_PRODUCTIVE);
    }
}

static double estimate_vle_source_policy_work(double fanout, int64 depth)
{
    double frontier;
    double work;
    int64 step;

    if (fanout <= 0 || depth <= 0)
        return 0.0;

    frontier = 1.0;
    work = 0.0;
    for (step = 0; step < depth; step++)
    {
        frontier *= fanout;
        if (frontier >= (double)PG_INT64_MAX - work)
            return (double)PG_INT64_MAX;
        work += frontier;
    }

    return work;
}

static int64 estimate_vle_empty_lifecycle_batch_size(
    const VLESourcePolicyProfile *profile)
{
    double expected_work;
    int64 batch_size;

    Assert(profile != NULL);

    if (!profile->empty_lifecycle_eligible)
        return 0;

    expected_work = estimate_vle_source_policy_work(
        profile->fanout_budget, profile->depth);
    expected_work *= Max(profile->materialization_weight,
                         (int64)VLE_MATERIALIZATION_WEIGHT_SCALAR);
    batch_size = round_vle_source_cost_evidence(expected_work);
    batch_size = Max(batch_size, (int64)VLE_EMPTY_LIFECYCLE_BATCH_MIN);
    batch_size = Min(batch_size, (int64)VLE_EMPTY_LIFECYCLE_BATCH_MAX);

    return batch_size;
}

static int64 select_vle_payload_replay_batch_size(
    const VLESourcePolicyProfile *profile, int64 replay_percent)
{
    int64 base_batch_size;
    int64 replay_extra;

    Assert(profile != NULL);

    if (!profile->empty_lifecycle_eligible ||
        replay_percent <= 0 ||
        profile->materialization_weight < VLE_MATERIALIZATION_WEIGHT_OBJECT)
    {
        return 0;
    }

    base_batch_size = estimate_vle_empty_lifecycle_batch_size(profile);
    replay_extra =
        (base_batch_size * profile->materialization_weight * replay_percent +
         99) / 100;

    return Min(base_batch_size + Max(replay_extra, (int64)1),
               (int64)VLE_EMPTY_LIFECYCLE_BATCH_MAX);
}

static int64 select_vle_payload_matrix_frontier_batch_size(
    const VLESourcePolicyProfile *profile,
    const VLESourceRuntimeThresholdFeedback *feedback)
{
    int64 base_batch_size;
    int64 filter_percent;
    int64 matrix_extra;

    Assert(profile != NULL);
    Assert(feedback != NULL);

    if (!profile->empty_lifecycle_eligible ||
        (feedback->payload_matrix_filter_percent <= 0 &&
         feedback->payload_matrix_prescan_percent <= 0 &&
         feedback->payload_matrix_replay_source_percent <= 0 &&
         feedback->payload_matrix_run_block_percent <= 0 &&
         feedback->payload_matrix_regroup_percent <= 0))
    {
        return 0;
    }

    base_batch_size = estimate_vle_empty_lifecycle_batch_size(profile);
    filter_percent = Max(feedback->payload_matrix_filter_percent,
                         feedback->payload_matrix_prescan_percent);
    filter_percent = Max(filter_percent,
                         feedback->payload_matrix_replay_source_percent);
    filter_percent = Max(filter_percent,
                         feedback->payload_matrix_run_block_percent);
    filter_percent = Max(filter_percent,
                         feedback->payload_matrix_regroup_percent);
    matrix_extra =
        (base_batch_size *
         Max(profile->materialization_weight,
             (int64)VLE_MATERIALIZATION_WEIGHT_SCALAR) *
         filter_percent + 99) / 100;

    return Min(base_batch_size + Max(matrix_extra, (int64)1),
               (int64)VLE_EMPTY_LIFECYCLE_BATCH_MAX);
}

static int64 select_vle_threshold_feedback_batch_size(
    const AgeVLESourceStats *stats)
{
    int64 batch_size;

    Assert(stats != NULL);

    if (stats->root_empty_batch_capacity <= 0)
        return 0;

    if (stats->root_empty_saturated_count > 0)
    {
        batch_size = Max(stats->root_empty_batch_capacity * 2,
                         stats->root_empty_completion_count);
    }
    else if (stats->root_empty_completion_count > 0)
    {
        batch_size = Max(
            stats->root_empty_completion_count *
                VLE_EMPTY_LIFECYCLE_BATCH_RELAX_MULTIPLIER,
            (int64)VLE_EMPTY_LIFECYCLE_BATCH_MIN);
        batch_size = Min(batch_size, stats->root_empty_batch_capacity);
    }
    else
    {
        batch_size = stats->root_empty_batch_capacity;
    }

    batch_size = Max(batch_size, (int64)VLE_EMPTY_LIFECYCLE_BATCH_MIN);
    batch_size = Min(batch_size, (int64)VLE_EMPTY_LIFECYCLE_BATCH_MAX);

    return batch_size;
}

static int64 select_vle_threshold_feedback_headroom_percent(
    const VLESourcePolicyProfile *profile,
    const VLESourceRuntimeThresholdFeedback *feedback)
{
    int64 headroom_percent;

    Assert(profile != NULL);
    Assert(feedback != NULL);

    headroom_percent = feedback->endpoint_headroom_percent;
    if (vle_threshold_feedback_is_directional_family(profile, feedback))
    {
        headroom_percent = Max(headroom_percent,
                               (int64)(VLE_EMPTY_LIFECYCLE_DIRECTIONAL_FAMILY_ENDPOINT_HEADROOM *
                                       100.0 + 0.5));
    }

    return headroom_percent;
}

static bool vle_threshold_feedback_is_directional_family(
    const VLESourcePolicyProfile *profile,
    const VLESourceRuntimeThresholdFeedback *feedback)
{
    Assert(profile != NULL);
    Assert(feedback != NULL);

    if (profile->direction_class_id != VLE_SOURCE_DIRECTION_BOTH)
    {
        return false;
    }

    return vle_source_direction_is_directional_family(
        feedback->source_direction_id);
}

static void set_vle_threshold_feedback_reason(
    VLESourceRuntimeThresholdFeedback *feedback,
    VLESourceThresholdFeedbackReason reason)
{
    Assert(feedback != NULL);

    feedback->reason_id = reason;
    feedback->feedback_class_id = classify_vle_threshold_feedback_reason_kind(
        reason);
}

const char *age_vle_threshold_feedback_reason_name(
    VLESourceThresholdFeedbackReason reason)
{
    switch (reason)
    {
        case VLE_SOURCE_THRESHOLD_REASON_PLANNED_EMPTY_LIFECYCLE:
            return "planned-empty-lifecycle";
        case VLE_SOURCE_THRESHOLD_REASON_ROOT_EMPTY_OBSERVED:
            return "root-empty-observed";
        case VLE_SOURCE_THRESHOLD_REASON_ROOT_EMPTY_SATURATED:
            return "root-empty-saturated";
        case VLE_SOURCE_THRESHOLD_REASON_ROOT_EMPTY_REPEAT_OBSERVED:
            return "root-empty-repeat-observed";
        case VLE_SOURCE_THRESHOLD_REASON_ROOT_EMPTY_REPEAT_SATURATED:
            return "root-empty-repeat-saturated";
        case VLE_SOURCE_THRESHOLD_REASON_NONE:
            return "none";
    }

    return "none";
}

static VLESourcePolicyClass classify_vle_threshold_feedback_reason_kind(
    VLESourceThresholdFeedbackReason reason)
{
    switch (reason)
    {
        case VLE_SOURCE_THRESHOLD_REASON_ROOT_EMPTY_SATURATED:
        case VLE_SOURCE_THRESHOLD_REASON_ROOT_EMPTY_REPEAT_SATURATED:
            return VLE_SOURCE_POLICY_CLASS_ADJACENCY_EMPTY_BATCH;
        case VLE_SOURCE_THRESHOLD_REASON_ROOT_EMPTY_REPEAT_OBSERVED:
            return VLE_SOURCE_POLICY_CLASS_ADJACENCY_EMPTY_LIFECYCLE;
        case VLE_SOURCE_THRESHOLD_REASON_PLANNED_EMPTY_LIFECYCLE:
        case VLE_SOURCE_THRESHOLD_REASON_ROOT_EMPTY_OBSERVED:
            return VLE_SOURCE_POLICY_CLASS_ADJACENCY_CACHE_SEEDED;
        case VLE_SOURCE_THRESHOLD_REASON_NONE:
            return VLE_SOURCE_POLICY_CLASS_NONE;
    }

    return VLE_SOURCE_POLICY_CLASS_ADJACENCY_FEEDBACK;
}

static VLESourcePolicyClass classify_vle_payload_feedback_reason_kind(
    VLESourcePayloadFeedbackReason reason)
{
    switch (reason)
    {
        case VLE_SOURCE_PAYLOAD_REASON_REPLAY_RATIO_OBSERVED:
            return VLE_SOURCE_POLICY_CLASS_ADJACENCY_REPLAY;
        case VLE_SOURCE_PAYLOAD_REASON_REPLAY_OBSERVED:
            return VLE_SOURCE_POLICY_CLASS_ADJACENCY_REPLAY_OBSERVED;
        case VLE_SOURCE_PAYLOAD_REASON_CACHE_SEEDED:
            return VLE_SOURCE_POLICY_CLASS_ADJACENCY_CACHE_SEEDED;
        case VLE_SOURCE_PAYLOAD_REASON_VALUE_POSTING_OBSERVED:
            return VLE_SOURCE_POLICY_CLASS_ADJACENCY_COMPOSITE_VALUE_POSTING;
        case VLE_SOURCE_PAYLOAD_REASON_DIRECTORY_FILTER_OBSERVED:
        case VLE_SOURCE_PAYLOAD_REASON_COMPOSITE_PREFILTER_OBSERVED:
            return VLE_SOURCE_POLICY_CLASS_ADJACENCY_COMPOSITE_PREFILTER;
        case VLE_SOURCE_PAYLOAD_REASON_MATRIX_PRESCAN_COMPACT:
        case VLE_SOURCE_PAYLOAD_REASON_MATRIX_FULL_GROUP_DRAIN:
        case VLE_SOURCE_PAYLOAD_REASON_MATRIX_REPLAY_SOURCE_BATCH:
        case VLE_SOURCE_PAYLOAD_REASON_MATRIX_BLOCK_TAG_BATCH:
        case VLE_SOURCE_PAYLOAD_REASON_MATRIX_RUN_BLOCK_STREAM:
        case VLE_SOURCE_PAYLOAD_REASON_MATRIX_RAW_BLOCK_BATCH:
        case VLE_SOURCE_PAYLOAD_REASON_MATRIX_RUN_BLOCK_GROUP:
        case VLE_SOURCE_PAYLOAD_REASON_MATRIX_PAGE_GROUP:
        case VLE_SOURCE_PAYLOAD_REASON_MATRIX_FALLBACK_REGROUP:
            return VLE_SOURCE_POLICY_CLASS_MATRIX_FRONTIER_PRE_SCAN;
        case VLE_SOURCE_PAYLOAD_REASON_SCAN_OBSERVED:
            return VLE_SOURCE_POLICY_CLASS_ADJACENCY_PAYLOAD_SCAN;
        case VLE_SOURCE_PAYLOAD_REASON_NONE:
            return VLE_SOURCE_POLICY_CLASS_NONE;
    }

    return VLE_SOURCE_POLICY_CLASS_ADJACENCY_PAYLOAD;
}

static void set_vle_payload_feedback_reason(
    VLESourceRuntimePayloadFeedback *feedback,
    VLESourcePayloadFeedbackReason reason)
{
    Assert(feedback != NULL);

    feedback->reason_id = reason;
    feedback->feedback_class_id = classify_vle_payload_feedback_reason_kind(
        reason);
}

const char *age_vle_payload_feedback_reason_name(
    VLESourcePayloadFeedbackReason reason)
{
    switch (reason)
    {
        case VLE_SOURCE_PAYLOAD_REASON_SCAN_OBSERVED:
            return "payload-scan-observed";
        case VLE_SOURCE_PAYLOAD_REASON_VALUE_POSTING_OBSERVED:
            return "payload-value-posting-observed";
        case VLE_SOURCE_PAYLOAD_REASON_DIRECTORY_FILTER_OBSERVED:
            return "payload-directory-filter-observed";
        case VLE_SOURCE_PAYLOAD_REASON_COMPOSITE_PREFILTER_OBSERVED:
            return "payload-composite-prefilter-observed";
        case VLE_SOURCE_PAYLOAD_REASON_MATRIX_PRESCAN_COMPACT:
            return "payload-matrix-prescan-compact";
        case VLE_SOURCE_PAYLOAD_REASON_MATRIX_FULL_GROUP_DRAIN:
            return "payload-matrix-full-group-drain";
        case VLE_SOURCE_PAYLOAD_REASON_MATRIX_REPLAY_SOURCE_BATCH:
            return "payload-matrix-replay-source-batch";
        case VLE_SOURCE_PAYLOAD_REASON_MATRIX_BLOCK_TAG_BATCH:
            return "payload-matrix-block-tag-batch";
        case VLE_SOURCE_PAYLOAD_REASON_MATRIX_RUN_BLOCK_STREAM:
            return "payload-matrix-run-block-stream";
        case VLE_SOURCE_PAYLOAD_REASON_MATRIX_RAW_BLOCK_BATCH:
            return "payload-matrix-raw-block-batch";
        case VLE_SOURCE_PAYLOAD_REASON_MATRIX_RUN_BLOCK_GROUP:
            return "payload-matrix-run-block-group";
        case VLE_SOURCE_PAYLOAD_REASON_MATRIX_PAGE_GROUP:
            return "payload-matrix-page-group";
        case VLE_SOURCE_PAYLOAD_REASON_MATRIX_FALLBACK_REGROUP:
            return "payload-matrix-fallback-regroup";
        case VLE_SOURCE_PAYLOAD_REASON_REPLAY_RATIO_OBSERVED:
            return "payload-replay-ratio-observed";
        case VLE_SOURCE_PAYLOAD_REASON_REPLAY_OBSERVED:
            return "payload-replay-observed";
        case VLE_SOURCE_PAYLOAD_REASON_CACHE_SEEDED:
            return "payload-cache-seeded";
        case VLE_SOURCE_PAYLOAD_REASON_NONE:
            return "none";
    }

    return "none";
}

const char *age_vle_source_policy_recommendation_name(
    VLESourcePolicyRecommendation recommendation)
{
    switch (recommendation)
    {
        case VLE_SOURCE_RECOMMENDATION_KEEP_LAYOUT:
            return "keep-layout";
        case VLE_SOURCE_RECOMMENDATION_COLLECT_ENDPOINT_STATS:
            return "collect-endpoint-stats";
        case VLE_SOURCE_RECOMMENDATION_KEEP_VALUE_POSTING:
            return "keep-value-posting";
        case VLE_SOURCE_RECOMMENDATION_KEEP_PROPERTY_PREFILTER:
            return "keep-property-prefilter";
        case VLE_SOURCE_RECOMMENDATION_KEEP_AGE_ADJACENCY:
            return "keep-age-adjacency";
        case VLE_SOURCE_RECOMMENDATION_KEEP_MATRIX_FRONTIER_BATCH:
            return "keep-matrix-frontier-batch";
        case VLE_SOURCE_RECOMMENDATION_KEEP_EMPTY_BATCH:
            return "keep-empty-batch";
        case VLE_SOURCE_RECOMMENDATION_KEEP_EMPTY_LIFECYCLE:
            return "keep-empty-lifecycle";
        case VLE_SOURCE_RECOMMENDATION_PREFER_AGE_ADJACENCY_DEPTH:
            return "prefer-age-adjacency-depth";
        case VLE_SOURCE_RECOMMENDATION_PREFER_AGE_ADJACENCY_MATERIALIZATION:
            return "prefer-age-adjacency-materialization";
        case VLE_SOURCE_RECOMMENDATION_PREFER_AGE_ADJACENCY_UNDIRECTED:
            return "prefer-age-adjacency-undirected";
        case VLE_SOURCE_RECOMMENDATION_KEEP_AGE_ADJACENCY_UNDIRECTED:
            return "keep-age-adjacency-undirected";
        case VLE_SOURCE_RECOMMENDATION_KEEP_DIRECTIONAL_SPLIT:
            return "keep-directional-split";
        case VLE_SOURCE_RECOMMENDATION_KEEP_ENDPOINT_BTREE:
            return "keep-endpoint-btree";
        case VLE_SOURCE_RECOMMENDATION_KEEP_GLOBAL_METADATA:
            return "keep-global-metadata";
        case VLE_SOURCE_RECOMMENDATION_NO_CANDIDATES:
            return "no-candidates";
        case VLE_SOURCE_RECOMMENDATION_KEEP_LOCAL_SOURCE:
            return "keep-local-source";
        case VLE_SOURCE_RECOMMENDATION_CHECK_AGE_ADJACENCY_DENSITY:
            return "check-age-adjacency-density";
        case VLE_SOURCE_RECOMMENDATION_KEEP_PACKED_SUPPRESSED:
            return "keep-packed-suppressed";
        case VLE_SOURCE_RECOMMENDATION_CHECK_FIXED_SOURCE_COVERAGE:
            return "check-fixed-source-coverage";
        case VLE_SOURCE_RECOMMENDATION_OBSERVE:
            return "observe";
        case VLE_SOURCE_RECOMMENDATION_NONE:
            break;
    }

    return "unknown";
}

const char *age_vle_stream_composite_source_reason_name(
    AgeVLEStreamCompositeSourceReason reason)
{
    switch (reason)
    {
        case AGE_VLE_STREAM_COMPOSITE_REASON_MISSING_TERMINAL_LABEL:
            return "missing-terminal-label";
        case AGE_VLE_STREAM_COMPOSITE_REASON_PROPERTY_LABEL_UNKNOWN:
            return "property-label-unknown";
        case AGE_VLE_STREAM_COMPOSITE_REASON_LABEL_MISMATCH:
            return "label-mismatch";
        case AGE_VLE_STREAM_COMPOSITE_REASON_TERMINAL_LABEL_PROPERTY:
            return "terminal-label-property";
        case AGE_VLE_STREAM_COMPOSITE_REASON_ENDPOINT_LABEL_ACCEPTANCE:
            return "endpoint-label-acceptance";
        case AGE_VLE_STREAM_COMPOSITE_REASON_NONE:
            break;
    }

    return "unknown";
}

bool age_vle_stream_composite_source_reason_is_eligible(
    AgeVLEStreamCompositeSourceReason reason)
{
    return reason ==
           AGE_VLE_STREAM_COMPOSITE_REASON_TERMINAL_LABEL_PROPERTY ||
           reason ==
           AGE_VLE_STREAM_COMPOSITE_REASON_ENDPOINT_LABEL_ACCEPTANCE;
}

const char *age_graph_property_selectivity_source_name(
    AgeGraphPropertySelectivitySource source)
{
    switch (source)
    {
        case AGE_GRAPH_PROPERTY_SELECTIVITY_FALLBACK:
            return "fallback";
        case AGE_GRAPH_PROPERTY_SELECTIVITY_TYPED_MCV:
            return "typed-mcv";
        case AGE_GRAPH_PROPERTY_SELECTIVITY_FALLBACK_MCV_CEILING:
            return "fallback-mcv-ceiling";
        case AGE_GRAPH_PROPERTY_SELECTIVITY_TYPED_DISTINCT:
            return "typed-distinct";
        case AGE_GRAPH_PROPERTY_SELECTIVITY_FALLBACK_CEILING:
            return "fallback-ceiling";
        case AGE_GRAPH_PROPERTY_SELECTIVITY_NONE:
            break;
    }

    return "none";
}

static int64 calculate_vle_source_ratio_percent(int64 numerator,
                                                int64 denominator)
{
    if (numerator <= 0 || denominator <= 0)
        return 0;

    return Min((int64)100, (numerator * 100 + denominator / 2) /
               denominator);
}

static double select_vle_empty_lifecycle_endpoint_headroom(
    const VLESourcePolicyProfile *profile)
{
    Assert(profile != NULL);

    if (!profile->empty_lifecycle_eligible)
        return VLE_CACHE_SEED_ENDPOINT_HEADROOM;

    if (profile->empty_lifecycle_batch_size >=
        VLE_EMPTY_LIFECYCLE_BATCH_STRONG)
    {
        return VLE_EMPTY_LIFECYCLE_BATCH_ENDPOINT_HEADROOM;
    }

    return VLE_EMPTY_LIFECYCLE_ENDPOINT_HEADROOM;
}

static void build_vle_source_policy_profile(
    VLESourcePolicyProfile *profile, const VLEStreamSourceCostInput *input)
{
    VLESourceRuntimeThresholdFeedback threshold_feedback;

    Assert(profile != NULL);
    Assert(input != NULL);

    profile->output_requirement = input->output_requirement;
    profile->consumer_class_id = age_vle_source_policy_consumer_class_id(
        input->output_requirement);
    profile->materialization_weight = age_vle_output_materialization_weight(
        input->output_requirement);
    switch (input->direction)
    {
        case CYPHER_REL_DIR_RIGHT:
            profile->direction_class_id = VLE_SOURCE_DIRECTION_OUT;
            profile->outgoing_active = true;
            profile->incoming_active = false;
            break;
        case CYPHER_REL_DIR_LEFT:
            profile->direction_class_id = VLE_SOURCE_DIRECTION_IN;
            profile->outgoing_active = false;
            profile->incoming_active = true;
            break;
        case CYPHER_REL_DIR_NONE:
            profile->direction_class_id = VLE_SOURCE_DIRECTION_BOTH;
            profile->outgoing_active = true;
            profile->incoming_active = true;
            break;
        default:
            profile->direction_class_id = VLE_SOURCE_DIRECTION_NONE;
            profile->outgoing_active = true;
            profile->incoming_active = true;
            break;
    }
    profile->cost_eligible = !input->upper_infinite &&
        !input->has_property_constraints;
    profile->depth = input->upper_infinite ? 0 : Max(input->upper, (int64)1);
    profile->cache_seed_eligible = profile->cost_eligible &&
        profile->depth > 1 &&
        ((profile->outgoing_active && input->age_adjacency_out) ||
         (profile->incoming_active && input->age_adjacency_in));
    profile->empty_lifecycle_eligible = profile->cache_seed_eligible;
    profile->composite_prefilter_planned =
        input->composite_prefilter_planned &&
        input->composite_candidate_fanout > 0 &&
        input->composite_fanout > 0 &&
        input->composite_fanout < input->composite_candidate_fanout;
    profile->composite_candidate_fanout =
        profile->composite_prefilter_planned ?
        input->composite_candidate_fanout : 0;
    profile->composite_fanout =
        profile->composite_prefilter_planned ?
        input->composite_fanout : 0;
    profile->out_value_posting_source_kind =
        input->evidence->start_value_posting_source_kind;
    profile->in_value_posting_source_kind =
        input->evidence->end_value_posting_source_kind;

    switch (input->output_requirement)
    {
        case AGE_VLE_OUTPUT_REQUIREMENT_PATH:
            profile->fanout_budget = VLE_ENDPOINT_BTREE_PATH_FANOUT;
            break;
        case AGE_VLE_OUTPUT_REQUIREMENT_TERMINAL_VERTEX:
        case AGE_VLE_OUTPUT_REQUIREMENT_TERMINAL_PROPERTIES:
            profile->fanout_budget =
                VLE_ENDPOINT_BTREE_TERMINAL_OBJECT_FANOUT;
            break;
        case AGE_VLE_OUTPUT_REQUIREMENT_TERMINAL_PROPERTY:
        case AGE_VLE_OUTPUT_REQUIREMENT_UNKNOWN:
            profile->fanout_budget = VLE_ENDPOINT_BTREE_TERMINAL_FANOUT;
            break;
        default:
            profile->fanout_budget = VLE_ENDPOINT_BTREE_TERMINAL_FANOUT;
            break;
    }

    profile->empty_lifecycle_batch_size =
        estimate_vle_empty_lifecycle_batch_size(profile);
    profile->cache_seed_endpoint_headroom =
        select_vle_empty_lifecycle_endpoint_headroom(profile);
    profile->outgoing_endpoint_headroom =
        profile->cache_seed_endpoint_headroom;
    profile->incoming_endpoint_headroom =
        profile->cache_seed_endpoint_headroom;
    profile->threshold_input_known = false;
    profile->threshold_directional_family = false;
    profile->threshold_split_out = false;
    profile->threshold_split_in = false;
    profile->threshold_input_headroom_percent = 0;
    profile->threshold_input_batch_size = 0;
    profile->threshold_input_observed_count = 0;
    profile->threshold_input_saturated_count = 0;
    profile->threshold_input_relaxed_count = 0;
    profile->threshold_input_out_observed_count = 0;
    profile->threshold_input_in_observed_count = 0;
    profile->threshold_input_out_saturated_count = 0;
    profile->threshold_input_in_saturated_count = 0;
    profile->threshold_input_direction_id = VLE_SOURCE_DIRECTION_NONE;
    profile->threshold_input_reason_id = VLE_SOURCE_THRESHOLD_REASON_NONE;
    profile->threshold_input_class_id = VLE_SOURCE_POLICY_CLASS_NONE;
    profile->payload_input_known = false;
    profile->payload_input_headroom_percent = 0;
    profile->payload_input_scan_runs = 0;
    profile->payload_input_replay_runs = 0;
    profile->payload_input_seed_runs = 0;
    profile->payload_input_replay_percent = 0;
    profile->payload_input_seed_percent = 0;
    profile->payload_input_observed_count = 0;
    profile->payload_input_value_posting_observed_count = 0;
    profile->payload_input_matrix_active_percent = 0;
    profile->payload_input_matrix_filter_percent = 0;
    profile->payload_input_matrix_prescan_percent = 0;
    profile->payload_input_matrix_replay_source_percent = 0;
    profile->payload_input_matrix_run_block_percent = 0;
    profile->payload_input_matrix_regroup_percent = 0;
    profile->payload_input_reason_id = VLE_SOURCE_PAYLOAD_REASON_NONE;
    profile->payload_input_class_id = VLE_SOURCE_POLICY_CLASS_NONE;
    profile->payload_input_value_posting_source_kind =
        VLE_SOURCE_VALUE_POSTING_NONE;

    if (lookup_vle_source_threshold_feedback(input, profile,
                                             &threshold_feedback))
    {
        if (threshold_feedback.observed_count > 0)
        {
            int64 threshold_headroom_percent =
                select_vle_threshold_feedback_headroom_percent(
                    profile, &threshold_feedback);
            double threshold_headroom =
                (double)threshold_headroom_percent / 100.0;

            if (threshold_headroom > 0.0 &&
                threshold_headroom < profile->cache_seed_endpoint_headroom)
            {
                profile->cache_seed_endpoint_headroom = threshold_headroom;
            }
            if (vle_threshold_feedback_is_directional_family(
                    profile, &threshold_feedback))
            {
                bool split_out = threshold_feedback.out_observed_count > 0 &&
                    threshold_feedback.in_observed_count == 0;
                bool split_in = threshold_feedback.in_observed_count > 0 &&
                    threshold_feedback.out_observed_count == 0;

                profile->threshold_directional_family = true;
                if (!split_out && !split_in &&
                    threshold_feedback.out_observed_count > 0 &&
                    threshold_feedback.in_observed_count > 0)
                {
                    split_out =
                        threshold_feedback.out_observed_count >=
                        threshold_feedback.in_observed_count * 2;
                    split_in =
                        threshold_feedback.in_observed_count >=
                        threshold_feedback.out_observed_count * 2;
                }
                if (!split_out && !split_in &&
                    threshold_feedback.source_direction_id ==
                    VLE_SOURCE_DIRECTION_MIXED)
                {
                    split_out = true;
                }
                if (split_out)
                {
                    profile->outgoing_endpoint_headroom = threshold_headroom;
                    profile->threshold_split_out = true;
                }
                else if (split_in)
                {
                    profile->incoming_endpoint_headroom = threshold_headroom;
                    profile->threshold_split_in = true;
                }
                else
                {
                    profile->outgoing_endpoint_headroom =
                        threshold_headroom;
                    profile->incoming_endpoint_headroom =
                        threshold_headroom;
                }
            }
            else
            {
                profile->outgoing_endpoint_headroom =
                    profile->cache_seed_endpoint_headroom;
                profile->incoming_endpoint_headroom =
                    profile->cache_seed_endpoint_headroom;
            }
            if (threshold_feedback.empty_lifecycle_batch_size >
                profile->empty_lifecycle_batch_size)
            {
                profile->empty_lifecycle_batch_size =
                    threshold_feedback.empty_lifecycle_batch_size;
            }
        }
        if (threshold_feedback.payload_endpoint_headroom_percent > 0)
        {
            int64 payload_headroom_percent =
                select_vle_payload_feedback_headroom_percent_for_profile(
                    profile, &threshold_feedback);
            double payload_headroom;
            int64 payload_batch_size = select_vle_payload_replay_batch_size(
                profile, threshold_feedback.replay_percent);
            int64 matrix_batch_size =
                select_vle_payload_matrix_frontier_batch_size(
                    profile, &threshold_feedback);

            payload_headroom = (double)payload_headroom_percent / 100.0;
            if (payload_headroom > 0.0 &&
                payload_headroom < profile->cache_seed_endpoint_headroom)
            {
                profile->cache_seed_endpoint_headroom = payload_headroom;
                profile->outgoing_endpoint_headroom =
                    Min(profile->outgoing_endpoint_headroom,
                        payload_headroom);
                profile->incoming_endpoint_headroom =
                    Min(profile->incoming_endpoint_headroom,
                        payload_headroom);
            }
            if (payload_batch_size > profile->empty_lifecycle_batch_size)
            {
                profile->empty_lifecycle_batch_size = payload_batch_size;
            }
            if (matrix_batch_size > profile->empty_lifecycle_batch_size)
            {
                profile->empty_lifecycle_batch_size = matrix_batch_size;
            }
        }

        profile->threshold_input_known = threshold_feedback.observed_count > 0;
        profile->threshold_input_headroom_percent =
            select_vle_threshold_feedback_headroom_percent(
                profile, &threshold_feedback);
        profile->threshold_input_batch_size =
            threshold_feedback.empty_lifecycle_batch_size;
        profile->threshold_input_observed_count =
            threshold_feedback.observed_count;
        profile->threshold_input_saturated_count =
            threshold_feedback.saturated_count;
        profile->threshold_input_relaxed_count =
            threshold_feedback.relaxed_count;
        profile->threshold_input_out_observed_count =
            threshold_feedback.out_observed_count;
        profile->threshold_input_in_observed_count =
            threshold_feedback.in_observed_count;
        profile->threshold_input_out_saturated_count =
            threshold_feedback.out_saturated_count;
        profile->threshold_input_in_saturated_count =
            threshold_feedback.in_saturated_count;
        profile->threshold_input_direction_id =
            threshold_feedback.source_direction_id;
        profile->threshold_input_reason_id = threshold_feedback.reason_id;
        profile->threshold_input_class_id =
            threshold_feedback.feedback_class_id;
        profile->payload_input_known =
            threshold_feedback.payload_observed_count > 0;
        profile->payload_input_headroom_percent =
            profile->payload_input_known ?
            select_vle_payload_feedback_headroom_percent_for_profile(
                profile, &threshold_feedback) :
            threshold_feedback.payload_endpoint_headroom_percent;
        profile->payload_input_scan_runs = threshold_feedback.scan_runs;
        profile->payload_input_replay_runs = threshold_feedback.replay_runs;
        profile->payload_input_seed_runs = threshold_feedback.seed_runs;
        profile->payload_input_replay_percent =
            threshold_feedback.replay_percent;
        profile->payload_input_seed_percent =
            threshold_feedback.seed_percent;
        profile->payload_input_observed_count =
            threshold_feedback.payload_observed_count;
        profile->payload_input_reason_id = threshold_feedback.payload_reason_id;
        profile->payload_input_class_id = threshold_feedback.payload_class_id;
        profile->payload_input_value_posting_observed_count =
            threshold_feedback.payload_value_posting_observed_count;
        profile->payload_input_matrix_active_percent =
            threshold_feedback.payload_matrix_active_percent;
        profile->payload_input_matrix_filter_percent =
            threshold_feedback.payload_matrix_filter_percent;
        profile->payload_input_matrix_prescan_percent =
            threshold_feedback.payload_matrix_prescan_percent;
        profile->payload_input_matrix_replay_source_percent =
            threshold_feedback.payload_matrix_replay_source_percent;
        profile->payload_input_matrix_run_block_percent =
            threshold_feedback.payload_matrix_run_block_percent;
        profile->payload_input_matrix_regroup_percent =
            threshold_feedback.payload_matrix_regroup_percent;
        profile->payload_input_value_posting_source_kind =
            threshold_feedback.payload_value_posting_source_kind;
    }
}

static bool vle_source_endpoint_work_within_policy(
    const VLESourcePolicyDecision *decision,
    bool age_adjacency_available,
    double endpoint_headroom,
    const VLESourcePolicyProfile *profile)
{
    double headroom_limit;

    Assert(decision != NULL);
    Assert(profile != NULL);

    if (profile->cache_seed_eligible && age_adjacency_available)
    {
        headroom_limit = decision->limit_work * endpoint_headroom;

        return decision->endpoint_work <= headroom_limit;
    }

    if (decision->endpoint_work < decision->limit_work)
        return true;

    if (profile->depth <= 1 &&
        decision->endpoint_work <= decision->limit_work)
        return vle_source_policy_prefers_endpoint_tie(age_adjacency_available,
                                                      profile);

    return false;
}

static bool vle_source_policy_prefers_endpoint_tie(
    bool age_adjacency_available, const VLESourcePolicyProfile *profile)
{
    Assert(profile != NULL);

    if (!age_adjacency_available)
        return true;

    if (vle_source_consumer_is_terminal_scalar(profile->consumer_class_id))
        return true;

    return false;
}

static bool vle_source_profile_has_value_posting_payload(
    const VLESourcePolicyProfile *profile)
{
    Assert(profile != NULL);

    return profile->payload_input_known &&
        profile->payload_input_class_id ==
        VLE_SOURCE_POLICY_CLASS_ADJACENCY_COMPOSITE_VALUE_POSTING &&
        profile->payload_input_value_posting_observed_count > 0;
}

static void set_vle_source_policy_reason(
    VLESourcePolicyDecision *decision, VLESourcePolicyReason reason)
{
    Assert(decision != NULL);

    decision->reason = reason;
}

static const char *vle_source_policy_reason_text(
    VLESourcePolicyReason reason)
{
    switch (reason)
    {
        case VLE_SOURCE_POLICY_REASON_LAYOUT:
            return "layout";
        case VLE_SOURCE_POLICY_REASON_INACTIVE_DIRECTION:
            return "inactive-direction";
        case VLE_SOURCE_POLICY_REASON_NO_SOURCE:
            return "no-source";
        case VLE_SOURCE_POLICY_REASON_COMPOSITE_PREFILTER:
            return "composite-prefilter";
        case VLE_SOURCE_POLICY_REASON_COMPOSITE_VALUE_POSTING:
            return "composite-value-posting";
        case VLE_SOURCE_POLICY_REASON_ENDPOINT_HEADROOM:
            return "endpoint-headroom";
        case VLE_SOURCE_POLICY_REASON_ENDPOINT_WORK:
            return "endpoint-work";
        case VLE_SOURCE_POLICY_REASON_ENDPOINT_ONLY:
            return "endpoint-only";
        case VLE_SOURCE_POLICY_REASON_ADJACENCY_ONLY:
            return "adjacency-only";
        case VLE_SOURCE_POLICY_REASON_UNKNOWN_FANOUT:
            return "unknown-fanout";
        case VLE_SOURCE_POLICY_REASON_WORK_TIE:
            return "work-tie";
        case VLE_SOURCE_POLICY_REASON_EMPTY_LIFECYCLE_HEADROOM:
            return "empty-lifecycle-headroom";
        case VLE_SOURCE_POLICY_REASON_CACHE_SEED_HEADROOM:
            return "cache-seed-headroom";
        case VLE_SOURCE_POLICY_REASON_WORK_EXCEEDS_LIMIT:
            return "work-exceeds-limit";
        case VLE_SOURCE_POLICY_REASON_COMBINED_WORK_TIE:
            return "combined-work-tie";
        case VLE_SOURCE_POLICY_REASON_COMBINED_WORK_EXCEEDS_LIMIT:
            return "combined-work-exceeds-limit";
        case VLE_SOURCE_POLICY_REASON_DIRECTIONAL_FAMILY_PRODUCTIVE:
            return "directional-family-productive";
    }

    return "layout";
}

static bool vle_source_policy_reason_is_composite(
    VLESourcePolicyReason reason)
{
    return reason == VLE_SOURCE_POLICY_REASON_COMPOSITE_PREFILTER ||
           reason == VLE_SOURCE_POLICY_REASON_COMPOSITE_VALUE_POSTING;
}

static bool vle_source_policy_has_reason(
    const VLESourcePolicyDecision *out_policy,
    const VLESourcePolicyDecision *in_policy,
    VLESourcePolicyReason reason)
{
    Assert(out_policy != NULL);
    Assert(in_policy != NULL);

    return out_policy->reason == reason || in_policy->reason == reason;
}

static bool vle_source_policy_uses_age_adjacency(
    const VLESourcePolicyDecision *out_policy,
    const VLESourcePolicyDecision *in_policy)
{
    Assert(out_policy != NULL);
    Assert(in_policy != NULL);

    return out_policy->kind == AGE_VLE_STREAM_DIRECTED_SOURCE_AGE_ADJACENCY ||
        in_policy->kind == AGE_VLE_STREAM_DIRECTED_SOURCE_AGE_ADJACENCY;
}

static char *format_vle_stream_source_cost_policy(
    const VLEStreamSourceCostDecision *decision,
    const VLESourcePolicyDecision *out_policy,
    const VLESourcePolicyDecision *in_policy,
    const VLESourcePolicyProfile *profile)
{
    VLESourcePolicyFeedback feedback;

    Assert(decision != NULL);
    Assert(out_policy != NULL);
    Assert(in_policy != NULL);
    Assert(profile != NULL);

    choose_vle_source_policy_feedback(&feedback, out_policy, in_policy,
                                      profile);

    if (profile->composite_prefilter_planned)
    {
        return psprintf("out=%s/in=%s depth=%s "
                        "endpoint-work=sum(out:%.0f/%.0f,"
                        "in:%.0f/%.0f) combined-work=all:%.0f/%.0f "
                        "composite-work=planned(out:%.0f,in:%.0f) "
                        "value-posting=out:%s/in:%s "
                        "reason=out:%s/in:%s "
                        "class=%s recommendation=%s",
                        age_vle_directed_source_kind_name(
                            decision->outgoing_kind),
                        age_vle_directed_source_kind_name(
                            decision->incoming_kind),
                        profile->cost_eligible ? "costed" : "layout",
                        out_policy->endpoint_work, out_policy->limit_work,
                        in_policy->endpoint_work, in_policy->limit_work,
                        Max(out_policy->combined_endpoint_work,
                            in_policy->combined_endpoint_work),
                        Max(out_policy->combined_limit_work,
                            in_policy->combined_limit_work),
                        out_policy->composite_work, in_policy->composite_work,
                        age_vle_value_posting_source_name(
                            profile->out_value_posting_source_kind),
                        age_vle_value_posting_source_name(
                            profile->in_value_posting_source_kind),
                        vle_source_policy_reason_text(out_policy->reason),
                        vle_source_policy_reason_text(in_policy->reason),
                        age_vle_source_policy_class_name(
                            feedback.source_class_id),
                        age_vle_source_policy_recommendation_name(
                            feedback.recommendation_kind));
    }

    return psprintf("out=%s/in=%s depth=%s "
                    "endpoint-work=sum(out:%.0f/%.0f,"
                    "in:%.0f/%.0f) combined-work=all:%.0f/%.0f "
                    "reason=out:%s/in:%s "
                    "class=%s recommendation=%s",
                    age_vle_directed_source_kind_name(
                        decision->outgoing_kind),
                    age_vle_directed_source_kind_name(
                        decision->incoming_kind),
                    profile->cost_eligible ? "costed" : "layout",
                    out_policy->endpoint_work, out_policy->limit_work,
                    in_policy->endpoint_work, in_policy->limit_work,
                    Max(out_policy->combined_endpoint_work,
                        in_policy->combined_endpoint_work),
                    Max(out_policy->combined_limit_work,
                        in_policy->combined_limit_work),
                    vle_source_policy_reason_text(out_policy->reason),
                    vle_source_policy_reason_text(in_policy->reason),
                    age_vle_source_policy_class_name(
                        feedback.source_class_id),
                    age_vle_source_policy_recommendation_name(
                        feedback.recommendation_kind));
}

static void choose_vle_source_policy_feedback(
    VLESourcePolicyFeedback *feedback,
    const VLESourcePolicyDecision *out_policy,
    const VLESourcePolicyDecision *in_policy,
    const VLESourcePolicyProfile *profile)
{
    Assert(feedback != NULL);
    Assert(out_policy != NULL);
    Assert(in_policy != NULL);
    Assert(profile != NULL);

    set_vle_source_policy_feedback_class(
        feedback, VLE_SOURCE_POLICY_CLASS_LAYOUT);
    feedback->recommendation_kind = VLE_SOURCE_RECOMMENDATION_KEEP_LAYOUT;

    if (!profile->cost_eligible)
        return;

    if (out_policy->kind == AGE_VLE_STREAM_DIRECTED_SOURCE_AGE_ADJACENCY ||
        in_policy->kind == AGE_VLE_STREAM_DIRECTED_SOURCE_AGE_ADJACENCY)
    {
        if (vle_source_policy_has_reason(
                out_policy, in_policy,
                VLE_SOURCE_POLICY_REASON_UNKNOWN_FANOUT))
        {
            set_vle_source_policy_feedback_class(
                feedback, VLE_SOURCE_POLICY_CLASS_ADJACENCY_STREAM);
            feedback->recommendation_kind =
                VLE_SOURCE_RECOMMENDATION_COLLECT_ENDPOINT_STATS;
            return;
        }
        if (vle_source_policy_reason_is_composite(out_policy->reason) ||
            vle_source_policy_reason_is_composite(in_policy->reason))
        {
            if (vle_source_profile_has_value_posting_payload(profile))
            {
                set_vle_source_policy_feedback_class(
                    feedback,
                    VLE_SOURCE_POLICY_CLASS_ADJACENCY_COMPOSITE_VALUE_POSTING);
                feedback->recommendation_kind =
                    VLE_SOURCE_RECOMMENDATION_KEEP_VALUE_POSTING;
                return;
            }
            set_vle_source_policy_feedback_class(
                feedback,
                VLE_SOURCE_POLICY_CLASS_ADJACENCY_COMPOSITE_PREFILTER);
            feedback->recommendation_kind =
                VLE_SOURCE_RECOMMENDATION_KEEP_PROPERTY_PREFILTER;
            return;
        }
        if (profile->payload_input_known &&
            (profile->payload_input_class_id ==
             VLE_SOURCE_POLICY_CLASS_ADJACENCY_REPLAY ||
             profile->payload_input_class_id ==
             VLE_SOURCE_POLICY_CLASS_ADJACENCY_REPLAY_OBSERVED))
        {
            set_vle_source_policy_feedback_class(
                feedback, VLE_SOURCE_POLICY_CLASS_ADJACENCY_REPLAY);
            feedback->recommendation_kind =
                VLE_SOURCE_RECOMMENDATION_KEEP_AGE_ADJACENCY;
            return;
        }
        if (profile->payload_input_known &&
            (profile->payload_input_matrix_filter_percent > 0 ||
             profile->payload_input_matrix_prescan_percent > 0 ||
             profile->payload_input_matrix_replay_source_percent > 0 ||
             profile->payload_input_matrix_run_block_percent > 0 ||
             profile->payload_input_matrix_regroup_percent > 0))
        {
            set_vle_source_policy_feedback_class(
                feedback, VLE_SOURCE_POLICY_CLASS_MATRIX_FRONTIER_PRE_SCAN);
            feedback->recommendation_kind =
                VLE_SOURCE_RECOMMENDATION_KEEP_MATRIX_FRONTIER_BATCH;
            return;
        }
        if (profile->threshold_input_known &&
            profile->threshold_input_class_id ==
            VLE_SOURCE_POLICY_CLASS_ADJACENCY_EMPTY_BATCH)
        {
            set_vle_source_policy_feedback_class(
                feedback, VLE_SOURCE_POLICY_CLASS_ADJACENCY_EMPTY_BATCH);
            feedback->recommendation_kind =
                VLE_SOURCE_RECOMMENDATION_KEEP_EMPTY_BATCH;
            return;
        }
        if (profile->threshold_input_known &&
            profile->threshold_input_class_id ==
            VLE_SOURCE_POLICY_CLASS_ADJACENCY_EMPTY_LIFECYCLE)
        {
            set_vle_source_policy_feedback_class(
                feedback,
                VLE_SOURCE_POLICY_CLASS_ADJACENCY_EMPTY_LIFECYCLE);
            feedback->recommendation_kind =
                VLE_SOURCE_RECOMMENDATION_KEEP_EMPTY_LIFECYCLE;
            return;
        }
        if (profile->threshold_input_known &&
            profile->threshold_input_class_id ==
            VLE_SOURCE_POLICY_CLASS_ADJACENCY_CACHE_SEEDED)
        {
            set_vle_source_policy_feedback_class(
                feedback,
                VLE_SOURCE_POLICY_CLASS_ADJACENCY_EMPTY_LIFECYCLE);
            feedback->recommendation_kind =
                VLE_SOURCE_RECOMMENDATION_KEEP_EMPTY_LIFECYCLE;
            return;
        }
        if (profile->cache_seed_eligible)
        {
            set_vle_source_policy_feedback_class(
                feedback, VLE_SOURCE_POLICY_CLASS_ADJACENCY_CACHE_SEEDED);
            feedback->recommendation_kind =
                VLE_SOURCE_RECOMMENDATION_PREFER_AGE_ADJACENCY_DEPTH;
            return;
        }
        if (vle_source_policy_has_reason(
                out_policy, in_policy, VLE_SOURCE_POLICY_REASON_WORK_TIE))
        {
            if (vle_source_consumer_is_materialized(
                    profile->consumer_class_id))
            {
                set_vle_source_policy_feedback_class(
                    feedback,
                    VLE_SOURCE_POLICY_CLASS_ADJACENCY_MATERIALIZED_TIE);
                feedback->recommendation_kind =
                    VLE_SOURCE_RECOMMENDATION_PREFER_AGE_ADJACENCY_MATERIALIZATION;
                return;
            }
            set_vle_source_policy_feedback_class(
                feedback, VLE_SOURCE_POLICY_CLASS_ADJACENCY_WORK_TIE);
            feedback->recommendation_kind =
                VLE_SOURCE_RECOMMENDATION_PREFER_AGE_ADJACENCY_DEPTH;
            return;
        }
        if (vle_source_policy_has_reason(
                out_policy, in_policy,
                VLE_SOURCE_POLICY_REASON_COMBINED_WORK_TIE))
        {
            set_vle_source_policy_feedback_class(
                feedback,
                VLE_SOURCE_POLICY_CLASS_ADJACENCY_COMBINED_WORK_TIE);
            feedback->recommendation_kind =
                VLE_SOURCE_RECOMMENDATION_PREFER_AGE_ADJACENCY_UNDIRECTED;
            return;
        }
        if (vle_source_policy_has_reason(
                out_policy, in_policy,
                VLE_SOURCE_POLICY_REASON_COMBINED_WORK_EXCEEDS_LIMIT))
        {
            set_vle_source_policy_feedback_class(
                feedback, VLE_SOURCE_POLICY_CLASS_ADJACENCY_COMBINED_WORK);
            feedback->recommendation_kind =
                VLE_SOURCE_RECOMMENDATION_KEEP_AGE_ADJACENCY_UNDIRECTED;
            return;
        }
        if (vle_source_policy_has_reason(
                out_policy, in_policy,
                VLE_SOURCE_POLICY_REASON_DIRECTIONAL_FAMILY_PRODUCTIVE))
        {
            set_vle_source_policy_feedback_class(
                feedback,
                profile->threshold_input_class_id !=
                    VLE_SOURCE_POLICY_CLASS_NONE ?
                profile->threshold_input_class_id :
                VLE_SOURCE_POLICY_CLASS_ADJACENCY_EMPTY_BATCH);
            feedback->recommendation_kind =
                VLE_SOURCE_RECOMMENDATION_KEEP_DIRECTIONAL_SPLIT;
            return;
        }
        if (vle_source_policy_has_reason(
                out_policy, in_policy,
                VLE_SOURCE_POLICY_REASON_WORK_EXCEEDS_LIMIT))
        {
            set_vle_source_policy_feedback_class(
                feedback, VLE_SOURCE_POLICY_CLASS_ADJACENCY_WORK);
            feedback->recommendation_kind =
                VLE_SOURCE_RECOMMENDATION_KEEP_AGE_ADJACENCY;
            return;
        }

        set_vle_source_policy_feedback_class(
            feedback, VLE_SOURCE_POLICY_CLASS_ADJACENCY_ONLY);
        feedback->recommendation_kind =
            VLE_SOURCE_RECOMMENDATION_KEEP_AGE_ADJACENCY;
        return;
    }

    if (out_policy->kind == AGE_VLE_STREAM_DIRECTED_SOURCE_ENDPOINT_BTREE ||
        in_policy->kind == AGE_VLE_STREAM_DIRECTED_SOURCE_ENDPOINT_BTREE)
    {
        set_vle_source_policy_feedback_class(
            feedback, VLE_SOURCE_POLICY_CLASS_ENDPOINT_DIRECT);
        feedback->recommendation_kind =
            VLE_SOURCE_RECOMMENDATION_KEEP_ENDPOINT_BTREE;
        return;
    }

    set_vle_source_policy_feedback_class(
        feedback, VLE_SOURCE_POLICY_CLASS_NO_SOURCE);
    feedback->recommendation_kind =
        VLE_SOURCE_RECOMMENDATION_KEEP_GLOBAL_METADATA;
}

static VLESourceConsumerClass age_vle_source_policy_consumer_class_id(
    AgeVLEOutputRequirement requirement)
{
    switch (requirement)
    {
        case AGE_VLE_OUTPUT_REQUIREMENT_TERMINAL_VERTEX:
        case AGE_VLE_OUTPUT_REQUIREMENT_TERMINAL_PROPERTIES:
            return VLE_SOURCE_CONSUMER_TERMINAL_OBJECT;
        case AGE_VLE_OUTPUT_REQUIREMENT_TERMINAL_PROPERTY:
            return VLE_SOURCE_CONSUMER_TERMINAL_SCALAR;
        case AGE_VLE_OUTPUT_REQUIREMENT_PATH:
            return VLE_SOURCE_CONSUMER_PATH_MATERIALIZED;
        case AGE_VLE_OUTPUT_REQUIREMENT_UNKNOWN:
            return VLE_SOURCE_CONSUMER_UNKNOWN;
    }

    return VLE_SOURCE_CONSUMER_UNKNOWN;
}

static int64 age_vle_output_materialization_weight(
    AgeVLEOutputRequirement requirement)
{
    switch (requirement)
    {
        case AGE_VLE_OUTPUT_REQUIREMENT_PATH:
            return VLE_MATERIALIZATION_WEIGHT_PATH;
        case AGE_VLE_OUTPUT_REQUIREMENT_TERMINAL_VERTEX:
        case AGE_VLE_OUTPUT_REQUIREMENT_TERMINAL_PROPERTIES:
            return VLE_MATERIALIZATION_WEIGHT_OBJECT;
        case AGE_VLE_OUTPUT_REQUIREMENT_TERMINAL_PROPERTY:
            return VLE_MATERIALIZATION_WEIGHT_SCALAR;
        case AGE_VLE_OUTPUT_REQUIREMENT_UNKNOWN:
            return VLE_MATERIALIZATION_WEIGHT_NONE;
    }

    return VLE_MATERIALIZATION_WEIGHT_NONE;
}

static const char *age_vle_source_runtime_dominant_name(
    VLESourceRuntimeDominantKind kind)
{
    switch (kind)
    {
        case VLE_SOURCE_RUNTIME_DOMINANT_AGE_ADJACENCY:
            return "age-adjacency";
        case VLE_SOURCE_RUNTIME_DOMINANT_ENDPOINT_BTREE:
            return "endpoint-btree";
        case VLE_SOURCE_RUNTIME_DOMINANT_PACKED:
            return "packed";
        case VLE_SOURCE_RUNTIME_DOMINANT_NONE:
            break;
    }

    return "none";
}

static void choose_vle_source_runtime_dominant(
    const AgeVLESourceStats *stats, VLESourceRuntimeDominant *dominant)
{
    Assert(stats != NULL);
    Assert(dominant != NULL);

    dominant->kind = VLE_SOURCE_RUNTIME_DOMINANT_NONE;
    dominant->scans = 0;
    dominant->candidates = 0;

    if (stats->age_adjacency_candidates > dominant->candidates)
    {
        dominant->kind = VLE_SOURCE_RUNTIME_DOMINANT_AGE_ADJACENCY;
        dominant->scans = stats->age_adjacency_scans;
        dominant->candidates = stats->age_adjacency_candidates;
    }
    if (stats->endpoint_btree_candidates > dominant->candidates)
    {
        dominant->kind = VLE_SOURCE_RUNTIME_DOMINANT_ENDPOINT_BTREE;
        dominant->scans = stats->endpoint_btree_scans;
        dominant->candidates = stats->endpoint_btree_candidates;
    }
    if (stats->packed_candidates > dominant->candidates)
    {
        dominant->kind = VLE_SOURCE_RUNTIME_DOMINANT_PACKED;
        dominant->scans = stats->packed_scans;
        dominant->candidates = stats->packed_candidates;
    }
}

static void choose_vle_source_runtime_feedback(
    const AgeVLESourceStats *stats,
    const VLESourceRuntimeDominant *dominant,
    const AgeVLEStreamEdgeSource *source,
    VLESourceRuntimeFeedback *feedback)
{
    Assert(stats != NULL);
    Assert(dominant != NULL);
    Assert(feedback != NULL);

    set_vle_source_runtime_feedback_class(
        feedback, VLE_SOURCE_POLICY_CLASS_IDLE);
    feedback->recommendation_kind =
        VLE_SOURCE_RECOMMENDATION_NO_CANDIDATES;

    if (source != NULL &&
        source->policy_class_id != VLE_SOURCE_POLICY_CLASS_NONE &&
        source->policy_recommendation_kind != VLE_SOURCE_RECOMMENDATION_NONE &&
        vle_source_policy_is_composite_class_id(source->policy_class_id) &&
        age_vle_runtime_value_posting_pruning_count(stats, source) > 0)
    {
        set_vle_source_runtime_feedback_class(
            feedback,
            VLE_SOURCE_POLICY_CLASS_ADJACENCY_COMPOSITE_VALUE_POSTING);
        feedback->recommendation_kind =
            VLE_SOURCE_RECOMMENDATION_KEEP_VALUE_POSTING;
        return;
    }

    if (source != NULL &&
        source->policy_recommendation_kind != VLE_SOURCE_RECOMMENDATION_NONE &&
        source->policy_class_id ==
        VLE_SOURCE_POLICY_CLASS_ADJACENCY_COMPOSITE_PREFILTER &&
        stats->age_adjacency_payload_property_prefilter_runs > 0)
    {
        copy_vle_source_runtime_feedback_class(
            feedback, source->policy_class_id);
        feedback->recommendation_kind = source->policy_recommendation_kind;
        return;
    }

    if (stats->candidates_yielded <= 0 && stats->candidates_pushed <= 0)
        return;

    if (stats->age_adjacency_payload_replays <= 0 &&
        stats->missing_vertex_attempts > 0 &&
        stats->missing_vertex_source_hits == stats->missing_vertex_attempts)
    {
        if (source != NULL &&
            source->policy_class_id != VLE_SOURCE_POLICY_CLASS_NONE &&
            source->policy_recommendation_kind !=
            VLE_SOURCE_RECOMMENDATION_NONE &&
            vle_source_policy_is_composite_class_id(
                source->policy_class_id) &&
            stats->age_adjacency_scans > 0 &&
            vle_runtime_dominant_matches_plan(dominant, source))
        {
            copy_vle_source_runtime_feedback_class(
                feedback, source->policy_class_id);
            feedback->recommendation_kind =
                source->policy_recommendation_kind;
            return;
        }

        if (source != NULL &&
            source->policy_class_id != VLE_SOURCE_POLICY_CLASS_NONE &&
            source->policy_recommendation_kind !=
            VLE_SOURCE_RECOMMENDATION_NONE &&
            source->empty_lifecycle_eligible &&
            vle_source_policy_is_empty_lifecycle_class_id(
                source->policy_class_id) &&
            stats->age_adjacency_payload_replays <= 0 &&
            stats->age_adjacency_scans > 0 &&
            age_vle_has_empty_source_suppression(stats) &&
            vle_runtime_dominant_matches_plan(dominant, source))
        {
            copy_vle_source_runtime_feedback_class(
                feedback, source->policy_class_id);
            feedback->recommendation_kind =
                source->policy_recommendation_kind;
            return;
        }

        set_vle_source_runtime_feedback_class(
            feedback, VLE_SOURCE_POLICY_CLASS_MISSING_VERTEX_SOURCE);
        feedback->recommendation_kind =
            VLE_SOURCE_RECOMMENDATION_KEEP_LOCAL_SOURCE;
        return;
    }

    if (stats->age_adjacency_scans > 0)
    {
        if (source != NULL &&
            source->policy_class_id != VLE_SOURCE_POLICY_CLASS_NONE &&
            source->policy_recommendation_kind !=
            VLE_SOURCE_RECOMMENDATION_NONE &&
            vle_source_policy_is_composite_class_id(
                source->policy_class_id) &&
                   age_vle_runtime_value_posting_pruning_count(stats,
                                                               source) > 0)
        {
            set_vle_source_runtime_feedback_class(
                feedback,
                VLE_SOURCE_POLICY_CLASS_ADJACENCY_COMPOSITE_VALUE_POSTING);
            feedback->recommendation_kind =
                VLE_SOURCE_RECOMMENDATION_KEEP_VALUE_POSTING;
            return;
        }

        if (source != NULL &&
            source->policy_recommendation_kind !=
            VLE_SOURCE_RECOMMENDATION_NONE &&
            source->policy_class_id ==
            VLE_SOURCE_POLICY_CLASS_ADJACENCY_COMPOSITE_PREFILTER &&
            stats->age_adjacency_payload_property_prefilter_runs > 0)
        {
            copy_vle_source_runtime_feedback_class(
                feedback, source->policy_class_id);
            feedback->recommendation_kind =
                source->policy_recommendation_kind;
            return;
        }

        if (source != NULL &&
            source->policy_class_id != VLE_SOURCE_POLICY_CLASS_NONE &&
            source->policy_recommendation_kind !=
            VLE_SOURCE_RECOMMENDATION_NONE &&
            source->empty_lifecycle_eligible &&
            vle_source_policy_is_empty_lifecycle_class_id(
                source->policy_class_id) &&
            stats->age_adjacency_payload_replays <= 0 &&
            age_vle_has_empty_source_suppression(stats) &&
            vle_runtime_dominant_matches_plan(dominant, source))
        {
            copy_vle_source_runtime_feedback_class(
                feedback, source->policy_class_id);
            feedback->recommendation_kind =
                source->policy_recommendation_kind;
            return;
        }

        if (source != NULL && !source->cache_seed_eligible &&
            (stats->age_adjacency_payload_replays > 0 ||
             stats->age_adjacency_payload_cache_seeds > 0))
        {
            copy_vle_source_runtime_feedback_class(
                feedback,
                source->policy_class_id == VLE_SOURCE_POLICY_CLASS_NONE ?
                VLE_SOURCE_POLICY_CLASS_ADJACENCY_WORK :
                source->policy_class_id);
            feedback->recommendation_kind =
                source->policy_recommendation_kind ==
                VLE_SOURCE_RECOMMENDATION_NONE ?
                VLE_SOURCE_RECOMMENDATION_KEEP_AGE_ADJACENCY :
                source->policy_recommendation_kind;
            return;
        }

        if (stats->age_adjacency_payload_replays > 0)
        {
            set_vle_source_runtime_feedback_class(
                feedback, VLE_SOURCE_POLICY_CLASS_ADJACENCY_REPLAY);
            feedback->recommendation_kind =
                VLE_SOURCE_RECOMMENDATION_KEEP_AGE_ADJACENCY;
            return;
        }
        if (stats->age_adjacency_payload_cache_seeds > 0)
        {
            set_vle_source_runtime_feedback_class(
                feedback, VLE_SOURCE_POLICY_CLASS_ADJACENCY_CACHE_SEEDED);
            feedback->recommendation_kind =
                VLE_SOURCE_RECOMMENDATION_PREFER_AGE_ADJACENCY_DEPTH;
            return;
        }

        set_vle_source_runtime_feedback_class(
            feedback, VLE_SOURCE_POLICY_CLASS_ADJACENCY_STREAM);
        feedback->recommendation_kind =
            VLE_SOURCE_RECOMMENDATION_CHECK_AGE_ADJACENCY_DENSITY;
        return;
    }

    if (stats->endpoint_btree_scans > 0)
    {
        set_vle_source_runtime_feedback_class(
            feedback, VLE_SOURCE_POLICY_CLASS_ENDPOINT_DIRECT);
        feedback->recommendation_kind =
            VLE_SOURCE_RECOMMENDATION_KEEP_ENDPOINT_BTREE;
        return;
    }

    if (stats->packed_policy_skips > 0)
    {
        set_vle_source_runtime_feedback_class(
            feedback, VLE_SOURCE_POLICY_CLASS_PACKED_POLICY_SUPPRESSED);
        feedback->recommendation_kind =
            VLE_SOURCE_RECOMMENDATION_KEEP_PACKED_SUPPRESSED;
        return;
    }

    if (stats->packed_scans > 0)
    {
        set_vle_source_runtime_feedback_class(
            feedback, VLE_SOURCE_POLICY_CLASS_PACKED_FALLBACK);
        feedback->recommendation_kind =
            VLE_SOURCE_RECOMMENDATION_CHECK_FIXED_SOURCE_COVERAGE;
        return;
    }

    copy_vle_source_runtime_feedback_class(
        feedback, VLE_SOURCE_POLICY_CLASS_NONE);
    feedback->recommendation_kind = VLE_SOURCE_RECOMMENDATION_OBSERVE;
}

static void choose_vle_source_runtime_pressure(
    const AgeVLESourceStats *stats,
    const VLESourceRuntimeDominant *dominant,
    const AgeVLEStreamEdgeSource *source,
    const VLESourceRuntimeFeedback *feedback,
    VLESourceRuntimePressure *pressure)
{
    Assert(stats != NULL);
    Assert(dominant != NULL);
    Assert(feedback != NULL);
    Assert(pressure != NULL);

    pressure->kind = VLE_SOURCE_RUNTIME_PRESSURE_STABLE;
    pressure->direction_id = VLE_SOURCE_DIRECTION_NONE;

    if (age_vle_runtime_value_posting_pruning_count(stats, source) > 0)
    {
        pressure->kind =
            VLE_SOURCE_RUNTIME_PRESSURE_ADJACENCY_COMPOSITE_VALUE_POSTING;
        return;
    }

    if (feedback->source_class_id ==
        VLE_SOURCE_POLICY_CLASS_ADJACENCY_COMPOSITE_PREFILTER)
    {
        pressure->kind =
            VLE_SOURCE_RUNTIME_PRESSURE_ADJACENCY_COMPOSITE_PREFILTER;
        return;
    }

    if (stats->candidates_yielded <= 0 && stats->candidates_pushed <= 0)
    {
        pressure->kind = VLE_SOURCE_RUNTIME_PRESSURE_IDLE;
        return;
    }

    if (!vle_runtime_dominant_matches_plan(dominant, source))
    {
        pressure->kind = VLE_SOURCE_RUNTIME_PRESSURE_SOURCE_MISMATCH;
        return;
    }

    if (!vle_runtime_class_matches_plan(feedback, source))
    {
        pressure->kind = VLE_SOURCE_RUNTIME_PRESSURE_CLASS_MISMATCH;
        return;
    }

    if (feedback->source_class_id ==
        VLE_SOURCE_POLICY_CLASS_ADJACENCY_MATERIALIZED_TIE)
    {
        pressure->kind = VLE_SOURCE_RUNTIME_PRESSURE_MATERIALIZATION_TIE;
        return;
    }

    if (feedback->source_class_id == VLE_SOURCE_POLICY_CLASS_ADJACENCY_REPLAY)
    {
        pressure->kind =
            VLE_SOURCE_RUNTIME_PRESSURE_ADJACENCY_PAYLOAD_REPLAY;
        return;
    }

    if (stats->age_adjacency_empty_scans > 0)
    {
        pressure->kind = VLE_SOURCE_RUNTIME_PRESSURE_ADJACENCY_EMPTY_PROBE;
        return;
    }

    if (age_vle_has_empty_source_suppression(stats))
    {
        if (age_vle_empty_lifecycle_matches_plan(stats, source))
        {
            VLESourceEmptyEvidenceKind evidence;

            evidence = age_vle_empty_lifecycle_evidence_kind(stats);
            switch (evidence)
            {
                case VLE_SOURCE_EMPTY_EVIDENCE_RUN:
                    pressure->kind =
                        age_vle_empty_lifecycle_batch_saturated(stats) ?
                        VLE_SOURCE_RUNTIME_PRESSURE_ADJACENCY_EMPTY_RUN_BATCH :
                        VLE_SOURCE_RUNTIME_PRESSURE_ADJACENCY_EMPTY_RUN;
                    pressure->direction_id =
                        age_vle_empty_lifecycle_direction_kind(stats);
                    return;
                case VLE_SOURCE_EMPTY_EVIDENCE_FRONTIER:
                    pressure->kind =
                        age_vle_empty_lifecycle_batch_saturated(stats) ?
                        VLE_SOURCE_RUNTIME_PRESSURE_ADJACENCY_EMPTY_FRONTIER_BATCH :
                        VLE_SOURCE_RUNTIME_PRESSURE_ADJACENCY_EMPTY_FRONTIER;
                    pressure->direction_id =
                        age_vle_empty_lifecycle_direction_kind(stats);
                    return;
                case VLE_SOURCE_EMPTY_EVIDENCE_CACHE:
                    pressure->kind =
                        VLE_SOURCE_RUNTIME_PRESSURE_ADJACENCY_EMPTY_CACHE;
                    pressure->direction_id =
                        age_vle_empty_lifecycle_direction_kind(stats);
                    return;
                case VLE_SOURCE_EMPTY_EVIDENCE_NONE:
                case VLE_SOURCE_EMPTY_EVIDENCE_COMPLETE:
                case VLE_SOURCE_EMPTY_EVIDENCE_DIRECTORY_FILTER:
                case VLE_SOURCE_EMPTY_EVIDENCE_EMPTY_SCAN:
                case VLE_SOURCE_EMPTY_EVIDENCE_ENDPOINT_EMPTY_SCAN:
                    break;
            }

            pressure->kind =
                VLE_SOURCE_RUNTIME_PRESSURE_ADJACENCY_EMPTY_COMPLETE;
            pressure->direction_id =
                age_vle_empty_lifecycle_direction_kind(stats);
            return;
        }

        pressure->kind =
            VLE_SOURCE_RUNTIME_PRESSURE_ADJACENCY_EMPTY_SUPPRESSED;
        pressure->direction_id =
            age_vle_empty_lifecycle_direction_kind(stats);
        return;
    }

    if (source != NULL && source->cache_seed_eligible &&
        stats->age_adjacency_scans > 0 &&
        stats->age_adjacency_payload_replays == 0 &&
        stats->age_adjacency_payload_cache_seeds == 0)
    {
        pressure->kind = VLE_SOURCE_RUNTIME_PRESSURE_CACHE_SEED_MISSED;
        return;
    }

    if (stats->age_adjacency_scans > 0 &&
        stats->age_adjacency_candidates < stats->age_adjacency_scans)
    {
        pressure->kind = VLE_SOURCE_RUNTIME_PRESSURE_ADJACENCY_DENSITY_LOW;
        return;
    }

    if (stats->endpoint_btree_scans > 0 &&
        stats->endpoint_btree_candidates > stats->endpoint_btree_scans)
    {
        pressure->kind = VLE_SOURCE_RUNTIME_PRESSURE_ENDPOINT_FANOUT;
        return;
    }
}

static void choose_vle_source_runtime_suppression(
    const AgeVLESourceStats *stats,
    const AgeVLEStreamEdgeSource *source,
    VLESourceRuntimeSuppression *suppression)
{
    bool out_suppressed;
    bool in_suppressed;

    Assert(stats != NULL);
    Assert(suppression != NULL);

    out_suppressed = stats->age_adjacency_empty_source_skip_out > 0 ||
        stats->age_adjacency_empty_source_cache_hit_out > 0 ||
        stats->age_adjacency_empty_source_frontier_mark_out > 0 ||
        stats->age_adjacency_empty_source_run_skip_out > 0;
    in_suppressed = stats->age_adjacency_empty_source_skip_in > 0 ||
        stats->age_adjacency_empty_source_cache_hit_in > 0 ||
        stats->age_adjacency_empty_source_frontier_mark_in > 0 ||
        stats->age_adjacency_empty_source_run_skip_in > 0;

    suppression->direction_id = out_suppressed && in_suppressed ?
        VLE_SOURCE_DIRECTION_BOTH :
        (out_suppressed ? VLE_SOURCE_DIRECTION_OUT :
         (in_suppressed ? VLE_SOURCE_DIRECTION_IN :
          VLE_SOURCE_DIRECTION_NONE));
    suppression->planned_match = true;

    if (!out_suppressed && !in_suppressed)
        return;

    if (source == NULL)
    {
        suppression->planned_match = false;
        return;
    }

    if (out_suppressed &&
        source->outgoing_kind != AGE_VLE_STREAM_DIRECTED_SOURCE_AGE_ADJACENCY)
    {
        suppression->planned_match = false;
    }
    if (in_suppressed &&
        source->incoming_kind != AGE_VLE_STREAM_DIRECTED_SOURCE_AGE_ADJACENCY)
    {
        suppression->planned_match = false;
    }
}

static int64
age_vle_runtime_value_pruning_count(const AgeVLESourceStats *stats)
{
    Assert(stats != NULL);

    return stats->age_adjacency_payload_vertex_set_block_value_filtered +
        stats->age_adjacency_payload_vertex_set_block_value_posting_filtered +
        stats->age_adjacency_payload_vertex_set_directory_value_filtered +
        stats->age_adjacency_payload_vertex_set_directory_value_posting_filtered;
}

static int64
age_vle_runtime_value_posting_pruning_count(
    const AgeVLESourceStats *stats, const AgeVLEStreamEdgeSource *source)
{
    VLESourceValuePostingKind value_posting_source_kind;
    int64 value_pruning_count;

    Assert(stats != NULL);

    value_pruning_count = age_vle_runtime_value_pruning_count(stats);
    if (value_pruning_count > 0)
        return value_pruning_count;

    if (source == NULL)
        return 0;

    value_posting_source_kind =
        age_vle_source_value_posting_source_kind_for_direction(
            source, source->policy_active_direction_id);
    if (value_posting_source_kind == VLE_SOURCE_VALUE_POSTING_NONE)
    {
        return 0;
    }

    return stats->age_adjacency_payload_property_filtered +
        stats->age_adjacency_payload_cache_filtered +
        stats->age_adjacency_payload_composite_block_filtered +
        stats->age_adjacency_payload_composite_directory_filtered;
}

static VLESourceValuePostingKind
age_vle_source_value_posting_source_kind_for_direction(
    const AgeVLEStreamEdgeSource *source,
    VLESourceDirectionClass source_direction_id)
{
    Assert(source != NULL);

    switch (source_direction_id)
    {
        case VLE_SOURCE_DIRECTION_OUT:
            return source->start_value_posting_source_kind;
        case VLE_SOURCE_DIRECTION_IN:
            return source->end_value_posting_source_kind;
        case VLE_SOURCE_DIRECTION_NONE:
        case VLE_SOURCE_DIRECTION_BOTH:
        case VLE_SOURCE_DIRECTION_MIXED:
            break;
    }

    if (source->start_value_posting_source_kind !=
        VLE_SOURCE_VALUE_POSTING_NONE)
    {
        return source->start_value_posting_source_kind;
    }
    if (source->end_value_posting_source_kind !=
        VLE_SOURCE_VALUE_POSTING_NONE)
    {
        return source->end_value_posting_source_kind;
    }

    return VLE_SOURCE_VALUE_POSTING_NONE;
}

static VLESourceValuePostingKind
vle_source_input_value_posting_source_kind_for_direction(
    const VLEStreamSourceCostInput *input,
    VLESourceDirectionClass source_direction_id)
{
    Assert(input != NULL);

    if (input->evidence == NULL)
        return VLE_SOURCE_VALUE_POSTING_NONE;

    switch (source_direction_id)
    {
        case VLE_SOURCE_DIRECTION_OUT:
            return input->evidence->start_value_posting_source_kind;
        case VLE_SOURCE_DIRECTION_IN:
            return input->evidence->end_value_posting_source_kind;
        case VLE_SOURCE_DIRECTION_NONE:
        case VLE_SOURCE_DIRECTION_BOTH:
        case VLE_SOURCE_DIRECTION_MIXED:
            break;
    }

    if (input->evidence->start_value_posting_source_kind !=
        VLE_SOURCE_VALUE_POSTING_NONE)
    {
        return input->evidence->start_value_posting_source_kind;
    }
    if (input->evidence->end_value_posting_source_kind !=
        VLE_SOURCE_VALUE_POSTING_NONE)
    {
        return input->evidence->end_value_posting_source_kind;
    }

    return VLE_SOURCE_VALUE_POSTING_NONE;
}

static const char *age_vle_source_runtime_pressure_name(
    VLESourceRuntimePressureKind kind)
{
    switch (kind)
    {
        case VLE_SOURCE_RUNTIME_PRESSURE_ADJACENCY_COMPOSITE_VALUE_POSTING:
            return "adjacency-composite-value-posting";
        case VLE_SOURCE_RUNTIME_PRESSURE_ADJACENCY_COMPOSITE_PREFILTER:
            return "adjacency-composite-prefilter";
        case VLE_SOURCE_RUNTIME_PRESSURE_IDLE:
            return "idle";
        case VLE_SOURCE_RUNTIME_PRESSURE_SOURCE_MISMATCH:
            return "source-mismatch";
        case VLE_SOURCE_RUNTIME_PRESSURE_CLASS_MISMATCH:
            return "class-mismatch";
        case VLE_SOURCE_RUNTIME_PRESSURE_MATERIALIZATION_TIE:
            return "materialization-tie";
        case VLE_SOURCE_RUNTIME_PRESSURE_ADJACENCY_PAYLOAD_REPLAY:
            return "adjacency-payload-replay";
        case VLE_SOURCE_RUNTIME_PRESSURE_ADJACENCY_EMPTY_PROBE:
            return "adjacency-empty-probe";
        case VLE_SOURCE_RUNTIME_PRESSURE_ADJACENCY_EMPTY_RUN:
        case VLE_SOURCE_RUNTIME_PRESSURE_ADJACENCY_EMPTY_RUN_BATCH:
            return "adjacency-empty-run";
        case VLE_SOURCE_RUNTIME_PRESSURE_ADJACENCY_EMPTY_FRONTIER:
        case VLE_SOURCE_RUNTIME_PRESSURE_ADJACENCY_EMPTY_FRONTIER_BATCH:
            return "adjacency-empty-frontier";
        case VLE_SOURCE_RUNTIME_PRESSURE_ADJACENCY_EMPTY_CACHE:
            return "adjacency-empty-cache";
        case VLE_SOURCE_RUNTIME_PRESSURE_ADJACENCY_EMPTY_COMPLETE:
            return "adjacency-empty-complete";
        case VLE_SOURCE_RUNTIME_PRESSURE_ADJACENCY_EMPTY_SUPPRESSED:
            return "adjacency-empty-suppressed";
        case VLE_SOURCE_RUNTIME_PRESSURE_CACHE_SEED_MISSED:
            return "cache-seed-missed";
        case VLE_SOURCE_RUNTIME_PRESSURE_ADJACENCY_DENSITY_LOW:
            return "adjacency-density-low";
        case VLE_SOURCE_RUNTIME_PRESSURE_ENDPOINT_FANOUT:
            return "endpoint-fanout";
        case VLE_SOURCE_RUNTIME_PRESSURE_STABLE:
            break;
    }

    return "stable";
}

static char *age_vle_source_runtime_pressure_action(
    const VLESourceRuntimePressure *pressure)
{
    const char *action;
    bool include_direction = false;

    Assert(pressure != NULL);

    switch (pressure->kind)
    {
        case VLE_SOURCE_RUNTIME_PRESSURE_ADJACENCY_COMPOSITE_VALUE_POSTING:
            action = "keep-value-posting";
            break;
        case VLE_SOURCE_RUNTIME_PRESSURE_ADJACENCY_COMPOSITE_PREFILTER:
            action = "keep-property-prefilter";
            break;
        case VLE_SOURCE_RUNTIME_PRESSURE_IDLE:
            action = "no-candidates";
            break;
        case VLE_SOURCE_RUNTIME_PRESSURE_SOURCE_MISMATCH:
            action = "fix-source-handoff";
            break;
        case VLE_SOURCE_RUNTIME_PRESSURE_CLASS_MISMATCH:
            action = "tune-source-policy";
            break;
        case VLE_SOURCE_RUNTIME_PRESSURE_MATERIALIZATION_TIE:
            action = "keep-adjacency-materialization";
            break;
        case VLE_SOURCE_RUNTIME_PRESSURE_ADJACENCY_PAYLOAD_REPLAY:
            action = "keep-payload-replay";
            break;
        case VLE_SOURCE_RUNTIME_PRESSURE_ADJACENCY_EMPTY_PROBE:
            action = "suppress-empty-source";
            break;
        case VLE_SOURCE_RUNTIME_PRESSURE_ADJACENCY_EMPTY_RUN:
            action = "keep-empty-run";
            include_direction = true;
            break;
        case VLE_SOURCE_RUNTIME_PRESSURE_ADJACENCY_EMPTY_RUN_BATCH:
            action = "keep-empty-run-batch";
            include_direction = true;
            break;
        case VLE_SOURCE_RUNTIME_PRESSURE_ADJACENCY_EMPTY_FRONTIER:
            action = "keep-empty-frontier";
            include_direction = true;
            break;
        case VLE_SOURCE_RUNTIME_PRESSURE_ADJACENCY_EMPTY_FRONTIER_BATCH:
            action = "keep-empty-frontier-batch";
            include_direction = true;
            break;
        case VLE_SOURCE_RUNTIME_PRESSURE_ADJACENCY_EMPTY_CACHE:
            action = "keep-empty-cache";
            include_direction = true;
            break;
        case VLE_SOURCE_RUNTIME_PRESSURE_ADJACENCY_EMPTY_COMPLETE:
            action = "batch-empty-completion";
            include_direction = true;
            break;
        case VLE_SOURCE_RUNTIME_PRESSURE_ADJACENCY_EMPTY_SUPPRESSED:
            action = "observe-suppression";
            include_direction = true;
            break;
        case VLE_SOURCE_RUNTIME_PRESSURE_CACHE_SEED_MISSED:
            action = "check-payload-replay";
            break;
        case VLE_SOURCE_RUNTIME_PRESSURE_ADJACENCY_DENSITY_LOW:
            action = "check-fallback-suppression";
            break;
        case VLE_SOURCE_RUNTIME_PRESSURE_ENDPOINT_FANOUT:
            action = "watch-endpoint-budget";
            break;
        case VLE_SOURCE_RUNTIME_PRESSURE_STABLE:
            action = "observe";
            break;
    }

    if (!include_direction ||
        pressure->direction_id == VLE_SOURCE_DIRECTION_NONE)
    {
        return pstrdup(action);
    }

    return psprintf("%s:%s", action,
                    age_vle_source_direction_name(pressure->direction_id));
}

static const char *age_vle_source_runtime_suppression_name(
    VLESourceDirectionClass direction_id)
{
    switch (direction_id)
    {
        case VLE_SOURCE_DIRECTION_OUT:
            return "out:age-adjacency/in:none";
        case VLE_SOURCE_DIRECTION_IN:
            return "out:none/in:age-adjacency";
        case VLE_SOURCE_DIRECTION_BOTH:
            return "out:age-adjacency/in:age-adjacency";
        case VLE_SOURCE_DIRECTION_NONE:
        case VLE_SOURCE_DIRECTION_MIXED:
            break;
    }

    return "out:none/in:none";
}

static VLESourceEmptyEvidenceKind age_vle_empty_lifecycle_evidence_kind(
    const AgeVLESourceStats *stats)
{
    Assert(stats != NULL);

    if (stats->age_adjacency_empty_source_run_skips > 0)
        return VLE_SOURCE_EMPTY_EVIDENCE_RUN;
    if (stats->age_adjacency_empty_source_frontier_marks > 0)
        return VLE_SOURCE_EMPTY_EVIDENCE_FRONTIER;
    if (stats->age_adjacency_empty_source_cache_hits > 0)
        return VLE_SOURCE_EMPTY_EVIDENCE_CACHE;
    if (stats->age_adjacency_empty_source_skips > 0)
        return VLE_SOURCE_EMPTY_EVIDENCE_COMPLETE;
    if (stats->age_adjacency_directory_filtered_empty_scans > 0)
        return VLE_SOURCE_EMPTY_EVIDENCE_DIRECTORY_FILTER;
    if (stats->age_adjacency_empty_scans > 0)
        return VLE_SOURCE_EMPTY_EVIDENCE_EMPTY_SCAN;
    if (stats->endpoint_btree_empty_scans > 0)
        return VLE_SOURCE_EMPTY_EVIDENCE_ENDPOINT_EMPTY_SCAN;
    return VLE_SOURCE_EMPTY_EVIDENCE_NONE;
}

static const char *age_vle_empty_lifecycle_evidence_name(
    VLESourceEmptyEvidenceKind evidence)
{
    switch (evidence)
    {
        case VLE_SOURCE_EMPTY_EVIDENCE_RUN:
            return "empty-run";
        case VLE_SOURCE_EMPTY_EVIDENCE_FRONTIER:
            return "empty-frontier";
        case VLE_SOURCE_EMPTY_EVIDENCE_CACHE:
            return "empty-cache";
        case VLE_SOURCE_EMPTY_EVIDENCE_COMPLETE:
            return "empty-complete";
        case VLE_SOURCE_EMPTY_EVIDENCE_DIRECTORY_FILTER:
            return "directory-filter";
        case VLE_SOURCE_EMPTY_EVIDENCE_EMPTY_SCAN:
            return "empty-scan";
        case VLE_SOURCE_EMPTY_EVIDENCE_ENDPOINT_EMPTY_SCAN:
            return "endpoint-empty-scan";
        case VLE_SOURCE_EMPTY_EVIDENCE_NONE:
            break;
    }

    return "none";
}

static VLESourceDirectionClass age_vle_empty_lifecycle_direction_kind(
    const AgeVLESourceStats *stats)
{
    bool out_evidence;
    bool in_evidence;

    Assert(stats != NULL);

    out_evidence = stats->age_adjacency_empty_source_skip_out > 0 ||
        stats->age_adjacency_empty_source_cache_hit_out > 0 ||
        stats->age_adjacency_empty_source_frontier_mark_out > 0 ||
        stats->age_adjacency_empty_source_run_skip_out > 0;
    in_evidence = stats->age_adjacency_empty_source_skip_in > 0 ||
        stats->age_adjacency_empty_source_cache_hit_in > 0 ||
        stats->age_adjacency_empty_source_frontier_mark_in > 0 ||
        stats->age_adjacency_empty_source_run_skip_in > 0;

    if (out_evidence && in_evidence)
        return VLE_SOURCE_DIRECTION_BOTH;
    if (out_evidence)
        return VLE_SOURCE_DIRECTION_OUT;
    if (in_evidence)
        return VLE_SOURCE_DIRECTION_IN;
    return VLE_SOURCE_DIRECTION_NONE;
}

static int64 age_vle_empty_lifecycle_completion_count(
    const AgeVLESourceStats *stats)
{
    Assert(stats != NULL);

    return stats->age_adjacency_empty_source_skips +
        stats->age_adjacency_empty_source_cache_hits +
        stats->age_adjacency_empty_source_frontier_marks +
        stats->age_adjacency_empty_source_run_skips;
}

static bool age_vle_empty_lifecycle_batch_saturated(
    const AgeVLESourceStats *stats)
{
    Assert(stats != NULL);

    if (stats->empty_lifecycle_batch_capacity <= 0)
        return false;

    return age_vle_empty_lifecycle_completion_count(stats) >=
        stats->empty_lifecycle_batch_capacity;
}

static VLESourceDirectionClass age_vle_root_empty_completion_direction_kind(
    const AgeVLESourceStats *stats)
{
    Assert(stats != NULL);

    if (stats->root_empty_completion_out > 0 &&
        stats->root_empty_completion_in > 0)
    {
        return VLE_SOURCE_DIRECTION_BOTH;
    }
    if (stats->root_empty_completion_out > 0)
        return VLE_SOURCE_DIRECTION_OUT;
    if (stats->root_empty_completion_in > 0)
        return VLE_SOURCE_DIRECTION_IN;

    return VLE_SOURCE_DIRECTION_NONE;
}

static bool age_vle_has_empty_source_suppression(
    const AgeVLESourceStats *stats)
{
    Assert(stats != NULL);

    return stats->age_adjacency_empty_source_skips > 0 ||
        stats->age_adjacency_empty_source_cache_hits > 0 ||
        stats->age_adjacency_empty_source_frontier_marks > 0 ||
        stats->age_adjacency_empty_source_run_skips > 0;
}

static bool age_vle_empty_lifecycle_matches_plan(
    const AgeVLESourceStats *stats, const AgeVLEStreamEdgeSource *source)
{
    Assert(stats != NULL);

    if (stats->age_adjacency_empty_scans <= 0 &&
        !age_vle_has_empty_source_suppression(stats))
    {
        return true;
    }

    return source != NULL && source->empty_lifecycle_eligible;
}

static bool age_vle_empty_lifecycle_context_matches_plan(
    const AgeVLESourceStats *stats, const AgeVLEStreamEdgeSource *source)
{
    Assert(stats != NULL);

    if (stats->empty_lifecycle_context_runs <= 0)
        return true;
    if (source == NULL)
        return false;
    if (source->empty_lifecycle_eligible)
    {
        return stats->empty_lifecycle_context_eligible_runs ==
                   stats->empty_lifecycle_context_runs &&
               stats->empty_lifecycle_context_depth ==
                   source->empty_lifecycle_depth;
    }

    return stats->empty_lifecycle_context_eligible_runs == 0 &&
        stats->empty_lifecycle_context_depth == 0;
}

static bool age_vle_empty_lifecycle_batch_matches_plan(
    const AgeVLESourceStats *stats, const AgeVLEStreamEdgeSource *source)
{
    Assert(stats != NULL);

    if (stats->empty_lifecycle_batch_capacity <= 0)
        return true;
    if (source == NULL)
        return false;
    if (source->empty_lifecycle_batch_size <= 0)
        return false;

    return stats->empty_lifecycle_batch_capacity ==
        source->empty_lifecycle_batch_size;
}
