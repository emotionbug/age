\set ON_ERROR_STOP on
\if :{?star_sources}
\else
\set star_sources 4
\endif
\if :{?star_fanout}
\else
\set star_fanout 4096
\endif
\if :{?cycle_vertices}
\else
\set cycle_vertices 1024
\endif
\if :{?cycle_fanout}
\else
\set cycle_fanout 64
\endif

LOAD 'age';
SET search_path = ag_catalog, "$user", public;
SET client_min_messages = warning;

SELECT drop_graph('wcoj_bench_sparse', true)
WHERE EXISTS (SELECT 1 FROM ag_graph WHERE name='wcoj_bench_sparse');
SELECT create_graph('wcoj_bench_sparse');
SELECT create_vlabel('wcoj_bench_sparse','S');
SELECT create_vlabel('wcoj_bench_sparse','T');
SELECT create_elabel('wcoj_bench_sparse','E');
INSERT INTO wcoj_bench_sparse."S" (id, properties)
SELECT _graphid(_label_id('wcoj_bench_sparse','S'), i),
       format('{"id":%s}', i)::agtype
FROM generate_series(1, :star_sources) i;
INSERT INTO wcoj_bench_sparse."T" (id, properties)
SELECT _graphid(_label_id('wcoj_bench_sparse','T'), i), '{}'::agtype
FROM generate_series(1, 1 + :star_sources * (:star_fanout - 1)) i;
INSERT INTO wcoj_bench_sparse."E" (id, start_id, end_id, properties)
SELECT _graphid(_label_id('wcoj_bench_sparse','E'),
                (source_no - 1) * :star_fanout + ordinal),
       _graphid(_label_id('wcoj_bench_sparse','S'), source_no),
       _graphid(_label_id('wcoj_bench_sparse','T'),
                CASE WHEN ordinal = 1 THEN 1
                     ELSE 2 + (source_no - 1) * (:star_fanout - 1) +
                              (ordinal - 2)
                END),
       '{}'::agtype
FROM generate_series(1, :star_sources) source_no,
     generate_series(1, :star_fanout) ordinal;
CREATE INDEX wcoj_bench_sparse_e_out
ON wcoj_bench_sparse."E" USING age_adjacency(start_id,id,end_id);
ANALYZE wcoj_bench_sparse."S";
ANALYZE wcoj_bench_sparse."T";
ANALYZE wcoj_bench_sparse."E";

SELECT drop_graph('wcoj_bench_dense', true)
WHERE EXISTS (SELECT 1 FROM ag_graph WHERE name='wcoj_bench_dense');
SELECT create_graph('wcoj_bench_dense');
SELECT create_vlabel('wcoj_bench_dense','S');
SELECT create_vlabel('wcoj_bench_dense','T');
SELECT create_elabel('wcoj_bench_dense','E');
INSERT INTO wcoj_bench_dense."S" (id, properties)
SELECT _graphid(_label_id('wcoj_bench_dense','S'), i),
       format('{"id":%s}', i)::agtype
FROM generate_series(1, :star_sources) i;
INSERT INTO wcoj_bench_dense."T" (id, properties)
SELECT _graphid(_label_id('wcoj_bench_dense','T'), i), '{}'::agtype
FROM generate_series(1, :star_fanout) i;
INSERT INTO wcoj_bench_dense."E" (id, start_id, end_id, properties)
SELECT _graphid(_label_id('wcoj_bench_dense','E'),
                (source_no - 1) * :star_fanout + target_no),
       _graphid(_label_id('wcoj_bench_dense','S'), source_no),
       _graphid(_label_id('wcoj_bench_dense','T'), target_no),
       '{}'::agtype
FROM generate_series(1, :star_sources) source_no,
     generate_series(1, :star_fanout) target_no;
CREATE INDEX wcoj_bench_dense_e_out
ON wcoj_bench_dense."E" USING age_adjacency(start_id,id,end_id);
ANALYZE wcoj_bench_dense."S";
ANALYZE wcoj_bench_dense."T";
ANALYZE wcoj_bench_dense."E";

