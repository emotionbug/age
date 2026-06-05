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
#include "access/relation.h"
#include "access/table.h"
#include "catalog/namespace.h"
#include "catalog/pg_collation_d.h"
#include "catalog/pg_am.h"
#include "catalog/pg_type_d.h"
#include "catalog/ag_namespace.h"
#include "catalog/ag_label.h"
#include "catalog/ag_graph.h"
#include "executor/cypher_adjacency_match.h"
#include "executor/cypher_property_projection.h"
#include "executor/cypher_vle_stream.h"
#include "nodes/makefuncs.h"
#include "nodes/cypher_nodes.h"
#include "nodes/nodeFuncs.h"
#include "nodes/primnodes.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/planmain.h"
#include "optimizer/planner.h"
#include "optimizer/restrictinfo.h"
#include "optimizer/tlist.h"
#include "parser/parse_func.h"
#include "rewrite/rewriteManip.h"
#include "utils/inval.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/relcache.h"
#include "utils/spccache.h"
#include "utils/syscache.h"

#include "optimizer/cypher_pathnode.h"
#include "optimizer/cypher_paths.h"
#include "optimizer/cypher_property_paths.h"
#include "parser/cypher_property_signature.h"
#include "utils/ag_func.h"
#include "utils/ag_guc.h"
#include "utils/ag_cache.h"
#include "utils/agtype.h"
#include "utils/age_vle_root.h"
#include "utils/age_vle_source_cost.h"
#include "utils/graphid.h"

typedef enum cypher_clause_kind
{
    CYPHER_CLAUSE_NONE,
    CYPHER_CLAUSE_CREATE,
    CYPHER_CLAUSE_SET,
    CYPHER_CLAUSE_DELETE,
    CYPHER_CLAUSE_MERGE
} cypher_clause_kind;

typedef CustomPath *(*cypher_path_factory)(PlannerInfo *root, RelOptInfo *rel,
                                           List *custom_private);

typedef struct PropertyIndexSurfaceContext
{
    Query *parse;
    Index rti;
    RangeTblEntry *rte;
} PropertyIndexSurfaceContext;

static set_rel_pathlist_hook_type prev_set_rel_pathlist_hook;
static set_join_pathlist_hook_type prev_set_join_pathlist_hook;
static create_upper_paths_hook_type prev_create_upper_paths_hook;
static List *adjacency_match_candidates = NIL;

static Oid cypher_create_clause_func_oid = InvalidOid;
static Oid cypher_set_clause_func_oid = InvalidOid;
static Oid cypher_delete_clause_func_oid = InvalidOid;
static Oid cypher_merge_clause_func_oid = InvalidOid;
static bool cypher_clause_func_callback_registered = false;

static void set_rel_pathlist(PlannerInfo *root, RelOptInfo *rel, Index rti,
                             RangeTblEntry *rte);
static void set_join_pathlist(PlannerInfo *root, RelOptInfo *joinrel,
                              RelOptInfo *outerrel, RelOptInfo *innerrel,
                              JoinType jointype, JoinPathExtraData *extra);
static void create_upper_paths(PlannerInfo *root, UpperRelationKind stage,
                               RelOptInfo *input_rel,
                               RelOptInfo *output_rel, void *extra);
static cypher_clause_kind get_cypher_clause_kind(RangeTblEntry *rte);
static void register_cypher_clause_function_oid_callbacks(void);
static void load_cypher_clause_function_oids(void);
static void invalidate_cypher_clause_function_oids(Datum arg, int cache_id,
                                                   uint32 hash_value);
static void replace_with_cypher_dml_path(PlannerInfo *root, RelOptInfo *rel,
                                         RangeTblEntry *rte,
                                         cypher_path_factory factory);
static void add_adjacency_match_custom_path(
    PlannerInfo *root, RelOptInfo *rel,
    CypherAdjacencyMatchCandidate *candidate);
static Const *find_endpoint_graphid_const(PlannerInfo *root,
                                          CypherAdjacencyMatchCandidate *candidate);
static bool adjacency_match_bound_expr_uses_age_id(Node *node);
static bool adjacency_match_bound_expr_uses_age_id_walker(Node *node,
                                                          void *context);
static void cost_adjacency_match_custom_path(PlannerInfo *root,
                                             RelOptInfo *rel,
                                             CustomPath *cp,
                                             CypherAdjacencyMatchCandidate *candidate);
static Plan *plan_age_adjacency_match_path(PlannerInfo *root,
                                           RelOptInfo *rel,
                                           CustomPath *best_path,
                                           List *tlist, List *clauses,
                                           List *custom_plans);
static List *build_adjacency_match_custom_scan_tlist(Index relid,
                                                     List *tlist,
                                                     List *clauses);
static void collect_adjacency_match_scan_vars_from_list(List *nodes,
                                                        void *context);
static bool collect_adjacency_match_scan_vars(Node *node, void *context);
static const char *adjacency_match_attr_name(AttrNumber attno);
static Oid adjacency_match_attr_type(AttrNumber attno);
static CypherAdjacencyMatchCandidate *pop_adjacency_match_candidate(
    PlannerInfo *root, RangeTblEntry *rte);
static void bind_adjacency_match_candidate_outer_relids(
    PlannerInfo *root, CypherAdjacencyMatchCandidate *candidate);
static void log_adjacency_match_join_paths(RelOptInfo *joinrel,
                                           RelOptInfo *outerrel,
                                           RelOptInfo *innerrel,
                                           JoinType jointype);
static void adjust_adjacency_match_join_rows(RelOptInfo *joinrel,
                                             JoinType jointype);
static bool path_contains_adjacency_match(Path *path);
static bool path_required_outer_is_subset(Path *path, Relids relids);
static const char *path_param_info_string(Path *path);
static void add_narrow_typed_collect_paths(PlannerInfo *root,
                                           RelOptInfo *output_rel);
static void add_narrow_array_agg_property_paths(PlannerInfo *root,
                                                RelOptInfo *output_rel);
static Path *try_narrow_typed_collect_path(PlannerInfo *root,
                                           RelOptInfo *output_rel,
                                           Path *path,
                                           List *handoffs);
static Path *try_narrow_array_agg_property_path(
    PlannerInfo *root, Path *path,
    CypherArrayAggPropertyHandoff *handoff);
static bool pathtarget_contains_only_expr(PathTarget *target, Node *expr);
static Node *rewrite_property_index_surface_mutator(Node *node, void *context);
static void add_property_projection_custom_path(PlannerInfo *root,
                                                RelOptInfo *input_rel,
                                                RelOptInfo *output_rel,
                                                List *slots,
                                                FinalPathExtraData *extra);
static List *make_property_projection_slot_private(List *slots);
static Const *make_oid_const(Oid value);
static void add_deferred_ordered_property_projection_path(
    PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel,
    FinalPathExtraData *extra);
static void add_deferred_projection_paths(PlannerInfo *root,
                                          RelOptInfo *output_rel,
                                          PathTarget *lower_target,
                                          PathTarget *final_target,
                                          FinalPathExtraData *extra,
                                          Cost cost_discount,
                                          const char *debug_label,
                                          bool prune_join_child_targets);
static Path *copy_path_with_deferred_projection_target(PlannerInfo *root,
                                                       Path *path,
                                                       PathTarget *target,
                                                       bool prune_join_child_targets);
static Path *copy_join_path_with_deferred_projection_target(PlannerInfo *root,
                                                            Path *path,
                                                            PathTarget *target,
                                                            bool prune_join_child_targets);
static PathTarget *build_child_deferred_projection_target(PlannerInfo *root,
                                                          Path *child_path,
                                                          PathTarget *target,
                                                          List *joinrestrictinfo,
                                                          bool preserve_label_core,
                                                          bool prune_child_target);
static void add_child_rel_vars_to_pathtarget(PathTarget *child_target,
                                             Path *child_path,
                                             Node *node);
static void add_child_join_clause_vars_to_pathtarget(PathTarget *child_target,
                                                     Path *child_path,
                                                     List *joinrestrictinfo);
static void add_child_label_core_vars_to_pathtarget(PlannerInfo *root,
                                                    PathTarget *child_target,
                                                    Path *child_path);
static void add_child_label_var_to_pathtarget(PlannerInfo *root,
                                              PathTarget *child_target,
                                              Index varno,
                                              AttrNumber attno,
                                              Oid vartype);
static bool target_contains_edge_ctid(PlannerInfo *root, PathTarget *target);
static List *join_path_clause_sources(Path *path);
static bool expression_belongs_to_child_relids(Node *expr, Relids relids);
static bool pathtarget_contains_expr(PathTarget *target, Node *expr);
static Path *copy_projection_capable_path_with_target(PlannerInfo *root,
                                                      Path *path,
                                                      PathTarget *target);
static Path *copy_path_node_shallow(Path *path);
static void add_deferred_count_agtype_projection_path(
    PlannerInfo *root, RelOptInfo *output_rel, FinalPathExtraData *extra);
static void add_deferred_count_agtype_plain_paths(PlannerInfo *root,
                                                  RelOptInfo *output_rel,
                                                  PathTarget *lower_target,
                                                  PathTarget *final_target);
static bool is_age_vle_values_rte(RangeTblEntry *rte, List **func_args);
static void add_age_vle_stream_custom_path(PlannerInfo *root, RelOptInfo *rel,
                                           RangeTblEntry *rte,
                                           List *func_args);
static List *make_age_vle_stream_const_flags(List *func_args);
static List *make_age_vle_stream_graph(List *func_args);
static List *make_age_vle_stream_edge(List *func_args);
static List *make_age_vle_stream_range_direction(List *func_args);
static List *make_age_vle_stream_output(List *func_args);
static List *make_age_vle_stream_edge_source(List *graph, List *edge,
                                             List *range_direction,
                                             List *output);
static void get_vle_stream_edge_source_indexes(
    const char *graph_name, const char *label_name,
    Oid *edge_label_oid,
    bool *adjacency_out, bool *adjacency_in,
    bool *endpoint_start, bool *endpoint_end);
static bool vle_stream_age_adjacency_index_matches(Relation index_rel,
                                                   bool outgoing);
static AgeVLEStreamDirectedSourceKind
age_vle_stream_directed_source_from_traversal(
    VLETraversalSourceKind source_kind);
static bool get_age_vle_stream_integer_const(Node *node, int64 *value,
                                             bool *isnull);
static bool get_age_vle_stream_string_const(Node *node, char **value,
                                            bool *isnull);
static Const *make_int8_const(int64 value);
static Const *make_text_const(const char *value);
static Const *make_agtype_const(agtype *value);
static Plan *plan_age_vle_stream_path(PlannerInfo *root, RelOptInfo *rel,
                                      CustomPath *best_path, List *tlist,
                                      List *clauses, List *custom_plans);
static Plan *plan_age_property_projection_path(PlannerInfo *root,
                                               RelOptInfo *rel,
                                               CustomPath *best_path,
                                               List *tlist, List *clauses,
                                               List *custom_plans);
static const CustomPathMethods age_adjacency_match_path_methods = {
    AGE_ADJACENCY_MATCH_SCAN_NAME,
    plan_age_adjacency_match_path,
    NULL};

static const CustomPathMethods age_property_projection_path_methods = {
    AGE_PROPERTY_PROJECTION_SCAN_NAME,
    plan_age_property_projection_path,
    NULL};

static const CustomPathMethods age_vle_stream_path_methods = {
    AGE_VLE_STREAM_SCAN_NAME,
    plan_age_vle_stream_path,
    NULL};

void cypher_clear_adjacency_match_candidates(void)
{
    adjacency_match_candidates = NIL;
}

void cypher_register_adjacency_match_candidate(Oid edge_label_oid,
                                               Oid index_oid,
                                               const char *edge_alias,
                                               const char *bound_endpoint_alias,
                                               Node *bound_endpoint_expr,
                                               const char *candidate_reason,
                                               bool outgoing,
                                               bool has_edge_variable_projection,
                                               bool has_edge_property_predicate,
                                               bool has_right_label_constraint,
                                               bool has_right_property_predicate,
                                               AttrNumber endpoint_attno)
{
    MemoryContext oldcontext;
    CypherAdjacencyMatchCandidate *candidate;

    if (!OidIsValid(edge_label_oid) ||
        !OidIsValid(index_oid) ||
        edge_alias == NULL)
    {
        return;
    }

    /*
     * The planner hook can be invoked while planning surrounding SQL after the
     * Cypher query has been analyzed. Keep candidate storage out of the
     * per-statement parser context, and remove entries as soon as the matching
     * edge RTE reaches set_rel_pathlist().
     */
    oldcontext = MemoryContextSwitchTo(TopTransactionContext);

    candidate = palloc0(sizeof(*candidate));
    candidate->edge_label_oid = edge_label_oid;
    candidate->index_oid = index_oid;
    candidate->edge_alias = pstrdup(edge_alias);
    candidate->bound_endpoint_alias = bound_endpoint_alias != NULL ?
        pstrdup(bound_endpoint_alias) : NULL;
    candidate->bound_endpoint_expr = copyObject(bound_endpoint_expr);
    candidate->candidate_reason = candidate_reason != NULL ?
        pstrdup(candidate_reason) : NULL;
    candidate->outgoing = outgoing;
    candidate->has_edge_variable_projection = has_edge_variable_projection;
    candidate->has_edge_property_predicate = has_edge_property_predicate;
    candidate->has_right_label_constraint = has_right_label_constraint;
    candidate->has_right_property_predicate = has_right_property_predicate;
    candidate->endpoint_attno = endpoint_attno;

    adjacency_match_candidates = lappend(adjacency_match_candidates,
                                         candidate);

    MemoryContextSwitchTo(oldcontext);
}

void set_rel_pathlist_init(void)
{
    prev_set_rel_pathlist_hook = set_rel_pathlist_hook;
    set_rel_pathlist_hook = set_rel_pathlist;
    prev_set_join_pathlist_hook = set_join_pathlist_hook;
    set_join_pathlist_hook = set_join_pathlist;
    prev_create_upper_paths_hook = create_upper_paths_hook;
    create_upper_paths_hook = create_upper_paths;
    RegisterCustomScanMethods(&age_adjacency_match_scan_methods);
    RegisterCustomScanMethods(&age_property_projection_scan_methods);
    RegisterCustomScanMethods(&age_vle_stream_scan_methods);
}

