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

module lucene_core:point_range_query.impl;

import :point_range_query;

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

/// Compares the `bytes_per_dim` bytes starting at `offset` of two packed values.
int32_t compare_dimension(std::span<const uint8_t> left, size_t left_offset, std::span<const uint8_t> right,
                          size_t right_offset, size_t bytes_per_dim) {
    for (size_t i = 0; i < bytes_per_dim; ++i) {
        const uint8_t a = left[left_offset + i];
        const uint8_t b = right[right_offset + i];
        if (a != b) {
            return a < b ? -1 : 1;
        }
    }
    return 0;
}

/// Whether `packed_value` lies inside the query, in every dimension.
bool matches(const PointRangeQuery &query, std::span<const uint8_t> packed_value) {
    const size_t bytes_per_dim = query.bytes_per_dim();
    const std::span<const uint8_t> lower(query.lower_point());
    const std::span<const uint8_t> upper(query.upper_point());
    for (size_t dim = 0; dim < query.num_dims(); ++dim) {
        const size_t offset = dim * bytes_per_dim;
        if (compare_dimension(packed_value, offset, lower, offset, bytes_per_dim) < 0) {
            return false;
        }
        if (compare_dimension(packed_value, offset, upper, offset, bytes_per_dim) > 0) {
            return false;
        }
    }
    return true;
}

/// How a cell of the point tree relates to the query.
Relation relate(const PointRangeQuery &query, std::span<const uint8_t> min_packed_value,
                std::span<const uint8_t> max_packed_value) {
    const size_t bytes_per_dim = query.bytes_per_dim();
    const std::span<const uint8_t> lower(query.lower_point());
    const std::span<const uint8_t> upper(query.upper_point());
    bool crosses = false;
    for (size_t dim = 0; dim < query.num_dims(); ++dim) {
        const size_t offset = dim * bytes_per_dim;
        if (compare_dimension(min_packed_value, offset, upper, offset, bytes_per_dim) > 0 ||
            compare_dimension(max_packed_value, offset, lower, offset, bytes_per_dim) < 0) {
            return Relation::kCellOutsideQuery;
        }
        if (compare_dimension(min_packed_value, offset, lower, offset, bytes_per_dim) < 0 ||
            compare_dimension(max_packed_value, offset, upper, offset, bytes_per_dim) > 0) {
            crosses = true;
        }
    }
    return crosses ? Relation::kCellCrossesQuery : Relation::kCellInsideQuery;
}

/// Collects the matching documents of one leaf.
class RangeVisitor final : public IntersectVisitor {
public:
    RangeVisitor(const PointRangeQuery &query, std::set<int32_t> &docs) : query_(&query), docs_(&docs) {}

    [[nodiscard]] VoidResult visit(int32_t doc_id) override {
        docs_->insert(doc_id);
        return {};
    }

    [[nodiscard]] VoidResult visit_with_packed_value(int32_t doc_id, std::span<const uint8_t> packed_value) override {
        if (matches(*query_, packed_value)) {
            docs_->insert(doc_id);
        }
        return {};
    }

    [[nodiscard]] Result<Relation> compare(std::span<const uint8_t> min_packed_value,
                                           std::span<const uint8_t> max_packed_value) const override {
        return relate(*query_, min_packed_value, max_packed_value);
    }

private:
    const PointRangeQuery *query_{nullptr};
    std::set<int32_t> *docs_{nullptr};
};

/// A [`DocIdSetIterator`] over the sorted documents of a set.
class SortedDocIdsIterator final : public DocIdSetIterator {
public:
    explicit SortedDocIdsIterator(std::vector<int32_t> docs) : docs_(std::move(docs)) {}

    [[nodiscard]] int32_t doc_id() const override { return doc_; }

    [[nodiscard]] Result<int32_t> next_doc() override {
        ++index_;
        return set_doc();
    }

    [[nodiscard]] Result<int32_t> advance(int32_t target) override {
        while (true) {
            if (doc_ >= target) {
                return doc_;
            }
            if (doc_ == NO_MORE_DOCS) {
                return doc_;
            }
            auto next = next_doc();
            if (!next.has_value()) {
                return std::unexpected(std::move(next.error()));
            }
        }
    }

    [[nodiscard]] Result<int64_t> cost() const override { return static_cast<int64_t>(docs_.size()); }

private:
    [[nodiscard]] int32_t set_doc() {
        if (index_ >= static_cast<int32_t>(docs_.size())) {
            doc_ = NO_MORE_DOCS;
        } else {
            doc_ = docs_[static_cast<size_t>(index_)];
        }
        return doc_;
    }

    std::vector<int32_t> docs_;
    int32_t index_{-1};
    int32_t doc_{-1};
};

/// A supplier that hands out the scorer of one leaf.
class PointRangeScorerSupplier final : public ScorerSupplier {
public:
    PointRangeScorerSupplier(std::unique_ptr<Scorer> scorer, int64_t cost)
            : scorer_(std::move(scorer)), cost_(cost) {}

