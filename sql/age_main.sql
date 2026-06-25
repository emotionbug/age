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

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION age" to load this file. \quit

--
-- catalog tables
--

CREATE TABLE ag_graph (
                          graphid oid NOT NULL,
                          name name NOT NULL,
                          namespace regnamespace NOT NULL
);

CREATE UNIQUE INDEX ag_graph_graphid_index ON ag_graph USING btree (graphid);

-- include content of the ag_graph table into the pg_dump output
SELECT pg_catalog.pg_extension_config_dump('ag_graph', '');

CREATE UNIQUE INDEX ag_graph_name_index ON ag_graph USING btree (name);

CREATE UNIQUE INDEX ag_graph_namespace_index
    ON ag_graph
    USING btree (namespace);

-- 0 is an invalid label ID
CREATE DOMAIN label_id AS int NOT NULL CHECK (VALUE > 0 AND VALUE <= 65535);

CREATE DOMAIN label_kind AS "char" NOT NULL CHECK (VALUE = 'v' OR VALUE = 'e');

CREATE TABLE ag_label (
                          name name NOT NULL,
                          graph oid NOT NULL,
                          id label_id,
                          kind label_kind,
                          relation regclass NOT NULL,
                          seq_name name NOT NULL,
                          CONSTRAINT fk_graph_oid
                              FOREIGN KEY(graph)
                                  REFERENCES ag_graph(graphid)
);

-- include content of the ag_label table into the pg_dump output
SELECT pg_catalog.pg_extension_config_dump('ag_label', '');

CREATE UNIQUE INDEX ag_label_name_graph_index
    ON ag_label
    USING btree (name, graph);

CREATE UNIQUE INDEX ag_label_graph_oid_index
    ON ag_label
    USING btree (graph, id);

CREATE UNIQUE INDEX ag_label_relation_index ON ag_label USING btree (relation);

CREATE UNIQUE INDEX ag_label_seq_name_graph_index
    ON ag_label
    USING btree (seq_name, graph);

--
-- Graph cardinality-estimation statistics cache.  Holds the
-- (src_label, edge_label, dst_label) edge-count triples for a graph, refreshed
-- explicitly via ag_catalog.age_refresh_graph_edge_label_stats().  This is the
-- persistent, plan-time-cheap form of age_graph_edge_label_stats() that the
-- graph-source cost model will condition fanout on (a full edge scan is too
-- expensive to run per plan).  Marked dumpable extension config like
-- ag_graph/ag_label.
--
-- NB: deliberately NO foreign key to ag_graph.  An FK to ag_graph fires a
-- referential-integrity check on every insert, which plans a probe query and
-- re-enters AGE's graph-join planner hook in a nested context where its
-- lowering-artifact state is unset, crashing the backend.  This is derived,
-- refreshable data, so a stale row after a graph drop is harmless and far
-- preferable to that crash.
--
CREATE TABLE ag_graph_edge_stats (
                          graph oid NOT NULL,
                          src_label name NOT NULL,
                          edge_label name NOT NULL,
                          dst_label name NOT NULL,
                          edge_count bigint NOT NULL
);
SELECT pg_catalog.pg_extension_config_dump('ag_graph_edge_stats', '');
CREATE UNIQUE INDEX ag_graph_edge_stats_triple_index
    ON ag_graph_edge_stats
    USING btree (graph, src_label, edge_label, dst_label);

--
-- Per (src_label, edge_label) out-degree cache: the plan-time-cheap form of
-- age_graph_edge_degree_stats(), refreshed via
-- ag_catalog.age_refresh_graph_edge_degree_stats().  The graph-source cost
-- model conditions the adjacency base (endpoint) fanout on avg_out_degree --
-- a per-source-label mean degree, more accurate than the global
-- reltuples/distinct estimate.  No FK to ag_graph (same reason as
-- ag_graph_edge_stats: the RI check re-enters the graph-join planner hook).
--
CREATE TABLE ag_graph_edge_degree_cache (
                          graph oid NOT NULL,
                          src_label name NOT NULL,
                          edge_label name NOT NULL,
                          max_out_degree bigint NOT NULL,
                          avg_out_degree double precision NOT NULL,
                          source_vertex_count bigint NOT NULL
);
SELECT pg_catalog.pg_extension_config_dump('ag_graph_edge_degree_cache', '');
CREATE UNIQUE INDEX ag_graph_edge_degree_cache_index
    ON ag_graph_edge_degree_cache
    USING btree (graph, src_label, edge_label);

