/*
 * For PostgreSQL Database Management System:
 * (formerly known as Postgres, then as Postgres95)
 *
 * Portions Copyright (c) 1996-2010, The PostgreSQL Global Development Group
 *
 * Portions Copyright (c) 1994, The Regents of the University of California
 *
 * Permission to use, copy, modify, and distribute this software and its documentation for any purpose,
 * without fee, and without a written agreement is hereby granted, provided that the above copyright notice
 * and this paragraph and the following two paragraphs appear in all copies.
 *
 * IN NO EVENT SHALL THE UNIVERSITY OF CALIFORNIA BE LIABLE TO ANY PARTY FOR DIRECT,
 * INDIRECT, SPECIAL, INCIDENTAL, OR CONSEQUENTIAL DAMAGES, INCLUDING LOST PROFITS,
 * ARISING OUT OF THE USE OF THIS SOFTWARE AND ITS DOCUMENTATION, EVEN IF THE UNIVERSITY
 * OF CALIFORNIA HAS BEEN ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * THE UNIVERSITY OF CALIFORNIA SPECIFICALLY DISCLAIMS ANY WARRANTIES, INCLUDING,
 * BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE.
 *
 * THE SOFTWARE PROVIDED HEREUNDER IS ON AN "AS IS" BASIS, AND THE UNIVERSITY OF CALIFORNIA
 * HAS NO OBLIGATIONS TO PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR MODIFICATIONS.
 */

#include "postgres.h"

#include "catalog/pg_proc.h"
#include "catalog/dependency.h"
#include "catalog/ag_label.h"
#include "commands/extension.h"
#include "executor/cypher_vle_stream.h"
#include "fmgr.h"
#include "commands/label_commands.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "parser/parsetree.h"
#include "parser/parse_coerce.h"
#include "parser/parse_collate.h"
#include "parser/parse_func.h"
#include "parser/cypher_clause.h"
#include "parser/parse_oper.h"
#include "parser/parse_relation.h"
#include "parser/parse_type.h"
#include "utils/builtins.h"
#include "utils/catcache.h"
#include "utils/float.h"
#include "utils/hsearch.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rls.h"
#include "utils/syscache.h"

#include "parser/cypher_expr.h"
#include "parser/cypher_property_signature.h"
#include "parser/cypher_transform_entity.h"
#include "parser/cypher_vle_agg.h"
#include "utils/ag_func.h"
#include "utils/agtype.h"
#include "utils/age_vle.h"
#include "utils/graphid.h"

/* names of typecast functions */
#define FUNC_AGTYPE_TYPECAST_EDGE "agtype_typecast_edge"
#define FUNC_AGTYPE_TYPECAST_PATH "agtype_typecast_path"
#define FUNC_AGTYPE_TYPECAST_VERTEX "agtype_typecast_vertex"
#define FUNC_AGTYPE_TYPECAST_NUMERIC "agtype_typecast_numeric"
#define FUNC_AGTYPE_TYPECAST_FLOAT "agtype_typecast_float"
#define FUNC_AGTYPE_TYPECAST_INT "agtype_typecast_int"
#define FUNC_AGTYPE_TYPECAST_PG_FLOAT8 "agtype_to_float8"
#define FUNC_AGTYPE_TYPECAST_PG_BIGINT "agtype_to_int8"
#define FUNC_AGTYPE_TYPECAST_PG_NUMERIC "agtype_to_numeric"
#define FUNC_AGTYPE_TYPECAST_BOOL "agtype_typecast_bool"
#define FUNC_AGTYPE_TYPECAST_PG_TEXT "agtype_to_text"

typedef struct function_exists_cache_key
{
    NameData funcname;
    NameData extension;
} function_exists_cache_key;

typedef struct function_exists_cache_entry
{
    function_exists_cache_key key;
    bool exists;
} function_exists_cache_entry;

typedef struct function_extension_cache_entry
{
    Oid func_oid;
    bool has_extension;
    NameData extension;
} function_extension_cache_entry;

typedef enum AgeSemanticSourceKind
{
    AGE_SEM_SOURCE_UNKNOWN,
    AGE_SEM_SOURCE_FIXED_PATH,
    AGE_SEM_SOURCE_COMPACT_VLE_PATH,
    AGE_SEM_SOURCE_COMPACT_VLE_EDGE_LIST,
    AGE_SEM_SOURCE_COMPACT_VLE_NODE_LIST,
    AGE_SEM_SOURCE_MATERIALIZED_AGTYPE
} AgeSemanticSourceKind;

static HTAB *function_exists_cache = NULL;
static HTAB *function_extension_cache = NULL;
static bool function_cache_callback_registered = false;
static Oid agtype_access_operator_oid = InvalidOid;
static Oid agtype_object_field_agtype_oid = InvalidOid;
static Oid agtype_object_field_int8_oid = InvalidOid;
static Oid agtype_object_field_float8_oid = InvalidOid;
static Oid agtype_object_field_text_agtype_oid = InvalidOid;
static Oid agtype_object_field_numeric_agtype_oid = InvalidOid;
static Oid agtype_object_field_numeric_oid = InvalidOid;
static Oid agtype_access_slice_oid = InvalidOid;
static Oid agtype_in_operator_oid = InvalidOid;
static Oid agtype_add_oid = InvalidOid;
static Oid agtype_build_empty_list_oid = InvalidOid;
static Oid agtype_build_list_oid = InvalidOid;
static Oid agtype_build_empty_map_oid = InvalidOid;
static Oid agtype_build_map_oid = InvalidOid;
static Oid agtype_build_map_nonull_oid = InvalidOid;
static Oid age_properties_oid = InvalidOid;
static Oid age_keys_oid = InvalidOid;
static Oid build_vertex_label_oid = InvalidOid;
static Oid build_edge_label_oid = InvalidOid;
static Oid agtype_string_match_starts_with_oid = InvalidOid;
static Oid agtype_string_match_ends_with_oid = InvalidOid;
static Oid agtype_string_match_contains_oid = InvalidOid;
static Oid text_to_agtype_oid = InvalidOid;
static Oid label_name_oid = InvalidOid;
static Oid age_vle_path_length_oid = InvalidOid;
static Oid age_vle_path_node_count_oid = InvalidOid;
static Oid age_vle_edge_tail_count_oid = InvalidOid;
static Oid age_vle_list_is_empty_oid = InvalidOid;
static Oid age_vle_list_slice_count_oid = InvalidOid;
static Oid age_vle_list_slice_is_empty_oid = InvalidOid;
static Oid age_materialize_vle_list_slice_oid = InvalidOid;
static Oid age_materialize_vle_slice_boundary_oid = InvalidOid;
static Oid age_materialize_vle_edges_oid = InvalidOid;
static Oid age_materialize_vle_nodes_oid = InvalidOid;
static Oid age_materialize_vle_node_at_oid = InvalidOid;
static Oid age_materialize_vle_node_tail_last_oid = InvalidOid;
static Oid age_vle_node_tail_last_id_oid = InvalidOid;
static Oid age_vle_node_id_at_oid = InvalidOid;
static Oid age_vle_node_label_at_oid = InvalidOid;
static Oid age_vle_node_labels_at_oid = InvalidOid;
static Oid age_vle_node_properties_at_oid = InvalidOid;
static Oid age_vle_node_property_at_oid = InvalidOid;
static Oid age_materialize_vle_nodes_slice_oid = InvalidOid;
static Oid age_materialize_vle_nodes_tail_oid = InvalidOid;
static Oid age_materialize_vle_nodes_reversed_oid = InvalidOid;
static Oid age_materialize_vle_edge_at_oid = InvalidOid;
static Oid age_materialize_vle_edge_reversed_at_oid = InvalidOid;
static Oid age_materialize_vle_edge_tail_last_oid = InvalidOid;
static Oid age_vle_edge_tail_last_id_oid = InvalidOid;
static Oid age_vle_tail_last_field_oid = InvalidOid;
static Oid age_vle_tail_last_edge_endpoint_oid = InvalidOid;
static Oid age_vle_tail_last_endpoint_field_oid = InvalidOid;
static Oid age_vle_edge_id_at_oid = InvalidOid;
static Oid age_vle_edge_index_exists_oid = InvalidOid;
static Oid age_vle_edge_indices_equal_oid = InvalidOid;
static Oid age_vle_edge_reversed_index_equal_oid = InvalidOid;
static Oid age_vle_edge_label_at_oid = InvalidOid;
static Oid age_vle_edge_properties_at_oid = InvalidOid;
static Oid age_vle_edge_property_at_oid = InvalidOid;
static Oid age_vle_edge_start_node_at_oid = InvalidOid;
static Oid age_vle_edge_end_node_at_oid = InvalidOid;
static Oid age_vle_edge_endpoint_field_at_oid = InvalidOid;
static Oid age_vle_edge_start_id_at_oid = InvalidOid;
static Oid age_vle_edge_end_id_at_oid = InvalidOid;
static Oid age_materialize_vle_edges_tail_oid = InvalidOid;
static Oid age_materialize_vle_edges_reversed_oid = InvalidOid;
static Oid age_vle_terminal_vertex_oid = InvalidOid;
static Oid age_vle_terminal_vertex_properties_oid = InvalidOid;
static Oid age_vle_terminal_vertex_property_oid = InvalidOid;
static Oid age_vle_terminal_vertex_property_from_path_oid = InvalidOid;

static Node *transform_cypher_expr_recurse(cypher_parsestate *cpstate,
                                           Node *expr);
static Node *transform_A_Const(cypher_parsestate *cpstate, A_Const *ac);
static Node *transform_ColumnRef(cypher_parsestate *cpstate, ColumnRef *cref);
static Node *transform_A_Indirection(cypher_parsestate *cpstate,
                                     A_Indirection *a_ind);
static Node *transform_AEXPR_OP(cypher_parsestate *cpstate, A_Expr *a);
static void coerce_pg_scalar_property_comparison(cypher_parsestate *cpstate,
                                                 A_Expr *a, Node **left,
                                                 Node **right);
static bool is_pg_scalar_property_expr(Node *node, Oid *result_type);
static bool is_scalar_comparison_operator(List *opname);
static Node *transform_cypher_comparison_aexpr_OP(cypher_parsestate *cpstate,
                                                  cypher_comparison_aexpr *a);
static Node *transform_BoolExpr(cypher_parsestate *cpstate, BoolExpr *expr);
static Node *transform_cypher_comparison_boolexpr(cypher_parsestate *cpstate,
                                                  cypher_comparison_boolexpr *b);
static Node *transform_cypher_bool_const(cypher_parsestate *cpstate,
                                         cypher_bool_const *bc);
static Node *transform_cypher_integer_const(cypher_parsestate *cpstate,
                                            cypher_integer_const *ic);
static Const *make_agtype_integer_const(int64 value, int location);
static Node *make_null_agtype_const(void);
static Node *transform_or_null_agtype(cypher_parsestate *cpstate, Node *node);
static void transform_slice_bounds_or_null(cypher_parsestate *cpstate,
                                           A_Indices *indices,
                                           Node **lower_expr,
                                           Node **upper_expr);
static void make_slice_bounds_to_null(int64 lower, int location,
                                      Node **lower_expr,
                                      Node **upper_expr);
static Node *make_vle_boundary_index_expr(const char *boundary_name,
                                          int location);
static bool parse_vle_boundary_function(FuncCall *fn, char **boundary_name,
                                        Node **arg, Node **index_expr);
static Node *transform_AEXPR_IN(cypher_parsestate *cpstate, A_Expr *a);
static Node *try_transform_vle_edge_self_membership(
    cypher_parsestate *cpstate, A_Expr *a);
static Node *try_transform_vle_edge_index_equality(
    cypher_parsestate *cpstate, A_Expr *a);
static Node *try_transform_entity_id_equality(cypher_parsestate *cpstate,
                                              A_Expr *a);
static Node *try_transform_edge_endpoint_id_equality(cypher_parsestate *cpstate,
                                                    A_Expr *a);
static NullTest *make_raw_null_test(Node *arg, NullTest *source);
static Node *try_transform_entity_null_test(cypher_parsestate *cpstate,
                                            NullTest *n);
static Node *try_transform_edge_endpoint_id_null_test(
    cypher_parsestate *cpstate, NullTest *n);
static Node *try_transform_edge_endpoint_null_test(cypher_parsestate *cpstate,
                                                   NullTest *n);
static Node *try_transform_entity_graphid_function(cypher_parsestate *cpstate,
                                                  FuncCall *fn);
static Node *try_build_edge_from_raw_attrs(cypher_parsestate *cpstate,
                                           const char *var_name,
                                           int location);
static Node *try_build_vertex_from_raw_attrs(cypher_parsestate *cpstate,
                                             const char *var_name,
                                             int location);
static Node *try_transform_entity_properties_expr(cypher_parsestate *cpstate,
                                                  Node *node, int location,
                                                  bool previous_vertex_ok);
static Node *try_transform_current_entity_properties(cypher_parsestate *cpstate,
                                                    FuncCall *fn);
static Node *try_transform_entity_keys(cypher_parsestate *cpstate,
                                       FuncCall *fn);
static Node *try_transform_current_entity_label(cypher_parsestate *cpstate,
                                               FuncCall *fn);
static Node *try_transform_current_vertex_labels(cypher_parsestate *cpstate,
                                                FuncCall *fn);
static bool parse_fixed_path_list_function(cypher_parsestate *cpstate,
                                           FuncCall *list_fn,
                                           char **list_name,
                                           cypher_node **start_node,
                                           cypher_relationship **single_rel,
                                           cypher_node **end_node,
                                           bool require_node_names);
static bool parse_fixed_path_cardinality_list(
    cypher_parsestate *cpstate, FuncCall *inner_fn, char **list_name,
    char **transform_name, cypher_node **start_node,
    cypher_relationship **single_rel, cypher_node **end_node);
static bool parse_fixed_path_cardinality_metadata(
    cypher_parsestate *cpstate, FuncCall *inner_fn, char **list_name,
    char **transform_name, int64 *edge_count, char **first_edge_name);
static Node *try_make_fixed_path_entity_list(cypher_parsestate *cpstate,
                                             FuncCall *fn, bool nodes_list);
static bool parse_path_list_slice_arg(A_Indirection *a_ind,
                                      FuncCall **list_fn,
                                      A_Indices **indices,
                                      char **list_name);
static bool parse_fixed_relationship_zero_index(
    cypher_parsestate *cpstate, A_Indirection *rel_ind,
    bool require_node_names, char **list_name, cypher_node **start_node,
    cypher_relationship **single_rel, cypher_node **end_node);
static Node *try_transform_fixed_path_length(cypher_parsestate *cpstate,
                                             FuncCall *fn);
static Node *try_transform_fixed_path_list_cardinality(
    cypher_parsestate *cpstate, FuncCall *fn);
static Node *try_transform_fixed_path_slice_cardinality(
    cypher_parsestate *cpstate, FuncCall *fn);
static Node *try_transform_fixed_path_relationships(cypher_parsestate *cpstate,
                                                    FuncCall *fn);
static Node *try_transform_fixed_path_nodes(cypher_parsestate *cpstate,
                                            FuncCall *fn);
static Node *try_transform_fixed_path_head_last(cypher_parsestate *cpstate,
                                               FuncCall *fn);
static Node *try_transform_fixed_path_head_last_consumer(
    cypher_parsestate *cpstate, FuncCall *fn);
static Node *try_transform_fixed_path_slice_head_last_consumer(
    cypher_parsestate *cpstate, FuncCall *fn);
static Node *try_transform_fixed_path_slice_transform_head_last_consumer(
    cypher_parsestate *cpstate, FuncCall *fn);
static Node *try_transform_fixed_path_tail(cypher_parsestate *cpstate,
                                           FuncCall *fn);
static Node *try_transform_fixed_path_slice_tail(cypher_parsestate *cpstate,
                                                 FuncCall *fn);
static Node *try_transform_fixed_path_reverse(cypher_parsestate *cpstate,
                                              FuncCall *fn);
static Node *try_transform_fixed_path_slice_reverse(cypher_parsestate *cpstate,
                                                    FuncCall *fn);
static Node *try_transform_fixed_path_indexed_id(cypher_parsestate *cpstate,
                                                 FuncCall *fn);
static Node *try_transform_fixed_path_indexed_edge_endpoint_id(
    cypher_parsestate *cpstate, FuncCall *fn);
static Node *try_transform_fixed_path_indexed_endpoint_vertex(
    cypher_parsestate *cpstate, FuncCall *fn);
static Node *try_transform_fixed_path_indexed_endpoint_vertex_function(
    cypher_parsestate *cpstate, FuncCall *fn);
static Node *try_transform_edge_endpoint_vertex(cypher_parsestate *cpstate,
                                                FuncCall *fn);
static Node *try_transform_edge_endpoint_vertex_function(
    cypher_parsestate *cpstate, FuncCall *fn);
static Node *try_transform_fixed_path_slice_endpoint_vertex(
    cypher_parsestate *cpstate, FuncCall *fn);
static Node *try_transform_fixed_path_slice_endpoint_vertex_function(
    cypher_parsestate *cpstate, FuncCall *fn);
static Node *try_transform_fixed_path_indexed_properties(
    cypher_parsestate *cpstate, FuncCall *fn);
static Node *try_transform_fixed_path_indexed_keys(cypher_parsestate *cpstate,
                                                  FuncCall *fn);
static Node *try_transform_fixed_path_indexed_property_access(
    cypher_parsestate *cpstate, A_Indirection *a_ind);
static Node *try_transform_fixed_path_indexed_endpoint_property_access(
    cypher_parsestate *cpstate, A_Indirection *a_ind);
static Node *try_transform_current_edge_endpoint_property_access(
    cypher_parsestate *cpstate, A_Indirection *a_ind);
static Node *try_transform_fixed_path_slice_endpoint_property_access(
    cypher_parsestate *cpstate, A_Indirection *a_ind);
static Node *try_transform_fixed_path_slice_head_last_property_access(
    cypher_parsestate *cpstate, A_Indirection *a_ind);
static Node *try_transform_fixed_path_list_slice(cypher_parsestate *cpstate,
                                                A_Indirection *a_ind);
static Node *try_transform_fixed_path_reverse_indexed_consumer(
    cypher_parsestate *cpstate, FuncCall *fn);
static Node *try_transform_fixed_path_indexed_label_type(
    cypher_parsestate *cpstate, FuncCall *fn);
static Node *try_transform_fixed_path_indexed_labels(
    cypher_parsestate *cpstate, FuncCall *fn);
static Node *try_transform_vle_edge_boundary_equality(
    cypher_parsestate *cpstate, A_Expr *a);
static Node *try_transform_vle_edge_reversed_equality(
    cypher_parsestate *cpstate, A_Expr *a);
static Node *try_transform_vle_edge_nested_transform_equality(
    cypher_parsestate *cpstate, A_Expr *a);
static Node *try_transform_vle_edge_normalized_equality(
    cypher_parsestate *cpstate, A_Expr *a);
static Expr *get_arbitrary_vle_relationship_list_expr(
    cypher_parsestate *cpstate, Node *arg);
static Node *transform_cypher_param(cypher_parsestate *cpstate,
                                    cypher_param *cp);
static Node *transform_cypher_map(cypher_parsestate *cpstate, cypher_map *cm);
static Node *transform_cypher_map_projection(cypher_parsestate *cpstate,
                                             cypher_map_projection *cmp);
static Node *transform_cypher_list(cypher_parsestate *cpstate,
                                   cypher_list *cl);
static Node *transform_cypher_string_match(cypher_parsestate *cpstate,
                                           cypher_string_match *csm_node);
static Node *transform_cypher_typecast(cypher_parsestate *cpstate,
                                       cypher_typecast *ctypecast);
static Node *transform_CaseExpr(cypher_parsestate *cpstate,
                                    CaseExpr *cexpr);
static Node *transform_CoalesceExpr(cypher_parsestate *cpstate,
                                    CoalesceExpr *cexpr);
static Node *transform_SubLink(cypher_parsestate *cpstate, SubLink *sublink);
static Node *transform_FuncCall(cypher_parsestate *cpstate, FuncCall *fn);
static bool try_rewrite_collect_property_access(Aggref *aggref);
static Node *try_rewrite_pg_scalar_property_access(FuncExpr *func_expr,
                                                   Oid helper_oid,
                                                   Oid result_type);
static Node *transform_WholeRowRef(ParseState *pstate, ParseNamespaceItem *pnsi,
                                   int location, int sublevels_up);
static ArrayExpr *make_agtype_array_expr(List *args);
static Node *make_agtype_case_when_not_null(Node *not_null_expr,
                                            Expr *result_expr,
                                            int location);
static Node *make_fixed_path_slice_list_expr(cypher_parsestate *cpstate,
                                             cypher_node *start_node,
                                             cypher_relationship *single_rel,
                                             cypher_node *end_node,
                                             const char *list_name,
                                             int64 lower, int64 upper,
                                             bool reversed, int location);
static Node *make_fixed_path_slice_list_result(cypher_parsestate *cpstate,
                                               cypher_node *start_node,
                                               cypher_relationship *single_rel,
                                               cypher_node *end_node,
                                               const char *list_name,
                                               int64 lower, int64 upper,
                                               bool reversed, int location);
static Node *transform_column_ref_for_indirection(cypher_parsestate *cpstate,
                                                  ColumnRef *cr);
static bool parse_single_column_ref(Node *node, char **name, int *location);
static Node *transform_external_ext_FuncCall(cypher_parsestate *cpstate,
                                             FuncCall *fn, List *targs,
                                             Form_pg_proc procform,
                                             const char *extension);
static Expr *get_current_any_single_vle_path_expr(cypher_parsestate *cpstate,
                                                  Node *arg);
static Expr *get_any_single_vle_path_expr(cypher_parsestate *cpstate,
                                          Node *arg);
static Expr *get_arbitrary_single_vle_path_expr(cypher_parsestate *cpstate,
                                                Node *arg);
static Expr *get_any_vle_edge_expr(cypher_parsestate *cpstate, Node *arg);
static bool is_fixed_one_hop_vle_rel(cypher_relationship *rel);
static Expr *get_visible_single_vle_path_expr_internal(
    cypher_parsestate *cpstate, Node *arg, bool require_fixed_one_hop);
static Expr *get_visible_any_single_vle_path_expr(cypher_parsestate *cpstate,
                                                  Node *arg);
static Node *try_transform_vle_path_length(cypher_parsestate *cpstate,
                                           FuncCall *fn);
static Node *try_transform_vle_path_id_access(cypher_parsestate *cpstate,
                                              FuncCall *fn);
static Node *try_transform_vle_path_boundary_id_function(
    cypher_parsestate *cpstate, FuncCall *fn);
static Node *try_transform_vle_path_endpoint_id_access(
    cypher_parsestate *cpstate, FuncCall *fn);
static Node *try_transform_vle_path_nested_transform_index_endpoint_id_access(
    cypher_parsestate *cpstate, FuncCall *fn);
static Node *transform_vle_path_nested_transform_index_endpoint(
    cypher_parsestate *cpstate, FuncCall *endpoint_fn,
    int64 start_mode_offset, int64 end_mode_offset, int location);
static Node *try_transform_vle_path_boundary_endpoint_id_access(
    cypher_parsestate *cpstate, FuncCall *fn);
static Node *transform_vle_path_boundary_endpoint(
    cypher_parsestate *cpstate, FuncCall *endpoint_fn,
    int64 start_tail_last_mode, int64 end_tail_last_mode,
    Oid start_index_func_oid, Oid end_index_func_oid, int location);
static Node *try_transform_vle_path_slice_boundary_endpoint_id_access(
    cypher_parsestate *cpstate, FuncCall *fn);
static Node *transform_vle_path_slice_boundary_endpoint(
    cypher_parsestate *cpstate, FuncCall *endpoint_fn,
    int64 start_mode_offset, int64 end_mode_offset);
static Node *try_transform_vle_path_endpoint_access(cypher_parsestate *cpstate,
                                                   FuncCall *fn);
static Node *try_transform_vle_path_nested_transform_index_endpoint_access(
    cypher_parsestate *cpstate, FuncCall *fn);
static Node *try_transform_vle_path_boundary_endpoint_access(
    cypher_parsestate *cpstate, FuncCall *fn);
static Node *try_transform_vle_path_slice_boundary_endpoint_access(
    cypher_parsestate *cpstate, FuncCall *fn);
static Node *try_transform_vle_path_endpoint_field(cypher_parsestate *cpstate,
                                                   FuncCall *fn);
static Node *try_transform_vle_path_nested_transform_index_endpoint_field(
    cypher_parsestate *cpstate, FuncCall *fn);
static Node *try_transform_vle_path_slice_boundary_endpoint_field(
    cypher_parsestate *cpstate, FuncCall *fn);
static Node *try_transform_vle_tail_last_endpoint_field(
    cypher_parsestate *cpstate, FuncCall *fn);
static Expr *get_current_any_vle_edge_expr(cypher_parsestate *cpstate,
                                           Node *arg);
static Expr *get_visible_single_vle_path_expr(cypher_parsestate *cpstate,
                                              Node *arg);
static Expr *get_visible_vle_edge_expr(cypher_parsestate *cpstate, Node *arg);
static Node *try_transform_vle_edge_variable_field(
    cypher_parsestate *cpstate, FuncCall *fn);
static Node *try_transform_vle_path_node_field(cypher_parsestate *cpstate,
                                               FuncCall *fn);
static Node *try_transform_vle_path_indexed_keys(cypher_parsestate *cpstate,
                                                 FuncCall *fn);
static Node *try_transform_vle_path_boundary_keys(cypher_parsestate *cpstate,
                                                  FuncCall *fn);
static Node *try_transform_vle_path_endpoint_keys(cypher_parsestate *cpstate,
                                                  FuncCall *fn);
static Node *try_transform_vle_path_visible_boundary_id_function(
    cypher_parsestate *cpstate, FuncCall *fn);
static Node *try_transform_vle_path_visible_boundary_field(
    cypher_parsestate *cpstate, FuncCall *fn);
static bool parse_visible_vle_boundary_endpoint(
    cypher_parsestate *cpstate, Node *node, Expr **vle_expr,
    Node **index_expr, bool *start_endpoint);
static Node *try_transform_vle_path_visible_boundary_endpoint_id(
    cypher_parsestate *cpstate, FuncCall *fn);
static Node *try_transform_vle_path_visible_boundary_endpoint_field(
    cypher_parsestate *cpstate, FuncCall *fn);
static Node *try_transform_vle_path_visible_boundary_endpoint_access(
    cypher_parsestate *cpstate, FuncCall *fn);
static bool parse_vle_path_boundary_list_index(cypher_parsestate *cpstate,
                                               Node *node, Expr **vle_expr,
                                               char **list_name,
                                               int64 *index);
static bool is_vle_direct_path_list_func(Node *node);
static bool parse_vle_normal_or_boundary_index(cypher_parsestate *cpstate,
                                               Node *node, Expr **vle_expr,
                                               Node **index_expr);
static bool parse_vle_nested_transform_edge_equality_index(
    cypher_parsestate *cpstate, Node *node, Expr **vle_expr,
    Node **index_expr, bool *reversed_index);
static bool parse_arbitrary_vle_path_nested_transform_index(
    cypher_parsestate *cpstate, A_Indirection *a_ind, Expr **vle_expr,
    char **list_name, Const **lower_expr, Const **upper_expr, int64 *mode);
static bool parse_vle_edge_endpoint_index(cypher_parsestate *cpstate,
                                           Node *node, Expr **vle_expr,
                                           Node **index_expr,
                                           bool *start_endpoint);
static bool get_vle_endpoint_mode_offset(const char *endpoint_name,
                                         int64 start_offset,
                                         int64 end_offset,
                                         int64 *mode_offset);
static bool parse_vle_endpoint_mode_offset_either(FuncCall *endpoint_fn,
                                                  NodeTag left_type,
                                                  NodeTag right_type,
                                                  int64 start_offset,
                                                  int64 end_offset,
                                                  char **endpoint_name,
                                                  bool *start_endpoint,
                                                  int64 *mode_offset);
static bool parse_vle_endpoint_mode_offset_either_arg(FuncCall *endpoint_fn,
                                                      NodeTag left_type,
                                                      NodeTag right_type,
                                                      int64 start_offset,
                                                      int64 end_offset,
                                                      char **endpoint_name,
                                                      bool *start_endpoint,
                                                      int64 *mode_offset,
                                                      Node **arg);
static bool get_vle_endpoint_field_mode(const char *field_name,
                                        bool start_endpoint, int64 *mode);
static bool get_vle_endpoint_field_mode_offset(const char *field_name,
                                               FuncCall *endpoint_fn,
                                               NodeTag left_type,
                                               NodeTag right_type,
                                               int64 *mode_offset);
static bool get_vle_field_mode_offset(const char *field_name,
                                      int64 label_type_offset,
                                      int64 labels_offset,
                                      int64 properties_offset,
                                      bool allow_type,
                                      int64 *mode_offset);
static bool get_vle_indexed_field_func_oid(const char *list_name,
                                           const char *field_name,
                                           Oid *func_oid);
static bool parse_vle_path_indexed_list_index(cypher_parsestate *cpstate,
                                               A_Indirection *a_ind,
                                               Expr **vle_expr,
                                               char **list_name,
                                               Node **index_expr);
static bool parse_arbitrary_vle_path_indexed_list_index(
    cypher_parsestate *cpstate, A_Indirection *a_ind, Expr **vle_expr,
    char **list_name, Node **index_expr);
static bool parse_vle_tail_reverse_list_slice(cypher_parsestate *cpstate,
                                               FuncCall *outer_fn,
                                               Expr **vle_expr,
                                               int64 *mode,
                                               bool arbitrary_path);
static bool parse_vle_path_list_slice(cypher_parsestate *cpstate,
                                       A_Indirection *a_ind,
                                       Expr **vle_expr,
                                      Node **lower_expr,
                                      Node **upper_expr,
                                      int64 *mode,
                                      bool arbitrary_path);
static Node *try_transform_vle_path_boundary_field(cypher_parsestate *cpstate,
                                                   FuncCall *fn);
static Node *try_transform_vle_path_slice_boundary_field(
    cypher_parsestate *cpstate, FuncCall *fn);
static Node *try_transform_vle_path_nested_transform_index_field(
    cypher_parsestate *cpstate, FuncCall *fn);
static Node *try_transform_vle_path_edge_label(cypher_parsestate *cpstate,
                                               FuncCall *fn);
static Node *try_transform_vle_path_edge_properties(
    cypher_parsestate *cpstate, FuncCall *fn);
static Node *try_transform_vle_path_slice_size(cypher_parsestate *cpstate,
                                               FuncCall *fn);
static Node *try_transform_vle_path_slice_is_empty(cypher_parsestate *cpstate,
                                                   FuncCall *fn);
static Node *transform_vle_path_slice_head_last(cypher_parsestate *cpstate,
                                                FuncCall *fn,
                                                int64 mode_offset);
static Node *try_transform_vle_path_slice_head_last(cypher_parsestate *cpstate,
                                                    FuncCall *fn);
static Node *try_transform_vle_path_slice_boundary_id_function(
    cypher_parsestate *cpstate, FuncCall *fn);
static Node *try_transform_vle_path_nested_transform_index_id_function(
    cypher_parsestate *cpstate, FuncCall *fn);
static bool parse_vle_path_list_function(cypher_parsestate *cpstate,
                                         Node *node, Expr **vle_expr,
                                         char **list_name,
                                         bool allow_visible);
static bool parse_arbitrary_vle_path_list_function(
    cypher_parsestate *cpstate, Node *node, Expr **vle_expr,
    char **list_name);
static bool parse_current_vle_path_list_function(cypher_parsestate *cpstate,
                                                 Node *node,
                                                 Expr **vle_expr,
                                                 char **list_name);
static bool parse_current_or_raw_vle_list(cypher_parsestate *cpstate,
                                          Node *node,
                                          Expr **vle_expr,
                                          char **list_name);
static bool parse_vle_path_or_raw_edge_list(
    cypher_parsestate *cpstate, Node *node, Expr **vle_expr,
    char **list_name, bool allow_visible_path);
static bool parse_arbitrary_vle_path_or_raw_edge_list(
    cypher_parsestate *cpstate, Node *node, Expr **vle_expr,
    char **list_name);
static bool parse_current_raw_vle_edge_list(cypher_parsestate *cpstate,
                                            Node *node,
                                            Expr **vle_expr,
                                            char **list_name);
static bool parse_current_vle_path_or_raw_edge_list_internal(
    cypher_parsestate *cpstate, Node *node, Expr **vle_expr,
    char **list_name, bool require_fixed_one_hop);
static bool parse_current_vle_path_or_fixed_raw_edge_list(
    cypher_parsestate *cpstate, Node *node, Expr **vle_expr,
    char **list_name);
static bool parse_vle_path_or_raw_edge_list_mode(
    cypher_parsestate *cpstate, Node *node, Expr **vle_expr,
    char **list_name, int64 nodes_mode, int64 relationships_mode,
    int64 *mode, bool allow_visible_path);
static bool parse_current_or_raw_vle_nested_list_slice_mode(
    cypher_parsestate *cpstate, Node *node, const char *outer_name,
    const char *inner_name, Expr **vle_expr, int64 *mode,
    bool arbitrary_path);
static bool parse_current_or_raw_vle_tail_reverse_list_slice_mode(
    cypher_parsestate *cpstate, Node *node, const char *outer_name,
    Expr **vle_expr, int64 *mode, bool arbitrary_path);
static bool parse_vle_tail_reverse_source_slice_mode(
    cypher_parsestate *cpstate, const char *outer_name, Node *node,
    Expr **vle_expr, int64 *mode, bool arbitrary_path);
static bool parse_vle_slice_boundary_source_mode(
    cypher_parsestate *cpstate, Node *node, Expr **vle_expr, int64 *mode,
    int64 *mode_flag);
static bool parse_vle_nested_tail_reverse_base_mode(
    cypher_parsestate *cpstate, FuncCall *inner_fn, int64 mode_flag,
    Expr **vle_expr, char **list_name, int64 *mode);
static bool parse_vle_named_path_list_function(
    cypher_parsestate *cpstate, Node *node, const char *expected_name,
    Expr **vle_expr, bool allow_visible);
static Node *try_transform_vle_path_size(cypher_parsestate *cpstate,
                                         FuncCall *fn);
static Node *try_transform_vle_path_is_empty(cypher_parsestate *cpstate,
                                             FuncCall *fn);
static Node *try_transform_vle_path_head_last(cypher_parsestate *cpstate,
                                              FuncCall *fn);
static Node *transform_vle_path_nested_transform_head_last(
    cypher_parsestate *cpstate, FuncCall *fn, int64 mode_offset);
static Node *try_transform_vle_path_nested_transform_head_last(
    cypher_parsestate *cpstate, FuncCall *fn);
static Node *transform_vle_path_any_slice_boundary_head_last(
    cypher_parsestate *cpstate, FuncCall *fn, int64 mode_offset);
static Node *try_transform_vle_path_list_tail_reverse(
    cypher_parsestate *cpstate, FuncCall *fn);
static Node *try_transform_vle_path_relationships(cypher_parsestate *cpstate,
                                                  FuncCall *fn);
static Node *try_transform_vle_path_nodes(cypher_parsestate *cpstate,
                                          FuncCall *fn);
static Node *try_transform_semantic_agtype_builtin(cypher_parsestate *cpstate,
                                                   FuncCall *fn);
static FuncCall *make_unary_func_call(FuncCall *fn, Node *arg, int location);
static A_Indices *make_index_indices(int64 index, int location);
static A_Indirection *make_indexed_indirection(Node *arg, int64 index,
                                               int location);
static FuncCall *make_indexed_unary_func_call(FuncCall *fn, Node *arg,
                                              int64 index, int location);
static char *make_raw_attr_name(const char *var_name, const char *attr_name);
static bool is_internal_alias_name(const char *name);
static bool has_rls_enabled_label(cypher_parsestate *cpstate,
                                  cypher_node *node);
static bool can_direct_rewrite_edge_endpoint(cypher_parsestate *cpstate,
                                             cypher_node *start_node,
                                             cypher_node *end_node);
static Node *make_raw_attr_var(ParseState *pstate, const char *var_name,
                               const char *attr_name, int location);
static bool make_raw_edge_vars(ParseState *pstate, const char *edge_name,
                               int location, Node **id, Node **start_id,
                               Node **end_id, Node **props);
static Node *make_fixed_path_edge_id_var(ParseState *pstate,
                                         cypher_relationship *single_rel,
                                         int location);
static Node *make_fixed_path_edge_endpoint_id_var(
    ParseState *pstate, cypher_relationship *single_rel, bool start_endpoint,
    int location);
static bool make_fixed_endpoint_edge_id_vars(
    ParseState *pstate, cypher_relationship *single_rel,
    const char *endpoint_id_name, int location, Node **edge_id,
    Node **endpoint_id);
static const char *get_fixed_path_endpoint_vertex_name(
    cypher_node *start_node, cypher_node *end_node, const char *endpoint_name);
static Node *make_raw_vertex_props_var(ParseState *pstate,
                                       const char *vertex_name,
                                       int location);
static bool make_raw_vertex_id_props_vars(ParseState *pstate,
                                          const char *vertex_name,
                                          int location,
                                          Node **vertex_id,
                                          Node **vertex_props);
static bool make_fixed_endpoint_vertex_vars(
    cypher_parsestate *cpstate, cypher_node *start_node,
    cypher_relationship *single_rel, cypher_node *end_node,
    const char *endpoint_name, int location,
    Node **edge_id, Node **vertex_id, Node **vertex_props);
static bool make_fixed_endpoint_vertex_props_var(
    cypher_parsestate *cpstate, cypher_node *start_node,
    cypher_relationship *single_rel, cypher_node *end_node,
    const char *endpoint_name, int location,
    Node **edge_id, Node **vertex_props);
static bool make_edge_endpoint_vertex_vars(
    cypher_parsestate *cpstate, const char *edge_name,
    const char *endpoint_name, int location, Node **edge_id,
    Node **vertex_id, Node **vertex_props);
static char *make_raw_edges_name(const char *var_name);
static char *make_fixed_path_indexed_raw_attr_name(
    char *list_name, int64 index, cypher_node *start_node,
    cypher_relationship *single_rel, cypher_node *end_node,
    const char *edge_attr_name, const char *vertex_attr_name);
static Node *make_fixed_path_indexed_raw_attr_var(
    ParseState *pstate, char *list_name, int64 index,
    cypher_node *start_node, cypher_relationship *single_rel,
    cypher_node *end_node, const char *edge_attr_name,
    const char *vertex_attr_name, int location);
static bool make_fixed_path_indexed_id_vars(
    ParseState *pstate, char *list_name, int64 index,
    cypher_node *start_node, cypher_relationship *single_rel,
    cypher_node *end_node, const char *edge_attr_name, int location,
    Node **edge_id, Node **target_id);
static bool make_fixed_path_indexed_props_vars(
    ParseState *pstate, char *list_name, int64 index,
    cypher_node *start_node, cypher_relationship *single_rel,
    cypher_node *end_node, int location, Node **edge_id, Node **target_props);
static bool is_nodes_list_name(const char *list_name);
static bool is_relationships_list_name(const char *list_name);
static bool is_path_list_name(const char *list_name);
static bool is_equal_operator_name(List *name);
static bool is_head_name(const char *func_name);
static bool is_last_name(const char *func_name);
static bool is_tail_name(const char *func_name);
static bool is_reverse_name(const char *func_name);
static bool is_size_name(const char *func_name);
static bool is_is_empty_name(const char *func_name);
static bool is_size_is_empty_name(const char *func_name);
static bool is_head_last_name(const char *func_name);
static bool is_tail_reverse_name(const char *func_name);
static bool is_id_name(const char *func_name);
static bool is_keys_name(const char *func_name);
static bool is_label_name(const char *func_name);
static bool is_labels_name(const char *func_name);
static bool is_properties_name(const char *func_name);
static bool is_type_name(const char *func_name);
static bool is_label_type_name(const char *func_name);
static bool is_vle_edge_field_name(const char *field_name);
static bool is_start_edge_endpoint_id_name(const char *func_name);
static bool is_end_edge_endpoint_id_name(const char *func_name);
static bool is_edge_endpoint_id_name(const char *func_name);
static const char *get_edge_endpoint_id_col_name(const char *func_name);
static bool get_vle_nested_tail_reverse_mode_flag(const char *outer_name,
                                                  const char *inner_name,
                                                  int64 *mode_flag);
static bool get_vle_nested_count_mode_flags(const char *outer_name,
                                            const char *inner_name,
                                            bool *tail_mode,
                                            bool *double_tail);
static bool get_vle_tail_reverse_list_slice_mode(const char *outer_name,
                                                 bool node_list,
                                                 int64 *mode);
static int64 get_vle_tail_reverse_access_index(bool tail,
                                               int64 index);
static bool get_vle_head_last_tail_reverse_index(const char *outer_name,
                                                 const char *inner_name,
                                                 int64 *index,
                                                 bool *tail_last);
static bool get_vle_nested_list_slice_mode(const char *outer_name,
                                           const char *inner_name,
                                           bool node_list, int64 *mode);
static Oid get_vle_tail_reverse_materialize_oid(const char *list_name,
                                                const char *outer_name);
static Oid get_vle_tail_last_materialize_oid(const char *list_name);
static Oid get_vle_tail_last_id_oid(const char *list_name);
static bool get_vle_tail_last_field_mode(const char *list_name,
                                         const char *field_name,
                                         int64 *mode);
static Oid get_vle_list_materialize_oid(const char *list_name);
static Oid get_vle_list_count_oid(const char *list_name, bool tail_mode);
static int64 get_vle_list_is_empty_mode(const char *list_name,
                                        bool tail_mode);
static Node *transform_vle_path_materialized_list(
    cypher_parsestate *cpstate, FuncCall *fn, const char *list_name);
static bool is_unary_func(FuncCall *fn);
static bool is_unary_func_arg(FuncCall *fn, NodeTag arg_type);
static bool is_single_unary_func_arg(FuncCall *fn, NodeTag arg_type);
static bool is_unary_func_arg_either(FuncCall *fn, NodeTag left_type,
                                     NodeTag right_type);
static bool is_single_func_name(FuncCall *fn);
static bool parse_single_func_name(FuncCall *fn, char **func_name);
static bool parse_single_unary_func_name(FuncCall *fn, char **func_name);
static bool parse_single_unary_func_arg_name(FuncCall *fn, NodeTag arg_type,
                                              char **func_name);
static bool parse_head_last_func_name(FuncCall *fn, char **head_last_name);
static bool parse_head_last_func_arg_name(FuncCall *fn, NodeTag arg_type,
                                          char **head_last_name);
static bool parse_head_last_func_arg(FuncCall *fn, NodeTag arg_type,
                                     char **head_last_name, Node **arg);
static bool parse_head_last_func_any_arg(FuncCall *fn, char **head_last_name,
                                         Node **arg);
static bool parse_tail_reverse_func_name(FuncCall *fn, char **tail_reverse_name);
static bool parse_tail_reverse_func_arg_name(FuncCall *fn, NodeTag arg_type,
                                             char **tail_reverse_name);
static bool parse_tail_reverse_func_arg(FuncCall *fn, NodeTag arg_type,
                                        char **tail_reverse_name, Node **arg);
static bool parse_tail_reverse_func_any_arg(FuncCall *fn,
                                            char **tail_reverse_name,
                                            Node **arg);
static bool parse_path_list_func_name(FuncCall *fn, char **list_name);
static bool parse_path_list_func_arg_name(FuncCall *fn, NodeTag arg_type,
                                          char **list_name);
static bool parse_path_list_func_arg(FuncCall *fn, NodeTag arg_type,
                                     char **list_name, Node **arg);
static bool parse_path_list_or_tail_reverse_func_name(FuncCall *fn,
                                                      char **func_name);
static bool parse_path_list_or_tail_reverse_func_arg(FuncCall *fn,
                                                     char **func_name,
                                                     Node **arg);
static bool parse_func_any_arg(FuncCall *fn, const char *func_name,
                               Node **arg);
static bool parse_func_arg(FuncCall *fn, const char *func_name,
                           NodeTag arg_type, Node **arg);
static bool parse_id_func_arg(FuncCall *fn, NodeTag arg_type, Node **arg);
static bool parse_edge_endpoint_id_func_arg(FuncCall *fn, NodeTag arg_type,
                                            char **func_name, Node **arg);
static bool parse_graphid_func_arg(FuncCall *fn, NodeTag arg_type,
                                   char **func_name, Node **arg);
static bool parse_size_is_empty_func_arg(FuncCall *fn, NodeTag arg_type,
                                         char **func_name, Node **arg);
static bool parse_label_type_func_arg(FuncCall *fn, NodeTag arg_type,
                                      char **func_name, Node **arg);
static bool parse_endpoint_func_arg(FuncCall *fn, NodeTag arg_type,
                                    char **endpoint_name, Node **arg);
static bool parse_endpoint_func_arg_name(FuncCall *fn, NodeTag arg_type,
                                         char **endpoint_name);
static bool parse_endpoint_func_any_arg(FuncCall *fn, char **endpoint_name,
                                        bool *start_endpoint, Node **arg);
static bool parse_edge_endpoint_columnref_arg(FuncCall *endpoint_fn,
                                              char **endpoint_name,
                                              char **edge_name);
static bool parse_keys_func_arg(FuncCall *fn, NodeTag arg_type, Node **arg);
static bool is_func_name_unary(FuncCall *fn, const char *func_name);
static bool is_func_name_unary_arg(FuncCall *fn, const char *func_name,
                                   NodeTag arg_type);
static bool is_func_name_either_unary_arg(FuncCall *fn,
                                          const char *left_name,
                                          const char *right_name,
                                          NodeTag arg_type);
static bool is_vle_field_name(const char *field_name, bool allow_type);
static bool is_endpoint_function_name(const char *func_name);
static bool is_start_endpoint_function_name(const char *func_name);
static bool parse_vle_endpoint_function_name(FuncCall *fn,
                                             char **endpoint_name,
                                             bool *start_endpoint);
static bool parse_endpoint_func_name(FuncCall *fn, char **endpoint_name);
static bool is_fixed_path_indexed_consumer_name(char *func_name);
static bool parse_fixed_path_indexed_consumer_call(FuncCall *fn,
                                                   NodeTag arg_type,
                                                   char **func_name);
static bool parse_fixed_path_indexed_consumer_arg_name(FuncCall *fn,
                                                       NodeTag arg_type,
                                                       char **func_name);
static bool is_fixed_path_endpoint_vertex_consumer_name(char *func_name);
static bool parse_fixed_path_endpoint_vertex_consumer_call(FuncCall *fn,
                                                           char **func_name);
static bool parse_fixed_path_endpoint_vertex_consumer_call_arg(FuncCall *fn,
                                                               char **func_name,
                                                               Node **arg);
static bool parse_fixed_path_endpoint_vertex_consumer_arg_name(
    FuncCall *fn, NodeTag arg_type, char **func_name);
static Node *try_transform_fixed_path_indexed_consumer(
    cypher_parsestate *cpstate, char *func_name, FuncCall *indexed_consumer);
static Node *try_transform_fixed_path_indexed_consumer_at(
    cypher_parsestate *cpstate, char *func_name, FuncCall *fn,
    FuncCall *list_fn, int64 index);
static bool get_nonnegative_integer_const(Node *node, int64 *value);
static bool get_nonnegative_integer_const_or_aconst(Node *node, int64 *value);
static bool parse_vle_bounds(FuncCall *fn, int64 *lower, int64 *upper);
static bool parse_optional_nonnegative_bound(Node *node, int64 *value);
static bool parse_optional_upper_bound(Node *node, int64 *upper,
                                       int64 default_upper);
static bool get_fixed_path_list_len(const char *list_name, int64 *list_len);
static bool get_fixed_path_head_last_index(const char *head_last_name,
                                           const char *list_name,
                                           bool tail_transform,
                                           int64 *index);
static bool parse_fixed_slice_bounds(A_Indices *indices, int64 list_len,
                                     int64 *lower, int64 *upper);
static bool parse_nonempty_fixed_slice_bounds(A_Indices *indices,
                                              int64 list_len,
                                              const char *transform_name,
                                              int64 *lower, int64 *upper);
static void clamp_slice_bounds_to_len(int64 *lower, int64 *upper,
                                      int64 list_len);
static void advance_tail_slice_lower_bound(int64 *lower, int64 upper);
static int64 get_slice_boundary_index(const char *head_last_name,
                                      bool reversed, int64 lower,
                                      int64 upper);
static bool has_nonempty_slice_bounds(int64 lower, int64 upper);
static bool parse_single_indirection_string(A_Indirection *a_ind,
                                            char **field_name);
static bool has_indirection_count(A_Indirection *a_ind, int min_count);
static bool parse_leading_indirection_index(A_Indirection *a_ind,
                                            A_Indices **indices);
static bool parse_single_indirection_index(A_Indirection *a_ind,
                                           A_Indices **indices);
static bool parse_single_indirection_slice(A_Indirection *a_ind,
                                           A_Indices **indices);
static bool parse_head_last_slice_context(Node *node, FuncCall **list_fn,
                                          char **list_name,
                                          char **head_last_name,
                                          char **transform_name,
                                          A_Indices **slice_indices);
static bool has_single_index(A_Indices *indices);
static bool parse_single_indirection_value_index(A_Indirection *a_ind,
                                                 A_Indices **indices);
static bool parse_nonnegative_index(A_Indices *indices, int64 *index);
static bool parse_single_indirection_nonnegative_index(A_Indirection *a_ind,
                                                       A_Indices **indices,
                                                       int64 *index);
static bool parse_leading_indirection_nonnegative_index(A_Indirection *a_ind,
                                                       int64 *index);
static bool parse_zero_index(A_Indices *indices);
static bool parse_single_indirection_zero_index(A_Indirection *a_ind);
static Const *make_agtype_string_key_const(Node *node);
static bool append_agtype_access_indirections(cypher_parsestate *cpstate,
                                              List **args, List *indirections,
                                              bool skip_first);
static bool parse_fixed_relationship_slice_head_last_index(
    Node *node, FuncCall **relationships_fn, int64 *index);
static bool parse_vle_path_nested_transform_index(cypher_parsestate *cpstate,
                                                  A_Indirection *a_ind,
                                                  Expr **vle_expr,
                                                  char **list_name,
                                                  Const **lower_expr,
                                                  Const **upper_expr,
                                                  int64 *mode);
static Oid get_vle_indexed_id_func_oid(const char *list_name);
static Node *make_vle_indexed_field_expr(const char *list_name,
                                         const char *field_name,
                                         Expr *vle_expr, Node *index_expr);
static Node *make_vle_path_indexed_field_expr(cypher_parsestate *cpstate,
                                              A_Indirection *a_ind,
                                              const char *field_name,
                                              bool require_nodes,
                                              bool require_relationships);
static Node *make_current_vle_edge_indexed_field_expr(
    cypher_parsestate *cpstate, A_Indirection *a_ind, const char *field_name);
static Node *try_transform_vle_path_tail_access(cypher_parsestate *cpstate,
                                                A_Indirection *a_ind);
static Node *try_transform_vle_path_reverse_access(cypher_parsestate *cpstate,
                                                   A_Indirection *a_ind);
static Node *try_transform_vle_path_nested_transform_access(
    cypher_parsestate *cpstate, A_Indirection *a_ind);
static Node *try_transform_vle_path_nodes_access(cypher_parsestate *cpstate,
                                                 A_Indirection *a_ind);
static Node *try_transform_vle_path_nodes_slice(cypher_parsestate *cpstate,
                                                A_Indirection *a_ind);
static Node *try_transform_vle_path_list_slice(cypher_parsestate *cpstate,
                                               A_Indirection *a_ind);
static Node *try_transform_vle_terminal_vertex_property_access(
    cypher_parsestate *cpstate, A_Indirection *a_ind);
static bool retarget_vle_terminal_property_output(cypher_parsestate *cpstate,
                                                  Node *vle_path,
                                                  Const *key_const);
static bool retarget_vle_terminal_properties_output(
    cypher_parsestate *cpstate, Node *raw_properties);
static FuncExpr *make_vle_index_properties_expr(cypher_parsestate *cpstate,
                                                A_Indirection *indexed_arg,
                                                int location);
static Node *make_vle_indexed_properties_keys_expr(const char *list_name,
                                                   Expr *vle_expr,
                                                   Node *index_expr,
                                                   int location);
static Node *try_transform_vle_path_boundary_property_access(
    cypher_parsestate *cpstate, A_Indirection *a_ind);
static Node *try_transform_vle_path_relationships_access(
    cypher_parsestate *cpstate, A_Indirection *a_ind);
static Node *try_transform_vle_edge_reverse_access(cypher_parsestate *cpstate,
                                                   A_Indirection *a_ind);
static List *cast_agtype_args_to_target_type(cypher_parsestate *cpstate,
                                             Form_pg_proc procform,
                                             List *fargs,
                                             Oid *target_types);
static Node *wrap_text_output_to_agtype(cypher_parsestate *cpstate,
                                        FuncExpr *fexpr);
static Form_pg_proc get_procform(FuncCall *fn, bool err_not_found);
static const char *get_mapped_extension(Oid func_oid);
static bool function_belongs_to_extension(Oid func_oid, const char *extension);
static bool is_extension_external(const char *extension);
static bool function_needs_graph_name_argument(const char *name, int name_len);
static char *construct_age_function_name(char *funcname, int funcname_len);
static void initialize_function_caches(void);
static void invalidate_function_caches(Datum arg, int cache_id,
                                       uint32 hash_value);
static void initialize_function_extension_cache(void);
static void initialize_function_exists_cache(void);
static Oid get_agtype_access_operator_oid(void);
static Oid get_agtype_object_field_agtype_oid(void);
static Oid get_agtype_object_field_int8_oid(void);
static Oid get_agtype_object_field_float8_oid(void);
static Oid get_agtype_object_field_text_agtype_oid(void);
static Oid get_agtype_object_field_numeric_agtype_oid(void);
static Oid get_agtype_object_field_numeric_oid(void);
static Oid get_agtype_access_slice_oid(void);
static Oid get_agtype_in_operator_oid(void);
static Oid get_agtype_add_oid(void);
static Oid get_agtype_build_empty_list_oid(void);
static Oid get_agtype_build_list_oid(void);
static Oid get_agtype_build_empty_map_oid(void);
static Oid get_agtype_build_map_oid(void);
static Oid get_agtype_build_map_nonull_oid(void);
static Oid get_age_properties_oid(void);
static Oid get_age_keys_oid(void);
static Oid get_build_vertex_label_oid(void);
static Oid get_build_edge_label_oid(void);
static Oid get_agtype_string_match_starts_with_oid(void);
static Oid get_agtype_string_match_ends_with_oid(void);
static Oid get_agtype_string_match_contains_oid(void);
static Oid get_text_to_agtype_oid(void);
static Oid get_label_name_oid(void);
static Oid get_age_vle_path_length_oid(void);
static Oid get_age_vle_path_node_count_oid(void);
static Oid get_age_vle_edge_tail_count_oid(void);
static Oid get_age_vle_list_is_empty_oid(void);
static Oid get_age_vle_list_slice_count_oid(void);
static Oid get_age_vle_list_slice_is_empty_oid(void);
static Oid get_age_materialize_vle_list_slice_oid(void);
static Oid get_age_materialize_vle_slice_boundary_oid(void);
static Oid get_age_materialize_vle_edges_oid(void);
static Oid get_age_materialize_vle_nodes_oid(void);
static Oid get_age_materialize_vle_node_at_oid(void);
static Oid get_age_materialize_vle_node_tail_last_oid(void);
static Oid get_age_vle_node_tail_last_id_oid(void);
static Oid get_age_vle_node_id_at_oid(void);
static Oid get_age_vle_node_properties_at_oid(void);
static Oid get_age_vle_node_property_at_oid(void);
static Oid get_age_materialize_vle_nodes_slice_oid(void);
static Oid get_age_materialize_vle_nodes_tail_oid(void);
static Oid get_age_materialize_vle_nodes_reversed_oid(void);
static Oid get_age_materialize_vle_edge_at_oid(void);
static Oid get_age_materialize_vle_edge_reversed_at_oid(void);
static Oid get_age_materialize_vle_edge_tail_last_oid(void);
static Oid get_age_vle_edge_tail_last_id_oid(void);
static Oid get_age_vle_tail_last_field_oid(void);
static Oid get_age_vle_tail_last_edge_endpoint_oid(void);
static Oid get_age_vle_tail_last_endpoint_field_oid(void);
static Oid get_age_vle_edge_id_at_oid(void);
static Oid get_age_vle_edge_index_exists_oid(void);
static Oid get_age_vle_edge_indices_equal_oid(void);
static Oid get_age_vle_edge_reversed_index_equal_oid(void);
static Oid get_age_vle_edge_label_at_oid(void);
static Oid get_age_vle_edge_properties_at_oid(void);
static Oid get_age_vle_edge_property_at_oid(void);
static Oid get_age_vle_edge_start_node_at_oid(void);
static Oid get_age_vle_edge_end_node_at_oid(void);
static Oid get_age_vle_edge_endpoint_field_at_oid(void);
static Oid get_age_vle_edge_start_id_at_oid(void);
static Oid get_age_vle_edge_end_id_at_oid(void);
static Oid get_age_materialize_vle_edges_tail_oid(void);
static Oid get_age_materialize_vle_edges_reversed_oid(void);
static Oid get_age_vle_terminal_vertex_oid(void);
static Oid get_age_vle_terminal_vertex_properties_oid(void);
static Oid get_age_vle_terminal_vertex_property_oid(void);
static Oid get_age_vle_terminal_vertex_property_from_path_oid(void);
static bool function_exists(char *funcname, char *extension);
static Node *coerce_expr_flexible(ParseState *pstate, Node *expr,
                                  Oid source_oid, Oid target_oid,
                                  int32 t_typemod, bool error_out);

/* transform a cypher expression */
Node *transform_cypher_expr(cypher_parsestate *cpstate, Node *expr,
                            ParseExprKind expr_kind)
{
    ParseState *pstate = (ParseState *)cpstate;
    ParseExprKind old_expr_kind;
    Node *result;

    /* save and restore identity of expression type we're parsing */
    Assert(expr_kind != EXPR_KIND_NONE);
    old_expr_kind = pstate->p_expr_kind;
    pstate->p_expr_kind = expr_kind;

    result = transform_cypher_expr_recurse(cpstate, expr);

    pstate->p_expr_kind = old_expr_kind;

    return result;
}

static Node *transform_cypher_expr_recurse(cypher_parsestate *cpstate,
                                           Node *expr)
{
    if (!expr)
        return NULL;

    /* guard against stack overflow due to overly complex expressions */
    check_stack_depth();

    switch (nodeTag(expr))
    {
    case T_A_Const:
        return transform_A_Const(cpstate, (A_Const *)expr);
    case T_ColumnRef:
        return transform_ColumnRef(cpstate, (ColumnRef *)expr);
    case T_A_Indirection:
        return transform_A_Indirection(cpstate, (A_Indirection *)expr);
    case T_A_Expr:
    {
        A_Expr *a = (A_Expr *)expr;

        switch (a->kind)
        {
        case AEXPR_OP:
            return transform_AEXPR_OP(cpstate, a);
        case AEXPR_IN:
            return transform_AEXPR_IN(cpstate, a);
        default:
            ereport(ERROR, (errmsg_internal("unrecognized A_Expr kind: %d",
                                            a->kind)));
        }
        break;
    }
    case T_BoolExpr:
        return transform_BoolExpr(cpstate, (BoolExpr *)expr);
    case T_NullTest:
    {
        NullTest *n = (NullTest *)expr;
        NullTest *transformed_expr = makeNode(NullTest);
        Node *raw_null_test;

        raw_null_test = try_transform_entity_null_test(cpstate, n);
        if (raw_null_test != NULL)
            return raw_null_test;

        raw_null_test = try_transform_edge_endpoint_id_null_test(cpstate, n);
        if (raw_null_test != NULL)
            return raw_null_test;

        raw_null_test = try_transform_edge_endpoint_null_test(cpstate, n);
        if (raw_null_test != NULL)
            return raw_null_test;

        transformed_expr->arg = (Expr *)transform_cypher_expr_recurse(cpstate,
                                                         (Node *)n->arg);
        transformed_expr->nulltesttype = n->nulltesttype;
        transformed_expr->argisrow = type_is_rowtype(exprType((Node *)transformed_expr->arg));
        transformed_expr->location = n->location;

        return (Node *) transformed_expr;
    }
    case T_CaseExpr:
        return transform_CaseExpr(cpstate, (CaseExpr *) expr);
    case T_CaseTestExpr:
        return expr;
    case T_CoalesceExpr:
        return transform_CoalesceExpr(cpstate, (CoalesceExpr *) expr);
    case T_ExtensibleNode:
    {
        if (is_ag_node(expr, cypher_bool_const))
        {
            return transform_cypher_bool_const(cpstate,
                                               (cypher_bool_const *)expr);
        }
        if (is_ag_node(expr, cypher_integer_const))
        {
            return transform_cypher_integer_const(cpstate,
                                                  (cypher_integer_const *)expr);
        }
        if (is_ag_node(expr, cypher_param))
        {
            return transform_cypher_param(cpstate, (cypher_param *)expr);
        }
        if (is_ag_node(expr, cypher_map))
        {
            return transform_cypher_map(cpstate, (cypher_map *)expr);
        }
        if (is_ag_node(expr, cypher_map_projection))
        {
            return transform_cypher_map_projection(
                cpstate, (cypher_map_projection *)expr);
        }
        if (is_ag_node(expr, cypher_list))
        {
            return transform_cypher_list(cpstate, (cypher_list *)expr);
        }
        if (is_ag_node(expr, cypher_string_match))
        {
            return transform_cypher_string_match(cpstate,
                                                 (cypher_string_match *)expr);
        }
        if (is_ag_node(expr, cypher_typecast))
        {
            return transform_cypher_typecast(cpstate,
                                             (cypher_typecast *)expr);
        }
        if (is_ag_node(expr, cypher_comparison_aexpr))
        {
            return transform_cypher_comparison_aexpr_OP(cpstate,
                                             (cypher_comparison_aexpr *)expr);
        }
        if (is_ag_node(expr, cypher_comparison_boolexpr))
        {
            return transform_cypher_comparison_boolexpr(cpstate,
                                             (cypher_comparison_boolexpr *)expr);
        }
        ereport(ERROR,
                (errmsg_internal("unrecognized ExtensibleNode: %s",
                                 ((ExtensibleNode *)expr)->extnodename)));

        return NULL;
    }
    case T_FuncCall:
        return transform_FuncCall(cpstate, (FuncCall *)expr);
    case T_SubLink:
        return transform_SubLink(cpstate, (SubLink *)expr);
    case T_Const:
    case T_FuncExpr:
    case T_Var:
    case T_OpExpr:
    case T_ScalarArrayOpExpr:
    case T_RelabelType:
    case T_CoerceViaIO:
    case T_ArrayCoerceExpr:
        /* Already transformed */
        return expr;
    default:
        ereport(ERROR, (errmsg_internal("unrecognized node type in transform_cypher_expr_recurse: %d",
                                        nodeTag(expr))));
    }
    return NULL;
}

static Node *transform_A_Const(cypher_parsestate *cpstate, A_Const *ac)
{
    ParseState *pstate = (ParseState *)cpstate;
    ParseCallbackState pcbstate;

    Datum d = (Datum)0;
    bool is_null = false;
    Const *c;

    setup_parser_errposition_callback(&pcbstate, pstate, ac->location);
    if (ac->isnull)
    {
        is_null = true;
        goto make_const;
    }

    switch (nodeTag(&ac->val))
    {
    case T_Integer:
        d = integer_to_agtype((int64)intVal(&ac->val));
        break;
    case T_Float:
        {
	    char *n = ac->val.sval.sval;
            char *endptr;
            int64 i;
            errno = 0;

            i = strtoi64(ac->val.fval.fval, &endptr, 10);

            if (errno == 0 && *endptr == '\0')
            {
                d = integer_to_agtype(i);
            }
            else
            {
                float8 f = float8in_internal(n, NULL, "double precision", n,
                                             NULL);

                d = float_to_agtype(f);
            }
        }
        break;
    case T_String:
        d = string_to_agtype(strVal(&ac->val));
        break;
    case T_Boolean:
        d = boolean_to_agtype(boolVal(&ac->val));
        break;
    default:
        ereport(ERROR, (errmsg_internal("unrecognized node type: %d",
                                        nodeTag(&ac->val))));
        return NULL;
    }
make_const:
    cancel_parser_errposition_callback(&pcbstate);

    /* typtypmod, typcollation, typlen, and typbyval of agtype are hard-coded. */
    c = makeConst(AGTYPEOID, -1, InvalidOid, -1, d, is_null, false);
    c->location = ac->location;
    return (Node *)c;
}

/*
 * Private function borrowed from PG's transformWholeRowRef.
 * Construct a whole-row reference to represent the notation "relation.*".
 */
static Node *transform_WholeRowRef(ParseState *pstate, ParseNamespaceItem *pnsi,
                                   int location, int sublevels_up)
{
    Var *result;
    int vnum;
    RangeTblEntry *rte;

    Assert(pnsi->p_rte != NULL);
    rte = pnsi->p_rte;

    /* Find the RTE's rangetable location */
    vnum = pnsi->p_rtindex;

    /*
     * Build the appropriate referencing node.  Note that if the RTE is a
     * function returning scalar, we create just a plain reference to the
     * function value, not a composite containing a single column.  This is
     * pretty inconsistent at first sight, but it's what we've done
     * historically.  One argument for it is that "rel" and "rel.*" mean the
     * same thing for composite relations, so why not for scalar functions...
     */
     result = makeWholeRowVar(rte, vnum, sublevels_up, true);

     /* location is not filled in by makeWholeRowVar */
     result->location = location;

     /* mark relation as requiring whole-row SELECT access */
     markVarForSelectPriv(pstate, result);

     return (Node *)result;
}

/*
 * Function to transform a ColumnRef node from the grammar into a Var node
 * Code borrowed from PG's transformColumnRef.
 */
static Node *transform_ColumnRef(cypher_parsestate *cpstate, ColumnRef *cref)
{
    ParseState *pstate = (ParseState *)cpstate;
    Node *field1 = NULL;
    Node *field2 = NULL;
    char *colname = NULL;
    char *nspname = NULL;
    char *relname = NULL;
    Node *node = NULL;
    ParseNamespaceItem *pnsi = NULL;
    int levels_up;

    switch (list_length(cref->fields))
    {
        case 1:
            {
                transform_entity *te;
                field1 = (Node*)linitial(cref->fields);

                Assert(IsA(field1, String));
                colname = strVal(field1);

                te = find_variable(cpstate, colname);
                if (te != NULL &&
                    te->type == ENT_EDGE &&
                    te->has_raw_targets)
                {
                    node = try_build_edge_from_raw_attrs(cpstate, colname,
                                                         cref->location);
                    if (node != NULL)
                        break;
                }

                /* Try to identify as an unqualified column */
                node = colNameToVar(pstate, colname, false, cref->location);
                if (node != NULL)
                {
                        break;
                }

                /*
                 * Try to find the columnRef as a transform_entity
                 * and extract the expr.
                 */
                if (te != NULL && te->expr != NULL &&
                    te->declared_in_current_clause)
                {
                    node = (Node *)te->expr;
                    break;
                }
                if (te != NULL && te->type == ENT_EDGE && te->has_raw_targets)
                {
                    node = try_build_edge_from_raw_attrs(cpstate, colname,
                                                         cref->location);
                    if (node != NULL)
                        break;
                }
                /*
                 * Not known as a column of any range-table entry.
                 * Try to find the name as a relation.  Note that only
                 * relations already entered into the rangetable will be
                 * recognized.
                 *
                 * This is a hack for backwards compatibility with
                 * PostQUEL-inspired syntax.  The preferred form now is
                 * "rel.*".
                 */
                pnsi = refnameNamespaceItem(pstate, NULL, colname,
                                           cref->location, &levels_up);
                if (pnsi)
                {
                    node = transform_WholeRowRef(pstate, pnsi, cref->location,
                                                 levels_up);
                }
                else
                {
                    ereport(ERROR,
                            (errcode(ERRCODE_UNDEFINED_COLUMN),
                             errmsg("could not find rte for %s", colname),
                             errhint("variable %s does not exist within scope of usage",
                                     colname),
                             parser_errposition(pstate, cref->location)));
                }

                if (node == NULL)
                {
                    ereport(ERROR, (errcode(ERRCODE_DATA_EXCEPTION),
                            errmsg("unable to transform whole row for %s",
                                   colname),
                             parser_errposition(pstate, cref->location)));
                }

                break;
            }
        case 2:
            {
                Oid inputTypeId = InvalidOid;
                Oid targetTypeId = InvalidOid;

                field1 = (Node*)linitial(cref->fields);
                field2 = (Node*)lsecond(cref->fields);

                Assert(IsA(field1, String));
                relname = strVal(field1);

                if (IsA(field2, String))
                {
                    colname = strVal(field2);
                }

                /* locate the referenced RTE */
                pnsi = refnameNamespaceItem(pstate, nspname, relname,
                                           cref->location, &levels_up);

                if (pnsi == NULL)
                {
                    ereport(ERROR,
                            (errcode(ERRCODE_UNDEFINED_COLUMN),
                             errmsg("could not find rte for %s.%s", relname,
                                    colname),
                             parser_errposition(pstate, cref->location)));
                    break;
                }

                /*
                 * TODO: Left in for potential future use.
                 * Is it a whole-row reference?
                 */
                if (IsA(field2, A_Star))
                {
                    node = transform_WholeRowRef(pstate, pnsi, cref->location,
                                                 levels_up);
                    break;
                }

                Assert(IsA(field2, String));

                /* try to identify as a column of the RTE */
                node = scanNSItemForColumn(pstate, pnsi, levels_up, colname,
                                           cref->location);

                if (node == NULL)
                {
                    ereport(ERROR,
                            (errcode(ERRCODE_UNDEFINED_COLUMN),
                             errmsg("could not find column %s in rel %s of rte",
                                    colname, relname),
                             parser_errposition(pstate, cref->location)));
                }

                /* coerce it to AGTYPE if possible */
                inputTypeId = exprType(node);
                targetTypeId = AGTYPEOID;

                if (can_coerce_type(1, &inputTypeId, &targetTypeId,
                                    COERCION_EXPLICIT))
                {
                    node = coerce_type(pstate, node, inputTypeId, targetTypeId,
                                       -1, COERCION_EXPLICIT,
                                       COERCE_EXPLICIT_CAST, -1);
                }
                break;
            }
        default:
            {
                ereport(ERROR,
                        (errcode(ERRCODE_SYNTAX_ERROR),
                         errmsg("improper qualified name (too many dotted names): %s",
                                NameListToString(cref->fields)),
                         parser_errposition(pstate, cref->location)));
                break;
            }
    }

    if (node == NULL)
    {
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_COLUMN),
                 errmsg("variable `%s` does not exist", colname),
                 parser_errposition(pstate, cref->location)));
    }

    return node;
}

static Node *transform_AEXPR_OP(cypher_parsestate *cpstate, A_Expr *a)
{
    ParseState *pstate = (ParseState *)cpstate;
    Node *last_srf = pstate->p_last_srf;
    Node *fast_expr = NULL;
    Node *lexpr = NULL;
    Node *rexpr = NULL;

    fast_expr = try_transform_vle_edge_index_equality(cpstate, a);
    if (fast_expr != NULL)
    {
        return fast_expr;
    }
    fast_expr = try_transform_vle_edge_boundary_equality(cpstate, a);
    if (fast_expr != NULL)
    {
        return fast_expr;
    }
    fast_expr = try_transform_vle_edge_reversed_equality(cpstate, a);
    if (fast_expr != NULL)
    {
        return fast_expr;
    }
    fast_expr = try_transform_vle_edge_nested_transform_equality(cpstate, a);
    if (fast_expr != NULL)
    {
        return fast_expr;
    }
    fast_expr = try_transform_vle_edge_normalized_equality(cpstate, a);
    if (fast_expr != NULL)
    {
        return fast_expr;
    }
    fast_expr = try_transform_entity_id_equality(cpstate, a);
    if (fast_expr != NULL)
    {
        return fast_expr;
    }
    fast_expr = try_transform_edge_endpoint_id_equality(cpstate, a);
    if (fast_expr != NULL)
    {
        return fast_expr;
    }

    lexpr = transform_cypher_expr_recurse(cpstate, a->lexpr);
    rexpr = transform_cypher_expr_recurse(cpstate, a->rexpr);
    coerce_pg_scalar_property_comparison(cpstate, a, &lexpr, &rexpr);

    return (Node *)make_op(pstate, a->name, lexpr, rexpr, last_srf,
                           a->location);
}

static void coerce_pg_scalar_property_comparison(cypher_parsestate *cpstate,
                                                 A_Expr *a, Node **left,
                                                 Node **right)
{
    ParseState *pstate = (ParseState *)cpstate;
    Oid scalar_type = InvalidOid;

    if (a == NULL || left == NULL || right == NULL ||
        !is_scalar_comparison_operator(a->name))
        return;

    if (is_pg_scalar_property_expr(*left, &scalar_type) &&
        exprType(*right) == AGTYPEOID)
    {
        Node *coerced;

        coerced = coerce_to_target_type(pstate, *right, AGTYPEOID,
                                        scalar_type, -1,
                                        COERCION_EXPLICIT,
                                        COERCE_EXPLICIT_CAST,
                                        a->location);
        if (coerced != NULL)
            *right = coerced;
        return;
    }

    if (is_pg_scalar_property_expr(*right, &scalar_type) &&
        exprType(*left) == AGTYPEOID)
    {
        Node *coerced;

        coerced = coerce_to_target_type(pstate, *left, AGTYPEOID,
                                        scalar_type, -1,
                                        COERCION_EXPLICIT,
                                        COERCE_EXPLICIT_CAST,
                                        a->location);
        if (coerced != NULL)
            *left = coerced;
    }
}

static bool is_pg_scalar_property_expr(Node *node, Oid *result_type)
{
    FuncExpr *func;

    if (result_type != NULL)
        *result_type = InvalidOid;

    if (node == NULL || !IsA(node, FuncExpr))
        return false;

    func = castNode(FuncExpr, node);
    if (func->funcid == get_agtype_object_field_int8_oid())
    {
        if (result_type != NULL)
            *result_type = INT8OID;
        return true;
    }
    if (func->funcid == get_agtype_object_field_float8_oid())
    {
        if (result_type != NULL)
            *result_type = FLOAT8OID;
        return true;
    }
    if (func->funcid == get_agtype_object_field_text_agtype_oid())
    {
        if (result_type != NULL)
            *result_type = TEXTOID;
        return true;
    }

    return false;
}

static bool is_scalar_comparison_operator(List *opname)
{
    char *name;

    if (list_length(opname) != 1)
        return false;

    name = strVal(linitial(opname));
    return strcmp(name, "=") == 0 ||
        strcmp(name, "<>") == 0 ||
        strcmp(name, "!=") == 0 ||
        strcmp(name, "<") == 0 ||
        strcmp(name, "<=") == 0 ||
        strcmp(name, ">") == 0 ||
        strcmp(name, ">=") == 0;
}

static Node *try_parse_current_entity_id_column(cypher_parsestate *cpstate,
                                                Node *node)
{
    ParseState *pstate = (ParseState *)cpstate;
    FuncCall *fn;
    Node *column_ref = NULL;
    char *var_name;
    int location = -1;
    transform_entity *entity;
    ParseNamespaceItem *pnsi;
    const char *id_colname;
    int levels_up;

    if (!IsA(node, FuncCall))
        return NULL;

    fn = (FuncCall *)node;
    if (!parse_id_func_arg(fn, T_ColumnRef, &column_ref))
    {
        return NULL;
    }

    if (!parse_single_column_ref(column_ref, &var_name, &location))
    {
        return NULL;
    }

    entity = find_variable(cpstate, var_name);
    if (entity == NULL ||
        !entity->declared_in_current_clause)
    {
        return NULL;
    }

    if (entity->type == ENT_VERTEX)
        id_colname = AG_VERTEX_COLNAME_ID;
    else if (entity->type == ENT_EDGE)
        id_colname = AG_EDGE_COLNAME_ID;
    else
        return NULL;

    pnsi = refnameNamespaceItem(pstate, NULL, var_name, location,
                                &levels_up);
    if (pnsi == NULL)
        return NULL;

    return scanNSItemForColumn(pstate, pnsi, levels_up, id_colname,
                               location);
}

static Node *try_coerce_expr_to_graphid(cypher_parsestate *cpstate, Node *node)
{
    ParseState *pstate = (ParseState *)cpstate;
    Node *expr;
    Oid input_type;
    Oid target_type = GRAPHIDOID;

    expr = transform_cypher_expr_recurse(cpstate, node);
    input_type = exprType(expr);
    if (!can_coerce_type(1, &input_type, &target_type, COERCION_EXPLICIT))
        return NULL;

    return coerce_type(pstate, expr, input_type, target_type, -1,
                       COERCION_EXPLICIT, COERCE_EXPLICIT_CAST,
                       exprLocation(node));
}

static Node *try_transform_entity_id_equality(cypher_parsestate *cpstate,
                                             A_Expr *a)
{
    ParseState *pstate = (ParseState *)cpstate;
    Node *id_expr;
    Node *graphid_expr;

    if (!is_equal_operator_name(a->name))
    {
        return NULL;
    }

    id_expr = try_parse_current_entity_id_column(cpstate, a->lexpr);
    if (id_expr != NULL)
    {
        graphid_expr = try_coerce_expr_to_graphid(cpstate, a->rexpr);
    }
    else
    {
        id_expr = try_parse_current_entity_id_column(cpstate, a->rexpr);
        if (id_expr == NULL)
            return NULL;

        graphid_expr = try_coerce_expr_to_graphid(cpstate, a->lexpr);
    }

    if (graphid_expr == NULL)
        return NULL;

    return (Node *)make_op(pstate, a->name, id_expr, graphid_expr,
                           pstate->p_last_srf, a->location);
}

static Node *try_parse_edge_endpoint_id_column(
    cypher_parsestate *cpstate, Node *node)
{
    ParseState *pstate = (ParseState *)cpstate;
    FuncCall *fn;
    Node *edge_arg = NULL;
    char *func_name;
    char *var_name;
    int location = -1;
    const char *col_name;
    transform_entity *entity;
    ParseNamespaceItem *pnsi;
    int levels_up;

    if (!IsA(node, FuncCall))
        return NULL;

    fn = (FuncCall *)node;
    if (!parse_edge_endpoint_id_func_arg(fn, T_ColumnRef, &func_name,
                                         &edge_arg))
        return NULL;

    col_name = get_edge_endpoint_id_col_name(func_name);
    if (col_name == NULL)
        return NULL;

    if (!parse_single_column_ref(edge_arg, &var_name, &location))
    {
        return NULL;
    }

    entity = find_variable(cpstate, var_name);
    if (entity == NULL || entity->type != ENT_EDGE)
    {
        return NULL;
    }

    if (entity->declared_in_current_clause)
    {
        pnsi = refnameNamespaceItem(pstate, NULL, var_name, location,
                                    &levels_up);
        if (pnsi == NULL)
            return NULL;

        return scanNSItemForColumn(pstate, pnsi, levels_up, col_name,
                                   location);
    }

    if (entity->has_raw_targets)
        return make_raw_attr_var(pstate, var_name, col_name, location);

    return NULL;
}

static Node *try_transform_edge_endpoint_id_equality(cypher_parsestate *cpstate,
                                                    A_Expr *a)
{
    ParseState *pstate = (ParseState *)cpstate;
    Node *id_expr;
    Node *graphid_expr;

    if (!is_equal_operator_name(a->name))
    {
        return NULL;
    }

    id_expr = try_parse_edge_endpoint_id_column(cpstate, a->lexpr);
    if (id_expr != NULL)
    {
        graphid_expr = try_coerce_expr_to_graphid(cpstate, a->rexpr);
    }
    else
    {
        id_expr = try_parse_edge_endpoint_id_column(cpstate, a->rexpr);
        if (id_expr == NULL)
            return NULL;

        graphid_expr = try_coerce_expr_to_graphid(cpstate, a->lexpr);
    }

    if (graphid_expr == NULL)
        return NULL;

    return (Node *)make_op(pstate, a->name, id_expr, graphid_expr,
                           pstate->p_last_srf, a->location);
}

static NullTest *make_raw_null_test(Node *arg, NullTest *source)
{
    NullTest *raw_null_test;

    raw_null_test = makeNode(NullTest);
    raw_null_test->arg = (Expr *)arg;
    raw_null_test->nulltesttype = source->nulltesttype;
    raw_null_test->argisrow = false;
    raw_null_test->location = source->location;

    return raw_null_test;
}

static Node *try_transform_entity_null_test(cypher_parsestate *cpstate,
                                            NullTest *n)
{
    ParseState *pstate = (ParseState *)cpstate;
    char *var_name;
    int location = -1;
    const char *id_col_name;
    transform_entity *entity;
    ParseNamespaceItem *pnsi;
    Node *id_expr = NULL;
    int levels_up;

    if (!parse_single_column_ref((Node *)n->arg, &var_name, &location))
        return NULL;

    entity = find_variable(cpstate, var_name);
    if (entity == NULL ||
        (entity->type != ENT_VERTEX && entity->type != ENT_EDGE))
    {
        return NULL;
    }

    id_col_name = entity->type == ENT_EDGE ? AG_EDGE_COLNAME_ID :
                                             AG_VERTEX_COLNAME_ID;

    if (entity->declared_in_current_clause)
    {
        pnsi = refnameNamespaceItem(pstate, NULL, var_name, location,
                                    &levels_up);
        if (pnsi == NULL)
            return NULL;

        id_expr = scanNSItemForColumn(pstate, pnsi, levels_up, id_col_name,
                                      location);
    }
    else if (entity->has_raw_targets)
    {
        id_expr = make_raw_attr_var(pstate, var_name, id_col_name, location);
    }

    if (id_expr == NULL)
        return NULL;

    return (Node *)make_raw_null_test(id_expr, n);
}

static Node *try_transform_edge_endpoint_id_null_test(
    cypher_parsestate *cpstate, NullTest *n)
{
    Node *id_expr;

    id_expr = try_parse_edge_endpoint_id_column(cpstate, (Node *)n->arg);
    if (id_expr == NULL)
        return NULL;

    return (Node *)make_raw_null_test(id_expr, n);
}

static Node *try_transform_edge_endpoint_null_test(cypher_parsestate *cpstate,
                                                   NullTest *n)
{
    FuncCall *endpoint_fn;
    Node *edge_id;
    Node *vertex_id;
    Node *vertex_props;
    char *endpoint_name;
    char *edge_name;

    if (!IsA(n->arg, FuncCall))
        return NULL;

    endpoint_fn = (FuncCall *)n->arg;
    if (!parse_edge_endpoint_columnref_arg(endpoint_fn, &endpoint_name,
                                           &edge_name))
    {
        return NULL;
    }

    if (!make_edge_endpoint_vertex_vars(
            cpstate, edge_name, endpoint_name, n->location, &edge_id,
            &vertex_id, &vertex_props))
    {
        return NULL;
    }

    return (Node *)make_raw_null_test(edge_id, n);
}

static Node *try_transform_entity_graphid_function(cypher_parsestate *cpstate,
                                                  FuncCall *fn)
{
    ParseState *pstate = (ParseState *)cpstate;
    char *func_name;
    char *var_name;
    Node *entity_arg = NULL;
    int location = -1;
    const char *col_name = NULL;
    transform_entity *entity;
    ParseNamespaceItem *pnsi;
    Node *graphid_expr = NULL;
    int levels_up;

    if (!parse_graphid_func_arg(fn, T_ColumnRef, &func_name, &entity_arg))
    {
        return NULL;
    }

    if (is_edge_endpoint_id_name(func_name))
    {
        graphid_expr = try_parse_edge_endpoint_id_column(cpstate, (Node *)fn);
        if (graphid_expr == NULL)
            return NULL;

        return coerce_type(pstate, graphid_expr, GRAPHIDOID, AGTYPEOID, -1,
                           COERCION_EXPLICIT, COERCE_EXPLICIT_CAST,
                           fn->location);
    }

    if (!parse_single_column_ref(entity_arg, &var_name, &location))
    {
        return NULL;
    }

    entity = find_variable(cpstate, var_name);
    if (entity == NULL)
        return NULL;

    if (is_id_name(func_name))
    {
        if (entity->type == ENT_VERTEX)
            col_name = AG_VERTEX_COLNAME_ID;
        else if (entity->type == ENT_EDGE)
            col_name = AG_EDGE_COLNAME_ID;
        else
            return NULL;
    }
    else
    {
        return NULL;
    }

    if (entity->declared_in_current_clause)
    {
        pnsi = refnameNamespaceItem(pstate, NULL, var_name, location,
                                    &levels_up);
        if (pnsi == NULL)
            return NULL;

        graphid_expr = scanNSItemForColumn(pstate, pnsi, levels_up,
                                           col_name, location);
    }
    else if (entity->has_raw_targets)
    {
        graphid_expr = make_raw_attr_var(pstate, var_name, col_name,
                                         location);
    }

    if (graphid_expr == NULL)
        return NULL;

    return coerce_type(pstate, graphid_expr, GRAPHIDOID, AGTYPEOID, -1,
                       COERCION_EXPLICIT, COERCE_EXPLICIT_CAST,
                       fn->location);
}

static FuncExpr *make_label_name_expr(cypher_parsestate *cpstate, Node *id,
                                      int location)
{
    Const *graph_oid_const;
    FuncExpr *label_name_expr;

    graph_oid_const = makeConst(OIDOID, -1, InvalidOid, sizeof(Oid),
                                ObjectIdGetDatum(cpstate->graph_oid), false,
                                true);
    label_name_expr = makeFuncExpr(get_label_name_oid(), CSTRINGOID,
                                   list_make2(graph_oid_const, id),
                                   InvalidOid, InvalidOid,
                                   COERCE_EXPLICIT_CALL);
    label_name_expr->location = location;

    return label_name_expr;
}

static FuncExpr *make_build_edge_expr(cypher_parsestate *cpstate, Node *id,
                                      Node *start_id, Node *end_id,
                                      Node *props, int location)
{
    Const *graph_oid_const;
    FuncExpr *edge_expr;

    graph_oid_const = makeConst(OIDOID, -1, InvalidOid, sizeof(Oid),
                                ObjectIdGetDatum(cpstate->graph_oid), false,
                                true);
    edge_expr = makeFuncExpr(get_build_edge_label_oid(), AGTYPEOID,
                             list_make5(graph_oid_const, id, start_id, end_id,
                                        props),
                             InvalidOid, InvalidOid,
                             COERCE_EXPLICIT_CALL);
    edge_expr->location = location;

    return edge_expr;
}

static FuncExpr *make_build_vertex_expr(cypher_parsestate *cpstate, Node *id,
                                        Node *props, int location)
{
    Const *graph_oid_const;
    FuncExpr *vertex_expr;

    graph_oid_const = makeConst(OIDOID, -1, InvalidOid, sizeof(Oid),
                                ObjectIdGetDatum(cpstate->graph_oid), false,
                                true);
    vertex_expr = makeFuncExpr(get_build_vertex_label_oid(), AGTYPEOID,
                               list_make3(graph_oid_const, id, props),
                               InvalidOid, InvalidOid,
                               COERCE_EXPLICIT_CALL);
    vertex_expr->location = location;

    return vertex_expr;
}

static Node *make_endpoint_vertex_agtype(cypher_parsestate *cpstate,
                                         Node *edge_id, Node *vertex_id,
                                         Node *vertex_props, int location)
{
    FuncExpr *vertex_expr;

    vertex_expr = make_build_vertex_expr(cpstate, vertex_id, vertex_props,
                                         location);

    return make_agtype_case_when_not_null(edge_id, (Expr *)vertex_expr,
                                          location);
}

static Node *try_build_edge_from_raw_attrs(cypher_parsestate *cpstate,
                                           const char *var_name,
                                           int location)
{
    ParseState *pstate = (ParseState *)cpstate;
    Node *id;
    Node *start_id;
    Node *end_id;
    Node *props;
    FuncExpr *edge_expr;

    if (!make_raw_edge_vars(pstate, var_name, location, &id, &start_id,
                            &end_id, &props))
        return NULL;

    edge_expr = make_build_edge_expr(cpstate, id, start_id, end_id, props,
                                     location);

    return make_agtype_case_when_not_null(id, (Expr *)edge_expr, location);
}

static FuncExpr *try_make_build_vertex_from_raw_attrs(
    cypher_parsestate *cpstate, const char *var_name, int location, Node **id)
{
    ParseState *pstate = (ParseState *)cpstate;
    Node *vertex_id;
    Node *props;

    if (!make_raw_vertex_id_props_vars(pstate, var_name, location,
                                       &vertex_id, &props))
        return NULL;

    if (id != NULL)
        *id = vertex_id;

    return make_build_vertex_expr(cpstate, vertex_id, props, location);
}

static Node *try_build_vertex_from_raw_attrs(cypher_parsestate *cpstate,
                                             const char *var_name,
                                             int location)
{
    Node *id = NULL;
    FuncExpr *vertex_expr;

    vertex_expr = try_make_build_vertex_from_raw_attrs(cpstate, var_name,
                                                       location, &id);
    if (vertex_expr == NULL)
        return NULL;

    return make_agtype_case_when_not_null(id, (Expr *)vertex_expr, location);
}

static inline bool is_output_expr_kind(ParseExprKind kind)
{
    return kind == EXPR_KIND_GROUP_BY ||
           kind == EXPR_KIND_ORDER_BY ||
           kind == EXPR_KIND_DISTINCT_ON ||
           kind == EXPR_KIND_HAVING;
}

static bool get_fixed_path_parts(cypher_parsestate *cpstate,
                                 ColumnRef *path_ref,
                                 cypher_node **start_node,
                                 cypher_relationship **single_rel,
                                 cypher_node **end_node,
                                 bool require_node_names)
{
    transform_entity *entity;
    cypher_path *path;
    char *path_name = NULL;

    if (!parse_single_column_ref((Node *)path_ref, &path_name, NULL))
    {
        return false;
    }

    entity = find_variable(cpstate, path_name);
    if (entity == NULL || entity->type != ENT_PATH)
        return false;

    path = entity->entity.path;
    if (path == NULL || list_length(path->path) != 3)
        return false;

    *start_node = (cypher_node *)linitial(path->path);
    *single_rel = (cypher_relationship *)lsecond(path->path);
    *end_node = (cypher_node *)lthird(path->path);
    if (!is_ag_node(*start_node, cypher_node) ||
        !is_ag_node(*single_rel, cypher_relationship) ||
        !is_ag_node(*end_node, cypher_node) ||
        (*single_rel)->varlen != NULL ||
        (*single_rel)->name == NULL)
    {
        return false;
    }

    if (require_node_names &&
        ((*start_node)->name == NULL || (*end_node)->name == NULL))
    {
        return false;
    }

    return true;
}

static bool get_fixed_path_metadata(cypher_parsestate *cpstate,
                                    ColumnRef *path_ref,
                                    int64 *edge_count,
                                    char **first_edge_name)
{
    transform_entity *entity;
    cypher_path *path;
    char *path_name = NULL;
    ListCell *lc;

    if (!parse_single_column_ref((Node *)path_ref, &path_name, NULL))
        return false;

    entity = find_variable(cpstate, path_name);
    if (entity == NULL || entity->type != ENT_PATH)
        return false;

    path = entity->entity.path;
    if (path == NULL || path->path == NIL)
        return false;

    *edge_count = 0;
    *first_edge_name = NULL;

    foreach(lc, path->path)
    {
        Node *path_node = lfirst(lc);

        if (is_ag_node(path_node, cypher_relationship))
        {
            cypher_relationship *rel = (cypher_relationship *)path_node;

            if (rel->varlen != NULL || rel->name == NULL)
                return false;

            if (*first_edge_name == NULL)
                *first_edge_name = rel->name;

            (*edge_count)++;
        }
    }

    return *edge_count > 0 && *first_edge_name != NULL;
}

static bool parse_fixed_path_list_function(cypher_parsestate *cpstate,
                                           FuncCall *list_fn,
                                           char **list_name,
                                           cypher_node **start_node,
                                           cypher_relationship **single_rel,
                                           cypher_node **end_node,
                                           bool require_node_names)
{
    Node *path_arg = NULL;

    if (!parse_path_list_func_arg(list_fn, T_ColumnRef, list_name,
                                  &path_arg))
    {
        return false;
    }

    return get_fixed_path_parts(cpstate, (ColumnRef *)path_arg, start_node,
                                single_rel,
                                end_node, require_node_names);
}

static bool parse_fixed_path_list_node(cypher_parsestate *cpstate,
                                       Node *node, char **list_name,
                                       cypher_node **start_node,
                                       cypher_relationship **single_rel,
                                       cypher_node **end_node,
                                       bool require_node_names)
{
    if (!IsA(node, FuncCall))
    {
        return false;
    }

    return parse_fixed_path_list_function(cpstate, (FuncCall *)node,
                                          list_name, start_node, single_rel,
                                          end_node, require_node_names);
}

static bool parse_fixed_path_cardinality_list(
    cypher_parsestate *cpstate, FuncCall *inner_fn, char **list_name,
    char **transform_name, cypher_node **start_node,
    cypher_relationship **single_rel, cypher_node **end_node)
{
    FuncCall *list_fn = NULL;
    char *inner_name = NULL;
    Node *inner_arg = NULL;

    *transform_name = NULL;

    if (!parse_path_list_or_tail_reverse_func_arg(inner_fn, &inner_name,
                                                  &inner_arg))
    {
        return false;
    }

    if (is_path_list_name(inner_name))
    {
        list_fn = inner_fn;
    }
    else if (is_tail_reverse_name(inner_name) &&
             IsA(inner_arg, FuncCall))
    {
        list_fn = (FuncCall *)inner_arg;
        *transform_name = inner_name;
    }
    else
    {
        return false;
    }

    return parse_fixed_path_list_function(cpstate, list_fn, list_name,
                                          start_node, single_rel, end_node,
                                          false);
}

static bool parse_fixed_path_cardinality_metadata(
    cypher_parsestate *cpstate, FuncCall *inner_fn, char **list_name,
    char **transform_name, int64 *edge_count, char **first_edge_name)
{
    FuncCall *list_fn = NULL;
    char *inner_name = NULL;
    Node *inner_arg = NULL;
    Node *path_arg = NULL;

    *transform_name = NULL;

    if (!parse_path_list_or_tail_reverse_func_arg(inner_fn, &inner_name,
                                                  &inner_arg))
    {
        return false;
    }

    if (is_path_list_name(inner_name))
    {
        list_fn = inner_fn;
    }
    else if (is_tail_reverse_name(inner_name) &&
             IsA(inner_arg, FuncCall))
    {
        list_fn = (FuncCall *)inner_arg;
        *transform_name = inner_name;
    }
    else
    {
        return false;
    }

    if (!parse_path_list_func_arg(list_fn, T_ColumnRef, list_name, &path_arg))
        return false;

    return get_fixed_path_metadata(cpstate, (ColumnRef *)path_arg, edge_count,
                                   first_edge_name);
}

static Node *try_make_fixed_path_entity_list(cypher_parsestate *cpstate,
                                             FuncCall *fn, bool nodes_list)
{
    ParseState *pstate = (ParseState *)cpstate;
    Node *path_arg = NULL;
    transform_entity *entity;
    cypher_path *path;
    char *path_name = NULL;
    char *list_name = NULL;
    char *first_edge_name = NULL;
    List *elems = NIL;
    ListCell *lc;
    FuncExpr *list_expr;
    Node *edge_id;

    if (!parse_path_list_func_arg(fn, T_ColumnRef, &list_name, &path_arg))
        return NULL;

    if ((nodes_list && !is_nodes_list_name(list_name)) ||
        (!nodes_list && !is_relationships_list_name(list_name)))
    {
        return NULL;
    }

    if (!parse_single_column_ref(path_arg, &path_name, NULL))
        return NULL;

    entity = find_variable(cpstate, path_name);
    if (entity == NULL || entity->type != ENT_PATH)
        return NULL;

    path = entity->entity.path;
    if (path == NULL || path->path == NIL)
        return NULL;

    foreach(lc, path->path)
    {
        Node *path_node = lfirst(lc);

        if (nodes_list && is_ag_node(path_node, cypher_node))
        {
            cypher_node *node = (cypher_node *)path_node;
            Node *vertex;

            if (node->name == NULL)
                return NULL;

            vertex = try_build_vertex_from_raw_attrs(cpstate, node->name,
                                                     fn->location);
            if (vertex == NULL)
                return NULL;

            elems = lappend(elems, vertex);
        }
        else if (!nodes_list && is_ag_node(path_node, cypher_relationship))
        {
            cypher_relationship *rel = (cypher_relationship *)path_node;
            Node *edge;

            if (rel->varlen != NULL || rel->name == NULL)
                return NULL;

            if (first_edge_name == NULL)
                first_edge_name = rel->name;

            edge = try_build_edge_from_raw_attrs(cpstate, rel->name,
                                                 fn->location);
            if (edge == NULL)
                return NULL;

            elems = lappend(elems, edge);
        }
        else if (is_ag_node(path_node, cypher_relationship))
        {
            cypher_relationship *rel = (cypher_relationship *)path_node;

            if (rel->varlen != NULL || rel->name == NULL)
                return NULL;

            if (first_edge_name == NULL)
                first_edge_name = rel->name;
        }
    }

    if (elems == NIL || first_edge_name == NULL)
        return NULL;

    edge_id = make_raw_attr_var(pstate, first_edge_name, AG_EDGE_COLNAME_ID,
                                fn->location);
    if (edge_id == NULL)
        return NULL;

    list_expr = makeFuncExpr(get_agtype_build_list_oid(), AGTYPEOID, elems,
                             InvalidOid, InvalidOid, COERCE_EXPLICIT_CALL);
    list_expr->location = fn->location;

    return make_agtype_case_when_not_null(edge_id, (Expr *)list_expr,
                                          fn->location);
}

static bool parse_path_list_slice_arg(A_Indirection *a_ind,
                                      FuncCall **list_fn,
                                      A_Indices **indices,
                                      char **list_name)
{
    if (!IsA(a_ind->arg, FuncCall) ||
        !parse_single_indirection_slice(a_ind, indices))
    {
        return false;
    }

    *list_fn = (FuncCall *)a_ind->arg;
    return parse_path_list_func_name(*list_fn, list_name);
}

static bool parse_fixed_relationship_zero_index(
    cypher_parsestate *cpstate, A_Indirection *rel_ind,
    bool require_node_names, char **list_name, cypher_node **start_node,
    cypher_relationship **single_rel, cypher_node **end_node)
{
    FuncCall *relationships_fn;

    if (!IsA(rel_ind->arg, FuncCall) ||
        !parse_single_indirection_zero_index(rel_ind))
    {
        return false;
    }

    relationships_fn = (FuncCall *)rel_ind->arg;
    return parse_fixed_path_list_function(cpstate, relationships_fn, list_name,
                                          start_node, single_rel, end_node,
                                          require_node_names) &&
        is_relationships_list_name(*list_name);
}

static bool parse_fixed_endpoint_zero_index(
    cypher_parsestate *cpstate, FuncCall *endpoint_fn,
    bool require_node_names, char **endpoint_name, char **list_name,
    cypher_node **start_node, cypher_relationship **single_rel,
    cypher_node **end_node)
{
    A_Indirection *rel_ind = NULL;
    Node *rel_ind_arg = NULL;

    if (!parse_endpoint_func_arg(endpoint_fn, T_A_Indirection,
                                 endpoint_name, &rel_ind_arg))
    {
        return false;
    }

    rel_ind = (A_Indirection *)rel_ind_arg;
    return parse_fixed_relationship_zero_index(
        cpstate, rel_ind, require_node_names, list_name, start_node,
        single_rel, end_node);
}

static bool parse_fixed_path_indexed_list(
    cypher_parsestate *cpstate, A_Indirection *a_ind,
    bool require_node_names, FuncCall **list_fn, char **list_name,
    cypher_node **start_node, cypher_relationship **single_rel,
    cypher_node **end_node, int64 *index)
{
    if (!IsA(a_ind->arg, FuncCall) ||
        !parse_single_indirection_nonnegative_index(a_ind, NULL, index))
    {
        return false;
    }

    *list_fn = (FuncCall *)a_ind->arg;
    return parse_fixed_path_list_function(cpstate, *list_fn, list_name,
                                          start_node, single_rel, end_node,
                                          require_node_names);
}

static bool parse_fixed_path_leading_indexed_list(
    cypher_parsestate *cpstate, A_Indirection *a_ind,
    bool require_node_names, FuncCall **list_fn, char **list_name,
    cypher_node **start_node, cypher_relationship **single_rel,
    cypher_node **end_node, int64 *index)
{
    if (!IsA(a_ind->arg, FuncCall) ||
        !parse_leading_indirection_nonnegative_index(a_ind, index))
    {
        return false;
    }

    *list_fn = (FuncCall *)a_ind->arg;
    return parse_fixed_path_list_function(cpstate, *list_fn, list_name,
                                          start_node, single_rel, end_node,
                                          require_node_names);
}

static bool parse_fixed_path_slice_list(
    cypher_parsestate *cpstate, A_Indirection *a_ind,
    bool require_node_names, FuncCall **list_fn, A_Indices **indices,
    char **list_name, cypher_node **start_node,
    cypher_relationship **single_rel, cypher_node **end_node)
{
    if (!parse_path_list_slice_arg(a_ind, list_fn, indices, list_name))
    {
        return false;
    }

    return parse_fixed_path_list_function(cpstate, *list_fn, list_name,
                                          start_node, single_rel, end_node,
                                          require_node_names);
}

static bool parse_fixed_path_head_last_slice_index(Node *node,
                                                   bool require_transform,
                                                   FuncCall **list_fn,
                                                   char **parsed_list_name,
                                                   int64 *index)
{
    A_Indices *slice_indices = NULL;
    char *head_last_name = NULL;
    char *transform_name = NULL;
    char *list_name = NULL;
    int64 lower = 0;
    int64 upper;
    int64 list_len;

    if (!parse_head_last_slice_context(node, list_fn, &list_name,
                                       &head_last_name, &transform_name,
                                       &slice_indices) ||
        require_transform != (transform_name != NULL))
    {
        return false;
    }

    if (!get_fixed_path_list_len(list_name, &list_len))
        return false;

    if (!parse_nonempty_fixed_slice_bounds(slice_indices, list_len,
                                           transform_name, &lower, &upper))
        return false;

    *index = get_slice_boundary_index(head_last_name,
                                      transform_name != NULL &&
                                      is_reverse_name(transform_name),
                                      lower, upper);
    if (parsed_list_name != NULL)
    {
        *parsed_list_name = list_name;
    }

    return true;
}

static bool make_fixed_path_indexed_props_result_vars(
    cypher_parsestate *cpstate, A_Indirection *a_ind, bool leading_index,
    int location, Node **edge_id, Node **target_props)
{
    ParseState *pstate = (ParseState *)cpstate;
    FuncCall *list_fn = NULL;
    cypher_node *start_node = NULL;
    cypher_relationship *single_rel = NULL;
    cypher_node *end_node = NULL;
    char *list_name = NULL;
    int64 index;

    if (leading_index)
    {
        if (!parse_fixed_path_leading_indexed_list(
                cpstate, a_ind, true, &list_fn, &list_name, &start_node,
                &single_rel, &end_node, &index))
        {
            return false;
        }
    }
    else if (!parse_fixed_path_indexed_list(cpstate, a_ind, true, &list_fn,
                                            &list_name, &start_node,
                                            &single_rel, &end_node, &index))
    {
        return false;
    }

    return make_fixed_path_indexed_props_vars(
        pstate, list_name, index, start_node, single_rel, end_node, location,
        edge_id, target_props);
}

static bool make_fixed_path_indexed_id_result_vars(
    cypher_parsestate *cpstate, A_Indirection *a_ind,
    const char *edge_attr_name, int location, Node **edge_id,
    Node **target_id, char **parsed_list_name)
{
    ParseState *pstate = (ParseState *)cpstate;
    FuncCall *list_fn = NULL;
    cypher_node *start_node = NULL;
    cypher_relationship *single_rel = NULL;
    cypher_node *end_node = NULL;
    char *list_name = NULL;
    int64 index;

    if (!parse_fixed_path_indexed_list(cpstate, a_ind, true, &list_fn,
                                       &list_name, &start_node, &single_rel,
                                       &end_node, &index))
    {
        return false;
    }

    if (parsed_list_name != NULL)
    {
        *parsed_list_name = list_name;
    }

    return make_fixed_path_indexed_id_vars(
        pstate, list_name, index, start_node, single_rel, end_node,
        edge_attr_name, location, edge_id, target_id);
}

static Node *try_transform_current_entity_properties(cypher_parsestate *cpstate,
                                                    FuncCall *fn)
{
    ParseState *pstate = (ParseState *)cpstate;
    char *var_name;
    Node *properties_arg = NULL;
    int location = -1;
    transform_entity *entity;
    ParseNamespaceItem *pnsi;
    int levels_up;

    if (is_output_expr_kind(pstate->p_expr_kind))
        return NULL;

    if (!parse_func_arg(fn, "properties", T_ColumnRef, &properties_arg))
    {
        return NULL;
    }

    if (!parse_single_column_ref(properties_arg, &var_name, &location))
    {
        return NULL;
    }

    entity = find_variable(cpstate, var_name);
    if (entity == NULL ||
        (entity->type != ENT_VERTEX && entity->type != ENT_EDGE))
    {
        return NULL;
    }

    if (!entity->declared_in_current_clause)
    {
        Node *raw_properties;

        raw_properties = make_raw_attr_var(pstate, var_name,
                                           AG_VERTEX_COLNAME_PROPERTIES,
                                           fn->location);
        if (entity->has_raw_targets &&
            retarget_vle_terminal_properties_output(cpstate, raw_properties))
        {
            return raw_properties;
        }

        return raw_properties;
    }

    pnsi = refnameNamespaceItem(pstate, NULL, var_name, location,
                                &levels_up);
    if (pnsi == NULL)
        return NULL;

    return scanNSItemForColumn(pstate, pnsi, levels_up,
                               AG_VERTEX_COLNAME_PROPERTIES, location);
}

static Node *make_age_keys_expr(Node *properties_expr, int location)
{
    FuncExpr *keys_expr;

    if (IsA(properties_expr, FuncExpr))
    {
        ((FuncExpr *)properties_expr)->location = location;
    }

    keys_expr = makeFuncExpr(get_age_keys_oid(), AGTYPEOID,
                             list_make1(properties_expr), InvalidOid,
                             InvalidOid, COERCE_EXPLICIT_CALL);
    keys_expr->location = location;

    return (Node *)keys_expr;
}

static FuncExpr *make_agtype_access_expr(List *access_args, int location,
                                         bool expand_variadic)
{
    FuncExpr *access_expr;
    Node *access_arg;
    ListCell *lc;
    bool first_arg = true;

    if (expand_variadic && list_length(access_args) >= 2)
    {
        access_arg = linitial(access_args);

        foreach(lc, access_args)
        {
            if (first_arg)
            {
                first_arg = false;
                continue;
            }

            access_expr = makeFuncExpr(get_agtype_object_field_agtype_oid(),
                                       AGTYPEOID,
                                       list_make2(access_arg, lfirst(lc)),
                                       InvalidOid, InvalidOid,
                                       COERCE_EXPLICIT_CALL);
            access_expr->location = location;
            access_arg = (Node *)access_expr;
        }

        return access_expr;
    }

    access_expr = makeFuncExpr(get_agtype_access_operator_oid(), AGTYPEOID,
                               list_make1(make_agtype_array_expr(access_args)),
                               InvalidOid, InvalidOid,
                               COERCE_EXPLICIT_CALL);
    access_expr->funcvariadic = true;
    access_expr->location = location;

    return access_expr;
}

static Node *make_case_when_not_null(Node *not_null_expr, Expr *result_expr,
                                     Oid result_type, int location)
{
    NullTest *not_null_test;
    CaseWhen *not_null_case;
    CaseExpr *case_expr;

    not_null_test = makeNode(NullTest);
    not_null_test->arg = (Expr *)copyObject(not_null_expr);
    not_null_test->nulltesttype = IS_NOT_NULL;
    not_null_test->argisrow = false;
    not_null_test->location = location;

    not_null_case = makeNode(CaseWhen);
    not_null_case->expr = (Expr *)not_null_test;
    not_null_case->result = result_expr;
    not_null_case->location = location;

    case_expr = makeNode(CaseExpr);
    case_expr->casetype = result_type;
    case_expr->casecollid = InvalidOid;
    case_expr->arg = NULL;
    case_expr->args = list_make1(not_null_case);
    case_expr->defresult = (Expr *)makeNullConst(result_type, -1, InvalidOid);
    case_expr->location = location;

    return (Node *)case_expr;
}

static Node *make_agtype_case_when_not_null(Node *not_null_expr,
                                            Expr *result_expr,
                                            int location)
{
    return make_case_when_not_null(not_null_expr, result_expr, AGTYPEOID,
                                   location);
}

static Node *make_fixed_path_slice_list_expr(cypher_parsestate *cpstate,
                                             cypher_node *start_node,
                                             cypher_relationship *single_rel,
                                             cypher_node *end_node,
                                             const char *list_name,
                                             int64 lower, int64 upper,
                                             bool reversed, int location)
{
    List *elems = NIL;
    FuncExpr *list_expr;

    if (upper == lower)
    {
        FuncExpr *empty_list_expr;

        empty_list_expr = makeFuncExpr(get_agtype_build_empty_list_oid(),
                                       AGTYPEOID, NIL, InvalidOid, InvalidOid,
                                       COERCE_EXPLICIT_CALL);
        empty_list_expr->location = location;
        return (Node *)empty_list_expr;
    }

    if (is_relationships_list_name(list_name))
    {
        Node *edge;

        edge = try_build_edge_from_raw_attrs(cpstate, single_rel->name,
                                             location);
        if (edge == NULL)
            return NULL;

        elems = list_make1(edge);
    }
    else
    {
        Node *start_vertex = NULL;
        Node *end_vertex = NULL;

        if (lower <= 0 && upper > 0)
        {
            start_vertex = try_build_vertex_from_raw_attrs(cpstate,
                                                           start_node->name,
                                                           location);
            if (start_vertex == NULL)
                return NULL;
        }

        if (lower <= 1 && upper > 1)
        {
            end_vertex = try_build_vertex_from_raw_attrs(cpstate,
                                                         end_node->name,
                                                         location);
            if (end_vertex == NULL)
                return NULL;
        }

        if (reversed)
        {
            if (end_vertex != NULL)
                elems = lappend(elems, end_vertex);
            if (start_vertex != NULL)
                elems = lappend(elems, start_vertex);
        }
        else
        {
            if (start_vertex != NULL)
                elems = lappend(elems, start_vertex);
            if (end_vertex != NULL)
                elems = lappend(elems, end_vertex);
        }
    }

    if (elems == NIL)
        return NULL;

    list_expr = makeFuncExpr(get_agtype_build_list_oid(), AGTYPEOID, elems,
                             InvalidOid, InvalidOid, COERCE_EXPLICIT_CALL);
    list_expr->location = location;

    return (Node *)list_expr;
}

static Node *make_fixed_path_slice_list_result(cypher_parsestate *cpstate,
                                               cypher_node *start_node,
                                               cypher_relationship *single_rel,
                                               cypher_node *end_node,
                                               const char *list_name,
                                               int64 lower, int64 upper,
                                               bool reversed, int location)
{
    ParseState *pstate = (ParseState *)cpstate;
    Node *edge_id;
    Node *list_expr;

    edge_id = make_fixed_path_edge_id_var(pstate, single_rel, location);
    if (edge_id == NULL)
        return NULL;

    list_expr = make_fixed_path_slice_list_expr(cpstate, start_node,
                                                single_rel, end_node,
                                                list_name, lower, upper,
                                                reversed, location);
    if (list_expr == NULL)
        return NULL;

    return make_agtype_case_when_not_null(edge_id, (Expr *)list_expr,
                                          location);
}

static Node *make_vle_binary_expr(Oid func_oid, Oid result_type,
                                  void *vle_expr, void *value_expr)
{
    return (Node *)makeFuncExpr(func_oid, result_type,
                                list_make2(vle_expr, value_expr),
                                InvalidOid, InvalidOid,
                                COERCE_EXPLICIT_CALL);
}

static Node *make_vle_binary_agtype_expr(Oid func_oid, void *vle_expr,
                                         void *value_expr)
{
    return make_vle_binary_expr(func_oid, AGTYPEOID, vle_expr, value_expr);
}

static Node *make_vle_indexed_list_expr(Expr *vle_expr, const char *list_name,
                                        int64 index, int location,
                                        Oid node_func_oid, Oid edge_func_oid)
{
    Const *index_expr;
    Oid func_oid;

    index_expr = make_agtype_integer_const(index, location);
    if (is_nodes_list_name(list_name))
    {
        func_oid = node_func_oid;
    }
    else
    {
        func_oid = edge_func_oid;
    }

    return make_vle_binary_agtype_expr(func_oid, (Node *)vle_expr,
                                       (Node *)index_expr);
}

static Node *make_vle_indexed_id_expr(Expr *vle_expr, const char *list_name,
                                      int64 index, int location)
{
    Oid func_oid;

    func_oid = is_nodes_list_name(list_name) ? get_age_vle_node_id_at_oid() :
        get_age_vle_edge_id_at_oid();

    return make_vle_indexed_list_expr(vle_expr, list_name, index, location,
                                      func_oid, func_oid);
}

static Node *make_vle_indexed_properties_expr(Expr *vle_expr,
                                              const char *list_name,
                                              int64 index, int location)
{
    Oid func_oid;

    func_oid = is_nodes_list_name(list_name) ?
        get_age_vle_node_properties_at_oid() :
        get_age_vle_edge_properties_at_oid();

    return make_vle_indexed_list_expr(vle_expr, list_name, index, location,
                                      func_oid, func_oid);
}

static Node *make_vle_indexed_property_expr(Expr *vle_expr,
                                            const char *list_name,
                                            Node *index_expr,
                                            Const *key_expr)
{
    Oid func_oid;

    func_oid = is_nodes_list_name(list_name) ?
        get_age_vle_node_property_at_oid() :
        get_age_vle_edge_property_at_oid();

    return (Node *)makeFuncExpr(func_oid, AGTYPEOID,
                                list_make3((Node *)vle_expr, index_expr,
                                           key_expr),
                                InvalidOid, InvalidOid,
                                COERCE_EXPLICIT_CALL);
}

static Node *make_vle_materialized_index_expr(Expr *vle_expr,
                                              const char *list_name,
                                              int64 index, int location)
{
    Oid func_oid;

    func_oid = is_nodes_list_name(list_name) ?
        get_age_materialize_vle_node_at_oid() :
        get_age_materialize_vle_edge_at_oid();

    return make_vle_indexed_list_expr(vle_expr, list_name, index, location,
                                      func_oid, func_oid);
}

static Node *make_vle_edge_endpoint_index_expr(Expr *vle_expr,
                                               Node *index_expr,
                                               bool start_endpoint,
                                               Oid start_func_oid,
                                               Oid end_func_oid)
{
    Oid func_oid;

    func_oid = start_endpoint ? start_func_oid : end_func_oid;
    return make_vle_binary_agtype_expr(func_oid, (Node *)vle_expr,
                                       index_expr);
}

static Node *make_vle_binary_mode_typed_expr(Oid func_oid, Oid result_type,
                                             Node *vle_expr, int64 mode,
                                             int location)
{
    Const *mode_expr;

    mode_expr = make_agtype_integer_const(mode, location);
    return make_vle_binary_expr(func_oid, result_type, vle_expr,
                                (Node *)mode_expr);
}

static Node *make_vle_binary_mode_expr(Oid func_oid, Node *vle_expr,
                                       int64 mode, int location)
{
    return make_vle_binary_mode_typed_expr(func_oid, AGTYPEOID, vle_expr,
                                           mode, location);
}

static Node *make_vle_unary_agtype_expr(Oid func_oid, void *vle_expr)
{
    return (Node *)makeFuncExpr(func_oid, AGTYPEOID, list_make1(vle_expr),
                                InvalidOid, InvalidOid,
                                COERCE_EXPLICIT_CALL);
}

static Node *make_vle_ternary_agtype_expr(Oid func_oid, void *vle_expr,
                                          void *first_expr, void *second_expr)
{
    return (Node *)makeFuncExpr(func_oid, AGTYPEOID,
                                list_make3(vle_expr, first_expr, second_expr),
                                InvalidOid, InvalidOid,
                                COERCE_EXPLICIT_CALL);
}

static Node *make_vle_ternary_mode_expr(Oid func_oid, Node *vle_expr,
                                        Node *first_expr, int64 mode,
                                        int location)
{
    Const *mode_expr;

    mode_expr = make_agtype_integer_const(mode, location);
    return make_vle_ternary_agtype_expr(func_oid, vle_expr, first_expr,
                                        (Node *)mode_expr);
}

static Node *make_vle_edge_endpoint_field_mode_expr(Expr *vle_expr,
                                                    Node *index_expr,
                                                    int64 mode, int location)
{
    return make_vle_ternary_mode_expr(get_age_vle_edge_endpoint_field_at_oid(),
                                      (Node *)vle_expr, index_expr, mode,
                                      location);
}

static Node *make_vle_four_arg_expr(Oid func_oid, Oid result_type,
                                    Node *vle_expr, Node *lower_expr,
                                    Node *upper_expr, Node *mode_expr)
{
    return (Node *)makeFuncExpr(func_oid, result_type,
                                list_make4(vle_expr, lower_expr, upper_expr,
                                           mode_expr),
                                InvalidOid, InvalidOid,
                                COERCE_EXPLICIT_CALL);
}

static Node *make_vle_slice_mode_expr(Oid func_oid, Oid result_type,
                                      Node *vle_expr, Node *lower_expr,
                                      Node *upper_expr, int64 mode,
                                      int location)
{
    Const *mode_expr;

    mode_expr = make_agtype_integer_const(mode, location);
    return make_vle_four_arg_expr(func_oid, result_type, vle_expr,
                                  lower_expr, upper_expr, (Node *)mode_expr);
}

static Node *make_vle_list_slice_count_expr(Node *vle_expr, Node *lower_expr,
                                            Node *upper_expr, int64 mode,
                                            int location, bool is_empty)
{
    Oid func_oid;
    Oid result_type;

    if (is_empty)
    {
        func_oid = get_age_vle_list_slice_is_empty_oid();
        result_type = BOOLOID;
    }
    else
    {
        func_oid = get_age_vle_list_slice_count_oid();
        result_type = AGTYPEOID;
    }

    return make_vle_slice_mode_expr(func_oid, result_type, vle_expr,
                                    lower_expr, upper_expr, mode, location);
}

static Node *make_vle_double_tail_count_expr(Node *vle_expr,
                                             const char *list_name,
                                             int location, bool is_empty)
{
    Node *lower_expr = NULL;
    Node *upper_expr = NULL;
    int64 mode = is_nodes_list_name(list_name) ? 1 : 0;

    make_slice_bounds_to_null(2, location, &lower_expr, &upper_expr);
    return make_vle_list_slice_count_expr(vle_expr, lower_expr, upper_expr,
                                          mode, location, is_empty);
}

static Node *make_vle_slice_boundary_expr(Node *vle_expr, Node *lower_expr,
                                          Node *upper_expr, Node *mode_expr)
{
    return make_vle_four_arg_expr(get_age_materialize_vle_slice_boundary_oid(),
                                  AGTYPEOID, vle_expr, lower_expr, upper_expr,
                                  mode_expr);
}

static Node *make_vle_slice_boundary_mode_expr(Node *vle_expr,
                                               Node *lower_expr,
                                               Node *upper_expr,
                                               int64 mode, int location)
{
    Const *mode_expr;

    mode_expr = make_agtype_integer_const(mode, location);
    return make_vle_slice_boundary_expr(vle_expr, lower_expr, upper_expr,
                                        (Node *)mode_expr);
}

static Node *make_vle_slice_boundary_head_last_expr(Node *vle_expr,
                                                    Node *lower_expr,
                                                    Node *upper_expr,
                                                    int64 mode,
                                                    int64 mode_offset,
                                                    bool last, int location)
{
    return make_vle_slice_boundary_mode_expr(
        vle_expr, lower_expr, upper_expr, mode + mode_offset + (last ? 1 : 0),
        location);
}

static Node *make_label_agtype_expr(cypher_parsestate *cpstate,
                                    Node *id_expr, int location)
{
    ParseState *pstate = (ParseState *)cpstate;
    FuncExpr *label_name_expr;
    Node *label_text_expr;
    FuncExpr *label_agtype_expr;

    label_name_expr = make_label_name_expr(cpstate, id_expr, location);

    label_text_expr = coerce_to_target_type(pstate, (Node *)label_name_expr,
                                            CSTRINGOID, TEXTOID, -1,
                                            COERCION_EXPLICIT,
                                            COERCE_EXPLICIT_CAST,
                                            location);
    if (label_text_expr == NULL)
        return NULL;

    label_agtype_expr = makeFuncExpr(get_text_to_agtype_oid(), AGTYPEOID,
                                     list_make1(label_text_expr), InvalidOid,
                                     InvalidOid, COERCE_SQL_SYNTAX);
    label_agtype_expr->location = location;

    return (Node *)label_agtype_expr;
}

static Node *make_single_label_list_expr(Node *label_agtype_expr, int location)
{
    FuncExpr *labels_list_expr;

    labels_list_expr = makeFuncExpr(get_agtype_build_list_oid(), AGTYPEOID,
                                    list_make1(label_agtype_expr),
                                    InvalidOid, InvalidOid,
                                    COERCE_EXPLICIT_CALL);
    labels_list_expr->location = location;

    return (Node *)labels_list_expr;
}

static Node *make_label_or_labels_expr(cypher_parsestate *cpstate,
                                       Node *id_expr, bool as_list,
                                       int location)
{
    Node *label_agtype_expr;

    label_agtype_expr = make_label_agtype_expr(cpstate, id_expr, location);
    if (label_agtype_expr == NULL)
        return NULL;

    if (as_list)
        return make_single_label_list_expr(label_agtype_expr, location);

    return label_agtype_expr;
}

static Node *try_transform_entity_keys(cypher_parsestate *cpstate, FuncCall *fn)
{
    ParseState *pstate = (ParseState *)cpstate;
    Node *entity_arg = NULL;
    Node *properties_expr = NULL;
    int location = -1;

    if (!parse_keys_func_arg(fn, T_ColumnRef, &entity_arg))
    {
        return NULL;
    }

    if (!parse_single_column_ref(entity_arg, NULL, &location))
    {
        return NULL;
    }

    properties_expr = try_transform_entity_properties_expr(
        cpstate, entity_arg, location,
        pstate->p_expr_kind != EXPR_KIND_SELECT_TARGET);
    if (properties_expr == NULL)
        return NULL;

    return make_age_keys_expr(properties_expr, fn->location);
}

static Node *try_transform_current_entity_label(cypher_parsestate *cpstate,
                                               FuncCall *fn)
{
    ParseState *pstate = (ParseState *)cpstate;
    char *func_name;
    char *var_name;
    Node *entity_arg = NULL;
    int location = -1;
    transform_entity *entity;
    ParseNamespaceItem *pnsi;
    Node *id_expr;
    Node *label_agtype_expr;
    int levels_up;

    if (is_output_expr_kind(pstate->p_expr_kind))
        return NULL;

    if (!parse_label_type_func_arg(fn, T_ColumnRef, &func_name, &entity_arg))
    {
        return NULL;
    }

    if (!parse_single_column_ref(entity_arg, &var_name, &location))
    {
        return NULL;
    }

    entity = find_variable(cpstate, var_name);
    if (entity == NULL ||
        (entity->type != ENT_VERTEX && entity->type != ENT_EDGE))
    {
        return NULL;
    }

    if (is_type_name(func_name) && entity->type != ENT_EDGE)
    {
        return NULL;
    }

    if (entity->declared_in_current_clause)
    {
        pnsi = refnameNamespaceItem(pstate, NULL, var_name, location,
                                    &levels_up);
        if (pnsi == NULL)
            return NULL;

        id_expr = scanNSItemForColumn(pstate, pnsi, levels_up,
                                      entity->type == ENT_EDGE ?
                                      AG_EDGE_COLNAME_ID :
                                      AG_VERTEX_COLNAME_ID,
                                      location);
    }
    else
    {
        id_expr = make_raw_attr_var(pstate, var_name,
                                    entity->type == ENT_EDGE ?
                                    AG_EDGE_COLNAME_ID :
                                    AG_VERTEX_COLNAME_ID,
                                    location);
    }

    if (id_expr == NULL)
        return NULL;

    label_agtype_expr = make_label_agtype_expr(cpstate, id_expr, fn->location);
    if (label_agtype_expr == NULL)
        return NULL;

    return make_agtype_case_when_not_null(id_expr, (Expr *)label_agtype_expr,
                                          fn->location);
}

static Node *try_transform_current_vertex_labels(cypher_parsestate *cpstate,
                                                FuncCall *fn)
{
    ParseState *pstate = (ParseState *)cpstate;
    char *var_name;
    Node *labels_arg = NULL;
    int location = -1;
    transform_entity *entity;
    ParseNamespaceItem *pnsi;
    Node *id_expr;
    Node *label_agtype_expr;
    Node *labels_list_expr;
    int levels_up;

    if (is_output_expr_kind(pstate->p_expr_kind))
        return NULL;

    if (!parse_func_arg(fn, "labels", T_ColumnRef, &labels_arg))
    {
        return NULL;
    }

    if (!parse_single_column_ref(labels_arg, &var_name, &location))
    {
        return NULL;
    }

    entity = find_variable(cpstate, var_name);
    if (entity == NULL ||
        entity->type != ENT_VERTEX)
    {
        return NULL;
    }

    if (entity->declared_in_current_clause)
    {
        pnsi = refnameNamespaceItem(pstate, NULL, var_name, location,
                                    &levels_up);
        if (pnsi == NULL)
            return NULL;

        id_expr = scanNSItemForColumn(pstate, pnsi, levels_up,
                                      AG_VERTEX_COLNAME_ID, location);
    }
    else
    {
        if (pstate->p_expr_kind == EXPR_KIND_SELECT_TARGET)
            return NULL;

        id_expr = make_raw_attr_var(pstate, var_name, AG_VERTEX_COLNAME_ID,
                                    location);
    }

    if (id_expr == NULL)
        return NULL;

    label_agtype_expr = make_label_agtype_expr(cpstate, id_expr, fn->location);
    if (label_agtype_expr == NULL)
        return NULL;

    labels_list_expr = make_single_label_list_expr(label_agtype_expr,
                                                   fn->location);

    return make_agtype_case_when_not_null(id_expr, (Expr *)labels_list_expr,
                                          fn->location);
}

/*
 * function for transforming cypher comparision A_Expr. Since this node is a
 * wrapper to let us know when a comparison occurs in a chained comparison,
 * we convert it to a regular A_expr and transform it.
 */
static Node *transform_cypher_comparison_aexpr_OP(cypher_parsestate *cpstate,
                                                  cypher_comparison_aexpr *a)
{
    A_Expr *n = makeNode(A_Expr);
    n->kind = a->kind;
    n->name = a->name;
    n->lexpr = a->lexpr;
    n->rexpr = a->rexpr;
    n->location = a->location;

    return (Node *)transform_AEXPR_OP(cpstate, n);
}


static Node *transform_AEXPR_IN(cypher_parsestate *cpstate, A_Expr *a)
{
    ParseState *pstate = (ParseState *)cpstate;
    cypher_list *rexpr;
    Node *result = NULL;
    Node *lexpr;
    List *rexprs;
    List *rvars;
    List *rnonvars;
    bool useOr;
    bool is_not_in;
    ListCell *l;

    if (!is_ag_node(a->rexpr, cypher_list))
    {
        /*
         * We need to build a function call here if the rexpr is already
         * tranformed. It can be already tranformed cypher_list as columnref.
         */
        Oid func_in_oid;
        FuncExpr *func_in_expr;
        List *args = NIL;

        result = try_transform_vle_edge_self_membership(cpstate, a);
        if (result != NULL)
        {
            return result;
        }

        args = lappend(args, transform_cypher_expr_recurse(cpstate, a->rexpr));
        args = lappend(args, transform_cypher_expr_recurse(cpstate, a->lexpr));

        /* get the agtype_in_operator function */
        func_in_oid = get_agtype_in_operator_oid();

        func_in_expr = makeFuncExpr(func_in_oid, AGTYPEOID, args, InvalidOid,
                                    InvalidOid, COERCE_EXPLICIT_CALL);

        func_in_expr->location = exprLocation(a->lexpr);

        return (Node *)func_in_expr;
    }

    Assert(is_ag_node(a->rexpr, cypher_list));

    rexpr = (cypher_list *)a->rexpr;
    is_not_in = (strcmp(strVal(linitial(a->name)), "<>") == 0);

    /*
     * Handle empty list case: x IN [] is always false, x NOT IN [] is always true.
     * We need to check this before processing to avoid returning NULL result
     * which causes "cache lookup failed for type 0" error.
     */
    if (rexpr->elems == NIL)
    {
        Datum bool_value;
        Const *const_result;

        /* If operator is <> (NOT IN), result is true; otherwise (IN) result is false */
        if (is_not_in)
        {
            bool_value = BoolGetDatum(true);
        }
        else
        {
            bool_value = BoolGetDatum(false);
        }

        const_result = makeConst(BOOLOID, -1, InvalidOid, sizeof(bool),
                                 bool_value, false, true);

        return (Node *)const_result;
    }

    /* If the operator is <>, combine with AND not OR. */
    useOr = !is_not_in;

    lexpr = transform_cypher_expr_recurse(cpstate, a->lexpr);

    rexprs = rvars = rnonvars = NIL;

    foreach(l, (List *) rexpr->elems)
    {
        Node *rexpr = transform_cypher_expr_recurse(cpstate, lfirst(l));

        rexprs = lappend(rexprs, rexpr);
                if (contain_vars_of_level(rexpr, 0))
                {
                        rvars = lappend(rvars, rexpr);
                }
                else
                {
                        rnonvars = lappend(rnonvars, rexpr);
                }
    }


    /*
     * ScalarArrayOpExpr is only going to be useful if there's more than one
     * non-Var righthand item.
     */
    if (rnonvars != NIL && lnext(rnonvars, list_head(rnonvars)) != NULL)
    {
        List *allexprs;
        Oid scalar_type;
        List *aexprs;
        ArrayExpr *newa;

        allexprs = list_concat(list_make1(lexpr), rnonvars);

        scalar_type = AGTYPEOID;

        /* verify they are a common type */
        if (!verify_common_type(scalar_type, allexprs))
        {
            ereport(ERROR,
                    errmsg_internal("not a common type: %d", scalar_type));
        }

        /*
         * coerce all the right-hand non-Var inputs to the common type
         * and build an ArrayExpr for them.
         */
        aexprs = NIL;
        foreach(l, rnonvars)
        {
            Node *rexpr = (Node *) lfirst(l);

            rexpr = coerce_to_common_type(pstate, rexpr, AGTYPEOID, "IN");
            aexprs = lappend(aexprs, rexpr);
        }
        newa = makeNode(ArrayExpr);
        newa->array_typeid = get_array_type(AGTYPEOID);
        /* array_collid will be set by parse_collate.c */
        newa->element_typeid = AGTYPEOID;
        newa->elements = aexprs;
        newa->multidims = false;
        result = (Node *) make_scalar_array_op(pstate, a->name, useOr,
                                               lexpr, (Node *) newa, a->location);

        /* Consider only the Vars (if any) in the loop below */
        rexprs = rvars;
    }

    /* Must do it the hard way, with a boolean expression tree. */
    foreach(l, rexprs)
    {
        Node *rexpr = (Node *) lfirst(l);
        Node *cmp;

        /* Ordinary scalar operator */
        cmp = (Node *) make_op(pstate, a->name, copyObject(lexpr), rexpr,
                               pstate->p_last_srf, a->location);

        cmp = coerce_to_boolean(pstate, cmp, "IN");
        if (result == NULL)
        {
            result = cmp;
        }
        else
        {
            result = (Node *) makeBoolExpr(useOr ? OR_EXPR : AND_EXPR,
                                           list_make2(result, cmp),
                                           a->location);
        }
    }

    return result;
}

static Node *try_transform_vle_edge_self_membership(
    cypher_parsestate *cpstate, A_Expr *a)
{
    Expr *left_vle_expr = NULL;
    Expr *right_vle_expr = NULL;
    Node *index_expr = NULL;
    Oid func_oid;
    bool reversed_index = false;

    if (!is_equal_operator_name(a->name))
    {
        return NULL;
    }

    right_vle_expr = get_arbitrary_vle_relationship_list_expr(cpstate,
                                                              a->rexpr);
    if (right_vle_expr == NULL)
    {
        return NULL;
    }
    if (!parse_vle_normal_or_boundary_index(cpstate, a->lexpr,
                                            &left_vle_expr, &index_expr) &&
        !parse_vle_nested_transform_edge_equality_index(
            cpstate, a->lexpr, &left_vle_expr, &index_expr, &reversed_index))
    {
        return NULL;
    }
    if (left_vle_expr != right_vle_expr)
    {
        return NULL;
    }

    func_oid = get_age_vle_edge_index_exists_oid();

    return make_vle_binary_agtype_expr(func_oid, left_vle_expr, index_expr);
}

static bool parse_current_vle_edge_indirection_index(
    cypher_parsestate *cpstate, A_Indirection *a_ind, Expr **vle_expr,
    Node **index_expr)
{
    A_Indices *indices = NULL;

    if (!parse_single_indirection_value_index(a_ind, &indices))
    {
        return false;
    }

    *vle_expr = get_current_any_vle_edge_expr(cpstate, a_ind->arg);
    if (*vle_expr == NULL)
    {
        return false;
    }

    *index_expr = transform_cypher_expr_recurse(cpstate, indices->uidx);

    return true;
}

static Node *try_transform_vle_edge_index_equality(
    cypher_parsestate *cpstate, A_Expr *a)
{
    A_Indirection *left_ind = NULL;
    A_Indirection *right_ind = NULL;
    Expr *left_vle_expr = NULL;
    Expr *right_vle_expr = NULL;
    Node *left_index_expr = NULL;
    Node *right_index_expr = NULL;
    Oid func_oid;

    if (!is_equal_operator_name(a->name) ||
        !IsA(a->lexpr, A_Indirection) ||
        !IsA(a->rexpr, A_Indirection))
    {
        return NULL;
    }

    left_ind = (A_Indirection *)a->lexpr;
    right_ind = (A_Indirection *)a->rexpr;
    if (!parse_current_vle_edge_indirection_index(cpstate, left_ind,
                                                  &left_vle_expr,
                                                  &left_index_expr) ||
        !parse_current_vle_edge_indirection_index(cpstate, right_ind,
                                                  &right_vle_expr,
                                                  &right_index_expr) ||
        left_vle_expr != right_vle_expr)
    {
        return NULL;
    }

    func_oid = get_age_vle_edge_indices_equal_oid();

    return make_vle_ternary_agtype_expr(func_oid, left_vle_expr,
                                        left_index_expr, right_index_expr);
}

static Node *try_transform_vle_edge_boundary_equality(
    cypher_parsestate *cpstate, A_Expr *a)
{
    Node *boundary_node = NULL;
    Node *indexed_node = NULL;
    FuncCall *boundary_fn = NULL;
    A_Indirection *a_ind = NULL;
    Expr *boundary_vle_expr = NULL;
    Expr *indexed_vle_expr = NULL;
    Node *boundary_arg = NULL;
    Node *boundary_index_expr = NULL;
    Node *indexed_index_expr = NULL;
    char *boundary_list_name = NULL;
    char *indexed_list_name = NULL;
    Oid func_oid;

    if (!is_equal_operator_name(a->name))
    {
        return NULL;
    }

    if (IsA(a->lexpr, FuncCall) && IsA(a->rexpr, A_Indirection))
    {
        boundary_node = a->lexpr;
        indexed_node = a->rexpr;
    }
    else if (IsA(a->lexpr, A_Indirection) && IsA(a->rexpr, FuncCall))
    {
        boundary_node = a->rexpr;
        indexed_node = a->lexpr;
    }
    else
    {
        return NULL;
    }

    boundary_fn = (FuncCall *)boundary_node;
    if (!parse_vle_boundary_function(boundary_fn, NULL, &boundary_arg,
                                     &boundary_index_expr))
    {
        return NULL;
    }

    if (!parse_arbitrary_vle_path_or_raw_edge_list(
            cpstate, boundary_arg, &boundary_vle_expr,
            &boundary_list_name) ||
        !is_relationships_list_name(boundary_list_name))
    {
        return NULL;
    }

    a_ind = (A_Indirection *)indexed_node;
    if (parse_arbitrary_vle_path_indexed_list_index(
            cpstate, a_ind, &indexed_vle_expr, &indexed_list_name,
            &indexed_index_expr) ||
        parse_vle_path_indexed_list_index(cpstate, a_ind, &indexed_vle_expr,
                                          &indexed_list_name,
                                          &indexed_index_expr))
    {
        if (!is_relationships_list_name(indexed_list_name))
        {
            return NULL;
        }
    }
    else
    {
        if (!parse_current_vle_edge_indirection_index(cpstate, a_ind,
                                                      &indexed_vle_expr,
                                                      &indexed_index_expr))
        {
            return NULL;
        }
    }

    if (indexed_vle_expr != boundary_vle_expr)
    {
        return NULL;
    }

    func_oid = get_age_vle_edge_indices_equal_oid();

    return make_vle_ternary_agtype_expr(func_oid, boundary_vle_expr,
                                        boundary_index_expr,
                                        indexed_index_expr);
}

static bool parse_vle_reverse_index(cypher_parsestate *cpstate, Node *node,
                                    Expr **vle_expr, Node **index_expr)
{
    A_Indirection *a_ind = NULL;
    A_Indices *indices = NULL;
    Node *edge_arg = NULL;
    FuncCall *reverse_fn = NULL;
    char *list_name = NULL;

    Assert(vle_expr != NULL);
    Assert(index_expr != NULL);

    if (!IsA(node, A_Indirection))
    {
        return false;
    }

    a_ind = (A_Indirection *)node;
    if (!IsA(a_ind->arg, FuncCall) ||
        !parse_single_indirection_value_index(a_ind, &indices))
    {
        return false;
    }

    reverse_fn = (FuncCall *)a_ind->arg;
    if (!parse_func_any_arg(reverse_fn, "reverse", &edge_arg))
    {
        return false;
    }

    if (!parse_arbitrary_vle_path_or_raw_edge_list(cpstate, edge_arg,
                                                   vle_expr, &list_name) ||
        !is_relationships_list_name(list_name))
    {
        return false;
    }

    *index_expr = transform_cypher_expr_recurse(cpstate, indices->uidx);

    return true;
}

static bool parse_vle_normal_or_boundary_index(cypher_parsestate *cpstate,
                                               Node *node, Expr **vle_expr,
                                               Node **index_expr)
{
    A_Indirection *a_ind = NULL;
    FuncCall *boundary_fn = NULL;
    Node *boundary_arg = NULL;
    char *list_name = NULL;
    char *boundary_name = NULL;
    char *boundary_list_name = NULL;

    Assert(vle_expr != NULL);
    Assert(index_expr != NULL);

    if (IsA(node, FuncCall))
    {
        boundary_fn = (FuncCall *)node;
        boundary_name = NULL;

        if (parse_head_last_func_name(boundary_fn, &boundary_name))
        {
            if (is_ag_node(linitial(boundary_fn->args), cypher_list))
            {
                cypher_list *cl = (cypher_list *)linitial(boundary_fn->args);

                if (list_length(cl->elems) != 1)
                {
                    return false;
                }

                return parse_vle_normal_or_boundary_index(
                    cpstate, linitial(cl->elems), vle_expr, index_expr);
            }
        }
    }

    if (IsA(node, A_Indirection))
    {
        a_ind = (A_Indirection *)node;

        if (parse_arbitrary_vle_path_indexed_list_index(
                cpstate, a_ind, vle_expr, &list_name, index_expr) ||
            parse_vle_path_indexed_list_index(cpstate, a_ind, vle_expr,
                                              &list_name, index_expr))
        {
            return is_relationships_list_name(list_name);
        }

        return parse_current_vle_edge_indirection_index(cpstate, a_ind,
                                                        vle_expr,
                                                        index_expr);
    }

    if (!IsA(node, FuncCall))
    {
        return false;
    }

    boundary_fn = (FuncCall *)node;
    if (!parse_vle_boundary_function(boundary_fn, NULL, &boundary_arg,
                                     index_expr))
    {
        return false;
    }

    return parse_arbitrary_vle_path_or_raw_edge_list(
            cpstate, boundary_arg, vle_expr, &boundary_list_name) &&
        is_relationships_list_name(boundary_list_name);
}

static Node *try_transform_vle_edge_normalized_equality(
    cypher_parsestate *cpstate, A_Expr *a)
{
    Expr *left_vle_expr = NULL;
    Expr *right_vle_expr = NULL;
    Node *left_index_expr = NULL;
    Node *right_index_expr = NULL;
    Oid func_oid;

    if (!is_equal_operator_name(a->name))
    {
        return NULL;
    }

    if (!parse_vle_normal_or_boundary_index(cpstate, a->lexpr,
                                            &left_vle_expr,
                                            &left_index_expr) ||
        !parse_vle_normal_or_boundary_index(cpstate, a->rexpr,
                                            &right_vle_expr,
                                            &right_index_expr))
    {
        return NULL;
    }
    if (left_vle_expr == NULL || left_vle_expr != right_vle_expr)
    {
        return NULL;
    }

    func_oid = get_age_vle_edge_indices_equal_oid();

    return make_vle_ternary_agtype_expr(func_oid, left_vle_expr,
                                        left_index_expr, right_index_expr);
}

static bool parse_vle_nested_transform_edge_equality_index(
    cypher_parsestate *cpstate, Node *node, Expr **vle_expr,
    Node **index_expr, bool *reversed_index)
{
    A_Indirection *a_ind = NULL;
    A_Indices *indices = NULL;
    Const *lower_expr = NULL;
    Const *upper_expr = NULL;
    char *list_name = NULL;
    int64 index;
    int64 mode;

    Assert(vle_expr != NULL);
    Assert(index_expr != NULL);
    Assert(reversed_index != NULL);

    if (!IsA(node, A_Indirection))
    {
        return false;
    }

    a_ind = (A_Indirection *)node;
    if (!parse_single_indirection_nonnegative_index(a_ind, &indices, &index))
    {
        return false;
    }

    if (!parse_arbitrary_vle_path_nested_transform_index(
            cpstate, a_ind, vle_expr, &list_name, &lower_expr, &upper_expr,
            &mode) ||
        !is_relationships_list_name(list_name))
    {
        return false;
    }

    if (mode >= VLE_SLICE_BOUNDARY_DOUBLE_TAIL_OFFSET)
    {
        *reversed_index = false;
        index += 2;
    }
    else if (mode >= VLE_SLICE_BOUNDARY_TAIL_REVERSE_OFFSET)
    {
        *reversed_index = true;
        index += 1;
    }
    else if (mode >= VLE_SLICE_BOUNDARY_REVERSE_OFFSET)
    {
        *reversed_index = true;
    }
    else
    {
        return false;
    }

    *index_expr = (Node *)make_agtype_integer_const(
        index, exprLocation(indices->uidx));

    return true;
}

static Node *try_transform_vle_edge_reversed_equality(
    cypher_parsestate *cpstate, A_Expr *a)
{
    Expr *reversed_vle_expr = NULL;
    Expr *normal_vle_expr = NULL;
    Node *reversed_index_expr = NULL;
    Node *normal_index_expr = NULL;
    Oid func_oid;

    if (!is_equal_operator_name(a->name))
    {
        return NULL;
    }

    if (!parse_vle_reverse_index(cpstate, a->lexpr, &reversed_vle_expr,
                                 &reversed_index_expr))
    {
        if (!parse_vle_reverse_index(cpstate, a->rexpr, &reversed_vle_expr,
                                     &reversed_index_expr))
        {
            return NULL;
        }
        if (!parse_vle_normal_or_boundary_index(cpstate, a->lexpr,
                                                &normal_vle_expr,
                                                &normal_index_expr))
        {
            return NULL;
        }
    }
    else if (!parse_vle_normal_or_boundary_index(cpstate, a->rexpr,
                                                 &normal_vle_expr,
                                                 &normal_index_expr))
    {
        return NULL;
    }

    if (normal_vle_expr == NULL || normal_vle_expr != reversed_vle_expr)
    {
        return NULL;
    }

    func_oid = get_age_vle_edge_reversed_index_equal_oid();

    return make_vle_ternary_agtype_expr(func_oid, reversed_vle_expr,
                                        reversed_index_expr,
                                        normal_index_expr);
}

static Node *try_transform_vle_edge_nested_transform_equality(
    cypher_parsestate *cpstate, A_Expr *a)
{
    Expr *nested_vle_expr = NULL;
    Expr *normal_vle_expr = NULL;
    Expr *other_nested_vle_expr = NULL;
    Node *nested_index_expr = NULL;
    Node *normal_index_expr = NULL;
    Node *other_nested_index_expr = NULL;
    bool nested_reversed = false;
    bool other_nested_reversed = false;
    Oid func_oid;

    if (!is_equal_operator_name(a->name))
    {
        return NULL;
    }

    if (!parse_vle_nested_transform_edge_equality_index(
            cpstate, a->lexpr, &nested_vle_expr, &nested_index_expr,
            &nested_reversed))
    {
        if (!parse_vle_nested_transform_edge_equality_index(
                cpstate, a->rexpr, &nested_vle_expr, &nested_index_expr,
                &nested_reversed))
        {
            return NULL;
        }
        if (!parse_vle_normal_or_boundary_index(cpstate, a->lexpr,
                                                &normal_vle_expr,
                                                &normal_index_expr))
        {
            return NULL;
        }
    }
    else if (!parse_vle_normal_or_boundary_index(cpstate, a->rexpr,
                                                 &normal_vle_expr,
                                                 &normal_index_expr))
    {
        if (!parse_vle_nested_transform_edge_equality_index(
                cpstate, a->rexpr, &other_nested_vle_expr,
                &other_nested_index_expr, &other_nested_reversed) ||
            other_nested_vle_expr == NULL ||
            other_nested_vle_expr != nested_vle_expr)
        {
            return NULL;
        }

        if (nested_reversed == other_nested_reversed)
        {
            func_oid = get_age_vle_edge_indices_equal_oid();
            return make_vle_ternary_agtype_expr(func_oid, nested_vle_expr,
                                                nested_index_expr,
                                                other_nested_index_expr);
        }
        if (nested_reversed)
        {
            func_oid = get_age_vle_edge_reversed_index_equal_oid();
            return make_vle_ternary_agtype_expr(func_oid, nested_vle_expr,
                                                nested_index_expr,
                                                other_nested_index_expr);
        }

        func_oid = get_age_vle_edge_reversed_index_equal_oid();
        return make_vle_ternary_agtype_expr(func_oid, nested_vle_expr,
                                            other_nested_index_expr,
                                            nested_index_expr);
    }

    if (normal_vle_expr == NULL || normal_vle_expr != nested_vle_expr)
    {
        return NULL;
    }

    if (nested_reversed)
    {
        func_oid = get_age_vle_edge_reversed_index_equal_oid();
    }
    else
    {
        func_oid = get_age_vle_edge_indices_equal_oid();
    }

    return make_vle_ternary_agtype_expr(func_oid, nested_vle_expr,
                                        nested_index_expr, normal_index_expr);
}

static Node *transform_BoolExpr(cypher_parsestate *cpstate, BoolExpr *expr)
{
    ParseState *pstate = (ParseState *)cpstate;
    List *args = NIL;
    const char *opname;
    ListCell *la;

    switch (expr->boolop)
    {
    case AND_EXPR:
        opname = "AND";
        break;
    case OR_EXPR:
        opname = "OR";
        break;
    case NOT_EXPR:
        opname = "NOT";
        break;
    default:
        ereport(ERROR, (errmsg_internal("unrecognized boolop: %d",
                                        (int)expr->boolop)));
        return NULL;
    }

    foreach (la, expr->args)
    {
        Node *arg = lfirst(la);

        arg = transform_cypher_expr_recurse(cpstate, arg);
        arg = coerce_to_boolean(pstate, arg, opname);

        args = lappend(args, arg);
    }

    return (Node *)makeBoolExpr(expr->boolop, args, expr->location);
}

/*
 * function for transforming cypher_comparison_boolexpr. Since this node is a
 * wrapper to let us know when a comparison occurs in a chained comparison,
 * we convert it to a PG BoolExpr and transform it.
 */
static Node *transform_cypher_comparison_boolexpr(cypher_parsestate *cpstate,
                                                  cypher_comparison_boolexpr *b)
{
    BoolExpr *n = makeNode(BoolExpr);

    n->boolop = b->boolop;
    n->args = b->args;
    n->location = b->location;

    return transform_BoolExpr(cpstate, n);
}


static Node *transform_cypher_bool_const(cypher_parsestate *cpstate,
                                         cypher_bool_const *bc)
{
    ParseState *pstate = (ParseState *)cpstate;
    ParseCallbackState pcbstate;
    Datum agt;
    Const *c;

    setup_parser_errposition_callback(&pcbstate, pstate, bc->location);
    agt = boolean_to_agtype(bc->boolean);
    cancel_parser_errposition_callback(&pcbstate);

    /* typtypmod, typcollation, typlen, and typbyval of agtype are hard-coded. */
    c = makeConst(AGTYPEOID, -1, InvalidOid, -1, agt, false, false);
    c->location = bc->location;

    return (Node *)c;
}

static Node *transform_cypher_integer_const(cypher_parsestate *cpstate,
                                            cypher_integer_const *ic)
{
    ParseState *pstate = (ParseState *)cpstate;
    ParseCallbackState pcbstate;
    Datum agt;
    Const *c;

    setup_parser_errposition_callback(&pcbstate, pstate, ic->location);
    agt = integer_to_agtype(ic->integer);
    cancel_parser_errposition_callback(&pcbstate);

    /* typtypmod, typcollation, typlen, and typbyval of agtype are hard-coded. */
    c = makeConst(AGTYPEOID, -1, InvalidOid, -1, agt, false, false);
    c->location = ic->location;

    return (Node *)c;
}

static Const *make_agtype_integer_const(int64 value, int location)
{
    Datum agt;
    Const *c;

    agt = integer_to_agtype(value);
    c = makeConst(AGTYPEOID, -1, InvalidOid, -1, agt, false, false);
    c->location = location;

    return c;
}

static Node *make_null_agtype_const(void)
{
    return (Node *)makeNullConst(AGTYPEOID, -1, InvalidOid);
}

static Node *transform_or_null_agtype(cypher_parsestate *cpstate, Node *node)
{
    if (node == NULL)
        return make_null_agtype_const();

    return transform_cypher_expr_recurse(cpstate, node);
}

static void transform_slice_bounds_or_null(cypher_parsestate *cpstate,
                                           A_Indices *indices,
                                           Node **lower_expr,
                                           Node **upper_expr)
{
    *lower_expr = transform_or_null_agtype(cpstate, indices->lidx);
    *upper_expr = transform_or_null_agtype(cpstate, indices->uidx);
}

static void make_slice_bounds_to_null(int64 lower, int location,
                                      Node **lower_expr,
                                      Node **upper_expr)
{
    *lower_expr = (Node *)make_agtype_integer_const(lower, location);
    *upper_expr = make_null_agtype_const();
}

static Node *make_vle_boundary_index_expr(const char *boundary_name,
                                          int location)
{
    if (is_head_name(boundary_name))
        return (Node *)make_agtype_integer_const(0, location);
    if (is_last_name(boundary_name))
        return (Node *)make_agtype_integer_const(-1, location);

    return NULL;
}

static bool parse_vle_boundary_function(FuncCall *fn, char **boundary_name,
                                        Node **arg, Node **index_expr)
{
    char *parsed_name = NULL;
    Node *parsed_index_expr = NULL;

    if (!parse_head_last_func_name(fn, &parsed_name))
    {
        return false;
    }

    parsed_index_expr = make_vle_boundary_index_expr(parsed_name,
                                                     fn->location);
    if (parsed_index_expr == NULL)
    {
        return false;
    }

    if (boundary_name != NULL)
    {
        *boundary_name = parsed_name;
    }
    if (arg != NULL)
    {
        *arg = linitial(fn->args);
    }
    if (index_expr != NULL)
    {
        *index_expr = parsed_index_expr;
    }

    return true;
}

static Node *transform_cypher_param(cypher_parsestate *cpstate,
                                    cypher_param *cp)
{
    ParseState *pstate = (ParseState *)cpstate;
    Const *const_str;
    FuncExpr *func_expr;
    Oid func_access_oid;
    List *args = NIL;

    if (!cpstate->params)
    {
        ereport(
            ERROR,
            (errcode(ERRCODE_UNDEFINED_PARAMETER),
             errmsg(
                 "parameters argument is missing from cypher() function call"),
             parser_errposition(pstate, cp->location)));
    }

    /* get the agtype_access_operator function */
    func_access_oid = get_agtype_access_operator_oid();

    args = lappend(args, copyObject(cpstate->params));

    const_str = makeConst(AGTYPEOID, -1, InvalidOid, -1,
                          string_to_agtype(cp->name), false, false);

    args = lappend(args, const_str);

    func_expr = makeFuncExpr(func_access_oid, AGTYPEOID, args, InvalidOid,
                             InvalidOid, COERCE_EXPLICIT_CALL);
    func_expr->location = cp->location;

    return (Node *)func_expr;
}

static Node *try_transform_entity_properties_expr(cypher_parsestate *cpstate,
                                                  Node *node, int location,
                                                  bool previous_vertex_ok)
{
    ParseState *pstate = (ParseState *)cpstate;
    char *var_name;
    transform_entity *entity;
    ParseNamespaceItem *pnsi;
    int levels_up;

    if (is_output_expr_kind(pstate->p_expr_kind))
        return NULL;

    if (!parse_single_column_ref(node, &var_name, &location))
    {
        return NULL;
    }

    entity = find_variable(cpstate, var_name);
    if (entity == NULL ||
        (entity->type != ENT_VERTEX && entity->type != ENT_EDGE))
        return NULL;

    if (entity->declared_in_current_clause)
    {
        pnsi = refnameNamespaceItem(pstate, NULL, var_name, location,
                                    &levels_up);
        if (pnsi == NULL)
            return NULL;

        return scanNSItemForColumn(pstate, pnsi, levels_up,
                                   AG_VERTEX_COLNAME_PROPERTIES,
                                   location);
    }

    if (entity->type == ENT_VERTEX && !previous_vertex_ok)
        return NULL;

    node = make_raw_attr_var(pstate, var_name, AG_VERTEX_COLNAME_PROPERTIES,
                             location);

    return node;
}

static Node *transform_cypher_map_projection(cypher_parsestate *cpstate,
                                             cypher_map_projection *cmp)
{
    ParseState *pstate;
    ListCell *lc;
    List *keyvals;
    Oid foid_agtype_build_map;
    FuncExpr *fexpr_new_map;
    bool has_all_prop_selector;
    Node *transformed_map_var;
    Oid foid_age_properties;
    Node *orig_map_expr;

    pstate = (ParseState *)cpstate;
    keyvals = NIL;
    has_all_prop_selector = false;
    fexpr_new_map = NULL;

    /*
     * Builds the original map: `age_properties(cmp->map_var)`. Whether map_var
     * is compatible (map, vertex or edge) is checked during the execution of
     * age_properties().
     */
    transformed_map_var = try_transform_entity_properties_expr(
        cpstate, (Node *)cmp->map_var, cmp->location, true);
    if (transformed_map_var != NULL)
    {
        orig_map_expr = transformed_map_var;
    }
    else
    {
        FuncExpr *fexpr_orig_map;

        transformed_map_var = transform_cypher_expr_recurse(
            cpstate, (Node *)cmp->map_var);
        foid_age_properties = get_age_properties_oid();
        fexpr_orig_map = makeFuncExpr(foid_age_properties, AGTYPEOID,
                                      list_make1(transformed_map_var),
                                      InvalidOid, InvalidOid,
                                      COERCE_EXPLICIT_CALL);
        fexpr_orig_map->location = cmp->location;
        orig_map_expr = (Node *)fexpr_orig_map;
    }

    /*
     * Builds a new map. Each map projection element is transformed into a key
     * value pair (except for the ALL_PROPERTIES_SELECTOR type).
     */
    foreach (lc, cmp->map_elements)
    {
        cypher_map_projection_element *elem;
        Const *key;
        Node *val;

        elem = lfirst(lc);
        key = NULL;
        val = NULL;

        if (elem->type == ALL_PROPERTIES_SELECTOR)
        {
            has_all_prop_selector = true;
            continue;
        }

        /* Makes key and val based on elem->type */
        switch (elem->type)
        {
            case PROPERTY_SELECTOR:
            {
                Const *key_agtype;

                /* Makes key from elem->key */
                key = makeConst(TEXTOID, -1, InvalidOid, -1,
                                CStringGetTextDatum(elem->key), false, false);

                /* Makes val from `age_properties(cmp->map_var).key` */
                key_agtype = makeConst(AGTYPEOID, -1, InvalidOid, -1,
                                       string_to_agtype(elem->key), false,
                                       false);
                val = (Node *)make_agtype_access_expr(
                    list_make2(orig_map_expr, key_agtype), -1, false);

                break;
            }
            case LITERAL_ENTRY:
            {
                key = makeConst(TEXTOID, -1, InvalidOid, -1,
                                CStringGetTextDatum(elem->key), false, false);
                val = transform_cypher_expr_recurse(cpstate, elem->value);
                break;
            }
            case VARIABLE_SELECTOR:
            {
                char *key_str;
                List *fields;

                Assert(IsA(elem->value, ColumnRef));

                /* Makes key from the ColumnRef's field */
                fields = ((ColumnRef *)elem->value)->fields;
                key_str = strVal(lfirst(list_head(fields)));
                key = makeConst(TEXTOID, -1, InvalidOid, -1,
                                CStringGetTextDatum(key_str), false, false);

                val = transform_cypher_expr_recurse(cpstate, elem->value);
                break;
            }
            case ALL_PROPERTIES_SELECTOR:
            {
                /*
                 * Key value pairs of the original map are added later outside
                 * the loop. Control never reaches this block.
                 */
                break;
            }
            default:
            {
                elog(ERROR, "unknown map projection element type");
            }
        }

        Assert(key);
        Assert(val);
        keyvals = lappend(lappend(keyvals, key), val);
    }

    if (keyvals)
    {
        foid_agtype_build_map = get_agtype_build_map_nonull_oid();
        fexpr_new_map = makeFuncExpr(foid_agtype_build_map, AGTYPEOID, keyvals,
                                     InvalidOid, InvalidOid,
                                     COERCE_EXPLICIT_CALL);
        fexpr_new_map->location = cmp->location;
    }

    /*
     * In case .* is present, returns age_properties(cmp->map_var) + the new
     * map. Else, returns the new map.
     */
    if (has_all_prop_selector)
    {
        if (!keyvals)
        {
            return orig_map_expr;
        }
        else
        {
            return (Node *)make_op(pstate, list_make1(makeString("+")),
                                   orig_map_expr,
                                   (Node *)fexpr_new_map,
                                   pstate->p_last_srf, -1);
        }
    }
    else
    {
        Assert(!has_all_prop_selector && fexpr_new_map);
        return (Node *)fexpr_new_map;
    }
}

/*
 * Helper function to transform a cypher map into an agtype map. The function
 * will use agtype_add to concatenate the argument list when the number of
 * parameters (keys and values) exceeds 100, a PG limitation.
 */
static Node *transform_cypher_map(cypher_parsestate *cpstate, cypher_map *cm)
{
    ParseState *pstate = (ParseState *)cpstate;
    List *newkeyvals_args = NIL;
    ListCell *le = NULL;
    FuncExpr *fexpr = NULL;
    FuncExpr *aa_lhs_arg = NULL;
    Oid abm_func_oid = InvalidOid;
    Oid aa_func_oid = InvalidOid;
    int nkeyvals = 0;
    int i = 0;

    /* get the number of keys and values */
    nkeyvals = list_length(cm->keyvals);

    /* error out if it isn't even */
    if (nkeyvals % 2 != 0)
    {
         ereport(ERROR,
                (errcode(ERRCODE_DATA_EXCEPTION),
                 errmsg("number of keys does not match number of values")));
    }

    if (nkeyvals == 0)
    {
        abm_func_oid = get_agtype_build_empty_map_oid();
    }
    else if (!cm->keep_null)
    {
        abm_func_oid = get_agtype_build_map_nonull_oid();
    }
    else
    {
        abm_func_oid = get_agtype_build_map_oid();
    }

    /* get the concat function oid, if necessary */
    if (nkeyvals > 100)
    {
        aa_func_oid = get_agtype_add_oid();
    }

    /* get the key/val list */
    le = list_head(cm->keyvals);
    /* while we have key/val to process */
    while (le != NULL)
    {
        Node *key = NULL;
        Node *val = NULL;
        Node *newval = NULL;
        ParseCallbackState pcbstate;
        Const *newkey = NULL;

        /* get the key */
        key = lfirst(le);
        le = lnext(cm->keyvals, le);
        /* get the value */
        val = lfirst(le);
        le = lnext(cm->keyvals, le);

        /* transform the value */
        newval = transform_cypher_expr_recurse(cpstate, val);

        /*
         * If we have more than 50 key/value pairs, 100 elements, we will need
         * to add in the list concatenation function.
         */
        if (i >= 50)
        {
            /* build the object for the first 50 pairs for concat */
            fexpr = makeFuncExpr(abm_func_oid, AGTYPEOID, newkeyvals_args,
                                 InvalidOid, InvalidOid, COERCE_EXPLICIT_CALL);
            fexpr->location = cm->location;

            /* initial case, set up for concatenating 2 lists */
            if (aa_lhs_arg == NULL)
            {
                aa_lhs_arg = fexpr;
            }
            /*
             * For every other case, concatenate the list on to the previous
             * concatenate operation.
             */
            else
            {
                List *aa_args = list_make2(aa_lhs_arg, fexpr);

                fexpr = makeFuncExpr(aa_func_oid, AGTYPEOID, aa_args,
                                     InvalidOid, InvalidOid,
                                     COERCE_EXPLICIT_CALL);
                fexpr->location = cm->location;

                /* set the lhs to the concatenation operation */
                aa_lhs_arg = fexpr;
            }

            /* reset for the next 50 pairs */
            newkeyvals_args = NIL;
            i = 0;
            fexpr = NULL;
        }

        /* build and append the transformed key/val pair */
        setup_parser_errposition_callback(&pcbstate, pstate, cm->location);
        /* typtypmod, typcollation, typlen, and typbyval of agtype are */
        /* hard-coded. */
        newkey = makeConst(TEXTOID, -1, InvalidOid, -1,
                           CStringGetTextDatum(strVal(key)), false, false);
        cancel_parser_errposition_callback(&pcbstate);

        newkeyvals_args = lappend(lappend(newkeyvals_args, newkey), newval);

        i++;
    }

    /* now build the final map function */
    fexpr = makeFuncExpr(abm_func_oid, AGTYPEOID, newkeyvals_args, InvalidOid,
                         InvalidOid, COERCE_EXPLICIT_CALL);
    fexpr->location = cm->location;

    /*
     * If there was a previous concatenation, build a final concatenation
     * function node.
     */
    if (aa_lhs_arg != NULL)
    {
        List *aa_args = list_make2(aa_lhs_arg, fexpr);

        fexpr = makeFuncExpr(aa_func_oid, AGTYPEOID, aa_args, InvalidOid,
                             InvalidOid, COERCE_EXPLICIT_CALL);
    }

    return (Node *)fexpr;
}

/*
 * Helper function to transform a cypher list into an agtype list. The function
 * will use agtype_add to concatenate argument lists when the number of list
 * elements, parameters, exceeds 100, a PG limitation.
 */
static Node *transform_cypher_list(cypher_parsestate *cpstate, cypher_list *cl)
{
    List *abl_args = NIL;
    ListCell *le = NULL;
    FuncExpr *aa_lhs_arg = NULL;
    FuncExpr *fexpr = NULL;
    Oid abl_func_oid = InvalidOid;
    Oid aa_func_oid = InvalidOid;
    int nelems = 0;
    int i = 0;

    /* determine which build function we need */
    nelems = list_length(cl->elems);
    if (nelems == 0)
    {
        abl_func_oid = get_agtype_build_empty_list_oid();
    }
    else
    {
        abl_func_oid = get_agtype_build_list_oid();
    }

    /* get the concat function oid, if necessary */
    if (nelems > 100)
    {
        aa_func_oid = get_agtype_add_oid();
    }

    /* iterate through the list of elements */
    foreach (le, cl->elems)
    {
        Node *texpr = NULL;

        /* transform the argument */
        texpr = transform_cypher_expr_recurse(cpstate, lfirst(le));

        /*
         * If we have more than 100 elements we will need to add in the list
         * concatenation function.
         */
        if (i >= 100)
        {
            /* build the list function node argument for concatenate */
            fexpr = makeFuncExpr(abl_func_oid, AGTYPEOID, abl_args, InvalidOid,
                                 InvalidOid, COERCE_EXPLICIT_CALL);
            fexpr->location = cl->location;

            /* initial case, set up for concatenating 2 lists */
            if (aa_lhs_arg == NULL)
            {
                aa_lhs_arg = fexpr;
            }
            /*
             * For every other case, concatenate the list on to the previous
             * concatenate operation.
             */
            else
            {
                List *aa_args = list_make2(aa_lhs_arg, fexpr);

                fexpr = makeFuncExpr(aa_func_oid, AGTYPEOID, aa_args,
                                     InvalidOid, InvalidOid,
                                     COERCE_EXPLICIT_CALL);
                fexpr->location = cl->location;

                /* set the lhs to the concatenation operation */
                aa_lhs_arg = fexpr;
            }

            /* reset */
            abl_args = NIL;
            i = 0;
            fexpr = NULL;
        }

        /* now add the latest transformed expression to the list */
        abl_args = lappend(abl_args, texpr);
        i++;
    }

    /* now build the final list function */
    fexpr = makeFuncExpr(abl_func_oid, AGTYPEOID, abl_args, InvalidOid,
                         InvalidOid, COERCE_EXPLICIT_CALL);
    fexpr->location = cl->location;

    /*
     * If there was a previous concatenation or list function, build a final
     * concatenation function node
     */
    if (aa_lhs_arg != NULL)
    {
        List *aa_args = list_make2(aa_lhs_arg, fexpr);

        fexpr = makeFuncExpr(aa_func_oid, AGTYPEOID, aa_args, InvalidOid,
                             InvalidOid, COERCE_EXPLICIT_CALL);
    }

    return (Node *)fexpr;
}

/* makes a VARIADIC agtype array */
static ArrayExpr *make_agtype_array_expr(List *args)
{
    ArrayExpr  *newa = makeNode(ArrayExpr);

    newa->elements = args;

    /* assume all the variadic arguments were coerced to the same type */
    newa->element_typeid = AGTYPEOID;
    newa->array_typeid = AGTYPEARRAYOID;

    if (!OidIsValid(newa->array_typeid))
    {
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_OBJECT),
                 errmsg("could not find array type for data type %s",
                        format_type_be(newa->element_typeid))));
    }

    /* array_collid will be set by parse_collate.c */
    newa->multidims = false;

    return newa;
}

/*
 * Transform a ColumnRef for indirection. Try to find the rte that the ColumnRef
 * references and pass the properties of that rte as what the ColumnRef is
 * referencing. Otherwise, reference the Var.
 */
static Node *transform_column_ref_for_indirection(cypher_parsestate *cpstate,
                                                  ColumnRef *cr)
{
    ParseState *pstate = (ParseState *)cpstate;
    ParseNamespaceItem *pnsi = NULL;
    Node *field1 = linitial(cr->fields);
    char *relname = NULL;
    Node *node = NULL;
    int levels_up = 0;
    transform_entity *entity;

    Assert(IsA(field1, String));
    relname = strVal(field1);

    /* locate the referenced RTE (used to be find_rte(cpstate, relname)) */
    pnsi = refnameNamespaceItem(pstate, NULL, relname, cr->location,
                                &levels_up);

    /*
     * If we didn't find anything, try looking for a previous variable
     * reference. Otherwise, return NULL (colNameToVar will return NULL
     * if nothing is found).
     */
    if (!pnsi)
    {
        Node *prev_var;

        entity = find_variable(cpstate, relname);
        if (entity != NULL &&
            entity->has_raw_targets &&
            (entity->type == ENT_VERTEX || entity->type == ENT_EDGE))
        {
            Node *raw_props;

            raw_props = make_raw_attr_var(pstate, relname,
                                          AG_VERTEX_COLNAME_PROPERTIES,
                                          cr->location);

            if (raw_props != NULL)
                return raw_props;
        }

        prev_var = colNameToVar(pstate, relname, false, cr->location);

        return prev_var;
    }

    /* find the properties column of the NSI and return a var for it */
    node = scanNSItemForColumn(pstate, pnsi, levels_up, "properties",
                               cr->location);
    if (node == NULL)
    {
        entity = find_variable(cpstate, relname);
        if (entity != NULL && entity->has_raw_targets)
        {
            node = make_raw_attr_var(pstate, relname,
                                     AG_VERTEX_COLNAME_PROPERTIES,
                                     cr->location);
        }
    }

    /*
     * If there's no "properties" column, continue transforming the
     * ColumnRef as an agtype value and try to apply the indirection via
     * agtype_access_operator.
     */
    return node;
}

typedef Node *(*cypher_fast_indirection_transform)(cypher_parsestate *cpstate,
                                                   A_Indirection *a_ind);

static Node *try_transform_fast_indirection(cypher_parsestate *cpstate,
                                            A_Indirection *a_ind)
{
    static const cypher_fast_indirection_transform fast_transforms[] = {
        try_transform_vle_terminal_vertex_property_access,
        try_transform_fixed_path_indexed_endpoint_property_access,
        try_transform_current_edge_endpoint_property_access,
        try_transform_fixed_path_slice_endpoint_property_access,
        try_transform_fixed_path_indexed_property_access,
        try_transform_fixed_path_slice_head_last_property_access,
        try_transform_fixed_path_list_slice,
        try_transform_vle_path_tail_access,
        try_transform_vle_path_reverse_access,
        try_transform_vle_path_nested_transform_access,
        try_transform_vle_path_nodes_access,
        try_transform_vle_path_nodes_slice,
        try_transform_vle_path_list_slice,
        try_transform_vle_path_boundary_property_access,
        try_transform_vle_path_relationships_access,
        try_transform_vle_edge_reverse_access,
    };
    int i;

    for (i = 0; i < lengthof(fast_transforms); i++)
    {
        Node *fast_expr;

        fast_expr = fast_transforms[i](cpstate, a_ind);
        if (fast_expr != NULL)
        {
            return fast_expr;
        }
    }

    return NULL;
}

static Node *transform_A_Indirection(cypher_parsestate *cpstate,
                                     A_Indirection *a_ind)
{
    ParseState *pstate = &cpstate->pstate;
    int location;
    ListCell *lc = NULL;
    Node *ind_arg_expr = NULL;
    FuncExpr *func_expr = NULL;
    Oid func_slice_oid = InvalidOid;
    List *args = NIL;
    bool is_access = false;
    Node *fast_expr = NULL;

    /* validate that we have an indirection with at least 1 entry */
    Assert(a_ind != NULL && list_length(a_ind->indirection));

    fast_expr = try_transform_fast_indirection(cpstate, a_ind);
    if (fast_expr != NULL)
    {
        return fast_expr;
    }

    /*
     * If the indirection argument is a ColumnRef, we want to pull out the
     * properties, as a var node, if possible.
     */
    if (IsA(a_ind->arg, ColumnRef))
    {
        ColumnRef *cr = (ColumnRef *)a_ind->arg;

        ind_arg_expr = transform_column_ref_for_indirection(cpstate, cr);
    }

    /*
     * If we didn't get the properties from a ColumnRef, just transform the
     * indirection argument.
     */
    if (ind_arg_expr == NULL)
    {
        ind_arg_expr = transform_cypher_expr_recurse(cpstate, a_ind->arg);
    }

    ind_arg_expr = coerce_to_common_type(pstate, ind_arg_expr, AGTYPEOID,
                                         "A_indirection");

    /* get the location of the expression */
    location = exprLocation(ind_arg_expr);

    /* add the expression as the first entry */
    args = lappend(args, ind_arg_expr);

    /* iterate through the indirections */
    foreach (lc, a_ind->indirection)
    {
        Node *node = lfirst(lc);

        /* is this a slice? */
        if (IsA(node, A_Indices) && ((A_Indices *)node)->is_slice)
        {
            A_Indices *indices = (A_Indices *)node;

            /* were we working on an access? if so, wrap and close it */
            if (is_access)
            {
                func_expr = make_agtype_access_expr(
                    args, location,
                    pstate->p_expr_kind == EXPR_KIND_SELECT_TARGET &&
                    cpstate->expand_select_target_access);

                /*
                 * The result of this function is the input to the next access
                 * or slice operator. So we need to start out with a new arg
                 * list with this function expression.
                 */
                args = lappend(NIL, func_expr);

                /* we are no longer working on an access */
                is_access = false;

            }

            /* add slice bounds to args */
            if (!indices->lidx)
            {
                A_Const *n = makeNode(A_Const);
                n->isnull = true;
                n->location = -1;
                node = transform_cypher_expr_recurse(cpstate, (Node *)n);
            }
            else
            {
                node = transform_cypher_expr_recurse(cpstate, indices->lidx);
            }

            args = lappend(args, node);

            if (!indices->uidx)
            {
                A_Const *n = makeNode(A_Const);
                n->isnull = true;
                n->location = -1;
                node = transform_cypher_expr_recurse(cpstate, (Node *)n);
            }
            else
            {
                node = transform_cypher_expr_recurse(cpstate, indices->uidx);
            }
            args = lappend(args, node);

            /* wrap and close it */
            if (!OidIsValid(func_slice_oid))
                func_slice_oid = get_agtype_access_slice_oid();
            func_expr = makeFuncExpr(func_slice_oid, AGTYPEOID, args,
                                     InvalidOid, InvalidOid,
                                     COERCE_EXPLICIT_CALL);
            func_expr->location = location;

            /*
             * The result of this function is the input to the next access
             * or slice operator. So we need to start out with a new arg
             * list with this function expression.
             */
            args = lappend(NIL, func_expr);
        }
        /* is this a string or index?*/
        else if (IsA(node, String) || IsA(node, A_Indices))
        {
            /* we are working on an access */
            is_access = true;

            /* is this an index? */
            if (IsA(node, A_Indices))
            {
                A_Indices *indices = (A_Indices *)node;

                node = transform_cypher_expr_recurse(cpstate, indices->uidx);
                args = lappend(args, node);
            }
            /* it must be a string */
            else
            {
                args = lappend(args, make_agtype_string_key_const(node));
            }
        }
        /* not an indirection we understand */
        else
        {
            ereport(ERROR,
                    (errmsg("invalid indirection node %d", nodeTag(node))));
        }
    }

    /* if we were doing an access, we need wrap the args with access func. */
    if (is_access)
    {
        func_expr = make_agtype_access_expr(
            args, location,
            pstate->p_expr_kind == EXPR_KIND_SELECT_TARGET &&
            cpstate->expand_select_target_access);
    }

    Assert(func_expr != NULL);
    func_expr->location = location;

    return (Node *)func_expr;
}

static Node *transform_cypher_string_match(cypher_parsestate *cpstate,
                                           cypher_string_match *csm_node)
{
    Node *expr;
    FuncExpr *func_expr;
    Oid func_access_oid;
    List *args = NIL;

    switch (csm_node->operation)
    {
    case CSMO_STARTS_WITH:
        func_access_oid = get_agtype_string_match_starts_with_oid();
        break;
    case CSMO_ENDS_WITH:
        func_access_oid = get_agtype_string_match_ends_with_oid();
        break;
    case CSMO_CONTAINS:
        func_access_oid = get_agtype_string_match_contains_oid();
        break;

    default:
        ereport(ERROR,
                (errmsg_internal("unknown Cypher string match operation")));
    }

    expr = transform_cypher_expr_recurse(cpstate, csm_node->lhs);
    args = lappend(args, expr);
    expr = transform_cypher_expr_recurse(cpstate, csm_node->rhs);
    args = lappend(args, expr);

    func_expr = makeFuncExpr(func_access_oid, AGTYPEOID, args, InvalidOid,
                             InvalidOid, COERCE_EXPLICIT_CALL);
    func_expr->location = csm_node->location;

    return (Node *)func_expr;
}

/*
 * Function to create a typecasting node
 */
static Node *transform_cypher_typecast(cypher_parsestate *cpstate,
                                       cypher_typecast *ctypecast)
{
    List *fname;
    FuncCall *fnode;
    ParseState *pstate;
    TypeName *target_typ;

    /* verify input parameter */
    Assert (cpstate != NULL);
    Assert (ctypecast != NULL);

    /* create the qualified function name, schema first */
    fname = list_make1(makeString("ag_catalog"));
    pstate = &cpstate->pstate;
    target_typ = ctypecast->typname;

    if (list_length(target_typ->names) == 1)
    {
        char *typecast = strVal(linitial(target_typ->names));
        int typecast_len = strlen(typecast);
        Oid scalar_property_helper_oid = InvalidOid;
        Oid scalar_property_result_type = InvalidOid;

        /* append the name of the requested typecast function */
        if (typecast_len == 4 && pg_strcasecmp(typecast, "edge") == 0)
        {
            fname = lappend(fname, makeString(FUNC_AGTYPE_TYPECAST_EDGE));
        }
        else if (typecast_len == 4 && pg_strcasecmp(typecast, "path") == 0)
        {
            fname = lappend(fname, makeString(FUNC_AGTYPE_TYPECAST_PATH));
        }
        else if (typecast_len == 6 && pg_strcasecmp(typecast, "vertex") == 0)
        {
            fname = lappend(fname, makeString(FUNC_AGTYPE_TYPECAST_VERTEX));
        }
        else if (typecast_len == 7 && pg_strcasecmp(typecast, "numeric") == 0)
        {
            fname = lappend(fname, makeString(FUNC_AGTYPE_TYPECAST_NUMERIC));
            scalar_property_helper_oid =
                get_agtype_object_field_numeric_agtype_oid();
            scalar_property_result_type = AGTYPEOID;
        }
        else if (typecast_len == 5 && pg_strcasecmp(typecast, "float") == 0)
        {
            fname = lappend(fname, makeString(FUNC_AGTYPE_TYPECAST_FLOAT));
        }
        else if ((typecast_len == 3 &&
                  pg_strcasecmp(typecast, "int") == 0) ||
                 (typecast_len == 7 &&
                  pg_strcasecmp(typecast, "integer") == 0))
        {
            fname = lappend(fname, makeString(FUNC_AGTYPE_TYPECAST_INT));
        }
        else if (typecast_len == 9 &&
                 pg_strcasecmp(typecast, "pg_float8") == 0)
        {
            fname = lappend(fname, makeString(FUNC_AGTYPE_TYPECAST_PG_FLOAT8));
            scalar_property_helper_oid = get_agtype_object_field_float8_oid();
            scalar_property_result_type = FLOAT8OID;
        }
        else if (typecast_len == 9 &&
                 pg_strcasecmp(typecast, "pg_bigint") == 0)
        {
            fname = lappend(fname, makeString(FUNC_AGTYPE_TYPECAST_PG_BIGINT));
            scalar_property_helper_oid = get_agtype_object_field_int8_oid();
            scalar_property_result_type = INT8OID;
        }
        else if (typecast_len == 10 &&
                 pg_strcasecmp(typecast, "pg_numeric") == 0)
        {
            fname = lappend(fname, makeString(FUNC_AGTYPE_TYPECAST_PG_NUMERIC));
            scalar_property_helper_oid = get_agtype_object_field_numeric_oid();
            scalar_property_result_type = NUMERICOID;
        }
        else if ((typecast_len == 4 &&
                  pg_strcasecmp(typecast, "bool") == 0) ||
                 (typecast_len == 7 &&
                  pg_strcasecmp(typecast, "boolean") == 0))
        {
            fname = lappend(fname, makeString(FUNC_AGTYPE_TYPECAST_BOOL));
        }
        else if (typecast_len == 7 &&
                 pg_strcasecmp(typecast, "pg_text") == 0)
        {
            fname = lappend(fname, makeString(FUNC_AGTYPE_TYPECAST_PG_TEXT));
            scalar_property_helper_oid =
                get_agtype_object_field_text_agtype_oid();
            scalar_property_result_type = TEXTOID;
        }
        else
        {
            goto fallback_coercion;
        }

        /* make a function call node */
        fnode = makeFuncCall(fname, list_make1(ctypecast->expr), COERCE_SQL_SYNTAX,
        ctypecast->location);

        /* return the transformed function */
        {
            Node *result = transform_FuncCall(cpstate, fnode);

            if (OidIsValid(scalar_property_helper_oid) &&
                OidIsValid(scalar_property_result_type) &&
                IsA(result, FuncExpr))
            {
                Node *rewritten;

                rewritten = try_rewrite_pg_scalar_property_access(
                    castNode(FuncExpr, result), scalar_property_helper_oid,
                    scalar_property_result_type);
                if (rewritten != NULL)
                    return rewritten;
            }

            return result;
        }
    }

fallback_coercion:
    {
        Oid source_oid;
        Oid target_oid;
        int32 t_typmod = -1;
        Node *expr;

        /* transform the expr before casting */
        expr = transform_cypher_expr_recurse(cpstate,
                                             ctypecast->expr);

        typenameTypeIdAndMod(pstate, target_typ, &target_oid, &t_typmod);
        source_oid = exprType(expr);

        /* errors out if cast not possible */
        expr = coerce_expr_flexible(pstate, expr, source_oid, target_oid,
                                    t_typmod, true);

        return expr;
    }
}

/*
 * Helper function to coerce an expression to the target type. If
 * no direct cast exists, it attempts to cast through text if the
 * source or target type is agtype. This improves interoperability
 * with types from other extensions.
 */
static Node *coerce_expr_flexible(ParseState *pstate, Node *expr,
                                  Oid source_oid, Oid target_oid,
                                  int32 t_typmod, bool error_out)
{
    const Oid text_oid = TEXTOID;
    Node *result;

    if (expr == NULL)
        return NULL;

    /* Try a direct cast */
    result = coerce_to_target_type(pstate, expr, source_oid, target_oid,
                                   t_typmod, COERCION_EXPLICIT,
                                   COERCE_EXPLICIT_CAST, -1);
    if (result != NULL)
        return result;

    /* Try cast via TEXT if either side is AGTYPE */
    if (source_oid == AGTYPEOID || target_oid == AGTYPEOID)
    {
        Node *to_text = coerce_to_target_type(pstate, expr, source_oid, text_oid,
                                              -1, COERCION_EXPLICIT,
                                              COERCE_EXPLICIT_CAST, -1);
        if (to_text != NULL)
        {
            result = coerce_to_target_type(pstate, to_text, text_oid, target_oid,
                                           t_typmod, COERCION_EXPLICIT,
                                           COERCE_EXPLICIT_CAST, -1);
            if (result != NULL)
                return result;
        }
    }

    if (error_out)
    {
        ereport(ERROR,
                (errmsg_internal("typecast \'%s\' not supported",
                                 format_type_be(target_oid))));
    }

    return NULL;
}

static Node *transform_external_ext_FuncCall(cypher_parsestate *cpstate,
                                             FuncCall *fn, List *targs,
                                             Form_pg_proc procform,
                                             const char *extension)
{
    ParseState *pstate = &cpstate->pstate;
    FuncExpr *fexpr = NULL;
    Node *retval = NULL;
    Node *last_srf = pstate->p_last_srf;
    Oid *proargtypes;

    /* make sure procform in not NULL */
    Assert(procform != NULL);
    proargtypes = procform->proargtypes.values;

    /* cast the agtype arguments to the types accepted by function */
    targs = cast_agtype_args_to_target_type(cpstate, procform, targs, proargtypes);

    /* now get the function node for the external function */
    fexpr = (FuncExpr *)ParseFuncOrColumn(pstate, fn->funcname, targs,
                                          last_srf, fn, false,
                                          fn->location);
    pfree(procform);

    /*
     * This will cast TEXT output to AGTYPE. It will error out if this is
     * not possible to do. For TEXT to AGTYPE we need to wrap the output
     * due to issues with creating a cast from TEXT to AGTYPE.
     */
    if (fexpr->funcresulttype == TEXTOID)
    {
        retval = wrap_text_output_to_agtype(cpstate, fexpr);
    }
    else
    {
        retval = (Node *)fexpr;
    }

    /* additional casts or wraps can be done here for other types */

    /* flag that an aggregate was found during a transform */
    if (retval != NULL && retval->type == T_Aggref)
    {
        cpstate->exprHasAgg = true;
    }

    /* we can just return it here */
    return retval;
}

/*
 * Cast a function's input parameter list from agtype to that function's input
 * type. This is used for functions that don't take agtype as input and where
 * there isn't an implicit cast to do this for us.
 */
static List *cast_agtype_args_to_target_type(cypher_parsestate *cpstate,
                                             Form_pg_proc procform,
                                             List *fargs,
                                             Oid *target_types)
{
    char *funcname = NameStr(procform->proname);
    int nargs = procform->pronargs;
    int given_nargs = list_length(fargs);
    ListCell *lc = NULL;

    /* verify the length of args are same */
    if (given_nargs != nargs)
    {
        ereport(ERROR,
                (errcode(ERRCODE_DATA_EXCEPTION),
                 errmsg("function %s requires %d arguments, %d given",
                        funcname, nargs, given_nargs)));
    }

    /* iterate through the function's args */
    foreach (lc, fargs)
    {
        Node *expr = lfirst(lc);
        Oid source_oid = exprType(expr);
        Oid target_oid = target_types[foreach_current_index(lc)];

        if (source_oid == target_oid)
        {
            continue;
        }

        /* errors out if cast not possible */
        expr = coerce_expr_flexible(&cpstate->pstate, expr, source_oid,
                                     target_oid, -1, true);

        lfirst(lc) = expr;
    }

    return fargs;
}

/*
 * Due to issues with creating a cast from text to agtype, we need to wrap a
 * function that outputs text with text_to_agtype.
 */
static Node *wrap_text_output_to_agtype(cypher_parsestate *cpstate,
                                        FuncExpr *fexpr)
{
    Oid func_oid;
    FuncExpr *retval;

    if (fexpr->funcresulttype != TEXTOID)
    {
        ereport(ERROR,
                (errcode(ERRCODE_DATA_EXCEPTION),
                 errmsg("can only wrap text to agtype")));
    }

    func_oid = get_text_to_agtype_oid();
    retval = makeFuncExpr(func_oid, AGTYPEOID, list_make1(fexpr), InvalidOid,
                          InvalidOid, COERCE_SQL_SYNTAX);
    retval->location = fexpr->location;

    /* return the wrapped function */
    return (Node *)retval;
}

/*
 * Returns Form_pg_proc struct for given function, if the function
 * is not in search path, it is not considered.
 */
static Form_pg_proc get_procform(FuncCall *fn, bool err_not_found)
{
    CatCList *catlist = NULL;
    Form_pg_proc procform = NULL;
    Form_pg_proc result = NULL;
    int nargs;
    int i = 0;
    List *asp = NIL;
    bool asp_fetched = false;
    bool found = false;
    char *funcname = NULL;
    int funcname_len;

    if (!parse_single_func_name(fn, &funcname))
        return NULL;
    funcname_len = strlen(funcname);

    /* get a list of matching functions */
    catlist = SearchSysCacheList1(PROCNAMEARGSNSP, CStringGetDatum(funcname));

    if (catlist->n_members == 0)
    {
        ReleaseSysCacheList(catlist);
        return NULL;
    }

    nargs = list_length(fn->args);

    /* iterate through them and verify that they are in the search path */
    for (i = 0; i < catlist->n_members; i++)
    {
        HeapTuple proctup = &catlist->members[i]->tuple;
        procform = (Form_pg_proc) GETSTRUCT(proctup);

        /*
         * Check if the function name, number of arguments, and
         * variadic match before checking if it is in the search
         * path.
         */
        if (nargs == procform->pronargs &&
            fn->func_variadic == procform->provariadic &&
            strnlen(NameStr(procform->proname), NAMEDATALEN) == funcname_len &&
            pg_strcasecmp(funcname, procform->proname.data) == 0)
        {
            if (!asp_fetched)
            {
                asp = fetch_search_path(false);
                asp_fetched = true;
            }

            if (list_member_oid(asp, procform->pronamespace) &&
                !isTempNamespace(procform->pronamespace))
            {
                found = true;
            }
        }

        if (found)
        {
            Size procform_size;

            procform_size = offsetof(FormData_pg_proc, proargtypes.values) +
                            sizeof(Oid) * procform->pronargs;
            result = palloc(procform_size);
            memcpy(result, procform, procform_size);
            break;
        }

        /* reset procform */
        procform = NULL;
    }

    /* Error out if function not found */
    if (err_not_found && (result == NULL))
    {
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_FUNCTION),
                 errmsg("function %s does not exist", funcname),
                 errhint("If the function is from an external extension, "
                         "make sure the extension is installed and the "
                         "function is in the search path.")));
    }

    /* we need to release the cache list */
    ReleaseSysCacheList(catlist);
    list_free(asp);

    return result;
}

static const char *get_mapped_extension(Oid func_oid)
{
    Oid extension_oid;
    char *extension = NULL;
    function_extension_cache_entry *entry;
    bool found;

    initialize_function_extension_cache();

    entry = hash_search(function_extension_cache, &func_oid, HASH_FIND, NULL);
    if (entry != NULL)
    {
        if (!entry->has_extension)
        {
            return NULL;
        }
        return NameStr(entry->extension);
    }

    extension_oid = getExtensionOfObject(ProcedureRelationId, func_oid);
    extension = get_extension_name(extension_oid);

    if (function_extension_cache == NULL)
    {
        initialize_function_extension_cache();
    }

    entry = hash_search(function_extension_cache, &func_oid, HASH_ENTER,
                        &found);
    if (!found)
    {
        entry->has_extension = (extension != NULL);
        if (entry->has_extension)
        {
            namestrcpy(&entry->extension, extension);
        }
    }

    if (extension != NULL)
    {
        pfree(extension);
    }

    return entry->has_extension ? NameStr(entry->extension) : NULL;
}

static bool function_belongs_to_extension(Oid func_oid, const char *extension)
{
    Oid extension_oid;
    char *extension_name = NULL;
    function_extension_cache_entry *entry;
    bool belongs;
    bool found;

    initialize_function_extension_cache();

    entry = hash_search(function_extension_cache, &func_oid, HASH_FIND, NULL);
    if (entry != NULL)
    {
        return entry->has_extension &&
               pg_strcasecmp(NameStr(entry->extension), extension) == 0;
    }

    extension_oid = getExtensionOfObject(ProcedureRelationId, func_oid);
    extension_name = get_extension_name(extension_oid);

    if (function_extension_cache == NULL)
    {
        initialize_function_extension_cache();
    }

    entry = hash_search(function_extension_cache, &func_oid, HASH_ENTER,
                        &found);
    if (!found)
    {
        entry->has_extension = (extension_name != NULL);
        if (entry->has_extension)
        {
            namestrcpy(&entry->extension, extension_name);
        }
    }

    belongs = (extension_name != NULL &&
               pg_strcasecmp(extension_name, extension) == 0);

    if (extension_name != NULL)
    {
        pfree(extension_name);
    }

    return belongs;
}

static bool is_extension_external(const char *extension)
{
    return ((extension != NULL) &&
            (pg_strcasecmp(extension, "age") != 0));
}

static bool function_needs_graph_name_argument(const char *name, int name_len)
{
    switch (name[0])
    {
        case 'e':
            return name_len == 7 && memcmp(name, "endNode", 7) == 0;
        case 's':
            return name_len == 9 && memcmp(name, "startNode", 9) == 0;
        case 'v':
            return (name_len == 3 && memcmp(name, "vle", 3) == 0) ||
                   (name_len == 12 &&
                    memcmp(name, "vle_internal", 12) == 0) ||
                   (name_len == 12 &&
                    memcmp(name, "vertex_stats", 12) == 0);
        default:
            return false;
    }
}

/* Returns age_ prefiexed lower case function name */
static char *construct_age_function_name(char *funcname, int funcname_len)
{
    char *ag_name = palloc(funcname_len + 5);
    int i;

    /* copy in the prefix - all AGE functions are prefixed with age_ */
    memcpy(ag_name, "age_", 4);

    /*
     * All AGE function names are in lower case. So, copy in the funcname
     * in lower case.
     */
    for (i = 0; i < funcname_len; i++)
    {
        ag_name[i + 4] = tolower(funcname[i]);
    }

    /* terminate it with 0 */
    ag_name[i + 4] = 0;

    return ag_name;
}


/*
 * Checks if a function exists. If the extension name is given,
 * then it checks if the function exists in that extension.
 */
static bool function_exists(char *funcname, char *extension)
{
    CatCList *catlist = NULL;
    bool found = false;
    function_exists_cache_key key;
    function_exists_cache_entry *entry;
    bool cache_found;
    int i = 0;

    initialize_function_exists_cache();
    MemSet(&key, 0, sizeof(key));
    namestrcpy(&key.funcname, funcname);
    if (extension != NULL)
    {
        namestrcpy(&key.extension, extension);
    }

    entry = hash_search(function_exists_cache, &key, HASH_FIND, NULL);
    if (entry != NULL)
    {
        return entry->exists;
    }

    /* get a list of matching functions */
    catlist = SearchSysCacheList1(PROCNAMEARGSNSP, CStringGetDatum(funcname));

    if (catlist->n_members == 0)
    {
        ReleaseSysCacheList(catlist);
        entry = hash_search(function_exists_cache, &key, HASH_ENTER,
                            &cache_found);
        entry->exists = false;
        return false;
    }
    else if (extension == NULL)
    {
        ReleaseSysCacheList(catlist);
        entry = hash_search(function_exists_cache, &key, HASH_ENTER,
                            &cache_found);
        entry->exists = true;
        return true;
    }

    for (i = 0; i < catlist->n_members; i++)
    {
        HeapTuple proctup = &catlist->members[i]->tuple;
        Form_pg_proc procform = (Form_pg_proc) GETSTRUCT(proctup);

        if (function_belongs_to_extension(procform->oid, extension))
        {
            found = true;
            break;
        }
    }

    /* we need to release the cache list */
    ReleaseSysCacheList(catlist);

    entry = hash_search(function_exists_cache, &key, HASH_ENTER,
                        &cache_found);
    entry->exists = found;

    return found;
}

static void initialize_function_exists_cache(void)
{
    HASHCTL hash_ctl;

    if (function_exists_cache != NULL)
    {
        return;
    }

    initialize_function_caches();

    MemSet(&hash_ctl, 0, sizeof(hash_ctl));
    hash_ctl.keysize = sizeof(function_exists_cache_key);
    hash_ctl.entrysize = sizeof(function_exists_cache_entry);
    hash_ctl.hcxt = CacheMemoryContext;

    function_exists_cache = hash_create("cypher function existence cache", 16,
                                        &hash_ctl,
                                        HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
}

static void initialize_function_extension_cache(void)
{
    HASHCTL hash_ctl;

    if (function_extension_cache != NULL)
    {
        return;
    }

    initialize_function_caches();

    MemSet(&hash_ctl, 0, sizeof(hash_ctl));
    hash_ctl.keysize = sizeof(Oid);
    hash_ctl.entrysize = sizeof(function_extension_cache_entry);
    hash_ctl.hcxt = CacheMemoryContext;

    function_extension_cache =
        hash_create("cypher function extension cache", 16, &hash_ctl,
                    HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
}

static void initialize_function_caches(void)
{
    if (!CacheMemoryContext)
    {
        CreateCacheMemoryContext();
    }

    if (!function_cache_callback_registered)
    {
        CacheRegisterSyscacheCallback(PROCOID, invalidate_function_caches,
                                      (Datum)0);
        CacheRegisterSyscacheCallback(PROCNAMEARGSNSP,
                                      invalidate_function_caches,
                                      (Datum)0);
        function_cache_callback_registered = true;
    }
}

static void invalidate_function_caches(Datum arg, int cache_id,
                                       uint32 hash_value)
{
    if (function_exists_cache != NULL)
    {
        hash_destroy(function_exists_cache);
        function_exists_cache = NULL;
    }

    if (function_extension_cache != NULL)
    {
        hash_destroy(function_extension_cache);
        function_extension_cache = NULL;
    }

    agtype_access_operator_oid = InvalidOid;
    agtype_object_field_agtype_oid = InvalidOid;
    agtype_object_field_int8_oid = InvalidOid;
    agtype_object_field_float8_oid = InvalidOid;
    agtype_object_field_text_agtype_oid = InvalidOid;
    agtype_object_field_numeric_agtype_oid = InvalidOid;
    agtype_object_field_numeric_oid = InvalidOid;
    agtype_access_slice_oid = InvalidOid;
    agtype_in_operator_oid = InvalidOid;
    agtype_add_oid = InvalidOid;
    agtype_build_empty_list_oid = InvalidOid;
    agtype_build_list_oid = InvalidOid;
    agtype_build_empty_map_oid = InvalidOid;
    agtype_build_map_oid = InvalidOid;
    agtype_build_map_nonull_oid = InvalidOid;
    age_properties_oid = InvalidOid;
    age_keys_oid = InvalidOid;
    build_vertex_label_oid = InvalidOid;
    build_edge_label_oid = InvalidOid;
    agtype_string_match_starts_with_oid = InvalidOid;
    agtype_string_match_ends_with_oid = InvalidOid;
    agtype_string_match_contains_oid = InvalidOid;
    text_to_agtype_oid = InvalidOid;
    label_name_oid = InvalidOid;
    age_vle_path_length_oid = InvalidOid;
    age_vle_path_node_count_oid = InvalidOid;
    age_vle_edge_tail_count_oid = InvalidOid;
    age_vle_list_is_empty_oid = InvalidOid;
    age_vle_list_slice_count_oid = InvalidOid;
    age_vle_list_slice_is_empty_oid = InvalidOid;
    age_materialize_vle_list_slice_oid = InvalidOid;
    age_materialize_vle_slice_boundary_oid = InvalidOid;
    age_materialize_vle_edges_oid = InvalidOid;
    age_materialize_vle_nodes_oid = InvalidOid;
    age_materialize_vle_node_at_oid = InvalidOid;
    age_materialize_vle_node_tail_last_oid = InvalidOid;
    age_vle_node_tail_last_id_oid = InvalidOid;
    age_vle_node_id_at_oid = InvalidOid;
    age_vle_node_label_at_oid = InvalidOid;
    age_vle_node_labels_at_oid = InvalidOid;
    age_vle_node_properties_at_oid = InvalidOid;
    age_vle_node_property_at_oid = InvalidOid;
    age_materialize_vle_nodes_slice_oid = InvalidOid;
    age_materialize_vle_nodes_tail_oid = InvalidOid;
    age_materialize_vle_nodes_reversed_oid = InvalidOid;
    age_materialize_vle_edge_at_oid = InvalidOid;
    age_materialize_vle_edge_reversed_at_oid = InvalidOid;
    age_materialize_vle_edge_tail_last_oid = InvalidOid;
    age_vle_edge_tail_last_id_oid = InvalidOid;
    age_vle_tail_last_field_oid = InvalidOid;
    age_vle_tail_last_edge_endpoint_oid = InvalidOid;
    age_vle_tail_last_endpoint_field_oid = InvalidOid;
    age_vle_edge_id_at_oid = InvalidOid;
    age_vle_edge_index_exists_oid = InvalidOid;
    age_vle_edge_indices_equal_oid = InvalidOid;
    age_vle_edge_reversed_index_equal_oid = InvalidOid;
    age_vle_edge_label_at_oid = InvalidOid;
    age_vle_edge_properties_at_oid = InvalidOid;
    age_vle_edge_property_at_oid = InvalidOid;
    age_vle_edge_start_node_at_oid = InvalidOid;
    age_vle_edge_end_node_at_oid = InvalidOid;
    age_vle_edge_endpoint_field_at_oid = InvalidOid;
    age_vle_edge_start_id_at_oid = InvalidOid;
    age_vle_edge_end_id_at_oid = InvalidOid;
    age_materialize_vle_edges_tail_oid = InvalidOid;
    age_materialize_vle_edges_reversed_oid = InvalidOid;
    age_vle_terminal_vertex_oid = InvalidOid;
    age_vle_terminal_vertex_properties_oid = InvalidOid;
    age_vle_terminal_vertex_property_oid = InvalidOid;
    age_vle_terminal_vertex_property_from_path_oid = InvalidOid;
    cypher_vle_agg_invalidate_oids();
}

static Oid get_agtype_access_operator_oid(void)
{
    initialize_function_caches();

    if (!OidIsValid(agtype_access_operator_oid))
    {
        agtype_access_operator_oid =
            get_ag_func_oid("agtype_access_operator", 1, AGTYPEARRAYOID);
    }

    return agtype_access_operator_oid;
}

static Oid get_agtype_object_field_agtype_oid(void)
{
    initialize_function_caches();

    if (!OidIsValid(agtype_object_field_agtype_oid))
    {
        agtype_object_field_agtype_oid =
            get_ag_func_oid("agtype_object_field_agtype", 2,
                            AGTYPEOID, AGTYPEOID);
    }

    return agtype_object_field_agtype_oid;
}

static Oid get_agtype_object_field_int8_oid(void)
{
    initialize_function_caches();

    if (!OidIsValid(agtype_object_field_int8_oid))
    {
        agtype_object_field_int8_oid =
            get_ag_func_oid("agtype_object_field_int8", 2,
                            AGTYPEOID, AGTYPEOID);
    }

    return agtype_object_field_int8_oid;
}

static Oid get_agtype_object_field_float8_oid(void)
{
    initialize_function_caches();

    if (!OidIsValid(agtype_object_field_float8_oid))
    {
        agtype_object_field_float8_oid =
            get_ag_func_oid("agtype_object_field_float8", 2,
                            AGTYPEOID, AGTYPEOID);
    }

    return agtype_object_field_float8_oid;
}

static Oid get_agtype_object_field_text_agtype_oid(void)
{
    initialize_function_caches();

    if (!OidIsValid(agtype_object_field_text_agtype_oid))
    {
        agtype_object_field_text_agtype_oid =
            get_ag_func_oid("agtype_object_field_text_agtype", 2,
                            AGTYPEOID, AGTYPEOID);
    }

    return agtype_object_field_text_agtype_oid;
}

static Oid get_agtype_object_field_numeric_agtype_oid(void)
{
    initialize_function_caches();

    if (!OidIsValid(agtype_object_field_numeric_agtype_oid))
    {
        agtype_object_field_numeric_agtype_oid =
            get_ag_func_oid("agtype_object_field_numeric_agtype", 2,
                            AGTYPEOID, AGTYPEOID);
    }

    return agtype_object_field_numeric_agtype_oid;
}

static Oid get_agtype_object_field_numeric_oid(void)
{
    if (!OidIsValid(agtype_object_field_numeric_oid))
    {
        agtype_object_field_numeric_oid =
            get_ag_func_oid("agtype_object_field_numeric", 2,
                            AGTYPEOID, AGTYPEOID);
    }

    return agtype_object_field_numeric_oid;
}

static bool try_rewrite_collect_property_access(Aggref *aggref)
{
    return false;
}

static Node *try_rewrite_pg_scalar_property_access(FuncExpr *func_expr,
                                                   Oid helper_oid,
                                                   Oid result_type)
{
    Node *arg;
    Node *properties;
    Node *key;
    FuncExpr *helper;

    if (func_expr == NULL ||
        !OidIsValid(helper_oid) ||
        !OidIsValid(result_type) ||
        list_length(func_expr->args) != 1)
    {
        return NULL;
    }

    arg = linitial(func_expr->args);
    if (!cypher_extract_property_access_terminal_args(arg, &properties, &key))
        return NULL;

    helper = makeFuncExpr(helper_oid, result_type,
                          list_make2(copyObject(properties), copyObject(key)),
                          InvalidOid, InvalidOid, COERCE_EXPLICIT_CALL);
    helper->location = func_expr->location;

    return (Node *)helper;
}

static Oid get_agtype_access_slice_oid(void)
{
    initialize_function_caches();

    if (!OidIsValid(agtype_access_slice_oid))
    {
        agtype_access_slice_oid =
            get_ag_func_oid("agtype_access_slice", 3, AGTYPEOID, AGTYPEOID,
                            AGTYPEOID);
    }

    return agtype_access_slice_oid;
}

static Oid get_agtype_in_operator_oid(void)
{
    initialize_function_caches();

    if (!OidIsValid(agtype_in_operator_oid))
    {
        agtype_in_operator_oid =
            get_ag_func_oid("agtype_in_operator", 2, AGTYPEOID, AGTYPEOID);
    }

    return agtype_in_operator_oid;
}

static Oid get_agtype_add_oid(void)
{
    initialize_function_caches();

    if (!OidIsValid(agtype_add_oid))
    {
        agtype_add_oid =
            get_ag_func_oid("agtype_add", 2, AGTYPEOID, AGTYPEOID);
    }

    return agtype_add_oid;
}

static Oid get_agtype_build_empty_list_oid(void)
{
    initialize_function_caches();

    if (!OidIsValid(agtype_build_empty_list_oid))
    {
        agtype_build_empty_list_oid =
            get_ag_func_oid("agtype_build_list", 0);
    }

    return agtype_build_empty_list_oid;
}

static Oid get_agtype_build_list_oid(void)
{
    initialize_function_caches();

    if (!OidIsValid(agtype_build_list_oid))
    {
        agtype_build_list_oid =
            get_ag_func_oid("agtype_build_list", 1, ANYOID);
    }

    return agtype_build_list_oid;
}

static Oid get_agtype_build_empty_map_oid(void)
{
    initialize_function_caches();

    if (!OidIsValid(agtype_build_empty_map_oid))
    {
        agtype_build_empty_map_oid = get_ag_func_oid("agtype_build_map", 0);
    }

    return agtype_build_empty_map_oid;
}

static Oid get_agtype_build_map_oid(void)
{
    initialize_function_caches();

    if (!OidIsValid(agtype_build_map_oid))
    {
        agtype_build_map_oid =
            get_ag_func_oid("agtype_build_map", 1, ANYOID);
    }

    return agtype_build_map_oid;
}

static Oid get_agtype_build_map_nonull_oid(void)
{
    initialize_function_caches();

    if (!OidIsValid(agtype_build_map_nonull_oid))
    {
        agtype_build_map_nonull_oid =
            get_ag_func_oid("agtype_build_map_nonull", 1, ANYOID);
    }

    return agtype_build_map_nonull_oid;
}

static Oid get_age_properties_oid(void)
{
    initialize_function_caches();

    if (!OidIsValid(age_properties_oid))
    {
        age_properties_oid = get_ag_func_oid("age_properties", 1, AGTYPEOID);
    }

    return age_properties_oid;
}

static Oid get_age_keys_oid(void)
{
    const AgeBuiltinFuncMeta *meta;

    initialize_function_caches();

    if (!OidIsValid(age_keys_oid))
    {
        meta = get_age_builtin_func_meta_by_name("keys");
        Assert(meta != NULL);
        age_keys_oid = get_age_builtin_func_oid(meta);
    }

    return age_keys_oid;
}

static Oid get_build_edge_label_oid(void)
{
    initialize_function_caches();

    if (!OidIsValid(build_edge_label_oid))
    {
        build_edge_label_oid =
            get_ag_func_oid("_agtype_build_edge_label", 5, OIDOID, GRAPHIDOID,
                            GRAPHIDOID, GRAPHIDOID, AGTYPEOID);
    }

    return build_edge_label_oid;
}

static Oid get_build_vertex_label_oid(void)
{
    initialize_function_caches();

    if (!OidIsValid(build_vertex_label_oid))
    {
        build_vertex_label_oid =
            get_ag_func_oid("_agtype_build_vertex_label", 3, OIDOID,
                            GRAPHIDOID, AGTYPEOID);
    }

    return build_vertex_label_oid;
}

static Oid get_agtype_string_match_starts_with_oid(void)
{
    initialize_function_caches();

    if (!OidIsValid(agtype_string_match_starts_with_oid))
    {
        agtype_string_match_starts_with_oid =
            get_ag_func_oid("agtype_string_match_starts_with", 2, AGTYPEOID,
                            AGTYPEOID);
    }

    return agtype_string_match_starts_with_oid;
}

static Oid get_agtype_string_match_ends_with_oid(void)
{
    initialize_function_caches();

    if (!OidIsValid(agtype_string_match_ends_with_oid))
    {
        agtype_string_match_ends_with_oid =
            get_ag_func_oid("agtype_string_match_ends_with", 2, AGTYPEOID,
                            AGTYPEOID);
    }

    return agtype_string_match_ends_with_oid;
}

static Oid get_agtype_string_match_contains_oid(void)
{
    initialize_function_caches();

    if (!OidIsValid(agtype_string_match_contains_oid))
    {
        agtype_string_match_contains_oid =
            get_ag_func_oid("agtype_string_match_contains", 2, AGTYPEOID,
                            AGTYPEOID);
    }

    return agtype_string_match_contains_oid;
}

static Oid get_text_to_agtype_oid(void)
{
    initialize_function_caches();

    if (!OidIsValid(text_to_agtype_oid))
    {
        text_to_agtype_oid = get_ag_func_oid("text_to_agtype", 1, TEXTOID);
    }

    return text_to_agtype_oid;
}

static Oid get_label_name_oid(void)
{
    initialize_function_caches();

    if (!OidIsValid(label_name_oid))
    {
        label_name_oid = get_ag_func_oid("_label_name", 2, OIDOID,
                                         GRAPHIDOID);
    }

    return label_name_oid;
}

static Oid get_age_vle_path_length_oid(void)
{
    initialize_function_caches();

    if (!OidIsValid(age_vle_path_length_oid))
    {
        age_vle_path_length_oid =
            get_age_builtin_func_oid_by_name("age_vle_path_length");
    }

    return age_vle_path_length_oid;
}

static Oid get_age_vle_path_node_count_oid(void)
{
    initialize_function_caches();

    if (!OidIsValid(age_vle_path_node_count_oid))
    {
        age_vle_path_node_count_oid =
            get_age_builtin_func_oid_by_name("age_vle_path_node_count");
    }

    return age_vle_path_node_count_oid;
}

static Oid get_age_vle_edge_tail_count_oid(void)
{
    initialize_function_caches();

    if (!OidIsValid(age_vle_edge_tail_count_oid))
    {
        age_vle_edge_tail_count_oid =
            get_age_builtin_func_oid_by_name("age_vle_edge_tail_count");
    }

    return age_vle_edge_tail_count_oid;
}

static Oid get_age_vle_list_is_empty_oid(void)
{
    initialize_function_caches();

    if (!OidIsValid(age_vle_list_is_empty_oid))
    {
        age_vle_list_is_empty_oid =
            get_ag_func_oid("age_vle_list_is_empty", 2, AGTYPEOID,
                            AGTYPEOID);
    }

    return age_vle_list_is_empty_oid;
}

static Oid get_age_vle_list_slice_count_oid(void)
{
    initialize_function_caches();

    if (!OidIsValid(age_vle_list_slice_count_oid))
    {
        age_vle_list_slice_count_oid =
            get_ag_func_oid("age_vle_list_slice_count", 4, AGTYPEOID,
                            AGTYPEOID, AGTYPEOID, AGTYPEOID);
    }

    return age_vle_list_slice_count_oid;
}

static Oid get_age_vle_list_slice_is_empty_oid(void)
{
    initialize_function_caches();

    if (!OidIsValid(age_vle_list_slice_is_empty_oid))
    {
        age_vle_list_slice_is_empty_oid =
            get_ag_func_oid("age_vle_list_slice_is_empty", 4, AGTYPEOID,
                            AGTYPEOID, AGTYPEOID, AGTYPEOID);
    }

    return age_vle_list_slice_is_empty_oid;
}

static Oid get_age_materialize_vle_list_slice_oid(void)
{
    initialize_function_caches();

    if (!OidIsValid(age_materialize_vle_list_slice_oid))
    {
        age_materialize_vle_list_slice_oid =
            get_age_builtin_func_oid_by_name("age_materialize_vle_list_slice");
    }

    return age_materialize_vle_list_slice_oid;
}

static Oid get_age_materialize_vle_slice_boundary_oid(void)
{
    initialize_function_caches();

    if (!OidIsValid(age_materialize_vle_slice_boundary_oid))
    {
        age_materialize_vle_slice_boundary_oid =
            get_age_builtin_func_oid_by_name(
                "age_materialize_vle_slice_boundary");
    }

    return age_materialize_vle_slice_boundary_oid;
}

static Oid get_age_materialize_vle_edges_oid(void)
{
    initialize_function_caches();

    if (!OidIsValid(age_materialize_vle_edges_oid))
    {
        age_materialize_vle_edges_oid =
            get_age_builtin_func_oid_by_name("age_materialize_vle_edges");
    }

    return age_materialize_vle_edges_oid;
}

static Oid get_age_materialize_vle_nodes_oid(void)
{
    initialize_function_caches();

    if (!OidIsValid(age_materialize_vle_nodes_oid))
    {
        age_materialize_vle_nodes_oid =
            get_age_builtin_func_oid_by_name("age_materialize_vle_nodes");
    }

    return age_materialize_vle_nodes_oid;
}

static Oid get_age_materialize_vle_node_at_oid(void)
{
    initialize_function_caches();

    if (!OidIsValid(age_materialize_vle_node_at_oid))
    {
        age_materialize_vle_node_at_oid =
            get_age_builtin_func_oid_by_name("age_materialize_vle_node_at");
    }

    return age_materialize_vle_node_at_oid;
}

static Oid get_age_materialize_vle_node_tail_last_oid(void)
{
    initialize_function_caches();

    if (!OidIsValid(age_materialize_vle_node_tail_last_oid))
    {
        age_materialize_vle_node_tail_last_oid =
            get_age_builtin_func_oid_by_name(
                "age_materialize_vle_node_tail_last");
    }

    return age_materialize_vle_node_tail_last_oid;
}

static Oid get_age_vle_node_tail_last_id_oid(void)
{
    initialize_function_caches();

    if (!OidIsValid(age_vle_node_tail_last_id_oid))
    {
        age_vle_node_tail_last_id_oid =
            get_ag_func_oid("age_vle_node_tail_last_id", 1, AGTYPEOID);
    }

    return age_vle_node_tail_last_id_oid;
}

static Oid get_age_vle_node_id_at_oid(void)
{
    initialize_function_caches();

    if (!OidIsValid(age_vle_node_id_at_oid))
    {
        age_vle_node_id_at_oid =
            get_ag_func_oid("age_vle_node_id_at", 2, AGTYPEOID, AGTYPEOID);
    }

    return age_vle_node_id_at_oid;
}

static Oid get_age_vle_node_label_at_oid(void)
{
    initialize_function_caches();

    if (!OidIsValid(age_vle_node_label_at_oid))
    {
        age_vle_node_label_at_oid =
            get_ag_func_oid("age_vle_node_label_at", 2, AGTYPEOID,
                            AGTYPEOID);
    }

    return age_vle_node_label_at_oid;
}

static Oid get_age_vle_node_labels_at_oid(void)
{
    initialize_function_caches();

    if (!OidIsValid(age_vle_node_labels_at_oid))
    {
        age_vle_node_labels_at_oid =
            get_ag_func_oid("age_vle_node_labels_at", 2, AGTYPEOID,
                            AGTYPEOID);
    }

    return age_vle_node_labels_at_oid;
}

static Oid get_age_vle_node_properties_at_oid(void)
{
    initialize_function_caches();

    if (!OidIsValid(age_vle_node_properties_at_oid))
    {
        age_vle_node_properties_at_oid =
            get_ag_func_oid("age_vle_node_properties_at", 2, AGTYPEOID,
                            AGTYPEOID);
    }

    return age_vle_node_properties_at_oid;
}

static Oid get_age_vle_node_property_at_oid(void)
{
    initialize_function_caches();

    if (!OidIsValid(age_vle_node_property_at_oid))
    {
        age_vle_node_property_at_oid =
            get_ag_func_oid("age_vle_node_property_at", 3, AGTYPEOID,
                            AGTYPEOID, AGTYPEOID);
    }

    return age_vle_node_property_at_oid;
}

static Oid get_age_materialize_vle_nodes_slice_oid(void)
{
    initialize_function_caches();

    if (!OidIsValid(age_materialize_vle_nodes_slice_oid))
    {
        age_materialize_vle_nodes_slice_oid =
            get_age_builtin_func_oid_by_name("age_materialize_vle_nodes_slice");
    }

    return age_materialize_vle_nodes_slice_oid;
}

static Oid get_age_materialize_vle_nodes_tail_oid(void)
{
    initialize_function_caches();

    if (!OidIsValid(age_materialize_vle_nodes_tail_oid))
    {
        age_materialize_vle_nodes_tail_oid =
            get_age_builtin_func_oid_by_name("age_materialize_vle_nodes_tail");
    }

    return age_materialize_vle_nodes_tail_oid;
}

static Oid get_age_materialize_vle_nodes_reversed_oid(void)
{
    initialize_function_caches();

    if (!OidIsValid(age_materialize_vle_nodes_reversed_oid))
    {
        age_materialize_vle_nodes_reversed_oid =
            get_age_builtin_func_oid_by_name(
                "age_materialize_vle_nodes_reversed");
    }

    return age_materialize_vle_nodes_reversed_oid;
}

static Oid get_age_materialize_vle_edge_at_oid(void)
{
    initialize_function_caches();

    if (!OidIsValid(age_materialize_vle_edge_at_oid))
    {
        age_materialize_vle_edge_at_oid =
            get_age_builtin_func_oid_by_name("age_materialize_vle_edge_at");
    }

    return age_materialize_vle_edge_at_oid;
}

static Oid get_age_materialize_vle_edge_reversed_at_oid(void)
{
    initialize_function_caches();

    if (!OidIsValid(age_materialize_vle_edge_reversed_at_oid))
    {
        age_materialize_vle_edge_reversed_at_oid =
            get_age_builtin_func_oid_by_name(
                "age_materialize_vle_edge_reversed_at");
    }

    return age_materialize_vle_edge_reversed_at_oid;
}

static Oid get_age_materialize_vle_edge_tail_last_oid(void)
{
    initialize_function_caches();

    if (!OidIsValid(age_materialize_vle_edge_tail_last_oid))
    {
        age_materialize_vle_edge_tail_last_oid =
            get_age_builtin_func_oid_by_name(
                "age_materialize_vle_edge_tail_last");
    }

    return age_materialize_vle_edge_tail_last_oid;
}

static Oid get_age_vle_edge_tail_last_id_oid(void)
{
    initialize_function_caches();

    if (!OidIsValid(age_vle_edge_tail_last_id_oid))
    {
        age_vle_edge_tail_last_id_oid =
            get_ag_func_oid("age_vle_edge_tail_last_id", 1, AGTYPEOID);
    }

    return age_vle_edge_tail_last_id_oid;
}

static Oid get_age_vle_tail_last_field_oid(void)
{
    initialize_function_caches();

    if (!OidIsValid(age_vle_tail_last_field_oid))
    {
        age_vle_tail_last_field_oid =
            get_ag_func_oid("age_vle_tail_last_field", 2, AGTYPEOID,
                            AGTYPEOID);
    }

    return age_vle_tail_last_field_oid;
}

static Oid get_age_vle_tail_last_edge_endpoint_oid(void)
{
    initialize_function_caches();

    if (!OidIsValid(age_vle_tail_last_edge_endpoint_oid))
    {
        age_vle_tail_last_edge_endpoint_oid =
            get_ag_func_oid("age_vle_tail_last_edge_endpoint", 2,
                            AGTYPEOID, AGTYPEOID);
    }

    return age_vle_tail_last_edge_endpoint_oid;
}

static Oid get_age_vle_tail_last_endpoint_field_oid(void)
{
    initialize_function_caches();

    if (!OidIsValid(age_vle_tail_last_endpoint_field_oid))
    {
        age_vle_tail_last_endpoint_field_oid =
            get_ag_func_oid("age_vle_tail_last_endpoint_field", 2,
                            AGTYPEOID, AGTYPEOID);
    }

    return age_vle_tail_last_endpoint_field_oid;
}

static Oid get_age_vle_edge_id_at_oid(void)
{
    initialize_function_caches();

    if (!OidIsValid(age_vle_edge_id_at_oid))
    {
        age_vle_edge_id_at_oid =
            get_ag_func_oid("age_vle_edge_id_at", 2, AGTYPEOID, AGTYPEOID);
    }

    return age_vle_edge_id_at_oid;
}

static Oid get_age_vle_edge_index_exists_oid(void)
{
    initialize_function_caches();

    if (!OidIsValid(age_vle_edge_index_exists_oid))
    {
        age_vle_edge_index_exists_oid =
            get_ag_func_oid("age_vle_edge_index_exists", 2, AGTYPEOID,
                            AGTYPEOID);
    }

    return age_vle_edge_index_exists_oid;
}

static Oid get_age_vle_edge_indices_equal_oid(void)
{
    initialize_function_caches();

    if (!OidIsValid(age_vle_edge_indices_equal_oid))
    {
        age_vle_edge_indices_equal_oid =
            get_ag_func_oid("age_vle_edge_indices_equal", 3, AGTYPEOID,
                            AGTYPEOID, AGTYPEOID);
    }

    return age_vle_edge_indices_equal_oid;
}

static Oid get_age_vle_edge_reversed_index_equal_oid(void)
{
    initialize_function_caches();

    if (!OidIsValid(age_vle_edge_reversed_index_equal_oid))
    {
        age_vle_edge_reversed_index_equal_oid =
            get_ag_func_oid("age_vle_edge_reversed_index_equal", 3,
                            AGTYPEOID, AGTYPEOID, AGTYPEOID);
    }

    return age_vle_edge_reversed_index_equal_oid;
}

static Oid get_age_vle_edge_label_at_oid(void)
{
    initialize_function_caches();

    if (!OidIsValid(age_vle_edge_label_at_oid))
    {
        age_vle_edge_label_at_oid =
            get_ag_func_oid("age_vle_edge_label_at", 2, AGTYPEOID,
                            AGTYPEOID);
    }

    return age_vle_edge_label_at_oid;
}

static Oid get_age_vle_edge_properties_at_oid(void)
{
    initialize_function_caches();

    if (!OidIsValid(age_vle_edge_properties_at_oid))
    {
        age_vle_edge_properties_at_oid =
            get_ag_func_oid("age_vle_edge_properties_at", 2, AGTYPEOID,
                            AGTYPEOID);
    }

    return age_vle_edge_properties_at_oid;
}

static Oid get_age_vle_edge_property_at_oid(void)
{
    initialize_function_caches();

    if (!OidIsValid(age_vle_edge_property_at_oid))
    {
        age_vle_edge_property_at_oid =
            get_ag_func_oid("age_vle_edge_property_at", 3, AGTYPEOID,
                            AGTYPEOID, AGTYPEOID);
    }

    return age_vle_edge_property_at_oid;
}

static Oid get_age_vle_edge_start_node_at_oid(void)
{
    initialize_function_caches();

    if (!OidIsValid(age_vle_edge_start_node_at_oid))
    {
        age_vle_edge_start_node_at_oid =
            get_ag_func_oid("age_vle_edge_start_node_at", 2, AGTYPEOID,
                            AGTYPEOID);
    }

    return age_vle_edge_start_node_at_oid;
}

static Oid get_age_vle_edge_end_node_at_oid(void)
{
    initialize_function_caches();

    if (!OidIsValid(age_vle_edge_end_node_at_oid))
    {
        age_vle_edge_end_node_at_oid =
            get_ag_func_oid("age_vle_edge_end_node_at", 2, AGTYPEOID,
                            AGTYPEOID);
    }

    return age_vle_edge_end_node_at_oid;
}

static Oid get_age_vle_edge_endpoint_field_at_oid(void)
{
    initialize_function_caches();

    if (!OidIsValid(age_vle_edge_endpoint_field_at_oid))
    {
        age_vle_edge_endpoint_field_at_oid =
            get_ag_func_oid("age_vle_edge_endpoint_field_at", 3,
                            AGTYPEOID, AGTYPEOID, AGTYPEOID);
    }

    return age_vle_edge_endpoint_field_at_oid;
}

static Oid get_age_vle_edge_start_id_at_oid(void)
{
    initialize_function_caches();

    if (!OidIsValid(age_vle_edge_start_id_at_oid))
    {
        age_vle_edge_start_id_at_oid =
            get_ag_func_oid("age_vle_edge_start_id_at", 2, AGTYPEOID,
                            AGTYPEOID);
    }

    return age_vle_edge_start_id_at_oid;
}

static Oid get_age_vle_edge_end_id_at_oid(void)
{
    initialize_function_caches();

    if (!OidIsValid(age_vle_edge_end_id_at_oid))
    {
        age_vle_edge_end_id_at_oid =
            get_ag_func_oid("age_vle_edge_end_id_at", 2, AGTYPEOID,
                            AGTYPEOID);
    }

    return age_vle_edge_end_id_at_oid;
}

static Oid get_age_materialize_vle_edges_tail_oid(void)
{
    initialize_function_caches();

    if (!OidIsValid(age_materialize_vle_edges_tail_oid))
    {
        age_materialize_vle_edges_tail_oid =
            get_age_builtin_func_oid_by_name("age_materialize_vle_edges_tail");
    }

    return age_materialize_vle_edges_tail_oid;
}

static Oid get_age_materialize_vle_edges_reversed_oid(void)
{
    initialize_function_caches();

    if (!OidIsValid(age_materialize_vle_edges_reversed_oid))
    {
        age_materialize_vle_edges_reversed_oid =
            get_age_builtin_func_oid_by_name(
                "age_materialize_vle_edges_reversed");
    }

    return age_materialize_vle_edges_reversed_oid;
}

static Oid get_age_vle_terminal_vertex_oid(void)
{
    initialize_function_caches();

    if (!OidIsValid(age_vle_terminal_vertex_oid))
    {
        age_vle_terminal_vertex_oid =
            get_ag_func_oid("age_vle_terminal_vertex", 1, AGTYPEOID);
    }

    return age_vle_terminal_vertex_oid;
}

static Oid get_age_vle_terminal_vertex_properties_oid(void)
{
    initialize_function_caches();

    if (!OidIsValid(age_vle_terminal_vertex_properties_oid))
    {
        age_vle_terminal_vertex_properties_oid =
            get_ag_func_oid("age_vle_terminal_vertex_properties", 1,
                            AGTYPEOID);
    }

    return age_vle_terminal_vertex_properties_oid;
}

static Oid get_age_vle_terminal_vertex_property_oid(void)
{
    initialize_function_caches();

    if (!OidIsValid(age_vle_terminal_vertex_property_oid))
    {
        age_vle_terminal_vertex_property_oid =
            get_ag_func_oid("age_vle_terminal_vertex_property", 2,
                            AGTYPEOID, AGTYPEOID);
    }

    return age_vle_terminal_vertex_property_oid;
}

static Oid get_age_vle_terminal_vertex_property_from_path_oid(void)
{
    initialize_function_caches();

    if (!OidIsValid(age_vle_terminal_vertex_property_from_path_oid))
    {
        age_vle_terminal_vertex_property_from_path_oid =
            get_ag_func_oid("age_vle_terminal_vertex_property_from_path", 2,
                            AGTYPEOID, AGTYPEOID);
    }

    return age_vle_terminal_vertex_property_from_path_oid;
}

static Expr *get_current_single_vle_path_expr_internal(
    cypher_parsestate *cpstate, Node *arg, bool require_fixed_one_hop)
{
    char *field_name = NULL;
    transform_entity *entity = NULL;
    cypher_path *path = NULL;
    cypher_relationship *rel = NULL;
    ListCell *lc = NULL;
    cypher_parsestate *search_cpstate = NULL;

    if (!parse_single_column_ref(arg, &field_name, NULL))
    {
        return NULL;
    }

    entity = find_variable(cpstate, field_name);
    if (entity == NULL || entity->type != ENT_PATH ||
        !entity->declared_in_current_clause)
    {
        return NULL;
    }

    path = entity->entity.path;
    if (path == NULL || list_length(path->path) != 3)
    {
        return NULL;
    }

    rel = (cypher_relationship *)lfirst(lnext(path->path,
                                             list_head(path->path)));
    if (rel == NULL ||
        (require_fixed_one_hop && !is_fixed_one_hop_vle_rel(rel)))
    {
        return NULL;
    }

    for (search_cpstate = cpstate; search_cpstate != NULL;
         search_cpstate =
             (cypher_parsestate *)search_cpstate->pstate.parentParseState)
    {
        foreach(lc, search_cpstate->entities)
        {
            transform_entity *vle_entity = lfirst(lc);

            if (vle_entity->type == ENT_VLE_EDGE &&
                vle_entity->entity.rel == rel &&
                vle_entity->expr != NULL &&
                exprType((Node *)vle_entity->expr) == AGTYPEOID)
            {
                return vle_entity->expr;
            }
        }
    }

    return NULL;
}

static Expr *get_current_any_single_vle_path_expr(cypher_parsestate *cpstate,
                                                  Node *arg)
{
    return get_current_single_vle_path_expr_internal(cpstate, arg, true);
}

static Expr *get_any_single_vle_path_expr(cypher_parsestate *cpstate,
                                          Node *arg)
{
    Expr *expr = get_current_any_single_vle_path_expr(cpstate, arg);

    if (expr != NULL)
    {
        return expr;
    }

    return get_visible_any_single_vle_path_expr(cpstate, arg);
}

static Expr *get_arbitrary_single_vle_path_expr(cypher_parsestate *cpstate,
                                                Node *arg)
{
    Expr *expr = get_current_single_vle_path_expr_internal(cpstate, arg,
                                                           false);

    if (expr != NULL)
    {
        return expr;
    }

    return get_visible_single_vle_path_expr_internal(cpstate, arg, false);
}

static bool is_fixed_one_hop_vle_rel(cypher_relationship *rel)
{
    FuncCall *fn = NULL;
    char *func_name = NULL;
    int64 lower;
    int64 upper;

    if (rel == NULL || rel->varlen == NULL || !IsA(rel->varlen, FuncCall))
    {
        return false;
    }

    fn = (FuncCall *)rel->varlen;
    if (!parse_single_func_name(fn, &func_name) ||
        pg_strcasecmp(func_name, "vle_internal") != 0 ||
        list_length(fn->args) < 5)
    {
        return false;
    }

    if (!parse_vle_bounds(fn, &lower, &upper))
    {
        return false;
    }

    return lower == 1 && upper == 1;
}

static Expr *get_current_vle_edge_expr_internal(cypher_parsestate *cpstate,
                                                Node *arg,
                                                bool require_fixed_one_hop)
{
    char *field_name = NULL;
    transform_entity *entity = NULL;

    if (!parse_single_column_ref(arg, &field_name, NULL))
    {
        return NULL;
    }

    entity = find_variable(cpstate, field_name);
    if (entity == NULL || entity->type != ENT_VLE_EDGE ||
        !entity->declared_in_current_clause || entity->expr == NULL ||
        exprType((Node *)entity->expr) != AGTYPEOID)
    {
        return NULL;
    }

    if (require_fixed_one_hop &&
        !is_fixed_one_hop_vle_rel((cypher_relationship *)entity->entity.rel))
    {
        return NULL;
    }

    return entity->expr;
}

static Expr *get_current_any_vle_edge_expr(cypher_parsestate *cpstate,
                                           Node *arg)
{
    return get_current_vle_edge_expr_internal(cpstate, arg, false);
}

static Expr *get_visible_single_vle_path_expr(cypher_parsestate *cpstate,
                                              Node *arg)
{
    return get_visible_single_vle_path_expr_internal(cpstate, arg, true);
}

static Expr *get_visible_any_single_vle_path_expr(cypher_parsestate *cpstate,
                                                  Node *arg)
{
    return get_visible_single_vle_path_expr_internal(cpstate, arg, true);
}

static Expr *get_visible_single_vle_path_expr_internal(
    cypher_parsestate *cpstate, Node *arg, bool require_fixed_one_hop)
{
    ParseState *pstate = (ParseState *)cpstate;
    char *field_name = NULL;
    int location = -1;
    transform_entity *entity = NULL;
    cypher_path *path = NULL;
    cypher_relationship *rel = NULL;
    char *raw_edges_name = NULL;
    Node *raw_edges = NULL;
    ListCell *lc = NULL;
    cypher_parsestate *search_cpstate = NULL;

    if (!parse_single_column_ref(arg, &field_name, &location))
    {
        return NULL;
    }

    entity = find_variable(cpstate, field_name);
    if (entity == NULL || entity->type != ENT_PATH ||
        entity->declared_in_current_clause ||
        entity->expr == NULL || !IsA(entity->expr, FuncExpr))
    {
        return NULL;
    }

    path = entity->entity.path;
    if (path == NULL || list_length(path->path) != 3)
    {
        return NULL;
    }

    rel = (cypher_relationship *)lfirst(lnext(path->path,
                                             list_head(path->path)));
    if (rel == NULL ||
        (require_fixed_one_hop && !is_fixed_one_hop_vle_rel(rel)))
    {
        return NULL;
    }

    raw_edges_name = make_raw_edges_name(field_name);
    raw_edges = colNameToVar(pstate, raw_edges_name, false, location);
    pfree(raw_edges_name);
    if (raw_edges != NULL)
    {
        return (Expr *)raw_edges;
    }

    if (!require_fixed_one_hop)
    {
        return NULL;
    }

    for (search_cpstate = cpstate; search_cpstate != NULL;
         search_cpstate =
             (cypher_parsestate *)search_cpstate->pstate.parentParseState)
    {
        foreach(lc, search_cpstate->entities)
        {
            transform_entity *vle_entity = lfirst(lc);

            if (vle_entity->type == ENT_VLE_EDGE &&
                vle_entity->entity.rel == rel &&
                vle_entity->expr != NULL &&
                exprType((Node *)vle_entity->expr) == AGTYPEOID)
            {
                return vle_entity->expr;
            }
        }
    }

    return NULL;
}

static Expr *get_visible_vle_edge_expr(cypher_parsestate *cpstate, Node *arg)
{
    ParseState *pstate = (ParseState *)cpstate;
    char *field_name = NULL;
    int location = -1;
    transform_entity *entity = NULL;
    char *raw_edges_name = NULL;
    Node *raw_edges = NULL;

    if (!parse_single_column_ref(arg, &field_name, &location))
    {
        return NULL;
    }

    entity = find_variable(cpstate, field_name);
    if (entity == NULL || entity->type != ENT_VLE_EDGE ||
        entity->declared_in_current_clause ||
        entity->expr == NULL)
    {
        return NULL;
    }

    raw_edges_name = make_raw_edges_name(field_name);
    raw_edges = colNameToVar(pstate, raw_edges_name, false, location);
    pfree(raw_edges_name);
    if (raw_edges != NULL)
    {
        return (Expr *)raw_edges;
    }

    if (!is_fixed_one_hop_vle_rel((cypher_relationship *)entity->entity.rel))
    {
        return NULL;
    }

    return entity->expr;
}

static Expr *get_any_vle_edge_expr(cypher_parsestate *cpstate, Node *arg)
{
    Expr *expr = get_current_any_vle_edge_expr(cpstate, arg);

    if (expr != NULL)
    {
        return expr;
    }

    return get_visible_vle_edge_expr(cpstate, arg);
}

static Expr *get_arbitrary_vle_relationship_list_expr(
    cypher_parsestate *cpstate, Node *arg)
{
    FuncCall *fn = NULL;

    if (!IsA(arg, FuncCall))
    {
        return get_current_any_vle_edge_expr(cpstate, arg);
    }

    fn = (FuncCall *)arg;
    if (!is_func_name_unary(fn, "relationships"))
    {
        return NULL;
    }

    return get_arbitrary_single_vle_path_expr(cpstate, linitial(fn->args));
}

static Node *try_transform_vle_path_length(cypher_parsestate *cpstate,
                                           FuncCall *fn)
{
    Node *path_arg = NULL;
    Expr *vle_expr = NULL;
    Oid func_oid;

    if (!parse_func_any_arg(fn, "length", &path_arg))
    {
        return NULL;
    }

    vle_expr = get_arbitrary_single_vle_path_expr(cpstate, path_arg);
    if (vle_expr == NULL)
    {
        return NULL;
    }

    func_oid = get_age_vle_path_length_oid();
    return make_vle_unary_agtype_expr(func_oid, (Node *)vle_expr);
}

static Node *try_transform_fixed_path_length(cypher_parsestate *cpstate,
                                             FuncCall *fn)
{
    ParseState *pstate = (ParseState *)cpstate;
    transform_entity *entity;
    cypher_path *path;
    ListCell *lc;
    int edge_count = 0;
    char *first_edge_name = NULL;
    char *path_name = NULL;
    Node *path_arg = NULL;
    Node *edge_id;

    if (!parse_func_arg(fn, "length", T_ColumnRef, &path_arg))
    {
        return NULL;
    }

    if (!parse_single_column_ref(path_arg, &path_name, NULL))
    {
        return NULL;
    }

    entity = find_variable(cpstate, path_name);
    if (entity == NULL || entity->type != ENT_PATH)
        return NULL;

    path = entity->entity.path;
    if (path == NULL || path->path == NIL)
        return NULL;

    foreach (lc, path->path)
    {
        if (is_ag_node(lfirst(lc), cypher_relationship))
        {
            cypher_relationship *rel = lfirst(lc);

            if (rel->varlen != NULL)
                return NULL;

            edge_count++;
            if (first_edge_name == NULL)
                first_edge_name = rel->name;
        }
    }

    if (edge_count == 0 || first_edge_name == NULL)
        return NULL;

    edge_id = make_raw_attr_var(pstate, first_edge_name, AG_EDGE_COLNAME_ID,
                                fn->location);
    if (edge_id == NULL)
        return NULL;

    return make_agtype_case_when_not_null(
        edge_id, (Expr *)make_agtype_integer_const(edge_count, fn->location),
        fn->location);
}

static Node *try_transform_fixed_path_list_cardinality(
    cypher_parsestate *cpstate, FuncCall *fn)
{
    ParseState *pstate = (ParseState *)cpstate;
    Node *inner_arg = NULL;
    cypher_node *start_node;
    cypher_relationship *single_rel;
    cypher_node *end_node;
    char *func_name;
    char *list_name;
    char *transform_name;
    Node *edge_id;
    Expr *result_expr;
    int64 cardinality;
    int64 edge_count;
    char *first_edge_name;
    Oid result_type;

    if (!parse_size_is_empty_func_arg(fn, T_FuncCall, &func_name, &inner_arg))
    {
        return NULL;
    }

    if (parse_fixed_path_cardinality_metadata(cpstate, (FuncCall *)inner_arg,
                                              &list_name, &transform_name,
                                              &edge_count,
                                              &first_edge_name))
    {
        if (is_nodes_list_name(list_name))
            cardinality = edge_count + 1;
        else
            cardinality = edge_count;

        if (transform_name != NULL && is_tail_name(transform_name))
            cardinality = Max(cardinality - 1, 0);

        edge_id = make_raw_attr_var(pstate, first_edge_name,
                                    AG_EDGE_COLNAME_ID, fn->location);
        if (edge_id == NULL)
            return NULL;

        if (is_size_name(func_name))
        {
            result_expr = (Expr *)make_agtype_integer_const(cardinality,
                                                           fn->location);
            result_type = AGTYPEOID;
        }
        else
        {
            Const *bool_const;

            bool_const = makeConst(BOOLOID, -1, InvalidOid, sizeof(bool),
                                   BoolGetDatum(cardinality == 0), false,
                                   true);
            bool_const->location = fn->location;
            result_expr = (Expr *)bool_const;
            result_type = BOOLOID;
        }

        return make_case_when_not_null(edge_id, result_expr, result_type,
                                       fn->location);
    }

    if (!parse_fixed_path_cardinality_list(cpstate, (FuncCall *)inner_arg,
                                           &list_name,
                                           &transform_name, &start_node,
                                           &single_rel, &end_node))
    {
        return NULL;
    }

    if (is_nodes_list_name(list_name))
    {
        if (transform_name != NULL && is_tail_name(transform_name))
            cardinality = 1;
        else
            cardinality = 2;
    }
    else
    {
        if (transform_name != NULL && is_tail_name(transform_name))
            cardinality = 0;
        else
            cardinality = 1;
    }

    edge_id = make_fixed_path_edge_id_var(pstate, single_rel,
                                    fn->location);
    if (edge_id == NULL)
        return NULL;

    if (is_size_name(func_name))
    {
        result_expr = (Expr *)make_agtype_integer_const(cardinality,
                                                       fn->location);
        result_type = AGTYPEOID;
    }
    else
    {
        Const *bool_const;

        bool_const = makeConst(BOOLOID, -1, InvalidOid, sizeof(bool),
                               BoolGetDatum(cardinality == 0), false, true);
        bool_const->location = fn->location;
        result_expr = (Expr *)bool_const;
        result_type = BOOLOID;
    }

    return make_case_when_not_null(edge_id, result_expr, result_type,
                                   fn->location);
}

static Node *try_transform_fixed_path_slice_cardinality(
    cypher_parsestate *cpstate, FuncCall *fn)
{
    ParseState *pstate = (ParseState *)cpstate;
    A_Indirection *a_ind;
    Node *indirection_arg = NULL;
    FuncCall *list_fn;
    A_Indices *indices;
    cypher_node *start_node;
    cypher_relationship *single_rel;
    cypher_node *end_node;
    char *func_name;
    char *list_name;
    Node *edge_id;
    Expr *result_expr;
    int64 lower = 0;
    int64 upper;
    int64 list_len;
    int64 cardinality;
    Oid result_type;

    if (!parse_size_is_empty_func_arg(fn, T_A_Indirection, &func_name,
                                      &indirection_arg))
    {
        return NULL;
    }

    a_ind = (A_Indirection *)indirection_arg;
    if (!parse_fixed_path_slice_list(cpstate, a_ind, false, &list_fn,
                                     &indices, &list_name, &start_node,
                                     &single_rel, &end_node))
    {
        return NULL;
    }

    if (!get_fixed_path_list_len(list_name, &list_len))
        return NULL;

    if (!parse_fixed_slice_bounds(indices, list_len, &lower, &upper))
        return NULL;

    cardinality = upper - lower;

    edge_id = make_fixed_path_edge_id_var(pstate, single_rel,
                                    fn->location);
    if (edge_id == NULL)
        return NULL;

    if (is_size_name(func_name))
    {
        result_expr = (Expr *)make_agtype_integer_const(cardinality,
                                                       fn->location);
        result_type = AGTYPEOID;
    }
    else
    {
        Const *bool_const;

        bool_const = makeConst(BOOLOID, -1, InvalidOid, sizeof(bool),
                               BoolGetDatum(cardinality == 0), false, true);
        bool_const->location = fn->location;
        result_expr = (Expr *)bool_const;
        result_type = BOOLOID;
    }

    return make_case_when_not_null(edge_id, result_expr, result_type,
                                   fn->location);
}

static Node *try_transform_fixed_path_relationships(cypher_parsestate *cpstate,
                                                    FuncCall *fn)
{
    cypher_relationship *single_rel = NULL;
    cypher_node *start_node;
    cypher_node *end_node;
    char *list_name = NULL;
    Node *entity_list;

    entity_list = try_make_fixed_path_entity_list(cpstate, fn, false);
    if (entity_list != NULL)
        return entity_list;

    if (!parse_fixed_path_list_node(cpstate, (Node *)fn, &list_name,
                                    &start_node, &single_rel, &end_node,
                                    false) ||
        !is_relationships_list_name(list_name))
    {
        return NULL;
    }

    return make_fixed_path_slice_list_result(cpstate, start_node, single_rel,
                                             end_node, list_name, 0, 1, false,
                                             fn->location);
}

static Node *try_transform_fixed_path_nodes(cypher_parsestate *cpstate,
                                            FuncCall *fn)
{
    cypher_node *start_node;
    cypher_relationship *single_rel;
    cypher_node *end_node;
    char *list_name = NULL;
    Node *entity_list;

    entity_list = try_make_fixed_path_entity_list(cpstate, fn, true);
    if (entity_list != NULL)
        return entity_list;

    if (!parse_fixed_path_list_node(cpstate, (Node *)fn, &list_name,
                                    &start_node, &single_rel, &end_node,
                                    true) ||
        !is_nodes_list_name(list_name))
    {
        return NULL;
    }

    return make_fixed_path_slice_list_result(cpstate, start_node, single_rel,
                                             end_node, list_name, 0, 2, false,
                                             fn->location);
}

static Node *try_transform_fixed_path_indexed_id(cypher_parsestate *cpstate,
                                                 FuncCall *fn)
{
    ParseState *pstate = (ParseState *)cpstate;
    A_Indirection *a_ind;
    Node *indirection_arg = NULL;
    Node *edge_id;
    Node *target_id;
    Node *target_agtype;

    if (!parse_id_func_arg(fn, T_A_Indirection, &indirection_arg))
    {
        return NULL;
    }

    a_ind = (A_Indirection *)indirection_arg;
    if (!make_fixed_path_indexed_id_result_vars(
            cpstate, a_ind, AG_EDGE_COLNAME_ID, fn->location, &edge_id,
            &target_id, NULL))
    {
        return NULL;
    }

    target_agtype = coerce_type(pstate, target_id, GRAPHIDOID, AGTYPEOID, -1,
                                COERCION_EXPLICIT, COERCE_EXPLICIT_CAST,
                                fn->location);

    return make_agtype_case_when_not_null(edge_id, (Expr *)target_agtype,
                                          fn->location);
}

static Node *try_transform_fixed_path_head_last(cypher_parsestate *cpstate,
                                               FuncCall *fn)
{
    ParseState *pstate = (ParseState *)cpstate;
    FuncCall *list_fn;
    cypher_node *start_node;
    cypher_relationship *single_rel;
    cypher_node *end_node;
    Node *list_arg = NULL;
    char *func_name;
    char *list_name;
    Node *edge_id;
    Node *result_expr;

    if (!parse_head_last_func_arg(fn, T_FuncCall, &func_name, &list_arg))
    {
        return NULL;
    }

    list_fn = (FuncCall *)list_arg;
    if (!parse_fixed_path_list_node(cpstate, (Node *)list_fn, &list_name,
                                    &start_node, &single_rel, &end_node,
                                    true))
        return NULL;

    edge_id = make_fixed_path_edge_id_var(pstate, single_rel,
                                    fn->location);
    if (edge_id == NULL)
        return NULL;

    if (is_relationships_list_name(list_name))
    {
        result_expr = try_build_edge_from_raw_attrs(cpstate, single_rel->name,
                                                    fn->location);
        if (result_expr == NULL)
            return NULL;
    }
    else
    {
        char *node_name;
        FuncExpr *vertex_expr;

        if (is_head_name(func_name))
            node_name = start_node->name;
        else
            node_name = end_node->name;

        vertex_expr = try_make_build_vertex_from_raw_attrs(cpstate, node_name,
                                                           fn->location,
                                                           NULL);
        if (vertex_expr == NULL)
            return NULL;

        result_expr = (Node *)vertex_expr;
    }

    return make_agtype_case_when_not_null(edge_id, (Expr *)result_expr,
                                          fn->location);
}

static Node *try_transform_fixed_path_head_last_consumer(
    cypher_parsestate *cpstate, FuncCall *fn)
{
    FuncCall *head_last_fn;
    FuncCall *list_fn;
    FuncCall *tail_fn;
    Node *head_last_arg = NULL;
    Node *tail_arg = NULL;
    char *func_name;
    char *head_last_name;
    char *list_name;
    int64 index;

    if (!parse_fixed_path_indexed_consumer_call(fn, T_FuncCall, &func_name))
    {
        return NULL;
    }

    head_last_fn = (FuncCall *)linitial(fn->args);
    if (!parse_head_last_func_arg(head_last_fn, T_FuncCall,
                                  &head_last_name, &head_last_arg))
    {
        return NULL;
    }

    list_fn = (FuncCall *)head_last_arg;
    if (!parse_single_func_name(list_fn, &list_name))
        return NULL;

    if (is_tail_name(list_name))
    {
        tail_fn = list_fn;
        if (!parse_func_arg(tail_fn, "tail", T_FuncCall, &tail_arg))
        {
            return NULL;
        }

        list_fn = (FuncCall *)tail_arg;
        if (!parse_single_func_name(list_fn, &list_name))
            return NULL;

        if (!get_fixed_path_head_last_index(head_last_name, list_name, true,
                                            &index))
            return NULL;
    }
    else
    {
        if (!get_fixed_path_head_last_index(head_last_name, list_name, false,
                                            &index))
            return NULL;
    }

    return try_transform_fixed_path_indexed_consumer_at(cpstate, func_name,
                                                        fn, list_fn, index);
}

static Node *try_transform_fixed_path_slice_head_last_consumer(
    cypher_parsestate *cpstate, FuncCall *fn)
{
    FuncCall *head_last_fn;
    FuncCall *list_fn;
    char *func_name;
    int64 index;

    if (!parse_fixed_path_indexed_consumer_call(fn, T_FuncCall, &func_name))
    {
        return NULL;
    }

    head_last_fn = (FuncCall *)linitial(fn->args);
    if (!parse_fixed_path_head_last_slice_index((Node *)head_last_fn, false,
                                                &list_fn, NULL, &index))
        return NULL;

    return try_transform_fixed_path_indexed_consumer_at(cpstate, func_name,
                                                        fn, list_fn, index);
}

static Node *try_transform_fixed_path_slice_transform_head_last_consumer(
    cypher_parsestate *cpstate, FuncCall *fn)
{
    FuncCall *head_last_fn;
    FuncCall *list_fn;
    char *func_name;
    int64 index;

    if (!parse_fixed_path_indexed_consumer_call(fn, T_FuncCall, &func_name))
    {
        return NULL;
    }

    head_last_fn = (FuncCall *)linitial(fn->args);
    if (!parse_fixed_path_head_last_slice_index((Node *)head_last_fn, true,
                                                &list_fn, NULL, &index))
        return NULL;

    return try_transform_fixed_path_indexed_consumer_at(cpstate, func_name,
                                                        fn, list_fn, index);
}

static Node *try_transform_fixed_path_slice_head_last_property_access(
    cypher_parsestate *cpstate, A_Indirection *a_ind)
{
    A_Indirection *indexed_arg;
    FuncCall *list_fn;
    List *indexed_indirection;
    int64 index;

    if (!IsA(a_ind->arg, FuncCall) ||
        a_ind->indirection == NIL)
    {
        return NULL;
    }

    if (!parse_fixed_path_head_last_slice_index(a_ind->arg, false,
                                                &list_fn, NULL, &index) &&
        !parse_fixed_path_head_last_slice_index(a_ind->arg, true,
                                                &list_fn, NULL, &index))
    {
        return NULL;
    }

    indexed_arg = make_indexed_indirection((Node *)list_fn, index,
                                           exprLocation((Node *)a_ind));
    indexed_indirection = indexed_arg->indirection;
    indexed_indirection = list_concat(indexed_indirection,
                                      copyObject(a_ind->indirection));

    indexed_arg->indirection = indexed_indirection;

    return try_transform_fixed_path_indexed_property_access(cpstate,
                                                            indexed_arg);
}

static Node *try_transform_fixed_path_slice_endpoint_property_access(
    cypher_parsestate *cpstate, A_Indirection *a_ind)
{
    FuncCall *endpoint_fn;
    FuncCall *relationships_fn;
    FuncCall *indexed_endpoint_fn;
    A_Indirection *indexed_endpoint_arg;
    Node *endpoint_arg = NULL;
    char *endpoint_name;
    int64 index;

    if (!IsA(a_ind->arg, FuncCall) ||
        a_ind->indirection == NIL)
    {
        return NULL;
    }

    endpoint_fn = (FuncCall *)a_ind->arg;
    if (!parse_endpoint_func_arg(endpoint_fn, T_FuncCall, &endpoint_name,
                                 &endpoint_arg))
    {
        return NULL;
    }

    if (!parse_fixed_relationship_slice_head_last_index(endpoint_arg,
                                                        &relationships_fn,
                                                        &index))
    {
        return NULL;
    }

    indexed_endpoint_fn = make_indexed_unary_func_call(
        endpoint_fn, (Node *)relationships_fn, index,
        exprLocation((Node *)a_ind));

    indexed_endpoint_arg = makeNode(A_Indirection);
    indexed_endpoint_arg->arg = (Node *)indexed_endpoint_fn;
    indexed_endpoint_arg->indirection = copyObject(a_ind->indirection);

    return try_transform_fixed_path_indexed_endpoint_property_access(
        cpstate, indexed_endpoint_arg);
}

static Node *try_transform_fixed_path_tail(cypher_parsestate *cpstate,
                                           FuncCall *fn)
{
    Node *list_arg = NULL;
    cypher_node *start_node;
    cypher_relationship *single_rel;
    cypher_node *end_node;
    char *list_name;
    int64 list_len;

    if (!parse_func_arg(fn, "tail", T_FuncCall, &list_arg))
    {
        return NULL;
    }

    if (!parse_fixed_path_list_node(cpstate, list_arg, &list_name,
                                    &start_node, &single_rel, &end_node,
                                    false) ||
        end_node->name == NULL)
        return NULL;

    if (!get_fixed_path_list_len(list_name, &list_len))
        return NULL;

    return make_fixed_path_slice_list_result(cpstate, start_node, single_rel,
                                             end_node, list_name, 1, list_len,
                                             false, fn->location);
}

static Node *try_transform_fixed_path_list_slice(cypher_parsestate *cpstate,
                                                A_Indirection *a_ind)
{
    FuncCall *list_fn;
    A_Indices *indices;
    cypher_node *start_node;
    cypher_relationship *single_rel;
    cypher_node *end_node;
    char *list_name;
    int64 lower = 0;
    int64 upper;
    int64 list_len;

    if (!parse_fixed_path_slice_list(cpstate, a_ind, true, &list_fn,
                                     &indices, &list_name, &start_node,
                                     &single_rel, &end_node))
    {
        return NULL;
    }

    if (!get_fixed_path_list_len(list_name, &list_len))
        return NULL;

    if (!parse_fixed_slice_bounds(indices, list_len, &lower, &upper))
        return NULL;

    return make_fixed_path_slice_list_result(cpstate, start_node, single_rel,
                                             end_node, list_name, lower, upper,
                                             false,
                                             exprLocation((Node *)a_ind));
}

static Node *try_transform_fixed_path_slice_tail(cypher_parsestate *cpstate,
                                                 FuncCall *fn)
{
    A_Indirection *slice_arg;
    Node *slice_arg_node = NULL;
    A_Indirection *tail_slice_arg;
    A_Indices *slice_indices;
    A_Indices *tail_indices;
    FuncCall *list_fn;
    char *list_name;
    int64 lower = 0;
    int64 upper;

    if (!parse_func_arg(fn, "tail", T_A_Indirection, &slice_arg_node))
    {
        return NULL;
    }

    slice_arg = (A_Indirection *)slice_arg_node;
    if (!parse_path_list_slice_arg(slice_arg, &list_fn, &slice_indices,
                                   &list_name))
    {
        return NULL;
    }

    if (!parse_optional_nonnegative_bound(slice_indices->lidx, &lower))
    {
        return NULL;
    }

    tail_indices = makeNode(A_Indices);
    tail_indices->is_slice = true;
    {
        A_Const *lower_const = makeNode(A_Const);

        lower_const->val.ival.type = T_Integer;
        lower_const->val.ival.ival = lower + 1;
        lower_const->location = fn->location;
        tail_indices->lidx = (Node *)lower_const;
    }

    if (slice_indices->uidx != NULL)
    {
        if (!parse_optional_nonnegative_bound(slice_indices->uidx, &upper))
        {
            return NULL;
        }

        {
            A_Const *upper_const = makeNode(A_Const);

            upper_const->val.ival.type = T_Integer;
            upper_const->val.ival.ival = upper;
            upper_const->location = fn->location;
            tail_indices->uidx = (Node *)upper_const;
        }
    }
    else
    {
        tail_indices->uidx = NULL;
    }

    tail_slice_arg = makeNode(A_Indirection);
    tail_slice_arg->arg = (Node *)copyObject(list_fn);
    tail_slice_arg->indirection = list_make1(tail_indices);

    return try_transform_fixed_path_list_slice(cpstate, tail_slice_arg);
}

static Node *try_transform_fixed_path_reverse(cypher_parsestate *cpstate,
                                              FuncCall *fn)
{
    Node *list_arg = NULL;
    cypher_node *start_node;
    cypher_relationship *single_rel;
    cypher_node *end_node;
    char *list_name;
    int64 list_len;

    if (!parse_func_arg(fn, "reverse", T_FuncCall, &list_arg))
    {
        return NULL;
    }

    if (!parse_fixed_path_list_node(cpstate, list_arg, &list_name,
                                    &start_node, &single_rel, &end_node,
                                    true))
        return NULL;

    if (!get_fixed_path_list_len(list_name, &list_len))
        return NULL;

    return make_fixed_path_slice_list_result(cpstate, start_node, single_rel,
                                             end_node, list_name, 0, list_len,
                                             true, fn->location);
}

static Node *try_transform_fixed_path_slice_reverse(cypher_parsestate *cpstate,
                                                    FuncCall *fn)
{
    A_Indirection *slice_arg;
    Node *slice_arg_node = NULL;
    A_Indices *indices;
    FuncCall *list_fn;
    cypher_node *start_node;
    cypher_relationship *single_rel;
    cypher_node *end_node;
    char *list_name;
    int64 lower = 0;
    int64 upper;
    int64 list_len;

    if (!parse_func_arg(fn, "reverse", T_A_Indirection, &slice_arg_node))
    {
        return NULL;
    }

    slice_arg = (A_Indirection *)slice_arg_node;
    if (!parse_fixed_path_slice_list(cpstate, slice_arg, true, &list_fn,
                                     &indices, &list_name, &start_node,
                                     &single_rel, &end_node))
    {
        return NULL;
    }

    if (!get_fixed_path_list_len(list_name, &list_len))
        return NULL;

    if (!parse_fixed_slice_bounds(indices, list_len, &lower, &upper))
        return NULL;

    return make_fixed_path_slice_list_result(cpstate, start_node, single_rel,
                                             end_node, list_name, lower, upper,
                                             true, fn->location);
}

static Node *try_transform_fixed_path_indexed_edge_endpoint_id(
    cypher_parsestate *cpstate, FuncCall *fn)
{
    ParseState *pstate = (ParseState *)cpstate;
    A_Indirection *a_ind;
    Node *indirection_arg = NULL;
    cypher_node *start_node;
    cypher_relationship *single_rel;
    cypher_node *end_node;
    char *func_name;
    char *list_name;
    Node *edge_id;
    Node *target_id;
    Node *target_agtype;

    if (!parse_edge_endpoint_id_func_arg(fn, T_A_Indirection, &func_name,
                                         &indirection_arg))
    {
        return NULL;
    }

    a_ind = (A_Indirection *)indirection_arg;
    if (!parse_fixed_relationship_zero_index(cpstate, a_ind, false, &list_name,
                                             &start_node, &single_rel,
                                             &end_node))
    {
        return NULL;
    }

    if (!make_fixed_endpoint_edge_id_vars(pstate, single_rel, func_name,
                                          fn->location, &edge_id,
                                          &target_id))
        return NULL;

    target_agtype = coerce_type(pstate, target_id, GRAPHIDOID, AGTYPEOID, -1,
                                COERCION_EXPLICIT, COERCE_EXPLICIT_CAST,
                                fn->location);

    return make_agtype_case_when_not_null(edge_id, (Expr *)target_agtype,
                                          fn->location);
}

static Node *make_fixed_endpoint_vertex_agtype(
    cypher_parsestate *cpstate, cypher_node *start_node,
    cypher_relationship *single_rel, cypher_node *end_node,
    const char *endpoint_name, int location)
{
    Node *edge_id;
    Node *vertex_id;
    Node *vertex_props;

    if (!make_fixed_endpoint_vertex_vars(cpstate, start_node, single_rel,
                                         end_node, endpoint_name, location,
                                         &edge_id, &vertex_id, &vertex_props))
        return NULL;

    return make_endpoint_vertex_agtype(cpstate, edge_id, vertex_id,
                                       vertex_props, location);
}

static Node *make_fixed_endpoint_vertex_consumer_expr(
    cypher_parsestate *cpstate, const char *func_name, Node *vertex_id,
    Node *vertex_props, int location)
{
    ParseState *pstate = (ParseState *)cpstate;

    if (is_id_name(func_name))
    {
        return coerce_type(pstate, vertex_id, GRAPHIDOID, AGTYPEOID,
                           -1, COERCION_EXPLICIT, COERCE_EXPLICIT_CAST,
                           location);
    }

    if (is_properties_name(func_name))
        return vertex_props;

    if (is_keys_name(func_name))
        return make_age_keys_expr(vertex_props, location);

    return make_label_or_labels_expr(cpstate, vertex_id,
                                     is_labels_name(func_name), location);
}

static Node *make_endpoint_vertex_consumer_agtype(
    cypher_parsestate *cpstate, const char *func_name, Node *edge_id,
    Node *vertex_id, Node *vertex_props, int location)
{
    Node *result_expr;

    result_expr = make_fixed_endpoint_vertex_consumer_expr(
        cpstate, func_name, vertex_id, vertex_props, location);
    if (result_expr == NULL)
        return NULL;

    return make_agtype_case_when_not_null(edge_id, (Expr *)result_expr,
                                          location);
}

static Node *make_fixed_endpoint_vertex_consumer_agtype(
    cypher_parsestate *cpstate, cypher_node *start_node,
    cypher_relationship *single_rel, cypher_node *end_node,
    const char *endpoint_name, const char *func_name, int location)
{
    Node *edge_id;
    Node *vertex_id;
    Node *vertex_props;

    if (!make_fixed_endpoint_vertex_vars(cpstate, start_node, single_rel,
                                         end_node, endpoint_name, location,
                                         &edge_id, &vertex_id, &vertex_props))
        return NULL;

    return make_endpoint_vertex_consumer_agtype(cpstate, func_name, edge_id,
                                                vertex_id, vertex_props,
                                                location);
}

static Node *try_transform_fixed_path_indexed_endpoint_vertex(
    cypher_parsestate *cpstate, FuncCall *fn)
{
    cypher_node *start_node;
    cypher_relationship *single_rel;
    cypher_node *end_node;
    char *func_name;
    char *list_name;

    if (!parse_fixed_endpoint_zero_index(cpstate, fn, true, &func_name,
                                         &list_name, &start_node, &single_rel,
                                         &end_node))
    {
        return NULL;
    }

    return make_fixed_endpoint_vertex_agtype(cpstate, start_node, single_rel,
                                             end_node, func_name,
                                             fn->location);
}

static Node *try_transform_fixed_path_indexed_endpoint_vertex_function(
    cypher_parsestate *cpstate, FuncCall *fn)
{
    FuncCall *endpoint_fn;
    Node *endpoint_arg = NULL;
    cypher_node *start_node;
    cypher_relationship *single_rel;
    cypher_node *end_node;
    char *func_name;
    char *endpoint_name;
    char *list_name;

    if (!parse_fixed_path_endpoint_vertex_consumer_call_arg(fn, &func_name,
                                                            &endpoint_arg))
    {
        return NULL;
    }

    endpoint_fn = (FuncCall *)endpoint_arg;
    if (!parse_fixed_endpoint_zero_index(cpstate, endpoint_fn, true,
                                         &endpoint_name, &list_name,
                                         &start_node, &single_rel, &end_node))
    {
        return NULL;
    }

    return make_fixed_endpoint_vertex_consumer_agtype(
        cpstate, start_node, single_rel, end_node, endpoint_name, func_name,
        fn->location);
}

static Node *try_transform_edge_endpoint_vertex(cypher_parsestate *cpstate,
                                                FuncCall *fn)
{
    Node *edge_id;
    Node *vertex_id;
    Node *vertex_props;
    char *endpoint_name;
    char *edge_name;

    if (!parse_edge_endpoint_columnref_arg(fn, &endpoint_name, &edge_name))
    {
        return NULL;
    }

    if (!make_edge_endpoint_vertex_vars(
            cpstate, edge_name, endpoint_name, fn->location, &edge_id,
            &vertex_id, &vertex_props))
    {
        return NULL;
    }

    return make_endpoint_vertex_agtype(cpstate, edge_id, vertex_id,
                                       vertex_props, fn->location);
}

static Node *try_transform_edge_endpoint_vertex_function(
    cypher_parsestate *cpstate, FuncCall *fn)
{
    FuncCall *endpoint_fn;
    Node *endpoint_arg = NULL;
    Node *edge_id;
    Node *vertex_id;
    Node *vertex_props;
    char *func_name;
    char *endpoint_name;
    char *edge_name;

    if (!parse_fixed_path_endpoint_vertex_consumer_call_arg(fn, &func_name,
                                                            &endpoint_arg))
    {
        return NULL;
    }

    endpoint_fn = (FuncCall *)endpoint_arg;
    if (!parse_edge_endpoint_columnref_arg(endpoint_fn, &endpoint_name,
                                           &edge_name))
    {
        return NULL;
    }

    if (!make_edge_endpoint_vertex_vars(
            cpstate, edge_name, endpoint_name, fn->location, &edge_id,
            &vertex_id, &vertex_props))
    {
        return NULL;
    }

    return make_endpoint_vertex_consumer_agtype(cpstate, func_name, edge_id,
                                                vertex_id, vertex_props,
                                                fn->location);
}

static Node *try_transform_fixed_path_slice_endpoint_vertex(
    cypher_parsestate *cpstate, FuncCall *fn)
{
    FuncCall *relationships_fn;
    FuncCall *indexed_endpoint_fn;
    Node *endpoint_arg = NULL;
    char *endpoint_name;
    int64 index;

    if (!parse_endpoint_func_arg(fn, T_FuncCall, &endpoint_name,
                                 &endpoint_arg))
    {
        return NULL;
    }

    if (!parse_fixed_relationship_slice_head_last_index(endpoint_arg,
                                                        &relationships_fn,
                                                        &index))
    {
        return NULL;
    }

    indexed_endpoint_fn = make_indexed_unary_func_call(
        fn, (Node *)relationships_fn, index, fn->location);

    return try_transform_fixed_path_indexed_endpoint_vertex(
        cpstate, indexed_endpoint_fn);
}

static Node *try_transform_fixed_path_slice_endpoint_vertex_function(
    cypher_parsestate *cpstate, FuncCall *fn)
{
    FuncCall *endpoint_fn;
    Node *endpoint_fn_arg = NULL;
    FuncCall *relationships_fn;
    FuncCall *indexed_endpoint_fn;
    FuncCall *indexed_consumer;
    Node *endpoint_arg = NULL;
    char *func_name;
    char *endpoint_name;
    int64 index;

    if (!parse_fixed_path_endpoint_vertex_consumer_call_arg(fn, &func_name,
                                                            &endpoint_fn_arg))
    {
        return NULL;
    }

    endpoint_fn = (FuncCall *)endpoint_fn_arg;
    if (!parse_endpoint_func_arg(endpoint_fn, T_FuncCall, &endpoint_name,
                                 &endpoint_arg))
    {
        return NULL;
    }

    if (!parse_fixed_relationship_slice_head_last_index(endpoint_arg,
                                                        &relationships_fn,
                                                        &index))
    {
        return NULL;
    }

    indexed_endpoint_fn = make_indexed_unary_func_call(
        endpoint_fn, (Node *)relationships_fn, index, fn->location);
    indexed_consumer = make_unary_func_call(fn, (Node *)indexed_endpoint_fn,
                                            fn->location);

    return try_transform_fixed_path_indexed_endpoint_vertex_function(
        cpstate, indexed_consumer);
}

static Node *try_transform_fixed_path_indexed_properties(
    cypher_parsestate *cpstate, FuncCall *fn)
{
    A_Indirection *a_ind;
    Node *indirection_arg = NULL;
    Node *edge_id;
    Node *target_props;

    if (!parse_func_arg(fn, "properties", T_A_Indirection, &indirection_arg))
    {
        return NULL;
    }

    a_ind = (A_Indirection *)indirection_arg;
    if (!make_fixed_path_indexed_props_result_vars(
            cpstate, a_ind, false, fn->location, &edge_id, &target_props))
    {
        return NULL;
    }

    return make_agtype_case_when_not_null(edge_id, (Expr *)target_props,
                                          fn->location);
}

static Node *try_transform_fixed_path_indexed_keys(cypher_parsestate *cpstate,
                                                  FuncCall *fn)
{
    A_Indirection *a_ind;
    Node *indirection_arg = NULL;
    Node *edge_id;
    Node *target_props;
    Node *keys_expr;

    if (!parse_keys_func_arg(fn, T_A_Indirection, &indirection_arg))
    {
        return NULL;
    }

    a_ind = (A_Indirection *)indirection_arg;
    if (!make_fixed_path_indexed_props_result_vars(
            cpstate, a_ind, false, fn->location, &edge_id, &target_props))
    {
        return NULL;
    }

    keys_expr = make_age_keys_expr(target_props, fn->location);

    return make_agtype_case_when_not_null(edge_id, (Expr *)keys_expr,
                                          fn->location);
}

static Node *try_transform_fixed_path_indexed_property_access(
    cypher_parsestate *cpstate, A_Indirection *a_ind)
{
    Node *edge_id;
    Node *target_props;
    List *args = NIL;
    FuncExpr *access_expr;

    if (!has_indirection_count(a_ind, 2))
    {
        return NULL;
    }

    if (!make_fixed_path_indexed_props_result_vars(
            cpstate, a_ind, true, exprLocation((Node *)a_ind), &edge_id,
            &target_props))
    {
        return NULL;
    }

    args = lappend(args, target_props);
    if (!append_agtype_access_indirections(cpstate, &args, a_ind->indirection,
                                           true))
    {
        return NULL;
    }

    access_expr = make_agtype_access_expr(args, exprLocation((Node *)a_ind),
                                          true);

    return make_agtype_case_when_not_null(edge_id, (Expr *)access_expr,
                                          exprLocation((Node *)a_ind));
}

static Node *try_transform_fixed_path_indexed_endpoint_property_access(
    cypher_parsestate *cpstate, A_Indirection *a_ind)
{
    FuncCall *endpoint_fn;
    cypher_node *start_node;
    cypher_relationship *single_rel;
    cypher_node *end_node;
    char *func_name;
    char *list_name;
    Node *edge_id;
    Node *target_props;
    List *args = NIL;
    FuncExpr *access_expr;

    if (!IsA(a_ind->arg, FuncCall) ||
        !has_indirection_count(a_ind, 1))
    {
        return NULL;
    }

    endpoint_fn = (FuncCall *)a_ind->arg;
    if (!parse_fixed_endpoint_zero_index(cpstate, endpoint_fn, true,
                                         &func_name, &list_name, &start_node,
                                         &single_rel, &end_node))
    {
        return NULL;
    }

    if (!make_fixed_endpoint_vertex_props_var(
            cpstate, start_node, single_rel, end_node, func_name,
            exprLocation((Node *)a_ind), &edge_id, &target_props))
        return NULL;

    args = lappend(args, target_props);
    if (!append_agtype_access_indirections(cpstate, &args, a_ind->indirection,
                                           false))
    {
        return NULL;
    }

    access_expr = make_agtype_access_expr(args, exprLocation((Node *)a_ind),
                                          true);

    return make_agtype_case_when_not_null(edge_id, (Expr *)access_expr,
                                          exprLocation((Node *)a_ind));
}

static Node *try_transform_current_edge_endpoint_property_access(
    cypher_parsestate *cpstate, A_Indirection *a_ind)
{
    FuncCall *endpoint_fn;
    Node *edge_id;
    Node *vertex_id;
    Node *vertex_props;
    List *args = NIL;
    FuncExpr *access_expr;
    char *endpoint_name;
    char *edge_name;

    if (!IsA(a_ind->arg, FuncCall) ||
        !has_indirection_count(a_ind, 1))
    {
        return NULL;
    }

    endpoint_fn = (FuncCall *)a_ind->arg;
    if (!parse_edge_endpoint_columnref_arg(endpoint_fn, &endpoint_name,
                                           &edge_name))
    {
        return NULL;
    }

    if (!make_edge_endpoint_vertex_vars(
            cpstate, edge_name, endpoint_name, exprLocation((Node *)a_ind),
            &edge_id, &vertex_id, &vertex_props))
    {
        return NULL;
    }

    args = lappend(args, vertex_props);
    if (!append_agtype_access_indirections(cpstate, &args, a_ind->indirection,
                                           false))
    {
        return NULL;
    }

    access_expr = make_agtype_access_expr(args, exprLocation((Node *)a_ind),
                                          true);

    return make_agtype_case_when_not_null(edge_id, (Expr *)access_expr,
                                          exprLocation((Node *)a_ind));
}

static Node *try_transform_fixed_path_reverse_indexed_consumer(
    cypher_parsestate *cpstate, FuncCall *fn)
{
    A_Indirection *a_ind;
    FuncCall *reverse_fn;
    FuncCall *list_fn;
    Node *reverse_arg = NULL;
    char *func_name;
    char *list_name;
    int64 reverse_index;
    int64 normal_index;

    if (!parse_fixed_path_indexed_consumer_call(fn, T_A_Indirection,
                                                &func_name))
    {
        return NULL;
    }

    a_ind = (A_Indirection *)linitial(fn->args);
    if (!IsA(a_ind->arg, FuncCall) ||
        !parse_single_indirection_nonnegative_index(a_ind, NULL,
                                                   &reverse_index))
    {
        return NULL;
    }

    reverse_fn = (FuncCall *)a_ind->arg;
    if (!parse_func_arg(reverse_fn, "reverse", T_FuncCall, &reverse_arg))
    {
        return NULL;
    }

    list_fn = (FuncCall *)reverse_arg;
    if (!parse_single_func_name(list_fn, &list_name))
        return NULL;

    if (is_nodes_list_name(list_name))
    {
        if (reverse_index > 1)
            return NULL;
        normal_index = reverse_index == 0 ? 1 : 0;
    }
    else if (is_relationships_list_name(list_name))
    {
        if (reverse_index != 0)
            return NULL;
        normal_index = 0;
    }
    else
    {
        return NULL;
    }

    return try_transform_fixed_path_indexed_consumer_at(cpstate, func_name,
                                                        fn, list_fn,
                                                        normal_index);
}

static Node *try_transform_fixed_path_indexed_label_type(
    cypher_parsestate *cpstate, FuncCall *fn)
{
    A_Indirection *a_ind;
    char *func_name;
    char *list_name = NULL;
    Node *edge_id;
    Node *target_id;
    Node *label_expr;
    Node *indirection_arg = NULL;

    if (!parse_label_type_func_arg(fn, T_A_Indirection, &func_name,
                                   &indirection_arg))
    {
        return NULL;
    }

    a_ind = (A_Indirection *)indirection_arg;
    if (!make_fixed_path_indexed_id_result_vars(
            cpstate, a_ind, AG_EDGE_COLNAME_ID, fn->location, &edge_id,
            &target_id, &list_name) ||
        (is_type_name(func_name) && !is_relationships_list_name(list_name)))
    {
        return NULL;
    }

    label_expr = make_label_or_labels_expr(cpstate, target_id, false,
                                           fn->location);
    if (label_expr == NULL)
        return NULL;

    return make_agtype_case_when_not_null(edge_id, (Expr *)label_expr,
                                          fn->location);
}

static Node *try_transform_fixed_path_indexed_labels(
    cypher_parsestate *cpstate, FuncCall *fn)
{
    A_Indirection *a_ind;
    char *list_name = NULL;
    Node *edge_id;
    Node *target_id;
    Node *labels_expr;
    Node *indirection_arg = NULL;

    if (!parse_func_arg(fn, "labels", T_A_Indirection, &indirection_arg))
    {
        return NULL;
    }

    a_ind = (A_Indirection *)indirection_arg;
    if (!make_fixed_path_indexed_id_result_vars(
            cpstate, a_ind, NULL, fn->location, &edge_id, &target_id,
            &list_name) ||
        !is_nodes_list_name(list_name))
    {
        return NULL;
    }

    labels_expr = make_label_or_labels_expr(cpstate, target_id, true,
                                            fn->location);
    if (labels_expr == NULL)
        return NULL;

    return make_agtype_case_when_not_null(edge_id, (Expr *)labels_expr,
                                          fn->location);
}

static Node *try_transform_vle_path_id_access(cypher_parsestate *cpstate,
                                              FuncCall *fn)
{
    A_Indirection *a_ind = NULL;
    Node *indirection_arg = NULL;
    char *list_name = NULL;
    Expr *vle_expr = NULL;
    Node *index_expr = NULL;

    if (!parse_id_func_arg(fn, T_A_Indirection, &indirection_arg))
    {
        return NULL;
    }

    a_ind = (A_Indirection *)indirection_arg;
    if (!parse_arbitrary_vle_path_indexed_list_index(cpstate, a_ind,
                                                     &vle_expr, &list_name,
                                                     &index_expr) &&
        !parse_vle_path_indexed_list_index(cpstate, a_ind, &vle_expr,
                                           &list_name, &index_expr))
    {
        return NULL;
    }

    return make_vle_binary_agtype_expr(get_vle_indexed_id_func_oid(list_name),
                                       (Node *)vle_expr, (Node *)index_expr);
}

static bool parse_vle_path_tail_last_list(cypher_parsestate *cpstate,
                                          Node *node, Expr **vle_expr,
                                          char **list_name)
{
    Node *tail_arg = NULL;
    Node *list_arg = NULL;
    FuncCall *last_fn = NULL;
    FuncCall *tail_fn = NULL;

    if (!IsA(node, FuncCall))
    {
        return false;
    }

    last_fn = (FuncCall *)node;
    if (!parse_func_arg(last_fn, "last", T_FuncCall, &tail_arg))
    {
        return false;
    }

    tail_fn = (FuncCall *)tail_arg;
    if (!parse_func_arg(tail_fn, "tail", T_FuncCall, &list_arg))
    {
        return false;
    }

    if (!parse_current_vle_path_or_fixed_raw_edge_list(
            cpstate, list_arg, vle_expr, list_name))
    {
        return false;
    }

    return *vle_expr != NULL;
}

static bool parse_vle_tail_last_endpoint(cypher_parsestate *cpstate,
                                         Node *node, Expr **vle_expr,
                                         bool *start_endpoint)
{
    FuncCall *endpoint_fn = NULL;
    char *list_name = NULL;

    if (!IsA(node, FuncCall))
    {
        return false;
    }

    endpoint_fn = (FuncCall *)node;
    if (!parse_vle_endpoint_function_name(endpoint_fn, NULL, start_endpoint))
    {
        return false;
    }

    return parse_vle_path_tail_last_list(cpstate,
                                         linitial(endpoint_fn->args),
                                         vle_expr, &list_name) &&
        is_relationships_list_name(list_name);
}

static Node *try_transform_vle_path_boundary_id_function(
    cypher_parsestate *cpstate, FuncCall *fn)
{
    Node *boundary_arg = NULL;
    char *list_name = NULL;
    Expr *vle_expr = NULL;
    Node *nested_expr = NULL;
    Oid func_oid;
    int64 index;

    if (!parse_id_func_arg(fn, T_FuncCall, &boundary_arg))
    {
        return NULL;
    }

    if (parse_vle_path_tail_last_list(cpstate, boundary_arg, &vle_expr,
                                      &list_name))
    {
        func_oid = get_vle_tail_last_id_oid(list_name);
        return make_vle_unary_agtype_expr(func_oid, (Node *)vle_expr);
    }

    if (IsA(boundary_arg, FuncCall))
    {
        nested_expr = transform_vle_path_nested_transform_head_last(
            cpstate, (FuncCall *)boundary_arg, VLE_SLICE_BOUNDARY_ID_OFFSET);
        if (nested_expr != NULL)
        {
            return nested_expr;
        }
    }

    if (!parse_vle_path_boundary_list_index(cpstate, boundary_arg,
                                            &vle_expr, &list_name, &index))
    {
        return NULL;
    }

    return make_vle_indexed_id_expr(vle_expr, list_name, index, fn->location);
}

static Node *transform_vle_path_edge_endpoint_index(
    cypher_parsestate *cpstate, Node *node, Oid start_func_oid,
    Oid end_func_oid)
{
    Expr *vle_expr = NULL;
    Node *index_expr = NULL;
    bool start_endpoint;

    if (!parse_vle_edge_endpoint_index(cpstate, node, &vle_expr, &index_expr,
                                       &start_endpoint))
    {
        return NULL;
    }

    return make_vle_edge_endpoint_index_expr(vle_expr, index_expr,
                                             start_endpoint, start_func_oid,
                                             end_func_oid);
}

static Node *try_transform_vle_path_endpoint_id_access(
    cypher_parsestate *cpstate, FuncCall *fn)
{
    Node *endpoint_arg = NULL;

    if (!parse_id_func_arg(fn, T_FuncCall, &endpoint_arg))
    {
        return NULL;
    }

    return transform_vle_path_edge_endpoint_index(
        cpstate, endpoint_arg,
        get_age_vle_edge_start_id_at_oid(),
        get_age_vle_edge_end_id_at_oid());
}

static Node *try_transform_vle_path_nested_transform_index_endpoint_id_access(
    cypher_parsestate *cpstate, FuncCall *fn)
{
    Node *endpoint_arg = NULL;

    if (!parse_id_func_arg(fn, T_FuncCall, &endpoint_arg))
    {
        return NULL;
    }

    return transform_vle_path_nested_transform_index_endpoint(
        cpstate, (FuncCall *)endpoint_arg, VLE_SLICE_BOUNDARY_START_ID_OFFSET,
        VLE_SLICE_BOUNDARY_END_ID_OFFSET, fn->location);
}

static Node *make_vle_nested_transform_index_mode_expr(
    cypher_parsestate *cpstate, A_Indirection *a_ind, int64 mode_offset,
    int location, char **list_name, bool require_relationships)
{
    Expr *vle_expr = NULL;
    char *parsed_list_name = NULL;
    Const *lower_expr = NULL;
    Const *upper_expr = NULL;
    int64 mode;

    if (!parse_vle_path_nested_transform_index(cpstate, a_ind, &vle_expr,
                                               &parsed_list_name, &lower_expr,
                                               &upper_expr, &mode))
    {
        return NULL;
    }

    if (require_relationships && !is_relationships_list_name(parsed_list_name))
    {
        return NULL;
    }

    if (list_name != NULL)
        *list_name = parsed_list_name;

    return make_vle_slice_boundary_mode_expr(
        (Node *)vle_expr, (Node *)lower_expr, (Node *)upper_expr,
        mode + mode_offset, location);
}

static Node *transform_vle_path_nested_transform_index_endpoint(
    cypher_parsestate *cpstate, FuncCall *endpoint_fn,
    int64 start_mode_offset, int64 end_mode_offset, int location)
{
    A_Indirection *a_ind = NULL;
    Node *endpoint_arg = NULL;
    int64 mode_offset;

    if (!parse_vle_endpoint_mode_offset_either_arg(
            endpoint_fn, T_A_Indirection, T_A_Indirection,
            start_mode_offset, end_mode_offset, NULL, NULL, &mode_offset,
            &endpoint_arg))
    {
        return NULL;
    }

    a_ind = (A_Indirection *)endpoint_arg;
    return make_vle_nested_transform_index_mode_expr(
        cpstate, a_ind, mode_offset, location, NULL, true);
}

static Node *try_transform_vle_path_boundary_endpoint_id_access(
    cypher_parsestate *cpstate, FuncCall *fn)
{
    Node *endpoint_arg = NULL;

    if (!parse_id_func_arg(fn, T_FuncCall, &endpoint_arg))
    {
        return NULL;
    }

    return transform_vle_path_boundary_endpoint(
        cpstate, (FuncCall *)endpoint_arg, 2, 3,
        get_age_vle_edge_start_id_at_oid(), get_age_vle_edge_end_id_at_oid(),
        fn->location);
}

static Node *transform_vle_path_boundary_endpoint(
    cypher_parsestate *cpstate, FuncCall *endpoint_fn,
    int64 start_tail_last_mode, int64 end_tail_last_mode,
    Oid start_index_func_oid, Oid end_index_func_oid, int location)
{
    char *list_name = NULL;
    Expr *vle_expr = NULL;
    Const *index_expr = NULL;
    Oid func_oid;
    bool start_endpoint;
    Node *endpoint_arg = NULL;
    int64 index;
    int64 mode;

    if (!parse_endpoint_func_arg(endpoint_fn, T_FuncCall, NULL,
                                 &endpoint_arg) ||
        !parse_vle_endpoint_function_name(endpoint_fn, NULL, &start_endpoint))
    {
        return NULL;
    }

    if (parse_vle_path_tail_last_list(cpstate, endpoint_arg,
                                      &vle_expr, &list_name) &&
        is_relationships_list_name(list_name))
    {
        mode = start_endpoint ? start_tail_last_mode : end_tail_last_mode;
        func_oid = get_age_vle_tail_last_edge_endpoint_oid();
        return make_vle_binary_mode_expr(func_oid, (Node *)vle_expr, mode,
                                         location);
    }

    if (!parse_vle_path_boundary_list_index(cpstate, endpoint_arg,
                                            &vle_expr, &list_name, &index) ||
        !is_relationships_list_name(list_name))
    {
        return NULL;
    }

    index_expr = make_agtype_integer_const(index, location);
    return make_vle_edge_endpoint_index_expr(
        vle_expr, (Node *)index_expr, start_endpoint,
        start_index_func_oid, end_index_func_oid);
}

static Node *try_transform_vle_path_slice_boundary_endpoint_id_access(
    cypher_parsestate *cpstate, FuncCall *fn)
{
    Node *endpoint_arg = NULL;

    if (!parse_id_func_arg(fn, T_FuncCall, &endpoint_arg))
    {
        return NULL;
    }

    return transform_vle_path_slice_boundary_endpoint(
        cpstate, (FuncCall *)endpoint_arg, VLE_SLICE_BOUNDARY_START_ID_OFFSET,
        VLE_SLICE_BOUNDARY_END_ID_OFFSET);
}

static Node *transform_vle_path_slice_boundary_endpoint(
    cypher_parsestate *cpstate, FuncCall *endpoint_fn,
    int64 start_mode_offset, int64 end_mode_offset)
{
    FuncCall *boundary_fn = NULL;
    Node *boundary_arg = NULL;
    int64 mode_offset;

    if (!parse_vle_endpoint_mode_offset_either_arg(
            endpoint_fn, T_FuncCall, T_FuncCall, start_mode_offset,
            end_mode_offset, NULL, NULL, &mode_offset, &boundary_arg))
    {
        return NULL;
    }

    boundary_fn = (FuncCall *)boundary_arg;
    return transform_vle_path_any_slice_boundary_head_last(cpstate,
                                                           boundary_fn,
                                                           mode_offset);
}

static Node *try_transform_vle_path_endpoint_access(cypher_parsestate *cpstate,
                                                   FuncCall *fn)
{
    Expr *vle_expr = NULL;
    Node *index_expr = NULL;
    bool start_endpoint;
    Oid func_oid;

    if (!parse_vle_edge_endpoint_index(cpstate, (Node *)fn, &vle_expr,
                                       &index_expr, &start_endpoint))
    {
        return NULL;
    }

    func_oid = start_endpoint ? get_age_vle_edge_start_node_at_oid() :
        get_age_vle_edge_end_node_at_oid();

    return make_vle_binary_agtype_expr(func_oid, (Node *)vle_expr,
                                       index_expr);
}

static Node *make_vle_edge_endpoint_field_expr(const char *field_name,
                                               Expr *vle_expr,
                                               Node *index_expr,
                                               bool start_endpoint,
                                               int location)
{
    int64 mode;

    if (!get_vle_endpoint_field_mode(field_name, start_endpoint, &mode))
    {
        return NULL;
    }

    return make_vle_edge_endpoint_field_mode_expr(vle_expr, index_expr, mode,
                                                  location);
}

static Node *make_vle_tail_last_endpoint_field_expr(const char *field_name,
                                                    Expr *vle_expr,
                                                    bool start_endpoint,
                                                    int location)
{
    int64 mode;

    if (!get_vle_endpoint_field_mode(field_name, start_endpoint, &mode))
    {
        return NULL;
    }

    return make_vle_binary_mode_expr(get_age_vle_tail_last_endpoint_field_oid(),
                                     (Node *)vle_expr, mode, location);
}

static Node *make_vle_tail_last_field_expr(const char *list_name,
                                           const char *field_name,
                                           Expr *vle_expr, int location)
{
    int64 mode;

    if (!get_vle_tail_last_field_mode(list_name, field_name, &mode))
    {
        return NULL;
    }

    return make_vle_binary_mode_expr(get_age_vle_tail_last_field_oid(),
                                     (Node *)vle_expr, mode, location);
}

static bool parse_vle_field_arg_name(FuncCall *fn, NodeTag arg_type,
                                     bool allow_type, char **field_name)
{
    return parse_single_unary_func_arg_name(fn, arg_type, field_name) &&
        is_vle_field_name(*field_name, allow_type);
}

static bool parse_vle_field_arg(FuncCall *fn, NodeTag arg_type,
                                bool allow_type, Node **arg,
                                char **field_name)
{
    if (!parse_vle_field_arg_name(fn, arg_type, allow_type, field_name))
    {
        return false;
    }

    if (arg != NULL)
    {
        *arg = linitial(fn->args);
    }

    return true;
}

static bool parse_vle_edge_field_arg_name(FuncCall *fn, NodeTag arg_type,
                                          char **field_name)
{
    return parse_single_unary_func_arg_name(fn, arg_type, field_name) &&
        is_vle_edge_field_name(*field_name);
}

static bool parse_vle_endpoint_field_mode_offset(FuncCall *fn,
                                                 NodeTag left_type,
                                                 NodeTag right_type,
                                                 FuncCall **endpoint_fn,
                                                 int64 *mode_offset)
{
    char *field_name = NULL;
    Node *endpoint_arg = NULL;

    if (!parse_vle_field_arg(fn, T_FuncCall, false, &endpoint_arg,
                             &field_name))
    {
        return false;
    }

    *endpoint_fn = (FuncCall *)endpoint_arg;
    return get_vle_endpoint_field_mode_offset(
        field_name, *endpoint_fn, left_type, right_type, mode_offset);
}

static Node *try_transform_vle_path_nested_transform_index_endpoint_access(
    cypher_parsestate *cpstate, FuncCall *fn)
{
    if (!is_single_unary_func_arg(fn, T_A_Indirection))
    {
        return NULL;
    }

    return transform_vle_path_nested_transform_index_endpoint(
        cpstate, fn, VLE_SLICE_BOUNDARY_START_NODE_OFFSET,
        VLE_SLICE_BOUNDARY_END_NODE_OFFSET, fn->location);
}

static Node *try_transform_vle_path_boundary_endpoint_access(
    cypher_parsestate *cpstate, FuncCall *fn)
{
    char *list_name = NULL;
    Expr *vle_expr = NULL;
    Const *index_expr = NULL;
    Oid func_oid;
    bool start_endpoint;
    Node *endpoint_arg = NULL;
    int64 index;
    int64 mode;

    if (!parse_endpoint_func_arg(fn, T_FuncCall, NULL, &endpoint_arg) ||
        !parse_vle_endpoint_function_name(fn, NULL, &start_endpoint))
    {
        return NULL;
    }

    if (parse_vle_path_tail_last_list(cpstate, endpoint_arg,
                                      &vle_expr, &list_name) &&
        is_relationships_list_name(list_name))
    {
        mode = start_endpoint ? 0 : 1;
        func_oid = get_age_vle_tail_last_edge_endpoint_oid();
        return make_vle_binary_mode_expr(func_oid, (Node *)vle_expr, mode,
                                         fn->location);
    }

    if (!parse_vle_path_boundary_list_index(cpstate, endpoint_arg,
                                            &vle_expr, &list_name, &index) ||
        !is_relationships_list_name(list_name))
    {
        return NULL;
    }

    index_expr = make_agtype_integer_const(index, fn->location);
    func_oid = start_endpoint ? get_age_vle_edge_start_node_at_oid() :
        get_age_vle_edge_end_node_at_oid();

    return make_vle_binary_agtype_expr(func_oid, (Node *)vle_expr,
                                       (Node *)index_expr);
}

static Node *try_transform_vle_path_slice_boundary_endpoint_access(
    cypher_parsestate *cpstate, FuncCall *fn)
{
    return transform_vle_path_slice_boundary_endpoint(
        cpstate, fn, VLE_SLICE_BOUNDARY_START_NODE_OFFSET,
        VLE_SLICE_BOUNDARY_END_NODE_OFFSET);
}

static Node *try_transform_vle_path_endpoint_field(cypher_parsestate *cpstate,
                                                   FuncCall *fn)
{
    char *field_name = NULL;
    Expr *vle_expr = NULL;
    Node *index_expr = NULL;
    bool start_endpoint;
    Node *endpoint_arg = NULL;

    if (!parse_vle_field_arg(fn, T_FuncCall, false, &endpoint_arg,
                             &field_name) ||
        !parse_vle_edge_endpoint_index(cpstate, endpoint_arg, &vle_expr,
                                       &index_expr, &start_endpoint))
    {
        return NULL;
    }

    return make_vle_edge_endpoint_field_expr(field_name, vle_expr, index_expr,
                                             start_endpoint, fn->location);
}

static Node *try_transform_vle_path_nested_transform_index_endpoint_field(
    cypher_parsestate *cpstate, FuncCall *fn)
{
    FuncCall *endpoint_fn = NULL;
    int64 mode_offset;

    if (!parse_vle_endpoint_field_mode_offset(fn, T_A_Indirection,
                                              T_A_Indirection,
                                              &endpoint_fn, &mode_offset))
    {
        return NULL;
    }

    return transform_vle_path_nested_transform_index_endpoint(
        cpstate, endpoint_fn, mode_offset, mode_offset, fn->location);
}

static Node *try_transform_vle_path_slice_boundary_endpoint_field(
    cypher_parsestate *cpstate, FuncCall *fn)
{
    FuncCall *endpoint_fn = NULL;
    int64 mode_offset;

    if (!parse_vle_endpoint_field_mode_offset(fn, T_FuncCall,
                                              T_A_Indirection,
                                              &endpoint_fn, &mode_offset))
    {
        return NULL;
    }

    return transform_vle_path_slice_boundary_endpoint(cpstate, endpoint_fn,
                                                      mode_offset,
                                                      mode_offset);
}

static Node *try_transform_vle_tail_last_endpoint_field(
    cypher_parsestate *cpstate, FuncCall *fn)
{
    char *field_name = NULL;
    Node *endpoint_arg = NULL;
    Expr *vle_expr = NULL;
    bool start_endpoint;

    if (!parse_vle_field_arg(fn, T_FuncCall, false, &endpoint_arg,
                             &field_name))
    {
        return NULL;
    }

    if (!parse_vle_tail_last_endpoint(cpstate, endpoint_arg, &vle_expr,
                                      &start_endpoint))
    {
        return NULL;
    }

    return make_vle_tail_last_endpoint_field_expr(field_name, vle_expr,
                                                  start_endpoint,
                                                  fn->location);
}

static bool parse_vle_indexed_field_arg(FuncCall *fn, bool edge_only,
                                        bool allow_type,
                                        A_Indirection **a_ind,
                                        char **field_name)
{
    char *parsed_field_name = NULL;

    if (edge_only)
    {
        if (!parse_vle_edge_field_arg_name(fn, T_A_Indirection,
                                           &parsed_field_name))
        {
            return false;
        }
    }
    else if (!parse_vle_field_arg_name(fn, T_A_Indirection, allow_type,
                                       &parsed_field_name))
    {
        return false;
    }

    *a_ind = (A_Indirection *)linitial(fn->args);
    if (field_name != NULL)
    {
        *field_name = parsed_field_name;
    }

    return true;
}

static Node *try_transform_vle_edge_variable_field(
    cypher_parsestate *cpstate, FuncCall *fn)
{
    A_Indirection *a_ind = NULL;
    char *field_name = NULL;

    if (!parse_vle_indexed_field_arg(fn, true, false, &a_ind, &field_name))
    {
        return NULL;
    }

    return make_current_vle_edge_indexed_field_expr(cpstate, a_ind,
                                                    field_name);
}

static Node *try_transform_vle_path_node_field(cypher_parsestate *cpstate,
                                               FuncCall *fn)
{
    A_Indirection *a_ind = NULL;
    char *field_name = NULL;

    if (!parse_vle_indexed_field_arg(fn, false, false, &a_ind, &field_name))
    {
        return NULL;
    }

    return make_vle_path_indexed_field_expr(cpstate, a_ind, field_name,
                                            true, false);
}

static bool parse_vle_direct_indexed_keys_source(cypher_parsestate *cpstate,
                                                 A_Indirection *a_ind,
                                                 Expr **vle_expr,
                                                 char **list_name,
                                                 Node **index_expr)
{
    A_Indices *indices = NULL;
    bool allow_visible_path;

    if (!parse_single_indirection_value_index(a_ind, &indices))
    {
        return false;
    }

    allow_visible_path = IsA(a_ind->arg, FuncCall);
    if (!parse_vle_path_or_raw_edge_list(cpstate, a_ind->arg, vle_expr,
                                         list_name, allow_visible_path))
    {
        return false;
    }

    *index_expr = transform_cypher_expr_recurse(cpstate, indices->uidx);
    return true;
}

static Node *try_transform_vle_path_indexed_keys(cypher_parsestate *cpstate,
                                                 FuncCall *fn)
{
    A_Indirection *a_ind = NULL;
    char *list_name = NULL;
    Expr *vle_expr = NULL;
    Node *index_expr = NULL;
    FuncExpr *properties_expr = NULL;
    Node *keys_expr = NULL;
    Node *indirection_arg = NULL;

    if (!parse_keys_func_arg(fn, T_A_Indirection, &indirection_arg))
    {
        return NULL;
    }

    a_ind = (A_Indirection *)indirection_arg;
    if (!parse_vle_direct_indexed_keys_source(cpstate, a_ind, &vle_expr,
                                              &list_name, &index_expr))
    {
        if (IsA(a_ind->arg, FuncCall))
        {
            properties_expr = make_vle_index_properties_expr(
                cpstate, a_ind, fn->location);
            if (properties_expr == NULL)
            {
                return NULL;
            }

            return make_age_keys_expr((Node *)properties_expr, fn->location);
        }

        return NULL;
    }

    if (vle_expr == NULL)
        return NULL;

    keys_expr = make_vle_indexed_properties_keys_expr(
        list_name, vle_expr, index_expr, fn->location);
    if (keys_expr == NULL)
    {
        return NULL;
    }

    return keys_expr;
}

static Node *try_transform_vle_path_boundary_keys(cypher_parsestate *cpstate,
                                                  FuncCall *fn)
{
    Node *boundary_arg = NULL;
    FuncCall *boundary_fn = NULL;
    char *list_name = NULL;
    Expr *vle_expr = NULL;
    Node *index_expr = NULL;
    FuncExpr *properties_expr = NULL;
    Node *keys_expr = NULL;
    Node *boundary_source = NULL;

    if (!parse_keys_func_arg(fn, T_FuncCall, &boundary_arg))
    {
        return NULL;
    }

    boundary_fn = (FuncCall *)boundary_arg;
    if (!parse_vle_boundary_function(boundary_fn, NULL, &boundary_source,
                                     &index_expr))
    {
        return NULL;
    }

    if (IsA(boundary_source, FuncCall) ||
        IsA(boundary_source, A_Indirection))
    {
        properties_expr = (FuncExpr *)
            transform_vle_path_any_slice_boundary_head_last(
                cpstate, boundary_fn,
                VLE_SLICE_BOUNDARY_PROPERTIES_OFFSET);

        if (properties_expr != NULL)
        {
            return make_age_keys_expr((Node *)properties_expr, fn->location);
        }
    }

    if (!parse_vle_path_or_raw_edge_list(cpstate, boundary_source,
                                         &vle_expr, &list_name, true))
    {
        return NULL;
    }

    if (vle_expr == NULL)
        return NULL;

    keys_expr = make_vle_indexed_properties_keys_expr(
        list_name, vle_expr, index_expr, fn->location);
    if (keys_expr == NULL)
    {
        return NULL;
    }

    return keys_expr;
}

static Node *try_transform_vle_path_endpoint_keys(cypher_parsestate *cpstate,
                                                  FuncCall *fn)
{
    Node *endpoint_arg = NULL;
    FuncCall *endpoint_fn = NULL;
    FuncCall *boundary_fn = NULL;
    FuncExpr *properties_expr = NULL;
    char *endpoint_name = NULL;
    char *list_name = NULL;
    Expr *vle_expr = NULL;
    Node *index_expr = NULL;
    bool start_endpoint = false;
    Node *endpoint_source = NULL;
    int64 mode_offset;
    int64 index;

    if (!parse_keys_func_arg(fn, T_FuncCall, &endpoint_arg))
    {
        return NULL;
    }

    endpoint_fn = (FuncCall *)endpoint_arg;
    if (!parse_vle_endpoint_mode_offset_either_arg(
            endpoint_fn, T_FuncCall, T_A_Indirection,
            VLE_SLICE_BOUNDARY_START_PROPERTIES_OFFSET,
            VLE_SLICE_BOUNDARY_END_PROPERTIES_OFFSET, &endpoint_name,
            &start_endpoint, &mode_offset, &endpoint_source))
    {
        return NULL;
    }

    if (IsA(endpoint_source, A_Indirection))
    {
        properties_expr = (FuncExpr *)
            transform_vle_path_nested_transform_index_endpoint(
                cpstate, endpoint_fn, mode_offset, mode_offset,
                fn->location);
    }
    else
    {
        boundary_fn = (FuncCall *)endpoint_source;
        properties_expr = (FuncExpr *)
            transform_vle_path_any_slice_boundary_head_last(cpstate,
                                                            boundary_fn,
                                                            mode_offset);
    }

    if (properties_expr == NULL)
    {
        if (boundary_fn == NULL)
        {
            return NULL;
        }

        if (parse_vle_path_boundary_list_index(
                cpstate, (Node *)boundary_fn, &vle_expr, &list_name, &index) &&
            is_relationships_list_name(list_name))
        {
            index_expr = (Node *)make_agtype_integer_const(index,
                                                           fn->location);
        }
        else if (!parse_visible_vle_boundary_endpoint(
                     cpstate, (Node *)endpoint_fn, &vle_expr, &index_expr,
                     &start_endpoint))
        {
            return NULL;
        }

        properties_expr = (FuncExpr *)make_vle_edge_endpoint_field_expr(
            "properties", vle_expr, index_expr, start_endpoint, fn->location);
        if (properties_expr == NULL)
            return NULL;
    }

    return make_age_keys_expr((Node *)properties_expr, fn->location);
}

static bool parse_visible_vle_boundary_list(cypher_parsestate *cpstate,
                                            FuncCall *boundary_fn,
                                            char **list_name,
                                            Expr **vle_expr,
                                            Node **index_expr)
{
    FuncCall *list_fn = NULL;
    Node *boundary_arg = NULL;
    Node *path_arg = NULL;

    if (!parse_vle_boundary_function(boundary_fn, NULL,
                                     &boundary_arg, index_expr))
    {
        return false;
    }

    if (IsA(boundary_arg, FuncCall))
    {
        list_fn = (FuncCall *)boundary_arg;
        if (!parse_path_list_func_arg(list_fn, T_ColumnRef, list_name,
                                      &path_arg))
        {
            return false;
        }

        *vle_expr = get_visible_single_vle_path_expr(cpstate, path_arg);
    }
    else
    {
        *list_name = "relationships";
        *vle_expr = get_visible_vle_edge_expr(cpstate,
                                              boundary_arg);
    }

    return *vle_expr != NULL && exprType((Node *)*vle_expr) == AGTYPEOID;
}

static Node *try_transform_vle_path_visible_boundary_id_function(
    cypher_parsestate *cpstate, FuncCall *fn)
{
    Node *boundary_arg = NULL;
    char *list_name = NULL;
    Expr *vle_expr = NULL;
    Node *index_expr = NULL;
    Oid func_oid;

    if (!parse_id_func_arg(fn, T_FuncCall, &boundary_arg))
    {
        return NULL;
    }

    if (!parse_visible_vle_boundary_list(cpstate, (FuncCall *)boundary_arg,
                                         &list_name, &vle_expr, &index_expr))
        return NULL;

    func_oid = get_vle_indexed_id_func_oid(list_name);
    return make_vle_binary_agtype_expr(func_oid, (Node *)vle_expr,
                                       index_expr);
}

static Node *try_transform_vle_path_visible_boundary_field(
    cypher_parsestate *cpstate, FuncCall *fn)
{
    FuncCall *boundary_fn = NULL;
    char *field_name = NULL;
    char *list_name = NULL;
    Node *boundary_arg = NULL;
    Expr *vle_expr = NULL;
    Node *index_expr = NULL;
    Oid func_oid;

    if (!parse_vle_field_arg(fn, T_FuncCall, true, &boundary_arg,
                             &field_name))
    {
        return NULL;
    }

    boundary_fn = (FuncCall *)boundary_arg;
    if (!parse_visible_vle_boundary_list(cpstate, boundary_fn, &list_name,
                                         &vle_expr, &index_expr) ||
        !get_vle_indexed_field_func_oid(list_name, field_name, &func_oid))
        return NULL;

    return make_vle_binary_agtype_expr(func_oid, (Node *)vle_expr,
                                       index_expr);
}

static bool parse_visible_vle_boundary_endpoint(
    cypher_parsestate *cpstate, Node *node, Expr **vle_expr,
    Node **index_expr, bool *start_endpoint)
{
    FuncCall *endpoint_fn = NULL;
    FuncCall *boundary_fn = NULL;
    Node *boundary_arg = NULL;
    char *list_name = NULL;

    if (!IsA(node, FuncCall))
        return false;

    endpoint_fn = (FuncCall *)node;
    if (!parse_endpoint_func_arg(endpoint_fn, T_FuncCall, NULL,
                                 &boundary_arg) ||
        !parse_vle_endpoint_function_name(endpoint_fn, NULL, start_endpoint))
    {
        return false;
    }

    boundary_fn = (FuncCall *)boundary_arg;
    return parse_visible_vle_boundary_list(cpstate, boundary_fn, &list_name,
                                           vle_expr, index_expr) &&
        is_relationships_list_name(list_name);
}

static Node *transform_visible_vle_boundary_endpoint_index(
    cypher_parsestate *cpstate, Node *node, Oid start_func_oid,
    Oid end_func_oid)
{
    Expr *vle_expr = NULL;
    Node *index_expr = NULL;
    bool start_endpoint;

    if (!parse_visible_vle_boundary_endpoint(cpstate, node, &vle_expr,
                                             &index_expr,
                                             &start_endpoint))
    {
        return NULL;
    }

    return make_vle_edge_endpoint_index_expr(vle_expr, index_expr,
                                             start_endpoint, start_func_oid,
                                             end_func_oid);
}

static Node *try_transform_vle_path_visible_boundary_endpoint_id(
    cypher_parsestate *cpstate, FuncCall *fn)
{
    Node *endpoint_arg = NULL;

    if (!parse_id_func_arg(fn, T_FuncCall, &endpoint_arg))
    {
        return NULL;
    }

    return transform_visible_vle_boundary_endpoint_index(
        cpstate, endpoint_arg, get_age_vle_edge_start_id_at_oid(),
        get_age_vle_edge_end_id_at_oid());
}

static Node *try_transform_vle_path_visible_boundary_endpoint_field(
    cypher_parsestate *cpstate, FuncCall *fn)
{
    char *field_name = NULL;
    Node *endpoint_arg = NULL;
    Expr *vle_expr = NULL;
    Node *index_expr = NULL;
    bool start_endpoint;

    if (!parse_vle_field_arg(fn, T_FuncCall, false, &endpoint_arg,
                             &field_name))
    {
        return NULL;
    }

    if (!parse_visible_vle_boundary_endpoint(cpstate, endpoint_arg,
                                             &vle_expr, &index_expr,
                                             &start_endpoint))
    {
        return NULL;
    }

    return make_vle_edge_endpoint_field_expr(field_name, vle_expr, index_expr,
                                             start_endpoint, fn->location);
}

static Node *try_transform_vle_path_visible_boundary_endpoint_access(
    cypher_parsestate *cpstate, FuncCall *fn)
{
    Expr *vle_expr = NULL;
    Node *index_expr = NULL;
    bool start_endpoint;
    Oid func_oid;

    if (!parse_visible_vle_boundary_endpoint(cpstate, (Node *)fn, &vle_expr,
                                             &index_expr, &start_endpoint))
    {
        return NULL;
    }

    func_oid = start_endpoint ? get_age_vle_edge_start_node_at_oid() :
        get_age_vle_edge_end_node_at_oid();

    return make_vle_binary_agtype_expr(func_oid, (Node *)vle_expr,
                                       index_expr);
}

static bool parse_vle_path_boundary_list_index(cypher_parsestate *cpstate,
                                               Node *node, Expr **vle_expr,
                                               char **list_name,
                                               int64 *index)
{
    FuncCall *outer_fn = NULL;
    FuncCall *inner_fn = NULL;
    FuncCall *list_fn = NULL;
    Node *inner_arg = NULL;
    Node *list_arg = NULL;
    char *outer_name = NULL;
    char *inner_name = NULL;
    bool tail_last = false;

    if (!IsA(node, FuncCall))
    {
        return false;
    }

    outer_fn = (FuncCall *)node;
    if (!parse_head_last_func_name(outer_fn, &outer_name))
    {
        return false;
    }

    inner_arg = linitial(outer_fn->args);
    if (parse_arbitrary_vle_path_or_raw_edge_list(cpstate, inner_arg,
                                                  vle_expr, list_name))
    {
        *index = is_head_name(outer_name) ? 0 : -1;
        return exprType((Node *)*vle_expr) == AGTYPEOID;
    }

    if (!IsA(inner_arg, FuncCall))
    {
        return false;
    }

    inner_fn = (FuncCall *)inner_arg;
    if (!parse_tail_reverse_func_any_arg(inner_fn, &inner_name, &list_arg))
    {
        return false;
    }

    if (!IsA(list_arg, FuncCall))
    {
        return false;
    }

    list_fn = (FuncCall *)list_arg;
    if (!parse_vle_path_list_function(cpstate, (Node *)list_fn, vle_expr,
                                      list_name, true))
    {
        return false;
    }

    if (!get_vle_head_last_tail_reverse_index(outer_name, inner_name, index,
                                              &tail_last))
    {
        return false;
    }
    if (tail_last)
    {
        return false;
    }

    return *vle_expr != NULL && exprType((Node *)*vle_expr) == AGTYPEOID;
}

static bool is_vle_direct_path_list_func(Node *node)
{
    FuncCall *fn;
    char *func_name = NULL;

    if (!IsA(node, FuncCall))
        return false;

    fn = (FuncCall *)node;
    return parse_path_list_func_name(fn, &func_name);
}

static bool parse_vle_edge_endpoint_index(cypher_parsestate *cpstate,
                                           Node *node, Expr **vle_expr,
                                           Node **index_expr,
                                           bool *start_endpoint)
{
    FuncCall *endpoint_fn = NULL;
    Node *endpoint_arg = NULL;
    char *list_name = NULL;
    int64 boundary_index;

    if (!IsA(node, FuncCall))
    {
        return false;
    }

    endpoint_fn = (FuncCall *)node;
    if (!parse_endpoint_func_any_arg(endpoint_fn, NULL, start_endpoint,
                                     &endpoint_arg))
    {
        return false;
    }

    if (IsA(endpoint_arg, A_Indirection))
    {
        A_Indirection *a_ind = (A_Indirection *)endpoint_arg;

        if (!is_vle_direct_path_list_func(a_ind->arg) &&
            parse_vle_path_indexed_list_index(cpstate, a_ind, vle_expr,
                                              &list_name, index_expr))
        {
            return is_relationships_list_name(list_name);
        }

        if (parse_current_vle_edge_indirection_index(cpstate, a_ind,
                                                     vle_expr, index_expr))
        {
            return true;
        }

        return false;
    }

    if (!parse_vle_path_boundary_list_index(cpstate, endpoint_arg,
                                            vle_expr, &list_name,
                                            &boundary_index) ||
        !is_relationships_list_name(list_name))
    {
        return false;
    }

    *index_expr = (Node *)make_agtype_integer_const(
        boundary_index, exprLocation(endpoint_arg));

    return true;
}

static bool get_vle_endpoint_mode_offset(const char *endpoint_name,
                                         int64 start_offset,
                                         int64 end_offset,
                                         int64 *mode_offset)
{
    if (is_start_endpoint_function_name(endpoint_name))
    {
        *mode_offset = start_offset;
        return true;
    }
    if (is_endpoint_function_name(endpoint_name))
    {
        *mode_offset = end_offset;
        return true;
    }

    return false;
}

static bool parse_vle_endpoint_mode_offset_either(FuncCall *endpoint_fn,
                                                  NodeTag left_type,
                                                  NodeTag right_type,
                                                  int64 start_offset,
                                                  int64 end_offset,
                                                  char **endpoint_name,
                                                  bool *start_endpoint,
                                                  int64 *mode_offset)
{
    char *parsed_name = NULL;

    if (!is_unary_func_arg_either(endpoint_fn, left_type, right_type) ||
        !parse_vle_endpoint_function_name(endpoint_fn, &parsed_name,
                                          start_endpoint) ||
        !get_vle_endpoint_mode_offset(parsed_name, start_offset, end_offset,
                                      mode_offset))
    {
        return false;
    }

    if (endpoint_name != NULL)
    {
        *endpoint_name = parsed_name;
    }

    return true;
}

static bool parse_vle_endpoint_mode_offset_either_arg(FuncCall *endpoint_fn,
                                                      NodeTag left_type,
                                                      NodeTag right_type,
                                                      int64 start_offset,
                                                      int64 end_offset,
                                                      char **endpoint_name,
                                                      bool *start_endpoint,
                                                      int64 *mode_offset,
                                                      Node **arg)
{
    if (!parse_vle_endpoint_mode_offset_either(
            endpoint_fn, left_type, right_type, start_offset, end_offset,
            endpoint_name, start_endpoint, mode_offset))
    {
        return false;
    }

    if (arg != NULL)
    {
        *arg = linitial(endpoint_fn->args);
    }

    return true;
}

typedef enum VLEFieldModeKind
{
    VLE_FIELD_MODE_LABEL_TYPE,
    VLE_FIELD_MODE_LABELS,
    VLE_FIELD_MODE_PROPERTIES
} VLEFieldModeKind;

static bool get_vle_field_mode_kind(const char *field_name, bool allow_type,
                                    VLEFieldModeKind *kind)
{
    if (is_label_name(field_name) || (allow_type && is_type_name(field_name)))
    {
        *kind = VLE_FIELD_MODE_LABEL_TYPE;
        return true;
    }
    if (is_labels_name(field_name))
    {
        *kind = VLE_FIELD_MODE_LABELS;
        return true;
    }
    if (is_properties_name(field_name))
    {
        *kind = VLE_FIELD_MODE_PROPERTIES;
        return true;
    }

    return false;
}

static bool is_vle_field_name(const char *field_name, bool allow_type)
{
    VLEFieldModeKind kind;

    return get_vle_field_mode_kind(field_name, allow_type, &kind);
}

static bool get_vle_endpoint_field_mode(const char *field_name,
                                        bool start_endpoint, int64 *mode)
{
    VLEFieldModeKind kind;

    if (!get_vle_field_mode_kind(field_name, false, &kind))
    {
        return false;
    }

    switch (kind)
    {
    case VLE_FIELD_MODE_LABEL_TYPE:
        *mode = start_endpoint ? 0 : 1;
        break;
    case VLE_FIELD_MODE_LABELS:
        *mode = start_endpoint ? 2 : 3;
        break;
    case VLE_FIELD_MODE_PROPERTIES:
        *mode = start_endpoint ? 4 : 5;
        break;
    default:
        return false;
    }

    return true;
}

static bool get_vle_endpoint_field_mode_offset(const char *field_name,
                                               FuncCall *endpoint_fn,
                                               NodeTag left_type,
                                               NodeTag right_type,
                                               int64 *mode_offset)
{
    int64 start_offset;
    int64 end_offset;

    if (!get_vle_field_mode_offset(field_name,
                                   VLE_SLICE_BOUNDARY_START_LABEL_OFFSET,
                                   VLE_SLICE_BOUNDARY_START_LABELS_OFFSET,
                                   VLE_SLICE_BOUNDARY_START_PROPERTIES_OFFSET,
                                   false, &start_offset) ||
        !get_vle_field_mode_offset(field_name,
                                   VLE_SLICE_BOUNDARY_END_LABEL_OFFSET,
                                   VLE_SLICE_BOUNDARY_END_LABELS_OFFSET,
                                   VLE_SLICE_BOUNDARY_END_PROPERTIES_OFFSET,
                                   false, &end_offset))
    {
        return false;
    }

    return parse_vle_endpoint_mode_offset_either(
        endpoint_fn, left_type, right_type, start_offset, end_offset, NULL,
        NULL, mode_offset);
}

static bool get_vle_field_mode_offset(const char *field_name,
                                      int64 label_type_offset,
                                      int64 labels_offset,
                                      int64 properties_offset,
                                      bool allow_type,
                                      int64 *mode_offset)
{
    VLEFieldModeKind kind;

    if (!get_vle_field_mode_kind(field_name, allow_type, &kind))
    {
        return false;
    }

    switch (kind)
    {
    case VLE_FIELD_MODE_LABEL_TYPE:
        *mode_offset = label_type_offset;
        break;
    case VLE_FIELD_MODE_LABELS:
        *mode_offset = labels_offset;
        break;
    case VLE_FIELD_MODE_PROPERTIES:
        *mode_offset = properties_offset;
        break;
    default:
        return false;
    }

    return true;
}

static bool get_vle_tail_last_field_mode(const char *list_name,
                                         const char *field_name, int64 *mode)
{
    VLEFieldModeKind kind;

    if (!get_vle_field_mode_kind(field_name, true, &kind))
    {
        return false;
    }

    if (is_nodes_list_name(list_name))
    {
        switch (kind)
        {
        case VLE_FIELD_MODE_LABEL_TYPE:
            if (is_type_name(field_name))
            {
                return false;
            }
            *mode = 0;
            return true;
        case VLE_FIELD_MODE_LABELS:
            *mode = 1;
            return true;
        case VLE_FIELD_MODE_PROPERTIES:
            *mode = 2;
            return true;
        default:
            return false;
        }
    }

    if (is_relationships_list_name(list_name))
    {
        switch (kind)
        {
        case VLE_FIELD_MODE_LABEL_TYPE:
            *mode = 3;
            return true;
        case VLE_FIELD_MODE_PROPERTIES:
            *mode = 4;
            return true;
        default:
            return false;
        }
    }

    return false;
}

static bool get_vle_indexed_field_func_oid(const char *list_name,
                                           const char *field_name,
                                           Oid *func_oid)
{
    VLEFieldModeKind kind;

    if (!get_vle_field_mode_kind(field_name, true, &kind))
    {
        return false;
    }

    if (is_nodes_list_name(list_name))
    {
        switch (kind)
        {
        case VLE_FIELD_MODE_LABEL_TYPE:
            if (is_type_name(field_name))
            {
                return false;
            }
            *func_oid = get_age_vle_node_label_at_oid();
            return true;
        case VLE_FIELD_MODE_LABELS:
            *func_oid = get_age_vle_node_labels_at_oid();
            return true;
        case VLE_FIELD_MODE_PROPERTIES:
            *func_oid = get_age_vle_node_properties_at_oid();
            return true;
        default:
            return false;
        }
    }

    if (is_relationships_list_name(list_name))
    {
        switch (kind)
        {
        case VLE_FIELD_MODE_LABEL_TYPE:
            *func_oid = get_age_vle_edge_label_at_oid();
            return true;
        case VLE_FIELD_MODE_PROPERTIES:
            *func_oid = get_age_vle_edge_properties_at_oid();
            return true;
        default:
            return false;
        }
    }

    return false;
}

static Oid get_vle_indexed_id_func_oid(const char *list_name)
{
    if (is_nodes_list_name(list_name))
        return get_age_vle_node_id_at_oid();

    return get_age_vle_edge_id_at_oid();
}

static Node *make_vle_indexed_field_expr(const char *list_name,
                                         const char *field_name,
                                         Expr *vle_expr, Node *index_expr)
{
    Oid func_oid;

    if (!get_vle_indexed_field_func_oid(list_name, field_name, &func_oid))
    {
        return NULL;
    }

    return make_vle_binary_agtype_expr(func_oid, (Node *)vle_expr,
                                       index_expr);
}

static Node *make_vle_path_indexed_field_expr(cypher_parsestate *cpstate,
                                              A_Indirection *a_ind,
                                              const char *field_name,
                                              bool require_nodes,
                                              bool require_relationships)
{
    char *list_name = NULL;
    Expr *vle_expr = NULL;
    Node *index_expr = NULL;

    if (!parse_vle_path_indexed_list_index(cpstate, a_ind, &vle_expr,
                                           &list_name, &index_expr))
    {
        return NULL;
    }

    if ((require_nodes && !is_nodes_list_name(list_name)) ||
        (require_relationships && !is_relationships_list_name(list_name)))
    {
        return NULL;
    }

    return make_vle_indexed_field_expr(list_name, field_name, vle_expr,
                                       index_expr);
}

static Node *make_current_vle_edge_indexed_field_expr(
    cypher_parsestate *cpstate, A_Indirection *a_ind, const char *field_name)
{
    Expr *vle_expr = NULL;
    Node *index_expr = NULL;

    if (!parse_current_vle_edge_indirection_index(cpstate, a_ind, &vle_expr,
                                                  &index_expr))
    {
        return NULL;
    }

    return make_vle_indexed_field_expr("relationships", field_name,
                                       vle_expr, index_expr);
}

static Node *make_vle_indexed_properties_keys_expr(const char *list_name,
                                                   Expr *vle_expr,
                                                   Node *index_expr,
                                                   int location)
{
    Node *properties_expr;

    properties_expr = make_vle_indexed_field_expr(
        list_name, "properties", vle_expr, index_expr);
    if (properties_expr == NULL)
    {
        return NULL;
    }

    return make_age_keys_expr(properties_expr, location);
}

static bool parse_vle_path_indexed_list_index(cypher_parsestate *cpstate,
                                              A_Indirection *a_ind,
                                              Expr **vle_expr,
                                              char **list_name,
                                              Node **index_expr)
{
    FuncCall *outer_fn = NULL;
    A_Indices *indices = NULL;
    Node *outer_arg = NULL;
    char *outer_name = NULL;
    int64 constant_index;

    if (!IsA(a_ind->arg, FuncCall) ||
        !parse_single_indirection_value_index(a_ind, &indices))
    {
        return false;
    }

    outer_fn = (FuncCall *)a_ind->arg;
    if (parse_path_list_func_name(outer_fn, &outer_name))
    {
        return false;
    }

    if (!parse_tail_reverse_func_any_arg(outer_fn, &outer_name, &outer_arg) ||
        !parse_nonnegative_index(indices, &constant_index))
    {
        return false;
    }

    if (!parse_vle_path_or_raw_edge_list(cpstate, outer_arg,
                                         vle_expr, list_name, true))
    {
        return false;
    }

    if (*vle_expr == NULL)
    {
        return false;
    }

    *index_expr = (Node *)make_agtype_integer_const(
        get_vle_tail_reverse_access_index(is_tail_name(outer_name),
                                          constant_index),
        exprLocation(indices->uidx));

    return true;
}

static bool parse_arbitrary_vle_path_indexed_list_index(
    cypher_parsestate *cpstate, A_Indirection *a_ind, Expr **vle_expr,
    char **list_name, Node **index_expr)
{
    A_Indices *indices = NULL;

    if (!parse_single_indirection_value_index(a_ind, &indices))
    {
        return false;
    }

    if (!parse_arbitrary_vle_path_list_function(cpstate, a_ind->arg,
                                                vle_expr, list_name))
    {
        return false;
    }

    if (*vle_expr == NULL)
    {
        return false;
    }

    *index_expr = transform_cypher_expr_recurse(cpstate, indices->uidx);

    return true;
}

static Node *try_transform_vle_path_boundary_field(cypher_parsestate *cpstate,
                                                   FuncCall *fn)
{
    Node *field_arg = NULL;
    char *field_name = NULL;
    char *list_name = NULL;
    Expr *vle_expr = NULL;
    Const *index_expr = NULL;
    int64 index;

    if (!parse_vle_field_arg(fn, T_FuncCall, true, &field_arg, &field_name))
    {
        return NULL;
    }

    if (parse_vle_path_tail_last_list(cpstate, field_arg, &vle_expr,
                                      &list_name))
    {
        return make_vle_tail_last_field_expr(list_name, field_name, vle_expr,
                                             fn->location);
    }

    if (!parse_vle_path_boundary_list_index(cpstate, field_arg,
                                            &vle_expr, &list_name, &index))
    {
        return NULL;
    }

    index_expr = make_agtype_integer_const(index, fn->location);
    return make_vle_indexed_field_expr(list_name, field_name, vle_expr,
                                       (Node *)index_expr);
}

static bool parse_vle_field_mode_offset_arg(FuncCall *fn, NodeTag arg_type,
                                            bool allow_type, Node **arg,
                                            char **field_name,
                                            int64 *mode_offset)
{
    char *parsed_field_name = NULL;

    if (!parse_vle_field_arg_name(fn, arg_type, allow_type,
                                  &parsed_field_name) ||
        !get_vle_field_mode_offset(parsed_field_name,
                                   VLE_SLICE_BOUNDARY_LABEL_OFFSET,
                                   VLE_SLICE_BOUNDARY_LABELS_OFFSET,
                                   VLE_SLICE_BOUNDARY_PROPERTIES_OFFSET,
                                   allow_type, mode_offset))
    {
        return false;
    }

    if (arg != NULL)
    {
        *arg = linitial(fn->args);
    }
    if (field_name != NULL)
    {
        *field_name = parsed_field_name;
    }

    return true;
}

static Node *try_transform_vle_path_slice_boundary_field(
    cypher_parsestate *cpstate, FuncCall *fn)
{
    FuncCall *boundary_fn = NULL;
    Node *boundary_arg = NULL;
    int64 mode_offset;

    if (!parse_vle_field_mode_offset_arg(fn, T_FuncCall, true,
                                         &boundary_arg, NULL, &mode_offset))
    {
        return NULL;
    }

    boundary_fn = (FuncCall *)boundary_arg;
    return transform_vle_path_any_slice_boundary_head_last(cpstate,
                                                           boundary_fn,
                                                           mode_offset);
}

static Node *try_transform_vle_path_nested_transform_index_field(
    cypher_parsestate *cpstate, FuncCall *fn)
{
    A_Indirection *a_ind = NULL;
    Node *indirection_arg = NULL;
    char *list_name = NULL;
    char *field_name = NULL;
    int64 mode_offset;
    Node *field_expr = NULL;

    if (!parse_vle_field_mode_offset_arg(fn, T_A_Indirection, true,
                                         &indirection_arg, &field_name,
                                         &mode_offset))
    {
        return NULL;
    }

    a_ind = (A_Indirection *)indirection_arg;
    field_expr = make_vle_nested_transform_index_mode_expr(
        cpstate, a_ind, mode_offset, fn->location, &list_name, false);
    if (field_expr == NULL)
    {
        return NULL;
    }

    {
        int64 ignored_mode;

        if (!get_vle_tail_last_field_mode(list_name, field_name,
                                          &ignored_mode))
        {
            return NULL;
        }
    }

    return field_expr;
}

static Node *try_transform_vle_path_edge_label(cypher_parsestate *cpstate,
                                               FuncCall *fn)
{
    A_Indirection *a_ind = NULL;
    char *field_name = NULL;

    if (!parse_vle_indexed_field_arg(fn, true, false, &a_ind, &field_name) ||
        is_properties_name(field_name))
    {
        return NULL;
    }

    return make_vle_path_indexed_field_expr(cpstate, a_ind, field_name,
                                            false, true);
}

static Node *try_transform_vle_path_edge_properties(
    cypher_parsestate *cpstate, FuncCall *fn)
{
    A_Indirection *a_ind = NULL;
    char *field_name = NULL;

    if (!parse_vle_indexed_field_arg(fn, true, false, &a_ind, &field_name) ||
        !is_properties_name(field_name))
    {
        return NULL;
    }

    return make_vle_path_indexed_field_expr(cpstate, a_ind, field_name,
                                            false, false);
}

static bool parse_vle_tail_reverse_list_slice(cypher_parsestate *cpstate,
                                               FuncCall *outer_fn,
                                               Expr **vle_expr,
                                               int64 *mode,
                                               bool arbitrary_path)
{
    char *outer_name = NULL;
    Node *source_arg = NULL;

    if (!parse_tail_reverse_func_any_arg(outer_fn, &outer_name, &source_arg))
    {
        return false;
    }

    return parse_vle_tail_reverse_source_slice_mode(
        cpstate, outer_name, source_arg, vle_expr, mode, arbitrary_path);
}

static bool parse_vle_path_list_slice(cypher_parsestate *cpstate,
                                      A_Indirection *a_ind,
                                      Expr **vle_expr,
                                      Node **lower_expr,
                                      Node **upper_expr,
                                      int64 *mode,
                                      bool arbitrary_path)
{
    A_Indices *indices = NULL;
    FuncCall *outer_fn = NULL;
    char *outer_name = NULL;
    char *list_name = NULL;

    if (!parse_single_indirection_slice(a_ind, &indices))
    {
        return false;
    }

    if (IsA(a_ind->arg, FuncCall))
    {
        outer_fn = (FuncCall *)a_ind->arg;
        if (parse_path_list_func_name(outer_fn, &outer_name))
        {
            if (arbitrary_path)
            {
                if (!parse_arbitrary_vle_path_list_function(
                        cpstate, (Node *)outer_fn, vle_expr, &list_name))
                {
                    return false;
                }

                *mode = is_nodes_list_name(list_name) ? 1 : 0;
            }
            else if (!parse_vle_path_or_raw_edge_list_mode(
                         cpstate, (Node *)outer_fn, vle_expr, &list_name, 1,
                         0, mode, true))
            {
                return false;
            }
        }
        else if (!parse_tail_reverse_func_name(outer_fn, &outer_name) ||
                 !parse_vle_tail_reverse_list_slice(cpstate, outer_fn,
                                                    vle_expr, mode,
                                                    arbitrary_path))
        {
            return false;
        }
    }
    else
    {
        if (!parse_vle_path_or_raw_edge_list_mode(cpstate, a_ind->arg,
                                                  vle_expr, &list_name, 0, 0,
                                                  mode, false))
        {
            return false;
        }
    }

    if (*vle_expr == NULL)
    {
        return false;
    }

    transform_slice_bounds_or_null(cpstate, indices, lower_expr, upper_expr);

    return true;
}

static Node *transform_vle_path_slice_count(cypher_parsestate *cpstate,
                                            FuncCall *fn,
                                            const char *func_name,
                                            bool is_empty)
{
    A_Indirection *a_ind = NULL;
    Node *indirection_arg = NULL;
    Expr *vle_expr = NULL;
    Node *lower_expr = NULL;
    Node *upper_expr = NULL;
    int64 mode = 0;

    if (!parse_func_arg(fn, func_name, T_A_Indirection, &indirection_arg))
    {
        return NULL;
    }

    a_ind = (A_Indirection *)indirection_arg;
    if (!parse_vle_path_list_slice(cpstate, a_ind, &vle_expr, &lower_expr,
                                   &upper_expr, &mode, true))
    {
        return NULL;
    }

    return make_vle_list_slice_count_expr((Node *)vle_expr, lower_expr,
                                          upper_expr, mode, fn->location,
                                          is_empty);
}

static Node *try_transform_vle_path_slice_size(cypher_parsestate *cpstate,
                                               FuncCall *fn)
{
    return transform_vle_path_slice_count(cpstate, fn, "size", false);
}

static Node *try_transform_vle_path_slice_is_empty(cypher_parsestate *cpstate,
                                                   FuncCall *fn)
{
    return transform_vle_path_slice_count(cpstate, fn, "isEmpty", true);
}

static Node *transform_vle_path_slice_head_last(cypher_parsestate *cpstate,
                                                FuncCall *fn,
                                                int64 mode_offset)
{
    A_Indirection *a_ind = NULL;
    A_Indices *indices = NULL;
    Expr *vle_expr = NULL;
    Node *lower_expr = NULL;
    Node *upper_expr = NULL;
    char *head_last_name = NULL;
    Node *head_last_arg = NULL;
    int64 mode = 0;
    int64 mode_flag = 0;
    bool last = false;

    if (!parse_head_last_func_arg(fn, T_A_Indirection, &head_last_name,
                                  &head_last_arg))
    {
        return NULL;
    }
    last = is_last_name(head_last_name);

    a_ind = (A_Indirection *)head_last_arg;
    if (!parse_single_indirection_slice(a_ind, &indices))
    {
        return NULL;
    }

    if (!parse_vle_slice_boundary_source_mode(cpstate, a_ind->arg,
                                              &vle_expr, &mode,
                                              &mode_flag))
    {
        return NULL;
    }

    if (vle_expr == NULL)
    {
        return NULL;
    }

    if (last)
    {
        mode++;
    }

    transform_slice_bounds_or_null(cpstate, indices, &lower_expr, &upper_expr);

    mode += mode_offset;
    mode += mode_flag;
    return make_vle_slice_boundary_mode_expr((Node *)vle_expr, lower_expr,
                                             upper_expr, mode, fn->location);
}

static Node *try_transform_vle_path_slice_head_last(cypher_parsestate *cpstate,
                                                    FuncCall *fn)
{
    return transform_vle_path_slice_head_last(cpstate, fn, 0);
}

static Node *try_transform_vle_path_slice_boundary_id_function(
    cypher_parsestate *cpstate, FuncCall *fn)
{
    Node *boundary_arg = NULL;

    if (!parse_id_func_arg(fn, T_FuncCall, &boundary_arg))
    {
        return NULL;
    }

    return transform_vle_path_slice_head_last(
        cpstate, (FuncCall *)boundary_arg, VLE_SLICE_BOUNDARY_ID_OFFSET);
}

static Node *try_transform_vle_path_nested_transform_index_id_function(
    cypher_parsestate *cpstate, FuncCall *fn)
{
    Node *indirection_arg = NULL;

    if (!parse_id_func_arg(fn, T_A_Indirection, &indirection_arg))
    {
        return NULL;
    }

    return make_vle_nested_transform_index_mode_expr(
        cpstate, (A_Indirection *)indirection_arg,
        VLE_SLICE_BOUNDARY_ID_OFFSET, fn->location, NULL, false);
}

static bool parse_vle_path_list_function(cypher_parsestate *cpstate,
                                         Node *node, Expr **vle_expr,
                                         char **list_name,
                                         bool allow_visible)
{
    FuncCall *list_fn = NULL;
    Node *path_arg = NULL;

    *vle_expr = NULL;
    *list_name = NULL;

    if (!IsA(node, FuncCall))
    {
        return false;
    }

    list_fn = (FuncCall *)node;
    if (!parse_path_list_func_arg(list_fn, T_ColumnRef, list_name,
                                  &path_arg))
    {
        return false;
    }

    if (allow_visible)
    {
        *vle_expr = get_any_single_vle_path_expr(cpstate, path_arg);
    }
    else
    {
        *vle_expr = get_current_any_single_vle_path_expr(
            cpstate, path_arg);
    }

    return *vle_expr != NULL;
}

static bool parse_arbitrary_vle_path_list_function(
    cypher_parsestate *cpstate, Node *node, Expr **vle_expr,
    char **list_name)
{
    FuncCall *list_fn = NULL;
    Node *path_arg = NULL;

    *vle_expr = NULL;
    *list_name = NULL;

    if (!IsA(node, FuncCall))
    {
        return false;
    }

    list_fn = (FuncCall *)node;
    if (!parse_path_list_func_arg(list_fn, T_ColumnRef, list_name,
                                  &path_arg))
    {
        return false;
    }

    *vle_expr = get_arbitrary_single_vle_path_expr(cpstate, path_arg);

    return *vle_expr != NULL;
}

static bool parse_arbitrary_vle_path_or_raw_edge_list(
    cypher_parsestate *cpstate, Node *node, Expr **vle_expr,
    char **list_name)
{
    *vle_expr = NULL;
    *list_name = NULL;

    if (IsA(node, FuncCall))
    {
        return parse_arbitrary_vle_path_list_function(cpstate, node,
                                                      vle_expr, list_name);
    }

    *vle_expr = get_any_vle_edge_expr(cpstate, node);
    if (*vle_expr != NULL)
    {
        *list_name = "relationships";
        return true;
    }

    return false;
}

static bool parse_current_vle_path_list_function(cypher_parsestate *cpstate,
                                                 Node *node,
                                                 Expr **vle_expr,
                                                 char **list_name)
{
    return parse_vle_path_list_function(cpstate, node, vle_expr, list_name,
                                        false);
}

static bool parse_current_or_raw_vle_list(cypher_parsestate *cpstate,
                                          Node *node,
                                          Expr **vle_expr,
                                          char **list_name)
{
    return parse_vle_path_or_raw_edge_list(cpstate, node, vle_expr, list_name,
                                           false);
}

static bool parse_vle_path_or_raw_edge_list(
    cypher_parsestate *cpstate, Node *node, Expr **vle_expr,
    char **list_name, bool allow_visible_path)
{
    *vle_expr = NULL;
    *list_name = NULL;

    if (IsA(node, FuncCall))
    {
        if (allow_visible_path)
        {
            return parse_arbitrary_vle_path_list_function(cpstate, node,
                                                          vle_expr,
                                                          list_name);
        }

        return parse_vle_path_list_function(cpstate, node, vle_expr,
                                            list_name, allow_visible_path);
    }

    *vle_expr = get_any_vle_edge_expr(cpstate, node);
    if (*vle_expr != NULL)
    {
        *list_name = "relationships";
        return true;
    }

    return false;
}

static bool parse_vle_path_or_raw_edge_list_mode(
    cypher_parsestate *cpstate, Node *node, Expr **vle_expr,
    char **list_name, int64 nodes_mode, int64 relationships_mode,
    int64 *mode, bool allow_visible_path)
{
    char *parsed_list_name = NULL;

    if (!parse_vle_path_or_raw_edge_list(cpstate, node, vle_expr,
                                         &parsed_list_name,
                                         allow_visible_path))
    {
        return false;
    }

    if (list_name != NULL)
    {
        *list_name = parsed_list_name;
    }
    *mode = is_nodes_list_name(parsed_list_name) ? nodes_mode :
        relationships_mode;

    return true;
}

static bool parse_current_or_raw_vle_nested_list_slice_mode(
    cypher_parsestate *cpstate, Node *node, const char *outer_name,
    const char *inner_name, Expr **vle_expr, int64 *mode,
    bool arbitrary_path)
{
    char *list_name = NULL;

    if (arbitrary_path && IsA(node, FuncCall))
    {
        if (!parse_arbitrary_vle_path_list_function(cpstate, node, vle_expr,
                                                    &list_name))
        {
            return false;
        }

        *mode = is_nodes_list_name(list_name) ? 1 : 0;
    }
    else if (!parse_vle_path_or_raw_edge_list_mode(cpstate, node, vle_expr,
                                                   &list_name, 1, 0, mode,
                                                   true))
    {
        return false;
    }

    return get_vle_nested_list_slice_mode(
        outer_name, inner_name, is_nodes_list_name(list_name), mode);
}

static bool parse_current_or_raw_vle_tail_reverse_list_slice_mode(
    cypher_parsestate *cpstate, Node *node, const char *outer_name,
    Expr **vle_expr, int64 *mode, bool arbitrary_path)
{
    if (IsA(node, FuncCall))
    {
        if (is_tail_name(outer_name))
        {
            if (arbitrary_path)
            {
                char *list_name = NULL;

                if (!parse_arbitrary_vle_path_list_function(
                        cpstate, node, vle_expr, &list_name))
                {
                    return false;
                }

                *mode = is_nodes_list_name(list_name) ? 3 : 2;
                return true;
            }

            return parse_vle_path_or_raw_edge_list_mode(
                cpstate, node, vle_expr, NULL, 3, 2, mode, true);
        }

        if (is_reverse_name(outer_name))
        {
            if (arbitrary_path)
            {
                char *list_name = NULL;

                if (!parse_arbitrary_vle_path_list_function(
                        cpstate, node, vle_expr, &list_name))
                {
                    return false;
                }

                *mode = is_nodes_list_name(list_name) ? 5 : 4;
                return true;
            }

            return parse_vle_path_or_raw_edge_list_mode(
                cpstate, node, vle_expr, NULL, 5, 4, mode, true);
        }

        return false;
    }

    return parse_vle_path_or_raw_edge_list_mode(cpstate, node, vle_expr,
                                                NULL, 0, 0, mode, false) &&
        get_vle_tail_reverse_list_slice_mode(outer_name, false, mode);
}

static bool parse_vle_tail_reverse_source_slice_mode(
    cypher_parsestate *cpstate, const char *outer_name, Node *node,
    Expr **vle_expr, int64 *mode, bool arbitrary_path)
{
    FuncCall *inner_fn = NULL;
    Node *inner_arg = NULL;
    char *inner_name = NULL;

    if (!is_tail_reverse_name(outer_name))
    {
        return false;
    }

    if (IsA(node, FuncCall))
    {
        inner_fn = (FuncCall *)node;
        if (parse_tail_reverse_func_any_arg(inner_fn, &inner_name,
                                            &inner_arg))
        {
            if (!parse_current_or_raw_vle_nested_list_slice_mode(
                    cpstate, inner_arg, outer_name, inner_name,
                    vle_expr, mode, arbitrary_path))
            {
                return false;
            }

            return *vle_expr != NULL;
        }
    }

    if (!parse_current_or_raw_vle_tail_reverse_list_slice_mode(
            cpstate, node, outer_name, vle_expr, mode, arbitrary_path))
    {
        return false;
    }

    return *vle_expr != NULL;
}

static bool parse_vle_slice_boundary_source_mode(
    cypher_parsestate *cpstate, Node *node, Expr **vle_expr, int64 *mode,
    int64 *mode_flag)
{
    FuncCall *outer_fn = NULL;
    FuncCall *list_fn = NULL;
    Node *outer_arg = NULL;
    char *outer_name = NULL;
    char *list_name = NULL;

    *mode_flag = 0;

    if (!IsA(node, FuncCall))
    {
        if (!parse_vle_path_or_raw_edge_list_mode(cpstate, node, vle_expr,
                                                  &list_name, 2, 0, mode,
                                                  false))
        {
            return false;
        }

        return true;
    }

    outer_fn = (FuncCall *)node;
    if (parse_path_list_func_name(outer_fn, &outer_name))
    {
        return parse_vle_path_or_raw_edge_list_mode(cpstate, node, vle_expr,
                                                    NULL, 2, 0, mode, true);
    }

    if (!parse_tail_reverse_func_any_arg(outer_fn, &outer_name, &outer_arg))
    {
        return false;
    }

    if (!IsA(outer_arg, FuncCall))
    {
        if (!parse_vle_path_or_raw_edge_list_mode(
                cpstate, outer_arg, vle_expr, &list_name, 0,
                is_tail_name(outer_name) ? 4 : 0, mode, false))
            return false;

        if (is_reverse_name(outer_name))
        {
            *mode_flag = VLE_SLICE_BOUNDARY_REVERSE_OFFSET;
        }
        return true;
    }

    list_fn = (FuncCall *)outer_arg;
    if (!parse_path_list_or_tail_reverse_func_arg(list_fn, &list_name, NULL))
    {
        return false;
    }

    if (get_vle_nested_tail_reverse_mode_flag(outer_name, list_name,
                                              mode_flag))
    {
        return parse_vle_nested_tail_reverse_base_mode(
            cpstate, list_fn, 0, vle_expr, &list_name, mode);
    }

    if (is_reverse_name(outer_name))
    {
        *mode_flag = VLE_SLICE_BOUNDARY_REVERSE_OFFSET;
        return parse_vle_path_or_raw_edge_list_mode(
            cpstate, (Node *)list_fn, vle_expr, NULL, 2, 0, mode, true);
    }

    return parse_vle_path_or_raw_edge_list_mode(
        cpstate, (Node *)list_fn, vle_expr, NULL, 6, 4, mode, true);
}

static bool parse_vle_nested_tail_reverse_base_mode(
    cypher_parsestate *cpstate, FuncCall *inner_fn, int64 mode_flag,
    Expr **vle_expr, char **list_name, int64 *mode)
{
    Node *inner_arg = NULL;

    if (!parse_path_list_or_tail_reverse_func_arg(inner_fn, NULL, &inner_arg))
    {
        return false;
    }

    return parse_vle_path_or_raw_edge_list_mode(
        cpstate, inner_arg, vle_expr, list_name, 6 + mode_flag,
        4 + mode_flag, mode, true);
}

static bool parse_vle_named_path_list_function(
    cypher_parsestate *cpstate, Node *node, const char *expected_name,
    Expr **vle_expr, bool allow_visible)
{
    char *list_name = NULL;

    if (allow_visible)
    {
        return parse_arbitrary_vle_path_list_function(cpstate, node,
                                                      vle_expr,
                                                      &list_name) &&
            pg_strcasecmp(list_name, expected_name) == 0;
    }

    return parse_current_vle_path_list_function(cpstate, node, vle_expr,
                                                &list_name) &&
        pg_strcasecmp(list_name, expected_name) == 0;
}

static bool parse_current_vle_head_last_list(cypher_parsestate *cpstate,
                                             const char *outer_name,
                                             FuncCall *inner_fn,
                                             bool allow_tail_last,
                                             bool allow_raw_relationships,
                                             Expr **vle_expr,
                                             char **list_name,
                                             int64 *index,
                                             bool *tail_last)
{
    char *inner_name = NULL;
    Node *inner_arg = NULL;

    *vle_expr = NULL;
    *list_name = NULL;
    *tail_last = false;

    if (parse_path_list_func_arg(inner_fn, T_ColumnRef, &inner_name,
                                 &inner_arg))
    {
        if (!parse_vle_path_or_raw_edge_list(cpstate, (Node *)inner_fn,
                                             vle_expr, list_name, true))
        {
            return false;
        }
        *index = is_head_name(outer_name) ? 0 : -1;
        return true;
    }

    if (!parse_tail_reverse_func_any_arg(inner_fn, &inner_name, &inner_arg))
    {
        return false;
    }

    if (IsA(inner_arg, FuncCall))
    {
        if (!parse_vle_path_or_raw_edge_list(cpstate, inner_arg,
                                             vle_expr, list_name, true))
        {
            return false;
        }
    }
    else if (allow_raw_relationships)
    {
        if (!parse_current_or_raw_vle_list(cpstate, inner_arg,
                                           vle_expr, list_name))
        {
            return false;
        }
    }
    else
    {
        return false;
    }

    if (*vle_expr == NULL ||
        !get_vle_head_last_tail_reverse_index(outer_name, inner_name,
                                              index, tail_last))
    {
        return false;
    }

    return allow_tail_last || !*tail_last;
}

static bool parse_current_vle_head_last_source(cypher_parsestate *cpstate,
                                               const char *outer_name,
                                               Node *node,
                                               bool allow_tail_last,
                                               bool allow_raw_relationships,
                                               Expr **vle_expr,
                                               char **list_name,
                                               int64 *index,
                                               bool *tail_last)
{
    if (!IsA(node, FuncCall))
    {
        if (!parse_current_or_raw_vle_list(cpstate, node, vle_expr,
                                           list_name))
        {
            return false;
        }

        *index = is_head_name(outer_name) ? 0 : -1;
        *tail_last = false;
        return true;
    }

    return parse_current_vle_head_last_list(cpstate, outer_name,
                                            (FuncCall *)node,
                                            allow_tail_last,
                                            allow_raw_relationships,
                                            vle_expr, list_name, index,
                                            tail_last);
}

static bool parse_current_raw_vle_edge_list(cypher_parsestate *cpstate,
                                            Node *node,
                                            Expr **vle_expr,
                                            char **list_name)
{
    *vle_expr = get_current_any_vle_edge_expr(cpstate, node);
    if (*vle_expr == NULL)
    {
        return false;
    }

    *list_name = "relationships";

    return true;
}

static bool parse_current_vle_path_or_raw_edge_list_internal(
    cypher_parsestate *cpstate, Node *node, Expr **vle_expr,
    char **list_name, bool require_fixed_one_hop)
{
    if (IsA(node, FuncCall))
    {
        return parse_current_vle_path_list_function(cpstate, node, vle_expr,
                                                    list_name);
    }

    *vle_expr = get_current_vle_edge_expr_internal(cpstate, node,
                                                   require_fixed_one_hop);
    if (*vle_expr == NULL)
    {
        return false;
    }

    *list_name = "relationships";
    return true;
}

static bool parse_current_vle_path_or_fixed_raw_edge_list(
    cypher_parsestate *cpstate, Node *node, Expr **vle_expr,
    char **list_name)
{
    return parse_current_vle_path_or_raw_edge_list_internal(
        cpstate, node, vle_expr, list_name, true);
}

static bool parse_vle_nested_count_source(cypher_parsestate *cpstate,
                                          FuncCall *list_fn,
                                          Expr **vle_expr,
                                          char **list_name)
{
    Node *source_arg = NULL;

    if (!parse_path_list_or_tail_reverse_func_arg(list_fn, NULL,
                                                  &source_arg))
    {
        return false;
    }

    if (IsA(source_arg, FuncCall))
    {
        return parse_arbitrary_vle_path_list_function(cpstate, source_arg,
                                                      vle_expr, list_name);
    }

    return parse_current_raw_vle_edge_list(cpstate, source_arg, vle_expr,
                                           list_name);
}

static bool parse_vle_nested_count_list_arg(
    cypher_parsestate *cpstate, const char *outer_name, FuncCall *list_fn,
    Expr **vle_expr, char **list_name, bool *tail_mode, bool *double_tail)
{
    char *inner_name = NULL;
    Node *inner_arg = NULL;

    if (!parse_path_list_or_tail_reverse_func_arg(list_fn, &inner_name,
                                                  &inner_arg))
    {
        return false;
    }

    if (parse_arbitrary_vle_path_list_function(cpstate, (Node *)list_fn,
                                               vle_expr, list_name))
    {
        *tail_mode = is_tail_name(outer_name);
        return true;
    }

    if (!is_tail_reverse_name(inner_name) ||
        !get_vle_nested_count_mode_flags(outer_name, inner_name, tail_mode,
                                         double_tail))
    {
        return false;
    }

    return parse_vle_nested_count_source(cpstate, list_fn, vle_expr,
                                         list_name);
}

static bool parse_vle_count_list_arg(cypher_parsestate *cpstate,
                                     FuncCall *inner_fn, Expr **vle_expr,
                                     char **list_name, bool *tail_mode,
                                     bool *double_tail)
{
    FuncCall *list_fn = NULL;
    char *inner_name = NULL;
    Node *inner_arg = NULL;

    *vle_expr = NULL;
    *list_name = NULL;
    *tail_mode = false;
    *double_tail = false;

    if (!parse_path_list_or_tail_reverse_func_arg(inner_fn, &inner_name,
                                                  &inner_arg))
    {
        return false;
    }

    if (is_path_list_name(inner_name))
    {
        return parse_arbitrary_vle_path_list_function(cpstate,
                                                      (Node *)inner_fn,
                                                      vle_expr, list_name);
    }
    else if (is_tail_reverse_name(inner_name) &&
             IsA(inner_arg, FuncCall))
    {
        list_fn = (FuncCall *)inner_arg;
        if (!parse_vle_nested_count_list_arg(cpstate, inner_name, list_fn,
                                             vle_expr, list_name, tail_mode,
                                             double_tail))
        {
            return false;
        }
    }
    else if (is_tail_reverse_name(inner_name))
    {
        if (parse_current_raw_vle_edge_list(cpstate, inner_arg,
                                            vle_expr, list_name))
        {
            *tail_mode = is_tail_name(inner_name);
        }
    }
    else
    {
        return false;
    }

    return *vle_expr != NULL;
}

static Node *transform_vle_path_count(cypher_parsestate *cpstate,
                                      FuncCall *fn, const char *func_name,
                                      bool is_empty)
{
    Node *inner_arg = NULL;
    FuncCall *inner_fn = NULL;
    char *list_name = NULL;
    Expr *vle_expr = NULL;
    Oid func_oid;
    int64 mode;
    bool tail_mode = false;
    bool double_tail = false;

    if (!parse_func_arg(fn, func_name, T_FuncCall, &inner_arg))
    {
        return NULL;
    }

    inner_fn = (FuncCall *)inner_arg;
    if (!parse_vle_count_list_arg(cpstate, inner_fn, &vle_expr, &list_name,
                                  &tail_mode, &double_tail))
    {
        return NULL;
    }

    if (double_tail)
    {
        return make_vle_double_tail_count_expr((Node *)vle_expr, list_name,
                                               fn->location, is_empty);
    }

    if (is_empty)
    {
        mode = get_vle_list_is_empty_mode(list_name, tail_mode);
        func_oid = get_age_vle_list_is_empty_oid();
        return make_vle_binary_mode_typed_expr(func_oid, BOOLOID,
                                               (Node *)vle_expr, mode,
                                               fn->location);
    }

    func_oid = get_vle_list_count_oid(list_name, tail_mode);
    return make_vle_unary_agtype_expr(func_oid, (Node *)vle_expr);
}

static Node *try_transform_vle_path_size(cypher_parsestate *cpstate,
                                         FuncCall *fn)
{
    return transform_vle_path_count(cpstate, fn, "size", false);
}

static Node *try_transform_vle_path_is_empty(cypher_parsestate *cpstate,
                                             FuncCall *fn)
{
    return transform_vle_path_count(cpstate, fn, "isEmpty", true);
}

static Node *try_transform_vle_path_head_last(cypher_parsestate *cpstate,
                                              FuncCall *fn)
{
    char *outer_name = NULL;
    char *list_name = NULL;
    Node *head_last_arg = NULL;
    Expr *vle_expr = NULL;
    Oid func_oid;
    int64 index;
    bool tail_last = false;

    if (!parse_head_last_func_any_arg(fn, &outer_name, &head_last_arg))
    {
        return NULL;
    }

    if (!parse_current_vle_head_last_source(cpstate, outer_name,
                                            head_last_arg, true, true,
                                            &vle_expr, &list_name, &index,
                                            &tail_last))
    {
        return NULL;
    }

    if (vle_expr == NULL)
    {
        return NULL;
    }

    if (tail_last)
    {
        func_oid = get_vle_tail_last_materialize_oid(list_name);
        return make_vle_unary_agtype_expr(func_oid, (Node *)vle_expr);
    }

    return make_vle_materialized_index_expr(vle_expr, list_name, index,
                                            fn->location);
}

static Node *transform_vle_path_nested_transform_head_last(
    cypher_parsestate *cpstate, FuncCall *fn, int64 mode_offset)
{
    FuncCall *outer_fn = NULL;
    FuncCall *inner_fn = NULL;
    A_Indirection *slice_arg = NULL;
    A_Indices *slice_indices = NULL;
    Expr *vle_expr = NULL;
    Node *lower_expr = NULL;
    Node *upper_expr = NULL;
    char *head_last_name = NULL;
    Node *head_last_arg = NULL;
    Node *outer_arg = NULL;
    char *outer_name = NULL;
    char *inner_name = NULL;
    char *list_name = NULL;
    int64 mode_flag = 0;
    int64 mode;
    bool last;

    if (!parse_head_last_func_arg(fn, T_FuncCall, &head_last_name,
                                  &head_last_arg))
    {
        return NULL;
    }
    last = is_last_name(head_last_name);

    outer_fn = (FuncCall *)head_last_arg;
    if (!parse_tail_reverse_func_any_arg(outer_fn, &outer_name, &outer_arg))
    {
        return NULL;
    }

    if (IsA(outer_arg, A_Indirection))
    {
        slice_arg = (A_Indirection *)outer_arg;
        if (!parse_single_indirection_slice(slice_arg, &slice_indices))
        {
            return NULL;
        }

        if (IsA(slice_arg->arg, FuncCall))
        {
            if (!parse_vle_path_or_raw_edge_list_mode(
                    cpstate, slice_arg->arg, &vle_expr, NULL, 2, 0, &mode,
                    true))
            {
                return NULL;
            }
        }
        else
        {
            if (!parse_vle_path_or_raw_edge_list_mode(
                    cpstate, slice_arg->arg, &vle_expr, &list_name, 2, 0,
                    &mode, false))
            {
                return NULL;
            }
        }

        if (is_tail_name(outer_name))
        {
            mode_flag = VLE_SLICE_BOUNDARY_SLICE_TAIL_OFFSET;
        }
        else
        {
            mode_flag = VLE_SLICE_BOUNDARY_SLICE_REVERSE_OFFSET;
        }

        if (vle_expr == NULL)
        {
            return NULL;
        }

        transform_slice_bounds_or_null(cpstate, slice_indices,
                                       &lower_expr, &upper_expr);
        return make_vle_slice_boundary_head_last_expr(
            (Node *)vle_expr, lower_expr, upper_expr,
            mode + mode_flag, mode_offset, last, fn->location);
    }

    if (!IsA(outer_arg, FuncCall))
    {
        return NULL;
    }

    inner_fn = (FuncCall *)outer_arg;
    if (!parse_tail_reverse_func_any_arg(inner_fn, &inner_name, NULL) ||
        !get_vle_nested_tail_reverse_mode_flag(outer_name, inner_name,
                                               &mode_flag))
    {
        return NULL;
    }

    if (!parse_vle_nested_tail_reverse_base_mode(cpstate, inner_fn, mode_flag,
                                                 &vle_expr, &list_name,
                                                 &mode))
    {
        return NULL;
    }

    if (vle_expr == NULL)
    {
        return NULL;
    }

    make_slice_bounds_to_null(0, fn->location, &lower_expr, &upper_expr);
    return make_vle_slice_boundary_head_last_expr(
        (Node *)vle_expr, lower_expr, upper_expr, mode, mode_offset, last,
        fn->location);
}

static Node *try_transform_vle_path_nested_transform_head_last(
    cypher_parsestate *cpstate, FuncCall *fn)
{
    return transform_vle_path_nested_transform_head_last(cpstate, fn, 0);
}

static Node *transform_vle_path_any_slice_boundary_head_last(
    cypher_parsestate *cpstate, FuncCall *fn, int64 mode_offset)
{
    Node *retval = NULL;

    retval = transform_vle_path_nested_transform_head_last(cpstate, fn,
                                                           mode_offset);
    if (retval != NULL)
    {
        return retval;
    }

    return transform_vle_path_slice_head_last(cpstate, fn, mode_offset);
}

static Node *try_transform_vle_path_list_tail_reverse(
    cypher_parsestate *cpstate, FuncCall *fn)
{
    FuncCall *inner_fn = NULL;
    Node *inner_arg = NULL;
    char *outer_name = NULL;
    char *list_name = NULL;
    Expr *vle_expr = NULL;
    Oid func_oid;

    if (!parse_tail_reverse_func_arg(fn, T_FuncCall, &outer_name, &inner_arg))
    {
        return NULL;
    }

    inner_fn = (FuncCall *)inner_arg;
    if (!parse_vle_path_list_function(cpstate, (Node *)inner_fn, &vle_expr,
                                      &list_name, true))
    {
        return NULL;
    }

    func_oid = get_vle_tail_reverse_materialize_oid(list_name, outer_name);
    return make_vle_unary_agtype_expr(func_oid, (Node *)vle_expr);
}

static Node *try_transform_vle_path_relationships(cypher_parsestate *cpstate,
                                                  FuncCall *fn)
{
    return transform_vle_path_materialized_list(cpstate, fn, "relationships");
}

static Node *try_transform_vle_path_nodes(cypher_parsestate *cpstate,
                                          FuncCall *fn)
{
    return transform_vle_path_materialized_list(cpstate, fn, "nodes");
}

static FuncCall *make_unary_func_call(FuncCall *fn, Node *arg, int location)
{
    return makeFuncCall(copyObject(fn->funcname), list_make1(arg),
                        COERCE_SQL_SYNTAX, location);
}

static A_Indices *make_index_indices(int64 index, int location)
{
    A_Const *index_const;
    A_Indices *indices;

    index_const = makeNode(A_Const);
    index_const->val.ival.type = T_Integer;
    index_const->val.ival.ival = index;
    index_const->location = location;

    indices = makeNode(A_Indices);
    indices->is_slice = false;
    indices->lidx = NULL;
    indices->uidx = (Node *)index_const;

    return indices;
}

static A_Indirection *make_indexed_indirection(Node *arg, int64 index,
                                               int location)
{
    A_Indirection *indexed_arg;

    indexed_arg = makeNode(A_Indirection);
    indexed_arg->arg = (Node *)copyObject(arg);
    indexed_arg->indirection = list_make1(make_index_indices(index,
                                                             location));

    return indexed_arg;
}

static FuncCall *make_indexed_unary_func_call(FuncCall *fn, Node *arg,
                                              int64 index, int location)
{
    A_Indirection *indexed_arg;

    indexed_arg = make_indexed_indirection(arg, index, location);
    return make_unary_func_call(fn, (Node *)indexed_arg,
                                exprLocation((Node *)fn));
}

static char *make_raw_attr_name(const char *var_name, const char *attr_name)
{
    return psprintf("%sraw_%s_%s", AGE_DEFAULT_ALIAS_PREFIX, var_name,
                    attr_name);
}

static char *make_raw_edges_name(const char *var_name)
{
    return psprintf("%sraw_%s_edges", AGE_DEFAULT_ALIAS_PREFIX, var_name);
}

static bool is_internal_alias_name(const char *name)
{
    return name != NULL &&
        pg_strncasecmp(name, AGE_DEFAULT_ALIAS_PREFIX,
                       sizeof(AGE_DEFAULT_ALIAS_PREFIX) - 1) == 0;
}

static bool has_rls_enabled_label(cypher_parsestate *cpstate,
                                  cypher_node *node)
{
    Oid relid;

    if (node == NULL || node->label == NULL)
        return false;

    relid = get_label_relation(node->label, cpstate->graph_oid);
    if (!OidIsValid(relid))
        return false;

    return check_enable_rls(relid, InvalidOid, true) == RLS_ENABLED;
}

static bool can_direct_rewrite_edge_endpoint(cypher_parsestate *cpstate,
                                             cypher_node *start_node,
                                             cypher_node *end_node)
{
    if (start_node == NULL || end_node == NULL ||
        start_node->name == NULL || end_node->name == NULL ||
        is_internal_alias_name(start_node->name) ||
        is_internal_alias_name(end_node->name))
    {
        return false;
    }

    return !has_rls_enabled_label(cpstate, start_node) &&
        !has_rls_enabled_label(cpstate, end_node);
}

static Node *make_raw_attr_var(ParseState *pstate, const char *var_name,
                               const char *attr_name, int location)
{
    char *raw_attr_name;
    Node *raw_var;

    raw_attr_name = make_raw_attr_name(var_name, attr_name);
    raw_var = colNameToVar(pstate, raw_attr_name, false, location);
    pfree(raw_attr_name);

    return raw_var;
}

static bool make_raw_edge_vars(ParseState *pstate, const char *edge_name,
                               int location, Node **id, Node **start_id,
                               Node **end_id, Node **props)
{
    *id = make_raw_attr_var(pstate, edge_name, AG_EDGE_COLNAME_ID, location);
    *start_id = make_raw_attr_var(pstate, edge_name, AG_EDGE_COLNAME_START_ID,
                                  location);
    *end_id = make_raw_attr_var(pstate, edge_name, AG_EDGE_COLNAME_END_ID,
                                location);
    *props = make_raw_attr_var(pstate, edge_name, AG_EDGE_COLNAME_PROPERTIES,
                               location);

    return *id != NULL && *start_id != NULL && *end_id != NULL &&
        *props != NULL;
}

static Node *make_fixed_path_edge_id_var(ParseState *pstate,
                                         cypher_relationship *single_rel,
                                         int location)
{
    return make_raw_attr_var(pstate, single_rel->name, AG_EDGE_COLNAME_ID,
                             location);
}

static Node *make_fixed_path_edge_endpoint_id_var(
    ParseState *pstate, cypher_relationship *single_rel, bool start_endpoint,
    int location)
{
    return make_raw_attr_var(pstate, single_rel->name,
                             start_endpoint ? AG_EDGE_COLNAME_START_ID :
                             AG_EDGE_COLNAME_END_ID,
                             location);
}

static bool make_fixed_endpoint_edge_id_vars(
    ParseState *pstate, cypher_relationship *single_rel,
    const char *endpoint_id_name, int location, Node **edge_id,
    Node **endpoint_id)
{
    *edge_id = make_fixed_path_edge_id_var(pstate, single_rel, location);
    *endpoint_id = make_fixed_path_edge_endpoint_id_var(
        pstate, single_rel, is_start_edge_endpoint_id_name(endpoint_id_name),
        location);

    return *edge_id != NULL && *endpoint_id != NULL;
}

static const char *get_fixed_path_endpoint_vertex_name(
    cypher_node *start_node, cypher_node *end_node, const char *endpoint_name)
{
    return is_start_endpoint_function_name(endpoint_name) ? start_node->name :
        end_node->name;
}

static Node *make_raw_vertex_props_var(ParseState *pstate,
                                       const char *vertex_name,
                                       int location)
{
    return make_raw_attr_var(pstate, vertex_name, AG_VERTEX_COLNAME_PROPERTIES,
                             location);
}

static bool make_raw_vertex_id_props_vars(ParseState *pstate,
                                          const char *vertex_name,
                                          int location,
                                          Node **vertex_id,
                                          Node **vertex_props)
{
    *vertex_id = make_raw_attr_var(pstate, vertex_name, AG_VERTEX_COLNAME_ID,
                                   location);
    *vertex_props = make_raw_vertex_props_var(pstate, vertex_name, location);

    return *vertex_id != NULL && *vertex_props != NULL;
}

static bool make_fixed_endpoint_edge_id_and_vertex_name(
    cypher_parsestate *cpstate, cypher_node *start_node,
    cypher_relationship *single_rel, cypher_node *end_node,
    const char *endpoint_name, int location,
    Node **edge_id, const char **vertex_name)
{
    ParseState *pstate = (ParseState *)cpstate;

    if (!can_direct_rewrite_edge_endpoint(cpstate, start_node, end_node))
        return false;

    *vertex_name = get_fixed_path_endpoint_vertex_name(start_node, end_node,
                                                       endpoint_name);
    *edge_id = make_fixed_path_edge_id_var(pstate, single_rel, location);

    return *edge_id != NULL;
}

static bool make_fixed_endpoint_vertex_vars(
    cypher_parsestate *cpstate, cypher_node *start_node,
    cypher_relationship *single_rel, cypher_node *end_node,
    const char *endpoint_name, int location,
    Node **edge_id, Node **vertex_id, Node **vertex_props)
{
    ParseState *pstate = (ParseState *)cpstate;
    const char *vertex_name;

    if (!make_fixed_endpoint_edge_id_and_vertex_name(
            cpstate, start_node, single_rel, end_node, endpoint_name, location,
            edge_id, &vertex_name))
        return false;

    return
        make_raw_vertex_id_props_vars(pstate, vertex_name, location, vertex_id,
                                      vertex_props);
}

static bool make_fixed_endpoint_vertex_props_var(
    cypher_parsestate *cpstate, cypher_node *start_node,
    cypher_relationship *single_rel, cypher_node *end_node,
    const char *endpoint_name, int location,
    Node **edge_id, Node **vertex_props)
{
    ParseState *pstate = (ParseState *)cpstate;
    const char *vertex_name;

    if (!make_fixed_endpoint_edge_id_and_vertex_name(
            cpstate, start_node, single_rel, end_node, endpoint_name, location,
            edge_id, &vertex_name))
        return false;

    *vertex_props = make_raw_vertex_props_var(pstate, vertex_name, location);

    return *vertex_props != NULL;
}

static bool make_edge_endpoint_vertex_vars(
    cypher_parsestate *cpstate, const char *edge_name,
    const char *endpoint_name, int location, Node **edge_id,
    Node **vertex_id, Node **vertex_props)
{
    ParseState *pstate = (ParseState *)cpstate;
    transform_entity *prev_entity = NULL;
    ListCell *lc;

    *edge_id = NULL;
    *vertex_id = NULL;
    *vertex_props = NULL;

    foreach (lc, cpstate->entities)
    {
        transform_entity *entity = lfirst(lc);

        if (entity->type == ENT_EDGE &&
            entity->entity.rel != NULL &&
            entity->entity.rel->name != NULL &&
            (entity->declared_in_current_clause || entity->has_raw_targets) &&
            entity->entity.rel->varlen == NULL &&
            pg_strcasecmp(entity->entity.rel->name, edge_name) == 0)
        {
            transform_entity *next_entity = NULL;
            cypher_relationship *rel = entity->entity.rel;
            cypher_node *start_node = NULL;
            cypher_node *end_node = NULL;
            const char *vertex_name = NULL;
            ListCell *next_lc = lnext(cpstate->entities, lc);

            if (prev_entity == NULL || next_lc == NULL)
                return false;

            next_entity = lfirst(next_lc);
            if (prev_entity->type != ENT_VERTEX ||
                next_entity->type != ENT_VERTEX ||
                prev_entity->entity.node == NULL ||
                next_entity->entity.node == NULL)
            {
                return false;
            }

            if (rel->dir == CYPHER_REL_DIR_RIGHT)
            {
                start_node = prev_entity->entity.node;
                end_node = next_entity->entity.node;
            }
            else if (rel->dir == CYPHER_REL_DIR_LEFT)
            {
                start_node = next_entity->entity.node;
                end_node = prev_entity->entity.node;
            }
            else
            {
                return false;
            }

            if (!can_direct_rewrite_edge_endpoint(cpstate, start_node,
                                                  end_node))
                return false;

            vertex_name = get_fixed_path_endpoint_vertex_name(
                start_node, end_node, endpoint_name);
            *edge_id = make_raw_attr_var(pstate, edge_name,
                                         AG_EDGE_COLNAME_ID, location);
            if (!make_raw_vertex_id_props_vars(pstate, vertex_name, location,
                                               vertex_id, vertex_props))
            {
                return false;
            }

            return *edge_id != NULL;
        }

        prev_entity = entity;
    }

    return false;
}

static char *make_fixed_path_indexed_raw_attr_name(
    char *list_name, int64 index, cypher_node *start_node,
    cypher_relationship *single_rel, cypher_node *end_node,
    const char *edge_attr_name, const char *vertex_attr_name)
{
    if (is_relationships_list_name(list_name))
    {
        if (edge_attr_name == NULL || index != 0)
            return NULL;

        return make_raw_attr_name(single_rel->name, edge_attr_name);
    }

    if (is_nodes_list_name(list_name))
    {
        if (index == 0)
            return make_raw_attr_name(start_node->name, vertex_attr_name);
        if (index == 1)
            return make_raw_attr_name(end_node->name, vertex_attr_name);
    }

    return NULL;
}

static Node *make_fixed_path_indexed_raw_attr_var(
    ParseState *pstate, char *list_name, int64 index,
    cypher_node *start_node, cypher_relationship *single_rel,
    cypher_node *end_node, const char *edge_attr_name,
    const char *vertex_attr_name, int location)
{
    char *raw_attr_name;
    Node *raw_var;

    raw_attr_name = make_fixed_path_indexed_raw_attr_name(
        list_name, index, start_node, single_rel, end_node, edge_attr_name,
        vertex_attr_name);
    if (raw_attr_name == NULL)
        return NULL;

    raw_var = colNameToVar(pstate, raw_attr_name, false, location);
    pfree(raw_attr_name);

    return raw_var;
}

static bool make_fixed_path_indexed_id_vars(
    ParseState *pstate, char *list_name, int64 index,
    cypher_node *start_node, cypher_relationship *single_rel,
    cypher_node *end_node, const char *edge_attr_name, int location,
    Node **edge_id, Node **target_id)
{
    *edge_id = make_fixed_path_edge_id_var(pstate, single_rel, location);
    *target_id = make_fixed_path_indexed_raw_attr_var(
        pstate, list_name, index, start_node, single_rel, end_node,
        edge_attr_name, AG_VERTEX_COLNAME_ID, location);

    return *edge_id != NULL && *target_id != NULL;
}

static bool make_fixed_path_indexed_props_vars(
    ParseState *pstate, char *list_name, int64 index,
    cypher_node *start_node, cypher_relationship *single_rel,
    cypher_node *end_node, int location, Node **edge_id, Node **target_props)
{
    *edge_id = make_fixed_path_edge_id_var(pstate, single_rel, location);
    *target_props = make_fixed_path_indexed_raw_attr_var(
        pstate, list_name, index, start_node, single_rel, end_node,
        AG_EDGE_COLNAME_PROPERTIES, AG_VERTEX_COLNAME_PROPERTIES, location);

    return *edge_id != NULL && *target_props != NULL;
}

static bool is_nodes_list_name(const char *list_name)
{
    return pg_strcasecmp(list_name, "nodes") == 0;
}

static bool is_relationships_list_name(const char *list_name)
{
    return pg_strcasecmp(list_name, "relationships") == 0;
}

static bool is_path_list_name(const char *list_name)
{
    return is_nodes_list_name(list_name) ||
        is_relationships_list_name(list_name);
}

static bool is_equal_operator_name(List *name)
{
    return list_length(name) == 1 &&
        pg_strcasecmp(strVal(linitial(name)), "=") == 0;
}

static bool is_head_name(const char *func_name)
{
    return pg_strcasecmp(func_name, "head") == 0;
}

static bool is_last_name(const char *func_name)
{
    return pg_strcasecmp(func_name, "last") == 0;
}

static bool is_tail_name(const char *func_name)
{
    return pg_strcasecmp(func_name, "tail") == 0;
}

static bool is_reverse_name(const char *func_name)
{
    return pg_strcasecmp(func_name, "reverse") == 0;
}

static bool is_size_name(const char *func_name)
{
    const AgeBuiltinFuncMeta *meta;

    meta = get_age_builtin_func_meta_by_name(func_name);
    return meta != NULL && meta->fast_path == AGE_FUNC_FAST_SIZE;
}

static bool is_is_empty_name(const char *func_name)
{
    return pg_strcasecmp(func_name, "isEmpty") == 0;
}

static bool is_size_is_empty_name(const char *func_name)
{
    return is_size_name(func_name) || is_is_empty_name(func_name);
}

static bool is_head_last_name(const char *func_name)
{
    return is_head_name(func_name) || is_last_name(func_name);
}

static bool is_tail_reverse_name(const char *func_name)
{
    return is_tail_name(func_name) || is_reverse_name(func_name);
}

static bool is_id_name(const char *func_name)
{
    const AgeBuiltinFuncMeta *meta;

    meta = get_age_builtin_func_meta_by_name(func_name);
    return meta != NULL && meta->fast_path == AGE_FUNC_FAST_ID;
}

static bool is_keys_name(const char *func_name)
{
    const AgeBuiltinFuncMeta *meta;

    meta = get_age_builtin_func_meta_by_name(func_name);
    return meta != NULL && meta->fast_path == AGE_FUNC_FAST_KEYS;
}

static bool is_label_name(const char *func_name)
{
    return pg_strcasecmp(func_name, "label") == 0;
}

static bool is_labels_name(const char *func_name)
{
    return pg_strcasecmp(func_name, "labels") == 0;
}

static bool is_properties_name(const char *func_name)
{
    return pg_strcasecmp(func_name, "properties") == 0;
}

static bool is_type_name(const char *func_name)
{
    return pg_strcasecmp(func_name, "type") == 0;
}

static bool is_label_type_name(const char *func_name)
{
    return is_label_name(func_name) || is_type_name(func_name);
}

static bool is_vle_edge_field_name(const char *field_name)
{
    return is_properties_name(field_name) || is_label_type_name(field_name);
}

static bool is_start_edge_endpoint_id_name(const char *func_name)
{
    return pg_strcasecmp(func_name, "start_id") == 0;
}

static bool is_end_edge_endpoint_id_name(const char *func_name)
{
    return pg_strcasecmp(func_name, "end_id") == 0;
}

static bool is_edge_endpoint_id_name(const char *func_name)
{
    return is_start_edge_endpoint_id_name(func_name) ||
        is_end_edge_endpoint_id_name(func_name);
}

static const char *get_edge_endpoint_id_col_name(const char *func_name)
{
    if (is_start_edge_endpoint_id_name(func_name))
        return AG_EDGE_COLNAME_START_ID;
    if (is_end_edge_endpoint_id_name(func_name))
        return AG_EDGE_COLNAME_END_ID;

    return NULL;
}

typedef enum VLENestedTailReverseKind
{
    VLE_NESTED_TAIL_REVERSE_INVALID,
    VLE_NESTED_DOUBLE_TAIL,
    VLE_NESTED_TAIL_REVERSE,
    VLE_NESTED_REVERSE_TAIL
} VLENestedTailReverseKind;

static VLENestedTailReverseKind get_vle_nested_tail_reverse_kind(
    const char *outer_name, const char *inner_name)
{
    if (is_tail_name(outer_name) &&
        is_tail_name(inner_name))
    {
        return VLE_NESTED_DOUBLE_TAIL;
    }

    if (is_tail_name(outer_name) &&
        is_reverse_name(inner_name))
    {
        return VLE_NESTED_TAIL_REVERSE;
    }

    if (is_reverse_name(outer_name) &&
        is_tail_name(inner_name))
    {
        return VLE_NESTED_REVERSE_TAIL;
    }

    return VLE_NESTED_TAIL_REVERSE_INVALID;
}

static bool get_vle_nested_tail_reverse_mode_flag(const char *outer_name,
                                                  const char *inner_name,
                                                  int64 *mode_flag)
{
    switch (get_vle_nested_tail_reverse_kind(outer_name, inner_name))
    {
    case VLE_NESTED_DOUBLE_TAIL:
        *mode_flag = VLE_SLICE_BOUNDARY_DOUBLE_TAIL_OFFSET;
        return true;
    case VLE_NESTED_TAIL_REVERSE:
        *mode_flag = VLE_SLICE_BOUNDARY_TAIL_REVERSE_OFFSET;
        return true;
    case VLE_NESTED_REVERSE_TAIL:
        *mode_flag = VLE_SLICE_BOUNDARY_REVERSE_OFFSET;
        return true;
    default:
        return false;
    }
}

static bool get_vle_nested_count_mode_flags(const char *outer_name,
                                            const char *inner_name,
                                            bool *tail_mode,
                                            bool *double_tail)
{
    switch (get_vle_nested_tail_reverse_kind(outer_name, inner_name))
    {
    case VLE_NESTED_DOUBLE_TAIL:
        *tail_mode = true;
        *double_tail = true;
        return true;
    case VLE_NESTED_TAIL_REVERSE:
    case VLE_NESTED_REVERSE_TAIL:
        *tail_mode = true;
        *double_tail = false;
        return true;
    default:
        return false;
    }
}

static bool get_vle_tail_reverse_list_slice_mode(const char *outer_name,
                                                 bool node_list,
                                                 int64 *mode)
{
    if (is_tail_name(outer_name))
    {
        *mode = node_list ? 3 : 2;
        return true;
    }

    if (is_reverse_name(outer_name))
    {
        *mode = node_list ? 5 : 4;
        return true;
    }

    return false;
}

static int64 get_vle_tail_reverse_access_index(bool tail,
                                               int64 index)
{
    if (tail)
    {
        return index + 1;
    }

    return -index - 1;
}

static bool get_vle_head_last_tail_reverse_index(const char *outer_name,
                                                 const char *inner_name,
                                                 int64 *index,
                                                 bool *tail_last)
{
    *tail_last = false;

    if (is_tail_name(inner_name))
    {
        if (is_head_name(outer_name))
        {
            *index = 1;
        }
        else
        {
            *tail_last = true;
            *index = -1;
        }

        return true;
    }

    if (is_reverse_name(inner_name))
    {
        *index = is_head_name(outer_name) ? -1 : 0;
        return true;
    }

    return false;
}

static bool get_vle_nested_list_slice_mode(const char *outer_name,
                                           const char *inner_name,
                                           bool node_list, int64 *mode)
{
    switch (get_vle_nested_tail_reverse_kind(outer_name, inner_name))
    {
    case VLE_NESTED_DOUBLE_TAIL:
        *mode = node_list ? 7 : 6;
        return true;
    case VLE_NESTED_TAIL_REVERSE:
        *mode = node_list ? 9 : 8;
        return true;
    case VLE_NESTED_REVERSE_TAIL:
        *mode = node_list ? 11 : 10;
        return true;
    default:
        return false;
    }
}

static Oid get_vle_tail_reverse_materialize_oid(const char *list_name,
                                                const char *outer_name)
{
    if (is_nodes_list_name(list_name))
    {
        if (is_tail_name(outer_name))
        {
            return get_age_materialize_vle_nodes_tail_oid();
        }

        return get_age_materialize_vle_nodes_reversed_oid();
    }

    if (is_tail_name(outer_name))
    {
        return get_age_materialize_vle_edges_tail_oid();
    }

    return get_age_materialize_vle_edges_reversed_oid();
}

static Oid get_vle_tail_last_materialize_oid(const char *list_name)
{
    if (is_nodes_list_name(list_name))
    {
        return get_age_materialize_vle_node_tail_last_oid();
    }

    return get_age_materialize_vle_edge_tail_last_oid();
}

static Oid get_vle_tail_last_id_oid(const char *list_name)
{
    if (is_nodes_list_name(list_name))
    {
        return get_age_vle_node_tail_last_id_oid();
    }

    return get_age_vle_edge_tail_last_id_oid();
}

static Oid get_vle_list_materialize_oid(const char *list_name)
{
    if (is_nodes_list_name(list_name))
    {
        return get_age_materialize_vle_nodes_oid();
    }

    return get_age_materialize_vle_edges_oid();
}

static Oid get_vle_list_count_oid(const char *list_name, bool tail_mode)
{
    if (tail_mode && is_relationships_list_name(list_name))
    {
        return get_age_vle_edge_tail_count_oid();
    }

    if (is_nodes_list_name(list_name) && !tail_mode)
    {
        return get_age_vle_path_node_count_oid();
    }

    return get_age_vle_path_length_oid();
}

static int64 get_vle_list_is_empty_mode(const char *list_name, bool tail_mode)
{
    if (tail_mode)
    {
        return is_nodes_list_name(list_name) ? 2 : 3;
    }

    return is_nodes_list_name(list_name) ? 0 : 1;
}

static Node *transform_vle_path_materialized_list(
    cypher_parsestate *cpstate, FuncCall *fn, const char *list_name)
{
    char *parsed_list_name = NULL;
    Expr *vle_expr;
    Oid func_oid;

    if (!parse_arbitrary_vle_path_list_function(cpstate, (Node *)fn,
                                                &vle_expr,
                                                &parsed_list_name) ||
        pg_strcasecmp(parsed_list_name, list_name) != 0)
    {
        return NULL;
    }

    func_oid = get_vle_list_materialize_oid(list_name);
    return make_vle_unary_agtype_expr(func_oid, (Node *)vle_expr);
}

static bool is_unary_func(FuncCall *fn)
{
    return list_length(fn->args) == 1;
}

static bool is_unary_func_arg(FuncCall *fn, NodeTag arg_type)
{
    return is_unary_func(fn) && nodeTag(linitial(fn->args)) == arg_type;
}

static bool is_single_unary_func_arg(FuncCall *fn, NodeTag arg_type)
{
    return is_single_func_name(fn) && is_unary_func_arg(fn, arg_type);
}

static bool is_unary_func_arg_either(FuncCall *fn, NodeTag left_type,
                                     NodeTag right_type)
{
    NodeTag arg_type;

    if (!is_unary_func(fn))
        return false;

    arg_type = nodeTag(linitial(fn->args));
    return arg_type == left_type || arg_type == right_type;
}

static bool parse_single_column_ref(Node *node, char **name, int *location)
{
    ColumnRef *cr;

    if (!IsA(node, ColumnRef))
        return false;

    cr = (ColumnRef *)node;
    if (list_length(cr->fields) != 1 ||
        !IsA(linitial(cr->fields), String))
    {
        return false;
    }

    if (name != NULL)
        *name = strVal(linitial(cr->fields));
    if (location != NULL)
        *location = cr->location;

    return true;
}

static bool is_single_func_name(FuncCall *fn)
{
    return list_length(fn->funcname) == 1;
}

static bool parse_single_func_name(FuncCall *fn, char **func_name)
{
    if (!is_single_func_name(fn))
        return false;

    if (func_name != NULL)
        *func_name = strVal(linitial(fn->funcname));

    return true;
}

static bool parse_single_unary_func_name(FuncCall *fn, char **func_name)
{
    if (!is_unary_func(fn) || !parse_single_func_name(fn, func_name))
        return false;

    return true;
}

static bool parse_single_unary_func_arg_name(FuncCall *fn, NodeTag arg_type,
                                             char **func_name)
{
    if (!is_single_unary_func_arg(fn, arg_type))
        return false;

    if (func_name != NULL)
        *func_name = strVal(linitial(fn->funcname));

    return true;
}

static bool parse_head_last_func_name(FuncCall *fn, char **head_last_name)
{
    return parse_single_unary_func_name(fn, head_last_name) &&
        is_head_last_name(*head_last_name);
}

static bool parse_head_last_func_arg_name(FuncCall *fn, NodeTag arg_type,
                                          char **head_last_name)
{
    return parse_single_unary_func_arg_name(fn, arg_type, head_last_name) &&
        is_head_last_name(*head_last_name);
}

static bool parse_head_last_func_arg(FuncCall *fn, NodeTag arg_type,
                                     char **head_last_name, Node **arg)
{
    if (!parse_head_last_func_arg_name(fn, arg_type, head_last_name))
    {
        return false;
    }

    if (arg != NULL)
    {
        *arg = linitial(fn->args);
    }

    return true;
}

static bool parse_head_last_func_any_arg(FuncCall *fn, char **head_last_name,
                                         Node **arg)
{
    char *parsed_name = NULL;

    if (!parse_head_last_func_name(fn, &parsed_name))
    {
        return false;
    }

    if (head_last_name != NULL)
    {
        *head_last_name = parsed_name;
    }
    if (arg != NULL)
    {
        *arg = linitial(fn->args);
    }

    return true;
}

static bool parse_tail_reverse_func_name(FuncCall *fn,
                                         char **tail_reverse_name)
{
    return parse_single_unary_func_name(fn, tail_reverse_name) &&
        is_tail_reverse_name(*tail_reverse_name);
}

static bool parse_tail_reverse_func_arg_name(FuncCall *fn, NodeTag arg_type,
                                             char **tail_reverse_name)
{
    return parse_single_unary_func_arg_name(fn, arg_type,
                                            tail_reverse_name) &&
        is_tail_reverse_name(*tail_reverse_name);
}

static bool parse_tail_reverse_func_arg(FuncCall *fn, NodeTag arg_type,
                                        char **tail_reverse_name, Node **arg)
{
    if (!parse_tail_reverse_func_arg_name(fn, arg_type, tail_reverse_name))
    {
        return false;
    }

    if (arg != NULL)
    {
        *arg = linitial(fn->args);
    }

    return true;
}

static bool parse_tail_reverse_func_any_arg(FuncCall *fn,
                                            char **tail_reverse_name,
                                            Node **arg)
{
    char *parsed_name = NULL;

    if (!parse_tail_reverse_func_name(fn, &parsed_name))
    {
        return false;
    }

    if (tail_reverse_name != NULL)
    {
        *tail_reverse_name = parsed_name;
    }
    if (arg != NULL)
    {
        *arg = linitial(fn->args);
    }

    return true;
}

static bool parse_path_list_func_name(FuncCall *fn, char **list_name)
{
    return parse_single_unary_func_name(fn, list_name) &&
        is_path_list_name(*list_name);
}

static bool parse_path_list_func_arg_name(FuncCall *fn, NodeTag arg_type,
                                          char **list_name)
{
    return parse_single_unary_func_arg_name(fn, arg_type, list_name) &&
        is_path_list_name(*list_name);
}

static bool parse_path_list_func_arg(FuncCall *fn, NodeTag arg_type,
                                     char **list_name, Node **arg)
{
    if (!parse_path_list_func_arg_name(fn, arg_type, list_name))
    {
        return false;
    }

    if (arg != NULL)
    {
        *arg = linitial(fn->args);
    }

    return true;
}

static bool parse_path_list_or_tail_reverse_func_name(FuncCall *fn,
                                                      char **func_name)
{
    return parse_single_unary_func_name(fn, func_name) &&
        (is_path_list_name(*func_name) ||
         is_tail_reverse_name(*func_name));
}

static bool parse_path_list_or_tail_reverse_func_arg(FuncCall *fn,
                                                     char **func_name,
                                                     Node **arg)
{
    char *parsed_name = NULL;

    if (!parse_path_list_or_tail_reverse_func_name(fn, &parsed_name))
    {
        return false;
    }

    if (func_name != NULL)
    {
        *func_name = parsed_name;
    }
    if (arg != NULL)
    {
        *arg = linitial(fn->args);
    }

    return true;
}

static bool parse_func_any_arg(FuncCall *fn, const char *func_name,
                               Node **arg)
{
    if (!is_func_name_unary(fn, func_name))
    {
        return false;
    }

    if (arg != NULL)
    {
        *arg = linitial(fn->args);
    }

    return true;
}

static bool parse_func_arg(FuncCall *fn, const char *func_name,
                           NodeTag arg_type, Node **arg)
{
    if (!is_func_name_unary_arg(fn, func_name, arg_type))
    {
        return false;
    }

    if (arg != NULL)
    {
        *arg = linitial(fn->args);
    }

    return true;
}

static bool parse_id_func_arg(FuncCall *fn, NodeTag arg_type, Node **arg)
{
    return parse_func_arg(fn, "id", arg_type, arg);
}

static bool parse_edge_endpoint_id_func_arg(FuncCall *fn, NodeTag arg_type,
                                            char **func_name, Node **arg)
{
    char *parsed_name = NULL;

    if (!parse_single_unary_func_arg_name(fn, arg_type, &parsed_name) ||
        !is_edge_endpoint_id_name(parsed_name))
    {
        return false;
    }

    if (func_name != NULL)
    {
        *func_name = parsed_name;
    }
    if (arg != NULL)
    {
        *arg = linitial(fn->args);
    }

    return true;
}

static bool parse_graphid_func_arg(FuncCall *fn, NodeTag arg_type,
                                   char **func_name, Node **arg)
{
    char *parsed_name = NULL;

    if (!parse_single_unary_func_arg_name(fn, arg_type, &parsed_name) ||
        (!is_id_name(parsed_name) && !is_edge_endpoint_id_name(parsed_name)))
    {
        return false;
    }

    if (func_name != NULL)
    {
        *func_name = parsed_name;
    }
    if (arg != NULL)
    {
        *arg = linitial(fn->args);
    }

    return true;
}

static bool parse_size_is_empty_func_arg(FuncCall *fn, NodeTag arg_type,
                                         char **func_name, Node **arg)
{
    char *parsed_name = NULL;

    if (!parse_single_unary_func_arg_name(fn, arg_type, &parsed_name) ||
        !is_size_is_empty_name(parsed_name))
    {
        return false;
    }

    if (func_name != NULL)
    {
        *func_name = parsed_name;
    }
    if (arg != NULL)
    {
        *arg = linitial(fn->args);
    }

    return true;
}

static bool parse_label_type_func_arg(FuncCall *fn, NodeTag arg_type,
                                      char **func_name, Node **arg)
{
    char *parsed_name = NULL;

    if (!parse_single_unary_func_arg_name(fn, arg_type, &parsed_name) ||
        !is_label_type_name(parsed_name))
    {
        return false;
    }

    if (func_name != NULL)
    {
        *func_name = parsed_name;
    }
    if (arg != NULL)
    {
        *arg = linitial(fn->args);
    }

    return true;
}

static bool parse_endpoint_func_arg(FuncCall *fn, NodeTag arg_type,
                                    char **endpoint_name, Node **arg)
{
    char *parsed_name = NULL;

    if (!parse_endpoint_func_arg_name(fn, arg_type, &parsed_name))
    {
        return false;
    }

    if (endpoint_name != NULL)
    {
        *endpoint_name = parsed_name;
    }
    if (arg != NULL)
    {
        *arg = linitial(fn->args);
    }

    return true;
}

static bool parse_endpoint_func_arg_name(FuncCall *fn, NodeTag arg_type,
                                         char **endpoint_name)
{
    return parse_single_unary_func_arg_name(fn, arg_type, endpoint_name) &&
        is_endpoint_function_name(*endpoint_name);
}

static bool parse_endpoint_func_any_arg(FuncCall *fn, char **endpoint_name,
                                        bool *start_endpoint, Node **arg)
{
    if (!is_unary_func(fn) ||
        !parse_vle_endpoint_function_name(fn, endpoint_name, start_endpoint))
    {
        return false;
    }

    if (arg != NULL)
    {
        *arg = linitial(fn->args);
    }

    return true;
}

static bool parse_edge_endpoint_columnref_arg(FuncCall *endpoint_fn,
                                              char **endpoint_name,
                                              char **edge_name)
{
    Node *edge_arg = NULL;

    return parse_endpoint_func_arg(endpoint_fn, T_ColumnRef, endpoint_name,
                                   &edge_arg) &&
        parse_single_column_ref(edge_arg, edge_name, NULL);
}

static bool parse_keys_func_arg(FuncCall *fn, NodeTag arg_type, Node **arg)
{
    if (!is_func_name_either_unary_arg(fn, "keys", "age_keys", arg_type))
    {
        return false;
    }

    if (arg != NULL)
    {
        *arg = linitial(fn->args);
    }

    return true;
}

static bool is_func_name_unary(FuncCall *fn, const char *func_name)
{
    char *actual_name = NULL;

    return parse_single_unary_func_name(fn, &actual_name) &&
        pg_strcasecmp(actual_name, func_name) == 0;
}

static bool is_func_name_unary_arg(FuncCall *fn, const char *func_name,
                                   NodeTag arg_type)
{
    char *actual_name = NULL;

    return parse_single_unary_func_arg_name(fn, arg_type, &actual_name) &&
        pg_strcasecmp(actual_name, func_name) == 0;
}

static bool is_func_name_either_unary_arg(FuncCall *fn,
                                          const char *left_name,
                                          const char *right_name,
                                          NodeTag arg_type)
{
    char *actual_name = NULL;

    return parse_single_unary_func_arg_name(fn, arg_type, &actual_name) &&
        (pg_strcasecmp(actual_name, left_name) == 0 ||
         pg_strcasecmp(actual_name, right_name) == 0);
}

static bool is_endpoint_function_name(const char *func_name)
{
    const AgeBuiltinFuncMeta *meta;

    meta = get_age_builtin_func_meta_by_name(func_name);
    return meta != NULL && meta->fast_path == AGE_FUNC_FAST_ENDPOINT;
}

static bool is_start_endpoint_function_name(const char *func_name)
{
    const AgeBuiltinFuncMeta *meta;

    meta = get_age_builtin_func_meta_by_name(func_name);
    return meta != NULL && meta->fast_path == AGE_FUNC_FAST_ENDPOINT &&
        pg_strcasecmp(meta->sql_name, "age_startnode") == 0;
}

static bool parse_vle_endpoint_function_name(FuncCall *fn,
                                             char **endpoint_name,
                                             bool *start_endpoint)
{
    char *parsed_name = NULL;

    if (!parse_endpoint_func_name(fn, &parsed_name))
    {
        return false;
    }

    if (is_start_endpoint_function_name(parsed_name))
    {
        if (start_endpoint != NULL)
        {
            *start_endpoint = true;
        }
    }
    else if (is_endpoint_function_name(parsed_name))
    {
        if (start_endpoint != NULL)
        {
            *start_endpoint = false;
        }
    }
    else
    {
        return false;
    }

    if (endpoint_name != NULL)
    {
        *endpoint_name = parsed_name;
    }

    return true;
}

static bool parse_endpoint_func_name(FuncCall *fn, char **endpoint_name)
{
    char *parsed_name = NULL;

    if (!parse_single_unary_func_name(fn, &parsed_name) ||
        !is_endpoint_function_name(parsed_name))
    {
        return false;
    }

    if (endpoint_name != NULL)
    {
        *endpoint_name = parsed_name;
    }

    return true;
}

static bool is_fixed_path_indexed_consumer_name(char *func_name)
{
    return is_id_name(func_name) ||
        is_properties_name(func_name) ||
        is_keys_name(func_name) ||
        is_label_type_name(func_name) ||
        is_labels_name(func_name) ||
        is_edge_endpoint_id_name(func_name);
}

static bool parse_fixed_path_indexed_consumer_call(FuncCall *fn,
                                                   NodeTag arg_type,
                                                   char **func_name)
{
    return parse_fixed_path_indexed_consumer_arg_name(fn, arg_type,
                                                      func_name);
}

static bool parse_fixed_path_indexed_consumer_arg_name(FuncCall *fn,
                                                       NodeTag arg_type,
                                                       char **func_name)
{
    return parse_single_unary_func_arg_name(fn, arg_type, func_name) &&
        is_fixed_path_indexed_consumer_name(*func_name);
}

static bool is_fixed_path_endpoint_vertex_consumer_name(char *func_name)
{
    return is_id_name(func_name) ||
        is_properties_name(func_name) ||
        is_keys_name(func_name) ||
        is_label_name(func_name) ||
        is_labels_name(func_name);
}

static bool parse_fixed_path_endpoint_vertex_consumer_call(FuncCall *fn,
                                                           char **func_name)
{
    return parse_fixed_path_endpoint_vertex_consumer_arg_name(fn, T_FuncCall,
                                                              func_name);
}

static bool parse_fixed_path_endpoint_vertex_consumer_call_arg(FuncCall *fn,
                                                               char **func_name,
                                                               Node **arg)
{
    if (!parse_fixed_path_endpoint_vertex_consumer_call(fn, func_name))
    {
        return false;
    }

    if (arg != NULL)
    {
        *arg = linitial(fn->args);
    }

    return true;
}

static bool parse_fixed_path_endpoint_vertex_consumer_arg_name(
    FuncCall *fn, NodeTag arg_type, char **func_name)
{
    return parse_single_unary_func_arg_name(fn, arg_type, func_name) &&
        is_fixed_path_endpoint_vertex_consumer_name(*func_name);
}

static Node *try_transform_fixed_path_indexed_consumer(
    cypher_parsestate *cpstate, char *func_name, FuncCall *indexed_consumer)
{
    if (is_id_name(func_name))
        return try_transform_fixed_path_indexed_id(cpstate, indexed_consumer);
    if (is_properties_name(func_name))
        return try_transform_fixed_path_indexed_properties(cpstate,
                                                          indexed_consumer);
    if (is_keys_name(func_name))
        return try_transform_fixed_path_indexed_keys(cpstate, indexed_consumer);
    if (is_label_type_name(func_name))
        return try_transform_fixed_path_indexed_label_type(cpstate,
                                                          indexed_consumer);
    if (is_labels_name(func_name))
        return try_transform_fixed_path_indexed_labels(cpstate,
                                                      indexed_consumer);

    return try_transform_fixed_path_indexed_edge_endpoint_id(cpstate,
                                                            indexed_consumer);
}

static Node *try_transform_fixed_path_indexed_consumer_at(
    cypher_parsestate *cpstate, char *func_name, FuncCall *fn,
    FuncCall *list_fn, int64 index)
{
    FuncCall *indexed_consumer;

    indexed_consumer = make_indexed_unary_func_call(fn, (Node *)list_fn,
                                                    index, fn->location);

    return try_transform_fixed_path_indexed_consumer(cpstate, func_name,
                                                     indexed_consumer);
}

static bool get_nonnegative_integer_const(Node *node, int64 *value)
{
    if (!is_ag_node(node, cypher_integer_const))
    {
        return false;
    }

    *value = ((cypher_integer_const *)node)->integer;
    return *value >= 0;
}

static bool get_nonnegative_integer_const_or_aconst(Node *node, int64 *value)
{
    if (get_nonnegative_integer_const(node, value))
        return true;

    if (IsA(node, A_Const) &&
        nodeTag(&((A_Const *)node)->val) == T_Integer)
    {
        *value = intVal(&((A_Const *)node)->val);
        return *value >= 0;
    }

    return false;
}

static bool parse_vle_bounds(FuncCall *fn, int64 *lower, int64 *upper)
{
    if (list_length(fn->args) < 5)
    {
        return false;
    }

    return get_nonnegative_integer_const_or_aconst(list_nth(fn->args, 3),
                                                  lower) &&
        get_nonnegative_integer_const_or_aconst(list_nth(fn->args, 4), upper);
}

static bool parse_single_indirection_string(A_Indirection *a_ind,
                                            char **field_name)
{
    if (list_length(a_ind->indirection) != 1 ||
        !IsA(linitial(a_ind->indirection), String))
    {
        return false;
    }

    if (field_name != NULL)
        *field_name = strVal(linitial(a_ind->indirection));

    return true;
}

static bool has_indirection_count(A_Indirection *a_ind, int min_count)
{
    return list_length(a_ind->indirection) >= min_count;
}

static bool parse_leading_indirection_index(A_Indirection *a_ind,
                                            A_Indices **indices)
{
    if (list_length(a_ind->indirection) < 1 ||
        !IsA(linitial(a_ind->indirection), A_Indices))
    {
        return false;
    }

    *indices = linitial(a_ind->indirection);
    return true;
}

static bool parse_single_indirection_index(A_Indirection *a_ind,
                                           A_Indices **indices)
{
    if (list_length(a_ind->indirection) != 1 ||
        !IsA(linitial(a_ind->indirection), A_Indices))
    {
        return false;
    }

    *indices = linitial(a_ind->indirection);
    return true;
}

static bool parse_single_indirection_slice(A_Indirection *a_ind,
                                           A_Indices **indices)
{
    return parse_single_indirection_index(a_ind, indices) &&
        (*indices)->is_slice;
}

static bool parse_head_last_slice_context(Node *node, FuncCall **list_fn,
                                          char **list_name,
                                          char **head_last_name,
                                          char **transform_name,
                                          A_Indices **slice_indices)
{
    FuncCall *head_last_fn;
    FuncCall *transform_fn;
    A_Indirection *slice_arg;
    Node *head_last_arg = NULL;
    Node *transform_arg = NULL;
    char *parsed_transform_name = NULL;

    if (!IsA(node, FuncCall))
        return false;

    head_last_fn = (FuncCall *)node;
    if (!parse_head_last_func_arg(head_last_fn, T_A_Indirection,
                                  head_last_name, &head_last_arg) &&
        !parse_head_last_func_arg(head_last_fn, T_FuncCall, head_last_name,
                                  &head_last_arg))
    {
        return false;
    }

    if (IsA(head_last_arg, A_Indirection))
    {
        slice_arg = (A_Indirection *)head_last_arg;
    }
    else if (IsA(head_last_arg, FuncCall))
    {
        transform_fn = (FuncCall *)head_last_arg;
        if (!parse_tail_reverse_func_arg(transform_fn, T_A_Indirection,
                                         &parsed_transform_name,
                                         &transform_arg))
            return false;

        slice_arg = (A_Indirection *)transform_arg;
    }
    else
    {
        return false;
    }

    if (!parse_path_list_slice_arg(slice_arg, list_fn, slice_indices,
                                   list_name))
    {
        return false;
    }

    if (transform_name != NULL)
        *transform_name = parsed_transform_name;

    return true;
}

static bool has_single_index(A_Indices *indices)
{
    return !indices->is_slice && indices->uidx != NULL;
}

static bool parse_single_indirection_value_index(A_Indirection *a_ind,
                                                 A_Indices **indices)
{
    return parse_single_indirection_index(a_ind, indices) &&
        has_single_index(*indices);
}

static bool parse_nonnegative_index(A_Indices *indices, int64 *index)
{
    return has_single_index(indices) &&
        get_nonnegative_integer_const_or_aconst(indices->uidx, index);
}

static bool parse_single_indirection_nonnegative_index(A_Indirection *a_ind,
                                                       A_Indices **indices,
                                                       int64 *index)
{
    A_Indices *parsed_indices = NULL;

    if (!parse_single_indirection_index(a_ind, &parsed_indices) ||
        !parse_nonnegative_index(parsed_indices, index))
    {
        return false;
    }

    if (indices != NULL)
        *indices = parsed_indices;

    return true;
}

static bool parse_leading_indirection_nonnegative_index(A_Indirection *a_ind,
                                                       int64 *index)
{
    A_Indices *indices = NULL;

    return parse_leading_indirection_index(a_ind, &indices) &&
        parse_nonnegative_index(indices, index);
}

static bool parse_zero_index(A_Indices *indices)
{
    int64 index;

    return parse_nonnegative_index(indices, &index) && index == 0;
}

static bool parse_single_indirection_zero_index(A_Indirection *a_ind)
{
    A_Indices *indices = NULL;

    return parse_single_indirection_index(a_ind, &indices) &&
        parse_zero_index(indices);
}

static Const *make_agtype_string_key_const(Node *node)
{
    return makeConst(AGTYPEOID, -1, InvalidOid, -1,
                     string_to_agtype(strVal(node)), false, false);
}

static bool parse_optional_nonnegative_bound(Node *node, int64 *value)
{
    return node == NULL || get_nonnegative_integer_const_or_aconst(node, value);
}

static bool parse_optional_upper_bound(Node *node, int64 *upper,
                                       int64 default_upper)
{
    if (node == NULL)
    {
        *upper = default_upper;
        return true;
    }

    return parse_optional_nonnegative_bound(node, upper);
}

static bool get_fixed_path_list_len(const char *list_name, int64 *list_len)
{
    if (is_nodes_list_name(list_name))
    {
        *list_len = 2;
        return true;
    }
    if (is_relationships_list_name(list_name))
    {
        *list_len = 1;
        return true;
    }

    return false;
}

static bool get_fixed_path_head_last_index(const char *head_last_name,
                                           const char *list_name,
                                           bool tail_transform,
                                           int64 *index)
{
    int64 lower = 0;
    int64 upper;

    if (!get_fixed_path_list_len(list_name, &upper))
        return false;

    if (tail_transform)
    {
        if (!is_nodes_list_name(list_name))
            return false;

        advance_tail_slice_lower_bound(&lower, upper);
    }

    *index = get_slice_boundary_index(head_last_name, false, lower, upper);

    return true;
}

static bool parse_fixed_slice_bounds(A_Indices *indices, int64 list_len,
                                     int64 *lower, int64 *upper)
{
    *lower = 0;

    if (!parse_optional_nonnegative_bound(indices->lidx, lower))
        return false;

    if (!parse_optional_upper_bound(indices->uidx, upper, list_len))
        return false;

    clamp_slice_bounds_to_len(lower, upper, list_len);

    return true;
}

static bool parse_nonempty_fixed_slice_bounds(A_Indices *indices,
                                              int64 list_len,
                                              const char *transform_name,
                                              int64 *lower, int64 *upper)
{
    if (!parse_fixed_slice_bounds(indices, list_len, lower, upper))
        return false;

    if (transform_name != NULL &&
        is_tail_name(transform_name))
    {
        advance_tail_slice_lower_bound(lower, *upper);
    }

    return has_nonempty_slice_bounds(*lower, *upper);
}

static void clamp_slice_bounds_to_len(int64 *lower, int64 *upper,
                                      int64 list_len)
{
    if (*lower > list_len)
        *lower = list_len;
    if (*upper > list_len)
        *upper = list_len;
    if (*upper < *lower)
        *upper = *lower;
}

static void advance_tail_slice_lower_bound(int64 *lower, int64 upper)
{
    (*lower)++;
    if (*lower > upper)
        *lower = upper;
}

static int64 get_slice_boundary_index(const char *head_last_name,
                                      bool reversed, int64 lower,
                                      int64 upper)
{
    if (reversed)
        return is_head_name(head_last_name) ? upper - 1 : lower;

    return is_head_name(head_last_name) ? lower : upper - 1;
}

static bool has_nonempty_slice_bounds(int64 lower, int64 upper)
{
    return upper > lower;
}

static bool append_agtype_access_indirections(cypher_parsestate *cpstate,
                                              List **args, List *indirections,
                                              bool skip_first)
{
    ListCell *lc = NULL;

    foreach (lc, indirections)
    {
        Node *node = lfirst(lc);

        if (skip_first)
        {
            skip_first = false;
            continue;
        }

        if (IsA(node, String))
        {
            *args = lappend(*args, make_agtype_string_key_const(node));
        }
        else if (IsA(node, A_Indices))
        {
            A_Indices *indices = (A_Indices *)node;

            if (!has_single_index(indices))
            {
                return false;
            }
            *args = lappend(*args,
                            transform_cypher_expr_recurse(cpstate,
                                                          indices->uidx));
        }
        else
        {
            return false;
        }
    }

    return true;
}

static bool parse_fixed_relationship_slice_head_last_index(
    Node *node, FuncCall **relationships_fn, int64 *index)
{
    FuncCall *list_fn;
    char *list_name = NULL;

    if (!parse_fixed_path_head_last_slice_index(node, false, &list_fn,
                                                &list_name, index) &&
        !parse_fixed_path_head_last_slice_index(node, true, &list_fn,
                                                &list_name, index))
    {
        return false;
    }

    if (!is_relationships_list_name(list_name) || *index != 0)
        return false;

    *relationships_fn = list_fn;
    return true;
}

static bool parse_vle_path_nested_transform_index(cypher_parsestate *cpstate,
                                                  A_Indirection *a_ind,
                                                  Expr **vle_expr,
                                                  char **list_name,
                                                  Const **lower_expr,
                                                  Const **upper_expr,
                                                  int64 *mode)
{
    FuncCall *outer_fn = NULL;
    FuncCall *inner_fn = NULL;
    Node *inner_arg = NULL;
    A_Indices *indices = NULL;
    char *outer_name = NULL;
    char *inner_name = NULL;
    int64 index;
    int64 mode_flag = 0;

    if (!IsA(a_ind->arg, FuncCall) ||
        !parse_single_indirection_nonnegative_index(a_ind, &indices, &index))
    {
        return false;
    }

    outer_fn = (FuncCall *)a_ind->arg;
    if (!parse_tail_reverse_func_arg(outer_fn, T_FuncCall, &outer_name,
                                     &inner_arg))
    {
        return false;
    }

    inner_fn = (FuncCall *)inner_arg;
    if (!parse_tail_reverse_func_any_arg(inner_fn, &inner_name, NULL) ||
        !get_vle_nested_tail_reverse_mode_flag(outer_name, inner_name,
                                               &mode_flag))
    {
        return false;
    }

    if (!parse_vle_nested_tail_reverse_base_mode(cpstate, inner_fn, mode_flag,
                                                 vle_expr, list_name, mode))
    {
        return false;
    }

    if (*vle_expr == NULL)
    {
        return false;
    }

    *lower_expr = make_agtype_integer_const(index, exprLocation(indices->uidx));
    *upper_expr = make_agtype_integer_const(index + 1,
                                            exprLocation(indices->uidx));

    return true;
}

static bool parse_arbitrary_vle_nested_tail_reverse_base_mode(
    cypher_parsestate *cpstate, FuncCall *inner_fn, int64 mode_flag,
    Expr **vle_expr, char **list_name, int64 *mode)
{
    Node *inner_arg = NULL;

    if (!parse_path_list_or_tail_reverse_func_arg(inner_fn, NULL,
                                                  &inner_arg))
    {
        return false;
    }

    if (IsA(inner_arg, FuncCall))
    {
        if (!parse_arbitrary_vle_path_list_function(cpstate, inner_arg,
                                                    vle_expr, list_name))
        {
            return false;
        }

        *mode = is_nodes_list_name(*list_name) ? 6 + mode_flag :
            4 + mode_flag;
        return true;
    }

    return parse_vle_path_or_raw_edge_list_mode(
        cpstate, inner_arg, vle_expr, list_name, 6 + mode_flag,
        4 + mode_flag, mode, true);
}

static bool parse_arbitrary_vle_path_nested_transform_index(
    cypher_parsestate *cpstate, A_Indirection *a_ind, Expr **vle_expr,
    char **list_name, Const **lower_expr, Const **upper_expr, int64 *mode)
{
    FuncCall *outer_fn = NULL;
    FuncCall *inner_fn = NULL;
    Node *inner_arg = NULL;
    A_Indices *indices = NULL;
    char *outer_name = NULL;
    char *inner_name = NULL;
    int64 index;
    int64 mode_flag = 0;

    if (!IsA(a_ind->arg, FuncCall) ||
        !parse_single_indirection_nonnegative_index(a_ind, &indices, &index))
    {
        return false;
    }

    outer_fn = (FuncCall *)a_ind->arg;
    if (!parse_tail_reverse_func_arg(outer_fn, T_FuncCall, &outer_name,
                                     &inner_arg))
    {
        return false;
    }

    inner_fn = (FuncCall *)inner_arg;
    if (!parse_tail_reverse_func_any_arg(inner_fn, &inner_name, NULL) ||
        !get_vle_nested_tail_reverse_mode_flag(outer_name, inner_name,
                                               &mode_flag))
    {
        return false;
    }

    if (!parse_arbitrary_vle_nested_tail_reverse_base_mode(
            cpstate, inner_fn, mode_flag, vle_expr, list_name, mode))
    {
        return false;
    }

    if (*vle_expr == NULL)
    {
        return false;
    }

    *lower_expr = make_agtype_integer_const(index, exprLocation(indices->uidx));
    *upper_expr = make_agtype_integer_const(index + 1,
                                            exprLocation(indices->uidx));

    return true;
}

static bool parse_vle_tail_access_source(cypher_parsestate *cpstate,
                                         Node *tail_arg, Expr **vle_expr,
                                         char **list_name,
                                         int64 *tail_index)
{
    FuncCall *list_fn = NULL;
    Node *source_arg = NULL;
    char *source_name = NULL;

    if (!IsA(tail_arg, FuncCall))
    {
        return parse_vle_path_or_raw_edge_list(cpstate, tail_arg, vle_expr,
                                               list_name, false);
    }

    list_fn = (FuncCall *)tail_arg;
    if (!parse_path_list_or_tail_reverse_func_arg(list_fn, &source_name,
                                                  &source_arg))
    {
        return false;
    }

    if (is_reverse_name(source_name) &&
        !IsA(source_arg, FuncCall))
    {
        if (!parse_vle_path_or_raw_edge_list(cpstate, source_arg,
                                             vle_expr, list_name, false))
        {
            return false;
        }

        *tail_index = -*tail_index - 3;
        return true;
    }

    if (!is_path_list_name(source_name))
    {
        return false;
    }

    return parse_vle_path_list_function(cpstate, (Node *)list_fn, vle_expr,
                                        list_name, true);
}

static Node *try_transform_vle_path_tail_access(cypher_parsestate *cpstate,
                                                A_Indirection *a_ind)
{
    FuncCall *tail_fn = NULL;
    A_Indices *indices = NULL;
    Node *tail_arg = NULL;
    Expr *vle_expr = NULL;
    char *list_name = NULL;
    int64 tail_index;

    if (!IsA(a_ind->arg, FuncCall) ||
        !parse_single_indirection_nonnegative_index(a_ind, &indices,
                                                   &tail_index))
    {
        return NULL;
    }

    tail_fn = (FuncCall *)a_ind->arg;
    if (!parse_func_any_arg(tail_fn, "tail", &tail_arg))
    {
        return NULL;
    }

    if (!parse_vle_tail_access_source(cpstate, tail_arg, &vle_expr,
                                      &list_name, &tail_index))
    {
        return NULL;
    }

    return make_vle_materialized_index_expr(vle_expr, list_name,
                                            get_vle_tail_reverse_access_index(
                                                true, tail_index),
                                            exprLocation(indices->uidx));
}

static Node *try_transform_vle_path_reverse_access(cypher_parsestate *cpstate,
                                                   A_Indirection *a_ind)
{
    FuncCall *reverse_fn = NULL;
    A_Indices *indices = NULL;
    Node *reverse_arg = NULL;
    Expr *vle_expr = NULL;
    char *list_name = NULL;
    int64 reverse_index;

    if (!IsA(a_ind->arg, FuncCall) ||
        !parse_single_indirection_nonnegative_index(a_ind, &indices,
                                                   &reverse_index))
    {
        return NULL;
    }

    reverse_fn = (FuncCall *)a_ind->arg;
    if (!parse_func_any_arg(reverse_fn, "reverse", &reverse_arg))
    {
        return NULL;
    }

    if (!parse_vle_path_or_raw_edge_list(cpstate, reverse_arg, &vle_expr,
                                         &list_name, true))
    {
        return NULL;
    }

    return make_vle_materialized_index_expr(vle_expr, list_name,
                                            get_vle_tail_reverse_access_index(
                                                false, reverse_index),
                                            exprLocation(indices->uidx));
}

static Node *try_transform_vle_path_nested_transform_access(
    cypher_parsestate *cpstate, A_Indirection *a_ind)
{
    return make_vle_nested_transform_index_mode_expr(
        cpstate, a_ind, 0, exprLocation((Node *)a_ind), NULL, false);
}

static bool parse_vle_named_indirection(
    cypher_parsestate *cpstate, A_Indirection *a_ind,
    const char *expected_name, bool is_slice, A_Indices **indices,
    Expr **vle_expr, bool allow_visible)
{
    if (!IsA(a_ind->arg, FuncCall))
    {
        return false;
    }

    if (is_slice)
    {
        if (!parse_single_indirection_slice(a_ind, indices))
        {
            return false;
        }
    }
    else if (!parse_single_indirection_value_index(a_ind, indices))
    {
        return false;
    }

    return parse_vle_named_path_list_function(
        cpstate, a_ind->arg, expected_name, vle_expr, allow_visible);
}

static Node *make_vle_list_slice_mode_bounds_expr(Oid func_oid,
                                                  Expr *vle_expr,
                                                  Node *lower_expr,
                                                  Node *upper_expr,
                                                  int64 mode,
                                                  int location)
{
    return make_vle_slice_mode_expr(func_oid, AGTYPEOID, (Node *)vle_expr,
                                    lower_expr, upper_expr, mode, location);
}

static Node *try_transform_vle_path_nodes_access(cypher_parsestate *cpstate,
                                                 A_Indirection *a_ind)
{
    A_Indices *indices = NULL;
    Expr *vle_expr = NULL;
    Node *index_expr = NULL;

    if (!parse_vle_named_indirection(cpstate, a_ind, "nodes", false,
                                     &indices, &vle_expr, true))
    {
        return NULL;
    }

    index_expr = transform_cypher_expr_recurse(cpstate, indices->uidx);

    return make_vle_binary_agtype_expr(get_age_materialize_vle_node_at_oid(),
                                       (Node *)vle_expr, index_expr);
}

static Node *try_transform_vle_path_nodes_slice(cypher_parsestate *cpstate,
                                                A_Indirection *a_ind)
{
    A_Indices *indices = NULL;
    Expr *vle_expr = NULL;
    Node *lower_expr = NULL;
    Node *upper_expr = NULL;

    if (!parse_vle_named_indirection(cpstate, a_ind, "nodes", true,
                                     &indices, &vle_expr, true))
    {
        return NULL;
    }

    transform_slice_bounds_or_null(cpstate, indices, &lower_expr, &upper_expr);

    return make_vle_ternary_agtype_expr(get_age_materialize_vle_nodes_slice_oid(),
                                        (Node *)vle_expr, lower_expr,
                                        upper_expr);
}

static Node *try_transform_vle_path_list_slice(cypher_parsestate *cpstate,
                                               A_Indirection *a_ind)
{
    Expr *vle_expr = NULL;
    Node *lower_expr = NULL;
    Node *upper_expr = NULL;
    Oid func_oid;
    int64 mode = 0;

    if (!parse_vle_path_list_slice(cpstate, a_ind, &vle_expr, &lower_expr,
                                   &upper_expr, &mode, false))
    {
        return NULL;
    }

    func_oid = get_age_materialize_vle_list_slice_oid();
    return make_vle_list_slice_mode_bounds_expr(func_oid, vle_expr,
                                                lower_expr, upper_expr,
                                                mode,
                                                exprLocation((Node *)a_ind));
}

static bool retarget_vle_terminal_property_output(cypher_parsestate *cpstate,
                                                  Node *vle_path,
                                                  Const *key_const)
{
    ParseState *pstate = &cpstate->pstate;
    RangeTblEntry *outer_rte;
    RangeTblEntry *vle_rte;
    TargetEntry *sub_te;
    Var *outer_var;
    Var *inner_var;
    Query *subquery;
    List *row;
    Const *marker;

    if (vle_path == NULL || !IsA(vle_path, Var) ||
        key_const == NULL || key_const->consttype != AGTYPEOID)
    {
        return false;
    }

    outer_var = castNode(Var, vle_path);
    if (outer_var->varlevelsup != 0 ||
        outer_var->varno <= 0 ||
        outer_var->varno > list_length(pstate->p_rtable))
    {
        return false;
    }

    outer_rte = rt_fetch(outer_var->varno, pstate->p_rtable);
    if (outer_rte == NULL || outer_rte->rtekind != RTE_SUBQUERY ||
        outer_rte->subquery == NULL)
    {
        return false;
    }

    subquery = outer_rte->subquery;
    sub_te = get_tle_by_resno(subquery->targetList, outer_var->varattno);
    if (sub_te == NULL || sub_te->expr == NULL || !IsA(sub_te->expr, Var))
    {
        return false;
    }

    inner_var = castNode(Var, sub_te->expr);
    if (inner_var->varlevelsup != 0 ||
        inner_var->varno <= 0 ||
        inner_var->varno > list_length(subquery->rtable))
    {
        return false;
    }

    vle_rte = rt_fetch(inner_var->varno, subquery->rtable);
    if (vle_rte == NULL ||
        vle_rte->rtekind != RTE_VALUES ||
        list_length(vle_rte->values_lists) != 1 ||
        vle_rte->eref == NULL ||
        vle_rte->eref->colnames == NIL)
    {
        return false;
    }

    row = linitial_node(List, vle_rte->values_lists);
    if (list_length(row) == AGE_VLE_STREAM_ARG_TERMINAL_PROPERTY + 3)
    {
        Const *existing_key = list_nth_node(
            Const, row, AGE_VLE_STREAM_ARG_TERMINAL_PROPERTY + 2);

        return !existing_key->constisnull && equal(existing_key, key_const);
    }
    if (list_length(row) != AGE_VLE_STREAM_ARG_GRAMMAR_NODE + 3 ||
        !IsA(lsecond(row), Const))
    {
        return false;
    }

    marker = lsecond_node(Const, row);
    if (marker->constisnull ||
        marker->consttype != TEXTOID ||
        strcmp(TextDatumGetCString(marker->constvalue),
               AGE_VLE_STREAM_MARKER) != 0)
    {
        return false;
    }

    row = lappend(row, copyObject(key_const));
    vle_rte->values_lists = list_make1(row);
    vle_rte->coltypes = lappend_oid(vle_rte->coltypes, AGTYPEOID);
    vle_rte->coltypmods = lappend_int(vle_rte->coltypmods, -1);
    vle_rte->colcollations = lappend_oid(vle_rte->colcollations, InvalidOid);
    vle_rte->eref->colnames =
        lappend(vle_rte->eref->colnames,
                makeString("__age_vle_terminal_property"));
    if (vle_rte->alias != NULL && vle_rte->alias != vle_rte->eref)
    {
        vle_rte->alias->colnames =
            lappend(vle_rte->alias->colnames,
                    makeString("__age_vle_terminal_property"));
    }

    return true;
}

static bool retarget_vle_terminal_properties_output(
    cypher_parsestate *cpstate, Node *raw_properties)
{
    ParseState *pstate = &cpstate->pstate;
    RangeTblEntry *outer_rte;
    RangeTblEntry *vle_rte;
    TargetEntry *sub_te;
    Var *outer_var;
    Var *inner_var;
    FuncExpr *properties_expr;
    Query *subquery;
    List *row;
    Const *marker;

    if (raw_properties == NULL || !IsA(raw_properties, Var))
    {
        return false;
    }

    outer_var = castNode(Var, raw_properties);
    if (outer_var->varlevelsup != 0 ||
        outer_var->varno <= 0 ||
        outer_var->varno > list_length(pstate->p_rtable))
    {
        return false;
    }

    outer_rte = rt_fetch(outer_var->varno, pstate->p_rtable);
    if (outer_rte == NULL || outer_rte->rtekind != RTE_SUBQUERY ||
        outer_rte->subquery == NULL)
    {
        return false;
    }

    subquery = outer_rte->subquery;
    sub_te = get_tle_by_resno(subquery->targetList, outer_var->varattno);
    if (sub_te == NULL || sub_te->expr == NULL ||
        !IsA(sub_te->expr, FuncExpr))
    {
        return false;
    }

    properties_expr = castNode(FuncExpr, sub_te->expr);
    if (properties_expr->funcid != get_age_vle_terminal_vertex_properties_oid() ||
        list_length(properties_expr->args) != 1 ||
        !IsA(linitial(properties_expr->args), Var))
    {
        return false;
    }

    inner_var = linitial_node(Var, properties_expr->args);
    if (inner_var->varlevelsup != 0 ||
        inner_var->varno <= 0 ||
        inner_var->varno > list_length(subquery->rtable))
    {
        return false;
    }

    vle_rte = rt_fetch(inner_var->varno, subquery->rtable);
    if (vle_rte == NULL ||
        vle_rte->rtekind != RTE_VALUES ||
        list_length(vle_rte->values_lists) != 1 ||
        vle_rte->eref == NULL ||
        vle_rte->eref->colnames == NIL)
    {
        return false;
    }

    row = linitial_node(List, vle_rte->values_lists);
    if (list_length(row) == AGE_VLE_STREAM_ARG_TERMINAL_PROPERTY + 3)
    {
        Const *existing_key = list_nth_node(
            Const, row, AGE_VLE_STREAM_ARG_TERMINAL_PROPERTY + 2);

        if (!existing_key->constisnull)
        {
            return false;
        }
        sub_te->expr = (Expr *)copyObject(inner_var);
        return true;
    }
    if (list_length(row) != AGE_VLE_STREAM_ARG_GRAMMAR_NODE + 3 ||
        !IsA(lsecond(row), Const))
    {
        return false;
    }

    marker = lsecond_node(Const, row);
    if (marker->constisnull ||
        marker->consttype != TEXTOID ||
        strcmp(TextDatumGetCString(marker->constvalue),
               AGE_VLE_STREAM_MARKER) != 0)
    {
        return false;
    }

    row = lappend(row, make_null_agtype_const());
    vle_rte->values_lists = list_make1(row);
    vle_rte->coltypes = lappend_oid(vle_rte->coltypes, AGTYPEOID);
    vle_rte->coltypmods = lappend_int(vle_rte->coltypmods, -1);
    vle_rte->colcollations = lappend_oid(vle_rte->colcollations, InvalidOid);
    vle_rte->eref->colnames =
        lappend(vle_rte->eref->colnames,
                makeString("__age_vle_terminal_property"));
    if (vle_rte->alias != NULL && vle_rte->alias != vle_rte->eref)
    {
        vle_rte->alias->colnames =
            lappend(vle_rte->alias->colnames,
                    makeString("__age_vle_terminal_property"));
    }
    sub_te->expr = (Expr *)copyObject(inner_var);

    return true;
}

static Node *try_transform_vle_terminal_vertex_property_access(
    cypher_parsestate *cpstate, A_Indirection *a_ind)
{
    char *var_name = NULL;
    char *field_name = NULL;
    transform_entity *entity = NULL;
    FuncExpr *terminal_expr = NULL;
    Node *vle_path = NULL;
    Node *properties = NULL;
    Const *key_const = NULL;
    int location = exprLocation((Node *)a_ind);

    if (!parse_single_column_ref(a_ind->arg, &var_name, &location) ||
        !parse_single_indirection_string(a_ind, &field_name))
    {
        return NULL;
    }

    entity = find_variable(cpstate, var_name);
    if (entity == NULL || entity->type != ENT_VERTEX ||
        entity->expr == NULL || !IsA(entity->expr, FuncExpr))
    {
        return NULL;
    }

    terminal_expr = castNode(FuncExpr, entity->expr);
    if (terminal_expr->funcid != get_age_vle_terminal_vertex_oid() ||
        list_length(terminal_expr->args) != 1)
    {
        return NULL;
    }

    properties = make_raw_attr_var(&cpstate->pstate, var_name,
                                   AG_VERTEX_COLNAME_PROPERTIES, location);
    if (properties == NULL)
    {
        return NULL;
    }

    key_const = make_agtype_string_key_const((Node *)makeString(field_name));

    vle_path = make_raw_attr_var(&cpstate->pstate, var_name, "edges",
                                 location);
    if (vle_path != NULL)
    {
        if (retarget_vle_terminal_property_output(cpstate, vle_path,
                                                  key_const))
        {
            return copyObject(vle_path);
        }

        return (Node *)makeFuncExpr(
            get_age_vle_terminal_vertex_property_from_path_oid(),
            AGTYPEOID, list_make2(vle_path, key_const),
            InvalidOid, InvalidOid, COERCE_EXPLICIT_CALL);
    }

    if (IsA(properties, FuncExpr))
    {
        FuncExpr *properties_expr = castNode(FuncExpr, properties);

        if (properties_expr->funcid ==
            get_age_vle_terminal_vertex_properties_oid() &&
            list_length(properties_expr->args) == 1)
        {
            return (Node *)makeFuncExpr(
                get_age_vle_terminal_vertex_property_from_path_oid(),
                AGTYPEOID,
                list_make2(copyObject(linitial(properties_expr->args)),
                           key_const),
                InvalidOid, InvalidOid, COERCE_EXPLICIT_CALL);
        }
    }

    return (Node *)makeFuncExpr(get_age_vle_terminal_vertex_property_oid(),
                                AGTYPEOID, list_make2(properties, key_const),
                                InvalidOid, InvalidOid,
                                COERCE_EXPLICIT_CALL);
}

static Node *try_transform_vle_boundary_direct_property_access(
    cypher_parsestate *cpstate, A_Indirection *a_ind)
{
    FuncCall *fn = NULL;
    Node *head_last_arg = NULL;
    Expr *vle_expr = NULL;
    Node *index_expr = NULL;
    Const *key_expr = NULL;
    char *field_name = NULL;
    char *head_last_name = NULL;
    char *list_name = NULL;
    int64 index;
    bool tail_last = false;

    if (!IsA(a_ind->arg, FuncCall) ||
        !parse_single_indirection_string(a_ind, &field_name))
    {
        return NULL;
    }

    fn = (FuncCall *)a_ind->arg;
    if (!parse_head_last_func_any_arg(fn, &head_last_name, &head_last_arg) ||
        !parse_current_vle_head_last_source(cpstate, head_last_name,
                                            head_last_arg, true, true,
                                            &vle_expr, &list_name, &index,
                                            &tail_last) ||
        vle_expr == NULL || tail_last)
    {
        return NULL;
    }

    key_expr = make_agtype_string_key_const((Node *)makeString(field_name));
    index_expr = (Node *)make_agtype_integer_const(index,
                                                   exprLocation(a_ind->arg));

    return make_vle_indexed_property_expr(vle_expr, list_name, index_expr,
                                          key_expr);
}

static FuncExpr *make_vle_index_properties_expr(cypher_parsestate *cpstate,
                                                A_Indirection *indexed_arg,
                                                int location)
{
    Expr *vle_expr = NULL;
    Node *index_expr = NULL;
    char *list_name = NULL;
    FuncExpr *properties_expr = NULL;

    properties_expr = (FuncExpr *)make_vle_nested_transform_index_mode_expr(
        cpstate, indexed_arg, VLE_SLICE_BOUNDARY_PROPERTIES_OFFSET, location,
        NULL, false);
    if (properties_expr != NULL)
        return properties_expr;

    properties_expr = (FuncExpr *)make_current_vle_edge_indexed_field_expr(
        cpstate, indexed_arg, "properties");
    if (properties_expr != NULL)
        return properties_expr;

    if (!parse_arbitrary_vle_path_indexed_list_index(cpstate, indexed_arg,
                                                     &vle_expr, &list_name,
                                                     &index_expr) &&
        !parse_vle_path_indexed_list_index(cpstate, indexed_arg, &vle_expr,
                                           &list_name, &index_expr))
    {
        return NULL;
    }

    return (FuncExpr *)make_vle_indexed_field_expr(
        list_name, "properties", vle_expr, index_expr);
}

static Node *try_transform_vle_path_boundary_property_access(
    cypher_parsestate *cpstate, A_Indirection *a_ind)
{
    Node *direct_expr = NULL;
    FuncExpr *properties_expr = NULL;
    FuncExpr *access_expr = NULL;
    List *args = NIL;
    Expr *vle_expr = NULL;
    Node *index_expr = NULL;
    A_Indices *indices = NULL;
    char *list_name = NULL;
    int64 index;
    int64 mode_offset;
    bool start_endpoint = false;
    bool endpoint_index = false;
    bool tail_last = false;
    bool tail_last_endpoint = false;
    bool skip_first_indirection = false;

    direct_expr = try_transform_vle_boundary_direct_property_access(cpstate,
                                                                    a_ind);
    if (direct_expr != NULL)
    {
        return direct_expr;
    }

    if (!has_indirection_count(a_ind, 1))
    {
        return NULL;
    }

    if (IsA(a_ind->arg, A_Indirection))
    {
        A_Indirection *nested_arg = (A_Indirection *)a_ind->arg;

        properties_expr = make_vle_index_properties_expr(
            cpstate, nested_arg, exprLocation((Node *)nested_arg));
        if (properties_expr != NULL)
        {
            goto build_access;
        }

        return NULL;
    }

    if (has_indirection_count(a_ind, 2) &&
        parse_leading_indirection_index(a_ind, &indices))
    {
        A_Indirection indexed_arg;

        MemSet(&indexed_arg, 0, sizeof(A_Indirection));
        indexed_arg.type = T_A_Indirection;
        indexed_arg.arg = a_ind->arg;
        indexed_arg.indirection = list_make1(indices);

        properties_expr = make_vle_index_properties_expr(
            cpstate, &indexed_arg, exprLocation((Node *)a_ind));
        if (properties_expr != NULL)
        {
            skip_first_indirection = true;
            goto build_access;
        }
    }

    if (!IsA(a_ind->arg, FuncCall))
    {
        return NULL;
    }

    {
        FuncCall *arg_fn = (FuncCall *)a_ind->arg;
        Node *direct_endpoint_arg = NULL;
        Node *endpoint_arg = NULL;

        if (parse_endpoint_func_arg(arg_fn, T_A_Indirection, NULL,
                                    &direct_endpoint_arg) &&
            is_vle_direct_path_list_func(
                ((A_Indirection *)direct_endpoint_arg)->arg))
        {
            return NULL;
        }

        if (parse_vle_endpoint_mode_offset_either_arg(
                arg_fn, T_FuncCall, T_A_Indirection,
                VLE_SLICE_BOUNDARY_START_PROPERTIES_OFFSET,
                VLE_SLICE_BOUNDARY_END_PROPERTIES_OFFSET, NULL, NULL,
                &mode_offset, &endpoint_arg))
        {
            if (IsA(endpoint_arg, FuncCall))
            {
                properties_expr = (FuncExpr *)
                    transform_vle_path_any_slice_boundary_head_last(
                        cpstate, (FuncCall *)endpoint_arg, mode_offset);
                if (properties_expr != NULL)
                {
                    goto build_access;
                }
            }
            else
            {
                A_Indirection *endpoint_indirection =
                    (A_Indirection *)endpoint_arg;

                properties_expr = (FuncExpr *)
                    transform_vle_path_nested_transform_index_endpoint(
                        cpstate, arg_fn, mode_offset, mode_offset,
                        exprLocation((Node *)endpoint_indirection));
                if (properties_expr != NULL)
                {
                    goto build_access;
                }
            }
        }
    }

    properties_expr =
        (FuncExpr *)transform_vle_path_any_slice_boundary_head_last(
            cpstate, (FuncCall *)a_ind->arg,
            VLE_SLICE_BOUNDARY_PROPERTIES_OFFSET);
    if (properties_expr != NULL)
    {
        goto build_access;
    }

    tail_last_endpoint = parse_vle_tail_last_endpoint(cpstate, a_ind->arg,
                                                      &vle_expr,
                                                      &start_endpoint);
    if (!tail_last_endpoint)
    {
        endpoint_index = parse_vle_edge_endpoint_index(cpstate, a_ind->arg,
                                                       &vle_expr, &index_expr,
                                                       &start_endpoint);
    }
    if (!tail_last_endpoint && !endpoint_index)
    {
        tail_last = parse_vle_path_tail_last_list(cpstate, a_ind->arg,
                                                  &vle_expr, &list_name);
    }
    if (!tail_last_endpoint && !endpoint_index && !tail_last &&
        !parse_vle_path_boundary_list_index(cpstate, a_ind->arg, &vle_expr,
                                            &list_name, &index))
    {
        return NULL;
    }

    if (tail_last_endpoint)
    {
        properties_expr = (FuncExpr *)make_vle_tail_last_endpoint_field_expr(
            "properties", vle_expr, start_endpoint, exprLocation(a_ind->arg));
        if (properties_expr == NULL)
            return NULL;
    }
    else if (endpoint_index)
    {
        properties_expr = (FuncExpr *)make_vle_edge_endpoint_field_expr(
            "properties", vle_expr, index_expr, start_endpoint,
            exprLocation(a_ind->arg));
        if (properties_expr == NULL)
            return NULL;
    }
    else if (tail_last)
    {
        properties_expr = (FuncExpr *)make_vle_tail_last_field_expr(
            list_name, "properties", vle_expr, exprLocation(a_ind->arg));
        if (properties_expr == NULL)
            return NULL;
    }
    else
    {
        properties_expr = (FuncExpr *)make_vle_indexed_properties_expr(
            vle_expr, list_name, index, exprLocation(a_ind->arg));
    }

build_access:
    properties_expr->location = exprLocation(a_ind->arg);

    args = lappend(args, properties_expr);
    if (!append_agtype_access_indirections(cpstate, &args, a_ind->indirection,
                                           skip_first_indirection))
    {
        return NULL;
    }

    access_expr = make_agtype_access_expr(args, exprLocation(a_ind->arg),
                                          true);

    return (Node *)access_expr;
}

static Node *try_transform_vle_path_relationships_access(
    cypher_parsestate *cpstate, A_Indirection *a_ind)
{
    A_Indices *indices = NULL;
    Expr *vle_expr = NULL;
    Node *index_expr = NULL;

    if (!parse_vle_named_indirection(cpstate, a_ind, "relationships", false,
                                     &indices, &vle_expr, true))
    {
        return NULL;
    }

    index_expr = transform_cypher_expr_recurse(cpstate, indices->uidx);

    return make_vle_binary_agtype_expr(get_age_materialize_vle_edge_at_oid(),
                                       (Node *)vle_expr, index_expr);
}

static bool parse_current_raw_vle_reverse_source(cypher_parsestate *cpstate,
                                                 FuncCall *reverse_fn,
                                                 Expr **vle_expr,
                                                 char **list_name)
{
    Node *reverse_arg = NULL;

    if (!parse_func_any_arg(reverse_fn, "reverse", &reverse_arg) ||
        IsA(reverse_arg, FuncCall))
    {
        return false;
    }

    return parse_vle_path_or_raw_edge_list(cpstate, reverse_arg,
                                           vle_expr, list_name, false);
}

static Node *try_transform_vle_edge_reverse_access(cypher_parsestate *cpstate,
                                                   A_Indirection *a_ind)
{
    FuncCall *reverse_fn = NULL;
    A_Indices *indices = NULL;
    Expr *vle_expr = NULL;
    Node *index_expr = NULL;
    char *list_name = NULL;
    Oid func_oid;
    int64 reverse_index;

    if (!IsA(a_ind->arg, FuncCall) ||
        !parse_single_indirection_value_index(a_ind, &indices))
    {
        return NULL;
    }

    reverse_fn = (FuncCall *)a_ind->arg;
    if (!parse_current_raw_vle_reverse_source(cpstate, reverse_fn, &vle_expr,
                                              &list_name))
    {
        return NULL;
    }

    if (parse_nonnegative_index(indices, &reverse_index))
    {
        index_expr = (Node *)make_agtype_integer_const(
            -reverse_index - 1, exprLocation(indices->uidx));
        func_oid = get_age_materialize_vle_edge_at_oid();
    }
    else
    {
        index_expr = transform_cypher_expr_recurse(cpstate, indices->uidx);
        func_oid = get_age_materialize_vle_edge_reversed_at_oid();
    }

    return make_vle_binary_agtype_expr(func_oid, (Node *)vle_expr,
                                       index_expr);
}

static AgeSemanticSourceKind classify_path_length_source(
    cypher_parsestate *cpstate, Node *arg, Expr **vle_expr)
{
    char *path_name = NULL;
    transform_entity *entity;

    *vle_expr = get_arbitrary_single_vle_path_expr(cpstate, arg);
    if (*vle_expr != NULL)
    {
        return AGE_SEM_SOURCE_COMPACT_VLE_PATH;
    }

    if (parse_single_column_ref(arg, &path_name, NULL))
    {
        entity = find_variable(cpstate, path_name);
        if (entity != NULL && entity->type == ENT_PATH)
        {
            return AGE_SEM_SOURCE_FIXED_PATH;
        }
    }

    return AGE_SEM_SOURCE_UNKNOWN;
}

static AgeSemanticSourceKind classify_path_list_count_source(
    cypher_parsestate *cpstate, FuncCall *inner_fn, Expr **vle_expr,
    char **list_name, bool *tail_mode, bool *double_tail)
{
    Node *path_arg = NULL;
    char *inner_name = NULL;

    if (parse_vle_count_list_arg(cpstate, inner_fn, vle_expr, list_name,
                                 tail_mode, double_tail))
    {
        if (is_nodes_list_name(*list_name))
        {
            return AGE_SEM_SOURCE_COMPACT_VLE_NODE_LIST;
        }

        return AGE_SEM_SOURCE_COMPACT_VLE_EDGE_LIST;
    }

    if (parse_path_list_func_arg(inner_fn, T_ColumnRef, &inner_name,
                                 &path_arg))
    {
        char *path_name = NULL;
        transform_entity *entity;

        if (parse_single_column_ref(path_arg, &path_name, NULL))
        {
            entity = find_variable(cpstate, path_name);
            if (entity != NULL && entity->type == ENT_PATH)
            {
                return AGE_SEM_SOURCE_FIXED_PATH;
            }
        }
    }

    return AGE_SEM_SOURCE_UNKNOWN;
}

static Node *transform_semantic_length_builtin(cypher_parsestate *cpstate,
                                               FuncCall *fn,
                                               const AgeBuiltinFuncMeta *meta)
{
    Node *path_arg = NULL;
    Expr *vle_expr = NULL;
    AgeSemanticSourceKind source_kind;

    if (!parse_func_any_arg(fn, meta->cypher_name, &path_arg))
    {
        return NULL;
    }

    source_kind = classify_path_length_source(cpstate, path_arg, &vle_expr);
    if (source_kind == AGE_SEM_SOURCE_COMPACT_VLE_PATH)
    {
        return make_vle_unary_agtype_expr(get_age_vle_path_length_oid(),
                                          (Node *)vle_expr);
    }
    if (source_kind == AGE_SEM_SOURCE_FIXED_PATH)
    {
        return try_transform_fixed_path_length(cpstate, fn);
    }

    return NULL;
}

static Node *transform_semantic_size_builtin(cypher_parsestate *cpstate,
                                             FuncCall *fn,
                                             const AgeBuiltinFuncMeta *meta)
{
    Node *inner_arg = NULL;
    FuncCall *inner_fn = NULL;
    char *list_name = NULL;
    Expr *vle_expr = NULL;
    AgeSemanticSourceKind source_kind;
    Oid func_oid;
    bool tail_mode = false;
    bool double_tail = false;

    if (!parse_func_arg(fn, meta->cypher_name, T_FuncCall, &inner_arg))
    {
        return NULL;
    }

    inner_fn = (FuncCall *)inner_arg;
    source_kind = classify_path_list_count_source(cpstate, inner_fn, &vle_expr,
                                                  &list_name, &tail_mode,
                                                  &double_tail);

    if (source_kind == AGE_SEM_SOURCE_FIXED_PATH)
    {
        return try_transform_fixed_path_list_cardinality(cpstate, fn);
    }

    if (source_kind != AGE_SEM_SOURCE_COMPACT_VLE_NODE_LIST &&
        source_kind != AGE_SEM_SOURCE_COMPACT_VLE_EDGE_LIST)
    {
        return NULL;
    }

    if (double_tail)
    {
        return make_vle_double_tail_count_expr((Node *)vle_expr, list_name,
                                               fn->location, false);
    }

    func_oid = get_vle_list_count_oid(list_name, tail_mode);
    return make_vle_unary_agtype_expr(func_oid, (Node *)vle_expr);
}

static Node *try_transform_semantic_agtype_builtin(cypher_parsestate *cpstate,
                                                   FuncCall *fn)
{
    const AgeBuiltinFuncMeta *meta;
    char *func_name = NULL;

    if (!parse_single_func_name(fn, &func_name))
    {
        return NULL;
    }

    meta = get_age_builtin_func_meta_by_name(func_name);
    if (meta == NULL)
    {
        return NULL;
    }

    switch (meta->fast_path)
    {
    case AGE_FUNC_FAST_LENGTH:
        return transform_semantic_length_builtin(cpstate, fn, meta);
    case AGE_FUNC_FAST_SIZE:
        return transform_semantic_size_builtin(cpstate, fn, meta);
    default:
        return NULL;
    }
}

/*
 * Code borrowed from PG's transformFuncCall and updated for AGE
 */
typedef Node *(*cypher_fast_func_transform)(cypher_parsestate *cpstate,
                                            FuncCall *fn);

static Node *try_transform_fast_func(cypher_parsestate *cpstate, FuncCall *fn)
{
    static const cypher_fast_func_transform fast_transforms[] = {
        try_transform_semantic_agtype_builtin,
        try_transform_entity_graphid_function,
        try_transform_fixed_path_head_last_consumer,
        try_transform_fixed_path_slice_head_last_consumer,
        try_transform_fixed_path_slice_transform_head_last_consumer,
        try_transform_fixed_path_indexed_edge_endpoint_id,
        try_transform_fixed_path_indexed_endpoint_vertex,
        try_transform_fixed_path_slice_endpoint_vertex,
        try_transform_edge_endpoint_vertex,
        try_transform_fixed_path_indexed_endpoint_vertex_function,
        try_transform_fixed_path_slice_endpoint_vertex_function,
        try_transform_edge_endpoint_vertex_function,
        try_transform_fixed_path_indexed_properties,
        try_transform_current_entity_properties,
        try_transform_vle_path_indexed_keys,
        try_transform_vle_path_boundary_keys,
        try_transform_vle_path_endpoint_keys,
        try_transform_entity_keys,
        try_transform_fixed_path_indexed_keys,
        try_transform_fixed_path_reverse_indexed_consumer,
        try_transform_fixed_path_indexed_label_type,
        try_transform_current_entity_label,
        try_transform_fixed_path_indexed_labels,
        try_transform_current_vertex_labels,
        try_transform_fixed_path_length,
        try_transform_fixed_path_list_cardinality,
        try_transform_fixed_path_slice_cardinality,
        try_transform_fixed_path_relationships,
        try_transform_fixed_path_nodes,
        try_transform_fixed_path_head_last,
        try_transform_fixed_path_tail,
        try_transform_fixed_path_slice_tail,
        try_transform_fixed_path_reverse,
        try_transform_fixed_path_slice_reverse,
        try_transform_fixed_path_indexed_id,
        try_transform_vle_path_endpoint_id_access,
        try_transform_vle_path_nested_transform_index_endpoint_id_access,
        try_transform_vle_path_boundary_endpoint_id_access,
        try_transform_vle_path_slice_boundary_endpoint_id_access,
        try_transform_vle_path_visible_boundary_endpoint_id,
        try_transform_vle_edge_variable_field,
        try_transform_vle_path_endpoint_field,
        try_transform_vle_path_nested_transform_index_endpoint_field,
        try_transform_vle_path_slice_boundary_endpoint_field,
        try_transform_vle_path_visible_boundary_endpoint_field,
        try_transform_vle_tail_last_endpoint_field,
        try_transform_vle_path_node_field,
        try_transform_vle_path_visible_boundary_field,
        try_transform_vle_path_boundary_field,
        try_transform_vle_path_slice_boundary_field,
        try_transform_vle_path_nested_transform_index_field,
        try_transform_vle_path_edge_label,
        try_transform_vle_path_edge_properties,
        try_transform_vle_path_endpoint_access,
        try_transform_vle_path_nested_transform_index_endpoint_access,
        try_transform_vle_path_boundary_endpoint_access,
        try_transform_vle_path_slice_boundary_endpoint_access,
        try_transform_vle_path_visible_boundary_endpoint_access,
        try_transform_vle_path_slice_boundary_id_function,
        try_transform_vle_path_visible_boundary_id_function,
        try_transform_vle_path_nested_transform_index_id_function,
        try_transform_vle_path_boundary_id_function,
        try_transform_vle_path_id_access,
        try_transform_vle_path_length,
        try_transform_vle_path_slice_size,
        try_transform_vle_path_size,
        try_transform_vle_path_slice_is_empty,
        try_transform_vle_path_is_empty,
        try_transform_vle_path_slice_head_last,
        try_transform_vle_path_head_last,
        try_transform_vle_path_nested_transform_head_last,
        try_transform_vle_path_list_tail_reverse,
        try_transform_vle_path_relationships,
        try_transform_vle_path_nodes,
    };
    int i;
    Node *retval = NULL;

    for (i = 0; i < lengthof(fast_transforms); i++)
    {
        retval = fast_transforms[i](cpstate, fn);
        if (retval != NULL)
        {
            return retval;
        }
    }

    return NULL;
}

static Node *transform_FuncCall(cypher_parsestate *cpstate, FuncCall *fn)
{
    ParseState *pstate = &cpstate->pstate;
    Node *last_srf = pstate->p_last_srf;
    List *targs = NIL;
    List *fname = NIL;
    ListCell *arg;
    Node *retval = NULL;
    char *name = NULL;

    retval = try_transform_fast_func(cpstate, fn);
    if (retval != NULL)
    {
        return retval;
    }

    /* Transform the list of arguments ... */
    foreach(arg, fn->args)
    {
        Node *farg = NULL;

        farg = (Node *)lfirst(arg);
        targs = lappend(targs, transform_cypher_expr_recurse(cpstate, farg));
    }

    /* within group should not happen */
    Assert(!fn->agg_within_group);

    /* If it is a qualified function call, let it through. */
    if (!parse_single_func_name(fn, &name))
    {
        fname = fn->funcname;
    }
    /*
     * Else We need to check if the function call is for
     * age or for some external extension.
     */
    else
    {
        int name_len = strlen(name);
        char *ag_name = construct_age_function_name(name, name_len);

        if (function_exists(ag_name, "age"))
        {
            /* qualify the name with our schema name */
            fname = list_make2(makeString("ag_catalog"), makeString(ag_name));

            /*
             * Currently 3 functions need the graph name passed in as the first
             * argument - in addition to the other arguments: startNode, endNode,
             * and vle. So, check for those 3 functions here and that the arg list
             * is not empty. Then prepend the graph name if necessary.
             */
            if ((targs != NIL) &&
                function_needs_graph_name_argument(name, name_len))
            {
                char *graph_name = cpstate->graph_name;
                Datum d = string_to_agtype(graph_name);
                Const *c = makeConst(AGTYPEOID, -1, InvalidOid, -1, d, false,
                                    false);

                targs = lcons(c, targs);
            }
        }
        /*
         * If it's not in age, check if it's a potential call to some function
         * in another installed extension.
         */
        else
        {
            Form_pg_proc procform = get_procform(fn, false);
            const char *extension;

            pfree(ag_name);

            if (procform == NULL)
            {
                ereport(ERROR,
                        (errcode(ERRCODE_UNDEFINED_FUNCTION),
                        errmsg("function %s does not exist", name),
                        errhint("If the function is from an external extension, "
                                "make sure the extension is installed and the "
                                "function is in the search path.")));
            }

            extension = get_mapped_extension(procform->oid);

            /*
             * If the function is from another extension, transform
             * it if possible and return the function expr.
             */
            if (is_extension_external(extension))
            {
                retval = transform_external_ext_FuncCall(cpstate, fn, targs,
                                                         procform, extension);
                return retval;
            }
            /*
             * Else we have a function that is in the search_path, and not
             * qualified, but is not in an extension. Pass it through.
             */
            else
            {
                pfree(procform);
                fname = fn->funcname;
            }
        }
    }

    /* ... and hand off to ParseFuncOrColumn */
    retval = ParseFuncOrColumn(pstate, fname, targs, last_srf, fn, false,
                               fn->location);

    if (retval != NULL && retval->type == T_Aggref)
    {
        cypher_rewrite_vle_tail_last_count_agg(castNode(Aggref, retval));
        try_rewrite_collect_property_access(castNode(Aggref, retval));
    }

    /* flag that an aggregate was found during a transform */
    if (retval != NULL && retval->type == T_Aggref)
    {
        cpstate->exprHasAgg = true;
    }

    return retval;
}

/*
 * Code borrowed from PG's transformCoalesceExpr and updated for AGE
 */
static Node *transform_CoalesceExpr(cypher_parsestate *cpstate, CoalesceExpr
                                    *cexpr)
{
    ParseState *pstate = &cpstate->pstate;
    CoalesceExpr *newcexpr = makeNode(CoalesceExpr);
    Node *last_srf = pstate->p_last_srf;
    List *newargs = NIL;
    List *newcoercedargs = NIL;
    ListCell *args;

    foreach(args, cexpr->args)
    {
        Node *e = (Node *)lfirst(args);
        Node *newe;

        newe = transform_cypher_expr_recurse(cpstate, e);
        newargs = lappend(newargs, newe);
    }

    newcexpr->coalescetype = select_common_type(pstate, newargs, "COALESCE",
                                                NULL);
    /* coalescecollid will be set by parse_collate.c */

    /* Convert arguments if necessary */
    foreach(args, newargs)
    {
        Node *e = (Node *)lfirst(args);
        Node *newe;

        newe = coerce_to_common_type(pstate, e, newcexpr->coalescetype,
                                     "COALESCE");
        newcoercedargs = lappend(newcoercedargs, newe);
    }

    /* if any subexpression contained a SRF, complain */
    if (pstate->p_last_srf != last_srf)
    {
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 /* translator: %s is name of a SQL construct, eg GROUP BY */
                 errmsg("set-returning functions are not allowed in %s",
                        "COALESCE"),
                 parser_errposition(pstate, exprLocation(pstate->p_last_srf))));
    }

    newcexpr->args = newcoercedargs;
    newcexpr->location = cexpr->location;
    return (Node *) newcexpr;
}

/*
 * Code borrowed from PG's transformCaseExpr and updated for AGE
 */
static Node *transform_CaseExpr(cypher_parsestate *cpstate, CaseExpr
                                *cexpr)
{
    ParseState *pstate = &cpstate->pstate;
    CaseExpr   *newcexpr = makeNode(CaseExpr);
    Node       *last_srf = pstate->p_last_srf;
    Node       *arg;
    CaseTestExpr *placeholder;
    List       *newargs;
    List       *resultexprs;
    ListCell   *l;
    Node       *defresult;
    Oid         ptype;

    /* transform the test expression, if any */
    arg = transform_cypher_expr_recurse(cpstate, (Node *) cexpr->arg);

    /* generate placeholder for test expression */
    if (arg)
    {
        if (exprType(arg) == UNKNOWNOID)
            arg = coerce_to_common_type(pstate, arg, TEXTOID, "CASE");

        assign_expr_collations(pstate, arg);

        placeholder = makeNode(CaseTestExpr);
        placeholder->typeId = exprType(arg);
        placeholder->typeMod = exprTypmod(arg);
        placeholder->collation = exprCollation(arg);
    }
    else
    {
        placeholder = NULL;
    }

    newcexpr->arg = (Expr *) arg;

    /* transform the list of arguments */
    newargs = NIL;
    resultexprs = NIL;
    foreach(l, cexpr->args)
    {
        CaseWhen   *w = lfirst_node(CaseWhen, l);
        CaseWhen   *neww = makeNode(CaseWhen);
        Node       *warg;

        warg = (Node *) w->expr;
        if (placeholder)
        {
            if(is_ag_node(warg, cypher_comparison_aexpr) ||
               is_ag_node(warg, cypher_comparison_boolexpr) )
            {
                List *funcname = list_make1(makeString("ag_catalog"));
                funcname = lappend(funcname, makeString("bool_to_agtype"));

                warg = (Node *) makeFuncCall(funcname, list_make1(warg),
                                             COERCE_EXPLICIT_CAST,
                                             cexpr->location);
            }

            /* shorthand form was specified, so expand... */
            warg = (Node *) makeSimpleA_Expr(AEXPR_OP, "=",
                                             (Node *) placeholder,
                                             warg,
                                             w->location);
        }
        neww->expr = (Expr *) transform_cypher_expr_recurse(cpstate, warg);

        neww->expr = (Expr *) coerce_to_boolean(pstate,
                                                (Node *) neww->expr,
                                                "CASE/WHEN");

        warg = (Node *) w->result;

        if(is_ag_node(warg, cypher_comparison_aexpr) ||
           is_ag_node(warg, cypher_comparison_boolexpr) )
        {
            List *funcname = list_make1(makeString("ag_catalog"));
            funcname = lappend(funcname, makeString("bool_to_agtype"));

            warg = (Node *) makeFuncCall(funcname, list_make1(warg),
                                         COERCE_EXPLICIT_CAST,
                                         cexpr->location);
        }

        neww->result = (Expr *) transform_cypher_expr_recurse(cpstate, warg);
        neww->location = w->location;

        newargs = lappend(newargs, neww);
        resultexprs = lappend(resultexprs, neww->result);
    }

    newcexpr->args = newargs;

    /* transform the default clause */
    defresult = (Node *) cexpr->defresult;
    if (defresult == NULL)
    {
        A_Const    *n = makeNode(A_Const);

        n->isnull = true;
        n->location = -1;
        defresult = (Node *) n;
    }
    newcexpr->defresult = (Expr *) transform_cypher_expr_recurse(cpstate, defresult);

    resultexprs = lcons(newcexpr->defresult, resultexprs);

    /*
     * we pass a NULL context to select_common_type because the common types can
     * only be AGTYPEOID or BOOLOID. If it returns invalidoid, we know there is a
     * boolean involved.
     */
    ptype = select_common_type(pstate, resultexprs, NULL, NULL);

    /* InvalidOid shows that there is a boolean in the result expr. */
    if (ptype == InvalidOid)
    {
        /* we manually set the type to boolean here to handle the bool casting. */
        ptype = BOOLOID;
    }

    Assert(OidIsValid(ptype));
    newcexpr->casetype = ptype;
    /* casecollid will be set by parse_collate.c */

    /* Convert default result clause, if necessary */
    newcexpr->defresult = (Expr *)
        coerce_to_common_type(pstate,
                              (Node *) newcexpr->defresult,
                              ptype,
                              "CASE/ELSE");

    /* Convert when-clause results, if necessary */
    foreach(l, newcexpr->args)
    {
        CaseWhen   *w = (CaseWhen *) lfirst(l);

        w->result = (Expr *)
            coerce_to_common_type(pstate,
                                  (Node *) w->result,
                                  ptype,
                                  "CASE/WHEN");
    }

    /* if any subexpression contained a SRF, complain */
    if (pstate->p_last_srf != last_srf)
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
        /* translator: %s is name of a SQL construct, eg GROUP BY */
                 errmsg("set-returning functions are not allowed in %s",
                        "CASE"),
                 errhint("You might be able to move the set-returning function into a LATERAL FROM item."),
                 parser_errposition(pstate,
                                    exprLocation(pstate->p_last_srf))));

    newcexpr->location = cexpr->location;

    return (Node *) newcexpr;
}

/* from PG's transformSubLink but reduced and hooked into our parser */
static Node *transform_SubLink(cypher_parsestate *cpstate, SubLink *sublink)
{
    Node *result = (Node*)sublink;
    Query *qtree;
    ParseState *pstate = (ParseState*)cpstate;
    const char *err = NULL;
    /*
     * Check to see if the sublink is in an invalid place within the query. We
     * allow sublinks everywhere in SELECT/INSERT/UPDATE/DELETE, but generally
     * not in utility statements.
     */
    switch (pstate->p_expr_kind)
    {
        case EXPR_KIND_NONE:
            Assert(false);          /* can't happen */
            break;
        case EXPR_KIND_OTHER:
            /* Accept sublink here; caller must throw error if wanted */

            break;
        case EXPR_KIND_JOIN_ON:
        case EXPR_KIND_JOIN_USING:
        case EXPR_KIND_FROM_FUNCTION:
        case EXPR_KIND_SELECT_TARGET:
        case EXPR_KIND_FROM_SUBSELECT:
        case EXPR_KIND_WHERE:
        case EXPR_KIND_INSERT_TARGET:
            /* okay */
            break;
        default:
            ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                            errmsg_internal("unsupported SubLink"),
                            parser_errposition(pstate, sublink->location)));
    }
    if (err)
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg_internal("%s", err),
                 parser_errposition(pstate, sublink->location)));

    pstate->p_hasSubLinks = true;
    /*
     * OK, let's transform the sub-SELECT.
     */
    qtree = cypher_parse_sub_analyze(sublink->subselect, cpstate, NULL, false,
                                     true);

    /*
     * Check that we got a SELECT.  Anything else should be impossible given
     * restrictions of the grammar, but check anyway.
     */
    if (!IsA(qtree, Query) || qtree->commandType != CMD_SELECT)
        elog(ERROR, "unexpected non-SELECT command in SubLink");

    sublink->subselect = (Node *)qtree;

    if (sublink->subLinkType == EXISTS_SUBLINK)
    {
        /*
         * EXISTS needs no test expression or combining operator. These fields
         * should be null already, but make sure.
         */
        sublink->testexpr = NULL;
        sublink->operName = NIL;
    }
    else if (sublink->subLinkType == EXPR_SUBLINK ||
             sublink->subLinkType == ARRAY_SUBLINK)
    {
        /*
         * Make sure the subselect delivers a single column (ignoring resjunk
         * targets).
         */
        if (count_nonjunk_tlist_entries(qtree->targetList) != 1)
        {
            ereport(ERROR,
                    (errcode(ERRCODE_SYNTAX_ERROR),
                     errmsg("subquery must return only one column"),
                     parser_errposition(pstate, sublink->location)));
        }

        /*
         * EXPR and ARRAY need no test expression or combining operator. These
         * fields should be null already, but make sure.
         */
        sublink->testexpr = NULL;
        sublink->operName = NIL;
    }
    else if (sublink->subLinkType == MULTIEXPR_SUBLINK)
    {
        /* Same as EXPR case, except no restriction on number of columns */
        sublink->testexpr = NULL;
        sublink->operName = NIL;
    }
    else
        elog(ERROR, "unsupported SubLink type");

    return result;
}
