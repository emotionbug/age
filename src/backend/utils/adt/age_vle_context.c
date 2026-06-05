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
#include "access/table.h"
#include "catalog/ag_label.h"
#include "common/hashfn.h"
#include "executor/tuptable.h"
#include "utils/age_vle_adjacency_cache.h"
#include "utils/age_vle_context.h"
#include "utils/datum.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"

struct VLEContextEndpointIndexSource
{
    Relation edge_rel;
    Relation index_rel;
    TupleDesc tupdesc;
    TupleTableSlot *slot;
    IndexScanDesc scan_desc;
};

struct VLEContextAgeAdjacencyPayloadSource
{
    graphid source_vertex_id;
    Oid index_oid;
    int32 terminal_label_id;
    AgeAdjacencyVisiblePayloadScan *payload_scan;
    VLEAdjacencyPayloadCacheEntry *cache_entry;
    graphid *frontier_empty_keys;
    int64 scanned;
    int64 replay_index;
    int64 frontier_empty_count;
    int64 frontier_empty_capacity;
    bool use_payload_cache;
    bool replay_payload_cache;
    bool empty_suppressed;
    bool outgoing;
    uint32 terminal_property_filter_id;
    bool pending_payload_valid;
    bool cache_seed_run_recorded;
    AgeAdjacencyPayload pending_payload;
};

typedef struct VLEContextPackedAdjacencyLists
{
    GraphEdgeAdjList *edge_out;
    GraphEdgeAdjList *edge_in;
    GraphEdgeAdjList *edge_self;
} VLEContextPackedAdjacencyLists;

struct VLEContextPackedAdjacencySource
{
    VLEContextPackedAdjacencyLists lists;
    int64 edge_out_idx;
    int64 edge_in_idx;
    int64 edge_self_idx;
    bool suppress_out;
    bool suppress_in;
    bool suppress_self;
};

static void cache_edge_property_constraints(VLE_local_context *vlelctx);
static void free_edge_property_constraints(VLE_local_context *vlelctx);
static void free_cached_property_value(agtype_value *value);
static void free_context_identity(VLE_local_context *vlelctx);
static void free_terminal_output_resources(VLE_local_context *vlelctx);
static void reset_root_empty_completion_summary(
    VLE_local_context *vlelctx);
static void record_root_empty_completion(
    VLE_local_context *vlelctx, bool outgoing);
static AgeAdjacencyVisiblePayloadScan *age_vle_context_get_payload_scan(
    VLE_local_context *vlelctx, const VLEContextSourceCursor *cursor);
static VLEAdjacencyPayloadCacheEntry *age_vle_context_get_payload_cache_entry(
    VLE_local_context *vlelctx, const VLEContextSourceCursor *cursor,
    uint32 terminal_property_filter_id, bool *found);
static uint32 age_vle_context_terminal_property_filter_id(
    VLE_local_context *vlelctx);
static bool age_vle_context_path_length_targets_terminal_depth(
    VLE_local_context *vlelctx, int64 target_path_length);
static bool age_vle_context_cursor_targets_terminal_property_depth(
    VLE_local_context *vlelctx, const VLEContextSourceCursor *cursor);
static bool age_vle_context_endpoint_label_can_prune_source(
    VLE_local_context *vlelctx, int64 target_path_length);
static int32 age_vle_context_label_id_for_target_path_length(
    VLE_local_context *vlelctx, int64 target_path_length);
static void age_vle_context_init_packed_adjacency_lists(
    VLE_local_context *vlelctx, vertex_entry *entry,
    VLEContextPackedAdjacencyLists *lists);
static bool packed_adjacency_source_policy_suppresses_all(
    VLE_local_context *vlelctx,
    bool suppress_out, bool suppress_in, bool suppress_self);
static bool packed_adjacency_lists_have_source(
    const VLEContextPackedAdjacencyLists *lists);
static bool next_packed_adjacency_entry(GraphEdgeAdjList *edge_out,
                                        int64 *edge_out_idx,
                                        GraphEdgeAdjList *edge_in,
                                        int64 *edge_in_idx,
                                        GraphEdgeAdjList *edge_self,
                                        int64 *edge_self_idx,
                                        GraphEdgeAdjEntry **adj_entry);
static bool age_vle_context_age_adjacency_frontier_empty_seen(
    VLEContextAgeAdjacencyPayloadSource *source, graphid source_vertex_id);
static void age_vle_context_queue_age_adjacency_frontier_empty(
    VLE_local_context *vlelctx, VLEContextAgeAdjacencyPayloadSource *source,
    graphid source_vertex_id);
static void age_vle_context_mark_age_adjacency_frontier_empty(
    VLE_local_context *vlelctx, VLEContextAgeAdjacencyPayloadSource *source,
    graphid source_vertex_id);
static void age_vle_context_apply_age_adjacency_frontier_empty(
    VLE_local_context *vlelctx, VLEContextAgeAdjacencyPayloadSource *source);
static bool age_vle_context_cursor_known_empty(
    VLE_local_context *vlelctx, const VLEContextSourceCursor *cursor);
static AgeAdjacencyMatchTerminalPropertyLookup *
age_vle_context_get_terminal_property_lookup(VLE_local_context *vlelctx);
static void age_vle_context_prepare_terminal_property_prefilter(
    VLE_local_context *vlelctx, VLEContextAgeAdjacencyPayloadSource *source);

void age_vle_context_apply_base(
    VLE_local_context *vlelctx,
    const VLETraversalContextApply *context_apply)
{
    Datum d_edge_property_constraint = 0;

    Assert(vlelctx != NULL);
    Assert(context_apply != NULL);
    Assert(context_apply->ggctx != NULL);

    vlelctx->use_cache = context_apply->use_cache;
    vlelctx->vle_grammar_node_id = context_apply->vle_grammar_node_id;
    vlelctx->graph_name = context_apply->graph_name;
    vlelctx->graph_oid = context_apply->graph_oid;
    vlelctx->ggctx = context_apply->ggctx;
    vlelctx->traversal.edge_state.use_local =
        context_apply->use_local_edge_state;

    vlelctx->num_edge_property_constraints =
        context_apply->edge_property_constraint_count;
    vlelctx->edge_property_relation_cache = NULL;
    if (vlelctx->num_edge_property_constraints > 0)
    {
        Assert(context_apply->edge_property_constraint != NULL);
        vlelctx->edge_property_constraint =
            context_apply->edge_property_constraint;
        d_edge_property_constraint =
            AGTYPE_P_GET_DATUM(context_apply->edge_property_constraint);
        vlelctx->edge_property_constraint_datum =
            d_edge_property_constraint;
        vlelctx->edge_property_constraint_hash =
            datum_image_hash(d_edge_property_constraint, false, -1);
        cache_edge_property_constraints(vlelctx);
    }
    else
    {
        vlelctx->edge_property_constraint = NULL;
        vlelctx->edge_property_constraint_datum = 0;
        vlelctx->edge_property_constraint_hash = 0;
        vlelctx->edge_property_constraint_pairs = NULL;
        vlelctx->num_cached_edge_property_constraints = 0;
    }

    vlelctx->edge_label_name = NULL;
    vlelctx->edge_label_name_oid = context_apply->edge_label_oid;
    vlelctx->terminal_label_id = context_apply->terminal_label_id;
    vlelctx->terminal_endpoint_label_id =
        context_apply->terminal_endpoint_label_id;
    vlelctx->root.source_indexes = context_apply->source_indexes;
    vlelctx->source_policy_known = context_apply->source_policy_known;
    vlelctx->source_policy_outgoing_kind =
        context_apply->source_policy_outgoing_kind;
    vlelctx->source_policy_incoming_kind =
        context_apply->source_policy_incoming_kind;
    vlelctx->empty_lifecycle_policy_known =
        context_apply->empty_lifecycle_policy_known;
    vlelctx->empty_lifecycle_eligible =
        context_apply->empty_lifecycle_eligible;
    vlelctx->empty_lifecycle_depth =
        context_apply->empty_lifecycle_depth;
    vlelctx->empty_lifecycle_batch_size =
        context_apply->empty_lifecycle_batch_size;

    vlelctx->root.lidx = context_apply->lower;
    vlelctx->root.uidx = context_apply->upper;
    vlelctx->root.uidx_infinite = context_apply->upper_infinite;
    vlelctx->root.terminal_property_prefilter_eligible =
        context_apply->terminal_property_prefilter_eligible;
    vlelctx->root.terminal_property_index_oid =
        context_apply->terminal_property_index_oid;
    vlelctx->root.terminal_property_predicate_known =
        context_apply->terminal_property_predicate_known &&
        context_apply->terminal_property_predicate_key_known;
    if (vlelctx->root.terminal_property_predicate_known)
    {
        vlelctx->root.terminal_property_predicate_key.type = AGTV_STRING;
        vlelctx->root.terminal_property_predicate_key.val.string.len =
            context_apply->terminal_property_predicate_key_len;
        vlelctx->root.terminal_property_predicate_key.val.string.val =
            pnstrdup(context_apply->terminal_property_predicate_key_value,
                    context_apply->terminal_property_predicate_key_len);
        vlelctx->root.terminal_property_predicate_key_is_char =
            context_apply->terminal_property_predicate_key_is_char;
        vlelctx->root.terminal_property_predicate_key_char =
            context_apply->terminal_property_predicate_key_char;
    }
    vlelctx->root.terminal_property_predicate_value =
        context_apply->terminal_property_predicate_value;
    vlelctx->root.terminal_property_predicate_null =
        context_apply->terminal_property_predicate_null;
    vlelctx->root.terminal_property_prefetch_threshold =
        context_apply->terminal_property_source_prefetch_threshold;
    vlelctx->output.terminal_property_prefetch_budget =
        context_apply->terminal_property_prefetch_budget;
    age_vle_context_record_empty_lifecycle_policy(vlelctx);
}

static void cache_edge_property_constraints(VLE_local_context *vlelctx)
{
    agtype_iterator *constraint_it = NULL;
    agtype_iterator_token token;
    agtype_pair *pairs = NULL;
    agtype_value agtv;
    int index;

    Assert(vlelctx != NULL);

    vlelctx->edge_property_constraint_pairs = NULL;
    vlelctx->num_cached_edge_property_constraints = 0;

    if (vlelctx->num_edge_property_constraints <= 0)
        return;

    pairs = palloc0(sizeof(agtype_pair) *
                    vlelctx->num_edge_property_constraints);

    constraint_it = agtype_iterator_init(
        &vlelctx->edge_property_constraint->root);
    token = agtype_iterator_next(&constraint_it, &agtv, true);
    Assert(token == WAGT_BEGIN_OBJECT);

    for (index = 0; index < vlelctx->num_edge_property_constraints; index++)
    {
        token = agtype_iterator_next(&constraint_it, &pairs[index].key, true);
        Assert(token == WAGT_KEY);

        token = agtype_iterator_next(&constraint_it, &pairs[index].value, true);
        Assert(token == WAGT_VALUE);
    }

    token = agtype_iterator_next(&constraint_it, &agtv, true);
    Assert(token == WAGT_END_OBJECT);

    vlelctx->edge_property_constraint_pairs = pairs;
    vlelctx->num_cached_edge_property_constraints =
        vlelctx->num_edge_property_constraints;
}

static void free_cached_property_value(agtype_value *value)
{
    switch (value->type)
    {
        case AGTV_STRING:
            pfree_if_not_null(value->val.string.val);
            break;

        case AGTV_NUMERIC:
            pfree_if_not_null(value->val.numeric);
            break;

        case AGTV_VERTEX:
        case AGTV_EDGE:
        case AGTV_PATH:
        case AGTV_ARRAY:
        case AGTV_OBJECT:
            pfree_agtype_value_content(value);
            break;

        default:
            break;
    }
}