--
-- Per (dst_label, edge_label) in-degree cache: the terminal-side mirror of
-- ag_graph_edge_degree_cache, used to condition the base fanout of *incoming*
-- (reverse) adjacency expansions on the terminal vertex's average in-degree.
--
CREATE TABLE ag_graph_edge_in_degree_cache (
                          graph oid NOT NULL,
                          dst_label name NOT NULL,
                          edge_label name NOT NULL,
                          max_in_degree bigint NOT NULL,
                          avg_in_degree double precision NOT NULL,
                          terminal_vertex_count bigint NOT NULL
);
SELECT pg_catalog.pg_extension_config_dump('ag_graph_edge_in_degree_cache', '');
CREATE UNIQUE INDEX ag_graph_edge_in_degree_cache_index
    ON ag_graph_edge_in_degree_cache
    USING btree (graph, dst_label, edge_label);

--
-- catalog lookup functions
--

CREATE FUNCTION ag_catalog._label_id(graph_name name, label_name name)
    RETURNS label_id
    LANGUAGE c
    STABLE
PARALLEL SAFE
AS 'MODULE_PATHNAME';

--
-- utility functions
--

CREATE FUNCTION ag_catalog.create_graph(graph_name name)
    RETURNS void
    LANGUAGE c
    AS 'MODULE_PATHNAME';

CREATE FUNCTION ag_catalog.drop_graph(graph_name name, cascade boolean = false)
    RETURNS void
    LANGUAGE c
    AS 'MODULE_PATHNAME';

CREATE FUNCTION ag_catalog.create_vlabel(graph_name cstring, label_name cstring)
    RETURNS void
    LANGUAGE c
    AS 'MODULE_PATHNAME';

CREATE FUNCTION ag_catalog.create_elabel(graph_name cstring, label_name cstring)
    RETURNS void
    LANGUAGE c
    AS 'MODULE_PATHNAME';

CREATE TABLE ag_catalog.ag_graph_index (
    graph_oid oid NOT NULL,
    graph_name name NOT NULL,
    label_name name NOT NULL,
    label_kind "char" NOT NULL,
    index_name name NOT NULL,
    index_oid oid NOT NULL,
    index_kind text NOT NULL,
    direction text,
    property_names name[] NOT NULL DEFAULT ARRAY[]::name[],
    provider text NOT NULL,
    options jsonb NOT NULL DEFAULT '{}'::jsonb,
    created_at timestamptz NOT NULL DEFAULT now()
);

CREATE UNIQUE INDEX ag_graph_index_graph_index_name_idx
ON ag_catalog.ag_graph_index (graph_oid, index_name);

CREATE FUNCTION ag_catalog.show_indexes()
    RETURNS TABLE(name name, type text, entity_type text,
                  labels_or_types name[], properties name[], state text,
                  provider text, options jsonb)
    LANGUAGE sql
    STABLE
AS $$
    SELECT gi.index_name,
           gi.index_kind,
           CASE gi.label_kind WHEN 'v' THEN 'NODE' ELSE 'RELATIONSHIP' END,
           ARRAY[gi.label_name]::name[],
           gi.property_names,
           CASE WHEN c.oid IS NULL THEN 'DROPPED' ELSE 'ONLINE' END,
           gi.provider,
           gi.options
    FROM ag_catalog.ag_graph_index gi
    LEFT JOIN pg_catalog.pg_class c ON c.oid = gi.index_oid
    ORDER BY gi.graph_name, gi.label_name, gi.index_name
