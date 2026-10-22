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

export module lucene_core:token_stream;

import :types;
import :error;

import std;

import :attribute_source;
import :attributes;

namespace lucene {

/// The stream of tokens produced by an analyzer.
///
/// Port of the `TokenStream` trait (`analysis/token_stream.rs`).  A stream is a
/// chain: the tokenizer at the head writes into the shared [`AttributeSource`],
/// each [`TokenFilter`] mutates the attributes and forwards the call.
///
/// Every stage returns the **same** `AttributeSource` reference, so a consumer
/// can grab an attribute pointer once and keep using it across tokens.
export class TokenStream {
public:
    virtual ~TokenStream() = default;

    /// The shared attribute instances of this stream.
    [[nodiscard]] virtual AttributeSource &attributes() = 0;

    /// Resets the stream to the start of its input.
    [[nodiscard]] virtual VoidResult reset() { return {}; }

    /// Advances to the next token, returning `false` at the end of the stream.
    [[nodiscard]] virtual Result<bool> increment_token() = 0;

    /// Called after the last token was consumed.
    [[nodiscard]] virtual VoidResult end() {
        attributes().end_attributes();
        return {};
    }

    /// Releases resources held by the stream.
    [[nodiscard]] virtual VoidResult close() { return {}; }

    // ---- convenience accessors ----

    [[nodiscard]] CharTermAttribute *term_attribute() { return attributes().add_attribute<CharTermAttribute>(); }
    [[nodiscard]] OffsetAttribute *offset_attribute() { return attributes().add_attribute<OffsetAttribute>(); }
    [[nodiscard]] PositionIncrementAttribute *position_increment_attribute() {
        return attributes().add_attribute<PositionIncrementAttribute>();
    }
};

/// Base class for a filter over another [`TokenStream`].
///
/// The filter shares the input's attribute instances; `attributes()` therefore
/// forwards to the input rather than owning its own source.
export class TokenFilter : public TokenStream {
public:
    explicit TokenFilter(std::unique_ptr<TokenStream> input) : input_(std::move(input)) {}

    [[nodiscard]] AttributeSource &attributes() override { return input_->attributes(); }

    [[nodiscard]] VoidResult reset() override { return input_->reset(); }
    [[nodiscard]] Result<bool> increment_token() override { return input_->increment_token(); }
    [[nodiscard]] VoidResult end() override { return input_->end(); }
    [[nodiscard]] VoidResult close() override { return input_->close(); }

    [[nodiscard]] TokenStream &input() noexcept { return *input_; }

protected:
    std::unique_ptr<TokenStream> input_;
};

} // namespace lucene
