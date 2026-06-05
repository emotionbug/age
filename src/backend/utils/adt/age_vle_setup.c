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

#include "access/age_adjacency.h"
#include "access/genam.h"
#include "access/relation.h"
#include "catalog/ag_graph.h"
#include "catalog/ag_label.h"
#include "commands/defrem.h"
#include "utils/ag_cache.h"
#include "utils/agtype.h"
#include "utils/age_vle_setup.h"
#include "utils/builtins.h"
#include "utils/rel.h"

#define VLE_SETUP_LOOKUP_CACHE_SIZE 8

static Oid get_cached_vle_graph_oid(const char *graph_name,
                                    int graph_name_len);
static Oid get_cached_vle_label_relation(Oid graph_oid,
                                         const char *label_name,
                                         int label_name_len);
static void get_age_adjacency_indexes_for_label(Oid edge_label_oid,
                                                Oid *outgoing_index_oid,
                                                Oid *incoming_index_oid);
static void get_edge_endpoint_btree_indexes_for_label(Oid edge_label_oid,
                                                      Oid *start_index_oid,
                                                      Oid *end_index_oid);
static bool age_adjacency_index_matches(Relation index_rel, bool outgoing);
static void init_vle_traversal_shape(AgeVLEInput *input,
                                     VLETraversalShape *shape);
static void init_vle_traversal_load_policy(
    AgeVLEInput *input, int64 vle_grammar_node_id,
    VLETraversalSetup *setup);

void init_invalid_vle_traversal_source_indexes(
    VLETraversalSourceIndexes *indexes)
{
    Assert(indexes != NULL);

    indexes->age_adjacency_out_index_oid = InvalidOid;
    indexes->age_adjacency_in_index_oid = InvalidOid;
    indexes->edge_start_index_oid = InvalidOid;
    indexes->edge_end_index_oid = InvalidOid;
}

void init_vle_traversal_source_indexes_for_label(
    VLETraversalSourceIndexes *indexes, Oid edge_label_oid)
{
    Assert(indexes != NULL);

    init_invalid_vle_traversal_source_indexes(indexes);
    if (!OidIsValid(edge_label_oid))
        return;

    get_age_adjacency_indexes_for_label(
        edge_label_oid, &indexes->age_adjacency_out_index_oid,
        &indexes->age_adjacency_in_index_oid);
    get_edge_endpoint_btree_indexes_for_label(
        edge_label_oid, &indexes->edge_start_index_oid,
        &indexes->edge_end_index_oid);
}

GRAPH_global_context *load_vle_graph_context_for_traversal(
    const VLETraversalGraphLoad *graph_load)
{
    const VLETraversalLoadPolicy *load_policy;

    Assert(graph_load != NULL);

    load_policy = &graph_load->source_policy.load_policy;

    return manage_GRAPH_global_contexts_len_for_vle(
        graph_load->graph_name, graph_load->graph_name_len,
        graph_load->graph_oid,
        load_policy->load_edge_property_metadata,
        graph_load->edge_label_oid,
        load_policy->load_edge_metadata,
        load_policy->load_vertex_metadata);
}

