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

#include "access/tableam.h"
#include "catalog/ag_label.h"
#include "catalog/pg_type_d.h"
#include "commands/explain_format.h"
#include "commands/explain_state.h"
#include "executor/cypher_property_projection.h"
#include "executor/executor.h"
#include "nodes/pg_list.h"
#include "utils/agtype.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/numeric.h"
#include "utils/rel.h"

typedef struct AgeProjectionListElement
{
    agtype_value *keys;
    int key_count;
    Oid value_type;
    Oid field_result_type;
} AgeProjectionListElement;

typedef struct AgePropertyProjectionSlot
{
    agtype_value *keys;
    int key_count;
    int source_slot_index;
    Oid value_type;
    Oid field_result_type;
    int final_materialization_weight;
    /*
     * When is_list is set the output column is an agtype list built from
     * elements (RETURN [a, b, ...]); the scalar keys/value_type above are
     * unused and the slot always reads the heap (never reuses another slot).
     */
    bool is_list;
    int element_count;
    AgeProjectionListElement *elements;
} AgePropertyProjectionSlot;

typedef struct AgePropertyProjectionScanState
{
    CustomScanState css;
    TableScanDesc scan;
    TupleTableSlot *heap_slot;
    /*
     * One scalar-agtype materialization buffer per output slot.  A single
     * shared buffer would alias the agtype output of every column in a
     * multi-column projection (each column would see the last column's value),
     * so each slot owns its buffer; buffers are reused across rows, preserving
     * the single-column fast path.
     */
    agtype **value_buffers;
    Size *value_buffer_sizes;
    agtype *bool_false_value;
    agtype *bool_true_value;
    AgePropertyProjectionSlot *slots;
    agtype_value *slot_values;
    bool *slot_value_ready;
    bool *slot_value_found;
    bool *slot_value_needs_free;
    int slot_count;
} AgePropertyProjectionScanState;

static Node *create_age_property_projection_scan_state(CustomScan *cscan);
static void begin_age_property_projection_scan(CustomScanState *node,
                                               EState *estate, int eflags);
static TupleTableSlot *exec_age_property_projection_scan(CustomScanState *node);
static TupleTableSlot *access_age_property_projection_scan(ScanState *node);
static void end_age_property_projection_scan(CustomScanState *node);
static void rescan_age_property_projection_scan(CustomScanState *node);
static void explain_age_property_projection_scan(CustomScanState *node,
                                                 List *ancestors,
                                                 ExplainState *es);
static void load_property_projection_slots(AgePropertyProjectionScanState *state,
                                           CustomScan *cscan);
static void load_property_projection_slot(AgePropertyProjectionSlot *slot,
                                          List *descriptor);
static agtype_value *load_property_projection_keys(List *key_consts,
                                                   int *key_count);
static bool property_projection_find_keys(
    agtype_value *keys, int key_count, agtype *properties, agtype_value *value,
    bool *value_needs_free);
static bool build_property_projection_list_datum(
    AgePropertyProjectionScanState *state, int slot_index, agtype *properties,
    Datum *result);
static void link_property_projection_duplicate_slots(
    AgePropertyProjectionScanState *state);
static bool load_property_projection_slot_value(
    AgePropertyProjectionScanState *state, int slot_index,
    agtype *properties);
static bool property_projection_slots_same_path(
    const AgePropertyProjectionSlot *left,
    const AgePropertyProjectionSlot *right);
static void free_property_projection_slot(AgePropertyProjectionSlot *slot);
static char *format_property_projection_key_path(
    AgePropertyProjectionSlot *slot);
static const char *format_property_projection_type(Oid type_oid);
static char *format_property_projection_slot(
    AgePropertyProjectionSlot *slot);
static char *format_property_projection_slots(
    AgePropertyProjectionScanState *state);
static char *format_property_projection_summary(
    AgePropertyProjectionScanState *state);
static bool property_projection_find_path(
    AgePropertyProjectionSlot *slot, agtype *properties, agtype_value *value,
    bool *value_needs_free);
static agtype *property_projection_value_to_agtype(
    AgePropertyProjectionScanState *state, int slot_index, agtype_value *value);
static bool property_projection_value_to_datum(
    AgePropertyProjectionScanState *state, AgePropertyProjectionSlot *slot,
    agtype_value *value, Datum *result);
static Datum property_projection_typed_value_to_datum(
    Oid field_result_type, agtype_value *value, bool *is_pointer);
static Datum property_projection_value_to_int8(agtype_value *value);
static Datum property_projection_value_to_float8(agtype_value *value);
static Datum property_projection_value_to_numeric(agtype_value *value);
static Datum property_projection_value_to_text(agtype_value *value);
static Datum property_projection_value_to_bool(agtype_value *value);
static agtype *prepare_property_projection_buffer(
    AgePropertyProjectionScanState *state, int slot_index, Size required_size);
