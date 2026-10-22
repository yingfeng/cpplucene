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

export module lucene_core:explanation;

import :types;
import std;

import :number;

namespace lucene {

/// Describes how the score of a document for a query was computed.
///
/// Port of `Explanation` (`core/search/explanation.rs`): a tree of nodes, each
/// with a value, a description and sub-explanations.  A non-matching
/// explanation has the value `0.0f` and `is_match() == false`.
export class Explanation {
public:
    Explanation() = default;

    /// A matching node without sub-explanations.
    [[nodiscard]] static Explanation match_no_details(Number value, std::string description);
    /// A matching node.
    [[nodiscard]] static Explanation match_(Number value, std::string description,
                                            std::vector<Explanation> details);
    /// A non-matching node without sub-explanations; its value is `0.0f`.
    [[nodiscard]] static Explanation no_match_no_details(std::string description);
    /// A non-matching node.
    [[nodiscard]] static Explanation no_match(std::string description, std::vector<Explanation> details);
    /// A matching node with value `0` used to report a failure to explain.
    [[nodiscard]] static Explanation error_explanation(std::string description);

    [[nodiscard]] bool is_match() const noexcept { return matched_; }
    [[nodiscard]] const Number &value() const noexcept { return value_; }
    [[nodiscard]] const std::string &description() const noexcept { return description_; }
    [[nodiscard]] const std::vector<Explanation> &details() const noexcept { return details_; }

    /// `value = description`, indented by two spaces per level, one node per
    /// line.
    [[nodiscard]] std::string to_string() const;

    [[nodiscard]] bool operator==(const Explanation &other) const {
        if (matched_ != other.matched_ || value_ != other.value_ || description_ != other.description_ ||
            details_.size() != other.details_.size()) {
            return false;
        }
        return std::ranges::equal(details_, other.details_);
    }

private:
    Explanation(bool matched, Number value, std::string description, std::vector<Explanation> details);

    void append_to(std::string &buffer, size_t depth) const;

    bool matched_{false};
    Number value_{0.0F};
    std::string description_;
    std::vector<Explanation> details_;
};

} // namespace lucene
