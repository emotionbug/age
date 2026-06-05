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

#include "catalog/pg_type_d.h"
#include "nodes/makefuncs.h"
#include "parser/parse_func.h"
#include "utils/agtype.h"

#include "parser/cypher_vle_agg.h"
#include "utils/ag_func.h"
#include "utils/age_vle.h"

static Oid age_label_oid = InvalidOid;
static Oid age_labels_oid = InvalidOid;
static Oid age_properties_oid = InvalidOid;
static Oid age_type_oid = InvalidOid;
static Oid age_startnode_oid = InvalidOid;
static Oid age_endnode_oid = InvalidOid;
static Oid age_materialize_vle_node_tail_last_oid = InvalidOid;
static Oid age_vle_node_tail_last_id_oid = InvalidOid;
static Oid age_materialize_vle_edge_tail_last_oid = InvalidOid;
static Oid age_vle_edge_tail_last_id_oid = InvalidOid;
static Oid age_vle_tail_last_field_oid = InvalidOid;
static Oid age_vle_tail_last_edge_endpoint_oid = InvalidOid;
static Oid age_vle_tail_last_endpoint_field_oid = InvalidOid;
static Oid age_materialize_vle_slice_boundary_oid = InvalidOid;
static Oid count_any_agg_func_oid = InvalidOid;

static bool is_count_any_aggref(Aggref *aggref);
static bool get_agtype_integer_const_value(Const *c, int64 *value);
static FuncExpr *make_vle_unary_agtype_expr(Oid func_oid, Node *vle_expr);
static FuncExpr *make_vle_binary_mode_expr(Oid func_oid, Node *vle_expr,
                                           int64 mode, int location);
static Const *make_agtype_integer_const(int64 value, int location);
static FuncExpr *make_tail_last_entity_id_expr(FuncExpr *entity_expr);
static FuncExpr *make_tail_last_endpoint_id_expr(FuncExpr *endpoint_expr,
                                                 int location);
static FuncExpr *make_tail_last_field_id_expr(FuncExpr *field_expr,
                                              int location);
static FuncExpr *make_tail_last_wrapped_field_id_expr(FuncExpr *field_expr);
static FuncExpr *make_slice_boundary_count_id_expr(FuncExpr *boundary_expr);
static bool get_slice_boundary_count_id_mode(int64 mode, int64 *id_mode);
static int64 get_slice_boundary_wrapper_offset(int64 *mode);
static Oid get_age_label_oid(void);
static Oid get_age_labels_oid(void);
static Oid get_age_properties_oid(void);
static Oid get_age_type_oid(void);
static Oid get_age_startnode_oid(void);
static Oid get_age_endnode_oid(void);
static Oid get_age_materialize_vle_node_tail_last_oid(void);
static Oid get_age_vle_node_tail_last_id_oid(void);
static Oid get_age_materialize_vle_edge_tail_last_oid(void);
static Oid get_age_vle_edge_tail_last_id_oid(void);
static Oid get_age_vle_tail_last_field_oid(void);
static Oid get_age_vle_tail_last_edge_endpoint_oid(void);
static Oid get_age_vle_tail_last_endpoint_field_oid(void);
static Oid get_age_materialize_vle_slice_boundary_oid(void);
static Oid get_count_any_agg_oid(void);

bool cypher_rewrite_vle_tail_last_count_agg(Aggref *aggref)
{
    TargetEntry *arg_tle;
    FuncExpr *arg_expr;
    FuncExpr *replacement = NULL;

    if (!is_count_any_aggref(aggref))
    {
        return false;
    }

    arg_tle = linitial_node(TargetEntry, aggref->args);
    if (arg_tle == NULL || arg_tle->expr == NULL ||
        !IsA(arg_tle->expr, FuncExpr))
    {
        return false;
    }

    arg_expr = castNode(FuncExpr, arg_tle->expr);
    replacement = make_tail_last_entity_id_expr(arg_expr);
    if (replacement == NULL)
    {
        replacement = make_tail_last_endpoint_id_expr(arg_expr,
                                                      arg_expr->location);
    }
    if (replacement == NULL)
    {
        replacement = make_tail_last_field_id_expr(arg_expr,
                                                   arg_expr->location);
    }
    if (replacement == NULL)
    {
        replacement = make_tail_last_wrapped_field_id_expr(arg_expr);
    }
    if (replacement == NULL)
    {
        replacement = make_slice_boundary_count_id_expr(arg_expr);
    }
    if (replacement == NULL)
    {
        return false;
    }

    replacement->location = arg_expr->location;
    arg_tle->expr = (Expr *)replacement;

    return true;
}

