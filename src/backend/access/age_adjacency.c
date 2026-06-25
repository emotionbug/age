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

#include <math.h>

#include "access/age_adjacency.h"
#include "access/amapi.h"
#include "access/genam.h"
#include "access/generic_xlog.h"
#include "access/relation.h"
#include "access/relscan.h"
#include "access/tableam.h"
#include "access/visibilitymap.h"
#include "catalog/ag_label.h"
#include "catalog/index.h"
#include "executor/tuptable.h"
#include "fmgr.h"
#include "funcapi.h"
#include "nodes/nodes.h"
#include "nodes/nodeFuncs.h"
#include "nodes/pathnodes.h"
#include "nodes/tidbitmap.h"
#include "optimizer/optimizer.h"
#include "storage/bufmgr.h"
#include "storage/bufpage.h"
#include "storage/itemptr.h"
#include "storage/lmgr.h"
#include "utils/graphid.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/tuplestore.h"

#define AGE_ADJACENCY_MAGIC 0xA9EAD620
#define AGE_ADJACENCY_VERSION 1
#define AGE_ADJACENCY_DIRECTORY_LABEL_BLOOM_SLOTS 2
#define AGE_ADJACENCY_DIRECTORY_EXACT_VERTEX_SLOTS 8
#define AGE_ADJACENCY_DIRECTORY_EXACT_VERTEX_OVERFLOW PG_UINT16_MAX
#define AGE_ADJACENCY_DIRECTORY_COMPRESSED_VERTEX_SLOTS 32
#define AGE_ADJACENCY_DIRECTORY_COMPRESSED_VERTEX_OVERFLOW PG_UINT16_MAX
#define AGE_ADJACENCY_DIRECTORY_VERTEX_BLOOM_WORDS 4
#define AGE_ADJACENCY_VALUE_POSTING_BLOOM_WORDS 8
#define AGE_ADJACENCY_MAIN_BLOCK_VERTEX_BLOOM_WORDS 4
#define AGE_ADJACENCY_MAIN_BLOCK_EXACT_VERTEX_SLOTS 8
#define AGE_ADJACENCY_MAIN_BLOCK_EXACT_VERTEX_OVERFLOW PG_UINT16_MAX
#define AGE_ADJACENCY_MAIN_BLOCK_COMPRESSED_VERTEX_SLOTS 32
#define AGE_ADJACENCY_MAIN_BLOCK_COMPRESSED_VERTEX_OVERFLOW PG_UINT16_MAX
#define AGE_ADJACENCY_METAPAGE_BLKNO 0
#define AGE_ADJACENCY_EQUAL_STRATEGY 1
#define AGE_ADJACENCY_PAYLOAD_NATTS 3

#define AGE_ADJACENCY_PAGE_META 1
#define AGE_ADJACENCY_PAGE_DIRECTORY 2
#define AGE_ADJACENCY_PAGE_MAIN 3
#define AGE_ADJACENCY_PAGE_DELTA 4

#define AGE_ADJACENCY_MAIN_BLOCK_FULL 0x0001
#define AGE_ADJACENCY_MAIN_BLOCK_COMPACT 0x0002
#define AGE_ADJACENCY_DELTA_PAGE_COMPACT 0x0001
#define AGE_ADJACENCY_DELTA_REINDEX_THRESHOLD 4096
#define AGE_ADJACENCY_PAGE_USABLE_BYTES \
    (BLCKSZ - MAXALIGN(SizeOfPageHeaderData) - \
     MAXALIGN(sizeof(AgeAdjacencyPageOpaqueData)))
#define AGE_ADJACENCY_ITEMS_PER_PAGE(item_size) \
    Max(1, (int) (AGE_ADJACENCY_PAGE_USABLE_BYTES / \
                  (MAXALIGN(item_size) + sizeof(ItemIdData))))
#define AGE_ADJACENCY_MAIN_RUN_BLOCK_HEADER \
    MAXALIGN(offsetof(AgeAdjacencyMainRunBlockData, data))
#define AGE_ADJACENCY_MAIN_RUN_BLOCK_FULL_SIZE(count) \
    (AGE_ADJACENCY_MAIN_RUN_BLOCK_HEADER + \
     sizeof(AgeAdjacencyMainPostingData) * (count))
#define AGE_ADJACENCY_MAIN_RUN_BLOCK_COMPACT_SIZE(count) \
    (AGE_ADJACENCY_MAIN_RUN_BLOCK_HEADER + \
     sizeof(AgeAdjacencyMainCompactPostingData) * (count))
#define AGE_ADJACENCY_MAIN_RUN_BLOCK_FULL_MAX_POSTINGS \
    Max(1, (int) ((AGE_ADJACENCY_PAGE_USABLE_BYTES - sizeof(ItemIdData) - \
                   AGE_ADJACENCY_MAIN_RUN_BLOCK_HEADER) / \
                  sizeof(AgeAdjacencyMainPostingData)))
#define AGE_ADJACENCY_MAIN_RUN_BLOCK_COMPACT_MAX_POSTINGS \
    Max(1, (int) ((AGE_ADJACENCY_PAGE_USABLE_BYTES - sizeof(ItemIdData) - \
                   AGE_ADJACENCY_MAIN_RUN_BLOCK_HEADER) / \
                  sizeof(AgeAdjacencyMainCompactPostingData)))

typedef struct AgeAdjacencyMetaPageData
{
    uint32 magic;
    uint16 version;
    uint16 flags;
    Oid heap_relid;
    AttrNumber key_attno;
    BlockNumber first_directory_blkno;
    BlockNumber last_directory_blkno;
    BlockNumber first_main_blkno;
    BlockNumber last_main_blkno;
    BlockNumber first_delta_blkno;
    BlockNumber last_delta_blkno;
    uint64 postings;
    uint64 directory_entries;
    uint64 delta_postings;
} AgeAdjacencyMetaPageData;

typedef AgeAdjacencyMetaPageData *AgeAdjacencyMetaPage;

typedef struct AgeAdjacencyPageOpaqueData
{
    uint32 magic;
    uint16 version;
    uint16 page_type;
    uint16 flags;
    uint16 reserved;
    BlockNumber next_blkno;
    uint32 posting_count;
    graphid min_key;
    graphid max_key;
    int32 key_label_id;
    int32 edge_label_id;
    int32 next_label_id;
} AgeAdjacencyPageOpaqueData;

typedef AgeAdjacencyPageOpaqueData *AgeAdjacencyPageOpaque;

typedef struct AgeAdjacencyPostingData
{
    graphid key;
    ItemPointerData heap_tid;
    graphid edge_id;
    graphid next_vertex_id;
} AgeAdjacencyPostingData;

typedef AgeAdjacencyPostingData *AgeAdjacencyPosting;

typedef struct AgeAdjacencyDeltaCompactPostingData
{
    uint8 key_entry_id[6];
    ItemPointerData heap_tid;
    uint8 edge_entry_id[6];
    uint8 next_entry_id[6];
} AgeAdjacencyDeltaCompactPostingData;

StaticAssertDecl(sizeof(AgeAdjacencyDeltaCompactPostingData) == 24,
                 "AgeAdjacencyDeltaCompactPostingData size changed");

typedef AgeAdjacencyDeltaCompactPostingData *AgeAdjacencyDeltaCompactPosting;

typedef struct AgeAdjacencyMainPostingData
{
    ItemPointerData heap_tid;
    graphid edge_id;
    graphid next_vertex_id;
} AgeAdjacencyMainPostingData;

typedef AgeAdjacencyMainPostingData *AgeAdjacencyMainPosting;

typedef struct AgeAdjacencyMainCompactPostingData
{
    ItemPointerData heap_tid;
    uint8 edge_entry_id[6];
    uint8 next_entry_id[6];
} AgeAdjacencyMainCompactPostingData;

StaticAssertDecl(sizeof(AgeAdjacencyMainCompactPostingData) == 18,
                 "AgeAdjacencyMainCompactPostingData size changed");

typedef AgeAdjacencyMainCompactPostingData *AgeAdjacencyMainCompactPosting;

typedef struct AgeAdjacencyMainRunBlockData
{
    uint16 posting_count;
    uint16 flags;
    uint16 exact_next_vertex_count;
    uint16 compressed_next_entry_count;
    int32 edge_label_id;
    int32 next_label_id;
    graphid min_next_vertex_id;
    graphid max_next_vertex_id;
    uint64 next_vertex_bloom[AGE_ADJACENCY_MAIN_BLOCK_VERTEX_BLOOM_WORDS];
    uint64 value_posting_bloom[AGE_ADJACENCY_VALUE_POSTING_BLOOM_WORDS];
    uint8 compressed_next_entries[
        AGE_ADJACENCY_MAIN_BLOCK_COMPRESSED_VERTEX_SLOTS][6];
    graphid exact_next_vertex_ids[AGE_ADJACENCY_MAIN_BLOCK_EXACT_VERTEX_SLOTS];
    char data[FLEXIBLE_ARRAY_MEMBER];
} AgeAdjacencyMainRunBlockData;

typedef AgeAdjacencyMainRunBlockData *AgeAdjacencyMainRunBlock;

typedef struct AgeAdjacencyDirectoryEntryData
{
    graphid key;
    BlockNumber first_blkno;
    OffsetNumber first_offnum;
    uint32 posting_count;
    uint32 main_block_count;
    int32 min_next_label_id;
    int32 max_next_label_id;
    uint32 next_label_count;
    graphid min_next_vertex_id;
    graphid max_next_vertex_id;
    uint64 next_vertex_bloom;
    uint64 next_vertex_bloom_wide[AGE_ADJACENCY_DIRECTORY_VERTEX_BLOOM_WORDS];
    uint64 value_posting_bloom[AGE_ADJACENCY_VALUE_POSTING_BLOOM_WORDS];
    uint16 exact_next_vertex_count;
    uint16 compressed_next_entry_count;
    int32 next_vertex_bloom_label_id[AGE_ADJACENCY_DIRECTORY_LABEL_BLOOM_SLOTS];
    uint64 next_vertex_label_bloom[AGE_ADJACENCY_DIRECTORY_LABEL_BLOOM_SLOTS];
    uint64 next_vertex_label_value_posting_bloom[
        AGE_ADJACENCY_DIRECTORY_LABEL_BLOOM_SLOTS]
        [AGE_ADJACENCY_VALUE_POSTING_BLOOM_WORDS];
    uint8 compressed_next_entries[
        AGE_ADJACENCY_DIRECTORY_COMPRESSED_VERTEX_SLOTS][6];
    graphid exact_next_vertex_ids[AGE_ADJACENCY_DIRECTORY_EXACT_VERTEX_SLOTS];
} AgeAdjacencyDirectoryEntryData;

typedef AgeAdjacencyDirectoryEntryData *AgeAdjacencyDirectoryEntry;

typedef struct AgeAdjacencyDirectoryPageCache
{
    bool valid;
    BlockNumber blkno;
    graphid min_key;
    graphid max_key;
    int count;
    int capacity;
    AgeAdjacencyDirectoryEntryData *entries;
} AgeAdjacencyDirectoryPageCache;

typedef struct AgeAdjacencyMainPageCache
{
    bool valid;
    BlockNumber blkno;
    BlockNumber next_blkno;
    OffsetNumber next_offnum;
    uint16 next_posting_index;
    OffsetNumber maxoff;
    int count;
    int capacity;
    struct AgeAdjacencyCachedPosting *postings;
} AgeAdjacencyMainPageCache;

typedef struct AgeAdjacencyCachedPosting
{
    AgeAdjacencyMainPostingData posting;
} AgeAdjacencyCachedPosting;

typedef struct AgeAdjacencyBuildState
{
    double indtuples;
    AgeAdjacencyPostingData *postings;
    Size count;
    Size capacity;
    AgeAdjacencyDirectoryEntryData *directory_entries;
    Size directory_count;
    Size directory_capacity;
} AgeAdjacencyBuildState;

typedef struct AgeAdjacencyScanTarget
{
    TIDBitmap *tbm;
    Tuplestorestate *tupstore;
    TupleDesc tupdesc;
    Relation heap_rel;
    Snapshot snapshot;
    TupleTableSlot *slot;
    Buffer visibilitymap_buffer;
    BlockNumber visibilitymap_cached_block;
    bool visibilitymap_cache_valid;
    bool visibilitymap_cached_all_visible;
    bool fetch_properties;
    int32 terminal_label_id;
    int64 terminal_label_filtered;
    int64 terminal_directory_label_filtered;
    int64 terminal_property_filtered;
    int64 terminal_cache_filtered;
    int64 terminal_cache_label_filtered;
    int64 terminal_cache_property_filtered;
    int64 terminal_vertex_set_range_filtered;
    int64 terminal_vertex_set_sorted_filtered;
    int64 terminal_vertex_set_block_filtered;
    int64 terminal_vertex_set_block_value_filtered;
    int64 terminal_vertex_set_block_value_posting_filtered;
    int64 terminal_vertex_set_block_range_filtered;
    int64 terminal_vertex_set_block_exact_filtered;
    int64 terminal_vertex_set_block_compressed_filtered;
    int64 terminal_vertex_set_block_bloom_filtered;
    int64 terminal_vertex_set_block_posting_filtered;
    int64 terminal_vertex_set_directory_filtered;
    int64 terminal_vertex_set_directory_range_filtered;
    int64 terminal_vertex_set_directory_exact_filtered;
    int64 terminal_vertex_set_directory_label_bloom_filtered;
    int64 terminal_vertex_set_directory_compressed_filtered;
    int64 terminal_vertex_set_directory_wide_bloom_filtered;
    int64 terminal_vertex_set_directory_value_filtered;
    int64 terminal_vertex_set_directory_value_posting_filtered;
    int64 terminal_composite_requests;
    int64 terminal_composite_block_filtered;
    int64 terminal_composite_directory_filtered;
    int64 terminal_composite_directory_estimated;
    AgeAdjacencyVertexSetFilter terminal_vertex_set_filter;
    Oid terminal_property_index_oid;
    uint32 terminal_property_filter_id;
    int64 terminal_property_match_count;
    AgeAdjacencyVertexFilterCallback terminal_vertex_filter;
    void *terminal_vertex_filter_state;
    bool has_terminal_property_summary;
    AgeAdjacencyPayloadCallback callback;
    void *callback_state;
} AgeAdjacencyScanTarget;

typedef enum AgeAdjacencyCacheFilterResult
{
    AGE_ADJACENCY_CACHE_FILTER_PASS,
    AGE_ADJACENCY_CACHE_FILTER_LABEL,
    AGE_ADJACENCY_CACHE_FILTER_PROPERTY
} AgeAdjacencyCacheFilterResult;

struct AgeAdjacencyVisiblePayloadScan
{
    Relation index_rel;
    Relation heap_rel;
    TupleTableSlot *slot;
    AgeAdjacencyMetaPageData meta;
    AgeAdjacencyScanTarget target;
    AgeAdjacencyDirectoryPageCache directory_cache;
    AgeAdjacencyMainPageCache main_cache;
    graphid active_key;
    bool key_active;
    bool main_active;
    uint32 main_remaining;
    int64 main_label_candidate_count;
    bool main_composite_estimate_recorded;
    BlockNumber main_blkno;
    OffsetNumber main_offnum;
    uint16 main_posting_index;
    int main_cache_index;
    BlockNumber delta_blkno;
    OffsetNumber delta_offnum;
    int32 parallel_slice_index;
    int32 parallel_slice_count;
    uint64 main_posting_ordinal;
    uint64 delta_posting_ordinal;
};

typedef struct AgeAdjacencyVisiblePayloadKeyCursor
{
    void *tag;
    graphid active_key;
    bool key_active;
    bool main_active;
    uint32 main_remaining;
    int64 main_label_candidate_count;
    uint32 initial_main_remaining;
    int64 initial_main_label_candidate_count;
    bool main_composite_estimate_recorded;
    BlockNumber main_blkno;
    OffsetNumber main_offnum;
    uint16 main_posting_index;
    int main_cache_index;
    BlockNumber delta_blkno;
    OffsetNumber delta_offnum;
    AgeAdjacencyPayload payload;
    bool payload_valid;
    bool seen;
} AgeAdjacencyVisiblePayloadKeyCursor;

struct AgeAdjacencyVisiblePayloadRunScan
{
    AgeAdjacencyVisiblePayloadScan *scan;
    AgeAdjacencyVisiblePayloadKeyCursor *cursors;
    int64 *active_cursor_indexes;
    AgeAdjacencyVisiblePayloadRunPayloadCallback payload_callback;
    void *payload_callback_state;
    AgeAdjacencyVisiblePayloadRunFilteredKeyCallback filtered_key_callback;
    void *filtered_key_callback_state;
    int64 cursor_count;
    int64 active_cursor_count;
    int64 initial_active_key_count;
    bool known_empty;
};

typedef struct AgeAdjacencyCandidateStore
{
    Tuplestorestate *tupstore;
    TupleDesc tupdesc;
} AgeAdjacencyCandidateStore;

typedef struct AgeAdjacencyCandidateRowStore
{
    Tuplestorestate *tupstore;
    TupleDesc tupdesc;
    graphid source_vertex_id;
    bool outgoing;
} AgeAdjacencyCandidateRowStore;

typedef struct AgeAdjacencyDeltaProbeStats
{
    int64 pages_visited;
    int64 pages_skipped;
    int64 entries_scanned;
} AgeAdjacencyDeltaProbeStats;

typedef struct AgeAdjacencyMainProbeStats
{
    bool found;
    int64 pages_visited;
    int64 window_offsets;
    int64 page_offsets;
    int64 block_items;
    int64 compact_block_items;
    int64 full_block_items;
    int64 entries_cached;
    int64 label_groups;
} AgeAdjacencyMainProbeStats;

PG_FUNCTION_INFO_V1(age_adjacency_handler);
PG_FUNCTION_INFO_V1(age_adjacency_debug_payload);
PG_FUNCTION_INFO_V1(age_adjacency_debug_stats);
PG_FUNCTION_INFO_V1(age_adjacency_debug_directory_probe);
PG_FUNCTION_INFO_V1(age_adjacency_debug_key_known_empty);
PG_FUNCTION_INFO_V1(age_adjacency_debug_key_known_empty_range);
PG_FUNCTION_INFO_V1(age_adjacency_debug_composite_probe);
PG_FUNCTION_INFO_V1(age_adjacency_debug_main_probe);
PG_FUNCTION_INFO_V1(age_adjacency_debug_delta_probe);
PG_FUNCTION_INFO_V1(age_adjacency_debug_delta_maintenance);
PG_FUNCTION_INFO_V1(age_adjacency_reindex_if_needed);
PG_FUNCTION_INFO_V1(age_adjacency_candidate_edges);
PG_FUNCTION_INFO_V1(age_adjacency_candidate_edge_rows);

static void age_adjacency_init_metapage(Relation heap_rel,
                                        Relation index_rel);
static void age_adjacency_init_page(Page page, uint16 page_type);
static void age_adjacency_validate_index(Relation index_rel);
static void age_adjacency_read_meta(Relation index_rel,
                                    AgeAdjacencyMetaPageData *meta_out);
static AgeAdjacencyMetaPage age_adjacency_get_meta(Page page);
static Buffer age_adjacency_new_buffer(Relation index_rel);
static void age_adjacency_form_posting(Relation index_rel, Datum *values,
                                       bool *isnull, ItemPointer heap_tid,
                                       AgeAdjacencyPostingData *posting);
static void age_adjacency_store_entry_id48(uint8 *dest, int64 entry_id);
static int64 age_adjacency_load_entry_id48(const uint8 *src);
static bool age_adjacency_main_run_block_can_compact(
    AgeAdjacencyPostingData *postings, Size posting_count,
    int32 *edge_label_id_out, int32 *next_label_id_out);
static bool age_adjacency_main_run_block_fits(uint16 flags,
                                              Size posting_count);
static Size age_adjacency_main_run_block_size(uint16 flags,
                                              Size posting_count);
static void age_adjacency_main_run_block_get_posting(
    AgeAdjacencyMainRunBlock block, uint16 posting_index, graphid key,
    AgeAdjacencyPostingData *posting);
static bool age_adjacency_main_run_block_matches_terminal_label(
    AgeAdjacencyMainRunBlock block, int32 terminal_label_id);
static bool age_adjacency_main_run_block_may_match_compact_postings(
    AgeAdjacencyMainRunBlock block, uint16 posting_index,
    const AgeAdjacencyVertexSetFilter *filter);
static void age_adjacency_form_delta_posting(AgeAdjacencyPosting posting,
                                             AgeAdjacencyDeltaCompactPosting delta);
static void age_adjacency_delta_posting_get_posting(
    AgeAdjacencyDeltaCompactPosting delta, AgeAdjacencyPageOpaque opaque,
    AgeAdjacencyPostingData *posting);
static void age_adjacency_append_item(Relation index_rel, uint16 page_type,
                                      const void *item, Size item_size,
                                      graphid item_key,
                                      int32 key_label_id,
                                      int32 edge_label_id,
                                      int32 next_label_id,
                                      uint32 posting_units,
                                      BlockNumber *inserted_blkno,
                                      OffsetNumber *inserted_offnum);
static void age_adjacency_append_main_run_block(
    Relation index_rel, AgeAdjacencyPostingData *postings, Size posting_count,
    graphid run_key, BlockNumber *inserted_blkno,
    OffsetNumber *inserted_offnum);
static void age_adjacency_append_delta_posting(Relation index_rel,
                                               AgeAdjacencyPosting posting);
static void age_adjacency_append_directory_entry(Relation index_rel,
                                                 AgeAdjacencyDirectoryEntry entry);
static int age_adjacency_compare_postings(const void *left, const void *right);
static void age_adjacency_build_main_runs(Relation index_rel,
                                          AgeAdjacencyBuildState *buildstate);
static void age_adjacency_build_callback(Relation index_rel, ItemPointer tid,
                                         Datum *values, bool *isnull,
                                         bool tuple_is_alive, void *state);
static int64 age_adjacency_scan_posting_run(Relation index_rel,
                                            BlockNumber blkno,
                                            OffsetNumber offnum,
                                            uint32 posting_count, graphid key,
                                            AgeAdjacencyScanTarget *target);
static bool age_adjacency_search_directory(Relation index_rel, graphid key,
                                           AgeAdjacencyDirectoryEntryData *entry_out,
                                           int64 *pages_visited,
                                           int64 *entries_scanned);
static bool age_adjacency_search_directory_with_meta(
    Relation index_rel, graphid key,
    const AgeAdjacencyMetaPageData *meta,
    AgeAdjacencyDirectoryEntryData *entry_out, int64 *pages_visited,
    int64 *entries_scanned);
static bool age_adjacency_find_directory_entry_with_meta(
    Relation index_rel, graphid key,
    const AgeAdjacencyMetaPageData *meta,
    AgeAdjacencyDirectoryEntryData *entry_out);
static bool age_adjacency_find_directory_entry_cached(
    AgeAdjacencyVisiblePayloadScan *scan, graphid key,
    AgeAdjacencyDirectoryEntryData *entry_out);
static bool age_adjacency_directory_entry_matches_terminal_label(
    AgeAdjacencyDirectoryEntry entry, int32 terminal_label_id);
static bool age_adjacency_directory_entry_may_match_vertex_set(
    AgeAdjacencyDirectoryEntry entry,
    const AgeAdjacencyVertexSetFilter *filter);
static uint64 age_adjacency_vertex_bloom_add(uint64 bloom, graphid vertex_id);
static void age_adjacency_vertex_bloom256_add(uint64 *bloom,
                                              graphid vertex_id);
static bool age_adjacency_vertex_bloom256_may_contain(const uint64 *bloom,
                                                       graphid vertex_id);
static bool age_adjacency_vertex_bloom256_intersects(const uint64 *left,
                                                     const uint64 *right);
static void age_adjacency_value_posting_bloom_add(uint64 *bloom,
                                                  graphid vertex_id);
static bool age_adjacency_value_posting_bloom_intersects(
    const uint64 *left, const uint64 *right);
static void age_adjacency_vertex_set_filter_prepare_value_summary(
    AgeAdjacencyVertexSetFilter *filter, uint32 value_filter_id);
static bool age_adjacency_main_run_block_may_match_vertex_range(
    AgeAdjacencyMainRunBlock block,
    const AgeAdjacencyVertexSetFilter *filter);
static void age_adjacency_main_run_block_add_exact_vertex(
    AgeAdjacencyMainRunBlock block, graphid next_vertex_id);
static bool age_adjacency_main_run_block_may_match_exact_vertex(
    AgeAdjacencyMainRunBlock block,
    const AgeAdjacencyVertexSetFilter *filter);
static void age_adjacency_main_run_block_add_compressed_vertex(
    AgeAdjacencyMainRunBlock block, graphid next_vertex_id);
static bool age_adjacency_main_run_block_may_match_compressed_vertex(
    AgeAdjacencyMainRunBlock block,
    const AgeAdjacencyVertexSetFilter *filter);
static bool age_adjacency_main_run_block_may_match_vertex_bloom(
    AgeAdjacencyMainRunBlock block,
    const AgeAdjacencyVertexSetFilter *filter);
static bool age_adjacency_main_run_block_may_match_value_posting(
    AgeAdjacencyMainRunBlock block,
    const AgeAdjacencyVertexSetFilter *filter);
static void age_adjacency_directory_entry_add_exact_vertex(
    AgeAdjacencyDirectoryEntry entry, graphid next_vertex_id);
static bool age_adjacency_directory_entry_exact_vertex_may_match(
    AgeAdjacencyDirectoryEntry entry,
    const AgeAdjacencyVertexSetFilter *filter, bool *has_exact_summary);
static void age_adjacency_directory_entry_add_compressed_vertex(
    AgeAdjacencyDirectoryEntry entry, graphid next_vertex_id);
static bool age_adjacency_directory_entry_may_match_compressed_vertex(
    AgeAdjacencyDirectoryEntry entry,
    const AgeAdjacencyVertexSetFilter *filter);
static void age_adjacency_directory_entry_add_label_bloom(
    AgeAdjacencyDirectoryEntry entry, int32 next_label_id,
    graphid next_vertex_id);
static bool age_adjacency_directory_entry_label_bloom_may_match(
    AgeAdjacencyDirectoryEntry entry, int32 terminal_label_id,
    const AgeAdjacencyVertexSetFilter *filter, bool *has_label_bloom);
static bool age_adjacency_vertex_bloom_may_contain(uint64 bloom,
                                                   graphid vertex_id);
static bool age_adjacency_directory_entry_may_match_vertex_bloom(
    AgeAdjacencyDirectoryEntry entry,
    const AgeAdjacencyVertexSetFilter *filter);
static bool age_adjacency_directory_entry_may_match_wide_vertex_bloom(
    AgeAdjacencyDirectoryEntry entry,
    const AgeAdjacencyVertexSetFilter *filter);
static bool age_adjacency_directory_entry_may_match_value_summary(
    AgeAdjacencyDirectoryEntry entry,
    const AgeAdjacencyVertexSetFilter *filter);
static bool age_adjacency_directory_entry_may_match_value_posting(
    AgeAdjacencyDirectoryEntry entry, int32 terminal_label_id,
    const AgeAdjacencyVertexSetFilter *filter);
static const char *age_adjacency_directory_entry_value_posting_source(
    AgeAdjacencyDirectoryEntry entry, int32 terminal_label_id);
static bool age_adjacency_directory_entry_range_rejects(
    AgeAdjacencyDirectoryEntry entry, AgeAdjacencyScanTarget *target);
static bool age_adjacency_directory_entry_exact_rejects(
    AgeAdjacencyDirectoryEntry entry, AgeAdjacencyScanTarget *target);
static bool age_adjacency_directory_entry_label_bloom_rejects(
    AgeAdjacencyDirectoryEntry entry, AgeAdjacencyScanTarget *target);
static bool age_adjacency_directory_entry_compressed_rejects(
    AgeAdjacencyDirectoryEntry entry, AgeAdjacencyScanTarget *target);
static bool age_adjacency_directory_entry_value_summary_rejects(
    AgeAdjacencyDirectoryEntry entry, AgeAdjacencyScanTarget *target);
static bool age_adjacency_directory_entry_value_posting_rejects(
    AgeAdjacencyDirectoryEntry entry, AgeAdjacencyScanTarget *target);
static bool age_adjacency_directory_entry_wide_bloom_rejects(
    AgeAdjacencyDirectoryEntry entry, AgeAdjacencyScanTarget *target);
static bool age_adjacency_directory_entry_matches_composite_target(
    AgeAdjacencyDirectoryEntry entry, AgeAdjacencyScanTarget *target,
    bool *label_mismatch, bool *property_mismatch);
static void age_adjacency_visible_payload_scan_record_composite_estimate(
    AgeAdjacencyVisiblePayloadScan *scan, AgeAdjacencyDirectoryEntry entry);
static bool age_adjacency_visible_payload_scan_apply_directory_vertex_set_filter(
    AgeAdjacencyVisiblePayloadScan *scan);
static int64 age_adjacency_directory_entry_terminal_label_postings(
    AgeAdjacencyDirectoryEntry entry, int32 terminal_label_id);
static void age_adjacency_load_directory_cache(
    AgeAdjacencyVisiblePayloadScan *scan, BlockNumber blkno);
static void age_adjacency_load_main_cache(AgeAdjacencyVisiblePayloadScan *scan,
                                          BlockNumber blkno);
static void age_adjacency_probe_directory(Relation index_rel, graphid key,
                                          bool *found,
                                          int64 *pages_visited,
                                          int64 *entries_scanned);
static void age_adjacency_probe_main_run(
    Relation index_rel, graphid key, const AgeAdjacencyMetaPageData *meta,
    AgeAdjacencyMainProbeStats *stats);
static void age_adjacency_probe_delta_pages(
    Relation index_rel, graphid key, const AgeAdjacencyMetaPageData *meta,
    AgeAdjacencyDeltaProbeStats *stats);
static const char *age_adjacency_delta_maintenance_action(
    const AgeAdjacencyMetaPageData *meta, BlockNumber index_pages,
    int64 *delta_pages_out, int64 *delta_tuples_per_page_out,
    const char **reason_out);
static int64 age_adjacency_emit_posting(AgeAdjacencyPosting posting,
                                        AgeAdjacencyScanTarget *target);
static bool age_adjacency_posting_visible_payload(
    AgeAdjacencyPosting posting, AgeAdjacencyScanTarget *target,
    AgeAdjacencyPayload *payload);
static bool age_adjacency_posting_matches_terminal_label(
    AgeAdjacencyScanTarget *target, graphid next_vertex_id);
