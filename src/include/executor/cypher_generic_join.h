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
    AGE_GENERIC_JOIN_PRIVATE_COUNT
} AgeGenericJoinPrivateField;

typedef enum AgeGenericProviderKind
{
    AGE_GENERIC_PROVIDER_VERTEX = 0,
    AGE_GENERIC_PROVIDER_EDGE
} AgeGenericProviderKind;

typedef enum AgeGenericProviderDescField
{
    AGE_GENERIC_PROVIDER_DESC_KIND = 0,
    AGE_GENERIC_PROVIDER_DESC_REL_RTI,
    AGE_GENERIC_PROVIDER_DESC_VAR1,
    AGE_GENERIC_PROVIDER_DESC_VAR2,
    AGE_GENERIC_PROVIDER_DESC_KEY1_ATTNO,
    AGE_GENERIC_PROVIDER_DESC_KEY2_ATTNO,
    AGE_GENERIC_PROVIDER_DESC_EDGE_ID_ATTNO,
    AGE_GENERIC_PROVIDER_DESC_OUTPUT_WIDTH,
    AGE_GENERIC_PROVIDER_DESC_OUTPUT_OFFSET,
    AGE_GENERIC_PROVIDER_DESC_COUNT
} AgeGenericProviderDescField;

extern const CustomScanMethods age_generic_join_scan_methods;

#endif