void set_rel_pathlist_fini(void)
{
    create_upper_paths_hook = prev_create_upper_paths_hook;
    set_join_pathlist_hook = prev_set_join_pathlist_hook;
    set_rel_pathlist_hook = prev_set_rel_pathlist_hook;
}

void cypher_rewrite_property_index_surfaces(Query *parse)
{
    ListCell *lc;
    Index rti = 1;

    if (parse == NULL)
    {
        return;
    }

    foreach(lc, parse->rtable)
    {
        RangeTblEntry *rte = lfirst_node(RangeTblEntry, lc);
        PropertyIndexSurfaceContext context;

        if (rte->rtekind == RTE_SUBQUERY && rte->subquery != NULL)
        {
            cypher_rewrite_property_index_surfaces(rte->subquery);
            rti++;
            continue;
        }

        if (parse->jointree == NULL || parse->jointree->quals == NULL)
        {
            rti++;
            continue;
        }

        if (rte->rtekind != RTE_RELATION || !OidIsValid(rte->relid))
        {
            rti++;
            continue;
        }

        context.parse = parse;
        context.rti = rti;
        context.rte = rte;
        parse->jointree->quals = (Node *)expression_tree_mutator(
            parse->jointree->quals, rewrite_property_index_surface_mutator,
            &context);

        rti++;
    }
}

static Node *rewrite_property_index_surface_mutator(Node *node, void *context)
{
    PropertyIndexSurfaceContext *surface_context = context;
    OpExpr *op;
    Node *left;
    Node *right;
    CypherPropertyIndexHandoff index_handoff;

    if (node == NULL)
        return NULL;

    if (!IsA(node, OpExpr))
    {
        return expression_tree_mutator(node,
                                       rewrite_property_index_surface_mutator,
                                       context);
    }

    op = castNode(OpExpr, node);
    if (list_length(op->args) != 2)
        return expression_tree_mutator(node,
                                       rewrite_property_index_surface_mutator,
                                       context);

    left = linitial(op->args);
    right = lsecond(op->args);

    if (cypher_find_matching_property_index_handoff_for_rte(
            surface_context->rte, surface_context->rti, left,
            &index_handoff))
    {
        Node *index_expr;

        index_expr = cypher_make_property_index_handoff_expr(&index_handoff);
        if (index_expr != NULL && !equal(index_expr, left))
            return cypher_replace_property_index_side(op, true, index_expr);
    }

    if (cypher_find_matching_property_index_handoff_for_rte(
            surface_context->rte, surface_context->rti, right,
            &index_handoff))
    {
        Node *index_expr;

        index_expr = cypher_make_property_index_handoff_expr(&index_handoff);
        if (index_expr != NULL && !equal(index_expr, right))
            return cypher_replace_property_index_side(op, false, index_expr);
    }

    return expression_tree_mutator(node, rewrite_property_index_surface_mutator,
                                   context);
}

static void set_rel_pathlist(PlannerInfo *root, RelOptInfo *rel, Index rti,
                             RangeTblEntry *rte)
{
    CypherAdjacencyMatchCandidate *candidate;

    if (prev_set_rel_pathlist_hook)
        prev_set_rel_pathlist_hook(root, rel, rti, rte);

    if (cypher_rewrite_property_equals_restrictions(root, rel, rti))
    {
        cypher_canonicalize_property_index_predicates(rel);
        check_index_predicates(root, rel);
        cypher_canonicalize_property_index_restrictions(rel);
        create_index_paths(root, rel);
    }

    candidate = pop_adjacency_match_candidate(root, rte);
    if (candidate != NULL)
    {
        ereport(DEBUG2,
                (errmsg_internal("AGE adjacency MATCH candidate visible to planner: "
                                 "edge_rel=%u index=%u alias=%s direction=%s "
                                 "endpoint_attno=%d endpoint_alias=%s "
                                 "endpoint_rti=%u edge_var=%s edge_props=%s "
                                 "right_label=%s right_props=%s reason=%s "
                                 "endpoint_expr=%s",
                                 candidate->edge_label_oid,
                                 candidate->index_oid,
                                 candidate->edge_alias,
                                 candidate->outgoing ? "outgoing" : "incoming",
                                 candidate->endpoint_attno,
                                 candidate->bound_endpoint_alias != NULL ?
                                 candidate->bound_endpoint_alias : "<none>",
                                 candidate->bound_endpoint_rti,
                                 candidate->has_edge_variable_projection ?
                                 "true" : "false",
                                 candidate->has_edge_property_predicate ?
                                 "true" : "false",
                                 candidate->has_right_label_constraint ?
                                 "true" : "false",
                                 candidate->has_right_property_predicate ?
                                 "true" : "false",
                                 candidate->candidate_reason != NULL ?
                                 candidate->candidate_reason : "unknown",
                                 candidate->bound_endpoint_expr != NULL ?
                                 nodeToString(candidate->bound_endpoint_expr) :
                                 "<none>")));
        add_adjacency_match_custom_path(root, rel, candidate);
    }

    if (rte != NULL)
    {
        List *vle_values_args = NIL;

        if (is_age_vle_values_rte(rte, &vle_values_args))
        {
            add_age_vle_stream_custom_path(root, rel, rte, vle_values_args);
        }
    }

    switch (get_cypher_clause_kind(rte))
    {
    case CYPHER_CLAUSE_CREATE:
        replace_with_cypher_dml_path(root, rel, rte, create_cypher_create_path);
        break;
    case CYPHER_CLAUSE_SET:
        replace_with_cypher_dml_path(root, rel, rte, create_cypher_set_path);
        break;
    case CYPHER_CLAUSE_DELETE:
        replace_with_cypher_dml_path(root, rel, rte, create_cypher_delete_path);
        break;
    case CYPHER_CLAUSE_MERGE:
        replace_with_cypher_dml_path(root, rel, rte, create_cypher_merge_path);
        break;
    case CYPHER_CLAUSE_NONE:
        break;
    default:
        ereport(ERROR, (errmsg_internal("invalid cypher_clause_kind")));
    }
}

static void set_join_pathlist(PlannerInfo *root, RelOptInfo *joinrel,
                              RelOptInfo *outerrel, RelOptInfo *innerrel,
                              JoinType jointype, JoinPathExtraData *extra)
{
    if (prev_set_join_pathlist_hook)
        prev_set_join_pathlist_hook(root, joinrel, outerrel, innerrel,
                                    jointype, extra);

    if (!age_enable_adjacency_match_custom_path)
        return;

    adjust_adjacency_match_join_rows(joinrel, jointype);
    log_adjacency_match_join_paths(joinrel, outerrel, innerrel, jointype);
}

static void create_upper_paths(PlannerInfo *root, UpperRelationKind stage,
                               RelOptInfo *input_rel,
                               RelOptInfo *output_rel, void *extra)
{
    List *property_slots = NIL;
    const char *target_source = NULL;

    if (prev_create_upper_paths_hook != NULL)
        prev_create_upper_paths_hook(root, stage, input_rel, output_rel, extra);

    if (stage == UPPERREL_GROUP_AGG)
    {
        if (cypher_rewrite_property_access_aggregate_pathtarget(
                output_rel->reltarget))
        {
            ereport(DEBUG2,
                    (errmsg_internal("AGE property access aggregate rewritten")));
        }
        add_narrow_typed_collect_paths(root, output_rel);
        add_narrow_array_agg_property_paths(root, output_rel);
        return;
    }

    if (stage != UPPERREL_FINAL)
        return;

    add_deferred_count_agtype_projection_path(root, output_rel,
                                             (FinalPathExtraData *)extra);
    add_deferred_ordered_property_projection_path(root, input_rel, output_rel,
                                                 (FinalPathExtraData *)extra);

    if (cypher_detect_simple_property_projection(root, input_rel, output_rel,
                                                 &property_slots,
                                                 &target_source))
    {
        ereport(DEBUG2,
                (errmsg_internal("AGE simple property projection visible after "
                                 "scan/join target: input_paths=%d "
                                 "output_paths=%d target_source=%s "
                                 "slot_count=%d",
                                 list_length(input_rel->pathlist),
                                 list_length(output_rel->pathlist),
                                 target_source,
                                 list_length(property_slots))));
        add_property_projection_custom_path(root, input_rel, output_rel,
                                            property_slots,
                                            (FinalPathExtraData *)extra);
    }
}

static void log_adjacency_match_join_paths(RelOptInfo *joinrel,
                                           RelOptInfo *outerrel,
                                           RelOptInfo *innerrel,
                                           JoinType jointype)
{
    ListCell *lc;

    if (joinrel == NULL ||
        joinrel->pathlist == NIL)
    {
        return;
    }

    foreach(lc, joinrel->pathlist)
    {
        Path *path = lfirst(lc);
        JoinPath *joinpath;
        Path *outer_path;
        Path *inner_path;

        if (!IsA(path, NestPath))
            continue;

        joinpath = (JoinPath *)path;
        outer_path = joinpath->outerjoinpath;
        inner_path = joinpath->innerjoinpath;

        if (!path_contains_adjacency_match(outer_path) &&
            !path_contains_adjacency_match(inner_path))
        {
            continue;
        }

        ereport(DEBUG2,
                (errmsg_internal("AGE adjacency MATCH join path visible: "
                                 "jointype=%d joinrelids=%s outerrelids=%s "
                                 "innerrelids=%s joinrel_rows=%.0f "
                                 "path_rows=%.0f path_required_outer=%s "
                                 "outer_rows=%.0f outer_required_outer=%s "
                                 "inner_rows=%.0f inner_required_outer=%s",
                                 (int)jointype,
                                 bmsToString(joinrel->relids),
                                 outerrel != NULL ?
                                 bmsToString(outerrel->relids) : "<none>",
                                 innerrel != NULL ?
                                 bmsToString(innerrel->relids) : "<none>",
                                 joinrel->rows,
                                 path->rows,
                                 path_param_info_string(path),
                                 outer_path != NULL ? outer_path->rows : 0,
                                 path_param_info_string(outer_path),
                                 inner_path != NULL ? inner_path->rows : 0,
                                 path_param_info_string(inner_path))));
    }
}

static void add_deferred_ordered_property_projection_path(
    PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel,
    FinalPathExtraData *extra)
{
    PathTarget *lower_target;
    PathTarget *final_target;

    if (!cypher_build_ordered_property_projection_targets(
            root, input_rel, output_rel, extra, &lower_target, &final_target))
    {
        return;
    }

    add_deferred_projection_paths(root, output_rel, lower_target, final_target,
                                  extra, 1.0,
                                  "ordered property projection",
                                  true);
}

static void add_deferred_projection_paths(PlannerInfo *root,
                                          RelOptInfo *output_rel,
                                          PathTarget *lower_target,
                                          PathTarget *final_target,
                                          FinalPathExtraData *extra,
                                          Cost cost_discount,
                                          const char *debug_label,
                                          bool prune_join_child_targets)
{
    List *new_paths = NIL;
    ListCell *lc;

    foreach(lc, output_rel->pathlist)
    {
        Path *path = lfirst(lc);
        LimitPath *limit_path;
        Path *deferred_path;
        ProjectionPath *project_path;
        Cost original_startup_cost;
        Cost original_total_cost;
        int disabled_nodes;

        if (!IsA(path, LimitPath))
            continue;

        original_startup_cost = path->startup_cost;
        original_total_cost = path->total_cost;
        disabled_nodes = path->disabled_nodes;

        limit_path = palloc(sizeof(*limit_path));
        memcpy(limit_path, path, sizeof(*limit_path));
        limit_path->path.pathtype = T_Limit;
        limit_path->subpath = copy_path_with_deferred_projection_target(
            root, limit_path->subpath, lower_target,
            prune_join_child_targets);
        if (limit_path->subpath == NULL)
            continue;

        limit_path->path.pathtarget = limit_path->subpath->pathtarget;
        limit_path->path.pathkeys = limit_path->subpath->pathkeys;

        project_path = create_projection_path(root, output_rel,
                                              (Path *)limit_path,
                                              final_target);
        deferred_path = (Path *)project_path;
        deferred_path->rows = extra->count_est > 0 ? extra->count_est :
            path->rows;
        deferred_path->disabled_nodes = disabled_nodes;
        deferred_path->startup_cost = original_startup_cost;
        deferred_path->total_cost = Max(original_startup_cost,
                                        original_total_cost - cost_discount);

        ereport(DEBUG2,
                (errmsg_internal("AGE deferred %s path added: "
                                 "original_cost=%.2f new_cost=%.2f "
                                 "rows=%.0f",
                                 debug_label,
                                 original_total_cost,
                                 deferred_path->total_cost,
                                 deferred_path->rows)));

        new_paths = lappend(new_paths, deferred_path);
    }

    foreach(lc, new_paths)
        add_path(output_rel, lfirst(lc));
}