static bool age_adjacency_posting_matches_terminal_vertex_filter(
    AgeAdjacencyScanTarget *target, graphid next_vertex_id);
static bool age_adjacency_vertex_set_sorted_contains(
    const AgeAdjacencyVertexSetFilter *filter, graphid vertex_id);
static AgeAdjacencyCacheFilterResult
age_adjacency_posting_matches_cache_filters(
    AgeAdjacencyPosting posting, AgeAdjacencyScanTarget *target);
static bool age_adjacency_delta_page_may_contain_key(
    AgeAdjacencyPageOpaque opaque, graphid key);
static bool age_adjacency_visible_payload_scan_next_main(
    AgeAdjacencyVisiblePayloadScan *scan, AgeAdjacencyPayload *payload);
static bool age_adjacency_visible_payload_scan_next_delta(
    AgeAdjacencyVisiblePayloadScan *scan, AgeAdjacencyPayload *payload);
static bool age_adjacency_visible_payload_key_cursor_begin(
    AgeAdjacencyVisiblePayloadScan *scan,
    AgeAdjacencyVisiblePayloadKeyCursor *cursor, graphid key);
static bool age_adjacency_visible_payload_key_cursor_next(
    AgeAdjacencyVisiblePayloadScan *scan,
    AgeAdjacencyVisiblePayloadKeyCursor *cursor,
    AgeAdjacencyPayload *payload);
static void age_adjacency_visible_payload_run_scan_activate_cursor(
    AgeAdjacencyVisiblePayloadRunScan *scan, int64 cursor_index);
static void age_adjacency_visible_payload_run_scan_deactivate_cursor(
    AgeAdjacencyVisiblePayloadRunScan *scan, int64 active_index);
static int age_adjacency_visible_payload_key_cursor_compare_payload(
    const AgeAdjacencyVisiblePayloadKeyCursor *left,
    const AgeAdjacencyVisiblePayloadKeyCursor *right);
static bool age_adjacency_visible_payload_key_cursor_next_main(
    AgeAdjacencyVisiblePayloadScan *scan,
    AgeAdjacencyVisiblePayloadKeyCursor *cursor,
    AgeAdjacencyPayload *payload);
static bool age_adjacency_visible_payload_key_cursor_next_delta(
    AgeAdjacencyVisiblePayloadScan *scan,
    AgeAdjacencyVisiblePayloadKeyCursor *cursor,
    AgeAdjacencyPayload *payload);
static bool age_adjacency_store_candidate(const AgeAdjacencyPayload *payload,
                                          void *callback_state);
static bool age_adjacency_store_candidate_row(const AgeAdjacencyPayload *payload,
                                              void *callback_state);
static int64 age_adjacency_scan_payload(Relation index_rel, graphid key,
                                        AgeAdjacencyScanTarget *target);
static int64 age_adjacency_scan_payload_with_meta(Relation index_rel,
                                                  graphid key,
                                                  AgeAdjacencyScanTarget *target,
                                                  const AgeAdjacencyMetaPageData *meta);
static bool age_adjacency_visible_payload_scan_accept_slice(
    AgeAdjacencyVisiblePayloadScan *scan, bool main_posting);
static IndexBuildResult *age_adjacency_build(Relation heap_rel,
                                             Relation index_rel,
                                             struct IndexInfo *index_info);
static void age_adjacency_build_empty(Relation index_rel);
static bool age_adjacency_insert(Relation index_rel, Datum *values,
                                 bool *isnull, ItemPointer heap_tid,
                                 Relation heap_rel,
                                 IndexUniqueCheck check_unique,
                                 bool index_unchanged,
                                 struct IndexInfo *index_info);
static IndexBulkDeleteResult *age_adjacency_bulk_delete(IndexVacuumInfo *info,
                                                        IndexBulkDeleteResult *stats,
                                                        IndexBulkDeleteCallback callback,
                                                        void *callback_state);
static IndexBulkDeleteResult *age_adjacency_vacuum_cleanup(IndexVacuumInfo *info,
                                                           IndexBulkDeleteResult *stats);
static bool age_adjacency_read_planner_meta(Oid index_oid,
                                            AgeAdjacencyMetaPageData *meta_out,
                                            BlockNumber *pages_out);
static bool age_adjacency_extract_constant_index_key(IndexPath *path,
                                                     graphid *key_out);
static bool age_adjacency_probe_planner_delta(Oid index_oid, graphid key,
                                              AgeAdjacencyDeltaProbeStats *stats);
static void age_adjacency_cost_estimate(struct PlannerInfo *root,
                                        struct IndexPath *path,
                                        double loop_count,
                                        Cost *index_startup_cost,
                                        Cost *index_total_cost,
                                        Selectivity *index_selectivity,
                                        double *index_correlation,
                                        double *index_pages);
static bytea *age_adjacency_options(Datum reloptions, bool validate);
static bool age_adjacency_validate(Oid opclass_oid);
static IndexScanDesc age_adjacency_begin_scan(Relation index_rel, int nkeys,
                                              int norderbys);
static void age_adjacency_rescan(IndexScanDesc scan, ScanKey keys, int nkeys,
                                 ScanKey orderbys, int norderbys);
static int64 age_adjacency_get_bitmap(IndexScanDesc scan, TIDBitmap *tbm);
static void age_adjacency_end_scan(IndexScanDesc scan);

static void
age_adjacency_init_page(Page page, uint16 page_type)
{
    AgeAdjacencyPageOpaque opaque;

    PageInit(page, BLCKSZ, sizeof(AgeAdjacencyPageOpaqueData));

    opaque = (AgeAdjacencyPageOpaque) PageGetSpecialPointer(page);
    opaque->magic = AGE_ADJACENCY_MAGIC;
    opaque->version = AGE_ADJACENCY_VERSION;
    opaque->page_type = page_type;
    opaque->flags = 0;
    opaque->reserved = 0;
    opaque->next_blkno = InvalidBlockNumber;
    opaque->posting_count = 0;
    opaque->min_key = 0;
    opaque->max_key = 0;
    opaque->key_label_id = INVALID_LABEL_ID;
    opaque->edge_label_id = INVALID_LABEL_ID;
    opaque->next_label_id = INVALID_LABEL_ID;
}

static void
age_adjacency_init_metapage(Relation heap_rel, Relation index_rel)
{
    Buffer metabuf;
    Page page;
    AgeAdjacencyMetaPageData meta;
    OffsetNumber offnum;
    bool needs_wal = RelationNeedsWAL(index_rel);
    GenericXLogState *state = NULL;

    if (RelationGetNumberOfBlocks(index_rel) != 0)
    {
        ereport(ERROR,
                (errcode(ERRCODE_INDEX_CORRUPTED),
                 errmsg("age_adjacency index relation is not empty")));
    }

    metabuf = age_adjacency_new_buffer(index_rel);
    LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);

    if (needs_wal)
    {
        state = GenericXLogStart(index_rel);
        page = GenericXLogRegisterBuffer(state, metabuf,
                                         GENERIC_XLOG_FULL_IMAGE);
    }
    else
    {
        page = BufferGetPage(metabuf);
    }

    age_adjacency_init_page(page, AGE_ADJACENCY_PAGE_META);

    memset(&meta, 0, sizeof(meta));
    meta.magic = AGE_ADJACENCY_MAGIC;
    meta.version = AGE_ADJACENCY_VERSION;
    meta.heap_relid = RelationGetRelid(heap_rel);
    meta.key_attno = 1;
    meta.first_directory_blkno = InvalidBlockNumber;
    meta.last_directory_blkno = InvalidBlockNumber;
    meta.first_main_blkno = InvalidBlockNumber;
    meta.last_main_blkno = InvalidBlockNumber;
    meta.first_delta_blkno = InvalidBlockNumber;
    meta.last_delta_blkno = InvalidBlockNumber;
    meta.postings = 0;
    meta.directory_entries = 0;
    meta.delta_postings = 0;

    offnum = PageAddItem(page, (Item) &meta, sizeof(meta),
                         FirstOffsetNumber, false, false);
    if (offnum == InvalidOffsetNumber)
    {
        elog(ERROR, "failed to initialize age_adjacency metapage");
    }

    if (needs_wal)
    {
        GenericXLogFinish(state);
    }
    else
    {
        MarkBufferDirty(metabuf);
    }

    UnlockReleaseBuffer(metabuf);
}

static AgeAdjacencyMetaPage
age_adjacency_get_meta(Page page)
{
    ItemId item_id;

    if (PageGetMaxOffsetNumber(page) < FirstOffsetNumber)
    {
        ereport(ERROR,
                (errcode(ERRCODE_INDEX_CORRUPTED),
                 errmsg("age_adjacency metapage has no metadata item")));
    }

    item_id = PageGetItemId(page, FirstOffsetNumber);
    if (!ItemIdIsNormal(item_id))
    {
        ereport(ERROR,
                (errcode(ERRCODE_INDEX_CORRUPTED),
                 errmsg("age_adjacency metapage metadata item is invalid")));
    }

    return (AgeAdjacencyMetaPage) PageGetItem(page, item_id);
}

static Buffer
age_adjacency_new_buffer(Relation index_rel)
{
    Buffer buffer;

    LockRelationForExtension(index_rel, ExclusiveLock);
    buffer = ReadBuffer(index_rel, P_NEW);
    UnlockRelationForExtension(index_rel, ExclusiveLock);

    return buffer;
}

static void
age_adjacency_validate_index(Relation index_rel)
{
    TupleDesc tupdesc = RelationGetDescr(index_rel);
    int i;

    if (IndexRelationGetNumberOfKeyAttributes(index_rel) !=
        AGE_ADJACENCY_PAYLOAD_NATTS ||
        IndexRelationGetNumberOfAttributes(index_rel) !=
        AGE_ADJACENCY_PAYLOAD_NATTS)
    {
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("age_adjacency payload v4 indexes require exactly "
                        "three graphid key columns"),
                 errhint("Use (endpoint_id, edge_id, next_vertex_id), such as "
                         "(start_id, id, end_id) or (end_id, id, start_id).")));
    }

    for (i = 0; i < AGE_ADJACENCY_PAYLOAD_NATTS; i++)
    {
        Form_pg_attribute attr = TupleDescAttr(tupdesc, i);

        if (attr->atttypid != GRAPHIDOID)
        {
            ereport(ERROR,
                    (errcode(ERRCODE_DATATYPE_MISMATCH),
                     errmsg("age_adjacency payload v4 column %d must be graphid",
                            i + 1)));
        }
    }
}

static void
age_adjacency_read_meta(Relation index_rel,
                        AgeAdjacencyMetaPageData *meta_out)
{
    Buffer metabuf;
    Page page;
    AgeAdjacencyPageOpaque opaque;
    AgeAdjacencyMetaPage meta;

    if (RelationGetNumberOfBlocks(index_rel) <= AGE_ADJACENCY_METAPAGE_BLKNO)
    {
        ereport(ERROR,
                (errcode(ERRCODE_INDEX_CORRUPTED),
                 errmsg("age_adjacency index has no metapage")));
    }

    metabuf = ReadBuffer(index_rel, AGE_ADJACENCY_METAPAGE_BLKNO);
    LockBuffer(metabuf, BUFFER_LOCK_SHARE);

    page = BufferGetPage(metabuf);
    opaque = (AgeAdjacencyPageOpaque) PageGetSpecialPointer(page);
    if (opaque->magic != AGE_ADJACENCY_MAGIC ||
        opaque->version != AGE_ADJACENCY_VERSION ||
        opaque->page_type != AGE_ADJACENCY_PAGE_META)
    {
        ereport(ERROR,
                (errcode(ERRCODE_INDEX_CORRUPTED),
                 errmsg("age_adjacency metapage is invalid")));
    }

    meta = age_adjacency_get_meta(page);
    if (meta->magic != AGE_ADJACENCY_MAGIC ||
        meta->version != AGE_ADJACENCY_VERSION)
    {
        ereport(ERROR,
                (errcode(ERRCODE_INDEX_CORRUPTED),
                 errmsg("age_adjacency metapage metadata is invalid")));
    }

    if (meta_out != NULL)
    {
        memcpy(meta_out, meta, sizeof(AgeAdjacencyMetaPageData));
    }

    UnlockReleaseBuffer(metabuf);
}

static void
age_adjacency_form_posting(Relation index_rel, Datum *values, bool *isnull,
                           ItemPointer heap_tid,
                           AgeAdjacencyPostingData *posting)
{
    int i;

    age_adjacency_validate_index(index_rel);

    for (i = 0; i < AGE_ADJACENCY_PAYLOAD_NATTS; i++)
    {
        if (isnull[i])
        {
            ereport(ERROR,
                    (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                     errmsg("age_adjacency payload v4 does not support null "
                            "columns")));
        }
    }

    posting->key = DATUM_GET_GRAPHID(values[0]);
    ItemPointerCopy(heap_tid, &posting->heap_tid);
    posting->edge_id = DATUM_GET_GRAPHID(values[1]);
    posting->next_vertex_id = DATUM_GET_GRAPHID(values[2]);
}

static void
age_adjacency_store_entry_id48(uint8 *dest, int64 entry_id)
{
    uint64 value = (uint64) entry_id;

    Assert(dest != NULL);
    Assert(entry_id_is_valid(entry_id));

    dest[0] = (uint8) (value & 0xff);
    dest[1] = (uint8) ((value >> 8) & 0xff);
    dest[2] = (uint8) ((value >> 16) & 0xff);
    dest[3] = (uint8) ((value >> 24) & 0xff);
    dest[4] = (uint8) ((value >> 32) & 0xff);
    dest[5] = (uint8) ((value >> 40) & 0xff);
}

static int64
age_adjacency_load_entry_id48(const uint8 *src)
{
    uint64 value;

    Assert(src != NULL);

    value = ((uint64) src[0]) |
            (((uint64) src[1]) << 8) |
            (((uint64) src[2]) << 16) |
            (((uint64) src[3]) << 24) |
            (((uint64) src[4]) << 32) |
            (((uint64) src[5]) << 40);
    return (int64) value;
}

static bool
age_adjacency_main_run_block_can_compact(AgeAdjacencyPostingData *postings,
                                         Size posting_count,
                                         int32 *edge_label_id_out,
                                         int32 *next_label_id_out)
{
    int32 edge_label_id;
    int32 next_label_id;
    Size i;

    Assert(postings != NULL);
    Assert(posting_count > 0);

    edge_label_id = get_graphid_label_id(postings[0].edge_id);
    next_label_id = get_graphid_label_id(postings[0].next_vertex_id);
    if (!label_id_is_valid(edge_label_id) ||
        !label_id_is_valid(next_label_id))
    {
        return false;
    }

    for (i = 1; i < posting_count; i++)
    {
        if (get_graphid_label_id(postings[i].edge_id) != edge_label_id ||
            get_graphid_label_id(postings[i].next_vertex_id) != next_label_id)
        {
            return false;
        }
    }

    if (edge_label_id_out != NULL)
    {
        *edge_label_id_out = edge_label_id;
    }
    if (next_label_id_out != NULL)
    {
        *next_label_id_out = next_label_id;
    }

    return true;
}

static Size
age_adjacency_main_run_block_size(uint16 flags, Size posting_count)
{
    if ((flags & AGE_ADJACENCY_MAIN_BLOCK_COMPACT) != 0)
    {
        return AGE_ADJACENCY_MAIN_RUN_BLOCK_COMPACT_SIZE(posting_count);
    }

    return AGE_ADJACENCY_MAIN_RUN_BLOCK_FULL_SIZE(posting_count);
}

static bool
age_adjacency_main_run_block_fits(uint16 flags, Size posting_count)
{
    Size block_size;

    if (posting_count == 0 || posting_count > PG_UINT16_MAX)
        return false;

    block_size = age_adjacency_main_run_block_size(flags, posting_count);

    return MAXALIGN(block_size) + sizeof(ItemIdData) <=
           AGE_ADJACENCY_PAGE_USABLE_BYTES;
}

static void
age_adjacency_main_run_block_get_posting(AgeAdjacencyMainRunBlock block,
                                         uint16 posting_index,
                                         graphid key,
                                         AgeAdjacencyPostingData *posting)
{
    Assert(block != NULL);
    Assert(posting != NULL);
    Assert(posting_index < block->posting_count);

    posting->key = key;
    if ((block->flags & AGE_ADJACENCY_MAIN_BLOCK_COMPACT) != 0)
    {
        AgeAdjacencyMainCompactPosting compact_posting;

        compact_posting =
            &((AgeAdjacencyMainCompactPosting) block->data)[posting_index];
        memcpy(&posting->heap_tid, &compact_posting->heap_tid,
               sizeof(ItemPointerData));
        posting->edge_id =
            make_graphid(block->edge_label_id,
                         age_adjacency_load_entry_id48(
                             compact_posting->edge_entry_id));
        posting->next_vertex_id =
            make_graphid(block->next_label_id,
                         age_adjacency_load_entry_id48(
                             compact_posting->next_entry_id));
    }
    else
    {
        AgeAdjacencyMainPosting full_posting;

        full_posting = &((AgeAdjacencyMainPosting) block->data)[posting_index];
        ItemPointerCopy(&full_posting->heap_tid, &posting->heap_tid);
        posting->edge_id = full_posting->edge_id;
        posting->next_vertex_id = full_posting->next_vertex_id;
    }
}

static bool
age_adjacency_main_run_block_matches_terminal_label(
    AgeAdjacencyMainRunBlock block, int32 terminal_label_id)
{
    Assert(block != NULL);

    if (!label_id_is_valid(terminal_label_id))
        return true;

    /*
     * Only compact blocks carry a block-level homogeneous next label
     * descriptor.  Full blocks must fall through to per-posting filtering.
     */
    if ((block->flags & AGE_ADJACENCY_MAIN_BLOCK_COMPACT) == 0)
        return true;

    return block->next_label_id == terminal_label_id;
}

static bool
age_adjacency_main_run_block_may_match_compact_postings(
    AgeAdjacencyMainRunBlock block, uint16 posting_index,
    const AgeAdjacencyVertexSetFilter *filter)
{
    AgeAdjacencyMainCompactPosting compact_postings;
    uint16 i;

    Assert(block != NULL);

    if (filter == NULL || !filter->has_sorted_vertex_ids)
        return true;

    if ((block->flags & AGE_ADJACENCY_MAIN_BLOCK_COMPACT) == 0)
        return true;

    if (filter->has_range &&
        (make_graphid(block->next_label_id, 0) > filter->max_vertex_id ||
         make_graphid(block->next_label_id, ENTRY_ID_MASK) <
         filter->min_vertex_id))
    {
        return false;
    }

    compact_postings = (AgeAdjacencyMainCompactPosting) block->data;
    for (i = posting_index; i < block->posting_count; i++)
    {
        graphid next_vertex_id;

        next_vertex_id = make_graphid(
            block->next_label_id,
            age_adjacency_load_entry_id48(compact_postings[i].next_entry_id));
        if (age_adjacency_vertex_set_sorted_contains(filter, next_vertex_id))
            return true;
    }

    return false;
}

static bool
age_adjacency_directory_entry_matches_terminal_label(
    AgeAdjacencyDirectoryEntry entry, int32 terminal_label_id)
{
    Assert(entry != NULL);

    if (!label_id_is_valid(terminal_label_id))
        return true;
    if (!label_id_is_valid(entry->min_next_label_id) ||
        !label_id_is_valid(entry->max_next_label_id))
    {
        return true;
    }

    return terminal_label_id >= entry->min_next_label_id &&
        terminal_label_id <= entry->max_next_label_id;
}

static bool
age_adjacency_directory_entry_may_match_vertex_set(
    AgeAdjacencyDirectoryEntry entry,
    const AgeAdjacencyVertexSetFilter *filter)
{
    Assert(entry != NULL);

    if (filter == NULL || !filter->has_range)
        return true;

    if (entry->min_next_vertex_id == 0 && entry->max_next_vertex_id == 0)
        return true;

    if (entry->min_next_vertex_id > filter->max_vertex_id ||
        entry->max_next_vertex_id < filter->min_vertex_id)
    {
        return false;
    }

    if (!age_adjacency_directory_entry_exact_vertex_may_match(entry, filter,
                                                              NULL))
        return false;

    return age_adjacency_directory_entry_may_match_vertex_bloom(entry,
                                                                filter);
}

static uint32
age_adjacency_property_filter_id_mix(uint32 filter_id, uint32 value_hash)
{
    filter_id ^= value_hash + 0x9e3779b9 + (filter_id << 6) +
                 (filter_id >> 2);
    if (filter_id == 0)
        filter_id = 1;

    return filter_id;
}

uint32
age_adjacency_property_filter_id(Oid property_index_oid,
                                 Datum property_value,
                                 bool property_value_isnull)
{
    uint32 value_hash;

    if (!OidIsValid(property_index_oid))
        return 0;

    if (property_value_isnull)
        value_hash = 0x51f15eED;
    else
        value_hash = datum_image_hash(property_value, false, -1);

    return age_adjacency_property_filter_id_mix((uint32)property_index_oid,
                                                value_hash);
}

static uint64
age_adjacency_vertex_bloom_add(uint64 bloom, graphid vertex_id)
{
    uint64 hash;
    uint64 mixed;

    hash = (uint64)vertex_id;
    mixed = hash ^ (hash >> 33);
    mixed *= UINT64CONST(0xff51afd7ed558ccd);
    mixed ^= mixed >> 33;
    mixed *= UINT64CONST(0xc4ceb9fe1a85ec53);
    mixed ^= mixed >> 33;

    bloom |= UINT64CONST(1) << (mixed & 63);
    bloom |= UINT64CONST(1) << ((mixed >> 6) & 63);
    bloom |= UINT64CONST(1) << ((mixed >> 12) & 63);

    return bloom;
}

static void
age_adjacency_vertex_bloom256_add(uint64 *bloom, graphid vertex_id)
{
    uint64 hash;
    uint64 mixed;

    Assert(bloom != NULL);

    hash = (uint64)vertex_id;
    mixed = hash ^ (hash >> 33);
    mixed *= UINT64CONST(0xff51afd7ed558ccd);
    mixed ^= mixed >> 33;
    mixed *= UINT64CONST(0xc4ceb9fe1a85ec53);
    mixed ^= mixed >> 33;

    bloom[mixed & 3] |= UINT64CONST(1) << ((mixed >> 2) & 63);
    bloom[(mixed >> 8) & 3] |= UINT64CONST(1) << ((mixed >> 10) & 63);
    bloom[(mixed >> 16) & 3] |= UINT64CONST(1) << ((mixed >> 18) & 63);
}

static bool
age_adjacency_vertex_bloom256_may_contain(const uint64 *bloom,
                                          graphid vertex_id)
{
    uint64 candidate[AGE_ADJACENCY_MAIN_BLOCK_VERTEX_BLOOM_WORDS] = {0, 0, 0, 0};
    int i;
    bool has_bloom = false;

    Assert(bloom != NULL);

    for (i = 0; i < AGE_ADJACENCY_MAIN_BLOCK_VERTEX_BLOOM_WORDS; i++)
    {
        if (bloom[i] != 0)
            has_bloom = true;
    }
    if (!has_bloom)
        return true;

    age_adjacency_vertex_bloom256_add(candidate, vertex_id);
    for (i = 0; i < AGE_ADJACENCY_MAIN_BLOCK_VERTEX_BLOOM_WORDS; i++)
    {
        if ((bloom[i] & candidate[i]) != candidate[i])
            return false;
    }

    return true;
}

static bool
age_adjacency_vertex_bloom256_intersects(const uint64 *left,
                                         const uint64 *right)
{
    int i;
    bool has_left = false;
    bool has_right = false;

    Assert(left != NULL);
    Assert(right != NULL);

    for (i = 0; i < AGE_ADJACENCY_MAIN_BLOCK_VERTEX_BLOOM_WORDS; i++)
    {
        if (left[i] != 0)
            has_left = true;
        if (right[i] != 0)
            has_right = true;
        if ((left[i] & right[i]) != 0)
            return true;
    }

    return !has_left || !has_right;
}

static void
age_adjacency_value_posting_bloom_add(uint64 *bloom, graphid vertex_id)
{
    uint64 hash;
    uint64 mixed;
    int i;

    Assert(bloom != NULL);

    hash = (uint64) vertex_id ^ UINT64CONST(0x7a6f4d2c9e3779b9);
    for (i = 0; i < 6; i++)
    {
        mixed = hash + UINT64CONST(0x9e3779b97f4a7c15) * (uint64) (i + 1);
        mixed ^= mixed >> 30;
        mixed *= UINT64CONST(0xbf58476d1ce4e5b9);
        mixed ^= mixed >> 27;
        mixed *= UINT64CONST(0x94d049bb133111eb);
        mixed ^= mixed >> 31;
        bloom[mixed & (AGE_ADJACENCY_VALUE_POSTING_BLOOM_WORDS - 1)] |=
            UINT64CONST(1) << ((mixed >> 8) & 63);
        hash = mixed;
    }
}

static bool
age_adjacency_value_posting_bloom_intersects(const uint64 *left,
                                             const uint64 *right)
{
    bool has_left = false;
    bool has_right = false;
    int i;

    Assert(left != NULL);
    Assert(right != NULL);

    for (i = 0; i < AGE_ADJACENCY_VALUE_POSTING_BLOOM_WORDS; i++)
    {
        if (left[i] != 0)
            has_left = true;
        if (right[i] != 0)
            has_right = true;
        if ((left[i] & right[i]) != 0)
            return true;
    }

    return !has_left || !has_right;
}

static void
age_adjacency_vertex_set_filter_prepare_value_summary(
    AgeAdjacencyVertexSetFilter *filter, uint32 value_filter_id)
{
    int64 i;

    Assert(filter != NULL);

    filter->value_bloom = 0;
    memset(filter->value_bloom_wide, 0, sizeof(filter->value_bloom_wide));
    memset(filter->value_posting_bloom, 0,
           sizeof(filter->value_posting_bloom));
    filter->value_filter_id = value_filter_id;
    filter->has_value_summary = false;

    if (value_filter_id == 0 ||
        !filter->has_sorted_vertex_ids ||
        filter->sorted_vertex_ids == NULL ||
        filter->sorted_vertex_count <= 0)
    {
        return;
    }

    for (i = 0; i < filter->sorted_vertex_count; i++)
    {
        filter->value_bloom = age_adjacency_vertex_bloom_add(
            filter->value_bloom, filter->sorted_vertex_ids[i]);
        age_adjacency_vertex_bloom256_add(filter->value_bloom_wide,
                                          filter->sorted_vertex_ids[i]);
        age_adjacency_value_posting_bloom_add(filter->value_posting_bloom,
                                              filter->sorted_vertex_ids[i]);
    }
    filter->has_value_summary = true;
}

static bool
age_adjacency_main_run_block_may_match_vertex_range(
    AgeAdjacencyMainRunBlock block,
    const AgeAdjacencyVertexSetFilter *filter)
{
    Assert(block != NULL);

    if (filter == NULL || !filter->has_range)
        return true;

    if (block->min_next_vertex_id == 0 && block->max_next_vertex_id == 0)
        return true;

    return block->min_next_vertex_id <= filter->max_vertex_id &&
        block->max_next_vertex_id >= filter->min_vertex_id;
}

static void
age_adjacency_main_run_block_add_exact_vertex(
    AgeAdjacencyMainRunBlock block, graphid next_vertex_id)
{
    uint16 i;

    Assert(block != NULL);

    if (block->exact_next_vertex_count ==
        AGE_ADJACENCY_MAIN_BLOCK_EXACT_VERTEX_OVERFLOW)
    {
        return;
    }

    for (i = 0; i < block->exact_next_vertex_count; i++)
    {
        if (block->exact_next_vertex_ids[i] == next_vertex_id)
            return;
    }

    if (block->exact_next_vertex_count >=
        AGE_ADJACENCY_MAIN_BLOCK_EXACT_VERTEX_SLOTS)
    {
        block->exact_next_vertex_count =
            AGE_ADJACENCY_MAIN_BLOCK_EXACT_VERTEX_OVERFLOW;
        return;
    }

    block->exact_next_vertex_ids[block->exact_next_vertex_count] =
        next_vertex_id;
    block->exact_next_vertex_count++;
}

static bool
age_adjacency_main_run_block_may_match_exact_vertex(
    AgeAdjacencyMainRunBlock block,
    const AgeAdjacencyVertexSetFilter *filter)
{
    int64 vertex_index;
    uint16 exact_index;

    Assert(block != NULL);

    if (filter == NULL ||
        !filter->has_sorted_vertex_ids ||
        filter->sorted_vertex_ids == NULL ||
        filter->sorted_vertex_count <= 0 ||
        block->exact_next_vertex_count == 0 ||
        block->exact_next_vertex_count ==
        AGE_ADJACENCY_MAIN_BLOCK_EXACT_VERTEX_OVERFLOW)
    {
        return true;
    }

    for (vertex_index = 0; vertex_index < filter->sorted_vertex_count;
         vertex_index++)
    {
        for (exact_index = 0; exact_index < block->exact_next_vertex_count;
             exact_index++)
        {
            if (filter->sorted_vertex_ids[vertex_index] ==
                block->exact_next_vertex_ids[exact_index])
            {
                return true;
            }
        }
    }

    return false;
}

static void
age_adjacency_main_run_block_add_compressed_vertex(
    AgeAdjacencyMainRunBlock block, graphid next_vertex_id)
{
    int64 next_entry_id;
    uint16 i;

    Assert(block != NULL);

    if ((block->flags & AGE_ADJACENCY_MAIN_BLOCK_COMPACT) == 0 ||
        block->compressed_next_entry_count ==
        AGE_ADJACENCY_MAIN_BLOCK_COMPRESSED_VERTEX_OVERFLOW)
    {
        return;
    }

    next_entry_id = get_graphid_entry_id(next_vertex_id);
    for (i = 0; i < block->compressed_next_entry_count; i++)
    {
        if (age_adjacency_load_entry_id48(
                block->compressed_next_entries[i]) == next_entry_id)
        {
            return;
        }
    }

    if (block->compressed_next_entry_count >=
        AGE_ADJACENCY_MAIN_BLOCK_COMPRESSED_VERTEX_SLOTS)
    {
        block->compressed_next_entry_count =
            AGE_ADJACENCY_MAIN_BLOCK_COMPRESSED_VERTEX_OVERFLOW;
        return;
    }

    age_adjacency_store_entry_id48(
        block->compressed_next_entries[block->compressed_next_entry_count],
        next_entry_id);
    block->compressed_next_entry_count++;
}

