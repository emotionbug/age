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
    double reltuples;
    double start_fanout;
    double end_fanout;
} VLESourceFanoutEvidence;

typedef struct VLEStreamSourceCostInput
{
    AgeVLEStreamEdgeSourceKind source_kind;
    AgeVLEStreamDirectedSourceKind outgoing_kind;
    AgeVLEStreamDirectedSourceKind incoming_kind;
    const VLESourceFanoutEvidence *evidence;
    int64 upper;
    bool upper_infinite;
    bool has_property_constraints;
    bool endpoint_start;
    bool endpoint_end;
    bool age_adjacency_out;
    bool age_adjacency_in;
} VLEStreamSourceCostInput;

typedef struct VLEStreamSourceCostDecision
{
    AgeVLEStreamDirectedSourceKind outgoing_kind;
    AgeVLEStreamDirectedSourceKind incoming_kind;
    char *policy_text;
} VLEStreamSourceCostDecision;

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
extern double estimate_vle_edge_endpoint_fanout(
    Oid edge_label_oid, AttrNumber endpoint_attno, double reltuples);
extern double get_vle_relation_estimated_tuples(Oid relation_oid);
extern char *format_vle_stream_edge_source_evidence(
    AgeVLEStreamEdgeSource *source);
extern char *format_vle_source_runtime_evidence(
    const AgeVLESourceStats *stats);

#endif
