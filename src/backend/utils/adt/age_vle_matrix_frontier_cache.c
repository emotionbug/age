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

#include "common/hashfn.h"
#include "utils/age_vle_matrix_frontier_cache.h"
#include "utils/memutils.h"

static uint64 age_vle_matrix_frontier_source_hash(
    const graphid *source_vertex_ids, int64 source_vertex_count);

static uint64 age_vle_matrix_frontier_source_hash(
    const graphid *source_vertex_ids, int64 source_vertex_count)
{
    uint64 hash = UINT64CONST(0x9e3779b97f4a7c15);
    int64 i;

    Assert(source_vertex_ids != NULL);
    Assert(source_vertex_count > 0);

    for (i = 0; i < source_vertex_count; i++)
    {
        uint64 item_hash = (uint64)source_vertex_ids[i];

        item_hash ^= item_hash >> 33;
        item_hash *= UINT64CONST(0xff51afd7ed558ccd);
        item_hash ^= item_hash >> 33;
        item_hash *= UINT64CONST(0xc4ceb9fe1a85ec53);
        item_hash ^= item_hash >> 33;
        hash ^= item_hash + UINT64CONST(0x9e3779b97f4a7c15) +
            (hash << 6) + (hash >> 2);
    }
    hash ^= hash >> 33;
    hash *= UINT64CONST(0xff51afd7ed558ccd);
    hash ^= hash >> 33;
    hash *= UINT64CONST(0xc4ceb9fe1a85ec53);
    hash ^= hash >> 33;

    return hash;
}

void age_vle_matrix_frontier_cache_init_single_source_key(
    VLEMatrixFrontierCacheKey *key, Oid graph_oid, Oid edge_label_oid,
    Oid index_oid, graphid source_vertex_id, int32 terminal_label_id,
    int64 target_path_length, bool outgoing,
    uint32 terminal_property_filter_id)
{
    graphid source_vertex_ids[1];

    Assert(key != NULL);

    source_vertex_ids[0] = source_vertex_id;
    age_vle_matrix_frontier_cache_init_block_key(
        key, graph_oid, edge_label_oid, index_oid, source_vertex_ids, 1,
        terminal_label_id, target_path_length, outgoing,
        terminal_property_filter_id);
}

void age_vle_matrix_frontier_cache_init_block_key(
    VLEMatrixFrontierCacheKey *key, Oid graph_oid, Oid edge_label_oid,
    Oid index_oid, const graphid *source_vertex_ids,
    int64 source_vertex_count, int32 terminal_label_id,
    int64 target_path_length, bool outgoing,
    uint32 terminal_property_filter_id)
{
    graphid representative_source_vertex_id;

    Assert(key != NULL);
    Assert(source_vertex_ids != NULL);
    Assert(source_vertex_count > 0);

    representative_source_vertex_id = source_vertex_ids[0];
    MemSet(key, 0, sizeof(*key));
    key->graph_oid = graph_oid;
    key->edge_label_oid = edge_label_oid;
    key->index_oid = index_oid;
    key->source_vertex_id = representative_source_vertex_id;
    key->frontier_hash = age_vle_matrix_frontier_source_hash(
        source_vertex_ids, source_vertex_count);
    key->frontier_source_count = source_vertex_count;
    key->terminal_label_id = terminal_label_id;
    key->target_path_length = target_path_length;
    key->outgoing = outgoing;
    key->terminal_property_filter_id = terminal_property_filter_id;
}

VLEMatrixFrontierCacheEntry *age_vle_matrix_frontier_cache_get(
    HTAB **matrix_cache, const VLEMatrixFrontierCacheKey *key, bool *found)
{
    VLEMatrixFrontierCacheEntry *entry;
    MemoryContext oldctx;
    HASHCTL ctl;

    Assert(key != NULL);
    Assert(found != NULL);

    if (*matrix_cache == NULL)
    {
        oldctx = MemoryContextSwitchTo(TopMemoryContext);
        MemSet(&ctl, 0, sizeof(ctl));
        ctl.keysize = sizeof(VLEMatrixFrontierCacheKey);
        ctl.entrysize = sizeof(VLEMatrixFrontierCacheEntry);
        ctl.hash = tag_hash;
        *matrix_cache = hash_create("VLE matrix frontier cache", 1024,
                                    &ctl, HASH_ELEM | HASH_FUNCTION);
        MemoryContextSwitchTo(oldctx);
    }

    entry = hash_search(*matrix_cache, key, HASH_ENTER, found);
    if (!*found)
    {
        entry->count = 0;
        entry->capacity = 0;
        entry->payloads = NULL;
        entry->known_empty = false;
    }

    return entry;
}

VLEMatrixFrontierCacheEntry *age_vle_matrix_frontier_cache_lookup(
    HTAB *matrix_cache, const VLEMatrixFrontierCacheKey *key)
{
    Assert(key != NULL);

    if (matrix_cache == NULL)
        return NULL;

    return hash_search(matrix_cache, key, HASH_FIND, NULL);
}

void age_vle_matrix_frontier_cache_append(
    VLEMatrixFrontierCacheEntry *cache_entry, graphid source_vertex_id,
    const AgeAdjacencyPayload *payload)
{
    Assert(cache_entry != NULL);
    Assert(payload != NULL);

    if (cache_entry->count == cache_entry->capacity)
    {
        int64 new_capacity = cache_entry->capacity == 0 ? 8 :
                             cache_entry->capacity * 2;
        MemoryContext oldctx;

        oldctx = MemoryContextSwitchTo(TopMemoryContext);
        if (cache_entry->payloads == NULL)
        {
            cache_entry->payloads = palloc_array(VLEMatrixFrontierPayload,
                                                 new_capacity);
        }
        else
        {
            cache_entry->payloads = repalloc_array(cache_entry->payloads,
                                                   VLEMatrixFrontierPayload,
                                                   new_capacity);
        }
        MemoryContextSwitchTo(oldctx);
        cache_entry->capacity = new_capacity;
    }

    cache_entry->known_empty = false;
    cache_entry->payloads[cache_entry->count].source_vertex_id =
        source_vertex_id;
    cache_entry->payloads[cache_entry->count].payload = *payload;
    cache_entry->count++;
}

void age_vle_matrix_frontier_cache_discard(
    VLEMatrixFrontierCacheEntry *cache_entry)
{
    Assert(cache_entry != NULL);

    if (cache_entry->payloads != NULL)
        pfree(cache_entry->payloads);
    cache_entry->count = 0;
    cache_entry->capacity = 0;
    cache_entry->payloads = NULL;
    cache_entry->known_empty = false;
}

void age_vle_matrix_frontier_cache_mark_empty(
    VLEMatrixFrontierCacheEntry *cache_entry)
{
    Assert(cache_entry != NULL);

    age_vle_matrix_frontier_cache_discard(cache_entry);
    cache_entry->known_empty = true;
}

void age_vle_matrix_frontier_cache_free(HTAB **matrix_cache)
{
    HASH_SEQ_STATUS status;
    VLEMatrixFrontierCacheEntry *entry;

    Assert(matrix_cache != NULL);

    if (*matrix_cache == NULL)
        return;

    hash_seq_init(&status, *matrix_cache);
    while ((entry = hash_seq_search(&status)) != NULL)
    {
        if (entry->payloads != NULL)
        {
            pfree(entry->payloads);
            entry->payloads = NULL;
        }
    }
    hash_destroy(*matrix_cache);
    *matrix_cache = NULL;
}
