/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/relation.h"
#include "catalog/namespace.h"
#include "commands/defrem.h"
#include "commands/trigger.h"
#include "common/hashfn.h"
#include "commands/label_commands.h"
#include "executor/executor.h"
#include "port/atomics.h"
#include "storage/bufmgr.h"
#include "storage/bufpage.h"
#include "storage/itemid.h"
#include "storage/lwlock.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"
#include "utils/builtins.h"

#if PG_VERSION_NUM >= 170000
#include "storage/dsm_registry.h"
#else
#include "storage/ipc.h"
#include "storage/shmem.h"
#endif

#include "utils/age_global_graph.h"
#include "catalog/ag_graph.h"
#include "catalog/ag_label.h"
#include "catalog/pg_am.h"
#include "utils/ag_cache.h"
#include "utils/agtype.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "access/age_adjacency.h"
#include "funcapi.h"
#include "miscadmin.h"

#include <pthread.h>

/* defines */
#define VERTEX_HTAB_NAME "Vertex to edge lists " /* added a space at end for */
#define EDGE_HTAB_NAME "Edge to vertex mapping " /* the graph name to follow */
#define VERTEX_HTAB_INITIAL_SIZE 1024
#define EDGE_HTAB_INITIAL_SIZE 1024

/* Maximum number of graphs tracked for version counting */
#define AGE_MAX_GRAPHS 128

/*
 * Graph version counter entry. Stored in shared memory (DSM or shmem)
 * so that all backends can see mutation events. The version counter is
 * incremented by Cypher mutations (CREATE/DELETE/SET/MERGE) and by
 * SQL triggers on label tables. VLE cache invalidation checks this
 * counter instead of snapshot xmin/xmax/curcid.
 */
typedef struct GraphVersionEntry
{
    Oid graph_oid;                 /* graph identifier (0 = unused slot) */
    pg_atomic_uint64 version;      /* monotonic change counter */
} GraphVersionEntry;

/*
 * Shared memory state for graph version tracking.
 * Contains a fixed-size array of per-graph version counters.
 */
typedef struct GraphVersionState
{
    LWLock lock;                   /* protects slot allocation only */
    int num_entries;               /* number of active entries */
    GraphVersionEntry entries[AGE_MAX_GRAPHS];
} GraphVersionState;

typedef struct EntryPropertyRelationCacheEntry
{
    Oid relid;
    Relation rel;
} EntryPropertyRelationCacheEntry;

typedef struct VertexPropertyPrefetchEntry
{
    graphid vertex_id;
    vertex_entry *ve;
} VertexPropertyPrefetchEntry;

/*
 * Version mode detection — determined once per backend on first use.
 * DSM:      PG 17+ GetNamedDSMSegment (no shared_preload_libraries needed)
 * SHMEM:    PG < 17 with shared_preload_libraries
 * SNAPSHOT: PG < 17 without shared_preload_libraries (current behavior)
 */
typedef enum
{
    VERSION_MODE_UNKNOWN = 0,
    VERSION_MODE_DSM,
    VERSION_MODE_SHMEM,
    VERSION_MODE_SNAPSHOT
} VersionMode;

static VersionMode version_mode = VERSION_MODE_UNKNOWN;
static Oid cached_version_graph_oid = InvalidOid;
static int cached_version_graph_index = -1;

/* For PG < 17 shmem path */
static GraphVersionState *shmem_version_state = NULL;

/* internal data structures implementation */

typedef struct edge_label_adj_entry
{
    Oid edge_label_table_oid;
    GraphEdgeAdjList *adj_edges;
    struct edge_label_adj_entry *next;
} edge_label_adj_entry;

/* edge entry for the edge_hashtable */
typedef struct edge_entry
{
    graphid edge_id;               /* edge id, it is also the hash key */
    Oid edge_label_table_oid;      /* the label table oid */
    char *edge_label_name;         /* the label name */
    ItemPointerData tid;           /* physical tuple location for lazy fetch */
    graphid start_vertex_id;       /* start vertex */
    graphid end_vertex_id;         /* end vertex */
    vertex_entry *start_vertex;     /* linked start vertex entry, if valid */
    vertex_entry *end_vertex;       /* linked end vertex entry, if valid */
    int64 edge_index;              /* dense index for traversal-local state */
    int edge_property_count;        /* top-level property pair count */
    int edge_property_size;         /* varlena size for exact-object checks */
    uint32 edge_property_hash;      /* datum image hash for exact-object checks */
    char *cached_property_key;      /* graph-context copy of one property key */
    int cached_property_key_len;
    char cached_property_key_char;  /* inline key for one-byte property keys */
    Datum cached_property_value;    /* graph-context agtype result for key */
    bool cached_property_value_valid;
} edge_entry;

typedef struct graph_label_entry
{
    NameData name;
    Oid relation;
    int32 label_id;
} graph_label_entry;

typedef struct graph_label_list
{
    graph_label_entry *entries;
    int count;
    int capacity;
} graph_label_list;

typedef struct GraphIdHashEntry
{
    graphid id;
} GraphIdHashEntry;

typedef struct GraphLabelIdHashEntry
{
    int32 label_id;
} GraphLabelIdHashEntry;

typedef struct EdgeTidHashEntry
{
    ItemPointerData tid;
    edge_entry *edge;
} EdgeTidHashEntry;

/*
 * GRAPH global context per graph. They are chained together via next.
 * Be aware that the global pointer will point to the root BUT that
 * the root will change as new graphs are added to the top.
 */
typedef struct GRAPH_global_context
{
    MemoryContext memory_context;     /* owned allocations for this graph */
    char *graph_name;              /* graph name */
    Oid graph_oid;                 /* graph oid for searching */
    HTAB *vertex_hashtable;        /* hashtable to hold vertex edge lists */
    HTAB *edge_hashtable;          /* hashtable to hold edge to vertex map */
    HTAB *edge_tid_hashtable;      /* hashtable to map heap TID to edge */
    uint64 graph_version;          /* version counter for cache invalidation */
    TransactionId xmin;            /* snapshot fallback: transaction xmin */
    TransactionId xmax;            /* snapshot fallback: transaction xmax */
    CommandId curcid;              /* snapshot fallback: command id */
    int64 num_loaded_vertices;     /* number of loaded vertices in this graph */
    int64 num_loaded_edges;        /* number of loaded edges in this graph */
    graph_label_list vertex_labels; /* loaded vertex labels for shared names */
    graph_label_list edge_labels;   /* loaded edge labels for shared names */
    bool edge_property_metadata_loaded; /* edge property count/hash available */
    bool edge_metadata_loaded;      /* edge hash/TID metadata available */
    bool vertex_metadata_loaded;    /* vertex hash/TID metadata available */
    Oid loaded_edge_label_oid;      /* InvalidOid means all edge labels */
    bool adjacency_index_candidate_source; /* VLE uses index payloads */
    ListGraphId *vertices;         /* vertices for vertex hashtable cleanup */
    struct GRAPH_global_context *next; /* next graph */
} GRAPH_global_context;

/* container for GRAPH_global_context and its mutex */
typedef struct GRAPH_global_context_container
{
    /* head of the list */
    GRAPH_global_context *contexts;

    /* mutex to protect the list */
    pthread_mutex_t mutex_lock;
} GRAPH_global_context_container;

/* global variable to hold the per process GRAPH global contexts */
static GRAPH_global_context_container global_graph_contexts_container = {0};

/* declarations */
/* GRAPH global context functions */
static bool free_specific_GRAPH_global_context(GRAPH_global_context *ggctx);
static GRAPH_global_context *manage_GRAPH_global_contexts_internal(
    const char *graph_name, int graph_name_len, Oid graph_oid,
    bool load_edge_property_metadata, Oid edge_label_oid,
    bool load_edge_metadata, bool load_vertex_metadata);
static bool delete_specific_GRAPH_global_context_by_oid(Oid graph_oid);
static bool delete_specific_GRAPH_global_contexts_len(const char *graph_name,
                                                      int graph_name_len);
static bool delete_GRAPH_global_contexts(void);
static Oid get_cached_global_graph_oid_len(const char *graph_name,
                                           int graph_name_len);
static void get_global_graph_scalar_arg_no_copy(char *funcname,
                                                agtype *agt_arg,
                                                enum agtype_value_type type,
                                                bool error,
                                                agtype_value *result,
                                                bool *needs_free);
static void create_GRAPH_global_hashtables(GRAPH_global_context *ggctx);
static void load_GRAPH_global_hashtables(GRAPH_global_context *ggctx,
                                         bool load_edge_property_metadata,
                                         Oid edge_label_oid,
                                         bool load_edge_metadata,
                                         bool load_vertex_metadata);
static void load_vertex_hashtable(GRAPH_global_context *ggctx,
                                  Snapshot snapshot,
                                  graph_label_list *vertex_labels,
                                  HTAB *vertex_filter,
                                  HTAB *vertex_label_filter);
static int load_vertex_tuples_by_index_range(GRAPH_global_context *ggctx,
                                             Relation graph_vertex_label,
                                             Snapshot snapshot, Oid index_oid,
                                             TupleDesc tupdesc,
                                             Oid vertex_label_table_oid,
                                             const char *vertex_label_name,
                                             HTAB *vertex_filter,
                                             graphid min_vertex_id,
                                             graphid max_vertex_id,
                                             int vertices_needed);
static void load_edge_hashtable(GRAPH_global_context *ggctx,
                                Snapshot snapshot,
                                graph_label_list *edge_labels,
                                bool load_edge_property_metadata,
                                Oid edge_label_oid,
                                HTAB *edge_endpoint_ids);
static void get_edge_label_age_adjacency_indexes(Oid edge_label_oid,
                                                 Oid *out_index_oid,
                                                 Oid *in_index_oid);
static bool edge_label_has_age_adjacency_indexes(Oid edge_label_oid);
static bool age_adjacency_index_matches(Relation index_rel, bool outgoing);
static void link_edge_hashtable_to_vertices(GRAPH_global_context *ggctx);
static void freeze_GRAPH_global_hashtables(GRAPH_global_context *ggctx);
static bool graph_context_satisfies_edge_load(GRAPH_global_context *ggctx,
                                              bool load_edge_property_metadata,
                                              Oid edge_label_oid,
                                              bool load_edge_metadata);
static void get_ag_labels_names(Snapshot snapshot, Oid graph_oid,
                                graph_label_list *vertex_labels,
                                graph_label_list *edge_labels);
static void append_graph_label(graph_label_list *labels, Name label_name,
                               Oid relation, int32 label_id);
static bool append_cached_graph_label(graph_label_list *labels,
                                      label_cache_data *label_cache,
                                      Oid graph_oid, char label_kind);
static void append_endpoint_vertex_labels(GRAPH_global_context *ggctx,
                                          HTAB *vertex_label_filter);
static bool load_vertex_tuple(GRAPH_global_context *ggctx, TupleDesc tupdesc,
                              HeapTuple tuple, Oid vertex_label_table_oid,
                              const char *vertex_label_name,
                              HTAB *vertex_filter,
                              bool use_vertex_range,
                              graphid min_vertex_id,
                              graphid max_vertex_id);
static void load_endpoint_vertex_skeletons(GRAPH_global_context *ggctx,
                                           HTAB *edge_endpoint_ids);
static void free_ag_label_names(graph_label_list *labels);
static HTAB *create_graphid_hash(const char *name);
static void add_graphid_hash(HTAB *hash, graphid id);
static bool graphid_hash_contains(HTAB *hash, graphid id);
static HTAB *create_label_id_hash(const char *name);
static void add_label_id_hash(HTAB *hash, int32 label_id);
static bool label_id_hash_contains(HTAB *hash, int32 label_id);
static bool insert_edge_entry(GRAPH_global_context *ggctx, graphid edge_id,
                              ItemPointerData tid, graphid start_vertex_id,
                              graphid end_vertex_id, Oid edge_label_table_oid,
                              const char *edge_label_name,
                              int edge_property_count,
                              int edge_property_size,
                              uint32 edge_property_hash,
                              int64 *edge_index, edge_entry **edge);
static bool insert_vertex_edge(GRAPH_global_context *ggctx,
                               edge_entry *edge,
                               graphid start_vertex_id, graphid end_vertex_id,
                               graphid edge_id, int64 edge_index,
                               Oid edge_label_table_oid,
                               const char *edge_label_name);
static void append_labeled_vertex_edge(edge_label_adj_entry **head,
                                       Oid edge_label_table_oid,
                                       graphid edge_id,
                                       graphid next_vertex_id,
                                       vertex_entry *next_vertex_entry,
                                       int64 edge_index);
static GraphEdgeAdjList *get_labeled_vertex_adj_edges(
    edge_label_adj_entry *head, Oid edge_label_table_oid);
static void free_labeled_vertex_edges(edge_label_adj_entry *head);
static GraphEdgeAdjList *append_graph_edge_adj(GraphEdgeAdjList *container,
                                               graphid edge_id,
                                               graphid next_vertex_id,
                                               vertex_entry *next_vertex_entry,
                                               int64 edge_index);
static GraphEdgeAdjList *compact_graph_edge_adj(GraphEdgeAdjList *container);
static void compact_labeled_vertex_edges(edge_label_adj_entry *head);
static void free_graph_edge_adj(GraphEdgeAdjList *container);
static bool insert_vertex_entry(GRAPH_global_context *ggctx, graphid vertex_id,
                                Oid vertex_label_table_oid,
                                const char *vertex_label_name,
                                ItemPointerData tid);
static Datum fetch_entry_properties(Oid relid, ItemPointerData tid,
                                    AttrNumber property_attr,
                                    HTAB *relation_cache,
                                    const char *stale_msg);
static Datum fetch_entry_properties_from_relation(Relation rel,
                                                  ItemPointerData tid,
                                                  AttrNumber property_attr,
                                                  const char *stale_msg);
static bool get_vertex_entry_property_with_relation_uncached(vertex_entry *ve,
                                                             Relation rel,
                                                             agtype_value *key,
                                                             Datum *result);
static bool get_edge_entry_property_with_relation_uncached(edge_entry *ee,
                                                           Relation rel,
                                                           agtype_value *key,
                                                           Datum *result);
static Datum fetch_vertex_entry_properties_by_id(vertex_entry *ve,
                                                 HTAB *relation_cache);
static bool get_vertex_entry_scalar_property_with_relation(vertex_entry *ve,
                                                           Relation rel,
                                                           agtype_value *key,
                                                           Datum *result);
static void cache_vertex_entry_scalar_property(vertex_entry *ve,
                                               agtype_value *key,
                                               agtype_value *property_value);
static void cache_edge_entry_scalar_property(edge_entry *ee,
                                             agtype_value *key,
                                             agtype_value *property_value);
static int64 prefetch_vertex_block_scalar_property_cache(
    GRAPH_global_context *ggctx, Relation rel, BlockNumber blockno,
    graphid target_vertex_id, agtype_value *key, int64 max_cached,
    Datum *target_result, bool *target_found);
static Relation get_entry_property_relation(Oid relid, HTAB *relation_cache);
static bool is_ggctx_invalid_with_snapshot(GRAPH_global_context *ggctx,
                                           Snapshot *snapshot);
/* definitions */

/*
 * Helper function to determine validity of the passed GRAPH_global_context.
 *
 * Uses graph-specific version counters (via DSM or shmem) when available.
 * Falls back to snapshot-based invalidation when shared memory is not
 * initialized (PG < 17 without shared_preload_libraries).
 *
 * The version counter approach only invalidates when the specific graph
 * has been mutated (via Cypher operations or SQL triggers), avoiding false
 * invalidation from unrelated transactions on the server.
 */
bool is_ggctx_invalid(GRAPH_global_context *ggctx)
{
    Snapshot snapshot = NULL;

    return is_ggctx_invalid_with_snapshot(ggctx, &snapshot);
}

static bool is_ggctx_invalid_with_snapshot(GRAPH_global_context *ggctx,
                                           Snapshot *snapshot)
{
    /* use version counter if DSM or SHMEM mode is active */
    if (version_mode == VERSION_MODE_DSM || version_mode == VERSION_MODE_SHMEM)
    {
        uint64 current_version = get_graph_version(ggctx->graph_oid);

        /*
         * If current_version is 0, no mutations have been tracked through
         * the version counter system yet. Fall through to snapshot-based
         * checking for safety — the graph may have been mutated via paths
         * that don't increment the counter (e.g., before executor hooks
         * are in place, or via direct SQL without triggers).
         *
         * Once current_version > 0, we know the counter is actively
         * tracking this graph and can rely on it exclusively.
         */
        if (current_version > 0)
        {
            return (ggctx->graph_version != current_version);
        }
        /* fall through to snapshot check */
    }

    /* SNAPSHOT fallback: original behavior — check snapshot ids */
    if (*snapshot == NULL)
    {
        *snapshot = GetActiveSnapshot();
    }

    return (ggctx->xmin != (*snapshot)->xmin ||
            ggctx->xmax != (*snapshot)->xmax ||
            ggctx->curcid != (*snapshot)->curcid);
}
/*
 * Helper function to create the global vertex and edge hashtables. One
 * hashtable will hold the vertex, its edges (both incoming and exiting) as a
 * list, and its properties datum. The other hashtable will hold the edge, its
 * properties datum, and its source and target vertex.
 */
static void create_GRAPH_global_hashtables(GRAPH_global_context *ggctx)
{
    HASHCTL vertex_ctl;
    HASHCTL edge_ctl;
    HASHCTL edge_tid_ctl;
    char *graph_name = NULL;
    char *vhn = NULL;
    char *ehn = NULL;
    char *ethn = NULL;

    /* get the graph name */
    graph_name = ggctx->graph_name;
    vhn = psprintf("%s%s", VERTEX_HTAB_NAME, graph_name);
    ehn = psprintf("%s%s", EDGE_HTAB_NAME, graph_name);
    ethn = psprintf("GRAPH edge TID hash %s", graph_name);

    /* initialize the vertex hashtable */
    MemSet(&vertex_ctl, 0, sizeof(vertex_ctl));
    vertex_ctl.keysize = sizeof(int64);
    vertex_ctl.entrysize = sizeof(vertex_entry);
    vertex_ctl.hash = tag_hash;
    ggctx->vertex_hashtable = hash_create(vhn, VERTEX_HTAB_INITIAL_SIZE,
                                          &vertex_ctl,
                                          HASH_ELEM | HASH_FUNCTION);
    pfree_if_not_null(vhn);

    /* initialize the edge hashtable */
    MemSet(&edge_ctl, 0, sizeof(edge_ctl));
    edge_ctl.keysize = sizeof(int64);
    edge_ctl.entrysize = sizeof(edge_entry);
    edge_ctl.hash = tag_hash;
    ggctx->edge_hashtable = hash_create(ehn, EDGE_HTAB_INITIAL_SIZE, &edge_ctl,
                                        HASH_ELEM | HASH_FUNCTION);
    pfree_if_not_null(ehn);

    MemSet(&edge_tid_ctl, 0, sizeof(edge_tid_ctl));
    edge_tid_ctl.keysize = sizeof(ItemPointerData);
    edge_tid_ctl.entrysize = sizeof(EdgeTidHashEntry);
    edge_tid_ctl.hash = tag_hash;
    ggctx->edge_tid_hashtable = hash_create(ethn, EDGE_HTAB_INITIAL_SIZE,
                                            &edge_tid_ctl,
                                            HASH_ELEM | HASH_FUNCTION);
    pfree_if_not_null(ethn);
}

/* helper function to get label names for the specified graph */
static void get_ag_labels_names(Snapshot snapshot, Oid graph_oid,
                                graph_label_list *vertex_labels,
                                graph_label_list *edge_labels)
{
    ScanKeyData scan_key;
    Relation ag_label;
    SysScanDesc scan_desc;
    HeapTuple tuple;
    TupleDesc tupdesc;

    /* we need a valid snapshot */
    Assert(snapshot != NULL);

    /* setup the table to be scanned, ag_label in this case */
    ag_label = table_open(ag_label_relation_id(), AccessShareLock);

    /* get the tupdesc - we don't need to release this one */
    tupdesc = RelationGetDescr(ag_label);
    /* bail if the number of columns differs - this table has 5 */
    Assert(tupdesc->natts == Natts_ag_label);

    ScanKeyInit(&scan_key, Anum_ag_label_graph, BTEqualStrategyNumber,
                F_OIDEQ, ObjectIdGetDatum(graph_oid));

    scan_desc = systable_beginscan(ag_label, ag_label_graph_oid_index_id(),
                                   true, snapshot, 1, &scan_key);

    while (HeapTupleIsValid(tuple = systable_getnext(scan_desc)))
    {
        bool is_null;
        Datum datum;
        char label_type;
        Name label_name;
        Oid label_relation;
        int32 label_id;

        datum = heap_getattr(tuple, Anum_ag_label_kind, tupdesc, &is_null);
        if (is_null)
        {
            continue;
        }

        label_type = DatumGetChar(datum);
        if (label_type == LABEL_TYPE_VERTEX || label_type == LABEL_TYPE_EDGE)
        {
            datum = heap_getattr(tuple, Anum_ag_label_name, tupdesc, &is_null);
            if (is_null)
            {
                continue;
            }

            label_name = DatumGetName(datum);

            datum = heap_getattr(tuple, Anum_ag_label_relation, tupdesc,
                                 &is_null);
            Assert(!is_null);
            label_relation = DatumGetObjectId(datum);

            datum = heap_getattr(tuple, Anum_ag_label_id, tupdesc, &is_null);
            Assert(!is_null);
            label_id = DatumGetInt32(datum);

            if (label_type == LABEL_TYPE_VERTEX)
            {
                append_graph_label(vertex_labels, label_name, label_relation,
                                   label_id);
            }
            else
            {
                append_graph_label(edge_labels, label_name, label_relation,
                                   label_id);
            }
        }
    }

    systable_endscan(scan_desc);
    table_close(ag_label, AccessShareLock);
}

static void append_graph_label(graph_label_list *labels, Name label_name,
                               Oid relation, int32 label_id)
{
    graph_label_entry *label;

    if (labels->count >= labels->capacity)
    {
        int new_capacity = labels->capacity == 0 ? 8 : labels->capacity * 2;

        if (labels->entries == NULL)
        {
            labels->entries = palloc(sizeof(*labels->entries) * new_capacity);
        }
        else
        {
            labels->entries = repalloc(labels->entries,
                                       sizeof(*labels->entries) *
                                       new_capacity);
        }

        labels->capacity = new_capacity;
    }

    label = &labels->entries[labels->count++];
    namestrcpy(&label->name, NameStr(*label_name));
    label->relation = relation;
    label->label_id = label_id;
}

static bool append_cached_graph_label(graph_label_list *labels,
                                      label_cache_data *label_cache,
                                      Oid graph_oid, char label_kind)
{
    if (label_cache == NULL ||
        label_cache->graph != graph_oid ||
        label_cache->kind != label_kind)
    {
        return false;
    }

    append_graph_label(labels, &label_cache->name, label_cache->relation,
                       label_cache->id);
    return true;
}

static void append_endpoint_vertex_labels(GRAPH_global_context *ggctx,
                                          HTAB *vertex_label_filter)
{
    HASH_SEQ_STATUS status;
    GraphLabelIdHashEntry *entry;

    hash_seq_init(&status, vertex_label_filter);
    while ((entry = hash_seq_search(&status)) != NULL)
    {
        label_cache_data *label_cache;

        label_cache = search_label_graph_oid_cache_cached(ggctx->graph_oid,
                                                          entry->label_id);
        (void)append_cached_graph_label(&ggctx->vertex_labels, label_cache,
                                        ggctx->graph_oid, LABEL_KIND_VERTEX);
    }
}

