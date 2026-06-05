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
        (c)-[:D {bucket: 1}]->(a)
$$) AS (r agtype);

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

SELECT drop_graph('cypher_vle_followup', true);

\pset format aligned
