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
static const char *graph_join_component_from_evidence(
    const AgeGraphJoinPathEvidence *evidence);
static Const *make_graph_join_text_const(const char *value);
static void set_graph_join_descriptor_value(List *descriptor, int index,
                                            Node *value);

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

    age_graph_join_init_path_request(&request, path, component, connector,
                                     bound, order_property, source_evidence,
                                     shared_state_required);

    return age_graph_join_table_add_candidate(table, &request);
}

void age_graph_join_init_path_request(
    AgeGraphJoinCandidateRequest *request, Path *path, const char *component,
    const char *connector, const char *bound, const char *order_property,
    const char *source_evidence, bool shared_state_required)
{
    Relids provided_relids = NULL;
    int output_width = 0;

    Assert(request != NULL);
    Assert(path != NULL);

    memset(request, 0, sizeof(*request));
    if (path->parent != NULL)
    {
        provided_relids = path->parent->relids;
        if (path->parent->reltarget != NULL)
            output_width = path->parent->reltarget->width;
    }
    if (path->pathtarget != NULL && path->pathtarget->width > 0)
        output_width = path->pathtarget->width;

    request->component = component;
    request->connector = connector;
    request->bound = bound;
    request->order_property = order_property;
    request->source_evidence = source_evidence;
    request->required_outer = PATH_REQ_OUTER(path);
    request->provided_relids = provided_relids;
    request->rows = path->rows;
    request->startup_cost = path->startup_cost;
    request->total_cost = path->total_cost;
    request->output_width = output_width;
    request->parallel_safe = path->parallel_safe;
    request->parallel_aware = path->parallel_aware;
    request->parallel_workers = path->parallel_workers;
    request->gather_cost = 0;
    request->order_preserving = path->pathkeys != NIL;
    request->shared_state_required = shared_state_required;
}

AgeGraphJoinCandidate *age_graph_join_table_add_scheduled_candidate(
    AgeGraphJoinCandidateTable *table, Path *path, const char *component,
    const char *connector, const char *bound, const char *order_property,
    const char *source_evidence, double rows, Cost startup_cost,
    Cost total_cost, bool shared_state_required)
{
    AgeGraphJoinCandidateRequest request;

    age_graph_join_init_path_request(&request, path, component, connector,
                                     bound, order_property, source_evidence,
                                     shared_state_required);
    request.rows = rows;
    request.startup_cost = startup_cost;
    request.total_cost = total_cost;

    return age_graph_join_table_add_candidate(table, &request);
}

AgeGraphJoinCandidate *age_graph_join_table_add_path_evidence_candidate(
    AgeGraphJoinCandidateTable *table, Path *path, const char *component,
    const AgeGraphJoinPathEvidence *evidence)
{
    AgeGraphJoinCandidateRequest request;
    Cost total_cost;

    Assert(path != NULL);

    if (table == NULL ||
        evidence == NULL ||
        evidence->order_property == NULL)
    {
        return NULL;
    }

    age_graph_join_init_path_request(
        &request, path,
        component != NULL ? component :
        graph_join_component_from_evidence(evidence),
        evidence->connector != NULL ? evidence->connector : "graph-connector",
        evidence->bound ? "bound" : "unbound",
        evidence->order_property,
        evidence->source_evidence != NULL ?
        evidence->source_evidence : "path-evidence",
        false);

    total_cost = evidence->selected_total_cost > 0 ?
        evidence->selected_total_cost : path->total_cost;
    request.total_cost = total_cost;
    request.rows = path->rows;

    return age_graph_join_table_add_candidate(table, &request);
}

