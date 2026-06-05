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

#ifndef AGE_VLE_MATERIALIZER_CACHE_H
#define AGE_VLE_MATERIALIZER_CACHE_H

#include "postgres.h"

#include "fmgr.h"
#include "utils/age_global_graph.h"
#include "utils/agtype.h"
#include "utils/hsearch.h"

typedef struct VLEMaterializerObjectCache VLEMaterializerObjectCache;
typedef struct VLEMaterializerHandoff VLEMaterializerHandoff;

typedef agtype *(*VLEMaterializerBuildObject) (
    const VLEMaterializerHandoff *handoff, graphid id);

typedef enum VLEMaterializerOutputRequirement
{
    VLE_MATERIALIZER_OUTPUT_UNKNOWN = 0,
    VLE_MATERIALIZER_OUTPUT_PATH,
    VLE_MATERIALIZER_OUTPUT_NODE_LIST,
    VLE_MATERIALIZER_OUTPUT_EDGE_LIST,
    VLE_MATERIALIZER_OUTPUT_TYPED_VERTEX,
    VLE_MATERIALIZER_OUTPUT_TYPED_EDGE
} VLEMaterializerOutputRequirement;

struct VLEMaterializerHandoff
{
    GRAPH_global_context *ggctx;
    HTAB *relation_cache;
    VLEMaterializerBuildObject build_object;
    VLEMaterializerOutputRequirement output_requirement;
    graphid traversal_root_id;
    bool traversal_root_valid;
    graphid candidate_vertex_id;
    bool candidate_vertex_valid;
};

extern VLEMaterializerObjectCache *age_vle_materializer_object_cache_get(
    FunctionCallInfo fcinfo, Oid graph_oid);
extern void age_vle_materializer_cache_prefetch_vertices(
    const VLEMaterializerHandoff *handoff, const graphid *vertex_ids,
    int64 nvertex_ids);
extern agtype *age_vle_materializer_cache_get_vertex_object(
    VLEMaterializerObjectCache *object_cache,
    const VLEMaterializerHandoff *handoff, graphid vertex_id);
extern agtype *age_vle_materializer_cache_get_edge_object(
    VLEMaterializerObjectCache *object_cache,
    const VLEMaterializerHandoff *handoff, graphid edge_id);
extern agtype *age_vle_materializer_cache_get_typed_vertex(
    VLEMaterializerObjectCache *object_cache,
    const VLEMaterializerHandoff *handoff, graphid vertex_id);
extern agtype *age_vle_materializer_cache_get_typed_edge(
    VLEMaterializerObjectCache *object_cache,
    const VLEMaterializerHandoff *handoff, graphid edge_id);

#endif
