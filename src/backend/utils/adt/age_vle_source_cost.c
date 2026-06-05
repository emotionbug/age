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

typedef struct VLESourceRuntimeDominant
{
    const char *name;
    int64 scans;
    int64 candidates;
} VLESourceRuntimeDominant;

typedef struct VLESourceRuntimeFeedback
{
    const char *source_class;
    const char *recommendation;
} VLESourceRuntimeFeedback;

typedef struct VLESourceRuntimePressure
{
    const char *name;
    const char *action;
} VLESourceRuntimePressure;

typedef struct VLESourceRuntimeSuppression
{
    char *source_text;
    bool planned_match;
} VLESourceRuntimeSuppression;

typedef struct VLESourcePolicyDecision
{
    AgeVLEStreamDirectedSourceKind kind;
    double endpoint_work;
    double limit_work;
    double combined_endpoint_work;
    double combined_limit_work;
    const char *reason;
} VLESourcePolicyDecision;

typedef struct VLESourcePolicyFeedback
{
    const char *source_class;
    const char *recommendation;
} VLESourcePolicyFeedback;

typedef struct VLESourcePolicyProfile
{
    AgeVLEOutputRequirement output_requirement;
    const char *consumer_class;
    const char *direction_class;
    double fanout_budget;
    int64 materialization_weight;
    double cache_seed_endpoint_headroom;
    int64 depth;
    int64 empty_lifecycle_batch_size;
    bool cost_eligible;
    bool cache_seed_eligible;
    bool empty_lifecycle_eligible;
    bool outgoing_active;
    bool incoming_active;
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
    const char *reason;
} VLESourceRuntimePayloadFeedback;

typedef struct VLESourceThresholdCacheKey
{
    char graph_name[NAMEDATALEN];
    char label_name[NAMEDATALEN];
    Oid edge_label_oid;
    char consumer_class[32];
    char active_direction[8];
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
    char source_direction[8];
    char reason[64];
    char payload_reason[64];
} VLESourceThresholdCacheEntry;

#define VLE_ENDPOINT_BTREE_TERMINAL_FANOUT 2.0
#define VLE_ENDPOINT_BTREE_TERMINAL_OBJECT_FANOUT 1.0
#define VLE_ENDPOINT_BTREE_PATH_FANOUT 1.0
#define VLE_CACHE_SEED_ENDPOINT_HEADROOM 0.75
#define VLE_EMPTY_LIFECYCLE_ENDPOINT_HEADROOM 0.50
#define VLE_EMPTY_LIFECYCLE_BATCH_ENDPOINT_HEADROOM 0.35
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
    bool direction_active, const VLESourcePolicyProfile *profile);
static void apply_vle_source_combined_policy(
    VLESourcePolicyDecision *out_policy,
    VLESourcePolicyDecision *in_policy,
    const VLEStreamSourceCostInput *input,
    const VLESourcePolicyProfile *profile);
static double estimate_vle_source_policy_work(double fanout, int64 depth);
static int64 estimate_vle_empty_lifecycle_batch_size(
    const VLESourcePolicyProfile *profile);
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
    const VLESourcePolicyProfile *profile);
static bool vle_source_policy_prefers_endpoint_tie(
    bool age_adjacency_available, const VLESourcePolicyProfile *profile);
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
static const char *age_vle_output_requirement_name(
    AgeVLEOutputRequirement requirement);
static const char *age_vle_source_policy_consumer_class(
    AgeVLEOutputRequirement requirement);
static double calculate_vle_source_scan_density(int64 candidates,
                                                int64 scans);
static void choose_vle_source_runtime_dominant(
    const AgeVLESourceStats *stats, VLESourceRuntimeDominant *dominant);
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
static const char *age_vle_empty_suppression_action(
    const AgeVLESourceStats *stats);
static const char *age_vle_empty_lifecycle_evidence_name(
    const AgeVLESourceStats *stats);
static const char *age_vle_empty_lifecycle_direction(
    const AgeVLESourceStats *stats);
static char *age_vle_empty_lifecycle_action(
    const AgeVLESourceStats *stats, const char *action);
static int64 age_vle_empty_lifecycle_completion_count(
    const AgeVLESourceStats *stats);
static bool age_vle_empty_lifecycle_batch_saturated(
    const AgeVLESourceStats *stats);
