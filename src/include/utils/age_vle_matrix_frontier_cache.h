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
    int64 source_count;
    AgeAdjacencyPayload payload;
} VLEMatrixFrontierPayload;

typedef struct VLEMatrixFrontierCacheEntry
{
    VLEMatrixFrontierCacheKey key;
    int64 count;
    int64 capacity;
    VLEMatrixFrontierPayload *payloads;
    graphid *source_vertex_ids;
    int64 source_vertex_count;
    uint64 last_access_clock;
    int64 access_count;
    bool known_empty;
} VLEMatrixFrontierCacheEntry;

/*
 * Directory key that narrows the compatible-overlap probe to the entries that
 * actually own a given source vertex under one direction/depth, instead of a
 * full hash-table scan.
 */
typedef struct VLEMatrixFrontierDirectoryKey
{
    graphid source_vertex_id;
    int64 target_path_length;
    bool outgoing;
} VLEMatrixFrontierDirectoryKey;

typedef struct VLEMatrixFrontierDirectoryEntry
{
    VLEMatrixFrontierDirectoryKey key;
    VLEMatrixFrontierCacheEntry **members;
    int32 member_count;
    int32 member_capacity;
    int64 probe_count;
    int64 hit_count;
} VLEMatrixFrontierDirectoryEntry;

typedef struct VLEMatrixFrontierCacheStats
{
    int64 probe_candidates;
    int64 probe_scans;
    int64 probe_hits;
    int64 evictions;
    int64 eviction_work;
    int64 cold_evictions;
} VLEMatrixFrontierCacheStats;

/*
 * Owning container for the matrix-frontier cache.  It keeps the payload entry
 * table, the source/direction/depth directory that bounds compatible probing,
 * a monotonic access clock, and the bounded residency budget that evicts stale
 * payloads outside the reuse window.
 */
typedef struct VLEMatrixFrontierCache
{
    HTAB *entries;
    HTAB *directory;
    uint64 access_clock;
    uint64 last_sweep_clock;
    int64 resident_payloads;
    int64 resident_sources;
    int64 residency_limit;
    int64 reuse_window;
    VLEMatrixFrontierCacheStats stats;
} VLEMatrixFrontierCache;

typedef struct VLEMatrixFrontierCacheEvidence
{
    int64 requested_source_count;
    int64 matching_source_count;
    int64 entry_source_count;
    int64 payload_count;
    int64 matching_payload_count;
    int64 reuse_distance;
    int64 eviction_work;
    int64 replay_work;
    int64 raw_work;
    int64 scalar_work;
    bool exact_match;
    bool selected_replay;
} VLEMatrixFrontierCacheEvidence;

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
extern uint64 age_vle_matrix_frontier_cache_advance_clock(
    VLEMatrixFrontierCache *matrix_cache);
extern VLEMatrixFrontierCacheEntry *age_vle_matrix_frontier_cache_get(
    VLEMatrixFrontierCache **matrix_cache, const VLEMatrixFrontierCacheKey *key,
    bool *found);
extern VLEMatrixFrontierCacheEntry *age_vle_matrix_frontier_cache_lookup(
    VLEMatrixFrontierCache *matrix_cache, const VLEMatrixFrontierCacheKey *key);
extern VLEMatrixFrontierCacheEntry *age_vle_matrix_frontier_cache_probe(
    VLEMatrixFrontierCache *matrix_cache, const VLEMatrixFrontierCacheKey *key,
    const graphid *source_vertex_ids, int64 source_vertex_count,
    graphid current_source_vertex_id,
    VLEMatrixFrontierCacheEvidence *evidence);
extern void age_vle_matrix_frontier_cache_set_sources(
    VLEMatrixFrontierCache *matrix_cache,
    VLEMatrixFrontierCacheEntry *cache_entry,
    const graphid *source_vertex_ids, int64 source_vertex_count);
extern void age_vle_matrix_frontier_cache_append(
    VLEMatrixFrontierCache *matrix_cache,
    VLEMatrixFrontierCacheEntry *cache_entry, graphid source_vertex_id,
    const AgeAdjacencyPayload *payload);
extern void age_vle_matrix_frontier_cache_append_reserved(
    VLEMatrixFrontierCache *matrix_cache,
    VLEMatrixFrontierCacheEntry *cache_entry, graphid source_vertex_id,
    const AgeAdjacencyPayload *payload, int64 source_count);
extern void age_vle_matrix_frontier_cache_reserve(
    VLEMatrixFrontierCacheEntry *cache_entry, int64 additional_payloads);
extern void age_vle_matrix_frontier_cache_discard(
    VLEMatrixFrontierCache *matrix_cache,
    VLEMatrixFrontierCacheEntry *cache_entry);
extern void age_vle_matrix_frontier_cache_mark_empty(
    VLEMatrixFrontierCache *matrix_cache,
    VLEMatrixFrontierCacheEntry *cache_entry);
extern void age_vle_matrix_frontier_cache_free(
    VLEMatrixFrontierCache **matrix_cache);

#endif
