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

SELECT create_graph('cypher_vle');

-- Create a graph to test
SELECT * FROM cypher('cypher_vle', $$CREATE (b:begin)-[:edge {name: 'main edge', number: 1, dangerous: {type: "all", level: "all"}}]->(u1:middle)-[:edge {name: 'main edge', number: 2, dangerous: {type: "all", level: "all"}, packages: [2,4,6]}]->(u2:middle)-[:edge {name: 'main edge', number: 3, dangerous: {type: "all", level: "all"}}]->(u3:middle)-[:edge {name: 'main edge', number: 4, dangerous: {type: "all", level: "all"}}]->(e:end), (u1)-[:self_loop {name: 'self loop', number: 1, dangerous: {type: "all", level: "all"}}]->(u1), (e)-[:self_loop {name: 'self loop', number: 2, dangerous: {type: "all", level: "all"}}]->(e), (b)-[:alternate_edge {name: 'alternate edge', number: 1, packages: [2,4,6], dangerous: {type: "poisons", level: "all"}}]->(u1), (u2)-[:alternate_edge {name: 'alternate edge', number: 2, packages: [2,4,6], dangerous: {type: "poisons", level: "all"}}]->(u3), (u3)-[:alternate_edge {name: 'alternate edge', number: 3, packages: [2,4,6], dangerous: {type: "poisons", level: "all"}}]->(e), (u2)-[:bypass_edge {name: 'bypass edge', number: 1, packages: [1,3,5,7]}]->(e), (e)-[:alternate_edge {name: 'backup edge', number: 1, packages: [1,3,5,7]}]->(u3), (u3)-[:alternate_edge {name: 'backup edge', number: 2, packages: [1,3,5,7]}]->(u2), (u2)-[:bypass_edge {name: 'bypass edge', number: 2, packages: [1,3,5,7], dangerous: {type: "poisons", level: "all"}}]->(b) RETURN b, e $$) AS (b agtype, e agtype);

