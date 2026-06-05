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

#include <ctype.h>

#include "access/xact.h"
#include "catalog/namespace.h"
#include "catalog/pg_class_d.h"
#include "catalog/pg_type_d.h"
#include "commands/defrem.h"
#include "commands/sequence.h"
#include "commands/tablecmds.h"
#include "catalog/pg_trigger.h"
#include "executor/spi.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "parser/parser.h"
#include "parser/parse_func.h"
#include "tcop/utility.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/inval.h"
#include "utils/json.h"
#include "utils/lsyscache.h"

#include "catalog/ag_graph.h"
#include "catalog/ag_label.h"
#include "commands/label_commands.h"
#include "utils/ag_cache.h"
#include "utils/name_validation.h"

/*
 * Relation name doesn't have to be label name but the same name is used so
 * that users can find the backed relation for a label only by its name.
 */
#define gen_label_relation_name(label_name) (label_name)

static void create_table_for_label(char *graph_name, char *label_name,
                                   char *schema_name, char *rel_name,
                                   char *seq_name, char label_type,
                                   List *parents);

/* common */
static List *create_edge_table_elements(char *graph_name, char *label_name,
                                        char *schema_name, char *rel_name,
                                        char *seq_name);
static List *create_vertex_table_elements(char *graph_name, char *label_name,
                                          char *schema_name, char *rel_name,
                                          char *seq_name);
static void create_sequence_for_label(RangeVar *seq_range_var);
static Constraint *build_pk_constraint(void);
static Constraint *build_id_default(char *graph_name, char *label_name,
                                    char *schema_name, char *seq_name);
static FuncCall *build_id_default_func_expr(char *graph_name, char *label_name,
                                            char *schema_name, char *seq_name);
static Constraint *build_not_null_constraint(void);
static Constraint *build_properties_default(void);
static void alter_sequence_owned_by_for_label(RangeVar *seq_range_var,
                                              char *rel_name);
static int32 get_new_label_id(Oid graph_oid, Oid nsp_id);
static void change_label_id_default(char *graph_name, char *label_name,
                                    char *schema_name, char *seq_name,
                                    Oid relid);

/* drop */
static void remove_relation(List *qname);
static void range_var_callback_for_remove_relation(const RangeVar *rel,
                                                   Oid rel_oid,
                                                   Oid odl_rel_oid,
                                                   void *arg);
static void create_index_on_column(char *schema_name,
                                   char *rel_name,
                                   char *colname,
                                   bool unique);
static char *create_index_on_property(char *schema_name, char *rel_name,
                                      char *property_name,
                                      char *property_type,
                                      char *index_name);
static char *create_index_on_adjacency(char *schema_name, char *rel_name,
                                       bool outgoing, char *index_name);
static IndexElem *make_graphid_index_elem(char *colname, char *opclass_name);
static Node *build_property_index_expr(char *property_name);
static Node *build_typed_property_index_expr(char *property_name,
                                             char *property_type);
static char *property_index_helper_for_type(char *property_type);
static void build_property_terminal_index_args(char *property_name,
                                               Node **container, Node **key);
static Node *build_property_key_const(char *property_name);
static List *build_property_path_index_args(char *property_name);
static char *make_property_index_name(char *rel_name, char *property_name,
                                      char *property_type);
static char *make_adjacency_index_name(char *rel_name, bool outgoing);
static label_cache_data *lookup_label_for_index_helper(char *graph_name,
                                                       char *label_name,
                                                       char required_kind);
static void record_graph_index_metadata(char *graph_name,
                                        label_cache_data *label_cache,
                                        char *index_name,
                                        char *index_kind,
                                        char *direction,
                                        char *property_name,
                                        char *provider,
                                        char *property_type);
static void delete_graph_index_metadata(char *graph_name, char *index_name);
static Oid create_label_with_graph_cache(char *graph_name, char *label_name,
                                         char label_type, List *parents,
                                         graph_cache_data *cache_data);

PG_FUNCTION_INFO_V1(age_is_valid_label_name);

Datum age_is_valid_label_name(PG_FUNCTION_ARGS)
{
    agtype *agt_arg = NULL;
    agtype_value agtv_value;
    char *label_name = NULL;
    bool is_valid = false;
    bool value_needs_free = false;

    if (PG_ARGISNULL(0))
    {
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                errmsg("label name must not be NULL")));
    }

    agt_arg = AG_GET_ARG_AGTYPE_P(0);

    if (!AGT_ROOT_IS_SCALAR(agt_arg))
    {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("is_valid_label_name() only supports scalar arguments")));
    }

    (void)get_ith_agtype_value_from_container_no_copy(&agt_arg->root, 0,
                                                      &agtv_value,
                                                      &value_needs_free);

    if (agtv_value.type != AGTV_STRING)
    {
        if (value_needs_free)
            pfree_agtype_value_content(&agtv_value);
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("is_valid_label_name() only supports string arguments")));
    }

    label_name = pnstrdup(agtv_value.val.string.val,
                          agtv_value.val.string.len);
    if (value_needs_free)
        pfree_agtype_value_content(&agtv_value);

    is_valid = is_valid_label_name(label_name, 0);
    pfree_if_not_null(label_name);

    if (is_valid)
    {
        PG_RETURN_BOOL(true);
    }

    PG_RETURN_BOOL(false);
}

PG_FUNCTION_INFO_V1(create_vlabel);

/*
 * This is a callback function
 * This function will be called when the user will call SELECT create_vlabel.
 * The function takes two parameters
 * 1. Graph name
 * 2. Label Name
 * Function will create a vertex label
 * Function returns an error if graph or label names or not provided
*/

Datum create_vlabel(PG_FUNCTION_ARGS)
{
    char *graph_name;
    Oid graph_oid;
    graph_cache_data *graph_cache;
    List *parent;
    RangeVar *rv;
    char *label_name;

    /* checking if user has not provided the graph name */
    if (PG_ARGISNULL(0))
    {
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                errmsg("graph name must not be NULL")));
    }

    /* checking if user has not provided the label name */
    if (PG_ARGISNULL(1))
    {
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                errmsg("label name must not be NULL")));
    }

    graph_name = PG_GETARG_CSTRING(0);
    label_name = PG_GETARG_CSTRING(1);

    /* validate the graph and label names */
    if (is_valid_graph_name(graph_name) == 0)
    {
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                errmsg("graph name is invalid")));
    }

    if (is_valid_label_name(label_name, 0) == 0)
    {
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                errmsg("label name is invalid")));
    }

    /* Check if graph does not exist */
    graph_cache = search_graph_name_cache_cached(graph_name);
    if (graph_cache == NULL)
    {
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_SCHEMA),
                        errmsg("graph \"%s\" does not exist.", graph_name)));
    }

    graph_oid = graph_cache->oid;

    /* Check if label with the input name already exists */
    if (label_exists(label_name, graph_oid))
    {
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_SCHEMA),
                        errmsg("label \"%s\" already exists", label_name)));
    }

    /* Create the default label tables */
    rv = makeRangeVar(graph_name, pstrdup(AG_DEFAULT_LABEL_VERTEX), -1);

    parent = list_make1(rv);

    create_label_with_graph_cache(graph_name, label_name, LABEL_TYPE_VERTEX,
                                  parent, graph_cache);

    ereport(NOTICE,
            (errmsg("VLabel \"%s\" has been created", label_name)));

    PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(create_elabel);

/*
 * This is a callback function
 * This function will be called when the user will call SELECT create_elabel.
 * The function takes two parameters
 * 1. Graph name
 * 2. Label Name
 * Function will create an edge label
 * Function returns an error if graph or label names or not provided
*/