static bool
age_adjacency_main_run_block_may_match_compressed_vertex(
    AgeAdjacencyMainRunBlock block,
    const AgeAdjacencyVertexSetFilter *filter)
{
    int64 vertex_index;
    uint16 compressed_index;

    Assert(block != NULL);

    if ((block->flags & AGE_ADJACENCY_MAIN_BLOCK_COMPACT) == 0 ||
        filter == NULL ||
        !filter->has_sorted_vertex_ids ||
        filter->sorted_vertex_ids == NULL ||
        filter->sorted_vertex_count <= 0 ||
        block->compressed_next_entry_count == 0 ||
        block->compressed_next_entry_count ==
        AGE_ADJACENCY_MAIN_BLOCK_COMPRESSED_VERTEX_OVERFLOW)
    {
        return true;
    }

    for (vertex_index = 0; vertex_index < filter->sorted_vertex_count;
         vertex_index++)
    {
        graphid vertex_id = filter->sorted_vertex_ids[vertex_index];
        int64 entry_id;

        if (get_graphid_label_id(vertex_id) != block->next_label_id)
            continue;

        entry_id = get_graphid_entry_id(vertex_id);
        for (compressed_index = 0;
             compressed_index < block->compressed_next_entry_count;
             compressed_index++)
        {
            if (age_adjacency_load_entry_id48(
                    block->compressed_next_entries[compressed_index]) ==
                entry_id)
            {
                return true;
            }
        }
    }

    return false;
}

static bool
age_adjacency_main_run_block_may_match_vertex_bloom(
    AgeAdjacencyMainRunBlock block,
    const AgeAdjacencyVertexSetFilter *filter)
{
    int64 i;

    Assert(block != NULL);

    if (filter == NULL ||
        !filter->has_sorted_vertex_ids ||
        filter->sorted_vertex_ids == NULL ||
        filter->sorted_vertex_count <= 0)
    {
        return true;
    }

    for (i = 0; i < filter->sorted_vertex_count; i++)
    {
        if (age_adjacency_vertex_bloom256_may_contain(
                block->next_vertex_bloom, filter->sorted_vertex_ids[i]))
        {
            return true;
        }
    }

    return false;
}

static bool
age_adjacency_main_run_block_may_match_value_posting(
    AgeAdjacencyMainRunBlock block, const AgeAdjacencyVertexSetFilter *filter)
{
    Assert(block != NULL);

    if (filter == NULL || !filter->has_value_summary)
        return true;

    return age_adjacency_value_posting_bloom_intersects(
        block->value_posting_bloom, filter->value_posting_bloom);
}

static void
age_adjacency_directory_entry_add_exact_vertex(
    AgeAdjacencyDirectoryEntry entry, graphid next_vertex_id)
{
    uint16 i;

    Assert(entry != NULL);

    if (entry->exact_next_vertex_count ==
        AGE_ADJACENCY_DIRECTORY_EXACT_VERTEX_OVERFLOW)
    {
        return;
    }

    for (i = 0; i < entry->exact_next_vertex_count; i++)
    {
        if (entry->exact_next_vertex_ids[i] == next_vertex_id)
            return;
    }

    if (entry->exact_next_vertex_count >=
        AGE_ADJACENCY_DIRECTORY_EXACT_VERTEX_SLOTS)
    {
        entry->exact_next_vertex_count =
            AGE_ADJACENCY_DIRECTORY_EXACT_VERTEX_OVERFLOW;
        return;
    }

    entry->exact_next_vertex_ids[entry->exact_next_vertex_count] =
        next_vertex_id;
    entry->exact_next_vertex_count++;
}

static bool
age_adjacency_directory_entry_exact_vertex_may_match(
    AgeAdjacencyDirectoryEntry entry,
    const AgeAdjacencyVertexSetFilter *filter, bool *has_exact_summary)
{
    int64 vertex_index;
    uint16 exact_index;

    Assert(entry != NULL);

    if (has_exact_summary != NULL)
        *has_exact_summary = false;

    if (filter == NULL ||
        !filter->has_sorted_vertex_ids ||
        filter->sorted_vertex_ids == NULL ||
        filter->sorted_vertex_count <= 0 ||
        entry->exact_next_vertex_count == 0 ||
        entry->exact_next_vertex_count ==
        AGE_ADJACENCY_DIRECTORY_EXACT_VERTEX_OVERFLOW)
    {
        return true;
    }

    if (has_exact_summary != NULL)
        *has_exact_summary = true;

    for (vertex_index = 0; vertex_index < filter->sorted_vertex_count;
         vertex_index++)
    {
        for (exact_index = 0; exact_index < entry->exact_next_vertex_count;
             exact_index++)
        {
            if (filter->sorted_vertex_ids[vertex_index] ==
                entry->exact_next_vertex_ids[exact_index])
            {
                return true;
            }
        }
    }

    return false;
}

static void
age_adjacency_directory_entry_add_compressed_vertex(
    AgeAdjacencyDirectoryEntry entry, graphid next_vertex_id)
{
    int64 next_entry_id;
    uint16 i;

    Assert(entry != NULL);

    if (entry->compressed_next_entry_count ==
        AGE_ADJACENCY_DIRECTORY_COMPRESSED_VERTEX_OVERFLOW)
    {
        return;
    }

    next_entry_id = get_graphid_entry_id(next_vertex_id);
    for (i = 0; i < entry->compressed_next_entry_count; i++)
    {
        if (age_adjacency_load_entry_id48(
                entry->compressed_next_entries[i]) == next_entry_id)
        {
            return;
        }
    }

    if (entry->compressed_next_entry_count >=
        AGE_ADJACENCY_DIRECTORY_COMPRESSED_VERTEX_SLOTS)
    {
        entry->compressed_next_entry_count =
            AGE_ADJACENCY_DIRECTORY_COMPRESSED_VERTEX_OVERFLOW;
        return;
    }

    age_adjacency_store_entry_id48(
        entry->compressed_next_entries[entry->compressed_next_entry_count],
        next_entry_id);
    entry->compressed_next_entry_count++;
}

static bool
age_adjacency_directory_entry_may_match_compressed_vertex(
    AgeAdjacencyDirectoryEntry entry,
    const AgeAdjacencyVertexSetFilter *filter)
{
    int64 vertex_index;
    uint16 compressed_index;

    Assert(entry != NULL);

    if (entry->next_label_count != 1 ||
        filter == NULL ||
        !filter->has_sorted_vertex_ids ||
        filter->sorted_vertex_ids == NULL ||
        filter->sorted_vertex_count <= 0 ||
        entry->compressed_next_entry_count == 0 ||
        entry->compressed_next_entry_count ==
        AGE_ADJACENCY_DIRECTORY_COMPRESSED_VERTEX_OVERFLOW)
    {
        return true;
    }

    for (vertex_index = 0; vertex_index < filter->sorted_vertex_count;
         vertex_index++)
    {
        graphid vertex_id = filter->sorted_vertex_ids[vertex_index];
        int64 entry_id;

        if (get_graphid_label_id(vertex_id) != entry->min_next_label_id)
            continue;

        entry_id = get_graphid_entry_id(vertex_id);
        for (compressed_index = 0;
             compressed_index < entry->compressed_next_entry_count;
             compressed_index++)
        {
            if (age_adjacency_load_entry_id48(
                    entry->compressed_next_entries[compressed_index]) ==
                entry_id)
            {
                return true;
            }
        }
    }

    return false;
}

static void
age_adjacency_directory_entry_add_label_bloom(
    AgeAdjacencyDirectoryEntry entry, int32 next_label_id,
    graphid next_vertex_id)
{
    int i;

    Assert(entry != NULL);

    if (!label_id_is_valid(next_label_id))
        return;

    for (i = 0; i < AGE_ADJACENCY_DIRECTORY_LABEL_BLOOM_SLOTS; i++)
    {
        if (entry->next_vertex_bloom_label_id[i] == next_label_id)
        {
            entry->next_vertex_label_bloom[i] =
                age_adjacency_vertex_bloom_add(
                    entry->next_vertex_label_bloom[i], next_vertex_id);
            age_adjacency_value_posting_bloom_add(
                entry->next_vertex_label_value_posting_bloom[i],
                next_vertex_id);
            return;
        }
    }

    for (i = 0; i < AGE_ADJACENCY_DIRECTORY_LABEL_BLOOM_SLOTS; i++)
    {
        if (!label_id_is_valid(entry->next_vertex_bloom_label_id[i]))
        {
            entry->next_vertex_bloom_label_id[i] = next_label_id;
            entry->next_vertex_label_bloom[i] =
                age_adjacency_vertex_bloom_add(0, next_vertex_id);
            age_adjacency_value_posting_bloom_add(
                entry->next_vertex_label_value_posting_bloom[i],
                next_vertex_id);
            return;
        }
    }
}

static bool
age_adjacency_directory_entry_label_bloom_may_match(
    AgeAdjacencyDirectoryEntry entry, int32 terminal_label_id,
    const AgeAdjacencyVertexSetFilter *filter, bool *has_label_bloom)
{
    int i;

    Assert(entry != NULL);

    if (has_label_bloom != NULL)
        *has_label_bloom = false;

    if (!label_id_is_valid(terminal_label_id) ||
        filter == NULL ||
        !filter->has_sorted_vertex_ids ||
        filter->sorted_vertex_ids == NULL ||
        filter->sorted_vertex_count <= 0)
    {
        return true;
    }

    for (i = 0; i < AGE_ADJACENCY_DIRECTORY_LABEL_BLOOM_SLOTS; i++)
    {
        int64 vertex_index;

        if (entry->next_vertex_bloom_label_id[i] != terminal_label_id)
            continue;

        if (has_label_bloom != NULL)
            *has_label_bloom = true;
        if (entry->next_vertex_label_bloom[i] == 0)
            return true;

        for (vertex_index = 0; vertex_index < filter->sorted_vertex_count;
             vertex_index++)
        {
            if (age_adjacency_vertex_bloom_may_contain(
                    entry->next_vertex_label_bloom[i],
                    filter->sorted_vertex_ids[vertex_index]))
            {
                return true;
            }
        }

        return false;
    }

    return true;
}

static bool
age_adjacency_vertex_bloom_may_contain(uint64 bloom, graphid vertex_id)
{
    uint64 candidate;

    if (bloom == 0)
        return true;

    candidate = age_adjacency_vertex_bloom_add(0, vertex_id);
    return (bloom & candidate) == candidate;
}

static bool
age_adjacency_directory_entry_may_match_vertex_bloom(
    AgeAdjacencyDirectoryEntry entry,
    const AgeAdjacencyVertexSetFilter *filter)
{
    int64 i;

    Assert(entry != NULL);

    if (filter == NULL || !filter->has_sorted_vertex_ids ||
        filter->sorted_vertex_ids == NULL ||
        filter->sorted_vertex_count <= 0 ||
        entry->next_vertex_bloom == 0)
    {
        return true;
    }

    for (i = 0; i < filter->sorted_vertex_count; i++)
    {
        if (age_adjacency_vertex_bloom_may_contain(
                entry->next_vertex_bloom, filter->sorted_vertex_ids[i]))
        {
            return true;
        }
    }

    return false;
}

static bool
age_adjacency_directory_entry_may_match_wide_vertex_bloom(
    AgeAdjacencyDirectoryEntry entry,
    const AgeAdjacencyVertexSetFilter *filter)
{
    int64 i;

    Assert(entry != NULL);

    if (filter == NULL ||
        !filter->has_sorted_vertex_ids ||
        filter->sorted_vertex_ids == NULL ||
        filter->sorted_vertex_count <= 0)
    {
        return true;
    }

    for (i = 0; i < filter->sorted_vertex_count; i++)
    {
        if (age_adjacency_vertex_bloom256_may_contain(
                entry->next_vertex_bloom_wide, filter->sorted_vertex_ids[i]))
        {
            return true;
        }
    }

    return false;
}

static bool
age_adjacency_directory_entry_may_match_value_summary(
    AgeAdjacencyDirectoryEntry entry,
    const AgeAdjacencyVertexSetFilter *filter)
{
    Assert(entry != NULL);

    if (filter == NULL || !filter->has_value_summary)
        return true;

    if (entry->next_vertex_bloom != 0 &&
        (entry->next_vertex_bloom & filter->value_bloom) == 0)
    {
        return false;
    }

    return age_adjacency_vertex_bloom256_intersects(
        entry->next_vertex_bloom_wide, filter->value_bloom_wide);
}

static bool
age_adjacency_directory_entry_may_match_value_posting(
    AgeAdjacencyDirectoryEntry entry, int32 terminal_label_id,
    const AgeAdjacencyVertexSetFilter *filter)
{
    int i;

    Assert(entry != NULL);

    if (filter == NULL || !filter->has_value_summary)
        return true;
    if (label_id_is_valid(terminal_label_id))
    {
        for (i = 0; i < AGE_ADJACENCY_DIRECTORY_LABEL_BLOOM_SLOTS; i++)
        {
            if (entry->next_vertex_bloom_label_id[i] != terminal_label_id)
                continue;

            return age_adjacency_value_posting_bloom_intersects(
                entry->next_vertex_label_value_posting_bloom[i],
                filter->value_posting_bloom);
        }
    }
    if (entry->next_label_count != 1)
        return true;

    return age_adjacency_value_posting_bloom_intersects(
        entry->value_posting_bloom, filter->value_posting_bloom);
}

static const char *
age_adjacency_directory_entry_value_posting_source(
    AgeAdjacencyDirectoryEntry entry, int32 terminal_label_id)
{
    int i;

    Assert(entry != NULL);

    if (label_id_is_valid(terminal_label_id))
    {
        for (i = 0; i < AGE_ADJACENCY_DIRECTORY_LABEL_BLOOM_SLOTS; i++)
        {
            if (entry->next_vertex_bloom_label_id[i] == terminal_label_id)
                return "label-slice";
        }
    }
    if (entry->next_label_count == 1)
        return "run";

    return "none";
}

static bool
age_adjacency_directory_entry_range_rejects(
    AgeAdjacencyDirectoryEntry entry, AgeAdjacencyScanTarget *target)
{
    const AgeAdjacencyVertexSetFilter *filter;

    Assert(entry != NULL);
    Assert(target != NULL);

    filter = &target->terminal_vertex_set_filter;
    if (!age_adjacency_directory_entry_matches_terminal_label(
            entry, target->terminal_label_id))
    {
        return false;
    }
    if (filter == NULL || !filter->has_range)
        return false;
    if (entry->min_next_vertex_id == 0 && entry->max_next_vertex_id == 0)
        return false;

    return entry->min_next_vertex_id > filter->max_vertex_id ||
           entry->max_next_vertex_id < filter->min_vertex_id;
}

static bool
age_adjacency_directory_entry_exact_rejects(
    AgeAdjacencyDirectoryEntry entry, AgeAdjacencyScanTarget *target)
{
    bool has_exact_summary = false;

    Assert(entry != NULL);
    Assert(target != NULL);

    if (!age_adjacency_directory_entry_matches_terminal_label(
            entry, target->terminal_label_id))
    {
        return false;
    }
    if (age_adjacency_directory_entry_range_rejects(entry, target))
        return false;

    return !age_adjacency_directory_entry_exact_vertex_may_match(
               entry, &target->terminal_vertex_set_filter,
               &has_exact_summary) &&
           has_exact_summary;
}

static bool
age_adjacency_directory_entry_label_bloom_rejects(
    AgeAdjacencyDirectoryEntry entry, AgeAdjacencyScanTarget *target)
{
    bool has_label_bloom = false;

    Assert(entry != NULL);
    Assert(target != NULL);

    if (!age_adjacency_directory_entry_matches_terminal_label(
            entry, target->terminal_label_id))
    {
        return false;
    }
    if (!age_adjacency_directory_entry_may_match_vertex_set(
            entry, &target->terminal_vertex_set_filter))
    {
        return false;
    }

    return !age_adjacency_directory_entry_label_bloom_may_match(
               entry, target->terminal_label_id,
               &target->terminal_vertex_set_filter, &has_label_bloom) &&
           has_label_bloom;
}

static bool
age_adjacency_directory_entry_value_summary_rejects(
    AgeAdjacencyDirectoryEntry entry, AgeAdjacencyScanTarget *target)
{
    Assert(entry != NULL);
    Assert(target != NULL);

    if (!age_adjacency_directory_entry_matches_terminal_label(
            entry, target->terminal_label_id))
    {
        return false;
    }
    if (!age_adjacency_directory_entry_may_match_vertex_set(
            entry, &target->terminal_vertex_set_filter))
    {
        return false;
    }
    if (!age_adjacency_directory_entry_label_bloom_may_match(
            entry, target->terminal_label_id,
            &target->terminal_vertex_set_filter, NULL))
    {
        return false;
    }

    return !age_adjacency_directory_entry_may_match_value_summary(
        entry, &target->terminal_vertex_set_filter);
}

static bool
age_adjacency_directory_entry_value_posting_rejects(
    AgeAdjacencyDirectoryEntry entry, AgeAdjacencyScanTarget *target)
{
    Assert(entry != NULL);
    Assert(target != NULL);

    if (!age_adjacency_directory_entry_matches_terminal_label(
            entry, target->terminal_label_id))
    {
        return false;
    }
    if (!age_adjacency_directory_entry_may_match_vertex_set(
            entry, &target->terminal_vertex_set_filter))
    {
        return false;
    }
    if (!age_adjacency_directory_entry_label_bloom_may_match(
            entry, target->terminal_label_id,
            &target->terminal_vertex_set_filter, NULL))
    {
        return false;
    }
    if (!age_adjacency_directory_entry_may_match_compressed_vertex(
            entry, &target->terminal_vertex_set_filter))
    {
        return false;
    }
    if (!age_adjacency_directory_entry_may_match_value_summary(
            entry, &target->terminal_vertex_set_filter))
    {
        return false;
    }

    return !age_adjacency_directory_entry_may_match_value_posting(
        entry, target->terminal_label_id, &target->terminal_vertex_set_filter);
}

static bool
age_adjacency_directory_entry_compressed_rejects(
    AgeAdjacencyDirectoryEntry entry, AgeAdjacencyScanTarget *target)
{
    Assert(entry != NULL);
    Assert(target != NULL);

    if (!age_adjacency_directory_entry_matches_terminal_label(
            entry, target->terminal_label_id))
    {
        return false;
    }
    if (!age_adjacency_directory_entry_may_match_vertex_set(
            entry, &target->terminal_vertex_set_filter))
    {
        return false;
    }
    if (!age_adjacency_directory_entry_label_bloom_may_match(
            entry, target->terminal_label_id,
            &target->terminal_vertex_set_filter, NULL))
    {
        return false;
    }

    return !age_adjacency_directory_entry_may_match_compressed_vertex(
        entry, &target->terminal_vertex_set_filter);
}

static bool
age_adjacency_directory_entry_wide_bloom_rejects(
    AgeAdjacencyDirectoryEntry entry, AgeAdjacencyScanTarget *target)
{
    Assert(entry != NULL);
    Assert(target != NULL);

    if (!age_adjacency_directory_entry_matches_terminal_label(
            entry, target->terminal_label_id))
    {
        return false;
    }
    if (!age_adjacency_directory_entry_may_match_vertex_set(
            entry, &target->terminal_vertex_set_filter))
    {
        return false;
    }
    if (!age_adjacency_directory_entry_label_bloom_may_match(
            entry, target->terminal_label_id,
            &target->terminal_vertex_set_filter, NULL))
    {
        return false;
    }

    return !age_adjacency_directory_entry_may_match_wide_vertex_bloom(
        entry, &target->terminal_vertex_set_filter);
}

static bool
age_adjacency_directory_entry_matches_composite_target(
    AgeAdjacencyDirectoryEntry entry, AgeAdjacencyScanTarget *target,
    bool *label_mismatch, bool *property_mismatch)
{
    bool label_matches;
    bool property_matches;

    Assert(entry != NULL);
    Assert(target != NULL);

    label_matches = age_adjacency_directory_entry_matches_terminal_label(
        entry, target->terminal_label_id);
    property_matches = age_adjacency_directory_entry_may_match_vertex_set(
        entry, &target->terminal_vertex_set_filter);
    if (label_matches && property_matches)
        property_matches =
            age_adjacency_directory_entry_label_bloom_may_match(
                entry, target->terminal_label_id,
                &target->terminal_vertex_set_filter, NULL);
    if (label_matches && property_matches)
        property_matches =
            age_adjacency_directory_entry_may_match_compressed_vertex(
                entry, &target->terminal_vertex_set_filter);
    if (label_matches && property_matches)
        property_matches =
            age_adjacency_directory_entry_may_match_value_summary(
                entry, &target->terminal_vertex_set_filter);
    if (label_matches && property_matches)
        property_matches =
            age_adjacency_directory_entry_may_match_value_posting(
                entry, target->terminal_label_id,
                &target->terminal_vertex_set_filter);
    if (label_matches && property_matches)
        property_matches =
            age_adjacency_directory_entry_may_match_wide_vertex_bloom(
                entry, &target->terminal_vertex_set_filter);

    if (label_mismatch != NULL)
        *label_mismatch = !label_matches;
    if (property_mismatch != NULL)
        *property_mismatch = label_matches && !property_matches;

    return label_matches && property_matches;
}

static void
age_adjacency_visible_payload_scan_record_composite_estimate(
    AgeAdjacencyVisiblePayloadScan *scan, AgeAdjacencyDirectoryEntry entry)
{
    int64 label_postings;
    int64 estimated_postings;

    Assert(scan != NULL);
    Assert(entry != NULL);

    if (scan->main_composite_estimate_recorded ||
        !scan->target.has_terminal_property_summary ||
        scan->target.terminal_property_match_count < 0)
    {
        return;
    }

    label_postings = age_adjacency_directory_entry_terminal_label_postings(
        entry, scan->target.terminal_label_id);
    estimated_postings = Min(label_postings,
                             scan->target.terminal_property_match_count);
    if (estimated_postings < label_postings)
        scan->target.terminal_composite_directory_estimated +=
            estimated_postings;
    scan->main_composite_estimate_recorded = true;
}

static bool
age_adjacency_visible_payload_scan_apply_directory_vertex_set_filter(
    AgeAdjacencyVisiblePayloadScan *scan)
{
    AgeAdjacencyDirectoryEntryData entry;
    bool property_mismatch;

    if (scan == NULL || !scan->key_active || !scan->main_active ||
        scan->main_cache.valid || scan->main_cache.count > 0 ||
        scan->main_cache_index > 0)
    {
        return false;
    }

    if (!scan->target.terminal_vertex_set_filter.has_range)
        return false;

    if (!age_adjacency_find_directory_entry_cached(scan, scan->active_key,
                                                   &entry))
    {
        return false;
    }

    age_adjacency_visible_payload_scan_record_composite_estimate(scan, &entry);

    if (age_adjacency_directory_entry_matches_composite_target(
            &entry, &scan->target, NULL, &property_mismatch) ||
        !property_mismatch)
    {
        return false;
    }

    scan->target.terminal_property_filtered += scan->main_remaining;
    scan->target.terminal_cache_filtered += scan->main_remaining;
    scan->target.terminal_cache_property_filtered += scan->main_remaining;
    scan->target.terminal_vertex_set_directory_filtered +=
        scan->main_remaining;
    if (age_adjacency_directory_entry_range_rejects(&entry, &scan->target))
        scan->target.terminal_vertex_set_directory_range_filtered +=
            scan->main_remaining;
    if (age_adjacency_directory_entry_exact_rejects(&entry, &scan->target))
        scan->target.terminal_vertex_set_directory_exact_filtered +=
            scan->main_remaining;
    if (age_adjacency_directory_entry_label_bloom_rejects(&entry,
                                                          &scan->target))
    {
        scan->target.terminal_vertex_set_directory_label_bloom_filtered +=
            scan->main_remaining;
    }
    if (age_adjacency_directory_entry_compressed_rejects(
            &entry, &scan->target))
    {
        scan->target.terminal_vertex_set_directory_compressed_filtered +=
            scan->main_remaining;
    }
    if (age_adjacency_directory_entry_value_summary_rejects(
            &entry, &scan->target))
    {
        scan->target.terminal_vertex_set_directory_value_filtered +=
            scan->main_remaining;
    }
    if (age_adjacency_directory_entry_value_posting_rejects(
            &entry, &scan->target))
    {
        scan->target.terminal_vertex_set_directory_value_posting_filtered +=
            scan->main_remaining;
    }
    if (age_adjacency_directory_entry_wide_bloom_rejects(
            &entry, &scan->target))
    {
        scan->target.terminal_vertex_set_directory_wide_bloom_filtered +=
            scan->main_remaining;
    }
    if (scan->target.has_terminal_property_summary)
        scan->target.terminal_composite_directory_filtered +=
            scan->main_remaining;
    scan->main_remaining = 0;
    scan->main_label_candidate_count = 0;
    scan->main_active = false;
    scan->main_blkno = InvalidBlockNumber;
    scan->main_offnum = FirstOffsetNumber;
    scan->main_posting_index = 0;

    return true;
}

static int64
age_adjacency_directory_entry_terminal_label_postings(
    AgeAdjacencyDirectoryEntry entry, int32 terminal_label_id)
{
    Assert(entry != NULL);

    if (!label_id_is_valid(terminal_label_id))
        return entry->posting_count;
    if (!age_adjacency_directory_entry_matches_terminal_label(
            entry, terminal_label_id))
    {
        return 0;
    }
    if (entry->min_next_label_id == entry->max_next_label_id ||
        entry->next_label_count <= 1)
    {
        return entry->posting_count;
    }

    return (entry->posting_count + entry->next_label_count - 1) /
           entry->next_label_count;
}

static void
age_adjacency_form_delta_posting(AgeAdjacencyPosting posting,
                                 AgeAdjacencyDeltaCompactPosting delta)
{
    Assert(posting != NULL);
    Assert(delta != NULL);

    age_adjacency_store_entry_id48(delta->key_entry_id,
                                   get_graphid_entry_id(posting->key));
    memcpy(&delta->heap_tid, &posting->heap_tid, sizeof(ItemPointerData));
    age_adjacency_store_entry_id48(delta->edge_entry_id,
                                   get_graphid_entry_id(posting->edge_id));
    age_adjacency_store_entry_id48(delta->next_entry_id,
                                   get_graphid_entry_id(
                                       posting->next_vertex_id));
}

static void
age_adjacency_delta_posting_get_posting(AgeAdjacencyDeltaCompactPosting delta,
                                        AgeAdjacencyPageOpaque opaque,
                                        AgeAdjacencyPostingData *posting)
{
    Assert(delta != NULL);
    Assert(opaque != NULL);
    Assert(posting != NULL);
    Assert((opaque->flags & AGE_ADJACENCY_DELTA_PAGE_COMPACT) != 0);

    posting->key =
        make_graphid(opaque->key_label_id,
                     age_adjacency_load_entry_id48(delta->key_entry_id));
    memcpy(&posting->heap_tid, &delta->heap_tid, sizeof(ItemPointerData));
    posting->edge_id =
        make_graphid(opaque->edge_label_id,
                     age_adjacency_load_entry_id48(delta->edge_entry_id));
    posting->next_vertex_id =
        make_graphid(opaque->next_label_id,
                     age_adjacency_load_entry_id48(delta->next_entry_id));
}