-- Test the VLE match integration
-- Each should find 400
SELECT * FROM cypher('cypher_vle', $$MATCH (u:begin)-[*]->(v:end) RETURN count(*) $$) AS (e agtype);
SELECT * FROM cypher('cypher_vle', $$MATCH (u:begin)-[*..]->(v:end) RETURN count(*) $$) AS (e agtype);
SELECT * FROM cypher('cypher_vle', $$MATCH (u:begin)-[*0..]->(v:end) RETURN count(*) $$) AS (e agtype);
SELECT * FROM cypher('cypher_vle', $$MATCH (u:begin)-[*1..]->(v:end) RETURN count(*) $$) AS (e agtype);
SELECT * FROM cypher('cypher_vle', $$MATCH (u:begin)-[*1..200]->(v:end) RETURN count(*) $$) AS (e agtype);
-- Bounded path counts should flow through the Cypher VLE stream, not direct SRF calls.
SELECT * FROM cypher('cypher_vle', $$MATCH p=(u:begin)-[*3..3]->(v:end) RETURN count(p) $$) AS (paths agtype);
-- Each should find 2
SELECT * FROM cypher('cypher_vle', $$MATCH (u:begin)<-[*]-(v:end) RETURN count(*) $$) AS (e agtype);
SELECT * FROM cypher('cypher_vle', $$MATCH (u:begin)<-[*..]-(v:end) RETURN count(*) $$) AS (e agtype);
SELECT * FROM cypher('cypher_vle', $$MATCH (u:begin)<-[*0..]-(v:end) RETURN count(*) $$) AS (e agtype);
SELECT * FROM cypher('cypher_vle', $$MATCH (u:begin)<-[*1..]-(v:end) RETURN count(*) $$) AS (e agtype);
SELECT * FROM cypher('cypher_vle', $$MATCH (u:begin)<-[*1..200]-(v:end) RETURN count(*) $$) AS (e agtype);
-- Should find 1
SELECT * FROM cypher('cypher_vle', $$MATCH p=(u:begin)<-[*3..3]-(v:end) RETURN count(p) $$) AS (paths agtype);
-- Each should find 7092
SELECT * FROM cypher('cypher_vle', $$MATCH (u:begin)-[*]-(v:end) RETURN count(*) $$) AS (e agtype);
SELECT * FROM cypher('cypher_vle', $$MATCH (u:begin)-[*..]-(v:end) RETURN count(*) $$) AS (e agtype);
SELECT * FROM cypher('cypher_vle', $$MATCH (u:begin)-[*0..]-(v:end) RETURN count(*) $$) AS (e agtype);
SELECT * FROM cypher('cypher_vle', $$MATCH (u:begin)-[*1..]-(v:end) RETURN count(*) $$) AS (e agtype);
SELECT * FROM cypher('cypher_vle', $$MATCH (u:begin)-[*1..200]-(v:end) RETURN count(*) $$) AS (e agtype);
-- Should find 12
SELECT * FROM cypher('cypher_vle', $$MATCH p=(u:begin)-[*3..3]-(v:end) RETURN count(p) $$) AS (paths agtype);
-- Each should find 1
SELECT * FROM cypher('cypher_vle', $$MATCH (u:begin)-[:edge*]-(v:end) RETURN count(*) $$) AS (e agtype);
SELECT * FROM cypher('cypher_vle', $$MATCH (u:begin)-[:edge* {name: "main edge"}]-(v:end) RETURN count(*) $$) AS (e agtype);
SELECT * FROM cypher('cypher_vle', $$MATCH (u:begin)-[* {name: "main edge"}]-(v:end) RETURN count(*) $$) AS (e agtype);
-- Each should find 1
SELECT * FROM cypher('cypher_vle', $$MATCH ()<-[*4..4 {name: "main edge"}]-() RETURN count(*) $$) AS (e agtype);
SELECT * FROM cypher('cypher_vle', $$MATCH (u)<-[*4..4 {name: "main edge"}]-() RETURN count(*) $$) AS (e agtype);
SELECT * FROM cypher('cypher_vle', $$MATCH ()<-[*4..4 {name: "main edge"}]-(v) RETURN count(*) $$) AS (e agtype);
-- Each should find 2922
SELECT * FROM cypher('cypher_vle', $$MATCH ()-[*]->() RETURN count(*) $$) AS (e agtype);
SELECT * FROM cypher('cypher_vle', $$MATCH (u)-[*]->() RETURN count(*) $$) AS (e agtype);
SELECT * FROM cypher('cypher_vle', $$MATCH ()-[*]->(v) RETURN count(*) $$) AS (e agtype);
-- Runtime nodes()/relationships() should project compact VLE paths through the marker stream.
SELECT * FROM cypher('cypher_vle', $$MATCH p=(u:begin)-[*3..3]->(v:end) RETURN count(nodes(p)), count(relationships(p)), sum(length(p)) $$) AS (node_lists agtype, relationship_lists agtype, total_length agtype);
-- Optimized anonymous terminal: bound start, unused anonymous end
SELECT * FROM cypher('cypher_vle', $$MATCH (u:begin)-[e*1..2 {name: "main edge"}]->() RETURN count(*) $$) AS (e agtype);
SELECT * FROM cypher('cypher_vle', $$MATCH p=(u:begin)-[e*1..2 {name: "main edge"}]->() RETURN count(p) $$) AS (e agtype);
-- Optimized right-bound paths-to: anonymous start, selective anonymous end
SELECT * FROM cypher('cypher_vle', $$MATCH ()-[e*1..2 {name: "main edge"}]->(:end) RETURN count(*) $$) AS (e agtype);
SELECT * FROM cypher('cypher_vle', $$MATCH p=()-[e*1..2 {name: "main edge"}]->(:end) RETURN count(p) $$) AS (e agtype);
SELECT * FROM cypher('cypher_vle', $$EXPLAIN (COSTS OFF) MATCH p=()-[e*1..2 {name: "main edge"}]->(:end) RETURN count(p) $$) AS (plan agtype);
SELECT * FROM cypher('cypher_vle', $$EXPLAIN (VERBOSE, COSTS OFF) MATCH ()-[e*1..2 {name: "main edge"}]->(:end) RETURN count(*) $$) AS (plan agtype);
SELECT * FROM cypher('cypher_vle', $$EXPLAIN (COSTS OFF) MATCH (:begin)-[e*1..2 {name: "main edge"}]->(:end) RETURN count(*) $$) AS (plan agtype);
-- Optimized two-bound lower-zero VLE: age_vle handles start=end zero-path semantics.
SELECT * FROM cypher('cypher_vle', $$MATCH (:begin)-[e*0..2 {name: "main edge"}]->(:end) RETURN count(*) $$) AS (e agtype);
SELECT * FROM cypher('cypher_vle', $$MATCH (u:begin)-[e*0..2 {name: "main edge"}]->(u) RETURN count(*) $$) AS (e agtype);
SELECT * FROM cypher('cypher_vle', $$EXPLAIN (COSTS OFF) MATCH (:begin)-[e*0..2 {name: "main edge"}]->(:end) RETURN count(*) $$) AS (plan agtype);
-- Terminal property output should retarget the marker stream descriptor, not a SQL function OID.
SELECT * FROM cypher('cypher_vle', $$EXPLAIN (VERBOSE, COSTS OFF) MATCH (:begin)-[*1..2]->(n) RETURN n $$) AS (plan agtype);
SELECT * FROM cypher('cypher_vle', $$EXPLAIN (VERBOSE, COSTS OFF) MATCH (:begin)-[*1..2]->(n) RETURN properties(n) $$) AS (plan agtype);
SELECT * FROM cypher('cypher_vle', $$MATCH (:begin)-[*1..2]->(n) RETURN count(n), count(properties(n)) $$) AS (nodes agtype, props agtype);
SELECT * FROM cypher('cypher_vle', $$EXPLAIN (VERBOSE, COSTS OFF) MATCH (:begin)-[*1..2]->(n) RETURN n.name $$) AS (plan agtype);
-- Should find 2
SELECT * FROM cypher('cypher_vle', $$MATCH (u:begin)<-[e*]-(v:end) RETURN e $$) AS (e agtype);
-- Should find 5
SELECT * FROM cypher('cypher_vle', $$MATCH p=(:begin)<-[*1..1]-()-[]-() RETURN p ORDER BY p $$) AS (e agtype);
-- Should find 2922
SELECT * FROM cypher('cypher_vle', $$MATCH p=()-[*]->(v) RETURN count(*) $$) AS (e agtype);
-- Should find 2
SELECT * FROM cypher('cypher_vle', $$MATCH p=(u:begin)-[*3..3]->(v:end) RETURN p $$) AS (e agtype);
-- Should find 12
SELECT * FROM cypher('cypher_vle', $$MATCH p=(u:begin)-[*3..3]-(v:end) RETURN p $$) AS (e agtype);
-- Each should find 2
SELECT * FROM cypher('cypher_vle', $$MATCH p=(u:begin)<-[*]-(v:end) RETURN p $$) AS (e agtype);
SELECT * FROM cypher('cypher_vle', $$MATCH p=(u:begin)<-[e*]-(v:end) RETURN p $$) AS (e agtype);
SELECT * FROM cypher('cypher_vle', $$MATCH p=(u:begin)<-[e*]-(v:end) RETURN e $$) AS (e agtype);
SELECT * FROM cypher('cypher_vle', $$MATCH p=(:begin)<-[*]-()<-[]-(:end) RETURN p $$) AS (e agtype);
-- Each should return 31
SELECT count(*) FROM cypher('cypher_vle', $$ MATCH ()-[e1]->(v)-[e2]->() RETURN e1,e2 $$) AS (e1 agtype, e2 agtype);
SELECT count(*) FROM cypher('cypher_vle', $$
	MATCH ()-[e1*1..1]->(v)-[e2*1..1]->()
	RETURN e1, e2
$$) AS (e1 agtype, e2 agtype);
SELECT count(*) FROM cypher('cypher_vle', $$
	MATCH (v)-[e1*1..1]->()-[e2*1..1]->()
	RETURN e1, e2
$$) AS (e1 agtype, e2 agtype);
SELECT count(*) FROM cypher('cypher_vle', $$
	MATCH ()-[e1]->(v)-[e2*1..1]->()
	RETURN e1, e2
$$) AS (e1 agtype, e2 agtype);
SELECT count(*) FROM cypher('cypher_vle', $$
    MATCH ()-[e1]->()-[e2*1..1]->()
    RETURN e1, e2
$$) AS (e1 agtype, e2 agtype);
SELECT count(*) FROM cypher('cypher_vle', $$
	MATCH ()-[e1*1..1]->(v)-[e2]->()
	RETURN e1, e2
$$) AS (e1 agtype, e2 agtype);
SELECT count(*) FROM cypher('cypher_vle', $$
    MATCH ()-[e1*1..1]->()-[e2]->()
    RETURN e1, e2
$$) AS (e1 agtype, e2 agtype);
SELECT count(*) FROM cypher('cypher_vle', $$
    MATCH (a)-[e1]->(a)-[e2*1..1]->()
    RETURN e1, e2
$$) AS (e1 agtype, e2 agtype);
SELECT count(*) FROM cypher('cypher_vle', $$
        MATCH (a) MATCH (a)-[e1*1..1]->(v)
        RETURN e1
$$) AS (e1 agtype);
SELECT count(*) FROM cypher('cypher_vle', $$
        MATCH (a) MATCH ()-[e1*1..1]->(a)
        RETURN e1
$$) AS (e1 agtype);
SELECT count(*)
FROM cypher('cypher_vle', $$
    MATCH (a)-[e*1..1]->()
    RETURN a, e
$$) AS (e1 agtype, e2 agtype);
-- Should return 1 path
SELECT * FROM cypher('cypher_vle', $$ MATCH p=()<-[e1*]-(:end)-[e2*]->(:begin) RETURN p $$) AS (result agtype);
-- Each should return 3
SELECT * FROM cypher('cypher_vle', $$MATCH (u:begin)-[e*0..1]->(v) RETURN id(u), e, id(v) $$) AS (u agtype, e agtype, v agtype);
SELECT * FROM cypher('cypher_vle', $$MATCH p=(u:begin)-[e*0..1]->(v) RETURN p $$) AS (p agtype);
-- Each should return 5
SELECT * FROM cypher('cypher_vle', $$MATCH (u)-[e*0..0]->(v) RETURN id(u), e, id(v) $$) AS (u agtype, e agtype, v agtype);
SELECT * FROM cypher('cypher_vle', $$MATCH p=(u)-[e*0..0]->(v) RETURN id(u), p, id(v) $$) AS (u agtype, p agtype, v agtype);
-- Each should return 13 and will be the same
SELECT * FROM cypher('cypher_vle', $$MATCH p=()-[*0..0]->()-[]->() RETURN p $$) AS (p agtype);
SELECT * FROM cypher('cypher_vle', $$MATCH p=()-[]->()-[*0..0]->() RETURN p ORDER BY p $$) AS (p agtype);
-- Constrained start with anonymous terminal should stream directly and still materialize paths
SELECT * FROM cypher('cypher_vle', $$MATCH p=(:begin)-[:edge*1..2]->() RETURN count(p), min(length(p)), max(length(p)) $$) AS (paths agtype, min_length agtype, max_length agtype);
SELECT * FROM cypher('cypher_vle', $$MATCH p=(:begin)-[:edge*1..1]->() RETURN p $$) AS (p agtype);
SELECT * FROM cypher('cypher_vle', $$MATCH p=(:begin {not_here: true})-[:edge*1..2]->() RETURN count(p) $$) AS (paths agtype);
-- Chained zero-length VLE must keep enough endpoint information to build valid paths
SELECT * FROM cypher('cypher_vle', $$MATCH p=(:begin)-[:edge]->()-[*0..0]->() RETURN count(p), min(length(p)), max(length(p)) $$) AS (paths agtype, min_length agtype, max_length agtype);

--
-- Test VLE inside of a BEGIN/COMMIT block
--
BEGIN;

SELECT create_graph('mygraph');

/* should create 1 path with 1 edge */
SELECT * FROM cypher('mygraph', $$
    CREATE (a:Node {name: 'a'})-[:Edge]->(c:Node {name: 'c'})
$$) AS (g1 agtype);