Datum create_elabel(PG_FUNCTION_ARGS)
{
    char *graph_name;
    Oid graph_oid;
    graph_cache_data *graph_cache;
    List *parent;
    RangeVar *rv;
    char *label_name;

    /* checking if user has not provided the graph name */
    if (PG_ARGISNULL(0))
    {
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                errmsg("graph name must not be NULL")));
    }

    /* checking if user has not provided the label name */
    if (PG_ARGISNULL(1))
    {
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                errmsg("label name must not be NULL")));
    }

    graph_name = PG_GETARG_CSTRING(0);
    label_name = PG_GETARG_CSTRING(1);

    /* validate the graph and label names */
    if (is_valid_graph_name(graph_name) == 0)
    {
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                errmsg("graph name is invalid")));
    }

    if (is_valid_label_name(label_name, 0) == 0)
    {
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                errmsg("label name is invalid")));
    }

    /* Check if graph does not exist */
    graph_cache = search_graph_name_cache_cached(graph_name);
    if (graph_cache == NULL)
    {
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_SCHEMA),
                 errmsg("graph \"%s\" does not exist.", graph_name)));
    }

    graph_oid = graph_cache->oid;

    /* Check if label with the input name already exists */
    if (label_exists(label_name, graph_oid))
    {
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_SCHEMA),
                        errmsg("label \"%s\" already exists", label_name)));
    }

    /* Create the default label tables */
    rv = makeRangeVar(graph_name, pstrdup(AG_DEFAULT_LABEL_EDGE), -1);

    parent = list_make1(rv);
    create_label_with_graph_cache(graph_name, label_name, LABEL_TYPE_EDGE,
                                  parent, graph_cache);

    ereport(NOTICE,
            (errmsg("ELabel \"%s\" has been created", label_name)));

    PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(create_property_index);
PG_FUNCTION_INFO_V1(create_property_source_index);
PG_FUNCTION_INFO_V1(create_property_source_index_named);
PG_FUNCTION_INFO_V1(create_adjacency_index);
PG_FUNCTION_INFO_V1(create_adjacency_index_named);
PG_FUNCTION_INFO_V1(create_adjacency_indexes);
PG_FUNCTION_INFO_V1(create_adjacency_indexes_named);
PG_FUNCTION_INFO_V1(drop_graph_index);

Datum create_property_index(PG_FUNCTION_ARGS)
{
    char *graph_name;
    char *label_name;
    char *property_name;
    char *property_type = NULL;
    graph_cache_data *graph_cache;
    label_cache_data *label_cache;
    char *index_name;

    if (PG_ARGISNULL(0))
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("graph name must not be NULL")));
    if (PG_ARGISNULL(1))
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("label name must not be NULL")));
    if (PG_ARGISNULL(2))
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("property name must not be NULL")));
    if (PG_NARGS() > 3 && PG_ARGISNULL(3))
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("property type must not be NULL")));

    graph_name = PG_GETARG_CSTRING(0);
    label_name = PG_GETARG_CSTRING(1);
    property_name = PG_GETARG_CSTRING(2);
    if (PG_NARGS() > 3)
        property_type = PG_GETARG_CSTRING(3);

    if (is_valid_graph_name(graph_name) == 0)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("graph name is invalid")));
    if (is_valid_label_name(label_name, 0) == 0)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("label name is invalid")));
    if (property_name[0] == '\0')
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("property name must not be empty")));
    if (property_type != NULL && property_type[0] == '\0')
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("property type must not be empty")));

    graph_cache = search_graph_name_cache_cached(graph_name);
    if (graph_cache == NULL)
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_SCHEMA),
                 errmsg("graph \"%s\" does not exist.", graph_name)));

    label_cache = search_label_name_graph_cache_cached(label_name,
                                                       graph_cache->oid);
    if (label_cache == NULL)
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_SCHEMA),
                 errmsg("label \"%s\" does not exist", label_name)));

    index_name = create_index_on_property(
        graph_name, (char *)get_label_cache_relation_name(label_cache),
        property_name, property_type, NULL);
    record_graph_index_metadata(graph_name, label_cache, index_name,
                                "PROPERTY", NULL, property_name, "btree",
                                property_type);

    PG_RETURN_VOID();
}

Datum create_property_source_index(PG_FUNCTION_ARGS)
{
    return create_property_index(fcinfo);
}

Datum create_property_source_index_named(PG_FUNCTION_ARGS)
{
    char *graph_name;
    char *label_name;
    char *property_name;
    char *index_name;
    char *property_type = NULL;
    label_cache_data *label_cache;
    char *created_index_name;

    if (PG_ARGISNULL(0) || PG_ARGISNULL(1) || PG_ARGISNULL(2) ||
        PG_ARGISNULL(3))
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("graph, label, property, and index name are required")));

    graph_name = PG_GETARG_CSTRING(0);
    label_name = PG_GETARG_CSTRING(1);
    property_name = PG_GETARG_CSTRING(2);
    index_name = PG_GETARG_CSTRING(3);
    if (PG_NARGS() > 4 && !PG_ARGISNULL(4))
        property_type = PG_GETARG_CSTRING(4);

    if (index_name[0] == '\0')
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("index name must not be empty")));

    label_cache = lookup_label_for_index_helper(graph_name, label_name, '\0');
    created_index_name = create_index_on_property(
        graph_name, (char *)get_label_cache_relation_name(label_cache),
        property_name, property_type, index_name);
    record_graph_index_metadata(graph_name, label_cache, created_index_name,
                                "PROPERTY", NULL, property_name, "btree",
                                property_type);

    PG_RETURN_TEXT_P(cstring_to_text(created_index_name));
}

Datum create_adjacency_index(PG_FUNCTION_ARGS)
{
    char *graph_name;
    char *label_name;
    char *direction;
    label_cache_data *label_cache;
    bool outgoing;
    char *index_name;

    if (PG_ARGISNULL(0))
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("graph name must not be NULL")));
    if (PG_ARGISNULL(1))
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("label name must not be NULL")));
    if (PG_ARGISNULL(2))
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("direction must not be NULL")));

    graph_name = PG_GETARG_CSTRING(0);
    label_name = PG_GETARG_CSTRING(1);
    direction = PG_GETARG_CSTRING(2);

    if (pg_strcasecmp(direction, "out") == 0 ||
        pg_strcasecmp(direction, "outgoing") == 0)
        outgoing = true;
    else if (pg_strcasecmp(direction, "in") == 0 ||
             pg_strcasecmp(direction, "incoming") == 0)
        outgoing = false;
    else
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("direction must be 'out' or 'in'")));

    label_cache = lookup_label_for_index_helper(graph_name, label_name,
                                                LABEL_TYPE_EDGE);
    index_name = create_index_on_adjacency(
        graph_name, (char *)get_label_cache_relation_name(label_cache),
        outgoing, NULL);
    record_graph_index_metadata(graph_name, label_cache, index_name,
                                "ADJACENCY", outgoing ? "out" : "in",
                                NULL, "age_adjacency", NULL);

    PG_RETURN_VOID();
}

Datum create_adjacency_index_named(PG_FUNCTION_ARGS)
{
    char *graph_name;
    char *label_name;
    char *direction;
    char *index_name;
    label_cache_data *label_cache;
    bool outgoing;
    char *created_index_name;

    if (PG_ARGISNULL(0) || PG_ARGISNULL(1) || PG_ARGISNULL(2) ||
        PG_ARGISNULL(3))
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("graph, label, direction, and index name are required")));

    graph_name = PG_GETARG_CSTRING(0);
    label_name = PG_GETARG_CSTRING(1);
    direction = PG_GETARG_CSTRING(2);
    index_name = PG_GETARG_CSTRING(3);

    if (pg_strcasecmp(direction, "out") == 0 ||
        pg_strcasecmp(direction, "outgoing") == 0)
        outgoing = true;
    else if (pg_strcasecmp(direction, "in") == 0 ||
             pg_strcasecmp(direction, "incoming") == 0)
        outgoing = false;
    else
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("direction must be 'out' or 'in'")));

    label_cache = lookup_label_for_index_helper(graph_name, label_name,
                                                LABEL_TYPE_EDGE);
    created_index_name = create_index_on_adjacency(
        graph_name, (char *)get_label_cache_relation_name(label_cache),
        outgoing, index_name);
    record_graph_index_metadata(graph_name, label_cache, created_index_name,
                                "ADJACENCY", outgoing ? "out" : "in",
                                NULL, "age_adjacency", NULL);

    PG_RETURN_TEXT_P(cstring_to_text(created_index_name));
}

