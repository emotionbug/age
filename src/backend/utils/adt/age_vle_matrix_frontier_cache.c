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

/*
 * Bounded residency budget.  resident_payloads + resident_sources is kept under
 * this many graphid-sized units; entries unused for longer than the reuse
 * window (in access-clock ticks) are evicted so stale payloads do not skew the
 * replay/raw/scalar cutover.
 */
#define VLE_MATRIX_FRONTIER_RESIDENCY_LIMIT 262144
#define VLE_MATRIX_FRONTIER_REUSE_WINDOW 4096

static uint64 age_vle_matrix_frontier_source_hash(
    const graphid *source_vertex_ids, int64 source_vertex_count);
static bool age_vle_matrix_frontier_cache_key_compatible(
    const VLEMatrixFrontierCacheKey *left,
    const VLEMatrixFrontierCacheKey *right);
static bool age_vle_matrix_frontier_source_contains(
    const graphid *source_vertex_ids, int64 source_vertex_count,
    graphid source_vertex_id);
static void age_vle_matrix_frontier_cache_build_evidence(
    const VLEMatrixFrontierCacheEntry *entry,
    const graphid *source_vertex_ids, int64 source_vertex_count,
    uint64 access_clock, bool exact_match,
    VLEMatrixFrontierCacheEvidence *evidence);
static void age_vle_matrix_frontier_directory_make_key(
    VLEMatrixFrontierDirectoryKey *dkey,
    const VLEMatrixFrontierCacheEntry *entry, graphid source_vertex_id);
static void age_vle_matrix_frontier_directory_register(
    VLEMatrixFrontierCache *cache, VLEMatrixFrontierCacheEntry *entry,
    graphid source_vertex_id);
static void age_vle_matrix_frontier_directory_deregister(
    VLEMatrixFrontierCache *cache, VLEMatrixFrontierCacheEntry *entry,
    graphid source_vertex_id);
static void age_vle_matrix_frontier_cache_evict_entry(
    VLEMatrixFrontierCache *cache, VLEMatrixFrontierCacheEntry *entry);
static void age_vle_matrix_frontier_cache_enforce_residency(
    VLEMatrixFrontierCache *cache, VLEMatrixFrontierCacheEntry *protect);

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

uint64 age_vle_matrix_frontier_cache_advance_clock(
    VLEMatrixFrontierCache *matrix_cache)
{
    Assert(matrix_cache != NULL);

    return ++matrix_cache->access_clock;
}

VLEMatrixFrontierCacheEntry *age_vle_matrix_frontier_cache_get(
    VLEMatrixFrontierCache **matrix_cache, const VLEMatrixFrontierCacheKey *key,
    bool *found)
{
    VLEMatrixFrontierCache *cache;
    VLEMatrixFrontierCacheEntry *entry;
    MemoryContext oldctx;
    HASHCTL ctl;

    Assert(matrix_cache != NULL);
    Assert(key != NULL);
    Assert(found != NULL);

    if (*matrix_cache == NULL)
    {
        oldctx = MemoryContextSwitchTo(TopMemoryContext);
        cache = palloc0(sizeof(VLEMatrixFrontierCache));
        MemSet(&ctl, 0, sizeof(ctl));
        ctl.keysize = sizeof(VLEMatrixFrontierCacheKey);
        ctl.entrysize = sizeof(VLEMatrixFrontierCacheEntry);
        ctl.hash = tag_hash;
        cache->entries = hash_create("VLE matrix frontier cache", 1024,
                                     &ctl, HASH_ELEM | HASH_FUNCTION);
        MemSet(&ctl, 0, sizeof(ctl));
        ctl.keysize = sizeof(VLEMatrixFrontierDirectoryKey);
        ctl.entrysize = sizeof(VLEMatrixFrontierDirectoryEntry);
        ctl.hash = tag_hash;
        cache->directory = hash_create("VLE matrix frontier directory", 1024,
                                       &ctl, HASH_ELEM | HASH_FUNCTION);
        cache->residency_limit = VLE_MATRIX_FRONTIER_RESIDENCY_LIMIT;
        cache->reuse_window = VLE_MATRIX_FRONTIER_REUSE_WINDOW;
        *matrix_cache = cache;
        MemoryContextSwitchTo(oldctx);
    }

    cache = *matrix_cache;
    entry = hash_search(cache->entries, key, HASH_ENTER, found);
    if (!*found)
    {
        entry->count = 0;
        entry->capacity = 0;
        entry->payloads = NULL;
        entry->source_vertex_ids = NULL;
        entry->source_vertex_count = 0;
        entry->last_access_clock = 0;
        entry->access_count = 0;
        entry->known_empty = false;
    }

    return entry;
}