/* should return 1 path with 1 edge */
SELECT * FROM cypher('mygraph', $$
    MATCH p = ()-[:Edge*]->()
    RETURN p
$$) AS (g2 agtype);

/* should delete the original path and replace it with a path with 2 edges */
SELECT * FROM cypher('mygraph', $$
    MATCH (a:Node {name: 'a'})-[e:Edge]->(c:Node {name: 'c'})
    DELETE e
    CREATE (a)-[:Edge]->(:Node {name: 'b'})-[:Edge]->(c)
$$) AS (g3 agtype);

/* should find 2 paths with 1 edge */
SELECT * FROM cypher('mygraph', $$
    MATCH p = ()-[:Edge]->()
    RETURN p
$$) AS (g4 agtype);

/* should return 3 paths, 2 with 1 edge, 1 with 2 edges */
SELECT * FROM cypher('mygraph', $$
    MATCH p = ()-[:Edge*]->()
    RETURN p
$$) AS (g5 agtype);

SELECT drop_graph('mygraph', true);

COMMIT;

--
-- Test VLE inside procedures
--

SELECT create_graph('mygraph');
SELECT create_vlabel('mygraph', 'head');
SELECT create_vlabel('mygraph', 'tail');
SELECT create_vlabel('mygraph', 'node');
SELECT create_elabel('mygraph', 'next');

CREATE OR REPLACE FUNCTION create_list(list_name text)
RETURNS void
LANGUAGE 'plpgsql'
AS $$
DECLARE
    ag_param agtype;
BEGIN
    ag_param = FORMAT('{"list_name": "%s"}', $1)::agtype;
    PERFORM * FROM cypher('mygraph', $CYPHER$
        MERGE (:head {name: $list_name})-[:next]->(:tail {name: $list_name})
    $CYPHER$, ag_param) AS (a agtype);
END $$;

CREATE OR REPLACE FUNCTION prepend_node(list_name text, node_content text)
RETURNS void
LANGUAGE 'plpgsql'
AS $$
DECLARE
    ag_param agtype;
BEGIN
    ag_param = FORMAT('{"list_name": "%s", "node_content": "%s"}', $1, $2)::agtype;
    PERFORM * FROM cypher('mygraph', $CYPHER$
        MATCH (h:head {name: $list_name})-[e:next]->(v)
        DELETE e
        CREATE (h)-[:next]->(:node {content: $node_content})-[:next]->(v)
    $CYPHER$, ag_param) AS (a agtype);
END $$;

CREATE OR REPLACE FUNCTION show_list_use_vle(list_name text)
RETURNS TABLE(node agtype)
LANGUAGE 'plpgsql'
AS $$
DECLARE
    ag_param agtype;
BEGIN
    ag_param = FORMAT('{"list_name": "%s"}', $1)::agtype;
    RETURN QUERY
    SELECT * FROM cypher('mygraph', $CYPHER$
        MATCH (h:head {name: $list_name})-[e:next*]->(v:node)
        RETURN v
        ORDER BY size(e)
    $CYPHER$, ag_param) AS (node agtype);
END $$;

-- create a list
SELECT create_list('list01');

-- prepend a node 'a'
-- should find 1 row
SELECT prepend_node('list01', 'a');
SELECT * FROM show_list_use_vle('list01');

-- prepend a node 'b'
-- should find 2 rows
SELECT prepend_node('list01', 'b');
SELECT * FROM show_list_use_vle('list01');

-- prepend a node 'c'
-- should find 3 rows
SELECT prepend_node('list01', 'c');
SELECT * FROM show_list_use_vle('list01');

DROP FUNCTION show_list_use_vle;

SELECT drop_graph('mygraph', true);

-- invalid reuse of VLE
SELECT * FROM cypher('cypher_vle', $$ MATCH ()-[p]-()-[p *]-() RETURN p $$)as (p agtype);
SELECT * FROM cypher('cypher_vle', $$ MATCH ()-[p *]-()-[p]-() RETURN p $$)as (p agtype);
SELECT * FROM cypher('cypher_vle', $$ MATCH (p)-[p *]-() RETURN p $$)as (p agtype);
SELECT * FROM cypher('cypher_vle', $$ MATCH ()-[p *]-(p) RETURN p $$)as (p agtype);
SELECT * FROM cypher('cypher_vle', $$ MATCH p=()-[p *]-() RETURN p $$)as (p agtype);
SELECT * FROM cypher('cypher_vle', $$ MATCH ()-[p *]-()-[p *]-() RETURN p $$)as (p agtype);
SELECT * FROM cypher('cypher_vle', $$ MATCH ()-[p]-() MATCH ()-[p *]-() RETURN p $$)as (p agtype);
SELECT * FROM cypher('cypher_vle', $$ MATCH ()-[p *]-() MATCH ()-[p]-() RETURN p $$)as (p agtype);
SELECT * FROM cypher('cypher_vle', $$ MATCH ()-[p *]-() MATCH ()-[p *]-() RETURN p $$)as (p agtype);
SELECT * FROM cypher('cypher_vle', $$ MATCH (p) MATCH ()-[p *]-() RETURN p $$)as (p agtype);
SELECT * FROM cypher('cypher_vle', $$ MATCH ()-[p *]-() MATCH (p) RETURN p $$)as (p agtype);
SELECT * FROM cypher('cypher_vle', $$ MATCH p=() MATCH ()-[p *]-() RETURN p $$)as (p agtype);
SELECT * FROM cypher('cypher_vle', $$ MATCH ()-[p *]-() MATCH p=() RETURN p $$)as (p agtype);

-- issue 1033, agtype_access_operator not working on containerized edges
SELECT create_graph('access');