Datum create_adjacency_indexes(PG_FUNCTION_ARGS)
{
    char *graph_name;
    char *label_name;
    label_cache_data *label_cache;
    char *index_name;

    if (PG_ARGISNULL(0))
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("graph name must not be NULL")));
    if (PG_ARGISNULL(1))
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("label name must not be NULL")));

    graph_name = PG_GETARG_CSTRING(0);
    label_name = PG_GETARG_CSTRING(1);
    label_cache = lookup_label_for_index_helper(graph_name, label_name,
                                                LABEL_TYPE_EDGE);
    index_name = create_index_on_adjacency(
        graph_name, (char *)get_label_cache_relation_name(label_cache),
        true, NULL);
    record_graph_index_metadata(graph_name, label_cache, index_name,
                                "ADJACENCY", "out", NULL,
                                "age_adjacency", NULL);
    index_name = create_index_on_adjacency(
        graph_name, (char *)get_label_cache_relation_name(label_cache),
        false, NULL);
    record_graph_index_metadata(graph_name, label_cache, index_name,
                                "ADJACENCY", "in", NULL,
                                "age_adjacency", NULL);

    PG_RETURN_VOID();
}

Datum create_adjacency_indexes_named(PG_FUNCTION_ARGS)
{
    char *graph_name;
    char *label_name;
    char *index_name;
    label_cache_data *label_cache;
    char *created_index_name;

    if (PG_ARGISNULL(0) || PG_ARGISNULL(1) || PG_ARGISNULL(2))
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("graph, label, and index name are required")));

    graph_name = PG_GETARG_CSTRING(0);
    label_name = PG_GETARG_CSTRING(1);
    index_name = PG_GETARG_CSTRING(2);
    label_cache = lookup_label_for_index_helper(graph_name, label_name,
                                                LABEL_TYPE_EDGE);
    created_index_name = create_index_on_adjacency(
        graph_name, (char *)get_label_cache_relation_name(label_cache),
        true, psprintf("%s_out", index_name));
    record_graph_index_metadata(graph_name, label_cache, created_index_name,
                                "ADJACENCY", "out", NULL,
                                "age_adjacency", NULL);
    created_index_name = create_index_on_adjacency(
        graph_name, (char *)get_label_cache_relation_name(label_cache),
        false, psprintf("%s_in", index_name));
    record_graph_index_metadata(graph_name, label_cache, created_index_name,
                                "ADJACENCY", "in", NULL,
                                "age_adjacency", NULL);

    PG_RETURN_TEXT_P(cstring_to_text(index_name));
}

Datum drop_graph_index(PG_FUNCTION_ARGS)
{
    char *graph_name;
    char *index_name;
    Oid schema_oid;
    Oid index_oid;
    char *drop_sql;

    if (PG_ARGISNULL(0) || PG_ARGISNULL(1))
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("graph and index name are required")));

    graph_name = PG_GETARG_CSTRING(0);
    index_name = PG_GETARG_CSTRING(1);
    if (index_name[0] == '\0')
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("index name must not be empty")));

    schema_oid = get_namespace_oid(graph_name, false);
    index_oid = get_relname_relid(index_name, schema_oid);
    if (OidIsValid(index_oid))
    {
        drop_sql = psprintf("DROP INDEX %s.%s",
                            quote_identifier(graph_name),
                            quote_identifier(index_name));
        if (SPI_connect() != SPI_OK_CONNECT)
            elog(ERROR, "SPI_connect failed");
        if (SPI_execute(drop_sql, false, 0) != SPI_OK_UTILITY)
            elog(ERROR, "failed to drop graph index \"%s\"", index_name);
        if (SPI_finish() != SPI_OK_FINISH)
            elog(ERROR, "SPI_finish failed");
    }

    delete_graph_index_metadata(graph_name, index_name);

    PG_RETURN_TEXT_P(cstring_to_text(index_name));
}

/*
 * For the new label, create an entry in ag_catalog.ag_label, create a
 * new table and sequence. Returns the new label relation OID.
 */
Oid create_label(char *graph_name, char *label_name, char label_type,
                 List *parents)
{
    graph_cache_data *cache_data;

    if (!is_valid_label_name(label_name, label_type))
    {
        ereport(ERROR, (errcode(ERRCODE_UNDEFINED_SCHEMA),
                        errmsg("label name is invalid")));
    }

    cache_data = search_graph_name_cache_cached(graph_name);
    if (!cache_data)
    {
        ereport(ERROR, (errcode(ERRCODE_UNDEFINED_SCHEMA),
                        errmsg("graph \"%s\" does not exist", graph_name)));
    }

    return create_label_with_graph_cache(graph_name, label_name, label_type,
                                         parents, cache_data);
}

Oid create_label_with_graph_oid(char *graph_name, Oid graph_oid,
                                char *label_name, char label_type,
                                List *parents)
{
    graph_cache_data *cache_data;

    if (!is_valid_label_name(label_name, label_type))
    {
        ereport(ERROR, (errcode(ERRCODE_UNDEFINED_SCHEMA),
                        errmsg("label name is invalid")));
    }

    cache_data = search_graph_namespace_cache_cached(graph_oid);
    if (!cache_data || strcmp(NameStr(cache_data->name), graph_name) != 0)
    {
        cache_data = search_graph_name_cache_cached(graph_name);
    }
    if (!cache_data || cache_data->oid != graph_oid)
    {
        ereport(ERROR, (errcode(ERRCODE_UNDEFINED_SCHEMA),
                        errmsg("graph \"%s\" does not exist", graph_name)));
    }

    return create_label_with_graph_cache(graph_name, label_name, label_type,
                                         parents, cache_data);
}

static Oid create_label_with_graph_cache(char *graph_name, char *label_name,
                                         char label_type, List *parents,
                                         graph_cache_data *cache_data)
{
    Oid graph_oid;
    Oid nsp_id;
    char *schema_name;
    char *rel_name;
    char *seq_name;
    RangeVar *seq_range_var;
    int32 label_id;
    Oid relation_id;

    graph_oid = cache_data->oid;
    nsp_id = cache_data->namespace;

    /* graph namespace names are kept in sync with graph names */
    schema_name = pstrdup(NameStr(cache_data->name));
    rel_name = gen_label_relation_name(label_name);
    seq_name = ChooseRelationName(rel_name, "id", "seq", nsp_id, false);
    seq_range_var = makeRangeVar(schema_name, seq_name, -1);
    create_sequence_for_label(seq_range_var);

    /* create a table for the new label */
    create_table_for_label(graph_name, label_name, schema_name, rel_name,
                           seq_name, label_type, parents);

    /* record the new label in ag_label */
    relation_id = get_relname_relid(rel_name, nsp_id);

    /* If a label has parents, switch the parents id default, with its own. */
    if (list_length(parents) != 0)
        change_label_id_default(graph_name, label_name, schema_name, seq_name,
                                relation_id);

    /* associate the sequence with the "id" column */
    alter_sequence_owned_by_for_label(seq_range_var, rel_name);

    /* get a new "id" for the new label */
    label_id = get_new_label_id(graph_oid, nsp_id);

    insert_label(label_name, graph_oid, label_id, label_type,
                 relation_id, seq_name);

    CommandCounterIncrement();

    return relation_id;
}

/* 
 * CREATE TABLE `schema_name`.`rel_name` (
 * "id" graphid PRIMARY KEY DEFAULT "ag_catalog"."_graphid"(...),
 * "start_id" graphid NOT NULL note: only for edge labels
 * "end_id" graphid NOT NULL  note: only for edge labels
 * "properties" agtype NOT NULL DEFAULT "ag_catalog"."agtype_build_map"()
 * )
 */
