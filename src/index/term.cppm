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

export module lucene_core:term;

import :types;
import std;

import :bytes_ref;
import :error;

namespace lucene {

/// A word from the text: the unit of search.
///
/// Port of `Term` (`core/index/term.rs`).  A term is a field name plus the term
/// bytes; terms are ordered by field name first, then by the bytes.
export class Term {
public:
    Term() = default;

    /// A term with the given field and bytes, keeping the `BytesRef` as is.
    Term(std::string field, BytesRef bytes);

    /// A term whose bytes are the UTF-8 encoding of `text`.
    [[nodiscard]] static Term from_text(std::string field, std::string_view text);

    /// A term with an empty value; handy for reuse of a field name.
    [[nodiscard]] static Term from_empty(std::string field);

    [[nodiscard]] const std::string &field() const noexcept { return field_; }
    [[nodiscard]] const BytesRef &bytes() const noexcept { return bytes_; }

    /// The term text, decoded as UTF-8 when possible and rendered as raw bytes
    /// otherwise.
    [[nodiscard]] std::string text() const;

    /// `field:text`, used in messages.
    [[nodiscard]] std::string to_string() const;

    /// Orders by field name, then by bytes.  Returns a negative value when this
    /// term sorts before `other`.
    [[nodiscard]] int32_t compare_to(const Term &other) const;

    [[nodiscard]] bool operator==(const Term &other) const {
        return field_ == other.field_ && bytes_.compare_to(other.bytes_) == 0;
    }

private:
    std::string field_;
    BytesRef bytes_;
};

} // namespace lucene