void cypher_vle_agg_invalidate_oids(void)
{
    age_label_oid = InvalidOid;
    age_labels_oid = InvalidOid;
    age_properties_oid = InvalidOid;
    age_type_oid = InvalidOid;
    age_startnode_oid = InvalidOid;
    age_endnode_oid = InvalidOid;
    age_materialize_vle_node_tail_last_oid = InvalidOid;
    age_vle_node_tail_last_id_oid = InvalidOid;
    age_materialize_vle_edge_tail_last_oid = InvalidOid;
    age_vle_edge_tail_last_id_oid = InvalidOid;
    age_vle_tail_last_field_oid = InvalidOid;
    age_vle_tail_last_edge_endpoint_oid = InvalidOid;
    age_vle_tail_last_endpoint_field_oid = InvalidOid;
    age_materialize_vle_slice_boundary_oid = InvalidOid;
    count_any_agg_func_oid = InvalidOid;
}

static bool is_count_any_aggref(Aggref *aggref)
{
    return aggref != NULL &&
        aggref->aggfnoid == get_count_any_agg_oid() &&
        !aggref->aggstar &&
        aggref->aggdirectargs == NIL &&
        aggref->aggorder == NIL &&
        aggref->aggdistinct == NIL &&
        aggref->aggfilter == NULL &&
        list_length(aggref->args) == 1;
}

static bool get_agtype_integer_const_value(Const *c, int64 *value)
{
    agtype *agt;
    agtentry entry;
    char *base;
    uint32 type_header;

    if (c == NULL || c->constisnull || c->consttype != AGTYPEOID)
    {
        return false;
    }

    agt = DATUM_GET_AGTYPE_P(c->constvalue);
    if (!AGT_ROOT_IS_SCALAR(agt) || AGT_ROOT_COUNT(agt) != 1)
    {
        return false;
    }

    entry = agt->root.children[0];
    if (!AGTE_IS_AGTYPE(entry))
    {
        return false;
    }

    base = (char *)&agt->root.children[1];
    type_header = *((uint32 *)base);
    if (type_header != AGT_HEADER_INTEGER)
    {
        return false;
    }

    *value = *((int64 *)(base + sizeof(uint32)));
    return true;
}

static FuncExpr *make_vle_unary_agtype_expr(Oid func_oid, Node *vle_expr)
{
    return makeFuncExpr(func_oid, AGTYPEOID, list_make1(vle_expr),
                        InvalidOid, InvalidOid, COERCE_EXPLICIT_CALL);
}

static FuncExpr *make_vle_binary_mode_expr(Oid func_oid, Node *vle_expr,
                                           int64 mode, int location)
{
    Const *mode_expr;

    mode_expr = make_agtype_integer_const(mode, location);
    return makeFuncExpr(func_oid, AGTYPEOID, list_make2(vle_expr, mode_expr),
                        InvalidOid, InvalidOid, COERCE_EXPLICIT_CALL);
}

static Const *make_agtype_integer_const(int64 value, int location)
{
    Datum agt;
    Const *c;

    agt = integer_to_agtype(value);
    c = makeConst(AGTYPEOID, -1, InvalidOid, -1, agt, false, false);
    c->location = location;

    return c;
}

static FuncExpr *make_tail_last_entity_id_expr(FuncExpr *entity_expr)
{
    if (entity_expr->funcid == get_age_materialize_vle_node_tail_last_oid() &&
        list_length(entity_expr->args) == 1)
    {
        return make_vle_unary_agtype_expr(
            get_age_vle_node_tail_last_id_oid(),
            copyObject(linitial(entity_expr->args)));
    }

    if (entity_expr->funcid == get_age_materialize_vle_edge_tail_last_oid() &&
        list_length(entity_expr->args) == 1)
    {
        return make_vle_unary_agtype_expr(
            get_age_vle_edge_tail_last_id_oid(),
            copyObject(linitial(entity_expr->args)));
    }

    return NULL;
}

