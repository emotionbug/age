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
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#include "postgres.h"

#include "utils/age_global_graph.h"
#include "utils/age_vle_root.h"

static int64 get_matching_initial_edge_count(GraphEdgeAdjList *edges);
static void init_source_candidates_from_indexes(
    VLETraversalSourceCandidates *candidates,
    const VLETraversalSourceIndexes *indexes);
static Oid get_source_index_oid(
    const VLETraversalSourceIndexes *indexes,
    VLETraversalSourceKind kind, bool outgoing);
static bool traversal_source_candidate_available(
    const VLETraversalSourceCandidates *candidates,
    VLETraversalSourceKind kind, bool outgoing);

cypher_rel_dir reverse_vle_edge_direction(cypher_rel_dir edge_direction)
{
    switch (edge_direction)
    {
        case CYPHER_REL_DIR_RIGHT:
            return CYPHER_REL_DIR_LEFT;

        case CYPHER_REL_DIR_LEFT:
            return CYPHER_REL_DIR_RIGHT;

        case CYPHER_REL_DIR_NONE:
            return CYPHER_REL_DIR_NONE;

        default:
            elog(ERROR, "reverse_vle_edge_direction: unknown edge direction");
    }

    return CYPHER_REL_DIR_NONE;
}

void init_vle_root_descriptor_from_setup(
    VLETraversalRootDescriptor *root,
    const VLETraversalSetup *setup, GRAPH_global_context *ggctx,
    const VLETraversalRootSelectionInput *selection_input,
    const VLETraversalSourceLayoutInput *source_layout_input)
{
    const VLETraversalShape *shape;

    Assert(root != NULL);
    Assert(setup != NULL);
    Assert(ggctx != NULL);
    Assert(selection_input != NULL);
    Assert(source_layout_input != NULL);

    shape = &setup->shape;
    root->path_function = VLE_FUNCTION_PATHS_BETWEEN;
    root->start_vertex_id = 0;
    root->end_vertex_id = 0;
    root->direction = shape->direction;
    root->reverse_paths_to = false;
    root->reverse_output_path = false;
    root->next_vertex = peek_stack_head(get_graph_vertices(ggctx));
    if (root->next_vertex == NULL &&
        setup->graph_load.source_policy.load_policy.load_vertex_metadata)
    {
        elog(ERROR, "age_vle: empty graph");
    }

    if (!shape->initial_start_valid)
    {
        if (shape->initial_end_valid)
        {
            root->path_function = VLE_FUNCTION_PATHS_TO;
            root->start_vertex_id = shape->initial_veid;
            root->next_vertex = NULL;
        }
        else
        {
            if (root->next_vertex == NULL)
                elog(ERROR, "age_vle: empty graph");

            root->path_function = VLE_FUNCTION_PATHS_TO;
            root->start_vertex_id = get_graphid(root->next_vertex);
            root->next_vertex = next_GraphIdNode(root->next_vertex);
        }
    }
    else
    {
        root->start_vertex_id = shape->initial_vsid;
    }

    if (!shape->initial_end_valid)
    {
        if (root->path_function == VLE_FUNCTION_PATHS_TO)
            root->path_function = VLE_FUNCTION_PATHS_ALL;
        else
            root->path_function = VLE_FUNCTION_PATHS_FROM;
        root->end_vertex_id = 0;
    }
    else
    {
        if (root->path_function != VLE_FUNCTION_PATHS_TO)
            root->path_function = VLE_FUNCTION_PATHS_BETWEEN;
        root->end_vertex_id = shape->initial_veid;
    }

    if (root->path_function == VLE_FUNCTION_PATHS_TO)
    {
        root->reverse_paths_to = true;
        root->reverse_output_path = true;
        root->start_vertex_id = root->end_vertex_id;
        root->next_vertex = NULL;
        root->direction = reverse_vle_edge_direction(root->direction);
    }
    else if (!selection_input->empty_length_range &&
             !selection_input->zero_length_only &&
             root->path_function == VLE_FUNCTION_PATHS_BETWEEN)
    {
        int64 start_edge_count;
        int64 end_edge_count;

        start_edge_count = get_vle_initial_edge_count(
            selection_input, root->start_vertex_id, root->direction);
        end_edge_count = get_vle_initial_edge_count(
            selection_input, root->end_vertex_id,
            reverse_vle_edge_direction(root->direction));

        if (end_edge_count < start_edge_count)
        {
            graphid original_start_id = root->start_vertex_id;

            root->start_vertex_id = root->end_vertex_id;
            root->end_vertex_id = original_start_id;
            root->direction = reverse_vle_edge_direction(root->direction);
            root->reverse_output_path = true;
        }
    }

    init_vle_traversal_source_layout(&root->source_layout,
                                     source_layout_input, root->direction);
}

