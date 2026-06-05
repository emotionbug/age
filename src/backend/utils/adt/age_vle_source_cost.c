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
#include "utils/syscache.h"

typedef struct VLESourceRuntimeDominant
{
    const char *name;
    int64 scans;
    int64 candidates;
} VLESourceRuntimeDominant;

typedef struct VLESourcePolicyDecision
{
    AgeVLEStreamDirectedSourceKind kind;
    double endpoint_work;
    double limit_work;
    const char *reason;
} VLESourcePolicyDecision;

#define VLE_ENDPOINT_BTREE_POLICY_FANOUT 2.0

static const char *age_vle_stream_source_kind_name(
    AgeVLEStreamEdgeSourceKind kind);
static const char *age_vle_directed_source_kind_name(
    AgeVLEStreamDirectedSourceKind kind);
static void choose_vle_source_policy_decision(
    VLESourcePolicyDecision *decision,
    AgeVLEStreamDirectedSourceKind current_kind, double fanout,
    bool endpoint_available, bool age_adjacency_available,
    int64 depth, bool cost_eligible);
static double estimate_vle_source_policy_work(double fanout, int64 depth);
static char *format_vle_stream_source_cost_policy(
    const VLEStreamSourceCostDecision *decision,
    const VLESourcePolicyDecision *out_policy,
    const VLESourcePolicyDecision *in_policy,
    bool cost_eligible);
static double calculate_vle_source_scan_density(int64 candidates,
                                                int64 scans);
static void choose_vle_source_runtime_dominant(
    const AgeVLESourceStats *stats, VLESourceRuntimeDominant *dominant);

void estimate_vle_source_fanout_evidence(
    VLESourceFanoutEvidence *evidence, Oid edge_label_oid)
{
    Assert(evidence != NULL);

    evidence->edge_label_oid = edge_label_oid;
    evidence->reltuples = 0.0;
    evidence->start_fanout = 0.0;
    evidence->end_fanout = 0.0;

    if (!OidIsValid(edge_label_oid))
        return;

    evidence->reltuples = get_vle_relation_estimated_tuples(edge_label_oid);
    if (evidence->reltuples <= 0)
        return;

    evidence->start_fanout = estimate_vle_edge_endpoint_fanout(
        edge_label_oid, Anum_ag_label_edge_table_start_id,
        evidence->reltuples);
    evidence->end_fanout = estimate_vle_edge_endpoint_fanout(
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
    bool cost_eligible;
    int64 depth;
    VLESourcePolicyDecision out_policy;
    VLESourcePolicyDecision in_policy;

    Assert(decision != NULL);
    Assert(input != NULL);
    Assert(input->evidence != NULL);

    decision->outgoing_kind = input->outgoing_kind;
    decision->incoming_kind = input->incoming_kind;
    decision->policy_text = NULL;

    if (input->source_kind != AGE_VLE_STREAM_EDGE_SOURCE_LOCAL_INDEX_CANDIDATE)
        return;

    cost_eligible = !input->upper_infinite &&
        !input->has_property_constraints;
    depth = input->upper_infinite ? 0 : Max(input->upper, (int64)1);

    choose_vle_source_policy_decision(
        &out_policy,
        input->outgoing_kind, input->evidence->start_fanout,
        input->endpoint_start, input->age_adjacency_out, depth,
        cost_eligible);
    choose_vle_source_policy_decision(
        &in_policy,
        input->incoming_kind, input->evidence->end_fanout,
        input->endpoint_end, input->age_adjacency_in, depth, cost_eligible);

    decision->outgoing_kind = out_policy.kind;
    decision->incoming_kind = in_policy.kind;
    decision->policy_text = format_vle_stream_source_cost_policy(
        decision, &out_policy, &in_policy, cost_eligible);
}

double estimate_vle_edge_endpoint_fanout(
    Oid edge_label_oid, AttrNumber endpoint_attno, double reltuples)
{
    HeapTuple stat_tuple;
    Form_pg_statistic stats;
    double distinct;

    if (!OidIsValid(edge_label_oid) || endpoint_attno <= 0 || reltuples <= 0)
        return 0.0;

    stat_tuple = SearchSysCache3(STATRELATTINH,
                                 ObjectIdGetDatum(edge_label_oid),
                                 Int16GetDatum(endpoint_attno),
                                 BoolGetDatum(false));
    if (!HeapTupleIsValid(stat_tuple))
        return 0.0;

    stats = (Form_pg_statistic) GETSTRUCT(stat_tuple);
    if (stats->stadistinct > 0)
        distinct = stats->stadistinct;
    else if (stats->stadistinct < 0)
        distinct = -stats->stadistinct * reltuples;
    else
        distinct = 0.0;

    ReleaseSysCache(stat_tuple);

    if (distinct <= 0)
        return 0.0;

    distinct = Max(distinct, 1.0);
    distinct = Min(distinct, reltuples);

    return Max(reltuples / distinct, 1.0);
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
    char *source_text;

    if (source == NULL)
        return pstrdup("unknown");

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
        return psprintf("%s, cost=reltuples=%lld fanout=start:%lld/end:%lld",
                        source_text,
                        (long long)source->relation_tuples,
                        (long long)source->start_fanout,
                        (long long)source->end_fanout);
    }

    return psprintf("%s, cost=reltuples=%lld fanout=start:%lld/end:%lld "
                    "policy=%s",
                    source_text,
                    (long long)source->relation_tuples,
                    (long long)source->start_fanout,
                    (long long)source->end_fanout,
                    source->cost_policy);
}

