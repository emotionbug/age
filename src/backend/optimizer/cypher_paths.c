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

#include "catalog/pg_statistic.h"
#include "catalog/pg_type_d.h"
#include "executor/cypher_adjacency_match.h"
#include "nodes/makefuncs.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/restrictinfo.h"
#include "rewrite/rewriteManip.h"
#include "utils/inval.h"
#include "utils/memutils.h"
#include "utils/spccache.h"
#include "utils/syscache.h"

#include "optimizer/cypher_pathnode.h"
#include "optimizer/cypher_paths.h"
#include "utils/ag_func.h"
#include "utils/ag_guc.h"

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
static List *adjacency_match_candidates = NIL;

static Oid cypher_create_clause_func_oid = InvalidOid;
static Oid cypher_set_clause_func_oid = InvalidOid;
static Oid cypher_delete_clause_func_oid = InvalidOid;
static Oid cypher_merge_clause_func_oid = InvalidOid;
static bool cypher_clause_func_callback_registered = false;

static void set_rel_pathlist(PlannerInfo *root, RelOptInfo *rel, Index rti,
                             RangeTblEntry *rte);
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
static CypherAdjacencyMatchCandidate *pop_adjacency_match_candidate(
    PlannerInfo *root, RangeTblEntry *rte);
static void bind_adjacency_match_candidate_outer_relids(
    PlannerInfo *root, CypherAdjacencyMatchCandidate *candidate);

static const CustomPathMethods age_adjacency_match_path_methods = {
    AGE_ADJACENCY_MATCH_SCAN_NAME,
    plan_age_adjacency_match_path,
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
    RegisterCustomScanMethods(&age_adjacency_match_scan_methods);
}

void set_rel_pathlist_fini(void)
{
    set_rel_pathlist_hook = prev_set_rel_pathlist_hook;
}

static void set_rel_pathlist(PlannerInfo *root, RelOptInfo *rel, Index rti,
                             RangeTblEntry *rte)
{
    CypherAdjacencyMatchCandidate *candidate;

    if (prev_set_rel_pathlist_hook)
        prev_set_rel_pathlist_hook(root, rel, rti, rte);

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

    if (!age_enable_adjacency_match_custom_path ||
        candidate == NULL ||
        candidate->bound_endpoint_expr == NULL ||
        candidate->required_outer == NULL ||
        candidate->has_edge_variable_projection ||
        candidate->has_edge_property_predicate ||
        candidate->has_right_property_predicate)
    {
        return;
    }

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
    cp->flags = 0;
    cp->custom_paths = NIL;
    cp->custom_private = list_make3(
        candidate->bound_endpoint_expr,
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

    (void) root;
    (void) custom_plans;

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
    cs->custom_scan_tlist = NIL;
    cs->custom_relids = bms_make_singleton(rel->relid);
    cs->methods = &age_adjacency_match_scan_methods;

    return (Plan *)cs;
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
