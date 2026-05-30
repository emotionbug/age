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

#ifndef AG_CYPHER_PATHS_H
#define AG_CYPHER_PATHS_H

#include "nodes/nodes.h"
#include "nodes/pathnodes.h"
#include "nodes/pg_list.h"
#include "utils/relcache.h"

typedef struct CypherAdjacencyMatchCandidate
{
    Oid edge_label_oid;
    Oid index_oid;
    char *edge_alias;
    char *bound_endpoint_alias;
    Node *bound_endpoint_expr;
    char *candidate_reason;
    Index bound_endpoint_rti;
    Relids required_outer;
    bool outgoing;
    bool has_edge_variable_projection;
    bool has_edge_property_predicate;
    bool has_right_label_constraint;
    bool has_right_property_predicate;
    AttrNumber endpoint_attno;
} CypherAdjacencyMatchCandidate;

void set_rel_pathlist_init(void);
void set_rel_pathlist_fini(void);
void cypher_clear_adjacency_match_candidates(void);
void cypher_register_adjacency_match_candidate(Oid edge_label_oid,
                                               Oid index_oid,
                                               const char *edge_alias,
                                               const char *bound_endpoint_alias,
                                               Node *bound_endpoint_expr,
                                               const char *candidate_reason,
                                               bool outgoing,
                                               bool has_edge_variable_projection,
                                               bool has_edge_property_predicate,
                                               bool has_right_label_constraint,
                                               bool has_right_property_predicate,
                                               AttrNumber endpoint_attno);

#endif