static void create_table_for_label(char *graph_name, char *label_name,
                                   char *schema_name, char *rel_name,
                                   char *seq_name, char label_type,
                                   List *parents)
{
    CreateStmt *create_stmt;
    PlannedStmt *wrapper;

    create_stmt = makeNode(CreateStmt);

    /* relpersistence is set to RELPERSISTENCE_PERMANENT by makeRangeVar() */
    create_stmt->relation = makeRangeVar(schema_name, rel_name, -1);

    /*
     * When a new table has parents, do not create a column definition list.
     * Use the parents' column definition list instead, via Postgres'
     * inheritance system.
     */
    if (list_length(parents) != 0)
    {
        create_stmt->tableElts = NIL;
    }
    else if (label_type == LABEL_TYPE_EDGE)
    {
        create_stmt->tableElts = create_edge_table_elements(
            graph_name, label_name, schema_name, rel_name, seq_name);
    }
    else if (label_type == LABEL_TYPE_VERTEX)
    {
        create_stmt->tableElts = create_vertex_table_elements(
            graph_name, label_name, schema_name, rel_name, seq_name);
    }
    else
    {
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                        errmsg("undefined label type \'%c\'", label_type)));
    }

    create_stmt->inhRelations = parents;
    create_stmt->partbound = NULL;
    create_stmt->ofTypename = NULL;
    create_stmt->constraints = NIL;
    create_stmt->options = NIL;
    create_stmt->oncommit = ONCOMMIT_NOOP;
    create_stmt->tablespacename = NULL;
    create_stmt->if_not_exists = false;

    wrapper = makeNode(PlannedStmt);
    wrapper->commandType = CMD_UTILITY;
    wrapper->canSetTag = false;
    wrapper->utilityStmt = (Node *)create_stmt;
    wrapper->stmt_location = -1;
    wrapper->stmt_len = 0;

    ProcessUtility(wrapper, "(generated CREATE TABLE command)", false,
                   PROCESS_UTILITY_SUBCOMMAND, NULL, NULL, None_Receiver,
                   NULL);
    
    /* Create index on id columns */
    if (label_type == LABEL_TYPE_VERTEX)
    {
        create_index_on_column(schema_name, rel_name, "id", true);
    }
    else if (label_type == LABEL_TYPE_EDGE)
    {
        create_index_on_column(schema_name, rel_name, "start_id", false);
        create_index_on_column(schema_name, rel_name, "end_id", false);
    }

    /*
     * Install a cache invalidation trigger on the new label table, if the
     * trigger function exists. The function is registered in the extension
     * SQL (age_main.sql). It may not exist if running against an older
     * version of the extension SQL that hasn't been upgraded yet.
     *
     * When installed, the trigger fires AFTER INSERT/UPDATE/DELETE/TRUNCATE
     * (FOR EACH STATEMENT) and increments the graph's version counter so
     * VLE caches are properly invalidated when the table is modified via SQL.
     */
    {
        Oid func_oid;

        /* check if the trigger function is registered in the catalog */
        func_oid = LookupFuncName(
            list_make2(makeString("ag_catalog"),
                       makeString("age_invalidate_graph_cache")),
            0, NULL, true);

        if (OidIsValid(func_oid))
        {
            CreateTrigStmt *trigger_stmt = makeNode(CreateTrigStmt);
            PlannedStmt *trigger_wrapper;

            trigger_stmt->replace = false;
            trigger_stmt->isconstraint = false;
            trigger_stmt->trigname = "_age_cache_invalidate";
            trigger_stmt->relation = makeRangeVar(schema_name, rel_name, -1);
            trigger_stmt->funcname = list_make2(makeString("ag_catalog"),
                                                makeString("age_invalidate_graph_cache"));
            trigger_stmt->args = NIL;
            trigger_stmt->row = false;
            trigger_stmt->timing = TRIGGER_TYPE_AFTER;
            trigger_stmt->events = TRIGGER_TYPE_INSERT | TRIGGER_TYPE_UPDATE |
                                   TRIGGER_TYPE_DELETE | TRIGGER_TYPE_TRUNCATE;
            trigger_stmt->columns = NIL;
            trigger_stmt->whenClause = NULL;
            trigger_stmt->transitionRels = NIL;
            trigger_stmt->deferrable = false;
            trigger_stmt->initdeferred = false;
            trigger_stmt->constrrel = NULL;

            trigger_wrapper = makeNode(PlannedStmt);
            trigger_wrapper->commandType = CMD_UTILITY;
            trigger_wrapper->canSetTag = false;
            trigger_wrapper->utilityStmt = (Node *) trigger_stmt;
            trigger_wrapper->stmt_location = -1;
            trigger_wrapper->stmt_len = 0;

            ProcessUtility(trigger_wrapper,
                           "(generated CREATE TRIGGER command)",
                           false, PROCESS_UTILITY_SUBCOMMAND,
                           NULL, NULL, None_Receiver, NULL);

            CommandCounterIncrement();
        }
    }
}

static void create_index_on_column(char *schema_name,
                                   char *rel_name,
                                   char *colname,
                                   bool unique)
{
    IndexStmt *index_stmt;
    IndexElem *index_col;
    PlannedStmt *index_wrapper;

    index_stmt = makeNode(IndexStmt);
    index_col = makeNode(IndexElem);
    index_col->name = colname;
    index_col->expr = NULL;
    index_col->indexcolname = NULL;
    index_col->collation = NIL;
    index_col->opclass = list_make1(makeString("graphid_ops"));
    index_col->opclassopts = NIL;
    index_col->ordering = SORTBY_DEFAULT;
    index_col->nulls_ordering = SORTBY_NULLS_DEFAULT;

    index_stmt->relation = makeRangeVar(schema_name, rel_name, -1);
    index_stmt->accessMethod = "btree";
    index_stmt->tableSpace = NULL;
    index_stmt->indexParams = list_make1(index_col);
    index_stmt->options = NIL;
    index_stmt->whereClause = NULL;
    index_stmt->excludeOpNames = NIL;
    index_stmt->idxcomment = NULL;
    index_stmt->indexOid = InvalidOid;
    index_stmt->unique = unique;
    index_stmt->nulls_not_distinct = false;
    index_stmt->primary = unique;
    index_stmt->isconstraint = unique;
    index_stmt->deferrable = false;
    index_stmt->initdeferred = false;
    index_stmt->transformed = false;
    index_stmt->concurrent = false;
    index_stmt->if_not_exists = false;
    index_stmt->reset_default_tblspc = false;

    index_wrapper = makeNode(PlannedStmt);
    index_wrapper->commandType = CMD_UTILITY;
    index_wrapper->canSetTag = false;
    index_wrapper->utilityStmt = (Node *)index_stmt;
    index_wrapper->stmt_location = -1;
    index_wrapper->stmt_len = 0;

    ProcessUtility(index_wrapper, "(generated CREATE INDEX command)", false,
                   PROCESS_UTILITY_SUBCOMMAND, NULL, NULL, None_Receiver,
                   NULL);
}

static char *create_index_on_property(char *schema_name, char *rel_name,
                                      char *property_name,
                                      char *property_type,
                                      char *index_name)
{
    IndexStmt *index_stmt;
    IndexElem *index_expr;
    PlannedStmt *index_wrapper;

    index_stmt = makeNode(IndexStmt);
    index_expr = makeNode(IndexElem);
    index_expr->name = NULL;
    index_expr->expr = property_type == NULL ?
        build_property_index_expr(property_name) :
        build_typed_property_index_expr(property_name, property_type);
    index_expr->indexcolname = NULL;
    index_expr->collation = NIL;
    index_expr->opclass = NIL;
    index_expr->opclassopts = NIL;
    index_expr->ordering = SORTBY_DEFAULT;
    index_expr->nulls_ordering = SORTBY_NULLS_DEFAULT;

    if (index_name == NULL)
        index_name = make_property_index_name(rel_name, property_name,
                                              property_type);
    index_stmt->idxname = index_name;
    index_stmt->relation = makeRangeVar(schema_name, rel_name, -1);
    index_stmt->accessMethod = "btree";
    index_stmt->tableSpace = NULL;
    index_stmt->indexParams = list_make1(index_expr);
    index_stmt->options = NIL;
    index_stmt->whereClause = NULL;
    index_stmt->excludeOpNames = NIL;
    index_stmt->idxcomment = NULL;
    index_stmt->indexOid = InvalidOid;
    index_stmt->unique = false;
    index_stmt->nulls_not_distinct = false;
    index_stmt->primary = false;
    index_stmt->isconstraint = false;
    index_stmt->deferrable = false;
    index_stmt->initdeferred = false;
    index_stmt->transformed = false;
    index_stmt->concurrent = false;
    index_stmt->if_not_exists = false;
    index_stmt->reset_default_tblspc = false;

    index_wrapper = makeNode(PlannedStmt);
    index_wrapper->commandType = CMD_UTILITY;
    index_wrapper->canSetTag = false;
    index_wrapper->utilityStmt = (Node *)index_stmt;
    index_wrapper->stmt_location = -1;
    index_wrapper->stmt_len = 0;

    ProcessUtility(index_wrapper, "(generated CREATE PROPERTY INDEX command)",
                   false, PROCESS_UTILITY_SUBCOMMAND, NULL, NULL,
                   None_Receiver, NULL);
    CommandCounterIncrement();

    return index_name;
}