static Path *copy_path_with_deferred_projection_target(PlannerInfo *root,
                                                       Path *path,
                                                       PathTarget *target,
                                                       bool prune_join_child_targets)
{
    if (path == NULL)
        return NULL;

    if (path->pathtarget != NULL &&
        target != NULL &&
        equal(path->pathtarget->exprs, target->exprs))
    {
        return path;
    }

    if (IsA(path, LimitPath))
    {
        LimitPath *limit_path;

        limit_path = palloc(sizeof(*limit_path));
        memcpy(limit_path, path, sizeof(*limit_path));

        limit_path->subpath = copy_path_with_deferred_projection_target(
            root, limit_path->subpath, target, prune_join_child_targets);
        if (limit_path->subpath == NULL)
            return NULL;
        limit_path->path.pathtype = T_Limit;
        limit_path->path.pathtarget = limit_path->subpath->pathtarget;
        limit_path->path.pathkeys = limit_path->subpath->pathkeys;
        return (Path *)limit_path;
    }
    if (IsA(path, GatherMergePath))
    {
        GatherMergePath *gm_path;

        gm_path = palloc(sizeof(*gm_path));
        memcpy(gm_path, path, sizeof(*gm_path));

        gm_path->subpath = copy_path_with_deferred_projection_target(
            root, gm_path->subpath, target, prune_join_child_targets);
        if (gm_path->subpath == NULL)
            return NULL;
        gm_path->path.pathtype = T_GatherMerge;
        gm_path->path.pathtarget = gm_path->subpath->pathtarget;
        gm_path->path.pathkeys = path->pathkeys;
        return (Path *)gm_path;
    }
    if (IsA(path, UpperUniquePath))
    {
        UpperUniquePath *unique_path;

        unique_path = palloc(sizeof(*unique_path));
        memcpy(unique_path, path, sizeof(*unique_path));

        unique_path->subpath = copy_path_with_deferred_projection_target(
            root, unique_path->subpath, target, prune_join_child_targets);
        if (unique_path->subpath == NULL)
            return NULL;
        unique_path->path.pathtype = T_Unique;
        unique_path->path.pathtarget = unique_path->subpath->pathtarget;
        unique_path->path.pathkeys = unique_path->subpath->pathkeys;
        return (Path *)unique_path;
    }
    if (IsA(path, SortPath))
    {
        SortPath *sort_path;

        sort_path = palloc(sizeof(*sort_path));
        memcpy(sort_path, path, sizeof(*sort_path));

        sort_path->subpath = copy_path_with_deferred_projection_target(
            root, sort_path->subpath, target, prune_join_child_targets);
        if (sort_path->subpath == NULL)
            return NULL;
        sort_path->path.pathtype = T_Sort;
        sort_path->path.pathtarget = sort_path->subpath->pathtarget;
        sort_path->path.pathkeys = path->pathkeys;
        return (Path *)sort_path;
    }
    if (IsA(path, MaterialPath))
    {
        MaterialPath *material_path;

        material_path = palloc(sizeof(*material_path));
        memcpy(material_path, path, sizeof(*material_path));

        material_path->subpath = copy_path_with_deferred_projection_target(
            root, material_path->subpath, target, prune_join_child_targets);
        if (material_path->subpath == NULL)
            return NULL;
        material_path->path.pathtype = T_Material;
        material_path->path.pathtarget = material_path->subpath->pathtarget;
        material_path->path.pathkeys = material_path->subpath->pathkeys;
        return (Path *)material_path;
    }
    if (IsA(path, ProjectionPath))
    {
        ProjectionPath *projection_path = castNode(ProjectionPath, path);

        return copy_path_with_deferred_projection_target(
            root, projection_path->subpath, target, prune_join_child_targets);
    }
    if (IsA(path, NestPath) || IsA(path, MergePath) || IsA(path, HashPath))
        return copy_join_path_with_deferred_projection_target(root, path,
                                                              target,
                                                              prune_join_child_targets);

    if (!is_projection_capable_path(path))
        return NULL;

    return copy_projection_capable_path_with_target(root, path, target);
}

static Path *copy_join_path_with_deferred_projection_target(PlannerInfo *root,
                                                            Path *path,
                                                            PathTarget *target,
                                                            bool prune_join_child_targets)
{
    JoinPath *join_path;
    Path *outer_path;
    Path *inner_path;
    PathTarget *outer_target;
    PathTarget *inner_target;

    join_path = (JoinPath *)copy_path_node_shallow(path);
    if (join_path == NULL)
        return NULL;

    outer_path = join_path->outerjoinpath;
    inner_path = join_path->innerjoinpath;
    outer_target = build_child_deferred_projection_target(
        root, outer_path, target, join_path_clause_sources(path),
        target_contains_edge_ctid(root, target),
        prune_join_child_targets);
    inner_target = build_child_deferred_projection_target(
        root, inner_path, target, join_path_clause_sources(path),
        target_contains_edge_ctid(root, target),
        prune_join_child_targets);

    join_path->outerjoinpath = copy_path_with_deferred_projection_target(
        root, outer_path, outer_target, prune_join_child_targets);
    join_path->innerjoinpath = copy_path_with_deferred_projection_target(
        root, inner_path, inner_target, prune_join_child_targets);

    if (join_path->outerjoinpath == NULL || join_path->innerjoinpath == NULL)
        return NULL;

    join_path->path.pathtarget = target;
    return (Path *)join_path;
}

static PathTarget *build_child_deferred_projection_target(PlannerInfo *root,
                                                          Path *child_path,
                                                          PathTarget *target,
                                                          List *joinrestrictinfo,
                                                          bool preserve_label_core,
                                                          bool prune_child_target)
{
    PathTarget *child_target;
    ListCell *lc;

    child_target = prune_child_target ? create_empty_pathtarget() :
        copy_pathtarget(child_path->pathtarget);
    foreach(lc, target->exprs)
    {
        Node *expr = lfirst(lc);
        bool child_owned;

        child_owned = expression_belongs_to_child_relids(
            expr, child_path->parent->relids);
        if (child_owned &&
            !pathtarget_contains_expr(child_target, expr))
        {
            add_column_to_pathtarget(child_target, (Expr *)copyObject(expr),
                                     0);
        }
        else if (!child_owned)
        {
            add_child_rel_vars_to_pathtarget(child_target, child_path, expr);
        }
    }

    if (!prune_child_target)
    {
        add_child_rel_vars_to_pathtarget(child_target, child_path,
                                         (Node *)target->exprs);
    }
    add_child_join_clause_vars_to_pathtarget(child_target, child_path,
                                             joinrestrictinfo);
    if (child_path->param_info != NULL)
    {
        add_child_join_clause_vars_to_pathtarget(
            child_target, child_path, child_path->param_info->ppi_clauses);
    }
    if (preserve_label_core)
        add_child_label_core_vars_to_pathtarget(root, child_target, child_path);

    child_target = set_pathtarget_cost_width(root, child_target);

    return child_target;
}

static void add_child_rel_vars_to_pathtarget(PathTarget *child_target,
                                             Path *child_path,
                                             Node *node)
{
    List *vars;
    ListCell *lc;

    vars = pull_var_clause(node,
                           PVC_RECURSE_AGGREGATES |
                           PVC_RECURSE_WINDOWFUNCS |
                           PVC_RECURSE_PLACEHOLDERS);

    foreach(lc, vars)
    {
        Var *var = lfirst_node(Var, lc);

        if (var->varno <= 0 ||
            !bms_is_member(var->varno, child_path->parent->relids) ||
            pathtarget_contains_expr(child_target, (Node *)var))
        {
            continue;
        }

        add_column_to_pathtarget(child_target, (Expr *)copyObject(var), 0);
    }

    list_free(vars);
}

static void add_child_join_clause_vars_to_pathtarget(PathTarget *child_target,
                                                     Path *child_path,
                                                     List *joinrestrictinfo)
{
    ListCell *lc;

    foreach(lc, joinrestrictinfo)
    {
        RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc);

        add_child_rel_vars_to_pathtarget(child_target, child_path,
                                         (Node *)rinfo->clause);
    }
}

static void add_child_label_core_vars_to_pathtarget(PlannerInfo *root,
                                                    PathTarget *child_target,
                                                    Path *child_path)
{
    int varno;

    for (varno = 1; varno < root->simple_rel_array_size; varno++)
    {
        RelOptInfo *rel = root->simple_rel_array[varno];

        if (rel == NULL ||
            !bms_is_member(varno, child_path->parent->relids))
        {
            continue;
        }

        if (rel->max_attr < Anum_ag_label_edge_table_properties)
            continue;

        add_child_label_var_to_pathtarget(root, child_target, varno,
                                          SelfItemPointerAttributeNumber,
                                          TIDOID);
        add_child_label_var_to_pathtarget(root, child_target, varno,
                                          Anum_ag_label_edge_table_id,
                                          GRAPHIDOID);
        add_child_label_var_to_pathtarget(root, child_target, varno,
                                          Anum_ag_label_edge_table_start_id,
                                          GRAPHIDOID);
        add_child_label_var_to_pathtarget(root, child_target, varno,
                                          Anum_ag_label_edge_table_end_id,
                                          GRAPHIDOID);
        add_child_label_var_to_pathtarget(root, child_target, varno,
                                          Anum_ag_label_edge_table_properties,
                                          AGTYPEOID);
    }
}

static void add_child_label_var_to_pathtarget(PlannerInfo *root,
                                              PathTarget *child_target,
                                              Index varno,
                                              AttrNumber attno,
                                              Oid vartype)
{
    RelOptInfo *rel;
    Var *var;

    rel = root->simple_rel_array[varno];
    if (rel == NULL)
        return;

    if (attno > 0 && attno > rel->max_attr)
        return;

    var = makeVar(varno, attno, vartype, -1, InvalidOid, 0);
    if (pathtarget_contains_expr(child_target, (Node *)var))
        return;

    add_column_to_pathtarget(child_target, (Expr *)var, 0);
}

static bool target_contains_edge_ctid(PlannerInfo *root, PathTarget *target)
{
    List *vars;
    ListCell *lc;
    bool result = false;

    vars = pull_var_clause((Node *)target->exprs,
                           PVC_RECURSE_AGGREGATES |
                           PVC_RECURSE_WINDOWFUNCS |
                           PVC_RECURSE_PLACEHOLDERS);

    foreach(lc, vars)
    {
        Var *var = lfirst_node(Var, lc);

        if (var->varattno == SelfItemPointerAttributeNumber &&
            var->varno > 0 &&
            var->varno < root->simple_rel_array_size &&
            root->simple_rel_array[var->varno] != NULL &&
            root->simple_rel_array[var->varno]->max_attr >=
            Anum_ag_label_edge_table_properties)
        {
            result = true;
            break;
        }
    }

    list_free(vars);

    return result;
}

static List *join_path_clause_sources(Path *path)
{
    JoinPath *join_path = (JoinPath *)path;
    List *clauses;

    clauses = list_copy(join_path->joinrestrictinfo);
    if (IsA(path, MergePath))
    {
        MergePath *merge_path = (MergePath *)path;

        clauses = list_concat(clauses, list_copy(merge_path->path_mergeclauses));
    }
    else if (IsA(path, HashPath))
    {
        HashPath *hash_path = (HashPath *)path;

        clauses = list_concat(clauses, list_copy(hash_path->path_hashclauses));
    }

    return clauses;
}

static bool expression_belongs_to_child_relids(Node *expr, Relids relids)
{
    List *vars;
    ListCell *lc;
    bool result = true;

    if (expr == NULL)
        return false;

    vars = pull_var_clause(expr,
                           PVC_RECURSE_AGGREGATES |
                           PVC_RECURSE_WINDOWFUNCS |
                           PVC_RECURSE_PLACEHOLDERS);
    if (vars == NIL)
        return false;

    foreach(lc, vars)
    {
        Var *var = lfirst_node(Var, lc);

        if (var->varno <= 0 || !bms_is_member(var->varno, relids))
        {
            result = false;
            break;
        }
    }

    list_free(vars);

    return result;
}

static bool pathtarget_contains_expr(PathTarget *target, Node *expr)
{
    ListCell *lc;

    if (target == NULL || expr == NULL)
        return false;

    foreach(lc, target->exprs)
    {
        if (equal(lfirst(lc), expr))
            return true;
    }

    return false;
}

static Path *copy_projection_capable_path_with_target(PlannerInfo *root,
                                                      Path *path,
                                                      PathTarget *target)
{
    Path *copy;
    QualCost oldcost;

    Assert(is_projection_capable_path(path));

    copy = copy_path_node_shallow(path);
    if (copy == NULL)
        return NULL;

    oldcost = copy->pathtarget->cost;
    copy->pathtarget = target;
    copy->startup_cost += target->cost.startup - oldcost.startup;
    copy->total_cost += target->cost.startup - oldcost.startup +
        (target->cost.per_tuple - oldcost.per_tuple) * copy->rows;

    return copy;
}

static Path *copy_path_node_shallow(Path *path)
{
    Path *copy;
    Size size;

    if (path == NULL)
        return NULL;

    switch (nodeTag(path))
    {
        case T_Path:
            size = sizeof(Path);
            break;
        case T_IndexPath:
            size = sizeof(IndexPath);
            break;
        case T_BitmapHeapPath:
            size = sizeof(BitmapHeapPath);
            break;
        case T_TidPath:
            size = sizeof(TidPath);
            break;
        case T_TidRangePath:
            size = sizeof(TidRangePath);
            break;
        case T_SubqueryScanPath:
            size = sizeof(SubqueryScanPath);
            break;
        case T_ForeignPath:
            size = sizeof(ForeignPath);
            break;
        case T_CustomPath:
            size = sizeof(CustomPath);
            break;
        case T_GatherPath:
            size = sizeof(GatherPath);
            break;
        case T_NestPath:
            size = sizeof(NestPath);
            break;
        case T_MergePath:
            size = sizeof(MergePath);
            break;
        case T_HashPath:
            size = sizeof(HashPath);
            break;
        case T_GroupPath:
            size = sizeof(GroupPath);
            break;
        case T_AggPath:
            size = sizeof(AggPath);
            break;
        case T_WindowAggPath:
            size = sizeof(WindowAggPath);
            break;
        default:
            return NULL;
    }

    copy = palloc(size);
    memcpy(copy, path, size);

    return copy;
}

static void add_deferred_count_agtype_projection_path(
    PlannerInfo *root, RelOptInfo *output_rel, FinalPathExtraData *extra)
{
    PathTarget *lower_target;
    PathTarget *final_target;

    if (root == NULL || output_rel == NULL ||
        !cypher_build_scalar_final_deferred_targets(
            root, root->processed_tlist, &lower_target, &final_target))
    {
        return;
    }

    if (extra != NULL && extra->limit_needed)
    {
        add_deferred_projection_paths(root, output_rel, lower_target,
                                      final_target, extra, 0.5,
                                      "count agtype projection",
                                      false);
    }

    add_deferred_count_agtype_plain_paths(root, output_rel, lower_target,
                                          final_target);
}

static void add_deferred_count_agtype_plain_paths(PlannerInfo *root,
                                                  RelOptInfo *output_rel,
                                                  PathTarget *lower_target,
                                                  PathTarget *final_target)
{
    List *new_paths = NIL;
    ListCell *lc;

    foreach(lc, output_rel->pathlist)
    {
        Path *path = lfirst(lc);
        Path *lower_path;
        ProjectionPath *project_path;
        Path *deferred_path;

        if (IsA(path, LimitPath))
            continue;

        lower_path = copy_path_with_deferred_projection_target(root, path,
                                                               lower_target,
                                                               false);
        if (lower_path == NULL || lower_path == path)
            continue;

        project_path = create_projection_path(root, output_rel, lower_path,
                                              final_target);
        deferred_path = (Path *)project_path;
        deferred_path->rows = path->rows;
        deferred_path->disabled_nodes = path->disabled_nodes;
        deferred_path->startup_cost = path->startup_cost;
        deferred_path->total_cost = Max(path->startup_cost,
                                        path->total_cost - 0.25);

        new_paths = lappend(new_paths, deferred_path);
    }

    foreach(lc, new_paths)
        add_path(output_rel, lfirst(lc));
}

