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

#include "commands/explain_format.h"
#include "commands/explain_state.h"
#include "executor/cypher_generic_join.h"
#include "executor/executor.h"
#include "lib/stringinfo.h"
#include "nodes/pg_list.h"
#include "utils/graphid.h"
#include "utils/memutils.h"

typedef struct AgeGenericRow
{
    graphid key1;
    graphid key2;
    graphid edge_id;
    bool edge_id_valid;
    MinimalTuple tuple;
} AgeGenericRow;

typedef struct AgeGenericProvider
{
    AgeGenericProviderKind kind;
    int var1;
    int var2;
    AttrNumber key1_attno;
    AttrNumber key2_attno;
    AttrNumber edge_id_attno;
    PlanState *plan_state;
    TupleTableSlot *tuple_slot;
    AgeGenericRow *rows;
    int row_count;
    int row_capacity;
    int bag_start;
    int bag_count;
} AgeGenericProvider;

typedef struct AgeGenericLevel
{
    MemoryContext context;
    graphid *candidates;
    int candidate_count;
    int next_candidate;
    bool initialized;
} AgeGenericLevel;

typedef struct AgeGenericDomain
{
    graphid *keys;
    int count;
} AgeGenericDomain;

typedef struct AgeGenericJoinState
{
    CustomScanState css;
    int variable_count;
    Index *variable_rtis;
    int provider_count;
    AgeGenericProvider *providers;
    AgeGenericLevel *levels;
    graphid *bindings;
    bool search_started;
    bool binding_ready;
    int *combination_indexes;
    int *enumeration_order;
    graphid *selected_edge_ids;
    bool *selected_edge_id_valid;
    Bitmapset **uniqueness_groups;
    int uniqueness_group_count;
    bool combination_started;
    MemoryContext data_context;
    bool materialized;
    bool exhausted;
    int64 rows_materialized;
    int64 semijoin_passes;
    int64 semijoin_rows_removed;
    int64 semijoin_provider_rows_after;
    int64 semijoin_final_domain_keys;
    int64 bindings_completed;
    int64 candidate_flat_rows;
    int64 candidate_combinations;
    int64 uniqueness_rejects;
    int64 rows_emitted;
    int64 rescans;
    Size peak_memory;
} AgeGenericJoinState;

static inline void
increment_generic_counter(int64 *counter)
{
    if (*counter < PG_INT64_MAX)
        (*counter)++;
}

static inline void
add_generic_counter(int64 *counter, int64 amount)
{
    if (amount <= 0)
        return;
    if (*counter > PG_INT64_MAX - amount)
        *counter = PG_INT64_MAX;
    else
        *counter += amount;
}

static Node *create_age_generic_join_state(CustomScan *cscan);
static void begin_age_generic_join(CustomScanState *node, EState *estate,
                                   int eflags);
static TupleTableSlot *exec_age_generic_join(CustomScanState *node);
static TupleTableSlot *access_age_generic_join(ScanState *node);
static bool recheck_age_generic_join(ScanState *node, TupleTableSlot *slot);
static void end_age_generic_join(CustomScanState *node);
static void rescan_age_generic_join(CustomScanState *node);
static void explain_age_generic_join(CustomScanState *node, List *ancestors,
                                     ExplainState *es);
static void reduce_generic_providers(AgeGenericJoinState *state);

const CustomScanMethods age_generic_join_scan_methods = {
    AGE_GENERIC_JOIN_SCAN_NAME,
    create_age_generic_join_state
};

static const CustomExecMethods age_generic_join_exec_methods = {
    AGE_GENERIC_JOIN_SCAN_NAME,
    begin_age_generic_join,
    exec_age_generic_join,
    end_age_generic_join,
    rescan_age_generic_join,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    explain_age_generic_join
};

static Node *
create_age_generic_join_state(CustomScan *cscan)
{
    AgeGenericJoinState *state;
    List *variable_rtis;
    List *provider_descs;
    List *uniqueness_group_descs;
    ListCell *lc;
    int index = 0;

    if (list_length(cscan->custom_private) !=
        AGE_GENERIC_JOIN_PRIVATE_COUNT)
    {
        elog(ERROR, "invalid AGE Generic Join private descriptor");
    }

    state = palloc0(sizeof(*state));
    state->css.ss.ps.type = T_CustomScanState;
    state->css.flags = cscan->flags;
    state->css.methods = &age_generic_join_exec_methods;
    state->variable_count = intVal(list_nth(
        cscan->custom_private, AGE_GENERIC_JOIN_PRIVATE_VARIABLE_COUNT));
    variable_rtis = list_nth(
        cscan->custom_private, AGE_GENERIC_JOIN_PRIVATE_VARIABLE_RTIS);
    provider_descs = list_nth(
        cscan->custom_private, AGE_GENERIC_JOIN_PRIVATE_PROVIDER_DESCS);
    uniqueness_group_descs = list_nth(
        cscan->custom_private,
        AGE_GENERIC_JOIN_PRIVATE_UNIQUENESS_GROUPS);
    if (state->variable_count < 2 ||
        list_length(variable_rtis) != state->variable_count ||
        provider_descs == NIL ||
        list_length(uniqueness_group_descs) != list_length(provider_descs))
    {
        elog(ERROR, "invalid AGE Generic Join arity");
    }

    state->variable_rtis = palloc0(sizeof(Index) * state->variable_count);
    foreach(lc, variable_rtis)
        state->variable_rtis[index++] = (Index)intVal(lfirst(lc));
    state->provider_count = list_length(provider_descs);

    return (Node *)state;
}

