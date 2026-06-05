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
extern int64 age_adjacency_visible_payload_scan_run_postings(
    AgeAdjacencyVisiblePayloadScan *scan);
extern int64 age_adjacency_visible_payload_scan_active_postings(
    AgeAdjacencyVisiblePayloadScan *scan);
extern bool age_adjacency_estimate_terminal_label_postings(
    Oid index_oid, graphid key, int32 terminal_label_id,
    int64 *run_postings, int64 *terminal_postings, int64 *label_groups,
    const char **value_posting_source);
extern bool age_adjacency_visible_payload_scan_key_known_empty(
    AgeAdjacencyVisiblePayloadScan *scan, graphid key);
extern bool age_adjacency_visible_payload_scan_next(
    AgeAdjacencyVisiblePayloadScan *scan, AgeAdjacencyPayload *payload);
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