static void add_narrow_array_agg_property_paths(PlannerInfo *root,
                                                RelOptInfo *output_rel)
{
    CypherArrayAggPropertyHandoff handoff;
    Path *best_narrow_path = NULL;
    ListCell *lc;

    if (root == NULL || output_rel == NULL || output_rel->pathlist == NIL)
        return;

    if (!cypher_find_array_agg_property_handoff(output_rel->reltarget,
                                                &handoff))
        return;

    foreach(lc, output_rel->pathlist)
    {
        Path *path = lfirst(lc);
        Path *new_path;

        new_path = try_narrow_array_agg_property_path(root, path, &handoff);
        if (new_path != NULL)
        {
            lfirst(lc) = new_path;
            if (best_narrow_path == NULL ||
                compare_path_costs(new_path, best_narrow_path, TOTAL_COST) < 0)
            {
                best_narrow_path = new_path;
            }
            if (output_rel->cheapest_total_path == path)
                output_rel->cheapest_total_path = new_path;
            if (output_rel->cheapest_startup_path == path)
                output_rel->cheapest_startup_path = new_path;
        }
    }

    if (best_narrow_path != NULL)
    {
        output_rel->cheapest_total_path = best_narrow_path;
        output_rel->cheapest_startup_path = best_narrow_path;
    }
}

static Path *try_narrow_array_agg_property_path(
    PlannerInfo *root, Path *path, CypherArrayAggPropertyHandoff *handoff)
{
    AggPath *agg_path;
    PathTarget *narrow_target;
    PathTarget *agg_target;
    Path *narrow_subpath;
    Path *agg_subpath;
    AggPath *new_agg;
    List *arg_plans;

    if (path == NULL || handoff == NULL || handoff->arg_exprs == NIL ||
        !IsA(path, AggPath))
    {
        return NULL;
    }

    agg_path = (AggPath *)path;
    if (agg_path->aggstrategy == AGG_PLAIN)
    {
        if (agg_path->groupClause != NIL)
            return NULL;
    }
    else if (agg_path->aggstrategy == AGG_SORTED ||
             agg_path->aggstrategy == AGG_HASHED)
    {
        if (agg_path->groupClause == NIL)
            return NULL;
    }
    else
    {
        return NULL;
    }

    if (agg_path->qual != NIL || agg_path->subpath == NULL)
        return NULL;

    if (list_length(handoff->arg_exprs) == 1 &&
        pathtarget_contains_only_expr(agg_path->subpath->pathtarget,
                                      linitial(handoff->arg_exprs)))
    {
        return NULL;
    }

    arg_plans = cypher_build_array_agg_property_arg_plans(
        agg_path->subpath->pathtarget, handoff);
    if (arg_plans == NIL)
        return NULL;

    narrow_target = create_empty_pathtarget();
    if (!cypher_add_aggregate_group_exprs_to_target(narrow_target, agg_path))
        return NULL;
    if (!cypher_add_array_agg_property_arg_plans_to_target(narrow_target,
                                                           arg_plans))
        return NULL;
    narrow_target = set_pathtarget_cost_width(root, narrow_target);

    narrow_subpath = copy_path_with_deferred_projection_target(
        root, agg_path->subpath, narrow_target, false);
    if (narrow_subpath == NULL)
        return NULL;

    agg_subpath = (Path *)create_projection_path(root, narrow_subpath->parent,
                                                 narrow_subpath,
                                                 narrow_target);

    agg_target = cypher_build_array_agg_property_target(
        root, agg_path->path.pathtarget, handoff, arg_plans);
    if (agg_target == NULL)
        return NULL;

    new_agg = palloc(sizeof(*new_agg));
    memcpy(new_agg, agg_path, sizeof(*new_agg));
    new_agg->path.pathtype = T_Agg;
    new_agg->path.pathtarget = agg_target;
    new_agg->subpath = agg_subpath;
    new_agg->path.startup_cost = agg_path->path.startup_cost;
    new_agg->path.total_cost = Max(agg_path->path.startup_cost,
                                   agg_path->path.total_cost - 1.0);

    return (Path *)new_agg;
}

static void add_narrow_typed_collect_paths(PlannerInfo *root,
                                           RelOptInfo *output_rel)
{
    List *handoffs = NIL;
    Path *best_narrow_path = NULL;
    ListCell *lc;

    if (root == NULL || output_rel == NULL || output_rel->pathlist == NIL)
        return;

    if (!cypher_find_multi_typed_collect_handoffs(output_rel->reltarget,
                                                  &handoffs))
    {
        return;
    }

    foreach(lc, output_rel->pathlist)
    {
        Path *path = lfirst(lc);
        Path *new_path;

        new_path = try_narrow_typed_collect_path(root, output_rel, path,
                                                 handoffs);
        if (new_path != NULL)
        {
            lfirst(lc) = new_path;
            if (best_narrow_path == NULL ||
                compare_path_costs(new_path, best_narrow_path, TOTAL_COST) < 0)
            {
                best_narrow_path = new_path;
            }
            if (output_rel->cheapest_total_path == path)
                output_rel->cheapest_total_path = new_path;
            if (output_rel->cheapest_startup_path == path)
                output_rel->cheapest_startup_path = new_path;
        }
    }

    if (best_narrow_path != NULL)
    {
        output_rel->cheapest_total_path = best_narrow_path;
        output_rel->cheapest_startup_path = best_narrow_path;
    }
}

static Path *try_narrow_typed_collect_path(PlannerInfo *root,
                                           RelOptInfo *output_rel,
                                           Path *path,
                                           List *handoffs)
{
    AggPath *agg_path;
    PathTarget *narrow_target;
    PathTarget *agg_target;
    Path *narrow_subpath;
    Path *agg_subpath;
    AggPath *new_agg;
    List *arg_plans;

    if (path == NULL || handoffs == NIL || !IsA(path, AggPath))
        return NULL;

    agg_path = (AggPath *)path;
    if (agg_path->aggstrategy == AGG_PLAIN)
    {
        if (agg_path->groupClause != NIL)
            return NULL;
    }
    else if (agg_path->aggstrategy == AGG_SORTED ||
             agg_path->aggstrategy == AGG_HASHED)
    {
        if (agg_path->groupClause == NIL)
            return NULL;
    }
    else
    {
        return NULL;
    }

    if (agg_path->qual != NIL || agg_path->subpath == NULL)
        return NULL;

    arg_plans = cypher_build_typed_collect_arg_plans(
        agg_path->subpath->pathtarget, handoffs);
    if (arg_plans == NIL)
        return NULL;

    if (list_length(arg_plans) == 1)
    {
        CypherTypedCollectArgPlan *arg_plan = linitial(arg_plans);

        if (pathtarget_contains_only_expr(agg_path->subpath->pathtarget,
                                          arg_plan->arg))
        {
            return NULL;
        }
    }

    /*
     * create_agg_plan() asks its child for CP_LABEL_TLIST, which otherwise
     * lets a projection-capable base scan fall back to its physical tlist.
     */
    narrow_target = create_empty_pathtarget();
    if (!cypher_add_aggregate_group_exprs_to_target(narrow_target, agg_path))
        return NULL;
    if (!cypher_add_typed_collect_arg_plans_to_target(narrow_target,
                                                      arg_plans))
        return NULL;
    narrow_target = set_pathtarget_cost_width(root, narrow_target);

    narrow_subpath = copy_path_with_deferred_projection_target(
        root, agg_path->subpath, narrow_target, false);
    if (narrow_subpath == NULL)
        return NULL;

    agg_subpath = narrow_subpath;
    if (list_length(handoffs) == 1 &&
        ((CypherTypedCollectHandoff *)linitial(handoffs))->aggref->aggdistinct == NIL)
    {
        agg_subpath = (Path *)create_projection_path(root,
                                                     narrow_subpath->parent,
                                                     narrow_subpath,
                                                     narrow_target);
    }

    agg_target = cypher_build_typed_collect_agg_target(
        root, agg_path->path.pathtarget, arg_plans);
    if (agg_target == NULL)
        return NULL;

    new_agg = palloc(sizeof(*new_agg));
    memcpy(new_agg, agg_path, sizeof(*new_agg));
    new_agg->path.pathtype = T_Agg;
    new_agg->path.pathtarget = agg_target;
    new_agg->subpath = agg_subpath;
    new_agg->path.startup_cost = agg_path->path.startup_cost;
    new_agg->path.total_cost = Max(agg_path->path.startup_cost,
                                   agg_path->path.total_cost - 1.0);

    return (Path *)new_agg;
}

static bool pathtarget_contains_only_expr(PathTarget *target, Node *expr)
{
    if (target == NULL || expr == NULL || list_length(target->exprs) != 1)
        return false;

    return equal(linitial(target->exprs), expr);
}

static void add_property_projection_custom_path(PlannerInfo *root,
                                                RelOptInfo *input_rel,
                                                RelOptInfo *output_rel,
                                                List *slots,
                                                FinalPathExtraData *extra)
{
    CustomPath *cp;
    Path *path;
    RelOptInfo *base_rel;
    RangeTblEntry *base_rte;
    Var *properties_var;
    CypherCachedPropertySlotDescriptor *first_slot;
    Index scanrelid;
    int scanrelid_int;
    Path *reference_path;
    double rows;
    double pages;
    Cost random_page_cost;
    Cost seq_page_cost;

    if (input_rel == NULL || output_rel == NULL || slots == NIL ||
        root->parse == NULL ||
        input_rel->pathlist == NIL ||
        !bms_get_singleton_member(input_rel->relids, &scanrelid_int))
    {
        return;
    }
    scanrelid = (Index)scanrelid_int;
    first_slot = linitial(slots);

    if (first_slot->container == NULL ||
        first_slot->keys == NIL ||
        !IsA(first_slot->container, Var))
    {
        return;
    }

    properties_var = castNode(Var, first_slot->container);
    if (properties_var->varno != scanrelid ||
        scanrelid <= 0 ||
        scanrelid >= root->simple_rel_array_size)
    {
        return;
    }
    foreach_ptr(CypherCachedPropertySlotDescriptor, slot, slots)
    {
        Var *slot_properties_var;

        if (slot->container == NULL ||
            slot->keys == NIL ||
            !IsA(slot->container, Var))
        {
            return;
        }

        slot_properties_var = castNode(Var, slot->container);
        if (slot_properties_var->varno != scanrelid ||
            slot_properties_var->varattno !=
            Anum_ag_label_vertex_table_properties)
        {
            return;
        }
    }

    base_rel = root->simple_rel_array[scanrelid];
    base_rte = root->simple_rte_array[scanrelid];
    if (base_rel == NULL ||
        base_rte == NULL ||
        base_rte->rtekind != RTE_RELATION ||
        base_rel->reloptkind != RELOPT_BASEREL ||
        base_rel->baserestrictinfo != NIL ||
        base_rel->rows <= 0 ||
        base_rel->pages <= 0)
    {
        return;
    }

    reference_path = linitial(input_rel->pathlist);
    rows = clamp_row_est(Max(base_rel->rows, 1.0));
    pages = Max(base_rel->pages, 1);
    get_tablespace_page_costs(base_rel->reltablespace, &random_page_cost,
                              &seq_page_cost);
    (void) random_page_cost;

    cp = makeNode(CustomPath);
    cp->path.pathtype = T_CustomScan;
    cp->path.parent = output_rel;
    cp->path.pathtarget = input_rel->reltarget;
    cp->path.param_info = NULL;
    cp->path.parallel_aware = false;
    cp->path.parallel_safe = false;
    cp->path.parallel_workers = 0;
    cp->path.pathkeys = reference_path->pathkeys;
    cp->path.rows = rows;
    cp->path.startup_cost = 0;
    cp->path.total_cost = seq_page_cost * pages + cpu_tuple_cost * rows;
    cp->flags = CUSTOMPATH_SUPPORT_PROJECTION;
    cp->custom_paths = NIL;
    cp->custom_private = list_make2(make_property_projection_slot_private(slots),
                                    makeInteger(scanrelid));
    cp->methods = &age_property_projection_path_methods;

    path = (Path *)cp;
    if (extra != NULL && extra->limit_needed)
    {
        path = (Path *)create_limit_path(root, output_rel, path,
                                         root->parse->limitOffset,
                                         root->parse->limitCount,
                                         root->parse->limitOption,
                                         extra->offset_est,
                                         extra->count_est);
    }

    add_path(output_rel, path);

    ereport(DEBUG2,
            (errmsg_internal("AGE property projection CustomPath added: "
                             "scanrelid=%u rows=%.0f total_cost=%.2f "
                             "slot_count=%d limit_needed=%s",
                             scanrelid, cp->path.rows,
                             cp->path.total_cost,
                             list_length(slots),
                             extra != NULL && extra->limit_needed ?
                             "true" : "false")));
}

static List *make_property_projection_slot_private(List *slots)
{
    List *slot_private = NIL;
    ListCell *lc;

    foreach(lc, slots)
    {
        CypherCachedPropertySlotDescriptor *slot = lfirst(lc);

        slot_private = lappend(
            slot_private,
            list_make3(copyObject(slot->keys),
                       make_oid_const(slot->value_type),
                       make_oid_const(slot->field_result_type)));
    }

    return slot_private;
}

