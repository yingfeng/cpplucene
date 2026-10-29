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

export module lucene_core:char_filter;

import :analyzer;
import :token_stream;
import :types;

import std;

namespace lucene {

/// Rewrites the raw field value before the tokenizer sees it.
///
/// Port of `CharFilter` (`analysis/char_filter.rs`).  Lucene's char filters are
/// `Reader` decorators; this port reads the whole field value at once (it is in
/// memory anyway) and returns the transformed text, which is why the interface is
/// a single `filter` call instead of an incremental reader.
///
/// **Deviation:** because the value is transformed up front, the offsets the
/// tokenizer reports refer to the **transformed** text, not to the original one.
/// Lucene's filters carry a `correctOffset` table for that; nothing in this port
/// consumes offsets yet, so the correction is left out.
export class CharFilter {
public:
    virtual ~CharFilter() = default;

    /// The transformed text.
    [[nodiscard]] virtual Result<std::string> filter(std::string_view input) const = 0;

    /// The filter's name, used by [`CharFilterAnalyzer::to_string`].
    [[nodiscard]] virtual std::string to_string() const = 0;
};

/// Applies a chain of [`CharFilter`]s and then delegates the analysis to another
/// analyzer.
///
/// Port of `AnalyzerWrapper` over `initReader`.  The wrapped analyzer keeps its
/// own component reuse, so this wrapper adds no per-call allocation beyond the
/// transformed text.
export class CharFilterAnalyzer final : public Analyzer {
public:
    CharFilterAnalyzer(std::unique_ptr<Analyzer> delegate, std::vector<std::unique_ptr<CharFilter>> filters);

    [[nodiscard]] Result<TokenStream *> token_stream(std::string_view field_name, std::string_view text) override;

    [[nodiscard]] std::string to_string() const override;

    [[nodiscard]] const Analyzer &delegate() const noexcept { return *delegate_; }

protected:
    /// Never called: [`token_stream`] forwards to the wrapped analyzer.
    [[nodiscard]] Result<std::unique_ptr<TokenStreamComponents>> create_components(std::string_view field_name) override;

private:
    std::unique_ptr<Analyzer> delegate_;
    std::vector<std::unique_ptr<CharFilter>> filters_;
};

} // namespace lucene