static void free_ag_label_names(graph_label_list *labels)
{
    pfree_if_not_null(labels->entries);
    labels->entries = NULL;
    labels->count = 0;
    labels->capacity = 0;
}

static HTAB *create_graphid_hash(const char *name)
{
    HASHCTL ctl;

    MemSet(&ctl, 0, sizeof(ctl));
    ctl.keysize = sizeof(graphid);
    ctl.entrysize = sizeof(GraphIdHashEntry);
    ctl.hash = tag_hash;

    return hash_create(name, 1024, &ctl, HASH_ELEM | HASH_FUNCTION);
}

static void add_graphid_hash(HTAB *hash, graphid id)
{
    bool found;

    (void)hash_search(hash, &id, HASH_ENTER, &found);
}

static bool graphid_hash_contains(HTAB *hash, graphid id)
{
    bool found;

    (void)hash_search(hash, &id, HASH_FIND, &found);
    return found;
}

static HTAB *create_label_id_hash(const char *name)
{
    HASHCTL ctl;

    MemSet(&ctl, 0, sizeof(ctl));
    ctl.keysize = sizeof(int32);
    ctl.entrysize = sizeof(GraphLabelIdHashEntry);
    ctl.hash = tag_hash;

    return hash_create(name, 16, &ctl, HASH_ELEM | HASH_FUNCTION);
}

static void add_label_id_hash(HTAB *hash, int32 label_id)
{
    bool found;

    (void)hash_search(hash, &label_id, HASH_ENTER, &found);
}

static bool label_id_hash_contains(HTAB *hash, int32 label_id)
{
    bool found;

    (void)hash_search(hash, &label_id, HASH_FIND, &found);
    return found;
}

/*
 * Helper function to insert one edge/edge->vertex, key/value pair, in the
 * current GRAPH global edge hashtable.
 */
static bool insert_edge_entry(GRAPH_global_context *ggctx, graphid edge_id,
                              ItemPointerData tid, graphid start_vertex_id,
                              graphid end_vertex_id, Oid edge_label_table_oid,
                              const char *edge_label_name,
                              int edge_property_count,
                              int edge_property_size,
                              uint32 edge_property_hash,
                              int64 *edge_index, edge_entry **edge)
{
    edge_entry *ee = NULL;
    bool found = false;
    int64 new_edge_index = ggctx->num_loaded_edges;

    /* search for the edge */
    ee = (edge_entry *)hash_search(ggctx->edge_hashtable, (void *)&edge_id,
                                      HASH_ENTER, &found);

    /* if the hash enter returned is NULL, error out */
    if (ee == NULL)
    {
        elog(ERROR, "insert_edge_entry: hash table returned NULL for ee");
    }

    /*
     * If we found the key, either we have a duplicate, or we made a mistake and
     * inserted it already. Either way, this isn't good so don't insert it and
     * return false.
     */
    if (found)
    {
        ereport(WARNING,
                (errcode(ERRCODE_DATA_EXCEPTION),
                 errmsg("edge: [id: " INT64_FORMAT ", start: " INT64_FORMAT ", end: " INT64_FORMAT ", label oid: %d] %s",
                        edge_id, start_vertex_id, end_vertex_id,
                        edge_label_table_oid, "duplicate edge found")));

        ereport(WARNING,
                (errcode(ERRCODE_DATA_EXCEPTION),
                 errmsg("previous edge: [id: " INT64_FORMAT ", start: " INT64_FORMAT ", end: " INT64_FORMAT ", label oid: %d]",
                        ee->edge_id, ee->start_vertex_id, ee->end_vertex_id,
                        ee->edge_label_table_oid)));

        return false;
    }

    /* not sure if we really need to zero out the entry, as we set everything */
    MemSet(ee, 0, sizeof(edge_entry));

    /*
     * Set the edge id - this is important as this is the hash key value used
     * for hash function collisions.
     */
    ee->edge_id = edge_id;
    ee->tid = tid;
    ee->start_vertex_id = start_vertex_id;
    ee->end_vertex_id = end_vertex_id;
    ee->edge_label_table_oid = edge_label_table_oid;
    ee->edge_label_name = (char *)edge_label_name;
    ee->edge_index = new_edge_index;
    ee->edge_property_count = edge_property_count;
    ee->edge_property_size = edge_property_size;
    ee->edge_property_hash = edge_property_hash;
    if (edge_index != NULL)
    {
        *edge_index = new_edge_index;
    }
    if (edge != NULL)
    {
        *edge = ee;
    }

    /* increment the number of loaded edges */
    ggctx->num_loaded_edges++;

    return true;
}

/*
 * Helper function to insert an entire vertex into the current GRAPH global
 * vertex hashtable. It will return false if there is a duplicate.
 */
static bool insert_vertex_entry(GRAPH_global_context *ggctx, graphid vertex_id,
                                Oid vertex_label_table_oid,
                                const char *vertex_label_name,
                                ItemPointerData tid)
{
    vertex_entry *ve = NULL;
    bool found = false;
    MemoryContext oldctx;

    /* search for the vertex */
    ve = (vertex_entry *)hash_search(ggctx->vertex_hashtable,
                                     (void *)&vertex_id, HASH_ENTER, &found);

    /* if the hash enter returned is NULL, error out */
    if (ve == NULL)
    {
        elog(ERROR, "insert_vertex_entry: hash table returned NULL for ve");
    }

    /* we should never have duplicates, warn and return false */
    if (found)
    {
        ereport(WARNING,
                (errcode(ERRCODE_DATA_EXCEPTION),
                 errmsg("vertex: [id: " INT64_FORMAT ", label oid: %d] %s",
                        vertex_id, vertex_label_table_oid,
                        "duplicate vertex found")));

        ereport(WARNING,
                (errcode(ERRCODE_DATA_EXCEPTION),
                 errmsg("previous vertex: [id: " INT64_FORMAT ", label oid: %d]",
                        ve->vertex_id, ve->vertex_label_table_oid)));

        return false;
    }

    /* again, MemSet may not be needed here */
    MemSet(ve, 0, sizeof(vertex_entry));

    /*
     * Set the vertex id - this is important as this is the hash key value
     * used for hash function collisions.
     */
    ve->vertex_id = vertex_id;
    /* set the label table oid for this vertex */
    ve->vertex_label_table_oid = vertex_label_table_oid;
    ve->vertex_label_name = (char *)vertex_label_name;
    /* set the TID for lazy property fetch */
    ve->tid = tid;
    ve->adj_edges_in = NULL;
    ve->adj_edges_out = NULL;
    ve->adj_edges_self = NULL;
    ve->edges_in_by_label = NULL;
    ve->edges_out_by_label = NULL;
    ve->edges_self_by_label = NULL;

    /* we also need to store the vertex id for clean up of vertex lists */
    oldctx = MemoryContextSwitchTo(ggctx->memory_context);
    ggctx->vertices = append_graphid(ggctx->vertices, vertex_id);
    MemoryContextSwitchTo(oldctx);

    /* increment the number of loaded vertices */
    ggctx->num_loaded_vertices++;

    return true;
}

/*
 * Helper function to append one edge to an existing vertex in the current
 * global vertex hashtable.
 */
static bool insert_vertex_edge(GRAPH_global_context *ggctx,
                               edge_entry *edge,
                               graphid start_vertex_id, graphid end_vertex_id,
                               graphid edge_id, int64 edge_index,
                               Oid edge_label_table_oid,
                               const char *edge_label_name)
{
    vertex_entry *start_value = NULL;
    vertex_entry *end_value = NULL;
    bool start_found = false;
    bool end_found = false;
    bool is_selfloop = false;
    MemoryContext oldctx;

    /* is it a self loop */
    is_selfloop = (start_vertex_id == end_vertex_id);

    /* search for the start vertex of the edge */
    start_value = (vertex_entry *)hash_search(ggctx->vertex_hashtable,
                                              (void *)&start_vertex_id,
                                              HASH_FIND, &start_found);

    if (!is_selfloop)
    {
        end_value = (vertex_entry *)hash_search(ggctx->vertex_hashtable,
                                                (void *)&end_vertex_id,
                                                HASH_FIND, &end_found);
    }
    else
    {
        end_found = start_found;
        end_value = start_value;
    }

    if (edge != NULL)
    {
        edge->start_vertex = start_found ? start_value : NULL;
        edge->end_vertex = end_found ? end_value : NULL;
    }

    /*
     * If we found the start_vertex_id and it is a self loop, add the edge to
     * adj_edges_self and we're done
     */
    if (start_found && is_selfloop)
    {
        oldctx = MemoryContextSwitchTo(ggctx->memory_context);
        start_value->adj_edges_self = append_graph_edge_adj(
            start_value->adj_edges_self, edge_id, start_vertex_id,
            start_value, edge_index);
        append_labeled_vertex_edge(&start_value->edges_self_by_label,
                                   edge_label_table_oid, edge_id,
                                   start_vertex_id, start_value, edge_index);
        MemoryContextSwitchTo(oldctx);
        return true;
    }
    /*
     * Otherwise, if we found the start_vertex_id add the edge to the
     * adj_edges_out list of the start vertex
     */
    else if (start_found)
    {
        oldctx = MemoryContextSwitchTo(ggctx->memory_context);
        start_value->adj_edges_out = append_graph_edge_adj(
            start_value->adj_edges_out, edge_id, end_vertex_id,
            end_value, edge_index);
        append_labeled_vertex_edge(&start_value->edges_out_by_label,
                                   edge_label_table_oid, edge_id,
                                   end_vertex_id, end_value, edge_index);
        MemoryContextSwitchTo(oldctx);
    }

    /*
     * If we found the start_vertex_id and the end_vertex_id add the edge to the
     * adj_edges_in list of the end vertex
     */
    if (start_found && end_found)
    {
        oldctx = MemoryContextSwitchTo(ggctx->memory_context);
        end_value->adj_edges_in = append_graph_edge_adj(
            end_value->adj_edges_in, edge_id, start_vertex_id,
            start_value, edge_index);
        append_labeled_vertex_edge(&end_value->edges_in_by_label,
                                   edge_label_table_oid, edge_id,
                                   start_vertex_id, start_value, edge_index);
        MemoryContextSwitchTo(oldctx);
        return true;
    }
    /*
     * Otherwise we need to generate the appropriate warning message about the
     * dangling edge that we found.
     */
    else if (!start_found && end_found)
    {
        ereport(WARNING,
                (errcode(ERRCODE_DATA_EXCEPTION),
                 errmsg("edge: [id: " INT64_FORMAT ", start: " INT64_FORMAT ", end: " INT64_FORMAT ", label: %s] %s",
                        edge_id, start_vertex_id, end_vertex_id,
                        edge_label_name, "start vertex not found")));
    }
    else if (start_found && !end_found)
    {
        ereport(WARNING,
                (errcode(ERRCODE_DATA_EXCEPTION),
                 errmsg("edge: [id: " INT64_FORMAT ", start: " INT64_FORMAT ", end: " INT64_FORMAT ", label: %s] %s",
                        edge_id, start_vertex_id, end_vertex_id,
                        edge_label_name, "end vertex not found")));
    }
    else
    {
        ereport(WARNING,
                (errcode(ERRCODE_DATA_EXCEPTION),
                 errmsg("edge: [id: " INT64_FORMAT ", start: " INT64_FORMAT ", end: " INT64_FORMAT ", label: %s] %s",
                        edge_id, start_vertex_id, end_vertex_id,
                        edge_label_name, "start and end vertices not found")));
    }

    return false;
}

static void append_labeled_vertex_edge(edge_label_adj_entry **head,
                                       Oid edge_label_table_oid,
                                       graphid edge_id,
                                       graphid next_vertex_id,
                                       vertex_entry *next_vertex_entry,
                                       int64 edge_index)
{
    edge_label_adj_entry *entry = NULL;

    for (entry = *head; entry != NULL; entry = entry->next)
    {
        if (entry->edge_label_table_oid == edge_label_table_oid)
        {
            entry->adj_edges = append_graph_edge_adj(entry->adj_edges,
                                                     edge_id,
                                                     next_vertex_id,
                                                     next_vertex_entry,
                                                     edge_index);
            return;
        }
    }

    entry = palloc0(sizeof(*entry));
    entry->edge_label_table_oid = edge_label_table_oid;
    entry->adj_edges = append_graph_edge_adj(NULL, edge_id, next_vertex_id,
                                             next_vertex_entry, edge_index);
    entry->next = *head;
    *head = entry;
}

static GraphEdgeAdjList *get_labeled_vertex_adj_edges(
    edge_label_adj_entry *head, Oid edge_label_table_oid)
{
    edge_label_adj_entry *entry = NULL;

    for (entry = head; entry != NULL; entry = entry->next)
    {
        if (entry->edge_label_table_oid == edge_label_table_oid)
        {
            return entry->adj_edges;
        }
    }

    return NULL;
}

static void free_labeled_vertex_edges(edge_label_adj_entry *head)
{
    edge_label_adj_entry *entry = head;

    while (entry != NULL)
    {
        edge_label_adj_entry *next = entry->next;

        free_graph_edge_adj(entry->adj_edges);
        pfree(entry);
        entry = next;
    }
}

static GraphEdgeAdjList *append_graph_edge_adj(GraphEdgeAdjList *container,
                                               graphid edge_id,
                                               graphid next_vertex_id,
                                               vertex_entry *next_vertex_entry,
                                               int64 edge_index)
{
    if (container == NULL)
    {
        container = palloc(sizeof(GraphEdgeAdjList));
        container->capacity = 8;
        container->size = 0;
        container->items = palloc(sizeof(GraphEdgeAdjEntry) *
                                  container->capacity);
    }
    else if (container->size >= container->capacity)
    {
        container->capacity *= 2;
        container->items = repalloc(container->items,
                                    sizeof(GraphEdgeAdjEntry) *
                                    container->capacity);
    }

    container->items[container->size].edge_id = edge_id;
    container->items[container->size].next_vertex_id = next_vertex_id;
    container->items[container->size].next_vertex_entry = next_vertex_entry;
    container->items[container->size].edge_index = edge_index;
    container->size++;

    return container;
}

static GraphEdgeAdjList *compact_graph_edge_adj(GraphEdgeAdjList *container)
{
    if (container == NULL)
    {
        return NULL;
    }

    if (container->size == 0)
    {
        pfree_if_not_null(container->items);
        container->items = NULL;
        container->capacity = 0;
        return container;
    }

    if (container->capacity > container->size)
    {
        container->items = repalloc(container->items,
                                    sizeof(GraphEdgeAdjEntry) *
                                    container->size);
        container->capacity = container->size;
    }

    return container;
}

static void compact_labeled_vertex_edges(edge_label_adj_entry *head)
{
    edge_label_adj_entry *entry = NULL;

    for (entry = head; entry != NULL; entry = entry->next)
    {
        entry->adj_edges = compact_graph_edge_adj(entry->adj_edges);
    }
}

static void free_graph_edge_adj(GraphEdgeAdjList *container)
{
    if (container == NULL)
    {
        return;
    }

    pfree_if_not_null(container->items);
    pfree(container);
}

/* helper routine to load all vertices into the GRAPH global vertex hashtable */
static void load_vertex_hashtable(GRAPH_global_context *ggctx,
                                  Snapshot snapshot,
                                  graph_label_list *vertex_labels,
                                  HTAB *vertex_filter,
                                  HTAB *vertex_label_filter)
{
    int i;

    /* go through all vertex label tables in list */
    for (i = 0; i < vertex_labels->count; i++)
    {
        Relation graph_vertex_label;
        TableScanDesc scan_desc;
        HeapTuple tuple;
        graph_label_entry *label_entry;
        char *vertex_label_name;
        Oid vertex_label_table_oid;
        TupleDesc tupdesc;
        int vertices_needed = -1;
        bool use_vertex_range = false;
        graphid min_vertex_id = 0;
        graphid max_vertex_id = 0;

        label_entry = &vertex_labels->entries[i];
        vertex_label_name = NameStr(label_entry->name);
        vertex_label_table_oid = label_entry->relation;
        if (vertex_label_filter != NULL &&
            !label_id_hash_contains(vertex_label_filter, label_entry->label_id))
        {
            continue;
        }

        /* open the relation (table) and begin the scan */
        graph_vertex_label = table_open(vertex_label_table_oid, AccessShareLock);
        /* get the tupdesc - we don't need to release this one */
        tupdesc = RelationGetDescr(graph_vertex_label);
        /* bail if the number of columns differs */
        if (tupdesc->natts != 2)
        {
            ereport(ERROR,
                    (errcode(ERRCODE_UNDEFINED_TABLE),
                     errmsg("Invalid number of attributes for %s.%s",
                     ggctx->graph_name, vertex_label_name)));
        }

        if (vertex_filter != NULL)
        {
            HASH_SEQ_STATUS status;
            GraphIdHashEntry *entry;

            vertices_needed = 0;
            hash_seq_init(&status, vertex_filter);
            while ((entry = hash_seq_search(&status)) != NULL)
            {
                if (get_graphid_label_id(entry->id) != label_entry->label_id)
                {
                    continue;
                }
                if (!use_vertex_range)
                {
                    min_vertex_id = entry->id;
                    max_vertex_id = entry->id;
                    use_vertex_range = true;
                }
                else
                {
                    if (entry->id < min_vertex_id)
                    {
                        min_vertex_id = entry->id;
                    }
                    if (entry->id > max_vertex_id)
                    {
                        max_vertex_id = entry->id;
                    }
                }
                vertices_needed++;
            }
            if (vertices_needed == 0)
            {
                table_close(graph_vertex_label, AccessShareLock);
                continue;
            }
            else
            {
                Oid index_oid;
                double label_tuples;
                double range_span;

                label_tuples = graph_vertex_label->rd_rel->reltuples;
                range_span = (double) (max_vertex_id - min_vertex_id + 1);
                if (label_tuples > 0 &&
                    range_span < (label_tuples / 2.0))
                {
                    index_oid = find_usable_btree_index_for_attr(
                        graph_vertex_label, Anum_ag_label_vertex_table_id);
                    if (OidIsValid(index_oid))
                    {
                        vertices_needed = load_vertex_tuples_by_index_range(
                            ggctx, graph_vertex_label, snapshot, index_oid,
                            tupdesc, vertex_label_table_oid, vertex_label_name,
                            vertex_filter, min_vertex_id, max_vertex_id,
                            vertices_needed);

                        table_close(graph_vertex_label, AccessShareLock);
                        continue;
                    }
                }
            }
        }

        scan_desc = table_beginscan(graph_vertex_label, snapshot, 0, NULL);
        /* get all tuples in table and insert them into graph hashtables */
        while((tuple = heap_getnext(scan_desc, ForwardScanDirection)) != NULL)
        {
            if (load_vertex_tuple(ggctx, tupdesc, tuple,
                                  vertex_label_table_oid,
                                  vertex_label_name, vertex_filter,
                                  use_vertex_range, min_vertex_id,
                                  max_vertex_id) &&
                vertices_needed > 0)
            {
                vertices_needed--;
                if (vertices_needed == 0)
                {
                    break;
                }
            }
        }

        /* end the scan and close the relation */
        table_endscan(scan_desc);
        table_close(graph_vertex_label, AccessShareLock);
    }
}

static int load_vertex_tuples_by_index_range(GRAPH_global_context *ggctx,
                                             Relation graph_vertex_label,
                                             Snapshot snapshot, Oid index_oid,
                                             TupleDesc tupdesc,
                                             Oid vertex_label_table_oid,
                                             const char *vertex_label_name,
                                             HTAB *vertex_filter,
                                             graphid min_vertex_id,
                                             graphid max_vertex_id,
                                             int vertices_needed)
{
    Relation index_rel;
    IndexScanDesc scan_desc;
    TupleTableSlot *slot;
    ScanKeyData scan_key[2];

    ScanKeyInit(&scan_key[0], Anum_ag_label_vertex_table_id,
                BTGreaterEqualStrategyNumber, F_INT8GE,
                GRAPHID_GET_DATUM(min_vertex_id));
    ScanKeyInit(&scan_key[1], Anum_ag_label_vertex_table_id,
                BTLessEqualStrategyNumber, F_INT8LE,
                GRAPHID_GET_DATUM(max_vertex_id));

    index_rel = index_open(index_oid, AccessShareLock);
    slot = table_slot_create(graph_vertex_label, NULL);
    scan_desc = index_beginscan(graph_vertex_label, index_rel, snapshot, NULL,
                                2, 0);
    index_rescan(scan_desc, scan_key, 2, NULL, 0);

    while (vertices_needed > 0 &&
           index_getnext_slot(scan_desc, ForwardScanDirection, slot))
    {
        bool should_free;
        HeapTuple tuple;

        tuple = ExecFetchSlotHeapTuple(slot, true, &should_free);
        if (load_vertex_tuple(ggctx, tupdesc, tuple,
                              vertex_label_table_oid,
                              vertex_label_name, vertex_filter, true,
                              min_vertex_id, max_vertex_id))
        {
            vertices_needed--;
        }
        if (should_free)
        {
            heap_freetuple(tuple);
        }
        ExecClearTuple(slot);
    }

    index_endscan(scan_desc);
    ExecDropSingleTupleTableSlot(slot);
    index_close(index_rel, AccessShareLock);

    return vertices_needed;
}

static bool load_vertex_tuple(GRAPH_global_context *ggctx, TupleDesc tupdesc,
                              HeapTuple tuple, Oid vertex_label_table_oid,
                              const char *vertex_label_name,
                              HTAB *vertex_filter,
                              bool use_vertex_range,
                              graphid min_vertex_id,
                              graphid max_vertex_id)
{
    graphid vertex_id;
    bool isnull;
    bool inserted = false;

    vertex_id = DatumGetInt64(heap_getattr(tuple,
                                           Anum_ag_label_vertex_table_id,
                                           tupdesc, &isnull));
    if (isnull)
    {
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_TABLE),
                 errmsg("vertex id is null for %s.%s",
                        ggctx->graph_name, vertex_label_name)));
    }
    if (use_vertex_range &&
        (vertex_id < min_vertex_id || vertex_id > max_vertex_id))
    {
        return false;
    }
    if (vertex_filter != NULL && !graphid_hash_contains(vertex_filter,
                                                        vertex_id))
    {
        return false;
    }

    /* insert vertex into vertex hashtable with TID (no property copy) */
    inserted = insert_vertex_entry(ggctx, vertex_id, vertex_label_table_oid,
                                   vertex_label_name, tuple->t_self);

    /* warn if there is a duplicate */
    if (!inserted)
    {
         ereport(WARNING,
                 (errcode(ERRCODE_DATA_EXCEPTION),
                  errmsg("ignored duplicate vertex")));
    }

    return true;
}