static int
compare_generic_rows(const void *left, const void *right)
{
    const AgeGenericRow *a = left;
    const AgeGenericRow *b = right;

    if (a->key1 < b->key1)
        return -1;
    if (a->key1 > b->key1)
        return 1;
    if (a->key2 < b->key2)
        return -1;
    if (a->key2 > b->key2)
        return 1;
    if (a->edge_id_valid && b->edge_id_valid)
    {
        if (a->edge_id < b->edge_id)
            return -1;
        if (a->edge_id > b->edge_id)
            return 1;
    }
    return 0;
}

static void
append_generic_row(AgeGenericJoinState *state, AgeGenericProvider *provider,
                   graphid key1, graphid key2, graphid edge_id,
                   bool edge_id_valid, TupleTableSlot *slot)
{
    MemoryContext oldcontext;

    oldcontext = MemoryContextSwitchTo(state->data_context);
    if (provider->row_count == provider->row_capacity)
    {
        int new_capacity = provider->row_capacity == 0 ? 128 :
            provider->row_capacity * 2;

        if (new_capacity <= provider->row_capacity ||
            (Size)new_capacity > MaxAllocSize / sizeof(AgeGenericRow))
        {
            ereport(ERROR,
                    (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                     errmsg("AGE Generic Join provider is too large")));
        }
        if (provider->rows == NULL)
        {
            provider->rows = palloc(sizeof(AgeGenericRow) * new_capacity);
        }
        else
        {
            provider->rows = repalloc(provider->rows,
                                      sizeof(AgeGenericRow) * new_capacity);
        }
        provider->row_capacity = new_capacity;
    }
    provider->rows[provider->row_count].key1 = key1;
    provider->rows[provider->row_count].key2 = key2;
    provider->rows[provider->row_count].edge_id = edge_id;
    provider->rows[provider->row_count].edge_id_valid = edge_id_valid;
    provider->rows[provider->row_count].tuple = ExecCopySlotMinimalTuple(slot);
    provider->row_count++;
    MemoryContextSwitchTo(oldcontext);

    increment_generic_counter(&state->rows_materialized);
}

static void
materialize_generic_providers(AgeGenericJoinState *state)
{
    int provider_index;

    if (state->materialized)
        return;

    for (provider_index = 0;
         provider_index < state->provider_count;
         provider_index++)
    {
        AgeGenericProvider *provider = &state->providers[provider_index];
        TupleTableSlot *slot;

        for (;;)
        {
            Datum value;
            graphid key1;
            graphid key2 = 0;
            graphid edge_id = 0;
            bool isnull;
            bool edge_id_valid = false;

            slot = ExecProcNode(provider->plan_state);
            if (TupIsNull(slot))
                break;
            value = slot_getattr(slot, provider->key1_attno, &isnull);
            if (isnull)
                continue;
            key1 = DATUM_GET_GRAPHID(value);
            if (provider->kind == AGE_GENERIC_PROVIDER_EDGE)
            {
                value = slot_getattr(slot, provider->key2_attno, &isnull);
                if (isnull)
                    continue;
                key2 = DATUM_GET_GRAPHID(value);
                value = slot_getattr(slot, provider->edge_id_attno, &isnull);
                if (!isnull)
                {
                    edge_id = DATUM_GET_GRAPHID(value);
                    edge_id_valid = true;
                }
            }
            append_generic_row(state, provider, key1, key2, edge_id,
                               edge_id_valid, slot);
        }

        if (provider->row_count > 1)
        {
            qsort(provider->rows, provider->row_count,
                  sizeof(AgeGenericRow), compare_generic_rows);
        }
        if (provider->row_count == 0)
            state->exhausted = true;
    }
    if (!state->exhausted)
        reduce_generic_providers(state);
    state->materialized = true;
    state->peak_memory = Max(state->peak_memory,
                             MemoryContextMemAllocated(state->data_context,
                                                       true));
}

static int
lower_bound_key1(AgeGenericProvider *provider, graphid key)
{
    int low = 0;
    int high = provider->row_count;

    while (low < high)
    {
        int middle = low + (high - low) / 2;

        if (provider->rows[middle].key1 < key)
            low = middle + 1;
        else
            high = middle;
    }
    return low;
}

static int
upper_bound_key1(AgeGenericProvider *provider, graphid key)
{
    int low = 0;
    int high = provider->row_count;

    while (low < high)
    {
        int middle = low + (high - low) / 2;

        if (provider->rows[middle].key1 <= key)
            low = middle + 1;
        else
            high = middle;
    }
    return low;
}

