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

#include "postgres.h"

#include "access/amapi.h"
#include "access/genam.h"
#include "fmgr.h"
#include "nodes/nodes.h"

PG_FUNCTION_INFO_V1(age_adjacency_handler);

static void age_adjacency_unsupported(const char *callback_name);
static IndexBuildResult *age_adjacency_build(Relation heap_rel,
                                             Relation index_rel,
                                             struct IndexInfo *index_info);
static void age_adjacency_build_empty(Relation index_rel);
static bool age_adjacency_insert(Relation index_rel, Datum *values,
                                 bool *isnull, ItemPointer heap_tid,
                                 Relation heap_rel,
                                 IndexUniqueCheck check_unique,
                                 bool index_unchanged,
                                 struct IndexInfo *index_info);
static IndexBulkDeleteResult *age_adjacency_bulk_delete(IndexVacuumInfo *info,
                                                        IndexBulkDeleteResult *stats,
                                                        IndexBulkDeleteCallback callback,
                                                        void *callback_state);
static IndexBulkDeleteResult *age_adjacency_vacuum_cleanup(IndexVacuumInfo *info,
                                                           IndexBulkDeleteResult *stats);
static void age_adjacency_cost_estimate(struct PlannerInfo *root,
                                        struct IndexPath *path,
                                        double loop_count,
                                        Cost *index_startup_cost,
                                        Cost *index_total_cost,
                                        Selectivity *index_selectivity,
                                        double *index_correlation,
                                        double *index_pages);
static bytea *age_adjacency_options(Datum reloptions, bool validate);
static bool age_adjacency_validate(Oid opclass_oid);
static IndexScanDesc age_adjacency_begin_scan(Relation index_rel, int nkeys,
                                              int norderbys);
static void age_adjacency_rescan(IndexScanDesc scan, ScanKey keys, int nkeys,
                                 ScanKey orderbys, int norderbys);
static void age_adjacency_end_scan(IndexScanDesc scan);

static void
age_adjacency_unsupported(const char *callback_name)
{
    ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
             errmsg("age_adjacency index access method does not yet support %s",
                    callback_name)));
}

static IndexBuildResult *
age_adjacency_build(Relation heap_rel, Relation index_rel,
                    struct IndexInfo *index_info)
{
    (void) heap_rel;
    (void) index_rel;
    (void) index_info;

    age_adjacency_unsupported("building indexes");
    return NULL;
}

static void
age_adjacency_build_empty(Relation index_rel)
{
    (void) index_rel;

    age_adjacency_unsupported("building empty indexes");
}

static bool
age_adjacency_insert(Relation index_rel, Datum *values, bool *isnull,
                     ItemPointer heap_tid, Relation heap_rel,
                     IndexUniqueCheck check_unique, bool index_unchanged,
                     struct IndexInfo *index_info)
{
    (void) index_rel;
    (void) values;
    (void) isnull;
    (void) heap_tid;
    (void) heap_rel;
    (void) check_unique;
    (void) index_unchanged;
    (void) index_info;

    age_adjacency_unsupported("inserting index tuples");
    return false;
}

static IndexBulkDeleteResult *
age_adjacency_bulk_delete(IndexVacuumInfo *info, IndexBulkDeleteResult *stats,
                          IndexBulkDeleteCallback callback,
                          void *callback_state)
{
    (void) info;
    (void) callback;
    (void) callback_state;

    age_adjacency_unsupported("bulk delete");
    return stats;
}

static IndexBulkDeleteResult *
age_adjacency_vacuum_cleanup(IndexVacuumInfo *info,
                             IndexBulkDeleteResult *stats)
{
    (void) info;

    age_adjacency_unsupported("vacuum cleanup");
    return stats;
}