SELECT * FROM cypher('access',$$ CREATE ()-[:knows]->() $$) as (results agtype);
SELECT * FROM cypher('access',$$ CREATE ()-[:knows]->()-[:knows]->()$$) as (results agtype);
SELECT * FROM cypher('access',$$ CREATE ()-[:knows {id:0}]->()-[:knows {id: 1}]->() $$) as (results agtype);
SELECT * FROM cypher('access',$$ CREATE ()-[:knows {id:2, arry:[0,1,2,3,{name: "joe"}]}]->()-[:knows {id: 3, arry:[1,3,{name:"john", stats: {age: 1000}}]}]->() $$) as (results agtype);
SELECT * FROM cypher('access', $$ MATCH (u)-[e*]->(v) RETURN e $$)as (edges agtype);
SELECT * FROM cypher('access', $$ MATCH (u)-[e*2..2]->(v) RETURN e $$)as (edges agtype);
SELECT * FROM cypher('access',$$ MATCH ()-[e*2..2]->() RETURN properties(e[0]) $$) as (prop_first_edge agtype);
SELECT * FROM cypher('access',$$ MATCH ()-[e*2..2]->() RETURN e[0].id $$) as (results agtype);
SELECT * FROM cypher('access',$$ MATCH ()-[e*2..2]->() RETURN e[0].arry[2] $$) as (results agtype);
SELECT * FROM cypher('access',$$ MATCH ()-[e*2..2]->() RETURN properties(e[1]) $$) as (prop_second_edge agtype);
SELECT * FROM cypher('access',$$ MATCH ()-[e*2..2]->() RETURN e[1].id $$) as (results agtype);
SELECT * FROM cypher('access',$$ MATCH ()-[e*2..2]->() RETURN e[1].arry[2] $$) as (results agtype);
SELECT * FROM cypher('access',$$ MATCH ()-[e*2..2]->() RETURN e[1].arry[2].stats $$) as (results agtype);
SELECT * FROM cypher('access',$$ MATCH ()-[e*2..2]->() RETURN properties(e[2]) $$) as (prop_third_edge agtype);
SELECT * FROM cypher('access',$$ MATCH p=()-[e*2..2]->() RETURN e[1].arry[2].stats, tail(e)[0].id, relationships(p)[1].id $$) as (edge_index_stats agtype, tail_index_id agtype, relationship_index_id agtype);
SELECT * FROM cypher('access',$$ MATCH ()-[e*2..2]->() RETURN type(e[0]), label(e[0]) $$) as (edge_type agtype, edge_label agtype);
SELECT * FROM cypher('access',$$ MATCH ()-[e*2..2]->() RETURN e[0] = e[0], e[0] = e[1] $$) as (same_edge agtype, different_edge agtype);
SELECT * FROM cypher('access',$$ MATCH p=()-[e*2..2]->() RETURN head(e) IN e, tail(e)[0] IN e, relationships(p)[1] IN relationships(p), last(e) IN e $$) as (head_in_edges agtype, tail_index_in_edges agtype, relationship_index_in_path agtype, last_in_edges agtype);
SELECT * FROM cypher('access',$$ MATCH ()-[e*2..2]->() RETURN last([e[0]]) = head(e), head([tail(e)[0]]) = last(e), last([e[1]]) IN e $$) as (singleton_head_equal agtype, singleton_tail_equal agtype, singleton_in_edges agtype);
SELECT * FROM cypher('access',$$ MATCH ()-[e*2..2]->() RETURN tail(reverse(e))[0] IN e, reverse(tail(e))[0] IN e, tail(tail(e))[0] IN e $$) as (tail_reverse_in_edges agtype, reverse_tail_in_edges agtype, double_tail_in_edges agtype);
SELECT * FROM cypher('access',$$ MATCH p=()-[e*2..2]->() RETURN tail(reverse(e))[0] = e[0], reverse(tail(e))[0] = e[1], tail(tail(e))[0] = e[0], reverse(tail(e))[0] = relationships(p)[1] $$) as (nested_tail_reverse_same agtype, nested_reverse_tail_same agtype, nested_double_tail_same agtype, nested_path_relationship_same agtype);
SELECT * FROM cypher('access',$$ MATCH ()-[e*2..2]->() RETURN tail(reverse(e))[0] = reverse(tail(e))[0], tail(reverse(e))[0] = tail(reverse(e))[0], reverse(tail(e))[0] = tail(reverse(e))[0], tail(tail(e))[0] = tail(tail(e))[0] $$) as (nested_cross_same agtype, nested_tail_reverse_self agtype, nested_cross_symmetric agtype, nested_empty_self agtype);
SELECT * FROM cypher('access',$$ MATCH p=()-[*2..2]->() RETURN relationships(p)[1].id $$) as (relationship_id agtype);
SELECT * FROM cypher('access',$$ MATCH p=()-[*2..2]->() RETURN size(relationships(p)) $$) as (relationship_count agtype);
SELECT * FROM cypher('access',$$ MATCH p=()-[*2..2]->() RETURN size(relationships(p)[1..]), size(nodes(p)[1..]), size(tail(nodes(p))[0..1]) $$) as (relationship_suffix_count agtype, node_suffix_count agtype, tail_node_prefix_count agtype);
SELECT * FROM cypher('access',$$ MATCH p=()-[*2..2]->() RETURN isEmpty(relationships(p)[2..]), isEmpty(nodes(p)[3..]), isEmpty(tail(nodes(p))[1..]) $$) as (relationship_suffix_empty agtype, node_suffix_empty agtype, tail_node_suffix_empty agtype);
SELECT * FROM cypher('access',$$ MATCH p=()-[*2..2]->() RETURN size(tail(nodes(p))), size(reverse(nodes(p))), size(tail(relationships(p))), size(reverse(relationships(p))) $$) as (tail_node_count agtype, reverse_node_count agtype, tail_relationship_count agtype, reverse_relationship_count agtype);
SELECT * FROM cypher('access',$$ MATCH p=()-[*2..2]->() RETURN isEmpty(tail(nodes(p))), isEmpty(reverse(nodes(p))), isEmpty(tail(relationships(p))), isEmpty(reverse(relationships(p))) $$) as (tail_nodes_empty agtype, reverse_nodes_empty agtype, tail_relationships_empty agtype, reverse_relationships_empty agtype);
SELECT * FROM cypher('access',$$ MATCH p=()-[*2..2]->() RETURN head(nodes(p)).id, last(nodes(p)).id, head(relationships(p)).id, last(relationships(p)).id $$) as (first_node agtype, last_node agtype, first_relationship agtype, last_relationship agtype);
SELECT * FROM cypher('access',$$ MATCH p=()-[*2..2]->() RETURN id(nodes(p)[1]), id(relationships(p)[1]) $$) as (node_id agtype, relationship_id agtype);
SELECT * FROM cypher('access',$$ MATCH p=()-[*2..2]->() RETURN id(startNode(relationships(p)[1])), id(endNode(relationships(p)[1])) $$) as (start_node_id agtype, end_node_id agtype);
SELECT * FROM cypher('access',$$ MATCH p=()-[*2..2]->() RETURN id(startNode(head(relationships(p)))), id(endNode(head(reverse(relationships(p))))) $$) as (head_start_id agtype, reverse_end_id agtype);
SELECT * FROM cypher('access',$$ MATCH p=()-[*2..2]->() RETURN startNode(relationships(p)[1]), endNode(relationships(p)[1]) $$) as (start_node agtype, end_node agtype);
SELECT * FROM cypher('access',$$ MATCH p=()-[*2..2]->() RETURN startNode(head(relationships(p))), endNode(head(reverse(relationships(p)))) $$) as (head_start_node agtype, reverse_end_node agtype);
SELECT * FROM cypher('access',$$ MATCH p=()-[*2..2]->() RETURN label(startNode(relationships(p)[1])), labels(endNode(head(relationships(p)))), properties(startNode(reverse(relationships(p))[0])) $$) as (endpoint_label agtype, endpoint_labels agtype, endpoint_properties agtype);
SELECT * FROM cypher('access',$$ MATCH p=()-[*2..2]->() RETURN label(nodes(p)[1]), labels(nodes(p)[1]), properties(nodes(p)[1]) $$) as (node_label agtype, node_labels agtype, node_properties agtype);
SELECT * FROM cypher('access',$$ MATCH p=()-[*2..2]->() RETURN label(head(nodes(p))), labels(head(tail(nodes(p)))), properties(head(reverse(nodes(p)))) $$) as (head_node_label agtype, tail_node_labels agtype, reverse_node_properties agtype);
SELECT * FROM cypher('access',$$ MATCH p=()-[*2..2]->() RETURN type(relationships(p)[1]), label(relationships(p)[1]) $$) as (edge_type agtype, edge_label agtype);
SELECT * FROM cypher('access',$$ MATCH p=()-[*2..2]->() RETURN properties(relationships(p)[1]) $$) as (edge_properties agtype);
SELECT * FROM cypher('access',$$ MATCH p=()-[*2..2]->() RETURN type(head(relationships(p))), label(head(reverse(relationships(p)))), properties(head(tail(relationships(p)))) $$) as (head_edge_type agtype, reverse_edge_label agtype, tail_edge_properties agtype);
SELECT * FROM cypher('access',$$ MATCH p=()-[*2..2]->() RETURN nodes(p)[1..] $$) as (node_suffix agtype);
SELECT * FROM cypher('access',$$ MATCH p=()-[*2..2]->() RETURN head(tail(nodes(p))).id, head(reverse(nodes(p))).id $$) as (tail_first_node agtype, reverse_first_node agtype);
SELECT * FROM cypher('access',$$ MATCH p=()-[*2..2]->() RETURN id(head(nodes(p))), id(head(tail(nodes(p)))), id(head(reverse(nodes(p)))) $$) as (first_node_id agtype, tail_node_id agtype, reverse_node_id agtype);
SELECT * FROM cypher('access',$$ MATCH p=()-[*0..1]->() RETURN count(last(tail(nodes(p)))), count(last(tail(relationships(p)))) $$) as (tail_last_node_count agtype, tail_last_relationship_count agtype);
SELECT * FROM cypher('access',$$ MATCH p=()-[*0..1]->() RETURN count(id(last(tail(nodes(p))))), count(id(last(tail(relationships(p))))) $$) as (tail_last_node_id_count agtype, tail_last_relationship_id_count agtype);
SELECT * FROM cypher('access',$$ MATCH p=()-[*0..1]->() RETURN count(label(last(tail(nodes(p))))), count(properties(last(tail(nodes(p))))), count(type(last(tail(relationships(p))))) $$) as (tail_last_node_label_count agtype, tail_last_node_properties_count agtype, tail_last_relationship_type_count agtype);
SELECT * FROM cypher('access',$$ MATCH p=()-[*1..2]->() RETURN count(startNode(last(tail(relationships(p))))), count(endNode(last(tail(relationships(p))))), count(id(startNode(last(tail(relationships(p)))))), count(id(endNode(last(tail(relationships(p)))))) $$) as (tail_last_start_node_count agtype, tail_last_end_node_count agtype, tail_last_start_id_count agtype, tail_last_end_id_count agtype);
SELECT * FROM cypher('access',$$ MATCH p=()-[*1..2]->() RETURN count(label(startNode(last(tail(relationships(p)))))), count(labels(endNode(last(tail(relationships(p)))))), count(properties(startNode(last(tail(relationships(p)))))) $$) as (tail_last_start_label_count agtype, tail_last_end_labels_count agtype, tail_last_start_properties_count agtype);
SELECT * FROM cypher('access',$$ MATCH p=()-[*2..2]->() RETURN id(tail(nodes(p))[0]), id(tail(relationships(p))[0]) $$) as (tail_node0_id agtype, tail_relationship0_id agtype);
SELECT * FROM cypher('access',$$ MATCH p=()-[*2..2]->() RETURN id(reverse(nodes(p))[0]), id(reverse(relationships(p))[0]) $$) as (reverse_node0_id agtype, reverse_relationship0_id agtype);
SELECT * FROM cypher('access',$$ MATCH p=()-[*2..2]->() RETURN label(reverse(nodes(p))[0]), properties(reverse(nodes(p))[0]), type(reverse(relationships(p))[0]), properties(reverse(relationships(p))[0]) $$) as (reverse_node0_label agtype, reverse_node0_properties agtype, reverse_relationship0_type agtype, reverse_relationship0_properties agtype);
SELECT * FROM cypher('access',$$ MATCH p=()-[*2..2]->() RETURN id(startNode(reverse(relationships(p))[0])), id(endNode(reverse(relationships(p))[0])) $$) as (reverse_relationship0_start_id agtype, reverse_relationship0_end_id agtype);
SELECT * FROM cypher('access',$$ MATCH p=()-[*2..2]->() RETURN id(head(reverse(nodes(p))[0..1])), id(head(reverse(relationships(p))[0..1])) $$) as (reverse_node_slice_head_id agtype, reverse_relationship_slice_head_id agtype);
SELECT * FROM cypher('access',$$ MATCH p=()-[*2..2]->() RETURN type(head(reverse(relationships(p))[0..1])), properties(head(reverse(relationships(p))[0..1])) $$) as (reverse_relationship_slice_head_type agtype, reverse_relationship_slice_head_properties agtype);
SELECT * FROM cypher('access',$$ MATCH p=()-[*2..2]->() RETURN reverse(relationships(p))[0..1], reverse(nodes(p))[0..1] $$) as (reverse_relationship_slice agtype, reverse_node_slice agtype);
SELECT * FROM cypher('access',$$ MATCH p=()-[*2..2]->() RETURN head(tail(relationships(p))).id, head(reverse(relationships(p))).id $$) as (tail_first_relationship agtype, reverse_first_relationship agtype);
SELECT * FROM cypher('access',$$ MATCH p=()-[*2..2]->() RETURN id(head(relationships(p))), id(head(tail(relationships(p)))), id(head(reverse(relationships(p)))) $$) as (first_relationship_id agtype, tail_relationship_id agtype, reverse_relationship_id agtype);
SELECT * FROM cypher('access',$$ MATCH p=()-[*2..2]->() RETURN head(tail(relationships(p))).id, head(reverse(relationships(p))).arry[2].stats $$) as (tail_first_relationship_id agtype, reverse_relationship_stats agtype);
SELECT * FROM cypher('access',$$ MATCH p=()-[*2..2]->() RETURN relationships(p)[1..] $$) as (relationship_suffix agtype);
SELECT * FROM cypher('access',$$ MATCH ()-[e*2..2]->() RETURN e[0..1] $$) as (edge_slice agtype);
SELECT * FROM cypher('access',$$ MATCH ()-[e*2..2]->() RETURN e[-1..] $$) as (edge_slice agtype);
SELECT * FROM cypher('access',$$ MATCH ()-[e*2..2]->() RETURN size(e), isEmpty(e) $$) as (edge_size agtype, is_empty agtype);
SELECT * FROM cypher('access',$$ MATCH ()-[e*2..2]->() RETURN id(head(e)), id(last(e)), type(head(e)), id(startNode(last(e))) $$) as (head_edge_id agtype, last_edge_id agtype, head_edge_type agtype, last_edge_start_id agtype);
SELECT * FROM cypher('access',$$ MATCH ()-[e*2..2]->() RETURN head(e).id, last(e).id, last(e).arry[2].stats $$) as (head_edge_id agtype, last_edge_id agtype, last_edge_stats agtype);
SELECT * FROM cypher('access',$$ MATCH ()-[e*2..2]->() RETURN head(e).label, last(e).start_id, last(e).end_id $$) as (head_edge_label agtype, last_edge_start_id agtype, last_edge_end_id agtype);
SELECT * FROM cypher('access',$$ MATCH ()-[e*2..2]->() RETURN size(e[0..1]), size(e[-1..]), size(tail(e)[0..1]) $$) as (edge_prefix_count agtype, edge_suffix_count agtype, tail_edge_prefix_count agtype);
SELECT * FROM cypher('access',$$ MATCH ()-[e*2..2]->() RETURN isEmpty(e[0..0]), isEmpty(e[0..1]), isEmpty(tail(e)[1..]) $$) as (edge_empty_prefix agtype, edge_nonempty_prefix agtype, tail_edge_suffix_empty agtype);
SELECT * FROM cypher('access',$$ MATCH ()-[e*2..2]->() RETURN id(head(e[0..1])), id(last(e[-1..])), id(head(tail(e)[0..1])) $$) as (edge_slice_head_id agtype, edge_slice_last_id agtype, tail_edge_slice_head_id agtype);
SELECT * FROM cypher('access',$$ MATCH p=()-[*2..2]->() RETURN id(head(relationships(p)[1..])), id(last(nodes(p)[..2])), id(head(tail(nodes(p))[0..1])) $$) as (relationship_slice_head_id agtype, node_slice_last_id agtype, tail_node_slice_head_id agtype);
SELECT * FROM cypher('access',$$ MATCH ()-[e*2..2]->() RETURN type(head(e[0..1])), properties(last(e[-1..])) $$) as (edge_slice_head_type agtype, edge_slice_last_properties agtype);
SELECT * FROM cypher('access',$$ MATCH p=()-[*2..2]->() RETURN label(head(nodes(p)[1..])), labels(last(tail(nodes(p))[0..1])), properties(last(nodes(p)[..2])) $$) as (node_slice_head_label agtype, tail_node_slice_last_labels agtype, node_slice_last_properties agtype);
SELECT * FROM cypher('access',$$ MATCH ()-[e*2..2]->() RETURN last(e[-1..]).arry[2].stats $$) as (edge_slice_last_stats agtype);
SELECT * FROM cypher('access',$$ MATCH ()-[e*2..2]->() RETURN id(startNode(head(e[0..1]))), id(endNode(last(e[-1..]))) $$) as (edge_slice_head_start_id agtype, edge_slice_last_end_id agtype);
SELECT * FROM cypher('access',$$ MATCH p=()-[*2..2]->() RETURN id(startNode(head(relationships(p)[1..]))), labels(endNode(last(relationships(p)[1..]))), properties(startNode(head(relationships(p)[1..]))) $$) as (rel_slice_head_start_id agtype, rel_slice_last_end_labels agtype, rel_slice_head_start_properties agtype);
SELECT * FROM cypher('access',$$ MATCH ()-[e*2..2]->() RETURN size(tail(e)), size(reverse(e)) $$) as (tail_edge_size agtype, reverse_edge_size agtype);
SELECT * FROM cypher('access',$$ MATCH ()-[e*2..2]->() RETURN isEmpty(tail(e)), isEmpty(reverse(e)) $$) as (tail_edge_empty agtype, reverse_edge_empty agtype);
SELECT * FROM cypher('access',$$ MATCH ()-[e*2..2]->() RETURN size(tail(reverse(e))), size(reverse(tail(e))), isEmpty(tail(reverse(e))), isEmpty(reverse(tail(e))) $$) as (tail_reverse_edge_size agtype, reverse_tail_edge_size agtype, tail_reverse_edge_empty agtype, reverse_tail_edge_empty agtype);
SELECT * FROM cypher('access',$$ MATCH ()-[e*2..2]->() RETURN size(tail(tail(e))), isEmpty(tail(tail(e))) $$) as (double_tail_edge_size agtype, double_tail_edge_empty agtype);
SELECT * FROM cypher('access',$$ MATCH ()-[e*2..2]->() RETURN tail(tail(e))[0..1] $$) as (double_tail_edge_slice agtype);
SELECT * FROM cypher('access',$$ MATCH p=()-[*2..2]->() RETURN count(head(tail(tail(nodes(p)))[0..1])), count(head(tail(tail(relationships(p)))[0..1])) $$) as (double_tail_node_slice_count agtype, double_tail_rel_slice_count agtype);
SELECT * FROM cypher('access',$$ MATCH ()-[e*2..2]->() RETURN size(tail(tail(e))[0..1]), isEmpty(tail(tail(e))[0..1]) $$) as (double_tail_edge_slice_size agtype, double_tail_edge_slice_empty agtype);
SELECT * FROM cypher('access',$$ MATCH ()-[e*2..2]->() RETURN count(startNode(e[0])), count(endNode(e[0])), count(id(startNode(e[0]))), count(id(endNode(e[0]))) $$) as (edge_start_node_count agtype, edge_end_node_count agtype, edge_start_id_count agtype, edge_end_id_count agtype);
SELECT * FROM cypher('access',$$ MATCH ()-[e*2..2]->() RETURN id(tail(e)[0]) $$) as (tail_edge0_id agtype);
SELECT * FROM cypher('access',$$ MATCH ()-[e*2..2]->() RETURN id(reverse(e)[0]) $$) as (reverse_edge0_id agtype);
SELECT * FROM cypher('access',$$ MATCH ()-[e*2..2]->() RETURN type(reverse(e)[0]), properties(reverse(e)[0]) $$) as (reverse_edge0_type agtype, reverse_edge0_properties agtype);
SELECT * FROM cypher('access',$$ MATCH ()-[e*2..2]->() RETURN id(head(reverse(e)[0..1])), head(reverse(e)[0..1]).arry[2].stats, id(endNode(head(reverse(e)[0..1]))) $$) as (reverse_edge_slice_head_id agtype, reverse_edge_slice_stats agtype, reverse_edge_slice_end_id agtype);
SELECT * FROM cypher('access',$$ MATCH ()-[e*2..2]->() RETURN tail(e)[0..1], reverse(e)[0..1] $$) as (tail_edge_slice agtype, reverse_edge_slice agtype);
SELECT * FROM cypher('access',$$ MATCH ()-[e*2..2]->() RETURN tail(reverse(e))[0..1], reverse(tail(e))[0..1] $$) as (tail_reverse_edge_slice agtype, reverse_tail_edge_slice agtype);
SELECT * FROM cypher('access',$$ MATCH p=()-[*2..2]->() RETURN count(head(tail(reverse(nodes(p)))[0..1])), count(head(reverse(tail(nodes(p)))[0..1])), count(head(tail(reverse(relationships(p)))[0..1])), count(head(reverse(tail(relationships(p)))[0..1])) $$) as (tail_reverse_node_slice_count agtype, reverse_tail_node_slice_count agtype, tail_reverse_rel_slice_count agtype, reverse_tail_rel_slice_count agtype);
SELECT * FROM cypher('access',$$ MATCH ()-[e*2..2]->() RETURN size(tail(reverse(e))[0..1]), size(reverse(tail(e))[0..1]), isEmpty(tail(reverse(e))[0..1]), isEmpty(reverse(tail(e))[0..1]) $$) as (tail_reverse_edge_slice_size agtype, reverse_tail_edge_slice_size agtype, tail_reverse_edge_slice_empty agtype, reverse_tail_edge_slice_empty agtype);
SELECT * FROM cypher('access',$$ MATCH ()-[e*2..2]->() RETURN id(head(tail(reverse(e))[0..1])), id(last(reverse(tail(e))[0..1])), id(head(tail(tail(e))[0..1])) $$) as (tail_reverse_slice_head_id agtype, reverse_tail_slice_last_id agtype, double_tail_slice_head_id agtype);
SELECT * FROM cypher('access',$$ MATCH ()-[e*2..2]->() RETURN id(last(tail(reverse(e)))), id(head(reverse(tail(e)))), last(tail(tail(e))).id $$) as (tail_reverse_last_id agtype, reverse_tail_head_id agtype, double_tail_last_id agtype);
SELECT * FROM cypher('access',$$ MATCH ()-[e*2..2]->() RETURN type(last(tail(reverse(e)))), properties(head(reverse(tail(e)))), head(reverse(tail(e))).arry[2].stats $$) as (tail_reverse_last_type agtype, reverse_tail_head_properties agtype, reverse_tail_head_stats agtype);
SELECT * FROM cypher('access',$$ MATCH ()-[e*2..2]->() RETURN id(startNode(last(tail(reverse(e))))), id(endNode(head(reverse(tail(e))))), labels(startNode(head(reverse(tail(e))))), properties(endNode(last(tail(reverse(e))))) $$) as (tail_reverse_last_start_id agtype, reverse_tail_head_end_id agtype, reverse_tail_head_start_labels agtype, tail_reverse_last_end_properties agtype);
SELECT * FROM cypher('access',$$ MATCH ()-[e*2..2]->() RETURN id(tail(reverse(e))[0]), id(reverse(tail(e))[0]), id(tail(tail(e))[0]) $$) as (tail_reverse_index_id agtype, reverse_tail_index_id agtype, double_tail_index_id agtype);
SELECT * FROM cypher('access',$$ MATCH ()-[e*2..2]->() RETURN type(tail(reverse(e))[0]), properties(reverse(tail(e))[0]), properties(tail(tail(e))[0]) $$) as (tail_reverse_index_type agtype, reverse_tail_index_properties agtype, double_tail_index_properties agtype);
SELECT * FROM cypher('access',$$ MATCH ()-[e*2..2]->() RETURN id(startNode(tail(reverse(e))[0])), id(endNode(reverse(tail(e))[0])), labels(startNode(tail(tail(e))[0])) $$) as (tail_reverse_start_id agtype, reverse_tail_end_id agtype, double_tail_start_labels agtype);
SELECT * FROM cypher('access',$$ MATCH ()-[e*2..2]->() RETURN reverse(tail(e))[0].arry[2].stats, tail(reverse(e))[0].id, tail(tail(e))[0].id $$) as (reverse_tail_index_stats agtype, tail_reverse_index_property_id agtype, double_tail_index_property_id agtype);
SELECT * FROM cypher('access',$$ MATCH ()-[e*1..2]->() RETURN count(last(tail(e))) $$) as (tail_last_edge_count agtype);
SELECT * FROM cypher('access',$$ MATCH ()-[e*1..2]->() RETURN count(id(last(tail(e)))) $$) as (tail_last_edge_id_count agtype);
SELECT * FROM cypher('access',$$ MATCH ()-[e*1..2]->() RETURN count(type(last(tail(e)))), count(properties(last(tail(e)))), count(last(tail(e)).arry[2].stats) $$) as (tail_last_edge_type_count agtype, tail_last_edge_properties_count agtype, tail_last_edge_property_count agtype);
SELECT * FROM cypher('access',$$ MATCH ()-[e*1..2]->() RETURN count(startNode(last(tail(e)))), count(endNode(last(tail(e)))), count(id(startNode(last(tail(e))))), count(id(endNode(last(tail(e))))) $$) as (tail_last_edge_start_node_count agtype, tail_last_edge_end_node_count agtype, tail_last_edge_start_id_count agtype, tail_last_edge_end_id_count agtype);
SELECT * FROM cypher('access',$$ MATCH ()-[e*1..2]->() RETURN count(label(startNode(last(tail(e))))), count(labels(endNode(last(tail(e))))), count(properties(endNode(last(tail(e))))) $$) as (tail_last_edge_start_label_count agtype, tail_last_edge_end_labels_count agtype, tail_last_edge_end_properties_count agtype);
SELECT * FROM cypher('access',$$ MATCH ()-[e*2..2]->() RETURN tail(e) $$) as (edge_tail agtype);