static const char *graph_join_component_from_evidence(
    const AgeGraphJoinPathEvidence *evidence)
{
    const char *connector;
    const char *order_property;

    if (evidence == NULL)
        return "graph-component";

    connector = evidence->connector;
    order_property = evidence->order_property;

    if (connector != NULL &&
        (strcmp(connector, "postgres-index-seek") == 0 ||
         strcmp(connector, "node-property-index-seek") == 0))
    {
        return "node-property-seek";
    }

    if (connector != NULL &&
        (strncmp(connector, "vle-", 4) == 0 ||
         strcmp(connector, "matrix-frontier-expand") == 0))
    {
        return "vle-expansion";
    }

    if (connector != NULL &&
        (strncmp(connector, "adjacency-", 10) == 0 ||
         strcmp(connector, "age-adjacency") == 0))
    {
        return "adjacency-expansion";
    }

    if (order_property != NULL &&
        strcmp(order_property, "index-anchored") == 0)
        return "node-property-seek";
    if (order_property != NULL &&
        (strcmp(order_property, "vle-frontier-anchored") == 0 ||
         strcmp(order_property, "matrix-frontier-anchored") == 0 ||
         strcmp(order_property, "expand-into-verification") == 0))
    {
        return "vle-expansion";
    }
    if (order_property != NULL &&
        (strcmp(order_property, "adjacency-directory-anchored") == 0 ||
         strcmp(order_property, "adjacency-anchored") == 0))
    {
        return "adjacency-expansion";
    }

    return "graph-component";
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

bool age_graph_join_apply_selected_path_cost(
    const AgeGraphJoinCandidateTable *table, Path *path)
{
    AgeGraphJoinCandidate *selected;

    Assert(path != NULL);

    selected = age_graph_join_table_select_cheapest(table);
    if (selected == NULL)
        return false;

    path->rows = selected->connector.rows;
    path->startup_cost = selected->connector.startup_cost;
    path->total_cost = selected->connector.total_cost;

    return true;
}

bool age_graph_join_order_property_is_bound(const char *order_property)
{
    if (order_property == NULL)
        return false;

    return strcmp(order_property, "index-anchored") == 0 ||
           strcmp(order_property, "adjacency-directory-anchored") == 0 ||
           strcmp(order_property, "adjacency-anchored") == 0 ||
           strcmp(order_property, "vle-frontier-anchored") == 0 ||
           strcmp(order_property, "expand-into-verification") == 0 ||
           strcmp(order_property, "matrix-frontier-anchored") == 0;
}

void age_graph_join_init_path_evidence(
    AgeGraphJoinPathEvidence *evidence)
{
    Assert(evidence != NULL);

    memset(evidence, 0, sizeof(*evidence));
    evidence->candidate_count = 1;
}

double age_graph_join_path_evidence_credit(
    const AgeGraphJoinPathEvidence *outer_evidence,
    const AgeGraphJoinPathEvidence *inner_evidence)
{
    double credit = 1.0;

    if (outer_evidence != NULL &&
        outer_evidence->candidate_count > 1 &&
        outer_evidence->selected_total_cost > 0 &&
        outer_evidence->next_total_cost > outer_evidence->selected_total_cost)
    {
        credit = Min(credit,
                     Max(0.25,
                         outer_evidence->selected_total_cost /
                         outer_evidence->next_total_cost));
    }
    if (inner_evidence != NULL &&
        inner_evidence->candidate_count > 1 &&
        inner_evidence->selected_total_cost > 0 &&
        inner_evidence->next_total_cost > inner_evidence->selected_total_cost)
    {
        credit = Min(credit,
                     Max(0.25,
                         inner_evidence->selected_total_cost /
                         inner_evidence->next_total_cost));
    }

    return credit;
}

int age_graph_join_table_candidate_count(
    const AgeGraphJoinCandidateTable *table)
{
    if (table == NULL)
        return 0;

    return list_length(table->candidates);
}

AgeGraphJoinCandidate *age_graph_join_table_select_next_best(
    const AgeGraphJoinCandidateTable *table,
    const AgeGraphJoinCandidate *selected)
{
    AgeGraphJoinCandidate *next_best = NULL;
    ListCell *lc;

    if (table == NULL || selected == NULL)
        return NULL;

    foreach(lc, table->candidates)
    {
        AgeGraphJoinCandidate *candidate = lfirst(lc);

        if (candidate == selected)
            continue;
        if (next_best == NULL ||
            candidate->connector.total_cost < next_best->connector.total_cost)
        {
            next_best = candidate;
        }
    }

    return next_best;
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
    descriptor = lappend(descriptor,
                         makeConst(INT4OID, -1, InvalidOid, sizeof(int32),
                                   Int32GetDatum(1), false, true));
    descriptor = lappend(descriptor, make_graph_join_text_const("none"));
    descriptor = lappend(descriptor, make_graph_join_text_const("none"));
    descriptor = lappend(descriptor, make_graph_join_text_const("none"));
    descriptor = lappend(descriptor,
                         makeConst(FLOAT8OID, -1, InvalidOid,
                                   sizeof(float8), Float8GetDatum(0),
                                   false, true));
    descriptor = lappend(descriptor,
                         makeConst(FLOAT8OID, -1, InvalidOid,
                                   sizeof(float8), Float8GetDatum(0),
                                   false, true));

    return descriptor;
}

List *age_graph_join_table_selected_private(
    const AgeGraphJoinCandidateTable *table)
{
    AgeGraphJoinCandidate *candidate;
    AgeGraphJoinCandidate *next_best;
    List *descriptor;

    candidate = age_graph_join_table_select_cheapest(table);
    if (candidate == NULL)
        return NIL;

    descriptor = age_graph_join_candidate_private(candidate);
    next_best = age_graph_join_table_select_next_best(table, candidate);

    set_graph_join_descriptor_value(
        descriptor, AGE_GRAPH_JOIN_DESC_CANDIDATE_COUNT,
        (Node *)makeConst(INT4OID, -1, InvalidOid, sizeof(int32),
                          Int32GetDatum(
                              age_graph_join_table_candidate_count(table)),
                          false, true));
    if (next_best != NULL)
    {
        set_graph_join_descriptor_value(
            descriptor, AGE_GRAPH_JOIN_DESC_NEXT_CONNECTOR,
            (Node *)make_graph_join_text_const(next_best->connector.kind));
        set_graph_join_descriptor_value(
            descriptor, AGE_GRAPH_JOIN_DESC_NEXT_ORDER_PROPERTY,
            (Node *)make_graph_join_text_const(
                next_best->connector.order_property));
        set_graph_join_descriptor_value(
            descriptor, AGE_GRAPH_JOIN_DESC_NEXT_SOURCE_EVIDENCE,
            (Node *)make_graph_join_text_const(
                next_best->connector.source_evidence));
        set_graph_join_descriptor_value(
            descriptor, AGE_GRAPH_JOIN_DESC_NEXT_ROWS,
            (Node *)makeConst(FLOAT8OID, -1, InvalidOid, sizeof(float8),
                              Float8GetDatum(next_best->connector.rows),
                              false, true));
        set_graph_join_descriptor_value(
            descriptor, AGE_GRAPH_JOIN_DESC_NEXT_TOTAL_COST,
            (Node *)makeConst(FLOAT8OID, -1, InvalidOid, sizeof(float8),
                              Float8GetDatum(
                                  next_best->connector.total_cost),
                              false, true));
    }

    return descriptor;
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

int64 age_graph_join_descriptor_int_field(List *descriptor, int index,
                                          int64 fallback)
{
    Const *value;

    if (descriptor == NIL || list_length(descriptor) <= index)
        return fallback;

    value = list_nth_node(Const, descriptor, index);
    if (value->constisnull)
        return fallback;
    if (value->consttype == INT4OID)
        return (int64)DatumGetInt32(value->constvalue);
    if (value->consttype == INT8OID)
        return DatumGetInt64(value->constvalue);
    if (value->consttype == OIDOID)
        return (int64)DatumGetObjectId(value->constvalue);

    return fallback;
}

double age_graph_join_descriptor_float_field(List *descriptor, int index,
                                             double fallback)
{
    Const *value;

    if (descriptor == NIL || list_length(descriptor) <= index)
        return fallback;

    value = list_nth_node(Const, descriptor, index);
    if (value->constisnull)
        return fallback;
    if (value->consttype == FLOAT8OID)
        return DatumGetFloat8(value->constvalue);
    if (value->consttype == INT4OID)
        return (double)DatumGetInt32(value->constvalue);
    if (value->consttype == INT8OID)
        return (double)DatumGetInt64(value->constvalue);

    return fallback;
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

static void set_graph_join_descriptor_value(List *descriptor, int index,
                                            Node *value)
{
    ListCell *cell;

    Assert(descriptor != NIL);
    Assert(value != NULL);
    Assert(list_length(descriptor) > index);

    cell = list_nth_cell(descriptor, index);
    lfirst(cell) = value;
}
