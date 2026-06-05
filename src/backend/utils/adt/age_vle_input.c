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

#include "utils/age_vle.h"

static void get_vle_input_scalar_arg_no_copy(const char *funcname,
                                             agtype *agt_arg,
                                             enum agtype_value_type type,
                                             bool error,
                                             agtype_value *result,
                                             bool *needs_free);

bool age_vle_input_arg_is_null(AgeVLEInput *input, int argno)
{
    if (input == NULL || argno < 0 || argno >= input->nargs)
        return true;

    return input->args[argno].isnull;
}

agtype *age_vle_input_get_agtype(AgeVLEInput *input, int argno)
{
    if (age_vle_input_arg_is_null(input, argno))
        return NULL;

    return DATUM_GET_AGTYPE_P(input->args[argno].value);
}

bool age_vle_input_get_vertex_or_id(AgeVLEInput *input, int argno,
                                    const char *type_error_msg,
                                    graphid *result)
{
    bool known;
    bool is_null;
    graphid vertex_id;

    (void)type_error_msg;

    if (argno == 1)
    {
        known = input->start_vertex_known;
        is_null = input->start_vertex_null;
        vertex_id = input->start_vertex_id;
    }
    else
    {
        Assert(argno == 2);
        known = input->end_vertex_known;
        is_null = input->end_vertex_null;
        vertex_id = input->end_vertex_id;
    }

    Assert(known);
    if (!known || is_null)
        return false;

    *result = vertex_id;

    return true;
}

char *age_vle_input_get_graph_name(AgeVLEInput *input, int *graph_name_len)
{
    agtype_value agtv_temp;
    bool agtv_temp_needs_free = false;
    char *graph_name;

    Assert(graph_name_len != NULL);

    if (input->graph_name_known)
    {
        if (input->graph_name_null)
        {
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                     errmsg("age_vle: agtype argument must not be AGTV_NULL")));
        }
        *graph_name_len = input->graph_name_len;
        return pnstrdup(input->graph_name_value, input->graph_name_len);
    }

    get_vle_input_scalar_arg_no_copy("age_vle",
                                     age_vle_input_get_agtype(input, 0),
                                     AGTV_STRING, true, &agtv_temp,
                                     &agtv_temp_needs_free);
    *graph_name_len = agtv_temp.val.string.len;
    graph_name = pnstrdup(agtv_temp.val.string.val, *graph_name_len);
    if (agtv_temp_needs_free)
        pfree_agtype_value_content(&agtv_temp);

    return graph_name;
}

int64 age_vle_input_get_grammar_node(AgeVLEInput *input)
{
    agtype_value agtv_temp;
    bool agtv_temp_needs_free = false;
    int64 vle_grammar_node_id;

    if (input->grammar_node_known)
    {
        if (input->grammar_node_null)
        {
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                     errmsg("age_vle: agtype argument must not be AGTV_NULL")));
        }
        return input->grammar_node_value;
    }

    get_vle_input_scalar_arg_no_copy("age_vle",
                                     age_vle_input_get_agtype(input, 7),
                                     AGTV_INTEGER, true, &agtv_temp,
                                     &agtv_temp_needs_free);
    vle_grammar_node_id = agtv_temp.val.int_value;
    if (agtv_temp_needs_free)
        pfree_agtype_value_content(&agtv_temp);

    return vle_grammar_node_id;
}

void age_vle_input_get_edge_prototype(AgeVLEInput *input,
                                      AgeVLEInputEdgePrototype *edge)
{
    agtype_value agtv_temp;
    agtype_value *agtv_object;
    bool agtv_temp_needs_free = false;

    Assert(edge != NULL);

    memset(edge, 0, sizeof(*edge));
    if (input->edge_prototype_known)
    {
        edge->property_constraint_count =
            input->edge_property_constraint_count;
        if (input->edge_label_known && input->edge_label_len != 0)
        {
            edge->label_known = true;
            edge->label_name = pnstrdup(input->edge_label_value,
                                        input->edge_label_len);
            edge->label_len = input->edge_label_len;
        }
        if (input->edge_properties_known &&
            !input->edge_properties_null &&
            edge->property_constraint_count > 0)
        {
            edge->property_constraint =
                (agtype *)PG_DETOAST_DATUM_COPY(
                    input->edge_properties_value);
        }
        return;
    }

    get_vle_input_scalar_arg_no_copy("age_vle",
                                     age_vle_input_get_agtype(input, 3),
                                     AGTV_EDGE, true, &agtv_temp,
                                     &agtv_temp_needs_free);
    agtv_object = AGTYPE_EDGE_GET_PROPERTIES(&agtv_temp);
    edge->property_constraint_count = agtv_object->val.object.num_pairs;
    if (edge->property_constraint_count > 0)
        edge->property_constraint = agtype_value_to_agtype(agtv_object);

    agtv_object = AGTYPE_EDGE_GET_LABEL(&agtv_temp);
    if (agtv_object->type == AGTV_STRING &&
        agtv_object->val.string.len != 0)
    {
        edge->label_known = true;
        edge->label_name = pnstrdup(agtv_object->val.string.val,
                                    agtv_object->val.string.len);
        edge->label_len = agtv_object->val.string.len;
    }

    if (agtv_temp_needs_free)
        pfree_agtype_value_content(&agtv_temp);
}

