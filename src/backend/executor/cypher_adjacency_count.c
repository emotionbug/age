/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "postgres.h"

#include "access/age_adjacency.h"
#include "catalog/pg_type_d.h"
#include "commands/explain_format.h"
#include "commands/explain_state.h"
#include "common/int.h"
#include "executor/cypher_adjacency_count.h"
#include "executor/executor.h"
#include "nodes/pg_list.h"
#include "utils/agtype.h"
#include "utils/builtins.h"
#include "utils/graphid.h"
#include "utils/lsyscache.h"

typedef struct AgeAdjacencyCountScanState
{
    CustomScanState css;
    Oid index_oid;
    graphid key;
    Oid output_type;
    AgeAdjacencyCountMode count_mode;
    int64 source_rows;
    int64 count;
    bool emitted;
    bool executed;
} AgeAdjacencyCountScanState;

static Node *create_age_adjacency_count_scan_state(CustomScan *cscan);
static void begin_age_adjacency_count_scan(CustomScanState *node,
                                           EState *estate, int eflags);
static TupleTableSlot *exec_age_adjacency_count_scan(CustomScanState *node);
static TupleTableSlot *access_age_adjacency_count_scan(ScanState *node);
static bool recheck_age_adjacency_count_scan(ScanState *node,
                                             TupleTableSlot *slot);
static void end_age_adjacency_count_scan(CustomScanState *node);
static void rescan_age_adjacency_count_scan(CustomScanState *node);
static void explain_age_adjacency_count_scan(CustomScanState *node,
                                             List *ancestors,
                                             ExplainState *es);

const CustomScanMethods age_adjacency_count_scan_methods = {
    AGE_ADJACENCY_COUNT_SCAN_NAME,
    create_age_adjacency_count_scan_state
};

static const CustomExecMethods age_adjacency_count_exec_methods = {
    AGE_ADJACENCY_COUNT_SCAN_NAME,
    begin_age_adjacency_count_scan,
    exec_age_adjacency_count_scan,
    end_age_adjacency_count_scan,
    rescan_age_adjacency_count_scan,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    explain_age_adjacency_count_scan
};

static Node *
create_age_adjacency_count_scan_state(CustomScan *cscan)
{
    AgeAdjacencyCountScanState *state;
    Const *index_const;
    Const *key_const;
    Const *type_const;

    Assert(list_length(cscan->custom_private) ==
           AGE_ADJACENCY_COUNT_PRIVATE_COUNT);

    index_const = list_nth_node(
        Const, cscan->custom_private,
        AGE_ADJACENCY_COUNT_PRIVATE_INDEX_OID);
    key_const = list_nth_node(
        Const, cscan->custom_private,
        AGE_ADJACENCY_COUNT_PRIVATE_KEY);
    type_const = list_nth_node(
        Const, cscan->custom_private,
        AGE_ADJACENCY_COUNT_PRIVATE_OUTPUT_TYPE);

    Assert(!index_const->constisnull && index_const->consttype == OIDOID);
    Assert(!key_const->constisnull && key_const->consttype == GRAPHIDOID);
    Assert(!type_const->constisnull && type_const->consttype == OIDOID);

    state = palloc0(sizeof(*state));
    state->css.ss.ps.type = T_CustomScanState;
    state->css.flags = cscan->flags;
    state->css.methods = &age_adjacency_count_exec_methods;
    state->index_oid = DatumGetObjectId(index_const->constvalue);
    state->key = DATUM_GET_GRAPHID(key_const->constvalue);
    state->output_type = DatumGetObjectId(type_const->constvalue);
    state->count_mode = AGE_ADJACENCY_COUNT_VISIBLE_SCAN;

    return (Node *)state;
}

static void
begin_age_adjacency_count_scan(CustomScanState *node, EState *estate,
                               int eflags)
{
    CustomScan *cscan = (CustomScan *)node->ss.ps.plan;
    ListCell *lc;

    foreach(lc, cscan->custom_plans)
    {
        Plan *subplan = (Plan *) lfirst(lc);

        node->custom_ps = lappend(node->custom_ps,
                                  ExecInitNode(subplan, estate, eflags));
    }

    Assert(list_length(node->custom_ps) == 1);
}