static void
age_adjacency_append_item(Relation index_rel, uint16 page_type,
                          const void *item, Size item_size,
                          graphid item_key,
                          int32 key_label_id,
                          int32 edge_label_id,
                          int32 next_label_id,
                          uint32 posting_units,
                          BlockNumber *inserted_blkno,
                          OffsetNumber *inserted_offnum)
{
    Buffer metabuf;
    Buffer databuf = InvalidBuffer;
    Buffer oldbuf = InvalidBuffer;
    Page metapage;
    Page datapage;
    AgeAdjacencyMetaPage meta;
    AgeAdjacencyPageOpaque opaque;
    BlockNumber *first_blkno;
    BlockNumber *last_blkno;
    BlockNumber target_blkno;
    OffsetNumber offnum;
    bool needs_wal = RelationNeedsWAL(index_rel);
    bool metadirty = false;
    bool datadirty = false;
    bool olddirty = false;
    GenericXLogState *state = NULL;

    metabuf = ReadBuffer(index_rel, AGE_ADJACENCY_METAPAGE_BLKNO);
    LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);

    if (needs_wal)
    {
        state = GenericXLogStart(index_rel);
        metapage = GenericXLogRegisterBuffer(state, metabuf, 0);
    }
    else
    {
        metapage = BufferGetPage(metabuf);
    }

    meta = age_adjacency_get_meta(metapage);
    if (meta->magic != AGE_ADJACENCY_MAGIC ||
        meta->version != AGE_ADJACENCY_VERSION)
    {
        ereport(ERROR,
                (errcode(ERRCODE_INDEX_CORRUPTED),
                 errmsg("age_adjacency metapage metadata is invalid")));
    }

    if (page_type == AGE_ADJACENCY_PAGE_DIRECTORY)
    {
        first_blkno = &meta->first_directory_blkno;
        last_blkno = &meta->last_directory_blkno;
    }
    else if (page_type == AGE_ADJACENCY_PAGE_MAIN)
    {
        first_blkno = &meta->first_main_blkno;
        last_blkno = &meta->last_main_blkno;
    }
    else if (page_type == AGE_ADJACENCY_PAGE_DELTA)
    {
        first_blkno = &meta->first_delta_blkno;
        last_blkno = &meta->last_delta_blkno;
    }
    else
    {
        elog(ERROR, "unexpected age_adjacency page type %u", page_type);
    }

    target_blkno = *last_blkno;
    if (BlockNumberIsValid(target_blkno))
    {
        databuf = ReadBuffer(index_rel, target_blkno);
        LockBuffer(databuf, BUFFER_LOCK_EXCLUSIVE);

        if (needs_wal)
        {
            datapage = GenericXLogRegisterBuffer(state, databuf, 0);
        }
        else
        {
            datapage = BufferGetPage(databuf);
        }

        if (PageGetExactFreeSpace(datapage) <
            MAXALIGN(item_size) + sizeof(ItemIdData) ||
            (page_type == AGE_ADJACENCY_PAGE_DELTA &&
             ((AgeAdjacencyPageOpaque) PageGetSpecialPointer(datapage))->
             posting_count > 0 &&
             (((AgeAdjacencyPageOpaque) PageGetSpecialPointer(datapage))->
              key_label_id != key_label_id ||
              ((AgeAdjacencyPageOpaque) PageGetSpecialPointer(datapage))->
              edge_label_id != edge_label_id ||
              ((AgeAdjacencyPageOpaque) PageGetSpecialPointer(datapage))->
              next_label_id != next_label_id)))
        {
            Buffer newbuf;
            Page newpage;
            AgeAdjacencyPageOpaque oldopaque;

            oldopaque = (AgeAdjacencyPageOpaque) PageGetSpecialPointer(datapage);
            oldbuf = databuf;

            newbuf = age_adjacency_new_buffer(index_rel);
            LockBuffer(newbuf, BUFFER_LOCK_EXCLUSIVE);

            oldopaque->next_blkno = BufferGetBlockNumber(newbuf);
            olddirty = true;

            if (needs_wal)
            {
                newpage = GenericXLogRegisterBuffer(state, newbuf,
                                                    GENERIC_XLOG_FULL_IMAGE);
            }
            else
            {
                newpage = BufferGetPage(newbuf);
            }

            age_adjacency_init_page(newpage, page_type);

            databuf = newbuf;
            datapage = newpage;
            target_blkno = BufferGetBlockNumber(databuf);
            *last_blkno = target_blkno;
            metadirty = true;
        }
    }
    else
    {
        databuf = age_adjacency_new_buffer(index_rel);
        LockBuffer(databuf, BUFFER_LOCK_EXCLUSIVE);

        if (needs_wal)
        {
            datapage = GenericXLogRegisterBuffer(state, databuf,
                                                GENERIC_XLOG_FULL_IMAGE);
        }
        else
        {
            datapage = BufferGetPage(databuf);
        }

        age_adjacency_init_page(datapage, page_type);

        target_blkno = BufferGetBlockNumber(databuf);
        *first_blkno = target_blkno;
        *last_blkno = target_blkno;
        metadirty = true;
    }

    offnum = PageAddItem(datapage, (Item) item, item_size,
                         InvalidOffsetNumber, false, false);
    if (offnum == InvalidOffsetNumber)
    {
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("failed to append age_adjacency posting")));
    }

    opaque = (AgeAdjacencyPageOpaque) PageGetSpecialPointer(datapage);
    if (opaque->magic != AGE_ADJACENCY_MAGIC ||
        opaque->version != AGE_ADJACENCY_VERSION ||
        opaque->page_type != page_type)
    {
        ereport(ERROR,
                (errcode(ERRCODE_INDEX_CORRUPTED),
                 errmsg("age_adjacency page is invalid")));
    }

    if (page_type == AGE_ADJACENCY_PAGE_DELTA &&
        opaque->posting_count == 0)
    {
        opaque->flags |= AGE_ADJACENCY_DELTA_PAGE_COMPACT;
        opaque->key_label_id = key_label_id;
        opaque->edge_label_id = edge_label_id;
        opaque->next_label_id = next_label_id;
    }

    if (page_type == AGE_ADJACENCY_PAGE_MAIN ||
        page_type == AGE_ADJACENCY_PAGE_DELTA)
    {
        bool page_was_empty = opaque->posting_count == 0;

        opaque->posting_count += posting_units;
        if (page_was_empty)
        {
            opaque->min_key = item_key;
            opaque->max_key = item_key;
        }
        else
        {
            opaque->min_key = Min(opaque->min_key, item_key);
            opaque->max_key = Max(opaque->max_key, item_key);
        }
        meta->postings += posting_units;
        if (page_type == AGE_ADJACENCY_PAGE_DELTA)
        {
            meta->delta_postings += posting_units;
        }
    }
    else
    {
        AgeAdjacencyDirectoryEntry entry = (AgeAdjacencyDirectoryEntry) item;

        if (meta->directory_entries == 0 || opaque->posting_count == 0)
        {
            opaque->min_key = entry->key;
            opaque->max_key = entry->key;
        }
        else
        {
            opaque->min_key = Min(opaque->min_key, entry->key);
            opaque->max_key = Max(opaque->max_key, entry->key);
        }
        opaque->posting_count++;
        meta->directory_entries++;
    }
    metadirty = true;
    datadirty = true;

    if (inserted_blkno != NULL)
    {
        *inserted_blkno = target_blkno;
    }
    if (inserted_offnum != NULL)
    {
        *inserted_offnum = offnum;
    }

    if (needs_wal)
    {
        GenericXLogFinish(state);
    }
    else
    {
        if (datadirty)
        {
            MarkBufferDirty(databuf);
        }
        if (olddirty)
        {
            MarkBufferDirty(oldbuf);
        }
        if (metadirty)
        {
            MarkBufferDirty(metabuf);
        }
    }

    UnlockReleaseBuffer(databuf);
    if (BufferIsValid(oldbuf))
    {
        UnlockReleaseBuffer(oldbuf);
    }
    UnlockReleaseBuffer(metabuf);
}

static void
age_adjacency_append_main_run_block(Relation index_rel,
                                    AgeAdjacencyPostingData *postings,
                                    Size posting_count,
                                    graphid run_key,
                                    BlockNumber *inserted_blkno,
                                    OffsetNumber *inserted_offnum)
{
    AgeAdjacencyMainRunBlock block;
    Size block_size;
    Size i;
    int32 edge_label_id = INVALID_LABEL_ID;
    int32 next_label_id = INVALID_LABEL_ID;
    uint16 flags = AGE_ADJACENCY_MAIN_BLOCK_FULL;

    Assert(posting_count > 0);
    Assert(posting_count <= AGE_ADJACENCY_MAIN_RUN_BLOCK_FULL_MAX_POSTINGS ||
           posting_count <= AGE_ADJACENCY_MAIN_RUN_BLOCK_COMPACT_MAX_POSTINGS);
    if (age_adjacency_main_run_block_can_compact(postings, posting_count,
                                                 &edge_label_id,
                                                 &next_label_id))
    {
        flags = AGE_ADJACENCY_MAIN_BLOCK_COMPACT;
    }
    Assert(age_adjacency_main_run_block_fits(flags, posting_count));

    block_size = age_adjacency_main_run_block_size(flags, posting_count);
    block = palloc0(block_size);
    block->posting_count = (uint16) posting_count;
    block->flags = flags;
    block->edge_label_id = edge_label_id;
    block->next_label_id = next_label_id;
    block->min_next_vertex_id = postings[0].next_vertex_id;
    block->max_next_vertex_id = postings[0].next_vertex_id;
    for (i = 0; i < posting_count; i++)
    {
        if (postings[i].next_vertex_id < block->min_next_vertex_id)
            block->min_next_vertex_id = postings[i].next_vertex_id;
        if (postings[i].next_vertex_id > block->max_next_vertex_id)
            block->max_next_vertex_id = postings[i].next_vertex_id;
        age_adjacency_main_run_block_add_exact_vertex(
            block, postings[i].next_vertex_id);
        age_adjacency_main_run_block_add_compressed_vertex(
            block, postings[i].next_vertex_id);
        age_adjacency_vertex_bloom256_add(block->next_vertex_bloom,
                                          postings[i].next_vertex_id);
        age_adjacency_value_posting_bloom_add(block->value_posting_bloom,
                                              postings[i].next_vertex_id);
    }

    if ((flags & AGE_ADJACENCY_MAIN_BLOCK_COMPACT) != 0)
    {
        AgeAdjacencyMainCompactPosting compact_postings =
            (AgeAdjacencyMainCompactPosting) block->data;

        for (i = 0; i < posting_count; i++)
        {
            memcpy(&compact_postings[i].heap_tid, &postings[i].heap_tid,
                   sizeof(ItemPointerData));
            age_adjacency_store_entry_id48(
                compact_postings[i].edge_entry_id,
                get_graphid_entry_id(postings[i].edge_id));
            age_adjacency_store_entry_id48(
                compact_postings[i].next_entry_id,
                get_graphid_entry_id(postings[i].next_vertex_id));
        }
    }
    else
    {
        AgeAdjacencyMainPosting full_postings =
            (AgeAdjacencyMainPosting) block->data;

        for (i = 0; i < posting_count; i++)
        {
            ItemPointerCopy(&postings[i].heap_tid,
                            &full_postings[i].heap_tid);
            full_postings[i].edge_id = postings[i].edge_id;
            full_postings[i].next_vertex_id = postings[i].next_vertex_id;
        }
    }

    age_adjacency_append_item(index_rel, AGE_ADJACENCY_PAGE_MAIN,
                              block, block_size, run_key,
                              INVALID_LABEL_ID, INVALID_LABEL_ID,
                              INVALID_LABEL_ID,
                              (uint32) posting_count,
                              inserted_blkno, inserted_offnum);
    pfree(block);
}

static void
age_adjacency_append_delta_posting(Relation index_rel,
                                   AgeAdjacencyPosting posting)
{
    AgeAdjacencyDeltaCompactPostingData delta_posting;

    age_adjacency_form_delta_posting(posting, &delta_posting);
    age_adjacency_append_item(index_rel, AGE_ADJACENCY_PAGE_DELTA,
                              &delta_posting,
                              sizeof(AgeAdjacencyDeltaCompactPostingData),
                              posting->key,
                              get_graphid_label_id(posting->key),
                              get_graphid_label_id(posting->edge_id),
                              get_graphid_label_id(posting->next_vertex_id),
                              1, NULL, NULL);
}

static void
age_adjacency_append_directory_entry(Relation index_rel,
                                     AgeAdjacencyDirectoryEntry entry)
{
    age_adjacency_append_item(index_rel, AGE_ADJACENCY_PAGE_DIRECTORY, entry,
                              sizeof(AgeAdjacencyDirectoryEntryData),
                              entry->key,
                              INVALID_LABEL_ID, INVALID_LABEL_ID,
                              INVALID_LABEL_ID, 1,
                              NULL, NULL);
}

static int
age_adjacency_compare_postings(const void *left, const void *right)
{
    const AgeAdjacencyPostingData *l = left;
    const AgeAdjacencyPostingData *r = right;

    if (l->key < r->key)
    {
        return -1;
    }
    if (l->key > r->key)
    {
        return 1;
    }
    if (get_graphid_label_id(l->next_vertex_id) <
        get_graphid_label_id(r->next_vertex_id))
    {
        return -1;
    }
    if (get_graphid_label_id(l->next_vertex_id) >
        get_graphid_label_id(r->next_vertex_id))
    {
        return 1;
    }
    if (get_graphid_label_id(l->edge_id) < get_graphid_label_id(r->edge_id))
    {
        return -1;
    }
    if (get_graphid_label_id(l->edge_id) > get_graphid_label_id(r->edge_id))
    {
        return 1;
    }
    if (ItemPointerGetBlockNumber(&l->heap_tid) <
        ItemPointerGetBlockNumber(&r->heap_tid))
    {
        return -1;
    }
    if (ItemPointerGetBlockNumber(&l->heap_tid) >
        ItemPointerGetBlockNumber(&r->heap_tid))
    {
        return 1;
    }
    if (ItemPointerGetOffsetNumber(&l->heap_tid) <
        ItemPointerGetOffsetNumber(&r->heap_tid))
    {
        return -1;
    }
    if (ItemPointerGetOffsetNumber(&l->heap_tid) >
        ItemPointerGetOffsetNumber(&r->heap_tid))
    {
        return 1;
    }
    return 0;
}

static void
age_adjacency_build_main_runs(Relation index_rel,
                              AgeAdjacencyBuildState *buildstate)
{
    Size i = 0;
    Size dir_index;

    if (buildstate->count == 0)
    {
        return;
    }

    qsort(buildstate->postings, buildstate->count,
          sizeof(AgeAdjacencyPostingData), age_adjacency_compare_postings);

    /*
     * Write all main postings before directory entries. This keeps directory
     * pages contiguous, which lets lookup binary-search directory page ranges.
     */
    while (i < buildstate->count)
    {
        Size run_start = i;
        Size run_count = 0;
        Size run_index;
        int label_bloom_slot;
        int32 previous_next_label_id = INVALID_LABEL_ID;
        AgeAdjacencyDirectoryEntryData entry;

        memset(&entry, 0, sizeof(entry));
        for (label_bloom_slot = 0;
             label_bloom_slot < AGE_ADJACENCY_DIRECTORY_LABEL_BLOOM_SLOTS;
             label_bloom_slot++)
        {
            entry.next_vertex_bloom_label_id[label_bloom_slot] =
                INVALID_LABEL_ID;
        }
        entry.key = buildstate->postings[i].key;
        entry.min_next_label_id = get_graphid_label_id(
            buildstate->postings[i].next_vertex_id);
        entry.max_next_label_id = entry.min_next_label_id;
        entry.min_next_vertex_id = buildstate->postings[i].next_vertex_id;
        entry.max_next_vertex_id = entry.min_next_vertex_id;

        while (i < buildstate->count &&
               buildstate->postings[i].key == entry.key)
        {
            graphid next_vertex_id = buildstate->postings[i].next_vertex_id;
            int32 next_label_id = get_graphid_label_id(
                next_vertex_id);

            if (label_id_is_valid(next_label_id))
            {
                if (next_label_id != previous_next_label_id)
                {
                    entry.next_label_count++;
                    previous_next_label_id = next_label_id;
                }
                if (!label_id_is_valid(entry.min_next_label_id) ||
                    next_label_id < entry.min_next_label_id)
                    entry.min_next_label_id = next_label_id;
                if (!label_id_is_valid(entry.max_next_label_id) ||
                    next_label_id > entry.max_next_label_id)
                    entry.max_next_label_id = next_label_id;
            }
            if (next_vertex_id < entry.min_next_vertex_id)
                entry.min_next_vertex_id = next_vertex_id;
            if (next_vertex_id > entry.max_next_vertex_id)
                entry.max_next_vertex_id = next_vertex_id;
            entry.next_vertex_bloom =
                age_adjacency_vertex_bloom_add(entry.next_vertex_bloom,
                                               next_vertex_id);
            age_adjacency_vertex_bloom256_add(entry.next_vertex_bloom_wide,
                                              next_vertex_id);
            age_adjacency_value_posting_bloom_add(entry.value_posting_bloom,
                                                  next_vertex_id);
            age_adjacency_directory_entry_add_exact_vertex(&entry,
                                                           next_vertex_id);
            age_adjacency_directory_entry_add_compressed_vertex(&entry,
                                                                next_vertex_id);
            age_adjacency_directory_entry_add_label_bloom(&entry,
                                                          next_label_id,
                                                          next_vertex_id);
            run_count++;
            i++;
        }

        if (run_count > PG_UINT32_MAX)
        {
            ereport(ERROR,
                    (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                     errmsg("too many age_adjacency postings for one key")));
        }

        for (run_index = 0; run_index < run_count;)
        {
            BlockNumber blkno;
            OffsetNumber offnum;
            Size chunk_count;
            uint16 chunk_flags;
            int32 edge_label_id;
            int32 next_label_id;

            edge_label_id =
                get_graphid_label_id(
                    buildstate->postings[run_start + run_index].edge_id);
            next_label_id =
                get_graphid_label_id(
                    buildstate->postings[run_start + run_index].next_vertex_id);
            chunk_flags = label_id_is_valid(edge_label_id) &&
                          label_id_is_valid(next_label_id) ?
                          AGE_ADJACENCY_MAIN_BLOCK_COMPACT :
                          AGE_ADJACENCY_MAIN_BLOCK_FULL;
            chunk_count = 1;
            while (run_index + chunk_count < run_count &&
                   age_adjacency_main_run_block_fits(chunk_flags,
                                                     chunk_count + 1) &&
                   get_graphid_label_id(
                       buildstate->postings[
                           run_start + run_index + chunk_count].edge_id) ==
                   edge_label_id &&
                   get_graphid_label_id(
                       buildstate->postings[
                           run_start + run_index + chunk_count].
                           next_vertex_id) == next_label_id)
            {
                chunk_count++;
            }
            age_adjacency_append_main_run_block(
                index_rel, &buildstate->postings[run_start + run_index],
                chunk_count, entry.key, &blkno, &offnum);
            if (run_index == 0)
            {
                entry.first_blkno = blkno;
                entry.first_offnum = offnum;
            }
            entry.main_block_count++;
            run_index += chunk_count;
        }

        entry.posting_count = (uint32) run_count;
        if (buildstate->directory_count == buildstate->directory_capacity)
        {
            Size new_capacity = buildstate->directory_capacity == 0 ? 1024 :
                                buildstate->directory_capacity * 2;

            if (buildstate->directory_entries == NULL)
            {
                buildstate->directory_entries =
                    palloc_array(AgeAdjacencyDirectoryEntryData,
                                 new_capacity);
            }
            else
            {
                buildstate->directory_entries =
                    repalloc_array(buildstate->directory_entries,
                                   AgeAdjacencyDirectoryEntryData,
                                   new_capacity);
            }
            buildstate->directory_capacity = new_capacity;
        }

        buildstate->directory_entries[buildstate->directory_count++] = entry;
    }

    for (dir_index = 0; dir_index < buildstate->directory_count; dir_index++)
    {
        age_adjacency_append_directory_entry(
            index_rel, &buildstate->directory_entries[dir_index]);
    }
}

static void
age_adjacency_build_callback(Relation index_rel, ItemPointer tid,
                             Datum *values, bool *isnull,
                             bool tuple_is_alive, void *state)
{
    AgeAdjacencyBuildState *buildstate = state;
    AgeAdjacencyPostingData posting;

    if (!tuple_is_alive)
    {
        return;
    }

    age_adjacency_form_posting(index_rel, values, isnull, tid, &posting);
    if (buildstate->count == buildstate->capacity)
    {
        Size new_capacity = buildstate->capacity == 0 ? 1024 :
                            buildstate->capacity * 2;

        if (buildstate->postings == NULL)
        {
            buildstate->postings = palloc_array(AgeAdjacencyPostingData,
                                                new_capacity);
        }
        else
        {
            buildstate->postings = repalloc_array(buildstate->postings,
                                                  AgeAdjacencyPostingData,
                                                  new_capacity);
        }
        buildstate->capacity = new_capacity;
    }

    buildstate->postings[buildstate->count++] = posting;
    buildstate->indtuples++;
}

static int64
age_adjacency_emit_posting(AgeAdjacencyPosting posting,
                           AgeAdjacencyScanTarget *target)
{
    AgeAdjacencyPayload payload;

    if (!age_adjacency_posting_visible_payload(posting, target, &payload))
    {
        return 0;
    }

    if (target != NULL && target->tbm != NULL)
    {
        tbm_add_tuples(target->tbm, &posting->heap_tid, 1, false);
    }

    if (target != NULL && target->tupstore != NULL)
    {
        Datum values[3];
        bool nulls[3] = {false, false, false};

        values[0] = ItemPointerGetDatum(&posting->heap_tid);
        values[1] = GRAPHID_GET_DATUM(posting->edge_id);
        values[2] = GRAPHID_GET_DATUM(posting->next_vertex_id);
        tuplestore_putvalues(target->tupstore, target->tupdesc,
                             values, nulls);
    }

    if (target != NULL && target->callback != NULL)
    {
        if (!target->callback(&payload, target->callback_state))
        {
            return -1;
        }
    }

    return 1;
}

static bool
age_adjacency_posting_visible_payload(AgeAdjacencyPosting posting,
                                      AgeAdjacencyScanTarget *target,
                                      AgeAdjacencyPayload *payload)
{
    Assert(payload != NULL);

    ItemPointerCopy(&posting->heap_tid, &payload->heap_tid);
    payload->edge_id = posting->edge_id;
    payload->next_vertex_id = posting->next_vertex_id;
    payload->properties = (Datum) 0;
    payload->properties_isnull = true;

    if (!age_adjacency_posting_matches_terminal_label(
            target, posting->next_vertex_id))
    {
        return false;
    }
    if (!age_adjacency_posting_matches_terminal_vertex_filter(
            target, posting->next_vertex_id))
    {
        return false;
    }

    if (target != NULL && target->heap_rel != NULL)
    {
        bool fetch_tuple = true;

        if (!target->fetch_properties)
        {
            BlockNumber heap_blk;
            bool all_visible;

            heap_blk = ItemPointerGetBlockNumber(&posting->heap_tid);
            if (target->visibilitymap_cache_valid &&
                target->visibilitymap_cached_block == heap_blk)
            {
                all_visible = target->visibilitymap_cached_all_visible;
            }
            else
            {
                all_visible = VM_ALL_VISIBLE(target->heap_rel, heap_blk,
                                             &target->visibilitymap_buffer);
                target->visibilitymap_cached_block = heap_blk;
                target->visibilitymap_cache_valid = true;
                target->visibilitymap_cached_all_visible = all_visible;
            }

            if (all_visible)
            {
                fetch_tuple = false;
            }
        }

        if (fetch_tuple &&
            !table_tuple_fetch_row_version(target->heap_rel,
                                           &posting->heap_tid,
                                           target->snapshot,
                                           target->slot))
        {
            return false;
        }
    }

    if (target != NULL && target->fetch_properties && target->slot != NULL)
    {
        payload->properties =
            slot_getattr(target->slot, Anum_ag_label_edge_table_properties,
                         &payload->properties_isnull);
    }

    return true;
}

static bool
age_adjacency_posting_matches_terminal_label(AgeAdjacencyScanTarget *target,
                                             graphid next_vertex_id)
{
    if (target == NULL || !label_id_is_valid(target->terminal_label_id))
        return true;

    if (get_graphid_label_id(next_vertex_id) == target->terminal_label_id)
        return true;

    target->terminal_label_filtered++;
    return false;
}

static bool
age_adjacency_posting_matches_terminal_vertex_filter(
    AgeAdjacencyScanTarget *target, graphid next_vertex_id)
{
    if (target == NULL)
        return true;

    if (target->terminal_vertex_set_filter.vertex_ids != NULL)
    {
        if (target->terminal_vertex_set_filter.has_range &&
            (next_vertex_id < target->terminal_vertex_set_filter.min_vertex_id ||
             next_vertex_id > target->terminal_vertex_set_filter.max_vertex_id))
        {
            target->terminal_vertex_set_range_filtered++;
            target->terminal_property_filtered++;
            return false;
        }

        if (target->terminal_vertex_set_filter.has_sorted_vertex_ids)
        {
            if (age_adjacency_vertex_set_sorted_contains(
                    &target->terminal_vertex_set_filter, next_vertex_id))
            {
                return true;
            }

            target->terminal_vertex_set_sorted_filtered++;
            target->terminal_property_filtered++;
            return false;
        }

        if (hash_search(target->terminal_vertex_set_filter.vertex_ids,
                        &next_vertex_id, HASH_FIND, NULL) != NULL)
        {
            return true;
        }

        target->terminal_property_filtered++;
        return false;
    }

    if (target->terminal_vertex_filter == NULL)
        return true;

    if (target->terminal_vertex_filter(next_vertex_id,
                                       target->terminal_vertex_filter_state))
        return true;

    target->terminal_property_filtered++;
    return false;
}

static bool
age_adjacency_vertex_set_sorted_contains(
    const AgeAdjacencyVertexSetFilter *filter, graphid vertex_id)
{
    int64 low = 0;
    int64 high;

    Assert(filter != NULL);
    Assert(filter->has_sorted_vertex_ids);

    high = filter->sorted_vertex_count;
    while (low < high)
    {
        int64 mid = low + ((high - low) / 2);
        graphid candidate = filter->sorted_vertex_ids[mid];

        if (candidate == vertex_id)
            return true;
        if (candidate < vertex_id)
            low = mid + 1;
        else
            high = mid;
    }

    return false;
}

static AgeAdjacencyCacheFilterResult
age_adjacency_posting_matches_cache_filters(AgeAdjacencyPosting posting,
                                            AgeAdjacencyScanTarget *target)
{
    if (!age_adjacency_posting_matches_terminal_label(
            target, posting->next_vertex_id))
    {
        return AGE_ADJACENCY_CACHE_FILTER_LABEL;
    }
    if (!age_adjacency_posting_matches_terminal_vertex_filter(
            target, posting->next_vertex_id))
    {
        return AGE_ADJACENCY_CACHE_FILTER_PROPERTY;
    }

    return AGE_ADJACENCY_CACHE_FILTER_PASS;
}

static bool
age_adjacency_delta_page_may_contain_key(AgeAdjacencyPageOpaque opaque,
                                         graphid key)
{
    Assert(opaque != NULL);
    Assert(opaque->page_type == AGE_ADJACENCY_PAGE_DELTA);

    if (opaque->posting_count == 0)
    {
        return false;
    }

    return key >= opaque->min_key && key <= opaque->max_key;
}

static bool
age_adjacency_store_candidate(const AgeAdjacencyPayload *payload,
                              void *callback_state)
{
    AgeAdjacencyCandidateStore *store = callback_state;
    Datum values[2];
    bool nulls[2] = {false, false};

    values[0] = GRAPHID_GET_DATUM(payload->edge_id);
    values[1] = GRAPHID_GET_DATUM(payload->next_vertex_id);
    tuplestore_putvalues(store->tupstore, store->tupdesc, values, nulls);

    return true;
}

static bool
age_adjacency_store_candidate_row(const AgeAdjacencyPayload *payload,
                                  void *callback_state)
{
    AgeAdjacencyCandidateRowStore *store = callback_state;
    Datum values[4];
    bool nulls[4] = {false, false, false, false};

    values[0] = GRAPHID_GET_DATUM(payload->edge_id);
    if (store->outgoing)
    {
        values[1] = GRAPHID_GET_DATUM(store->source_vertex_id);
        values[2] = GRAPHID_GET_DATUM(payload->next_vertex_id);
    }
    else
    {
        values[1] = GRAPHID_GET_DATUM(payload->next_vertex_id);
        values[2] = GRAPHID_GET_DATUM(store->source_vertex_id);
    }
    values[3] = payload->properties;
    nulls[3] = payload->properties_isnull;

    tuplestore_putvalues(store->tupstore, store->tupdesc, values, nulls);

    return true;
}

static int64
age_adjacency_scan_posting_run(Relation index_rel, BlockNumber blkno,
                               OffsetNumber offnum, uint32 posting_count,
                               graphid key, AgeAdjacencyScanTarget *target)
{
    int64 matches = 0;
    uint32 remaining = posting_count;

    while (remaining > 0 && BlockNumberIsValid(blkno))
    {
        Buffer buf;
        Page page;
        AgeAdjacencyPageOpaque opaque;
        OffsetNumber maxoff;

        buf = ReadBuffer(index_rel, blkno);
        LockBuffer(buf, BUFFER_LOCK_SHARE);
        page = BufferGetPage(buf);

        opaque = (AgeAdjacencyPageOpaque) PageGetSpecialPointer(page);
        if (opaque->magic != AGE_ADJACENCY_MAGIC ||
            opaque->version != AGE_ADJACENCY_VERSION ||
            opaque->page_type != AGE_ADJACENCY_PAGE_MAIN)
        {
            ereport(ERROR,
                    (errcode(ERRCODE_INDEX_CORRUPTED),
                     errmsg("age_adjacency main page %u is invalid", blkno)));
        }

        maxoff = PageGetMaxOffsetNumber(page);
        for (;
             offnum <= maxoff && remaining > 0;
             offnum = OffsetNumberNext(offnum))
        {
            ItemId item_id = PageGetItemId(page, offnum);
            AgeAdjacencyMainRunBlock block;
            uint16 i;

            if (!ItemIdIsNormal(item_id))
            {
                continue;
            }

            block = (AgeAdjacencyMainRunBlock) PageGetItem(page, item_id);
            if (target != NULL &&
                !age_adjacency_main_run_block_may_match_vertex_range(
                    block, &target->terminal_vertex_set_filter))
            {
                uint32 skipped = Min((uint32)block->posting_count, remaining);

                target->terminal_property_filtered += skipped;
                target->terminal_vertex_set_block_filtered += skipped;
                if (target->terminal_vertex_set_filter.has_value_summary)
                    target->terminal_vertex_set_block_value_filtered += skipped;
                target->terminal_vertex_set_block_range_filtered += skipped;
                if (target->has_terminal_property_summary)
                    target->terminal_composite_block_filtered += skipped;
                remaining -= skipped;
                continue;
            }
            if (target != NULL &&
                !age_adjacency_main_run_block_may_match_exact_vertex(
                    block, &target->terminal_vertex_set_filter))
            {
                uint32 skipped = Min((uint32)block->posting_count, remaining);

                target->terminal_property_filtered += skipped;
                target->terminal_vertex_set_block_filtered += skipped;
                if (target->terminal_vertex_set_filter.has_value_summary)
                    target->terminal_vertex_set_block_value_filtered += skipped;
                target->terminal_vertex_set_block_exact_filtered += skipped;
                if (target->has_terminal_property_summary)
                    target->terminal_composite_block_filtered += skipped;
                remaining -= skipped;
                continue;
            }
            if (target != NULL &&
                !age_adjacency_main_run_block_may_match_compressed_vertex(
                    block, &target->terminal_vertex_set_filter))
            {
                uint32 skipped = Min((uint32)block->posting_count, remaining);

                target->terminal_property_filtered += skipped;
                target->terminal_vertex_set_block_filtered += skipped;
                if (target->terminal_vertex_set_filter.has_value_summary)
                    target->terminal_vertex_set_block_value_filtered += skipped;
                target->terminal_vertex_set_block_compressed_filtered += skipped;
                if (target->has_terminal_property_summary)
                    target->terminal_composite_block_filtered += skipped;
                remaining -= skipped;
                continue;
            }
            if (target != NULL &&
                !age_adjacency_main_run_block_may_match_vertex_bloom(
                    block, &target->terminal_vertex_set_filter))
            {
                uint32 skipped = Min((uint32)block->posting_count, remaining);

                target->terminal_property_filtered += skipped;
                target->terminal_vertex_set_block_filtered += skipped;
                if (target->terminal_vertex_set_filter.has_value_summary)
                    target->terminal_vertex_set_block_value_filtered += skipped;
                target->terminal_vertex_set_block_bloom_filtered += skipped;
                if (target->has_terminal_property_summary)
                    target->terminal_composite_block_filtered += skipped;
                remaining -= skipped;
                continue;
            }
            if (target != NULL &&
                !age_adjacency_main_run_block_may_match_value_posting(
                    block, &target->terminal_vertex_set_filter))
            {
                uint32 skipped = Min((uint32)block->posting_count, remaining);

                target->terminal_property_filtered += skipped;
                target->terminal_vertex_set_block_filtered += skipped;
                target->terminal_vertex_set_block_value_filtered += skipped;
                target->terminal_vertex_set_block_value_posting_filtered +=
                    skipped;
                if (target->has_terminal_property_summary)
                    target->terminal_composite_block_filtered += skipped;
                remaining -= skipped;
                continue;
            }
            if (target != NULL &&
                !age_adjacency_main_run_block_may_match_compact_postings(
                    block, 0, &target->terminal_vertex_set_filter))
            {
                uint32 skipped = Min((uint32)block->posting_count, remaining);

                target->terminal_property_filtered += skipped;
                target->terminal_vertex_set_block_filtered += skipped;
                if (target->terminal_vertex_set_filter.has_value_summary)
                    target->terminal_vertex_set_block_value_filtered += skipped;
                target->terminal_vertex_set_block_posting_filtered += skipped;
                if (target->has_terminal_property_summary)
                    target->terminal_composite_block_filtered += skipped;
                remaining -= skipped;
                continue;
            }
            for (i = 0; i < block->posting_count && remaining > 0; i++)
            {
                AgeAdjacencyPostingData posting;
                int64 emitted;

                remaining--;
                age_adjacency_main_run_block_get_posting(block, i, key,
                                                         &posting);

                emitted = age_adjacency_emit_posting(&posting, target);
                if (emitted < 0)
                {
                    UnlockReleaseBuffer(buf);
                    return matches;
                }
                matches += emitted;
            }
        }

        blkno = opaque->next_blkno;
        offnum = FirstOffsetNumber;
        UnlockReleaseBuffer(buf);
    }

    return matches;
}

