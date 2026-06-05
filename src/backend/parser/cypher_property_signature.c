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
#include "nodes/nodeFuncs.h"
#include "nodes/primnodes.h"

#include "parser/cypher_property_signature.h"
#include "utils/ag_func.h"
#include "utils/agtype.h"

static Oid agtype_access_operator_oid = InvalidOid;
static Oid agtype_object_field_agtype_oid = InvalidOid;
static Oid agtype_object_field_int8_oid = InvalidOid;
static Oid agtype_object_field_float8_oid = InvalidOid;
static Oid agtype_object_field_numeric_agtype_oid = InvalidOid;
static Oid agtype_object_field_numeric_oid = InvalidOid;
static Oid agtype_object_field_text_agtype_oid = InvalidOid;

#define AGTYPE_FIELD_DESCRIPTOR_INDEX 0

typedef struct CypherPropertyFieldDescriptor
{
    Oid value_type;
    /* InvalidOid marks AGTYPEOID, which must be resolved at runtime. */
    Oid field_result_type;
    const char *field_name;
    Oid *field_oid;
} CypherPropertyFieldDescriptor;

static CypherPropertyFieldDescriptor property_field_descriptors[] = {
    {InvalidOid, InvalidOid, "agtype_object_field_agtype",
     &agtype_object_field_agtype_oid},
    {INT8OID, INT8OID, "agtype_object_field_int8",
     &agtype_object_field_int8_oid},
    {FLOAT8OID, FLOAT8OID, "agtype_object_field_float8",
     &agtype_object_field_float8_oid},
    {NUMERICOID, InvalidOid, "agtype_object_field_numeric_agtype",
     &agtype_object_field_numeric_agtype_oid},
    {NUMERICOID, NUMERICOID, "agtype_object_field_numeric",
     &agtype_object_field_numeric_oid},
    {TEXTOID, TEXTOID, "agtype_object_field_text_agtype",
     &agtype_object_field_text_agtype_oid}
};

static bool property_key_paths_equal(List *left, List *right);
static Node *make_prefix_property_access_expr(Node *container, List *keys);
static CypherPropertyFieldDescriptor *find_property_field_descriptor(
    Oid funcid);
static CypherPropertyFieldDescriptor *find_property_field_descriptor_by_type(
    Oid value_type, Oid field_result_type);
static Oid get_property_field_oid_from_descriptor(
    CypherPropertyFieldDescriptor *descriptor);
static Oid get_property_field_value_type(
    CypherPropertyFieldDescriptor *descriptor);
static Oid get_property_field_result_type(
    CypherPropertyFieldDescriptor *descriptor);
static Oid get_agtype_access_operator_oid(void);

bool cypher_extract_property_access_signature(
    Node *node, CypherPropertyAccessSignature *signature)
{
    FuncExpr *func;

    if (signature == NULL)
        return false;

    signature->container = NULL;
    signature->keys = NIL;
    signature->value_type = InvalidOid;
    signature->field_result_type = InvalidOid;

    node = strip_implicit_coercions(node);

    if (node == NULL || !IsA(node, FuncExpr))
        return false;

    func = castNode(FuncExpr, node);

    {
        CypherPropertyFieldDescriptor *descriptor;

        descriptor = find_property_field_descriptor(func->funcid);
        if (descriptor != NULL)
        {
            CypherPropertyAccessSignature inner_signature;

            if (list_length(func->args) != 2)
                return false;

            signature->value_type = get_property_field_value_type(descriptor);
            signature->field_result_type =
                get_property_field_result_type(descriptor);

            if (cypher_extract_property_access_signature(linitial(func->args),
                                                         &inner_signature))
            {
                signature->container = inner_signature.container;
                signature->keys = lappend(inner_signature.keys,
                                          lsecond(func->args));
            }
            else
            {
                signature->container = linitial(func->args);
                signature->keys = list_make1(lsecond(func->args));
            }

            return true;
        }
    }

    if (func->funcid == get_agtype_access_operator_oid())
    {
        ArrayExpr *array;

        signature->value_type = AGTYPEOID;
        signature->field_result_type = AGTYPEOID;

        if (list_length(func->args) == 2)
        {
            signature->container = linitial(func->args);
            signature->keys = list_make1(lsecond(func->args));
            return true;
        }

        if (list_length(func->args) != 1 || !IsA(linitial(func->args), ArrayExpr))
            return false;

        array = linitial_node(ArrayExpr, func->args);
        if (list_length(array->elements) < 2)
            return false;

        signature->container = linitial(array->elements);
        signature->keys = list_copy_tail(array->elements, 1);
        return true;
    }

    return false;
}

bool cypher_equal_property_access_signature(Node *left, Node *right)
{
    CypherPropertyAccessSignature left_signature;
    CypherPropertyAccessSignature right_signature;

    if (!cypher_extract_property_access_signature(left, &left_signature) ||
        !cypher_extract_property_access_signature(right, &right_signature))
    {
        return false;
    }

    return equal(left_signature.container, right_signature.container) &&
           property_key_paths_equal(left_signature.keys, right_signature.keys) &&
           left_signature.value_type == right_signature.value_type &&
           left_signature.field_result_type ==
           right_signature.field_result_type;
}

