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

#ifndef AGE_ADJACENCY_H
#define AGE_ADJACENCY_H

#include "postgres.h"

#include "port/atomics.h"
#include "storage/itemptr.h"
#include "utils/graphid.h"
#include "utils/hsearch.h"
#include "utils/snapshot.h"

typedef struct AgeAdjacencyPayload
{
    ItemPointerData heap_tid;
    graphid edge_id;
    graphid next_vertex_id;
    Datum properties;
    bool properties_isnull;
} AgeAdjacencyPayload;

typedef bool (*AgeAdjacencyPayloadCallback) (const AgeAdjacencyPayload *payload,
                                             void *callback_state);
typedef bool (*AgeAdjacencyVertexFilterCallback) (graphid vertex_id,
                                                  void *callback_state);

typedef struct AgeAdjacencyVertexSetFilter
{
    HTAB *vertex_ids;
    const graphid *sorted_vertex_ids;
    const char *source;
    int64 matches;
    int64 sorted_vertex_count;
    graphid min_vertex_id;
    graphid max_vertex_id;
    uint64 value_bloom;
    uint64 value_bloom_wide[4];
    uint64 value_posting_bloom[8];
    uint32 value_filter_id;
    bool has_range;
    bool has_sorted_vertex_ids;
    bool has_value_summary;
} AgeAdjacencyVertexSetFilter;

typedef struct AgeAdjacencyCompositeTerminalFilter
{
    int32 terminal_label_id;
    Oid property_index_oid;
    uint32 property_filter_id;
    int64 property_match_count;
    AgeAdjacencyVertexFilterCallback vertex_filter;
    void *vertex_filter_state;
    AgeAdjacencyVertexSetFilter vertex_set_filter;
    const char *source;
    bool has_property_summary;
    bool has_vertex_filter;
    bool has_vertex_set_filter;
} AgeAdjacencyCompositeTerminalFilter;

typedef struct AgeAdjacencyVisiblePayloadScan AgeAdjacencyVisiblePayloadScan;
typedef struct AgeAdjacencyVisiblePayloadRunScan
    AgeAdjacencyVisiblePayloadRunScan;

typedef struct AgeAdjacencyVisiblePayloadRunKey
{
    graphid key;
    void *tag;
} AgeAdjacencyVisiblePayloadRunKey;

typedef struct AgeAdjacencyVisiblePayloadRunKeyEvidence
{
    int64 run_postings;
    int64 terminal_postings;
    bool active;
    bool known_empty;
} AgeAdjacencyVisiblePayloadRunKeyEvidence;

typedef struct AgeAdjacencyVisiblePayloadRunGroupEvidence
{
    int64 seed_groups;
    int64 seed_group_cursors;
    int64 shared_page_seed_groups;
    int64 shared_page_seed_cursors;
    int64 shared_page_run_block_groups;
    int64 shared_page_run_block_cursors;
    int64 shared_page_run_block_intersections;
    int64 shared_page_run_block_intersection_cursors;
    int64 shared_page_run_block_intersection_skips;
    int64 shared_page_run_block_direct_seeds;
    int64 shared_page_run_block_direct_seed_cursors;
    int64 shared_page_run_block_streams;
    int64 shared_page_run_block_stream_cursors;
    int64 shared_page_run_block_stream_positions;
    int64 shared_page_run_block_full_group_drains;
    int64 shared_page_run_block_full_group_drain_cursors;
    int64 shared_page_fallbacks;
    int64 shared_page_fallback_regroups;
    int64 shared_page_fallback_regroup_cursors;
} AgeAdjacencyVisiblePayloadRunGroupEvidence;

typedef struct AgeAdjacencyVisiblePayloadRunNextBatch
{
    BlockNumber blkno;
    OffsetNumber offnum;
    uint16 position_index;
    uint16 position_count;
    int64 source_count;
    bool shared_run_block_stream;
} AgeAdjacencyVisiblePayloadRunNextBatch;

typedef struct AgeAdjacencyVisiblePayloadRunNextItem
{
    AgeAdjacencyPayload payload;
    void *tag;
    AgeAdjacencyVisiblePayloadRunNextBatch batch;
} AgeAdjacencyVisiblePayloadRunNextItem;

