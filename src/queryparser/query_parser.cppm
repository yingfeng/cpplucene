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

export module lucene_core:query_parser;

import :analyzer;
import :boolean_clause;
import :error;
import :query;
import :query_parser_base;
import :query_parser_token;
import :types;

import std;

namespace lucene {

/// The classic query syntax.
///
/// Port of `QueryParser` (`queryparser/classic/query_parser.rs`).  The grammar,
/// straight out of the reference implementation's `.jj` file:
///
/// ```text
/// Query  ::= ( Clause )*
/// Clause ::= [ "+" | "-" | "NOT" ] [ <TERM> ":" ] ( <TERM> | "(" Query ")" )
/// ```
///
/// so a query is a list of clauses, each optionally required (`+`), prohibited
/// (`-`/`NOT`) and optionally prefixed by a field name.  A clause is either a
/// term — plain, `foo*`, `f?o*`, `/regexp/`, `"a phrase"`, `"a phrase"~2`,
/// `[a TO b]`, `{a TO b}`, `term~0.8`, any of those with `^2` — or a nested query
/// in parentheses.  `AND`, `&&`, `OR` and `||` join clauses.
///
/// **Deviation:** the parser is hand written instead of being generated from the
/// grammar.  It takes the same tokens from [`tokenize_query`] and produces the
/// same queries, but the JavaCC lookahead is spelled out as explicit checks in
/// [`parse_query`] (see the notes there), which keeps the code readable and
/// keeps the generated-code machinery (`CharStream`, `TokenMgrError`,
/// `ParseException`) out of the port.
///
/// The parser is not thread safe; create one per thread.
export class QueryParser final : public QueryParserBase {
public:
    /// A parser over `analyzer` for terms that do not name a field.
    QueryParser(std::string default_field, Analyzer &analyzer);

    /// Parses `query`; the result is a null pointer when the analysis filtered
    /// every clause away (an empty query, or only stop words).
    [[nodiscard]] Result<std::shared_ptr<Query>> parse(std::string_view query);

private:
    // ---- grammar productions, one per rule of the `.jj` file ----

    [[nodiscard]] Result<std::shared_ptr<Query>> parse_query(std::string_view field);
    [[nodiscard]] Result<std::shared_ptr<Query>> parse_clause(std::string_view field);
    [[nodiscard]] Result<std::shared_ptr<Query>> parse_term(std::string_view field);
    [[nodiscard]] Result<std::shared_ptr<Query>> parse_multi_term(std::string_view field,
                                                                  std::vector<BooleanClause> &clauses,
                                                                  std::shared_ptr<Query> &first_query);
    [[nodiscard]] Conjunction parse_conjunction();
    [[nodiscard]] Modifier parse_modifiers();

    // ---- token helpers ----

    [[nodiscard]] const QueryToken &peek(size_t offset = 0) const;
    [[nodiscard]] bool accept(int32_t kind);
    const QueryToken &advance();
    [[nodiscard]] VoidResult expect(int32_t kind);
    [[nodiscard]] LuceneError unexpected_token(std::string_view expectation) const;
    /// Consumes the `NUMBER` that must follow a `^`, reporting it when missing.
    [[nodiscard]] Result<const QueryToken *> take_number();
    /// Consumes `kind` when it is next, returning a pointer to the consumed token.
    [[nodiscard]] const QueryToken *take(int32_t kind);
    [[nodiscard]] bool is_multi_term_start() const;

    std::vector<QueryToken> tokens_;
    size_t position_{0};
};

} // namespace lucene
