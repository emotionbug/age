/*
 * For PostgreSQL Database Management System:
 * (formerly known as Postgres, then as Postgres95)
 *
 * Portions Copyright (c) 1996-2010, The PostgreSQL Global Development Group
 *
 * Portions Copyright (c) 1994, The Regents of the University of California
 *
 * Permission to use, copy, modify, and distribute this software and its documentation for any purpose,
 * without fee, and without a written agreement is hereby granted, provided that the above copyright notice
 * and this paragraph and the following two paragraphs appear in all copies.
 *
 * IN NO EVENT SHALL THE UNIVERSITY OF CALIFORNIA BE LIABLE TO ANY PARTY FOR DIRECT,
 * INDIRECT, SPECIAL, INCIDENTAL, OR CONSEQUENTIAL DAMAGES, INCLUDING LOST PROFITS,
 * ARISING OUT OF THE USE OF THIS SOFTWARE AND ITS DOCUMENTATION, EVEN IF THE UNIVERSITY
 * OF CALIFORNIA HAS BEEN ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * THE UNIVERSITY OF CALIFORNIA SPECIFICALLY DISCLAIMS ANY WARRANTIES, INCLUDING,
 * BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE.
 *
 * THE SOFTWARE PROVIDED HEREUNDER IS ON AN "AS IS" BASIS, AND THE UNIVERSITY OF CALIFORNIA
 * HAS NO OBLIGATIONS TO PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR MODIFICATIONS.
 */

#include "postgres.h"

#include "nodes/makefuncs.h"
#include "parser/parse_relation.h"
#include "parser/parse_target.h"

#include "parser/cypher_expr.h"
#include "parser/cypher_item.h"
#include "utils/ag_func.h"
#include "utils/agtype.h"

static List *ExpandAllTables(ParseState *pstate, int location);
static List *expand_pnsi_attrs(ParseState *pstate, ParseNamespaceItem *pnsi,
			       int sublevels_up, bool require_col_privs,
                               int location);
static Node *try_transform_path_count_item(cypher_parsestate *cpstate,
                                           Node *node,
                                           ParseExprKind expr_kind);
static Node *try_transform_property_count_item(cypher_parsestate *cpstate,
                                               FuncCall *fn,
                                               FuncCall *count_arg_fn,
                                               ParseExprKind expr_kind);
static char *make_raw_edges_name(const char *var_name);
static char *make_raw_path_count_anchor_name(const char *var_name);
static char *make_raw_properties_name(const char *var_name);

/* see transformTargetEntry() */
TargetEntry *transform_cypher_item(cypher_parsestate *cpstate, Node *node,
                                   Node *expr, ParseExprKind expr_kind,
                                   char *colname, bool resjunk)
{
    ParseState *pstate = (ParseState *)cpstate;

    if (!expr)
    {
        expr = try_transform_path_count_item(cpstate, node, expr_kind);
    }

    if (!expr)
    {
        expr = transform_cypher_expr(cpstate, node, expr_kind);
    }

    if (!colname && !resjunk)
        colname = FigureColname(node);

    return makeTargetEntry((Expr *)expr, (AttrNumber)pstate->p_next_resno++,
                           colname, resjunk);
}

