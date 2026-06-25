\set ON_ERROR_STOP on

-- Online/additive alternative to rebuild_endpoint_pair_indexes.sql.
--
-- This retains the historical scalar endpoint indexes and creates two
-- endpoint-pair B-trees under deterministic auxiliary names.  Each CREATE
-- INDEX CONCURRENTLY runs as a top-level psql command via \gexec.

LOAD 'age';
SET search_path TO ag_catalog, public;

SELECT format(
           'CREATE INDEX CONCURRENTLY %I ON %I.%I USING btree (start_id, end_id)',
           left(c.relname, 40) || '_age_se_' ||
               substr(md5(n.nspname || '.' || c.relname || ':se'), 1, 8),
           n.nspname, c.relname)
FROM ag_catalog.ag_label AS l
JOIN pg_catalog.pg_class AS c ON c.oid = l.relation
JOIN pg_catalog.pg_namespace AS n ON n.oid = c.relnamespace
JOIN pg_catalog.pg_attribute AS sa
  ON sa.attrelid = c.oid AND sa.attname = 'start_id' AND NOT sa.attisdropped
JOIN pg_catalog.pg_attribute AS ea
  ON ea.attrelid = c.oid AND ea.attname = 'end_id' AND NOT ea.attisdropped
WHERE l.kind = 'e'
  AND NOT EXISTS (
      SELECT 1
      FROM pg_catalog.pg_index AS i
      JOIN pg_catalog.pg_class AS ic ON ic.oid = i.indexrelid
      JOIN pg_catalog.pg_am AS am ON am.oid = ic.relam
      WHERE i.indrelid = c.oid
        AND i.indisvalid
        AND i.indisready
        AND i.indpred IS NULL
        AND i.indexprs IS NULL
        AND am.amname = 'btree'
        AND i.indnkeyatts >= 2
        AND i.indkey[0] = sa.attnum
        AND i.indkey[1] = ea.attnum)
ORDER BY n.nspname, c.relname
\gexec

SELECT format(
           'CREATE INDEX CONCURRENTLY %I ON %I.%I USING btree (end_id, start_id)',
           left(c.relname, 40) || '_age_es_' ||
               substr(md5(n.nspname || '.' || c.relname || ':es'), 1, 8),
           n.nspname, c.relname)
FROM ag_catalog.ag_label AS l
JOIN pg_catalog.pg_class AS c ON c.oid = l.relation
JOIN pg_catalog.pg_namespace AS n ON n.oid = c.relnamespace
JOIN pg_catalog.pg_attribute AS sa
  ON sa.attrelid = c.oid AND sa.attname = 'start_id' AND NOT sa.attisdropped
JOIN pg_catalog.pg_attribute AS ea
  ON ea.attrelid = c.oid AND ea.attname = 'end_id' AND NOT ea.attisdropped
WHERE l.kind = 'e'
  AND NOT EXISTS (
      SELECT 1
      FROM pg_catalog.pg_index AS i
      JOIN pg_catalog.pg_class AS ic ON ic.oid = i.indexrelid
      JOIN pg_catalog.pg_am AS am ON am.oid = ic.relam
      WHERE i.indrelid = c.oid
        AND i.indisvalid
        AND i.indisready
        AND i.indpred IS NULL
        AND i.indexprs IS NULL
        AND am.amname = 'btree'
        AND i.indnkeyatts >= 2
        AND i.indkey[0] = ea.attnum
        AND i.indkey[1] = sa.attnum)
ORDER BY n.nspname, c.relname
\gexec