static int
lower_bound_pair(AgeGenericProvider *provider, graphid key1, graphid key2)
{
    int low = lower_bound_key1(provider, key1);
    int high = upper_bound_key1(provider, key1);

    while (low < high)
    {
        int middle = low + (high - low) / 2;

        if (provider->rows[middle].key2 < key2)
            low = middle + 1;
        else
            high = middle;
    }
    return low;
}

static int
upper_bound_pair(AgeGenericProvider *provider, graphid key1, graphid key2)
{
    int low = lower_bound_key1(provider, key1);
    int high = upper_bound_key1(provider, key1);

    while (low < high)
    {
        int middle = low + (high - low) / 2;

        if (provider->rows[middle].key2 <= key2)
            low = middle + 1;
        else
            high = middle;
    }
    return low;
}

static graphid *
provider_domain(AgeGenericJoinState *state, AgeGenericProvider *provider,
                int variable, MemoryContext context, int *domain_count)
{
    graphid *domain;
    int start = 0;
    int end = provider->row_count;
    int row_index;
    int count = 0;
    graphid previous = 0;
    bool have_previous = false;
    bool use_key2 = false;
    MemoryContext oldcontext;

    *domain_count = 0;
    if (provider->row_count == 0)
        return NULL;

    if (provider->kind == AGE_GENERIC_PROVIDER_EDGE &&
        provider->var2 == variable)
    {
        graphid prefix = state->bindings[provider->var1];

        start = lower_bound_key1(provider, prefix);
        end = upper_bound_key1(provider, prefix);
        use_key2 = true;
    }
    else if (provider->var1 != variable)
    {
        return NULL;
    }
    if (start >= end)
        return NULL;

    oldcontext = MemoryContextSwitchTo(context);
    domain = palloc(sizeof(graphid) * (end - start));
    MemoryContextSwitchTo(oldcontext);
    for (row_index = start; row_index < end; row_index++)
    {
        graphid value = use_key2 ? provider->rows[row_index].key2 :
            provider->rows[row_index].key1;

        if (!have_previous || value != previous)
        {
            domain[count++] = value;
            previous = value;
            have_previous = true;
        }
    }
    *domain_count = count;
    return domain;
}

static int
intersect_generic_domains(graphid *left, int left_count,
                          const graphid *right, int right_count)
{
    int left_index = 0;
    int right_index = 0;
    int output_count = 0;

    while (left_index < left_count && right_index < right_count)
    {
        if (left[left_index] < right[right_index])
            left_index++;
        else if (left[left_index] > right[right_index])
            right_index++;
        else
        {
            left[output_count++] = left[left_index];
            left_index++;
            right_index++;
        }
    }
    return output_count;
}

static int
compare_generic_graphids(const void *left, const void *right)
{
    graphid a = *(const graphid *)left;
    graphid b = *(const graphid *)right;

    if (a < b)
        return -1;
    if (a > b)
        return 1;
    return 0;
}

/*
 * Return a sorted distinct domain for one unbound provider endpoint.  key1
 * is already ordered by the provider sort.  key2 is ordered only within a
 * key1 prefix, so it needs a compact scratch sort for query-wide reduction.
 */
static graphid *
generic_provider_unbound_domain(AgeGenericProvider *provider, int variable,
                                MemoryContext context, int *domain_count)
{
    graphid *domain;
    bool use_key2;
    int row_index;
    int count = 0;
    MemoryContext oldcontext;

    *domain_count = 0;
    if (provider->row_count <= 0)
        return NULL;
    if (provider->var1 == variable)
        use_key2 = false;
    else if (provider->kind == AGE_GENERIC_PROVIDER_EDGE &&
             provider->var2 == variable)
        use_key2 = true;
    else
        return NULL;

    oldcontext = MemoryContextSwitchTo(context);
    domain = palloc(sizeof(graphid) * provider->row_count);
    MemoryContextSwitchTo(oldcontext);
    for (row_index = 0; row_index < provider->row_count; row_index++)
    {
        domain[row_index] = use_key2 ? provider->rows[row_index].key2 :
            provider->rows[row_index].key1;
    }
    if (use_key2 && provider->row_count > 1)
    {
        qsort(domain, provider->row_count, sizeof(graphid),
              compare_generic_graphids);
    }
    for (row_index = 0; row_index < provider->row_count; row_index++)
    {
        if (count == 0 || domain[row_index] != domain[count - 1])
            domain[count++] = domain[row_index];
    }
    *domain_count = count;
    return domain;
}

