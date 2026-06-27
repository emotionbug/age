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

#include "executor/cypher_factorized_binding.h"
#include "executor/executor.h"

void
age_binding_add_counter(int64 *counter, int64 increment)
{
    if (increment <= 0)
        return;
    if (*counter > PG_INT64_MAX - increment)
        *counter = PG_INT64_MAX;
    else
        *counter += increment;
}

void
age_binding_add_byte_counter(int64 *counter, Size increment)
{
    if (increment == 0)
        return;
    if (increment > (Size)PG_INT64_MAX)
        *counter = PG_INT64_MAX;
    else
        age_binding_add_counter(counter, (int64)increment);
}

static void *
age_binding_enlarge_array(void *array, int *capacity, int required,
                          Size element_size, MemoryContext context,
                          int64 *memory_bytes, const char *error_detail)
{
    void *new_array;
    MemoryContext oldcontext;
    int old_capacity;
    int new_capacity;

    if (capacity == NULL || required <= 0 || element_size == 0 ||
        context == NULL || memory_bytes == NULL)
    {
        elog(ERROR, "invalid AGE factorized binding array state");
    }
    if (*capacity >= required)
        return array;

    old_capacity = *capacity;
    new_capacity = old_capacity == 0 ? 4 : old_capacity * 2;
    while (new_capacity < required)
    {
        if (new_capacity > PG_INT32_MAX / 2)
            new_capacity = required;
        else
            new_capacity *= 2;
    }

    if (new_capacity <= old_capacity ||
        (Size)new_capacity > MaxAllocSize / element_size)
    {
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("%s", error_detail)));
    }

    oldcontext = MemoryContextSwitchTo(context);
    if (array == NULL)
        new_array = palloc0(element_size * new_capacity);
    else
    {
        new_array = repalloc(array, element_size * new_capacity);
        memset((char *)new_array + element_size * old_capacity, 0,
               element_size * (new_capacity - old_capacity));
    }
    MemoryContextSwitchTo(oldcontext);

    *capacity = new_capacity;
    age_binding_add_byte_counter(
        memory_bytes, element_size * (new_capacity - old_capacity));
    return new_array;
}

AgeBindingMultiplicity
age_binding_multiplicity_zero(void)
{
    AgeBindingMultiplicity multiplicity;

    multiplicity.count = 0;
    multiplicity.overflowed = false;
    return multiplicity;
}

AgeBindingMultiplicity
age_binding_multiplicity_one(void)
{
    AgeBindingMultiplicity multiplicity;

    multiplicity.count = 1;
    multiplicity.overflowed = false;
    return multiplicity;
}

AgeBindingMultiplicity
age_binding_multiplicity_from_count(int64 count)
{
    AgeBindingMultiplicity multiplicity;

    if (count < 0)
        elog(ERROR, "invalid AGE factorized binding multiplicity");
    multiplicity.count = count;
    multiplicity.overflowed = false;
    return multiplicity;
}

AgeBindingMultiplicity
age_binding_multiplicity_product(AgeBindingMultiplicity left,
                                 AgeBindingMultiplicity right)
{
    AgeBindingMultiplicity product;

    product.overflowed = left.overflowed || right.overflowed;
    if (left.count < 0 || right.count < 0)
        elog(ERROR, "invalid AGE factorized binding multiplicity");
    if (left.count == 0 || right.count == 0)
    {
        product.count = 0;
        return product;
    }
    if (left.count > PG_INT64_MAX / right.count)
    {
        product.count = PG_INT64_MAX;
        product.overflowed = true;
        return product;
    }
    product.count = left.count * right.count;
    return product;
}

AgeBindingMultiplicity
age_binding_multiplicity_product_count(
    AgeBindingMultiplicity multiplicity, int64 count)
{
    return age_binding_multiplicity_product(
        multiplicity, age_binding_multiplicity_from_count(count));
}

int64
age_binding_multiplicity_count(AgeBindingMultiplicity multiplicity)
{
    if (multiplicity.count < 0)
        elog(ERROR, "invalid AGE factorized binding multiplicity");
    return multiplicity.count;
}

bool
age_binding_multiplicity_overflowed(AgeBindingMultiplicity multiplicity)
{
    return multiplicity.overflowed;
}

bool
age_binding_multiplicity_is_zero(AgeBindingMultiplicity multiplicity)
{
    return age_binding_multiplicity_count(multiplicity) == 0;
}

