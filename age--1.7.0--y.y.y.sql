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

--* This is a TEMPLATE for upgrading from the previous version of Apache AGE
--* Please adjust the below ALTER EXTENSION to reflect the -- correct version it
--* is upgrading to.

-- This will only work within a major version of PostgreSQL, not across
-- major versions.

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "ALTER EXTENSION age UPDATE TO '1.X.0'" to load this file. \quit

--* Please add all additions, deletions, and modifications to the end of this
--* file. We need to keep the order of these changes.
--* REMOVE ALL LINES ABOVE, and this one, that start with --*

--
-- pg_upgrade support functions
--
-- These functions help users upgrade PostgreSQL major versions using pg_upgrade
-- while preserving Apache AGE graph data.
--

CREATE FUNCTION ag_catalog.age_prepare_pg_upgrade()
    RETURNS void
    LANGUAGE plpgsql
    SET search_path = ag_catalog, pg_catalog
    AS $function$
DECLARE
    graph_count integer;
BEGIN
    -- Check if namespace column is already oid type (already prepared)
    IF EXISTS (
        SELECT 1 FROM information_schema.columns
        WHERE table_schema = 'ag_catalog'
          AND table_name = 'ag_graph'
          AND column_name = 'namespace'
          AND data_type = 'oid'
    ) THEN
        RAISE NOTICE 'Database already prepared for pg_upgrade (namespace is oid type).';
        RETURN;
    END IF;

    -- Drop existing backup table if it exists (from a previous failed attempt)
    DROP TABLE IF EXISTS public._age_pg_upgrade_backup;

    -- Create backup table with graph names mapped to namespace names
    -- We store nspname directly (not regnamespace::text) to avoid quoting issues
    -- Names survive pg_upgrade while OIDs don't
    CREATE TABLE public._age_pg_upgrade_backup AS
    SELECT
        g.graphid AS old_graphid,
        g.name AS graph_name,
        n.nspname AS namespace_name
    FROM ag_catalog.ag_graph g
    JOIN pg_namespace n ON n.oid = g.namespace::oid;

    SELECT count(*) INTO graph_count FROM public._age_pg_upgrade_backup;

    RAISE NOTICE 'Created backup table public._age_pg_upgrade_backup with % graph(s)', graph_count;

    -- Even with zero graphs, we still need to convert the column type
    -- because the regnamespace type itself blocks pg_upgrade

    -- Drop the existing regnamespace-based index
    DROP INDEX IF EXISTS ag_catalog.ag_graph_namespace_index;

    -- Convert namespace column from regnamespace to oid
    ALTER TABLE ag_catalog.ag_graph
        ALTER COLUMN namespace TYPE oid USING namespace::oid;

    -- Recreate the index with oid type
    CREATE UNIQUE INDEX ag_graph_namespace_index
        ON ag_catalog.ag_graph USING btree (namespace);

    -- Create a view for backward-compatible display of namespace as schema name
    CREATE OR REPLACE VIEW ag_catalog.ag_graph_view AS
    SELECT graphid, name, namespace::regnamespace AS namespace
    FROM ag_catalog.ag_graph;

    RAISE NOTICE 'Successfully prepared database for pg_upgrade.';
    RAISE NOTICE 'The ag_graph.namespace column has been converted from regnamespace to oid.';
    RAISE NOTICE 'You can now run pg_upgrade.';
    RAISE NOTICE 'After pg_upgrade completes, run: SELECT age_finish_pg_upgrade();';
END;
$function$;

COMMENT ON FUNCTION ag_catalog.age_prepare_pg_upgrade() IS
'Prepares an AGE database for pg_upgrade by converting ag_graph.namespace from regnamespace to oid type. Run this before pg_upgrade.';

CREATE FUNCTION ag_catalog.age_finish_pg_upgrade()
    RETURNS void
    LANGUAGE plpgsql
    SET search_path = ag_catalog, pg_catalog
    AS $function$
DECLARE
    mapping_count integer;
    updated_labels integer;
    updated_graphs integer;