static bool
build_generic_reduction_domains(AgeGenericJoinState *state,
                                MemoryContext context,
                                AgeGenericDomain *domains)
{
    bool *initialized;
    int provider_index;
    int variable;

    initialized = MemoryContextAllocZero(
        context, sizeof(bool) * state->variable_count);
    for (provider_index = 0;
         provider_index < state->provider_count;
         provider_index++)
    {
        AgeGenericProvider *provider = &state->providers[provider_index];
        int endpoint_count = provider->kind == AGE_GENERIC_PROVIDER_EDGE ?
            2 : 1;
        int endpoint;

        for (endpoint = 0; endpoint < endpoint_count; endpoint++)
        {
            graphid *provider_domain;
            int provider_domain_count;

            variable = endpoint == 0 ? provider->var1 : provider->var2;
            provider_domain = generic_provider_unbound_domain(
                provider, variable, context, &provider_domain_count);
            if (provider_domain_count <= 0)
                return false;
            if (!initialized[variable])
            {
                domains[variable].keys = provider_domain;
                domains[variable].count = provider_domain_count;
                initialized[variable] = true;
            }
            else
            {
                domains[variable].count = intersect_generic_domains(
                    domains[variable].keys, domains[variable].count,
                    provider_domain, provider_domain_count);
                if (domains[variable].count <= 0)
                    return false;
            }
        }
    }

    for (variable = 0; variable < state->variable_count; variable++)
    {
        if (!initialized[variable] || domains[variable].count <= 0)
            return false;
    }
    return true;
}

static bool
generic_domain_contains(const AgeGenericDomain *domain, graphid key)
{
    int low = 0;
    int high = domain->count;

    while (low < high)
    {
        int middle = low + (high - low) / 2;

        if (domain->keys[middle] < key)
            low = middle + 1;
        else
            high = middle;
    }
    return low < domain->count && domain->keys[low] == key;
}

static int64
generic_provider_row_total(AgeGenericJoinState *state)
{
    int64 total = 0;
    int provider_index;

    for (provider_index = 0;
         provider_index < state->provider_count;
         provider_index++)
    {
        add_generic_counter(&total, state->providers[provider_index].row_count);
    }
    return total;
}

static int64
generic_domain_key_total(AgeGenericJoinState *state,
                         const AgeGenericDomain *domains)
{
    int64 total = 0;
    int variable;

    for (variable = 0; variable < state->variable_count; variable++)
        add_generic_counter(&total, domains[variable].count);
    return total;
}

static int64
filter_generic_provider(AgeGenericProvider *provider,
                        const AgeGenericDomain *domains)
{
    int read_index;
    int write_index = 0;
    int old_count = provider->row_count;

    for (read_index = 0; read_index < old_count; read_index++)
    {
        AgeGenericRow *row = &provider->rows[read_index];

        if (!generic_domain_contains(&domains[provider->var1], row->key1))
            continue;
        if (provider->kind == AGE_GENERIC_PROVIDER_EDGE &&
            !generic_domain_contains(&domains[provider->var2], row->key2))
        {
            continue;
        }
        if (write_index != read_index)
            provider->rows[write_index] = *row;
        write_index++;
    }
    provider->row_count = write_index;
    return (int64)old_count - write_index;
}

/*
 * Enforce query-wide endpoint consistency before variable DFS.  Repeated
 * semijoin passes are complete for tree-shaped binary relations after at
 * most the component diameter, and remain a safe pruning step for cycles.
 * Stopping after a bounded number of passes affects only pruning strength,
 * never result correctness.
 */
static void
reduce_generic_providers(AgeGenericJoinState *state)
{
    MemoryContext reduction_context;
    int max_passes = Max(state->variable_count * 2, 1);
    int pass;

    reduction_context = AllocSetContextCreate(
        state->css.ss.ps.state->es_query_cxt,
        "AGE Generic Join semijoin reduction", ALLOCSET_SMALL_SIZES);
    state->semijoin_provider_rows_after = 0;
    state->semijoin_final_domain_keys = 0;
    for (pass = 0; pass < max_passes; pass++)
    {
        AgeGenericDomain *domains;
        int64 rows_removed = 0;
        int provider_index;

        MemoryContextReset(reduction_context);
        domains = MemoryContextAllocZero(
            reduction_context,
            sizeof(AgeGenericDomain) * state->variable_count);
        increment_generic_counter(&state->semijoin_passes);
        if (!build_generic_reduction_domains(state, reduction_context,
                                             domains))
        {
            state->peak_memory = Max(
                state->peak_memory,
                MemoryContextMemAllocated(state->data_context, true) +
                MemoryContextMemAllocated(reduction_context, true));
            state->semijoin_provider_rows_after =
                generic_provider_row_total(state);
            state->semijoin_final_domain_keys = 0;
            state->exhausted = true;
            break;
        }

        state->peak_memory = Max(
            state->peak_memory,
            MemoryContextMemAllocated(state->data_context, true) +
            MemoryContextMemAllocated(reduction_context, true));

        for (provider_index = 0;
             provider_index < state->provider_count;
             provider_index++)
        {
            AgeGenericProvider *provider = &state->providers[provider_index];

            rows_removed += filter_generic_provider(provider, domains);
            if (provider->row_count <= 0)
            {
                state->exhausted = true;
                break;
            }
        }
        add_generic_counter(&state->semijoin_rows_removed, rows_removed);
        state->semijoin_provider_rows_after =
            generic_provider_row_total(state);
        state->semijoin_final_domain_keys =
            state->exhausted ? 0 : generic_domain_key_total(state, domains);
        if (state->exhausted || rows_removed == 0)
            break;
    }
    MemoryContextDelete(reduction_context);
}

