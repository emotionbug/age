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

#ifndef AGE_VLE_ADJACENCY_CACHE_H
#define AGE_VLE_ADJACENCY_CACHE_H

#include "postgres.h"

#include "access/age_adjacency.h"
#include "utils/hsearch.h"

typedef struct VLEAdjacencyPayloadCacheKey
{
    Oid index_oid;
    graphid source_vertex_id;
} VLEAdjacencyPayloadCacheKey;

typedef struct VLEAdjacencyPayloadCacheEntry
{
    VLEAdjacencyPayloadCacheKey key;
    int64 count;
    int64 capacity;
    AgeAdjacencyPayload *payloads;
} VLEAdjacencyPayloadCacheEntry;

extern VLEAdjacencyPayloadCacheEntry *age_vle_adjacency_payload_cache_get(
    HTAB **payload_cache, Oid index_oid, graphid source_vertex_id,
    bool *found);
extern void age_vle_adjacency_payload_cache_append(
    VLEAdjacencyPayloadCacheEntry *cache_entry,
    const AgeAdjacencyPayload *payload);
extern void age_vle_adjacency_payload_cache_discard(
    VLEAdjacencyPayloadCacheEntry *cache_entry);
extern void age_vle_adjacency_payload_cache_free(HTAB **payload_cache);

#endif