BEGIN
    -- Check if backup table exists
    IF NOT EXISTS (
        SELECT 1 FROM information_schema.tables
        WHERE table_schema = 'public'
          AND table_name = '_age_pg_upgrade_backup'
    ) THEN
        RAISE EXCEPTION 'Backup table public._age_pg_upgrade_backup not found. '
            'Did you run age_prepare_pg_upgrade() before pg_upgrade?';
    END IF;

    -- Check if namespace column is oid type (was properly prepared)
    IF NOT EXISTS (
        SELECT 1 FROM information_schema.columns
        WHERE table_schema = 'ag_catalog'
          AND table_name = 'ag_graph'
          AND column_name = 'namespace'
          AND data_type = 'oid'
    ) THEN
        RAISE EXCEPTION 'ag_graph.namespace is not oid type. '
            'Did you run age_prepare_pg_upgrade() before pg_upgrade?';
    END IF;

    -- Create temporary mapping table with old and new OIDs
    CREATE TEMP TABLE _graphid_mapping AS
    SELECT
        b.old_graphid,
        b.graph_name,
        n.oid AS new_graphid
    FROM public._age_pg_upgrade_backup b
    JOIN pg_namespace n ON n.nspname = b.namespace_name;

    GET DIAGNOSTICS mapping_count = ROW_COUNT;

    -- Verify all backup rows were mapped (detect missing schemas)
    DECLARE
        backup_count integer;
    BEGIN
        SELECT count(*) INTO backup_count FROM public._age_pg_upgrade_backup;
        IF mapping_count < backup_count THEN
            RAISE EXCEPTION 'Only % of % graphs could be mapped. Some schema names may have changed or been dropped.',
                mapping_count, backup_count;
        END IF;
    END;

    -- Handle zero-graph case (still need to restore schema)
    IF mapping_count = 0 THEN
        RAISE NOTICE 'No graphs to remap (empty backup table).';
        DROP TABLE _graphid_mapping;
        -- Skip to schema restoration
    ELSE
        RAISE NOTICE 'Found % graph(s) to remap', mapping_count;

        -- Temporarily drop foreign key constraint
        ALTER TABLE ag_catalog.ag_label DROP CONSTRAINT IF EXISTS fk_graph_oid;

        -- Update ag_label.graph references to use new OIDs
        UPDATE ag_catalog.ag_label l
        SET graph = m.new_graphid
        FROM _graphid_mapping m
        WHERE l.graph = m.old_graphid;

        GET DIAGNOSTICS updated_labels = ROW_COUNT;
        RAISE NOTICE 'Updated % label record(s)', updated_labels;

        -- Update ag_graph.graphid and ag_graph.namespace to new OIDs
        UPDATE ag_catalog.ag_graph g
        SET graphid = m.new_graphid,
            namespace = m.new_graphid
        FROM _graphid_mapping m
        WHERE g.graphid = m.old_graphid;

        GET DIAGNOSTICS updated_graphs = ROW_COUNT;
        RAISE NOTICE 'Updated % graph record(s)', updated_graphs;

        -- Restore foreign key constraint
        ALTER TABLE ag_catalog.ag_label
        ADD CONSTRAINT fk_graph_oid
            FOREIGN KEY(graph) REFERENCES ag_catalog.ag_graph(graphid);

        -- Clean up temporary mapping table
        DROP TABLE _graphid_mapping;

        RAISE NOTICE 'Successfully completed pg_upgrade OID remapping.';
    END IF;

    --
    -- Restore original schema (revert namespace to regnamespace)
    --
    RAISE NOTICE 'Restoring original schema...';

    -- Drop the view (no longer needed with regnamespace)
    DROP VIEW IF EXISTS ag_catalog.ag_graph_view;

    -- Drop the existing oid-based index
    DROP INDEX IF EXISTS ag_catalog.ag_graph_namespace_index;

    -- Convert namespace column back to regnamespace
    ALTER TABLE ag_catalog.ag_graph
        ALTER COLUMN namespace TYPE regnamespace USING namespace::regnamespace;

    -- Recreate the index with regnamespace type
    CREATE UNIQUE INDEX ag_graph_namespace_index
        ON ag_catalog.ag_graph USING btree (namespace);

    RAISE NOTICE 'Successfully restored ag_graph.namespace to regnamespace type.';

    --
    -- Invalidate AGE's internal caches by touching each graph's namespace
    -- AGE registers a syscache callback on NAMESPACEOID, so altering a schema
    -- triggers cache invalidation. This ensures cypher queries work immediately
    -- without requiring a session reconnect.
    --
    -- We use xact-level advisory lock (auto-released at transaction end)
    -- and preserve original schema ownership.
    --
    RAISE NOTICE 'Invalidating AGE caches...';
    PERFORM pg_catalog.pg_advisory_xact_lock(hashtext('age_finish_pg_upgrade'));
    DECLARE
        graph_rec RECORD;
        cache_invalidated boolean := false;
    BEGIN
        FOR graph_rec IN
            SELECT n.nspname AS ns_name, r.rolname AS owner_name
            FROM ag_catalog.ag_graph g
            JOIN pg_namespace n ON n.oid = g.namespace
            JOIN pg_roles r ON r.oid = n.nspowner
        LOOP
            BEGIN
                -- Touch schema by changing owner to current_user then back to original
                -- This triggers cache invalidation without permanently changing ownership
                EXECUTE format('ALTER SCHEMA %I OWNER TO %I', graph_rec.ns_name, current_user);
                EXECUTE format('ALTER SCHEMA %I OWNER TO %I', graph_rec.ns_name, graph_rec.owner_name);
                cache_invalidated := true;
            EXCEPTION WHEN insufficient_privilege THEN
                -- If we can't change ownership, skip this schema
                -- The cache will be invalidated on first use anyway
                RAISE NOTICE 'Could not invalidate cache for schema % (insufficient privileges)', graph_rec.ns_name;
            END;
        END LOOP;
        IF NOT cache_invalidated AND (SELECT count(*) FROM ag_catalog.ag_graph) > 0 THEN
            RAISE NOTICE 'Cache invalidation skipped. You may need to reconnect for cypher queries to work.';
        END IF;
    END;

    -- Now that all steps succeeded, clean up the backup table
    DROP TABLE IF EXISTS public._age_pg_upgrade_backup;

    RAISE NOTICE '';
    RAISE NOTICE 'pg_upgrade complete. All graph data has been preserved.';