static void
build_generic_level(AgeGenericJoinState *state, int depth)
{
    AgeGenericLevel *level = &state->levels[depth];
    graphid *candidates = NULL;
    int candidate_count = 0;
    int provider_index;
    bool found_provider = false;

    MemoryContextReset(level->context);
    for (provider_index = 0;
         provider_index < state->provider_count;
         provider_index++)
    {
        AgeGenericProvider *provider = &state->providers[provider_index];
        graphid *domain;
        int domain_count;

        if (provider->var1 != depth && provider->var2 != depth)
            continue;
        domain = provider_domain(state, provider, depth, level->context,
                                 &domain_count);
        found_provider = true;
        if (domain_count == 0)
        {
            candidate_count = 0;
            candidates = NULL;
            break;
        }
        if (candidates == NULL)
        {
            candidates = domain;
            candidate_count = domain_count;
        }
        else
        {
            candidate_count = intersect_generic_domains(
                candidates, candidate_count, domain, domain_count);
            if (candidate_count == 0)
                break;
        }
    }

    level->candidates = found_provider ? candidates : NULL;
    level->candidate_count = found_provider ? candidate_count : 0;
    level->next_candidate = 0;
    level->initialized = true;
}

static void
reset_generic_level(AgeGenericJoinState *state, int depth)
{
    AgeGenericLevel *level = &state->levels[depth];

    MemoryContextReset(level->context);
    level->candidates = NULL;
    level->candidate_count = 0;
    level->next_candidate = 0;
    level->initialized = false;
}

static bool
prepare_generic_bags(AgeGenericJoinState *state)
{
    int provider_index;
    int64 flat_rows = 1;

    for (provider_index = 0;
         provider_index < state->provider_count;
         provider_index++)
    {
        AgeGenericProvider *provider = &state->providers[provider_index];
        graphid key1 = state->bindings[provider->var1];
        int start;
        int end;

        if (provider->kind == AGE_GENERIC_PROVIDER_VERTEX)
        {
            start = lower_bound_key1(provider, key1);
            end = upper_bound_key1(provider, key1);
        }
        else
        {
            graphid key2 = state->bindings[provider->var2];

            start = lower_bound_pair(provider, key1, key2);
            end = upper_bound_pair(provider, key1, key2);
        }
        provider->bag_start = start;
        provider->bag_count = end - start;
        if (provider->bag_count <= 0)
            return false;
        if (flat_rows > PG_INT64_MAX / provider->bag_count)
            flat_rows = PG_INT64_MAX;
        else
            flat_rows *= provider->bag_count;
        state->enumeration_order[provider_index] = provider_index;
        state->combination_indexes[provider_index] = -1;
        state->selected_edge_id_valid[provider_index] = false;
    }

    for (provider_index = 1;
         provider_index < state->provider_count;
         provider_index++)
    {
        int selected = state->enumeration_order[provider_index];
        int position = provider_index;

        while (position > 0 &&
               state->providers[state->enumeration_order[position - 1]].
                   bag_count > state->providers[selected].bag_count)
        {
            state->enumeration_order[position] =
                state->enumeration_order[position - 1];
            position--;
        }
        state->enumeration_order[position] = selected;
    }
    state->combination_started = false;
    state->binding_ready = true;
    increment_generic_counter(&state->bindings_completed);
    add_generic_counter(&state->candidate_flat_rows, flat_rows);
    return true;
}

static bool
next_generic_binding(AgeGenericJoinState *state)
{
    int depth;

    if (state->exhausted)
        return false;
    materialize_generic_providers(state);
    if (state->exhausted)
        return false;

    depth = state->search_started ? state->variable_count - 1 : 0;
    state->search_started = true;
    while (depth >= 0)
    {
        AgeGenericLevel *level = &state->levels[depth];

        if (!level->initialized)
            build_generic_level(state, depth);
        if (level->next_candidate < level->candidate_count)
        {
            state->bindings[depth] =
                level->candidates[level->next_candidate++];
            if (depth == state->variable_count - 1)
            {
                if (prepare_generic_bags(state))
                    return true;
                continue;
            }
            depth++;
            reset_generic_level(state, depth);
            continue;
        }

        reset_generic_level(state, depth);
        depth--;
    }

    state->exhausted = true;
    return false;
}