void age_vle_context_apply_output(
    VLE_local_context *vlelctx,
    const VLETraversalOutputApply *output_apply)
{
    Assert(vlelctx != NULL);
    Assert(output_apply != NULL);

    vlelctx->output.requirement = output_apply->output_requirement;
    vlelctx->output.emit_terminal_only = output_apply->emit_terminal_only;
    vlelctx->output.emit_terminal_property = output_apply->emit_terminal_property;
    vlelctx->output.carry_frame_vertex_entry =
        output_apply->carry_frame_vertex_entry;
    vlelctx->output.terminal_property_key_is_char =
        output_apply->terminal_property_key_is_char;
    vlelctx->output.terminal_property_key_char =
        output_apply->terminal_property_key_char;

    if (output_apply->has_terminal_property_key)
    {
        const agtype_value *key_value;

        key_value = &output_apply->terminal_property_key;
        Assert(key_value->type == AGTV_STRING);
        vlelctx->output.terminal_property_key = *key_value;
        vlelctx->output.terminal_property_key.val.string.val =
            pnstrdup(key_value->val.string.val, key_value->val.string.len);
    }
}

void age_vle_context_apply_edge_state(
    VLE_local_context *vlelctx,
    const VLETraversalEdgeStateApply *edge_state_apply)
{
    Assert(vlelctx != NULL);
    Assert(edge_state_apply != NULL);

    if (!edge_state_apply->initialize)
        return;

    age_vle_edge_state_init(&vlelctx->traversal.edge_state,
                            edge_state_apply->use_local,
                            edge_state_apply->capacity);
}

void age_vle_context_apply_root(
    VLE_local_context *vlelctx, const VLETraversalRootDescriptor *root)
{
    Assert(vlelctx != NULL);
    Assert(root != NULL);

    vlelctx->root.path_function = root->path_function;
    vlelctx->root.vsid = root->start_vertex_id;
    vlelctx->root.veid = root->end_vertex_id;
    vlelctx->root.edge_direction = root->direction;
    vlelctx->root.next_vertex = root->next_vertex;
    vlelctx->root.reverse_paths_to = root->reverse_paths_to;
    vlelctx->root.reverse_output_path = root->reverse_output_path;
    vlelctx->root.source_layout = root->source_layout;
    vlelctx->root.empty_completion = root->empty_completion;
}

void age_vle_context_get_current_root(
    VLETraversalRootDescriptor *root, VLE_local_context *vlelctx)
{
    Assert(root != NULL);
    Assert(vlelctx != NULL);

    root->path_function = vlelctx->root.path_function;
    root->start_vertex_id = vlelctx->root.vsid;
    root->end_vertex_id = vlelctx->root.veid;
    root->direction = vlelctx->root.edge_direction;
    root->next_vertex = vlelctx->root.next_vertex;
    root->reverse_paths_to = vlelctx->root.reverse_paths_to;
    root->reverse_output_path = vlelctx->root.reverse_output_path;
    root->source_layout = vlelctx->root.source_layout;
    root->empty_completion = vlelctx->root.empty_completion;
}

int64 age_vle_context_terminal_prefetch_budget(VLE_local_context *vlelctx)
{
    Assert(vlelctx != NULL);

    return vlelctx->root.uidx_infinite ? -1 : vlelctx->root.uidx + 1;
}

void age_vle_context_reset_terminal_property_direct_result(
    VLE_local_context *vlelctx, int64 prefetch_budget)
{
    Assert(vlelctx != NULL);

    vlelctx->output.terminal_property_prefetch_budget = prefetch_budget;
    vlelctx->output.terminal_property_result = (Datum) 0;
    vlelctx->output.terminal_property_result_valid = false;
    vlelctx->output.terminal_property_result_is_null = true;
    if (vlelctx->output.terminal_property_prefetched_blocks != NULL)
    {
        hash_destroy(vlelctx->output.terminal_property_prefetched_blocks);
        vlelctx->output.terminal_property_prefetched_blocks = NULL;
    }
}

void age_vle_context_close_adjacency_scans(VLE_local_context *vlelctx)
{
    Assert(vlelctx != NULL);

    if (vlelctx->root.age_adjacency_out_scan != NULL)
    {
        age_adjacency_end_visible_payload_scan(
            vlelctx->root.age_adjacency_out_scan);
        vlelctx->root.age_adjacency_out_scan = NULL;
    }
    if (vlelctx->root.age_adjacency_in_scan != NULL)
    {
        age_adjacency_end_visible_payload_scan(
            vlelctx->root.age_adjacency_in_scan);
        vlelctx->root.age_adjacency_in_scan = NULL;
    }
    if (vlelctx->root.terminal_property_lookup != NULL)
    {
        age_adjacency_match_terminal_property_end(
            vlelctx->root.terminal_property_lookup);
        vlelctx->root.terminal_property_lookup = NULL;
    }
}

void age_vle_context_free_adjacency_payload_cache(
    VLE_local_context *vlelctx)
{
    Assert(vlelctx != NULL);

    age_vle_adjacency_payload_cache_free(
        &vlelctx->root.age_adjacency_payload_cache);
}

void age_vle_context_release_runtime_resources(VLE_local_context *vlelctx)
{
    Assert(vlelctx != NULL);

    age_vle_context_close_adjacency_scans(vlelctx);
    destroy_entry_property_relation_cache(
        vlelctx->edge_property_relation_cache);
    vlelctx->edge_property_relation_cache = NULL;
    if (vlelctx->output.terminal_property_prefetched_blocks != NULL)
    {
        hash_destroy(vlelctx->output.terminal_property_prefetched_blocks);
        vlelctx->output.terminal_property_prefetched_blocks = NULL;
    }
    age_vle_context_reset_terminal_property_batch(vlelctx);
}

void age_vle_context_release_all_resources(VLE_local_context *vlelctx)
{
    Assert(vlelctx != NULL);

    free_context_identity(vlelctx);
    free_terminal_output_resources(vlelctx);
    age_vle_context_release_runtime_resources(vlelctx);
    age_vle_context_free_adjacency_payload_cache(vlelctx);
    free_edge_property_constraints(vlelctx);
    age_vle_traversal_state_free(&vlelctx->traversal);
}

void age_vle_context_reset_source_stats(VLE_local_context *vlelctx)
{
    Assert(vlelctx != NULL);

    memset(&vlelctx->source_stats, 0, sizeof(vlelctx->source_stats));
}

void age_vle_context_get_source_stats(
    VLE_local_context *vlelctx, AgeVLESourceStats *stats)
{
    int64 empty_completion_count;

    Assert(vlelctx != NULL);
    Assert(stats != NULL);

    *stats = vlelctx->source_stats;
    if (stats->root_empty_completion_count > 0 ||
        stats->empty_lifecycle_batch_capacity <= 0)
    {
        return;
    }

    empty_completion_count =
        stats->age_adjacency_empty_source_skips +
        stats->age_adjacency_empty_source_cache_hits +
        stats->age_adjacency_empty_source_frontier_marks +
        stats->age_adjacency_empty_source_run_skips;
    if (empty_completion_count <= 0)
        return;

    stats->root_empty_completion_count = empty_completion_count;
    stats->root_empty_completion_out =
        stats->age_adjacency_empty_source_skip_out +
        stats->age_adjacency_empty_source_cache_hit_out +
        stats->age_adjacency_empty_source_frontier_mark_out +
        stats->age_adjacency_empty_source_run_skip_out;
    stats->root_empty_completion_in =
        stats->age_adjacency_empty_source_skip_in +
        stats->age_adjacency_empty_source_cache_hit_in +
        stats->age_adjacency_empty_source_frontier_mark_in +
        stats->age_adjacency_empty_source_run_skip_in;
    stats->root_empty_batch_capacity =
        stats->empty_lifecycle_batch_capacity;
    stats->root_empty_saturated_count =
        empty_completion_count >= stats->empty_lifecycle_batch_capacity ?
        1 : 0;
}

static void reset_root_empty_completion_summary(
    VLE_local_context *vlelctx)
{
    Assert(vlelctx != NULL);

    vlelctx->root.empty_completion.completion_count = 0;
    vlelctx->root.empty_completion.out_count = 0;
    vlelctx->root.empty_completion.in_count = 0;
    vlelctx->root.empty_completion.saturated = false;
}

static void record_root_empty_completion(
    VLE_local_context *vlelctx, bool outgoing)
{
    VLETraversalEmptyCompletionSummary *summary;
    int64 batch_capacity;

    Assert(vlelctx != NULL);

    summary = &vlelctx->root.empty_completion;
    batch_capacity = summary->batch_capacity;
    if (batch_capacity <= 0)
        batch_capacity = vlelctx->source_stats.empty_lifecycle_batch_capacity;
    if (batch_capacity <= 0 &&
        vlelctx->empty_lifecycle_policy_known &&
        vlelctx->empty_lifecycle_eligible)
    {
        batch_capacity = vlelctx->empty_lifecycle_batch_size;
    }
    if (batch_capacity <= 0)
        return;

    if (summary->batch_capacity <= 0)
        summary->batch_capacity = batch_capacity;

    summary->completion_count++;
    vlelctx->source_stats.root_empty_completion_count++;
    if (outgoing)
    {
        summary->out_count++;
        vlelctx->source_stats.root_empty_completion_out++;
    }
    else
    {
        summary->in_count++;
        vlelctx->source_stats.root_empty_completion_in++;
    }

    vlelctx->source_stats.root_empty_batch_capacity =
        Max(vlelctx->source_stats.root_empty_batch_capacity,
            summary->batch_capacity);

    if (summary->batch_capacity > 0 &&
        summary->completion_count >= summary->batch_capacity &&
        !summary->saturated)
    {
        summary->saturated = true;
        vlelctx->source_stats.root_empty_saturated_count++;
    }
}

void age_vle_context_record_empty_lifecycle_policy(
    VLE_local_context *vlelctx)
{
    Assert(vlelctx != NULL);

    if (!vlelctx->empty_lifecycle_policy_known)
        return;

    vlelctx->source_stats.empty_lifecycle_context_runs++;
    if (vlelctx->empty_lifecycle_eligible)
    {
        vlelctx->source_stats.empty_lifecycle_context_eligible_runs++;
        vlelctx->source_stats.empty_lifecycle_context_depth =
            Max(vlelctx->source_stats.empty_lifecycle_context_depth,
                vlelctx->empty_lifecycle_depth);
        vlelctx->source_stats.empty_lifecycle_batch_capacity =
            Max(vlelctx->source_stats.empty_lifecycle_batch_capacity,
                vlelctx->empty_lifecycle_batch_size);
    }
}

int64 age_vle_context_empty_lifecycle_batch_size(VLE_local_context *vlelctx)
{
    Assert(vlelctx != NULL);

    if (!vlelctx->empty_lifecycle_policy_known ||
        !vlelctx->empty_lifecycle_eligible)
    {
        return 0;
    }

    return vlelctx->empty_lifecycle_batch_size;
}

void age_vle_context_record_source_scan(
    VLE_local_context *vlelctx, VLEContextSourceStatsKind kind)
{
    Assert(vlelctx != NULL);

    switch (kind)
    {
        case VLE_CONTEXT_SOURCE_STATS_AGE_ADJACENCY:
            vlelctx->source_stats.age_adjacency_scans++;
            break;
        case VLE_CONTEXT_SOURCE_STATS_ENDPOINT_BTREE:
            vlelctx->source_stats.endpoint_btree_scans++;
            break;
        case VLE_CONTEXT_SOURCE_STATS_PACKED_ADJACENCY:
            vlelctx->source_stats.packed_scans++;
            break;
    }
}

void age_vle_context_record_source_candidate(
    VLE_local_context *vlelctx, VLEContextSourceStatsKind kind)
{
    Assert(vlelctx != NULL);

    vlelctx->source_stats.candidates_yielded++;
    switch (kind)
    {
        case VLE_CONTEXT_SOURCE_STATS_AGE_ADJACENCY:
            vlelctx->source_stats.age_adjacency_candidates++;
            break;
        case VLE_CONTEXT_SOURCE_STATS_ENDPOINT_BTREE:
            vlelctx->source_stats.endpoint_btree_candidates++;
            break;
        case VLE_CONTEXT_SOURCE_STATS_PACKED_ADJACENCY:
            vlelctx->source_stats.packed_candidates++;
            break;
    }
}

