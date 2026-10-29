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

export module lucene_core:mapping_char_filter;

import :char_filter;
import :types;

import std;

namespace lucene {

/// Replaces substrings of the input, preferring the longest rule at every
/// position.
///
/// Port of `MappingCharFilter`
/// (`analysis/common/charfilter/mapping_char_filter.rs`).  The rules are the
/// parsed form of a `mapping.txt` file: `"à" => "a"` becomes
/// `{"à", "a"}`.
///
/// **Deviation:** the reference implementation builds a trie and reports the
/// offsets of the original text; this port keeps the rules sorted by descending
/// input length and scans them at each position, and leaves the offsets pointing
/// into the transformed text (see [`CharFilter`]).
export class MappingCharFilter final : public CharFilter {
public:
    /// `mappings` maps each input string to its replacement; an empty replacement
    /// deletes the input.
    explicit MappingCharFilter(std::vector<std::pair<std::string, std::string>> mappings);

    /// Parses the `"from" => "to"` lines of a mapping file, skipping blank lines
    /// and `#` comments.
    [[nodiscard]] static Result<MappingCharFilter> parse(std::string_view rules_text);

    [[nodiscard]] Result<std::string> filter(std::string_view input) const override;

    [[nodiscard]] std::string to_string() const override;

    [[nodiscard]] const std::vector<std::pair<std::string, std::string>> &mappings() const noexcept {
        return mappings_;
    }

private:
    std::vector<std::pair<std::string, std::string>> mappings_;
};

} // namespace lucene
