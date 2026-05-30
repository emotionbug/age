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

LOAD 'age';
SET search_path TO ag_catalog;

COPY (
SELECT amname || ':' || amtype::text
FROM pg_am
WHERE amname = 'age_adjacency'
) TO STDOUT;

COPY (
SELECT opc.opcname || ':' || t.typname || ':' || am.amname || ':' ||
       opc.opcdefault
FROM pg_opclass opc
JOIN pg_am am ON am.oid = opc.opcmethod
JOIN pg_type t ON t.oid = opc.opcintype
WHERE am.amname = 'age_adjacency'
ORDER BY 1
) TO STDOUT;

CREATE TEMP TABLE age_adjacency_smoke
(
    id graphid NOT NULL,
    start_id graphid NOT NULL,
    end_id graphid NOT NULL
);

INSERT INTO age_adjacency_smoke VALUES
(_graphid(2, 1), _graphid(1, 1), _graphid(1, 2)),
(_graphid(2, 2), _graphid(1, 1), _graphid(1, 3)),
(_graphid(2, 3), _graphid(1, 4), _graphid(1, 5));

CREATE INDEX age_adjacency_smoke_bad_idx
ON age_adjacency_smoke USING age_adjacency (start_id);

CREATE INDEX age_adjacency_smoke_start_idx
ON age_adjacency_smoke USING age_adjacency (start_id, id, end_id);

INSERT INTO age_adjacency_smoke VALUES
(_graphid(2, 4), _graphid(1, 1), _graphid(1, 6));

SET enable_seqscan = off;

COPY (
SELECT count(*)::text
FROM age_adjacency_smoke
WHERE start_id = _graphid(1, 1)
) TO STDOUT;

RESET enable_seqscan;

COPY (
SELECT edge_id::text || ':' || next_vertex_id::text
FROM age_adjacency_debug_payload('age_adjacency_smoke_start_idx'::regclass,
                                 _graphid(1, 1))
ORDER BY edge_id
) TO STDOUT;

DELETE FROM age_adjacency_smoke
WHERE id = _graphid(2, 2);

COPY (
SELECT edge_id::text || ':' || next_vertex_id::text
FROM age_adjacency_debug_payload('age_adjacency_smoke_start_idx'::regclass,
                                 _graphid(1, 1))
ORDER BY edge_id
) TO STDOUT;

COPY (
SELECT postings::text
FROM age_adjacency_debug_stats('age_adjacency_smoke_start_idx'::regclass)
) TO STDOUT;

DO $$
BEGIN
    PERFORM create_graph('age_adj_vle');
    PERFORM create_vlabel('age_adj_vle', 'N');
    PERFORM create_elabel('age_adj_vle', 'R');
END
$$;

INSERT INTO age_adj_vle."N"(id, properties) VALUES
(_graphid(_label_id('age_adj_vle', 'N'), 1), '{"i": 0}'::agtype),
(_graphid(_label_id('age_adj_vle', 'N'), 2), '{"i": 1}'::agtype),
(_graphid(_label_id('age_adj_vle', 'N'), 3), '{"i": 2}'::agtype);

INSERT INTO age_adj_vle."R"(id, start_id, end_id, properties) VALUES
(_graphid(_label_id('age_adj_vle', 'R'), 1),
 _graphid(_label_id('age_adj_vle', 'N'), 1),
 _graphid(_label_id('age_adj_vle', 'N'), 2),
 '{"kind": "forward"}'::agtype),
(_graphid(_label_id('age_adj_vle', 'R'), 2),
 _graphid(_label_id('age_adj_vle', 'N'), 2),
 _graphid(_label_id('age_adj_vle', 'N'), 3),
 '{"kind": "forward"}'::agtype),
(_graphid(_label_id('age_adj_vle', 'R'), 3),
 _graphid(_label_id('age_adj_vle', 'N'), 2),
 _graphid(_label_id('age_adj_vle', 'N'), 2),
 '{"kind": "self"}'::agtype);

CREATE INDEX age_adj_vle_r_start_payload_idx
ON age_adj_vle."R" USING age_adjacency (start_id, id, end_id);

CREATE INDEX age_adj_vle_r_end_payload_idx
ON age_adj_vle."R" USING age_adjacency (end_id, id, start_id);

COPY (
SELECT count(*)::text
FROM cypher('age_adj_vle',
            $$MATCH p=(:N {i: 0})-[:R*1..2]->() RETURN p$$)
AS (p agtype)
) TO STDOUT;

COPY (
SELECT count(*)::text
FROM cypher('age_adj_vle',
            $$MATCH p=()-[:R*1..2]->(:N {i: 2}) RETURN p$$)
AS (p agtype)
) TO STDOUT;

COPY (
SELECT count(*)::text
FROM cypher('age_adj_vle',
            $$MATCH p=(:N {i: 1})-[:R*1..1]-() RETURN p$$)
AS (p agtype)
) TO STDOUT;

DO $$
BEGIN
    PERFORM drop_graph('age_adj_vle', true);
END
$$;

VACUUM age_adjacency_smoke;

COPY (
SELECT postings::text
FROM age_adjacency_debug_stats('age_adjacency_smoke_start_idx'::regclass)
) TO STDOUT;
