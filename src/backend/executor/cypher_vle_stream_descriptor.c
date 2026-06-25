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

#include "executor/cypher_vle_stream.h"
#include "nodes/cypher_nodes.h"
#include "nodes/pg_list.h"
#include "nodes/value.h"
#include "utils/age_vle_source_cost.h"
#include "utils/agtype.h"
#include "utils/builtins.h"

static const char *age_vle_stream_arg_name(int argno);
static bool age_vle_stream_const_flag(CustomScan *cscan, int argno);
static bool age_vle_stream_private_bool(List *descriptor, int index);
static int64 age_vle_stream_private_int64(List *descriptor, int index);
static char *age_vle_stream_private_text(List *descriptor, int index);
static Datum age_vle_stream_private_agtype(List *descriptor, int index,
                                           bool *isnull);

void read_age_vle_stream_graph(CustomScan *cscan,
                               AgeVLEStreamGraph *graph)
{
    List *descriptor;

    Assert(cscan != NULL);
    Assert(graph != NULL);

    descriptor = list_nth_node(List, cscan->custom_private,
                               AGE_VLE_STREAM_PRIVATE_GRAPH);
    Assert(list_length(descriptor) == AGE_VLE_STREAM_GRAPH_COUNT);

    graph->graph_known =
        age_vle_stream_private_bool(descriptor,
                                    AGE_VLE_STREAM_GRAPH_KNOWN);
    graph->graph_null =
        age_vle_stream_private_bool(descriptor,
                                    AGE_VLE_STREAM_GRAPH_NULL);
    graph->graph_name =
        age_vle_stream_private_text(descriptor,
                                    AGE_VLE_STREAM_GRAPH_VALUE);
}

void read_age_vle_stream_edge(CustomScan *cscan,
                              AgeVLEStreamEdge *edge)
{
    List *descriptor;

    Assert(cscan != NULL);
    Assert(edge != NULL);

    descriptor = list_nth_node(List, cscan->custom_private,
                               AGE_VLE_STREAM_PRIVATE_EDGE);
    Assert(list_length(descriptor) == AGE_VLE_STREAM_EDGE_COUNT);

    edge->edge_known =
        age_vle_stream_private_bool(descriptor,
                                    AGE_VLE_STREAM_EDGE_KNOWN);
    edge->label_known =
        age_vle_stream_private_bool(descriptor,
                                    AGE_VLE_STREAM_EDGE_LABEL_KNOWN);
    edge->label_name =
        age_vle_stream_private_text(descriptor,
                                    AGE_VLE_STREAM_EDGE_LABEL_VALUE);
    edge->properties_known =
        age_vle_stream_private_bool(descriptor,
                                    AGE_VLE_STREAM_EDGE_PROPERTIES_KNOWN);
    edge->properties_value =
        age_vle_stream_private_agtype(
            descriptor, AGE_VLE_STREAM_EDGE_PROPERTIES_VALUE,
            &edge->properties_null);
    edge->properties_count =
        (int)age_vle_stream_private_int64(
            descriptor, AGE_VLE_STREAM_EDGE_PROPERTIES_COUNT);
}

void read_age_vle_stream_range_direction(CustomScan *cscan,
                                         AgeVLEStreamRangeDirection *range)
{
    List *descriptor;

    Assert(cscan != NULL);
    Assert(range != NULL);

    descriptor = list_nth_node(List, cscan->custom_private,
                               AGE_VLE_STREAM_PRIVATE_RANGE_DIRECTION);
    Assert(list_length(descriptor) == AGE_VLE_STREAM_RANGE_DIRECTION_COUNT);

    range->lower_known =
        age_vle_stream_private_bool(descriptor,
                                    AGE_VLE_STREAM_RANGE_LOWER_KNOWN);
    range->lower_null =
        age_vle_stream_private_bool(descriptor,
                                    AGE_VLE_STREAM_RANGE_LOWER_NULL);
    range->lower_value =
        age_vle_stream_private_int64(descriptor,
                                     AGE_VLE_STREAM_RANGE_LOWER_VALUE);
    range->upper_known =
        age_vle_stream_private_bool(descriptor,
                                    AGE_VLE_STREAM_RANGE_UPPER_KNOWN);
    range->upper_null =
        age_vle_stream_private_bool(descriptor,
                                    AGE_VLE_STREAM_RANGE_UPPER_NULL);
    range->upper_value =
        age_vle_stream_private_int64(descriptor,
                                     AGE_VLE_STREAM_RANGE_UPPER_VALUE);
    range->direction_known =
        age_vle_stream_private_bool(descriptor,
                                    AGE_VLE_STREAM_DIRECTION_KNOWN);
    range->direction_null =
        age_vle_stream_private_bool(descriptor,
                                    AGE_VLE_STREAM_DIRECTION_NULL);
    range->direction_value =
        age_vle_stream_private_int64(descriptor,
                                     AGE_VLE_STREAM_DIRECTION_VALUE);
}

