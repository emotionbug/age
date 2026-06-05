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

#include "utils/age_global_graph.h"
#include "utils/age_vle_terminal_output.h"
#include "utils/hsearch.h"

typedef struct VLEPrefetchedTerminalBlock
{
    Oid relid;
    BlockNumber blockno;
} VLEPrefetchedTerminalBlock;

static Datum build_vle_terminal_property(
    VLE_local_context *vlelctx, const VLETerminalPropertyLookup *lookup,
    bool *is_null);
static Datum get_vle_terminal_vertex_property(
    VLE_local_context *vlelctx, const VLETerminalPropertyLookup *lookup,
    graphid terminal_id, bool *is_null);
static Datum build_vle_terminal_properties(VLE_local_context *vlelctx,
                                           bool *is_null);
static Datum get_vle_terminal_vertex_properties(
    VLE_local_context *vlelctx, graphid terminal_id, bool *is_null);
static void cache_terminal_property_result(VLE_local_context *vlelctx,
                                           graphid terminal_id);
static void cache_direct_terminal_property_result(
    VLE_local_context *vlelctx, const VLETerminalPropertyLookup *lookup,
    graphid terminal_id);
static void cache_direct_terminal_property_result_for_entry(
    VLE_local_context *vlelctx, const VLETerminalPropertyLookup *lookup,
    vertex_entry *ve);
static Datum get_terminal_property_for_entry(
    VLE_local_context *vlelctx, const VLETerminalPropertyLookup *lookup,
    vertex_entry *ve, bool *is_null);
static bool cache_terminal_property_char_hit_for_entry(
    VLE_local_context *vlelctx, const VLETerminalPropertyLookup *lookup,
    vertex_entry *ve);
static bool get_cached_terminal_property_result(
    const VLETerminalPropertyLookup *lookup, vertex_entry *ve,
    Datum *property);
static bool is_terminal_property_block_prefetched(
    const VLETerminalPropertyLookup *lookup, Oid relid, BlockNumber blockno);
static void mark_terminal_property_block_prefetched(
    const VLETerminalPropertyLookup *lookup, Oid relid, BlockNumber blockno);

bool age_vle_terminal_output_uses_direct_dfs(
    const VLETerminalOutputPolicy *policy)
{
    Assert(policy != NULL);

    return policy->direct_property;
}

void age_vle_terminal_output_cache_result(
    VLE_local_context *vlelctx, const VLETerminalOutputPolicy *policy,
    const VLETraversalStep *step)
{
    Assert(vlelctx != NULL);
    Assert(policy != NULL);
    Assert(step != NULL);

    if (policy->direct_property)
    {
        if (step->vertex_entry != NULL)
        {
            if (policy->char_fast_path &&
                cache_terminal_property_char_hit_for_entry(vlelctx,
                                                           &policy->lookup,
                                                           step->vertex_entry))
            {
                return;
            }

            cache_direct_terminal_property_result_for_entry(vlelctx,
                                                            &policy->lookup,
                                                            step->vertex_entry);
        }
        else
        {
            cache_direct_terminal_property_result(vlelctx, &policy->lookup,
                                                  step->vertex_id);
        }
        return;
    }

    cache_terminal_property_result(vlelctx, step->vertex_id);
}

bool age_vle_terminal_output_emit_property(
    VLE_local_context *vlelctx, const VLETerminalOutputPolicy *policy,
    FuncCallContext *funcctx, bool is_zero_bound,
    const VLEIteratorOutputTarget *target)
{
    bool property_is_null = true;
    Datum property;

    Assert(vlelctx != NULL);
    Assert(policy != NULL);
    Assert(target != NULL);
    Assert(policy->emit_property);

    if (!is_zero_bound && policy->direct_property)
    {
        age_vle_context_get_terminal_property_result(
            vlelctx, &property, &property_is_null);
    }
    else
    {
        MemoryContext terminal_oldctx;

        terminal_oldctx =
            MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);
        property = build_vle_terminal_property(vlelctx, &policy->lookup,
                                               &property_is_null);
        MemoryContextSwitchTo(terminal_oldctx);
    }

    if (property_is_null)
    {
        age_vle_iterator_output_target_set_null(target);
        return true;
    }
    age_vle_iterator_output_target_set_datum(target, property);
    return true;
}