VLEMatrixFrontierCacheEntry *age_vle_matrix_frontier_cache_lookup(
    VLEMatrixFrontierCache *matrix_cache, const VLEMatrixFrontierCacheKey *key)
{
    Assert(key != NULL);

    if (matrix_cache == NULL || matrix_cache->entries == NULL)
        return NULL;

    return hash_search(matrix_cache->entries, key, HASH_FIND, NULL);
}

static void age_vle_matrix_frontier_directory_make_key(
    VLEMatrixFrontierDirectoryKey *dkey,
    const VLEMatrixFrontierCacheEntry *entry, graphid source_vertex_id)
{
    MemSet(dkey, 0, sizeof(*dkey));
    dkey->source_vertex_id = source_vertex_id;
    dkey->target_path_length = entry->key.target_path_length;
    dkey->outgoing = entry->key.outgoing;
}

static void age_vle_matrix_frontier_directory_register(
    VLEMatrixFrontierCache *cache, VLEMatrixFrontierCacheEntry *entry,
    graphid source_vertex_id)
{
    VLEMatrixFrontierDirectoryKey dkey;
    VLEMatrixFrontierDirectoryEntry *dentry;
    MemoryContext oldctx;
    bool found;

    age_vle_matrix_frontier_directory_make_key(&dkey, entry, source_vertex_id);

    oldctx = MemoryContextSwitchTo(TopMemoryContext);
    dentry = hash_search(cache->directory, &dkey, HASH_ENTER, &found);
    if (!found)
    {
        dentry->members = NULL;
        dentry->member_count = 0;
        dentry->member_capacity = 0;
        dentry->probe_count = 0;
        dentry->hit_count = 0;
    }
    if (dentry->member_count == dentry->member_capacity)
    {
        int32 new_capacity = dentry->member_capacity == 0 ?
            4 : dentry->member_capacity * 2;

        if (dentry->members == NULL)
        {
            dentry->members = palloc_array(VLEMatrixFrontierCacheEntry *,
                                           new_capacity);
        }
        else
        {
            dentry->members = repalloc_array(dentry->members,
                                             VLEMatrixFrontierCacheEntry *,
                                             new_capacity);
        }
        dentry->member_capacity = new_capacity;
    }
    dentry->members[dentry->member_count++] = entry;
    MemoryContextSwitchTo(oldctx);
}

static void age_vle_matrix_frontier_directory_deregister(
    VLEMatrixFrontierCache *cache, VLEMatrixFrontierCacheEntry *entry,
    graphid source_vertex_id)
{
    VLEMatrixFrontierDirectoryKey dkey;
    VLEMatrixFrontierDirectoryEntry *dentry;
    int32 i;

    age_vle_matrix_frontier_directory_make_key(&dkey, entry, source_vertex_id);
    dentry = hash_search(cache->directory, &dkey, HASH_FIND, NULL);
    if (dentry == NULL)
        return;

    for (i = 0; i < dentry->member_count; i++)
    {
        if (dentry->members[i] == entry)
        {
            dentry->members[i] = dentry->members[dentry->member_count - 1];
            dentry->member_count--;
            return;
        }
    }
}

