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

export module lucene_core:analyzer;

import :types;
import :error;

import std;

import :token_stream;
import :tokenizer;

namespace lucene {

/// How an [`Analyzer`] reuses its token stream components.
/// Port of `ReuseStrategyEnum`.
export enum class ReuseStrategy : uint8_t {
    /// One component chain for every field.
    Global = 0,
    /// One component chain per field name.
    PerField = 1,
};

/// The head (source) and tail (sink) of a component chain.
///
/// `tail` owns the whole chain, including the `source` tokenizer, which is kept
/// as a raw pointer only so the text can be handed to the head of the chain.
export class TokenStreamComponents {
public:
    TokenStreamComponents(Tokenizer *source, std::unique_ptr<TokenStream> tail);

    /// The sink: what the caller iterates.
    [[nodiscard]] TokenStream &token_stream() noexcept { return *tail_; }
    /// The source: where the text is fed in.
    [[nodiscard]] Tokenizer &source() noexcept { return *source_; }

    /// Feeds `text` into the source.
    [[nodiscard]] VoidResult set_reader(std::string_view text) { return source_->set_reader(text); }

private:
    Tokenizer *source_;
    std::unique_ptr<TokenStream> tail_;
};

/// Turns text into a [`TokenStream`].  Port of the `Analyzer` trait.
///
/// **Thread safety:** unlike Lucene's thread-local reusable components, one
/// analyzer instance must not be used concurrently; create one per thread.  The
/// returned stream stays valid until the next call on the same analyzer.
export class Analyzer {
public:
    explicit Analyzer(ReuseStrategy strategy = ReuseStrategy::Global);
    virtual ~Analyzer() = default;

    Analyzer(const Analyzer &) = delete;
    Analyzer &operator=(const Analyzer &) = delete;

    /// Prepares `text` for `field_name` and returns the sink of the chain.
    [[nodiscard]] Result<TokenStream *> token_stream(std::string_view field_name, std::string_view text);

    [[nodiscard]] ReuseStrategy reuse_strategy() const noexcept { return strategy_; }

    /// Human readable name, used in `toString()` of search types.
    [[nodiscard]] virtual std::string to_string() const = 0;

protected:
    /// Builds a fresh component chain for `field_name`.
    [[nodiscard]] virtual Result<std::unique_ptr<TokenStreamComponents>> create_components(
        std::string_view field_name) = 0;

private:
    [[nodiscard]] Result<TokenStreamComponents *> components_for(std::string_view field_name);

    ReuseStrategy strategy_;
    std::unique_ptr<TokenStreamComponents> global_components_{};
    std::map<std::string, std::unique_ptr<TokenStreamComponents>, std::less<>> per_field_components_{};
};

} // namespace lucene