AgeBindingNode *
age_binding_create_node(MemoryContext context, graphid key, bool key_valid)
{
    AgeBindingNode *node;
    MemoryContext oldcontext;

    if (context == NULL)
        elog(ERROR, "invalid AGE factorized binding node context");

    oldcontext = MemoryContextSwitchTo(context);
    node = palloc0(sizeof(AgeBindingNode));
    MemoryContextSwitchTo(oldcontext);

    node->key.key = key;
    node->key.key_valid = key_valid;
    node->context = context;
    node->multiplicity = age_binding_multiplicity_one();
    node->candidate_flat_rows = 1;
    node->memory_bytes = sizeof(AgeBindingNode);
    return node;
}

static int64
age_binding_node_base_memory(const AgeBindingNode *node)
{
    int64 memory_bytes = sizeof(AgeBindingNode);

    age_binding_add_byte_counter(
        &memory_bytes, sizeof(AgeBindingSourceBag) *
        (Size)node->source_bag_capacity);
    age_binding_add_byte_counter(
        &memory_bytes, sizeof(AgeBindingEdgeBag) *
        (Size)node->edge_bag_capacity);
    age_binding_add_byte_counter(
        &memory_bytes, sizeof(AgeChildBinding) *
        (Size)node->child_capacity);
    return memory_bytes;
}

void
age_binding_reset_node(AgeBindingNode *node)
{
    if (node == NULL)
        elog(ERROR, "invalid AGE factorized binding node");

    if (node->source_bags != NULL && node->source_bag_capacity > 0)
    {
        memset(node->source_bags, 0,
               sizeof(AgeBindingSourceBag) * node->source_bag_capacity);
    }
    if (node->edge_bags != NULL && node->edge_bag_capacity > 0)
    {
        memset(node->edge_bags, 0,
               sizeof(AgeBindingEdgeBag) * node->edge_bag_capacity);
    }
    if (node->children != NULL && node->child_capacity > 0)
    {
        memset(node->children, 0,
               sizeof(AgeChildBinding) * node->child_capacity);
    }
    node->source_bag_count = 0;
    node->edge_bag_count = 0;
    node->child_count = 0;
    node->multiplicity = age_binding_multiplicity_one();
    node->candidate_flat_rows = 1;
    node->materialized_flat_rows = 0;
    node->flat_rows_avoided = 0;
    node->memory_bytes = age_binding_node_base_memory(node);
}

void
age_binding_free_node(AgeBindingNode *node)
{
    if (node == NULL)
        return;
    if (node->source_bags != NULL)
        pfree(node->source_bags);
    if (node->edge_bags != NULL)
        pfree(node->edge_bags);
    if (node->children != NULL)
        pfree(node->children);
    pfree(node);
}

void
age_binding_node_account_memory(AgeBindingNode *node, Size bytes)
{
    if (node == NULL)
        elog(ERROR, "invalid AGE factorized binding node");
    age_binding_add_byte_counter(&node->memory_bytes, bytes);
}

AgeBindingSourceBag *
age_binding_node_add_source_bag(AgeBindingNode *node, graphid key,
                                int first_row, int row_count, void *payload,
                                Size payload_bytes)
{
    AgeBindingSourceBag *source_bag;

    if (node == NULL || first_row < 0 || row_count <= 0)
        elog(ERROR, "invalid AGE factorized binding source bag");

    node->source_bags = age_binding_enlarge_array(
        node->source_bags, &node->source_bag_capacity,
        node->source_bag_count + 1, sizeof(AgeBindingSourceBag),
        node->context, &node->memory_bytes,
        "AGE factorized binding source bags are too large");

    source_bag = &node->source_bags[node->source_bag_count++];
    source_bag->key = key;
    source_bag->first_row = first_row;
    source_bag->row_count = row_count;
    source_bag->payload = payload;
    source_bag->payload_bytes = payload_bytes;
    age_binding_node_account_memory(node, payload_bytes);
    return source_bag;
}

AgeBindingEdgeBag *
age_binding_node_add_edge_bag(AgeBindingNode *node, int source_bag_index,
                              int first_payload, int payload_count,
                              int logical_count, void *payload,
                              Size payload_bytes)
{
    AgeBindingEdgeBag *edge_bag;

    if (node == NULL || source_bag_index < -1 || first_payload < 0 ||
        payload_count <= 0 || logical_count <= 0)
    {
        elog(ERROR, "invalid AGE factorized binding edge bag");
    }
    if (source_bag_index >= node->source_bag_count)
        elog(ERROR, "invalid AGE factorized binding edge source bag");

    node->edge_bags = age_binding_enlarge_array(
        node->edge_bags, &node->edge_bag_capacity,
        node->edge_bag_count + 1, sizeof(AgeBindingEdgeBag),
        node->context, &node->memory_bytes,
        "AGE factorized binding edge bags are too large");

    edge_bag = &node->edge_bags[node->edge_bag_count++];
    edge_bag->source_bag_index = source_bag_index;
    edge_bag->first_payload = first_payload;
    edge_bag->payload_count = payload_count;
    edge_bag->logical_count = logical_count;
    edge_bag->payload = payload;
    edge_bag->payload_bytes = payload_bytes;
    age_binding_node_account_memory(node, payload_bytes);
    return edge_bag;
}