static void age_vle_matrix_frontier_cache_evict_entry(
    VLEMatrixFrontierCache *cache, VLEMatrixFrontierCacheEntry *entry)
{
    int64 freed = entry->count + entry->source_vertex_count;
    int64 i;

    if (entry->source_vertex_ids != NULL)
    {
        for (i = 0; i < entry->source_vertex_count; i++)
        {
            age_vle_matrix_frontier_directory_deregister(
                cache, entry, entry->source_vertex_ids[i]);
        }
        pfree(entry->source_vertex_ids);
        entry->source_vertex_ids = NULL;
    }
    cache->resident_sources -= entry->source_vertex_count;
    entry->source_vertex_count = 0;

    if (entry->payloads != NULL)
    {
        pfree(entry->payloads);
        entry->payloads = NULL;
    }
    cache->resident_payloads -= entry->count;
    entry->count = 0;
    entry->capacity = 0;
    entry->known_empty = false;
    entry->last_access_clock = 0;
    entry->access_count = 0;

    cache->stats.evictions++;
    cache->stats.eviction_work += freed;
}

/*
 * Ordering for the over-limit eviction pass: coldest first, where "cold" means
 * the entry has been replayed (accessed via the directory) the fewest times,
 * breaking ties toward the least recently touched.  This preserves source
 * buckets with high directory reuse density and reclaims the ones that never
 * paid off.
 */
typedef struct VLEMatrixFrontierEvictionCandidate
{
    VLEMatrixFrontierCacheEntry *entry;
    int64 access_count;
    uint64 last_access_clock;
} VLEMatrixFrontierEvictionCandidate;

static int age_vle_matrix_frontier_eviction_compare(const void *a,
                                                    const void *b)
{
    const VLEMatrixFrontierEvictionCandidate *left = a;
    const VLEMatrixFrontierEvictionCandidate *right = b;

    if (left->access_count != right->access_count)
        return left->access_count < right->access_count ? -1 : 1;
    if (left->last_access_clock != right->last_access_clock)
        return left->last_access_clock < right->last_access_clock ? -1 : 1;
    return 0;
}

static bool age_vle_matrix_frontier_cache_entry_resident(
    const VLEMatrixFrontierCacheEntry *entry)
{
    return entry->count > 0 || entry->known_empty ||
        entry->source_vertex_count > 0;
}

