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

#include "access/age_adjacency.h"
#include "access/amapi.h"
#include "access/genam.h"
#include "access/generic_xlog.h"
#include "access/relation.h"
#include "access/relscan.h"
#include "access/tableam.h"
#include "catalog/ag_label.h"
#include "executor/tuptable.h"
#include "fmgr.h"
#include "funcapi.h"
#include "nodes/nodes.h"
#include "nodes/pathnodes.h"
#include "nodes/tidbitmap.h"
#include "optimizer/optimizer.h"
#include "storage/bufmgr.h"
#include "storage/bufpage.h"
#include "storage/itemptr.h"
#include "storage/lmgr.h"
#include "utils/graphid.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/tuplestore.h"

#define AGE_ADJACENCY_MAGIC 0xA9EAD620
#define AGE_ADJACENCY_VERSION 3
#define AGE_ADJACENCY_METAPAGE_BLKNO 0
#define AGE_ADJACENCY_EQUAL_STRATEGY 1
#define AGE_ADJACENCY_PAYLOAD_NATTS 3

#define AGE_ADJACENCY_PAGE_META 1
#define AGE_ADJACENCY_PAGE_DIRECTORY 2
#define AGE_ADJACENCY_PAGE_MAIN 3
#define AGE_ADJACENCY_PAGE_DELTA 4

#define AGE_ADJACENCY_DELTA_REINDEX_THRESHOLD 4096
#define AGE_ADJACENCY_PAGE_USABLE_BYTES \
    (BLCKSZ - MAXALIGN(SizeOfPageHeaderData) - \
     MAXALIGN(sizeof(AgeAdjacencyPageOpaqueData)))
#define AGE_ADJACENCY_ITEMS_PER_PAGE(item_size) \
    Max(1, (int) (AGE_ADJACENCY_PAGE_USABLE_BYTES / \
                  (MAXALIGN(item_size) + sizeof(ItemIdData))))

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
    BlockNumber next_blkno;
    uint32 posting_count;
    graphid min_key;
    graphid max_key;
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

typedef struct AgeAdjacencyDirectoryEntryData
{
    graphid key;
    BlockNumber first_blkno;
    OffsetNumber first_offnum;
    uint32 posting_count;
} AgeAdjacencyDirectoryEntryData;

typedef AgeAdjacencyDirectoryEntryData *AgeAdjacencyDirectoryEntry;

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
    AgeAdjacencyPayloadCallback callback;
    void *callback_state;
} AgeAdjacencyScanTarget;

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

PG_FUNCTION_INFO_V1(age_adjacency_handler);
PG_FUNCTION_INFO_V1(age_adjacency_debug_payload);
PG_FUNCTION_INFO_V1(age_adjacency_debug_stats);
PG_FUNCTION_INFO_V1(age_adjacency_debug_directory_probe);
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
static void age_adjacency_append_item(Relation index_rel, uint16 page_type,
                                      const void *item, Size item_size,
                                      BlockNumber *inserted_blkno,
                                      OffsetNumber *inserted_offnum);
static void age_adjacency_append_posting(Relation index_rel,
                                         AgeAdjacencyPosting posting,
                                         uint16 page_type,
                                         BlockNumber *inserted_blkno,
                                         OffsetNumber *inserted_offnum);
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
static bool age_adjacency_find_directory_entry(Relation index_rel, graphid key,
                                               AgeAdjacencyDirectoryEntryData *entry_out);
static void age_adjacency_probe_directory(Relation index_rel, graphid key,
                                          bool *found,
                                          int64 *pages_visited,
                                          int64 *entries_scanned);
static int64 age_adjacency_emit_posting(AgeAdjacencyPosting posting,
                                        AgeAdjacencyScanTarget *target);
static bool age_adjacency_store_candidate(const AgeAdjacencyPayload *payload,
                                          void *callback_state);
static bool age_adjacency_store_candidate_row(const AgeAdjacencyPayload *payload,
                                              void *callback_state);
static int64 age_adjacency_scan_payload(Relation index_rel, graphid key,
                                        AgeAdjacencyScanTarget *target);
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
    opaque->next_blkno = InvalidBlockNumber;
    opaque->posting_count = 0;
    opaque->min_key = 0;
    opaque->max_key = 0;
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
                 errmsg("age_adjacency payload v3 indexes require exactly "
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
                     errmsg("age_adjacency payload v3 column %d must be graphid",
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
                     errmsg("age_adjacency payload v3 does not support null "
                            "columns")));
        }
    }

    posting->key = DATUM_GET_GRAPHID(values[0]);
    ItemPointerCopy(heap_tid, &posting->heap_tid);
    posting->edge_id = DATUM_GET_GRAPHID(values[1]);
    posting->next_vertex_id = DATUM_GET_GRAPHID(values[2]);
}

