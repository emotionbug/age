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

#include "access/htup_details.h"
#include "utils/age_vle_container.h"
#include "utils/agtype.h"

static VLE_path_container *create_VLE_path_container(int64 path_size);
static void set_VLE_container_materializer_metadata(
    VLE_path_container *vpc, Oid graph_oid,
    VLEMaterializerOutputRequirement output_requirement,
    graphid traversal_root_id, bool traversal_root_valid,
    graphid candidate_vertex_id, bool candidate_vertex_valid);
static VLE_path_container *build_VLE_path_container(
    const VLEContainerBuildInput *input);
static VLE_path_container *build_reversed_VLE_path_container(
    const VLEContainerBuildInput *input);
static VLE_path_container *build_VLE_terminal_container(
    const VLEContainerBuildInput *input);
static VLE_path_container *build_VLE_terminal_zero_container(
    const VLEContainerBuildInput *input);
static VLE_path_container *build_VLE_zero_container(
    const VLEContainerBuildInput *input);

static VLE_path_container *create_VLE_path_container(int64 path_size)
{
    VLE_path_container *vpc = NULL;
    Size container_size_bytes = 0;

    container_size_bytes = offsetof(VLE_path_container, graphid_array_data) +
                           (sizeof(graphid) * path_size);

    vpc = palloc(container_size_bytes);

    SET_VARSIZE(vpc, container_size_bytes);

    vpc->header = AGT_FBINARY | AGT_FBINARY_TYPE_VLE_PATH;
    vpc->graphid_array_size = path_size;
    vpc->container_size_bytes = container_size_bytes;
    vpc->traversal_root_id = 0;
    vpc->candidate_vertex_id = 0;
    vpc->output_requirement = VLE_MATERIALIZER_OUTPUT_UNKNOWN;
    vpc->traversal_root_valid = false;
    vpc->candidate_vertex_valid = false;

    return vpc;
}

static void set_VLE_container_materializer_metadata(
    VLE_path_container *vpc, Oid graph_oid,
    VLEMaterializerOutputRequirement output_requirement,
    graphid traversal_root_id, bool traversal_root_valid,
    graphid candidate_vertex_id, bool candidate_vertex_valid)
{
    Assert(vpc != NULL);

    vpc->graph_oid = graph_oid;
    vpc->output_requirement = output_requirement;
    vpc->traversal_root_id = traversal_root_id;
    vpc->traversal_root_valid = traversal_root_valid;
    vpc->candidate_vertex_id = candidate_vertex_id;
    vpc->candidate_vertex_valid = candidate_vertex_valid;
}

static VLE_path_container *build_VLE_path_container(
    const VLEContainerBuildInput *input)
{
    GraphIdStack *stack;
    GraphIdStack *vertex_stack;
    VLE_path_container *vpc = NULL;
    graphid *graphid_array = NULL;
    graphid *edge_array = NULL;
    graphid *vertex_array = NULL;
    int ssize = 0;
    int j = 0;

    Assert(input != NULL);
    stack = input->path_stack;
    vertex_stack = input->path_vertex_stack;
    if (stack == NULL)
    {
        return NULL;
    }

    ssize = gid_stack_size(stack);
    vpc = create_VLE_path_container((ssize * 2) + 1);

    Assert(gid_stack_size(vertex_stack) == ssize + 1);
    set_VLE_container_materializer_metadata(
        vpc, input->graph_oid, VLE_MATERIALIZER_OUTPUT_PATH,
        vertex_stack->array[0], true, vertex_stack->array[ssize], true);
    graphid_array = GET_GRAPHID_ARRAY_FROM_CONTAINER(vpc);
    edge_array = stack->array;
    vertex_array = vertex_stack->array;

    for (j = 0; j < ssize; j++)
    {
        graphid_array[j * 2] = vertex_array[j];
        graphid_array[(j * 2) + 1] = edge_array[j];
    }
    graphid_array[ssize * 2] = vertex_array[ssize];

    return vpc;
}

static VLE_path_container *build_reversed_VLE_path_container(
    const VLEContainerBuildInput *input)
{
    GraphIdStack *stack;
    GraphIdStack *vertex_stack;
    VLE_path_container *vpc = NULL;
    graphid *graphid_array = NULL;
    graphid *edge_array = NULL;
    graphid *vertex_array = NULL;
    int index = 0;
    int ssize = 0;
    int j = 0;

    Assert(input != NULL);
    stack = input->path_stack;
    vertex_stack = input->path_vertex_stack;
    if (stack == NULL)
    {
        return NULL;
    }

    ssize = gid_stack_size(stack);
    vpc = create_VLE_path_container((ssize * 2) + 1);

    Assert(gid_stack_size(vertex_stack) == ssize + 1);
    set_VLE_container_materializer_metadata(
        vpc, input->graph_oid, VLE_MATERIALIZER_OUTPUT_PATH,
        vertex_stack->array[0], true, vertex_stack->array[0], true);
    graphid_array = GET_GRAPHID_ARRAY_FROM_CONTAINER(vpc);
    edge_array = stack->array;
    vertex_array = vertex_stack->array;

    graphid_array[0] = vertex_array[ssize];
    index = 1;
    for (j = ssize - 1; j >= 0; j--)
    {
        graphid_array[index] = edge_array[j];
        graphid_array[index + 1] = vertex_array[j];
        index += 2;
    }

    return vpc;
}

