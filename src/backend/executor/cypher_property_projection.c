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

typedef struct AgePropertyProjectionSlot
{
    agtype_value *keys;
    int key_count;
    int source_slot_index;
    Oid value_type;
    Oid field_result_type;
    int final_materialization_weight;
} AgePropertyProjectionSlot;

typedef struct AgePropertyProjectionScanState
{
    CustomScanState css;
    TableScanDesc scan;
    TupleTableSlot *heap_slot;
    agtype *value_buffer;
    Size value_buffer_size;
    agtype *bool_false_value;
    agtype *bool_true_value;
    AgePropertyProjectionSlot *slots;
    agtype_value *slot_values;
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
static void link_property_projection_duplicate_slots(
    AgePropertyProjectionScanState *state);
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
static bool property_projection_find_path(
    AgePropertyProjectionSlot *slot, agtype *properties, agtype_value *value,
    bool *value_needs_free);
static agtype *property_projection_value_to_agtype(
    AgePropertyProjectionScanState *state, agtype_value *value);
static bool property_projection_value_to_datum(
    AgePropertyProjectionScanState *state, AgePropertyProjectionSlot *slot,
    agtype_value *value, Datum *result);
static Datum property_projection_value_to_int8(agtype_value *value);
static Datum property_projection_value_to_float8(agtype_value *value);
static Datum property_projection_value_to_numeric(agtype_value *value);
static Datum property_projection_value_to_text(agtype_value *value);
static agtype *prepare_property_projection_buffer(
    AgePropertyProjectionScanState *state, Size required_size);
static agtype *property_projection_integer_to_agtype(
    AgePropertyProjectionScanState *state, int64 int_value);
static agtype *property_projection_float_to_agtype(
    AgePropertyProjectionScanState *state, float8 float_value);
static agtype *property_projection_numeric_to_agtype(
    AgePropertyProjectionScanState *state, Numeric numeric);
static agtype *property_projection_string_to_agtype(
    AgePropertyProjectionScanState *state, const char *string,
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
            memset(state->slot_value_needs_free, 0,
                   sizeof(bool) * state->slot_count);
            for (i = 0; i < state->slot_count; i++)
            {
                AgePropertyProjectionSlot *projection_slot = &state->slots[i];
                bool found;
                agtype_value *value;

                if (projection_slot->source_slot_index >= 0)
                {
                    int source_slot_index = projection_slot->source_slot_index;

                    found = state->slot_value_found[source_slot_index];
                    value = &state->slot_values[source_slot_index];
                }
                else
                {
                    found = property_projection_find_path(
                        projection_slot, properties, &state->slot_values[i],
                        &state->slot_value_needs_free[i]);
                    state->slot_value_found[i] = found;
                    value = &state->slot_values[i];
                }

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
            free_property_projection_slot(&state->slots[i]);
        pfree(state->slots);
        state->slots = NULL;
        state->slot_count = 0;
    }
    pfree_if_not_null(state->slot_values);
    pfree_if_not_null(state->slot_value_found);
    pfree_if_not_null(state->slot_value_needs_free);
    state->slot_values = NULL;
    state->slot_value_found = NULL;
    state->slot_value_needs_free = NULL;
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
    state->slot_value_found = palloc0(sizeof(bool) * state->slot_count);
    state->slot_value_needs_free = palloc0(sizeof(bool) *
                                           state->slot_count);

    foreach(lc, cscan->custom_private)
    {
        load_property_projection_slot(&state->slots[slot_index],
                                      lfirst_node(List, lc));
        slot_index++;
    }
    link_property_projection_duplicate_slots(state);
}

static void load_property_projection_slot(AgePropertyProjectionSlot *slot,
                                          List *descriptor)
{
    List *key_consts;
    ListCell *lc;
    Const *value_type_const;
    Const *field_result_type_const;
    Integer *final_materialization_weight;
    int key_index = 0;

    Assert(list_length(descriptor) == 4);
    key_consts = linitial_node(List, descriptor);
    value_type_const = lsecond_node(Const, descriptor);
    field_result_type_const = list_nth_node(Const, descriptor, 2);
    final_materialization_weight = list_nth_node(Integer, descriptor, 3);
    Assert(key_consts != NIL);
    Assert(!value_type_const->constisnull);
    Assert(!field_result_type_const->constisnull);

    slot->source_slot_index = -1;
    slot->value_type = DatumGetObjectId(value_type_const->constvalue);
    slot->field_result_type =
        DatumGetObjectId(field_result_type_const->constvalue);
    slot->final_materialization_weight = intVal(final_materialization_weight);

    slot->key_count = list_length(key_consts);
    slot->keys = palloc0(sizeof(agtype_value) * slot->key_count);

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

        slot->keys[key_index].type = AGTV_STRING;
        slot->keys[key_index].val.string.len = key_value.val.string.len;
        slot->keys[key_index].val.string.val =
            pnstrdup(key_value.val.string.val, key_value.val.string.len);
        key_index++;
    }
}

