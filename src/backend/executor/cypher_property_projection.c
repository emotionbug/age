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
#include "executor/cypher_property_projection.h"
#include "executor/executor.h"
#include "utils/agtype.h"
#include "utils/builtins.h"
#include "utils/numeric.h"
#include "utils/rel.h"

typedef struct AgePropertyProjectionScanState
{
    CustomScanState css;
    TableScanDesc scan;
    TupleTableSlot *heap_slot;
    agtype *value_buffer;
    Size value_buffer_size;
    agtype *bool_false_value;
    agtype *bool_true_value;
    agtype_value key;
    Oid value_type;
    Oid field_result_type;
} AgePropertyProjectionScanState;

static Node *create_age_property_projection_scan_state(CustomScan *cscan);
static void begin_age_property_projection_scan(CustomScanState *node,
                                               EState *estate, int eflags);
static TupleTableSlot *exec_age_property_projection_scan(CustomScanState *node);
static TupleTableSlot *access_age_property_projection_scan(ScanState *node);
static void end_age_property_projection_scan(CustomScanState *node);
static void rescan_age_property_projection_scan(CustomScanState *node);
static void load_property_projection_key(AgePropertyProjectionScanState *state,
                                         CustomScan *cscan);
static agtype *property_projection_value_to_agtype(
    AgePropertyProjectionScanState *state, agtype_value *value);
static bool property_projection_value_to_datum(
    AgePropertyProjectionScanState *state, agtype_value *value,
    Datum *result);
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
    NULL};

static Node *create_age_property_projection_scan_state(CustomScan *cscan)
{
    AgePropertyProjectionScanState *state;

    state = palloc0(sizeof(*state));
    state->css.ss.ps.type = T_CustomScanState;
    state->css.flags = cscan->flags;
    state->css.methods = &age_property_projection_exec_methods;
    load_property_projection_key(state, cscan);

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
        agtype_value value;
        bool found;
        bool value_needs_free = false;
        bool properties_isnull;

        properties_datum = slot_getattr(
            state->heap_slot, Anum_ag_label_vertex_table_properties,
            &properties_isnull);
        ExecClearTuple(slot);
        if (properties_isnull)
        {
            slot->tts_values[0] = (Datum)0;
            slot->tts_isnull[0] = true;
        }
        else
        {
            properties = DATUM_GET_AGTYPE_P(properties_datum);
            found = find_agtype_value_from_container_no_copy(
                &properties->root, AGT_FOBJECT, &state->key, &value,
                &value_needs_free);
            if (!found || value.type == AGTV_NULL)
            {
                slot->tts_values[0] = (Datum)0;
                slot->tts_isnull[0] = true;
            }
            else
            {
                slot->tts_isnull[0] =
                    !property_projection_value_to_datum(
                        state, &value, &slot->tts_values[0]);
            }
            if (value_needs_free)
                pfree_agtype_value_content(&value);
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
}

static void rescan_age_property_projection_scan(CustomScanState *node)
{
    AgePropertyProjectionScanState *state =
        (AgePropertyProjectionScanState *)node;

    if (state->scan != NULL)
        table_rescan(state->scan, NULL);
}

static void load_property_projection_key(AgePropertyProjectionScanState *state,
                                         CustomScan *cscan)
{
    Const *key_const;
    Const *value_type_const;
    Const *field_result_type_const;
    agtype *key_agtype;
    agtype_value key_value;
    bool key_needs_free = false;

    Assert(list_length(cscan->custom_private) == 3);
    key_const = linitial_node(Const, cscan->custom_private);
    value_type_const = lsecond_node(Const, cscan->custom_private);
    field_result_type_const = list_nth_node(Const, cscan->custom_private, 2);
    Assert(!key_const->constisnull);
    Assert(!value_type_const->constisnull);
    Assert(!field_result_type_const->constisnull);

    state->value_type = DatumGetObjectId(value_type_const->constvalue);
    state->field_result_type =
        DatumGetObjectId(field_result_type_const->constvalue);

    key_agtype = DATUM_GET_AGTYPE_P(key_const->constvalue);
    (void)get_ith_agtype_value_from_container_no_copy(&key_agtype->root, 0,
                                                      &key_value,
                                                      &key_needs_free);
    Assert(key_value.type == AGTV_STRING);
    Assert(!key_needs_free);

    state->key.type = AGTV_STRING;
    state->key.val.string.len = key_value.val.string.len;
    state->key.val.string.val = pnstrdup(key_value.val.string.val,
                                         key_value.val.string.len);
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
    AgePropertyProjectionScanState *state, agtype_value *value,
    Datum *result)
{
    if (state->field_result_type == AGTYPEOID)
    {
        *result = AGTYPE_P_GET_DATUM(
            property_projection_value_to_agtype(state, value));
        return true;
    }
    if (state->field_result_type == INT8OID)
    {
        *result = property_projection_value_to_int8(value);
        return true;
    }
    if (state->field_result_type == FLOAT8OID)
    {
        *result = property_projection_value_to_float8(value);
        return true;
    }
    if (state->field_result_type == NUMERICOID)
    {
        *result = property_projection_value_to_numeric(value);
        return true;
    }
    if (state->field_result_type == TEXTOID)
    {
        *result = property_projection_value_to_text(value);
        return true;
    }

    ereport(ERROR,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
             errmsg("unsupported cached property field result type %u",
                    state->field_result_type)));
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