int64 age_vle_input_get_range_lower(AgeVLEInput *input)
{
    agtype *agt_arg;
    agtype_value agtv_temp;
    bool agtv_temp_needs_free = false;
    int64 lower;

    if (input->lower_known)
        return input->lower_null ? 1 : input->lower_value;

    if (age_vle_input_arg_is_null(input, 4))
        return 1;

    agt_arg = age_vle_input_get_agtype(input, 4);
    if (is_agtype_null(agt_arg))
        return 1;

    get_vle_input_scalar_arg_no_copy("age_vle", agt_arg, AGTV_INTEGER, true,
                                     &agtv_temp, &agtv_temp_needs_free);
    lower = agtv_temp.val.int_value;
    if (agtv_temp_needs_free)
        pfree_agtype_value_content(&agtv_temp);

    return lower;
}

void age_vle_input_get_range_upper(AgeVLEInput *input, int64 *upper,
                                   bool *is_infinite)
{
    agtype *agt_arg;
    agtype_value agtv_temp;
    bool agtv_temp_needs_free = false;

    Assert(upper != NULL);
    Assert(is_infinite != NULL);

    if (input->upper_known)
    {
        *is_infinite = input->upper_null;
        *upper = input->upper_null ? 0 : input->upper_value;
        return;
    }

    if (age_vle_input_arg_is_null(input, 5))
    {
        *is_infinite = true;
        *upper = 0;
        return;
    }

    agt_arg = age_vle_input_get_agtype(input, 5);
    if (is_agtype_null(agt_arg))
    {
        *is_infinite = true;
        *upper = 0;
        return;
    }

    get_vle_input_scalar_arg_no_copy("age_vle", agt_arg, AGTV_INTEGER, true,
                                     &agtv_temp, &agtv_temp_needs_free);
    *upper = agtv_temp.val.int_value;
    *is_infinite = false;
    if (agtv_temp_needs_free)
        pfree_agtype_value_content(&agtv_temp);
}

int64 age_vle_input_get_direction(AgeVLEInput *input)
{
    agtype_value agtv_temp;
    bool agtv_temp_needs_free = false;
    int64 direction;

    if (input->direction_known)
    {
        if (input->direction_null)
        {
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                     errmsg("age_vle: agtype argument must not be AGTV_NULL")));
        }
        return input->direction_value;
    }

    get_vle_input_scalar_arg_no_copy("age_vle",
                                     age_vle_input_get_agtype(input, 6),
                                     AGTV_INTEGER, true, &agtv_temp,
                                     &agtv_temp_needs_free);
    direction = agtv_temp.val.int_value;
    if (agtv_temp_needs_free)
        pfree_agtype_value_content(&agtv_temp);

    return direction;
}

bool age_vle_input_get_terminal_property_key(AgeVLEInput *input,
                                             agtype_value *key_value)
{
    agtype *agt_arg_key;
    bool key_needs_free = false;

    Assert(key_value != NULL);

    if (input->terminal_property_key_known)
    {
        if (input->terminal_property_key_null)
            return false;

        key_value->type = AGTV_STRING;
        key_value->val.string.val =
            (char *)input->terminal_property_key_value;
        key_value->val.string.len = input->terminal_property_key_len;
        return true;
    }

    agt_arg_key = age_vle_input_get_agtype(input, 8);
    get_vle_input_scalar_arg_no_copy("age_vle", agt_arg_key, AGTV_STRING,
                                     false, key_value, &key_needs_free);
    if (key_value->type == AGTV_STRING)
    {
        if (key_needs_free)
        {
            char *key_copy;

            key_copy = pnstrdup(key_value->val.string.val,
                                key_value->val.string.len);
            pfree_agtype_value_content(key_value);
            key_value->val.string.val = key_copy;
            key_value->type = AGTV_STRING;
        }
        return true;
    }

    if (key_needs_free)
        pfree_agtype_value_content(key_value);

    return false;
}

static void get_vle_input_scalar_arg_no_copy(const char *funcname,
                                             agtype *agt_arg,
                                             enum agtype_value_type type,
                                             bool error,
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
            pfree_agtype_value_content(result);
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("%s: agtype argument must not be AGTV_NULL",
                        funcname)));
    }

    if (error && result->type != type)
    {
        if (*needs_free)
            pfree_agtype_value_content(result);
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("%s: agtype argument of wrong type", funcname)));
    }
}
