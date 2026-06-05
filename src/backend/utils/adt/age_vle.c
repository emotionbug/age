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

/*
 * VLE (Variable-Length Edge) semantics and cost model
 * ---------------------------------------------------
 *
 * This file implements variable-length relationship matching for Cypher
 * patterns of the form (a)-[*min..max]->(b). The semantics and cost model
 * are often misunderstood; this note exists to prevent future
 * misdiagnoses (see issue #2349).
 *
 * Semantics: edge-isomorphism (openCypher-mandated)
 *
 *   A path is valid iff no edge appears in it more than once. Vertices MAY
 *   recur. This is "edge-isomorphism" (a.k.a. relationship-uniqueness) per
 *   the openCypher specification; it is NOT vertex-isomorphism.
 *
 *   Example: in the triangle A-[e1]->B-[e2]->C-[e3]->A, the query
 *     MATCH (a)-[*3]->(b) WHERE id(a) = id(A)
 *   MUST return the path (A, e1, B, e2, C, e3, A) with b = A. Switching
 *   to vertex-isomorphism would silently drop this path and violate the
 *   spec. Any "optimization" that tracks visited vertices as a filter
 *   rather than visited edges is therefore incorrect, not merely faster.
 *
 * Cost model
 *
 *   With E total edges in the traversal-reachable subgraph and a bounded
 *   pattern [*min..max], the number of enumerated paths is bounded by
 *   sum_{k=min..max} P(E, k) where P(E, k) = E! / (E - k)! -- polynomial
 *   in E for fixed max, but factorial in the depth bound.
 *
 *   Unbounded patterns ([*], [*1..]) have no termination guarantee other
 *   than edge-uniqueness depletion. On a cycle-rich graph the worst case
 *   is O(E!). This is inherent to edge-isomorphic path enumeration and
 *   cannot be reduced by algorithm change without changing semantics.
 *   Users who want reachability (not full enumeration) should bound the
 *   upper length or use a dedicated function such as shortestPath().
 *
 * Implementation pointer
 *
 *   Cycle prevention is enforced by dense edge-state flags, set and cleared
 *   during DFS traversal in dfs_find_a_path_between() and
 *   dfs_find_a_path_from(). Candidate providers check the same dense
 *   VLE_EDGE_STATE_USED flag before pushing new traversal frames.
 */

#include "postgres.h"

#include "access/age_adjacency.h"
#include "access/genam.h"
#include "access/heapam.h"
#include "access/relation.h"
#include "access/table.h"
#include "common/hashfn.h"
#include "commands/defrem.h"
#include "executor/tuptable.h"
#include "funcapi.h"
#include "utils/rel.h"
#include "utils/relcache.h"
#include "utils/snapmgr.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"

#include "utils/age_vle.h"
#include "utils/age_vle_adjacency_cache.h"
#include "utils/age_vle_apply.h"
#include "utils/age_vle_candidate_source.h"
#include "utils/age_vle_container.h"
#include "utils/age_vle_context.h"
#include "utils/age_vle_iterator_materialization.h"
#include "utils/age_vle_materializer_cache.h"
#include "utils/age_vle_root.h"
#include "utils/age_vle_setup.h"
#include "utils/age_vle_terminal_output.h"
#include "utils/age_vle_traversal.h"
#include "catalog/ag_graph.h"
#include "catalog/ag_label.h"
#include "nodes/cypher_nodes.h"
#include "utils/ag_cache.h"
#include "utils/agtype_raw.h"

/* defines */
#define EXISTS_HTAB_NAME "known edges"
#define EXISTS_HTAB_NAME_MIN_SIZE 16
#define EDGE_UNIQUENESS_FAST_ARGS 16
#define EDGE_UNIQUENESS_NESTED_SCAN_LIMIT 64
#define MAXIMUM_NUMBER_OF_CACHED_LOCAL_CONTEXTS 5

typedef struct VLETerminalPropertyCache
{
    Oid relid;
    Relation rel;
    agtype *key_arg;
    agtype_value key;
    bool key_valid;
    MemoryContextCallback callback;
} VLETerminalPropertyCache;

struct VLECleanupCallback
{
    MemoryContextCallback callback;
    struct VLE_local_context *vlelctx;
};

typedef struct VLEIteratorOutputState
{
    FuncCallContext *funcctx;
    struct VLE_local_context *vlelctx;
    VLETerminalOutputPolicy *policy;
    VLEIteratorOutputTarget target;
} VLEIteratorOutputState;

typedef struct VLEIteratorOutputBuildState
{
    FuncCallContext *funcctx;
    struct VLE_local_context *vlelctx;
    VLETerminalOutputPolicy *policy;
} VLEIteratorOutputBuildState;

typedef struct VLEIteratorSearchState
{
    struct VLE_local_context *vlelctx;
    VLETerminalOutputPolicy *policy;
    bool found_a_path;
    bool is_zero_bound;
} VLEIteratorSearchState;

struct AgeVLEIterator
{
    FuncCallContext *funcctx;
    VLE_local_context *vlelctx;
    AgeVLESourceStats source_stats;
    bool exhausted;
    bool zero_bound_pending;
};

typedef enum VLEIndexedPropertyEntity
{
    VLE_INDEXED_PROPERTY_VERTEX,
    VLE_INDEXED_PROPERTY_EDGE
} VLEIndexedPropertyEntity;

typedef struct VLEIndexedPropertyLookup
{
    GRAPH_global_context *ggctx;
    graphid entity_id;
    VLEPropertyKeyDescriptor key_desc;
    VLEIndexedPropertyEntity entity;
    bool candidate_vertex_matches;
} VLEIndexedPropertyLookup;

typedef struct VLESliceBoundaryMode
{
    bool return_id;
    bool return_label;
    bool return_labels;
    bool return_properties;
    bool return_endpoint;
    bool return_endpoint_id;
    bool return_endpoint_label;
    bool return_endpoint_labels;
    bool return_endpoint_properties;
    bool start_endpoint;
    bool double_tail_list;
    bool tail_reverse_list;
    bool reverse_list;
    bool slice_tail_list;
    bool slice_reverse_list;
} VLESliceBoundaryMode;

typedef struct edge_uniqueness_argtype_cache
{
    int nargs;
    Oid types[FLEXIBLE_ARRAY_MEMBER];
} edge_uniqueness_argtype_cache;

/* declarations */

/* global variable to hold the per process global cached VLE_local contexts */
static VLE_local_context *global_vle_local_contexts = NULL;

/* agtype functions */
static void get_vle_scalar_arg_no_copy(char *funcname, agtype *agt_arg,
                                       enum agtype_value_type type, bool error,
                                       agtype_value *result,
                                       bool *needs_free);
static VLETerminalPropertyCache *get_vle_terminal_property_cache(
    FunctionCallInfo fcinfo);
static Relation get_vle_terminal_property_relation(
    FunctionCallInfo fcinfo, Oid relid);
static void get_vle_terminal_property_key(FunctionCallInfo fcinfo,
                                          const char *funcname,
                                          agtype *agt_arg_key,
                                          agtype_value *key_value,
                                          bool *key_needs_free);
static bool validate_vle_property_key(agtype_value *key_value,
                                      bool key_needs_free);
static void destroy_vle_terminal_property_cache(void *arg);
static void cleanup_vle_local_context_resources(VLE_local_context *vlelctx);
static void cleanup_vle_local_context_callback(void *arg);
static void register_vle_local_context_cleanup(FuncCallContext *funcctx,
                                               VLE_local_context *vlelctx);
static void deactivate_vle_local_context_cleanup(VLE_local_context *vlelctx);
static graphid get_agtype_scalar_graphid_arg(agtype *agt_arg,
                                             const char *type_error_msg);
static bool get_agtype_scalar_bool_arg(agtype *agt_arg,
                                       const char *type_error_msg);
bool age_vle_edge_property_match_context_has_constraints(
    const VLEEdgePropertyMatchContext *match_context);
bool age_vle_edge_property_matches(
    const VLEEdgePropertyMatchContext *match_context, edge_entry *ee);
static bool cached_edge_property_constraints_match(
    const VLEEdgePropertyMatchContext *match_context,
    agtype_container *edge_properties);
static bool agtype_value_satisfies_property_constraint(
    agtype_value *property_value, agtype_value *constraint_value);
static void free_VLE_local_context(VLE_local_context *vlelctx);
/* VLE graph traversal functions */
/* graphid data structures */
static void load_initial_dfs_stacks(VLE_local_context *vlelctx);
static bool dfs_find_a_path_between(VLE_local_context *vlelctx);
static bool dfs_find_a_path_from(VLE_local_context *vlelctx);
static inline bool dfs_find_terminal_property_path_from(
    VLE_local_context *vlelctx)
    __attribute__((always_inline));
static void init_dfs_acceptance(VLE_local_context *vlelctx,
                                VLETraversalAcceptance *acceptance,
                                bool require_terminal);
static void extend_dfs_from_vertex_if_needed(VLE_local_context *vlelctx,
                                             const VLETraversalStep *step);
static void finish_vle_iterator(AgeVLEIterator *iterator, bool mark_clean);
static void sync_vle_iterator_source_stats(AgeVLEIterator *iterator);
static bool emit_materialized_terminal_property_batch(
    AgeVLEIterator *iterator, VLEIteratorOutputState *output,
    bool *handled);
static bool materialize_terminal_property_batch_path(void *state);
static bool emit_terminal_property_output_callback(
    void *state, const VLEIteratorMaterialization *materialization,
    const VLEIteratorOutputTarget *target);
static bool emit_terminal_full_properties_output_callback(
    void *state, const VLEIteratorOutputTarget *target);
static void *build_vle_iterator_container_callback(
    void *state, const VLEIteratorMaterialization *materialization);
static void init_vle_iterator_output_callbacks(
    VLEIteratorOutputCallbacks *callbacks,
    VLEIteratorOutputBuildState *state);
static bool search_vle_iterator_path(AgeVLEIterator *iterator,
                                     VLEIteratorSearchState *search);
static bool search_vle_iterator_path_function(
    VLEIteratorSearchState *search);
static void advance_vle_iterator_start(AgeVLEIterator *iterator,
                                       VLEIteratorSearchState *search,
                                       bool *done);
static int get_edge_uniqueness_args_fast(FunctionCallInfo fcinfo,
                                         Datum **args, Oid **types,
                                         bool **nulls, Datum *fast_args,
                                         Oid *fast_types, bool *fast_nulls);
static Oid get_cached_edge_uniqueness_argtype(FunctionCallInfo fcinfo,
                                              int argno, int nargs);
static void add_valid_vertex_edges(VLE_local_context *vlelctx,
                                   graphid vertex_id);
static void add_valid_vertex_edges_for_entry(VLE_local_context *vlelctx,
                                             vertex_entry *ve);
static inline void add_valid_vertex_edges_for_entry_impl(
    VLE_local_context *vlelctx, vertex_entry *ve)
    __attribute__((always_inline));
static void init_vle_indexed_property_lookup(
    VLEIndexedPropertyLookup *lookup, GRAPH_global_context *ggctx,
    const VLE_path_container *vpc, graphid entity_id, agtype_value *key,
    VLEIndexedPropertyEntity entity);
static bool get_vle_indexed_property_from_lookup(
    FunctionCallInfo fcinfo, const VLEIndexedPropertyLookup *lookup,
    Datum *property);
static agtype_value *build_vle_edge_value(GRAPH_global_context *ggctx,
                                          graphid edge_id,
                                          HTAB *relation_cache);
static agtype_value *build_vle_vertex_value(GRAPH_global_context *ggctx,
                                            graphid vertex_id,
                                            HTAB *relation_cache);
static agtype *build_empty_agtype_object(void);
static bool scan_vle_label_tuple_by_graphid(Relation rel, AttrNumber id_attno,
                                            graphid id,
                                            TupleTableSlot *slot);
static agtype *build_vle_typed_edge_agtype(
    const VLEMaterializerHandoff *handoff, graphid edge_id);
static agtype *build_vle_typed_vertex_agtype(
    const VLEMaterializerHandoff *handoff, graphid vertex_id);
static agtype_value *build_path(VLE_path_container *vpc);
static agtype *build_path_agtype(VLE_path_container *vpc,
                                 VLEMaterializerObjectCache *object_cache);
static agtype *build_edge_list_agtype(VLE_path_container *vpc,
                                      VLEMaterializerObjectCache *object_cache);
static agtype *build_node_list_agtype(VLE_path_container *vpc,
                                      VLEMaterializerObjectCache *object_cache);
static agtype_value *build_edge_list(VLE_path_container *vpc);
static agtype_value *build_empty_agtype_value_array(void);
static inline int64 get_vle_container_edge_count(
    const VLE_path_container *vpc);
static inline int64 get_vle_container_node_count(
    const VLE_path_container *vpc);
static inline bool normalize_vle_container_index(int64 count,
                                                 int64 *index);
static int64 agtv_vle_node_count(agtype *agt_arg_vpc);
static bool get_vle_edge_id_at_index(agtype *agt_arg_vpc,
                                     int64 edge_index, graphid *edge_id);
static agtype *materialize_vle_vertex_at_agtype(
    agtype *agt_arg_vpc, int64 node_index,
    VLEMaterializerObjectCache *object_cache);
static agtype *materialize_vle_edge_at_agtype(
    agtype *agt_arg_vpc, int64 edge_index,
    VLEMaterializerObjectCache *object_cache);
static VLEMaterializerHandoff make_vle_materializer_handoff(
    GRAPH_global_context *ggctx, HTAB *relation_cache,
    VLEMaterializerBuildObject build_object,
    VLEMaterializerOutputRequirement output_requirement,
    graphid traversal_root_id, bool traversal_root_valid,
    graphid candidate_vertex_id, bool candidate_vertex_valid);
static VLEMaterializerHandoff make_vle_materializer_handoff_from_container(
    GRAPH_global_context *ggctx, HTAB *relation_cache,
    VLEMaterializerBuildObject build_object,
    VLEMaterializerOutputRequirement output_requirement,
    const VLE_path_container *vpc);
static void prefetch_vle_materializer_vertices(
    const VLEMaterializerHandoff *handoff, const graphid *graphid_array,
    int64 graphid_array_size);
static agtype_value *agtv_materialize_vle_edge_endpoint_at(
    agtype *agt_arg_vpc, int64 edge_index, bool start_endpoint);
static Datum age_vle_edge_endpoint_id_at(FunctionCallInfo fcinfo,
                                         bool start_endpoint);
static agtype *build_empty_agtype_array(void);
static int64 decode_vle_slice_boundary_mode(int64 mode,
                                            VLESliceBoundaryMode *decoded);

static const VLETraversalApplyOps vle_traversal_apply_ops = {
    age_vle_context_refresh_source_indexes,
    load_initial_dfs_stacks
};

/* VLE_local_context cache management */
static VLE_local_context *get_cached_VLE_local_context(int64 vle_node_id);
static void cache_VLE_local_context(VLE_local_context *vlelctx);

static const VLETraversalContextCacheOps vle_traversal_context_cache_ops = {
    get_cached_VLE_local_context,
    cache_VLE_local_context
};

/* definitions */

/*
 * Helper function to retrieve a cached VLE local context. It will also purge
 * off any contexts beyond the maximum defined number of cached contexts. It
 * will promote (a very basic LRU) the recently fetched context to the head of
 * the list. If a context doesn't exist or is dirty, it will purge it off and
 * return NULL.
 */
static VLE_local_context *get_cached_VLE_local_context(int64 vle_grammar_node_id)
{
    VLE_local_context *vlelctx = global_vle_local_contexts;
    VLE_local_context *prev = NULL;
    VLE_local_context *next = NULL;
    int cache_size = 0;

    /* while we have contexts to check */
    while (vlelctx != NULL)
    {
        /* purge any contexts past the maximum cache size */
        if (cache_size >= MAXIMUM_NUMBER_OF_CACHED_LOCAL_CONTEXTS)
        {
            /* set the next pointer to the context that follows */
            next = vlelctx->next;

            /*
             * Clear (unlink) the previous context's next pointer, if needed.
             * Also clear prev as we are at the end of available cached contexts
             * and just purging them off. Remember, this forms a loop that will
             * exit the while after purging.
             */
            if (prev != NULL)
            {
                prev->next = NULL;
                prev = NULL;
            }

            /* free the context */
            free_VLE_local_context(vlelctx);

            /* set to the next one */
            vlelctx = next;

            /* if there is another context beyond the max, we will re-enter */
            continue;
        }

        /* if this context belongs to this grammar node */
        if (vlelctx->vle_grammar_node_id == vle_grammar_node_id)
        {
            /* and isn't dirty */
            if (vlelctx->is_dirty == false)
            {
                GRAPH_global_context *ggctx = NULL;

                /*
                 * Verify that the exact GRAPH global context associated with
                 * this VLE context is still linked and valid. Multiple load
                 * scopes can exist for the same graph, so graph OID alone is
                 * not specific enough here.
                 */
                ggctx = is_GRAPH_global_context_current(vlelctx->ggctx) ?
                    vlelctx->ggctx : NULL;

                /*
                 * If ggctx == NULL, vlelctx is bad and vlelctx needs to be
                 * removed.
                 * If ggctx == vlelctx->ggctx, then vlelctx is good.
                 * If ggctx != vlelctx->ggctx, then vlelctx needs to be updated.
                 * In the end, vlelctx->ggctx will be set to ggctx.
                 */

                /*
                 * If the returned ggctx isn't valid (there was some update to
                 * the underlying graph), then set it to NULL. This will force a
                 * rebuild of it.
                 */
                if (ggctx != NULL && is_ggctx_invalid(ggctx))
                {
                    ggctx = NULL;
                }

                vlelctx->ggctx = ggctx;

                /*
                 * If the context is good and isn't at the head of the cache,
                 * promote it to the head.
                 */
                if (ggctx != NULL && vlelctx != global_vle_local_contexts)
                {
                    /* adjust the links to cut out the node */
                    prev->next = vlelctx->next;
                    /* point the context to the old head of the list */
                    vlelctx->next = global_vle_local_contexts;
                    /* point the head to this context */
                    global_vle_local_contexts = vlelctx;
                }

                /* if we have a good one, return it. */
                if (ggctx != NULL)
                {
                    return vlelctx;
                }
            }

            /* otherwise, clean and remove it, and return NULL */

            /* set the top if necessary and unlink it */
            if (prev == NULL)
            {
                global_vle_local_contexts = vlelctx->next;
            }
            else
            {
                prev->next = vlelctx->next;
            }

            /* now free it and return NULL */
            free_VLE_local_context(vlelctx);
            return NULL;
        }
        /* save the previous context */
        prev = vlelctx;
        /* get the next context */
        vlelctx = vlelctx->next;
        /* keep track of cache size */
        cache_size++;
    }
    return vlelctx;
}

static void cache_VLE_local_context(VLE_local_context *vlelctx)
{
    /* if the context passed is null, just return */
    if (vlelctx == NULL)
    {
        return;
    }

    /* if the global link is null, just assign it the local context */
    if (global_vle_local_contexts == NULL)
    {
        global_vle_local_contexts = vlelctx;
        return;
    }

    /* if there is a global link, add the local context to the top */
    vlelctx->next = global_vle_local_contexts;
    global_vle_local_contexts = vlelctx;
}

bool age_vle_edge_property_match_context_has_constraints(
    const VLEEdgePropertyMatchContext *match_context)
{
    Assert(match_context != NULL);

    return match_context->constraint_count > 0;
}

/*
 * Compare the edge constraint properties against an edge entry's property.
 */
bool age_vle_edge_property_matches(
    const VLEEdgePropertyMatchContext *match_context, edge_entry *ee)
{
    agtype *edge_property = NULL;
    agtype_container *agtc_edge_property = NULL;
    agtype_container *agtc_edge_property_constraint = NULL;
    agtype_iterator *constraint_it = NULL;
    agtype_iterator *property_it = NULL;
    int num_edge_property_constraints = 0;
    int num_edge_properties = 0;

    Assert(match_context != NULL);

    /* get the number of conditions from the prototype edge */
    num_edge_property_constraints = match_context->constraint_count;

    /*
     * add_valid_vertex_edges() handles direction selection, label selection,
     * and cycle pruning before consulting edge-state match status. This helper
     * only verifies property constraints against an already label-compatible
     * candidate.
     */
    if (num_edge_property_constraints == 0)
    {
        return true;
    }

    agtc_edge_property_constraint = &match_context->constraint->root;

    if (num_edge_property_constraints > get_edge_entry_property_count(ee))
    {
        return false;
    }

    if (num_edge_property_constraints == get_edge_entry_property_count(ee))
    {
        if (get_edge_entry_property_size(ee) !=
            VARSIZE_ANY(match_context->constraint))
        {
            return false;
        }

        if (get_edge_entry_property_hash(ee) !=
            match_context->constraint_hash)
        {
            return false;
        }
    }

    /*
     * Fetch edge properties once and cache locally. With thin entries,
     * get_edge_entry_properties() does a heap_fetch, so we avoid calling
     * it multiple times for the same edge.
     */
    {
        Datum edge_props_datum = get_edge_entry_properties_with_cache(
            ee, match_context->relation_cache);

        edge_property = DATUM_GET_AGTYPE_P(edge_props_datum);
        agtc_edge_property = &edge_property->root;
        num_edge_properties = AGTYPE_CONTAINER_SIZE(agtc_edge_property);

        /*
         * Check to see if the edge_properties object has AT LEAST as many
         * pairs to compare as the edge_property_constraint object has pairs.
         * If not, it can't possibly match.
         */
        if (num_edge_property_constraints > num_edge_properties)
        {
            return false;
        }

        /*
         * If the number of constraints are the same as the number of
         * properties, then the datums would be the same if they match.
         */
        if (num_edge_property_constraints == num_edge_properties)
        {
            uint32 edge_props_hash = 0;

            if (VARSIZE_ANY(edge_property) !=
                VARSIZE_ANY(match_context->constraint))
            {
                return false;
            }

            edge_props_hash = datum_image_hash(edge_props_datum, false, -1);

            /* check the hash first */
            if (match_context->constraint_hash == edge_props_hash)
            {
                /* if the hashes match, check the datum images */
                if (datum_image_eq(match_context->constraint_datum,
                                   edge_props_datum, false, -1))
                {
                    return true;
                }
            }

            /* if we got here they aren't the same */
            return false;
        }

        if (match_context->cached_constraint_count > 0)
        {
            return cached_edge_property_constraints_match(
                match_context, agtc_edge_property);
        }

        /* get the iterators */
        constraint_it = agtype_iterator_init(agtc_edge_property_constraint);
        property_it = agtype_iterator_init(agtc_edge_property);

        /* return the value of deep contains */
        return agtype_deep_contains(&property_it, &constraint_it, false);
    }
}

static bool cached_edge_property_constraints_match(
    const VLEEdgePropertyMatchContext *match_context,
    agtype_container *edge_properties)
{
    int index;

    Assert(match_context != NULL);

    for (index = 0; index < match_context->cached_constraint_count;
         index++)
    {
        agtype_pair *constraint =
            &match_context->cached_constraints[index];
        agtype_value property_value;
        bool property_value_needs_free = false;
        bool found;
        bool matches;

        found = find_agtype_value_from_container_no_copy(
            edge_properties, AGT_FOBJECT, &constraint->key, &property_value,
            &property_value_needs_free);
        if (!found)
        {
            return false;
        }

        matches = agtype_value_satisfies_property_constraint(
            &property_value, &constraint->value);

        if (property_value_needs_free)
        {
            pfree_agtype_value_content(&property_value);
        }

        if (!matches)
        {
            return false;
        }
    }

    return true;
}

static bool agtype_value_satisfies_property_constraint(
    agtype_value *property_value, agtype_value *constraint_value)
{
    if (property_value->type != constraint_value->type)
    {
        return false;
    }

    if (IS_A_AGTYPE_SCALAR(property_value))
    {
        return compare_agtype_scalar_values(property_value,
                                            constraint_value) == 0;
    }

    Assert(property_value->type == AGTV_BINARY);
    Assert(constraint_value->type == AGTV_BINARY);

    {
        agtype_iterator *property_it = NULL;
        agtype_iterator *constraint_it = NULL;

        property_it = agtype_iterator_init(property_value->val.binary.data);
        constraint_it = agtype_iterator_init(
            constraint_value->val.binary.data);

        return agtype_deep_contains(&property_it, &constraint_it, false);
    }
}

static void get_vle_scalar_arg_no_copy(char *funcname, agtype *agt_arg,
                                       enum agtype_value_type type, bool error,
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

static graphid get_agtype_scalar_graphid_arg(agtype *agt_arg,
                                             const char *type_error_msg)
{
    agtype_value agtv_value;
    bool value_needs_free = false;
    graphid result;
    bool found;

    if (!AGT_ROOT_IS_SCALAR(agt_arg))
    {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("%s", type_error_msg)));
    }

    found = get_ith_agtype_value_from_container_no_copy(&agt_arg->root, 0,
                                                        &agtv_value,
                                                        &value_needs_free);
    Assert(found);

    if (agtv_value.type != AGTV_INTEGER)
    {
        if (value_needs_free)
        {
            pfree_agtype_value_content(&agtv_value);
        }
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("%s", type_error_msg)));
    }

    result = agtv_value.val.int_value;

    if (value_needs_free)
    {
        pfree_agtype_value_content(&agtv_value);
    }

    return result;
}

static bool get_agtype_scalar_bool_arg(agtype *agt_arg,
                                       const char *type_error_msg)
{
    agtype_value agtv_value;
    bool value_needs_free = false;
    bool result;
    bool found;

    if (!AGT_ROOT_IS_SCALAR(agt_arg))
    {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("%s", type_error_msg)));
    }

    found = get_ith_agtype_value_from_container_no_copy(&agt_arg->root, 0,
                                                        &agtv_value,
                                                        &value_needs_free);
    Assert(found);

    if (agtv_value.type != AGTV_BOOL)
    {
        if (value_needs_free)
        {
            pfree_agtype_value_content(&agtv_value);
        }
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("%s", type_error_msg)));
    }

    result = agtv_value.val.boolean;

    if (value_needs_free)
    {
        pfree_agtype_value_content(&agtv_value);
    }

    return result;
}

static void free_VLE_local_context(VLE_local_context *vlelctx)
{
    if (vlelctx == NULL)
        return;

    deactivate_vle_local_context_cleanup(vlelctx);
    age_vle_context_release_all_resources(vlelctx);
    pfree_if_not_null(vlelctx);
}

static void cleanup_vle_local_context_resources(VLE_local_context *vlelctx)
{
    if (vlelctx == NULL)
        return;

    age_vle_context_release_runtime_resources(vlelctx);
}

static void cleanup_vle_local_context_callback(void *arg)
{
    VLECleanupCallback *callback = arg;

    if (callback == NULL ||
        callback->vlelctx == NULL)
    {
        return;
    }

    cleanup_vle_local_context_resources(callback->vlelctx);
    callback->vlelctx->cleanup_callback = NULL;
    callback->vlelctx = NULL;
}

static void register_vle_local_context_cleanup(FuncCallContext *funcctx,
                                               VLE_local_context *vlelctx)
{
    MemoryContext oldctx;
    VLECleanupCallback *callback;

    if (funcctx == NULL ||
        vlelctx == NULL)
    {
        return;
    }

    deactivate_vle_local_context_cleanup(vlelctx);

    oldctx = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);
    callback = palloc0(sizeof(*callback));
    callback->callback.func = cleanup_vle_local_context_callback;
    callback->callback.arg = callback;
    callback->vlelctx = vlelctx;
    MemoryContextRegisterResetCallback(funcctx->multi_call_memory_ctx,
                                       &callback->callback);
    MemoryContextSwitchTo(oldctx);

    vlelctx->cleanup_callback = callback;
}

static void deactivate_vle_local_context_cleanup(VLE_local_context *vlelctx)
{
    if (vlelctx == NULL ||
        vlelctx->cleanup_callback == NULL)
    {
        return;
    }

    vlelctx->cleanup_callback->vlelctx = NULL;
    vlelctx->cleanup_callback = NULL;
}

/* load the initial edges into the DFS frame stack */
static void load_initial_dfs_stacks(VLE_local_context *vlelctx)
{
    vertex_entry *start_vertex = NULL;
    graphid start_vertex_id;
    graphid end_vertex_id;

    age_vle_context_reset_traversal_for_start(vlelctx);
    start_vertex_id = age_vle_context_start_vertex_id(vlelctx);
    end_vertex_id = age_vle_context_end_vertex_id(vlelctx);

    /*
     * If either the vsid or veid don't exist - don't load anything because
     * there won't be anything to find.
     */
    if (age_vle_context_is_empty_length_range(vlelctx) ||
        age_vle_context_is_zero_length_only(vlelctx))
    {
        return;
    }

    start_vertex = get_vertex_entry(vlelctx->ggctx, start_vertex_id);
    if (start_vertex == NULL)
    {
        if (age_vle_context_uses_local_edge_state(vlelctx) &&
            age_vle_context_has_edge_label(vlelctx) &&
            !age_vle_context_has_edge_property_constraints(vlelctx))
        {
            add_valid_vertex_edges(vlelctx, start_vertex_id);
        }
        return;
    }

    if ((age_vle_context_path_function(vlelctx) == VLE_FUNCTION_PATHS_BETWEEN ||
         (age_vle_context_path_function(vlelctx) == VLE_FUNCTION_PATHS_TO &&
          !age_vle_context_reverse_paths_to(vlelctx))) &&
        end_vertex_id != start_vertex_id &&
        get_vertex_entry(vlelctx->ggctx, end_vertex_id) == NULL)
    {
        return;
    }

    /* add in the edges for the start vertex */
    add_valid_vertex_edges_for_entry(vlelctx, start_vertex);
}

static void extend_dfs_from_vertex_if_needed(VLE_local_context *vlelctx,
                                             const VLETraversalStep *step)
{
    Assert(step != NULL);

    if (age_vle_context_reached_upper_bound(vlelctx, step->path_length))
    {
        return;
    }

    if (step->vertex_entry != NULL)
    {
        add_valid_vertex_edges_for_entry(vlelctx, step->vertex_entry);
    }
    else
    {
        add_valid_vertex_edges(vlelctx, step->vertex_id);
    }
}

static void init_dfs_acceptance(VLE_local_context *vlelctx,
                                VLETraversalAcceptance *acceptance,
                                bool require_terminal)
{
    Assert(vlelctx != NULL);
    Assert(acceptance != NULL);

    age_vle_context_init_acceptance(vlelctx, acceptance, require_terminal);
}

/*
 * Helper function to find one path BETWEEN two vertices.
 *
 * Note: On the very first entry into this function, the starting vertex's edges
 * should have already been loaded into the edge stack (this should have been
 * done by the SRF initialization phase).
 *
 * This function will always return on either a valid path found (true) or none
 * found (false). If one is found, the position (vertex & edge) will still be in
 * the stack. Each successive invocation within the SRF will then look for the
 * next available path until there aren't any left.
 */
static bool dfs_find_a_path_between(VLE_local_context *vlelctx)
{
    VLETraversalAcceptance acceptance;
    VLETerminalOutputPolicy output_policy;

    Assert(vlelctx != NULL);

    init_dfs_acceptance(vlelctx, &acceptance, true);
    age_vle_context_init_terminal_output_policy(vlelctx, &output_policy);

    while (true)
    {
        VLETraversalStep step;
        bool found;

        if (!age_vle_context_consume_next_step(
                vlelctx, "dfs_find_a_path_between", &step))
        {
            return false;
        }

        /*
         * Is this the end of a path that meets our requirements? Is its length
         * within the bounds specified?
         */
        found = age_vle_accepts_step(&acceptance, &step);

        /*
         * If we have found the end vertex but, we are not within our upper
         * bounds, we need to back up. We still need to continue traversing
         * the graph if we aren't within our lower bounds, though.
         */
        if (age_vle_step_over_upper_bound(&acceptance, &step))
        {
            continue;
        }

        extend_dfs_from_vertex_if_needed(vlelctx, &step);

        if (found &&
            age_vle_terminal_output_path_matches_predicate(vlelctx, &step))
        {
            age_vle_terminal_output_cache_result(vlelctx, &output_policy,
                                                 &step);
            return true;
        }
    }

    return false;
}

/*
 * Helper function to find one path FROM a start vertex.
 *
 * Note: On the very first entry into this function, the starting vertex's edges
 * should have already been loaded into the edge stack (this should have been
 * done by the SRF initialization phase).
 *
 * This function will always return on either a valid path found (true) or none
 * found (false). If one is found, the position (vertex & edge) will still be in
 * the stack. Each successive invocation within the SRF will then look for the
 * next available path until there aren't any left.
 */