static void link_property_projection_duplicate_slots(
    AgePropertyProjectionScanState *state)
{
    int slot_index;

    Assert(state != NULL);

    for (slot_index = 0; slot_index < state->slot_count; slot_index++)
    {
        int source_index;

        for (source_index = 0; source_index < slot_index; source_index++)
        {
            if (property_projection_slots_same_path(
                    &state->slots[source_index],
                    &state->slots[slot_index]))
            {
                state->slots[slot_index].source_slot_index = source_index;
                break;
            }
        }
    }
}

static bool property_projection_slots_same_path(
    const AgePropertyProjectionSlot *left,
    const AgePropertyProjectionSlot *right)
{
    int key_index;

    Assert(left != NULL);
    Assert(right != NULL);

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

static void free_property_projection_slot(AgePropertyProjectionSlot *slot)
{
    int i;

    if (slot == NULL || slot->keys == NULL)
        return;

    for (i = 0; i < slot->key_count; i++)
        pfree_if_not_null(slot->keys[i].val.string.val);
    pfree(slot->keys);
    slot->keys = NULL;
    slot->key_count = 0;
}

static char *format_property_projection_key_path(
    AgePropertyProjectionSlot *slot)
{
    StringInfoData buf;
    int i;

    initStringInfo(&buf);
    for (i = 0; i < slot->key_count; i++)
    {
        if (i > 0)
            appendStringInfoChar(&buf, '.');
        appendBinaryStringInfo(&buf, slot->keys[i].val.string.val,
                               slot->keys[i].val.string.len);
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

static bool property_projection_find_path(
    AgePropertyProjectionSlot *slot, agtype *properties, agtype_value *value,
    bool *value_needs_free)
{
    agtype_container *container = &properties->root;
    agtype_value current;
    bool current_needs_free = false;
    int i;

    for (i = 0; i < slot->key_count; i++)
    {
        bool found;

        found = find_agtype_value_from_container_no_copy(
            container, AGT_FOBJECT, &slot->keys[i], &current,
            &current_needs_free);
        if (!found)
            return false;

        if (i == slot->key_count - 1)
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

static agtype *property_projection_value_to_agtype(
    AgePropertyProjectionScanState *state, agtype_value *value)
{
    if (value->type == AGTV_INTEGER)
    {
        return property_projection_integer_to_agtype(state,
                                                    value->val.int_value);
    }
    if (value->type == AGTV_FLOAT)
    {
        return property_projection_float_to_agtype(state,
                                                  value->val.float_value);
    }
    if (value->type == AGTV_NUMERIC)
    {
        return property_projection_numeric_to_agtype(state,
                                                    value->val.numeric);
    }
    if (value->type == AGTV_STRING)
    {
        return property_projection_string_to_agtype(state,
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

static bool property_projection_value_to_datum(
    AgePropertyProjectionScanState *state, AgePropertyProjectionSlot *slot,
    agtype_value *value, Datum *result)
{
    if (slot->field_result_type == AGTYPEOID)
    {
        *result = AGTYPE_P_GET_DATUM(
            property_projection_value_to_agtype(state, value));
        return true;
    }
    if (slot->field_result_type == INT8OID)
    {
        *result = property_projection_value_to_int8(value);
        return true;
    }
    if (slot->field_result_type == FLOAT8OID)
    {
        *result = property_projection_value_to_float8(value);
        return true;
    }
    if (slot->field_result_type == NUMERICOID)
    {
        *result = property_projection_value_to_numeric(value);
        return true;
    }
    if (slot->field_result_type == TEXTOID)
    {
        *result = property_projection_value_to_text(value);
        return true;
    }

    ereport(ERROR,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
             errmsg("unsupported cached property field result type %u",
                    slot->field_result_type)));
    return false;
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
    AgePropertyProjectionScanState *state, Size required_size)
{
    if (state->value_buffer == NULL)
    {
        state->value_buffer = palloc(required_size);
        state->value_buffer_size = required_size;
    }
    else if (state->value_buffer_size < required_size)
    {
        state->value_buffer = repalloc(state->value_buffer, required_size);
        state->value_buffer_size = required_size;
    }

    return state->value_buffer;
}

static agtype *property_projection_integer_to_agtype(
    AgePropertyProjectionScanState *state, int64 int_value)
{
    agtype *out;
    char *data;
    agtentry entry;
    uint32 header;
    uint32 type_header;

    out = prepare_property_projection_buffer(
        state, VARHDRSZ + sizeof(uint32) + sizeof(agtentry) +
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
    AgePropertyProjectionScanState *state, float8 float_value)
{
    agtype *out;
    char *data;
    agtentry entry;
    uint32 header;
    uint32 type_header;

    out = prepare_property_projection_buffer(
        state, VARHDRSZ + sizeof(uint32) + sizeof(agtentry) +
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
    AgePropertyProjectionScanState *state, Numeric numeric)
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
        state, VARHDRSZ + sizeof(uint32) + sizeof(agtentry) + numeric_len);
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
    AgePropertyProjectionScanState *state, const char *string,
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
        state, VARHDRSZ + sizeof(uint32) + sizeof(agtentry) + string_len);
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
