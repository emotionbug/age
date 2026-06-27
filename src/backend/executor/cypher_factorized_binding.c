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
