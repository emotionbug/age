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

#ifndef AG_CYPHER_FACTORIZED_BINDING_H
#define AG_CYPHER_FACTORIZED_BINDING_H

#include "access/htup.h"
#include "executor/tuptable.h"
#include "nodes/bitmapset.h"
#include "utils/graphid.h"
#include "utils/memutils.h"

typedef struct AgeBindingSourceRow
{
    graphid key;
    MinimalTuple tuple;
} AgeBindingSourceRow;

typedef struct AgeSourceBag
{
    graphid key;
    int first_row;
    int row_count;
} AgeSourceBag;

typedef struct AgeEdgeBag
{
    int first_payload;
    int payload_count;
    int logical_count;
} AgeEdgeBag;

typedef struct AgeBindingMultiplicity
{
    int64 count;
    bool overflowed;
} AgeBindingMultiplicity;

typedef struct AgeBindingKeyNode
{
    graphid key;
    bool key_valid;
} AgeBindingKeyNode;

typedef struct AgeBindingSourceBag
{
    graphid key;
    int first_row;
    int row_count;
    void *payload;
    Size payload_bytes;
} AgeBindingSourceBag;

typedef struct AgeBindingEdgeBag
{
    int source_bag_index;
    int first_payload;
    int payload_count;
    int logical_count;
    void *payload;
    Size payload_bytes;
} AgeBindingEdgeBag;

struct AgeBindingNode;

typedef struct AgeChildBinding
{
    struct AgeBindingNode *node;
    AgeBindingMultiplicity multiplicity;
} AgeChildBinding;

typedef struct AgeBindingNode
{
    AgeBindingKeyNode key;
    AgeBindingSourceBag *source_bags;
    int source_bag_count;
    int source_bag_capacity;
    AgeBindingEdgeBag *edge_bags;
    int edge_bag_count;
    int edge_bag_capacity;
    AgeChildBinding *children;
    int child_count;
    int child_capacity;
    AgeBindingMultiplicity multiplicity;
    int64 candidate_flat_rows;
    int64 materialized_flat_rows;
    int64 flat_rows_avoided;
    int64 memory_bytes;
    MemoryContext context;
} AgeBindingNode;

typedef enum AgeBindingNodeFlatFactorKind
{
    AGE_BINDING_NODE_FLAT_SOURCE_BAG = 0,
    AGE_BINDING_NODE_FLAT_EDGE_BAG,
    AGE_BINDING_NODE_FLAT_CHILD
} AgeBindingNodeFlatFactorKind;

typedef struct AgeBindingNodeFlatFactor
{
    AgeBindingNodeFlatFactorKind kind;
    int factor_index;
    int logical_count;
} AgeBindingNodeFlatFactor;

typedef struct AgeBindingNodeFlatEnumerator
{
    const AgeBindingNode *node;
    AgeBindingNodeFlatFactor *factors;
    int factor_count;
    int factor_capacity;
    int *indexes;
    MemoryContext context;
    bool active;
    bool started;
    int64 candidate_flat_rows;
    int64 emitted_rows;
    int64 starts;
    int64 steps;
    int64 memory_bytes;
} AgeBindingNodeFlatEnumerator;

typedef int (*AgeBindingFlatEnumeratorCountCallback)(
    void *callback_state, int factor_index);
typedef bool (*AgeBindingFlatEnumeratorAcceptCallback)(
    void *callback_state, int factor_index, int tuple_index);
typedef bool (*AgeBindingFlatEnumeratorEdgeIdCallback)(
    void *callback_state, int factor_index, int tuple_index,
    graphid *edge_id);

typedef struct AgeBindingFlatEnumerator
{
    int factor_count;
    int *logical_counts;
    int *indexes;
    int *enumeration_order;
    graphid *selected_edge_ids;
    bool *selected_edge_id_valid;
    Bitmapset **uniqueness_groups;
    AgeBindingFlatEnumeratorAcceptCallback accept_callback;
    AgeBindingFlatEnumeratorEdgeIdCallback edge_id_callback;
    void *callback_state;
    bool active;
    bool started;
    int64 starts;
    int64 steps;
    int64 uniqueness_rejects;
} AgeBindingFlatEnumerator;

extern void age_binding_add_counter(int64 *counter, int64 increment);
extern void age_binding_add_byte_counter(int64 *counter, Size increment);
extern AgeBindingMultiplicity age_binding_multiplicity_zero(void);
extern AgeBindingMultiplicity age_binding_multiplicity_one(void);
extern AgeBindingMultiplicity age_binding_multiplicity_from_count(int64 count);
extern AgeBindingMultiplicity age_binding_multiplicity_product(
    AgeBindingMultiplicity left, AgeBindingMultiplicity right);
extern AgeBindingMultiplicity age_binding_multiplicity_product_count(
    AgeBindingMultiplicity multiplicity, int64 count);
extern int64 age_binding_multiplicity_count(
    AgeBindingMultiplicity multiplicity);
extern bool age_binding_multiplicity_overflowed(
    AgeBindingMultiplicity multiplicity);
extern bool age_binding_multiplicity_is_zero(
    AgeBindingMultiplicity multiplicity);
extern AgeBindingNode *age_binding_create_node(
    MemoryContext context, graphid key, bool key_valid);