static agtype *property_projection_integer_to_agtype(
    AgePropertyProjectionScanState *state, int slot_index, int64 int_value);
static agtype *property_projection_float_to_agtype(
    AgePropertyProjectionScanState *state, int slot_index, float8 float_value);
static agtype *property_projection_numeric_to_agtype(
    AgePropertyProjectionScanState *state, int slot_index, Numeric numeric);
static agtype *property_projection_string_to_agtype(
    AgePropertyProjectionScanState *state, int slot_index, const char *string,
    int string_len);
static agtype *property_projection_bool_to_agtype(bool boolean);

const CustomScanMethods age_property_projection_scan_methods = {
    AGE_PROPERTY_PROJECTION_SCAN_NAME,
    create_age_property_projection_scan_state};

static const CustomExecMethods age_property_projection_exec_methods = {
    AGE_PROPERTY_PROJECTION_SCAN_NAME,
    begin_age_property_projection_scan,
    exec_age_property_projection_scan,
    end_age_property_projection_scan,
    rescan_age_property_projection_scan,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    explain_age_property_projection_scan};

static Node *create_age_property_projection_scan_state(CustomScan *cscan)
{
    AgePropertyProjectionScanState *state;

    state = palloc0(sizeof(*state));
    state->css.ss.ps.type = T_CustomScanState;
    state->css.flags = cscan->flags;
    state->css.methods = &age_property_projection_exec_methods;
    load_property_projection_slots(state, cscan);

    return (Node *)state;
}

static void begin_age_property_projection_scan(CustomScanState *node,
                                               EState *estate, int eflags)
{
    AgePropertyProjectionScanState *state =
        (AgePropertyProjectionScanState *)node;

    (void) eflags;

    state->heap_slot = table_slot_create(node->ss.ss_currentRelation, NULL);
    state->scan = table_beginscan(node->ss.ss_currentRelation,
                                  estate->es_snapshot, 0, NULL);
    state->bool_false_value = property_projection_bool_to_agtype(false);
    state->bool_true_value = property_projection_bool_to_agtype(true);
}

static TupleTableSlot *exec_age_property_projection_scan(CustomScanState *node)
{
    return access_age_property_projection_scan(&node->ss);
}

static TupleTableSlot *access_age_property_projection_scan(ScanState *node)
{
    AgePropertyProjectionScanState *state =
        (AgePropertyProjectionScanState *)node;
    TupleTableSlot *slot = node->ss_ScanTupleSlot;

    while (table_scan_getnextslot(state->scan, ForwardScanDirection,
                                  state->heap_slot))
    {
        Datum properties_datum;
        agtype *properties;
        bool properties_isnull;
        int i;

        properties_datum = slot_getattr(
            state->heap_slot, Anum_ag_label_vertex_table_properties,
            &properties_isnull);
        ExecClearTuple(slot);
        if (properties_isnull)
        {
            for (i = 0; i < state->slot_count; i++)
            {
                slot->tts_values[i] = (Datum)0;
                slot->tts_isnull[i] = true;
            }
        }
        else
        {
            properties = DATUM_GET_AGTYPE_P(properties_datum);
            memset(state->slot_value_found, 0,
                   sizeof(bool) * state->slot_count);
            memset(state->slot_value_ready, 0,
                   sizeof(bool) * state->slot_count);
            memset(state->slot_value_needs_free, 0,
                   sizeof(bool) * state->slot_count);
            for (i = 0; i < state->slot_count; i++)
            {
                AgePropertyProjectionSlot *projection_slot = &state->slots[i];
                bool found;
                agtype_value *value;

                if (projection_slot->is_list)
                {
                    slot->tts_isnull[i] =
                        !build_property_projection_list_datum(
                            state, i, properties, &slot->tts_values[i]);
                    continue;
                }

                found = load_property_projection_slot_value(state, i,
                                                            properties);
                value = &state->slot_values[i];

                if (!found || value->type == AGTV_NULL)
                {
                    slot->tts_values[i] = (Datum)0;
                    slot->tts_isnull[i] = true;
                }
                else
                {
                    slot->tts_isnull[i] =
                        !property_projection_value_to_datum(
                            state, projection_slot, value,
                            &slot->tts_values[i]);
                }
            }
            for (i = 0; i < state->slot_count; i++)
            {
                if (state->slots[i].source_slot_index < 0 &&
                    state->slot_value_found[i] &&
                    state->slot_value_needs_free[i])
                {
                    pfree_agtype_value_content(&state->slot_values[i]);
                }
            }
        }
        ExecStoreVirtualTuple(slot);

        return slot;
    }

    return ExecClearTuple(slot);
}

