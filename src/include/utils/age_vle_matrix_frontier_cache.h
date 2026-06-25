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

#ifndef AGE_VLE_MATRIX_FRONTIER_CACHE_H
#define AGE_VLE_MATRIX_FRONTIER_CACHE_H

#include "postgres.h"

#include "access/age_adjacency.h"
#include "utils/hsearch.h"

typedef struct VLEMatrixFrontierCacheKey
{
    Oid graph_oid;
    Oid edge_label_oid;
    Oid index_oid;
    graphid source_vertex_id;
    uint64 frontier_hash;
    int64 frontier_source_count;
    int32 terminal_label_id;
    int64 target_path_length;
    bool outgoing;
    uint32 terminal_property_filter_id;
} VLEMatrixFrontierCacheKey;

typedef struct VLEMatrixFrontierPayload
{
    graphid source_vertex_id;
    AgeAdjacencyPayload payload;
} VLEMatrixFrontierPayload;

typedef struct VLEMatrixFrontierCacheEntry
{
    VLEMatrixFrontierCacheKey key;
    int64 count;
    int64 capacity;
    VLEMatrixFrontierPayload *payloads;
    bool known_empty;
} VLEMatrixFrontierCacheEntry;

extern void age_vle_matrix_frontier_cache_init_single_source_key(
    VLEMatrixFrontierCacheKey *key, Oid graph_oid, Oid edge_label_oid,
    Oid index_oid, graphid source_vertex_id, int32 terminal_label_id,
    int64 target_path_length, bool outgoing,
    uint32 terminal_property_filter_id);
extern void age_vle_matrix_frontier_cache_init_block_key(
    VLEMatrixFrontierCacheKey *key, Oid graph_oid, Oid edge_label_oid,
    Oid index_oid, const graphid *source_vertex_ids,
    int64 source_vertex_count, int32 terminal_label_id,
    int64 target_path_length, bool outgoing,
    uint32 terminal_property_filter_id);
extern VLEMatrixFrontierCacheEntry *age_vle_matrix_frontier_cache_get(
    HTAB **matrix_cache, const VLEMatrixFrontierCacheKey *key,
    bool *found);
extern VLEMatrixFrontierCacheEntry *age_vle_matrix_frontier_cache_lookup(
    HTAB *matrix_cache, const VLEMatrixFrontierCacheKey *key);
extern void age_vle_matrix_frontier_cache_append(
    VLEMatrixFrontierCacheEntry *cache_entry, graphid source_vertex_id,
    const AgeAdjacencyPayload *payload);
extern void age_vle_matrix_frontier_cache_discard(
    VLEMatrixFrontierCacheEntry *cache_entry);
extern void age_vle_matrix_frontier_cache_mark_empty(
    VLEMatrixFrontierCacheEntry *cache_entry);
extern void age_vle_matrix_frontier_cache_free(HTAB **matrix_cache);

#endif