static char *create_index_on_adjacency(char *schema_name, char *rel_name,
                                       bool outgoing, char *index_name)
{
    IndexStmt *index_stmt;
    IndexElem *endpoint_col;
    IndexElem *id_col;
    IndexElem *next_col;
    PlannedStmt *index_wrapper;

    endpoint_col = make_graphid_index_elem(
        outgoing ? AG_EDGE_COLNAME_START_ID : AG_EDGE_COLNAME_END_ID,
        "graphid_age_adjacency_ops");
    id_col = make_graphid_index_elem(AG_EDGE_COLNAME_ID,
                                     "graphid_age_adjacency_ops");
    next_col = make_graphid_index_elem(
        outgoing ? AG_EDGE_COLNAME_END_ID : AG_EDGE_COLNAME_START_ID,
        "graphid_age_adjacency_ops");

    index_stmt = makeNode(IndexStmt);
    if (index_name == NULL)
        index_name = make_adjacency_index_name(rel_name, outgoing);
    index_stmt->idxname = index_name;
    index_stmt->relation = makeRangeVar(schema_name, rel_name, -1);
    index_stmt->accessMethod = "age_adjacency";
    index_stmt->tableSpace = NULL;
    index_stmt->indexParams = list_make3(endpoint_col, id_col, next_col);
    index_stmt->options = NIL;
    index_stmt->whereClause = NULL;
    index_stmt->excludeOpNames = NIL;
    index_stmt->idxcomment = NULL;
    index_stmt->indexOid = InvalidOid;
    index_stmt->unique = false;
    index_stmt->nulls_not_distinct = false;
    index_stmt->primary = false;
    index_stmt->isconstraint = false;
    index_stmt->deferrable = false;
    index_stmt->initdeferred = false;
    index_stmt->transformed = false;
    index_stmt->concurrent = false;
    index_stmt->if_not_exists = true;
    index_stmt->reset_default_tblspc = false;

    index_wrapper = makeNode(PlannedStmt);
    index_wrapper->commandType = CMD_UTILITY;
    index_wrapper->canSetTag = false;
    index_wrapper->utilityStmt = (Node *)index_stmt;
    index_wrapper->stmt_location = -1;
    index_wrapper->stmt_len = 0;

    ProcessUtility(index_wrapper, "(generated CREATE ADJACENCY INDEX command)",
                   false, PROCESS_UTILITY_SUBCOMMAND, NULL, NULL,
                   None_Receiver, NULL);
    CommandCounterIncrement();

    return index_name;
}

static IndexElem *make_graphid_index_elem(char *colname, char *opclass_name)
{
    IndexElem *index_col;

    index_col = makeNode(IndexElem);
    index_col->name = colname;
    index_col->expr = NULL;
    index_col->indexcolname = NULL;
    index_col->collation = NIL;
    index_col->opclass = opclass_name != NULL ?
        list_make1(makeString(opclass_name)) : NIL;
    index_col->opclassopts = NIL;
    index_col->ordering = SORTBY_DEFAULT;
    index_col->nulls_ordering = SORTBY_NULLS_DEFAULT;

    return index_col;
}

static Node *build_property_index_expr(char *property_name)
{
    FuncCall *access_call;
    List *args;

    args = build_property_path_index_args(property_name);
    access_call = makeFuncCall(
        list_make2(makeString("ag_catalog"),
                   makeString("agtype_access_operator")),
        args, COERCE_SQL_SYNTAX, -1);

    if (list_length(args) == 1)
        access_call->func_variadic = true;

    return (Node *)access_call;
}

static Node *build_typed_property_index_expr(char *property_name,
                                             char *property_type)
{
    FuncCall *access_call;
    char *function_name;
    Node *container;
    Node *key;

    if (pg_strcasecmp(property_type, "agtype") == 0)
    {
        return build_property_index_expr(property_name);
    }

    function_name = property_index_helper_for_type(property_type);
    build_property_terminal_index_args(property_name, &container, &key);

    access_call = makeFuncCall(
        list_make2(makeString("ag_catalog"), makeString(function_name)),
        list_make2(container, key),
        COERCE_SQL_SYNTAX, -1);

    return (Node *)access_call;
}

static char *property_index_helper_for_type(char *property_type)
{
    if (pg_strcasecmp(property_type, "pg_bigint") == 0 ||
        pg_strcasecmp(property_type, "bigint") == 0 ||
        pg_strcasecmp(property_type, "int8") == 0)
    {
        return "agtype_object_field_int8";
    }
    if (pg_strcasecmp(property_type, "pg_float8") == 0 ||
        pg_strcasecmp(property_type, "float8") == 0 ||
        pg_strcasecmp(property_type, "double precision") == 0)
    {
        return "agtype_object_field_float8";
    }
    if (pg_strcasecmp(property_type, "pg_text") == 0 ||
        pg_strcasecmp(property_type, "text") == 0)
    {
        return "agtype_object_field_text_agtype";
    }
    if (pg_strcasecmp(property_type, "pg_numeric") == 0)
    {
        return "agtype_object_field_numeric";
    }
    if (pg_strcasecmp(property_type, "numeric") == 0)
    {
        return "agtype_object_field_numeric_agtype";
    }

    ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                    errmsg("unsupported property index type \"%s\"",
                           property_type)));
}

static void build_property_terminal_index_args(char *property_name,
                                               Node **container, Node **key)
{
    List *path_args;

    Assert(container != NULL);
    Assert(key != NULL);

    path_args = build_property_path_index_args(property_name);
    if (list_length(path_args) == 2)
    {
        *container = linitial(path_args);
        *key = lsecond(path_args);
    }
    else
    {
        A_ArrayExpr *path_array = linitial_node(A_ArrayExpr, path_args);
        A_ArrayExpr *prefix_array;
        FuncCall *prefix_call;
        List *prefix_elements = NIL;
        ListCell *lc;
        int element_index = 0;
        int last_index = list_length(path_array->elements) - 1;

        if (last_index < 1)
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                            errmsg("property path requires at least one key")));

        foreach(lc, path_array->elements)
        {
            if (element_index < last_index)
                prefix_elements = lappend(prefix_elements,
                                          copyObject(lfirst(lc)));
            else
                *key = copyObject(lfirst(lc));
            element_index++;
        }

        prefix_array = makeNode(A_ArrayExpr);
        prefix_array->elements = prefix_elements;
        prefix_array->list_start = -1;
        prefix_array->list_end = -1;
        prefix_array->location = -1;

        prefix_call = makeFuncCall(
            list_make2(makeString("ag_catalog"),
                       makeString("agtype_access_operator")),
            list_make1(prefix_array), COERCE_SQL_SYNTAX, -1);
        prefix_call->func_variadic = true;
        *container = (Node *)prefix_call;
    }
}

static Node *build_property_key_const(char *property_name)
{
    A_Const *key_const;
    TypeCast *key_cast;
    StringInfoData escaped_key;

    initStringInfo(&escaped_key);
    escape_json(&escaped_key, property_name);

    key_const = makeNode(A_Const);
    key_const->val.sval.type = T_String;
    key_const->val.sval.sval = escaped_key.data;
    key_const->location = -1;

    key_cast = makeNode(TypeCast);
    key_cast->arg = (Node *)key_const;
    key_cast->typeName = makeTypeNameFromNameList(
        list_make2(makeString("ag_catalog"), makeString("agtype")));
    key_cast->location = -1;

    return (Node *)key_cast;
}