static bool
next_generic_combination(AgeGenericJoinState *state)
{
    int depth;

    if (!state->binding_ready)
        return false;
    depth = state->combination_started ? state->provider_count - 1 : 0;
    state->combination_started = true;

    while (depth >= 0)
    {
        int provider_index = state->enumeration_order[depth];
        AgeGenericProvider *provider = &state->providers[provider_index];
        int local_index = state->combination_indexes[provider_index] + 1;

        state->selected_edge_id_valid[depth] = false;
        while (local_index < provider->bag_count)
        {
            AgeGenericRow *row = &provider->rows[
                provider->bag_start + local_index];
            bool conflict = false;
            int previous_depth;

            state->combination_indexes[provider_index] = local_index;
            if (row->edge_id_valid)
            {
                for (previous_depth = 0;
                     previous_depth < depth;
                     previous_depth++)
                {
                    int previous_provider =
                        state->enumeration_order[previous_depth];

                    if (state->selected_edge_id_valid[previous_depth] &&
                        state->selected_edge_ids[previous_depth] ==
                            row->edge_id &&
                        bms_overlap(
                            state->uniqueness_groups[provider_index],
                            state->uniqueness_groups[previous_provider]))
                    {
                        conflict = true;
                        break;
                    }
                }
                if (conflict)
                {
                    increment_generic_counter(&state->uniqueness_rejects);
                    local_index++;
                    continue;
                }
                state->selected_edge_ids[depth] = row->edge_id;
                state->selected_edge_id_valid[depth] = true;
            }

            if (depth == state->provider_count - 1)
            {
                increment_generic_counter(&state->candidate_combinations);
                return true;
            }
            depth++;
            provider_index = state->enumeration_order[depth];
            state->combination_indexes[provider_index] = -1;
            state->selected_edge_id_valid[depth] = false;
            break;
        }

        if (local_index < provider->bag_count)
            continue;
        state->combination_indexes[provider_index] = -1;
        state->selected_edge_id_valid[depth] = false;
        depth--;
    }

    state->binding_ready = false;
    state->combination_started = false;
    return false;
}

static TupleTableSlot *
materialize_generic_combination(AgeGenericJoinState *state)
{
    TupleTableSlot *raw_slot = state->css.ss.ss_ScanTupleSlot;
    int raw_index = 0;
    int provider_index;

    ExecClearTuple(raw_slot);
    for (provider_index = 0;
         provider_index < state->provider_count;
         provider_index++)
    {
        AgeGenericProvider *provider = &state->providers[provider_index];
        AgeGenericRow *row = &provider->rows[
            provider->bag_start +
            state->combination_indexes[provider_index]];
        int attr_index;

        ExecStoreMinimalTuple(row->tuple, provider->tuple_slot, false);
        slot_getallattrs(provider->tuple_slot);
        for (attr_index = 0;
             attr_index < provider->tuple_slot->tts_tupleDescriptor->natts;
             attr_index++)
        {
            if (raw_index >= raw_slot->tts_tupleDescriptor->natts)
                elog(ERROR, "AGE Generic Join raw tuple width overflow");
            raw_slot->tts_values[raw_index] =
                provider->tuple_slot->tts_values[attr_index];
            raw_slot->tts_isnull[raw_index] =
                provider->tuple_slot->tts_isnull[attr_index];
            raw_index++;
        }
    }
    if (raw_index != raw_slot->tts_tupleDescriptor->natts)
    {
        elog(ERROR, "AGE Generic Join raw tuple width mismatch: %d versus %d",
             raw_index, raw_slot->tts_tupleDescriptor->natts);
    }
    ExecStoreVirtualTuple(raw_slot);
    return raw_slot;
}

static TupleTableSlot *
access_age_generic_join(ScanState *node)
{
    AgeGenericJoinState *state = (AgeGenericJoinState *)node;

    for (;;)
    {
        if (!state->binding_ready && !next_generic_binding(state))
            return ExecClearTuple(node->ss_ScanTupleSlot);
        if (next_generic_combination(state))
            return materialize_generic_combination(state);
    }
}

static TupleTableSlot *
exec_age_generic_join(CustomScanState *node)
{
    AgeGenericJoinState *state = (AgeGenericJoinState *)node;
    TupleTableSlot *slot;

    slot = ExecScan(&node->ss, access_age_generic_join,
                    recheck_age_generic_join);
    if (!TupIsNull(slot))
        increment_generic_counter(&state->rows_emitted);
    return slot;
}

static bool
recheck_age_generic_join(ScanState *node, TupleTableSlot *slot)
{
    (void)node;
    (void)slot;
    return true;
}

static void
initialize_generic_uniqueness_groups(AgeGenericJoinState *state,
                                     List *group_descs)
{
    ListCell *group_desc_cell;
    int provider_index = 0;

    foreach(group_desc_cell, group_descs)
    {
        List *group_ids = lfirst(group_desc_cell);
        ListCell *group_cell;

        foreach(group_cell, group_ids)
        {
            int group_id = intVal(lfirst(group_cell));

            if (group_id < 0)
                elog(ERROR, "invalid AGE Generic Join uniqueness group");
            state->uniqueness_groups[provider_index] = bms_add_member(
                state->uniqueness_groups[provider_index], group_id);
            state->uniqueness_group_count = Max(
                state->uniqueness_group_count, group_id + 1);
        }
        provider_index++;
    }
}