static void age_vle_matrix_frontier_cache_enforce_residency(
    VLEMatrixFrontierCache *cache, VLEMatrixFrontierCacheEntry *protect)
{
    HASH_SEQ_STATUS status;
    VLEMatrixFrontierCacheEntry *entry;
    bool over_limit;

    Assert(cache != NULL);

    if (cache->entries == NULL)
        return;

    over_limit = cache->resident_payloads + cache->resident_sources >
        cache->residency_limit;
    if (!over_limit &&
        cache->access_clock - cache->last_sweep_clock <
            (uint64) cache->reuse_window)
    {
        return;
    }
    cache->last_sweep_clock = cache->access_clock;

    /*
     * Pass 1: reuse-density-aware staleness sweep.  An entry that has been
     * replayed at least once (access_count > 0) keeps the full reuse window;
     * an entry that was seeded but never reused gets a quarter window so cold
     * payloads do not linger.  Residency overflow tightens both windows.
     */
    hash_seq_init(&status, cache->entries);
    while ((entry = hash_seq_search(&status)) != NULL)
    {
        int64 entry_window;
        uint64 threshold;

        if (entry == protect ||
            !age_vle_matrix_frontier_cache_entry_resident(entry))
        {
            continue;
        }

        entry_window = entry->access_count > 0 ? cache->reuse_window :
            cache->reuse_window / 4;
        if (over_limit)
            entry_window /= 4;
        if (entry_window < 1)
            entry_window = 1;
        if (cache->access_clock <= (uint64) entry_window)
            continue;
        threshold = cache->access_clock - (uint64) entry_window;
        if (entry->last_access_clock == 0 ||
            entry->last_access_clock > threshold)
        {
            continue;
        }
        age_vle_matrix_frontier_cache_evict_entry(cache, entry);
    }

    /*
     * Pass 2: if still over the hard residency budget, evict the coldest
     * (lowest directory reuse density) entries until back under budget.
     */
    if (cache->resident_payloads + cache->resident_sources <=
        cache->residency_limit)
    {
        return;
    }

    {
        VLEMatrixFrontierEvictionCandidate *candidates;
        int64 candidate_count = 0;
        int64 capacity = hash_get_num_entries(cache->entries);
        int64 i;

        if (capacity <= 0)
            return;

        candidates = palloc_array(VLEMatrixFrontierEvictionCandidate,
                                  capacity);
        hash_seq_init(&status, cache->entries);
        while ((entry = hash_seq_search(&status)) != NULL)
        {
            if (entry == protect ||
                !age_vle_matrix_frontier_cache_entry_resident(entry))
            {
                continue;
            }
            if (candidate_count >= capacity)
            {
                hash_seq_term(&status);
                break;
            }
            candidates[candidate_count].entry = entry;
            candidates[candidate_count].access_count = entry->access_count;
            candidates[candidate_count].last_access_clock =
                entry->last_access_clock;
            candidate_count++;
        }

        qsort(candidates, candidate_count,
              sizeof(VLEMatrixFrontierEvictionCandidate),
              age_vle_matrix_frontier_eviction_compare);

        for (i = 0; i < candidate_count; i++)
        {
            if (cache->resident_payloads + cache->resident_sources <=
                cache->residency_limit)
            {
                break;
            }
            age_vle_matrix_frontier_cache_evict_entry(cache,
                                                      candidates[i].entry);
            cache->stats.cold_evictions++;
        }
        pfree(candidates);
    }
}

static bool age_vle_matrix_frontier_cache_key_compatible(
    const VLEMatrixFrontierCacheKey *left,
    const VLEMatrixFrontierCacheKey *right)
{
    Assert(left != NULL);
    Assert(right != NULL);

    return left->graph_oid == right->graph_oid &&
        left->edge_label_oid == right->edge_label_oid &&
        left->index_oid == right->index_oid &&
        left->terminal_label_id == right->terminal_label_id &&
        left->target_path_length == right->target_path_length &&
        left->outgoing == right->outgoing &&
        left->terminal_property_filter_id ==
            right->terminal_property_filter_id;
}

static bool age_vle_matrix_frontier_source_contains(
    const graphid *source_vertex_ids, int64 source_vertex_count,
    graphid source_vertex_id)
{
    int64 high = source_vertex_count;
    int64 low = 0;

    while (low < high)
    {
        int64 mid = low + (high - low) / 2;

        if (source_vertex_ids[mid] == source_vertex_id)
            return true;
        if (source_vertex_ids[mid] < source_vertex_id)
            low = mid + 1;
        else
            high = mid;
    }

    return false;
}

static void age_vle_matrix_frontier_cache_build_evidence(
    const VLEMatrixFrontierCacheEntry *entry,
    const graphid *source_vertex_ids, int64 source_vertex_count,
    uint64 access_clock, bool exact_match,
    VLEMatrixFrontierCacheEvidence *evidence)
{
    int64 entry_index = 0;
    int64 request_index = 0;
    int64 payload_index;

    Assert(entry != NULL);
    Assert(source_vertex_ids != NULL);
    Assert(source_vertex_count > 0);
    Assert(evidence != NULL);

    memset(evidence, 0, sizeof(*evidence));
    evidence->requested_source_count = source_vertex_count;
    evidence->entry_source_count = entry->source_vertex_count;
    evidence->payload_count = entry->count;
    evidence->exact_match = exact_match;
    if (entry->last_access_clock > 0 &&
        access_clock > entry->last_access_clock)
    {
        evidence->reuse_distance = access_clock - entry->last_access_clock;
    }

    while (entry_index < entry->source_vertex_count &&
           request_index < source_vertex_count)
    {
        graphid entry_id = entry->source_vertex_ids[entry_index];
        graphid request_id = source_vertex_ids[request_index];

        if (entry_id == request_id)
        {
            evidence->matching_source_count++;
            entry_index++;
            request_index++;
        }
        else if (entry_id < request_id)
            entry_index++;
        else
            request_index++;
    }

    for (payload_index = 0; payload_index < entry->count; payload_index++)
    {
        if (age_vle_matrix_frontier_source_contains(
                source_vertex_ids, source_vertex_count,
                entry->payloads[payload_index].source_vertex_id))
        {
            evidence->matching_payload_count++;
        }
    }
}