SELECT * FROM cypher('access',$$ MATCH ()-[e*]->() RETURN properties(e[0]), properties(e[1]) $$) as (prop_1st agtype, prop_2nd agtype);
SELECT * FROM cypher('access',$$ MATCH ()-[e*]->() RETURN e[0].id, e[1].id $$) as (results_1st agtype, results_2nd agtype);
SELECT * FROM cypher('access',$$ MATCH ()-[e*]->() RETURN e[0].arry, e[1].arry $$) as (results_1st agtype, results_2nd agtype);
SELECT * FROM cypher('access',$$ MATCH ()-[e*]->() RETURN e[0].arry[2], e[1].arry[2] $$) as (results_1st agtype, results_2nd agtype);

SELECT drop_graph('access', true);

-- issue 1043
SELECT create_graph('issue_1043');
SELECT * FROM cypher('issue_1043', $$ CREATE (n)-[:KNOWS {n:'hello'}]->({n:'hello'}) $$) as (a agtype);
SELECT * FROM cypher('issue_1043', $$ MATCH (x)<-[y *]-(),({n:y[0].n}) RETURN x ORDER BY id(x) $$) as (a agtype);
SELECT * FROM cypher('issue_1043', $$ CREATE (n)-[:KNOWS {n:'hello'}]->({n:'hello'}) $$) as (a agtype);
SELECT * FROM cypher('issue_1043', $$ MATCH (x)<-[y *]-(),({n:y[0].n}) RETURN x ORDER BY id(x) $$) as (a agtype);