static void
initialize_generic_provider(AgeGenericJoinState *state,
                            AgeGenericProvider *provider,
                            PlanState *plan_state, List *desc,
                            EState *estate)
{
    TupleDesc tuple_desc = ExecGetResultType(plan_state);

    if (list_length(desc) != AGE_GENERIC_PROVIDER_DESC_COUNT)
        elog(ERROR, "invalid AGE Generic Join provider descriptor");

    provider->kind = (AgeGenericProviderKind)intVal(list_nth(
        desc, AGE_GENERIC_PROVIDER_DESC_KIND));
    provider->var1 = intVal(list_nth(desc, AGE_GENERIC_PROVIDER_DESC_VAR1));
    provider->var2 = intVal(list_nth(desc, AGE_GENERIC_PROVIDER_DESC_VAR2));
    provider->key1_attno = (AttrNumber)intVal(list_nth(
        desc, AGE_GENERIC_PROVIDER_DESC_KEY1_ATTNO));
    provider->key2_attno = (AttrNumber)intVal(list_nth(
        desc, AGE_GENERIC_PROVIDER_DESC_KEY2_ATTNO));
    provider->edge_id_attno = (AttrNumber)intVal(list_nth(
        desc, AGE_GENERIC_PROVIDER_DESC_EDGE_ID_ATTNO));
    provider->plan_state = plan_state;

    if ((provider->kind != AGE_GENERIC_PROVIDER_VERTEX &&
         provider->kind != AGE_GENERIC_PROVIDER_EDGE) ||
        provider->var1 < 0 || provider->var1 >= state->variable_count ||
        provider->key1_attno <= 0 || provider->key1_attno > tuple_desc->natts)
    {
        elog(ERROR, "invalid AGE Generic Join provider keys");
    }
    if (provider->kind == AGE_GENERIC_PROVIDER_EDGE &&
        (provider->var2 <= provider->var1 ||
         provider->var2 >= state->variable_count ||
         provider->key2_attno <= 0 ||
         provider->key2_attno > tuple_desc->natts ||
         provider->edge_id_attno <= 0 ||
         provider->edge_id_attno > tuple_desc->natts))
    {
        elog(ERROR, "invalid AGE Generic Join edge provider");
    }

    provider->tuple_slot = ExecInitExtraTupleSlot(
        estate, tuple_desc, &TTSOpsMinimalTuple);
}

static void
begin_age_generic_join(CustomScanState *node, EState *estate, int eflags)
{
    AgeGenericJoinState *state = (AgeGenericJoinState *)node;
    CustomScan *cscan = castNode(CustomScan, node->ss.ps.plan);
    List *provider_descs = list_nth(
        cscan->custom_private, AGE_GENERIC_JOIN_PRIVATE_PROVIDER_DESCS);
    List *uniqueness_group_descs = list_nth(
        cscan->custom_private, AGE_GENERIC_JOIN_PRIVATE_UNIQUENESS_GROUPS);
    ListCell *plan_cell;
    ListCell *desc_cell;
    int provider_index = 0;
    int depth;

    foreach(plan_cell, cscan->custom_plans)
    {
        Plan *subplan = (Plan *)lfirst(plan_cell);

        node->custom_ps = lappend(node->custom_ps,
                                  ExecInitNode(subplan, estate, eflags));
    }
    if (list_length(node->custom_ps) != state->provider_count ||
        list_length(provider_descs) != state->provider_count ||
        list_length(uniqueness_group_descs) != state->provider_count)
    {
        elog(ERROR, "AGE Generic Join child count does not match providers");
    }

    state->providers = palloc0(sizeof(AgeGenericProvider) *
                                state->provider_count);
    state->levels = palloc0(sizeof(AgeGenericLevel) * state->variable_count);
    state->bindings = palloc0(sizeof(graphid) * state->variable_count);
    state->combination_indexes = palloc0(sizeof(int) * state->provider_count);
    state->enumeration_order = palloc0(sizeof(int) * state->provider_count);
    state->selected_edge_ids = palloc0(sizeof(graphid) *
                                        state->provider_count);
    state->selected_edge_id_valid = palloc0(sizeof(bool) *
                                             state->provider_count);
    state->uniqueness_groups = palloc0(sizeof(Bitmapset *) *
                                        state->provider_count);
    state->data_context = AllocSetContextCreate(
        estate->es_query_cxt, "AGE Generic Join provider rows",
        ALLOCSET_DEFAULT_SIZES);

    initialize_generic_uniqueness_groups(state, uniqueness_group_descs);

    provider_index = 0;
    forboth(plan_cell, node->custom_ps, desc_cell, provider_descs)
    {
        PlanState *plan_state = lfirst(plan_cell);
        List *desc = lfirst(desc_cell);
        AgeGenericProvider *provider = &state->providers[provider_index];

        initialize_generic_provider(state, provider, plan_state, desc, estate);
        provider_index++;
    }

    for (depth = 0; depth < state->variable_count; depth++)
    {
        state->levels[depth].context = AllocSetContextCreate(
            estate->es_query_cxt, "AGE Generic Join variable domain",
            ALLOCSET_SMALL_SIZES);
    }
}