static bool dfs_find_a_path_from(VLE_local_context *vlelctx)
{
    VLETraversalAcceptance acceptance;
    VLETerminalOutputPolicy output_policy;

    Assert(vlelctx != NULL);

    init_dfs_acceptance(vlelctx, &acceptance, false);
    age_vle_context_init_terminal_output_policy(vlelctx, &output_policy);

    while (true)
    {
        VLETraversalStep step;
        bool found;

        if (!age_vle_context_consume_next_step(
                vlelctx, "dfs_find_a_path_from", &step))
        {
            return false;
        }

        /*
         * Is this a path that meets our requirements? Is its length within the
         * bounds specified?
         */
        found = age_vle_accepts_step(&acceptance, &step);

        extend_dfs_from_vertex_if_needed(vlelctx, &step);

        if (found &&
            age_vle_terminal_output_path_matches_predicate(vlelctx, &step))
        {
            age_vle_terminal_output_cache_result(vlelctx, &output_policy,
                                                 &step);
            return true;
        }
    }

    return false;
}

static inline bool dfs_find_terminal_property_path_from(
    VLE_local_context *vlelctx)
{
    VLETraversalAcceptance acceptance;
    VLETerminalOutputPolicy output_policy;

    Assert(vlelctx != NULL);
    Assert(age_vle_context_emits_terminal_property(vlelctx));
    Assert(!age_vle_context_reverse_output_path(vlelctx));

    init_dfs_acceptance(vlelctx, &acceptance, false);
    age_vle_context_init_terminal_output_policy(vlelctx, &output_policy);
    Assert(age_vle_terminal_output_uses_direct_dfs(&output_policy));

    while (true)
    {
        VLETraversalStep step;

        if (!age_vle_context_consume_next_step(
                vlelctx, "dfs_find_terminal_property_path_from",
                &step))
        {
            return false;
        }

        extend_dfs_from_vertex_if_needed(vlelctx, &step);

        if (age_vle_accepts_step(&acceptance, &step) &&
            age_vle_terminal_output_path_matches_predicate(vlelctx, &step))
        {
            age_vle_terminal_output_cache_result(vlelctx, &output_policy,
                                                 &step);
            return true;
        }
    }

    return false;
}

/*
 * Helper function to add in valid vertex edges as part of the dfs path
 * algorithm. What constitutes a valid edge is the following -
 *
 *     1) Edge matches the correct direction specified.
 *     2) Edge is not currently in the path.
 *     3) Edge matches minimum edge properties specified.
 *
 * Note: The vertex must exist.
 */
static void add_valid_vertex_edges(VLE_local_context *vlelctx,
                                   graphid vertex_id)
{
    vertex_entry *ve = NULL;

    /* get the vertex entry */
    ve = get_vertex_entry(vlelctx->ggctx, vertex_id);
    /* there better be a valid vertex */
    if (ve == NULL)
    {
        if (age_vle_context_missing_vertex_sources_known_empty(vlelctx,
                                                               vertex_id))
        {
            return;
        }

        if (age_vle_push_candidates_from_missing_vertex_source(
                vlelctx, vertex_id))
        {
            return;
        }

        elog(ERROR, "add_valid_vertex_edges: no vertex found");
    }

    age_vle_context_remember_vertex_entry(vlelctx, vertex_id, ve);

    add_valid_vertex_edges_for_entry(vlelctx, ve);
}

static void add_valid_vertex_edges_for_entry(VLE_local_context *vlelctx,
                                             vertex_entry *ve)
{
    add_valid_vertex_edges_for_entry_impl(vlelctx, ve);
}

static inline void add_valid_vertex_edges_for_entry_impl(
    VLE_local_context *vlelctx, vertex_entry *ve)
{
    age_vle_push_candidates_from_vertex_entry(vlelctx, ve);

    /* The relation cache is owned by the VLE context and freed with it. */
}

static void init_vle_indexed_property_lookup(
    VLEIndexedPropertyLookup *lookup, GRAPH_global_context *ggctx,
    const VLE_path_container *vpc, graphid entity_id, agtype_value *key,
    VLEIndexedPropertyEntity entity)
{
    Assert(lookup != NULL);
    Assert(ggctx != NULL);
    Assert(vpc != NULL);
    Assert(key != NULL);

    lookup->ggctx = ggctx;
    lookup->entity_id = entity_id;
    lookup->key_desc.key = key;
    lookup->key_desc.is_char = key->type == AGTV_STRING &&
                               key->val.string.len == 1;
    lookup->key_desc.key_char = lookup->key_desc.is_char ?
                                key->val.string.val[0] : '\0';
    lookup->entity = entity;
    lookup->candidate_vertex_matches =
        entity == VLE_INDEXED_PROPERTY_VERTEX &&
        vpc->candidate_vertex_valid &&
        vpc->candidate_vertex_id == entity_id;
}

static bool get_vle_indexed_property_from_lookup(
    FunctionCallInfo fcinfo, const VLEIndexedPropertyLookup *lookup,
    Datum *property)
{
    Relation rel;

    Assert(fcinfo != NULL);
    Assert(lookup != NULL);
    Assert(lookup->key_desc.key != NULL);
    Assert(property != NULL);

    if (lookup->entity == VLE_INDEXED_PROPERTY_VERTEX)
    {
        vertex_entry *ve;

        ve = ensure_vertex_entry_skeleton(lookup->ggctx, lookup->entity_id);
        Assert(ve != NULL);

        if (lookup->candidate_vertex_matches &&
            get_vertex_entry_cached_property(ve, lookup->key_desc.key,
                                             property))
        {
            return true;
        }

        rel = get_vle_terminal_property_relation(
            fcinfo, get_vertex_entry_label_table_oid(ve));
        return get_vertex_entry_property_with_relation(ve, rel,
                                                       lookup->key_desc.key,
                                                       property);
    }

    Assert(lookup->entity == VLE_INDEXED_PROPERTY_EDGE);
    {
        edge_entry *ee;

        ee = get_edge_entry(lookup->ggctx, lookup->entity_id);
        Assert(ee != NULL);

        rel = get_vle_terminal_property_relation(
            fcinfo, get_edge_entry_label_table_oid(ee));
        return get_edge_entry_property_with_relation(ee, rel,
                                                     lookup->key_desc.key,
                                                     property);
    }
}

/*
 * Helper function to build an AGTV_ARRAY of edges from an array of graphids.
 *
 * Note: You should free the array when done. Although, it should be freed
 *       when the context is destroyed from the return of the SRF call.
 */
static agtype_value *build_edge_list(VLE_path_container *vpc)
{
    GRAPH_global_context *ggctx = NULL;
    agtype_in_state edges_result;
    HTAB *relation_cache = NULL;
    Oid graph_oid = InvalidOid;
    graphid *graphid_array = NULL;
    int64 graphid_array_size = 0;
    int index = 0;

    graphid_array_size = vpc->graphid_array_size;
    if (graphid_array_size <= 1)
    {
        return build_empty_agtype_value_array();
    }

    /* get the graph_oid */
    graph_oid = vpc->graph_oid;

    /* get the GRAPH global context for this graph */
    ggctx = find_GRAPH_global_context(graph_oid);
    /* verify we got a global context */
    Assert(ggctx != NULL);

    /* get the graphid_array and size */
    graphid_array = GET_GRAPHID_ARRAY_FROM_CONTAINER(vpc);

    /* initialize our agtype array */
    MemSet(&edges_result, 0, sizeof(agtype_in_state));
    edges_result.res = push_agtype_value(&edges_result.parse_state,
                                         WAGT_BEGIN_ARRAY, NULL);
    if (graphid_array_size > 3)
    {
        relation_cache = create_entry_property_relation_cache(
            "vle edge materialization relation cache");
    }

    for (index = 1; index < graphid_array_size - 1; index += 2)
    {
        agtype_value *agtv_edge = NULL;

        agtv_edge = build_vle_edge_value(ggctx, graphid_array[index],
                                         relation_cache);
        /* push the edge*/
        edges_result.res = push_agtype_value(&edges_result.parse_state,
                                             WAGT_ELEM, agtv_edge);
    }

    /* close our agtype array */
    edges_result.res = push_agtype_value(&edges_result.parse_state,
                                         WAGT_END_ARRAY, NULL);
    destroy_entry_property_relation_cache(relation_cache);

    /* make it an array */
    edges_result.res->type = AGTV_ARRAY;

    /* return it */
    return edges_result.res;
}

static agtype *build_empty_agtype_array(void)
{
    agtype_in_state result;
    agtype *agt_result;

    MemSet(&result, 0, sizeof(agtype_in_state));

    result.res = push_agtype_value(&result.parse_state, WAGT_BEGIN_ARRAY,
                                   NULL);
    result.res = push_agtype_value(&result.parse_state, WAGT_END_ARRAY, NULL);
    agt_result = agtype_value_to_agtype(result.res);

    pfree_agtype_in_state(&result);

    return agt_result;
}

static agtype_value *build_empty_agtype_value_array(void)
{
    agtype_in_state result;

    MemSet(&result, 0, sizeof(agtype_in_state));

    result.res = push_agtype_value(&result.parse_state, WAGT_BEGIN_ARRAY,
                                   NULL);
    result.res = push_agtype_value(&result.parse_state, WAGT_END_ARRAY, NULL);
    result.res->type = AGTV_ARRAY;

    return result.res;
}

static agtype_value *build_vle_edge_value(GRAPH_global_context *ggctx,
                                          graphid edge_id,
                                          HTAB *relation_cache)
{
    edge_entry *ee = NULL;

    ee = get_edge_entry(ggctx, edge_id);
    Assert(ee != NULL);

    return agtype_value_build_edge(get_edge_entry_id(ee),
                                   get_edge_entry_label_name(ee),
                                   get_edge_entry_end_vertex_id(ee),
                                   get_edge_entry_start_vertex_id(ee),
                                       get_edge_entry_properties_with_cache(
                                           ee, relation_cache));
}

static agtype_value *build_vle_vertex_value(GRAPH_global_context *ggctx,
                                            graphid vertex_id,
                                            HTAB *relation_cache)
{
    vertex_entry *ve = NULL;
    label_cache_data *label_cache = NULL;
    agtype_value *vertex;
    agtype *properties;
    agtype *empty_properties = NULL;
    char *label_name;
    bool free_properties = false;

    ve = get_vertex_entry(ggctx, vertex_id);
    if (ve != NULL)
    {
        label_name = get_vertex_entry_label_name(ve);
        properties = DATUM_GET_AGTYPE_P(
            get_vertex_entry_properties_with_cache(ve, relation_cache));
        if (properties == NULL)
        {
            empty_properties = build_empty_agtype_object();
            properties = empty_properties;
        }
    }
    else
    {
        Relation vertex_rel;
        TupleTableSlot *slot;
        bool found;
        bool isnull = false;
        Datum properties_datum;

        label_cache = search_label_graph_oid_cache_cached(
            get_GRAPH_global_context_oid(ggctx),
            get_graphid_label_id(vertex_id));
        if (label_cache == NULL || !OidIsValid(label_cache->relation))
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_EXCEPTION),
                     errmsg("missing VLE vertex label for graphid %ld",
                            (long)vertex_id)));

        vertex_rel = table_open(label_cache->relation, AccessShareLock);
        slot = table_slot_create(vertex_rel, NULL);
        found = scan_vle_label_tuple_by_graphid(
            vertex_rel, Anum_ag_label_vertex_table_id, vertex_id, slot);
        if (!found)
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_EXCEPTION),
                     errmsg("missing VLE vertex row for graphid %ld",
                            (long)vertex_id)));

        properties_datum = slot_getattr(
            slot, Anum_ag_label_vertex_table_properties, &isnull);
        if (isnull)
        {
            empty_properties = build_empty_agtype_object();
            properties = empty_properties;
        }
        else
        {
            properties = (agtype *)PG_DETOAST_DATUM_COPY(properties_datum);
            free_properties = true;
        }
        label_name = NameStr(label_cache->name);

        ExecDropSingleTupleTableSlot(slot);
        table_close(vertex_rel, AccessShareLock);
    }

    vertex = agtype_value_build_vertex(vertex_id, label_name,
                                       PointerGetDatum(properties));

    if (empty_properties != NULL)
        pfree(empty_properties);
    else if (free_properties)
        pfree(properties);

    return vertex;
}

static agtype *build_empty_agtype_object(void)
{
    agtype_build_state *bstate;
    agtype *object;

    bstate = init_agtype_build_state(0, AGT_FOBJECT);
    object = build_agtype(bstate);
    pfree_agtype_build_state(bstate);

    return object;
}

static bool scan_vle_label_tuple_by_graphid(Relation rel, AttrNumber id_attno,
                                            graphid id,
                                            TupleTableSlot *slot)
{
    ScanKeyData scan_keys[1];
    Oid index_oid;
    bool found = false;

    Assert(rel != NULL);
    Assert(slot != NULL);

    index_oid = find_usable_btree_index_for_attr(rel, id_attno);
    ScanKeyInit(&scan_keys[0], id_attno, BTEqualStrategyNumber, F_GRAPHIDEQ,
                GRAPHID_GET_DATUM(id));

    if (OidIsValid(index_oid))
    {
        Relation index_rel;
        IndexScanDesc scan_desc;

        index_rel = index_open(index_oid, AccessShareLock);
        scan_desc = index_beginscan(rel, index_rel, GetActiveSnapshot(),
                                    NULL, 1, 0);
        index_rescan(scan_desc, scan_keys, 1, NULL, 0);
        found = index_getnext_slot(scan_desc, ForwardScanDirection, slot);
        if (found)
            ExecMaterializeSlot(slot);
        index_endscan(scan_desc);
        index_close(index_rel, AccessShareLock);
    }
    else
    {
        TableScanDesc scan_desc;

        scan_desc = table_beginscan(rel, GetActiveSnapshot(), 1, scan_keys);
        found = table_scan_getnextslot(scan_desc, ForwardScanDirection, slot);
        if (found)
            ExecMaterializeSlot(slot);
        table_endscan(scan_desc);
    }

    return found;
}

static agtype *build_vle_vertex_object_agtype(
    const VLEMaterializerHandoff *handoff, graphid vertex_id)
{
    vertex_entry *ve = NULL;
    label_cache_data *label_cache = NULL;
    agtype_build_state *bstate;
    agtype *properties;
    agtype *empty_properties = NULL;
    agtype *vertex;
    char *label_name;
    bool free_properties = false;

    Assert(handoff != NULL);

    ve = get_vertex_entry(handoff->ggctx, vertex_id);
    if (ve != NULL)
    {
        label_name = get_vertex_entry_label_name(ve);
        properties = DATUM_GET_AGTYPE_P(
            get_vertex_entry_properties_with_cache(ve,
                                                   handoff->relation_cache));
        if (properties == NULL)
        {
            empty_properties = build_empty_agtype_object();
            properties = empty_properties;
        }
    }
    else
    {
        Relation vertex_rel;
        TupleTableSlot *slot;
        bool found;
        bool isnull = false;
        Datum properties_datum;

        label_cache = search_label_graph_oid_cache_cached(
            get_GRAPH_global_context_oid(handoff->ggctx),
            get_graphid_label_id(vertex_id));
        if (label_cache == NULL || !OidIsValid(label_cache->relation))
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_EXCEPTION),
                     errmsg("missing VLE vertex label for graphid %ld",
                            (long)vertex_id)));

        vertex_rel = table_open(label_cache->relation, AccessShareLock);
        slot = table_slot_create(vertex_rel, NULL);
        found = scan_vle_label_tuple_by_graphid(
            vertex_rel, Anum_ag_label_vertex_table_id, vertex_id, slot);

        if (!found)
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_EXCEPTION),
                     errmsg("missing VLE vertex row for graphid %ld",
                            (long)vertex_id)));

        properties_datum = slot_getattr(
            slot, Anum_ag_label_vertex_table_properties, &isnull);
        if (isnull)
        {
            empty_properties = build_empty_agtype_object();
            properties = empty_properties;
        }
        else
        {
            properties = (agtype *)PG_DETOAST_DATUM_COPY(properties_datum);
            free_properties = true;
        }
        label_name = NameStr(label_cache->name);

        ExecDropSingleTupleTableSlot(slot);
        table_close(vertex_rel, AccessShareLock);
    }

    bstate = init_agtype_build_state(3, AGT_FOBJECT);
    write_string(bstate, "id");
    write_string(bstate, "label");
    write_string(bstate, "properties");
    write_graphid(bstate, vertex_id);
    write_string(bstate, label_name);
    write_container(bstate, properties);
    vertex = build_agtype(bstate);
    pfree_agtype_build_state(bstate);

    if (empty_properties != NULL)
        pfree(empty_properties);
    else if (free_properties)
        pfree(properties);

    return vertex;
}

static agtype *build_vle_edge_object_agtype(
    const VLEMaterializerHandoff *handoff, graphid edge_id)
{
    edge_entry *ee = NULL;
    label_cache_data *label_cache = NULL;
    agtype_build_state *bstate;
    agtype *properties;
    agtype *empty_properties = NULL;
    agtype *edge;
    graphid start_id;
    graphid end_id;
    char *label_name;
    bool free_properties = false;

    Assert(handoff != NULL);

    ee = find_edge_entry(handoff->ggctx, edge_id);

    if (ee != NULL)
    {
        label_name = get_edge_entry_label_name(ee);
        start_id = get_edge_entry_start_vertex_id(ee);
        end_id = get_edge_entry_end_vertex_id(ee);

        if (graph_global_context_has_edge_metadata(handoff->ggctx) &&
            get_edge_entry_property_size(ee) > 0 &&
            get_edge_entry_property_count(ee) == 0)
        {
            empty_properties = build_empty_agtype_object();
            properties = empty_properties;
        }
        else
        {
            properties = DATUM_GET_AGTYPE_P(
                get_edge_entry_properties_with_cache(ee,
                                                    handoff->relation_cache));
            if (properties == NULL)
            {
                empty_properties = build_empty_agtype_object();
                properties = empty_properties;
            }
        }
    }
    else
    {
        Relation edge_rel;
        TupleTableSlot *slot;
        bool found;
        bool isnull = false;
        Datum properties_datum;

        label_cache = search_label_graph_oid_cache_cached(
            get_GRAPH_global_context_oid(handoff->ggctx),
            get_graphid_label_id(edge_id));
        if (label_cache == NULL || !OidIsValid(label_cache->relation))
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_EXCEPTION),
                     errmsg("missing VLE edge label for graphid %ld",
                            (long)edge_id)));

        edge_rel = table_open(label_cache->relation, AccessShareLock);
        slot = table_slot_create(edge_rel, NULL);
        found = scan_vle_label_tuple_by_graphid(
            edge_rel, Anum_ag_label_edge_table_id, edge_id, slot);

        if (!found)
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_EXCEPTION),
                     errmsg("missing VLE edge row for graphid %ld",
                            (long)edge_id)));

        start_id = DatumGetInt64(slot_getattr(
            slot, Anum_ag_label_edge_table_start_id, &isnull));
        if (isnull)
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_EXCEPTION),
                     errmsg("VLE edge start_id is null for graphid %ld",
                            (long)edge_id)));
        end_id = DatumGetInt64(slot_getattr(
            slot, Anum_ag_label_edge_table_end_id, &isnull));
        if (isnull)
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_EXCEPTION),
                     errmsg("VLE edge end_id is null for graphid %ld",
                            (long)edge_id)));

        properties_datum = slot_getattr(
            slot, Anum_ag_label_edge_table_properties, &isnull);
        if (isnull)
        {
            empty_properties = build_empty_agtype_object();
            properties = empty_properties;
        }
        else
        {
            properties = (agtype *)PG_DETOAST_DATUM_COPY(properties_datum);
            free_properties = true;
        }
        label_name = NameStr(label_cache->name);

        ExecDropSingleTupleTableSlot(slot);
        table_close(edge_rel, AccessShareLock);
    }

    bstate = init_agtype_build_state(5, AGT_FOBJECT);
    write_string(bstate, "id");
    write_string(bstate, "label");
    write_string(bstate, "end_id");
    write_string(bstate, "start_id");
    write_string(bstate, "properties");
    write_graphid(bstate, edge_id);
    write_string(bstate, label_name);
    write_graphid(bstate, end_id);
    write_graphid(bstate, start_id);
    write_container(bstate, properties);
    edge = build_agtype(bstate);
    pfree_agtype_build_state(bstate);

    if (empty_properties != NULL)
        pfree(empty_properties);
    else if (free_properties)
        pfree(properties);

    return edge;
}

static agtype *build_vle_typed_edge_agtype(
    const VLEMaterializerHandoff *handoff, graphid edge_id)
{
    Assert(handoff != NULL);

    return agtype_value_to_agtype(
        build_vle_edge_value(handoff->ggctx, edge_id,
                             handoff->relation_cache));
}

static agtype *build_vle_typed_vertex_agtype(
    const VLEMaterializerHandoff *handoff, graphid vertex_id)
{
    Assert(handoff != NULL);

    return agtype_value_to_agtype(
        build_vle_vertex_value(handoff->ggctx, vertex_id,
                               handoff->relation_cache));
}

/*
 * Helper function to build an array of type AGTV_PATH from an array of
 * graphids.
 *
 * Note: You should free the array when done. Although, it should be freed
 *       when the context is destroyed from the return of the SRF call.
 */
static agtype_value *build_path(VLE_path_container *vpc)
{
    GRAPH_global_context *ggctx = NULL;
    agtype_in_state path_result;
    HTAB *relation_cache = NULL;
    Oid graph_oid = InvalidOid;
    graphid *graphid_array = NULL;
    int64 graphid_array_size = 0;
    int index = 0;

    /* get the graph_oid */
    graph_oid = vpc->graph_oid;

    /* get the GRAPH global context for this graph */
    ggctx = find_GRAPH_global_context(graph_oid);
    /* verify we got a global context */
    Assert(ggctx != NULL);

    /* get the graphid_array and size */
    graphid_array = GET_GRAPHID_ARRAY_FROM_CONTAINER(vpc);
    graphid_array_size = vpc->graphid_array_size;

    /* initialize our agtype array */
    MemSet(&path_result, 0, sizeof(agtype_in_state));
    path_result.res = push_agtype_value(&path_result.parse_state,
                                        WAGT_BEGIN_ARRAY, NULL);
    if (graphid_array_size > 3)
    {
        relation_cache = create_entry_property_relation_cache(
            "vle path materialization relation cache");
    }

    for (index = 0; index < graphid_array_size; index += 2)
    {
        agtype_value *agtv_vertex = NULL;
        agtype_value *agtv_edge = NULL;

        /* reconstruct the vertex */
        agtv_vertex = build_vle_vertex_value(ggctx, graphid_array[index],
                                             relation_cache);
        /* push the vertex */
        path_result.res = push_agtype_value(&path_result.parse_state, WAGT_ELEM,
                                            agtv_vertex);

        /*
         * Remember that we have more vertices than edges. So, we need to check
         * if the above vertex was the last vertex in the path.
         */
        if (index + 1 >= graphid_array_size)
        {
            break;
        }

        /* reconstruct the edge */
        agtv_edge = build_vle_edge_value(ggctx, graphid_array[index + 1],
                                         relation_cache);
        /* push the edge*/
        path_result.res = push_agtype_value(&path_result.parse_state, WAGT_ELEM,
                                            agtv_edge);
    }

    /* close our agtype array */
    path_result.res = push_agtype_value(&path_result.parse_state,
                                        WAGT_END_ARRAY, NULL);
    if (relation_cache != NULL)
    {
        destroy_entry_property_relation_cache(relation_cache);
    }

    /* make it a path */
    path_result.res->type = AGTV_PATH;

    /* return the path */
    return path_result.res;
}

static VLEMaterializerHandoff make_vle_materializer_handoff(
    GRAPH_global_context *ggctx, HTAB *relation_cache,
    VLEMaterializerBuildObject build_object,
    VLEMaterializerOutputRequirement output_requirement,
    graphid traversal_root_id, bool traversal_root_valid,
    graphid candidate_vertex_id, bool candidate_vertex_valid)
{
    VLEMaterializerHandoff handoff;

    handoff.ggctx = ggctx;
    handoff.relation_cache = relation_cache;
    handoff.build_object = build_object;
    handoff.output_requirement = output_requirement;
    handoff.traversal_root_id = traversal_root_id;
    handoff.traversal_root_valid = traversal_root_valid;
    handoff.candidate_vertex_id = candidate_vertex_id;
    handoff.candidate_vertex_valid = candidate_vertex_valid;

    return handoff;
}

static VLEMaterializerHandoff make_vle_materializer_handoff_from_container(
    GRAPH_global_context *ggctx, HTAB *relation_cache,
    VLEMaterializerBuildObject build_object,
    VLEMaterializerOutputRequirement output_requirement,
    const VLE_path_container *vpc)
{
    VLEMaterializerOutputRequirement effective_output;

    Assert(vpc != NULL);

    effective_output = output_requirement;
    if (effective_output == VLE_MATERIALIZER_OUTPUT_UNKNOWN)
        effective_output = vpc->output_requirement;

    return make_vle_materializer_handoff(
        ggctx, relation_cache, build_object, effective_output,
        vpc->traversal_root_id, vpc->traversal_root_valid,
        vpc->candidate_vertex_id, vpc->candidate_vertex_valid);
}

static void prefetch_vle_materializer_vertices(
    const VLEMaterializerHandoff *handoff, const graphid *graphid_array,
    int64 graphid_array_size)
{
    graphid *vertex_ids;
    int64 vertex_count;
    int64 index;
    int64 vertex_index = 0;

    Assert(handoff != NULL);
    Assert(graphid_array != NULL || graphid_array_size == 0);

    if (graphid_array_size <= 3)
        return;

    vertex_count = (graphid_array_size + 1) / 2;
    vertex_ids = palloc(sizeof(graphid) * vertex_count);
    for (index = 0; index < graphid_array_size; index += 2)
        vertex_ids[vertex_index++] = graphid_array[index];

    age_vle_materializer_cache_prefetch_vertices(handoff, vertex_ids,
                                                 vertex_index);
    pfree(vertex_ids);
}

static agtype *build_path_agtype(VLE_path_container *vpc,
                                 VLEMaterializerObjectCache *object_cache)
{
    GRAPH_global_context *ggctx = NULL;
    agtype_build_state *path_state;
    agtype_build_state *scalar_state;
    HTAB *relation_cache = NULL;
    Oid graph_oid = InvalidOid;
    graphid *graphid_array = NULL;
    int64 graphid_array_size = 0;
    int index = 0;
    agtype *path;
    agtype *result;
    VLEMaterializerHandoff vertex_handoff;
    VLEMaterializerHandoff edge_handoff;

    graph_oid = vpc->graph_oid;
    ggctx = find_GRAPH_global_context(graph_oid);
    Assert(ggctx != NULL);

    graphid_array = GET_GRAPHID_ARRAY_FROM_CONTAINER(vpc);
    graphid_array_size = vpc->graphid_array_size;
    path_state = init_agtype_build_state(graphid_array_size, AGT_FARRAY);
    if (graphid_array_size > 3)
    {
        relation_cache = create_entry_property_relation_cache(
            "vle path raw materialization relation cache");
    }
    vertex_handoff = make_vle_materializer_handoff(
        ggctx, relation_cache, build_vle_vertex_object_agtype,
        VLE_MATERIALIZER_OUTPUT_PATH, vpc->traversal_root_id,
        vpc->traversal_root_valid, vpc->candidate_vertex_id,
        vpc->candidate_vertex_valid);
    edge_handoff = make_vle_materializer_handoff(
        ggctx, relation_cache, build_vle_edge_object_agtype,
        VLE_MATERIALIZER_OUTPUT_PATH, vpc->traversal_root_id,
        vpc->traversal_root_valid, vpc->candidate_vertex_id,
        vpc->candidate_vertex_valid);
    prefetch_vle_materializer_vertices(&vertex_handoff, graphid_array,
                                       graphid_array_size);

    for (index = 0; index < graphid_array_size; index += 2)
    {
        agtype *vertex;

        vertex = age_vle_materializer_cache_get_vertex_object(
            object_cache, &vertex_handoff, graphid_array[index]);
        write_extended(path_state, vertex, AGT_HEADER_VERTEX);
        if (object_cache == NULL)
            pfree(vertex);

        if (index + 1 < graphid_array_size)
        {
            agtype *edge;

            edge = age_vle_materializer_cache_get_edge_object(
                object_cache, &edge_handoff, graphid_array[index + 1]);
            write_extended(path_state, edge, AGT_HEADER_EDGE);
            if (object_cache == NULL)
                pfree(edge);
        }
    }

    destroy_entry_property_relation_cache(relation_cache);
    path = build_agtype(path_state);
    pfree_agtype_build_state(path_state);

    scalar_state = init_agtype_build_state(1, AGT_FARRAY | AGT_FSCALAR);
    write_extended(scalar_state, path, AGT_HEADER_PATH);
    result = build_agtype(scalar_state);
    pfree_agtype_build_state(scalar_state);
    pfree(path);

    return result;
}

static agtype *build_edge_list_agtype(VLE_path_container *vpc,
                                      VLEMaterializerObjectCache *object_cache)
{
    GRAPH_global_context *ggctx = NULL;
    agtype_build_state *edges_state;
    HTAB *relation_cache = NULL;
    graphid *graphid_array = NULL;
    int64 graphid_array_size = 0;
    int64 edge_count = 0;
    int64 index;
    agtype *result;
    VLEMaterializerHandoff edge_handoff;

    graphid_array_size = vpc->graphid_array_size;
    edge_count = (graphid_array_size - 1) / 2;
    edges_state = init_agtype_build_state(edge_count, AGT_FARRAY);
    if (edge_count > 1)
    {
        relation_cache = create_entry_property_relation_cache(
            "vle edge raw materialization relation cache");
    }

    ggctx = find_GRAPH_global_context(vpc->graph_oid);
    Assert(ggctx != NULL);
    edge_handoff = make_vle_materializer_handoff(
        ggctx, relation_cache, build_vle_edge_object_agtype,
        VLE_MATERIALIZER_OUTPUT_EDGE_LIST, vpc->traversal_root_id,
        vpc->traversal_root_valid, vpc->candidate_vertex_id,
        vpc->candidate_vertex_valid);

    graphid_array = GET_GRAPHID_ARRAY_FROM_CONTAINER(vpc);
    for (index = 1; index < graphid_array_size - 1; index += 2)
    {
        agtype *edge;

        edge = age_vle_materializer_cache_get_edge_object(
            object_cache, &edge_handoff, graphid_array[index]);
        write_extended(edges_state, edge, AGT_HEADER_EDGE);
        if (object_cache == NULL)
            pfree(edge);
    }

    destroy_entry_property_relation_cache(relation_cache);
    result = build_agtype(edges_state);
    pfree_agtype_build_state(edges_state);

    return result;
}

static agtype *build_node_list_agtype(VLE_path_container *vpc,
                                      VLEMaterializerObjectCache *object_cache)
{
    GRAPH_global_context *ggctx = NULL;
    agtype_build_state *nodes_state;
    HTAB *relation_cache = NULL;
    graphid *graphid_array = NULL;
    int64 graphid_array_size = 0;
    int64 node_count = 0;
    int64 index;
    agtype *result;
    VLEMaterializerHandoff vertex_handoff;

    ggctx = find_GRAPH_global_context(vpc->graph_oid);
    Assert(ggctx != NULL);

    graphid_array = GET_GRAPHID_ARRAY_FROM_CONTAINER(vpc);
    graphid_array_size = vpc->graphid_array_size;
    node_count = (graphid_array_size + 1) / 2;

    nodes_state = init_agtype_build_state(node_count, AGT_FARRAY);
    if (node_count > 1)
    {
        relation_cache = create_entry_property_relation_cache(
            "vle node raw materialization relation cache");
    }
    vertex_handoff = make_vle_materializer_handoff(
        ggctx, relation_cache, build_vle_vertex_object_agtype,
        VLE_MATERIALIZER_OUTPUT_NODE_LIST, vpc->traversal_root_id,
        vpc->traversal_root_valid, vpc->candidate_vertex_id,
        vpc->candidate_vertex_valid);
    prefetch_vle_materializer_vertices(&vertex_handoff, graphid_array,
                                       graphid_array_size);

    for (index = 0; index < graphid_array_size; index += 2)
    {
        agtype *vertex;

        vertex = age_vle_materializer_cache_get_vertex_object(
            object_cache, &vertex_handoff, graphid_array[index]);
        write_extended(nodes_state, vertex, AGT_HEADER_VERTEX);
        if (object_cache == NULL)
            pfree(vertex);
    }

    destroy_entry_property_relation_cache(relation_cache);
    result = build_agtype(nodes_state);
    pfree_agtype_build_state(nodes_state);

    return result;
}

/*
 * All front facing PG and exposed functions below
 */