void read_age_vle_stream_output(CustomScan *cscan,
                                AgeVLEStreamOutput *output)
{
    List *descriptor;

    Assert(cscan != NULL);
    Assert(output != NULL);

    descriptor = list_nth_node(List, cscan->custom_private,
                               AGE_VLE_STREAM_PRIVATE_OUTPUT);
    Assert(list_length(descriptor) == AGE_VLE_STREAM_OUTPUT_COUNT);

    output->grammar_known =
        age_vle_stream_private_bool(descriptor,
                                    AGE_VLE_STREAM_OUTPUT_GRAMMAR_KNOWN);
    output->grammar_null =
        age_vle_stream_private_bool(descriptor,
                                    AGE_VLE_STREAM_OUTPUT_GRAMMAR_NULL);
    output->grammar_value =
        age_vle_stream_private_int64(descriptor,
                                     AGE_VLE_STREAM_OUTPUT_GRAMMAR_VALUE);
    output->requirement =
        (AgeVLEOutputRequirement)age_vle_stream_private_int64(
            descriptor, AGE_VLE_STREAM_OUTPUT_REQUIREMENT);
    output->terminal_key_known =
        age_vle_stream_private_bool(
            descriptor, AGE_VLE_STREAM_OUTPUT_TERMINAL_KEY_KNOWN);
    output->terminal_key_null =
        age_vle_stream_private_bool(
            descriptor, AGE_VLE_STREAM_OUTPUT_TERMINAL_KEY_NULL);
    output->terminal_key_value =
        age_vle_stream_private_text(
            descriptor, AGE_VLE_STREAM_OUTPUT_TERMINAL_KEY_VALUE);
    output->terminal_key_len =
        (int)age_vle_stream_private_int64(
            descriptor, AGE_VLE_STREAM_OUTPUT_TERMINAL_KEY_LEN);
    output->terminal_key_is_char =
        age_vle_stream_private_bool(
            descriptor, AGE_VLE_STREAM_OUTPUT_TERMINAL_KEY_IS_CHAR);
    output->terminal_label_known =
        age_vle_stream_private_bool(
            descriptor, AGE_VLE_STREAM_OUTPUT_TERMINAL_LABEL_KNOWN);
    output->terminal_label_id =
        (int32)age_vle_stream_private_int64(
            descriptor, AGE_VLE_STREAM_OUTPUT_TERMINAL_LABEL_ID);
    output->terminal_label_mode =
        (AgeVLETerminalLabelMode)age_vle_stream_private_int64(
            descriptor, AGE_VLE_STREAM_OUTPUT_TERMINAL_LABEL_MODE);
    output->materializer_vertex_prefetch =
        age_vle_stream_private_bool(
            descriptor,
            AGE_VLE_STREAM_OUTPUT_MATERIALIZER_VERTEX_PREFETCH);
    output->materializer_prefetch_min_rel_candidates =
        (int)age_vle_stream_private_int64(
            descriptor,
            AGE_VLE_STREAM_OUTPUT_MATERIALIZER_PREFETCH_MIN_REL_CANDIDATES);
}

