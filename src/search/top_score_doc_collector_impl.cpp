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

module lucene_core:top_score_doc_collector.impl;

import :top_score_doc_collector;

import :collector;
import :doc_id_set_iterator;
import :error;
import :hit_queue;
import :index_searcher;
import :leaf_collector;
import :leaf_reader_context;
import :max_score_accumulator;
import :scorable;
import :score_doc;
import :score_mode;
import :top_docs;
import :top_docs_collector;
import :total_hits;
import :types;

import std;

namespace lucene {

namespace {

/// The next representable `float` above `value`; `infinity` stays `infinity`.
///
/// The reference implementation gets this from `f32::next_up`, which is `nextafter` towards positive
/// infinity.
float next_up(float value) { return std::nextafter(value, std::numeric_limits<float>::infinity()); }

} // namespace

/// Collects one leaf into its parent [`TopScoreDocCollector`].
///
/// Port of `TopScoreDocLeafCollector` (`core/search/top_score_doc_collector.rs`).
/// It holds a borrowed reference to the collector, which owns the hit queue and
/// the hit counters.
class TopScoreDocLeafCollector final : public LeafCollector {
public:
    TopScoreDocLeafCollector(TopScoreDocCollector &base, int32_t doc_base, int32_t after_doc, float after_score)
        : base_(base), doc_base_(doc_base), after_doc_(after_doc), after_score_(after_score) {}

    [[nodiscard]] VoidResult set_scorer(Scorable &scorer) override {
        if (base_.min_score_acc() == nullptr) {
            return update_min_competitive_score(scorer);
        }
        return update_global_min_competitive_score(scorer);
    }

    [[nodiscard]] VoidResult collect(int32_t doc, Scorable &scorer) override {
        auto score_result = scorer.score();
        if (!score_result.has_value()) {
            return std::unexpected(std::move(score_result.error()));
        }
        return collect_scored(doc, scorer, score_result.value());
    }

    [[nodiscard]] bool supports_precomputed_score() const override { return true; }

    [[nodiscard]] Result<bool> collect_with_score(int32_t doc, Scorable &scorer, float score) override {
        auto collected = collect_scored(doc, scorer, score);
        if (!collected.has_value()) {
            return std::unexpected(std::move(collected.error()));
        }
        return true;
    }

    [[nodiscard]] std::string to_string() const override { return "TopScoreDocLeafCollector"; }

private:
    /// The body of both [`collect`] and [`collect_with_score`].
    [[nodiscard]] VoidResult collect_scored(int32_t doc, Scorable &scorer, float score) {
        TopDocsCollectorBase &base = base_.base();
        ++base.total_hits;
        const size_t hit_count_so_far = base.total_hits;

        const std::shared_ptr<MaxScoreAccumulator> &acc = base_.min_score_acc();
        if (acc != nullptr && (hit_count_so_far & acc->mod_interval()) == 0) {
            if (auto updated = update_global_min_competitive_score(scorer); !updated.has_value()) {
                return updated;
            }
        }

        if (base_.check_after() && base_.after().has_value() &&
            (score > after_score_ || (score == after_score_ && doc <= after_doc_))) {
            // The hit was already returned on a previous page.
            if (base.total_hits_relation == TotalHits::Relation::EqualTo) {
                // We just reached the threshold: the minimum competitive score
                // can start being published.
                return update_min_competitive_score(scorer);
            }
            return {};
        }

        const ScoreDoc *pq_top = base.pq.top();
        if (pq_top == nullptr) {
            return err(LuceneError::illegal_state("hit queue is empty"));
        }
        if (score <= pq_top->score) {
            // The common case for queries that match many documents.  Hits are
            // collected in increasing document order and the queue favours lower
            // document numbers on a score tie, so an equal score cannot compete.
            if (hit_count_so_far == base_.total_hits_threshold() + 1) {
                return update_min_competitive_score(scorer);
            }
            return {};
        }
        return collect_competitive_hit(scorer, doc, score);
    }

    [[nodiscard]] VoidResult collect_competitive_hit(Scorable &scorer, int32_t doc, float score) {
        TopDocsCollectorBase &base = base_.base();
        ScoreDoc *pq_top = base.pq.top_mut();
        if (pq_top == nullptr) {
            return err(LuceneError::illegal_state("hit queue is empty"));
        }
        pq_top->doc = doc + doc_base_;
        pq_top->score = score;
        if (auto updated = base.pq.update_top(); !updated.has_value()) {
            return std::unexpected(std::move(updated.error()));
        }
        return update_min_competitive_score(scorer);
    }