bool age_vle_terminal_output_emit_full_properties(
    VLE_local_context *vlelctx, FuncCallContext *funcctx,
    const VLEIteratorOutputTarget *target)
{
    MemoryContext terminal_oldctx;
    bool properties_is_null = true;
    Datum properties;

    Assert(vlelctx != NULL);
    Assert(funcctx != NULL);
    Assert(target != NULL);
    Assert(age_vle_context_output_requirement(vlelctx) ==
           AGE_VLE_OUTPUT_REQUIREMENT_TERMINAL_PROPERTIES);

    terminal_oldctx =
        MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);
    properties = build_vle_terminal_properties(vlelctx, &properties_is_null);
    MemoryContextSwitchTo(terminal_oldctx);
    if (properties_is_null)
    {
        age_vle_iterator_output_target_set_null(target);
        return true;
    }
    age_vle_iterator_output_target_set_datum(target, properties);
    return true;
}

bool age_vle_terminal_output_should_materialize_batch(
    VLE_local_context *vlelctx)
{
    return age_vle_context_emits_terminal_property(vlelctx) &&
           !age_vle_context_reverse_output_path(vlelctx) &&
           age_vle_context_uses_local_edge_state(vlelctx) &&
           age_vle_context_has_edge_label(vlelctx) &&
           !age_vle_context_has_edge_property_constraints(vlelctx);
}

void age_vle_terminal_output_materialize_batch(
    VLE_local_context *vlelctx, VLETerminalOutputMaterializePath find_path,
    void *find_path_state)
{
    VLETerminalPropertyLookup lookup;
    VLETerminalPropertyBatchFetch fetch;
    bool saved_emit_terminal_property;

    Assert(vlelctx != NULL);
    Assert(find_path != NULL);

    if (age_vle_context_terminal_property_batch_materialized(vlelctx))
        return;

    saved_emit_terminal_property =
        age_vle_context_emits_terminal_property(vlelctx);
    age_vle_context_set_emit_terminal_property(vlelctx, false);
    while (find_path(find_path_state))
    {
        age_vle_context_append_terminal_property_batch_id(
            vlelctx, age_vle_context_current_terminal_vertex_id(vlelctx));
    }
    age_vle_context_set_emit_terminal_property(
        vlelctx, saved_emit_terminal_property);

    age_vle_context_init_terminal_property_lookup(vlelctx, &lookup);
    age_vle_context_init_terminal_property_batch_fetch(vlelctx, &lookup,
                                                       &fetch);
    age_vle_context_fetch_terminal_property_batch(vlelctx, &fetch);
}

static Datum get_vle_terminal_vertex_property(
    VLE_local_context *vlelctx, const VLETerminalPropertyLookup *lookup,
    graphid terminal_id, bool *is_null)
{
    vertex_entry *ve = NULL;

    Assert(age_vle_context_emits_terminal_property(vlelctx));
    Assert(lookup != NULL);
    Assert(lookup->key_desc.key != NULL);
    Assert(lookup->key_desc.key->type == AGTV_STRING);

    *is_null = true;

    ve = age_vle_context_get_cached_vertex_entry(vlelctx, terminal_id);
    if (ve == NULL)
    {
        ve = get_vertex_entry(vlelctx->ggctx, terminal_id);
        Assert(ve != NULL);
    }

    return get_terminal_property_for_entry(vlelctx, lookup, ve, is_null);
}

static Datum build_vle_terminal_property(
    VLE_local_context *vlelctx, const VLETerminalPropertyLookup *lookup,
    bool *is_null)
{
    graphid terminal_id;

    terminal_id = age_vle_context_current_terminal_vertex_id(vlelctx);

    return get_vle_terminal_vertex_property(vlelctx, lookup, terminal_id,
                                            is_null);
}

