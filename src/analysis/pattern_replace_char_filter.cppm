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

export module lucene_core:pattern_replace_char_filter;

import :char_filter;
import :types;

import std;

namespace lucene {

/// Replaces every match of a regular expression with a replacement string.
///
/// Port of `PatternReplaceCharFilter`
/// (`analysis/common/charfilter/pattern_replace_char_filter.rs`).
///
/// **Deviation:** the reference implementation uses Java's `Pattern`, which
/// supports back references such as `$1`; this port uses `std::regex`
/// (ECMAScript grammar) with `std::regex_replace`, so `$1` works but Java-only
/// constructs (`\p{...}`, POSIX classes in some forms, look-behind) do not.
export class PatternReplaceCharFilter final : public CharFilter {
public:
    /// Compiles `pattern`; an invalid expression is reported instead of thrown.
    [[nodiscard]] static Result<PatternReplaceCharFilter> create(std::string pattern, std::string replacement);

    [[nodiscard]] Result<std::string> filter(std::string_view input) const override;

    [[nodiscard]] std::string to_string() const override;

    [[nodiscard]] const std::string &pattern() const noexcept { return pattern_; }
    [[nodiscard]] const std::string &replacement() const noexcept { return replacement_; }

private:
    PatternReplaceCharFilter(std::string pattern, std::string replacement, std::regex compiled);

    std::string pattern_;
    std::string replacement_;
    std::regex compiled_;
};

} // namespace lucene
