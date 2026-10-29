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

module lucene_core:span_weight.impl;

import :span_weight;

import :explanation;
import :index_searcher;
import :leaf_reader;
import :leaf_reader_context;
import :numeric_doc_values;
import :query;
import :scorer;
import :scorer_supplier;
import :similarity;
import :span_query;
import :span_scorer;
import :spans;
import :term;
import :term_states;
import :types;
import :weight;

import std;

namespace lucene {

namespace {

/// A supplier over a scorer that was already built.  [`get`] may only be called
/// once, just like the reference implementation's one-shot suppliers.
class SingleScorerSupplier final : public ScorerSupplier {
public:
    explicit SingleScorerSupplier(std::unique_ptr<Scorer> scorer) : scorer_(std::move(scorer)) {}

    [[nodiscard]] Result<std::unique_ptr<Scorer>> get(int64_t /*lead_cost*/, const LeafReaderContext & /*context*/,
                                                      const IndexSearcher & /*searcher*/) override {
        return std::move(scorer_);
    }

    [[nodiscard]] Result<int64_t> cost(const LeafReaderContext & /*context*/,
                                       const IndexSearcher & /*searcher*/) override {
        if (scorer_ == nullptr) {
            return int64_t{0};
        }
        return scorer_->cost();
    }

private:
    std::unique_ptr<Scorer> scorer_;
};

} // namespace

SpanWeight::SpanWeight(const SpanQuery &query, const IndexSearcher &searcher, TermStatesMap term_states, float boost)
    : query_(&query), similarity_(searcher.similarity_ptr().get()), field_(query.get_field()), boost_(boost) {
    if (term_states.empty() || field_.empty()) {
        return;
    }

    std::vector<TermStatistics> statistics;
    statistics.reserve(term_states.size());
    for (const auto &[term, states] : term_states) {
        auto doc_freq = states->doc_freq();
        if (!doc_freq.has_value()) {
            // A state built without statistics cannot feed the similarity; the
            // scores then fall back to a constant one, which the caller asked for
            // by not requesting scores in the first place.
            return;
        }
        if (doc_freq.value() <= 0) {
            continue;
        }
        auto total_term_freq = states->total_term_freq();
        if (!total_term_freq.has_value()) {
            return;
        }
        auto created = searcher.term_statistics(term, doc_freq.value(), total_term_freq.value());
        if (!created.has_value()) {
            return;
        }
        statistics.push_back(std::move(created.value()));
    }
    if (statistics.empty()) {
        return;
    }
    auto collection = searcher.collection_statistics(field_);
    if (!collection.has_value()) {
        return;
    }
    auto scorer = similarity_->scorer(boost_, collection.value().value(), statistics);
    if (scorer.has_value()) {
        sim_scorer_ = std::move(scorer.value());
    }
}

Result<std::unique_ptr<ScorerSupplier>> SpanWeight::scorer_supplier(const LeafReaderContext &context,
                                                                   const IndexSearcher & /*searcher*/) {
    auto spans = get_spans(context, SpanPostings::kPositions);
    if (!spans.has_value()) {
        return std::unexpected(std::move(spans.error()));
    }
    if (spans.value() == nullptr) {
        return std::unique_ptr<ScorerSupplier>{};
    }

    auto norms = context.reader()->get_norm_values(field_);
    if (!norms.has_value()) {
        return std::unexpected(std::move(norms.error()));
    }
    std::unique_ptr<Scorer> scorer =
            std::make_unique<SpanScorer>(std::move(spans.value()), sim_scorer_, std::move(norms.value()));
    return std::unique_ptr<ScorerSupplier>(std::make_unique<SingleScorerSupplier>(std::move(scorer)));
}

Result<Explanation> SpanWeight::explain(const LeafReaderContext &context, int32_t doc,
                                        const IndexSearcher &searcher) const {
    // `Weight::explain` is const, but building a scorer needs the searcher's
    // per-leaf state.  The object itself is not const at the call site, so the
    // cast is well defined.
    auto scorer = const_cast<SpanWeight *>(this)->scorer(context, searcher);
    if (!scorer.has_value()) {
        return std::unexpected(std::move(scorer.error()));
    }
    if (scorer.value() == nullptr) {
        return Explanation::no_match_no_details("no matching term");
    }
    auto advanced = scorer.value()->advance(doc);
    if (!advanced.has_value()) {
        return std::unexpected(std::move(advanced.error()));
    }
    if (advanced.value() != doc) {
        return Explanation::no_match_no_details("no matching term");
    }
    auto *span_scorer = dynamic_cast<SpanScorer *>(scorer.value().get());
    if (span_scorer == nullptr) {
        return err(LuceneError::illegal_state("span weight expected a span scorer"));
    }
    auto freq = span_scorer->sloppy_freq();
    if (!freq.has_value()) {
        return std::unexpected(std::move(freq.error()));
    }
    if (sim_scorer_ == nullptr) {
        return Explanation::match_no_details(0.0F, "Frequency:" + std::format("{}", freq.value()));
    }

    const Explanation freq_explanation =
            Explanation::match_no_details(freq.value(), "phraseFreq=" + std::format("{}", freq.value()));
    int64_t norm = 1;
    auto norms = context.reader()->get_norm_values(field_);
    if (!norms.has_value()) {
        return std::unexpected(std::move(norms.error()));
    }
    if (norms.value() != nullptr) {
        auto exact = norms.value()->advance_exact(doc);
        if (!exact.has_value()) {
            return std::unexpected(std::move(exact.error()));
        }
        if (exact.value()) {
            auto value = norms.value()->long_value();
            if (!value.has_value()) {
                return std::unexpected(std::move(value.error()));
            }
            norm = value.value();
        }
    }
    auto score_explanation = sim_scorer_->explain(freq_explanation, norm);
    if (!score_explanation.has_value()) {
        return std::unexpected(std::move(score_explanation.error()));
    }
    auto query_text = query_->to_string("");
    const std::string description = "weight(" + (query_text.has_value() ? query_text.value() : std::string("?")) +
                                    " in " + std::to_string(doc) + ") [" + std::string(similarity_->name()) +
                                    "], result of:";
    return Explanation::match_(score_explanation.value().value(), description,
                               std::vector<Explanation>{std::move(score_explanation.value())});
}

Result<bool> SpanWeight::is_cacheable(const LeafReaderContext & /*context*/) const { return true; }

} // namespace lucene