    /// The scorer is only handed out once; a second request rebuilds nothing and
    /// answers a null pointer, which the search loop treats as "no match".
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

PointRangeQuery::PointRangeQuery(std::string field, std::vector<uint8_t> lower_point, std::vector<uint8_t> upper_point,
                                 size_t num_dims)
        : field_(std::move(field)), lower_point_(std::move(lower_point)), upper_point_(std::move(upper_point)),
          num_dims_(num_dims), bytes_per_dim_(num_dims == 0 ? 0 : lower_point_.size() / num_dims) {}

Result<PointRangeQuery> PointRangeQuery::create(std::string field, std::vector<uint8_t> lower_point,
                                                std::vector<uint8_t> upper_point, size_t num_dims) {
    if (num_dims == 0) {
        return err(LuceneError::illegal_argument("num_dims must be greater than zero"));
    }
    if (lower_point.empty()) {
        return err(LuceneError::illegal_argument("lower_point has length of zero"));
    }
    if (lower_point.size() % num_dims != 0) {
        return err(LuceneError::illegal_argument("lower_point is not a fixed multiple of num_dims"));
    }
    if (lower_point.size() != upper_point.size()) {
        return err(LuceneError::illegal_argument(
                std::format("lower_point has length={} but upper_point has different length={}",
                            lower_point.size(), upper_point.size())));
    }
    return PointRangeQuery(std::move(field), std::move(lower_point), std::move(upper_point), num_dims);
}

Result<std::unique_ptr<Weight>> PointRangeQuery::create_weight(const IndexSearcher &searcher, ScoreMode score_mode,
                                                               float boost) const {
    (void)searcher;
    return std::unique_ptr<Weight>(new PointRangeWeight(*this, boost, score_mode));
}

VoidResult PointRangeQuery::visit(QueryVisitor &visitor) const {
    if (visitor.accept_field(field_)) {
        return visitor.visit_leaf(*this);
    }
    return {};
}

Result<std::string> PointRangeQuery::to_string(std::string_view field) const {
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
    for (size_t dim = 0; dim < num_dims_; ++dim) {
        if (dim > 0) {
            buffer += ',';
        }
        const size_t start = dim * bytes_per_dim_;
        const std::span<const uint8_t> lower(lower_point_.data() + start, bytes_per_dim_);
        const std::span<const uint8_t> upper(upper_point_.data() + start, bytes_per_dim_);
        buffer += std::format("[{} TO {}]", hex(lower), hex(upper));
    }
    return buffer;
}

bool PointRangeQuery::equals(const Query &other) const {
    const auto *range = dynamic_cast<const PointRangeQuery *>(&other);
    if (range == nullptr) {
        return false;
    }
    return field_ == range->field_ && num_dims_ == range->num_dims_ && bytes_per_dim_ == range->bytes_per_dim_ &&
           lower_point_ == range->lower_point_ && upper_point_ == range->upper_point_;
}

size_t PointRangeQuery::hash() const {
    size_t result = std::hash<std::string>{}(field_);
    for (const uint8_t byte : lower_point_) {
        result = result * 31 + byte;
    }
    for (const uint8_t byte : upper_point_) {
        result = result * 31 + byte;
    }
    return result * 31 + num_dims_;
}

PointRangeWeight::PointRangeWeight(const PointRangeQuery &query, float score, ScoreMode score_mode)
        : query_(query), score_(score), score_mode_(score_mode) {}

Result<std::unique_ptr<Scorer>> PointRangeWeight::scorer_for(const LeafReaderContext &context) const {
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
    RangeVisitor visitor(query_, matching);
    if (auto intersected = points.intersect(visitor); !intersected.has_value()) {
        return std::unexpected(std::move(intersected.error()));
    }
    if (matching.empty()) {
        return std::unique_ptr<Scorer>{};
    }
    std::vector<int32_t> docs(matching.begin(), matching.end());
    std::unique_ptr<DocIdSetIterator> iterator(new SortedDocIdsIterator(std::move(docs)));
    return std::unique_ptr<Scorer>(ConstantScoreScorer::from_disi(score_, score_mode_, std::move(iterator)));
}

Result<std::unique_ptr<ScorerSupplier>> PointRangeWeight::scorer_supplier(const LeafReaderContext &context,
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
    std::unique_ptr<ScorerSupplier> supplier(
            new PointRangeScorerSupplier(std::move(scorer.value()), cost.value()));
    return supplier;
}

Result<Explanation> PointRangeWeight::explain(const LeafReaderContext &context, int32_t doc,
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
        return Explanation::no_match_no_details("the point of the document is outside the range");
    }
    auto score = scorer.value()->score();
    if (!score.has_value()) {
        return std::unexpected(std::move(score.error()));
    }
    return Explanation::match_no_details(score.value(), std::format("point range matched, score={}", score.value()));
}

} // namespace lucene