static FuncExpr *make_tail_last_endpoint_id_expr(FuncExpr *endpoint_expr,
                                                 int location)
{
    FuncExpr *edge_expr;
    int64 mode;

    if (endpoint_expr->funcid == get_age_vle_tail_last_edge_endpoint_oid() &&
        list_length(endpoint_expr->args) == 2 &&
        IsA(lsecond(endpoint_expr->args), Const))
    {
        Const *mode_const;

        mode_const = lsecond_node(Const, endpoint_expr->args);
        if (!get_agtype_integer_const_value(mode_const, &mode) ||
            mode < 0 || mode > 1)
        {
            return NULL;
        }

        return make_vle_binary_mode_expr(
            get_age_vle_tail_last_edge_endpoint_oid(),
            copyObject(linitial(endpoint_expr->args)), mode + 2,
            location);
    }

    if ((endpoint_expr->funcid != get_age_startnode_oid() &&
         endpoint_expr->funcid != get_age_endnode_oid()) ||
        list_length(endpoint_expr->args) != 2 ||
        !IsA(lsecond(endpoint_expr->args), FuncExpr))
    {
        return NULL;
    }

    edge_expr = lsecond_node(FuncExpr, endpoint_expr->args);
    if (edge_expr->funcid != get_age_materialize_vle_edge_tail_last_oid() ||
        list_length(edge_expr->args) != 1)
    {
        return NULL;
    }

    mode = (endpoint_expr->funcid == get_age_startnode_oid()) ? 2 : 3;
    return make_vle_binary_mode_expr(
        get_age_vle_tail_last_edge_endpoint_oid(),
        copyObject(linitial(edge_expr->args)), mode, location);
}

static FuncExpr *make_tail_last_field_id_expr(FuncExpr *field_expr,
                                              int location)
{
    Const *mode_const;
    int64 mode;
    Oid func_oid;
    int64 endpoint_id_mode;

    if (field_expr->funcid == get_age_vle_tail_last_field_oid() &&
        list_length(field_expr->args) == 2 &&
        IsA(lsecond(field_expr->args), Const))
    {
        mode_const = lsecond_node(Const, field_expr->args);
        if (!get_agtype_integer_const_value(mode_const, &mode) ||
            mode < 0 || mode > 4)
        {
            return NULL;
        }

        func_oid = (mode <= 2) ? get_age_vle_node_tail_last_id_oid() :
            get_age_vle_edge_tail_last_id_oid();
        return make_vle_unary_agtype_expr(
            func_oid, copyObject(linitial(field_expr->args)));
    }

    if (field_expr->funcid != get_age_vle_tail_last_endpoint_field_oid() ||
        list_length(field_expr->args) != 2 ||
        !IsA(lsecond(field_expr->args), Const))
    {
        return NULL;
    }

    mode_const = lsecond_node(Const, field_expr->args);
    if (!get_agtype_integer_const_value(mode_const, &mode) ||
        mode < 0 || mode > 5)
    {
        return NULL;
    }

    endpoint_id_mode = (mode % 2 == 0) ? 2 : 3;
    return make_vle_binary_mode_expr(
        get_age_vle_tail_last_edge_endpoint_oid(),
        copyObject(linitial(field_expr->args)), endpoint_id_mode, location);
}

