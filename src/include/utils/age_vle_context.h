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

#ifndef AGE_VLE_CONTEXT_H
#define AGE_VLE_CONTEXT_H

#include "postgres.h"

#include "access/age_adjacency.h"
#include "executor/cypher_adjacency_match_terminal.h"
#include "utils/age_global_graph.h"
#include "utils/age_vle.h"
#include "utils/age_vle_adjacency_cache.h"
#include "utils/age_vle_container.h"
#include "utils/age_vle_matrix_frontier_cache.h"
#include "utils/age_vle_root.h"
#include "utils/age_vle_terminal_property_batch.h"
#include "utils/age_vle_traversal.h"
#include "utils/hsearch.h"

typedef struct VLECleanupCallback VLECleanupCallback;

typedef struct VLEContextOutputState
{
    AgeVLEOutputRequirement requirement;
    bool emit_terminal_only;
    bool emit_terminal_property;
    bool carry_frame_vertex_entry;
    agtype_value terminal_property_key;
    bool terminal_property_key_is_char;
    char terminal_property_key_char;
    HTAB *terminal_property_prefetched_blocks;
    int64 terminal_property_prefetch_budget;
    Datum terminal_property_result;
    bool terminal_property_result_valid;
    bool terminal_property_result_is_null;
    VLETerminalPropertyBatchState terminal_property_batch;
} VLEContextOutputState;

typedef struct VLEContextTraversalRootState
{
    graphid vsid;                  /* starting vertex id */
    graphid veid;                  /* ending vertex id */
    int64 lidx;                    /* lower (start) bound index */
    int64 uidx;                    /* upper (end) bound index */
    bool uidx_infinite;            /* flag if the upper bound is omitted */
    cypher_rel_dir edge_direction; /* the direction of the edge */
    VLETraversalSourceIndexes source_indexes; /* adjacency/endpoint indexes */
    VLETraversalSourceLayout source_layout;
    AgeAdjacencyVisiblePayloadScan *age_adjacency_out_scan;
    AgeAdjacencyVisiblePayloadScan *age_adjacency_in_scan;
    HTAB *age_adjacency_payload_cache;
    HTAB *matrix_frontier_cache;
    VLE_path_function path_function; /* which path function to use */
    bool reverse_paths_to;         /* traverse paths-to from the bound end */
    bool reverse_output_path;      /* reverse traversal result before return */
    GraphIdNode *next_vertex;      /* for VLE_FUNCTION_PATHS_TO */
    VLETraversalEmptyCompletionSummary empty_completion;
    AgeAdjacencyMatchTerminalPropertyLookup *terminal_property_lookup;
    bool terminal_property_prefilter_eligible;
    Oid terminal_property_index_oid;
    bool terminal_property_predicate_known;
    agtype_value terminal_property_predicate_key;
    bool terminal_property_predicate_key_is_char;
    char terminal_property_predicate_key_char;
    Datum terminal_property_predicate_value;
    bool terminal_property_predicate_null;
    int64 terminal_property_prefetch_threshold;
} VLEContextTraversalRootState;

typedef struct VLEContextSourceCursor
{
    graphid source_vertex_id;
    VLETraversalSourceKind source_kind;
    Oid index_oid;
    Oid edge_label_oid;
    int32 edge_label_id;
    int32 terminal_label_id;
    int64 target_path_length;
    bool outgoing;
    bool skip_self_loops;
    bool has_property_constraints;
} VLEContextSourceCursor;

typedef struct VLEContextExpansionSourceRun
{
    graphid source_vertex_id;
    int64 source_path_length;
    bool used_out_source;
    bool used_in_source;
    bool missing_vertex_fallback;
    bool missing_vertex_eligible;
} VLEContextExpansionSourceRun;

typedef struct VLEContextAgeAdjacencyPayloadSource
    VLEContextAgeAdjacencyPayloadSource;

typedef struct VLEContextEndpointIndexSource VLEContextEndpointIndexSource;

typedef struct VLEContextEndpointIndexTuple
{
    graphid edge_id;
    graphid start_vertex_id;
    graphid end_vertex_id;
} VLEContextEndpointIndexTuple;

typedef struct VLEContextPackedAdjacencySource VLEContextPackedAdjacencySource;

