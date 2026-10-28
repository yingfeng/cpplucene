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

module lucene_core:query_visitor.impl;

import :query_visitor;

import std;

import :byte_run_automaton;
import :error;
import :occur;
import :query;
import :term;
import :types;

namespace lucene {

DefaultQueryVisitor QueryVisitor::get_sub_visitor(Occur occur, const Query & /*parent*/) {
    // A `MUST_NOT` clause excludes documents, so nothing a visitor wants to know
    // about the matched terms lives inside it.
    return DefaultQueryVisitor(occur == Occur::MustNot ? nullptr : this);
}

VoidResult DefaultQueryVisitor::consume_terms(const Query &query, std::span<const Term> terms) {
    if (visitor_ == nullptr) {
        return {};
    }
    return visitor_->consume_terms(query, terms);
}

VoidResult DefaultQueryVisitor::consume_terms_matching(const Query &query, std::string_view field,
                                                       const AutomatonSupplier &automaton) {
    if (visitor_ == nullptr) {
        return {};
    }
    return visitor_->consume_terms_matching(query, field, automaton);
}

VoidResult DefaultQueryVisitor::visit_leaf(const Query &query) {
    if (visitor_ == nullptr) {
        return {};
    }
    return visitor_->visit_leaf(query);
}

bool DefaultQueryVisitor::accept_field(std::string_view field) const {
    return visitor_ != nullptr && visitor_->accept_field(field);
}

DefaultQueryVisitor DefaultQueryVisitor::get_sub_visitor(Occur occur, const Query &parent) {
    if (visitor_ == nullptr || occur == Occur::MustNot) {
        return DefaultQueryVisitor(nullptr);
    }
    return DefaultQueryVisitor(visitor_);
}

} // namespace lucene
