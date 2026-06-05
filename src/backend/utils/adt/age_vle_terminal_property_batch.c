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

#include "access/genam.h"
#include "access/heapam.h"
#include "access/relation.h"
#include "access/table.h"
#include "catalog/ag_label.h"
#include "common/hashfn.h"
#include "executor/tuptable.h"
#include "utils/ag_cache.h"
#include "utils/agtype.h"
#include "utils/age_vle_terminal_property_batch.h"
#include "utils/graphid.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

typedef struct VLETerminalPropertyBatchEntry
{
    graphid terminal_id;
    Datum property;
    bool is_null;
    bool found;
} VLETerminalPropertyBatchEntry;

typedef struct VLETerminalLabelBatchEntry
{
    int32 label_id;
    int64 needed;
    graphid min_terminal_id;
    graphid max_terminal_id;
} VLETerminalLabelBatchEntry;

static void scan_terminal_property_label_batch(
    const VLETerminalPropertyBatchFetch *fetch, HTAB *terminal_map,
    VLETerminalLabelBatchEntry *label_entry);
static bool cache_batch_terminal_property_tuple(
    const VLETerminalPropertyBatchFetch *fetch, HTAB *terminal_map,
    Oid relid, HeapTuple tuple, TupleDesc tupdesc);

void age_vle_terminal_property_batch_reset(
    VLETerminalPropertyBatchState *batch)
{
    Assert(batch != NULL);

    pfree_if_not_null(batch->ids);
    pfree_if_not_null(batch->results);
    pfree_if_not_null(batch->nulls);
    memset(batch, 0, sizeof(*batch));
}

void age_vle_terminal_property_batch_fetch(
    VLETerminalPropertyBatchState *batch,
    const VLETerminalPropertyBatchFetch *fetch)
{
    HASHCTL ctl;
    HTAB *terminal_map;
    HTAB *label_map;
    HASH_SEQ_STATUS label_status;
    VLETerminalLabelBatchEntry *label_entry;
    int64 i;

    Assert(batch != NULL);
    Assert(fetch != NULL);
    Assert(fetch->property_key != NULL);

    MemSet(&ctl, 0, sizeof(ctl));
    ctl.keysize = sizeof(graphid);
    ctl.entrysize = sizeof(VLETerminalPropertyBatchEntry);
    ctl.hash = tag_hash;
    terminal_map = hash_create("VLE terminal property batch", 1024, &ctl,
                               HASH_ELEM | HASH_FUNCTION);

    MemSet(&ctl, 0, sizeof(ctl));
    ctl.keysize = sizeof(int32);
    ctl.entrysize = sizeof(VLETerminalLabelBatchEntry);
    ctl.hash = tag_hash;
    label_map = hash_create("VLE terminal property label batch", 16, &ctl,
                            HASH_ELEM | HASH_FUNCTION);

    for (i = 0; i < batch->count; i++)
    {
        VLETerminalPropertyBatchEntry *entry;
        bool found;
        graphid terminal_id = batch->ids[i];
        int32 label_id = get_graphid_label_id(terminal_id);

        entry = hash_search(terminal_map, &terminal_id, HASH_ENTER, &found);
        if (!found)
        {
            entry->terminal_id = terminal_id;
            entry->property = (Datum) 0;
            entry->is_null = true;
            entry->found = false;
        }

        if (found)
        {
            continue;
        }

        label_entry = hash_search(label_map, &label_id, HASH_ENTER, &found);
        if (!found)
        {
            label_entry->label_id = label_id;
            label_entry->needed = 1;
            label_entry->min_terminal_id = terminal_id;
            label_entry->max_terminal_id = terminal_id;
        }
        else
        {
            label_entry->needed++;
            if (terminal_id < label_entry->min_terminal_id)
            {
                label_entry->min_terminal_id = terminal_id;
            }
            if (terminal_id > label_entry->max_terminal_id)
            {
                label_entry->max_terminal_id = terminal_id;
            }
        }
    }

    hash_seq_init(&label_status, label_map);
    while ((label_entry = hash_seq_search(&label_status)) != NULL)
    {
        scan_terminal_property_label_batch(fetch, terminal_map,
                                           label_entry);
    }

    for (i = 0; i < batch->count; i++)
    {
        VLETerminalPropertyBatchEntry *entry;
        bool found;
        graphid terminal_id = batch->ids[i];

        entry = hash_search(terminal_map, &terminal_id, HASH_FIND, &found);
        if (found && entry->found && !entry->is_null)
        {
            batch->results[i] = entry->property;
            batch->nulls[i] = false;
        }
    }

    hash_destroy(label_map);
    hash_destroy(terminal_map);
}

