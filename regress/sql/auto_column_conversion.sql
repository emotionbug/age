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

SELECT create_graph('auto_column_conversion');

SELECT * FROM cypher('auto_column_conversion',
                    $$RETURN true, 'AGE', 2022$$) AS (ignore age_auto_column);

SELECT * FROM cypher('auto_column_conversion',
                    $$RETURN 42 AS answer$$) AS (signature age_auto_column);

SELECT * FROM cypher('auto_column_conversion', $$
    WITH true AS b
    RETURN 'AGE', b, 'AGE2'
$$) AS (any_name age_auto_column);

SELECT *
FROM cypher('auto_column_conversion', $$
    CREATE (:Person {name: 'Ada'})-[:KNOWS {since: 2022}]->(:Person {name: 'Bob'})
$$) AS (signature agtype);

SELECT * FROM cypher('auto_column_conversion', $$
    MATCH (a:Person)-[e:KNOWS]->(b:Person)
    RETURN a.name AS from_name, e.since AS since, b.name AS to_name
$$) AS (signature age_auto_column);

SELECT count(*) FROM cypher('auto_column_conversion', $$
    MATCH (n:Person)
    RETURN n.name AS name, label(n) AS label
$$) AS (signature age_auto_column);

SELECT *
FROM (
    SELECT * FROM cypher('auto_column_conversion', $$
        RETURN 7 AS n, 'seven' AS word
    $$) AS (signature age_auto_column)
) AS expanded;

SELECT * FROM cypher('auto_column_conversion',
                    $$RETURN true, 'AGE', 2022$$) AS (ignore bool);

SELECT * FROM cypher('auto_column_conversion', $$
    RETURN 1, 2, 3
$$) AS (a agtype, b agtype);

SELECT drop_graph('auto_column_conversion', true);