$$;

CREATE FUNCTION ag_catalog.show_indexes(graph_name name)
    RETURNS TABLE(name name, type text, entity_type text,
                  labels_or_types name[], properties name[], state text,
                  provider text, options jsonb)
    LANGUAGE sql
    STABLE
AS $$
    SELECT gi.index_name,
           gi.index_kind,
           CASE gi.label_kind WHEN 'v' THEN 'NODE' ELSE 'RELATIONSHIP' END,
           ARRAY[gi.label_name]::name[],
           gi.property_names,
           CASE WHEN c.oid IS NULL THEN 'DROPPED' ELSE 'ONLINE' END,
           gi.provider,
           gi.options
    FROM ag_catalog.ag_graph_index gi
    LEFT JOIN pg_catalog.pg_class c ON c.oid = gi.index_oid
    WHERE gi.graph_name = $1
    ORDER BY gi.label_name, gi.index_name
$$;

CREATE FUNCTION ag_catalog.create_property_index(graph_name cstring,
                                                 label_name cstring,
                                                 property_name cstring)
    RETURNS void
    LANGUAGE c
    AS 'MODULE_PATHNAME';

CREATE FUNCTION ag_catalog.create_property_index(graph_name cstring,
                                                 label_name cstring,
                                                 property_name cstring,
                                                 property_type cstring)
    RETURNS void
    LANGUAGE c
    AS 'MODULE_PATHNAME';

CREATE FUNCTION ag_catalog.create_property_source_index(graph_name cstring,
                                                        label_name cstring,
                                                        property_name cstring)
    RETURNS void
    LANGUAGE c
    AS 'MODULE_PATHNAME';

CREATE FUNCTION ag_catalog.create_property_source_index(graph_name cstring,
                                                        label_name cstring,
                                                        property_name cstring,
                                                        property_type cstring)
    RETURNS void
    LANGUAGE c
    AS 'MODULE_PATHNAME';

CREATE FUNCTION ag_catalog.create_property_source_index_named(graph_name cstring,
                                                              label_name cstring,
                                                              property_name cstring,
                                                              index_name cstring)
    RETURNS text
    LANGUAGE c
    AS 'MODULE_PATHNAME';

CREATE FUNCTION ag_catalog.create_property_source_index_named(graph_name cstring,
                                                              label_name cstring,
                                                              property_name cstring,
                                                              index_name cstring,
                                                              property_type cstring)
    RETURNS text
    LANGUAGE c
    AS 'MODULE_PATHNAME';

CREATE FUNCTION ag_catalog.create_adjacency_index(graph_name cstring,
                                                  edge_label_name cstring,
                                                  direction cstring)
    RETURNS void
    LANGUAGE c
    AS 'MODULE_PATHNAME';

CREATE FUNCTION ag_catalog.create_adjacency_indexes(graph_name cstring,
                                                    edge_label_name cstring)
    RETURNS void
    LANGUAGE c
    AS 'MODULE_PATHNAME';

CREATE FUNCTION ag_catalog.create_adjacency_index_named(graph_name cstring,
                                                        edge_label_name cstring,
                                                        direction cstring,
                                                        index_name cstring)
    RETURNS text
    LANGUAGE c
    AS 'MODULE_PATHNAME';

CREATE FUNCTION ag_catalog.create_adjacency_indexes_named(graph_name cstring,
                                                         edge_label_name cstring,
                                                         index_name cstring)
    RETURNS text
    LANGUAGE c
    AS 'MODULE_PATHNAME';

CREATE FUNCTION ag_catalog.drop_graph_index(graph_name cstring,
                                            index_name cstring)
    RETURNS text
    LANGUAGE c
    AS 'MODULE_PATHNAME';

COMMENT ON FUNCTION ag_catalog.create_property_source_index(cstring, cstring, cstring)
IS 'Create a property source index for graph pattern candidate lookup.';

