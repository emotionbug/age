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

#include "access/xact.h"
#include "access/age_adjacency.h"
#include "access/genam.h"
#include "access/relation.h"
#include "access/table.h"
#include "catalog/namespace.h"
#include "catalog/pg_collation_d.h"
#include "catalog/pg_am.h"
#include "catalog/pg_statistic.h"
#include "catalog/pg_type_d.h"
#include "catalog/ag_namespace.h"
#include "catalog/ag_label.h"
#include "catalog/ag_graph.h"
#include "utils/age_global_graph.h"
#include "commands/label_commands.h"
#include "executor/cypher_adjacency_match.h"
#include "executor/cypher_property_projection.h"
#include "executor/cypher_vle_stream.h"
#include "fmgr.h"
#include "nodes/makefuncs.h"
#include "nodes/cypher_nodes.h"
#include "nodes/nodeFuncs.h"
#include "nodes/primnodes.h"
#include "optimizer/cost.h"
#include "optimizer/cypher_graph_join.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/planmain.h"
#include "optimizer/planner.h"
#include "optimizer/restrictinfo.h"
#include "optimizer/tlist.h"
#include "parser/parse_func.h"
#include "parser/cypher_clause.h"
#include "rewrite/rewriteManip.h"
#include "utils/inval.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/relcache.h"
#include "utils/spccache.h"
#include "utils/syscache.h"

#include "optimizer/cypher_pathnode.h"
#include "optimizer/cypher_paths.h"
#include "optimizer/cypher_property_paths.h"
#include "parser/cypher_property_signature.h"
#include "utils/ag_func.h"
#include "utils/ag_cache.h"
#include "utils/agtype.h"
#include "utils/age_vle_root.h"
#include "utils/age_vle_source_cost.h"
#include "utils/graphid.h"

typedef enum cypher_clause_kind
{
    CYPHER_CLAUSE_NONE,
    CYPHER_CLAUSE_CREATE,
    CYPHER_CLAUSE_SET,
    CYPHER_CLAUSE_DELETE,
    CYPHER_CLAUSE_MERGE
} cypher_clause_kind;

typedef CustomPath *(*cypher_path_factory)(PlannerInfo *root, RelOptInfo *rel,
                                           List *custom_private);

typedef struct PropertyIndexSurfaceContext
{
    Query *parse;
    Index rti;
    RangeTblEntry *rte;
} PropertyIndexSurfaceContext;

typedef struct GraphPropertySourceSelectivity
{
    double selectivity;
    AgeGraphPropertySelectivitySource source_kind;
} GraphPropertySourceSelectivity;

typedef enum GraphJoinNestLoopComponentIndex
{
    GRAPH_JOIN_NEST_NODE_PROPERTY = 0,
    GRAPH_JOIN_NEST_VLE,
    GRAPH_JOIN_NEST_ADJACENCY,
    GRAPH_JOIN_NEST_VALUE_JOIN,
    GRAPH_JOIN_NEST_APPLY,
    GRAPH_JOIN_NEST_CARTESIAN,
    GRAPH_JOIN_NEST_COMPONENT_COUNT
} GraphJoinNestLoopComponentIndex;

typedef struct GraphJoinNestLoopCandidate
{
    int component_index;
    Path *outer_path;
    Path *inner_path;
    AgeGraphJoinPathEvidence outer_evidence;
    AgeGraphJoinPathEvidence inner_evidence;
    AgeGraphJoinCandidateTable *table;
    AgeGraphJoinCandidate *candidate;
    Cost total_cost;
} GraphJoinNestLoopCandidate;

typedef struct CypherGraphPatternRegistryEntry
{
    char *graph_pattern_key;
    List *vle_marker_aliases;
    List *node_aliases;
    List *index_oids;
    AgeGraphJoinLoweringArtifact *artifact;
} CypherGraphPatternRegistryEntry;

static set_rel_pathlist_hook_type prev_set_rel_pathlist_hook;
static set_join_pathlist_hook_type prev_set_join_pathlist_hook;
static create_upper_paths_hook_type prev_create_upper_paths_hook;
static List *adjacency_match_candidates = NIL;
static List *graph_pattern_registry = NIL;
static bool cypher_path_xact_callback_registered = false;

static Oid cypher_create_clause_func_oid = InvalidOid;
static Oid cypher_set_clause_func_oid = InvalidOid;
static Oid cypher_delete_clause_func_oid = InvalidOid;
static Oid cypher_merge_clause_func_oid = InvalidOid;
static bool cypher_clause_func_callback_registered = false;

static void set_rel_pathlist(PlannerInfo *root, RelOptInfo *rel, Index rti,
                             RangeTblEntry *rte);
static void set_join_pathlist(PlannerInfo *root, RelOptInfo *joinrel,
                              RelOptInfo *outerrel, RelOptInfo *innerrel,
                              JoinType jointype, JoinPathExtraData *extra);
static void create_upper_paths(PlannerInfo *root, UpperRelationKind stage,
                               RelOptInfo *input_rel,
                               RelOptInfo *output_rel, void *extra);
static void cypher_path_xact_callback(XactEvent event, void *arg);
static cypher_clause_kind get_cypher_clause_kind(RangeTblEntry *rte);
static void register_cypher_clause_function_oid_callbacks(void);
static void load_cypher_clause_function_oids(void);
static void invalidate_cypher_clause_function_oids(Datum arg, int cache_id,
                                                   uint32 hash_value);
static void replace_with_cypher_dml_path(PlannerInfo *root, RelOptInfo *rel,
                                         RangeTblEntry *rte,
                                         cypher_path_factory factory);
typedef struct AdjacencyMatchPayloadRequest AdjacencyMatchPayloadRequest;
static void add_adjacency_match_custom_path(
    PlannerInfo *root, RelOptInfo *rel,
    CypherAdjacencyMatchCandidate *candidate);
static bool adjacency_match_can_add_partial_path(
    RelOptInfo *rel, CypherAdjacencyMatchCandidate *candidate,
    const AdjacencyMatchPayloadRequest *payload_request,
    Relids path_required_outer, int *parallel_workers);
static double adjacency_match_parallel_divisor(int parallel_workers);
static CustomPath *make_adjacency_match_partial_custom_path(
    RelOptInfo *rel, CustomPath *serial_path, int parallel_workers);
static void set_adjacency_match_custom_private_descriptor(
    CustomPath *path, List *descriptor);
static Path *add_adjacency_match_partial_gather_path(PlannerInfo *root,
                                                     RelOptInfo *rel,
                                                     CustomPath *partial_path,
                                                     double gathered_rows);
static Const *find_endpoint_graphid_const(PlannerInfo *root,
                                          CypherAdjacencyMatchCandidate *candidate);
static bool adjacency_match_bound_expr_uses_age_id(Node *node);
static bool adjacency_match_bound_expr_uses_age_id_walker(Node *node,
                                                          void *context);
struct AdjacencyMatchPayloadRequest
{
    Bitmapset *attrs;
    int attr_mask;
    bool unsupported;
    bool fetch_properties;
};

static void cost_adjacency_match_custom_path(PlannerInfo *root,
                                             RelOptInfo *rel,
                                             CustomPath *cp,
                                             CypherAdjacencyMatchCandidate *candidate,
                                             const AdjacencyMatchPayloadRequest *payload_request,
                                             Const *endpoint_const);
static bool adjacency_match_requires_edge_payload(
    const CypherAdjacencyMatchCandidate *candidate);
static bool adjacency_match_has_terminal_property_prefetch(
    const CypherAdjacencyMatchCandidate *candidate);
static AgeAdjacencyMatchValueKind adjacency_match_terminal_property_value_kind(
    const CypherAdjacencyMatchCandidate *candidate);
static AgeAdjacencyMatchTerminalStrategy adjacency_match_terminal_source_strategy(
    const CypherAdjacencyMatchCandidate *candidate);
static AgeAdjacencyMatchSourceHandoff adjacency_match_source_handoff_kind(
    const CypherAdjacencyMatchCandidate *candidate,
    const AdjacencyMatchPayloadRequest *payload_request);
static void estimate_adjacency_match_source_handoff(
    CypherAdjacencyMatchCandidate *candidate,
    const AdjacencyMatchPayloadRequest *payload_request);
static int adjacency_match_terminal_prefetch_threshold(
    const CypherAdjacencyMatchCandidate *candidate,
    const AdjacencyMatchPayloadRequest *payload_request);
static bool adjacency_match_plans_terminal_property_prefetch(
    const CypherAdjacencyMatchCandidate *candidate,
    const AdjacencyMatchPayloadRequest *payload_request);
static int adjacency_match_residual_predicate_count(
    const CypherAdjacencyMatchCandidate *candidate);
static int adjacency_match_index_solved_predicate_count(
    const CypherAdjacencyMatchCandidate *candidate,
    const AdjacencyMatchPayloadRequest *payload_request);
static AgeAdjacencyMatchTerminalPrefetchReason
adjacency_match_terminal_prefetch_reason_kind(
    const CypherAdjacencyMatchCandidate *candidate,
    const AdjacencyMatchPayloadRequest *payload_request);
static Cost adjacency_match_property_source_build_cost(
    const CypherAdjacencyMatchCandidate *candidate,
    const AdjacencyMatchPayloadRequest *payload_request);
static AgeGraphJoinConnectorKind adjacency_match_join_order_connector_kind(
    const CypherAdjacencyMatchCandidate *candidate);
static AgeGraphJoinBoundKind adjacency_match_join_order_bound_kind(
    const CypherAdjacencyMatchCandidate *candidate);
static AdjacencyMatchPayloadRequest build_adjacency_match_payload_request(
    Index relid, List *target_nodes, List *clauses);
static List *make_adjacency_match_descriptor(
    CypherAdjacencyMatchCandidate *candidate,
    const AdjacencyMatchPayloadRequest *payload_request,
    const AgeGraphJoinCandidateTable *graph_join_table,
    const AgeGraphJoinCandidate *graph_join_candidate);
static AgeGraphJoinLoweringArtifact *make_adjacency_match_lowering_artifact(
    RelOptInfo *rel, CypherAdjacencyMatchCandidate *candidate,
    const AdjacencyMatchPayloadRequest *payload_request,
    Relids path_required_outer);
static void prepare_adjacency_match_graph_join_artifact(
    PlannerInfo *root, RelOptInfo *rel,
    CypherAdjacencyMatchCandidate *candidate,
    const AdjacencyMatchPayloadRequest *payload_request,
    Relids path_required_outer);
static AgeGraphJoinCandidateTable *bind_adjacency_match_graph_join_artifact(
    PlannerInfo *root, CustomPath *cp, CypherAdjacencyMatchCandidate *candidate,
    bool register_path);
static AgeGraphJoinLoweringInput make_adjacency_match_lowering_input(
    RelOptInfo *rel, const CypherAdjacencyMatchCandidate *candidate,
    Relids path_required_outer);
static void apply_adjacency_match_graph_join_parallel_properties(
    AgeGraphJoinCandidateTable *table, bool parallel_safe,
    bool parallel_aware, int parallel_workers, Cost gather_cost,
    bool shared_state_required);
static void apply_adjacency_match_graph_join_parallel_costs(
    AgeGraphJoinCandidateTable *table, double divisor, Cost gather_cost);
static double adjacency_match_connector_scheduled_work(
    const CypherAdjacencyMatchCandidate *candidate);
static Cost adjacency_match_connector_scheduled_cost(
    const CypherAdjacencyMatchCandidate *candidate,
    const AdjacencyMatchPayloadRequest *payload_request,
    Cost startup_cost);
static AgeGraphJoinSourceEvidenceKind
adjacency_match_connector_source_evidence_kind(
    const CypherAdjacencyMatchCandidate *candidate);
static Plan *plan_age_adjacency_match_path(PlannerInfo *root,
                                           RelOptInfo *rel,
                                           CustomPath *best_path,
                                           List *tlist, List *clauses,
                                           List *custom_plans);
static List *build_adjacency_match_custom_scan_tlist(
    Index relid, const AdjacencyMatchPayloadRequest *payload_request);
static int adjacency_match_payload_attr_mask(Bitmapset *attrs);
static void collect_adjacency_match_scan_vars_from_list(List *nodes,
                                                        void *context);
static bool collect_adjacency_match_scan_vars(Node *node, void *context);
static const char *adjacency_match_attr_name(AttrNumber attno);
static Oid adjacency_match_attr_type(AttrNumber attno);
static CypherAdjacencyMatchCandidate *pop_adjacency_match_candidate(
    PlannerInfo *root, RangeTblEntry *rte);
static CypherGraphPatternRegistryEntry *find_graph_pattern_registry_entry(
    const char *graph_pattern_key);
static CypherGraphPatternRegistryEntry *ensure_graph_pattern_registry_entry(
    const char *graph_pattern_key);
static AgeGraphJoinLoweringArtifact *graph_pattern_registry_artifact(
    const char *graph_pattern_key);
static void graph_pattern_registry_register_declared_cover(
    PlannerInfo *root, RelOptInfo *rel, const char *graph_pattern_key);
static void graph_pattern_registry_declare_typed_source(
    const char *graph_pattern_key, AgeGraphJoinSourceKind source_kind_id,
    AgeGraphJoinComponentKind component_family_kind);
static AgeGraphJoinCandidateTable *graph_pattern_registry_typed_entry_table(
    const char *graph_pattern_key, AgeGraphJoinSourceKind source_kind_id,
    AgeGraphJoinComponentKind component_family_kind);
static bool graph_pattern_registry_has_text(List *items, const char *value);
static void graph_pattern_registry_add_text(List **items, const char *value);
static char *find_graph_pattern_by_vle_marker(RangeTblEntry *rte);
static void bind_adjacency_match_candidate_outer_relids(
    PlannerInfo *root, CypherAdjacencyMatchCandidate *candidate);
static void restrict_adjacency_match_terminal_property_prefetch(
    PlannerInfo *root, CypherAdjacencyMatchCandidate *candidate);
static void register_index_pattern_reference(Oid index_oid,
                                             const char *graph_pattern_key);
static char *find_graph_pattern_by_node_alias(const char *node_alias);
static char *find_graph_pattern_by_index_oid(Oid index_oid);
static Oid graph_join_index_path_oid(Path *path);
static void log_graph_expansion_join_paths(RelOptInfo *joinrel,
                                           RelOptInfo *outerrel,
                                           RelOptInfo *innerrel,
                                           JoinType jointype);
static void register_graph_join_index_path_candidates(PlannerInfo *root,
                                                      RelOptInfo *rel);
static AgeGraphJoinLoweringArtifact *make_index_path_graph_join_artifact(
    RelOptInfo *rel, Path *path, const char *graph_pattern_key);
static AgeGraphJoinLoweringInput make_index_path_graph_join_input(
    RelOptInfo *rel, Path *path);
static void register_graph_join_joinrel_lowering_evidence(
    PlannerInfo *root, RelOptInfo *joinrel, Path *path,
    const AgeGraphJoinPathEvidence *evidence);
static void add_graph_join_candidate_nestloop_path(PlannerInfo *root,
                                                   RelOptInfo *joinrel,
                                                   RelOptInfo *outerrel,
                                                   RelOptInfo *innerrel,
                                                   JoinType jointype,
                                                   JoinPathExtraData *extra);
static bool graph_join_rel_candidate_is_component_covered(
    AgeGraphJoinRelMetadata *metadata,
    const AgeGraphJoinRelCandidate *candidate);
static bool graph_join_component_candidate_covers_evidence(
    const AgeGraphJoinRelComponentCandidate *component_candidate,
    const AgeGraphJoinPathEvidence *evidence);
static bool path_get_graph_join_rel_evidence(
    Path *path, AgeGraphJoinPathEvidence *evidence);
static int graph_join_nestloop_component_index(
    AgeGraphJoinComponentKind component_kind);
static void graph_join_nestloop_candidate_update(
    List **component_candidates, int component_index,
    Path *outer_path, Path *inner_path,
    const AgeGraphJoinPathEvidence *outer_evidence,
    const AgeGraphJoinPathEvidence *inner_evidence,
    AgeGraphJoinCandidateTable *table, AgeGraphJoinCandidate *candidate);
static Cost graph_join_nestloop_candidate_total_cost(
    Path *outer_path, Path *inner_path, AgeGraphJoinCandidate *candidate);
static GraphJoinNestLoopCandidate *graph_join_nestloop_candidate_slot(
    List **component_candidates, int component_index,
    AgeGraphJoinCandidate *candidate);
static bool graph_join_nestloop_candidate_slot_matches(
    GraphJoinNestLoopCandidate *slot, int component_index,
    AgeGraphJoinCandidate *candidate);
static bool graph_join_nestloop_component_identity_matches(
    const AgeGraphJoinComponent *existing,
    const AgeGraphJoinComponent *current);
static bool graph_join_nestloop_connector_identity_matches(
    const AgeGraphJoinConnector *existing,
    const AgeGraphJoinConnector *current);
static bool graph_join_nestloop_text_matches(const char *existing,
                                             const char *current);
static bool graph_join_nestloop_component_matches(
    AgeGraphJoinComponentKind existing_kind,
    AgeGraphJoinComponentKind current_kind);
static bool graph_join_nestloop_order_property_matches(
    AgeGraphJoinOrderPropertyKind existing_kind,
    AgeGraphJoinOrderPropertyKind current_kind);
static bool graph_join_nestloop_connector_matches(
    AgeGraphJoinConnectorKind existing_kind,
    AgeGraphJoinConnectorKind current_kind);
static bool graph_join_nestloop_source_evidence_matches(
    AgeGraphJoinSourceEvidenceKind existing_kind,
    AgeGraphJoinSourceEvidenceKind current_kind);
static bool graph_join_nestloop_source_kind_matches(
    AgeGraphJoinSourceKind existing_kind,
    AgeGraphJoinSourceKind current_kind);
static Path *graph_join_gather_partial_path(PlannerInfo *root,
                                            RelOptInfo *rel, Path *path);
static void add_graph_join_candidate_nestloop_alternative(
    PlannerInfo *root, RelOptInfo *joinrel, RelOptInfo *outerrel,
    RelOptInfo *innerrel, JoinType jointype, JoinPathExtraData *extra,
    GraphJoinNestLoopCandidate *candidate_slot);
static void graph_join_consider_nestloop_candidate_pair(
    RelOptInfo *outerrel, RelOptInfo *innerrel,
    Path *outer_path, Path *inner_path,
    const AgeGraphJoinPathEvidence *outer_evidence,
    const AgeGraphJoinPathEvidence *inner_evidence,
    List **component_candidates);
static bool graph_join_candidate_evidence_fits_pair(
    Path *outer_path, Path *inner_path,
    const AgeGraphJoinPathEvidence *evidence);
static void adjust_graph_expansion_join_rows(PlannerInfo *root,
                                             RelOptInfo *joinrel,
                                             JoinType jointype);
static AgeGraphJoinCandidateTable *make_graph_expansion_join_candidate_table(
    Path *outer_path, Path *inner_path,
    const AgeGraphJoinPathEvidence *outer_evidence,
    const AgeGraphJoinPathEvidence *inner_evidence);
static void add_graph_expansion_join_side_candidate(
    AgeGraphJoinCandidateTable *table, Path *outer_path, Path *inner_path,
    Path *evidence_path, const AgeGraphJoinPathEvidence *evidence);
static void add_graph_expansion_join_value_candidate(
    AgeGraphJoinCandidateTable *table, Path *outer_path, Path *inner_path,
    const AgeGraphJoinPathEvidence *outer_evidence,
    const AgeGraphJoinPathEvidence *inner_evidence);
static void add_graph_expansion_join_connector_candidate(
    AgeGraphJoinCandidateTable *table, Path *outer_path, Path *inner_path,
    const AgeGraphJoinPathEvidence *outer_evidence,
    const AgeGraphJoinPathEvidence *inner_evidence);
static const char *graph_join_pattern_key_from_evidence_pair(
    const AgeGraphJoinPathEvidence *outer_evidence,
    const AgeGraphJoinPathEvidence *inner_evidence);
static Relids graph_join_joinrelids_from_paths(Path *outer_path,
                                               Path *inner_path);
static Relids graph_join_required_outer_from_paths(Path *outer_path,
                                                   Path *inner_path,
                                                   Relids provided_relids);
static int graph_join_output_width_from_paths(Path *outer_path,
                                              Path *inner_path);
static bool graph_join_paths_are_parallel_safe(Path *outer_path,
                                               Path *inner_path);
static bool graph_join_candidate_nestloop_pair_is_legal(
    RelOptInfo *outerrel, RelOptInfo *innerrel, Path *outer_path,
    Path *inner_path);
static bool path_contains_graph_expansion(Path *path);
static bool path_get_graph_expansion_join_evidence(
    Path *path, Relids outer_relids, AgeGraphJoinPathEvidence *evidence);
static bool custom_path_adjacency_join_evidence(
    CustomPath *custom_path, AgeGraphJoinPathEvidence *evidence);
static bool custom_path_vle_join_evidence(
    CustomPath *custom_path, AgeGraphJoinPathEvidence *evidence);
static bool custom_path_vle_has_bound_endpoints(CustomPath *custom_path);
static AgeGraphJoinOrderPropertyKind index_path_join_order_property_kind(
    Path *path);
static bool path_is_index_backed(Path *path);
static int64 vle_stream_descriptor_int8_field(List *descriptor, int index,
                                              int64 fallback);
static bool vle_stream_edge_source_bool_field(List *descriptor, int index);
static bool path_required_outer_is_subset(Path *path, Relids relids);
static const char *path_param_info_string(Path *path);
static void add_narrow_typed_collect_paths(PlannerInfo *root,
                                           RelOptInfo *output_rel);
static void add_narrow_array_agg_property_paths(PlannerInfo *root,
                                                RelOptInfo *output_rel);
static Path *try_narrow_typed_collect_path(PlannerInfo *root,
                                           RelOptInfo *output_rel,
                                           Path *path,
                                           List *handoffs);
static Path *try_narrow_array_agg_property_path(
    PlannerInfo *root, Path *path,
    CypherArrayAggPropertyHandoff *handoff);
static bool pathtarget_contains_only_expr(PathTarget *target, Node *expr);
static Node *rewrite_property_index_surface_mutator(Node *node, void *context);
typedef struct VLETerminalPropertyQualRewriteContext
{
    const char *terminal_key;
    char *matched_key;
    Oid agtype_access_operator_oid;
    Oid age_vle_terminal_vertex_oid;
} VLETerminalPropertyQualRewriteContext;
typedef struct VLETerminalPropertyPredicate
{
    bool known;
    bool isnull;
    char *key;
    Const *value;
    Node *value_expr;
} VLETerminalPropertyPredicate;
static List *rewrite_vle_terminal_property_quals(List *clauses, List *output);
static List *remove_vle_terminal_property_predicate_quals(
    List *clauses, List *edge_source);
static List *remove_vle_terminal_property_child_plan_quals(
    List *custom_plans, List *edge_source, List *output);
static Node *rewrite_vle_terminal_property_qual_mutator(Node *node,
                                                        void *context);
static Node *try_rewrite_vle_terminal_property_access(
    FuncExpr *func, VLETerminalPropertyQualRewriteContext *context);
static bool extract_vle_terminal_property_predicate(
    List *restrictinfos, List *output, VLETerminalPropertyPredicate *predicate);
static bool match_vle_terminal_property_predicate_expr(
    Node *node, VLETerminalPropertyQualRewriteContext *context,
    Oid agtype_eq_oid, VLETerminalPropertyPredicate *predicate);
static void add_property_projection_custom_path(PlannerInfo *root,
                                                RelOptInfo *input_rel,
                                                RelOptInfo *output_rel,
                                                List *slots,
                                                FinalPathExtraData *extra);
static List *make_property_projection_slot_private(List *slots);
static Const *make_oid_const(Oid value);
static void add_deferred_ordered_property_projection_path(
    PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel,
    FinalPathExtraData *extra);
static void add_deferred_projection_paths(PlannerInfo *root,
                                          RelOptInfo *output_rel,
                                          PathTarget *lower_target,
                                          PathTarget *final_target,
                                          FinalPathExtraData *extra,
                                          Cost cost_discount,
                                          const char *debug_label,
                                          bool prune_join_child_targets);
static Path *copy_path_with_deferred_projection_target(PlannerInfo *root,
                                                       Path *path,
                                                       PathTarget *target,
                                                       bool prune_join_child_targets);
static Path *copy_join_path_with_deferred_projection_target(PlannerInfo *root,
                                                            Path *path,
                                                            PathTarget *target,
                                                            bool prune_join_child_targets);
static PathTarget *build_child_deferred_projection_target(PlannerInfo *root,
                                                          Path *child_path,
                                                          PathTarget *target,
                                                          List *joinrestrictinfo,
                                                          bool preserve_label_core,
                                                          bool prune_child_target);
static void add_child_rel_vars_to_pathtarget(PathTarget *child_target,
                                             Path *child_path,
                                             Node *node);
static void add_child_join_clause_vars_to_pathtarget(PathTarget *child_target,
                                                     Path *child_path,
                                                     List *joinrestrictinfo);
static void add_child_label_core_vars_to_pathtarget(PlannerInfo *root,
                                                    PathTarget *child_target,
                                                    Path *child_path);
static void add_child_label_var_to_pathtarget(PlannerInfo *root,
                                              PathTarget *child_target,
                                              Index varno,
                                              AttrNumber attno,
                                              Oid vartype);
static bool target_contains_edge_ctid(PlannerInfo *root, PathTarget *target);
static List *join_path_clause_sources(Path *path);
static bool expression_belongs_to_child_relids(Node *expr, Relids relids);
static bool pathtarget_contains_expr(PathTarget *target, Node *expr);
static Path *copy_projection_capable_path_with_target(PlannerInfo *root,
                                                      Path *path,
                                                      PathTarget *target);
static Path *copy_path_node_shallow(Path *path);
static void add_deferred_count_agtype_projection_path(
    PlannerInfo *root, RelOptInfo *output_rel, FinalPathExtraData *extra);
static void add_deferred_count_agtype_plain_paths(PlannerInfo *root,
                                                  RelOptInfo *output_rel,
                                                  PathTarget *lower_target,
                                                  PathTarget *final_target);
static bool is_age_vle_values_rte(RangeTblEntry *rte, List **func_args);
static void add_age_vle_stream_custom_path(PlannerInfo *root, RelOptInfo *rel,
                                           RangeTblEntry *rte,
                                           List *func_args,
                                           const char *graph_pattern_key);
static void cost_age_vle_stream_custom_path(CustomPath *cp,
                                            Path *reference_path,
                                            List *range_direction,
                                            List *edge_source);
static AgeGraphJoinLoweringArtifact *make_vle_stream_graph_join_artifact(
    PlannerInfo *root, RelOptInfo *rel, CustomPath *cp, List *func_args,
    List *edge_source, const char *graph_pattern_key);
static AgeGraphJoinLoweringInput make_vle_stream_graph_join_input(
    PlannerInfo *root, RelOptInfo *rel, CustomPath *cp, List *func_args);
static void add_vle_stream_expand_candidate(
    AgeGraphJoinCandidateTable *table, CustomPath *cp,
    const AgeGraphJoinLoweringInput *lowering_input,
    AgeGraphJoinBoundKind bound_kind,
    AgeGraphJoinConnectorKind connector_kind,
    AgeGraphJoinOrderPropertyKind order_property_kind,
    AgeGraphJoinSourceEvidenceKind source_evidence_kind,
    bool has_bound_endpoints, double base_fanout);
static double vle_stream_matrix_frontier_scheduled_work(
    List *edge_source, VLESourceDirectionClass direction_id,
    double base_fanout,
    double path_rows, double composite_fanout,
    AgeVLEStreamCompositePlanKind composite_plan_kind);
static bool vle_stream_matrix_frontier_has_prescan_compact(
    List *edge_source, double *compact_fanout);
static AgeGraphJoinOrderPropertyKind vle_stream_base_join_order_property_kind(
    List *func_args, List *edge_source);
static bool vle_stream_func_args_have_bound_endpoints(List *func_args);
static AgeGraphJoinBoundKind graph_join_bound_kind_from_vle_direction(
    VLESourceDirectionClass direction);
static List *make_age_vle_stream_const_flags(List *func_args);
static List *make_age_vle_stream_graph(List *func_args);
static List *make_age_vle_stream_edge(List *func_args);
static List *make_age_vle_stream_range_direction(List *func_args);
static List *make_age_vle_stream_output(List *func_args);
static List *make_age_vle_stream_edge_source(List *graph, List *edge,
                                             List *range_direction,
                                             List *output,
                                             List *func_args,
                                             List *restrictinfos,
                                             Node **terminal_property_predicate_expr,
                                             PlannerInfo *root);
static int64 choose_vle_stream_terminal_candidate_fanout(
    const VLESourceFanoutEvidence *source_evidence, cypher_rel_dir direction);
static int64 clamp_vle_stream_composite_fanout(int64 candidate_fanout,
                                               double selectivity);
static GraphPropertySourceSelectivity estimate_graph_property_source_selectivity(
    Oid property_index_oid, double fallback_selectivity,
    Const *property_value);
static bool estimate_graph_property_index_mcv_selectivity(Oid property_index_oid,
                                                          Const *property_value,
                                                          double *selectivity);
static bool estimate_graph_property_index_distinct_values(Oid property_index_oid,
                                                          double reltuples,
                                                          double *distinct_values);
static int64 graph_property_selectivity_ppm(double selectivity);
static void get_vle_stream_edge_source_indexes(
    const char *graph_name, const char *label_name,
    Oid *edge_label_oid,
    Oid *age_adjacency_out_index_oid,
    Oid *age_adjacency_in_index_oid,
    bool *adjacency_out, bool *adjacency_in,
    bool *endpoint_start, bool *endpoint_end);
static void apply_vle_stream_directory_fanout_evidence(
    VLESourceFanoutEvidence *source_evidence, List *func_args,
    Oid age_adjacency_out_index_oid, Oid age_adjacency_in_index_oid,
    int32 terminal_label_id, PlannerInfo *root);
static bool find_age_vle_stream_endpoint_graphid_const(
    PlannerInfo *root, Node *node, graphid *value, bool *isnull);
static bool vle_stream_age_adjacency_index_matches(Relation index_rel,
                                                   bool outgoing);
static AgeVLEStreamDirectedSourceKind
age_vle_stream_directed_source_from_traversal(
    VLETraversalSourceKind source_kind);
static bool get_age_vle_stream_integer_const(Node *node, int64 *value,
                                             bool *isnull);
static bool get_age_vle_stream_vertex_id_const(Node *node, graphid *value,
                                               bool *isnull);
static bool get_age_vle_stream_string_const(Node *node, char **value,
                                            bool *isnull);
static Const *make_int8_const(int64 value);
static Const *make_text_const(const char *value);
static Const *make_agtype_const(agtype *value);
static Plan *plan_age_vle_stream_path(PlannerInfo *root, RelOptInfo *rel,
                                      CustomPath *best_path, List *tlist,
                                      List *clauses, List *custom_plans);
static Plan *plan_age_property_projection_path(PlannerInfo *root,
                                               RelOptInfo *rel,
                                               CustomPath *best_path,
                                               List *tlist, List *clauses,
                                               List *custom_plans);
static const CustomPathMethods age_adjacency_match_path_methods = {
    AGE_ADJACENCY_MATCH_SCAN_NAME,
    plan_age_adjacency_match_path,
    NULL};

static const CustomPathMethods age_property_projection_path_methods = {
    AGE_PROPERTY_PROJECTION_SCAN_NAME,
    plan_age_property_projection_path,
    NULL};

static const CustomPathMethods age_vle_stream_path_methods = {
    AGE_VLE_STREAM_SCAN_NAME,
    plan_age_vle_stream_path,
    NULL};

void cypher_clear_adjacency_match_candidates(void)
{
    adjacency_match_candidates = NIL;
    graph_pattern_registry = NIL;
}

void cypher_register_graph_pattern_handoff(const char *graph_pattern_key)
{
    (void) ensure_graph_pattern_registry_entry(graph_pattern_key);
}

void cypher_declare_graph_pattern_source(
    const char *graph_pattern_key, AgeGraphJoinSourceKind source_kind_id,
    AgeGraphJoinComponentKind component_family_kind)
{
    graph_pattern_registry_declare_typed_source(
        graph_pattern_key, source_kind_id, component_family_kind);
}

void cypher_register_vle_pattern_handoff(const char *marker_alias,
                                         const char *graph_pattern_key)
{
    CypherGraphPatternRegistryEntry *entry;

    if (marker_alias == NULL || graph_pattern_key == NULL)
        return;

    entry = ensure_graph_pattern_registry_entry(graph_pattern_key);
    graph_pattern_registry_add_text(&entry->vle_marker_aliases,
                                    marker_alias);
}

void cypher_register_node_pattern_handoff(const char *node_alias,
                                          const char *graph_pattern_key)
{
    CypherGraphPatternRegistryEntry *entry;

    if (node_alias == NULL || graph_pattern_key == NULL)
        return;

    entry = ensure_graph_pattern_registry_entry(graph_pattern_key);
    graph_pattern_registry_add_text(&entry->node_aliases, node_alias);
}

void cypher_register_adjacency_match_candidate(Oid edge_label_oid,
                                               Oid index_oid,
                                               Oid graph_oid,
                                               const char *edge_alias,
                                               const char *graph_pattern_key,
                                               const char *bound_endpoint_alias,
                                               Node *bound_endpoint_expr,
                                               const char *index_source,
                                               AgeAdjacencyMatchIndexKind index_kind_id,
                                               const char *index_provider,
                                               AgeAdjacencyMatchIndexDirection index_direction_id,
                                               int32 index_property_count,
                                               bool index_metadata_backed,
                                               const char *right_property_key,
                                               Oid right_property_index_oid,
                                               const char *right_property_index_source,
                                               const char *right_property_index_provider,
                                               const char *right_property_index_type,
                                               bool right_property_index_metadata_backed,
                                               Const *right_property_value,
                                               Node *right_property_value_expr,
                                               bool outgoing,
                                               bool has_edge_variable_projection,
                                               bool has_edge_property_predicate,
                                               bool has_right_label_constraint,
                                               bool has_right_property_predicate,
                                               int32 right_label_id,
                                               AttrNumber endpoint_attno)
{
    MemoryContext oldcontext;
    CypherAdjacencyMatchCandidate *candidate;

    if (!OidIsValid(edge_label_oid) ||
        !OidIsValid(index_oid) ||
        edge_alias == NULL)
    {
        return;
    }

    /*
     * The planner hook can be invoked while planning surrounding SQL after the
     * Cypher query has been analyzed. Keep candidate storage out of the
     * per-statement parser context, and remove entries as soon as the matching
     * edge RTE reaches set_rel_pathlist().
     */
    oldcontext = MemoryContextSwitchTo(TopTransactionContext);
    (void) ensure_graph_pattern_registry_entry(graph_pattern_key);

    candidate = palloc0(sizeof(*candidate));
    candidate->graph_oid = graph_oid;
    candidate->edge_label_oid = edge_label_oid;
    candidate->index_oid = index_oid;
    candidate->edge_alias = pstrdup(edge_alias);
    candidate->graph_pattern_key = graph_pattern_key != NULL ?
        pstrdup(graph_pattern_key) : NULL;
    candidate->bound_endpoint_alias = bound_endpoint_alias != NULL ?
        pstrdup(bound_endpoint_alias) : NULL;
    candidate->bound_endpoint_expr = copyObject(bound_endpoint_expr);
    candidate->index_source = index_source != NULL ?
        pstrdup(index_source) : NULL;
    candidate->index_kind_id = index_kind_id;
    candidate->index_provider = index_provider != NULL ?
        pstrdup(index_provider) : NULL;
    candidate->index_direction_id = index_direction_id;
    candidate->index_property_count = index_property_count;
    candidate->index_metadata_backed = index_metadata_backed;
    candidate->right_property_key = right_property_key != NULL ?
        pstrdup(right_property_key) : NULL;
    candidate->right_property_index_oid = right_property_index_oid;
    candidate->right_property_index_source =
        right_property_index_source != NULL ?
        pstrdup(right_property_index_source) : NULL;
    candidate->right_property_index_provider =
        right_property_index_provider != NULL ?
        pstrdup(right_property_index_provider) : NULL;
    candidate->right_property_index_type =
        right_property_index_type != NULL ?
        pstrdup(right_property_index_type) : pstrdup("agtype");
    candidate->right_property_index_metadata_backed =
        right_property_index_metadata_backed;
    candidate->right_property_value = right_property_value != NULL ?
        copyObject(right_property_value) : NULL;
    candidate->right_property_value_expr = right_property_value_expr != NULL ?
        copyObject(right_property_value_expr) : NULL;
    candidate->right_property_prefetch_eligible =
        adjacency_match_has_terminal_property_prefetch(candidate);
    candidate->right_property_value_kind_id =
        adjacency_match_terminal_property_value_kind(candidate);
    candidate->outgoing = outgoing;
    candidate->has_edge_variable_projection = has_edge_variable_projection;
    candidate->has_edge_property_predicate = has_edge_property_predicate;
    candidate->has_right_label_constraint = has_right_label_constraint;
    candidate->has_right_property_predicate = has_right_property_predicate;
    candidate->right_label_id = right_label_id;
    candidate->terminal_source_strategy_id =
        adjacency_match_terminal_source_strategy(candidate);
    candidate->endpoint_attno = endpoint_attno;

    adjacency_match_candidates = lappend(adjacency_match_candidates,
                                         candidate);

    MemoryContextSwitchTo(oldcontext);
}

void set_rel_pathlist_init(void)
{
    prev_set_rel_pathlist_hook = set_rel_pathlist_hook;
    set_rel_pathlist_hook = set_rel_pathlist;
    prev_set_join_pathlist_hook = set_join_pathlist_hook;
    set_join_pathlist_hook = set_join_pathlist;
    prev_create_upper_paths_hook = create_upper_paths_hook;
    create_upper_paths_hook = create_upper_paths;
    RegisterCustomScanMethods(&age_adjacency_match_scan_methods);
    RegisterCustomScanMethods(&age_property_projection_scan_methods);
    RegisterCustomScanMethods(&age_vle_stream_scan_methods);
    if (!cypher_path_xact_callback_registered)
    {
        RegisterXactCallback(cypher_path_xact_callback, NULL);
        cypher_path_xact_callback_registered = true;
    }
}

void set_rel_pathlist_fini(void)
{
    create_upper_paths_hook = prev_create_upper_paths_hook;
    set_join_pathlist_hook = prev_set_join_pathlist_hook;
    set_rel_pathlist_hook = prev_set_rel_pathlist_hook;
    if (cypher_path_xact_callback_registered)
    {
        UnregisterXactCallback(cypher_path_xact_callback, NULL);
        cypher_path_xact_callback_registered = false;
    }
}

static void cypher_path_xact_callback(XactEvent event, void *arg)
{
    switch (event)
    {
    case XACT_EVENT_COMMIT:
    case XACT_EVENT_ABORT:
    case XACT_EVENT_PARALLEL_COMMIT:
    case XACT_EVENT_PARALLEL_ABORT:
        cypher_clear_adjacency_match_candidates();
        break;
    default:
        break;
    }
}

void cypher_rewrite_property_index_surfaces(Query *parse)
{
    ListCell *lc;
    Index rti = 1;

    if (parse == NULL)
    {
        return;
    }

    foreach(lc, parse->rtable)
    {
        RangeTblEntry *rte = lfirst_node(RangeTblEntry, lc);
        PropertyIndexSurfaceContext context;

        if (rte->rtekind == RTE_SUBQUERY && rte->subquery != NULL)
        {
            cypher_rewrite_property_index_surfaces(rte->subquery);
            rti++;
            continue;
        }

        if (parse->jointree == NULL || parse->jointree->quals == NULL)
        {
            rti++;
            continue;
        }

        if (rte->rtekind != RTE_RELATION || !OidIsValid(rte->relid))
        {
            rti++;
            continue;
        }

        context.parse = parse;
        context.rti = rti;
        context.rte = rte;
        parse->jointree->quals = (Node *)expression_tree_mutator(
            parse->jointree->quals, rewrite_property_index_surface_mutator,
            &context);

        rti++;
    }
}

static Node *rewrite_property_index_surface_mutator(Node *node, void *context)
{
    PropertyIndexSurfaceContext *surface_context;
    OpExpr *op;
    Node *left;
    Node *right;
    CypherPropertyIndexHandoff index_handoff;

    if (node == NULL)
        return NULL;
    if (context == NULL)
        return expression_tree_mutator(node,
                                       rewrite_property_index_surface_mutator,
                                       context);

    surface_context = context;

    if (!IsA(node, OpExpr))
    {
        return expression_tree_mutator(node,
                                       rewrite_property_index_surface_mutator,
                                       context);
    }

    op = castNode(OpExpr, node);
    if (list_length(op->args) != 2)
        return expression_tree_mutator(node,
                                       rewrite_property_index_surface_mutator,
                                       context);

    left = linitial(op->args);
    right = lsecond(op->args);

    if (cypher_find_matching_property_index_handoff_for_rte(
            surface_context->rte, surface_context->rti, left,
            &index_handoff))
    {
        Node *index_expr;
        const char *aliasname = NULL;
        char *graph_pattern_key;

        if (surface_context->rte->alias != NULL)
            aliasname = surface_context->rte->alias->aliasname;
        else if (surface_context->rte->eref != NULL)
            aliasname = surface_context->rte->eref->aliasname;
        graph_pattern_key = find_graph_pattern_by_node_alias(aliasname);
        register_index_pattern_reference(index_handoff.index_oid,
                                         graph_pattern_key);
        index_expr = cypher_make_property_index_handoff_expr(&index_handoff);
        if (index_expr != NULL && !equal(index_expr, left))
            return cypher_replace_property_index_side(op, true, index_expr);
    }

    if (cypher_find_matching_property_index_handoff_for_rte(
            surface_context->rte, surface_context->rti, right,
            &index_handoff))
    {
        Node *index_expr;
        const char *aliasname = NULL;
        char *graph_pattern_key;

        if (surface_context->rte->alias != NULL)
            aliasname = surface_context->rte->alias->aliasname;
        else if (surface_context->rte->eref != NULL)
            aliasname = surface_context->rte->eref->aliasname;
        graph_pattern_key = find_graph_pattern_by_node_alias(aliasname);
        register_index_pattern_reference(index_handoff.index_oid,
                                         graph_pattern_key);
        index_expr = cypher_make_property_index_handoff_expr(&index_handoff);
        if (index_expr != NULL && !equal(index_expr, right))
            return cypher_replace_property_index_side(op, false, index_expr);
    }

    return expression_tree_mutator(node, rewrite_property_index_surface_mutator,
                                   context);
}

static void set_rel_pathlist(PlannerInfo *root, RelOptInfo *rel, Index rti,
                             RangeTblEntry *rte)
{
    CypherAdjacencyMatchCandidate *candidate;
    const char *aliasname = NULL;
    char *node_graph_pattern_key = NULL;

    if (prev_set_rel_pathlist_hook)
        prev_set_rel_pathlist_hook(root, rel, rti, rte);

    age_graph_join_metadata_begin(root);
    if (rte != NULL)
    {
        if (rte->alias != NULL)
            aliasname = rte->alias->aliasname;
        else if (rte->eref != NULL)
            aliasname = rte->eref->aliasname;
    }
    node_graph_pattern_key = find_graph_pattern_by_node_alias(aliasname);
    graph_pattern_registry_register_declared_cover(root, rel,
                                                   node_graph_pattern_key);

    if (cypher_rewrite_property_equals_restrictions(root, rel, rti))
    {
        cypher_canonicalize_property_index_predicates(rel);
        check_index_predicates(root, rel);
        cypher_canonicalize_property_index_restrictions(rel);
        create_index_paths(root, rel);
    }

    /*
     * Add a custom path for every candidate registered against this edge, not
     * just the first.  When more than one direction is viable for the same edge
     * (a forward expansion from a bound source and a reverse expansion from a
     * selective terminal), each becomes a competing path and add_path() keeps
     * the cheaper one -- the basis for cost-based direction selection (item 10).
     * With a single candidate this loops once, unchanged from before.
     */
    while ((candidate = pop_adjacency_match_candidate(root, rte)) != NULL)
    {
        graph_pattern_registry_register_declared_cover(
            root, rel, candidate->graph_pattern_key);
        ereport(DEBUG2,
                (errmsg_internal("AGE adjacency MATCH candidate visible to planner: "
                             "edge_rel=%u index=%u alias=%s direction=%s "
                             "endpoint_attno=%d endpoint_alias=%s "
                             "endpoint_rti=%u edge_var=%s edge_props=%s "
                             "right_label=%s right_props=%s "
                             "index_source=%s index_kind=%s provider=%s "
                             "metadata=%s pattern=%s right_property_key=%s "
                             "right_property_index=%s right_property_value=%s "
                             "endpoint_expr=%s",
                                 candidate->edge_label_oid,
                                 candidate->index_oid,
                                 candidate->edge_alias,
                                 candidate->outgoing ? "outgoing" : "incoming",
                                 candidate->endpoint_attno,
                                 candidate->bound_endpoint_alias != NULL ?
                                 candidate->bound_endpoint_alias : "<none>",
                                 candidate->bound_endpoint_rti,
                                 candidate->has_edge_variable_projection ?
                                 "true" : "false",
                                 candidate->has_edge_property_predicate ?
                                 "true" : "false",
                                 candidate->has_right_label_constraint ?
                                 "true" : "false",
                                 candidate->has_right_property_predicate ?
                                 "true" : "false",
                                 candidate->index_source != NULL ?
                                 candidate->index_source : "unknown",
                                 age_adjacency_match_index_kind_name(
                                     candidate->index_kind_id),
                                 candidate->index_provider != NULL ?
                                 candidate->index_provider : "unknown",
                                 candidate->index_metadata_backed ?
                                 "true" : "false",
                                 candidate->graph_pattern_key != NULL ?
                                 candidate->graph_pattern_key :
                                 "<none>",
                                 candidate->right_property_key != NULL ?
                                 candidate->right_property_key : "<none>",
                                 candidate->right_property_index_source != NULL ?
                                 candidate->right_property_index_source :
                                 "<none>",
                                 candidate->right_property_value != NULL ?
                                 nodeToString(candidate->right_property_value) :
                                 "<none>",
                                 candidate->bound_endpoint_expr != NULL ?
                                 nodeToString(candidate->bound_endpoint_expr) :
                                 "<none>")));
        add_adjacency_match_custom_path(root, rel, candidate);
    }

    if (rte != NULL)
    {
        List *vle_values_args = NIL;

        if (is_age_vle_values_rte(rte, &vle_values_args))
        {
            char *graph_pattern_key;

            graph_pattern_key = find_graph_pattern_by_vle_marker(rte);
            graph_pattern_registry_register_declared_cover(
                root, rel, graph_pattern_key);
            add_age_vle_stream_custom_path(root, rel, rte, vle_values_args,
                                           graph_pattern_key);
        }
    }

    switch (get_cypher_clause_kind(rte))
    {
    case CYPHER_CLAUSE_CREATE:
        replace_with_cypher_dml_path(root, rel, rte, create_cypher_create_path);
        break;
    case CYPHER_CLAUSE_SET:
        replace_with_cypher_dml_path(root, rel, rte, create_cypher_set_path);
        break;
    case CYPHER_CLAUSE_DELETE:
        replace_with_cypher_dml_path(root, rel, rte, create_cypher_delete_path);
        break;
    case CYPHER_CLAUSE_MERGE:
        replace_with_cypher_dml_path(root, rel, rte, create_cypher_merge_path);
        break;
    case CYPHER_CLAUSE_NONE:
        break;
    default:
        ereport(ERROR, (errmsg_internal("invalid cypher_clause_kind")));
    }

    register_graph_join_index_path_candidates(root, rel);
    age_graph_join_refresh_rel_metadata(root, rel,
                                        path_get_graph_join_rel_evidence);
}

static void set_join_pathlist(PlannerInfo *root, RelOptInfo *joinrel,
                              RelOptInfo *outerrel, RelOptInfo *innerrel,
                              JoinType jointype, JoinPathExtraData *extra)
{
    age_graph_join_metadata_begin(root);
    age_graph_join_refresh_rel_metadata(root, outerrel,
                                        path_get_graph_join_rel_evidence);
    age_graph_join_refresh_rel_metadata(root, innerrel,
                                        path_get_graph_join_rel_evidence);

    if (prev_set_join_pathlist_hook)
        prev_set_join_pathlist_hook(root, joinrel, outerrel, innerrel,
                                    jointype, extra);

    add_graph_join_candidate_nestloop_path(root, joinrel, outerrel, innerrel,
                                           jointype, extra);
    adjust_graph_expansion_join_rows(root, joinrel, jointype);
    log_graph_expansion_join_paths(joinrel, outerrel, innerrel, jointype);
    age_graph_join_refresh_rel_metadata(root, joinrel,
                                        path_get_graph_join_rel_evidence);
}

static void create_upper_paths(PlannerInfo *root, UpperRelationKind stage,
                               RelOptInfo *input_rel,
                               RelOptInfo *output_rel, void *extra)
{
    List *property_slots = NIL;
    const char *target_source = NULL;

    if (prev_create_upper_paths_hook != NULL)
        prev_create_upper_paths_hook(root, stage, input_rel, output_rel, extra);

    if (stage == UPPERREL_GROUP_AGG)
    {
        if (cypher_rewrite_property_access_aggregate_pathtarget(
                output_rel->reltarget))
        {
            ereport(DEBUG2,
                    (errmsg_internal("AGE property access aggregate rewritten")));
        }
        add_narrow_typed_collect_paths(root, output_rel);
        add_narrow_array_agg_property_paths(root, output_rel);
        return;
    }

    if (stage != UPPERREL_FINAL)
        return;

    add_deferred_count_agtype_projection_path(root, output_rel,
                                             (FinalPathExtraData *)extra);
    add_deferred_ordered_property_projection_path(root, input_rel, output_rel,
                                                 (FinalPathExtraData *)extra);

    if (cypher_detect_simple_property_projection(root, input_rel, output_rel,
                                                 &property_slots,
                                                 &target_source))
    {
        ereport(DEBUG2,
                (errmsg_internal("AGE simple property projection visible after "
                                 "scan/join target: input_paths=%d "
                                 "output_paths=%d target_source=%s "
                                 "slot_count=%d",
                                 list_length(input_rel->pathlist),
                                 list_length(output_rel->pathlist),
                                 target_source,
                                 list_length(property_slots))));
        add_property_projection_custom_path(root, input_rel, output_rel,
                                            property_slots,
                                            (FinalPathExtraData *)extra);
    }
}

static void log_graph_expansion_join_paths(RelOptInfo *joinrel,
                                           RelOptInfo *outerrel,
                                           RelOptInfo *innerrel,
                                           JoinType jointype)
{
    ListCell *lc;

    if (joinrel == NULL ||
        joinrel->pathlist == NIL)
    {
        return;
    }

    foreach(lc, joinrel->pathlist)
    {
        Path *path = lfirst(lc);
        JoinPath *joinpath;
        Path *outer_path;
        Path *inner_path;

        if (!IsA(path, NestPath))
            continue;

        joinpath = (JoinPath *)path;
        outer_path = joinpath->outerjoinpath;
        inner_path = joinpath->innerjoinpath;

        if (!path_contains_graph_expansion(outer_path) &&
            !path_contains_graph_expansion(inner_path))
        {
            continue;
        }

        ereport(DEBUG2,
                (errmsg_internal("AGE graph expansion join path visible: "
                                 "jointype=%d joinrelids=%s outerrelids=%s "
                                 "innerrelids=%s joinrel_rows=%.0f "
                                 "path_rows=%.0f path_required_outer=%s "
                                 "outer_rows=%.0f outer_required_outer=%s "
                                 "inner_rows=%.0f inner_required_outer=%s",
                                 (int)jointype,
                                 bmsToString(joinrel->relids),
                                 outerrel != NULL ?
                                 bmsToString(outerrel->relids) : "<none>",
                                 innerrel != NULL ?
                                 bmsToString(innerrel->relids) : "<none>",
                                 joinrel->rows,
                                 path->rows,
                                 path_param_info_string(path),
                                 outer_path != NULL ? outer_path->rows : 0,
                                 path_param_info_string(outer_path),
                                 inner_path != NULL ? inner_path->rows : 0,
                                 path_param_info_string(inner_path))));
    }
}

static bool path_get_graph_join_rel_evidence(
    Path *path, AgeGraphJoinPathEvidence *evidence)
{
    AgeGraphJoinPathEvidence local_evidence;

    if (path == NULL)
        return false;
    if (evidence == NULL)
        evidence = &local_evidence;
    age_graph_join_init_path_evidence(evidence);

    if (IsA(path, CustomPath))
    {
        CustomPath *custom_path = (CustomPath *)path;

        if (custom_path->methods == &age_adjacency_match_path_methods)
        {
            if (!custom_path_adjacency_join_evidence(custom_path, evidence))
                return false;
        }
        else if (custom_path->methods == &age_vle_stream_path_methods)
        {
            if (!custom_path_vle_join_evidence(custom_path, evidence))
                return false;
        }
        else
            return false;

        age_graph_join_complete_path_evidence(path, evidence);
        return evidence->order_property_kind != AGE_GRAPH_JOIN_ORDER_UNKNOWN;
    }

    if (!IsA(path, CustomPath))
    {
        AgeGraphJoinOrderPropertyKind order_property_kind;

        order_property_kind = index_path_join_order_property_kind(path);
        if (order_property_kind != AGE_GRAPH_JOIN_ORDER_UNKNOWN)
        {
            evidence->component_kind =
                AGE_GRAPH_JOIN_COMPONENT_NODE_PROPERTY_SEEK;
            evidence->connector_kind =
                AGE_GRAPH_JOIN_CONNECTOR_POSTGRES_INDEX_SEEK;
            evidence->order_property_kind = order_property_kind;
            evidence->source_evidence_kind =
                AGE_GRAPH_JOIN_SOURCE_EVIDENCE_POSTGRES_INDEX;
            evidence->source_kind_id =
                AGE_GRAPH_JOIN_SOURCE_NODE_PROPERTY_INDEX;
            evidence->candidate_count = 1;
            evidence->selected_total_cost = path->total_cost;
            evidence->next_total_cost = 0;
            evidence->bound = age_graph_join_order_property_kind_is_bound(
                evidence->order_property_kind);
            age_graph_join_complete_path_evidence(path, evidence);
            return true;
        }
    }

    if (IsA(path, MaterialPath))
        return path_get_graph_join_rel_evidence(
            ((MaterialPath *)path)->subpath, evidence);

    if (IsA(path, GatherPath))
    {
        GatherPath *gather_path = (GatherPath *)path;

        if (!path_get_graph_join_rel_evidence(gather_path->subpath,
                                              evidence))
        {
            return false;
        }
        evidence->selected_total_cost = path->total_cost;
        evidence->required_outer = PATH_REQ_OUTER(path);
        if (path->parent != NULL)
            evidence->provided_relids = path->parent->relids;
        evidence->parallel_safe = path->parallel_safe;
        evidence->parallel_aware = gather_path->subpath->parallel_aware;
        evidence->parallel_workers = gather_path->subpath->parallel_workers;
        evidence->order_preserving = path->pathkeys != NIL;
        evidence->physical_properties_known = true;
        age_graph_join_complete_path_evidence(path, evidence);
        return evidence->order_property_kind != AGE_GRAPH_JOIN_ORDER_UNKNOWN;
    }

    if (IsA(path, MemoizePath))
        return path_get_graph_join_rel_evidence(
            ((MemoizePath *)path)->subpath, evidence);

    if (IsA(path, ProjectionPath))
        return path_get_graph_join_rel_evidence(
            ((ProjectionPath *)path)->subpath, evidence);

    if (IsA(path, SubqueryScanPath))
        return path_get_graph_join_rel_evidence(
            ((SubqueryScanPath *)path)->subpath, evidence);

    return false;
}

static void register_graph_join_index_path_candidates(PlannerInfo *root,
                                                      RelOptInfo *rel)
{
    ListCell *lc;
    RangeTblEntry *rte;

    if (root == NULL ||
        rel == NULL ||
        rel->pathlist == NIL ||
        rel->relid == 0 ||
        rel->relid >= root->simple_rel_array_size)
    {
        return;
    }

    /*
     * Only graph label relations participate in graph-join lowering.  Plain
     * relations -- catalog tables like ag_graph or the ag_graph_edge_stats
     * stats cache, reached while planning a nested SPI query in the same
     * transaction -- must be skipped, or the lowering artifact (kept in a
     * transaction-lifetime registry) is re-entered with stale cross-statement
     * state and crashes the backend.
     */
    rte = root->simple_rte_array[rel->relid];
    if (rte == NULL || rte->rtekind != RTE_RELATION)
    {
        return;
    }

    /*
     * Skip when AGE's own catalogs are absent.  This hook stays installed for
     * the whole session, so it still fires while planning ordinary queries
     * after DROP EXTENSION age; get_graph_oid_for_table() then reaches
     * ag_relation_id("ag_label"), which ereports "table ag_label does not
     * exist".  A missing-OK lookup avoids that and correctly treats a dropped
     * extension as "no graph relations to lower".
     */
    {
        Oid ag_catalog_ns = LookupNamespaceNoError("ag_catalog");

        if (!OidIsValid(ag_catalog_ns) ||
            !OidIsValid(get_relname_relid("ag_label", ag_catalog_ns)))
        {
            return;
        }
    }

    if (!OidIsValid(get_graph_oid_for_table(rte->relid)))
    {
        return;
    }

    foreach(lc, rel->pathlist)
    {
        Path *path = lfirst(lc);
        AgeGraphJoinLoweringArtifact *artifact;
        AgeGraphJoinLoweringArtifact *shared_artifact;
        AgeGraphJoinCandidateTable *bound_table;

        artifact = make_index_path_graph_join_artifact(
            rel, path, find_graph_pattern_by_index_oid(
                graph_join_index_path_oid(path)));
        if (artifact == NULL)
            continue;

        shared_artifact = age_graph_join_register_rel_lowering_artifact_pool(
            root, rel, artifact);
        if (shared_artifact == NULL)
            shared_artifact = artifact;
        bound_table = age_graph_join_register_rel_lowering_artifact_path(
            root, rel, path, shared_artifact);
        if (bound_table == NULL)
            continue;
    }
}

static AgeGraphJoinLoweringArtifact *make_index_path_graph_join_artifact(
    RelOptInfo *rel, Path *path, const char *graph_pattern_key)
{
    AgeGraphJoinLoweringArtifact *artifact;
    AgeGraphJoinCandidateTable *table;
    AgeGraphJoinLoweringInput input;
    const char *effective_pattern_key;
    AgeGraphJoinOrderPropertyKind order_property_kind;

    order_property_kind = index_path_join_order_property_kind(path);
    if (order_property_kind == AGE_GRAPH_JOIN_ORDER_UNKNOWN)
        return NULL;

    effective_pattern_key = graph_pattern_key != NULL ?
        graph_pattern_key : "graph-pattern";
    artifact = graph_pattern_registry_artifact(effective_pattern_key);
    if (artifact == NULL)
        return NULL;

    table = age_graph_join_make_candidate_table();
    input = make_index_path_graph_join_input(rel, path);
    (void) age_graph_join_table_add_lowered_typed_path_candidate(
        table, path, &input,
        age_graph_join_order_property_kind_is_bound(
            order_property_kind) ?
        AGE_GRAPH_JOIN_BOUND_BOUND : AGE_GRAPH_JOIN_BOUND_UNBOUND,
        AGE_GRAPH_JOIN_COMPONENT_NODE_PROPERTY_SEEK,
        AGE_GRAPH_JOIN_CONNECTOR_POSTGRES_INDEX_SEEK,
        AGE_GRAPH_JOIN_ORDER_INDEX_ANCHORED,
        AGE_GRAPH_JOIN_SOURCE_EVIDENCE_POSTGRES_INDEX,
        AGE_GRAPH_JOIN_SOURCE_NODE_PROPERTY_INDEX, false);

    age_graph_join_lowering_artifact_add_typed_table(
        artifact, AGE_GRAPH_JOIN_SOURCE_NODE_PROPERTY_INDEX,
        AGE_GRAPH_JOIN_COMPONENT_NODE_PROPERTY_SEEK, table);

    return artifact;
}

static AgeGraphJoinLoweringInput make_index_path_graph_join_input(
    RelOptInfo *rel, Path *path)
{
    AgeGraphJoinLoweringInput input;

    memset(&input, 0, sizeof(input));
    input.solved_relids = rel != NULL ? rel->relids : NULL;
    input.required_outer = path != NULL ? PATH_REQ_OUTER(path) : NULL;
    input.provided_relids = rel != NULL ? rel->relids : NULL;
    if (path != NULL && path->pathtarget != NULL &&
        path->pathtarget->width > 0)
        input.output_width = path->pathtarget->width;
    else if (rel != NULL && rel->reltarget != NULL)
        input.output_width = rel->reltarget->width;

    return input;
}

static void register_graph_join_joinrel_lowering_evidence(
    PlannerInfo *root, RelOptInfo *joinrel, Path *path,
    const AgeGraphJoinPathEvidence *evidence)
{
    AgeGraphJoinCandidateTable *table;

    if (root == NULL ||
        joinrel == NULL ||
        path == NULL ||
        evidence == NULL ||
        evidence->order_property_kind == AGE_GRAPH_JOIN_ORDER_UNKNOWN)
    {
        return;
    }

    table = age_graph_join_make_candidate_table();
    (void) age_graph_join_table_add_path_evidence_candidate(
        table, path, evidence);
    age_graph_join_register_rel_lowering_table(root, joinrel, path, table);
}

static void add_graph_join_candidate_nestloop_path(PlannerInfo *root,
                                                   RelOptInfo *joinrel,
                                                   RelOptInfo *outerrel,
                                                   RelOptInfo *innerrel,
                                                   JoinType jointype,
                                                   JoinPathExtraData *extra)
{
    AgeGraphJoinRelMetadata *outer_metadata;
    AgeGraphJoinRelMetadata *inner_metadata;
    ListCell *outer_lc;
    List *component_candidates = NIL;
    ListCell *candidate_lc;
    int skipped_outer_candidates = 0;
    int skipped_inner_candidates = 0;

    if (root == NULL ||
        joinrel == NULL ||
        outerrel == NULL ||
        innerrel == NULL ||
        extra == NULL ||
        outerrel->pathlist == NIL ||
        innerrel->pathlist == NIL ||
        jointype == JOIN_RIGHT ||
        jointype == JOIN_RIGHT_SEMI ||
        jointype == JOIN_RIGHT_ANTI ||
        jointype == JOIN_FULL)
    {
        return;
    }

    if (!age_graph_join_metadata_matches_root(root))
        return;

    outer_metadata = age_graph_join_get_rel_metadata(outerrel, false);
    inner_metadata = age_graph_join_get_rel_metadata(innerrel, false);
    if (outer_metadata == NULL ||
        inner_metadata == NULL ||
        outer_metadata->candidates == NIL ||
        inner_metadata->candidates == NIL)
    {
        return;
    }

    foreach(outer_lc, outer_metadata->component_candidates)
    {
        AgeGraphJoinRelComponentCandidate *outer_component = lfirst(outer_lc);
        ListCell *inner_lc;

        foreach(inner_lc, inner_metadata->component_candidates)
        {
            AgeGraphJoinRelComponentCandidate *inner_component =
                lfirst(inner_lc);

            graph_join_consider_nestloop_candidate_pair(
                outerrel, innerrel, outer_component->path,
                inner_component->path, &outer_component->evidence,
                &inner_component->evidence, &component_candidates);
        }

        foreach(inner_lc, inner_metadata->candidates)
        {
            AgeGraphJoinRelCandidate *inner_rel_candidate = lfirst(inner_lc);

            if (graph_join_rel_candidate_is_component_covered(
                    inner_metadata, inner_rel_candidate))
            {
                skipped_inner_candidates++;
                continue;
            }

            graph_join_consider_nestloop_candidate_pair(
                outerrel, innerrel, outer_component->path,
                inner_rel_candidate->path, &outer_component->evidence,
                &inner_rel_candidate->evidence, &component_candidates);
        }
    }

    foreach(outer_lc, outer_metadata->candidates)
    {
        AgeGraphJoinRelCandidate *outer_rel_candidate = lfirst(outer_lc);
        ListCell *inner_lc;

        if (graph_join_rel_candidate_is_component_covered(
                outer_metadata, outer_rel_candidate))
        {
            skipped_outer_candidates++;
            continue;
        }

        foreach(inner_lc, inner_metadata->component_candidates)
        {
            AgeGraphJoinRelComponentCandidate *inner_component =
                lfirst(inner_lc);

            graph_join_consider_nestloop_candidate_pair(
                outerrel, innerrel, outer_rel_candidate->path,
                inner_component->path, &outer_rel_candidate->evidence,
                &inner_component->evidence, &component_candidates);
        }
    }

    foreach(candidate_lc, component_candidates)
    {
        GraphJoinNestLoopCandidate *candidate_slot = lfirst(candidate_lc);

        add_graph_join_candidate_nestloop_alternative(
            root, joinrel, outerrel, innerrel, jointype, extra,
            candidate_slot);
    }

    if (skipped_outer_candidates > 0 || skipped_inner_candidates > 0)
    {
        ereport(DEBUG2,
                (errmsg_internal("AGE graph join pair enumeration pruned: "
                                 "joinrelids=%s outer-skipped=%d "
                                 "inner-skipped=%d outer-components=%d "
                                 "inner-components=%d",
                                 bmsToString(joinrel->relids),
                                 skipped_outer_candidates,
                                 skipped_inner_candidates,
                                 list_length(
                                     outer_metadata->component_candidates),
                                 list_length(
                                     inner_metadata->component_candidates))));
    }
}

static bool graph_join_rel_candidate_is_component_covered(
    AgeGraphJoinRelMetadata *metadata,
    const AgeGraphJoinRelCandidate *candidate)
{
    ListCell *lc;

    if (metadata == NULL ||
        candidate == NULL ||
        candidate->evidence.order_property_kind == AGE_GRAPH_JOIN_ORDER_UNKNOWN)
    {
        return false;
    }

    foreach(lc, metadata->component_candidates)
    {
        AgeGraphJoinRelComponentCandidate *component_candidate = lfirst(lc);

        if (graph_join_component_candidate_covers_evidence(
                component_candidate, &candidate->evidence))
        {
            return true;
        }
    }

    return false;
}

static bool graph_join_component_candidate_covers_evidence(
    const AgeGraphJoinRelComponentCandidate *component_candidate,
    const AgeGraphJoinPathEvidence *evidence)
{
    const AgeGraphJoinPathEvidence *component_evidence;

    if (component_candidate == NULL ||
        component_candidate->path == NULL ||
        evidence == NULL ||
        evidence->order_property_kind == AGE_GRAPH_JOIN_ORDER_UNKNOWN)
    {
        return false;
    }

    component_evidence = &component_candidate->evidence;

    if (!graph_join_nestloop_component_matches(
            component_evidence->component_kind,
            evidence->component_kind))
    {
        return false;
    }
    if (!graph_join_nestloop_connector_matches(
            component_evidence->connector_kind,
            evidence->connector_kind))
        return false;
    if (!graph_join_nestloop_order_property_matches(
            component_evidence->order_property_kind,
            evidence->order_property_kind))
    {
        return false;
    }

    if (!graph_join_nestloop_source_evidence_matches(
            component_evidence->source_evidence_kind,
            evidence->source_evidence_kind))
        return false;

    return graph_join_nestloop_text_matches(component_evidence->pattern_key,
                                            evidence->pattern_key) &&
           graph_join_nestloop_source_kind_matches(
               component_evidence->source_kind_id,
               evidence->source_kind_id) &&
           bms_equal(component_evidence->solved_relids,
                     evidence->solved_relids) &&
           bms_equal(component_evidence->provided_relids,
                     evidence->provided_relids) &&
           bms_equal(component_evidence->required_outer,
                     evidence->required_outer) &&
           component_evidence->parallel_safe == evidence->parallel_safe &&
           component_evidence->parallel_aware == evidence->parallel_aware &&
           component_evidence->parallel_workers ==
           evidence->parallel_workers &&
           component_evidence->gather_cost == evidence->gather_cost &&
           component_evidence->output_width == evidence->output_width &&
           component_evidence->order_preserving ==
           evidence->order_preserving &&
           component_evidence->shared_state_required ==
           evidence->shared_state_required;
}

static int graph_join_nestloop_component_index(
    AgeGraphJoinComponentKind component_kind)
{
    switch (component_kind)
    {
        case AGE_GRAPH_JOIN_COMPONENT_NODE_PROPERTY_SEEK:
            return GRAPH_JOIN_NEST_NODE_PROPERTY;
        case AGE_GRAPH_JOIN_COMPONENT_VLE_EXPANSION:
            return GRAPH_JOIN_NEST_VLE;
        case AGE_GRAPH_JOIN_COMPONENT_ADJACENCY_EXPANSION:
            return GRAPH_JOIN_NEST_ADJACENCY;
        case AGE_GRAPH_JOIN_COMPONENT_VALUE_JOIN:
            return GRAPH_JOIN_NEST_VALUE_JOIN;
        case AGE_GRAPH_JOIN_COMPONENT_APPLY:
            return GRAPH_JOIN_NEST_APPLY;
        case AGE_GRAPH_JOIN_COMPONENT_CARTESIAN:
            return GRAPH_JOIN_NEST_CARTESIAN;
        case AGE_GRAPH_JOIN_COMPONENT_UNKNOWN:
            break;
    }

    return -1;
}

static void graph_join_consider_nestloop_candidate_pair(
    RelOptInfo *outerrel, RelOptInfo *innerrel,
    Path *outer_path, Path *inner_path,
    const AgeGraphJoinPathEvidence *outer_evidence,
    const AgeGraphJoinPathEvidence *inner_evidence,
    List **component_candidates)
{
    AgeGraphJoinCandidateTable *candidate_table;
    AgeGraphJoinCandidate *candidate;
    int component_index;

    if (outer_evidence == NULL ||
        inner_evidence == NULL ||
        component_candidates == NULL)
    {
        return;
    }

    if (!path_contains_graph_expansion(outer_path) &&
        !path_contains_graph_expansion(inner_path))
    {
        return;
    }

    if (inner_evidence->shared_state_required &&
        outer_path != NULL &&
        outer_path->rows > 1.0)
    {
        return;
    }

    if (!graph_join_candidate_nestloop_pair_is_legal(
            outerrel, innerrel, outer_path, inner_path))
    {
        return;
    }
    if (!graph_join_candidate_evidence_fits_pair(
            outer_path, inner_path, outer_evidence) ||
        !graph_join_candidate_evidence_fits_pair(
            outer_path, inner_path, inner_evidence))
    {
        return;
    }

    candidate_table = make_graph_expansion_join_candidate_table(
        outer_path, inner_path, outer_evidence, inner_evidence);
    candidate = age_graph_join_table_select_cheapest(candidate_table);
    if (candidate == NULL)
        return;

    component_index = graph_join_nestloop_component_index(
        candidate->component.family_kind);
    if (component_index < 0)
        return;

    graph_join_nestloop_candidate_update(
        component_candidates, component_index, outer_path, inner_path,
        outer_evidence, inner_evidence, candidate_table, candidate);
}

static void graph_join_nestloop_candidate_update(
    List **component_candidates, int component_index,
    Path *outer_path, Path *inner_path,
    const AgeGraphJoinPathEvidence *outer_evidence,
    const AgeGraphJoinPathEvidence *inner_evidence,
    AgeGraphJoinCandidateTable *table, AgeGraphJoinCandidate *candidate)
{
    GraphJoinNestLoopCandidate *slot;
    Cost total_cost;

    if (component_candidates == NULL ||
        candidate == NULL)
    {
        return;
    }

    slot = graph_join_nestloop_candidate_slot(component_candidates,
                                             component_index, candidate);
    if (slot == NULL)
        return;

    total_cost = graph_join_nestloop_candidate_total_cost(outer_path,
                                                          inner_path,
                                                          candidate);
    if (slot->candidate != NULL &&
        slot->total_cost <= total_cost)
    {
        return;
    }

    slot->outer_path = outer_path;
    slot->inner_path = inner_path;
    if (outer_evidence != NULL)
        slot->outer_evidence = *outer_evidence;
    if (inner_evidence != NULL)
        slot->inner_evidence = *inner_evidence;
    slot->table = table;
    slot->candidate = candidate;
    slot->total_cost = total_cost;
}

static Cost graph_join_nestloop_candidate_total_cost(
    Path *outer_path, Path *inner_path, AgeGraphJoinCandidate *candidate)
{
    Cost total_cost;

    if (candidate == NULL)
        return 0;

    total_cost = candidate->connector.total_cost;
    if (outer_path != NULL)
        total_cost += outer_path->total_cost;
    if (inner_path != NULL)
        total_cost += inner_path->total_cost;

    return total_cost;
}

static GraphJoinNestLoopCandidate *graph_join_nestloop_candidate_slot(
    List **component_candidates, int component_index,
    AgeGraphJoinCandidate *candidate)
{
    ListCell *lc;
    GraphJoinNestLoopCandidate *slot;

    if (component_candidates == NULL ||
        component_index < 0 ||
        candidate == NULL)
    {
        return NULL;
    }

    foreach(lc, *component_candidates)
    {
        slot = lfirst(lc);

        if (graph_join_nestloop_candidate_slot_matches(
                slot, component_index, candidate))
        {
            return slot;
        }
    }

    slot = palloc0(sizeof(*slot));
    slot->component_index = component_index;
    *component_candidates = lappend(*component_candidates, slot);

    return slot;
}

static bool graph_join_nestloop_candidate_slot_matches(
    GraphJoinNestLoopCandidate *slot, int component_index,
    AgeGraphJoinCandidate *candidate)
{
    AgeGraphJoinCandidate *existing;

    if (slot == NULL || candidate == NULL ||
        slot->component_index != component_index)
    {
        return false;
    }
    if (slot->candidate == NULL)
        return true;

    existing = slot->candidate;

    return graph_join_nestloop_component_identity_matches(
               &existing->component, &candidate->component) &&
           graph_join_nestloop_connector_identity_matches(
               &existing->connector, &candidate->connector) &&
           graph_join_nestloop_text_matches(existing->pattern_key,
                                            candidate->pattern_key) &&
           graph_join_nestloop_source_kind_matches(
               existing->source_kind_id, candidate->source_kind_id);
}

static bool graph_join_nestloop_component_identity_matches(
    const AgeGraphJoinComponent *existing,
    const AgeGraphJoinComponent *current)
{
    if (existing == NULL || current == NULL)
        return false;

    if (!bms_equal(existing->solved_relids, current->solved_relids) ||
        !bms_equal(existing->provided_relids, current->provided_relids) ||
        !bms_equal(existing->required_outer, current->required_outer))
    {
        return false;
    }

    return existing->parallel_safe == current->parallel_safe &&
           existing->parallel_aware == current->parallel_aware &&
           existing->parallel_workers == current->parallel_workers &&
           existing->gather_cost == current->gather_cost &&
           existing->output_width == current->output_width &&
           existing->order_preserving == current->order_preserving &&
           existing->shared_state_required == current->shared_state_required;
}

static bool graph_join_nestloop_connector_identity_matches(
    const AgeGraphJoinConnector *existing,
    const AgeGraphJoinConnector *current)
{
    if (existing == NULL || current == NULL)
        return false;

    if (!graph_join_nestloop_connector_matches(existing->kind_id,
                                               current->kind_id))
        return false;
    if (!graph_join_nestloop_source_evidence_matches(
            existing->source_evidence_kind, current->source_evidence_kind))
        return false;

    return
           existing->bound_kind == current->bound_kind &&
           graph_join_nestloop_order_property_matches(
               existing->order_property_kind,
               current->order_property_kind) &&
           bms_equal(existing->solved_relids, current->solved_relids) &&
           bms_equal(existing->provided_relids, current->provided_relids) &&
           bms_equal(existing->required_outer, current->required_outer);
}

static bool graph_join_nestloop_text_matches(const char *existing,
                                             const char *current)
{
    if (existing == NULL || current == NULL)
        return existing == current;

    return strcmp(existing, current) == 0;
}

static bool graph_join_nestloop_component_matches(
    AgeGraphJoinComponentKind existing_kind,
    AgeGraphJoinComponentKind current_kind)
{
    return existing_kind != AGE_GRAPH_JOIN_COMPONENT_UNKNOWN &&
        current_kind != AGE_GRAPH_JOIN_COMPONENT_UNKNOWN &&
        existing_kind == current_kind;
}

static bool graph_join_nestloop_order_property_matches(
    AgeGraphJoinOrderPropertyKind existing_kind,
    AgeGraphJoinOrderPropertyKind current_kind)
{
    return existing_kind != AGE_GRAPH_JOIN_ORDER_UNKNOWN &&
        current_kind != AGE_GRAPH_JOIN_ORDER_UNKNOWN &&
        existing_kind == current_kind;
}

static bool graph_join_nestloop_connector_matches(
    AgeGraphJoinConnectorKind existing_kind,
    AgeGraphJoinConnectorKind current_kind)
{
    return existing_kind != AGE_GRAPH_JOIN_CONNECTOR_UNKNOWN &&
        current_kind != AGE_GRAPH_JOIN_CONNECTOR_UNKNOWN &&
        existing_kind == current_kind;
}

static bool graph_join_nestloop_source_kind_matches(
    AgeGraphJoinSourceKind existing_kind,
    AgeGraphJoinSourceKind current_kind)
{
    return existing_kind != AGE_GRAPH_JOIN_SOURCE_UNKNOWN &&
        current_kind != AGE_GRAPH_JOIN_SOURCE_UNKNOWN &&
        existing_kind == current_kind;
}

static bool graph_join_nestloop_source_evidence_matches(
    AgeGraphJoinSourceEvidenceKind existing_kind,
    AgeGraphJoinSourceEvidenceKind current_kind)
{
    if (existing_kind == AGE_GRAPH_JOIN_SOURCE_EVIDENCE_UNKNOWN ||
        current_kind == AGE_GRAPH_JOIN_SOURCE_EVIDENCE_UNKNOWN)
        return false;

    return existing_kind == current_kind;
}

static Path *graph_join_gather_partial_path(PlannerInfo *root,
                                            RelOptInfo *rel, Path *path)
{
    if (root == NULL ||
        rel == NULL ||
        path == NULL ||
        !path->parallel_aware)
    {
        return path;
    }

    return (Path *)create_gather_path(root, rel, path, rel->reltarget,
                                      PATH_REQ_OUTER(path), NULL);
}

static void add_graph_join_candidate_nestloop_alternative(
    PlannerInfo *root, RelOptInfo *joinrel, RelOptInfo *outerrel,
    RelOptInfo *innerrel, JoinType jointype, JoinPathExtraData *extra,
    GraphJoinNestLoopCandidate *candidate_slot)
{
    JoinCostWorkspace workspace;
    Relids required_outer;
    Path *outer_path;
    Path *inner_path;
    NestPath *nest_path;
    double adjusted_rows;
    double row_ratio;
    double candidate_credit;
    Cost adjusted_total_cost;
    AgeGraphJoinCandidate *next_best;
    AgeGraphJoinPathEvidence join_evidence;

    if (candidate_slot == NULL ||
        candidate_slot->candidate == NULL ||
        candidate_slot->outer_path == NULL ||
        candidate_slot->inner_path == NULL)
    {
        return;
    }

    outer_path = candidate_slot->outer_path;
    inner_path = candidate_slot->inner_path;
    outer_path = graph_join_gather_partial_path(root, outerrel, outer_path);
    inner_path = graph_join_gather_partial_path(root, innerrel, inner_path);
    if (path_contains_graph_expansion(outer_path) &&
        !path_contains_graph_expansion(inner_path) &&
        !IsA(inner_path, MaterialPath) &&
        outer_path->rows > 1.0)
    {
        inner_path = (Path *)create_material_path(innerrel, inner_path);
    }

    required_outer = calc_nestloop_required_outer(
        outerrel->relids, PATH_REQ_OUTER(outer_path),
        innerrel->relids, PATH_REQ_OUTER(inner_path));
    initial_cost_nestloop(root, &workspace, jointype,
                          outer_path, inner_path, extra);
    nest_path = create_nestloop_path(root, joinrel, jointype, &workspace,
                                     extra, outer_path, inner_path,
                                     extra->restrictlist, NIL,
                                     required_outer);

    adjusted_rows = clamp_row_est(Max(
        candidate_slot->candidate->connector.rows, 1.0));
    adjusted_rows = Min(nest_path->jpath.path.rows, adjusted_rows);
    row_ratio = adjusted_rows / Max(nest_path->jpath.path.rows, 1.0);
    row_ratio = Max(row_ratio, 0.01);
    candidate_credit = age_graph_join_path_evidence_credit(
        &candidate_slot->outer_evidence, &candidate_slot->inner_evidence);
    adjusted_total_cost = nest_path->jpath.path.startup_cost +
        (nest_path->jpath.path.total_cost -
         nest_path->jpath.path.startup_cost) * row_ratio * candidate_credit;
    if (adjusted_total_cost < nest_path->jpath.path.startup_cost)
        adjusted_total_cost = nest_path->jpath.path.startup_cost;

    nest_path->jpath.path.rows = adjusted_rows;
    nest_path->jpath.path.total_cost = adjusted_total_cost;
    age_graph_join_init_path_evidence(&join_evidence);
    join_evidence.component_kind =
        candidate_slot->candidate->component.family_kind;
    join_evidence.connector_kind = candidate_slot->candidate->connector.kind_id;
    join_evidence.order_property_kind =
        candidate_slot->candidate->connector.order_property_kind;
    join_evidence.source_evidence_kind =
        candidate_slot->candidate->connector.source_evidence_kind;
    join_evidence.pattern_key = candidate_slot->candidate->pattern_key;
    join_evidence.source_kind_id = candidate_slot->candidate->source_kind_id;
    join_evidence.solved_relids = bms_copy(
        candidate_slot->candidate->connector.solved_relids);
    join_evidence.required_outer = required_outer;
    join_evidence.provided_relids = joinrel->relids;
    join_evidence.candidate_count =
        age_graph_join_table_candidate_count(candidate_slot->table);
    join_evidence.declared_cover_count =
        candidate_slot->table->declared_entry_count;
    join_evidence.cover_match_kind =
        candidate_slot->table->cover_match_kind;
    join_evidence.selected_total_cost = nest_path->jpath.path.total_cost;
    next_best = age_graph_join_table_select_next_best(
        candidate_slot->table, candidate_slot->candidate);
    join_evidence.next_total_cost = next_best != NULL ?
        next_best->connector.total_cost : 0;
    if (joinrel->reltarget != NULL)
        join_evidence.output_width = joinrel->reltarget->width;
    join_evidence.parallel_safe =
        nest_path->jpath.path.parallel_safe &&
        candidate_slot->candidate->component.parallel_safe;
    join_evidence.parallel_aware =
        join_evidence.parallel_safe &&
        candidate_slot->candidate->component.parallel_aware;
    join_evidence.parallel_workers = join_evidence.parallel_safe ?
        candidate_slot->candidate->component.parallel_workers : 0;
    join_evidence.gather_cost = join_evidence.parallel_safe ?
        candidate_slot->candidate->component.gather_cost : 0;
    join_evidence.order_preserving = nest_path->jpath.path.pathkeys != NIL;
    join_evidence.shared_state_required =
        candidate_slot->candidate->component.shared_state_required;
    join_evidence.physical_properties_known = true;
    join_evidence.bound = age_graph_join_order_property_kind_is_bound(
        join_evidence.order_property_kind);
    age_graph_join_complete_path_evidence((Path *)nest_path, &join_evidence);
    age_graph_join_register_rel_path_evidence(
        root, joinrel, (Path *)nest_path, &join_evidence);
    register_graph_join_joinrel_lowering_evidence(
        root, joinrel, (Path *)nest_path, &join_evidence);

    ereport(DEBUG2,
            (errmsg_internal("AGE graph join candidate nestloop added: "
                             "jointype=%d joinrelids=%s component=%s "
                             "family=%s connector=%s order=%s source=%s "
                             "pattern=%s source-kind=%s rows=%.0f "
                             "cost=%.2f candidates=%d solved=%s "
                             "required=%s provided=%s",
                             (int)jointype,
                             bmsToString(joinrel->relids),
                             candidate_slot->candidate->component.display_name,
                             age_graph_join_component_name(
                                 candidate_slot->candidate->component.
                                 family_kind),
                             age_graph_join_connector_name(
                                 candidate_slot->candidate->connector.kind_id),
                             age_graph_join_order_property_name(
                                 candidate_slot->candidate->connector.
                                 order_property_kind),
                             age_graph_join_source_evidence_kind_name(
                                 candidate_slot->candidate->connector.
                                 source_evidence_kind),
                             candidate_slot->candidate->pattern_key != NULL ?
                             candidate_slot->candidate->pattern_key :
                             "<none>",
                             age_graph_join_source_kind_name(
                                 candidate_slot->candidate->source_kind_id),
                             nest_path->jpath.path.rows,
                             nest_path->jpath.path.total_cost,
                             age_graph_join_table_candidate_count(
                                 candidate_slot->table),
                             bmsToString(
                                 candidate_slot->candidate->
                                 component.solved_relids),
                             bmsToString(
                                 candidate_slot->candidate->
                                 component.required_outer),
                             bmsToString(
                                 candidate_slot->candidate->
                                 component.provided_relids))));

    add_path(joinrel, (Path *)nest_path);
}

static bool graph_join_candidate_nestloop_pair_is_legal(
    RelOptInfo *outerrel, RelOptInfo *innerrel, Path *outer_path,
    Path *inner_path)
{
    Relids outer_paramrels;
    Relids innerrelids;

    (void)inner_path;

    if (outerrel == NULL ||
        innerrel == NULL ||
        outer_path == NULL)
    {
        return false;
    }

    outer_paramrels = PATH_REQ_OUTER(outer_path);
    innerrelids = innerrel->top_parent_relids != NULL ?
        innerrel->top_parent_relids : innerrel->relids;

    return !bms_overlap(outer_paramrels, innerrelids);
}

static bool graph_join_candidate_evidence_fits_pair(
    Path *outer_path, Path *inner_path,
    const AgeGraphJoinPathEvidence *evidence)
{
    Relids provided_relids;
    Relids required_outer;
    Relids allowed_required;
    bool fits;
    const char *failure_reason = NULL;

    if (evidence == NULL ||
        evidence->order_property_kind == AGE_GRAPH_JOIN_ORDER_UNKNOWN)
    {
        return false;
    }

    if (evidence->declared_cover_count > 0 &&
        evidence->cover_match_kind != AGE_GRAPH_JOIN_COVER_MATCHED)
    {
        ereport(DEBUG2,
                (errmsg_internal("AGE graph join evidence rejected: "
                                 "reason=%s component-family=%s "
                                 "connector=%s pattern=%s source-kind=%s "
                                 "declared-cover=%ld",
                                 age_graph_join_cover_match_kind_name(
                                     evidence->cover_match_kind),
                                 age_graph_join_component_name(
                                     evidence->component_kind),
                                 evidence->connector_kind !=
                                 AGE_GRAPH_JOIN_CONNECTOR_UNKNOWN ?
                                 age_graph_join_connector_name(
                                     evidence->connector_kind) : "<none>",
                                 evidence->pattern_key != NULL ?
                                 evidence->pattern_key : "<none>",
                                 age_graph_join_source_kind_name(
                                     evidence->source_kind_id),
                                 (long)evidence->declared_cover_count)));
        return false;
    }

    provided_relids = graph_join_joinrelids_from_paths(outer_path,
                                                       inner_path);
    required_outer = graph_join_required_outer_from_paths(
        outer_path, inner_path, provided_relids);
    allowed_required = bms_union(provided_relids, required_outer);
    fits = true;
    if (evidence->provided_relids != NULL &&
        provided_relids != NULL &&
        !bms_is_subset(evidence->provided_relids, provided_relids))
    {
        fits = false;
        failure_reason = "provided-outside-pair";
    }
    if (evidence->solved_relids != NULL &&
        provided_relids != NULL &&
        !bms_is_subset(evidence->solved_relids, provided_relids))
    {
        fits = false;
        if (failure_reason == NULL)
            failure_reason = "solved-outside-pair";
    }
    if (evidence->required_outer != NULL &&
        !bms_is_subset(evidence->required_outer, allowed_required))
    {
        fits = false;
        if (failure_reason == NULL)
            failure_reason = "required-outside-pair";
    }

    if (!fits)
    {
        ereport(DEBUG2,
                (errmsg_internal("AGE graph join evidence rejected: "
                                 "reason=%s component-family=%s "
                                 "connector=%s pattern=%s source-kind=%s "
                                 "provided=%s solved=%s required=%s "
                                 "pair-provided=%s pair-required=%s",
                                 failure_reason != NULL ? failure_reason :
                                 "unknown",
                                 age_graph_join_component_name(
                                     evidence->component_kind),
                                 evidence->connector_kind !=
                                 AGE_GRAPH_JOIN_CONNECTOR_UNKNOWN ?
                                 age_graph_join_connector_name(
                                     evidence->connector_kind) : "<none>",
                                 evidence->pattern_key != NULL ?
                                 evidence->pattern_key : "<none>",
                                 age_graph_join_source_kind_name(
                                     evidence->source_kind_id),
                                 bmsToString(evidence->provided_relids),
                                 bmsToString(evidence->solved_relids),
                                 bmsToString(evidence->required_outer),
                                 bmsToString(provided_relids),
                                 bmsToString(allowed_required))));
    }

    bms_free(provided_relids);
    bms_free(required_outer);
    bms_free(allowed_required);

    return fits;
}

static void add_deferred_ordered_property_projection_path(
    PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel,
    FinalPathExtraData *extra)
{
    PathTarget *lower_target;
    PathTarget *final_target;

    if (!cypher_build_ordered_property_projection_targets(
            root, input_rel, output_rel, extra, &lower_target, &final_target))
    {
        return;
    }

    add_deferred_projection_paths(root, output_rel, lower_target, final_target,
                                  extra, 1.0,
                                  "ordered property projection",
                                  true);
}

static void add_deferred_projection_paths(PlannerInfo *root,
                                          RelOptInfo *output_rel,
                                          PathTarget *lower_target,
                                          PathTarget *final_target,
                                          FinalPathExtraData *extra,
                                          Cost cost_discount,
                                          const char *debug_label,
                                          bool prune_join_child_targets)
{
    List *new_paths = NIL;
    ListCell *lc;

    foreach(lc, output_rel->pathlist)
    {
        Path *path = lfirst(lc);
        LimitPath *limit_path;
        Path *deferred_path;
        ProjectionPath *project_path;
        Cost original_startup_cost;
        Cost original_total_cost;
        int disabled_nodes;

        if (!IsA(path, LimitPath))
            continue;

        original_startup_cost = path->startup_cost;
        original_total_cost = path->total_cost;
        disabled_nodes = path->disabled_nodes;

        limit_path = palloc(sizeof(*limit_path));
        memcpy(limit_path, path, sizeof(*limit_path));
        limit_path->path.pathtype = T_Limit;
        limit_path->subpath = copy_path_with_deferred_projection_target(
            root, limit_path->subpath, lower_target,
            prune_join_child_targets);
        if (limit_path->subpath == NULL)
            continue;

        limit_path->path.pathtarget = limit_path->subpath->pathtarget;
        limit_path->path.pathkeys = limit_path->subpath->pathkeys;

        project_path = create_projection_path(root, output_rel,
                                              (Path *)limit_path,
                                              final_target);
        deferred_path = (Path *)project_path;
        deferred_path->rows = extra->count_est > 0 ? extra->count_est :
            path->rows;
        deferred_path->disabled_nodes = disabled_nodes;
        deferred_path->startup_cost = original_startup_cost;
        deferred_path->total_cost = Max(original_startup_cost,
                                        original_total_cost - cost_discount);

        ereport(DEBUG2,
                (errmsg_internal("AGE deferred %s path added: "
                                 "original_cost=%.2f new_cost=%.2f "
                                 "rows=%.0f",
                                 debug_label,
                                 original_total_cost,
                                 deferred_path->total_cost,
                                 deferred_path->rows)));

        new_paths = lappend(new_paths, deferred_path);
    }

    foreach(lc, new_paths)
        add_path(output_rel, lfirst(lc));
}

static Path *copy_path_with_deferred_projection_target(PlannerInfo *root,
                                                       Path *path,
                                                       PathTarget *target,
                                                       bool prune_join_child_targets)
{
    if (path == NULL)
        return NULL;

    if (path->pathtarget != NULL &&
        target != NULL &&
        equal(path->pathtarget->exprs, target->exprs))
    {
        return path;
    }

    if (IsA(path, LimitPath))
    {
        LimitPath *limit_path;

        limit_path = palloc(sizeof(*limit_path));
        memcpy(limit_path, path, sizeof(*limit_path));

        limit_path->subpath = copy_path_with_deferred_projection_target(
            root, limit_path->subpath, target, prune_join_child_targets);
        if (limit_path->subpath == NULL)
            return NULL;
        limit_path->path.pathtype = T_Limit;
        limit_path->path.pathtarget = limit_path->subpath->pathtarget;
        limit_path->path.pathkeys = limit_path->subpath->pathkeys;
        return (Path *)limit_path;
    }
    if (IsA(path, GatherMergePath))
    {
        GatherMergePath *gm_path;

        gm_path = palloc(sizeof(*gm_path));
        memcpy(gm_path, path, sizeof(*gm_path));

        gm_path->subpath = copy_path_with_deferred_projection_target(
            root, gm_path->subpath, target, prune_join_child_targets);
        if (gm_path->subpath == NULL)
            return NULL;
        gm_path->path.pathtype = T_GatherMerge;
        gm_path->path.pathtarget = gm_path->subpath->pathtarget;
        gm_path->path.pathkeys = path->pathkeys;
        return (Path *)gm_path;
    }
    if (IsA(path, UpperUniquePath))
    {
        UpperUniquePath *unique_path;

        unique_path = palloc(sizeof(*unique_path));
        memcpy(unique_path, path, sizeof(*unique_path));

        unique_path->subpath = copy_path_with_deferred_projection_target(
            root, unique_path->subpath, target, prune_join_child_targets);
        if (unique_path->subpath == NULL)
            return NULL;
        unique_path->path.pathtype = T_Unique;
        unique_path->path.pathtarget = unique_path->subpath->pathtarget;
        unique_path->path.pathkeys = unique_path->subpath->pathkeys;
        return (Path *)unique_path;
    }
    if (IsA(path, SortPath))
    {
        SortPath *sort_path;

        sort_path = palloc(sizeof(*sort_path));
        memcpy(sort_path, path, sizeof(*sort_path));

        sort_path->subpath = copy_path_with_deferred_projection_target(
            root, sort_path->subpath, target, prune_join_child_targets);
        if (sort_path->subpath == NULL)
            return NULL;
        sort_path->path.pathtype = T_Sort;
        sort_path->path.pathtarget = sort_path->subpath->pathtarget;
        sort_path->path.pathkeys = path->pathkeys;
        return (Path *)sort_path;
    }
    if (IsA(path, MaterialPath))
    {
        MaterialPath *material_path;

        material_path = palloc(sizeof(*material_path));
        memcpy(material_path, path, sizeof(*material_path));

        material_path->subpath = copy_path_with_deferred_projection_target(
            root, material_path->subpath, target, prune_join_child_targets);
        if (material_path->subpath == NULL)
            return NULL;
        material_path->path.pathtype = T_Material;
        material_path->path.pathtarget = material_path->subpath->pathtarget;
        material_path->path.pathkeys = material_path->subpath->pathkeys;
        return (Path *)material_path;
    }
    if (IsA(path, ProjectionPath))
    {
        ProjectionPath *projection_path = castNode(ProjectionPath, path);

        return copy_path_with_deferred_projection_target(
            root, projection_path->subpath, target, prune_join_child_targets);
    }
    if (IsA(path, NestPath) || IsA(path, MergePath) || IsA(path, HashPath))
        return copy_join_path_with_deferred_projection_target(root, path,
                                                              target,
                                                              prune_join_child_targets);

    if (!is_projection_capable_path(path))
        return NULL;

    return copy_projection_capable_path_with_target(root, path, target);
}

static Path *copy_join_path_with_deferred_projection_target(PlannerInfo *root,
                                                            Path *path,
                                                            PathTarget *target,
                                                            bool prune_join_child_targets)
{
    JoinPath *join_path;
    Path *outer_path;
    Path *inner_path;
    PathTarget *outer_target;
    PathTarget *inner_target;

    join_path = (JoinPath *)copy_path_node_shallow(path);
    if (join_path == NULL)
        return NULL;

    outer_path = join_path->outerjoinpath;
    inner_path = join_path->innerjoinpath;
    outer_target = build_child_deferred_projection_target(
        root, outer_path, target, join_path_clause_sources(path),
        target_contains_edge_ctid(root, target),
        prune_join_child_targets);
    inner_target = build_child_deferred_projection_target(
        root, inner_path, target, join_path_clause_sources(path),
        target_contains_edge_ctid(root, target),
        prune_join_child_targets);

    join_path->outerjoinpath = copy_path_with_deferred_projection_target(
        root, outer_path, outer_target, prune_join_child_targets);
    join_path->innerjoinpath = copy_path_with_deferred_projection_target(
        root, inner_path, inner_target, prune_join_child_targets);

    if (join_path->outerjoinpath == NULL || join_path->innerjoinpath == NULL)
        return NULL;

    join_path->path.pathtarget = target;
    return (Path *)join_path;
}

static PathTarget *build_child_deferred_projection_target(PlannerInfo *root,
                                                          Path *child_path,
                                                          PathTarget *target,
                                                          List *joinrestrictinfo,
                                                          bool preserve_label_core,
                                                          bool prune_child_target)
{
    PathTarget *child_target;
    ListCell *lc;

    child_target = prune_child_target ? create_empty_pathtarget() :
        copy_pathtarget(child_path->pathtarget);
    foreach(lc, target->exprs)
    {
        Node *expr = lfirst(lc);
        bool child_owned;

        child_owned = expression_belongs_to_child_relids(
            expr, child_path->parent->relids);
        if (child_owned &&
            !pathtarget_contains_expr(child_target, expr))
        {
            add_column_to_pathtarget(child_target, (Expr *)copyObject(expr),
                                     0);
        }
        else if (!child_owned)
        {
            add_child_rel_vars_to_pathtarget(child_target, child_path, expr);
        }
    }

    if (!prune_child_target)
    {
        add_child_rel_vars_to_pathtarget(child_target, child_path,
                                         (Node *)target->exprs);
    }
    add_child_join_clause_vars_to_pathtarget(child_target, child_path,
                                             joinrestrictinfo);
    if (child_path->param_info != NULL)
    {
        add_child_join_clause_vars_to_pathtarget(
            child_target, child_path, child_path->param_info->ppi_clauses);
    }
    if (preserve_label_core)
        add_child_label_core_vars_to_pathtarget(root, child_target, child_path);

    child_target = set_pathtarget_cost_width(root, child_target);

    return child_target;
}

static void add_child_rel_vars_to_pathtarget(PathTarget *child_target,
                                             Path *child_path,
                                             Node *node)
{
    List *vars;
    ListCell *lc;

    vars = pull_var_clause(node,
                           PVC_RECURSE_AGGREGATES |
                           PVC_RECURSE_WINDOWFUNCS |
                           PVC_RECURSE_PLACEHOLDERS);

    foreach(lc, vars)
    {
        Var *var = lfirst_node(Var, lc);

        if (var->varno <= 0 ||
            !bms_is_member(var->varno, child_path->parent->relids) ||
            pathtarget_contains_expr(child_target, (Node *)var))
        {
            continue;
        }

        add_column_to_pathtarget(child_target, (Expr *)copyObject(var), 0);
    }

    list_free(vars);
}

static void add_child_join_clause_vars_to_pathtarget(PathTarget *child_target,
                                                     Path *child_path,
                                                     List *joinrestrictinfo)
{
    ListCell *lc;

    foreach(lc, joinrestrictinfo)
    {
        RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc);

        add_child_rel_vars_to_pathtarget(child_target, child_path,
                                         (Node *)rinfo->clause);
    }
}

static void add_child_label_core_vars_to_pathtarget(PlannerInfo *root,
                                                    PathTarget *child_target,
                                                    Path *child_path)
{
    int varno;

    for (varno = 1; varno < root->simple_rel_array_size; varno++)
    {
        RelOptInfo *rel = root->simple_rel_array[varno];

        if (rel == NULL ||
            !bms_is_member(varno, child_path->parent->relids))
        {
            continue;
        }

        if (rel->max_attr < Anum_ag_label_edge_table_properties)
            continue;

        add_child_label_var_to_pathtarget(root, child_target, varno,
                                          SelfItemPointerAttributeNumber,
                                          TIDOID);
        add_child_label_var_to_pathtarget(root, child_target, varno,
                                          Anum_ag_label_edge_table_id,
                                          GRAPHIDOID);
        add_child_label_var_to_pathtarget(root, child_target, varno,
                                          Anum_ag_label_edge_table_start_id,
                                          GRAPHIDOID);
        add_child_label_var_to_pathtarget(root, child_target, varno,
                                          Anum_ag_label_edge_table_end_id,
                                          GRAPHIDOID);
        add_child_label_var_to_pathtarget(root, child_target, varno,
                                          Anum_ag_label_edge_table_properties,
                                          AGTYPEOID);
    }
}

static void add_child_label_var_to_pathtarget(PlannerInfo *root,
                                              PathTarget *child_target,
                                              Index varno,
                                              AttrNumber attno,
                                              Oid vartype)
{
    RelOptInfo *rel;
    Var *var;

    rel = root->simple_rel_array[varno];
    if (rel == NULL)
        return;

    if (attno > 0 && attno > rel->max_attr)
        return;

    var = makeVar(varno, attno, vartype, -1, InvalidOid, 0);
    if (pathtarget_contains_expr(child_target, (Node *)var))
        return;

    add_column_to_pathtarget(child_target, (Expr *)var, 0);
}

static bool target_contains_edge_ctid(PlannerInfo *root, PathTarget *target)
{
    List *vars;
    ListCell *lc;
    bool result = false;

    vars = pull_var_clause((Node *)target->exprs,
                           PVC_RECURSE_AGGREGATES |
                           PVC_RECURSE_WINDOWFUNCS |
                           PVC_RECURSE_PLACEHOLDERS);

    foreach(lc, vars)
    {
        Var *var = lfirst_node(Var, lc);

        if (var->varattno == SelfItemPointerAttributeNumber &&
            var->varno > 0 &&
            var->varno < root->simple_rel_array_size &&
            root->simple_rel_array[var->varno] != NULL &&
            root->simple_rel_array[var->varno]->max_attr >=
            Anum_ag_label_edge_table_properties)
        {
            result = true;
            break;
        }
    }

    list_free(vars);

    return result;
}

static List *join_path_clause_sources(Path *path)
{
    JoinPath *join_path = (JoinPath *)path;
    List *clauses;

    clauses = list_copy(join_path->joinrestrictinfo);
    if (IsA(path, MergePath))
    {
        MergePath *merge_path = (MergePath *)path;

        clauses = list_concat(clauses, list_copy(merge_path->path_mergeclauses));
    }
    else if (IsA(path, HashPath))
    {
        HashPath *hash_path = (HashPath *)path;

        clauses = list_concat(clauses, list_copy(hash_path->path_hashclauses));
    }

    return clauses;
}

static bool expression_belongs_to_child_relids(Node *expr, Relids relids)
{
    List *vars;
    ListCell *lc;
    bool result = true;

    if (expr == NULL)
        return false;

    vars = pull_var_clause(expr,
                           PVC_RECURSE_AGGREGATES |
                           PVC_RECURSE_WINDOWFUNCS |
                           PVC_RECURSE_PLACEHOLDERS);
    if (vars == NIL)
        return false;

    foreach(lc, vars)
    {
        Var *var = lfirst_node(Var, lc);

        if (var->varno <= 0 || !bms_is_member(var->varno, relids))
        {
            result = false;
            break;
        }
    }

    list_free(vars);

    return result;
}

static bool pathtarget_contains_expr(PathTarget *target, Node *expr)
{
    ListCell *lc;

    if (target == NULL || expr == NULL)
        return false;

    foreach(lc, target->exprs)
    {
        if (equal(lfirst(lc), expr))
            return true;
    }

    return false;
}

static Path *copy_projection_capable_path_with_target(PlannerInfo *root,
                                                      Path *path,
                                                      PathTarget *target)
{
    Path *copy;
    QualCost oldcost;

    Assert(is_projection_capable_path(path));

    copy = copy_path_node_shallow(path);
    if (copy == NULL)
        return NULL;

    oldcost = copy->pathtarget->cost;
    copy->pathtarget = target;
    copy->startup_cost += target->cost.startup - oldcost.startup;
    copy->total_cost += target->cost.startup - oldcost.startup +
        (target->cost.per_tuple - oldcost.per_tuple) * copy->rows;

    return copy;
}

static Path *copy_path_node_shallow(Path *path)
{
    Path *copy;
    Size size;

    if (path == NULL)
        return NULL;

    switch (nodeTag(path))
    {
        case T_Path:
            size = sizeof(Path);
            break;
        case T_IndexPath:
            size = sizeof(IndexPath);
            break;
        case T_BitmapHeapPath:
            size = sizeof(BitmapHeapPath);
            break;
        case T_TidPath:
            size = sizeof(TidPath);
            break;
        case T_TidRangePath:
            size = sizeof(TidRangePath);
            break;
        case T_SubqueryScanPath:
            size = sizeof(SubqueryScanPath);
            break;
        case T_ForeignPath:
            size = sizeof(ForeignPath);
            break;
        case T_CustomPath:
            size = sizeof(CustomPath);
            break;
        case T_GatherPath:
            size = sizeof(GatherPath);
            break;
        case T_NestPath:
            size = sizeof(NestPath);
            break;
        case T_MergePath:
            size = sizeof(MergePath);
            break;
        case T_HashPath:
            size = sizeof(HashPath);
            break;
        case T_GroupPath:
            size = sizeof(GroupPath);
            break;
        case T_AggPath:
            size = sizeof(AggPath);
            break;
        case T_WindowAggPath:
            size = sizeof(WindowAggPath);
            break;
        default:
            return NULL;
    }

    copy = palloc(size);
    memcpy(copy, path, size);

    return copy;
}

static void add_deferred_count_agtype_projection_path(
    PlannerInfo *root, RelOptInfo *output_rel, FinalPathExtraData *extra)
{
    PathTarget *lower_target;
    PathTarget *final_target;

    if (root == NULL || output_rel == NULL ||
        !cypher_build_scalar_final_deferred_targets(
            root, root->processed_tlist, &lower_target, &final_target))
    {
        return;
    }

    if (extra != NULL && extra->limit_needed)
    {
        add_deferred_projection_paths(root, output_rel, lower_target,
                                      final_target, extra, 0.5,
                                      "count agtype projection",
                                      false);
    }

    add_deferred_count_agtype_plain_paths(root, output_rel, lower_target,
                                          final_target);
}

static void add_deferred_count_agtype_plain_paths(PlannerInfo *root,
                                                  RelOptInfo *output_rel,
                                                  PathTarget *lower_target,
                                                  PathTarget *final_target)
{
    List *new_paths = NIL;
    ListCell *lc;

    foreach(lc, output_rel->pathlist)
    {
        Path *path = lfirst(lc);
        Path *lower_path;
        ProjectionPath *project_path;
        Path *deferred_path;

        if (IsA(path, LimitPath))
            continue;

        lower_path = copy_path_with_deferred_projection_target(root, path,
                                                               lower_target,
                                                               false);
        if (lower_path == NULL || lower_path == path)
            continue;

        project_path = create_projection_path(root, output_rel, lower_path,
                                              final_target);
        deferred_path = (Path *)project_path;
        deferred_path->rows = path->rows;
        deferred_path->disabled_nodes = path->disabled_nodes;
        deferred_path->startup_cost = path->startup_cost;
        deferred_path->total_cost = Max(path->startup_cost,
                                        path->total_cost - 0.25);

        new_paths = lappend(new_paths, deferred_path);
    }

    foreach(lc, new_paths)
        add_path(output_rel, lfirst(lc));
}

static void add_narrow_array_agg_property_paths(PlannerInfo *root,
                                                RelOptInfo *output_rel)
{
    CypherArrayAggPropertyHandoff handoff;
    Path *best_narrow_path = NULL;
    ListCell *lc;

    if (root == NULL || output_rel == NULL || output_rel->pathlist == NIL)
        return;

    if (!cypher_find_array_agg_property_handoff(output_rel->reltarget,
                                                &handoff))
        return;

    foreach(lc, output_rel->pathlist)
    {
        Path *path = lfirst(lc);
        Path *new_path;

        new_path = try_narrow_array_agg_property_path(root, path, &handoff);
        if (new_path != NULL)
        {
            lfirst(lc) = new_path;
            if (best_narrow_path == NULL ||
                compare_path_costs(new_path, best_narrow_path, TOTAL_COST) < 0)
            {
                best_narrow_path = new_path;
            }
            if (output_rel->cheapest_total_path == path)
                output_rel->cheapest_total_path = new_path;
            if (output_rel->cheapest_startup_path == path)
                output_rel->cheapest_startup_path = new_path;
        }
    }

    if (best_narrow_path != NULL)
    {
        output_rel->cheapest_total_path = best_narrow_path;
        output_rel->cheapest_startup_path = best_narrow_path;
    }
}

static Path *try_narrow_array_agg_property_path(
    PlannerInfo *root, Path *path, CypherArrayAggPropertyHandoff *handoff)
{
    AggPath *agg_path;
    PathTarget *narrow_target;
    PathTarget *agg_target;
    Path *narrow_subpath;
    Path *agg_subpath;
    AggPath *new_agg;
    List *arg_plans;
    double materialization_credit;
    double type_vector_credit;
    double index_domain_credit;
    double input_rows;

    if (path == NULL || handoff == NULL || handoff->arg_exprs == NIL ||
        !IsA(path, AggPath))
    {
        return NULL;
    }
    if (handoff->cached_property_slot_count <= 0 ||
        list_length(handoff->cached_property_slots) !=
        handoff->cached_property_slot_count ||
        list_length(handoff->payload_value_types) !=
        handoff->cached_property_slot_count ||
        handoff->property_descriptor_slot_count !=
        handoff->cached_property_slot_count)
    {
        return NULL;
    }

    agg_path = (AggPath *)path;
    if (agg_path->aggstrategy == AGG_PLAIN)
    {
        if (agg_path->groupClause != NIL)
            return NULL;
    }
    else if (agg_path->aggstrategy == AGG_SORTED ||
             agg_path->aggstrategy == AGG_HASHED)
    {
        if (agg_path->groupClause == NIL)
            return NULL;
    }
    else
    {
        return NULL;
    }

    if (agg_path->qual != NIL || agg_path->subpath == NULL)
        return NULL;

    cypher_refresh_array_agg_property_index_domains(root, handoff);

    if (list_length(handoff->arg_exprs) == 1 &&
        pathtarget_contains_only_expr(agg_path->subpath->pathtarget,
                                      linitial(handoff->arg_exprs)))
    {
        return NULL;
    }

    arg_plans = cypher_build_array_agg_property_arg_plans(
        agg_path->subpath->pathtarget, handoff);
    if (arg_plans == NIL)
        return NULL;

    narrow_target = create_empty_pathtarget();
    if (!cypher_add_aggregate_group_exprs_to_target(narrow_target, agg_path))
        return NULL;
    if (!cypher_add_array_agg_property_arg_plans_to_target(narrow_target,
                                                           arg_plans))
        return NULL;
    narrow_target = set_pathtarget_cost_width(root, narrow_target);

    narrow_subpath = copy_path_with_deferred_projection_target(
        root, agg_path->subpath, narrow_target, false);
    if (narrow_subpath == NULL)
        return NULL;

    agg_subpath = (Path *)create_projection_path(root, narrow_subpath->parent,
                                                 narrow_subpath,
                                                 narrow_target);

    agg_target = cypher_build_array_agg_property_target(
        root, agg_path->path.pathtarget, handoff, arg_plans);
    if (agg_target == NULL)
        return NULL;

    new_agg = palloc(sizeof(*new_agg));
    memcpy(new_agg, agg_path, sizeof(*new_agg));
    new_agg->path.pathtype = T_Agg;
    new_agg->path.pathtarget = agg_target;
    new_agg->subpath = agg_subpath;
    new_agg->path.startup_cost = agg_path->path.startup_cost;
    input_rows = agg_path->subpath->rows;
    cypher_array_agg_property_materialization_credits(handoff, input_rows,
                                                      &type_vector_credit,
                                                      &index_domain_credit);
    materialization_credit = type_vector_credit + index_domain_credit;
    new_agg->path.total_cost = Max(agg_path->path.startup_cost,
                                   agg_path->path.total_cost -
                                       materialization_credit);

    {
        char *descriptor;

        descriptor = cypher_format_array_agg_property_handoff(handoff);
        ereport(DEBUG2,
                (errmsg_internal("AGE array_agg property handoff: %s "
                                 "input_rows=%.0f type-credit=%.2f "
                                 "index-credit=%.2f credit=%.2f",
                                 descriptor, input_rows,
                                 type_vector_credit, index_domain_credit,
                                 materialization_credit)));
        pfree(descriptor);
    }

    return (Path *)new_agg;
}

static void add_narrow_typed_collect_paths(PlannerInfo *root,
                                           RelOptInfo *output_rel)
{
    List *handoffs = NIL;
    Path *best_narrow_path = NULL;
    ListCell *lc;

    if (root == NULL || output_rel == NULL || output_rel->pathlist == NIL)
        return;

    if (!cypher_find_multi_typed_collect_handoffs(output_rel->reltarget,
                                                  &handoffs))
    {
        return;
    }

    foreach(lc, output_rel->pathlist)
    {
        Path *path = lfirst(lc);
        Path *new_path;

        new_path = try_narrow_typed_collect_path(root, output_rel, path,
                                                 handoffs);
        if (new_path != NULL)
        {
            lfirst(lc) = new_path;
            if (best_narrow_path == NULL ||
                compare_path_costs(new_path, best_narrow_path, TOTAL_COST) < 0)
            {
                best_narrow_path = new_path;
            }
            if (output_rel->cheapest_total_path == path)
                output_rel->cheapest_total_path = new_path;
            if (output_rel->cheapest_startup_path == path)
                output_rel->cheapest_startup_path = new_path;
        }
    }

    if (best_narrow_path != NULL)
    {
        output_rel->cheapest_total_path = best_narrow_path;
        output_rel->cheapest_startup_path = best_narrow_path;
    }
}

static Path *try_narrow_typed_collect_path(PlannerInfo *root,
                                           RelOptInfo *output_rel,
                                           Path *path,
                                           List *handoffs)
{
    AggPath *agg_path;
    PathTarget *narrow_target;
    PathTarget *agg_target;
    Path *narrow_subpath;
    Path *agg_subpath;
    AggPath *new_agg;
    List *arg_plans;
    double materialization_credit;
    double input_rows;

    if (path == NULL || handoffs == NIL || !IsA(path, AggPath))
        return NULL;

    agg_path = (AggPath *)path;
    if (agg_path->aggstrategy == AGG_PLAIN)
    {
        if (agg_path->groupClause != NIL)
            return NULL;
    }
    else if (agg_path->aggstrategy == AGG_SORTED ||
             agg_path->aggstrategy == AGG_HASHED)
    {
        if (agg_path->groupClause == NIL)
            return NULL;
    }
    else
    {
        return NULL;
    }

    if (agg_path->qual != NIL || agg_path->subpath == NULL)
        return NULL;

    arg_plans = cypher_build_typed_collect_arg_plans(
        agg_path->subpath->pathtarget, handoffs);
    if (arg_plans == NIL)
        return NULL;

    if (list_length(arg_plans) == 1)
    {
        CypherTypedCollectArgPlan *arg_plan = linitial(arg_plans);

        if (pathtarget_contains_only_expr(agg_path->subpath->pathtarget,
                                          arg_plan->arg))
        {
            return NULL;
        }
    }

    /*
     * create_agg_plan() asks its child for CP_LABEL_TLIST, which otherwise
     * lets a projection-capable base scan fall back to its physical tlist.
     */
    narrow_target = create_empty_pathtarget();
    if (!cypher_add_aggregate_group_exprs_to_target(narrow_target, agg_path))
        return NULL;
    if (!cypher_add_typed_collect_arg_plans_to_target(narrow_target,
                                                      arg_plans))
        return NULL;
    narrow_target = set_pathtarget_cost_width(root, narrow_target);

    narrow_subpath = copy_path_with_deferred_projection_target(
        root, agg_path->subpath, narrow_target, false);
    if (narrow_subpath == NULL)
        return NULL;

    agg_subpath = narrow_subpath;
    if (list_length(handoffs) == 1 &&
        ((CypherTypedCollectHandoff *)linitial(handoffs))->aggref->aggdistinct == NIL)
    {
        agg_subpath = (Path *)create_projection_path(root,
                                                     narrow_subpath->parent,
                                                     narrow_subpath,
                                                     narrow_target);
    }

    agg_target = cypher_build_typed_collect_agg_target(
        root, agg_path->path.pathtarget, arg_plans);
    if (agg_target == NULL)
        return NULL;

    new_agg = palloc(sizeof(*new_agg));
    memcpy(new_agg, agg_path, sizeof(*new_agg));
    new_agg->path.pathtype = T_Agg;
    new_agg->path.pathtarget = agg_target;
    new_agg->subpath = agg_subpath;
    new_agg->path.startup_cost = agg_path->path.startup_cost;
    input_rows = agg_path->subpath->rows;
    materialization_credit =
        cypher_typed_collect_materialization_credit(arg_plans, input_rows);
    new_agg->path.total_cost = Max(agg_path->path.startup_cost,
                                   agg_path->path.total_cost -
                                       materialization_credit);

    return (Path *)new_agg;
}

static bool pathtarget_contains_only_expr(PathTarget *target, Node *expr)
{
    if (target == NULL || expr == NULL || list_length(target->exprs) != 1)
        return false;

    return equal(linitial(target->exprs), expr);
}

static void add_property_projection_custom_path(PlannerInfo *root,
                                                RelOptInfo *input_rel,
                                                RelOptInfo *output_rel,
                                                List *slots,
                                                FinalPathExtraData *extra)
{
    CustomPath *cp;
    Path *path;
    RelOptInfo *base_rel;
    RangeTblEntry *base_rte;
    Var *properties_var;
    CypherCachedPropertySlotDescriptor *first_slot;
    Index scanrelid;
    int scanrelid_int;
    Path *reference_path;
    double rows;
    double pages;
    double materialization_credit;
    Cost local_random_page_cost;
    Cost local_seq_page_cost;

    if (input_rel == NULL || output_rel == NULL || slots == NIL ||
        root->parse == NULL ||
        input_rel->pathlist == NIL ||
        !bms_get_singleton_member(input_rel->relids, &scanrelid_int))
    {
        return;
    }
    scanrelid = (Index)scanrelid_int;
    first_slot = linitial(slots);

    if (first_slot->container == NULL ||
        first_slot->keys == NIL ||
        !IsA(first_slot->container, Var))
    {
        return;
    }

    properties_var = castNode(Var, first_slot->container);
    if (properties_var->varno != scanrelid ||
        scanrelid <= 0 ||
        scanrelid >= root->simple_rel_array_size)
    {
        return;
    }
    foreach_ptr(CypherCachedPropertySlotDescriptor, slot, slots)
    {
        Var *slot_properties_var;

        if (slot->container == NULL ||
            slot->keys == NIL ||
            !IsA(slot->container, Var))
        {
            return;
        }

        slot_properties_var = castNode(Var, slot->container);
        if (slot_properties_var->varno != scanrelid ||
            slot_properties_var->varattno !=
            Anum_ag_label_vertex_table_properties)
        {
            return;
        }
    }

    base_rel = root->simple_rel_array[scanrelid];
    base_rte = root->simple_rte_array[scanrelid];
    if (base_rel == NULL ||
        base_rte == NULL ||
        base_rte->rtekind != RTE_RELATION ||
        base_rel->reloptkind != RELOPT_BASEREL ||
        base_rel->baserestrictinfo != NIL ||
        base_rel->rows <= 0 ||
        base_rel->pages <= 0)
    {
        return;
    }

    reference_path = linitial(input_rel->pathlist);
    rows = clamp_row_est(Max(base_rel->rows, 1.0));
    pages = Max(base_rel->pages, 1);
    get_tablespace_page_costs(base_rel->reltablespace, &local_random_page_cost,
                              &local_seq_page_cost);
    (void) local_random_page_cost;

    cp = makeNode(CustomPath);
    cp->path.pathtype = T_CustomScan;
    cp->path.parent = output_rel;
    cp->path.pathtarget = input_rel->reltarget;
    cp->path.param_info = NULL;
    cp->path.parallel_aware = false;
    cp->path.parallel_safe = false;
    cp->path.parallel_workers = 0;
    cp->path.pathkeys = reference_path->pathkeys;
    materialization_credit =
        cypher_property_projection_materialization_credit(slots, rows);
    cp->path.rows = rows;
    cp->path.startup_cost = 0;
    cp->path.total_cost = local_seq_page_cost * pages + cpu_tuple_cost * rows;
    cp->path.total_cost = Max(cp->path.startup_cost,
                              cp->path.total_cost - materialization_credit);
    cp->flags = CUSTOMPATH_SUPPORT_PROJECTION;
    cp->custom_paths = NIL;
    cp->custom_private = list_make2(make_property_projection_slot_private(slots),
                                    makeInteger(scanrelid));
    cp->methods = &age_property_projection_path_methods;

    path = (Path *)cp;
    if (extra != NULL && extra->limit_needed)
    {
        path = (Path *)create_limit_path(root, output_rel, path,
                                         root->parse->limitOffset,
                                         root->parse->limitCount,
                                         root->parse->limitOption,
                                         extra->offset_est,
                                         extra->count_est);
    }

    add_path(output_rel, path);

    ereport(DEBUG2,
            (errmsg_internal("AGE property projection CustomPath added: "
                             "scanrelid=%u rows=%.0f total_cost=%.2f "
                             "materialization_credit=%.2f "
                             "slot_count=%d limit_needed=%s",
                             scanrelid, cp->path.rows,
                             cp->path.total_cost,
                             materialization_credit,
                             list_length(slots),
                             extra != NULL && extra->limit_needed ?
                             "true" : "false")));
}

static List *make_property_projection_slot_descriptor(
    CypherCachedPropertySlotDescriptor *slot)
{
    return list_make4(copyObject(slot->keys),
                      make_oid_const(slot->value_type),
                      make_oid_const(slot->field_result_type),
                      makeInteger(slot->final_materialization_weight));
}

static List *make_property_projection_slot_private(List *slots)
{
    List *slot_private = NIL;
    ListCell *lc;

    foreach(lc, slots)
    {
        CypherCachedPropertySlotDescriptor *slot = lfirst(lc);

        /*
         * A list-output slot (RETURN [a, b, ...]) is encoded as a String tag
         * "agtype_list" followed by one 4-element descriptor per list element,
         * so the executor can tell it apart from a scalar slot (whose first
         * member is the key-path List) and rebuild the agtype list directly.
         */
        if (slot->is_list_output)
        {
            List *element_privs = NIL;
            ListCell *elem_lc;

            foreach(elem_lc, slot->list_elements)
            {
                element_privs = lappend(
                    element_privs,
                    make_property_projection_slot_descriptor(lfirst(elem_lc)));
            }

            slot_private = lappend(
                slot_private,
                lcons(makeString(AGE_PROPERTY_PROJECTION_LIST_TAG),
                      element_privs));
        }
        else
        {
            slot_private = lappend(
                slot_private,
                make_property_projection_slot_descriptor(slot));
        }
    }

    return slot_private;
}

static Plan *plan_age_property_projection_path(PlannerInfo *root,
                                               RelOptInfo *rel,
                                               CustomPath *best_path,
                                               List *tlist, List *clauses,
                                               List *custom_plans)
{
    CustomScan *cs;
    List *slot_private;
    Integer *scanrelid_value;
    Index scanrelid;

    (void) root;
    (void) rel;
    (void) clauses;
    (void) custom_plans;

    slot_private = linitial_node(List, best_path->custom_private);
    scanrelid_value = lsecond_node(Integer, best_path->custom_private);
    scanrelid = intVal(scanrelid_value);

    cs = makeNode(CustomScan);
    cs->scan.plan.startup_cost = best_path->path.startup_cost;
    cs->scan.plan.total_cost = best_path->path.total_cost;
    cs->scan.plan.plan_rows = best_path->path.rows;
    cs->scan.plan.plan_width = best_path->path.pathtarget->width;
    cs->scan.plan.parallel_aware = false;
    cs->scan.plan.parallel_safe = false;
    cs->scan.plan.async_capable = false;
    cs->scan.plan.targetlist = tlist;
    cs->scan.plan.qual = NIL;
    cs->scan.plan.lefttree = NULL;
    cs->scan.plan.righttree = NULL;
    cs->scan.scanrelid = scanrelid;

    cs->flags = best_path->flags;
    cs->custom_plans = NIL;
    cs->custom_exprs = NIL;
    cs->custom_private = copyObject(slot_private);
    cs->custom_scan_tlist = copyObject(tlist);
    cs->custom_relids = bms_make_singleton(scanrelid);
    cs->methods = &age_property_projection_scan_methods;

    return (Plan *)cs;
}

static Const *make_oid_const(Oid value)
{
    return makeConst(OIDOID, -1, InvalidOid, sizeof(Oid),
                     ObjectIdGetDatum(value), false, true);
}

static bool is_age_vle_values_rte(RangeTblEntry *rte, List **func_args)
{
    List *row;
    List *args = NIL;
    ListCell *lc;
    int colno = 0;
    Const *marker;

    if (func_args != NULL)
        *func_args = NIL;

    if (rte == NULL ||
        rte->rtekind != RTE_VALUES ||
        list_length(rte->values_lists) != 1 ||
        rte->eref == NULL ||
        rte->eref->colnames == NIL)
    {
        return false;
    }

    row = linitial_node(List, rte->values_lists);
    if (list_length(row) != AGE_VLE_STREAM_ARG_GRAMMAR_NODE + 3 &&
        list_length(row) != AGE_VLE_STREAM_ARG_TERMINAL_PROPERTY + 3 &&
        list_length(row) != AGE_VLE_STREAM_ARG_TERMINAL_LABEL + 3)
    {
        return false;
    }
    if (!IsA(lsecond(row), Const))
        return false;

    marker = lsecond_node(Const, row);
    if (marker->constisnull ||
        marker->consttype != TEXTOID ||
        strcmp(TextDatumGetCString(marker->constvalue),
               AGE_VLE_STREAM_MARKER) != 0)
    {
        return false;
    }

    foreach(lc, row)
    {
        if (colno > 1)
            args = lappend(args, lfirst(lc));
        colno++;
    }

    if (func_args != NULL)
        *func_args = args;

    return true;
}

static void add_age_vle_stream_custom_path(PlannerInfo *root, RelOptInfo *rel,
                                           RangeTblEntry *rte,
                                           List *func_args,
                                           const char *graph_pattern_key)
{
    CustomPath *cp;
    Path *reference_path;
    List *custom_private = NIL;
    List *range_direction;
    List *edge_source;
    AgeGraphJoinLoweringArtifact *graph_join_artifact;
    AgeGraphJoinLoweringArtifact *shared_graph_join_artifact;
    AgeGraphJoinCandidateTable *graph_join_table;
    Node *terminal_property_predicate_expr = NULL;

    if (rel == NULL ||
        rel->reloptkind != RELOPT_BASEREL ||
        rel->pathlist == NIL ||
        func_args == NIL)
    {
        return;
    }

    reference_path = linitial(rel->pathlist);
    if (rte->rtekind == RTE_VALUES)
        rel->pathlist = NIL;

    cp = makeNode(CustomPath);
    cp->path.pathtype = T_CustomScan;
    cp->path.parent = rel;
    cp->path.pathtarget = rel->reltarget;
    cp->path.param_info = reference_path->param_info;
    cp->path.parallel_aware = false;
    cp->path.parallel_safe = false;
    cp->path.parallel_workers = 0;
    cp->path.pathkeys = reference_path->pathkeys;
    cp->flags = 0;
    cp->custom_paths = list_make1(reference_path);
    custom_private = lappend(custom_private, copyObject(func_args));
    custom_private = lappend(custom_private,
                             makeInteger(list_length(func_args)));
    custom_private = lappend(custom_private,
                             make_age_vle_stream_const_flags(
                                 func_args));
    custom_private = lappend(custom_private,
                             make_age_vle_stream_graph(func_args));
    custom_private = lappend(custom_private,
                             make_age_vle_stream_edge(func_args));
    range_direction = make_age_vle_stream_range_direction(func_args);
    custom_private = lappend(custom_private, range_direction);
    custom_private = lappend(custom_private,
                             make_age_vle_stream_output(func_args));
    edge_source = make_age_vle_stream_edge_source(
        list_nth_node(List, custom_private, 3),
        list_nth_node(List, custom_private, 4),
        range_direction,
        list_nth_node(List, custom_private, 6),
        func_args, rel->baserestrictinfo,
        &terminal_property_predicate_expr, root);
    custom_private = lappend(custom_private, edge_source);
    custom_private = lappend(
        custom_private,
        terminal_property_predicate_expr != NULL ?
        terminal_property_predicate_expr :
        (Node *)makeNullConst(AGTYPEOID, -1, InvalidOid));
    cp->custom_private = custom_private;
    cost_age_vle_stream_custom_path(cp, reference_path, range_direction,
                                    edge_source);
    graph_join_artifact =
        make_vle_stream_graph_join_artifact(root, rel, cp, func_args,
                                            edge_source, graph_pattern_key);
    shared_graph_join_artifact =
        age_graph_join_register_rel_lowering_artifact_pool(root, rel,
                                                           graph_join_artifact);
    if (shared_graph_join_artifact == NULL)
        shared_graph_join_artifact = graph_join_artifact;
    graph_join_table = age_graph_join_register_rel_lowering_artifact_path(
        root, rel, &cp->path, shared_graph_join_artifact);
    Assert(graph_join_table != NULL);
    (void) age_graph_join_apply_selected_path_cost(graph_join_table,
                                                   &cp->path);
    cp->custom_private = lappend(cp->custom_private,
                                 age_graph_join_table_selected_private(
                                     graph_join_table));
    cp->methods = &age_vle_stream_path_methods;

    add_path(rel, (Path *)cp);

    (void) root;
    ereport(DEBUG2,
            (errmsg_internal("AGE VLE stream CustomPath added: relid=%u "
                             "rows=%.0f total_cost=%.2f columns=%d",
                             rel->relid, cp->path.rows, cp->path.total_cost,
                             list_length(rte->eref->colnames))));
}

static void cost_age_vle_stream_custom_path(CustomPath *cp,
                                            Path *reference_path,
                                            List *range_direction,
                                            List *edge_source)
{
    int64 upper;
    int64 direction;
    int64 start_fanout;
    int64 end_fanout;
    int64 composite_fanout;
    int64 materialization_weight;
    int64 depth_factor;
    double fanout;
    double estimated_rows;
    double cpu_weight;
    AgeVLEStreamCompositePlanKind composite_plan_kind;

    upper = vle_stream_descriptor_int8_field(
        range_direction, AGE_VLE_STREAM_RANGE_UPPER_VALUE, 1);
    direction = vle_stream_descriptor_int8_field(
        range_direction, AGE_VLE_STREAM_DIRECTION_VALUE,
        CYPHER_REL_DIR_RIGHT);
    start_fanout = vle_stream_descriptor_int8_field(
        edge_source, AGE_VLE_STREAM_EDGE_SOURCE_START_FANOUT, 0);
    end_fanout = vle_stream_descriptor_int8_field(
        edge_source, AGE_VLE_STREAM_EDGE_SOURCE_END_FANOUT, 0);
    composite_fanout = vle_stream_descriptor_int8_field(
        edge_source, AGE_VLE_STREAM_EDGE_SOURCE_COMPOSITE_SOURCE_FANOUT, 0);
    materialization_weight = vle_stream_descriptor_int8_field(
        edge_source,
        AGE_VLE_STREAM_EDGE_SOURCE_POLICY_MATERIALIZATION_WEIGHT, 1);
    composite_plan_kind =
        (AgeVLEStreamCompositePlanKind)vle_stream_descriptor_int8_field(
            edge_source,
            AGE_VLE_STREAM_EDGE_SOURCE_COMPOSITE_SOURCE_PLANNED_KIND,
            AGE_VLE_STREAM_COMPOSITE_PLAN_UNKNOWN);

    if (direction == CYPHER_REL_DIR_LEFT)
        fanout = end_fanout;
    else if (direction == CYPHER_REL_DIR_NONE)
        fanout = start_fanout + end_fanout;
    else
        fanout = start_fanout;

    if (age_vle_stream_composite_plan_is_property_prefilter(
            composite_plan_kind) &&
        composite_fanout > 0)
    {
        fanout = Min(fanout, (double)composite_fanout);
    }

    depth_factor = upper > 0 ? Min(upper, 4) : 4;
    estimated_rows = fanout > 0.0 ?
        clamp_row_est(fanout * depth_factor) : 1.0;
    cpu_weight = Max(materialization_weight, 1) * cpu_tuple_cost;

    cp->path.rows = estimated_rows;
    cp->path.startup_cost = reference_path->startup_cost;
    cp->path.total_cost = reference_path->startup_cost +
        Max(reference_path->total_cost - reference_path->startup_cost, 0.0) *
        0.50 + estimated_rows * cpu_weight;
}

static AgeGraphJoinLoweringArtifact *make_vle_stream_graph_join_artifact(
    PlannerInfo *root, RelOptInfo *rel, CustomPath *cp, List *func_args,
    List *edge_source, const char *graph_pattern_key)
{
    AgeGraphJoinCandidateTable *table;
    AgeGraphJoinCandidateTable *terminal_property_table;
    AgeGraphJoinLoweringInput lowering_input;
    AgeGraphJoinConnectorKind base_connector_kind;
    AgeGraphJoinConnectorKind composite_connector_kind;
    AgeGraphJoinBoundKind bound_kind;
    AgeVLEStreamCompositePlanKind composite_plan_kind;
    bool has_bound_endpoints;
    bool has_composite_prefilter;
    bool has_matrix_frontier;
    bool terminal_property_prefilter_eligible;
    bool terminal_property_predicate_known;
    VLESourceDirectionClass policy_active_direction_id;
    AgeGraphJoinOrderPropertyKind base_order_property_kind;
    double start_fanout;
    double end_fanout;
    double base_fanout;
    double composite_fanout;
    double materialization_weight;
    double matrix_scheduled_work;
    bool matrix_prescan_compact;
    bool matrix_candidate_added = false;
    const double matrix_frontier_min_fanout = 16.0;

    table = graph_pattern_registry_typed_entry_table(
        graph_pattern_key, AGE_GRAPH_JOIN_SOURCE_VLE_MARKER,
        AGE_GRAPH_JOIN_COMPONENT_VLE_EXPANSION);
    if (table == NULL)
        return NULL;
    lowering_input = make_vle_stream_graph_join_input(root, rel, cp,
                                                      func_args);

    policy_active_direction_id =
        (VLESourceDirectionClass)vle_stream_descriptor_int8_field(
            edge_source,
            AGE_VLE_STREAM_EDGE_SOURCE_POLICY_ACTIVE_DIRECTION_KIND,
            VLE_SOURCE_DIRECTION_NONE);
    bound_kind = graph_join_bound_kind_from_vle_direction(
        policy_active_direction_id);
    has_bound_endpoints = vle_stream_func_args_have_bound_endpoints(
        func_args);
    if (has_bound_endpoints)
    {
        base_connector_kind = AGE_GRAPH_JOIN_CONNECTOR_VLE_EXPAND_INTO;
    }
    else if (policy_active_direction_id == VLE_SOURCE_DIRECTION_BOTH)
    {
        base_connector_kind =
            AGE_GRAPH_JOIN_CONNECTOR_VLE_BIDIRECTIONAL_EXPAND;
    }
    else
    {
        base_connector_kind = AGE_GRAPH_JOIN_CONNECTOR_VLE_EXPAND;
    }
    base_order_property_kind = vle_stream_base_join_order_property_kind(
        func_args, edge_source);
    start_fanout = (double)vle_stream_descriptor_int8_field(
        edge_source, AGE_VLE_STREAM_EDGE_SOURCE_START_FANOUT, 0);
    end_fanout = (double)vle_stream_descriptor_int8_field(
        edge_source, AGE_VLE_STREAM_EDGE_SOURCE_END_FANOUT, 0);
    materialization_weight = (double)vle_stream_descriptor_int8_field(
        edge_source,
        AGE_VLE_STREAM_EDGE_SOURCE_POLICY_MATERIALIZATION_WEIGHT, 1);
    if (policy_active_direction_id == VLE_SOURCE_DIRECTION_IN)
        base_fanout = end_fanout;
    else if (policy_active_direction_id == VLE_SOURCE_DIRECTION_BOTH)
        base_fanout = start_fanout + end_fanout;
    else
        base_fanout = start_fanout;
    if (base_fanout <= 0.0)
        base_fanout = Max(cp->path.rows, 1.0);
    if (materialization_weight <= 0.0)
        materialization_weight = 1.0;
    composite_plan_kind =
        (AgeVLEStreamCompositePlanKind)vle_stream_descriptor_int8_field(
            edge_source,
            AGE_VLE_STREAM_EDGE_SOURCE_COMPOSITE_SOURCE_PLANNED_KIND,
            AGE_VLE_STREAM_COMPOSITE_PLAN_UNKNOWN);
    has_composite_prefilter = vle_stream_edge_source_bool_field(
        edge_source, AGE_VLE_STREAM_EDGE_SOURCE_COMPOSITE_SOURCE_KNOWN) &&
        age_vle_stream_composite_plan_is_property_prefilter(
            composite_plan_kind);
    terminal_property_prefilter_eligible = vle_stream_edge_source_bool_field(
        edge_source,
        AGE_VLE_STREAM_EDGE_SOURCE_TERMINAL_PROPERTY_PREFILTER_ELIGIBLE);
    terminal_property_predicate_known = vle_stream_edge_source_bool_field(
        edge_source,
        AGE_VLE_STREAM_EDGE_SOURCE_TERMINAL_PROPERTY_PREDICATE_KNOWN);
    has_matrix_frontier = !has_bound_endpoints &&
        !has_composite_prefilter &&
        base_fanout >= matrix_frontier_min_fanout &&
        (base_order_property_kind == AGE_GRAPH_JOIN_ORDER_VLE_FRONTIER ||
         policy_active_direction_id != VLE_SOURCE_DIRECTION_BOTH);

    if (!has_composite_prefilter)
    {
        matrix_prescan_compact = has_matrix_frontier &&
            vle_stream_matrix_frontier_has_prescan_compact(edge_source, NULL);
        if (matrix_prescan_compact)
        {
            matrix_scheduled_work =
                vle_stream_matrix_frontier_scheduled_work(
                    edge_source, policy_active_direction_id, base_fanout,
                    cp->path.rows,
                    (double)vle_stream_descriptor_int8_field(
                        edge_source,
                        AGE_VLE_STREAM_EDGE_SOURCE_COMPOSITE_SOURCE_FANOUT,
                        0),
                    composite_plan_kind);
            (void) age_graph_join_table_add_lowered_typed_scheduled_candidate(
                table, &cp->path, &lowering_input, bound_kind,
                AGE_GRAPH_JOIN_COMPONENT_VLE_EXPANSION,
                AGE_GRAPH_JOIN_CONNECTOR_MATRIX_FRONTIER_EXPAND,
                AGE_GRAPH_JOIN_ORDER_MATRIX_FRONTIER,
                AGE_GRAPH_JOIN_SOURCE_EVIDENCE_VLE_MATRIX_FRONTIER,
                AGE_GRAPH_JOIN_SOURCE_VLE_MARKER,
                clamp_row_est(matrix_scheduled_work), cp->path.startup_cost,
                cp->path.startup_cost +
                matrix_scheduled_work * cpu_operator_cost * 0.02 +
                Max(matrix_scheduled_work, 1.0) * cpu_tuple_cost * 0.005,
                true);
            matrix_candidate_added = true;
        }
        add_vle_stream_expand_candidate(
            table, cp, &lowering_input, bound_kind, base_connector_kind,
            base_order_property_kind,
            AGE_GRAPH_JOIN_SOURCE_EVIDENCE_VLE_EDGE_SOURCE,
            has_bound_endpoints, base_fanout);
        if (has_matrix_frontier && !matrix_candidate_added)
        {
            matrix_scheduled_work =
                vle_stream_matrix_frontier_scheduled_work(
                    edge_source, policy_active_direction_id, base_fanout,
                    cp->path.rows,
                    (double)vle_stream_descriptor_int8_field(
                        edge_source,
                        AGE_VLE_STREAM_EDGE_SOURCE_COMPOSITE_SOURCE_FANOUT,
                        0),
                    composite_plan_kind);
            (void) age_graph_join_table_add_lowered_typed_scheduled_candidate(
                table, &cp->path, &lowering_input, bound_kind,
                AGE_GRAPH_JOIN_COMPONENT_VLE_EXPANSION,
                AGE_GRAPH_JOIN_CONNECTOR_MATRIX_FRONTIER_EXPAND,
                AGE_GRAPH_JOIN_ORDER_MATRIX_FRONTIER,
                AGE_GRAPH_JOIN_SOURCE_EVIDENCE_VLE_MATRIX_FRONTIER,
                AGE_GRAPH_JOIN_SOURCE_VLE_MARKER,
                clamp_row_est(matrix_scheduled_work), cp->path.startup_cost,
                cp->path.startup_cost +
                matrix_scheduled_work * cpu_operator_cost *
                0.15 +
                Max(matrix_scheduled_work, 1.0) * cpu_tuple_cost *
                0.05,
                true);
        }
    }

    if (has_composite_prefilter)
    {
        composite_fanout = (double)vle_stream_descriptor_int8_field(
            edge_source, AGE_VLE_STREAM_EDGE_SOURCE_COMPOSITE_SOURCE_FANOUT,
            0);

        if (composite_fanout <= 0.0)
            composite_fanout = Max(cp->path.rows, 1.0);

        if (terminal_property_prefilter_eligible &&
            terminal_property_predicate_known)
        {
            terminal_property_table = graph_pattern_registry_typed_entry_table(
                graph_pattern_key, AGE_GRAPH_JOIN_SOURCE_VLE_TERMINAL_PROPERTY,
                AGE_GRAPH_JOIN_COMPONENT_NODE_PROPERTY_SEEK);
            if (terminal_property_table != NULL)
            {
                (void) age_graph_join_table_add_lowered_typed_scheduled_candidate(
                    terminal_property_table, &cp->path, &lowering_input,
                    bound_kind,
                    AGE_GRAPH_JOIN_COMPONENT_NODE_PROPERTY_SEEK,
                    AGE_GRAPH_JOIN_CONNECTOR_NODE_PROPERTY_INDEX_SEEK,
                    AGE_GRAPH_JOIN_ORDER_INDEX_ANCHORED,
                    AGE_GRAPH_JOIN_SOURCE_EVIDENCE_NODE_PROPERTY_INDEX,
                    AGE_GRAPH_JOIN_SOURCE_VLE_TERMINAL_PROPERTY,
                    clamp_row_est(composite_fanout), cp->path.startup_cost,
                    cp->path.startup_cost +
                    Max(composite_fanout, 1.0) * cpu_operator_cost * 0.03,
                    false);
            }
        }

        if (has_bound_endpoints)
        {
            composite_connector_kind =
                AGE_GRAPH_JOIN_CONNECTOR_VLE_COMPOSITE_EXPAND_INTO;
        }
        else
        {
            composite_connector_kind =
                AGE_GRAPH_JOIN_CONNECTOR_VLE_COMPOSITE_EXPAND;
        }

        (void) age_graph_join_table_add_lowered_typed_scheduled_candidate(
            table, &cp->path, &lowering_input, bound_kind,
            AGE_GRAPH_JOIN_COMPONENT_VLE_EXPANSION,
            composite_connector_kind,
            AGE_GRAPH_JOIN_ORDER_INDEX_ANCHORED,
            AGE_GRAPH_JOIN_SOURCE_EVIDENCE_VLE_EDGE_SOURCE,
            AGE_GRAPH_JOIN_SOURCE_VLE_MARKER,
            cp->path.rows, cp->path.startup_cost, cp->path.startup_cost,
            true);

        (void) age_graph_join_table_add_lowered_typed_scheduled_candidate(
            table, &cp->path, &lowering_input, bound_kind,
            AGE_GRAPH_JOIN_COMPONENT_VLE_EXPANSION,
            has_bound_endpoints ? AGE_GRAPH_JOIN_CONNECTOR_VLE_EXPAND :
            base_connector_kind,
            base_order_property_kind,
            AGE_GRAPH_JOIN_SOURCE_EVIDENCE_VLE_EDGE_SOURCE,
            AGE_GRAPH_JOIN_SOURCE_VLE_MARKER,
            clamp_row_est(base_fanout), cp->path.startup_cost,
            cp->path.startup_cost +
            base_fanout * materialization_weight * cpu_tuple_cost +
            Max(base_fanout - composite_fanout, 1.0) * cpu_operator_cost,
            true);
    }

    return graph_pattern_registry_artifact(
        graph_pattern_key != NULL ? graph_pattern_key : "graph-pattern");
}

static AgeGraphJoinLoweringInput make_vle_stream_graph_join_input(
    PlannerInfo *root, RelOptInfo *rel, CustomPath *cp, List *func_args)
{
    AgeGraphJoinLoweringInput input;
    Relids solved_relids;
    ListCell *lc;

    memset(&input, 0, sizeof(input));
    solved_relids = rel != NULL ? bms_copy(rel->relids) : NULL;

    foreach(lc, func_args)
    {
        Node *arg = lfirst(lc);
        Relids arg_relids;

        if (arg == NULL)
            continue;

        arg_relids = pull_varnos(root, arg);
        if (arg_relids != NULL)
            solved_relids = bms_add_members(solved_relids, arg_relids);
    }

    input.solved_relids = solved_relids;
    input.required_outer = cp != NULL ? PATH_REQ_OUTER(&cp->path) : NULL;
    input.provided_relids = rel != NULL ? rel->relids : NULL;
    if (cp != NULL && cp->path.pathtarget != NULL &&
        cp->path.pathtarget->width > 0)
        input.output_width = cp->path.pathtarget->width;
    else if (rel != NULL && rel->reltarget != NULL)
        input.output_width = rel->reltarget->width;

    return input;
}

static void add_vle_stream_expand_candidate(
    AgeGraphJoinCandidateTable *table, CustomPath *cp,
    const AgeGraphJoinLoweringInput *lowering_input,
    AgeGraphJoinBoundKind bound_kind,
    AgeGraphJoinConnectorKind connector_kind,
    AgeGraphJoinOrderPropertyKind order_property_kind,
    AgeGraphJoinSourceEvidenceKind source_evidence_kind,
    bool has_bound_endpoints, double base_fanout)
{
    double connector_rows;

    Assert(table != NULL);
    Assert(cp != NULL);

    if (!has_bound_endpoints)
    {
        (void) age_graph_join_table_add_lowered_typed_path_candidate(
            table, &cp->path, lowering_input, bound_kind,
            AGE_GRAPH_JOIN_COMPONENT_VLE_EXPANSION, connector_kind,
            order_property_kind, source_evidence_kind,
            AGE_GRAPH_JOIN_SOURCE_VLE_MARKER, true);
        return;
    }

    connector_rows = clamp_row_est(Max(1.0, Min(base_fanout, cp->path.rows)));
    (void) age_graph_join_table_add_lowered_typed_scheduled_candidate(
        table, &cp->path, lowering_input, bound_kind,
        AGE_GRAPH_JOIN_COMPONENT_VLE_EXPANSION, connector_kind,
        order_property_kind, AGE_GRAPH_JOIN_SOURCE_EVIDENCE_VLE_EDGE_SOURCE,
        AGE_GRAPH_JOIN_SOURCE_VLE_MARKER,
        connector_rows,
        cp->path.startup_cost,
        cp->path.startup_cost +
        connector_rows * cpu_operator_cost * 0.08 +
        connector_rows * cpu_tuple_cost * 0.03,
        true);
}

static double vle_stream_matrix_frontier_scheduled_work(
    List *edge_source, VLESourceDirectionClass direction_id,
    double base_fanout,
    double path_rows, double composite_fanout,
    AgeVLEStreamCompositePlanKind composite_plan_kind)
{
    AgeVLEStreamFanoutSourceKind fanout_source_kind;
    VLESourceValuePostingKind value_posting_source_kind;
    double scheduled_work;
    double prescan_compact_fanout;

    if (direction_id == VLE_SOURCE_DIRECTION_IN)
    {
        fanout_source_kind =
            (AgeVLEStreamFanoutSourceKind)vle_stream_descriptor_int8_field(
                edge_source,
                AGE_VLE_STREAM_EDGE_SOURCE_END_FANOUT_SOURCE_KIND,
                AGE_VLE_STREAM_FANOUT_SOURCE_UNKNOWN);
        value_posting_source_kind =
            (VLESourceValuePostingKind)vle_stream_descriptor_int8_field(
                edge_source,
                AGE_VLE_STREAM_EDGE_SOURCE_END_VALUE_POSTING_SOURCE_KIND,
                VLE_SOURCE_VALUE_POSTING_NONE);
    }
    else
    {
        fanout_source_kind =
            (AgeVLEStreamFanoutSourceKind)vle_stream_descriptor_int8_field(
                edge_source,
                AGE_VLE_STREAM_EDGE_SOURCE_START_FANOUT_SOURCE_KIND,
                AGE_VLE_STREAM_FANOUT_SOURCE_UNKNOWN);
        value_posting_source_kind =
            (VLESourceValuePostingKind)vle_stream_descriptor_int8_field(
                edge_source,
                AGE_VLE_STREAM_EDGE_SOURCE_START_VALUE_POSTING_SOURCE_KIND,
                VLE_SOURCE_VALUE_POSTING_NONE);
    }

    scheduled_work = Max(base_fanout, 1.0);
    if (age_vle_stream_fanout_source_is_directory_label(fanout_source_kind))
    {
        scheduled_work = Min(Max(path_rows, 1.0), scheduled_work);
        if (value_posting_source_kind != VLE_SOURCE_VALUE_POSTING_NONE)
        {
            scheduled_work = Max(1.0, scheduled_work * 0.50);
        }
    }
    if (composite_fanout > 0.0 &&
        composite_plan_kind != AGE_VLE_STREAM_COMPOSITE_PLAN_NONE)
    {
        scheduled_work = Min(scheduled_work, Max(composite_fanout, 1.0));
    }
    if (vle_stream_matrix_frontier_has_prescan_compact(
            edge_source, &prescan_compact_fanout))
    {
        scheduled_work = Min(scheduled_work,
                             Max(prescan_compact_fanout, 1.0));
    }

    return scheduled_work;
}

static bool vle_stream_matrix_frontier_has_prescan_compact(
    List *edge_source, double *compact_fanout)
{
    AgeVLEStreamCompositePlanKind composite_plan_kind;
    int64 terminal_match_count;
    int64 composite_candidate_fanout;

    if (compact_fanout != NULL)
        *compact_fanout = 0.0;
    if (edge_source == NIL ||
        !vle_stream_edge_source_bool_field(
            edge_source,
            AGE_VLE_STREAM_EDGE_SOURCE_TERMINAL_PROPERTY_SOURCE_KNOWN) ||
        !vle_stream_edge_source_bool_field(
            edge_source,
            AGE_VLE_STREAM_EDGE_SOURCE_TERMINAL_PROPERTY_PREDICATE_KNOWN))
    {
        return false;
    }

    composite_plan_kind =
        (AgeVLEStreamCompositePlanKind)vle_stream_descriptor_int8_field(
            edge_source,
            AGE_VLE_STREAM_EDGE_SOURCE_COMPOSITE_SOURCE_PLANNED_KIND,
            AGE_VLE_STREAM_COMPOSITE_PLAN_UNKNOWN);
    if (composite_plan_kind == AGE_VLE_STREAM_COMPOSITE_PLAN_UNKNOWN ||
        age_vle_stream_composite_plan_is_property_prefilter(
            composite_plan_kind) ||
        composite_plan_kind == AGE_VLE_STREAM_COMPOSITE_PLAN_NONE)
    {
        return false;
    }

    terminal_match_count = vle_stream_descriptor_int8_field(
        edge_source, AGE_VLE_STREAM_EDGE_SOURCE_TERMINAL_PROPERTY_MATCH_COUNT,
        0);
    composite_candidate_fanout = vle_stream_descriptor_int8_field(
        edge_source, AGE_VLE_STREAM_EDGE_SOURCE_COMPOSITE_SOURCE_CANDIDATE_FANOUT,
        0);
    if (terminal_match_count <= 0 || composite_candidate_fanout <= 0)
        return false;

    if (compact_fanout != NULL)
        *compact_fanout = Min((double)terminal_match_count,
                              (double)composite_candidate_fanout);

    return true;
}

static AgeGraphJoinOrderPropertyKind vle_stream_base_join_order_property_kind(
    List *func_args, List *edge_source)
{
    AgeVLEStreamFanoutSourceKind start_fanout_source_kind;
    AgeVLEStreamFanoutSourceKind end_fanout_source_kind;

    start_fanout_source_kind =
        (AgeVLEStreamFanoutSourceKind)vle_stream_descriptor_int8_field(
            edge_source, AGE_VLE_STREAM_EDGE_SOURCE_START_FANOUT_SOURCE_KIND,
            AGE_VLE_STREAM_FANOUT_SOURCE_UNKNOWN);
    end_fanout_source_kind =
        (AgeVLEStreamFanoutSourceKind)vle_stream_descriptor_int8_field(
            edge_source, AGE_VLE_STREAM_EDGE_SOURCE_END_FANOUT_SOURCE_KIND,
            AGE_VLE_STREAM_FANOUT_SOURCE_UNKNOWN);
    if (age_vle_stream_fanout_source_is_directory_label(
            start_fanout_source_kind) ||
        age_vle_stream_fanout_source_is_directory_label(
            end_fanout_source_kind))
        return AGE_GRAPH_JOIN_ORDER_VLE_FRONTIER;

    if (vle_stream_edge_source_bool_field(
            edge_source,
            AGE_VLE_STREAM_EDGE_SOURCE_CACHE_SEED_ELIGIBLE) ||
        vle_stream_edge_source_bool_field(
            edge_source,
            AGE_VLE_STREAM_EDGE_SOURCE_EMPTY_LIFECYCLE_ELIGIBLE) ||
        vle_stream_edge_source_bool_field(
            edge_source,
            AGE_VLE_STREAM_EDGE_SOURCE_PAYLOAD_INPUT_KNOWN))
        return AGE_GRAPH_JOIN_ORDER_VLE_FRONTIER;

    if (vle_stream_func_args_have_bound_endpoints(func_args))
        return AGE_GRAPH_JOIN_ORDER_EXPAND_INTO;

    return AGE_GRAPH_JOIN_ORDER_QUERY;
}

static bool vle_stream_func_args_have_bound_endpoints(List *func_args)
{
    Node *start_arg;
    Node *end_arg;

    if (list_length(func_args) <= AGE_VLE_STREAM_ARG_END)
        return false;

    start_arg = list_nth(func_args, AGE_VLE_STREAM_ARG_START);
    end_arg = list_nth(func_args, AGE_VLE_STREAM_ARG_END);
    if (IsA(start_arg, Const) && ((Const *)start_arg)->constisnull)
        return false;
    if (IsA(end_arg, Const) && ((Const *)end_arg)->constisnull)
        return false;

    return true;
}

static AgeGraphJoinBoundKind graph_join_bound_kind_from_vle_direction(
    VLESourceDirectionClass direction)
{
    switch (direction)
    {
        case VLE_SOURCE_DIRECTION_OUT:
            return AGE_GRAPH_JOIN_BOUND_VLE_OUT;
        case VLE_SOURCE_DIRECTION_IN:
            return AGE_GRAPH_JOIN_BOUND_VLE_IN;
        case VLE_SOURCE_DIRECTION_BOTH:
            return AGE_GRAPH_JOIN_BOUND_VLE_BOTH;
        case VLE_SOURCE_DIRECTION_MIXED:
            return AGE_GRAPH_JOIN_BOUND_VLE_MIXED;
        case VLE_SOURCE_DIRECTION_NONE:
            break;
    }

    return AGE_GRAPH_JOIN_BOUND_UNKNOWN;
}

static Plan *plan_age_vle_stream_path(PlannerInfo *root, RelOptInfo *rel,
                                      CustomPath *best_path, List *tlist,
                                      List *clauses, List *custom_plans)
{
    CustomScan *cs;
    List *func_args;
    Integer *nargs_value;
    List *const_flags;
    List *graph;
    List *edge;
    List *range_direction;
    List *output;
    List *edge_source;
    List *graph_join_descriptor = NIL;
    Node *terminal_property_predicate_expr;
    Node *plan_terminal_property_predicate_expr = NULL;
    List *scan_quals;
    AgeGraphJoinLoweringArtifact *graph_join_artifact;
    AgeGraphJoinCandidateTable *graph_join_table;

    func_args = linitial_node(List, best_path->custom_private);
    nargs_value = lsecond_node(Integer, best_path->custom_private);
    const_flags = list_nth_node(List, best_path->custom_private, 2);
    graph = list_nth_node(List, best_path->custom_private, 3);
    edge = list_nth_node(List, best_path->custom_private, 4);
    range_direction = list_nth_node(List, best_path->custom_private, 5);
    output = list_nth_node(List, best_path->custom_private, 6);
    edge_source = list_nth_node(List, best_path->custom_private, 7);
    terminal_property_predicate_expr = list_nth(best_path->custom_private, 8);
    edge_source = make_age_vle_stream_edge_source(
        graph, edge, range_direction, output, func_args, clauses,
        &plan_terminal_property_predicate_expr, root);
    if (plan_terminal_property_predicate_expr != NULL)
        terminal_property_predicate_expr = plan_terminal_property_predicate_expr;
    graph_join_artifact =
        make_vle_stream_graph_join_artifact(root, rel, best_path,
                                            func_args, edge_source, NULL);
    graph_join_table = age_graph_join_bind_lowering_artifact_to_path(
        &best_path->path, graph_join_artifact);
    Assert(graph_join_table != NULL);
    graph_join_descriptor = age_graph_join_table_selected_private(
        graph_join_table);

    cs = makeNode(CustomScan);
    cs->scan.plan.startup_cost = best_path->path.startup_cost;
    cs->scan.plan.total_cost = best_path->path.total_cost;
    cs->scan.plan.plan_rows = best_path->path.rows;
    cs->scan.plan.plan_width = rel->reltarget->width;
    cs->scan.plan.parallel_aware = false;
    cs->scan.plan.parallel_safe = false;
    cs->scan.plan.async_capable = false;
    cs->scan.plan.targetlist = tlist;
    scan_quals = extract_actual_clauses(clauses, false);
    scan_quals = remove_vle_terminal_property_predicate_quals(scan_quals,
                                                              edge_source);
    scan_quals = rewrite_vle_terminal_property_quals(scan_quals, output);
    cs->scan.plan.qual = scan_quals;
    cs->scan.plan.lefttree = NULL;
    cs->scan.plan.righttree = NULL;
    cs->scan.scanrelid = 0;

    cs->flags = best_path->flags;
    cs->custom_plans = remove_vle_terminal_property_child_plan_quals(
        custom_plans, edge_source, output);
    cs->custom_exprs = copyObject(func_args);
    if (terminal_property_predicate_expr != NULL &&
        !IsA(terminal_property_predicate_expr, Const))
    {
        cs->custom_exprs = lappend(cs->custom_exprs,
                                   copyObject(terminal_property_predicate_expr));
    }
    cs->custom_private = NIL;
    cs->custom_private = lappend(cs->custom_private, copyObject(nargs_value));
    cs->custom_private = lappend(cs->custom_private, copyObject(const_flags));
    cs->custom_private = lappend(cs->custom_private, copyObject(graph));
    cs->custom_private = lappend(cs->custom_private, copyObject(edge));
    cs->custom_private = lappend(cs->custom_private,
                                 copyObject(range_direction));
    cs->custom_private = lappend(cs->custom_private, copyObject(output));
    cs->custom_private = lappend(cs->custom_private,
                                 copyObject(edge_source));
    cs->custom_private = lappend(
        cs->custom_private, copyObject(terminal_property_predicate_expr));
    cs->custom_private = lappend(cs->custom_private,
                                 copyObject(graph_join_descriptor));
    cs->custom_scan_tlist = copyObject(tlist);
    cs->custom_relids = bms_make_singleton(rel->relid);
    cs->methods = &age_vle_stream_scan_methods;

    return (Plan *)cs;
}

static List *remove_vle_terminal_property_child_plan_quals(
    List *custom_plans, List *edge_source, List *output)
{
    ListCell *lc;

    foreach(lc, custom_plans)
    {
        Plan *plan = lfirst(lc);

        if (plan == NULL || plan->qual == NIL)
            continue;

        plan->qual = remove_vle_terminal_property_predicate_quals(
            plan->qual, edge_source);
        plan->qual = rewrite_vle_terminal_property_quals(plan->qual, output);
    }

    return custom_plans;
}

static List *remove_vle_terminal_property_predicate_quals(
    List *clauses, List *edge_source)
{
    VLETerminalPropertyQualRewriteContext context;
    List *filtered = NIL;
    Oid agtype_eq_oid;
    char *predicate_key;
    ListCell *lc;

    if (clauses == NIL || edge_source == NIL ||
        list_length(edge_source) != AGE_VLE_STREAM_EDGE_SOURCE_COUNT ||
        intVal(list_nth_node(Integer, edge_source,
                             AGE_VLE_STREAM_EDGE_SOURCE_TERMINAL_PROPERTY_PREDICATE_KNOWN)) == 0)
    {
        return clauses;
    }

    {
        Const *key_const;

        key_const = list_nth_node(
            Const, edge_source,
            AGE_VLE_STREAM_EDGE_SOURCE_TERMINAL_PROPERTY_PREDICATE_KEY);
        if (key_const->constisnull)
            return clauses;
        predicate_key = TextDatumGetCString(key_const->constvalue);
    }
    if (predicate_key == NULL)
        return clauses;

    context.terminal_key = predicate_key;
    context.matched_key = NULL;
    context.agtype_access_operator_oid =
        get_ag_func_oid("agtype_access_operator", 1, AGTYPEARRAYOID);
    context.age_vle_terminal_vertex_oid =
        get_ag_func_oid("age_vle_terminal_vertex", 1, AGTYPEOID);
    agtype_eq_oid = get_ag_func_oid("agtype_eq", 2, AGTYPEOID, AGTYPEOID);

    foreach(lc, clauses)
    {
        Node *clause = lfirst(lc);
        VLETerminalPropertyPredicate predicate = {0};

        if (match_vle_terminal_property_predicate_expr(
                clause, &context, agtype_eq_oid, &predicate))
        {
            if (predicate.key != NULL)
                pfree(predicate.key);
            continue;
        }
        filtered = lappend(filtered, clause);
    }

    pfree(predicate_key);
    return filtered;
}

static List *rewrite_vle_terminal_property_quals(List *clauses, List *output)
{
    VLETerminalPropertyQualRewriteContext context;
    char *terminal_key = NULL;
    bool terminal_key_null = true;
    bool terminal_property_output;

    if (clauses == NIL || output == NIL ||
        list_length(output) != AGE_VLE_STREAM_OUTPUT_COUNT)
    {
        return clauses;
    }

    terminal_property_output =
        DatumGetInt64(list_nth_node(Const, output,
                                    AGE_VLE_STREAM_OUTPUT_REQUIREMENT)->constvalue) ==
        AGE_VLE_OUTPUT_REQUIREMENT_TERMINAL_PROPERTY;
    if (!terminal_property_output ||
        intVal(list_nth_node(Integer, output,
                             AGE_VLE_STREAM_OUTPUT_TERMINAL_KEY_KNOWN)) == 0 ||
        intVal(list_nth_node(Integer, output,
                             AGE_VLE_STREAM_OUTPUT_TERMINAL_KEY_NULL)) != 0)
    {
        return clauses;
    }

    terminal_key = TextDatumGetCString(
        list_nth_node(Const, output,
                      AGE_VLE_STREAM_OUTPUT_TERMINAL_KEY_VALUE)->constvalue);
    terminal_key_null = terminal_key == NULL;
    if (terminal_key_null)
        return clauses;

    context.terminal_key = terminal_key;
    context.matched_key = NULL;
    context.agtype_access_operator_oid =
        get_ag_func_oid("agtype_access_operator", 1, AGTYPEARRAYOID);
    context.age_vle_terminal_vertex_oid =
        get_ag_func_oid("age_vle_terminal_vertex", 1, AGTYPEOID);

    clauses = (List *)expression_tree_mutator(
        (Node *)clauses, rewrite_vle_terminal_property_qual_mutator,
        &context);
    pfree(terminal_key);

    return clauses;
}

static Node *rewrite_vle_terminal_property_qual_mutator(Node *node,
                                                        void *context)
{
    Node *rewritten;

    if (node == NULL)
        return NULL;

    if (IsA(node, FuncExpr))
    {
        rewritten = try_rewrite_vle_terminal_property_access(
            castNode(FuncExpr, node),
            (VLETerminalPropertyQualRewriteContext *)context);
        if (rewritten != NULL)
            return rewritten;
    }

    return expression_tree_mutator(node,
                                   rewrite_vle_terminal_property_qual_mutator,
                                   context);
}

static Node *try_rewrite_vle_terminal_property_access(
    FuncExpr *func, VLETerminalPropertyQualRewriteContext *context)
{
    ArrayExpr *access_args;
    FuncExpr *terminal_vertex;
    Node *terminal_arg;
    Node *key_arg;
    char *key = NULL;
    bool key_null = false;

    Assert(func != NULL);
    Assert(context != NULL);

    if (func->funcid != context->agtype_access_operator_oid ||
        list_length(func->args) != 1 ||
        !IsA(linitial(func->args), ArrayExpr))
    {
        return NULL;
    }

    access_args = linitial_node(ArrayExpr, func->args);
    if (list_length(access_args->elements) != 2)
        return NULL;

    terminal_arg = linitial(access_args->elements);
    key_arg = lsecond(access_args->elements);
    if (terminal_arg == NULL || !IsA(terminal_arg, FuncExpr))
        return NULL;

    terminal_vertex = castNode(FuncExpr, terminal_arg);
    if (terminal_vertex->funcid != context->age_vle_terminal_vertex_oid ||
        list_length(terminal_vertex->args) != 1)
    {
        return NULL;
    }

    if (!get_age_vle_stream_string_const(key_arg, &key, &key_null) ||
        key_null || key == NULL)
    {
        return NULL;
    }

    if (context->terminal_key != NULL &&
        strcmp(key, context->terminal_key) != 0)
    {
        pfree(key);
        return NULL;
    }
    if (context->terminal_key == NULL)
    {
        if (context->matched_key != NULL)
            pfree(context->matched_key);
        context->matched_key = pstrdup(key);
    }
    pfree(key);

    return (Node *)copyObject(linitial(terminal_vertex->args));
}

static bool extract_vle_terminal_property_predicate(
    List *restrictinfos, List *output, VLETerminalPropertyPredicate *predicate)
{
    VLETerminalPropertyQualRewriteContext context;
    char *terminal_key = NULL;
    Oid agtype_eq_oid;
    ListCell *lc;

    Assert(predicate != NULL);
    memset(predicate, 0, sizeof(*predicate));

    if (restrictinfos == NIL || output == NIL ||
        list_length(output) != AGE_VLE_STREAM_OUTPUT_COUNT)
    {
        return false;
    }

    if (intVal(list_nth_node(Integer, output,
                             AGE_VLE_STREAM_OUTPUT_TERMINAL_KEY_KNOWN)) != 0 &&
        intVal(list_nth_node(Integer,
                             output,
                             AGE_VLE_STREAM_OUTPUT_TERMINAL_KEY_NULL)) == 0)
    {
        terminal_key = TextDatumGetCString(
            list_nth_node(Const, output,
                          AGE_VLE_STREAM_OUTPUT_TERMINAL_KEY_VALUE)->constvalue);
    }

    context.terminal_key = terminal_key;
    context.matched_key = NULL;
    context.agtype_access_operator_oid =
        get_ag_func_oid("agtype_access_operator", 1, AGTYPEARRAYOID);
    context.age_vle_terminal_vertex_oid =
        get_ag_func_oid("age_vle_terminal_vertex", 1, AGTYPEOID);
    agtype_eq_oid = get_ag_func_oid("agtype_eq", 2, AGTYPEOID, AGTYPEOID);

    foreach(lc, restrictinfos)
    {
        RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc);

        if (match_vle_terminal_property_predicate_expr(
                (Node *)rinfo->clause, &context, agtype_eq_oid, predicate))
        {
            if (predicate->key == NULL && context.matched_key != NULL)
                predicate->key = pstrdup(context.matched_key);
            if (context.matched_key != NULL)
                pfree(context.matched_key);
            if (terminal_key != NULL)
                pfree(terminal_key);
            return true;
        }
        if (context.matched_key != NULL)
        {
            pfree(context.matched_key);
            context.matched_key = NULL;
        }
    }

    if (terminal_key != NULL)
        pfree(terminal_key);
    return false;
}

static bool match_vle_terminal_property_predicate_expr(
    Node *node, VLETerminalPropertyQualRewriteContext *context,
    Oid agtype_eq_oid, VLETerminalPropertyPredicate *predicate)
{
    OpExpr *op;
    Node *left;
    Node *right;
    Node *rewritten;
    Node *value_expr;

    if (node == NULL || !IsA(node, OpExpr))
        return false;

    op = castNode(OpExpr, node);
    if (op->opfuncid != agtype_eq_oid || list_length(op->args) != 2)
        return false;

    left = linitial(op->args);
    right = lsecond(op->args);
    if (IsA(left, FuncExpr) && IsA(right, Const))
    {
        rewritten = try_rewrite_vle_terminal_property_access(
            castNode(FuncExpr, left), context);
        value_expr = right;
    }
    else if (IsA(left, Const) && IsA(right, FuncExpr))
    {
        rewritten = try_rewrite_vle_terminal_property_access(
            castNode(FuncExpr, right), context);
        value_expr = left;
    }
    else if (IsA(left, FuncExpr))
    {
        rewritten = try_rewrite_vle_terminal_property_access(
            castNode(FuncExpr, left), context);
        value_expr = right;
    }
    else if (IsA(right, FuncExpr))
    {
        rewritten = try_rewrite_vle_terminal_property_access(
            castNode(FuncExpr, right), context);
        value_expr = left;
    }
    else
    {
        return false;
    }

    if (rewritten == NULL)
        return false;
    if (exprType(value_expr) != AGTYPEOID)
        return false;

    predicate->known = true;
    if (context->terminal_key != NULL)
        predicate->key = pstrdup(context->terminal_key);
    else if (context->matched_key != NULL)
        predicate->key = pstrdup(context->matched_key);
    predicate->value_expr = value_expr;
    if (IsA(value_expr, Const))
    {
        Const *value = castNode(Const, value_expr);

        predicate->isnull = value->constisnull;
        predicate->value = value;
    }

    return true;
}

static List *make_age_vle_stream_const_flags(List *func_args)
{
    List *flags = NIL;
    ListCell *lc;

    foreach(lc, func_args)
    {
        Node *arg = (Node *)lfirst(lc);

        flags = lappend(flags, makeInteger(IsA(arg, Const) ? 1 : 0));
    }

    return flags;
}

static List *make_age_vle_stream_graph(List *func_args)
{
    char *graph_name = NULL;
    bool graph_null = false;
    bool graph_known;
    List *descriptor = NIL;

    graph_known =
        list_length(func_args) > AGE_VLE_STREAM_ARG_GRAPH &&
        get_age_vle_stream_string_const(
            list_nth(func_args, AGE_VLE_STREAM_ARG_GRAPH), &graph_name,
            &graph_null);

    descriptor = lappend(descriptor, makeInteger(graph_known ? 1 : 0));
    descriptor = lappend(descriptor, makeInteger(graph_null ? 1 : 0));
    descriptor = lappend(descriptor, make_text_const(graph_name));

    if (graph_name != NULL)
        pfree(graph_name);

    return descriptor;
}

static List *make_age_vle_stream_edge(List *func_args)
{
    Node *node;
    Const *const_arg;
    agtype *agt;
    agtype_value edge_value;
    agtype_value *label_value;
    agtype_value *properties_value;
    agtype *properties = NULL;
    char *label_name = NULL;
    int properties_count = 0;
    bool needs_free = false;
    bool found;
    bool edge_known = false;
    bool label_known = false;
    bool properties_known = false;
    List *descriptor = NIL;

    if (list_length(func_args) <= AGE_VLE_STREAM_ARG_EDGE)
        goto build_descriptor;

    node = list_nth(func_args, AGE_VLE_STREAM_ARG_EDGE);
    if (node == NULL || !IsA(node, Const))
        goto build_descriptor;

    const_arg = (Const *)node;
    if (const_arg->constisnull || const_arg->consttype != AGTYPEOID)
        goto build_descriptor;

    agt = DATUM_GET_AGTYPE_P(const_arg->constvalue);
    if (!AGTYPE_CONTAINER_IS_SCALAR(&agt->root))
        goto build_descriptor;

    found = get_ith_agtype_value_from_container_no_copy(&agt->root, 0,
                                                        &edge_value,
                                                        &needs_free);
    if (!found || edge_value.type != AGTV_EDGE)
    {
        if (needs_free)
            pfree_agtype_value_content(&edge_value);
        goto build_descriptor;
    }

    edge_known = true;
    label_value = AGTYPE_EDGE_GET_LABEL(&edge_value);
    if (label_value->type == AGTV_STRING)
    {
        label_name = pnstrdup(label_value->val.string.val,
                              label_value->val.string.len);
        label_known = true;
    }

    properties_value = AGTYPE_EDGE_GET_PROPERTIES(&edge_value);
    if (properties_value->type == AGTV_OBJECT)
    {
        properties_count = properties_value->val.object.num_pairs;
        properties = agtype_value_to_agtype(properties_value);
        properties_known = true;
    }

    if (needs_free)
        pfree_agtype_value_content(&edge_value);

build_descriptor:
    descriptor = lappend(descriptor, makeInteger(edge_known ? 1 : 0));
    descriptor = lappend(descriptor, makeInteger(label_known ? 1 : 0));
    descriptor = lappend(descriptor, make_text_const(label_name));
    descriptor = lappend(descriptor, makeInteger(properties_known ? 1 : 0));
    descriptor = lappend(descriptor, makeInteger(properties == NULL ? 1 : 0));
    descriptor = lappend(descriptor, make_agtype_const(properties));
    descriptor = lappend(descriptor, make_int8_const(properties_count));

    if (label_name != NULL)
        pfree(label_name);

    return descriptor;
}

static List *make_age_vle_stream_range_direction(List *func_args)
{
    int64 lower = 0;
    int64 upper = 0;
    int64 direction = 0;
    bool lower_null = false;
    bool upper_null = false;
    bool direction_null = false;
    bool lower_known;
    bool upper_known;
    bool direction_known;
    List *descriptor = NIL;

    lower_known =
        list_length(func_args) > AGE_VLE_STREAM_ARG_LOWER &&
        get_age_vle_stream_integer_const(
            list_nth(func_args, AGE_VLE_STREAM_ARG_LOWER), &lower,
            &lower_null);
    upper_known =
        list_length(func_args) > AGE_VLE_STREAM_ARG_UPPER &&
        get_age_vle_stream_integer_const(
            list_nth(func_args, AGE_VLE_STREAM_ARG_UPPER), &upper,
            &upper_null);
    direction_known =
        list_length(func_args) > AGE_VLE_STREAM_ARG_DIRECTION &&
        get_age_vle_stream_integer_const(
            list_nth(func_args, AGE_VLE_STREAM_ARG_DIRECTION), &direction,
            &direction_null);

    descriptor = lappend(descriptor, makeInteger(lower_known ? 1 : 0));
    descriptor = lappend(descriptor, makeInteger(lower_null ? 1 : 0));
    descriptor = lappend(descriptor, make_int8_const(lower));
    descriptor = lappend(descriptor, makeInteger(upper_known ? 1 : 0));
    descriptor = lappend(descriptor, makeInteger(upper_null ? 1 : 0));
    descriptor = lappend(descriptor, make_int8_const(upper));
    descriptor = lappend(descriptor, makeInteger(direction_known ? 1 : 0));
    descriptor = lappend(descriptor, makeInteger(direction_null ? 1 : 0));
    descriptor = lappend(descriptor, make_int8_const(direction));

    return descriptor;
}

static List *make_age_vle_stream_output(List *func_args)
{
    int64 grammar = 0;
    bool grammar_null = false;
    bool grammar_known;
    char *terminal_key = NULL;
    bool terminal_key_null = true;
    bool terminal_key_known = false;
    int terminal_key_len = 0;
    int64 terminal_label_id = 0;
    bool terminal_label_null = false;
    bool terminal_label_known = false;
    AgeVLETerminalLabelMode terminal_label_mode =
        AGE_VLE_TERMINAL_LABEL_NONE;
    AgeVLEOutputRequirement output_requirement =
        AGE_VLE_OUTPUT_REQUIREMENT_PATH;
    bool materializer_vertex_prefetch;
    List *descriptor = NIL;

    grammar_known =
        list_length(func_args) > AGE_VLE_STREAM_ARG_GRAMMAR_NODE &&
        get_age_vle_stream_integer_const(
            list_nth(func_args, AGE_VLE_STREAM_ARG_GRAMMAR_NODE), &grammar,
            &grammar_null);
    if (list_length(func_args) > AGE_VLE_STREAM_ARG_TERMINAL_PROPERTY)
    {
        terminal_key_known =
            get_age_vle_stream_string_const(
                list_nth(func_args, AGE_VLE_STREAM_ARG_TERMINAL_PROPERTY),
                &terminal_key, &terminal_key_null);
        if (terminal_key_known)
        {
            if (terminal_key_null)
            {
                output_requirement =
                    AGE_VLE_OUTPUT_REQUIREMENT_TERMINAL_PROPERTIES;
            }
            else
            {
                output_requirement =
                    AGE_VLE_OUTPUT_REQUIREMENT_TERMINAL_PROPERTY;
            }
            if (!terminal_key_null && terminal_key != NULL)
            {
                terminal_key_len = strlen(terminal_key);
            }
        }
        if (!terminal_key_known)
        {
            terminal_label_known =
                get_age_vle_stream_integer_const(
                    list_nth(func_args, AGE_VLE_STREAM_ARG_TERMINAL_PROPERTY),
                    &terminal_label_id, &terminal_label_null);
            if (terminal_label_null)
                terminal_label_known = false;
            else if (terminal_label_known && terminal_label_id < 0)
            {
                terminal_label_id = -(terminal_label_id + 1);
                terminal_label_mode = AGE_VLE_TERMINAL_LABEL_ENDPOINT_ONLY;
            }
            else if (terminal_label_known)
            {
                terminal_label_mode = AGE_VLE_TERMINAL_LABEL_ALL_DEPTH;
            }
        }
    }
    if (list_length(func_args) > AGE_VLE_STREAM_ARG_TERMINAL_LABEL)
    {
        terminal_label_known =
            get_age_vle_stream_integer_const(
                list_nth(func_args, AGE_VLE_STREAM_ARG_TERMINAL_LABEL),
                &terminal_label_id, &terminal_label_null);
        if (terminal_label_null)
            terminal_label_known = false;
        else if (terminal_label_known && terminal_label_id < 0)
        {
            terminal_label_id = -(terminal_label_id + 1);
            terminal_label_mode = AGE_VLE_TERMINAL_LABEL_ENDPOINT_ONLY;
        }
        else if (terminal_label_known &&
                 terminal_label_mode == AGE_VLE_TERMINAL_LABEL_NONE)
        {
            terminal_label_mode = AGE_VLE_TERMINAL_LABEL_ALL_DEPTH;
        }
    }
    if (output_requirement == AGE_VLE_OUTPUT_REQUIREMENT_PATH &&
        grammar_known && !grammar_null && grammar < 0)
    {
        output_requirement = AGE_VLE_OUTPUT_REQUIREMENT_TERMINAL_VERTEX;
    }
    materializer_vertex_prefetch =
        output_requirement == AGE_VLE_OUTPUT_REQUIREMENT_PATH;

    descriptor = lappend(descriptor, makeInteger(grammar_known ? 1 : 0));
    descriptor = lappend(descriptor, makeInteger(grammar_null ? 1 : 0));
    descriptor = lappend(descriptor, make_int8_const(grammar));
    descriptor = lappend(descriptor,
                         make_int8_const((int64)output_requirement));
    descriptor = lappend(descriptor, makeInteger(terminal_key_known ? 1 : 0));
    descriptor = lappend(descriptor, makeInteger(terminal_key_null ? 1 : 0));
    descriptor = lappend(descriptor, make_text_const(terminal_key));
    descriptor = lappend(descriptor, make_int8_const(terminal_key_len));
    descriptor = lappend(descriptor,
                         makeInteger(terminal_key_len == 1 ? 1 : 0));
    descriptor = lappend(descriptor,
                         makeInteger(terminal_label_known ? 1 : 0));
    descriptor = lappend(descriptor, make_int8_const(terminal_label_id));
    descriptor = lappend(descriptor, make_int8_const(terminal_label_mode));
    descriptor = lappend(descriptor,
                         makeInteger(materializer_vertex_prefetch ? 1 : 0));
    descriptor = lappend(
        descriptor,
        make_int8_const(AGE_VERTEX_PROPERTY_PREFETCH_MIN_REL_CANDIDATES));

    if (terminal_key != NULL)
        pfree(terminal_key);

    return descriptor;
}

static List *make_age_vle_stream_edge_source(List *graph, List *edge,
                                             List *range_direction,
                                             List *output,
                                             List *func_args,
                                             List *restrictinfos,
                                             Node **terminal_property_predicate_expr,
                                             PlannerInfo *root)
{
    AgeVLEStreamEdgeSourceKind source_kind =
        AGE_VLE_STREAM_EDGE_SOURCE_GLOBAL_METADATA;
    List *descriptor = NIL;
    bool graph_known;
    bool graph_null;
    bool edge_known;
    bool label_known;
    bool properties_known;
    bool grammar_known;
    bool grammar_null;
    bool direction_known;
    bool direction_null;
    bool upper_known;
    bool upper_null;
    int64 properties_count;
    int64 output_requirement;
    int64 terminal_label_id;
    int64 terminal_label_mode;
    int64 direction_value;
    int64 upper_value;
    bool adjacency_out = false;
    bool adjacency_in = false;
    bool endpoint_start = false;
    bool endpoint_end = false;
    bool local_edge_state = false;
    bool anonymous_edge_label = false;
    Oid edge_label_oid = InvalidOid;
    Oid age_adjacency_out_index_oid = InvalidOid;
    Oid age_adjacency_in_index_oid = InvalidOid;
    VLESourceFanoutEvidence source_evidence = {0};
    VLETraversalSourceCandidates source_candidates;
    VLETraversalSourceLayoutInput source_input;
    VLETraversalSourceLayoutDecision source_decision;
    VLEStreamSourceCostInput cost_input;
    VLEStreamSourceCostDecision cost_decision;
    AgeVLEStreamDirectedSourceKind outgoing_kind =
        AGE_VLE_STREAM_DIRECTED_SOURCE_NONE;
    AgeVLEStreamDirectedSourceKind incoming_kind =
        AGE_VLE_STREAM_DIRECTED_SOURCE_NONE;
    AgeVLEStreamDirectedSourceKind policy_outgoing_kind =
        AGE_VLE_STREAM_DIRECTED_SOURCE_NONE;
    AgeVLEStreamDirectedSourceKind policy_incoming_kind =
        AGE_VLE_STREAM_DIRECTED_SOURCE_NONE;
    char *cost_policy = NULL;
    VLESourceConsumerClass policy_consumer_class_id =
        VLE_SOURCE_CONSUMER_UNKNOWN;
    AgeVLEOutputRequirement policy_output_requirement =
        AGE_VLE_OUTPUT_REQUIREMENT_UNKNOWN;
    VLESourceDirectionClass policy_active_direction_id =
        VLE_SOURCE_DIRECTION_NONE;
    int64 policy_fanout_budget = 0;
    int64 policy_materialization_weight = 0;
    VLESourcePolicyClass policy_class_id = VLE_SOURCE_POLICY_CLASS_NONE;
    VLESourcePolicyRecommendation policy_recommendation_kind =
        VLE_SOURCE_RECOMMENDATION_NONE;
    bool cache_seed_eligible = false;
    int64 endpoint_headroom_percent = 0;
    bool empty_lifecycle_eligible = false;
    int64 empty_lifecycle_depth = 0;
    int64 empty_lifecycle_batch_size = 0;
    bool threshold_input_known = false;
    int64 threshold_input_headroom_percent = 0;
    int64 threshold_input_batch_size = 0;
    int64 threshold_input_observed_count = 0;
    int64 threshold_input_saturated_count = 0;
    int64 threshold_input_relaxed_count = 0;
    VLESourceDirectionClass threshold_input_direction_id =
        VLE_SOURCE_DIRECTION_NONE;
    VLESourceThresholdFeedbackReason threshold_input_reason_id =
        VLE_SOURCE_THRESHOLD_REASON_NONE;
    VLESourcePolicyClass threshold_input_class_id =
        VLE_SOURCE_POLICY_CLASS_NONE;
    bool payload_input_known = false;
    int64 payload_input_headroom_percent = 0;
    int64 payload_input_scan_runs = 0;
    int64 payload_input_replay_runs = 0;
    int64 payload_input_seed_runs = 0;
    int64 payload_input_replay_percent = 0;
    int64 payload_input_seed_percent = 0;
    int64 payload_input_observed_count = 0;
    int64 payload_input_value_posting_observed_count = 0;
    int64 payload_input_matrix_active_percent = 0;
    int64 payload_input_matrix_filter_percent = 0;
    int64 payload_input_matrix_prescan_percent = 0;
    int64 payload_input_matrix_replay_source_percent = 0;
    int64 payload_input_matrix_run_block_percent = 0;
    int64 payload_input_matrix_regroup_percent = 0;
    int64 payload_input_matrix_compaction_percent = 0;
    int64 payload_input_matrix_dense_percent = 0;
    int64 payload_input_matrix_duplicate_percent = 0;
    int64 payload_input_matrix_compaction_pressure_percent = 0;
    int64 payload_input_matrix_residency_pressure_percent = 0;
    int64 payload_input_matrix_replay_cutover_percent = 0;
    int64 payload_input_matrix_prefetch_cutover_percent = 0;
    int64 payload_input_matrix_payload_coverage_percent = 0;
    int64 payload_input_matrix_source_overlap_percent = 0;
    int64 payload_input_matrix_reuse_proximity_percent = 0;
    AgeVLEMatrixFrontierCursorPolicy outgoing_matrix_cursor_policy = {0};
    AgeVLEMatrixFrontierCursorPolicy incoming_matrix_cursor_policy = {0};
    VLESourcePayloadFeedbackReason payload_input_reason_id =
        VLE_SOURCE_PAYLOAD_REASON_NONE;
    VLESourcePolicyClass payload_input_class_id =
        VLE_SOURCE_POLICY_CLASS_NONE;
    VLESourceValuePostingKind payload_input_value_posting_source_kind =
        VLE_SOURCE_VALUE_POSTING_NONE;
    bool terminal_property_source_known = false;
    Oid terminal_property_index_oid = InvalidOid;
    uint32 terminal_property_filter_id = 0;
    char *terminal_property_label = NULL;
    char *terminal_property_source = NULL;
    char *terminal_property_provider = NULL;
    char *terminal_property_type = NULL;
    int32 terminal_property_match_count = 0;
    const char *terminal_property_source_key = NULL;
    bool composite_source_known = false;
    bool composite_source_eligible = false;
    AgeVLEStreamCompositeSourceReason composite_source_reason_kind =
        AGE_VLE_STREAM_COMPOSITE_REASON_NONE;
    int64 composite_source_property_tuples = 0;
    int64 composite_source_candidate_fanout = 0;
    int64 composite_source_fanout = 0;
    AgeVLEStreamCompositePlanKind composite_source_planned_kind =
        AGE_VLE_STREAM_COMPOSITE_PLAN_UNKNOWN;
    double composite_source_selectivity = 0.0;
    AgeGraphPropertySelectivitySource
        composite_source_selectivity_source_kind =
            AGE_GRAPH_PROPERTY_SELECTIVITY_NONE;
    VLETerminalPropertyPredicate terminal_property_predicate = {0};
    AgeVLEStreamTerminalValueKind terminal_property_value_kind_id =
        AGE_VLE_STREAM_TERMINAL_VALUE_NONE;
    bool terminal_property_prefilter_eligible = false;
    int64 terminal_property_prefetch_threshold = 0;
    char *graph_name = NULL;
    char *label_name = NULL;
    char *terminal_key = NULL;
    bool terminal_key_null = true;

    Assert(list_length(graph) == AGE_VLE_STREAM_GRAPH_COUNT);
    Assert(list_length(edge) == AGE_VLE_STREAM_EDGE_COUNT);
    Assert(list_length(range_direction) ==
           AGE_VLE_STREAM_RANGE_DIRECTION_COUNT);
    Assert(list_length(output) == AGE_VLE_STREAM_OUTPUT_COUNT);
    if (terminal_property_predicate_expr != NULL)
        *terminal_property_predicate_expr = NULL;

    graph_known = intVal(list_nth_node(Integer, graph,
                                       AGE_VLE_STREAM_GRAPH_KNOWN)) != 0;
    graph_null = intVal(list_nth_node(Integer, graph,
                                      AGE_VLE_STREAM_GRAPH_NULL)) != 0;
    edge_known = intVal(list_nth_node(Integer, edge,
                                      AGE_VLE_STREAM_EDGE_KNOWN)) != 0;
    label_known = intVal(list_nth_node(Integer, edge,
                                       AGE_VLE_STREAM_EDGE_LABEL_KNOWN)) != 0;
    properties_known =
        intVal(list_nth_node(Integer, edge,
                             AGE_VLE_STREAM_EDGE_PROPERTIES_KNOWN)) != 0;
    properties_count = DatumGetInt64(
        list_nth_node(Const, edge,
                      AGE_VLE_STREAM_EDGE_PROPERTIES_COUNT)->constvalue);
    grammar_known =
        intVal(list_nth_node(Integer, output,
                             AGE_VLE_STREAM_OUTPUT_GRAMMAR_KNOWN)) != 0;
    grammar_null =
        intVal(list_nth_node(Integer, output,
                             AGE_VLE_STREAM_OUTPUT_GRAMMAR_NULL)) != 0;
    output_requirement = DatumGetInt64(
        list_nth_node(Const, output,
                      AGE_VLE_STREAM_OUTPUT_REQUIREMENT)->constvalue);
    terminal_label_id =
        intVal(list_nth_node(Integer, output,
                             AGE_VLE_STREAM_OUTPUT_TERMINAL_LABEL_KNOWN)) != 0 ?
        DatumGetInt64(
            list_nth_node(Const, output,
                          AGE_VLE_STREAM_OUTPUT_TERMINAL_LABEL_ID)->constvalue) :
        INVALID_LABEL_ID;
    terminal_label_mode = DatumGetInt64(
        list_nth_node(Const, output,
                      AGE_VLE_STREAM_OUTPUT_TERMINAL_LABEL_MODE)->constvalue);
    if (intVal(list_nth_node(Integer, output,
                             AGE_VLE_STREAM_OUTPUT_TERMINAL_KEY_KNOWN)) != 0 &&
        intVal(list_nth_node(Integer, output,
                             AGE_VLE_STREAM_OUTPUT_TERMINAL_KEY_NULL)) == 0)
    {
        terminal_key = TextDatumGetCString(
            list_nth_node(Const, output,
                          AGE_VLE_STREAM_OUTPUT_TERMINAL_KEY_VALUE)->constvalue);
        terminal_key_null = false;
    }
    direction_known =
        intVal(list_nth_node(Integer, range_direction,
                             AGE_VLE_STREAM_DIRECTION_KNOWN)) != 0;
    direction_null =
        intVal(list_nth_node(Integer, range_direction,
                             AGE_VLE_STREAM_DIRECTION_NULL)) != 0;
    direction_value = DatumGetInt64(
        list_nth_node(Const, range_direction,
                      AGE_VLE_STREAM_DIRECTION_VALUE)->constvalue);
    upper_known =
        intVal(list_nth_node(Integer, range_direction,
                             AGE_VLE_STREAM_RANGE_UPPER_KNOWN)) != 0;
    upper_null =
        intVal(list_nth_node(Integer, range_direction,
                             AGE_VLE_STREAM_RANGE_UPPER_NULL)) != 0;
    upper_value = DatumGetInt64(
        list_nth_node(Const, range_direction,
                      AGE_VLE_STREAM_RANGE_UPPER_VALUE)->constvalue);

    if (!graph_known || graph_null || !edge_known || !label_known ||
        !properties_known || !grammar_known || grammar_null ||
        !direction_known || direction_null)
    {
        source_kind = AGE_VLE_STREAM_EDGE_SOURCE_DYNAMIC;
        goto build_descriptor;
    }

    graph_name = TextDatumGetCString(
        list_nth_node(Const, graph,
                      AGE_VLE_STREAM_GRAPH_VALUE)->constvalue);
    label_name = TextDatumGetCString(
        list_nth_node(Const, edge,
                      AGE_VLE_STREAM_EDGE_LABEL_VALUE)->constvalue);
    anonymous_edge_label = label_name[0] == '\0';

    get_vle_stream_edge_source_indexes(graph_name, label_name,
                                       &edge_label_oid,
                                       &age_adjacency_out_index_oid,
                                       &age_adjacency_in_index_oid,
                                       &adjacency_out, &adjacency_in,
                                       &endpoint_start, &endpoint_end);
    extract_vle_terminal_property_predicate(
        restrictinfos, output, &terminal_property_predicate);
    if (terminal_property_predicate.known)
    {
        terminal_property_value_kind_id =
            terminal_property_predicate.value != NULL ?
            (terminal_property_predicate.isnull ?
             AGE_VLE_STREAM_TERMINAL_VALUE_NULL :
             AGE_VLE_STREAM_TERMINAL_VALUE_CONST) :
            AGE_VLE_STREAM_TERMINAL_VALUE_RUNTIME_SLOT;
        if (terminal_property_predicate_expr != NULL &&
            terminal_property_predicate.value == NULL)
        {
            *terminal_property_predicate_expr =
                copyObject(terminal_property_predicate.value_expr);
        }
    }

    terminal_property_source_key =
        terminal_property_predicate.key != NULL ?
        terminal_property_predicate.key :
        (!terminal_key_null ? terminal_key : NULL);
    if (terminal_property_source_key != NULL)
    {
        Oid graph_oid = get_graph_oid(graph_name);

        terminal_property_source_known =
            get_age_graph_property_index_metadata(
                graph_oid, NULL, terminal_property_source_key,
                &terminal_property_index_oid,
                &terminal_property_label, &terminal_property_source,
                &terminal_property_provider, &terminal_property_type,
                &terminal_property_match_count);
        if (!terminal_property_source_known)
        {
            composite_source_known = false;
        }
        else if (!label_id_is_valid((int32)terminal_label_id))
        {
            composite_source_known = true;
            composite_source_reason_kind =
                AGE_VLE_STREAM_COMPOSITE_REASON_MISSING_TERMINAL_LABEL;
        }
        else
        {
            int32 property_label_id = terminal_property_label == NULL ?
                INVALID_LABEL_ID :
                get_label_id(terminal_property_label, graph_oid);

            composite_source_known = true;
            composite_source_property_tuples =
                round_vle_source_cost_evidence(
                    get_vle_relation_estimated_tuples(
                        terminal_property_index_oid));
            if (!label_id_is_valid(property_label_id))
            {
                composite_source_reason_kind =
                    AGE_VLE_STREAM_COMPOSITE_REASON_PROPERTY_LABEL_UNKNOWN;
            }
            else if (property_label_id != (int32)terminal_label_id)
            {
                composite_source_reason_kind =
                    AGE_VLE_STREAM_COMPOSITE_REASON_LABEL_MISMATCH;
            }
            else
            {
                composite_source_eligible = true;
                composite_source_reason_kind =
                    terminal_label_mode == AGE_VLE_TERMINAL_LABEL_ALL_DEPTH ?
                    AGE_VLE_STREAM_COMPOSITE_REASON_TERMINAL_LABEL_PROPERTY :
                    AGE_VLE_STREAM_COMPOSITE_REASON_ENDPOINT_LABEL_ACCEPTANCE;
                terminal_property_prefilter_eligible =
                    terminal_property_predicate.known &&
                    !terminal_property_predicate.isnull &&
                    terminal_label_mode == AGE_VLE_TERMINAL_LABEL_ALL_DEPTH;
                terminal_property_prefetch_threshold =
                    terminal_property_prefilter_eligible ? 2 : 0;
                if (terminal_property_prefilter_eligible &&
                    terminal_property_predicate.value != NULL)
                {
                    terminal_property_filter_id =
                        age_adjacency_property_filter_id(
                            terminal_property_index_oid,
                            terminal_property_predicate.value->constvalue,
                            terminal_property_predicate.value->constisnull);
                }
            }
        }
    }
    estimate_vle_source_fanout_evidence(&source_evidence, edge_label_oid);
    apply_vle_stream_directory_fanout_evidence(
        &source_evidence, func_args,
        age_adjacency_out_index_oid, age_adjacency_in_index_oid,
        (int32)terminal_label_id, root);
    if (composite_source_known)
    {
        composite_source_candidate_fanout =
            choose_vle_stream_terminal_candidate_fanout(
                &source_evidence, (cypher_rel_dir)direction_value);
        composite_source_fanout = composite_source_candidate_fanout;
        if (composite_source_eligible)
        {
            if (terminal_property_prefilter_eligible &&
                terminal_property_prefetch_threshold > 0 &&
                composite_source_candidate_fanout >=
                terminal_property_prefetch_threshold)
            {
                GraphPropertySourceSelectivity property_selectivity;

                property_selectivity =
                    estimate_graph_property_source_selectivity(
                        terminal_property_index_oid, 0.15,
                        terminal_property_predicate.value);
                composite_source_planned_kind =
                    AGE_VLE_STREAM_COMPOSITE_PLAN_PROPERTY_PREFILTER;
                composite_source_selectivity = property_selectivity.selectivity;
                composite_source_selectivity_source_kind =
                    property_selectivity.source_kind;
                composite_source_fanout =
                    clamp_vle_stream_composite_fanout(
                        composite_source_candidate_fanout,
                        property_selectivity.selectivity);
            }
            else if (terminal_property_prefilter_eligible)
            {
                composite_source_planned_kind =
                    AGE_VLE_STREAM_COMPOSITE_PLAN_BELOW_THRESHOLD;
            }
            else
            {
                composite_source_planned_kind =
                    AGE_VLE_STREAM_COMPOSITE_PLAN_METADATA_ONLY;
            }
        }
        else
        {
            composite_source_planned_kind = AGE_VLE_STREAM_COMPOSITE_PLAN_NONE;
            composite_source_fanout = 0;
        }
    }
    if (properties_count == 0)
    {
        bool has_out_source = adjacency_out ||
            (!anonymous_edge_label && endpoint_start);
        bool has_in_source = adjacency_in ||
            (!anonymous_edge_label && endpoint_end);

        if (((cypher_rel_dir)direction_value == CYPHER_REL_DIR_RIGHT &&
             has_out_source) ||
            ((cypher_rel_dir)direction_value == CYPHER_REL_DIR_LEFT &&
             has_in_source) ||
            ((cypher_rel_dir)direction_value == CYPHER_REL_DIR_NONE &&
             has_out_source && has_in_source))
        {
            source_kind = AGE_VLE_STREAM_EDGE_SOURCE_LOCAL_INDEX_CANDIDATE;
            local_edge_state = true;
        }
    }

    source_candidates.age_adjacency_out = adjacency_out;
    source_candidates.age_adjacency_in = adjacency_in;
    source_candidates.endpoint_start = endpoint_start;
    source_candidates.endpoint_end = endpoint_end;

    source_input.indexes = NULL;
    source_input.upper = upper_value;
    source_input.upper_infinite = !upper_known || upper_null;
    source_input.use_local_edge_state = local_edge_state;
    source_input.label_constrained = true;
    source_input.has_property_constraints = properties_count > 0;
    source_input.preferred_source_known = false;
    source_input.preferred_outgoing_kind = VLE_TRAVERSAL_SOURCE_NONE;
    source_input.preferred_incoming_kind = VLE_TRAVERSAL_SOURCE_NONE;
    select_vle_traversal_source_layout(&source_decision, &source_input,
                                       &source_candidates);
    outgoing_kind = age_vle_stream_directed_source_from_traversal(
        source_decision.outgoing_kind);
    incoming_kind = age_vle_stream_directed_source_from_traversal(
        source_decision.incoming_kind);
    if (source_kind == AGE_VLE_STREAM_EDGE_SOURCE_LOCAL_INDEX_CANDIDATE)
    {
        cost_input.source_kind = source_kind;
        cost_input.outgoing_kind = outgoing_kind;
        cost_input.incoming_kind = incoming_kind;
        cost_input.graph_name = graph_name;
        cost_input.label_name = label_name;
        cost_input.evidence = &source_evidence;
        cost_input.upper = upper_value;
        cost_input.upper_infinite = !upper_known || upper_null;
        cost_input.direction = (cypher_rel_dir)direction_value;
        cost_input.output_requirement =
            (AgeVLEOutputRequirement)output_requirement;
        cost_input.has_property_constraints = properties_count > 0;
        cost_input.endpoint_start = endpoint_start;
        cost_input.endpoint_end = endpoint_end;
        cost_input.age_adjacency_out = adjacency_out;
        cost_input.age_adjacency_in = adjacency_in;
        cost_input.start_fanout_known = source_evidence.start_fanout_known;
        cost_input.end_fanout_known = source_evidence.end_fanout_known;
        cost_input.composite_prefilter_planned =
            age_vle_stream_composite_plan_is_property_prefilter(
                composite_source_planned_kind);
        cost_input.terminal_label_id = (int32)terminal_label_id;
        cost_input.terminal_property_index_oid = terminal_property_index_oid;
        cost_input.terminal_property_filter_id =
            terminal_property_filter_id;
        cost_input.composite_candidate_fanout =
            composite_source_candidate_fanout;
        cost_input.composite_fanout = composite_source_fanout;
        choose_vle_stream_source_cost_decision(&cost_decision, &cost_input);
        policy_outgoing_kind = cost_decision.outgoing_kind;
        policy_incoming_kind = cost_decision.incoming_kind;
        cost_policy = cost_decision.policy_text;
        policy_output_requirement =
            cost_decision.policy_output_requirement;
        policy_consumer_class_id = cost_decision.policy_consumer_class_id;
        policy_active_direction_id =
            cost_decision.policy_active_direction_id;
        policy_fanout_budget = cost_decision.policy_fanout_budget;
        policy_materialization_weight =
            cost_decision.policy_materialization_weight;
        policy_class_id = cost_decision.policy_class_id;
        policy_recommendation_kind =
            cost_decision.policy_recommendation_kind;
        cache_seed_eligible = cost_decision.cache_seed_eligible;
        endpoint_headroom_percent = cost_decision.endpoint_headroom_percent;
        empty_lifecycle_eligible = cost_decision.empty_lifecycle_eligible;
        empty_lifecycle_depth = cost_decision.empty_lifecycle_depth;
        empty_lifecycle_batch_size =
            cost_decision.empty_lifecycle_batch_size;
        threshold_input_known = cost_decision.threshold_input_known;
        threshold_input_headroom_percent =
            cost_decision.threshold_input_headroom_percent;
        threshold_input_batch_size = cost_decision.threshold_input_batch_size;
        threshold_input_observed_count =
            cost_decision.threshold_input_observed_count;
        threshold_input_saturated_count =
            cost_decision.threshold_input_saturated_count;
        threshold_input_relaxed_count =
            cost_decision.threshold_input_relaxed_count;
        threshold_input_direction_id =
            cost_decision.threshold_input_direction_id;
        threshold_input_reason_id = cost_decision.threshold_input_reason_id;
        threshold_input_class_id = cost_decision.threshold_input_class_id;
        payload_input_known = cost_decision.payload_input_known;
        payload_input_headroom_percent =
            cost_decision.payload_input_headroom_percent;
        payload_input_scan_runs = cost_decision.payload_input_scan_runs;
        payload_input_replay_runs = cost_decision.payload_input_replay_runs;
        payload_input_seed_runs = cost_decision.payload_input_seed_runs;
        payload_input_replay_percent =
            cost_decision.payload_input_replay_percent;
        payload_input_seed_percent = cost_decision.payload_input_seed_percent;
        payload_input_observed_count =
            cost_decision.payload_input_observed_count;
        payload_input_value_posting_observed_count =
            cost_decision.payload_input_value_posting_observed_count;
        payload_input_matrix_active_percent =
            cost_decision.payload_input_matrix_active_percent;
        payload_input_matrix_filter_percent =
            cost_decision.payload_input_matrix_filter_percent;
        payload_input_matrix_prescan_percent =
            cost_decision.payload_input_matrix_prescan_percent;
        payload_input_matrix_replay_source_percent =
            cost_decision.payload_input_matrix_replay_source_percent;
        payload_input_matrix_run_block_percent =
            cost_decision.payload_input_matrix_run_block_percent;
        payload_input_matrix_regroup_percent =
            cost_decision.payload_input_matrix_regroup_percent;
        payload_input_matrix_compaction_percent =
            cost_decision.payload_input_matrix_compaction_percent;
        payload_input_matrix_dense_percent =
            cost_decision.payload_input_matrix_dense_percent;
        payload_input_matrix_duplicate_percent =
            cost_decision.payload_input_matrix_duplicate_percent;
        payload_input_matrix_compaction_pressure_percent =
            cost_decision.payload_input_matrix_compaction_pressure_percent;
        payload_input_matrix_residency_pressure_percent =
            cost_decision.payload_input_matrix_residency_pressure_percent;
        payload_input_matrix_replay_cutover_percent =
            cost_decision.payload_input_matrix_replay_cutover_percent;
        payload_input_matrix_prefetch_cutover_percent =
            cost_decision.payload_input_matrix_prefetch_cutover_percent;
        payload_input_matrix_payload_coverage_percent =
            cost_decision.payload_input_matrix_payload_coverage_percent;
        payload_input_matrix_source_overlap_percent =
            cost_decision.payload_input_matrix_source_overlap_percent;
        payload_input_matrix_reuse_proximity_percent =
            cost_decision.payload_input_matrix_reuse_proximity_percent;
        outgoing_matrix_cursor_policy =
            cost_decision.outgoing_matrix_cursor_policy;
        incoming_matrix_cursor_policy =
            cost_decision.incoming_matrix_cursor_policy;
        payload_input_reason_id = cost_decision.payload_input_reason_id;
        payload_input_class_id = cost_decision.payload_input_class_id;
        payload_input_value_posting_source_kind =
            cost_decision.payload_input_value_posting_source_kind;
        outgoing_kind = cost_decision.outgoing_kind;
        incoming_kind = cost_decision.incoming_kind;
    }
    else
    {
        policy_outgoing_kind = outgoing_kind;
        policy_incoming_kind = incoming_kind;
    }

build_descriptor:
    descriptor = lappend(descriptor, make_int8_const((int64)source_kind));
    descriptor = lappend(descriptor, makeInteger(adjacency_out ? 1 : 0));
    descriptor = lappend(descriptor, makeInteger(adjacency_in ? 1 : 0));
    descriptor = lappend(descriptor, makeInteger(endpoint_start ? 1 : 0));
    descriptor = lappend(descriptor, makeInteger(endpoint_end ? 1 : 0));
    descriptor = lappend(descriptor, makeInteger(local_edge_state ? 1 : 0));
    descriptor = lappend(descriptor, make_int8_const((int64)edge_label_oid));
    descriptor = lappend(descriptor, make_int8_const((int64)outgoing_kind));
    descriptor = lappend(descriptor, make_int8_const((int64)incoming_kind));
    descriptor = lappend(descriptor, make_int8_const(
        round_vle_source_cost_evidence(source_evidence.reltuples)));
    descriptor = lappend(descriptor, make_int8_const(
        round_vle_source_cost_evidence(source_evidence.start_fanout)));
    descriptor = lappend(descriptor, make_int8_const(
        round_vle_source_cost_evidence(source_evidence.end_fanout)));
    descriptor = lappend(descriptor,
                         makeInteger(source_evidence.relation_tuples_known ?
                                     1 : 0));
    descriptor = lappend(descriptor,
                         makeInteger(source_evidence.start_fanout_known ?
                                     1 : 0));
    descriptor = lappend(descriptor,
                         makeInteger(source_evidence.end_fanout_known ?
                                     1 : 0));
    descriptor = lappend(descriptor, make_int8_const(
        (int64)source_evidence.start_fanout_source_kind));
    descriptor = lappend(descriptor, make_int8_const(
        (int64)source_evidence.end_fanout_source_kind));
    descriptor = lappend(descriptor, make_int8_const(
        (int64)source_evidence.start_value_posting_source_kind));
    descriptor = lappend(descriptor, make_int8_const(
        (int64)source_evidence.end_value_posting_source_kind));
    descriptor = lappend(descriptor, make_text_const(cost_policy));
    descriptor = lappend(descriptor,
                         make_int8_const((int64)policy_outgoing_kind));
    descriptor = lappend(descriptor,
                         make_int8_const((int64)policy_incoming_kind));
    descriptor = lappend(descriptor,
                         make_int8_const((int64)policy_output_requirement));
    descriptor = lappend(descriptor,
                         make_int8_const((int64)policy_consumer_class_id));
    descriptor = lappend(descriptor, make_int8_const(
        (int64)policy_active_direction_id));
    descriptor = lappend(descriptor, make_int8_const(policy_fanout_budget));
    descriptor = lappend(descriptor,
                         make_int8_const(policy_materialization_weight));
    descriptor = lappend(descriptor, make_int8_const((int64)policy_class_id));
    descriptor = lappend(
        descriptor, make_int8_const((int64)policy_recommendation_kind));
    descriptor = lappend(descriptor, makeInteger(cache_seed_eligible ? 1 : 0));
    descriptor = lappend(descriptor,
                         make_int8_const(endpoint_headroom_percent));
    descriptor = lappend(descriptor,
                         makeInteger(empty_lifecycle_eligible ? 1 : 0));
    descriptor = lappend(descriptor,
                         make_int8_const(empty_lifecycle_depth));
    descriptor = lappend(descriptor,
                         make_int8_const(empty_lifecycle_batch_size));
    descriptor = lappend(descriptor,
                         makeInteger(threshold_input_known ? 1 : 0));
    descriptor = lappend(descriptor,
                         make_int8_const(threshold_input_headroom_percent));
    descriptor = lappend(descriptor,
                         make_int8_const(threshold_input_batch_size));
    descriptor = lappend(descriptor,
                         make_int8_const(threshold_input_observed_count));
    descriptor = lappend(descriptor,
                         make_int8_const(threshold_input_saturated_count));
    descriptor = lappend(descriptor,
                         make_int8_const(threshold_input_relaxed_count));
    descriptor = lappend(
        descriptor, make_int8_const((int64)threshold_input_direction_id));
    descriptor = lappend(
        descriptor, make_int8_const((int64)threshold_input_reason_id));
    descriptor = lappend(descriptor,
                         make_int8_const((int64)threshold_input_class_id));
    descriptor = lappend(descriptor, makeInteger(payload_input_known ? 1 : 0));
    descriptor = lappend(descriptor,
                         make_int8_const(payload_input_headroom_percent));
    descriptor = lappend(descriptor,
                         make_int8_const(payload_input_scan_runs));
    descriptor = lappend(descriptor,
                         make_int8_const(payload_input_replay_runs));
    descriptor = lappend(descriptor,
                         make_int8_const(payload_input_seed_runs));
    descriptor = lappend(descriptor,
                         make_int8_const(payload_input_replay_percent));
    descriptor = lappend(descriptor,
                         make_int8_const(payload_input_seed_percent));
    descriptor = lappend(descriptor,
                         make_int8_const(payload_input_observed_count));
    descriptor = lappend(
        descriptor,
        make_int8_const(payload_input_value_posting_observed_count));
    descriptor = lappend(descriptor,
                         make_int8_const(payload_input_matrix_active_percent));
    descriptor = lappend(descriptor,
                         make_int8_const(payload_input_matrix_filter_percent));
    descriptor = lappend(descriptor,
                         make_int8_const(payload_input_matrix_prescan_percent));
    descriptor = lappend(
        descriptor,
        make_int8_const(payload_input_matrix_replay_source_percent));
    descriptor = lappend(descriptor,
                         make_int8_const(payload_input_matrix_run_block_percent));
    descriptor = lappend(descriptor,
                         make_int8_const(payload_input_matrix_regroup_percent));
    descriptor = lappend(
        descriptor,
        make_int8_const(payload_input_matrix_compaction_percent));
    descriptor = lappend(descriptor,
                         make_int8_const(payload_input_matrix_dense_percent));
    descriptor = lappend(
        descriptor, make_int8_const(payload_input_matrix_duplicate_percent));
    descriptor = lappend(
        descriptor,
        make_int8_const(payload_input_matrix_compaction_pressure_percent));
    descriptor = lappend(
        descriptor,
        make_int8_const(payload_input_matrix_residency_pressure_percent));
    descriptor = lappend(
        descriptor,
        make_int8_const(payload_input_matrix_replay_cutover_percent));
    descriptor = lappend(
        descriptor,
        make_int8_const(payload_input_matrix_prefetch_cutover_percent));
    descriptor = lappend(
        descriptor,
        make_int8_const(payload_input_matrix_payload_coverage_percent));
    descriptor = lappend(
        descriptor,
        make_int8_const(payload_input_matrix_source_overlap_percent));
    descriptor = lappend(
        descriptor,
        make_int8_const(payload_input_matrix_reuse_proximity_percent));
    descriptor = lappend(
        descriptor, make_int8_const(outgoing_matrix_cursor_policy.dense_percent));
    descriptor = lappend(
        descriptor,
        make_int8_const(outgoing_matrix_cursor_policy.duplicate_percent));
    descriptor = lappend(
        descriptor,
        make_int8_const(
            outgoing_matrix_cursor_policy.compaction_pressure_percent));
    descriptor = lappend(
        descriptor,
        make_int8_const(
            outgoing_matrix_cursor_policy.residency_pressure_percent));
    descriptor = lappend(
        descriptor,
        make_int8_const(outgoing_matrix_cursor_policy.replay_percent));
    descriptor = lappend(
        descriptor,
        make_int8_const(outgoing_matrix_cursor_policy.prefetch_percent));
    descriptor = lappend(
        descriptor,
        make_int8_const(outgoing_matrix_cursor_policy.payload_coverage_percent));
    descriptor = lappend(
        descriptor,
        make_int8_const(outgoing_matrix_cursor_policy.source_overlap_percent));
    descriptor = lappend(
        descriptor,
        make_int8_const(outgoing_matrix_cursor_policy.reuse_proximity_percent));
    descriptor = lappend(
        descriptor, make_int8_const(incoming_matrix_cursor_policy.dense_percent));
    descriptor = lappend(
        descriptor,
        make_int8_const(incoming_matrix_cursor_policy.duplicate_percent));
    descriptor = lappend(
        descriptor,
        make_int8_const(
            incoming_matrix_cursor_policy.compaction_pressure_percent));
    descriptor = lappend(
        descriptor,
        make_int8_const(
            incoming_matrix_cursor_policy.residency_pressure_percent));
    descriptor = lappend(
        descriptor,
        make_int8_const(incoming_matrix_cursor_policy.replay_percent));
    descriptor = lappend(
        descriptor,
        make_int8_const(incoming_matrix_cursor_policy.prefetch_percent));
    descriptor = lappend(
        descriptor,
        make_int8_const(incoming_matrix_cursor_policy.payload_coverage_percent));
    descriptor = lappend(
        descriptor,
        make_int8_const(incoming_matrix_cursor_policy.source_overlap_percent));
    descriptor = lappend(
        descriptor,
        make_int8_const(incoming_matrix_cursor_policy.reuse_proximity_percent));
    descriptor = lappend(
        descriptor, make_int8_const((int64)payload_input_reason_id));
    descriptor = lappend(descriptor,
                         make_int8_const((int64)payload_input_class_id));
    descriptor = lappend(descriptor,
                         make_int8_const(
                             (int64)payload_input_value_posting_source_kind));
    descriptor = lappend(descriptor,
                         makeInteger(terminal_property_source_known ? 1 : 0));
    descriptor = lappend(descriptor, make_int8_const(terminal_label_id));
    descriptor = lappend(descriptor,
                         make_int8_const((int64)terminal_property_index_oid));
    descriptor = lappend(descriptor,
                         make_int8_const((int64)terminal_property_filter_id));
    descriptor = lappend(descriptor, make_text_const(terminal_property_label));
    descriptor = lappend(descriptor, make_text_const(terminal_property_source));
    descriptor = lappend(descriptor,
                         make_text_const(terminal_property_provider));
    descriptor = lappend(descriptor, make_text_const(terminal_property_type));
    descriptor = lappend(descriptor,
                         make_int8_const(terminal_property_match_count));
    descriptor = lappend(descriptor,
                         makeInteger(composite_source_known ? 1 : 0));
    descriptor = lappend(
        descriptor, make_int8_const((int64)composite_source_reason_kind));
    descriptor = lappend(descriptor,
                         make_int8_const(composite_source_property_tuples));
    descriptor = lappend(descriptor,
                         make_int8_const(composite_source_candidate_fanout));
    descriptor = lappend(descriptor,
                         make_int8_const(composite_source_fanout));
    descriptor = lappend(
        descriptor, make_int8_const(
            graph_property_selectivity_ppm(composite_source_selectivity)));
    descriptor = lappend(descriptor, make_int8_const(
        (int64)composite_source_selectivity_source_kind));
    descriptor = lappend(
        descriptor, make_int8_const((int64)composite_source_planned_kind));
    descriptor = lappend(
        descriptor, makeInteger(terminal_property_predicate.known ? 1 : 0));
    descriptor = lappend(
        descriptor, make_text_const(terminal_property_predicate.key));
    descriptor = lappend(
        descriptor, makeInteger(terminal_property_predicate.isnull ? 1 : 0));
    descriptor = lappend(
        descriptor,
        terminal_property_predicate.value != NULL ?
        copyObject(terminal_property_predicate.value) :
        makeNullConst(AGTYPEOID, -1, InvalidOid));
    descriptor = lappend(descriptor,
                         make_int8_const(
                             (int64)terminal_property_value_kind_id));
    descriptor = lappend(
        descriptor, makeInteger(terminal_property_prefilter_eligible ? 1 : 0));
    descriptor = lappend(
        descriptor, make_int8_const(terminal_property_prefetch_threshold));

    if (graph_name != NULL)
        pfree(graph_name);
    if (label_name != NULL)
        pfree(label_name);
    if (terminal_key != NULL)
        pfree(terminal_key);
    if (terminal_property_predicate.key != NULL)
        pfree(terminal_property_predicate.key);
    if (terminal_property_label != NULL)
        pfree(terminal_property_label);
    if (terminal_property_source != NULL)
        pfree(terminal_property_source);
    if (terminal_property_provider != NULL)
        pfree(terminal_property_provider);
    if (terminal_property_type != NULL)
        pfree(terminal_property_type);
    if (cost_policy != NULL)
        pfree(cost_policy);

    return descriptor;
}

static int64 choose_vle_stream_terminal_candidate_fanout(
    const VLESourceFanoutEvidence *source_evidence, cypher_rel_dir direction)
{
    bool start_known;
    bool end_known;
    int64 start_fanout;
    int64 end_fanout;

    if (source_evidence == NULL)
        return 0;

    start_known = source_evidence->start_fanout_known;
    end_known = source_evidence->end_fanout_known;
    start_fanout =
        round_vle_source_cost_evidence(source_evidence->start_fanout);
    end_fanout =
        round_vle_source_cost_evidence(source_evidence->end_fanout);

    if (direction == CYPHER_REL_DIR_RIGHT)
        return start_known ? start_fanout : 0;
    if (direction == CYPHER_REL_DIR_LEFT)
        return end_known ? end_fanout : 0;

    if (start_known && end_known)
        return Min(start_fanout, end_fanout);
    if (start_known)
        return start_fanout;
    if (end_known)
        return end_fanout;
    return 0;
}

static int64 clamp_vle_stream_composite_fanout(int64 candidate_fanout,
                                               double selectivity)
{
    int64 composite_fanout;

    if (candidate_fanout <= 0)
        return 0;

    composite_fanout = round_vle_source_cost_evidence(
        (double)candidate_fanout * selectivity);
    if (composite_fanout < 1)
        composite_fanout = 1;
    if (composite_fanout > candidate_fanout)
        composite_fanout = candidate_fanout;

    return composite_fanout;
}

static GraphPropertySourceSelectivity estimate_graph_property_source_selectivity(
    Oid property_index_oid, double fallback_selectivity,
    Const *property_value)
{
    GraphPropertySourceSelectivity estimate;
    double reltuples;
    double mcv_selectivity;
    double distinct_values;

    estimate.selectivity = fallback_selectivity;
    estimate.source_kind = AGE_GRAPH_PROPERTY_SELECTIVITY_FALLBACK;

    if (!OidIsValid(property_index_oid) || fallback_selectivity <= 0)
        return estimate;

    if (estimate_graph_property_index_mcv_selectivity(
            property_index_oid, property_value, &mcv_selectivity))
    {
        if (mcv_selectivity <= fallback_selectivity)
        {
            estimate.selectivity = mcv_selectivity;
            estimate.source_kind = AGE_GRAPH_PROPERTY_SELECTIVITY_TYPED_MCV;
        }
        else
        {
            estimate.source_kind =
                AGE_GRAPH_PROPERTY_SELECTIVITY_FALLBACK_MCV_CEILING;
        }
        return estimate;
    }

    reltuples = get_vle_relation_estimated_tuples(property_index_oid);
    if (!estimate_graph_property_index_distinct_values(
            property_index_oid, reltuples, &distinct_values))
        return estimate;

    if (distinct_values > 1.0)
    {
        double statistics_selectivity = 1.0 / distinct_values;

        if (statistics_selectivity < fallback_selectivity)
        {
            estimate.selectivity = statistics_selectivity;
            estimate.source_kind =
                AGE_GRAPH_PROPERTY_SELECTIVITY_TYPED_DISTINCT;
        }
        else
        {
            estimate.source_kind =
                AGE_GRAPH_PROPERTY_SELECTIVITY_FALLBACK_CEILING;
        }
    }

    return estimate;
}

static bool estimate_graph_property_index_mcv_selectivity(Oid property_index_oid,
                                                          Const *property_value,
                                                          double *selectivity)
{
    HeapTuple stat_tuple;
    AttStatsSlot sslot;
    Oid op_oid;
    Oid op_func_oid;
    FmgrInfo eqproc;
    Oid collation;
    int i;

    Assert(selectivity != NULL);
    *selectivity = 0.0;

    if (!OidIsValid(property_index_oid) || property_value == NULL ||
        property_value->constisnull)
        return false;

    stat_tuple = SearchSysCache3(STATRELATTINH,
                                 ObjectIdGetDatum(property_index_oid),
                                 Int16GetDatum(1),
                                 BoolGetDatum(false));
    if (!HeapTupleIsValid(stat_tuple))
        return false;

    if (!get_attstatsslot(&sslot, stat_tuple, STATISTIC_KIND_MCV, InvalidOid,
                          ATTSTATSSLOT_VALUES | ATTSTATSSLOT_NUMBERS))
    {
        ReleaseSysCache(stat_tuple);
        return false;
    }

    if (sslot.valuetype != property_value->consttype ||
        sslot.nvalues <= 0 || sslot.nnumbers <= 0)
    {
        free_attstatsslot(&sslot);
        ReleaseSysCache(stat_tuple);
        return false;
    }

    op_oid = OpernameGetOprid(list_make1(makeString("=")),
                              property_value->consttype,
                              property_value->consttype);
    if (!OidIsValid(op_oid))
    {
        free_attstatsslot(&sslot);
        ReleaseSysCache(stat_tuple);
        return false;
    }
    op_func_oid = get_opcode(op_oid);
    if (!OidIsValid(op_func_oid))
    {
        free_attstatsslot(&sslot);
        ReleaseSysCache(stat_tuple);
        return false;
    }

    fmgr_info(op_func_oid, &eqproc);
    collation = OidIsValid(sslot.stacoll) ? sslot.stacoll :
        property_value->constcollid;
    for (i = 0; i < sslot.nvalues && i < sslot.nnumbers; i++)
    {
        if (DatumGetBool(FunctionCall2Coll(&eqproc, collation,
                                           property_value->constvalue,
                                           sslot.values[i])))
        {
            *selectivity = sslot.numbers[i];
            free_attstatsslot(&sslot);
            ReleaseSysCache(stat_tuple);
            return *selectivity > 0;
        }
    }

    free_attstatsslot(&sslot);
    ReleaseSysCache(stat_tuple);
    return false;
}

static bool estimate_graph_property_index_distinct_values(Oid property_index_oid,
                                                          double reltuples,
                                                          double *distinct_values)
{
    HeapTuple stat_tuple;
    Form_pg_statistic stats;
    double distinct;

    Assert(distinct_values != NULL);
    *distinct_values = 0.0;

    if (!OidIsValid(property_index_oid) || reltuples <= 0)
        return false;

    stat_tuple = SearchSysCache3(STATRELATTINH,
                                 ObjectIdGetDatum(property_index_oid),
                                 Int16GetDatum(1),
                                 BoolGetDatum(false));
    if (!HeapTupleIsValid(stat_tuple))
        return false;

    stats = (Form_pg_statistic) GETSTRUCT(stat_tuple);
    if (stats->stadistinct > 0)
        distinct = stats->stadistinct;
    else if (stats->stadistinct < 0)
        distinct = -stats->stadistinct * reltuples;
    else
        distinct = 0.0;

    ReleaseSysCache(stat_tuple);

    if (distinct <= 0)
        return false;

    distinct = Max(distinct, 1.0);
    distinct = Min(distinct, reltuples);
    *distinct_values = distinct;
    return true;
}

static int64 graph_property_selectivity_ppm(double selectivity)
{
    if (selectivity <= 0)
        return 0;
    if (selectivity >= 1.0)
        return 1000000;

    return (int64)(selectivity * 1000000.0 + 0.5);
}

static void get_vle_stream_edge_source_indexes(
    const char *graph_name, const char *label_name,
    Oid *edge_label_oid,
    Oid *age_adjacency_out_index_oid,
    Oid *age_adjacency_in_index_oid,
    bool *adjacency_out, bool *adjacency_in,
    bool *endpoint_start, bool *endpoint_end)
{
    static Oid age_adjacency_am_oid = InvalidOid;
    Oid graph_oid;
    label_cache_data *label_cache;
    Relation edge_rel;
    List *index_list;
    ListCell *lc;
    bool anonymous_edge_label;

    Assert(edge_label_oid != NULL);
    Assert(age_adjacency_out_index_oid != NULL);
    Assert(age_adjacency_in_index_oid != NULL);
    Assert(adjacency_out != NULL);
    Assert(adjacency_in != NULL);
    Assert(endpoint_start != NULL);
    Assert(endpoint_end != NULL);

    *edge_label_oid = InvalidOid;
    *age_adjacency_out_index_oid = InvalidOid;
    *age_adjacency_in_index_oid = InvalidOid;
    *adjacency_out = false;
    *adjacency_in = false;
    *endpoint_start = false;
    *endpoint_end = false;

    if (graph_name == NULL || label_name == NULL)
        return;

    graph_oid = get_graph_oid(graph_name);
    if (!OidIsValid(graph_oid))
        return;

    anonymous_edge_label = label_name[0] == '\0';
    if (anonymous_edge_label)
        label_name = AG_DEFAULT_LABEL_EDGE;

    label_cache = search_label_name_graph_cache_cached(label_name, graph_oid);
    if (label_cache == NULL || !OidIsValid(label_cache->relation))
        return;

    *edge_label_oid = label_cache->relation;
    edge_rel = relation_open(label_cache->relation, AccessShareLock);
    *endpoint_start = OidIsValid(find_usable_btree_index_for_attr(
        edge_rel, Anum_ag_label_edge_table_start_id));
    *endpoint_end = OidIsValid(find_usable_btree_index_for_attr(
        edge_rel, Anum_ag_label_edge_table_end_id));

    if (!OidIsValid(age_adjacency_am_oid))
        age_adjacency_am_oid =
            GetSysCacheOid1(AMNAME, Anum_pg_am_oid,
                            CStringGetDatum("age_adjacency"));

    if (OidIsValid(age_adjacency_am_oid))
    {
        index_list = RelationGetIndexList(edge_rel);
        foreach(lc, index_list)
        {
            Oid index_oid = lfirst_oid(lc);
            Relation index_rel;

            index_rel = index_open(index_oid, AccessShareLock);
            if (index_rel->rd_rel->relam == age_adjacency_am_oid &&
                index_rel->rd_index != NULL &&
                index_rel->rd_index->indisvalid &&
                index_rel->rd_index->indisready)
            {
                if (vle_stream_age_adjacency_index_matches(index_rel, true))
                {
                    *age_adjacency_out_index_oid = index_oid;
                    *adjacency_out = true;
                }
                if (vle_stream_age_adjacency_index_matches(index_rel, false))
                {
                    *age_adjacency_in_index_oid = index_oid;
                    *adjacency_in = true;
                }
            }
            index_close(index_rel, AccessShareLock);
            if (*adjacency_out && *adjacency_in)
                break;
        }
        list_free(index_list);
    }

    relation_close(edge_rel, AccessShareLock);

    if (anonymous_edge_label && !*adjacency_out && !*adjacency_in)
    {
        *endpoint_start = false;
        *endpoint_end = false;
    }
}

static void apply_vle_stream_directory_fanout_evidence(
    VLESourceFanoutEvidence *source_evidence, List *func_args,
    Oid age_adjacency_out_index_oid, Oid age_adjacency_in_index_oid,
    int32 terminal_label_id, PlannerInfo *root)
{
    graphid start_id;
    graphid end_id;
    bool start_null = false;
    bool end_null = false;

    Assert(source_evidence != NULL);

    if (func_args == NIL)
        return;

    if (OidIsValid(age_adjacency_out_index_oid) &&
        list_length(func_args) > AGE_VLE_STREAM_ARG_START &&
        find_age_vle_stream_endpoint_graphid_const(
            root,
            list_nth(func_args, AGE_VLE_STREAM_ARG_START),
            &start_id, &start_null) &&
        !start_null)
    {
        int64 run_postings;
        int64 terminal_postings;
        VLESourceValuePostingKind value_posting_source_kind;

        if (age_adjacency_estimate_terminal_label_postings(
                age_adjacency_out_index_oid, start_id, 0,
                &run_postings, NULL, NULL, NULL, NULL))
        {
            source_evidence->start_fanout = (double)run_postings;
            source_evidence->start_fanout_known = true;
            source_evidence->start_fanout_source_kind =
                AGE_VLE_STREAM_FANOUT_SOURCE_DIRECTORY;
        }
        if (label_id_is_valid(terminal_label_id) &&
            age_adjacency_estimate_terminal_label_postings(
                age_adjacency_out_index_oid, start_id, terminal_label_id,
                &run_postings, &terminal_postings, NULL,
                &value_posting_source_kind, NULL))
        {
            source_evidence->start_fanout = (double)terminal_postings;
            source_evidence->start_fanout_known = true;
            source_evidence->start_fanout_source_kind =
                AGE_VLE_STREAM_FANOUT_SOURCE_DIRECTORY_LABEL;
            source_evidence->start_value_posting_source_kind =
                value_posting_source_kind;
        }
    }

    if (OidIsValid(age_adjacency_in_index_oid) &&
        list_length(func_args) > AGE_VLE_STREAM_ARG_END &&
        find_age_vle_stream_endpoint_graphid_const(
            root,
            list_nth(func_args, AGE_VLE_STREAM_ARG_END),
            &end_id, &end_null) &&
        !end_null)
    {
        int64 run_postings;
        int64 terminal_postings;
        VLESourceValuePostingKind value_posting_source_kind;

        if (age_adjacency_estimate_terminal_label_postings(
                age_adjacency_in_index_oid, end_id, 0,
                &run_postings, NULL, NULL, NULL, NULL))
        {
            source_evidence->end_fanout = (double)run_postings;
            source_evidence->end_fanout_known = true;
            source_evidence->end_fanout_source_kind =
                AGE_VLE_STREAM_FANOUT_SOURCE_DIRECTORY;
        }
        if (label_id_is_valid(terminal_label_id) &&
            age_adjacency_estimate_terminal_label_postings(
                age_adjacency_in_index_oid, end_id, terminal_label_id,
                &run_postings, &terminal_postings, NULL,
                &value_posting_source_kind, NULL))
        {
            source_evidence->end_fanout = (double)terminal_postings;
            source_evidence->end_fanout_known = true;
            source_evidence->end_fanout_source_kind =
                AGE_VLE_STREAM_FANOUT_SOURCE_DIRECTORY_LABEL;
            source_evidence->end_value_posting_source_kind =
                value_posting_source_kind;
        }
    }
}

static bool find_age_vle_stream_endpoint_graphid_const(
    PlannerInfo *root, Node *node, graphid *value, bool *isnull)
{
    RelOptInfo *endpoint_rel;
    Relids relids;
    int rti;
    ListCell *lc;

    Assert(value != NULL);
    Assert(isnull != NULL);

    if (get_age_vle_stream_vertex_id_const(node, value, isnull))
        return true;

    if (root == NULL || node == NULL)
        return false;

    relids = pull_varnos(root, node);
    if (bms_is_empty(relids))
    {
        bms_free(relids);
        return false;
    }
    if (bms_membership(relids) != BMS_SINGLETON)
    {
        bms_free(relids);
        return false;
    }

    rti = bms_singleton_member(relids);
    bms_free(relids);
    if (rti <= 0 ||
        rti >= root->simple_rel_array_size ||
        root->simple_rel_array[rti] == NULL)
    {
        return false;
    }

    endpoint_rel = root->simple_rel_array[rti];
    foreach(lc, endpoint_rel->baserestrictinfo)
    {
        RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc);
        OpExpr *op;
        Node *left;
        Node *right;

        if (!IsA(rinfo->clause, OpExpr))
            continue;

        op = castNode(OpExpr, rinfo->clause);
        if (list_length(op->args) != 2)
            continue;

        left = linitial(op->args);
        right = lsecond(op->args);

        if (IsA(left, Var) && IsA(right, Const))
        {
            Var *var = castNode(Var, left);
            Const *con = castNode(Const, right);

            if (var->varno == rti &&
                var->varattno == Anum_ag_label_vertex_table_id &&
                var->vartype == GRAPHIDOID &&
                con->consttype == GRAPHIDOID &&
                !con->constisnull)
            {
                *value = (graphid)DatumGetInt64(con->constvalue);
                *isnull = false;
                return true;
            }
        }
        else if (IsA(left, Const) && IsA(right, Var))
        {
            Const *con = castNode(Const, left);
            Var *var = castNode(Var, right);

            if (var->varno == rti &&
                var->varattno == Anum_ag_label_vertex_table_id &&
                var->vartype == GRAPHIDOID &&
                con->consttype == GRAPHIDOID &&
                !con->constisnull)
            {
                *value = (graphid)DatumGetInt64(con->constvalue);
                *isnull = false;
                return true;
            }
        }
    }

    return false;
}

static bool vle_stream_age_adjacency_index_matches(Relation index_rel,
                                                   bool outgoing)
{
    int2vector *indkey;

    if (index_rel->rd_index->indnkeyatts != 3 ||
        index_rel->rd_index->indnatts != 3)
    {
        return false;
    }

    indkey = &index_rel->rd_index->indkey;
    if (outgoing)
    {
        return indkey->values[0] == Anum_ag_label_edge_table_start_id &&
               indkey->values[1] == Anum_ag_label_edge_table_id &&
               indkey->values[2] == Anum_ag_label_edge_table_end_id;
    }

    return indkey->values[0] == Anum_ag_label_edge_table_end_id &&
           indkey->values[1] == Anum_ag_label_edge_table_id &&
           indkey->values[2] == Anum_ag_label_edge_table_start_id;
}

static AgeVLEStreamDirectedSourceKind
age_vle_stream_directed_source_from_traversal(
    VLETraversalSourceKind source_kind)
{
    switch (source_kind)
    {
        case VLE_TRAVERSAL_SOURCE_NONE:
            return AGE_VLE_STREAM_DIRECTED_SOURCE_NONE;
        case VLE_TRAVERSAL_SOURCE_AGE_ADJACENCY:
            return AGE_VLE_STREAM_DIRECTED_SOURCE_AGE_ADJACENCY;
        case VLE_TRAVERSAL_SOURCE_ENDPOINT_BTREE:
            return AGE_VLE_STREAM_DIRECTED_SOURCE_ENDPOINT_BTREE;
    }

    return AGE_VLE_STREAM_DIRECTED_SOURCE_NONE;
}

static bool get_age_vle_stream_integer_const(Node *node, int64 *value,
                                             bool *isnull)
{
    Const *const_arg;
    agtype *agt;
    agtype_value agtv;
    bool needs_free = false;
    bool found;

    Assert(value != NULL);
    Assert(isnull != NULL);

    *value = 0;
    *isnull = false;

    if (node == NULL || !IsA(node, Const))
        return false;

    const_arg = (Const *)node;
    if (const_arg->constisnull)
    {
        *isnull = true;
        return true;
    }
    if (const_arg->consttype != AGTYPEOID)
        return false;

    agt = DATUM_GET_AGTYPE_P(const_arg->constvalue);
    if (!AGTYPE_CONTAINER_IS_SCALAR(&agt->root))
        return false;

    found = get_ith_agtype_value_from_container_no_copy(&agt->root, 0, &agtv,
                                                        &needs_free);
    if (!found || agtv.type == AGTV_NULL)
    {
        if (needs_free)
            pfree_agtype_value_content(&agtv);
        *isnull = true;
        return true;
    }
    if (agtv.type != AGTV_INTEGER)
    {
        if (needs_free)
            pfree_agtype_value_content(&agtv);
        return false;
    }

    *value = agtv.val.int_value;
    if (needs_free)
        pfree_agtype_value_content(&agtv);

    return true;
}

static bool get_age_vle_stream_vertex_id_const(Node *node, graphid *value,
                                               bool *isnull)
{
    Const *const_arg;
    agtype *agt;
    agtype_value agtv;
    agtype_value *id_value;
    bool needs_free = false;
    bool found;

    Assert(value != NULL);
    Assert(isnull != NULL);

    *value = 0;
    *isnull = false;

    if (node == NULL || !IsA(node, Const))
        return false;

    const_arg = (Const *)node;
    if (const_arg->constisnull)
    {
        *isnull = true;
        return true;
    }
    if (const_arg->consttype != AGTYPEOID)
        return false;

    agt = DATUM_GET_AGTYPE_P(const_arg->constvalue);
    if (!AGTYPE_CONTAINER_IS_SCALAR(&agt->root))
        return false;

    found = get_ith_agtype_value_from_container_no_copy(&agt->root, 0, &agtv,
                                                        &needs_free);
    if (!found || agtv.type == AGTV_NULL)
    {
        if (needs_free)
            pfree_agtype_value_content(&agtv);
        *isnull = true;
        return true;
    }
    if (agtv.type == AGTV_INTEGER)
    {
        *value = (graphid)agtv.val.int_value;
        if (needs_free)
            pfree_agtype_value_content(&agtv);
        return true;
    }
    if (agtv.type != AGTV_VERTEX)
    {
        if (needs_free)
            pfree_agtype_value_content(&agtv);
        return false;
    }

    id_value = AGTYPE_VERTEX_GET_ID(&agtv);
    if (id_value == NULL || id_value->type != AGTV_INTEGER)
    {
        if (needs_free)
            pfree_agtype_value_content(&agtv);
        return false;
    }

    *value = (graphid)id_value->val.int_value;
    if (needs_free)
        pfree_agtype_value_content(&agtv);

    return true;
}

static bool get_age_vle_stream_string_const(Node *node, char **value,
                                            bool *isnull)
{
    Const *const_arg;
    agtype *agt;
    agtype_value agtv;
    bool needs_free = false;
    bool found;

    Assert(value != NULL);
    Assert(isnull != NULL);

    *value = NULL;
    *isnull = false;

    if (node == NULL || !IsA(node, Const))
        return false;

    const_arg = (Const *)node;
    if (const_arg->constisnull)
    {
        *isnull = true;
        return true;
    }
    if (const_arg->consttype != AGTYPEOID)
        return false;

    agt = DATUM_GET_AGTYPE_P(const_arg->constvalue);
    if (!AGTYPE_CONTAINER_IS_SCALAR(&agt->root))
        return false;

    found = get_ith_agtype_value_from_container_no_copy(&agt->root, 0, &agtv,
                                                        &needs_free);
    if (!found || agtv.type == AGTV_NULL)
    {
        if (needs_free)
            pfree_agtype_value_content(&agtv);
        *isnull = true;
        return true;
    }
    if (agtv.type != AGTV_STRING)
    {
        if (needs_free)
            pfree_agtype_value_content(&agtv);
        return false;
    }

    *value = pnstrdup(agtv.val.string.val, agtv.val.string.len);
    if (needs_free)
        pfree_agtype_value_content(&agtv);

    return true;
}

static Const *make_int8_const(int64 value)
{
    return makeConst(INT8OID, -1, InvalidOid, sizeof(int64),
                     Int64GetDatum(value), false, FLOAT8PASSBYVAL);
}

static Const *make_text_const(const char *value)
{
    if (value == NULL)
    {
        return makeConst(TEXTOID, -1, InvalidOid, -1, (Datum)0, true,
                         false);
    }

    return makeConst(TEXTOID, -1, DEFAULT_COLLATION_OID, -1,
                     CStringGetTextDatum(value), false, false);
}

static Const *make_agtype_const(agtype *value)
{
    if (value == NULL)
    {
        return makeConst(AGTYPEOID, -1, InvalidOid, -1, (Datum)0, true,
                         false);
    }

    return makeConst(AGTYPEOID, -1, InvalidOid, -1, AGTYPE_P_GET_DATUM(value),
                     false, false);
}

static void adjust_graph_expansion_join_rows(PlannerInfo *root,
                                             RelOptInfo *joinrel,
                                             JoinType jointype)
{
    ListCell *lc;

    if (joinrel == NULL ||
        joinrel->pathlist == NIL)
    {
        return;
    }

    foreach(lc, joinrel->pathlist)
    {
        Path *path = lfirst(lc);
        JoinPath *joinpath;
        Path *outer_path;
        Path *inner_path;
        double child_rows;
        double adjusted_rows;
        double row_ratio;
        Cost old_startup_cost;
        Cost old_total_cost;
        Cost adjusted_total_cost;
        double candidate_credit;
        bool outer_has_expansion;
        bool inner_has_expansion;
        AgeGraphJoinCandidateTable *join_candidate_table;
        AgeGraphJoinCandidate *selected_join_candidate;
        AgeGraphJoinCandidate *next_best_candidate;
        AgeGraphJoinPathEvidence join_evidence;
        AgeGraphJoinPathEvidence outer_evidence;
        AgeGraphJoinPathEvidence inner_evidence;

        if (!IsA(path, NestPath))
            continue;

        joinpath = (JoinPath *)path;
        outer_path = joinpath->outerjoinpath;
        inner_path = joinpath->innerjoinpath;
        age_graph_join_init_path_evidence(&outer_evidence);
        age_graph_join_init_path_evidence(&inner_evidence);

        outer_has_expansion = path_contains_graph_expansion(outer_path);
        inner_has_expansion = path_contains_graph_expansion(inner_path);
        if (inner_path != NULL &&
            inner_path->parent != NULL &&
            path_get_graph_expansion_join_evidence(
                outer_path, inner_path->parent->relids, &outer_evidence))
        {
            outer_evidence.bound =
                age_graph_join_order_property_kind_is_bound(
                    outer_evidence.order_property_kind);
        }
        if (outer_path != NULL &&
            outer_path->parent != NULL &&
            path_get_graph_expansion_join_evidence(
                inner_path, outer_path->parent->relids, &inner_evidence))
        {
            inner_evidence.bound =
                age_graph_join_order_property_kind_is_bound(
                    inner_evidence.order_property_kind);
        }

        if (outer_path == NULL ||
            inner_path == NULL ||
            (!outer_has_expansion && !inner_has_expansion))
        {
            continue;
        }

        /*
         * Do not mutate shared joinrel or ParamPathInfo estimates here.  This
         * only tightens the opt-in NestPath that contains a graph expansion
         * CustomPath after core join path creation has finished.
         */
        child_rows = outer_path->rows * inner_path->rows;
        adjusted_rows = clamp_row_est(Max(child_rows, 1.0));
        join_candidate_table = make_graph_expansion_join_candidate_table(
            outer_path, inner_path, &outer_evidence, &inner_evidence);
        selected_join_candidate = age_graph_join_table_select_cheapest(
            join_candidate_table);

        if (selected_join_candidate != NULL)
        {
            adjusted_rows = Min(adjusted_rows,
                                clamp_row_est(Max(
                                    selected_join_candidate->connector.rows,
                                    1.0)));
        }
        else if (outer_has_expansion &&
                 !inner_has_expansion &&
            outer_path->parent != NULL &&
            path_required_outer_is_subset(inner_path, outer_path->parent->relids))
        {
            adjusted_rows = Min(adjusted_rows,
                                clamp_row_est(Max(outer_path->rows, 1.0)));
        }
        else if (inner_has_expansion &&
                 !outer_has_expansion &&
                 inner_path->parent != NULL &&
                 path_required_outer_is_subset(outer_path,
                                               inner_path->parent->relids))
        {
            adjusted_rows = Min(adjusted_rows,
                                clamp_row_est(Max(inner_path->rows, 1.0)));
        }

        if (adjusted_rows >= path->rows)
            continue;

        old_startup_cost = path->startup_cost;
        old_total_cost = path->total_cost;
        row_ratio = adjusted_rows / Max(path->rows, 1.0);
        row_ratio = Max(row_ratio, 0.01);
        candidate_credit = age_graph_join_path_evidence_credit(
            &outer_evidence, &inner_evidence);
        adjusted_total_cost =
            old_startup_cost + (old_total_cost - old_startup_cost) *
            row_ratio * candidate_credit;
        if (adjusted_total_cost < old_startup_cost)
            adjusted_total_cost = old_startup_cost;

        ereport(DEBUG2,
                (errmsg_internal("AGE graph expansion join rows adjusted: "
                                 "jointype=%d joinrelids=%s old_rows=%.0f "
                                 "new_rows=%.0f outer_rows=%.0f "
                                 "inner_rows=%.0f old_total=%.2f "
                                 "new_total=%.2f candidate_credit=%.3f "
                                 "selected_component=%s "
                                 "selected_connector=%s selected_order=%s "
                                 "selected_source=%s selected_rows=%.0f "
                                 "selected_required=%s selected_provided=%s "
                                 "outer_connector=%s outer_order=%s "
                                 "outer_source=%s inner_connector=%s "
                                 "inner_order=%s inner_source=%s "
                                 "join_candidates=%d outer_candidates=%ld "
                                 "inner_candidates=%ld",
                                 (int)jointype,
                                 bmsToString(joinrel->relids),
                                 path->rows,
                                 adjusted_rows,
                                 outer_path->rows,
                                 inner_path->rows,
                                 old_total_cost,
                                 adjusted_total_cost,
                                 candidate_credit,
                                 selected_join_candidate != NULL ?
                                 selected_join_candidate->component.display_name :
                                 "none",
                                 selected_join_candidate != NULL ?
                                 age_graph_join_connector_name(
                                     selected_join_candidate->connector.
                                     kind_id) :
                                 "none",
                                 selected_join_candidate != NULL ?
                                 age_graph_join_order_property_name(
                                     selected_join_candidate->connector.
                                     order_property_kind) :
                                 "none",
                                 selected_join_candidate != NULL ?
                                 age_graph_join_source_evidence_kind_name(
                                     selected_join_candidate->connector.
                                     source_evidence_kind) :
                                 "none",
                                 selected_join_candidate != NULL ?
                                 selected_join_candidate->connector.rows : 0.0,
                                 selected_join_candidate != NULL ?
                                 bmsToString(selected_join_candidate->
                                             component.required_outer) :
                                 "none",
                                 selected_join_candidate != NULL ?
                                 bmsToString(selected_join_candidate->
                                             component.provided_relids) :
                                 "none",
                                 outer_evidence.connector_kind !=
                                 AGE_GRAPH_JOIN_CONNECTOR_UNKNOWN ?
                                 age_graph_join_connector_name(
                                     outer_evidence.connector_kind) : "none",
                                 outer_evidence.order_property_kind !=
                                 AGE_GRAPH_JOIN_ORDER_UNKNOWN ?
                                 age_graph_join_order_property_name(
                                     outer_evidence.order_property_kind) :
                                 "none",
                                 outer_evidence.source_evidence_kind !=
                                 AGE_GRAPH_JOIN_SOURCE_EVIDENCE_UNKNOWN ?
                                 age_graph_join_source_evidence_kind_name(
                                     outer_evidence.source_evidence_kind) :
                                 "none",
                                 inner_evidence.connector_kind !=
                                 AGE_GRAPH_JOIN_CONNECTOR_UNKNOWN ?
                                 age_graph_join_connector_name(
                                     inner_evidence.connector_kind) : "none",
                                 inner_evidence.order_property_kind !=
                                 AGE_GRAPH_JOIN_ORDER_UNKNOWN ?
                                 age_graph_join_order_property_name(
                                     inner_evidence.order_property_kind) :
                                 "none",
                                 inner_evidence.source_evidence_kind !=
                                 AGE_GRAPH_JOIN_SOURCE_EVIDENCE_UNKNOWN ?
                                 age_graph_join_source_evidence_kind_name(
                                     inner_evidence.source_evidence_kind) :
                                 "none",
                                 age_graph_join_table_candidate_count(
                                     join_candidate_table),
                                 (long)outer_evidence.candidate_count,
                                 (long)inner_evidence.candidate_count)));

        path->rows = adjusted_rows;
        path->total_cost = adjusted_total_cost;
        if (selected_join_candidate != NULL)
        {
            age_graph_join_init_path_evidence(&join_evidence);
            join_evidence.component_kind =
                selected_join_candidate->component.family_kind;
            join_evidence.connector_kind =
                selected_join_candidate->connector.kind_id;
            join_evidence.order_property_kind =
                selected_join_candidate->connector.order_property_kind;
            join_evidence.source_evidence_kind =
                selected_join_candidate->connector.source_evidence_kind;
            join_evidence.solved_relids = bms_copy(
                selected_join_candidate->connector.solved_relids);
            join_evidence.required_outer = PATH_REQ_OUTER(path);
            join_evidence.provided_relids = joinrel->relids;
            join_evidence.candidate_count =
                age_graph_join_table_candidate_count(join_candidate_table);
            join_evidence.declared_cover_count =
                join_candidate_table->declared_entry_count;
            join_evidence.cover_match_kind =
                join_candidate_table->cover_match_kind;
            join_evidence.selected_total_cost = path->total_cost;
            next_best_candidate = age_graph_join_table_select_next_best(
                join_candidate_table, selected_join_candidate);
            join_evidence.next_total_cost = next_best_candidate != NULL ?
                next_best_candidate->connector.total_cost : 0;
            if (joinrel->reltarget != NULL)
                join_evidence.output_width = joinrel->reltarget->width;
            join_evidence.parallel_safe =
                path->parallel_safe &&
                selected_join_candidate->component.parallel_safe;
            join_evidence.parallel_aware =
                join_evidence.parallel_safe &&
                selected_join_candidate->component.parallel_aware;
            join_evidence.parallel_workers = join_evidence.parallel_safe ?
                selected_join_candidate->component.parallel_workers : 0;
            join_evidence.gather_cost = join_evidence.parallel_safe ?
                selected_join_candidate->component.gather_cost : 0;
            join_evidence.order_preserving = path->pathkeys != NIL;
            join_evidence.shared_state_required =
                selected_join_candidate->component.shared_state_required;
            join_evidence.physical_properties_known = true;
            join_evidence.bound = age_graph_join_order_property_kind_is_bound(
                join_evidence.order_property_kind);
            age_graph_join_complete_path_evidence(path, &join_evidence);
            age_graph_join_register_rel_path_evidence(root, joinrel, path,
                                                      &join_evidence);
            register_graph_join_joinrel_lowering_evidence(root, joinrel, path,
                                                          &join_evidence);
        }
    }
}

static AgeGraphJoinCandidateTable *make_graph_expansion_join_candidate_table(
    Path *outer_path, Path *inner_path,
    const AgeGraphJoinPathEvidence *outer_evidence,
    const AgeGraphJoinPathEvidence *inner_evidence)
{
    AgeGraphJoinCandidateTable *table;

    table = age_graph_join_make_candidate_table();
    if (outer_evidence != NULL)
        table->declared_entry_count = Max(table->declared_entry_count,
                                          outer_evidence->declared_cover_count);
    if (inner_evidence != NULL)
        table->declared_entry_count = Max(table->declared_entry_count,
                                          inner_evidence->declared_cover_count);
    if ((outer_evidence != NULL &&
         outer_evidence->cover_match_kind == AGE_GRAPH_JOIN_COVER_MATCHED) ||
        (inner_evidence != NULL &&
         inner_evidence->cover_match_kind == AGE_GRAPH_JOIN_COVER_MATCHED))
    {
        table->cover_match_kind = AGE_GRAPH_JOIN_COVER_MATCHED;
    }
    else if (outer_evidence != NULL &&
             outer_evidence->cover_match_kind != AGE_GRAPH_JOIN_COVER_UNKNOWN)
    {
        table->cover_match_kind = outer_evidence->cover_match_kind;
    }
    else if (inner_evidence != NULL)
    {
        table->cover_match_kind = inner_evidence->cover_match_kind;
    }

    if (outer_path != NULL &&
        outer_evidence != NULL &&
        outer_evidence->bound)
    {
        add_graph_expansion_join_side_candidate(
            table, outer_path, inner_path, outer_path, outer_evidence);
    }

    if (inner_path != NULL &&
        inner_evidence != NULL &&
        inner_evidence->bound)
    {
        add_graph_expansion_join_side_candidate(
            table, outer_path, inner_path, inner_path, inner_evidence);
    }

    add_graph_expansion_join_value_candidate(table, outer_path, inner_path,
                                             outer_evidence,
                                             inner_evidence);
    add_graph_expansion_join_connector_candidate(table, outer_path, inner_path,
                                                 outer_evidence,
                                                 inner_evidence);

    return table;
}

static void add_graph_expansion_join_side_candidate(
    AgeGraphJoinCandidateTable *table, Path *outer_path, Path *inner_path,
    Path *evidence_path, const AgeGraphJoinPathEvidence *evidence)
{
    AgeGraphJoinCandidateRequest request;
    Relids provided_relids;
    Relids required_outer;
    Relids solved_relids;

    if (table == NULL ||
        evidence_path == NULL ||
        evidence == NULL ||
        evidence->order_property_kind == AGE_GRAPH_JOIN_ORDER_UNKNOWN ||
        evidence->component_kind == AGE_GRAPH_JOIN_COMPONENT_UNKNOWN)
    {
        return;
    }

    provided_relids = graph_join_joinrelids_from_paths(outer_path,
                                                       inner_path);
    required_outer = graph_join_required_outer_from_paths(
        outer_path, inner_path, provided_relids);
    solved_relids = bms_copy(provided_relids);
    age_graph_join_init_path_request(
        &request, evidence_path,
        evidence->bound ? AGE_GRAPH_JOIN_BOUND_BOUND :
        AGE_GRAPH_JOIN_BOUND_UNBOUND,
        evidence->component_kind, evidence->connector_kind,
        evidence->order_property_kind, evidence->source_evidence_kind,
        evidence->source_kind_id,
        evidence->shared_state_required);
    request.required_outer = required_outer;
    request.solved_relids = solved_relids;
    request.provided_relids = provided_relids;
    request.rows = evidence_path->rows;
    request.total_cost = evidence->selected_total_cost > 0 ?
        evidence->selected_total_cost : evidence_path->total_cost;
    request.total_cost = age_graph_join_apply_evidence_alternative_credit(
        request.startup_cost, request.total_cost,
        age_graph_join_evidence_alternative_credit(evidence));
    request.pattern_key = evidence->pattern_key;
    request.output_width = graph_join_output_width_from_paths(outer_path,
                                                              inner_path);
    request.parallel_safe =
        graph_join_paths_are_parallel_safe(outer_path, inner_path) &&
        evidence->parallel_safe;
    request.parallel_aware = request.parallel_safe &&
        evidence->parallel_aware;
    request.parallel_workers = request.parallel_safe ?
        evidence->parallel_workers : 0;
    request.gather_cost = request.parallel_safe ? evidence->gather_cost : 0;
    request.order_preserving =
        (outer_path != NULL && outer_path->pathkeys != NIL) ||
        (inner_path != NULL && inner_path->pathkeys != NIL);

    (void) age_graph_join_table_add_candidate(table, &request);
}

static void add_graph_expansion_join_value_candidate(
    AgeGraphJoinCandidateTable *table, Path *outer_path, Path *inner_path,
    const AgeGraphJoinPathEvidence *outer_evidence,
    const AgeGraphJoinPathEvidence *inner_evidence)
{
    AgeGraphJoinCandidateRequest request;
    Relids provided_relids;
    Relids required_outer;
    double outer_rows;
    double inner_rows;
    double value_rows;
    Cost startup_cost;
    Cost total_cost;

    if (table == NULL ||
        outer_path == NULL ||
        inner_path == NULL ||
        outer_evidence == NULL ||
        inner_evidence == NULL ||
        !outer_evidence->bound ||
        !inner_evidence->bound)
    {
        return;
    }

    provided_relids = graph_join_joinrelids_from_paths(outer_path,
                                                       inner_path);
    required_outer = graph_join_required_outer_from_paths(
        outer_path, inner_path, provided_relids);
    outer_rows = Max(outer_path->rows, 1.0);
    inner_rows = Max(inner_path->rows, 1.0);
    value_rows = clamp_row_est(Max(1.0, Min(outer_rows, inner_rows)));
    startup_cost = outer_path->startup_cost + inner_path->startup_cost;
    total_cost = startup_cost +
        value_rows * cpu_operator_cost * 0.10 +
        Max(outer_rows + inner_rows, 1.0) * cpu_tuple_cost * 0.01;
    total_cost = age_graph_join_apply_evidence_alternative_credit(
        startup_cost, total_cost,
        Min(age_graph_join_evidence_alternative_credit(outer_evidence),
            age_graph_join_evidence_alternative_credit(inner_evidence)));

    age_graph_join_init_path_request(
        &request,
        outer_path->total_cost <= inner_path->total_cost ?
        outer_path : inner_path,
        AGE_GRAPH_JOIN_BOUND_BOUND,
        AGE_GRAPH_JOIN_COMPONENT_VALUE_JOIN,
        AGE_GRAPH_JOIN_CONNECTOR_VALUE_JOIN, AGE_GRAPH_JOIN_ORDER_VALUE,
        AGE_GRAPH_JOIN_SOURCE_EVIDENCE_GRAPH_VALUE,
        AGE_GRAPH_JOIN_SOURCE_VALUE_JOIN, false);
    request.solved_relids = bms_copy(provided_relids);
    request.required_outer = required_outer;
    request.provided_relids = provided_relids;
    request.rows = value_rows;
    request.startup_cost = startup_cost;
    request.total_cost = total_cost;
    request.pattern_key = graph_join_pattern_key_from_evidence_pair(
        outer_evidence, inner_evidence);
    request.output_width = graph_join_output_width_from_paths(outer_path,
                                                              inner_path);
    request.parallel_safe = graph_join_paths_are_parallel_safe(outer_path,
                                                               inner_path);
    request.parallel_aware = false;
    request.parallel_workers = 0;
    request.gather_cost = 0;
    request.order_preserving = false;
    request.shared_state_required =
        outer_evidence->shared_state_required ||
        inner_evidence->shared_state_required;

    (void) age_graph_join_table_add_candidate(table, &request);
}

static void add_graph_expansion_join_connector_candidate(
    AgeGraphJoinCandidateTable *table, Path *outer_path, Path *inner_path,
    const AgeGraphJoinPathEvidence *outer_evidence,
    const AgeGraphJoinPathEvidence *inner_evidence)
{
    AgeGraphJoinCandidateRequest request;
    Relids provided_relids;
    Relids required_outer;
    Relids inner_required;
    Relids outer_relids;
    AgeGraphJoinComponentKind component_kind;
    AgeGraphJoinConnectorKind connector_kind;
    AgeGraphJoinOrderPropertyKind order_property_kind;
    AgeGraphJoinSourceEvidenceKind source_evidence_kind;
    AgeGraphJoinSourceKind source_kind_id;
    double outer_rows;
    double inner_rows;
    double join_rows;
    Cost startup_cost;
    Cost total_cost;

    if (table == NULL ||
        outer_path == NULL ||
        inner_path == NULL ||
        outer_evidence == NULL ||
        inner_evidence == NULL)
    {
        return;
    }

    provided_relids = graph_join_joinrelids_from_paths(outer_path,
                                                       inner_path);
    required_outer = graph_join_required_outer_from_paths(
        outer_path, inner_path, provided_relids);
    inner_required = PATH_REQ_OUTER(inner_path);
    outer_relids = outer_path->parent != NULL ?
        outer_path->parent->relids : NULL;
    if (inner_required != NULL &&
        outer_relids != NULL &&
        bms_overlap(inner_required, outer_relids))
    {
        component_kind = AGE_GRAPH_JOIN_COMPONENT_APPLY;
        connector_kind = AGE_GRAPH_JOIN_CONNECTOR_APPLY;
        order_property_kind = AGE_GRAPH_JOIN_ORDER_APPLY;
        source_evidence_kind = AGE_GRAPH_JOIN_SOURCE_EVIDENCE_GRAPH_APPLY;
        source_kind_id = AGE_GRAPH_JOIN_SOURCE_APPLY;
    }
    else
    {
        component_kind = AGE_GRAPH_JOIN_COMPONENT_CARTESIAN;
        connector_kind = AGE_GRAPH_JOIN_CONNECTOR_CARTESIAN;
        order_property_kind = AGE_GRAPH_JOIN_ORDER_CARTESIAN;
        source_evidence_kind =
            AGE_GRAPH_JOIN_SOURCE_EVIDENCE_GRAPH_CARTESIAN;
        source_kind_id = AGE_GRAPH_JOIN_SOURCE_CARTESIAN;
    }

    outer_rows = Max(outer_path->rows, 1.0);
    inner_rows = Max(inner_path->rows, 1.0);
    join_rows = clamp_row_est(outer_rows * inner_rows);
    startup_cost = outer_path->startup_cost + inner_path->startup_cost;
    total_cost = startup_cost +
        outer_path->total_cost + inner_path->total_cost +
        join_rows * cpu_tuple_cost +
        Max(outer_rows + inner_rows, 1.0) * cpu_operator_cost * 0.25;
    total_cost = age_graph_join_apply_evidence_alternative_credit(
        startup_cost, total_cost,
        Min(age_graph_join_evidence_alternative_credit(outer_evidence),
            age_graph_join_evidence_alternative_credit(inner_evidence)));

    age_graph_join_init_path_request(
        &request, inner_path,
        component_kind == AGE_GRAPH_JOIN_COMPONENT_APPLY ?
        AGE_GRAPH_JOIN_BOUND_BOUND : AGE_GRAPH_JOIN_BOUND_UNBOUND,
        component_kind, connector_kind, order_property_kind,
        source_evidence_kind, source_kind_id, false);
    request.solved_relids = bms_copy(provided_relids);
    request.required_outer = required_outer;
    request.provided_relids = provided_relids;
    request.rows = join_rows;
    request.startup_cost = startup_cost;
    request.total_cost = total_cost;
    request.pattern_key = graph_join_pattern_key_from_evidence_pair(
        outer_evidence, inner_evidence);
    request.output_width = graph_join_output_width_from_paths(outer_path,
                                                              inner_path);
    request.parallel_safe = graph_join_paths_are_parallel_safe(outer_path,
                                                               inner_path);
    request.parallel_aware = false;
    request.parallel_workers = 0;
    request.gather_cost = 0;
    request.order_preserving = false;
    request.shared_state_required =
        inner_evidence->shared_state_required;

    (void) age_graph_join_table_add_candidate(table, &request);
}

static const char *graph_join_pattern_key_from_evidence_pair(
    const AgeGraphJoinPathEvidence *outer_evidence,
    const AgeGraphJoinPathEvidence *inner_evidence)
{
    const char *outer_pattern;
    const char *inner_pattern;

    outer_pattern = outer_evidence != NULL ? outer_evidence->pattern_key :
        NULL;
    inner_pattern = inner_evidence != NULL ? inner_evidence->pattern_key :
        NULL;

    if (outer_pattern != NULL &&
        inner_pattern != NULL)
    {
        if (strcmp(outer_pattern, inner_pattern) == 0)
            return outer_pattern;
        return "graph-pattern-join";
    }
    if (outer_pattern != NULL)
        return outer_pattern;
    if (inner_pattern != NULL)
        return inner_pattern;

    return NULL;
}

static Relids graph_join_joinrelids_from_paths(Path *outer_path,
                                               Path *inner_path)
{
    Relids relids = NULL;

    if (outer_path != NULL && outer_path->parent != NULL)
        relids = bms_union(relids, outer_path->parent->relids);
    if (inner_path != NULL && inner_path->parent != NULL)
        relids = bms_add_members(relids, inner_path->parent->relids);

    return relids;
}

static Relids graph_join_required_outer_from_paths(Path *outer_path,
                                                   Path *inner_path,
                                                   Relids provided_relids)
{
    Relids required_outer = NULL;

    if (outer_path != NULL)
        required_outer = bms_union(required_outer, PATH_REQ_OUTER(outer_path));
    if (inner_path != NULL)
        required_outer = bms_add_members(required_outer,
                                         PATH_REQ_OUTER(inner_path));
    if (provided_relids != NULL)
        required_outer = bms_del_members(required_outer, provided_relids);

    return required_outer;
}

static int graph_join_output_width_from_paths(Path *outer_path,
                                              Path *inner_path)
{
    int output_width = 0;

    if (outer_path != NULL)
    {
        if (outer_path->pathtarget != NULL &&
            outer_path->pathtarget->width > 0)
            output_width += outer_path->pathtarget->width;
        else if (outer_path->parent != NULL &&
                 outer_path->parent->reltarget != NULL)
            output_width += outer_path->parent->reltarget->width;
    }
    if (inner_path != NULL)
    {
        if (inner_path->pathtarget != NULL &&
            inner_path->pathtarget->width > 0)
            output_width += inner_path->pathtarget->width;
        else if (inner_path->parent != NULL &&
                 inner_path->parent->reltarget != NULL)
            output_width += inner_path->parent->reltarget->width;
    }

    return output_width;
}

static bool graph_join_paths_are_parallel_safe(Path *outer_path,
                                               Path *inner_path)
{
    return outer_path != NULL &&
           inner_path != NULL &&
           outer_path->parallel_safe &&
           inner_path->parallel_safe;
}

static bool path_get_graph_expansion_join_evidence(
    Path *path, Relids outer_relids, AgeGraphJoinPathEvidence *evidence)
{
    AgeGraphJoinPathEvidence local_evidence;

    if (path == NULL)
        return false;
    if (evidence == NULL)
        evidence = &local_evidence;
    age_graph_join_init_path_evidence(evidence);

    if (IsA(path, CustomPath))
    {
        CustomPath *custom_path = (CustomPath *)path;

        if (!path_required_outer_is_subset(path, outer_relids))
            return false;

        if (custom_path->methods == &age_adjacency_match_path_methods)
        {
            if (!custom_path_adjacency_join_evidence(custom_path, evidence))
                return false;
        }
        else if (custom_path->methods == &age_vle_stream_path_methods)
        {
            if (!custom_path_vle_join_evidence(custom_path, evidence))
                return false;
        }
        else
            return false;

        age_graph_join_complete_path_evidence(path, evidence);
        return evidence->order_property_kind != AGE_GRAPH_JOIN_ORDER_UNKNOWN;
    }

    /*
     * A node/property index seek can be an unparameterized anchor.  Keep it in
     * the graph join evidence vocabulary even when it does not require the
     * opposite side's relids.
     */
    if (!IsA(path, CustomPath))
    {
        AgeGraphJoinOrderPropertyKind order_property_kind;

        order_property_kind = index_path_join_order_property_kind(path);
        if (order_property_kind != AGE_GRAPH_JOIN_ORDER_UNKNOWN)
        {
            evidence->connector_kind =
                AGE_GRAPH_JOIN_CONNECTOR_POSTGRES_INDEX_SEEK;
            evidence->order_property_kind =
                AGE_GRAPH_JOIN_ORDER_INDEX_ANCHORED;
            evidence->source_evidence_kind =
                AGE_GRAPH_JOIN_SOURCE_EVIDENCE_POSTGRES_INDEX;
            evidence->source_kind_id =
                AGE_GRAPH_JOIN_SOURCE_NODE_PROPERTY_INDEX;
            evidence->component_kind =
                AGE_GRAPH_JOIN_COMPONENT_NODE_PROPERTY_SEEK;
            evidence->candidate_count = 1;
            evidence->selected_total_cost = path->total_cost;
            evidence->next_total_cost = 0;
            evidence->bound = age_graph_join_order_property_kind_is_bound(
                evidence->order_property_kind);
            age_graph_join_complete_path_evidence(path, evidence);
            return true;
        }
    }

    if (IsA(path, MaterialPath))
        return path_get_graph_expansion_join_evidence(
            ((MaterialPath *)path)->subpath, outer_relids, evidence);

    if (IsA(path, MemoizePath))
        return path_get_graph_expansion_join_evidence(
            ((MemoizePath *)path)->subpath, outer_relids, evidence);

    if (IsA(path, ProjectionPath))
        return path_get_graph_expansion_join_evidence(
            ((ProjectionPath *)path)->subpath, outer_relids, evidence);

    if (IsA(path, SubqueryScanPath))
        return path_get_graph_expansion_join_evidence(
            ((SubqueryScanPath *)path)->subpath, outer_relids, evidence);

    return false;
}

static AgeGraphJoinOrderPropertyKind index_path_join_order_property_kind(
    Path *path)
{
    if (!path_is_index_backed(path))
        return AGE_GRAPH_JOIN_ORDER_UNKNOWN;

    return AGE_GRAPH_JOIN_ORDER_INDEX_ANCHORED;
}

static bool path_is_index_backed(Path *path)
{
    ListCell *lc;

    if (path == NULL)
        return false;

    if (IsA(path, IndexPath))
    {
        IndexPath *index_path = (IndexPath *)path;

        return index_path->indexclauses != NIL;
    }

    if (IsA(path, BitmapHeapPath))
        return path_is_index_backed(((BitmapHeapPath *)path)->bitmapqual);

    if (IsA(path, BitmapAndPath))
    {
        BitmapAndPath *bitmap_path = (BitmapAndPath *)path;

        foreach(lc, bitmap_path->bitmapquals)
        {
            if (path_is_index_backed(lfirst(lc)))
                return true;
        }
    }

    if (IsA(path, BitmapOrPath))
    {
        BitmapOrPath *bitmap_path = (BitmapOrPath *)path;

        foreach(lc, bitmap_path->bitmapquals)
        {
            if (path_is_index_backed(lfirst(lc)))
                return true;
        }
    }

    return false;
}

static Oid graph_join_index_path_oid(Path *path)
{
    ListCell *lc;

    if (path == NULL)
        return InvalidOid;

    if (IsA(path, IndexPath))
    {
        IndexPath *index_path = (IndexPath *)path;

        if (index_path->indexinfo != NULL &&
            index_path->indexclauses != NIL)
        {
            return index_path->indexinfo->indexoid;
        }

        return InvalidOid;
    }

    if (IsA(path, BitmapHeapPath))
        return graph_join_index_path_oid(
            ((BitmapHeapPath *)path)->bitmapqual);

    if (IsA(path, BitmapAndPath))
    {
        BitmapAndPath *bitmap_path = (BitmapAndPath *)path;

        foreach(lc, bitmap_path->bitmapquals)
        {
            Oid index_oid = graph_join_index_path_oid(lfirst(lc));

            if (OidIsValid(index_oid))
                return index_oid;
        }
    }

    if (IsA(path, BitmapOrPath))
    {
        BitmapOrPath *bitmap_path = (BitmapOrPath *)path;

        foreach(lc, bitmap_path->bitmapquals)
        {
            Oid index_oid = graph_join_index_path_oid(lfirst(lc));

            if (OidIsValid(index_oid))
                return index_oid;
        }
    }

    return InvalidOid;
}

static bool custom_path_adjacency_join_evidence(
    CustomPath *custom_path, AgeGraphJoinPathEvidence *evidence)
{
    List *descriptor;

    Assert(evidence != NULL);
    age_graph_join_init_path_evidence(evidence);

    if (custom_path == NULL ||
        custom_path->methods != &age_adjacency_match_path_methods ||
        list_length(custom_path->custom_private) < 5)
    {
        return false;
    }

    descriptor = list_nth_node(List, custom_path->custom_private, 4);
    if (list_length(descriptor) != AGE_ADJACENCY_MATCH_DESC_COUNT)
        return false;

    evidence->connector_kind =
        (AgeGraphJoinConnectorKind)age_graph_join_descriptor_int_field(
            descriptor,
            AGE_ADJACENCY_MATCH_DESC_JOIN_ORDER_CONNECTOR_KIND,
            AGE_GRAPH_JOIN_CONNECTOR_UNKNOWN);
    evidence->order_property_kind =
        (AgeGraphJoinOrderPropertyKind)age_graph_join_descriptor_int_field(
            descriptor,
            AGE_ADJACENCY_MATCH_DESC_JOIN_ORDER_PROPERTY_KIND,
            AGE_GRAPH_JOIN_ORDER_UNKNOWN);
    evidence->source_kind_id =
        (AgeGraphJoinSourceKind)age_graph_join_descriptor_int_field(
            descriptor,
            AGE_ADJACENCY_MATCH_DESC_JOIN_ORDER_SOURCE_KIND_ID,
            AGE_GRAPH_JOIN_SOURCE_UNKNOWN);
    if (evidence->order_property_kind == AGE_GRAPH_JOIN_ORDER_INDEX_ANCHORED)
        evidence->source_evidence_kind =
            AGE_GRAPH_JOIN_SOURCE_EVIDENCE_NODE_PROPERTY_INDEX;
    else if (evidence->order_property_kind ==
             AGE_GRAPH_JOIN_ORDER_ADJACENCY_DIRECTORY)
        evidence->source_evidence_kind =
            AGE_GRAPH_JOIN_SOURCE_EVIDENCE_ADJACENCY_DIRECTORY;
    else if (evidence->order_property_kind == AGE_GRAPH_JOIN_ORDER_ADJACENCY)
        evidence->source_evidence_kind =
            AGE_GRAPH_JOIN_SOURCE_EVIDENCE_ADJACENCY_VALUE;
    else
        evidence->source_evidence_kind =
            AGE_GRAPH_JOIN_SOURCE_EVIDENCE_ADJACENCY_PAYLOAD;
    evidence->component_kind = AGE_GRAPH_JOIN_COMPONENT_ADJACENCY_EXPANSION;
    evidence->candidate_count = age_graph_join_descriptor_int_field(
        descriptor, AGE_ADJACENCY_MATCH_DESC_JOIN_ORDER_CANDIDATE_COUNT, 1);
    evidence->declared_cover_count = age_graph_join_descriptor_int_field(
        descriptor,
        AGE_ADJACENCY_MATCH_DESC_JOIN_ORDER_DECLARED_COVER_COUNT, 0);
    evidence->cover_match_kind =
        (AgeGraphJoinCoverMatchKind)age_graph_join_descriptor_int_field(
            descriptor,
            AGE_ADJACENCY_MATCH_DESC_JOIN_ORDER_COVER_MATCH_KIND,
            AGE_GRAPH_JOIN_COVER_UNKNOWN);
    evidence->selected_total_cost = custom_path->path.total_cost;
    evidence->next_total_cost = age_graph_join_descriptor_float_field(
        descriptor, AGE_ADJACENCY_MATCH_DESC_JOIN_ORDER_NEXT_TOTAL_COST, 0);
    evidence->parallel_safe = age_graph_join_descriptor_int_field(
        descriptor, AGE_ADJACENCY_MATCH_DESC_PARALLEL_SAFE, 0) != 0;
    evidence->parallel_aware = age_graph_join_descriptor_int_field(
        descriptor, AGE_ADJACENCY_MATCH_DESC_PARALLEL_AWARE, 0) != 0;
    evidence->parallel_workers = (int)age_graph_join_descriptor_int_field(
        descriptor, AGE_ADJACENCY_MATCH_DESC_PARALLEL_WORKERS, 0);
    evidence->gather_cost = age_graph_join_descriptor_float_field(
        descriptor, AGE_ADJACENCY_MATCH_DESC_PARALLEL_GATHER_COST, 0);
    evidence->order_preserving = age_graph_join_descriptor_int_field(
        descriptor, AGE_ADJACENCY_MATCH_DESC_ORDER_PRESERVING, 0) != 0;
    evidence->shared_state_required = age_graph_join_descriptor_int_field(
        descriptor, AGE_ADJACENCY_MATCH_DESC_SHARED_STATE_REQUIRED, 0) != 0;
    evidence->physical_properties_known = true;
    evidence->bound = age_graph_join_order_property_kind_is_bound(
        evidence->order_property_kind);

    return evidence->order_property_kind != AGE_GRAPH_JOIN_ORDER_UNKNOWN;
}

static bool custom_path_vle_join_evidence(
    CustomPath *custom_path, AgeGraphJoinPathEvidence *evidence)
{
    List *edge_source;
    List *graph_join_descriptor;
    Node *graph_join_private;
    AgeVLEStreamCompositePlanKind composite_plan_kind;
    AgeVLEStreamFanoutSourceKind start_fanout_source_kind;
    AgeVLEStreamFanoutSourceKind end_fanout_source_kind;

    Assert(evidence != NULL);
    age_graph_join_init_path_evidence(evidence);

    if (custom_path == NULL ||
        custom_path->methods != &age_vle_stream_path_methods ||
        list_length(custom_path->custom_private) < 8)
    {
        return false;
    }

    if (list_length(custom_path->custom_private) >
        AGE_VLE_STREAM_PRIVATE_GRAPH_JOIN + 1)
    {
        graph_join_private = list_nth(custom_path->custom_private,
                                      AGE_VLE_STREAM_PRIVATE_GRAPH_JOIN + 1);
        if (graph_join_private != NULL &&
            IsA(graph_join_private, List) &&
            list_length((List *)graph_join_private) ==
            AGE_GRAPH_JOIN_DESC_COUNT)
        {
            graph_join_descriptor = castNode(List, graph_join_private);
            evidence->pattern_key = age_graph_join_descriptor_text_field(
                graph_join_descriptor,
                AGE_GRAPH_JOIN_DESC_PATTERN_KEY);
            evidence->component_kind =
                (AgeGraphJoinComponentKind)age_graph_join_descriptor_int_field(
                    graph_join_descriptor,
                    AGE_GRAPH_JOIN_DESC_COMPONENT_KIND,
                    AGE_GRAPH_JOIN_COMPONENT_UNKNOWN);
            evidence->connector_kind =
                (AgeGraphJoinConnectorKind)age_graph_join_descriptor_int_field(
                    graph_join_descriptor, AGE_GRAPH_JOIN_DESC_CONNECTOR_KIND,
                    AGE_GRAPH_JOIN_CONNECTOR_UNKNOWN);
            evidence->order_property_kind =
                (AgeGraphJoinOrderPropertyKind)
                age_graph_join_descriptor_int_field(
                    graph_join_descriptor,
                    AGE_GRAPH_JOIN_DESC_ORDER_PROPERTY_KIND,
                    AGE_GRAPH_JOIN_ORDER_UNKNOWN);
            evidence->source_evidence_kind =
                (AgeGraphJoinSourceEvidenceKind)
                age_graph_join_descriptor_int_field(
                    graph_join_descriptor,
                    AGE_GRAPH_JOIN_DESC_SOURCE_EVIDENCE_KIND,
                    AGE_GRAPH_JOIN_SOURCE_EVIDENCE_UNKNOWN);
            evidence->source_kind_id =
                (AgeGraphJoinSourceKind)age_graph_join_descriptor_int_field(
                    graph_join_descriptor, AGE_GRAPH_JOIN_DESC_SOURCE_KIND_ID,
                    AGE_GRAPH_JOIN_SOURCE_UNKNOWN);
            evidence->candidate_count = age_graph_join_descriptor_int_field(
                graph_join_descriptor, AGE_GRAPH_JOIN_DESC_CANDIDATE_COUNT,
                1);
            evidence->declared_cover_count =
                age_graph_join_descriptor_int_field(
                    graph_join_descriptor,
                    AGE_GRAPH_JOIN_DESC_DECLARED_COVER_COUNT, 0);
            evidence->cover_match_kind =
                (AgeGraphJoinCoverMatchKind)
                age_graph_join_descriptor_int_field(
                    graph_join_descriptor,
                    AGE_GRAPH_JOIN_DESC_COVER_MATCH_KIND,
                    AGE_GRAPH_JOIN_COVER_UNKNOWN);
            evidence->selected_total_cost =
                age_graph_join_descriptor_float_field(
                    graph_join_descriptor, AGE_GRAPH_JOIN_DESC_TOTAL_COST,
                    custom_path->path.total_cost);
            evidence->next_total_cost =
                age_graph_join_descriptor_float_field(
                    graph_join_descriptor,
                    AGE_GRAPH_JOIN_DESC_NEXT_TOTAL_COST, 0);
            evidence->output_width = (int)age_graph_join_descriptor_int_field(
                graph_join_descriptor, AGE_GRAPH_JOIN_DESC_OUTPUT_WIDTH, 0);
            evidence->parallel_safe = age_graph_join_descriptor_int_field(
                graph_join_descriptor, AGE_GRAPH_JOIN_DESC_PARALLEL_SAFE,
                0) != 0;
            evidence->parallel_aware = age_graph_join_descriptor_int_field(
                graph_join_descriptor, AGE_GRAPH_JOIN_DESC_PARALLEL_AWARE,
                0) != 0;
            evidence->parallel_workers =
                (int)age_graph_join_descriptor_int_field(
                    graph_join_descriptor,
                    AGE_GRAPH_JOIN_DESC_PARALLEL_WORKERS, 0);
            evidence->gather_cost = age_graph_join_descriptor_float_field(
                graph_join_descriptor, AGE_GRAPH_JOIN_DESC_GATHER_COST, 0);
            evidence->order_preserving = age_graph_join_descriptor_int_field(
                graph_join_descriptor, AGE_GRAPH_JOIN_DESC_ORDER_PRESERVING,
                0) != 0;
            evidence->shared_state_required =
                age_graph_join_descriptor_int_field(
                    graph_join_descriptor,
                    AGE_GRAPH_JOIN_DESC_SHARED_STATE_REQUIRED, 0) != 0;
            evidence->physical_properties_known = true;
            evidence->bound = age_graph_join_order_property_kind_is_bound(
                evidence->order_property_kind);

            return evidence->order_property_kind !=
                   AGE_GRAPH_JOIN_ORDER_UNKNOWN;
        }
    }

    edge_source = list_nth_node(List, custom_path->custom_private, 7);
    if (list_length(edge_source) != AGE_VLE_STREAM_EDGE_SOURCE_COUNT)
        return false;

    composite_plan_kind =
        (AgeVLEStreamCompositePlanKind)vle_stream_descriptor_int8_field(
            edge_source,
            AGE_VLE_STREAM_EDGE_SOURCE_COMPOSITE_SOURCE_PLANNED_KIND,
            AGE_VLE_STREAM_COMPOSITE_PLAN_UNKNOWN);
    if (vle_stream_edge_source_bool_field(
            edge_source,
            AGE_VLE_STREAM_EDGE_SOURCE_COMPOSITE_SOURCE_KNOWN) &&
        age_vle_stream_composite_plan_is_property_prefilter(
            composite_plan_kind))
    {
        evidence->connector_kind =
            AGE_GRAPH_JOIN_CONNECTOR_VLE_COMPOSITE_EXPAND;
        evidence->order_property_kind = AGE_GRAPH_JOIN_ORDER_INDEX_ANCHORED;
        evidence->source_evidence_kind =
            AGE_GRAPH_JOIN_SOURCE_EVIDENCE_VLE_EDGE_SOURCE;
        evidence->component_kind = AGE_GRAPH_JOIN_COMPONENT_VLE_EXPANSION;
        evidence->selected_total_cost = custom_path->path.total_cost;
        evidence->bound = true;
        return true;
    }

    start_fanout_source_kind =
        (AgeVLEStreamFanoutSourceKind)vle_stream_descriptor_int8_field(
            edge_source, AGE_VLE_STREAM_EDGE_SOURCE_START_FANOUT_SOURCE_KIND,
            AGE_VLE_STREAM_FANOUT_SOURCE_UNKNOWN);
    end_fanout_source_kind =
        (AgeVLEStreamFanoutSourceKind)vle_stream_descriptor_int8_field(
            edge_source, AGE_VLE_STREAM_EDGE_SOURCE_END_FANOUT_SOURCE_KIND,
            AGE_VLE_STREAM_FANOUT_SOURCE_UNKNOWN);
    if (age_vle_stream_fanout_source_is_directory_label(
            start_fanout_source_kind) ||
        age_vle_stream_fanout_source_is_directory_label(
            end_fanout_source_kind))
    {
        evidence->connector_kind = AGE_GRAPH_JOIN_CONNECTOR_VLE_EXPAND;
        evidence->order_property_kind = AGE_GRAPH_JOIN_ORDER_VLE_FRONTIER;
        evidence->source_evidence_kind =
            AGE_GRAPH_JOIN_SOURCE_EVIDENCE_VLE_EDGE_SOURCE;
        evidence->component_kind = AGE_GRAPH_JOIN_COMPONENT_VLE_EXPANSION;
        evidence->selected_total_cost = custom_path->path.total_cost;
        evidence->bound = true;
        return true;
    }

    if (vle_stream_edge_source_bool_field(
            edge_source,
            AGE_VLE_STREAM_EDGE_SOURCE_CACHE_SEED_ELIGIBLE) ||
        vle_stream_edge_source_bool_field(
            edge_source,
            AGE_VLE_STREAM_EDGE_SOURCE_EMPTY_LIFECYCLE_ELIGIBLE) ||
        vle_stream_edge_source_bool_field(
            edge_source,
            AGE_VLE_STREAM_EDGE_SOURCE_PAYLOAD_INPUT_KNOWN))
    {
        evidence->connector_kind = AGE_GRAPH_JOIN_CONNECTOR_VLE_EXPAND;
        evidence->order_property_kind = AGE_GRAPH_JOIN_ORDER_VLE_FRONTIER;
        evidence->source_evidence_kind =
            AGE_GRAPH_JOIN_SOURCE_EVIDENCE_VLE_EDGE_SOURCE;
        evidence->component_kind = AGE_GRAPH_JOIN_COMPONENT_VLE_EXPANSION;
        evidence->selected_total_cost = custom_path->path.total_cost;
        evidence->bound = true;
        return true;
    }

    if (custom_path_vle_has_bound_endpoints(custom_path))
    {
        evidence->connector_kind = AGE_GRAPH_JOIN_CONNECTOR_VLE_EXPAND_INTO;
        evidence->order_property_kind = AGE_GRAPH_JOIN_ORDER_EXPAND_INTO;
        evidence->source_evidence_kind =
            AGE_GRAPH_JOIN_SOURCE_EVIDENCE_VLE_EDGE_SOURCE;
        evidence->component_kind = AGE_GRAPH_JOIN_COMPONENT_VLE_EXPANSION;
        evidence->selected_total_cost = custom_path->path.total_cost;
        evidence->bound = true;
        return true;
    }

    evidence->connector_kind = AGE_GRAPH_JOIN_CONNECTOR_VLE_EXPAND;
    evidence->order_property_kind = AGE_GRAPH_JOIN_ORDER_QUERY;
    evidence->source_evidence_kind =
        AGE_GRAPH_JOIN_SOURCE_EVIDENCE_VLE_EDGE_SOURCE;
    evidence->component_kind = AGE_GRAPH_JOIN_COMPONENT_VLE_EXPANSION;
    evidence->selected_total_cost = custom_path->path.total_cost;
    evidence->bound = false;
    return true;
}

static bool custom_path_vle_has_bound_endpoints(CustomPath *custom_path)
{
    List *func_args;

    if (custom_path == NULL ||
        custom_path->methods != &age_vle_stream_path_methods ||
        custom_path->custom_private == NIL)
    {
        return false;
    }

    func_args = linitial_node(List, custom_path->custom_private);
    return vle_stream_func_args_have_bound_endpoints(func_args);
}

static int64 vle_stream_descriptor_int8_field(List *descriptor, int index,
                                              int64 fallback)
{
    Const *value;

    if (descriptor == NIL || list_length(descriptor) <= index)
        return fallback;

    value = list_nth_node(Const, descriptor, index);
    if (value->constisnull || value->consttype != INT8OID)
        return fallback;

    return DatumGetInt64(value->constvalue);
}

static bool vle_stream_edge_source_bool_field(List *descriptor, int index)
{
    Node *value;

    value = list_nth(descriptor, index);
    if (value == NULL || !IsA(value, Integer))
        return false;

    return intVal(value) != 0;
}

static bool path_required_outer_is_subset(Path *path, Relids relids)
{
    Relids required_outer;

    if (path == NULL ||
        relids == NULL)
    {
        return false;
    }

    required_outer = PATH_REQ_OUTER(path);

    return !bms_is_empty(required_outer) &&
           bms_is_subset(required_outer, relids);
}

static bool path_contains_graph_expansion(Path *path)
{
    if (path == NULL)
        return false;

    if (IsA(path, CustomPath))
    {
        CustomPath *custom_path = (CustomPath *)path;

        return custom_path->methods == &age_adjacency_match_path_methods ||
               custom_path->methods == &age_vle_stream_path_methods;
    }

    if (IsA(path, NestPath) ||
        IsA(path, MergePath) ||
        IsA(path, HashPath))
    {
        JoinPath *join_path = (JoinPath *)path;

        return path_contains_graph_expansion(join_path->outerjoinpath) ||
               path_contains_graph_expansion(join_path->innerjoinpath);
    }

    if (IsA(path, MaterialPath))
        return path_contains_graph_expansion(((MaterialPath *)path)->subpath);

    if (IsA(path, GatherPath))
        return path_contains_graph_expansion(((GatherPath *)path)->subpath);

    if (IsA(path, GatherMergePath))
        return path_contains_graph_expansion(
            ((GatherMergePath *)path)->subpath);

    if (IsA(path, MemoizePath))
        return path_contains_graph_expansion(((MemoizePath *)path)->subpath);

    if (IsA(path, ProjectionPath))
        return path_contains_graph_expansion(((ProjectionPath *)path)->subpath);

    if (IsA(path, SubqueryScanPath))
        return path_contains_graph_expansion(((SubqueryScanPath *)path)->subpath);

    return false;
}

static const char *path_param_info_string(Path *path)
{
    if (path == NULL ||
        path->param_info == NULL)
    {
        return "<none>";
    }

    return bmsToString(PATH_REQ_OUTER(path));
}

static CypherAdjacencyMatchCandidate *pop_adjacency_match_candidate(
    PlannerInfo *root, RangeTblEntry *rte)
{
    ListCell *lc;
    const char *aliasname = NULL;

    if (rte == NULL ||
        rte->rtekind != RTE_RELATION ||
        adjacency_match_candidates == NIL)
    {
        return NULL;
    }

    if (rte->alias != NULL)
        aliasname = rte->alias->aliasname;
    else if (rte->eref != NULL)
        aliasname = rte->eref->aliasname;

    foreach(lc, adjacency_match_candidates)
    {
        CypherAdjacencyMatchCandidate *candidate = lfirst(lc);

        if (candidate->edge_label_oid == rte->relid &&
            aliasname != NULL &&
            strcmp(candidate->edge_alias, aliasname) == 0)
        {
            adjacency_match_candidates =
                list_delete_cell(adjacency_match_candidates, lc);
            bind_adjacency_match_candidate_outer_relids(root, candidate);
            return candidate;
        }
    }

    return NULL;
}

static CypherGraphPatternRegistryEntry *find_graph_pattern_registry_entry(
    const char *graph_pattern_key)
{
    ListCell *lc;

    if (graph_pattern_key == NULL)
        return NULL;

    foreach(lc, graph_pattern_registry)
    {
        CypherGraphPatternRegistryEntry *entry = lfirst(lc);

        if (entry != NULL &&
            entry->graph_pattern_key != NULL &&
            strcmp(entry->graph_pattern_key, graph_pattern_key) == 0)
        {
            return entry;
        }
    }

    return NULL;
}

static CypherGraphPatternRegistryEntry *ensure_graph_pattern_registry_entry(
    const char *graph_pattern_key)
{
    MemoryContext oldcontext;
    CypherGraphPatternRegistryEntry *entry;

    if (graph_pattern_key == NULL)
        return NULL;

    entry = find_graph_pattern_registry_entry(graph_pattern_key);
    if (entry != NULL)
        return entry;

    oldcontext = MemoryContextSwitchTo(TopTransactionContext);

    entry = palloc0(sizeof(*entry));
    entry->graph_pattern_key = pstrdup(graph_pattern_key);
    graph_pattern_registry = lappend(graph_pattern_registry, entry);

    MemoryContextSwitchTo(oldcontext);

    return entry;
}

static AgeGraphJoinLoweringArtifact *graph_pattern_registry_artifact(
    const char *graph_pattern_key)
{
    MemoryContext oldcontext;
    CypherGraphPatternRegistryEntry *entry;

    entry = ensure_graph_pattern_registry_entry(graph_pattern_key);
    if (entry == NULL)
        return NULL;

    if (entry->artifact != NULL)
        return entry->artifact;

    oldcontext = MemoryContextSwitchTo(TopTransactionContext);
    entry->artifact = age_graph_join_make_pattern_lowering_artifact(
        entry->graph_pattern_key, AGE_GRAPH_JOIN_SOURCE_UNKNOWN, NULL);
    MemoryContextSwitchTo(oldcontext);

    return entry->artifact;
}

static void graph_pattern_registry_register_declared_cover(
    PlannerInfo *root, RelOptInfo *rel, const char *graph_pattern_key)
{
    CypherGraphPatternRegistryEntry *entry;

    if (root == NULL ||
        rel == NULL ||
        graph_pattern_key == NULL)
    {
        return;
    }

    entry = find_graph_pattern_registry_entry(graph_pattern_key);
    if (entry == NULL ||
        entry->artifact == NULL ||
        entry->artifact->entries == NIL)
    {
        return;
    }

    (void) age_graph_join_register_rel_lowering_artifact_pool(
        root, rel, entry->artifact);
}

static void graph_pattern_registry_declare_typed_source(
    const char *graph_pattern_key, AgeGraphJoinSourceKind source_kind_id,
    AgeGraphJoinComponentKind component_family_kind)
{
    AgeGraphJoinLoweringArtifact *artifact;
    MemoryContext oldcontext;

    if (graph_pattern_key == NULL ||
        source_kind_id == AGE_GRAPH_JOIN_SOURCE_UNKNOWN)
    {
        return;
    }

    artifact = graph_pattern_registry_artifact(graph_pattern_key);
    if (artifact == NULL)
        return;

    oldcontext = MemoryContextSwitchTo(TopTransactionContext);
    age_graph_join_lowering_artifact_declare_typed_entry(
        artifact, source_kind_id, component_family_kind);
    MemoryContextSwitchTo(oldcontext);
}

static AgeGraphJoinCandidateTable *graph_pattern_registry_typed_entry_table(
    const char *graph_pattern_key, AgeGraphJoinSourceKind source_kind_id,
    AgeGraphJoinComponentKind component_family_kind)
{
    AgeGraphJoinLoweringArtifact *artifact;
    const char *effective_pattern_key;

    if (source_kind_id == AGE_GRAPH_JOIN_SOURCE_UNKNOWN)
        return NULL;

    effective_pattern_key = graph_pattern_key != NULL ?
        graph_pattern_key : "graph-pattern";
    artifact = graph_pattern_registry_artifact(effective_pattern_key);
    if (artifact == NULL)
        return NULL;

    if (graph_pattern_key == NULL)
        return age_graph_join_lowering_artifact_typed_entry_table(
            artifact, source_kind_id, component_family_kind);

    return age_graph_join_lowering_artifact_declared_entry_table(
        artifact, source_kind_id, component_family_kind);
}

static bool graph_pattern_registry_has_text(List *items, const char *value)
{
    ListCell *lc;

    if (value == NULL)
        return false;

    foreach(lc, items)
    {
        char *existing = lfirst(lc);

        if (existing != NULL && strcmp(existing, value) == 0)
            return true;
    }

    return false;
}

static void graph_pattern_registry_add_text(List **items, const char *value)
{
    MemoryContext oldcontext;

    if (items == NULL || value == NULL)
        return;

    if (graph_pattern_registry_has_text(*items, value))
        return;

    oldcontext = MemoryContextSwitchTo(TopTransactionContext);
    *items = lappend(*items, pstrdup(value));
    MemoryContextSwitchTo(oldcontext);
}

static char *find_graph_pattern_by_vle_marker(RangeTblEntry *rte)
{
    ListCell *lc;
    const char *aliasname = NULL;

    if (rte == NULL)
        return NULL;

    if (rte->alias != NULL)
        aliasname = rte->alias->aliasname;
    else if (rte->eref != NULL)
        aliasname = rte->eref->aliasname;
    if (aliasname == NULL)
        return NULL;

    foreach(lc, graph_pattern_registry)
    {
        CypherGraphPatternRegistryEntry *entry = lfirst(lc);

        if (entry != NULL &&
            graph_pattern_registry_has_text(entry->vle_marker_aliases,
                                            aliasname))
        {
            return entry->graph_pattern_key;
        }
    }

    return NULL;
}

static void register_index_pattern_reference(Oid index_oid,
                                             const char *graph_pattern_key)
{
    MemoryContext oldcontext;
    CypherGraphPatternRegistryEntry *entry;

    if (!OidIsValid(index_oid) || graph_pattern_key == NULL)
        return;

    entry = ensure_graph_pattern_registry_entry(graph_pattern_key);
    if (entry == NULL || list_member_oid(entry->index_oids, index_oid))
        return;
    oldcontext = MemoryContextSwitchTo(TopTransactionContext);
    entry->index_oids = lappend_oid(entry->index_oids, index_oid);
    MemoryContextSwitchTo(oldcontext);
}

static char *find_graph_pattern_by_node_alias(const char *node_alias)
{
    ListCell *lc;

    if (node_alias == NULL)
        return NULL;

    foreach(lc, graph_pattern_registry)
    {
        CypherGraphPatternRegistryEntry *entry = lfirst(lc);

        if (entry != NULL &&
            graph_pattern_registry_has_text(entry->node_aliases, node_alias))
        {
            return entry->graph_pattern_key;
        }
    }

    return NULL;
}

static char *find_graph_pattern_by_index_oid(Oid index_oid)
{
    ListCell *lc;

    if (!OidIsValid(index_oid))
        return NULL;

    foreach(lc, graph_pattern_registry)
    {
        CypherGraphPatternRegistryEntry *entry = lfirst(lc);

        if (entry != NULL && list_member_oid(entry->index_oids, index_oid))
            return entry->graph_pattern_key;
    }

    return NULL;
}

static void bind_adjacency_match_candidate_outer_relids(
    PlannerInfo *root, CypherAdjacencyMatchCandidate *candidate)
{
    ListCell *lc;
    Index rti = 1;
    Index edge_rti = 0;
    Index alias_rti = 0;
    Relids expr_relids = NULL;
    int expr_rti = 0;

    if (root == NULL ||
        root->parse == NULL ||
        candidate == NULL)
    {
        return;
    }

    if (candidate->edge_alias != NULL ||
        candidate->bound_endpoint_alias != NULL)
    {
        foreach(lc, root->parse->rtable)
        {
            RangeTblEntry *rte = lfirst(lc);
            const char *aliasname = NULL;

            if (rte->alias != NULL)
                aliasname = rte->alias->aliasname;
            else if (rte->eref != NULL)
                aliasname = rte->eref->aliasname;

            if (candidate->edge_alias != NULL &&
                aliasname != NULL &&
                strcmp(candidate->edge_alias, aliasname) == 0)
                edge_rti = rti;

            if (candidate->bound_endpoint_alias != NULL &&
                aliasname != NULL &&
                strcmp(candidate->bound_endpoint_alias, aliasname) == 0)
                alias_rti = rti;

            if ((candidate->edge_alias == NULL || edge_rti > 0) &&
                (candidate->bound_endpoint_alias == NULL || alias_rti > 0))
                break;

            rti++;
        }
    }
    candidate->edge_rti = edge_rti;
    if (edge_rti > 0)
        candidate->solved_relids = bms_make_singleton(edge_rti);

    if (candidate->bound_endpoint_expr != NULL)
    {
        expr_relids = pull_varnos(root, candidate->bound_endpoint_expr);
        if (!bms_is_empty(expr_relids))
        {
            if (bms_membership(expr_relids) == BMS_SINGLETON)
                expr_rti = bms_singleton_member(expr_relids);

            /*
             * Only trust the bound-endpoint expression's own varno when it
             * agrees with the alias-resolved rti (or there is no alias to cross
             * check).  The expression is captured at parse time and stored on
             * the candidate outside the query tree, so its varno is NOT
             * remapped during subquery pull-up/flattening.  For a single-edge
             * pattern the parse-time and plan-time indexes coincide, but for a
             * multi-hop pattern they diverge: the stale varno then points at an
             * unrelated intervening rel (e.g. an earlier edge), which would make
             * required_outer reference the wrong relation and leave the bound
             * endpoint unbound at execution.  When they disagree, fall through
             * to the alias path below, which OffsetVarNodes the expression onto
             * the correct (alias-resolved) rti.
             */
            if (expr_rti > 0 &&
                expr_rti < root->simple_rel_array_size &&
                root->simple_rel_array[expr_rti] != NULL &&
                (alias_rti == 0 || expr_rti == alias_rti))
            {
                candidate->required_outer = expr_relids;
                candidate->bound_endpoint_rti = expr_rti;
                if (candidate->solved_relids != NULL)
                    candidate->solved_relids = bms_add_members(
                        candidate->solved_relids, expr_relids);
                else
                    candidate->solved_relids = bms_copy(expr_relids);
                restrict_adjacency_match_terminal_property_prefetch(root,
                                                                    candidate);
                return;
            }
        }
    }

    if (alias_rti == 0)
        return;

    if (expr_rti > 0 &&
        alias_rti != expr_rti &&
        candidate->bound_endpoint_expr != NULL)
    {
        OffsetVarNodes(candidate->bound_endpoint_expr,
                       (int)alias_rti - expr_rti, 0);
    }

    candidate->bound_endpoint_rti = alias_rti;
    candidate->required_outer = bms_make_singleton(alias_rti);
    if (candidate->solved_relids != NULL)
        candidate->solved_relids = bms_add_member(candidate->solved_relids,
                                                  alias_rti);
    else
        candidate->solved_relids = bms_make_singleton(alias_rti);
    restrict_adjacency_match_terminal_property_prefetch(root, candidate);
}

static void restrict_adjacency_match_terminal_property_prefetch(
    PlannerInfo *root, CypherAdjacencyMatchCandidate *candidate)
{
    Relids value_relids;

    if (root == NULL ||
        candidate == NULL ||
        candidate->right_property_value_expr == NULL)
    {
        return;
    }

    value_relids = pull_varnos(root, candidate->right_property_value_expr);
    if (!bms_is_empty(value_relids) &&
        !bms_is_subset(value_relids, candidate->required_outer))
    {
        candidate->right_property_value_expr = NULL;
        candidate->right_property_value = NULL;
        candidate->right_property_index_metadata_backed = false;
        candidate->right_property_index_oid = InvalidOid;
        candidate->right_property_prefetch_eligible = false;
        candidate->right_property_value_kind_id =
            AGE_ADJACENCY_MATCH_VALUE_NONE;
    }
    else
    {
        candidate->right_property_value_kind_id =
            adjacency_match_terminal_property_value_kind(candidate);
        candidate->right_property_prefetch_eligible =
            adjacency_match_has_terminal_property_prefetch(candidate);
    }
    candidate->terminal_source_strategy_id =
        adjacency_match_terminal_source_strategy(candidate);
    bms_free(value_relids);
}

static void add_adjacency_match_custom_path(
    PlannerInfo *root, RelOptInfo *rel,
    CypherAdjacencyMatchCandidate *candidate)
{
    CustomPath *cp;
    Expr *key_expr;
    Const *endpoint_const;
    Relids path_required_outer;
    AdjacencyMatchPayloadRequest payload_request;
    AgeGraphJoinCandidateTable *graph_join_table;
    AgeGraphJoinCandidate *graph_join_candidate;
    int partial_workers = 0;
    bool can_add_partial;

    if (candidate == NULL ||
        candidate->bound_endpoint_expr == NULL ||
        candidate->required_outer == NULL ||
        adjacency_match_bound_expr_uses_age_id(candidate->bound_endpoint_expr))
    {
        return;
    }

    key_expr = (Expr *)candidate->bound_endpoint_expr;
    endpoint_const = find_endpoint_graphid_const(root, candidate);
    if (endpoint_const != NULL)
        key_expr = (Expr *)endpoint_const;
    path_required_outer = endpoint_const != NULL ? NULL :
        candidate->required_outer;
    payload_request = build_adjacency_match_payload_request(
        rel->relid, rel->reltarget->exprs, rel->baserestrictinfo);

    cp = makeNode(CustomPath);
    cp->path.pathtype = T_CustomScan;
    cp->path.parent = rel;
    cp->path.pathtarget = rel->reltarget;
    cp->path.param_info =
        get_baserel_parampathinfo(root, rel, path_required_outer);
    cp->path.parallel_aware = false;
    cp->path.parallel_safe = path_required_outer == NULL &&
        rel->consider_parallel;
    cp->path.parallel_workers = 0;
    cp->path.pathkeys = NIL;

    cost_adjacency_match_custom_path(root, rel, cp, candidate,
                                     &payload_request, endpoint_const);
    prepare_adjacency_match_graph_join_artifact(root, rel, candidate,
                                                &payload_request,
                                                path_required_outer);
    graph_join_table = bind_adjacency_match_graph_join_artifact(
        root, cp, candidate, true);
    Assert(graph_join_table != NULL);
    graph_join_candidate = age_graph_join_table_select_cheapest(
        graph_join_table);
    Assert(graph_join_candidate != NULL);
    (void) age_graph_join_apply_selected_path_cost(graph_join_table,
                                                   &cp->path);
    cp->flags = CUSTOMPATH_SUPPORT_PROJECTION;
    cp->custom_paths = NIL;
    cp->custom_private = list_make5(
        key_expr,
        candidate->right_property_value_expr != NULL ?
        copyObject(candidate->right_property_value_expr) :
        (Node *)makeNullConst(AGTYPEOID, -1, InvalidOid),
        makeConst(OIDOID, -1, InvalidOid, sizeof(Oid),
                  ObjectIdGetDatum(candidate->index_oid), false, true),
        makeConst(BOOLOID, -1, InvalidOid, sizeof(bool),
                  BoolGetDatum(candidate->outgoing), false, true),
        make_adjacency_match_descriptor(candidate, &payload_request,
                                        graph_join_table,
                                        graph_join_candidate));
    cp->methods = &age_adjacency_match_path_methods;

    add_path(rel, (Path *)cp);
    can_add_partial = adjacency_match_can_add_partial_path(rel, candidate,
                                                           &payload_request,
                                                           path_required_outer,
                                                           &partial_workers);
    ereport(DEBUG2,
            (errmsg_internal("AGE adjacency MATCH partial path check: "
                             "edge_rel=%u reloptkind=%d consider_parallel=%s "
                             "path_required_outer=%s terminal_fanout=%.0f "
                             "payload_properties=%s edge_variable=%s "
                             "edge_props=%s right_props=%s can_add=%s "
                             "workers=%d",
                             candidate->edge_label_oid,
                             (int)rel->reloptkind,
                             rel->consider_parallel ? "true" : "false",
                             path_param_info_string(&cp->path),
                             candidate->estimated_terminal_fanout,
                             payload_request.fetch_properties ?
                             "true" : "false",
                             candidate->has_edge_variable_projection ?
                             "true" : "false",
                             candidate->has_edge_property_predicate ?
                             "true" : "false",
                             candidate->has_right_property_predicate ?
                             "true" : "false",
                             can_add_partial ? "true" : "false",
                             partial_workers)));
    if (can_add_partial)
    {
        CustomPath *partial_path;
        Path *gather_path;
        AgeGraphJoinCandidateTable *partial_graph_join_table;
        AgeGraphJoinCandidate *partial_graph_join_candidate;
        Cost partial_gather_cost;

        partial_path = make_adjacency_match_partial_custom_path(
            rel, cp, partial_workers);
        partial_graph_join_table = bind_adjacency_match_graph_join_artifact(
            root, partial_path, candidate, false);
        Assert(partial_graph_join_table != NULL);
        partial_gather_cost = parallel_setup_cost +
            parallel_tuple_cost * cp->path.rows;
        apply_adjacency_match_graph_join_parallel_costs(
            partial_graph_join_table,
            adjacency_match_parallel_divisor(partial_workers),
            partial_gather_cost);
        apply_adjacency_match_graph_join_parallel_properties(
            partial_graph_join_table, true, true, partial_workers,
            partial_gather_cost,
            candidate->has_right_property_predicate);
        partial_graph_join_candidate = age_graph_join_table_select_cheapest(
            partial_graph_join_table);
        Assert(partial_graph_join_candidate != NULL);
        set_adjacency_match_custom_private_descriptor(
            partial_path,
            make_adjacency_match_descriptor(candidate, &payload_request,
                                            partial_graph_join_table,
                                            partial_graph_join_candidate));
        add_partial_path(rel, (Path *)partial_path);
        gather_path = add_adjacency_match_partial_gather_path(
            root, rel, partial_path, cp->path.rows);
        if (gather_path != NULL &&
            candidate->has_right_property_predicate &&
            gather_path->total_cost < cp->path.total_cost)
        {
            /* Keep serial replay as fallback without hiding the cheaper DSM plan. */
            cp->path.disabled_nodes++;
        }
        age_graph_join_register_rel_lowering_table(
            root, rel, &partial_path->path, partial_graph_join_table);
        if (gather_path != NULL)
            age_graph_join_register_rel_lowering_table(
                root, rel, gather_path, partial_graph_join_table);
    }

    ereport(DEBUG2,
            (errmsg_internal("AGE adjacency MATCH CustomPath added: "
                             "edge_rel=%u index=%u required_outer=%s "
                             "path_required_outer=%s endpoint_const=%s "
                             "edge_variable=%s edge_props=%s right_props=%s "
                             "rows=%.0f total_cost=%.2f",
                             candidate->edge_label_oid,
                             candidate->index_oid,
                             bmsToString(candidate->required_outer),
                             path_param_info_string(&cp->path),
                             endpoint_const != NULL ? "true" : "false",
                             candidate->has_edge_variable_projection ?
                             "true" : "false",
                             candidate->has_edge_property_predicate ?
                             "true" : "false",
                             candidate->has_right_property_predicate ?
                             "true" : "false",
                             cp->path.rows,
                             cp->path.total_cost)));
}

static bool adjacency_match_can_add_partial_path(
    RelOptInfo *rel, CypherAdjacencyMatchCandidate *candidate,
    const AdjacencyMatchPayloadRequest *payload_request,
    Relids path_required_outer, int *parallel_workers)
{
    double index_pages;

    if (parallel_workers != NULL)
        *parallel_workers = 0;
    if (rel == NULL || candidate == NULL || payload_request == NULL)
        return false;
    if (!rel->consider_parallel || path_required_outer != NULL)
        return false;
    if (payload_request->fetch_properties)
        return false;
    if (candidate->estimated_source_fanout < 2.0)
        return false;
    if (candidate->has_edge_property_predicate ||
        candidate->has_edge_variable_projection)
    {
        return false;
    }
    if (candidate->has_right_property_predicate &&
        (candidate->right_property_value_kind_id !=
             AGE_ADJACENCY_MATCH_VALUE_CONST ||
         (candidate->source_handoff_kind_id !=
              AGE_ADJACENCY_MATCH_SOURCE_HANDOFF_PROPERTY_SET &&
          candidate->source_handoff_kind_id !=
              AGE_ADJACENCY_MATCH_SOURCE_HANDOFF_LABEL_PROPERTY_INTERSECTION) ||
         candidate->estimated_property_source_matches <= 0 ||
         candidate->estimated_property_source_matches > 65536.0))
    {
        return false;
    }
    if (candidate->estimated_source_blocks > 0)
        index_pages = Max(1.0, candidate->estimated_source_blocks);
    else
        index_pages =
            Max(1.0, ceil(candidate->estimated_source_fanout / 128.0));
    if (parallel_workers != NULL)
    {
        *parallel_workers = compute_parallel_worker(
            rel, -1, index_pages, max_parallel_workers_per_gather);
        if (*parallel_workers <= 0 &&
            candidate->estimated_source_fanout >= 2.0)
        {
            *parallel_workers = 1;
        }
    }

    return parallel_workers != NULL && *parallel_workers > 0;
}

static double adjacency_match_parallel_divisor(int parallel_workers)
{
    Assert(parallel_workers > 0);

    return (double)parallel_workers + 0.5;
}

static CustomPath *make_adjacency_match_partial_custom_path(
    RelOptInfo *rel, CustomPath *serial_path, int parallel_workers)
{
    CustomPath *partial_path;
    double divisor;

    Assert(rel != NULL);
    Assert(serial_path != NULL);
    Assert(parallel_workers > 0);

    partial_path = makeNode(CustomPath);
    partial_path->path = serial_path->path;
    partial_path->path.parallel_safe = true;
    partial_path->path.parallel_aware = true;
    partial_path->path.parallel_workers = parallel_workers;
    divisor = adjacency_match_parallel_divisor(parallel_workers);
    partial_path->path.rows =
        clamp_row_est(Max(serial_path->path.rows / divisor, 1.0));
    partial_path->path.total_cost =
        serial_path->path.startup_cost +
        (serial_path->path.total_cost - serial_path->path.startup_cost) /
        divisor;
    partial_path->flags = serial_path->flags;
    partial_path->custom_paths = copyObject(serial_path->custom_paths);
    partial_path->custom_private = copyObject(serial_path->custom_private);
    partial_path->methods = serial_path->methods;

    return partial_path;
}

static void set_adjacency_match_custom_private_descriptor(
    CustomPath *path, List *descriptor)
{
    ListCell *cell;

    Assert(path != NULL);
    Assert(path->custom_private != NIL);
    Assert(descriptor != NIL);

    cell = list_nth_cell(path->custom_private,
                         list_length(path->custom_private) - 1);
    lfirst(cell) = descriptor;
}

static Path *add_adjacency_match_partial_gather_path(PlannerInfo *root,
                                                     RelOptInfo *rel,
                                                     CustomPath *partial_path,
                                                     double gathered_rows)
{
    Path *gather_path;
    ListCell *lc;

    if (root == NULL ||
        rel == NULL ||
        partial_path == NULL)
    {
        return NULL;
    }

    gather_path = (Path *)create_gather_path(root, rel,
                                             (Path *)partial_path,
                                             rel->reltarget, NULL, NULL);
    gather_path->rows = clamp_row_est(gathered_rows);
    add_path(rel, gather_path);
    foreach(lc, rel->pathlist)
    {
        if (lfirst(lc) == gather_path)
            return gather_path;
    }

    return NULL;
}

static Const *find_endpoint_graphid_const(PlannerInfo *root,
                                          CypherAdjacencyMatchCandidate *candidate)
{
    RelOptInfo *endpoint_rel;
    ListCell *lc;

    if (candidate->bound_endpoint_rti <= 0 ||
        candidate->bound_endpoint_rti >= root->simple_rel_array_size)
        return NULL;

    endpoint_rel = root->simple_rel_array[candidate->bound_endpoint_rti];
    if (endpoint_rel == NULL)
        return NULL;

    /*
     * When a previous-clause vertex has id(vertex) = const, the bound endpoint
     * expression can still refer to a hidden raw target Var.  Use the base
     * relation's id restriction as the CustomScan key so executor setrefs do
     * not evaluate that hidden Var against the wrong slot.
     */
    foreach(lc, endpoint_rel->baserestrictinfo)
    {
        RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc);
        OpExpr *op;
        Node *left;
        Node *right;

        if (!IsA(rinfo->clause, OpExpr))
            continue;

        op = castNode(OpExpr, rinfo->clause);
        if (list_length(op->args) != 2)
            continue;

        left = linitial(op->args);
        right = lsecond(op->args);

        if (IsA(left, Var) && IsA(right, Const))
        {
            Var *var = castNode(Var, left);
            Const *con = castNode(Const, right);

            if (var->varno == candidate->bound_endpoint_rti &&
                var->varattno == Anum_ag_label_vertex_table_id &&
                var->vartype == GRAPHIDOID &&
                con->consttype == GRAPHIDOID &&
                !con->constisnull)
                return copyObject(con);
        }
        else if (IsA(left, Const) && IsA(right, Var))
        {
            Const *con = castNode(Const, left);
            Var *var = castNode(Var, right);

            if (var->varno == candidate->bound_endpoint_rti &&
                var->varattno == Anum_ag_label_vertex_table_id &&
                var->vartype == GRAPHIDOID &&
                con->consttype == GRAPHIDOID &&
                !con->constisnull)
                return copyObject(con);
        }
    }

    return NULL;
}

static bool adjacency_match_bound_expr_uses_age_id(Node *node)
{
    bool found = false;

    (void)adjacency_match_bound_expr_uses_age_id_walker(node, &found);

    return found;
}

static bool adjacency_match_bound_expr_uses_age_id_walker(Node *node,
                                                          void *context)
{
    bool *found = context;

    if (node == NULL || *found)
        return false;

    if (IsA(node, FuncExpr))
    {
        FuncExpr *func = (FuncExpr *)node;
        Oid age_id_oid = get_ag_func_oid("age_id", 1, AGTYPEOID);

        if (func->funcid == age_id_oid)
        {
            *found = true;
            return false;
        }
    }

    return expression_tree_walker(node,
                                  adjacency_match_bound_expr_uses_age_id_walker,
                                  context);
}

static void cost_adjacency_match_custom_path(PlannerInfo *root,
                                             RelOptInfo *rel,
                                             CustomPath *cp,
                                             CypherAdjacencyMatchCandidate *candidate,
                                             const AdjacencyMatchPayloadRequest *payload_request,
                                             Const *endpoint_const)
{
    double rows;
    VLESourceFanoutEvidence source_evidence;
    double endpoint_fanout;
    double terminal_fanout;
    double composite_fanout;
    double pages;
    double estimated_payload_rows;
    double page_probe_cost;
    Cost property_source_build_cost;
    double heap_recheck_cost;
    double right_property_recheck_cost;
    double cpu_cost;
    double residual_weight;
    double index_solved_credit;
    int residual_count;
    int index_solved_count;
    bool edge_payload_required;
    Cost local_random_page_cost;
    Cost local_seq_page_cost;

    rows = cp->path.param_info != NULL ? cp->path.param_info->ppi_rows :
                                         rel->rows;
    rows = clamp_row_est(rows);
    if (rows < 1)
        rows = 1;

    pages = rel->pages > 0 ? rel->pages : 1;
    estimate_vle_source_fanout_evidence(&source_evidence,
                                        candidate->edge_label_oid);
    if (source_evidence.reltuples <= 0 && rel->tuples > 0)
        source_evidence.reltuples = rel->tuples;
    endpoint_fanout = select_vle_source_fanout_for_endpoint(
        &source_evidence, candidate->endpoint_attno);
    if (endpoint_fanout <= 0)
        endpoint_fanout = Min(rows, 8.0);

    /*
     * Condition the base (endpoint) fanout on the cached per-source-label
     * average out-degree when the degree cache has been refreshed: a mean
     * degree conditioned on the source vertex label, more accurate than the
     * global reltuples/distinct estimate.  Opt-in -- no degree cache leaves the
     * estimate unchanged.
     */
    {
        label_cache_data *edge_label_cache =
            search_label_relation_cache(candidate->edge_label_oid);

        if (edge_label_cache != NULL &&
            candidate->bound_endpoint_rti > 0 &&
            candidate->bound_endpoint_rti < root->simple_rel_array_size)
        {
            RangeTblEntry *src_rte =
                root->simple_rte_array[candidate->bound_endpoint_rti];

            if (src_rte != NULL && src_rte->rtekind == RTE_RELATION)
            {
                label_cache_data *src_label_cache =
                    search_label_relation_cache(src_rte->relid);

                if (src_label_cache != NULL &&
                    src_label_cache->graph == edge_label_cache->graph)
                {
                    /*
                     * The bound endpoint is the source for an outgoing
                     * expansion (its out-degree is the fanout) but the terminal
                     * for an incoming expansion (its in-degree is the fanout),
                     * so pick the matching directional cache.
                     */
                    double cached_degree = candidate->outgoing ?
                        age_cached_edge_avg_out_degree(
                            edge_label_cache->graph,
                            NameStr(src_label_cache->name),
                            NameStr(edge_label_cache->name)) :
                        age_cached_edge_avg_in_degree(
                            edge_label_cache->graph,
                            NameStr(src_label_cache->name),
                            NameStr(edge_label_cache->name));

                    if (cached_degree > 0.0)
                        endpoint_fanout = cached_degree;
                }
            }
        }
    }

    terminal_fanout = endpoint_fanout;
    candidate->estimated_terminal_label_groups = 0;
    candidate->estimated_value_posting_source_kind =
        VLE_SOURCE_VALUE_POSTING_NONE;
    candidate->estimated_main_blocks = 0;
    candidate->estimated_fanout_from_directory = false;
    if (endpoint_const != NULL &&
        endpoint_const->consttype == GRAPHIDOID &&
        !endpoint_const->constisnull)
    {
        int64 run_postings;
        int64 terminal_postings;
        int64 label_groups;
        int64 main_blocks;
        VLESourceValuePostingKind value_posting_source_kind;

        if (age_adjacency_estimate_terminal_label_postings(
                candidate->index_oid,
                (graphid)DatumGetInt64(endpoint_const->constvalue),
                candidate->right_label_id,
                &run_postings, &terminal_postings, &label_groups,
                &value_posting_source_kind, &main_blocks))
        {
            endpoint_fanout = (double)run_postings;
            if (candidate->has_right_label_constraint &&
                label_id_is_valid(candidate->right_label_id))
            {
                terminal_fanout = (double)terminal_postings;
            }
            else
            {
                terminal_fanout = endpoint_fanout;
            }
            candidate->estimated_terminal_label_groups =
                (double)label_groups;
            candidate->estimated_value_posting_source_kind =
                value_posting_source_kind;
            candidate->estimated_main_blocks = (double)main_blocks;
            candidate->estimated_fanout_from_directory = true;
        }
    }
    else if (candidate->has_right_label_constraint &&
             label_id_is_valid(candidate->right_label_id))
    {
        double dst_label_selectivity = -1.0;
        label_cache_data *edge_label_cache =
            search_label_relation_cache(candidate->edge_label_oid);

        if (edge_label_cache != NULL)
        {
            label_cache_data *dst_label_cache =
                search_label_graph_oid_cache(edge_label_cache->graph,
                                             candidate->right_label_id);

            if (dst_label_cache != NULL)
            {
                const char *src_label_name = NULL;
                label_cache_data *src_label_cache = NULL;

                /*
                 * Source vertex label of the bound endpoint, when its relation
                 * is a label table in this graph.  The cached triple
                 * (src_label, edge_label, dst_label) conditions the fanout on
                 * the source label -- the dimension the adjacency directory
                 * cannot express (it keys only on edge_label -> dst_label).
                 * When the source label is not recoverable the lookup falls
                 * back to the all-source ratio.
                 */
                if (candidate->bound_endpoint_rti > 0 &&
                    candidate->bound_endpoint_rti < root->simple_rel_array_size)
                {
                    RangeTblEntry *src_rte =
                        root->simple_rte_array[candidate->bound_endpoint_rti];

                    if (src_rte != NULL && src_rte->rtekind == RTE_RELATION)
                        src_label_cache =
                            search_label_relation_cache(src_rte->relid);
                }
                if (src_label_cache != NULL &&
                    src_label_cache->graph == edge_label_cache->graph)
                    src_label_name = NameStr(src_label_cache->name);

                dst_label_selectivity = age_cached_edge_dst_label_selectivity(
                    edge_label_cache->graph,
                    src_label_name,
                    NameStr(edge_label_cache->name),
                    NameStr(dst_label_cache->name));
            }
        }

        /*
         * Condition the destination-label fanout on the cached triple
         * selectivity from ag_graph_edge_stats when the graph statistics cache
         * has been refreshed; otherwise keep the 0.50 default so an
         * un-refreshed graph's plans are unchanged.
         */
        terminal_fanout = Max(1.0, terminal_fanout *
                              (dst_label_selectivity >= 0.0 ?
                               dst_label_selectivity : 0.50));
    }
    candidate->estimated_endpoint_fanout = endpoint_fanout;
    candidate->estimated_terminal_fanout = terminal_fanout;
    candidate->estimated_composite_selectivity = 0.0;
    candidate->estimated_composite_selectivity_source_kind =
        AGE_GRAPH_PROPERTY_SELECTIVITY_NONE;

    rows = clamp_row_est(Min(rows, terminal_fanout));
    if (rows < 1)
        rows = 1;
    composite_fanout = rows;
    if (adjacency_match_plans_terminal_property_prefetch(candidate,
                                                        payload_request))
    {
        GraphPropertySourceSelectivity property_selectivity;
        double fallback_selectivity;

        fallback_selectivity =
            candidate->has_right_label_constraint ? 0.15 : 0.25;
        property_selectivity = estimate_graph_property_source_selectivity(
            candidate->right_property_index_oid, fallback_selectivity,
            candidate->right_property_value);
        candidate->estimated_composite_selectivity =
            property_selectivity.selectivity;
        candidate->estimated_composite_selectivity_source_kind =
            property_selectivity.source_kind;
        candidate->estimated_property_source_matches = clamp_row_est(
            Max(1.0,
                get_vle_relation_estimated_tuples(
                    candidate->right_property_index_oid) *
                property_selectivity.selectivity));
        rows = clamp_row_est(Max(1.0, rows * property_selectivity.selectivity));
        composite_fanout = rows;
    }
    else
    {
        candidate->estimated_property_source_matches = 0;
    }
    candidate->estimated_composite_fanout = composite_fanout;
    estimate_adjacency_match_source_handoff(candidate, payload_request);
    estimated_payload_rows = rows;
    if (payload_request != NULL)
        edge_payload_required = payload_request->fetch_properties;
    else
        edge_payload_required = adjacency_match_requires_edge_payload(
            candidate);
    residual_count = adjacency_match_residual_predicate_count(candidate);
    index_solved_count =
        adjacency_match_index_solved_predicate_count(candidate,
                                                     payload_request);

    get_tablespace_page_costs(rel->reltablespace, &local_random_page_cost,
                              &local_seq_page_cost);

    /*
     * age_adjacency v3 lookup performs a small directory probe and then scans
     * the endpoint posting run. Charge a bounded page probe plus heap
     * visibility rechecks for the estimated payload rows.
     */
    if (candidate->estimated_source_blocks > 0)
        pages = Min(pages, candidate->estimated_source_blocks);
    page_probe_cost = Min(pages, 4.0) * local_random_page_cost * 0.03;
    property_source_build_cost = adjacency_match_property_source_build_cost(
        candidate, payload_request);
    heap_recheck_cost = estimated_payload_rows * local_random_page_cost *
        (edge_payload_required ? 0.035 : 0.012);
    right_property_recheck_cost = 0;
    if (candidate->has_right_property_predicate)
    {
        right_property_recheck_cost = estimated_payload_rows * cpu_tuple_cost *
            (adjacency_match_plans_terminal_property_prefetch(
                candidate, payload_request) ?
             0.05 : 1.25);
    }
    residual_weight = 1.0 + (0.35 * residual_count);
    index_solved_credit = Max(0.70, 1.0 - (0.10 * index_solved_count));
    cpu_cost = estimated_payload_rows * cpu_tuple_cost *
        (edge_payload_required ? 3.0 : 1.4) * residual_weight *
        index_solved_credit;

    cp->path.rows = rows;
    cp->path.startup_cost = page_probe_cost + property_source_build_cost;
    cp->path.total_cost = cp->path.startup_cost + heap_recheck_cost +
        right_property_recheck_cost + cpu_cost;

    (void) root;
    (void) local_seq_page_cost;
}

static bool adjacency_match_requires_edge_payload(
    const CypherAdjacencyMatchCandidate *candidate)
{
    return candidate != NULL &&
        (candidate->has_edge_variable_projection ||
         candidate->has_edge_property_predicate);
}

static bool adjacency_match_has_terminal_property_prefetch(
    const CypherAdjacencyMatchCandidate *candidate)
{
    return candidate != NULL &&
        candidate->has_right_property_predicate &&
        candidate->right_property_index_metadata_backed &&
        OidIsValid(candidate->right_property_index_oid) &&
        candidate->right_property_value_expr != NULL &&
        (candidate->right_property_value == NULL ||
         !candidate->right_property_value->constisnull);
}

static AgeAdjacencyMatchValueKind
adjacency_match_terminal_property_value_kind(
    const CypherAdjacencyMatchCandidate *candidate)
{
    if (candidate == NULL || candidate->right_property_value_expr == NULL)
        return AGE_ADJACENCY_MATCH_VALUE_NONE;
    if (candidate->right_property_value != NULL)
        return AGE_ADJACENCY_MATCH_VALUE_CONST;
    return AGE_ADJACENCY_MATCH_VALUE_RUNTIME_SLOT;
}

static AgeAdjacencyMatchTerminalStrategy
adjacency_match_terminal_source_strategy(
    const CypherAdjacencyMatchCandidate *candidate)
{
    bool label_prune;
    bool property_prefetch;

    if (candidate == NULL)
        return AGE_ADJACENCY_MATCH_TERMINAL_STRATEGY_NONE;

    label_prune = candidate->has_right_label_constraint &&
        label_id_is_valid(candidate->right_label_id);
    property_prefetch = adjacency_match_has_terminal_property_prefetch(
        candidate);

    if (label_prune && property_prefetch)
        return AGE_ADJACENCY_MATCH_TERMINAL_STRATEGY_LABEL_PROPERTY;
    if (label_prune)
        return AGE_ADJACENCY_MATCH_TERMINAL_STRATEGY_LABEL_BLOCK;
    if (property_prefetch)
        return AGE_ADJACENCY_MATCH_TERMINAL_STRATEGY_PROPERTY_SOURCE;
    if (candidate->has_right_property_predicate)
        return AGE_ADJACENCY_MATCH_TERMINAL_STRATEGY_PROPERTY_RECHECK;

    return AGE_ADJACENCY_MATCH_TERMINAL_STRATEGY_NONE;
}

static AgeAdjacencyMatchSourceHandoff
adjacency_match_source_handoff_kind(
    const CypherAdjacencyMatchCandidate *candidate,
    const AdjacencyMatchPayloadRequest *payload_request)
{
    bool label_slice;
    bool property_set;

    Assert(candidate != NULL);

    label_slice = candidate->has_right_label_constraint &&
        label_id_is_valid(candidate->right_label_id);
    property_set = adjacency_match_plans_terminal_property_prefetch(
        candidate, payload_request);

    if (label_slice && property_set)
        return AGE_ADJACENCY_MATCH_SOURCE_HANDOFF_LABEL_PROPERTY_INTERSECTION;
    if (property_set)
        return AGE_ADJACENCY_MATCH_SOURCE_HANDOFF_PROPERTY_SET;
    if (label_slice)
        return AGE_ADJACENCY_MATCH_SOURCE_HANDOFF_LABEL_SLICE;
    if (candidate->has_right_property_predicate)
        return AGE_ADJACENCY_MATCH_SOURCE_HANDOFF_RECHECK;

    return AGE_ADJACENCY_MATCH_SOURCE_HANDOFF_POSTING_RUN;
}

static void
estimate_adjacency_match_source_handoff(
    CypherAdjacencyMatchCandidate *candidate,
    const AdjacencyMatchPayloadRequest *payload_request)
{
    double source_ratio;

    Assert(candidate != NULL);

    candidate->source_handoff_kind_id =
        adjacency_match_source_handoff_kind(candidate, payload_request);
    if (candidate->source_handoff_kind_id ==
            AGE_ADJACENCY_MATCH_SOURCE_HANDOFF_PROPERTY_SET ||
        candidate->source_handoff_kind_id ==
            AGE_ADJACENCY_MATCH_SOURCE_HANDOFF_LABEL_PROPERTY_INTERSECTION)
    {
        candidate->estimated_source_fanout =
            candidate->estimated_composite_fanout;
    }
    else if (candidate->source_handoff_kind_id ==
             AGE_ADJACENCY_MATCH_SOURCE_HANDOFF_LABEL_SLICE)
    {
        candidate->estimated_source_fanout =
            candidate->estimated_terminal_fanout;
    }
    else
    {
        candidate->estimated_source_fanout =
            candidate->estimated_endpoint_fanout;
    }

    candidate->estimated_source_fanout =
        Max(candidate->estimated_source_fanout, 1.0);
    if (candidate->estimated_main_blocks <= 0 ||
        candidate->estimated_endpoint_fanout <= 0)
    {
        candidate->estimated_source_blocks = 0;
        return;
    }

    source_ratio = candidate->estimated_source_fanout /
        candidate->estimated_endpoint_fanout;
    candidate->estimated_source_blocks = Max(
        1.0, ceil(candidate->estimated_main_blocks * Min(source_ratio, 1.0)));
}

static int
adjacency_match_terminal_prefetch_threshold(
    const CypherAdjacencyMatchCandidate *candidate,
    const AdjacencyMatchPayloadRequest *payload_request)
{
    bool fetch_properties;

    if (!adjacency_match_has_terminal_property_prefetch(candidate))
        return 0;

    fetch_properties = payload_request != NULL ?
        payload_request->fetch_properties :
        adjacency_match_requires_edge_payload(candidate);

    if (fetch_properties)
        return 2;
    if (candidate->estimated_terminal_fanout >= 16.0)
        return 2;

    return 3;
}

static AgeAdjacencyMatchTerminalPrefetchReason
adjacency_match_terminal_prefetch_reason_kind(
    const CypherAdjacencyMatchCandidate *candidate,
    const AdjacencyMatchPayloadRequest *payload_request)
{
    bool fetch_properties;

    if (!adjacency_match_has_terminal_property_prefetch(candidate))
        return AGE_ADJACENCY_MATCH_PREFETCH_REASON_NOT_INDEXABLE;

    fetch_properties = payload_request != NULL ?
        payload_request->fetch_properties :
        adjacency_match_requires_edge_payload(candidate);

    if (fetch_properties)
        return AGE_ADJACENCY_MATCH_PREFETCH_REASON_EDGE_PAYLOAD_REQUIRED;
    if (candidate->estimated_terminal_fanout >= 16.0)
        return AGE_ADJACENCY_MATCH_PREFETCH_REASON_LARGE_TERMINAL_FANOUT;

    return AGE_ADJACENCY_MATCH_PREFETCH_REASON_SMALL_TERMINAL_FANOUT;
}

static Cost
adjacency_match_property_source_build_cost(
    const CypherAdjacencyMatchCandidate *candidate,
    const AdjacencyMatchPayloadRequest *payload_request)
{
    if (candidate == NULL ||
        !adjacency_match_plans_terminal_property_prefetch(candidate,
                                                          payload_request) ||
        candidate->estimated_property_source_matches <= 0)
    {
        return 0;
    }

    return candidate->estimated_property_source_matches *
        (cpu_index_tuple_cost + cpu_operator_cost);
}

static AgeGraphJoinConnectorKind
adjacency_match_join_order_connector_kind(
    const CypherAdjacencyMatchCandidate *candidate)
{
    if (candidate == NULL)
        return AGE_GRAPH_JOIN_CONNECTOR_UNKNOWN;

    if (candidate->has_right_property_predicate ||
        candidate->has_right_label_constraint)
        return AGE_GRAPH_JOIN_CONNECTOR_ADJACENCY_COMPOSITE_EXPAND;

    return AGE_GRAPH_JOIN_CONNECTOR_ADJACENCY_EXPAND;
}

static AgeGraphJoinBoundKind
adjacency_match_join_order_bound_kind(
    const CypherAdjacencyMatchCandidate *candidate)
{
    if (candidate == NULL)
        return AGE_GRAPH_JOIN_BOUND_UNKNOWN;

    return candidate->outgoing ? AGE_GRAPH_JOIN_BOUND_ADJACENCY_START :
        AGE_GRAPH_JOIN_BOUND_ADJACENCY_END;
}

static bool
adjacency_match_plans_terminal_property_prefetch(
    const CypherAdjacencyMatchCandidate *candidate,
    const AdjacencyMatchPayloadRequest *payload_request)
{
    int threshold;

    if (!adjacency_match_has_terminal_property_prefetch(candidate))
        return false;

    threshold = adjacency_match_terminal_prefetch_threshold(candidate,
                                                           payload_request);
    if (threshold <= 0)
        return false;

    return candidate->estimated_terminal_fanout >= threshold;
}

static int
adjacency_match_residual_predicate_count(
    const CypherAdjacencyMatchCandidate *candidate)
{
    int count = 0;

    if (candidate->has_edge_property_predicate)
        count++;
    if (candidate->has_right_label_constraint)
        count++;
    if (candidate->has_right_property_predicate)
        count++;

    return count;
}

static int
adjacency_match_index_solved_predicate_count(
    const CypherAdjacencyMatchCandidate *candidate,
    const AdjacencyMatchPayloadRequest *payload_request)
{
    int count = 0;

    if (label_id_is_valid(candidate->right_label_id))
        count++;
    if (adjacency_match_plans_terminal_property_prefetch(candidate,
                                                        payload_request))
    {
        count++;
    }

    return count;
}

static AgeGraphJoinLoweringArtifact *make_adjacency_match_lowering_artifact(
    RelOptInfo *rel, CypherAdjacencyMatchCandidate *candidate,
    const AdjacencyMatchPayloadRequest *payload_request,
    Relids path_required_outer)
{
    AgeGraphJoinLoweringArtifact *artifact;
    AgeGraphJoinCandidateTable *node_property_table;
    AgeGraphJoinCandidateTable *adjacency_table;
    AgeGraphJoinLoweringInput lowering_input;
    double scheduled_work;
    Cost scheduled_cost;
    double rows;
    int output_width;
    Cost source_build_cost;

    adjacency_table = graph_pattern_registry_typed_entry_table(
        candidate->graph_pattern_key, AGE_GRAPH_JOIN_SOURCE_ADJACENCY_EXPANSION,
        AGE_GRAPH_JOIN_COMPONENT_ADJACENCY_EXPANSION);
    if (adjacency_table == NULL)
        return NULL;
    lowering_input = make_adjacency_match_lowering_input(
        rel, candidate, path_required_outer);
    scheduled_work = adjacency_match_connector_scheduled_work(candidate);
    source_build_cost = adjacency_match_property_source_build_cost(
        candidate, payload_request);
    scheduled_cost = adjacency_match_connector_scheduled_cost(
        candidate, payload_request, source_build_cost);
    rows = clamp_row_est(Max(scheduled_work, 1.0));
    output_width = lowering_input.output_width;

    if (adjacency_match_has_terminal_property_prefetch(candidate))
    {
        AgeGraphJoinCandidateRequest request;

        node_property_table = graph_pattern_registry_typed_entry_table(
            candidate->graph_pattern_key, AGE_GRAPH_JOIN_SOURCE_ADJACENCY_NODE_PROPERTY,
            AGE_GRAPH_JOIN_COMPONENT_NODE_PROPERTY_SEEK);
        if (node_property_table == NULL)
            return NULL;

        memset(&request, 0, sizeof(request));
        request.display_name = candidate->edge_alias != NULL ?
            candidate->edge_alias : "edge";
        request.component_family_kind =
            AGE_GRAPH_JOIN_COMPONENT_NODE_PROPERTY_SEEK;
        request.connector_kind =
            AGE_GRAPH_JOIN_CONNECTOR_NODE_PROPERTY_INDEX_SEEK;
        request.bound_kind = adjacency_match_join_order_bound_kind(candidate);
        request.order_property_kind = AGE_GRAPH_JOIN_ORDER_INDEX_ANCHORED;
        request.source_evidence_kind =
            AGE_GRAPH_JOIN_SOURCE_EVIDENCE_NODE_PROPERTY_INDEX;
        request.solved_relids = lowering_input.solved_relids;
        request.required_outer = lowering_input.required_outer;
        request.provided_relids = lowering_input.provided_relids;
        request.rows = rows;
        request.startup_cost = source_build_cost;
        request.total_cost = scheduled_cost +
            Max(candidate->estimated_composite_fanout, 1.0) *
            cpu_operator_cost * (payload_request != NULL &&
                                 payload_request->fetch_properties ?
                                 0.18 : 0.08);
        request.output_width = output_width;
        request.parallel_safe = false;
        request.parallel_aware = false;
        request.parallel_workers = 0;
        request.gather_cost = 0;
        request.order_preserving = false;
        request.shared_state_required = false;
        (void) age_graph_join_table_add_candidate(node_property_table,
                                                  &request);

        memset(&request, 0, sizeof(request));
        request.display_name = candidate->edge_alias != NULL ?
            candidate->edge_alias : "edge";
        request.component_family_kind =
            AGE_GRAPH_JOIN_COMPONENT_ADJACENCY_EXPANSION;
        request.connector_kind =
            AGE_GRAPH_JOIN_CONNECTOR_ADJACENCY_VALUE_JOIN;
        request.bound_kind = adjacency_match_join_order_bound_kind(candidate);
        request.order_property_kind = AGE_GRAPH_JOIN_ORDER_INDEX_ANCHORED;
        request.source_evidence_kind =
            AGE_GRAPH_JOIN_SOURCE_EVIDENCE_NODE_PROPERTY_INDEX;
        request.solved_relids = lowering_input.solved_relids;
        request.required_outer = lowering_input.required_outer;
        request.provided_relids = lowering_input.provided_relids;
        request.rows = rows;
        request.startup_cost = source_build_cost;
        request.total_cost = scheduled_cost +
            Max(scheduled_work, 1.0) * cpu_tuple_cost;
        request.output_width = output_width;
        request.parallel_safe = false;
        request.parallel_aware = false;
        request.parallel_workers = 0;
        request.gather_cost = 0;
        request.order_preserving = false;
        request.shared_state_required = false;
        (void) age_graph_join_table_add_candidate(adjacency_table,
                                                  &request);
    }

    {
        AgeGraphJoinCandidateRequest request;

        memset(&request, 0, sizeof(request));
        request.display_name = candidate->edge_alias != NULL ?
            candidate->edge_alias : "edge";
        request.component_family_kind =
            AGE_GRAPH_JOIN_COMPONENT_ADJACENCY_EXPANSION;
        request.connector_kind =
            adjacency_match_join_order_connector_kind(candidate);
        request.bound_kind = adjacency_match_join_order_bound_kind(candidate);
        if (adjacency_match_has_terminal_property_prefetch(candidate))
            request.order_property_kind = AGE_GRAPH_JOIN_ORDER_INDEX_ANCHORED;
        else if (candidate->estimated_fanout_from_directory)
            request.order_property_kind =
                AGE_GRAPH_JOIN_ORDER_ADJACENCY_DIRECTORY;
        else if (candidate->index_metadata_backed)
            request.order_property_kind = AGE_GRAPH_JOIN_ORDER_ADJACENCY;
        else
            request.order_property_kind = AGE_GRAPH_JOIN_ORDER_QUERY;
        request.source_evidence_kind =
            adjacency_match_connector_source_evidence_kind(candidate);
        request.solved_relids = lowering_input.solved_relids;
        request.required_outer = lowering_input.required_outer;
        request.provided_relids = lowering_input.provided_relids;
        request.rows = rows;
        request.startup_cost = source_build_cost;
        request.total_cost = scheduled_cost;
        request.output_width = output_width;
        request.parallel_safe = false;
        request.parallel_aware = false;
        request.parallel_workers = 0;
        request.gather_cost = 0;
        request.order_preserving = false;
        request.shared_state_required = false;
        (void) age_graph_join_table_add_candidate(adjacency_table,
                                                  &request);
    }

    artifact = graph_pattern_registry_artifact(
        candidate->graph_pattern_key != NULL ?
        candidate->graph_pattern_key : "graph-pattern");

    return artifact;
}

static void prepare_adjacency_match_graph_join_artifact(
    PlannerInfo *root, RelOptInfo *rel,
    CypherAdjacencyMatchCandidate *candidate,
    const AdjacencyMatchPayloadRequest *payload_request,
    Relids path_required_outer)
{
    if (root == NULL ||
        rel == NULL ||
        candidate == NULL ||
        candidate->graph_join_artifact != NULL)
    {
        return;
    }

    candidate->graph_join_artifact = make_adjacency_match_lowering_artifact(
        rel, candidate, payload_request, path_required_outer);
    candidate->graph_join_lowering_required_outer = path_required_outer;
    candidate->graph_join_artifact =
        age_graph_join_register_rel_lowering_artifact_pool(
        root, rel, candidate->graph_join_artifact);
    if (candidate->graph_join_artifact == NULL)
        candidate->graph_join_artifact = make_adjacency_match_lowering_artifact(
            rel, candidate, payload_request, path_required_outer);
}

static AgeGraphJoinCandidateTable *bind_adjacency_match_graph_join_artifact(
    PlannerInfo *root, CustomPath *cp, CypherAdjacencyMatchCandidate *candidate,
    bool register_path)
{
    if (cp == NULL ||
        candidate == NULL ||
        candidate->graph_join_artifact == NULL)
    {
        return NULL;
    }

    if (register_path)
    {
        return age_graph_join_register_rel_lowering_artifact_path(
            root, cp->path.parent, &cp->path, candidate->graph_join_artifact);
    }

    return age_graph_join_bind_lowering_artifact_to_path(
        &cp->path, candidate->graph_join_artifact);
}

static AgeGraphJoinLoweringInput make_adjacency_match_lowering_input(
    RelOptInfo *rel, const CypherAdjacencyMatchCandidate *candidate,
    Relids path_required_outer)
{
    AgeGraphJoinLoweringInput input;

    memset(&input, 0, sizeof(input));
    if (candidate != NULL)
        input.solved_relids = candidate->solved_relids;
    input.required_outer = path_required_outer;
    input.provided_relids = rel != NULL ? rel->relids : NULL;
    if (rel != NULL && rel->reltarget != NULL)
        input.output_width = rel->reltarget->width;

    return input;
}

static void apply_adjacency_match_graph_join_parallel_properties(
    AgeGraphJoinCandidateTable *table, bool parallel_safe,
    bool parallel_aware, int parallel_workers, Cost gather_cost,
    bool shared_state_required)
{
    ListCell *lc;

    if (table == NULL)
        return;

    foreach(lc, table->candidates)
    {
        AgeGraphJoinCandidate *candidate = lfirst(lc);

        candidate->component.parallel_safe = parallel_safe;
        candidate->component.parallel_aware =
            parallel_safe && parallel_aware;
        candidate->component.parallel_workers =
            candidate->component.parallel_aware ? parallel_workers : 0;
        candidate->component.gather_cost =
            candidate->component.parallel_safe ? gather_cost : 0;
        candidate->component.shared_state_required =
            candidate->component.parallel_aware && shared_state_required;
    }
}

static void apply_adjacency_match_graph_join_parallel_costs(
    AgeGraphJoinCandidateTable *table, double divisor, Cost gather_cost)
{
    ListCell *lc;

    if (table == NULL || divisor <= 1.0)
        return;

    foreach(lc, table->candidates)
    {
        AgeGraphJoinCandidate *candidate = lfirst(lc);
        Cost run_cost;

        candidate->connector.rows =
            clamp_row_est(Max(candidate->connector.rows / divisor, 1.0));
        candidate->component.estimated_rows = candidate->connector.rows;

        run_cost = Max(candidate->connector.total_cost -
                       candidate->connector.startup_cost, 0.0);
        candidate->connector.total_cost =
            candidate->connector.startup_cost + run_cost / divisor +
            gather_cost;
    }
}

static double adjacency_match_connector_scheduled_work(
    const CypherAdjacencyMatchCandidate *candidate)
{
    double scheduled_work;

    Assert(candidate != NULL);

    scheduled_work = Max(candidate->estimated_composite_fanout, 1.0);
    if (candidate->estimated_fanout_from_directory &&
        candidate->estimated_value_posting_source_kind !=
        VLE_SOURCE_VALUE_POSTING_NONE)
    {
        scheduled_work = Max(1.0, scheduled_work * 0.50);
    }

    return scheduled_work;
}

static Cost adjacency_match_connector_scheduled_cost(
    const CypherAdjacencyMatchCandidate *candidate,
    const AdjacencyMatchPayloadRequest *payload_request,
    Cost startup_cost)
{
    double scheduled_work;
    double payload_weight;
    double residual_weight;
    double index_credit;
    int residual_count;
    int index_solved_count;
    bool fetch_properties;

    Assert(candidate != NULL);

    scheduled_work = adjacency_match_connector_scheduled_work(candidate);
    fetch_properties = payload_request != NULL ?
        payload_request->fetch_properties :
        adjacency_match_requires_edge_payload(candidate);
    payload_weight = fetch_properties ? 1.65 : 0.70;
    residual_count = adjacency_match_residual_predicate_count(candidate);
    index_solved_count =
        adjacency_match_index_solved_predicate_count(candidate,
                                                     payload_request);
    residual_weight = 1.0 + (0.20 * residual_count);
    index_credit = Max(0.60, 1.0 - (0.12 * index_solved_count));

    if (candidate->estimated_fanout_from_directory)
        index_credit = Min(index_credit, 0.82);
    if (adjacency_match_plans_terminal_property_prefetch(candidate,
                                                        payload_request))
    {
        index_credit = Min(index_credit, 0.72);
    }

    return startup_cost +
        scheduled_work * cpu_operator_cost * 0.12 * residual_weight *
        index_credit +
        scheduled_work * cpu_tuple_cost * payload_weight * residual_weight *
        index_credit;
}

static AgeGraphJoinSourceEvidenceKind
adjacency_match_connector_source_evidence_kind(
    const CypherAdjacencyMatchCandidate *candidate)
{
    Assert(candidate != NULL);

    if (candidate->estimated_fanout_from_directory &&
        candidate->estimated_value_posting_source_kind !=
        VLE_SOURCE_VALUE_POSTING_NONE)
    {
        return AGE_GRAPH_JOIN_SOURCE_EVIDENCE_ADJACENCY_VALUE;
    }
    if (candidate->estimated_fanout_from_directory)
        return AGE_GRAPH_JOIN_SOURCE_EVIDENCE_ADJACENCY_DIRECTORY;
    if (candidate->index_metadata_backed)
        return AGE_GRAPH_JOIN_SOURCE_EVIDENCE_ADJACENCY_PAYLOAD;

    return AGE_GRAPH_JOIN_SOURCE_EVIDENCE_UNKNOWN;
}

static List *make_adjacency_match_descriptor(
    CypherAdjacencyMatchCandidate *candidate,
    const AdjacencyMatchPayloadRequest *payload_request,
    const AgeGraphJoinCandidateTable *graph_join_table,
    const AgeGraphJoinCandidate *graph_join_candidate)
{
    List *descriptor;
    AgeGraphJoinCandidate *next_best;
    bool edge_payload_required;
    int attr_mask = 0;

    Assert(candidate != NULL);
    Assert(graph_join_table != NULL);
    Assert(graph_join_candidate != NULL);
    if (payload_request != NULL)
    {
        edge_payload_required = payload_request->fetch_properties;
        attr_mask = payload_request->attr_mask;
    }
    else
        edge_payload_required = adjacency_match_requires_edge_payload(
            candidate);

    descriptor = list_make5(
        makeConst(OIDOID, -1, InvalidOid, sizeof(Oid),
                  ObjectIdGetDatum(candidate->graph_oid), false, true),
        makeConst(BOOLOID, -1, InvalidOid, sizeof(bool),
                  BoolGetDatum(candidate->has_edge_variable_projection),
                  false, true),
        makeConst(BOOLOID, -1, InvalidOid, sizeof(bool),
                  BoolGetDatum(candidate->has_edge_property_predicate),
                  false, true),
        makeConst(BOOLOID, -1, InvalidOid, sizeof(bool),
                  BoolGetDatum(candidate->has_right_label_constraint),
                  false, true),
        makeConst(BOOLOID, -1, InvalidOid, sizeof(bool),
                  BoolGetDatum(candidate->has_right_property_predicate),
                  false, true));
    descriptor = lappend(descriptor,
                         makeConst(BOOLOID, -1, InvalidOid, sizeof(bool),
                                   BoolGetDatum(edge_payload_required),
                                   false, true));
    descriptor = lappend(descriptor,
                         makeConst(INT4OID, -1, InvalidOid, sizeof(int32),
                                   Int32GetDatum((int32)attr_mask),
                                   false, true));
    descriptor = lappend(descriptor,
                         makeConst(INT4OID, -1, InvalidOid, sizeof(int32),
                                   Int32GetDatum(candidate->right_label_id),
                                   false, true));
    descriptor = lappend(descriptor,
                         makeConst(INT4OID, -1, InvalidOid, sizeof(int32),
                                   Int32GetDatum((int32)
                                                 candidate->endpoint_attno),
                                   false, true));
    descriptor = lappend(descriptor,
                         makeConst(INT8OID, -1, InvalidOid, sizeof(int64),
                                   Int64GetDatum((int64)(
                                       candidate->estimated_endpoint_fanout +
                                       0.5)),
                                   false, true));
    descriptor = lappend(descriptor,
                         makeConst(INT8OID, -1, InvalidOid, sizeof(int64),
                                   Int64GetDatum((int64)(
                                       candidate->estimated_terminal_fanout +
                                       0.5)),
                                   false, true));
    descriptor = lappend(descriptor,
                         makeConst(INT8OID, -1, InvalidOid, sizeof(int64),
                                   Int64GetDatum((int64)(
                                       candidate->estimated_composite_fanout +
                                       0.5)),
                                   false, true));
    descriptor = lappend(descriptor,
                         makeConst(INT4OID, -1, InvalidOid, sizeof(int32),
                                   Int32GetDatum((int32)
                                       candidate->source_handoff_kind_id),
                                   false, true));
    descriptor = lappend(descriptor,
                         makeConst(INT8OID, -1, InvalidOid, sizeof(int64),
                                   Int64GetDatum((int64)(
                                       candidate->estimated_source_fanout +
                                       0.5)),
                                   false, true));
    descriptor = lappend(descriptor,
                         makeConst(INT8OID, -1, InvalidOid, sizeof(int64),
                                   Int64GetDatum((int64)(
                                       candidate->estimated_source_blocks +
                                       0.5)),
                                   false, true));
    descriptor = lappend(descriptor,
                         makeConst(INT8OID, -1, InvalidOid, sizeof(int64),
                                   Int64GetDatum((int64)(
                                       candidate->estimated_property_source_matches +
                                       0.5)),
                                   false, true));
    descriptor = lappend(descriptor,
                         makeConst(INT8OID, -1, InvalidOid, sizeof(int64),
                                   Int64GetDatum(
                                       graph_property_selectivity_ppm(
                                           candidate->estimated_composite_selectivity)),
                                   false, true));
    descriptor = lappend(descriptor,
                         makeConst(INT8OID, -1, InvalidOid, sizeof(int64),
                                   Int64GetDatum((int64)
                                       candidate->estimated_composite_selectivity_source_kind),
                                   false, true));
    descriptor = lappend(descriptor,
                         makeConst(INT8OID, -1, InvalidOid, sizeof(int64),
                                   Int64GetDatum((int64)
                                       candidate->estimated_value_posting_source_kind),
                                   false, true));
    descriptor = lappend(descriptor,
                         makeConst(INT8OID, -1, InvalidOid, sizeof(int64),
                                   Int64GetDatum((int64)(
                                       candidate->estimated_terminal_label_groups +
                                       0.5)),
                                   false, true));
    descriptor = lappend(descriptor,
                         makeConst(INT8OID, -1, InvalidOid, sizeof(int64),
                                   Int64GetDatum((int64)(
                                       candidate->estimated_main_blocks +
                                       0.5)),
                                   false, true));
    descriptor = lappend(descriptor,
                         makeConst(TEXTOID, -1, DEFAULT_COLLATION_OID, -1,
                                   CStringGetTextDatum(
                                       candidate->estimated_fanout_from_directory ?
                                       "directory" : "statistics"),
                                   false, false));
    descriptor = lappend(descriptor,
                         makeConst(TEXTOID, -1, DEFAULT_COLLATION_OID, -1,
                                   CStringGetTextDatum(
                                       candidate->index_source != NULL ?
                                       candidate->index_source :
                                       "unknown"),
                                   false, false));
    descriptor = lappend(descriptor,
                         makeConst(INT8OID, -1, InvalidOid, sizeof(int64),
                                   Int64GetDatum((int64)
                                       candidate->index_kind_id),
                                   false, true));
    descriptor = lappend(descriptor,
                         makeConst(TEXTOID, -1, DEFAULT_COLLATION_OID, -1,
                                   CStringGetTextDatum(
                                       candidate->index_provider != NULL ?
                                       candidate->index_provider :
                                       "unknown"),
                                   false, false));
    descriptor = lappend(descriptor,
                         makeConst(INT8OID, -1, InvalidOid, sizeof(int64),
                                   Int64GetDatum((int64)
                                       candidate->index_direction_id),
                                   false, true));
    descriptor = lappend(descriptor,
                         makeConst(INT4OID, -1, InvalidOid, sizeof(int32),
                                   Int32GetDatum(
                                       candidate->index_property_count),
                                   false, true));
    descriptor = lappend(descriptor,
                         makeConst(BOOLOID, -1, InvalidOid, sizeof(bool),
                                   BoolGetDatum(
                                       candidate->index_metadata_backed),
                                   false, true));
    descriptor = lappend(descriptor,
                         makeConst(TEXTOID, -1, DEFAULT_COLLATION_OID, -1,
                                   CStringGetTextDatum(
                                       candidate->right_property_key != NULL ?
                                       candidate->right_property_key :
                                       "none"),
                                   false, false));
    descriptor = lappend(descriptor,
                         makeConst(OIDOID, -1, InvalidOid, sizeof(Oid),
                                   ObjectIdGetDatum(
                                       candidate->right_property_index_oid),
                                   false, true));
    descriptor = lappend(descriptor,
                         makeConst(TEXTOID, -1, DEFAULT_COLLATION_OID, -1,
                                   CStringGetTextDatum(
                                       candidate->right_property_index_source != NULL ?
                                       candidate->right_property_index_source :
                                       "none"),
                                   false, false));
    descriptor = lappend(descriptor,
                         makeConst(TEXTOID, -1, DEFAULT_COLLATION_OID, -1,
                                   CStringGetTextDatum(
                                       candidate->right_property_index_provider != NULL ?
                                       candidate->right_property_index_provider :
                                       "none"),
                                   false, false));
    descriptor = lappend(descriptor,
                         makeConst(TEXTOID, -1, DEFAULT_COLLATION_OID, -1,
                                   CStringGetTextDatum(
                                       candidate->right_property_index_type != NULL ?
                                       candidate->right_property_index_type :
                                       "agtype"),
                                   false, false));
    descriptor = lappend(descriptor,
                         makeConst(BOOLOID, -1, InvalidOid, sizeof(bool),
                                   BoolGetDatum(
                                       candidate->right_property_index_metadata_backed),
                                   false, true));
    descriptor = lappend(descriptor,
                         makeConst(BOOLOID, -1, InvalidOid, sizeof(bool),
                                   BoolGetDatum(
                                       candidate->right_property_prefetch_eligible),
                                   false, true));
    descriptor = lappend(descriptor,
                         makeConst(INT8OID, -1, InvalidOid, sizeof(int64),
                                   Int64GetDatum((int64)
                                       candidate->right_property_value_kind_id),
                                   false, true));
    descriptor = lappend(descriptor,
                         makeConst(INT8OID, -1, InvalidOid, sizeof(int64),
                                   Int64GetDatum((int64)
                                       candidate->terminal_source_strategy_id),
                                   false, true));
    descriptor = lappend(descriptor,
                         makeConst(INT4OID, -1, InvalidOid, sizeof(int32),
                                   Int32GetDatum(
                                       adjacency_match_terminal_prefetch_threshold(
                                           candidate, payload_request)),
                                   false, true));
    descriptor = lappend(descriptor,
                         makeConst(INT8OID, -1, InvalidOid, sizeof(int64),
                                   Int64GetDatum((int64)
                                       adjacency_match_terminal_prefetch_reason_kind(
                                           candidate, payload_request)),
                                   false, true));
    descriptor = lappend(descriptor,
                         makeConst(TEXTOID, -1, DEFAULT_COLLATION_OID, -1,
                                   CStringGetTextDatum(
                                       graph_join_candidate->component.display_name),
                                   false, false));
    descriptor = lappend(descriptor,
                         makeConst(INT4OID, -1, InvalidOid, sizeof(int32),
                                   Int32GetDatum(
                                       graph_join_candidate->connector.kind_id),
                                   false, true));
    descriptor = lappend(descriptor,
                         makeConst(TEXTOID, -1, DEFAULT_COLLATION_OID, -1,
                                   CStringGetTextDatum(
                                       age_graph_join_bound_kind_name(
                                           graph_join_candidate->connector.
                                           bound_kind)),
                                   false, false));
    descriptor = lappend(descriptor,
                         makeConst(
                             INT4OID, -1, InvalidOid, sizeof(int32),
                             Int32GetDatum(
                                 graph_join_candidate->connector.
                                 order_property_kind),
                             false, true));
    descriptor = lappend(descriptor,
                         makeConst(TEXTOID, -1, DEFAULT_COLLATION_OID, -1,
                                   CStringGetTextDatum(
                                       age_graph_join_source_evidence_kind_name(
                                           graph_join_candidate->connector.
                                           source_evidence_kind)),
                                   false, false));
    descriptor = lappend(descriptor,
                         makeConst(INT4OID, -1, InvalidOid, sizeof(int32),
                                   Int32GetDatum(
                                       graph_join_candidate->source_kind_id),
                                   false, true));
    descriptor = lappend(descriptor,
                         makeConst(TEXTOID, -1, DEFAULT_COLLATION_OID, -1,
                                   CStringGetTextDatum(
                                       bmsToString(
                                           graph_join_candidate->
                                           connector.solved_relids)),
                                   false, false));
    descriptor = lappend(descriptor,
                         makeConst(INT4OID, -1, InvalidOid, sizeof(int32),
                                   Int32GetDatum(
                                       age_graph_join_table_candidate_count(
                                           graph_join_table)),
                                   false, true));
    descriptor = lappend(descriptor,
                         makeConst(INT4OID, -1, InvalidOid, sizeof(int32),
                                   Int32GetDatum(
                                       graph_join_table->declared_entry_count),
                                   false, true));
    descriptor = lappend(descriptor,
                         makeConst(INT4OID, -1, InvalidOid, sizeof(int32),
                                   Int32GetDatum(
                                       graph_join_table->cover_match_kind),
                                   false, true));
    next_best = age_graph_join_table_select_next_best(graph_join_table,
                                                      graph_join_candidate);
    descriptor = lappend(descriptor,
                         makeConst(
                             INT4OID, -1, InvalidOid, sizeof(int32),
                             Int32GetDatum(
                                 next_best != NULL ?
                                 next_best->connector.kind_id :
                                 AGE_GRAPH_JOIN_CONNECTOR_UNKNOWN),
                             false, true));
    descriptor = lappend(descriptor,
                         makeConst(
                             INT4OID, -1, InvalidOid, sizeof(int32),
                             Int32GetDatum(
                                 next_best != NULL ?
                                 next_best->connector.order_property_kind :
                                 AGE_GRAPH_JOIN_ORDER_UNKNOWN),
                             false, true));
    descriptor = lappend(descriptor,
                         makeConst(TEXTOID, -1, DEFAULT_COLLATION_OID, -1,
                                   CStringGetTextDatum(
                                       next_best != NULL ?
                                       age_graph_join_source_evidence_kind_name(
                                           next_best->connector.
                                           source_evidence_kind) :
                                       "none"),
                                   false, false));
    descriptor = lappend(descriptor,
                         makeConst(FLOAT8OID, -1, InvalidOid,
                                   sizeof(float8),
                                   Float8GetDatum(
                                       next_best != NULL ?
                                       next_best->connector.rows : 0),
                                   false, true));
    descriptor = lappend(descriptor,
                         makeConst(FLOAT8OID, -1, InvalidOid,
                                   sizeof(float8),
                                   Float8GetDatum(
                                       next_best != NULL ?
                                       next_best->connector.total_cost : 0),
                                   false, true));
    descriptor = lappend(descriptor,
                         makeConst(BOOLOID, -1, InvalidOid, sizeof(bool),
                                   BoolGetDatum(
                                       graph_join_candidate->
                                       component.parallel_safe),
                                   false, true));
    descriptor = lappend(descriptor,
                         makeConst(BOOLOID, -1, InvalidOid, sizeof(bool),
                                   BoolGetDatum(
                                       graph_join_candidate->
                                       component.parallel_aware),
                                   false, true));
    descriptor = lappend(descriptor,
                         makeConst(INT4OID, -1, InvalidOid, sizeof(int32),
                                   Int32GetDatum(
                                       graph_join_candidate->
                                       component.parallel_workers),
                                   false, true));
    descriptor = lappend(descriptor,
                         makeConst(FLOAT8OID, -1, InvalidOid,
                                   sizeof(float8),
                                   Float8GetDatum(
                                       graph_join_candidate->
                                       component.gather_cost),
                                   false, true));
    descriptor = lappend(descriptor,
                         makeConst(BOOLOID, -1, InvalidOid, sizeof(bool),
                                   BoolGetDatum(
                                       graph_join_candidate->
                                       component.order_preserving),
                                   false, true));
    descriptor = lappend(descriptor,
                         makeConst(BOOLOID, -1, InvalidOid, sizeof(bool),
                                   BoolGetDatum(
                                       graph_join_candidate->
                                       component.shared_state_required),
                                   false, true));
    descriptor = lappend(descriptor,
                         candidate->right_property_value != NULL ?
                         copyObject(candidate->right_property_value) :
                         makeConst(AGTYPEOID, -1, InvalidOid, -1,
                                   (Datum)0, true, false));

    return descriptor;
}

/*
 * Collect the edge-only restriction clauses for the adjacency scan -- those that
 * reference no relation other than the scanned edge rel (e.g. an edge property
 * predicate r.weight > 4).  These can be evaluated against the produced edge
 * tuple inside the cursor to skip non-matching edges early, before they reach
 * the surrounding ExecScan qual/projection (item 9: reduce the payload-filtered
 * count).  Join clauses (which reference an outer rel) are excluded -- their
 * outer Vars are not available during cursor iteration.  The clauses remain in
 * scan.plan.qual as well, so correctness never depends on the early skip.
 */
static List *collect_adjacency_match_edge_only_quals(Index scanrelid,
                                                     List *clauses)
{
    List *edge_quals = NIL;
    Relids self = bms_make_singleton(scanrelid);
    ListCell *lc;

    foreach(lc, clauses)
    {
        RestrictInfo *rinfo;
        Node *clause = lfirst(lc);

        if (!IsA(clause, RestrictInfo))
            continue;
        rinfo = (RestrictInfo *) clause;
        if (rinfo->pseudoconstant)
            continue;
        if (rinfo->clause == NULL)
            continue;
        if (bms_is_subset(rinfo->clause_relids, self))
            edge_quals = lappend(edge_quals, rinfo->clause);
    }

    bms_free(self);
    return edge_quals;
}

static Plan *plan_age_adjacency_match_path(PlannerInfo *root,
                                           RelOptInfo *rel,
                                           CustomPath *best_path,
                                           List *tlist, List *clauses,
                                           List *custom_plans)
{
    CustomScan *cs;
    List *custom_scan_tlist;
    AdjacencyMatchPayloadRequest payload_request;
    List *edge_only_quals;

    (void) root;
    (void) custom_plans;

    payload_request = build_adjacency_match_payload_request(rel->relid, tlist,
                                                            clauses);
    custom_scan_tlist = build_adjacency_match_custom_scan_tlist(
        rel->relid, &payload_request);

    cs = makeNode(CustomScan);

    cs->scan.plan.startup_cost = best_path->path.startup_cost;
    cs->scan.plan.total_cost = best_path->path.total_cost;
    cs->scan.plan.plan_rows = best_path->path.rows;
    cs->scan.plan.plan_width = rel->reltarget->width;
    cs->scan.plan.parallel_aware = best_path->path.parallel_aware;
    cs->scan.plan.parallel_safe = best_path->path.parallel_safe;
    cs->scan.plan.async_capable = false;
    cs->scan.plan.targetlist = tlist;
    cs->scan.plan.qual = extract_actual_clauses(clauses, false);
    cs->scan.plan.lefttree = NULL;
    cs->scan.plan.righttree = NULL;
    cs->scan.scanrelid = rel->relid;

    cs->flags = best_path->flags;
    cs->custom_plans = NIL;
    cs->custom_exprs = list_make2(linitial(best_path->custom_private),
                                  lsecond(best_path->custom_private));
    /*
     * Edge-only restriction clauses are appended as a third custom_exprs entry
     * (a List, recursed by setrefs so its Vars are fixed) so the executor can
     * skip non-matching edges in the cursor.  They also stay in scan.plan.qual,
     * so the early skip is a pure optimization and never affects correctness.
     */
    edge_only_quals = collect_adjacency_match_edge_only_quals(rel->relid,
                                                              clauses);
    cs->custom_exprs = lappend(cs->custom_exprs, edge_only_quals);
    cs->custom_private = list_copy_tail(best_path->custom_private, 2);
    cs->custom_scan_tlist = custom_scan_tlist;
    cs->custom_relids = bms_make_singleton(rel->relid);
    cs->methods = &age_adjacency_match_scan_methods;

    return (Plan *)cs;
}

typedef struct AdjacencyMatchScanVarContext
{
    Index relid;
    Bitmapset *attrs;
    bool unsupported;
} AdjacencyMatchScanVarContext;

static AdjacencyMatchPayloadRequest build_adjacency_match_payload_request(
    Index relid, List *target_nodes, List *clauses)
{
    AdjacencyMatchScanVarContext context;
    AdjacencyMatchPayloadRequest request;

    context.relid = relid;
    context.attrs = NULL;
    context.unsupported = false;

    collect_adjacency_match_scan_vars_from_list(target_nodes, &context);
    collect_adjacency_match_scan_vars_from_list(clauses, &context);

    request.attrs = context.attrs;
    request.attr_mask = adjacency_match_payload_attr_mask(context.attrs);
    request.unsupported = context.unsupported;
    request.fetch_properties = context.unsupported || context.attrs == NULL ||
        bms_is_member(Anum_ag_label_edge_table_properties, context.attrs);

    return request;
}

static List *build_adjacency_match_custom_scan_tlist(
    Index relid, const AdjacencyMatchPayloadRequest *payload_request)
{
    List *custom_tlist = NIL;
    int attno;
    int resno = 1;

    if (payload_request == NULL ||
        payload_request->unsupported ||
        payload_request->attrs == NULL)
        return NIL;

    attno = -1;
    while ((attno = bms_next_member(payload_request->attrs, attno)) >= 0)
    {
        Var *var;
        TargetEntry *tle;
        Oid vartype;

        vartype = adjacency_match_attr_type((AttrNumber)attno);
        if (!OidIsValid(vartype))
            return NIL;

        var = makeVar(relid, (AttrNumber)attno, vartype, -1, InvalidOid, 0);
        tle = makeTargetEntry((Expr *)var, resno++,
                              pstrdup(adjacency_match_attr_name(
                                  (AttrNumber)attno)),
                              false);
        custom_tlist = lappend(custom_tlist, tle);
    }

    return custom_tlist;
}

static int adjacency_match_payload_attr_mask(Bitmapset *attrs)
{
    int mask = 0;
    int attno = -1;

    if (attrs == NULL)
        return 0;

    while ((attno = bms_next_member(attrs, attno)) >= 0)
        mask |= (1 << (attno - 1));

    return mask;
}

static void collect_adjacency_match_scan_vars_from_list(List *nodes,
                                                        void *context)
{
    ListCell *lc;

    foreach(lc, nodes)
    {
        Node *node = lfirst(lc);

        if (node == NULL)
            continue;

        if (IsA(node, TargetEntry))
            node = (Node *)((TargetEntry *)node)->expr;
        else if (IsA(node, RestrictInfo))
            node = (Node *)((RestrictInfo *)node)->clause;

        collect_adjacency_match_scan_vars(node, context);
    }
}

static bool collect_adjacency_match_scan_vars(Node *node, void *context)
{
    AdjacencyMatchScanVarContext *var_context =
        (AdjacencyMatchScanVarContext *)context;

    if (node == NULL || var_context->unsupported)
        return false;

    if (IsA(node, Var))
    {
        Var *var = (Var *)node;

        if (var->varno != var_context->relid)
            return false;

        if (var->varattno < Anum_ag_label_edge_table_id ||
            var->varattno > Anum_ag_label_edge_table_properties)
        {
            var_context->unsupported = true;
            return false;
        }

        var_context->attrs =
            bms_add_member(var_context->attrs, var->varattno);
        return false;
    }

    return expression_tree_walker(node, collect_adjacency_match_scan_vars,
                                  context);
}

static const char *adjacency_match_attr_name(AttrNumber attno)
{
    switch (attno)
    {
    case Anum_ag_label_edge_table_id:
        return "id";
    case Anum_ag_label_edge_table_start_id:
        return "start_id";
    case Anum_ag_label_edge_table_end_id:
        return "end_id";
    case Anum_ag_label_edge_table_properties:
        return "properties";
    default:
        elog(ERROR, "unexpected AGE adjacency match attr %d", attno);
    }
}

static Oid adjacency_match_attr_type(AttrNumber attno)
{
    switch (attno)
    {
    case Anum_ag_label_edge_table_id:
    case Anum_ag_label_edge_table_start_id:
    case Anum_ag_label_edge_table_end_id:
        return GRAPHIDOID;
    case Anum_ag_label_edge_table_properties:
        return AGTYPEOID;
    default:
        return InvalidOid;
    }
}

/*
 * Check to see if the rte is a Cypher clause. An rte is only a Cypher clause
 * if it is a subquery, with the last entry in its target list, that is a
 * FuncExpr.
 */
static cypher_clause_kind get_cypher_clause_kind(RangeTblEntry *rte)
{
    TargetEntry *te;
    FuncExpr *fe;

    /* If it's not a subquery, it's not a Cypher clause. */
    if (rte->rtekind != RTE_SUBQUERY)
        return CYPHER_CLAUSE_NONE;

    /* Make sure the targetList isn't NULL. NULL means potential EXIST subclause */
    if (rte->subquery->targetList == NULL)
        return CYPHER_CLAUSE_NONE;

    /* A Cypher clause function is always the last entry. */
    te = llast(rte->subquery->targetList);

    /* If the last entry is not a FuncExpr, it's not a Cypher clause. */
    if (!IsA(te->expr, FuncExpr))
        return CYPHER_CLAUSE_NONE;

    fe = (FuncExpr *)te->expr;

    load_cypher_clause_function_oids();

    if (fe->funcid == cypher_create_clause_func_oid)
        return CYPHER_CLAUSE_CREATE;
    if (fe->funcid == cypher_set_clause_func_oid)
        return CYPHER_CLAUSE_SET;
    if (fe->funcid == cypher_delete_clause_func_oid)
        return CYPHER_CLAUSE_DELETE;
    if (fe->funcid == cypher_merge_clause_func_oid)
        return CYPHER_CLAUSE_MERGE;
    else
        return CYPHER_CLAUSE_NONE;
}

static void register_cypher_clause_function_oid_callbacks(void)
{
    if (!cypher_clause_func_callback_registered)
    {
        CacheRegisterSyscacheCallback(PROCOID,
                                      invalidate_cypher_clause_function_oids,
                                      (Datum)0);
        CacheRegisterSyscacheCallback(PROCNAMEARGSNSP,
                                      invalidate_cypher_clause_function_oids,
                                      (Datum)0);
        cypher_clause_func_callback_registered = true;
    }
}

static void load_cypher_clause_function_oids(void)
{
    register_cypher_clause_function_oid_callbacks();

    if (!OidIsValid(cypher_create_clause_func_oid))
    {
        cypher_create_clause_func_oid =
            get_ag_func_oid(CREATE_CLAUSE_FUNCTION_NAME, 1, INTERNALOID);
    }

    if (!OidIsValid(cypher_set_clause_func_oid))
    {
        cypher_set_clause_func_oid =
            get_ag_func_oid(SET_CLAUSE_FUNCTION_NAME, 1, INTERNALOID);
    }

    if (!OidIsValid(cypher_delete_clause_func_oid))
    {
        cypher_delete_clause_func_oid =
            get_ag_func_oid(DELETE_CLAUSE_FUNCTION_NAME, 1, INTERNALOID);
    }

    if (!OidIsValid(cypher_merge_clause_func_oid))
    {
        cypher_merge_clause_func_oid =
            get_ag_func_oid(MERGE_CLAUSE_FUNCTION_NAME, 1, INTERNALOID);
    }
}

static void invalidate_cypher_clause_function_oids(Datum arg, int cache_id,
                                                   uint32 hash_value)
{
    cypher_create_clause_func_oid = InvalidOid;
    cypher_set_clause_func_oid = InvalidOid;
    cypher_delete_clause_func_oid = InvalidOid;
    cypher_merge_clause_func_oid = InvalidOid;
    cypher_property_path_invalidate_oids();
    cypher_property_signature_invalidate_oids();
}

static void replace_with_cypher_dml_path(PlannerInfo *root, RelOptInfo *rel,
                                         RangeTblEntry *rte,
                                         cypher_path_factory factory)
{
    TargetEntry *te;
    FuncExpr *fe;
    List *custom_private;
    CustomPath *cp;

    /* Add the pattern to the CustomPath */
    te = (TargetEntry *)llast(rte->subquery->targetList);
    fe = (FuncExpr *)te->expr;
    /* pass the const that holds the data structure to the path. */
    custom_private = fe->args;

    cp = factory(root, rel, custom_private);

    /* Discard any preexisting paths */
    rel->pathlist = NIL;
    rel->partial_pathlist = NIL;

    add_path(rel, (Path *)cp);
}
