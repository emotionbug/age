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

#ifndef AG_CYPHER_PROPERTY_SIGNATURE_H
#define AG_CYPHER_PROPERTY_SIGNATURE_H

#include "nodes/pg_list.h"
#include "nodes/nodes.h"

typedef struct CypherPropertyAccessSignature
{
    Node *container;
    List *keys;
    Oid value_type;
    Oid field_result_type;
} CypherPropertyAccessSignature;

bool cypher_extract_property_access_signature(
    Node *node, CypherPropertyAccessSignature *signature);
bool cypher_equal_property_access_signature(Node *left, Node *right);
bool cypher_property_access_signature_matches(
    const CypherPropertyAccessSignature *signature, Node *expr);
bool cypher_extract_property_access_terminal_args(Node *node, Node **object,
                                                  Node **key);
Oid cypher_get_property_field_oid(Oid value_type, Oid field_result_type);
bool cypher_property_field_func_matches(Oid funcid, Oid value_type,
                                        Oid field_result_type);
/*
 * Optimizer facet of the shared property physical descriptor: the typed
 * collect aggregate, scalar->agtype conversion, and lower/final materialization
 * cost (weight / wire width) per physical type.  The parser owns the table; the
 * optimizer consumes only these accessors.
 */
Oid cypher_property_typed_collect_agg_oid(Oid value_type);
bool cypher_property_is_typed_collect_agg_oid(Oid agg_oid, Oid *value_type);
bool cypher_property_scalar_field_func_matches(Oid funcid, Oid result_type);
bool cypher_property_scalar_to_agtype_func_matches(Oid funcid, Oid value_type);
int cypher_property_final_materialization_weight(Oid field_result_type);
int cypher_property_slot_wire_width(Oid value_type);
void cypher_property_signature_invalidate_oids(void);

#endif