bool init_vle_root_descriptor_from_refresh(
    VLETraversalRootDescriptor *root,
    const VLETraversalRootDescriptor *current_root,
    const VLEContextRefreshInput *refresh,
    const VLETraversalSourceLayoutInput *source_layout_input)
{
    Assert(root != NULL);
    Assert(current_root != NULL);
    Assert(refresh != NULL);
    Assert(source_layout_input != NULL);

    *root = *current_root;

    if (!refresh->start_valid)
    {
        if (!root->reverse_paths_to)
        {
            if (root->next_vertex == NULL)
                return false;
            root->start_vertex_id = get_graphid(root->next_vertex);
            root->next_vertex = next_GraphIdNode(root->next_vertex);
        }
    }
    else
    {
        root->start_vertex_id = refresh->start_vertex_id;
    }

    if (!refresh->end_valid)
    {
        root->end_vertex_id = 0;
    }
    else
    {
        root->end_vertex_id = refresh->end_vertex_id;
        if (root->reverse_paths_to)
        {
            root->start_vertex_id = refresh->end_vertex_id;
            root->next_vertex = NULL;
        }
    }

    if (!root->reverse_paths_to && root->reverse_output_path)
    {
        graphid original_start_id = root->start_vertex_id;

        root->start_vertex_id = root->end_vertex_id;
        root->end_vertex_id = original_start_id;
    }
    init_vle_traversal_source_layout(&root->source_layout,
                                     source_layout_input, root->direction);

    return true;
}

void init_vle_traversal_source_layout(
    VLETraversalSourceLayout *layout,
    const VLETraversalSourceLayoutInput *input,
    cypher_rel_dir direction)
{
    const VLETraversalSourceIndexes *indexes;
    VLETraversalSourceCandidates candidates;
    VLETraversalSourceLayoutDecision decision;

    Assert(layout != NULL);
    Assert(input != NULL);
    Assert(input->indexes != NULL);

    indexes = input->indexes;
    layout->direction = direction;
    layout->use_local_edge_state = input->use_local_edge_state;
    layout->label_constrained = input->label_constrained;
    layout->has_property_constraints = input->has_property_constraints;
    layout->outgoing_source.kind = VLE_TRAVERSAL_SOURCE_NONE;
    layout->outgoing_source.index_oid = InvalidOid;
    layout->incoming_source.kind = VLE_TRAVERSAL_SOURCE_NONE;
    layout->incoming_source.index_oid = InvalidOid;

    if (!layout->label_constrained)
        return;

    init_source_candidates_from_indexes(&candidates, indexes);
    select_vle_traversal_source_layout(&decision, input, &candidates);

    layout->outgoing_source.kind = decision.outgoing_kind;
    layout->outgoing_source.index_oid =
        get_source_index_oid(indexes, decision.outgoing_kind, true);
    layout->incoming_source.kind = decision.incoming_kind;
    layout->incoming_source.index_oid =
        get_source_index_oid(indexes, decision.incoming_kind, false);
}

void select_vle_traversal_source_layout(
    VLETraversalSourceLayoutDecision *decision,
    const VLETraversalSourceLayoutInput *input,
    const VLETraversalSourceCandidates *candidates)
{
    bool prefer_outgoing_endpoint;
    bool prefer_incoming_endpoint;

    Assert(decision != NULL);
    Assert(input != NULL);
    Assert(candidates != NULL);

    decision->outgoing_kind = VLE_TRAVERSAL_SOURCE_NONE;
    decision->incoming_kind = VLE_TRAVERSAL_SOURCE_NONE;
    decision->prefer_endpoint_btree =
        input->use_local_edge_state &&
        !input->has_property_constraints &&
        !input->upper_infinite &&
        input->upper <= 1;

    if (!input->label_constrained)
        return;

    prefer_outgoing_endpoint =
        decision->prefer_endpoint_btree && candidates->endpoint_start;
    prefer_incoming_endpoint =
        decision->prefer_endpoint_btree && candidates->endpoint_end;

    if (decision->outgoing_kind == VLE_TRAVERSAL_SOURCE_NONE &&
        !prefer_outgoing_endpoint && candidates->age_adjacency_out)
    {
        decision->outgoing_kind = VLE_TRAVERSAL_SOURCE_AGE_ADJACENCY;
    }
    else if (input->use_local_edge_state && candidates->endpoint_start)
    {
        decision->outgoing_kind = VLE_TRAVERSAL_SOURCE_ENDPOINT_BTREE;
    }

    if (decision->incoming_kind == VLE_TRAVERSAL_SOURCE_NONE &&
        !prefer_incoming_endpoint && candidates->age_adjacency_in)
    {
        decision->incoming_kind = VLE_TRAVERSAL_SOURCE_AGE_ADJACENCY;
    }
    else if (input->use_local_edge_state && candidates->endpoint_end)
    {
        decision->incoming_kind = VLE_TRAVERSAL_SOURCE_ENDPOINT_BTREE;
    }

    if (input->preferred_source_known)
    {
        if (traversal_source_candidate_available(
                candidates, input->preferred_outgoing_kind, true))
        {
            decision->outgoing_kind = input->preferred_outgoing_kind;
        }
        if (traversal_source_candidate_available(
                candidates, input->preferred_incoming_kind, false))
        {
            decision->incoming_kind = input->preferred_incoming_kind;
        }
    }
}