AgeChildBinding *
age_binding_node_add_child(AgeBindingNode *node, AgeBindingNode *child)
{
    AgeChildBinding *child_binding;

    if (node == NULL || child == NULL)
        elog(ERROR, "invalid AGE factorized child binding");

    node->children = age_binding_enlarge_array(
        node->children, &node->child_capacity, node->child_count + 1,
        sizeof(AgeChildBinding), node->context, &node->memory_bytes,
        "AGE factorized child bindings are too large");

    child_binding = &node->children[node->child_count++];
    child_binding->node = child;
    child_binding->multiplicity = age_binding_node_multiplicity(child);
    return child_binding;
}

AgeBindingMultiplicity
age_binding_node_multiplicity(AgeBindingNode *node)
{
    AgeBindingMultiplicity multiplicity;
    int index;

    if (node == NULL)
        elog(ERROR, "invalid AGE factorized binding node");

    multiplicity = age_binding_multiplicity_one();
    for (index = 0; index < node->source_bag_count; index++)
    {
        multiplicity = age_binding_multiplicity_product_count(
            multiplicity, node->source_bags[index].row_count);
    }
    for (index = 0; index < node->edge_bag_count; index++)
    {
        multiplicity = age_binding_multiplicity_product_count(
            multiplicity, node->edge_bags[index].logical_count);
    }
    for (index = 0; index < node->child_count; index++)
    {
        AgeChildBinding *child = &node->children[index];

        child->multiplicity = age_binding_node_multiplicity(child->node);
        multiplicity = age_binding_multiplicity_product(
            multiplicity, child->multiplicity);
    }
    node->multiplicity = multiplicity;
    node->candidate_flat_rows = multiplicity.count;
    return multiplicity;
}

int64
age_binding_node_flat_cardinality(AgeBindingNode *node)
{
    return age_binding_multiplicity_count(
        age_binding_node_multiplicity(node));
}

void
age_binding_node_note_flat_enumeration(AgeBindingNode *node,
                                       int64 materialized_rows)
{
    int64 candidate_rows;

    if (node == NULL || materialized_rows < 0)
        elog(ERROR, "invalid AGE factorized binding enumeration count");

    candidate_rows = age_binding_node_flat_cardinality(node);
    age_binding_add_counter(&node->materialized_flat_rows,
                            materialized_rows);
    node->flat_rows_avoided =
        candidate_rows > node->materialized_flat_rows ?
        candidate_rows - node->materialized_flat_rows : 0;
}

int64
age_binding_node_candidate_flat_rows(const AgeBindingNode *node)
{
    if (node == NULL)
        elog(ERROR, "invalid AGE factorized binding node");
    return node->candidate_flat_rows;
}

int64
age_binding_node_materialized_flat_rows(const AgeBindingNode *node)
{
    if (node == NULL)
        elog(ERROR, "invalid AGE factorized binding node");
    return node->materialized_flat_rows;
}

int64
age_binding_node_flat_rows_avoided(const AgeBindingNode *node)
{
    if (node == NULL)
        elog(ERROR, "invalid AGE factorized binding node");
    return node->flat_rows_avoided;
}

int64
age_binding_node_memory_bytes(const AgeBindingNode *node)
{
    if (node == NULL)
        elog(ERROR, "invalid AGE factorized binding node");
    return node->memory_bytes;
}

void
age_binding_init_node_flat_enumerator(
    AgeBindingNodeFlatEnumerator *enumerator, MemoryContext context)
{
    if (enumerator == NULL || context == NULL)
        elog(ERROR, "invalid AGE factorized binding node enumerator");
    memset(enumerator, 0, sizeof(*enumerator));
    enumerator->context = context;
}