static void load_endpoint_vertex_skeletons(GRAPH_global_context *ggctx,
                                           HTAB *edge_endpoint_ids)
{
    HASH_SEQ_STATUS status;
    GraphIdHashEntry *entry;
    ItemPointerData invalid_tid;

    ItemPointerSetInvalid(&invalid_tid);
    hash_seq_init(&status, edge_endpoint_ids);
    while ((entry = hash_seq_search(&status)) != NULL)
    {
        label_cache_data *label_cache;
        Oid vertex_label_table_oid = InvalidOid;
        const char *vertex_label_name = NULL;

        label_cache = search_label_graph_oid_cache_cached(
            ggctx->graph_oid, get_graphid_label_id(entry->id));
        if (label_cache != NULL)
        {
            vertex_label_table_oid = label_cache->relation;
            vertex_label_name = NameStr(label_cache->name);
        }

        (void) insert_vertex_entry(ggctx, entry->id, vertex_label_table_oid,
                                   vertex_label_name, invalid_tid);
    }
}

/*
 * Helper function to load all of the GRAPH global hashtables (vertex & edge)
 * for the current global context.
 */
static void load_GRAPH_global_hashtables(GRAPH_global_context *ggctx,
                                         bool load_edge_property_metadata,
                                         Oid edge_label_oid,
                                         bool load_edge_metadata,
                                         bool load_vertex_metadata)
{
    Snapshot snapshot = GetActiveSnapshot();
    HTAB *edge_endpoint_ids = NULL;
    HTAB *edge_endpoint_label_ids = NULL;
    bool targeted_edge_label = false;

    /* initialize statistics */
    ggctx->num_loaded_vertices = 0;
    ggctx->num_loaded_edges = 0;

    if (OidIsValid(edge_label_oid))
    {
        label_cache_data *edge_label_cache;

        edge_label_cache = search_label_relation_cache_cached(edge_label_oid);
        targeted_edge_label =
            append_cached_graph_label(&ggctx->edge_labels, edge_label_cache,
                                      ggctx->graph_oid, LABEL_KIND_EDGE);
        if (!targeted_edge_label)
        {
            get_ag_labels_names(snapshot, ggctx->graph_oid,
                                &ggctx->vertex_labels, &ggctx->edge_labels);
        }

        if (load_edge_metadata)
        {
            edge_endpoint_ids = create_graphid_hash("VLE edge endpoint ids");
            edge_endpoint_label_ids =
                create_label_id_hash("VLE edge endpoint label ids");
            load_edge_hashtable(ggctx, snapshot, &ggctx->edge_labels,
                                load_edge_property_metadata, edge_label_oid,
                                edge_endpoint_ids);
        }
        if (edge_endpoint_ids != NULL &&
            hash_get_num_entries(edge_endpoint_ids) > 0)
        {
            HASH_SEQ_STATUS status;
            GraphIdHashEntry *entry;

            hash_seq_init(&status, edge_endpoint_ids);
            while ((entry = hash_seq_search(&status)) != NULL)
            {
                add_label_id_hash(edge_endpoint_label_ids,
                                  get_graphid_label_id(entry->id));
            }
        }
        /*
         * Adjacency-index VLE loads do not materialize vertex metadata. Keep
         * the label surface equally narrow: vertex labels are only needed
         * when load_vertex_hashtable() will scan vertex relations below.
         */
        if (load_vertex_metadata &&
            (edge_endpoint_ids == NULL ||
             hash_get_num_entries(edge_endpoint_ids) == 0))
        {
            if (targeted_edge_label)
            {
                graph_label_list edge_labels = {0};

                get_ag_labels_names(snapshot, ggctx->graph_oid,
                                    &ggctx->vertex_labels,
                                    &edge_labels);
                free_ag_label_names(&edge_labels);
            }
            load_vertex_hashtable(ggctx, snapshot, &ggctx->vertex_labels,
                                  NULL, NULL);
        }
        else if (load_vertex_metadata)
        {
            if (targeted_edge_label)
            {
                append_endpoint_vertex_labels(ggctx,
                                              edge_endpoint_label_ids);
            }
            load_vertex_hashtable(ggctx, snapshot, &ggctx->vertex_labels,
                                  edge_endpoint_ids, edge_endpoint_label_ids);
        }
        else if (load_edge_metadata &&
                 edge_endpoint_ids != NULL &&
                 hash_get_num_entries(edge_endpoint_ids) > 0)
        {
            load_endpoint_vertex_skeletons(ggctx, edge_endpoint_ids);
        }
        ggctx->adjacency_index_candidate_source =
            targeted_edge_label &&
            edge_label_has_age_adjacency_indexes(edge_label_oid);
        if (load_edge_metadata && !ggctx->adjacency_index_candidate_source)
        {
            link_edge_hashtable_to_vertices(ggctx);
        }
        if (edge_endpoint_ids != NULL)
        {
            hash_destroy(edge_endpoint_ids);
        }
        if (edge_endpoint_label_ids != NULL)
        {
            hash_destroy(edge_endpoint_label_ids);
        }
    }
    else
    {
        get_ag_labels_names(snapshot, ggctx->graph_oid, &ggctx->vertex_labels,
                            &ggctx->edge_labels);

        /* insert all of our vertices */
        if (load_vertex_metadata)
        {
            load_vertex_hashtable(ggctx, snapshot, &ggctx->vertex_labels, NULL,
                                  NULL);
        }

        /* insert all of our edges */
        if (load_edge_metadata)
        {
            load_edge_hashtable(ggctx, snapshot, &ggctx->edge_labels,
                                load_edge_property_metadata, InvalidOid, NULL);
        }
    }
    ggctx->edge_property_metadata_loaded = load_edge_property_metadata;
    ggctx->edge_metadata_loaded = load_edge_metadata;
    ggctx->vertex_metadata_loaded = load_vertex_metadata;
    ggctx->loaded_edge_label_oid = edge_label_oid;
}

static bool edge_label_has_age_adjacency_indexes(Oid edge_label_oid)
{
    Oid out_index_oid;
    Oid in_index_oid;

    get_edge_label_age_adjacency_indexes(edge_label_oid, &out_index_oid,
                                         &in_index_oid);
    return OidIsValid(out_index_oid) && OidIsValid(in_index_oid);
}

static void get_edge_label_age_adjacency_indexes(Oid edge_label_oid,
                                                 Oid *out_index_oid,
                                                 Oid *in_index_oid)
{
    static Oid age_adjacency_am_oid = InvalidOid;
    Relation edge_rel;
    List *index_list;
    ListCell *lc;

    Assert(out_index_oid != NULL);
    Assert(in_index_oid != NULL);

    *out_index_oid = InvalidOid;
    *in_index_oid = InvalidOid;

    if (!OidIsValid(edge_label_oid))
    {
        return;
    }

    if (!OidIsValid(age_adjacency_am_oid))
    {
        age_adjacency_am_oid = get_index_am_oid("age_adjacency", true);
        if (!OidIsValid(age_adjacency_am_oid))
        {
            return;
        }
    }

    edge_rel = relation_open(edge_label_oid, AccessShareLock);
    index_list = RelationGetIndexList(edge_rel);

    foreach(lc, index_list)
    {
        Oid index_oid = lfirst_oid(lc);
        Relation index_rel;

        index_rel = index_open(index_oid, AccessShareLock);
        if (index_rel->rd_rel->relam == age_adjacency_am_oid &&
            index_rel->rd_index != NULL &&
            index_rel->rd_index->indisvalid &&
            index_rel->rd_index->indisready)
        {
            if (age_adjacency_index_matches(index_rel, true))
            {
                *out_index_oid = index_oid;
            }
            if (age_adjacency_index_matches(index_rel, false))
            {
                *in_index_oid = index_oid;
            }
        }
        index_close(index_rel, AccessShareLock);
        if (OidIsValid(*out_index_oid) && OidIsValid(*in_index_oid))
        {
            break;
        }
    }

    list_free(index_list);
    relation_close(edge_rel, AccessShareLock);
}

static bool age_adjacency_index_matches(Relation index_rel, bool outgoing)
{
    int2vector *indkey;

    if (index_rel->rd_index->indnkeyatts != 3 ||
        index_rel->rd_index->indnatts != 3)
    {
        return false;
    }

    indkey = &index_rel->rd_index->indkey;

    if (outgoing)
    {
        return indkey->values[0] == Anum_ag_label_edge_table_start_id &&
               indkey->values[1] == Anum_ag_label_edge_table_id &&
               indkey->values[2] == Anum_ag_label_edge_table_end_id;
    }

    return indkey->values[0] == Anum_ag_label_edge_table_end_id &&
           indkey->values[1] == Anum_ag_label_edge_table_id &&
           indkey->values[2] == Anum_ag_label_edge_table_start_id;
}

/*
 * Helper routine to load all edges into the GRAPH global edge and vertex
 * hashtables.
 */
static void load_edge_hashtable(GRAPH_global_context *ggctx,
                                Snapshot snapshot,
                                graph_label_list *edge_labels,
                                bool load_edge_property_metadata,
                                Oid edge_label_oid,
                                HTAB *edge_endpoint_ids)
{
    int i;

    /* go through all edge label tables in list */
    for (i = 0; i < edge_labels->count; i++)
    {
        Relation graph_edge_label;
        TableScanDesc scan_desc;
        HeapTuple tuple;
        graph_label_entry *label_entry;
        char *edge_label_name;
        Oid edge_label_table_oid;
        TupleDesc tupdesc;

        label_entry = &edge_labels->entries[i];
        edge_label_name = NameStr(label_entry->name);
        edge_label_table_oid = label_entry->relation;
        if (OidIsValid(edge_label_oid) &&
            edge_label_table_oid != edge_label_oid)
        {
            continue;
        }

        /* open the relation (table) and begin the scan */
        graph_edge_label = table_open(edge_label_table_oid, AccessShareLock);
        scan_desc = table_beginscan(graph_edge_label, snapshot, 0, NULL);
        /* get the tupdesc - we don't need to release this one */
        tupdesc = RelationGetDescr(graph_edge_label);
        /* bail if the number of columns differs */
        if (tupdesc->natts != 4)
        {
            ereport(ERROR,
                    (errcode(ERRCODE_UNDEFINED_TABLE),
                     errmsg("Invalid number of attributes for %s.%s",
                     ggctx->graph_name, edge_label_name)));
        }
        /* get all tuples in table and insert them into graph hashtables */
        while((tuple = heap_getnext(scan_desc, ForwardScanDirection)) != NULL)
        {
            graphid edge_id;
            graphid edge_vertex_start_id;
            graphid edge_vertex_end_id;
            Datum edge_properties;
            bool isnull;
            bool inserted = false;
            int edge_property_count = 0;
            int edge_property_size = 0;
            uint32 edge_property_hash = 0;
            int64 edge_index = 0;
            edge_entry *ee = NULL;

            edge_id = DatumGetInt64(heap_getattr(tuple,
                                                 Anum_ag_label_edge_table_id,
                                                 tupdesc, &isnull));
            if (isnull)
            {
                ereport(ERROR,
                        (errcode(ERRCODE_UNDEFINED_TABLE),
                         errmsg("edge id is null for %s.%s",
                                ggctx->graph_name, edge_label_name)));
            }

            edge_vertex_start_id = DatumGetInt64(heap_getattr(
                tuple, Anum_ag_label_edge_table_start_id, tupdesc, &isnull));
            if (isnull)
            {
                ereport(ERROR,
                        (errcode(ERRCODE_UNDEFINED_TABLE),
                         errmsg("edge start_id is null for %s.%s",
                                ggctx->graph_name, edge_label_name)));
            }

            edge_vertex_end_id = DatumGetInt64(heap_getattr(
                tuple, Anum_ag_label_edge_table_end_id, tupdesc, &isnull));
            if (isnull)
            {
                ereport(ERROR,
                        (errcode(ERRCODE_UNDEFINED_TABLE),
                         errmsg("edge end_id is null for %s.%s",
                                ggctx->graph_name, edge_label_name)));
            }

            if (load_edge_property_metadata)
            {
                edge_properties = heap_getattr(
                    tuple, Anum_ag_label_edge_table_properties, tupdesc,
                    &isnull);
                if (isnull)
                {
                    ereport(ERROR,
                            (errcode(ERRCODE_UNDEFINED_TABLE),
                             errmsg("edge properties are null for %s.%s",
                                    ggctx->graph_name, edge_label_name)));
                }
                else
                {
                    agtype *agt_edge_properties =
                        DATUM_GET_AGTYPE_P(edge_properties);

                    edge_property_count =
                        AGTYPE_CONTAINER_SIZE(&agt_edge_properties->root);
                    edge_property_size = VARSIZE_ANY(agt_edge_properties);
                    edge_property_hash =
                        datum_image_hash(edge_properties, false, -1);
                }
            }

            /* insert edge into edge hashtable with TID (no property copy) */
            inserted = insert_edge_entry(ggctx, edge_id, tuple->t_self,
                                         edge_vertex_start_id,
                                         edge_vertex_end_id,
                                         edge_label_table_oid,
                                         edge_label_name,
                                         edge_property_count,
                                         edge_property_size,
                                         edge_property_hash,
                                         &edge_index, &ee);

            /* warn if there is a duplicate */
            if (!inserted)
            {
                 ereport(WARNING,
                         (errcode(ERRCODE_DATA_EXCEPTION),
                          errmsg("ignored duplicate edge")));
                 continue;
            }

            if (ggctx->edge_tid_hashtable != NULL)
            {
                EdgeTidHashEntry *tid_entry;
                bool found;

                tid_entry = hash_search(ggctx->edge_tid_hashtable,
                                        &tuple->t_self, HASH_ENTER, &found);
                (void) found;
                if (tid_entry == NULL)
                {
                    elog(ERROR,
                         "load_edge_hashtable: edge TID hash returned NULL");
                }
                tid_entry->edge = ee;
            }

            /* insert the edge into the start and end vertices edge lists */
            if (edge_endpoint_ids != NULL)
            {
                add_graphid_hash(edge_endpoint_ids, edge_vertex_start_id);
                add_graphid_hash(edge_endpoint_ids, edge_vertex_end_id);
            }
            else
            {
                inserted = insert_vertex_edge(ggctx, ee,
                                              edge_vertex_start_id,
                                              edge_vertex_end_id, edge_id,
                                              edge_index,
                                              edge_label_table_oid,
                                              edge_label_name);
                if (!inserted)
                {
                    ereport(WARNING,
                            (errcode(ERRCODE_DATA_EXCEPTION),
                             errmsg("ignored malformed or dangling edge")));
                }
            }
        }

        /* end the scan and close the relation */
        table_endscan(scan_desc);
        table_close(graph_edge_label, AccessShareLock);
    }
}

static void link_edge_hashtable_to_vertices(GRAPH_global_context *ggctx)
{
    HASH_SEQ_STATUS status;
    edge_entry *ee;

    hash_seq_init(&status, ggctx->edge_hashtable);
    while ((ee = hash_seq_search(&status)) != NULL)
    {
        if (!insert_vertex_edge(ggctx, ee, ee->start_vertex_id,
                                ee->end_vertex_id, ee->edge_id,
                                ee->edge_index,
                                ee->edge_label_table_oid,
                                ee->edge_label_name))
        {
            ereport(WARNING,
                    (errcode(ERRCODE_DATA_EXCEPTION),
                     errmsg("ignored malformed or dangling edge")));
        }
    }
}

/*
 * Helper function to freeze the GRAPH global hashtables from additional
 * inserts. This may, or may not, be useful. Currently, these hashtables are
 * only seen by the creating process and only for reading.
 */
static void freeze_GRAPH_global_hashtables(GRAPH_global_context *ggctx)
{
    GraphIdNode *curr_vertex = NULL;

    curr_vertex = peek_stack_head(ggctx->vertices);
    while (curr_vertex != NULL)
    {
        vertex_entry *value = NULL;
        bool found = false;
        graphid vertex_id = get_graphid(curr_vertex);

        value = (vertex_entry *)hash_search(ggctx->vertex_hashtable,
                                            (void *)&vertex_id, HASH_FIND,
                                            &found);
        if (found == false)
        {
            elog(ERROR, "missing vertex entry during adjacency compaction");
        }

        value->adj_edges_in = compact_graph_edge_adj(value->adj_edges_in);
        value->adj_edges_out = compact_graph_edge_adj(value->adj_edges_out);
        value->adj_edges_self = compact_graph_edge_adj(value->adj_edges_self);
        compact_labeled_vertex_edges(value->edges_in_by_label);
        compact_labeled_vertex_edges(value->edges_out_by_label);
        compact_labeled_vertex_edges(value->edges_self_by_label);

        curr_vertex = next_GraphIdNode(curr_vertex);
    }

    if (ggctx->vertex_metadata_loaded)
        hash_freeze(ggctx->vertex_hashtable);
    if (ggctx->edge_metadata_loaded)
        hash_freeze(ggctx->edge_hashtable);
    hash_freeze(ggctx->edge_tid_hashtable);
}

/*
 * Helper function to free the entire specified GRAPH global context. After
 * running this you should not use the pointer in ggctx.
 */
static bool free_specific_GRAPH_global_context(GRAPH_global_context *ggctx)
{
    HASH_SEQ_STATUS vertex_status;
    vertex_entry *value;
    MemoryContext graph_ctx;

    /* don't do anything if NULL */
    if (ggctx == NULL)
    {
        return true;
    }
    graph_ctx = ggctx->memory_context;

    /* free the graph name */
    pfree_if_not_null(ggctx->graph_name);
    ggctx->graph_name = NULL;

    ggctx->graph_oid = InvalidOid;
    ggctx->next = NULL;

    /*
     * Drop the traversal list before freeing vertex payloads.  The hash table is
     * the source of owned vertex entries; this list is only an auxiliary
     * traversal surface and must not participate in payload cleanup.
     */
    free_ListGraphId(ggctx->vertices);
    ggctx->vertices = NULL;

    /*
     * Free payloads from the vertex hash itself.  The vertices list is only a
     * traversal/cleanup aid and can be stale after malformed edge handling or a
     * partially-built context; cleanup must still be able to reclaim the hash.
     */
    hash_seq_init(&vertex_status, ggctx->vertex_hashtable);
    while ((value = hash_seq_search(&vertex_status)) != NULL)
    {
        /* free the adjacency lists associated with this vertex */
        free_graph_edge_adj(value->adj_edges_in);
        free_graph_edge_adj(value->adj_edges_out);
        free_graph_edge_adj(value->adj_edges_self);
        free_labeled_vertex_edges(value->edges_in_by_label);
        free_labeled_vertex_edges(value->edges_out_by_label);
        free_labeled_vertex_edges(value->edges_self_by_label);
        pfree_if_not_null(DatumGetPointer(value->cached_properties));
        pfree_if_not_null(value->cached_property_key);
        pfree_if_not_null(DatumGetPointer(value->cached_property_value));

        value->adj_edges_in = NULL;
        value->adj_edges_out = NULL;
        value->adj_edges_self = NULL;
        value->edges_in_by_label = NULL;
        value->edges_out_by_label = NULL;
        value->edges_self_by_label = NULL;
        value->cached_properties = (Datum) 0;
        value->cached_properties_valid = false;
        value->cached_property_key = NULL;
        value->cached_property_key_len = 0;
        value->cached_property_key_char = '\0';
        value->cached_property_value = (Datum) 0;
        value->cached_property_value_valid = false;

    }
    if (ggctx->edge_hashtable != NULL)
    {
        HASH_SEQ_STATUS edge_status;
        edge_entry *edge_value;

        hash_seq_init(&edge_status, ggctx->edge_hashtable);
        while ((edge_value = hash_seq_search(&edge_status)) != NULL)
        {
            pfree_if_not_null(edge_value->cached_property_key);
            pfree_if_not_null(DatumGetPointer(
                edge_value->cached_property_value));
            edge_value->cached_property_key = NULL;
            edge_value->cached_property_key_len = 0;
            edge_value->cached_property_key_char = '\0';
            edge_value->cached_property_value = (Datum) 0;
            edge_value->cached_property_value_valid = false;
        }
    }

    free_ag_label_names(&ggctx->vertex_labels);
    free_ag_label_names(&ggctx->edge_labels);

    /* free the hashtables */
    hash_destroy(ggctx->vertex_hashtable);
    hash_destroy(ggctx->edge_hashtable);
    hash_destroy(ggctx->edge_tid_hashtable);

    ggctx->vertex_hashtable = NULL;
    ggctx->edge_hashtable = NULL;
    ggctx->edge_tid_hashtable = NULL;

    /* free the context */
    pfree_if_not_null(ggctx);
    ggctx = NULL;
    if (graph_ctx != NULL)
        MemoryContextDelete(graph_ctx);

    return true;
}

/*
 * Helper function to manage the GRAPH global contexts. It will create the
 * context for the graph specified, provided it isn't already built and valid.
 * During processing it will free (delete) all invalid GRAPH contexts. It
 * returns the GRAPH global context for the specified graph.
 *
 * NOTE: Function uses a MUTEX for global_graph_contexts
 *
 */
GRAPH_global_context *manage_GRAPH_global_contexts(char *graph_name,
                                                   Oid graph_oid)
{
    return manage_GRAPH_global_contexts_internal(graph_name, strlen(graph_name),
                                                graph_oid, true, InvalidOid,
                                                true, true);
}

GRAPH_global_context *manage_GRAPH_global_contexts_len(const char *graph_name,
                                                       int graph_name_len,
                                                       Oid graph_oid)
{
    return manage_GRAPH_global_contexts_internal(graph_name, graph_name_len,
                                                graph_oid, true, InvalidOid,
                                                true, true);
}

GRAPH_global_context *manage_GRAPH_global_contexts_len_for_vle(
    const char *graph_name, int graph_name_len, Oid graph_oid,
    bool load_edge_property_metadata, Oid edge_label_oid,
    bool load_edge_metadata, bool load_vertex_metadata)
{
    return manage_GRAPH_global_contexts_internal(graph_name, graph_name_len,
                                                graph_oid,
                                                load_edge_property_metadata,
                                                edge_label_oid,
                                                load_edge_metadata,
                                                load_vertex_metadata);
}

