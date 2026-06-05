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

#include "utils/age_vle_materializer_cache.h"

typedef struct VLEMaterializerObjectCacheEntry
{
    graphid id;
    agtype *object;
} VLEMaterializerObjectCacheEntry;

struct VLEMaterializerObjectCache
{
    Oid graph_oid;
    MemoryContext context;
    HTAB *vertices;
    HTAB *edges;
    HTAB *typed_vertices;
    HTAB *typed_edges;
};

static agtype *copy_agtype_to_context(agtype *source, MemoryContext context);
static agtype *age_vle_materializer_cache_get_object(
    VLEMaterializerObjectCache *object_cache, HTAB *objects,
    const VLEMaterializerHandoff *handoff, graphid id);
static void age_vle_materializer_cache_seed_vertex_candidate(
    VLEMaterializerObjectCache *object_cache, HTAB *objects,
    const VLEMaterializerHandoff *handoff, graphid requested_id);

static agtype *copy_agtype_to_context(agtype *source, MemoryContext context)
{
    agtype *copy;
    Size size;

    size = VARSIZE(source);
    copy = MemoryContextAlloc(context, size);
    memcpy(copy, source, size);

    return copy;
}

VLEMaterializerObjectCache *age_vle_materializer_object_cache_get(
    FunctionCallInfo fcinfo, Oid graph_oid)
{
    VLEMaterializerObjectCache *cache;
    HASHCTL hash_ctl;
    MemoryContext old_context;

    cache = (VLEMaterializerObjectCache *) fcinfo->flinfo->fn_extra;
    if (cache != NULL && cache->graph_oid == graph_oid)
        return cache;

    old_context = MemoryContextSwitchTo(fcinfo->flinfo->fn_mcxt);
    cache = MemoryContextAllocZero(fcinfo->flinfo->fn_mcxt, sizeof(*cache));
    cache->graph_oid = graph_oid;
    cache->context = fcinfo->flinfo->fn_mcxt;

    memset(&hash_ctl, 0, sizeof(hash_ctl));
    hash_ctl.keysize = sizeof(graphid);
    hash_ctl.entrysize = sizeof(VLEMaterializerObjectCacheEntry);
    hash_ctl.hcxt = fcinfo->flinfo->fn_mcxt;
    cache->vertices = hash_create("VLE materialized vertex object cache", 128,
                                  &hash_ctl,
                                  HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
    cache->edges = hash_create("VLE materialized edge object cache", 128,
                               &hash_ctl,
                               HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
    cache->typed_vertices =
        hash_create("VLE materialized typed vertex cache", 128,
                    &hash_ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
    cache->typed_edges =
        hash_create("VLE materialized typed edge cache", 128,
                    &hash_ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
    fcinfo->flinfo->fn_extra = cache;
    MemoryContextSwitchTo(old_context);

    return cache;
}

void age_vle_materializer_cache_prefetch_vertices(
    const VLEMaterializerHandoff *handoff, const graphid *vertex_ids,
    int64 nvertex_ids)
{
    HTAB *relation_cache;

    Assert(handoff != NULL);
    Assert(handoff->ggctx != NULL);

    if (nvertex_ids <= 1 || handoff->relation_cache == NULL)
        return;

    relation_cache = handoff->relation_cache;
    (void) prefetch_vertex_entry_properties_by_ids(
        handoff->ggctx, vertex_ids, nvertex_ids, &relation_cache,
        "vle materializer vertex property prefetch relation cache");
}

static agtype *age_vle_materializer_cache_get_object(
    VLEMaterializerObjectCache *object_cache, HTAB *objects,
    const VLEMaterializerHandoff *handoff, graphid id)
{
    VLEMaterializerObjectCacheEntry *entry;
    agtype *object;
    bool found;

    Assert(handoff != NULL);
    Assert(handoff->ggctx != NULL);
    Assert(handoff->build_object != NULL);
    Assert(handoff->output_requirement != VLE_MATERIALIZER_OUTPUT_UNKNOWN);

    if (object_cache == NULL)
        return handoff->build_object(handoff, id);

    entry = hash_search(objects, &id, HASH_ENTER, &found);
    if (found)
        return entry->object;

    object = handoff->build_object(handoff, id);
    entry->object = copy_agtype_to_context(object, object_cache->context);
    pfree(object);

    return entry->object;
}

static void age_vle_materializer_cache_seed_vertex_candidate(
    VLEMaterializerObjectCache *object_cache, HTAB *objects,
    const VLEMaterializerHandoff *handoff, graphid requested_id)
{
    VLEMaterializerObjectCacheEntry *entry;
    vertex_entry *candidate;
    Datum cached_properties;
    agtype *object;
    bool found;

    if (object_cache == NULL || !handoff->candidate_vertex_valid ||
        handoff->candidate_vertex_id == requested_id)
    {
        return;
    }

    candidate = get_vertex_entry(handoff->ggctx, handoff->candidate_vertex_id);
    if (candidate == NULL ||
        !get_vertex_entry_cached_properties(candidate, &cached_properties))
    {
        return;
    }

    entry = hash_search(objects, &handoff->candidate_vertex_id, HASH_ENTER,
                        &found);
    if (found)
    {
        return;
    }

    object = handoff->build_object(handoff, handoff->candidate_vertex_id);
    entry->object = copy_agtype_to_context(object, object_cache->context);
    pfree(object);
}

agtype *age_vle_materializer_cache_get_vertex_object(
    VLEMaterializerObjectCache *object_cache,
    const VLEMaterializerHandoff *handoff, graphid vertex_id)
{
    age_vle_materializer_cache_seed_vertex_candidate(
        object_cache, object_cache == NULL ? NULL : object_cache->vertices,
        handoff, vertex_id);

    return age_vle_materializer_cache_get_object(
        object_cache, object_cache == NULL ? NULL : object_cache->vertices,
        handoff, vertex_id);
}

agtype *age_vle_materializer_cache_get_edge_object(
    VLEMaterializerObjectCache *object_cache,
    const VLEMaterializerHandoff *handoff, graphid edge_id)
{
    return age_vle_materializer_cache_get_object(
        object_cache, object_cache == NULL ? NULL : object_cache->edges,
        handoff, edge_id);
}

agtype *age_vle_materializer_cache_get_typed_vertex(
    VLEMaterializerObjectCache *object_cache,
    const VLEMaterializerHandoff *handoff, graphid vertex_id)
{
    age_vle_materializer_cache_seed_vertex_candidate(
        object_cache,
        object_cache == NULL ? NULL : object_cache->typed_vertices,
        handoff, vertex_id);

    return age_vle_materializer_cache_get_object(
        object_cache,
        object_cache == NULL ? NULL : object_cache->typed_vertices,
        handoff, vertex_id);
}

agtype *age_vle_materializer_cache_get_typed_edge(
    VLEMaterializerObjectCache *object_cache,
    const VLEMaterializerHandoff *handoff, graphid edge_id)
{
    return age_vle_materializer_cache_get_object(
        object_cache, object_cache == NULL ? NULL : object_cache->typed_edges,
        handoff, edge_id);
}