static void
age_adjacency_load_main_cache(AgeAdjacencyVisiblePayloadScan *scan,
                              BlockNumber blkno)
{
    Buffer buf;
    Page page;
    AgeAdjacencyPageOpaque opaque;
    OffsetNumber offnum;
    OffsetNumber maxoff;
    OffsetNumber window_end;

    if (scan->main_cache.valid && scan->main_cache.blkno == blkno)
    {
        return;
    }

    scan->main_cache.valid = false;
    scan->main_cache.blkno = blkno;
    scan->main_cache.next_blkno = InvalidBlockNumber;
    scan->main_cache.next_offnum = InvalidOffsetNumber;
    scan->main_cache.next_posting_index = 0;
    scan->main_cache.maxoff = InvalidOffsetNumber;
    scan->main_cache.count = 0;

    buf = ReadBuffer(scan->index_rel, blkno);
    LockBuffer(buf, BUFFER_LOCK_SHARE);
    page = BufferGetPage(buf);

    opaque = (AgeAdjacencyPageOpaque) PageGetSpecialPointer(page);
    if (opaque->magic != AGE_ADJACENCY_MAGIC ||
        opaque->version != AGE_ADJACENCY_VERSION ||
        opaque->page_type != AGE_ADJACENCY_PAGE_MAIN)
    {
        ereport(ERROR,
                (errcode(ERRCODE_INDEX_CORRUPTED),
                 errmsg("age_adjacency main page %u is invalid", blkno)));
    }

    maxoff = PageGetMaxOffsetNumber(page);
    scan->main_cache.maxoff = maxoff;
    window_end = maxoff;

    for (offnum = scan->main_offnum;
         offnum <= window_end;
         offnum = OffsetNumberNext(offnum))
    {
        ItemId item_id = PageGetItemId(page, offnum);
        AgeAdjacencyMainRunBlock block;
        uint16 posting_index;

        if (!ItemIdIsNormal(item_id))
        {
            continue;
        }

        block = (AgeAdjacencyMainRunBlock) PageGetItem(page, item_id);
        posting_index = offnum == scan->main_offnum ?
                        scan->main_posting_index : 0;
        if (!age_adjacency_main_run_block_matches_terminal_label(
                block, scan->target.terminal_label_id))
        {
            uint32 skipped = Min((uint32)(block->posting_count -
                                          posting_index),
                                  scan->main_remaining);

            scan->target.terminal_label_filtered += skipped;
            scan->target.terminal_cache_filtered += skipped;
            scan->target.terminal_cache_label_filtered += skipped;
            scan->main_remaining -= skipped;
            if (scan->main_remaining == 0)
                break;
            continue;
        }
        if (!age_adjacency_main_run_block_may_match_vertex_range(
                block, &scan->target.terminal_vertex_set_filter))
        {
            uint32 skipped = Min((uint32)(block->posting_count -
                                          posting_index),
                                  scan->main_remaining);

            scan->target.terminal_property_filtered += skipped;
            scan->target.terminal_cache_filtered += skipped;
            scan->target.terminal_cache_property_filtered += skipped;
            scan->target.terminal_vertex_set_block_filtered += skipped;
            if (scan->target.terminal_vertex_set_filter.has_value_summary)
                scan->target.terminal_vertex_set_block_value_filtered += skipped;
            scan->target.terminal_vertex_set_block_range_filtered += skipped;
            if (scan->target.has_terminal_property_summary)
                scan->target.terminal_composite_block_filtered += skipped;
            scan->main_remaining -= skipped;
            if (scan->main_remaining == 0)
                break;
            continue;
        }
        if (!age_adjacency_main_run_block_may_match_exact_vertex(
                block, &scan->target.terminal_vertex_set_filter))
        {
            uint32 skipped = Min((uint32)(block->posting_count -
                                          posting_index),
                                  scan->main_remaining);

            scan->target.terminal_property_filtered += skipped;
            scan->target.terminal_cache_filtered += skipped;
            scan->target.terminal_cache_property_filtered += skipped;
            scan->target.terminal_vertex_set_block_filtered += skipped;
            if (scan->target.terminal_vertex_set_filter.has_value_summary)
                scan->target.terminal_vertex_set_block_value_filtered += skipped;
            scan->target.terminal_vertex_set_block_exact_filtered += skipped;
            if (scan->target.has_terminal_property_summary)
                scan->target.terminal_composite_block_filtered += skipped;
            scan->main_remaining -= skipped;
            if (scan->main_remaining == 0)
                break;
            continue;
        }
        if (!age_adjacency_main_run_block_may_match_compressed_vertex(
                block, &scan->target.terminal_vertex_set_filter))
        {
            uint32 skipped = Min((uint32)(block->posting_count -
                                          posting_index),
                                  scan->main_remaining);

            scan->target.terminal_property_filtered += skipped;
            scan->target.terminal_cache_filtered += skipped;
            scan->target.terminal_cache_property_filtered += skipped;
            scan->target.terminal_vertex_set_block_filtered += skipped;
            if (scan->target.terminal_vertex_set_filter.has_value_summary)
                scan->target.terminal_vertex_set_block_value_filtered += skipped;
            scan->target.terminal_vertex_set_block_compressed_filtered += skipped;
            if (scan->target.has_terminal_property_summary)
                scan->target.terminal_composite_block_filtered += skipped;
            scan->main_remaining -= skipped;
            if (scan->main_remaining == 0)
                break;
            continue;
        }
        if (!age_adjacency_main_run_block_may_match_vertex_bloom(
                block, &scan->target.terminal_vertex_set_filter))
        {
            uint32 skipped = Min((uint32)(block->posting_count -
                                          posting_index),
                                  scan->main_remaining);

            scan->target.terminal_property_filtered += skipped;
            scan->target.terminal_cache_filtered += skipped;
            scan->target.terminal_cache_property_filtered += skipped;
            scan->target.terminal_vertex_set_block_filtered += skipped;
            if (scan->target.terminal_vertex_set_filter.has_value_summary)
                scan->target.terminal_vertex_set_block_value_filtered += skipped;
            scan->target.terminal_vertex_set_block_bloom_filtered += skipped;
            if (scan->target.has_terminal_property_summary)
                scan->target.terminal_composite_block_filtered += skipped;
            scan->main_remaining -= skipped;
            if (scan->main_remaining == 0)
                break;
            continue;
        }
        if (!age_adjacency_main_run_block_may_match_value_posting(
                block, &scan->target.terminal_vertex_set_filter))
        {
            uint32 skipped = Min((uint32)(block->posting_count -
                                          posting_index),
                                  scan->main_remaining);

            scan->target.terminal_property_filtered += skipped;
            scan->target.terminal_cache_filtered += skipped;
            scan->target.terminal_cache_property_filtered += skipped;
            scan->target.terminal_vertex_set_block_filtered += skipped;
            scan->target.terminal_vertex_set_block_value_filtered += skipped;
            scan->target.terminal_vertex_set_block_value_posting_filtered +=
                skipped;
            if (scan->target.has_terminal_property_summary)
                scan->target.terminal_composite_block_filtered += skipped;
            scan->main_remaining -= skipped;
            if (scan->main_remaining == 0)
                break;
            continue;
        }
        if (!age_adjacency_main_run_block_may_match_compact_postings(
                block, posting_index,
                &scan->target.terminal_vertex_set_filter))
        {
            uint32 skipped = Min((uint32)(block->posting_count -
                                          posting_index),
                                  scan->main_remaining);

            scan->target.terminal_property_filtered += skipped;
            scan->target.terminal_cache_filtered += skipped;
            scan->target.terminal_cache_property_filtered += skipped;
            scan->target.terminal_vertex_set_block_filtered += skipped;
            if (scan->target.terminal_vertex_set_filter.has_value_summary)
                scan->target.terminal_vertex_set_block_value_filtered += skipped;
            scan->target.terminal_vertex_set_block_posting_filtered += skipped;
            if (scan->target.has_terminal_property_summary)
                scan->target.terminal_composite_block_filtered += skipped;
            scan->main_remaining -= skipped;
            if (scan->main_remaining == 0)
                break;
            continue;
        }
        while (posting_index < block->posting_count &&
               scan->main_cache.count < scan->main_remaining)
        {
            AgeAdjacencyCachedPosting *cached;
            AgeAdjacencyPostingData posting;
            AgeAdjacencyCacheFilterResult filter_result;

            age_adjacency_main_run_block_get_posting(block, posting_index,
                                                     scan->active_key,
                                                     &posting);
            posting_index++;
            filter_result = age_adjacency_posting_matches_cache_filters(
                &posting, &scan->target);
            if (filter_result != AGE_ADJACENCY_CACHE_FILTER_PASS)
            {
                scan->target.terminal_cache_filtered++;
                if (filter_result == AGE_ADJACENCY_CACHE_FILTER_LABEL)
                    scan->target.terminal_cache_label_filtered++;
                else if (filter_result == AGE_ADJACENCY_CACHE_FILTER_PROPERTY)
                    scan->target.terminal_cache_property_filtered++;
                scan->main_remaining--;
                if (scan->main_remaining == 0)
                    break;
                continue;
            }

            if (scan->main_cache.count == scan->main_cache.capacity)
            {
                int new_capacity = scan->main_cache.capacity == 0 ? 64 :
                                   scan->main_cache.capacity * 2;

                if (scan->main_cache.postings == NULL)
                {
                    scan->main_cache.postings =
                        palloc_array(AgeAdjacencyCachedPosting, new_capacity);
                }
                else
                {
                    scan->main_cache.postings =
                        repalloc_array(scan->main_cache.postings,
                                       AgeAdjacencyCachedPosting,
                                       new_capacity);
                }
                scan->main_cache.capacity = new_capacity;
            }

            cached = &scan->main_cache.postings[scan->main_cache.count++];
            ItemPointerCopy(&posting.heap_tid, &cached->posting.heap_tid);
            cached->posting.edge_id = posting.edge_id;
            cached->posting.next_vertex_id = posting.next_vertex_id;
        }

        if (scan->main_cache.count >= scan->main_remaining)
        {
            if (posting_index < block->posting_count)
            {
                scan->main_cache.next_blkno = blkno;
                scan->main_cache.next_offnum = offnum;
                scan->main_cache.next_posting_index = posting_index;
            }
            else
            {
                scan->main_cache.next_blkno = opaque->next_blkno;
                scan->main_cache.next_offnum = FirstOffsetNumber;
                scan->main_cache.next_posting_index = 0;
            }
            scan->main_cache.valid = true;
            UnlockReleaseBuffer(buf);
            return;
        }
    }

    scan->main_cache.next_blkno = opaque->next_blkno;
    scan->main_cache.next_offnum = FirstOffsetNumber;
    scan->main_cache.next_posting_index = 0;
    scan->main_cache.valid = true;
    UnlockReleaseBuffer(buf);
}

static bool
age_adjacency_search_directory(Relation index_rel, graphid key,
                               AgeAdjacencyDirectoryEntryData *entry_out,
                               int64 *pages_visited,
                               int64 *entries_scanned)
{
    AgeAdjacencyMetaPageData meta;

    age_adjacency_read_meta(index_rel, &meta);
    return age_adjacency_search_directory_with_meta(index_rel, key, &meta,
                                                    entry_out, pages_visited,
                                                    entries_scanned);
}

static bool
age_adjacency_search_directory_with_meta(
    Relation index_rel, graphid key, const AgeAdjacencyMetaPageData *meta,
    AgeAdjacencyDirectoryEntryData *entry_out, int64 *pages_visited,
    int64 *entries_scanned)
{
    int64 low;
    int64 high;

    if (!BlockNumberIsValid(meta->first_directory_blkno) ||
        !BlockNumberIsValid(meta->last_directory_blkno))
    {
        return false;
    }

    low = meta->first_directory_blkno;
    high = meta->last_directory_blkno;

    while (low <= high)
    {
        Buffer buf;
        Page page;
        AgeAdjacencyPageOpaque opaque;
        OffsetNumber offnum;
        OffsetNumber maxoff;
        BlockNumber blkno = (BlockNumber) (low + (high - low) / 2);

        buf = ReadBuffer(index_rel, blkno);
        LockBuffer(buf, BUFFER_LOCK_SHARE);
        page = BufferGetPage(buf);
        if (pages_visited != NULL)
        {
            (*pages_visited)++;
        }

        opaque = (AgeAdjacencyPageOpaque) PageGetSpecialPointer(page);
        if (opaque->magic != AGE_ADJACENCY_MAGIC ||
            opaque->version != AGE_ADJACENCY_VERSION ||
            opaque->page_type != AGE_ADJACENCY_PAGE_DIRECTORY)
        {
            ereport(ERROR,
                    (errcode(ERRCODE_INDEX_CORRUPTED),
                     errmsg("age_adjacency directory page %u is invalid",
                            blkno)));
        }

        if (key < opaque->min_key)
        {
            UnlockReleaseBuffer(buf);
            high = (int64) blkno - 1;
            continue;
        }
        if (key > opaque->max_key)
        {
            UnlockReleaseBuffer(buf);
            low = (int64) blkno + 1;
            continue;
        }

        maxoff = PageGetMaxOffsetNumber(page);
        for (offnum = FirstOffsetNumber;
             offnum <= maxoff;
             offnum = OffsetNumberNext(offnum))
        {
            ItemId item_id = PageGetItemId(page, offnum);
            AgeAdjacencyDirectoryEntry entry;

            if (!ItemIdIsNormal(item_id))
            {
                continue;
            }
            if (entries_scanned != NULL)
            {
                (*entries_scanned)++;
            }

            entry = (AgeAdjacencyDirectoryEntry) PageGetItem(page, item_id);
            if (entry->key == key)
            {
                memcpy(entry_out, entry,
                       sizeof(AgeAdjacencyDirectoryEntryData));
                UnlockReleaseBuffer(buf);
                return true;
            }
        }

        UnlockReleaseBuffer(buf);
        return false;
    }

    return false;
}

static bool
age_adjacency_find_directory_entry_with_meta(
    Relation index_rel, graphid key, const AgeAdjacencyMetaPageData *meta,
    AgeAdjacencyDirectoryEntryData *entry_out)
{
    return age_adjacency_search_directory_with_meta(index_rel, key, meta,
                                                    entry_out, NULL, NULL);
}

bool
age_adjacency_estimate_terminal_label_postings(
    Oid index_oid, graphid key, int32 terminal_label_id,
    int64 *run_postings, int64 *terminal_postings, int64 *label_groups,
    const char **value_posting_source, int64 *main_blocks)
{
    Relation index_rel;
    AgeAdjacencyMetaPageData meta;
    AgeAdjacencyDirectoryEntryData entry;
    bool found;

    if (run_postings != NULL)
        *run_postings = 0;
    if (terminal_postings != NULL)
        *terminal_postings = 0;
    if (label_groups != NULL)
        *label_groups = 0;
    if (value_posting_source != NULL)
        *value_posting_source = "none";
    if (main_blocks != NULL)
        *main_blocks = 0;
    if (!OidIsValid(index_oid))
        return false;

    index_rel = index_open(index_oid, AccessShareLock);
    age_adjacency_read_meta(index_rel, &meta);
    found = age_adjacency_find_directory_entry_with_meta(index_rel, key,
                                                         &meta, &entry);
    if (found)
    {
        if (run_postings != NULL)
            *run_postings = entry.posting_count;
        if (terminal_postings != NULL)
        {
            *terminal_postings =
                age_adjacency_directory_entry_terminal_label_postings(
                    &entry, terminal_label_id);
        }
        if (label_groups != NULL)
            *label_groups = entry.next_label_count;
        if (value_posting_source != NULL)
            *value_posting_source =
                age_adjacency_directory_entry_value_posting_source(
                    &entry, terminal_label_id);
        if (main_blocks != NULL)
            *main_blocks = entry.main_block_count;
    }
    index_close(index_rel, AccessShareLock);

    return found;
}

bool
age_adjacency_estimate_terminal_label_postings_batch(
    Oid index_oid, int32 terminal_label_id,
    AgeAdjacencyTerminalLabelPostingEstimate *estimates,
    int64 estimate_count)
{
    Relation index_rel;
    AgeAdjacencyMetaPageData meta;
    int64 i;

    if (estimates == NULL || estimate_count <= 0)
        return false;
    for (i = 0; i < estimate_count; i++)
    {
        estimates[i].run_postings = 0;
        estimates[i].terminal_postings = 0;
        estimates[i].label_groups = 0;
        estimates[i].value_posting_source = "none";
        estimates[i].main_blocks = 0;
        estimates[i].composite_matches = true;
        estimates[i].label_mismatch = false;
        estimates[i].property_mismatch = false;
        estimates[i].found = false;
    }
    if (!OidIsValid(index_oid))
        return false;

    index_rel = index_open(index_oid, AccessShareLock);
    age_adjacency_read_meta(index_rel, &meta);
    for (i = 0; i < estimate_count; i++)
    {
        AgeAdjacencyDirectoryEntryData entry;

        if (!age_adjacency_find_directory_entry_with_meta(
                index_rel, estimates[i].key, &meta, &entry))
            continue;

        estimates[i].found = true;
        estimates[i].run_postings = entry.posting_count;
        estimates[i].terminal_postings =
            age_adjacency_directory_entry_terminal_label_postings(
                &entry, terminal_label_id);
        estimates[i].label_groups = entry.next_label_count;
        estimates[i].value_posting_source =
            age_adjacency_directory_entry_value_posting_source(
                &entry, terminal_label_id);
        estimates[i].main_blocks = entry.main_block_count;
    }
    index_close(index_rel, AccessShareLock);

    return true;
}

bool
age_adjacency_estimate_composite_terminal_postings_batch(
    Oid index_oid, const AgeAdjacencyCompositeTerminalFilter *filter,
    AgeAdjacencyTerminalLabelPostingEstimate *estimates,
    int64 estimate_count)
{
    Relation index_rel;
    AgeAdjacencyMetaPageData meta;
    AgeAdjacencyScanTarget target;
    int64 i;

    if (filter == NULL || estimates == NULL || estimate_count <= 0)
        return false;
    for (i = 0; i < estimate_count; i++)
    {
        estimates[i].run_postings = 0;
        estimates[i].terminal_postings = 0;
        estimates[i].label_groups = 0;
        estimates[i].value_posting_source = "none";
        estimates[i].main_blocks = 0;
        estimates[i].composite_matches = true;
        estimates[i].label_mismatch = false;
        estimates[i].property_mismatch = false;
        estimates[i].found = false;
    }
    if (!OidIsValid(index_oid))
        return false;

    memset(&target, 0, sizeof(target));
    target.terminal_label_id = filter->terminal_label_id;
    target.terminal_property_index_oid = filter->property_index_oid;
    target.terminal_property_filter_id = filter->property_filter_id;
    target.terminal_property_match_count = filter->property_match_count;
    target.has_terminal_property_summary = filter->has_property_summary;
    target.terminal_vertex_filter = filter->vertex_filter;
    target.terminal_vertex_filter_state = filter->vertex_filter_state;
    if (filter->has_vertex_set_filter)
        target.terminal_vertex_set_filter = filter->vertex_set_filter;

    index_rel = index_open(index_oid, AccessShareLock);
    age_adjacency_read_meta(index_rel, &meta);
    for (i = 0; i < estimate_count; i++)
    {
        AgeAdjacencyDirectoryEntryData entry;

        if (!age_adjacency_find_directory_entry_with_meta(
                index_rel, estimates[i].key, &meta, &entry))
            continue;

        estimates[i].found = true;
        estimates[i].run_postings = entry.posting_count;
        estimates[i].terminal_postings =
            age_adjacency_directory_entry_terminal_label_postings(
                &entry, filter->terminal_label_id);
        estimates[i].label_groups = entry.next_label_count;
        estimates[i].value_posting_source =
            age_adjacency_directory_entry_value_posting_source(
                &entry, filter->terminal_label_id);
        estimates[i].main_blocks = entry.main_block_count;
        estimates[i].composite_matches =
            age_adjacency_directory_entry_matches_composite_target(
                &entry, &target, &estimates[i].label_mismatch,
                &estimates[i].property_mismatch);
    }
    index_close(index_rel, AccessShareLock);

    return true;
}

static void
age_adjacency_load_directory_cache(AgeAdjacencyVisiblePayloadScan *scan,
                                   BlockNumber blkno)
{
    Buffer buf;
    Page page;
    AgeAdjacencyPageOpaque opaque;
    OffsetNumber offnum;
    OffsetNumber maxoff;

    if (scan->directory_cache.valid && scan->directory_cache.blkno == blkno)
    {
        return;
    }

    scan->directory_cache.valid = false;
    scan->directory_cache.blkno = blkno;
    scan->directory_cache.count = 0;

    buf = ReadBuffer(scan->index_rel, blkno);
    LockBuffer(buf, BUFFER_LOCK_SHARE);
    page = BufferGetPage(buf);

    opaque = (AgeAdjacencyPageOpaque) PageGetSpecialPointer(page);
    if (opaque->magic != AGE_ADJACENCY_MAGIC ||
        opaque->version != AGE_ADJACENCY_VERSION ||
        opaque->page_type != AGE_ADJACENCY_PAGE_DIRECTORY)
    {
        ereport(ERROR,
                (errcode(ERRCODE_INDEX_CORRUPTED),
                 errmsg("age_adjacency directory page %u is invalid",
                        blkno)));
    }

    scan->directory_cache.min_key = opaque->min_key;
    scan->directory_cache.max_key = opaque->max_key;
    maxoff = PageGetMaxOffsetNumber(page);
    for (offnum = FirstOffsetNumber;
         offnum <= maxoff;
         offnum = OffsetNumberNext(offnum))
    {
        ItemId item_id = PageGetItemId(page, offnum);
        AgeAdjacencyDirectoryEntry entry;

        if (!ItemIdIsNormal(item_id))
        {
            continue;
        }

        if (scan->directory_cache.count == scan->directory_cache.capacity)
        {
            int new_capacity = scan->directory_cache.capacity == 0 ? 64 :
                               scan->directory_cache.capacity * 2;

            if (scan->directory_cache.entries == NULL)
            {
                scan->directory_cache.entries =
                    palloc_array(AgeAdjacencyDirectoryEntryData,
                                 new_capacity);
            }
            else
            {
                scan->directory_cache.entries =
                    repalloc_array(scan->directory_cache.entries,
                                   AgeAdjacencyDirectoryEntryData,
                                   new_capacity);
            }
            scan->directory_cache.capacity = new_capacity;
        }

        entry = (AgeAdjacencyDirectoryEntry) PageGetItem(page, item_id);
        scan->directory_cache.entries[scan->directory_cache.count++] = *entry;
    }

    scan->directory_cache.valid = true;
    UnlockReleaseBuffer(buf);
}

static bool
age_adjacency_find_directory_entry_cached(
    AgeAdjacencyVisiblePayloadScan *scan, graphid key,
    AgeAdjacencyDirectoryEntryData *entry_out)
{
    int64 low;
    int64 high;

    if (!BlockNumberIsValid(scan->meta.first_directory_blkno) ||
        !BlockNumberIsValid(scan->meta.last_directory_blkno))
    {
        return false;
    }

    if (scan->directory_cache.valid &&
        key >= scan->directory_cache.min_key &&
        key <= scan->directory_cache.max_key)
    {
        int i;

        for (i = 0; i < scan->directory_cache.count; i++)
        {
            if (scan->directory_cache.entries[i].key == key)
            {
                *entry_out = scan->directory_cache.entries[i];
                return true;
            }
        }
        return false;
    }

    low = scan->meta.first_directory_blkno;
    high = scan->meta.last_directory_blkno;
    while (low <= high)
    {
        BlockNumber blkno = (BlockNumber) (low + (high - low) / 2);

        age_adjacency_load_directory_cache(scan, blkno);
        if (key < scan->directory_cache.min_key)
        {
            high = (int64) blkno - 1;
            continue;
        }
        if (key > scan->directory_cache.max_key)
        {
            low = (int64) blkno + 1;
            continue;
        }

        return age_adjacency_find_directory_entry_cached(scan, key,
                                                         entry_out);
    }

    return false;
}

static void
age_adjacency_probe_directory(Relation index_rel, graphid key, bool *found,
                              int64 *pages_visited, int64 *entries_scanned)
{
    AgeAdjacencyDirectoryEntryData entry;

    *pages_visited = 0;
    *entries_scanned = 0;
    *found = age_adjacency_search_directory(index_rel, key, &entry,
                                            pages_visited, entries_scanned);
}

static void
age_adjacency_probe_main_run(Relation index_rel, graphid key,
                             const AgeAdjacencyMetaPageData *meta,
                             AgeAdjacencyMainProbeStats *stats)
{
    AgeAdjacencyDirectoryEntryData entry;
    BlockNumber blkno;
    OffsetNumber offnum;
    uint32 remaining;

    Assert(meta != NULL);
    Assert(stats != NULL);

    memset(stats, 0, sizeof(*stats));
    stats->found = age_adjacency_find_directory_entry_with_meta(
        index_rel, key, meta, &entry);
    if (!stats->found)
    {
        return;
    }

    stats->label_groups = entry.next_label_count;
    blkno = entry.first_blkno;
    offnum = entry.first_offnum;
    remaining = entry.posting_count;
    while (remaining > 0 && BlockNumberIsValid(blkno))
    {
        Buffer buf;
        Page page;
        AgeAdjacencyPageOpaque opaque;
        BlockNumber next_blkno;
        OffsetNumber maxoff;
        OffsetNumber current;
        bool in_window = false;

        buf = ReadBuffer(index_rel, blkno);
        LockBuffer(buf, BUFFER_LOCK_SHARE);
        page = BufferGetPage(buf);

        opaque = (AgeAdjacencyPageOpaque) PageGetSpecialPointer(page);
        if (opaque->magic != AGE_ADJACENCY_MAGIC ||
            opaque->version != AGE_ADJACENCY_VERSION ||
            opaque->page_type != AGE_ADJACENCY_PAGE_MAIN)
        {
            ereport(ERROR,
                    (errcode(ERRCODE_INDEX_CORRUPTED),
                     errmsg("age_adjacency main page %u is invalid", blkno)));
        }

        stats->pages_visited++;
        next_blkno = opaque->next_blkno;
        maxoff = PageGetMaxOffsetNumber(page);

        if (offnum > maxoff)
        {
            blkno = next_blkno;
            offnum = FirstOffsetNumber;
            UnlockReleaseBuffer(buf);
            continue;
        }

        for (current = FirstOffsetNumber;
             current <= maxoff;
             current = OffsetNumberNext(current))
        {
            ItemId item_id = PageGetItemId(page, current);
            AgeAdjacencyMainRunBlock block;
            uint16 posting_index = 0;

            if (!ItemIdIsNormal(item_id))
            {
                continue;
            }

            block = (AgeAdjacencyMainRunBlock) PageGetItem(page, item_id);
            stats->block_items++;
            if ((block->flags & AGE_ADJACENCY_MAIN_BLOCK_COMPACT) != 0)
            {
                stats->compact_block_items++;
            }
            else
            {
                stats->full_block_items++;
            }
            stats->page_offsets += block->posting_count;
            if (current == offnum)
            {
                in_window = true;
            }
            if (!in_window)
            {
                continue;
            }

            while (posting_index < block->posting_count && remaining > 0)
            {
                remaining--;
                stats->window_offsets++;
                stats->entries_cached++;
                posting_index++;
            }

            if (remaining == 0)
            {
                break;
            }
        }

        blkno = next_blkno;
        offnum = FirstOffsetNumber;
        UnlockReleaseBuffer(buf);
    }
}

static void
age_adjacency_probe_delta_pages(Relation index_rel, graphid key,
                                const AgeAdjacencyMetaPageData *meta,
                                AgeAdjacencyDeltaProbeStats *stats)
{
    BlockNumber blkno;

    Assert(meta != NULL);
    Assert(stats != NULL);

    memset(stats, 0, sizeof(*stats));

    blkno = meta->first_delta_blkno;
    while (BlockNumberIsValid(blkno))
    {
        Buffer buf;
        Page page;
        AgeAdjacencyPageOpaque opaque;
        BlockNumber next_blkno;
        OffsetNumber offnum;
        OffsetNumber maxoff;

        buf = ReadBuffer(index_rel, blkno);
        LockBuffer(buf, BUFFER_LOCK_SHARE);
        page = BufferGetPage(buf);

        opaque = (AgeAdjacencyPageOpaque) PageGetSpecialPointer(page);
        if (opaque->magic != AGE_ADJACENCY_MAGIC ||
            opaque->version != AGE_ADJACENCY_VERSION ||
            opaque->page_type != AGE_ADJACENCY_PAGE_DELTA)
        {
            ereport(ERROR,
                    (errcode(ERRCODE_INDEX_CORRUPTED),
                     errmsg("age_adjacency delta page %u is invalid", blkno)));
        }

        stats->pages_visited++;
        next_blkno = opaque->next_blkno;
        if (!age_adjacency_delta_page_may_contain_key(opaque, key))
        {
            stats->pages_skipped++;
            blkno = next_blkno;
            UnlockReleaseBuffer(buf);
            continue;
        }

        maxoff = PageGetMaxOffsetNumber(page);
        for (offnum = FirstOffsetNumber;
             offnum <= maxoff;
             offnum = OffsetNumberNext(offnum))
        {
            ItemId item_id = PageGetItemId(page, offnum);

            if (ItemIdIsNormal(item_id))
            {
                stats->entries_scanned++;
            }
        }

        blkno = next_blkno;
        UnlockReleaseBuffer(buf);
    }
}