static GRAPH_global_context *manage_GRAPH_global_contexts_internal(
    const char *graph_name, int graph_name_len, Oid graph_oid,
    bool load_edge_property_metadata, Oid edge_label_oid,
    bool load_edge_metadata, bool load_vertex_metadata)
{
    GRAPH_global_context *new_ggctx = NULL;
    GRAPH_global_context *curr_ggctx = NULL;
    GRAPH_global_context *prev_ggctx = NULL;
    GRAPH_global_context *found_ggctx = NULL;
    Snapshot invalidation_snapshot = NULL;
    MemoryContext oldctx = NULL;

    /* we need a higher context, or one that isn't destroyed by SRF exit */
    oldctx = MemoryContextSwitchTo(TopMemoryContext);

    /*
     * We need to see if any GRAPH global contexts already exist and if any do
     * for this particular graph. There are 5 possibilities -
     *
     *     1) There are no global contexts.
     *     2) One does exist for this graph but, is invalid.
     *     3) One does exist for this graph and is valid.
     *     4) One or more other contexts do exist and all are valid.
     *     5) One or more other contexts do exist but, one or more are invalid.
     */

    /* lock the global contexts list */
    pthread_mutex_lock(&global_graph_contexts_container.mutex_lock);

    /* free the invalidated GRAPH global contexts first */
    prev_ggctx = NULL;
    curr_ggctx = global_graph_contexts_container.contexts;
    while (curr_ggctx != NULL)
    {
        GRAPH_global_context *next_ggctx = curr_ggctx->next;

        /* if the transaction ids have changed, we have an invalid graph */
        if (is_ggctx_invalid_with_snapshot(curr_ggctx,
                                           &invalidation_snapshot))
        {
            bool success = false;

            /*
             * If prev_ggctx is NULL then we are freeing the top of the
             * contexts. So, we need to point the contexts variable to the
             * new (next) top context, if there is one.
             */
            if (prev_ggctx == NULL)
            {
                global_graph_contexts_container.contexts = next_ggctx;
            }
            else
            {
                prev_ggctx->next = curr_ggctx->next;
            }

            /* free the current graph context */
            success = free_specific_GRAPH_global_context(curr_ggctx);

            /* if it wasn't successfull, there was a missing vertex entry */
            if (!success)
            {
                /* unlock the mutex so we don't get a deadlock */
                pthread_mutex_unlock(&global_graph_contexts_container.mutex_lock);

                ereport(ERROR, (errcode(ERRCODE_DATA_EXCEPTION),
                                errmsg("missing vertex or edge entry during free")));
            }
        }
        else
        {
            if (found_ggctx == NULL &&
                curr_ggctx->graph_oid == graph_oid &&
                (!load_vertex_metadata ||
                 curr_ggctx->vertex_metadata_loaded) &&
                graph_context_satisfies_edge_load(
                    curr_ggctx, load_edge_property_metadata, edge_label_oid,
                    load_edge_metadata))
            {
                found_ggctx = curr_ggctx;
            }
            prev_ggctx = curr_ggctx;
        }

        /* advance to the next context */
        curr_ggctx = next_ggctx;
    }

    if (found_ggctx != NULL)
    {
        /* switch our context back */
        MemoryContextSwitchTo(oldctx);

        /* we are done unlock the global contexts list */
        pthread_mutex_unlock(&global_graph_contexts_container.mutex_lock);

        return found_ggctx;
    }

    /* otherwise, we need to create one and possibly attach it */
    new_ggctx = palloc(sizeof(GRAPH_global_context));
    MemSet(new_ggctx, 0, sizeof(GRAPH_global_context));
    new_ggctx->memory_context =
        AllocSetContextCreate(TopMemoryContext,
                              "AGE global graph context",
                              ALLOCSET_DEFAULT_SIZES);

    if (global_graph_contexts_container.contexts != NULL)
    {
        new_ggctx->next = global_graph_contexts_container.contexts;
    }
    else
    {
        new_ggctx->next = NULL;
    }

    /* set the global context variable */
    global_graph_contexts_container.contexts = new_ggctx;

    MemoryContextSwitchTo(new_ggctx->memory_context);

    /* set the graph name and oid */
    new_ggctx->graph_name = pnstrdup(graph_name, graph_name_len);
    new_ggctx->graph_oid = graph_oid;

    /* set the graph version counter for cache invalidation */
    new_ggctx->graph_version = get_graph_version(graph_oid);

    /* set snapshot fields for SNAPSHOT fallback mode */
    {
        Snapshot snap = GetActiveSnapshot();

        new_ggctx->xmin = snap->xmin;
        new_ggctx->xmax = snap->xmax;
        new_ggctx->curcid = snap->curcid;
    }

    /* initialize our vertices list */
    new_ggctx->vertices = NULL;

    /* build the hashtables for this graph */
    create_GRAPH_global_hashtables(new_ggctx);
    load_GRAPH_global_hashtables(new_ggctx, load_edge_property_metadata,
                                 edge_label_oid, load_edge_metadata,
                                 load_vertex_metadata);
    freeze_GRAPH_global_hashtables(new_ggctx);

    /* unlock the global contexts list */
    pthread_mutex_unlock(&global_graph_contexts_container.mutex_lock);

    /* switch back to the previous memory context */
    MemoryContextSwitchTo(oldctx);

    return new_ggctx;
}

static bool graph_context_satisfies_edge_load(GRAPH_global_context *ggctx,
                                              bool load_edge_property_metadata,
                                              Oid edge_label_oid,
                                              bool load_edge_metadata)
{
    if (load_edge_metadata && !ggctx->edge_metadata_loaded)
    {
        return false;
    }

    if (load_edge_property_metadata && !ggctx->edge_property_metadata_loaded)
    {
        return false;
    }

    if (!OidIsValid(ggctx->loaded_edge_label_oid))
    {
        return true;
    }

    if (!OidIsValid(edge_label_oid))
    {
        return false;
    }

    return ggctx->loaded_edge_label_oid == edge_label_oid;
}

/*
 * Helper function to delete all of the global graph contexts used by the
 * process. When done the global global_graph_contexts will be NULL.
 *
 * NOTE: Function uses a MUTEX global_graph_contexts_mutex
 */
static bool delete_GRAPH_global_contexts(void)
{
    GRAPH_global_context *curr_ggctx = NULL;
    bool retval = false;

    /* lock contexts list */
    pthread_mutex_lock(&global_graph_contexts_container.mutex_lock);

    /* get the first context, if any */
    curr_ggctx = global_graph_contexts_container.contexts;

    /* free all GRAPH global contexts */
    while (curr_ggctx != NULL)
    {
        GRAPH_global_context *next_ggctx = curr_ggctx->next;
        bool success = false;

        /* free the current graph context */
        success = free_specific_GRAPH_global_context(curr_ggctx);

        /* if it wasn't successfull, there was a missing vertex entry */
        if (!success)
        {
            /* unlock the mutex so we don't get a deadlock */
            pthread_mutex_unlock(&global_graph_contexts_container.mutex_lock);

            ereport(ERROR, (errcode(ERRCODE_DATA_EXCEPTION),
                            errmsg("missing vertex or edge entry during free")));
        }

        /* advance to the next context */
        curr_ggctx = next_ggctx;

        retval = true;
    }

    /* reset the head of the contexts to NULL */
    global_graph_contexts_container.contexts = NULL;

    /* unlock the global contexts list */
    pthread_mutex_unlock(&global_graph_contexts_container.mutex_lock);

    return retval;
}

/*
 * Helper function to delete a specific global graph context used by the
 * process.
 */
static bool delete_specific_GRAPH_global_contexts_len(const char *graph_name,
                                                      int graph_name_len)
{
    Oid graph_oid = InvalidOid;

    if (graph_name == NULL)
    {
        return false;
    }

    graph_oid = get_cached_global_graph_oid_len(graph_name, graph_name_len);

    return delete_specific_GRAPH_global_context_by_oid(graph_oid);
}

static Oid get_cached_global_graph_oid_len(const char *graph_name,
                                           int graph_name_len)
{
    static NameData cached_graph_name;
    static Oid cached_graph_oid = InvalidOid;
    static uint64 cached_generation = 0;
    uint64 current_generation = get_graph_cache_generation();
    char *graph_name_cstr;
    NameData graph_name_buf;
    bool free_graph_name = false;

    if (OidIsValid(cached_graph_oid) &&
        cached_generation == current_generation &&
        graph_name_len < NAMEDATALEN &&
        strncmp(NameStr(cached_graph_name), graph_name, graph_name_len) == 0 &&
        NameStr(cached_graph_name)[graph_name_len] == '\0')
    {
        return cached_graph_oid;
    }

    if (graph_name_len < NAMEDATALEN)
    {
        memcpy(NameStr(graph_name_buf), graph_name, graph_name_len);
        NameStr(graph_name_buf)[graph_name_len] = '\0';
        graph_name_cstr = NameStr(graph_name_buf);
    }
    else
    {
        graph_name_cstr = pnstrdup(graph_name, graph_name_len);
        free_graph_name = true;
    }

    cached_graph_oid = get_graph_oid(graph_name_cstr);
    if (OidIsValid(cached_graph_oid))
    {
        if (graph_name_len < NAMEDATALEN)
        {
            cached_graph_name = graph_name_buf;
        }
        else
        {
            namestrcpy(&cached_graph_name, graph_name_cstr);
        }
        cached_generation = current_generation;
    }
    if (free_graph_name)
    {
        pfree(graph_name_cstr);
    }

    return cached_graph_oid;
}

static void get_global_graph_scalar_arg_no_copy(char *funcname,
                                                agtype *agt_arg,
                                                enum agtype_value_type type,
                                                bool error,
                                                agtype_value *result,
                                                bool *needs_free)
{
    bool found;

    Assert(funcname != NULL);
    Assert(agt_arg != NULL);
    Assert(result != NULL);
    Assert(needs_free != NULL);

    if (!AGTYPE_CONTAINER_IS_SCALAR(&agt_arg->root))
    {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("%s: agtype argument must be a scalar", funcname)));
    }

    found = get_ith_agtype_value_from_container_no_copy(&agt_arg->root, 0,
                                                        result, needs_free);
    Assert(found);

    if (error && result->type == AGTV_NULL)
    {
        if (*needs_free)
        {
            pfree_agtype_value_content(result);
        }
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("%s: agtype argument must not be AGTV_NULL",
                        funcname)));
    }

    if (error && result->type != type)
    {
        if (*needs_free)
        {
            pfree_agtype_value_content(result);
        }
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("%s: agtype argument of wrong type", funcname)));
    }
}

static bool delete_specific_GRAPH_global_context_by_oid(Oid graph_oid)
{
    GRAPH_global_context *prev_ggctx = NULL;
    GRAPH_global_context *curr_ggctx = NULL;

    if (!OidIsValid(graph_oid))
    {
        return false;
    }

    /* lock the global contexts list */
    pthread_mutex_lock(&global_graph_contexts_container.mutex_lock);

    /* get the first context, if any */
    curr_ggctx = global_graph_contexts_container.contexts;

    /* find the specified GRAPH global context */
    while (curr_ggctx != NULL)
    {
        GRAPH_global_context *next_ggctx = curr_ggctx->next;

        if (curr_ggctx->graph_oid == graph_oid)
        {
            bool success = false;
            /*
             * If prev_ggctx is NULL then we are freeing the top of the
             * contexts. So, we need to point the global variable to the
             * new (next) top context, if there is one.
             */
            if (prev_ggctx == NULL)
            {
                global_graph_contexts_container.contexts = next_ggctx;
            }
            else
            {
                prev_ggctx->next = curr_ggctx->next;
            }

            /* free the current graph context */
            success = free_specific_GRAPH_global_context(curr_ggctx);

            /* unlock the global contexts list */
            pthread_mutex_unlock(&global_graph_contexts_container.mutex_lock);

            /* if it wasn't successfull, there was a missing vertex entry */
            if (!success)
            {
                ereport(ERROR, (errcode(ERRCODE_DATA_EXCEPTION),
                                errmsg("missing vertex_entry during free")));
            }

            /* we found and freed it, return true */
            return true;
        }

        /* save the current as previous and advance to the next one */
        prev_ggctx = curr_ggctx;
        curr_ggctx = next_ggctx;
    }

    /* unlock the global contexts list */
    pthread_mutex_unlock(&global_graph_contexts_container.mutex_lock);

    /* we didn't find it, return false */
    return false;
}

/*
 * Helper function to retrieve a vertex_entry from the graph's vertex hash
 * table. If there isn't one, it returns a NULL. The latter is necessary for
 * checking if the vsid and veid entries exist.
 */
vertex_entry *get_vertex_entry(GRAPH_global_context *ggctx, graphid vertex_id)
{
    vertex_entry *ve = NULL;
    bool found = false;

    /* retrieve the current vertex entry */
    ve = (vertex_entry *)hash_search(ggctx->vertex_hashtable,
                                     (void *)&vertex_id, HASH_FIND, &found);
    return ve;
}

vertex_entry *ensure_vertex_entry_skeleton(GRAPH_global_context *ggctx,
                                           graphid vertex_id)
{
    vertex_entry *ve;
    label_cache_data *label_cache;
    ItemPointerData invalid_tid;
    Oid vertex_label_table_oid = InvalidOid;
    const char *vertex_label_name = NULL;

    ve = get_vertex_entry(ggctx, vertex_id);
    if (ve != NULL)
        return ve;

    label_cache = search_label_graph_oid_cache_cached(
        ggctx->graph_oid, get_graphid_label_id(vertex_id));
    if (label_cache != NULL)
    {
        vertex_label_table_oid = label_cache->relation;
        vertex_label_name = NameStr(label_cache->name);
    }

    ItemPointerSetInvalid(&invalid_tid);
    (void) insert_vertex_entry(ggctx, vertex_id, vertex_label_table_oid,
                               vertex_label_name, invalid_tid);

    return get_vertex_entry(ggctx, vertex_id);
}

/* helper function to retrieve an edge_entry from the graph's edge hash table */
edge_entry *find_edge_entry(GRAPH_global_context *ggctx, graphid edge_id)
{
    edge_entry *ee = NULL;
    bool found = false;

    ee = (edge_entry *)hash_search(ggctx->edge_hashtable, (void *)&edge_id,
                                   HASH_FIND, &found);
    if (!found)
        return NULL;

    return ee;
}

/* helper function to retrieve an edge_entry from the graph's edge hash table */
edge_entry *get_edge_entry(GRAPH_global_context *ggctx, graphid edge_id)
{
    edge_entry *ee = NULL;

    ee = find_edge_entry(ggctx, edge_id);
    /* it should be found, otherwise we have problems */
    Assert(ee != NULL);

    return ee;
}

edge_entry *get_edge_entry_by_tid(GRAPH_global_context *ggctx,
                                  const ItemPointerData *tid)
{
    EdgeTidHashEntry *entry = NULL;
    bool found = false;

    entry = hash_search(ggctx->edge_tid_hashtable, tid, HASH_FIND, &found);
    if (!found)
    {
        return NULL;
    }

    return entry->edge;
}

bool get_edge_entry_vle_fields_by_tid(GRAPH_global_context *ggctx,
                                      const ItemPointerData *tid,
                                      graphid *edge_id,
                                      Oid *label_table_oid,
                                      graphid *start_vertex_id,
                                      graphid *end_vertex_id,
                                      int64 *edge_index,
                                      vertex_entry **start_vertex,
                                      vertex_entry **end_vertex,
                                      edge_entry **edge)
{
    EdgeTidHashEntry *entry = NULL;
    edge_entry *ee;
    bool found = false;

    entry = hash_search(ggctx->edge_tid_hashtable, tid, HASH_FIND, &found);
    if (!found)
    {
        return false;
    }

    ee = entry->edge;
    *edge_id = ee->edge_id;
    *label_table_oid = ee->edge_label_table_oid;
    *start_vertex_id = ee->start_vertex_id;
    *end_vertex_id = ee->end_vertex_id;
    *edge_index = ee->edge_index;
    if (start_vertex != NULL)
    {
        *start_vertex = ee->start_vertex;
    }
    if (end_vertex != NULL)
    {
        *end_vertex = ee->end_vertex;
    }
    if (edge != NULL)
    {
        *edge = ee;
    }

    return true;
}

/*
 * Helper function to find the GRAPH_global_context used by the specified
 * graph_oid. If not found, it returns NULL.
 */
GRAPH_global_context *find_GRAPH_global_context(Oid graph_oid)
{
    GRAPH_global_context *ggctx = NULL;

    /* lock the global contexts lists */
    pthread_mutex_lock(&global_graph_contexts_container.mutex_lock);

    /* get the root */
    ggctx = global_graph_contexts_container.contexts;

    while(ggctx != NULL)
    {
        /* if we found it return it */
        if (ggctx->graph_oid == graph_oid)
        {
            /* unlock the global contexts lists */
            pthread_mutex_unlock(&global_graph_contexts_container.mutex_lock);

            return ggctx;
        }

        /* advance to the next context */
        ggctx = ggctx->next;
    }

    /* unlock the global contexts lists */
    pthread_mutex_unlock(&global_graph_contexts_container.mutex_lock);

    /* we did not find it so return NULL */
    return NULL;
}

Oid get_GRAPH_global_context_oid(GRAPH_global_context *ggctx)
{
    Assert(ggctx != NULL);

    return ggctx->graph_oid;
}

bool is_GRAPH_global_context_current(GRAPH_global_context *ggctx)
{
    GRAPH_global_context *curr_ggctx = NULL;
    bool found = false;

    if (ggctx == NULL)
    {
        return false;
    }

    pthread_mutex_lock(&global_graph_contexts_container.mutex_lock);
    curr_ggctx = global_graph_contexts_container.contexts;
    while (curr_ggctx != NULL)
    {
        if (curr_ggctx == ggctx)
        {
            found = true;
            break;
        }
        curr_ggctx = curr_ggctx->next;
    }
    pthread_mutex_unlock(&global_graph_contexts_container.mutex_lock);

    return found && !is_ggctx_invalid(ggctx);
}

/* graph vertices accessor */
ListGraphId *get_graph_vertices(GRAPH_global_context *ggctx)
{
    return ggctx->vertices;
}

int64 get_graph_num_loaded_edges(GRAPH_global_context *ggctx)
{
    return ggctx->num_loaded_edges;
}

bool graph_global_context_has_edge_metadata(GRAPH_global_context *ggctx)
{
    return ggctx->edge_metadata_loaded;
}

bool graph_edge_labels_have_age_adjacency_indexes(
    Oid graph_oid, bool require_outgoing, bool require_incoming)
{
    Snapshot snapshot = GetActiveSnapshot();
    Relation ag_label;
    TupleDesc tupdesc;
    SysScanDesc scan_desc;
    ScanKeyData scan_key;
    HeapTuple tuple;
    bool saw_edge_label = false;
    bool covered = true;

    ag_label = table_open(ag_label_relation_id(), AccessShareLock);
    tupdesc = RelationGetDescr(ag_label);

    ScanKeyInit(&scan_key, Anum_ag_label_graph, BTEqualStrategyNumber,
                F_OIDEQ, ObjectIdGetDatum(graph_oid));
    scan_desc = systable_beginscan(ag_label, ag_label_graph_oid_index_id(),
                                   true, snapshot, 1, &scan_key);

    while ((tuple = systable_getnext(scan_desc)) != NULL)
    {
        Datum datum;
        bool is_null;
        Oid edge_label_oid;
        Oid out_index_oid;
        Oid in_index_oid;

        datum = heap_getattr(tuple, Anum_ag_label_kind, tupdesc, &is_null);
        if (is_null || DatumGetChar(datum) != LABEL_KIND_EDGE)
        {
            continue;
        }

        datum = heap_getattr(tuple, Anum_ag_label_relation, tupdesc,
                             &is_null);
        if (is_null)
        {
            covered = false;
            break;
        }

        edge_label_oid = DatumGetObjectId(datum);
        get_edge_label_age_adjacency_indexes(edge_label_oid, &out_index_oid,
                                             &in_index_oid);
        saw_edge_label = true;
        if ((require_outgoing && !OidIsValid(out_index_oid)) ||
            (require_incoming && !OidIsValid(in_index_oid)))
        {
            covered = false;
            break;
        }
    }

    systable_endscan(scan_desc);
    table_close(ag_label, AccessShareLock);

    return saw_edge_label && covered;
}

int32 get_graph_edge_label_id(GRAPH_global_context *ggctx,
                              Oid edge_label_oid)
{
    int i;

    Assert(ggctx != NULL);

    if (!OidIsValid(edge_label_oid))
    {
        return INVALID_LABEL_ID;
    }

    for (i = 0; i < ggctx->edge_labels.count; i++)
    {
        graph_label_entry *label = &ggctx->edge_labels.entries[i];

        if (label->relation == edge_label_oid)
        {
            return label->label_id;
        }
    }

    return INVALID_LABEL_ID;
}

int get_graph_edge_label_source_candidates(
    GRAPH_global_context *ggctx, GraphEdgeLabelSourceCandidate **candidates)
{
    GraphEdgeLabelSourceCandidate *result;
    int count = 0;
    int i;

    Assert(ggctx != NULL);
    Assert(candidates != NULL);

    *candidates = NULL;
    if (ggctx->edge_labels.count <= 0)
    {
        return 0;
    }

    result = palloc0(sizeof(*result) * ggctx->edge_labels.count);
    for (i = 0; i < ggctx->edge_labels.count; i++)
    {
        graph_label_entry *label;
        Oid out_index_oid;
        Oid in_index_oid;

        label = &ggctx->edge_labels.entries[i];
        if (IS_DEFAULT_LABEL_EDGE(NameStr(label->name)))
        {
            continue;
        }
        get_edge_label_age_adjacency_indexes(label->relation,
                                             &out_index_oid,
                                             &in_index_oid);
        if (!OidIsValid(out_index_oid) && !OidIsValid(in_index_oid))
        {
            continue;
        }

        result[count].label_name = NameStr(label->name);
        result[count].edge_label_oid = label->relation;
        result[count].label_id = label->label_id;
        result[count].age_adjacency_out_index_oid = out_index_oid;
        result[count].age_adjacency_in_index_oid = in_index_oid;
        count++;
    }

    if (count == 0)
    {
        pfree(result);
        return 0;
    }

    *candidates = result;
    return count;
}

/* vertex_entry accessor functions */
graphid get_vertex_entry_id(vertex_entry *ve)
{
    return ve->vertex_id;
}

GraphEdgeAdjList *get_vertex_entry_adj_edges_in(vertex_entry *ve)
{
    return ve->adj_edges_in;
}

GraphEdgeAdjList *get_vertex_entry_adj_edges_out(vertex_entry *ve)
{
    return ve->adj_edges_out;
}

GraphEdgeAdjList *get_vertex_entry_adj_edges_self(vertex_entry *ve)
{
    return ve->adj_edges_self;
}

GraphEdgeAdjList *get_vertex_entry_adj_edges_in_for_label(
    vertex_entry *ve, Oid edge_label_table_oid)
{
    return get_labeled_vertex_adj_edges(ve->edges_in_by_label,
                                        edge_label_table_oid);
}

GraphEdgeAdjList *get_vertex_entry_adj_edges_out_for_label(
    vertex_entry *ve, Oid edge_label_table_oid)
{
    return get_labeled_vertex_adj_edges(ve->edges_out_by_label,
                                        edge_label_table_oid);
}

GraphEdgeAdjList *get_vertex_entry_adj_edges_self_for_label(
    vertex_entry *ve, Oid edge_label_table_oid)
{
    return get_labeled_vertex_adj_edges(ve->edges_self_by_label,
                                        edge_label_table_oid);
}

Oid get_vertex_entry_label_table_oid(vertex_entry *ve)
{
    return ve->vertex_label_table_oid;
}

char *get_vertex_entry_label_name(vertex_entry *ve)
{
    return ve->vertex_label_name;
}

/*
 * Fetch vertex properties on demand from the heap via stored TID.
 *
 * Returns a datumCopy of the properties in the current memory context.
 * The caller does not need to free the result explicitly — it will be
 * freed when the memory context is reset (typically the SRF multi-call
 * context for VLE, which is cleaned up when the SRF completes).
 *
 * If the tuple is no longer visible (e.g., concurrent mutation between
 * cache build and fetch), the version counter should have invalidated
 * the cache. If we get here with a stale TID, it indicates a bug in
 * the invalidation logic.
 */
Datum get_vertex_entry_properties(vertex_entry *ve)
{
    return get_vertex_entry_properties_with_cache(ve, NULL);
}

Datum get_vertex_entry_properties_with_cache(vertex_entry *ve,
                                             HTAB *relation_cache)
{
    if (ve->cached_properties_valid)
    {
        return ve->cached_properties;
    }

    if (!ItemPointerIsValid(&ve->tid))
    {
        return fetch_vertex_entry_properties_by_id(ve, relation_cache);
    }

    return fetch_entry_properties(
        ve->vertex_label_table_oid, ve->tid, 2, relation_cache,
        "get_vertex_entry_properties: stale TID - "
        "vertex entry references a tuple that is no longer visible");
}

