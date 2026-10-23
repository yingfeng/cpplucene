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

module lucene_core:phrase_query.impl;

import :phrase_query;

import std;

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
import :weight;

namespace lucene {

namespace {

/// Scores nothing; used when the query's scores are not needed.
class PhraseDummySimScorer final : public SimScorer {
public:
    [[nodiscard]] float score(float /*freq*/, int64_t /*norm*/) const override { return 0.0F; }
};

std::shared_ptr<const SimScorer> dummy_sim_scorer() {
    static const auto instance = std::make_shared<const PhraseDummySimScorer>();
    return instance;
}

/// What one leaf costs the phrase: its smallest document frequency and the
/// expected cost of one match.
struct LeafPhrase {
    int64_t cost{0};
    float match_cost{0.0F};
};

/// Reads the document frequency of every term of the phrase in `reader`;
/// `nullopt` when one of the terms is missing from the leaf.
Result<std::optional<LeafPhrase>> analyze_leaf(LeafReader &reader, std::string_view field,
                                               const std::vector<Term> &terms) {
    auto field_terms = reader.terms(field);
    if (!field_terms.has_value()) {
        return std::unexpected(std::move(field_terms.error()));
    }
    if (field_terms.value() == nullptr) {
        return std::optional<LeafPhrase>{};
    }
    Terms &dictionary = *field_terms.value();
    auto positions_enum = dictionary.iterator();
    if (!positions_enum.has_value()) {
        return std::unexpected(std::move(positions_enum.error()));
    }

    LeafPhrase info{.cost = std::numeric_limits<int64_t>::max(), .match_cost = 0.0F};
    for (const Term &term : terms) {
        auto found = positions_enum.value()->seek_exact(term.bytes().as_slice());
        if (!found.has_value()) {
            return std::unexpected(std::move(found.error()));
        }
        if (!found.value()) {
            return std::optional<LeafPhrase>{};
        }
        auto frequency = positions_enum.value()->doc_freq();
        if (!frequency.has_value()) {
            return std::unexpected(std::move(frequency.error()));
        }
        const int64_t doc_freq = frequency.value();
        if (doc_freq <= 0) {
            return std::optional<LeafPhrase>{};
        }
        info.cost = std::min(info.cost, doc_freq);
        info.match_cost += 1.0F + std::log(static_cast<float>(doc_freq));
    }
    if (info.cost == std::numeric_limits<int64_t>::max()) {
        info.cost = 0;
    }
    return std::optional<LeafPhrase>(info);
}

/// Opens the postings of every term of the phrase, sorted by ascending document
/// frequency (the exact matcher expects its lead first); `nullptr` when a term is
/// missing from the leaf, i.e. the leaf cannot match.
Result<std::unique_ptr<PhraseScorer>> build_phrase_scorer(LeafReader &reader, std::string_view field,
                                                          const std::vector<Term> &terms,
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
    auto terms_enum = dictionary.iterator();
    if (!terms_enum.has_value()) {
        return std::unexpected(std::move(terms_enum.error()));
    }

    std::vector<std::pair<int64_t, PostingsAndFreq>> opened;
    opened.reserve(terms.size());
    for (size_t i = 0; i < terms.size(); ++i) {
        auto found = terms_enum.value()->seek_exact(terms[i].bytes().as_slice());
        if (!found.has_value()) {
            return std::unexpected(std::move(found.error()));
        }
        if (!found.value()) {
            return std::unique_ptr<PhraseScorer>{};
        }
        auto doc_freq = terms_enum.value()->doc_freq();
        if (!doc_freq.has_value()) {
            return std::unexpected(std::move(doc_freq.error()));
        }
        auto postings = terms_enum.value()->postings_with_flags(nullptr, POSITIONS);
        if (!postings.has_value()) {
            return std::unexpected(std::move(postings.error()));
        }
        if (postings.value() == nullptr) {
            return std::unique_ptr<PhraseScorer>{};
        }
        opened.emplace_back(doc_freq.value(),
                            PostingsAndFreq{.postings = std::move(postings.value()),
                                            .position = positions[i],
                                            .terms = {terms[i]}});
    }

    // The rarest term leads the exact matcher, which keeps the number of
    // position lookups low.
    std::ranges::sort(opened, [](const auto &left, const auto &right) { return left.first < right.first; });
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
    return std::unique_ptr<PhraseScorer>(new PhraseScorer(std::move(matcher), std::move(sim_scorer),
                                                          std::move(norms.value())));
}

/// Supplies the phrase scorer of one leaf.
class PhraseScorerSupplier final : public ScorerSupplier {
public:
    PhraseScorerSupplier(std::string field, std::vector<Term> terms, std::vector<int32_t> positions, int32_t slop,
                         int64_t cost, float match_cost, std::shared_ptr<const SimScorer> sim_scorer)
            : field_(std::move(field)), terms_(std::move(terms)), positions_(std::move(positions)), slop_(slop),
              cost_(cost), match_cost_(match_cost), sim_scorer_(std::move(sim_scorer)) {}

