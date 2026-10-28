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

export module lucene_core:query_parser_constants;

import :types;
import std;

namespace lucene {

/// The token ids and literal images of the classic query syntax.
///
/// Port of `query_parser_constants` (`queryparser/classic/query_parser_constants.rs`).
///
/// **Deviation:** the reference implementation stops at these constants as well —
/// the JFlex grammar itself was never ported — so this module is the vocabulary a
/// lexer would use, not a parser.  A caller can build a lexer on top of
/// [`syntax_token_image`] and the ids below.
export namespace query_syntax {

    /// End of file.
    inline constexpr int32_t EOF_TOKEN = 0;
    /// A digit.
    inline constexpr int32_t NUM_CHAR = 1;
    /// A backslash escape.
    inline constexpr int32_t ESCAPED_CHAR = 2;
    /// The first character of a term.
    inline constexpr int32_t TERM_START_CHAR = 3;
    /// A character that may appear inside a term.
    inline constexpr int32_t TERM_CHAR = 4;
    /// Whitespace.
    inline constexpr int32_t WHITESPACE = 5;
    /// A character that may appear inside a quoted term.
    inline constexpr int32_t QUOTED_CHAR = 6;

    // ---- keywords and punctuation ----

    /// `AND`
    inline constexpr int32_t AND = 8;
    /// `OR`
    inline constexpr int32_t OR = 9;
    /// `NOT`
    inline constexpr int32_t NOT = 10;
    /// `+`
    inline constexpr int32_t PLUS = 11;
    /// `-`
    inline constexpr int32_t MINUS = 12;
    /// A bare `+`/`-` that is not a clause modifier.
    inline constexpr int32_t BAREOPER = 13;
    /// `(`
    inline constexpr int32_t LPAREN = 14;
    /// `)`
    inline constexpr int32_t RPAREN = 15;
    /// `:`
    inline constexpr int32_t COLON = 16;
    /// `*`
    inline constexpr int32_t STAR = 17;
    /// `^`
    inline constexpr int32_t CARAT = 18;

    // ---- terms ----

    /// A quoted term.
    inline constexpr int32_t QUOTED = 19;
    /// A plain term.
    inline constexpr int32_t TERM = 20;
    /// The `~<number>` of a fuzzy or proximity query.
    inline constexpr int32_t FUZZY_SLOP = 21;
    /// `term*`
    inline constexpr int32_t PREFIXTERM = 22;
    /// `te*m`
    inline constexpr int32_t WILDTERM = 23;
    /// `/regexp/`
    inline constexpr int32_t REGEXPTERM = 24;

    // ---- ranges ----

    /// `[`
    inline constexpr int32_t RANGEIN_START = 25;
    /// `{`
    inline constexpr int32_t RANGEEX_START = 26;
    /// A number inside a range.
    inline constexpr int32_t NUMBER = 27;
    /// `TO`
    inline constexpr int32_t RANGE_TO = 28;
    /// `]`
    inline constexpr int32_t RANGEIN_END = 29;
    /// `}`
    inline constexpr int32_t RANGEEX_END = 30;
    /// A quoted value inside a range.
    inline constexpr int32_t RANGE_QUOTED = 31;
    /// An unquoted value inside a range.
    inline constexpr int32_t RANGE_GOOP = 32;

    // ---- lexical states ----

    /// The state after `^`.
    inline constexpr int32_t BOOST_LEXICAL_STATE = 0;
    /// The state inside a range.
    inline constexpr int32_t RANGE_LEXICAL_STATE = 1;
    /// The state of ordinary query text.
    inline constexpr int32_t DEFAULT_LEXICAL_STATE = 2;

    /// The literal image of a token kind, as the reference implementation lists it.
    [[nodiscard]] const std::string &token_image(int32_t kind);
    /// How many token kinds [`token_image`] knows.
    [[nodiscard]] constexpr size_t token_image_count() noexcept { return 33; }

} // namespace query_syntax

} // namespace lucene
