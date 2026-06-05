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
#include "utils/age_vle_adjacency_cache.h"
#include "utils/memutils.h"

VLEAdjacencyPayloadCacheEntry *age_vle_adjacency_payload_cache_get(
    HTAB **payload_cache, Oid index_oid, graphid source_vertex_id,
    int32 terminal_label_id, uint32 terminal_property_filter_id, bool *found)
{
    VLEAdjacencyPayloadCacheKey cache_key;
    VLEAdjacencyPayloadCacheEntry *entry;
    MemoryContext oldctx;
    HASHCTL ctl;

    if (*payload_cache == NULL)
    {
        oldctx = MemoryContextSwitchTo(TopMemoryContext);
        MemSet(&ctl, 0, sizeof(ctl));
        ctl.keysize = sizeof(VLEAdjacencyPayloadCacheKey);
        ctl.entrysize = sizeof(VLEAdjacencyPayloadCacheEntry);
        ctl.hash = tag_hash;
        *payload_cache = hash_create("VLE age_adjacency payload cache", 1024,
                                     &ctl, HASH_ELEM | HASH_FUNCTION);
        MemoryContextSwitchTo(oldctx);
    }

    MemSet(&cache_key, 0, sizeof(cache_key));
    cache_key.index_oid = index_oid;
    cache_key.source_vertex_id = source_vertex_id;
    cache_key.terminal_label_id = terminal_label_id;
    cache_key.terminal_property_filter_id = terminal_property_filter_id;

    entry = hash_search(*payload_cache, &cache_key, HASH_ENTER, found);
    if (!*found)
    {
        entry->count = 0;
        entry->capacity = 0;
        entry->payloads = NULL;
        entry->known_empty = false;
    }

    return entry;
}

VLEAdjacencyPayloadCacheEntry *age_vle_adjacency_payload_cache_lookup(
    HTAB *payload_cache, Oid index_oid, graphid source_vertex_id,
    int32 terminal_label_id, uint32 terminal_property_filter_id)
{
    VLEAdjacencyPayloadCacheKey cache_key;

    if (payload_cache == NULL)
        return NULL;

    MemSet(&cache_key, 0, sizeof(cache_key));
    cache_key.index_oid = index_oid;
    cache_key.source_vertex_id = source_vertex_id;
    cache_key.terminal_label_id = terminal_label_id;
    cache_key.terminal_property_filter_id = terminal_property_filter_id;

    return hash_search(payload_cache, &cache_key, HASH_FIND, NULL);
}

void age_vle_adjacency_payload_cache_append(
    VLEAdjacencyPayloadCacheEntry *cache_entry,
    const AgeAdjacencyPayload *payload)
{
    if (cache_entry->count == cache_entry->capacity)
    {
        int64 new_capacity = cache_entry->capacity == 0 ? 8 :
                             cache_entry->capacity * 2;
        MemoryContext oldctx;

        oldctx = MemoryContextSwitchTo(TopMemoryContext);
        if (cache_entry->payloads == NULL)
        {
            cache_entry->payloads = palloc_array(AgeAdjacencyPayload,
                                                 new_capacity);
        }
        else
        {
            cache_entry->payloads = repalloc_array(cache_entry->payloads,
                                                   AgeAdjacencyPayload,
                                                   new_capacity);
        }
        MemoryContextSwitchTo(oldctx);
        cache_entry->capacity = new_capacity;
    }

    cache_entry->known_empty = false;
    cache_entry->payloads[cache_entry->count++] = *payload;
}

void age_vle_adjacency_payload_cache_discard(
    VLEAdjacencyPayloadCacheEntry *cache_entry)
{
    if (cache_entry->payloads != NULL)
    {
        pfree(cache_entry->payloads);
    }
    cache_entry->count = 0;
    cache_entry->capacity = 0;
    cache_entry->payloads = NULL;
    cache_entry->known_empty = false;
}

void age_vle_adjacency_payload_cache_mark_empty(
    VLEAdjacencyPayloadCacheEntry *cache_entry)
{
    age_vle_adjacency_payload_cache_discard(cache_entry);
    cache_entry->known_empty = true;
}

void age_vle_adjacency_payload_cache_free(HTAB **payload_cache)
{
    HASH_SEQ_STATUS status;
    VLEAdjacencyPayloadCacheEntry *entry;

    if (*payload_cache == NULL)
    {
        return;
    }

    hash_seq_init(&status, *payload_cache);
    while ((entry = hash_seq_search(&status)) != NULL)
    {
        if (entry->payloads != NULL)
        {
            pfree(entry->payloads);
            entry->payloads = NULL;
        }
    }
    hash_destroy(*payload_cache);
    *payload_cache = NULL;
}