    [[nodiscard]] Result<std::unique_ptr<Scorer>> get(int64_t /*lead_cost*/, const LeafReaderContext &context,
                                                      const IndexSearcher & /*searcher*/) override {
        LeafReader *reader = context.reader();
        if (reader == nullptr) {
            return std::unique_ptr<Scorer>{};
        }
        auto scorer = build_phrase_scorer(*reader, field_, terms_, positions_, slop_, match_cost_, sim_scorer_);
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
    std::vector<Term> terms_;
    std::vector<int32_t> positions_;
    int32_t slop_{0};
    int64_t cost_{0};
    float match_cost_{0.0F};
    std::shared_ptr<const SimScorer> sim_scorer_;
};

} // namespace

// --------------------------------------------------------------- PhraseQuery

PhraseQuery::PhraseQuery(std::string field, std::vector<Term> terms, std::vector<int32_t> positions, int32_t slop)
        : field_(std::move(field)), terms_(std::move(terms)), positions_(std::move(positions)), slop_(slop) {}

Result<PhraseQuery> PhraseQuery::create(std::string field, std::vector<Term> terms) {
    const size_t length = terms.size();
    std::vector<int32_t> positions;
    positions.reserve(length);
    for (size_t i = 0; i < length; ++i) {
        positions.push_back(static_cast<int32_t>(i));
    }
    return with_positions(std::move(field), std::move(terms), std::move(positions), DEFAULT_SLOP);
}

Result<PhraseQuery> PhraseQuery::with_slop(std::string field, std::vector<Term> terms, int32_t slop) {
    const size_t length = terms.size();
    std::vector<int32_t> positions;
    positions.reserve(length);
    for (size_t i = 0; i < length; ++i) {
        positions.push_back(static_cast<int32_t>(i));
    }
    return with_positions(std::move(field), std::move(terms), std::move(positions), slop);
}

Result<PhraseQuery> PhraseQuery::with_positions(std::string field, std::vector<Term> terms,
                                                std::vector<int32_t> positions, int32_t slop) {
    if (terms.empty()) {
        return err(LuceneError::illegal_argument("phrase must contain at least one term"));
    }
    if (terms.size() != positions.size()) {
        return err(LuceneError::illegal_argument(
                std::format("The number of positions must match the number of terms: got {} positions for {} terms",
                            positions.size(), terms.size())));
    }
    if (slop < 0) {
        return err(LuceneError::illegal_argument("slop must be greater than or equal to 0"));
    }
    for (size_t i = 0; i < terms.size(); ++i) {
        if (terms[i].field() != field) {
            return err(LuceneError::illegal_argument(
                    std::format("All terms of a phrase must be on the same field, got {} and {}", field,
                                terms[i].field())));
        }
        if (positions[i] < 0) {
            return err(LuceneError::illegal_argument("positions must be greater than or equal to 0"));
        }
        if (i > 0 && positions[i] < positions[i - 1]) {
            return err(LuceneError::illegal_argument(
                    std::format("Positions must be added in order, got {} after {}", positions[i], positions[i - 1])));
        }
    }
    return PhraseQuery(std::move(field), std::move(terms), std::move(positions), slop);
}

Result<PhraseQuery> PhraseQuery::from_terms(std::vector<Term> terms) {
    return from_terms_with_slop(std::move(terms), DEFAULT_SLOP);
}

Result<PhraseQuery> PhraseQuery::from_terms_with_slop(std::vector<Term> terms, int32_t slop) {
    const size_t length = terms.size();
    std::vector<int32_t> positions;
    positions.reserve(length);
    for (size_t i = 0; i < length; ++i) {
        positions.push_back(static_cast<int32_t>(i));
    }
    return from_terms_and_positions(std::move(terms), std::move(positions), slop);
}

Result<PhraseQuery> PhraseQuery::from_terms_and_positions(std::vector<Term> terms, std::vector<int32_t> positions,
                                                          int32_t slop) {
    if (terms.empty()) {
        return err(LuceneError::illegal_argument("phrase must contain at least one term"));
    }
    const std::string field = terms.front().field();
    return with_positions(field, std::move(terms), std::move(positions), slop);
}

Result<PhraseQuery> PhraseQuery::from_term_texts(std::string field, std::vector<std::string> texts) {
    std::vector<Term> terms;
    terms.reserve(texts.size());
    for (const std::string &text : texts) {
        terms.push_back(Term::from_text(field, text));
    }
    return create(std::move(field), std::move(terms));
}

Result<std::unique_ptr<Weight>> PhraseQuery::create_weight(const IndexSearcher &searcher, ScoreMode score_mode,
                                                           float boost) const {
    auto weight = PhraseWeight::create(*this, boost, score_mode, searcher);
    if (!weight.has_value()) {
        return std::unexpected(std::move(weight.error()));
    }
    std::unique_ptr<Weight> result = std::move(weight.value());
    return result;
}

Result<std::shared_ptr<Query>> PhraseQuery::rewrite(const IndexSearcher &searcher) const {
    (void)searcher;
    if (terms_.empty()) {
        return std::shared_ptr<Query>(std::make_shared<MatchNoDocsQuery>());
    }
    if (terms_.size() == 1) {
        return std::shared_ptr<Query>(std::make_shared<TermQuery>(terms_.front()));
    }
    return std::shared_ptr<Query>{};
}

VoidResult PhraseQuery::visit(QueryVisitor &visitor) const {
    if (visitor.accept_field(field_)) {
        return visitor.consume_terms(*this, terms_);
    }
    return {};
}

Result<std::string> PhraseQuery::to_string(std::string_view field) const {
    std::string buffer;
    if (field != field_) {
        buffer += field_;
        buffer += ':';
    }
    buffer += '"';

    const std::optional<int32_t> max_position =
            positions_.empty() ? std::optional<int32_t>{} : std::optional<int32_t>(positions_.back());
    std::vector<std::optional<std::string>> pieces;
    if (max_position.has_value()) {
        pieces.resize(static_cast<size_t>(max_position.value()) + 1);
    }

    for (size_t i = 0; i < terms_.size(); ++i) {
        const std::string text = terms_[i].text();
        std::optional<std::string> &piece = pieces[static_cast<size_t>(positions_[i])];
        if (!piece.has_value()) {
            piece = text;
        } else {
            piece = piece.value() + "|" + text;
        }
    }

    for (size_t i = 0; i < pieces.size(); ++i) {
        if (i > 0) {
            buffer += ' ';
        }
        buffer += pieces[i].has_value() ? pieces[i].value() : "?";
    }

    buffer += '"';
    if (slop_ != 0) {
        buffer += std::format("~{}", slop_);
    }
    return buffer;
}

bool PhraseQuery::equals(const Query &other) const {
    const auto *phrase = dynamic_cast<const PhraseQuery *>(&other);
    if (phrase == nullptr) {
        return false;
    }
    return field_ == phrase->field_ && terms_ == phrase->terms_ && positions_ == phrase->positions_ &&
           slop_ == phrase->slop_;
}

size_t PhraseQuery::hash() const {
    size_t result = std::hash<std::string>{}(field_);
    for (const Term &term : terms_) {
        result = result * 31 + std::hash<std::string>{}(term.field());
        for (const uint8_t byte : term.bytes().as_slice()) {
            result = result * 31 + byte;
        }
    }
    for (const int32_t position : positions_) {
        result = result * 31 + static_cast<size_t>(position);
    }
    return result * 31 + static_cast<size_t>(slop_);
}

// -------------------------------------------------------------- PhraseWeight

PhraseWeight::PhraseWeight(const PhraseQuery &query, float boost, ScoreMode score_mode,
                           std::shared_ptr<const Similarity> similarity, std::shared_ptr<const SimScorer> sim_scorer)
        : query_(query), boost_(boost), score_mode_(score_mode), similarity_(std::move(similarity)),
          sim_scorer_(std::move(sim_scorer)) {}

Result<std::unique_ptr<PhraseWeight>> PhraseWeight::create(const PhraseQuery &query, float boost, ScoreMode score_mode,
                                                           const IndexSearcher &searcher) {
    auto weight = std::unique_ptr<PhraseWeight>(
            new PhraseWeight(query, boost, score_mode, searcher.similarity_ptr(), nullptr));

    if (!score_mode_needs_scores(score_mode)) {
        // Without scores the statistics are irrelevant.
        weight->sim_scorer_ = dummy_sim_scorer();
        return weight;
    }

    auto collection_stats = searcher.collection_statistics(query.field());
    if (!collection_stats.has_value()) {
        return std::unexpected(std::move(collection_stats.error()));
    }
    if (!collection_stats.value().has_value()) {
        // The field does not exist, so the phrase cannot match.
        return weight;
    }

    std::vector<TermStatistics> term_stats;
    term_stats.reserve(query.terms().size());
    for (const Term &term : query.terms()) {
        auto doc_freq = searcher.doc_freq(term);
        if (!doc_freq.has_value()) {
            return std::unexpected(std::move(doc_freq.error()));
        }
        if (doc_freq.value() <= 0) {
            // A term missing from the whole index means the phrase matches nothing.
            return weight;
        }
        auto total_term_freq = searcher.total_term_freq(term);
        if (!total_term_freq.has_value()) {
            return std::unexpected(std::move(total_term_freq.error()));
        }
        const int64_t occurrences =
                std::max<int64_t>(total_term_freq.value(), static_cast<int64_t>(doc_freq.value()));
        auto created = searcher.term_statistics(term, doc_freq.value(), occurrences);
        if (!created.has_value()) {
            return std::unexpected(std::move(created.error()));
        }
        term_stats.push_back(std::move(created.value()));
    }

    auto scorer = searcher.similarity().scorer(boost, collection_stats.value().value(), term_stats);
    if (!scorer.has_value()) {
        return std::unexpected(std::move(scorer.error()));
    }
    weight->sim_scorer_ = std::move(scorer.value());
    return weight;
}

Result<std::unique_ptr<PhraseScorer>> PhraseWeight::scorer_for(const LeafReaderContext &context) const {
    LeafReader *reader = context.reader();
    if (reader == nullptr || sim_scorer_ == nullptr) {
        return std::unique_ptr<PhraseScorer>{};
    }
    auto analyzed = analyze_leaf(*reader, query_.field(), query_.terms());
    if (!analyzed.has_value()) {
        return std::unexpected(std::move(analyzed.error()));
    }
    if (!analyzed.value().has_value()) {
        return std::unique_ptr<PhraseScorer>{};
    }
    return build_phrase_scorer(*reader, query_.field(), query_.terms(), query_.positions(), query_.slop(),
                               analyzed.value().value().match_cost, sim_scorer_);
}

Result<std::unique_ptr<ScorerSupplier>> PhraseWeight::scorer_supplier(const LeafReaderContext &context,
                                                                      const IndexSearcher &searcher) {
    (void)searcher;
    if (sim_scorer_ == nullptr) {
        return std::unique_ptr<ScorerSupplier>{};
    }
    LeafReader *reader = context.reader();
    if (reader == nullptr) {
        return std::unique_ptr<ScorerSupplier>{};
    }
    auto analyzed = analyze_leaf(*reader, query_.field(), query_.terms());
    if (!analyzed.has_value()) {
        return std::unexpected(std::move(analyzed.error()));
    }
    if (!analyzed.value().has_value()) {
        return std::unique_ptr<ScorerSupplier>{};
    }
    std::unique_ptr<ScorerSupplier> supplier(new PhraseScorerSupplier(
            query_.field(), query_.terms(), query_.positions(), query_.slop(), analyzed.value().value().cost,
            analyzed.value().value().match_cost, sim_scorer_));
    return supplier;
}

Result<int32_t> PhraseWeight::count(const LeafReaderContext & /*context*/, const IndexSearcher & /*searcher*/) {
    return -1;
}

Result<bool> PhraseWeight::is_cacheable(const LeafReaderContext & /*context*/) const { return true; }

Result<Explanation> PhraseWeight::explain(const LeafReaderContext &context, int32_t doc,
                                          const IndexSearcher &searcher) const {
    (void)searcher;
    if (sim_scorer_ == nullptr) {
        return Explanation::no_match_no_details("no matching term in the index");
    }
    auto scorer = scorer_for(context);
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

const Query &PhraseWeight::get_query() const { return query_; }

} // namespace lucene