static void scan_terminal_property_label_batch(
    const VLETerminalPropertyBatchFetch *fetch, HTAB *terminal_map,
    VLETerminalLabelBatchEntry *label_entry)
{
    label_cache_data *label_cache;
    Relation rel;
    TupleDesc tupdesc;
    Oid index_oid = InvalidOid;
    double label_tuples;
    double range_span;
    int64 remaining;

    Assert(fetch != NULL);
    Assert(terminal_map != NULL);
    Assert(label_entry != NULL);

    label_cache = search_label_graph_oid_cache_cached(
        fetch->graph_oid, label_entry->label_id);
    if (label_cache == NULL || !OidIsValid(label_cache->relation))
    {
        return;
    }

    rel = table_open(label_cache->relation, AccessShareLock);
    tupdesc = RelationGetDescr(rel);
    label_tuples = rel->rd_rel->reltuples;
    range_span = (double) (label_entry->max_terminal_id -
                           label_entry->min_terminal_id + 1);
    remaining = label_entry->needed;

    if (label_tuples > 0 && range_span < (label_tuples / 2.0))
    {
        index_oid = find_usable_btree_index_for_attr(
            rel, Anum_ag_label_vertex_table_id);
    }

    if (OidIsValid(index_oid))
    {
        Relation index_rel;
        IndexScanDesc scan;
        TupleTableSlot *slot;
        ScanKeyData scan_key[2];

        ScanKeyInit(&scan_key[0], Anum_ag_label_vertex_table_id,
                    BTGreaterEqualStrategyNumber, F_INT8GE,
                    GRAPHID_GET_DATUM(label_entry->min_terminal_id));
        ScanKeyInit(&scan_key[1], Anum_ag_label_vertex_table_id,
                    BTLessEqualStrategyNumber, F_INT8LE,
                    GRAPHID_GET_DATUM(label_entry->max_terminal_id));

        index_rel = index_open(index_oid, AccessShareLock);
        slot = table_slot_create(rel, NULL);
        scan = index_beginscan(rel, index_rel, GetActiveSnapshot(), NULL,
                               2, 0);
        index_rescan(scan, scan_key, 2, NULL, 0);
        while (remaining > 0 &&
               index_getnext_slot(scan, ForwardScanDirection, slot))
        {
            bool should_free;
            HeapTuple tuple;

            tuple = ExecFetchSlotHeapTuple(slot, true, &should_free);
            if (cache_batch_terminal_property_tuple(fetch, terminal_map,
                                                    RelationGetRelid(rel),
                                                    tuple, tupdesc))
            {
                remaining--;
            }
            if (should_free)
            {
                heap_freetuple(tuple);
            }
            ExecClearTuple(slot);
        }
        index_endscan(scan);
        ExecDropSingleTupleTableSlot(slot);
        index_close(index_rel, AccessShareLock);
    }
    else
    {
        TableScanDesc scan;
        HeapTuple tuple;

        scan = table_beginscan(rel, GetActiveSnapshot(), 0, NULL);
        while (remaining > 0 &&
               (tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
        {
            if (cache_batch_terminal_property_tuple(fetch, terminal_map,
                                                    RelationGetRelid(rel),
                                                    tuple, tupdesc))
            {
                remaining--;
            }
        }
        table_endscan(scan);
    }

    table_close(rel, AccessShareLock);
}

static bool cache_batch_terminal_property_tuple(
    const VLETerminalPropertyBatchFetch *fetch, HTAB *terminal_map,
    Oid relid, HeapTuple tuple, TupleDesc tupdesc)
{
    VLETerminalPropertyBatchEntry *entry;
    agtype_value property_value;
    bool property_value_needs_free = false;
    agtype *properties;
    Datum props;
    Datum vertex_id_datum;
    graphid vertex_id;
    bool found = false;
    bool isnull;

    Assert(fetch != NULL);
    Assert(fetch->ggctx != NULL);
    Assert(fetch->property_key != NULL);
    Assert(terminal_map != NULL);

    vertex_id_datum = heap_getattr(tuple, Anum_ag_label_vertex_table_id,
                                   tupdesc, &isnull);
    if (isnull)
    {
        return false;
    }
    vertex_id = DatumGetInt64(vertex_id_datum);
    entry = hash_search(terminal_map, &vertex_id, HASH_FIND, &found);
    if (!found || entry->found)
    {
        return false;
    }

    props = heap_getattr(tuple, Anum_ag_label_vertex_table_properties,
                         tupdesc, &isnull);
    if (isnull)
    {
        entry->found = true;
        entry->is_null = true;
        return true;
    }

    (void) cache_vertex_entry_tuple_scalar_property(
        fetch->ggctx, relid, tuple, tupdesc, fetch->property_key);

    properties = DATUM_GET_AGTYPE_P(props);
    if (find_agtype_value_from_container_no_copy(
            &properties->root, AGT_FOBJECT, fetch->property_key,
            &property_value, &property_value_needs_free) &&
        property_value.type != AGTV_NULL)
    {
        if (property_value.type == AGTV_INTEGER)
        {
            entry->property = PointerGetDatum(
                agtype_integer_to_agtype(property_value.val.int_value));
        }
        else
        {
            entry->property = PointerGetDatum(
                agtype_value_to_agtype(&property_value));
        }
        entry->is_null = false;
    }
    if (property_value_needs_free)
    {
        pfree_agtype_value_content(&property_value);
    }

    entry->found = true;
    return true;
}