static void end_age_property_projection_scan(CustomScanState *node)
{
    AgePropertyProjectionScanState *state =
        (AgePropertyProjectionScanState *)node;

    if (state->scan != NULL)
    {
        table_endscan(state->scan);
        state->scan = NULL;
    }
    if (state->heap_slot != NULL)
    {
        ExecDropSingleTupleTableSlot(state->heap_slot);
        state->heap_slot = NULL;
    }
    if (state->slots != NULL)
    {
        int i;

        for (i = 0; i < state->slot_count; i++)
        {
            free_property_projection_slot(&state->slots[i]);
            if (state->value_buffers != NULL)
                pfree_if_not_null(state->value_buffers[i]);
        }
        pfree(state->slots);
        state->slots = NULL;
        state->slot_count = 0;
    }
    pfree_if_not_null(state->slot_values);
    pfree_if_not_null(state->slot_value_ready);
    pfree_if_not_null(state->slot_value_found);
    pfree_if_not_null(state->slot_value_needs_free);
    pfree_if_not_null(state->value_buffers);
    pfree_if_not_null(state->value_buffer_sizes);
    state->slot_values = NULL;
    state->slot_value_ready = NULL;
    state->slot_value_found = NULL;
    state->slot_value_needs_free = NULL;
    state->value_buffers = NULL;
    state->value_buffer_sizes = NULL;
}

static void rescan_age_property_projection_scan(CustomScanState *node)
{
    AgePropertyProjectionScanState *state =
        (AgePropertyProjectionScanState *)node;

    if (state->scan != NULL)
        table_rescan(state->scan, NULL);
}

static void explain_age_property_projection_scan(CustomScanState *node,
                                                 List *ancestors,
                                                 ExplainState *es)
{
    AgePropertyProjectionScanState *state =
        (AgePropertyProjectionScanState *)node;

    (void)ancestors;

    if (!es->verbose)
        return;

    ExplainPropertyText("Cached Property Slots",
                        format_property_projection_slots(state),
                        es);
    ExplainPropertyInteger("Cached Property Slot Count", NULL,
                           state->slot_count, es);
    ExplainPropertyText("Cached Property Summary",
                        format_property_projection_summary(state),
                        es);
}

static void load_property_projection_slots(AgePropertyProjectionScanState *state,
                                           CustomScan *cscan)
{
    ListCell *lc;
    int slot_index = 0;

    Assert(cscan->custom_private != NIL);

    state->slot_count = list_length(cscan->custom_private);
    state->slots = palloc0(sizeof(AgePropertyProjectionSlot) *
                           state->slot_count);
    state->slot_values = palloc0(sizeof(agtype_value) * state->slot_count);
    state->slot_value_ready = palloc0(sizeof(bool) * state->slot_count);
    state->slot_value_found = palloc0(sizeof(bool) * state->slot_count);
    state->slot_value_needs_free = palloc0(sizeof(bool) *
                                           state->slot_count);
    state->value_buffers = palloc0(sizeof(agtype *) * state->slot_count);
    state->value_buffer_sizes = palloc0(sizeof(Size) * state->slot_count);

    foreach(lc, cscan->custom_private)
    {
        load_property_projection_slot(&state->slots[slot_index],
                                      lfirst_node(List, lc));
        slot_index++;
    }
    link_property_projection_duplicate_slots(state);
}

static agtype_value *load_property_projection_keys(List *key_consts,
                                                   int *key_count)
{
    agtype_value *keys;
    ListCell *lc;
    int key_index = 0;

    Assert(key_consts != NIL);

    *key_count = list_length(key_consts);
    keys = palloc0(sizeof(agtype_value) * (*key_count));

    foreach(lc, key_consts)
    {
        Const *key_const = lfirst_node(Const, lc);
        agtype *key_agtype;
        agtype_value key_value;
        bool key_needs_free = false;

        Assert(!key_const->constisnull);
        Assert(key_const->consttype == AGTYPEOID);

        key_agtype = DATUM_GET_AGTYPE_P(key_const->constvalue);
        (void)get_ith_agtype_value_from_container_no_copy(&key_agtype->root, 0,
                                                          &key_value,
                                                          &key_needs_free);
        Assert(key_value.type == AGTV_STRING);
        Assert(!key_needs_free);

        keys[key_index].type = AGTV_STRING;
        keys[key_index].val.string.len = key_value.val.string.len;
        keys[key_index].val.string.val =
            pnstrdup(key_value.val.string.val, key_value.val.string.len);
        key_index++;
    }

    return keys;
}

static void load_property_projection_list_element(
    AgeProjectionListElement *element, List *descriptor)
{
    List *key_consts;
    Const *value_type_const;
    Const *field_result_type_const;

    Assert(list_length(descriptor) == 4);
    key_consts = linitial_node(List, descriptor);
    value_type_const = lsecond_node(Const, descriptor);
    field_result_type_const = list_nth_node(Const, descriptor, 2);
    Assert(!value_type_const->constisnull);
    Assert(!field_result_type_const->constisnull);

    element->value_type = DatumGetObjectId(value_type_const->constvalue);
    element->field_result_type =
        DatumGetObjectId(field_result_type_const->constvalue);
    element->keys = load_property_projection_keys(key_consts,
                                                  &element->key_count);
}