static List *build_property_path_index_args(char *property_name)
{
    ColumnRef *properties;
    List *elements;
    char *path;
    char *component;
    char *dot;
    A_ArrayExpr *array_expr;

    properties = makeNode(ColumnRef);
    properties->fields = list_make1(makeString(AG_VERTEX_COLNAME_PROPERTIES));
    properties->location = -1;

    path = pstrdup(property_name);
    component = path;
    dot = strchr(component, '.');

    if (dot == NULL)
        return list_make2(properties, build_property_key_const(component));

    elements = list_make1(properties);
    for (;;)
    {
        dot = strchr(component, '.');
        if (dot != NULL)
            *dot = '\0';

        if (component[0] == '\0')
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                            errmsg("property path contains an empty key")));

        elements = lappend(elements, build_property_key_const(component));

        if (dot == NULL)
            break;

        component = dot + 1;
    }

    array_expr = makeNode(A_ArrayExpr);
    array_expr->elements = elements;
    array_expr->list_start = -1;
    array_expr->list_end = -1;
    array_expr->location = -1;

    return list_make1(array_expr);
}

static char *make_property_index_name(char *rel_name, char *property_name,
                                      char *property_type)
{
    char *raw_name;
    int i;

    if (property_type == NULL || pg_strcasecmp(property_type, "agtype") == 0)
        raw_name = psprintf("%s_%s_property_idx", rel_name, property_name);
    else
        raw_name = psprintf("%s_%s_%s_property_idx", rel_name, property_name,
                            property_type);

    for (i = 0; raw_name[i] != '\0'; i++)
    {
        if (!isalnum((unsigned char)raw_name[i]) && raw_name[i] != '_')
            raw_name[i] = '_';
    }

    return raw_name;
}

static char *make_adjacency_index_name(char *rel_name, bool outgoing)
{
    char *raw_name;
    int i;

    raw_name = psprintf("%s_%s_adjacency_idx", rel_name,
                        outgoing ? "out" : "in");

    for (i = 0; raw_name[i] != '\0'; i++)
    {
        if (!isalnum((unsigned char)raw_name[i]) && raw_name[i] != '_')
            raw_name[i] = '_';
    }

    return raw_name;
}

static label_cache_data *lookup_label_for_index_helper(char *graph_name,
                                                       char *label_name,
                                                       char required_kind)
{
    graph_cache_data *graph_cache;
    label_cache_data *label_cache;

    if (is_valid_graph_name(graph_name) == 0)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("graph name is invalid")));
    if (is_valid_label_name(label_name, 0) == 0)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("label name is invalid")));

    graph_cache = search_graph_name_cache_cached(graph_name);
    if (graph_cache == NULL)
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_SCHEMA),
                 errmsg("graph \"%s\" does not exist.", graph_name)));

    label_cache = search_label_name_graph_cache_cached(label_name,
                                                       graph_cache->oid);
    if (label_cache == NULL)
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_SCHEMA),
                 errmsg("label \"%s\" does not exist", label_name)));

    if (required_kind != '\0' && label_cache->kind != required_kind)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("label \"%s\" is not an edge label", label_name)));

    return label_cache;
}

static void record_graph_index_metadata(char *graph_name,
                                        label_cache_data *label_cache,
                                        char *index_name,
                                        char *index_kind,
                                        char *direction,
                                        char *property_name,
                                        char *provider,
                                        char *property_type)
{
    Oid schema_oid;
    Oid index_oid;
    Oid argtypes[11] = {
        OIDOID, TEXTOID, TEXTOID, CHAROID, TEXTOID,
        OIDOID, TEXTOID, TEXTOID, TEXTOID, TEXTOID,
        TEXTOID
    };
    Datum values[11];
    char nulls[11] = {' ', ' ', ' ', ' ', ' ', ' ', ' ', 'n', 'n', ' ', 'n'};
    int ret;

    schema_oid = get_namespace_oid(graph_name, false);
    index_oid = get_relname_relid(index_name, schema_oid);
    if (!OidIsValid(index_oid))
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_OBJECT),
                 errmsg("index \"%s\" was not created", index_name)));

    values[0] = ObjectIdGetDatum(label_cache->graph);
    values[1] = CStringGetTextDatum(graph_name);
    values[2] = CStringGetTextDatum(NameStr(label_cache->name));
    values[3] = CharGetDatum(label_cache->kind);
    values[4] = CStringGetTextDatum(index_name);
    values[5] = ObjectIdGetDatum(index_oid);
    values[6] = CStringGetTextDatum(index_kind);
    if (direction != NULL)
    {
        values[7] = CStringGetTextDatum(direction);
        nulls[7] = ' ';
    }
    else
        values[7] = (Datum)0;
    if (property_name != NULL)
    {
        values[8] = CStringGetTextDatum(property_name);
        nulls[8] = ' ';
    }
    else
        values[8] = (Datum)0;
    values[9] = CStringGetTextDatum(provider);
    if (property_type != NULL)
    {
        values[10] = CStringGetTextDatum(property_type);
        nulls[10] = ' ';
    }
    else
        values[10] = (Datum)0;

    if (SPI_connect() != SPI_OK_CONNECT)
        elog(ERROR, "SPI_connect failed");

    ret = SPI_execute_with_args(
        "INSERT INTO ag_catalog.ag_graph_index "
        "(graph_oid, graph_name, label_name, label_kind, index_name, "
        " index_oid, index_kind, direction, property_names, provider, options) "
        "VALUES ($1, $2::text::name, $3::text::name, $4, $5::text::name, "
        " $6, $7, $8, "
        " CASE WHEN $9 IS NULL THEN ARRAY[]::name[] "
        "      ELSE ARRAY[$9::text::name] END, "
        " $10, "
        " jsonb_strip_nulls(jsonb_build_object("
        "     'direction', $8, "
        "     'property_type', "
        "     CASE WHEN $9 IS NULL THEN NULL "
        "          ELSE COALESCE($11::text, 'agtype') END)) "
        ") "
        "ON CONFLICT (graph_oid, index_name) DO UPDATE SET "
        " index_oid = EXCLUDED.index_oid, "
        " index_kind = EXCLUDED.index_kind, "
        " direction = EXCLUDED.direction, "
        " property_names = EXCLUDED.property_names, "
        " provider = EXCLUDED.provider, "
        " options = EXCLUDED.options",
        11, argtypes, values, nulls, false, 0);

    if (ret != SPI_OK_INSERT)
        elog(ERROR, "failed to record graph index metadata");

    if (SPI_finish() != SPI_OK_FINISH)
        elog(ERROR, "SPI_finish failed");
}

static void delete_graph_index_metadata(char *graph_name, char *index_name)
{
    Oid graph_oid;
    Oid argtypes[2] = {OIDOID, TEXTOID};
    Datum values[2];
    char nulls[2] = {' ', ' '};
    int ret;

    graph_oid = get_graph_oid(graph_name);
    if (!OidIsValid(graph_oid))
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_SCHEMA),
                 errmsg("graph \"%s\" does not exist.", graph_name)));

    values[0] = ObjectIdGetDatum(graph_oid);
    values[1] = CStringGetTextDatum(index_name);

    if (SPI_connect() != SPI_OK_CONNECT)
        elog(ERROR, "SPI_connect failed");

    ret = SPI_execute_with_args(
        "DELETE FROM ag_catalog.ag_graph_index "
        "WHERE graph_oid = $1 AND index_name = $2::text::name",
        2, argtypes, values, nulls, false, 0);

    if (ret != SPI_OK_DELETE)
        elog(ERROR, "failed to delete graph index metadata");

    if (SPI_finish() != SPI_OK_FINISH)
        elog(ERROR, "SPI_finish failed");
}

/* 
 * CREATE TABLE `schema_name`.`rel_name` (
 * "id" graphid PRIMARY KEY DEFAULT "ag_catalog"."_graphid"(...),
 * "start_id" graphid NOT NULL
 * "end_id" graphid NOT NULL
 * "properties" agtype NOT NULL DEFAULT "ag_catalog"."agtype_build_map"()
 * )
 */
