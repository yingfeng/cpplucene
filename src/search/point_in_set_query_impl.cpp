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

module lucene_core:point_in_set_query.impl;

import :point_in_set_query;

import std;

import :constant_score_scorer;
import :doc_id_set_iterator;
import :error;
import :explanation;
import :index_searcher;
import :leaf_reader;
import :leaf_reader_context;
import :point_values;
import :query;
import :query_visitor;
import :score_mode;
import :scorer;
import :scorer_supplier;
import :types;
import :weight;

namespace lucene {

namespace {

int32_t compare_bytes(std::span<const uint8_t> left, std::span<const uint8_t> right) {
    const auto result = std::lexicographical_compare_three_way(left.begin(), left.end(), right.begin(), right.end());
    if (result < 0) {
        return -1;
    }
    return result > 0 ? 1 : 0;
}

/// Collects the documents whose point is one of the accepted ones.
class SetVisitor final : public IntersectVisitor {
public:
    SetVisitor(const PointInSetQuery &query, std::set<int32_t> &docs) : query_(&query), docs_(&docs) {}

    [[nodiscard]] VoidResult visit(int32_t doc_id) override {
        docs_->insert(doc_id);
        return {};
    }

    [[nodiscard]] VoidResult visit_with_packed_value(int32_t doc_id, std::span<const uint8_t> packed_value) override {
        if (contains(packed_value)) {
            docs_->insert(doc_id);
        }
        return {};
    }

    [[nodiscard]] Result<Relation> compare(std::span<const uint8_t> min_packed_value,
                                           std::span<const uint8_t> max_packed_value) const override {
        // The accepted points are sorted, so the first one that is not below
        // the cell decides whether the cell can match at all.
        for (const std::vector<uint8_t> &point : query_->points()) {
            const std::span<const uint8_t> candidate(point);
            if (compare_bytes(candidate, min_packed_value) < 0) {
                continue;
            }
            if (compare_bytes(candidate, max_packed_value) > 0) {
                return Relation::kCellOutsideQuery;
            }
            // A single-point cell that holds an accepted point is fully inside.
            if (compare_bytes(min_packed_value, max_packed_value) == 0) {
                return Relation::kCellInsideQuery;
            }
            return Relation::kCellCrossesQuery;
        }
        return Relation::kCellOutsideQuery;
    }

private:
    [[nodiscard]] bool contains(std::span<const uint8_t> packed_value) const {
        for (const std::vector<uint8_t> &point : query_->points()) {
            if (compare_bytes(point, packed_value) == 0) {
                return true;
            }
        }
        return false;
    }

    const PointInSetQuery *query_{nullptr};
    std::set<int32_t> *docs_{nullptr};
};

/// A [`DocIdSetIterator`] over sorted documents.
class SortedDocIds final : public DocIdSetIterator {
public:
    explicit SortedDocIds(std::vector<int32_t> docs) : docs_(std::move(docs)) {}

    [[nodiscard]] int32_t doc_id() const override { return doc_; }

    [[nodiscard]] Result<int32_t> next_doc() override {
        ++index_;
        doc_ = index_ >= static_cast<int32_t>(docs_.size()) ? NO_MORE_DOCS : docs_[static_cast<size_t>(index_)];
        return doc_;
    }

    [[nodiscard]] Result<int32_t> advance(int32_t target) override {
        while (doc_ < target && doc_ != NO_MORE_DOCS) {
            auto next = next_doc();
            if (!next.has_value()) {
                return std::unexpected(std::move(next.error()));
            }
        }
        return doc_;
    }

    [[nodiscard]] Result<int64_t> cost() const override { return static_cast<int64_t>(docs_.size()); }

private:
    std::vector<int32_t> docs_;
    int32_t index_{-1};
    int32_t doc_{-1};
};

/// Hands the leaf scorer out once.
class SingleScorerSupplier final : public ScorerSupplier {
public:
    SingleScorerSupplier(std::unique_ptr<Scorer> scorer, int64_t cost) : scorer_(std::move(scorer)), cost_(cost) {}

