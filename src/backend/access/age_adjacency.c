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
#define AGE_ADJACENCY_VERSION 2
#define AGE_ADJACENCY_METAPAGE_BLKNO 0
#define AGE_ADJACENCY_EQUAL_STRATEGY 1
#define AGE_ADJACENCY_PAYLOAD_NATTS 3

#define AGE_ADJACENCY_PAGE_META 1
#define AGE_ADJACENCY_PAGE_DATA 2

typedef struct AgeAdjacencyMetaPageData
{
    uint32 magic;
    uint16 version;
    uint16 flags;
    Oid heap_relid;
    AttrNumber key_attno;
    BlockNumber first_data_blkno;
    BlockNumber last_data_blkno;
    uint64 postings;
} AgeAdjacencyMetaPageData;

typedef AgeAdjacencyMetaPageData *AgeAdjacencyMetaPage;

typedef struct AgeAdjacencyPageOpaqueData
{
    uint32 magic;
    uint16 version;
    uint16 page_type;
    BlockNumber next_blkno;
    uint32 posting_count;
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

typedef struct AgeAdjacencyBuildState
{
    double indtuples;
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

PG_FUNCTION_INFO_V1(age_adjacency_handler);
PG_FUNCTION_INFO_V1(age_adjacency_debug_payload);
PG_FUNCTION_INFO_V1(age_adjacency_debug_stats);

static void age_adjacency_init_metapage(Relation heap_rel,
                                        Relation index_rel);
static void age_adjacency_init_page(Page page, uint16 page_type);
static void age_adjacency_validate_index(Relation index_rel);
static void age_adjacency_check_metapage(Relation index_rel);
static void age_adjacency_read_meta(Relation index_rel,
                                    AgeAdjacencyMetaPageData *meta_out);
static AgeAdjacencyMetaPage age_adjacency_get_meta(Page page);
static Buffer age_adjacency_new_buffer(Relation index_rel);
static void age_adjacency_form_posting(Relation index_rel, Datum *values,
                                       bool *isnull, ItemPointer heap_tid,
                                       AgeAdjacencyPostingData *posting);
static void age_adjacency_append_posting(Relation index_rel,
                                         AgeAdjacencyPosting posting);
static void age_adjacency_build_callback(Relation index_rel, ItemPointer tid,
                                         Datum *values, bool *isnull,
                                         bool tuple_is_alive, void *state);
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
    meta.first_data_blkno = InvalidBlockNumber;
    meta.last_data_blkno = InvalidBlockNumber;
    meta.postings = 0;

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
                 errmsg("age_adjacency payload v2 indexes require exactly "
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
                     errmsg("age_adjacency payload v2 column %d must be graphid",
                            i + 1)));
        }
    }
}

static void
age_adjacency_check_metapage(Relation index_rel)
{
    AgeAdjacencyMetaPageData meta;

    age_adjacency_read_meta(index_rel, &meta);
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
                     errmsg("age_adjacency payload v2 does not support null "
                            "columns")));
        }
    }

    posting->key = DATUM_GET_GRAPHID(values[0]);
    ItemPointerCopy(heap_tid, &posting->heap_tid);
    posting->edge_id = DATUM_GET_GRAPHID(values[1]);
    posting->next_vertex_id = DATUM_GET_GRAPHID(values[2]);
}