static void load_property_projection_slot(AgePropertyProjectionSlot *slot,
                                          List *descriptor)
{
    List *key_consts;
    Const *value_type_const;
    Const *field_result_type_const;
    Integer *final_materialization_weight;
    Node *first;

    Assert(descriptor != NIL);

    /*
     * A list-output slot is tagged with a leading String; everything after it
     * is one 4-element element descriptor.  A scalar slot's first member is the
     * key-path List, so dispatch on the node tag.
     */
    first = (Node *) linitial(descriptor);
    if (IsA(first, String))
    {
        List *element_descriptors = list_copy_tail(descriptor, 1);
        ListCell *lc;
        int element_index = 0;

        Assert(strcmp(strVal(first), AGE_PROPERTY_PROJECTION_LIST_TAG) == 0);
        Assert(element_descriptors != NIL);

        slot->is_list = true;
        slot->source_slot_index = -1;
        slot->element_count = list_length(element_descriptors);
        slot->elements = palloc0(sizeof(AgeProjectionListElement) *
                                 slot->element_count);
        slot->value_type = AGTYPEOID;
        slot->field_result_type = AGTYPEOID;
        slot->final_materialization_weight = slot->element_count;

        foreach(lc, element_descriptors)
        {
            load_property_projection_list_element(
                &slot->elements[element_index], lfirst_node(List, lc));
            element_index++;
        }
        return;
    }

    Assert(list_length(descriptor) == 4);
    key_consts = linitial_node(List, descriptor);
    value_type_const = lsecond_node(Const, descriptor);
    field_result_type_const = list_nth_node(Const, descriptor, 2);
    final_materialization_weight = list_nth_node(Integer, descriptor, 3);
    Assert(!value_type_const->constisnull);
    Assert(!field_result_type_const->constisnull);

    slot->source_slot_index = -1;
    slot->value_type = DatumGetObjectId(value_type_const->constvalue);
    slot->field_result_type =
        DatumGetObjectId(field_result_type_const->constvalue);
    slot->final_materialization_weight = intVal(final_materialization_weight);

    slot->keys = load_property_projection_keys(key_consts, &slot->key_count);
}

static void link_property_projection_duplicate_slots(
    AgePropertyProjectionScanState *state)
{
    int slot_index;

    Assert(state != NULL);

    for (slot_index = 0; slot_index < state->slot_count; slot_index++)
    {
        int best_source_index = slot_index;
        int source_index;

        for (source_index = 0; source_index < state->slot_count;
             source_index++)
        {
            AgePropertyProjectionSlot *source_slot;
            AgePropertyProjectionSlot *slot;

            source_slot = &state->slots[source_index];
            slot = &state->slots[slot_index];
            if (property_projection_slots_same_path(
                    source_slot, slot) &&
                (source_slot->final_materialization_weight <
                 state->slots[best_source_index].final_materialization_weight ||
                 (source_slot->final_materialization_weight ==
                  state->slots[best_source_index].final_materialization_weight &&
                  source_index < best_source_index)))
            {
                best_source_index = source_index;
            }
        }

        state->slots[slot_index].source_slot_index =
            best_source_index == slot_index ? -1 : best_source_index;
    }
}

static bool load_property_projection_slot_value(
    AgePropertyProjectionScanState *state, int slot_index, agtype *properties)
{
    AgePropertyProjectionSlot *slot = &state->slots[slot_index];
    int source_slot_index;
    bool found;

    if (state->slot_value_ready[slot_index])
        return state->slot_value_found[slot_index];

    source_slot_index = slot->source_slot_index;
    if (source_slot_index >= 0)
    {
        found = load_property_projection_slot_value(state, source_slot_index,
                                                   properties);
        state->slot_values[slot_index] =
            state->slot_values[source_slot_index];
        state->slot_value_found[slot_index] = found;
        state->slot_value_ready[slot_index] = true;
        return found;
    }

    found = property_projection_find_path(
        slot, properties, &state->slot_values[slot_index],
        &state->slot_value_needs_free[slot_index]);
    state->slot_value_found[slot_index] = found;
    state->slot_value_ready[slot_index] = true;

    return found;
}

static bool property_projection_slots_same_path(
    const AgePropertyProjectionSlot *left,
    const AgePropertyProjectionSlot *right)
{
    int key_index;

    Assert(left != NULL);
    Assert(right != NULL);

    /* list-output slots always read the heap; never share a cached path */
    if (left->is_list || right->is_list)
        return false;

    if (left->key_count != right->key_count)
        return false;

    for (key_index = 0; key_index < left->key_count; key_index++)
    {
        const agtype_value *left_key = &left->keys[key_index];
        const agtype_value *right_key = &right->keys[key_index];

        if (left_key->val.string.len != right_key->val.string.len ||
            memcmp(left_key->val.string.val, right_key->val.string.val,
                   left_key->val.string.len) != 0)
        {
            return false;
        }
    }

    return true;
}