void
age_binding_reset_node_flat_enumerator(
    AgeBindingNodeFlatEnumerator *enumerator)
{
    int index;

    if (enumerator == NULL)
        elog(ERROR, "invalid AGE factorized binding node enumerator");

    enumerator->node = NULL;
    for (index = 0; index < enumerator->factor_capacity; index++)
    {
        if (enumerator->factors != NULL)
            memset(&enumerator->factors[index], 0,
                   sizeof(AgeBindingNodeFlatFactor));
        if (enumerator->indexes != NULL)
            enumerator->indexes[index] = -1;
    }
    enumerator->factor_count = 0;
    enumerator->active = false;
    enumerator->started = false;
    enumerator->candidate_flat_rows = 0;
    enumerator->emitted_rows = 0;
}

void
age_binding_cleanup_node_flat_enumerator(
    AgeBindingNodeFlatEnumerator *enumerator)
{
    if (enumerator == NULL)
        return;
    if (enumerator->factors != NULL)
        pfree(enumerator->factors);
    if (enumerator->indexes != NULL)
        pfree(enumerator->indexes);
    memset(enumerator, 0, sizeof(*enumerator));
}

static void
age_binding_node_flat_enumerator_reserve(
    AgeBindingNodeFlatEnumerator *enumerator, int required)
{
    AgeBindingNodeFlatFactor *new_factors;
    int *new_indexes;
    MemoryContext oldcontext;
    int old_capacity;
    int new_capacity;

    if (required <= enumerator->factor_capacity)
        return;

    old_capacity = enumerator->factor_capacity;
    new_capacity = old_capacity == 0 ? 4 : old_capacity * 2;
    while (new_capacity < required)
    {
        if (new_capacity > PG_INT32_MAX / 2)
            new_capacity = required;
        else
            new_capacity *= 2;
    }
    if (new_capacity <= old_capacity ||
        (Size)new_capacity > MaxAllocSize /
            sizeof(AgeBindingNodeFlatFactor) ||
        (Size)new_capacity > MaxAllocSize / sizeof(int))
    {
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("AGE factorized binding node enumerator is too large")));
    }

    oldcontext = MemoryContextSwitchTo(enumerator->context);
    if (enumerator->factors == NULL)
        new_factors = palloc0(sizeof(AgeBindingNodeFlatFactor) *
                              new_capacity);
    else
    {
        new_factors = repalloc(enumerator->factors,
                               sizeof(AgeBindingNodeFlatFactor) *
                               new_capacity);
        memset(&new_factors[old_capacity], 0,
               sizeof(AgeBindingNodeFlatFactor) *
               (new_capacity - old_capacity));
    }
    if (enumerator->indexes == NULL)
        new_indexes = palloc(sizeof(int) * new_capacity);
    else
        new_indexes = repalloc(enumerator->indexes,
                               sizeof(int) * new_capacity);
    MemoryContextSwitchTo(oldcontext);

    memset(&new_indexes[old_capacity], -1,
           sizeof(int) * (new_capacity - old_capacity));
    enumerator->factors = new_factors;
    enumerator->indexes = new_indexes;
    enumerator->factor_capacity = new_capacity;
    age_binding_add_byte_counter(
        &enumerator->memory_bytes,
        sizeof(AgeBindingNodeFlatFactor) *
        (Size)(new_capacity - old_capacity));
    age_binding_add_byte_counter(
        &enumerator->memory_bytes,
        sizeof(int) * (Size)(new_capacity - old_capacity));
}

static void
age_binding_node_flat_enumerator_add_factor(
    AgeBindingNodeFlatEnumerator *enumerator,
    AgeBindingNodeFlatFactorKind kind, int factor_index, int logical_count)
{
    AgeBindingNodeFlatFactor *factor;

    if (logical_count < 0)
        elog(ERROR, "invalid AGE factorized binding node factor");

    age_binding_node_flat_enumerator_reserve(
        enumerator, enumerator->factor_count + 1);
    factor = &enumerator->factors[enumerator->factor_count];
    factor->kind = kind;
    factor->factor_index = factor_index;
    factor->logical_count = logical_count;
    enumerator->indexes[enumerator->factor_count] = -1;
    enumerator->factor_count++;
}

