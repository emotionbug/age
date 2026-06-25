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
#include "access/table.h"
#include "access/tableam.h"
#include "catalog/ag_label.h"
#include "catalog/namespace.h"
#include "catalog/pg_type_d.h"
#include "executor/cypher_adjacency_match_terminal.h"
#include "nodes/makefuncs.h"
#include "utils/ag_cache.h"
#include "utils/ag_func.h"
#include "utils/agtype.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/hsearch.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

typedef struct AgeAdjacencyMatchTerminalPropertyCacheEntry
{
    graphid vertex_id;
    bool matches;
} AgeAdjacencyMatchTerminalPropertyCacheEntry;

typedef struct AgeAdjacencyMatchTerminalPropertyScanKey
{
    Datum value;
    Oid eq_proc_oid;
} AgeAdjacencyMatchTerminalPropertyScanKey;

struct AgeAdjacencyMatchTerminalPropertyLookup
{
    bool active;
    Oid property_index_oid;
    MemoryContext context;
    Relation vertex_rel;
    Relation vertex_id_index_rel;
    TupleTableSlot *slot;
    Datum property_key;
    Datum property_value;
    bool property_value_isnull;
    HTAB *match_cache;
    graphid *property_index_vertex_ids;
    int64 property_index_vertex_count;
    int64 property_index_vertex_capacity;
    bool property_index_prefetched;
    bool property_index_available;
    bool property_index_prefetch_attempted;
    int64 property_index_prefetch_run_count;
    int64 property_index_prefetch_candidate_count;
    int64 property_index_prefetch_threshold;
    int64 property_index_prefetch_skipped_small;
    int64 property_index_matches;
    graphid property_index_min_vertex_id;
    graphid property_index_max_vertex_id;
    bool property_index_has_vertex_range;
    int64 cache_hits;
    int64 index_lookups;
};

static Oid terminal_property_agtype_eq_oid = InvalidOid;
static Oid terminal_property_int8_eq_oid = InvalidOid;
static Oid terminal_property_float8_eq_oid = InvalidOid;
static Oid terminal_property_numeric_eq_oid = InvalidOid;
static Oid terminal_property_text_eq_oid = InvalidOid;

static bool terminal_property_lookup_request_valid(
    const AgeAdjacencyMatchTerminalPropertyRequest *request);
static HTAB *create_terminal_property_match_cache(MemoryContext context);
static bool build_terminal_property_scan_key(
    AgeAdjacencyMatchTerminalPropertyLookup *lookup, Oid index_value_type,
    AgeAdjacencyMatchTerminalPropertyScanKey *scan_key);
static bool read_terminal_property_scalar(Datum property_value,
                                          agtype_value *scalar_value,
                                          bool *needs_free);
static bool convert_terminal_property_scan_value(
    agtype_value *scalar_value, Oid value_type, Datum *value);
static bool convert_terminal_property_string(
    agtype_value *scalar_value, Datum (*input_func)(FunctionCallInfo),
    Datum *value);
static Datum terminal_property_numeric_from_cstring(char *str);
static Datum terminal_property_text_from_cstring(char *str);
static Datum terminal_property_text_from_cstring_len(char *str, int len);
static Oid get_terminal_property_eq_oid(Oid value_type);
static bool prefetch_terminal_property_index_matches(
    AgeAdjacencyMatchTerminalPropertyLookup *lookup);
static bool terminal_property_index_lookup(
    AgeAdjacencyMatchTerminalPropertyLookup *lookup, graphid vertex_id);
static bool terminal_property_object_field_equals(Datum properties,
                                                  Datum key,
                                                  Datum value);
static bool terminal_property_cache_lookup(
    AgeAdjacencyMatchTerminalPropertyLookup *lookup, graphid vertex_id,
    bool *matches);
static void terminal_property_cache_store(
    AgeAdjacencyMatchTerminalPropertyLookup *lookup, graphid vertex_id,
    bool matches);
static void terminal_property_prefetch_append_vertex(
    AgeAdjacencyMatchTerminalPropertyLookup *lookup, graphid vertex_id);
static int terminal_property_vertex_id_cmp(const void *left,
                                           const void *right);
static Oid get_terminal_property_agtype_eq_oid(void);
static Oid lookup_terminal_property_eq_oid(Oid value_type, Oid *cache);