void age_vle_context_record_source_empty_scan(
    VLE_local_context *vlelctx, VLEContextSourceStatsKind kind)
{
    Assert(vlelctx != NULL);

    switch (kind)
    {
        case VLE_CONTEXT_SOURCE_STATS_AGE_ADJACENCY:
            vlelctx->source_stats.age_adjacency_empty_scans++;
            break;
        case VLE_CONTEXT_SOURCE_STATS_ENDPOINT_BTREE:
            vlelctx->source_stats.endpoint_btree_empty_scans++;
            break;
        case VLE_CONTEXT_SOURCE_STATS_PACKED_ADJACENCY:
            break;
    }
}

void age_vle_context_record_age_adjacency_directory_filtered_empty_scan(
    VLE_local_context *vlelctx)
{
    Assert(vlelctx != NULL);

    vlelctx->source_stats.age_adjacency_directory_filtered_empty_scans++;
}

int64 age_vle_context_age_adjacency_directory_filtered(
    VLE_local_context *vlelctx)
{
    Assert(vlelctx != NULL);

    return vlelctx->source_stats.
        age_adjacency_payload_vertex_set_directory_filtered;
}

void age_vle_context_record_age_adjacency_empty_source_skip(
    VLE_local_context *vlelctx, bool outgoing)
{
    Assert(vlelctx != NULL);

    vlelctx->source_stats.age_adjacency_empty_source_skips++;
    record_root_empty_completion(vlelctx, outgoing);
    if (outgoing)
        vlelctx->source_stats.age_adjacency_empty_source_skip_out++;
    else
        vlelctx->source_stats.age_adjacency_empty_source_skip_in++;
}

void age_vle_context_record_age_adjacency_empty_source_cache_hit(
    VLE_local_context *vlelctx, bool outgoing)
{
    Assert(vlelctx != NULL);

    vlelctx->source_stats.age_adjacency_empty_source_cache_hits++;
    record_root_empty_completion(vlelctx, outgoing);
    if (outgoing)
        vlelctx->source_stats.age_adjacency_empty_source_cache_hit_out++;
    else
        vlelctx->source_stats.age_adjacency_empty_source_cache_hit_in++;
}

void age_vle_context_record_age_adjacency_empty_source_frontier_mark(
    VLE_local_context *vlelctx, bool outgoing)
{
    Assert(vlelctx != NULL);

    vlelctx->source_stats.age_adjacency_empty_source_frontier_marks++;
    record_root_empty_completion(vlelctx, outgoing);
    if (outgoing)
        vlelctx->source_stats.age_adjacency_empty_source_frontier_mark_out++;
    else
        vlelctx->source_stats.age_adjacency_empty_source_frontier_mark_in++;
}

void age_vle_context_record_age_adjacency_empty_source_frontier_batch(
    VLE_local_context *vlelctx, bool outgoing, int64 key_count)
{
    Assert(vlelctx != NULL);

    if (key_count <= 0)
        return;

    vlelctx->source_stats.age_adjacency_empty_source_frontier_batch_flushes++;
    vlelctx->source_stats.age_adjacency_empty_source_frontier_batch_keys +=
        key_count;
    vlelctx->source_stats.age_adjacency_empty_source_frontier_batch_max =
        Max(vlelctx->source_stats.age_adjacency_empty_source_frontier_batch_max,
            key_count);
    if (outgoing)
        vlelctx->source_stats.age_adjacency_empty_source_frontier_batch_out++;
    else
        vlelctx->source_stats.age_adjacency_empty_source_frontier_batch_in++;
}

void age_vle_context_record_age_adjacency_empty_source_run_skip(
    VLE_local_context *vlelctx, bool outgoing)
{
    Assert(vlelctx != NULL);

    vlelctx->source_stats.age_adjacency_empty_source_run_skips++;
    record_root_empty_completion(vlelctx, outgoing);
    if (outgoing)
        vlelctx->source_stats.age_adjacency_empty_source_run_skip_out++;
    else
        vlelctx->source_stats.age_adjacency_empty_source_run_skip_in++;
}

void age_vle_context_record_source_push(VLE_local_context *vlelctx)
{
    Assert(vlelctx != NULL);

    vlelctx->source_stats.candidates_pushed++;
}

void age_vle_context_record_missing_vertex_attempt(
    VLE_local_context *vlelctx)
{
    Assert(vlelctx != NULL);

    vlelctx->source_stats.missing_vertex_attempts++;
}

void age_vle_context_record_missing_vertex_hit(VLE_local_context *vlelctx)
{
    Assert(vlelctx != NULL);

    vlelctx->source_stats.missing_vertex_source_hits++;
}

void age_vle_context_record_packed_suppression(
    VLE_local_context *vlelctx, bool suppress_out, bool suppress_in,
    bool suppress_self)
{
    Assert(vlelctx != NULL);

    if (suppress_out)
        vlelctx->source_stats.packed_suppress_out++;
    if (suppress_in)
        vlelctx->source_stats.packed_suppress_in++;
    if (suppress_self)
        vlelctx->source_stats.packed_suppress_self++;
}

void age_vle_context_record_packed_empty_skip(VLE_local_context *vlelctx)
{
    Assert(vlelctx != NULL);

    vlelctx->source_stats.packed_empty_skips++;
}

void age_vle_context_record_packed_policy_skip(VLE_local_context *vlelctx)
{
    Assert(vlelctx != NULL);

    vlelctx->source_stats.packed_policy_skips++;
}

void age_vle_context_record_age_adjacency_payload_replay(
    VLE_local_context *vlelctx)
{
    Assert(vlelctx != NULL);

    vlelctx->source_stats.age_adjacency_payload_replays++;
}

void age_vle_context_record_age_adjacency_payload_replay_run(
    VLE_local_context *vlelctx)
{
    Assert(vlelctx != NULL);

    vlelctx->source_stats.age_adjacency_payload_replay_runs++;
}

void age_vle_context_record_age_adjacency_payload_scan(
    VLE_local_context *vlelctx)
{
    Assert(vlelctx != NULL);

    vlelctx->source_stats.age_adjacency_payload_scans++;
}

void age_vle_context_record_age_adjacency_payload_scan_run(
    VLE_local_context *vlelctx)
{
    Assert(vlelctx != NULL);

    vlelctx->source_stats.age_adjacency_payload_scan_runs++;
}

void age_vle_context_record_age_adjacency_payload_cache_seed(
    VLE_local_context *vlelctx)
{
    Assert(vlelctx != NULL);

    vlelctx->source_stats.age_adjacency_payload_cache_seeds++;
}

void age_vle_context_record_age_adjacency_payload_cache_seed_run(
    VLE_local_context *vlelctx)
{
    Assert(vlelctx != NULL);

    vlelctx->source_stats.age_adjacency_payload_cache_seed_runs++;
}

int64 age_vle_context_get_or_create_local_edge_index(
    VLE_local_context *vlelctx, graphid edge_id)
{
    Assert(vlelctx != NULL);

    return age_vle_traversal_get_or_create_local_edge_index(
        &vlelctx->traversal, edge_id);
}

void age_vle_context_init_candidate_match_result(
    VLECandidateMatchResult *match_result,
    const VLEEdgePropertyMatchContext *match_context)
{
    Assert(match_result != NULL);
    Assert(match_context != NULL);

    match_result->match_context = match_context;
    match_result->edge_for_match = NULL;
}

void age_vle_context_apply_candidate_match_result(
    VLE_local_context *vlelctx, const VLETraversalCandidate *candidate,
    const VLECandidateMatchResult *match_result, const char *caller)
{
    bool matched;

    Assert(vlelctx != NULL);
    Assert(candidate != NULL);
    Assert(match_result != NULL);
    Assert(match_result->match_context != NULL);

    if (!age_vle_traversal_candidate_needs_match_check(
            &vlelctx->traversal, candidate->edge_index, caller))
    {
        return;
    }

    if (!age_vle_edge_property_match_context_has_constraints(
            match_result->match_context))
    {
        matched = true;
    }
    else
    {
        Assert(match_result->edge_for_match != NULL);
        matched = age_vle_edge_property_matches(
            match_result->match_context, match_result->edge_for_match);
    }

    age_vle_traversal_candidate_mark_match(
        &vlelctx->traversal, candidate->edge_index, matched, caller);
}

bool age_vle_context_push_candidate_if_matched(
    VLE_local_context *vlelctx, const VLETraversalCandidate *candidate,
    const char *caller)
{
    int32 target_label_id;

    Assert(vlelctx != NULL);
    Assert(candidate != NULL);

    target_label_id = age_vle_context_label_id_for_target_path_length(
        vlelctx, vlelctx->traversal.path_depth + 1);
    if (label_id_is_valid(target_label_id) &&
        get_graphid_label_id(candidate->next_vertex_id) !=
        target_label_id)
    {
        return false;
    }

    return age_vle_traversal_push_candidate_if_matched(
        &vlelctx->traversal, candidate, caller);
}

void age_vle_context_refresh_source_indexes(VLE_local_context *vlelctx)
{
    Assert(vlelctx != NULL);

    if (!OidIsValid(vlelctx->edge_label_name_oid))
    {
        init_invalid_vle_traversal_source_indexes(
            &vlelctx->root.source_indexes);
        age_vle_context_refresh_source_layout(vlelctx);
        return;
    }

    init_vle_traversal_source_indexes_for_label(
        &vlelctx->root.source_indexes, vlelctx->edge_label_name_oid);
    age_vle_context_refresh_source_layout(vlelctx);
}

bool age_vle_context_should_load_initial_stacks(VLE_local_context *vlelctx)
{
    Assert(vlelctx != NULL);

    return !(!vlelctx->root.uidx_infinite &&
             vlelctx->root.lidx > vlelctx->root.uidx);
}

void age_vle_context_init_traversal_state(VLE_local_context *vlelctx)
{
    Assert(vlelctx != NULL);

    age_vle_traversal_state_init(&vlelctx->traversal);
}

void age_vle_context_reset_traversal_for_start(VLE_local_context *vlelctx)
{
    Assert(vlelctx != NULL);

    age_vle_traversal_state_reset(&vlelctx->traversal, vlelctx->root.vsid);
    vlelctx->cached_vertex_id = 0;
    vlelctx->cached_vertex_entry = NULL;
}

bool age_vle_context_consume_next_step(
    VLE_local_context *vlelctx, const char *caller, VLETraversalStep *step)
{
    Assert(vlelctx != NULL);
    Assert(step != NULL);

    if (!age_vle_consume_next_frame(&vlelctx->traversal, caller, step))
    {
        return false;
    }

    if (step->vertex_entry != NULL)
    {
        age_vle_context_remember_vertex_entry(vlelctx, step->vertex_id,
                                              step->vertex_entry);
    }

    return true;
}

void age_vle_context_mark_dirty(VLE_local_context *vlelctx)
{
    Assert(vlelctx != NULL);

    vlelctx->is_dirty = true;
}

graphid age_vle_context_start_vertex_id(VLE_local_context *vlelctx)
{
    Assert(vlelctx != NULL);

    return vlelctx->root.vsid;
}

graphid age_vle_context_end_vertex_id(VLE_local_context *vlelctx)
{
    Assert(vlelctx != NULL);

    return vlelctx->root.veid;
}

VLE_path_function age_vle_context_path_function(VLE_local_context *vlelctx)
{
    Assert(vlelctx != NULL);

    return vlelctx->root.path_function;
}

bool age_vle_context_reverse_paths_to(VLE_local_context *vlelctx)
{
    Assert(vlelctx != NULL);

    return vlelctx->root.reverse_paths_to;
}

bool age_vle_context_reverse_output_path(VLE_local_context *vlelctx)
{
    Assert(vlelctx != NULL);

    return vlelctx->root.reverse_output_path;
}

cypher_rel_dir age_vle_context_edge_direction(VLE_local_context *vlelctx)
{
    Assert(vlelctx != NULL);

    return vlelctx->root.edge_direction;
}