static int64
age_adjacency_scan_payload(Relation index_rel, graphid key,
                           AgeAdjacencyScanTarget *target)
{
    AgeAdjacencyMetaPageData meta;

    age_adjacency_read_meta(index_rel, &meta);
    return age_adjacency_scan_payload_with_meta(index_rel, key, target, &meta);
}

static int64
age_adjacency_scan_payload_with_meta(Relation index_rel, graphid key,
                                     AgeAdjacencyScanTarget *target,
                                     const AgeAdjacencyMetaPageData *meta)
{
    AgeAdjacencyDirectoryEntryData entry;
    BlockNumber blkno;
    int64 matches = 0;

    if (age_adjacency_find_directory_entry_with_meta(index_rel, key, meta,
                                                     &entry))
    {
        matches += age_adjacency_scan_posting_run(index_rel,
                                                  entry.first_blkno,
                                                  entry.first_offnum,
                                                  entry.posting_count,
                                                  key, target);
    }

    blkno = meta->first_delta_blkno;
    while (BlockNumberIsValid(blkno))
    {
        Buffer buf;
        Page page;
        AgeAdjacencyPageOpaque opaque;
        BlockNumber next_blkno;
        OffsetNumber offnum;
        OffsetNumber maxoff;

        buf = ReadBuffer(index_rel, blkno);
        LockBuffer(buf, BUFFER_LOCK_SHARE);
        page = BufferGetPage(buf);

        opaque = (AgeAdjacencyPageOpaque) PageGetSpecialPointer(page);
        if (opaque->magic != AGE_ADJACENCY_MAGIC ||
            opaque->version != AGE_ADJACENCY_VERSION ||
            opaque->page_type != AGE_ADJACENCY_PAGE_DELTA)
        {
            ereport(ERROR,
                    (errcode(ERRCODE_INDEX_CORRUPTED),
                     errmsg("age_adjacency delta page %u is invalid", blkno)));
        }

        next_blkno = opaque->next_blkno;
        if (!age_adjacency_delta_page_may_contain_key(opaque, key))
        {
            blkno = next_blkno;
            UnlockReleaseBuffer(buf);
            continue;
        }

        maxoff = PageGetMaxOffsetNumber(page);
        for (offnum = FirstOffsetNumber;
             offnum <= maxoff;
             offnum = OffsetNumberNext(offnum))
        {
            ItemId item_id = PageGetItemId(page, offnum);
            AgeAdjacencyDeltaCompactPosting delta_posting;
            AgeAdjacencyPostingData posting;
            int64 emitted;

            if (!ItemIdIsNormal(item_id))
            {
                continue;
            }

            delta_posting = (AgeAdjacencyDeltaCompactPosting) PageGetItem(
                page, item_id);
            age_adjacency_delta_posting_get_posting(delta_posting, opaque,
                                                    &posting);
            if (posting.key != key)
            {
                continue;
            }

            emitted = age_adjacency_emit_posting(&posting, target);
            if (emitted < 0)
            {
                UnlockReleaseBuffer(buf);
                return matches;
            }
            matches += emitted;
        }

        blkno = next_blkno;
        UnlockReleaseBuffer(buf);
    }

    return matches;
}

static IndexBuildResult *
age_adjacency_build(Relation heap_rel, Relation index_rel,
                    struct IndexInfo *index_info)
{
    IndexBuildResult *result;
    AgeAdjacencyBuildState buildstate;
    double reltuples;

    age_adjacency_validate_index(index_rel);
    age_adjacency_init_metapage(heap_rel, index_rel);

    memset(&buildstate, 0, sizeof(buildstate));
    reltuples = table_index_build_scan(heap_rel, index_rel, index_info,
                                       true, true,
                                       age_adjacency_build_callback,
                                       &buildstate, NULL);
    age_adjacency_build_main_runs(index_rel, &buildstate);

    result = (IndexBuildResult *) palloc0(sizeof(IndexBuildResult));
    result->heap_tuples = reltuples;
    result->index_tuples = buildstate.indtuples;

    return result;
}

static void
age_adjacency_build_empty(Relation index_rel)
{
    age_adjacency_init_metapage(index_rel, index_rel);
}

static bool
age_adjacency_insert(Relation index_rel, Datum *values, bool *isnull,
                     ItemPointer heap_tid, Relation heap_rel,
                     IndexUniqueCheck check_unique, bool index_unchanged,
                     struct IndexInfo *index_info)
{
    AgeAdjacencyPostingData posting;

    (void) heap_rel;
    (void) check_unique;
    (void) index_unchanged;
    (void) index_info;

    age_adjacency_form_posting(index_rel, values, isnull, heap_tid, &posting);
    age_adjacency_append_delta_posting(index_rel, &posting);

    return false;
}

static IndexBulkDeleteResult *
age_adjacency_bulk_delete(IndexVacuumInfo *info, IndexBulkDeleteResult *stats,
                          IndexBulkDeleteCallback callback,
                          void *callback_state)
{
    Buffer metabuf;
    BlockNumber nblocks;
    BlockNumber blkno;
    bool needs_wal;
    AgeAdjacencyMetaPageData meta_copy;

    if (stats == NULL)
    {
        stats = (IndexBulkDeleteResult *) palloc0(sizeof(IndexBulkDeleteResult));
    }

    needs_wal = RelationNeedsWAL(info->index);
    metabuf = ReadBuffer(info->index, AGE_ADJACENCY_METAPAGE_BLKNO);
    LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);

    nblocks = RelationGetNumberOfBlocks(info->index);
    for (blkno = AGE_ADJACENCY_METAPAGE_BLKNO + 1; blkno < nblocks; blkno++)
    {
        Buffer buf;
        Page page;
        Page metapage;
        AgeAdjacencyMetaPage meta;
        AgeAdjacencyPageOpaque opaque;
        OffsetNumber maxoff;
        int offnum;
        uint64 removed = 0;
        GenericXLogState *state = NULL;

        buf = ReadBuffer(info->index, blkno);
        LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);

        if (needs_wal)
        {
            state = GenericXLogStart(info->index);
            metapage = GenericXLogRegisterBuffer(state, metabuf, 0);
            page = GenericXLogRegisterBuffer(state, buf, 0);
        }
        else
        {
            metapage = BufferGetPage(metabuf);
            page = BufferGetPage(buf);
        }

        meta = age_adjacency_get_meta(metapage);
        opaque = (AgeAdjacencyPageOpaque) PageGetSpecialPointer(page);
        if (opaque->magic != AGE_ADJACENCY_MAGIC ||
            opaque->version != AGE_ADJACENCY_VERSION ||
            (opaque->page_type != AGE_ADJACENCY_PAGE_MAIN &&
             opaque->page_type != AGE_ADJACENCY_PAGE_DELTA &&
             opaque->page_type != AGE_ADJACENCY_PAGE_DIRECTORY))
        {
            ereport(ERROR,
                    (errcode(ERRCODE_INDEX_CORRUPTED),
                     errmsg("age_adjacency page %u is invalid", blkno)));
        }

        if (opaque->page_type == AGE_ADJACENCY_PAGE_DIRECTORY)
        {
            if (needs_wal)
            {
                GenericXLogAbort(state);
            }
            UnlockReleaseBuffer(buf);
            continue;
        }

        maxoff = PageGetMaxOffsetNumber(page);
        for (offnum = maxoff; offnum >= FirstOffsetNumber; offnum--)
        {
            ItemId item_id = PageGetItemId(page, (OffsetNumber) offnum);

            if (!ItemIdIsNormal(item_id))
            {
                continue;
            }

            if (opaque->page_type == AGE_ADJACENCY_PAGE_MAIN)
            {
                AgeAdjacencyMainRunBlock block;
                uint16 read_index;
                uint16 write_index = 0;
                uint16 block_removed = 0;

                block = (AgeAdjacencyMainRunBlock) PageGetItem(page,
                                                               item_id);
                if ((block->flags & AGE_ADJACENCY_MAIN_BLOCK_COMPACT) != 0)
                {
                    AgeAdjacencyMainCompactPosting compact_postings =
                        (AgeAdjacencyMainCompactPosting) block->data;

                    for (read_index = 0;
                         read_index < block->posting_count;
                         read_index++)
                    {
                        ItemPointerData posting_tid;

                        memcpy(&posting_tid,
                               &compact_postings[read_index].heap_tid,
                               sizeof(ItemPointerData));
                        if (callback(&posting_tid, callback_state))
                        {
                            removed++;
                            block_removed++;
                            continue;
                        }

                        if (write_index != read_index)
                        {
                            compact_postings[write_index] =
                                compact_postings[read_index];
                        }
                        write_index++;
                    }
                }
                else
                {
                    AgeAdjacencyMainPosting full_postings =
                        (AgeAdjacencyMainPosting) block->data;

                    for (read_index = 0;
                         read_index < block->posting_count;
                         read_index++)
                    {
                        ItemPointer posting_tid =
                            &full_postings[read_index].heap_tid;

                        if (callback(posting_tid, callback_state))
                        {
                            removed++;
                            block_removed++;
                            continue;
                        }

                        if (write_index != read_index)
                        {
                            full_postings[write_index] =
                                full_postings[read_index];
                        }
                        write_index++;
                    }
                }

                if (block_removed == block->posting_count)
                {
                    PageIndexTupleDelete(page, (OffsetNumber) offnum);
                }
                else if (block_removed > 0)
                {
                    block->posting_count = write_index;
                }
            }
            else
            {
                AgeAdjacencyDeltaCompactPosting posting;
                ItemPointerData posting_tid;

                posting = (AgeAdjacencyDeltaCompactPosting) PageGetItem(
                    page, item_id);
                memcpy(&posting_tid, &posting->heap_tid,
                       sizeof(ItemPointerData));

                if (callback(&posting_tid, callback_state))
                {
                    PageIndexTupleDelete(page, (OffsetNumber) offnum);
                    removed++;
                }
            }
        }

        if (removed > 0)
        {
            opaque->posting_count -= removed;
            if (meta->postings >= removed)
            {
                meta->postings -= removed;
            }
            else
            {
                meta->postings = 0;
            }
            if (opaque->page_type == AGE_ADJACENCY_PAGE_DELTA)
            {
                if (meta->delta_postings >= removed)
                {
                    meta->delta_postings -= removed;
                }
                else
                {
                    meta->delta_postings = 0;
                }
            }
            stats->tuples_removed += removed;

            if (needs_wal)
            {
                GenericXLogFinish(state);
            }
            else
            {
                MarkBufferDirty(buf);
                MarkBufferDirty(metabuf);
            }
        }
        else if (needs_wal)
        {
            GenericXLogAbort(state);
        }

        UnlockReleaseBuffer(buf);
    }

    memcpy(&meta_copy, age_adjacency_get_meta(BufferGetPage(metabuf)),
           sizeof(AgeAdjacencyMetaPageData));
    stats->num_pages = nblocks;
    stats->estimated_count = false;
    stats->num_index_tuples = meta_copy.postings;

    UnlockReleaseBuffer(metabuf);

    return stats;
}

static IndexBulkDeleteResult *
age_adjacency_vacuum_cleanup(IndexVacuumInfo *info,
                             IndexBulkDeleteResult *stats)
{
    AgeAdjacencyMetaPageData meta;

    if (stats == NULL)
    {
        stats = (IndexBulkDeleteResult *) palloc0(sizeof(IndexBulkDeleteResult));
    }

    age_adjacency_read_meta(info->index, &meta);

    stats->num_pages = RelationGetNumberOfBlocks(info->index);
    stats->estimated_count = false;
    stats->num_index_tuples = meta.postings;

    return stats;
}

static bool
age_adjacency_read_planner_meta(Oid index_oid,
                                AgeAdjacencyMetaPageData *meta_out,
                                BlockNumber *pages_out)
{
    Relation index_rel;

    if (!OidIsValid(index_oid))
    {
        return false;
    }

    index_rel = index_open(index_oid, AccessShareLock);
    if (RelationGetNumberOfBlocks(index_rel) <= AGE_ADJACENCY_METAPAGE_BLKNO)
    {
        index_close(index_rel, AccessShareLock);
        return false;
    }

    if (pages_out != NULL)
    {
        *pages_out = RelationGetNumberOfBlocks(index_rel);
    }
    age_adjacency_read_meta(index_rel, meta_out);

    index_close(index_rel, AccessShareLock);
    return true;
}

static bool
age_adjacency_extract_constant_index_key(IndexPath *path, graphid *key_out)
{
    ListCell *lc;

    if (path == NULL || key_out == NULL)
    {
        return false;
    }

    foreach(lc, path->indexclauses)
    {
        IndexClause *iclause = lfirst_node(IndexClause, lc);
        ListCell *qlc;

        if (iclause->indexcol != 0)
        {
            continue;
        }

        foreach(qlc, iclause->indexquals)
        {
            RestrictInfo *rinfo = lfirst_node(RestrictInfo, qlc);
            Expr *clause = rinfo->clause;
            Node *rightop;

            if (!IsA(clause, OpExpr))
            {
                continue;
            }

            rightop = get_rightop(clause);
            if (rightop != NULL && IsA(rightop, Const))
            {
                Const *constant = (Const *) rightop;

                if (!constant->constisnull &&
                    constant->consttype == GRAPHIDOID)
                {
                    *key_out = DATUM_GET_GRAPHID(constant->constvalue);
                    return true;
                }
            }
        }
    }

    return false;
}

static bool
age_adjacency_probe_planner_delta(Oid index_oid, graphid key,
                                  AgeAdjacencyDeltaProbeStats *stats)
{
    Relation index_rel;
    AgeAdjacencyMetaPageData meta;

    if (!OidIsValid(index_oid) || stats == NULL)
    {
        return false;
    }

    index_rel = index_open(index_oid, AccessShareLock);
    if (RelationGetNumberOfBlocks(index_rel) <= AGE_ADJACENCY_METAPAGE_BLKNO)
    {
        index_close(index_rel, AccessShareLock);
        return false;
    }

    age_adjacency_read_meta(index_rel, &meta);
    age_adjacency_probe_delta_pages(index_rel, key, &meta, stats);

    index_close(index_rel, AccessShareLock);
    return true;
}

static const char *
age_adjacency_delta_maintenance_action(
    const AgeAdjacencyMetaPageData *meta, BlockNumber index_pages,
    int64 *delta_pages_out, int64 *delta_tuples_per_page_out,
    const char **reason_out)
{
    int64 delta_tuples_per_page;
    int64 delta_pages;
    const char *action;
    const char *reason;

    Assert(meta != NULL);

    delta_tuples_per_page =
        AGE_ADJACENCY_ITEMS_PER_PAGE(
            sizeof(AgeAdjacencyDeltaCompactPostingData));
    delta_pages = meta->delta_postings > 0 ?
                  (int64) ceil((double) meta->delta_postings /
                               (double) delta_tuples_per_page) : 0;

    if (meta->delta_postings >= AGE_ADJACENCY_DELTA_REINDEX_THRESHOLD)
    {
        action = "reindex-delta";
        reason = "delta-postings-threshold";
    }
    else if (delta_pages > 1)
    {
        action = "range-skip-delta";
        reason = "multi-page-delta-range-summary";
    }
    else if (meta->delta_postings > 0)
    {
        action = "observe-delta";
        reason = "single-page-delta";
    }
    else
    {
        action = "none";
        reason = "no-delta";
    }

    if (index_pages <= AGE_ADJACENCY_METAPAGE_BLKNO)
    {
        reason = "empty-index";
    }

    if (delta_pages_out != NULL)
    {
        *delta_pages_out = delta_pages;
    }
    if (delta_tuples_per_page_out != NULL)
    {
        *delta_tuples_per_page_out = delta_tuples_per_page;
    }
    if (reason_out != NULL)
    {
        *reason_out = reason;
    }

    return action;
}

static void
age_adjacency_cost_estimate(struct PlannerInfo *root, struct IndexPath *path,
                            double loop_count, Cost *index_startup_cost,
                            Cost *index_total_cost,
                            Selectivity *index_selectivity,
                            double *index_correlation, double *index_pages)
{
    double pages = 1;
    double tuples = 1;
    double directory_entries = 1;
    double directory_pages;
    double directory_probe_pages;
    double selectivity;
    double expected_tuples;
    double main_pages;
    double delta_postings = 0;
    double delta_pages = 0;
    double delta_cpu_tuples = 0;
    double main_tuples_per_page =
        AGE_ADJACENCY_MAIN_RUN_BLOCK_COMPACT_MAX_POSTINGS;
    double delta_tuples_per_page =
        AGE_ADJACENCY_ITEMS_PER_PAGE(
            sizeof(AgeAdjacencyDeltaCompactPostingData));
    double directory_entries_per_page =
        AGE_ADJACENCY_ITEMS_PER_PAGE(sizeof(AgeAdjacencyDirectoryEntryData));
    AgeAdjacencyMetaPageData meta;
    BlockNumber actual_pages;
    bool have_meta = false;
    bool have_constant_key = false;
    graphid constant_key = 0;

    (void) root;

    if (path != NULL && path->indexinfo != NULL)
    {
        pages = Max(path->indexinfo->pages, 1);
        tuples = Max(path->indexinfo->tuples, 1);

        if (!path->indexinfo->hypothetical)
        {
            have_meta = age_adjacency_read_planner_meta(
                path->indexinfo->indexoid, &meta, &actual_pages);
        }
    }

    if (have_meta)
    {
        pages = Max((double) actual_pages, 1);
        tuples = Max((double) meta.postings, 1);
        directory_entries = Max((double) meta.directory_entries, 1);
        delta_postings = (double) meta.delta_postings;
    }
    else
    {
        directory_entries = Max(tuples, 1);
    }

    selectivity = Min(0.1, Max(1.0 / tuples, 0.0001));
    expected_tuples = Max(1.0, tuples * selectivity);
    directory_pages = Max(1.0,
                          ceil(directory_entries /
                               directory_entries_per_page));
    directory_probe_pages = Max(1.0, ceil(log(directory_pages + 1.0) /
                                          log(2.0)));
    main_pages = Max(1.0, ceil(expected_tuples / main_tuples_per_page));
    delta_pages = delta_postings > 0 ?
                  ceil(delta_postings / delta_tuples_per_page) : 0;
    delta_cpu_tuples = delta_postings;

    have_constant_key = age_adjacency_extract_constant_index_key(path,
                                                                &constant_key);
    if (have_meta && have_constant_key && delta_postings > 0)
    {
        AgeAdjacencyDeltaProbeStats delta_stats;

        if (age_adjacency_probe_planner_delta(path->indexinfo->indexoid,
                                              constant_key,
                                              &delta_stats))
        {
            delta_cpu_tuples = (double) delta_stats.entries_scanned;
        }
    }

    *index_startup_cost = random_page_cost;
    *index_total_cost =
        *index_startup_cost +
        (directory_probe_pages + main_pages + delta_pages) * random_page_cost +
        (expected_tuples + delta_cpu_tuples) * cpu_index_tuple_cost;

    if (delta_postings >= AGE_ADJACENCY_DELTA_REINDEX_THRESHOLD)
    {
        *index_total_cost *= 2.0;
    }
    *index_total_cost *= Max(loop_count, 1.0);

    *index_selectivity = selectivity;
    *index_correlation = 0;
    *index_pages = pages;
}

static bytea *
age_adjacency_options(Datum reloptions, bool validate)
{
    (void) reloptions;
    (void) validate;

    return NULL;
}

static bool
age_adjacency_validate(Oid opclass_oid)
{
    (void) opclass_oid;

    return true;
}

static IndexScanDesc
age_adjacency_begin_scan(Relation index_rel, int nkeys, int norderbys)
{
    if (norderbys != 0)
    {
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("age_adjacency does not support ordered scans")));
    }

    return RelationGetIndexScan(index_rel, nkeys, norderbys);
}

static void
age_adjacency_rescan(IndexScanDesc scan, ScanKey keys, int nkeys,
                     ScanKey orderbys, int norderbys)
{
    (void) orderbys;

    if (norderbys != 0)
    {
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("age_adjacency does not support ordered scans")));
    }

    if (keys != NULL && nkeys > 0)
    {
        if (nkeys != scan->numberOfKeys)
        {
            elog(ERROR, "unexpected age_adjacency scan key count");
        }

        memcpy(scan->keyData, keys, sizeof(ScanKeyData) * nkeys);
    }
}

static int64
age_adjacency_get_bitmap(IndexScanDesc scan, TIDBitmap *tbm)
{
    ScanKey key;
    AgeAdjacencyScanTarget target;

    if (scan->numberOfKeys != 1)
    {
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("age_adjacency scans require one equality key")));
    }

    key = &scan->keyData[0];
    if (key->sk_attno != 1 ||
        key->sk_strategy != AGE_ADJACENCY_EQUAL_STRATEGY)
    {
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("age_adjacency scans only support equality on the "
                        "first key column")));
    }

    if (key->sk_flags & SK_ISNULL)
    {
        return 0;
    }

    memset(&target, 0, sizeof(target));
    target.tbm = tbm;

    return age_adjacency_scan_payload(scan->indexRelation,
                                      DATUM_GET_GRAPHID(key->sk_argument),
                                      &target);
}

static void
age_adjacency_end_scan(IndexScanDesc scan)
{
    (void) scan;
}

AgeAdjacencyVisiblePayloadScan *
age_adjacency_begin_visible_payload_scan(Oid index_oid, Snapshot snapshot,
                                         bool fetch_properties)
{
    AgeAdjacencyVisiblePayloadScan *scan;

    scan = palloc0(sizeof(AgeAdjacencyVisiblePayloadScan));
    scan->index_rel = index_open(index_oid, AccessShareLock);
    age_adjacency_validate_index(scan->index_rel);
    age_adjacency_read_meta(scan->index_rel, &scan->meta);

    scan->heap_rel = relation_open(scan->meta.heap_relid, AccessShareLock);
    scan->slot = table_slot_create(scan->heap_rel, NULL);

    memset(&scan->target, 0, sizeof(scan->target));
    scan->target.heap_rel = scan->heap_rel;
    scan->target.snapshot = snapshot != NULL ? snapshot : GetActiveSnapshot();
    scan->target.slot = scan->slot;
    scan->target.visibilitymap_buffer = InvalidBuffer;
    scan->target.visibilitymap_cached_block = InvalidBlockNumber;
    scan->target.visibilitymap_cache_valid = false;
    scan->target.visibilitymap_cached_all_visible = false;
    scan->target.fetch_properties = fetch_properties;
    scan->target.terminal_label_id = INVALID_LABEL_ID;

    return scan;
}

void
age_adjacency_visible_payload_scan_set_terminal_label(
    AgeAdjacencyVisiblePayloadScan *scan, int32 terminal_label_id)
{
    if (scan == NULL)
    {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("age_adjacency visible payload scan is required")));
    }

    scan->target.terminal_label_id = terminal_label_id;
}

void
age_adjacency_visible_payload_scan_set_terminal_vertex_filter(
    AgeAdjacencyVisiblePayloadScan *scan,
    AgeAdjacencyVertexFilterCallback callback, void *callback_state)
{
    AgeAdjacencyCompositeTerminalFilter filter;

    if (scan == NULL)
    {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("age_adjacency visible payload scan is required")));
    }

    memset(&filter, 0, sizeof(filter));
    filter.terminal_label_id = scan->target.terminal_label_id;
    filter.vertex_filter = callback;
    filter.vertex_filter_state = callback_state;
    filter.has_vertex_filter = callback != NULL;
    filter.source = "callback";
    age_adjacency_visible_payload_scan_set_composite_terminal_filter(scan,
                                                                     &filter);
}

void
age_adjacency_visible_payload_scan_set_terminal_vertex_set_filter(
    AgeAdjacencyVisiblePayloadScan *scan,
    const AgeAdjacencyVertexSetFilter *filter)
{
    AgeAdjacencyCompositeTerminalFilter composite_filter;

    if (scan == NULL)
    {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("age_adjacency visible payload scan is required")));
    }

    memset(&composite_filter, 0, sizeof(composite_filter));
    composite_filter.terminal_label_id = scan->target.terminal_label_id;
    if (filter != NULL)
    {
        composite_filter.vertex_set_filter = *filter;
        composite_filter.source = filter->source;
        composite_filter.has_vertex_set_filter = true;
    }
    age_adjacency_visible_payload_scan_set_composite_terminal_filter(
        scan, &composite_filter);
}

void
age_adjacency_visible_payload_scan_set_composite_terminal_filter(
    AgeAdjacencyVisiblePayloadScan *scan,
    const AgeAdjacencyCompositeTerminalFilter *filter)
{
    if (scan == NULL)
    {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("age_adjacency visible payload scan is required")));
    }

    if (filter == NULL)
    {
        scan->target.terminal_label_id = INVALID_LABEL_ID;
        scan->target.terminal_vertex_filter = NULL;
        scan->target.terminal_vertex_filter_state = NULL;
        scan->target.terminal_property_index_oid = InvalidOid;
        scan->target.terminal_property_filter_id = 0;
        scan->target.terminal_property_match_count = 0;
        scan->target.has_terminal_property_summary = false;
        memset(&scan->target.terminal_vertex_set_filter, 0,
               sizeof(scan->target.terminal_vertex_set_filter));
        return;
    }

    if (filter->has_property_summary)
        scan->target.terminal_composite_requests++;
    scan->target.terminal_label_id = filter->terminal_label_id;
    scan->target.terminal_vertex_filter = filter->has_vertex_filter ?
        filter->vertex_filter : NULL;
    scan->target.terminal_vertex_filter_state = filter->has_vertex_filter ?
        filter->vertex_filter_state : NULL;
    scan->target.terminal_property_index_oid = filter->property_index_oid;
    scan->target.terminal_property_filter_id = filter->property_filter_id;
    scan->target.terminal_property_match_count = filter->property_match_count;
    scan->target.has_terminal_property_summary = filter->has_property_summary;
    if (filter->has_vertex_set_filter)
    {
        scan->target.terminal_vertex_set_filter =
            filter->vertex_set_filter;
        age_adjacency_vertex_set_filter_prepare_value_summary(
            &scan->target.terminal_vertex_set_filter,
            filter->property_filter_id);
    }
    else
    {
        memset(&scan->target.terminal_vertex_set_filter, 0,
               sizeof(scan->target.terminal_vertex_set_filter));
    }
    (void) age_adjacency_visible_payload_scan_apply_directory_vertex_set_filter(
        scan);
}

bool
age_adjacency_visible_payload_scan_begin_key(
    AgeAdjacencyVisiblePayloadScan *scan, graphid key)
{
    AgeAdjacencyVisiblePayloadKeyCursor cursor;

    if (scan == NULL)
    {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("age_adjacency visible payload scan is required")));
    }

    if (!age_adjacency_visible_payload_key_cursor_begin(scan, &cursor, key))
        return false;

    scan->active_key = cursor.active_key;
    scan->key_active = cursor.key_active;
    scan->main_active = cursor.main_active;
    scan->main_remaining = cursor.main_remaining;
    scan->main_label_candidate_count = cursor.main_label_candidate_count;
    scan->main_composite_estimate_recorded =
        cursor.main_composite_estimate_recorded;
    scan->main_blkno = cursor.main_blkno;
    scan->main_offnum = cursor.main_offnum;
    scan->main_posting_index = cursor.main_posting_index;
    scan->main_cache_index = cursor.main_cache_index;
    scan->delta_blkno = cursor.delta_blkno;
    scan->delta_offnum = cursor.delta_offnum;
    scan->main_posting_ordinal = 0;
    scan->delta_posting_ordinal = 0;

    return true;
}

void
age_adjacency_visible_payload_scan_set_parallel_slice(
    AgeAdjacencyVisiblePayloadScan *scan, int32 slice_index,
    int32 slice_count)
{
    if (scan == NULL)
    {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("age_adjacency visible payload scan is required")));
    }

    if (slice_count <= 1)
    {
        scan->parallel_slice_index = 0;
        scan->parallel_slice_count = 0;
        return;
    }

    if (slice_index < 0 || slice_index >= slice_count)
    {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("age_adjacency parallel slice requires "
                        "0 <= slice_index < slice_count")));
    }

    scan->parallel_slice_index = slice_index;
    scan->parallel_slice_count = slice_count;
}

int64
age_adjacency_visible_payload_scan_run_postings(
    AgeAdjacencyVisiblePayloadScan *scan)
{
    if (scan == NULL || !scan->key_active || !scan->main_active)
        return 0;

    return (int64)scan->main_remaining;
}

int64
age_adjacency_visible_payload_scan_active_postings(
    AgeAdjacencyVisiblePayloadScan *scan)
{
    if (scan == NULL || !scan->key_active || !scan->main_active)
        return 0;

    return scan->main_label_candidate_count;
}

bool
age_adjacency_visible_payload_scan_key_known_empty(
    AgeAdjacencyVisiblePayloadScan *scan, graphid key)
{
    AgeAdjacencyDirectoryEntryData entry;

    if (scan == NULL)
    {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("age_adjacency visible payload scan is required")));
    }

    if (BlockNumberIsValid(scan->meta.first_delta_blkno))
        return false;

    if (!BlockNumberIsValid(scan->meta.first_directory_blkno) ||
        !BlockNumberIsValid(scan->meta.last_directory_blkno))
    {
        return true;
    }

    if (!age_adjacency_find_directory_entry_cached(scan, key, &entry))
        return true;

    return !age_adjacency_directory_entry_matches_composite_target(
        &entry, &scan->target, NULL, NULL);
}

bool
age_adjacency_visible_payload_scan_next(
    AgeAdjacencyVisiblePayloadScan *scan, AgeAdjacencyPayload *payload)
{
    if (scan == NULL || payload == NULL)
    {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("age_adjacency visible payload scan next requires "
                        "scan and payload")));
    }

    if (!scan->key_active)
    {
        return false;
    }

    if (age_adjacency_visible_payload_scan_next_main(scan, payload))
    {
        return true;
    }

    if (age_adjacency_visible_payload_scan_next_delta(scan, payload))
    {
        return true;
    }

    scan->key_active = false;
    return false;
}

