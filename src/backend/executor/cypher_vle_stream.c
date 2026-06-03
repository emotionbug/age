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

#include "executor/cypher_vle_stream.h"
#include "executor/executor.h"
#include "nodes/execnodes.h"
#include "utils/memutils.h"
#include "utils/tuplestore.h"

typedef struct AgeVLEStreamScanState
{
    CustomScanState css;
    SetExprState *setexpr;
    MemoryContext argcontext;
    bool exhausted;
} AgeVLEStreamScanState;

static Node *create_age_vle_stream_scan_state(CustomScan *cscan);
static void begin_age_vle_stream_scan(CustomScanState *node, EState *estate,
                                      int eflags);
static TupleTableSlot *exec_age_vle_stream_scan(CustomScanState *node);
static TupleTableSlot *access_age_vle_stream_scan(ScanState *node);
static bool recheck_age_vle_stream_scan(ScanState *node, TupleTableSlot *slot);
static void end_age_vle_stream_scan(CustomScanState *node);
static void rescan_age_vle_stream_scan(CustomScanState *node);
static void reset_age_vle_stream_setexpr(AgeVLEStreamScanState *state);

const CustomScanMethods age_vle_stream_scan_methods = {
    AGE_VLE_STREAM_SCAN_NAME,
    create_age_vle_stream_scan_state};

static const CustomExecMethods age_vle_stream_exec_methods = {
    AGE_VLE_STREAM_SCAN_NAME,
    begin_age_vle_stream_scan,
    exec_age_vle_stream_scan,
    end_age_vle_stream_scan,
    rescan_age_vle_stream_scan,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL};

static Node *create_age_vle_stream_scan_state(CustomScan *cscan)
{
    AgeVLEStreamScanState *state;

    state = palloc0(sizeof(*state));
    state->css.ss.ps.type = T_CustomScanState;
    state->css.flags = cscan->flags;
    state->css.methods = &age_vle_stream_exec_methods;

    return (Node *)state;
}

static void begin_age_vle_stream_scan(CustomScanState *node, EState *estate,
                                      int eflags)
{
    AgeVLEStreamScanState *state = (AgeVLEStreamScanState *)node;
    Expr *funcexpr;

    (void) estate;
    (void) eflags;

    Assert(list_length(((CustomScan *)node->ss.ps.plan)->custom_exprs) == 1);
    funcexpr = (Expr *)linitial(((CustomScan *)node->ss.ps.plan)->custom_exprs);

    state->setexpr = ExecInitFunctionResultSet(funcexpr,
                                               node->ss.ps.ps_ExprContext,
                                               &node->ss.ps);
    state->argcontext = AllocSetContextCreate(CurrentMemoryContext,
                                              "AGE VLE stream arguments",
                                              ALLOCSET_DEFAULT_SIZES);
    state->exhausted = false;
}

static TupleTableSlot *exec_age_vle_stream_scan(CustomScanState *node)
{
    return ExecScan(&node->ss, access_age_vle_stream_scan,
                    recheck_age_vle_stream_scan);
}

static TupleTableSlot *access_age_vle_stream_scan(ScanState *node)
{
    AgeVLEStreamScanState *state = (AgeVLEStreamScanState *)node;
    ExprContext *econtext = node->ps.ps_ExprContext;
    TupleTableSlot *slot = node->ss_ScanTupleSlot;
    ExprDoneCond isdone;
    MemoryContext oldcontext;
    Datum result;
    bool isnull;

    if (state->exhausted)
        return ExecClearTuple(slot);

    ExecClearTuple(slot);
    ResetExprContext(econtext);

    oldcontext = MemoryContextSwitchTo(state->argcontext);
    result = ExecMakeFunctionResultSet(state->setexpr, econtext,
                                       state->argcontext, &isnull, &isdone);
    MemoryContextSwitchTo(oldcontext);

    if (isdone == ExprEndResult)
    {
        state->exhausted = true;
        return ExecClearTuple(slot);
    }

    if (slot->tts_tupleDescriptor->natts > 0)
    {
        slot->tts_values[0] = result;
        slot->tts_isnull[0] = isnull;
    }
    ExecStoreVirtualTuple(slot);

    return slot;
}

static bool recheck_age_vle_stream_scan(ScanState *node, TupleTableSlot *slot)
{
    (void)node;
    (void)slot;

    return true;
}

static void end_age_vle_stream_scan(CustomScanState *node)
{
    AgeVLEStreamScanState *state = (AgeVLEStreamScanState *)node;

    ReScanExprContext(node->ss.ps.ps_ExprContext);
    reset_age_vle_stream_setexpr(state);
}

static void rescan_age_vle_stream_scan(CustomScanState *node)
{
    AgeVLEStreamScanState *state = (AgeVLEStreamScanState *)node;

    ReScanExprContext(node->ss.ps.ps_ExprContext);
    reset_age_vle_stream_setexpr(state);
    if (state->argcontext != NULL)
        MemoryContextReset(state->argcontext);
    state->exhausted = false;
}

static void reset_age_vle_stream_setexpr(AgeVLEStreamScanState *state)
{
    if (state->setexpr == NULL)
        return;

    if (state->setexpr->funcResultStore != NULL)
    {
        tuplestore_end(state->setexpr->funcResultStore);
        state->setexpr->funcResultStore = NULL;
    }
    state->setexpr->setArgsValid = false;
}