bool age_vle_context_is_empty_length_range(VLE_local_context *vlelctx)
{
    Assert(vlelctx != NULL);

    return !vlelctx->root.uidx_infinite &&
           vlelctx->root.lidx > vlelctx->root.uidx;
}

bool age_vle_context_is_zero_length_only(VLE_local_context *vlelctx)
{
    Assert(vlelctx != NULL);

    return vlelctx->root.lidx == 0 &&
           !vlelctx->root.uidx_infinite &&
           vlelctx->root.uidx == 0;
}

bool age_vle_context_should_emit_zero_bound(VLE_local_context *vlelctx)
{
    Assert(vlelctx != NULL);

    if (label_id_is_valid(vlelctx->terminal_endpoint_label_id) &&
        get_graphid_label_id(vlelctx->root.vsid) !=
        vlelctx->terminal_endpoint_label_id)
    {
        return false;
    }

    if (vlelctx->root.path_function == VLE_FUNCTION_PATHS_BETWEEN)
        return vlelctx->root.vsid == vlelctx->root.veid;

    return true;
}

bool age_vle_context_reached_upper_bound(
    VLE_local_context *vlelctx, int64 path_length)
{
    Assert(vlelctx != NULL);

    return !vlelctx->root.uidx_infinite &&
           path_length >= vlelctx->root.uidx;
}

bool age_vle_context_should_prefetch_terminal_property_block(
    VLE_local_context *vlelctx)
{
    Assert(vlelctx != NULL);

    return vlelctx->root.uidx_infinite || vlelctx->root.uidx >= 64;
}

bool age_vle_context_emits_terminal_property(VLE_local_context *vlelctx)
{
    Assert(vlelctx != NULL);

    return vlelctx->output.emit_terminal_property;
}

AgeVLEOutputRequirement age_vle_context_output_requirement(
    VLE_local_context *vlelctx)
{
    Assert(vlelctx != NULL);

    return vlelctx->output.requirement;
}

void age_vle_context_set_emit_terminal_property(
    VLE_local_context *vlelctx, bool emit_terminal_property)
{
    Assert(vlelctx != NULL);

    vlelctx->output.emit_terminal_property = emit_terminal_property;
}

void age_vle_context_init_terminal_property_lookup(
    VLE_local_context *vlelctx, VLETerminalPropertyLookup *lookup)
{
    Assert(vlelctx != NULL);
    Assert(lookup != NULL);
    Assert(vlelctx->output.terminal_property_key.type == AGTV_STRING);

    lookup->ggctx = vlelctx->ggctx;
    lookup->key_desc.key = &vlelctx->output.terminal_property_key;
    lookup->key_desc.is_char = vlelctx->output.terminal_property_key_is_char;
    lookup->key_desc.key_char = vlelctx->output.terminal_property_key_char;
    lookup->relation_cache = &vlelctx->edge_property_relation_cache;
    lookup->relation_cache_name = "VLE terminal property relation cache";
    lookup->prefetched_blocks =
        &vlelctx->output.terminal_property_prefetched_blocks;
    lookup->prefetch_budget =
        &vlelctx->output.terminal_property_prefetch_budget;
    lookup->allow_block_prefetch =
        age_vle_context_should_prefetch_terminal_property_block(vlelctx);
}

void age_vle_context_init_terminal_output_policy(
    VLE_local_context *vlelctx, VLETerminalOutputPolicy *policy)
{
    Assert(vlelctx != NULL);
    Assert(policy != NULL);

    policy->emit_property = vlelctx->output.emit_terminal_property;
    policy->direct_property = vlelctx->output.emit_terminal_property &&
                              !age_vle_context_reverse_output_path(vlelctx);
    if (policy->emit_property)
    {
        age_vle_context_init_terminal_property_lookup(vlelctx,
                                                      &policy->lookup);
        policy->char_fast_path = policy->direct_property &&
                                 policy->lookup.key_desc.is_char;
    }
    else
    {
        MemSet(&policy->lookup, 0, sizeof(policy->lookup));
        policy->char_fast_path = false;
    }
}

void age_vle_context_init_terminal_property_batch_fetch(
    VLE_local_context *vlelctx, const VLETerminalPropertyLookup *lookup,
    VLETerminalPropertyBatchFetch *fetch)
{
    Assert(vlelctx != NULL);
    Assert(lookup != NULL);
    Assert(fetch != NULL);
    Assert(lookup->key_desc.key != NULL);
    Assert(lookup->key_desc.key->type == AGTV_STRING);

    fetch->graph_oid = vlelctx->graph_oid;
    fetch->ggctx = lookup->ggctx;
    fetch->property_key = lookup->key_desc.key;
}

void age_vle_context_set_terminal_property_result(
    VLE_local_context *vlelctx, Datum property, bool is_null)
{
    Assert(vlelctx != NULL);

    vlelctx->output.terminal_property_result = property;
    vlelctx->output.terminal_property_result_is_null = is_null;
    vlelctx->output.terminal_property_result_valid = true;
}

void age_vle_context_clear_terminal_property_result(VLE_local_context *vlelctx)
{
    Assert(vlelctx != NULL);

    vlelctx->output.terminal_property_result = (Datum) 0;
    vlelctx->output.terminal_property_result_is_null = true;
    vlelctx->output.terminal_property_result_valid = false;
}

void age_vle_context_get_terminal_property_result(
    VLE_local_context *vlelctx, Datum *property, bool *is_null)
{
    Assert(vlelctx != NULL);
    Assert(property != NULL);
    Assert(is_null != NULL);
    Assert(vlelctx->output.terminal_property_result_valid);

    *property = vlelctx->output.terminal_property_result;
    *is_null = vlelctx->output.terminal_property_result_is_null;
}

void age_vle_context_reset_terminal_property_batch(VLE_local_context *vlelctx)
{
    Assert(vlelctx != NULL);

    age_vle_terminal_property_batch_reset(
        &vlelctx->output.terminal_property_batch);
}

bool age_vle_context_terminal_property_batch_materialized(
    VLE_local_context *vlelctx)
{
    Assert(vlelctx != NULL);

    return vlelctx->output.terminal_property_batch.materialized;
}

void age_vle_context_append_terminal_property_batch_id(
    VLE_local_context *vlelctx, graphid terminal_id)
{
    VLETerminalPropertyBatchState *batch;

    Assert(vlelctx != NULL);

    batch = &vlelctx->output.terminal_property_batch;
    if (batch->count >= batch->capacity)
    {
        int64 new_capacity = batch->capacity > 0 ?
            batch->capacity * 2 : 256;

        if (batch->ids == NULL)
            batch->ids = palloc(sizeof(graphid) * new_capacity);
        else
            batch->ids = repalloc(batch->ids,
                                  sizeof(graphid) * new_capacity);
        batch->capacity = new_capacity;
    }

    batch->ids[batch->count++] = terminal_id;
}

void age_vle_context_fetch_terminal_property_batch(
    VLE_local_context *vlelctx, const VLETerminalPropertyBatchFetch *fetch)
{
    VLETerminalPropertyBatchState *batch;

    Assert(vlelctx != NULL);
    Assert(fetch != NULL);

    batch = &vlelctx->output.terminal_property_batch;
    if (batch->count > 0)
    {
        batch->results = palloc0(sizeof(Datum) * batch->count);
        batch->nulls = palloc(sizeof(bool) * batch->count);
        memset(batch->nulls, true, sizeof(bool) * batch->count);
        age_vle_terminal_property_batch_fetch(batch, fetch);
    }

    batch->emit_index = 0;
    batch->materialized = true;
}

bool age_vle_context_next_terminal_property_batch_result(
    VLE_local_context *vlelctx, Datum *property, bool *is_null)
{
    VLETerminalPropertyBatchState *batch;
    int64 index;

    Assert(vlelctx != NULL);
    Assert(property != NULL);
    Assert(is_null != NULL);

    batch = &vlelctx->output.terminal_property_batch;
    Assert(batch->materialized);

    if (batch->emit_index >= batch->count)
        return false;

    index = batch->emit_index++;
    *property = batch->results[index];
    *is_null = batch->nulls[index];

    return true;
}

void age_vle_context_init_acceptance(
    VLE_local_context *vlelctx, VLETraversalAcceptance *acceptance,
    bool require_terminal)
{
    Assert(vlelctx != NULL);
    Assert(acceptance != NULL);

    age_vle_acceptance_init(acceptance, vlelctx->root.lidx,
                            vlelctx->root.uidx,
                            vlelctx->root.uidx_infinite);
    if (require_terminal)
        age_vle_acceptance_require_terminal(acceptance, vlelctx->root.veid);
    if (label_id_is_valid(vlelctx->terminal_endpoint_label_id) &&
        !age_vle_context_reverse_output_path(vlelctx))
    {
        age_vle_acceptance_require_terminal_label(
            acceptance, vlelctx->terminal_endpoint_label_id);
    }
}

bool age_vle_context_has_zero_bound_start(VLE_local_context *vlelctx)
{
    Assert(vlelctx != NULL);

    return !age_vle_context_is_empty_length_range(vlelctx) &&
           vlelctx->root.lidx == 0 &&
           age_vle_context_should_emit_zero_bound(vlelctx);
}

bool age_vle_context_can_advance_start_vertex(VLE_local_context *vlelctx)
{
    Assert(vlelctx != NULL);

    return vlelctx->root.path_function == VLE_FUNCTION_PATHS_ALL ||
           vlelctx->root.path_function == VLE_FUNCTION_PATHS_TO;
}

bool age_vle_context_search_is_complete(
    VLE_local_context *vlelctx, bool found_path)
{
    Assert(vlelctx != NULL);

    return found_path ||
           vlelctx->root.next_vertex == NULL ||
           vlelctx->root.path_function == VLE_FUNCTION_PATHS_BETWEEN ||
           vlelctx->root.path_function == VLE_FUNCTION_PATHS_FROM;
}

void age_vle_context_advance_start_vertex(VLE_local_context *vlelctx)
{
    Assert(vlelctx != NULL);
    Assert(vlelctx->root.next_vertex != NULL);

    vlelctx->root.vsid = get_graphid(vlelctx->root.next_vertex);
    vlelctx->root.next_vertex = next_GraphIdNode(vlelctx->root.next_vertex);
    reset_root_empty_completion_summary(vlelctx);
}

void age_vle_context_remember_vertex_entry(
    VLE_local_context *vlelctx, graphid vertex_id, vertex_entry *entry)
{
    Assert(vlelctx != NULL);

    vlelctx->cached_vertex_id = vertex_id;
    vlelctx->cached_vertex_entry = entry;
}

vertex_entry *age_vle_context_get_cached_vertex_entry(
    VLE_local_context *vlelctx, graphid vertex_id)
{
    Assert(vlelctx != NULL);

    if (vlelctx->cached_vertex_entry == NULL ||
        vlelctx->cached_vertex_id != vertex_id)
    {
        return NULL;
    }

    return vlelctx->cached_vertex_entry;
}

vertex_entry *age_vle_context_frame_vertex_entry(
    VLE_local_context *vlelctx, vertex_entry *entry)
{
    Assert(vlelctx != NULL);

    return vlelctx->output.carry_frame_vertex_entry ? entry : NULL;
}

bool age_vle_context_carries_frame_vertex_entry(VLE_local_context *vlelctx)
{
    Assert(vlelctx != NULL);

    return vlelctx->output.carry_frame_vertex_entry;
}

bool age_vle_context_uses_local_edge_state(VLE_local_context *vlelctx)
{
    Assert(vlelctx != NULL);

    return vlelctx->traversal.edge_state.use_local;
}

bool age_vle_context_has_edge_label(VLE_local_context *vlelctx)
{
    Assert(vlelctx != NULL);

    return OidIsValid(vlelctx->edge_label_name_oid);
}

Oid age_vle_context_edge_label_oid(VLE_local_context *vlelctx)
{
    Assert(vlelctx != NULL);

    return vlelctx->edge_label_name_oid;
}