/*
 * PG VLE function that takes the following input and returns a row called edges
 * of type agtype BINARY VLE_path_container (this is an internal structure for
 * returning a graphid array of the path. You need to use internal routines to
 * properly use this data) -
 *
 *     0 - agtype REQUIRED (graph name as string)
 *                 Note: This is automatically added by transform_FuncCall.
 *
 *     1 - agtype OPTIONAL (start vertex as a vertex or the integer id)
 *                 Note: Leaving this NULL switches the path algorithm from
 *                       VLE_FUNCTION_PATHS_BETWEEN to VLE_FUNCTION_PATHS_TO
 *     2 - agtype OPTIONAL (end vertex as a vertex or the integer id)
 *                 Note: Leaving this NULL switches the path algorithm from
 *                       VLE_FUNCTION_PATHS_BETWEEN to VLE_FUNCTION_PATHS_FROM
 *                       or - if the starting vertex is NULL - from
 *                       VLE_FUNCTION_PATHS_TO to VLE_FUNCTION_PATHS_ALL
 *     3 - agtype REQUIRED (edge prototype to match as an edge)
 *                 Note: Only the label and properties are used. The
 *                       rest is ignored.
 *     4 - agtype OPTIONAL lidx (lower range index)
 *                 Note: 0 itself is currently not supported but here it is
 *                       equivalent to 1.
 *                       A NULL is appropriate here for a 0 lower bound.
 *     5 - agtype OPTIONAL uidx (upper range index)
 *                 Note: A NULL is appropriate here for an infinite upper bound.
 *     6 - agtype REQUIRED edge direction (enum) as an integer. REQUIRED
 *
 * AGE VLE Stream CustomScan uses this iterator adapter to set up traversal
 * state and then pull one output row at a time.
 */
static AgeVLEIterator *age_vle_iterator_create(AgeVLEInput *input,
                                               FuncCallContext *funcctx)
{
    AgeVLEIterator *iterator;
    VLE_local_context *vlelctx;
    MemoryContext oldctx;

    if (funcctx == NULL)
        elog(ERROR, "age_vle iterator requires a function call context");

    /* all of these arguments need to be non NULL */
    if (age_vle_input_arg_is_null(input, 0) || /* graph name */
        age_vle_input_arg_is_null(input, 3) || /* edge prototype */
        age_vle_input_arg_is_null(input, 6))   /* direction */
    {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("age_vle: invalid NULL argument passed")));
    }

    oldctx = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);
    iterator = palloc0(sizeof(*iterator));
    iterator->funcctx = funcctx;

    /* build the local vle context */
    vlelctx = build_vle_local_context_for_input(
        input, funcctx, &vle_traversal_apply_ops,
        &vle_traversal_context_cache_ops);
    iterator->vlelctx = vlelctx;

    /*
     * If the context is NULL, there are no paths to find. This can happen when
     * a cached VLE context has exhausted its vertex list (e.g., from a NULL
     * OPTIONAL MATCH variable).
     */
    if (vlelctx == NULL)
    {
        iterator->exhausted = true;
        MemoryContextSwitchTo(oldctx);
        return iterator;
    }

    register_vle_local_context_cleanup(funcctx, vlelctx);

    /* if we are starting from zero [*0..x] flag it */
    if (age_vle_context_has_zero_bound_start(vlelctx))
    {
        iterator->zero_bound_pending = true;
    }

    MemoryContextSwitchTo(oldctx);

    return iterator;
}

AgeVLEIterator *age_vle_iterator_create_from_input(AgeVLEInput *input,
                                                   FuncCallContext *funcctx)
{
    if (input == NULL ||
        input->nargs < 0 ||
        input->nargs > AGE_VLE_MAX_ARGS)
    {
        elog(ERROR, "invalid age_vle iterator input");
    }

    return age_vle_iterator_create(input, funcctx);
}

static void finish_vle_iterator(AgeVLEIterator *iterator, bool mark_clean)
{
    VLE_local_context *vlelctx;

    Assert(iterator != NULL);

    vlelctx = iterator->vlelctx;
    if (vlelctx == NULL)
    {
        iterator->exhausted = true;
        return;
    }

    sync_vle_iterator_source_stats(iterator);
    cleanup_vle_local_context_resources(vlelctx);
    deactivate_vle_local_context_cleanup(vlelctx);

    if (mark_clean)
        vlelctx->is_dirty = false;

    if (vlelctx->use_cache == false)
        free_VLE_local_context(vlelctx);

    iterator->vlelctx = NULL;
    iterator->exhausted = true;
}

static void sync_vle_iterator_source_stats(AgeVLEIterator *iterator)
{
    Assert(iterator != NULL);

    if (iterator->vlelctx != NULL)
    {
        age_vle_context_get_source_stats(iterator->vlelctx,
                                         &iterator->source_stats);
    }
}

void age_vle_iterator_get_source_stats(AgeVLEIterator *iterator,
                                       AgeVLESourceStats *stats)
{
    Assert(stats != NULL);

    memset(stats, 0, sizeof(*stats));
    if (iterator == NULL)
        return;

    sync_vle_iterator_source_stats(iterator);
    *stats = iterator->source_stats;
}

static bool emit_materialized_terminal_property_batch(
    AgeVLEIterator *iterator, VLEIteratorOutputState *output,
    bool *handled)
{
    VLE_local_context *vlelctx;
    Datum property = (Datum) 0;
    bool property_is_null = true;

    Assert(iterator != NULL);
    Assert(output != NULL);
    Assert(handled != NULL);

    *handled = false;
    vlelctx = output->vlelctx;
    if (!age_vle_terminal_output_should_materialize_batch(vlelctx))
        return false;

    if (!age_vle_context_terminal_property_batch_materialized(vlelctx))
    {
        MemoryContext materialize_oldctx;

        materialize_oldctx =
            MemoryContextSwitchTo(output->funcctx->multi_call_memory_ctx);
        age_vle_terminal_output_materialize_batch(
            vlelctx, materialize_terminal_property_batch_path, vlelctx);
        MemoryContextSwitchTo(materialize_oldctx);
    }

    *handled = true;
    if (age_vle_context_next_terminal_property_batch_result(
            vlelctx, &property, &property_is_null))
    {
        if (property_is_null)
        {
            age_vle_iterator_output_target_set_null(&output->target);
            return true;
        }
        age_vle_iterator_output_target_set_datum(&output->target, property);
        return true;
    }

    finish_vle_iterator(iterator, false);
    return false;
}

static bool materialize_terminal_property_batch_path(void *state)
{
    Assert(state != NULL);

    return dfs_find_a_path_from(state);
}

static bool emit_terminal_property_output_callback(
    void *state, const VLEIteratorMaterialization *materialization,
    const VLEIteratorOutputTarget *target)
{
    VLEIteratorOutputBuildState *build_state;

    Assert(state != NULL);
    Assert(materialization != NULL);
    Assert(target != NULL);

    build_state = state;
    Assert(build_state->vlelctx != NULL);
    Assert(build_state->policy != NULL);

    return age_vle_terminal_output_emit_property(
        build_state->vlelctx, build_state->policy, build_state->funcctx,
        materialization->is_zero_bound, target);
}

static bool emit_terminal_full_properties_output_callback(
    void *state, const VLEIteratorOutputTarget *target)
{
    VLEIteratorOutputBuildState *build_state;

    Assert(state != NULL);
    Assert(target != NULL);

    build_state = state;
    return age_vle_terminal_output_emit_full_properties(
        build_state->vlelctx, build_state->funcctx, target);
}

static void *build_vle_iterator_container_callback(
    void *state, const VLEIteratorMaterialization *materialization)
{
    VLEIteratorOutputBuildState *build_state;
    VLEContainerBuildInput input;

    Assert(state != NULL);
    Assert(materialization != NULL);

    build_state = state;
    Assert(build_state->vlelctx != NULL);

    age_vle_context_init_container_build_input(build_state->vlelctx, &input);
    return age_vle_build_container(&input, materialization);
}

static void init_vle_iterator_output_callbacks(
    VLEIteratorOutputCallbacks *callbacks,
    VLEIteratorOutputBuildState *state)
{
    Assert(callbacks != NULL);
    Assert(state != NULL);

    callbacks->state = state;
    callbacks->emit_terminal_property =
        emit_terminal_property_output_callback;
    callbacks->emit_terminal_properties =
        emit_terminal_full_properties_output_callback;
    callbacks->build_container = build_vle_iterator_container_callback;
}

static bool search_vle_iterator_path_function(
    VLEIteratorSearchState *search)
{
    VLE_local_context *vlelctx;

    Assert(search != NULL);
    Assert(search->vlelctx != NULL);
    Assert(search->policy != NULL);

    vlelctx = search->vlelctx;
    switch (age_vle_context_path_function(vlelctx))
    {
        case VLE_FUNCTION_PATHS_TO:
            if (age_vle_context_reverse_paths_to(vlelctx))
                return dfs_find_a_path_from(vlelctx);
            /* fall through */
        case VLE_FUNCTION_PATHS_BETWEEN:
            return dfs_find_a_path_between(vlelctx);

        case VLE_FUNCTION_PATHS_ALL:
        case VLE_FUNCTION_PATHS_FROM:
            if (age_vle_terminal_output_uses_direct_dfs(search->policy))
                return dfs_find_terminal_property_path_from(vlelctx);
            return dfs_find_a_path_from(vlelctx);

        default:
            return false;
    }
}

static void advance_vle_iterator_start(AgeVLEIterator *iterator,
                                       VLEIteratorSearchState *search,
                                       bool *done)
{
    VLE_local_context *vlelctx;

    Assert(iterator != NULL);
    Assert(search != NULL);
    Assert(search->vlelctx != NULL);
    Assert(done != NULL);

    vlelctx = search->vlelctx;
    if (!age_vle_context_can_advance_start_vertex(vlelctx))
    {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("AGE VLE stream: invalid path function")));
    }

    age_vle_context_advance_start_vertex(vlelctx);
    apply_vle_start_vertex_activation(vlelctx, &vle_traversal_apply_ops);

    if (age_vle_context_is_empty_length_range(vlelctx))
    {
        *done = true;
    }
    else if (age_vle_context_has_zero_bound_start(vlelctx))
    {
        search->is_zero_bound =
            age_vle_terminal_output_path_matches_predicate(vlelctx, NULL);
        *done = search->is_zero_bound;
        iterator->zero_bound_pending = false;
    }
    else
    {
        *done = false;
    }
}

static bool search_vle_iterator_path(AgeVLEIterator *iterator,
                                     VLEIteratorSearchState *search)
{
    VLE_local_context *vlelctx;
    bool done = false;
    MemoryContext oldctx;

    Assert(iterator != NULL);
    Assert(search != NULL);
    Assert(search->vlelctx != NULL);

    vlelctx = search->vlelctx;
    search->found_a_path = false;
    search->is_zero_bound = false;

    if (age_vle_context_is_empty_length_range(vlelctx))
    {
        done = true;
    }
    else if (iterator->zero_bound_pending)
    {
        search->is_zero_bound =
            age_vle_terminal_output_path_matches_predicate(vlelctx, NULL);
        done = search->is_zero_bound;
        iterator->zero_bound_pending = false;
    }

    oldctx = MemoryContextSwitchTo(iterator->funcctx->multi_call_memory_ctx);

    while (!done)
    {
        search->found_a_path = search_vle_iterator_path_function(search);

        if (age_vle_context_search_is_complete(vlelctx,
                                               search->found_a_path))
        {
            done = true;
        }
        else
        {
            advance_vle_iterator_start(iterator, search, &done);
        }
    }

    MemoryContextSwitchTo(oldctx);

    return search->found_a_path || search->is_zero_bound;
}

bool age_vle_iterator_next(AgeVLEIterator *iterator, Datum *result,
                           bool *is_null)
{
    FuncCallContext *funcctx;
    VLE_local_context *vlelctx;
    VLETerminalOutputPolicy output_policy;
    VLEIteratorOutputState output_state;
    VLEIteratorOutputBuildState output_build_state;
    VLEIteratorOutputCallbacks output_callbacks;
    VLEIteratorSearchState search_state;
    bool output_handled = false;

    age_vle_iterator_output_target_init(&output_state.target, result,
                                        is_null);
    age_vle_iterator_output_target_reset(&output_state.target);

    if (iterator == NULL ||
        iterator->exhausted)
    {
        return false;
    }

    funcctx = iterator->funcctx;
    vlelctx = iterator->vlelctx;
    if (vlelctx == NULL)
    {
        iterator->exhausted = true;
        return false;
    }

    age_vle_context_init_terminal_output_policy(vlelctx, &output_policy);
    output_state.funcctx = funcctx;
    output_state.vlelctx = vlelctx;
    output_state.policy = &output_policy;

    output_build_state.funcctx = funcctx;
    output_build_state.vlelctx = vlelctx;
    output_build_state.policy = &output_policy;
    init_vle_iterator_output_callbacks(&output_callbacks,
                                       &output_build_state);

    if (emit_materialized_terminal_property_batch(iterator, &output_state,
                                                  &output_handled) ||
        output_handled)
    {
        return output_handled && !iterator->exhausted;
    }

    search_state.vlelctx = vlelctx;
    search_state.policy = &output_policy;
    if (search_vle_iterator_path(iterator, &search_state))
    {
        VLEIteratorMaterialization materialization;

        age_vle_iterator_materialization_init(
            &materialization,
            age_vle_context_output_requirement(vlelctx),
            output_policy.emit_property,
            age_vle_context_reverse_output_path(vlelctx),
            search_state.is_zero_bound);

        return age_vle_iterator_emit_result(
            &output_callbacks, &output_state.target, &materialization);
    }

    finish_vle_iterator(iterator, true);
    return false;
}

void age_vle_iterator_end(AgeVLEIterator *iterator)
{
    if (iterator == NULL)
        return;

    finish_vle_iterator(iterator, false);
}

/*
 * Exposed helper function to make an agtype AGTV_PATH from a
 * VLE_path_container.
 */
agtype *agt_materialize_vle_path(agtype *agt_arg_vpc)
{
    VLE_path_container *vpc = NULL;

    Assert(agt_arg_vpc != NULL);
    Assert(AGT_ROOT_IS_BINARY(agt_arg_vpc));
    Assert(AGT_ROOT_BINARY_FLAGS(agt_arg_vpc) == AGT_FBINARY_TYPE_VLE_PATH);

    vpc = (VLE_path_container *)agt_arg_vpc;

    return build_path_agtype(vpc, NULL);
}

/*
 * Exposed helper function to make an agtype_value AGTV_PATH from a
 * VLE_path_container.
 */
agtype_value *agtv_materialize_vle_path(agtype *agt_arg_vpc)
{
    VLE_path_container *vpc = NULL;
    agtype_value *agtv_path = NULL;

    /* the passed argument should not be NULL */
    Assert(agt_arg_vpc != NULL);

    /*
     * The path must be a binary container and the type of the object in the
     * container must be an AGT_FBINARY_TYPE_VLE_PATH.
     */
    Assert(AGT_ROOT_IS_BINARY(agt_arg_vpc));
    Assert(AGT_ROOT_BINARY_FLAGS(agt_arg_vpc) == AGT_FBINARY_TYPE_VLE_PATH);

    /* get the container */
    vpc = (VLE_path_container *)agt_arg_vpc;

    /* it should not be null */
    Assert(vpc != NULL);

    /* build the AGTV_PATH from the VLE_path_container */
    agtv_path = build_path(vpc);

    return agtv_path;
}

int64 agt_vle_append_path_interior(agtype *agt_arg_vpc,
                                   agtype_in_state *result)
{
    GRAPH_global_context *ggctx = NULL;
    VLE_path_container *vpc = NULL;
    HTAB *relation_cache = NULL;
    graphid *graphid_array = NULL;
    Oid graph_oid = InvalidOid;
    int64 graphid_array_size = 0;
    int64 index;
    int64 appended = 0;

    Assert(agt_arg_vpc != NULL);
    Assert(result != NULL);
    Assert(AGT_ROOT_IS_BINARY(agt_arg_vpc));
    Assert(AGT_ROOT_BINARY_FLAGS(agt_arg_vpc) == AGT_FBINARY_TYPE_VLE_PATH);

    vpc = (VLE_path_container *)agt_arg_vpc;
    graphid_array_size = vpc->graphid_array_size;
    if (graphid_array_size <= 1)
    {
        return 0;
    }

    graph_oid = vpc->graph_oid;
    ggctx = find_GRAPH_global_context(graph_oid);
    Assert(ggctx != NULL);
    graphid_array = GET_GRAPHID_ARRAY_FROM_CONTAINER(vpc);

    if (graphid_array_size > 3)
    {
        relation_cache = create_entry_property_relation_cache(
            "vle path interior materialization relation cache");
    }

    for (index = 1; index < graphid_array_size - 1; index++)
    {
        agtype_value *agtv_value = NULL;

        if (index % 2 == 1)
        {
            agtv_value = build_vle_edge_value(ggctx, graphid_array[index],
                                              relation_cache);
        }
        else
        {
            agtv_value = build_vle_vertex_value(ggctx, graphid_array[index],
                                                relation_cache);
        }

        result->res = push_agtype_value(&result->parse_state, WAGT_ELEM,
                                        agtv_value);
        appended++;
    }

    destroy_entry_property_relation_cache(relation_cache);

    return appended;
}

/* PG function to match 2 VLE edges */
PG_FUNCTION_INFO_V1(age_match_two_vle_edges);

Datum age_match_two_vle_edges(PG_FUNCTION_ARGS)
{
    agtype *agt_arg_vpc = NULL;
    VLE_path_container *left_path = NULL, *right_path = NULL;
    graphid *left_array, *right_array;
    int left_array_size;

    /*
     * If either argument is NULL, return FALSE. This can occur in
     * OPTIONAL MATCH (LEFT JOIN) contexts where a preceding clause
     * produced no results.
     */
    if (PG_ARGISNULL(0) || PG_ARGISNULL(1))
    {
        PG_RETURN_BOOL(false);
    }

    /* get the VLE_path_container argument */
    agt_arg_vpc = AG_GET_ARG_AGTYPE_P(0);

    if (!AGT_ROOT_IS_BINARY(agt_arg_vpc) ||
        AGT_ROOT_BINARY_FLAGS(agt_arg_vpc) != AGT_FBINARY_TYPE_VLE_PATH)
    {
        ereport(ERROR,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
            errmsg("argument 1 of age_match_two_vle_edges must be a VLE_Path_Container")));
    }

    /* cast argument as a VLE_Path_Container and extract graphid array */
    left_path = (VLE_path_container *)agt_arg_vpc;
    left_array_size = left_path->graphid_array_size;
    left_array = GET_GRAPHID_ARRAY_FROM_CONTAINER(left_path);

    agt_arg_vpc = AG_GET_ARG_AGTYPE_P(1);

    if (!AGT_ROOT_IS_BINARY(agt_arg_vpc) ||
        AGT_ROOT_BINARY_FLAGS(agt_arg_vpc) != AGT_FBINARY_TYPE_VLE_PATH)
    {
        ereport(ERROR,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
            errmsg("argument 2 of age_match_two_vle_edges must be a VLE_Path_Container")));
    }

    /* cast argument as a VLE_Path_Container and extract graphid array */
    right_path = (VLE_path_container *)agt_arg_vpc;
    right_array = GET_GRAPHID_ARRAY_FROM_CONTAINER(right_path);

    if (left_array[left_array_size - 1] != right_array[0])
    {
        PG_FREE_IF_COPY(left_path, 0);
        PG_FREE_IF_COPY(right_path, 1);
        PG_RETURN_BOOL(false);
    }

    PG_FREE_IF_COPY(left_path, 0);
    PG_FREE_IF_COPY(right_path, 1);
    PG_RETURN_BOOL(true);
}

/*
 * This function is used when we need to know if the passed in id is at the end
 * of a path. The first arg is the path, the second is the vertex id to check and
 * the last is a boolean that says whether to check the start or the end of the
 * vle path.
 */
PG_FUNCTION_INFO_V1(age_match_vle_edge_to_id_qual);

Datum age_match_vle_edge_to_id_qual(PG_FUNCTION_ARGS)
{
    agtype *agt_arg_vpc = NULL;
    agtype *edge_id = NULL;
    agtype *pos_agt = NULL;
    VLE_path_container *vle_path = NULL;
    graphid *array = NULL;
    bool vle_is_on_left = false;
    graphid gid = 0;
    Oid type1;

    /* check argument count */
    if (PG_NARGS() != 3)
    {
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("age_match_vle_edge_to_id_qual() invalid number of arguments")));
    }

    /*
     * If any argument is NULL, return FALSE. This can occur in
     * OPTIONAL MATCH (LEFT JOIN) contexts where a preceding clause
     * produced no results.
     */
    if (PG_ARGISNULL(0) || PG_ARGISNULL(1) || PG_ARGISNULL(2))
    {
        PG_RETURN_BOOL(false);
    }

    /* get the VLE_path_container argument */
    agt_arg_vpc = DATUM_GET_AGTYPE_P(PG_GETARG_DATUM(0));

    if (!AGT_ROOT_IS_BINARY(agt_arg_vpc) ||
        AGT_ROOT_BINARY_FLAGS(agt_arg_vpc) != AGT_FBINARY_TYPE_VLE_PATH)
    {
        ereport(ERROR,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
            errmsg("argument 1 of age_match_vle_edge_to_edge_qual must be a VLE_Path_Container")));
    }

    /* cast argument as a VLE_Path_Container and extract graphid array */
    vle_path = (VLE_path_container *)agt_arg_vpc;
    array = GET_GRAPHID_ARRAY_FROM_CONTAINER(vle_path);

    /*
     * Get arg type for argument 1 — cache in fn_extra to avoid
     * repeated expression type resolution.
     */
    if (fcinfo->flinfo->fn_extra == NULL)
    {
        Oid *cached_type = MemoryContextAlloc(fcinfo->flinfo->fn_mcxt,
                                               sizeof(Oid));
        *cached_type = get_fn_expr_argtype(fcinfo->flinfo, 1);
        fcinfo->flinfo->fn_extra = cached_type;
    }
    type1 = *(Oid *)fcinfo->flinfo->fn_extra;

    if (type1 == AGTYPEOID)
    {
        /* Get the edge id we are checking the end of the list too */
        edge_id = AG_GET_ARG_AGTYPE_P(1);
        gid = get_agtype_scalar_graphid_arg(edge_id,
            "argument 2 of age_match_vle_edge_to_edge_qual must be an integer");
    }
    else if (type1 == GRAPHIDOID)
    {
        gid = DATUM_GET_GRAPHID(PG_GETARG_DATUM(1));
    }
    else
    {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("match_vle_terminal_edge() argument 1 must be an agtype integer or a graphid")));
    }

    pos_agt = AG_GET_ARG_AGTYPE_P(2);

    vle_is_on_left = get_agtype_scalar_bool_arg(pos_agt,
        "argument 3 of age_match_vle_edge_to_edge_qual must be an integer");

    if (vle_is_on_left)
    {
        int array_size = vle_path->graphid_array_size;

        /*
         * Path is like ...[vle_edge]-()-[regular_edge]... Get the graphid of
         * the vertex at the endof the path and check that it matches the id
         * that was passed in the second arg. The transform logic is responsible
         * for making that the start or end id, depending on its direction.
         */
        if (gid != array[array_size - 1])
        {
            PG_RETURN_BOOL(false);
        }

        PG_RETURN_BOOL(true);
    }
    else
    {
        /*
         * Path is like ...[edge]-()-[vle_edge]... Get the vertex at the start
         * of the vle edge and check against id.
         */
        if (gid != array[0])
        {
            PG_RETURN_BOOL(false);
        }

        PG_RETURN_BOOL(true);
    }
}

/*
 * Exposed helper function to make an agtype_value AGTV_ARRAY of edges from a
 * VLE_path_container.
 */
agtype_value *agtv_materialize_vle_edges(agtype *agt_arg_vpc)
{
    VLE_path_container *vpc = NULL;
    agtype_value *agtv_array = NULL;

    /* the passed argument should not be NULL */
    Assert(agt_arg_vpc != NULL);

    /*
     * The path must be a binary container and the type of the object in the
     * container must be an AGT_FBINARY_TYPE_VLE_PATH.
     */
    Assert(AGT_ROOT_IS_BINARY(agt_arg_vpc));
    Assert(AGT_ROOT_BINARY_FLAGS(agt_arg_vpc) == AGT_FBINARY_TYPE_VLE_PATH);

    /* get the container */
    vpc = (VLE_path_container *)agt_arg_vpc;

    /* it should not be null */
    Assert(vpc != NULL);

    /* build the AGTV_ARRAY of edges from the VLE_path_container */
    agtv_array = build_edge_list(vpc);

    return agtv_array;

}

agtype_value *agtv_materialize_vle_edge_at(agtype *agt_arg_vpc,
                                           int64 edge_index)
{
    GRAPH_global_context *ggctx = NULL;
    VLE_path_container *vpc = NULL;
    graphid *graphid_array = NULL;
    int64 edge_count = 0;
    Oid graph_oid = InvalidOid;

    Assert(agt_arg_vpc != NULL);
    Assert(AGT_ROOT_IS_BINARY(agt_arg_vpc));
    Assert(AGT_ROOT_BINARY_FLAGS(agt_arg_vpc) == AGT_FBINARY_TYPE_VLE_PATH);

    vpc = (VLE_path_container *)agt_arg_vpc;
    edge_count = get_vle_container_edge_count(vpc);
    if (!normalize_vle_container_index(edge_count, &edge_index))
    {
        return NULL;
    }

    graph_oid = vpc->graph_oid;
    ggctx = find_GRAPH_global_context(graph_oid);
    Assert(ggctx != NULL);

    graphid_array = GET_GRAPHID_ARRAY_FROM_CONTAINER(vpc);

    return build_vle_edge_value(ggctx, graphid_array[(edge_index * 2) + 1],
                                NULL);
}

static agtype *materialize_vle_edge_at_agtype(
    agtype *agt_arg_vpc, int64 edge_index,
    VLEMaterializerObjectCache *object_cache)
{
    GRAPH_global_context *ggctx = NULL;
    VLE_path_container *vpc = NULL;
    graphid *graphid_array = NULL;
    int64 edge_count = 0;
    Oid graph_oid = InvalidOid;
    VLEMaterializerHandoff edge_handoff;

    Assert(agt_arg_vpc != NULL);
    Assert(AGT_ROOT_IS_BINARY(agt_arg_vpc));
    Assert(AGT_ROOT_BINARY_FLAGS(agt_arg_vpc) == AGT_FBINARY_TYPE_VLE_PATH);

    vpc = (VLE_path_container *)agt_arg_vpc;
    edge_count = get_vle_container_edge_count(vpc);
    if (!normalize_vle_container_index(edge_count, &edge_index))
    {
        return NULL;
    }

    graph_oid = vpc->graph_oid;
    ggctx = find_GRAPH_global_context(graph_oid);
    Assert(ggctx != NULL);

    graphid_array = GET_GRAPHID_ARRAY_FROM_CONTAINER(vpc);
    edge_handoff = make_vle_materializer_handoff_from_container(
        ggctx, NULL, build_vle_typed_edge_agtype,
        VLE_MATERIALIZER_OUTPUT_TYPED_EDGE, vpc);

    return age_vle_materializer_cache_get_typed_edge(
        object_cache, &edge_handoff, graphid_array[(edge_index * 2) + 1]);
}

static agtype *materialize_vle_vertex_at_agtype(
    agtype *agt_arg_vpc, int64 node_index,
    VLEMaterializerObjectCache *object_cache)
{
    GRAPH_global_context *ggctx = NULL;
    VLE_path_container *vpc = NULL;
    graphid *graphid_array = NULL;
    int64 node_count;
    VLEMaterializerHandoff vertex_handoff;

    Assert(agt_arg_vpc != NULL);
    Assert(AGT_ROOT_IS_BINARY(agt_arg_vpc));
    Assert(AGT_ROOT_BINARY_FLAGS(agt_arg_vpc) == AGT_FBINARY_TYPE_VLE_PATH);

    vpc = (VLE_path_container *)agt_arg_vpc;
    node_count = get_vle_container_node_count(vpc);
    if (!normalize_vle_container_index(node_count, &node_index))
    {
        return NULL;
    }

    ggctx = find_GRAPH_global_context(vpc->graph_oid);
    Assert(ggctx != NULL);
    graphid_array = GET_GRAPHID_ARRAY_FROM_CONTAINER(vpc);
    vertex_handoff = make_vle_materializer_handoff_from_container(
        ggctx, NULL, build_vle_typed_vertex_agtype,
        VLE_MATERIALIZER_OUTPUT_TYPED_VERTEX, vpc);

    return age_vle_materializer_cache_get_typed_vertex(
        object_cache, &vertex_handoff, graphid_array[node_index * 2]);
}

static agtype_value *agtv_materialize_vle_edge_endpoint_at(
    agtype *agt_arg_vpc, int64 edge_index, bool start_endpoint)
{
    GRAPH_global_context *ggctx = NULL;
    VLE_path_container *vpc = NULL;
    edge_entry *ee = NULL;
    graphid *graphid_array = NULL;
    graphid endpoint_id;
    graphid edge_id;
    int64 edge_count = 0;
    Oid graph_oid = InvalidOid;

    Assert(agt_arg_vpc != NULL);
    Assert(AGT_ROOT_IS_BINARY(agt_arg_vpc));
    Assert(AGT_ROOT_BINARY_FLAGS(agt_arg_vpc) == AGT_FBINARY_TYPE_VLE_PATH);

    vpc = (VLE_path_container *)agt_arg_vpc;
    edge_count = get_vle_container_edge_count(vpc);
    if (!normalize_vle_container_index(edge_count, &edge_index))
    {
        return NULL;
    }

    graph_oid = vpc->graph_oid;
    ggctx = find_GRAPH_global_context(graph_oid);
    Assert(ggctx != NULL);

    graphid_array = GET_GRAPHID_ARRAY_FROM_CONTAINER(vpc);
    edge_id = graphid_array[(edge_index * 2) + 1];
    ee = get_edge_entry(ggctx, edge_id);
    Assert(ee != NULL);

    endpoint_id = start_endpoint ?
        get_edge_entry_start_vertex_id(ee) :
        get_edge_entry_end_vertex_id(ee);

    return build_vle_vertex_value(ggctx, endpoint_id, NULL);
}

agtype_value *agtv_materialize_vle_edges_slice(agtype *agt_arg_vpc,
                                               int64 lower_index,
                                               int64 upper_index)
{
    GRAPH_global_context *ggctx = NULL;
    VLE_path_container *vpc = NULL;
    agtype_in_state edges_result;
    HTAB *relation_cache = NULL;
    graphid *graphid_array = NULL;
    Oid graph_oid = InvalidOid;
    int64 index;

    Assert(agt_arg_vpc != NULL);
    Assert(AGT_ROOT_IS_BINARY(agt_arg_vpc));
    Assert(AGT_ROOT_BINARY_FLAGS(agt_arg_vpc) == AGT_FBINARY_TYPE_VLE_PATH);

    if (lower_index >= upper_index)
    {
        return build_empty_agtype_value_array();
    }

    vpc = (VLE_path_container *)agt_arg_vpc;
    graph_oid = vpc->graph_oid;
    ggctx = find_GRAPH_global_context(graph_oid);
    Assert(ggctx != NULL);
    graphid_array = GET_GRAPHID_ARRAY_FROM_CONTAINER(vpc);

    MemSet(&edges_result, 0, sizeof(agtype_in_state));
    edges_result.res = push_agtype_value(&edges_result.parse_state,
                                         WAGT_BEGIN_ARRAY, NULL);

    if (upper_index - lower_index > 1)
    {
        relation_cache = create_entry_property_relation_cache(
            "vle edge slice materialization relation cache");
    }

    for (index = lower_index; index < upper_index; index++)
    {
        agtype_value *agtv_edge = NULL;

        agtv_edge = build_vle_edge_value(
            ggctx, graphid_array[(index * 2) + 1], relation_cache);
        edges_result.res = push_agtype_value(&edges_result.parse_state,
                                             WAGT_ELEM, agtv_edge);
    }

    edges_result.res = push_agtype_value(&edges_result.parse_state,
                                         WAGT_END_ARRAY, NULL);
    destroy_entry_property_relation_cache(relation_cache);
    edges_result.res->type = AGTV_ARRAY;

    return edges_result.res;
}

static agtype_value *agtv_materialize_vle_edges_reversed_slice(
    agtype *agt_arg_vpc, int64 lower_index, int64 upper_index)
{
    GRAPH_global_context *ggctx = NULL;
    VLE_path_container *vpc = NULL;
    agtype_in_state edges_result;
    HTAB *relation_cache = NULL;
    graphid *graphid_array = NULL;
    Oid graph_oid = InvalidOid;
    int64 edge_count;
    int64 index;

    Assert(agt_arg_vpc != NULL);
    Assert(AGT_ROOT_IS_BINARY(agt_arg_vpc));
    Assert(AGT_ROOT_BINARY_FLAGS(agt_arg_vpc) == AGT_FBINARY_TYPE_VLE_PATH);

    if (lower_index >= upper_index)
    {
        return build_empty_agtype_value_array();
    }

    vpc = (VLE_path_container *)agt_arg_vpc;
    edge_count = agtv_vle_edge_count(agt_arg_vpc);
    graph_oid = vpc->graph_oid;
    ggctx = find_GRAPH_global_context(graph_oid);
    Assert(ggctx != NULL);
    graphid_array = GET_GRAPHID_ARRAY_FROM_CONTAINER(vpc);

    MemSet(&edges_result, 0, sizeof(agtype_in_state));
    edges_result.res = push_agtype_value(&edges_result.parse_state,
                                         WAGT_BEGIN_ARRAY, NULL);

    if (upper_index - lower_index > 1)
    {
        relation_cache = create_entry_property_relation_cache(
            "vle reversed edge slice materialization relation cache");
    }

    for (index = upper_index - 1; index >= lower_index; index--)
    {
        agtype_value *agtv_edge = NULL;
        int64 original_index = edge_count - index - 1;

        agtv_edge = build_vle_edge_value(
            ggctx, graphid_array[(original_index * 2) + 1],
            relation_cache);
        edges_result.res = push_agtype_value(&edges_result.parse_state,
                                             WAGT_ELEM, agtv_edge);
    }

    edges_result.res = push_agtype_value(&edges_result.parse_state,
                                         WAGT_END_ARRAY, NULL);
    destroy_entry_property_relation_cache(relation_cache);
    edges_result.res->type = AGTV_ARRAY;

    return edges_result.res;
}