int64
age_binding_begin_node_flat_enumerator(
    AgeBindingNodeFlatEnumerator *enumerator, const AgeBindingNode *node)
{
    AgeBindingMultiplicity candidate;
    AgeBindingNode *mutable_node;
    int index;

    if (enumerator == NULL || node == NULL)
        elog(ERROR, "invalid AGE factorized binding node enumerator");

    mutable_node = (AgeBindingNode *)node;
    candidate = age_binding_node_multiplicity(mutable_node);

    age_binding_reset_node_flat_enumerator(enumerator);
    enumerator->node = node;
    enumerator->candidate_flat_rows = candidate.count;
    enumerator->active = candidate.count > 0;
    age_binding_add_counter(&enumerator->starts, 1);

    for (index = 0; index < node->source_bag_count; index++)
    {
        age_binding_node_flat_enumerator_add_factor(
            enumerator, AGE_BINDING_NODE_FLAT_SOURCE_BAG, index,
            node->source_bags[index].row_count);
    }
    for (index = 0; index < node->edge_bag_count; index++)
    {
        age_binding_node_flat_enumerator_add_factor(
            enumerator, AGE_BINDING_NODE_FLAT_EDGE_BAG, index,
            node->edge_bags[index].logical_count);
    }
    for (index = 0; index < node->child_count; index++)
    {
        age_binding_node_flat_enumerator_add_factor(
            enumerator, AGE_BINDING_NODE_FLAT_CHILD, index,
            node->children[index].multiplicity.count);
    }
    return candidate.count;
}

bool
age_binding_node_flat_enumerator_next(
    AgeBindingNodeFlatEnumerator *enumerator)
{
    int depth;

    if (enumerator == NULL)
        elog(ERROR, "invalid AGE factorized binding node enumerator");
    if (!enumerator->active)
        return false;
    if (enumerator->factor_count == 0)
    {
        if (enumerator->started)
        {
            enumerator->active = false;
            enumerator->started = false;
            return false;
        }
        enumerator->started = true;
        age_binding_add_counter(&enumerator->emitted_rows, 1);
        return true;
    }

    depth = enumerator->started ? enumerator->factor_count - 1 : 0;
    enumerator->started = true;

    while (depth >= 0)
    {
        int tuple_index = enumerator->indexes[depth] + 1;
        int tuple_count = enumerator->factors[depth].logical_count;

        while (tuple_index < tuple_count)
        {
            enumerator->indexes[depth] = tuple_index;
            age_binding_add_counter(&enumerator->steps, 1);

            if (depth == enumerator->factor_count - 1)
            {
                age_binding_add_counter(&enumerator->emitted_rows, 1);
                return true;
            }

            depth++;
            enumerator->indexes[depth] = -1;
            break;
        }

        if (tuple_index < tuple_count)
            continue;

        enumerator->indexes[depth] = -1;
        depth--;
    }

    enumerator->active = false;
    enumerator->started = false;
    return false;
}

int
age_binding_node_flat_enumerator_index(
    const AgeBindingNodeFlatEnumerator *enumerator,
    AgeBindingNodeFlatFactorKind kind, int factor_index)
{
    int index;

    if (enumerator == NULL || factor_index < 0)
        elog(ERROR, "invalid AGE factorized binding node enumerator index");

    for (index = 0; index < enumerator->factor_count; index++)
    {
        if (enumerator->factors[index].kind == kind &&
            enumerator->factors[index].factor_index == factor_index)
        {
            return enumerator->indexes[index];
        }
    }
    elog(ERROR, "AGE factorized binding node factor was not found");
    return -1;
}

int64
age_binding_node_flat_enumerator_candidate_rows(
    const AgeBindingNodeFlatEnumerator *enumerator)
{
    if (enumerator == NULL)
        elog(ERROR, "invalid AGE factorized binding node enumerator");
    return enumerator->candidate_flat_rows;
}

int64
age_binding_node_flat_enumerator_emitted_rows(
    const AgeBindingNodeFlatEnumerator *enumerator)
{
    if (enumerator == NULL)
        elog(ERROR, "invalid AGE factorized binding node enumerator");
    return enumerator->emitted_rows;
}

int64
age_binding_node_flat_enumerator_starts(
    const AgeBindingNodeFlatEnumerator *enumerator)
{
    if (enumerator == NULL)
        elog(ERROR, "invalid AGE factorized binding node enumerator");
    return enumerator->starts;
}

int64
age_binding_node_flat_enumerator_steps(
    const AgeBindingNodeFlatEnumerator *enumerator)
{
    if (enumerator == NULL)
        elog(ERROR, "invalid AGE factorized binding node enumerator");
    return enumerator->steps;
}

int64
age_binding_node_flat_enumerator_memory_bytes(
    const AgeBindingNodeFlatEnumerator *enumerator)
{
    if (enumerator == NULL)
        elog(ERROR, "invalid AGE factorized binding node enumerator");
    return enumerator->memory_bytes;
}

static int
compare_age_binding_source_rows(const void *left, const void *right)
{
    const AgeBindingSourceRow *a = left;
    const AgeBindingSourceRow *b = right;

    if (a->key < b->key)
        return -1;
    if (a->key > b->key)
        return 1;
    return 0;
}