static void free_property_projection_keys(agtype_value *keys, int key_count)
{
    int i;

    if (keys == NULL)
        return;

    for (i = 0; i < key_count; i++)
        pfree_if_not_null(keys[i].val.string.val);
    pfree(keys);
}

static void free_property_projection_slot(AgePropertyProjectionSlot *slot)
{
    if (slot == NULL)
        return;

    if (slot->is_list)
    {
        int i;

        for (i = 0; i < slot->element_count; i++)
            free_property_projection_keys(slot->elements[i].keys,
                                          slot->elements[i].key_count);
        pfree_if_not_null(slot->elements);
        slot->elements = NULL;
        slot->element_count = 0;
        return;
    }

    if (slot->keys == NULL)
        return;

    free_property_projection_keys(slot->keys, slot->key_count);
    slot->keys = NULL;
    slot->key_count = 0;
}

static void append_property_projection_key_path(StringInfo buf,
                                                agtype_value *keys,
                                                int key_count)
{
    int i;

    for (i = 0; i < key_count; i++)
    {
        if (i > 0)
            appendStringInfoChar(buf, '.');
        appendBinaryStringInfo(buf, keys[i].val.string.val,
                               keys[i].val.string.len);
    }
}

static char *format_property_projection_key_path(
    AgePropertyProjectionSlot *slot)
{
    StringInfoData buf;

    initStringInfo(&buf);
    if (slot->is_list)
    {
        int i;

        appendStringInfoChar(&buf, '[');
        for (i = 0; i < slot->element_count; i++)
        {
            if (i > 0)
                appendStringInfoString(&buf, ", ");
            append_property_projection_key_path(&buf, slot->elements[i].keys,
                                                slot->elements[i].key_count);
        }
        appendStringInfoChar(&buf, ']');
    }
    else
    {
        append_property_projection_key_path(&buf, slot->keys, slot->key_count);
    }

    return buf.data;
}

static const char *format_property_projection_type(Oid type_oid)
{
    if (!OidIsValid(type_oid))
        return "invalid";

    return format_type_be(type_oid);
}

static char *format_property_projection_slot(
    AgePropertyProjectionSlot *slot)
{
    char *source;

    source = slot->source_slot_index >= 0 ?
        psprintf("slot-%d", slot->source_slot_index + 1) :
        pstrdup("heap-properties");

    return psprintf("source=%s, key=%s, value=%s, field=%s, final-weight=%d",
                    source,
                    format_property_projection_key_path(slot),
                    format_property_projection_type(slot->value_type),
                    format_property_projection_type(slot->field_result_type),
                    slot->final_materialization_weight);
}

static char *format_property_projection_slots(
    AgePropertyProjectionScanState *state)
{
    StringInfoData buf;
    int i;

    initStringInfo(&buf);
    for (i = 0; i < state->slot_count; i++)
    {
        if (i > 0)
            appendStringInfoString(&buf, "; ");
        appendStringInfo(&buf, "%d:{%s}", i + 1,
                         format_property_projection_slot(&state->slots[i]));
    }

    return buf.data;
}

static char *format_property_projection_summary(
    AgePropertyProjectionScanState *state)
{
    int heap_lookup_count = 0;
    int reused_slot_count = 0;
    int total_final_weight = 0;
    int heap_final_weight = 0;
    int max_final_weight = 0;
    int i;

    Assert(state != NULL);

    for (i = 0; i < state->slot_count; i++)
    {
        AgePropertyProjectionSlot *slot = &state->slots[i];

        total_final_weight += slot->final_materialization_weight;
        max_final_weight = Max(max_final_weight,
                               slot->final_materialization_weight);
        if (slot->source_slot_index >= 0)
        {
            reused_slot_count++;
        }
        else
        {
            heap_lookup_count++;
            heap_final_weight += slot->final_materialization_weight;
        }
    }

    return psprintf("slots=%d heap-lookups=%d reused=%d "
                    "final-weight=%d heap-final-weight=%d "
                    "max-final-weight=%d",
                    state->slot_count, heap_lookup_count, reused_slot_count,
                    total_final_weight, heap_final_weight, max_final_weight);
}

static bool property_projection_find_keys(
    agtype_value *keys, int key_count, agtype *properties, agtype_value *value,
    bool *value_needs_free)
{
    agtype_container *container = &properties->root;
    agtype_value current;
    bool current_needs_free = false;
    int i;

    for (i = 0; i < key_count; i++)
    {
        bool found;

        found = find_agtype_value_from_container_no_copy(
            container, AGT_FOBJECT, &keys[i], &current,
            &current_needs_free);
        if (!found)
            return false;

        if (i == key_count - 1)
        {
            *value = current;
            *value_needs_free = current_needs_free;
            return true;
        }

        if (current.type != AGTV_BINARY ||
            !AGTYPE_CONTAINER_IS_OBJECT(current.val.binary.data))
        {
            if (current_needs_free)
                pfree_agtype_value_content(&current);
            return false;
        }

        container = current.val.binary.data;
    }

    return false;
}