agtype_value *agtv_materialize_vle_edges_reversed(agtype *agt_arg_vpc)
{
    GRAPH_global_context *ggctx = NULL;
    VLE_path_container *vpc = NULL;
    agtype_in_state edges_result;
    HTAB *relation_cache = NULL;
    graphid *graphid_array = NULL;
    Oid graph_oid = InvalidOid;
    int64 edge_count;
    int64 index;

    Assert(agt_arg_vpc != NULL);
    Assert(AGT_ROOT_IS_BINARY(agt_arg_vpc));
    Assert(AGT_ROOT_BINARY_FLAGS(agt_arg_vpc) == AGT_FBINARY_TYPE_VLE_PATH);

    vpc = (VLE_path_container *)agt_arg_vpc;
    edge_count = agtv_vle_edge_count(agt_arg_vpc);
    if (edge_count == 0)
    {
        return build_empty_agtype_value_array();
    }

    graph_oid = vpc->graph_oid;
    ggctx = find_GRAPH_global_context(graph_oid);
    Assert(ggctx != NULL);
    graphid_array = GET_GRAPHID_ARRAY_FROM_CONTAINER(vpc);

    MemSet(&edges_result, 0, sizeof(agtype_in_state));
    edges_result.res = push_agtype_value(&edges_result.parse_state,
                                         WAGT_BEGIN_ARRAY, NULL);

    if (edge_count > 1)
    {
        relation_cache = create_entry_property_relation_cache(
            "vle edge reverse materialization relation cache");
    }

    for (index = edge_count - 1; index >= 0; index--)
    {
        agtype_value *agtv_edge = NULL;

        agtv_edge = build_vle_edge_value(
            ggctx, graphid_array[(index * 2) + 1], relation_cache);
        edges_result.res = push_agtype_value(&edges_result.parse_state,
                                             WAGT_ELEM, agtv_edge);
    }

    edges_result.res = push_agtype_value(&edges_result.parse_state,
                                         WAGT_END_ARRAY, NULL);
    destroy_entry_property_relation_cache(relation_cache);
    edges_result.res->type = AGTV_ARRAY;

    return edges_result.res;
}

agtype *agt_vle_edge_properties_at(agtype *agt_arg_vpc, int64 edge_index)
{
    GRAPH_global_context *ggctx = NULL;
    VLE_path_container *vpc = NULL;
    edge_entry *ee = NULL;
    graphid *graphid_array = NULL;
    graphid edge_id;
    int64 edge_count;

    Assert(agt_arg_vpc != NULL);
    Assert(AGT_ROOT_IS_BINARY(agt_arg_vpc));
    Assert(AGT_ROOT_BINARY_FLAGS(agt_arg_vpc) == AGT_FBINARY_TYPE_VLE_PATH);

    vpc = (VLE_path_container *)agt_arg_vpc;
    edge_count = get_vle_container_edge_count(vpc);
    if (!normalize_vle_container_index(edge_count, &edge_index))
    {
        return NULL;
    }

    graphid_array = GET_GRAPHID_ARRAY_FROM_CONTAINER(vpc);
    edge_id = graphid_array[(edge_index * 2) + 1];

    ggctx = find_GRAPH_global_context(vpc->graph_oid);
    Assert(ggctx != NULL);
    ee = get_edge_entry(ggctx, edge_id);
    Assert(ee != NULL);

    return DATUM_GET_AGTYPE_P(get_edge_entry_properties(ee));
}

agtype_value *agtv_materialize_vle_nodes(agtype *agt_arg_vpc)
{
    GRAPH_global_context *ggctx = NULL;
    VLE_path_container *vpc = NULL;
    agtype_in_state nodes_result;
    HTAB *relation_cache = NULL;
    graphid *graphid_array = NULL;
    Oid graph_oid = InvalidOid;
    int64 graphid_array_size = 0;
    int64 index;

    Assert(agt_arg_vpc != NULL);
    Assert(AGT_ROOT_IS_BINARY(agt_arg_vpc));
    Assert(AGT_ROOT_BINARY_FLAGS(agt_arg_vpc) == AGT_FBINARY_TYPE_VLE_PATH);

    vpc = (VLE_path_container *)agt_arg_vpc;
    graph_oid = vpc->graph_oid;
    ggctx = find_GRAPH_global_context(graph_oid);
    Assert(ggctx != NULL);
    graphid_array = GET_GRAPHID_ARRAY_FROM_CONTAINER(vpc);
    graphid_array_size = vpc->graphid_array_size;

    MemSet(&nodes_result, 0, sizeof(agtype_in_state));
    nodes_result.res = push_agtype_value(&nodes_result.parse_state,
                                         WAGT_BEGIN_ARRAY, NULL);

    if (graphid_array_size > 1)
    {
        relation_cache = create_entry_property_relation_cache(
            "vle node materialization relation cache");
    }

    for (index = 0; index < graphid_array_size; index += 2)
    {
        agtype_value *agtv_vertex = NULL;

        agtv_vertex = build_vle_vertex_value(ggctx, graphid_array[index],
                                             relation_cache);
        nodes_result.res = push_agtype_value(&nodes_result.parse_state,
                                             WAGT_ELEM, agtv_vertex);
    }

    nodes_result.res = push_agtype_value(&nodes_result.parse_state,
                                         WAGT_END_ARRAY, NULL);
    destroy_entry_property_relation_cache(relation_cache);
    nodes_result.res->type = AGTV_ARRAY;

    return nodes_result.res;
}

static agtype_value *agtv_materialize_vle_nodes_slice(agtype *agt_arg_vpc,
                                                      int64 lower_index,
                                                      int64 upper_index)
{
    GRAPH_global_context *ggctx = NULL;
    VLE_path_container *vpc = NULL;
    agtype_in_state nodes_result;
    HTAB *relation_cache = NULL;
    graphid *graphid_array = NULL;
    Oid graph_oid = InvalidOid;
    int64 index;

    Assert(agt_arg_vpc != NULL);
    Assert(AGT_ROOT_IS_BINARY(agt_arg_vpc));
    Assert(AGT_ROOT_BINARY_FLAGS(agt_arg_vpc) == AGT_FBINARY_TYPE_VLE_PATH);

    if (lower_index >= upper_index)
    {
        return build_empty_agtype_value_array();
    }

    vpc = (VLE_path_container *)agt_arg_vpc;
    graph_oid = vpc->graph_oid;
    ggctx = find_GRAPH_global_context(graph_oid);
    Assert(ggctx != NULL);
    graphid_array = GET_GRAPHID_ARRAY_FROM_CONTAINER(vpc);

    MemSet(&nodes_result, 0, sizeof(agtype_in_state));
    nodes_result.res = push_agtype_value(&nodes_result.parse_state,
                                         WAGT_BEGIN_ARRAY, NULL);

    if (upper_index - lower_index > 1)
    {
        relation_cache = create_entry_property_relation_cache(
            "vle node slice materialization relation cache");
    }

    for (index = lower_index; index < upper_index; index++)
    {
        agtype_value *agtv_vertex = NULL;

        agtv_vertex = build_vle_vertex_value(
            ggctx, graphid_array[index * 2], relation_cache);
        nodes_result.res = push_agtype_value(&nodes_result.parse_state,
                                             WAGT_ELEM, agtv_vertex);
    }

    nodes_result.res = push_agtype_value(&nodes_result.parse_state,
                                         WAGT_END_ARRAY, NULL);
    destroy_entry_property_relation_cache(relation_cache);
    nodes_result.res->type = AGTV_ARRAY;

    return nodes_result.res;
}

static agtype_value *agtv_materialize_vle_nodes_reversed_slice(
    agtype *agt_arg_vpc, int64 lower_index, int64 upper_index)
{
    GRAPH_global_context *ggctx = NULL;
    VLE_path_container *vpc = NULL;
    agtype_in_state nodes_result;
    HTAB *relation_cache = NULL;
    graphid *graphid_array = NULL;
    Oid graph_oid = InvalidOid;
    int64 node_count;
    int64 index;

    Assert(agt_arg_vpc != NULL);
    Assert(AGT_ROOT_IS_BINARY(agt_arg_vpc));
    Assert(AGT_ROOT_BINARY_FLAGS(agt_arg_vpc) == AGT_FBINARY_TYPE_VLE_PATH);

    if (lower_index >= upper_index)
    {
        return build_empty_agtype_value_array();
    }

    vpc = (VLE_path_container *)agt_arg_vpc;
    graph_oid = vpc->graph_oid;
    ggctx = find_GRAPH_global_context(graph_oid);
    Assert(ggctx != NULL);
    graphid_array = GET_GRAPHID_ARRAY_FROM_CONTAINER(vpc);
    node_count = agtv_vle_node_count(agt_arg_vpc);

    MemSet(&nodes_result, 0, sizeof(agtype_in_state));
    nodes_result.res = push_agtype_value(&nodes_result.parse_state,
                                         WAGT_BEGIN_ARRAY, NULL);

    if (upper_index - lower_index > 1)
    {
        relation_cache = create_entry_property_relation_cache(
            "vle reversed node slice materialization relation cache");
    }

    for (index = upper_index - 1; index >= lower_index; index--)
    {
        agtype_value *agtv_vertex = NULL;
        int64 original_index = node_count - index - 1;

        agtv_vertex = build_vle_vertex_value(
            ggctx, graphid_array[original_index * 2], relation_cache);
        nodes_result.res = push_agtype_value(&nodes_result.parse_state,
                                             WAGT_ELEM, agtv_vertex);
    }

    nodes_result.res = push_agtype_value(&nodes_result.parse_state,
                                         WAGT_END_ARRAY, NULL);
    destroy_entry_property_relation_cache(relation_cache);
    nodes_result.res->type = AGTV_ARRAY;

    return nodes_result.res;
}

static agtype_value *agtv_materialize_vle_nodes_reversed(agtype *agt_arg_vpc)
{
    GRAPH_global_context *ggctx = NULL;
    VLE_path_container *vpc = NULL;
    agtype_in_state nodes_result;
    HTAB *relation_cache = NULL;
    graphid *graphid_array = NULL;
    Oid graph_oid = InvalidOid;
    int64 node_count;
    int64 index;

    Assert(agt_arg_vpc != NULL);
    Assert(AGT_ROOT_IS_BINARY(agt_arg_vpc));
    Assert(AGT_ROOT_BINARY_FLAGS(agt_arg_vpc) == AGT_FBINARY_TYPE_VLE_PATH);

    vpc = (VLE_path_container *)agt_arg_vpc;
    graph_oid = vpc->graph_oid;
    ggctx = find_GRAPH_global_context(graph_oid);
    Assert(ggctx != NULL);
    graphid_array = GET_GRAPHID_ARRAY_FROM_CONTAINER(vpc);
    node_count = agtv_vle_node_count(agt_arg_vpc);

    MemSet(&nodes_result, 0, sizeof(agtype_in_state));
    nodes_result.res = push_agtype_value(&nodes_result.parse_state,
                                         WAGT_BEGIN_ARRAY, NULL);

    if (node_count > 1)
    {
        relation_cache = create_entry_property_relation_cache(
            "vle node reverse materialization relation cache");
    }

    for (index = node_count - 1; index >= 0; index--)
    {
        agtype_value *agtv_vertex = NULL;

        agtv_vertex = build_vle_vertex_value(
            ggctx, graphid_array[index * 2], relation_cache);
        nodes_result.res = push_agtype_value(&nodes_result.parse_state,
                                             WAGT_ELEM, agtv_vertex);
    }

    nodes_result.res = push_agtype_value(&nodes_result.parse_state,
                                         WAGT_END_ARRAY, NULL);
    destroy_entry_property_relation_cache(relation_cache);
    nodes_result.res->type = AGTV_ARRAY;

    return nodes_result.res;
}

static inline int64 get_vle_container_edge_count(
    const VLE_path_container *vpc)
{
    Assert(vpc != NULL);

    return (vpc->graphid_array_size - 1) / 2;
}

static inline int64 get_vle_container_node_count(
    const VLE_path_container *vpc)
{
    Assert(vpc != NULL);

    return (vpc->graphid_array_size + 1) / 2;
}

static inline bool normalize_vle_container_index(int64 count, int64 *index)
{
    Assert(index != NULL);

    if (*index < 0)
        *index = count + *index;

    return *index >= 0 && *index < count;
}

int64 agtv_vle_edge_count(agtype *agt_arg_vpc)
{
    VLE_path_container *vpc = NULL;

    Assert(agt_arg_vpc != NULL);
    Assert(AGT_ROOT_IS_BINARY(agt_arg_vpc));
    Assert(AGT_ROOT_BINARY_FLAGS(agt_arg_vpc) == AGT_FBINARY_TYPE_VLE_PATH);

    vpc = (VLE_path_container *)agt_arg_vpc;

    return get_vle_container_edge_count(vpc);
}

bool agt_vle_contains_edge_id(agtype *agt_arg_vpc, graphid edge_id)
{
    VLE_path_container *vpc = NULL;
    graphid *graphid_array = NULL;
    int64 graphid_array_size = 0;
    int64 i;

    Assert(agt_arg_vpc != NULL);
    Assert(AGT_ROOT_IS_BINARY(agt_arg_vpc));
    Assert(AGT_ROOT_BINARY_FLAGS(agt_arg_vpc) == AGT_FBINARY_TYPE_VLE_PATH);

    vpc = (VLE_path_container *)agt_arg_vpc;
    graphid_array = GET_GRAPHID_ARRAY_FROM_CONTAINER(vpc);
    graphid_array_size = vpc->graphid_array_size;

    for (i = 1; i < graphid_array_size - 1; i += 2)
    {
        if (graphid_array[i] == edge_id)
        {
            return true;
        }
    }

    return false;
}

static bool get_vle_edge_id_at_index(agtype *agt_arg_vpc, int64 edge_index,
                                     graphid *edge_id)
{
    VLE_path_container *vpc = NULL;
    graphid *graphid_array = NULL;
    int64 edge_count;

    Assert(agt_arg_vpc != NULL);
    Assert(edge_id != NULL);
    Assert(AGT_ROOT_IS_BINARY(agt_arg_vpc));
    Assert(AGT_ROOT_BINARY_FLAGS(agt_arg_vpc) == AGT_FBINARY_TYPE_VLE_PATH);

    vpc = (VLE_path_container *)agt_arg_vpc;
    edge_count = get_vle_container_edge_count(vpc);
    if (!normalize_vle_container_index(edge_count, &edge_index))
    {
        return false;
    }

    graphid_array = GET_GRAPHID_ARRAY_FROM_CONTAINER(vpc);
    *edge_id = graphid_array[(edge_index * 2) + 1];

    return true;
}

static int64 agtv_vle_node_count(agtype *agt_arg_vpc)
{
    VLE_path_container *vpc = NULL;

    Assert(agt_arg_vpc != NULL);
    Assert(AGT_ROOT_IS_BINARY(agt_arg_vpc));
    Assert(AGT_ROOT_BINARY_FLAGS(agt_arg_vpc) == AGT_FBINARY_TYPE_VLE_PATH);

    vpc = (VLE_path_container *)agt_arg_vpc;

    return get_vle_container_node_count(vpc);
}

/* PG wrapper function for agtv_materialize_vle_edges */
PG_FUNCTION_INFO_V1(age_materialize_vle_edges);

Datum age_materialize_vle_edges(PG_FUNCTION_ARGS)
{
    agtype *agt_arg_vpc = NULL;
    VLE_path_container *vpc = NULL;
    VLEMaterializerObjectCache *object_cache;
    int64 edge_count;

    /* if we have a NULL VLE_path_container, return NULL */
    if (PG_ARGISNULL(0))
    {
        PG_RETURN_NULL();
    }

    /* get the VLE_path_container argument */
    agt_arg_vpc = AG_GET_ARG_AGTYPE_P(0);

    /* if NULL, return NULL */
    if (is_agtype_null(agt_arg_vpc))
    {
        PG_RETURN_NULL();
    }

    edge_count = agtv_vle_edge_count(agt_arg_vpc);
    if (edge_count == 0)
    {
        PG_RETURN_POINTER(build_empty_agtype_array());
    }

    Assert(AGT_ROOT_IS_BINARY(agt_arg_vpc));
    Assert(AGT_ROOT_BINARY_FLAGS(agt_arg_vpc) == AGT_FBINARY_TYPE_VLE_PATH);
    vpc = (VLE_path_container *)agt_arg_vpc;
    object_cache = age_vle_materializer_object_cache_get(fcinfo, vpc->graph_oid);

    PG_RETURN_POINTER(build_edge_list_agtype(vpc, object_cache));
}

PG_FUNCTION_INFO_V1(age_materialize_vle_nodes);

Datum age_materialize_vle_nodes(PG_FUNCTION_ARGS)
{
    agtype *agt_arg_vpc = NULL;
    VLE_path_container *vpc = NULL;
    VLEMaterializerObjectCache *object_cache;

    if (PG_ARGISNULL(0))
    {
        PG_RETURN_NULL();
    }

    agt_arg_vpc = AG_GET_ARG_AGTYPE_P(0);
    if (is_agtype_null(agt_arg_vpc))
    {
        PG_RETURN_NULL();
    }

    Assert(AGT_ROOT_IS_BINARY(agt_arg_vpc));
    Assert(AGT_ROOT_BINARY_FLAGS(agt_arg_vpc) == AGT_FBINARY_TYPE_VLE_PATH);
    vpc = (VLE_path_container *)agt_arg_vpc;
    object_cache = age_vle_materializer_object_cache_get(fcinfo, vpc->graph_oid);

    PG_RETURN_POINTER(build_node_list_agtype(vpc, object_cache));
}

PG_FUNCTION_INFO_V1(age_vle_path_node_count);

Datum age_vle_path_node_count(PG_FUNCTION_ARGS)
{
    agtype *agt_arg_vpc = NULL;
    agtype_value agtv_result;

    if (PG_ARGISNULL(0))
    {
        PG_RETURN_NULL();
    }

    agt_arg_vpc = AG_GET_ARG_AGTYPE_P(0);
    if (is_agtype_null(agt_arg_vpc))
    {
        PG_RETURN_NULL();
    }

    agtv_result.type = AGTV_INTEGER;
    agtv_result.val.int_value = agtv_vle_node_count(agt_arg_vpc);

    PG_RETURN_POINTER(agtype_value_to_agtype(&agtv_result));
}

PG_FUNCTION_INFO_V1(age_vle_edge_tail_count);

Datum age_vle_edge_tail_count(PG_FUNCTION_ARGS)
{
    agtype *agt_arg_vpc = NULL;
    agtype_value agtv_result;
    int64 edge_count;

    if (PG_ARGISNULL(0))
    {
        PG_RETURN_NULL();
    }

    agt_arg_vpc = AG_GET_ARG_AGTYPE_P(0);
    if (is_agtype_null(agt_arg_vpc))
    {
        PG_RETURN_NULL();
    }

    edge_count = agtv_vle_edge_count(agt_arg_vpc);
    agtv_result.type = AGTV_INTEGER;
    agtv_result.val.int_value = edge_count > 0 ? edge_count - 1 : 0;

    PG_RETURN_POINTER(agtype_value_to_agtype(&agtv_result));
}

PG_FUNCTION_INFO_V1(age_vle_list_is_empty);

Datum age_vle_list_is_empty(PG_FUNCTION_ARGS)
{
    agtype *agt_arg_vpc = NULL;
    agtype_value agtv_mode;
    bool mode_needs_free = false;
    int64 edge_count;
    int64 node_count;
    int64 mode;
    bool result;

    if (PG_ARGISNULL(0) || PG_ARGISNULL(1))
    {
        PG_RETURN_NULL();
    }

    agt_arg_vpc = AG_GET_ARG_AGTYPE_P(0);
    if (is_agtype_null(agt_arg_vpc))
    {
        PG_RETURN_NULL();
    }

    get_vle_scalar_arg_no_copy("age_vle_list_is_empty",
                               AG_GET_ARG_AGTYPE_P(1), AGTV_INTEGER, true,
                               &agtv_mode, &mode_needs_free);
    mode = agtv_mode.val.int_value;
    if (mode_needs_free)
    {
        pfree_agtype_value_content(&agtv_mode);
    }

    edge_count = agtv_vle_edge_count(agt_arg_vpc);
    node_count = edge_count + 1;

    switch (mode)
    {
        case 0:
            result = node_count == 0;
            break;

        case 1:
            result = edge_count == 0;
            break;

        case 2:
            result = node_count <= 1;
            break;

        case 3:
            result = edge_count <= 1;
            break;

        default:
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                     errmsg("age_vle_list_is_empty: invalid mode")));
    }

    PG_RETURN_BOOL(result);
}

PG_FUNCTION_INFO_V1(age_vle_list_slice_count);

Datum age_vle_list_slice_count(PG_FUNCTION_ARGS)
{
    agtype *agt_arg_vpc = NULL;
    agtype *agt_lidx = NULL;
    agtype *agt_uidx = NULL;
    agtype_value lidx_value;
    agtype_value uidx_value;
    agtype_value mode_value;
    agtype_value agtv_result;
    bool lidx_needs_free = false;
    bool uidx_needs_free = false;
    bool mode_needs_free = false;
    int64 edge_count;
    int64 list_count;
    int64 lower_index = 0;
    int64 upper_index = 0;
    int64 mode;

    if (PG_ARGISNULL(0) || PG_ARGISNULL(3))
    {
        PG_RETURN_NULL();
    }

    if (PG_ARGISNULL(1) && PG_ARGISNULL(2))
    {
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("slice start and/or end is required")));
    }

    agt_arg_vpc = AG_GET_ARG_AGTYPE_P(0);
    if (is_agtype_null(agt_arg_vpc))
    {
        PG_RETURN_NULL();
    }

    get_vle_scalar_arg_no_copy("age_vle_list_slice_count",
                               AG_GET_ARG_AGTYPE_P(3), AGTV_INTEGER, true,
                               &mode_value, &mode_needs_free);
    mode = mode_value.val.int_value;
    if (mode_needs_free)
    {
        pfree_agtype_value_content(&mode_value);
    }

    edge_count = agtv_vle_edge_count(agt_arg_vpc);
    switch (mode)
    {
    case 0:
        list_count = edge_count;
        break;
    case 1:
        list_count = edge_count + 1;
        break;
    case 2:
        list_count = Max(edge_count - 1, 0);
        break;
    case 3:
        list_count = edge_count;
        break;
    case 6:
        list_count = Max(edge_count - 2, 0);
        break;
    case 7:
        list_count = Max(edge_count - 1, 0);
        break;
    case 8:
        list_count = Max(edge_count - 1, 0);
        break;
    case 9:
        list_count = edge_count;
        break;
    case 10:
        list_count = Max(edge_count - 1, 0);
        break;
    case 11:
        list_count = edge_count;
        break;
    default:
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("age_vle_list_slice_count: invalid mode")));
    }

    if (PG_ARGISNULL(1))
    {
        lower_index = 0;
    }
    else
    {
        agt_lidx = AG_GET_ARG_AGTYPE_P(1);
        get_vle_scalar_arg_no_copy("age_vle_list_slice_count",
                                   agt_lidx, AGTV_INTEGER, false,
                                   &lidx_value, &lidx_needs_free);
        if (lidx_value.type == AGTV_NULL)
        {
            if (lidx_needs_free)
            {
                pfree_agtype_value_content(&lidx_value);
            }
            PG_RETURN_NULL();
        }
        if (lidx_value.type != AGTV_INTEGER)
        {
            if (lidx_needs_free)
            {
                pfree_agtype_value_content(&lidx_value);
            }
            ereport(ERROR,
                    (errmsg("array slices must resolve to an integer value")));
        }
        lower_index = lidx_value.val.int_value;
        if (lidx_needs_free)
        {
            pfree_agtype_value_content(&lidx_value);
        }
    }

    if (PG_ARGISNULL(2))
    {
        upper_index = list_count;
    }
    else
    {
        agt_uidx = AG_GET_ARG_AGTYPE_P(2);
        get_vle_scalar_arg_no_copy("age_vle_list_slice_count",
                                   agt_uidx, AGTV_INTEGER, false,
                                   &uidx_value, &uidx_needs_free);
        if (uidx_value.type == AGTV_NULL)
        {
            if (uidx_needs_free)
            {
                pfree_agtype_value_content(&uidx_value);
            }
            PG_RETURN_NULL();
        }
        if (uidx_value.type != AGTV_INTEGER)
        {
            if (uidx_needs_free)
            {
                pfree_agtype_value_content(&uidx_value);
            }
            ereport(ERROR,
                    (errmsg("array slices must resolve to an integer value")));
        }
        upper_index = uidx_value.val.int_value;
        if (uidx_needs_free)
        {
            pfree_agtype_value_content(&uidx_value);
        }
    }

    if (lower_index < 0)
    {
        lower_index = list_count + lower_index;
    }
    if (lower_index < 0)
    {
        lower_index = 0;
    }
    if (lower_index > list_count)
    {
        lower_index = list_count;
    }
    if (upper_index < 0)
    {
        upper_index = list_count + upper_index;
    }
    if (upper_index < 0)
    {
        upper_index = 0;
    }
    if (upper_index > list_count)
    {
        upper_index = list_count;
    }

    agtv_result.type = AGTV_INTEGER;
    agtv_result.val.int_value = Max(upper_index - lower_index, 0);

    PG_RETURN_POINTER(agtype_value_to_agtype(&agtv_result));
}

PG_FUNCTION_INFO_V1(age_vle_list_slice_is_empty);

Datum age_vle_list_slice_is_empty(PG_FUNCTION_ARGS)
{
    agtype *agt_arg_vpc = NULL;
    agtype *agt_lidx = NULL;
    agtype *agt_uidx = NULL;
    agtype_value lidx_value;
    agtype_value uidx_value;
    agtype_value mode_value;
    bool lidx_needs_free = false;
    bool uidx_needs_free = false;
    bool mode_needs_free = false;
    int64 edge_count;
    int64 list_count;
    int64 lower_index = 0;
    int64 upper_index = 0;
    int64 mode;

    if (PG_ARGISNULL(0) || PG_ARGISNULL(3))
    {
        PG_RETURN_NULL();
    }

    if (PG_ARGISNULL(1) && PG_ARGISNULL(2))
    {
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("slice start and/or end is required")));
    }

    agt_arg_vpc = AG_GET_ARG_AGTYPE_P(0);
    if (is_agtype_null(agt_arg_vpc))
    {
        PG_RETURN_NULL();
    }

    get_vle_scalar_arg_no_copy("age_vle_list_slice_is_empty",
                               AG_GET_ARG_AGTYPE_P(3), AGTV_INTEGER, true,
                               &mode_value, &mode_needs_free);
    mode = mode_value.val.int_value;
    if (mode_needs_free)
    {
        pfree_agtype_value_content(&mode_value);
    }

    edge_count = agtv_vle_edge_count(agt_arg_vpc);
    switch (mode)
    {
    case 0:
        list_count = edge_count;
        break;
    case 1:
        list_count = edge_count + 1;
        break;
    case 2:
        list_count = Max(edge_count - 1, 0);
        break;
    case 3:
        list_count = edge_count;
        break;
    case 6:
        list_count = Max(edge_count - 2, 0);
        break;
    case 7:
        list_count = Max(edge_count - 1, 0);
        break;
    case 8:
        list_count = Max(edge_count - 1, 0);
        break;
    case 9:
        list_count = edge_count;
        break;
    case 10:
        list_count = Max(edge_count - 1, 0);
        break;
    case 11:
        list_count = edge_count;
        break;
    default:
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("age_vle_list_slice_is_empty: invalid mode")));
    }

    if (PG_ARGISNULL(1))
    {
        lower_index = 0;
    }
    else
    {
        agt_lidx = AG_GET_ARG_AGTYPE_P(1);
        get_vle_scalar_arg_no_copy("age_vle_list_slice_is_empty",
                                   agt_lidx, AGTV_INTEGER, false,
                                   &lidx_value, &lidx_needs_free);
        if (lidx_value.type == AGTV_NULL)
        {
            if (lidx_needs_free)
            {
                pfree_agtype_value_content(&lidx_value);
            }
            PG_RETURN_NULL();
        }
        if (lidx_value.type != AGTV_INTEGER)
        {
            if (lidx_needs_free)
            {
                pfree_agtype_value_content(&lidx_value);
            }
            ereport(ERROR,
                    (errmsg("array slices must resolve to an integer value")));
        }
        lower_index = lidx_value.val.int_value;
        if (lidx_needs_free)
        {
            pfree_agtype_value_content(&lidx_value);
        }
    }

    if (PG_ARGISNULL(2))
    {
        upper_index = list_count;
    }
    else
    {
        agt_uidx = AG_GET_ARG_AGTYPE_P(2);
        get_vle_scalar_arg_no_copy("age_vle_list_slice_is_empty",
                                   agt_uidx, AGTV_INTEGER, false,
                                   &uidx_value, &uidx_needs_free);
        if (uidx_value.type == AGTV_NULL)
        {
            if (uidx_needs_free)
            {
                pfree_agtype_value_content(&uidx_value);
            }
            PG_RETURN_NULL();
        }
        if (uidx_value.type != AGTV_INTEGER)
        {
            if (uidx_needs_free)
            {
                pfree_agtype_value_content(&uidx_value);
            }
            ereport(ERROR,
                    (errmsg("array slices must resolve to an integer value")));
        }
        upper_index = uidx_value.val.int_value;
        if (uidx_needs_free)
        {
            pfree_agtype_value_content(&uidx_value);
        }
    }

    if (lower_index < 0)
    {
        lower_index = list_count + lower_index;
    }
    if (lower_index < 0)
    {
        lower_index = 0;
    }
    if (lower_index > list_count)
    {
        lower_index = list_count;
    }
    if (upper_index < 0)
    {
        upper_index = list_count + upper_index;
    }
    if (upper_index < 0)
    {
        upper_index = 0;
    }
    if (upper_index > list_count)
    {
        upper_index = list_count;
    }

    PG_RETURN_BOOL(upper_index <= lower_index);
}

PG_FUNCTION_INFO_V1(age_materialize_vle_slice_boundary);

