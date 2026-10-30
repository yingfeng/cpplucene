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

module lucene_core:multi_phrase_query.impl;

import :multi_phrase_query;

import std;

import :boolean_clause;
import :boolean_query;
import :collection_statistics;
import :error;
import :exact_phrase_matcher;
import :explanation;
import :index_searcher;
import :leaf_reader;
import :leaf_reader_context;
import :match_no_docs_query;
import :numeric_doc_values;
import :phrase_matcher;
import :phrase_scorer;
import :postings_enum;
import :query;
import :query_visitor;
import :score_mode;
import :scorer;
import :scorer_supplier;
import :similarity;
import :sloppy_phrase_matcher;
import :term;
import :term_query;
import :term_statistics;
import :terms;
import :terms_enum;
import :types;
import :union_postings;
import :weight;

namespace lucene {

namespace {

/// Scores nothing; used when none of the terms exists in the index or when the
/// query's scores are not needed.
class MultiPhraseDummySimScorer final : public SimScorer {
public:
    [[nodiscard]] float score(float /*freq*/, int64_t /*norm*/) const override { return 0.0F; }
};

std::shared_ptr<const SimScorer> dummy_sim_scorer() {
    static const auto instance = std::make_shared<const MultiPhraseDummySimScorer>();
    return instance;
}

/// What one leaf costs the query.
struct LeafCost {
    int64_t cost{0};
    float match_cost{0.0F};
};

/// Reads the document frequency of every term of every position in `reader`;
/// `nullopt` when some position has none of its terms in the leaf.
Result<std::optional<LeafCost>> analyze_leaf(LeafReader &reader, std::string_view field,
                                             const std::vector<std::vector<Term>> &term_arrays) {
    auto field_terms = reader.terms(field);
    if (!field_terms.has_value()) {
        return std::unexpected(std::move(field_terms.error()));
    }
    if (field_terms.value() == nullptr) {
        return std::optional<LeafCost>{};
    }
    Terms &dictionary = *field_terms.value();
    auto terms_enum = dictionary.iterator();
    if (!terms_enum.has_value()) {
        return std::unexpected(std::move(terms_enum.error()));
    }

    LeafCost info{.cost = std::numeric_limits<int64_t>::max(), .match_cost = 0.0F};
    for (const std::vector<Term> &terms : term_arrays) {
        int64_t slot_freq = std::numeric_limits<int64_t>::max();
        for (const Term &term : terms) {
            auto found = terms_enum.value()->seek_exact(term.bytes().as_slice());
            if (!found.has_value()) {
                return std::unexpected(std::move(found.error()));
            }
            if (!found.value()) {
                continue;
            }
            auto doc_freq = terms_enum.value()->doc_freq();
            if (!doc_freq.has_value()) {
                return std::unexpected(std::move(doc_freq.error()));
            }
            const int64_t frequency = doc_freq.value();
            if (frequency <= 0) {
                continue;
            }
            slot_freq = std::min(slot_freq, frequency);
            info.match_cost += 1.0F + std::log(static_cast<float>(frequency));
        }
        if (slot_freq == std::numeric_limits<int64_t>::max()) {
            // This position cannot match in this leaf.
            return std::optional<LeafCost>{};
        }
        info.cost = std::min(info.cost, slot_freq);
    }
    if (info.cost == std::numeric_limits<int64_t>::max()) {
        info.cost = 0;
    }
    return std::optional<LeafCost>(info);
}

/// Opens the postings of every position, unioning the terms of a position; a
/// null pointer means the leaf cannot match.
Result<std::unique_ptr<PhraseScorer>> build_scorer(LeafReader &reader, std::string_view field,
                                                   const std::vector<std::vector<Term>> &term_arrays,
                                                   const std::vector<int32_t> &positions, int32_t slop,
                                                   float match_cost, std::shared_ptr<const SimScorer> sim_scorer) {
    auto field_terms = reader.terms(field);
    if (!field_terms.has_value()) {
        return std::unexpected(std::move(field_terms.error()));
    }
    if (field_terms.value() == nullptr) {
        return std::unique_ptr<PhraseScorer>{};
    }
    Terms &dictionary = *field_terms.value();
    if (!dictionary.has_positions()) {
        return err(LuceneError::illegal_state(std::format(
                "field \"{}\" was indexed without position data; cannot run MultiPhraseQuery", field)));
    }
    auto terms_enum = dictionary.iterator();
    if (!terms_enum.has_value()) {
        return std::unexpected(std::move(terms_enum.error()));
    }

    std::vector<std::pair<int64_t, PostingsAndFreq>> opened;
    opened.reserve(term_arrays.size());
    for (size_t i = 0; i < term_arrays.size(); ++i) {
        const std::vector<Term> &terms = term_arrays[i];
        std::vector<std::unique_ptr<PostingsEnum>> subs;
        int64_t slot_freq = std::numeric_limits<int64_t>::max();
        for (const Term &term : terms) {
            auto found = terms_enum.value()->seek_exact(term.bytes().as_slice());
            if (!found.has_value()) {
                return std::unexpected(std::move(found.error()));
            }
            if (!found.value()) {
                continue;
            }
            auto doc_freq = terms_enum.value()->doc_freq();
            if (!doc_freq.has_value()) {
                return std::unexpected(std::move(doc_freq.error()));
            }
            if (doc_freq.value() <= 0) {
                continue;
            }
            auto postings = terms_enum.value()->postings_with_flags(nullptr, POSITIONS);
            if (!postings.has_value()) {
                return std::unexpected(std::move(postings.error()));
            }
            if (postings.value() == nullptr) {
                continue;
            }
            slot_freq = std::min(slot_freq, static_cast<int64_t>(doc_freq.value()));
            subs.push_back(std::move(postings.value()));
        }
        if (subs.empty()) {
            return std::unique_ptr<PhraseScorer>{};
        }

        PostingsAndFreq slot{.postings = nullptr, .position = positions[i], .terms = terms};
        if (subs.size() == 1) {
            slot.postings = std::move(subs.front());
        } else {
            slot.postings = std::make_unique<UnionPostingsEnum>(std::move(subs));
        }
        opened.emplace_back(slot_freq, std::move(slot));
    }

    // Keep phrase order; see the note in `phrase_query_impl.cpp` about why the
    // lead must be the first position.
    std::ranges::stable_sort(opened, [](const auto &left, const auto &right) {
        return left.second.position < right.second.position;
    });
    std::vector<PostingsAndFreq> postings;
    postings.reserve(opened.size());
    for (auto &[doc_freq, entry] : opened) {
        (void)doc_freq;
        postings.push_back(std::move(entry));
    }

    std::unique_ptr<PhraseMatcher> matcher;
    if (slop == 0) {
        matcher = std::make_unique<ExactPhraseMatcher>(std::move(postings), match_cost);
    } else {
        matcher = std::make_unique<SloppyPhraseMatcher>(std::move(postings), static_cast<size_t>(slop), match_cost,
                                                        false);
    }

    auto norms = reader.get_norm_values(field);
    if (!norms.has_value()) {
        return std::unexpected(std::move(norms.error()));
    }
    return std::unique_ptr<PhraseScorer>(
            new PhraseScorer(std::move(matcher), std::move(sim_scorer), std::move(norms.value())));
}

/// Supplies the multi-phrase scorer of one leaf.
class MultiPhraseScorerSupplier final : public ScorerSupplier {
public:
    MultiPhraseScorerSupplier(std::string field, std::vector<std::vector<Term>> term_arrays,
                              std::vector<int32_t> positions, int32_t slop, int64_t cost, float match_cost,
                              std::shared_ptr<const SimScorer> sim_scorer)
            : field_(std::move(field)), term_arrays_(std::move(term_arrays)), positions_(std::move(positions)),
              slop_(slop), cost_(cost), match_cost_(match_cost), sim_scorer_(std::move(sim_scorer)) {}

