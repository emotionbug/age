\set ON_ERROR_STOP on

-- Blocking, in-place migration for the historical AGE endpoint indexes.
--
-- For every edge label, replace the default one-column indexes
--
--     (start_id) and (end_id)
--
-- with endpoint-pair indexes that retain the historical names:
--
--     (start_id, end_id) and (end_id, start_id)
--
-- CREATE INDEX takes a lock that blocks writes to each label table.  Use the
-- concurrent companion script when that lock is not acceptable.

LOAD 'age';
SET search_path TO ag_catalog, public;

DO $endpoint_pair_migration$
DECLARE
    edge record;
    start_attnum smallint;
    end_attnum smallint;
    start_name text;
    end_name text;
    old_index_name text;
    replacement_name text;
    pair_exists boolean;
BEGIN
    FOR edge IN
        SELECT l.relation::oid AS relid,
               n.nspname AS schema_name,
               c.relname AS relation_name
        FROM ag_catalog.ag_label AS l
        JOIN pg_catalog.pg_class AS c ON c.oid = l.relation
        JOIN pg_catalog.pg_namespace AS n ON n.oid = c.relnamespace
        WHERE l.kind = 'e'
        ORDER BY n.nspname, c.relname
    LOOP
        SELECT a.attnum
        INTO start_attnum
        FROM pg_catalog.pg_attribute AS a
        WHERE a.attrelid = edge.relid
          AND a.attname = 'start_id'
          AND NOT a.attisdropped;

        SELECT a.attnum
        INTO end_attnum
        FROM pg_catalog.pg_attribute AS a
        WHERE a.attrelid = edge.relid
          AND a.attname = 'end_id'
          AND NOT a.attisdropped;

        IF start_attnum IS NULL OR end_attnum IS NULL THEN
            RAISE WARNING 'skipping %.%: endpoint columns are missing',
                          edge.schema_name, edge.relation_name;
            CONTINUE;
        END IF;

        -- Match PostgreSQL's historical <relation>_start_id_idx naming for
        -- ordinary label names while remaining within NAMEDATALEN.
        start_name := left(edge.relation_name,
                           63 - length('_start_id_idx')) || '_start_id_idx';
        end_name := left(edge.relation_name,
                         63 - length('_end_id_idx')) || '_end_id_idx';

        SELECT EXISTS (
            SELECT 1
            FROM pg_catalog.pg_index AS i
            JOIN pg_catalog.pg_class AS ic ON ic.oid = i.indexrelid
            JOIN pg_catalog.pg_am AS am ON am.oid = ic.relam
            WHERE i.indrelid = edge.relid
              AND i.indisvalid
              AND i.indisready
              AND i.indpred IS NULL
              AND i.indexprs IS NULL
              AND am.amname = 'btree'
              AND i.indnkeyatts >= 2
              AND i.indkey[0] = start_attnum
              AND i.indkey[1] = end_attnum)
        INTO pair_exists;

        IF NOT pair_exists THEN
            SELECT ic.relname
            INTO old_index_name
            FROM pg_catalog.pg_index AS i
            JOIN pg_catalog.pg_class AS ic ON ic.oid = i.indexrelid
            JOIN pg_catalog.pg_am AS am ON am.oid = ic.relam
            WHERE i.indrelid = edge.relid
              AND ic.relname = start_name
              AND i.indpred IS NULL
              AND i.indexprs IS NULL
              AND am.amname = 'btree'
              AND i.indnkeyatts = 1
              AND i.indkey[0] = start_attnum
            LIMIT 1;

            replacement_name := start_name;
            IF old_index_name IS NOT NULL THEN
                EXECUTE format('DROP INDEX %I.%I', edge.schema_name,
                               old_index_name);
            ELSIF to_regclass(format('%I.%I', edge.schema_name,
                                    replacement_name)) IS NOT NULL THEN
                replacement_name := left(edge.relation_name, 40) ||
                    '_age_se_' || substr(md5(edge.schema_name || '.' ||
                                             edge.relation_name), 1, 8);
            END IF;

            EXECUTE format(
                'CREATE INDEX %I ON %I.%I USING btree (start_id, end_id)',
                replacement_name, edge.schema_name, edge.relation_name);
            RAISE NOTICE 'created %.% on %.% (start_id, end_id)',
                         edge.schema_name, replacement_name,
                         edge.schema_name, edge.relation_name;
        END IF;

        SELECT EXISTS (
            SELECT 1
            FROM pg_catalog.pg_index AS i
            JOIN pg_catalog.pg_class AS ic ON ic.oid = i.indexrelid
            JOIN pg_catalog.pg_am AS am ON am.oid = ic.relam
            WHERE i.indrelid = edge.relid
              AND i.indisvalid
              AND i.indisready
              AND i.indpred IS NULL
              AND i.indexprs IS NULL
              AND am.amname = 'btree'
              AND i.indnkeyatts >= 2
              AND i.indkey[0] = end_attnum
              AND i.indkey[1] = start_attnum)
        INTO pair_exists;

        IF NOT pair_exists THEN
            SELECT ic.relname
            INTO old_index_name
            FROM pg_catalog.pg_index AS i
            JOIN pg_catalog.pg_class AS ic ON ic.oid = i.indexrelid
            JOIN pg_catalog.pg_am AS am ON am.oid = ic.relam
            WHERE i.indrelid = edge.relid
              AND ic.relname = end_name
              AND i.indpred IS NULL
              AND i.indexprs IS NULL
              AND am.amname = 'btree'
              AND i.indnkeyatts = 1
              AND i.indkey[0] = end_attnum
            LIMIT 1;

            replacement_name := end_name;
            IF old_index_name IS NOT NULL THEN
                EXECUTE format('DROP INDEX %I.%I', edge.schema_name,
                               old_index_name);
            ELSIF to_regclass(format('%I.%I', edge.schema_name,
                                    replacement_name)) IS NOT NULL THEN
                replacement_name := left(edge.relation_name, 40) ||
                    '_age_es_' || substr(md5(edge.schema_name || '.' ||
                                             edge.relation_name), 1, 8);
            END IF;

            EXECUTE format(
                'CREATE INDEX %I ON %I.%I USING btree (end_id, start_id)',
                replacement_name, edge.schema_name, edge.relation_name);
            RAISE NOTICE 'created %.% on %.% (end_id, start_id)',
                         edge.schema_name, replacement_name,
                         edge.schema_name, edge.relation_name;
        END IF;
    END LOOP;
END
$endpoint_pair_migration$;
