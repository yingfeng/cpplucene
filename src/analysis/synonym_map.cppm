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

export module lucene_core:synonym_map;

import :types;

import std;

namespace lucene {

/// A set of synonym rules: a sequence of input words maps to a list of
/// alternative word sequences.
///
/// Port of `SynonymMap` (`analysis/common/synonym/synonym_map.rs`).
///
/// **Deviation:** the reference implementation compiles the rules into an FST of
/// bytes and walks it with `SynonymGraphFilter`; this port keeps them in an
/// ordered map keyed by the input words, which is what
/// [`SynonymFilter`](lucene_core:synonym_filter) needs and avoids porting the
/// FST-based map.  The observable behaviour of a rule set is the same; the
/// multi-word alternatives lose the graph positioning the FST version carries.
export class SynonymMap {
public:
    /// Collects rules and builds a [`SynonymMap`].
    class Builder {
    public:
        explicit Builder(bool deduplicate = true) : deduplicate_(deduplicate) {}

        /// Maps `input` to `output`; `include_original` adds the input itself as
        /// one more alternative, which is what "keep the matched words" means.
        void add(std::vector<std::string> input, std::vector<std::string> output, bool include_original);

        /// Maps every key of `rules` to its list of alternatives, all of them
        /// keeping the input words (`include_original` of the reference
        /// implementation's `add(Map, boolean)`).
        void add_all(const std::map<std::vector<std::string>, std::vector<std::vector<std::string>>> &rules);

        [[nodiscard]] SynonymMap build() const;

    private:
        std::map<std::string, std::vector<std::vector<std::string>>> rules_;
        bool deduplicate_;
    };

    /// The alternatives for the exact input sequence, or `nullptr` when the
    /// sequence is not in the map.
    [[nodiscard]] const std::vector<std::vector<std::string>> *lookup(const std::vector<std::string> &input) const;

    /// The longest input sequence a rule expects; never zero.
    [[nodiscard]] size_t max_input_length() const noexcept { return max_input_length_; }

    /// The number of rules.
    [[nodiscard]] size_t size() const noexcept { return rules_.size(); }

    /// The join of `words`, which is what the map uses as its key.
    [[nodiscard]] static std::string key_of(const std::vector<std::string> &words);

private:
    std::map<std::string, std::vector<std::vector<std::string>>> rules_;
    size_t max_input_length_{1};
};

} // namespace lucene