COMMENT ON FUNCTION ag_catalog.create_property_source_index(cstring, cstring, cstring, cstring)
IS 'Create a typed property source index for graph pattern candidate lookup.';

COMMENT ON FUNCTION ag_catalog.create_adjacency_index(cstring, cstring, cstring)
IS 'Create one directional age_adjacency source index for an edge label.';

COMMENT ON FUNCTION ag_catalog.create_adjacency_indexes(cstring, cstring)
IS 'Create outgoing and incoming age_adjacency source indexes for an edge label.';

COMMENT ON FUNCTION ag_catalog.drop_graph_index(cstring, cstring)
IS 'Drop a helper-managed graph index and remove its metadata.';

CREATE FUNCTION ag_catalog.alter_graph(graph_name name, operation cstring,
                                       new_value name)
    RETURNS void
    LANGUAGE c
    AS 'MODULE_PATHNAME';

CREATE FUNCTION ag_catalog.drop_label(graph_name name, label_name name,
                                      force boolean = false)
    RETURNS void
    LANGUAGE c
    AS 'MODULE_PATHNAME';

--
-- If `load_as_agtype` is true, property values are loaded as agtype; otherwise
-- loaded as string.
--
CREATE FUNCTION ag_catalog.load_labels_from_file(graph_name name,
                                                 label_name name,
                                                 file_path text,
                                                 id_field_exists bool default true,
                                                 load_as_agtype bool default false)
    RETURNS void
    LANGUAGE c
    AS 'MODULE_PATHNAME';

CREATE FUNCTION ag_catalog.load_edges_from_file(graph_name name,
                                                label_name name,
                                                file_path text,
                                                load_as_agtype bool default false)
    RETURNS void
    LANGUAGE c
    AS 'MODULE_PATHNAME';

--
-- graphid type
--

-- define graphid as a shell type first
CREATE TYPE graphid;

CREATE FUNCTION ag_catalog.graphid_in(cstring)
    RETURNS graphid
    LANGUAGE c
    IMMUTABLE
RETURNS NULL ON NULL INPUT
PARALLEL SAFE
AS 'MODULE_PATHNAME';

CREATE FUNCTION ag_catalog.graphid_out(graphid)
    RETURNS cstring
    LANGUAGE c
    IMMUTABLE
RETURNS NULL ON NULL INPUT
PARALLEL SAFE
AS 'MODULE_PATHNAME';

-- binary I/O functions
CREATE FUNCTION ag_catalog.graphid_send(graphid)
    RETURNS bytea
    LANGUAGE c
    IMMUTABLE
RETURNS NULL ON NULL INPUT
PARALLEL SAFE
AS 'MODULE_PATHNAME';

CREATE FUNCTION ag_catalog.graphid_recv(internal)
    RETURNS graphid
    LANGUAGE c
    IMMUTABLE
RETURNS NULL ON NULL INPUT
PARALLEL SAFE
AS 'MODULE_PATHNAME';

CREATE TYPE graphid (
  INPUT = ag_catalog.graphid_in,
  OUTPUT = ag_catalog.graphid_out,
  SEND = ag_catalog.graphid_send,
  RECEIVE = ag_catalog.graphid_recv,
  INTERNALLENGTH = 8,
  PASSEDBYVALUE,
  ALIGNMENT = float8,
  STORAGE = plain
);

--
-- graphid - comparison operators (=, <>, <, >, <=, >=)
--

CREATE FUNCTION ag_catalog.graphid_eq(graphid, graphid)
    RETURNS boolean
    LANGUAGE c
    IMMUTABLE
RETURNS NULL ON NULL INPUT
PARALLEL SAFE
AS 'MODULE_PATHNAME';

CREATE OPERATOR = (
  FUNCTION = ag_catalog.graphid_eq,
  LEFTARG = graphid,
  RIGHTARG = graphid,
  COMMUTATOR = =,
  NEGATOR = <>,
  RESTRICT = eqsel,
  JOIN = eqjoinsel,
  HASHES,
  MERGES
);