static FuncExpr *make_tail_last_wrapped_field_id_expr(FuncExpr *field_expr)
{
    FuncExpr *inner_expr;
    bool label_wrapper;
    bool labels_wrapper;
    bool properties_wrapper;
    bool type_wrapper;

    label_wrapper = field_expr->funcid == get_age_label_oid();
    labels_wrapper = field_expr->funcid == get_age_labels_oid();
    properties_wrapper = field_expr->funcid == get_age_properties_oid();
    type_wrapper = field_expr->funcid == get_age_type_oid();

    if ((!label_wrapper && !labels_wrapper && !properties_wrapper &&
         !type_wrapper) ||
         list_length(field_expr->args) != 1 ||
        !IsA(linitial(field_expr->args), FuncExpr))
    {
        return NULL;
    }

    inner_expr = linitial_node(FuncExpr, field_expr->args);

    if (inner_expr->funcid == get_age_materialize_vle_node_tail_last_oid() &&
        list_length(inner_expr->args) == 1 &&
        (label_wrapper || labels_wrapper || properties_wrapper))
    {
        return make_vle_unary_agtype_expr(
            get_age_vle_node_tail_last_id_oid(),
            copyObject(linitial(inner_expr->args)));
    }

    if (inner_expr->funcid == get_age_materialize_vle_edge_tail_last_oid() &&
        list_length(inner_expr->args) == 1 &&
        (label_wrapper || properties_wrapper || type_wrapper))
    {
        return make_vle_unary_agtype_expr(
            get_age_vle_edge_tail_last_id_oid(),
            copyObject(linitial(inner_expr->args)));
    }

    if (label_wrapper || labels_wrapper || properties_wrapper)
    {
        return make_tail_last_endpoint_id_expr(inner_expr,
                                               field_expr->location);
    }

    return NULL;
}

static FuncExpr *make_slice_boundary_count_id_expr(FuncExpr *boundary_expr)
{
    Const *mode_const;
    int64 mode;
    int64 id_mode;

    if (boundary_expr->funcid != get_age_materialize_vle_slice_boundary_oid() ||
        list_length(boundary_expr->args) != 4 ||
        !IsA(lfourth(boundary_expr->args), Const))
    {
        return NULL;
    }

    mode_const = lfourth_node(Const, boundary_expr->args);
    if (!get_agtype_integer_const_value(mode_const, &mode) ||
        !get_slice_boundary_count_id_mode(mode, &id_mode))
    {
        return NULL;
    }

    return makeFuncExpr(
        get_age_materialize_vle_slice_boundary_oid(), AGTYPEOID,
        list_make4(copyObject(linitial(boundary_expr->args)),
                   copyObject(lsecond(boundary_expr->args)),
                   copyObject(lthird(boundary_expr->args)),
                   make_agtype_integer_const(id_mode,
                                             boundary_expr->location)),
        InvalidOid, InvalidOid, COERCE_EXPLICIT_CALL);
}