static List *create_edge_table_elements(char *graph_name, char *label_name,
                                        char *schema_name, char *rel_name,
                                        char *seq_name)
{
    ColumnDef *id;
    ColumnDef *start_id;
    ColumnDef *end_id;
    ColumnDef *props;

    /* "id" graphid PRIMARY KEY DEFAULT "ag_catalog"."_graphid"(...) */
    id = makeColumnDef(AG_EDGE_COLNAME_ID, GRAPHIDOID, -1, InvalidOid);
    id->constraints = list_make2(build_pk_constraint(),
                                 build_id_default(graph_name, label_name,
                                                  schema_name, seq_name));

    /* "start_id" graphid NOT NULL */
    start_id = makeColumnDef(AG_EDGE_COLNAME_START_ID, GRAPHIDOID, -1,
                             InvalidOid);
    start_id->constraints = list_make1(build_not_null_constraint());

    /* "end_id" graphid NOT NULL */
    end_id = makeColumnDef(AG_EDGE_COLNAME_END_ID, GRAPHIDOID, -1, InvalidOid);
    end_id->constraints = list_make1(build_not_null_constraint());

    /* "properties" agtype NOT NULL DEFAULT "ag_catalog"."agtype_build_map"() */
    props = makeColumnDef(AG_EDGE_COLNAME_PROPERTIES, AGTYPEOID, -1,
                          InvalidOid);
    props->constraints = list_make2(build_not_null_constraint(),
                                    build_properties_default());

    return list_make4(id, start_id, end_id, props);
}

/* 
 * CREATE TABLE `schema_name`.`rel_name` (
 * "id" graphid PRIMARY KEY DEFAULT "ag_catalog"."_graphid"(...),
 * "properties" agtype NOT NULL DEFAULT "ag_catalog"."agtype_build_map"()
 * )
 */
static List *create_vertex_table_elements(char *graph_name, char *label_name,
                                          char *schema_name, char *rel_name,
                                          char *seq_name)
{
    ColumnDef *id;
    ColumnDef *props;

    /* "id" graphid PRIMARY KEY DEFAULT "ag_catalog"."_graphid"(...) */
    id = makeColumnDef(AG_VERTEX_COLNAME_ID, GRAPHIDOID, -1, InvalidOid);
    id->constraints = list_make2(build_not_null_constraint(),
                                 build_id_default(graph_name, label_name,
                                                  schema_name, seq_name));

    /* "properties" agtype NOT NULL DEFAULT "ag_catalog"."agtype_build_map"() */
    props = makeColumnDef(AG_VERTEX_COLNAME_PROPERTIES, AGTYPEOID, -1,
                          InvalidOid);
    props->constraints = list_make2(build_not_null_constraint(),
                                    build_properties_default());

    return list_make2(id, props);
}

/* CREATE SEQUENCE `seq_range_var` MAXVALUE `LOCAL_ID_MAX` */
static void create_sequence_for_label(RangeVar *seq_range_var)
{
    ParseState *pstate;
    CreateSeqStmt *seq_stmt;
    /* greater than MAXINT8LEN+1 */
    char buf[32];
    DefElem *maxvalue;
    int len;

    pstate = make_parsestate(NULL);
    pstate->p_sourcetext = "(generated CREATE SEQUENCE command)";

    seq_stmt = makeNode(CreateSeqStmt);
    seq_stmt->sequence = seq_range_var;
    len = pg_lltoa(ENTRY_ID_MAX, buf);
    maxvalue = makeDefElem("maxvalue", (Node *)makeFloat(pnstrdup(buf, len)),
                           -1);
    seq_stmt->options = list_make1(maxvalue);
    seq_stmt->ownerId = InvalidOid;
    seq_stmt->for_identity = false;
    seq_stmt->if_not_exists = false;

    DefineSequence(pstate, seq_stmt);
    CommandCounterIncrement();
}

/*
 * Builds the primary key constraint for when a table is created.
 */
static Constraint *build_pk_constraint(void)
{
    Constraint *pk;

    pk = makeNode(Constraint);
    pk->contype = CONSTR_PRIMARY;
    pk->location = -1;
    pk->keys = NULL;
    pk->options = NIL;
    pk->indexname = NULL;
    pk->indexspace = NULL;

    return pk;
}

/*
 * Construct a FuncCall node that will create the default logic for the label's
 * id.
 */
static FuncCall *build_id_default_func_expr(char *graph_name, char *label_name,
                                            char *schema_name, char *seq_name)
{
    List *label_id_func_name;
    A_Const *graph_name_const;
    A_Const *label_name_const;
    List *label_id_func_args;
    FuncCall *label_id_func;
    List *nextval_func_name;
    char *qualified_seq_name;
    A_Const *qualified_seq_name_const;
    TypeCast *regclass_cast;
    List *nextval_func_args;
    FuncCall *nextval_func;
    List *graphid_func_name;
    List *graphid_func_args;
    FuncCall *graphid_func;

    /* Build a node that gets the label id */
    label_id_func_name = list_make2(makeString("ag_catalog"),
                                    makeString("_label_id"));
    graph_name_const = makeNode(A_Const);
    graph_name_const->val.sval.type = T_String;
    graph_name_const->val.sval.sval = graph_name;
    graph_name_const->location = -1;
    label_name_const = makeNode(A_Const);
    label_name_const->val.sval.type = T_String;
    label_name_const->val.sval.sval = label_name;
    label_name_const->location = -1;
    label_id_func_args = list_make2(graph_name_const, label_name_const);
    label_id_func = makeFuncCall(label_id_func_name, label_id_func_args, COERCE_SQL_SYNTAX, -1);

    /* Build a node that will get the next val from the label's sequence */
    nextval_func_name = SystemFuncName("nextval");
    qualified_seq_name = quote_qualified_identifier(schema_name, seq_name);
    qualified_seq_name_const = makeNode(A_Const);
    qualified_seq_name_const->val.sval.type = T_String;
    qualified_seq_name_const->val.sval.sval = qualified_seq_name;
    qualified_seq_name_const->location = -1;
    regclass_cast = makeNode(TypeCast);
    regclass_cast->typeName = SystemTypeName("regclass");
    regclass_cast->arg = (Node *)qualified_seq_name_const;
    regclass_cast->location = -1;
    nextval_func_args = list_make1(regclass_cast);
    nextval_func = makeFuncCall(nextval_func_name, nextval_func_args, COERCE_SQL_SYNTAX, -1);

    /*
     * Build a node that constructs the graphid from the label id function
     * and the next val function for the given sequence.
     */
    graphid_func_name = list_make2(makeString("ag_catalog"),
                                   makeString("_graphid"));
    graphid_func_args = list_make2(label_id_func, nextval_func);
    graphid_func = makeFuncCall(graphid_func_name, graphid_func_args, COERCE_SQL_SYNTAX, -1);

    return graphid_func;
}

/*
 * Construct a default constraint on the id column for a newly created table
 */
static Constraint *build_id_default(char *graph_name, char *label_name,
                                    char *schema_name, char *seq_name)
{
    FuncCall *graphid_func;
    Constraint *id_default;

    graphid_func = build_id_default_func_expr(graph_name, label_name,
                                              schema_name, seq_name);

    id_default = makeNode(Constraint);
    id_default->contype = CONSTR_DEFAULT;
    id_default->location = -1;
    id_default->raw_expr = (Node *)graphid_func;
    id_default->cooked_expr = NULL;

    return id_default;
}

/* NOT NULL */
static Constraint *build_not_null_constraint(void)
{
    Constraint *not_null;

    not_null = makeNode(Constraint);
    not_null->contype = CONSTR_NOTNULL;
    not_null->location = -1;

    return not_null;
}