CREATE FUNCTION ag_catalog.graphid_ne(graphid, graphid)
    RETURNS boolean
    LANGUAGE c
    IMMUTABLE
RETURNS NULL ON NULL INPUT
PARALLEL SAFE
AS 'MODULE_PATHNAME';

CREATE OPERATOR <> (
  FUNCTION = ag_catalog.graphid_ne,
  LEFTARG = graphid,
  RIGHTARG = graphid,
  COMMUTATOR = <>,
  NEGATOR = =,
  RESTRICT = neqsel,
  JOIN = neqjoinsel
);

CREATE FUNCTION ag_catalog.graphid_lt(graphid, graphid)
    RETURNS boolean
    LANGUAGE c
    IMMUTABLE
RETURNS NULL ON NULL INPUT
PARALLEL SAFE
AS 'MODULE_PATHNAME';

CREATE OPERATOR < (
  FUNCTION = ag_catalog.graphid_lt,
  LEFTARG = graphid,
  RIGHTARG = graphid,
  COMMUTATOR = >,
  NEGATOR = >=,
  RESTRICT = scalarltsel,
  JOIN = scalarltjoinsel
);

CREATE FUNCTION ag_catalog.graphid_gt(graphid, graphid)
    RETURNS boolean
    LANGUAGE c
    IMMUTABLE
RETURNS NULL ON NULL INPUT
PARALLEL SAFE
AS 'MODULE_PATHNAME';

CREATE OPERATOR > (
  FUNCTION = ag_catalog.graphid_gt,
  LEFTARG = graphid,
  RIGHTARG = graphid,
  COMMUTATOR = <,
  NEGATOR = <=,
  RESTRICT = scalargtsel,
  JOIN = scalargtjoinsel
);

CREATE FUNCTION ag_catalog.graphid_le(graphid, graphid)
    RETURNS boolean
    LANGUAGE c
    IMMUTABLE
RETURNS NULL ON NULL INPUT
PARALLEL SAFE
AS 'MODULE_PATHNAME';

CREATE OPERATOR <= (
  FUNCTION = ag_catalog.graphid_le,
  LEFTARG = graphid,
  RIGHTARG = graphid,
  COMMUTATOR = >=,
  NEGATOR = >,
  RESTRICT = scalarlesel,
  JOIN = scalarlejoinsel
);

CREATE FUNCTION ag_catalog.graphid_ge(graphid, graphid)
    RETURNS boolean
    LANGUAGE c
    IMMUTABLE
RETURNS NULL ON NULL INPUT
PARALLEL SAFE
AS 'MODULE_PATHNAME';

CREATE OPERATOR >= (
  FUNCTION = ag_catalog.graphid_ge,
  LEFTARG = graphid,
  RIGHTARG = graphid,
  COMMUTATOR = <=,
  NEGATOR = <,
  RESTRICT = scalargesel,
  JOIN = scalargejoinsel
);

--
-- graphid - B-tree support functions
--

-- comparison support
CREATE FUNCTION ag_catalog.graphid_btree_cmp(graphid, graphid)
    RETURNS int
    LANGUAGE c
    IMMUTABLE
RETURNS NULL ON NULL INPUT
PARALLEL SAFE
AS 'MODULE_PATHNAME';

-- sort support
CREATE FUNCTION ag_catalog.graphid_btree_sort(internal)
    RETURNS void
    LANGUAGE c
    IMMUTABLE
RETURNS NULL ON NULL INPUT
PARALLEL SAFE
AS 'MODULE_PATHNAME';

--
-- define operator classes for graphid
--