static bool get_slice_boundary_count_id_mode(int64 mode, int64 *id_mode)
{
    int64 wrapper_offset;
    int64 return_offset;
    int64 base_mode;

    wrapper_offset = get_slice_boundary_wrapper_offset(&mode);
    if (mode < 0 || mode >= VLE_SLICE_BOUNDARY_REVERSE_OFFSET)
    {
        return false;
    }

    if (mode >= VLE_SLICE_BOUNDARY_END_PROPERTIES_OFFSET)
    {
        return_offset = VLE_SLICE_BOUNDARY_END_ID_OFFSET;
        base_mode = mode - VLE_SLICE_BOUNDARY_END_PROPERTIES_OFFSET;
    }
    else if (mode >= VLE_SLICE_BOUNDARY_START_PROPERTIES_OFFSET)
    {
        return_offset = VLE_SLICE_BOUNDARY_START_ID_OFFSET;
        base_mode = mode - VLE_SLICE_BOUNDARY_START_PROPERTIES_OFFSET;
    }
    else if (mode >= VLE_SLICE_BOUNDARY_END_LABELS_OFFSET)
    {
        return_offset = VLE_SLICE_BOUNDARY_END_ID_OFFSET;
        base_mode = mode - VLE_SLICE_BOUNDARY_END_LABELS_OFFSET;
    }
    else if (mode >= VLE_SLICE_BOUNDARY_START_LABELS_OFFSET)
    {
        return_offset = VLE_SLICE_BOUNDARY_START_ID_OFFSET;
        base_mode = mode - VLE_SLICE_BOUNDARY_START_LABELS_OFFSET;
    }
    else if (mode >= VLE_SLICE_BOUNDARY_END_LABEL_OFFSET)
    {
        return_offset = VLE_SLICE_BOUNDARY_END_ID_OFFSET;
        base_mode = mode - VLE_SLICE_BOUNDARY_END_LABEL_OFFSET;
    }
    else if (mode >= VLE_SLICE_BOUNDARY_START_LABEL_OFFSET)
    {
        return_offset = VLE_SLICE_BOUNDARY_START_ID_OFFSET;
        base_mode = mode - VLE_SLICE_BOUNDARY_START_LABEL_OFFSET;
    }
    else if (mode >= VLE_SLICE_BOUNDARY_END_ID_OFFSET)
    {
        return false;
    }
    else if (mode >= VLE_SLICE_BOUNDARY_START_ID_OFFSET)
    {
        return false;
    }
    else if (mode >= VLE_SLICE_BOUNDARY_END_NODE_OFFSET)
    {
        return_offset = VLE_SLICE_BOUNDARY_END_ID_OFFSET;
        base_mode = mode - VLE_SLICE_BOUNDARY_END_NODE_OFFSET;
    }
    else if (mode >= VLE_SLICE_BOUNDARY_START_NODE_OFFSET)
    {
        return_offset = VLE_SLICE_BOUNDARY_START_ID_OFFSET;
        base_mode = mode - VLE_SLICE_BOUNDARY_START_NODE_OFFSET;
    }
    else if (mode >= VLE_SLICE_BOUNDARY_PROPERTIES_OFFSET)
    {
        return_offset = VLE_SLICE_BOUNDARY_ID_OFFSET;
        base_mode = mode - VLE_SLICE_BOUNDARY_PROPERTIES_OFFSET;
    }
    else if (mode >= VLE_SLICE_BOUNDARY_LABELS_OFFSET)
    {
        return_offset = VLE_SLICE_BOUNDARY_ID_OFFSET;
        base_mode = mode - VLE_SLICE_BOUNDARY_LABELS_OFFSET;
    }
    else if (mode >= VLE_SLICE_BOUNDARY_LABEL_OFFSET)
    {
        return_offset = VLE_SLICE_BOUNDARY_ID_OFFSET;
        base_mode = mode - VLE_SLICE_BOUNDARY_LABEL_OFFSET;
    }
    else if (mode >= VLE_SLICE_BOUNDARY_ID_OFFSET)
    {
        return false;
    }
    else
    {
        return_offset = VLE_SLICE_BOUNDARY_ID_OFFSET;
        base_mode = mode;
    }

    if (base_mode < 0 || base_mode > 7)
    {
        return false;
    }

    *id_mode = wrapper_offset + return_offset + base_mode;
    return true;
}

static int64 get_slice_boundary_wrapper_offset(int64 *mode)
{
    if (*mode >= VLE_SLICE_BOUNDARY_SLICE_REVERSE_OFFSET)
    {
        *mode -= VLE_SLICE_BOUNDARY_SLICE_REVERSE_OFFSET;
        return VLE_SLICE_BOUNDARY_SLICE_REVERSE_OFFSET;
    }
    if (*mode >= VLE_SLICE_BOUNDARY_SLICE_TAIL_OFFSET)
    {
        *mode -= VLE_SLICE_BOUNDARY_SLICE_TAIL_OFFSET;
        return VLE_SLICE_BOUNDARY_SLICE_TAIL_OFFSET;
    }
    if (*mode >= VLE_SLICE_BOUNDARY_DOUBLE_TAIL_OFFSET)
    {
        *mode -= VLE_SLICE_BOUNDARY_DOUBLE_TAIL_OFFSET;
        return VLE_SLICE_BOUNDARY_DOUBLE_TAIL_OFFSET;
    }
    if (*mode >= VLE_SLICE_BOUNDARY_TAIL_REVERSE_OFFSET)
    {
        *mode -= VLE_SLICE_BOUNDARY_TAIL_REVERSE_OFFSET;
        return VLE_SLICE_BOUNDARY_TAIL_REVERSE_OFFSET;
    }
    if (*mode >= VLE_SLICE_BOUNDARY_REVERSE_OFFSET)
    {
        *mode -= VLE_SLICE_BOUNDARY_REVERSE_OFFSET;
        return VLE_SLICE_BOUNDARY_REVERSE_OFFSET;
    }

    return 0;
}

static Oid get_age_label_oid(void)
{
    if (!OidIsValid(age_label_oid))
        age_label_oid = get_ag_func_oid("age_label", 1, AGTYPEOID);

    return age_label_oid;
}

static Oid get_age_labels_oid(void)
{
    if (!OidIsValid(age_labels_oid))
        age_labels_oid = get_ag_func_oid("age_labels", 1, AGTYPEOID);

    return age_labels_oid;
}

