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
#include "catalog/ag_label.h"
#include "catalog/ag_namespace.h"
#include "catalog/pg_collation_d.h"
#include "catalog/pg_type_d.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/primnodes.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/restrictinfo.h"
#include "optimizer/tlist.h"
#include "parser/parse_func.h"
#include "parser/parse_oper.h"
#include "parser/cypher_property_signature.h"
#include "rewrite/rewriteManip.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/relcache.h"

#include "optimizer/cypher_property_paths.h"
#include "utils/ag_func.h"
#include "utils/agtype.h"

static Oid age_collect_agg_func_oid = InvalidOid;
static Oid array_agg_anynonarray_agg_func_oid = InvalidOid;
static Oid count_any_agg_func_oid = InvalidOid;
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
static Oid age_array_agg_map_slots_agg_func_oid = InvalidOid;
static Oid age_array_agg_list_slots_agg_func_oid = InvalidOid;
static Oid agtype_build_map_nonull_func_oid = InvalidOid;
static Oid agtype_build_list_func_oid = InvalidOid;
static Oid agtype_ctid_property_field_agtype_func_oid = InvalidOid;
static Oid agtype_id_property_field_agtype_func_oid = InvalidOid;
static Oid int8_to_agtype_func_oid = InvalidOid;
static Oid float8_to_agtype_func_oid = InvalidOid;
static Oid numeric_to_agtype_func_oid = InvalidOid;
static Oid text_to_agtype_func_oid = InvalidOid;
static Oid agtype_eq_func_oid = InvalidOid;
static Oid agtype_lt_func_oid = InvalidOid;
static Oid agtype_le_func_oid = InvalidOid;
static Oid agtype_gt_func_oid = InvalidOid;
static Oid agtype_ge_func_oid = InvalidOid;
static Oid agtype_field_equals_func_oid = InvalidOid;
static Oid agtype_field_cmp_func_oid = InvalidOid;
static Oid agtype_field_exists_nonnull_func_oid = InvalidOid;

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

typedef struct CypherArrayAggPropertyRewriteContext
{
    CypherArrayAggPropertyHandoff *handoff;
    List *arg_plans;
    bool rewritten;
} CypherArrayAggPropertyRewriteContext;

typedef struct CypherCanonicalPropertyIndexExpr
{
    CypherCachedPropertySlotDescriptor slot;
    Node *expr;
} CypherCanonicalPropertyIndexExpr;

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
static bool rewrite_property_access_aggregate_expr(Node *node);
static bool rewrite_property_access_aggregate_walker(Node *node,
                                                     void *context);
static bool rewrite_count_property_access_expr(Node *node);
static bool is_count_any_aggref(Aggref *aggref);
static bool extract_count_property_access_args(Aggref *aggref, Node **object,
                                               Node **key);
static bool extract_property_access_args(Node *node, Node **properties,
                                         Node **key);
static bool extract_property_path_const_keys(Node *node, List **keys);
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
static bool cached_property_slots_share_key_source(
    const CypherCachedPropertySlotDescriptor *left,
    const CypherCachedPropertySlotDescriptor *right);
static bool cached_property_slots_same_physical_signature(
    const CypherCachedPropertySlotDescriptor *left,
    const CypherCachedPropertySlotDescriptor *right);
static bool cached_property_slot_uses_typed_physical_result(
    const CypherCachedPropertySlotDescriptor *slot);
static int cached_property_slot_final_materialization_weight(
    Oid field_result_type);
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
static bool make_cached_property_slot_descriptor(
    const CypherPropertyHandoffDescriptor *handoff,
    CypherCachedPropertySlotDescriptor *slot);
static void refresh_typed_collect_cached_slot(
    CypherTypedCollectHandoff *handoff);
static void refresh_property_index_cached_slot(
    CypherPropertyIndexHandoff *handoff);
static void refresh_scalar_final_cached_slot(
    CypherScalarFinalHandoff *handoff);
static void init_property_index_handoff(Node *expr,
                                        CypherPropertyIndexHandoff *handoff);
static bool property_index_expr_matches(Node *index_expr,
                                        CypherPropertyIndexHandoff *handoff);
static void set_property_index_handoff_expr(Node *index_expr,
                                            CypherPropertyIndexHandoff *handoff,
                                            bool copy_expr);
static bool detect_simple_property_projection_target(PathTarget *target,
                                                     List **slots);
static bool is_simple_property_access_target(
    Node *node, CypherCachedPropertySlotDescriptor *slot);
static bool detect_ordered_property_projection_delay(
    PlannerInfo *root, CypherCachedPropertySlotDescriptor *output_slot,
    CypherCachedPropertySlotDescriptor *sort_slot, bool *reuse_sort_output,
    TargetEntry **sort_tle);
static Node *try_rewrite_property_equals_clause(Node *clause, Index rti,
                                                RelOptInfo *rel);
static bool match_property_access_expr(Node *node, Index rti,
                                       Node **properties, Node **key);
static bool property_object_belongs_to_rti(Node *node, Index rti);
static List *collect_canonical_property_index_exprs(RelOptInfo *rel);
static bool collect_canonical_property_index_exprs_walker(Node *node,
                                                          void *context);
static Node *canonicalize_property_index_exprs_mutator(Node *node,
                                                       void *context);
static CypherCanonicalPropertyIndexExpr *find_canonical_property_index_expr(
    List *exprs, const CypherCachedPropertySlotDescriptor *slot, Node *expr);
static void add_canonical_property_index_expr(
    List **exprs, const CypherCachedPropertySlotDescriptor *slot, Node *expr);
static Expr *make_int4_zero_compare_expr(FuncExpr *cmp_expr,
                                         const char *operator_name);
static const char *property_compare_operator_name(Oid opfuncid,
                                                  bool commuted);
static Oid get_cached_agtype_field_equals_oid(void);
static Oid get_cached_agtype_field_cmp_oid(void);
static Oid get_cached_agtype_field_exists_nonnull_oid(void);
static Oid get_cached_agtype_eq_oid(void);
static Oid get_cached_agtype_cmp_func_oid(const char *name, Oid *cache);
static Oid get_cached_age_collect_oid(void);
static Oid get_cached_array_agg_anynonarray_agg_oid(void);
static Oid get_cached_count_any_agg_oid(void);
static Oid get_cached_age_collect_numeric_property_agg_oid(void);
static Oid get_cached_age_collect_numeric_path_property_agg_oid(void);
static Oid get_cached_typed_collect_agg_oid(Oid value_type);
static bool is_typed_collect_agg_oid(Oid agg_oid, Oid *value_type);
static Oid get_cached_age_array_agg_property_agg_oid(void);
static Oid get_cached_age_array_agg_map2_property_agg_oid(void);
static Oid get_cached_age_array_agg_map_property_agg_oid(void);
static Oid get_cached_age_array_agg_list_property_agg_oid(void);
static Oid get_cached_age_array_agg_map_slots_agg_oid(void);
static Oid get_cached_age_array_agg_list_slots_agg_oid(void);
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
static bool find_array_agg_property_handoff_walker(Node *node,
                                                   void *context);