-- B-tree strategies
--   1: less than
--   2: less than or equal
--   3: equal
--   4: greater than or equal
--   5: greater than
--
-- B-tree support functions
--   1: compare two keys and return an integer less than zero, zero, or greater
--      than zero, indicating whether the first key is less than, equal to, or
--      greater than the second
--   2: return the addresses of C-callable sort support function(s) (optional)
--   3: compare a test value to a base value plus/minus an offset, and return
--      true or false according to the comparison result (optional)
CREATE OPERATOR CLASS graphid_ops DEFAULT FOR TYPE graphid USING btree AS
  OPERATOR 1 <,
  OPERATOR 2 <=,
  OPERATOR 3 =,
  OPERATOR 4 >=,
  OPERATOR 5 >,
  FUNCTION 1 ag_catalog.graphid_btree_cmp (graphid, graphid),
  FUNCTION 2 ag_catalog.graphid_btree_sort (internal);

--
-- AGE adjacency index access method
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

CREATE FUNCTION ag_catalog.age_adjacency_debug_stats(index_oid regclass)
    RETURNS TABLE(index_version int, num_pages bigint, postings bigint,
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

CREATE FUNCTION ag_catalog.age_adjacency_debug_key_known_empty(index_oid regclass,
                                                               key graphid,
                                                               terminal_label_id int)
    RETURNS boolean
    LANGUAGE c
    STABLE
    STRICT
AS 'MODULE_PATHNAME';

CREATE FUNCTION ag_catalog.age_adjacency_debug_key_known_empty_range(index_oid regclass,
                                                                     key graphid,
                                                                     terminal_label_id int,
                                                                     min_vertex_id graphid,
                                                                     max_vertex_id graphid)
    RETURNS boolean
    LANGUAGE c
    STABLE
    STRICT
AS 'MODULE_PATHNAME';

CREATE FUNCTION ag_catalog.age_adjacency_debug_composite_probe(index_oid regclass,
                                                               key graphid,
                                                               terminal_label_id int,
                                                               matched_vertex_id graphid,
                                                               property_match_count bigint)
    RETURNS TABLE(emitted bigint, cache_filtered bigint,
                  cache_property bigint, set_range_filter bigint,
                  set_sorted_filter bigint, set_block_filter bigint,
                  set_block_value_filter bigint,
                  set_block_value_posting_filter bigint,
                  set_directory_filter bigint, composite_block_filter bigint,
                  composite_directory_filter bigint,
                  composite_directory_estimate bigint,
                  set_block_range_filter bigint,
                  set_block_exact_filter bigint,
                  set_block_compressed_filter bigint,
                  set_block_bloom_filter bigint,
                  set_block_posting_filter bigint,
                  set_directory_range_filter bigint,
                  set_directory_exact_filter bigint,
                  set_directory_label_bloom_filter bigint,
                  set_directory_compressed_filter bigint,
                  set_directory_wide_bloom_filter bigint,
                  set_directory_value_filter bigint,
                  set_directory_value_posting_filter bigint)
    LANGUAGE c
    STABLE
    STRICT
AS 'MODULE_PATHNAME';

CREATE FUNCTION ag_catalog.age_adjacency_debug_main_probe(index_oid regclass,
                                                          key graphid)
    RETURNS TABLE(found boolean, main_pages_visited bigint,
                  main_window_offsets bigint, main_page_offsets bigint,
                  main_block_items bigint,
                  main_compact_block_items bigint,
                  main_full_block_items bigint,
                  main_entries_cached bigint,
                  main_label_groups bigint)
    LANGUAGE c
    STABLE
    STRICT
AS 'MODULE_PATHNAME';

CREATE FUNCTION ag_catalog.age_adjacency_debug_delta_probe(index_oid regclass,
                                                           key graphid)
    RETURNS TABLE(delta_pages_visited bigint,
                  delta_pages_skipped bigint,
                  delta_entries_scanned bigint)
    LANGUAGE c
    STABLE
    STRICT
AS 'MODULE_PATHNAME';

CREATE FUNCTION ag_catalog.age_adjacency_debug_delta_maintenance(index_oid regclass)
    RETURNS TABLE(action text, reason text,
                  delta_postings bigint, delta_pages bigint,
                  delta_tuples_per_page bigint,
                  delta_reindex_threshold bigint,
                  delta_reindex_recommended boolean)
    LANGUAGE c
    STABLE
    STRICT
AS 'MODULE_PATHNAME';

CREATE FUNCTION ag_catalog.age_adjacency_reindex_if_needed(index_oid regclass)
    RETURNS TABLE(reindexed boolean, action text, reason text,
                  before_delta_postings bigint,
                  after_delta_postings bigint,
                  after_postings bigint)
    LANGUAGE c
    VOLATILE
    STRICT
AS 'MODULE_PATHNAME';

--
-- graphid functions
--

CREATE FUNCTION ag_catalog._graphid(label_id int, entry_id bigint)
    RETURNS graphid
    LANGUAGE c
    IMMUTABLE
PARALLEL SAFE
AS 'MODULE_PATHNAME';

CREATE FUNCTION ag_catalog._label_name(graph_oid oid, graphid)
    RETURNS cstring
    LANGUAGE c
    IMMUTABLE
PARALLEL SAFE
AS 'MODULE_PATHNAME';

CREATE FUNCTION ag_catalog._extract_label_id(graphid)
    RETURNS label_id
    LANGUAGE c
    STABLE
PARALLEL SAFE
AS 'MODULE_PATHNAME';

--
-- Graph cardinality-estimation statistics: the (src_label, edge_label,
-- dst_label) edge-count triples for a graph.  This is the source-selectivity
-- authority the graph-source cost model lacks today -- fanout estimation keys
-- only on an edge label's total reltuples, so it cannot tell that, say,
-- ':KNOWS' edges run mostly between ':Person' vertices rather than to
-- ':Company'.  Exposed as a refresh primitive (computed by scanning the child
-- edge tables in C) intended to back an ANALYZE-time stats cache and
-- triple/label conditioning in the graph-source cost model.
--
CREATE FUNCTION ag_catalog.age_graph_edge_label_stats(graph_name name)
    RETURNS TABLE(src_label name, edge_label name, dst_label name,
                  edge_count bigint)
    LANGUAGE c
    STABLE
AS 'MODULE_PATHNAME';

--
-- Per (src_label, edge_label) out-degree distribution: the maximum and average
-- number of edges leaving a single source vertex, with the source-vertex count.
-- Surfaces hub skew (a power-law degree tail) that the mean degree -- already
-- used by the cost model -- hides.
--
CREATE FUNCTION ag_catalog.age_graph_edge_degree_stats(graph_name name)
    RETURNS TABLE(src_label name, edge_label name, max_out_degree bigint,
                  avg_out_degree double precision, source_vertex_count bigint)
    LANGUAGE c
    STABLE
AS 'MODULE_PATHNAME';

--
-- Per (dst_label, edge_label) in-degree distribution: the mirror of
-- age_graph_edge_degree_stats(), counting edges arriving at each terminal
-- vertex.  Supplies the reverse-direction fanout for expanding a pattern from
-- its terminal side.
--
CREATE FUNCTION ag_catalog.age_graph_edge_in_degree_stats(graph_name name)
    RETURNS TABLE(dst_label name, edge_label name, max_in_degree bigint,
                  avg_in_degree double precision, terminal_vertex_count bigint)
    LANGUAGE c
    STABLE
AS 'MODULE_PATHNAME';

--
-- Refresh the graph cardinality-estimation statistics cache for one graph:
-- replace its rows in ag_graph_edge_stats with the current triple counts from
-- age_graph_edge_label_stats().  Returns the number of triples cached.
--
CREATE FUNCTION ag_catalog.age_refresh_graph_edge_label_stats(graph_name name)
    RETURNS bigint
    LANGUAGE plpgsql
AS $age_refresh_graph_edge_label_stats$
DECLARE
    graph_oid oid;
    triple_count bigint;
BEGIN
    SELECT g.graphid INTO graph_oid
    FROM ag_catalog.ag_graph g
    WHERE g.name = graph_name;

    IF graph_oid IS NULL THEN
        RAISE EXCEPTION 'graph "%" does not exist', graph_name
            USING ERRCODE = 'invalid_parameter_value';
    END IF;

    DELETE FROM ag_catalog.ag_graph_edge_stats WHERE graph = graph_oid;

    INSERT INTO ag_catalog.ag_graph_edge_stats
                (graph, src_label, edge_label, dst_label, edge_count)
    SELECT graph_oid, s.src_label, s.edge_label, s.dst_label, s.edge_count
    FROM ag_catalog.age_graph_edge_label_stats(graph_name) s;

    GET DIAGNOSTICS triple_count = ROW_COUNT;
    RETURN triple_count;
END
$age_refresh_graph_edge_label_stats$;

--
-- Refresh the per (src_label, edge_label) out-degree cache for one graph from
-- age_graph_edge_degree_stats().  Returns the number of rows cached.
--
CREATE FUNCTION ag_catalog.age_refresh_graph_edge_degree_stats(graph_name name)
    RETURNS bigint
    LANGUAGE plpgsql
AS $age_refresh_graph_edge_degree_stats$
DECLARE
    graph_oid oid;
    row_count bigint;
BEGIN
    SELECT g.graphid INTO graph_oid
    FROM ag_catalog.ag_graph g
    WHERE g.name = graph_name;

    IF graph_oid IS NULL THEN
        RAISE EXCEPTION 'graph "%" does not exist', graph_name
            USING ERRCODE = 'invalid_parameter_value';
    END IF;

    DELETE FROM ag_catalog.ag_graph_edge_degree_cache WHERE graph = graph_oid;

    INSERT INTO ag_catalog.ag_graph_edge_degree_cache
                (graph, src_label, edge_label, max_out_degree,
                 avg_out_degree, source_vertex_count)
    SELECT graph_oid, s.src_label, s.edge_label, s.max_out_degree,
           s.avg_out_degree, s.source_vertex_count
    FROM ag_catalog.age_graph_edge_degree_stats(graph_name) s;

    GET DIAGNOSTICS row_count = ROW_COUNT;
    RETURN row_count;
END
$age_refresh_graph_edge_degree_stats$;

--
-- Refresh the per (dst_label, edge_label) in-degree cache for one graph from
-- age_graph_edge_in_degree_stats().  Returns the number of rows cached.
--
CREATE FUNCTION ag_catalog.age_refresh_graph_edge_in_degree_stats(graph_name name)
    RETURNS bigint
    LANGUAGE plpgsql
AS $age_refresh_graph_edge_in_degree_stats$
DECLARE
    graph_oid oid;
    row_count bigint;
BEGIN
    SELECT g.graphid INTO graph_oid
    FROM ag_catalog.ag_graph g
    WHERE g.name = graph_name;

    IF graph_oid IS NULL THEN
        RAISE EXCEPTION 'graph "%" does not exist', graph_name
            USING ERRCODE = 'invalid_parameter_value';
    END IF;

    DELETE FROM ag_catalog.ag_graph_edge_in_degree_cache WHERE graph = graph_oid;

    INSERT INTO ag_catalog.ag_graph_edge_in_degree_cache
                (graph, dst_label, edge_label, max_in_degree,
                 avg_in_degree, terminal_vertex_count)
    SELECT graph_oid, s.dst_label, s.edge_label, s.max_in_degree,
           s.avg_in_degree, s.terminal_vertex_count
    FROM ag_catalog.age_graph_edge_in_degree_stats(graph_name) s;

    GET DIAGNOSTICS row_count = ROW_COUNT;
    RETURN row_count;
END
$age_refresh_graph_edge_in_degree_stats$;

--
-- VLE cache invalidation trigger function.
-- Installed on graph label tables to catch SQL-level mutations
-- (INSERT/UPDATE/DELETE/TRUNCATE) and increment the graph's
-- version counter so VLE caches are properly invalidated.
--
CREATE FUNCTION ag_catalog.age_invalidate_graph_cache()
    RETURNS trigger
    LANGUAGE c
AS 'MODULE_PATHNAME';