static VLE_path_container *build_VLE_terminal_container(
    const VLEContainerBuildInput *input)
{
    GraphIdStack *vertex_stack;
    VLE_path_container *vpc = NULL;
    graphid *graphid_array = NULL;
    int ssize;
    graphid terminal_id;

    Assert(input != NULL);
    vertex_stack = input->path_vertex_stack;
    if (vertex_stack == NULL)
    {
        return NULL;
    }

    ssize = gid_stack_size(input->path_stack);
    Assert(gid_stack_size(vertex_stack) == ssize + 1);

    terminal_id = input->reverse_output_path ? vertex_stack->array[0] :
                                               vertex_stack->array[ssize];

    vpc = create_VLE_path_container(1);
    set_VLE_container_materializer_metadata(
        vpc, input->graph_oid, VLE_MATERIALIZER_OUTPUT_TYPED_VERTEX,
        vertex_stack->array[0], true, terminal_id, true);

    graphid_array = GET_GRAPHID_ARRAY_FROM_CONTAINER(vpc);
    graphid_array[0] = terminal_id;

    return vpc;
}

static VLE_path_container *build_VLE_terminal_zero_container(
    const VLEContainerBuildInput *input)
{
    VLE_path_container *vpc = NULL;
    graphid *graphid_array = NULL;

    Assert(input != NULL);

    vpc = create_VLE_path_container(1);
    set_VLE_container_materializer_metadata(
        vpc, input->graph_oid, VLE_MATERIALIZER_OUTPUT_TYPED_VERTEX,
        input->start_vertex_id, true, input->start_vertex_id, true);

    graphid_array = GET_GRAPHID_ARRAY_FROM_CONTAINER(vpc);
    graphid_array[0] = input->start_vertex_id;

    return vpc;
}

static VLE_path_container *build_VLE_zero_container(
    const VLEContainerBuildInput *input)
{
    GraphIdStack *stack;
    VLE_path_container *vpc = NULL;
    graphid *graphid_array = NULL;

    Assert(input != NULL);
    stack = input->path_stack;

    if (gid_stack_size(stack) != 0)
    {
        ereport(ERROR,
                (errcode(ERRCODE_DATA_EXCEPTION),
                 errmsg("build_VLE_zero_container: stack is not empty")));
    }

    vpc = create_VLE_path_container(1);

    set_VLE_container_materializer_metadata(
        vpc, input->graph_oid, VLE_MATERIALIZER_OUTPUT_PATH,
        input->start_vertex_id, true, input->start_vertex_id, true);

    graphid_array = GET_GRAPHID_ARRAY_FROM_CONTAINER(vpc);
    graphid_array[0] = input->start_vertex_id;

    return vpc;
}

VLE_path_container *age_vle_build_container(
    const VLEContainerBuildInput *input,
    const VLEIteratorMaterialization *materialization)
{
    Assert(input != NULL);
    Assert(materialization != NULL);

    switch (materialization->container_kind)
    {
        case VLE_ITERATOR_CONTAINER_PATH:
            Assert(input->path_stack != NULL);
            Assert(input->path_stack->size > 0);
            return build_VLE_path_container(input);

        case VLE_ITERATOR_CONTAINER_REVERSED_PATH:
            Assert(input->path_stack != NULL);
            Assert(input->path_stack->size > 0);
            return build_reversed_VLE_path_container(input);

        case VLE_ITERATOR_CONTAINER_ZERO_PATH:
            return build_VLE_zero_container(input);

        case VLE_ITERATOR_CONTAINER_TERMINAL_VERTEX:
            Assert(input->path_stack != NULL);
            Assert(input->path_stack->size > 0);
            return build_VLE_terminal_container(input);

        case VLE_ITERATOR_CONTAINER_ZERO_TERMINAL_VERTEX:
            return build_VLE_terminal_zero_container(input);

        case VLE_ITERATOR_CONTAINER_NONE:
            break;
    }

    ereport(ERROR,
            (errcode(ERRCODE_INTERNAL_ERROR),
             errmsg("AGE VLE stream: materialization has no container output")));
    return NULL;
}