AgeAdjacencyVisiblePayloadRunScan *
age_adjacency_begin_visible_payload_run_scan(
    Oid index_oid, Snapshot snapshot, bool fetch_properties,
    int32 terminal_label_id, const graphid *keys, int64 key_count)
{
    AgeAdjacencyVisiblePayloadRunKey *tagged_keys;
    AgeAdjacencyVisiblePayloadRunScan *run_scan;
    int64 i;

    if (keys == NULL || key_count <= 0)
        return NULL;

    tagged_keys = palloc_array(AgeAdjacencyVisiblePayloadRunKey, key_count);
    for (i = 0; i < key_count; i++)
    {
        tagged_keys[i].key = keys[i];
        tagged_keys[i].tag = (void *) (intptr_t) i;
    }

    run_scan = age_adjacency_begin_visible_payload_run_scan_with_tags(
        index_oid, snapshot, fetch_properties, terminal_label_id, tagged_keys,
        key_count);
    pfree(tagged_keys);
    return run_scan;
}

AgeAdjacencyVisiblePayloadRunScan *
age_adjacency_begin_visible_payload_run_scan_with_tags(
    Oid index_oid, Snapshot snapshot, bool fetch_properties,
    int32 terminal_label_id, const AgeAdjacencyVisiblePayloadRunKey *keys,
    int64 key_count)
{
    AgeAdjacencyVisiblePayloadRunOptions options;

    memset(&options, 0, sizeof(options));
    options.terminal_label_id = terminal_label_id;
    return age_adjacency_begin_visible_payload_run_scan_with_options(
        index_oid, snapshot, fetch_properties, &options, keys, key_count);
}

AgeAdjacencyVisiblePayloadRunScan *
age_adjacency_begin_visible_payload_run_scan_with_options(
    Oid index_oid, Snapshot snapshot, bool fetch_properties,
    const AgeAdjacencyVisiblePayloadRunOptions *options,
    const AgeAdjacencyVisiblePayloadRunKey *keys, int64 key_count)
{
    AgeAdjacencyVisiblePayloadRunScan *run_scan;
    AgeAdjacencyCompositeTerminalFilter composite_filter;
    int64 active_postings;
    int64 i;
    int64 run_postings;
    bool known_empty;

    if (keys == NULL || key_count <= 0)
        return NULL;

    run_scan = palloc0(sizeof(*run_scan));
    run_scan->scan = age_adjacency_begin_visible_payload_scan(
        index_oid, snapshot, fetch_properties);
    if (options != NULL)
    {
        age_adjacency_visible_payload_scan_set_terminal_label(
            run_scan->scan, options->terminal_label_id);
        run_scan->payload_callback = options->payload_callback;
        run_scan->payload_callback_state = options->payload_callback_state;
        run_scan->filtered_key_callback = options->filtered_key_callback;
        run_scan->filtered_key_callback_state =
            options->filtered_key_callback_state;
    }
    run_scan->cursors = palloc0_array(AgeAdjacencyVisiblePayloadKeyCursor,
                                      key_count);
    run_scan->active_cursor_indexes = palloc_array(int64, key_count);
    run_scan->cursor_count = key_count;

    run_postings = 0;
    active_postings = 0;
    for (i = 0; i < key_count; i++)
    {
        AgeAdjacencyVisiblePayloadKeyCursor *cursor;
        bool cursor_begun;

        cursor = &run_scan->cursors[i];
        cursor_begun = age_adjacency_visible_payload_key_cursor_begin(
            run_scan->scan, cursor, keys[i].key);
        cursor->tag = keys[i].tag;
        if (!cursor_begun)
            continue;
        run_postings += cursor->main_remaining;
        active_postings += cursor->main_label_candidate_count;
    }

    known_empty = false;
    memset(&composite_filter, 0, sizeof(composite_filter));
    if (options != NULL && options->filter_callback != NULL &&
        options->filter_callback(run_postings, active_postings,
                                 &composite_filter, &known_empty,
                                 options->filter_callback_state))
    {
        age_adjacency_visible_payload_scan_set_composite_terminal_filter(
            run_scan->scan, &composite_filter);
        age_adjacency_visible_payload_scan_reset_runtime(run_scan->scan);
        for (i = 0; i < key_count; i++)
        {
            AgeAdjacencyVisiblePayloadKeyCursor *cursor;
            bool cursor_begun;

            cursor = &run_scan->cursors[i];
            cursor_begun = age_adjacency_visible_payload_key_cursor_begin(
                run_scan->scan, cursor, keys[i].key);
            cursor->tag = keys[i].tag;
            if (!cursor_begun)
                continue;
        }
    }

    if (known_empty)
    {
        run_scan->known_empty = true;
        return run_scan;
    }

    for (i = 0; i < key_count; i++)
    {
        AgeAdjacencyVisiblePayloadKeyCursor *cursor;

        cursor = &run_scan->cursors[i];
        if (!cursor->key_active)
        {
            cursor->seen = true;
            if (run_scan->filtered_key_callback != NULL)
                run_scan->filtered_key_callback(
                    cursor->tag, run_scan->filtered_key_callback_state);
            continue;
        }
        cursor->payload_valid =
            age_adjacency_visible_payload_key_cursor_next(
                run_scan->scan, cursor, &cursor->payload);
        if (!cursor->payload_valid)
        {
            cursor->seen = true;
            if (run_scan->filtered_key_callback != NULL)
                run_scan->filtered_key_callback(
                    cursor->tag, run_scan->filtered_key_callback_state);
            continue;
        }
        run_scan->initial_active_key_count++;
        age_adjacency_visible_payload_run_scan_activate_cursor(run_scan, i);
    }

    return run_scan;
}

static void
age_adjacency_visible_payload_run_scan_activate_cursor(
    AgeAdjacencyVisiblePayloadRunScan *scan, int64 cursor_index)
{
    AgeAdjacencyVisiblePayloadKeyCursor *cursor;
    int64 insert_index;

    Assert(scan != NULL);
    Assert(cursor_index >= 0 && cursor_index < scan->cursor_count);
    Assert(scan->active_cursor_count < scan->cursor_count);

    cursor = &scan->cursors[cursor_index];
    Assert(cursor->payload_valid);
    insert_index = scan->active_cursor_count;
    while (insert_index > 0)
    {
        AgeAdjacencyVisiblePayloadKeyCursor *before;

        before = &scan->cursors[
            scan->active_cursor_indexes[insert_index - 1]];
        if (age_adjacency_visible_payload_key_cursor_compare_payload(
                before, cursor) <= 0)
            break;
        scan->active_cursor_indexes[insert_index] =
            scan->active_cursor_indexes[insert_index - 1];
        insert_index--;
    }

    scan->active_cursor_indexes[insert_index] = cursor_index;
    scan->active_cursor_count++;
}

static void
age_adjacency_visible_payload_run_scan_deactivate_cursor(
    AgeAdjacencyVisiblePayloadRunScan *scan, int64 active_index)
{
    int64 i;

    Assert(scan != NULL);
    Assert(active_index >= 0 && active_index < scan->active_cursor_count);

    for (i = active_index + 1; i < scan->active_cursor_count; i++)
        scan->active_cursor_indexes[i - 1] = scan->active_cursor_indexes[i];
    scan->active_cursor_count--;
}

static int
age_adjacency_visible_payload_key_cursor_compare_payload(
    const AgeAdjacencyVisiblePayloadKeyCursor *left,
    const AgeAdjacencyVisiblePayloadKeyCursor *right)
{
    Assert(left != NULL);
    Assert(right != NULL);
    Assert(left->payload_valid);
    Assert(right->payload_valid);

    if (left->payload.next_vertex_id != right->payload.next_vertex_id)
    {
        return left->payload.next_vertex_id < right->payload.next_vertex_id ?
            -1 : 1;
    }
    if (left->payload.edge_id != right->payload.edge_id)
        return left->payload.edge_id < right->payload.edge_id ? -1 : 1;
    if (left->initial_main_label_candidate_count !=
        right->initial_main_label_candidate_count)
    {
        return left->initial_main_label_candidate_count <
            right->initial_main_label_candidate_count ? -1 : 1;
    }
    if (left->active_key != right->active_key)
        return left->active_key < right->active_key ? -1 : 1;

    return 0;
}

bool
age_adjacency_visible_payload_run_scan_next(
    AgeAdjacencyVisiblePayloadRunScan *scan, AgeAdjacencyPayload *payload,
    int64 *key_index)
{
    void *tag;

    if (key_index == NULL)
    {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("age_adjacency visible payload run scan next requires "
                        "key_index")));
    }

    if (!age_adjacency_visible_payload_run_scan_next_tag(scan, payload, &tag))
        return false;

    *key_index = (int64) (intptr_t) tag;
    return true;
}

bool
age_adjacency_visible_payload_run_scan_next_tag(
    AgeAdjacencyVisiblePayloadRunScan *scan, AgeAdjacencyPayload *payload,
    void **tag)
{
    AgeAdjacencyVisiblePayloadKeyCursor *selected;
    int64 selected_active_index;
    int64 selected_cursor_index;

    if (scan == NULL || payload == NULL || tag == NULL)
    {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("age_adjacency visible payload run scan next requires "
                        "scan, payload, and tag")));
    }

    if (scan->active_cursor_count == 0)
        return false;

    selected_active_index = 0;
    selected_cursor_index = scan->active_cursor_indexes[selected_active_index];
    selected = &scan->cursors[selected_cursor_index];
    Assert(selected->payload_valid);

    *payload = selected->payload;
    *tag = selected->tag;
    selected->seen = true;
    if (scan->payload_callback != NULL)
        scan->payload_callback(selected->tag, payload,
                               scan->payload_callback_state);
    selected->payload_valid = age_adjacency_visible_payload_key_cursor_next(
        scan->scan, selected, &selected->payload);
    age_adjacency_visible_payload_run_scan_deactivate_cursor(
        scan, selected_active_index);
    if (selected->payload_valid)
        age_adjacency_visible_payload_run_scan_activate_cursor(
            scan, selected_cursor_index);

    return true;
}

bool
age_adjacency_visible_payload_run_scan_key_seen(
    AgeAdjacencyVisiblePayloadRunScan *scan, int64 key_index)
{
    if (scan == NULL || key_index < 0 || key_index >= scan->cursor_count)
    {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("age_adjacency visible payload run scan seen check "
                        "requires scan and valid key_index")));
    }

    return scan->cursors[key_index].seen;
}

bool
age_adjacency_visible_payload_run_scan_key_evidence(
    AgeAdjacencyVisiblePayloadRunScan *scan, int64 key_index,
    AgeAdjacencyVisiblePayloadRunKeyEvidence *evidence)
{
    AgeAdjacencyVisiblePayloadKeyCursor *cursor;

    if (scan == NULL || key_index < 0 || key_index >= scan->cursor_count ||
        evidence == NULL)
    {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("age_adjacency visible payload run scan evidence "
                        "requires scan, valid key_index, and evidence")));
    }

    cursor = &scan->cursors[key_index];
    evidence->run_postings = cursor->initial_main_remaining;
    evidence->terminal_postings = cursor->initial_main_label_candidate_count;
    evidence->active = !scan->known_empty &&
                       cursor->key_active &&
                       (cursor->main_active ||
                        BlockNumberIsValid(cursor->delta_blkno));
    evidence->known_empty = scan->known_empty || !evidence->active;

    return evidence->active;
}

int64
age_adjacency_visible_payload_run_scan_active_keys(
    AgeAdjacencyVisiblePayloadRunScan *scan)
{
    if (scan == NULL)
    {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("age_adjacency visible payload run scan active key "
                        "count requires scan")));
    }

    return scan->initial_active_key_count;
}

void
age_adjacency_end_visible_payload_run_scan(
    AgeAdjacencyVisiblePayloadRunScan *scan)
{
    if (scan == NULL)
        return;

    age_adjacency_end_visible_payload_scan(scan->scan);
    if (scan->active_cursor_indexes != NULL)
        pfree(scan->active_cursor_indexes);
    if (scan->cursors != NULL)
        pfree(scan->cursors);
    pfree(scan);
}

int64
age_adjacency_visible_payload_scan_label_filtered(
    AgeAdjacencyVisiblePayloadScan *scan)
{
    if (scan == NULL)
        return 0;

    return scan->target.terminal_label_filtered;
}

int64
age_adjacency_visible_payload_scan_directory_label_filtered(
    AgeAdjacencyVisiblePayloadScan *scan)
{
    if (scan == NULL)
        return 0;

    return scan->target.terminal_directory_label_filtered;
}

int64
age_adjacency_visible_payload_scan_property_filtered(
    AgeAdjacencyVisiblePayloadScan *scan)
{
    if (scan == NULL)
        return 0;

    return scan->target.terminal_property_filtered;
}

int64
age_adjacency_visible_payload_scan_cache_filtered(
    AgeAdjacencyVisiblePayloadScan *scan)
{
    if (scan == NULL)
        return 0;

    return scan->target.terminal_cache_filtered;
}

int64
age_adjacency_visible_payload_scan_cache_label_filtered(
    AgeAdjacencyVisiblePayloadScan *scan)
{
    if (scan == NULL)
        return 0;

    return scan->target.terminal_cache_label_filtered;
}

int64
age_adjacency_visible_payload_scan_cache_property_filtered(
    AgeAdjacencyVisiblePayloadScan *scan)
{
    if (scan == NULL)
        return 0;

    return scan->target.terminal_cache_property_filtered;
}

int64
age_adjacency_visible_payload_scan_vertex_set_range_filtered(
    AgeAdjacencyVisiblePayloadScan *scan)
{
    if (scan == NULL)
        return 0;

    return scan->target.terminal_vertex_set_range_filtered;
}

int64
age_adjacency_visible_payload_scan_vertex_set_sorted_filtered(
    AgeAdjacencyVisiblePayloadScan *scan)
{
    if (scan == NULL)
        return 0;

    return scan->target.terminal_vertex_set_sorted_filtered;
}

int64
age_adjacency_visible_payload_scan_vertex_set_block_filtered(
    AgeAdjacencyVisiblePayloadScan *scan)
{
    if (scan == NULL)
        return 0;

    return scan->target.terminal_vertex_set_block_filtered;
}

int64
age_adjacency_visible_payload_scan_vertex_set_block_value_filtered(
    AgeAdjacencyVisiblePayloadScan *scan)
{
    if (scan == NULL)
        return 0;

    return scan->target.terminal_vertex_set_block_value_filtered;
}

int64
age_adjacency_visible_payload_scan_vertex_set_block_value_posting_filtered(
    AgeAdjacencyVisiblePayloadScan *scan)
{
    if (scan == NULL)
        return 0;

    return scan->target.terminal_vertex_set_block_value_posting_filtered;
}

int64
age_adjacency_visible_payload_scan_vertex_set_block_posting_filtered(
    AgeAdjacencyVisiblePayloadScan *scan)
{
    if (scan == NULL)
        return 0;

    return scan->target.terminal_vertex_set_block_posting_filtered;
}

int64
age_adjacency_visible_payload_scan_vertex_set_block_compressed_filtered(
    AgeAdjacencyVisiblePayloadScan *scan)
{
    if (scan == NULL)
        return 0;

    return scan->target.terminal_vertex_set_block_compressed_filtered;
}

int64
age_adjacency_visible_payload_scan_vertex_set_directory_filtered(
    AgeAdjacencyVisiblePayloadScan *scan)
{
    if (scan == NULL)
        return 0;

    return scan->target.terminal_vertex_set_directory_filtered;
}

int64
age_adjacency_visible_payload_scan_vertex_set_directory_range_filtered(
    AgeAdjacencyVisiblePayloadScan *scan)
{
    if (scan == NULL)
        return 0;

    return scan->target.terminal_vertex_set_directory_range_filtered;
}

int64
age_adjacency_visible_payload_scan_vertex_set_directory_exact_filtered(
    AgeAdjacencyVisiblePayloadScan *scan)
{
    if (scan == NULL)
        return 0;

    return scan->target.terminal_vertex_set_directory_exact_filtered;
}

int64
age_adjacency_visible_payload_scan_vertex_set_directory_label_bloom_filtered(
    AgeAdjacencyVisiblePayloadScan *scan)
{
    if (scan == NULL)
        return 0;

    return scan->target.terminal_vertex_set_directory_label_bloom_filtered;
}

int64
age_adjacency_visible_payload_scan_vertex_set_directory_compressed_filtered(
    AgeAdjacencyVisiblePayloadScan *scan)
{
    if (scan == NULL)
        return 0;

    return scan->target.terminal_vertex_set_directory_compressed_filtered;
}

int64
age_adjacency_visible_payload_scan_vertex_set_directory_wide_bloom_filtered(
    AgeAdjacencyVisiblePayloadScan *scan)
{
    if (scan == NULL)
        return 0;

    return scan->target.terminal_vertex_set_directory_wide_bloom_filtered;
}

int64
age_adjacency_visible_payload_scan_vertex_set_directory_value_filtered(
    AgeAdjacencyVisiblePayloadScan *scan)
{
    if (scan == NULL)
        return 0;

    return scan->target.terminal_vertex_set_directory_value_filtered;
}

int64
age_adjacency_visible_payload_scan_vertex_set_directory_value_posting_filtered(
    AgeAdjacencyVisiblePayloadScan *scan)
{
    if (scan == NULL)
        return 0;

    return scan->target.terminal_vertex_set_directory_value_posting_filtered;
}

int64
age_adjacency_visible_payload_scan_composite_requests(
    AgeAdjacencyVisiblePayloadScan *scan)
{
    if (scan == NULL)
        return 0;

    return scan->target.terminal_composite_requests;
}

int64
age_adjacency_visible_payload_scan_composite_block_filtered(
    AgeAdjacencyVisiblePayloadScan *scan)
{
    if (scan == NULL)
        return 0;

    return scan->target.terminal_composite_block_filtered;
}

int64
age_adjacency_visible_payload_scan_composite_directory_filtered(
    AgeAdjacencyVisiblePayloadScan *scan)
{
    if (scan == NULL)
        return 0;

    return scan->target.terminal_composite_directory_filtered;
}

int64
age_adjacency_visible_payload_scan_composite_directory_estimated(
    AgeAdjacencyVisiblePayloadScan *scan)
{
    if (scan == NULL)
        return 0;

    return scan->target.terminal_composite_directory_estimated;
}

void
age_adjacency_visible_payload_scan_reset_runtime(
    AgeAdjacencyVisiblePayloadScan *scan)
{
    if (scan == NULL)
        return;

    scan->target.terminal_label_filtered = 0;
    scan->target.terminal_directory_label_filtered = 0;
    scan->target.terminal_property_filtered = 0;
    scan->target.terminal_cache_filtered = 0;
    scan->target.terminal_cache_label_filtered = 0;
    scan->target.terminal_cache_property_filtered = 0;
    scan->target.terminal_vertex_set_range_filtered = 0;
    scan->target.terminal_vertex_set_sorted_filtered = 0;
    scan->target.terminal_vertex_set_block_filtered = 0;
    scan->target.terminal_vertex_set_block_value_filtered = 0;
    scan->target.terminal_vertex_set_block_value_posting_filtered = 0;
    scan->target.terminal_vertex_set_block_range_filtered = 0;
    scan->target.terminal_vertex_set_block_exact_filtered = 0;
    scan->target.terminal_vertex_set_block_compressed_filtered = 0;
    scan->target.terminal_vertex_set_block_bloom_filtered = 0;
    scan->target.terminal_vertex_set_block_posting_filtered = 0;
    scan->target.terminal_vertex_set_directory_filtered = 0;
    scan->target.terminal_vertex_set_directory_range_filtered = 0;
    scan->target.terminal_vertex_set_directory_exact_filtered = 0;
    scan->target.terminal_vertex_set_directory_label_bloom_filtered = 0;
    scan->target.terminal_vertex_set_directory_compressed_filtered = 0;
    scan->target.terminal_vertex_set_directory_wide_bloom_filtered = 0;
    scan->target.terminal_vertex_set_directory_value_filtered = 0;
    scan->target.terminal_vertex_set_directory_value_posting_filtered = 0;
    scan->target.terminal_composite_requests = 0;
    scan->target.terminal_composite_block_filtered = 0;
    scan->target.terminal_composite_directory_filtered = 0;
    scan->target.terminal_composite_directory_estimated = 0;
}

int64
age_adjacency_visible_payload_scan_foreach(
    AgeAdjacencyVisiblePayloadScan *scan, graphid key,
    AgeAdjacencyPayloadCallback callback, void *callback_state)
{
    AgeAdjacencyPayload payload;
    int64 matches = 0;

    if (scan == NULL || callback == NULL)
    {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("age_adjacency visible payload scan requires a "
                        "callback")));
    }

    (void) age_adjacency_visible_payload_scan_begin_key(scan, key);
    while (age_adjacency_visible_payload_scan_next(scan, &payload))
    {
        if (!callback(&payload, callback_state))
        {
            break;
        }
        matches++;
    }

    return matches;
}

static bool
age_adjacency_visible_payload_key_cursor_begin(
    AgeAdjacencyVisiblePayloadScan *scan,
    AgeAdjacencyVisiblePayloadKeyCursor *cursor, graphid key)
{
    AgeAdjacencyDirectoryEntryData entry;

    Assert(scan != NULL);
    Assert(cursor != NULL);

    memset(cursor, 0, sizeof(*cursor));
    cursor->active_key = key;
    cursor->key_active = true;
    cursor->main_active = false;
    cursor->main_remaining = 0;
    cursor->main_label_candidate_count = 0;
    cursor->main_blkno = InvalidBlockNumber;
    cursor->main_offnum = FirstOffsetNumber;
    cursor->main_posting_index = 0;
    cursor->main_cache_index = 0;
    cursor->main_composite_estimate_recorded = false;
    cursor->delta_blkno = scan->meta.first_delta_blkno;
    cursor->delta_offnum = FirstOffsetNumber;
    scan->main_cache.valid = false;
    scan->target.callback = NULL;
    scan->target.callback_state = NULL;

    if (age_adjacency_find_directory_entry_cached(scan, key, &entry))
    {
        bool label_mismatch;
        bool property_mismatch;

        if (age_adjacency_directory_entry_matches_composite_target(
                &entry, &scan->target, &label_mismatch,
                &property_mismatch))
        {
            age_adjacency_visible_payload_scan_record_composite_estimate(
                scan, &entry);
            cursor->main_active = true;
            cursor->main_remaining = entry.posting_count;
            cursor->main_label_candidate_count =
                age_adjacency_directory_entry_terminal_label_postings(
                    &entry, scan->target.terminal_label_id);
            cursor->initial_main_remaining = cursor->main_remaining;
            cursor->initial_main_label_candidate_count =
                cursor->main_label_candidate_count;
            cursor->main_blkno = entry.first_blkno;
            cursor->main_offnum = entry.first_offnum;
            cursor->main_posting_index = 0;
        }
        else
        {
            if (!label_mismatch)
                age_adjacency_visible_payload_scan_record_composite_estimate(
                    scan, &entry);
            if (label_mismatch)
            {
                scan->target.terminal_label_filtered += entry.posting_count;
                scan->target.terminal_directory_label_filtered +=
                    entry.posting_count;
                scan->target.terminal_cache_label_filtered +=
                    entry.posting_count;
            }
            else if (property_mismatch)
            {
                scan->target.terminal_property_filtered += entry.posting_count;
                scan->target.terminal_vertex_set_directory_filtered +=
                    entry.posting_count;
                if (age_adjacency_directory_entry_range_rejects(
                        &entry, &scan->target))
                {
                    scan->target.terminal_vertex_set_directory_range_filtered +=
                        entry.posting_count;
                }
                if (age_adjacency_directory_entry_exact_rejects(
                        &entry, &scan->target))
                {
                    scan->target.terminal_vertex_set_directory_exact_filtered +=
                        entry.posting_count;
                }
                if (age_adjacency_directory_entry_label_bloom_rejects(
                        &entry, &scan->target))
                {
                    scan->target.terminal_vertex_set_directory_label_bloom_filtered +=
                        entry.posting_count;
                }
                if (age_adjacency_directory_entry_compressed_rejects(
                        &entry, &scan->target))
                {
                    scan->target.terminal_vertex_set_directory_compressed_filtered +=
                        entry.posting_count;
                }
                if (age_adjacency_directory_entry_value_summary_rejects(
                        &entry, &scan->target))
                {
                    scan->target.terminal_vertex_set_directory_value_filtered +=
                        entry.posting_count;
                }
                if (age_adjacency_directory_entry_value_posting_rejects(
                        &entry, &scan->target))
                {
                    scan->target.terminal_vertex_set_directory_value_posting_filtered +=
                        entry.posting_count;
                }
                if (age_adjacency_directory_entry_wide_bloom_rejects(
                        &entry, &scan->target))
                {
                    scan->target.terminal_vertex_set_directory_wide_bloom_filtered +=
                        entry.posting_count;
                }
                scan->target.terminal_cache_property_filtered +=
                    entry.posting_count;
                if (scan->target.has_terminal_property_summary)
                    scan->target.terminal_composite_directory_filtered +=
                        entry.posting_count;
            }
            scan->target.terminal_cache_filtered += entry.posting_count;
        }
    }

    return cursor->main_active || BlockNumberIsValid(cursor->delta_blkno);
}

static bool
age_adjacency_visible_payload_key_cursor_next(
    AgeAdjacencyVisiblePayloadScan *scan,
    AgeAdjacencyVisiblePayloadKeyCursor *cursor,
    AgeAdjacencyPayload *payload)
{
    Assert(scan != NULL);
    Assert(cursor != NULL);
    Assert(payload != NULL);

    if (!cursor->key_active)
        return false;

    if (age_adjacency_visible_payload_key_cursor_next_main(scan, cursor,
                                                           payload))
        return true;

    if (age_adjacency_visible_payload_key_cursor_next_delta(scan, cursor,
                                                            payload))
        return true;

    cursor->key_active = false;
    return false;
}

static bool
age_adjacency_visible_payload_key_cursor_next_main(
    AgeAdjacencyVisiblePayloadScan *scan,
    AgeAdjacencyVisiblePayloadKeyCursor *cursor,
    AgeAdjacencyPayload *payload)
{
    Assert(scan != NULL);
    Assert(cursor != NULL);
    Assert(payload != NULL);

    while (cursor->main_active && cursor->main_remaining > 0 &&
           BlockNumberIsValid(cursor->main_blkno))
    {
        scan->main_remaining = cursor->main_remaining;
        scan->main_offnum = cursor->main_offnum;
        scan->main_posting_index = cursor->main_posting_index;
        age_adjacency_load_main_cache(scan, cursor->main_blkno);
        cursor->main_remaining = scan->main_remaining;
        while (cursor->main_cache_index < scan->main_cache.count &&
               cursor->main_remaining > 0)
        {
            AgeAdjacencyCachedPosting *cached =
                &scan->main_cache.postings[cursor->main_cache_index++];
            AgeAdjacencyPostingData posting;

            cursor->main_remaining--;
            posting.key = cursor->active_key;
            ItemPointerCopy(&cached->posting.heap_tid, &posting.heap_tid);
            posting.edge_id = cached->posting.edge_id;
            posting.next_vertex_id = cached->posting.next_vertex_id;

            if (!age_adjacency_visible_payload_scan_accept_slice(scan, true))
                continue;

            if (age_adjacency_posting_visible_payload(&posting,
                                                      &scan->target,
                                                      payload))
                return true;
        }

        cursor->main_blkno = scan->main_cache.next_blkno;
        cursor->main_offnum = scan->main_cache.next_offnum;
        cursor->main_posting_index = scan->main_cache.next_posting_index;
        cursor->main_cache_index = 0;
        scan->main_cache.valid = false;
    }

    cursor->main_active = false;
    return false;
}

static bool
age_adjacency_visible_payload_key_cursor_next_delta(
    AgeAdjacencyVisiblePayloadScan *scan,
    AgeAdjacencyVisiblePayloadKeyCursor *cursor,
    AgeAdjacencyPayload *payload)
{
    Assert(scan != NULL);
    Assert(cursor != NULL);
    Assert(payload != NULL);

    while (BlockNumberIsValid(cursor->delta_blkno))
    {
        Buffer buf;
        Page page;
        AgeAdjacencyPageOpaque opaque;
        BlockNumber next_blkno;
        OffsetNumber maxoff;
        OffsetNumber offnum;

        buf = ReadBuffer(scan->index_rel, cursor->delta_blkno);
        LockBuffer(buf, BUFFER_LOCK_SHARE);
        page = BufferGetPage(buf);

        opaque = (AgeAdjacencyPageOpaque) PageGetSpecialPointer(page);
        if (opaque->magic != AGE_ADJACENCY_MAGIC ||
            opaque->version != AGE_ADJACENCY_VERSION ||
            opaque->page_type != AGE_ADJACENCY_PAGE_DELTA)
        {
            ereport(ERROR,
                    (errcode(ERRCODE_INDEX_CORRUPTED),
                     errmsg("age_adjacency delta page %u is invalid",
                            cursor->delta_blkno)));
        }

        next_blkno = opaque->next_blkno;
        if (!age_adjacency_delta_page_may_contain_key(opaque,
                                                      cursor->active_key))
        {
            cursor->delta_blkno = next_blkno;
            cursor->delta_offnum = FirstOffsetNumber;
            UnlockReleaseBuffer(buf);
            continue;
        }

        maxoff = PageGetMaxOffsetNumber(page);
        for (offnum = cursor->delta_offnum;
             offnum <= maxoff;
             offnum = OffsetNumberNext(offnum))
        {
            ItemId item_id = PageGetItemId(page, offnum);
            AgeAdjacencyDeltaCompactPosting delta_posting;
            AgeAdjacencyPostingData posting;

            if (!ItemIdIsNormal(item_id))
                continue;

            delta_posting = (AgeAdjacencyDeltaCompactPosting) PageGetItem(
                page, item_id);
            age_adjacency_delta_posting_get_posting(delta_posting, opaque,
                                                    &posting);
            if (posting.key != cursor->active_key)
                continue;

            cursor->delta_offnum = OffsetNumberNext(offnum);
            if (cursor->delta_offnum > maxoff)
            {
                cursor->delta_blkno = next_blkno;
                cursor->delta_offnum = FirstOffsetNumber;
            }
            if (!age_adjacency_visible_payload_scan_accept_slice(scan, false))
                continue;

            if (age_adjacency_posting_visible_payload(&posting,
                                                      &scan->target,
                                                      payload))
            {
                UnlockReleaseBuffer(buf);
                return true;
            }
        }

        cursor->delta_blkno = next_blkno;
        cursor->delta_offnum = FirstOffsetNumber;
        UnlockReleaseBuffer(buf);
    }

    return false;
}

