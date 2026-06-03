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
#include "utils/spccache.h"
#include "utils/syscache.h"

#include "optimizer/cypher_pathnode.h"
#include "optimizer/cypher_paths.h"
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

static set_rel_pathlist_hook_type prev_set_rel_pathlist_hook;
static set_join_pathlist_hook_type prev_set_join_pathlist_hook;
static create_upper_paths_hook_type prev_create_upper_paths_hook;
static List *adjacency_match_candidates = NIL;

static Oid cypher_create_clause_func_oid = InvalidOid;
static Oid cypher_set_clause_func_oid = InvalidOid;
static Oid cypher_delete_clause_func_oid = InvalidOid;
static Oid cypher_merge_clause_func_oid = InvalidOid;
static Oid agtype_access_operator_func_oid = InvalidOid;
static Oid agtype_eq_func_oid = InvalidOid;
static Oid agtype_lt_func_oid = InvalidOid;
static Oid agtype_le_func_oid = InvalidOid;
static Oid agtype_gt_func_oid = InvalidOid;
static Oid agtype_ge_func_oid = InvalidOid;
static Oid count_any_agg_func_oid = InvalidOid;
static Oid age_collect_float8_agg_func_oid = InvalidOid;
static Oid age_collect_int8_agg_func_oid = InvalidOid;
static Oid age_collect_text_agg_func_oid = InvalidOid;
static Oid array_agg_anynonarray_agg_func_oid = InvalidOid;
static Oid age_array_agg_property_agg_func_oid = InvalidOid;
static Oid age_array_agg_map2_property_agg_func_oid = InvalidOid;
static Oid age_array_agg_map_property_agg_func_oid = InvalidOid;
static Oid agtype_build_map_nonull_func_oid = InvalidOid;
static Oid age_array_agg_list_property_agg_func_oid = InvalidOid;
static Oid agtype_build_list_func_oid = InvalidOid;
static Oid agtype_object_field_int8_func_oid = InvalidOid;
static Oid agtype_ctid_field_agtype_func_oid = InvalidOid;
static Oid int8_to_agtype_func_oid = InvalidOid;
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
                                              FuncExpr **access_expr,
                                              const char **target_source);
static bool is_simple_property_access_target(Node *node);
static bool rewrite_count_property_access_pathtarget(PathTarget *target);
static bool rewrite_property_access_aggregate_expr(Node *node);
static bool rewrite_property_access_aggregate_walker(Node *node,
                                                     void *context);
static bool rewrite_collect_typed_scalar_expr(Node *node);
static void add_narrow_distinct_collect_paths(PlannerInfo *root,
                                             RelOptInfo *output_rel);
static Node *find_typed_distinct_collect_arg(PathTarget *target);
static Path *try_narrow_distinct_collect_path(PlannerInfo *root,
                                             RelOptInfo *output_rel,
                                             Path *path, Node *arg);
static bool pathtarget_contains_only_expr(PathTarget *target, Node *expr);
static bool rewrite_collect_numeric_property_expr(Node *node);
static bool rewrite_count_property_access_expr(Node *node);
static bool rewrite_array_agg_property_access_expr(Node *node);
static bool rewrite_array_agg_map2_property_access_expr(Node *node);
static bool rewrite_array_agg_map_property_access_expr(Node *node);
static bool rewrite_array_agg_list_property_access_expr(Node *node);
static bool is_count_any_aggref(Aggref *aggref);
static bool is_ag_catalog_aggref(Aggref *aggref);
static bool is_array_agg_agtype_aggref(Aggref *aggref);
static FuncExpr *extract_count_property_access_arg(Aggref *aggref);
static FuncExpr *extract_simple_property_access_arg(Aggref *aggref);
static bool extract_property_access_args(Node *node, Node **properties,
                                         Node **key);
static bool extract_map2_property_access_args(Expr *expr, Node **properties,
                                              Node **out_key1,
                                              Node **prop_key1,
                                              Node **out_key2,
                                              Node **prop_key2);
static bool extract_map_property_access_args(Expr *expr, Node **properties,
                                             List **out_keys,
                                             List **prop_keys);
static bool extract_list_property_access_args(Expr *expr, Node **properties,
                                              List **prop_keys);
static bool same_property_source(Node *left, Node *right);
static List *extract_map_build_args(FuncExpr *map_expr);
static Const *make_const_array_const(List *elements, Oid array_type,
                                     Oid element_type);
static Oid get_cached_count_any_agg_oid(void);
static Oid get_cached_age_collect_float8_agg_oid(void);
static Oid get_cached_age_collect_int8_agg_oid(void);
static Oid get_cached_age_collect_text_agg_oid(void);
static Oid get_cached_array_agg_anynonarray_agg_oid(void);
static Oid get_cached_age_array_agg_property_agg_oid(void);
static Oid get_cached_age_array_agg_map2_property_agg_oid(void);
static Oid get_cached_age_array_agg_map_property_agg_oid(void);
static Oid get_cached_agtype_build_map_nonull_oid(void);
static Oid get_cached_age_array_agg_list_property_agg_oid(void);
static Oid get_cached_agtype_build_list_oid(void);
static Oid get_cached_agtype_field_exists_nonnull_oid(void);
static Oid get_cached_agtype_field_equals_oid(void);
static Oid get_cached_agtype_field_cmp_oid(void);
static Oid get_cached_agtype_eq_oid(void);
static Oid get_cached_agtype_cmp_func_oid(const char *name, Oid *cache);
static void rewrite_property_equals_restrictions(PlannerInfo *root,
                                                 RelOptInfo *rel, Index rti);
static bool rel_has_matching_expression_index(RelOptInfo *rel, Node *expr);
static Node *try_rewrite_property_equals_clause(Node *clause, Index rti,
                                                RelOptInfo *rel);
static bool match_property_access_expr(Node *node, Index rti,
                                       Node **properties, Node **key);
static Expr *make_int4_zero_compare_expr(FuncExpr *cmp_expr,
                                         const char *operator_name);