    [[nodiscard]] Result<std::unique_ptr<Scorer>> get(int64_t /*lead_cost*/, const LeafReaderContext &context,
                                                      const IndexSearcher & /*searcher*/) override {
        LeafReader *reader = context.reader();
        if (reader == nullptr) {
            return std::unique_ptr<Scorer>{};
        }
        auto scorer = build_scorer(*reader, field_, term_arrays_, positions_, slop_, match_cost_, sim_scorer_);
        if (!scorer.has_value()) {
            return std::unexpected(std::move(scorer.error()));
        }
        if (scorer.value() == nullptr) {
            return std::unique_ptr<Scorer>{};
        }
        return std::unique_ptr<Scorer>(std::move(scorer.value()));
    }

    [[nodiscard]] Result<int64_t> cost(const LeafReaderContext & /*context*/,
                                       const IndexSearcher & /*searcher*/) override {
        return cost_;
    }

private:
    std::string field_;
    std::vector<std::vector<Term>> term_arrays_;
    std::vector<int32_t> positions_;
    int32_t slop_{0};
    int64_t cost_{0};
    float match_cost_{0.0F};
    std::shared_ptr<const SimScorer> sim_scorer_;
};

/// All the terms of the query, flattened.
std::vector<Term> flatten(const std::vector<std::vector<Term>> &term_arrays) {
    std::vector<Term> flat;
    for (const std::vector<Term> &terms : term_arrays) {
        flat.insert(flat.end(), terms.begin(), terms.end());
    }
    return flat;
}

} // namespace

// ---------------------------------------------------------- MultiPhraseQuery

MultiPhraseQuery::MultiPhraseQuery(std::optional<std::string> field, std::vector<std::vector<Term>> term_arrays,
                                   std::vector<int32_t> positions, int32_t slop)
        : field_(std::move(field)), term_arrays_(std::move(term_arrays)), positions_(std::move(positions)), slop_(slop) {
}

MultiPhraseQuery::Builder::Builder(const MultiPhraseQuery &query)
        : field_(query.field()), term_arrays_(query.term_arrays()), positions_(query.positions()),
          slop_(query.slop()) {}

MultiPhraseQuery::Builder MultiPhraseQuery::builder() { return Builder(); }

VoidResult MultiPhraseQuery::Builder::set_slop(int32_t slop) {
    if (slop < 0) {
        return err(LuceneError::illegal_argument("slop must be greater than or equal to 0"));
    }
    slop_ = slop;
    return {};
}

VoidResult MultiPhraseQuery::Builder::add_term(Term term) {
    return add_terms({std::move(term)});
}

VoidResult MultiPhraseQuery::Builder::add_terms(std::vector<Term> terms) {
    const int32_t position = positions_.empty() ? 0 : positions_.back() + 1;
    return add_terms_with_position(std::move(terms), position);
}

VoidResult MultiPhraseQuery::Builder::add_terms_with_position(std::vector<Term> terms, int32_t position) {
    if (terms.empty()) {
        return err(LuceneError::illegal_argument("a position of a multi phrase query needs at least one term"));
    }
    if (!positions_.empty() && position < positions_.back()) {
        return err(LuceneError::illegal_argument(
                std::format("Positions must be added in order, got {} after {}", position, positions_.back())));
    }
    const std::string &field = terms.front().field();
    for (const Term &term : terms) {
        if (term.field() != field) {
            return err(LuceneError::illegal_argument(
                    std::format("All terms must be on the same field, got {} and {}", field, term.field())));
        }
    }
    if (field_.has_value() && field_.value() != field) {
        return err(LuceneError::illegal_argument(
                std::format("All terms must be on the same field, got {} and {}", field_.value(), field)));
    }
    field_ = field;
    term_arrays_.push_back(std::move(terms));
    positions_.push_back(position);
    return {};
}

Result<MultiPhraseQuery> MultiPhraseQuery::Builder::build() {
    return MultiPhraseQuery(std::move(field_), std::move(term_arrays_), std::move(positions_), slop_);
}

Result<std::unique_ptr<Weight>> MultiPhraseQuery::create_weight(const IndexSearcher &searcher, ScoreMode score_mode,
                                                                float boost) const {
    if (!field_.has_value()) {
        return err(LuceneError::illegal_state("MultiPhraseQuery field is not set"));
    }
    auto weight = MultiPhraseWeight::create(*this, boost, score_mode, searcher);
    if (!weight.has_value()) {
        return std::unexpected(std::move(weight.error()));
    }
    std::unique_ptr<Weight> result = std::move(weight.value());
    return result;
}

Result<std::shared_ptr<Query>> MultiPhraseQuery::rewrite(const IndexSearcher &searcher) const {
    (void)searcher;
    if (term_arrays_.empty()) {
        return std::shared_ptr<Query>(MatchNoDocsQuery::create("empty MultiPhraseQuery"));
    }
    if (term_arrays_.size() == 1) {
        BooleanQuery::Builder builder;
        for (const Term &term : term_arrays_.front()) {
            auto added = builder.add(std::make_shared<TermQuery>(term), Occur::Should);
            if (!added.has_value()) {
                return std::unexpected(std::move(added.error()));
            }
        }
        auto built = builder.build();
        if (!built.has_value()) {
            return std::unexpected(std::move(built.error()));
        }
        return std::shared_ptr<Query>(std::make_shared<BooleanQuery>(std::move(built.value())));
    }
    return std::shared_ptr<Query>{};
}

VoidResult MultiPhraseQuery::visit(QueryVisitor &visitor) const {
    if (!field_.has_value()) {
        return {};
    }
    if (!visitor.accept_field(field_.value())) {
        return {};
    }
    return visitor.consume_terms(*this, flatten(term_arrays_));
}

Result<std::string> MultiPhraseQuery::to_string(std::string_view field) const {
    std::string buffer;
    if (field_.has_value()) {
        if (field != field_.value()) {
            buffer += field_.value();
            buffer += ':';
        }
    } else {
        buffer += "null:";
    }
    buffer += '"';
    int32_t last_position = -1;
    for (size_t i = 0; i < term_arrays_.size(); ++i) {
        const std::vector<Term> &terms = term_arrays_[i];
        const int32_t position = positions_[i];
        if (i != 0) {
            buffer += ' ';
            for (int32_t gap = 1; gap < position - last_position; ++gap) {
                buffer += "? ";
            }
        }
        if (terms.size() > 1) {
            buffer += '(';
            for (size_t j = 0; j < terms.size(); ++j) {
                if (j > 0) {
                    buffer += ' ';
                }
                buffer += terms[j].text();
            }
            buffer += ')';
        } else {
            buffer += terms.front().text();
        }
        last_position = position;
    }
    buffer += '"';
    if (slop_ != 0) {
        buffer += std::format("~{}", slop_);
    }
    return buffer;
}

bool MultiPhraseQuery::equals(const Query &other) const {
    const auto *multi = dynamic_cast<const MultiPhraseQuery *>(&other);
    if (multi == nullptr) {
        return false;
    }
    return slop_ == multi->slop_ && term_arrays_ == multi->term_arrays_ && positions_ == multi->positions_;
}

size_t MultiPhraseQuery::hash() const {
    size_t result = std::hash<int32_t>{}(slop_);
    for (const std::vector<Term> &terms : term_arrays_) {
        for (const Term &term : terms) {
            result = result * 31 + std::hash<std::string>{}(term.field());
            for (const uint8_t byte : term.bytes().as_slice()) {
                result = result * 31 + byte;
            }
        }
    }
    for (const int32_t position : positions_) {
        result = result * 31 + static_cast<size_t>(position);
    }
    return result;
}

// --------------------------------------------------------- MultiPhraseWeight

MultiPhraseWeight::MultiPhraseWeight(const MultiPhraseQuery &query, float boost, ScoreMode score_mode,
                                     std::shared_ptr<const Similarity> similarity,
                                     std::shared_ptr<const SimScorer> sim_scorer)
        : query_(query), boost_(boost), score_mode_(score_mode), similarity_(std::move(similarity)),
          sim_scorer_(std::move(sim_scorer)) {}

Result<std::unique_ptr<MultiPhraseWeight>> MultiPhraseWeight::create(const MultiPhraseQuery &query, float boost,
                                                                     ScoreMode score_mode,
                                                                     const IndexSearcher &searcher) {
    auto weight = std::unique_ptr<MultiPhraseWeight>(
            new MultiPhraseWeight(query, boost, score_mode, searcher.similarity_ptr(), nullptr));

    if (!score_mode_needs_scores(score_mode)) {
        weight->sim_scorer_ = dummy_sim_scorer();
        return weight;
    }

    const std::string &field = query.field().value();
    std::vector<TermStatistics> all_term_stats;
    std::set<std::string> seen;
    for (const std::vector<Term> &terms : query.term_arrays()) {
        for (const Term &term : terms) {
            std::string key = term.field();
            key += '\0';
            key += term.text();
            if (!seen.insert(key).second) {
                continue;
            }
            auto doc_freq = searcher.doc_freq(term);
            if (!doc_freq.has_value()) {
                return std::unexpected(std::move(doc_freq.error()));
            }
            if (doc_freq.value() <= 0) {
                continue;
            }
            auto total_term_freq = searcher.total_term_freq(term);
            if (!total_term_freq.has_value()) {
                return std::unexpected(std::move(total_term_freq.error()));
            }
            auto created = searcher.term_statistics(
                    term, doc_freq.value(),
                    std::max<int64_t>(total_term_freq.value(), static_cast<int64_t>(doc_freq.value())));
            if (!created.has_value()) {
                return std::unexpected(std::move(created.error()));
            }
            all_term_stats.push_back(std::move(created.value()));
        }
    }

    if (all_term_stats.empty()) {
        // None of the terms exists, so nothing can match.
        return weight;
    }

    auto collection_stats = searcher.collection_statistics(field);
    if (!collection_stats.has_value()) {
        return std::unexpected(std::move(collection_stats.error()));
    }
    if (!collection_stats.value().has_value()) {
        return weight;
    }
    auto scorer = searcher.similarity().scorer(boost, collection_stats.value().value(), all_term_stats);
    if (!scorer.has_value()) {
        return std::unexpected(std::move(scorer.error()));
    }
    weight->sim_scorer_ = std::move(scorer.value());
    return weight;
}

Result<std::unique_ptr<ScorerSupplier>> MultiPhraseWeight::scorer_supplier(const LeafReaderContext &context,
                                                                           const IndexSearcher &searcher) {
    (void)searcher;
    if (sim_scorer_ == nullptr || !query_.field().has_value()) {
        return std::unique_ptr<ScorerSupplier>{};
    }
    LeafReader *reader = context.reader();
    if (reader == nullptr) {
        return std::unique_ptr<ScorerSupplier>{};
    }
    auto analyzed = analyze_leaf(*reader, query_.field().value(), query_.term_arrays());
    if (!analyzed.has_value()) {
        return std::unexpected(std::move(analyzed.error()));
    }
    if (!analyzed.value().has_value()) {
        return std::unique_ptr<ScorerSupplier>{};
    }
    std::unique_ptr<ScorerSupplier> supplier(new MultiPhraseScorerSupplier(
            query_.field().value(), query_.term_arrays(), query_.positions(), query_.slop(),
            analyzed.value().value().cost, analyzed.value().value().match_cost, sim_scorer_));
    return supplier;
}

Result<int32_t> MultiPhraseWeight::count(const LeafReaderContext & /*context*/, const IndexSearcher & /*searcher*/) {
    return -1;
}

Result<bool> MultiPhraseWeight::is_cacheable(const LeafReaderContext & /*context*/) const { return true; }

Result<Explanation> MultiPhraseWeight::explain(const LeafReaderContext &context, int32_t doc,
                                               const IndexSearcher &searcher) const {
    (void)searcher;
    if (sim_scorer_ == nullptr) {
        return Explanation::no_match_no_details("no matching term in the index");
    }
    LeafReader *reader = context.reader();
    if (reader == nullptr || !query_.field().has_value()) {
        return Explanation::no_match_no_details("no matching term in this segment");
    }
    auto analyzed = analyze_leaf(*reader, query_.field().value(), query_.term_arrays());
    if (!analyzed.has_value()) {
        return std::unexpected(std::move(analyzed.error()));
    }
    if (!analyzed.value().has_value()) {
        return Explanation::no_match_no_details("no matching term in this segment");
    }
    auto scorer = build_scorer(*reader, query_.field().value(), query_.term_arrays(), query_.positions(),
                               query_.slop(), analyzed.value().value().match_cost, sim_scorer_);
    if (!scorer.has_value()) {
        return std::unexpected(std::move(scorer.error()));
    }
    if (scorer.value() == nullptr) {
        return Explanation::no_match_no_details("no matching term in this segment");
    }
    auto advanced = scorer.value()->advance(doc);
    if (!advanced.has_value()) {
        return std::unexpected(std::move(advanced.error()));
    }
    if (advanced.value() != doc) {
        return Explanation::no_match_no_details("no matching phrase in document");
    }
    auto matched = scorer.value()->matches();
    if (!matched.has_value()) {
        return std::unexpected(std::move(matched.error()));
    }
    if (!matched.value()) {
        return Explanation::no_match_no_details("no matching phrase in document");
    }
    auto score = scorer.value()->score();
    if (!score.has_value()) {
        return std::unexpected(std::move(score.error()));
    }
    auto query_text = query_.to_string("");
    if (!query_text.has_value()) {
        return std::unexpected(std::move(query_text.error()));
    }
    return Explanation::match_no_details(
            score.value(),
            std::format("weight({} in {}), phrase freq={}", query_text.value(), doc, scorer.value()->freq()));
}

const Query &MultiPhraseWeight::get_query() const { return query_; }

} // namespace lucene