static void
age_adjacency_append_posting(Relation index_rel, AgeAdjacencyPosting posting)
{
    Buffer metabuf;
    Buffer databuf = InvalidBuffer;
    Buffer oldbuf = InvalidBuffer;
    Page metapage;
    Page datapage;
    AgeAdjacencyMetaPage meta;
    AgeAdjacencyPageOpaque opaque;
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

    target_blkno = meta->last_data_blkno;
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
            MAXALIGN(sizeof(AgeAdjacencyPostingData)) + sizeof(ItemIdData))
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

            age_adjacency_init_page(newpage, AGE_ADJACENCY_PAGE_DATA);

            databuf = newbuf;
            datapage = newpage;
            target_blkno = BufferGetBlockNumber(databuf);
            meta->last_data_blkno = target_blkno;
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

        age_adjacency_init_page(datapage, AGE_ADJACENCY_PAGE_DATA);

        target_blkno = BufferGetBlockNumber(databuf);
        meta->first_data_blkno = target_blkno;
        meta->last_data_blkno = target_blkno;
        metadirty = true;
    }

    offnum = PageAddItem(datapage, (Item) posting,
                         sizeof(AgeAdjacencyPostingData),
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
        opaque->page_type != AGE_ADJACENCY_PAGE_DATA)
    {
        ereport(ERROR,
                (errcode(ERRCODE_INDEX_CORRUPTED),
                 errmsg("age_adjacency data page is invalid")));
    }
    opaque->posting_count++;

    meta->postings++;
    metadirty = true;
    datadirty = true;

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
    age_adjacency_append_posting(index_rel, &posting);
    buildstate->indtuples++;
}

static int64
age_adjacency_scan_payload(Relation index_rel, graphid key,
                           AgeAdjacencyScanTarget *target)
{
    BlockNumber nblocks;
    BlockNumber blkno;
    int64 matches = 0;

    age_adjacency_check_metapage(index_rel);
    nblocks = RelationGetNumberOfBlocks(index_rel);

    for (blkno = AGE_ADJACENCY_METAPAGE_BLKNO + 1; blkno < nblocks; blkno++)
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
            opaque->page_type != AGE_ADJACENCY_PAGE_DATA)
        {
            ereport(ERROR,
                    (errcode(ERRCODE_INDEX_CORRUPTED),
                     errmsg("age_adjacency data page %u is invalid", blkno)));
        }

        maxoff = PageGetMaxOffsetNumber(page);
        for (offnum = FirstOffsetNumber;
             offnum <= maxoff;
             offnum = OffsetNumberNext(offnum))
        {
            ItemId item_id = PageGetItemId(page, offnum);
            AgeAdjacencyPosting posting;

            if (!ItemIdIsNormal(item_id))
            {
                continue;
            }

            posting = (AgeAdjacencyPosting) PageGetItem(page, item_id);
            if (posting->key != key)
            {
                continue;
            }

            if (target != NULL && target->heap_rel != NULL)
            {
                if (!table_tuple_fetch_row_version(target->heap_rel,
                                                   &posting->heap_tid,
                                                   target->snapshot,
                                                   target->slot))
                {
                    continue;
                }
            }

            matches++;
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

                if (!target->callback(&payload, target->callback_state))
                {
                    UnlockReleaseBuffer(buf);
                    return matches;
                }
            }
        }

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
    age_adjacency_append_posting(index_rel, &posting);

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
            opaque->page_type != AGE_ADJACENCY_PAGE_DATA)
        {
            ereport(ERROR,
                    (errcode(ERRCODE_INDEX_CORRUPTED),
                     errmsg("age_adjacency data page %u is invalid", blkno)));
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

static void
age_adjacency_cost_estimate(struct PlannerInfo *root, struct IndexPath *path,
                            double loop_count, Cost *index_startup_cost,
                            Cost *index_total_cost,
                            Selectivity *index_selectivity,
                            double *index_correlation, double *index_pages)
{
    double pages = 1;
    double tuples = 1;

    (void) root;
    (void) loop_count;

    if (path != NULL && path->indexinfo != NULL)
    {
        pages = Max(path->indexinfo->pages, 1);
        tuples = Max(path->indexinfo->tuples, 1);
    }

    *index_startup_cost = 0;
    *index_total_cost = pages * seq_page_cost +
                        tuples * cpu_index_tuple_cost;
    *index_selectivity = Min(0.1, Max(1.0 / tuples, 0.0001));
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
age_adjacency_debug_stats(PG_FUNCTION_ARGS)
{
    Oid index_oid = PG_GETARG_OID(0);
    Relation index_rel;
    AgeAdjacencyMetaPageData meta;
    ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
    Datum values[2];
    bool nulls[2] = {false, false};

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