typedef enum VLEContextSourceStatsKind
{
    VLE_CONTEXT_SOURCE_STATS_AGE_ADJACENCY,
    VLE_CONTEXT_SOURCE_STATS_ENDPOINT_BTREE,
    VLE_CONTEXT_SOURCE_STATS_PACKED_ADJACENCY
} VLEContextSourceStatsKind;

typedef struct VLEPropertyKeyDescriptor
{
    agtype_value *key;
    bool is_char;
    char key_char;
} VLEPropertyKeyDescriptor;

typedef struct VLETerminalPropertyLookup
{
    GRAPH_global_context *ggctx;
    VLEPropertyKeyDescriptor key_desc;
    HTAB **relation_cache;
    const char *relation_cache_name;
    HTAB **prefetched_blocks;
    int64 *prefetch_budget;
    bool allow_block_prefetch;
} VLETerminalPropertyLookup;

typedef struct VLETerminalOutputPolicy
{
    bool emit_property;
    bool direct_property;
    bool char_fast_path;
    VLETerminalPropertyLookup lookup;
} VLETerminalOutputPolicy;

typedef struct VLEEdgePropertyMatchContext
{
    agtype *constraint;
    Datum constraint_datum;
    uint32 constraint_hash;
    agtype_pair *cached_constraints;
    int constraint_count;
    int cached_constraint_count;
    HTAB *relation_cache;
} VLEEdgePropertyMatchContext;

typedef struct VLECandidateMatchResult
{
    const VLEEdgePropertyMatchContext *match_context;
    edge_entry *edge_for_match;
} VLECandidateMatchResult;

/* VLE local context per each unique age_vle iterator activation */
typedef struct VLE_local_context
{
    char *graph_name;              /* name of the graph */
    Oid graph_oid;                 /* graph oid for searching */
    GRAPH_global_context *ggctx;   /* global graph context pointer */
    char *edge_label_name;         /* edge label name for match */
    Oid edge_label_name_oid;       /* edge label name oid for match */
    int32 terminal_label_id;       /* terminal vertex label id constraint */
    int32 terminal_endpoint_label_id; /* final endpoint label constraint */
    agtype *edge_property_constraint; /* edge property constraint as agtype */
    int num_edge_property_constraints; /* number of edge property constraints */
    Datum edge_property_constraint_datum; /* edge property constraint as Datum */
    uint32 edge_property_constraint_hash; /* edge property constraint hash */
    agtype_pair *edge_property_constraint_pairs; /* cached constraints */
    int num_cached_edge_property_constraints; /* cached constraint count */
    HTAB *edge_property_relation_cache; /* relation cache for property fetch */
    bool source_policy_known;
    VLETraversalSourceKind source_policy_outgoing_kind;
    VLETraversalSourceKind source_policy_incoming_kind;
    bool empty_lifecycle_policy_known;
    bool empty_lifecycle_eligible;
    int64 empty_lifecycle_depth;
    int64 empty_lifecycle_batch_size;
    bool matrix_frontier_policy_known;
    bool matrix_frontier_eligible;
    int64 matrix_frontier_depth;
    int64 matrix_frontier_batch_size;
    VLEContextTraversalRootState root; /* root, bounds, source scan state */
    VLETraversalState traversal;   /* DFS frames, path stacks, edge state */
    graphid cached_vertex_id;      /* most recent vertex entry loaded by DFS */
    vertex_entry *cached_vertex_entry; /* cached entry for terminal output */
    VLEContextOutputState output;  /* output policy and terminal scratch */
    AgeVLESourceStats source_stats; /* runtime source selection counters */
    int64 vle_grammar_node_id;     /* the unique VLE grammar assigned node id */
    bool use_cache;                /* are we using VLE_local_context cache */
    struct VLE_local_context *next; /* next chained VLE_local_context */
    bool is_dirty;                 /* is this VLE context reusable */
    VLECleanupCallback *cleanup_callback;
} VLE_local_context;

typedef struct VLETraversalApplyInput
{
    AgeVLEInput *input;
    const VLETraversalSetup *setup;
    GRAPH_global_context *ggctx;
    bool use_cache;
    int64 vle_grammar_node_id;
} VLETraversalApplyInput;

