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

#ifndef AGE_ADJACENCY_H
#define AGE_ADJACENCY_H

#include "postgres.h"

#include "storage/itemptr.h"
#include "utils/graphid.h"
#include "utils/snapshot.h"

typedef struct AgeAdjacencyPayload
{
    ItemPointerData heap_tid;
    graphid edge_id;
    graphid next_vertex_id;
    Datum properties;
    bool properties_isnull;
} AgeAdjacencyPayload;

typedef bool (*AgeAdjacencyPayloadCallback) (const AgeAdjacencyPayload *payload,
                                             void *callback_state);

extern int64 age_adjacency_foreach_visible_payload(Oid index_oid,
                                                   graphid key,
                                                   Snapshot snapshot,
                                                   AgeAdjacencyPayloadCallback callback,
                                                   void *callback_state);

#endif