static bool property_projection_find_path(
    AgePropertyProjectionSlot *slot, agtype *properties, agtype_value *value,
    bool *value_needs_free)
{
    return property_projection_find_keys(slot->keys, slot->key_count,
                                         properties, value, value_needs_free);
}

/*
 * Build the agtype list output for a list-output slot directly from the heap
 * tuple's properties, mirroring agtype_build_list() semantics: each element is
 * the (possibly typed/cast) property value, and a missing element becomes an
 * agtype null rather than nulling the whole column.  The serialized list is
 * copied into the slot's reusable buffer so steady-state memory stays bounded.
 */
static bool build_property_projection_list_datum(
    AgePropertyProjectionScanState *state, int slot_index, agtype *properties,
    Datum *result)
{
    AgePropertyProjectionSlot *slot = &state->slots[slot_index];
    agtype_in_state list_state;
    agtype *built;
    agtype *buffer;
    Size built_size;
    int i;

    memset(&list_state, 0, sizeof(list_state));
    list_state.res = push_agtype_value(&list_state.parse_state,
                                       WAGT_BEGIN_ARRAY, NULL);

    for (i = 0; i < slot->element_count; i++)
    {
        AgeProjectionListElement *element = &slot->elements[i];
        agtype_value value;
        bool needs_free = false;
        bool found;

        found = property_projection_find_keys(element->keys, element->key_count,
                                              properties, &value, &needs_free);

        if (!found || value.type == AGTV_NULL)
        {
            add_agtype((Datum) 0, true, &list_state, AGTYPEOID, false);
        }
        else if (element->field_result_type == AGTYPEOID)
        {
            agtype *element_agtype = agtype_value_to_agtype(&value);

            add_agtype(AGTYPE_P_GET_DATUM(element_agtype), false, &list_state,
                       AGTYPEOID, false);
            pfree(element_agtype);
        }
        else
        {
            bool is_pointer = false;
            Datum element_datum = property_projection_typed_value_to_datum(
                element->field_result_type, &value, &is_pointer);

            add_agtype(element_datum, false, &list_state,
                       element->field_result_type, false);
            if (is_pointer)
                pfree(DatumGetPointer(element_datum));
        }

        if (needs_free)
            pfree_agtype_value_content(&value);
    }

    list_state.res = push_agtype_value(&list_state.parse_state, WAGT_END_ARRAY,
                                       NULL);
    built = agtype_value_to_agtype(list_state.res);
    pfree_agtype_in_state(&list_state);

    built_size = VARSIZE(built);
    buffer = prepare_property_projection_buffer(state, slot_index, built_size);
    memcpy(buffer, built, built_size);
    pfree(built);

    *result = AGTYPE_P_GET_DATUM(buffer);
    return true;
}

static agtype *property_projection_value_to_agtype(
    AgePropertyProjectionScanState *state, int slot_index, agtype_value *value)
{
    if (value->type == AGTV_INTEGER)
    {
        return property_projection_integer_to_agtype(state, slot_index,
                                                    value->val.int_value);
    }
    if (value->type == AGTV_FLOAT)
    {
        return property_projection_float_to_agtype(state, slot_index,
                                                  value->val.float_value);
    }
    if (value->type == AGTV_NUMERIC)
    {
        return property_projection_numeric_to_agtype(state, slot_index,
                                                    value->val.numeric);
    }
    if (value->type == AGTV_STRING)
    {
        return property_projection_string_to_agtype(state, slot_index,
                                                   value->val.string.val,
                                                   value->val.string.len);
    }
    if (value->type == AGTV_BOOL)
    {
        return value->val.boolean ? state->bool_true_value :
            state->bool_false_value;
    }

    return agtype_value_to_agtype(value);
}

/*
 * Single typed-conversion dispatch for the scalar physical field result types.
 * Both the scalar projection (property_projection_value_to_datum) and the list
 * builder (build_property_projection_list_datum) route their non-agtype results
 * through here, so the int8/float8/numeric/text cast vocabulary lives in exactly
 * one switch instead of a duplicated chain at each call site.  *is_pointer (when
 * non-NULL) reports whether the returned Datum is a freshly palloc'd pointer the
 * caller may free after consuming it.  Adding a new scalar physical type means
 * extending this one switch plus its property_projection_value_to_* helper.
 */