typedef struct VLETraversalContextApply
{
    char *graph_name;
    Oid graph_oid;
    GRAPH_global_context *ggctx;
    bool use_cache;
    int64 vle_grammar_node_id;
    bool use_local_edge_state;
    agtype *edge_property_constraint;
    int edge_property_constraint_count;
    Oid edge_label_oid;
    int32 terminal_label_id;
    int32 terminal_endpoint_label_id;
    VLETraversalSourceIndexes source_indexes;
    bool source_policy_known;
    VLETraversalSourceKind source_policy_outgoing_kind;
    VLETraversalSourceKind source_policy_incoming_kind;
    bool empty_lifecycle_policy_known;
    bool empty_lifecycle_eligible;
    int64 empty_lifecycle_depth;
    int64 empty_lifecycle_batch_size;
    bool matrix_frontier_policy_known;
    bool matrix_frontier_eligible;
    int64 matrix_frontier_depth;
    int64 matrix_frontier_batch_size;
    int64 lower;
    int64 upper;
    bool upper_infinite;
    int64 terminal_property_prefetch_budget;
    bool terminal_property_prefilter_eligible;
    Oid terminal_property_index_oid;
    bool terminal_property_predicate_known;
    bool terminal_property_predicate_key_known;
    const char *terminal_property_predicate_key_value;
    int terminal_property_predicate_key_len;
    bool terminal_property_predicate_key_is_char;
    char terminal_property_predicate_key_char;
    Datum terminal_property_predicate_value;
    bool terminal_property_predicate_null;
    int64 terminal_property_source_prefetch_threshold;
} VLETraversalContextApply;

typedef struct VLETraversalOutputApply
{
    AgeVLEOutputRequirement output_requirement;
    bool emit_terminal_only;
    bool emit_terminal_property;
    bool carry_frame_vertex_entry;
    bool has_terminal_property_key;
    agtype_value terminal_property_key;
    bool terminal_property_key_is_char;
    char terminal_property_key_char;
    bool terminal_label_known;
    int32 terminal_label_id;
    AgeVLETerminalLabelMode terminal_label_mode;
} VLETraversalOutputApply;

typedef struct VLETraversalEdgeStateApply
{
    bool initialize;
    bool use_local;
    int64 capacity;
} VLETraversalEdgeStateApply;

typedef struct VLETraversalSetupApply
{
    VLETraversalContextApply context;
    VLETraversalRootDescriptor root;
    VLETraversalOutputApply output;
    VLETraversalEdgeStateApply edge_state;
} VLETraversalSetupApply;

typedef struct VLETraversalRefreshApply
{
    VLETraversalRootDescriptor root;
    int64 terminal_property_prefetch_budget;
    bool mark_dirty;
} VLETraversalRefreshApply;

typedef struct VLETraversalActivationApply
{
    bool refresh_source_indexes;
    bool init_traversal_state;
    bool load_initial_stacks;
    bool mark_dirty;
} VLETraversalActivationApply;

typedef struct VLETraversalCachedReuseApply
{
    VLETraversalRefreshApply refresh;
    VLETraversalActivationApply activation;
} VLETraversalCachedReuseApply;

typedef struct VLETraversalRootApplyInput
{
    VLETraversalRootSelectionInput selection;
    VLETraversalSourceLayoutInput source_layout;
    VLETraversalRootDescriptor current_root;
} VLETraversalRootApplyInput;

extern void age_vle_context_apply_base(
    VLE_local_context *vlelctx,
    const VLETraversalContextApply *context_apply);
extern void age_vle_context_apply_output(
    VLE_local_context *vlelctx,
    const VLETraversalOutputApply *output_apply);
extern void age_vle_context_apply_edge_state(
    VLE_local_context *vlelctx,
    const VLETraversalEdgeStateApply *edge_state_apply);
extern void age_vle_context_apply_root(
    VLE_local_context *vlelctx, const VLETraversalRootDescriptor *root);
extern void age_vle_context_get_current_root(
    VLETraversalRootDescriptor *root, VLE_local_context *vlelctx);
extern int64 age_vle_context_terminal_prefetch_budget(
    VLE_local_context *vlelctx);