static void
age_adjacency_append_item(Relation index_rel, uint16 page_type,
                          const void *item, Size item_size,
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
            MAXALIGN(item_size) + sizeof(ItemIdData))
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

    if (page_type == AGE_ADJACENCY_PAGE_MAIN ||
        page_type == AGE_ADJACENCY_PAGE_DELTA)
    {
        AgeAdjacencyPosting posting = (AgeAdjacencyPosting) item;

        opaque->posting_count++;
        if (opaque->posting_count == 1)
        {
            opaque->min_key = posting->key;
            opaque->max_key = posting->key;
        }
        else
        {
            opaque->min_key = Min(opaque->min_key, posting->key);
            opaque->max_key = Max(opaque->max_key, posting->key);
        }
        meta->postings++;
        if (page_type == AGE_ADJACENCY_PAGE_DELTA)
        {
            meta->delta_postings++;
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
age_adjacency_append_posting(Relation index_rel, AgeAdjacencyPosting posting,
                             uint16 page_type, BlockNumber *inserted_blkno,
                             OffsetNumber *inserted_offnum)
{
    if (page_type != AGE_ADJACENCY_PAGE_MAIN &&
        page_type != AGE_ADJACENCY_PAGE_DELTA)
    {
        elog(ERROR, "unexpected age_adjacency posting page type %u",
             page_type);
    }

    age_adjacency_append_item(index_rel, page_type, posting,
                              sizeof(AgeAdjacencyPostingData),
                              inserted_blkno, inserted_offnum);
}

static void
age_adjacency_append_directory_entry(Relation index_rel,
                                     AgeAdjacencyDirectoryEntry entry)
{
    age_adjacency_append_item(index_rel, AGE_ADJACENCY_PAGE_DIRECTORY, entry,
                              sizeof(AgeAdjacencyDirectoryEntryData),
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
        AgeAdjacencyDirectoryEntryData entry;

        memset(&entry, 0, sizeof(entry));
        entry.key = buildstate->postings[i].key;

        while (i < buildstate->count &&
               buildstate->postings[i].key == entry.key)
        {
            BlockNumber blkno;
            OffsetNumber offnum;

            age_adjacency_append_posting(index_rel, &buildstate->postings[i],
                                         AGE_ADJACENCY_PAGE_MAIN,
                                         &blkno, &offnum);
            if (i == run_start)
            {
                entry.first_blkno = blkno;
                entry.first_offnum = offnum;
            }
            run_count++;
            i++;
        }

        if (run_count > PG_UINT32_MAX)
        {
            ereport(ERROR,
                    (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                     errmsg("too many age_adjacency postings for one key")));
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
    if (target != NULL && target->heap_rel != NULL)
    {
        if (!table_tuple_fetch_row_version(target->heap_rel,
                                           &posting->heap_tid,
                                           target->snapshot,
                                           target->slot))
        {
            return 0;
        }
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
        AgeAdjacencyPayload payload;

        ItemPointerCopy(&posting->heap_tid, &payload.heap_tid);
        payload.edge_id = posting->edge_id;
        payload.next_vertex_id = posting->next_vertex_id;
        payload.properties = (Datum) 0;
        payload.properties_isnull = true;

        if (target->slot != NULL)
        {
            payload.properties =
                slot_getattr(target->slot, Anum_ag_label_edge_table_properties,
                             &payload.properties_isnull);
        }

        if (!target->callback(&payload, target->callback_state))
        {
            return -1;
        }
    }

    return 1;
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
            AgeAdjacencyPosting posting;
            int64 emitted;

            remaining--;
            if (!ItemIdIsNormal(item_id))
            {
                continue;
            }

            posting = (AgeAdjacencyPosting) PageGetItem(page, item_id);
            if (posting->key != key)
            {
                continue;
            }

            emitted = age_adjacency_emit_posting(posting, target);
            if (emitted < 0)
            {
                UnlockReleaseBuffer(buf);
                return matches;
            }
            matches += emitted;
        }

        blkno = opaque->next_blkno;
        offnum = FirstOffsetNumber;
        UnlockReleaseBuffer(buf);
    }

    return matches;
}

static bool
age_adjacency_search_directory(Relation index_rel, graphid key,
                               AgeAdjacencyDirectoryEntryData *entry_out,
                               int64 *pages_visited,
                               int64 *entries_scanned)
{
    AgeAdjacencyMetaPageData meta;
    int64 low;
    int64 high;

    age_adjacency_read_meta(index_rel, &meta);
    if (!BlockNumberIsValid(meta.first_directory_blkno) ||
        !BlockNumberIsValid(meta.last_directory_blkno))
    {
        return false;
    }

    low = meta.first_directory_blkno;
    high = meta.last_directory_blkno;

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
age_adjacency_find_directory_entry(Relation index_rel, graphid key,
                                   AgeAdjacencyDirectoryEntryData *entry_out)
{
    return age_adjacency_search_directory(index_rel, key, entry_out, NULL,
                                          NULL);
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

static int64
age_adjacency_scan_payload(Relation index_rel, graphid key,
                           AgeAdjacencyScanTarget *target)
{
    AgeAdjacencyMetaPageData meta;
    AgeAdjacencyDirectoryEntryData entry;
    BlockNumber blkno;
    int64 matches = 0;

    age_adjacency_read_meta(index_rel, &meta);

    if (age_adjacency_find_directory_entry(index_rel, key, &entry))
    {
        matches += age_adjacency_scan_posting_run(index_rel,
                                                  entry.first_blkno,
                                                  entry.first_offnum,
                                                  entry.posting_count,
                                                  key, target);
    }

    blkno = meta.first_delta_blkno;
    while (BlockNumberIsValid(blkno))
    {
        Buffer buf;
        Page page;
        AgeAdjacencyPageOpaque opaque;
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

        maxoff = PageGetMaxOffsetNumber(page);
        for (offnum = FirstOffsetNumber;
             offnum <= maxoff;
             offnum = OffsetNumberNext(offnum))
        {
            ItemId item_id = PageGetItemId(page, offnum);
            AgeAdjacencyPosting posting;
            int64 emitted;

            if (!ItemIdIsNormal(item_id))
            {
                continue;
            }

            posting = (AgeAdjacencyPosting) PageGetItem(page, item_id);
            if (posting->key != key)
            {
                continue;
            }

            emitted = age_adjacency_emit_posting(posting, target);
            if (emitted < 0)
            {
                UnlockReleaseBuffer(buf);
                return matches;
            }
            matches += emitted;
        }

        blkno = opaque->next_blkno;
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
    age_adjacency_append_posting(index_rel, &posting,
                                 AGE_ADJACENCY_PAGE_DELTA, NULL, NULL);

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
            AgeAdjacencyPosting posting;

            if (!ItemIdIsNormal(item_id))
            {
                continue;
            }

            posting = (AgeAdjacencyPosting) PageGetItem(page, item_id);
            if (callback(&posting->heap_tid, callback_state))
            {
                PageIndexTupleDelete(page, (OffsetNumber) offnum);
                removed++;
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
    double tuples_per_page =
        AGE_ADJACENCY_ITEMS_PER_PAGE(sizeof(AgeAdjacencyPostingData));
    double directory_entries_per_page =
        AGE_ADJACENCY_ITEMS_PER_PAGE(sizeof(AgeAdjacencyDirectoryEntryData));
    AgeAdjacencyMetaPageData meta;
    BlockNumber actual_pages;
    bool have_meta = false;

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
    main_pages = Max(1.0, ceil(expected_tuples / tuples_per_page));
    delta_pages = delta_postings > 0 ?
                  ceil(delta_postings / tuples_per_page) : 0;

    *index_startup_cost = random_page_cost;
    *index_total_cost =
        *index_startup_cost +
        (directory_probe_pages + main_pages + delta_pages) * random_page_cost +
        (expected_tuples + delta_postings) * cpu_index_tuple_cost;

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

int64
age_adjacency_foreach_visible_payload(Oid index_oid, graphid key,
                                      Snapshot snapshot,
                                      AgeAdjacencyPayloadCallback callback,
                                      void *callback_state)
{
    Relation index_rel;
    Relation heap_rel;
    TupleTableSlot *slot;
    AgeAdjacencyMetaPageData meta;
    AgeAdjacencyScanTarget target;
    int64 matches;

    if (callback == NULL)
    {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("age_adjacency visible payload scan requires a "
                        "callback")));
    }

    index_rel = index_open(index_oid, AccessShareLock);
    age_adjacency_validate_index(index_rel);
    age_adjacency_read_meta(index_rel, &meta);

    heap_rel = relation_open(meta.heap_relid, AccessShareLock);
    slot = table_slot_create(heap_rel, NULL);

    memset(&target, 0, sizeof(target));
    target.heap_rel = heap_rel;
    target.snapshot = snapshot != NULL ? snapshot : GetActiveSnapshot();
    target.slot = slot;
    target.callback = callback;
    target.callback_state = callback_state;

    matches = age_adjacency_scan_payload(index_rel, key, &target);

    ExecDropSingleTupleTableSlot(slot);
    relation_close(heap_rel, AccessShareLock);
    index_close(index_rel, AccessShareLock);

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
    age_adjacency_read_meta(index_rel, &meta);

    values[0] = Int64GetDatum((int64) RelationGetNumberOfBlocks(index_rel));
    values[1] = Int64GetDatum((int64) meta.postings);
    values[2] = Int64GetDatum((int64) meta.directory_entries);
    values[3] = Int64GetDatum((int64) meta.delta_postings);
    values[4] = Int64GetDatum((int64) AGE_ADJACENCY_DELTA_REINDEX_THRESHOLD);
    values[5] = BoolGetDatum(meta.delta_postings >=
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