static int64 decode_vle_slice_boundary_mode(int64 mode,
                                            VLESliceBoundaryMode *decoded)
{
    MemSet(decoded, 0, sizeof(*decoded));

    if (mode >= VLE_SLICE_BOUNDARY_SLICE_REVERSE_OFFSET)
    {
        decoded->slice_reverse_list = true;
        mode -= VLE_SLICE_BOUNDARY_SLICE_REVERSE_OFFSET;
    }
    else if (mode >= VLE_SLICE_BOUNDARY_SLICE_TAIL_OFFSET)
    {
        decoded->slice_tail_list = true;
        mode -= VLE_SLICE_BOUNDARY_SLICE_TAIL_OFFSET;
    }
    else if (mode >= VLE_SLICE_BOUNDARY_DOUBLE_TAIL_OFFSET)
    {
        decoded->double_tail_list = true;
        mode -= VLE_SLICE_BOUNDARY_DOUBLE_TAIL_OFFSET;
    }
    else if (mode >= VLE_SLICE_BOUNDARY_TAIL_REVERSE_OFFSET)
    {
        decoded->tail_reverse_list = true;
        mode -= VLE_SLICE_BOUNDARY_TAIL_REVERSE_OFFSET;
    }
    else if (mode >= VLE_SLICE_BOUNDARY_REVERSE_OFFSET)
    {
        decoded->reverse_list = true;
        mode -= VLE_SLICE_BOUNDARY_REVERSE_OFFSET;
    }

    if (mode >= VLE_SLICE_BOUNDARY_ID_OFFSET &&
        mode < VLE_SLICE_BOUNDARY_LABEL_OFFSET)
    {
        decoded->return_id = true;
        mode -= VLE_SLICE_BOUNDARY_ID_OFFSET;
    }
    else if (mode >= VLE_SLICE_BOUNDARY_LABEL_OFFSET &&
             mode < VLE_SLICE_BOUNDARY_LABELS_OFFSET)
    {
        decoded->return_label = true;
        mode -= VLE_SLICE_BOUNDARY_LABEL_OFFSET;
    }
    else if (mode >= VLE_SLICE_BOUNDARY_LABELS_OFFSET &&
             mode < VLE_SLICE_BOUNDARY_PROPERTIES_OFFSET)
    {
        decoded->return_labels = true;
        mode -= VLE_SLICE_BOUNDARY_LABELS_OFFSET;
    }
    else if (mode >= VLE_SLICE_BOUNDARY_PROPERTIES_OFFSET &&
             mode < VLE_SLICE_BOUNDARY_START_NODE_OFFSET)
    {
        decoded->return_properties = true;
        mode -= VLE_SLICE_BOUNDARY_PROPERTIES_OFFSET;
    }
    else if (mode >= VLE_SLICE_BOUNDARY_START_NODE_OFFSET &&
             mode < VLE_SLICE_BOUNDARY_END_NODE_OFFSET)
    {
        decoded->return_endpoint = true;
        decoded->start_endpoint = true;
        mode -= VLE_SLICE_BOUNDARY_START_NODE_OFFSET;
    }
    else if (mode >= VLE_SLICE_BOUNDARY_END_NODE_OFFSET &&
             mode < VLE_SLICE_BOUNDARY_START_ID_OFFSET)
    {
        decoded->return_endpoint = true;
        decoded->start_endpoint = false;
        mode -= VLE_SLICE_BOUNDARY_END_NODE_OFFSET;
    }
    else if (mode >= VLE_SLICE_BOUNDARY_START_ID_OFFSET &&
             mode < VLE_SLICE_BOUNDARY_END_ID_OFFSET)
    {
        decoded->return_endpoint_id = true;
        decoded->start_endpoint = true;
        mode -= VLE_SLICE_BOUNDARY_START_ID_OFFSET;
    }
    else if (mode >= VLE_SLICE_BOUNDARY_END_ID_OFFSET &&
             mode < VLE_SLICE_BOUNDARY_START_LABEL_OFFSET)
    {
        decoded->return_endpoint_id = true;
        decoded->start_endpoint = false;
        mode -= VLE_SLICE_BOUNDARY_END_ID_OFFSET;
    }
    else if (mode >= VLE_SLICE_BOUNDARY_START_LABEL_OFFSET &&
             mode < VLE_SLICE_BOUNDARY_END_LABEL_OFFSET)
    {
        decoded->return_endpoint_label = true;
        decoded->start_endpoint = true;
        mode -= VLE_SLICE_BOUNDARY_START_LABEL_OFFSET;
    }
    else if (mode >= VLE_SLICE_BOUNDARY_END_LABEL_OFFSET &&
             mode < VLE_SLICE_BOUNDARY_START_LABELS_OFFSET)
    {
        decoded->return_endpoint_label = true;
        decoded->start_endpoint = false;
        mode -= VLE_SLICE_BOUNDARY_END_LABEL_OFFSET;
    }
    else if (mode >= VLE_SLICE_BOUNDARY_START_LABELS_OFFSET &&
             mode < VLE_SLICE_BOUNDARY_END_LABELS_OFFSET)
    {
        decoded->return_endpoint_labels = true;
        decoded->start_endpoint = true;
        mode -= VLE_SLICE_BOUNDARY_START_LABELS_OFFSET;
    }
    else if (mode >= VLE_SLICE_BOUNDARY_END_LABELS_OFFSET &&
             mode < VLE_SLICE_BOUNDARY_START_PROPERTIES_OFFSET)
    {
        decoded->return_endpoint_labels = true;
        decoded->start_endpoint = false;
        mode -= VLE_SLICE_BOUNDARY_END_LABELS_OFFSET;
    }
    else if (mode >= VLE_SLICE_BOUNDARY_START_PROPERTIES_OFFSET &&
             mode < VLE_SLICE_BOUNDARY_END_PROPERTIES_OFFSET)
    {
        decoded->return_endpoint_properties = true;
        decoded->start_endpoint = true;
        mode -= VLE_SLICE_BOUNDARY_START_PROPERTIES_OFFSET;
    }
    else if (mode >= VLE_SLICE_BOUNDARY_END_PROPERTIES_OFFSET &&
             mode < VLE_SLICE_BOUNDARY_REVERSE_OFFSET)
    {
        decoded->return_endpoint_properties = true;
        decoded->start_endpoint = false;
        mode -= VLE_SLICE_BOUNDARY_END_PROPERTIES_OFFSET;
    }

    return mode;
}

Datum age_materialize_vle_slice_boundary(PG_FUNCTION_ARGS)
{
    GRAPH_global_context *ggctx = NULL;
    VLE_path_container *vpc = NULL;
    agtype *agt_arg_vpc = NULL;
    agtype *agt_lidx = NULL;
    agtype *agt_uidx = NULL;
    agtype_value lidx_value;
    agtype_value uidx_value;
    agtype_value mode_value;
    agtype_value id_result;
    agtype_value label_result;
    agtype_value label_array_elem;
    agtype_in_state label_array_result;
    agtype_value *agtv_result = NULL;
    VLESliceBoundaryMode decoded;
    bool lidx_needs_free = false;
    bool uidx_needs_free = false;
    bool mode_needs_free = false;
    bool return_node = false;
    bool return_last = false;
    graphid *graphid_array = NULL;
    int64 edge_count;
    int64 list_count;
    int64 lower_index = 0;
    int64 upper_index = 0;
    int64 base_index = 0;
    int64 original_index;
    int64 mode;

    if (PG_ARGISNULL(0) || PG_ARGISNULL(3))
    {
        PG_RETURN_NULL();
    }

    if (PG_ARGISNULL(1) && PG_ARGISNULL(2))
    {
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("slice start and/or end is required")));
    }

    agt_arg_vpc = AG_GET_ARG_AGTYPE_P(0);
    if (is_agtype_null(agt_arg_vpc))
    {
        PG_RETURN_NULL();
    }

    get_vle_scalar_arg_no_copy("age_materialize_vle_slice_boundary",
                               AG_GET_ARG_AGTYPE_P(3), AGTV_INTEGER, true,
                               &mode_value, &mode_needs_free);
    mode = mode_value.val.int_value;
    if (mode_needs_free)
    {
        pfree_agtype_value_content(&mode_value);
    }
    mode = decode_vle_slice_boundary_mode(mode, &decoded);

    edge_count = agtv_vle_edge_count(agt_arg_vpc);
    switch (mode)
    {
    case 0:
    case 1:
        list_count = edge_count;
        return_node = false;
        return_last = mode == 1;
        break;
    case 2:
    case 3:
        list_count = edge_count + 1;
        return_node = true;
        return_last = mode == 3;
        break;
    case 4:
    case 5:
        if (decoded.double_tail_list)
        {
            list_count = Max(edge_count - 2, 0);
            base_index = 2;
        }
        else if (decoded.tail_reverse_list)
        {
            list_count = Max(edge_count - 1, 0);
        }
        else
        {
            list_count = Max(edge_count - 1, 0);
            base_index = 1;
        }
        return_node = false;
        return_last = mode == 5;
        break;
    case 6:
    case 7:
        if (decoded.double_tail_list)
        {
            list_count = Max(edge_count - 1, 0);
            base_index = 2;
        }
        else if (decoded.tail_reverse_list)
        {
            list_count = edge_count;
        }
        else
        {
            list_count = edge_count;
            base_index = 1;
        }
        return_node = true;
        return_last = mode == 7;
        break;
    default:
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("age_materialize_vle_slice_boundary: invalid mode")));
    }

    if (PG_ARGISNULL(1))
    {
        lower_index = 0;
    }
    else
    {
        agt_lidx = AG_GET_ARG_AGTYPE_P(1);
        get_vle_scalar_arg_no_copy("age_materialize_vle_slice_boundary",
                                   agt_lidx, AGTV_INTEGER, false,
                                   &lidx_value, &lidx_needs_free);
        if (lidx_value.type == AGTV_NULL)
        {
            if (lidx_needs_free)
            {
                pfree_agtype_value_content(&lidx_value);
            }
            PG_RETURN_NULL();
        }
        if (lidx_value.type != AGTV_INTEGER)
        {
            if (lidx_needs_free)
            {
                pfree_agtype_value_content(&lidx_value);
            }
            ereport(ERROR,
                    (errmsg("array slices must resolve to an integer value")));
        }
        lower_index = lidx_value.val.int_value;
        if (lidx_needs_free)
        {
            pfree_agtype_value_content(&lidx_value);
        }
    }

    if (PG_ARGISNULL(2))
    {
        upper_index = list_count;
    }
    else
    {
        agt_uidx = AG_GET_ARG_AGTYPE_P(2);
        get_vle_scalar_arg_no_copy("age_materialize_vle_slice_boundary",
                                   agt_uidx, AGTV_INTEGER, false,
                                   &uidx_value, &uidx_needs_free);
        if (uidx_value.type == AGTV_NULL)
        {
            if (uidx_needs_free)
            {
                pfree_agtype_value_content(&uidx_value);
            }
            PG_RETURN_NULL();
        }
        if (uidx_value.type != AGTV_INTEGER)
        {
            if (uidx_needs_free)
            {
                pfree_agtype_value_content(&uidx_value);
            }
            ereport(ERROR,
                    (errmsg("array slices must resolve to an integer value")));
        }
        upper_index = uidx_value.val.int_value;
        if (uidx_needs_free)
        {
            pfree_agtype_value_content(&uidx_value);
        }
    }

    if (lower_index < 0)
    {
        lower_index = list_count + lower_index;
    }
    if (lower_index < 0)
    {
        lower_index = 0;
    }
    if (lower_index > list_count)
    {
        lower_index = list_count;
    }
    if (upper_index < 0)
    {
        upper_index = list_count + upper_index;
    }
    if (upper_index < 0)
    {
        upper_index = 0;
    }
    if (upper_index > list_count)
    {
        upper_index = list_count;
    }

    if (upper_index <= lower_index)
    {
        PG_RETURN_NULL();
    }

    if (decoded.slice_tail_list)
    {
        if (upper_index <= lower_index + 1)
        {
            PG_RETURN_NULL();
        }

        original_index = return_last ? upper_index - 1 : lower_index + 1;
    }
    else if (decoded.slice_reverse_list)
    {
        original_index = return_last ? lower_index : upper_index - 1;
    }
    else
    {
        original_index = return_last ? upper_index - 1 : lower_index;
    }
    if (decoded.reverse_list || decoded.tail_reverse_list)
    {
        original_index = list_count - original_index - 1;
    }
    original_index += base_index;
    vpc = (VLE_path_container *)agt_arg_vpc;
    graphid_array = GET_GRAPHID_ARRAY_FROM_CONTAINER(vpc);

    if (return_node)
    {
        graphid node_id = graphid_array[original_index * 2];

        if (decoded.return_endpoint || decoded.return_endpoint_id || decoded.return_endpoint_label ||
            decoded.return_endpoint_labels || decoded.return_endpoint_properties)
        {
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                     errmsg("age_materialize_vle_slice_boundary: endpoint mode is invalid for nodes")));
        }

        if (decoded.return_id)
        {
            id_result.type = AGTV_INTEGER;
            id_result.val.int_value = node_id;
            PG_RETURN_POINTER(agtype_value_to_agtype(&id_result));
        }

        ggctx = find_GRAPH_global_context(vpc->graph_oid);
        Assert(ggctx != NULL);

        if (decoded.return_label || decoded.return_labels || decoded.return_properties)
        {
            vertex_entry *ve = get_vertex_entry(ggctx, node_id);
            char *label_name = NULL;

            Assert(ve != NULL);
            if (decoded.return_properties)
            {
                PG_RETURN_DATUM(get_vertex_entry_properties(ve));
            }

            label_name = get_vertex_entry_label_name(ve);
            label_result.type = AGTV_STRING;
            label_result.val.string.val = label_name;
            label_result.val.string.len = strlen(label_name);
            if (decoded.return_label)
            {
                PG_RETURN_POINTER(agtype_value_to_agtype(&label_result));
            }

            label_array_elem = label_result;
            MemSet(&label_array_result, 0, sizeof(agtype_in_state));
            label_array_result.res = push_agtype_value(
                &label_array_result.parse_state, WAGT_BEGIN_ARRAY, NULL);
            label_array_result.res = push_agtype_value(
                &label_array_result.parse_state, WAGT_ELEM,
                &label_array_elem);
            label_array_result.res = push_agtype_value(
                &label_array_result.parse_state, WAGT_END_ARRAY, NULL);

            PG_RETURN_POINTER(agtype_value_to_agtype(label_array_result.res));
        }

        agtv_result = build_vle_vertex_value(ggctx, node_id, NULL);
    }
    else
    {
        graphid edge_id = graphid_array[(original_index * 2) + 1];

        if (decoded.return_id)
        {
            id_result.type = AGTV_INTEGER;
            id_result.val.int_value = edge_id;
            PG_RETURN_POINTER(agtype_value_to_agtype(&id_result));
        }

        ggctx = find_GRAPH_global_context(vpc->graph_oid);
        Assert(ggctx != NULL);

        if (decoded.return_label || decoded.return_properties)
        {
            edge_entry *ee = get_edge_entry(ggctx, edge_id);
            char *label_name = NULL;

            Assert(ee != NULL);
            if (decoded.return_properties)
            {
                PG_RETURN_DATUM(get_edge_entry_properties(ee));
            }

            label_name = get_edge_entry_label_name(ee);
            label_result.type = AGTV_STRING;
            label_result.val.string.val = label_name;
            label_result.val.string.len = strlen(label_name);

            PG_RETURN_POINTER(agtype_value_to_agtype(&label_result));
        }
        if (decoded.return_labels)
        {
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                     errmsg("age_materialize_vle_slice_boundary: labels mode is invalid for relationships")));
        }
        if (decoded.return_endpoint || decoded.return_endpoint_id || decoded.return_endpoint_label ||
            decoded.return_endpoint_labels || decoded.return_endpoint_properties)
        {
            edge_entry *ee = get_edge_entry(ggctx, edge_id);
            graphid endpoint_id;

            Assert(ee != NULL);
            endpoint_id = decoded.start_endpoint ?
                get_edge_entry_start_vertex_id(ee) :
                get_edge_entry_end_vertex_id(ee);

            if (decoded.return_endpoint_id)
            {
                id_result.type = AGTV_INTEGER;
                id_result.val.int_value = endpoint_id;
                PG_RETURN_POINTER(agtype_value_to_agtype(&id_result));
            }
            if (decoded.return_endpoint)
            {
                agtv_result = build_vle_vertex_value(ggctx, endpoint_id, NULL);
                PG_RETURN_POINTER(agtype_value_to_agtype(agtv_result));
            }
            if (decoded.return_endpoint_label || decoded.return_endpoint_labels ||
                decoded.return_endpoint_properties)
            {
                vertex_entry *ve = get_vertex_entry(ggctx, endpoint_id);
                char *label_name = NULL;

                Assert(ve != NULL);
                if (decoded.return_endpoint_properties)
                {
                    PG_RETURN_DATUM(get_vertex_entry_properties(ve));
                }

                label_name = get_vertex_entry_label_name(ve);
                label_result.type = AGTV_STRING;
                label_result.val.string.val = label_name;
                label_result.val.string.len = strlen(label_name);
                if (decoded.return_endpoint_label)
                {
                    PG_RETURN_POINTER(agtype_value_to_agtype(&label_result));
                }

                label_array_elem = label_result;
                MemSet(&label_array_result, 0, sizeof(agtype_in_state));
                label_array_result.res = push_agtype_value(
                    &label_array_result.parse_state, WAGT_BEGIN_ARRAY, NULL);
                label_array_result.res = push_agtype_value(
                    &label_array_result.parse_state, WAGT_ELEM,
                    &label_array_elem);
                label_array_result.res = push_agtype_value(
                    &label_array_result.parse_state, WAGT_END_ARRAY, NULL);

                PG_RETURN_POINTER(
                    agtype_value_to_agtype(label_array_result.res));
            }
        }

        agtv_result = build_vle_edge_value(ggctx, edge_id, NULL);
    }

    PG_RETURN_POINTER(agtype_value_to_agtype(agtv_result));
}

PG_FUNCTION_INFO_V1(age_materialize_vle_node_at);

Datum age_materialize_vle_node_at(PG_FUNCTION_ARGS)
{
    agtype *agt_arg_vpc = NULL;
    agtype_value agtv_index;
    agtype *vertex = NULL;
    VLEMaterializerObjectCache *object_cache;
    VLE_path_container *vpc;
    bool index_needs_free = false;
    int64 node_index;

    if (PG_ARGISNULL(0) || PG_ARGISNULL(1))
    {
        PG_RETURN_NULL();
    }

    agt_arg_vpc = AG_GET_ARG_AGTYPE_P(0);
    if (is_agtype_null(agt_arg_vpc))
    {
        PG_RETURN_NULL();
    }
    Assert(AGT_ROOT_IS_BINARY(agt_arg_vpc));
    Assert(AGT_ROOT_BINARY_FLAGS(agt_arg_vpc) == AGT_FBINARY_TYPE_VLE_PATH);
    vpc = (VLE_path_container *)agt_arg_vpc;
    object_cache = age_vle_materializer_object_cache_get(fcinfo, vpc->graph_oid);

    get_vle_scalar_arg_no_copy("age_materialize_vle_node_at",
                               AG_GET_ARG_AGTYPE_P(1), AGTV_INTEGER, true,
                               &agtv_index, &index_needs_free);

    node_index = agtv_index.val.int_value;
    if (index_needs_free)
    {
        pfree_agtype_value_content(&agtv_index);
    }

    vertex = materialize_vle_vertex_at_agtype(agt_arg_vpc, node_index,
                                              object_cache);
    if (vertex == NULL)
    {
        PG_RETURN_NULL();
    }

    PG_RETURN_POINTER(vertex);
}

PG_FUNCTION_INFO_V1(age_materialize_vle_node_reversed_at);

Datum age_materialize_vle_node_reversed_at(PG_FUNCTION_ARGS)
{
    agtype *agt_arg_vpc = NULL;
    agtype_value agtv_index;
    agtype *vertex = NULL;
    VLEMaterializerObjectCache *object_cache;
    VLE_path_container *vpc;
    bool index_needs_free = false;
    int64 node_index;

    if (PG_ARGISNULL(0) || PG_ARGISNULL(1))
    {
        PG_RETURN_NULL();
    }

    agt_arg_vpc = AG_GET_ARG_AGTYPE_P(0);
    if (is_agtype_null(agt_arg_vpc))
    {
        PG_RETURN_NULL();
    }
    Assert(AGT_ROOT_IS_BINARY(agt_arg_vpc));
    Assert(AGT_ROOT_BINARY_FLAGS(agt_arg_vpc) == AGT_FBINARY_TYPE_VLE_PATH);
    vpc = (VLE_path_container *)agt_arg_vpc;
    object_cache = age_vle_materializer_object_cache_get(fcinfo, vpc->graph_oid);

    get_vle_scalar_arg_no_copy("age_materialize_vle_node_reversed_at",
                               AG_GET_ARG_AGTYPE_P(1), AGTV_INTEGER, true,
                               &agtv_index, &index_needs_free);

    node_index = agtv_index.val.int_value;
    if (index_needs_free)
    {
        pfree_agtype_value_content(&agtv_index);
    }

    if (node_index == PG_INT64_MIN)
    {
        PG_RETURN_NULL();
    }

    vertex = materialize_vle_vertex_at_agtype(agt_arg_vpc, -node_index - 1,
                                              object_cache);
    if (vertex == NULL)
    {
        PG_RETURN_NULL();
    }

    PG_RETURN_POINTER(vertex);
}

PG_FUNCTION_INFO_V1(age_materialize_vle_node_tail_last);

Datum age_materialize_vle_node_tail_last(PG_FUNCTION_ARGS)
{
    GRAPH_global_context *ggctx = NULL;
    VLE_path_container *vpc = NULL;
    agtype *agt_arg_vpc = NULL;
    graphid *graphid_array = NULL;
    int64 node_count;
    agtype_value *agtv_vertex = NULL;

    if (PG_ARGISNULL(0))
    {
        PG_RETURN_NULL();
    }

    agt_arg_vpc = AG_GET_ARG_AGTYPE_P(0);
    if (is_agtype_null(agt_arg_vpc))
    {
        PG_RETURN_NULL();
    }

    Assert(AGT_ROOT_IS_BINARY(agt_arg_vpc));
    Assert(AGT_ROOT_BINARY_FLAGS(agt_arg_vpc) == AGT_FBINARY_TYPE_VLE_PATH);

    vpc = (VLE_path_container *)agt_arg_vpc;
    node_count = agtv_vle_node_count(agt_arg_vpc);
    if (node_count <= 1)
    {
        PG_RETURN_NULL();
    }

    ggctx = find_GRAPH_global_context(vpc->graph_oid);
    Assert(ggctx != NULL);
    graphid_array = GET_GRAPHID_ARRAY_FROM_CONTAINER(vpc);

    agtv_vertex = build_vle_vertex_value(
        ggctx, graphid_array[(node_count - 1) * 2], NULL);

    PG_RETURN_POINTER(agtype_value_to_agtype(agtv_vertex));
}

PG_FUNCTION_INFO_V1(age_vle_node_tail_last_id);

Datum age_vle_node_tail_last_id(PG_FUNCTION_ARGS)
{
    VLE_path_container *vpc = NULL;
    agtype *agt_arg_vpc = NULL;
    agtype_value agtv_result;
    graphid *graphid_array = NULL;
    int64 node_count;

    if (PG_ARGISNULL(0))
    {
        PG_RETURN_NULL();
    }

    agt_arg_vpc = AG_GET_ARG_AGTYPE_P(0);
    if (is_agtype_null(agt_arg_vpc))
    {
        PG_RETURN_NULL();
    }

    Assert(AGT_ROOT_IS_BINARY(agt_arg_vpc));
    Assert(AGT_ROOT_BINARY_FLAGS(agt_arg_vpc) == AGT_FBINARY_TYPE_VLE_PATH);

    vpc = (VLE_path_container *)agt_arg_vpc;
    node_count = agtv_vle_node_count(agt_arg_vpc);
    if (node_count <= 1)
    {
        PG_RETURN_NULL();
    }

    graphid_array = GET_GRAPHID_ARRAY_FROM_CONTAINER(vpc);
    agtv_result.type = AGTV_INTEGER;
    agtv_result.val.int_value = graphid_array[(node_count - 1) * 2];

    PG_RETURN_POINTER(agtype_value_to_agtype(&agtv_result));
}

PG_FUNCTION_INFO_V1(age_vle_node_id_at);

Datum age_vle_node_id_at(PG_FUNCTION_ARGS)
{
    VLE_path_container *vpc = NULL;
    agtype *agt_arg_vpc = NULL;
    agtype_value agtv_index;
    agtype_value agtv_result;
    bool index_needs_free = false;
    graphid *graphid_array = NULL;
    int64 node_count;
    int64 node_index;

    if (PG_ARGISNULL(0) || PG_ARGISNULL(1))
    {
        PG_RETURN_NULL();
    }

    agt_arg_vpc = AG_GET_ARG_AGTYPE_P(0);
    if (is_agtype_null(agt_arg_vpc))
    {
        PG_RETURN_NULL();
    }

    get_vle_scalar_arg_no_copy("age_vle_node_id_at",
                               AG_GET_ARG_AGTYPE_P(1), AGTV_INTEGER, true,
                               &agtv_index, &index_needs_free);

    node_index = agtv_index.val.int_value;
    if (index_needs_free)
    {
        pfree_agtype_value_content(&agtv_index);
    }

    Assert(AGT_ROOT_IS_BINARY(agt_arg_vpc));
    Assert(AGT_ROOT_BINARY_FLAGS(agt_arg_vpc) == AGT_FBINARY_TYPE_VLE_PATH);

    vpc = (VLE_path_container *)agt_arg_vpc;
    node_count = get_vle_container_node_count(vpc);
    if (!normalize_vle_container_index(node_count, &node_index))
    {
        PG_RETURN_NULL();
    }

    graphid_array = GET_GRAPHID_ARRAY_FROM_CONTAINER(vpc);
    agtv_result.type = AGTV_INTEGER;
    agtv_result.val.int_value = graphid_array[node_index * 2];

    PG_RETURN_POINTER(agtype_value_to_agtype(&agtv_result));
}

PG_FUNCTION_INFO_V1(age_vle_node_label_at);

Datum age_vle_node_label_at(PG_FUNCTION_ARGS)
{
    GRAPH_global_context *ggctx = NULL;
    VLE_path_container *vpc = NULL;
    vertex_entry *ve = NULL;
    agtype *agt_arg_vpc = NULL;
    agtype_value agtv_index;
    agtype_value agtv_result;
    bool index_needs_free = false;
    graphid *graphid_array = NULL;
    graphid node_id;
    int64 node_count;
    int64 node_index;
    char *label_name = NULL;

    if (PG_ARGISNULL(0) || PG_ARGISNULL(1))
    {
        PG_RETURN_NULL();
    }

    agt_arg_vpc = AG_GET_ARG_AGTYPE_P(0);
    if (is_agtype_null(agt_arg_vpc))
    {
        PG_RETURN_NULL();
    }

    get_vle_scalar_arg_no_copy("age_vle_node_label_at",
                               AG_GET_ARG_AGTYPE_P(1), AGTV_INTEGER, true,
                               &agtv_index, &index_needs_free);

    node_index = agtv_index.val.int_value;
    if (index_needs_free)
    {
        pfree_agtype_value_content(&agtv_index);
    }

    Assert(AGT_ROOT_IS_BINARY(agt_arg_vpc));
    Assert(AGT_ROOT_BINARY_FLAGS(agt_arg_vpc) == AGT_FBINARY_TYPE_VLE_PATH);

    vpc = (VLE_path_container *)agt_arg_vpc;
    node_count = get_vle_container_node_count(vpc);
    if (!normalize_vle_container_index(node_count, &node_index))
    {
        PG_RETURN_NULL();
    }

    graphid_array = GET_GRAPHID_ARRAY_FROM_CONTAINER(vpc);
    node_id = graphid_array[node_index * 2];

    ggctx = find_GRAPH_global_context(vpc->graph_oid);
    Assert(ggctx != NULL);
    ve = get_vertex_entry(ggctx, node_id);
    Assert(ve != NULL);

    label_name = get_vertex_entry_label_name(ve);
    agtv_result.type = AGTV_STRING;
    agtv_result.val.string.val = label_name;
    agtv_result.val.string.len = strlen(label_name);

    PG_RETURN_POINTER(agtype_value_to_agtype(&agtv_result));
}

PG_FUNCTION_INFO_V1(age_vle_node_labels_at);

Datum age_vle_node_labels_at(PG_FUNCTION_ARGS)
{
    GRAPH_global_context *ggctx = NULL;
    VLE_path_container *vpc = NULL;
    vertex_entry *ve = NULL;
    agtype *agt_arg_vpc = NULL;
    agtype_value agtv_index;
    agtype_value agtv_label;
    agtype_in_state agis_result;
    bool index_needs_free = false;
    graphid *graphid_array = NULL;
    graphid node_id;
    int64 node_count;
    int64 node_index;
    char *label_name = NULL;

    if (PG_ARGISNULL(0) || PG_ARGISNULL(1))
    {
        PG_RETURN_NULL();
    }

    agt_arg_vpc = AG_GET_ARG_AGTYPE_P(0);
    if (is_agtype_null(agt_arg_vpc))
    {
        PG_RETURN_NULL();
    }

    get_vle_scalar_arg_no_copy("age_vle_node_labels_at",
                               AG_GET_ARG_AGTYPE_P(1), AGTV_INTEGER, true,
                               &agtv_index, &index_needs_free);

    node_index = agtv_index.val.int_value;
    if (index_needs_free)
    {
        pfree_agtype_value_content(&agtv_index);
    }

    Assert(AGT_ROOT_IS_BINARY(agt_arg_vpc));
    Assert(AGT_ROOT_BINARY_FLAGS(agt_arg_vpc) == AGT_FBINARY_TYPE_VLE_PATH);

    vpc = (VLE_path_container *)agt_arg_vpc;
    node_count = get_vle_container_node_count(vpc);
    if (!normalize_vle_container_index(node_count, &node_index))
    {
        PG_RETURN_NULL();
    }

    graphid_array = GET_GRAPHID_ARRAY_FROM_CONTAINER(vpc);
    node_id = graphid_array[node_index * 2];

    ggctx = find_GRAPH_global_context(vpc->graph_oid);
    Assert(ggctx != NULL);
    ve = get_vertex_entry(ggctx, node_id);
    Assert(ve != NULL);

    label_name = get_vertex_entry_label_name(ve);
    agtv_label.type = AGTV_STRING;
    agtv_label.val.string.val = label_name;
    agtv_label.val.string.len = strlen(label_name);

    MemSet(&agis_result, 0, sizeof(agtype_in_state));
    agis_result.res = push_agtype_value(&agis_result.parse_state,
                                        WAGT_BEGIN_ARRAY, NULL);
    agis_result.res = push_agtype_value(&agis_result.parse_state, WAGT_ELEM,
                                        &agtv_label);
    agis_result.res = push_agtype_value(&agis_result.parse_state,
                                        WAGT_END_ARRAY, NULL);

    PG_RETURN_POINTER(agtype_value_to_agtype(agis_result.res));
}

PG_FUNCTION_INFO_V1(age_vle_node_properties_at);

Datum age_vle_node_properties_at(PG_FUNCTION_ARGS)
{
    GRAPH_global_context *ggctx = NULL;
    VLE_path_container *vpc = NULL;
    vertex_entry *ve = NULL;
    agtype *agt_arg_vpc = NULL;
    agtype_value agtv_index;
    bool index_needs_free = false;
    graphid *graphid_array = NULL;
    graphid node_id;
    int64 node_count;
    int64 node_index;

    if (PG_ARGISNULL(0) || PG_ARGISNULL(1))
    {
        PG_RETURN_NULL();
    }

    agt_arg_vpc = AG_GET_ARG_AGTYPE_P(0);
    if (is_agtype_null(agt_arg_vpc))
    {
        PG_RETURN_NULL();
    }

    get_vle_scalar_arg_no_copy("age_vle_node_properties_at",
                               AG_GET_ARG_AGTYPE_P(1), AGTV_INTEGER, true,
                               &agtv_index, &index_needs_free);

    node_index = agtv_index.val.int_value;
    if (index_needs_free)
    {
        pfree_agtype_value_content(&agtv_index);
    }

    Assert(AGT_ROOT_IS_BINARY(agt_arg_vpc));
    Assert(AGT_ROOT_BINARY_FLAGS(agt_arg_vpc) == AGT_FBINARY_TYPE_VLE_PATH);

    vpc = (VLE_path_container *)agt_arg_vpc;
    node_count = get_vle_container_node_count(vpc);
    if (!normalize_vle_container_index(node_count, &node_index))
    {
        PG_RETURN_NULL();
    }

    graphid_array = GET_GRAPHID_ARRAY_FROM_CONTAINER(vpc);
    node_id = graphid_array[node_index * 2];

    ggctx = find_GRAPH_global_context(vpc->graph_oid);
    Assert(ggctx != NULL);
    ve = get_vertex_entry(ggctx, node_id);
    Assert(ve != NULL);

    PG_RETURN_DATUM(get_vertex_entry_properties(ve));
}

PG_FUNCTION_INFO_V1(age_vle_node_property_at);

Datum age_vle_node_property_at(PG_FUNCTION_ARGS)
{
    GRAPH_global_context *ggctx = NULL;
    VLE_path_container *vpc = NULL;
    agtype *agt_arg_vpc = NULL;
    agtype_value agtv_index;
    agtype_value key_value;
    VLEIndexedPropertyLookup lookup;
    Datum property = (Datum) 0;
    bool index_needs_free = false;
    bool key_needs_free = false;
    graphid *graphid_array = NULL;
    graphid node_id;
    int64 node_count;
    int64 node_index;
    bool found;

    if (PG_ARGISNULL(0) || PG_ARGISNULL(1) || PG_ARGISNULL(2))
    {
        PG_RETURN_NULL();
    }

    agt_arg_vpc = AG_GET_ARG_AGTYPE_P(0);
    if (is_agtype_null(agt_arg_vpc))
    {
        PG_RETURN_NULL();
    }

    get_vle_scalar_arg_no_copy("age_vle_node_property_at",
                               AG_GET_ARG_AGTYPE_P(1), AGTV_INTEGER, true,
                               &agtv_index, &index_needs_free);
    get_vle_terminal_property_key(fcinfo, "age_vle_node_property_at",
                                  AG_GET_ARG_AGTYPE_P(2), &key_value,
                                  &key_needs_free);
    if (!validate_vle_property_key(&key_value, key_needs_free))
    {
        if (index_needs_free)
            pfree_agtype_value_content(&agtv_index);
        PG_RETURN_NULL();
    }

    node_index = agtv_index.val.int_value;
    if (index_needs_free)
    {
        pfree_agtype_value_content(&agtv_index);
    }

    Assert(AGT_ROOT_IS_BINARY(agt_arg_vpc));
    Assert(AGT_ROOT_BINARY_FLAGS(agt_arg_vpc) == AGT_FBINARY_TYPE_VLE_PATH);

    vpc = (VLE_path_container *)agt_arg_vpc;
    node_count = get_vle_container_node_count(vpc);
    if (!normalize_vle_container_index(node_count, &node_index))
    {
        if (key_needs_free)
            pfree_agtype_value_content(&key_value);
        PG_RETURN_NULL();
    }

    graphid_array = GET_GRAPHID_ARRAY_FROM_CONTAINER(vpc);
    node_id = graphid_array[node_index * 2];

    ggctx = find_GRAPH_global_context(vpc->graph_oid);
    Assert(ggctx != NULL);
    init_vle_indexed_property_lookup(&lookup, ggctx, vpc, node_id,
                                     &key_value,
                                     VLE_INDEXED_PROPERTY_VERTEX);
    found = get_vle_indexed_property_from_lookup(fcinfo, &lookup, &property);

    if (key_needs_free)
        pfree_agtype_value_content(&key_value);

    if (!found)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(property);
}

