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

#include "access/age_adjacency.h"
#include "catalog/ag_label.h"
#include "executor/cypher_adjacency_match.h"
#include "executor/executor.h"
#include "utils/datum.h"
#include "utils/graphid.h"
#include "utils/memutils.h"
#include "utils/rel.h"

typedef struct AgeAdjacencyMatchTuple
{
    graphid edge_id;
    graphid start_id;
    graphid end_id;
    Datum properties;
    bool properties_isnull;
} AgeAdjacencyMatchTuple;

typedef struct AgeAdjacencyMatchScanState
{
    CustomScanState css;
    Oid index_oid;
    bool outgoing;
    bool fetch_properties;
    int scan_nattrs;
    AttrNumber *scan_attnos;
    graphid current_key;
    AgeAdjacencyVisiblePayloadScan *payload_scan;
    ExprState *key_expr_state;
    MemoryContext payload_scan_context;
    MemoryContext scan_context;
    List *tuples;
    ListCell *next_tuple;
    bool scanned;
} AgeAdjacencyMatchScanState;

static Node *create_age_adjacency_match_scan_state(CustomScan *cscan);
static void begin_age_adjacency_match_scan(CustomScanState *node,
                                           EState *estate, int eflags);
static TupleTableSlot *exec_age_adjacency_match_scan(CustomScanState *node);
static TupleTableSlot *access_age_adjacency_match_scan(ScanState *node);
static bool recheck_age_adjacency_match_scan(ScanState *node,
                                             TupleTableSlot *slot);
static void end_age_adjacency_match_scan(CustomScanState *node);
static void rescan_age_adjacency_match_scan(CustomScanState *node);
static bool store_age_adjacency_match_tuple(
    const AgeAdjacencyPayload *payload, void *callback_state);

const CustomScanMethods age_adjacency_match_scan_methods = {
    AGE_ADJACENCY_MATCH_SCAN_NAME, create_age_adjacency_match_scan_state};

static const CustomExecMethods age_adjacency_match_exec_methods = {
    AGE_ADJACENCY_MATCH_SCAN_NAME,
    begin_age_adjacency_match_scan,
    exec_age_adjacency_match_scan,
    end_age_adjacency_match_scan,
    rescan_age_adjacency_match_scan,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL};

static Node *create_age_adjacency_match_scan_state(CustomScan *cscan)
{
    AgeAdjacencyMatchScanState *state;
    Const *index_const;
    Const *outgoing_const;
    ListCell *lc;
    int nattrs = 0;

    state = palloc0(sizeof(*state));

    Assert(list_length(cscan->custom_private) == 2);

    index_const = linitial(cscan->custom_private);
    outgoing_const = lsecond(cscan->custom_private);

    state->index_oid = DatumGetObjectId(index_const->constvalue);
    state->outgoing = DatumGetBool(outgoing_const->constvalue);
    state->fetch_properties = true;
    state->css.ss.ps.type = T_CustomScanState;
    state->css.flags = cscan->flags;
    state->css.methods = &age_adjacency_match_exec_methods;

    if (cscan->custom_scan_tlist != NIL)
    {
        state->scan_nattrs = list_length(cscan->custom_scan_tlist);
        state->scan_attnos =
            palloc0(sizeof(AttrNumber) * state->scan_nattrs);

        foreach(lc, cscan->custom_scan_tlist)
        {
            TargetEntry *tle = lfirst_node(TargetEntry, lc);
            Var *var;

            Assert(IsA(tle->expr, Var));
            var = (Var *)tle->expr;
            state->scan_attnos[nattrs++] = var->varattno;
        }

        state->fetch_properties = false;
        for (nattrs = 0; nattrs < state->scan_nattrs; nattrs++)
        {
            if (state->scan_attnos[nattrs] ==
                Anum_ag_label_edge_table_properties)
            {
                state->fetch_properties = true;
                break;
            }
        }
    }

    return (Node *)state;
}

static void begin_age_adjacency_match_scan(CustomScanState *node,
                                           EState *estate, int eflags)
{
    AgeAdjacencyMatchScanState *state =
        (AgeAdjacencyMatchScanState *)node;
    CustomScan *cscan = (CustomScan *)node->ss.ps.plan;
    MemoryContext oldcontext;

    (void) eflags;

    state->scan_context = AllocSetContextCreate(
        node->ss.ps.state->es_query_cxt,
        "AGE adjacency match scan context",
        ALLOCSET_DEFAULT_SIZES);
    state->payload_scan_context = node->ss.ps.state->es_query_cxt;
    oldcontext = MemoryContextSwitchTo(state->payload_scan_context);
    state->payload_scan = age_adjacency_begin_visible_payload_scan(
        state->index_oid, estate->es_snapshot, state->fetch_properties);
    MemoryContextSwitchTo(oldcontext);

    if (cscan->custom_exprs != NIL)
    {
        Expr *key_expr = linitial(cscan->custom_exprs);

        state->key_expr_state =
            ExecInitExpr(key_expr, (PlanState *)node);
    }
}

static TupleTableSlot *exec_age_adjacency_match_scan(CustomScanState *node)
{
    return ExecScan(&node->ss, access_age_adjacency_match_scan,
                    recheck_age_adjacency_match_scan);
}