SELECT drop_graph('wcoj_bench_cycle', true)
WHERE EXISTS (SELECT 1 FROM ag_graph WHERE name='wcoj_bench_cycle');
SELECT create_graph('wcoj_bench_cycle');
SELECT create_vlabel('wcoj_bench_cycle','A');
SELECT create_vlabel('wcoj_bench_cycle','B');
SELECT create_vlabel('wcoj_bench_cycle','C');
SELECT create_elabel('wcoj_bench_cycle','E1');
SELECT create_elabel('wcoj_bench_cycle','E2');
SELECT create_elabel('wcoj_bench_cycle','E3');
INSERT INTO wcoj_bench_cycle."A" (id, properties)
SELECT _graphid(_label_id('wcoj_bench_cycle','A'), i), '{}'::agtype
FROM generate_series(1, :cycle_vertices) i;
INSERT INTO wcoj_bench_cycle."B" (id, properties)
SELECT _graphid(_label_id('wcoj_bench_cycle','B'), i), '{}'::agtype
FROM generate_series(1, :cycle_vertices) i;
INSERT INTO wcoj_bench_cycle."C" (id, properties)
SELECT _graphid(_label_id('wcoj_bench_cycle','C'), i), '{}'::agtype
FROM generate_series(1, :cycle_vertices) i;
-- E1 can start only in the lower half of A.
INSERT INTO wcoj_bench_cycle."E1" (id,start_id,end_id,properties)
SELECT _graphid(_label_id('wcoj_bench_cycle','E1'),
                (a - 1) * :cycle_fanout + ordinal),
       _graphid(_label_id('wcoj_bench_cycle','A'), a),
       _graphid(_label_id('wcoj_bench_cycle','B'),
                1 + ((a - 1) * :cycle_fanout + ordinal - 1) % :cycle_vertices),
       '{}'::agtype
FROM generate_series(1, :cycle_vertices / 2) a,
     generate_series(1, :cycle_fanout) ordinal;
INSERT INTO wcoj_bench_cycle."E2" (id,start_id,end_id,properties)
SELECT _graphid(_label_id('wcoj_bench_cycle','E2'),
                (b - 1) * :cycle_fanout + ordinal),
       _graphid(_label_id('wcoj_bench_cycle','B'), b),
       _graphid(_label_id('wcoj_bench_cycle','C'),
                1 + ((b - 1) * :cycle_fanout + ordinal - 1) % :cycle_vertices),
       '{}'::agtype
FROM generate_series(1, :cycle_vertices) b,
     generate_series(1, :cycle_fanout) ordinal;
-- E3 can end only in the upper half of A, so the cycle result is empty.
INSERT INTO wcoj_bench_cycle."E3" (id,start_id,end_id,properties)
SELECT _graphid(_label_id('wcoj_bench_cycle','E3'),
                (c - 1) * :cycle_fanout + ordinal),
       _graphid(_label_id('wcoj_bench_cycle','C'), c),
       _graphid(_label_id('wcoj_bench_cycle','A'),
                1 + :cycle_vertices / 2 +
                ((c - 1) * :cycle_fanout + ordinal - 1) %
                (:cycle_vertices / 2)),
       '{}'::agtype
FROM generate_series(1, :cycle_vertices) c,
     generate_series(1, :cycle_fanout) ordinal;
CREATE INDEX wcoj_bench_cycle_e1_out
ON wcoj_bench_cycle."E1" USING age_adjacency(start_id,id,end_id);
CREATE INDEX wcoj_bench_cycle_e2_out
ON wcoj_bench_cycle."E2" USING age_adjacency(start_id,id,end_id);
CREATE INDEX wcoj_bench_cycle_e3_out
ON wcoj_bench_cycle."E3" USING age_adjacency(start_id,id,end_id);
ANALYZE wcoj_bench_cycle."A";
ANALYZE wcoj_bench_cycle."B";
ANALYZE wcoj_bench_cycle."C";
ANALYZE wcoj_bench_cycle."E1";
ANALYZE wcoj_bench_cycle."E2";
ANALYZE wcoj_bench_cycle."E3";
CHECKPOINT;