SELECT drop_graph('issue_1043', true);

-- issue 1910
SELECT create_graph('issue_1910');
SELECT * FROM cypher('issue_1910', $$ MATCH (n) WHERE EXISTS((n)-[*1]-({name: 'Willem Defoe'}))
                                      RETURN n.full_name $$) AS (full_name agtype);
SELECT * FROM cypher('issue_1910', $$ CREATE ({name: 'Jane Doe'})-[:KNOWS]->({name: 'John Doe'}) $$) AS (result agtype);
SELECT * FROM cypher('issue_1910', $$ CREATE ({name: 'Donald Defoe'})-[:KNOWS]->({name: 'Willem Defoe'}) $$) AS (result agtype);
SELECT * FROM cypher('issue_1910', $$ MATCH (u {name: 'John Doe'})
                                      MERGE (u)-[:KNOWS]->({name: 'Willem Defoe'}) $$) AS (result agtype);

SELECT * FROM cypher('issue_1910', $$ EXPLAIN (VERBOSE, COSTS OFF)
                                      MATCH (n) WHERE EXISTS((n)-[*]-({name: 'Willem Defoe'}))
                                      RETURN n.name $$) AS (plan agtype);
SELECT * FROM cypher('issue_1910', $$ MATCH (n) WHERE EXISTS((n)-[*]-({name: 'Willem Defoe'}))
                                      RETURN n.name $$) AS (name agtype);