typedef enum VLESourceValuePostingKind
{
    VLE_SOURCE_VALUE_POSTING_NONE = 0,
    VLE_SOURCE_VALUE_POSTING_RUN,
    VLE_SOURCE_VALUE_POSTING_LABEL_SLICE,
    VLE_SOURCE_VALUE_POSTING_OTHER
} VLESourceValuePostingKind;

typedef struct AgeAdjacencyTerminalLabelPostingEstimate
{
    graphid key;
    BlockNumber first_blkno;
    OffsetNumber first_offnum;
    int64 run_postings;
    int64 terminal_postings;
    int64 label_groups;
    VLESourceValuePostingKind value_posting_source_kind;
    int64 main_blocks;
    bool composite_matches;
    bool label_mismatch;
    bool property_mismatch;
    bool found;
} AgeAdjacencyTerminalLabelPostingEstimate;

typedef bool (*AgeAdjacencyVisiblePayloadRunFilterCallback) (
    int64 run_postings, int64 active_postings,
    AgeAdjacencyCompositeTerminalFilter *filter, bool *known_empty,
    void *callback_state);
typedef void (*AgeAdjacencyVisiblePayloadRunPayloadCallback) (
    void *tag, const AgeAdjacencyPayload *payload, void *callback_state);
typedef void (*AgeAdjacencyVisiblePayloadRunFilteredKeyCallback) (
    void *tag, void *callback_state);
typedef void (*AgeAdjacencyVisiblePayloadRunBlockBatchCallback) (
    void *const *tags, int64 tag_count, BlockNumber blkno,
    OffsetNumber offnum, const uint16 *positions, uint16 position_count,
    void *callback_state);

typedef struct AgeAdjacencyVisiblePayloadRunOptions
{
    int32 terminal_label_id;
    const AgeAdjacencyCompositeTerminalFilter *prepared_filter;
    const AgeAdjacencyTerminalLabelPostingEstimate *prepared_estimates;
    int64 prepared_estimate_count;
    bool prepared_filter_valid;
    bool prepared_known_empty;
    bool prepared_estimates_valid;
    bool prepared_seed_ordered;
    AgeAdjacencyVisiblePayloadRunFilterCallback filter_callback;
    void *filter_callback_state;
    AgeAdjacencyVisiblePayloadRunPayloadCallback payload_callback;
    void *payload_callback_state;
    AgeAdjacencyVisiblePayloadRunFilteredKeyCallback filtered_key_callback;
    void *filtered_key_callback_state;
    AgeAdjacencyVisiblePayloadRunBlockBatchCallback block_batch_callback;
    void *block_batch_callback_state;
} AgeAdjacencyVisiblePayloadRunOptions;

extern uint32 age_adjacency_property_filter_id(Oid property_index_oid,
                                               Datum property_value,
                                               bool property_value_isnull);
extern int64 age_adjacency_foreach_visible_payload(Oid index_oid,
                                                   graphid key,
                                                   Snapshot snapshot,
                                                   bool fetch_properties,
                                                   AgeAdjacencyPayloadCallback callback,
                                                   void *callback_state);
extern AgeAdjacencyVisiblePayloadScan *age_adjacency_begin_visible_payload_scan(
    Oid index_oid, Snapshot snapshot, bool fetch_properties);
extern void age_adjacency_visible_payload_scan_set_terminal_label(
    AgeAdjacencyVisiblePayloadScan *scan, int32 terminal_label_id);
extern void age_adjacency_visible_payload_scan_set_terminal_vertex_filter(
    AgeAdjacencyVisiblePayloadScan *scan,
    AgeAdjacencyVertexFilterCallback callback, void *callback_state);
extern void age_adjacency_visible_payload_scan_set_terminal_vertex_set_filter(
    AgeAdjacencyVisiblePayloadScan *scan,
    const AgeAdjacencyVertexSetFilter *filter);
extern void age_adjacency_visible_payload_scan_set_composite_terminal_filter(
    AgeAdjacencyVisiblePayloadScan *scan,
    const AgeAdjacencyCompositeTerminalFilter *filter);
extern bool age_adjacency_visible_payload_scan_begin_key(
    AgeAdjacencyVisiblePayloadScan *scan, graphid key);
extern void age_adjacency_visible_payload_scan_set_parallel_slice(
    AgeAdjacencyVisiblePayloadScan *scan, int32 slice_index,
    int32 slice_count);