AgeAdjacencyMatchTerminalPropertyLookup *
age_adjacency_match_terminal_property_begin(
    const AgeAdjacencyMatchTerminalPropertyRequest *request,
    MemoryContext context)
{
    AgeAdjacencyMatchTerminalPropertyLookup *lookup;
    MemoryContext oldcontext;
    label_cache_data *label_cache;
    Oid index_oid;
    agtype_value *key_value;
    agtype *key_agtype;

    Assert(context != NULL);

    if (!terminal_property_lookup_request_valid(request))
        return NULL;

    label_cache = search_label_graph_oid_cache_cached(request->graph_oid,
                                                      request->right_label_id);
    if (label_cache == NULL || !OidIsValid(label_cache->relation))
        return NULL;

    oldcontext = MemoryContextSwitchTo(context);
    lookup = palloc0(sizeof(*lookup));
    lookup->property_index_oid = request->property_index_oid;
    lookup->context = context;
    lookup->vertex_rel = table_open(label_cache->relation, AccessShareLock);
    index_oid = find_usable_btree_index_for_attr(
        lookup->vertex_rel, Anum_ag_label_vertex_table_id);
    if (!OidIsValid(index_oid))
    {
        table_close(lookup->vertex_rel, AccessShareLock);
        pfree(lookup);
        MemoryContextSwitchTo(oldcontext);
        return NULL;
    }

    lookup->vertex_id_index_rel = index_open(index_oid, AccessShareLock);
    lookup->slot = table_slot_create(lookup->vertex_rel, NULL);
    key_value = string_to_agtype_value((char *)request->property_key);
    key_agtype = agtype_value_to_agtype(key_value);
    lookup->property_key = AGTYPE_P_GET_DATUM(key_agtype);
    lookup->property_value = (Datum)0;
    lookup->property_value_isnull = true;
    lookup->match_cache = create_terminal_property_match_cache(context);
    lookup->active = true;
    MemoryContextSwitchTo(oldcontext);

    return lookup;
}

void age_adjacency_match_terminal_property_end(
    AgeAdjacencyMatchTerminalPropertyLookup *lookup)
{
    if (lookup == NULL)
        return;

    if (lookup->slot != NULL)
    {
        ExecDropSingleTupleTableSlot(lookup->slot);
        lookup->slot = NULL;
    }
    if (lookup->vertex_id_index_rel != NULL)
    {
        index_close(lookup->vertex_id_index_rel, AccessShareLock);
        lookup->vertex_id_index_rel = NULL;
    }
    if (lookup->vertex_rel != NULL)
    {
        table_close(lookup->vertex_rel, AccessShareLock);
        lookup->vertex_rel = NULL;
    }
    if (lookup->match_cache != NULL)
    {
        hash_destroy(lookup->match_cache);
        lookup->match_cache = NULL;
    }
    if (lookup->property_index_vertex_ids != NULL)
    {
        pfree(lookup->property_index_vertex_ids);
        lookup->property_index_vertex_ids = NULL;
    }
    lookup->active = false;
    pfree(lookup);
}

void age_adjacency_match_terminal_property_rescan(
    AgeAdjacencyMatchTerminalPropertyLookup *lookup)
{
    if (lookup == NULL)
        return;
    if (lookup->slot != NULL)
        ExecClearTuple(lookup->slot);
}

void age_adjacency_match_terminal_property_set_value(
    AgeAdjacencyMatchTerminalPropertyLookup *lookup, Datum property_value,
    bool property_value_isnull)
{
    if (lookup == NULL)
        return;

    if (!lookup->property_value_isnull && !property_value_isnull &&
        datumIsEqual(lookup->property_value, property_value, false, -1))
        return;

    if (lookup->match_cache != NULL)
        hash_destroy(lookup->match_cache);
    if (lookup->property_index_vertex_ids != NULL)
        pfree(lookup->property_index_vertex_ids);
    lookup->match_cache = create_terminal_property_match_cache(
        lookup->context);
    lookup->property_index_vertex_ids = NULL;
    lookup->property_index_vertex_count = 0;
    lookup->property_index_vertex_capacity = 0;
    lookup->property_index_prefetched = false;
    lookup->property_index_available = false;
    lookup->property_index_prefetch_attempted = false;
    lookup->property_index_prefetch_run_count = 0;
    lookup->property_index_prefetch_candidate_count = 0;
    lookup->property_index_prefetch_threshold = 0;
    lookup->property_index_prefetch_skipped_small = 0;
    lookup->property_index_matches = 0;
    lookup->property_index_min_vertex_id = 0;
    lookup->property_index_max_vertex_id = 0;
    lookup->property_index_has_vertex_range = false;
    lookup->cache_hits = 0;
    lookup->index_lookups = 0;
    lookup->property_value = property_value;
    lookup->property_value_isnull = property_value_isnull;
}