END;
$function$;

COMMENT ON FUNCTION ag_catalog.age_finish_pg_upgrade() IS
'Completes pg_upgrade by remapping stale OIDs and restoring the original schema. Run this after pg_upgrade.';

CREATE FUNCTION ag_catalog.age_revert_pg_upgrade_changes()
    RETURNS void
    LANGUAGE plpgsql
    SET search_path = ag_catalog, pg_catalog
    AS $function$
BEGIN
    -- Check if namespace column is oid type (needs reverting)
    IF NOT EXISTS (
        SELECT 1 FROM information_schema.columns
        WHERE table_schema = 'ag_catalog'
          AND table_name = 'ag_graph'
          AND column_name = 'namespace'
          AND data_type = 'oid'
    ) THEN
        RAISE NOTICE 'ag_graph.namespace is already regnamespace type. Nothing to revert.';
        RETURN;
    END IF;

    -- Drop the view (no longer needed with regnamespace)
    DROP VIEW IF EXISTS ag_catalog.ag_graph_view;

    -- Drop the existing oid-based index
    DROP INDEX IF EXISTS ag_catalog.ag_graph_namespace_index;

    -- Convert namespace column back to regnamespace
    ALTER TABLE ag_catalog.ag_graph
        ALTER COLUMN namespace TYPE regnamespace USING namespace::regnamespace;

    -- Recreate the index with regnamespace type
    CREATE UNIQUE INDEX ag_graph_namespace_index
        ON ag_catalog.ag_graph USING btree (namespace);

    --
    -- Invalidate AGE's internal caches by touching each graph's namespace
    -- We use xact-level advisory lock and preserve original ownership
    --
    PERFORM pg_catalog.pg_advisory_xact_lock(hashtext('age_revert_pg_upgrade'));
    DECLARE
        graph_rec RECORD;
    BEGIN
        FOR graph_rec IN
            SELECT n.nspname AS ns_name, r.rolname AS owner_name
            FROM ag_catalog.ag_graph g
            JOIN pg_namespace n ON n.oid = g.namespace
            JOIN pg_roles r ON r.oid = n.nspowner
        LOOP
            BEGIN
                -- Touch schema by changing owner to current_user then back to original
                EXECUTE format('ALTER SCHEMA %I OWNER TO %I', graph_rec.ns_name, current_user);
                EXECUTE format('ALTER SCHEMA %I OWNER TO %I', graph_rec.ns_name, graph_rec.owner_name);
            EXCEPTION WHEN insufficient_privilege THEN
                RAISE NOTICE 'Could not invalidate cache for schema % (insufficient privileges)', graph_rec.ns_name;
            END;
        END LOOP;
    END;

    RAISE NOTICE 'Successfully reverted ag_graph.namespace to regnamespace type.';
    RAISE NOTICE '';
    RAISE NOTICE 'Upgrade preparation has been cancelled.';
    RAISE NOTICE 'You may want to drop the backup table: DROP TABLE IF EXISTS public._age_pg_upgrade_backup;';
END;
$function$;

COMMENT ON FUNCTION ag_catalog.age_revert_pg_upgrade_changes() IS
'Reverts schema changes if you need to cancel after age_prepare_pg_upgrade() but before pg_upgrade. Not needed after age_finish_pg_upgrade().';

CREATE FUNCTION ag_catalog.age_pg_upgrade_status()
    RETURNS TABLE (
        status text,
        namespace_type text,
        graph_count bigint,
        backup_exists boolean,
        message text
    )
    LANGUAGE plpgsql
    SET search_path = ag_catalog, pg_catalog
    AS $function$
DECLARE
    ns_type text;
    g_count bigint;
    backup_exists boolean;
BEGIN
    -- Get namespace column type
    SELECT data_type INTO ns_type
    FROM information_schema.columns
    WHERE table_schema = 'ag_catalog'
      AND table_name = 'ag_graph'
      AND column_name = 'namespace';

    -- Get graph count
    SELECT count(*) INTO g_count FROM ag_catalog.ag_graph;

    -- Check for backup table
    SELECT EXISTS(
        SELECT 1 FROM information_schema.tables
        WHERE table_schema = 'public'
          AND table_name = '_age_pg_upgrade_backup'
    ) INTO backup_exists;

    -- Determine status and message
    IF ns_type = 'regnamespace' AND NOT backup_exists THEN
        -- Normal state - ready for use, needs prep before pg_upgrade
        RETURN QUERY SELECT
            'NORMAL'::text,
            ns_type,
            g_count,
            backup_exists,
            'Run SELECT age_prepare_pg_upgrade(); before pg_upgrade'::text;
    ELSIF ns_type = 'regnamespace' AND backup_exists THEN
        -- Unusual state - backup exists but schema wasn't converted
        RETURN QUERY SELECT
            'WARNING'::text,
            ns_type,
            g_count,
            backup_exists,
            'Backup table exists but schema not converted. Run age_prepare_pg_upgrade() again.'::text;
    ELSIF ns_type = 'oid' AND backup_exists THEN
        -- Prepared and ready for pg_upgrade, or awaiting finish after pg_upgrade
        RETURN QUERY SELECT
            'PREPARED - AWAITING FINISH'::text,
            ns_type,
            g_count,
            backup_exists,
            'After pg_upgrade, run SELECT age_finish_pg_upgrade();'::text;
    ELSE
        -- oid type without backup - manually converted or partial state
        RETURN QUERY SELECT
            'CONVERTED'::text,
            ns_type,
            g_count,
            backup_exists,
            'Namespace is oid type. If upgrading, ensure backup table exists.'::text;
    END IF;
