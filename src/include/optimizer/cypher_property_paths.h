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

#ifndef AG_CYPHER_PROPERTY_PATHS_H
#define AG_CYPHER_PROPERTY_PATHS_H

#include "access/attnum.h"
#include "nodes/nodes.h"
#include "nodes/parsenodes.h"
#include "nodes/pathnodes.h"
#include "nodes/primnodes.h"
#include "parser/cypher_property_signature.h"

typedef struct CypherPropertyHandoffDescriptor
{
    CypherPropertyAccessSignature property_signature;
    Oid final_func_oid;
    Oid agg_func_oid;
    Node *index_expr;
} CypherPropertyHandoffDescriptor;

typedef struct CypherCachedPropertySlotDescriptor
{
    Node *container;
    List *keys;
    Oid value_type;
    Oid field_result_type;
    Oid final_func_oid;
    Oid agg_func_oid;
    Node *index_expr;
} CypherCachedPropertySlotDescriptor;

typedef struct CypherScalarFinalHandoff
{
    Node *scalar_expr;
    Oid value_type;
    Oid field_result_type;
    Oid final_func_oid;
    bool has_property_descriptor;
    CypherPropertyHandoffDescriptor property_descriptor;
    bool has_cached_property_slot;
    CypherCachedPropertySlotDescriptor cached_property_slot;
} CypherScalarFinalHandoff;

typedef struct CypherTypedCollectHandoff
{
    Aggref *aggref;
    Node *arg_expr;
    Oid value_type;
    Oid agg_func_oid;
    bool has_property_descriptor;
    CypherPropertyHandoffDescriptor property_descriptor;
    bool has_cached_property_slot;
    CypherCachedPropertySlotDescriptor cached_property_slot;
} CypherTypedCollectHandoff;

typedef struct CypherTypedCollectArgPlan
{
    CypherTypedCollectHandoff *handoff;
    Node *arg;
    Index sortgroupref;
} CypherTypedCollectArgPlan;

typedef struct CypherPropertyIndexHandoff
{
    Node *query_expr;
    Node *index_expr;
    bool has_property_descriptor;
    CypherPropertyHandoffDescriptor property_descriptor;
    bool has_cached_property_slot;
    CypherCachedPropertySlotDescriptor cached_property_slot;
} CypherPropertyIndexHandoff;

typedef struct CypherArrayAggPropertyHandoff
{
    Aggref *aggref;
    Node *properties;
    Node *key_path;
    Oid agg_func_oid;
    List *arg_exprs;
    List *arg_types;
} CypherArrayAggPropertyHandoff;

typedef struct CypherArrayAggPropertyArgPlan
{
    Node *arg;
    Index sortgroupref;
} CypherArrayAggPropertyArgPlan;

bool cypher_rewrite_collect_typed_scalar_expr(Node *node);
bool cypher_rewrite_collect_numeric_property_expr(Node *node);
bool cypher_rewrite_array_agg_property_expr(Node *node);
bool cypher_rewrite_property_access_aggregate_pathtarget(PathTarget *target);
bool cypher_find_matching_property_index_handoff(
    RelOptInfo *rel, Node *expr, CypherPropertyIndexHandoff *handoff);
bool cypher_find_matching_property_index_handoff_for_rte(
    RangeTblEntry *rte, Index rti, Node *expr,
    CypherPropertyIndexHandoff *handoff);
Node *cypher_make_property_index_handoff_expr(
    CypherPropertyIndexHandoff *handoff);
Node *cypher_replace_property_index_side(OpExpr *op, bool replace_left,
                                         Node *index_expr);
bool cypher_rewrite_property_equals_restrictions(
    PlannerInfo *root, RelOptInfo *rel, Index rti);
void cypher_canonicalize_property_index_predicates(RelOptInfo *rel);
void cypher_canonicalize_property_index_restrictions(RelOptInfo *rel);
Node *cypher_make_cached_property_slot_expr(
    const CypherCachedPropertySlotDescriptor *slot);
bool cypher_make_property_access_slot_descriptor(
    Node *expr, CypherCachedPropertySlotDescriptor *slot);
Node *cypher_make_property_path_slot_expr(Node *container, Node *path,
                                          Oid value_type,
                                          Oid field_result_type);
bool cypher_find_typed_collect_handoffs(
    PathTarget *target, bool require_distinct, List **handoffs);
bool cypher_find_multi_typed_collect_handoffs(PathTarget *target,
                                              List **handoffs);
bool cypher_find_typed_collect_handoff(
    PathTarget *target, bool require_distinct,
    CypherTypedCollectHandoff *handoff);
bool cypher_find_typed_distinct_collect_handoff(
    PathTarget *target, CypherTypedCollectHandoff *handoff);
List *cypher_build_typed_collect_arg_plans(PathTarget *target,
                                           List *handoffs);
bool cypher_add_typed_collect_arg_plans_to_target(PathTarget *target,
                                                  List *arg_plans);
bool cypher_add_aggregate_group_exprs_to_target(PathTarget *target,
                                                AggPath *agg_path);
PathTarget *cypher_build_typed_collect_agg_target(
    PlannerInfo *root, PathTarget *target, List *arg_plans);
bool cypher_find_array_agg_property_handoff(
    PathTarget *target, CypherArrayAggPropertyHandoff *handoff);
List *cypher_build_array_agg_property_arg_plans(
    PathTarget *target, CypherArrayAggPropertyHandoff *handoff);
bool cypher_add_array_agg_property_arg_plans_to_target(PathTarget *target,
                                                       List *arg_plans);
PathTarget *cypher_build_array_agg_property_target(
    PlannerInfo *root, PathTarget *target,
    CypherArrayAggPropertyHandoff *handoff, List *arg_plans);
bool cypher_extract_typed_property_sort_args(Node *node, Node **properties,
                                             Node **key);
Node *cypher_make_ctid_property_field_agtype_expr(Oid relid, Node *ctid,
                                                  AttrNumber properties_attno,
                                                  Node *key);
Node *cypher_make_id_property_field_agtype_expr(Oid relid, Node *id,
                                                AttrNumber properties_attno,
                                                Node *key);
Node *cypher_make_ctid_property_path_field_agtype_expr(
    Oid relid, Node *ctid, AttrNumber properties_attno, List *keys);
Node *cypher_make_id_property_path_field_agtype_expr(
    Oid relid, Node *id, AttrNumber properties_attno, List *keys);
bool cypher_extract_scalar_final_handoff(Node *node,
                                         CypherScalarFinalHandoff *handoff);
bool cypher_build_scalar_final_deferred_targets(
    PlannerInfo *root, List *processed_tlist,
    PathTarget **lower_target, PathTarget **final_target);
bool cypher_detect_simple_property_projection(
    PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel,
    List **slots, const char **target_source);
bool cypher_build_ordered_property_projection_targets(
    PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel,
    FinalPathExtraData *extra, PathTarget **lower_target,
    PathTarget **final_target);
void cypher_property_path_invalidate_oids(void);

#endif