int64 prefetch_vertex_entry_properties_by_ids(
    GRAPH_global_context *ggctx, const graphid *vertex_ids,
    int64 nvertex_ids, HTAB **relation_cache, const char *cache_name)
{
    HTAB *needed_vertices;
    HASHCTL hash_ctl;
    Oid *relids;
    int64 *rel_candidate_counts;
    int64 nrelids = 0;
    int64 prefetch_needed = 0;
    int64 loaded = 0;
    int64 i;

    Assert(ggctx != NULL);
    Assert(vertex_ids != NULL || nvertex_ids == 0);
    Assert(relation_cache != NULL);

    if (nvertex_ids <= 0)
        return 0;

    memset(&hash_ctl, 0, sizeof(hash_ctl));
    hash_ctl.keysize = sizeof(graphid);
    hash_ctl.entrysize = sizeof(VertexPropertyPrefetchEntry);
    needed_vertices = hash_create("vertex property prefetch candidates",
                                  (long)nvertex_ids, &hash_ctl,
                                  HASH_ELEM | HASH_BLOBS);
    relids = palloc(sizeof(Oid) * nvertex_ids);
    rel_candidate_counts = palloc0(sizeof(int64) * nvertex_ids);

    for (i = 0; i < nvertex_ids; i++)
    {
        VertexPropertyPrefetchEntry *entry;
        vertex_entry *ve;
        graphid vertex_id = vertex_ids[i];
        bool found;
        bool relid_seen = false;
        int64 relidx;

        ve = get_vertex_entry(ggctx, vertex_id);
        if (ve == NULL || ve->cached_properties_valid ||
            !OidIsValid(ve->vertex_label_table_oid))
            continue;

        entry = hash_search(needed_vertices, &vertex_id, HASH_ENTER, &found);
        if (found)
            continue;

        entry->ve = ve;

        for (relidx = 0; relidx < nrelids; relidx++)
        {
            if (relids[relidx] == ve->vertex_label_table_oid)
            {
                relid_seen = true;
                rel_candidate_counts[relidx]++;
                break;
            }
        }
        if (!relid_seen)
        {
            relids[nrelids++] = ve->vertex_label_table_oid;
            rel_candidate_counts[nrelids - 1] = 1;
        }
    }

    for (i = 0; i < nrelids; i++)
    {
        if (rel_candidate_counts[i] >=
            AGE_VERTEX_PROPERTY_PREFETCH_MIN_REL_CANDIDATES)
        {
            prefetch_needed += rel_candidate_counts[i];
        }
    }

    if (prefetch_needed == 0)
    {
        hash_destroy(needed_vertices);
        pfree(rel_candidate_counts);
        pfree(relids);
        return 0;
    }

    if (*relation_cache == NULL)
    {
        *relation_cache = create_entry_property_relation_cache(cache_name);
    }

    for (i = 0; i < nrelids && loaded < prefetch_needed; i++)
    {
        Relation rel;
        TupleDesc tupdesc;
        TableScanDesc scan_desc;
        HeapTuple tuple;

        if (rel_candidate_counts[i] <
            AGE_VERTEX_PROPERTY_PREFETCH_MIN_REL_CANDIDATES)
        {
            continue;
        }

        rel = get_entry_property_relation(relids[i], *relation_cache);
        tupdesc = RelationGetDescr(rel);
        scan_desc = table_beginscan(rel, GetActiveSnapshot(), 0, NULL);
        while ((tuple = heap_getnext(scan_desc, ForwardScanDirection)) != NULL)
        {
            VertexPropertyPrefetchEntry *entry;
            graphid vertex_id;
            Datum vertex_id_datum;
            bool isnull;

            vertex_id_datum = heap_getattr(
                tuple, Anum_ag_label_vertex_table_id, tupdesc, &isnull);
            if (isnull)
                continue;

            vertex_id = DatumGetInt64(vertex_id_datum);
            entry = hash_search(needed_vertices, &vertex_id, HASH_FIND,
                                NULL);
            if (entry != NULL &&
                entry->ve->vertex_label_table_oid == relids[i] &&
                !entry->ve->cached_properties_valid)
            {
                Datum props;

                entry->ve->tid = tuple->t_self;
                props = heap_getattr(tuple,
                                     Anum_ag_label_vertex_table_properties,
                                     tupdesc, &isnull);
                if (!isnull)
                {
                    MemoryContext oldctx;

                    oldctx = MemoryContextSwitchTo(TopMemoryContext);
                    entry->ve->cached_properties = datumCopy(props, false, -1);
                    entry->ve->cached_properties_valid = true;
                    MemoryContextSwitchTo(oldctx);
                }
                loaded++;
                if (loaded >= prefetch_needed)
                    break;
            }
        }
        table_endscan(scan_desc);
    }

    hash_destroy(needed_vertices);
    pfree(rel_candidate_counts);
    pfree(relids);

    return loaded;
}

bool get_vertex_entry_cached_properties(vertex_entry *ve, Datum *properties)
{
    Assert(ve != NULL);
    Assert(properties != NULL);

    *properties = (Datum) 0;
    if (!ve->cached_properties_valid)
    {
        return false;
    }

    *properties = ve->cached_properties;
    return true;
}

Datum get_vertex_entry_properties_with_relation(vertex_entry *ve, Relation rel)
{
    Assert(RelationGetRelid(rel) == ve->vertex_label_table_oid);

    if (!ItemPointerIsValid(&ve->tid))
    {
        return fetch_vertex_entry_properties_by_id(ve, NULL);
    }

    return fetch_entry_properties_from_relation(
        rel, ve->tid, 2,
        "get_vertex_entry_properties: stale TID - "
        "vertex entry references a tuple that is no longer visible");
}

static Datum fetch_vertex_entry_properties_by_id(vertex_entry *ve,
                                                 HTAB *relation_cache)
{
    Relation rel;
    Oid index_oid;
    TableScanDesc scan_desc;
    HeapTuple tuple;
    TupleDesc tupdesc;
    Datum result = (Datum) 0;
    bool close_rel = false;

    if (!OidIsValid(ve->vertex_label_table_oid))
    {
        elog(ERROR, "vertex entry has no label relation for lazy fetch");
    }

    rel = get_entry_property_relation(ve->vertex_label_table_oid,
                                      relation_cache);
    close_rel = relation_cache == NULL;
    tupdesc = RelationGetDescr(rel);
    index_oid = find_usable_btree_index_for_attr(
        rel, Anum_ag_label_vertex_table_id);
    if (OidIsValid(index_oid))
    {
        Relation index_rel;
        IndexScanDesc index_scan;
        TupleTableSlot *slot;
        ScanKeyData scan_key;

        ScanKeyInit(&scan_key, Anum_ag_label_vertex_table_id,
                    BTEqualStrategyNumber, F_INT8EQ,
                    GRAPHID_GET_DATUM(ve->vertex_id));
        index_rel = index_open(index_oid, AccessShareLock);
        slot = table_slot_create(rel, NULL);
        index_scan = index_beginscan(rel, index_rel, GetActiveSnapshot(),
                                     NULL, 1, 0);
        index_rescan(index_scan, &scan_key, 1, NULL, 0);
        if (index_getnext_slot(index_scan, ForwardScanDirection, slot))
        {
            bool should_free;
            bool isnull;
            Datum props;

            tuple = ExecFetchSlotHeapTuple(slot, true, &should_free);
            ve->tid = tuple->t_self;
            props = heap_getattr(tuple, Anum_ag_label_vertex_table_properties,
                                 tupdesc, &isnull);
            if (!isnull)
            {
                MemoryContext oldctx;

                oldctx = MemoryContextSwitchTo(TopMemoryContext);
                result = datumCopy(props, false, -1);
                MemoryContextSwitchTo(oldctx);
                ve->cached_properties = result;
                ve->cached_properties_valid = true;
            }
            if (should_free)
            {
                heap_freetuple(tuple);
            }
        }
        index_endscan(index_scan);
        ExecDropSingleTupleTableSlot(slot);
        index_close(index_rel, AccessShareLock);
        if (result != (Datum) 0)
        {
            if (close_rel)
            {
                table_close(rel, AccessShareLock);
            }
            return result;
        }
    }

    scan_desc = table_beginscan(rel, GetActiveSnapshot(), 0, NULL);
    while ((tuple = heap_getnext(scan_desc, ForwardScanDirection)) != NULL)
    {
        graphid vertex_id;
        bool isnull;

        vertex_id = DatumGetInt64(heap_getattr(
            tuple, Anum_ag_label_vertex_table_id, tupdesc, &isnull));
        if (isnull || vertex_id != ve->vertex_id)
        {
            continue;
        }

        ve->tid = tuple->t_self;
        {
            bool props_isnull;
            Datum props;

            props = heap_getattr(tuple,
                                 Anum_ag_label_vertex_table_properties,
                                 tupdesc, &props_isnull);
            if (!props_isnull)
            {
                MemoryContext oldctx;

                oldctx = MemoryContextSwitchTo(TopMemoryContext);
                result = datumCopy(props, false, -1);
                MemoryContextSwitchTo(oldctx);
                ve->cached_properties = result;
                ve->cached_properties_valid = true;
            }
        }
        break;
    }
    table_endscan(scan_desc);
    if (close_rel)
    {
        table_close(rel, AccessShareLock);
    }

    if (result == (Datum) 0)
    {
        elog(ERROR, "vertex entry lazy fetch could not find vertex id "
             INT64_FORMAT, ve->vertex_id);
    }

    return result;
}

bool get_vertex_entry_cached_property(vertex_entry *ve, agtype_value *key,
                                      Datum *result)
{
    Assert(key != NULL);
    Assert(result != NULL);

    return get_vertex_entry_cached_property_str(
        ve, key->val.string.val, key->val.string.len, result);
}

bool get_vertex_entry_cached_property_str(vertex_entry *ve,
                                          const char *key, int key_len,
                                          Datum *result)
{
    Assert(key != NULL);
    Assert(result != NULL);

    *result = (Datum) 0;
    if (ve->cached_property_value_valid &&
        ve->cached_property_key_len == key_len &&
        (key_len == 1 ? ve->cached_property_key_char == key[0] :
         memcmp(ve->cached_property_key, key, key_len) == 0))
    {
        *result = ve->cached_property_value;
        return true;
    }

    return false;
}

bool get_vertex_entry_cached_property_char(vertex_entry *ve, char key,
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

bool get_vertex_entry_property_with_cache(vertex_entry *ve,
                                          HTAB *relation_cache,
                                          agtype_value *key, Datum *result)
{
    Relation rel;
    bool close_rel;

    if (get_vertex_entry_cached_property(ve, key, result))
    {
        return true;
    }
    if (!ItemPointerIsValid(&ve->tid))
    {
        (void) fetch_vertex_entry_properties_by_id(ve, relation_cache);
    }

    rel = get_entry_property_relation(ve->vertex_label_table_oid,
                                      relation_cache);
    close_rel = relation_cache == NULL;

    if (get_vertex_entry_property_with_relation_uncached(ve, rel, key, result))
    {
        if (close_rel)
            table_close(rel, AccessShareLock);
        return true;
    }

    if (close_rel)
        table_close(rel, AccessShareLock);
    return false;
}

bool get_vertex_entry_property_with_lazy_cache(vertex_entry *ve,
                                               HTAB **relation_cache,
                                               const char *cache_name,
                                               agtype_value *key,
                                               Datum *result)
{
    Relation rel;

    if (get_vertex_entry_cached_property(ve, key, result))
    {
        return true;
    }
    if (!ItemPointerIsValid(&ve->tid))
    {
        (void) fetch_vertex_entry_properties_by_id(ve, *relation_cache);
    }

    if (*relation_cache == NULL)
    {
        *relation_cache = create_entry_property_relation_cache(cache_name);
    }

    rel = get_entry_property_relation(ve->vertex_label_table_oid,
                                      *relation_cache);
    return get_vertex_entry_property_with_relation_uncached(ve, rel, key,
                                                           result);
}

bool get_vertex_entry_scalar_property_with_lazy_cache(vertex_entry *ve,
                                                      HTAB **relation_cache,
                                                      const char *cache_name,
                                                      agtype_value *key,
                                                      Datum *result)
{
    Relation rel;

    if (get_vertex_entry_cached_property(ve, key, result))
    {
        return true;
    }
    if (!ItemPointerIsValid(&ve->tid))
    {
        (void) fetch_vertex_entry_properties_by_id(ve, *relation_cache);
    }

    if (*relation_cache == NULL)
    {
        *relation_cache = create_entry_property_relation_cache(cache_name);
    }

    rel = get_entry_property_relation(ve->vertex_label_table_oid,
                                      *relation_cache);
    return get_vertex_entry_scalar_property_with_relation(ve, rel, key,
                                                         result);
}

bool cache_vertex_entry_tuple_scalar_property(
    GRAPH_global_context *ggctx, Oid relid, HeapTuple tuple,
    TupleDesc tupdesc, agtype_value *key)
{
    Datum vertex_id_datum;
    Datum props;
    graphid vertex_id;
    vertex_entry *ve;
    agtype *properties;
    agtype_value property_value;
    bool property_value_needs_free = false;
    bool found = false;
    bool isnull;

    Assert(ggctx != NULL);
    Assert(tuple != NULL);
    Assert(tupdesc != NULL);
    Assert(key != NULL);

    vertex_id_datum = heap_getattr(tuple, Anum_ag_label_vertex_table_id,
                                   tupdesc, &isnull);
    if (isnull)
        return false;

    vertex_id = DatumGetInt64(vertex_id_datum);
    ve = (vertex_entry *)hash_search(ggctx->vertex_hashtable,
                                     (void *)&vertex_id,
                                     HASH_FIND, &found);
    if (!found || ve->vertex_label_table_oid != relid)
        return false;

    props = heap_getattr(tuple, Anum_ag_label_vertex_table_properties,
                         tupdesc, &isnull);
    if (isnull)
        return false;

    ve->tid = tuple->t_self;
    if (!ve->cached_properties_valid)
    {
        MemoryContext oldctx;

        oldctx = MemoryContextSwitchTo(TopMemoryContext);
        ve->cached_properties = datumCopy(props, false, -1);
        ve->cached_properties_valid = true;
        MemoryContextSwitchTo(oldctx);
    }

    properties = DATUM_GET_AGTYPE_P(props);
    if (find_agtype_value_from_container_no_copy(&properties->root,
                                                 AGT_FOBJECT, key,
                                                 &property_value,
                                                 &property_value_needs_free) &&
        property_value.type != AGTV_NULL)
    {
        cache_vertex_entry_scalar_property(ve, key, &property_value);
    }
    if (property_value_needs_free)
        pfree_agtype_value_content(&property_value);

    return true;
}

BlockNumber get_vertex_entry_tid_block(vertex_entry *ve)
{
    return ItemPointerGetBlockNumber(&ve->tid);
}

int64 prefetch_vertex_entry_block_scalar_property_cache(
    GRAPH_global_context *ggctx, vertex_entry *ve, HTAB **relation_cache,
    const char *cache_name, agtype_value *key, int64 max_cached,
    Datum *target_result, bool *target_found)
{
    Relation rel;

    Assert(ggctx != NULL);
    Assert(ve != NULL);
    Assert(key != NULL);

    if (!ItemPointerIsValid(&ve->tid))
    {
        (void) fetch_vertex_entry_properties_by_id(ve, *relation_cache);
    }

    if (*relation_cache == NULL)
    {
        *relation_cache = create_entry_property_relation_cache(cache_name);
    }

    rel = get_entry_property_relation(ve->vertex_label_table_oid,
                                      *relation_cache);

    if (target_result != NULL)
    {
        *target_result = (Datum) 0;
    }
    if (target_found != NULL)
    {
        *target_found = false;
    }

    return prefetch_vertex_block_scalar_property_cache(
        ggctx, rel, ItemPointerGetBlockNumber(&ve->tid), ve->vertex_id,
        key, max_cached, target_result, target_found);
}

bool get_vertex_entry_property_with_relation(vertex_entry *ve, Relation rel,
                                             agtype_value *key, Datum *result)
{
    Assert(RelationGetRelid(rel) == ve->vertex_label_table_oid);

    if (get_vertex_entry_cached_property(ve, key, result))
    {
        return true;
    }
    if (!ItemPointerIsValid(&ve->tid))
    {
        (void) fetch_vertex_entry_properties_by_id(ve, NULL);
    }

    return get_vertex_entry_property_with_relation_uncached(ve, rel, key,
                                                           result);
}

static bool get_vertex_entry_property_with_relation_uncached(vertex_entry *ve,
                                                             Relation rel,
                                                             agtype_value *key,
                                                             Datum *result)
{
    HeapTupleData tuple;
    Buffer buffer;
    bool tuple_found;
    bool property_found = false;
    agtype *properties = NULL;

    Assert(RelationGetRelid(rel) == ve->vertex_label_table_oid);
    Assert(key != NULL);
    Assert(result != NULL);

    *result = (Datum) 0;

    if (ve->cached_properties_valid)
    {
        properties = DATUM_GET_AGTYPE_P(ve->cached_properties);
    }
    else
    {
        tuple.t_self = ve->tid;
        tuple_found = heap_fetch(rel, GetActiveSnapshot(), &tuple, &buffer,
                                 true);
        if (tuple_found)
        {
            TupleDesc tupdesc = RelationGetDescr(rel);
            bool isnull;
            Datum props;

            props = heap_getattr(&tuple, 2, tupdesc, &isnull);
            if (!isnull)
            {
                MemoryContext oldctx;

                oldctx = MemoryContextSwitchTo(TopMemoryContext);
                ve->cached_properties = datumCopy(props, false, -1);
                ve->cached_properties_valid = true;
                MemoryContextSwitchTo(oldctx);

                properties = DATUM_GET_AGTYPE_P(ve->cached_properties);
            }

            ReleaseBuffer(buffer);
        }

        if (!tuple_found)
        {
            elog(ERROR, "%s",
                 "get_vertex_entry_property: stale TID - "
                 "vertex entry references a tuple that is no longer visible");
        }
    }

    if (properties != NULL)
    {
        agtype_value property_value;
        bool property_value_needs_free = false;

        if (find_agtype_value_from_container_no_copy(&properties->root,
                                                     AGT_FOBJECT, key,
                                                     &property_value,
                                                     &property_value_needs_free) &&
            property_value.type != AGTV_NULL)
        {
            cache_vertex_entry_scalar_property(ve, key, &property_value);
            *result = ve->cached_property_value;
            property_found = true;
        }
        if (property_value_needs_free)
        {
            pfree_agtype_value_content(&property_value);
        }
    }

    return property_found;
}

static bool get_vertex_entry_scalar_property_with_relation(vertex_entry *ve,
                                                           Relation rel,
                                                           agtype_value *key,
                                                           Datum *result)
{
    HeapTupleData tuple;
    Buffer buffer;
    bool tuple_found;
    bool property_found = false;
    agtype *properties = NULL;

    Assert(RelationGetRelid(rel) == ve->vertex_label_table_oid);
    Assert(key != NULL);
    Assert(result != NULL);

    *result = (Datum) 0;

    if (ve->cached_properties_valid)
    {
        properties = DATUM_GET_AGTYPE_P(ve->cached_properties);
    }
    else
    {
        tuple.t_self = ve->tid;
        tuple_found = heap_fetch(rel, GetActiveSnapshot(), &tuple, &buffer,
                                 true);
        if (tuple_found)
        {
            TupleDesc tupdesc = RelationGetDescr(rel);
            bool isnull;
            Datum props;

            props = heap_getattr(&tuple, 2, tupdesc, &isnull);
            if (!isnull)
            {
                properties = DATUM_GET_AGTYPE_P(props);
            }

            ReleaseBuffer(buffer);
        }

        if (!tuple_found)
        {
            elog(ERROR, "%s",
                 "get_vertex_entry_property: stale TID - "
                 "vertex entry references a tuple that is no longer visible");
        }
    }

    if (properties != NULL)
    {
        agtype_value property_value;
        bool property_value_needs_free = false;

        if (find_agtype_value_from_container_no_copy(&properties->root,
                                                     AGT_FOBJECT, key,
                                                     &property_value,
                                                     &property_value_needs_free) &&
            property_value.type != AGTV_NULL)
        {
            cache_vertex_entry_scalar_property(ve, key, &property_value);
            *result = ve->cached_property_value;
            property_found = true;
        }
        if (property_value_needs_free)
        {
            pfree_agtype_value_content(&property_value);
        }
    }

    return property_found;
}

static void cache_vertex_entry_scalar_property(vertex_entry *ve,
                                               agtype_value *key,
                                               agtype_value *property_value)
{
    MemoryContext oldctx;

    oldctx = MemoryContextSwitchTo(TopMemoryContext);
    pfree_if_not_null(ve->cached_property_key);
    pfree_if_not_null(DatumGetPointer(ve->cached_property_value));
    ve->cached_property_key_len = key->val.string.len;
    if (key->val.string.len == 1)
    {
        ve->cached_property_key = NULL;
        ve->cached_property_key_char = key->val.string.val[0];
    }
    else
    {
        ve->cached_property_key = pnstrdup(key->val.string.val,
                                           key->val.string.len);
        ve->cached_property_key_char = '\0';
    }
    if (property_value->type == AGTV_INTEGER)
    {
        ve->cached_property_value = PointerGetDatum(
            agtype_integer_to_agtype(property_value->val.int_value));
    }
    else
    {
        ve->cached_property_value = PointerGetDatum(
            agtype_value_to_agtype(property_value));
    }
    ve->cached_property_value_valid = true;
    MemoryContextSwitchTo(oldctx);
}

static void cache_edge_entry_scalar_property(edge_entry *ee,
                                             agtype_value *key,
                                             agtype_value *property_value)
{
    MemoryContext oldctx;

    oldctx = MemoryContextSwitchTo(TopMemoryContext);
    pfree_if_not_null(ee->cached_property_key);
    pfree_if_not_null(DatumGetPointer(ee->cached_property_value));
    ee->cached_property_key_len = key->val.string.len;
    if (key->val.string.len == 1)
    {
        ee->cached_property_key = NULL;
        ee->cached_property_key_char = key->val.string.val[0];
    }
    else
    {
        ee->cached_property_key = pnstrdup(key->val.string.val,
                                           key->val.string.len);
        ee->cached_property_key_char = '\0';
    }
    if (property_value->type == AGTV_INTEGER)
    {
        ee->cached_property_value = PointerGetDatum(
            agtype_integer_to_agtype(property_value->val.int_value));
    }
    else
    {
        ee->cached_property_value = PointerGetDatum(
            agtype_value_to_agtype(property_value));
    }
    ee->cached_property_value_valid = true;
    MemoryContextSwitchTo(oldctx);
}

static int64 prefetch_vertex_block_scalar_property_cache(
    GRAPH_global_context *ggctx, Relation rel, BlockNumber blockno,
    graphid target_vertex_id, agtype_value *key, int64 max_cached,
    Datum *target_result, bool *target_found)
{
    Buffer buffer;
    Page page;
    OffsetNumber maxoff;
    OffsetNumber offnum;
    TupleDesc tupdesc;
    Snapshot snapshot;
    int64 cached = 0;

    Assert(ggctx != NULL);
    Assert(rel != NULL);
    Assert(key != NULL);

    snapshot = GetActiveSnapshot();
    tupdesc = RelationGetDescr(rel);

    buffer = ReadBuffer(rel, blockno);
    LockBuffer(buffer, BUFFER_LOCK_SHARE);
    page = BufferGetPage(buffer);
    maxoff = PageGetMaxOffsetNumber(page);

    for (offnum = FirstOffsetNumber;
         offnum <= maxoff;
         offnum = OffsetNumberNext(offnum))
    {
        ItemId itemid;
        HeapTupleData tuple;
        bool isnull;
        Datum vertex_id_datum;
        Datum props;
        graphid vertex_id;
        vertex_entry *block_ve;
        bool found = false;
        agtype *properties;
        agtype_value property_value;
        bool property_value_needs_free = false;

        itemid = PageGetItemId(page, offnum);
        if (!ItemIdIsNormal(itemid))
        {
            continue;
        }

        ItemPointerSet(&tuple.t_self, blockno, offnum);
        tuple.t_tableOid = RelationGetRelid(rel);
        tuple.t_data = (HeapTupleHeader) PageGetItem(page, itemid);
        tuple.t_len = ItemIdGetLength(itemid);

        if (!HeapTupleSatisfiesVisibility(&tuple, snapshot, buffer))
        {
            continue;
        }

        vertex_id_datum = heap_getattr(&tuple, Anum_ag_label_vertex_table_id,
                                       tupdesc, &isnull);
        if (isnull)
        {
            continue;
        }

        vertex_id = DatumGetInt64(vertex_id_datum);
        block_ve = (vertex_entry *)hash_search(ggctx->vertex_hashtable,
                                               (void *)&vertex_id,
                                               HASH_FIND, &found);
        if (!found || block_ve->vertex_label_table_oid != RelationGetRelid(rel))
        {
            continue;
        }

        props = heap_getattr(&tuple, Anum_ag_label_vertex_table_properties,
                             tupdesc, &isnull);
        if (isnull)
        {
            continue;
        }

        properties = DATUM_GET_AGTYPE_P(props);
        if (find_agtype_value_from_container_no_copy(&properties->root,
                                                     AGT_FOBJECT, key,
                                                     &property_value,
                                                     &property_value_needs_free) &&
            property_value.type != AGTV_NULL)
        {
            cache_vertex_entry_scalar_property(block_ve, key,
                                               &property_value);
            if (vertex_id == target_vertex_id && target_result != NULL &&
                target_found != NULL)
            {
                *target_result = block_ve->cached_property_value;
                *target_found = true;
            }
            cached++;
            if (max_cached >= 0 && cached >= max_cached)
            {
                break;
            }
        }
        if (property_value_needs_free)
        {
            pfree_agtype_value_content(&property_value);
        }
    }

    LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
    ReleaseBuffer(buffer);

    return cached;
}

static Datum fetch_entry_properties(Oid relid, ItemPointerData tid,
                                    AttrNumber property_attr,
                                    HTAB *relation_cache,
                                    const char *stale_msg)
{
    Relation rel;
    Datum result = (Datum) 0;
    bool close_rel = false;

    rel = get_entry_property_relation(relid, relation_cache);
    close_rel = relation_cache == NULL;

    result = fetch_entry_properties_from_relation(rel, tid, property_attr,
                                                  stale_msg);

    if (close_rel)
    {
        table_close(rel, AccessShareLock);
    }

    return result;
}

static Datum fetch_entry_properties_from_relation(Relation rel,
                                                  ItemPointerData tid,
                                                  AttrNumber property_attr,
                                                  const char *stale_msg)
{
    HeapTupleData tuple;
    Buffer buffer;
    Datum result = (Datum) 0;

    tuple.t_self = tid;

    if (heap_fetch(rel, GetActiveSnapshot(), &tuple, &buffer, true))
    {
        TupleDesc tupdesc = RelationGetDescr(rel);
        bool isnull;
        Datum props;

        props = heap_getattr(&tuple, property_attr, tupdesc, &isnull);
        if (!isnull)
        {
            result = datumCopy(props, false, -1);
        }

        ReleaseBuffer(buffer);
    }

    /*
     * If heap_fetch failed, the tuple is no longer visible. This should
     * not happen under normal operation because the version counter
     * invalidates the cache when the graph is mutated.
     */
    if (result == (Datum) 0)
    {
        elog(ERROR, "%s", stale_msg);
    }

    return result;
}

/* edge_entry accessor functions */
graphid get_edge_entry_id(edge_entry *ee)
{
    return ee->edge_id;
}

Oid get_edge_entry_label_table_oid(edge_entry *ee)
{
    return ee->edge_label_table_oid;
}

char *get_edge_entry_label_name(edge_entry *ee)
{
    return ee->edge_label_name;
}

/*
 * Fetch edge properties on demand from the heap via stored TID.
 * See get_vertex_entry_properties for memory and safety notes.
 */
Datum get_edge_entry_properties(edge_entry *ee)
{
    return get_edge_entry_properties_with_cache(ee, NULL);
}

Datum get_edge_entry_properties_with_cache(edge_entry *ee,
                                           HTAB *relation_cache)
{
    return fetch_entry_properties(
        ee->edge_label_table_oid, ee->tid, 4, relation_cache,
        "get_edge_entry_properties: stale TID - "
        "edge entry references a tuple that is no longer visible");
}

bool get_edge_entry_cached_property(edge_entry *ee, agtype_value *key,
                                    Datum *result)
{
    Assert(key != NULL);
    Assert(result != NULL);

    *result = (Datum) 0;
    if (ee->cached_property_value_valid &&
        ee->cached_property_key_len == key->val.string.len &&
        (key->val.string.len == 1 ?
         ee->cached_property_key_char == key->val.string.val[0] :
         memcmp(ee->cached_property_key, key->val.string.val,
                key->val.string.len) == 0))
    {
        *result = ee->cached_property_value;
        return true;
    }

    return false;
}

bool get_edge_entry_property_with_cache(edge_entry *ee,
                                        HTAB *relation_cache,
                                        agtype_value *key, Datum *result)
{
    Relation rel;
    bool close_rel;

    Assert(key != NULL);
    Assert(result != NULL);

    if (get_edge_entry_cached_property(ee, key, result))
    {
        return true;
    }

    rel = get_entry_property_relation(ee->edge_label_table_oid,
                                      relation_cache);
    close_rel = relation_cache == NULL;
    if (get_edge_entry_property_with_relation_uncached(ee, rel, key, result))
    {
        if (close_rel)
        {
            table_close(rel, AccessShareLock);
        }
        return true;
    }

    if (close_rel)
    {
        table_close(rel, AccessShareLock);
    }

    return false;
}

bool get_edge_entry_property_with_relation(edge_entry *ee, Relation rel,
                                           agtype_value *key, Datum *result)
{
    Assert(RelationGetRelid(rel) == ee->edge_label_table_oid);

    if (get_edge_entry_cached_property(ee, key, result))
    {
        return true;
    }

    return get_edge_entry_property_with_relation_uncached(ee, rel, key,
                                                         result);
}

static bool get_edge_entry_property_with_relation_uncached(edge_entry *ee,
                                                           Relation rel,
                                                           agtype_value *key,
                                                           Datum *result)
{
    agtype *properties;
    agtype_value property_value;
    bool property_value_needs_free = false;
    bool found;
    bool property_found = false;

    Assert(RelationGetRelid(rel) == ee->edge_label_table_oid);
    Assert(key != NULL);
    Assert(result != NULL);

    *result = (Datum) 0;
    properties = DATUM_GET_AGTYPE_P(
        fetch_entry_properties_from_relation(
            rel, ee->tid, 4,
            "get_edge_entry_property: stale TID - "
            "edge entry references a tuple that is no longer visible"));

    found = find_agtype_value_from_container_no_copy(
        &properties->root, AGT_FOBJECT, key, &property_value,
        &property_value_needs_free);
    if (found && property_value.type != AGTV_NULL)
    {
        cache_edge_entry_scalar_property(ee, key, &property_value);
        *result = ee->cached_property_value;
        property_found = true;
    }
    if (property_value_needs_free)
    {
        pfree_agtype_value_content(&property_value);
    }

    return property_found;
}

HTAB *create_entry_property_relation_cache(const char *name)
{
    HASHCTL hash_ctl;

    MemSet(&hash_ctl, 0, sizeof(hash_ctl));
    hash_ctl.keysize = sizeof(Oid);
    hash_ctl.entrysize = sizeof(EntryPropertyRelationCacheEntry);
    hash_ctl.hcxt = CurrentMemoryContext;

    return hash_create(name, 8, &hash_ctl,
                       HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
}

void destroy_entry_property_relation_cache(HTAB *relation_cache)
{
    HASH_SEQ_STATUS hash_seq;
    EntryPropertyRelationCacheEntry *entry;

    if (relation_cache == NULL)
    {
        return;
    }

    hash_seq_init(&hash_seq, relation_cache);
    while ((entry = hash_seq_search(&hash_seq)) != NULL)
    {
        if (entry->rel != NULL)
        {
            table_close(entry->rel, AccessShareLock);
        }
    }

    hash_destroy(relation_cache);
}

static Relation get_entry_property_relation(Oid relid, HTAB *relation_cache)
{
    EntryPropertyRelationCacheEntry *entry;
    bool found;

    if (relation_cache == NULL)
    {
        return table_open(relid, AccessShareLock);
    }

    entry = hash_search(relation_cache, &relid, HASH_ENTER, &found);
    if (!found)
    {
        entry->relid = relid;
        entry->rel = table_open(relid, AccessShareLock);
    }

    return entry->rel;
}

graphid get_edge_entry_start_vertex_id(edge_entry *ee)
{
    return ee->start_vertex_id;
}

graphid get_edge_entry_end_vertex_id(edge_entry *ee)
{
    return ee->end_vertex_id;
}

int64 get_edge_entry_index(edge_entry *ee)
{
    return ee->edge_index;
}

void get_edge_entry_vle_fields(edge_entry *ee, graphid *edge_id,
                               Oid *label_table_oid,
                               graphid *start_vertex_id,
                               graphid *end_vertex_id, int64 *edge_index)
{
    *edge_id = ee->edge_id;
    *label_table_oid = ee->edge_label_table_oid;
    *start_vertex_id = ee->start_vertex_id;
    *end_vertex_id = ee->end_vertex_id;
    *edge_index = ee->edge_index;
}

int get_edge_entry_property_count(edge_entry *ee)
{
    return ee->edge_property_count;
}

int get_edge_entry_property_size(edge_entry *ee)
{
    return ee->edge_property_size;
}

uint32 get_edge_entry_property_hash(edge_entry *ee)
{
    return ee->edge_property_hash;
}

/* PostgreSQL SQL facing functions */

/* PG wrapper function for age_delete_global_graphs */
PG_FUNCTION_INFO_V1(age_delete_global_graphs);

Datum age_delete_global_graphs(PG_FUNCTION_ARGS)
{
    agtype_value agtv_temp;
    agtype_value *agtv_temp_ptr = NULL;
    bool agtv_temp_needs_free = false;
    bool success = false;

    /* get the graph name if supplied */
    if (!PG_ARGISNULL(0))
    {
        get_global_graph_scalar_arg_no_copy("delete_global_graphs",
                                            AG_GET_ARG_AGTYPE_P(0),
                                            AGTV_STRING, false,
                                            &agtv_temp,
                                            &agtv_temp_needs_free);
        agtv_temp_ptr = &agtv_temp;
    }

    if (agtv_temp_ptr == NULL || agtv_temp_ptr->type == AGTV_NULL)
    {
        success = delete_GRAPH_global_contexts();
    }
    else if (agtv_temp_ptr->type == AGTV_STRING)
    {
        success = delete_specific_GRAPH_global_contexts_len(
            agtv_temp_ptr->val.string.val, agtv_temp_ptr->val.string.len);
    }
    else
    {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("delete_global_graphs: invalid graph name type")));
    }

    if (agtv_temp_needs_free)
    {
        pfree_agtype_value_content(&agtv_temp);
    }

    PG_RETURN_BOOL(success);
}