END;
$function$;

COMMENT ON FUNCTION ag_catalog.age_pg_upgrade_status() IS
'Returns the current pg_upgrade readiness status of the AGE installation.';

--
-- VLE materialization and accessor helpers added after the initial 1.7.0 SQL.
CREATE FUNCTION ag_catalog.age_vle_path_length(agtype) RETURNS agtype LANGUAGE C STABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE FUNCTION ag_catalog.age_vle_terminal_id(agtype) RETURNS graphid LANGUAGE C STABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE FUNCTION ag_catalog.age_vle_terminal_vertex(agtype) RETURNS agtype LANGUAGE C STABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE FUNCTION ag_catalog.age_vle_terminal_vertex_properties(agtype) RETURNS agtype LANGUAGE C STABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE FUNCTION ag_catalog.age_vle_terminal_vertex_property(agtype, agtype) RETURNS agtype LANGUAGE C STABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE FUNCTION ag_catalog.age_vle_terminal_vertex_property_from_path(agtype, agtype) RETURNS agtype LANGUAGE C STABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE FUNCTION ag_catalog.age_vle_path_node_count(agtype) RETURNS agtype LANGUAGE C STABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE FUNCTION ag_catalog.age_vle_edge_tail_count(agtype) RETURNS agtype LANGUAGE C STABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE FUNCTION ag_catalog.age_vle_list_is_empty(agtype, agtype) RETURNS boolean LANGUAGE C STABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE FUNCTION ag_catalog.age_materialize_vle_nodes(agtype) RETURNS agtype LANGUAGE C STABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE FUNCTION ag_catalog.age_materialize_vle_node_at(agtype, agtype) RETURNS agtype LANGUAGE C STABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE FUNCTION ag_catalog.age_materialize_vle_node_reversed_at(agtype, agtype) RETURNS agtype LANGUAGE C STABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE FUNCTION ag_catalog.age_materialize_vle_node_tail_last(agtype) RETURNS agtype LANGUAGE C STABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE FUNCTION ag_catalog.age_vle_node_tail_last_id(agtype) RETURNS agtype LANGUAGE C STABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE FUNCTION ag_catalog.age_vle_node_id_at(agtype, agtype) RETURNS agtype LANGUAGE C STABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE FUNCTION ag_catalog.age_vle_node_label_at(agtype, agtype) RETURNS agtype LANGUAGE C STABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE FUNCTION ag_catalog.age_vle_node_labels_at(agtype, agtype) RETURNS agtype LANGUAGE C STABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE FUNCTION ag_catalog.age_vle_node_properties_at(agtype, agtype) RETURNS agtype LANGUAGE C STABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE FUNCTION ag_catalog.age_vle_node_property_at(agtype, agtype, agtype) RETURNS agtype LANGUAGE C STABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE FUNCTION ag_catalog.age_materialize_vle_nodes_slice(agtype, agtype, agtype) RETURNS agtype LANGUAGE C STABLE PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE FUNCTION ag_catalog.age_materialize_vle_list_slice(agtype, agtype, agtype, agtype) RETURNS agtype LANGUAGE C STABLE PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE FUNCTION ag_catalog.age_vle_list_slice_count(agtype, agtype, agtype, agtype) RETURNS agtype LANGUAGE C STABLE PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE FUNCTION ag_catalog.age_vle_list_slice_is_empty(agtype, agtype, agtype, agtype) RETURNS boolean LANGUAGE C STABLE PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE FUNCTION ag_catalog.age_materialize_vle_slice_boundary(agtype, agtype, agtype, agtype) RETURNS agtype LANGUAGE C STABLE PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE FUNCTION ag_catalog.age_materialize_vle_nodes_tail(agtype) RETURNS agtype LANGUAGE C STABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE FUNCTION ag_catalog.age_materialize_vle_nodes_reversed(agtype) RETURNS agtype LANGUAGE C STABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE FUNCTION ag_catalog.age_materialize_vle_edge_at(agtype, agtype) RETURNS agtype LANGUAGE C STABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE FUNCTION ag_catalog.age_materialize_vle_edge_reversed_at(agtype, agtype) RETURNS agtype LANGUAGE C STABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE FUNCTION ag_catalog.age_materialize_vle_edge_tail_last(agtype) RETURNS agtype LANGUAGE C STABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE FUNCTION ag_catalog.age_vle_edge_tail_last_id(agtype) RETURNS agtype LANGUAGE C STABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE FUNCTION ag_catalog.age_vle_tail_last_field(agtype, agtype) RETURNS agtype LANGUAGE C STABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE FUNCTION ag_catalog.age_vle_tail_last_edge_endpoint(agtype, agtype) RETURNS agtype LANGUAGE C STABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE FUNCTION ag_catalog.age_vle_tail_last_endpoint_field(agtype, agtype) RETURNS agtype LANGUAGE C STABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE FUNCTION ag_catalog.age_vle_edge_id_at(agtype, agtype) RETURNS agtype LANGUAGE C STABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE FUNCTION ag_catalog.age_vle_edge_index_exists(agtype, agtype) RETURNS agtype LANGUAGE C STABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE FUNCTION ag_catalog.age_vle_edge_indices_equal(agtype, agtype, agtype) RETURNS agtype LANGUAGE C STABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE FUNCTION ag_catalog.age_vle_edge_reversed_index_equal(agtype, agtype, agtype) RETURNS agtype LANGUAGE C STABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE FUNCTION ag_catalog.age_vle_edge_label_at(agtype, agtype) RETURNS agtype LANGUAGE C STABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE FUNCTION ag_catalog.age_vle_edge_properties_at(agtype, agtype) RETURNS agtype LANGUAGE C STABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE FUNCTION ag_catalog.age_vle_edge_property_at(agtype, agtype, agtype) RETURNS agtype LANGUAGE C STABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE FUNCTION ag_catalog.age_vle_edge_start_node_at(agtype, agtype) RETURNS agtype LANGUAGE C STABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE FUNCTION ag_catalog.age_vle_edge_end_node_at(agtype, agtype) RETURNS agtype LANGUAGE C STABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE FUNCTION ag_catalog.age_vle_edge_endpoint_field_at(agtype, agtype, agtype) RETURNS agtype LANGUAGE C STABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE FUNCTION ag_catalog.age_vle_edge_start_id_at(agtype, agtype) RETURNS agtype LANGUAGE C STABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE FUNCTION ag_catalog.age_vle_edge_end_id_at(agtype, agtype) RETURNS agtype LANGUAGE C STABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE FUNCTION ag_catalog.age_materialize_vle_edges_tail(agtype) RETURNS agtype LANGUAGE C STABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE FUNCTION ag_catalog.age_materialize_vle_edges_reversed(agtype) RETURNS agtype LANGUAGE C STABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';

