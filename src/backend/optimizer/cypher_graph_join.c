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

#include "catalog/pg_collation_d.h"
#include "catalog/pg_type_d.h"
#include "nodes/makefuncs.h"
#include "optimizer/cypher_graph_join.h"
#include "utils/builtins.h"

static char *copy_graph_join_text(const char *value,
                                  const char *fallback);
static Const *make_graph_join_text_const(const char *value);

AgeGraphJoinCandidate *age_graph_join_make_candidate(
    const AgeGraphJoinCandidateRequest *request)
{
    AgeGraphJoinCandidate *candidate;

    Assert(request != NULL);

    candidate = palloc0(sizeof(*candidate));
    candidate->component.name =
        copy_graph_join_text(request->component, "unknown");
    candidate->component.required_outer = bms_copy(request->required_outer);
    candidate->component.provided_relids = bms_copy(request->provided_relids);
    candidate->component.estimated_rows = request->rows;
    candidate->component.output_width = request->output_width;
    candidate->component.parallel_safe = request->parallel_safe;
    candidate->component.parallel_aware = request->parallel_aware;
    candidate->component.parallel_workers = request->parallel_workers;
    candidate->component.gather_cost = request->gather_cost;
    candidate->component.order_preserving = request->order_preserving;
    candidate->component.shared_state_required =
        request->shared_state_required;

    candidate->connector.kind =
        copy_graph_join_text(request->connector, "unknown");
    candidate->connector.bound =
        copy_graph_join_text(request->bound, "unknown");
    candidate->connector.order_property =
        copy_graph_join_text(request->order_property, "query-order");
    candidate->connector.source_evidence =
        copy_graph_join_text(request->source_evidence, "unknown");
    candidate->connector.rows = request->rows;
    candidate->connector.startup_cost = request->startup_cost;
    candidate->connector.total_cost = request->total_cost;

    return candidate;
}

AgeGraphJoinCandidateTable *age_graph_join_make_candidate_table(void)
{
    AgeGraphJoinCandidateTable *table;

    table = palloc0(sizeof(*table));

    return table;
}

AgeGraphJoinCandidate *age_graph_join_table_add_candidate(
    AgeGraphJoinCandidateTable *table,
    const AgeGraphJoinCandidateRequest *request)
{
    AgeGraphJoinCandidate *candidate;

    Assert(table != NULL);

    candidate = age_graph_join_make_candidate(request);
    table->candidates = lappend(table->candidates, candidate);

    return candidate;
}

AgeGraphJoinCandidate *age_graph_join_table_add_path_candidate(
    AgeGraphJoinCandidateTable *table, Path *path, const char *component,
    const char *connector, const char *bound, const char *order_property,
    const char *source_evidence, bool shared_state_required)
{
    AgeGraphJoinCandidateRequest request;
    Relids provided_relids = NULL;
    int output_width = 0;

    Assert(path != NULL);

    memset(&request, 0, sizeof(request));
    if (path->parent != NULL)
    {
        provided_relids = path->parent->relids;
        if (path->parent->reltarget != NULL)
            output_width = path->parent->reltarget->width;
    }
    if (path->pathtarget != NULL && path->pathtarget->width > 0)
        output_width = path->pathtarget->width;

    request.component = component;
    request.connector = connector;
    request.bound = bound;
    request.order_property = order_property;
    request.source_evidence = source_evidence;
    request.required_outer = PATH_REQ_OUTER(path);
    request.provided_relids = provided_relids;
    request.rows = path->rows;
    request.startup_cost = path->startup_cost;
    request.total_cost = path->total_cost;
    request.output_width = output_width;
    request.parallel_safe = path->parallel_safe;
    request.parallel_aware = path->parallel_aware;
    request.parallel_workers = path->parallel_workers;
    request.gather_cost = 0;
    request.order_preserving = path->pathkeys != NIL;
    request.shared_state_required = shared_state_required;

    return age_graph_join_table_add_candidate(table, &request);
}

AgeGraphJoinCandidate *age_graph_join_table_select_cheapest(
    const AgeGraphJoinCandidateTable *table)
{
    AgeGraphJoinCandidate *selected = NULL;
    ListCell *lc;

    if (table == NULL)
        return NULL;

    foreach(lc, table->candidates)
    {
        AgeGraphJoinCandidate *candidate = lfirst(lc);

        if (selected == NULL ||
            candidate->connector.total_cost < selected->connector.total_cost)
        {
            selected = candidate;
        }
    }

    return selected;
}

