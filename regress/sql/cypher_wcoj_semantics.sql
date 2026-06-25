/*
 * Cyclic / fork fixed-MATCH semantics lock-in for the WCOJ-style multiway
 * intersection work (TODO item 6).  These results are the correctness contract
 * a future multiway-intersection executor MUST preserve: the binary-join plan
 * and a WCOJ plan have to return identical rows.  Scalars are returned (not
 * whole vertices) so the expected output is stable across runs.
 */
LOAD 'age';
SET search_path = ag_catalog, public;

SELECT create_graph('wcoj_semantics');
SELECT create_vlabel('wcoj_semantics', 'N');
SELECT create_elabel('wcoj_semantics', 'E');

-- Triangle 1->2->3->1, with a dangling tail 3->4 and a fork target 2->5.
SELECT * FROM cypher('wcoj_semantics', $$
    CREATE (a:N {id: 1})-[:E]->(b:N {id: 2})-[:E]->(c:N {id: 3})-[:E]->(a),
           (c)-[:E]->(:N {id: 4}),
           (b)-[:E]->(:N {id: 5})
$$) AS (z agtype);

-- Triangle: a directed 3-cycle closing back to the start variable enumerates
-- each rotation once (the cycle is matched from every vertex).
SELECT a, b, c FROM cypher('wcoj_semantics', $$
    MATCH (a:N)-[:E]->(b:N)-[:E]->(c:N)-[:E]->(a)
    RETURN a.id AS a, b.id AS b, c.id AS c
$$) AS (a agtype, b agtype, c agtype)
ORDER BY a, b, c;

-- Fork: one vertex with two outgoing edges yields the unordered pair of its
-- distinct targets (b.id < c.id de-duplicates the symmetric match).
SELECT a, b, c FROM cypher('wcoj_semantics', $$
    MATCH (b:N)<-[:E]-(a:N)-[:E]->(c:N)
    WHERE b.id < c.id
    RETURN a.id AS a, b.id AS b, c.id AS c
$$) AS (a agtype, b agtype, c agtype)
ORDER BY a, b, c;

-- Relationship uniqueness: within one MATCH the same edge may not be reused, so
-- a two-hop pattern never returns a path that traverses a single edge twice.
SELECT a, b, c FROM cypher('wcoj_semantics', $$
    MATCH (a:N)-[r1:E]->(b:N)-[r2:E]->(c:N)
    RETURN a.id AS a, b.id AS b, c.id AS c
$$) AS (a agtype, b agtype, c agtype)
ORDER BY a, b, c;

-- Diamond / two-path-between-endpoints: count the distinct intermediate fan.
SELECT total FROM cypher('wcoj_semantics', $$
    MATCH (a:N {id: 1})-[:E]->(b:N)-[:E]->(c:N)
    RETURN count(*) AS total
$$) AS (total agtype);

SELECT drop_graph('wcoj_semantics', true);

-- A second graph for the 4-cycle and diamond patterns the gate also calls out,
-- kept separate so the triangle/fork results above stay isolated.  A 4-cycle
-- 10->11->12->13->10 and a diamond where 1 reaches 4 by two distinct two-hop
-- paths (1->2->4 and 1->3->4).
SELECT create_graph('wcoj_semantics_more');
SELECT create_vlabel('wcoj_semantics_more', 'N');
SELECT create_elabel('wcoj_semantics_more', 'E');
SELECT * FROM cypher('wcoj_semantics_more', $$
    CREATE (n10:N {id: 10})-[:E]->(n11:N {id: 11})-[:E]->(n12:N {id: 12})
                          -[:E]->(n13:N {id: 13})-[:E]->(n10)
$$) AS (z agtype);
SELECT * FROM cypher('wcoj_semantics_more', $$
    CREATE (a:N {id: 1})-[:E]->(:N {id: 2}), (a)-[:E]->(:N {id: 3}),
           (:N {id: 4})
$$) AS (z agtype);
SELECT * FROM cypher('wcoj_semantics_more', $$
    MATCH (b:N), (d:N {id: 4})
    WHERE b.id IN [2, 3]
    CREATE (b)-[:E]->(d)
$$) AS (z agtype);

-- 4-cycle: a directed 4-cycle closing to the start variable enumerates each of
-- its four rotations once.
SELECT a, b, c, d FROM cypher('wcoj_semantics_more', $$
    MATCH (a:N)-[:E]->(b:N)-[:E]->(c:N)-[:E]->(e:N)-[:E]->(a)
    RETURN a.id AS a, b.id AS b, c.id AS c, e.id AS d
$$) AS (a agtype, b agtype, c agtype, d agtype)
ORDER BY a, b, c, d;

-- Diamond: two distinct two-hop paths from :id 1 to :id 4 (via 2 and via 3),
-- so the closed pattern returns both intermediates.
SELECT mid FROM cypher('wcoj_semantics_more', $$
    MATCH (a:N {id: 1})-[:E]->(m:N)-[:E]->(d:N {id: 4})
    RETURN m.id AS mid
$$) AS (mid agtype)
ORDER BY mid;

SELECT drop_graph('wcoj_semantics_more', true);