static const char *age_vle_root_empty_completion_direction(
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
static bool vle_source_policy_is_empty_lifecycle_class(
    const char *source_class);
static bool vle_source_policy_uses_age_adjacency(
    const VLESourcePolicyDecision *out_policy,
    const VLESourcePolicyDecision *in_policy);
static HTAB *get_vle_source_threshold_cache(void);
static bool lookup_vle_source_threshold_feedback(
    const VLEStreamSourceCostInput *input,
    const VLESourcePolicyProfile *profile,
    VLESourceRuntimeThresholdFeedback *feedback);
static void derive_vle_source_runtime_payload_feedback(
    VLESourceRuntimePayloadFeedback *feedback,
    const AgeVLESourceStats *stats, const AgeVLEStreamEdgeSource *source);
static int64 select_vle_payload_replay_strong_percent(
    const AgeVLEStreamEdgeSource *source);
static int64 select_vle_payload_replay_strong_headroom_percent(
    const AgeVLEStreamEdgeSource *source);
static int64 age_vle_output_materialization_weight(
    AgeVLEOutputRequirement requirement);
static int64 select_vle_payload_seed_headroom_percent(
    const AgeVLEStreamEdgeSource *source);
static int64 select_vle_threshold_feedback_batch_size(
    const AgeVLESourceStats *stats);
static int64 calculate_vle_source_ratio_percent(int64 numerator,
                                                int64 denominator);
static void build_vle_source_threshold_cache_key(
    VLESourceThresholdCacheKey *key, const char *graph_name,
    const char *label_name, Oid edge_label_oid, const char *consumer_class,
    const char *active_direction);

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
    decision->policy_consumer = NULL;
    decision->policy_consumer_class = NULL;
    decision->policy_active_direction = NULL;
    decision->policy_fanout_budget = 0;
    decision->policy_materialization_weight = 0;
    decision->policy_class = NULL;
    decision->policy_recommendation = NULL;
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
    decision->threshold_input_source = NULL;
    decision->threshold_input_reason = NULL;
    decision->payload_input_known = false;
    decision->payload_input_headroom_percent = 0;
    decision->payload_input_scan_runs = 0;
    decision->payload_input_replay_runs = 0;
    decision->payload_input_seed_runs = 0;
    decision->payload_input_replay_percent = 0;
    decision->payload_input_seed_percent = 0;
    decision->payload_input_observed_count = 0;
    decision->payload_input_reason = NULL;

    if (input->source_kind != AGE_VLE_STREAM_EDGE_SOURCE_LOCAL_INDEX_CANDIDATE)
        return;

    build_vle_source_policy_profile(&profile, input);

    choose_vle_source_policy_decision(
        &out_policy,
        input->outgoing_kind, input->evidence->start_fanout,
        input->start_fanout_known, input->endpoint_start,
        input->age_adjacency_out, profile.outgoing_active, &profile);
    choose_vle_source_policy_decision(
        &in_policy,
        input->incoming_kind, input->evidence->end_fanout,
        input->end_fanout_known, input->endpoint_end,
        input->age_adjacency_in, profile.incoming_active, &profile);
    apply_vle_source_combined_policy(&out_policy, &in_policy, input,
                                     &profile);

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
        profile.payload_input_reason = "none";
    }
    decision->policy_consumer = age_vle_output_requirement_name(
        profile.output_requirement);
    decision->policy_consumer_class = profile.consumer_class;
    decision->policy_active_direction = profile.direction_class;
    decision->policy_fanout_budget =
        round_vle_source_cost_evidence(profile.fanout_budget);
    decision->policy_materialization_weight = profile.materialization_weight;
    decision->policy_class = feedback.source_class;
    decision->policy_recommendation = feedback.recommendation;
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
    decision->threshold_input_source = profile.threshold_input_source;
    decision->threshold_input_reason = profile.threshold_input_reason;
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
    decision->payload_input_reason = profile.payload_input_reason;
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
    bool payload_input_known;
    const char *state_text;
    char *source_text;

    if (source == NULL)
        return pstrdup("unknown");

    payload_input_known = source->payload_input_observed_count > 0 &&
        source->payload_input_headroom_percent > 0 &&
        source->payload_input_reason != NULL &&
        strcmp(source->payload_input_reason, "none") != 0;
    state_text = source->local_edge_state ?
        "dense-local" : "global-edge-state";
    source_text = psprintf("%s, fixed-source=out=%s/in=%s, candidates="
                           "age_adjacency=%s/%s endpoint-btree=%s/%s, "
                           "state=%s",
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

    if (source->relation_tuples <= 0 &&
        source->start_fanout <= 0 &&
        source->end_fanout <= 0 &&
        source->cost_policy == NULL)
        return source_text;

    if (source->cost_policy == NULL)
    {
        return psprintf("%s, cost=reltuples=%lld fanout=start:%lld/end:%lld "
                        "stats=rel:%s/start:%s/end:%s",
                        source_text,
                        (long long)source->relation_tuples,
                        (long long)source->start_fanout,
                        (long long)source->end_fanout,
                        source->relation_tuples_known ? "known" : "unknown",
                        source->start_fanout_known ? "known" : "unknown",
                        source->end_fanout_known ? "known" : "unknown");
    }

    return psprintf("%s, cost=reltuples=%lld fanout=start:%lld/end:%lld "
                    "stats=rel:%s/start:%s/end:%s "
                    "profile=consumer:%s/class:%s/active:%s/budget:%lld/"
                    "weight:%lld "
                    "cache-seed=%s endpoint-headroom=%.2f "
                    "empty-lifecycle=%s/depth:%lld "
                    "empty-batch=%s/size:%lld "
                    "threshold-input=%s/headroom:%lld/batch:%lld/source:%s/"
                    "reason:%s threshold-cache=observed:%lld/saturated:%lld/"
                    "relaxed:%lld payload-input=%s/headroom:%lld/"
                    "scan-runs:%lld/replay-runs:%lld/seed-runs:%lld/"
                    "replay-percent:%lld/seed-percent:%lld/"
                    "observed:%lld/reason:%s policy=%s",
                    source_text,
                    (long long)source->relation_tuples,
                    (long long)source->start_fanout,
                    (long long)source->end_fanout,
                    source->relation_tuples_known ? "known" : "unknown",
                    source->start_fanout_known ? "known" : "unknown",
                    source->end_fanout_known ? "known" : "unknown",
                    source->policy_consumer == NULL ?
                        "unknown" : source->policy_consumer,
                    source->policy_consumer_class == NULL ?
                        "unknown" : source->policy_consumer_class,
                    source->policy_active_direction == NULL ?
                        "unknown" : source->policy_active_direction,
                    (long long)source->policy_fanout_budget,
                    (long long)source->policy_materialization_weight,
                    source->cache_seed_eligible ? "eligible" : "ineligible",
                    (double)source->endpoint_headroom_percent / 100.0,
                    source->empty_lifecycle_eligible ?
                        "eligible" : "ineligible",
                    (long long)source->empty_lifecycle_depth,
                    source->empty_lifecycle_batch_size > 0 ?
                        "eligible" : "ineligible",
                    (long long)source->empty_lifecycle_batch_size,
                    source->threshold_input_known ?
                        "runtime-cache" : "none",
                    source->threshold_input_known ?
                        (long long)source->threshold_input_headroom_percent :
                        0LL,
                    source->threshold_input_known ?
                        (long long)source->threshold_input_batch_size :
                        0LL,
                    source->threshold_input_source == NULL ?
                        "none" : source->threshold_input_source,
                    source->threshold_input_reason == NULL ?
                        "none" : source->threshold_input_reason,
                    source->threshold_input_known ?
                        (long long)source->threshold_input_observed_count :
                        0LL,
                    source->threshold_input_known ?
                        (long long)source->threshold_input_saturated_count :
                        0LL,
                    source->threshold_input_known ?
                        (long long)source->threshold_input_relaxed_count :
                        0LL,
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
                    !payload_input_known || source->payload_input_reason == NULL ?
                        "none" : source->payload_input_reason,
                    source->cost_policy);
}