PG_FUNCTION_INFO_V1(age_materialize_vle_nodes_slice);

Datum age_materialize_vle_nodes_slice(PG_FUNCTION_ARGS)
{
    agtype *agt_arg_vpc = NULL;
    agtype *agt_lidx = NULL;
    agtype *agt_uidx = NULL;
    agtype_value lidx_value;
    agtype_value uidx_value;
    agtype_value *lidx_value_ptr = NULL;
    agtype_value *uidx_value_ptr = NULL;
    agtype_value *agtv_array = NULL;
    bool lidx_needs_free = false;
    bool uidx_needs_free = false;
    int64 node_count;
    int64 lower_index = 0;
    int64 upper_index = 0;

    if (PG_ARGISNULL(0))
    {
        PG_RETURN_NULL();
    }

    if (PG_ARGISNULL(1) && PG_ARGISNULL(2))
    {
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("slice start and/or end is required")));
    }

    agt_arg_vpc = AG_GET_ARG_AGTYPE_P(0);
    if (is_agtype_null(agt_arg_vpc))
    {
        PG_RETURN_NULL();
    }

    node_count = agtv_vle_node_count(agt_arg_vpc);
    if (PG_ARGISNULL(1))
    {
        lower_index = 0;
    }
    else
    {
        agt_lidx = AG_GET_ARG_AGTYPE_P(1);
        get_vle_scalar_arg_no_copy("age_materialize_vle_nodes_slice",
                                   agt_lidx, AGTV_INTEGER, false,
                                   &lidx_value, &lidx_needs_free);
        lidx_value_ptr = &lidx_value;
        if (lidx_value_ptr->type == AGTV_NULL)
        {
            if (lidx_needs_free)
            {
                pfree_agtype_value_content(lidx_value_ptr);
            }
            PG_RETURN_NULL();
        }
    }

    if (PG_ARGISNULL(2))
    {
        upper_index = node_count;
    }
    else
    {
        agt_uidx = AG_GET_ARG_AGTYPE_P(2);
        get_vle_scalar_arg_no_copy("age_materialize_vle_nodes_slice",
                                   agt_uidx, AGTV_INTEGER, false,
                                   &uidx_value, &uidx_needs_free);
        uidx_value_ptr = &uidx_value;
        if (uidx_value_ptr->type == AGTV_NULL)
        {
            if (lidx_value_ptr != NULL && lidx_needs_free)
            {
                pfree_agtype_value_content(lidx_value_ptr);
            }
            if (uidx_needs_free)
            {
                pfree_agtype_value_content(uidx_value_ptr);
            }
            PG_RETURN_NULL();
        }
    }

    if ((lidx_value_ptr != NULL && lidx_value_ptr->type != AGTV_INTEGER) ||
        (uidx_value_ptr != NULL && uidx_value_ptr->type != AGTV_INTEGER))
    {
        if (lidx_value_ptr != NULL && lidx_needs_free)
        {
            pfree_agtype_value_content(lidx_value_ptr);
        }
        if (uidx_value_ptr != NULL && uidx_needs_free)
        {
            pfree_agtype_value_content(uidx_value_ptr);
        }
        ereport(ERROR,
                (errmsg("array slices must resolve to an integer value")));
    }

    if (lidx_value_ptr != NULL)
    {
        lower_index = lidx_value_ptr->val.int_value;
        if (lidx_needs_free)
        {
            pfree_agtype_value_content(lidx_value_ptr);
        }
    }
    if (uidx_value_ptr != NULL)
    {
        upper_index = uidx_value_ptr->val.int_value;
        if (uidx_needs_free)
        {
            pfree_agtype_value_content(uidx_value_ptr);
        }
    }

    if (lower_index < 0)
    {
        lower_index = node_count + lower_index;
    }
    if (lower_index < 0)
    {
        lower_index = 0;
    }
    if (lower_index > node_count)
    {
        lower_index = node_count;
    }
    if (upper_index < 0)
    {
        upper_index = node_count + upper_index;
    }
    if (upper_index < 0)
    {
        upper_index = 0;
    }
    if (upper_index > node_count)
    {
        upper_index = node_count;
    }

    if (upper_index <= lower_index)
    {
        PG_FREE_IF_COPY(agt_arg_vpc, 0);
        PG_FREE_IF_COPY(agt_lidx, 1);
        PG_FREE_IF_COPY(agt_uidx, 2);
        PG_RETURN_POINTER(build_empty_agtype_array());
    }

    agtv_array = agtv_materialize_vle_nodes_slice(
        agt_arg_vpc, lower_index, upper_index);

    PG_FREE_IF_COPY(agt_arg_vpc, 0);
    PG_FREE_IF_COPY(agt_lidx, 1);
    PG_FREE_IF_COPY(agt_uidx, 2);

    PG_RETURN_POINTER(agtype_value_to_agtype(agtv_array));
}

PG_FUNCTION_INFO_V1(age_materialize_vle_list_slice);

Datum age_materialize_vle_list_slice(PG_FUNCTION_ARGS)
{
    agtype *agt_arg_vpc = NULL;
    agtype *agt_lidx = NULL;
    agtype *agt_uidx = NULL;
    agtype_value lidx_value;
    agtype_value uidx_value;
    agtype_value mode_value;
    agtype_value *agtv_array = NULL;
    bool lidx_needs_free = false;
    bool uidx_needs_free = false;
    bool mode_needs_free = false;
    bool reverse = false;
    bool node_list = false;
    int64 edge_count;
    int64 list_count;
    int64 lower_index = 0;
    int64 upper_index = 0;
    int64 base_index = 0;
    int64 reverse_base_index = 0;
    int64 mode;

    if (PG_ARGISNULL(0) || PG_ARGISNULL(3))
    {
        PG_RETURN_NULL();
    }

    if (PG_ARGISNULL(1) && PG_ARGISNULL(2))
    {
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("slice start and/or end is required")));
    }

    agt_arg_vpc = AG_GET_ARG_AGTYPE_P(0);
    if (is_agtype_null(agt_arg_vpc))
    {
        PG_RETURN_NULL();
    }

    get_vle_scalar_arg_no_copy("age_materialize_vle_list_slice",
                               AG_GET_ARG_AGTYPE_P(3), AGTV_INTEGER, true,
                               &mode_value, &mode_needs_free);
    mode = mode_value.val.int_value;
    if (mode_needs_free)
    {
        pfree_agtype_value_content(&mode_value);
    }

    edge_count = agtv_vle_edge_count(agt_arg_vpc);
    switch (mode)
    {
    case 0:
        list_count = edge_count;
        break;
    case 1:
        node_list = true;
        list_count = edge_count + 1;
        break;
    case 2:
        list_count = Max(edge_count - 1, 0);
        base_index = 1;
        break;
    case 3:
        node_list = true;
        list_count = edge_count;
        base_index = 1;
        break;
    case 4:
        reverse = true;
        list_count = edge_count;
        break;
    case 5:
        reverse = true;
        node_list = true;
        list_count = edge_count + 1;
        break;
    case 6:
        list_count = Max(edge_count - 2, 0);
        base_index = 2;
        break;
    case 7:
        node_list = true;
        list_count = Max(edge_count - 1, 0);
        base_index = 2;
        break;
    case 8:
        reverse = true;
        list_count = Max(edge_count - 1, 0);
        reverse_base_index = 1;
        break;
    case 9:
        reverse = true;
        node_list = true;
        list_count = edge_count;
        reverse_base_index = 1;
        break;
    case 10:
        reverse = true;
        list_count = Max(edge_count - 1, 0);
        break;
    case 11:
        reverse = true;
        node_list = true;
        list_count = edge_count;
        break;
    default:
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("age_materialize_vle_list_slice: invalid mode")));
    }

    if (PG_ARGISNULL(1))
    {
        lower_index = 0;
    }
    else
    {
        agt_lidx = AG_GET_ARG_AGTYPE_P(1);
        get_vle_scalar_arg_no_copy("age_materialize_vle_list_slice",
                                   agt_lidx, AGTV_INTEGER, false,
                                   &lidx_value, &lidx_needs_free);
        if (lidx_value.type == AGTV_NULL)
        {
            if (lidx_needs_free)
            {
                pfree_agtype_value_content(&lidx_value);
            }
            PG_RETURN_NULL();
        }
        if (lidx_value.type != AGTV_INTEGER)
        {
            if (lidx_needs_free)
            {
                pfree_agtype_value_content(&lidx_value);
            }
            ereport(ERROR,
                    (errmsg("array slices must resolve to an integer value")));
        }
        lower_index = lidx_value.val.int_value;
        if (lidx_needs_free)
        {
            pfree_agtype_value_content(&lidx_value);
        }
    }

    if (PG_ARGISNULL(2))
    {
        upper_index = list_count;
    }
    else
    {
        agt_uidx = AG_GET_ARG_AGTYPE_P(2);
        get_vle_scalar_arg_no_copy("age_materialize_vle_list_slice",
                                   agt_uidx, AGTV_INTEGER, false,
                                   &uidx_value, &uidx_needs_free);
        if (uidx_value.type == AGTV_NULL)
        {
            if (uidx_needs_free)
            {
                pfree_agtype_value_content(&uidx_value);
            }
            PG_RETURN_NULL();
        }
        if (uidx_value.type != AGTV_INTEGER)
        {
            if (uidx_needs_free)
            {
                pfree_agtype_value_content(&uidx_value);
            }
            ereport(ERROR,
                    (errmsg("array slices must resolve to an integer value")));
        }
        upper_index = uidx_value.val.int_value;
        if (uidx_needs_free)
        {
            pfree_agtype_value_content(&uidx_value);
        }
    }

    if (lower_index < 0)
    {
        lower_index = list_count + lower_index;
    }
    if (lower_index < 0)
    {
        lower_index = 0;
    }
    if (lower_index > list_count)
    {
        lower_index = list_count;
    }
    if (upper_index < 0)
    {
        upper_index = list_count + upper_index;
    }
    if (upper_index < 0)
    {
        upper_index = 0;
    }
    if (upper_index > list_count)
    {
        upper_index = list_count;
    }

    if (upper_index <= lower_index)
    {
        PG_RETURN_POINTER(build_empty_agtype_array());
    }

    if (reverse)
    {
        lower_index += reverse_base_index;
        upper_index += reverse_base_index;
        agtv_array = node_list ?
            agtv_materialize_vle_nodes_reversed_slice(agt_arg_vpc,
                                                      lower_index,
                                                      upper_index) :
            agtv_materialize_vle_edges_reversed_slice(agt_arg_vpc,
                                                      lower_index,
                                                      upper_index);
    }
    else
    {
        lower_index += base_index;
        upper_index += base_index;
        agtv_array = node_list ?
            agtv_materialize_vle_nodes_slice(agt_arg_vpc, lower_index,
                                             upper_index) :
            agtv_materialize_vle_edges_slice(agt_arg_vpc, lower_index,
                                             upper_index);
    }

    PG_RETURN_POINTER(agtype_value_to_agtype(agtv_array));
}

PG_FUNCTION_INFO_V1(age_materialize_vle_nodes_tail);

Datum age_materialize_vle_nodes_tail(PG_FUNCTION_ARGS)
{
    agtype *agt_arg_vpc = NULL;
    agtype_value *agtv_array = NULL;
    int64 node_count;

    if (PG_ARGISNULL(0))
    {
        PG_RETURN_NULL();
    }

    agt_arg_vpc = AG_GET_ARG_AGTYPE_P(0);
    if (is_agtype_null(agt_arg_vpc))
    {
        PG_RETURN_NULL();
    }

    node_count = agtv_vle_node_count(agt_arg_vpc);
    if (node_count <= 1)
    {
        PG_RETURN_POINTER(build_empty_agtype_array());
    }

    agtv_array = agtv_materialize_vle_nodes_slice(
        agt_arg_vpc, Min(node_count, 1), node_count);

    PG_RETURN_POINTER(agtype_value_to_agtype(agtv_array));
}

PG_FUNCTION_INFO_V1(age_materialize_vle_nodes_reversed);

Datum age_materialize_vle_nodes_reversed(PG_FUNCTION_ARGS)
{
    agtype *agt_arg_vpc = NULL;
    agtype_value *agtv_array = NULL;

    if (PG_ARGISNULL(0))
    {
        PG_RETURN_NULL();
    }

    agt_arg_vpc = AG_GET_ARG_AGTYPE_P(0);
    if (is_agtype_null(agt_arg_vpc))
    {
        PG_RETURN_NULL();
    }

    agtv_array = agtv_materialize_vle_nodes_reversed(agt_arg_vpc);

    PG_RETURN_POINTER(agtype_value_to_agtype(agtv_array));
}

PG_FUNCTION_INFO_V1(age_materialize_vle_edge_at);

Datum age_materialize_vle_edge_at(PG_FUNCTION_ARGS)
{
    agtype *agt_arg_vpc = NULL;
    agtype_value agtv_index;
    agtype *edge = NULL;
    VLEMaterializerObjectCache *object_cache;
    VLE_path_container *vpc;
    bool index_needs_free = false;
    int64 edge_index;

    if (PG_ARGISNULL(0) || PG_ARGISNULL(1))
    {
        PG_RETURN_NULL();
    }

    agt_arg_vpc = AG_GET_ARG_AGTYPE_P(0);
    if (is_agtype_null(agt_arg_vpc))
    {
        PG_RETURN_NULL();
    }
    Assert(AGT_ROOT_IS_BINARY(agt_arg_vpc));
    Assert(AGT_ROOT_BINARY_FLAGS(agt_arg_vpc) == AGT_FBINARY_TYPE_VLE_PATH);
    vpc = (VLE_path_container *)agt_arg_vpc;
    object_cache = age_vle_materializer_object_cache_get(fcinfo, vpc->graph_oid);

    get_vle_scalar_arg_no_copy("age_materialize_vle_edge_at",
                               AG_GET_ARG_AGTYPE_P(1), AGTV_INTEGER, true,
                               &agtv_index, &index_needs_free);

    edge_index = agtv_index.val.int_value;
    if (index_needs_free)
    {
        pfree_agtype_value_content(&agtv_index);
    }

    edge = materialize_vle_edge_at_agtype(agt_arg_vpc, edge_index,
                                          object_cache);
    if (edge == NULL)
    {
        PG_RETURN_NULL();
    }

    PG_RETURN_POINTER(edge);
}

PG_FUNCTION_INFO_V1(age_materialize_vle_edge_reversed_at);

Datum age_materialize_vle_edge_reversed_at(PG_FUNCTION_ARGS)
{
    agtype *agt_arg_vpc = NULL;
    agtype_value agtv_index;
    agtype *edge = NULL;
    VLEMaterializerObjectCache *object_cache;
    VLE_path_container *vpc;
    bool index_needs_free = false;
    int64 edge_index;

    if (PG_ARGISNULL(0) || PG_ARGISNULL(1))
    {
        PG_RETURN_NULL();
    }

    agt_arg_vpc = AG_GET_ARG_AGTYPE_P(0);
    if (is_agtype_null(agt_arg_vpc))
    {
        PG_RETURN_NULL();
    }
    Assert(AGT_ROOT_IS_BINARY(agt_arg_vpc));
    Assert(AGT_ROOT_BINARY_FLAGS(agt_arg_vpc) == AGT_FBINARY_TYPE_VLE_PATH);
    vpc = (VLE_path_container *)agt_arg_vpc;
    object_cache = age_vle_materializer_object_cache_get(fcinfo, vpc->graph_oid);

    get_vle_scalar_arg_no_copy("age_materialize_vle_edge_reversed_at",
                               AG_GET_ARG_AGTYPE_P(1), AGTV_INTEGER, true,
                               &agtv_index, &index_needs_free);

    edge_index = agtv_index.val.int_value;
    if (index_needs_free)
    {
        pfree_agtype_value_content(&agtv_index);
    }

    if (edge_index == PG_INT64_MIN)
    {
        PG_RETURN_NULL();
    }

    edge = materialize_vle_edge_at_agtype(agt_arg_vpc, -edge_index - 1,
                                          object_cache);
    if (edge == NULL)
    {
        PG_RETURN_NULL();
    }

    PG_RETURN_POINTER(edge);
}

PG_FUNCTION_INFO_V1(age_materialize_vle_edge_tail_last);

Datum age_materialize_vle_edge_tail_last(PG_FUNCTION_ARGS)
{
    GRAPH_global_context *ggctx = NULL;
    VLE_path_container *vpc = NULL;
    agtype *agt_arg_vpc = NULL;
    agtype_value *agtv_edge = NULL;
    graphid *graphid_array = NULL;
    int64 edge_count;

    if (PG_ARGISNULL(0))
    {
        PG_RETURN_NULL();
    }

    agt_arg_vpc = AG_GET_ARG_AGTYPE_P(0);
    if (is_agtype_null(agt_arg_vpc))
    {
        PG_RETURN_NULL();
    }

    Assert(AGT_ROOT_IS_BINARY(agt_arg_vpc));
    Assert(AGT_ROOT_BINARY_FLAGS(agt_arg_vpc) == AGT_FBINARY_TYPE_VLE_PATH);

    vpc = (VLE_path_container *)agt_arg_vpc;
    edge_count = agtv_vle_edge_count(agt_arg_vpc);
    if (edge_count <= 1)
    {
        PG_RETURN_NULL();
    }

    ggctx = find_GRAPH_global_context(vpc->graph_oid);
    Assert(ggctx != NULL);
    graphid_array = GET_GRAPHID_ARRAY_FROM_CONTAINER(vpc);
    agtv_edge = build_vle_edge_value(ggctx, graphid_array[edge_count * 2 - 1],
                                     NULL);

    PG_RETURN_POINTER(agtype_value_to_agtype(agtv_edge));
}

PG_FUNCTION_INFO_V1(age_vle_edge_tail_last_id);

Datum age_vle_edge_tail_last_id(PG_FUNCTION_ARGS)
{
    VLE_path_container *vpc = NULL;
    agtype *agt_arg_vpc = NULL;
    agtype_value agtv_result;
    graphid *graphid_array = NULL;
    int64 edge_count;

    if (PG_ARGISNULL(0))
    {
        PG_RETURN_NULL();
    }

    agt_arg_vpc = AG_GET_ARG_AGTYPE_P(0);
    if (is_agtype_null(agt_arg_vpc))
    {
        PG_RETURN_NULL();
    }

    Assert(AGT_ROOT_IS_BINARY(agt_arg_vpc));
    Assert(AGT_ROOT_BINARY_FLAGS(agt_arg_vpc) == AGT_FBINARY_TYPE_VLE_PATH);

    vpc = (VLE_path_container *)agt_arg_vpc;
    edge_count = agtv_vle_edge_count(agt_arg_vpc);
    if (edge_count <= 1)
    {
        PG_RETURN_NULL();
    }

    graphid_array = GET_GRAPHID_ARRAY_FROM_CONTAINER(vpc);
    agtv_result.type = AGTV_INTEGER;
    agtv_result.val.int_value = graphid_array[edge_count * 2 - 1];

    PG_RETURN_POINTER(agtype_value_to_agtype(&agtv_result));
}

PG_FUNCTION_INFO_V1(age_vle_tail_last_field);

Datum age_vle_tail_last_field(PG_FUNCTION_ARGS)
{
    GRAPH_global_context *ggctx = NULL;
    VLE_path_container *vpc = NULL;
    vertex_entry *ve = NULL;
    edge_entry *ee = NULL;
    agtype *agt_arg_vpc = NULL;
    agtype_value agtv_mode;
    agtype_value agtv_result;
    agtype_value agtv_label;
    agtype_in_state agis_result;
    bool mode_needs_free = false;
    graphid *graphid_array = NULL;
    graphid entity_id;
    int64 node_count;
    int64 edge_count;
    int64 mode;
    char *label_name = NULL;

    if (PG_ARGISNULL(0) || PG_ARGISNULL(1))
    {
        PG_RETURN_NULL();
    }

    agt_arg_vpc = AG_GET_ARG_AGTYPE_P(0);
    if (is_agtype_null(agt_arg_vpc))
    {
        PG_RETURN_NULL();
    }

    get_vle_scalar_arg_no_copy("age_vle_tail_last_field",
                               AG_GET_ARG_AGTYPE_P(1), AGTV_INTEGER, true,
                               &agtv_mode, &mode_needs_free);
    mode = agtv_mode.val.int_value;
    if (mode_needs_free)
    {
        pfree_agtype_value_content(&agtv_mode);
    }

    Assert(AGT_ROOT_IS_BINARY(agt_arg_vpc));
    Assert(AGT_ROOT_BINARY_FLAGS(agt_arg_vpc) == AGT_FBINARY_TYPE_VLE_PATH);

    vpc = (VLE_path_container *)agt_arg_vpc;
    graphid_array = GET_GRAPHID_ARRAY_FROM_CONTAINER(vpc);

    if (mode >= 0 && mode <= 2)
    {
        node_count = agtv_vle_node_count(agt_arg_vpc);
        if (node_count <= 1)
        {
            PG_RETURN_NULL();
        }

        entity_id = graphid_array[(node_count - 1) * 2];
        ggctx = find_GRAPH_global_context(vpc->graph_oid);
        Assert(ggctx != NULL);
        ve = get_vertex_entry(ggctx, entity_id);
        Assert(ve != NULL);

        if (mode == 2)
        {
            PG_RETURN_DATUM(get_vertex_entry_properties(ve));
        }

        label_name = get_vertex_entry_label_name(ve);
        agtv_label.type = AGTV_STRING;
        agtv_label.val.string.val = label_name;
        agtv_label.val.string.len = strlen(label_name);

        if (mode == 0)
        {
            PG_RETURN_POINTER(agtype_value_to_agtype(&agtv_label));
        }

        MemSet(&agis_result, 0, sizeof(agtype_in_state));
        agis_result.res = push_agtype_value(&agis_result.parse_state,
                                            WAGT_BEGIN_ARRAY, NULL);
        agis_result.res = push_agtype_value(&agis_result.parse_state,
                                            WAGT_ELEM, &agtv_label);
        agis_result.res = push_agtype_value(&agis_result.parse_state,
                                            WAGT_END_ARRAY, NULL);

        PG_RETURN_POINTER(agtype_value_to_agtype(agis_result.res));
    }

    edge_count = agtv_vle_edge_count(agt_arg_vpc);
    if (edge_count <= 1)
    {
        PG_RETURN_NULL();
    }

    if (mode < 3 || mode > 4)
    {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("age_vle_tail_last_field: invalid mode")));
    }

    entity_id = graphid_array[edge_count * 2 - 1];
    ggctx = find_GRAPH_global_context(vpc->graph_oid);
    Assert(ggctx != NULL);
    ee = get_edge_entry(ggctx, entity_id);
    Assert(ee != NULL);

    if (mode == 3)
    {
        label_name = get_edge_entry_label_name(ee);
        agtv_result.type = AGTV_STRING;
        agtv_result.val.string.val = label_name;
        agtv_result.val.string.len = strlen(label_name);

        PG_RETURN_POINTER(agtype_value_to_agtype(&agtv_result));
    }
    else if (mode == 4)
    {
        PG_RETURN_DATUM(get_edge_entry_properties(ee));
    }

    Assert(false);
    PG_RETURN_NULL();
}

PG_FUNCTION_INFO_V1(age_vle_tail_last_edge_endpoint);

Datum age_vle_tail_last_edge_endpoint(PG_FUNCTION_ARGS)
{
    GRAPH_global_context *ggctx = NULL;
    VLE_path_container *vpc = NULL;
    edge_entry *ee = NULL;
    agtype *agt_arg_vpc = NULL;
    agtype_value agtv_mode;
    agtype_value agtv_result;
    agtype_value *agtv_vertex = NULL;
    bool mode_needs_free = false;
    graphid *graphid_array = NULL;
    graphid edge_id;
    graphid endpoint_id;
    int64 edge_count;
    int64 mode;

    if (PG_ARGISNULL(0) || PG_ARGISNULL(1))
    {
        PG_RETURN_NULL();
    }

    agt_arg_vpc = AG_GET_ARG_AGTYPE_P(0);
    if (is_agtype_null(agt_arg_vpc))
    {
        PG_RETURN_NULL();
    }

    get_vle_scalar_arg_no_copy("age_vle_tail_last_edge_endpoint",
                               AG_GET_ARG_AGTYPE_P(1), AGTV_INTEGER, true,
                               &agtv_mode, &mode_needs_free);
    mode = agtv_mode.val.int_value;
    if (mode_needs_free)
    {
        pfree_agtype_value_content(&agtv_mode);
    }

    Assert(AGT_ROOT_IS_BINARY(agt_arg_vpc));
    Assert(AGT_ROOT_BINARY_FLAGS(agt_arg_vpc) == AGT_FBINARY_TYPE_VLE_PATH);

    vpc = (VLE_path_container *)agt_arg_vpc;
    edge_count = agtv_vle_edge_count(agt_arg_vpc);
    if (edge_count <= 1)
    {
        PG_RETURN_NULL();
    }

    graphid_array = GET_GRAPHID_ARRAY_FROM_CONTAINER(vpc);
    edge_id = graphid_array[edge_count * 2 - 1];

    if (mode < 0 || mode > 3)
    {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("age_vle_tail_last_edge_endpoint: invalid mode")));
    }

    ggctx = find_GRAPH_global_context(vpc->graph_oid);
    Assert(ggctx != NULL);
    ee = get_edge_entry(ggctx, edge_id);
    Assert(ee != NULL);

    endpoint_id = (mode == 0 || mode == 2) ?
        get_edge_entry_start_vertex_id(ee) :
        get_edge_entry_end_vertex_id(ee);

    if (mode == 0 || mode == 1)
    {
        agtv_vertex = build_vle_vertex_value(ggctx, endpoint_id, NULL);

        PG_RETURN_POINTER(agtype_value_to_agtype(agtv_vertex));
    }
    else if (mode == 2 || mode == 3)
    {
        agtv_result.type = AGTV_INTEGER;
        agtv_result.val.int_value = endpoint_id;

        PG_RETURN_POINTER(agtype_value_to_agtype(&agtv_result));
    }

    Assert(false);
    PG_RETURN_NULL();
}

PG_FUNCTION_INFO_V1(age_vle_tail_last_endpoint_field);

Datum age_vle_tail_last_endpoint_field(PG_FUNCTION_ARGS)
{
    GRAPH_global_context *ggctx = NULL;
    VLE_path_container *vpc = NULL;
    edge_entry *ee = NULL;
    vertex_entry *ve = NULL;
    agtype *agt_arg_vpc = NULL;
    agtype_value agtv_mode;
    agtype_value agtv_label;
    agtype_in_state agis_result;
    bool mode_needs_free = false;
    graphid *graphid_array = NULL;
    graphid edge_id;
    graphid endpoint_id;
    int64 edge_count;
    int64 mode;
    char *label_name = NULL;

    if (PG_ARGISNULL(0) || PG_ARGISNULL(1))
    {
        PG_RETURN_NULL();
    }

    agt_arg_vpc = AG_GET_ARG_AGTYPE_P(0);
    if (is_agtype_null(agt_arg_vpc))
    {
        PG_RETURN_NULL();
    }

    get_vle_scalar_arg_no_copy("age_vle_tail_last_endpoint_field",
                               AG_GET_ARG_AGTYPE_P(1), AGTV_INTEGER, true,
                               &agtv_mode, &mode_needs_free);
    mode = agtv_mode.val.int_value;
    if (mode_needs_free)
    {
        pfree_agtype_value_content(&agtv_mode);
    }

    Assert(AGT_ROOT_IS_BINARY(agt_arg_vpc));
    Assert(AGT_ROOT_BINARY_FLAGS(agt_arg_vpc) == AGT_FBINARY_TYPE_VLE_PATH);

    vpc = (VLE_path_container *)agt_arg_vpc;
    edge_count = agtv_vle_edge_count(agt_arg_vpc);
    if (edge_count <= 1)
    {
        PG_RETURN_NULL();
    }

    graphid_array = GET_GRAPHID_ARRAY_FROM_CONTAINER(vpc);
    edge_id = graphid_array[edge_count * 2 - 1];

    if (mode < 0 || mode > 5)
    {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("age_vle_tail_last_endpoint_field: invalid mode")));
    }

    ggctx = find_GRAPH_global_context(vpc->graph_oid);
    Assert(ggctx != NULL);
    ee = get_edge_entry(ggctx, edge_id);
    Assert(ee != NULL);

    endpoint_id = (mode % 2 == 0) ?
        get_edge_entry_start_vertex_id(ee) :
        get_edge_entry_end_vertex_id(ee);
    ve = get_vertex_entry(ggctx, endpoint_id);
    Assert(ve != NULL);

    if (mode == 4 || mode == 5)
    {
        PG_RETURN_DATUM(get_vertex_entry_properties(ve));
    }

    label_name = get_vertex_entry_label_name(ve);
    agtv_label.type = AGTV_STRING;
    agtv_label.val.string.val = label_name;
    agtv_label.val.string.len = strlen(label_name);

    if (mode == 0 || mode == 1)
    {
        PG_RETURN_POINTER(agtype_value_to_agtype(&agtv_label));
    }
    else if (mode == 2 || mode == 3)
    {
        MemSet(&agis_result, 0, sizeof(agtype_in_state));
        agis_result.res = push_agtype_value(&agis_result.parse_state,
                                            WAGT_BEGIN_ARRAY, NULL);
        agis_result.res = push_agtype_value(&agis_result.parse_state,
                                            WAGT_ELEM, &agtv_label);
        agis_result.res = push_agtype_value(&agis_result.parse_state,
                                            WAGT_END_ARRAY, NULL);

        PG_RETURN_POINTER(agtype_value_to_agtype(agis_result.res));
    }

    Assert(false);
    PG_RETURN_NULL();
}

PG_FUNCTION_INFO_V1(age_vle_edge_id_at);

Datum age_vle_edge_id_at(PG_FUNCTION_ARGS)
{
    VLE_path_container *vpc = NULL;
    agtype *agt_arg_vpc = NULL;
    agtype_value agtv_index;
    agtype_value agtv_result;
    bool index_needs_free = false;
    graphid *graphid_array = NULL;
    int64 edge_count;
    int64 edge_index;

    if (PG_ARGISNULL(0) || PG_ARGISNULL(1))
    {
        PG_RETURN_NULL();
    }

    agt_arg_vpc = AG_GET_ARG_AGTYPE_P(0);
    if (is_agtype_null(agt_arg_vpc))
    {
        PG_RETURN_NULL();
    }

    get_vle_scalar_arg_no_copy("age_vle_edge_id_at",
                               AG_GET_ARG_AGTYPE_P(1), AGTV_INTEGER, true,
                               &agtv_index, &index_needs_free);

    edge_index = agtv_index.val.int_value;
    if (index_needs_free)
    {
        pfree_agtype_value_content(&agtv_index);
    }

    Assert(AGT_ROOT_IS_BINARY(agt_arg_vpc));
    Assert(AGT_ROOT_BINARY_FLAGS(agt_arg_vpc) == AGT_FBINARY_TYPE_VLE_PATH);

    vpc = (VLE_path_container *)agt_arg_vpc;
    edge_count = get_vle_container_edge_count(vpc);
    if (!normalize_vle_container_index(edge_count, &edge_index))
    {
        PG_RETURN_NULL();
    }

    graphid_array = GET_GRAPHID_ARRAY_FROM_CONTAINER(vpc);
    agtv_result.type = AGTV_INTEGER;
    agtv_result.val.int_value = graphid_array[(edge_index * 2) + 1];

    PG_RETURN_POINTER(agtype_value_to_agtype(&agtv_result));
}

PG_FUNCTION_INFO_V1(age_vle_edge_index_exists);