static void init_source_candidates_from_indexes(
    VLETraversalSourceCandidates *candidates,
    const VLETraversalSourceIndexes *indexes)
{
    Assert(candidates != NULL);
    Assert(indexes != NULL);

    candidates->age_adjacency_out =
        OidIsValid(indexes->age_adjacency_out_index_oid);
    candidates->age_adjacency_in =
        OidIsValid(indexes->age_adjacency_in_index_oid);
    candidates->endpoint_start = OidIsValid(indexes->edge_start_index_oid);
    candidates->endpoint_end = OidIsValid(indexes->edge_end_index_oid);
}

static bool traversal_source_candidate_available(
    const VLETraversalSourceCandidates *candidates,
    VLETraversalSourceKind kind, bool outgoing)
{
    Assert(candidates != NULL);

    switch (kind)
    {
        case VLE_TRAVERSAL_SOURCE_NONE:
            return false;
        case VLE_TRAVERSAL_SOURCE_AGE_ADJACENCY:
            return outgoing ? candidates->age_adjacency_out :
                candidates->age_adjacency_in;
        case VLE_TRAVERSAL_SOURCE_ENDPOINT_BTREE:
            return outgoing ? candidates->endpoint_start :
                candidates->endpoint_end;
    }

    return false;
}

static Oid get_source_index_oid(
    const VLETraversalSourceIndexes *indexes,
    VLETraversalSourceKind kind, bool outgoing)
{
    Assert(indexes != NULL);

    switch (kind)
    {
        case VLE_TRAVERSAL_SOURCE_AGE_ADJACENCY:
            return outgoing ? indexes->age_adjacency_out_index_oid :
                              indexes->age_adjacency_in_index_oid;
        case VLE_TRAVERSAL_SOURCE_ENDPOINT_BTREE:
            return outgoing ? indexes->edge_start_index_oid :
                              indexes->edge_end_index_oid;
        case VLE_TRAVERSAL_SOURCE_NONE:
            return InvalidOid;
    }

    return InvalidOid;
}

int64 get_vle_initial_edge_count(
    const VLETraversalRootSelectionInput *input, graphid vertex_id,
    cypher_rel_dir edge_direction)
{
    vertex_entry *ve = NULL;
    int64 count = 0;
    GraphEdgeAdjList *edges = NULL;

    Assert(input != NULL);
    Assert(input->ggctx != NULL);

    ve = get_vertex_entry(input->ggctx, vertex_id);
    if (ve == NULL)
    {
        return 0;
    }

    if (edge_direction == CYPHER_REL_DIR_RIGHT ||
        edge_direction == CYPHER_REL_DIR_NONE)
    {
        if (input->edge_label_oid != InvalidOid)
        {
            edges = get_vertex_entry_adj_edges_out_for_label(
                ve, input->edge_label_oid);
        }
        else
        {
            edges = get_vertex_entry_adj_edges_out(ve);
        }
        count += get_matching_initial_edge_count(edges);
    }
    if (edge_direction == CYPHER_REL_DIR_LEFT ||
        edge_direction == CYPHER_REL_DIR_NONE)
    {
        if (input->edge_label_oid != InvalidOid)
        {
            edges = get_vertex_entry_adj_edges_in_for_label(
                ve, input->edge_label_oid);
        }
        else
        {
            edges = get_vertex_entry_adj_edges_in(ve);
        }
        count += get_matching_initial_edge_count(edges);
    }

    if (input->edge_label_oid != InvalidOid)
    {
        edges = get_vertex_entry_adj_edges_self_for_label(
            ve, input->edge_label_oid);
    }
    else
    {
        edges = get_vertex_entry_adj_edges_self(ve);
    }
    count += get_matching_initial_edge_count(edges);

    return count;
}

static int64 get_matching_initial_edge_count(GraphEdgeAdjList *edges)
{
    if (edges == NULL)
    {
        return 0;
    }

    return edges->size;
}
