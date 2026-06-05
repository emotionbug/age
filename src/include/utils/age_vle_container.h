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

#ifndef AGE_VLE_CONTAINER_H
#define AGE_VLE_CONTAINER_H

#include "postgres.h"

#include "utils/age_graphid_ds.h"
#include "utils/age_vle.h"
#include "utils/age_vle_iterator_materialization.h"
#include "utils/age_vle_materializer_cache.h"

/*
 * Container to hold the graphid array that contains one valid path. This
 * structure will allow it to be easily passed as an AGTYPE pointer. The
 * structure is set up to contains a BINARY container that can be accessed by
 * functions that need to process the path.
 */
struct VLE_path_container
{
    char vl_len_[4]; /* Do not touch this field! */
    uint32 header;
    uint32 graph_oid;
    int64 graphid_array_size;
    int64 container_size_bytes;
    graphid traversal_root_id;
    graphid candidate_vertex_id;
    VLEMaterializerOutputRequirement output_requirement;
    bool traversal_root_valid;
    bool candidate_vertex_valid;
    graphid graphid_array_data;
};

#define GET_GRAPHID_ARRAY_FROM_CONTAINER(vpc) \
            (graphid *) (&(vpc)->graphid_array_data)

typedef struct VLEContainerBuildInput
{
    Oid graph_oid;
    graphid start_vertex_id;
    bool reverse_output_path;
    GraphIdStack *path_stack;
    GraphIdStack *path_vertex_stack;
} VLEContainerBuildInput;

extern VLE_path_container *age_vle_build_container(
    const VLEContainerBuildInput *input,
    const VLEIteratorMaterialization *materialization);

#endif