static Node *try_transform_path_count_item(cypher_parsestate *cpstate,
                                           Node *node,
                                           ParseExprKind expr_kind)
{
    ParseState *pstate = (ParseState *)cpstate;
    FuncCall *fn;
    FuncCall *count_arg_fn = NULL;
    Node *func_name_node;
    ColumnRef *path_ref;
    char *path_name;
    char *raw_edges_name;
    char *count_anchor_name;
    Node *raw_edges_var;
    Node *count_anchor_var;
    ColumnRef *count_arg_ref;
    FuncCall *count_fn;
    Node *expr;

    if (expr_kind != EXPR_KIND_SELECT_TARGET || node == NULL ||
        !IsA(node, FuncCall))
    {
        return NULL;
    }

    fn = (FuncCall *)node;
    if (fn->funcname == NIL)
    {
        return NULL;
    }

    func_name_node = llast(fn->funcname);
    if (!IsA(func_name_node, String))
    {
        return NULL;
    }

    if (pg_strcasecmp(strVal(func_name_node), "int8_to_agtype") == 0 &&
        list_length(fn->args) == 1 && IsA(linitial(fn->args), FuncCall))
    {
        FuncCall *inner_fn = (FuncCall *)linitial(fn->args);
        Node *inner_name_node;

        if (inner_fn->funcname == NIL)
        {
            return NULL;
        }

        inner_name_node = llast(inner_fn->funcname);
        if (!IsA(inner_name_node, String) ||
            pg_strcasecmp(strVal(inner_name_node), "count") != 0)
        {
            return NULL;
        }

        count_arg_fn = inner_fn;
    }
    else if (pg_strcasecmp(strVal(func_name_node), "count") == 0)
    {
        count_arg_fn = fn;
    }
    else
    {
        return NULL;
    }

    if (list_length(count_arg_fn->args) != 1 || count_arg_fn->agg_star ||
        count_arg_fn->agg_distinct)
    {
        return NULL;
    }

    expr = try_transform_property_count_item(cpstate, fn, count_arg_fn,
                                             expr_kind);
    if (expr != NULL)
    {
        return expr;
    }

    if (!IsA(linitial(count_arg_fn->args), ColumnRef))
    {
        return NULL;
    }

    path_ref = (ColumnRef *)linitial(count_arg_fn->args);
    if (list_length(path_ref->fields) != 1 ||
        !IsA(linitial(path_ref->fields), String))
    {
        return NULL;
    }

    path_name = strVal(linitial(path_ref->fields));
    count_anchor_name = make_raw_path_count_anchor_name(path_name);
    count_anchor_var = colNameToVar(pstate, count_anchor_name, false,
                                    path_ref->location);
    if (count_anchor_var != NULL)
    {
        count_arg_ref = makeNode(ColumnRef);
        count_arg_ref->fields = list_make1(makeString(count_anchor_name));
        count_arg_ref->location = path_ref->location;
    }
    else
    {
        pfree(count_anchor_name);
        count_anchor_name = NULL;
    }

    raw_edges_name = make_raw_edges_name(path_name);
    raw_edges_var = colNameToVar(pstate, raw_edges_name, false,
                                 path_ref->location);
    if (count_anchor_name == NULL && raw_edges_var == NULL)
    {
        pfree(raw_edges_name);
        return NULL;
    }

    if (count_anchor_name == NULL)
    {
        count_arg_ref = makeNode(ColumnRef);
        count_arg_ref->fields = list_make1(makeString(raw_edges_name));
        count_arg_ref->location = path_ref->location;
    }

    count_fn = copyObject(count_arg_fn);
    count_fn->args = list_make1(count_arg_ref);

    if (count_arg_fn == fn)
    {
        expr = transform_cypher_expr(cpstate, (Node *)count_fn, expr_kind);
    }
    else
    {
        FuncCall *wrapper_fn = copyObject(fn);

        wrapper_fn->args = list_make1(count_fn);
        expr = transform_cypher_expr(cpstate, (Node *)wrapper_fn, expr_kind);
    }
    if (count_anchor_name != NULL)
        pfree(count_anchor_name);
    pfree(raw_edges_name);

    return expr;
}