extern void age_binding_reset_node(AgeBindingNode *node);
extern void age_binding_free_node(AgeBindingNode *node);
extern void age_binding_node_account_memory(
    AgeBindingNode *node, Size bytes);
extern AgeBindingSourceBag *age_binding_node_add_source_bag(
    AgeBindingNode *node, graphid key, int first_row, int row_count,
    void *payload, Size payload_bytes);
extern AgeBindingEdgeBag *age_binding_node_add_edge_bag(
    AgeBindingNode *node, int source_bag_index, int first_payload,
    int payload_count, int logical_count, void *payload, Size payload_bytes);
extern AgeChildBinding *age_binding_node_add_child(
    AgeBindingNode *node, AgeBindingNode *child);
extern AgeBindingMultiplicity age_binding_node_multiplicity(
    AgeBindingNode *node);
extern int64 age_binding_node_flat_cardinality(AgeBindingNode *node);
extern void age_binding_node_note_flat_enumeration(
    AgeBindingNode *node, int64 materialized_rows);
extern int64 age_binding_node_candidate_flat_rows(
    const AgeBindingNode *node);
extern int64 age_binding_node_materialized_flat_rows(
    const AgeBindingNode *node);
extern int64 age_binding_node_flat_rows_avoided(
    const AgeBindingNode *node);
extern int64 age_binding_node_memory_bytes(const AgeBindingNode *node);
extern void age_binding_init_node_flat_enumerator(
    AgeBindingNodeFlatEnumerator *enumerator, MemoryContext context);
extern void age_binding_reset_node_flat_enumerator(
    AgeBindingNodeFlatEnumerator *enumerator);
extern void age_binding_cleanup_node_flat_enumerator(
    AgeBindingNodeFlatEnumerator *enumerator);
extern int64 age_binding_begin_node_flat_enumerator(
    AgeBindingNodeFlatEnumerator *enumerator, const AgeBindingNode *node);
extern bool age_binding_node_flat_enumerator_next(
    AgeBindingNodeFlatEnumerator *enumerator);
extern int age_binding_node_flat_enumerator_index(
    const AgeBindingNodeFlatEnumerator *enumerator,
    AgeBindingNodeFlatFactorKind kind, int factor_index);
extern int64 age_binding_node_flat_enumerator_candidate_rows(
    const AgeBindingNodeFlatEnumerator *enumerator);
extern int64 age_binding_node_flat_enumerator_emitted_rows(
    const AgeBindingNodeFlatEnumerator *enumerator);
extern int64 age_binding_node_flat_enumerator_starts(
    const AgeBindingNodeFlatEnumerator *enumerator);
extern int64 age_binding_node_flat_enumerator_steps(
    const AgeBindingNodeFlatEnumerator *enumerator);
extern int64 age_binding_node_flat_enumerator_memory_bytes(
    const AgeBindingNodeFlatEnumerator *enumerator);
extern void age_binding_append_source_row(
    AgeBindingSourceRow **source_rows, int *source_row_count,
    int *source_row_capacity, int64 *source_bag_bytes, MemoryContext context,
    graphid key, TupleTableSlot *slot);
extern AgeSourceBag *age_binding_build_source_bags(
    AgeBindingSourceRow *source_rows, int source_row_count,
    int *source_bag_count, int64 *source_bag_bytes, MemoryContext context);
extern AgeSourceBag *age_binding_get_source_bag(
    AgeSourceBag *source_bags, int source_bag_count, int source_bag_index);
extern int age_binding_find_source_bag_index(
    AgeSourceBag *source_bags, int source_bag_count,
    const AgeSourceBag *source_bag);
extern int age_binding_source_bag_row_count(const AgeSourceBag *source_bag);
extern int age_binding_add_source_multiplicity(
    int *logical_count, const AgeSourceBag *source_bag);
extern int age_binding_edge_bag_record_payload(
    AgeEdgeBag *edge_bag, const AgeSourceBag *source_bag);
extern void age_binding_init_flat_enumerator(
    AgeBindingFlatEnumerator *enumerator, int factor_count,
    MemoryContext context);
extern void age_binding_reset_flat_enumerator(
    AgeBindingFlatEnumerator *enumerator);
extern int64 age_binding_begin_flat_enumerator(
    AgeBindingFlatEnumerator *enumerator,
    AgeBindingFlatEnumeratorCountCallback count_callback,
    AgeBindingFlatEnumeratorAcceptCallback accept_callback,
    AgeBindingFlatEnumeratorEdgeIdCallback edge_id_callback,
    void *callback_state, Bitmapset **uniqueness_groups);
extern bool age_binding_flat_enumerator_next(
    AgeBindingFlatEnumerator *enumerator);
extern int age_binding_flat_enumerator_index(
    const AgeBindingFlatEnumerator *enumerator, int factor_index);
extern int64 age_binding_flat_enumerator_starts(
    const AgeBindingFlatEnumerator *enumerator);
extern int64 age_binding_flat_enumerator_steps(
    const AgeBindingFlatEnumerator *enumerator);
extern int64 age_binding_flat_enumerator_uniqueness_rejects(
    const AgeBindingFlatEnumerator *enumerator);

#endif
