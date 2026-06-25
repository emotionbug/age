/*
 * VLE follow-up coverage for boundary and dense-state behavior.
 */

LOAD 'age';
SET search_path TO ag_catalog;
\pset format unaligned

SELECT create_graph('cypher_vle_followup');

SELECT * FROM cypher('cypher_vle_followup', $$
    CREATE
        (a:Node {name: 'a'}),
        (b:Node {name: 'b'}),
        (c:Node {name: 'c'}),
        (d:Node {name: 'd'}),
        (a)-[:E {kind: 'chain'}]->(b),
        (b)-[:E {kind: 'chain'}]->(c),
        (c)-[:E {kind: 'chain'}]->(d),
        (c)-[:E {kind: 'back'}]->(b),
        (b)-[:E {kind: 'self'}]->(b),
        (a)-[:D {bucket: 0}]->(c),
        (a)-[:D {bucket: 1}]->(d),
        (b)-[:D {bucket: 0}]->(d),
        (c)-[:D {bucket: 1}]->(a),
        (rs:Node {name: 'rs'}),
        (rh:Node {name: 'rh'}),
        (rm0:Node {name: 'rm0'}),
        (rm1:Node {name: 'rm1'}),
        (rm2:Node {name: 'rm2'}),
        (rm3:Node {name: 'rm3'}),
        (rl0:Node {name: 'rl0'}),
        (rl1:Node {name: 'rl1'}),
        (rl2:Node {name: 'rl2'}),
        (rl3:Node {name: 'rl3'}),
        (ws:Node {name: 'ws'}),
        (rs)-[:E {kind: 'replay'}]->(rm0),
        (rs)-[:E {kind: 'replay'}]->(rm1),
        (rs)-[:E {kind: 'replay'}]->(rm2),
        (rs)-[:E {kind: 'replay'}]->(rm3),
        (rm0)-[:E {kind: 'replay'}]->(rh),
        (rm1)-[:E {kind: 'replay'}]->(rh),
        (rm2)-[:E {kind: 'replay'}]->(rh),
        (rm3)-[:E {kind: 'replay'}]->(rh),
        (rh)-[:E {kind: 'replay'}]->(rl0),
        (rh)-[:E {kind: 'replay'}]->(rl1),
        (rh)-[:E {kind: 'replay'}]->(rl2),
        (rh)-[:E {kind: 'replay'}]->(rl3)
$$) AS (r agtype);

-- Build a frontier wider than the initial worklist capacity and keep the
-- queue populated while depth-two work is appended.
SELECT * FROM cypher('cypher_vle_followup', $$
    MATCH (s:Node {name: 'ws'})
    UNWIND range(0, 79) AS i
    CREATE (s)-[:E {kind: 'wide'}]->(:Node {name: 'wm', i: i})
              -[:E {kind: 'wide'}]->(:Node {name: 'wl', i: i})
$$) AS (r agtype);

-- The level-batch iterator must preserve every terminal while its consumed
-- prefix is compacted and reused for newly appended work.
SELECT * FROM cypher('cypher_vle_followup', $$
    MATCH (:Node {name: 'ws'})-[:E*1..2 {kind: 'wide'}]->(n)
    RETURN count(n.i)
$$) AS (terminals agtype);

-- Reversed two-bound traversal should find the reverse chain and back edge.
SELECT * FROM cypher('cypher_vle_followup', $$
    MATCH p=(:Node {name: 'd'})<-[*2..3]-(:Node {name: 'a'})
    RETURN count(p)
$$) AS (paths agtype);

-- Zero-length two-bound traversal emits only when both endpoints are identical.
SELECT * FROM cypher('cypher_vle_followup', $$
    MATCH p=(:Node {name: 'a'})-[*0..0]->(:Node {name: 'a'})
    RETURN count(p)
$$) AS (paths agtype);

SELECT * FROM cypher('cypher_vle_followup', $$
    MATCH p=(:Node {name: 'a'})-[*0..0]->(:Node {name: 'b'})
    RETURN count(p)
$$) AS (paths agtype);

-- Unbound-start VLE with a missing endpoint should return no rows.
SELECT * FROM cypher('cypher_vle_followup', $$
    MATCH p=()-[*1..2]->(:Missing {name: 'nope'})
    RETURN count(p)
$$) AS (paths agtype);

-- Dense edge-index traversal state should preserve branch uniqueness.
SELECT * FROM cypher('cypher_vle_followup', $$
    MATCH p=(:Node {name: 'a'})-[:D*1..2]->()
    RETURN count(p)
$$) AS (paths agtype);

-- Reconvergent branches must retain every level-batch path and terminal row.
SELECT * FROM cypher('cypher_vle_followup', $$
    MATCH p=(:Node {name: 'rs'})-[:E*1..3 {kind: 'replay'}]->(n)
    RETURN count(p), count(n.name)
$$) AS (paths agtype, terminals agtype);

SELECT drop_graph('cypher_vle_followup', true);

\pset format aligned