VLEMatrixFrontierCacheEntry *age_vle_matrix_frontier_cache_probe(
    VLEMatrixFrontierCache *matrix_cache, const VLEMatrixFrontierCacheKey *key,
    const graphid *source_vertex_ids, int64 source_vertex_count,
    graphid current_source_vertex_id,
    VLEMatrixFrontierCacheEvidence *evidence)
{
    VLEMatrixFrontierCacheEntry *best_entry = NULL;
    VLEMatrixFrontierCacheEvidence best_evidence;
    VLEMatrixFrontierCacheEntry *entry;
    VLEMatrixFrontierDirectoryKey dkey;
    VLEMatrixFrontierDirectoryEntry *dentry;
    uint64 access_clock;
    int64 eviction_pressure;
    int32 i;

    Assert(key != NULL);
    Assert(source_vertex_ids != NULL);
    Assert(source_vertex_count > 0);
    Assert(evidence != NULL);

    memset(evidence, 0, sizeof(*evidence));
    if (matrix_cache == NULL || matrix_cache->entries == NULL)
        return NULL;

    access_clock = ++matrix_cache->access_clock;
    eviction_pressure = matrix_cache->resident_payloads +
        matrix_cache->resident_sources - matrix_cache->residency_limit;
    if (eviction_pressure < 0)
        eviction_pressure = 0;

    entry = age_vle_matrix_frontier_cache_lookup(matrix_cache, key);
    if (entry != NULL && (entry->known_empty || entry->payloads != NULL) &&
        entry->source_vertex_count > 0 &&
        age_vle_matrix_frontier_source_contains(
            entry->source_vertex_ids, entry->source_vertex_count,
            current_source_vertex_id))
    {
        age_vle_matrix_frontier_cache_build_evidence(
            entry, source_vertex_ids, source_vertex_count, access_clock, true,
            evidence);
        evidence->eviction_work = eviction_pressure;
        return entry;
    }

    /*
     * No exact entry owns the current source.  Restrict the compatible-overlap
     * scan to the directory bucket that lists every entry holding this source
     * vertex under the same direction/depth instead of sweeping the whole
     * entry table.
     */
    MemSet(&dkey, 0, sizeof(dkey));
    dkey.source_vertex_id = current_source_vertex_id;
    dkey.target_path_length = key->target_path_length;
    dkey.outgoing = key->outgoing;
    dentry = hash_search(matrix_cache->directory, &dkey, HASH_FIND, NULL);
    if (dentry == NULL || dentry->member_count == 0)
        return NULL;

    dentry->probe_count++;
    matrix_cache->stats.probe_scans++;
    matrix_cache->stats.probe_candidates += dentry->member_count;

    memset(&best_evidence, 0, sizeof(best_evidence));
    for (i = 0; i < dentry->member_count; i++)
    {
        VLEMatrixFrontierCacheEvidence candidate;

        entry = dentry->members[i];
        if ((!entry->known_empty && entry->payloads == NULL) ||
            entry->source_vertex_count <= 0 ||
            !age_vle_matrix_frontier_cache_key_compatible(&entry->key, key) ||
            !age_vle_matrix_frontier_source_contains(
                entry->source_vertex_ids, entry->source_vertex_count,
                current_source_vertex_id))
        {
            continue;
        }

        age_vle_matrix_frontier_cache_build_evidence(
            entry, source_vertex_ids, source_vertex_count, access_clock, false,
            &candidate);
        if (candidate.matching_source_count <= 0)
            continue;

        if (best_entry == NULL ||
            candidate.matching_source_count >
                best_evidence.matching_source_count ||
            (candidate.matching_source_count ==
                 best_evidence.matching_source_count &&
             candidate.matching_payload_count >
                 best_evidence.matching_payload_count) ||
            (candidate.matching_source_count ==
                 best_evidence.matching_source_count &&
             candidate.matching_payload_count ==
                 best_evidence.matching_payload_count &&
             candidate.payload_count < best_evidence.payload_count) ||
            (candidate.matching_source_count ==
                 best_evidence.matching_source_count &&
             candidate.matching_payload_count ==
                 best_evidence.matching_payload_count &&
             candidate.payload_count == best_evidence.payload_count &&
             candidate.reuse_distance < best_evidence.reuse_distance))
        {
            best_entry = entry;
            best_evidence = candidate;
        }
    }

    if (best_entry == NULL)
        return NULL;

    dentry->hit_count++;
    matrix_cache->stats.probe_hits++;

    *evidence = best_evidence;
    evidence->eviction_work = eviction_pressure;
    return best_entry;
}

