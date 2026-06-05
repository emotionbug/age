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
#include "catalog/ag_namespace.h"
#include "catalog/pg_type_d.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/primnodes.h"
#include "optimizer/tlist.h"
#include "parser/parse_func.h"
#include "parser/cypher_property_signature.h"
#include "rewrite/rewriteManip.h"
#include "utils/array.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/relcache.h"

#include "optimizer/cypher_property_paths.h"
#include "utils/ag_func.h"
#include "utils/agtype.h"

static Oid age_collect_agg_func_oid = InvalidOid;
static Oid array_agg_anynonarray_agg_func_oid = InvalidOid;
static Oid age_collect_numeric_property_agg_func_oid = InvalidOid;
static Oid age_collect_numeric_path_property_agg_func_oid = InvalidOid;
static Oid age_collect_float8_agg_func_oid = InvalidOid;
static Oid age_collect_int8_agg_func_oid = InvalidOid;
static Oid age_collect_numeric_agg_func_oid = InvalidOid;
static Oid age_collect_text_agg_func_oid = InvalidOid;
static Oid age_array_agg_property_agg_func_oid = InvalidOid;
static Oid age_array_agg_map2_property_agg_func_oid = InvalidOid;
static Oid age_array_agg_map_property_agg_func_oid = InvalidOid;
static Oid age_array_agg_list_property_agg_func_oid = InvalidOid;
static Oid agtype_build_map_nonull_func_oid = InvalidOid;
static Oid agtype_build_list_func_oid = InvalidOid;
static Oid agtype_ctid_property_field_agtype_func_oid = InvalidOid;
static Oid agtype_id_property_field_agtype_func_oid = InvalidOid;
static Oid int8_to_agtype_func_oid = InvalidOid;
static Oid float8_to_agtype_func_oid = InvalidOid;
static Oid numeric_to_agtype_func_oid = InvalidOid;
static Oid text_to_agtype_func_oid = InvalidOid;

typedef struct CypherTypedCollectDescriptor
{
    Oid value_type;
    const char *agg_name;
    Oid *agg_oid;
} CypherTypedCollectDescriptor;

typedef struct CypherScalarPhysicalDescriptor
{
    Oid value_type;
    /* InvalidOid means the helper returns AGTYPEOID, resolved at runtime. */
    Oid field_result_type;
    const char *final_name;
    Oid *final_oid;
} CypherScalarPhysicalDescriptor;

static CypherTypedCollectDescriptor typed_collect_descriptors[] = {
    {FLOAT8OID, "age_collect_float8", &age_collect_float8_agg_func_oid},
    {INT8OID, "age_collect_int8", &age_collect_int8_agg_func_oid},
    {NUMERICOID, "age_collect_numeric", &age_collect_numeric_agg_func_oid},
    {TEXTOID, "age_collect_text", &age_collect_text_agg_func_oid}
};

static CypherScalarPhysicalDescriptor scalar_physical_descriptors[] = {
    {INT8OID, INT8OID, "int8_to_agtype", &int8_to_agtype_func_oid},
    {FLOAT8OID, FLOAT8OID, "float8_to_agtype", &float8_to_agtype_func_oid},
    {NUMERICOID, InvalidOid, NULL, NULL},
    {NUMERICOID, NUMERICOID, "numeric_to_agtype",
     &numeric_to_agtype_func_oid},
    {TEXTOID, TEXTOID, "text_to_agtype", &text_to_agtype_func_oid}
};

static bool is_ag_catalog_aggref(Aggref *aggref);
static bool is_array_agg_agtype_aggref(Aggref *aggref);
static bool rewrite_array_agg_property_access_expr(Node *node);
static bool rewrite_array_agg_map2_property_access_expr(Node *node);
static bool rewrite_array_agg_map_property_access_expr(Node *node);
static bool rewrite_array_agg_list_property_access_expr(Node *node);
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
static Const *make_property_path_agtype_const(List *keys);
static bool extract_agtype_const_string(Const *key, agtype_value *value);
static Const *make_const_array_const(List *elements, Oid array_type,
                                     Oid element_type);
static bool extract_typed_collect_handoff(Aggref *aggref,
                                          bool require_distinct,
                                          CypherTypedCollectHandoff *handoff);
static bool init_property_handoff_descriptor(
    Node *expr, Oid final_func_oid, Oid agg_func_oid, Node *index_expr,
    CypherPropertyHandoffDescriptor *descriptor);
static void init_property_index_handoff(Node *expr,
                                        CypherPropertyIndexHandoff *handoff);