void read_age_vle_stream_edge_source(CustomScan *cscan,
                                     AgeVLEStreamEdgeSource *source)
{
    List *descriptor;

    Assert(cscan != NULL);
    Assert(source != NULL);

    descriptor = list_nth_node(List, cscan->custom_private,
                               AGE_VLE_STREAM_PRIVATE_EDGE_SOURCE);
    Assert(list_length(descriptor) == AGE_VLE_STREAM_EDGE_SOURCE_COUNT);

    source->kind =
        (AgeVLEStreamEdgeSourceKind)age_vle_stream_private_int64(
            descriptor, AGE_VLE_STREAM_EDGE_SOURCE_KIND);
    source->adjacency_out =
        age_vle_stream_private_bool(
            descriptor, AGE_VLE_STREAM_EDGE_SOURCE_ADJACENCY_OUT);
    source->adjacency_in =
        age_vle_stream_private_bool(
            descriptor, AGE_VLE_STREAM_EDGE_SOURCE_ADJACENCY_IN);
    source->endpoint_start =
        age_vle_stream_private_bool(
            descriptor, AGE_VLE_STREAM_EDGE_SOURCE_ENDPOINT_START);
    source->endpoint_end =
        age_vle_stream_private_bool(
            descriptor, AGE_VLE_STREAM_EDGE_SOURCE_ENDPOINT_END);
    source->local_edge_state =
        age_vle_stream_private_bool(
            descriptor, AGE_VLE_STREAM_EDGE_SOURCE_LOCAL_EDGE_STATE);
    source->edge_label_oid =
        (Oid)age_vle_stream_private_int64(
            descriptor, AGE_VLE_STREAM_EDGE_SOURCE_EDGE_LABEL_OID);
    source->outgoing_kind =
        (AgeVLEStreamDirectedSourceKind)age_vle_stream_private_int64(
            descriptor, AGE_VLE_STREAM_EDGE_SOURCE_OUTGOING_KIND);
    source->incoming_kind =
        (AgeVLEStreamDirectedSourceKind)age_vle_stream_private_int64(
            descriptor, AGE_VLE_STREAM_EDGE_SOURCE_INCOMING_KIND);
    source->relation_tuples =
        age_vle_stream_private_int64(
            descriptor, AGE_VLE_STREAM_EDGE_SOURCE_RELATION_TUPLES);
    source->start_fanout =
        age_vle_stream_private_int64(
            descriptor, AGE_VLE_STREAM_EDGE_SOURCE_START_FANOUT);
    source->end_fanout =
        age_vle_stream_private_int64(
            descriptor, AGE_VLE_STREAM_EDGE_SOURCE_END_FANOUT);
    source->relation_tuples_known =
        age_vle_stream_private_bool(
            descriptor, AGE_VLE_STREAM_EDGE_SOURCE_RELATION_TUPLES_KNOWN);
    source->start_fanout_known =
        age_vle_stream_private_bool(
            descriptor, AGE_VLE_STREAM_EDGE_SOURCE_START_FANOUT_KNOWN);
    source->end_fanout_known =
        age_vle_stream_private_bool(
            descriptor, AGE_VLE_STREAM_EDGE_SOURCE_END_FANOUT_KNOWN);
    source->start_fanout_source_kind =
        (AgeVLEStreamFanoutSourceKind)age_vle_stream_private_int64(
            descriptor,
            AGE_VLE_STREAM_EDGE_SOURCE_START_FANOUT_SOURCE_KIND);
    source->end_fanout_source_kind =
        (AgeVLEStreamFanoutSourceKind)age_vle_stream_private_int64(
            descriptor,
            AGE_VLE_STREAM_EDGE_SOURCE_END_FANOUT_SOURCE_KIND);
    source->start_value_posting_source_kind =
        (VLESourceValuePostingKind)age_vle_stream_private_int64(
            descriptor,
            AGE_VLE_STREAM_EDGE_SOURCE_START_VALUE_POSTING_SOURCE_KIND);
    source->end_value_posting_source_kind =
        (VLESourceValuePostingKind)age_vle_stream_private_int64(
            descriptor,
            AGE_VLE_STREAM_EDGE_SOURCE_END_VALUE_POSTING_SOURCE_KIND);
    source->cost_policy =
        age_vle_stream_private_text(
            descriptor, AGE_VLE_STREAM_EDGE_SOURCE_COST_POLICY);
    source->policy_outgoing_kind =
        (AgeVLEStreamDirectedSourceKind)age_vle_stream_private_int64(
            descriptor, AGE_VLE_STREAM_EDGE_SOURCE_POLICY_OUTGOING_KIND);
    source->policy_incoming_kind =
        (AgeVLEStreamDirectedSourceKind)age_vle_stream_private_int64(
            descriptor, AGE_VLE_STREAM_EDGE_SOURCE_POLICY_INCOMING_KIND);
    source->policy_output_requirement =
        (AgeVLEOutputRequirement)age_vle_stream_private_int64(
            descriptor,
            AGE_VLE_STREAM_EDGE_SOURCE_POLICY_OUTPUT_REQUIREMENT);
    source->policy_consumer_class_id =
        (VLESourceConsumerClass)age_vle_stream_private_int64(
            descriptor,
            AGE_VLE_STREAM_EDGE_SOURCE_POLICY_CONSUMER_CLASS_KIND);
    source->policy_active_direction_id =
        (VLESourceDirectionClass)age_vle_stream_private_int64(
            descriptor,
            AGE_VLE_STREAM_EDGE_SOURCE_POLICY_ACTIVE_DIRECTION_KIND);
    source->policy_fanout_budget =
        age_vle_stream_private_int64(
            descriptor, AGE_VLE_STREAM_EDGE_SOURCE_POLICY_FANOUT_BUDGET);
    source->policy_materialization_weight =
        age_vle_stream_private_int64(
            descriptor,
            AGE_VLE_STREAM_EDGE_SOURCE_POLICY_MATERIALIZATION_WEIGHT);
    source->policy_class_id =
        (VLESourcePolicyClass)age_vle_stream_private_int64(
            descriptor, AGE_VLE_STREAM_EDGE_SOURCE_POLICY_CLASS_KIND);
    source->policy_recommendation_kind =
        (VLESourcePolicyRecommendation)age_vle_stream_private_int64(
            descriptor,
            AGE_VLE_STREAM_EDGE_SOURCE_POLICY_RECOMMENDATION_KIND);
    source->cache_seed_eligible =
        age_vle_stream_private_bool(
            descriptor, AGE_VLE_STREAM_EDGE_SOURCE_CACHE_SEED_ELIGIBLE);
    source->endpoint_headroom_percent =
        age_vle_stream_private_int64(
            descriptor, AGE_VLE_STREAM_EDGE_SOURCE_ENDPOINT_HEADROOM_PERCENT);
    source->empty_lifecycle_eligible =
        age_vle_stream_private_bool(
            descriptor,
            AGE_VLE_STREAM_EDGE_SOURCE_EMPTY_LIFECYCLE_ELIGIBLE);
    source->empty_lifecycle_depth =
        age_vle_stream_private_int64(
            descriptor, AGE_VLE_STREAM_EDGE_SOURCE_EMPTY_LIFECYCLE_DEPTH);
    source->empty_lifecycle_batch_size =
        age_vle_stream_private_int64(
            descriptor,
            AGE_VLE_STREAM_EDGE_SOURCE_EMPTY_LIFECYCLE_BATCH_SIZE);
    source->threshold_input_known =
        age_vle_stream_private_bool(
            descriptor, AGE_VLE_STREAM_EDGE_SOURCE_THRESHOLD_INPUT_KNOWN);
    source->threshold_input_headroom_percent =
        age_vle_stream_private_int64(
            descriptor,
            AGE_VLE_STREAM_EDGE_SOURCE_THRESHOLD_INPUT_HEADROOM_PERCENT);
    source->threshold_input_batch_size =
        age_vle_stream_private_int64(
            descriptor,
            AGE_VLE_STREAM_EDGE_SOURCE_THRESHOLD_INPUT_BATCH_SIZE);
    source->threshold_input_observed_count =
        age_vle_stream_private_int64(
            descriptor,
            AGE_VLE_STREAM_EDGE_SOURCE_THRESHOLD_INPUT_OBSERVED_COUNT);
    source->threshold_input_saturated_count =
        age_vle_stream_private_int64(
            descriptor,
            AGE_VLE_STREAM_EDGE_SOURCE_THRESHOLD_INPUT_SATURATED_COUNT);
    source->threshold_input_relaxed_count =
        age_vle_stream_private_int64(
            descriptor,
            AGE_VLE_STREAM_EDGE_SOURCE_THRESHOLD_INPUT_RELAXED_COUNT);
    source->threshold_input_direction_id =
        (VLESourceDirectionClass)age_vle_stream_private_int64(
            descriptor,
            AGE_VLE_STREAM_EDGE_SOURCE_THRESHOLD_INPUT_DIRECTION_KIND);
    source->threshold_input_reason_id =
        (VLESourceThresholdFeedbackReason)age_vle_stream_private_int64(
            descriptor,
            AGE_VLE_STREAM_EDGE_SOURCE_THRESHOLD_INPUT_REASON_KIND);
    source->threshold_input_class_id =
        (VLESourcePolicyClass)age_vle_stream_private_int64(
            descriptor,
            AGE_VLE_STREAM_EDGE_SOURCE_THRESHOLD_INPUT_CLASS_KIND);
    source->payload_input_known =
        age_vle_stream_private_bool(
            descriptor, AGE_VLE_STREAM_EDGE_SOURCE_PAYLOAD_INPUT_KNOWN);
    source->payload_input_headroom_percent =
        age_vle_stream_private_int64(
            descriptor,
            AGE_VLE_STREAM_EDGE_SOURCE_PAYLOAD_INPUT_HEADROOM_PERCENT);
    source->payload_input_scan_runs =
        age_vle_stream_private_int64(
            descriptor, AGE_VLE_STREAM_EDGE_SOURCE_PAYLOAD_INPUT_SCAN_RUNS);
    source->payload_input_replay_runs =
        age_vle_stream_private_int64(
            descriptor, AGE_VLE_STREAM_EDGE_SOURCE_PAYLOAD_INPUT_REPLAY_RUNS);
    source->payload_input_seed_runs =
        age_vle_stream_private_int64(
            descriptor, AGE_VLE_STREAM_EDGE_SOURCE_PAYLOAD_INPUT_SEED_RUNS);
    source->payload_input_replay_percent =
        age_vle_stream_private_int64(
            descriptor,
            AGE_VLE_STREAM_EDGE_SOURCE_PAYLOAD_INPUT_REPLAY_PERCENT);
    source->payload_input_seed_percent =
        age_vle_stream_private_int64(
            descriptor,
            AGE_VLE_STREAM_EDGE_SOURCE_PAYLOAD_INPUT_SEED_PERCENT);
    source->payload_input_observed_count =
        age_vle_stream_private_int64(
            descriptor,
            AGE_VLE_STREAM_EDGE_SOURCE_PAYLOAD_INPUT_OBSERVED_COUNT);
    source->payload_input_value_posting_observed_count =
        age_vle_stream_private_int64(
            descriptor,
            AGE_VLE_STREAM_EDGE_SOURCE_PAYLOAD_INPUT_VALUE_POSTING_OBSERVED_COUNT);
    source->payload_input_matrix_active_percent =
        age_vle_stream_private_int64(
            descriptor,
            AGE_VLE_STREAM_EDGE_SOURCE_PAYLOAD_INPUT_MATRIX_ACTIVE_PERCENT);
    source->payload_input_matrix_filter_percent =
        age_vle_stream_private_int64(
            descriptor,
            AGE_VLE_STREAM_EDGE_SOURCE_PAYLOAD_INPUT_MATRIX_FILTER_PERCENT);
    source->payload_input_matrix_prescan_percent =
        age_vle_stream_private_int64(
            descriptor,
            AGE_VLE_STREAM_EDGE_SOURCE_PAYLOAD_INPUT_MATRIX_PRESCAN_PERCENT);
    source->payload_input_matrix_replay_source_percent =
        age_vle_stream_private_int64(
            descriptor,
            AGE_VLE_STREAM_EDGE_SOURCE_PAYLOAD_INPUT_MATRIX_REPLAY_SOURCE_PERCENT);
    source->payload_input_matrix_run_block_percent =
        age_vle_stream_private_int64(
            descriptor,
            AGE_VLE_STREAM_EDGE_SOURCE_PAYLOAD_INPUT_MATRIX_RUN_BLOCK_PERCENT);
    source->payload_input_matrix_regroup_percent =
        age_vle_stream_private_int64(
            descriptor,
            AGE_VLE_STREAM_EDGE_SOURCE_PAYLOAD_INPUT_MATRIX_REGROUP_PERCENT);
    source->payload_input_reason_id =
        (VLESourcePayloadFeedbackReason)age_vle_stream_private_int64(
            descriptor,
            AGE_VLE_STREAM_EDGE_SOURCE_PAYLOAD_INPUT_REASON_KIND);
    source->payload_input_class_id =
        (VLESourcePolicyClass)age_vle_stream_private_int64(
            descriptor,
            AGE_VLE_STREAM_EDGE_SOURCE_PAYLOAD_INPUT_CLASS_KIND);
    source->payload_input_value_posting_source_kind =
        (VLESourceValuePostingKind)age_vle_stream_private_int64(
            descriptor,
            AGE_VLE_STREAM_EDGE_SOURCE_PAYLOAD_INPUT_VALUE_POSTING_SOURCE_KIND);
    source->terminal_property_source_known =
        age_vle_stream_private_bool(
            descriptor,
            AGE_VLE_STREAM_EDGE_SOURCE_TERMINAL_PROPERTY_SOURCE_KNOWN);
    source->terminal_label_id =
        (int32)age_vle_stream_private_int64(
            descriptor, AGE_VLE_STREAM_EDGE_SOURCE_TERMINAL_LABEL_ID);
    source->terminal_property_index_oid =
        (Oid)age_vle_stream_private_int64(
            descriptor,
            AGE_VLE_STREAM_EDGE_SOURCE_TERMINAL_PROPERTY_INDEX_OID);
    source->terminal_property_filter_id =
        (uint32)age_vle_stream_private_int64(
            descriptor,
            AGE_VLE_STREAM_EDGE_SOURCE_TERMINAL_PROPERTY_FILTER_ID);
    source->terminal_property_label =
        age_vle_stream_private_text(
            descriptor,
            AGE_VLE_STREAM_EDGE_SOURCE_TERMINAL_PROPERTY_LABEL);
    source->terminal_property_source =
        age_vle_stream_private_text(
            descriptor,
            AGE_VLE_STREAM_EDGE_SOURCE_TERMINAL_PROPERTY_SOURCE);
    source->terminal_property_provider =
        age_vle_stream_private_text(
            descriptor,
            AGE_VLE_STREAM_EDGE_SOURCE_TERMINAL_PROPERTY_PROVIDER);
    source->terminal_property_type =
        age_vle_stream_private_text(
            descriptor,
            AGE_VLE_STREAM_EDGE_SOURCE_TERMINAL_PROPERTY_TYPE);
    source->terminal_property_match_count =
        age_vle_stream_private_int64(
            descriptor,
            AGE_VLE_STREAM_EDGE_SOURCE_TERMINAL_PROPERTY_MATCH_COUNT);
    source->composite_source_known =
        age_vle_stream_private_bool(
            descriptor,
            AGE_VLE_STREAM_EDGE_SOURCE_COMPOSITE_SOURCE_KNOWN);
    source->composite_source_reason_kind =
        (AgeVLEStreamCompositeSourceReason)age_vle_stream_private_int64(
            descriptor,
            AGE_VLE_STREAM_EDGE_SOURCE_COMPOSITE_SOURCE_REASON_KIND);
    source->composite_source_property_tuples =
        age_vle_stream_private_int64(
            descriptor,
            AGE_VLE_STREAM_EDGE_SOURCE_COMPOSITE_SOURCE_PROPERTY_TUPLES);
    source->composite_source_candidate_fanout =
        age_vle_stream_private_int64(
            descriptor,
            AGE_VLE_STREAM_EDGE_SOURCE_COMPOSITE_SOURCE_CANDIDATE_FANOUT);
    source->composite_source_fanout =
        age_vle_stream_private_int64(
            descriptor,
            AGE_VLE_STREAM_EDGE_SOURCE_COMPOSITE_SOURCE_FANOUT);
    source->composite_source_selectivity_ppm =
        age_vle_stream_private_int64(
            descriptor,
            AGE_VLE_STREAM_EDGE_SOURCE_COMPOSITE_SOURCE_SELECTIVITY_PPM);
    source->composite_source_selectivity_source_kind =
        (AgeGraphPropertySelectivitySource)age_vle_stream_private_int64(
            descriptor,
            AGE_VLE_STREAM_EDGE_SOURCE_COMPOSITE_SOURCE_SELECTIVITY_SOURCE_KIND);
    source->composite_source_planned_kind =
        (AgeVLEStreamCompositePlanKind)age_vle_stream_private_int64(
            descriptor,
            AGE_VLE_STREAM_EDGE_SOURCE_COMPOSITE_SOURCE_PLANNED_KIND);
    source->terminal_property_predicate_known =
        age_vle_stream_private_bool(
            descriptor,
            AGE_VLE_STREAM_EDGE_SOURCE_TERMINAL_PROPERTY_PREDICATE_KNOWN);
    source->terminal_property_predicate_key =
        age_vle_stream_private_text(
            descriptor,
            AGE_VLE_STREAM_EDGE_SOURCE_TERMINAL_PROPERTY_PREDICATE_KEY);
    source->terminal_property_predicate_null =
        age_vle_stream_private_bool(
            descriptor,
            AGE_VLE_STREAM_EDGE_SOURCE_TERMINAL_PROPERTY_PREDICATE_NULL);
    source->terminal_property_predicate_value =
        list_nth_node(Const, descriptor,
                      AGE_VLE_STREAM_EDGE_SOURCE_TERMINAL_PROPERTY_PREDICATE_VALUE)->constvalue;
    source->terminal_property_value_kind_id =
        (AgeVLEStreamTerminalValueKind)age_vle_stream_private_int64(
            descriptor,
            AGE_VLE_STREAM_EDGE_SOURCE_TERMINAL_PROPERTY_VALUE_KIND_ID);
    source->terminal_property_prefilter_eligible =
        age_vle_stream_private_bool(
            descriptor,
            AGE_VLE_STREAM_EDGE_SOURCE_TERMINAL_PROPERTY_PREFILTER_ELIGIBLE);
    source->terminal_property_prefetch_threshold =
        age_vle_stream_private_int64(
            descriptor,
            AGE_VLE_STREAM_EDGE_SOURCE_TERMINAL_PROPERTY_PREFETCH_THRESHOLD);
}