static void
age_adjacency_cost_estimate(struct PlannerInfo *root, struct IndexPath *path,
                            double loop_count, Cost *index_startup_cost,
                            Cost *index_total_cost,
                            Selectivity *index_selectivity,
                            double *index_correlation, double *index_pages)
{
    (void) root;
    (void) path;
    (void) loop_count;
    (void) index_startup_cost;
    (void) index_total_cost;
    (void) index_selectivity;
    (void) index_correlation;
    (void) index_pages;

    age_adjacency_unsupported("planner cost estimation");
}

static bytea *
age_adjacency_options(Datum reloptions, bool validate)
{
    (void) reloptions;
    (void) validate;

    return NULL;
}

static bool
age_adjacency_validate(Oid opclass_oid)
{
    (void) opclass_oid;

    return true;
}

static IndexScanDesc
age_adjacency_begin_scan(Relation index_rel, int nkeys, int norderbys)
{
    (void) index_rel;
    (void) nkeys;
    (void) norderbys;

    age_adjacency_unsupported("scanning indexes");
    return NULL;
}

static void
age_adjacency_rescan(IndexScanDesc scan, ScanKey keys, int nkeys,
                     ScanKey orderbys, int norderbys)
{
    (void) scan;
    (void) keys;
    (void) nkeys;
    (void) orderbys;
    (void) norderbys;

    age_adjacency_unsupported("rescanning indexes");
}

static void
age_adjacency_end_scan(IndexScanDesc scan)
{
    (void) scan;

    age_adjacency_unsupported("ending index scans");
}

Datum
age_adjacency_handler(PG_FUNCTION_ARGS)
{
    IndexAmRoutine *amroutine = makeNode(IndexAmRoutine);

    amroutine->amstrategies = 1;
    amroutine->amsupport = 0;
    amroutine->amoptsprocnum = 0;
    amroutine->amcanorder = false;
    amroutine->amcanorderbyop = false;
    amroutine->amcanhash = false;
    amroutine->amconsistentequality = true;
    amroutine->amconsistentordering = false;
    amroutine->amcanbackward = false;
    amroutine->amcanunique = false;
    amroutine->amcanmulticol = false;
    amroutine->amoptionalkey = false;
    amroutine->amsearcharray = false;
    amroutine->amsearchnulls = false;
    amroutine->amstorage = false;
    amroutine->amclusterable = false;
    amroutine->ampredlocks = false;
    amroutine->amcanparallel = false;
    amroutine->amcanbuildparallel = false;
    amroutine->amcaninclude = false;
    amroutine->amusemaintenanceworkmem = false;
    amroutine->amsummarizing = false;
    amroutine->amparallelvacuumoptions = 0;
    amroutine->amkeytype = InvalidOid;

    amroutine->ambuild = age_adjacency_build;
    amroutine->ambuildempty = age_adjacency_build_empty;
    amroutine->aminsert = age_adjacency_insert;
    amroutine->aminsertcleanup = NULL;
    amroutine->ambulkdelete = age_adjacency_bulk_delete;
    amroutine->amvacuumcleanup = age_adjacency_vacuum_cleanup;
    amroutine->amcanreturn = NULL;
    amroutine->amcostestimate = age_adjacency_cost_estimate;
    amroutine->amgettreeheight = NULL;
    amroutine->amoptions = age_adjacency_options;
    amroutine->amproperty = NULL;
    amroutine->ambuildphasename = NULL;
    amroutine->amvalidate = age_adjacency_validate;
    amroutine->amadjustmembers = NULL;
    amroutine->ambeginscan = age_adjacency_begin_scan;
    amroutine->amrescan = age_adjacency_rescan;
    amroutine->amgettuple = NULL;
    amroutine->amgetbitmap = NULL;
    amroutine->amendscan = age_adjacency_end_scan;
    amroutine->ammarkpos = NULL;
    amroutine->amrestrpos = NULL;
    amroutine->amestimateparallelscan = NULL;
    amroutine->aminitparallelscan = NULL;
    amroutine->amparallelrescan = NULL;
    amroutine->amtranslatestrategy = NULL;
    amroutine->amtranslatecmptype = NULL;

    PG_RETURN_POINTER(amroutine);
}