static Node *try_transform_property_count_item(cypher_parsestate *cpstate,
                                               FuncCall *fn,
                                               FuncCall *count_arg_fn,
                                               ParseExprKind expr_kind)
{
    ParseState *pstate = (ParseState *)cpstate;
    ColumnRef *property_ref;
    A_Indirection *property_indirection;
    char *var_name;
    char *property_name;
    char *raw_props_name;
    Node *raw_props_var;
    Const *property_key;
    FuncExpr *exists_expr;
    FuncCall *count_fn;
    Node *expr;

    if (IsA(linitial(count_arg_fn->args), A_Indirection))
    {
        property_indirection = (A_Indirection *)linitial(count_arg_fn->args);
        if (!IsA(property_indirection->arg, ColumnRef) ||
            list_length(property_indirection->indirection) != 1 ||
            !IsA(linitial(property_indirection->indirection), String))
        {
            return NULL;
        }

        property_ref = castNode(ColumnRef, property_indirection->arg);
        if (list_length(property_ref->fields) != 1 ||
            !IsA(linitial(property_ref->fields), String))
        {
            return NULL;
        }

        var_name = strVal(linitial(property_ref->fields));
        property_name = strVal(linitial(property_indirection->indirection));
    }
    else if (IsA(linitial(count_arg_fn->args), ColumnRef))
    {
        property_ref = (ColumnRef *)linitial(count_arg_fn->args);
        if (list_length(property_ref->fields) != 2 ||
            !IsA(linitial(property_ref->fields), String) ||
            !IsA(lsecond(property_ref->fields), String))
        {
            return NULL;
        }

        var_name = strVal(linitial(property_ref->fields));
        property_name = strVal(lsecond(property_ref->fields));
    }
    else
    {
        return NULL;
    }
    raw_props_name = make_raw_properties_name(var_name);
    raw_props_var = colNameToVar(pstate, raw_props_name, false,
                                 property_ref->location);
    pfree(raw_props_name);
    if (raw_props_var == NULL)
        return NULL;

    property_key = makeConst(AGTYPEOID, -1, InvalidOid, -1,
                             string_to_agtype(property_name), false, false);
    exists_expr = makeFuncExpr(
        get_ag_func_oid("agtype_object_field_exists_nonnull", 2,
                        AGTYPEOID, AGTYPEOID),
        BOOLOID,
        list_make2(raw_props_var, property_key),
        InvalidOid, InvalidOid, COERCE_EXPLICIT_CALL);
    exists_expr->location = property_ref->location;

    count_fn = copyObject(count_arg_fn);
    count_fn->args = list_make1(exists_expr);

    if (count_arg_fn == fn)
    {
        expr = transform_cypher_expr(cpstate, (Node *)count_fn, expr_kind);
    }
    else
    {
        FuncCall *wrapper_fn = copyObject(fn);

        wrapper_fn->args = list_make1(count_fn);
        expr = transform_cypher_expr(cpstate, (Node *)wrapper_fn, expr_kind);
    }

    return expr;
}

static char *make_raw_edges_name(const char *var_name)
{
    return psprintf("%sraw_%s_edges", AGE_DEFAULT_ALIAS_PREFIX, var_name);
}

static char *make_raw_path_count_anchor_name(const char *var_name)
{
    return psprintf("%sraw_%s_count_anchor", AGE_DEFAULT_ALIAS_PREFIX,
                    var_name);
}

static char *make_raw_properties_name(const char *var_name)
{
    return psprintf("%sraw_%s_properties", AGE_DEFAULT_ALIAS_PREFIX, var_name);
}

/* see transformTargetList() */
List *transform_cypher_item_list(cypher_parsestate *cpstate, List *item_list,
                                 List **groupClause, ParseExprKind expr_kind)
{
    List *target_list = NIL;
    ListCell *li;
    List *group_clause = NIL;
    bool hasAgg = false;
    bool expand_star;

    expand_star = (expr_kind != EXPR_KIND_UPDATE_SOURCE);

    foreach (li, item_list)
    {
        ResTarget *item = lfirst(li);
        TargetEntry *te;

        if (expand_star)
        {
            if (IsA(item->val, ColumnRef))
            {
                ColumnRef  *cref = (ColumnRef *) item->val;

                if (IsA(llast(cref->fields), A_Star))
                {
                    ParseState *pstate = &cpstate->pstate;

                    /* we only allow a bare '*' */
                    if (list_length(cref->fields) != 1)
                    {
                        ereport(ERROR, (errcode(ERRCODE_SYNTAX_ERROR),
                                        errmsg("Invalid number of fields for *"),
                                        parser_errposition(pstate,
                                                           cref->location)));
                    }

                    target_list = list_concat(target_list,
                                              ExpandAllTables(pstate,
                                                              cref->location));
                    continue;
                }
            }
        }
        /* Clear the exprHasAgg flag to check transform for an aggregate */
        cpstate->exprHasAgg = false;

        /* transform the item */
        te = transform_cypher_item(cpstate, item->val, NULL, expr_kind,
                                   item->name, false);

        target_list = lappend(target_list, te);

        /*
         * Did the transformed item contain an aggregate function? If it didn't,
         * add it to the potential group_clause. If it did, flag that we found
         * an aggregate in an expression
         */
        if (!cpstate->exprHasAgg)
        {
            group_clause = lappend(group_clause, item->val);
        }
        else
        {
            hasAgg = true;
        }
    }

    /*
     * If we found an aggregate function, we need to return the group_clause,
     * even if NIL. parseCheckAggregates at the end of transform_cypher_return
     * will verify if it is valid.
     */
    if (hasAgg)
    {
        *groupClause = group_clause;
    }

    return target_list;
}