void init_vle_traversal_setup(AgeVLEInput *input,
                              int64 vle_grammar_node_id,
                              VLETraversalSetup *setup)
{
    AgeVLEInputEdgePrototype edge_prototype;

    Assert(input != NULL);
    Assert(setup != NULL);

    MemSet(setup, 0, sizeof(*setup));
    setup->graph_load.edge_label_oid = InvalidOid;
    init_invalid_vle_traversal_source_indexes(
        &setup->graph_load.source_policy.indexes);

    setup->graph_load.graph_name =
        age_vle_input_get_graph_name(input,
                                     &setup->graph_load.graph_name_len);
    setup->graph_load.graph_oid =
        get_cached_vle_graph_oid(setup->graph_load.graph_name,
                                 setup->graph_load.graph_name_len);

    age_vle_input_get_edge_prototype(input, &edge_prototype);
    setup->edge_property_constraint_count =
        edge_prototype.property_constraint_count;
    if (edge_prototype.label_known && edge_prototype.label_len != 0)
    {
        setup->graph_load.edge_label_oid = get_cached_vle_label_relation(
            setup->graph_load.graph_oid, edge_prototype.label_name,
            edge_prototype.label_len);
        pfree(edge_prototype.label_name);
        edge_prototype.label_name = NULL;
    }
    setup->edge_property_constraint = edge_prototype.property_constraint;

    init_vle_traversal_source_indexes_for_label(
        &setup->graph_load.source_policy.indexes,
        setup->graph_load.edge_label_oid);

    init_vle_traversal_shape(input, &setup->shape);

    init_vle_traversal_load_policy(input, vle_grammar_node_id, setup);
}

static void init_vle_traversal_shape(AgeVLEInput *input,
                                     VLETraversalShape *shape)
{
    Assert(input != NULL);
    Assert(shape != NULL);

    shape->initial_start_valid = age_vle_input_get_vertex_or_id(
        input, 1, "start vertex argument must be a vertex or the integer id",
        &shape->initial_vsid);
    shape->initial_end_valid = age_vle_input_get_vertex_or_id(
        input, 2, "end vertex argument must be a vertex or the integer id",
        &shape->initial_veid);
    shape->lower = age_vle_input_get_range_lower(input);
    age_vle_input_get_range_upper(input, &shape->upper,
                                  &shape->upper_infinite);
    shape->direction = (cypher_rel_dir)age_vle_input_get_direction(input);
}

static void init_vle_traversal_load_policy(
    AgeVLEInput *input, int64 vle_grammar_node_id,
    VLETraversalSetup *setup)
{
    const VLETraversalSourceIndexes *indexes;
    VLETraversalLoadPolicy *policy;
    const VLETraversalShape *shape;

    Assert(input != NULL);
    Assert(setup != NULL);

    indexes = &setup->graph_load.source_policy.indexes;
    policy = &setup->graph_load.source_policy.load_policy;
    shape = &setup->shape;
    policy->load_edge_property_metadata =
        setup->edge_property_constraint_count > 0;
    policy->load_edge_metadata = true;
    policy->load_vertex_metadata = true;

    if (policy->load_edge_metadata &&
        !policy->load_edge_property_metadata &&
        OidIsValid(setup->graph_load.edge_label_oid) &&
        (shape->initial_start_valid || shape->initial_end_valid) &&
        input->nargs == 8)
    {
        policy->load_vertex_metadata = false;
    }

    if (vle_grammar_node_id < 0 &&
        !policy->load_edge_property_metadata &&
        OidIsValid(setup->graph_load.edge_label_oid) &&
        (shape->initial_start_valid || shape->initial_end_valid))
    {
        bool has_out_source =
            OidIsValid(indexes->age_adjacency_out_index_oid) ||
            OidIsValid(indexes->edge_start_index_oid);
        bool has_in_source =
            OidIsValid(indexes->age_adjacency_in_index_oid) ||
            OidIsValid(indexes->edge_end_index_oid);

        if ((shape->direction == CYPHER_REL_DIR_RIGHT && has_out_source) ||
            (shape->direction == CYPHER_REL_DIR_LEFT && has_in_source) ||
            (shape->direction == CYPHER_REL_DIR_NONE &&
             has_out_source && has_in_source))
        {
            policy->load_edge_metadata = false;
        }
    }
}