extern void age_vle_context_reset_terminal_property_direct_result(
    VLE_local_context *vlelctx, int64 prefetch_budget);
extern void age_vle_context_release_runtime_resources(
    VLE_local_context *vlelctx);
extern void age_vle_context_release_all_resources(
    VLE_local_context *vlelctx);
extern void age_vle_context_reset_source_stats(VLE_local_context *vlelctx);
extern void age_vle_context_get_source_stats(
    VLE_local_context *vlelctx, AgeVLESourceStats *stats);
extern void age_vle_context_record_empty_lifecycle_policy(
    VLE_local_context *vlelctx);
extern void age_vle_context_record_matrix_frontier_policy(
    VLE_local_context *vlelctx);
extern void age_vle_context_record_matrix_frontier_source_run(
    VLE_local_context *vlelctx, int64 source_count);
extern void age_vle_context_record_matrix_frontier_source_run_evidence(
    VLE_local_context *vlelctx, int64 run_postings,
    int64 terminal_postings);
extern int64 age_vle_context_empty_lifecycle_batch_size(
    VLE_local_context *vlelctx);
extern void age_vle_context_record_source_scan(
    VLE_local_context *vlelctx, VLEContextSourceStatsKind kind);
extern void age_vle_context_record_source_candidate(
    VLE_local_context *vlelctx, VLEContextSourceStatsKind kind);
extern void age_vle_context_record_source_empty_scan(
    VLE_local_context *vlelctx, VLEContextSourceStatsKind kind);
extern void age_vle_context_record_age_adjacency_directory_filtered_empty_scan(
    VLE_local_context *vlelctx);
extern int64 age_vle_context_age_adjacency_directory_filtered(
    VLE_local_context *vlelctx);
extern void age_vle_context_record_age_adjacency_empty_source_skip(
    VLE_local_context *vlelctx, bool outgoing);
extern void age_vle_context_record_age_adjacency_empty_source_cache_hit(
    VLE_local_context *vlelctx, bool outgoing);
extern void age_vle_context_record_age_adjacency_empty_source_frontier_mark(
    VLE_local_context *vlelctx, bool outgoing);
extern void age_vle_context_record_age_adjacency_empty_source_frontier_batch(
    VLE_local_context *vlelctx, bool outgoing, int64 key_count);
extern void age_vle_context_record_age_adjacency_empty_source_run_skip(
    VLE_local_context *vlelctx, bool outgoing);
extern void age_vle_context_record_source_push(VLE_local_context *vlelctx);
extern void age_vle_context_record_missing_vertex_attempt(
    VLE_local_context *vlelctx);
extern void age_vle_context_record_missing_vertex_hit(
    VLE_local_context *vlelctx);
extern void age_vle_context_record_packed_suppression(
    VLE_local_context *vlelctx, bool suppress_out, bool suppress_in,
    bool suppress_self);
extern void age_vle_context_record_packed_empty_skip(
    VLE_local_context *vlelctx);
extern void age_vle_context_record_packed_policy_skip(
    VLE_local_context *vlelctx);
extern void age_vle_context_record_age_adjacency_payload_replay(
    VLE_local_context *vlelctx);
extern void age_vle_context_record_age_adjacency_payload_replay_run(
    VLE_local_context *vlelctx);
extern void age_vle_context_record_age_adjacency_payload_scan(
    VLE_local_context *vlelctx);
extern void age_vle_context_record_age_adjacency_payload_scan_run(
    VLE_local_context *vlelctx);
extern void age_vle_context_record_age_adjacency_payload_cache_seed(
    VLE_local_context *vlelctx);
extern void age_vle_context_record_age_adjacency_payload_cache_seed_run(
    VLE_local_context *vlelctx);
extern int64 age_vle_context_get_or_create_local_edge_index(
    VLE_local_context *vlelctx, graphid edge_id);
extern void age_vle_context_init_candidate_match_result(
    VLECandidateMatchResult *match_result,
    const VLEEdgePropertyMatchContext *match_context);
extern void age_vle_context_apply_candidate_match_result(
    VLE_local_context *vlelctx, const VLETraversalCandidate *candidate,
    const VLECandidateMatchResult *match_result, const char *caller);