void age_vle_matrix_frontier_cache_set_sources(
    VLEMatrixFrontierCache *matrix_cache,
    VLEMatrixFrontierCacheEntry *cache_entry,
    const graphid *source_vertex_ids, int64 source_vertex_count)
{
    MemoryContext oldctx;
    int64 i;

    Assert(matrix_cache != NULL);
    Assert(cache_entry != NULL);
    Assert(source_vertex_ids != NULL);
    Assert(source_vertex_count > 0);

    if (cache_entry->source_vertex_ids != NULL)
    {
        for (i = 0; i < cache_entry->source_vertex_count; i++)
        {
            age_vle_matrix_frontier_directory_deregister(
                matrix_cache, cache_entry, cache_entry->source_vertex_ids[i]);
        }
        matrix_cache->resident_sources -= cache_entry->source_vertex_count;
        pfree(cache_entry->source_vertex_ids);
    }
    oldctx = MemoryContextSwitchTo(TopMemoryContext);
    cache_entry->source_vertex_ids = palloc_array(graphid,
                                                  source_vertex_count);
    memcpy(cache_entry->source_vertex_ids, source_vertex_ids,
           sizeof(graphid) * source_vertex_count);
    MemoryContextSwitchTo(oldctx);
    cache_entry->source_vertex_count = source_vertex_count;
    cache_entry->last_access_clock = ++matrix_cache->access_clock;
    matrix_cache->resident_sources += source_vertex_count;
    for (i = 0; i < source_vertex_count; i++)
    {
        age_vle_matrix_frontier_directory_register(
            matrix_cache, cache_entry, source_vertex_ids[i]);
    }

    age_vle_matrix_frontier_cache_enforce_residency(matrix_cache, cache_entry);
}

void age_vle_matrix_frontier_cache_append(
    VLEMatrixFrontierCache *matrix_cache,
    VLEMatrixFrontierCacheEntry *cache_entry, graphid source_vertex_id,
    const AgeAdjacencyPayload *payload)
{
    Assert(matrix_cache != NULL);
    Assert(cache_entry != NULL);
    Assert(payload != NULL);

    if (cache_entry->count == cache_entry->capacity)
    {
        age_vle_matrix_frontier_cache_reserve(
            cache_entry, Max(cache_entry->capacity, 8));
    }

    cache_entry->known_empty = false;
    cache_entry->payloads[cache_entry->count].source_vertex_id =
        source_vertex_id;
    cache_entry->payloads[cache_entry->count].source_count = 1;
    cache_entry->payloads[cache_entry->count].payload = *payload;
    cache_entry->count++;
    matrix_cache->resident_payloads++;
}

