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

typedef int (*AgeBindingFlatEnumeratorCountCallback)(
    void *callback_state, int factor_index);
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