static Node *rewrite_array_agg_property_target_mutator(Node *node,
                                                       void *context);
static bool make_array_agg_property_handoff_args(
    Aggref *aggref, CypherArrayAggPropertyHandoff *handoff);
static bool make_array_agg_single_property_args(
    Aggref *aggref, CypherArrayAggPropertyHandoff *handoff);
static bool make_array_agg_map2_property_args(
    Aggref *aggref, CypherArrayAggPropertyHandoff *handoff);
static bool make_array_agg_map_property_args(
    Aggref *aggref, CypherArrayAggPropertyHandoff *handoff);
static bool make_array_agg_list_property_args(
    Aggref *aggref, CypherArrayAggPropertyHandoff *handoff);
static bool extract_text_array_const_elements(Node *node, List **elements);
static bool extract_agtype_array_const_elements(Node *node, List **elements);
static Node *find_typed_collect_rewrite_arg(Aggref *aggref, List *arg_plans);
static Node *make_typed_collect_handoff_arg(
    CypherTypedCollectHandoff *handoff);
static Expr *add_or_get_lower_scalar_handoff(
    PathTarget *target, CypherScalarFinalHandoff *handoff,
    Index ressortgroupref);
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

bool cypher_rewrite_property_access_aggregate_pathtarget(PathTarget *target)
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

Node *cypher_make_property_index_handoff_expr(
    CypherPropertyIndexHandoff *handoff)
{
    Node *slot_expr;

    if (handoff == NULL || handoff->index_expr == NULL)
        return NULL;

    if (handoff->has_cached_property_slot)
    {
        slot_expr = cypher_make_cached_property_slot_expr(
            &handoff->cached_property_slot);
        if (slot_expr != NULL)
            return slot_expr;
    }

    return copyObject(handoff->index_expr);
}

Node *cypher_replace_property_index_side(OpExpr *op, bool replace_left,
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

bool cypher_rewrite_property_equals_restrictions(
    PlannerInfo *root, RelOptInfo *rel, Index rti)
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

        rewritten = try_rewrite_property_equals_clause((Node *)rinfo->clause,
                                                       rti, rel);
        if (rewritten != NULL)
        {
            rinfo->clause = (Expr *)rewritten;
            rewritten_any = true;
        }
    }

    return rewritten_any;
}

void cypher_canonicalize_property_index_predicates(RelOptInfo *rel)
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

void cypher_canonicalize_property_index_restrictions(RelOptInfo *rel)
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

bool cypher_detect_simple_property_projection(
    PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel,
    List **slots, const char **target_source)
{
    (void)root;

    if (slots == NULL || target_source == NULL)
        return false;

    *slots = NIL;
    *target_source = NULL;

    if (input_rel != NULL &&
        detect_simple_property_projection_target(input_rel->reltarget, slots))
    {
        *target_source = "input_rel";
        return true;
    }

    if (output_rel != NULL &&
        detect_simple_property_projection_target(output_rel->reltarget, slots))
    {
        *target_source = "output_rel";
        return true;
    }

    return false;
}

bool cypher_build_ordered_property_projection_targets(
    PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel,
    FinalPathExtraData *extra, PathTarget **lower_target,
    PathTarget **final_target)
{
    CypherCachedPropertySlotDescriptor output_slot;
    CypherCachedPropertySlotDescriptor sort_slot;
    TargetEntry *sort_tle = NULL;
    PathTarget *lower;
    PathTarget *final;
    Var *properties_var;
    Var *lookup_var;
    Node *final_output_expr;
    Oid relid;
    AttrNumber properties_attno;
    bool reuse_sort_output = false;

    if (lower_target == NULL || final_target == NULL)
        return false;

    *lower_target = NULL;
    *final_target = NULL;

    if (input_rel == NULL || output_rel == NULL || extra == NULL ||
        !extra->limit_needed ||
        !detect_ordered_property_projection_delay(root, &output_slot,
                                                  &sort_slot,
                                                  &reuse_sort_output,
                                                  &sort_tle) ||
        !IsA(output_slot.container, Var))
    {
        return false;
    }

    properties_var = castNode(Var, output_slot.container);
    if (!reuse_sort_output &&
        (properties_var->varno <= 0 ||
         properties_var->varno >= root->simple_rel_array_size ||
         root->simple_rte_array[properties_var->varno] == NULL ||
         root->simple_rte_array[properties_var->varno]->rtekind != RTE_RELATION))
    {
        return false;
    }

    if (!reuse_sort_output &&
        properties_var->varattno != Anum_ag_label_vertex_table_properties)
    {
        if (properties_var->varattno != Anum_ag_label_edge_table_properties)
            return false;
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
                relid, (Node *)lookup_var, properties_attno,
                output_slot.keys);
        }
        else
        {
            lookup_var = makeVar(properties_var->varno,
                                 SelfItemPointerAttributeNumber,
                                 TIDOID, -1, InvalidOid,
                                 properties_var->varlevelsup);
            final_output_expr = cypher_make_ctid_property_path_field_agtype_expr(
                relid, (Node *)lookup_var, properties_attno,
                output_slot.keys);
        }
    }

    if (final_output_expr == NULL)
        return false;

    lower = create_empty_pathtarget();
    if (lookup_var != NULL)
        add_column_to_pathtarget(lower, (Expr *)copyObject(lookup_var), 0);
    add_column_to_pathtarget(lower, (Expr *)copyObject(sort_tle->expr),
                             sort_tle->ressortgroupref);
    lower = set_pathtarget_cost_width(root, lower);

    final = create_empty_pathtarget();
    if (reuse_sort_output)
    {
        add_column_to_pathtarget(final, (Expr *)final_output_expr,
                                 sort_tle->ressortgroupref);
    }
    else
    {
        add_column_to_pathtarget(final, (Expr *)final_output_expr, 0);
        add_column_to_pathtarget(final, (Expr *)copyObject(sort_tle->expr),
                                 sort_tle->ressortgroupref);
    }
    final = set_pathtarget_cost_width(root, final);

    *lower_target = lower;
    *final_target = final;
    return true;
}