bool cypher_property_access_signature_matches(
    const CypherPropertyAccessSignature *signature, Node *expr)
{
    CypherPropertyAccessSignature expr_signature;

    if (signature == NULL ||
        !cypher_extract_property_access_signature(expr, &expr_signature))
    {
        return false;
    }

    return equal(signature->container, expr_signature.container) &&
           property_key_paths_equal(signature->keys, expr_signature.keys) &&
           signature->value_type == expr_signature.value_type &&
           signature->field_result_type == expr_signature.field_result_type;
}

bool cypher_extract_property_access_terminal_args(Node *node, Node **object,
                                                  Node **key)
{
    CypherPropertyAccessSignature signature;
    int key_count;

    if (object == NULL || key == NULL ||
        !cypher_extract_property_access_signature(node, &signature))
    {
        return false;
    }

    key_count = list_length(signature.keys);
    if (key_count == 0)
        return false;

    *object = make_prefix_property_access_expr(signature.container,
                                               list_copy_head(signature.keys,
                                                              key_count - 1));
    *key = copyObject(llast(signature.keys));
    return true;
}

void cypher_property_signature_invalidate_oids(void)
{
    agtype_access_operator_oid = InvalidOid;
    agtype_object_field_agtype_oid = InvalidOid;
    agtype_object_field_int8_oid = InvalidOid;
    agtype_object_field_float8_oid = InvalidOid;
    agtype_object_field_numeric_agtype_oid = InvalidOid;
    agtype_object_field_numeric_oid = InvalidOid;
    agtype_object_field_text_agtype_oid = InvalidOid;
}

Oid cypher_get_property_field_oid(Oid value_type, Oid field_result_type)
{
    CypherPropertyFieldDescriptor *descriptor;

    descriptor = find_property_field_descriptor_by_type(value_type,
                                                        field_result_type);
    if (descriptor == NULL)
        return InvalidOid;

    return get_property_field_oid_from_descriptor(descriptor);
}

bool cypher_property_field_func_matches(Oid funcid, Oid value_type,
                                        Oid field_result_type)
{
    CypherPropertyFieldDescriptor *descriptor;

    descriptor = find_property_field_descriptor_by_type(value_type,
                                                        field_result_type);
    if (descriptor == NULL)
        return false;

    return get_property_field_oid_from_descriptor(descriptor) == funcid;
}

static bool property_key_paths_equal(List *left, List *right)
{
    ListCell *left_lc;
    ListCell *right_lc;

    if (list_length(left) != list_length(right))
        return false;

    forboth(left_lc, left, right_lc, right)
    {
        if (!equal(lfirst(left_lc), lfirst(right_lc)))
            return false;
    }

    return true;
}

static Node *make_prefix_property_access_expr(Node *container, List *keys)
{
    Node *prefix;
    ListCell *lc;

    Assert(container != NULL);

    prefix = copyObject(container);
    foreach(lc, keys)
    {
        prefix = (Node *)makeFuncExpr(get_property_field_oid_from_descriptor(
                                          &property_field_descriptors[
                                              AGTYPE_FIELD_DESCRIPTOR_INDEX]),
                                      AGTYPEOID,
                                      list_make2(prefix, copyObject(lfirst(lc))),
                                      InvalidOid, InvalidOid,
                                      COERCE_EXPLICIT_CALL);
    }

    return prefix;
}

static CypherPropertyFieldDescriptor *find_property_field_descriptor(
    Oid funcid)
{
    int i;

    if (!OidIsValid(funcid))
        return NULL;

    for (i = 0; i < lengthof(property_field_descriptors); i++)
    {
        CypherPropertyFieldDescriptor *descriptor =
            &property_field_descriptors[i];

        if (get_property_field_oid_from_descriptor(descriptor) == funcid)
            return descriptor;
    }

    return NULL;
}

static CypherPropertyFieldDescriptor *find_property_field_descriptor_by_type(
    Oid value_type, Oid field_result_type)
{
    int i;

    for (i = 0; i < lengthof(property_field_descriptors); i++)
    {
        CypherPropertyFieldDescriptor *descriptor =
            &property_field_descriptors[i];

        if (get_property_field_value_type(descriptor) == value_type &&
            get_property_field_result_type(descriptor) == field_result_type)
        {
            return descriptor;
        }
    }

    return NULL;
}

static Oid get_property_field_oid_from_descriptor(
    CypherPropertyFieldDescriptor *descriptor)
{
    Assert(descriptor != NULL);

    if (!OidIsValid(*descriptor->field_oid))
    {
        *descriptor->field_oid =
            get_ag_func_oid(descriptor->field_name, 2,
                            AGTYPEOID, AGTYPEOID);
    }

    return *descriptor->field_oid;
}

static Oid get_property_field_value_type(
    CypherPropertyFieldDescriptor *descriptor)
{
    Assert(descriptor != NULL);

    if (!OidIsValid(descriptor->value_type))
        return AGTYPEOID;

    return descriptor->value_type;
}

static Oid get_property_field_result_type(
    CypherPropertyFieldDescriptor *descriptor)
{
    Assert(descriptor != NULL);

    if (!OidIsValid(descriptor->field_result_type))
        return AGTYPEOID;

    return descriptor->field_result_type;
}

static Oid get_agtype_access_operator_oid(void)
{
    if (!OidIsValid(agtype_access_operator_oid))
    {
        agtype_access_operator_oid =
            get_ag_func_oid("agtype_access_operator", 1, AGTYPEARRAYOID);
    }

    return agtype_access_operator_oid;
}