static const char *property_compare_operator_name(Oid opfuncid,
                                                  bool commuted);
static void add_property_projection_custom_path(PlannerInfo *root,
                                                RelOptInfo *input_rel,
                                                RelOptInfo *output_rel,
                                                FuncExpr *access_expr,
                                                FinalPathExtraData *extra);
static bool detect_ordered_property_projection_delay(PlannerInfo *root,
                                                     Node **properties,
                                                     Node **key,
                                                     TargetEntry **sort_tle);
static void add_deferred_ordered_property_projection_path(
    PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel,
    FinalPathExtraData *extra);
static Path *copy_path_with_deferred_projection_target(PlannerInfo *root,
                                                       Path *path,
                                                       PathTarget *target);
static bool extract_int8_property_sort_args(Node *node, Node **properties,
                                            Node **key);
static Oid get_cached_agtype_object_field_int8_oid(void);
static Oid get_cached_agtype_ctid_field_agtype_oid(void);
static Oid get_cached_int8_to_agtype_oid(void);
static void add_deferred_count_agtype_projection_path(
    PlannerInfo *root, RelOptInfo *output_rel, FinalPathExtraData *extra);
static bool build_count_agtype_deferred_targets(PlannerInfo *root,
                                                PathTarget **lower_target,
                                                PathTarget **final_target);
static bool extract_int8_to_agtype_arg(Node *node, Node **arg);
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
static Oid get_cached_agtype_access_operator_oid(void);
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

static void set_rel_pathlist(PlannerInfo *root, RelOptInfo *rel, Index rti,
                             RangeTblEntry *rte)
{
    CypherAdjacencyMatchCandidate *candidate;
    FuncExpr *vle_func_expr = NULL;

    if (prev_set_rel_pathlist_hook)
        prev_set_rel_pathlist_hook(root, rel, rti, rte);

    rewrite_property_equals_restrictions(root, rel, rti);

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
    FuncExpr *access_expr = NULL;
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
                                          &access_expr, &target_source))
    {
        ereport(DEBUG2,
                (errmsg_internal("AGE simple property projection visible after "
                                 "scan/join target: input_paths=%d "
                                 "output_paths=%d target_source=%s funcid=%u",
                                 list_length(input_rel->pathlist),
                                 list_length(output_rel->pathlist),
                                 target_source,
                                 access_expr->funcid)));
        add_property_projection_custom_path(root, input_rel, output_rel,
                                            access_expr,
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
                                              FuncExpr **access_expr,
                                              const char **target_source)
{
    Node *expr;

    (void)root;

    if (input_rel != NULL &&
        input_rel->reltarget != NULL &&
        list_length(input_rel->reltarget->exprs) == 1)
    {
        expr = linitial(input_rel->reltarget->exprs);
        if (is_simple_property_access_target(expr))
        {
            *access_expr = castNode(FuncExpr, expr);
            *target_source = "input_rel";
            return true;
        }
    }

    if (output_rel != NULL &&
        output_rel->reltarget != NULL &&
        list_length(output_rel->reltarget->exprs) == 1)
    {
        expr = linitial(output_rel->reltarget->exprs);
        if (is_simple_property_access_target(expr))
        {
            *access_expr = castNode(FuncExpr, expr);
            *target_source = "output_rel";
            return true;
        }
    }

    return false;
}

static bool is_simple_property_access_target(Node *node)
{
    FuncExpr *func;
    Var *properties_var;
    Const *key_const;

    if (node == NULL || !IsA(node, FuncExpr))
        return false;

    func = castNode(FuncExpr, node);
    if (list_length(func->args) != 2)
    {
        return false;
    }

    if (!IsA(linitial(func->args), Var) ||
        !IsA(lsecond(func->args), Const))
    {
        return false;
    }

    if (func->funcid != get_cached_agtype_access_operator_oid())
        return false;

    properties_var = linitial_node(Var, func->args);
    key_const = lsecond_node(Const, func->args);

    if (properties_var->varattno != Anum_ag_label_vertex_table_properties ||
        key_const->constisnull ||
        key_const->consttype != AGTYPEOID)
    {
        return false;
    }

    return true;
}

static bool detect_ordered_property_projection_delay(PlannerInfo *root,
                                                     Node **properties,
                                                     Node **key,
                                                     TargetEntry **sort_tle)
{
    TargetEntry *output_tle = NULL;
    SortGroupClause *sort_clause;
    Node *output_properties;
    Node *output_key;
    Node *sort_properties;
    Node *sort_key;
    ListCell *lc;

    if (root == NULL || root->parse == NULL ||
        root->parse->sortClause == NIL ||
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
        !extract_property_access_args((Node *)output_tle->expr,
                                      &output_properties, &output_key))
    {
        return false;
    }

    sort_clause = linitial_node(SortGroupClause, root->parse->sortClause);
    *sort_tle = get_sortgroupref_tle(sort_clause->tleSortGroupRef,
                                     root->parse->targetList);

    if (*sort_tle == NULL ||
        !(*sort_tle)->resjunk ||
        !extract_int8_property_sort_args((Node *)(*sort_tle)->expr,
                                         &sort_properties, &sort_key))
    {
        return false;
    }

    if (!same_property_source(output_properties, sort_properties) ||
        !equal(output_key, sort_key))
    {
        return false;
    }

    *properties = output_properties;
    *key = output_key;
    return true;
}

static void add_deferred_ordered_property_projection_path(
    PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel,
    FinalPathExtraData *extra)
{
    Node *properties = NULL;
    Node *key = NULL;
    TargetEntry *sort_tle = NULL;
    PathTarget *lower_target;
    PathTarget *final_target;
    Var *properties_var;
    Var *ctid_var;
    FuncExpr *final_output_expr;
    Const *relid_const;
    Oid relid;
    ListCell *lc;

    if (input_rel == NULL || output_rel == NULL || extra == NULL ||
        !extra->limit_needed ||
        !detect_ordered_property_projection_delay(root, &properties,
                                                  &key, &sort_tle) ||
        !IsA(properties, Var))
    {
        return;
    }

    properties_var = castNode(Var, properties);
    if (properties_var->varno <= 0 ||
        properties_var->varno >= root->simple_rel_array_size ||
        properties_var->varattno != Anum_ag_label_vertex_table_properties ||
        root->simple_rte_array[properties_var->varno] == NULL ||
        root->simple_rte_array[properties_var->varno]->rtekind != RTE_RELATION)
    {
        return;
    }

    relid = root->simple_rte_array[properties_var->varno]->relid;
    ctid_var = makeVar(properties_var->varno, SelfItemPointerAttributeNumber,
                       TIDOID, -1, InvalidOid, properties_var->varlevelsup);
    relid_const = makeConst(OIDOID, -1, InvalidOid, sizeof(Oid),
                            ObjectIdGetDatum(relid), false, true);
    final_output_expr = makeFuncExpr(get_cached_agtype_ctid_field_agtype_oid(),
                                     AGTYPEOID,
                                     list_make3(relid_const,
                                                copyObject(ctid_var),
                                                copyObject(key)),
                                     InvalidOid, InvalidOid,
                                     COERCE_EXPLICIT_CALL);

    lower_target = create_empty_pathtarget();
    add_column_to_pathtarget(lower_target, (Expr *)copyObject(ctid_var), 0);
    add_column_to_pathtarget(lower_target, (Expr *)copyObject(sort_tle->expr),
                             sort_tle->ressortgroupref);
    lower_target = set_pathtarget_cost_width(root, lower_target);

    final_target = create_empty_pathtarget();
    add_column_to_pathtarget(final_target, (Expr *)final_output_expr, 0);
    add_column_to_pathtarget(final_target, (Expr *)copyObject(sort_tle->expr),
                             sort_tle->ressortgroupref);
    final_target = set_pathtarget_cost_width(root, final_target);

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
            root, limit_path->subpath, lower_target);
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
                                        original_total_cost - 1.0);

        ereport(DEBUG2,
                (errmsg_internal("AGE deferred ordered property projection "
                                 "path added: original_cost=%.2f "
                                 "new_cost=%.2f rows=%.0f",
                                 original_total_cost,
                                 deferred_path->total_cost,
                                 deferred_path->rows)));

        add_path(output_rel, deferred_path);
    }
}