const char *age_vle_stream_fanout_source_name(
    AgeVLEStreamFanoutSourceKind source)
{
    switch (source)
    {
        case AGE_VLE_STREAM_FANOUT_SOURCE_STATISTICS:
            return "statistics";
        case AGE_VLE_STREAM_FANOUT_SOURCE_DIRECTORY:
            return "directory";
        case AGE_VLE_STREAM_FANOUT_SOURCE_DIRECTORY_LABEL:
            return "directory-label";
        case AGE_VLE_STREAM_FANOUT_SOURCE_UNKNOWN:
            break;
    }

    return "unknown";
}

bool age_vle_stream_fanout_source_is_directory_label(
    AgeVLEStreamFanoutSourceKind source)
{
    return source == AGE_VLE_STREAM_FANOUT_SOURCE_DIRECTORY_LABEL;
}

const char *age_vle_stream_composite_plan_name(
    AgeVLEStreamCompositePlanKind plan)
{
    switch (plan)
    {
        case AGE_VLE_STREAM_COMPOSITE_PLAN_NONE:
            return "none";
        case AGE_VLE_STREAM_COMPOSITE_PLAN_PROPERTY_PREFILTER:
            return "property-prefilter";
        case AGE_VLE_STREAM_COMPOSITE_PLAN_BELOW_THRESHOLD:
            return "below-threshold";
        case AGE_VLE_STREAM_COMPOSITE_PLAN_METADATA_ONLY:
            return "metadata-only";
        case AGE_VLE_STREAM_COMPOSITE_PLAN_UNKNOWN:
            break;
    }

    return "unknown";
}