char *format_vle_source_runtime_evidence(const AgeVLESourceStats *stats,
                                         const AgeVLEStreamEdgeSource *source)
{
    VLESourceRuntimeDominant dominant;
    VLESourceRuntimeFeedback feedback;
    VLESourceRuntimePressure pressure;
    VLESourceRuntimeSuppression suppression;
    VLESourceRuntimeThresholdFeedback threshold_feedback;
    double age_adjacency_density;
    double endpoint_btree_density;
    double packed_density;
    const char *planned_class;
    const char *planned_recommendation;

    Assert(stats != NULL);

    choose_vle_source_runtime_dominant(stats, &dominant);
    choose_vle_source_runtime_feedback(stats, &dominant, source, &feedback);
    choose_vle_source_runtime_pressure(stats, &dominant, source, &feedback,
                                       &pressure);
    choose_vle_source_runtime_suppression(stats, source, &suppression);
    derive_vle_source_runtime_threshold_feedback(&threshold_feedback, stats,
                                                 source);
    planned_class = source == NULL ? NULL : source->policy_class;
    planned_recommendation = source == NULL ? NULL :
        source->policy_recommendation;
    age_adjacency_density = calculate_vle_source_scan_density(
        stats->age_adjacency_candidates, stats->age_adjacency_scans);
    endpoint_btree_density = calculate_vle_source_scan_density(
        stats->endpoint_btree_candidates, stats->endpoint_btree_scans);
    packed_density = calculate_vle_source_scan_density(
        stats->packed_candidates, stats->packed_scans);

    return psprintf("sources=age_adjacency=%lld/%lld/%lld/%lld/%lld "
                    "endpoint-btree=%lld/%lld packed=%lld/%lld/%lld/%lld, "
                    "empty=age_adjacency:%lld/endpoint-btree:%lld, "
                    "empty-suppressed=age_adjacency:%lld/out:%lld/in:%lld, "
                    "empty-cache=age_adjacency:%lld/out:%lld/in:%lld, "
                    "empty-frontier=age_adjacency:%lld/out:%lld/in:%lld, "
                    "empty-frontier-batch=flushes:%lld/out:%lld/in:%lld/"
                    "keys:%lld/max:%lld, "
                    "empty-run=age_adjacency:%lld/out:%lld/in:%lld, "
                    "payload-cache=runs:scan:%lld/replay:%lld/seed:%lld/"
                    "tuples:scan:%lld/replay:%lld/seeds:%lld, "
                    "empty-plan=%s/depth:%lld match=%s, "
                    "empty-context=%s/depth:%lld/runs:%lld match=%s, "
                    "empty-batch=%s/size:%lld/capacity:%lld match=%s, "
                    "empty-summary=completion:%lld/batch:%lld/saturated:%s, "
                    "root-empty=completion:%lld/out:%lld/in:%lld/"
                    "batch:%lld/saturated-roots:%lld, "
                    "threshold-feedback=%s/headroom:%lld/batch:%lld/source:%s/"
                    "reason:%s, "
                    "empty-evidence=%s, "
                    "suppressed-source=%s suppression-match=%s, "
                    "packed-suppressed=out:%lld/in:%lld/self:%lld, "
                    "missing-vertex=%lld/%lld, candidates=%lld/%lld, "
                    "feedback=dominant=%s planned=out:%s/in:%s "
                    "source-match=%s class=%s planned-class=%s "
                    "class-match=%s recommendation=%s "
                    "planned-recommendation=%s "
                    "density=age_adjacency:%.2f,"
                    "endpoint-btree:%.2f,packed:%.2f yield=%lld/%lld "
                    "replay=%lld/%lld push=%lld/%lld "
                    "pressure=%s action=%s",
                    (long long)stats->age_adjacency_scans,
                    (long long)stats->age_adjacency_candidates,
                    (long long)stats->age_adjacency_payload_scans,
                    (long long)stats->age_adjacency_payload_replays,
                    (long long)stats->age_adjacency_payload_cache_seeds,
                    (long long)stats->endpoint_btree_scans,
                    (long long)stats->endpoint_btree_candidates,
                    (long long)stats->packed_scans,
                    (long long)stats->packed_candidates,
                    (long long)stats->packed_empty_skips,
                    (long long)stats->packed_policy_skips,
                    (long long)stats->age_adjacency_empty_scans,
                    (long long)stats->endpoint_btree_empty_scans,
                    (long long)stats->age_adjacency_empty_source_skips,
                    (long long)stats->age_adjacency_empty_source_skip_out,
                    (long long)stats->age_adjacency_empty_source_skip_in,
                    (long long)stats->age_adjacency_empty_source_cache_hits,
                    (long long)stats->age_adjacency_empty_source_cache_hit_out,
                    (long long)stats->age_adjacency_empty_source_cache_hit_in,
                    (long long)stats->age_adjacency_empty_source_frontier_marks,
                    (long long)stats->age_adjacency_empty_source_frontier_mark_out,
                    (long long)stats->age_adjacency_empty_source_frontier_mark_in,
                    (long long)stats->age_adjacency_empty_source_frontier_batch_flushes,
                    (long long)stats->age_adjacency_empty_source_frontier_batch_out,
                    (long long)stats->age_adjacency_empty_source_frontier_batch_in,
                    (long long)stats->age_adjacency_empty_source_frontier_batch_keys,
                    (long long)stats->age_adjacency_empty_source_frontier_batch_max,
                    (long long)stats->age_adjacency_empty_source_run_skips,
                    (long long)stats->age_adjacency_empty_source_run_skip_out,
                    (long long)stats->age_adjacency_empty_source_run_skip_in,
                    (long long)stats->age_adjacency_payload_scan_runs,
                    (long long)stats->age_adjacency_payload_replay_runs,
                    (long long)stats->age_adjacency_payload_cache_seed_runs,
                    (long long)stats->age_adjacency_payload_scans,
                    (long long)stats->age_adjacency_payload_replays,
                    (long long)stats->age_adjacency_payload_cache_seeds,
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
                    (long long)age_vle_empty_lifecycle_completion_count(
                        stats),
                    (long long)stats->empty_lifecycle_batch_capacity,
                    age_vle_empty_lifecycle_batch_saturated(stats) ?
                        "true" : "false",
                    (long long)stats->root_empty_completion_count,
                    (long long)stats->root_empty_completion_out,
                    (long long)stats->root_empty_completion_in,
                    (long long)stats->root_empty_batch_capacity,
                    (long long)stats->root_empty_saturated_count,
                    threshold_feedback.eligible ? "eligible" : "ineligible",
                    (long long)threshold_feedback.endpoint_headroom_percent,
                    (long long)threshold_feedback.empty_lifecycle_batch_size,
                    threshold_feedback.source_direction,
                    threshold_feedback.reason,
                    age_vle_empty_lifecycle_evidence_name(stats),
                    suppression.source_text,
                    suppression.planned_match ? "true" : "false",
                    (long long)stats->packed_suppress_out,
                    (long long)stats->packed_suppress_in,
                    (long long)stats->packed_suppress_self,
                    (long long)stats->missing_vertex_source_hits,
                    (long long)stats->missing_vertex_attempts,
                    (long long)stats->candidates_pushed,
                    (long long)stats->candidates_yielded,
                    dominant.name,
                    source == NULL ? "unknown" :
                        age_vle_directed_source_kind_name(
                            source->outgoing_kind),
                    source == NULL ? "unknown" :
                        age_vle_directed_source_kind_name(
                            source->incoming_kind),
                    vle_runtime_dominant_matches_plan(&dominant, source) ?
                        "true" : "false",
                    feedback.source_class,
                    planned_class == NULL ? "unknown" : planned_class,
                    planned_class == NULL ||
                        strcmp(planned_class, feedback.source_class) == 0 ?
                        "true" : "false",
                    feedback.recommendation,
                    planned_recommendation == NULL ?
                        "unknown" : planned_recommendation,
                    age_adjacency_density,
                    endpoint_btree_density,
                    packed_density,
                    (long long)dominant.candidates,
                    (long long)dominant.scans,
                    (long long)stats->age_adjacency_payload_replays,
                    (long long)stats->age_adjacency_payload_scans,
                    (long long)stats->candidates_pushed,
                    (long long)stats->candidates_yielded,
                    pressure.name,
                    pressure.action);
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
    feedback->observed_count = 0;
    feedback->saturated_count = 0;
    feedback->relaxed_count = 0;
    feedback->scan_runs = 0;
    feedback->replay_runs = 0;
    feedback->seed_runs = 0;
    feedback->replay_percent = 0;
    feedback->seed_percent = 0;
    feedback->payload_observed_count = 0;
    feedback->payload_endpoint_headroom_percent = 0;
    feedback->source_direction = "none";
    feedback->reason = "none";
    feedback->payload_reason = "none";

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
    feedback->source_direction =
        age_vle_root_empty_completion_direction(stats);
    feedback->reason = stats->root_empty_completion_count > 0 ?
        "root-empty-observed" : "planned-empty-lifecycle";

    if (stats->root_empty_saturated_count > 0)
    {
        feedback->saturated = true;
        feedback->endpoint_headroom_percent =
            (int64)(VLE_EMPTY_LIFECYCLE_BATCH_ENDPOINT_HEADROOM * 100.0 +
                    0.5);
        feedback->reason = "root-empty-saturated";
    }
}