/* PG wrapper function for age_vertex_degree */
PG_FUNCTION_INFO_V1(age_vertex_stats);

Datum age_vertex_stats(PG_FUNCTION_ARGS)
{
    GRAPH_global_context *ggctx = NULL;
    vertex_entry *ve = NULL;
    GraphEdgeAdjList *edges = NULL;
    agtype_value agtv_vertex;
    agtype_value *agtv_temp = NULL;
    agtype_value graph_name_value;
    bool graph_name_needs_free = false;
    bool vertex_needs_free = false;
    agtype_value agtv_integer;
    agtype_in_state result;
    agtype *agt_result;
    Oid graph_oid = InvalidOid;
    graphid vid = 0;
    int64 self_loops = 0;
    int64 degree = 0;

    /* the graph name is required, but this generally isn't user supplied */
    if (PG_ARGISNULL(0))
    {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("vertex_stats: graph name cannot be NULL")));
    }

    /* get the graph name */
    get_global_graph_scalar_arg_no_copy("vertex_stats", AG_GET_ARG_AGTYPE_P(0),
                                        AGTV_STRING, true, &graph_name_value,
                                        &graph_name_needs_free);

    /* we need the vertex */
    if (PG_ARGISNULL(1))
    {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("vertex_stats: vertex cannot be NULL")));
    }

    /* get the vertex */
    get_global_graph_scalar_arg_no_copy("vertex_stats", AG_GET_ARG_AGTYPE_P(1),
                                        AGTV_VERTEX, true, &agtv_vertex,
                                        &vertex_needs_free);

    /* get the graph oid */
    graph_oid = get_cached_global_graph_oid_len(graph_name_value.val.string.val,
                                                graph_name_value.val.string.len);

    /*
     * Create or retrieve the GRAPH global context for this graph. This function
     * will also purge off invalidated contexts.
     */
    ggctx = manage_GRAPH_global_contexts_len(graph_name_value.val.string.val,
                                             graph_name_value.val.string.len,
                                             graph_oid);

    /* get the id */
    agtv_temp = AGTYPE_VERTEX_GET_ID(&agtv_vertex);
    vid = agtv_temp->val.int_value;

    /* get the vertex entry */
    ve = get_vertex_entry(ggctx, vid);

    /* zero the state */
    memset(&result, 0, sizeof(agtype_in_state));

    /* start the object */
    result.res = push_agtype_value(&result.parse_state, WAGT_BEGIN_OBJECT,
                                   NULL);
    /* store the id */
    result.res = push_agtype_value(&result.parse_state, WAGT_KEY,
                                   string_to_agtype_value("id"));
    result.res = push_agtype_value(&result.parse_state, WAGT_VALUE, agtv_temp);

    /* store the label */
    agtv_temp = AGTYPE_VERTEX_GET_LABEL(&agtv_vertex);
    result.res = push_agtype_value(&result.parse_state, WAGT_KEY,
                                   string_to_agtype_value("label"));
    result.res = push_agtype_value(&result.parse_state, WAGT_VALUE, agtv_temp);

    /* set up an integer for returning values */
    agtv_temp = &agtv_integer;
    agtv_temp->type = AGTV_INTEGER;
    agtv_temp->val.int_value = 0;

    /* get and store the self_loops */
    edges = get_vertex_entry_adj_edges_self(ve);
    self_loops = (edges != NULL) ? edges->size : 0;
    agtv_temp->val.int_value = self_loops;
    result.res = push_agtype_value(&result.parse_state, WAGT_KEY,
                                   string_to_agtype_value("self_loops"));
    result.res = push_agtype_value(&result.parse_state, WAGT_VALUE, agtv_temp);

    /* get and store the in_degree */
    edges = get_vertex_entry_adj_edges_in(ve);
    degree = (edges != NULL) ? edges->size : 0;
    agtv_temp->val.int_value = degree + self_loops;
    result.res = push_agtype_value(&result.parse_state, WAGT_KEY,
                                   string_to_agtype_value("in_degree"));
    result.res = push_agtype_value(&result.parse_state, WAGT_VALUE, agtv_temp);

    /* get and store the out_degree */
    edges = get_vertex_entry_adj_edges_out(ve);
    degree = (edges != NULL) ? edges->size : 0;
    agtv_temp->val.int_value = degree + self_loops;
    result.res = push_agtype_value(&result.parse_state, WAGT_KEY,
                                   string_to_agtype_value("out_degree"));
    result.res = push_agtype_value(&result.parse_state, WAGT_VALUE, agtv_temp);

    /* close the object */
    result.res = push_agtype_value(&result.parse_state, WAGT_END_OBJECT, NULL);

    result.res->type = AGTV_OBJECT;

    agt_result = agtype_value_to_agtype(result.res);
    if (graph_name_needs_free)
    {
        pfree_agtype_value_content(&graph_name_value);
    }
    if (vertex_needs_free)
    {
        pfree_agtype_value_content(&agtv_vertex);
    }

    PG_RETURN_POINTER(agt_result);
}

/* PG wrapper function for age_graph_stats */
PG_FUNCTION_INFO_V1(age_graph_stats);

Datum age_graph_stats(PG_FUNCTION_ARGS)
{
    GRAPH_global_context *ggctx = NULL;
    agtype_value *agtv_temp = NULL;
    agtype_value graph_name_value;
    bool graph_name_needs_free = false;
    agtype_value agtv_integer;
    agtype_in_state result;
    agtype *agt_result;
    Oid graph_oid = InvalidOid;

    /* the graph name is required, but this generally isn't user supplied */
    if (PG_ARGISNULL(0))
    {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("graph_stats: graph name cannot be NULL")));
    }

    /* get the graph name */
    get_global_graph_scalar_arg_no_copy("graph_stats", AG_GET_ARG_AGTYPE_P(0),
                                        AGTV_STRING, true, &graph_name_value,
                                        &graph_name_needs_free);

    /* get the graph oid */
    graph_oid = get_cached_global_graph_oid_len(graph_name_value.val.string.val,
                                                graph_name_value.val.string.len);

    /*
     * Remove any context for this graph. This is done to allow graph_stats to
     * show any load issues.
     */
    delete_specific_GRAPH_global_context_by_oid(graph_oid);

    /*
     * Create or retrieve the GRAPH global context for this graph. This function
     * will also purge off invalidated contexts.
     */
    ggctx = manage_GRAPH_global_contexts_len(graph_name_value.val.string.val,
                                             graph_name_value.val.string.len,
                                             graph_oid);

    /* zero the state */
    memset(&result, 0, sizeof(agtype_in_state));

    /* start the object */
    result.res = push_agtype_value(&result.parse_state, WAGT_BEGIN_OBJECT,
                                   NULL);
    /* store the graph name */
    result.res = push_agtype_value(&result.parse_state, WAGT_KEY,
                                   string_to_agtype_value("graph"));
    result.res = push_agtype_value(&result.parse_state, WAGT_VALUE,
                                   &graph_name_value);

    /* set up an integer for returning values */
    agtv_temp = &agtv_integer;
    agtv_temp->type = AGTV_INTEGER;
    agtv_temp->val.int_value = 0;

    /* get and store num_loaded_vertices */
    agtv_temp->val.int_value = ggctx->num_loaded_vertices;
    result.res = push_agtype_value(&result.parse_state, WAGT_KEY,
                                   string_to_agtype_value("num_loaded_vertices"));
    result.res = push_agtype_value(&result.parse_state, WAGT_VALUE, agtv_temp);

    /* get and store num_loaded_edges */
    agtv_temp->val.int_value = ggctx->num_loaded_edges;
    result.res = push_agtype_value(&result.parse_state, WAGT_KEY,
                                   string_to_agtype_value("num_loaded_edges"));
    result.res = push_agtype_value(&result.parse_state, WAGT_VALUE, agtv_temp);

    /* close the object */
    result.res = push_agtype_value(&result.parse_state, WAGT_END_OBJECT, NULL);

    result.res->type = AGTV_OBJECT;

    agt_result = agtype_value_to_agtype(result.res);
    if (graph_name_needs_free)
    {
        pfree_agtype_value_content(&graph_name_value);
    }

    PG_RETURN_POINTER(agt_result);
}

/*
 * ============================================================================
 * Graph Version Counter Implementation
 *
 * Provides per-graph monotonic version counters in shared memory for
 * cross-backend VLE cache invalidation. Three modes are supported:
 *
 * DSM (PG 17+):  Uses GetNamedDSMSegment — works without shared_preload_libs
 * SHMEM (PG <17): Uses shmem_request/startup hooks — needs shared_preload_libs
 * SNAPSHOT:       Falls back to original snapshot-based invalidation
 * ============================================================================
 */

#if PG_VERSION_NUM >= 170000
/*
 * DSM path: GetNamedDSMSegment init callback.
 * Called once when the DSM segment is first created.
 */
static void age_dsm_init_callback(void *ptr)
{
    GraphVersionState *state = (GraphVersionState *) ptr;

    LWLockInitialize(&state->lock,
                     LWLockNewTrancheId());
    LWLockRegisterTranche(state->lock.tranche, "age_graph_version");
    state->num_entries = 0;
    memset(state->entries, 0, sizeof(state->entries));
}

/*
 * Get the shared GraphVersionState via DSM registry.
 * The segment is created on first access and persists until server shutdown.
 */
static GraphVersionState *get_version_state_dsm(void)
{
    bool found;

    return (GraphVersionState *)
        GetNamedDSMSegment("age_graph_versions",
                           sizeof(GraphVersionState),
                           age_dsm_init_callback,
                           &found);
}
#endif /* PG_VERSION_NUM >= 170000 */

/*
 * SHMEM path: request and startup hooks for PG < 17.
 * These are registered in _PG_init when shared_preload_libraries is used.
 * On PG 17+, DSM is used instead and these functions are not called.
 */
#if PG_VERSION_NUM < 170000
void age_graph_version_shmem_request(void)
{
    RequestAddinShmemSpace(MAXALIGN(sizeof(GraphVersionState)));
}

void age_graph_version_shmem_startup(void)
{
    bool found;

    LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);

    shmem_version_state =
        (GraphVersionState *) ShmemInitStruct("AGE Graph Version State",
                                              sizeof(GraphVersionState),
                                              &found);
    if (!found)
    {
        LWLockInitialize(&shmem_version_state->lock,
                         LWLockNewTrancheId());
        LWLockRegisterTranche(shmem_version_state->lock.tranche,
                              "age_graph_version");
        shmem_version_state->num_entries = 0;
        memset(shmem_version_state->entries, 0,
               sizeof(shmem_version_state->entries));
    }

    LWLockRelease(AddinShmemInitLock);
}
#endif /* PG_VERSION_NUM < 170000 */

/*
 * Detect which version mode to use. Called once per backend on first access.
 * Emits a DEBUG1 log message indicating the chosen mode.
 */
static void detect_version_mode(void)
{
#if PG_VERSION_NUM >= 170000
    version_mode = VERSION_MODE_DSM;
    elog(DEBUG1, "AGE: VLE cache using DSM version counter");
#else
    if (shmem_version_state != NULL)
    {
        version_mode = VERSION_MODE_SHMEM;
        elog(DEBUG1, "AGE: VLE cache using SHMEM version counter");
    }
    else
    {
        version_mode = VERSION_MODE_SNAPSHOT;
        elog(DEBUG1, "AGE: VLE cache using snapshot-based invalidation "
             "(add AGE to shared_preload_libraries for better caching)");
    }
#endif
}