--
-- Objects added to the 1.7.0 extension body after the initial 1.7.0 SQL.
--
CREATE FUNCTION ag_catalog.create_property_index(graph_name cstring,
                                                 label_name cstring,
                                                 property_name cstring)
    RETURNS void
    LANGUAGE c
AS 'MODULE_PATHNAME';

CREATE DOMAIN ag_catalog.age_auto_column AS ag_catalog.agtype;

CREATE FUNCTION ag_catalog.agtype_sortsupport(internal)
    RETURNS void
    LANGUAGE c
    IMMUTABLE
PARALLEL SAFE
AS 'MODULE_PATHNAME';

CREATE FUNCTION ag_catalog.agtype_ctid_field_agtype(oid, tid, agtype)
    RETURNS agtype
    LANGUAGE c
    STABLE
RETURNS NULL ON NULL INPUT
PARALLEL RESTRICTED
AS 'MODULE_PATHNAME';

CREATE FUNCTION ag_catalog.agtype_object_field_exists_nonnull(agtype, agtype)
    RETURNS bool
    LANGUAGE c
    IMMUTABLE
RETURNS NULL ON NULL INPUT
PARALLEL SAFE
AS 'MODULE_PATHNAME';

CREATE FUNCTION ag_catalog.agtype_object_field_equals(agtype, agtype, agtype)
    RETURNS bool
    LANGUAGE c
    IMMUTABLE
RETURNS NULL ON NULL INPUT
PARALLEL SAFE
AS 'MODULE_PATHNAME';

CREATE FUNCTION ag_catalog.agtype_object_field_cmp(agtype, agtype, agtype)
    RETURNS int4
    LANGUAGE c
    IMMUTABLE
RETURNS NULL ON NULL INPUT
PARALLEL SAFE
AS 'MODULE_PATHNAME';

CREATE FUNCTION ag_catalog.agtype_object_field_int8(agtype, agtype)
    RETURNS int8
    LANGUAGE c
    IMMUTABLE
RETURNS NULL ON NULL INPUT
PARALLEL SAFE
AS 'MODULE_PATHNAME';

CREATE FUNCTION ag_catalog.agtype_object_field_float8(agtype, agtype)
    RETURNS float8
    LANGUAGE c
    IMMUTABLE
RETURNS NULL ON NULL INPUT
PARALLEL SAFE
AS 'MODULE_PATHNAME';