static bool property_index_expr_matches(Node *index_expr,
                                        CypherPropertyIndexHandoff *handoff);
static void set_property_index_handoff_expr(Node *index_expr,
                                            CypherPropertyIndexHandoff *handoff,
                                            bool copy_expr);
static Oid get_cached_age_collect_oid(void);
static Oid get_cached_array_agg_anynonarray_agg_oid(void);
static Oid get_cached_age_collect_numeric_property_agg_oid(void);
static Oid get_cached_age_collect_numeric_path_property_agg_oid(void);
static Oid get_cached_typed_collect_agg_oid(Oid value_type);
static bool is_typed_collect_agg_oid(Oid agg_oid, Oid *value_type);
static Oid get_cached_age_array_agg_property_agg_oid(void);
static Oid get_cached_age_array_agg_map2_property_agg_oid(void);
static Oid get_cached_age_array_agg_map_property_agg_oid(void);
static Oid get_cached_age_array_agg_list_property_agg_oid(void);
static Oid get_cached_agtype_build_map_nonull_oid(void);
static Oid get_cached_agtype_build_list_oid(void);
static Oid get_scalar_property_field_result_type(
    CypherScalarPhysicalDescriptor *descriptor);
static CypherScalarPhysicalDescriptor *find_scalar_property_field_descriptor(
    Oid funcid, Oid result_type);
static Oid get_cached_agtype_ctid_property_field_agtype_oid(void);
static Oid get_cached_agtype_id_property_field_agtype_oid(void);
static Oid get_cached_scalar_to_agtype_oid(
    CypherScalarPhysicalDescriptor *descriptor);
static CypherScalarPhysicalDescriptor *find_scalar_to_agtype_descriptor(
    Oid funcid, Oid value_type);
static Node *append_property_path_field_expr(Node *expr, List *keys);
static Const *make_agtype_key_array_const(List *keys);

bool cypher_rewrite_collect_typed_scalar_expr(Node *node)
{
    Aggref *aggref;
    CypherTypedCollectHandoff handoff;

    if (node == NULL || !IsA(node, Aggref))
        return false;

    aggref = castNode(Aggref, node);
    if (!extract_typed_collect_handoff(aggref, false, &handoff))
        return false;

    aggref->aggfnoid = handoff.agg_func_oid;
    aggref->aggvariadic = false;
    aggref->aggargtypes = list_make1_oid(handoff.value_type);

    return true;
}

bool cypher_rewrite_collect_numeric_property_expr(Node *node)
{
    Aggref *aggref;
    TargetEntry *arg_tle;
    FuncExpr *func;
    CypherPropertyAccessSignature signature;
    TargetEntry *properties_tle;
    TargetEntry *key_tle;

    if (node == NULL || !IsA(node, Aggref))
        return false;

    aggref = castNode(Aggref, node);
    if (!is_ag_catalog_aggref(aggref) ||
        aggref->aggfnoid != get_cached_age_collect_oid() ||
        aggref->aggstar ||
        aggref->aggdirectargs != NIL ||
        aggref->aggorder != NIL ||
        aggref->aggdistinct != NIL ||
        aggref->aggfilter != NULL ||
        list_length(aggref->aggargtypes) != 1 ||
        linitial_oid(aggref->aggargtypes) != AGTYPEOID ||
        list_length(aggref->args) != 1)
    {
        return false;
    }

    arg_tle = linitial_node(TargetEntry, aggref->args);
    if (arg_tle == NULL || arg_tle->expr == NULL ||
        !IsA(arg_tle->expr, FuncExpr))
    {
        return false;
    }

    func = castNode(FuncExpr, arg_tle->expr);
    if (find_scalar_property_field_descriptor(func->funcid, AGTYPEOID) ==
        NULL ||
        list_length(func->args) != 2)
    {
        return false;
    }

    if (cypher_extract_property_access_signature((Node *)func, &signature) &&
        list_length(signature.keys) > 1)
    {
        Const *key_array;

        key_array = make_agtype_key_array_const(signature.keys);
        if (key_array == NULL)
            return false;

        properties_tle = makeTargetEntry(
            (Expr *)copyObject(signature.container), 1, NULL, false);
        key_tle = makeTargetEntry((Expr *)key_array, 2, NULL, false);

        aggref->aggfnoid =
            get_cached_age_collect_numeric_path_property_agg_oid();
        aggref->aggargtypes = list_make2_oid(AGTYPEOID, AGTYPEARRAYOID);
    }
    else
    {
        properties_tle = makeTargetEntry(
            (Expr *)copyObject(linitial(func->args)), 1, NULL, false);
        key_tle = makeTargetEntry(
            (Expr *)copyObject(lsecond(func->args)), 2, NULL, false);

        aggref->aggfnoid = get_cached_age_collect_numeric_property_agg_oid();
        aggref->aggargtypes = list_make2_oid(AGTYPEOID, AGTYPEOID);
    }

    aggref->args = list_make2(properties_tle, key_tle);
    aggref->aggvariadic = false;

    return true;
}

