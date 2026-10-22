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

export module lucene_core:field_token_streams;

import :types;
import std;

import :attribute_source;
import :error;
import :token_stream;

namespace lucene {

/// A [`TokenStream`] that yields one string as a single token.
///
/// Port of `StringTokenStream` (`document/field.rs`).  Used by fields that are
/// indexed but not tokenized (`StringField`, `KeywordField`), so the indexer
/// does not need an analyzer for them.  The instance is reused across documents
/// by its owning field, exactly like the reference implementation's `ReusedIndexingTokenStream`.
///
/// **Deviation:** the offsets are in Lucene's usual unit (UTF-16 code units),
/// which this port converts from the UTF-8 input.
export class StringTokenStream final : public TokenStream {
public:
    StringTokenStream() = default;

    StringTokenStream(const StringTokenStream &) = delete;
    StringTokenStream &operator=(const StringTokenStream &) = delete;
    StringTokenStream(StringTokenStream &&) noexcept = delete;
    StringTokenStream &operator=(StringTokenStream &&) noexcept = delete;
    ~StringTokenStream() override = default;

    /// Sets the value the next [`reset`] cycle will emit.
    [[nodiscard]] VoidResult set_value(std::string_view value);

    [[nodiscard]] AttributeSource &attributes() override { return attributes_; }
    [[nodiscard]] VoidResult reset() override;
    [[nodiscard]] Result<bool> increment_token() override;
    [[nodiscard]] VoidResult end() override;

    /// The token text currently held, for tests and diagnostics.
    [[nodiscard]] std::string_view value() const noexcept { return value_; }
    /// The offset the single token spans, in UTF-16 code units.
    [[nodiscard]] uint32_t end_offset() const noexcept { return end_offset_; }

private:
    bool used_{false};
    bool has_value_{false};
    std::string value_;
    uint32_t end_offset_{0};
    AttributeSource attributes_;
};

/// A [`TokenStream`] that yields one binary value as a single token.
///
/// Port of `BinaryTokenStream` (`document/field.rs`); the term bytes go through
/// [`BytesTermAttribute`].
export class BinaryTokenStream final : public TokenStream {
public:
    BinaryTokenStream() = default;

    BinaryTokenStream(const BinaryTokenStream &) = delete;
    BinaryTokenStream &operator=(const BinaryTokenStream &) = delete;
    BinaryTokenStream(BinaryTokenStream &&) noexcept = delete;
    BinaryTokenStream &operator=(BinaryTokenStream &&) noexcept = delete;
    ~BinaryTokenStream() override = default;

    /// Sets the bytes the next [`reset`] cycle will emit.
    void set_value(std::span<const uint8_t> value);

    [[nodiscard]] AttributeSource &attributes() override { return attributes_; }
    [[nodiscard]] VoidResult reset() override { return {}; }
    [[nodiscard]] Result<bool> increment_token() override;
    [[nodiscard]] VoidResult end() override { return TokenStream::end(); }

private:
    bool used_{false};
    std::vector<uint8_t> value_;
    AttributeSource attributes_;
};

/// Length of `text` in UTF-16 code units, which is what Lucene offsets use.
export [[nodiscard]] Result<uint32_t> utf16_length(std::string_view text);

} // namespace lucene