/*
 * Get a pointer to the GraphVersionState, regardless of mode.
 * Returns NULL only in SNAPSHOT mode (no shared memory available).
 */
static GraphVersionState *get_version_state(void)
{
    if (version_mode == VERSION_MODE_UNKNOWN)
    {
        detect_version_mode();
    }

#if PG_VERSION_NUM >= 170000
    if (version_mode == VERSION_MODE_DSM)
    {
        return get_version_state_dsm();
    }
#endif

    if (version_mode == VERSION_MODE_SHMEM)
    {
        return shmem_version_state;
    }

    return NULL;
}

/*
 * Get the current version counter for a graph.
 * Returns 0 if the graph has never been tracked or if shared memory
 * is not available. Lock-free read via pg_atomic_read_u64.
 */
uint64 get_graph_version(Oid graph_oid)
{
    GraphVersionState *state = get_version_state();
    int i;

    if (state == NULL)
    {
        return 0;
    }

    if (cached_version_graph_oid == graph_oid &&
        cached_version_graph_index >= 0 &&
        cached_version_graph_index < state->num_entries &&
        state->entries[cached_version_graph_index].graph_oid == graph_oid)
    {
        return pg_atomic_read_u64(
            &state->entries[cached_version_graph_index].version);
    }

    /* lock-free scan of the array */
    for (i = 0; i < state->num_entries; i++)
    {
        if (state->entries[i].graph_oid == graph_oid)
        {
            cached_version_graph_oid = graph_oid;
            cached_version_graph_index = i;

            return pg_atomic_read_u64(&state->entries[i].version);
        }
    }

    cached_version_graph_oid = InvalidOid;
    cached_version_graph_index = -1;

    return 0;
}

/*
 * Increment the version counter for a graph.
 * Called after any graph mutation (Cypher or SQL trigger).
 * Lock-free for existing entries; acquires LWLock only to allocate new slots.
 */
void increment_graph_version(Oid graph_oid)
{
    GraphVersionState *state = get_version_state();
    int i;

    if (state == NULL)
    {
        return;
    }

    if (cached_version_graph_oid == graph_oid &&
        cached_version_graph_index >= 0 &&
        cached_version_graph_index < state->num_entries &&
        state->entries[cached_version_graph_index].graph_oid == graph_oid)
    {
        pg_atomic_fetch_add_u64(
            &state->entries[cached_version_graph_index].version, 1);
        return;
    }

    /* try to find existing entry (lock-free) */
    for (i = 0; i < state->num_entries; i++)
    {
        if (state->entries[i].graph_oid == graph_oid)
        {
            cached_version_graph_oid = graph_oid;
            cached_version_graph_index = i;

            pg_atomic_fetch_add_u64(&state->entries[i].version, 1);
            return;
        }
    }

    /* new graph — need lock to allocate slot */
    LWLockAcquire(&state->lock, LW_EXCLUSIVE);

    /* re-check after acquiring lock (another backend may have added it) */
    for (i = 0; i < state->num_entries; i++)
    {
        if (state->entries[i].graph_oid == graph_oid)
        {
            LWLockRelease(&state->lock);
            cached_version_graph_oid = graph_oid;
            cached_version_graph_index = i;

            pg_atomic_fetch_add_u64(&state->entries[i].version, 1);
            return;
        }
    }

    /* add new entry */
    if (state->num_entries < AGE_MAX_GRAPHS)
    {
        int idx = state->num_entries;

        state->entries[idx].graph_oid = graph_oid;
        pg_atomic_init_u64(&state->entries[idx].version, 1);
        cached_version_graph_oid = graph_oid;
        cached_version_graph_index = idx;

        /*
         * Write barrier ensures the entry fields are fully visible to
         * other backends before num_entries is incremented. This prevents
         * readers on weak memory-ordering architectures (e.g., ARM) from
         * seeing the incremented count before the entry is initialized.
         */
        pg_write_barrier();
        state->num_entries++;
    }
    else
    {
        elog(WARNING, "AGE: graph version counter table full (%d graphs)",
             AGE_MAX_GRAPHS);
    }

    LWLockRelease(&state->lock);
}

/*
 * Helper function to look up the graph OID for a given label table OID.
 * Uses AGE's label relation cache for fast lookup.
 * Returns InvalidOid if the table is not a graph label table.
 */
Oid get_graph_oid_for_table(Oid table_oid)
{
    label_cache_data *lcd = NULL;

    lcd = search_label_relation_cache_cached(table_oid);

    if (lcd != NULL)
    {
        return lcd->graph;
    }

    return InvalidOid;
}

/*
 * SQL-callable trigger function for VLE cache invalidation.
 * Installed on graph label tables (AFTER INSERT/UPDATE/DELETE FOR EACH STATEMENT).
 * Looks up which graph the triggering table belongs to and increments
 * that graph's version counter.
 */
PG_FUNCTION_INFO_V1(age_invalidate_graph_cache);

Datum age_invalidate_graph_cache(PG_FUNCTION_ARGS)
{
    TriggerData *trigdata;
    Oid table_oid;
    Oid graph_oid;

    /* verify called as trigger */
    if (!CALLED_AS_TRIGGER(fcinfo))
    {
        ereport(ERROR,
                (errcode(ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED),
                 errmsg("age_invalidate_graph_cache: not called as trigger")));
    }

    trigdata = (TriggerData *) fcinfo->context;
    table_oid = RelationGetRelid(trigdata->tg_relation);

    /* look up which graph this label table belongs to */
    graph_oid = get_graph_oid_for_table(table_oid);

    if (OidIsValid(graph_oid))
    {
        increment_graph_version(graph_oid);
    }

    /*
     * Trigger protocol: return a null pointer without setting fcinfo->isnull.
     * PG_RETURN_NULL() sets isnull=true, which violates the trigger protocol
     * and causes "trigger function returned null value" errors during COPY.
     */
    PG_RETURN_POINTER(NULL);
}

/*
 * age_graph_edge_label_stats(graph_name)
 *
 * Graph cardinality-estimation primitive: the (src_label, edge_label,
 * dst_label) edge-count triples for a graph.  This is the source-selectivity
 * authority the graph-source cost model lacks today -- fanout estimation keys
 * only on an edge label's total reltuples, so it cannot tell that ':KNOWS'
 * edges run mostly between ':Person' vertices rather than to ':Company'.
 *
 * Implemented in C (a direct heap scan of each child edge table) rather than
 * SQL because a plpgsql/SPI version crashes the backend: looping
 * RETURN QUERY EXECUTE over AGE label tables faults on the second iteration,
 * and the AGE label-id functions fault over the parent inheritance scan.  The
 * label id encoded in each endpoint graphid names the source/destination vertex
 * labels; the edge label is the scanned relation's own label.
 */
PG_FUNCTION_INFO_V1(age_graph_edge_label_stats);

typedef struct edge_label_triple_key
{
    int32 src_label_id;
    int32 edge_label_id;
    int32 dst_label_id;
} edge_label_triple_key;

typedef struct edge_label_triple_entry
{
    edge_label_triple_key key;
    int64 edge_count;
} edge_label_triple_entry;

Datum age_graph_edge_label_stats(PG_FUNCTION_ARGS)
{
    char *graph_name;
    Oid graph_oid;
    ReturnSetInfo *rsi;
    TupleDesc ret_tdesc;
    Tuplestorestate *tuple_store;
    HASHCTL hash_ctl;
    HTAB *triples;
    Relation ag_label;
    TupleDesc label_tupdesc;
    SysScanDesc label_scan;
    ScanKeyData label_key;
    HeapTuple label_tuple;
    Snapshot snapshot;
    HASH_SEQ_STATUS seq;
    edge_label_triple_entry *entry;

    if (PG_ARGISNULL(0))
    {
        ereport(ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                 errmsg("graph_name must not be null")));
    }

    graph_name = NameStr(*PG_GETARG_NAME(0));
    graph_oid = get_graph_oid(graph_name);
    if (!OidIsValid(graph_oid))
    {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("graph \"%s\" does not exist", graph_name)));
    }

    rsi = (ReturnSetInfo *) fcinfo->resultinfo;
    if (rsi == NULL || !IsA(rsi, ReturnSetInfo) ||
        (rsi->allowedModes & SFRM_Materialize) == 0)
    {
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("set-valued function called in context that cannot "
                        "accept a set")));
    }

    /*
     * Use the standard materialize-mode SRF setup so the tuplestore is created
     * in the proper memory context for every call site (a plain SELECT and an
     * INSERT ... SELECT set up the ReturnSetInfo/econtext differently).
     */
    InitMaterializedSRF(fcinfo, MAT_SRF_USE_EXPECTED_DESC);
    tuple_store = rsi->setResult;
    ret_tdesc = rsi->setDesc;

    MemSet(&hash_ctl, 0, sizeof(hash_ctl));
    hash_ctl.keysize = sizeof(edge_label_triple_key);
    hash_ctl.entrysize = sizeof(edge_label_triple_entry);
    hash_ctl.hcxt = CurrentMemoryContext;
    triples = hash_create("age edge label triple stats", 256, &hash_ctl,
                          HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);

    snapshot = GetActiveSnapshot();
    ag_label = table_open(ag_label_relation_id(), AccessShareLock);
    label_tupdesc = RelationGetDescr(ag_label);
    ScanKeyInit(&label_key, Anum_ag_label_graph, BTEqualStrategyNumber,
                F_OIDEQ, ObjectIdGetDatum(graph_oid));
    label_scan = systable_beginscan(ag_label, ag_label_graph_oid_index_id(),
                                    true, snapshot, 1, &label_key);

    while (HeapTupleIsValid(label_tuple = systable_getnext(label_scan)))
    {
        bool isnull;
        char label_kind;
        int32 edge_label_id;
        Oid edge_relation_oid;
        Relation edge_rel;
        TupleDesc edge_tupdesc;
        TableScanDesc edge_scan;
        HeapTuple edge_tuple;

        label_kind = DatumGetChar(heap_getattr(label_tuple, Anum_ag_label_kind,
                                               label_tupdesc, &isnull));
        if (isnull || label_kind != LABEL_TYPE_EDGE)
            continue;

        edge_label_id = DatumGetInt32(heap_getattr(label_tuple, Anum_ag_label_id,
                                                   label_tupdesc, &isnull));
        if (isnull)
            continue;

        edge_relation_oid = DatumGetObjectId(heap_getattr(
            label_tuple, Anum_ag_label_relation, label_tupdesc, &isnull));
        if (isnull || !OidIsValid(edge_relation_oid))
            continue;

        /*
         * A direct heap scan of an edge label table returns only that table's
         * own rows (inheritance is not expanded), so the default parent edge
         * relation contributes nothing and each child contributes its own
         * edges under its own label id.
         */
        edge_rel = table_open(edge_relation_oid, AccessShareLock);
        edge_tupdesc = RelationGetDescr(edge_rel);
        /* edge tables have 4 columns: id, start_id, end_id, properties */
        if (edge_tupdesc->natts != 4)
        {
            table_close(edge_rel, AccessShareLock);
            continue;
        }
        edge_scan = table_beginscan(edge_rel, snapshot, 0, NULL);
        while ((edge_tuple = heap_getnext(edge_scan,
                                          ForwardScanDirection)) != NULL)
        {
            graphid start_id;
            graphid end_id;
            bool start_isnull;
            bool end_isnull;
            edge_label_triple_key key;
            edge_label_triple_entry *te;
            bool found;

            start_id = DatumGetInt64(heap_getattr(
                edge_tuple, Anum_ag_label_edge_table_start_id, edge_tupdesc,
                &start_isnull));
            end_id = DatumGetInt64(heap_getattr(
                edge_tuple, Anum_ag_label_edge_table_end_id, edge_tupdesc,
                &end_isnull));
            if (start_isnull || end_isnull)
                continue;

            key.src_label_id = get_graphid_label_id(start_id);
            key.edge_label_id = edge_label_id;
            key.dst_label_id = get_graphid_label_id(end_id);

            te = (edge_label_triple_entry *) hash_search(triples, &key,
                                                         HASH_ENTER, &found);
            if (!found)
                te->edge_count = 0;
            te->edge_count++;
        }
        table_endscan(edge_scan);
        table_close(edge_rel, AccessShareLock);
    }

    systable_endscan(label_scan);
    table_close(ag_label, AccessShareLock);

    hash_seq_init(&seq, triples);
    while ((entry = (edge_label_triple_entry *) hash_seq_search(&seq)) != NULL)
    {
        Datum values[4];
        bool nulls[4] = {false, false, false, false};
        label_cache_data *src_label;
        label_cache_data *edge_label;
        label_cache_data *dst_label;

        src_label = search_label_graph_oid_cache(graph_oid,
                                                 entry->key.src_label_id);
        edge_label = search_label_graph_oid_cache(graph_oid,
                                                  entry->key.edge_label_id);
        dst_label = search_label_graph_oid_cache(graph_oid,
                                                 entry->key.dst_label_id);
        if (src_label == NULL || edge_label == NULL || dst_label == NULL)
            continue;

        values[0] = NameGetDatum(&src_label->name);
        values[1] = NameGetDatum(&edge_label->name);
        values[2] = NameGetDatum(&dst_label->name);
        values[3] = Int64GetDatum(entry->edge_count);

        tuplestore_putvalues(tuple_store, ret_tdesc, values, nulls);
    }
    hash_destroy(triples);

    PG_RETURN_NULL();
}

/*
 * Cached destination-label selectivity for an edge label: the fraction of that
 * edge label's edges whose destination vertex carries dst_label_name, read from
 * the ag_graph_edge_stats cache.  Returns -1 when the cache holds no rows for
 * the (optionally source-restricted) edge label (e.g. it was never refreshed),
 * so the caller keeps its default estimate; this keeps the conditioning opt-in
 * and leaves un-refreshed graphs' plans unchanged.
 *
 * When src_label_name is non-NULL the ratio is conditioned on that source
 * vertex label -- the fraction of src_label's edge_label edges that reach
 * dst_label.  This is the dimension the adjacency directory cannot express
 * (it keys only on edge_label -> dst_label), so it is the cache's net-new
 * authority for fanout.  When src_label_name is NULL the ratio is over all
 * sources, matching the original behaviour.
 */
double age_cached_edge_dst_label_selectivity(Oid graph_oid,
                                             const char *src_label_name,
                                             const char *edge_label_name,
                                             const char *dst_label_name)
{
    Oid stats_oid;
    Relation stats_rel;
    TupleDesc tupdesc;
    TableScanDesc scan;
    HeapTuple tuple;
    int64 total_count = 0;
    int64 matched_count = 0;

    if (!OidIsValid(graph_oid) || edge_label_name == NULL ||
        dst_label_name == NULL)
        return -1.0;

    stats_oid = ag_relation_id("ag_graph_edge_stats", "table");
    if (!OidIsValid(stats_oid))
        return -1.0;

    stats_rel = table_open(stats_oid, AccessShareLock);
    tupdesc = RelationGetDescr(stats_rel);
    scan = table_beginscan(stats_rel, GetActiveSnapshot(), 0, NULL);
    while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
    {
        bool isnull;
        Oid row_graph;
        Name row_src_label;
        Name row_edge_label;
        Name row_dst_label;
        int64 row_count;

        /* ag_graph_edge_stats columns: graph, src_label, edge_label, dst_label, edge_count */
        row_graph = DatumGetObjectId(heap_getattr(tuple, 1, tupdesc, &isnull));
        if (isnull || row_graph != graph_oid)
            continue;
        row_edge_label = DatumGetName(heap_getattr(tuple, 3, tupdesc, &isnull));
        if (isnull || strcmp(NameStr(*row_edge_label), edge_label_name) != 0)
            continue;
        if (src_label_name != NULL)
        {
            row_src_label = DatumGetName(heap_getattr(tuple, 2, tupdesc,
                                                      &isnull));
            if (isnull || strcmp(NameStr(*row_src_label), src_label_name) != 0)
                continue;
        }
        row_count = DatumGetInt64(heap_getattr(tuple, 5, tupdesc, &isnull));
        if (isnull)
            continue;

        total_count += row_count;

        row_dst_label = DatumGetName(heap_getattr(tuple, 4, tupdesc, &isnull));
        if (!isnull && strcmp(NameStr(*row_dst_label), dst_label_name) == 0)
            matched_count += row_count;
    }
    table_endscan(scan);
    table_close(stats_rel, AccessShareLock);

    if (total_count <= 0)
        return -1.0;

    return (double) matched_count / (double) total_count;
}

/*
 * age_graph_edge_degree_stats(graph_name)
 *
 * Per (src_label, edge_label) out-degree distribution: the maximum and average
 * number of edge_label edges leaving a single src_label vertex, plus the count
 * of source vertices.  The graph-source cost model already conditions on the
 * mean degree; this exposes the *max* so a planner/operator can see hub skew
 * (a power-law degree tail) that the mean hides -- e.g. one vertex with 10000
 * edges among thousands with one.  Implemented as a direct two-level heap-scan
 * aggregation: per-source-vertex degree first, then rolled up per source label.
 */
PG_FUNCTION_INFO_V1(age_graph_edge_degree_stats);

typedef struct edge_source_degree_key
{
    graphid source_id;
    int32 edge_label_id;
} edge_source_degree_key;

typedef struct edge_source_degree_entry
{
    edge_source_degree_key key;
    int64 degree;
} edge_source_degree_entry;

typedef struct edge_label_degree_key
{
    int32 src_label_id;
    int32 edge_label_id;
} edge_label_degree_key;

typedef struct edge_label_degree_entry
{
    edge_label_degree_key key;
    int64 max_degree;
    int64 sum_degree;
    int64 vertex_count;
} edge_label_degree_entry;

static Datum age_edge_degree_stats_worker(FunctionCallInfo fcinfo,
                                          AttrNumber endpoint_attno)
{
    char *graph_name;
    Oid graph_oid;
    ReturnSetInfo *rsi;
    TupleDesc ret_tdesc;
    Tuplestorestate *tuple_store;
    HASHCTL hash_ctl;
    HTAB *per_vertex;
    HTAB *per_label;
    Relation ag_label;
    TupleDesc label_tupdesc;
    SysScanDesc label_scan;
    ScanKeyData label_key;
    HeapTuple label_tuple;
    Snapshot snapshot;
    HASH_SEQ_STATUS seq;
    edge_source_degree_entry *vertex_entry;
    edge_label_degree_entry *label_entry;

    if (PG_ARGISNULL(0))
        ereport(ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                 errmsg("graph_name must not be null")));

    graph_name = NameStr(*PG_GETARG_NAME(0));
    graph_oid = get_graph_oid(graph_name);
    if (!OidIsValid(graph_oid))
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("graph \"%s\" does not exist", graph_name)));

    rsi = (ReturnSetInfo *) fcinfo->resultinfo;
    if (rsi == NULL || !IsA(rsi, ReturnSetInfo) ||
        (rsi->allowedModes & SFRM_Materialize) == 0)
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("set-valued function called in context that cannot "
                        "accept a set")));

    InitMaterializedSRF(fcinfo, MAT_SRF_USE_EXPECTED_DESC);
    tuple_store = rsi->setResult;
    ret_tdesc = rsi->setDesc;

    MemSet(&hash_ctl, 0, sizeof(hash_ctl));
    hash_ctl.keysize = sizeof(edge_source_degree_key);
    hash_ctl.entrysize = sizeof(edge_source_degree_entry);
    hash_ctl.hcxt = CurrentMemoryContext;
    per_vertex = hash_create("age edge source degree", 1024, &hash_ctl,
                             HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);

    MemSet(&hash_ctl, 0, sizeof(hash_ctl));
    hash_ctl.keysize = sizeof(edge_label_degree_key);
    hash_ctl.entrysize = sizeof(edge_label_degree_entry);
    hash_ctl.hcxt = CurrentMemoryContext;
    per_label = hash_create("age edge label degree", 64, &hash_ctl,
                            HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);

    snapshot = GetActiveSnapshot();
    ag_label = table_open(ag_label_relation_id(), AccessShareLock);
    label_tupdesc = RelationGetDescr(ag_label);
    ScanKeyInit(&label_key, Anum_ag_label_graph, BTEqualStrategyNumber,
                F_OIDEQ, ObjectIdGetDatum(graph_oid));
    label_scan = systable_beginscan(ag_label, ag_label_graph_oid_index_id(),
                                    true, snapshot, 1, &label_key);

    /* Pass 1: per-source-vertex degree for each edge label. */
    while (HeapTupleIsValid(label_tuple = systable_getnext(label_scan)))
    {
        bool isnull;
        char label_kind;
        int32 edge_label_id;
        Oid edge_relation_oid;
        Relation edge_rel;
        TupleDesc edge_tupdesc;
        TableScanDesc edge_scan;
        HeapTuple edge_tuple;

        label_kind = DatumGetChar(heap_getattr(label_tuple, Anum_ag_label_kind,
                                               label_tupdesc, &isnull));
        if (isnull || label_kind != LABEL_TYPE_EDGE)
            continue;
        edge_label_id = DatumGetInt32(heap_getattr(label_tuple, Anum_ag_label_id,
                                                   label_tupdesc, &isnull));
        if (isnull)
            continue;
        edge_relation_oid = DatumGetObjectId(heap_getattr(
            label_tuple, Anum_ag_label_relation, label_tupdesc, &isnull));
        if (isnull || !OidIsValid(edge_relation_oid))
            continue;

        edge_rel = table_open(edge_relation_oid, AccessShareLock);
        edge_tupdesc = RelationGetDescr(edge_rel);
        if (edge_tupdesc->natts != 4)
        {
            table_close(edge_rel, AccessShareLock);
            continue;
        }
        edge_scan = table_beginscan(edge_rel, snapshot, 0, NULL);
        while ((edge_tuple = heap_getnext(edge_scan,
                                          ForwardScanDirection)) != NULL)
        {
            graphid endpoint_id;
            bool endpoint_isnull;
            edge_source_degree_key key;
            edge_source_degree_entry *ve;
            bool found;

            endpoint_id = DatumGetInt64(heap_getattr(
                edge_tuple, endpoint_attno, edge_tupdesc,
                &endpoint_isnull));
            if (endpoint_isnull)
                continue;

            MemSet(&key, 0, sizeof(key));
            key.source_id = endpoint_id;
            key.edge_label_id = edge_label_id;
            ve = (edge_source_degree_entry *) hash_search(per_vertex, &key,
                                                          HASH_ENTER, &found);
            if (!found)
                ve->degree = 0;
            ve->degree++;
        }
        table_endscan(edge_scan);
        table_close(edge_rel, AccessShareLock);
    }
    systable_endscan(label_scan);
    table_close(ag_label, AccessShareLock);

    /* Pass 2: roll each source vertex's degree up under its source label. */
    hash_seq_init(&seq, per_vertex);
    while ((vertex_entry =
            (edge_source_degree_entry *) hash_seq_search(&seq)) != NULL)
    {
        edge_label_degree_key key;
        edge_label_degree_entry *le;
        bool found;

        MemSet(&key, 0, sizeof(key));
        key.src_label_id = get_graphid_label_id(vertex_entry->key.source_id);
        key.edge_label_id = vertex_entry->key.edge_label_id;
        le = (edge_label_degree_entry *) hash_search(per_label, &key,
                                                     HASH_ENTER, &found);
        if (!found)
        {
            le->max_degree = 0;
            le->sum_degree = 0;
            le->vertex_count = 0;
        }
        if (vertex_entry->degree > le->max_degree)
            le->max_degree = vertex_entry->degree;
        le->sum_degree += vertex_entry->degree;
        le->vertex_count++;
    }

    hash_seq_init(&seq, per_label);
    while ((label_entry =
            (edge_label_degree_entry *) hash_seq_search(&seq)) != NULL)
    {
        Datum values[5];
        bool nulls[5] = {false, false, false, false, false};
        label_cache_data *src_label;
        label_cache_data *edge_label;

        src_label = search_label_graph_oid_cache(graph_oid,
                                                 label_entry->key.src_label_id);
        edge_label = search_label_graph_oid_cache(graph_oid,
                                                  label_entry->key.edge_label_id);
        if (src_label == NULL || edge_label == NULL)
            continue;

        values[0] = NameGetDatum(&src_label->name);
        values[1] = NameGetDatum(&edge_label->name);
        values[2] = Int64GetDatum(label_entry->max_degree);
        values[3] = Float8GetDatum(label_entry->vertex_count > 0 ?
                                   (double) label_entry->sum_degree /
                                   (double) label_entry->vertex_count : 0.0);
        values[4] = Int64GetDatum(label_entry->vertex_count);
        tuplestore_putvalues(tuple_store, ret_tdesc, values, nulls);
    }

    hash_destroy(per_vertex);
    hash_destroy(per_label);

    PG_RETURN_NULL();
}

