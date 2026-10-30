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

export module lucene_core:token_stream_to_term_automaton_query;

import :types;
import std;

import :error;
import :term_automaton_query;
import :token_stream;

namespace lucene {

/// Pulls a token graph out of a [`TokenStream`] and turns it into a
/// [`TermAutomatonQuery`], one automaton state per position.
///
/// Port of `TokenStreamToTermAutomatonQuery`
/// (`sandbox/search/token_stream_to_term_automaton_query.rs`).
///
/// Experimental, like the reference implementation.  A `*` token becomes an
/// any-term transition; everything else becomes a term transition.
///
/// **Deviations** (C++ mechanics only): the term bytes are read from
/// [`CharTermAttribute`], which is the attribute the port's tokenizers fill (in
/// the reference implementation `CharTermAttributeImpl` doubles as the
/// `TermToBytesRefAttribute`); its UTF-8 bytes are the term.
export class TokenStreamToTermAutomatonQuery {
public:
    /// Position increments are preserved by default, so a hole in the token
    /// stream is rejected instead of silently squeezed out.
    TokenStreamToTermAutomatonQuery() = default;

    /// Whether to generate holes in the automaton for missing positions.
    void set_preserve_position_increments(bool enable_position_increments) noexcept {
        preserve_position_increments_ = enable_position_increments;
    }

    /// Consumes `input` (which is reset first) and builds the query.  Fails when
    /// the stream has a position increment larger than `1` and increments are
    /// preserved.
    [[nodiscard]] Result<TermAutomatonQuery> to_query(std::string field, TokenStream &input) const;

private:
    bool preserve_position_increments_{true};
};

} // namespace lucene