static Oid get_cached_vle_graph_oid(const char *graph_name,
                                    int graph_name_len)
{
    typedef struct VLEGraphLookupCacheEntry
    {
        NameData graph_name;
        Oid graph_oid;
        uint64 generation;
    } VLEGraphLookupCacheEntry;
    static VLEGraphLookupCacheEntry cache[VLE_SETUP_LOOKUP_CACHE_SIZE];
    static int next_cache_slot = 0;
    uint64 current_generation = get_graph_cache_generation();
    char *graph_name_cstr;
    NameData graph_name_buf;
    bool free_graph_name = false;
    Oid graph_oid;
    int i;

    for (i = 0;
         graph_name_len < NAMEDATALEN && i < VLE_SETUP_LOOKUP_CACHE_SIZE;
         i++)
    {
        if (OidIsValid(cache[i].graph_oid) &&
            cache[i].generation == current_generation &&
            strncmp(NameStr(cache[i].graph_name), graph_name,
                    graph_name_len) == 0 &&
            NameStr(cache[i].graph_name)[graph_name_len] == '\0')
        {
            return cache[i].graph_oid;
        }
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

    graph_oid = get_graph_oid(graph_name_cstr);
    if (OidIsValid(graph_oid))
    {
        VLEGraphLookupCacheEntry *entry;

        entry = &cache[next_cache_slot++ % VLE_SETUP_LOOKUP_CACHE_SIZE];
        entry->graph_oid = graph_oid;
        entry->generation = current_generation;
        if (graph_name_len < NAMEDATALEN)
        {
            entry->graph_name = graph_name_buf;
        }
        else
        {
            namestrcpy(&entry->graph_name, graph_name_cstr);
        }
    }
    if (free_graph_name)
    {
        pfree(graph_name_cstr);
    }

    return graph_oid;
}

static Oid get_cached_vle_label_relation(Oid graph_oid,
                                         const char *label_name,
                                         int label_name_len)
{
    typedef struct VLELabelLookupCacheEntry
    {
        NameData label_name;
        Oid graph_oid;
        Oid relation_oid;
        uint64 generation;
        bool valid;
    } VLELabelLookupCacheEntry;
    static VLELabelLookupCacheEntry cache[VLE_SETUP_LOOKUP_CACHE_SIZE];
    static int next_cache_slot = 0;
    uint64 current_generation = get_label_cache_generation();
    char *label_name_cstr;
    NameData label_name_buf;
    bool free_label_name = false;
    label_cache_data *label_cache;
    Oid relation_oid;
    int i;

    for (i = 0;
         label_name_len < NAMEDATALEN && i < VLE_SETUP_LOOKUP_CACHE_SIZE;
         i++)
    {
        if (cache[i].valid &&
            cache[i].graph_oid == graph_oid &&
            cache[i].generation == current_generation &&
            strncmp(NameStr(cache[i].label_name), label_name,
                    label_name_len) == 0 &&
            NameStr(cache[i].label_name)[label_name_len] == '\0')
        {
            return cache[i].relation_oid;
        }
    }

    if (label_name_len < NAMEDATALEN)
    {
        memcpy(NameStr(label_name_buf), label_name, label_name_len);
        NameStr(label_name_buf)[label_name_len] = '\0';
        label_name_cstr = NameStr(label_name_buf);
    }
    else
    {
        label_name_cstr = pnstrdup(label_name, label_name_len);
        free_label_name = true;
    }

    label_cache = search_label_name_graph_cache_cached(label_name_cstr,
                                                       graph_oid);
    relation_oid = label_cache != NULL ? label_cache->relation : InvalidOid;
    if (label_name_len < NAMEDATALEN)
    {
        VLELabelLookupCacheEntry *entry;

        entry = &cache[next_cache_slot++ % VLE_SETUP_LOOKUP_CACHE_SIZE];
        entry->graph_oid = graph_oid;
        entry->relation_oid = relation_oid;
        entry->generation = current_generation;
        entry->label_name = label_name_buf;
        entry->valid = true;
    }
    if (free_label_name)
    {
        pfree(label_name_cstr);
    }

    return relation_oid;
}

static void get_age_adjacency_indexes_for_label(Oid edge_label_oid,
                                                Oid *outgoing_index_oid,
                                                Oid *incoming_index_oid)
{
    static Oid age_adjacency_am_oid = InvalidOid;
    static Oid cached_edge_label_oid = InvalidOid;
    static uint64 cached_label_generation = 0;
    static Oid cached_outgoing_index_oid = InvalidOid;
    static Oid cached_incoming_index_oid = InvalidOid;
    uint64 current_label_generation;
    Relation edge_rel;
    List *index_list;
    ListCell *lc;

    *outgoing_index_oid = InvalidOid;
    *incoming_index_oid = InvalidOid;
    if (!OidIsValid(edge_label_oid))
    {
        return;
    }

    current_label_generation = get_label_cache_generation();
    if (cached_edge_label_oid == edge_label_oid &&
        cached_label_generation == current_label_generation)
    {
        *outgoing_index_oid = cached_outgoing_index_oid;
        *incoming_index_oid = cached_incoming_index_oid;
        return;
    }

    if (!OidIsValid(age_adjacency_am_oid))
    {
        age_adjacency_am_oid = get_index_am_oid("age_adjacency", true);
        if (!OidIsValid(age_adjacency_am_oid))
        {
            cached_edge_label_oid = edge_label_oid;
            cached_label_generation = current_label_generation;
            cached_outgoing_index_oid = InvalidOid;
            cached_incoming_index_oid = InvalidOid;
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
            index_rel->rd_index->indisready &&
            age_adjacency_index_matches(index_rel, true))
        {
            *outgoing_index_oid = index_oid;
        }
        if (index_rel->rd_rel->relam == age_adjacency_am_oid &&
            index_rel->rd_index != NULL &&
            index_rel->rd_index->indisvalid &&
            index_rel->rd_index->indisready &&
            age_adjacency_index_matches(index_rel, false))
        {
            *incoming_index_oid = index_oid;
        }
        index_close(index_rel, AccessShareLock);
        if (OidIsValid(*outgoing_index_oid) &&
            OidIsValid(*incoming_index_oid))
        {
            break;
        }
    }

    list_free(index_list);
    relation_close(edge_rel, AccessShareLock);

    cached_edge_label_oid = edge_label_oid;
    cached_label_generation = current_label_generation;
    cached_outgoing_index_oid = *outgoing_index_oid;
    cached_incoming_index_oid = *incoming_index_oid;
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

static void get_edge_endpoint_btree_indexes_for_label(Oid edge_label_oid,
                                                      Oid *start_index_oid,
                                                      Oid *end_index_oid)
{
    static Oid cached_edge_label_oid = InvalidOid;
    static uint64 cached_label_generation = 0;
    static Oid cached_start_index_oid = InvalidOid;
    static Oid cached_end_index_oid = InvalidOid;
    uint64 current_label_generation;
    Relation edge_rel;

    Assert(start_index_oid != NULL);
    Assert(end_index_oid != NULL);

    *start_index_oid = InvalidOid;
    *end_index_oid = InvalidOid;
    if (!OidIsValid(edge_label_oid))
        return;

    current_label_generation = get_label_cache_generation();
    if (cached_edge_label_oid == edge_label_oid &&
        cached_label_generation == current_label_generation)
    {
        *start_index_oid = cached_start_index_oid;
        *end_index_oid = cached_end_index_oid;
        return;
    }

    edge_rel = relation_open(edge_label_oid, AccessShareLock);
    *start_index_oid = find_usable_btree_index_for_attr(
        edge_rel, Anum_ag_label_edge_table_start_id);
    *end_index_oid = find_usable_btree_index_for_attr(
        edge_rel, Anum_ag_label_edge_table_end_id);
    relation_close(edge_rel, AccessShareLock);

    cached_edge_label_oid = edge_label_oid;
    cached_label_generation = current_label_generation;
    cached_start_index_oid = *start_index_oid;
    cached_end_index_oid = *end_index_oid;
}