static Plan *plan_age_property_projection_path(PlannerInfo *root,
                                               RelOptInfo *rel,
                                               CustomPath *best_path,
                                               List *tlist, List *clauses,
                                               List *custom_plans)
{
    CustomScan *cs;
    List *slot_private;
    Integer *scanrelid_value;
    Index scanrelid;

    (void) root;
    (void) rel;
    (void) clauses;
    (void) custom_plans;

    slot_private = linitial_node(List, best_path->custom_private);
    scanrelid_value = lsecond_node(Integer, best_path->custom_private);
    scanrelid = intVal(scanrelid_value);

    cs = makeNode(CustomScan);
    cs->scan.plan.startup_cost = best_path->path.startup_cost;
    cs->scan.plan.total_cost = best_path->path.total_cost;
    cs->scan.plan.plan_rows = best_path->path.rows;
    cs->scan.plan.plan_width = best_path->path.pathtarget->width;
    cs->scan.plan.parallel_aware = false;
    cs->scan.plan.parallel_safe = false;
    cs->scan.plan.async_capable = false;
    cs->scan.plan.targetlist = tlist;
    cs->scan.plan.qual = NIL;
    cs->scan.plan.lefttree = NULL;
    cs->scan.plan.righttree = NULL;
    cs->scan.scanrelid = scanrelid;

    cs->flags = best_path->flags;
    cs->custom_plans = NIL;
    cs->custom_exprs = NIL;
    cs->custom_private = copyObject(slot_private);
    cs->custom_scan_tlist = copyObject(tlist);
    cs->custom_relids = bms_make_singleton(scanrelid);
    cs->methods = &age_property_projection_scan_methods;

    return (Plan *)cs;
}

static Const *make_oid_const(Oid value)
{
    return makeConst(OIDOID, -1, InvalidOid, sizeof(Oid),
                     ObjectIdGetDatum(value), false, true);
}

static bool is_age_vle_values_rte(RangeTblEntry *rte, List **func_args)
{
    List *row;
    List *args = NIL;
    ListCell *lc;
    int colno = 0;
    Const *marker;

    if (func_args != NULL)
        *func_args = NIL;

    if (rte == NULL ||
        rte->rtekind != RTE_VALUES ||
        list_length(rte->values_lists) != 1 ||
        rte->eref == NULL ||
        rte->eref->colnames == NIL)
    {
        return false;
    }

    row = linitial_node(List, rte->values_lists);
    if (list_length(row) != AGE_VLE_STREAM_ARG_GRAMMAR_NODE + 3 &&
        list_length(row) != AGE_VLE_STREAM_ARG_TERMINAL_PROPERTY + 3)
    {
        return false;
    }
    if (!IsA(lsecond(row), Const))
        return false;

    marker = lsecond_node(Const, row);
    if (marker->constisnull ||
        marker->consttype != TEXTOID ||
        strcmp(TextDatumGetCString(marker->constvalue),
               AGE_VLE_STREAM_MARKER) != 0)
    {
        return false;
    }

    foreach(lc, row)
    {
        if (colno > 1)
            args = lappend(args, lfirst(lc));
        colno++;
    }

    if (func_args != NULL)
        *func_args = args;

    return true;
}

static void add_age_vle_stream_custom_path(PlannerInfo *root, RelOptInfo *rel,
                                           RangeTblEntry *rte,
                                           List *func_args)
{
    CustomPath *cp;
    Path *reference_path;
    List *custom_private = NIL;

    if (rel == NULL ||
        rel->reloptkind != RELOPT_BASEREL ||
        rel->pathlist == NIL ||
        func_args == NIL)
    {
        return;
    }

    reference_path = linitial(rel->pathlist);
    if (rte->rtekind == RTE_VALUES)
        rel->pathlist = NIL;

    cp = makeNode(CustomPath);
    cp->path.pathtype = T_CustomScan;
    cp->path.parent = rel;
    cp->path.pathtarget = rel->reltarget;
    cp->path.param_info = reference_path->param_info;
    cp->path.parallel_aware = false;
    cp->path.parallel_safe = false;
    cp->path.parallel_workers = 0;
    cp->path.pathkeys = reference_path->pathkeys;
    cp->path.rows = reference_path->rows;
    cp->path.startup_cost = 0;
    cp->path.total_cost = reference_path->total_cost * 0.80;
    cp->flags = 0;
    cp->custom_paths = NIL;
    custom_private = lappend(custom_private, copyObject(func_args));
    custom_private = lappend(custom_private,
                             makeInteger(list_length(func_args)));
    custom_private = lappend(custom_private,
                             make_age_vle_stream_const_flags(
                                 func_args));
    custom_private = lappend(custom_private,
                             make_age_vle_stream_graph(func_args));
    custom_private = lappend(custom_private,
                             make_age_vle_stream_edge(func_args));
    custom_private = lappend(custom_private,
                             make_age_vle_stream_range_direction(
                                 func_args));
    custom_private = lappend(custom_private,
                             make_age_vle_stream_output(func_args));
    custom_private = lappend(custom_private,
                             make_age_vle_stream_edge_source(
                                 list_nth_node(List, custom_private, 3),
                                 list_nth_node(List, custom_private, 4),
                                 list_nth_node(List, custom_private, 5),
                                 list_nth_node(List, custom_private, 6)));
    cp->custom_private = custom_private;
    cp->methods = &age_vle_stream_path_methods;

    add_path(rel, (Path *)cp);

    (void) root;
    ereport(DEBUG2,
            (errmsg_internal("AGE VLE stream CustomPath added: relid=%u "
                             "rows=%.0f total_cost=%.2f columns=%d",
                             rel->relid, cp->path.rows, cp->path.total_cost,
                             list_length(rte->eref->colnames))));
}

static Plan *plan_age_vle_stream_path(PlannerInfo *root, RelOptInfo *rel,
                                      CustomPath *best_path, List *tlist,
                                      List *clauses, List *custom_plans)
{
    CustomScan *cs;
    List *func_args;
    Integer *nargs_value;
    List *const_flags;
    List *graph;
    List *edge;
    List *range_direction;
    List *output;
    List *edge_source;

    (void) root;
    (void) clauses;
    (void) custom_plans;

    func_args = linitial_node(List, best_path->custom_private);
    nargs_value = lsecond_node(Integer, best_path->custom_private);
    const_flags = list_nth_node(List, best_path->custom_private, 2);
    graph = list_nth_node(List, best_path->custom_private, 3);
    edge = list_nth_node(List, best_path->custom_private, 4);
    range_direction = list_nth_node(List, best_path->custom_private, 5);
    output = list_nth_node(List, best_path->custom_private, 6);
    edge_source = list_nth_node(List, best_path->custom_private, 7);

    cs = makeNode(CustomScan);
    cs->scan.plan.startup_cost = best_path->path.startup_cost;
    cs->scan.plan.total_cost = best_path->path.total_cost;
    cs->scan.plan.plan_rows = best_path->path.rows;
    cs->scan.plan.plan_width = rel->reltarget->width;
    cs->scan.plan.parallel_aware = false;
    cs->scan.plan.parallel_safe = false;
    cs->scan.plan.async_capable = false;
    cs->scan.plan.targetlist = tlist;
    cs->scan.plan.qual = extract_actual_clauses(clauses, false);
    cs->scan.plan.lefttree = NULL;
    cs->scan.plan.righttree = NULL;
    cs->scan.scanrelid = 0;

    cs->flags = best_path->flags;
    cs->custom_plans = NIL;
    cs->custom_exprs = copyObject(func_args);
    cs->custom_private = NIL;
    cs->custom_private = lappend(cs->custom_private, copyObject(nargs_value));
    cs->custom_private = lappend(cs->custom_private, copyObject(const_flags));
    cs->custom_private = lappend(cs->custom_private, copyObject(graph));
    cs->custom_private = lappend(cs->custom_private, copyObject(edge));
    cs->custom_private = lappend(cs->custom_private,
                                 copyObject(range_direction));
    cs->custom_private = lappend(cs->custom_private, copyObject(output));
    cs->custom_private = lappend(cs->custom_private,
                                 copyObject(edge_source));
    cs->custom_scan_tlist = copyObject(tlist);
    cs->custom_relids = bms_make_singleton(rel->relid);
    cs->methods = &age_vle_stream_scan_methods;

    return (Plan *)cs;
}

static List *make_age_vle_stream_const_flags(List *func_args)
{
    List *flags = NIL;
    ListCell *lc;

    foreach(lc, func_args)
    {
        Node *arg = (Node *)lfirst(lc);

        flags = lappend(flags, makeInteger(IsA(arg, Const) ? 1 : 0));
    }

    return flags;
}

static List *make_age_vle_stream_graph(List *func_args)
{
    char *graph_name = NULL;
    bool graph_null = false;
    bool graph_known;
    List *descriptor = NIL;

    graph_known =
        list_length(func_args) > AGE_VLE_STREAM_ARG_GRAPH &&
        get_age_vle_stream_string_const(
            list_nth(func_args, AGE_VLE_STREAM_ARG_GRAPH), &graph_name,
            &graph_null);

    descriptor = lappend(descriptor, makeInteger(graph_known ? 1 : 0));
    descriptor = lappend(descriptor, makeInteger(graph_null ? 1 : 0));
    descriptor = lappend(descriptor, make_text_const(graph_name));

    if (graph_name != NULL)
        pfree(graph_name);

    return descriptor;
}

static List *make_age_vle_stream_edge(List *func_args)
{
    Node *node;
    Const *const_arg;
    agtype *agt;
    agtype_value edge_value;
    agtype_value *label_value;
    agtype_value *properties_value;
    agtype *properties = NULL;
    char *label_name = NULL;
    int properties_count = 0;
    bool needs_free = false;
    bool found;
    bool edge_known = false;
    bool label_known = false;
    bool properties_known = false;
    List *descriptor = NIL;

    if (list_length(func_args) <= AGE_VLE_STREAM_ARG_EDGE)
        goto build_descriptor;

    node = list_nth(func_args, AGE_VLE_STREAM_ARG_EDGE);
    if (node == NULL || !IsA(node, Const))
        goto build_descriptor;

    const_arg = (Const *)node;
    if (const_arg->constisnull || const_arg->consttype != AGTYPEOID)
        goto build_descriptor;

    agt = DATUM_GET_AGTYPE_P(const_arg->constvalue);
    if (!AGTYPE_CONTAINER_IS_SCALAR(&agt->root))
        goto build_descriptor;

    found = get_ith_agtype_value_from_container_no_copy(&agt->root, 0,
                                                        &edge_value,
                                                        &needs_free);
    if (!found || edge_value.type != AGTV_EDGE)
    {
        if (needs_free)
            pfree_agtype_value_content(&edge_value);
        goto build_descriptor;
    }

    edge_known = true;
    label_value = AGTYPE_EDGE_GET_LABEL(&edge_value);
    if (label_value->type == AGTV_STRING)
    {
        label_name = pnstrdup(label_value->val.string.val,
                              label_value->val.string.len);
        label_known = true;
    }

    properties_value = AGTYPE_EDGE_GET_PROPERTIES(&edge_value);
    if (properties_value->type == AGTV_OBJECT)
    {
        properties_count = properties_value->val.object.num_pairs;
        properties = agtype_value_to_agtype(properties_value);
        properties_known = true;
    }

    if (needs_free)
        pfree_agtype_value_content(&edge_value);

build_descriptor:
    descriptor = lappend(descriptor, makeInteger(edge_known ? 1 : 0));
    descriptor = lappend(descriptor, makeInteger(label_known ? 1 : 0));
    descriptor = lappend(descriptor, make_text_const(label_name));
    descriptor = lappend(descriptor, makeInteger(properties_known ? 1 : 0));
    descriptor = lappend(descriptor, makeInteger(properties == NULL ? 1 : 0));
    descriptor = lappend(descriptor, make_agtype_const(properties));
    descriptor = lappend(descriptor, make_int8_const(properties_count));

    if (label_name != NULL)
        pfree(label_name);

    return descriptor;
}

static List *make_age_vle_stream_range_direction(List *func_args)
{
    int64 lower = 0;
    int64 upper = 0;
    int64 direction = 0;
    bool lower_null = false;
    bool upper_null = false;
    bool direction_null = false;
    bool lower_known;
    bool upper_known;
    bool direction_known;
    List *descriptor = NIL;

    lower_known =
        list_length(func_args) > AGE_VLE_STREAM_ARG_LOWER &&
        get_age_vle_stream_integer_const(
            list_nth(func_args, AGE_VLE_STREAM_ARG_LOWER), &lower,
            &lower_null);
    upper_known =
        list_length(func_args) > AGE_VLE_STREAM_ARG_UPPER &&
        get_age_vle_stream_integer_const(
            list_nth(func_args, AGE_VLE_STREAM_ARG_UPPER), &upper,
            &upper_null);
    direction_known =
        list_length(func_args) > AGE_VLE_STREAM_ARG_DIRECTION &&
        get_age_vle_stream_integer_const(
            list_nth(func_args, AGE_VLE_STREAM_ARG_DIRECTION), &direction,
            &direction_null);

    descriptor = lappend(descriptor, makeInteger(lower_known ? 1 : 0));
    descriptor = lappend(descriptor, makeInteger(lower_null ? 1 : 0));
    descriptor = lappend(descriptor, make_int8_const(lower));
    descriptor = lappend(descriptor, makeInteger(upper_known ? 1 : 0));
    descriptor = lappend(descriptor, makeInteger(upper_null ? 1 : 0));
    descriptor = lappend(descriptor, make_int8_const(upper));
    descriptor = lappend(descriptor, makeInteger(direction_known ? 1 : 0));
    descriptor = lappend(descriptor, makeInteger(direction_null ? 1 : 0));
    descriptor = lappend(descriptor, make_int8_const(direction));

    return descriptor;
}

