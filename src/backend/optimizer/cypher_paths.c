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
#include "catalog/namespace.h"
#include "catalog/pg_statistic.h"
#include "catalog/pg_type_d.h"
#include "catalog/ag_namespace.h"
#include "catalog/ag_label.h"
#include "executor/cypher_adjacency_match.h"
#include "executor/cypher_property_projection.h"
#include "executor/cypher_vle_stream.h"
#include "nodes/makefuncs.h"
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
#include "utils/array.h"
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
#include "utils/agtype.h"
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
static Oid agtype_eq_func_oid = InvalidOid;
static Oid agtype_lt_func_oid = InvalidOid;
static Oid agtype_le_func_oid = InvalidOid;
static Oid agtype_gt_func_oid = InvalidOid;
static Oid agtype_ge_func_oid = InvalidOid;
static Oid count_any_agg_func_oid = InvalidOid;
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
static double estimate_adjacency_match_endpoint_fanout(
    RelOptInfo *rel, CypherAdjacencyMatchCandidate *candidate);
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
static bool detect_simple_property_projection(PlannerInfo *root,
                                              RelOptInfo *input_rel,
                                              RelOptInfo *output_rel,
                                              CypherCachedPropertySlotDescriptor *slot,
                                              const char **target_source);
static bool is_simple_property_access_target(
    Node *node, CypherCachedPropertySlotDescriptor *slot);
static bool rewrite_count_property_access_pathtarget(PathTarget *target);
static bool rewrite_property_access_aggregate_expr(Node *node);
static bool rewrite_property_access_aggregate_walker(Node *node,
                                                     void *context);
static void add_narrow_distinct_collect_paths(PlannerInfo *root,
                                             RelOptInfo *output_rel);
static Path *try_narrow_distinct_collect_path(PlannerInfo *root,
                                             RelOptInfo *output_rel,
                                             Path *path,
                                             CypherTypedCollectHandoff *handoff,
                                             CypherCachedPropertySlotDescriptor *slot);
static bool pathtarget_contains_only_expr(PathTarget *target, Node *expr);
static bool rewrite_count_property_access_expr(Node *node);
static bool is_count_any_aggref(Aggref *aggref);
static bool extract_count_property_access_args(Aggref *aggref, Node **object,
                                               Node **key);
static bool same_property_source(Node *left, Node *right);
static Oid get_cached_count_any_agg_oid(void);
static Oid get_cached_agtype_field_exists_nonnull_oid(void);
static Oid get_cached_agtype_field_equals_oid(void);
static Oid get_cached_agtype_field_cmp_oid(void);
static Oid get_cached_agtype_eq_oid(void);
static Oid get_cached_agtype_cmp_func_oid(const char *name, Oid *cache);
static bool rewrite_property_equals_restrictions(PlannerInfo *root,
                                                 RelOptInfo *rel, Index rti);
static Node *replace_property_index_side(OpExpr *op, bool replace_left,
                                         Node *index_expr);
static Node *rewrite_property_index_surface_mutator(Node *node, void *context);
static void canonicalize_property_index_predicates(RelOptInfo *rel);
static void canonicalize_property_index_restrictions(RelOptInfo *rel);
static bool collect_canonical_property_index_exprs_walker(Node *node,
                                                          void *context);
static Node *canonicalize_property_index_exprs_mutator(Node *node,
                                                       void *context);
static List *collect_canonical_property_index_exprs(RelOptInfo *rel);
static bool property_expr_list_contains(List *exprs, Node *expr);
static Node *try_rewrite_property_equals_clause(Node *clause, Index rti,
                                                RelOptInfo *rel);
static bool match_property_access_expr(Node *node, Index rti,
                                       Node **properties, Node **key);
static bool property_object_belongs_to_rti(Node *node, Index rti);
static Expr *make_int4_zero_compare_expr(FuncExpr *cmp_expr,
                                         const char *operator_name);
static const char *property_compare_operator_name(Oid opfuncid,
                                                  bool commuted);
static void add_property_projection_custom_path(PlannerInfo *root,
                                                RelOptInfo *input_rel,
                                                RelOptInfo *output_rel,
                                                CypherCachedPropertySlotDescriptor *slot,
                                                FinalPathExtraData *extra);
static Const *make_oid_const(Oid value);
static bool detect_ordered_property_projection_delay(PlannerInfo *root,
                                                     Node **properties,
                                                     List **keys,
                                                     bool *reuse_sort_output,
                                                     TargetEntry **sort_tle);
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
static bool build_count_agtype_deferred_targets(PlannerInfo *root,
                                                PathTarget **lower_target,
                                                PathTarget **final_target);
static bool is_age_vle_function_rte(RangeTblEntry *rte, FuncExpr **func_expr);
static void add_age_vle_stream_custom_path(PlannerInfo *root, RelOptInfo *rel,
                                           RangeTblEntry *rte,
                                           FuncExpr *func_expr);
static Plan *plan_age_vle_stream_path(PlannerInfo *root, RelOptInfo *rel,
                                      CustomPath *best_path, List *tlist,
                                      List *clauses, List *custom_plans);
static Plan *plan_age_property_projection_path(PlannerInfo *root,
                                               RelOptInfo *rel,
                                               CustomPath *best_path,
                                               List *tlist, List *clauses,
                                               List *custom_plans);