bool age_adjacency_match_terminal_property_prepare_prefilter(
    AgeAdjacencyMatchTerminalPropertyLookup *lookup, int64 run_count,
    int64 candidate_count,
    int64 prefetch_threshold)
{
    if (!age_adjacency_match_terminal_property_active(lookup) ||
        lookup->property_value_isnull ||
        !OidIsValid(lookup->property_index_oid))
    {
        return false;
    }

    if (lookup->property_index_prefetched)
        return true;
    lookup->property_index_prefetch_run_count = run_count;
    lookup->property_index_prefetch_candidate_count = candidate_count;
    lookup->property_index_prefetch_threshold = prefetch_threshold;
    if (prefetch_threshold <= 0)
    {
        lookup->property_index_prefetch_attempted = true;
        return false;
    }
    if (candidate_count > 0 && candidate_count < prefetch_threshold)
    {
        lookup->property_index_prefetch_attempted = true;
        lookup->property_index_prefetch_skipped_small++;
        return false;
    }

    lookup->property_index_prefetch_attempted = true;
    lookup->property_index_available =
        prefetch_terminal_property_index_matches(lookup);

    return lookup->property_index_prefetched;
}

bool age_adjacency_match_terminal_property_active(
    const AgeAdjacencyMatchTerminalPropertyLookup *lookup)
{
    return lookup != NULL && lookup->active;
}

bool age_adjacency_match_terminal_property_matches(
    AgeAdjacencyMatchTerminalPropertyLookup *lookup, graphid vertex_id)
{
    bool matches;

    Assert(age_adjacency_match_terminal_property_active(lookup));

    if (lookup->property_value_isnull)
        return false;
    if (terminal_property_cache_lookup(lookup, vertex_id, &matches))
        return matches;
    if (lookup->property_index_prefetched)
        return false;

    matches = terminal_property_index_lookup(lookup, vertex_id);
    terminal_property_cache_store(lookup, vertex_id, matches);

    return matches;
}

bool age_adjacency_match_terminal_property_prefilter_active(
    const AgeAdjacencyMatchTerminalPropertyLookup *lookup)
{
    return age_adjacency_match_terminal_property_active(lookup) &&
        lookup->property_index_prefetched;
}

bool age_adjacency_match_terminal_property_prefilter_matches(
    graphid vertex_id, void *callback_state)
{
    AgeAdjacencyMatchTerminalPropertyLookup *lookup = callback_state;
    bool matches = false;

    Assert(age_adjacency_match_terminal_property_prefilter_active(lookup));

    if (terminal_property_cache_lookup(lookup, vertex_id, &matches))
        return matches;

    return false;
}

bool age_adjacency_match_terminal_property_prefilter_set(
    const AgeAdjacencyMatchTerminalPropertyLookup *lookup,
    AgeAdjacencyVertexSetFilter *filter)
{
    if (filter == NULL)
        return false;
    memset(filter, 0, sizeof(*filter));
    if (!age_adjacency_match_terminal_property_prefilter_active(lookup) ||
        lookup->match_cache == NULL)
    {
        return false;
    }

    filter->vertex_ids = lookup->match_cache;
    filter->sorted_vertex_ids = lookup->property_index_vertex_ids;
    filter->source = "property-index-prefetch";
    filter->matches = lookup->property_index_matches;
    filter->sorted_vertex_count = lookup->property_index_vertex_count;
    filter->min_vertex_id = lookup->property_index_min_vertex_id;
    filter->max_vertex_id = lookup->property_index_max_vertex_id;
    filter->has_range = lookup->property_index_has_vertex_range;
    filter->has_sorted_vertex_ids = lookup->property_index_vertex_count > 0;

    return true;
}

AgeAdjacencyMatchTerminalPropertyMode
age_adjacency_match_terminal_property_mode_id(
    const AgeAdjacencyMatchTerminalPropertyLookup *lookup)
{
    if (!age_adjacency_match_terminal_property_active(lookup))
        return AGE_ADJACENCY_TERMINAL_PROPERTY_NONE;
    if (lookup->property_index_available)
        return AGE_ADJACENCY_TERMINAL_PROPERTY_SOURCE_PREFETCH;
    if (OidIsValid(lookup->property_index_oid) &&
        !lookup->property_value_isnull &&
        !lookup->property_index_prefetch_attempted)
        return AGE_ADJACENCY_TERMINAL_PROPERTY_DEFERRED_PREFETCH;
    if (lookup->match_cache != NULL)
        return AGE_ADJACENCY_TERMINAL_PROPERTY_ID_CACHE;
    return AGE_ADJACENCY_TERMINAL_PROPERTY_ID_BTREE;
}

