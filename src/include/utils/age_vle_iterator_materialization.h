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

#ifndef AGE_VLE_ITERATOR_MATERIALIZATION_H
#define AGE_VLE_ITERATOR_MATERIALIZATION_H

#include "postgres.h"

#include "utils/age_vle.h"

typedef enum VLEIteratorMaterializationKind
{
    VLE_ITERATOR_MATERIALIZE_PATH,
    VLE_ITERATOR_MATERIALIZE_TERMINAL_VERTEX,
    VLE_ITERATOR_MATERIALIZE_TERMINAL_PROPERTY,
    VLE_ITERATOR_MATERIALIZE_TERMINAL_PROPERTIES
} VLEIteratorMaterializationKind;

typedef enum VLEIteratorContainerKind
{
    VLE_ITERATOR_CONTAINER_NONE,
    VLE_ITERATOR_CONTAINER_PATH,
    VLE_ITERATOR_CONTAINER_REVERSED_PATH,
    VLE_ITERATOR_CONTAINER_ZERO_PATH,
    VLE_ITERATOR_CONTAINER_TERMINAL_VERTEX,
    VLE_ITERATOR_CONTAINER_ZERO_TERMINAL_VERTEX
} VLEIteratorContainerKind;

typedef struct VLEIteratorMaterialization
{
    VLEIteratorMaterializationKind kind;
    VLEIteratorContainerKind container_kind;
    bool is_zero_bound;
} VLEIteratorMaterialization;

typedef struct VLEIteratorOutputTarget
{
    Datum *result;
    bool *is_null;
} VLEIteratorOutputTarget;

typedef bool (*VLEIteratorEmitTerminalProperty) (
    void *state, const VLEIteratorMaterialization *materialization,
    const VLEIteratorOutputTarget *target);
typedef bool (*VLEIteratorEmitTerminalProperties) (
    void *state, const VLEIteratorOutputTarget *target);
typedef void *(*VLEIteratorBuildContainer) (
    void *state, const VLEIteratorMaterialization *materialization);

typedef struct VLEIteratorOutputCallbacks
{
    void *state;
    VLEIteratorEmitTerminalProperty emit_terminal_property;
    VLEIteratorEmitTerminalProperties emit_terminal_properties;
    VLEIteratorBuildContainer build_container;
} VLEIteratorOutputCallbacks;

extern bool age_vle_output_requirement_is_terminal_only(
    AgeVLEOutputRequirement requirement);
extern void age_vle_iterator_materialization_init(
    VLEIteratorMaterialization *materialization,
    AgeVLEOutputRequirement requirement,
    bool emit_terminal_property,
    bool reverse_output_path,
    bool is_zero_bound);
extern void age_vle_iterator_output_target_init(
    VLEIteratorOutputTarget *target, Datum *result, bool *is_null);
extern void age_vle_iterator_output_target_reset(
    const VLEIteratorOutputTarget *target);
extern void age_vle_iterator_output_target_set_null(
    const VLEIteratorOutputTarget *target);
extern void age_vle_iterator_output_target_set_datum(
    const VLEIteratorOutputTarget *target, Datum value);
extern void age_vle_iterator_output_target_set_pointer(
    const VLEIteratorOutputTarget *target, void *ptr);
extern bool age_vle_iterator_emit_result(
    const VLEIteratorOutputCallbacks *callbacks,
    const VLEIteratorOutputTarget *target,
    const VLEIteratorMaterialization *materialization);

#endif
