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

#ifndef AG_CYPHER_GENERIC_JOIN_H
#define AG_CYPHER_GENERIC_JOIN_H

#include "nodes/extensible.h"

#define AGE_GENERIC_JOIN_SCAN_NAME "AGE Generic Multiway Join"

typedef enum AgeGenericJoinPrivateField
{
    AGE_GENERIC_JOIN_PRIVATE_VARIABLE_COUNT = 0,
    AGE_GENERIC_JOIN_PRIVATE_VARIABLE_RTIS,
    AGE_GENERIC_JOIN_PRIVATE_PROVIDER_DESCS,
    AGE_GENERIC_JOIN_PRIVATE_UNIQUENESS_GROUPS,
    AGE_GENERIC_JOIN_PRIVATE_REDUCTION_DESC,
    AGE_GENERIC_JOIN_PRIVATE_CONSUMER,
    AGE_GENERIC_JOIN_PRIVATE_OUTPUT_TYPE,
    AGE_GENERIC_JOIN_PRIVATE_COUNT
} AgeGenericJoinPrivateField;

typedef enum AgeGenericConsumerKind
{
    AGE_GENERIC_CONSUMER_ROWS = 0,
    AGE_GENERIC_CONSUMER_COUNT
} AgeGenericConsumerKind;

typedef enum AgeGenericReductionShape
{
    AGE_GENERIC_REDUCTION_ALPHA_ACYCLIC = 0,
    AGE_GENERIC_REDUCTION_CYCLIC_CORE,
    AGE_GENERIC_REDUCTION_CYCLIC_WITH_TAIL
} AgeGenericReductionShape;

typedef enum AgeGenericReductionOrderKind
{
    AGE_GENERIC_REDUCTION_ORDER_NONE = 0,
    AGE_GENERIC_REDUCTION_ORDER_LEAF_PEEL
} AgeGenericReductionOrderKind;

typedef enum AgeGenericReductionDescriptorSource
{
    AGE_GENERIC_REDUCTION_SOURCE_LOCAL = 0,
    AGE_GENERIC_REDUCTION_SOURCE_GRAPH_JOIN_MATCH_IR
} AgeGenericReductionDescriptorSource;

typedef enum AgeGenericReductionDescField
{
    AGE_GENERIC_REDUCTION_DESC_SHAPE = 0,
    AGE_GENERIC_REDUCTION_DESC_CORE_VARIABLES,
    AGE_GENERIC_REDUCTION_DESC_TAIL_SEPARATORS,
    AGE_GENERIC_REDUCTION_DESC_ORDER_KIND,
    AGE_GENERIC_REDUCTION_DESC_ORDER_EDGES,
    AGE_GENERIC_REDUCTION_DESC_COMPONENT_COUNT,
    AGE_GENERIC_REDUCTION_DESC_COMPONENT_IDS,
    AGE_GENERIC_REDUCTION_DESC_SOURCE,
    AGE_GENERIC_REDUCTION_DESC_GHD_BAG_COUNT,
    AGE_GENERIC_REDUCTION_DESC_GHD_SEPARATOR_COUNT,
    AGE_GENERIC_REDUCTION_DESC_GHD_SEPARATORS,
    AGE_GENERIC_REDUCTION_DESC_SEMIJOIN_STEPS,
    AGE_GENERIC_REDUCTION_DESC_COUNT
} AgeGenericReductionDescField;

typedef enum AgeGenericSemijoinStepPhase
{
    AGE_GENERIC_SEMIJOIN_STEP_BOTTOM_UP = 0,
    AGE_GENERIC_SEMIJOIN_STEP_TOP_DOWN
} AgeGenericSemijoinStepPhase;

typedef enum AgeGenericSemijoinStepDescField
{
    AGE_GENERIC_SEMIJOIN_STEP_DESC_ID = 0,
    AGE_GENERIC_SEMIJOIN_STEP_DESC_PHASE,
    AGE_GENERIC_SEMIJOIN_STEP_DESC_PROVIDER_INDEX,
    AGE_GENERIC_SEMIJOIN_STEP_DESC_FROM_VARIABLE,
    AGE_GENERIC_SEMIJOIN_STEP_DESC_TO_VARIABLE,
    AGE_GENERIC_SEMIJOIN_STEP_DESC_KEY_VARIABLE,
    AGE_GENERIC_SEMIJOIN_STEP_DESC_KEY_IS_KEY1,
    AGE_GENERIC_SEMIJOIN_STEP_DESC_COUNT
} AgeGenericSemijoinStepDescField;

typedef enum AgeGenericProviderKind
{
    AGE_GENERIC_PROVIDER_VERTEX = 0,
    AGE_GENERIC_PROVIDER_EDGE
} AgeGenericProviderKind;

typedef enum AgeGenericProviderDescField
{
    AGE_GENERIC_PROVIDER_DESC_KIND = 0,
    AGE_GENERIC_PROVIDER_DESC_VAR1,
    AGE_GENERIC_PROVIDER_DESC_VAR2,
    AGE_GENERIC_PROVIDER_DESC_KEY1_ATTNO,
    AGE_GENERIC_PROVIDER_DESC_KEY2_ATTNO,
    AGE_GENERIC_PROVIDER_DESC_EDGE_ID_ATTNO,
    AGE_GENERIC_PROVIDER_DESC_COUNT
} AgeGenericProviderDescField;

extern const CustomScanMethods age_generic_join_scan_methods;

#endif