static Datum property_projection_typed_value_to_datum(
    Oid field_result_type, agtype_value *value, bool *is_pointer)
{
    if (is_pointer != NULL)
        *is_pointer = false;

    switch (field_result_type)
    {
    case INT8OID:
        return property_projection_value_to_int8(value);
    case FLOAT8OID:
        return property_projection_value_to_float8(value);
    case NUMERICOID:
        if (is_pointer != NULL)
            *is_pointer = true;
        return property_projection_value_to_numeric(value);
    case TEXTOID:
        if (is_pointer != NULL)
            *is_pointer = true;
        return property_projection_value_to_text(value);
    case BOOLOID:
        return property_projection_value_to_bool(value);
    default:
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("unsupported cached property field result type %u",
                        field_result_type)));
    }

    return (Datum) 0; /* unreachable */
}

static bool property_projection_value_to_datum(
    AgePropertyProjectionScanState *state, AgePropertyProjectionSlot *slot,
    agtype_value *value, Datum *result)
{
    if (slot->field_result_type == AGTYPEOID)
    {
        int slot_index = (int)(slot - state->slots);

        Assert(slot_index >= 0 && slot_index < state->slot_count);
        *result = AGTYPE_P_GET_DATUM(
            property_projection_value_to_agtype(state, slot_index, value));
        return true;
    }

    *result = property_projection_typed_value_to_datum(slot->field_result_type,
                                                       value, NULL);
    return true;
}

static Datum property_projection_value_to_int8(agtype_value *value)
{
    switch (value->type)
    {
    case AGTV_INTEGER:
        return Int64GetDatum(value->val.int_value);
    case AGTV_FLOAT:
        return DirectFunctionCall1(dtoi8,
                                   Float8GetDatum(value->val.float_value));
    case AGTV_NUMERIC:
        return DirectFunctionCall1(numeric_int8,
                                   NumericGetDatum(value->val.numeric));
    case AGTV_BOOL:
        return Int64GetDatum(value->val.boolean ? 1 : 0);
    default:
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("typecast expression must be an integer-compatible value")));
    }
}

static Datum property_projection_value_to_bool(agtype_value *value)
{
    /* Mirror the agtype->boolean cast: only an actual boolean matches. */
    if (value->type != AGTV_BOOL)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("typecast expression must be a boolean value")));

    return BoolGetDatum(value->val.boolean);
}

static Datum property_projection_value_to_float8(agtype_value *value)
{
    switch (value->type)
    {
    case AGTV_FLOAT:
        return Float8GetDatum(value->val.float_value);
    case AGTV_INTEGER:
        return Float8GetDatum((float8)value->val.int_value);
    case AGTV_NUMERIC:
        return DirectFunctionCall1(numeric_float8,
                                   NumericGetDatum(value->val.numeric));
    case AGTV_STRING:
        {
            char *str = pnstrdup(value->val.string.val,
                                 value->val.string.len);
            Datum result = DirectFunctionCall1(float8in,
                                               CStringGetDatum(str));

            pfree(str);
            return result;
        }
    default:
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("typecast expression must be a float-compatible value")));
    }
}

static Datum property_projection_value_to_numeric(agtype_value *value)
{
    switch (value->type)
    {
    case AGTV_INTEGER:
        return DirectFunctionCall1(int8_numeric,
                                   Int64GetDatum(value->val.int_value));
    case AGTV_FLOAT:
        return DirectFunctionCall1(float8_numeric,
                                   Float8GetDatum(value->val.float_value));
    case AGTV_NUMERIC:
        return NumericGetDatum(value->val.numeric);
    case AGTV_STRING:
        {
            char *str = pnstrdup(value->val.string.val,
                                 value->val.string.len);
            Datum result = DirectFunctionCall3(numeric_in,
                                               CStringGetDatum(str),
                                               ObjectIdGetDatum(InvalidOid),
                                               Int32GetDatum(-1));

            pfree(str);
            return result;
        }
    default:
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("typecast expression must be a number or a string")));
    }
}

static Datum property_projection_value_to_text(agtype_value *value)
{
    if (value->type != AGTV_STRING)
    {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("typecast expression must be a string")));
    }

    return PointerGetDatum(cstring_to_text_with_len(value->val.string.val,
                                                    value->val.string.len));
}

static agtype *prepare_property_projection_buffer(
    AgePropertyProjectionScanState *state, int slot_index, Size required_size)
{
    Assert(slot_index >= 0 && slot_index < state->slot_count);

    if (state->value_buffers[slot_index] == NULL)
    {
        state->value_buffers[slot_index] = palloc(required_size);
        state->value_buffer_sizes[slot_index] = required_size;
    }
    else if (state->value_buffer_sizes[slot_index] < required_size)
    {
        state->value_buffers[slot_index] =
            repalloc(state->value_buffers[slot_index], required_size);
        state->value_buffer_sizes[slot_index] = required_size;
    }

    return state->value_buffers[slot_index];
}