SELECT * FROM cypher('issue_1910', $$ MATCH (n) WHERE EXISTS((n)-[*1]-({name: 'Willem Defoe'}))
                                      RETURN n.name $$) AS (name agtype);
SELECT * FROM cypher('issue_1910', $$ MATCH (n) WHERE EXISTS((n)-[*2..2]-({name: 'Willem Defoe'}))
                                      RETURN n.name $$) AS (name agtype);

SELECT * FROM cypher('issue_1910', $$ MATCH p=()-[*1..1]->() RETURN count(startNode(relationships(p)[0]).name) + count(endNode(relationships(p)[0]).name) $$) AS (c agtype);
SELECT * FROM cypher('issue_1910', $$ MATCH ()-[e*1..1]->() RETURN count(startNode(e[0]).name) + count(endNode(e[0]).name) $$) AS (c agtype);
SELECT * FROM cypher('issue_1910', $$ MATCH ()-[e*1..2]->() RETURN count(startNode(last(tail(e))).name) + count(endNode(last(tail(e))).name) $$) AS (c agtype);
SELECT * FROM cypher('issue_1910', $$ MATCH ()-[e*1..1]->() RETURN count(startNode(head(e[0..1])).name) + count(endNode(last(e[0..])).name) $$) AS (c agtype);
SELECT * FROM cypher('issue_1910', $$ MATCH ()-[e*1..2]->() RETURN count(startNode(tail(reverse(e))[0]).name) + count(endNode(reverse(tail(e))[0]).name) $$) AS (c agtype);
SELECT * FROM cypher('issue_1910', $$ MATCH ()-[e*1..2]->() RETURN count(startNode(last(tail(reverse(e)))).name) + count(endNode(head(reverse(tail(e)))).name) $$) AS (c agtype);

SELECT drop_graph('issue_1910', true);

-- issue 2092: VLE with chained OPTIONAL MATCH and NULL handling
-- Previously, chained OPTIONAL MATCH with VLE would either segfault
-- (with WHERE IS NOT NULL) or error with "arguments cannot be NULL"
-- (without WHERE) instead of producing correct NULL-extended rows.
SELECT create_graph('issue_2092');

-- Set up a small graph where some OPTIONAL MATCH paths exist and some don't
SELECT * FROM cypher('issue_2092', $$
    CREATE (a:Person {name: 'Alice'})
    CREATE (b:Person {name: 'Bob'})
    CREATE (c:City {name: 'NYC'})
    CREATE (d:City {name: 'LA'})
    CREATE (e:Place {name: 'Central Park'})
    CREATE (a)-[:LIVES_IN]->(c)
    CREATE (c)-[:HAS_PLACE]->(e)
    CREATE (b)-[:LIVES_IN]->(d)
$$) AS (result agtype);

-- Alice lives in NYC which has Central Park.
-- Bob lives in LA which has no places.
-- VLE + chained OPTIONAL MATCH + WHERE IS NOT NULL: should return rows
-- without crashing (was: segfault)
SELECT * FROM cypher('issue_2092', $$
    MATCH (p:Person)-[:LIVES_IN*]->(c:City)
    OPTIONAL MATCH (c)-[:HAS_PLACE*]->(place)
    OPTIONAL MATCH (place)-[:NEARBY*]->(other)
    WHERE place IS NOT NULL
    RETURN p.name, place.name, other
    ORDER BY p.name
$$) AS (person agtype, place agtype, other agtype);