/* DEFAULT "ag_catalog"."agtype_build_map"() */
static Constraint *build_properties_default(void)
{
    List *func_name;
    FuncCall *func;
    Constraint *props_default;

    /* "ag_catalog"."agtype_build_map"() */
    func_name = list_make2(makeString("ag_catalog"),
                           makeString("agtype_build_map"));
    func = makeFuncCall(func_name, NIL, COERCE_SQL_SYNTAX, -1);

    props_default = makeNode(Constraint);
    props_default->contype = CONSTR_DEFAULT;
    props_default->location = -1;
    props_default->raw_expr = (Node *)func;
    props_default->cooked_expr = NULL;

    return props_default;
}

/*
 * Alter the default constraint on the label's id to the use the given
 * sequence.
 */
static void change_label_id_default(char *graph_name, char *label_name,
                                    char *schema_name, char *seq_name,
                                    Oid relid)
{
    ParseState *pstate;
    AlterTableStmt *tbl_stmt;
    AlterTableCmd *tbl_cmd;
    RangeVar *rv;
    FuncCall *func_call;
    AlterTableUtilityContext atuc;

    func_call = build_id_default_func_expr(graph_name, label_name, schema_name,
                                           seq_name);

    rv = makeRangeVar(schema_name, label_name, -1);

    pstate = make_parsestate(NULL);
    pstate->p_sourcetext = "(generated ALTER TABLE command)";

    tbl_stmt = makeNode(AlterTableStmt);
    tbl_stmt->relation = rv;
    tbl_stmt->missing_ok = false;

    tbl_cmd = makeNode(AlterTableCmd);
    tbl_cmd->subtype = AT_ColumnDefault;
    tbl_cmd->name = "id";
    tbl_cmd->def = (Node *)func_call;

    tbl_stmt->cmds = list_make1(tbl_cmd);

    atuc.relid = relid;
    atuc.queryEnv = pstate->p_queryEnv;
    atuc.queryString = pstate->p_sourcetext;

    AlterTable(tbl_stmt, AccessExclusiveLock, &atuc);

    CommandCounterIncrement();
}

/* CREATE SEQUENCE `seq_range_var` OWNED BY `schema_name`.`rel_name`."id" */
static void alter_sequence_owned_by_for_label(RangeVar *seq_range_var,
                                              char *rel_name)
{
    ParseState *pstate;
    AlterSeqStmt *seq_stmt;
    char *schema_name;
    List *id;
    DefElem *owned_by;

    pstate = make_parsestate(NULL);
    pstate->p_sourcetext = "(generated ALTER SEQUENCE command)";

    seq_stmt = makeNode(AlterSeqStmt);
    seq_stmt->sequence = seq_range_var;
    schema_name = seq_range_var->schemaname;
    id = list_make3(makeString(schema_name), makeString(rel_name),
                    makeString("id"));
    owned_by = makeDefElem("owned_by", (Node *)id, -1);
    seq_stmt->options = list_make1(owned_by);
    seq_stmt->for_identity = false;
    seq_stmt->missing_ok = false;

    AlterSequence(pstate, seq_stmt);
    CommandCounterIncrement();
}

static int32 get_new_label_id(Oid graph_oid, Oid nsp_id)
{
    Oid seq_id;
    int cnt;

    /* get the OID of the sequence */
    seq_id = get_relname_relid(LABEL_ID_SEQ_NAME, nsp_id);
    if (!OidIsValid(seq_id))
    {
        ereport(ERROR, (errcode(ERRCODE_UNDEFINED_TABLE),
                        errmsg("sequence \"%s\" does not exists",
                               LABEL_ID_SEQ_NAME)));
    }

    for (cnt = LABEL_ID_MIN; cnt <= LABEL_ID_MAX; cnt++)
    {
        int32 label_id;

        /* the data type of the sequence is integer (int4) */
        label_id = (int32) nextval_internal(seq_id, true);
        Assert(label_id_is_valid(label_id));
        if (!label_id_exists(graph_oid, label_id))
        {
            return (int32) label_id;
        }
    }

    ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                    errmsg("no more new labels are available"),
                    errhint("The maximum number of labels in a graph is %d",
                            LABEL_ID_MAX)));
    return 0;
}

PG_FUNCTION_INFO_V1(drop_label);

Datum drop_label(PG_FUNCTION_ARGS)
{
    Name graph_name;
    Name label_name;
    bool force;
    char *graph_name_str;
    graph_cache_data *cache_data;
    label_cache_data *label_cache;
    Oid graph_oid;
    char *label_name_str;
    char *schema_name;
    char *rel_name;
    List *qname;

    if (PG_ARGISNULL(0))
    {
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("graph name must not be NULL")));
    }
    if (PG_ARGISNULL(1))
    {
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("label name must not be NULL")));
    }
    graph_name = PG_GETARG_NAME(0);
    label_name = PG_GETARG_NAME(1);
    force = PG_GETARG_BOOL(2);

    graph_name_str = NameStr(*graph_name);
    cache_data = search_graph_name_cache_cached(graph_name_str);
    if (!cache_data)
    {
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_SCHEMA),
                 errmsg("graph \"%s\" does not exist", graph_name_str)));
    }
    graph_oid = cache_data->oid;

    label_name_str = NameStr(*label_name);
    label_cache = search_label_name_graph_cache_cached(label_name_str,
                                                       graph_oid);
    if (label_cache == NULL)
    {
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_TABLE),
                 errmsg("label \"%s\" does not exist", label_name_str)));
    }

    if (force)
    {
        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                        errmsg("force option is not supported yet")));
    }

    schema_name = pstrdup(NameStr(cache_data->name));

    rel_name = pstrdup(get_label_cache_relation_name(label_cache));

    /* build qualified name */
    qname = list_make2(makeString(schema_name), makeString(rel_name));

    remove_relation(qname);
    /* CommandCounterIncrement() is called in performDeletion() */

    /* delete_label() will be called in object_access() */

    ereport(NOTICE, (errmsg("label \"%s\".\"%s\" has been dropped",
                            graph_name_str, label_name_str)));

    PG_RETURN_VOID();
}

/* See RemoveRelations() for more details. */
static void remove_relation(List *qname)
{
    RangeVar *rel;
    Oid rel_oid;
    ObjectAddress address;

    Assert(list_length(qname) == 2);

    /* concurrent is false so lockmode is AccessExclusiveLock */

    /* relkind is RELKIND_RELATION */

    AcceptInvalidationMessages();

    rel = makeRangeVarFromNameList(qname);
    rel_oid = RangeVarGetRelidExtended(rel, AccessExclusiveLock,
                                       RVR_MISSING_OK,
                                       range_var_callback_for_remove_relation,
                                       NULL);

    if (!OidIsValid(rel_oid))
    {
        /*
         * before calling this function, this condition is already checked in
         * drop_graph()
         */
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                        errmsg("ag_label catalog is corrupted"),
                        errhint("Table \"%s\".\"%s\" does not exist",
                                rel->schemaname, rel->relname)));
    }

    /* concurrent is false */

    ObjectAddressSet(address, RelationRelationId, rel_oid);

    /*
     * set PERFORM_DELETION_INTERNAL flag so that object_access_hook can ignore
     * this deletion
     */
    performDeletion(&address, DROP_RESTRICT, PERFORM_DELETION_INTERNAL);
}

/* See RangeVarCallbackForDropRelation() for more details. */
static void range_var_callback_for_remove_relation(const RangeVar *rel,
                                                   Oid rel_oid,
                                                   Oid odl_rel_oid,
                                                   void *arg)
{
    /*
     * arg is NULL because relkind is always RELKIND_RELATION, heapOid is
     * always InvalidOid, partParentOid is always InvalidOid, and concurrent is
     * always false. See RemoveRelations() for more details.
     */

    /* heapOid is always InvalidOid */

    /* partParentOid is always InvalidOid */

    if (!OidIsValid(rel_oid))
        return;

    /* classform->relkind is always RELKIND_RELATION */

    /* relkind == expected_relkind */

    if (!object_ownercheck(rel_oid, get_rel_namespace(rel_oid), GetUserId()))
    {
        aclcheck_error(ACLCHECK_NOT_OWNER,
                       get_relkind_objtype(get_rel_relkind(rel_oid)),
                       rel->relname);
    }

    /* the target relation is not system class */

    /* relkind is always RELKIND_RELATION */

    /* is_partition is false */
}