extern void age_adjacency_visible_payload_scan_set_parallel_claim(
    AgeAdjacencyVisiblePayloadScan *scan, pg_atomic_uint64 *main_cursor,
    pg_atomic_uint64 *delta_cursor, uint32 chunk_size);
extern int64 age_adjacency_visible_payload_scan_run_postings(
    AgeAdjacencyVisiblePayloadScan *scan);
extern int64 age_adjacency_visible_payload_scan_active_postings(
    AgeAdjacencyVisiblePayloadScan *scan);
extern bool age_adjacency_estimate_terminal_label_postings(
    Oid index_oid, graphid key, int32 terminal_label_id,
    int64 *run_postings, int64 *terminal_postings, int64 *label_groups,
    VLESourceValuePostingKind *value_posting_source_kind,
    int64 *main_blocks);
extern bool age_adjacency_estimate_terminal_label_postings_batch(
    Oid index_oid, int32 terminal_label_id,
    AgeAdjacencyTerminalLabelPostingEstimate *estimates,
    int64 estimate_count);
extern bool age_adjacency_estimate_composite_terminal_postings_batch(
    Oid index_oid, const AgeAdjacencyCompositeTerminalFilter *filter,
    AgeAdjacencyTerminalLabelPostingEstimate *estimates,
    int64 estimate_count);
extern bool age_adjacency_visible_payload_scan_key_known_empty(
    AgeAdjacencyVisiblePayloadScan *scan, graphid key);
extern bool age_adjacency_visible_payload_scan_next(
    AgeAdjacencyVisiblePayloadScan *scan, AgeAdjacencyPayload *payload);
extern AgeAdjacencyVisiblePayloadRunScan *
age_adjacency_begin_visible_payload_run_scan(
    Oid index_oid, Snapshot snapshot, bool fetch_properties,
    int32 terminal_label_id, const graphid *keys, int64 key_count);
extern AgeAdjacencyVisiblePayloadRunScan *
age_adjacency_begin_visible_payload_run_scan_with_tags(
    Oid index_oid, Snapshot snapshot, bool fetch_properties,
    int32 terminal_label_id, const AgeAdjacencyVisiblePayloadRunKey *keys,
    int64 key_count);
extern AgeAdjacencyVisiblePayloadRunScan *
age_adjacency_begin_visible_payload_run_scan_with_options(
    Oid index_oid, Snapshot snapshot, bool fetch_properties,
    const AgeAdjacencyVisiblePayloadRunOptions *options,
    const AgeAdjacencyVisiblePayloadRunKey *keys, int64 key_count);
extern bool age_adjacency_visible_payload_run_scan_next(
    AgeAdjacencyVisiblePayloadRunScan *scan, AgeAdjacencyPayload *payload,
    int64 *key_index);
extern bool age_adjacency_visible_payload_run_scan_next_tag(
    AgeAdjacencyVisiblePayloadRunScan *scan, AgeAdjacencyPayload *payload,
    void **tag);
extern bool age_adjacency_visible_payload_run_scan_next_tag_batch(
    AgeAdjacencyVisiblePayloadRunScan *scan, AgeAdjacencyPayload *payload,
    void **tag, AgeAdjacencyVisiblePayloadRunNextBatch *batch);
extern int age_adjacency_visible_payload_run_scan_next_tag_batch_array(
    AgeAdjacencyVisiblePayloadRunScan *scan,
    const AgeAdjacencyVisiblePayloadRunNextBatch *seed_batch,
    AgeAdjacencyVisiblePayloadRunNextItem *items, int item_capacity);
extern bool age_adjacency_visible_payload_run_scan_key_seen(
    AgeAdjacencyVisiblePayloadRunScan *scan, int64 key_index);
extern bool age_adjacency_visible_payload_run_scan_key_evidence(
    AgeAdjacencyVisiblePayloadRunScan *scan, int64 key_index,
    AgeAdjacencyVisiblePayloadRunKeyEvidence *evidence);
extern bool age_adjacency_visible_payload_run_scan_group_evidence(
    AgeAdjacencyVisiblePayloadRunScan *scan,
    AgeAdjacencyVisiblePayloadRunGroupEvidence *evidence);
extern int64 age_adjacency_visible_payload_run_scan_active_keys(
    AgeAdjacencyVisiblePayloadRunScan *scan);
