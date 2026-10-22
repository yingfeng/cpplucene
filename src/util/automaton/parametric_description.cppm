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

export module lucene_core:parametric_description;

import :types;
import std;

namespace lucene {

/// A [`ParametricDescription`] describes the structure of a Levenshtein DFA for
/// some degree `n`.
///
/// There are four components of a parametric description, all parameterized on
/// the length of the word `w`:
///
/// 1. the number of states, [`ParametricDescription::size`];
/// 2. the set of final states, [`ParametricDescription::is_accept`];
/// 3. the transition function, [`ParametricDescription::transition`];
/// 4. the minimal boundary function, [`ParametricDescription::get_position`].
///
/// Port of `ParametricDescriptionBase` (`core/util/automation/levenshtein_automata.rs`).
export class ParametricDescriptionBase {
public:
    ParametricDescriptionBase() = default;
    ParametricDescriptionBase(const ParametricDescriptionBase &) = default;
    ParametricDescriptionBase &operator=(const ParametricDescriptionBase &) = default;
    ParametricDescriptionBase(ParametricDescriptionBase &&) = default;
    ParametricDescriptionBase &operator=(ParametricDescriptionBase &&) = default;
    virtual ~ParametricDescriptionBase() = default;

    /// The state number for a transition from `abs_state`, assuming `position`
    /// and the characteristic `vector`; `-1` means the empty state.
    [[nodiscard]] virtual int32_t transition(int32_t abs_state, int32_t position, int32_t vector,
                                             const class ParametricDescription &base) const = 0;
};

/// The parametric description of a Levenshtein DFA.
///
/// Port of `ParametricDescription` (`core/util/automation/levenshtein_automata.rs`).
export class ParametricDescription {
public:
    ParametricDescription(int32_t w, int32_t n, std::vector<int32_t> min_errors,
                          std::shared_ptr<const ParametricDescriptionBase> sub);

    /// The length of the word the DFA is built for.
    [[nodiscard]] int32_t w() const noexcept { return w_; }
    /// The edit distance the DFA is built for.
    [[nodiscard]] int32_t n() const noexcept { return n_; }

    /// The number of states needed to compute the Levenshtein DFA.
    [[nodiscard]] int32_t size() const noexcept {
        return static_cast<int32_t>(min_errors_.size()) * (w_ + 1);
    }

    /// Whether `abs_state` is a final state of the DFA.
    [[nodiscard]] bool is_accept(int32_t abs_state) const;

    /// The minimal boundary in the input word for `abs_state`.
    [[nodiscard]] int32_t get_position(int32_t abs_state) const noexcept { return abs_state % (w_ + 1); }

    [[nodiscard]] int32_t transition(int32_t state, int32_t position, int32_t vector) const;

    /// The minimum number of errors of each state, per offset; exposed for the
    /// description implementations.
    [[nodiscard]] const std::vector<int32_t> &min_errors() const noexcept { return min_errors_; }

private:
    int32_t w_;
    int32_t n_;
    std::vector<int32_t> min_errors_;
    std::shared_ptr<const ParametricDescriptionBase> sub_;
};

/// Extracts the `bits_per_value` wide field at `index` of the packed `data`.
///
/// Port of `unpack` (`core/util/automation/levenshtein_automata.rs`).
[[nodiscard]] int32_t unpack(std::span<const int64_t> data, int32_t index, size_t bits_per_value);

} // namespace lucene
