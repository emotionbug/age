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

--
-- aggregate function components for stdev(internal, agtype)
-- and stdevp(internal, agtype)
--
-- wrapper for the stdev final function to pass 0 instead of null
CREATE FUNCTION ag_catalog.age_float8_stddev_samp_aggfinalfn(_float8)
    RETURNS agtype
    LANGUAGE c
    IMMUTABLE
PARALLEL SAFE
AS 'MODULE_PATHNAME';

-- wrapper for the float8_accum to use agtype input
CREATE FUNCTION ag_catalog.age_agtype_float8_accum(_float8, agtype)
    RETURNS _float8
    LANGUAGE c
    IMMUTABLE
STRICT
PARALLEL SAFE
AS 'MODULE_PATHNAME';

-- aggregate definition for age_stdev(agtype)
CREATE AGGREGATE ag_catalog.age_stdev(agtype)
(
   stype = _float8,
   sfunc = ag_catalog.age_agtype_float8_accum,
   finalfunc = ag_catalog.age_float8_stddev_samp_aggfinalfn,
   combinefunc = float8_combine,
   finalfunc_modify = read_only,
   initcond = '{0,0,0}',
   parallel = safe
);

-- wrapper for the stdevp final function to pass 0 instead of null
CREATE FUNCTION ag_catalog.age_float8_stddev_pop_aggfinalfn(_float8)
    RETURNS agtype
    LANGUAGE c
    IMMUTABLE
PARALLEL SAFE
AS 'MODULE_PATHNAME';

-- aggregate definition for age_stdevp(agtype)
CREATE AGGREGATE ag_catalog.age_stdevp(agtype)
(
   stype = _float8,
   sfunc = age_agtype_float8_accum,
   finalfunc = ag_catalog.age_float8_stddev_pop_aggfinalfn,
   combinefunc = float8_combine,
   finalfunc_modify = read_only,
   initcond = '{0,0,0}',
   parallel = safe
);

--
-- aggregate function components for avg(agtype) and sum(agtype)
--
-- aggregate definition for avg(agytpe)
CREATE AGGREGATE ag_catalog.age_avg(agtype)
(
   stype = _float8,
   sfunc = ag_catalog.age_agtype_float8_accum,
   finalfunc = float8_avg,
   combinefunc = float8_combine,
   finalfunc_modify = read_only,
   initcond = '{0,0,0}',
   parallel = safe
);

-- sum aggtransfn
CREATE FUNCTION ag_catalog.age_agtype_sum(agtype, agtype)
    RETURNS agtype
    LANGUAGE c
    IMMUTABLE
STRICT
PARALLEL SAFE
AS 'MODULE_PATHNAME';

-- aggregate definition for sum(agytpe)
CREATE AGGREGATE ag_catalog.age_sum(agtype)
(
   stype = agtype,
   sfunc = ag_catalog.age_agtype_sum,
   combinefunc = ag_catalog.age_agtype_sum,
   finalfunc_modify = read_only,
   parallel = safe
);

--
-- aggregate functions for min(variadic "any") and max(variadic "any")
--
-- max transfer function
CREATE FUNCTION ag_catalog.age_agtype_larger_aggtransfn(agtype, variadic "any")
    RETURNS agtype
    LANGUAGE c
    IMMUTABLE
PARALLEL SAFE
AS 'MODULE_PATHNAME';

-- aggregate definition for max(variadic "any")
CREATE AGGREGATE ag_catalog.age_max(variadic "any")
(
   stype = agtype,
   sfunc = ag_catalog.age_agtype_larger_aggtransfn,
   combinefunc = ag_catalog.age_agtype_larger_aggtransfn,
   finalfunc_modify = read_only,
   parallel = safe
);

-- min transfer function
CREATE FUNCTION ag_catalog.age_agtype_smaller_aggtransfn(agtype, variadic "any")
    RETURNS agtype
    LANGUAGE c
    IMMUTABLE
PARALLEL SAFE
AS 'MODULE_PATHNAME';

-- aggregate definition for min(variadic "any")
CREATE AGGREGATE ag_catalog.age_min(variadic "any")
(
   stype = agtype,
   sfunc = ag_catalog.age_agtype_smaller_aggtransfn,
   combinefunc = ag_catalog.age_agtype_smaller_aggtransfn,
   finalfunc_modify = read_only,
   parallel = safe
);

--
-- aggregate functions percentileCont(internal, agtype) and
-- percentileDisc(internal, agtype)
--
-- percentile transfer function
CREATE FUNCTION ag_catalog.age_percentile_aggtransfn(internal, agtype, agtype)
    RETURNS internal
    LANGUAGE c
    IMMUTABLE