-- VLE + chained OPTIONAL MATCH without WHERE: should return NULL-extended
-- rows without error (was: "match_vle_terminal_edge() arguments cannot be
-- NULL")
SELECT * FROM cypher('issue_2092', $$
    MATCH (p:Person)-[:LIVES_IN*]->(c:City)
    OPTIONAL MATCH (c)-[:HAS_PLACE*]->(place)
    OPTIONAL MATCH (place)-[:NEARBY*]->(other)
    RETURN p.name, place.name, other
    ORDER BY p.name
$$) AS (person agtype, place agtype, other agtype);

-- Verify the happy path still works: Alice's full chain resolves
SELECT * FROM cypher('issue_2092', $$
    MATCH (p:Person)-[:LIVES_IN*]->(c:City)
    OPTIONAL MATCH (c)-[:HAS_PLACE*]->(place)
    WHERE place IS NOT NULL
    RETURN p.name, c.name, place.name
    ORDER BY p.name
$$) AS (person agtype, city agtype, place agtype);

SELECT drop_graph('issue_2092', true);

-- VLE start property filters should be able to use a label properties GIN index
-- without changing the pathless VLE lowering assertions in age_adjacency.
SELECT create_graph('vle_index_probe');

SELECT * FROM cypher('vle_index_probe', $$
    CREATE (n0:N {i: 0}),
           (n1:N {i: 1}),
           (n2:N {i: 2}),
           (n3:N {i: 3}),
           (n4:N {i: 4})
    CREATE (n0)-[:R]->(n1),
           (n0)-[:R]->(n2),
           (n1)-[:R]->(n3),
           (n1)-[:R]->(n4),
           (n2)-[:R]->(n3),
           (n2)-[:R]->(n4)
$$) AS (result agtype);

CREATE INDEX vle_index_probe_n_properties_gin_idx
ON vle_index_probe."N" USING gin (properties);
CREATE INDEX vle_index_probe_r_start_idx
ON vle_index_probe."R" USING btree (start_id);
ANALYZE vle_index_probe."N";
ANALYZE vle_index_probe."R";

SET enable_seqscan = off;

SELECT * FROM cypher('vle_index_probe',
                    $$EXPLAIN (VERBOSE, COSTS OFF) MATCH (:N {i: 0})-[*1..1]->(n) RETURN n.i$$)
AS (plan agtype);

SELECT * FROM cypher('vle_index_probe',
                    $$EXPLAIN (VERBOSE, COSTS OFF) MATCH (:N {i: 0})-[:R*1..1]->(n) RETURN n.i$$)
AS (plan agtype);

SELECT * FROM cypher('vle_index_probe',
                    $$EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY OFF) MATCH (:N {i: 0})-[:R*1..1]->(n) RETURN n.i$$)
AS (plan agtype);

CREATE INDEX vle_index_probe_r_start_adj_idx
ON vle_index_probe."R" USING age_adjacency (start_id, id, end_id);
CREATE INDEX vle_index_probe_r_end_adj_idx
ON vle_index_probe."R" USING age_adjacency (end_id, id, start_id);
ANALYZE vle_index_probe."R";

SELECT * FROM cypher('vle_index_probe',
                    $$EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY OFF) MATCH (:N {i: 0})-[:R*1..1]->(n) RETURN n.i$$)
AS (plan agtype);

SELECT * FROM cypher('vle_index_probe',
                    $$EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY OFF) MATCH (:N)-[:R*1..1]->(n) RETURN n.i$$)
AS (plan agtype);

SELECT * FROM cypher('vle_index_probe',
                    $$EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY OFF) MATCH (:N {i: 0})-[:R*1..2]->(n) RETURN n.i$$)
AS (plan agtype);

SET enable_seqscan = on;

SELECT drop_graph('vle_index_probe', true);

SELECT create_graph('vle_fanout_policy');

SELECT * FROM cypher('vle_fanout_policy', $$
    CREATE (n0:N {i: 0}),
           (n1:N {i: 1}),
           (n2:N {i: 2}),
           (n3:N {i: 3})
    CREATE (n0)-[:R]->(n1),
           (n0)-[:R]->(n2),
           (n0)-[:R]->(n3)
$$) AS (result agtype);

CREATE INDEX vle_fanout_policy_n_properties_gin_idx
ON vle_fanout_policy."N" USING gin (properties);
CREATE INDEX vle_fanout_policy_r_start_idx
ON vle_fanout_policy."R" USING btree (start_id);
CREATE INDEX vle_fanout_policy_r_end_idx
ON vle_fanout_policy."R" USING btree (end_id);
CREATE INDEX vle_fanout_policy_r_start_adj_idx
ON vle_fanout_policy."R" USING age_adjacency (start_id, id, end_id);
CREATE INDEX vle_fanout_policy_r_end_adj_idx
ON vle_fanout_policy."R" USING age_adjacency (end_id, id, start_id);
ANALYZE vle_fanout_policy."N";
ANALYZE vle_fanout_policy."R";

SET enable_seqscan = off;

SELECT * FROM cypher('vle_fanout_policy',
                    $$EXPLAIN (VERBOSE, COSTS OFF) MATCH (:N {i: 0})-[:R*1..2]->(n) RETURN n.i$$)
AS (plan agtype);

SELECT * FROM cypher('vle_fanout_policy',
                    $$EXPLAIN (VERBOSE, COSTS OFF) MATCH (:N {i: 0})-[:R*1..3]->(n) RETURN n.i$$)
AS (plan agtype);

SET enable_seqscan = on;

SELECT drop_graph('vle_fanout_policy', true);

SELECT * FROM cypher('cypher_vle',
                    $$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=()-[*1..2]->() RETURN length(p)$$)
AS (plan agtype);

SELECT * FROM cypher('cypher_vle',
                    $$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=()-[*1..2]->() RETURN size(relationships(p))$$)
AS (plan agtype);

SELECT * FROM cypher('cypher_vle',
                    $$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=()-[*1..2]->() RETURN size(tail(relationships(p)))$$)
AS (plan agtype);

SELECT * FROM cypher('cypher_vle',
                    $$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=()-[*1..2]->() RETURN isEmpty(tail(relationships(p)))$$)
AS (plan agtype);

SELECT * FROM cypher('cypher_vle',
                    $$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=()-[*1..2]->() RETURN size(relationships(p)[1..])$$)
AS (plan agtype);

SELECT * FROM cypher('cypher_vle',
                    $$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=()-[*1..2]->() RETURN isEmpty(relationships(p)[1..])$$)
AS (plan agtype);

SELECT * FROM cypher('cypher_vle',
                    $$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=()-[*1..2]->() RETURN size(tail(relationships(p))[0..1])$$)
AS (plan agtype);

SELECT * FROM cypher('cypher_vle',
                    $$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=()-[*1..2]->() RETURN size(nodes(p))$$)
AS (plan agtype);

SELECT * FROM cypher('cypher_vle',
                    $$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=()-[*1..2]->() RETURN count(p)$$)
AS (plan agtype);

SELECT * FROM cypher('cypher_vle',
                    $$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=()-[*1..2]->() RETURN count(last(tail(nodes(p)))) + count(last(tail(relationships(p)))) + count(startNode(last(tail(relationships(p))))) + count(endNode(last(tail(relationships(p))))) + count(label(last(tail(nodes(p))))) + count(properties(last(tail(nodes(p))))) + count(type(last(tail(relationships(p))))) + count(labels(endNode(last(tail(relationships(p)))))) + count(properties(startNode(last(tail(relationships(p))))))$$)
AS (plan agtype);

SELECT * FROM cypher('cypher_vle',
                    $$EXPLAIN (VERBOSE, COSTS OFF) MATCH ()-[e*2..3]->() RETURN count(last(tail(reverse(e)))) + count(head(reverse(tail(e)))) + count(last(tail(tail(e)))) + count(type(last(tail(reverse(e))))) + count(properties(head(reverse(tail(e)))))$$)
AS (plan agtype);

SELECT * FROM cypher('cypher_vle',
                    $$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=()-[*1..2]->() RETURN p$$)
AS (plan agtype);

SELECT * FROM cypher('cypher_vle',
                    $$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=(:begin)-[:edge*1..2]->() RETURN p$$)
AS (plan agtype);

SELECT * FROM cypher('cypher_vle',
                    $$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=()-[*1..2]->() RETURN relationships(p)[0]$$)
AS (plan agtype);

SELECT * FROM cypher('cypher_vle',
                    $$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=()-[*1..2]->() RETURN head(relationships(p))$$)
AS (plan agtype);

SELECT * FROM cypher('cypher_vle',
                    $$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=()-[*1..2]->() RETURN last(relationships(p))$$)
AS (plan agtype);

SELECT * FROM cypher('cypher_vle',
                    $$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=()-[*1..2]->() RETURN head(nodes(p))$$)
AS (plan agtype);

SELECT * FROM cypher('cypher_vle',
                    $$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=()-[*1..2]->() RETURN last(nodes(p)).id$$)
AS (plan agtype);

SELECT * FROM cypher('cypher_vle',
                    $$EXPLAIN (VERBOSE, COSTS OFF) MATCH p=()-[*1..2]->() RETURN last(relationships(p)).id$$)
AS (plan agtype);

--
-- Clean up
--

SELECT drop_graph('cypher_vle', true);

--
-- End
--
