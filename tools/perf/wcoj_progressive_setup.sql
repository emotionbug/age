\set ON_ERROR_STOP on
\if :{?sources}
\else
\set sources 8192
\endif
\if :{?fanout}
\else
\set fanout 32
\endif

LOAD 'age';
SET search_path = ag_catalog, "$user", public;

DROP TABLE IF EXISTS age_wcoj_progressive_sparse_args;
DROP TABLE IF EXISTS age_wcoj_progressive_dense_args;
SELECT drop_graph('age_wcoj_progressive_sparse', true)
WHERE EXISTS (
    SELECT 1 FROM ag_catalog.ag_graph
    WHERE name = 'age_wcoj_progressive_sparse');
SELECT drop_graph('age_wcoj_progressive_dense', true)
WHERE EXISTS (
    SELECT 1 FROM ag_catalog.ag_graph
    WHERE name = 'age_wcoj_progressive_dense');

SELECT create_graph('age_wcoj_progressive_sparse');
SELECT create_vlabel('age_wcoj_progressive_sparse', 'N');
SELECT create_elabel('age_wcoj_progressive_sparse', 'E');

-- Every source has fanout private destinations.  The intersection is empty,
-- so a progressive executor can stop after the second source instead of
-- consuming all sources * fanout postings.
INSERT INTO age_wcoj_progressive_sparse."E"
       (id, start_id, end_id, properties)
SELECT _graphid(_label_id('age_wcoj_progressive_sparse', 'E'),
                (round_no - 1) * :sources + source_no),
       _graphid(_label_id('age_wcoj_progressive_sparse', 'N'), source_no),
       _graphid(_label_id('age_wcoj_progressive_sparse', 'N'),
                :sources + (round_no - 1) * :sources + source_no),
       '{}'::agtype
FROM generate_series(1, :sources) source_no,
     generate_series(1, :fanout) round_no;

CREATE INDEX age_wcoj_progressive_sparse_e_start_idx
ON age_wcoj_progressive_sparse."E"
USING age_adjacency (start_id, id, end_id);

CREATE TABLE age_wcoj_progressive_sparse_args AS
SELECT ('[' || string_agg(
                    _graphid(_label_id(
                        'age_wcoj_progressive_sparse', 'N'), source_no)::text,
                    ',' ORDER BY source_no) || ']')::agtype AS source_ids
FROM generate_series(1, :sources) source_no;

SELECT create_graph('age_wcoj_progressive_dense');
SELECT create_vlabel('age_wcoj_progressive_dense', 'N');
SELECT create_elabel('age_wcoj_progressive_dense', 'E');

-- Every source has the same fanout destinations.  This pins the adaptive
-- dense-overlap fallback to the streaming global merge.
INSERT INTO age_wcoj_progressive_dense."E"
       (id, start_id, end_id, properties)
SELECT _graphid(_label_id('age_wcoj_progressive_dense', 'E'),
                (source_no - 1) * :fanout + target_no),
       _graphid(_label_id('age_wcoj_progressive_dense', 'N'), source_no),
       _graphid(_label_id('age_wcoj_progressive_dense', 'N'),
                :sources + target_no),
       '{}'::agtype
FROM generate_series(1, :sources) source_no,
     generate_series(1, :fanout) target_no;

CREATE INDEX age_wcoj_progressive_dense_e_start_idx
ON age_wcoj_progressive_dense."E"
USING age_adjacency (start_id, id, end_id);

CREATE TABLE age_wcoj_progressive_dense_args AS
SELECT ('[' || string_agg(
                    _graphid(_label_id(
                        'age_wcoj_progressive_dense', 'N'), source_no)::text,
                    ',' ORDER BY source_no) || ']')::agtype AS source_ids
FROM generate_series(1, :sources) source_no;

ANALYZE age_wcoj_progressive_sparse."E";
ANALYZE age_wcoj_progressive_dense."E";
CHECKPOINT;