static Datum get_vle_terminal_vertex_properties(
    VLE_local_context *vlelctx, graphid terminal_id, bool *is_null)
{
    vertex_entry *ve = NULL;
    HTAB *relation_cache;

    Assert(vlelctx != NULL);
    Assert(is_null != NULL);

    *is_null = true;

    ve = age_vle_context_get_cached_vertex_entry(vlelctx, terminal_id);
    if (ve == NULL)
    {
        ve = get_vertex_entry(vlelctx->ggctx, terminal_id);
        Assert(ve != NULL);
    }

    relation_cache = age_vle_ensure_edge_property_relation_cache(
        vlelctx, "VLE terminal properties relation cache");
    *is_null = false;

    return get_vertex_entry_properties_with_cache(ve, relation_cache);
}

static Datum build_vle_terminal_properties(VLE_local_context *vlelctx,
                                           bool *is_null)
{
    graphid terminal_id;

    terminal_id = age_vle_context_current_terminal_vertex_id(vlelctx);

    return get_vle_terminal_vertex_properties(vlelctx, terminal_id, is_null);
}

static void cache_terminal_property_result(VLE_local_context *vlelctx,
                                           graphid terminal_id)
{
    if (!age_vle_context_emits_terminal_property(vlelctx) ||
        age_vle_context_reverse_output_path(vlelctx))
    {
        age_vle_context_clear_terminal_property_result(vlelctx);
        return;
    }

    {
        VLETerminalPropertyLookup lookup;

        age_vle_context_init_terminal_property_lookup(vlelctx, &lookup);
        cache_direct_terminal_property_result(vlelctx, &lookup, terminal_id);
    }
}

static void cache_direct_terminal_property_result(
    VLE_local_context *vlelctx, const VLETerminalPropertyLookup *lookup,
    graphid terminal_id)
{
    vertex_entry *ve = NULL;

    Assert(lookup != NULL);
    Assert(lookup->key_desc.key != NULL);
    Assert(lookup->key_desc.key->type == AGTV_STRING);

    ve = age_vle_context_get_cached_vertex_entry(vlelctx, terminal_id);
    if (ve == NULL)
    {
        ve = get_vertex_entry(vlelctx->ggctx, terminal_id);
        Assert(ve != NULL);
    }

    cache_direct_terminal_property_result_for_entry(vlelctx, lookup, ve);
}

static void cache_direct_terminal_property_result_for_entry(
    VLE_local_context *vlelctx, const VLETerminalPropertyLookup *lookup,
    vertex_entry *ve)
{
    bool is_null = true;
    Datum property;

    Assert(lookup != NULL);
    Assert(lookup->key_desc.key != NULL);
    Assert(lookup->key_desc.key->type == AGTV_STRING);
    Assert(ve != NULL);

    property = get_terminal_property_for_entry(vlelctx, lookup, ve, &is_null);
    age_vle_context_set_terminal_property_result(vlelctx, property, is_null);
}