bool age_vle_context_has_edge_property_constraints(
    VLE_local_context *vlelctx)
{
    Assert(vlelctx != NULL);

    return vlelctx->num_edge_property_constraints > 0;
}

static void free_edge_property_constraints(VLE_local_context *vlelctx)
{
    int index;

    Assert(vlelctx != NULL);

    if (vlelctx->edge_property_constraint_pairs == NULL)
    {
        vlelctx->num_cached_edge_property_constraints = 0;
        return;
    }

    for (index = 0; index < vlelctx->num_cached_edge_property_constraints;
         index++)
    {
        agtype_pair *pair = &vlelctx->edge_property_constraint_pairs[index];

        pfree_if_not_null(pair->key.val.string.val);
        free_cached_property_value(&pair->value);
    }

    pfree(vlelctx->edge_property_constraint_pairs);
    vlelctx->edge_property_constraint_pairs = NULL;
    vlelctx->num_cached_edge_property_constraints = 0;
}

static void free_context_identity(VLE_local_context *vlelctx)
{
    Assert(vlelctx != NULL);

    if (vlelctx->graph_name != NULL)
    {
        pfree_if_not_null(vlelctx->graph_name);
        vlelctx->graph_name = NULL;
    }

    if (vlelctx->edge_label_name != NULL)
    {
        pfree_if_not_null(vlelctx->edge_label_name);
        vlelctx->edge_label_name = NULL;
    }
}

static void free_terminal_output_resources(VLE_local_context *vlelctx)
{
    Assert(vlelctx != NULL);

    if (vlelctx->output.emit_terminal_property &&
        vlelctx->output.terminal_property_key.type == AGTV_STRING)
    {
        pfree_if_not_null(
            vlelctx->output.terminal_property_key.val.string.val);
        vlelctx->output.terminal_property_key.val.string.val = NULL;
        vlelctx->output.terminal_property_key.val.string.len = 0;
        vlelctx->output.emit_terminal_property = false;
    }
    if (vlelctx->root.terminal_property_predicate_key.type == AGTV_STRING)
    {
        pfree_if_not_null(
            vlelctx->root.terminal_property_predicate_key.val.string.val);
        vlelctx->root.terminal_property_predicate_key.val.string.val = NULL;
        vlelctx->root.terminal_property_predicate_key.val.string.len = 0;
        vlelctx->root.terminal_property_predicate_known = false;
    }
}

HTAB *age_vle_ensure_edge_property_relation_cache(
    VLE_local_context *vlelctx, const char *cache_name)
{
    MemoryContext oldctx;

    Assert(vlelctx != NULL);

    if (vlelctx->edge_property_relation_cache != NULL)
        return vlelctx->edge_property_relation_cache;

    oldctx = MemoryContextSwitchTo(TopMemoryContext);
    vlelctx->edge_property_relation_cache =
        create_entry_property_relation_cache(cache_name);
    MemoryContextSwitchTo(oldctx);

    return vlelctx->edge_property_relation_cache;
}

void age_vle_context_init_edge_property_match(
    VLE_local_context *vlelctx, VLEEdgePropertyMatchContext *match_context)
{
    Assert(vlelctx != NULL);
    Assert(match_context != NULL);

    match_context->constraint = vlelctx->edge_property_constraint;
    match_context->constraint_datum = vlelctx->edge_property_constraint_datum;
    match_context->constraint_hash = vlelctx->edge_property_constraint_hash;
    match_context->cached_constraints =
        vlelctx->edge_property_constraint_pairs;
    match_context->constraint_count =
        vlelctx->num_edge_property_constraints;
    match_context->cached_constraint_count =
        vlelctx->num_cached_edge_property_constraints;
    match_context->relation_cache = match_context->constraint_count > 0 ?
        age_vle_ensure_edge_property_relation_cache(
            vlelctx, "VLE edge match relation cache") : NULL;
}

graphid age_vle_context_current_terminal_vertex_id(VLE_local_context *vlelctx)
{
    GraphIdStack *vertex_stack;
    int ssize;

    Assert(vlelctx != NULL);

    vertex_stack = vlelctx->traversal.path_vertex_stack;
    if (vertex_stack == NULL)
        return age_vle_context_start_vertex_id(vlelctx);

    ssize = gid_stack_size(vlelctx->traversal.path_stack);
    Assert(gid_stack_size(vertex_stack) == ssize + 1);

    return age_vle_context_reverse_output_path(vlelctx) ?
        vertex_stack->array[0] : vertex_stack->array[ssize];
}

void age_vle_context_init_container_build_input(
    VLE_local_context *vlelctx, VLEContainerBuildInput *input)
{
    Assert(vlelctx != NULL);
    Assert(input != NULL);

    input->graph_oid = vlelctx->graph_oid;
    input->start_vertex_id = age_vle_context_start_vertex_id(vlelctx);
    input->reverse_output_path =
        age_vle_context_reverse_output_path(vlelctx);
    input->path_stack = vlelctx->traversal.path_stack;
    input->path_vertex_stack = vlelctx->traversal.path_vertex_stack;
}

bool age_vle_context_init_source_cursor(
    VLE_local_context *vlelctx, VLEContextSourceCursor *cursor,
    graphid source_vertex_id, bool outgoing, bool skip_self_loops)
{
    const VLETraversalSourceLayout *layout;
    const VLETraversalDirectedSource *source;
    bool direction_matches;

    Assert(vlelctx != NULL);
    Assert(cursor != NULL);

    layout = &vlelctx->root.source_layout;
    if (!layout->label_constrained)
        return false;

    if (outgoing)
    {
        direction_matches = layout->direction == CYPHER_REL_DIR_RIGHT ||
                            layout->direction == CYPHER_REL_DIR_NONE;
        source = &layout->outgoing_source;
    }
    else
    {
        direction_matches = layout->direction == CYPHER_REL_DIR_LEFT ||
                            layout->direction == CYPHER_REL_DIR_NONE;
        source = &layout->incoming_source;
    }

    if (!direction_matches || source->kind == VLE_TRAVERSAL_SOURCE_NONE)
        return false;

    memset(cursor, 0, sizeof(*cursor));
    cursor->source_vertex_id = source_vertex_id;
    cursor->source_kind = source->kind;
    cursor->index_oid = source->index_oid;
    cursor->edge_label_oid = vlelctx->edge_label_name_oid;
    cursor->edge_label_id =
        get_graph_edge_label_id(vlelctx->ggctx, vlelctx->edge_label_name_oid);
    cursor->target_path_length = vlelctx->traversal.path_depth + 1;
    cursor->terminal_label_id =
        age_vle_context_label_id_for_target_path_length(
            vlelctx, cursor->target_path_length);
    cursor->outgoing = outgoing;
    cursor->skip_self_loops = skip_self_loops;
    cursor->has_property_constraints = layout->has_property_constraints;

    return true;
}

void age_vle_context_init_expansion_source_run(
    VLEContextExpansionSourceRun *run, graphid source_vertex_id)
{
    Assert(run != NULL);

    memset(run, 0, sizeof(*run));
    run->source_vertex_id = source_vertex_id;
}

void age_vle_context_init_missing_vertex_source_run(
    VLE_local_context *vlelctx, VLEContextExpansionSourceRun *run,
    graphid source_vertex_id)
{
    Assert(vlelctx != NULL);

    age_vle_context_init_expansion_source_run(run, source_vertex_id);
    run->source_path_length = vlelctx->traversal.path_depth;
    run->missing_vertex_fallback = true;
    run->missing_vertex_eligible =
        age_vle_context_uses_local_edge_state(vlelctx) &&
        age_vle_context_has_edge_label(vlelctx) &&
        !age_vle_context_has_edge_property_constraints(vlelctx);
}

bool age_vle_context_expansion_source_run_is_eligible(
    const VLEContextExpansionSourceRun *run)
{
    Assert(run != NULL);

    return !run->missing_vertex_fallback || run->missing_vertex_eligible;
}

bool age_vle_context_init_expansion_source_cursor(
    VLE_local_context *vlelctx, VLEContextExpansionSourceRun *run,
    VLEContextSourceCursor *cursor, bool outgoing)
{
    bool skip_self_loops;

    Assert(vlelctx != NULL);
    Assert(run != NULL);

    if (!age_vle_context_expansion_source_run_is_eligible(run))
        return false;

    if (run->missing_vertex_fallback)
    {
        skip_self_loops = !outgoing &&
            age_vle_context_edge_direction(vlelctx) == CYPHER_REL_DIR_NONE;
    }
    else
    {
        skip_self_loops = !outgoing && run->used_out_source;
    }

    return age_vle_context_init_source_cursor(
        vlelctx, cursor, run->source_vertex_id, outgoing, skip_self_loops);
}

bool age_vle_context_expansion_source_cursor_known_empty(
    VLE_local_context *vlelctx, const VLEContextSourceCursor *cursor)
{
    Assert(vlelctx != NULL);
    Assert(cursor != NULL);

    if (!age_vle_context_cursor_known_empty(vlelctx, cursor))
        return false;

    age_vle_context_record_age_adjacency_empty_source_run_skip(
        vlelctx, cursor->outgoing);

    return true;
}

void age_vle_context_record_expansion_source_result(
    VLEContextExpansionSourceRun *run, bool outgoing, bool used_source)
{
    Assert(run != NULL);

    if (outgoing)
        run->used_out_source = used_source;
    else
        run->used_in_source = used_source;
}

bool age_vle_context_missing_vertex_sources_known_empty(
    VLE_local_context *vlelctx, graphid source_vertex_id)
{
    VLEContextExpansionSourceRun run;
    bool saw_known_empty = false;
    bool out_known_empty = false;
    bool in_known_empty = false;
    bool outgoing;

    Assert(vlelctx != NULL);

    age_vle_context_init_missing_vertex_source_run(vlelctx, &run,
                                                   source_vertex_id);
    if (!age_vle_context_expansion_source_run_is_eligible(&run))
        return false;

    for (outgoing = true; ; outgoing = false)
    {
        VLEContextSourceCursor cursor;

        if (age_vle_context_init_expansion_source_cursor(
                vlelctx, &run, &cursor, outgoing))
        {
            if (!age_vle_context_cursor_known_empty(vlelctx, &cursor))
                return false;

            if (outgoing)
                out_known_empty = true;
            else
                in_known_empty = true;
            saw_known_empty = true;
        }

        if (!outgoing)
            break;
    }

    if (!saw_known_empty)
        return false;

    if (out_known_empty)
        age_vle_context_record_age_adjacency_empty_source_run_skip(vlelctx,
                                                                   true);
    if (in_known_empty)
        age_vle_context_record_age_adjacency_empty_source_run_skip(vlelctx,
                                                                   false);

    return true;
}

static bool age_vle_context_cursor_known_empty(
    VLE_local_context *vlelctx, const VLEContextSourceCursor *cursor)
{
    VLEAdjacencyPayloadCacheEntry *cache_entry;

    Assert(vlelctx != NULL);
    Assert(cursor != NULL);

    if (cursor->source_kind != VLE_TRAVERSAL_SOURCE_AGE_ADJACENCY ||
        cursor->has_property_constraints ||
        !age_vle_context_uses_local_edge_state(vlelctx) ||
        !OidIsValid(cursor->index_oid))
    {
        return false;
    }

    cache_entry = age_vle_adjacency_payload_cache_lookup(
        vlelctx->root.age_adjacency_payload_cache, cursor->index_oid,
        cursor->source_vertex_id, cursor->terminal_label_id,
        age_vle_context_terminal_property_filter_id(vlelctx));

    return cache_entry != NULL && cache_entry->known_empty;
}

