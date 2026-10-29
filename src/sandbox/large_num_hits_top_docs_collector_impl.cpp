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

module lucene_core:large_num_hits_top_docs_collector.impl;

import :large_num_hits_top_docs_collector;

import :collector;
import :error;
import :hit_queue;
import :leaf_collector;
import :leaf_reader_context;
import :priority_queue;
import :scorable;
import :score_doc;
import :score_mode;
import :top_docs;
import :total_hits;
import :types;

import std;

namespace lucene {

class LargeNumHitsLeafCollector final : public LeafCollector {
public:
    LargeNumHitsLeafCollector(LargeNumHitsTopDocsCollector &base, int32_t doc_base)
        : base_(&base), doc_base_(doc_base) {}

    [[nodiscard]] VoidResult collect(int32_t doc, Scorable &scorer) override;

    [[nodiscard]] std::string to_string() const override { return "LargeNumHitsTopDocsLeafCollector"; }

private:
    LargeNumHitsTopDocsCollector *base_;
    int32_t doc_base_;
};


LargeNumHitsTopDocsCollector::LargeNumHitsTopDocsCollector(size_t requested_hit_count)
    : requested_hit_count_(requested_hit_count), hits_(std::vector<ScoreDoc>{}) {}

Result<std::vector<ScoreDoc>> LargeNumHitsTopDocsCollector::populate_results(size_t how_many) {
    if (pq_.has_value()) {
        std::vector<ScoreDoc> results(how_many);
        for (size_t i = how_many; i > 0; --i) {
            auto popped = pq_->pop_unchecked();
            if (!popped.has_value()) {
                return std::unexpected(std::move(popped.error()));
            }
            results[i - 1] = std::move(popped.value());
        }
        return results;
    }

    // Fewer hits than requested: sort the vector instead of building a queue.
    if (!hits_.has_value()) {
        return std::unexpected(LuceneError::illegal_state("hits list has already been converted to a priority queue"));
    }
    std::vector<ScoreDoc> &hits = *hits_;
    std::stable_sort(hits.begin(), hits.end(), [](const ScoreDoc &left, const ScoreDoc &right) {
        if (left.score != right.score) {
            return left.score > right.score;
        }
        return left.doc < right.doc;
    });
    const size_t count = std::min(how_many, hits.size());
    return std::vector<ScoreDoc>(hits.begin(), hits.begin() + static_cast<std::ptrdiff_t>(count));
}

Result<TopDocs> LargeNumHitsTopDocsCollector::top_docs_with_how_many(size_t how_many) {
    if (how_many == 0 || how_many > total_hits_) {
        return std::unexpected(LuceneError::illegal_argument("Incorrect number of hits requested"));
    }
    auto results = populate_results(how_many);
    if (!results.has_value()) {
        return std::unexpected(std::move(results.error()));
    }
    return TopDocs::create(TotalHits::create(total_hits_, TotalHits::Relation::EqualTo), std::move(results.value()));
}

Result<TopDocs> LargeNumHitsTopDocsCollector::top_docs() {
    return top_docs_with_how_many(std::min(total_hits_, requested_hit_count_));
}

Result<std::unique_ptr<LeafCollector>> LargeNumHitsTopDocsCollector::get_leaf_collector(
    const LeafReaderContext &context, const Weight *, const IndexSearcher &) {
    return std::unique_ptr<LeafCollector>(new LargeNumHitsLeafCollector(*this, context.doc_base()));
}

VoidResult LargeNumHitsLeafCollector::collect(int32_t doc, Scorable &scorer) {
    auto score = scorer.score();
    if (!score.has_value()) {
        return std::unexpected(std::move(score.error()));
    }
    const float value = score.value();
    const int32_t global_doc = doc + doc_base_;

    if (base_->total_hits_ < base_->requested_hit_count_) {
        if (!base_->hits_.has_value()) {
            return std::unexpected(
                LuceneError::illegal_state("hits list has already been converted to a priority queue"));
        }
        base_->hits_->push_back(ScoreDoc{value, global_doc});
        ++base_->total_hits_;
        return {};
    }
    if (base_->total_hits_ == base_->requested_hit_count_) {
        // The vector is full: turn it into a priority queue.  This happens once.
        auto queue = new_hit_queue(base_->requested_hit_count_, false);
        if (!queue.has_value()) {
            return std::unexpected(std::move(queue.error()));
        }
        HitQueue built = std::move(queue.value());
        if (base_->hits_.has_value()) {
            for (const ScoreDoc &hit : *base_->hits_) {
                auto added = built.add(hit);
                if (!added.has_value()) {
                    return std::unexpected(std::move(added.error()));
                }
            }
            base_->hits_.reset();
        }
        base_->pq_.emplace(std::move(built));
    }

    if (!base_->pq_.has_value()) {
        return std::unexpected(LuceneError::illegal_state("priority queue has not been built"));
    }
    HitQueue &queue = *base_->pq_;
    ScoreDoc *top = queue.top_mut();
    if (top == nullptr) {
        return std::unexpected(LuceneError::illegal_state("priority queue is empty"));
    }
    if (value > top->score) {
        top->doc = global_doc;
        top->score = value;
        auto updated = queue.update_top();
        if (!updated.has_value()) {
            return std::unexpected(std::move(updated.error()));
        }
    }
    ++base_->total_hits_;
    return {};
}

} // namespace lucene
