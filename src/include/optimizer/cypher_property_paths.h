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
} CypherScalarFinalHandoff;

typedef struct CypherTypedCollectHandoff
{
    Node *arg_expr;
    Oid value_type;
    Oid agg_func_oid;
    bool has_property_descriptor;
    CypherPropertyHandoffDescriptor property_descriptor;
} CypherTypedCollectHandoff;

typedef struct CypherPropertyIndexHandoff
{
    Node *query_expr;
    Node *index_expr;
    bool has_property_descriptor;
    CypherPropertyHandoffDescriptor property_descriptor;
} CypherPropertyIndexHandoff;

bool cypher_rewrite_collect_typed_scalar_expr(Node *node);
bool cypher_rewrite_collect_numeric_property_expr(Node *node);
bool cypher_rewrite_array_agg_property_expr(Node *node);
bool cypher_find_matching_property_index_handoff(
    RelOptInfo *rel, Node *expr, CypherPropertyIndexHandoff *handoff);
bool cypher_find_matching_property_index_handoff_for_rte(
    RangeTblEntry *rte, Index rti, Node *expr,
    CypherPropertyIndexHandoff *handoff);
bool cypher_make_cached_property_slot_descriptor(
    const CypherPropertyHandoffDescriptor *handoff,
    CypherCachedPropertySlotDescriptor *slot);
Node *cypher_make_cached_property_slot_expr(
    const CypherCachedPropertySlotDescriptor *slot);
bool cypher_find_typed_distinct_collect_handoff(
    PathTarget *target, CypherTypedCollectHandoff *handoff);
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
Expr *cypher_add_or_get_lower_scalar_handoff(
    PathTarget *target, CypherScalarFinalHandoff *handoff,
    Index ressortgroupref);
void cypher_property_path_invalidate_oids(void);

#endif