    /// Publishes the worst score still in the queue so the scorer can skip
    /// everything below it.
    [[nodiscard]] VoidResult update_min_competitive_score(Scorable &scorer) {
        TopDocsCollectorBase &base = base_.base();
        if (base.total_hits <= base_.total_hits_threshold()) {
            return {};
        }
        const ScoreDoc *pq_top = base.pq.top();
        if (pq_top == nullptr) {
            return {};
        }
        // Hits are collected in document order and ties favour lower document
        // numbers, so the next representable float is enough.
        const float local_min_score = next_up(pq_top->score);
        if (local_min_score > min_competitive_score_) {
            if (auto set = scorer.set_min_competitive_score(local_min_score); !set.has_value()) {
                return set;
            }
            base.total_hits_relation = TotalHits::Relation::GreaterThanOrEqualTo;
            min_competitive_score_ = local_min_score;
            // Register the document id (not the next float) so that other leaves,
            // or leaf partitions, can require it when they come after this leaf.
            if (const std::shared_ptr<MaxScoreAccumulator> &acc = base_.min_score_acc(); acc != nullptr) {
                acc->accumulate(pq_top->doc, pq_top->score);
            }
        }
        return {};
    }

    /// Same as above, but seeded from the shared accumulator so every leaf
    /// partition starts from the best score seen so far.
    [[nodiscard]] VoidResult update_global_min_competitive_score(Scorable &scorer) {
        const std::shared_ptr<MaxScoreAccumulator> &acc = base_.min_score_acc();
        if (acc == nullptr) {
            return err(LuceneError::illegal_state("minimum score accumulator is missing"));
        }
        const int64_t max_min_score = acc->get_raw();
        if (max_min_score == std::numeric_limits<int64_t>::min()) {
            return {};
        }
        float score = MaxScoreAccumulator::to_score(max_min_score);
        if (doc_base_ >= MaxScoreAccumulator::doc_id(max_min_score)) {
            // Ties break on the document id and hits arrive in document order,
            // so a hit from a later leaf needs strictly more.
            score = next_up(score);
        }
        if (score > min_competitive_score_) {
            if (auto set = scorer.set_min_competitive_score(score); !set.has_value()) {
                return set;
            }
            min_competitive_score_ = score;
            base_.base().total_hits_relation = TotalHits::Relation::GreaterThanOrEqualTo;
        }
        return {};
    }

    TopScoreDocCollector &base_;
    float min_competitive_score_{0.0F};
    int32_t doc_base_;
    int32_t after_doc_;
    float after_score_;
};

TopScoreDocCollector::TopScoreDocCollector(TopDocsCollectorBase base, std::optional<ScoreDoc> after,
                                           size_t total_hits_threshold,
                                           std::shared_ptr<MaxScoreAccumulator> min_score_acc, bool check_after)
    : base_(std::move(base)), after_(std::move(after)), total_hits_threshold_(total_hits_threshold),
      min_score_acc_(std::move(min_score_acc)), check_after_(check_after) {}

Result<std::unique_ptr<TopScoreDocCollector>> TopScoreDocCollector::create(
        size_t num_hits, std::optional<ScoreDoc> after, size_t total_hits_threshold,
        std::shared_ptr<MaxScoreAccumulator> min_score_acc, bool check_after) {
    if (!check_after && after.has_value()) {
        return err(LuceneError::illegal_argument("first-page collector cannot have an after cursor"));
    }
    auto queue = new_hit_queue(num_hits, /*pre_populate=*/true);
    if (!queue.has_value()) {
        return std::unexpected(std::move(queue.error()));
    }
    return std::unique_ptr<TopScoreDocCollector>(new TopScoreDocCollector(
            TopDocsCollectorBase(std::move(queue.value())), std::move(after), total_hits_threshold,
            std::move(min_score_acc), check_after));
}

ScoreMode TopScoreDocCollector::score_mode() const {
    return total_hits_threshold_ == static_cast<size_t>(std::numeric_limits<int32_t>::max()) ? ScoreMode::Complete
                                                                                            : ScoreMode::TopScores;
}

Result<std::unique_ptr<LeafCollector>> TopScoreDocCollector::get_leaf_collector(const LeafReaderContext &context,
                                                                               const Weight * /*weight*/,
                                                                               const IndexSearcher & /*searcher*/) {
    const int32_t doc_base = context.doc_base();
    float after_score = std::numeric_limits<float>::infinity();
    int32_t after_doc = NO_MORE_DOCS;
    if (check_after_ && after_.has_value()) {
        after_score = after_.value().score;
        after_doc = after_.value().doc - doc_base;
    }
    return std::unique_ptr<LeafCollector>(new TopScoreDocLeafCollector(*this, doc_base, after_doc, after_score));
}

Result<TopDocs> TopScoreDocCollector::top_docs() {
    // The queue size bounds the valid hits; the range method clamps it.
    return top_docs_with_start_limit(0, static_cast<int32_t>(base_.pq.size()));
}

Result<TopDocs> TopScoreDocCollector::top_docs_with_start(int32_t start) {
    return top_docs_with_start_limit(start, static_cast<int32_t>(base_.pq.size()));
}

Result<TopDocs> TopScoreDocCollector::top_docs_with_start_limit(int32_t start, int32_t how_many) {
    return base_.top_docs_with_start_limit(start, how_many);
}

size_t TopScoreDocCollector::top_docs_size() const {
    size_t count = 0;
    for (const ScoreDoc *hit : base_.pq.elements()) {
        if (hit->doc != std::numeric_limits<int32_t>::max()) {
            ++count;
        }
    }
    return count;
}

} // namespace lucene