const char *age_adjacency_match_terminal_property_mode_name(
    AgeAdjacencyMatchTerminalPropertyMode mode)
{
    switch (mode)
    {
        case AGE_ADJACENCY_TERMINAL_PROPERTY_SOURCE_PREFETCH:
            return "property-index-prefetch";
        case AGE_ADJACENCY_TERMINAL_PROPERTY_DEFERRED_PREFETCH:
            return "deferred-prefetch";
        case AGE_ADJACENCY_TERMINAL_PROPERTY_ID_CACHE:
            return "id-btree-cache";
        case AGE_ADJACENCY_TERMINAL_PROPERTY_ID_BTREE:
            return "id-btree";
        case AGE_ADJACENCY_TERMINAL_PROPERTY_NONE:
            break;
    }

    return "none";
}

Oid age_adjacency_match_terminal_property_index_oid(
    const AgeAdjacencyMatchTerminalPropertyLookup *lookup)
{
    return lookup == NULL ? InvalidOid : lookup->property_index_oid;
}

uint32 age_adjacency_match_terminal_property_filter_id(
    const AgeAdjacencyMatchTerminalPropertyLookup *lookup)
{
    if (!age_adjacency_match_terminal_property_active(lookup) ||
        !OidIsValid(lookup->property_index_oid))
    {
        return 0;
    }

    return age_adjacency_property_filter_id(lookup->property_index_oid,
                                            lookup->property_value,
                                            lookup->property_value_isnull);
}

int64 age_adjacency_match_terminal_property_prefetched_matches(
    const AgeAdjacencyMatchTerminalPropertyLookup *lookup)
{
    return lookup == NULL ? 0 : lookup->property_index_matches;
}

int64 age_adjacency_match_terminal_property_cache_hits(
    const AgeAdjacencyMatchTerminalPropertyLookup *lookup)
{
    return lookup == NULL ? 0 : lookup->cache_hits;
}

int64 age_adjacency_match_terminal_property_index_lookups(
    const AgeAdjacencyMatchTerminalPropertyLookup *lookup)
{
    return lookup == NULL ? 0 : lookup->index_lookups;
}

int64 age_adjacency_match_terminal_property_prefetch_candidate_count(
    const AgeAdjacencyMatchTerminalPropertyLookup *lookup)
{
    return lookup == NULL ? 0 : lookup->property_index_prefetch_candidate_count;
}

int64 age_adjacency_match_terminal_property_prefetch_run_count(
    const AgeAdjacencyMatchTerminalPropertyLookup *lookup)
{
    return lookup == NULL ? 0 : lookup->property_index_prefetch_run_count;
}

int64 age_adjacency_match_terminal_property_prefetch_skipped_small(
    const AgeAdjacencyMatchTerminalPropertyLookup *lookup)
{
    return lookup == NULL ? 0 : lookup->property_index_prefetch_skipped_small;
}

int64 age_adjacency_match_terminal_property_prefetch_threshold(
    const AgeAdjacencyMatchTerminalPropertyLookup *lookup)
{
    return lookup == NULL ? 0 : lookup->property_index_prefetch_threshold;
}

static bool terminal_property_lookup_request_valid(
    const AgeAdjacencyMatchTerminalPropertyRequest *request)
{
    return request != NULL &&
        request->has_property_predicate &&
        request->metadata_backed &&
        label_id_is_valid(request->right_label_id) &&
        request->property_key != NULL;
}