PARALLEL SAFE
AS 'MODULE_PATHNAME';

-- percentile_cont final function
CREATE FUNCTION ag_catalog.age_percentile_cont_aggfinalfn(internal)
    RETURNS agtype
    LANGUAGE c
    IMMUTABLE
PARALLEL SAFE
AS 'MODULE_PATHNAME';

-- percentile_disc final function
CREATE FUNCTION ag_catalog.age_percentile_disc_aggfinalfn(internal)
    RETURNS agtype
    LANGUAGE c
    IMMUTABLE
PARALLEL SAFE
AS 'MODULE_PATHNAME';

-- aggregate definition for _percentilecont(agtype, agytpe)
CREATE AGGREGATE ag_catalog.age_percentilecont(agtype, agtype)
(
    stype = internal,
    sfunc = ag_catalog.age_percentile_aggtransfn,
    finalfunc = ag_catalog.age_percentile_cont_aggfinalfn,
    parallel = safe
);

-- aggregate definition for percentiledisc(agtype, agytpe)
CREATE AGGREGATE ag_catalog.age_percentiledisc(agtype, agtype)
(
    stype = internal,
    sfunc = ag_catalog.age_percentile_aggtransfn,
    finalfunc = ag_catalog.age_percentile_disc_aggfinalfn,
    parallel = safe
);

--
-- aggregate functions for collect(variadic "any")
--
-- collect transfer function
CREATE FUNCTION ag_catalog.age_collect_aggtransfn(internal, variadic "any")
    RETURNS internal
    LANGUAGE c
    IMMUTABLE
PARALLEL SAFE
AS 'MODULE_PATHNAME';

-- collect transfer function for direct property extraction
CREATE FUNCTION ag_catalog.age_collect_property_aggtransfn(internal, agtype, agtype)
    RETURNS internal
    LANGUAGE c
    IMMUTABLE
PARALLEL SAFE
AS 'MODULE_PATHNAME';

-- collect transfer function for typed float8 values
CREATE FUNCTION ag_catalog.age_collect_float8_transfn(internal, float8)
    RETURNS internal
    LANGUAGE c
    IMMUTABLE
PARALLEL SAFE
AS 'MODULE_PATHNAME';

-- collect final function for typed float8 values
CREATE FUNCTION ag_catalog.age_collect_float8_finalfn(internal)
    RETURNS agtype
    LANGUAGE c
    IMMUTABLE
PARALLEL SAFE
AS 'MODULE_PATHNAME';

-- collect transfer function for typed int8 values
CREATE FUNCTION ag_catalog.age_collect_int8_transfn(internal, int8)
    RETURNS internal
    LANGUAGE c
    IMMUTABLE
PARALLEL SAFE
AS 'MODULE_PATHNAME';

-- collect final function for typed int8 values
CREATE FUNCTION ag_catalog.age_collect_int8_finalfn(internal)
    RETURNS agtype
    LANGUAGE c
    IMMUTABLE
PARALLEL SAFE
AS 'MODULE_PATHNAME';

-- collect transfer function for typed text values
CREATE FUNCTION ag_catalog.age_collect_text_transfn(internal, text)
    RETURNS internal
    LANGUAGE c
    IMMUTABLE
PARALLEL SAFE
AS 'MODULE_PATHNAME';

-- collect final function for typed text values
CREATE FUNCTION ag_catalog.age_collect_text_finalfn(internal)
    RETURNS agtype
    LANGUAGE c
    IMMUTABLE
PARALLEL SAFE
AS 'MODULE_PATHNAME';

-- collect transfer function for typed numeric values
CREATE FUNCTION ag_catalog.age_collect_numeric_transfn(internal, numeric)
    RETURNS internal
    LANGUAGE c
    IMMUTABLE
PARALLEL SAFE
AS 'MODULE_PATHNAME';

-- collect transfer function for direct numeric property extraction
CREATE FUNCTION ag_catalog.age_collect_numeric_property_transfn(internal, agtype, agtype)
    RETURNS internal
    LANGUAGE c
    IMMUTABLE
PARALLEL SAFE
AS 'MODULE_PATHNAME';

-- collect transfer function for direct numeric property path extraction
CREATE FUNCTION ag_catalog.age_collect_numeric_path_property_transfn(internal, agtype, agtype[])
    RETURNS internal
    LANGUAGE c
    IMMUTABLE
PARALLEL SAFE
AS 'MODULE_PATHNAME';