    [[nodiscard]] Result<std::unique_ptr<Scorer>> get(int64_t /*lead_cost*/, const LeafReaderContext & /*context*/,
                                                      const IndexSearcher & /*searcher*/) override {
        return std::move(scorer_);
    }

    [[nodiscard]] Result<int64_t> cost(const LeafReaderContext & /*context*/,
                                       const IndexSearcher & /*searcher*/) override {
        return cost_;
    }

private:
    std::unique_ptr<Scorer> scorer_;
    int64_t cost_{0};
};

} // namespace

PointInSetQuery::PointInSetQuery(std::string field, std::vector<std::vector<uint8_t>> points, size_t num_dims)
        : field_(std::move(field)), points_(std::move(points)), num_dims_(num_dims),
          bytes_per_dim_(num_dims == 0 || points_.empty() ? 0 : points_.front().size() / num_dims) {}

Result<PointInSetQuery> PointInSetQuery::create(std::string field, std::vector<std::vector<uint8_t>> points,
                                                size_t num_dims) {
    if (num_dims == 0) {
        return err(LuceneError::illegal_argument("num_dims must be greater than zero"));
    }
    if (points.empty()) {
        return err(LuceneError::illegal_argument("the set of points must not be empty"));
    }
    const size_t length = points.front().size();
    if (length == 0) {
        return err(LuceneError::illegal_argument("a point has length of zero"));
    }
    if (length % num_dims != 0) {
        return err(LuceneError::illegal_argument("a point is not a fixed multiple of num_dims"));
    }
    for (const std::vector<uint8_t> &point : points) {
        if (point.size() != length) {
            return err(LuceneError::illegal_argument("all points of a set query need the same length"));
        }
    }
    std::ranges::sort(points);
    points.erase(std::unique(points.begin(), points.end()), points.end());
    return PointInSetQuery(std::move(field), std::move(points), num_dims);
}

Result<std::unique_ptr<Weight>> PointInSetQuery::create_weight(const IndexSearcher &searcher, ScoreMode score_mode,
                                                               float boost) const {
    (void)searcher;
    return std::unique_ptr<Weight>(new PointInSetWeight(*this, boost, score_mode));
}

VoidResult PointInSetQuery::visit(QueryVisitor &visitor) const {
    if (visitor.accept_field(field_)) {
        return visitor.visit_leaf(*this);
    }
    return {};
}

Result<std::string> PointInSetQuery::to_string(std::string_view field) const {
    const auto hex = [](std::span<const uint8_t> bytes) {
        std::string text;
        for (const uint8_t byte : bytes) {
            text += std::format("{:02x}", byte);
        }
        return text;
    };

    std::string buffer;
    if (field != field_) {
        buffer += field_;
        buffer += ':';
    }
    buffer += '{';
    for (size_t i = 0; i < points_.size(); ++i) {
        if (i > 0) {
            buffer += ',';
        }
        buffer += hex(std::span<const uint8_t>(points_[i]));
    }
    buffer += '}';
    return buffer;
}

bool PointInSetQuery::equals(const Query &other) const {
    const auto *set = dynamic_cast<const PointInSetQuery *>(&other);
    if (set == nullptr) {
        return false;
    }
    return field_ == set->field_ && num_dims_ == set->num_dims_ && bytes_per_dim_ == set->bytes_per_dim_ &&
           points_ == set->points_;
}

size_t PointInSetQuery::hash() const {
    size_t result = std::hash<std::string>{}(field_);
    for (const std::vector<uint8_t> &point : points_) {
        for (const uint8_t byte : point) {
            result = result * 31 + byte;
        }
        result = result * 31 + 17;
    }
    return result * 31 + num_dims_;
}

PointInSetWeight::PointInSetWeight(const PointInSetQuery &query, float score, ScoreMode score_mode)
        : query_(query), score_(score), score_mode_(score_mode) {}

Result<std::unique_ptr<Scorer>> PointInSetWeight::scorer_for(const LeafReaderContext &context) const {
    LeafReader *reader = context.reader();
    if (reader == nullptr) {
        return std::unique_ptr<Scorer>{};
    }
    auto values = reader->point_values(query_.field());
    if (!values.has_value()) {
        return std::unexpected(std::move(values.error()));
    }
    if (values.value() == nullptr) {
        return std::unique_ptr<Scorer>{};
    }
    PointValues &points = *values.value();

    auto num_dims = points.get_num_index_dimensions();
    if (!num_dims.has_value()) {
        return std::unexpected(std::move(num_dims.error()));
    }
    if (num_dims.value() != query_.num_dims()) {
        return err(LuceneError::illegal_argument(std::format(
                "field=\"{}\" was indexed with numIndexDimensions={} but this query has numDims={}", query_.field(),
                num_dims.value(), query_.num_dims())));
    }
    auto bytes_per_dim = points.get_bytes_per_dimension();
    if (!bytes_per_dim.has_value()) {
        return std::unexpected(std::move(bytes_per_dim.error()));
    }
    if (bytes_per_dim.value() != query_.bytes_per_dim()) {
        return err(LuceneError::illegal_argument(std::format(
                "field=\"{}\" was indexed with bytesPerDim={} but this query has bytesPerDim={}", query_.field(),
                bytes_per_dim.value(), query_.bytes_per_dim())));
    }

    std::set<int32_t> matching;
    SetVisitor visitor(query_, matching);
    if (auto intersected = points.intersect(visitor); !intersected.has_value()) {
        return std::unexpected(std::move(intersected.error()));
    }
    if (matching.empty()) {
        return std::unique_ptr<Scorer>{};
    }
    std::vector<int32_t> docs(matching.begin(), matching.end());
    std::unique_ptr<DocIdSetIterator> iterator(new SortedDocIds(std::move(docs)));
    return std::unique_ptr<Scorer>(ConstantScoreScorer::from_disi(score_, score_mode_, std::move(iterator)));
}

Result<std::unique_ptr<ScorerSupplier>> PointInSetWeight::scorer_supplier(const LeafReaderContext &context,
                                                                         const IndexSearcher &searcher) {
    (void)searcher;
    auto scorer = scorer_for(context);
    if (!scorer.has_value()) {
        return std::unexpected(std::move(scorer.error()));
    }
    if (scorer.value() == nullptr) {
        return std::unique_ptr<ScorerSupplier>{};
    }
    auto cost = scorer.value()->cost();
    if (!cost.has_value()) {
        return std::unexpected(std::move(cost.error()));
    }
    std::unique_ptr<ScorerSupplier> supplier(new SingleScorerSupplier(std::move(scorer.value()), cost.value()));
    return supplier;
}

Result<Explanation> PointInSetWeight::explain(const LeafReaderContext &context, int32_t doc,
                                              const IndexSearcher & /*searcher*/) const {
    auto scorer = scorer_for(context);
    if (!scorer.has_value()) {
        return std::unexpected(std::move(scorer.error()));
    }
    if (scorer.value() == nullptr) {
        return Explanation::no_match_no_details("no points for the field in this segment");
    }
    auto advanced = scorer.value()->advance(doc);
    if (!advanced.has_value()) {
        return std::unexpected(std::move(advanced.error()));
    }
    if (advanced.value() != doc) {
        return Explanation::no_match_no_details("the point of the document is not in the set");
    }
    auto score = scorer.value()->score();
    if (!score.has_value()) {
        return std::unexpected(std::move(score.error()));
    }
    return Explanation::match_no_details(score.value(), std::format("point in set matched, score={}", score.value()));
}

} // namespace lucene