bool age_vle_stream_composite_plan_is_property_prefilter(
    AgeVLEStreamCompositePlanKind plan)
{
    return plan == AGE_VLE_STREAM_COMPOSITE_PLAN_PROPERTY_PREFILTER;
}

const char *age_vle_stream_terminal_value_kind_name(
    AgeVLEStreamTerminalValueKind kind)
{
    switch (kind)
    {
        case AGE_VLE_STREAM_TERMINAL_VALUE_CONST:
            return "const";
        case AGE_VLE_STREAM_TERMINAL_VALUE_NULL:
            return "null";
        case AGE_VLE_STREAM_TERMINAL_VALUE_RUNTIME_SLOT:
            return "runtime-slot";
        case AGE_VLE_STREAM_TERMINAL_VALUE_NONE:
            break;
    }

    return "none";
}

const char *age_vle_stream_shape_name(AgeVLEStreamOutput *output, int nargs)
{
    if (output != NULL)
    {
        if (output->requirement ==
            AGE_VLE_OUTPUT_REQUIREMENT_TERMINAL_PROPERTY)
            return "terminal-property";
        if (output->requirement ==
            AGE_VLE_OUTPUT_REQUIREMENT_TERMINAL_VERTEX)
            return "terminal-vertex";
        if (output->requirement ==
            AGE_VLE_OUTPUT_REQUIREMENT_TERMINAL_PROPERTIES)
            return "terminal-properties";
    }

    if (nargs == AGE_VLE_STREAM_ARG_GRAMMAR_NODE + 1)
        return "cypher";
    if (nargs == AGE_VLE_STREAM_ARG_TERMINAL_PROPERTY + 1)
    {
        if (output != NULL && output->terminal_label_known)
            return "cypher";
        return "terminal-property";
    }
    if (nargs == AGE_VLE_STREAM_ARG_TERMINAL_LABEL + 1)
        return "terminal-property";

    return "unknown";
}