static void derive_vle_source_runtime_payload_feedback(
    VLESourceRuntimePayloadFeedback *feedback,
    const AgeVLESourceStats *stats, const AgeVLEStreamEdgeSource *source)
{
    Assert(feedback != NULL);
    Assert(stats != NULL);

    feedback->eligible = false;
    feedback->endpoint_headroom_percent = 0;
    feedback->scan_runs = 0;
    feedback->replay_runs = 0;
    feedback->seed_runs = 0;
    feedback->replay_percent = 0;
    feedback->seed_percent = 0;
    feedback->reason = "none";

    if (source == NULL || !source->cache_seed_eligible ||
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
    feedback->endpoint_headroom_percent = source->endpoint_headroom_percent;
    feedback->reason = "payload-scan-observed";

    if (stats->age_adjacency_payload_replay_runs > 0)
    {
        if (feedback->replay_percent >=
            select_vle_payload_replay_strong_percent(source))
        {
            feedback->endpoint_headroom_percent =
                select_vle_payload_replay_strong_headroom_percent(source);
            feedback->reason = "payload-replay-ratio-observed";
        }
        else
        {
            feedback->endpoint_headroom_percent =
                (int64)(VLE_PAYLOAD_REPLAY_ENDPOINT_HEADROOM * 100.0 + 0.5);
            feedback->reason = "payload-replay-observed";
        }
    }
    else if (stats->age_adjacency_payload_cache_seed_runs > 0)
    {
        feedback->endpoint_headroom_percent =
            select_vle_payload_seed_headroom_percent(source);
        feedback->reason = "payload-cache-seeded";
    }
}

static int64 select_vle_payload_replay_strong_percent(
    const AgeVLEStreamEdgeSource *source)
{
    if (source != NULL && source->policy_consumer_class != NULL &&
        strcmp(source->policy_consumer_class, "terminal-scalar") == 0)
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

static int64 select_vle_payload_seed_headroom_percent(
    const AgeVLEStreamEdgeSource *source)
{
    if (source != NULL && source->policy_consumer_class != NULL &&
        strcmp(source->policy_consumer_class, "terminal-scalar") == 0)
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
    VLESourceThresholdCacheEntry *entry;
    bool found;

    Assert(stats != NULL);

    derive_vle_source_runtime_threshold_feedback(&feedback, stats, source);
    derive_vle_source_runtime_payload_feedback(&payload_feedback, stats,
                                               source);
    if (((!feedback.eligible || feedback.endpoint_headroom_percent <= 0) &&
         (!payload_feedback.eligible ||
          payload_feedback.endpoint_headroom_percent <= 0)) ||
        graph_name == NULL || label_name == NULL || source == NULL ||
        !OidIsValid(source->edge_label_oid) ||
        source->policy_consumer_class == NULL ||
        source->policy_active_direction == NULL)
    {
        return;
    }

    build_vle_source_threshold_cache_key(&key, graph_name, label_name,
                                         source->edge_label_oid,
                                         source->policy_consumer_class,
                                         source->policy_active_direction);
    entry = hash_search(get_vle_source_threshold_cache(), &key,
                        HASH_ENTER, &found);
    if (!found)
    {
        entry->endpoint_headroom_percent =
            feedback.endpoint_headroom_percent;
        entry->empty_lifecycle_batch_size =
            feedback.empty_lifecycle_batch_size;
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
        strlcpy(entry->source_direction, feedback.source_direction,
                sizeof(entry->source_direction));
        strlcpy(entry->reason, feedback.reason, sizeof(entry->reason));
        strlcpy(entry->payload_reason, "none",
                sizeof(entry->payload_reason));
    }
    if (feedback.eligible && feedback.saturated)
    {
        entry->saturated_count++;
        entry->relaxed_count = 0;
        entry->endpoint_headroom_percent =
            Min(entry->endpoint_headroom_percent,
                feedback.endpoint_headroom_percent);
        entry->empty_lifecycle_batch_size =
            Max(entry->empty_lifecycle_batch_size,
                feedback.empty_lifecycle_batch_size);
        strlcpy(entry->source_direction, feedback.source_direction,
                sizeof(entry->source_direction));
        strlcpy(entry->reason, feedback.reason, sizeof(entry->reason));
    }
    else if (feedback.eligible && feedback.root_empty_completion_count > 0)
    {
        entry->relaxed_count++;
        entry->endpoint_headroom_percent =
            feedback.endpoint_headroom_percent;
        entry->empty_lifecycle_batch_size =
            feedback.empty_lifecycle_batch_size;
        strlcpy(entry->source_direction, feedback.source_direction,
                sizeof(entry->source_direction));
        strlcpy(entry->reason, feedback.reason, sizeof(entry->reason));
    }

    if (feedback.eligible)
        entry->observed_count++;

    if (payload_feedback.eligible)
    {
        entry->payload_endpoint_headroom_percent =
            entry->payload_endpoint_headroom_percent <= 0 ?
            payload_feedback.endpoint_headroom_percent :
            Min(entry->payload_endpoint_headroom_percent,
                payload_feedback.endpoint_headroom_percent);
        entry->payload_scan_runs += payload_feedback.scan_runs;
        entry->payload_replay_runs += payload_feedback.replay_runs;
        entry->payload_seed_runs += payload_feedback.seed_runs;
        entry->payload_replay_percent = calculate_vle_source_ratio_percent(
            entry->payload_replay_runs, entry->payload_scan_runs);
        entry->payload_seed_percent = calculate_vle_source_ratio_percent(
            entry->payload_seed_runs, entry->payload_scan_runs);
        entry->payload_observed_count++;
        strlcpy(entry->payload_reason, payload_feedback.reason,
                sizeof(entry->payload_reason));
    }
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
    feedback->observed_count = 0;
    feedback->saturated_count = 0;
    feedback->relaxed_count = 0;
    feedback->scan_runs = 0;
    feedback->replay_runs = 0;
    feedback->seed_runs = 0;
    feedback->replay_percent = 0;
    feedback->seed_percent = 0;
    feedback->payload_observed_count = 0;
    feedback->payload_endpoint_headroom_percent = 0;
    feedback->source_direction = "none";
    feedback->reason = "none";
    feedback->payload_reason = "none";

    if (!profile->cache_seed_eligible || input->graph_name == NULL ||
        input->label_name == NULL || profile->consumer_class == NULL ||
        profile->direction_class == NULL)
    {
        return false;
    }

    build_vle_source_threshold_cache_key(&key, input->graph_name,
                                         input->label_name,
                                         input->evidence->edge_label_oid,
                                         profile->consumer_class,
                                         profile->direction_class);
    entry = hash_search(get_vle_source_threshold_cache(), &key,
                        HASH_FIND, NULL);
    if (entry == NULL)
        return false;

    feedback->eligible = true;
    feedback->endpoint_headroom_percent = entry->endpoint_headroom_percent;
    feedback->empty_lifecycle_batch_size = entry->empty_lifecycle_batch_size;
    feedback->observed_count = entry->observed_count;
    feedback->saturated_count = entry->saturated_count;
    feedback->relaxed_count = entry->relaxed_count;
    feedback->payload_observed_count = entry->payload_observed_count;
    if (entry->payload_observed_count > 0)
    {
        feedback->scan_runs = entry->payload_scan_runs;
        feedback->replay_runs = entry->payload_replay_runs;
        feedback->seed_runs = entry->payload_seed_runs;
        feedback->replay_percent = entry->payload_replay_percent;
        feedback->seed_percent = entry->payload_seed_percent;
        feedback->payload_endpoint_headroom_percent =
            entry->payload_endpoint_headroom_percent;
        feedback->payload_reason = entry->payload_reason;
    }
    feedback->source_direction = entry->source_direction;
    feedback->reason = entry->reason;

    return true;
}

static void build_vle_source_threshold_cache_key(
    VLESourceThresholdCacheKey *key, const char *graph_name,
    const char *label_name, Oid edge_label_oid, const char *consumer_class,
    const char *active_direction)
{
    Assert(key != NULL);

    memset(key, 0, sizeof(*key));
    if (graph_name != NULL)
        strlcpy(key->graph_name, graph_name, sizeof(key->graph_name));
    if (label_name != NULL)
        strlcpy(key->label_name, label_name, sizeof(key->label_name));
    key->edge_label_oid = edge_label_oid;
    if (consumer_class != NULL)
        strlcpy(key->consumer_class, consumer_class,
                sizeof(key->consumer_class));
    if (active_direction != NULL)
        strlcpy(key->active_direction, active_direction,
                sizeof(key->active_direction));
}

static bool vle_runtime_dominant_matches_plan(
    const VLESourceRuntimeDominant *dominant,
    const AgeVLEStreamEdgeSource *source)
{
    Assert(dominant != NULL);

    if (source == NULL || strcmp(dominant->name, "none") == 0)
        return true;

    if (strcmp(dominant->name, "age-adjacency") == 0)
        return source->outgoing_kind ==
                   AGE_VLE_STREAM_DIRECTED_SOURCE_AGE_ADJACENCY ||
               source->incoming_kind ==
                   AGE_VLE_STREAM_DIRECTED_SOURCE_AGE_ADJACENCY;
    if (strcmp(dominant->name, "endpoint-btree") == 0)
        return source->outgoing_kind ==
                   AGE_VLE_STREAM_DIRECTED_SOURCE_ENDPOINT_BTREE ||
               source->incoming_kind ==
                   AGE_VLE_STREAM_DIRECTED_SOURCE_ENDPOINT_BTREE;
    if (strcmp(dominant->name, "packed") == 0)
        return source->kind == AGE_VLE_STREAM_EDGE_SOURCE_GLOBAL_METADATA;

    return false;
}

static bool vle_source_policy_is_empty_lifecycle_class(
    const char *source_class)
{
    if (source_class == NULL)
        return false;

    return strcmp(source_class, "adjacency-cache-seeded") == 0 ||
        strcmp(source_class, "adjacency-empty-lifecycle") == 0 ||
        strcmp(source_class, "adjacency-empty-batch") == 0;
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
    bool direction_active, const VLESourcePolicyProfile *profile)
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
    decision->reason = "layout";

    if (!direction_active)
    {
        decision->kind = AGE_VLE_STREAM_DIRECTED_SOURCE_NONE;
        decision->reason = "inactive-direction";
        return;
    }

    if (!endpoint_available && !age_adjacency_available)
    {
        decision->kind = AGE_VLE_STREAM_DIRECTED_SOURCE_NONE;
        decision->reason = "no-source";
        return;
    }
    if (!profile->cost_eligible)
        return;
    if (endpoint_available &&
        fanout_known &&
        vle_source_endpoint_work_within_policy(decision,
                                               age_adjacency_available,
                                               profile))
    {
        decision->kind = AGE_VLE_STREAM_DIRECTED_SOURCE_ENDPOINT_BTREE;
        decision->reason = profile->cache_seed_eligible &&
            age_adjacency_available ?
            "endpoint-headroom" : "endpoint-work";
        return;
    }
    if (age_adjacency_available)
    {
        decision->kind = AGE_VLE_STREAM_DIRECTED_SOURCE_AGE_ADJACENCY;
        if (!endpoint_available)
            decision->reason = "adjacency-only";
        else if (!fanout_known)
            decision->reason = "unknown-fanout";
        else if (decision->endpoint_work == decision->limit_work)
            decision->reason = "work-tie";
        else if (profile->empty_lifecycle_eligible &&
                 decision->endpoint_work < decision->limit_work)
            decision->reason = "empty-lifecycle-headroom";
        else if (profile->cache_seed_eligible &&
                 decision->endpoint_work < decision->limit_work)
            decision->reason = "cache-seed-headroom";
        else
            decision->reason = "work-exceeds-limit";
        return;
    }
    if (endpoint_available)
    {
        decision->kind = AGE_VLE_STREAM_DIRECTED_SOURCE_ENDPOINT_BTREE;
        decision->reason = fanout_known ? "endpoint-only" : "unknown-fanout";
        return;
    }

    decision->kind = AGE_VLE_STREAM_DIRECTED_SOURCE_NONE;
    decision->reason = "no-source";
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
    const char *reason;

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
        "combined-work-tie" : "combined-work-exceeds-limit";

    if (input->age_adjacency_out)
    {
        out_policy->kind = AGE_VLE_STREAM_DIRECTED_SOURCE_AGE_ADJACENCY;
        out_policy->reason = reason;
    }
    if (input->age_adjacency_in)
    {
        in_policy->kind = AGE_VLE_STREAM_DIRECTED_SOURCE_AGE_ADJACENCY;
        in_policy->reason = reason;
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
    profile->consumer_class = age_vle_source_policy_consumer_class(
        input->output_requirement);
    profile->materialization_weight = age_vle_output_materialization_weight(
        input->output_requirement);
    switch (input->direction)
    {
        case CYPHER_REL_DIR_RIGHT:
            profile->direction_class = "out";
            profile->outgoing_active = true;
            profile->incoming_active = false;
            break;
        case CYPHER_REL_DIR_LEFT:
            profile->direction_class = "in";
            profile->outgoing_active = false;
            profile->incoming_active = true;
            break;
        case CYPHER_REL_DIR_NONE:
            profile->direction_class = "both";
            profile->outgoing_active = true;
            profile->incoming_active = true;
            break;
        default:
            profile->direction_class = "unknown";
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
    profile->threshold_input_known = false;
    profile->threshold_input_headroom_percent = 0;
    profile->threshold_input_batch_size = 0;
    profile->threshold_input_observed_count = 0;
    profile->threshold_input_saturated_count = 0;
    profile->threshold_input_relaxed_count = 0;
    profile->threshold_input_source = "none";
    profile->threshold_input_reason = "none";
    profile->payload_input_known = false;
    profile->payload_input_headroom_percent = 0;
    profile->payload_input_scan_runs = 0;
    profile->payload_input_replay_runs = 0;
    profile->payload_input_seed_runs = 0;
    profile->payload_input_replay_percent = 0;
    profile->payload_input_seed_percent = 0;
    profile->payload_input_observed_count = 0;
    profile->payload_input_reason = "none";

    if (lookup_vle_source_threshold_feedback(input, profile,
                                             &threshold_feedback))
    {
        if (threshold_feedback.observed_count > 0)
        {
            double threshold_headroom =
                (double)threshold_feedback.endpoint_headroom_percent / 100.0;

            if (threshold_headroom > 0.0 &&
                threshold_headroom < profile->cache_seed_endpoint_headroom)
            {
                profile->cache_seed_endpoint_headroom = threshold_headroom;
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
            double payload_headroom =
                (double)threshold_feedback.payload_endpoint_headroom_percent /
                100.0;

            if (payload_headroom > 0.0 &&
                payload_headroom < profile->cache_seed_endpoint_headroom)
            {
                profile->cache_seed_endpoint_headroom = payload_headroom;
            }
        }

        profile->threshold_input_known = threshold_feedback.observed_count > 0;
        profile->threshold_input_headroom_percent =
            threshold_feedback.endpoint_headroom_percent;
        profile->threshold_input_batch_size =
            threshold_feedback.empty_lifecycle_batch_size;
        profile->threshold_input_observed_count =
            threshold_feedback.observed_count;
        profile->threshold_input_saturated_count =
            threshold_feedback.saturated_count;
        profile->threshold_input_relaxed_count =
            threshold_feedback.relaxed_count;
        profile->threshold_input_source = threshold_feedback.source_direction;
        profile->threshold_input_reason = threshold_feedback.reason;
        profile->payload_input_known =
            threshold_feedback.payload_observed_count > 0;
        profile->payload_input_headroom_percent =
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
        profile->payload_input_reason = threshold_feedback.payload_reason;
    }
}

static bool vle_source_endpoint_work_within_policy(
    const VLESourcePolicyDecision *decision,
    bool age_adjacency_available,
    const VLESourcePolicyProfile *profile)
{
    double headroom_limit;

    Assert(decision != NULL);
    Assert(profile != NULL);

    if (profile->cache_seed_eligible && age_adjacency_available)
    {
        headroom_limit = decision->limit_work *
            profile->cache_seed_endpoint_headroom;

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

    if (strcmp(profile->consumer_class, "terminal-scalar") == 0)
        return true;

    return false;
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
                    out_policy->reason, in_policy->reason,
                    feedback.source_class, feedback.recommendation);
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

    feedback->source_class = "layout";
    feedback->recommendation = "keep-layout";

    if (!profile->cost_eligible)
        return;

    if (out_policy->kind == AGE_VLE_STREAM_DIRECTED_SOURCE_AGE_ADJACENCY ||
        in_policy->kind == AGE_VLE_STREAM_DIRECTED_SOURCE_AGE_ADJACENCY)
    {
        if (strcmp(out_policy->reason, "unknown-fanout") == 0 ||
            strcmp(in_policy->reason, "unknown-fanout") == 0)
        {
            feedback->source_class = "unknown-fanout";
            feedback->recommendation = "collect-endpoint-stats";
            return;
        }
        if (profile->payload_input_known &&
            profile->payload_input_reason != NULL &&
            (strcmp(profile->payload_input_reason,
                    "payload-replay-ratio-observed") == 0 ||
             strcmp(profile->payload_input_reason,
                    "payload-replay-observed") == 0))
        {
            feedback->source_class = "adjacency-replay";
            feedback->recommendation = "keep-age-adjacency";
            return;
        }
        if (profile->threshold_input_known &&
            profile->threshold_input_reason != NULL &&
            strcmp(profile->threshold_input_reason,
                   "root-empty-saturated") == 0)
        {
            feedback->source_class = "adjacency-empty-batch";
            feedback->recommendation = "keep-empty-batch";
            return;
        }
        if (profile->threshold_input_known &&
            profile->threshold_input_reason != NULL &&
            strcmp(profile->threshold_input_reason,
                   "root-empty-observed") == 0)
        {
            feedback->source_class = "adjacency-empty-lifecycle";
            feedback->recommendation = "keep-empty-lifecycle";
            return;
        }
        if (profile->cache_seed_eligible)
        {
            feedback->source_class = "adjacency-cache-seeded";
            feedback->recommendation = "prefer-age-adjacency-depth";
            return;
        }
        if (strcmp(out_policy->reason, "work-tie") == 0 ||
            strcmp(in_policy->reason, "work-tie") == 0)
        {
            if (strcmp(profile->consumer_class, "terminal-object") == 0 ||
                strcmp(profile->consumer_class, "path-materialized") == 0)
            {
                feedback->source_class = "adjacency-materialized-tie";
                feedback->recommendation =
                    "prefer-age-adjacency-materialization";
                return;
            }
            feedback->source_class = "adjacency-work-tie";
            feedback->recommendation = "prefer-age-adjacency-depth";
            return;
        }
        if (strcmp(out_policy->reason, "combined-work-tie") == 0 ||
            strcmp(in_policy->reason, "combined-work-tie") == 0)
        {
            feedback->source_class = "adjacency-combined-work-tie";
            feedback->recommendation = "prefer-age-adjacency-undirected";
            return;
        }
        if (strcmp(out_policy->reason, "combined-work-exceeds-limit") == 0 ||
            strcmp(in_policy->reason, "combined-work-exceeds-limit") == 0)
        {
            feedback->source_class = "adjacency-combined-work";
            feedback->recommendation = "keep-age-adjacency-undirected";
            return;
        }
        if (strcmp(out_policy->reason, "work-exceeds-limit") == 0 ||
            strcmp(in_policy->reason, "work-exceeds-limit") == 0)
        {
            feedback->source_class = "adjacency-work";
            feedback->recommendation = "keep-age-adjacency";
            return;
        }

        feedback->source_class = "adjacency-only";
        feedback->recommendation = "keep-age-adjacency";
        return;
    }

    if (out_policy->kind == AGE_VLE_STREAM_DIRECTED_SOURCE_ENDPOINT_BTREE ||
        in_policy->kind == AGE_VLE_STREAM_DIRECTED_SOURCE_ENDPOINT_BTREE)
    {
        feedback->source_class = "endpoint-direct";
        feedback->recommendation = "keep-endpoint-btree";
        return;
    }

    feedback->source_class = "no-source";
    feedback->recommendation = "keep-global-metadata";
}

static const char *age_vle_output_requirement_name(
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
            return "unknown";
    }

    return "unknown";
}

static const char *age_vle_source_policy_consumer_class(
    AgeVLEOutputRequirement requirement)
{
    switch (requirement)
    {
        case AGE_VLE_OUTPUT_REQUIREMENT_TERMINAL_VERTEX:
        case AGE_VLE_OUTPUT_REQUIREMENT_TERMINAL_PROPERTIES:
            return "terminal-object";
        case AGE_VLE_OUTPUT_REQUIREMENT_TERMINAL_PROPERTY:
            return "terminal-scalar";
        case AGE_VLE_OUTPUT_REQUIREMENT_PATH:
            return "path-materialized";
        case AGE_VLE_OUTPUT_REQUIREMENT_UNKNOWN:
            return "unknown";
    }

    return "unknown";
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

static void choose_vle_source_runtime_dominant(
    const AgeVLESourceStats *stats, VLESourceRuntimeDominant *dominant)
{
    Assert(stats != NULL);
    Assert(dominant != NULL);

    dominant->name = "none";
    dominant->scans = 0;
    dominant->candidates = 0;

    if (stats->age_adjacency_candidates > dominant->candidates)
    {
        dominant->name = "age-adjacency";
        dominant->scans = stats->age_adjacency_scans;
        dominant->candidates = stats->age_adjacency_candidates;
    }
    if (stats->endpoint_btree_candidates > dominant->candidates)
    {
        dominant->name = "endpoint-btree";
        dominant->scans = stats->endpoint_btree_scans;
        dominant->candidates = stats->endpoint_btree_candidates;
    }
    if (stats->packed_candidates > dominant->candidates)
    {
        dominant->name = "packed";
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

    feedback->source_class = "idle";
    feedback->recommendation = "no-candidates";

    if (stats->candidates_yielded <= 0 && stats->candidates_pushed <= 0)
        return;

    if (stats->age_adjacency_payload_replays <= 0 &&
        stats->missing_vertex_attempts > 0 &&
        stats->missing_vertex_source_hits == stats->missing_vertex_attempts)
    {
        if (source != NULL &&
            source->policy_class != NULL &&
            source->policy_recommendation != NULL &&
            source->empty_lifecycle_eligible &&
            vle_source_policy_is_empty_lifecycle_class(
                source->policy_class) &&
            stats->age_adjacency_payload_replays <= 0 &&
            stats->age_adjacency_scans > 0 &&
            age_vle_has_empty_source_suppression(stats) &&
            vle_runtime_dominant_matches_plan(dominant, source))
        {
            feedback->source_class = source->policy_class;
            feedback->recommendation = source->policy_recommendation;
            return;
        }

        feedback->source_class = "missing-vertex-source";
        feedback->recommendation = "keep-local-source";
        return;
    }

    if (stats->age_adjacency_scans > 0)
    {
        if (source != NULL &&
            source->policy_class != NULL &&
            source->policy_recommendation != NULL &&
            source->empty_lifecycle_eligible &&
            vle_source_policy_is_empty_lifecycle_class(
                source->policy_class) &&
            stats->age_adjacency_payload_replays <= 0 &&
            age_vle_has_empty_source_suppression(stats) &&
            vle_runtime_dominant_matches_plan(dominant, source))
        {
            feedback->source_class = source->policy_class;
            feedback->recommendation = source->policy_recommendation;
            return;
        }

        if (source != NULL && !source->cache_seed_eligible &&
            (stats->age_adjacency_payload_replays > 0 ||
             stats->age_adjacency_payload_cache_seeds > 0))
        {
            feedback->source_class = source->policy_class == NULL ?
                "adjacency-work" : source->policy_class;
            feedback->recommendation = source->policy_recommendation == NULL ?
                "keep-age-adjacency" : source->policy_recommendation;
            return;
        }

        if (stats->age_adjacency_payload_replays > 0)
        {
            feedback->source_class = "adjacency-replay";
            feedback->recommendation = "keep-age-adjacency";
            return;
        }
        if (stats->age_adjacency_payload_cache_seeds > 0)
        {
            feedback->source_class = "adjacency-cache-seeded";
            feedback->recommendation = "prefer-age-adjacency-depth";
            return;
        }

        feedback->source_class = "adjacency-stream";
        feedback->recommendation = "check-age-adjacency-density";
        return;
    }

    if (stats->endpoint_btree_scans > 0)
    {
        feedback->source_class = "endpoint-direct";
        feedback->recommendation = "keep-endpoint-btree";
        return;
    }

    if (stats->packed_policy_skips > 0)
    {
        feedback->source_class = "packed-policy-suppressed";
        feedback->recommendation = "keep-packed-suppressed";
        return;
    }

    if (stats->packed_scans > 0)
    {
        feedback->source_class = "packed-fallback";
        feedback->recommendation = "check-fixed-source-coverage";
        return;
    }

    feedback->source_class = dominant->name;
    feedback->recommendation = "observe";
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

    pressure->name = "stable";
    pressure->action = "observe";

    if (stats->candidates_yielded <= 0 && stats->candidates_pushed <= 0)
    {
        pressure->name = "idle";
        pressure->action = "no-candidates";
        return;
    }

    if (!vle_runtime_dominant_matches_plan(dominant, source))
    {
        pressure->name = "source-mismatch";
        pressure->action = "fix-source-handoff";
        return;
    }

    if (source != NULL && source->policy_class != NULL &&
        strcmp(source->policy_class, feedback->source_class) != 0)
    {
        pressure->name = "class-mismatch";
        pressure->action = "tune-source-policy";
        return;
    }

    if (strcmp(feedback->source_class, "adjacency-materialized-tie") == 0)
    {
        pressure->name = "materialization-tie";
        pressure->action = "keep-adjacency-materialization";
        return;
    }

    if (stats->age_adjacency_empty_scans > 0)
    {
        pressure->name = "adjacency-empty-probe";
        pressure->action = "suppress-empty-source";
        return;
    }

    if (age_vle_has_empty_source_suppression(stats))
    {
        if (age_vle_empty_lifecycle_matches_plan(stats, source))
        {
            const char *evidence;

            evidence = age_vle_empty_lifecycle_evidence_name(stats);
            if (strcmp(evidence, "empty-run") == 0)
            {
                pressure->name = "adjacency-empty-run";
                pressure->action = age_vle_empty_lifecycle_action(
                    stats,
                    age_vle_empty_lifecycle_batch_saturated(stats) ?
                    "keep-empty-run-batch" : "keep-empty-run");
                return;
            }
            if (strcmp(evidence, "empty-frontier") == 0)
            {
                pressure->name = "adjacency-empty-frontier";
                pressure->action = age_vle_empty_lifecycle_action(
                    stats,
                    age_vle_empty_lifecycle_batch_saturated(stats) ?
                    "keep-empty-frontier-batch" : "keep-empty-frontier");
                return;
            }
            if (strcmp(evidence, "empty-cache") == 0)
            {
                pressure->name = "adjacency-empty-cache";
                pressure->action = age_vle_empty_lifecycle_action(
                    stats, "keep-empty-cache");
                return;
            }

            pressure->name = "adjacency-empty-complete";
            pressure->action = age_vle_empty_lifecycle_action(
                stats, "batch-empty-completion");
            return;
        }

        pressure->name = "adjacency-empty-suppressed";
        pressure->action = age_vle_empty_suppression_action(stats);
        return;
    }

    if (source != NULL && source->cache_seed_eligible &&
        stats->age_adjacency_scans > 0 &&
        stats->age_adjacency_payload_replays == 0 &&
        stats->age_adjacency_payload_cache_seeds == 0)
    {
        pressure->name = "cache-seed-missed";
        pressure->action = "check-payload-replay";
        return;
    }

    if (stats->age_adjacency_scans > 0 &&
        stats->age_adjacency_candidates < stats->age_adjacency_scans)
    {
        pressure->name = "adjacency-density-low";
        pressure->action = "check-fallback-suppression";
        return;
    }

    if (stats->endpoint_btree_scans > 0 &&
        stats->endpoint_btree_candidates > stats->endpoint_btree_scans)
    {
        pressure->name = "endpoint-fanout";
        pressure->action = "watch-endpoint-budget";
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

    suppression->source_text = psprintf(
        "out:%s/in:%s",
        out_suppressed ? "age-adjacency" : "none",
        in_suppressed ? "age-adjacency" : "none");
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

static const char *age_vle_empty_suppression_action(
    const AgeVLESourceStats *stats)
{
    bool out_suppressed;
    bool in_suppressed;

    Assert(stats != NULL);

    out_suppressed = stats->age_adjacency_empty_source_skip_out > 0 ||
        stats->age_adjacency_empty_source_cache_hit_out > 0 ||
        stats->age_adjacency_empty_source_frontier_mark_out > 0 ||
        stats->age_adjacency_empty_source_run_skip_out > 0;
    in_suppressed = stats->age_adjacency_empty_source_skip_in > 0 ||
        stats->age_adjacency_empty_source_cache_hit_in > 0 ||
        stats->age_adjacency_empty_source_frontier_mark_in > 0 ||
        stats->age_adjacency_empty_source_run_skip_in > 0;

    if (out_suppressed && in_suppressed)
    {
        return "observe-suppression:both";
    }
    if (out_suppressed)
        return "observe-suppression:out";
    if (in_suppressed)
        return "observe-suppression:in";
    return "observe-suppression";
}

static const char *age_vle_empty_lifecycle_evidence_name(
    const AgeVLESourceStats *stats)
{
    Assert(stats != NULL);

    if (stats->age_adjacency_empty_source_run_skips > 0)
        return "empty-run";
    if (stats->age_adjacency_empty_source_frontier_marks > 0)
        return "empty-frontier";
    if (stats->age_adjacency_empty_source_cache_hits > 0)
        return "empty-cache";
    if (stats->age_adjacency_empty_source_skips > 0)
        return "empty-complete";
    if (stats->age_adjacency_empty_scans > 0)
        return "empty-scan";
    if (stats->endpoint_btree_empty_scans > 0)
        return "endpoint-empty-scan";
    return "none";
}

static const char *age_vle_empty_lifecycle_direction(
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
        return "both";
    if (out_evidence)
        return "out";
    if (in_evidence)
        return "in";
    return "none";
}

static char *age_vle_empty_lifecycle_action(
    const AgeVLESourceStats *stats, const char *action)
{
    const char *direction;

    Assert(stats != NULL);
    Assert(action != NULL);

    direction = age_vle_empty_lifecycle_direction(stats);
    if (strcmp(direction, "none") == 0)
        return pstrdup(action);
    return psprintf("%s:%s", action, direction);
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

static const char *age_vle_root_empty_completion_direction(
    const AgeVLESourceStats *stats)
{
    Assert(stats != NULL);

    if (stats->root_empty_completion_out > 0 &&
        stats->root_empty_completion_in > 0)
    {
        return "both";
    }
    if (stats->root_empty_completion_out > 0)
        return "out";
    if (stats->root_empty_completion_in > 0)
        return "in";

    return "none";
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