static List *make_age_vle_stream_output(List *func_args)
{
    int64 grammar = 0;
    bool grammar_null = false;
    bool grammar_known;
    char *terminal_key = NULL;
    bool terminal_key_null = true;
    bool terminal_key_known = false;
    int terminal_key_len = 0;
    AgeVLEOutputRequirement output_requirement =
        AGE_VLE_OUTPUT_REQUIREMENT_PATH;
    bool materializer_vertex_prefetch;
    List *descriptor = NIL;

    grammar_known =
        list_length(func_args) > AGE_VLE_STREAM_ARG_GRAMMAR_NODE &&
        get_age_vle_stream_integer_const(
            list_nth(func_args, AGE_VLE_STREAM_ARG_GRAMMAR_NODE), &grammar,
            &grammar_null);
    if (list_length(func_args) > AGE_VLE_STREAM_ARG_TERMINAL_PROPERTY)
    {
        terminal_key_known =
            get_age_vle_stream_string_const(
                list_nth(func_args, AGE_VLE_STREAM_ARG_TERMINAL_PROPERTY),
                &terminal_key, &terminal_key_null);
        if (terminal_key_known && terminal_key_null)
        {
            output_requirement =
                AGE_VLE_OUTPUT_REQUIREMENT_TERMINAL_PROPERTIES;
        }
        else
        {
            output_requirement = AGE_VLE_OUTPUT_REQUIREMENT_TERMINAL_PROPERTY;
        }
        if (terminal_key_known && !terminal_key_null && terminal_key != NULL)
        {
            terminal_key_len = strlen(terminal_key);
        }
    }
    else if (grammar_known && !grammar_null && grammar < 0)
    {
        output_requirement = AGE_VLE_OUTPUT_REQUIREMENT_TERMINAL_VERTEX;
    }
    materializer_vertex_prefetch =
        output_requirement == AGE_VLE_OUTPUT_REQUIREMENT_PATH;

    descriptor = lappend(descriptor, makeInteger(grammar_known ? 1 : 0));
    descriptor = lappend(descriptor, makeInteger(grammar_null ? 1 : 0));
    descriptor = lappend(descriptor, make_int8_const(grammar));
    descriptor = lappend(descriptor,
                         make_int8_const((int64)output_requirement));
    descriptor = lappend(descriptor, makeInteger(terminal_key_known ? 1 : 0));
    descriptor = lappend(descriptor, makeInteger(terminal_key_null ? 1 : 0));
    descriptor = lappend(descriptor, make_text_const(terminal_key));
    descriptor = lappend(descriptor, make_int8_const(terminal_key_len));
    descriptor = lappend(descriptor,
                         makeInteger(terminal_key_len == 1 ? 1 : 0));
    descriptor = lappend(descriptor,
                         makeInteger(materializer_vertex_prefetch ? 1 : 0));
    descriptor = lappend(
        descriptor,
        make_int8_const(AGE_VERTEX_PROPERTY_PREFETCH_MIN_REL_CANDIDATES));

    if (terminal_key != NULL)
        pfree(terminal_key);

    return descriptor;
}

static List *make_age_vle_stream_edge_source(List *graph, List *edge,
                                             List *range_direction,
                                             List *output)
{
    AgeVLEStreamEdgeSourceKind source_kind =
        AGE_VLE_STREAM_EDGE_SOURCE_GLOBAL_METADATA;
    List *descriptor = NIL;
    bool graph_known;
    bool graph_null;
    bool edge_known;
    bool label_known;
    bool properties_known;
    bool grammar_known;
    bool grammar_null;
    bool direction_known;
    bool direction_null;
    bool upper_known;
    bool upper_null;
    int64 properties_count;
    int64 grammar_value;
    int64 output_requirement;
    int64 direction_value;
    int64 upper_value;
    bool adjacency_out = false;
    bool adjacency_in = false;
    bool endpoint_start = false;
    bool endpoint_end = false;
    bool local_edge_state = false;
    Oid edge_label_oid = InvalidOid;
    VLESourceFanoutEvidence source_evidence = {InvalidOid, 0.0, 0.0, 0.0};
    VLETraversalSourceCandidates source_candidates;
    VLETraversalSourceLayoutInput source_input;
    VLETraversalSourceLayoutDecision source_decision;
    VLEStreamSourceCostInput cost_input;
    VLEStreamSourceCostDecision cost_decision;
    AgeVLEStreamDirectedSourceKind outgoing_kind =
        AGE_VLE_STREAM_DIRECTED_SOURCE_NONE;
    AgeVLEStreamDirectedSourceKind incoming_kind =
        AGE_VLE_STREAM_DIRECTED_SOURCE_NONE;
    AgeVLEStreamDirectedSourceKind policy_outgoing_kind =
        AGE_VLE_STREAM_DIRECTED_SOURCE_NONE;
    AgeVLEStreamDirectedSourceKind policy_incoming_kind =
        AGE_VLE_STREAM_DIRECTED_SOURCE_NONE;
    char *cost_policy = NULL;
    char *graph_name = NULL;
    char *label_name = NULL;

    Assert(list_length(graph) == AGE_VLE_STREAM_GRAPH_COUNT);
    Assert(list_length(edge) == AGE_VLE_STREAM_EDGE_COUNT);
    Assert(list_length(range_direction) ==
           AGE_VLE_STREAM_RANGE_DIRECTION_COUNT);
    Assert(list_length(output) == AGE_VLE_STREAM_OUTPUT_COUNT);

    graph_known = intVal(list_nth_node(Integer, graph,
                                       AGE_VLE_STREAM_GRAPH_KNOWN)) != 0;
    graph_null = intVal(list_nth_node(Integer, graph,
                                      AGE_VLE_STREAM_GRAPH_NULL)) != 0;
    edge_known = intVal(list_nth_node(Integer, edge,
                                      AGE_VLE_STREAM_EDGE_KNOWN)) != 0;
    label_known = intVal(list_nth_node(Integer, edge,
                                       AGE_VLE_STREAM_EDGE_LABEL_KNOWN)) != 0;
    properties_known =
        intVal(list_nth_node(Integer, edge,
                             AGE_VLE_STREAM_EDGE_PROPERTIES_KNOWN)) != 0;
    properties_count = DatumGetInt64(
        list_nth_node(Const, edge,
                      AGE_VLE_STREAM_EDGE_PROPERTIES_COUNT)->constvalue);
    grammar_known =
        intVal(list_nth_node(Integer, output,
                             AGE_VLE_STREAM_OUTPUT_GRAMMAR_KNOWN)) != 0;
    grammar_null =
        intVal(list_nth_node(Integer, output,
                             AGE_VLE_STREAM_OUTPUT_GRAMMAR_NULL)) != 0;
    grammar_value = DatumGetInt64(
        list_nth_node(Const, output,
                      AGE_VLE_STREAM_OUTPUT_GRAMMAR_VALUE)->constvalue);
    output_requirement = DatumGetInt64(
        list_nth_node(Const, output,
                      AGE_VLE_STREAM_OUTPUT_REQUIREMENT)->constvalue);
    direction_known =
        intVal(list_nth_node(Integer, range_direction,
                             AGE_VLE_STREAM_DIRECTION_KNOWN)) != 0;
    direction_null =
        intVal(list_nth_node(Integer, range_direction,
                             AGE_VLE_STREAM_DIRECTION_NULL)) != 0;
    direction_value = DatumGetInt64(
        list_nth_node(Const, range_direction,
                      AGE_VLE_STREAM_DIRECTION_VALUE)->constvalue);
    upper_known =
        intVal(list_nth_node(Integer, range_direction,
                             AGE_VLE_STREAM_RANGE_UPPER_KNOWN)) != 0;
    upper_null =
        intVal(list_nth_node(Integer, range_direction,
                             AGE_VLE_STREAM_RANGE_UPPER_NULL)) != 0;
    upper_value = DatumGetInt64(
        list_nth_node(Const, range_direction,
                      AGE_VLE_STREAM_RANGE_UPPER_VALUE)->constvalue);

    if (!graph_known || graph_null || !edge_known || !label_known ||
        !properties_known || !grammar_known || grammar_null ||
        !direction_known || direction_null)
    {
        source_kind = AGE_VLE_STREAM_EDGE_SOURCE_DYNAMIC;
        goto build_descriptor;
    }

    graph_name = TextDatumGetCString(
        list_nth_node(Const, graph,
                      AGE_VLE_STREAM_GRAPH_VALUE)->constvalue);
    label_name = TextDatumGetCString(
        list_nth_node(Const, edge,
                      AGE_VLE_STREAM_EDGE_LABEL_VALUE)->constvalue);

    get_vle_stream_edge_source_indexes(graph_name, label_name,
                                       &edge_label_oid,
                                       &adjacency_out, &adjacency_in,
                                       &endpoint_start, &endpoint_end);
    estimate_vle_source_fanout_evidence(&source_evidence, edge_label_oid);
    if (grammar_value < 0 &&
        output_requirement != AGE_VLE_OUTPUT_REQUIREMENT_PATH &&
        properties_count == 0)
    {
        bool has_out_source = adjacency_out || endpoint_start;
        bool has_in_source = adjacency_in || endpoint_end;

        if (((cypher_rel_dir)direction_value == CYPHER_REL_DIR_RIGHT &&
             has_out_source) ||
            ((cypher_rel_dir)direction_value == CYPHER_REL_DIR_LEFT &&
             has_in_source) ||
            ((cypher_rel_dir)direction_value == CYPHER_REL_DIR_NONE &&
             has_out_source && has_in_source))
        {
            source_kind = AGE_VLE_STREAM_EDGE_SOURCE_LOCAL_INDEX_CANDIDATE;
            local_edge_state = true;
        }
    }

    source_candidates.age_adjacency_out = adjacency_out;
    source_candidates.age_adjacency_in = adjacency_in;
    source_candidates.endpoint_start = endpoint_start;
    source_candidates.endpoint_end = endpoint_end;

    source_input.indexes = NULL;
    source_input.upper = upper_value;
    source_input.upper_infinite = !upper_known || upper_null;
    source_input.use_local_edge_state = local_edge_state;
    source_input.label_constrained = true;
    source_input.has_property_constraints = properties_count > 0;
    source_input.preferred_source_known = false;
    source_input.preferred_outgoing_kind = VLE_TRAVERSAL_SOURCE_NONE;
    source_input.preferred_incoming_kind = VLE_TRAVERSAL_SOURCE_NONE;
    select_vle_traversal_source_layout(&source_decision, &source_input,
                                       &source_candidates);
    outgoing_kind = age_vle_stream_directed_source_from_traversal(
        source_decision.outgoing_kind);
    incoming_kind = age_vle_stream_directed_source_from_traversal(
        source_decision.incoming_kind);
    if (source_kind == AGE_VLE_STREAM_EDGE_SOURCE_LOCAL_INDEX_CANDIDATE)
    {
        cost_input.source_kind = source_kind;
        cost_input.outgoing_kind = outgoing_kind;
        cost_input.incoming_kind = incoming_kind;
        cost_input.evidence = &source_evidence;
        cost_input.upper = upper_value;
        cost_input.upper_infinite = !upper_known || upper_null;
        cost_input.has_property_constraints = properties_count > 0;
        cost_input.endpoint_start = endpoint_start;
        cost_input.endpoint_end = endpoint_end;
        cost_input.age_adjacency_out = adjacency_out;
        cost_input.age_adjacency_in = adjacency_in;
        choose_vle_stream_source_cost_decision(&cost_decision, &cost_input);
        policy_outgoing_kind = cost_decision.outgoing_kind;
        policy_incoming_kind = cost_decision.incoming_kind;
        cost_policy = cost_decision.policy_text;
        outgoing_kind = cost_decision.outgoing_kind;
        incoming_kind = cost_decision.incoming_kind;
    }
    else
    {
        policy_outgoing_kind = outgoing_kind;
        policy_incoming_kind = incoming_kind;
    }

build_descriptor:
    descriptor = lappend(descriptor, make_int8_const((int64)source_kind));
    descriptor = lappend(descriptor, makeInteger(adjacency_out ? 1 : 0));
    descriptor = lappend(descriptor, makeInteger(adjacency_in ? 1 : 0));
    descriptor = lappend(descriptor, makeInteger(endpoint_start ? 1 : 0));
    descriptor = lappend(descriptor, makeInteger(endpoint_end ? 1 : 0));
    descriptor = lappend(descriptor, makeInteger(local_edge_state ? 1 : 0));
    descriptor = lappend(descriptor, make_int8_const((int64)outgoing_kind));
    descriptor = lappend(descriptor, make_int8_const((int64)incoming_kind));
    descriptor = lappend(descriptor, make_int8_const(
        round_vle_source_cost_evidence(source_evidence.reltuples)));
    descriptor = lappend(descriptor, make_int8_const(
        round_vle_source_cost_evidence(source_evidence.start_fanout)));
    descriptor = lappend(descriptor, make_int8_const(
        round_vle_source_cost_evidence(source_evidence.end_fanout)));
    descriptor = lappend(descriptor, make_text_const(cost_policy));
    descriptor = lappend(descriptor,
                         make_int8_const((int64)policy_outgoing_kind));
    descriptor = lappend(descriptor,
                         make_int8_const((int64)policy_incoming_kind));

    if (graph_name != NULL)
        pfree(graph_name);
    if (label_name != NULL)
        pfree(label_name);
    if (cost_policy != NULL)
        pfree(cost_policy);

    return descriptor;
}

static void get_vle_stream_edge_source_indexes(
    const char *graph_name, const char *label_name,
    Oid *edge_label_oid,
    bool *adjacency_out, bool *adjacency_in,
    bool *endpoint_start, bool *endpoint_end)
{
    static Oid age_adjacency_am_oid = InvalidOid;
    Oid graph_oid;
    label_cache_data *label_cache;
    Relation edge_rel;
    List *index_list;
    ListCell *lc;

    Assert(edge_label_oid != NULL);
    Assert(adjacency_out != NULL);
    Assert(adjacency_in != NULL);
    Assert(endpoint_start != NULL);
    Assert(endpoint_end != NULL);

    *edge_label_oid = InvalidOid;
    *adjacency_out = false;
    *adjacency_in = false;
    *endpoint_start = false;
    *endpoint_end = false;

    if (graph_name == NULL || label_name == NULL)
        return;

    graph_oid = get_graph_oid(graph_name);
    if (!OidIsValid(graph_oid))
        return;

    label_cache = search_label_name_graph_cache_cached(label_name, graph_oid);
    if (label_cache == NULL || !OidIsValid(label_cache->relation))
        return;

    *edge_label_oid = label_cache->relation;
    edge_rel = relation_open(label_cache->relation, AccessShareLock);
    *endpoint_start = OidIsValid(find_usable_btree_index_for_attr(
        edge_rel, Anum_ag_label_edge_table_start_id));
    *endpoint_end = OidIsValid(find_usable_btree_index_for_attr(
        edge_rel, Anum_ag_label_edge_table_end_id));

    if (!OidIsValid(age_adjacency_am_oid))
        age_adjacency_am_oid =
            GetSysCacheOid1(AMNAME, Anum_pg_am_oid,
                            CStringGetDatum("age_adjacency"));

    if (OidIsValid(age_adjacency_am_oid))
    {
        index_list = RelationGetIndexList(edge_rel);
        foreach(lc, index_list)
        {
            Oid index_oid = lfirst_oid(lc);
            Relation index_rel;

            index_rel = index_open(index_oid, AccessShareLock);
            if (index_rel->rd_rel->relam == age_adjacency_am_oid &&
                index_rel->rd_index != NULL &&
                index_rel->rd_index->indisvalid &&
                index_rel->rd_index->indisready)
            {
                if (vle_stream_age_adjacency_index_matches(index_rel, true))
                    *adjacency_out = true;
                if (vle_stream_age_adjacency_index_matches(index_rel, false))
                    *adjacency_in = true;
            }
            index_close(index_rel, AccessShareLock);
            if (*adjacency_out && *adjacency_in)
                break;
        }
        list_free(index_list);
    }

    relation_close(edge_rel, AccessShareLock);
}