char *format_vle_source_runtime_evidence(const AgeVLESourceStats *stats)
{
    VLESourceRuntimeDominant dominant;
    double age_adjacency_density;
    double endpoint_btree_density;
    double packed_density;

    Assert(stats != NULL);

    choose_vle_source_runtime_dominant(stats, &dominant);
    age_adjacency_density = calculate_vle_source_scan_density(
        stats->age_adjacency_candidates, stats->age_adjacency_scans);
    endpoint_btree_density = calculate_vle_source_scan_density(
        stats->endpoint_btree_candidates, stats->endpoint_btree_scans);
    packed_density = calculate_vle_source_scan_density(
        stats->packed_candidates, stats->packed_scans);

    return psprintf("sources=age_adjacency=%lld/%lld/%lld/%lld/%lld "
                    "endpoint-btree=%lld/%lld packed=%lld/%lld/%lld/%lld, "
                    "packed-suppressed=out:%lld/in:%lld/self:%lld, "
                    "missing-vertex=%lld/%lld, candidates=%lld/%lld, "
                    "feedback=dominant=%s density=age_adjacency:%.2f,"
                    "endpoint-btree:%.2f,packed:%.2f yield=%lld/%lld "
                    "replay=%lld/%lld push=%lld/%lld",
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
                    (long long)stats->packed_suppress_out,
                    (long long)stats->packed_suppress_in,
                    (long long)stats->packed_suppress_self,
                    (long long)stats->missing_vertex_source_hits,
                    (long long)stats->missing_vertex_attempts,
                    (long long)stats->candidates_pushed,
                    (long long)stats->candidates_yielded,
                    dominant.name,
                    age_adjacency_density,
                    endpoint_btree_density,
                    packed_density,
                    (long long)dominant.candidates,
                    (long long)dominant.scans,
                    (long long)stats->age_adjacency_payload_replays,
                    (long long)stats->age_adjacency_payload_scans,
                    (long long)stats->candidates_pushed,
                    (long long)stats->candidates_yielded);
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
    bool endpoint_available, bool age_adjacency_available,
    int64 depth, bool cost_eligible)
{
    Assert(decision != NULL);

    decision->kind = current_kind;
    decision->endpoint_work = estimate_vle_source_policy_work(fanout, depth);
    decision->limit_work = estimate_vle_source_policy_work(
        VLE_ENDPOINT_BTREE_POLICY_FANOUT, depth);
    decision->reason = "layout";

    if (!endpoint_available && !age_adjacency_available)
    {
        decision->kind = AGE_VLE_STREAM_DIRECTED_SOURCE_NONE;
        decision->reason = "no-source";
        return;
    }
    if (!cost_eligible)
        return;
    if (endpoint_available &&
        fanout > 0 &&
        decision->endpoint_work <= decision->limit_work)
    {
        decision->kind = AGE_VLE_STREAM_DIRECTED_SOURCE_ENDPOINT_BTREE;
        decision->reason = "endpoint-work";
        return;
    }
    if (age_adjacency_available)
    {
        decision->kind = AGE_VLE_STREAM_DIRECTED_SOURCE_AGE_ADJACENCY;
        decision->reason = "work-exceeds-limit";
        return;
    }
    if (endpoint_available)
    {
        decision->kind = AGE_VLE_STREAM_DIRECTED_SOURCE_ENDPOINT_BTREE;
        decision->reason = fanout > 0 ? "endpoint-only" : "unknown-fanout";
        return;
    }

    decision->kind = AGE_VLE_STREAM_DIRECTED_SOURCE_NONE;
    decision->reason = "no-source";
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

static char *format_vle_stream_source_cost_policy(
    const VLEStreamSourceCostDecision *decision,
    const VLESourcePolicyDecision *out_policy,
    const VLESourcePolicyDecision *in_policy,
    bool cost_eligible)
{
    Assert(decision != NULL);
    Assert(out_policy != NULL);
    Assert(in_policy != NULL);

    return psprintf("out=%s/in=%s depth=%s endpoint-work=sum(out:%.0f/%.0f,"
                    "in:%.0f/%.0f) reason=out:%s/in:%s",
                    age_vle_directed_source_kind_name(
                        decision->outgoing_kind),
                    age_vle_directed_source_kind_name(
                        decision->incoming_kind),
                    cost_eligible ? "costed" : "layout",
                    out_policy->endpoint_work, out_policy->limit_work,
                    in_policy->endpoint_work, in_policy->limit_work,
                    out_policy->reason, in_policy->reason);
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