static void age_vle_context_init_packed_adjacency_lists(
    VLE_local_context *vlelctx, vertex_entry *entry,
    VLEContextPackedAdjacencyLists *lists)
{
    bool label_constrained;

    Assert(vlelctx != NULL);
    Assert(entry != NULL);
    Assert(lists != NULL);

    memset(lists, 0, sizeof(*lists));
    label_constrained = age_vle_context_has_edge_label(vlelctx);

    if (vlelctx->root.edge_direction == CYPHER_REL_DIR_RIGHT ||
        vlelctx->root.edge_direction == CYPHER_REL_DIR_NONE)
    {
        lists->edge_out = label_constrained ?
            get_vertex_entry_adj_edges_out_for_label(
                entry, vlelctx->edge_label_name_oid) :
            get_vertex_entry_adj_edges_out(entry);
    }
    if (vlelctx->root.edge_direction == CYPHER_REL_DIR_LEFT ||
        vlelctx->root.edge_direction == CYPHER_REL_DIR_NONE)
    {
        lists->edge_in = label_constrained ?
            get_vertex_entry_adj_edges_in_for_label(
                entry, vlelctx->edge_label_name_oid) :
            get_vertex_entry_adj_edges_in(entry);
    }
    lists->edge_self = label_constrained ?
        get_vertex_entry_adj_edges_self_for_label(
            entry, vlelctx->edge_label_name_oid) :
        get_vertex_entry_adj_edges_self(entry);
}

static void init_packed_adjacency_source_lists(
    VLE_local_context *vlelctx, vertex_entry *entry,
    bool suppress_out, bool suppress_in, bool suppress_self,
    VLEContextPackedAdjacencySource *source)
{
    Assert(source != NULL);

    age_vle_context_init_packed_adjacency_lists(vlelctx, entry,
                                                &source->lists);
    if (suppress_out && source->lists.edge_out != NULL)
    {
        source->lists.edge_out = NULL;
        source->suppress_out = true;
    }
    if (suppress_in && source->lists.edge_in != NULL)
    {
        source->lists.edge_in = NULL;
        source->suppress_in = true;
    }
    if (suppress_self && source->lists.edge_self != NULL)
    {
        source->lists.edge_self = NULL;
        source->suppress_self = true;
    }
}

VLEContextPackedAdjacencySource *
age_vle_context_begin_packed_adjacency_source_from_run(
    VLE_local_context *vlelctx, vertex_entry *entry,
    const VLEContextExpansionSourceRun *run)
{
    bool suppress_out;
    bool suppress_in;
    bool suppress_self;
    VLEContextPackedAdjacencySource *source;

    Assert(vlelctx != NULL);
    Assert(entry != NULL);
    Assert(run != NULL);

    suppress_out = run->used_out_source;
    suppress_in = run->used_in_source;
    suppress_self = suppress_out || suppress_in;
    if (packed_adjacency_source_policy_suppresses_all(
            vlelctx, suppress_out, suppress_in, suppress_self))
    {
        age_vle_context_record_packed_policy_skip(vlelctx);
        return NULL;
    }

    source = palloc0(sizeof(*source));
    init_packed_adjacency_source_lists(
        vlelctx, entry, suppress_out, suppress_in, suppress_self, source);
    age_vle_context_record_packed_suppression(
        vlelctx, source->suppress_out, source->suppress_in,
        source->suppress_self);

    if (!packed_adjacency_lists_have_source(&source->lists))
    {
        age_vle_context_record_packed_empty_skip(vlelctx);
        pfree(source);
        return NULL;
    }

    return source;
}

static bool packed_adjacency_source_policy_suppresses_all(
    VLE_local_context *vlelctx,
    bool suppress_out, bool suppress_in, bool suppress_self)
{
    const VLETraversalSourceLayout *layout;
    bool may_have_out;
    bool may_have_in;

    Assert(vlelctx != NULL);

    layout = &vlelctx->root.source_layout;
    if (!layout->use_local_edge_state ||
        !layout->label_constrained ||
        layout->has_property_constraints)
    {
        return false;
    }

    may_have_out = layout->direction == CYPHER_REL_DIR_RIGHT ||
        layout->direction == CYPHER_REL_DIR_NONE;
    may_have_in = layout->direction == CYPHER_REL_DIR_LEFT ||
        layout->direction == CYPHER_REL_DIR_NONE;

    suppress_out = suppress_out &&
        layout->outgoing_source.kind != VLE_TRAVERSAL_SOURCE_NONE;
    suppress_in = suppress_in &&
        layout->incoming_source.kind != VLE_TRAVERSAL_SOURCE_NONE;

    return (!may_have_out || suppress_out) &&
        (!may_have_in || suppress_in) &&
        suppress_self;
}

bool age_vle_context_packed_adjacency_source_next(
    VLEContextPackedAdjacencySource *source, GraphEdgeAdjEntry **adj_entry)
{
    Assert(source != NULL);

    return next_packed_adjacency_entry(
        source->lists.edge_out, &source->edge_out_idx,
        source->lists.edge_in, &source->edge_in_idx,
        source->lists.edge_self, &source->edge_self_idx, adj_entry);
}

void age_vle_context_end_packed_adjacency_source(
    VLEContextPackedAdjacencySource *source)
{
    if (source != NULL)
        pfree(source);
}

static bool packed_adjacency_lists_have_source(
    const VLEContextPackedAdjacencyLists *lists)
{
    Assert(lists != NULL);

    return (lists->edge_out != NULL && lists->edge_out->size > 0) ||
           (lists->edge_in != NULL && lists->edge_in->size > 0) ||
           (lists->edge_self != NULL && lists->edge_self->size > 0);
}

static bool next_packed_adjacency_entry(GraphEdgeAdjList *edge_out,
                                        int64 *edge_out_idx,
                                        GraphEdgeAdjList *edge_in,
                                        int64 *edge_in_idx,
                                        GraphEdgeAdjList *edge_self,
                                        int64 *edge_self_idx,
                                        GraphEdgeAdjEntry **adj_entry)
{
    Assert(adj_entry != NULL);

    if (edge_out != NULL && *edge_out_idx < edge_out->size)
    {
        *adj_entry = &edge_out->items[*edge_out_idx];
        (*edge_out_idx)++;
        return true;
    }

    if (edge_in != NULL && *edge_in_idx < edge_in->size)
    {
        *adj_entry = &edge_in->items[*edge_in_idx];
        (*edge_in_idx)++;
        return true;
    }

    if (edge_self != NULL && *edge_self_idx < edge_self->size)
    {
        *adj_entry = &edge_self->items[*edge_self_idx];
        (*edge_self_idx)++;
        return true;
    }

    *adj_entry = NULL;
    return false;
}

static AgeAdjacencyVisiblePayloadScan *age_vle_context_get_payload_scan(
    VLE_local_context *vlelctx, const VLEContextSourceCursor *cursor)
{
    AgeAdjacencyVisiblePayloadScan **payload_scan;

    Assert(vlelctx != NULL);
    Assert(cursor != NULL);
    Assert(cursor->source_kind == VLE_TRAVERSAL_SOURCE_AGE_ADJACENCY);
    Assert(OidIsValid(cursor->index_oid));

    if (cursor->outgoing)
        payload_scan = &vlelctx->root.age_adjacency_out_scan;
    else
        payload_scan = &vlelctx->root.age_adjacency_in_scan;

    if (*payload_scan == NULL)
    {
        *payload_scan = age_adjacency_begin_visible_payload_scan(
            cursor->index_oid, GetActiveSnapshot(), false);
    }

    return *payload_scan;
}

static VLEAdjacencyPayloadCacheEntry *age_vle_context_get_payload_cache_entry(
    VLE_local_context *vlelctx, const VLEContextSourceCursor *cursor,
    uint32 terminal_property_filter_id, bool *found)
{
    Assert(vlelctx != NULL);
    Assert(cursor != NULL);
    Assert(found != NULL);

    return age_vle_adjacency_payload_cache_get(
        &vlelctx->root.age_adjacency_payload_cache, cursor->index_oid,
        cursor->source_vertex_id, cursor->terminal_label_id,
        terminal_property_filter_id, found);
}

static uint32 age_vle_context_terminal_property_filter_id(
    VLE_local_context *vlelctx)
{
    Assert(vlelctx != NULL);

    if (!vlelctx->root.terminal_property_prefilter_eligible ||
        !OidIsValid(vlelctx->root.terminal_property_index_oid))
    {
        return 0;
    }

    return age_adjacency_property_filter_id(
        vlelctx->root.terminal_property_index_oid,
        vlelctx->root.terminal_property_predicate_value,
        vlelctx->root.terminal_property_predicate_null);
}

static bool age_vle_context_path_length_targets_terminal_depth(
    VLE_local_context *vlelctx, int64 target_path_length)
{
    Assert(vlelctx != NULL);

    return !vlelctx->root.uidx_infinite &&
        target_path_length >= vlelctx->root.uidx;
}

static bool age_vle_context_cursor_targets_terminal_property_depth(
    VLE_local_context *vlelctx, const VLEContextSourceCursor *cursor)
{
    Assert(vlelctx != NULL);
    Assert(cursor != NULL);

    return !vlelctx->root.uidx_infinite &&
        cursor->target_path_length > vlelctx->root.uidx;
}

static int32 age_vle_context_label_id_for_target_path_length(
    VLE_local_context *vlelctx, int64 target_path_length)
{
    Assert(vlelctx != NULL);

    if (label_id_is_valid(vlelctx->terminal_label_id))
        return vlelctx->terminal_label_id;

    if (label_id_is_valid(vlelctx->terminal_endpoint_label_id) &&
        age_vle_context_endpoint_label_can_prune_source(
            vlelctx, target_path_length))
    {
        return vlelctx->terminal_endpoint_label_id;
    }

    return INVALID_LABEL_ID;
}

static bool age_vle_context_endpoint_label_can_prune_source(
    VLE_local_context *vlelctx, int64 target_path_length)
{
    Assert(vlelctx != NULL);

    return !vlelctx->root.uidx_infinite &&
           vlelctx->root.lidx == vlelctx->root.uidx &&
           age_vle_context_path_length_targets_terminal_depth(
               vlelctx, target_path_length);
}

static AgeAdjacencyMatchTerminalPropertyLookup *
age_vle_context_get_terminal_property_lookup(VLE_local_context *vlelctx)
{
    AgeAdjacencyMatchTerminalPropertyRequest request;
    char *property_key;

    Assert(vlelctx != NULL);

    if (!vlelctx->root.terminal_property_prefilter_eligible)
        return NULL;
    if (vlelctx->root.terminal_property_lookup != NULL)
        return vlelctx->root.terminal_property_lookup;
    if (vlelctx->output.terminal_property_key.type != AGTV_STRING ||
        vlelctx->output.terminal_property_key.val.string.val == NULL)
    {
        return NULL;
    }

    property_key = pnstrdup(
        vlelctx->output.terminal_property_key.val.string.val,
        vlelctx->output.terminal_property_key.val.string.len);
    request.graph_oid = vlelctx->graph_oid;
    request.right_label_id = label_id_is_valid(vlelctx->terminal_label_id) ?
        vlelctx->terminal_label_id : vlelctx->terminal_endpoint_label_id;
    request.has_property_predicate = true;
    request.metadata_backed = true;
    request.property_index_oid = vlelctx->root.terminal_property_index_oid;
    request.property_key = property_key;
    request.property_value = vlelctx->root.terminal_property_predicate_value;
    request.property_value_isnull =
        vlelctx->root.terminal_property_predicate_null;
    vlelctx->root.terminal_property_lookup =
        age_adjacency_match_terminal_property_begin(
            &request, CurrentMemoryContext);
    pfree(property_key);

    return vlelctx->root.terminal_property_lookup;
}