static bool vle_stream_age_adjacency_index_matches(Relation index_rel,
                                                   bool outgoing)
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

static AgeVLEStreamDirectedSourceKind
age_vle_stream_directed_source_from_traversal(
    VLETraversalSourceKind source_kind)
{
    switch (source_kind)
    {
        case VLE_TRAVERSAL_SOURCE_NONE:
            return AGE_VLE_STREAM_DIRECTED_SOURCE_NONE;
        case VLE_TRAVERSAL_SOURCE_AGE_ADJACENCY:
            return AGE_VLE_STREAM_DIRECTED_SOURCE_AGE_ADJACENCY;
        case VLE_TRAVERSAL_SOURCE_ENDPOINT_BTREE:
            return AGE_VLE_STREAM_DIRECTED_SOURCE_ENDPOINT_BTREE;
    }

    return AGE_VLE_STREAM_DIRECTED_SOURCE_NONE;
}

static bool get_age_vle_stream_integer_const(Node *node, int64 *value,
                                             bool *isnull)
{
    Const *const_arg;
    agtype *agt;
    agtype_value agtv;
    bool needs_free = false;
    bool found;

    Assert(value != NULL);
    Assert(isnull != NULL);

    *value = 0;
    *isnull = false;

    if (node == NULL || !IsA(node, Const))
        return false;

    const_arg = (Const *)node;
    if (const_arg->constisnull)
    {
        *isnull = true;
        return true;
    }
    if (const_arg->consttype != AGTYPEOID)
        return false;

    agt = DATUM_GET_AGTYPE_P(const_arg->constvalue);
    if (!AGTYPE_CONTAINER_IS_SCALAR(&agt->root))
        return false;

    found = get_ith_agtype_value_from_container_no_copy(&agt->root, 0, &agtv,
                                                        &needs_free);
    if (!found || agtv.type == AGTV_NULL)
    {
        if (needs_free)
            pfree_agtype_value_content(&agtv);
        *isnull = true;
        return true;
    }
    if (agtv.type != AGTV_INTEGER)
    {
        if (needs_free)
            pfree_agtype_value_content(&agtv);
        return false;
    }

    *value = agtv.val.int_value;
    if (needs_free)
        pfree_agtype_value_content(&agtv);

    return true;
}

static bool get_age_vle_stream_string_const(Node *node, char **value,
                                            bool *isnull)
{
    Const *const_arg;
    agtype *agt;
    agtype_value agtv;
    bool needs_free = false;
    bool found;

    Assert(value != NULL);
    Assert(isnull != NULL);

    *value = NULL;
    *isnull = false;

    if (node == NULL || !IsA(node, Const))
        return false;

    const_arg = (Const *)node;
    if (const_arg->constisnull)
    {
        *isnull = true;
        return true;
    }
    if (const_arg->consttype != AGTYPEOID)
        return false;

    agt = DATUM_GET_AGTYPE_P(const_arg->constvalue);
    if (!AGTYPE_CONTAINER_IS_SCALAR(&agt->root))
        return false;

    found = get_ith_agtype_value_from_container_no_copy(&agt->root, 0, &agtv,
                                                        &needs_free);
    if (!found || agtv.type == AGTV_NULL)
    {
        if (needs_free)
            pfree_agtype_value_content(&agtv);
        *isnull = true;
        return true;
    }
    if (agtv.type != AGTV_STRING)
    {
        if (needs_free)
            pfree_agtype_value_content(&agtv);
        return false;
    }

    *value = pnstrdup(agtv.val.string.val, agtv.val.string.len);
    if (needs_free)
        pfree_agtype_value_content(&agtv);

    return true;
}

static Const *make_int8_const(int64 value)
{
    return makeConst(INT8OID, -1, InvalidOid, sizeof(int64),
                     Int64GetDatum(value), false, FLOAT8PASSBYVAL);
}

static Const *make_text_const(const char *value)
{
    if (value == NULL)
    {
        return makeConst(TEXTOID, -1, InvalidOid, -1, (Datum)0, true,
                         false);
    }

    return makeConst(TEXTOID, -1, DEFAULT_COLLATION_OID, -1,
                     CStringGetTextDatum(value), false, false);
}

static Const *make_agtype_const(agtype *value)
{
    if (value == NULL)
    {
        return makeConst(AGTYPEOID, -1, InvalidOid, -1, (Datum)0, true,
                         false);
    }

    return makeConst(AGTYPEOID, -1, InvalidOid, -1, AGTYPE_P_GET_DATUM(value),
                     false, false);
}

static void adjust_adjacency_match_join_rows(RelOptInfo *joinrel,
                                             JoinType jointype)
{
    ListCell *lc;

    if (joinrel == NULL ||
        joinrel->pathlist == NIL)
    {
        return;
    }

    foreach(lc, joinrel->pathlist)
    {
        Path *path = lfirst(lc);
        JoinPath *joinpath;
        Path *outer_path;
        Path *inner_path;
        double child_rows;
        double adjusted_rows;
        bool outer_has_adjacency;
        bool inner_has_adjacency;

        if (!IsA(path, NestPath))
            continue;

        joinpath = (JoinPath *)path;
        outer_path = joinpath->outerjoinpath;
        inner_path = joinpath->innerjoinpath;

        outer_has_adjacency = path_contains_adjacency_match(outer_path);
        inner_has_adjacency = path_contains_adjacency_match(inner_path);

        if (outer_path == NULL ||
            inner_path == NULL ||
            (!outer_has_adjacency && !inner_has_adjacency))
        {
            continue;
        }

        /*
         * Do not mutate shared joinrel or ParamPathInfo estimates here.  This
         * only tightens the opt-in NestPath that contains the adjacency
         * CustomPath after core join path creation has finished.
         */
        child_rows = outer_path->rows * inner_path->rows;
        adjusted_rows = clamp_row_est(Max(child_rows, 1.0));

        if (outer_has_adjacency &&
            !inner_has_adjacency &&
            outer_path->parent != NULL &&
            path_required_outer_is_subset(inner_path, outer_path->parent->relids))
        {
            adjusted_rows = Min(adjusted_rows,
                                clamp_row_est(Max(outer_path->rows, 1.0)));
        }
        else if (inner_has_adjacency &&
                 !outer_has_adjacency &&
                 inner_path->parent != NULL &&
                 path_required_outer_is_subset(outer_path,
                                               inner_path->parent->relids))
        {
            adjusted_rows = Min(adjusted_rows,
                                clamp_row_est(Max(inner_path->rows, 1.0)));
        }

        if (adjusted_rows >= path->rows)
            continue;

        ereport(DEBUG2,
                (errmsg_internal("AGE adjacency MATCH join rows adjusted: "
                                 "jointype=%d joinrelids=%s old_rows=%.0f "
                                 "new_rows=%.0f outer_rows=%.0f "
                                 "inner_rows=%.0f",
                                 (int)jointype,
                                 bmsToString(joinrel->relids),
                                 path->rows,
                                 adjusted_rows,
                                 outer_path->rows,
                                 inner_path->rows)));

        path->rows = adjusted_rows;
    }
}

static bool path_required_outer_is_subset(Path *path, Relids relids)
{
    Relids required_outer;

    if (path == NULL ||
        relids == NULL)
    {
        return false;
    }

    required_outer = PATH_REQ_OUTER(path);

    return !bms_is_empty(required_outer) &&
           bms_is_subset(required_outer, relids);
}

static bool path_contains_adjacency_match(Path *path)
{
    if (path == NULL)
        return false;

    if (IsA(path, CustomPath))
    {
        CustomPath *custom_path = (CustomPath *)path;

        return custom_path->methods == &age_adjacency_match_path_methods;
    }

    if (IsA(path, NestPath) ||
        IsA(path, MergePath) ||
        IsA(path, HashPath))
    {
        JoinPath *join_path = (JoinPath *)path;

        return path_contains_adjacency_match(join_path->outerjoinpath) ||
               path_contains_adjacency_match(join_path->innerjoinpath);
    }

    if (IsA(path, MaterialPath))
        return path_contains_adjacency_match(((MaterialPath *)path)->subpath);

    if (IsA(path, MemoizePath))
        return path_contains_adjacency_match(((MemoizePath *)path)->subpath);

    if (IsA(path, ProjectionPath))
        return path_contains_adjacency_match(((ProjectionPath *)path)->subpath);

    if (IsA(path, SubqueryScanPath))
        return path_contains_adjacency_match(((SubqueryScanPath *)path)->subpath);

    return false;
}

static const char *path_param_info_string(Path *path)
{
    if (path == NULL ||
        path->param_info == NULL)
    {
        return "<none>";
    }

    return bmsToString(PATH_REQ_OUTER(path));
}

static CypherAdjacencyMatchCandidate *pop_adjacency_match_candidate(
    PlannerInfo *root, RangeTblEntry *rte)
{
    ListCell *lc;
    const char *aliasname = NULL;

    if (rte == NULL ||
        rte->rtekind != RTE_RELATION ||
        adjacency_match_candidates == NIL)
    {
        return NULL;
    }

    if (rte->alias != NULL)
        aliasname = rte->alias->aliasname;
    else if (rte->eref != NULL)
        aliasname = rte->eref->aliasname;

    foreach(lc, adjacency_match_candidates)
    {
        CypherAdjacencyMatchCandidate *candidate = lfirst(lc);

        if (candidate->edge_label_oid == rte->relid &&
            aliasname != NULL &&
            strcmp(candidate->edge_alias, aliasname) == 0)
        {
            adjacency_match_candidates =
                list_delete_cell(adjacency_match_candidates, lc);
            bind_adjacency_match_candidate_outer_relids(root, candidate);
            return candidate;
        }
    }

    return NULL;
}

static void bind_adjacency_match_candidate_outer_relids(
    PlannerInfo *root, CypherAdjacencyMatchCandidate *candidate)
{
    ListCell *lc;
    Index rti = 1;
    Index alias_rti = 0;
    Relids expr_relids = NULL;
    int expr_rti = 0;

    if (root == NULL ||
        root->parse == NULL ||
        candidate == NULL)
    {
        return;
    }

    if (candidate->bound_endpoint_alias != NULL)
    {
        foreach(lc, root->parse->rtable)
        {
            RangeTblEntry *rte = lfirst(lc);
            const char *aliasname = NULL;

            if (rte->alias != NULL)
                aliasname = rte->alias->aliasname;
            else if (rte->eref != NULL)
                aliasname = rte->eref->aliasname;

            if (aliasname != NULL &&
                strcmp(candidate->bound_endpoint_alias, aliasname) == 0)
            {
                alias_rti = rti;
                break;
            }

            rti++;
        }
    }

    if (candidate->bound_endpoint_expr != NULL)
    {
        expr_relids = pull_varnos(root, candidate->bound_endpoint_expr);
        if (!bms_is_empty(expr_relids))
        {
            if (bms_membership(expr_relids) == BMS_SINGLETON)
                expr_rti = bms_singleton_member(expr_relids);

            if (expr_rti > 0 &&
                expr_rti < root->simple_rel_array_size &&
                root->simple_rel_array[expr_rti] != NULL)
            {
                candidate->required_outer = expr_relids;
                candidate->bound_endpoint_rti = expr_rti;
                return;
            }
        }
    }

    if (alias_rti == 0)
        return;

    if (expr_rti > 0 &&
        alias_rti != expr_rti &&
        candidate->bound_endpoint_expr != NULL)
    {
        OffsetVarNodes(candidate->bound_endpoint_expr,
                       (int)alias_rti - expr_rti, 0);
    }

    candidate->bound_endpoint_rti = alias_rti;
    candidate->required_outer = bms_make_singleton(alias_rti);
}

static void add_adjacency_match_custom_path(
    PlannerInfo *root, RelOptInfo *rel,
    CypherAdjacencyMatchCandidate *candidate)
{
    CustomPath *cp;
    Expr *key_expr;
    Const *endpoint_const;

    if (!age_enable_adjacency_match_custom_path ||
        candidate == NULL ||
        candidate->bound_endpoint_expr == NULL ||
        candidate->required_outer == NULL ||
        candidate->has_edge_variable_projection ||
        candidate->has_edge_property_predicate ||
        candidate->has_right_property_predicate ||
        adjacency_match_bound_expr_uses_age_id(candidate->bound_endpoint_expr))
    {
        return;
    }

    key_expr = (Expr *)candidate->bound_endpoint_expr;
    endpoint_const = find_endpoint_graphid_const(root, candidate);
    if (endpoint_const != NULL)
        key_expr = (Expr *)endpoint_const;

    cp = makeNode(CustomPath);
    cp->path.pathtype = T_CustomScan;
    cp->path.parent = rel;
    cp->path.pathtarget = rel->reltarget;
    cp->path.param_info =
        get_baserel_parampathinfo(root, rel, candidate->required_outer);
    cp->path.parallel_aware = false;
    cp->path.parallel_safe = false;
    cp->path.parallel_workers = 0;
    cp->path.pathkeys = NIL;

    cost_adjacency_match_custom_path(root, rel, cp, candidate);
    cp->flags = CUSTOMPATH_SUPPORT_PROJECTION;
    cp->custom_paths = NIL;
    cp->custom_private = list_make3(
        key_expr,
        makeConst(OIDOID, -1, InvalidOid, sizeof(Oid),
                  ObjectIdGetDatum(candidate->index_oid), false, true),
        makeConst(BOOLOID, -1, InvalidOid, sizeof(bool),
                  BoolGetDatum(candidate->outgoing), false, true));
    cp->methods = &age_adjacency_match_path_methods;

    add_path(rel, (Path *)cp);

    ereport(DEBUG2,
            (errmsg_internal("AGE adjacency MATCH CustomPath added: "
                             "edge_rel=%u index=%u required_outer=%s "
                             "rows=%.0f total_cost=%.2f",
                             candidate->edge_label_oid,
                             candidate->index_oid,
                             bmsToString(candidate->required_outer),
                             cp->path.rows,
                             cp->path.total_cost)));
}