List *age_graph_join_candidate_private(
    const AgeGraphJoinCandidate *candidate)
{
    List *descriptor = NIL;

    Assert(candidate != NULL);

    descriptor = lappend(descriptor,
                         make_graph_join_text_const(
                             candidate->component.name));
    descriptor = lappend(descriptor,
                         make_graph_join_text_const(
                             candidate->connector.kind));
    descriptor = lappend(descriptor,
                         make_graph_join_text_const(
                             candidate->connector.bound));
    descriptor = lappend(descriptor,
                         make_graph_join_text_const(
                             candidate->connector.order_property));
    descriptor = lappend(descriptor,
                         make_graph_join_text_const(
                             candidate->connector.source_evidence));
    descriptor = lappend(descriptor,
                         makeConst(FLOAT8OID, -1, InvalidOid,
                                   sizeof(float8),
                                   Float8GetDatum(candidate->connector.rows),
                                   false, true));
    descriptor = lappend(descriptor,
                         makeConst(FLOAT8OID, -1, InvalidOid,
                                   sizeof(float8),
                                   Float8GetDatum(
                                       candidate->connector.startup_cost),
                                   false, true));
    descriptor = lappend(descriptor,
                         makeConst(FLOAT8OID, -1, InvalidOid,
                                   sizeof(float8),
                                   Float8GetDatum(
                                       candidate->connector.total_cost),
                                   false, true));
    descriptor = lappend(descriptor,
                         makeConst(INT4OID, -1, InvalidOid, sizeof(int32),
                                   Int32GetDatum(
                                       candidate->component.output_width),
                                   false, true));
    descriptor = lappend(descriptor,
                         makeConst(BOOLOID, -1, InvalidOid, sizeof(bool),
                                   BoolGetDatum(
                                       candidate->component.parallel_safe),
                                   false, true));
    descriptor = lappend(descriptor,
                         makeConst(BOOLOID, -1, InvalidOid, sizeof(bool),
                                   BoolGetDatum(
                                       candidate->component.parallel_aware),
                                   false, true));
    descriptor = lappend(descriptor,
                         makeConst(INT4OID, -1, InvalidOid, sizeof(int32),
                                   Int32GetDatum(
                                       candidate->component.parallel_workers),
                                   false, true));
    descriptor = lappend(descriptor,
                         makeConst(FLOAT8OID, -1, InvalidOid,
                                   sizeof(float8),
                                   Float8GetDatum(
                                       candidate->component.gather_cost),
                                   false, true));
    descriptor = lappend(descriptor,
                         makeConst(BOOLOID, -1, InvalidOid, sizeof(bool),
                                   BoolGetDatum(
                                       candidate->component.order_preserving),
                                   false, true));
    descriptor = lappend(descriptor,
                         makeConst(BOOLOID, -1, InvalidOid, sizeof(bool),
                                   BoolGetDatum(
                                       candidate->component.shared_state_required),
                                   false, true));

    return descriptor;
}

List *age_graph_join_table_selected_private(
    const AgeGraphJoinCandidateTable *table)
{
    AgeGraphJoinCandidate *candidate;

    candidate = age_graph_join_table_select_cheapest(table);
    if (candidate == NULL)
        return NIL;

    return age_graph_join_candidate_private(candidate);
}

const char *age_graph_join_descriptor_text_field(List *descriptor, int index)
{
    Const *value;

    if (descriptor == NIL || list_length(descriptor) <= index)
        return NULL;

    value = list_nth_node(Const, descriptor, index);
    if (value->constisnull || value->consttype != TEXTOID)
        return NULL;

    return TextDatumGetCString(value->constvalue);
}

static char *copy_graph_join_text(const char *value, const char *fallback)
{
    if (value == NULL)
        value = fallback;

    return pstrdup(value);
}

static Const *make_graph_join_text_const(const char *value)
{
    return makeConst(TEXTOID, -1, DEFAULT_COLLATION_OID, -1,
                     CStringGetTextDatum(value != NULL ? value : "unknown"),
                     false, false);
}