static TupleTableSlot *access_age_adjacency_match_scan(ScanState *node)
{
    AgeAdjacencyMatchScanState *state =
        (AgeAdjacencyMatchScanState *)node;
    TupleTableSlot *slot = node->ss_ScanTupleSlot;

    if (!state->scanned)
    {
        ExprContext *econtext = node->ps.ps_ExprContext;
        MemoryContext oldcontext;
        Datum key_datum;
        bool isnull = false;

        state->scanned = true;
        MemoryContextReset(state->scan_context);
        state->tuples = NIL;
        state->next_tuple = NULL;

        if (state->key_expr_state == NULL)
            return ExecClearTuple(slot);

        key_datum = ExecEvalExpr(state->key_expr_state, econtext, &isnull);
        if (!isnull)
        {
            state->current_key = DATUM_GET_GRAPHID(key_datum);
            oldcontext = MemoryContextSwitchTo(state->payload_scan_context);
            age_adjacency_visible_payload_scan_foreach(
                state->payload_scan, state->current_key,
                store_age_adjacency_match_tuple,
                state);
            MemoryContextSwitchTo(oldcontext);
            state->next_tuple = list_head(state->tuples);
        }
    }

    if (state->next_tuple != NULL)
    {
        AgeAdjacencyMatchTuple *tuple = lfirst(state->next_tuple);
        int i;

        ExecClearTuple(slot);

        if (state->scan_nattrs > 0)
        {
            for (i = 0; i < state->scan_nattrs; i++)
            {
                switch (state->scan_attnos[i])
                {
                case Anum_ag_label_edge_table_id:
                    slot->tts_values[i] = GRAPHID_GET_DATUM(tuple->edge_id);
                    slot->tts_isnull[i] = false;
                    break;
                case Anum_ag_label_edge_table_start_id:
                    slot->tts_values[i] = GRAPHID_GET_DATUM(tuple->start_id);
                    slot->tts_isnull[i] = false;
                    break;
                case Anum_ag_label_edge_table_end_id:
                    slot->tts_values[i] = GRAPHID_GET_DATUM(tuple->end_id);
                    slot->tts_isnull[i] = false;
                    break;
                case Anum_ag_label_edge_table_properties:
                    slot->tts_values[i] = tuple->properties;
                    slot->tts_isnull[i] = tuple->properties_isnull;
                    break;
                default:
                    elog(ERROR, "unexpected AGE adjacency match attr %d",
                         state->scan_attnos[i]);
                }
            }
        }
        else
        {
            slot->tts_values[Anum_ag_label_edge_table_id - 1] =
                GRAPHID_GET_DATUM(tuple->edge_id);
            slot->tts_isnull[Anum_ag_label_edge_table_id - 1] = false;
            slot->tts_values[Anum_ag_label_edge_table_start_id - 1] =
                GRAPHID_GET_DATUM(tuple->start_id);
            slot->tts_isnull[Anum_ag_label_edge_table_start_id - 1] = false;
            slot->tts_values[Anum_ag_label_edge_table_end_id - 1] =
                GRAPHID_GET_DATUM(tuple->end_id);
            slot->tts_isnull[Anum_ag_label_edge_table_end_id - 1] = false;
            slot->tts_values[Anum_ag_label_edge_table_properties - 1] =
                tuple->properties;
            slot->tts_isnull[Anum_ag_label_edge_table_properties - 1] =
                tuple->properties_isnull;
        }

        ExecStoreVirtualTuple(slot);

        state->next_tuple = lnext(state->tuples, state->next_tuple);
        return slot;
    }

    return ExecClearTuple(slot);
}

static bool recheck_age_adjacency_match_scan(ScanState *node,
                                             TupleTableSlot *slot)
{
    (void) node;
    (void) slot;

    return true;
}

static void end_age_adjacency_match_scan(CustomScanState *node)
{
    AgeAdjacencyMatchScanState *state =
        (AgeAdjacencyMatchScanState *)node;

    if (state->payload_scan != NULL)
    {
        age_adjacency_end_visible_payload_scan(state->payload_scan);
        state->payload_scan = NULL;
    }
    if (state->scan_context != NULL)
    {
        MemoryContextDelete(state->scan_context);
        state->scan_context = NULL;
    }
}

static void rescan_age_adjacency_match_scan(CustomScanState *node)
{
    AgeAdjacencyMatchScanState *state =
        (AgeAdjacencyMatchScanState *)node;

    if (state->scan_context != NULL)
        MemoryContextReset(state->scan_context);
    state->tuples = NIL;
    state->next_tuple = NULL;
    state->scanned = false;
}

static bool store_age_adjacency_match_tuple(
    const AgeAdjacencyPayload *payload, void *callback_state)
{
    AgeAdjacencyMatchScanState *state =
        (AgeAdjacencyMatchScanState *)callback_state;
    AgeAdjacencyMatchTuple *tuple;
    MemoryContext oldcontext;

    oldcontext = MemoryContextSwitchTo(state->scan_context);
    tuple = palloc0(sizeof(*tuple));

    tuple->edge_id = payload->edge_id;
    if (state->outgoing)
    {
        tuple->start_id = state->current_key;
        tuple->end_id = payload->next_vertex_id;
    }
    else
    {
        tuple->start_id = payload->next_vertex_id;
        tuple->end_id = state->current_key;
    }
    tuple->properties_isnull = payload->properties_isnull;
    if (!payload->properties_isnull)
        tuple->properties = datumCopy(payload->properties, false, -1);

    state->tuples = lappend(state->tuples, tuple);
    MemoryContextSwitchTo(oldcontext);

    return true;
}