static bool make_cached_property_slot_descriptor(
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

    slot->has_property_descriptor = true;
    slot->property_descriptor = *handoff;
    slot->container = handoff->property_signature.container;
    slot->keys = handoff->property_signature.keys;
    slot->value_type = handoff->property_signature.value_type;
    slot->field_result_type = handoff->property_signature.field_result_type;
    slot->final_materialization_weight =
        cached_property_slot_final_materialization_weight(
            slot->field_result_type);
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

    if (slot->has_property_descriptor &&
        slot->property_descriptor.index_expr != NULL)
    {
        return copyObject(slot->property_descriptor.index_expr);
    }

    if (slot->index_expr != NULL)
        return copyObject(slot->index_expr);

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

bool cypher_make_property_access_slot_descriptor(
    Node *expr, CypherCachedPropertySlotDescriptor *slot)
{
    CypherPropertyHandoffDescriptor handoff;

    if (slot == NULL)
        return false;

    memset(&handoff, 0, sizeof(handoff));
    if (!cypher_extract_property_access_signature(
            expr, &handoff.property_signature))
    {
        return false;
    }

    return make_cached_property_slot_descriptor(&handoff, slot);
}

Node *cypher_make_property_path_slot_expr(Node *container, Node *path,
                                          Oid value_type,
                                          Oid field_result_type)
{
    CypherCachedPropertySlotDescriptor slot;
    List *keys = NIL;

    if (container == NULL || path == NULL ||
        !extract_property_path_const_keys(path, &keys))
    {
        return NULL;
    }

    memset(&slot, 0, sizeof(slot));
    slot.container = container;
    slot.keys = keys;
    slot.value_type = value_type;
    slot.field_result_type = field_result_type;
    slot.final_materialization_weight =
        cached_property_slot_final_materialization_weight(field_result_type);

    return cypher_make_cached_property_slot_expr(&slot);
}

bool cypher_find_typed_collect_handoffs(
    PathTarget *target, bool require_distinct, List **handoffs)
{
    List *found = NIL;
    ListCell *lc;

    if (handoffs == NULL)
        return false;

    *handoffs = NIL;

    if (target == NULL)
        return false;

    foreach(lc, target->exprs)
    {
        Node *expr = lfirst(lc);
        Aggref *aggref;
        CypherTypedCollectHandoff current_handoff;
        CypherTypedCollectHandoff *handoff_copy;

        if (expr == NULL || !IsA(expr, Aggref))
            continue;

        aggref = castNode(Aggref, expr);
        if (!is_typed_collect_agg_oid(aggref->aggfnoid, NULL))
            continue;

        if (!extract_typed_collect_handoff(aggref, require_distinct,
                                           &current_handoff))
            return false;

        handoff_copy = palloc(sizeof(*handoff_copy));
        *handoff_copy = current_handoff;
        found = lappend(found, handoff_copy);
    }

    if (found == NIL)
        return false;

    *handoffs = found;
    return true;
}

bool cypher_find_multi_typed_collect_handoffs(PathTarget *target,
                                              List **handoffs)
{
    ListCell *lc;

    if (!cypher_find_typed_collect_handoffs(target, false, handoffs))
        return false;

    if (list_length(*handoffs) == 1)
        return true;

    foreach(lc, *handoffs)
    {
        CypherTypedCollectHandoff *handoff = lfirst(lc);

        if (handoff == NULL || handoff->aggref == NULL ||
            handoff->aggref->aggdistinct != NIL)
        {
            *handoffs = NIL;
            return false;
        }
    }

    return true;
}

bool cypher_find_typed_collect_handoff(
    PathTarget *target, bool require_distinct,
    CypherTypedCollectHandoff *handoff)
{
    List *handoffs = NIL;

    if (handoff == NULL)
        return false;

    memset(handoff, 0, sizeof(CypherTypedCollectHandoff));

    if (!cypher_find_typed_collect_handoffs(target, require_distinct,
                                           &handoffs) ||
        list_length(handoffs) != 1)
    {
        return false;
    }

    *handoff = *(CypherTypedCollectHandoff *)linitial(handoffs);
    return true;
}

bool cypher_find_typed_distinct_collect_handoff(
    PathTarget *target, CypherTypedCollectHandoff *handoff)
{
    return cypher_find_typed_collect_handoff(target, true, handoff);
}

List *cypher_build_typed_collect_arg_plans(PathTarget *target, List *handoffs)
{
    List *arg_plans = NIL;
    ListCell *lc;
    Index sortgroupref = 1;
    int expr_index = 0;

    if (target == NULL || handoffs == NIL)
        return NIL;

    foreach(lc, target->exprs)
    {
        sortgroupref = Max(sortgroupref,
                           get_pathtarget_sortgroupref(target,
                                                       expr_index) + 1);
        expr_index++;
    }

    foreach(lc, handoffs)
    {
        CypherTypedCollectHandoff *handoff = lfirst(lc);
        CypherTypedCollectArgPlan *arg_plan;
        Node *arg;
        Index arg_sortgroupref;

        arg = make_typed_collect_handoff_arg(handoff);
        if (arg == NULL)
            return NIL;

        arg_sortgroupref = sortgroupref++;
        if (list_length(handoffs) == 1 && handoff->aggref->aggdistinct != NIL)
            arg_sortgroupref = 0;

        arg_plan = palloc(sizeof(*arg_plan));
        arg_plan->handoff = handoff;
        arg_plan->arg = arg;
        arg_plan->sortgroupref = arg_sortgroupref;
        arg_plans = lappend(arg_plans, arg_plan);
    }

    return arg_plans;
}

bool cypher_add_typed_collect_arg_plans_to_target(PathTarget *target,
                                                  List *arg_plans)
{
    ListCell *lc;

    if (target == NULL || arg_plans == NIL)
        return false;

    foreach(lc, arg_plans)
    {
        CypherTypedCollectArgPlan *arg_plan = lfirst(lc);

        add_column_to_pathtarget(target, (Expr *)copyObject(arg_plan->arg),
                                 arg_plan->sortgroupref);
    }

    return true;
}

bool cypher_add_aggregate_group_exprs_to_target(PathTarget *target,
                                                AggPath *agg_path)
{
    ListCell *group_lc;

    if (target == NULL || agg_path == NULL)
        return false;

    if (agg_path->groupClause == NIL)
        return true;

    if (agg_path->subpath == NULL ||
        agg_path->subpath->pathtarget == NULL)
    {
        return false;
    }

    foreach(group_lc, agg_path->groupClause)
    {
        SortGroupClause *group_clause;
        ListCell *expr_lc;
        int expr_index = 0;
        bool found = false;

        group_clause = lfirst_node(SortGroupClause, group_lc);
        foreach(expr_lc, agg_path->subpath->pathtarget->exprs)
        {
            Index sortgroupref;

            sortgroupref = get_pathtarget_sortgroupref(
                agg_path->subpath->pathtarget, expr_index);
            if (sortgroupref == group_clause->tleSortGroupRef)
            {
                add_column_to_pathtarget(target,
                                         (Expr *)copyObject(lfirst(expr_lc)),
                                         sortgroupref);
                found = true;
                break;
            }
            expr_index++;
        }

        if (!found)
            return false;
    }

    return true;
}

PathTarget *cypher_build_typed_collect_agg_target(
    PlannerInfo *root, PathTarget *target, List *arg_plans)
{
    PathTarget *new_target;
    ListCell *lc;
    int rewritten = 0;

    if (target == NULL || arg_plans == NIL)
    {
        return NULL;
    }

    new_target = copy_pathtarget(target);
    foreach(lc, new_target->exprs)
    {
        Node *expr = lfirst(lc);
        Aggref *aggref;
        TargetEntry *arg_tle;
        Node *arg;

        if (expr == NULL || !IsA(expr, Aggref))
            continue;

        aggref = castNode(Aggref, expr);
        arg = find_typed_collect_rewrite_arg(aggref, arg_plans);
        if (arg == NULL)
            continue;

        arg_tle = linitial_node(TargetEntry, aggref->args);
        arg_tle = copyObject(arg_tle);
        arg_tle->expr = (Expr *)copyObject(arg);
        aggref->args = list_make1(arg_tle);
        rewritten++;
    }

    if (rewritten != list_length(arg_plans))
        return NULL;

    return set_pathtarget_cost_width(root, new_target);
}

static Node *find_typed_collect_rewrite_arg(Aggref *aggref, List *arg_plans)
{
    ListCell *lc;

    if (aggref == NULL || list_length(aggref->args) != 1)
        return NULL;

    foreach(lc, arg_plans)
    {
        CypherTypedCollectArgPlan *arg_plan = lfirst(lc);
        CypherTypedCollectHandoff *handoff = arg_plan->handoff;
        TargetEntry *arg_tle;

        if (handoff == NULL ||
            aggref->aggfnoid != handoff->agg_func_oid ||
            aggref->aggdistinct != handoff->aggref->aggdistinct)
        {
            continue;
        }

        arg_tle = linitial_node(TargetEntry, aggref->args);
        if (arg_tle != NULL &&
            equal((Node *)arg_tle->expr, handoff->arg_expr))
        {
            return arg_plan->arg;
        }
    }

    return NULL;
}

static Node *make_typed_collect_handoff_arg(
    CypherTypedCollectHandoff *handoff)
{
    Node *slot_expr = NULL;

    if (handoff == NULL || handoff->arg_expr == NULL)
        return NULL;

    if (handoff->has_cached_property_slot)
        slot_expr = cypher_make_cached_property_slot_expr(
            &handoff->cached_property_slot);

    return slot_expr != NULL ? slot_expr : handoff->arg_expr;
}

bool cypher_find_array_agg_property_handoff(
    PathTarget *target, CypherArrayAggPropertyHandoff *handoff)
{
    bool duplicate;

    if (handoff == NULL)
        return false;

    memset(handoff, 0, sizeof(*handoff));
    if (target == NULL)
        return false;

    duplicate = expression_tree_walker((Node *)target->exprs,
                                       find_array_agg_property_handoff_walker,
                                       handoff);
    return !duplicate && handoff->aggref != NULL &&
        handoff->arg_exprs != NIL && handoff->arg_types != NIL &&
        OidIsValid(handoff->agg_func_oid);
}

static bool find_array_agg_property_handoff_walker(Node *node,
                                                   void *context)
{
    CypherArrayAggPropertyHandoff *handoff = context;
    Aggref *aggref;

    if (node == NULL)
        return false;

    if (!IsA(node, Aggref))
        return expression_tree_walker(node,
                                      find_array_agg_property_handoff_walker,
                                      context);

    aggref = castNode(Aggref, node);
    if (aggref->aggdistinct != NIL ||
        aggref->aggorder != NIL ||
        aggref->aggfilter != NULL)
    {
        return false;
    }

    if (handoff->aggref != NULL)
        return true;

    if (!make_array_agg_property_handoff_args(aggref, handoff))
        return false;

    handoff->aggref = aggref;

    return false;
}

List *cypher_build_array_agg_property_arg_plans(
    PathTarget *target, CypherArrayAggPropertyHandoff *handoff)
{
    List *arg_plans = NIL;
    ListCell *lc;
    Index sortgroupref = 1;
    int expr_index = 0;

    if (target == NULL || handoff == NULL || handoff->arg_exprs == NIL)
        return NIL;

    foreach(lc, target->exprs)
    {
        sortgroupref = Max(sortgroupref,
                           get_pathtarget_sortgroupref(target,
                                                       expr_index) + 1);
        expr_index++;
    }

    foreach(lc, handoff->arg_exprs)
    {
        CypherArrayAggPropertyArgPlan *arg_plan;

        arg_plan = palloc(sizeof(*arg_plan));
        arg_plan->arg = lfirst(lc);
        arg_plan->sortgroupref = sortgroupref++;
        arg_plans = lappend(arg_plans, arg_plan);
    }

    return arg_plans;
}

bool cypher_add_array_agg_property_arg_plans_to_target(PathTarget *target,
                                                       List *arg_plans)
{
    ListCell *lc;

    if (target == NULL || arg_plans == NIL)
        return false;

    foreach(lc, arg_plans)
    {
        CypherArrayAggPropertyArgPlan *arg_plan = lfirst(lc);

        add_column_to_pathtarget(target, (Expr *)copyObject(arg_plan->arg),
                                 arg_plan->sortgroupref);
    }

    return true;
}

PathTarget *cypher_build_array_agg_property_target(
    PlannerInfo *root, PathTarget *target,
    CypherArrayAggPropertyHandoff *handoff, List *arg_plans)
{
    CypherArrayAggPropertyRewriteContext context;
    PathTarget *new_target;

    if (target == NULL || handoff == NULL || arg_plans == NIL)
        return NULL;

    memset(&context, 0, sizeof(context));
    context.handoff = handoff;
    context.arg_plans = arg_plans;

    new_target = copy_pathtarget(target);
    new_target->exprs = (List *)expression_tree_mutator(
        (Node *)new_target->exprs,
        rewrite_array_agg_property_target_mutator,
        &context);

    if (!context.rewritten)
        return NULL;

    return set_pathtarget_cost_width(root, new_target);
}

static Node *rewrite_array_agg_property_target_mutator(Node *node,
                                                       void *context)
{
    CypherArrayAggPropertyRewriteContext *rewrite_context = context;
    CypherArrayAggPropertyHandoff *handoff = rewrite_context->handoff;
    Aggref *aggref;
    Aggref *new_aggref;
    List *new_args = NIL;
    ListCell *lc;
    int resno = 1;

    if (node == NULL)
        return NULL;

    if (!IsA(node, Aggref))
        return expression_tree_mutator(node,
                                       rewrite_array_agg_property_target_mutator,
                                       context);

    aggref = castNode(Aggref, node);
    if (!equal(aggref, handoff->aggref))
    {
        return expression_tree_mutator(node,
                                       rewrite_array_agg_property_target_mutator,
                                       context);
    }

    new_aggref = copyObject(aggref);
    foreach(lc, rewrite_context->arg_plans)
    {
        CypherArrayAggPropertyArgPlan *arg_plan = lfirst(lc);

        new_args = lappend(new_args,
                           makeTargetEntry((Expr *)copyObject(arg_plan->arg),
                                           resno++, NULL, false));
    }

    new_aggref->aggfnoid = handoff->agg_func_oid;
    new_aggref->aggargtypes = copyObject(handoff->arg_types);
    new_aggref->args = new_args;
    new_aggref->aggvariadic = false;
    rewrite_context->rewritten = true;

    return (Node *)new_aggref;
}

static bool make_array_agg_property_handoff_args(
    Aggref *aggref, CypherArrayAggPropertyHandoff *handoff)
{
    if (aggref == NULL || handoff == NULL)
        return false;

    if (aggref->aggfnoid == get_cached_age_array_agg_property_agg_oid())
        return make_array_agg_single_property_args(aggref, handoff);

    if (aggref->aggfnoid == get_cached_age_array_agg_map2_property_agg_oid())
        return make_array_agg_map2_property_args(aggref, handoff);

    if (aggref->aggfnoid == get_cached_age_array_agg_map_property_agg_oid())
        return make_array_agg_map_property_args(aggref, handoff);

    if (aggref->aggfnoid == get_cached_age_array_agg_list_property_agg_oid())
        return make_array_agg_list_property_args(aggref, handoff);

    return false;
}

static bool make_array_agg_single_property_args(
    Aggref *aggref, CypherArrayAggPropertyHandoff *handoff)
{
    TargetEntry *properties_tle;
    TargetEntry *key_tle;
    Node *value;

    if (aggref == NULL || list_length(aggref->args) != 2)
        return false;

    properties_tle = linitial_node(TargetEntry, aggref->args);
    key_tle = lsecond_node(TargetEntry, aggref->args);
    if (properties_tle == NULL || key_tle == NULL)
        return false;

    value = cypher_make_property_path_slot_expr(
        (Node *)properties_tle->expr, (Node *)key_tle->expr,
        AGTYPEOID, AGTYPEOID);
    if (value == NULL)
        return false;

    handoff->agg_func_oid = get_cached_array_agg_anynonarray_agg_oid();
    handoff->arg_exprs = list_make1(value);
    handoff->arg_types = list_make1_oid(AGTYPEOID);

    return true;
}

static bool make_array_agg_map2_property_args(
    Aggref *aggref, CypherArrayAggPropertyHandoff *handoff)
{
    TargetEntry *properties_tle;
    TargetEntry *out_key1_tle;
    TargetEntry *prop_key1_tle;
    TargetEntry *out_key2_tle;
    TargetEntry *prop_key2_tle;
    Node *value1;
    Node *value2;

    if (aggref == NULL || list_length(aggref->args) != 5)
        return false;

    properties_tle = linitial_node(TargetEntry, aggref->args);
    out_key1_tle = lsecond_node(TargetEntry, aggref->args);
    prop_key1_tle = lthird_node(TargetEntry, aggref->args);
    out_key2_tle = lfourth_node(TargetEntry, aggref->args);
    prop_key2_tle = lfirst_node(TargetEntry, list_nth_cell(aggref->args, 4));
    if (properties_tle == NULL || out_key1_tle == NULL ||
        prop_key1_tle == NULL || out_key2_tle == NULL ||
        prop_key2_tle == NULL)
    {
        return false;
    }

    value1 = cypher_make_property_path_slot_expr(
        (Node *)properties_tle->expr, (Node *)prop_key1_tle->expr,
        AGTYPEOID, AGTYPEOID);
    value2 = cypher_make_property_path_slot_expr(
        (Node *)properties_tle->expr, (Node *)prop_key2_tle->expr,
        AGTYPEOID, AGTYPEOID);
    if (value1 == NULL || value2 == NULL)
        return false;

    handoff->agg_func_oid = get_cached_age_array_agg_map_slots_agg_oid();
    handoff->arg_exprs = list_make4(copyObject(out_key1_tle->expr),
                                    value1,
                                    copyObject(out_key2_tle->expr),
                                    value2);
    handoff->arg_types = list_make4_oid(TEXTOID, AGTYPEOID,
                                        TEXTOID, AGTYPEOID);

    return true;
}

static bool make_array_agg_map_property_args(
    Aggref *aggref, CypherArrayAggPropertyHandoff *handoff)
{
    TargetEntry *properties_tle;
    TargetEntry *out_keys_tle;
    TargetEntry *prop_keys_tle;
    List *out_keys = NIL;
    List *prop_keys = NIL;
    List *args = NIL;
    ListCell *out_key_lc;
    ListCell *prop_key_lc;

    if (aggref == NULL || list_length(aggref->args) != 3)
        return false;

    properties_tle = linitial_node(TargetEntry, aggref->args);
    out_keys_tle = lsecond_node(TargetEntry, aggref->args);
    prop_keys_tle = lthird_node(TargetEntry, aggref->args);
    if (properties_tle == NULL || out_keys_tle == NULL ||
        prop_keys_tle == NULL ||
        !extract_text_array_const_elements((Node *)out_keys_tle->expr,
                                           &out_keys) ||
        !extract_agtype_array_const_elements((Node *)prop_keys_tle->expr,
                                             &prop_keys) ||
        list_length(out_keys) != list_length(prop_keys))
    {
        return false;
    }

    forboth(out_key_lc, out_keys, prop_key_lc, prop_keys)
    {
        Node *value;

        value = cypher_make_property_path_slot_expr(
            (Node *)properties_tle->expr, lfirst(prop_key_lc),
            AGTYPEOID, AGTYPEOID);
        if (value == NULL)
            return false;

        args = lappend(args, copyObject(lfirst(out_key_lc)));
        args = lappend(args, value);
    }

    if (args == NIL)
        return false;

    handoff->agg_func_oid = get_cached_age_array_agg_map_slots_agg_oid();
    handoff->arg_exprs = args;
    handoff->arg_types = NIL;
    forboth(out_key_lc, out_keys, prop_key_lc, prop_keys)
        handoff->arg_types = lappend_oid(lappend_oid(handoff->arg_types,
                                                     TEXTOID),
                                         AGTYPEOID);

    return true;
}

static bool make_array_agg_list_property_args(
    Aggref *aggref, CypherArrayAggPropertyHandoff *handoff)
{
    TargetEntry *properties_tle;
    TargetEntry *prop_keys_tle;
    List *paths = NIL;
    List *values = NIL;
    ListCell *lc;

    if (aggref == NULL || list_length(aggref->args) != 2)
        return false;

    properties_tle = linitial_node(TargetEntry, aggref->args);
    prop_keys_tle = lsecond_node(TargetEntry, aggref->args);
    if (properties_tle == NULL || prop_keys_tle == NULL ||
        !extract_agtype_array_const_elements((Node *)prop_keys_tle->expr,
                                             &paths))
    {
        return false;
    }

    foreach(lc, paths)
    {
        Node *value;

        value = cypher_make_property_path_slot_expr(
            (Node *)properties_tle->expr, lfirst(lc),
            AGTYPEOID, AGTYPEOID);
        if (value == NULL)
            return false;
        values = lappend(values, value);
    }

    if (values == NIL)
        return false;

    handoff->agg_func_oid = get_cached_age_array_agg_list_slots_agg_oid();
    handoff->arg_exprs = values;
    handoff->arg_types = NIL;
    foreach(lc, values)
        handoff->arg_types = lappend_oid(handoff->arg_types, AGTYPEOID);

    return true;
}

static bool extract_text_array_const_elements(Node *node, List **elements)
{
    Const *array_const;
    Datum *values;
    bool *nulls;
    int nelems;
    int i;

    if (elements == NULL)
        return false;

    *elements = NIL;
    if (node == NULL || !IsA(node, Const))
        return false;

    array_const = castNode(Const, node);
    if (array_const->constisnull ||
        array_const->consttype != TEXTARRAYOID)
    {
        return false;
    }

    deconstruct_array_builtin(DatumGetArrayTypeP(array_const->constvalue),
                              TEXTOID, &values, &nulls, &nelems);
    for (i = 0; i < nelems; i++)
    {
        Const *text_const;
        text *key_text;

        if (nulls[i])
            return false;

        key_text = DatumGetTextPP(values[i]);
        text_const = makeConst(TEXTOID, -1, DEFAULT_COLLATION_OID, -1,
                               PointerGetDatum(cstring_to_text_with_len(
                                   VARDATA_ANY(key_text),
                                   VARSIZE_ANY_EXHDR(key_text))),
                               false, false);
        *elements = lappend(*elements, text_const);
    }

    return *elements != NIL;
}

static bool extract_agtype_array_const_elements(Node *node, List **elements)
{
    Const *array_const;
    ArrayType *array;
    Datum *values;
    bool *nulls;
    int nelems;
    int16 typlen;
    bool typbyval;
    char typalign;
    int i;

    if (elements == NULL)
        return false;

    *elements = NIL;
    if (node == NULL || !IsA(node, Const))
        return false;

    array_const = castNode(Const, node);
    if (array_const->constisnull ||
        array_const->consttype != AGTYPEARRAYOID)
    {
        return false;
    }

    array = DatumGetArrayTypeP(array_const->constvalue);
    get_typlenbyvalalign(AGTYPEOID, &typlen, &typbyval, &typalign);
    deconstruct_array(array, AGTYPEOID, typlen, typbyval, typalign,
                      &values, &nulls, &nelems);

    for (i = 0; i < nelems; i++)
    {
        Const *path_const;

        if (nulls[i])
            return false;

        path_const = makeConst(AGTYPEOID, -1, InvalidOid, -1,
                               datumCopy(values[i], typbyval, typlen),
                               false, false);
        *elements = lappend(*elements, path_const);
    }

    return *elements != NIL;
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

static bool cached_property_slots_share_key_source(
    const CypherCachedPropertySlotDescriptor *left,
    const CypherCachedPropertySlotDescriptor *right)
{
    if (left == NULL || right == NULL)
        return false;

    return same_property_source(left->container, right->container) &&
        equal(left->keys, right->keys);
}

static bool cached_property_slots_same_physical_signature(
    const CypherCachedPropertySlotDescriptor *left,
    const CypherCachedPropertySlotDescriptor *right)
{
    if (!cached_property_slots_share_key_source(left, right))
        return false;

    return left->value_type == right->value_type &&
        left->field_result_type == right->field_result_type;
}

static bool cached_property_slot_uses_typed_physical_result(
    const CypherCachedPropertySlotDescriptor *slot)
{
    if (slot == NULL)
        return false;

    return slot->value_type != AGTYPEOID ||
        slot->field_result_type != AGTYPEOID;
}

static int cached_property_slot_final_materialization_weight(
    Oid field_result_type)
{
    if (field_result_type == AGTYPEOID)
        return 2;

    if (OidIsValid(field_result_type))
        return 1;

    return 0;
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

static bool extract_property_path_const_keys(Node *node, List **keys)
{
    Const *path_const;
    agtype *path_agtype;
    agtype_iterator *it;
    agtype_value value;
    agtype_iterator_token token;

    if (keys == NULL)
        return false;

    *keys = NIL;
    if (node == NULL || !IsA(node, Const))
        return false;

    path_const = castNode(Const, node);
    if (path_const->constisnull || path_const->consttype != AGTYPEOID)
        return false;

    if (extract_agtype_const_string(path_const, &value))
    {
        *keys = list_make1(copyObject(path_const));
        return true;
    }

    path_agtype = DATUM_GET_AGTYPE_P(path_const->constvalue);
    if (AGT_ROOT_IS_SCALAR(path_agtype) || !AGT_ROOT_IS_ARRAY(path_agtype))
        return false;

    it = agtype_iterator_init(&path_agtype->root);
    token = agtype_iterator_next(&it, &value, false);
    if (token != WAGT_BEGIN_ARRAY)
        return false;

    while ((token = agtype_iterator_next(&it, &value, true)) != WAGT_END_ARRAY)
    {
        Const *key_const;

        if (token != WAGT_ELEM || value.type != AGTV_STRING)
            return false;

        key_const = makeConst(AGTYPEOID, -1, InvalidOid, -1,
                              PointerGetDatum(agtype_value_to_agtype(&value)),
                              false, false);
        *keys = lappend(*keys, key_const);
    }

    return *keys != NIL;
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

    handoff->aggref = aggref;
    handoff->arg_expr = (Node *)arg_tle->expr;
    handoff->value_type = value_type;
    handoff->agg_func_oid = typed_agg_oid;
    handoff->has_property_descriptor =
        init_property_handoff_descriptor(
            handoff->arg_expr, InvalidOid, handoff->agg_func_oid, NULL,
            &handoff->property_descriptor);
    refresh_typed_collect_cached_slot(handoff);

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

static void refresh_typed_collect_cached_slot(
    CypherTypedCollectHandoff *handoff)
{
    handoff->has_cached_property_slot =
        handoff->has_property_descriptor &&
        make_cached_property_slot_descriptor(
            &handoff->property_descriptor, &handoff->cached_property_slot);
}

static void refresh_property_index_cached_slot(
    CypherPropertyIndexHandoff *handoff)
{
    handoff->has_cached_property_slot =
        handoff->has_property_descriptor &&
        make_cached_property_slot_descriptor(
            &handoff->property_descriptor, &handoff->cached_property_slot);
}

static void refresh_scalar_final_cached_slot(
    CypherScalarFinalHandoff *handoff)
{
    handoff->has_cached_property_slot =
        handoff->has_property_descriptor &&
        make_cached_property_slot_descriptor(
            &handoff->property_descriptor, &handoff->cached_property_slot);
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
    refresh_property_index_cached_slot(handoff);
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
    Node *slot_expr = NULL;

    if (handoff == NULL || index_expr == NULL)
        return;

    if (handoff->has_cached_property_slot)
        slot_expr = cypher_make_cached_property_slot_expr(
            &handoff->cached_property_slot);

    if (slot_expr != NULL && equal(slot_expr, index_expr))
        handoff->index_expr = slot_expr;
    else
        handoff->index_expr = copy_expr ? copyObject(index_expr) : index_expr;

    if (handoff->has_property_descriptor)
    {
        handoff->property_descriptor.index_expr = handoff->index_expr;
        refresh_property_index_cached_slot(handoff);
    }
}

static bool detect_simple_property_projection_target(PathTarget *target,
                                                     List **slots)
{
    List *found = NIL;
    ListCell *lc;
    bool multi_slot;

    if (target == NULL || target->exprs == NIL || slots == NULL)
        return false;

    multi_slot = list_length(target->exprs) > 1;
    foreach(lc, target->exprs)
    {
        Node *expr = lfirst(lc);
        CypherCachedPropertySlotDescriptor slot;
        CypherCachedPropertySlotDescriptor *slot_copy;

        if (!is_simple_property_access_target(expr, &slot))
            return false;
        if (multi_slot && slot.field_result_type == AGTYPEOID)
            return false;

        slot_copy = palloc(sizeof(*slot_copy));
        *slot_copy = slot;
        found = lappend(found, slot_copy);
    }

    *slots = found;
    return found != NIL;
}

static bool is_simple_property_access_target(
    Node *node, CypherCachedPropertySlotDescriptor *slot)
{
    CypherCachedPropertySlotDescriptor candidate;
    Var *properties_var;
    ListCell *lc;

    if (node == NULL || slot == NULL)
        return false;

    if (!cypher_make_property_access_slot_descriptor(node, &candidate))
        return false;

    if (!IsA(candidate.container, Var))
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
    if (properties_var->varattno != Anum_ag_label_vertex_table_properties)
        return false;

    foreach(lc, candidate.keys)
    {
        Const *key_const;

        if (!IsA(lfirst(lc), Const))
            return false;

        key_const = lfirst_node(Const, lc);
        if (key_const->constisnull || key_const->consttype != AGTYPEOID)
            return false;
    }

    *slot = candidate;
    return true;
}

static bool detect_ordered_property_projection_delay(
    PlannerInfo *root, CypherCachedPropertySlotDescriptor *output_slot,
    CypherCachedPropertySlotDescriptor *sort_slot, bool *reuse_sort_output,
    TargetEntry **sort_tle)
{
    TargetEntry *output_tle = NULL;
    SortGroupClause *sort_clause;
    ListCell *lc;

    if (root == NULL || root->parse == NULL ||
        output_slot == NULL || sort_slot == NULL ||
        reuse_sort_output == NULL || sort_tle == NULL ||
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
        !cypher_make_property_access_slot_descriptor((Node *)output_tle->expr,
                                                     output_slot))
    {
        return false;
    }

    sort_clause = linitial_node(SortGroupClause, root->parse->sortClause);
    *sort_tle = get_sortgroupref_tle(sort_clause->tleSortGroupRef,
                                     root->parse->targetList);

    if (*sort_tle == NULL ||
        (!(*sort_tle)->resjunk && *sort_tle != output_tle) ||
        !cypher_make_property_access_slot_descriptor(
            (Node *)(*sort_tle)->expr, sort_slot) ||
        sort_slot->value_type == AGTYPEOID)
    {
        return false;
    }

    if (!cached_property_slots_share_key_source(output_slot, sort_slot))
    {
        return false;
    }

    *reuse_sort_output = cached_property_slots_same_physical_signature(
        output_slot, sort_slot);
    if (output_slot->value_type != AGTYPEOID && !*reuse_sort_output)
        return false;

    return true;
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
    CypherCachedPropertySlotDescriptor slot;
    Node *slot_expr;

    if (node == NULL)
        return false;

    if (cypher_make_property_access_slot_descriptor(node, &slot) &&
        cached_property_slot_uses_typed_physical_result(&slot))
    {
        slot_expr = cypher_make_cached_property_slot_expr(&slot);
        if (slot_expr != NULL)
            add_canonical_property_index_expr(exprs, &slot, node);

        return false;
    }

    return expression_tree_walker(
        node, collect_canonical_property_index_exprs_walker, context);
}

static Node *canonicalize_property_index_exprs_mutator(Node *node,
                                                       void *context)
{
    List *exprs = (List *)context;
    CypherCachedPropertySlotDescriptor slot;
    Node *slot_expr;

    if (node == NULL)
        return NULL;

    if (cypher_make_property_access_slot_descriptor(node, &slot) &&
        cached_property_slot_uses_typed_physical_result(&slot))
    {
        CypherCanonicalPropertyIndexExpr *entry;

        slot_expr = cypher_make_cached_property_slot_expr(&slot);
        entry = find_canonical_property_index_expr(exprs, &slot, slot_expr);
        if (entry != NULL)
            return copyObject(entry->expr);
    }

    return expression_tree_mutator(
        node, canonicalize_property_index_exprs_mutator, context);
}

static CypherCanonicalPropertyIndexExpr *find_canonical_property_index_expr(
    List *exprs, const CypherCachedPropertySlotDescriptor *slot, Node *expr)
{
    ListCell *lc;

    foreach(lc, exprs)
    {
        CypherCanonicalPropertyIndexExpr *entry = lfirst(lc);

        if ((expr != NULL && equal(entry->expr, expr)) ||
            cached_property_slots_same_physical_signature(&entry->slot, slot))
        {
            return entry;
        }
    }

    return NULL;
}

static void add_canonical_property_index_expr(
    List **exprs, const CypherCachedPropertySlotDescriptor *slot, Node *expr)
{
    CypherCanonicalPropertyIndexExpr *entry;

    if (find_canonical_property_index_expr(*exprs, slot, expr) != NULL)
        return;

    entry = palloc0(sizeof(CypherCanonicalPropertyIndexExpr));
    entry->slot = *slot;
    entry->slot.container = copyObject(slot->container);
    entry->slot.keys = copyObject(slot->keys);
    entry->slot.index_expr = copyObject(slot->index_expr);
    entry->slot.property_descriptor.index_expr =
        copyObject(slot->property_descriptor.index_expr);
    entry->expr = copyObject(expr);
    *exprs = lappend(*exprs, entry);
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
            Node *index_expr;

            index_expr = cypher_make_property_index_handoff_expr(
                &index_handoff);
            if (index_expr == NULL)
                return NULL;

            return cypher_replace_property_index_side(op, true,
                                                      index_expr);
        }

        if (cypher_find_matching_property_index_handoff(rel, right,
                                                        &index_handoff))
        {
            Node *index_expr;

            index_expr = cypher_make_property_index_handoff_expr(
                &index_handoff);
            if (index_expr == NULL)
                return NULL;

            return cypher_replace_property_index_side(op, false,
                                                      index_expr);
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
    refresh_scalar_final_cached_slot(handoff);
    return true;
}

bool cypher_build_scalar_final_deferred_targets(
    PlannerInfo *root, List *processed_tlist,
    PathTarget **lower_target, PathTarget **final_target)
{
    ListCell *lc;
    bool found = false;

    if (root == NULL || processed_tlist == NIL ||
        lower_target == NULL || final_target == NULL)
    {
        return false;
    }

    *lower_target = create_empty_pathtarget();
    *final_target = create_empty_pathtarget();

    foreach(lc, processed_tlist)
    {
        TargetEntry *tle = lfirst_node(TargetEntry, lc);
        CypherScalarFinalHandoff handoff;

        if (!tle->resjunk &&
            cypher_extract_scalar_final_handoff((Node *)tle->expr, &handoff))
        {
            FuncExpr *final_expr;
            Expr *canonical_arg;
            Node *slot_expr = NULL;

            if (handoff.has_cached_property_slot)
                slot_expr = cypher_make_cached_property_slot_expr(
                    &handoff.cached_property_slot);

            handoff.scalar_expr = slot_expr != NULL ?
                slot_expr : copyObject(handoff.scalar_expr);
            canonical_arg = add_or_get_lower_scalar_handoff(
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

static Expr *add_or_get_lower_scalar_handoff(
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
    count_any_agg_func_oid = InvalidOid;
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
    age_array_agg_map_slots_agg_func_oid = InvalidOid;
    age_array_agg_list_slots_agg_func_oid = InvalidOid;
    agtype_build_map_nonull_func_oid = InvalidOid;
    agtype_build_list_func_oid = InvalidOid;
    agtype_ctid_property_field_agtype_func_oid = InvalidOid;
    agtype_id_property_field_agtype_func_oid = InvalidOid;
    int8_to_agtype_func_oid = InvalidOid;
    float8_to_agtype_func_oid = InvalidOid;
    numeric_to_agtype_func_oid = InvalidOid;
    text_to_agtype_func_oid = InvalidOid;
    agtype_eq_func_oid = InvalidOid;
    agtype_lt_func_oid = InvalidOid;
    agtype_le_func_oid = InvalidOid;
    agtype_gt_func_oid = InvalidOid;
    agtype_ge_func_oid = InvalidOid;
    agtype_field_equals_func_oid = InvalidOid;
    agtype_field_cmp_func_oid = InvalidOid;
    agtype_field_exists_nonnull_func_oid = InvalidOid;
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

static Oid get_cached_age_array_agg_map_slots_agg_oid(void)
{
    if (!OidIsValid(age_array_agg_map_slots_agg_func_oid))
    {
        age_array_agg_map_slots_agg_func_oid =
            get_ag_func_oid("age_array_agg_map_slots", 1, ANYOID);
    }

    return age_array_agg_map_slots_agg_func_oid;
}

static Oid get_cached_age_array_agg_list_slots_agg_oid(void)
{
    if (!OidIsValid(age_array_agg_list_slots_agg_func_oid))
    {
        age_array_agg_list_slots_agg_func_oid =
            get_ag_func_oid("age_array_agg_list_slots", 1, ANYOID);
    }

    return age_array_agg_list_slots_agg_func_oid;
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