Datum age_vle_edge_index_exists(PG_FUNCTION_ARGS)
{
    agtype *agt_arg_vpc = NULL;
    agtype_value agtv_index;
    bool index_needs_free = false;
    int64 edge_index;
    graphid edge_id;

    if (PG_ARGISNULL(0) || PG_ARGISNULL(1))
    {
        PG_RETURN_NULL();
    }

    agt_arg_vpc = AG_GET_ARG_AGTYPE_P(0);
    if (is_agtype_null(agt_arg_vpc))
    {
        PG_RETURN_NULL();
    }

    get_vle_scalar_arg_no_copy("age_vle_edge_index_exists",
                               AG_GET_ARG_AGTYPE_P(1), AGTV_INTEGER, false,
                               &agtv_index, &index_needs_free);

    if (agtv_index.type == AGTV_NULL)
    {
        if (index_needs_free)
        {
            pfree_agtype_value_content(&agtv_index);
        }
        PG_RETURN_NULL();
    }
    if (agtv_index.type != AGTV_INTEGER)
    {
        if (index_needs_free)
        {
            pfree_agtype_value_content(&agtv_index);
        }
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("age_vle_edge_index_exists: index must resolve to an integer")));
    }

    edge_index = agtv_index.val.int_value;
    if (index_needs_free)
    {
        pfree_agtype_value_content(&agtv_index);
    }

    Assert(AGT_ROOT_IS_BINARY(agt_arg_vpc));
    Assert(AGT_ROOT_BINARY_FLAGS(agt_arg_vpc) == AGT_FBINARY_TYPE_VLE_PATH);

    if (!get_vle_edge_id_at_index(agt_arg_vpc, edge_index, &edge_id))
    {
        PG_RETURN_NULL();
    }

    PG_RETURN_DATUM(boolean_to_agtype(true));
}

PG_FUNCTION_INFO_V1(age_vle_edge_indices_equal);

Datum age_vle_edge_indices_equal(PG_FUNCTION_ARGS)
{
    agtype *agt_arg_vpc = NULL;
    agtype_value agtv_left_index;
    agtype_value agtv_right_index;
    bool left_index_needs_free = false;
    bool right_index_needs_free = false;
    graphid left_edge_id;
    graphid right_edge_id;
    bool result;

    if (PG_ARGISNULL(0) || PG_ARGISNULL(1) || PG_ARGISNULL(2))
    {
        PG_RETURN_NULL();
    }

    agt_arg_vpc = AG_GET_ARG_AGTYPE_P(0);
    if (is_agtype_null(agt_arg_vpc))
    {
        PG_RETURN_NULL();
    }

    get_vle_scalar_arg_no_copy("age_vle_edge_indices_equal",
                               AG_GET_ARG_AGTYPE_P(1), AGTV_INTEGER, false,
                               &agtv_left_index, &left_index_needs_free);
    get_vle_scalar_arg_no_copy("age_vle_edge_indices_equal",
                               AG_GET_ARG_AGTYPE_P(2), AGTV_INTEGER, false,
                               &agtv_right_index, &right_index_needs_free);

    if (agtv_left_index.type == AGTV_NULL ||
        agtv_right_index.type == AGTV_NULL)
    {
        if (left_index_needs_free)
        {
            pfree_agtype_value_content(&agtv_left_index);
        }
        if (right_index_needs_free)
        {
            pfree_agtype_value_content(&agtv_right_index);
        }
        PG_RETURN_NULL();
    }
    if (agtv_left_index.type != AGTV_INTEGER ||
        agtv_right_index.type != AGTV_INTEGER)
    {
        if (left_index_needs_free)
        {
            pfree_agtype_value_content(&agtv_left_index);
        }
        if (right_index_needs_free)
        {
            pfree_agtype_value_content(&agtv_right_index);
        }
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("age_vle_edge_indices_equal: indexes must resolve to integers")));
    }

    Assert(AGT_ROOT_IS_BINARY(agt_arg_vpc));
    Assert(AGT_ROOT_BINARY_FLAGS(agt_arg_vpc) == AGT_FBINARY_TYPE_VLE_PATH);

    if (!get_vle_edge_id_at_index(agt_arg_vpc,
                                  agtv_left_index.val.int_value,
                                  &left_edge_id) ||
        !get_vle_edge_id_at_index(agt_arg_vpc,
                                  agtv_right_index.val.int_value,
                                  &right_edge_id))
    {
        if (left_index_needs_free)
        {
            pfree_agtype_value_content(&agtv_left_index);
        }
        if (right_index_needs_free)
        {
            pfree_agtype_value_content(&agtv_right_index);
        }
        PG_RETURN_NULL();
    }

    result = left_edge_id == right_edge_id;

    if (left_index_needs_free)
    {
        pfree_agtype_value_content(&agtv_left_index);
    }
    if (right_index_needs_free)
    {
        pfree_agtype_value_content(&agtv_right_index);
    }

    PG_RETURN_DATUM(boolean_to_agtype(result));
}

PG_FUNCTION_INFO_V1(age_vle_edge_reversed_index_equal);

Datum age_vle_edge_reversed_index_equal(PG_FUNCTION_ARGS)
{
    VLE_path_container *vpc = NULL;
    agtype *agt_arg_vpc = NULL;
    agtype_value agtv_reversed_index;
    agtype_value agtv_normal_index;
    bool reversed_index_needs_free = false;
    bool normal_index_needs_free = false;
    graphid reversed_edge_id;
    graphid normal_edge_id;
    int64 edge_count;
    int64 reversed_index;
    bool result;

    if (PG_ARGISNULL(0) || PG_ARGISNULL(1) || PG_ARGISNULL(2))
    {
        PG_RETURN_NULL();
    }

    agt_arg_vpc = AG_GET_ARG_AGTYPE_P(0);
    if (is_agtype_null(agt_arg_vpc))
    {
        PG_RETURN_NULL();
    }

    get_vle_scalar_arg_no_copy("age_vle_edge_reversed_index_equal",
                               AG_GET_ARG_AGTYPE_P(1), AGTV_INTEGER, false,
                               &agtv_reversed_index,
                               &reversed_index_needs_free);
    get_vle_scalar_arg_no_copy("age_vle_edge_reversed_index_equal",
                               AG_GET_ARG_AGTYPE_P(2), AGTV_INTEGER, false,
                               &agtv_normal_index, &normal_index_needs_free);

    if (agtv_reversed_index.type == AGTV_NULL ||
        agtv_normal_index.type == AGTV_NULL)
    {
        if (reversed_index_needs_free)
        {
            pfree_agtype_value_content(&agtv_reversed_index);
        }
        if (normal_index_needs_free)
        {
            pfree_agtype_value_content(&agtv_normal_index);
        }
        PG_RETURN_NULL();
    }
    if (agtv_reversed_index.type != AGTV_INTEGER ||
        agtv_normal_index.type != AGTV_INTEGER)
    {
        if (reversed_index_needs_free)
        {
            pfree_agtype_value_content(&agtv_reversed_index);
        }
        if (normal_index_needs_free)
        {
            pfree_agtype_value_content(&agtv_normal_index);
        }
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("age_vle_edge_reversed_index_equal: indexes must resolve to integers")));
    }

    Assert(AGT_ROOT_IS_BINARY(agt_arg_vpc));
    Assert(AGT_ROOT_BINARY_FLAGS(agt_arg_vpc) == AGT_FBINARY_TYPE_VLE_PATH);

    vpc = (VLE_path_container *)agt_arg_vpc;
    edge_count = get_vle_container_edge_count(vpc);
    reversed_index = agtv_reversed_index.val.int_value;
    if (!normalize_vle_container_index(edge_count, &reversed_index) ||
        !get_vle_edge_id_at_index(agt_arg_vpc,
                                  edge_count - reversed_index - 1,
                                  &reversed_edge_id) ||
        !get_vle_edge_id_at_index(agt_arg_vpc,
                                  agtv_normal_index.val.int_value,
                                  &normal_edge_id))
    {
        if (reversed_index_needs_free)
        {
            pfree_agtype_value_content(&agtv_reversed_index);
        }
        if (normal_index_needs_free)
        {
            pfree_agtype_value_content(&agtv_normal_index);
        }
        PG_RETURN_NULL();
    }

    result = reversed_edge_id == normal_edge_id;

    if (reversed_index_needs_free)
    {
        pfree_agtype_value_content(&agtv_reversed_index);
    }
    if (normal_index_needs_free)
    {
        pfree_agtype_value_content(&agtv_normal_index);
    }

    PG_RETURN_DATUM(boolean_to_agtype(result));
}

PG_FUNCTION_INFO_V1(age_vle_edge_label_at);

Datum age_vle_edge_label_at(PG_FUNCTION_ARGS)
{
    GRAPH_global_context *ggctx = NULL;
    VLE_path_container *vpc = NULL;
    edge_entry *ee = NULL;
    agtype *agt_arg_vpc = NULL;
    agtype_value agtv_index;
    agtype_value agtv_result;
    bool index_needs_free = false;
    graphid *graphid_array = NULL;
    graphid edge_id;
    int64 edge_count;
    int64 edge_index;
    char *label_name = NULL;

    if (PG_ARGISNULL(0) || PG_ARGISNULL(1))
    {
        PG_RETURN_NULL();
    }

    agt_arg_vpc = AG_GET_ARG_AGTYPE_P(0);
    if (is_agtype_null(agt_arg_vpc))
    {
        PG_RETURN_NULL();
    }

    get_vle_scalar_arg_no_copy("age_vle_edge_label_at",
                               AG_GET_ARG_AGTYPE_P(1), AGTV_INTEGER, true,
                               &agtv_index, &index_needs_free);

    edge_index = agtv_index.val.int_value;
    if (index_needs_free)
    {
        pfree_agtype_value_content(&agtv_index);
    }

    Assert(AGT_ROOT_IS_BINARY(agt_arg_vpc));
    Assert(AGT_ROOT_BINARY_FLAGS(agt_arg_vpc) == AGT_FBINARY_TYPE_VLE_PATH);

    vpc = (VLE_path_container *)agt_arg_vpc;
    edge_count = get_vle_container_edge_count(vpc);
    if (!normalize_vle_container_index(edge_count, &edge_index))
    {
        PG_RETURN_NULL();
    }

    graphid_array = GET_GRAPHID_ARRAY_FROM_CONTAINER(vpc);
    edge_id = graphid_array[(edge_index * 2) + 1];

    ggctx = find_GRAPH_global_context(vpc->graph_oid);
    Assert(ggctx != NULL);
    ee = get_edge_entry(ggctx, edge_id);
    Assert(ee != NULL);

    label_name = get_edge_entry_label_name(ee);
    agtv_result.type = AGTV_STRING;
    agtv_result.val.string.val = label_name;
    agtv_result.val.string.len = strlen(label_name);

    PG_RETURN_POINTER(agtype_value_to_agtype(&agtv_result));
}

PG_FUNCTION_INFO_V1(age_vle_edge_properties_at);

Datum age_vle_edge_properties_at(PG_FUNCTION_ARGS)
{
    GRAPH_global_context *ggctx = NULL;
    VLE_path_container *vpc = NULL;
    edge_entry *ee = NULL;
    agtype *agt_arg_vpc = NULL;
    agtype_value agtv_index;
    bool index_needs_free = false;
    graphid *graphid_array = NULL;
    graphid edge_id;
    int64 edge_count;
    int64 edge_index;

    if (PG_ARGISNULL(0) || PG_ARGISNULL(1))
    {
        PG_RETURN_NULL();
    }

    agt_arg_vpc = AG_GET_ARG_AGTYPE_P(0);
    if (is_agtype_null(agt_arg_vpc))
    {
        PG_RETURN_NULL();
    }

    get_vle_scalar_arg_no_copy("age_vle_edge_properties_at",
                               AG_GET_ARG_AGTYPE_P(1), AGTV_INTEGER, true,
                               &agtv_index, &index_needs_free);

    edge_index = agtv_index.val.int_value;
    if (index_needs_free)
    {
        pfree_agtype_value_content(&agtv_index);
    }

    Assert(AGT_ROOT_IS_BINARY(agt_arg_vpc));
    Assert(AGT_ROOT_BINARY_FLAGS(agt_arg_vpc) == AGT_FBINARY_TYPE_VLE_PATH);

    vpc = (VLE_path_container *)agt_arg_vpc;
    edge_count = get_vle_container_edge_count(vpc);
    if (!normalize_vle_container_index(edge_count, &edge_index))
    {
        PG_RETURN_NULL();
    }

    graphid_array = GET_GRAPHID_ARRAY_FROM_CONTAINER(vpc);
    edge_id = graphid_array[(edge_index * 2) + 1];

    ggctx = find_GRAPH_global_context(vpc->graph_oid);
    Assert(ggctx != NULL);
    ee = get_edge_entry(ggctx, edge_id);
    Assert(ee != NULL);

    PG_RETURN_DATUM(get_edge_entry_properties(ee));
}

PG_FUNCTION_INFO_V1(age_vle_edge_property_at);

Datum age_vle_edge_property_at(PG_FUNCTION_ARGS)
{
    GRAPH_global_context *ggctx = NULL;
    VLE_path_container *vpc = NULL;
    agtype *agt_arg_vpc = NULL;
    agtype_value agtv_index;
    agtype_value key_value;
    VLEIndexedPropertyLookup lookup;
    Datum property = (Datum) 0;
    bool index_needs_free = false;
    bool key_needs_free = false;
    graphid *graphid_array = NULL;
    graphid edge_id;
    int64 edge_count;
    int64 edge_index;
    bool found;

    if (PG_ARGISNULL(0) || PG_ARGISNULL(1) || PG_ARGISNULL(2))
    {
        PG_RETURN_NULL();
    }

    agt_arg_vpc = AG_GET_ARG_AGTYPE_P(0);
    if (is_agtype_null(agt_arg_vpc))
    {
        PG_RETURN_NULL();
    }

    get_vle_scalar_arg_no_copy("age_vle_edge_property_at",
                               AG_GET_ARG_AGTYPE_P(1), AGTV_INTEGER, true,
                               &agtv_index, &index_needs_free);
    get_vle_terminal_property_key(fcinfo, "age_vle_edge_property_at",
                                  AG_GET_ARG_AGTYPE_P(2), &key_value,
                                  &key_needs_free);
    if (!validate_vle_property_key(&key_value, key_needs_free))
    {
        if (index_needs_free)
            pfree_agtype_value_content(&agtv_index);
        PG_RETURN_NULL();
    }

    edge_index = agtv_index.val.int_value;
    if (index_needs_free)
    {
        pfree_agtype_value_content(&agtv_index);
    }

    Assert(AGT_ROOT_IS_BINARY(agt_arg_vpc));
    Assert(AGT_ROOT_BINARY_FLAGS(agt_arg_vpc) == AGT_FBINARY_TYPE_VLE_PATH);

    vpc = (VLE_path_container *)agt_arg_vpc;
    edge_count = get_vle_container_edge_count(vpc);
    if (!normalize_vle_container_index(edge_count, &edge_index))
    {
        if (key_needs_free)
            pfree_agtype_value_content(&key_value);
        PG_RETURN_NULL();
    }

    graphid_array = GET_GRAPHID_ARRAY_FROM_CONTAINER(vpc);
    edge_id = graphid_array[(edge_index * 2) + 1];

    ggctx = find_GRAPH_global_context(vpc->graph_oid);
    Assert(ggctx != NULL);
    init_vle_indexed_property_lookup(&lookup, ggctx, vpc, edge_id,
                                     &key_value,
                                     VLE_INDEXED_PROPERTY_EDGE);
    found = get_vle_indexed_property_from_lookup(fcinfo, &lookup, &property);

    if (key_needs_free)
        pfree_agtype_value_content(&key_value);

    if (!found)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(property);
}

static Datum age_vle_edge_endpoint_at(FunctionCallInfo fcinfo,
                                      bool start_endpoint)
{
    agtype *agt_arg_vpc = NULL;
    agtype_value agtv_index;
    agtype_value *agtv_vertex = NULL;
    bool index_needs_free = false;
    int64 edge_index;

    if (PG_ARGISNULL(0) || PG_ARGISNULL(1))
    {
        PG_RETURN_NULL();
    }

    agt_arg_vpc = AG_GET_ARG_AGTYPE_P(0);
    if (is_agtype_null(agt_arg_vpc))
    {
        PG_RETURN_NULL();
    }

    get_vle_scalar_arg_no_copy(start_endpoint ?
                               "age_vle_edge_start_node_at" :
                               "age_vle_edge_end_node_at",
                               AG_GET_ARG_AGTYPE_P(1), AGTV_INTEGER, true,
                               &agtv_index, &index_needs_free);

    edge_index = agtv_index.val.int_value;
    if (index_needs_free)
    {
        pfree_agtype_value_content(&agtv_index);
    }

    agtv_vertex = agtv_materialize_vle_edge_endpoint_at(
        agt_arg_vpc, edge_index, start_endpoint);
    if (agtv_vertex == NULL)
    {
        PG_RETURN_NULL();
    }

    PG_RETURN_POINTER(agtype_value_to_agtype(agtv_vertex));
}

PG_FUNCTION_INFO_V1(age_vle_edge_start_node_at);

Datum age_vle_edge_start_node_at(PG_FUNCTION_ARGS)
{
    return age_vle_edge_endpoint_at(fcinfo, true);
}

PG_FUNCTION_INFO_V1(age_vle_edge_end_node_at);

Datum age_vle_edge_end_node_at(PG_FUNCTION_ARGS)
{
    return age_vle_edge_endpoint_at(fcinfo, false);
}

PG_FUNCTION_INFO_V1(age_vle_edge_endpoint_field_at);

Datum age_vle_edge_endpoint_field_at(PG_FUNCTION_ARGS)
{
    GRAPH_global_context *ggctx = NULL;
    VLE_path_container *vpc = NULL;
    edge_entry *ee = NULL;
    vertex_entry *ve = NULL;
    agtype *agt_arg_vpc = NULL;
    agtype_value agtv_index;
    agtype_value agtv_mode;
    agtype_value agtv_label;
    agtype_in_state agis_result;
    bool index_needs_free = false;
    bool mode_needs_free = false;
    graphid *graphid_array = NULL;
    graphid edge_id;
    graphid endpoint_id;
    int64 edge_count;
    int64 edge_index;
    int64 mode;
    char *label_name = NULL;

    if (PG_ARGISNULL(0) || PG_ARGISNULL(1) || PG_ARGISNULL(2))
    {
        PG_RETURN_NULL();
    }

    agt_arg_vpc = AG_GET_ARG_AGTYPE_P(0);
    if (is_agtype_null(agt_arg_vpc))
    {
        PG_RETURN_NULL();
    }

    get_vle_scalar_arg_no_copy("age_vle_edge_endpoint_field_at",
                               AG_GET_ARG_AGTYPE_P(1), AGTV_INTEGER, true,
                               &agtv_index, &index_needs_free);
    get_vle_scalar_arg_no_copy("age_vle_edge_endpoint_field_at",
                               AG_GET_ARG_AGTYPE_P(2), AGTV_INTEGER, true,
                               &agtv_mode, &mode_needs_free);

    edge_index = agtv_index.val.int_value;
    mode = agtv_mode.val.int_value;
    if (index_needs_free)
    {
        pfree_agtype_value_content(&agtv_index);
    }
    if (mode_needs_free)
    {
        pfree_agtype_value_content(&agtv_mode);
    }

    Assert(AGT_ROOT_IS_BINARY(agt_arg_vpc));
    Assert(AGT_ROOT_BINARY_FLAGS(agt_arg_vpc) == AGT_FBINARY_TYPE_VLE_PATH);

    vpc = (VLE_path_container *)agt_arg_vpc;
    edge_count = get_vle_container_edge_count(vpc);
    if (!normalize_vle_container_index(edge_count, &edge_index))
    {
        PG_RETURN_NULL();
    }

    graphid_array = GET_GRAPHID_ARRAY_FROM_CONTAINER(vpc);
    edge_id = graphid_array[(edge_index * 2) + 1];

    if (mode < 0 || mode > 5)
    {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("age_vle_edge_endpoint_field_at: invalid mode")));
    }

    ggctx = find_GRAPH_global_context(vpc->graph_oid);
    Assert(ggctx != NULL);
    ee = get_edge_entry(ggctx, edge_id);
    Assert(ee != NULL);

    endpoint_id = (mode % 2 == 0) ?
        get_edge_entry_start_vertex_id(ee) :
        get_edge_entry_end_vertex_id(ee);
    ve = get_vertex_entry(ggctx, endpoint_id);
    Assert(ve != NULL);

    if (mode == 4 || mode == 5)
    {
        PG_RETURN_DATUM(get_vertex_entry_properties(ve));
    }

    label_name = get_vertex_entry_label_name(ve);
    agtv_label.type = AGTV_STRING;
    agtv_label.val.string.val = label_name;
    agtv_label.val.string.len = strlen(label_name);

    if (mode == 0 || mode == 1)
    {
        PG_RETURN_POINTER(agtype_value_to_agtype(&agtv_label));
    }
    else if (mode == 2 || mode == 3)
    {
        MemSet(&agis_result, 0, sizeof(agtype_in_state));
        agis_result.res = push_agtype_value(&agis_result.parse_state,
                                            WAGT_BEGIN_ARRAY, NULL);
        agis_result.res = push_agtype_value(&agis_result.parse_state,
                                            WAGT_ELEM, &agtv_label);
        agis_result.res = push_agtype_value(&agis_result.parse_state,
                                            WAGT_END_ARRAY, NULL);

        PG_RETURN_POINTER(agtype_value_to_agtype(agis_result.res));
    }

    Assert(false);
    PG_RETURN_NULL();
}

static Datum age_vle_edge_endpoint_id_at(FunctionCallInfo fcinfo,
                                         bool start_endpoint)
{
    GRAPH_global_context *ggctx = NULL;
    VLE_path_container *vpc = NULL;
    edge_entry *ee = NULL;
    agtype *agt_arg_vpc = NULL;
    agtype_value agtv_index;
    agtype_value agtv_result;
    bool index_needs_free = false;
    graphid *graphid_array = NULL;
    graphid edge_id;
    int64 edge_count;
    int64 edge_index;

    if (PG_ARGISNULL(0) || PG_ARGISNULL(1))
    {
        PG_RETURN_NULL();
    }

    agt_arg_vpc = AG_GET_ARG_AGTYPE_P(0);
    if (is_agtype_null(agt_arg_vpc))
    {
        PG_RETURN_NULL();
    }

    get_vle_scalar_arg_no_copy(start_endpoint ?
                               "age_vle_edge_start_id_at" :
                               "age_vle_edge_end_id_at",
                               AG_GET_ARG_AGTYPE_P(1), AGTV_INTEGER, true,
                               &agtv_index, &index_needs_free);

    edge_index = agtv_index.val.int_value;
    if (index_needs_free)
    {
        pfree_agtype_value_content(&agtv_index);
    }

    Assert(AGT_ROOT_IS_BINARY(agt_arg_vpc));
    Assert(AGT_ROOT_BINARY_FLAGS(agt_arg_vpc) == AGT_FBINARY_TYPE_VLE_PATH);

    vpc = (VLE_path_container *)agt_arg_vpc;
    edge_count = get_vle_container_edge_count(vpc);
    if (!normalize_vle_container_index(edge_count, &edge_index))
    {
        PG_RETURN_NULL();
    }

    graphid_array = GET_GRAPHID_ARRAY_FROM_CONTAINER(vpc);
    edge_id = graphid_array[(edge_index * 2) + 1];

    ggctx = find_GRAPH_global_context(vpc->graph_oid);
    Assert(ggctx != NULL);
    ee = get_edge_entry(ggctx, edge_id);
    Assert(ee != NULL);

    agtv_result.type = AGTV_INTEGER;
    agtv_result.val.int_value = start_endpoint ?
        get_edge_entry_start_vertex_id(ee) :
        get_edge_entry_end_vertex_id(ee);

    PG_RETURN_POINTER(agtype_value_to_agtype(&agtv_result));
}

PG_FUNCTION_INFO_V1(age_vle_edge_start_id_at);

Datum age_vle_edge_start_id_at(PG_FUNCTION_ARGS)
{
    return age_vle_edge_endpoint_id_at(fcinfo, true);
}

PG_FUNCTION_INFO_V1(age_vle_edge_end_id_at);

Datum age_vle_edge_end_id_at(PG_FUNCTION_ARGS)
{
    return age_vle_edge_endpoint_id_at(fcinfo, false);
}

PG_FUNCTION_INFO_V1(age_materialize_vle_edges_tail);

Datum age_materialize_vle_edges_tail(PG_FUNCTION_ARGS)
{
    agtype *agt_arg_vpc = NULL;
    agtype_value *agtv_array = NULL;
    int64 edge_count;

    if (PG_ARGISNULL(0))
    {
        PG_RETURN_NULL();
    }

    agt_arg_vpc = AG_GET_ARG_AGTYPE_P(0);
    if (is_agtype_null(agt_arg_vpc))
    {
        PG_RETURN_NULL();
    }

    edge_count = agtv_vle_edge_count(agt_arg_vpc);
    if (edge_count <= 1)
    {
        PG_RETURN_POINTER(build_empty_agtype_array());
    }

    agtv_array = agtv_materialize_vle_edges_slice(
        agt_arg_vpc, Min(edge_count, 1), edge_count);

    PG_RETURN_POINTER(agtype_value_to_agtype(agtv_array));
}

PG_FUNCTION_INFO_V1(age_materialize_vle_edges_reversed);

Datum age_materialize_vle_edges_reversed(PG_FUNCTION_ARGS)
{
    agtype *agt_arg_vpc = NULL;
    agtype_value *agtv_array = NULL;
    int64 edge_count;

    if (PG_ARGISNULL(0))
    {
        PG_RETURN_NULL();
    }

    agt_arg_vpc = AG_GET_ARG_AGTYPE_P(0);
    if (is_agtype_null(agt_arg_vpc))
    {
        PG_RETURN_NULL();
    }

    edge_count = agtv_vle_edge_count(agt_arg_vpc);
    if (edge_count == 0)
    {
        PG_RETURN_POINTER(build_empty_agtype_array());
    }

    agtv_array = agtv_materialize_vle_edges_reversed(agt_arg_vpc);

    PG_RETURN_POINTER(agtype_value_to_agtype(agtv_array));
}

/* PG wrapper function for age_materialize_vle_path */
PG_FUNCTION_INFO_V1(age_materialize_vle_path);

Datum age_materialize_vle_path(PG_FUNCTION_ARGS)
{
    agtype *agt_arg_vpc = NULL;
    VLE_path_container *vpc = NULL;
    VLEMaterializerObjectCache *object_cache;

    /* if we have a NULL VLE_path_container, return NULL */
    if (PG_ARGISNULL(0))
    {
        PG_RETURN_NULL();
    }

    /* get the VLE_path_container argument */
    agt_arg_vpc = AG_GET_ARG_AGTYPE_P(0);

    /* if NULL, return NULL */
    if (is_agtype_null(agt_arg_vpc))
    {
        PG_RETURN_NULL();
    }

    Assert(AGT_ROOT_IS_BINARY(agt_arg_vpc));
    Assert(AGT_ROOT_BINARY_FLAGS(agt_arg_vpc) == AGT_FBINARY_TYPE_VLE_PATH);
    vpc = (VLE_path_container *)agt_arg_vpc;
    object_cache = age_vle_materializer_object_cache_get(fcinfo, vpc->graph_oid);

    PG_RETURN_POINTER(build_path_agtype(vpc, object_cache));
}

PG_FUNCTION_INFO_V1(age_vle_path_length);

Datum age_vle_path_length(PG_FUNCTION_ARGS)
{
    agtype *agt_arg_vpc = NULL;
    agtype_value agtv_result;

    if (PG_ARGISNULL(0))
    {
        PG_RETURN_NULL();
    }

    agt_arg_vpc = AG_GET_ARG_AGTYPE_P(0);
    if (is_agtype_null(agt_arg_vpc))
    {
        PG_RETURN_NULL();
    }

    agtv_result.type = AGTV_INTEGER;
    agtv_result.val.int_value = agtv_vle_edge_count(agt_arg_vpc);

    PG_RETURN_POINTER(agtype_value_to_agtype(&agtv_result));
}

PG_FUNCTION_INFO_V1(age_vle_terminal_id);

Datum age_vle_terminal_id(PG_FUNCTION_ARGS)
{
    agtype *agt_arg_vpc = NULL;
    VLE_path_container *vpc = NULL;
    graphid *gida = NULL;

    if (PG_ARGISNULL(0))
    {
        PG_RETURN_NULL();
    }

    agt_arg_vpc = AG_GET_ARG_AGTYPE_P(0);
    if (is_agtype_null(agt_arg_vpc))
    {
        PG_RETURN_NULL();
    }

    Assert(AGT_ROOT_IS_BINARY(agt_arg_vpc));
    Assert(AGT_ROOT_BINARY_FLAGS(agt_arg_vpc) == AGT_FBINARY_TYPE_VLE_PATH);

    vpc = (VLE_path_container *)agt_arg_vpc;
    Assert(vpc->graphid_array_size >= 1);

    gida = GET_GRAPHID_ARRAY_FROM_CONTAINER(vpc);

    AG_RETURN_GRAPHID(gida[vpc->graphid_array_size - 1]);
}

PG_FUNCTION_INFO_V1(age_vle_terminal_vertex);

static VLETerminalPropertyCache *get_vle_terminal_property_cache(
    FunctionCallInfo fcinfo)
{
    VLETerminalPropertyCache *cache = NULL;

    cache = (VLETerminalPropertyCache *)fcinfo->flinfo->fn_extra;
    if (cache == NULL)
    {
        cache = MemoryContextAllocZero(fcinfo->flinfo->fn_mcxt,
                                       sizeof(VLETerminalPropertyCache));
        cache->callback.func = destroy_vle_terminal_property_cache;
        cache->callback.arg = cache;
        MemoryContextRegisterResetCallback(fcinfo->flinfo->fn_mcxt,
                                           &cache->callback);
        fcinfo->flinfo->fn_extra = cache;
    }

    return cache;
}

static Relation get_vle_terminal_property_relation(FunctionCallInfo fcinfo,
                                                   Oid relid)
{
    VLETerminalPropertyCache *cache;

    cache = get_vle_terminal_property_cache(fcinfo);

    if (cache->rel == NULL || cache->relid != relid)
    {
        if (cache->rel != NULL)
        {
            table_close(cache->rel, AccessShareLock);
        }

        cache->relid = relid;
        cache->rel = table_open(relid, AccessShareLock);
    }

    return cache->rel;
}

static void get_vle_terminal_property_key(FunctionCallInfo fcinfo,
                                          const char *funcname,
                                          agtype *agt_arg_key,
                                          agtype_value *key_value,
                                          bool *key_needs_free)
{
    VLETerminalPropertyCache *cache;
    MemoryContext old_context;

    cache = get_vle_terminal_property_cache(fcinfo);
    if (cache->key_valid && cache->key_arg == agt_arg_key)
    {
        *key_value = cache->key;
        *key_needs_free = false;
        return;
    }

    get_vle_scalar_arg_no_copy((char *)funcname, agt_arg_key, AGTV_STRING,
                               false, key_value, key_needs_free);
    if (key_value->type != AGTV_STRING)
    {
        return;
    }

    if (cache->key_valid)
    {
        return;
    }

    old_context = MemoryContextSwitchTo(fcinfo->flinfo->fn_mcxt);
    cache->key_arg = agt_arg_key;
    cache->key = *key_value;
    cache->key.val.string.val = pnstrdup(key_value->val.string.val,
                                         key_value->val.string.len);
    cache->key_valid = true;
    MemoryContextSwitchTo(old_context);
}

static bool validate_vle_property_key(agtype_value *key_value,
                                      bool key_needs_free)
{
    switch (key_value->type)
    {
    case AGTV_NULL:
        if (key_needs_free)
            pfree_agtype_value_content(key_value);
        return false;
    case AGTV_STRING:
        return true;
    case AGTV_INTEGER:
        if (key_needs_free)
            pfree_agtype_value_content(key_value);
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("AGTV_INTEGER is not a valid key type")));
        break;
    case AGTV_FLOAT:
        if (key_needs_free)
            pfree_agtype_value_content(key_value);
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("AGTV_FLOAT is not a valid key type")));
        break;
    case AGTV_NUMERIC:
        if (key_needs_free)
            pfree_agtype_value_content(key_value);
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("AGTV_NUMERIC is not a valid key type")));
        break;
    case AGTV_BOOL:
        if (key_needs_free)
            pfree_agtype_value_content(key_value);
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("AGTV_BOOL is not a valid key type")));
        break;
    default:
        if (key_needs_free)
            pfree_agtype_value_content(key_value);
        ereport(ERROR, (errmsg("unknown agtype scalar type")));
        break;
    }

    return false;
}

static void destroy_vle_terminal_property_cache(void *arg)
{
    VLETerminalPropertyCache *cache = (VLETerminalPropertyCache *)arg;

    if (cache->rel != NULL)
    {
        table_close(cache->rel, AccessShareLock);
        cache->rel = NULL;
        cache->relid = InvalidOid;
    }
}