static agtype *property_projection_integer_to_agtype(
    AgePropertyProjectionScanState *state, int slot_index, int64 int_value)
{
    agtype *out;
    char *data;
    agtentry entry;
    uint32 header;
    uint32 type_header;

    out = prepare_property_projection_buffer(
        state, slot_index, VARHDRSZ + sizeof(uint32) + sizeof(agtentry) +
        sizeof(uint32) + sizeof(int64));
    SET_VARSIZE(out, VARHDRSZ + sizeof(uint32) + sizeof(agtentry) +
                sizeof(uint32) + sizeof(int64));

    header = AGT_FARRAY | AGT_FSCALAR | 1;
    out->root.header = header;

    entry = AGTENTRY_IS_AGTYPE | AGTENTRY_HAS_OFF |
            (sizeof(uint32) + sizeof(int64));
    out->root.children[0] = entry;

    data = (char *)&out->root.children[1];
    type_header = AGT_HEADER_INTEGER;
    memcpy(data, &type_header, sizeof(type_header));
    memcpy(data + sizeof(type_header), &int_value, sizeof(int_value));

    return out;
}

static agtype *property_projection_float_to_agtype(
    AgePropertyProjectionScanState *state, int slot_index, float8 float_value)
{
    agtype *out;
    char *data;
    agtentry entry;
    uint32 header;
    uint32 type_header;

    out = prepare_property_projection_buffer(
        state, slot_index, VARHDRSZ + sizeof(uint32) + sizeof(agtentry) +
        sizeof(uint32) + sizeof(float8));
    SET_VARSIZE(out, VARHDRSZ + sizeof(uint32) + sizeof(agtentry) +
                sizeof(uint32) + sizeof(float8));

    header = AGT_FARRAY | AGT_FSCALAR | 1;
    out->root.header = header;

    entry = AGTENTRY_IS_AGTYPE | AGTENTRY_HAS_OFF |
            (sizeof(uint32) + sizeof(float8));
    out->root.children[0] = entry;

    data = (char *)&out->root.children[1];
    type_header = AGT_HEADER_FLOAT;
    memcpy(data, &type_header, sizeof(type_header));
    memcpy(data + sizeof(type_header), &float_value, sizeof(float_value));

    return out;
}

static agtype *property_projection_numeric_to_agtype(
    AgePropertyProjectionScanState *state, int slot_index, Numeric numeric)
{
    agtype *out;
    char *data;
    agtentry entry;
    int numeric_len;
    uint32 header;

    numeric_len = VARSIZE_ANY(numeric);
    if (numeric_len > AGTENTRY_OFFLENMASK)
    {
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("numeric length exceeds the maximum of %u bytes",
                        AGTENTRY_OFFLENMASK)));
    }

    out = prepare_property_projection_buffer(
        state, slot_index,
        VARHDRSZ + sizeof(uint32) + sizeof(agtentry) + numeric_len);
    SET_VARSIZE(out, VARHDRSZ + sizeof(uint32) + sizeof(agtentry) +
                numeric_len);

    header = AGT_FARRAY | AGT_FSCALAR | 1;
    out->root.header = header;

    entry = AGTENTRY_IS_NUMERIC | AGTENTRY_HAS_OFF | numeric_len;
    out->root.children[0] = entry;

    data = (char *)&out->root.children[1];
    memcpy(data, (char *)numeric, numeric_len);

    return out;
}

static agtype *property_projection_bool_to_agtype(bool boolean)
{
    agtype *out;
    agtentry entry;
    uint32 header;

    out = palloc(VARHDRSZ + sizeof(uint32) + sizeof(agtentry));
    SET_VARSIZE(out, VARHDRSZ + sizeof(uint32) + sizeof(agtentry));

    header = AGT_FARRAY | AGT_FSCALAR | 1;
    out->root.header = header;

    entry = boolean ? AGTENTRY_IS_BOOL_TRUE : AGTENTRY_IS_BOOL_FALSE;
    out->root.children[0] = entry;

    return out;
}

static agtype *property_projection_string_to_agtype(
    AgePropertyProjectionScanState *state, int slot_index, const char *string,
                                                    int string_len)
{
    agtype *out;
    char *data;
    agtentry entry;
    uint32 header;

    if (string_len > AGTENTRY_OFFLENMASK)
    {
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("string length exceeds the maximum of %u bytes",
                        AGTENTRY_OFFLENMASK)));
    }

    out = prepare_property_projection_buffer(
        state, slot_index,
        VARHDRSZ + sizeof(uint32) + sizeof(agtentry) + string_len);
    SET_VARSIZE(out, VARHDRSZ + sizeof(uint32) + sizeof(agtentry) +
                string_len);

    header = AGT_FARRAY | AGT_FSCALAR | 1;
    out->root.header = header;

    entry = AGTENTRY_IS_STRING | AGTENTRY_HAS_OFF | string_len;
    out->root.children[0] = entry;

    data = (char *)&out->root.children[1];
    memcpy(data, string, string_len);

    return out;
}
