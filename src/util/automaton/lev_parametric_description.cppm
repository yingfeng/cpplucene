/*
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

export module lucene_core:lev_parametric_description;

import :types;
import std;

import :parametric_description;

namespace lucene {

/// Parametric description for generating a Levenshtein automaton of degree 1.
///
/// Port of `lev1_parametric_description`
/// (`core/util/automation/lev1_parametric_description.rs`).
[[nodiscard]] ParametricDescription create_lev1_description(int32_t w);

/// Parametric description for generating a Levenshtein automaton of degree 1,
/// with transpositions as primitive edits.
///
/// Port of `lev1t_parametric_description`
/// (`core/util/automation/lev1t_parametric_description.rs`).
[[nodiscard]] ParametricDescription create_lev1t_description(int32_t w);

/// Parametric description for generating a Levenshtein automaton of degree 2.
///
/// Port of `lev2_parametric_description`
/// (`core/util/automation/lev2_parametric_description.rs`).
[[nodiscard]] ParametricDescription create_lev2_description(int32_t w);

/// Parametric description for generating a Levenshtein automaton of degree 2,
/// with transpositions as primitive edits.
///
/// Port of `lev2t_parametric_description`
/// (`core/util/automation/lev2t_parametric_description.rs`).
[[nodiscard]] ParametricDescription create_lev2t_description(int32_t w);

} // namespace lucene