static void age_vle_context_prepare_terminal_property_prefilter(
    VLE_local_context *vlelctx, VLEContextAgeAdjacencyPayloadSource *source)
{
    AgeAdjacencyMatchTerminalPropertyLookup *lookup;
    int64 run_postings;
    int64 active_postings;

    Assert(vlelctx != NULL);
    Assert(source != NULL);

    if (!vlelctx->root.terminal_property_prefilter_eligible ||
        source->payload_scan == NULL ||
        source->terminal_property_filter_id == 0 ||
        !label_id_is_valid(source->terminal_label_id))
    {
        return;
    }

    lookup = age_vle_context_get_terminal_property_lookup(vlelctx);
    if (lookup == NULL)
        return;

    age_adjacency_match_terminal_property_set_value(
        lookup, vlelctx->root.terminal_property_predicate_value,
        vlelctx->root.terminal_property_predicate_null);
    run_postings = age_adjacency_visible_payload_scan_run_postings(
        source->payload_scan);
    active_postings = age_adjacency_visible_payload_scan_active_postings(
        source->payload_scan);
    vlelctx->source_stats.age_adjacency_payload_property_prefilter_runs++;
    vlelctx->source_stats.age_adjacency_payload_property_prefilter_candidates +=
        active_postings;

    if (!age_adjacency_match_terminal_property_prepare_prefilter(
            lookup, run_postings, active_postings,
            vlelctx->root.terminal_property_prefetch_threshold))
    {
        return;
    }

    vlelctx->source_stats.age_adjacency_payload_property_prefetch_matches =
        Max(vlelctx->source_stats.age_adjacency_payload_property_prefetch_matches,
            age_adjacency_match_terminal_property_prefetched_matches(lookup));

    if (age_adjacency_match_terminal_property_prefetched_matches(lookup) == 0)
    {
        source->empty_suppressed = true;
        return;
    }

    {
        AgeAdjacencyCompositeTerminalFilter composite_filter;

        memset(&composite_filter, 0, sizeof(composite_filter));
        composite_filter.terminal_label_id = source->terminal_label_id;
        composite_filter.property_index_oid =
            age_adjacency_match_terminal_property_index_oid(lookup);
        composite_filter.property_filter_id = source->terminal_property_filter_id;
        composite_filter.property_match_count =
            age_adjacency_match_terminal_property_prefetched_matches(lookup);
        composite_filter.has_property_summary = true;
        if (age_adjacency_match_terminal_property_prefilter_set(
                lookup, &composite_filter.vertex_set_filter))
        {
            composite_filter.has_vertex_set_filter = true;
            composite_filter.source = "label-property-prefetch";
            age_adjacency_visible_payload_scan_set_composite_terminal_filter(
                source->payload_scan, &composite_filter);
            vlelctx->source_stats.age_adjacency_payload_property_vertex_set_runs++;
            return;
        }
    }

    {
        AgeAdjacencyCompositeTerminalFilter composite_filter;

        memset(&composite_filter, 0, sizeof(composite_filter));
        composite_filter.terminal_label_id = source->terminal_label_id;
        composite_filter.property_index_oid =
            age_adjacency_match_terminal_property_index_oid(lookup);
        composite_filter.property_filter_id = source->terminal_property_filter_id;
        composite_filter.property_match_count =
            age_adjacency_match_terminal_property_prefetched_matches(lookup);
        composite_filter.vertex_filter =
            age_adjacency_match_terminal_property_prefilter_matches;
        composite_filter.vertex_filter_state = lookup;
        composite_filter.has_property_summary = true;
        composite_filter.has_vertex_filter = true;
        composite_filter.source = "label-property-callback";
        age_adjacency_visible_payload_scan_set_composite_terminal_filter(
            source->payload_scan, &composite_filter);
    }
}

VLEContextAgeAdjacencyPayloadSource *
age_vle_context_begin_age_adjacency_payload_source(
    VLE_local_context *vlelctx, const VLEContextSourceCursor *cursor)
{
    bool found;
    VLEContextAgeAdjacencyPayloadSource *source;

    Assert(vlelctx != NULL);
    Assert(cursor != NULL);
    Assert(cursor->source_kind == VLE_TRAVERSAL_SOURCE_AGE_ADJACENCY);

    if (!OidIsValid(cursor->index_oid))
        return NULL;

    source = palloc0(sizeof(*source));
    source->source_vertex_id = cursor->source_vertex_id;
    source->index_oid = cursor->index_oid;
    source->terminal_label_id = cursor->terminal_label_id;
    source->outgoing = cursor->outgoing;
    source->terminal_property_filter_id =
        age_vle_context_cursor_targets_terminal_property_depth(vlelctx,
                                                               cursor) ?
        age_vle_context_terminal_property_filter_id(vlelctx) :
        0;
    source->use_payload_cache =
        age_vle_context_uses_local_edge_state(vlelctx) &&
        !cursor->has_property_constraints;

    if (source->use_payload_cache)
    {
        source->cache_entry = age_vle_context_get_payload_cache_entry(
            vlelctx, cursor, source->terminal_property_filter_id, &found);
        if (found && source->cache_entry->payloads != NULL)
        {
            source->replay_payload_cache = true;
            age_vle_context_record_age_adjacency_payload_replay_run(vlelctx);
            return source;
        }
        if (found && source->cache_entry->known_empty)
        {
            age_vle_context_record_age_adjacency_empty_source_cache_hit(
                vlelctx, cursor->outgoing);
            source->empty_suppressed = true;
            return source;
        }
        age_vle_adjacency_payload_cache_discard(source->cache_entry);
    }

    source->payload_scan = age_vle_context_get_payload_scan(vlelctx, cursor);
    {
        AgeAdjacencyCompositeTerminalFilter composite_filter;

        memset(&composite_filter, 0, sizeof(composite_filter));
        composite_filter.terminal_label_id = cursor->terminal_label_id;
        composite_filter.source = "label";
        age_adjacency_visible_payload_scan_set_composite_terminal_filter(
            source->payload_scan, &composite_filter);
    }
    age_adjacency_visible_payload_scan_reset_runtime(source->payload_scan);

    if (!age_adjacency_visible_payload_scan_begin_key(
            source->payload_scan, source->source_vertex_id))
    {
        age_vle_context_record_age_adjacency_empty_source_skip(
            vlelctx, cursor->outgoing);
        if (source->use_payload_cache)
        {
            age_vle_adjacency_payload_cache_mark_empty(source->cache_entry);
        }
        source->empty_suppressed = true;
    }
    else
    {
        age_vle_context_prepare_terminal_property_prefilter(vlelctx, source);
        if (source->empty_suppressed)
        {
            age_vle_context_record_age_adjacency_empty_source_skip(
                vlelctx, cursor->outgoing);
            if (source->use_payload_cache)
                age_vle_adjacency_payload_cache_mark_empty(
                    source->cache_entry);
            return source;
        }
        age_vle_context_record_age_adjacency_payload_scan_run(vlelctx);
    }

    return source;
}

bool age_vle_context_age_adjacency_payload_next(
    VLE_local_context *vlelctx, VLEContextAgeAdjacencyPayloadSource *source,
    AgeAdjacencyPayload *payload)
{
    Assert(vlelctx != NULL);
    Assert(source != NULL);
    Assert(payload != NULL);

    if (source->empty_suppressed)
        return false;

    if (source->replay_payload_cache)
    {
        if (source->replay_index >= source->cache_entry->count)
            return false;

        *payload = source->cache_entry->payloads[source->replay_index++];
        age_vle_context_record_age_adjacency_payload_replay(vlelctx);
        return true;
    }

    if (!age_adjacency_visible_payload_scan_next(source->payload_scan,
                                                 payload))
        return false;

    if (source->use_payload_cache)
    {
        if (!source->pending_payload_valid)
        {
            source->pending_payload = *payload;
            source->pending_payload_valid = true;
        }
        else
        {
            if (source->cache_entry->payloads == NULL)
            {
                age_vle_adjacency_payload_cache_append(
                    source->cache_entry, &source->pending_payload);
                age_vle_context_record_age_adjacency_payload_cache_seed(
                    vlelctx);
                if (!source->cache_seed_run_recorded)
                {
                    age_vle_context_record_age_adjacency_payload_cache_seed_run(
                        vlelctx);
                    source->cache_seed_run_recorded = true;
                }
            }
            age_vle_adjacency_payload_cache_append(source->cache_entry,
                                                   payload);
        }
    }
    source->scanned++;
    age_vle_context_record_age_adjacency_payload_scan(vlelctx);

    return true;
}

void age_vle_context_maybe_mark_age_adjacency_frontier_empty(
    VLE_local_context *vlelctx, VLEContextAgeAdjacencyPayloadSource *source,
    graphid next_source_vertex_id)
{
    Assert(vlelctx != NULL);
    Assert(source != NULL);

    if (!source->use_payload_cache ||
        source->payload_scan == NULL ||
        OidIsValid(source->index_oid) == false ||
        next_source_vertex_id == source->source_vertex_id)
    {
        return;
    }

    if (!age_adjacency_visible_payload_scan_key_known_empty(
            source->payload_scan, next_source_vertex_id))
    {
        return;
    }

    age_vle_context_queue_age_adjacency_frontier_empty(
        vlelctx, source, next_source_vertex_id);
}

static bool age_vle_context_age_adjacency_frontier_empty_seen(
    VLEContextAgeAdjacencyPayloadSource *source, graphid source_vertex_id)
{
    int64 i;

    Assert(source != NULL);

    for (i = 0; i < source->frontier_empty_count; i++)
    {
        if (source->frontier_empty_keys[i] == source_vertex_id)
            return true;
    }

    return false;
}

static void age_vle_context_queue_age_adjacency_frontier_empty(
    VLE_local_context *vlelctx, VLEContextAgeAdjacencyPayloadSource *source,
    graphid source_vertex_id)
{
    MemoryContext oldctx;
    int64 new_capacity;
    int64 planned_capacity;

    Assert(vlelctx != NULL);
    Assert(source != NULL);

    if (age_vle_context_age_adjacency_frontier_empty_seen(source,
                                                          source_vertex_id))
        return;

    if (source->frontier_empty_count == source->frontier_empty_capacity)
    {
        planned_capacity = age_vle_context_empty_lifecycle_batch_size(
            vlelctx);
        new_capacity = source->frontier_empty_capacity == 0 ?
                       Max(planned_capacity, (int64)8) :
                       source->frontier_empty_capacity * 2;
        oldctx = MemoryContextSwitchTo(TopMemoryContext);
        if (source->frontier_empty_keys == NULL)
        {
            source->frontier_empty_keys = palloc_array(graphid,
                                                       new_capacity);
        }
        else
        {
            source->frontier_empty_keys =
                repalloc_array(source->frontier_empty_keys, graphid,
                               new_capacity);
        }
        MemoryContextSwitchTo(oldctx);
        source->frontier_empty_capacity = new_capacity;
    }

    source->frontier_empty_keys[source->frontier_empty_count++] =
        source_vertex_id;
    age_vle_context_mark_age_adjacency_frontier_empty(
        vlelctx, source, source_vertex_id);
}

static void
age_vle_context_mark_age_adjacency_frontier_empty(
    VLE_local_context *vlelctx, VLEContextAgeAdjacencyPayloadSource *source,
    graphid source_vertex_id)
{
    VLEAdjacencyPayloadCacheEntry *cache_entry;
    bool found;

    Assert(vlelctx != NULL);
    Assert(source != NULL);

    cache_entry = age_vle_adjacency_payload_cache_get(
        &vlelctx->root.age_adjacency_payload_cache, source->index_oid,
        source_vertex_id, source->terminal_label_id,
        source->terminal_property_filter_id, &found);
    if (found && cache_entry->known_empty)
        return;
    if (found && cache_entry->payloads != NULL)
        return;

    age_vle_adjacency_payload_cache_mark_empty(cache_entry);
    age_vle_context_record_age_adjacency_empty_source_frontier_mark(
        vlelctx, source->outgoing);
}

static void age_vle_context_apply_age_adjacency_frontier_empty(
    VLE_local_context *vlelctx, VLEContextAgeAdjacencyPayloadSource *source)
{
    int64 i;

    Assert(vlelctx != NULL);
    Assert(source != NULL);

    age_vle_context_record_age_adjacency_empty_source_frontier_batch(
        vlelctx, source->outgoing, source->frontier_empty_count);
    for (i = 0; i < source->frontier_empty_count; i++)
    {
        age_vle_context_mark_age_adjacency_frontier_empty(
            vlelctx, source, source->frontier_empty_keys[i]);
    }
}

