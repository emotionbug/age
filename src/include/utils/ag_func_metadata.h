/*
 * AGE built-in function semantic metadata.
 */

#ifndef AG_FUNC_METADATA_H
#define AG_FUNC_METADATA_H

typedef enum AgeFuncSemanticArgKind
{
    AGE_FUNC_ARG_UNKNOWN = 0,
    AGE_FUNC_ARG_VERTEX_LIKE = 1 << 0,
    AGE_FUNC_ARG_EDGE_LIKE = 1 << 1,
    AGE_FUNC_ARG_PATH_LIKE = 1 << 2,
    AGE_FUNC_ARG_LIST_LIKE = 1 << 3,
    AGE_FUNC_ARG_ANY = 1 << 4
} AgeFuncSemanticArgKind;

typedef enum AgeFuncSemanticResultKind
{
    AGE_FUNC_RET_UNKNOWN,
    AGE_FUNC_RET_VERTEX_LIKE,
    AGE_FUNC_RET_SCALAR,
    AGE_FUNC_RET_LIST,
    AGE_FUNC_RET_PATH_LIKE,
    AGE_FUNC_RET_EDGE_LIKE,
    AGE_FUNC_RET_ANY
} AgeFuncSemanticResultKind;

typedef enum AgeFuncFastPathKind
{
    AGE_FUNC_FAST_NONE,
    AGE_FUNC_FAST_ID,
    AGE_FUNC_FAST_ENDPOINT,
    AGE_FUNC_FAST_LENGTH,
    AGE_FUNC_FAST_SIZE,
    AGE_FUNC_FAST_KEYS,
    AGE_FUNC_FAST_VLE_COUNT,
    AGE_FUNC_FAST_VLE_MATERIALIZE
} AgeFuncFastPathKind;

typedef enum AgeFuncSqlArgKind
{
    AGE_FUNC_SQL_ARG_AGTYPE,
    AGE_FUNC_SQL_ARG_ANY
} AgeFuncSqlArgKind;

typedef struct AgeBuiltinFuncMeta
{
    const char *sql_name;
    const char *cypher_name;
    int nargs;
    const AgeFuncSqlArgKind *arg_kinds;
    AgeFuncSemanticResultKind result_kind;
    AgeFuncSemanticArgKind arg_kind;
    AgeFuncFastPathKind fast_path;
} AgeBuiltinFuncMeta;

const AgeBuiltinFuncMeta *get_age_builtin_func_meta_by_name(
    const char *func_name);
const AgeBuiltinFuncMeta *get_age_builtin_func_meta_by_oid(Oid func_oid);
Oid get_age_builtin_func_oid(const AgeBuiltinFuncMeta *meta);
Oid get_age_builtin_func_oid_by_name(const char *func_name);