-- collect final function for typed numeric values
CREATE FUNCTION ag_catalog.age_collect_numeric_finalfn(internal)
    RETURNS agtype
    LANGUAGE c
    IMMUTABLE
PARALLEL SAFE
AS 'MODULE_PATHNAME';

-- array_agg transfer function for direct property extraction
CREATE FUNCTION ag_catalog.age_array_agg_property_transfn(internal, agtype, agtype)
    RETURNS internal
    LANGUAGE c
    IMMUTABLE
PARALLEL SAFE
AS 'MODULE_PATHNAME';

-- array_agg transfer function for two-key direct property map extraction
CREATE FUNCTION ag_catalog.age_array_agg_map2_property_transfn(internal, agtype, text, agtype, text, agtype)
    RETURNS internal
    LANGUAGE c
    IMMUTABLE
PARALLEL SAFE
AS 'MODULE_PATHNAME';

-- array_agg transfer function for direct property map extraction
CREATE FUNCTION ag_catalog.age_array_agg_map_property_transfn(internal, agtype, text[], agtype[])
    RETURNS internal
    LANGUAGE c
    IMMUTABLE
PARALLEL SAFE
AS 'MODULE_PATHNAME';

-- array_agg transfer function for direct property list extraction
CREATE FUNCTION ag_catalog.age_array_agg_list_property_transfn(internal, agtype, agtype[])
    RETURNS internal
    LANGUAGE c
    IMMUTABLE
PARALLEL SAFE
AS 'MODULE_PATHNAME';

-- array_agg transfer function for key/value map slots
CREATE FUNCTION ag_catalog.age_array_agg_map_slots_transfn(internal, variadic "any")
    RETURNS internal
    LANGUAGE c
    IMMUTABLE
PARALLEL SAFE
AS 'MODULE_PATHNAME';

-- array_agg transfer function for list value slots
CREATE FUNCTION ag_catalog.age_array_agg_list_slots_transfn(internal, variadic "any")
    RETURNS internal
    LANGUAGE c
    IMMUTABLE
PARALLEL SAFE
AS 'MODULE_PATHNAME';

-- array_agg final function for direct property extraction
CREATE FUNCTION ag_catalog.age_array_agg_property_finalfn(internal)
    RETURNS agtype[]
    LANGUAGE c
    IMMUTABLE
PARALLEL SAFE
AS 'MODULE_PATHNAME';

-- array_agg final function for slot-vector map/list state
CREATE FUNCTION ag_catalog.age_array_agg_slots_finalfn(internal)
    RETURNS agtype[]
    LANGUAGE c
    IMMUTABLE
PARALLEL SAFE
AS 'MODULE_PATHNAME';

-- array_agg combine function for slot-vector map/list state
CREATE FUNCTION ag_catalog.age_array_agg_slots_combine(internal, internal)
    RETURNS internal
    LANGUAGE c
    IMMUTABLE
PARALLEL SAFE
AS 'MODULE_PATHNAME';

-- array_agg serialize function for slot-vector map/list state
CREATE FUNCTION ag_catalog.age_array_agg_slots_serialize(internal)
    RETURNS bytea
    LANGUAGE c
    IMMUTABLE
PARALLEL SAFE
AS 'MODULE_PATHNAME';

-- array_agg deserialize function for slot-vector map/list state
CREATE FUNCTION ag_catalog.age_array_agg_slots_deserialize(bytea, internal)
    RETURNS internal
    LANGUAGE c
    IMMUTABLE
PARALLEL SAFE
AS 'MODULE_PATHNAME';

-- collect final function
CREATE FUNCTION ag_catalog.age_collect_aggfinalfn(internal)
    RETURNS agtype
    LANGUAGE c
    IMMUTABLE
PARALLEL SAFE
AS 'MODULE_PATHNAME';

-- aggregate definition for age_collect(variadic "any")
CREATE AGGREGATE ag_catalog.age_collect(variadic "any")
(
    stype = internal,
    sfunc = ag_catalog.age_collect_aggtransfn,
    finalfunc = ag_catalog.age_collect_aggfinalfn,
    parallel = safe
);

-- aggregate definition for age_collect_property(agtype, agtype)
CREATE AGGREGATE ag_catalog.age_collect_property(agtype, agtype)
(
    stype = internal,
    sfunc = ag_catalog.age_collect_property_aggtransfn,
    finalfunc = ag_catalog.age_collect_aggfinalfn,
    parallel = safe
);