static void
reset_age_generic_join_state(AgeGenericJoinState *state)
{
    int provider_index;
    int depth;

    if (state->data_context != NULL)
        MemoryContextReset(state->data_context);
    for (provider_index = 0;
         provider_index < state->provider_count;
         provider_index++)
    {
        AgeGenericProvider *provider = &state->providers[provider_index];

        provider->rows = NULL;
        provider->row_count = 0;
        provider->row_capacity = 0;
        provider->bag_start = 0;
        provider->bag_count = 0;
        state->combination_indexes[provider_index] = -1;
        state->selected_edge_id_valid[provider_index] = false;
    }
    for (depth = 0; depth < state->variable_count; depth++)
        reset_generic_level(state, depth);
    state->search_started = false;
    state->binding_ready = false;
    state->combination_started = false;
    state->materialized = false;
    state->exhausted = false;
}

static void
rescan_age_generic_join(CustomScanState *node)
{
    AgeGenericJoinState *state = (AgeGenericJoinState *)node;
    int provider_index;

    for (provider_index = 0;
         provider_index < state->provider_count;
         provider_index++)
    {
        ExecReScan(state->providers[provider_index].plan_state);
    }
    reset_age_generic_join_state(state);
    increment_generic_counter(&state->rescans);
}

static void
end_age_generic_join(CustomScanState *node)
{
    AgeGenericJoinState *state = (AgeGenericJoinState *)node;
    int provider_index;
    int depth;

    for (provider_index = 0;
         provider_index < state->provider_count;
         provider_index++)
    {
        ExecEndNode(state->providers[provider_index].plan_state);
    }
    for (depth = 0; depth < state->variable_count; depth++)
    {
        if (state->levels[depth].context != NULL)
            MemoryContextDelete(state->levels[depth].context);
    }
    if (state->data_context != NULL)
        MemoryContextDelete(state->data_context);
}

static char *
format_generic_variable_order(AgeGenericJoinState *state)
{
    StringInfoData buffer;
    int depth;

    initStringInfo(&buffer);
    for (depth = 0; depth < state->variable_count; depth++)
    {
        appendStringInfo(&buffer, "%s%d(rti=%u)", depth == 0 ? "" : ", ",
                         depth + 1, state->variable_rtis[depth]);
    }
    return buffer.data;
}

static void
explain_age_generic_join(CustomScanState *node, List *ancestors,
                         ExplainState *es)
{
    AgeGenericJoinState *state = (AgeGenericJoinState *)node;
    char *variable_order = format_generic_variable_order(state);

    (void)ancestors;
    ExplainPropertyText("Generic Join Algorithm",
                        "trie-ordered variable DFS with delayed edge bags",
                        es);
    ExplainPropertyText("Variable Order", variable_order, es);
    ExplainPropertyInteger("Variable Count", NULL, state->variable_count, es);
    ExplainPropertyInteger("Provider Count", NULL, state->provider_count, es);
    if (es->analyze)
    {
        int64 flat_rows_avoided;

        flat_rows_avoided =
            state->candidate_flat_rows > state->candidate_combinations ?
            state->candidate_flat_rows - state->candidate_combinations : 0;

        ExplainPropertyInteger("Provider Rows Materialized", NULL,
                               state->rows_materialized, es);
        ExplainPropertyInteger("Semijoin Reduction Passes", NULL,
                               state->semijoin_passes, es);
        ExplainPropertyInteger("Semijoin Rows Removed", NULL,
                               state->semijoin_rows_removed, es);
        ExplainPropertyInteger("Semijoin Provider Rows After", NULL,
                               state->semijoin_provider_rows_after, es);
        ExplainPropertyInteger("Semijoin Final Domain Keys", NULL,
                               state->semijoin_final_domain_keys, es);
        ExplainPropertyInteger("Complete Bindings", NULL,
                               state->bindings_completed, es);
        ExplainPropertyInteger("Candidate Flat Rows", NULL,
                               state->candidate_flat_rows, es);
        ExplainPropertyInteger("Candidate Bag Combinations", NULL,
                               state->candidate_combinations, es);
        ExplainPropertyInteger("Flat Rows Avoided", NULL,
                               flat_rows_avoided, es);
        ExplainPropertyInteger("Uniqueness Constraint Groups", NULL,
                               state->uniqueness_group_count, es);
        ExplainPropertyInteger("Uniqueness Rejects", NULL,
                               state->uniqueness_rejects, es);
        ExplainPropertyInteger("Rows Emitted", NULL, state->rows_emitted, es);
        ExplainPropertyInteger("Rescans", NULL, state->rescans, es);
        ExplainPropertyInteger("Peak Generic Join Memory", "bytes",
                               (int64)state->peak_memory, es);
    }
    pfree(variable_order);
}
