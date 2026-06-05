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

#include "utils/age_vle_iterator_materialization.h"

bool age_vle_output_requirement_is_terminal_only(
    AgeVLEOutputRequirement requirement)
{
    return requirement == AGE_VLE_OUTPUT_REQUIREMENT_TERMINAL_VERTEX ||
           requirement == AGE_VLE_OUTPUT_REQUIREMENT_TERMINAL_PROPERTIES ||
           requirement == AGE_VLE_OUTPUT_REQUIREMENT_TERMINAL_PROPERTY;
}

void age_vle_iterator_materialization_init(
    VLEIteratorMaterialization *materialization,
    AgeVLEOutputRequirement requirement,
    bool emit_terminal_property,
    bool reverse_output_path,
    bool is_zero_bound)
{
    Assert(materialization != NULL);

    materialization->is_zero_bound = is_zero_bound;
    materialization->container_kind = VLE_ITERATOR_CONTAINER_NONE;
    if (emit_terminal_property)
    {
        materialization->kind = VLE_ITERATOR_MATERIALIZE_TERMINAL_PROPERTY;
    }
    else if (requirement == AGE_VLE_OUTPUT_REQUIREMENT_TERMINAL_PROPERTIES)
    {
        materialization->kind = VLE_ITERATOR_MATERIALIZE_TERMINAL_PROPERTIES;
    }
    else if (age_vle_output_requirement_is_terminal_only(requirement))
    {
        materialization->kind = VLE_ITERATOR_MATERIALIZE_TERMINAL_VERTEX;
        materialization->container_kind = is_zero_bound ?
            VLE_ITERATOR_CONTAINER_ZERO_TERMINAL_VERTEX :
            VLE_ITERATOR_CONTAINER_TERMINAL_VERTEX;
    }
    else
    {
        materialization->kind = VLE_ITERATOR_MATERIALIZE_PATH;
        materialization->container_kind = is_zero_bound ?
            VLE_ITERATOR_CONTAINER_ZERO_PATH :
            (reverse_output_path ? VLE_ITERATOR_CONTAINER_REVERSED_PATH :
             VLE_ITERATOR_CONTAINER_PATH);
    }
}

void age_vle_iterator_output_target_init(
    VLEIteratorOutputTarget *target, Datum *result, bool *is_null)
{
    Assert(target != NULL);

    target->result = result;
    target->is_null = is_null;
}

void age_vle_iterator_output_target_reset(
    const VLEIteratorOutputTarget *target)
{
    Assert(target != NULL);

    if (target->result != NULL)
        *target->result = (Datum) 0;
    if (target->is_null != NULL)
        *target->is_null = true;
}

void age_vle_iterator_output_target_set_null(
    const VLEIteratorOutputTarget *target)
{
    Assert(target != NULL);

    if (target->is_null != NULL)
        *target->is_null = true;
}

void age_vle_iterator_output_target_set_datum(
    const VLEIteratorOutputTarget *target, Datum value)
{
    Assert(target != NULL);

    if (target->result != NULL)
        *target->result = value;
    if (target->is_null != NULL)
        *target->is_null = false;
}

void age_vle_iterator_output_target_set_pointer(
    const VLEIteratorOutputTarget *target, void *ptr)
{
    age_vle_iterator_output_target_set_datum(target, PointerGetDatum(ptr));
}

bool age_vle_iterator_emit_result(
    const VLEIteratorOutputCallbacks *callbacks,
    const VLEIteratorOutputTarget *target,
    const VLEIteratorMaterialization *materialization)
{
    void *container;

    Assert(callbacks != NULL);
    Assert(target != NULL);
    Assert(materialization != NULL);

    switch (materialization->kind)
    {
        case VLE_ITERATOR_MATERIALIZE_TERMINAL_PROPERTY:
            Assert(callbacks->emit_terminal_property != NULL);
            return callbacks->emit_terminal_property(
                callbacks->state, materialization, target);

        case VLE_ITERATOR_MATERIALIZE_TERMINAL_PROPERTIES:
            Assert(callbacks->emit_terminal_properties != NULL);
            return callbacks->emit_terminal_properties(callbacks->state,
                                                       target);

        case VLE_ITERATOR_MATERIALIZE_PATH:
        case VLE_ITERATOR_MATERIALIZE_TERMINAL_VERTEX:
            Assert(callbacks->build_container != NULL);
            container = callbacks->build_container(callbacks->state,
                                                   materialization);
            age_vle_iterator_output_target_set_pointer(target, container);
            return true;
    }

    return false;
}