bool cypher_rewrite_array_agg_property_expr(Node *node)
{
    return rewrite_array_agg_property_access_expr(node) ||
        rewrite_array_agg_map2_property_access_expr(node) ||
        rewrite_array_agg_map_property_access_expr(node) ||
        rewrite_array_agg_list_property_access_expr(node);
}

static bool rewrite_array_agg_property_access_expr(Node *node)
{
    Aggref *aggref;
    TargetEntry *arg_tle;
    TargetEntry *properties_tle;
    TargetEntry *key_tle;
    Node *properties;
    Node *key;

    if (node == NULL || !IsA(node, Aggref))
        return false;

    aggref = castNode(Aggref, node);
    if (!is_array_agg_agtype_aggref(aggref))
        return false;

    arg_tle = linitial_node(TargetEntry, aggref->args);
    if (arg_tle == NULL ||
        !extract_property_access_args((Node *)arg_tle->expr, &properties,
                                      &key))
    {
        return false;
    }

    properties_tle = makeTargetEntry(
        (Expr *)copyObject(properties), 1, NULL, false);
    key_tle = makeTargetEntry((Expr *)copyObject(key), 2, NULL, false);

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

bool cypher_find_matching_property_index_handoff(
    RelOptInfo *rel, Node *expr, CypherPropertyIndexHandoff *handoff)
{
    ListCell *index_lc;

    if (handoff == NULL)
        return false;

    init_property_index_handoff(expr, handoff);
    if (rel == NULL || expr == NULL)
        return false;

    foreach(index_lc, rel->indexlist)
    {
        IndexOptInfo *index = lfirst_node(IndexOptInfo, index_lc);
        ListCell *expr_lc;

        foreach(expr_lc, index->indexprs)
        {
            Node *index_expr = lfirst(expr_lc);

            if (property_index_expr_matches(index_expr, handoff))
            {
                set_property_index_handoff_expr(index_expr, handoff, false);
                return true;
            }
        }
    }

    return false;
}

bool cypher_find_matching_property_index_handoff_for_rte(
    RangeTblEntry *rte, Index rti, Node *expr,
    CypherPropertyIndexHandoff *handoff)
{
    Relation rel;
    List *index_oids;
    ListCell *lc;

    if (handoff == NULL)
        return false;

    init_property_index_handoff(expr, handoff);
    if (rte == NULL || !OidIsValid(rte->relid) || expr == NULL)
        return false;

    rel = table_open(rte->relid, AccessShareLock);
    index_oids = RelationGetIndexList(rel);

    foreach(lc, index_oids)
    {
        Oid index_oid = lfirst_oid(lc);
        Relation index_rel;
        List *index_exprs;
        ListCell *expr_lc;

        index_rel = index_open(index_oid, AccessShareLock);
        if (!index_rel->rd_index->indisvalid)
        {
            index_close(index_rel, AccessShareLock);
            continue;
        }

        index_exprs = RelationGetIndexExpressions(index_rel);
        if (index_exprs != NIL && rti != 1)
            ChangeVarNodes((Node *)index_exprs, 1, rti, 0);

        foreach(expr_lc, index_exprs)
        {
            Node *index_expr = lfirst(expr_lc);

            if (property_index_expr_matches(index_expr, handoff))
            {
                set_property_index_handoff_expr(index_expr, handoff, true);
                break;
            }
        }

        index_close(index_rel, AccessShareLock);
        if (handoff->index_expr != NULL)
            break;
    }

    list_free(index_oids);
    table_close(rel, AccessShareLock);

    return handoff->index_expr != NULL;
}

bool cypher_make_cached_property_slot_descriptor(
    const CypherPropertyHandoffDescriptor *handoff,
    CypherCachedPropertySlotDescriptor *slot)
{
    if (handoff == NULL || slot == NULL)
        return false;

    memset(slot, 0, sizeof(CypherCachedPropertySlotDescriptor));
    if (handoff->property_signature.container == NULL ||
        handoff->property_signature.keys == NIL)
    {
        return false;
    }

    slot->container = handoff->property_signature.container;
    slot->keys = handoff->property_signature.keys;
    slot->value_type = handoff->property_signature.value_type;
    slot->field_result_type = handoff->property_signature.field_result_type;
    slot->final_func_oid = handoff->final_func_oid;
    slot->agg_func_oid = handoff->agg_func_oid;
    slot->index_expr = handoff->index_expr;

    return true;
}

Node *cypher_make_cached_property_slot_expr(
    const CypherCachedPropertySlotDescriptor *slot)
{
    Node *expr;
    ListCell *lc;
    int key_index = 0;
    int key_count;

    if (slot == NULL || slot->container == NULL || slot->keys == NIL)
        return NULL;

    key_count = list_length(slot->keys);
    expr = copyObject(slot->container);

    foreach(lc, slot->keys)
    {
        Node *key = lfirst(lc);
        bool terminal_key = (++key_index == key_count);
        Oid value_type = terminal_key ? slot->value_type : AGTYPEOID;
        Oid result_type = terminal_key ? slot->field_result_type : AGTYPEOID;
        Oid field_oid;

        field_oid = cypher_get_property_field_oid(value_type, result_type);
        if (!OidIsValid(field_oid))
            return NULL;

        expr = (Node *)makeFuncExpr(field_oid,
                                    result_type,
                                    list_make2(expr, copyObject(key)),
                                    InvalidOid, InvalidOid,
                                    COERCE_EXPLICIT_CALL);
    }

    return expr;
}

bool cypher_find_typed_distinct_collect_handoff(
    PathTarget *target, CypherTypedCollectHandoff *handoff)
{
    Aggref *found = NULL;
    CypherTypedCollectHandoff found_handoff;
    ListCell *lc;

    if (handoff == NULL)
        return false;

    memset(handoff, 0, sizeof(CypherTypedCollectHandoff));

    if (target == NULL)
        return false;

    foreach(lc, target->exprs)
    {
        Node *expr = lfirst(lc);
        Aggref *aggref;
        CypherTypedCollectHandoff current_handoff;

        if (expr == NULL || !IsA(expr, Aggref))
            continue;

        aggref = castNode(Aggref, expr);
        if (!is_typed_collect_agg_oid(aggref->aggfnoid, NULL))
            continue;

        if (!extract_typed_collect_handoff(aggref, true, &current_handoff))
            return false;

        if (found != NULL)
            return false;

        found = aggref;
        found_handoff = current_handoff;
    }

    if (found == NULL)
        return false;

    *handoff = found_handoff;
    return true;
}

bool cypher_extract_typed_property_sort_args(Node *node, Node **properties,
                                             Node **key)
{
    FuncExpr *func;
    CypherScalarPhysicalDescriptor *descriptor;

    if (node == NULL || !IsA(node, FuncExpr))
        return false;

    func = castNode(FuncExpr, node);
    if (list_length(func->args) != 2)
        return false;

    descriptor = find_scalar_property_field_descriptor(func->funcid,
                                                       exprType(node));
    if (descriptor == NULL)
        return false;

    *properties = linitial(func->args);
    *key = lsecond(func->args);

    return *properties != NULL &&
        *key != NULL &&
        exprType(*properties) == AGTYPEOID &&
        exprType(*key) == AGTYPEOID;
}

Node *cypher_make_ctid_property_field_agtype_expr(Oid relid, Node *ctid,
                                                  AttrNumber properties_attno,
                                                  Node *key)
{
    Const *relid_const;
    Const *attno_const;

    relid_const = makeConst(OIDOID, -1, InvalidOid, sizeof(Oid),
                            ObjectIdGetDatum(relid), false, true);
    attno_const = makeConst(INT4OID, -1, InvalidOid, sizeof(int32),
                            Int32GetDatum((int32)properties_attno), false,
                            true);

    return (Node *)makeFuncExpr(
        get_cached_agtype_ctid_property_field_agtype_oid(),
        AGTYPEOID,
        list_make4(relid_const,
                   copyObject(ctid),
                   attno_const,
                   copyObject(key)),
        InvalidOid, InvalidOid,
        COERCE_EXPLICIT_CALL);
}

Node *cypher_make_id_property_field_agtype_expr(Oid relid, Node *id,
                                                AttrNumber properties_attno,
                                                Node *key)
{
    Const *relid_const;
    Const *attno_const;

    relid_const = makeConst(OIDOID, -1, InvalidOid, sizeof(Oid),
                            ObjectIdGetDatum(relid), false, true);
    attno_const = makeConst(INT4OID, -1, InvalidOid, sizeof(int32),
                            Int32GetDatum((int32)properties_attno), false,
                            true);

    return (Node *)makeFuncExpr(
        get_cached_agtype_id_property_field_agtype_oid(),
        AGTYPEOID,
        list_make4(relid_const,
                   copyObject(id),
                   attno_const,
                   copyObject(key)),
        InvalidOid, InvalidOid,
        COERCE_EXPLICIT_CALL);
}

Node *cypher_make_ctid_property_path_field_agtype_expr(
    Oid relid, Node *ctid, AttrNumber properties_attno, List *keys)
{
    Node *expr;

    if (keys == NIL)
        return NULL;

    expr = cypher_make_ctid_property_field_agtype_expr(
        relid, ctid, properties_attno, linitial(keys));

    return append_property_path_field_expr(expr, list_copy_tail(keys, 1));
}

Node *cypher_make_id_property_path_field_agtype_expr(
    Oid relid, Node *id, AttrNumber properties_attno, List *keys)
{
    Node *expr;

    if (keys == NIL)
        return NULL;

    expr = cypher_make_id_property_field_agtype_expr(
        relid, id, properties_attno, linitial(keys));

    return append_property_path_field_expr(expr, list_copy_tail(keys, 1));
}

static Node *append_property_path_field_expr(Node *expr, List *keys)
{
    ListCell *lc;

    foreach(lc, keys)
    {
        expr = (Node *)makeFuncExpr(cypher_get_property_field_oid(AGTYPEOID,
                                                                  AGTYPEOID),
                                    AGTYPEOID,
                                    list_make2(expr, copyObject(lfirst(lc))),
                                    InvalidOid, InvalidOid,
                                    COERCE_EXPLICIT_CALL);
    }

    return expr;
}

static bool extract_property_access_args(Node *node, Node **properties,
                                         Node **key)
{
    CypherPropertyAccessSignature signature;
    FuncExpr *func;
    ArrayExpr *array;

    if (cypher_extract_property_access_signature(node, &signature))
    {
        *properties = signature.container;
        *key = (Node *)make_property_path_agtype_const(signature.keys);
        return *properties != NULL && *key != NULL;
    }

    if (node == NULL || !IsA(node, FuncExpr))
        return false;

    func = castNode(FuncExpr, node);
    if (!cypher_property_field_func_matches(func->funcid, AGTYPEOID,
                                            AGTYPEOID))
        return false;

    if (list_length(func->args) == 2)
    {
        *properties = linitial(func->args);
        *key = lsecond(func->args);
    }
    else if (list_length(func->args) == 1 &&
             IsA(linitial(func->args), ArrayExpr))
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
        return false;

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

static Const *make_property_path_agtype_const(List *keys)
{
    agtype_in_state result;
    ListCell *lc;

    if (keys == NIL)
        return NULL;

    if (list_length(keys) == 1)
        return (Const *)copyObject(linitial(keys));

    memset(&result, 0, sizeof(agtype_in_state));
    result.res = push_agtype_value(&result.parse_state, WAGT_BEGIN_ARRAY,
                                   NULL);

    foreach(lc, keys)
    {
        Const *key;
        agtype_value value;

        if (!IsA(lfirst(lc), Const))
            return NULL;

        key = castNode(Const, lfirst(lc));
        if (!extract_agtype_const_string(key, &value))
            return NULL;

        result.res = push_agtype_value(&result.parse_state, WAGT_ELEM,
                                       &value);
    }

    result.res = push_agtype_value(&result.parse_state, WAGT_END_ARRAY,
                                   NULL);

    return makeConst(AGTYPEOID, -1, InvalidOid, -1,
                     PointerGetDatum(agtype_value_to_agtype(result.res)),
                     false, false);
}

static bool extract_agtype_const_string(Const *key, agtype_value *value)
{
    agtype *key_agtype;
    agtype_iterator *it;
    agtype_value tmp;
    agtype_iterator_token token;

    if (key == NULL ||
        key->constisnull ||
        key->consttype != AGTYPEOID)
    {
        return false;
    }

    key_agtype = DATUM_GET_AGTYPE_P(key->constvalue);
    if (!AGT_ROOT_IS_SCALAR(key_agtype))
        return false;

    it = agtype_iterator_init(&key_agtype->root);
    token = agtype_iterator_next(&it, &tmp, false);
    if (token != WAGT_BEGIN_ARRAY)
        return false;

    token = agtype_iterator_next(&it, value, false);
    return token == WAGT_ELEM && value->type == AGTV_STRING;
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

static Const *make_agtype_key_array_const(List *keys)
{
    Datum *values;
    ArrayType *array;
    ListCell *lc;
    int nelems;
    int i = 0;
    int16 typlen;
    bool typbyval;
    char typalign;

    nelems = list_length(keys);
    if (nelems <= 0)
        return NULL;

    values = palloc(sizeof(Datum) * nelems);
    foreach(lc, keys)
    {
        Const *key;

        if (!IsA(lfirst(lc), Const))
            return NULL;

        key = castNode(Const, lfirst(lc));
        if (key->constisnull || key->consttype != AGTYPEOID)
            return NULL;

        values[i++] = key->constvalue;
    }

    get_typlenbyvalalign(AGTYPEOID, &typlen, &typbyval, &typalign);
    array = construct_array(values, nelems, AGTYPEOID, typlen, typbyval,
                            typalign);

    return makeConst(AGTYPEARRAYOID, -1, InvalidOid, -1,
                     PointerGetDatum(array), false, false);
}

static bool extract_typed_collect_handoff(Aggref *aggref,
                                          bool require_distinct,
                                          CypherTypedCollectHandoff *handoff)
{
    TargetEntry *arg_tle;
    Oid value_type = InvalidOid;
    Oid typed_agg_oid = InvalidOid;

    if (handoff == NULL)
        return false;

    memset(handoff, 0, sizeof(CypherTypedCollectHandoff));

    if (aggref == NULL ||
        !is_ag_catalog_aggref(aggref) ||
        aggref->aggstar ||
        aggref->aggdirectargs != NIL ||
        aggref->aggfilter != NULL ||
        list_length(aggref->aggargtypes) != 1 ||
        list_length(aggref->args) != 1)
    {
        return false;
    }

    if (require_distinct && aggref->aggdistinct == NIL)
        return false;

    arg_tle = linitial_node(TargetEntry, aggref->args);
    if (arg_tle == NULL || arg_tle->expr == NULL)
        return false;

    if (aggref->aggfnoid == get_cached_age_collect_oid())
    {
        if (require_distinct)
            return false;

        value_type = exprType((Node *)arg_tle->expr);
        typed_agg_oid = get_cached_typed_collect_agg_oid(value_type);
    }
    else
    {
        if (!is_typed_collect_agg_oid(aggref->aggfnoid, &value_type))
            return false;

        typed_agg_oid = aggref->aggfnoid;
        if (linitial_oid(aggref->aggargtypes) != value_type ||
            exprType((Node *)arg_tle->expr) != value_type)
        {
            return false;
        }
    }

    if (!OidIsValid(typed_agg_oid))
        return false;

    handoff->arg_expr = (Node *)arg_tle->expr;
    handoff->value_type = value_type;
    handoff->agg_func_oid = typed_agg_oid;
    handoff->has_property_descriptor =
        init_property_handoff_descriptor(
            handoff->arg_expr, InvalidOid, handoff->agg_func_oid, NULL,
            &handoff->property_descriptor);

    return true;
}

static bool init_property_handoff_descriptor(
    Node *expr, Oid final_func_oid, Oid agg_func_oid, Node *index_expr,
    CypherPropertyHandoffDescriptor *descriptor)
{
    if (descriptor == NULL)
        return false;

    memset(descriptor, 0, sizeof(CypherPropertyHandoffDescriptor));
    descriptor->final_func_oid = final_func_oid;
    descriptor->agg_func_oid = agg_func_oid;
    descriptor->index_expr = index_expr;

    return cypher_extract_property_access_signature(
        expr, &descriptor->property_signature);
}

static void init_property_index_handoff(Node *expr,
                                        CypherPropertyIndexHandoff *handoff)
{
    Assert(handoff != NULL);

    memset(handoff, 0, sizeof(CypherPropertyIndexHandoff));
    handoff->query_expr = expr;
    handoff->has_property_descriptor =
        init_property_handoff_descriptor(
            expr, InvalidOid, InvalidOid, NULL,
            &handoff->property_descriptor);
}

static bool property_index_expr_matches(Node *index_expr,
                                        CypherPropertyIndexHandoff *handoff)
{
    if (index_expr == NULL || handoff == NULL || handoff->query_expr == NULL)
        return false;

    return equal(handoff->query_expr, index_expr) ||
           (handoff->has_property_descriptor &&
            cypher_property_access_signature_matches(
                &handoff->property_descriptor.property_signature,
                index_expr));
}

static void set_property_index_handoff_expr(Node *index_expr,
                                            CypherPropertyIndexHandoff *handoff,
                                            bool copy_expr)
{
    CypherCachedPropertySlotDescriptor slot;
    Node *slot_expr = NULL;

    if (handoff == NULL || index_expr == NULL)
        return;

    if (handoff->has_property_descriptor &&
        cypher_make_cached_property_slot_descriptor(
            &handoff->property_descriptor, &slot))
    {
        slot_expr = cypher_make_cached_property_slot_expr(&slot);
    }

    if (slot_expr != NULL && equal(slot_expr, index_expr))
        handoff->index_expr = slot_expr;
    else
        handoff->index_expr = copy_expr ? copyObject(index_expr) : index_expr;

    if (handoff->has_property_descriptor)
        handoff->property_descriptor.index_expr = handoff->index_expr;
}

bool cypher_extract_scalar_final_handoff(Node *node,
                                         CypherScalarFinalHandoff *handoff)
{
    FuncExpr *func;
    Node *candidate;
    Oid candidate_type;
    CypherScalarPhysicalDescriptor *descriptor;

    if (handoff == NULL)
        return false;

    memset(handoff, 0, sizeof(CypherScalarFinalHandoff));

    if (node == NULL || !IsA(node, FuncExpr))
        return false;

    func = castNode(FuncExpr, node);
    if (list_length(func->args) != 1)
        return false;

    candidate = linitial(func->args);
    if (candidate == NULL)
        return false;

    candidate_type = exprType(candidate);
    descriptor = find_scalar_to_agtype_descriptor(func->funcid,
                                                  candidate_type);
    if (descriptor == NULL)
        return false;

    handoff->scalar_expr = candidate;
    handoff->value_type = descriptor->value_type;
    handoff->field_result_type = candidate_type;
    handoff->final_func_oid = func->funcid;
    handoff->has_property_descriptor =
        init_property_handoff_descriptor(
            candidate, handoff->final_func_oid, InvalidOid, NULL,
            &handoff->property_descriptor);
    return true;
}

Expr *cypher_add_or_get_lower_scalar_handoff(
    PathTarget *target, CypherScalarFinalHandoff *handoff,
    Index ressortgroupref)
{
    ListCell *lc;
    Expr *expr;

    if (target == NULL || handoff == NULL || handoff->scalar_expr == NULL)
        return NULL;

    expr = (Expr *)handoff->scalar_expr;

    if (ressortgroupref == 0)
    {
        foreach(lc, target->exprs)
        {
            Node *existing = lfirst(lc);

            if (equal(existing, expr) ||
                (handoff->has_property_descriptor &&
                 cypher_property_access_signature_matches(
                     &handoff->property_descriptor.property_signature,
                     existing)))
            {
                return (Expr *)existing;
            }
        }
    }

    add_column_to_pathtarget(target, expr, ressortgroupref);
    return expr;
}

void cypher_property_path_invalidate_oids(void)
{
    age_collect_agg_func_oid = InvalidOid;
    array_agg_anynonarray_agg_func_oid = InvalidOid;
    age_collect_numeric_property_agg_func_oid = InvalidOid;
    age_collect_numeric_path_property_agg_func_oid = InvalidOid;
    age_collect_float8_agg_func_oid = InvalidOid;
    age_collect_int8_agg_func_oid = InvalidOid;
    age_collect_numeric_agg_func_oid = InvalidOid;
    age_collect_text_agg_func_oid = InvalidOid;
    age_array_agg_property_agg_func_oid = InvalidOid;
    age_array_agg_map2_property_agg_func_oid = InvalidOid;
    age_array_agg_map_property_agg_func_oid = InvalidOid;
    age_array_agg_list_property_agg_func_oid = InvalidOid;
    agtype_build_map_nonull_func_oid = InvalidOid;
    agtype_build_list_func_oid = InvalidOid;
    agtype_ctid_property_field_agtype_func_oid = InvalidOid;
    agtype_id_property_field_agtype_func_oid = InvalidOid;
    int8_to_agtype_func_oid = InvalidOid;
    float8_to_agtype_func_oid = InvalidOid;
    numeric_to_agtype_func_oid = InvalidOid;
    text_to_agtype_func_oid = InvalidOid;
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

static Oid get_cached_age_collect_oid(void)
{
    if (!OidIsValid(age_collect_agg_func_oid))
        age_collect_agg_func_oid = get_ag_func_oid("age_collect", 1, ANYOID);

    return age_collect_agg_func_oid;
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

static Oid get_cached_age_collect_numeric_property_agg_oid(void)
{
    if (!OidIsValid(age_collect_numeric_property_agg_func_oid))
    {
        age_collect_numeric_property_agg_func_oid =
            get_ag_func_oid("age_collect_numeric_property", 2,
                            AGTYPEOID, AGTYPEOID);
    }

    return age_collect_numeric_property_agg_func_oid;
}

static Oid get_cached_age_collect_numeric_path_property_agg_oid(void)
{
    if (!OidIsValid(age_collect_numeric_path_property_agg_func_oid))
    {
        age_collect_numeric_path_property_agg_func_oid =
            get_ag_func_oid("age_collect_numeric_path_property", 2,
                            AGTYPEOID, AGTYPEARRAYOID);
    }

    return age_collect_numeric_path_property_agg_func_oid;
}

static Oid get_cached_typed_collect_agg_oid(Oid value_type)
{
    int i;

    for (i = 0; i < lengthof(typed_collect_descriptors); i++)
    {
        CypherTypedCollectDescriptor *descriptor =
            &typed_collect_descriptors[i];

        if (descriptor->value_type != value_type)
            continue;

        if (!OidIsValid(*descriptor->agg_oid))
        {
            *descriptor->agg_oid =
                get_ag_func_oid(descriptor->agg_name, 1,
                                descriptor->value_type);
        }

        return *descriptor->agg_oid;
    }

    return InvalidOid;
}

static bool is_typed_collect_agg_oid(Oid agg_oid, Oid *value_type)
{
    int i;

    if (!OidIsValid(agg_oid))
        return false;

    for (i = 0; i < lengthof(typed_collect_descriptors); i++)
    {
        CypherTypedCollectDescriptor *descriptor =
            &typed_collect_descriptors[i];

        if (get_cached_typed_collect_agg_oid(descriptor->value_type) ==
            agg_oid)
        {
            if (value_type != NULL)
                *value_type = descriptor->value_type;
            return true;
        }
    }

    return false;
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

static CypherScalarPhysicalDescriptor *find_scalar_property_field_descriptor(
    Oid funcid, Oid result_type)
{
    int i;

    if (!OidIsValid(funcid))
        return NULL;

    for (i = 0; i < lengthof(scalar_physical_descriptors); i++)
    {
        CypherScalarPhysicalDescriptor *descriptor =
            &scalar_physical_descriptors[i];

        if (get_scalar_property_field_result_type(descriptor) != result_type)
            continue;

        if (cypher_property_field_func_matches(funcid,
                                               descriptor->value_type,
                                               result_type))
            return descriptor;
    }

    return NULL;
}

static Oid get_scalar_property_field_result_type(
    CypherScalarPhysicalDescriptor *descriptor)
{
    Assert(descriptor != NULL);

    if (!OidIsValid(descriptor->field_result_type))
        return AGTYPEOID;

    return descriptor->field_result_type;
}

static Oid get_cached_agtype_ctid_property_field_agtype_oid(void)
{
    if (!OidIsValid(agtype_ctid_property_field_agtype_func_oid))
    {
        agtype_ctid_property_field_agtype_func_oid =
            get_ag_func_oid("agtype_ctid_property_field_agtype", 4,
                            OIDOID, TIDOID, INT4OID, AGTYPEOID);
    }

    return agtype_ctid_property_field_agtype_func_oid;
}

static Oid get_cached_agtype_id_property_field_agtype_oid(void)
{
    if (!OidIsValid(agtype_id_property_field_agtype_func_oid))
    {
        agtype_id_property_field_agtype_func_oid =
            get_ag_func_oid("agtype_id_property_field_agtype", 4,
                            OIDOID, GRAPHIDOID, INT4OID, AGTYPEOID);
    }

    return agtype_id_property_field_agtype_func_oid;
}

static Oid get_cached_scalar_to_agtype_oid(
    CypherScalarPhysicalDescriptor *descriptor)
{
    Assert(descriptor != NULL);

    if (descriptor->final_name == NULL || descriptor->final_oid == NULL)
        return InvalidOid;

    if (!OidIsValid(*descriptor->final_oid))
    {
        *descriptor->final_oid =
            get_ag_func_oid(descriptor->final_name, 1,
                            descriptor->value_type);
    }

    return *descriptor->final_oid;
}

static CypherScalarPhysicalDescriptor *find_scalar_to_agtype_descriptor(
    Oid funcid, Oid value_type)
{
    int i;

    if (!OidIsValid(funcid))
        return NULL;

    for (i = 0; i < lengthof(scalar_physical_descriptors); i++)
    {
        CypherScalarPhysicalDescriptor *descriptor =
            &scalar_physical_descriptors[i];

        if (descriptor->value_type != value_type)
            continue;

        if (get_cached_scalar_to_agtype_oid(descriptor) == funcid)
            return descriptor;
    }

    return NULL;
}
