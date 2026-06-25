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

#ifndef AG_AGE_GLOBAL_GRAPH_H
#define AG_AGE_GLOBAL_GRAPH_H

#include "utils/age_graphid_ds.h"
#include "utils/agtype.h"
#include "utils/hsearch.h"
#include "utils/relcache.h"
#include "storage/block.h"
#include "storage/itemptr.h"

#define AGE_VERTEX_PROPERTY_PREFETCH_MIN_REL_CANDIDATES 8

/*
 * Graph nodes and edges are declared here for internal consumers. vertex_entry
 * exposes cached-property fast fields so VLE can inline single-byte probes.
 */

/* vertex entry for the vertex_hashtable */
typedef struct vertex_entry vertex_entry;
typedef struct edge_label_adj_entry edge_label_adj_entry;

/* edge entry for the edge_hashtable */
typedef struct edge_entry edge_entry;

typedef struct GRAPH_global_context GRAPH_global_context;

typedef struct GraphEdgeAdjEntry
{
    graphid edge_id;
    graphid next_vertex_id;
    vertex_entry *next_vertex_entry;
    int64 edge_index;
} GraphEdgeAdjEntry;

typedef struct GraphEdgeAdjList
{
    GraphEdgeAdjEntry *items;
    int64 size;
    int64 capacity;
} GraphEdgeAdjList;

typedef struct GraphEdgeLabelSourceCandidate
{
    const char *label_name;
    Oid edge_label_oid;
    int32 label_id;
    Oid age_adjacency_out_index_oid;
    Oid age_adjacency_in_index_oid;
} GraphEdgeLabelSourceCandidate;

/* vertex entry for the vertex_hashtable */
struct vertex_entry
{
    graphid vertex_id;             /* vertex id, it is also the hash key */
    GraphEdgeAdjList *adj_edges_in;
    GraphEdgeAdjList *adj_edges_out;
    GraphEdgeAdjList *adj_edges_self;
    edge_label_adj_entry *edges_in_by_label;
    edge_label_adj_entry *edges_out_by_label;
    edge_label_adj_entry *edges_self_by_label;
    Oid vertex_label_table_oid;    /* the label table oid */
    char *vertex_label_name;       /* the label name */
    ItemPointerData tid;           /* physical tuple location for lazy fetch */
    Datum cached_properties;       /* graph-context copy of vertex properties */
    bool cached_properties_valid;
    char *cached_property_key;      /* graph-context copy of one property key */
    int cached_property_key_len;
    char cached_property_key_char;  /* inline key for one-byte property keys */
    Datum cached_property_value;    /* graph-context agtype result for key */
    bool cached_property_value_valid;
};

static inline bool vertex_entry_cached_property_char_fast(vertex_entry *ve,
                                                          char key,
                                                          Datum *result)
    __attribute__((always_inline));

static inline bool vertex_entry_cached_property_char_fast(vertex_entry *ve,
                                                          char key,
                                                          Datum *result)
{
    Assert(result != NULL);

    *result = (Datum) 0;
    if (ve->cached_property_value_valid &&
        ve->cached_property_key_len == 1 &&
        ve->cached_property_key_char == key)
    {
        *result = ve->cached_property_value;
        return true;
    }

    return false;
}

/* GRAPH global context functions */
GRAPH_global_context *manage_GRAPH_global_contexts(char *graph_name,
                                                   Oid graph_oid);
GRAPH_global_context *manage_GRAPH_global_contexts_len(const char *graph_name,
                                                       int graph_name_len,
                                                       Oid graph_oid);
GRAPH_global_context *manage_GRAPH_global_contexts_len_for_vle(
    const char *graph_name, int graph_name_len, Oid graph_oid,
    bool load_edge_property_metadata, Oid edge_label_oid,
    bool load_edge_metadata, bool load_vertex_metadata);
GRAPH_global_context *find_GRAPH_global_context(Oid graph_oid);
Oid get_GRAPH_global_context_oid(GRAPH_global_context *ggctx);
bool is_GRAPH_global_context_current(GRAPH_global_context *ggctx);
bool is_ggctx_invalid(GRAPH_global_context *ggctx);
/* GRAPH retrieval functions */
ListGraphId *get_graph_vertices(GRAPH_global_context *ggctx);
int64 get_graph_num_loaded_edges(GRAPH_global_context *ggctx);
bool graph_global_context_has_edge_metadata(GRAPH_global_context *ggctx);
bool graph_edge_labels_have_age_adjacency_indexes(
    Oid graph_oid, bool require_outgoing, bool require_incoming);
int32 get_graph_edge_label_id(GRAPH_global_context *ggctx,
                              Oid edge_label_oid);
int get_graph_edge_label_source_candidates(
    GRAPH_global_context *ggctx, GraphEdgeLabelSourceCandidate **candidates);
vertex_entry *get_vertex_entry(GRAPH_global_context *ggctx,
                               graphid vertex_id);
vertex_entry *ensure_vertex_entry_skeleton(GRAPH_global_context *ggctx,
                                           graphid vertex_id);
edge_entry *get_edge_entry(GRAPH_global_context *ggctx, graphid edge_id);
edge_entry *find_edge_entry(GRAPH_global_context *ggctx, graphid edge_id);
edge_entry *get_edge_entry_by_tid(GRAPH_global_context *ggctx,
                                  const ItemPointerData *tid);
bool get_edge_entry_vle_fields_by_tid(GRAPH_global_context *ggctx,
                                      const ItemPointerData *tid,
                                      graphid *edge_id,
                                      Oid *label_table_oid,
                                      graphid *start_vertex_id,
                                      graphid *end_vertex_id,
                                      int64 *edge_index,
                                      vertex_entry **start_vertex,
                                      vertex_entry **end_vertex,
                                      edge_entry **edge);