static TupleTableSlot *
exec_age_adjacency_count_scan(CustomScanState *node)
{
    return ExecScan(&node->ss, access_age_adjacency_count_scan,
                    recheck_age_adjacency_count_scan);
}

static TupleTableSlot *
access_age_adjacency_count_scan(ScanState *node)
{
    AgeAdjacencyCountScanState *state =
        (AgeAdjacencyCountScanState *)node;
    TupleTableSlot *slot = node->ss_ScanTupleSlot;
    PlanState *source_plan;
    TupleTableSlot *source_slot;
    int64 per_source_count = 0;

    if (state->emitted)
        return ExecClearTuple(slot);

    state->emitted = true;
    state->executed = true;
    state->source_rows = 0;
    state->count = 0;
    source_plan = (PlanState *) linitial(state->css.custom_ps);

    for (;;)
    {
        source_slot = ExecProcNode(source_plan);
        if (TupIsNull(source_slot))
            break;
        if (state->source_rows == PG_INT64_MAX)
            ereport(ERROR,
                    (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                     errmsg("bigint out of range")));
        state->source_rows++;
    }

    if (state->source_rows > 0)
    {
        per_source_count = age_adjacency_count_visible_payloads(
            state->index_oid, state->key, node->ps.state->es_snapshot,
            &state->count_mode);
        if (pg_mul_s64_overflow(per_source_count, state->source_rows,
                                &state->count))
        {
            ereport(ERROR,
                    (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                     errmsg("bigint out of range")));
        }
    }

    ExecClearTuple(slot);
    if (slot->tts_tupleDescriptor->natts != 1)
        elog(ERROR, "AGE adjacency count scan expected one output column");

    if (state->output_type == INT8OID)
    {
        slot->tts_values[0] = Int64GetDatum(state->count);
    }
    else if (state->output_type == AGTYPEOID)
    {
        slot->tts_values[0] =
            PointerGetDatum(agtype_integer_to_agtype(state->count));
    }
    else
    {
        ereport(ERROR,
                (errcode(ERRCODE_DATATYPE_MISMATCH),
                 errmsg("AGE adjacency count scan cannot produce type %s",
                        format_type_be(state->output_type))));
    }
    slot->tts_isnull[0] = false;
    ExecStoreVirtualTuple(slot);

    return slot;
}

static bool
recheck_age_adjacency_count_scan(ScanState *node, TupleTableSlot *slot)
{
    (void)node;
    (void)slot;
    return true;
}

static void
end_age_adjacency_count_scan(CustomScanState *node)
{
    ListCell *lc;

    foreach(lc, node->custom_ps)
        ExecEndNode((PlanState *) lfirst(lc));
}

static void
rescan_age_adjacency_count_scan(CustomScanState *node)
{
    AgeAdjacencyCountScanState *state =
        (AgeAdjacencyCountScanState *)node;
    ListCell *lc;

    foreach(lc, node->custom_ps)
        ExecReScan((PlanState *) lfirst(lc));

    state->emitted = false;
    state->executed = false;
    state->source_rows = 0;
    state->count = 0;
    state->count_mode = AGE_ADJACENCY_COUNT_VISIBLE_SCAN;
}

static void
explain_age_adjacency_count_scan(CustomScanState *node, List *ancestors,
                                 ExplainState *es)
{
    AgeAdjacencyCountScanState *state =
        (AgeAdjacencyCountScanState *)node;
    char *index_name;

    (void)ancestors;

    index_name = get_rel_name(state->index_oid);
    ExplainPropertyText("Adjacency Count Index",
                        index_name != NULL ? index_name : "unknown", es);
    ExplainPropertyInteger("Adjacency Count Key", NULL, state->key, es);
    ExplainPropertyText(
        "Adjacency Count Contract",
        "directory-summary only when index counts are exact and every edge "
        "heap page is all-visible; otherwise visible posting scan",
        es);

    if (es->analyze && state->executed)
    {
        ExplainPropertyText(
            "Adjacency Count Runtime",
            state->count_mode == AGE_ADJACENCY_COUNT_DIRECTORY_SUMMARY ?
            "directory-summary" : "visible-posting-scan",
            es);
        ExplainPropertyInteger("Adjacency Count Source Rows", NULL,
                               state->source_rows, es);
        ExplainPropertyInteger("Adjacency Count Result", NULL,
                               state->count, es);
    }
}