extern bool age_vle_context_push_candidate_if_matched(
    VLE_local_context *vlelctx, const VLETraversalCandidate *candidate,
    const char *caller);
extern void age_vle_context_close_adjacency_scans(
    VLE_local_context *vlelctx);
extern void age_vle_context_free_adjacency_payload_cache(
    VLE_local_context *vlelctx);
extern void age_vle_context_refresh_source_indexes(
    VLE_local_context *vlelctx);
extern bool age_vle_context_should_load_initial_stacks(
    VLE_local_context *vlelctx);
extern void age_vle_context_init_traversal_state(
    VLE_local_context *vlelctx);
extern void age_vle_context_reset_traversal_for_start(
    VLE_local_context *vlelctx);
extern bool age_vle_context_consume_next_step(
    VLE_local_context *vlelctx, const char *caller, VLETraversalStep *step);
extern void age_vle_context_mark_dirty(VLE_local_context *vlelctx);
extern graphid age_vle_context_start_vertex_id(VLE_local_context *vlelctx);
extern graphid age_vle_context_end_vertex_id(VLE_local_context *vlelctx);
extern VLE_path_function age_vle_context_path_function(
    VLE_local_context *vlelctx);
extern bool age_vle_context_reverse_paths_to(VLE_local_context *vlelctx);
extern bool age_vle_context_reverse_output_path(VLE_local_context *vlelctx);
extern cypher_rel_dir age_vle_context_edge_direction(
    VLE_local_context *vlelctx);
extern bool age_vle_context_is_empty_length_range(
    VLE_local_context *vlelctx);
extern bool age_vle_context_is_zero_length_only(VLE_local_context *vlelctx);
extern bool age_vle_context_should_emit_zero_bound(
    VLE_local_context *vlelctx);
extern bool age_vle_context_reached_upper_bound(
    VLE_local_context *vlelctx, int64 path_length);
extern bool age_vle_context_should_prefetch_terminal_property_block(
    VLE_local_context *vlelctx);
extern bool age_vle_context_emits_terminal_property(
    VLE_local_context *vlelctx);
extern AgeVLEOutputRequirement age_vle_context_output_requirement(
    VLE_local_context *vlelctx);
extern void age_vle_context_set_emit_terminal_property(
    VLE_local_context *vlelctx, bool emit_terminal_property);
extern void age_vle_context_init_terminal_property_lookup(
    VLE_local_context *vlelctx, VLETerminalPropertyLookup *lookup);
extern void age_vle_context_init_terminal_output_policy(
    VLE_local_context *vlelctx, VLETerminalOutputPolicy *policy);
extern void age_vle_context_init_terminal_property_batch_fetch(
    VLE_local_context *vlelctx, const VLETerminalPropertyLookup *lookup,
    VLETerminalPropertyBatchFetch *fetch);
extern void age_vle_context_set_terminal_property_result(
    VLE_local_context *vlelctx, Datum property, bool is_null);
extern void age_vle_context_clear_terminal_property_result(
    VLE_local_context *vlelctx);
extern void age_vle_context_get_terminal_property_result(
    VLE_local_context *vlelctx, Datum *property, bool *is_null);
extern void age_vle_context_reset_terminal_property_batch(
    VLE_local_context *vlelctx);
extern bool age_vle_context_terminal_property_batch_materialized(
    VLE_local_context *vlelctx);
extern void age_vle_context_append_terminal_property_batch_id(
    VLE_local_context *vlelctx, graphid terminal_id);
extern void age_vle_context_fetch_terminal_property_batch(
    VLE_local_context *vlelctx, const VLETerminalPropertyBatchFetch *fetch);
extern bool age_vle_context_next_terminal_property_batch_result(
    VLE_local_context *vlelctx, Datum *property, bool *is_null);
extern void age_vle_context_init_acceptance(
    VLE_local_context *vlelctx, VLETraversalAcceptance *acceptance,
    bool require_terminal);
extern bool age_vle_context_has_zero_bound_start(
    VLE_local_context *vlelctx);
extern bool age_vle_context_can_advance_start_vertex(
    VLE_local_context *vlelctx);
extern bool age_vle_context_search_is_complete(
    VLE_local_context *vlelctx, bool found_path);
extern void age_vle_context_advance_start_vertex(
    VLE_local_context *vlelctx);