List *make_age_vle_stream_slot_descriptions(CustomScan *cscan, int nargs)
{
    List *descriptions = NIL;
    int argno;

    for (argno = 0; argno < nargs; argno++)
    {
        descriptions = lappend(descriptions,
                               psprintf("%s=%s",
                                        age_vle_stream_arg_name(argno),
                                        age_vle_stream_const_flag(cscan, argno) ?
                                        "const" : "dynamic"));
    }

    return descriptions;
}

char *format_age_vle_stream_graph(AgeVLEStreamGraph *graph)
{
    if (!graph->graph_known)
        return pstrdup("dynamic");

    if (graph->graph_null)
        return pstrdup("null");

    return pstrdup(graph->graph_name == NULL ? "" : graph->graph_name);
}

char *format_age_vle_stream_edge(AgeVLEStreamEdge *edge)
{
    const char *label_text;
    const char *properties_text;

    if (!edge->edge_known)
        return pstrdup("dynamic");

    label_text = edge->label_known ?
        (edge->label_name == NULL ? "" : edge->label_name) :
        "dynamic";
    properties_text = edge->properties_known ?
        psprintf("%d", edge->properties_count) :
        "dynamic";

    return psprintf("label=%s, properties=%s", label_text, properties_text);
}

char *format_age_vle_stream_endpoints(CustomScan *cscan)
{
    const char *start_text;
    const char *end_text;

    start_text = age_vle_stream_const_flag(cscan, AGE_VLE_STREAM_ARG_START) ?
        "const-id" : "runtime-id";
    end_text = age_vle_stream_const_flag(cscan, AGE_VLE_STREAM_ARG_END) ?
        "const-id" : "runtime-id";

    return psprintf("start=%s, end=%s", start_text, end_text);
}