static HTAB *create_terminal_property_match_cache(MemoryContext context)
{
    HASHCTL ctl;

    memset(&ctl, 0, sizeof(ctl));
    ctl.keysize = sizeof(graphid);
    ctl.entrysize = sizeof(AgeAdjacencyMatchTerminalPropertyCacheEntry);
    ctl.hcxt = context;

    return hash_create("AGE adjacency match terminal property cache",
                       1024, &ctl,
                       HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
}

static bool build_terminal_property_scan_key(
    AgeAdjacencyMatchTerminalPropertyLookup *lookup, Oid index_value_type,
    AgeAdjacencyMatchTerminalPropertyScanKey *scan_key)
{
    MemoryContext oldcontext;
    Oid value_type;
    Oid eq_proc_oid;
    agtype_value scalar_value;
    bool scalar_needs_free = false;
    Datum value = (Datum)0;
    bool conversion_ok = true;

    value_type = getBaseType(index_value_type);
    eq_proc_oid = get_terminal_property_eq_oid(value_type);
    if (!OidIsValid(eq_proc_oid))
        return false;

    oldcontext = CurrentMemoryContext;
    PG_TRY();
    {
        if (value_type == AGTYPEOID)
        {
            value = lookup->property_value;
        }
        else if (!read_terminal_property_scalar(lookup->property_value,
                                                &scalar_value,
                                                &scalar_needs_free))
        {
            conversion_ok = false;
        }
        else if (!convert_terminal_property_scan_value(&scalar_value,
                                                       value_type, &value))
        {
            conversion_ok = false;
        }

        if (scalar_needs_free)
            pfree_agtype_value_content(&scalar_value);
    }
    PG_CATCH();
    {
        MemoryContextSwitchTo(oldcontext);
        FlushErrorState();
        conversion_ok = false;
    }
    PG_END_TRY();

    if (!conversion_ok)
        return false;

    scan_key->value = value;
    scan_key->eq_proc_oid = eq_proc_oid;

    return true;
}

static bool read_terminal_property_scalar(Datum property_value,
                                          agtype_value *scalar_value,
                                          bool *needs_free)
{
    agtype *agt;

    agt = DATUM_GET_AGTYPE_P(property_value);
    if (!AGT_ROOT_IS_SCALAR(agt))
        return false;

    if (!get_ith_agtype_value_from_container_no_copy(&agt->root, 0,
                                                     scalar_value,
                                                     needs_free))
        return false;

    return scalar_value->type != AGTV_NULL;
}

static bool convert_terminal_property_scan_value(
    agtype_value *scalar_value, Oid value_type, Datum *value)
{
    char *str;

    if (value_type == INT8OID)
    {
        if (scalar_value->type == AGTV_INTEGER)
            *value = Int64GetDatum(scalar_value->val.int_value);
        else if (scalar_value->type == AGTV_FLOAT)
            *value = DirectFunctionCall1(dtoi8,
                                         Float8GetDatum(
                                             scalar_value->val.float_value));
        else if (scalar_value->type == AGTV_NUMERIC)
            *value = DirectFunctionCall1(numeric_int8,
                                         NumericGetDatum(
                                             scalar_value->val.numeric));
        else if (scalar_value->type == AGTV_BOOL)
            *value = Int64GetDatum(scalar_value->val.boolean ? 1 : 0);
        else if (!convert_terminal_property_string(scalar_value, int8in,
                                                   value))
            return false;
        return true;
    }

    if (value_type == FLOAT8OID)
    {
        if (scalar_value->type == AGTV_FLOAT)
            *value = Float8GetDatum(scalar_value->val.float_value);
        else if (scalar_value->type == AGTV_INTEGER)
            *value = Float8GetDatum((float8)scalar_value->val.int_value);
        else if (scalar_value->type == AGTV_NUMERIC)
            *value = DirectFunctionCall1(numeric_float8,
                                         NumericGetDatum(
                                             scalar_value->val.numeric));
        else if (!convert_terminal_property_string(scalar_value, float8in,
                                                   value))
            return false;
        return true;
    }

    if (value_type == NUMERICOID)
    {
        if (scalar_value->type == AGTV_NUMERIC)
            *value = NumericGetDatum(DatumGetNumericCopy(
                NumericGetDatum(scalar_value->val.numeric)));
        else if (scalar_value->type == AGTV_INTEGER)
            *value = DirectFunctionCall1(int8_numeric,
                                         Int64GetDatum(
                                             scalar_value->val.int_value));
        else if (scalar_value->type == AGTV_FLOAT)
            *value = DirectFunctionCall1(float8_numeric,
                                         Float8GetDatum(
                                             scalar_value->val.float_value));
        else if (scalar_value->type == AGTV_STRING)
        {
            str = pnstrdup(scalar_value->val.string.val,
                           scalar_value->val.string.len);
            *value = terminal_property_numeric_from_cstring(str);
            pfree(str);
        }
        else
            return false;
        return true;
    }

    if (value_type == TEXTOID)
    {
        if (scalar_value->type == AGTV_STRING)
            *value = terminal_property_text_from_cstring_len(
                scalar_value->val.string.val, scalar_value->val.string.len);
        else if (scalar_value->type == AGTV_INTEGER)
        {
            str = DatumGetCString(DirectFunctionCall1(
                int8out, Int64GetDatum(scalar_value->val.int_value)));
            *value = terminal_property_text_from_cstring(str);
        }
        else if (scalar_value->type == AGTV_FLOAT)
        {
            str = DatumGetCString(DirectFunctionCall1(
                float8out, Float8GetDatum(scalar_value->val.float_value)));
            *value = terminal_property_text_from_cstring(str);
        }
        else if (scalar_value->type == AGTV_NUMERIC)
        {
            str = DatumGetCString(DirectFunctionCall1(
                numeric_out, NumericGetDatum(scalar_value->val.numeric)));
            *value = terminal_property_text_from_cstring(str);
        }
        else if (scalar_value->type == AGTV_BOOL)
        {
            str = DatumGetCString(DirectFunctionCall1(
                boolout, BoolGetDatum(scalar_value->val.boolean)));
            *value = terminal_property_text_from_cstring(str);
        }
        else
            return false;
        return true;
    }

    return false;
}

static bool convert_terminal_property_string(
    agtype_value *scalar_value, Datum (*input_func)(FunctionCallInfo),
    Datum *value)
{
    char *str;

    if (scalar_value->type != AGTV_STRING)
        return false;

    str = pnstrdup(scalar_value->val.string.val,
                   scalar_value->val.string.len);
    *value = DirectFunctionCall1(input_func, CStringGetDatum(str));
    pfree(str);

    return true;
}

static Datum terminal_property_numeric_from_cstring(char *str)
{
    return DirectFunctionCall3(numeric_in,
                               CStringGetDatum(str),
                               ObjectIdGetDatum(InvalidOid),
                               Int32GetDatum(-1));
}

static Datum terminal_property_text_from_cstring(char *str)
{
    return CStringGetTextDatum(str);
}

static Datum terminal_property_text_from_cstring_len(char *str, int len)
{
    return PointerGetDatum(cstring_to_text_with_len(str, len));
}

static Oid get_terminal_property_eq_oid(Oid value_type)
{
    if (value_type == AGTYPEOID)
        return get_terminal_property_agtype_eq_oid();
    if (value_type == INT8OID)
        return lookup_terminal_property_eq_oid(value_type,
                                               &terminal_property_int8_eq_oid);
    if (value_type == FLOAT8OID)
        return lookup_terminal_property_eq_oid(
            value_type, &terminal_property_float8_eq_oid);
    if (value_type == NUMERICOID)
        return lookup_terminal_property_eq_oid(
            value_type, &terminal_property_numeric_eq_oid);
    if (value_type == TEXTOID)
        return lookup_terminal_property_eq_oid(value_type,
                                               &terminal_property_text_eq_oid);

    return InvalidOid;
}

static bool prefetch_terminal_property_index_matches(
    AgeAdjacencyMatchTerminalPropertyLookup *lookup)
{
    Relation property_index_rel;
    TupleDesc index_desc;
    Oid index_value_type;
    AgeAdjacencyMatchTerminalPropertyScanKey property_scan_key;
    ScanKeyData scan_key;
    IndexScanDesc scan_desc;

    if (!OidIsValid(lookup->property_index_oid) ||
        !SearchSysCacheExists1(RELOID,
                               ObjectIdGetDatum(lookup->property_index_oid)))
        return false;

    property_index_rel = index_open(lookup->property_index_oid,
                                    AccessShareLock);
    index_desc = RelationGetDescr(property_index_rel);
    if (property_index_rel->rd_index == NULL ||
        !property_index_rel->rd_index->indisvalid ||
        !property_index_rel->rd_index->indisready ||
        index_desc->natts < 1)
    {
        index_close(property_index_rel, AccessShareLock);
        return false;
    }

    index_value_type = TupleDescAttr(index_desc, 0)->atttypid;
    if (!build_terminal_property_scan_key(lookup, index_value_type,
                                          &property_scan_key))
    {
        index_close(property_index_rel, AccessShareLock);
        return false;
    }

    ScanKeyInit(&scan_key, 1, BTEqualStrategyNumber,
                property_scan_key.eq_proc_oid,
                property_scan_key.value);
    scan_desc = index_beginscan(lookup->vertex_rel, property_index_rel,
                                GetActiveSnapshot(), NULL, 1, 0);
    index_rescan(scan_desc, &scan_key, 1, NULL, 0);
    while (index_getnext_slot(scan_desc, ForwardScanDirection, lookup->slot))
    {
        Datum id;
        bool isnull;

        id = slot_getattr(lookup->slot, Anum_ag_label_vertex_table_id,
                          &isnull);
        if (!isnull)
        {
            graphid vertex_id;

            vertex_id = DATUM_GET_GRAPHID(id);
            terminal_property_cache_store(lookup, vertex_id, true);
            terminal_property_prefetch_append_vertex(lookup, vertex_id);
            if (!lookup->property_index_has_vertex_range)
            {
                lookup->property_index_min_vertex_id = vertex_id;
                lookup->property_index_max_vertex_id = vertex_id;
                lookup->property_index_has_vertex_range = true;
            }
            else
            {
                lookup->property_index_min_vertex_id =
                    Min(lookup->property_index_min_vertex_id, vertex_id);
                lookup->property_index_max_vertex_id =
                    Max(lookup->property_index_max_vertex_id, vertex_id);
            }
            lookup->property_index_matches++;
        }
        ExecClearTuple(lookup->slot);
    }
    index_endscan(scan_desc);
    index_close(property_index_rel, AccessShareLock);
    ExecClearTuple(lookup->slot);
    if (lookup->property_index_vertex_count > 1)
        qsort(lookup->property_index_vertex_ids,
              lookup->property_index_vertex_count, sizeof(graphid),
              terminal_property_vertex_id_cmp);

    lookup->property_index_prefetched = true;
    return true;
}

static bool terminal_property_index_lookup(
    AgeAdjacencyMatchTerminalPropertyLookup *lookup, graphid vertex_id)
{
    ScanKeyData scan_key;
    IndexScanDesc scan_desc;
    Datum properties;
    bool isnull;
    bool found;
    bool matches = false;

    Assert(lookup->vertex_rel != NULL);
    Assert(lookup->vertex_id_index_rel != NULL);
    Assert(lookup->slot != NULL);

    lookup->index_lookups++;
    ExecClearTuple(lookup->slot);
    ScanKeyInit(&scan_key, Anum_ag_label_vertex_table_id,
                BTEqualStrategyNumber, F_GRAPHIDEQ,
                GRAPHID_GET_DATUM(vertex_id));
    scan_desc = index_beginscan(lookup->vertex_rel,
                                lookup->vertex_id_index_rel,
                                GetActiveSnapshot(), NULL, 1, 0);
    index_rescan(scan_desc, &scan_key, 1, NULL, 0);
    found = index_getnext_slot(scan_desc, ForwardScanDirection,
                               lookup->slot);
    if (found)
    {
        properties = slot_getattr(lookup->slot,
                                  Anum_ag_label_vertex_table_properties,
                                  &isnull);
        if (!isnull)
        {
            matches = terminal_property_object_field_equals(
                properties, lookup->property_key, lookup->property_value);
        }
    }
    index_endscan(scan_desc);
    ExecClearTuple(lookup->slot);

    return matches;
}

static bool terminal_property_object_field_equals(Datum properties,
                                                  Datum key,
                                                  Datum value)
{
    agtype *properties_agtype = DATUM_GET_AGTYPE_P(properties);
    agtype *key_agtype = DATUM_GET_AGTYPE_P(key);
    agtype *value_agtype = DATUM_GET_AGTYPE_P(value);
    agtype_value key_value;
    agtype_value lhs_value;
    bool key_needs_free = false;
    bool lhs_needs_free = false;
    int cmp;

    if (!AGT_ROOT_IS_OBJECT(properties_agtype) ||
        !AGT_ROOT_IS_SCALAR(key_agtype))
        return false;

    if (!get_ith_agtype_value_from_container_no_copy(&key_agtype->root, 0,
                                                     &key_value,
                                                     &key_needs_free))
        return false;
    if (key_value.type != AGTV_STRING)
    {
        if (key_needs_free)
            pfree_agtype_value_content(&key_value);
        return false;
    }

    if (!find_agtype_value_from_container_no_copy(&properties_agtype->root,
                                                  AGT_FOBJECT, &key_value,
                                                  &lhs_value,
                                                  &lhs_needs_free) ||
        lhs_value.type == AGTV_NULL)
    {
        if (key_needs_free)
            pfree_agtype_value_content(&key_value);
        return false;
    }

    if (IS_A_AGTYPE_SCALAR(&lhs_value) && AGT_ROOT_IS_SCALAR(value_agtype))
    {
        agtype_value rhs_value;
        bool rhs_needs_free = false;

        if (!get_ith_agtype_value_from_container_no_copy(&value_agtype->root,
                                                         0, &rhs_value,
                                                         &rhs_needs_free))
        {
            if (lhs_needs_free)
                pfree_agtype_value_content(&lhs_value);
            if (key_needs_free)
                pfree_agtype_value_content(&key_value);
            return false;
        }

        if (lhs_value.type == rhs_value.type)
        {
            cmp = compare_agtype_scalar_values(&lhs_value, &rhs_value);
        }
        else
        {
            agtype *lhs_agtype;

            lhs_agtype = agtype_value_to_agtype(&lhs_value);
            cmp = compare_agtype_containers_orderability(&lhs_agtype->root,
                                                         &value_agtype->root);
        }
        if (rhs_needs_free)
            pfree_agtype_value_content(&rhs_value);
    }
    else
    {
        agtype *lhs_agtype;

        lhs_agtype = agtype_value_to_agtype(&lhs_value);
        cmp = compare_agtype_containers_orderability(&lhs_agtype->root,
                                                     &value_agtype->root);
    }

    if (lhs_needs_free)
        pfree_agtype_value_content(&lhs_value);
    if (key_needs_free)
        pfree_agtype_value_content(&key_value);

    return cmp == 0;
}

static bool terminal_property_cache_lookup(
    AgeAdjacencyMatchTerminalPropertyLookup *lookup, graphid vertex_id,
    bool *matches)
{
    AgeAdjacencyMatchTerminalPropertyCacheEntry *entry;

    if (lookup->match_cache == NULL)
        return false;

    entry = hash_search(lookup->match_cache, &vertex_id, HASH_FIND, NULL);
    if (entry == NULL)
        return false;

    lookup->cache_hits++;
    *matches = entry->matches;
    return true;
}

static void terminal_property_cache_store(
    AgeAdjacencyMatchTerminalPropertyLookup *lookup, graphid vertex_id,
    bool matches)
{
    AgeAdjacencyMatchTerminalPropertyCacheEntry *entry;
    bool found;

    if (lookup->match_cache == NULL)
        return;

    entry = hash_search(lookup->match_cache, &vertex_id, HASH_ENTER, &found);
    if (!found)
        entry->vertex_id = vertex_id;
    entry->matches = matches;
}

static void
terminal_property_prefetch_append_vertex(
    AgeAdjacencyMatchTerminalPropertyLookup *lookup, graphid vertex_id)
{
    MemoryContext oldcontext;

    if (lookup->property_index_vertex_count >=
        lookup->property_index_vertex_capacity)
    {
        int64 new_capacity;

        new_capacity = lookup->property_index_vertex_capacity == 0 ?
            16 : lookup->property_index_vertex_capacity * 2;
        oldcontext = MemoryContextSwitchTo(lookup->context);
        if (lookup->property_index_vertex_ids == NULL)
            lookup->property_index_vertex_ids =
                palloc(sizeof(graphid) * new_capacity);
        else
            lookup->property_index_vertex_ids =
                repalloc(lookup->property_index_vertex_ids,
                         sizeof(graphid) * new_capacity);
        MemoryContextSwitchTo(oldcontext);
        lookup->property_index_vertex_capacity = new_capacity;
    }

    lookup->property_index_vertex_ids[
        lookup->property_index_vertex_count++] = vertex_id;
}

static int
terminal_property_vertex_id_cmp(const void *left, const void *right)
{
    graphid left_id = *((const graphid *)left);
    graphid right_id = *((const graphid *)right);

    if (left_id < right_id)
        return -1;
    if (left_id > right_id)
        return 1;
    return 0;
}

static Oid get_terminal_property_agtype_eq_oid(void)
{
    if (!OidIsValid(terminal_property_agtype_eq_oid))
        terminal_property_agtype_eq_oid =
            get_ag_func_oid("agtype_eq", 2, AGTYPEOID, AGTYPEOID);

    return terminal_property_agtype_eq_oid;
}

static Oid lookup_terminal_property_eq_oid(Oid value_type, Oid *cache)
{
    Oid op_oid;

    if (OidIsValid(*cache))
        return *cache;

    op_oid = OpernameGetOprid(list_make1(makeString("=")), value_type,
                              value_type);
    if (!OidIsValid(op_oid))
        return InvalidOid;

    *cache = get_opcode(op_oid);

    return *cache;
}