extern void age_vle_context_remember_vertex_entry(
    VLE_local_context *vlelctx, graphid vertex_id, vertex_entry *entry);
extern vertex_entry *age_vle_context_get_cached_vertex_entry(
    VLE_local_context *vlelctx, graphid vertex_id);
extern vertex_entry *age_vle_context_frame_vertex_entry(
    VLE_local_context *vlelctx, vertex_entry *entry);
extern bool age_vle_context_carries_frame_vertex_entry(
    VLE_local_context *vlelctx);
extern bool age_vle_context_uses_local_edge_state(VLE_local_context *vlelctx);
extern bool age_vle_context_has_edge_label(VLE_local_context *vlelctx);
extern Oid age_vle_context_edge_label_oid(VLE_local_context *vlelctx);
extern bool age_vle_context_has_edge_property_constraints(
    VLE_local_context *vlelctx);
extern bool age_vle_edge_property_match_context_has_constraints(
    const VLEEdgePropertyMatchContext *match_context);
extern bool age_vle_edge_property_matches(
    const VLEEdgePropertyMatchContext *match_context, edge_entry *ee);
extern HTAB *age_vle_ensure_edge_property_relation_cache(
    VLE_local_context *vlelctx, const char *cache_name);
extern void age_vle_context_init_edge_property_match(
    VLE_local_context *vlelctx, VLEEdgePropertyMatchContext *match_context);
extern graphid age_vle_context_current_terminal_vertex_id(
    VLE_local_context *vlelctx);
extern void age_vle_context_init_container_build_input(
    VLE_local_context *vlelctx, VLEContainerBuildInput *input);
extern bool age_vle_context_init_source_cursor(
    VLE_local_context *vlelctx, VLEContextSourceCursor *cursor,
    graphid source_vertex_id, bool outgoing, bool skip_self_loops);
extern void age_vle_context_init_expansion_source_run(
    VLEContextExpansionSourceRun *run, graphid source_vertex_id);
extern void age_vle_context_init_missing_vertex_source_run(
    VLE_local_context *vlelctx, VLEContextExpansionSourceRun *run,
    graphid source_vertex_id);
extern bool age_vle_context_expansion_source_run_is_eligible(
    const VLEContextExpansionSourceRun *run);
extern bool age_vle_context_init_expansion_source_cursor(
    VLE_local_context *vlelctx, VLEContextExpansionSourceRun *run,
    VLEContextSourceCursor *cursor, bool outgoing);
extern bool age_vle_context_expansion_source_cursor_known_empty(
    VLE_local_context *vlelctx, const VLEContextSourceCursor *cursor);
extern void age_vle_context_record_expansion_source_result(
    VLEContextExpansionSourceRun *run, bool outgoing, bool used_source);
extern bool age_vle_context_missing_vertex_sources_known_empty(
    VLE_local_context *vlelctx, graphid source_vertex_id);
extern VLEContextPackedAdjacencySource *
age_vle_context_begin_packed_adjacency_source_from_run(
    VLE_local_context *vlelctx, vertex_entry *entry,
    const VLEContextExpansionSourceRun *run);
extern bool age_vle_context_packed_adjacency_source_next(
    VLEContextPackedAdjacencySource *source, GraphEdgeAdjEntry **adj_entry);
extern void age_vle_context_end_packed_adjacency_source(
    VLEContextPackedAdjacencySource *source);
extern VLEContextAgeAdjacencyPayloadSource *
age_vle_context_begin_age_adjacency_payload_source(
    VLE_local_context *vlelctx, const VLEContextSourceCursor *cursor);
extern bool age_vle_context_init_matrix_frontier_block_key(
    VLE_local_context *vlelctx, const VLEContextSourceCursor *cursors,
    int64 cursor_count, VLEMatrixFrontierCacheKey *key);
extern bool age_vle_context_init_matrix_frontier_cursor_array_key(
    VLE_local_context *vlelctx,
    const VLEContextSourceCursor *const *cursors, int64 cursor_count,
    VLEMatrixFrontierCacheKey *key);