/* vertex entry accessor functions*/
graphid get_vertex_entry_id(vertex_entry *ve);
GraphEdgeAdjList *get_vertex_entry_adj_edges_in(vertex_entry *ve);
GraphEdgeAdjList *get_vertex_entry_adj_edges_out(vertex_entry *ve);
GraphEdgeAdjList *get_vertex_entry_adj_edges_self(vertex_entry *ve);
GraphEdgeAdjList *get_vertex_entry_adj_edges_in_for_label(
    vertex_entry *ve, Oid edge_label_table_oid);
GraphEdgeAdjList *get_vertex_entry_adj_edges_out_for_label(
    vertex_entry *ve, Oid edge_label_table_oid);
GraphEdgeAdjList *get_vertex_entry_adj_edges_self_for_label(
    vertex_entry *ve, Oid edge_label_table_oid);
Oid get_vertex_entry_label_table_oid(vertex_entry *ve);
char *get_vertex_entry_label_name(vertex_entry *ve);
Datum get_vertex_entry_properties(vertex_entry *ve);
Datum get_vertex_entry_properties_with_cache(vertex_entry *ve,
                                             HTAB *relation_cache);
int64 prefetch_vertex_entry_properties_by_ids(
    GRAPH_global_context *ggctx, const graphid *vertex_ids,
    int64 nvertex_ids, HTAB **relation_cache, const char *cache_name);
bool get_vertex_entry_cached_properties(vertex_entry *ve, Datum *properties);
Datum get_vertex_entry_properties_with_relation(vertex_entry *ve,
                                                Relation rel);
bool get_vertex_entry_cached_property(vertex_entry *ve, agtype_value *key,
                                      Datum *result);
bool get_vertex_entry_cached_property_str(vertex_entry *ve,
                                          const char *key, int key_len,
                                          Datum *result);
bool get_vertex_entry_cached_property_char(vertex_entry *ve, char key,
                                           Datum *result);
bool get_vertex_entry_property_with_cache(vertex_entry *ve,
                                          HTAB *relation_cache,
                                          agtype_value *key, Datum *result);
bool get_vertex_entry_property_with_lazy_cache(vertex_entry *ve,
                                               HTAB **relation_cache,
                                               const char *cache_name,
                                               agtype_value *key,
                                               Datum *result);
bool get_vertex_entry_scalar_property_with_lazy_cache(vertex_entry *ve,
                                                      HTAB **relation_cache,
                                                      const char *cache_name,
                                                      agtype_value *key,
                                                      Datum *result);
BlockNumber get_vertex_entry_tid_block(vertex_entry *ve);
int64 prefetch_vertex_entry_block_scalar_property_cache(
    GRAPH_global_context *ggctx, vertex_entry *ve, HTAB **relation_cache,
    const char *cache_name, agtype_value *key, int64 max_cached,
    Datum *target_result, bool *target_found);
bool cache_vertex_entry_tuple_scalar_property(
    GRAPH_global_context *ggctx, Oid relid, HeapTuple tuple,
    TupleDesc tupdesc, agtype_value *key);
bool get_vertex_entry_property_with_relation(vertex_entry *ve, Relation rel,
                                             agtype_value *key, Datum *result);
/* edge entry accessor functions */
graphid get_edge_entry_id(edge_entry *ee);
Oid get_edge_entry_label_table_oid(edge_entry *ee);
char *get_edge_entry_label_name(edge_entry *ee);
Datum get_edge_entry_properties(edge_entry *ee);
Datum get_edge_entry_properties_with_cache(edge_entry *ee,
                                           HTAB *relation_cache);
bool get_edge_entry_cached_property(edge_entry *ee, agtype_value *key,
                                    Datum *result);
bool get_edge_entry_property_with_cache(edge_entry *ee,
                                        HTAB *relation_cache,
                                        agtype_value *key, Datum *result);
bool get_edge_entry_property_with_relation(edge_entry *ee, Relation rel,
                                           agtype_value *key, Datum *result);
graphid get_edge_entry_start_vertex_id(edge_entry *ee);
graphid get_edge_entry_end_vertex_id(edge_entry *ee);
int64 get_edge_entry_index(edge_entry *ee);
void get_edge_entry_vle_fields(edge_entry *ee, graphid *edge_id,
                               Oid *label_table_oid,
                               graphid *start_vertex_id,
                               graphid *end_vertex_id, int64 *edge_index);
int get_edge_entry_property_count(edge_entry *ee);
int get_edge_entry_property_size(edge_entry *ee);
uint32 get_edge_entry_property_hash(edge_entry *ee);
HTAB *create_entry_property_relation_cache(const char *name);
void destroy_entry_property_relation_cache(HTAB *relation_cache);

/* Graph version counter functions — shared memory (DSM or shmem) */
uint64 get_graph_version(Oid graph_oid);
void increment_graph_version(Oid graph_oid);
void remove_graph_version(Oid graph_oid);
Oid get_graph_oid_for_table(Oid table_oid);
double age_cached_edge_dst_label_selectivity(Oid graph_oid,
                                             const char *src_label_name,
                                             const char *edge_label_name,
                                             const char *dst_label_name);
double age_cached_edge_avg_out_degree(Oid graph_oid,
                                      const char *src_label_name,
                                      const char *edge_label_name);
double age_cached_edge_avg_in_degree(Oid graph_oid,
                                     const char *dst_label_name,
                                     const char *edge_label_name);

/* Shared memory initialization for PG < 17 (shmem_request_hook path) */
#if PG_VERSION_NUM < 170000
void age_graph_version_shmem_request(void);
void age_graph_version_shmem_startup(void);
#endif

#endif