static Datum get_terminal_property_for_entry(
    VLE_local_context *vlelctx, const VLETerminalPropertyLookup *lookup,
    vertex_entry *ve, bool *is_null)
{
    Datum property = (Datum) 0;

    Assert(lookup != NULL);
    Assert(lookup->key_desc.key != NULL);
    Assert(lookup->key_desc.key->type == AGTV_STRING);
    Assert(ve != NULL);

    *is_null = true;

    if (get_cached_terminal_property_result(lookup, ve, &property))
    {
        *is_null = false;
        return property;
    }

    if (lookup->allow_block_prefetch)
    {
        Oid relid = get_vertex_entry_label_table_oid(ve);
        BlockNumber blockno = get_vertex_entry_tid_block(ve);

        if (!is_terminal_property_block_prefetched(lookup, relid, blockno))
        {
            int64 cached;
            bool target_found = false;

            if (*lookup->prefetch_budget == 0)
            {
                cached = 0;
            }
            else
            {
                cached = prefetch_vertex_entry_block_scalar_property_cache(
                    lookup->ggctx, ve, lookup->relation_cache,
                    lookup->relation_cache_name, lookup->key_desc.key,
                    *lookup->prefetch_budget,
                    &property, &target_found);
            }
            mark_terminal_property_block_prefetched(lookup, relid, blockno);
            if (*lookup->prefetch_budget >= 0)
            {
                *lookup->prefetch_budget -=
                    Min(*lookup->prefetch_budget, cached);
            }

            if (target_found)
            {
                *is_null = false;
                return property;
            }
            else if (get_cached_terminal_property_result(lookup, ve,
                                                         &property))
            {
                *is_null = false;
                return property;
            }
        }
    }

    if (get_vertex_entry_scalar_property_with_lazy_cache(
            ve, lookup->relation_cache, lookup->relation_cache_name,
            lookup->key_desc.key, &property))
    {
        *is_null = false;
    }

    return property;
}

static bool cache_terminal_property_char_hit_for_entry(
    VLE_local_context *vlelctx, const VLETerminalPropertyLookup *lookup,
    vertex_entry *ve)
{
    Datum property = (Datum) 0;

    Assert(lookup != NULL);
    Assert(lookup->key_desc.key != NULL);
    Assert(lookup->key_desc.key->type == AGTV_STRING);
    Assert(lookup->key_desc.is_char);
    Assert(ve != NULL);

    if (!vertex_entry_cached_property_char_fast(
            ve, lookup->key_desc.key_char, &property))
    {
        return false;
    }

    age_vle_context_set_terminal_property_result(vlelctx, property, false);

    return true;
}

static bool get_cached_terminal_property_result(
    const VLETerminalPropertyLookup *lookup, vertex_entry *ve,
    Datum *property)
{
    const char *key;
    int key_len;

    Assert(lookup != NULL);
    Assert(lookup->key_desc.key != NULL);
    Assert(lookup->key_desc.key->type == AGTV_STRING);

    if (lookup->key_desc.is_char)
    {
        return vertex_entry_cached_property_char_fast(
            ve, lookup->key_desc.key_char, property);
    }

    key = lookup->key_desc.key->val.string.val;
    key_len = lookup->key_desc.key->val.string.len;

    return get_vertex_entry_cached_property_str(ve, key, key_len, property);
}

static bool is_terminal_property_block_prefetched(
    const VLETerminalPropertyLookup *lookup, Oid relid, BlockNumber blockno)
{
    VLEPrefetchedTerminalBlock key;
    bool found = false;

    Assert(lookup != NULL);
    Assert(lookup->prefetched_blocks != NULL);

    if (*lookup->prefetched_blocks == NULL)
    {
        return false;
    }

    MemSet(&key, 0, sizeof(key));
    key.relid = relid;
    key.blockno = blockno;

    (void) hash_search(*lookup->prefetched_blocks, &key, HASH_FIND, &found);

    return found;
}

static void mark_terminal_property_block_prefetched(
    const VLETerminalPropertyLookup *lookup, Oid relid, BlockNumber blockno)
{
    HASHCTL ctl;
    VLEPrefetchedTerminalBlock key;
    bool found;

    Assert(lookup != NULL);
    Assert(lookup->prefetched_blocks != NULL);

    if (*lookup->prefetched_blocks == NULL)
    {
        MemSet(&ctl, 0, sizeof(ctl));
        ctl.keysize = sizeof(VLEPrefetchedTerminalBlock);
        ctl.entrysize = sizeof(VLEPrefetchedTerminalBlock);
        *lookup->prefetched_blocks = hash_create(
            "VLE terminal property prefetched blocks", 16, &ctl,
            HASH_ELEM | HASH_BLOBS);
    }

    MemSet(&key, 0, sizeof(key));
    key.relid = relid;
    key.blockno = blockno;

    (void) hash_search(*lookup->prefetched_blocks, &key, HASH_ENTER, &found);
}