#define AGE_BUILTIN_FUNC_CATALOG(ENTRY) \
    ENTRY(age_id, age_id, id, 1, AGE_FUNC_RET_SCALAR, \
          AGE_FUNC_ARG_VERTEX_LIKE | AGE_FUNC_ARG_EDGE_LIKE, \
          AGE_FUNC_FAST_ID, AGE_FUNC_SQL_ARG_AGTYPE) \
    ENTRY(age_startnode, age_startnode, startNode, 2, AGE_FUNC_RET_VERTEX_LIKE, \
          AGE_FUNC_ARG_EDGE_LIKE, AGE_FUNC_FAST_ENDPOINT, \
          AGE_FUNC_SQL_ARG_AGTYPE, AGE_FUNC_SQL_ARG_AGTYPE) \
    ENTRY(age_endnode, age_endnode, endNode, 2, AGE_FUNC_RET_VERTEX_LIKE, \
          AGE_FUNC_ARG_EDGE_LIKE, AGE_FUNC_FAST_ENDPOINT, \
          AGE_FUNC_SQL_ARG_AGTYPE, AGE_FUNC_SQL_ARG_AGTYPE) \
    ENTRY(age_length, age_length, length, 1, AGE_FUNC_RET_SCALAR, \
          AGE_FUNC_ARG_PATH_LIKE, AGE_FUNC_FAST_LENGTH, \
          AGE_FUNC_SQL_ARG_AGTYPE) \
    ENTRY(age_size, age_size, size, 1, AGE_FUNC_RET_SCALAR, \
          AGE_FUNC_ARG_LIST_LIKE | AGE_FUNC_ARG_ANY, AGE_FUNC_FAST_SIZE, \
          AGE_FUNC_SQL_ARG_ANY) \
    ENTRY(age_keys, age_keys, keys, 1, AGE_FUNC_RET_LIST, \
          AGE_FUNC_ARG_VERTEX_LIKE | AGE_FUNC_ARG_EDGE_LIKE | \
          AGE_FUNC_ARG_ANY, AGE_FUNC_FAST_KEYS, AGE_FUNC_SQL_ARG_AGTYPE) \
    ENTRY(age_vle_path_length, age_vle_path_length, age_vle_path_length, 1, \
          AGE_FUNC_RET_SCALAR, AGE_FUNC_ARG_PATH_LIKE, \
          AGE_FUNC_FAST_VLE_COUNT, AGE_FUNC_SQL_ARG_AGTYPE) \
    ENTRY(age_vle_path_node_count, age_vle_path_node_count, \
          age_vle_path_node_count, 1, AGE_FUNC_RET_SCALAR, \
          AGE_FUNC_ARG_PATH_LIKE, AGE_FUNC_FAST_VLE_COUNT, \
          AGE_FUNC_SQL_ARG_AGTYPE) \
    ENTRY(age_vle_edge_tail_count, age_vle_edge_tail_count, \
          age_vle_edge_tail_count, 1, AGE_FUNC_RET_SCALAR, \
          AGE_FUNC_ARG_LIST_LIKE, AGE_FUNC_FAST_VLE_COUNT, \
          AGE_FUNC_SQL_ARG_AGTYPE) \
    ENTRY(age_materialize_vle_edges, age_materialize_vle_edges, \
          age_materialize_vle_edges, 1, AGE_FUNC_RET_LIST, \
          AGE_FUNC_ARG_PATH_LIKE, AGE_FUNC_FAST_VLE_MATERIALIZE, \
          AGE_FUNC_SQL_ARG_AGTYPE) \
    ENTRY(age_materialize_vle_nodes, age_materialize_vle_nodes, \
          age_materialize_vle_nodes, 1, AGE_FUNC_RET_LIST, \
          AGE_FUNC_ARG_PATH_LIKE, AGE_FUNC_FAST_VLE_MATERIALIZE, \
          AGE_FUNC_SQL_ARG_AGTYPE) \
    ENTRY(age_materialize_vle_node_at, age_materialize_vle_node_at, \
          age_materialize_vle_node_at, 2, AGE_FUNC_RET_VERTEX_LIKE, \
          AGE_FUNC_ARG_PATH_LIKE, AGE_FUNC_FAST_VLE_MATERIALIZE, \
          AGE_FUNC_SQL_ARG_AGTYPE, AGE_FUNC_SQL_ARG_AGTYPE) \
    ENTRY(age_materialize_vle_node_tail_last, \
          age_materialize_vle_node_tail_last, \
          age_materialize_vle_node_tail_last, 1, AGE_FUNC_RET_VERTEX_LIKE, \
          AGE_FUNC_ARG_LIST_LIKE, AGE_FUNC_FAST_VLE_MATERIALIZE, \
          AGE_FUNC_SQL_ARG_AGTYPE) \
    ENTRY(age_materialize_vle_edge_at, age_materialize_vle_edge_at, \
          age_materialize_vle_edge_at, 2, AGE_FUNC_RET_EDGE_LIKE, \
          AGE_FUNC_ARG_PATH_LIKE, AGE_FUNC_FAST_VLE_MATERIALIZE, \
          AGE_FUNC_SQL_ARG_AGTYPE, AGE_FUNC_SQL_ARG_AGTYPE) \
    ENTRY(age_materialize_vle_edge_reversed_at, \
          age_materialize_vle_edge_reversed_at, \
          age_materialize_vle_edge_reversed_at, 2, AGE_FUNC_RET_EDGE_LIKE, \
          AGE_FUNC_ARG_PATH_LIKE, AGE_FUNC_FAST_VLE_MATERIALIZE, \
          AGE_FUNC_SQL_ARG_AGTYPE, AGE_FUNC_SQL_ARG_AGTYPE) \
    ENTRY(age_materialize_vle_edge_tail_last, \
          age_materialize_vle_edge_tail_last, \
          age_materialize_vle_edge_tail_last, 1, AGE_FUNC_RET_EDGE_LIKE, \
          AGE_FUNC_ARG_LIST_LIKE, AGE_FUNC_FAST_VLE_MATERIALIZE, \
          AGE_FUNC_SQL_ARG_AGTYPE) \
    ENTRY(age_materialize_vle_list_slice, age_materialize_vle_list_slice, \
          age_materialize_vle_list_slice, 4, AGE_FUNC_RET_LIST, \
          AGE_FUNC_ARG_LIST_LIKE, AGE_FUNC_FAST_VLE_MATERIALIZE, \
          AGE_FUNC_SQL_ARG_AGTYPE, AGE_FUNC_SQL_ARG_AGTYPE, \
          AGE_FUNC_SQL_ARG_AGTYPE, AGE_FUNC_SQL_ARG_AGTYPE) \
    ENTRY(age_materialize_vle_slice_boundary, \
          age_materialize_vle_slice_boundary, \
          age_materialize_vle_slice_boundary, 4, AGE_FUNC_RET_ANY, \
          AGE_FUNC_ARG_LIST_LIKE, AGE_FUNC_FAST_VLE_MATERIALIZE, \
          AGE_FUNC_SQL_ARG_AGTYPE, AGE_FUNC_SQL_ARG_AGTYPE, \
          AGE_FUNC_SQL_ARG_AGTYPE, AGE_FUNC_SQL_ARG_AGTYPE) \
    ENTRY(age_materialize_vle_nodes_slice, age_materialize_vle_nodes_slice, \
          age_materialize_vle_nodes_slice, 3, AGE_FUNC_RET_LIST, \
          AGE_FUNC_ARG_LIST_LIKE, AGE_FUNC_FAST_VLE_MATERIALIZE, \
          AGE_FUNC_SQL_ARG_AGTYPE, AGE_FUNC_SQL_ARG_AGTYPE, \
          AGE_FUNC_SQL_ARG_AGTYPE) \
    ENTRY(age_materialize_vle_nodes_tail, age_materialize_vle_nodes_tail, \
          age_materialize_vle_nodes_tail, 1, AGE_FUNC_RET_LIST, \
          AGE_FUNC_ARG_LIST_LIKE, AGE_FUNC_FAST_VLE_MATERIALIZE, \
          AGE_FUNC_SQL_ARG_AGTYPE) \
    ENTRY(age_materialize_vle_nodes_reversed, \
          age_materialize_vle_nodes_reversed, \
          age_materialize_vle_nodes_reversed, 1, AGE_FUNC_RET_LIST, \
          AGE_FUNC_ARG_LIST_LIKE, AGE_FUNC_FAST_VLE_MATERIALIZE, \
          AGE_FUNC_SQL_ARG_AGTYPE) \
    ENTRY(age_materialize_vle_edges_tail, age_materialize_vle_edges_tail, \
          age_materialize_vle_edges_tail, 1, AGE_FUNC_RET_LIST, \
          AGE_FUNC_ARG_LIST_LIKE, AGE_FUNC_FAST_VLE_MATERIALIZE, \
          AGE_FUNC_SQL_ARG_AGTYPE) \
    ENTRY(age_materialize_vle_edges_reversed, \
          age_materialize_vle_edges_reversed, \
          age_materialize_vle_edges_reversed, 1, AGE_FUNC_RET_LIST, \
          AGE_FUNC_ARG_LIST_LIKE, AGE_FUNC_FAST_VLE_MATERIALIZE, \
          AGE_FUNC_SQL_ARG_AGTYPE)

#endif
