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

export module lucene_core:term_collecting_rewrite;

import :types;
import std;

import :block_term_state;
import :boost_attribute;
import :bytes_ref;
import :error;
import :index_searcher;
import :leaf_reader_context;
import :multi_term_query;
import :terms_enum;

namespace lucene {

/// A term collected by a multi-term rewrite, together with the state that lets a
/// [`TermQuery`](:term_query) skip the term dictionary lookup.
///
/// Port of `TermAndState` (`core/search/abstract_multi_term_query_constant_score_wrapper.rs`).
export struct TermAndState {
    BytesRef term{};
    TermStateEnum state;
    int32_t doc_freq{0};
    int64_t total_term_freq{0};

    TermAndState(BytesRef term, TermStateEnum state, int32_t doc_freq, int64_t total_term_freq)
            : term(std::move(term)), state(std::move(state)), doc_freq(doc_freq), total_term_freq(total_term_freq) {}
};

/// Receives the terms a multi-term query matches, leaf by leaf.
///
/// Port of the `TermCollector` trait (`core/search/term_collecting_rewrite.rs`).
export class TermCollector {
public:
    virtual ~TermCollector() = default;

    /// Tells the collector which leaf is being visited; this is the `ord` the
    /// [`TermStates`](:term_states) it builds register their states under.
    [[nodiscard]] virtual VoidResult set_reader_context(const LeafReaderContext &context) = 0;

    /// Handles one matched term.  Returns `false` to stop collecting, which the
    /// top-terms collector uses when the enumeration can no longer produce a
    /// competitive term.
    [[nodiscard]] virtual Result<bool> collect(const BytesRef &bytes, TermsEnum &terms_enum,
                                               const IndexSearcher &searcher) = 0;

    /// Tells the collector that `terms_enum` is about to enumerate the terms of
    /// the next leaf.
    [[nodiscard]] virtual VoidResult set_next_enum(TermsEnum &terms_enum) = 0;
};

/// Runs `collector` over every leaf of `searcher`'s reader for the terms
/// `query` matches.
///
/// Port of `TermCollectingRewrite::collect_terms`
/// (`core/search/term_collecting_rewrite.rs`).
export [[nodiscard]] VoidResult collect_terms(const IndexSearcher &searcher, const MultiTermQuery &query,
                                             TermCollector &collector);

/// The boost of the current term, as published by `terms_enum`; `1.0` when it
/// publishes no [`BoostAttribute`](:boost_attribute).
///
/// The reference implementation reads the attribute through its trait object and
/// falls back to the default when the enum reports it unsupported; the port's
/// attribute source is a plain lookup, so a missing attribute is the fallback.
export [[nodiscard]] Result<float> terms_enum_boost(TermsEnum &terms_enum);

} // namespace lucene