extern VLEContextAgeAdjacencyPayloadSource *
age_vle_context_begin_age_adjacency_payload_source_with_matrix_key(
    VLE_local_context *vlelctx, const VLEContextSourceCursor *cursor,
    const VLEMatrixFrontierCacheKey *matrix_key, bool record_matrix_block);
extern VLEContextAgeAdjacencyPayloadSource *
age_vle_context_begin_age_adjacency_payload_source_batch(
    VLE_local_context *vlelctx, const VLEContextSourceCursor *cursor,
    const VLEMatrixFrontierCacheKey *matrix_key, bool record_matrix_block);
extern bool age_vle_context_age_adjacency_payload_next(
    VLE_local_context *vlelctx, VLEContextAgeAdjacencyPayloadSource *source,
    AgeAdjacencyPayload *payload);
extern bool age_vle_context_age_adjacency_payload_source_uses_visible_scan(
    VLEContextAgeAdjacencyPayloadSource *source);
extern bool age_vle_context_age_adjacency_payload_source_replays_matrix(
    VLEContextAgeAdjacencyPayloadSource *source);
extern VLEMatrixFrontierCacheEntry *
age_vle_context_age_adjacency_payload_source_matrix_entry(
    VLEContextAgeAdjacencyPayloadSource *source);
extern void age_vle_context_age_adjacency_payload_source_accept_scanned_payload(
    VLE_local_context *vlelctx, VLEContextAgeAdjacencyPayloadSource *source,
    const AgeAdjacencyPayload *payload);
extern void age_vle_context_age_adjacency_payload_source_accept_matrix_replay(
    VLE_local_context *vlelctx, VLEContextAgeAdjacencyPayloadSource *source);
extern void age_vle_context_age_adjacency_payload_source_mark_empty(
    VLE_local_context *vlelctx, VLEContextAgeAdjacencyPayloadSource *source);
extern bool age_vle_context_prepare_age_adjacency_payload_source_run_filter(
    VLE_local_context *vlelctx, VLEContextAgeAdjacencyPayloadSource *source,
    int64 run_postings, int64 active_postings,
    AgeAdjacencyCompositeTerminalFilter *filter, bool *known_empty);
extern VLEMatrixFrontierCacheEntry *
age_vle_context_prepare_age_adjacency_matrix_seed_entry(
    VLE_local_context *vlelctx, VLEContextAgeAdjacencyPayloadSource *source);
extern void age_vle_context_bind_age_adjacency_matrix_seed_entry(
    VLEContextAgeAdjacencyPayloadSource *source,
    VLEMatrixFrontierCacheEntry *entry);
extern void age_vle_context_maybe_mark_age_adjacency_frontier_empty(
    VLE_local_context *vlelctx, VLEContextAgeAdjacencyPayloadSource *source,
    graphid next_source_vertex_id);
extern bool age_vle_context_age_adjacency_payload_source_empty_suppressed(
    VLEContextAgeAdjacencyPayloadSource *source);
extern void age_vle_context_end_age_adjacency_payload_source(
    VLE_local_context *vlelctx, VLEContextAgeAdjacencyPayloadSource *source);
extern VLEContextEndpointIndexSource *age_vle_context_begin_endpoint_index_source(
    const VLEContextSourceCursor *cursor);
extern bool age_vle_context_endpoint_index_source_next(
    VLEContextEndpointIndexSource *source,
    VLEContextEndpointIndexTuple *tuple);
extern void age_vle_context_end_endpoint_index_source(
    VLEContextEndpointIndexSource *source);
extern void age_vle_context_init_source_layout_input(
    VLETraversalSourceLayoutInput *input, VLE_local_context *vlelctx);
extern void age_vle_context_init_source_layout_input_from_apply(
    VLETraversalSourceLayoutInput *input,
    const VLETraversalContextApply *context_apply);
extern void age_vle_context_init_root_selection_input(
    VLETraversalRootSelectionInput *input, VLE_local_context *vlelctx);
extern void age_vle_context_init_root_selection_input_from_apply(
    VLETraversalRootSelectionInput *input,
    const VLETraversalContextApply *context_apply);
extern void age_vle_context_init_root_apply_input(
    VLETraversalRootApplyInput *input, VLE_local_context *vlelctx);
extern void age_vle_context_refresh_source_layout(
    VLE_local_context *vlelctx);

#endif