static Path *copy_path_with_deferred_projection_target(PlannerInfo *root,
                                                       Path *path,
                                                       PathTarget *target)
{
    if (path == NULL)
        return NULL;

    if (IsA(path, LimitPath))
    {
        LimitPath *limit_path;

        limit_path = palloc(sizeof(*limit_path));
        memcpy(limit_path, path, sizeof(*limit_path));

        limit_path->subpath = copy_path_with_deferred_projection_target(
            root, limit_path->subpath, target);
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
            root, gm_path->subpath, target);
        if (gm_path->subpath == NULL)
            return NULL;
        gm_path->path.pathtype = T_GatherMerge;
        gm_path->path.pathtarget = gm_path->subpath->pathtarget;
        gm_path->path.pathkeys = path->pathkeys;
        return (Path *)gm_path;
    }
    if (IsA(path, SortPath))
    {
        SortPath *sort_path;

        sort_path = palloc(sizeof(*sort_path));
        memcpy(sort_path, path, sizeof(*sort_path));

        sort_path->subpath = copy_path_with_deferred_projection_target(
            root, sort_path->subpath, target);
        if (sort_path->subpath == NULL)
            return NULL;
        sort_path->path.pathtype = T_Sort;
        sort_path->path.pathtarget = sort_path->subpath->pathtarget;
        sort_path->path.pathkeys = path->pathkeys;
        return (Path *)sort_path;
    }

    if (!is_projection_capable_path(path))
        return NULL;

    return (Path *)create_projection_path(root, path->parent, path, target);
}

static bool extract_int8_property_sort_args(Node *node, Node **properties,
                                            Node **key)
{
    FuncExpr *func;

    if (node == NULL || !IsA(node, FuncExpr))
        return false;

    func = castNode(FuncExpr, node);
    if (func->funcid != get_cached_agtype_object_field_int8_oid() ||
        list_length(func->args) != 2)
    {
        return false;
    }

    *properties = linitial(func->args);
    *key = lsecond(func->args);

    return *properties != NULL &&
        *key != NULL &&
        exprType(*properties) == AGTYPEOID &&
        exprType(*key) == AGTYPEOID;
}