CREATE FUNCTION ag_catalog.agtype_object_field_numeric_agtype(agtype, agtype)
    RETURNS agtype
    LANGUAGE c
    IMMUTABLE
RETURNS NULL ON NULL INPUT
PARALLEL SAFE
AS 'MODULE_PATHNAME';

CREATE FUNCTION ag_catalog.gin_extract_agtype_path(agtype, internal)
    RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C
IMMUTABLE
STRICT
PARALLEL SAFE;

CREATE FUNCTION ag_catalog.gin_extract_agtype_query_path(agtype, internal, int2,
                                                         internal, internal)
    RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C
IMMUTABLE
STRICT
PARALLEL SAFE;

CREATE FUNCTION ag_catalog.gin_consistent_agtype_path(internal, int2, agtype,
                                                      int4, internal, internal)
    RETURNS bool
AS 'MODULE_PATHNAME'
LANGUAGE C
IMMUTABLE
STRICT
PARALLEL SAFE;

CREATE FUNCTION ag_catalog.gin_triconsistent_agtype_path(internal, int2, agtype,
                                                         int4, internal,
                                                         internal, internal)
    RETURNS bool
AS 'MODULE_PATHNAME'
LANGUAGE C
IMMUTABLE
STRICT
PARALLEL SAFE;

CREATE OPERATOR CLASS ag_catalog.gin_agtype_path_ops
FOR TYPE agtype USING gin AS
  OPERATOR 7 @>(agtype, agtype),
  FUNCTION 1 pg_catalog.btint4cmp(int4, int4),
  FUNCTION 2 ag_catalog.gin_extract_agtype_path(agtype, internal),
  FUNCTION 3 ag_catalog.gin_extract_agtype_query_path(agtype, internal, int2,
                                                      internal, internal),
  FUNCTION 4 ag_catalog.gin_consistent_agtype_path(internal, int2, agtype,
                                                   int4, internal, internal),
  FUNCTION 6 ag_catalog.gin_triconsistent_agtype_path(internal, int2, agtype,
                                                      int4, internal, internal,
                                                      internal),
STORAGE int4;

CREATE FUNCTION ag_catalog._agtype_build_path_raw(VARIADIC "any")
    RETURNS agtype
    LANGUAGE c
    IMMUTABLE
CALLED ON NULL INPUT
PARALLEL SAFE
AS 'MODULE_PATHNAME';

CREATE FUNCTION ag_catalog._agtype_build_vertex_label(oid, graphid, agtype)
    RETURNS agtype
    LANGUAGE c
    IMMUTABLE
CALLED ON NULL INPUT
PARALLEL SAFE
AS 'MODULE_PATHNAME';

CREATE FUNCTION ag_catalog._agtype_build_edge_label(oid, graphid, graphid,
                                                    graphid, agtype)
    RETURNS agtype
    LANGUAGE c
    IMMUTABLE
CALLED ON NULL INPUT
PARALLEL SAFE
AS 'MODULE_PATHNAME';

CREATE FUNCTION ag_catalog.age_collect_property_aggtransfn(internal, agtype, agtype)
    RETURNS internal
    LANGUAGE c
    IMMUTABLE
PARALLEL SAFE
AS 'MODULE_PATHNAME';

CREATE FUNCTION ag_catalog.age_collect_float8_transfn(internal, float8)
    RETURNS internal
    LANGUAGE c
    IMMUTABLE
PARALLEL SAFE
AS 'MODULE_PATHNAME';

CREATE FUNCTION ag_catalog.age_collect_float8_finalfn(internal)
    RETURNS agtype
    LANGUAGE c
    IMMUTABLE
PARALLEL SAFE
AS 'MODULE_PATHNAME';

CREATE FUNCTION ag_catalog.age_collect_int8_transfn(internal, int8)
    RETURNS internal
    LANGUAGE c
    IMMUTABLE
PARALLEL SAFE
AS 'MODULE_PATHNAME';

CREATE FUNCTION ag_catalog.age_collect_int8_finalfn(internal)
    RETURNS agtype
    LANGUAGE c
    IMMUTABLE
PARALLEL SAFE
AS 'MODULE_PATHNAME';

CREATE FUNCTION ag_catalog.age_collect_text_transfn(internal, text)
    RETURNS internal
    LANGUAGE c
    IMMUTABLE
PARALLEL SAFE
AS 'MODULE_PATHNAME';

CREATE FUNCTION ag_catalog.age_collect_text_finalfn(internal)
    RETURNS agtype
    LANGUAGE c
    IMMUTABLE
PARALLEL SAFE
AS 'MODULE_PATHNAME';

CREATE FUNCTION ag_catalog.age_collect_numeric_property_transfn(internal, agtype, agtype)
    RETURNS internal
    LANGUAGE c
    IMMUTABLE
PARALLEL SAFE
AS 'MODULE_PATHNAME';

CREATE FUNCTION ag_catalog.age_collect_numeric_finalfn(internal)
    RETURNS agtype
    LANGUAGE c
    IMMUTABLE
PARALLEL SAFE
AS 'MODULE_PATHNAME';