static Oid get_age_properties_oid(void)
{
    if (!OidIsValid(age_properties_oid))
        age_properties_oid = get_ag_func_oid("age_properties", 1, AGTYPEOID);

    return age_properties_oid;
}

static Oid get_age_type_oid(void)
{
    if (!OidIsValid(age_type_oid))
        age_type_oid = get_ag_func_oid("age_type", 1, AGTYPEOID);

    return age_type_oid;
}

static Oid get_age_startnode_oid(void)
{
    if (!OidIsValid(age_startnode_oid))
    {
        age_startnode_oid = get_ag_func_oid("age_startnode", 2,
                                            AGTYPEOID, AGTYPEOID);
    }

    return age_startnode_oid;
}

static Oid get_age_endnode_oid(void)
{
    if (!OidIsValid(age_endnode_oid))
    {
        age_endnode_oid = get_ag_func_oid("age_endnode", 2,
                                          AGTYPEOID, AGTYPEOID);
    }

    return age_endnode_oid;
}

static Oid get_age_materialize_vle_node_tail_last_oid(void)
{
    if (!OidIsValid(age_materialize_vle_node_tail_last_oid))
    {
        age_materialize_vle_node_tail_last_oid =
            get_age_builtin_func_oid_by_name(
                "age_materialize_vle_node_tail_last");
    }

    return age_materialize_vle_node_tail_last_oid;
}

static Oid get_age_vle_node_tail_last_id_oid(void)
{
    if (!OidIsValid(age_vle_node_tail_last_id_oid))
    {
        age_vle_node_tail_last_id_oid =
            get_ag_func_oid("age_vle_node_tail_last_id", 1, AGTYPEOID);
    }

    return age_vle_node_tail_last_id_oid;
}

static Oid get_age_materialize_vle_edge_tail_last_oid(void)
{
    if (!OidIsValid(age_materialize_vle_edge_tail_last_oid))
    {
        age_materialize_vle_edge_tail_last_oid =
            get_age_builtin_func_oid_by_name(
                "age_materialize_vle_edge_tail_last");
    }

    return age_materialize_vle_edge_tail_last_oid;
}

static Oid get_age_vle_edge_tail_last_id_oid(void)
{
    if (!OidIsValid(age_vle_edge_tail_last_id_oid))
    {
        age_vle_edge_tail_last_id_oid =
            get_ag_func_oid("age_vle_edge_tail_last_id", 1, AGTYPEOID);
    }

    return age_vle_edge_tail_last_id_oid;
}

static Oid get_age_vle_tail_last_field_oid(void)
{
    if (!OidIsValid(age_vle_tail_last_field_oid))
    {
        age_vle_tail_last_field_oid =
            get_ag_func_oid("age_vle_tail_last_field", 2,
                            AGTYPEOID, AGTYPEOID);
    }

    return age_vle_tail_last_field_oid;
}

static Oid get_age_vle_tail_last_edge_endpoint_oid(void)
{
    if (!OidIsValid(age_vle_tail_last_edge_endpoint_oid))
    {
        age_vle_tail_last_edge_endpoint_oid =
            get_ag_func_oid("age_vle_tail_last_edge_endpoint", 2,
                            AGTYPEOID, AGTYPEOID);
    }

    return age_vle_tail_last_edge_endpoint_oid;
}

static Oid get_age_vle_tail_last_endpoint_field_oid(void)
{
    if (!OidIsValid(age_vle_tail_last_endpoint_field_oid))
    {
        age_vle_tail_last_endpoint_field_oid =
            get_ag_func_oid("age_vle_tail_last_endpoint_field", 2,
                            AGTYPEOID, AGTYPEOID);
    }

    return age_vle_tail_last_endpoint_field_oid;
}

static Oid get_age_materialize_vle_slice_boundary_oid(void)
{
    if (!OidIsValid(age_materialize_vle_slice_boundary_oid))
    {
        age_materialize_vle_slice_boundary_oid =
            get_age_builtin_func_oid_by_name(
                "age_materialize_vle_slice_boundary");
    }

    return age_materialize_vle_slice_boundary_oid;
}

static Oid get_count_any_agg_oid(void)
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