extern void age_adjacency_end_visible_payload_run_scan(
    AgeAdjacencyVisiblePayloadRunScan *scan);
extern int64 age_adjacency_visible_payload_scan_label_filtered(
    AgeAdjacencyVisiblePayloadScan *scan);
extern int64 age_adjacency_visible_payload_scan_directory_label_filtered(
    AgeAdjacencyVisiblePayloadScan *scan);
extern int64 age_adjacency_visible_payload_scan_property_filtered(
    AgeAdjacencyVisiblePayloadScan *scan);
extern int64 age_adjacency_visible_payload_scan_cache_filtered(
    AgeAdjacencyVisiblePayloadScan *scan);
extern int64 age_adjacency_visible_payload_scan_cache_label_filtered(
    AgeAdjacencyVisiblePayloadScan *scan);
extern int64 age_adjacency_visible_payload_scan_cache_property_filtered(
    AgeAdjacencyVisiblePayloadScan *scan);
extern int64 age_adjacency_visible_payload_scan_vertex_set_range_filtered(
    AgeAdjacencyVisiblePayloadScan *scan);
extern int64 age_adjacency_visible_payload_scan_vertex_set_sorted_filtered(
    AgeAdjacencyVisiblePayloadScan *scan);
extern int64 age_adjacency_visible_payload_scan_vertex_set_block_filtered(
    AgeAdjacencyVisiblePayloadScan *scan);
extern int64 age_adjacency_visible_payload_scan_vertex_set_block_value_filtered(
    AgeAdjacencyVisiblePayloadScan *scan);
extern int64 age_adjacency_visible_payload_scan_vertex_set_block_value_posting_filtered(
    AgeAdjacencyVisiblePayloadScan *scan);
extern int64 age_adjacency_visible_payload_scan_vertex_set_block_posting_filtered(
    AgeAdjacencyVisiblePayloadScan *scan);
extern int64 age_adjacency_visible_payload_scan_vertex_set_block_compressed_filtered(
    AgeAdjacencyVisiblePayloadScan *scan);
extern int64 age_adjacency_visible_payload_scan_vertex_set_directory_filtered(
    AgeAdjacencyVisiblePayloadScan *scan);
extern int64 age_adjacency_visible_payload_scan_vertex_set_directory_range_filtered(
    AgeAdjacencyVisiblePayloadScan *scan);
extern int64 age_adjacency_visible_payload_scan_vertex_set_directory_exact_filtered(
    AgeAdjacencyVisiblePayloadScan *scan);
extern int64 age_adjacency_visible_payload_scan_vertex_set_directory_label_bloom_filtered(
    AgeAdjacencyVisiblePayloadScan *scan);
extern int64 age_adjacency_visible_payload_scan_vertex_set_directory_compressed_filtered(
    AgeAdjacencyVisiblePayloadScan *scan);
extern int64 age_adjacency_visible_payload_scan_vertex_set_directory_wide_bloom_filtered(
    AgeAdjacencyVisiblePayloadScan *scan);
extern int64 age_adjacency_visible_payload_scan_vertex_set_directory_value_filtered(
    AgeAdjacencyVisiblePayloadScan *scan);
extern int64 age_adjacency_visible_payload_scan_vertex_set_directory_value_posting_filtered(
    AgeAdjacencyVisiblePayloadScan *scan);
extern int64 age_adjacency_visible_payload_scan_composite_requests(
    AgeAdjacencyVisiblePayloadScan *scan);
extern int64 age_adjacency_visible_payload_scan_composite_block_filtered(
    AgeAdjacencyVisiblePayloadScan *scan);
extern int64 age_adjacency_visible_payload_scan_composite_directory_filtered(
    AgeAdjacencyVisiblePayloadScan *scan);
extern int64 age_adjacency_visible_payload_scan_composite_directory_estimated(
    AgeAdjacencyVisiblePayloadScan *scan);
extern void age_adjacency_visible_payload_scan_reset_runtime(
    AgeAdjacencyVisiblePayloadScan *scan);
extern int64 age_adjacency_visible_payload_scan_foreach(
    AgeAdjacencyVisiblePayloadScan *scan, graphid key,
    AgeAdjacencyPayloadCallback callback, void *callback_state);
extern void age_adjacency_end_visible_payload_scan(
    AgeAdjacencyVisiblePayloadScan *scan);

#endif