/*
 * Out-degree: distribution of how many edge_label edges leave each src_label
 * source vertex (keyed on the edge's start endpoint).
 */
Datum age_graph_edge_degree_stats(PG_FUNCTION_ARGS)
{
    return age_edge_degree_stats_worker(fcinfo,
                                        Anum_ag_label_edge_table_start_id);
}

/*
 * In-degree: the mirror image -- how many edge_label edges arrive at each
 * dst_label terminal vertex (keyed on the edge's end endpoint).  Gives the
 * reverse-direction fanout the cost model needs to reason about expanding a
 * pattern from its terminal side instead of its source side.
 */
PG_FUNCTION_INFO_V1(age_graph_edge_in_degree_stats);

Datum age_graph_edge_in_degree_stats(PG_FUNCTION_ARGS)
{
    return age_edge_degree_stats_worker(fcinfo,
                                        Anum_ag_label_edge_table_end_id);
}

/*
 * age_adjacency_multiway_intersect(graph_name, edge_label, source_ids,
 *                                  direction)
 *
 * WCOJ-style multiway-intersection primitive (item 6 foundation).  Given a set
 * of source vertices, returns every destination vertex reached from EVERY
 * source -- i.e. the intersection of the sources' adjacency lists.  This is the
 * kernel a worst-case-optimal join uses to close a star/fork/cycle in one pass
 * instead of materializing pairwise binary-join intermediates.
 *
 * Each source carries a direction.  source_ids elements are either a bare
 * integer id (using the scalar `direction` argument, default 'out') or an
 * object {"id": <int>, "dir": "out"|"in"}.  'out' sources contribute their
 * out-neighbours (start_id index); 'in' sources contribute their in-neighbours
 * (end_id index).  Mixing directions in one call expresses triangle closure:
 * c in out(b) intersect in(a) for a driving edge (a)->(b).
 *
 * Both the out- and in-direction run scans yield destinations globally ordered
 * by next_vertex_id, so a two-way merge of their heads produces the combined
 * stream in sorted order.  For each contiguous destination group we record
 * which distinct sources reached it (one bit per source, deduped against
 * multigraph edges and repeated keys) and emit the destination once all
 * sources cover it.  No pairwise intermediate is built.
 *
 * The result must match the binary-join plan for the same pattern; that
 * correctness contract is pinned in regress/sql/cypher_wcoj_semantics.sql and
 * regress/sql/age_adjacency.sql.
 */
PG_FUNCTION_INFO_V1(age_adjacency_multiway_intersect);

/*
 * Resolve the age_adjacency index on an edge label whose key column order
 * traverses in the requested direction: outgoing scans (start_id, id, end_id)
 * so each posting's "next vertex" is the edge's end (an out-neighbour);
 * incoming scans (end_id, id, start_id) so the next vertex is the edge's start
 * (an in-neighbour).
 */
static Oid age_adjacency_resolve_index(Oid edge_relation_oid, bool outgoing)
{
    static Oid age_adjacency_am_oid = InvalidOid;
    Relation edge_rel;
    List *index_list;
    ListCell *lc;
    Oid result = InvalidOid;
    AttrNumber first_attno = outgoing ? Anum_ag_label_edge_table_start_id :
        Anum_ag_label_edge_table_end_id;
    AttrNumber last_attno = outgoing ? Anum_ag_label_edge_table_end_id :
        Anum_ag_label_edge_table_start_id;

    if (!OidIsValid(age_adjacency_am_oid))
        age_adjacency_am_oid = GetSysCacheOid1(AMNAME, Anum_pg_am_oid,
                                               CStringGetDatum("age_adjacency"));
    if (!OidIsValid(age_adjacency_am_oid))
        return InvalidOid;

    edge_rel = relation_open(edge_relation_oid, AccessShareLock);
    index_list = RelationGetIndexList(edge_rel);
    foreach(lc, index_list)
    {
        Oid index_oid = lfirst_oid(lc);
        Relation index_rel = index_open(index_oid, AccessShareLock);
        int2vector *indkey;

        indkey = &index_rel->rd_index->indkey;
        if (index_rel->rd_rel->relam == age_adjacency_am_oid &&
            index_rel->rd_index->indisvalid &&
            index_rel->rd_index->indisready &&
            index_rel->rd_index->indnkeyatts == 3 &&
            index_rel->rd_index->indnatts == 3 &&
            indkey->values[0] == first_attno &&
            indkey->values[1] == Anum_ag_label_edge_table_id &&
            indkey->values[2] == last_attno)
            result = index_oid;
        index_close(index_rel, AccessShareLock);
        if (OidIsValid(result))
            break;
    }
    list_free(index_list);
    relation_close(edge_rel, AccessShareLock);
    return result;
}

/*
 * A parsed source vertex plus the direction it expands in.  Triangle closure
 * intersects out-neighbours of some sources with in-neighbours of others
 * (c in out(b) and c in in(a)), so each source carries its own direction.
 */
typedef struct WcojSource
{
    graphid id;
    bool outgoing;
    int64 global_index;
} WcojSource;

static int age_wcoj_source_cmp(const void *a, const void *b)
{
    const WcojSource *l = (const WcojSource *) a;
    const WcojSource *r = (const WcojSource *) b;

    if (l->outgoing != r->outgoing)
        return l->outgoing ? 1 : -1;
    if (l->id < r->id)
        return -1;
    if (l->id > r->id)
        return 1;
    return 0;
}

/*
 * A run scan plus a one-item lookahead head.  Both the out- and in-direction
 * run scans emit destinations globally ordered by next_vertex_id, so a simple
 * two-way merge of their heads yields the combined stream in sorted order --
 * the leapfrog substrate for intersecting across both directions at once.
 */
typedef struct WcojMergeCursor
{
    AgeAdjacencyVisiblePayloadRunScan *scan;
    AgeAdjacencyPayload head;
    void *head_tag;
    bool head_valid;
} WcojMergeCursor;

static void age_wcoj_merge_cursor_refill(WcojMergeCursor *c)
{
    if (c->scan == NULL)
    {
        c->head_valid = false;
        return;
    }
    c->head_valid = age_adjacency_visible_payload_run_scan_next_tag(
        c->scan, &c->head, &c->head_tag);
}

/* Index of the cursor whose head has the smallest next_vertex_id, or -1. */
static int age_wcoj_merge_pick(WcojMergeCursor *cursors, int n)
{
    int best = -1;
    int i;

    for (i = 0; i < n; i++)
    {
        if (!cursors[i].head_valid)
            continue;
        if (best < 0 ||
            cursors[i].head.next_vertex_id < cursors[best].head.next_vertex_id)
            best = i;
    }
    return best;
}

Datum age_adjacency_multiway_intersect(PG_FUNCTION_ARGS)
{
    char *graph_name;
    char *edge_label_name;
    agtype *source_arg;
    bool default_outgoing = true;
    Oid graph_oid;
    Oid edge_relation_oid;
    ReturnSetInfo *rsi;
    TupleDesc ret_tdesc;
    Tuplestorestate *tuple_store;
    WcojSource *sources;
    int64 source_count = 0;
    int64 source_capacity = 0;
    int64 out_count = 0;
    int64 in_count = 0;
    int64 i;
    AgeAdjacencyVisiblePayloadRunKey *out_keys = NULL;
    AgeAdjacencyVisiblePayloadRunKey *in_keys = NULL;
    WcojMergeCursor cursors[2];
    int sel;
    graphid current_vertex = 0;
    bool have_current = false;
    uint64 *seen_words;
    int64 word_count;
    int64 distinct_seen = 0;
    agtype_iterator *it;
    agtype_value av;
    agtype_iterator_token tok;
    bool in_object = false;
    bool obj_have_id = false;
    bool obj_outgoing = true;
    graphid obj_id = 0;
    char *obj_pending_key = NULL;

    if (PG_ARGISNULL(0) || PG_ARGISNULL(1) || PG_ARGISNULL(2))
        ereport(ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                 errmsg("graph_name, edge_label, and source_ids must not be "
                        "null")));

    graph_name = NameStr(*PG_GETARG_NAME(0));
    edge_label_name = NameStr(*PG_GETARG_NAME(1));
    source_arg = AG_GET_ARG_AGTYPE_P(2);

    if (!PG_ARGISNULL(3))
    {
        char *dir = text_to_cstring(PG_GETARG_TEXT_PP(3));

        if (strcmp(dir, "out") == 0 || strcmp(dir, "outgoing") == 0)
            default_outgoing = true;
        else if (strcmp(dir, "in") == 0 || strcmp(dir, "incoming") == 0)
            default_outgoing = false;
        else
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                     errmsg("direction must be 'out' or 'in', got \"%s\"",
                            dir)));
    }

    graph_oid = get_graph_oid(graph_name);
    if (!OidIsValid(graph_oid))
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("graph \"%s\" does not exist", graph_name)));

    edge_relation_oid = get_label_relation(edge_label_name, graph_oid);
    if (!OidIsValid(edge_relation_oid))
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("edge label \"%s\" does not exist in graph \"%s\"",
                        edge_label_name, graph_name)));

    rsi = (ReturnSetInfo *) fcinfo->resultinfo;
    if (rsi == NULL || !IsA(rsi, ReturnSetInfo) ||
        (rsi->allowedModes & SFRM_Materialize) == 0)
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("set-valued function called in context that cannot "
                        "accept a set")));

    InitMaterializedSRF(fcinfo, MAT_SRF_USE_EXPECTED_DESC);
    tuple_store = rsi->setResult;
    ret_tdesc = rsi->setDesc;

    /*
     * Collect the source vertices.  Each array element is either a bare integer
     * vertex id (using the default direction) or an object {"id": <int>, "dir":
     * "out"|"in"} giving that source its own direction -- which lets a single
     * call intersect out-neighbours of some sources with in-neighbours of
     * others, the shape triangle closure needs.
     */
    if (!AGT_ROOT_IS_ARRAY(source_arg) || AGT_ROOT_IS_SCALAR(source_arg))
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("source_ids must be an agtype array of vertex ids")));

    sources = NULL;
    it = agtype_iterator_init(&source_arg->root);
    while ((tok = agtype_iterator_next(&it, &av, false)) != WAGT_DONE)
    {
        graphid add_id;
        bool add_outgoing;
        bool emit_source = false;

        switch (tok)
        {
        case WAGT_BEGIN_ARRAY:
            break;
        case WAGT_END_ARRAY:
            break;
        case WAGT_BEGIN_OBJECT:
            in_object = true;
            obj_have_id = false;
            obj_outgoing = default_outgoing;
            obj_pending_key = NULL;
            break;
        case WAGT_END_OBJECT:
            if (!obj_have_id)
                ereport(ERROR,
                        (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                         errmsg("source_ids object element requires an \"id\"")));
            in_object = false;
            add_id = obj_id;
            add_outgoing = obj_outgoing;
            emit_source = true;
            break;
        case WAGT_KEY:
            if (av.type != AGTV_STRING)
                ereport(ERROR,
                        (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                         errmsg("source_ids object keys must be strings")));
            obj_pending_key = pnstrdup(av.val.string.val, av.val.string.len);
            break;
        case WAGT_VALUE:
            /* object field value, paired with obj_pending_key */
            if (obj_pending_key != NULL &&
                strcmp(obj_pending_key, "id") == 0)
            {
                if (av.type != AGTV_INTEGER)
                    ereport(ERROR,
                            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                             errmsg("source_ids \"id\" must be an integer")));
                obj_id = (graphid) av.val.int_value;
                obj_have_id = true;
            }
            else if (obj_pending_key != NULL &&
                     strcmp(obj_pending_key, "dir") == 0)
            {
                if (av.type != AGTV_STRING)
                    ereport(ERROR,
                            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                             errmsg("source_ids \"dir\" must be a string")));
                if (av.val.string.len == 3 &&
                    strncmp(av.val.string.val, "out", 3) == 0)
                    obj_outgoing = true;
                else if (av.val.string.len == 2 &&
                         strncmp(av.val.string.val, "in", 2) == 0)
                    obj_outgoing = false;
                else
                    ereport(ERROR,
                            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                             errmsg("source_ids \"dir\" must be 'out' or "
                                    "'in'")));
            }
            obj_pending_key = NULL;
            break;
        case WAGT_ELEM:
            if (in_object)
                break;
            if (av.type != AGTV_INTEGER)
                ereport(ERROR,
                        (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                         errmsg("source_ids elements must be integer vertex "
                                "ids or {\"id\":..,\"dir\":..} objects")));
            add_id = (graphid) av.val.int_value;
            add_outgoing = default_outgoing;
            emit_source = true;
            break;
        default:
            break;
        }

        if (!emit_source)
            continue;

        if (source_count == source_capacity)
        {
            source_capacity = source_capacity == 0 ? 16 : source_capacity * 2;
            sources = (WcojSource *) (sources == NULL ?
                palloc(sizeof(WcojSource) * source_capacity) :
                repalloc(sources, sizeof(WcojSource) * source_capacity));
        }
        sources[source_count].id = add_id;
        sources[source_count].outgoing = add_outgoing;
        sources[source_count].global_index = 0;
        source_count++;
    }

    if (source_count == 0)
        PG_RETURN_NULL();

    /*
     * The intersection is over a set of distinct (vertex, direction) sources.
     * Dedup so repeated entries collapse to one cursor and source_count is the
     * number of distinct sources a destination must be reached from.  A vertex
     * used both out- and in-direction stays two distinct sources.
     */
    qsort(sources, source_count, sizeof(WcojSource), age_wcoj_source_cmp);
    {
        int64 unique = 0;

        for (i = 0; i < source_count; i++)
        {
            if (unique == 0 ||
                sources[i].id != sources[unique - 1].id ||
                sources[i].outgoing != sources[unique - 1].outgoing)
                sources[unique++] = sources[i];
        }
        source_count = unique;
    }

    /* Assign a global bitmap index and split by direction. */
    out_keys = (AgeAdjacencyVisiblePayloadRunKey *)
        palloc(sizeof(AgeAdjacencyVisiblePayloadRunKey) * source_count);
    in_keys = (AgeAdjacencyVisiblePayloadRunKey *)
        palloc(sizeof(AgeAdjacencyVisiblePayloadRunKey) * source_count);
    for (i = 0; i < source_count; i++)
    {
        sources[i].global_index = i;
        if (sources[i].outgoing)
        {
            out_keys[out_count].key = sources[i].id;
            out_keys[out_count].tag = (void *) (intptr_t) i;
            out_count++;
        }
        else
        {
            in_keys[in_count].key = sources[i].id;
            in_keys[in_count].tag = (void *) (intptr_t) i;
            in_count++;
        }
    }

    memset(cursors, 0, sizeof(cursors));
    if (out_count > 0)
    {
        Oid out_index = age_adjacency_resolve_index(edge_relation_oid, true);

        if (!OidIsValid(out_index))
            ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                     errmsg("edge label \"%s\" has no age_adjacency out-index",
                            edge_label_name),
                     errhint("Create an age_adjacency index on "
                             "(start_id, id, end_id).")));
        cursors[0].scan = age_adjacency_begin_visible_payload_run_scan_with_tags(
            out_index, GetActiveSnapshot(), false, 0, out_keys, out_count);
    }
    if (in_count > 0)
    {
        Oid in_index = age_adjacency_resolve_index(edge_relation_oid, false);

        if (!OidIsValid(in_index))
            ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                     errmsg("edge label \"%s\" has no age_adjacency in-index",
                            edge_label_name),
                     errhint("Create an age_adjacency index on "
                             "(end_id, id, start_id).")));
        cursors[1].scan = age_adjacency_begin_visible_payload_run_scan_with_tags(
            in_index, GetActiveSnapshot(), false, 0, in_keys, in_count);
    }

    word_count = (source_count + 63) / 64;
    seen_words = (uint64 *) palloc0(sizeof(uint64) * word_count);

    age_wcoj_merge_cursor_refill(&cursors[0]);
    age_wcoj_merge_cursor_refill(&cursors[1]);

    while ((sel = age_wcoj_merge_pick(cursors, 2)) >= 0)
    {
        int64 kidx = (int64) (intptr_t) cursors[sel].head_tag;
        graphid v = cursors[sel].head.next_vertex_id;

        if (!have_current || v != current_vertex)
        {
            if (have_current && distinct_seen == source_count)
            {
                Datum values[1];
                bool nulls[1] = {false};

                values[0] = GRAPHID_GET_DATUM(current_vertex);
                tuplestore_putvalues(tuple_store, ret_tdesc, values, nulls);
            }
            MemSet(seen_words, 0, sizeof(uint64) * word_count);
            distinct_seen = 0;
            current_vertex = v;
            have_current = true;
        }

        if (kidx >= 0 && kidx < source_count)
        {
            uint64 bit = UINT64CONST(1) << (kidx & 63);

            if (!(seen_words[kidx >> 6] & bit))
            {
                seen_words[kidx >> 6] |= bit;
                distinct_seen++;
            }
        }

        age_wcoj_merge_cursor_refill(&cursors[sel]);
    }

    if (have_current && distinct_seen == source_count)
    {
        Datum values[1];
        bool nulls[1] = {false};

        values[0] = GRAPHID_GET_DATUM(current_vertex);
        tuplestore_putvalues(tuple_store, ret_tdesc, values, nulls);
    }

    if (cursors[0].scan != NULL)
        age_adjacency_end_visible_payload_run_scan(cursors[0].scan);
    if (cursors[1].scan != NULL)
        age_adjacency_end_visible_payload_run_scan(cursors[1].scan);

    pfree(seen_words);
    pfree(out_keys);
    pfree(in_keys);
    pfree(sources);

    PG_RETURN_NULL();
}

/*
 * Cached average out-degree for a (src_label, edge_label) pair, read from
 * ag_graph_edge_degree_cache.  Returns -1 when the degree cache has no matching
 * row (e.g. it was never refreshed), so the caller keeps its default estimate;
 * this keeps the conditioning opt-in.  This is a per-source-label mean degree,
 * which refines the global reltuples/distinct base-fanout estimate.
 */
double age_cached_edge_avg_out_degree(Oid graph_oid,
                                      const char *src_label_name,
                                      const char *edge_label_name)
{
    Oid cache_oid;
    Relation cache_rel;
    TupleDesc tupdesc;
    TableScanDesc scan;
    HeapTuple tuple;
    double result = -1.0;

    if (!OidIsValid(graph_oid) || src_label_name == NULL ||
        edge_label_name == NULL)
        return -1.0;

    cache_oid = ag_relation_id("ag_graph_edge_degree_cache", "table");
    if (!OidIsValid(cache_oid))
        return -1.0;

    cache_rel = table_open(cache_oid, AccessShareLock);
    tupdesc = RelationGetDescr(cache_rel);
    scan = table_beginscan(cache_rel, GetActiveSnapshot(), 0, NULL);
    while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
    {
        bool isnull;
        Oid row_graph;
        Name row_src_label;
        Name row_edge_label;

        /* columns: graph, src_label, edge_label, max_out_degree, avg_out_degree, source_vertex_count */
        row_graph = DatumGetObjectId(heap_getattr(tuple, 1, tupdesc, &isnull));
        if (isnull || row_graph != graph_oid)
            continue;
        row_src_label = DatumGetName(heap_getattr(tuple, 2, tupdesc, &isnull));
        if (isnull || strcmp(NameStr(*row_src_label), src_label_name) != 0)
            continue;
        row_edge_label = DatumGetName(heap_getattr(tuple, 3, tupdesc, &isnull));
        if (isnull || strcmp(NameStr(*row_edge_label), edge_label_name) != 0)
            continue;
        result = DatumGetFloat8(heap_getattr(tuple, 5, tupdesc, &isnull));
        if (isnull)
            result = -1.0;
        break;
    }
    table_endscan(scan);
    table_close(cache_rel, AccessShareLock);

    return result;
}

/*
 * Cached average in-degree for a (dst_label, edge_label) pair, read from
 * ag_graph_edge_in_degree_cache.  The terminal-side mirror of
 * age_cached_edge_avg_out_degree(): conditions the base fanout of an incoming
 * (reverse) adjacency expansion on how many edge_label edges arrive at a
 * dst_label vertex.  Returns -1 when the cache has no matching row.
 */
double age_cached_edge_avg_in_degree(Oid graph_oid,
                                     const char *dst_label_name,
                                     const char *edge_label_name)
{
    Oid cache_oid;
    Relation cache_rel;
    TupleDesc tupdesc;
    TableScanDesc scan;
    HeapTuple tuple;
    double result = -1.0;

    if (!OidIsValid(graph_oid) || dst_label_name == NULL ||
        edge_label_name == NULL)
        return -1.0;

    cache_oid = ag_relation_id("ag_graph_edge_in_degree_cache", "table");
    if (!OidIsValid(cache_oid))
        return -1.0;

    cache_rel = table_open(cache_oid, AccessShareLock);
    tupdesc = RelationGetDescr(cache_rel);
    scan = table_beginscan(cache_rel, GetActiveSnapshot(), 0, NULL);
    while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
    {
        bool isnull;
        Oid row_graph;
        Name row_dst_label;
        Name row_edge_label;

        /* columns: graph, dst_label, edge_label, max_in_degree, avg_in_degree, terminal_vertex_count */
        row_graph = DatumGetObjectId(heap_getattr(tuple, 1, tupdesc, &isnull));
        if (isnull || row_graph != graph_oid)
            continue;
        row_dst_label = DatumGetName(heap_getattr(tuple, 2, tupdesc, &isnull));
        if (isnull || strcmp(NameStr(*row_dst_label), dst_label_name) != 0)
            continue;
        row_edge_label = DatumGetName(heap_getattr(tuple, 3, tupdesc, &isnull));
        if (isnull || strcmp(NameStr(*row_edge_label), edge_label_name) != 0)
            continue;
        result = DatumGetFloat8(heap_getattr(tuple, 5, tupdesc, &isnull));
        if (isnull)
            result = -1.0;
        break;
    }
    table_endscan(scan);
    table_close(cache_rel, AccessShareLock);

    return result;
}