static bool
age_adjacency_visible_payload_scan_next_main(
    AgeAdjacencyVisiblePayloadScan *scan, AgeAdjacencyPayload *payload)
{
    Assert(scan != NULL);
    Assert(payload != NULL);

    while (scan->main_active && scan->main_remaining > 0 &&
           BlockNumberIsValid(scan->main_blkno))
    {
        age_adjacency_load_main_cache(scan, scan->main_blkno);
        while (scan->main_cache_index < scan->main_cache.count &&
               scan->main_remaining > 0)
        {
            AgeAdjacencyCachedPosting *cached =
                &scan->main_cache.postings[scan->main_cache_index++];
            AgeAdjacencyPostingData posting;

            scan->main_remaining--;
            posting.key = scan->active_key;
            ItemPointerCopy(&cached->posting.heap_tid, &posting.heap_tid);
            posting.edge_id = cached->posting.edge_id;
            posting.next_vertex_id = cached->posting.next_vertex_id;

            if (!age_adjacency_visible_payload_scan_accept_slice(scan, true))
                continue;

            if (age_adjacency_posting_visible_payload(&posting,
                                                      &scan->target,
                                                      payload))
            {
                return true;
            }
        }

        scan->main_blkno = scan->main_cache.next_blkno;
        scan->main_offnum = scan->main_cache.next_offnum;
        scan->main_posting_index = scan->main_cache.next_posting_index;
        scan->main_cache_index = 0;
        scan->main_cache.valid = false;
    }

    scan->main_active = false;
    return false;
}

static bool
age_adjacency_visible_payload_scan_next_delta(
    AgeAdjacencyVisiblePayloadScan *scan, AgeAdjacencyPayload *payload)
{
    Assert(scan != NULL);
    Assert(payload != NULL);

    while (BlockNumberIsValid(scan->delta_blkno))
    {
        Buffer buf;
        Page page;
        AgeAdjacencyPageOpaque opaque;
        BlockNumber next_blkno;
        OffsetNumber maxoff;
        OffsetNumber offnum;

        buf = ReadBuffer(scan->index_rel, scan->delta_blkno);
        LockBuffer(buf, BUFFER_LOCK_SHARE);
        page = BufferGetPage(buf);

        opaque = (AgeAdjacencyPageOpaque) PageGetSpecialPointer(page);
        if (opaque->magic != AGE_ADJACENCY_MAGIC ||
            opaque->version != AGE_ADJACENCY_VERSION ||
            opaque->page_type != AGE_ADJACENCY_PAGE_DELTA)
        {
            ereport(ERROR,
                    (errcode(ERRCODE_INDEX_CORRUPTED),
                     errmsg("age_adjacency delta page %u is invalid",
                            scan->delta_blkno)));
        }

        next_blkno = opaque->next_blkno;
        if (!age_adjacency_delta_page_may_contain_key(opaque,
                                                      scan->active_key))
        {
            scan->delta_blkno = next_blkno;
            scan->delta_offnum = FirstOffsetNumber;
            UnlockReleaseBuffer(buf);
            continue;
        }

        maxoff = PageGetMaxOffsetNumber(page);
        for (offnum = scan->delta_offnum;
             offnum <= maxoff;
             offnum = OffsetNumberNext(offnum))
        {
            ItemId item_id = PageGetItemId(page, offnum);
            AgeAdjacencyDeltaCompactPosting delta_posting;
            AgeAdjacencyPostingData posting;

            if (!ItemIdIsNormal(item_id))
            {
                continue;
            }

            delta_posting = (AgeAdjacencyDeltaCompactPosting) PageGetItem(
                page, item_id);
            age_adjacency_delta_posting_get_posting(delta_posting, opaque,
                                                    &posting);
            if (posting.key != scan->active_key)
            {
                continue;
            }

            scan->delta_offnum = OffsetNumberNext(offnum);
            if (scan->delta_offnum > maxoff)
            {
                scan->delta_blkno = next_blkno;
                scan->delta_offnum = FirstOffsetNumber;
            }
            if (!age_adjacency_visible_payload_scan_accept_slice(scan, false))
            {
                continue;
            }
            if (age_adjacency_posting_visible_payload(&posting,
                                                      &scan->target,
                                                      payload))
            {
                UnlockReleaseBuffer(buf);
                return true;
            }
        }

        scan->delta_blkno = next_blkno;
        scan->delta_offnum = FirstOffsetNumber;
        UnlockReleaseBuffer(buf);
    }

    return false;
}

static bool
age_adjacency_visible_payload_scan_accept_slice(
    AgeAdjacencyVisiblePayloadScan *scan, bool main_posting)
{
    uint64 ordinal;

    Assert(scan != NULL);

    if (main_posting)
        ordinal = scan->main_posting_ordinal++;
    else
        ordinal = scan->delta_posting_ordinal++;

    if (scan->parallel_slice_count <= 1)
        return true;

    return (ordinal % (uint64)scan->parallel_slice_count) ==
           (uint64)scan->parallel_slice_index;
}

void
age_adjacency_end_visible_payload_scan(AgeAdjacencyVisiblePayloadScan *scan)
{
    if (scan == NULL)
    {
        return;
    }

    if (BufferIsValid(scan->target.visibilitymap_buffer))
    {
        ReleaseBuffer(scan->target.visibilitymap_buffer);
    }

    ExecDropSingleTupleTableSlot(scan->slot);
    relation_close(scan->heap_rel, AccessShareLock);
    index_close(scan->index_rel, AccessShareLock);
    if (scan->directory_cache.entries != NULL)
    {
        pfree(scan->directory_cache.entries);
    }
    if (scan->main_cache.postings != NULL)
    {
        pfree(scan->main_cache.postings);
    }
    pfree(scan);
}

int64
age_adjacency_foreach_visible_payload(Oid index_oid, graphid key,
                                      Snapshot snapshot,
                                      bool fetch_properties,
                                      AgeAdjacencyPayloadCallback callback,
                                      void *callback_state)
{
    AgeAdjacencyVisiblePayloadScan *scan;
    int64 matches;

    if (callback == NULL)
    {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("age_adjacency visible payload scan requires a "
                        "callback")));
    }

    scan = age_adjacency_begin_visible_payload_scan(index_oid, snapshot,
                                                    fetch_properties);
    matches = age_adjacency_visible_payload_scan_foreach(scan, key, callback,
                                                        callback_state);
    age_adjacency_end_visible_payload_scan(scan);

    return matches;
}

Datum
age_adjacency_debug_payload(PG_FUNCTION_ARGS)
{
    Oid index_oid = PG_GETARG_OID(0);
    graphid key = AG_GETARG_GRAPHID(1);
    Relation index_rel;
    Relation heap_rel;
    TupleTableSlot *slot;
    AgeAdjacencyMetaPageData meta;
    AgeAdjacencyScanTarget target;
    ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;

    if (rsinfo == NULL ||
        !IsA(rsinfo, ReturnSetInfo) ||
        (rsinfo->allowedModes & SFRM_Materialize) == 0)
    {
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("materialize mode required")));
    }

    InitMaterializedSRF(fcinfo, MAT_SRF_USE_EXPECTED_DESC);

    index_rel = index_open(index_oid, AccessShareLock);
    age_adjacency_validate_index(index_rel);
    age_adjacency_read_meta(index_rel, &meta);

    heap_rel = relation_open(meta.heap_relid, AccessShareLock);
    slot = table_slot_create(heap_rel, NULL);

    memset(&target, 0, sizeof(target));
    target.tupstore = rsinfo->setResult;
    target.tupdesc = rsinfo->setDesc;
    target.heap_rel = heap_rel;
    target.snapshot = GetActiveSnapshot();
    target.slot = slot;

    age_adjacency_scan_payload(index_rel, key, &target);

    ExecDropSingleTupleTableSlot(slot);
    relation_close(heap_rel, AccessShareLock);
    index_close(index_rel, AccessShareLock);

    return (Datum) 0;
}

Datum
age_adjacency_candidate_edges(PG_FUNCTION_ARGS)
{
    Oid index_oid = PG_GETARG_OID(0);
    graphid key = AG_GETARG_GRAPHID(1);
    Relation index_rel;
    Relation heap_rel;
    TupleTableSlot *slot;
    AgeAdjacencyMetaPageData meta;
    AgeAdjacencyScanTarget target;
    AgeAdjacencyCandidateStore store;
    ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;

    if (rsinfo == NULL ||
        !IsA(rsinfo, ReturnSetInfo) ||
        (rsinfo->allowedModes & SFRM_Materialize) == 0)
    {
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("materialize mode required")));
    }

    InitMaterializedSRF(fcinfo, MAT_SRF_USE_EXPECTED_DESC);

    index_rel = index_open(index_oid, AccessShareLock);
    age_adjacency_validate_index(index_rel);
    age_adjacency_read_meta(index_rel, &meta);

    heap_rel = relation_open(meta.heap_relid, AccessShareLock);
    slot = table_slot_create(heap_rel, NULL);

    store.tupstore = rsinfo->setResult;
    store.tupdesc = rsinfo->setDesc;

    memset(&target, 0, sizeof(target));
    target.heap_rel = heap_rel;
    target.snapshot = GetActiveSnapshot();
    target.slot = slot;
    target.callback = age_adjacency_store_candidate;
    target.callback_state = &store;

    age_adjacency_scan_payload(index_rel, key, &target);

    ExecDropSingleTupleTableSlot(slot);
    relation_close(heap_rel, AccessShareLock);
    index_close(index_rel, AccessShareLock);

    return (Datum) 0;
}

Datum
age_adjacency_candidate_edge_rows(PG_FUNCTION_ARGS)
{
    Oid index_oid = PG_GETARG_OID(0);
    graphid key = AG_GETARG_GRAPHID(1);
    bool outgoing = PG_GETARG_BOOL(2);
    Relation index_rel;
    Relation heap_rel;
    TupleTableSlot *slot;
    AgeAdjacencyMetaPageData meta;
    AgeAdjacencyScanTarget target;
    AgeAdjacencyCandidateRowStore store;
    ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;

    if (rsinfo == NULL ||
        !IsA(rsinfo, ReturnSetInfo) ||
        (rsinfo->allowedModes & SFRM_Materialize) == 0)
    {
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("materialize mode required")));
    }

    InitMaterializedSRF(fcinfo, MAT_SRF_USE_EXPECTED_DESC);

    index_rel = index_open(index_oid, AccessShareLock);
    age_adjacency_validate_index(index_rel);
    age_adjacency_read_meta(index_rel, &meta);

    heap_rel = relation_open(meta.heap_relid, AccessShareLock);
    slot = table_slot_create(heap_rel, NULL);

    store.tupstore = rsinfo->setResult;
    store.tupdesc = rsinfo->setDesc;
    store.source_vertex_id = key;
    store.outgoing = outgoing;

    memset(&target, 0, sizeof(target));
    target.heap_rel = heap_rel;
    target.snapshot = GetActiveSnapshot();
    target.slot = slot;
    target.callback = age_adjacency_store_candidate_row;
    target.callback_state = &store;

    age_adjacency_scan_payload(index_rel, key, &target);

    ExecDropSingleTupleTableSlot(slot);
    relation_close(heap_rel, AccessShareLock);
    index_close(index_rel, AccessShareLock);

    return (Datum) 0;
}

Datum
age_adjacency_debug_stats(PG_FUNCTION_ARGS)
{
    Oid index_oid = PG_GETARG_OID(0);
    Relation index_rel;
    AgeAdjacencyMetaPageData meta;
    ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
    Datum values[7];
    bool nulls[7] = {false, false, false, false, false, false, false};

    if (rsinfo == NULL ||
        !IsA(rsinfo, ReturnSetInfo) ||
        (rsinfo->allowedModes & SFRM_Materialize) == 0)
    {
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("materialize mode required")));
    }

    InitMaterializedSRF(fcinfo, MAT_SRF_USE_EXPECTED_DESC);

    index_rel = index_open(index_oid, AccessShareLock);
    age_adjacency_validate_index(index_rel);
    age_adjacency_read_meta(index_rel, &meta);

    values[0] = Int32GetDatum((int32) meta.version);
    values[1] = Int64GetDatum((int64) RelationGetNumberOfBlocks(index_rel));
    values[2] = Int64GetDatum((int64) meta.postings);
    values[3] = Int64GetDatum((int64) meta.directory_entries);
    values[4] = Int64GetDatum((int64) meta.delta_postings);
    values[5] = Int64GetDatum((int64) AGE_ADJACENCY_DELTA_REINDEX_THRESHOLD);
    values[6] = BoolGetDatum(meta.delta_postings >=
                             AGE_ADJACENCY_DELTA_REINDEX_THRESHOLD);
    tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);

    index_close(index_rel, AccessShareLock);

    return (Datum) 0;
}

Datum
age_adjacency_debug_directory_probe(PG_FUNCTION_ARGS)
{
    Oid index_oid = PG_GETARG_OID(0);
    graphid key = AG_GETARG_GRAPHID(1);
    Relation index_rel;
    ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
    Datum values[3];
    bool nulls[3] = {false, false, false};
    bool found;
    int64 pages_visited;
    int64 entries_scanned;

    if (rsinfo == NULL ||
        !IsA(rsinfo, ReturnSetInfo) ||
        (rsinfo->allowedModes & SFRM_Materialize) == 0)
    {
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("materialize mode required")));
    }

    InitMaterializedSRF(fcinfo, MAT_SRF_USE_EXPECTED_DESC);

    index_rel = index_open(index_oid, AccessShareLock);
    age_adjacency_validate_index(index_rel);

    age_adjacency_probe_directory(index_rel, key, &found, &pages_visited,
                                  &entries_scanned);

    values[0] = BoolGetDatum(found);
    values[1] = Int64GetDatum(pages_visited);
    values[2] = Int64GetDatum(entries_scanned);
    tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);

    index_close(index_rel, AccessShareLock);

    return (Datum) 0;
}

Datum
age_adjacency_debug_key_known_empty(PG_FUNCTION_ARGS)
{
    Oid index_oid = PG_GETARG_OID(0);
    graphid key = AG_GETARG_GRAPHID(1);
    int32 terminal_label_id = PG_GETARG_INT32(2);
    AgeAdjacencyVisiblePayloadScan *scan;
    bool known_empty;

    scan = age_adjacency_begin_visible_payload_scan(index_oid,
                                                    GetActiveSnapshot(),
                                                    false);
    age_adjacency_visible_payload_scan_set_terminal_label(scan,
                                                          terminal_label_id);
    known_empty = age_adjacency_visible_payload_scan_key_known_empty(scan,
                                                                    key);
    age_adjacency_end_visible_payload_scan(scan);

    PG_RETURN_BOOL(known_empty);
}

Datum
age_adjacency_debug_key_known_empty_range(PG_FUNCTION_ARGS)
{
    Oid index_oid = PG_GETARG_OID(0);
    graphid key = AG_GETARG_GRAPHID(1);
    int32 terminal_label_id = PG_GETARG_INT32(2);
    graphid min_vertex_id = AG_GETARG_GRAPHID(3);
    graphid max_vertex_id = AG_GETARG_GRAPHID(4);
    AgeAdjacencyVisiblePayloadScan *scan;
    AgeAdjacencyVertexSetFilter filter;
    bool known_empty;

    scan = age_adjacency_begin_visible_payload_scan(index_oid,
                                                    GetActiveSnapshot(),
                                                    false);
    age_adjacency_visible_payload_scan_set_terminal_label(scan,
                                                          terminal_label_id);
    memset(&filter, 0, sizeof(filter));
    filter.min_vertex_id = min_vertex_id;
    filter.max_vertex_id = max_vertex_id;
    filter.has_range = true;
    age_adjacency_visible_payload_scan_set_terminal_vertex_set_filter(scan,
                                                                      &filter);
    known_empty = age_adjacency_visible_payload_scan_key_known_empty(scan,
                                                                    key);
    age_adjacency_end_visible_payload_scan(scan);

    PG_RETURN_BOOL(known_empty);
}

Datum
age_adjacency_debug_composite_probe(PG_FUNCTION_ARGS)
{
    Oid index_oid = PG_GETARG_OID(0);
    graphid key = AG_GETARG_GRAPHID(1);
    int32 terminal_label_id = PG_GETARG_INT32(2);
    graphid matched_vertex_id = AG_GETARG_GRAPHID(3);
    int64 property_match_count = PG_GETARG_INT64(4);
    AgeAdjacencyVisiblePayloadScan *scan;
    AgeAdjacencyCompositeTerminalFilter filter;
    HASHCTL hash_ctl;
    bool found;
    int64 emitted = 0;
    ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
    Datum values[24];
    bool nulls[24] = {false, false, false, false, false, false, false,
                      false, false, false, false, false, false, false,
                      false, false, false, false, false, false, false,
                      false, false, false};

    if (rsinfo == NULL ||
        !IsA(rsinfo, ReturnSetInfo) ||
        (rsinfo->allowedModes & SFRM_Materialize) == 0)
    {
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("materialize mode required")));
    }

    InitMaterializedSRF(fcinfo, MAT_SRF_USE_EXPECTED_DESC);

    memset(&hash_ctl, 0, sizeof(hash_ctl));
    hash_ctl.keysize = sizeof(graphid);
    hash_ctl.entrysize = sizeof(graphid);
    hash_ctl.hcxt = CurrentMemoryContext;

    memset(&filter, 0, sizeof(filter));
    filter.terminal_label_id = terminal_label_id;
    filter.property_match_count = property_match_count;
    filter.vertex_set_filter.vertex_ids =
        hash_create("AGE adjacency debug composite matched vertices",
                    1, &hash_ctl,
                    HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
    (void) hash_search(filter.vertex_set_filter.vertex_ids,
                       &matched_vertex_id, HASH_ENTER, &found);
    filter.vertex_set_filter.sorted_vertex_ids = &matched_vertex_id;
    filter.vertex_set_filter.sorted_vertex_count = 1;
    filter.vertex_set_filter.min_vertex_id = matched_vertex_id;
    filter.vertex_set_filter.max_vertex_id = matched_vertex_id;
    filter.vertex_set_filter.matches = property_match_count;
    filter.vertex_set_filter.source = "debug-composite";
    filter.vertex_set_filter.has_range = true;
    filter.vertex_set_filter.has_sorted_vertex_ids = true;
    filter.source = "debug-composite";
    filter.property_filter_id = 1;
    filter.has_property_summary = true;
    filter.has_vertex_set_filter = true;

    scan = age_adjacency_begin_visible_payload_scan(index_oid,
                                                    GetActiveSnapshot(),
                                                    false);
    age_adjacency_visible_payload_scan_set_composite_terminal_filter(scan,
                                                                     &filter);
    if (age_adjacency_visible_payload_scan_begin_key(scan, key))
    {
        AgeAdjacencyPayload payload;

        while (age_adjacency_visible_payload_scan_next(scan, &payload))
            emitted++;
    }

    values[0] = Int64GetDatum(emitted);
    values[1] = Int64GetDatum(
        age_adjacency_visible_payload_scan_cache_filtered(scan));
    values[2] = Int64GetDatum(
        age_adjacency_visible_payload_scan_cache_property_filtered(scan));
    values[3] = Int64GetDatum(
        age_adjacency_visible_payload_scan_vertex_set_range_filtered(scan));
    values[4] = Int64GetDatum(
        age_adjacency_visible_payload_scan_vertex_set_sorted_filtered(scan));
    values[5] = Int64GetDatum(
        age_adjacency_visible_payload_scan_vertex_set_block_filtered(scan));
    values[6] = Int64GetDatum(
        scan->target.terminal_vertex_set_block_value_filtered);
    values[7] = Int64GetDatum(
        scan->target.terminal_vertex_set_block_value_posting_filtered);
    values[8] = Int64GetDatum(
        age_adjacency_visible_payload_scan_vertex_set_directory_filtered(scan));
    values[9] = Int64GetDatum(
        age_adjacency_visible_payload_scan_composite_block_filtered(scan));
    values[10] = Int64GetDatum(
        age_adjacency_visible_payload_scan_composite_directory_filtered(scan));
    values[11] = Int64GetDatum(
        age_adjacency_visible_payload_scan_composite_directory_estimated(scan));
    values[12] = Int64GetDatum(
        scan->target.terminal_vertex_set_block_range_filtered);
    values[13] = Int64GetDatum(
        scan->target.terminal_vertex_set_block_exact_filtered);
    values[14] = Int64GetDatum(
        scan->target.terminal_vertex_set_block_compressed_filtered);
    values[15] = Int64GetDatum(
        scan->target.terminal_vertex_set_block_bloom_filtered);
    values[16] = Int64GetDatum(
        scan->target.terminal_vertex_set_block_posting_filtered);
    values[17] = Int64GetDatum(
        scan->target.terminal_vertex_set_directory_range_filtered);
    values[18] = Int64GetDatum(
        scan->target.terminal_vertex_set_directory_exact_filtered);
    values[19] = Int64GetDatum(
        scan->target.terminal_vertex_set_directory_label_bloom_filtered);
    values[20] = Int64GetDatum(
        scan->target.terminal_vertex_set_directory_compressed_filtered);
    values[21] = Int64GetDatum(
        scan->target.terminal_vertex_set_directory_wide_bloom_filtered);
    values[22] = Int64GetDatum(
        scan->target.terminal_vertex_set_directory_value_filtered);
    values[23] = Int64GetDatum(
        scan->target.terminal_vertex_set_directory_value_posting_filtered);
    tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);

    age_adjacency_end_visible_payload_scan(scan);

    return (Datum) 0;
}

Datum
age_adjacency_debug_main_probe(PG_FUNCTION_ARGS)
{
    Oid index_oid = PG_GETARG_OID(0);
    graphid key = AG_GETARG_GRAPHID(1);
    Relation index_rel;
    AgeAdjacencyMetaPageData meta;
    AgeAdjacencyMainProbeStats stats;
    ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
    Datum values[9];
    bool nulls[9] = {false, false, false, false, false, false, false, false,
                     false};

    if (rsinfo == NULL ||
        !IsA(rsinfo, ReturnSetInfo) ||
        (rsinfo->allowedModes & SFRM_Materialize) == 0)
    {
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("materialize mode required")));
    }

    InitMaterializedSRF(fcinfo, MAT_SRF_USE_EXPECTED_DESC);

    index_rel = index_open(index_oid, AccessShareLock);
    age_adjacency_validate_index(index_rel);
    age_adjacency_read_meta(index_rel, &meta);

    age_adjacency_probe_main_run(index_rel, key, &meta, &stats);

    values[0] = BoolGetDatum(stats.found);
    values[1] = Int64GetDatum(stats.pages_visited);
    values[2] = Int64GetDatum(stats.window_offsets);
    values[3] = Int64GetDatum(stats.page_offsets);
    values[4] = Int64GetDatum(stats.block_items);
    values[5] = Int64GetDatum(stats.compact_block_items);
    values[6] = Int64GetDatum(stats.full_block_items);
    values[7] = Int64GetDatum(stats.entries_cached);
    values[8] = Int64GetDatum(stats.label_groups);
    tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);

    index_close(index_rel, AccessShareLock);

    return (Datum) 0;
}

Datum
age_adjacency_debug_delta_probe(PG_FUNCTION_ARGS)
{
    Oid index_oid = PG_GETARG_OID(0);
    graphid key = AG_GETARG_GRAPHID(1);
    Relation index_rel;
    AgeAdjacencyMetaPageData meta;
    AgeAdjacencyDeltaProbeStats stats;
    ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
    Datum values[3];
    bool nulls[3] = {false, false, false};

    if (rsinfo == NULL ||
        !IsA(rsinfo, ReturnSetInfo) ||
        (rsinfo->allowedModes & SFRM_Materialize) == 0)
    {
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("materialize mode required")));
    }

    InitMaterializedSRF(fcinfo, MAT_SRF_USE_EXPECTED_DESC);

    index_rel = index_open(index_oid, AccessShareLock);
    age_adjacency_validate_index(index_rel);
    age_adjacency_read_meta(index_rel, &meta);

    age_adjacency_probe_delta_pages(index_rel, key, &meta, &stats);

    values[0] = Int64GetDatum(stats.pages_visited);
    values[1] = Int64GetDatum(stats.pages_skipped);
    values[2] = Int64GetDatum(stats.entries_scanned);
    tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);

    index_close(index_rel, AccessShareLock);

    return (Datum) 0;
}

Datum
age_adjacency_debug_delta_maintenance(PG_FUNCTION_ARGS)
{
    Oid index_oid = PG_GETARG_OID(0);
    Relation index_rel;
    AgeAdjacencyMetaPageData meta;
    BlockNumber index_pages;
    int64 delta_pages;
    int64 delta_tuples_per_page;
    const char *action;
    const char *reason;
    ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
    Datum values[7];
    bool nulls[7] = {false, false, false, false, false, false, false};

    if (rsinfo == NULL ||
        !IsA(rsinfo, ReturnSetInfo) ||
        (rsinfo->allowedModes & SFRM_Materialize) == 0)
    {
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("materialize mode required")));
    }

    InitMaterializedSRF(fcinfo, MAT_SRF_USE_EXPECTED_DESC);

    index_rel = index_open(index_oid, AccessShareLock);
    age_adjacency_validate_index(index_rel);
    age_adjacency_read_meta(index_rel, &meta);
    index_pages = RelationGetNumberOfBlocks(index_rel);

    action = age_adjacency_delta_maintenance_action(
        &meta, index_pages, &delta_pages, &delta_tuples_per_page, &reason);

    values[0] = CStringGetTextDatum(action);
    values[1] = CStringGetTextDatum(reason);
    values[2] = Int64GetDatum((int64) meta.delta_postings);
    values[3] = Int64GetDatum(delta_pages);
    values[4] = Int64GetDatum(delta_tuples_per_page);
    values[5] = Int64GetDatum((int64) AGE_ADJACENCY_DELTA_REINDEX_THRESHOLD);
    values[6] = BoolGetDatum(meta.delta_postings >=
                             AGE_ADJACENCY_DELTA_REINDEX_THRESHOLD);
    tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);

    index_close(index_rel, AccessShareLock);

    return (Datum) 0;
}

Datum
age_adjacency_reindex_if_needed(PG_FUNCTION_ARGS)
{
    Oid index_oid = PG_GETARG_OID(0);
    Relation index_rel;
    AgeAdjacencyMetaPageData before_meta;
    AgeAdjacencyMetaPageData after_meta;
    BlockNumber index_pages;
    int64 delta_pages;
    int64 delta_tuples_per_page;
    const char *action;
    const char *reason;
    bool should_reindex;
    ReindexParams params = {0};
    ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
    Datum values[6];
    bool nulls[6] = {false, false, false, false, false, false};

    if (rsinfo == NULL ||
        !IsA(rsinfo, ReturnSetInfo) ||
        (rsinfo->allowedModes & SFRM_Materialize) == 0)
    {
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("materialize mode required")));
    }

    InitMaterializedSRF(fcinfo, MAT_SRF_USE_EXPECTED_DESC);

    index_rel = index_open(index_oid, AccessShareLock);
    age_adjacency_validate_index(index_rel);
    age_adjacency_read_meta(index_rel, &before_meta);
    index_pages = RelationGetNumberOfBlocks(index_rel);
    index_close(index_rel, AccessShareLock);

    action = age_adjacency_delta_maintenance_action(
        &before_meta, index_pages, &delta_pages, &delta_tuples_per_page,
        &reason);
    should_reindex = strcmp(action, "reindex-delta") == 0;

    if (should_reindex)
    {
        char persistence = get_rel_persistence(index_oid);

        params.options = REINDEXOPT_REPORT_PROGRESS;
        params.tablespaceOid = InvalidOid;
        reindex_index(NULL, index_oid, false, persistence, &params);
    }

    index_rel = index_open(index_oid, AccessShareLock);
    age_adjacency_read_meta(index_rel, &after_meta);
    index_close(index_rel, AccessShareLock);

    values[0] = BoolGetDatum(should_reindex);
    values[1] = CStringGetTextDatum(action);
    values[2] = CStringGetTextDatum(reason);
    values[3] = Int64GetDatum((int64) before_meta.delta_postings);
    values[4] = Int64GetDatum((int64) after_meta.delta_postings);
    values[5] = Int64GetDatum((int64) after_meta.postings);
    tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);

    return (Datum) 0;
}

Datum
age_adjacency_handler(PG_FUNCTION_ARGS)
{
    IndexAmRoutine *amroutine = makeNode(IndexAmRoutine);

    amroutine->amstrategies = 1;
    amroutine->amsupport = 0;
    amroutine->amoptsprocnum = 0;
    amroutine->amcanorder = false;
    amroutine->amcanorderbyop = false;
    amroutine->amcanhash = false;
    amroutine->amconsistentequality = true;
    amroutine->amconsistentordering = false;
    amroutine->amcanbackward = false;
    amroutine->amcanunique = false;
    amroutine->amcanmulticol = true;
    amroutine->amoptionalkey = false;
    amroutine->amsearcharray = false;
    amroutine->amsearchnulls = false;
    amroutine->amstorage = false;
    amroutine->amclusterable = false;
    amroutine->ampredlocks = false;
    amroutine->amcanparallel = false;
    amroutine->amcanbuildparallel = false;
    amroutine->amcaninclude = false;
    amroutine->amusemaintenanceworkmem = false;
    amroutine->amsummarizing = false;
    amroutine->amparallelvacuumoptions = 0;
    amroutine->amkeytype = InvalidOid;

    amroutine->ambuild = age_adjacency_build;
    amroutine->ambuildempty = age_adjacency_build_empty;
    amroutine->aminsert = age_adjacency_insert;
    amroutine->aminsertcleanup = NULL;
    amroutine->ambulkdelete = age_adjacency_bulk_delete;
    amroutine->amvacuumcleanup = age_adjacency_vacuum_cleanup;
    amroutine->amcanreturn = NULL;
    amroutine->amcostestimate = age_adjacency_cost_estimate;
    amroutine->amgettreeheight = NULL;
    amroutine->amoptions = age_adjacency_options;
    amroutine->amproperty = NULL;
    amroutine->ambuildphasename = NULL;
    amroutine->amvalidate = age_adjacency_validate;
    amroutine->amadjustmembers = NULL;
    amroutine->ambeginscan = age_adjacency_begin_scan;
    amroutine->amrescan = age_adjacency_rescan;
    amroutine->amgettuple = NULL;
    amroutine->amgetbitmap = age_adjacency_get_bitmap;
    amroutine->amendscan = age_adjacency_end_scan;
    amroutine->ammarkpos = NULL;
    amroutine->amrestrpos = NULL;
    amroutine->amestimateparallelscan = NULL;
    amroutine->aminitparallelscan = NULL;
    amroutine->amparallelrescan = NULL;
    amroutine->amtranslatestrategy = NULL;
    amroutine->amtranslatecmptype = NULL;

    PG_RETURN_POINTER(amroutine);
}
