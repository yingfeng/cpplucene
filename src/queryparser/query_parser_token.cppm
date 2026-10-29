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

export module lucene_core:query_parser_token;

import :error;
import :query_parser_constants;
import :types;

import std;

namespace lucene {

/// One token of the classic query syntax.
///
/// Port of the JavaCC `Token` (`queryparser/classic/Token.java`), collapsed to a
/// plain value: the reference implementation threads tokens through a `next`
/// chain, this port hands the parser a `std::vector` and indexes into it.  The
/// token kind is one of the `query_syntax::*` constants, and `image` is the
/// verbatim source text (escapes are still in it; the parser removes them with
/// [`discard_escape_char`]).
export struct QueryToken {
    /// One of the `query_syntax::*` ids, `query_syntax::EOF_TOKEN` for the end.
    int32_t kind{query_syntax::EOF_TOKEN};
    /// The verbatim source text of the token.
    std::string image;
    /// Byte offset of the first character, relative to the query text.
    size_t begin_offset{0};
    /// Byte offset one past the last character.
    size_t end_offset{0};
    /// 1-based line of the first character.
    int32_t begin_line{1};
    /// 1-based column of the first character.
    int32_t begin_column{1};
};

/// Splits `input` into the tokens of the classic query syntax, with
/// `query_syntax::EOF_TOKEN` (and an empty image) appended.
///
/// This is a hand written lexer for the JFlex grammar of the reference
/// implementation's classic parser.  Like the generated token manager it takes
/// the **longest** match and, on a tie, the rule that is declared first, which
/// is what makes `AND` an operator while `ANDR` is a term, `ab*` a prefix term
/// while `a*b` is a wildcard term, and so on.  Whitespace is skipped, except
/// inside a bare `+`/`-`/`!` operator, which keeps its trailing blank in the
/// image exactly like the reference implementation's `BAREOPER`.
///
/// An unmatched character (or an unterminated quote, or a `^` that is not
/// followed by a number) is reported as `LuceneError::parse` with the line and
/// column of the offending character.
[[nodiscard]] Result<std::vector<QueryToken>> tokenize_query(std::string_view input);

/// Removes the backslash escapes from a token image, decoding `\uXXXX` escapes
/// into UTF-8.
///
/// Port of `QueryParserBase::discardEscapeChar`.  A truncated `\uXXXX`, a
/// non-hexadecimal digit inside one, or a term that ends with a lone backslash
/// is reported as `LuceneError::parse`, exactly like the reference
/// implementation.
[[nodiscard]] Result<std::string> discard_escape_char(std::string_view image);

} // namespace lucene