void age_vle_matrix_frontier_cache_append_reserved(
    VLEMatrixFrontierCache *matrix_cache,
    VLEMatrixFrontierCacheEntry *cache_entry, graphid source_vertex_id,
    const AgeAdjacencyPayload *payload, int64 source_count)
{
    Assert(matrix_cache != NULL);
    Assert(cache_entry != NULL);
    Assert(payload != NULL);
    Assert(cache_entry->count < cache_entry->capacity);
    Assert(cache_entry->payloads != NULL);
    Assert(source_count > 0);

    cache_entry->known_empty = false;
    cache_entry->payloads[cache_entry->count].source_vertex_id =
        source_vertex_id;
    cache_entry->payloads[cache_entry->count].source_count = source_count;
    cache_entry->payloads[cache_entry->count].payload = *payload;
    cache_entry->count++;
    matrix_cache->resident_payloads++;
}

void age_vle_matrix_frontier_cache_reserve(
    VLEMatrixFrontierCacheEntry *cache_entry, int64 additional_payloads)
{
    int64 required_capacity;
    int64 new_capacity;
    MemoryContext oldctx;

    Assert(cache_entry != NULL);

    if (additional_payloads <= 0)
        return;

    required_capacity = cache_entry->count + additional_payloads;
    if (required_capacity <= cache_entry->capacity)
        return;

    new_capacity = cache_entry->capacity == 0 ? 8 : cache_entry->capacity;
    while (new_capacity < required_capacity)
        new_capacity *= 2;

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

void age_vle_matrix_frontier_cache_discard(
    VLEMatrixFrontierCache *matrix_cache,
    VLEMatrixFrontierCacheEntry *cache_entry)
{
    Assert(matrix_cache != NULL);
    Assert(cache_entry != NULL);

    if (cache_entry->payloads != NULL)
        pfree(cache_entry->payloads);
    matrix_cache->resident_payloads -= cache_entry->count;
    cache_entry->count = 0;
    cache_entry->capacity = 0;
    cache_entry->payloads = NULL;
    cache_entry->known_empty = false;
}

void age_vle_matrix_frontier_cache_mark_empty(
    VLEMatrixFrontierCache *matrix_cache,
    VLEMatrixFrontierCacheEntry *cache_entry)
{
    Assert(matrix_cache != NULL);
    Assert(cache_entry != NULL);

    age_vle_matrix_frontier_cache_discard(matrix_cache, cache_entry);
    cache_entry->known_empty = true;
}

void age_vle_matrix_frontier_cache_free(VLEMatrixFrontierCache **matrix_cache)
{
    HASH_SEQ_STATUS status;
    VLEMatrixFrontierCache *cache;
    VLEMatrixFrontierCacheEntry *entry;
    VLEMatrixFrontierDirectoryEntry *dentry;

    Assert(matrix_cache != NULL);

    if (*matrix_cache == NULL)
        return;

    cache = *matrix_cache;

    hash_seq_init(&status, cache->entries);
    while ((entry = hash_seq_search(&status)) != NULL)
    {
        if (entry->payloads != NULL)
        {
            pfree(entry->payloads);
            entry->payloads = NULL;
        }
        if (entry->source_vertex_ids != NULL)
        {
            pfree(entry->source_vertex_ids);
            entry->source_vertex_ids = NULL;
        }
    }
    hash_destroy(cache->entries);

    hash_seq_init(&status, cache->directory);
    while ((dentry = hash_seq_search(&status)) != NULL)
    {
        if (dentry->members != NULL)
        {
            pfree(dentry->members);
            dentry->members = NULL;
        }
    }
    hash_destroy(cache->directory);

    pfree(cache);
    *matrix_cache = NULL;
}