static Const *find_endpoint_graphid_const(PlannerInfo *root,
                                          CypherAdjacencyMatchCandidate *candidate)
{
    RelOptInfo *endpoint_rel;
    ListCell *lc;

    if (candidate->bound_endpoint_rti <= 0 ||
        candidate->bound_endpoint_rti >= root->simple_rel_array_size)
        return NULL;

    endpoint_rel = root->simple_rel_array[candidate->bound_endpoint_rti];
    if (endpoint_rel == NULL)
        return NULL;

    /*
     * When a previous-clause vertex has id(vertex) = const, the bound endpoint
     * expression can still refer to a hidden raw target Var.  Use the base
     * relation's id restriction as the CustomScan key so executor setrefs do
     * not evaluate that hidden Var against the wrong slot.
     */
    foreach(lc, endpoint_rel->baserestrictinfo)
    {
        RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc);
        OpExpr *op;
        Node *left;
        Node *right;

        if (!IsA(rinfo->clause, OpExpr))
            continue;

        op = castNode(OpExpr, rinfo->clause);
        if (list_length(op->args) != 2)
            continue;

        left = linitial(op->args);
        right = lsecond(op->args);

        if (IsA(left, Var) && IsA(right, Const))
        {
            Var *var = castNode(Var, left);
            Const *con = castNode(Const, right);

            if (var->varno == candidate->bound_endpoint_rti &&
                var->varattno == Anum_ag_label_vertex_table_id &&
                var->vartype == GRAPHIDOID &&
                con->consttype == GRAPHIDOID &&
                !con->constisnull)
                return copyObject(con);
        }
        else if (IsA(left, Const) && IsA(right, Var))
        {
            Const *con = castNode(Const, left);
            Var *var = castNode(Var, right);

            if (var->varno == candidate->bound_endpoint_rti &&
                var->varattno == Anum_ag_label_vertex_table_id &&
                var->vartype == GRAPHIDOID &&
                con->consttype == GRAPHIDOID &&
                !con->constisnull)
                return copyObject(con);
        }
    }

    return NULL;
}

static bool adjacency_match_bound_expr_uses_age_id(Node *node)
{
    bool found = false;

    (void)adjacency_match_bound_expr_uses_age_id_walker(node, &found);

    return found;
}

static bool adjacency_match_bound_expr_uses_age_id_walker(Node *node,
                                                          void *context)
{
    bool *found = context;

    if (node == NULL || *found)
        return false;

    if (IsA(node, FuncExpr))
    {
        FuncExpr *func = (FuncExpr *)node;
        Oid age_id_oid = get_ag_func_oid("age_id", 1, AGTYPEOID);

        if (func->funcid == age_id_oid)
        {
            *found = true;
            return false;
        }
    }

    return expression_tree_walker(node,
                                  adjacency_match_bound_expr_uses_age_id_walker,
                                  context);
}

static void cost_adjacency_match_custom_path(PlannerInfo *root,
                                             RelOptInfo *rel,
                                             CustomPath *cp,
                                             CypherAdjacencyMatchCandidate *candidate)
{
    double rows;
    VLESourceFanoutEvidence source_evidence;
    double endpoint_fanout;
    double pages;
    double estimated_payload_rows;
    double page_probe_cost;
    double heap_recheck_cost;
    double cpu_cost;
    Cost random_page_cost;
    Cost local_seq_page_cost;

    rows = cp->path.param_info != NULL ? cp->path.param_info->ppi_rows :
                                         rel->rows;
    rows = clamp_row_est(rows);
    if (rows < 1)
        rows = 1;

    pages = rel->pages > 0 ? rel->pages : 1;
    estimate_vle_source_fanout_evidence(&source_evidence,
                                        candidate->edge_label_oid);
    if (source_evidence.reltuples <= 0 && rel->tuples > 0)
        source_evidence.reltuples = rel->tuples;
    endpoint_fanout = select_vle_source_fanout_for_endpoint(
        &source_evidence, candidate->endpoint_attno);
    if (endpoint_fanout <= 0)
        endpoint_fanout = Min(rows, 8.0);

    rows = clamp_row_est(Min(rows, endpoint_fanout));
    if (rows < 1)
        rows = 1;
    estimated_payload_rows = rows;

    get_tablespace_page_costs(rel->reltablespace, &random_page_cost,
                              &local_seq_page_cost);

    /*
     * age_adjacency v3 lookup performs a small directory probe and then scans
     * the endpoint posting run. Charge a bounded page probe plus heap
     * visibility rechecks for the estimated payload rows.
     */
    page_probe_cost = Min(pages, 4.0) * random_page_cost * 0.03;
    heap_recheck_cost = estimated_payload_rows * random_page_cost * 0.02;
    cpu_cost = estimated_payload_rows * cpu_tuple_cost * 2.0;

    cp->path.rows = rows;
    cp->path.startup_cost = page_probe_cost;
    cp->path.total_cost = cp->path.startup_cost + heap_recheck_cost + cpu_cost;

    (void) root;
    (void) local_seq_page_cost;
}

static Plan *plan_age_adjacency_match_path(PlannerInfo *root,
                                           RelOptInfo *rel,
                                           CustomPath *best_path,
                                           List *tlist, List *clauses,
                                           List *custom_plans)
{
    CustomScan *cs;
    List *custom_scan_tlist;

    (void) root;
    (void) custom_plans;

    custom_scan_tlist = build_adjacency_match_custom_scan_tlist(
        rel->relid, tlist, clauses);

    cs = makeNode(CustomScan);

    cs->scan.plan.startup_cost = best_path->path.startup_cost;
    cs->scan.plan.total_cost = best_path->path.total_cost;
    cs->scan.plan.plan_rows = best_path->path.rows;
    cs->scan.plan.plan_width = rel->reltarget->width;
    cs->scan.plan.parallel_aware = false;
    cs->scan.plan.parallel_safe = false;
    cs->scan.plan.async_capable = false;
    cs->scan.plan.targetlist = tlist;
    cs->scan.plan.qual = extract_actual_clauses(clauses, false);
    cs->scan.plan.lefttree = NULL;
    cs->scan.plan.righttree = NULL;
    cs->scan.scanrelid = rel->relid;

    cs->flags = best_path->flags;
    cs->custom_plans = NIL;
    cs->custom_exprs = list_make1(linitial(best_path->custom_private));
    cs->custom_private = list_copy_tail(best_path->custom_private, 1);
    cs->custom_scan_tlist = custom_scan_tlist;
    cs->custom_relids = bms_make_singleton(rel->relid);
    cs->methods = &age_adjacency_match_scan_methods;

    return (Plan *)cs;
}

typedef struct AdjacencyMatchScanVarContext
{
    Index relid;
    Bitmapset *attrs;
    bool unsupported;
} AdjacencyMatchScanVarContext;

static List *build_adjacency_match_custom_scan_tlist(Index relid,
                                                     List *tlist,
                                                     List *clauses)
{
    AdjacencyMatchScanVarContext context;
    List *custom_tlist = NIL;
    int attno;
    int resno = 1;

    context.relid = relid;
    context.attrs = NULL;
    context.unsupported = false;

    collect_adjacency_match_scan_vars_from_list(tlist, &context);
    collect_adjacency_match_scan_vars_from_list(clauses, &context);

    if (context.unsupported || context.attrs == NULL)
        return NIL;

    attno = -1;
    while ((attno = bms_next_member(context.attrs, attno)) >= 0)
    {
        Var *var;
        TargetEntry *tle;
        Oid vartype;

        vartype = adjacency_match_attr_type((AttrNumber)attno);
        if (!OidIsValid(vartype))
            return NIL;

        var = makeVar(relid, (AttrNumber)attno, vartype, -1, InvalidOid, 0);
        tle = makeTargetEntry((Expr *)var, resno++,
                              pstrdup(adjacency_match_attr_name(
                                  (AttrNumber)attno)),
                              false);
        custom_tlist = lappend(custom_tlist, tle);
    }

    return custom_tlist;
}

static void collect_adjacency_match_scan_vars_from_list(List *nodes,
                                                        void *context)
{
    ListCell *lc;

    foreach(lc, nodes)
    {
        Node *node = lfirst(lc);

        if (node == NULL)
            continue;

        if (IsA(node, TargetEntry))
            node = (Node *)((TargetEntry *)node)->expr;
        else if (IsA(node, RestrictInfo))
            node = (Node *)((RestrictInfo *)node)->clause;

        collect_adjacency_match_scan_vars(node, context);
    }
}

static bool collect_adjacency_match_scan_vars(Node *node, void *context)
{
    AdjacencyMatchScanVarContext *var_context =
        (AdjacencyMatchScanVarContext *)context;

    if (node == NULL || var_context->unsupported)
        return false;

    if (IsA(node, Var))
    {
        Var *var = (Var *)node;

        if (var->varno != var_context->relid)
            return false;

        if (var->varattno < Anum_ag_label_edge_table_id ||
            var->varattno > Anum_ag_label_edge_table_properties)
        {
            var_context->unsupported = true;
            return false;
        }

        var_context->attrs =
            bms_add_member(var_context->attrs, var->varattno);
        return false;
    }

    return expression_tree_walker(node, collect_adjacency_match_scan_vars,
                                  context);
}

static const char *adjacency_match_attr_name(AttrNumber attno)
{
    switch (attno)
    {
    case Anum_ag_label_edge_table_id:
        return "id";
    case Anum_ag_label_edge_table_start_id:
        return "start_id";
    case Anum_ag_label_edge_table_end_id:
        return "end_id";
    case Anum_ag_label_edge_table_properties:
        return "properties";
    default:
        elog(ERROR, "unexpected AGE adjacency match attr %d", attno);
    }
}

static Oid adjacency_match_attr_type(AttrNumber attno)
{
    switch (attno)
    {
    case Anum_ag_label_edge_table_id:
    case Anum_ag_label_edge_table_start_id:
    case Anum_ag_label_edge_table_end_id:
        return GRAPHIDOID;
    case Anum_ag_label_edge_table_properties:
        return AGTYPEOID;
    default:
        return InvalidOid;
    }
}

/*
 * Check to see if the rte is a Cypher clause. An rte is only a Cypher clause
 * if it is a subquery, with the last entry in its target list, that is a
 * FuncExpr.
 */
static cypher_clause_kind get_cypher_clause_kind(RangeTblEntry *rte)
{
    TargetEntry *te;
    FuncExpr *fe;

    /* If it's not a subquery, it's not a Cypher clause. */
    if (rte->rtekind != RTE_SUBQUERY)
        return CYPHER_CLAUSE_NONE;

    /* Make sure the targetList isn't NULL. NULL means potential EXIST subclause */
    if (rte->subquery->targetList == NULL)
        return CYPHER_CLAUSE_NONE;

    /* A Cypher clause function is always the last entry. */
    te = llast(rte->subquery->targetList);

    /* If the last entry is not a FuncExpr, it's not a Cypher clause. */
    if (!IsA(te->expr, FuncExpr))
        return CYPHER_CLAUSE_NONE;

    fe = (FuncExpr *)te->expr;

    load_cypher_clause_function_oids();

    if (fe->funcid == cypher_create_clause_func_oid)
        return CYPHER_CLAUSE_CREATE;
    if (fe->funcid == cypher_set_clause_func_oid)
        return CYPHER_CLAUSE_SET;
    if (fe->funcid == cypher_delete_clause_func_oid)
        return CYPHER_CLAUSE_DELETE;
    if (fe->funcid == cypher_merge_clause_func_oid)
        return CYPHER_CLAUSE_MERGE;
    else
        return CYPHER_CLAUSE_NONE;
}

static void register_cypher_clause_function_oid_callbacks(void)
{
    if (!cypher_clause_func_callback_registered)
    {
        CacheRegisterSyscacheCallback(PROCOID,
                                      invalidate_cypher_clause_function_oids,
                                      (Datum)0);
        CacheRegisterSyscacheCallback(PROCNAMEARGSNSP,
                                      invalidate_cypher_clause_function_oids,
                                      (Datum)0);
        cypher_clause_func_callback_registered = true;
    }
}

static void load_cypher_clause_function_oids(void)
{
    register_cypher_clause_function_oid_callbacks();

    if (!OidIsValid(cypher_create_clause_func_oid))
    {
        cypher_create_clause_func_oid =
            get_ag_func_oid(CREATE_CLAUSE_FUNCTION_NAME, 1, INTERNALOID);
    }

    if (!OidIsValid(cypher_set_clause_func_oid))
    {
        cypher_set_clause_func_oid =
            get_ag_func_oid(SET_CLAUSE_FUNCTION_NAME, 1, INTERNALOID);
    }

    if (!OidIsValid(cypher_delete_clause_func_oid))
    {
        cypher_delete_clause_func_oid =
            get_ag_func_oid(DELETE_CLAUSE_FUNCTION_NAME, 1, INTERNALOID);
    }

    if (!OidIsValid(cypher_merge_clause_func_oid))
    {
        cypher_merge_clause_func_oid =
            get_ag_func_oid(MERGE_CLAUSE_FUNCTION_NAME, 1, INTERNALOID);
    }
}

static void invalidate_cypher_clause_function_oids(Datum arg, int cache_id,
                                                   uint32 hash_value)
{
    cypher_create_clause_func_oid = InvalidOid;
    cypher_set_clause_func_oid = InvalidOid;
    cypher_delete_clause_func_oid = InvalidOid;
    cypher_merge_clause_func_oid = InvalidOid;
    cypher_property_path_invalidate_oids();
    cypher_property_signature_invalidate_oids();
}

static void replace_with_cypher_dml_path(PlannerInfo *root, RelOptInfo *rel,
                                         RangeTblEntry *rte,
                                         cypher_path_factory factory)
{
    TargetEntry *te;
    FuncExpr *fe;
    List *custom_private;
    CustomPath *cp;

    /* Add the pattern to the CustomPath */
    te = (TargetEntry *)llast(rte->subquery->targetList);
    fe = (FuncExpr *)te->expr;
    /* pass the const that holds the data structure to the path. */
    custom_private = fe->args;

    cp = factory(root, rel, custom_private);

    /* Discard any preexisting paths */
    rel->pathlist = NIL;
    rel->partial_pathlist = NIL;

    add_path(rel, (Path *)cp);
}