CREATE FUNCTION ag_catalog.age_array_agg_property_transfn(internal, agtype, agtype)
    RETURNS internal
    LANGUAGE c
    IMMUTABLE
PARALLEL SAFE
AS 'MODULE_PATHNAME';

CREATE FUNCTION ag_catalog.age_array_agg_map2_property_transfn(internal, agtype, text, agtype, text, agtype)
    RETURNS internal
    LANGUAGE c
    IMMUTABLE
PARALLEL SAFE
AS 'MODULE_PATHNAME';

CREATE FUNCTION ag_catalog.age_array_agg_map_property_transfn(internal, agtype, text[], agtype[])
    RETURNS internal
    LANGUAGE c
    IMMUTABLE
PARALLEL SAFE
AS 'MODULE_PATHNAME';

CREATE FUNCTION ag_catalog.age_array_agg_list_property_transfn(internal, agtype, agtype[])
    RETURNS internal
    LANGUAGE c
    IMMUTABLE
PARALLEL SAFE
AS 'MODULE_PATHNAME';

CREATE FUNCTION ag_catalog.age_array_agg_property_finalfn(internal)
    RETURNS agtype[]
    LANGUAGE c
    IMMUTABLE
PARALLEL SAFE
AS 'MODULE_PATHNAME';

CREATE AGGREGATE ag_catalog.age_collect_property(agtype, agtype)
(
    stype = internal,
    sfunc = ag_catalog.age_collect_property_aggtransfn,
    finalfunc = ag_catalog.age_collect_aggfinalfn,
    parallel = safe
);

CREATE AGGREGATE ag_catalog.age_collect_float8(float8)
(
    stype = internal,
    sfunc = ag_catalog.age_collect_float8_transfn,
    finalfunc = ag_catalog.age_collect_float8_finalfn,
    parallel = safe
);

CREATE AGGREGATE ag_catalog.age_collect_int8(int8)
(
    stype = internal,
    sfunc = ag_catalog.age_collect_int8_transfn,
    finalfunc = ag_catalog.age_collect_int8_finalfn,
    parallel = safe
);

CREATE AGGREGATE ag_catalog.age_collect_text(text)
(
    stype = internal,
    sfunc = ag_catalog.age_collect_text_transfn,
    finalfunc = ag_catalog.age_collect_text_finalfn,
    parallel = safe
);

CREATE AGGREGATE ag_catalog.age_collect_numeric_property(agtype, agtype)
(
    stype = internal,
    sfunc = ag_catalog.age_collect_numeric_property_transfn,
    finalfunc = ag_catalog.age_collect_numeric_finalfn,
    parallel = safe
);

CREATE AGGREGATE ag_catalog.age_array_agg_property(agtype, agtype)
(
    stype = internal,
    sfunc = ag_catalog.age_array_agg_property_transfn,
    finalfunc = ag_catalog.age_array_agg_property_finalfn,
    parallel = safe
);

CREATE AGGREGATE ag_catalog.age_array_agg_map2_property(agtype, text, agtype, text, agtype)
(
    stype = internal,
    sfunc = ag_catalog.age_array_agg_map2_property_transfn,
    finalfunc = ag_catalog.age_array_agg_property_finalfn,
    parallel = safe
);

CREATE AGGREGATE ag_catalog.age_array_agg_map_property(agtype, text[], agtype[])
(
    stype = internal,
    sfunc = ag_catalog.age_array_agg_map_property_transfn,
    finalfunc = ag_catalog.age_array_agg_property_finalfn,
    parallel = safe
);

CREATE AGGREGATE ag_catalog.age_array_agg_list_property(agtype, agtype[])
(
    stype = internal,
    sfunc = ag_catalog.age_array_agg_list_property_transfn,
    finalfunc = ag_catalog.age_array_agg_property_finalfn,
    parallel = safe
);

CREATE FUNCTION ag_catalog.age_vle(IN agtype, IN agtype, IN agtype, IN agtype,
                                   IN agtype, IN agtype, IN agtype, IN agtype,
                                   IN agtype, OUT edges agtype)
    RETURNS SETOF agtype
LANGUAGE C
STABLE
CALLED ON NULL INPUT
PARALLEL UNSAFE
ROWS 100
AS 'MODULE_PATHNAME';

-- VLE cache invalidation trigger function
-- Installed on graph label tables to catch SQL-level mutations
-- and increment the per-graph version counter for VLE cache invalidation.
--
CREATE FUNCTION ag_catalog.age_invalidate_graph_cache()
    RETURNS trigger
    LANGUAGE c
AS 'MODULE_PATHNAME';

--
-- Install the cache invalidation trigger on all pre-existing label tables.
-- New label tables created after this upgrade will get the trigger
-- automatically via label_commands.c. This DO block handles tables
-- that were created before the upgrade.
--
DO $$
DECLARE
    r RECORD;