bool age_vle_context_age_adjacency_payload_source_empty_suppressed(
    VLEContextAgeAdjacencyPayloadSource *source)
{
    Assert(source != NULL);

    return source->empty_suppressed;
}

void age_vle_context_end_age_adjacency_payload_source(
    VLE_local_context *vlelctx, VLEContextAgeAdjacencyPayloadSource *source)
{
    Assert(vlelctx != NULL);

    if (source == NULL)
        return;

    if (source->payload_scan != NULL)
    {
        AgeVLESourceStats *stats = &vlelctx->source_stats;

        stats->age_adjacency_payload_property_filtered +=
            age_adjacency_visible_payload_scan_property_filtered(
                source->payload_scan);
        stats->age_adjacency_payload_cache_filtered +=
            age_adjacency_visible_payload_scan_cache_filtered(
                source->payload_scan);
        stats->age_adjacency_payload_cache_label_filtered +=
            age_adjacency_visible_payload_scan_cache_label_filtered(
                source->payload_scan);
        stats->age_adjacency_payload_cache_property_filtered +=
            age_adjacency_visible_payload_scan_cache_property_filtered(
                source->payload_scan);
        stats->age_adjacency_payload_composite_requests +=
            age_adjacency_visible_payload_scan_composite_requests(
                source->payload_scan);
        stats->age_adjacency_payload_composite_block_filtered +=
            age_adjacency_visible_payload_scan_composite_block_filtered(
                source->payload_scan);
        stats->age_adjacency_payload_composite_directory_filtered +=
            age_adjacency_visible_payload_scan_composite_directory_filtered(
                source->payload_scan);
        stats->age_adjacency_payload_composite_directory_estimated +=
            age_adjacency_visible_payload_scan_composite_directory_estimated(
                source->payload_scan);
        stats->age_adjacency_payload_vertex_set_range_filtered +=
            age_adjacency_visible_payload_scan_vertex_set_range_filtered(
                source->payload_scan);
        stats->age_adjacency_payload_vertex_set_sorted_filtered +=
            age_adjacency_visible_payload_scan_vertex_set_sorted_filtered(
                source->payload_scan);
        stats->age_adjacency_payload_vertex_set_block_filtered +=
            age_adjacency_visible_payload_scan_vertex_set_block_filtered(
                source->payload_scan);
        stats->age_adjacency_payload_vertex_set_block_value_filtered +=
            age_adjacency_visible_payload_scan_vertex_set_block_value_filtered(
                source->payload_scan);
        stats->age_adjacency_payload_vertex_set_block_value_posting_filtered +=
            age_adjacency_visible_payload_scan_vertex_set_block_value_posting_filtered(
                source->payload_scan);
        stats->age_adjacency_payload_vertex_set_block_compressed_filtered +=
            age_adjacency_visible_payload_scan_vertex_set_block_compressed_filtered(
                source->payload_scan);
        stats->age_adjacency_payload_vertex_set_block_posting_filtered +=
            age_adjacency_visible_payload_scan_vertex_set_block_posting_filtered(
                source->payload_scan);
        stats->age_adjacency_payload_vertex_set_directory_filtered +=
            age_adjacency_visible_payload_scan_vertex_set_directory_filtered(
                source->payload_scan);
        stats->age_adjacency_payload_vertex_set_directory_range_filtered +=
            age_adjacency_visible_payload_scan_vertex_set_directory_range_filtered(
                source->payload_scan);
        stats->age_adjacency_payload_vertex_set_directory_exact_filtered +=
            age_adjacency_visible_payload_scan_vertex_set_directory_exact_filtered(
                source->payload_scan);
        stats->age_adjacency_payload_vertex_set_directory_label_bloom_filtered +=
            age_adjacency_visible_payload_scan_vertex_set_directory_label_bloom_filtered(
                source->payload_scan);
        stats->age_adjacency_payload_vertex_set_directory_compressed_filtered +=
            age_adjacency_visible_payload_scan_vertex_set_directory_compressed_filtered(
                source->payload_scan);
        stats->age_adjacency_payload_vertex_set_directory_wide_bloom_filtered +=
            age_adjacency_visible_payload_scan_vertex_set_directory_wide_bloom_filtered(
                source->payload_scan);
        stats->age_adjacency_payload_vertex_set_directory_value_filtered +=
            age_adjacency_visible_payload_scan_vertex_set_directory_value_filtered(
                source->payload_scan);
        stats->age_adjacency_payload_vertex_set_directory_value_posting_filtered +=
            age_adjacency_visible_payload_scan_vertex_set_directory_value_posting_filtered(
                source->payload_scan);
    }

    if (source->use_payload_cache &&
        !source->replay_payload_cache &&
        source->scanned <= 1 &&
        source->cache_entry != NULL &&
        source->cache_entry->payloads != NULL)
    {
        age_vle_adjacency_payload_cache_discard(source->cache_entry);
    }
    age_vle_context_apply_age_adjacency_frontier_empty(vlelctx, source);
    pfree_if_not_null(source->frontier_empty_keys);
    pfree(source);
}

VLEContextEndpointIndexSource *age_vle_context_begin_endpoint_index_source(
    const VLEContextSourceCursor *cursor)
{
    VLEContextEndpointIndexSource *source;
    ScanKeyData scan_key;

    Assert(cursor != NULL);

    if (!OidIsValid(cursor->index_oid) ||
        !OidIsValid(cursor->edge_label_oid) ||
        cursor->has_property_constraints)
    {
        return NULL;
    }

    source = palloc0(sizeof(*source));
    source->edge_rel = table_open(cursor->edge_label_oid, AccessShareLock);
    source->index_rel = index_open(cursor->index_oid, AccessShareLock);
    source->tupdesc = RelationGetDescr(source->edge_rel);
    source->slot = table_slot_create(source->edge_rel, NULL);

    ScanKeyInit(&scan_key, 1, BTEqualStrategyNumber, F_INT8EQ,
                GRAPHID_GET_DATUM(cursor->source_vertex_id));
    source->scan_desc = index_beginscan(source->edge_rel, source->index_rel,
                                        GetActiveSnapshot(), NULL, 1, 0);
    index_rescan(source->scan_desc, &scan_key, 1, NULL, 0);

    return source;
}

bool age_vle_context_endpoint_index_source_next(
    VLEContextEndpointIndexSource *source,
    VLEContextEndpointIndexTuple *tuple_data)
{
    Assert(source != NULL);
    Assert(tuple_data != NULL);

    if (index_getnext_slot(source->scan_desc, ForwardScanDirection,
                           source->slot))
    {
        HeapTuple tuple;
        bool should_free;
        bool isnull;

        tuple = ExecFetchSlotHeapTuple(source->slot, true, &should_free);
        tuple_data->edge_id = DatumGetInt64(heap_getattr(
            tuple, Anum_ag_label_edge_table_id, source->tupdesc, &isnull));
        if (isnull)
            elog(ERROR, "edge id is null during VLE endpoint index scan");
        tuple_data->start_vertex_id = DatumGetInt64(heap_getattr(
            tuple, Anum_ag_label_edge_table_start_id, source->tupdesc,
            &isnull));
        if (isnull)
            elog(ERROR, "edge start_id is null during VLE endpoint index scan");
        tuple_data->end_vertex_id = DatumGetInt64(heap_getattr(
            tuple, Anum_ag_label_edge_table_end_id, source->tupdesc,
            &isnull));
        if (isnull)
            elog(ERROR, "edge end_id is null during VLE endpoint index scan");

        if (should_free)
            heap_freetuple(tuple);
        ExecClearTuple(source->slot);
        return true;
    }

    return false;
}

void age_vle_context_end_endpoint_index_source(
    VLEContextEndpointIndexSource *source)
{
    if (source == NULL)
        return;

    if (source->scan_desc != NULL)
        index_endscan(source->scan_desc);
    if (source->slot != NULL)
        ExecDropSingleTupleTableSlot(source->slot);
    if (source->index_rel != NULL)
        index_close(source->index_rel, AccessShareLock);
    if (source->edge_rel != NULL)
        table_close(source->edge_rel, AccessShareLock);
    pfree(source);
}

void age_vle_context_init_source_layout_input(
    VLETraversalSourceLayoutInput *input, VLE_local_context *vlelctx)
{
    Assert(input != NULL);
    Assert(vlelctx != NULL);

    input->indexes = &vlelctx->root.source_indexes;
    input->upper = vlelctx->root.uidx;
    input->upper_infinite = vlelctx->root.uidx_infinite;
    input->use_local_edge_state = vlelctx->traversal.edge_state.use_local;
    input->label_constrained = vlelctx->edge_label_name_oid != InvalidOid;
    input->has_property_constraints =
        vlelctx->num_edge_property_constraints > 0;
    input->preferred_source_known = vlelctx->source_policy_known;
    input->preferred_outgoing_kind = vlelctx->source_policy_outgoing_kind;
    input->preferred_incoming_kind = vlelctx->source_policy_incoming_kind;
}

void age_vle_context_init_source_layout_input_from_apply(
    VLETraversalSourceLayoutInput *input,
    const VLETraversalContextApply *context_apply)
{
    Assert(input != NULL);
    Assert(context_apply != NULL);

    input->indexes = &context_apply->source_indexes;
    input->upper = context_apply->upper;
    input->upper_infinite = context_apply->upper_infinite;
    input->use_local_edge_state = context_apply->use_local_edge_state;
    input->label_constrained = context_apply->edge_label_oid != InvalidOid;
    input->has_property_constraints =
        context_apply->edge_property_constraint_count > 0;
    input->preferred_source_known = context_apply->source_policy_known;
    input->preferred_outgoing_kind =
        context_apply->source_policy_outgoing_kind;
    input->preferred_incoming_kind =
        context_apply->source_policy_incoming_kind;
}

void age_vle_context_init_root_selection_input(
    VLETraversalRootSelectionInput *input, VLE_local_context *vlelctx)
{
    Assert(input != NULL);
    Assert(vlelctx != NULL);

    input->ggctx = vlelctx->ggctx;
    input->edge_label_oid = vlelctx->edge_label_name_oid;
    input->empty_length_range = !vlelctx->root.uidx_infinite &&
                                vlelctx->root.lidx > vlelctx->root.uidx;
    input->zero_length_only = vlelctx->root.lidx == 0 &&
                              !vlelctx->root.uidx_infinite &&
                              vlelctx->root.uidx == 0;
    input->empty_lifecycle_batch_size =
        age_vle_context_empty_lifecycle_batch_size(vlelctx);
}

void age_vle_context_init_root_selection_input_from_apply(
    VLETraversalRootSelectionInput *input,
    const VLETraversalContextApply *context_apply)
{
    Assert(input != NULL);
    Assert(context_apply != NULL);

    input->ggctx = context_apply->ggctx;
    input->edge_label_oid = context_apply->edge_label_oid;
    input->empty_length_range = !context_apply->upper_infinite &&
                                context_apply->lower > context_apply->upper;
    input->zero_length_only = context_apply->lower == 0 &&
                              !context_apply->upper_infinite &&
                              context_apply->upper == 0;
    input->empty_lifecycle_batch_size =
        context_apply->empty_lifecycle_policy_known &&
        context_apply->empty_lifecycle_eligible ?
        context_apply->empty_lifecycle_batch_size : 0;
}

void age_vle_context_init_root_apply_input(
    VLETraversalRootApplyInput *input, VLE_local_context *vlelctx)
{
    Assert(input != NULL);
    Assert(vlelctx != NULL);

    age_vle_context_init_root_selection_input(&input->selection, vlelctx);
    age_vle_context_init_source_layout_input(&input->source_layout, vlelctx);
    age_vle_context_get_current_root(&input->current_root, vlelctx);
}

void age_vle_context_refresh_source_layout(VLE_local_context *vlelctx)
{
    VLETraversalSourceLayoutInput input;

    Assert(vlelctx != NULL);

    age_vle_context_init_source_layout_input(&input, vlelctx);
    init_vle_traversal_source_layout(
        &vlelctx->root.source_layout, &input, vlelctx->root.edge_direction);
}
