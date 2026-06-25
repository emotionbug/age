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

#ifndef AG_CYPHER_ADJACENCY_MATCH_TERMINAL_H
#define AG_CYPHER_ADJACENCY_MATCH_TERMINAL_H

#include "postgres.h"

#include "access/age_adjacency.h"
#include "utils/graphid.h"

typedef struct AgeAdjacencyMatchTerminalPropertyLookup
    AgeAdjacencyMatchTerminalPropertyLookup;

typedef enum AgeAdjacencyMatchTerminalPropertyMode
{
    AGE_ADJACENCY_TERMINAL_PROPERTY_NONE = 0,
    AGE_ADJACENCY_TERMINAL_PROPERTY_SOURCE_PREFETCH,
    AGE_ADJACENCY_TERMINAL_PROPERTY_DEFERRED_PREFETCH,
    AGE_ADJACENCY_TERMINAL_PROPERTY_ID_CACHE,
    AGE_ADJACENCY_TERMINAL_PROPERTY_ID_BTREE
} AgeAdjacencyMatchTerminalPropertyMode;

typedef struct AgeAdjacencyMatchTerminalPropertyRequest
{
    Oid graph_oid;
    int32 right_label_id;
    bool has_property_predicate;
    bool metadata_backed;
    Oid property_index_oid;
    const char *property_key;
    Datum property_value;
    bool property_value_isnull;
} AgeAdjacencyMatchTerminalPropertyRequest;

extern AgeAdjacencyMatchTerminalPropertyLookup *
age_adjacency_match_terminal_property_begin(
    const AgeAdjacencyMatchTerminalPropertyRequest *request,
    MemoryContext context);
extern void age_adjacency_match_terminal_property_end(
    AgeAdjacencyMatchTerminalPropertyLookup *lookup);
extern void age_adjacency_match_terminal_property_rescan(
    AgeAdjacencyMatchTerminalPropertyLookup *lookup);
extern void age_adjacency_match_terminal_property_set_value(
    AgeAdjacencyMatchTerminalPropertyLookup *lookup, Datum property_value,
    bool property_value_isnull);
extern bool age_adjacency_match_terminal_property_prepare_prefilter(
    AgeAdjacencyMatchTerminalPropertyLookup *lookup, int64 run_count,
    int64 candidate_count,
    int64 prefetch_threshold);
extern bool age_adjacency_match_terminal_property_active(
    const AgeAdjacencyMatchTerminalPropertyLookup *lookup);
extern bool age_adjacency_match_terminal_property_matches(
    AgeAdjacencyMatchTerminalPropertyLookup *lookup, graphid vertex_id);
extern bool age_adjacency_match_terminal_property_prefilter_active(
    const AgeAdjacencyMatchTerminalPropertyLookup *lookup);
extern bool age_adjacency_match_terminal_property_prefilter_matches(
    graphid vertex_id, void *callback_state);
extern bool age_adjacency_match_terminal_property_prefilter_set(
    const AgeAdjacencyMatchTerminalPropertyLookup *lookup,
    AgeAdjacencyVertexSetFilter *filter);
extern AgeAdjacencyMatchTerminalPropertyMode
age_adjacency_match_terminal_property_mode_id(
    const AgeAdjacencyMatchTerminalPropertyLookup *lookup);
extern const char *age_adjacency_match_terminal_property_mode_name(
    AgeAdjacencyMatchTerminalPropertyMode mode);
extern Oid age_adjacency_match_terminal_property_index_oid(
    const AgeAdjacencyMatchTerminalPropertyLookup *lookup);
extern uint32 age_adjacency_match_terminal_property_filter_id(
    const AgeAdjacencyMatchTerminalPropertyLookup *lookup);
extern int64 age_adjacency_match_terminal_property_prefetched_matches(
    const AgeAdjacencyMatchTerminalPropertyLookup *lookup);
extern int64 age_adjacency_match_terminal_property_cache_hits(
    const AgeAdjacencyMatchTerminalPropertyLookup *lookup);
extern int64 age_adjacency_match_terminal_property_index_lookups(
    const AgeAdjacencyMatchTerminalPropertyLookup *lookup);
extern int64 age_adjacency_match_terminal_property_prefetch_candidate_count(
    const AgeAdjacencyMatchTerminalPropertyLookup *lookup);
extern int64 age_adjacency_match_terminal_property_prefetch_run_count(
    const AgeAdjacencyMatchTerminalPropertyLookup *lookup);
extern int64 age_adjacency_match_terminal_property_prefetch_skipped_small(
    const AgeAdjacencyMatchTerminalPropertyLookup *lookup);
extern int64 age_adjacency_match_terminal_property_prefetch_threshold(
    const AgeAdjacencyMatchTerminalPropertyLookup *lookup);

#endif
