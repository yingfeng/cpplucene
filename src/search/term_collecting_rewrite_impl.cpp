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

module lucene_core:term_collecting_rewrite.impl;

import :term_collecting_rewrite;

import std;

import :attribute_source;
import :boost_attribute;
import :bytes_ref;
import :error;
import :index_searcher;
import :leaf_reader;
import :leaf_reader_context;
import :multi_term_query;
import :terms;
import :terms_enum;
import :types;

namespace lucene {

VoidResult collect_terms(const IndexSearcher &searcher, const MultiTermQuery &query, TermCollector &collector) {
    auto leaves = searcher.leaves();
    if (!leaves.has_value()) {
        return std::unexpected(std::move(leaves.error()));
    }
    for (const LeafReaderContext &context : leaves.value()) {
        if (context.reader() == nullptr) {
            continue;
        }
        auto terms = context.reader()->terms(query.field());
        if (!terms.has_value()) {
            return std::unexpected(std::move(terms.error()));
        }
        if (terms.value() == nullptr) {
            // This leaf has no terms for the field.
            continue;
        }
        auto iterator = query.terms_enum_for(*terms.value());
        if (!iterator.has_value()) {
            return std::unexpected(std::move(iterator.error()));
        }
        TermsEnum &terms_enum = *iterator.value();

        if (auto context_set = collector.set_reader_context(context); !context_set.has_value()) {
            return std::unexpected(std::move(context_set.error()));
        }
        if (auto enum_set = collector.set_next_enum(terms_enum); !enum_set.has_value()) {
            return std::unexpected(std::move(enum_set.error()));
        }
        while (true) {
            auto next = terms_enum.next();
            if (!next.has_value()) {
                return std::unexpected(std::move(next.error()));
            }
            if (next.value() == nullptr) {
                break;
            }
            // The enumeration reuses its buffer, so the collector gets a copy.
            const BytesRef bytes = *next.value();
            auto keep_going = collector.collect(bytes, terms_enum, searcher);
            if (!keep_going.has_value()) {
                return std::unexpected(std::move(keep_going.error()));
            }
            if (!keep_going.value()) {
                return {};
            }
        }
    }
    return {};
}

Result<float> terms_enum_boost(TermsEnum &terms_enum) {
    auto attributes = terms_enum.attributes();
    if (!attributes.has_value()) {
        // An enum that has no attributes reports it unsupported; that is the
        // "no boost published" case of the reference implementation.
        if (attributes.error().kind() == ErrorKind::kUnsupportedOperation ||
            attributes.error().kind() == ErrorKind::kNotImplemented) {
            return BoostAttribute::kDefaultBoost;
        }
        return std::unexpected(std::move(attributes.error()));
    }
    if (attributes.value() == nullptr) {
        return BoostAttribute::kDefaultBoost;
    }
    const auto *boost = attributes.value()->get_attribute<BoostAttribute>();
    return boost == nullptr ? BoostAttribute::kDefaultBoost : boost->get_boost();
}

} // namespace lucene