Datum age_vle_terminal_vertex(PG_FUNCTION_ARGS)
{
    GRAPH_global_context *ggctx = NULL;
    VLE_path_container *vpc = NULL;
    agtype *agt_arg_vpc = NULL;
    agtype_value *agtv_vertex = NULL;
    graphid *gida = NULL;
    graphid terminal_id;

    if (PG_ARGISNULL(0))
    {
        PG_RETURN_NULL();
    }

    agt_arg_vpc = AG_GET_ARG_AGTYPE_P(0);
    if (is_agtype_null(agt_arg_vpc))
    {
        PG_RETURN_NULL();
    }

    Assert(AGT_ROOT_IS_BINARY(agt_arg_vpc));
    Assert(AGT_ROOT_BINARY_FLAGS(agt_arg_vpc) == AGT_FBINARY_TYPE_VLE_PATH);

    vpc = (VLE_path_container *)agt_arg_vpc;
    Assert(vpc->graphid_array_size >= 1);

    gida = GET_GRAPHID_ARRAY_FROM_CONTAINER(vpc);
    terminal_id = gida[vpc->graphid_array_size - 1];

    ggctx = find_GRAPH_global_context(vpc->graph_oid);
    Assert(ggctx != NULL);

    agtv_vertex = build_vle_vertex_value(ggctx, terminal_id, NULL);

    PG_RETURN_POINTER(agtype_value_to_agtype(agtv_vertex));
}

PG_FUNCTION_INFO_V1(age_vle_terminal_vertex_properties);

Datum age_vle_terminal_vertex_properties(PG_FUNCTION_ARGS)
{
    GRAPH_global_context *ggctx = NULL;
    VLE_path_container *vpc = NULL;
    vertex_entry *ve = NULL;
    agtype *agt_arg_vpc = NULL;
    graphid *gida = NULL;
    graphid terminal_id;
    Relation rel = NULL;

    if (PG_ARGISNULL(0))
    {
        PG_RETURN_NULL();
    }

    agt_arg_vpc = AG_GET_ARG_AGTYPE_P(0);
    if (is_agtype_null(agt_arg_vpc))
    {
        PG_RETURN_NULL();
    }

    Assert(AGT_ROOT_IS_BINARY(agt_arg_vpc));
    Assert(AGT_ROOT_BINARY_FLAGS(agt_arg_vpc) == AGT_FBINARY_TYPE_VLE_PATH);

    vpc = (VLE_path_container *)agt_arg_vpc;
    Assert(vpc->graphid_array_size >= 1);

    gida = GET_GRAPHID_ARRAY_FROM_CONTAINER(vpc);
    terminal_id = gida[vpc->graphid_array_size - 1];

    ggctx = find_GRAPH_global_context(vpc->graph_oid);
    Assert(ggctx != NULL);

    ve = get_vertex_entry(ggctx, terminal_id);
    Assert(ve != NULL);

    rel = get_vle_terminal_property_relation(
        fcinfo, get_vertex_entry_label_table_oid(ve));

    PG_RETURN_DATUM(get_vertex_entry_properties_with_relation(ve, rel));
}

PG_FUNCTION_INFO_V1(age_vle_terminal_vertex_property);

Datum age_vle_terminal_vertex_property(PG_FUNCTION_ARGS)
{
    agtype *properties = NULL;
    agtype *agt_arg_key = NULL;
    agtype_value key_value;
    agtype_value *property_value = NULL;
    bool key_needs_free = false;

    if (PG_ARGISNULL(0) || PG_ARGISNULL(1))
    {
        PG_RETURN_NULL();
    }

    properties = AG_GET_ARG_AGTYPE_P(0);
    if (is_agtype_null(properties))
    {
        PG_RETURN_NULL();
    }

    agt_arg_key = AG_GET_ARG_AGTYPE_P(1);
    get_vle_terminal_property_key(fcinfo,
                                  "age_vle_terminal_vertex_property",
                                  agt_arg_key, &key_value, &key_needs_free);

    switch (key_value.type)
    {
    case AGTV_NULL:
        if (key_needs_free)
            pfree_agtype_value_content(&key_value);
        PG_RETURN_NULL();
    case AGTV_STRING:
        break;
    case AGTV_INTEGER:
        if (key_needs_free)
            pfree_agtype_value_content(&key_value);
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("AGTV_INTEGER is not a valid key type")));
        break;
    case AGTV_FLOAT:
        if (key_needs_free)
            pfree_agtype_value_content(&key_value);
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("AGTV_FLOAT is not a valid key type")));
        break;
    case AGTV_NUMERIC:
        if (key_needs_free)
            pfree_agtype_value_content(&key_value);
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("AGTV_NUMERIC is not a valid key type")));
        break;
    case AGTV_BOOL:
        if (key_needs_free)
            pfree_agtype_value_content(&key_value);
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("AGTV_BOOL is not a valid key type")));
        break;
    default:
        if (key_needs_free)
            pfree_agtype_value_content(&key_value);
        ereport(ERROR, (errmsg("unknown agtype scalar type")));
        break;
    }

    property_value = find_agtype_value_from_container(&properties->root,
                                                      AGT_FOBJECT,
                                                      &key_value);

    if (key_needs_free)
        pfree_agtype_value_content(&key_value);

    if (property_value == NULL || property_value->type == AGTV_NULL)
    {
        PG_RETURN_NULL();
    }

    properties = agtype_value_to_agtype(property_value);

    PG_RETURN_POINTER(properties);
}

PG_FUNCTION_INFO_V1(age_vle_terminal_vertex_property_from_path);

Datum age_vle_terminal_vertex_property_from_path(PG_FUNCTION_ARGS)
{
    GRAPH_global_context *ggctx = NULL;
    VLE_path_container *vpc = NULL;
    agtype *agt_arg_vpc = NULL;
    agtype *agt_arg_key = NULL;
    agtype_value key_value;
    VLEIndexedPropertyLookup lookup;
    Datum property = (Datum) 0;
    graphid *gida = NULL;
    graphid terminal_id;
    bool key_needs_free = false;

    if (PG_ARGISNULL(0) || PG_ARGISNULL(1))
    {
        PG_RETURN_NULL();
    }

    agt_arg_vpc = AG_GET_ARG_AGTYPE_P(0);
    if (is_agtype_null(agt_arg_vpc))
    {
        PG_RETURN_NULL();
    }

    Assert(AGT_ROOT_IS_BINARY(agt_arg_vpc));
    Assert(AGT_ROOT_BINARY_FLAGS(agt_arg_vpc) == AGT_FBINARY_TYPE_VLE_PATH);

    agt_arg_key = AG_GET_ARG_AGTYPE_P(1);
    get_vle_terminal_property_key(
        fcinfo, "age_vle_terminal_vertex_property_from_path", agt_arg_key,
        &key_value, &key_needs_free);

    switch (key_value.type)
    {
    case AGTV_NULL:
        if (key_needs_free)
            pfree_agtype_value_content(&key_value);
        PG_RETURN_NULL();
    case AGTV_STRING:
        break;
    case AGTV_INTEGER:
        if (key_needs_free)
            pfree_agtype_value_content(&key_value);
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("AGTV_INTEGER is not a valid key type")));
        break;
    case AGTV_FLOAT:
        if (key_needs_free)
            pfree_agtype_value_content(&key_value);
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("AGTV_FLOAT is not a valid key type")));
        break;
    case AGTV_NUMERIC:
        if (key_needs_free)
            pfree_agtype_value_content(&key_value);
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("AGTV_NUMERIC is not a valid key type")));
        break;
    case AGTV_BOOL:
        if (key_needs_free)
            pfree_agtype_value_content(&key_value);
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("AGTV_BOOL is not a valid key type")));
        break;
    default:
        if (key_needs_free)
            pfree_agtype_value_content(&key_value);
        ereport(ERROR, (errmsg("unknown agtype scalar type")));
        break;
    }

    vpc = (VLE_path_container *)agt_arg_vpc;
    Assert(vpc->graphid_array_size >= 1);

    gida = GET_GRAPHID_ARRAY_FROM_CONTAINER(vpc);
    terminal_id = gida[vpc->graphid_array_size - 1];

    ggctx = find_GRAPH_global_context(vpc->graph_oid);
    Assert(ggctx != NULL);
    init_vle_indexed_property_lookup(&lookup, ggctx, vpc, terminal_id,
                                     &key_value,
                                     VLE_INDEXED_PROPERTY_VERTEX);
    if (!get_vle_indexed_property_from_lookup(fcinfo, &lookup, &property))
    {
        if (key_needs_free)
            pfree_agtype_value_content(&key_value);
        PG_RETURN_NULL();
    }

    if (key_needs_free)
        pfree_agtype_value_content(&key_value);

    PG_RETURN_DATUM(property);
}

/*
 * PG function to take a VLE_path_container and return whether the supplied end
 * vertex (target/veid) matches against the last edge in the VLE path. The VLE
 * path is encoded in a BINARY container.
 */
PG_FUNCTION_INFO_V1(age_match_vle_terminal_edge);

Datum age_match_vle_terminal_edge(PG_FUNCTION_ARGS)
{
    VLE_path_container *vpc = NULL;
    agtype *agt_arg_vsid = NULL;
    agtype *agt_arg_veid = NULL;
    agtype *agt_arg_path = NULL;
    graphid vsid = 0;
    graphid veid = 0;
    graphid *gida = NULL;
    int gidasize = 0;
    Oid type0, type1;

    /* check argument count */
    if (PG_NARGS() != 3)
    {
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("age_match_vle_terminal_edge() invalid number of arguments")));
    }

    /*
     * If any argument is NULL, return FALSE. This can occur when this
     * function is used as a join qual in an OPTIONAL MATCH (LEFT JOIN)
     * where a preceding OPTIONAL MATCH produced no results. Returning
     * FALSE allows PostgreSQL to produce the correct NULL-extended rows.
     */
    if (PG_ARGISNULL(0) || PG_ARGISNULL(1) || PG_ARGISNULL(2))
    {
        PG_RETURN_BOOL(false);
    }

    /* get the vpc */
    agt_arg_path = DATUM_GET_AGTYPE_P(PG_GETARG_DATUM(2));

    /* if the vpc is an agtype NULL, return FALSE */
    if (is_agtype_null(agt_arg_path))
    {
        PG_RETURN_BOOL(false);
    }

    /*
     * The vpc (path) must be a binary container and the type of the object in
     * the container must be an AGT_FBINARY_TYPE_VLE_PATH.
     */
    Assert(AGT_ROOT_IS_BINARY(agt_arg_path));
    Assert(AGT_ROOT_BINARY_FLAGS(agt_arg_path) == AGT_FBINARY_TYPE_VLE_PATH);

    /* get the container */
    vpc = (VLE_path_container *)agt_arg_path;

    /* get the graphid array from the container */
    gida = GET_GRAPHID_ARRAY_FROM_CONTAINER(vpc);

    /* get the gida array size */
    gidasize = vpc->graphid_array_size;

    /* verify the minimum size is 3 or 1 */
    Assert(gidasize >= 3 || gidasize == 1);

    /*
     * Get argument types directly instead of using extract_variadic_args.
     * This avoids the expensive exprType/get_call_expr_argtype overhead
     * on every call. Cache the types in fn_extra on first invocation.
     */
    if (fcinfo->flinfo->fn_extra == NULL)
    {
        Oid *cached_types = MemoryContextAlloc(fcinfo->flinfo->fn_mcxt,
                                                2 * sizeof(Oid));
        cached_types[0] = get_fn_expr_argtype(fcinfo->flinfo, 0);
        cached_types[1] = get_fn_expr_argtype(fcinfo->flinfo, 1);
        fcinfo->flinfo->fn_extra = cached_types;
    }
    type0 = ((Oid *)fcinfo->flinfo->fn_extra)[0];
    type1 = ((Oid *)fcinfo->flinfo->fn_extra)[1];

    /* get the vsid */
    if (type0 == AGTYPEOID)
    {
        agt_arg_vsid = DATUM_GET_AGTYPE_P(PG_GETARG_DATUM(0));

        if (!is_agtype_null(agt_arg_vsid))
        {
            vsid = get_agtype_scalar_graphid_arg(agt_arg_vsid,
                "match_vle_terminal_edge() argument 1 must be an agtype integer or a graphid");
        }
        else
        {
            PG_RETURN_BOOL(false);
        }
    }
    else if (type0 == GRAPHIDOID)
    {
        vsid = DATUM_GET_GRAPHID(PG_GETARG_DATUM(0));
    }
    else
    {
        ereport(ERROR,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
             errmsg("match_vle_terminal_edge() argument 1 must be an agtype integer or a graphid")));
    }

    /* get the veid */
    if (type1 == AGTYPEOID)
    {
        agt_arg_veid = DATUM_GET_AGTYPE_P(PG_GETARG_DATUM(1));

        if (!is_agtype_null(agt_arg_veid))
        {
            veid = get_agtype_scalar_graphid_arg(agt_arg_veid,
                "match_vle_terminal_edge() argument 2 must be an agtype integer or a graphid");
        }
        else
        {
            PG_RETURN_BOOL(false);
        }
    }
    else if (type1 == GRAPHIDOID)
    {
        veid = DATUM_GET_GRAPHID(PG_GETARG_DATUM(1));
    }
    else
    {
        ereport(ERROR,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
             errmsg("match_vle_terminal_edge() argument 2 must be an agtype integer or a graphid")));
    }

    /* compare the path beginning or end points */
    PG_RETURN_BOOL(gida[0] == vsid && veid == gida[gidasize - 1]);
}

/* PG helper function to build an agtype (Datum) edge for matching */
PG_FUNCTION_INFO_V1(age_build_vle_match_edge);

Datum age_build_vle_match_edge(PG_FUNCTION_ARGS)
{
    agtype_in_state result;
    agtype_value agtv_zero;
    agtype_value agtv_nstr;
    agtype_value agtv_temp;
    bool agtv_temp_needs_free = false;
    agtype *agt_result;

    /* create an agtype_value integer 0 */
    agtv_zero.type = AGTV_INTEGER;
    agtv_zero.val.int_value = 0;

    /* create an agtype_value null string */
    agtv_nstr.type = AGTV_STRING;
    agtv_nstr.val.string.len = 0;
    agtv_nstr.val.string.val = NULL;

    /* zero the state */
    memset(&result, 0, sizeof(agtype_in_state));

    /* start the object */
    result.res = push_agtype_value(&result.parse_state, WAGT_BEGIN_OBJECT,
                                   NULL);
    /* create dummy graph id */
    result.res = push_agtype_value(&result.parse_state, WAGT_KEY,
                                   string_to_agtype_value("id"));
    result.res = push_agtype_value(&result.parse_state, WAGT_VALUE, &agtv_zero);
    /* process the label */
    result.res = push_agtype_value(&result.parse_state, WAGT_KEY,
                                   string_to_agtype_value("label"));
    if (!PG_ARGISNULL(0))
    {
        get_vle_scalar_arg_no_copy("build_vle_match_edge",
                                   AG_GET_ARG_AGTYPE_P(0), AGTV_STRING, true,
                                   &agtv_temp, &agtv_temp_needs_free);
        result.res = push_agtype_value(&result.parse_state, WAGT_VALUE,
                                       &agtv_temp);
    }
    else
    {
        result.res = push_agtype_value(&result.parse_state, WAGT_VALUE,
                                       &agtv_nstr);
    }
    /* create dummy end_id */
    result.res = push_agtype_value(&result.parse_state, WAGT_KEY,
                                   string_to_agtype_value("end_id"));
    result.res = push_agtype_value(&result.parse_state, WAGT_VALUE, &agtv_zero);
    /* create dummy start_id */
    result.res = push_agtype_value(&result.parse_state, WAGT_KEY,
                                   string_to_agtype_value("start_id"));
    result.res = push_agtype_value(&result.parse_state, WAGT_VALUE, &agtv_zero);

    /* process the properties */
    result.res = push_agtype_value(&result.parse_state, WAGT_KEY,
                                   string_to_agtype_value("properties"));
    if (!PG_ARGISNULL(1))
    {
        agtype *properties = NULL;

        properties = AG_GET_ARG_AGTYPE_P(1);

        if (!AGT_ROOT_IS_OBJECT(properties))
        {
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                     errmsg("build_vle_match_edge(): properties argument must be an object")));
        }

        add_agtype((Datum)properties, false, &result, AGTYPEOID, false);

    }
    else
    {
        result.res = push_agtype_value(&result.parse_state, WAGT_BEGIN_OBJECT,
                                       NULL);
        result.res = push_agtype_value(&result.parse_state, WAGT_END_OBJECT,
                                       NULL);
    }

    result.res = push_agtype_value(&result.parse_state, WAGT_END_OBJECT, NULL);

    result.res->type = AGTV_EDGE;

    agt_result = agtype_value_to_agtype(result.res);
    if (agtv_temp_needs_free)
    {
        pfree_agtype_value_content(&agtv_temp);
    }

    PG_RETURN_POINTER(agt_result);
}

PG_FUNCTION_INFO_V1(_ag_enforce_edge_uniqueness2);

Datum _ag_enforce_edge_uniqueness2(PG_FUNCTION_ARGS)
{
    graphid gid1 = AG_GETARG_GRAPHID(0);
    graphid gid2 = AG_GETARG_GRAPHID(1);

    if (gid1 == gid2)
    {
        PG_RETURN_BOOL(false);
    }

    PG_RETURN_BOOL(true);
}

PG_FUNCTION_INFO_V1(_ag_enforce_edge_uniqueness3);

Datum _ag_enforce_edge_uniqueness3(PG_FUNCTION_ARGS)
{
    graphid gid1 = AG_GETARG_GRAPHID(0);
    graphid gid2 = AG_GETARG_GRAPHID(1);
    graphid gid3 = AG_GETARG_GRAPHID(2);

    if (gid1 == gid2 || gid1 == gid3 || gid2 == gid3)
    {
        PG_RETURN_BOOL(false);
    }

    PG_RETURN_BOOL(true);
}

PG_FUNCTION_INFO_V1(_ag_enforce_edge_uniqueness4);

Datum _ag_enforce_edge_uniqueness4(PG_FUNCTION_ARGS)
{
    graphid gid1 = AG_GETARG_GRAPHID(0);
    graphid gid2 = AG_GETARG_GRAPHID(1);
    graphid gid3 = AG_GETARG_GRAPHID(2);
    graphid gid4 = AG_GETARG_GRAPHID(3);

    if (gid1 == gid2 || gid1 == gid3 || gid1 == gid4 ||
        gid2 == gid3 || gid2 == gid4 || gid3 == gid4)
    {
        PG_RETURN_BOOL(false);
    }

    PG_RETURN_BOOL(true);
}

static int get_edge_uniqueness_args_fast(FunctionCallInfo fcinfo,
                                         Datum **args, Oid **types,
                                         bool **nulls, Datum *fast_args,
                                         Oid *fast_types, bool *fast_nulls)
{
    int nargs;
    int i;

    if (get_fn_expr_variadic(fcinfo->flinfo))
    {
        return extract_variadic_args(fcinfo, 0, true, args, types, nulls);
    }

    nargs = PG_NARGS();
    if (nargs > EDGE_UNIQUENESS_FAST_ARGS)
    {
        return extract_variadic_args(fcinfo, 0, true, args, types, nulls);
    }

    for (i = 0; i < nargs; i++)
    {
        fast_nulls[i] = PG_ARGISNULL(i);
        fast_types[i] = get_cached_edge_uniqueness_argtype(fcinfo, i, nargs);

        if (fast_types[i] == UNKNOWNOID &&
            get_fn_expr_arg_stable(fcinfo->flinfo, i))
        {
            fast_types[i] = TEXTOID;
            fast_args[i] = fast_nulls[i] ? (Datum)0 :
                           CStringGetTextDatum(PG_GETARG_POINTER(i));
        }
        else
        {
            fast_args[i] = PG_GETARG_DATUM(i);
        }
    }

    *args = fast_args;
    *types = fast_types;
    *nulls = fast_nulls;

    return nargs;
}

static Oid get_cached_edge_uniqueness_argtype(FunctionCallInfo fcinfo,
                                              int argno, int nargs)
{
    edge_uniqueness_argtype_cache *cache;
    int i;

    cache = (edge_uniqueness_argtype_cache *)fcinfo->flinfo->fn_extra;
    if (cache == NULL || cache->nargs != nargs)
    {
        cache = MemoryContextAlloc(fcinfo->flinfo->fn_mcxt,
                                   offsetof(edge_uniqueness_argtype_cache,
                                            types) +
                                   sizeof(Oid) * nargs);
        cache->nargs = nargs;

        for (i = 0; i < nargs; i++)
        {
            cache->types[i] = get_fn_expr_argtype(fcinfo->flinfo, i);
        }

        fcinfo->flinfo->fn_extra = cache;
    }

    return cache->types[argno];
}

static bool get_edge_uniqueness_vle_arg(Datum arg, Oid type,
                                        VLE_path_container **vpc)
{
    agtype *agt = NULL;

    if (type != AGTYPEOID)
    {
        return false;
    }

    agt = DATUM_GET_AGTYPE_P(arg);
    if (!AGT_ROOT_IS_BINARY(agt) ||
        AGT_ROOT_BINARY_FLAGS(agt) != AGT_FBINARY_TYPE_VLE_PATH)
    {
        return false;
    }

    *vpc = (VLE_path_container *)agt;
    return true;
}

static bool get_edge_uniqueness_fixed_edge_id(Datum arg, Oid type, int argno,
                                              graphid *edge_id)
{
    agtype *agt = NULL;
    agtype_value agtv_id;
    bool id_needs_free = false;
    bool id_found;

    if (type == INT8OID || type == GRAPHIDOID)
    {
        *edge_id = DatumGetInt64(arg);
        return true;
    }
    if (type != AGTYPEOID)
    {
        return false;
    }

    agt = DATUM_GET_AGTYPE_P(arg);
    if (AGT_ROOT_IS_BINARY(agt) &&
        AGT_ROOT_BINARY_FLAGS(agt) == AGT_FBINARY_TYPE_VLE_PATH)
    {
        return false;
    }
    if (!AGT_ROOT_IS_SCALAR(agt))
    {
        return false;
    }

    id_found = get_ith_agtype_value_from_container_no_copy(
        &agt->root, 0, &agtv_id, &id_needs_free);
    Assert(id_found);

    if (agtv_id.type != AGTV_INTEGER)
    {
        if (id_needs_free)
        {
            pfree_agtype_value_content(&agtv_id);
        }
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("_ag_enforce_edge_uniqueness parameter %d must resolve to an agtype integer",
                        argno)));
    }

    *edge_id = agtv_id.val.int_value;
    if (id_needs_free)
    {
        pfree_agtype_value_content(&agtv_id);
    }

    return true;
}

static bool vle_path_contains_edge_id(VLE_path_container *vpc, graphid edge_id)
{
    return agt_vle_contains_edge_id((agtype *)vpc, edge_id);
}

static int64 get_vle_path_edge_count(VLE_path_container *vpc)
{
    return get_vle_container_edge_count(vpc);
}

static bool vle_paths_have_common_edge(VLE_path_container *left_vpc,
                                       int64 left_edge_count,
                                       VLE_path_container *right_vpc,
                                       int64 right_edge_count)
{
    VLE_path_container *outer_vpc = left_vpc;
    VLE_path_container *inner_vpc = right_vpc;
    graphid *outer_array = NULL;
    graphid *inner_array = NULL;
    int64 outer_edge_count = left_edge_count;
    int64 inner_edge_count = right_edge_count;
    int64 i;
    int64 j;

    if (outer_edge_count > inner_edge_count)
    {
        outer_vpc = right_vpc;
        inner_vpc = left_vpc;
        outer_edge_count = right_edge_count;
        inner_edge_count = left_edge_count;
    }

    outer_array = GET_GRAPHID_ARRAY_FROM_CONTAINER(outer_vpc);
    inner_array = GET_GRAPHID_ARRAY_FROM_CONTAINER(inner_vpc);

    for (i = 0; i < outer_edge_count; i++)
    {
        graphid edge_id = outer_array[(i * 2) + 1];

        for (j = 0; j < inner_edge_count; j++)
        {
            if (edge_id == inner_array[(j * 2) + 1])
            {
                return true;
            }
        }
    }

    return false;
}

static bool try_enforce_two_vle_uniqueness(Datum *args, Oid *types,
                                           int nargs, bool *result)
{
    VLE_path_container *left_vpc = NULL;
    VLE_path_container *right_vpc = NULL;
    int64 left_edge_count;
    int64 right_edge_count;

    if (nargs != 2)
    {
        return false;
    }

    if (!get_edge_uniqueness_vle_arg(args[0], types[0], &left_vpc) ||
        !get_edge_uniqueness_vle_arg(args[1], types[1], &right_vpc))
    {
        return false;
    }

    left_edge_count = get_vle_path_edge_count(left_vpc);
    right_edge_count = get_vle_path_edge_count(right_vpc);
    if (left_edge_count == 0 || right_edge_count == 0)
    {
        *result = true;
        return true;
    }

    if (left_edge_count * right_edge_count > EDGE_UNIQUENESS_NESTED_SCAN_LIMIT)
    {
        return false;
    }

    *result = !vle_paths_have_common_edge(left_vpc, left_edge_count,
                                          right_vpc, right_edge_count);
    return true;
}

static bool try_enforce_one_vle_one_edge_uniqueness(Datum *args, Oid *types,
                                                    int nargs, bool *result)
{
    VLE_path_container *vpc = NULL;
    graphid edge_id = 0;

    if (nargs != 2)
    {
        return false;
    }

    if (get_edge_uniqueness_vle_arg(args[0], types[0], &vpc) &&
        get_edge_uniqueness_fixed_edge_id(args[1], types[1], 1, &edge_id))
    {
        *result = !vle_path_contains_edge_id(vpc, edge_id);
        return true;
    }

    if (get_edge_uniqueness_vle_arg(args[1], types[1], &vpc) &&
        get_edge_uniqueness_fixed_edge_id(args[0], types[0], 0, &edge_id))
    {
        *result = !vle_path_contains_edge_id(vpc, edge_id);
        return true;
    }

    return false;
}

/*
 * This function checks the edges in a MATCH clause to see if they are unique or
 * not. Filters out all the paths where the edge uniques rules are not met.
 * Arguments can be a combination of agtype ints and VLE_path_containers.
 */
PG_FUNCTION_INFO_V1(_ag_enforce_edge_uniqueness);

Datum _ag_enforce_edge_uniqueness(PG_FUNCTION_ARGS)
{
    HTAB *exists_hash = NULL;
    HASHCTL exists_ctl;
    Datum fast_args[EDGE_UNIQUENESS_FAST_ARGS];
    Oid fast_types[EDGE_UNIQUENESS_FAST_ARGS];
    bool fast_nulls[EDGE_UNIQUENESS_FAST_ARGS];
    Datum *args = NULL;
    bool *nulls = NULL;
    Oid *types = NULL;
    int nargs = 0;
    int64 estimated_edges = 0;
    int i = 0;
    bool fast_result = false;

    /* extract our arguments */
    nargs = get_edge_uniqueness_args_fast(fcinfo, &args, &types, &nulls,
                                          fast_args, fast_types, fast_nulls);

    /* verify the arguments */
    for (i = 0; i < nargs; i++)
    {
        if (nulls[i])
        {
             ereport(ERROR,
                     (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                      errmsg("_ag_enforce_edge_uniqueness argument %d must not be NULL",
                             i)));
        }
        if (types[i] != AGTYPEOID &&
            types[i] != INT8OID &&
            types[i] != GRAPHIDOID)
        {
             ereport(ERROR,
                     (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                      errmsg("_ag_enforce_edge_uniqueness argument %d must be AGTYPE, INT8, or GRAPHIDOID",
                             i)));
        }
    }

    if (try_enforce_one_vle_one_edge_uniqueness(args, types, nargs,
                                                &fast_result))
    {
        PG_RETURN_BOOL(fast_result);
    }
    if (try_enforce_two_vle_uniqueness(args, types, nargs, &fast_result))
    {
        PG_RETURN_BOOL(fast_result);
    }

    for (i = 0; i < nargs; i++)
    {
        if (types[i] == INT8OID || types[i] == GRAPHIDOID)
        {
            estimated_edges++;
        }
        else if (types[i] == AGTYPEOID)
        {
            agtype *agt_i = DATUM_GET_AGTYPE_P(args[i]);

            if (AGT_ROOT_IS_BINARY(agt_i) &&
                AGT_ROOT_BINARY_FLAGS(agt_i) == AGT_FBINARY_TYPE_VLE_PATH)
            {
                VLE_path_container *vpc = (VLE_path_container *)agt_i;

                estimated_edges += vpc->graphid_array_size / 2;
            }
            else
            {
                estimated_edges++;
            }
        }
    }

    if (estimated_edges < 2)
    {
        PG_RETURN_BOOL(true);
    }

    /* configure the hash table */
    MemSet(&exists_ctl, 0, sizeof(exists_ctl));
    exists_ctl.keysize = sizeof(int64);
    exists_ctl.entrysize = sizeof(int64);
    exists_ctl.hash = tag_hash;

    /* create exists_hash table */
    exists_hash = hash_create(EXISTS_HTAB_NAME,
                              Max(estimated_edges, EXISTS_HTAB_NAME_MIN_SIZE),
                              &exists_ctl, HASH_ELEM | HASH_FUNCTION);

    /* insert arguments into hash table */
    for (i = 0; i < nargs; i++)
    {
        /* if it is an INT8OID or a GRAPHIDOID */
        if (types[i] == INT8OID || types[i] == GRAPHIDOID)
        {
            graphid edge_id = 0;
            bool found = false;
            int64 *value = NULL;

            edge_id = DatumGetInt64(args[i]);

            /* insert the edge_id */
            value = (int64 *)hash_search(exists_hash, (void *)&edge_id,
                                         HASH_ENTER, &found);

            /* if we found it, we're done, we have a duplicate */
            if (found)
            {
                hash_destroy(exists_hash);
                PG_RETURN_BOOL(false);
            }
            /* otherwise, add it to the returned bucket */
            else
            {
                *value = edge_id;
            }

            continue;
        }
        else if (types[i] == AGTYPEOID)
        {
            /* get the argument */
            agtype *agt_i = DATUM_GET_AGTYPE_P(args[i]);

            /* if the argument is an AGTYPE VLE_path_container */
            if (AGT_ROOT_IS_BINARY(agt_i) &&
                AGT_ROOT_BINARY_FLAGS(agt_i) == AGT_FBINARY_TYPE_VLE_PATH)
            {
                VLE_path_container *vpc = NULL;
                graphid *graphid_array = NULL;
                int64 graphid_array_size = 0;
                int64 j = 0;

                /* cast to VLE_path_container */
                vpc = (VLE_path_container *)agt_i;

                /* get the graphid array */
                graphid_array = GET_GRAPHID_ARRAY_FROM_CONTAINER(vpc);

                /* get the graphid array size */
                graphid_array_size = vpc->graphid_array_size;

                /* insert all the edges in the vpc, into the hash table */
                for (j = 1; j < graphid_array_size - 1; j+=2)
                {
                    int64 *value = NULL;
                    bool found = false;
                    graphid edge_id = 0;

                    /* get the edge id */
                    edge_id = graphid_array[j];

                    /* insert the edge id */
                    value = (int64 *)hash_search(exists_hash, (void *)&edge_id,
                                                 HASH_ENTER, &found);

                    /* if we found it, we're done, we have a duplicate */
                    if (found)
                    {
                        hash_destroy(exists_hash);
                        PG_RETURN_BOOL(false);
                    }
                    /* otherwise, add it to the returned bucket */
                    else
                    {
                        *value = edge_id;
                    }
                }
            }
            /* if it is a regular AGTYPE scalar */
            else if (AGT_ROOT_IS_SCALAR(agt_i))
            {
                agtype_value agtv_id;
                int64 *value = NULL;
                bool found = false;
                bool id_needs_free = false;
                bool id_found;
                graphid edge_id = 0;

                id_found = get_ith_agtype_value_from_container_no_copy(
                    &agt_i->root, 0, &agtv_id, &id_needs_free);
                Assert(id_found);

                if (agtv_id.type != AGTV_INTEGER)
                {
                    if (id_needs_free)
                    {
                        pfree_agtype_value_content(&agtv_id);
                    }
                    ereport(ERROR,
                            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                             errmsg("_ag_enforce_edge_uniqueness parameter %d must resolve to an agtype integer",
                                    i)));
                }

                edge_id = agtv_id.val.int_value;

                if (id_needs_free)
                {
                    pfree_agtype_value_content(&agtv_id);
                }

                /* insert the edge_id */
                value = (int64 *)hash_search(exists_hash, (void *)&edge_id,
                                             HASH_ENTER, &found);

                /* if we found it, we're done, we have a duplicate */
                if (found)
                {
                    hash_destroy(exists_hash);
                    PG_RETURN_BOOL(false);
                }
                /* otherwise, add it to the returned bucket */
                else
                {
                    *value = edge_id;
                }
            }
            else
            {
                ereport(ERROR,
                        (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                         errmsg("_ag_enforce_edge_uniqueness invalid parameter type %d",
                                i)));
            }
        }
        /* it is neither a VLE_path_container, AGTYPE, INT8, or a GRAPHIDOID */
        else
        {
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                     errmsg("_ag_enforce_edge_uniqueness invalid parameter type %d",
                            i)));
        }
    }

    /* if all entries were successfully inserted, we have no duplicates */
    hash_destroy(exists_hash);
    PG_RETURN_BOOL(true);
}