BEGIN
    FOR r IN
        SELECT n.nspname AS schema_name, c.relname AS table_name
        FROM ag_catalog.ag_label l
        JOIN pg_catalog.pg_class c ON c.oid = l.relation
        JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace
        WHERE l.name != '_ag_label_vertex'
          AND l.name != '_ag_label_edge'
    LOOP
        -- Skip if trigger already exists on this table
        IF NOT EXISTS (
            SELECT 1 FROM pg_catalog.pg_trigger t
            JOIN pg_catalog.pg_class c ON c.oid = t.tgrelid
            JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace
            WHERE n.nspname = r.schema_name
              AND c.relname = r.table_name
              AND t.tgname = '_age_cache_invalidate'
        )
        THEN
            EXECUTE format(
                'CREATE TRIGGER _age_cache_invalidate '
                'AFTER INSERT OR UPDATE OR DELETE OR TRUNCATE '
                'ON %I.%I '
                'FOR EACH STATEMENT '
                'EXECUTE FUNCTION ag_catalog.age_invalidate_graph_cache()',
                r.schema_name, r.table_name
            );
        END IF;
    END LOOP;
END;
$$;

--
-- agtype <-> jsonb bidirectional casts
--

-- agtype -> jsonb (explicit)
-- Uses json intermediate (agtype_to_json -> json::jsonb) because agtype
-- extends jsonb's binary format with types (AGTV_INTEGER, AGTV_FLOAT,
-- AGTV_VERTEX, AGTV_EDGE, AGTV_PATH) that jsonb does not recognize.
CREATE FUNCTION ag_catalog.agtype_to_jsonb(agtype)
    RETURNS jsonb
    LANGUAGE sql
    IMMUTABLE
RETURNS NULL ON NULL INPUT
PARALLEL SAFE
AS 'SELECT ag_catalog.agtype_to_json($1)::jsonb';

CREATE CAST (agtype AS jsonb)
    WITH FUNCTION ag_catalog.agtype_to_jsonb(agtype);

-- jsonb -> agtype (explicit)
CREATE FUNCTION ag_catalog.jsonb_to_agtype(jsonb)
    RETURNS agtype
    LANGUAGE sql
    IMMUTABLE
RETURNS NULL ON NULL INPUT
PARALLEL SAFE
AS 'SELECT $1::text::agtype';

CREATE CAST (jsonb AS agtype)
    WITH FUNCTION ag_catalog.jsonb_to_agtype(jsonb);

--
-- AGE adjacency index access method.
--
CREATE FUNCTION ag_catalog.age_adjacency_handler(internal)
    RETURNS index_am_handler
    LANGUAGE c
AS 'MODULE_PATHNAME';

CREATE ACCESS METHOD age_adjacency TYPE INDEX
HANDLER ag_catalog.age_adjacency_handler;

COMMENT ON ACCESS METHOD age_adjacency IS
'AGE adjacency index access method';

CREATE OPERATOR CLASS graphid_age_adjacency_ops
DEFAULT FOR TYPE graphid USING age_adjacency AS
  OPERATOR 1 =;

CREATE FUNCTION ag_catalog.age_adjacency_debug_payload(index_oid regclass,
                                                       key graphid)
    RETURNS TABLE(heap_tid tid, edge_id graphid, next_vertex_id graphid)
    LANGUAGE c
    STABLE
    STRICT
AS 'MODULE_PATHNAME';

CREATE FUNCTION ag_catalog.age_adjacency_candidate_edges(index_oid regclass,
                                                         key graphid)
    RETURNS TABLE(edge_id graphid, next_vertex_id graphid)
    LANGUAGE c
    STABLE
    STRICT
AS 'MODULE_PATHNAME';

COMMENT ON FUNCTION ag_catalog.age_adjacency_candidate_edges(regclass, graphid)
IS 'Internal age_adjacency candidate provider for bound-endpoint adjacency scans; used only by opt-in experimental planner/parser paths.';

CREATE FUNCTION ag_catalog.age_adjacency_candidate_edge_rows(index_oid regclass,
                                                             key graphid,
                                                             outgoing boolean)
    RETURNS TABLE(id graphid, start_id graphid, end_id graphid,
                  properties agtype)
    LANGUAGE c
    STABLE
    STRICT
AS 'MODULE_PATHNAME';

COMMENT ON FUNCTION ag_catalog.age_adjacency_candidate_edge_rows(regclass, graphid, boolean)
IS 'Internal age_adjacency fixed-MATCH candidate provider; only considered when age.enable_adjacency_match is on and guarded one-hop/index/bound-endpoint conditions hold.';

CREATE FUNCTION ag_catalog.age_adjacency_debug_stats(index_oid regclass)
    RETURNS TABLE(num_pages bigint, postings bigint,
                  directory_entries bigint, delta_postings bigint,
                  delta_reindex_threshold bigint,
                  delta_reindex_recommended boolean)
    LANGUAGE c
    STABLE
    STRICT
AS 'MODULE_PATHNAME';

CREATE FUNCTION ag_catalog.age_adjacency_debug_directory_probe(index_oid regclass,
                                                               key graphid)
    RETURNS TABLE(found boolean, directory_pages_visited bigint,
                  directory_entries_scanned bigint)
    LANGUAGE c
    STABLE
    STRICT
AS 'MODULE_PATHNAME';