void
age_binding_append_source_row(AgeBindingSourceRow **source_rows,
                              int *source_row_count,
                              int *source_row_capacity,
                              int64 *source_bag_bytes,
                              MemoryContext context, graphid key,
                              TupleTableSlot *slot)
{
    MinimalTuple tuple;
    MemoryContext oldcontext;

    if (source_rows == NULL || source_row_count == NULL ||
        source_row_capacity == NULL || source_bag_bytes == NULL ||
        context == NULL || slot == NULL)
    {
        elog(ERROR, "invalid AGE binding source row append state");
    }

    oldcontext = MemoryContextSwitchTo(context);
    if (*source_row_count == *source_row_capacity)
    {
        int old_capacity = *source_row_capacity;
        int new_capacity = *source_row_capacity == 0 ? 16 :
            *source_row_capacity * 2;

        if (new_capacity <= *source_row_capacity ||
            (Size)new_capacity > MaxAllocSize / sizeof(AgeBindingSourceRow))
        {
            ereport(ERROR,
                    (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                     errmsg("AGE factorized binding source bag is too large")));
        }
        if (*source_rows == NULL)
        {
            *source_rows = palloc(
                sizeof(AgeBindingSourceRow) * new_capacity);
        }
        else
        {
            *source_rows = repalloc(
                *source_rows, sizeof(AgeBindingSourceRow) * new_capacity);
        }
        *source_row_capacity = new_capacity;
        age_binding_add_byte_counter(
            source_bag_bytes,
            sizeof(AgeBindingSourceRow) * (new_capacity - old_capacity));
    }

    tuple = ExecCopySlotMinimalTuple(slot);
    (*source_rows)[*source_row_count].key = key;
    (*source_rows)[*source_row_count].tuple = tuple;
    (*source_row_count)++;
    age_binding_add_byte_counter(source_bag_bytes, tuple->t_len);
    MemoryContextSwitchTo(oldcontext);
}

AgeSourceBag *
age_binding_build_source_bags(AgeBindingSourceRow *source_rows,
                              int source_row_count, int *source_bag_count,
                              int64 *source_bag_bytes,
                              MemoryContext context)
{
    AgeSourceBag *source_bags;
    MemoryContext oldcontext;
    int row_index;
    int bag_index = -1;

    if (source_bag_count == NULL || source_bag_bytes == NULL ||
        context == NULL)
    {
        elog(ERROR, "invalid AGE binding source bag build state");
    }
    *source_bag_count = 0;
    if (source_row_count <= 0)
        return NULL;
    if (source_rows == NULL)
        elog(ERROR, "invalid AGE binding source rows");

    qsort(source_rows, source_row_count, sizeof(AgeBindingSourceRow),
          compare_age_binding_source_rows);

    oldcontext = MemoryContextSwitchTo(context);
    source_bags = palloc0(sizeof(AgeSourceBag) * source_row_count);
    age_binding_add_byte_counter(source_bag_bytes,
                                 sizeof(AgeSourceBag) * source_row_count);
    MemoryContextSwitchTo(oldcontext);

    for (row_index = 0; row_index < source_row_count; row_index++)
    {
        if (bag_index < 0 ||
            source_bags[bag_index].key != source_rows[row_index].key)
        {
            bag_index++;
            source_bags[bag_index].key = source_rows[row_index].key;
            source_bags[bag_index].first_row = row_index;
        }
        source_bags[bag_index].row_count++;
    }
    *source_bag_count = bag_index + 1;
    return source_bags;
}

AgeSourceBag *
age_binding_get_source_bag(AgeSourceBag *source_bags, int source_bag_count,
                           int source_bag_index)
{
    if (source_bags == NULL || source_bag_index < 0 ||
        source_bag_index >= source_bag_count)
    {
        return NULL;
    }
    return &source_bags[source_bag_index];
}

int
age_binding_find_source_bag_index(AgeSourceBag *source_bags,
                                  int source_bag_count,
                                  const AgeSourceBag *source_bag)
{
    uintptr_t base;
    uintptr_t pointer;
    uintptr_t offset;
    uintptr_t index;

    if (source_bags == NULL || source_bag == NULL || source_bag_count <= 0)
        return -1;

    base = (uintptr_t)source_bags;
    pointer = (uintptr_t)source_bag;
    if (pointer < base)
        return -1;
    offset = pointer - base;
    if (offset % sizeof(AgeSourceBag) != 0)
        return -1;
    index = offset / sizeof(AgeSourceBag);
    if (index >= (uintptr_t)source_bag_count)
        return -1;
    return (int)index;
}

