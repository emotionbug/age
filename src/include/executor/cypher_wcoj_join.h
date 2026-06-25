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

#ifndef AG_CYPHER_WCOJ_JOIN_H
#define AG_CYPHER_WCOJ_JOIN_H

#include "nodes/extensible.h"

#define AGE_WCOJ_JOIN_SCAN_NAME "AGE WCOJ Multiway Join"

typedef enum AgeWCOJJoinPrivateField
{
    AGE_WCOJ_JOIN_PRIVATE_ARITY = 0,
    AGE_WCOJ_JOIN_PRIVATE_KEY_ATTNOS,
    AGE_WCOJ_JOIN_PRIVATE_REQUESTED_ENGINE,
    AGE_WCOJ_JOIN_PRIVATE_PLANNED_ENGINE,
    AGE_WCOJ_JOIN_PRIVATE_ESTIMATED_POSTINGS,
    AGE_WCOJ_JOIN_PRIVATE_PROVIDER_DESCS,
    AGE_WCOJ_JOIN_PRIVATE_UNIQUENESS_GROUPS,
    AGE_WCOJ_JOIN_PRIVATE_COUNT
} AgeWCOJJoinPrivateField;

typedef enum AgeWCOJProviderKind
{
    AGE_WCOJ_PROVIDER_PLAN_STREAM = 0,
    AGE_WCOJ_PROVIDER_ADJACENCY
} AgeWCOJProviderKind;

/*
 * Executor-safe, node-serializable provider descriptor.  OIDs are Const
 * nodes and OUTPUT_MAP is a list of integers.  Positive output-map entries
 * address the provider child plan; negative entries address edge table
 * attributes; zero is invalid.
 */
typedef enum AgeWCOJProviderDescField
{
    AGE_WCOJ_PROVIDER_DESC_KIND = 0,
    AGE_WCOJ_PROVIDER_DESC_EDGE_RTI,
    AGE_WCOJ_PROVIDER_DESC_EDGE_REL_OID,
    AGE_WCOJ_PROVIDER_DESC_INDEX_OID,
    AGE_WCOJ_PROVIDER_DESC_OUTGOING,
    AGE_WCOJ_PROVIDER_DESC_TERMINAL_ATTNO,
    AGE_WCOJ_PROVIDER_DESC_TERMINAL_LABEL_ID,
    AGE_WCOJ_PROVIDER_DESC_SOURCE_KEY_ATTNO,
    AGE_WCOJ_PROVIDER_DESC_OUTPUT_MAP,
    AGE_WCOJ_PROVIDER_DESC_OUTPUT_WIDTH,
    AGE_WCOJ_PROVIDER_DESC_OUTPUT_OFFSET,
    AGE_WCOJ_PROVIDER_DESC_PAYLOAD_MASK,
    AGE_WCOJ_PROVIDER_DESC_EDGE_ID_OUTPUT_ATTNO,
    AGE_WCOJ_PROVIDER_DESC_ESTIMATE_TRUSTED,
    AGE_WCOJ_PROVIDER_DESC_COUNT
} AgeWCOJProviderDescField;

extern const CustomScanMethods age_wcoj_join_scan_methods;

#endif