static void add_deferred_count_agtype_projection_path(
    PlannerInfo *root, RelOptInfo *output_rel, FinalPathExtraData *extra)
{
    PathTarget *lower_target;
    PathTarget *final_target;
    ListCell *lc;

    if (root == NULL || output_rel == NULL || extra == NULL ||
        !extra->limit_needed ||
        !build_count_agtype_deferred_targets(root, &lower_target,
                                             &final_target))
    {
        return;
    }

    foreach(lc, output_rel->pathlist)
    {
        Path *path = lfirst(lc);
        LimitPath *limit_path;
        ProjectionPath *project_path;
        Path *deferred_path;
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
            root, limit_path->subpath, lower_target);
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
                                        original_total_cost - 0.5);

        add_path(output_rel, deferred_path);
    }
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
        Node *count_arg = NULL;

        add_column_to_pathtarget(*final_target, (Expr *)copyObject(tle->expr),
                                 tle->ressortgroupref);

        if (!tle->resjunk &&
            extract_int8_to_agtype_arg((Node *)tle->expr, &count_arg))
        {
            add_column_to_pathtarget(*lower_target,
                                     (Expr *)copyObject(count_arg),
                                     tle->ressortgroupref);
            found = true;
        }
        else
        {
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

static bool extract_int8_to_agtype_arg(Node *node, Node **arg)
{
    FuncExpr *func;

    if (node == NULL || !IsA(node, FuncExpr))
        return false;

    func = castNode(FuncExpr, node);
    if (func->funcid != get_cached_int8_to_agtype_oid() ||
        list_length(func->args) != 1)
    {
        return false;
    }

    *arg = linitial(func->args);
    return *arg != NULL && exprType(*arg) == INT8OID;
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

    if (rewrite_collect_typed_scalar_expr(node) ||
        rewrite_collect_numeric_property_expr(node) ||
        rewrite_count_property_access_expr(node) ||
        rewrite_array_agg_property_access_expr(node) ||
        rewrite_array_agg_map2_property_access_expr(node) ||
        rewrite_array_agg_map_property_access_expr(node) ||
        rewrite_array_agg_list_property_access_expr(node))
    {
        *rewritten = true;
        return false;
    }

    return expression_tree_walker(node,
                                  rewrite_property_access_aggregate_walker,
                                  context);
}

static bool rewrite_collect_typed_scalar_expr(Node *node)
{
    return false;
}

static void add_narrow_distinct_collect_paths(PlannerInfo *root,
                                              RelOptInfo *output_rel)
{
    Node *arg;
    List *new_paths = NIL;
    ListCell *lc;

    if (root == NULL || output_rel == NULL || output_rel->pathlist == NIL)
        return;

    arg = find_typed_distinct_collect_arg(output_rel->reltarget);
    if (arg == NULL)
        return;

    foreach(lc, output_rel->pathlist)
    {
        Path *path = lfirst(lc);
        Path *new_path;

        new_path = try_narrow_distinct_collect_path(root, output_rel, path,
                                                    arg);
        if (new_path != NULL)
            new_paths = lappend(new_paths, new_path);
    }

    foreach(lc, new_paths)
        add_path(output_rel, lfirst(lc));
}

static Node *find_typed_distinct_collect_arg(PathTarget *target)
{
    Aggref *found = NULL;
    TargetEntry *arg_tle;
    ListCell *lc;

    if (target == NULL)
        return NULL;

    foreach(lc, target->exprs)
    {
        Node *expr = lfirst(lc);
        Aggref *aggref;
        Oid argtype;

        if (expr == NULL || !IsA(expr, Aggref))
            continue;

        aggref = castNode(Aggref, expr);
        if (!is_ag_catalog_aggref(aggref))
            continue;

        if (aggref->aggfnoid != get_cached_age_collect_float8_agg_oid() &&
            aggref->aggfnoid != get_cached_age_collect_int8_agg_oid() &&
            aggref->aggfnoid != get_cached_age_collect_text_agg_oid())
        {
            continue;
        }

        if (aggref->aggstar ||
            aggref->aggdirectargs != NIL ||
            aggref->aggdistinct == NIL ||
            aggref->aggfilter != NULL ||
            list_length(aggref->aggargtypes) != 1 ||
            list_length(aggref->args) != 1)
        {
            return NULL;
        }

        argtype = linitial_oid(aggref->aggargtypes);
        if (argtype != FLOAT8OID && argtype != INT8OID && argtype != TEXTOID)
            return NULL;

        if (found != NULL)
            return NULL;

        found = aggref;
    }

    if (found == NULL)
        return NULL;

    arg_tle = linitial_node(TargetEntry, found->args);
    if (arg_tle == NULL || arg_tle->expr == NULL)
        return NULL;

    return (Node *)arg_tle->expr;
}

static Path *try_narrow_distinct_collect_path(PlannerInfo *root,
                                              RelOptInfo *output_rel,
                                              Path *path, Node *arg)
{
    AggPath *agg_path;
    SortPath *sort_path;
    PathTarget *narrow_target;
    Path *projected_subpath;
    SortPath *new_sort;
    AggPath *new_agg;

    if (path == NULL || arg == NULL || !IsA(path, AggPath))
        return NULL;

    agg_path = (AggPath *)path;
    if (agg_path->aggstrategy != AGG_PLAIN ||
        agg_path->groupClause != NIL ||
        agg_path->qual != NIL ||
        agg_path->subpath == NULL ||
        !IsA(agg_path->subpath, SortPath))
    {
        return NULL;
    }

    sort_path = (SortPath *)agg_path->subpath;
    if (sort_path->subpath == NULL ||
        pathtarget_contains_only_expr(sort_path->path.pathtarget, arg))
    {
        return NULL;
    }

    narrow_target = create_empty_pathtarget();
    add_column_to_pathtarget(narrow_target, (Expr *)copyObject(arg), 0);
    narrow_target = set_pathtarget_cost_width(root, narrow_target);

    projected_subpath = (Path *)create_projection_path(
        root, sort_path->subpath->parent, sort_path->subpath, narrow_target);

    new_sort = palloc(sizeof(*new_sort));
    memcpy(new_sort, sort_path, sizeof(*new_sort));
    new_sort->path.pathtype = T_Sort;
    new_sort->path.pathtarget = narrow_target;
    new_sort->path.rows = sort_path->path.rows;
    new_sort->path.disabled_nodes = sort_path->path.disabled_nodes;
    new_sort->path.startup_cost = sort_path->path.startup_cost;
    new_sort->path.total_cost = Max(sort_path->path.startup_cost,
                                    sort_path->path.total_cost - 1.0);
    new_sort->subpath = projected_subpath;

    new_agg = palloc(sizeof(*new_agg));
    memcpy(new_agg, agg_path, sizeof(*new_agg));
    new_agg->path.pathtype = T_Agg;
    new_agg->subpath = (Path *)new_sort;
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

static bool rewrite_collect_numeric_property_expr(Node *node)
{
    return false;
}

static bool rewrite_count_property_access_expr(Node *node)
{
    Aggref *aggref;
    FuncExpr *access_expr;
    FuncExpr *exists_expr;
    TargetEntry *arg_tle;

    if (node == NULL || !IsA(node, Aggref))
        return false;

    aggref = castNode(Aggref, node);
    if (!is_count_any_aggref(aggref))
        return false;

    access_expr = extract_count_property_access_arg(aggref);
    if (access_expr == NULL)
        return false;

    exists_expr = makeFuncExpr(
        get_cached_agtype_field_exists_nonnull_oid(),
        BOOLOID,
        list_make2(copyObject(linitial(access_expr->args)),
                   copyObject(lsecond(access_expr->args))),
        InvalidOid, InvalidOid, COERCE_EXPLICIT_CALL);
    exists_expr->location = access_expr->location;

    arg_tle = linitial_node(TargetEntry, aggref->args);
    arg_tle->expr = (Expr *)exists_expr;
    aggref->aggargtypes = list_make1_oid(BOOLOID);

    return true;
}

static bool rewrite_array_agg_property_access_expr(Node *node)
{
    Aggref *aggref;
    FuncExpr *access_expr;
    TargetEntry *properties_tle;
    TargetEntry *key_tle;

    if (node == NULL || !IsA(node, Aggref))
        return false;

    aggref = castNode(Aggref, node);
    if (!is_array_agg_agtype_aggref(aggref))
        return false;

    access_expr = extract_simple_property_access_arg(aggref);
    if (access_expr == NULL)
        return false;

    properties_tle = makeTargetEntry(
        (Expr *)copyObject(linitial(access_expr->args)), 1, NULL, false);
    key_tle = makeTargetEntry(
        (Expr *)copyObject(lsecond(access_expr->args)), 2, NULL, false);

    aggref->aggfnoid = get_cached_age_array_agg_property_agg_oid();
    aggref->aggargtypes = list_make2_oid(AGTYPEOID, AGTYPEOID);
    aggref->args = list_make2(properties_tle, key_tle);
    aggref->aggvariadic = false;

    return true;
}

static bool rewrite_array_agg_map2_property_access_expr(Node *node)
{
    Aggref *aggref;
    TargetEntry *arg_tle;
    TargetEntry *properties_tle;
    TargetEntry *out_key1_tle;
    TargetEntry *prop_key1_tle;
    TargetEntry *out_key2_tle;
    TargetEntry *prop_key2_tle;
    Node *properties;
    Node *out_key1;
    Node *prop_key1;
    Node *out_key2;
    Node *prop_key2;

    if (node == NULL || !IsA(node, Aggref))
        return false;

    aggref = castNode(Aggref, node);
    if (!is_array_agg_agtype_aggref(aggref))
        return false;

    arg_tle = linitial_node(TargetEntry, aggref->args);
    if (arg_tle == NULL ||
        !extract_map2_property_access_args(arg_tle->expr, &properties,
                                           &out_key1, &prop_key1,
                                           &out_key2, &prop_key2))
    {
        return false;
    }

    properties_tle = makeTargetEntry((Expr *)copyObject(properties), 1,
                                     NULL, false);
    out_key1_tle = makeTargetEntry((Expr *)copyObject(out_key1), 2,
                                   NULL, false);
    prop_key1_tle = makeTargetEntry((Expr *)copyObject(prop_key1), 3,
                                    NULL, false);
    out_key2_tle = makeTargetEntry((Expr *)copyObject(out_key2), 4,
                                   NULL, false);
    prop_key2_tle = makeTargetEntry((Expr *)copyObject(prop_key2), 5,
                                    NULL, false);

    aggref->aggfnoid = get_cached_age_array_agg_map2_property_agg_oid();
    aggref->aggargtypes = list_make5_oid(AGTYPEOID, TEXTOID, AGTYPEOID,
                                         TEXTOID, AGTYPEOID);
    aggref->args = list_make5(properties_tle, out_key1_tle, prop_key1_tle,
                              out_key2_tle, prop_key2_tle);
    aggref->aggvariadic = false;

    return true;
}

static bool rewrite_array_agg_map_property_access_expr(Node *node)
{
    Aggref *aggref;
    TargetEntry *arg_tle;
    TargetEntry *properties_tle;
    TargetEntry *out_keys_tle;
    TargetEntry *prop_keys_tle;
    Node *properties;
    List *out_keys;
    List *prop_keys;

    if (node == NULL || !IsA(node, Aggref))
        return false;

    aggref = castNode(Aggref, node);
    if (!is_array_agg_agtype_aggref(aggref))
        return false;

    arg_tle = linitial_node(TargetEntry, aggref->args);
    if (arg_tle == NULL ||
        !extract_map_property_access_args(arg_tle->expr, &properties,
                                          &out_keys, &prop_keys))
    {
        return false;
    }

    properties_tle = makeTargetEntry((Expr *)copyObject(properties), 1,
                                     NULL, false);
    out_keys_tle = makeTargetEntry(
        (Expr *)make_const_array_const(out_keys, TEXTARRAYOID, TEXTOID),
        2, NULL, false);
    prop_keys_tle = makeTargetEntry(
        (Expr *)make_const_array_const(prop_keys, AGTYPEARRAYOID, AGTYPEOID),
        3, NULL, false);

    aggref->aggfnoid = get_cached_age_array_agg_map_property_agg_oid();
    aggref->aggargtypes = list_make3_oid(AGTYPEOID, TEXTARRAYOID,
                                         AGTYPEARRAYOID);
    aggref->args = list_make3(properties_tle, out_keys_tle, prop_keys_tle);
    aggref->aggvariadic = false;

    return true;
}

static bool rewrite_array_agg_list_property_access_expr(Node *node)
{
    Aggref *aggref;
    TargetEntry *arg_tle;
    TargetEntry *properties_tle;
    TargetEntry *prop_keys_tle;
    Node *properties;
    List *prop_keys;

    if (node == NULL || !IsA(node, Aggref))
        return false;

    aggref = castNode(Aggref, node);
    if (!is_array_agg_agtype_aggref(aggref))
        return false;

    arg_tle = linitial_node(TargetEntry, aggref->args);
    if (arg_tle == NULL ||
        !extract_list_property_access_args(arg_tle->expr, &properties,
                                           &prop_keys))
    {
        return false;
    }

    properties_tle = makeTargetEntry((Expr *)copyObject(properties), 1,
                                     NULL, false);
    prop_keys_tle = makeTargetEntry(
        (Expr *)make_const_array_const(prop_keys, AGTYPEARRAYOID, AGTYPEOID),
        2, NULL, false);

    aggref->aggfnoid = get_cached_age_array_agg_list_property_agg_oid();
    aggref->aggargtypes = list_make2_oid(AGTYPEOID, AGTYPEARRAYOID);
    aggref->args = list_make2(properties_tle, prop_keys_tle);
    aggref->aggvariadic = false;

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

static bool is_ag_catalog_aggref(Aggref *aggref)
{
    return aggref != NULL &&
        OidIsValid(aggref->aggfnoid) &&
        get_func_namespace(aggref->aggfnoid) == ag_catalog_namespace_id();
}

static bool is_array_agg_agtype_aggref(Aggref *aggref)
{
    return aggref != NULL &&
        aggref->aggfnoid == get_cached_array_agg_anynonarray_agg_oid() &&
        aggref->aggtype == AGTYPEARRAYOID &&
        !aggref->aggstar &&
        aggref->aggdirectargs == NIL &&
        aggref->aggorder == NIL &&
        aggref->aggdistinct == NIL &&
        aggref->aggfilter == NULL &&
        list_length(aggref->aggargtypes) == 1 &&
        linitial_oid(aggref->aggargtypes) == AGTYPEOID &&
        list_length(aggref->args) == 1;
}

static FuncExpr *extract_count_property_access_arg(Aggref *aggref)
{
    return extract_simple_property_access_arg(aggref);
}

static FuncExpr *extract_simple_property_access_arg(Aggref *aggref)
{
    TargetEntry *arg_tle;

    arg_tle = linitial_node(TargetEntry, aggref->args);
    if (arg_tle == NULL || arg_tle->expr == NULL ||
        !is_simple_property_access_target((Node *)arg_tle->expr))
    {
        return NULL;
    }

    return castNode(FuncExpr, arg_tle->expr);
}

static bool extract_property_access_args(Node *node, Node **properties,
                                         Node **key)
{
    FuncExpr *func;
    ArrayExpr *array;

    if (node == NULL || !IsA(node, FuncExpr))
        return false;

    func = castNode(FuncExpr, node);
    if (func->funcid != get_cached_agtype_access_operator_oid())
        return false;

    if (list_length(func->args) == 2)
    {
        *properties = linitial(func->args);
        *key = lsecond(func->args);
    }
    else if (list_length(func->args) == 1 && IsA(linitial(func->args), ArrayExpr))
    {
        array = linitial_node(ArrayExpr, func->args);
        if (list_length(array->elements) != 2)
            return false;

        *properties = linitial(array->elements);
        *key = lsecond(array->elements);
    }
    else
    {
        return false;
    }

    return *properties != NULL &&
        *key != NULL &&
        exprType(*properties) == AGTYPEOID &&
        exprType(*key) == AGTYPEOID;
}

static bool extract_map2_property_access_args(Expr *expr, Node **properties,
                                              Node **out_key1,
                                              Node **prop_key1,
                                              Node **out_key2,
                                              Node **prop_key2)
{
    FuncExpr *map_expr;
    List *map_args;
    Node *value1_properties;
    Node *value2_properties;

    if (expr == NULL || !IsA(expr, FuncExpr))
        return false;

    map_expr = castNode(FuncExpr, expr);
    if (map_expr->funcid != get_cached_agtype_build_map_nonull_oid())
        return false;

    map_args = extract_map_build_args(map_expr);
    if (list_length(map_args) != 4)
    {
        return false;
    }

    *out_key1 = linitial(map_args);
    *out_key2 = lthird(map_args);
    if (!IsA(*out_key1, Const) ||
        !IsA(*out_key2, Const) ||
        ((Const *)*out_key1)->constisnull ||
        ((Const *)*out_key2)->constisnull ||
        exprType(*out_key1) != TEXTOID ||
        exprType(*out_key2) != TEXTOID)
        return false;

    if (!extract_property_access_args(lsecond(map_args),
                                      &value1_properties, prop_key1) ||
        !extract_property_access_args(lfourth(map_args),
                                      &value2_properties, prop_key2))
    {
        return false;
    }

    if (!same_property_source(value1_properties, value2_properties))
        return false;

    if (!IsA(*prop_key1, Const) ||
        !IsA(*prop_key2, Const) ||
        ((Const *)*prop_key1)->constisnull ||
        ((Const *)*prop_key2)->constisnull)
    {
        return false;
    }

    *properties = value1_properties;
    return true;
}

static bool extract_map_property_access_args(Expr *expr, Node **properties,
                                             List **out_keys,
                                             List **prop_keys)
{
    FuncExpr *map_expr;
    List *map_args;
    Node *first_properties = NULL;
    int i;
    int nargs;

    if (expr == NULL || !IsA(expr, FuncExpr))
        return false;

    map_expr = castNode(FuncExpr, expr);
    if (map_expr->funcid != get_cached_agtype_build_map_nonull_oid())
        return false;

    map_args = extract_map_build_args(map_expr);
    if (list_length(map_args) < 6 ||
        list_length(map_args) % 2 != 0)
    {
        return false;
    }

    *out_keys = NIL;
    *prop_keys = NIL;

    nargs = list_length(map_args);
    for (i = 0; i < nargs; i += 2)
    {
        Node *out_key;
        Node *value_expr;
        Node *value_properties;
        Node *prop_key;

        out_key = list_nth(map_args, i);
        value_expr = list_nth(map_args, i + 1);

        if (!IsA(out_key, Const) ||
            ((Const *)out_key)->constisnull ||
            exprType(out_key) != TEXTOID)
        {
            return false;
        }

        if (!extract_property_access_args(value_expr, &value_properties,
                                          &prop_key))
        {
            return false;
        }

        if (!IsA(prop_key, Const) ||
            ((Const *)prop_key)->constisnull ||
            exprType(prop_key) != AGTYPEOID)
        {
            return false;
        }

        if (i == 0)
        {
            first_properties = value_properties;
        }
        else if (!same_property_source(first_properties, value_properties))
        {
            return false;
        }

        *out_keys = lappend(*out_keys, out_key);
        *prop_keys = lappend(*prop_keys, prop_key);
    }

    *properties = first_properties;
    return *properties != NULL && list_length(*out_keys) >= 3;
}

static bool extract_list_property_access_args(Expr *expr, Node **properties,
                                              List **prop_keys)
{
    FuncExpr *list_expr;
    List *list_args;
    Node *first_properties = NULL;
    int i;
    int nargs;

    if (expr == NULL || !IsA(expr, FuncExpr))
        return false;

    list_expr = castNode(FuncExpr, expr);
    if (list_expr->funcid != get_cached_agtype_build_list_oid())
        return false;

    list_args = extract_map_build_args(list_expr);
    nargs = list_length(list_args);
    if (nargs <= 0)
        return false;

    *prop_keys = NIL;
    for (i = 0; i < nargs; i++)
    {
        Node *value_expr;
        Node *value_properties;
        Node *prop_key;

        value_expr = list_nth(list_args, i);
        if (!extract_property_access_args(value_expr, &value_properties,
                                          &prop_key))
        {
            return false;
        }

        if (!IsA(prop_key, Const) ||
            ((Const *)prop_key)->constisnull ||
            exprType(prop_key) != AGTYPEOID)
        {
            return false;
        }

        if (i == 0)
        {
            first_properties = value_properties;
        }
        else if (!same_property_source(first_properties, value_properties))
        {
            return false;
        }

        *prop_keys = lappend(*prop_keys, prop_key);
    }

    *properties = first_properties;
    return *properties != NULL;
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

static List *extract_map_build_args(FuncExpr *map_expr)
{
    ArrayExpr *array;

    if (map_expr == NULL)
        return NIL;

    if (list_length(map_expr->args) == 1 &&
        IsA(linitial(map_expr->args), ArrayExpr))
    {
        array = linitial_node(ArrayExpr, map_expr->args);
        return array->elements;
    }

    return map_expr->args;
}

static Const *make_const_array_const(List *elements, Oid array_type,
                                     Oid element_type)
{
    ListCell *lc;
    Datum *values;
    ArrayType *array;
    int nelems;
    int i = 0;
    int16 typlen;
    bool typbyval;
    char typalign;

    nelems = list_length(elements);
    values = palloc(sizeof(Datum) * nelems);

    foreach(lc, elements)
    {
        Const *element = castNode(Const, lfirst(lc));

        Assert(!element->constisnull);
        values[i++] = element->constvalue;
    }

    get_typlenbyvalalign(element_type, &typlen, &typbyval, &typalign);
    array = construct_array(values, nelems, element_type, typlen, typbyval,
                            typalign);

    return makeConst(array_type, -1, InvalidOid, -1, PointerGetDatum(array),
                     false, false);
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

static Oid get_cached_age_collect_float8_agg_oid(void)
{
    if (!OidIsValid(age_collect_float8_agg_func_oid))
    {
        age_collect_float8_agg_func_oid =
            get_ag_func_oid("age_collect_float8", 1, FLOAT8OID);
    }

    return age_collect_float8_agg_func_oid;
}

static Oid get_cached_age_collect_int8_agg_oid(void)
{
    if (!OidIsValid(age_collect_int8_agg_func_oid))
    {
        age_collect_int8_agg_func_oid =
            get_ag_func_oid("age_collect_int8", 1, INT8OID);
    }

    return age_collect_int8_agg_func_oid;
}

static Oid get_cached_age_collect_text_agg_oid(void)
{
    if (!OidIsValid(age_collect_text_agg_func_oid))
    {
        age_collect_text_agg_func_oid =
            get_ag_func_oid("age_collect_text", 1, TEXTOID);
    }

    return age_collect_text_agg_func_oid;
}

static Oid get_cached_array_agg_anynonarray_agg_oid(void)
{
    if (!OidIsValid(array_agg_anynonarray_agg_func_oid))
    {
        Oid argtype = ANYNONARRAYOID;

        array_agg_anynonarray_agg_func_oid =
            LookupFuncName(list_make2(makeString("pg_catalog"),
                                      makeString("array_agg")),
                           1, &argtype, false);
    }

    return array_agg_anynonarray_agg_func_oid;
}

static Oid get_cached_age_array_agg_property_agg_oid(void)
{
    if (!OidIsValid(age_array_agg_property_agg_func_oid))
    {
        age_array_agg_property_agg_func_oid =
            get_ag_func_oid("age_array_agg_property", 2, AGTYPEOID,
                            AGTYPEOID);
    }

    return age_array_agg_property_agg_func_oid;
}

static Oid get_cached_age_array_agg_map2_property_agg_oid(void)
{
    if (!OidIsValid(age_array_agg_map2_property_agg_func_oid))
    {
        age_array_agg_map2_property_agg_func_oid =
            get_ag_func_oid("age_array_agg_map2_property", 5, AGTYPEOID,
                            TEXTOID, AGTYPEOID, TEXTOID, AGTYPEOID);
    }

    return age_array_agg_map2_property_agg_func_oid;
}

static Oid get_cached_age_array_agg_map_property_agg_oid(void)
{
    if (!OidIsValid(age_array_agg_map_property_agg_func_oid))
    {
        age_array_agg_map_property_agg_func_oid =
            get_ag_func_oid("age_array_agg_map_property", 3, AGTYPEOID,
                            TEXTARRAYOID, AGTYPEARRAYOID);
    }

    return age_array_agg_map_property_agg_func_oid;
}

static Oid get_cached_age_array_agg_list_property_agg_oid(void)
{
    if (!OidIsValid(age_array_agg_list_property_agg_func_oid))
    {
        age_array_agg_list_property_agg_func_oid =
            get_ag_func_oid("age_array_agg_list_property", 2, AGTYPEOID,
                            AGTYPEARRAYOID);
    }

    return age_array_agg_list_property_agg_func_oid;
}

static Oid get_cached_agtype_build_map_nonull_oid(void)
{
    if (!OidIsValid(agtype_build_map_nonull_func_oid))
    {
        agtype_build_map_nonull_func_oid =
            get_ag_func_oid("agtype_build_map_nonull", 1, ANYOID);
    }

    return agtype_build_map_nonull_func_oid;
}

static Oid get_cached_agtype_build_list_oid(void)
{
    if (!OidIsValid(agtype_build_list_func_oid))
    {
        agtype_build_list_func_oid =
            get_ag_func_oid("agtype_build_list", 1, ANYOID);
    }

    return agtype_build_list_func_oid;
}

static void add_property_projection_custom_path(PlannerInfo *root,
                                                RelOptInfo *input_rel,
                                                RelOptInfo *output_rel,
                                                FuncExpr *access_expr,
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

    if (input_rel == NULL || output_rel == NULL || access_expr == NULL ||
        root->parse == NULL ||
        input_rel->pathlist == NIL ||
        !bms_get_singleton_member(input_rel->relids, &scanrelid_int))
    {
        return;
    }
    scanrelid = (Index)scanrelid_int;

    properties_var = linitial_node(Var, access_expr->args);
    key_const = lsecond_node(Const, access_expr->args);
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
    cp->custom_private = list_make2(copyObject(key_const),
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
    Index scanrelid;

    (void) root;
    (void) rel;
    (void) clauses;
    (void) custom_plans;

    key_const = linitial_node(Const, best_path->custom_private);
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
    cs->custom_private = list_make1(copyObject(key_const));
    cs->custom_scan_tlist = copyObject(tlist);
    cs->custom_relids = bms_make_singleton(scanrelid);
    cs->methods = &age_property_projection_scan_methods;

    return (Plan *)cs;
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

static Oid get_cached_agtype_access_operator_oid(void)
{
    if (!OidIsValid(agtype_access_operator_func_oid))
    {
        agtype_access_operator_func_oid =
            get_ag_func_oid("agtype_access_operator", 1, AGTYPEARRAYOID);
    }

    return agtype_access_operator_func_oid;
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

static Oid get_cached_agtype_object_field_int8_oid(void)
{
    if (!OidIsValid(agtype_object_field_int8_func_oid))
    {
        agtype_object_field_int8_func_oid =
            get_ag_func_oid("agtype_object_field_int8", 2,
                            AGTYPEOID, AGTYPEOID);
    }

    return agtype_object_field_int8_func_oid;
}

static Oid get_cached_agtype_ctid_field_agtype_oid(void)
{
    if (!OidIsValid(agtype_ctid_field_agtype_func_oid))
    {
        agtype_ctid_field_agtype_func_oid =
            get_ag_func_oid("agtype_ctid_field_agtype", 3,
                            OIDOID, TIDOID, AGTYPEOID);
    }

    return agtype_ctid_field_agtype_func_oid;
}

static Oid get_cached_int8_to_agtype_oid(void)
{
    if (!OidIsValid(int8_to_agtype_func_oid))
    {
        int8_to_agtype_func_oid =
            get_ag_func_oid("int8_to_agtype", 1, INT8OID);
    }

    return int8_to_agtype_func_oid;
}

static void rewrite_property_equals_restrictions(PlannerInfo *root,
                                                 RelOptInfo *rel, Index rti)
{
    RangeTblEntry *rte;
    ListCell *lc;

    if (root == NULL ||
        rti <= 0 ||
        rti >= root->simple_rel_array_size ||
        rel == NULL ||
        rel->baserestrictinfo == NIL)
    {
        return;
    }

    rte = root->simple_rte_array[rti];
    if (rte == NULL || rte->rtekind != RTE_RELATION)
    {
        return;
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
        }
    }
}

static bool rel_has_matching_expression_index(RelOptInfo *rel, Node *expr)
{
    ListCell *index_lc;

    foreach(index_lc, rel->indexlist)
    {
        IndexOptInfo *index = lfirst_node(IndexOptInfo, index_lc);
        ListCell *expr_lc;

        foreach(expr_lc, index->indexprs)
        {
            if (equal(expr, lfirst(expr_lc)))
                return true;
        }
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

    if (match_property_access_expr(left, rti, &properties, &key))
    {
        const char *operator_name;

        if (rel_has_matching_expression_index(rel, left))
            return NULL;

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

        if (rel_has_matching_expression_index(rel, right))
            return NULL;

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
    FuncExpr *func;
    ArrayExpr *array;
    Node *properties_arg;
    Node *key_arg;
    Var *properties_var;

    if (node == NULL || !IsA(node, FuncExpr))
        return false;

    func = (FuncExpr *)node;
    if (func->funcid != get_cached_agtype_access_operator_oid() ||
        list_length(func->args) != 1)
        return false;

    if (!IsA(linitial(func->args), ArrayExpr))
        return false;

    array = (ArrayExpr *)linitial(func->args);
    if (list_length(array->elements) != 2)
        return false;

    properties_arg = linitial(array->elements);
    key_arg = lsecond(array->elements);
    if (!IsA(properties_arg, Var) || !IsA(key_arg, Const))
        return false;

    properties_var = (Var *)properties_arg;
    if (properties_var->varno != rti ||
        properties_var->vartype != AGTYPEOID ||
        ((Const *)key_arg)->consttype != AGTYPEOID ||
        ((Const *)key_arg)->constisnull)
        return false;

    *properties = properties_arg;
    *key = key_arg;
    return true;
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
    age_collect_float8_agg_func_oid = InvalidOid;
    age_collect_int8_agg_func_oid = InvalidOid;
    age_collect_text_agg_func_oid = InvalidOid;
    array_agg_anynonarray_agg_func_oid = InvalidOid;
    age_array_agg_property_agg_func_oid = InvalidOid;
    age_array_agg_map2_property_agg_func_oid = InvalidOid;
    age_array_agg_map_property_agg_func_oid = InvalidOid;
    agtype_build_map_nonull_func_oid = InvalidOid;
    age_array_agg_list_property_agg_func_oid = InvalidOid;
    agtype_build_list_func_oid = InvalidOid;
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
