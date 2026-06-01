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

#ifndef AG_GUC_H
#define AG_GUC_H

/*
 * AGE configuration parameters.
 *
 * Ideally, these parameters should be documented in a .sgml file.
 *
 * To add a new parameter, add a global variable. Add its definition
 * in the `define_config_params` function. Include this header file
 * to use the global variable. The parameters can be set just like
 * regular Postgres parameters. See guc.h for more details.
 */



/*
 * Enables the experimental fixed-length MATCH rewrite that scans
 * age_adjacency candidate providers for guarded one-hop shapes. Keep this
 * opt-in until cost/cardinality policy and regression coverage are settled.
 */
extern bool age_enable_adjacency_match;
extern bool age_enable_adjacency_match_custom_path;

void define_config_params(void);

#endif