int
age_binding_source_bag_row_count(const AgeSourceBag *source_bag)
{
    if (source_bag == NULL || source_bag->row_count <= 0)
        elog(ERROR, "invalid AGE factorized binding source bag");
    return source_bag->row_count;
}

int
age_binding_add_source_multiplicity(int *logical_count,
                                    const AgeSourceBag *source_bag)
{
    int first_tuple;
    int row_count;

    if (logical_count == NULL)
        elog(ERROR, "invalid AGE factorized binding multiplicity state");

    row_count = age_binding_source_bag_row_count(source_bag);
    if (*logical_count > PG_INT32_MAX - row_count)
    {
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("AGE factorized binding group is too large")));
    }
    first_tuple = *logical_count;
    *logical_count += row_count;
    return first_tuple;
}

int
age_binding_edge_bag_record_payload(AgeEdgeBag *edge_bag,
                                    const AgeSourceBag *source_bag)
{
    int first_tuple;

    if (edge_bag == NULL)
        elog(ERROR, "invalid AGE factorized binding edge bag");

    first_tuple = age_binding_add_source_multiplicity(
        &edge_bag->logical_count, source_bag);
    edge_bag->payload_count++;
    return first_tuple;
}

void
age_binding_init_flat_enumerator(AgeBindingFlatEnumerator *enumerator,
                                 int factor_count, MemoryContext context)
{
    MemoryContext oldcontext;

    if (enumerator == NULL || factor_count <= 0 || context == NULL)
        elog(ERROR, "invalid AGE factorized binding enumerator state");
    if ((Size)factor_count > MaxAllocSize / sizeof(int) ||
        (Size)factor_count > MaxAllocSize / sizeof(graphid) ||
        (Size)factor_count > MaxAllocSize / sizeof(bool))
    {
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("AGE factorized binding enumerator is too large")));
    }

    memset(enumerator, 0, sizeof(*enumerator));
    enumerator->factor_count = factor_count;

    oldcontext = MemoryContextSwitchTo(context);
    enumerator->logical_counts = palloc0(sizeof(int) * factor_count);
    enumerator->indexes = palloc0(sizeof(int) * factor_count);
    enumerator->enumeration_order = palloc0(sizeof(int) * factor_count);
    enumerator->selected_edge_ids = palloc0(sizeof(graphid) * factor_count);
    enumerator->selected_edge_id_valid = palloc0(sizeof(bool) * factor_count);
    MemoryContextSwitchTo(oldcontext);

    age_binding_reset_flat_enumerator(enumerator);
}

void
age_binding_reset_flat_enumerator(AgeBindingFlatEnumerator *enumerator)
{
    int factor_index;

    if (enumerator == NULL)
        elog(ERROR, "invalid AGE factorized binding enumerator state");

    for (factor_index = 0;
         factor_index < enumerator->factor_count;
         factor_index++)
    {
        enumerator->logical_counts[factor_index] = 0;
        enumerator->indexes[factor_index] = -1;
        enumerator->enumeration_order[factor_index] = factor_index;
        enumerator->selected_edge_ids[factor_index] = 0;
        enumerator->selected_edge_id_valid[factor_index] = false;
    }
    enumerator->uniqueness_groups = NULL;
    enumerator->edge_id_callback = NULL;
    enumerator->callback_state = NULL;
    enumerator->active = false;
    enumerator->started = false;
}

int64
age_binding_begin_flat_enumerator(
    AgeBindingFlatEnumerator *enumerator,
    AgeBindingFlatEnumeratorCountCallback count_callback,
    AgeBindingFlatEnumeratorEdgeIdCallback edge_id_callback,
    void *callback_state, Bitmapset **uniqueness_groups)
{
    int factor_index;
    int64 flat_rows = 1;

    if (enumerator == NULL || count_callback == NULL)
        elog(ERROR, "invalid AGE factorized binding enumerator state");

    age_binding_reset_flat_enumerator(enumerator);
    enumerator->edge_id_callback = edge_id_callback;
    enumerator->callback_state = callback_state;
    enumerator->uniqueness_groups = edge_id_callback != NULL ?
        uniqueness_groups : NULL;
    enumerator->active = true;
    age_binding_add_counter(&enumerator->starts, 1);

    for (factor_index = 0;
         factor_index < enumerator->factor_count;
         factor_index++)
    {
        int count = count_callback(callback_state, factor_index);

        if (count < 0)
            elog(ERROR, "invalid AGE factorized binding logical count");
        enumerator->logical_counts[factor_index] = count;
        if (count <= 0)
            flat_rows = 0;
        else if (flat_rows > PG_INT64_MAX / count)
            flat_rows = PG_INT64_MAX;
        else
            flat_rows *= count;
    }

    /*
     * Enumerate the smallest factor first.  This makes uniqueness conflicts cut
     * off the largest possible suffix of the Cartesian product while callers can
     * still materialize output in their own factor order.
     */
    for (factor_index = 1;
         factor_index < enumerator->factor_count;
         factor_index++)
    {
        int selected = enumerator->enumeration_order[factor_index];
        int position = factor_index;

        while (position > 0 &&
               enumerator->logical_counts[
                   enumerator->enumeration_order[position - 1]] >
               enumerator->logical_counts[selected])
        {
            enumerator->enumeration_order[position] =
                enumerator->enumeration_order[position - 1];
            position--;
        }
        enumerator->enumeration_order[position] = selected;
    }

    return flat_rows;
}