static Oid agtype_field_exists_nonnull_func_oid = InvalidOid;
static Oid agtype_field_equals_func_oid = InvalidOid;
static Oid agtype_field_cmp_func_oid = InvalidOid;

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
            &index_handoff) &&
        !equal(index_handoff.index_expr, left))
    {
        return replace_property_index_side(op, true,
                                           index_handoff.index_expr);
    }

    if (cypher_find_matching_property_index_handoff_for_rte(
            surface_context->rte, surface_context->rti, right,
            &index_handoff) &&
        !equal(index_handoff.index_expr, right))
    {
        return replace_property_index_side(op, false,
                                           index_handoff.index_expr);
    }

    return expression_tree_mutator(node, rewrite_property_index_surface_mutator,
                                   context);
}

static void set_rel_pathlist(PlannerInfo *root, RelOptInfo *rel, Index rti,
                             RangeTblEntry *rte)
{
    CypherAdjacencyMatchCandidate *candidate;
    FuncExpr *vle_func_expr = NULL;

    if (prev_set_rel_pathlist_hook)
        prev_set_rel_pathlist_hook(root, rel, rti, rte);

    if (rewrite_property_equals_restrictions(root, rel, rti))
    {
        canonicalize_property_index_predicates(rel);
        check_index_predicates(root, rel);
        canonicalize_property_index_restrictions(rel);
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

    if (is_age_vle_function_rte(rte, &vle_func_expr))
    {
        add_age_vle_stream_custom_path(root, rel, rte, vle_func_expr);
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
    CypherCachedPropertySlotDescriptor property_slot;
    const char *target_source = NULL;

    if (prev_create_upper_paths_hook != NULL)
        prev_create_upper_paths_hook(root, stage, input_rel, output_rel, extra);

    if (stage == UPPERREL_GROUP_AGG)
    {
        if (rewrite_count_property_access_pathtarget(output_rel->reltarget))
        {
            ereport(DEBUG2,
                    (errmsg_internal("AGE property access aggregate rewritten")));
        }
        add_narrow_distinct_collect_paths(root, output_rel);
        return;
    }

    if (stage != UPPERREL_FINAL)
        return;

    add_deferred_count_agtype_projection_path(root, output_rel,
                                             (FinalPathExtraData *)extra);
    add_deferred_ordered_property_projection_path(root, input_rel, output_rel,
                                                 (FinalPathExtraData *)extra);

    if (detect_simple_property_projection(root, input_rel, output_rel,
                                          &property_slot, &target_source))
    {
        ereport(DEBUG2,
                (errmsg_internal("AGE simple property projection visible after "
                                 "scan/join target: input_paths=%d "
                                 "output_paths=%d target_source=%s "
                                 "value_type=%u field_result_type=%u",
                                 list_length(input_rel->pathlist),
                                 list_length(output_rel->pathlist),
                                 target_source,
                                 property_slot.value_type,
                                 property_slot.field_result_type)));
        add_property_projection_custom_path(root, input_rel, output_rel,
                                            &property_slot,
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

static bool detect_simple_property_projection(PlannerInfo *root,
                                              RelOptInfo *input_rel,
                                              RelOptInfo *output_rel,
                                              CypherCachedPropertySlotDescriptor *slot,
                                              const char **target_source)
{
    Node *expr;

    (void)root;

    if (input_rel != NULL &&
        input_rel->reltarget != NULL &&
        list_length(input_rel->reltarget->exprs) == 1)
    {
        expr = linitial(input_rel->reltarget->exprs);
        if (is_simple_property_access_target(expr, slot))
        {
            *target_source = "input_rel";
            return true;
        }
    }

    if (output_rel != NULL &&
        output_rel->reltarget != NULL &&
        list_length(output_rel->reltarget->exprs) == 1)
    {
        expr = linitial(output_rel->reltarget->exprs);
        if (is_simple_property_access_target(expr, slot))
        {
            *target_source = "output_rel";
            return true;
        }
    }

    return false;
}

static bool is_simple_property_access_target(
    Node *node, CypherCachedPropertySlotDescriptor *slot)
{
    CypherPropertyHandoffDescriptor handoff;
    CypherCachedPropertySlotDescriptor candidate;
    Var *properties_var;
    Const *key_const;

    if (node == NULL || slot == NULL)
        return false;

    memset(&handoff, 0, sizeof(handoff));
    if (!cypher_extract_property_access_signature(
            node, &handoff.property_signature) ||
        !cypher_make_cached_property_slot_descriptor(&handoff, &candidate))
        return false;

    if (list_length(candidate.keys) != 1 ||
        !IsA(candidate.container, Var) ||
        !IsA(linitial(candidate.keys), Const))
        return false;

    if (candidate.field_result_type != AGTYPEOID &&
        candidate.field_result_type != INT8OID &&
        candidate.field_result_type != FLOAT8OID &&
        candidate.field_result_type != NUMERICOID &&
        candidate.field_result_type != TEXTOID)
    {
        return false;
    }

    properties_var = castNode(Var, candidate.container);
    key_const = linitial_node(Const, candidate.keys);

    if (properties_var->varattno != Anum_ag_label_vertex_table_properties ||
        key_const->constisnull ||
        key_const->consttype != AGTYPEOID)
    {
        return false;
    }

    *slot = candidate;
    return true;
}

static bool detect_ordered_property_projection_delay(PlannerInfo *root,
                                                     Node **properties,
                                                     List **keys,
                                                     bool *reuse_sort_output,
                                                     TargetEntry **sort_tle)
{
    TargetEntry *output_tle = NULL;
    SortGroupClause *sort_clause;
    CypherPropertyAccessSignature output_signature;
    CypherPropertyAccessSignature sort_signature;
    ListCell *lc;

    if (root == NULL || root->parse == NULL ||
        root->parse->sortClause == NIL ||
        root->parse->distinctClause != NIL ||
        list_length(root->parse->sortClause) != 1 ||
        root->parse->limitCount == NULL)
    {
        return false;
    }

    foreach(lc, root->parse->targetList)
    {
        TargetEntry *tle = lfirst_node(TargetEntry, lc);

        if (tle->resjunk)
            continue;

        if (output_tle != NULL)
            return false;

        output_tle = tle;
    }

    if (output_tle == NULL ||
        !cypher_extract_property_access_signature((Node *)output_tle->expr,
                                                  &output_signature))
    {
        return false;
    }

    sort_clause = linitial_node(SortGroupClause, root->parse->sortClause);
    *sort_tle = get_sortgroupref_tle(sort_clause->tleSortGroupRef,
                                     root->parse->targetList);

    if (*sort_tle == NULL ||
        (!(*sort_tle)->resjunk && *sort_tle != output_tle) ||
        !cypher_extract_property_access_signature((Node *)(*sort_tle)->expr,
                                                  &sort_signature) ||
        sort_signature.value_type == AGTYPEOID)
    {
        return false;
    }

    if (!same_property_source(output_signature.container,
                              sort_signature.container) ||
        !equal(output_signature.keys, sort_signature.keys))
    {
        return false;
    }

    *reuse_sort_output =
        output_signature.value_type == sort_signature.value_type &&
        output_signature.field_result_type == sort_signature.field_result_type;
    if (output_signature.value_type != AGTYPEOID && !*reuse_sort_output)
        return false;

    *properties = output_signature.container;
    *keys = output_signature.keys;
    return true;
}

static void add_deferred_ordered_property_projection_path(
    PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel,
    FinalPathExtraData *extra)
{
    Node *properties = NULL;
    List *keys = NIL;
    TargetEntry *sort_tle = NULL;
    PathTarget *lower_target;
    PathTarget *final_target;
    Var *properties_var;
    Var *lookup_var;
    Node *final_output_expr;
    Oid relid;
    AttrNumber properties_attno;
    bool reuse_sort_output = false;

    if (input_rel == NULL || output_rel == NULL || extra == NULL ||
        !extra->limit_needed ||
        !detect_ordered_property_projection_delay(root, &properties,
                                                  &keys,
                                                  &reuse_sort_output,
                                                  &sort_tle) ||
        !IsA(properties, Var))
    {
        return;
    }

    properties_var = castNode(Var, properties);
    if (!reuse_sort_output &&
        (properties_var->varno <= 0 ||
         properties_var->varno >= root->simple_rel_array_size ||
         root->simple_rte_array[properties_var->varno] == NULL ||
         root->simple_rte_array[properties_var->varno]->rtekind != RTE_RELATION))
    {
        return;
    }

    if (!reuse_sort_output &&
        properties_var->varattno != Anum_ag_label_vertex_table_properties)
    {
        if (properties_var->varattno != Anum_ag_label_edge_table_properties)
            return;
    }

    properties_attno = properties_var->varattno;

    if (reuse_sort_output)
    {
        lookup_var = NULL;
        final_output_expr = (Node *)copyObject(sort_tle->expr);
    }
    else
    {
        relid = root->simple_rte_array[properties_var->varno]->relid;
        if (properties_attno == Anum_ag_label_edge_table_properties)
        {
            lookup_var = makeVar(properties_var->varno,
                                 Anum_ag_label_edge_table_id,
                                 GRAPHIDOID, -1, InvalidOid,
                                 properties_var->varlevelsup);
            final_output_expr = cypher_make_id_property_path_field_agtype_expr(
                relid, (Node *)lookup_var, properties_attno, keys);
        }
        else
        {
            lookup_var = makeVar(properties_var->varno,
                                 SelfItemPointerAttributeNumber,
                                 TIDOID, -1, InvalidOid,
                                 properties_var->varlevelsup);
            final_output_expr = cypher_make_ctid_property_path_field_agtype_expr(
                relid, (Node *)lookup_var, properties_attno, keys);
        }
    }

    if (final_output_expr == NULL)
        return;

    lower_target = create_empty_pathtarget();
    if (lookup_var != NULL)
        add_column_to_pathtarget(lower_target, (Expr *)copyObject(lookup_var),
                                 0);
    add_column_to_pathtarget(lower_target, (Expr *)copyObject(sort_tle->expr),
                             sort_tle->ressortgroupref);
    lower_target = set_pathtarget_cost_width(root, lower_target);

    final_target = create_empty_pathtarget();
    if (reuse_sort_output)
    {
        add_column_to_pathtarget(final_target, (Expr *)final_output_expr,
                                 sort_tle->ressortgroupref);
    }
    else
    {
        add_column_to_pathtarget(final_target, (Expr *)final_output_expr, 0);
        add_column_to_pathtarget(final_target,
                                 (Expr *)copyObject(sort_tle->expr),
                                 sort_tle->ressortgroupref);
    }
    final_target = set_pathtarget_cost_width(root, final_target);

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
        !build_count_agtype_deferred_targets(root, &lower_target,
                                             &final_target))
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

static bool build_count_agtype_deferred_targets(PlannerInfo *root,
                                                PathTarget **lower_target,
                                                PathTarget **final_target)
{
    ListCell *lc;
    bool found = false;

    if (root == NULL || root->processed_tlist == NIL)
        return false;

    *lower_target = create_empty_pathtarget();
    *final_target = create_empty_pathtarget();

    foreach(lc, root->processed_tlist)
    {
        TargetEntry *tle = lfirst_node(TargetEntry, lc);
        CypherScalarFinalHandoff handoff;

        if (!tle->resjunk &&
            cypher_extract_scalar_final_handoff((Node *)tle->expr, &handoff))
        {
            CypherCachedPropertySlotDescriptor slot;
            FuncExpr *final_expr;
            Expr *canonical_arg;
            Node *slot_expr = NULL;

            if (handoff.has_property_descriptor &&
                cypher_make_cached_property_slot_descriptor(
                    &handoff.property_descriptor, &slot))
            {
                slot_expr = cypher_make_cached_property_slot_expr(&slot);
            }

            handoff.scalar_expr = slot_expr != NULL ?
                slot_expr : copyObject(handoff.scalar_expr);
            canonical_arg = cypher_add_or_get_lower_scalar_handoff(
                *lower_target, &handoff, tle->ressortgroupref);
            if (canonical_arg == NULL)
                return false;

            final_expr = castNode(FuncExpr, copyObject(tle->expr));
            final_expr->args = list_make1(copyObject(canonical_arg));
            add_column_to_pathtarget(*final_target, (Expr *)final_expr,
                                     tle->ressortgroupref);
            found = true;
        }
        else
        {
            add_column_to_pathtarget(*final_target,
                                     (Expr *)copyObject(tle->expr),
                                     tle->ressortgroupref);
            add_column_to_pathtarget(*lower_target,
                                     (Expr *)copyObject(tle->expr),
                                     tle->ressortgroupref);
        }
    }

    if (!found)
        return false;

    *lower_target = set_pathtarget_cost_width(root, *lower_target);
    *final_target = set_pathtarget_cost_width(root, *final_target);

    return true;
}

static bool rewrite_count_property_access_pathtarget(PathTarget *target)
{
    ListCell *lc;
    bool rewritten = false;

    if (target == NULL)
        return false;

    foreach(lc, target->exprs)
    {
        Node *expr = lfirst(lc);

        if (rewrite_property_access_aggregate_expr(expr))
            rewritten = true;
    }

    return rewritten;
}

static bool rewrite_property_access_aggregate_expr(Node *node)
{
    bool rewritten = false;

    rewrite_property_access_aggregate_walker(node, &rewritten);

    return rewritten;
}

static bool rewrite_property_access_aggregate_walker(Node *node,
                                                     void *context)
{
    bool *rewritten = (bool *)context;

    if (node == NULL)
        return false;

    if (cypher_rewrite_collect_typed_scalar_expr(node) ||
        cypher_rewrite_collect_numeric_property_expr(node) ||
        cypher_rewrite_array_agg_property_expr(node) ||
        rewrite_count_property_access_expr(node))
    {
        *rewritten = true;
        return false;
    }

    return expression_tree_walker(node,
                                  rewrite_property_access_aggregate_walker,
                                  context);
}

static void add_narrow_distinct_collect_paths(PlannerInfo *root,
                                              RelOptInfo *output_rel)
{
    CypherTypedCollectHandoff handoff;
    CypherCachedPropertySlotDescriptor property_slot;
    CypherCachedPropertySlotDescriptor *slot = NULL;
    List *new_paths = NIL;
    ListCell *lc;

    if (root == NULL || output_rel == NULL || output_rel->pathlist == NIL)
        return;

    if (!cypher_find_typed_distinct_collect_handoff(output_rel->reltarget,
                                                   &handoff))
        return;

    if (handoff.has_property_descriptor &&
        cypher_make_cached_property_slot_descriptor(
            &handoff.property_descriptor, &property_slot))
    {
        slot = &property_slot;
    }

    foreach(lc, output_rel->pathlist)
    {
        Path *path = lfirst(lc);
        Path *new_path;

        new_path = try_narrow_distinct_collect_path(root, output_rel, path,
                                                    &handoff, slot);
        if (new_path != NULL)
            new_paths = lappend(new_paths, new_path);
    }

    foreach(lc, new_paths)
        add_path(output_rel, lfirst(lc));
}

static Path *try_narrow_distinct_collect_path(PlannerInfo *root,
                                              RelOptInfo *output_rel,
                                              Path *path,
                                              CypherTypedCollectHandoff *handoff,
                                              CypherCachedPropertySlotDescriptor *slot)
{
    AggPath *agg_path;
    PathTarget *narrow_target;
    Path *narrow_subpath;
    AggPath *new_agg;
    Node *arg;
    Node *slot_expr = NULL;

    if (path == NULL || handoff == NULL || handoff->arg_expr == NULL ||
        !IsA(path, AggPath))
        return NULL;

    if (slot != NULL)
        slot_expr = cypher_make_cached_property_slot_expr(slot);

    arg = slot_expr != NULL ? slot_expr : handoff->arg_expr;

    agg_path = (AggPath *)path;
    if (agg_path->aggstrategy != AGG_PLAIN ||
        agg_path->groupClause != NIL ||
        agg_path->qual != NIL ||
        agg_path->subpath == NULL)
    {
        return NULL;
    }

    if (pathtarget_contains_only_expr(agg_path->subpath->pathtarget, arg))
    {
        return NULL;
    }

    narrow_target = create_empty_pathtarget();
    add_column_to_pathtarget(narrow_target, (Expr *)copyObject(arg), 0);
    narrow_target = set_pathtarget_cost_width(root, narrow_target);

    narrow_subpath = copy_path_with_deferred_projection_target(
        root, agg_path->subpath, narrow_target, false);
    if (narrow_subpath == NULL)
        return NULL;

    new_agg = palloc(sizeof(*new_agg));
    memcpy(new_agg, agg_path, sizeof(*new_agg));
    new_agg->path.pathtype = T_Agg;
    new_agg->subpath = narrow_subpath;
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

static bool rewrite_count_property_access_expr(Node *node)
{
    Aggref *aggref;
    FuncExpr *exists_expr;
    TargetEntry *arg_tle;
    Node *object = NULL;
    Node *key = NULL;

    if (node == NULL || !IsA(node, Aggref))
        return false;

    aggref = castNode(Aggref, node);
    if (!is_count_any_aggref(aggref))
        return false;

    if (!extract_count_property_access_args(aggref, &object, &key))
        return false;

    exists_expr = makeFuncExpr(
        get_cached_agtype_field_exists_nonnull_oid(),
        BOOLOID,
        list_make2(copyObject(object), copyObject(key)),
        InvalidOid, InvalidOid, COERCE_EXPLICIT_CALL);
    exists_expr->location = exprLocation(object);

    arg_tle = linitial_node(TargetEntry, aggref->args);
    arg_tle->expr = (Expr *)exists_expr;
    aggref->aggargtypes = list_make1_oid(BOOLOID);

    return true;
}

static bool is_count_any_aggref(Aggref *aggref)
{
    return aggref != NULL &&
        aggref->aggfnoid == get_cached_count_any_agg_oid() &&
        !aggref->aggstar &&
        aggref->aggdirectargs == NIL &&
        aggref->aggorder == NIL &&
        aggref->aggdistinct == NIL &&
        aggref->aggfilter == NULL &&
        list_length(aggref->args) == 1;
}

static bool extract_count_property_access_args(Aggref *aggref, Node **object,
                                               Node **key)
{
    TargetEntry *arg_tle;

    arg_tle = linitial_node(TargetEntry, aggref->args);
    if (arg_tle == NULL || arg_tle->expr == NULL)
        return false;

    return cypher_extract_property_access_terminal_args((Node *)arg_tle->expr,
                                                        object, key);
}

static bool same_property_source(Node *left, Node *right)
{
    Var *left_var;
    Var *right_var;

    if (left == NULL || right == NULL)
        return false;

    if (equal(left, right))
        return true;

    if (!IsA(left, Var) || !IsA(right, Var))
        return false;

    left_var = castNode(Var, left);
    right_var = castNode(Var, right);

    return left_var->varno == right_var->varno &&
        left_var->varattno == right_var->varattno &&
        left_var->varlevelsup == right_var->varlevelsup &&
        left_var->vartype == right_var->vartype;
}

static Oid get_cached_count_any_agg_oid(void)
{
    if (!OidIsValid(count_any_agg_func_oid))
    {
        Oid argtype = ANYOID;

        count_any_agg_func_oid =
            LookupFuncName(list_make2(makeString("pg_catalog"),
                                      makeString("count")),
                           1, &argtype, false);
    }

    return count_any_agg_func_oid;
}

static void add_property_projection_custom_path(PlannerInfo *root,
                                                RelOptInfo *input_rel,
                                                RelOptInfo *output_rel,
                                                CypherCachedPropertySlotDescriptor *slot,
                                                FinalPathExtraData *extra)
{
    CustomPath *cp;
    Path *path;
    RelOptInfo *base_rel;
    RangeTblEntry *base_rte;
    Var *properties_var;
    Const *key_const;
    Index scanrelid;
    int scanrelid_int;
    Path *reference_path;
    double rows;
    double pages;
    Cost random_page_cost;
    Cost seq_page_cost;

    if (input_rel == NULL || output_rel == NULL || slot == NULL ||
        root->parse == NULL ||
        input_rel->pathlist == NIL ||
        !bms_get_singleton_member(input_rel->relids, &scanrelid_int))
    {
        return;
    }
    scanrelid = (Index)scanrelid_int;

    if (slot->container == NULL ||
        slot->keys == NIL ||
        list_length(slot->keys) != 1 ||
        !IsA(slot->container, Var) ||
        !IsA(linitial(slot->keys), Const))
    {
        return;
    }

    properties_var = castNode(Var, slot->container);
    key_const = linitial_node(Const, slot->keys);
    if (properties_var->varno != scanrelid ||
        scanrelid <= 0 ||
        scanrelid >= root->simple_rel_array_size)
    {
        return;
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
    cp->custom_private = list_make4(copyObject(key_const),
                                    makeInteger(scanrelid),
                                    make_oid_const(slot->value_type),
                                    make_oid_const(slot->field_result_type));
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
                             "limit_needed=%s",
                             scanrelid, cp->path.rows,
                             cp->path.total_cost,
                             extra != NULL && extra->limit_needed ?
                             "true" : "false")));
}

static Plan *plan_age_property_projection_path(PlannerInfo *root,
                                               RelOptInfo *rel,
                                               CustomPath *best_path,
                                               List *tlist, List *clauses,
                                               List *custom_plans)
{
    CustomScan *cs;
    Const *key_const;
    Integer *scanrelid_value;
    Const *value_type_const;
    Const *field_result_type_const;
    Index scanrelid;

    (void) root;
    (void) rel;
    (void) clauses;
    (void) custom_plans;

    key_const = linitial_node(Const, best_path->custom_private);
    scanrelid_value = lsecond_node(Integer, best_path->custom_private);
    value_type_const = list_nth_node(Const, best_path->custom_private, 2);
    field_result_type_const = list_nth_node(Const, best_path->custom_private,
                                            3);
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
    cs->custom_private = list_make3(copyObject(key_const),
                                    copyObject(value_type_const),
                                    copyObject(field_result_type_const));
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

static bool is_age_vle_function_rte(RangeTblEntry *rte, FuncExpr **func_expr)
{
    RangeTblFunction *rtfunc;
    FuncExpr *candidate;
    const char *func_name;

    if (func_expr != NULL)
        *func_expr = NULL;

    if (rte == NULL ||
        rte->rtekind != RTE_FUNCTION ||
        rte->funcordinality ||
        list_length(rte->functions) != 1)
    {
        return false;
    }

    rtfunc = linitial_node(RangeTblFunction, rte->functions);
    if (!IsA(rtfunc->funcexpr, FuncExpr) ||
        rtfunc->funccolcount != 1)
    {
        return false;
    }

    candidate = (FuncExpr *)rtfunc->funcexpr;
    if (candidate->funcresulttype != AGTYPEOID ||
        !candidate->funcretset)
    {
        return false;
    }

    func_name = get_func_name(candidate->funcid);
    if (func_name == NULL ||
        strcmp(func_name, "age_vle") != 0)
    {
        return false;
    }

    if (func_expr != NULL)
        *func_expr = candidate;
    return true;
}

static void add_age_vle_stream_custom_path(PlannerInfo *root, RelOptInfo *rel,
                                           RangeTblEntry *rte,
                                           FuncExpr *func_expr)
{
    CustomPath *cp;
    Path *reference_path;

    if (rel == NULL ||
        rel->reloptkind != RELOPT_BASEREL ||
        rel->pathlist == NIL ||
        func_expr == NULL)
    {
        return;
    }

    reference_path = linitial(rel->pathlist);

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
    cp->custom_private = list_make1(copyObject(func_expr));
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
    FuncExpr *func_expr;

    (void) root;
    (void) clauses;
    (void) custom_plans;

    func_expr = linitial_node(FuncExpr, best_path->custom_private);

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
    cs->custom_exprs = list_make1(copyObject(func_expr));
    cs->custom_private = NIL;
    cs->custom_scan_tlist = copyObject(tlist);
    cs->custom_relids = bms_make_singleton(rel->relid);
    cs->methods = &age_vle_stream_scan_methods;

    return (Plan *)cs;
}

static Oid get_cached_agtype_eq_oid(void)
{
    if (!OidIsValid(agtype_eq_func_oid))
    {
        agtype_eq_func_oid =
            get_ag_func_oid("agtype_eq", 2, AGTYPEOID, AGTYPEOID);
    }

    return agtype_eq_func_oid;
}

static Oid get_cached_agtype_cmp_func_oid(const char *name, Oid *cache)
{
    if (!OidIsValid(*cache))
    {
        *cache = get_ag_func_oid(name, 2, AGTYPEOID, AGTYPEOID);
    }

    return *cache;
}

static Oid get_cached_agtype_field_exists_nonnull_oid(void)
{
    if (!OidIsValid(agtype_field_exists_nonnull_func_oid))
    {
        agtype_field_exists_nonnull_func_oid =
            get_ag_func_oid("agtype_object_field_exists_nonnull", 2,
                            AGTYPEOID, AGTYPEOID);
    }

    return agtype_field_exists_nonnull_func_oid;
}

static Oid get_cached_agtype_field_equals_oid(void)
{
    if (!OidIsValid(agtype_field_equals_func_oid))
    {
        agtype_field_equals_func_oid =
            get_ag_func_oid("agtype_object_field_equals", 3,
                            AGTYPEOID, AGTYPEOID, AGTYPEOID);
    }

    return agtype_field_equals_func_oid;
}

static Oid get_cached_agtype_field_cmp_oid(void)
{
    if (!OidIsValid(agtype_field_cmp_func_oid))
    {
        agtype_field_cmp_func_oid =
            get_ag_func_oid("agtype_object_field_cmp", 3,
                            AGTYPEOID, AGTYPEOID, AGTYPEOID);
    }

    return agtype_field_cmp_func_oid;
}

static bool rewrite_property_equals_restrictions(PlannerInfo *root,
                                                 RelOptInfo *rel, Index rti)
{
    RangeTblEntry *rte;
    ListCell *lc;
    bool rewritten_any = false;

    if (root == NULL ||
        rti <= 0 ||
        rti >= root->simple_rel_array_size ||
        rel == NULL ||
        rel->baserestrictinfo == NIL)
    {
        return false;
    }

    rte = root->simple_rte_array[rti];
    if (rte == NULL || rte->rtekind != RTE_RELATION)
    {
        return false;
    }

    foreach(lc, rel->baserestrictinfo)
    {
        RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc);
        Node *rewritten;

        if (rinfo->pseudoconstant)
            continue;

        rewritten = try_rewrite_property_equals_clause((Node *)rinfo->clause, rti,
                                                       rel);
        if (rewritten != NULL)
        {
            rinfo->clause = (Expr *)rewritten;
            rewritten_any = true;
        }
    }

    return rewritten_any;
}

static Node *replace_property_index_side(OpExpr *op, bool replace_left,
                                         Node *index_expr)
{
    OpExpr *copy;
    Node *left;
    Node *right;

    if (op == NULL || index_expr == NULL)
        return NULL;

    copy = copyObject(op);
    left = linitial(copy->args);
    right = lsecond(copy->args);
    copy->args = replace_left ?
        list_make2(copyObject(index_expr), right) :
        list_make2(left, copyObject(index_expr));

    return (Node *)copy;
}

static void canonicalize_property_index_predicates(RelOptInfo *rel)
{
    ListCell *lc;
    List *canonical_exprs;

    if (rel == NULL)
        return;

    canonical_exprs = collect_canonical_property_index_exprs(rel);
    if (canonical_exprs == NIL)
        return;

    foreach(lc, rel->indexlist)
    {
        IndexOptInfo *index = lfirst_node(IndexOptInfo, lc);

        if (index->indpred == NIL)
            continue;

        index->indpred = (List *)
            expression_tree_mutator((Node *)index->indpred,
                                    canonicalize_property_index_exprs_mutator,
                                    canonical_exprs);
    }
}

static void canonicalize_property_index_restrictions(RelOptInfo *rel)
{
    ListCell *index_lc;
    List *canonical_exprs;

    if (rel == NULL)
        return;

    canonical_exprs = collect_canonical_property_index_exprs(rel);
    if (canonical_exprs == NIL)
        return;

    foreach(index_lc, rel->indexlist)
    {
        IndexOptInfo *index = lfirst_node(IndexOptInfo, index_lc);
        ListCell *rinfo_lc;

        foreach(rinfo_lc, index->indrestrictinfo)
        {
            RestrictInfo *rinfo = lfirst_node(RestrictInfo, rinfo_lc);

            rinfo->clause = (Expr *)
                expression_tree_mutator(
                    (Node *)rinfo->clause,
                    canonicalize_property_index_exprs_mutator,
                    canonical_exprs);
        }
    }
}

static List *collect_canonical_property_index_exprs(RelOptInfo *rel)
{
    List *exprs = NIL;
    ListCell *lc;

    if (rel == NULL)
        return NIL;

    foreach(lc, rel->indexlist)
    {
        IndexOptInfo *index = lfirst_node(IndexOptInfo, lc);

        expression_tree_walker((Node *)index->indexprs,
                               collect_canonical_property_index_exprs_walker,
                               &exprs);
        expression_tree_walker((Node *)index->indpred,
                               collect_canonical_property_index_exprs_walker,
                               &exprs);
    }

    return exprs;
}

static bool collect_canonical_property_index_exprs_walker(Node *node,
                                                          void *context)
{
    List **exprs = (List **)context;
    CypherPropertyHandoffDescriptor handoff;
    CypherCachedPropertySlotDescriptor slot;
    Node *slot_expr;

    if (node == NULL)
        return false;

    memset(&handoff, 0, sizeof(handoff));
    if (cypher_extract_property_access_signature(
            node, &handoff.property_signature) &&
        cypher_make_cached_property_slot_descriptor(&handoff, &slot) &&
        (slot.value_type != AGTYPEOID ||
         slot.field_result_type != AGTYPEOID))
    {
        slot_expr = cypher_make_cached_property_slot_expr(&slot);
        if (slot_expr != NULL && equal(slot_expr, node) &&
            !property_expr_list_contains(*exprs, slot_expr))
        {
            *exprs = lappend(*exprs, slot_expr);
        }

        return false;
    }

    return expression_tree_walker(
        node, collect_canonical_property_index_exprs_walker, context);
}

static Node *canonicalize_property_index_exprs_mutator(Node *node,
                                                       void *context)
{
    List *exprs = (List *)context;
    CypherPropertyHandoffDescriptor handoff;
    CypherCachedPropertySlotDescriptor slot;
    Node *slot_expr;

    if (node == NULL)
        return NULL;

    memset(&handoff, 0, sizeof(handoff));
    if (cypher_extract_property_access_signature(
            node, &handoff.property_signature) &&
        cypher_make_cached_property_slot_descriptor(&handoff, &slot) &&
        (slot.value_type != AGTYPEOID ||
         slot.field_result_type != AGTYPEOID))
    {
        slot_expr = cypher_make_cached_property_slot_expr(&slot);
        if (slot_expr != NULL && property_expr_list_contains(exprs, slot_expr))
            return slot_expr;
    }

    return expression_tree_mutator(
        node, canonicalize_property_index_exprs_mutator, context);
}

static bool property_expr_list_contains(List *exprs, Node *expr)
{
    ListCell *lc;

    foreach(lc, exprs)
    {
        if (equal(lfirst(lc), expr))
            return true;
    }

    return false;
}

static Node *try_rewrite_property_equals_clause(Node *clause, Index rti,
                                                RelOptInfo *rel)
{
    OpExpr *op;
    Node *left;
    Node *right;
    Node *properties = NULL;
    Node *key = NULL;

    if (clause == NULL || !IsA(clause, OpExpr))
        return NULL;

    op = (OpExpr *)clause;
    if (list_length(op->args) != 2)
        return NULL;

    left = linitial(op->args);
    right = lsecond(op->args);

    {
        CypherPropertyIndexHandoff index_handoff;

        if (cypher_find_matching_property_index_handoff(rel, left,
                                                        &index_handoff))
        {
            return replace_property_index_side(op, true,
                                               index_handoff.index_expr);
        }

        if (cypher_find_matching_property_index_handoff(rel, right,
                                                        &index_handoff))
        {
            return replace_property_index_side(op, false,
                                               index_handoff.index_expr);
        }
    }

    if (match_property_access_expr(left, rti, &properties, &key))
    {
        const char *operator_name;

        if (op->opfuncid == get_cached_agtype_eq_oid())
        {
            return (Node *)makeFuncExpr(get_cached_agtype_field_equals_oid(),
                                        BOOLOID,
                                        list_make3(copyObject(properties),
                                                   copyObject(key),
                                                   copyObject(right)),
                                        InvalidOid, InvalidOid,
                                        COERCE_EXPLICIT_CALL);
        }

        operator_name = property_compare_operator_name(op->opfuncid, false);
        if (operator_name != NULL)
        {
            FuncExpr *cmp_expr;

            cmp_expr = makeFuncExpr(get_cached_agtype_field_cmp_oid(),
                                    INT4OID,
                                    list_make3(copyObject(properties),
                                               copyObject(key),
                                               copyObject(right)),
                                    InvalidOid, InvalidOid,
                                    COERCE_EXPLICIT_CALL);
            return (Node *)make_int4_zero_compare_expr(cmp_expr,
                                                       operator_name);
        }
    }

    if (match_property_access_expr(right, rti, &properties, &key))
    {
        const char *operator_name;

        if (op->opfuncid == get_cached_agtype_eq_oid())
        {
            return (Node *)makeFuncExpr(get_cached_agtype_field_equals_oid(),
                                        BOOLOID,
                                        list_make3(copyObject(properties),
                                                   copyObject(key),
                                                   copyObject(left)),
                                        InvalidOid, InvalidOid,
                                        COERCE_EXPLICIT_CALL);
        }

        operator_name = property_compare_operator_name(op->opfuncid, true);
        if (operator_name != NULL)
        {
            FuncExpr *cmp_expr;

            cmp_expr = makeFuncExpr(get_cached_agtype_field_cmp_oid(),
                                    INT4OID,
                                    list_make3(copyObject(properties),
                                               copyObject(key),
                                               copyObject(left)),
                                    InvalidOid, InvalidOid,
                                    COERCE_EXPLICIT_CALL);
            return (Node *)make_int4_zero_compare_expr(cmp_expr,
                                                       operator_name);
        }
    }

    return NULL;
}

static bool match_property_access_expr(Node *node, Index rti,
                                       Node **properties, Node **key)
{
    Node *properties_arg;
    Node *key_arg;

    if (!cypher_extract_property_access_terminal_args(node, &properties_arg,
                                                     &key_arg))
        return false;

    if (!IsA(key_arg, Const) ||
        !property_object_belongs_to_rti(properties_arg, rti))
        return false;

    if (((Const *)key_arg)->consttype != AGTYPEOID ||
        ((Const *)key_arg)->constisnull)
    {
        return false;
    }

    *properties = properties_arg;
    *key = key_arg;
    return true;
}

static bool property_object_belongs_to_rti(Node *node, Index rti)
{
    List *vars;
    ListCell *lc;
    bool found = false;

    if (node == NULL || exprType(node) != AGTYPEOID)
        return false;

    vars = pull_var_clause(node,
                           PVC_RECURSE_AGGREGATES |
                           PVC_RECURSE_WINDOWFUNCS |
                           PVC_RECURSE_PLACEHOLDERS);

    foreach(lc, vars)
    {
        Var *var = lfirst_node(Var, lc);

        if (var->varno != rti || var->vartype != AGTYPEOID)
        {
            list_free(vars);
            return false;
        }
        found = true;
    }

    list_free(vars);
    return found;
}

static Expr *make_int4_zero_compare_expr(FuncExpr *cmp_expr,
                                         const char *operator_name)
{
    Oid opno;
    Expr *expr;

    opno = OpernameGetOprid(list_make1(makeString(pstrdup(operator_name))),
                            INT4OID, INT4OID);
    if (!OidIsValid(opno))
        return NULL;

    expr = make_opclause(opno, BOOLOID, false,
                         (Expr *)cmp_expr,
                         (Expr *)makeConst(INT4OID, -1, InvalidOid,
                                           sizeof(int32),
                                           Int32GetDatum(0), false, true),
                         InvalidOid, InvalidOid);
    ((OpExpr *)expr)->opfuncid = get_opcode(opno);

    return expr;
}

static const char *property_compare_operator_name(Oid opfuncid, bool commuted)
{
    if (opfuncid == get_cached_agtype_cmp_func_oid("agtype_lt",
                                                   &agtype_lt_func_oid))
        return commuted ? ">" : "<";
    if (opfuncid == get_cached_agtype_cmp_func_oid("agtype_le",
                                                   &agtype_le_func_oid))
        return commuted ? ">=" : "<=";
    if (opfuncid == get_cached_agtype_cmp_func_oid("agtype_gt",
                                                   &agtype_gt_func_oid))
        return commuted ? "<" : ">";
    if (opfuncid == get_cached_agtype_cmp_func_oid("agtype_ge",
                                                   &agtype_ge_func_oid))
        return commuted ? "<=" : ">=";

    return NULL;
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
    endpoint_fanout =
        estimate_adjacency_match_endpoint_fanout(rel, candidate);
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

static double estimate_adjacency_match_endpoint_fanout(
    RelOptInfo *rel, CypherAdjacencyMatchCandidate *candidate)
{
    HeapTuple stat_tuple;
    Form_pg_statistic stats;
    double reltuples;
    double distinct;

    if (rel == NULL ||
        candidate == NULL ||
        !OidIsValid(candidate->edge_label_oid) ||
        candidate->endpoint_attno <= 0)
    {
        return 0;
    }

    stat_tuple = SearchSysCache3(STATRELATTINH,
                                 ObjectIdGetDatum(candidate->edge_label_oid),
                                 Int16GetDatum(candidate->endpoint_attno),
                                 BoolGetDatum(false));
    if (!HeapTupleIsValid(stat_tuple))
        return 0;

    stats = (Form_pg_statistic)GETSTRUCT(stat_tuple);
    reltuples = rel->tuples > 0 ? rel->tuples : 1;

    if (stats->stadistinct > 0)
        distinct = stats->stadistinct;
    else if (stats->stadistinct < 0)
        distinct = -stats->stadistinct * reltuples;
    else
        distinct = 0;

    ReleaseSysCache(stat_tuple);

    if (distinct <= 0)
        return 0;

    return Max(reltuples / distinct, 1.0);
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
    count_any_agg_func_oid = InvalidOid;
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