-- aggregate definition for age_collect_float8(float8)
CREATE AGGREGATE ag_catalog.age_collect_float8(float8)
(
    stype = internal,
    sfunc = ag_catalog.age_collect_float8_transfn,
    finalfunc = ag_catalog.age_collect_float8_finalfn,
    parallel = safe
);

-- aggregate definition for age_collect_int8(int8)
CREATE AGGREGATE ag_catalog.age_collect_int8(int8)
(
    stype = internal,
    sfunc = ag_catalog.age_collect_int8_transfn,
    finalfunc = ag_catalog.age_collect_int8_finalfn,
    parallel = safe
);

-- aggregate definition for age_collect_text(text)
CREATE AGGREGATE ag_catalog.age_collect_text(text)
(
    stype = internal,
    sfunc = ag_catalog.age_collect_text_transfn,
    finalfunc = ag_catalog.age_collect_text_finalfn,
    parallel = safe
);

-- aggregate definition for age_collect_numeric(numeric)
CREATE AGGREGATE ag_catalog.age_collect_numeric(numeric)
(
    stype = internal,
    sfunc = ag_catalog.age_collect_numeric_transfn,
    finalfunc = ag_catalog.age_collect_numeric_finalfn,
    parallel = safe
);

-- aggregate definition for age_collect_numeric_property(agtype, agtype)
CREATE AGGREGATE ag_catalog.age_collect_numeric_property(agtype, agtype)
(
    stype = internal,
    sfunc = ag_catalog.age_collect_numeric_property_transfn,
    finalfunc = ag_catalog.age_collect_numeric_finalfn,
    parallel = safe
);

-- aggregate definition for age_collect_numeric_path_property(agtype, agtype[])
CREATE AGGREGATE ag_catalog.age_collect_numeric_path_property(agtype, agtype[])
(
    stype = internal,
    sfunc = ag_catalog.age_collect_numeric_path_property_transfn,
    finalfunc = ag_catalog.age_collect_numeric_finalfn,
    parallel = safe
);

-- aggregate definition for age_array_agg_property(agtype, agtype)
CREATE AGGREGATE ag_catalog.age_array_agg_property(agtype, agtype)
(
    stype = internal,
    sfunc = ag_catalog.age_array_agg_property_transfn,
    finalfunc = ag_catalog.age_array_agg_property_finalfn,
    parallel = safe
);

-- aggregate definition for age_array_agg_map2_property(agtype, text, agtype, text, agtype)
CREATE AGGREGATE ag_catalog.age_array_agg_map2_property(agtype, text, agtype, text, agtype)
(
    stype = internal,
    sfunc = ag_catalog.age_array_agg_map2_property_transfn,
    finalfunc = ag_catalog.age_array_agg_property_finalfn,
    parallel = safe
);

-- aggregate definition for age_array_agg_map_property(agtype, text[], agtype[])
CREATE AGGREGATE ag_catalog.age_array_agg_map_property(agtype, text[], agtype[])
(
    stype = internal,
    sfunc = ag_catalog.age_array_agg_map_property_transfn,
    finalfunc = ag_catalog.age_array_agg_property_finalfn,
    parallel = safe
);

-- aggregate definition for age_array_agg_list_property(agtype, agtype[])
CREATE AGGREGATE ag_catalog.age_array_agg_list_property(agtype, agtype[])
(
    stype = internal,
    sfunc = ag_catalog.age_array_agg_list_property_transfn,
    finalfunc = ag_catalog.age_array_agg_property_finalfn,
    parallel = safe
);

-- aggregate definition for age_array_agg_map_slots(variadic "any")
CREATE AGGREGATE ag_catalog.age_array_agg_map_slots(variadic "any")
(
    stype = internal,
    sfunc = ag_catalog.age_array_agg_map_slots_transfn,
    combinefunc = ag_catalog.age_array_agg_slots_combine,
    serialfunc = ag_catalog.age_array_agg_slots_serialize,
    deserialfunc = ag_catalog.age_array_agg_slots_deserialize,
    finalfunc = ag_catalog.age_array_agg_slots_finalfn,
    parallel = safe
);

-- aggregate definition for age_array_agg_list_slots(variadic "any")
CREATE AGGREGATE ag_catalog.age_array_agg_list_slots(variadic "any")
(
    stype = internal,
    sfunc = ag_catalog.age_array_agg_list_slots_transfn,
    combinefunc = ag_catalog.age_array_agg_slots_combine,
    serialfunc = ag_catalog.age_array_agg_slots_serialize,
    deserialfunc = ag_catalog.age_array_agg_slots_deserialize,
    finalfunc = ag_catalog.age_array_agg_slots_finalfn,
    parallel = safe
);