bool
age_binding_flat_enumerator_next(AgeBindingFlatEnumerator *enumerator)
{
    int depth;

    if (enumerator == NULL)
        elog(ERROR, "invalid AGE factorized binding enumerator state");
    if (!enumerator->active)
        return false;

    depth = enumerator->started ? enumerator->factor_count - 1 : 0;
    enumerator->started = true;

    while (depth >= 0)
    {
        int factor_index = enumerator->enumeration_order[depth];
        int tuple_index = enumerator->indexes[factor_index] + 1;
        int tuple_count = enumerator->logical_counts[factor_index];

        enumerator->selected_edge_id_valid[depth] = false;
        while (tuple_index < tuple_count)
        {
            bool conflict = false;
            graphid edge_id = 0;
            bool edge_id_valid = false;

            enumerator->indexes[factor_index] = tuple_index;
            age_binding_add_counter(&enumerator->steps, 1);

            if (enumerator->edge_id_callback != NULL)
            {
                int previous_depth;

                edge_id_valid = enumerator->edge_id_callback(
                    enumerator->callback_state, factor_index, tuple_index,
                    &edge_id);

                if (edge_id_valid && enumerator->uniqueness_groups != NULL)
                {
                    for (previous_depth = 0;
                         previous_depth < depth;
                         previous_depth++)
                    {
                        int previous_factor =
                            enumerator->enumeration_order[previous_depth];

                        if (enumerator->selected_edge_id_valid[previous_depth] &&
                            enumerator->selected_edge_ids[previous_depth] ==
                                edge_id &&
                            bms_overlap(
                                enumerator->uniqueness_groups[factor_index],
                                enumerator->uniqueness_groups[previous_factor]))
                        {
                            conflict = true;
                            break;
                        }
                    }
                }
                if (conflict)
                {
                    age_binding_add_counter(&enumerator->uniqueness_rejects, 1);
                    tuple_index++;
                    continue;
                }
                if (edge_id_valid)
                {
                    enumerator->selected_edge_ids[depth] = edge_id;
                    enumerator->selected_edge_id_valid[depth] = true;
                }
            }

            if (depth == enumerator->factor_count - 1)
                return true;

            depth++;
            factor_index = enumerator->enumeration_order[depth];
            enumerator->indexes[factor_index] = -1;
            enumerator->selected_edge_id_valid[depth] = false;
            break;
        }

        if (tuple_index < tuple_count)
            continue;

        enumerator->indexes[factor_index] = -1;
        enumerator->selected_edge_id_valid[depth] = false;
        depth--;
    }

    enumerator->active = false;
    enumerator->started = false;
    return false;
}

int
age_binding_flat_enumerator_index(const AgeBindingFlatEnumerator *enumerator,
                                  int factor_index)
{
    if (enumerator == NULL || factor_index < 0 ||
        factor_index >= enumerator->factor_count)
    {
        elog(ERROR, "invalid AGE factorized binding enumerator index");
    }
    return enumerator->indexes[factor_index];
}

int64
age_binding_flat_enumerator_starts(
    const AgeBindingFlatEnumerator *enumerator)
{
    if (enumerator == NULL)
        elog(ERROR, "invalid AGE factorized binding enumerator state");
    return enumerator->starts;
}

int64
age_binding_flat_enumerator_steps(
    const AgeBindingFlatEnumerator *enumerator)
{
    if (enumerator == NULL)
        elog(ERROR, "invalid AGE factorized binding enumerator state");
    return enumerator->steps;
}

int64
age_binding_flat_enumerator_uniqueness_rejects(
    const AgeBindingFlatEnumerator *enumerator)
{
    if (enumerator == NULL)
        elog(ERROR, "invalid AGE factorized binding enumerator state");
    return enumerator->uniqueness_rejects;
}