/*
 * From PG's ExpandAllTables()
 *     Transforms '*' (in the target list) into a list of targetlist entries.
 */
static List *ExpandAllTables(ParseState *pstate, int location)
{
    List *target = NIL;
    bool found_table = false;
    ListCell *l;

    foreach(l, pstate->p_namespace)
    {
        ParseNamespaceItem *nsitem = (ParseNamespaceItem *) lfirst(l);

        /* Ignore table-only items */
        if (!nsitem->p_cols_visible)
            continue;
        /* Should not have any lateral-only items when parsing targetlist */
        Assert(!nsitem->p_lateral_only);
        /* Remember we found a p_cols_visible item */
        found_table = true;

        target = list_concat(target, expand_pnsi_attrs(pstate, nsitem, 0, true,
                                                       location));
    }

    /* Check for "RETURN *;" */
    if (!found_table)
        ereport(ERROR, (errcode(ERRCODE_SYNTAX_ERROR),
                        errmsg("RETURN * without a pattern is not valid"),
                        parser_errposition(pstate, location)));

    return target;
}

/*
 * From PG's expandNSItemAttrs
 * Modified to exclude hidden variables and aliases in RETURN *
 */
static List *expand_pnsi_attrs(ParseState *pstate, ParseNamespaceItem *pnsi,
			       int sublevels_up, bool require_col_privs,
                               int location)
{
    RangeTblEntry *rte = pnsi->p_rte;
    RTEPermissionInfo *perminfo = pnsi->p_perminfo;
    List *names, *vars;
    ListCell *name, *var;
    List *te_list = NIL;
    int var_prefix_len = sizeof(AGE_DEFAULT_VARNAME_PREFIX) - 1;
    int alias_prefix_len = sizeof(AGE_DEFAULT_ALIAS_PREFIX) - 1;

    vars = expandNSItemVars(pstate, pnsi, sublevels_up, location, &names);

    /*
     * Require read access to the table.  This is normally redundant with the
     * markVarForSelectPriv calls below, but not if the table has zero
     * columns.
     */
    if (rte->rtekind == RTE_RELATION)
     {
         Assert(perminfo != NULL);
         perminfo->requiredPerms |= ACL_SELECT;
     }

    /* iterate through the variables */
    forboth(name, names, var, vars)
    {
        char *label = strVal(lfirst(name));
        Var *varnode = (Var *)lfirst(var);
        TargetEntry *te;

        /* we want to skip our "hidden" variables */
        if (strncmp(AGE_DEFAULT_VARNAME_PREFIX, label, var_prefix_len) == 0)
            continue;

        /* we want to skip out "hidden" aliases */
        if (strncmp(AGE_DEFAULT_ALIAS_PREFIX, label, alias_prefix_len) == 0)
            continue;

        /* add this variable to the list */
        te = makeTargetEntry((Expr *)varnode,
                             (AttrNumber)pstate->p_next_resno++, label, false);
        te_list = lappend(te_list, te);

        /* Require read access to each column */
        markVarForSelectPriv(pstate, varnode);
    }

    Assert(name == NULL && var == NULL);    /* lists not the same length? */

    return te_list;
}