char *format_age_vle_stream_range(AgeVLEStreamRangeDirection *range)
{
    const char *lower_text;
    const char *upper_text;

    lower_text = range->lower_known ?
        (range->lower_null ? "default(1)" :
         psprintf("%lld", (long long)range->lower_value)) :
        "dynamic";
    upper_text = range->upper_known ?
        (range->upper_null ? "unbounded" :
         psprintf("%lld", (long long)range->upper_value)) :
        "dynamic";

    return psprintf("%s..%s", lower_text, upper_text);
}

const char *format_age_vle_stream_direction(AgeVLEStreamRangeDirection *range)
{
    if (!range->direction_known)
        return "dynamic";

    if (range->direction_null)
        return "null";

    switch ((cypher_rel_dir)range->direction_value)
    {
        case CYPHER_REL_DIR_NONE:
            return "any";
        case CYPHER_REL_DIR_LEFT:
            return "left";
        case CYPHER_REL_DIR_RIGHT:
            return "right";
    }

    return psprintf("unknown(%lld)", (long long)range->direction_value);
}

char *format_age_vle_stream_output(AgeVLEStreamOutput *output, int nargs)
{
    const char *grammar_text;
    const char *terminal_label_text;

    if (!output->grammar_known)
        grammar_text = "dynamic";
    else if (output->grammar_null)
        grammar_text = "null";
    else if (output->grammar_value < 0)
        grammar_text = "terminal-only";
    else
        grammar_text = "cached";
    terminal_label_text = output->terminal_label_known ?
        psprintf(", terminal-label=%d/%s", output->terminal_label_id,
                 output->terminal_label_mode ==
                 AGE_VLE_TERMINAL_LABEL_ENDPOINT_ONLY ?
                 "endpoint" : "all-depth") : "";

    if (output->requirement == AGE_VLE_OUTPUT_REQUIREMENT_TERMINAL_PROPERTY)
    {
        const char *key_text;

        key_text = output->terminal_key_known ?
            (output->terminal_key_null ? "null" :
             (output->terminal_key_value == NULL ? "" :
              output->terminal_key_value)) :
            "dynamic";

        return psprintf("terminal-property(requirement=%s, grammar=%s, "
                        "key=%s, len=%d, char-fast=%s%s)",
                        age_vle_output_requirement_name(output->requirement),
                        grammar_text, key_text, output->terminal_key_len,
                        output->terminal_key_is_char ? "true" : "false",
                        terminal_label_text);
    }
    if (output->requirement == AGE_VLE_OUTPUT_REQUIREMENT_TERMINAL_VERTEX)
    {
        return psprintf("terminal-vertex(requirement=%s, grammar=%s%s)",
                        age_vle_output_requirement_name(output->requirement),
                        grammar_text, terminal_label_text);
    }
    if (output->requirement ==
        AGE_VLE_OUTPUT_REQUIREMENT_TERMINAL_PROPERTIES)
    {
        return psprintf("terminal-properties(requirement=%s, grammar=%s%s)",
                        age_vle_output_requirement_name(output->requirement),
                        grammar_text, terminal_label_text);
    }

    return psprintf("path(requirement=%s, grammar=%s%s)",
                    age_vle_output_requirement_name(output->requirement),
                    grammar_text, terminal_label_text);
}

const char *format_age_vle_stream_materialization(
    AgeVLEStreamOutput *output, AgeVLEStreamEdgeSource *source)
{
    const char *object_source;

    if (output == NULL)
        return "unknown";

    object_source = source != NULL && source->local_edge_state ?
        "label-row-fallback" : "global-metadata";

    switch (output->requirement)
    {
        case AGE_VLE_OUTPUT_REQUIREMENT_PATH:
            if (output->materializer_vertex_prefetch)
            {
                return psprintf("path-container, vertex-prefetch=label-batch"
                                "(min-rel-candidates=%d), "
                                "object-source=%s",
                                output->materializer_prefetch_min_rel_candidates,
                                object_source);
            }
            return psprintf("path-container, object-source=%s",
                            object_source);
        case AGE_VLE_OUTPUT_REQUIREMENT_TERMINAL_VERTEX:
            return psprintf("terminal-vertex-container, object-source=%s",
                            object_source);
        case AGE_VLE_OUTPUT_REQUIREMENT_TERMINAL_PROPERTIES:
            return psprintf("terminal-properties-direct, vertex-source=%s",
                            object_source);
        case AGE_VLE_OUTPUT_REQUIREMENT_TERMINAL_PROPERTY:
            return "terminal-property-direct";
        case AGE_VLE_OUTPUT_REQUIREMENT_UNKNOWN:
            return "unknown";
    }

    return "unknown";
}

const char *format_age_vle_stream_terminal_slot(CustomScan *cscan,
                                                AgeVLEStreamOutput *output,
                                                int nargs)
{
    const char *slot_kind;
    const char *slot_source;

    if (nargs <= AGE_VLE_STREAM_ARG_TERMINAL_PROPERTY)
        return "absent";

    slot_kind = "property-key";
    if (output != NULL &&
        output->requirement == AGE_VLE_OUTPUT_REQUIREMENT_TERMINAL_PROPERTIES)
    {
        slot_kind = "full-properties";
    }

    slot_source = age_vle_stream_const_flag(
        cscan, AGE_VLE_STREAM_ARG_TERMINAL_PROPERTY) ?
        "const" : "dynamic";

    return psprintf("%s, source=%s", slot_kind, slot_source);
}

static const char *age_vle_stream_arg_name(int argno)
{
    switch (argno)
    {
        case AGE_VLE_STREAM_ARG_GRAPH:
            return "graph";
        case AGE_VLE_STREAM_ARG_START:
            return "start";
        case AGE_VLE_STREAM_ARG_END:
            return "end";
        case AGE_VLE_STREAM_ARG_EDGE:
            return "edge";
        case AGE_VLE_STREAM_ARG_LOWER:
            return "lower";
        case AGE_VLE_STREAM_ARG_UPPER:
            return "upper";
        case AGE_VLE_STREAM_ARG_DIRECTION:
            return "direction";
        case AGE_VLE_STREAM_ARG_GRAMMAR_NODE:
            return "grammar-node";
        case AGE_VLE_STREAM_ARG_TERMINAL_PROPERTY:
            return "terminal-property";
        case AGE_VLE_STREAM_ARG_TERMINAL_LABEL:
            return "terminal-label";
    }

    return "unknown";
}

static bool age_vle_stream_const_flag(CustomScan *cscan, int argno)
{
    List *const_flags = list_nth_node(List, cscan->custom_private,
                                      AGE_VLE_STREAM_PRIVATE_CONST_FLAGS);
    Integer *const_flag;

    if (argno < 0 || argno >= list_length(const_flags))
        return false;

    const_flag = list_nth_node(Integer, const_flags, argno);

    return intVal(const_flag) != 0;
}

static bool age_vle_stream_private_bool(List *descriptor, int index)
{
    Integer *value = list_nth_node(Integer, descriptor, index);

    return intVal(value) != 0;
}

static int64 age_vle_stream_private_int64(List *descriptor, int index)
{
    Const *value = list_nth_node(Const, descriptor, index);

    Assert(value->consttype == INT8OID);
    Assert(!value->constisnull);

    return DatumGetInt64(value->constvalue);
}

static char *age_vle_stream_private_text(List *descriptor, int index)
{
    Const *value = list_nth_node(Const, descriptor, index);

    Assert(value->consttype == TEXTOID);

    if (value->constisnull)
        return NULL;

    return TextDatumGetCString(value->constvalue);
}

static Datum age_vle_stream_private_agtype(List *descriptor, int index,
                                           bool *isnull)
{
    Const *value = list_nth_node(Const, descriptor, index);

    Assert(value->consttype == AGTYPEOID);
    Assert(isnull != NULL);

    *isnull = value->constisnull;
    if (value->constisnull)
        return (Datum)0;

    return value->constvalue;
}
