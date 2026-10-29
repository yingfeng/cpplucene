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

module lucene_core:range_field_query.impl;

import :range_field_query;

import std;

import :constant_score_scorer;
import :doc_id_set_iterator;
import :error;
import :explanation;
import :field_infos;
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

/// Compares the `bytes_per_dim` bytes at `left_offset` with the ones at
/// `right_offset`, unsigned.
int32_t compare_bytes(std::span<const uint8_t> left, size_t left_offset, std::span<const uint8_t> right,
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

/// The relation of one dimension of a cell to the query.
Result<Relation> compare_dimension(QueryType query_type, std::span<const uint8_t> query,
                                   std::span<const uint8_t> min_packed_value, std::span<const uint8_t> max_packed_value,
                                   size_t num_dims, size_t bytes_per_dim, size_t dim) {
    const size_t min_offset = dim * bytes_per_dim;
    const size_t max_offset = min_offset + bytes_per_dim * num_dims;
    const auto compare = [&](std::span<const uint8_t> left, size_t left_offset, std::span<const uint8_t> right,
                             size_t right_offset) {
        return compare_bytes(left, left_offset, right, right_offset, bytes_per_dim);
    };

    switch (query_type) {
        case QueryType::Intersects:
            if (compare(query, max_offset, min_packed_value, min_offset) < 0 ||
                compare(query, min_offset, max_packed_value, max_offset) > 0) {
                return Relation::kCellOutsideQuery;
            }
            if (compare(query, max_offset, max_packed_value, min_offset) >= 0 &&
                compare(query, min_offset, min_packed_value, max_offset) <= 0) {
                return Relation::kCellInsideQuery;
            }
            return Relation::kCellCrossesQuery;
        case QueryType::Within:
            if (compare(query, max_offset, min_packed_value, max_offset) < 0 ||
                compare(query, min_offset, max_packed_value, min_offset) > 0) {
                return Relation::kCellOutsideQuery;
            }
            if (compare(query, max_offset, max_packed_value, max_offset) >= 0 &&
                compare(query, min_offset, min_packed_value, min_offset) <= 0) {
                return Relation::kCellInsideQuery;
            }
            return Relation::kCellCrossesQuery;
        case QueryType::Contains:
            if (compare(query, max_offset, max_packed_value, max_offset) > 0 ||
                compare(query, min_offset, min_packed_value, min_offset) < 0) {
                return Relation::kCellOutsideQuery;
            }
            if (compare(query, max_offset, min_packed_value, max_offset) <= 0 &&
                compare(query, min_offset, max_packed_value, min_offset) >= 0) {
                return Relation::kCellInsideQuery;
            }
            return Relation::kCellCrossesQuery;
        case QueryType::Crosses:
            return err(LuceneError::unsupported_operation("Crosses has no per-dimension relation"));
    }
    return err(LuceneError::illegal_argument("unknown range query type"));
}

/// Whether one dimension of `packed_value` satisfies the query.
bool matches_dimension(QueryType query_type, std::span<const uint8_t> query, std::span<const uint8_t> packed_value,
                       size_t num_dims, size_t bytes_per_dim, size_t dim) {
    const size_t min_offset = dim * bytes_per_dim;
    const size_t max_offset = min_offset + bytes_per_dim * num_dims;
    const auto compare = [&](std::span<const uint8_t> left, size_t left_offset, std::span<const uint8_t> right,
                             size_t right_offset) {
        return compare_bytes(left, left_offset, right, right_offset, bytes_per_dim);
    };

    switch (query_type) {
        case QueryType::Intersects:
            return compare(query, max_offset, packed_value, min_offset) >= 0 &&
                   compare(query, min_offset, packed_value, max_offset) <= 0;
        case QueryType::Within:
            return compare(query, min_offset, packed_value, min_offset) <= 0 &&
                   compare(query, max_offset, packed_value, max_offset) >= 0;
        case QueryType::Contains:
            return compare(query, min_offset, packed_value, min_offset) >= 0 &&
                   compare(query, max_offset, packed_value, max_offset) <= 0;
        case QueryType::Crosses:
            break;
    }
    return false;
}

/// Collects the documents of one leaf whose ranges satisfy the query.
class RangeFieldVisitor final : public IntersectVisitor {
public:
    RangeFieldVisitor(const RangeFieldQuery &query, std::set<int32_t> &docs) : query_(&query), docs_(&docs) {}

    [[nodiscard]] VoidResult visit(int32_t doc_id) override {
        docs_->insert(doc_id);
        return {};
    }

    [[nodiscard]] VoidResult visit_with_packed_value(int32_t doc_id, std::span<const uint8_t> packed_value) override {
        auto matched = range_field_query_matches(query_->query_type(), query_->ranges(), packed_value,
                                                 query_->num_dims(), query_->bytes_per_dim());
        if (!matched.has_value()) {
            return std::unexpected(std::move(matched.error()));
        }
        if (matched.value()) {
            docs_->insert(doc_id);
        }
        return {};
    }

    [[nodiscard]] Result<Relation> compare(std::span<const uint8_t> min_packed_value,
                                           std::span<const uint8_t> max_packed_value) const override {
        return range_field_query_compare(query_->query_type(), query_->ranges(), min_packed_value, max_packed_value,
                                         query_->num_dims(), query_->bytes_per_dim());
    }

private:
    const RangeFieldQuery *query_{nullptr};
    std::set<int32_t> *docs_{nullptr};
};

/// A [`DocIdSetIterator`] over the ascending documents of a set.
class SortedDocIdsIterator final : public DocIdSetIterator {
public:
    explicit SortedDocIdsIterator(std::vector<int32_t> docs) : docs_(std::move(docs)) {}

    [[nodiscard]] int32_t doc_id() const override { return doc_; }

    [[nodiscard]] Result<int32_t> next_doc() override {
        ++index_;
        return position();
    }

    [[nodiscard]] Result<int32_t> advance(int32_t target) override {
        while (doc_ != NO_MORE_DOCS && doc_ < target) {
            auto next = next_doc();
            if (!next.has_value()) {
                return next;
            }
        }
        return doc_;
    }

    [[nodiscard]] Result<int64_t> cost() const override { return static_cast<int64_t>(docs_.size()); }

private:
    [[nodiscard]] int32_t position() {
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

/// A supplier that hands out one scorer; the supplier is used at most once.
class OneShotSupplier final : public ScorerSupplier {
public:
    OneShotSupplier(std::unique_ptr<Scorer> scorer, int64_t cost) : scorer_(std::move(scorer)), cost_(cost) {}

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
    int64_t cost_;
};

/// The weight of a [`RangeFieldQuery`].
class RangeFieldWeight final : public Weight {
public:
    RangeFieldWeight(const RangeFieldQuery &query, ScoreMode score_mode, float boost)
        : query_(query), score_mode_(score_mode), boost_(boost) {}

    [[nodiscard]] Result<Explanation> explain(const LeafReaderContext &context, int32_t doc,
                                              const IndexSearcher & /*searcher*/) const override {
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
            return Explanation::no_match_no_details("the range of this document does not match");
        }
        return Explanation::match_no_details(boost_,
                "range field matched (" + std::to_string(query_.num_dims()) + " dimension(s))");
    }

    [[nodiscard]] const Query &get_query() const override { return query_; }

    [[nodiscard]] Result<std::unique_ptr<ScorerSupplier>> scorer_supplier(const LeafReaderContext &context,
                                                                         const IndexSearcher & /*searcher*/) override {
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
        return std::unique_ptr<ScorerSupplier>(new OneShotSupplier(std::move(scorer.value()), cost.value()));
    }

    [[nodiscard]] Result<bool> is_cacheable(const LeafReaderContext & /*context*/) const override { return true; }

private:
    [[nodiscard]] Result<std::unique_ptr<Scorer>> scorer_for(const LeafReaderContext &context) const {
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

        auto field_infos = reader->field_infos();
        if (!field_infos.has_value()) {
            return std::unexpected(std::move(field_infos.error()));
        }
        const FieldInfo *info =
                field_infos.value() == nullptr ? nullptr : field_infos.value()->by_name(query_.field());
        if (info == nullptr) {
            return std::unique_ptr<Scorer>{};
        }
        if (info->point_dimension_count / 2 != query_.num_dims()) {
            return err(LuceneError::illegal_argument(
                    "field=\"" + query_.field() + "\" was indexed with numDims=" +
                    std::to_string(info->point_dimension_count / 2) + " but this query has numDims=" +
                    std::to_string(query_.num_dims())));
        }

        auto max_doc = reader->max_doc();
        if (!max_doc.has_value()) {
            return std::unexpected(std::move(max_doc.error()));
        }

        // Every document matches when the whole point cloud lies in the query.
        auto doc_count = points.get_doc_count();
        if (!doc_count.has_value()) {
            return std::unexpected(std::move(doc_count.error()));
        }
        bool all_docs_match = false;
        if (doc_count.value() == max_doc.value()) {
            auto min_value = points.get_min_packed_value();
            if (!min_value.has_value()) {
                return std::unexpected(std::move(min_value.error()));
            }
            auto max_value = points.get_max_packed_value();
            if (!max_value.has_value()) {
                return std::unexpected(std::move(max_value.error()));
            }
            if (!min_value.value().has_value() || !max_value.value().has_value()) {
                return err(LuceneError::illegal_state("the point values report no min/max value"));
            }
            auto relation = range_field_query_compare(query_.query_type(), query_.ranges(),
                    min_value.value().value().as_slice(), max_value.value().value().as_slice(),
                    query_.num_dims(), query_.bytes_per_dim());
            if (!relation.has_value()) {
                return std::unexpected(std::move(relation.error()));
            }
            all_docs_match = relation.value() == Relation::kCellInsideQuery;
        }
        if (all_docs_match) {
            std::unique_ptr<DocIdSetIterator> all(new AllDISI(max_doc.value()));
            return std::unique_ptr<Scorer>(ConstantScoreScorer::from_disi(boost_, score_mode_, std::move(all)));
        }

        std::set<int32_t> matching;
        RangeFieldVisitor visitor(query_, matching);
        if (auto intersected = points.intersect(visitor); !intersected.has_value()) {
            return std::unexpected(std::move(intersected.error()));
        }
        if (matching.empty()) {
            return std::unique_ptr<Scorer>{};
        }
        std::vector<int32_t> docs(matching.begin(), matching.end());
        std::unique_ptr<DocIdSetIterator> iterator(new SortedDocIdsIterator(std::move(docs)));
        return std::unique_ptr<Scorer>(ConstantScoreScorer::from_disi(boost_, score_mode_, std::move(iterator)));
    }

    const RangeFieldQuery &query_;
    ScoreMode score_mode_;
    float boost_;
};

} // namespace

Result<Relation> range_field_query_compare(QueryType query_type, std::span<const uint8_t> query,
                                           std::span<const uint8_t> min_packed_value,
                                           std::span<const uint8_t> max_packed_value, size_t num_dims,
                                           size_t bytes_per_dim) {
    if (query_type == QueryType::Crosses) {
        auto intersect_relation = range_field_query_compare(QueryType::Intersects, query, min_packed_value,
                                                            max_packed_value, num_dims, bytes_per_dim);
        if (!intersect_relation.has_value()) {
            return intersect_relation;
        }
        if (intersect_relation.value() == Relation::kCellOutsideQuery) {
            return Relation::kCellOutsideQuery;
        }
        auto within_relation = range_field_query_compare(QueryType::Within, query, min_packed_value, max_packed_value,
                                                         num_dims, bytes_per_dim);
        if (!within_relation.has_value()) {
            return within_relation;
        }
        if (within_relation.value() == Relation::kCellInsideQuery) {
            return Relation::kCellOutsideQuery;
        }
        if (intersect_relation.value() == Relation::kCellInsideQuery &&
            within_relation.value() == Relation::kCellOutsideQuery) {
            return Relation::kCellInsideQuery;
        }
        return Relation::kCellCrossesQuery;
    }

    bool inside = true;
    for (size_t dim = 0; dim < num_dims; ++dim) {
        auto relation = compare_dimension(query_type, query, min_packed_value, max_packed_value, num_dims,
                                          bytes_per_dim, dim);
        if (!relation.has_value()) {
            return relation;
        }
        if (relation.value() == Relation::kCellOutsideQuery) {
            return Relation::kCellOutsideQuery;
        }
        if (relation.value() != Relation::kCellInsideQuery) {
            inside = false;
        }
    }
    return inside ? Relation::kCellInsideQuery : Relation::kCellCrossesQuery;
}

Result<bool> range_field_query_matches(QueryType query_type, std::span<const uint8_t> query,
                                       std::span<const uint8_t> packed_value, size_t num_dims,
                                       size_t bytes_per_dim) {
    if (query_type == QueryType::Crosses) {
        auto intersects = range_field_query_matches(QueryType::Intersects, query, packed_value, num_dims,
                                                    bytes_per_dim);
        if (!intersects.has_value()) {
            return intersects;
        }
        if (!intersects.value()) {
            return false;
        }
        auto within = range_field_query_matches(QueryType::Within, query, packed_value, num_dims, bytes_per_dim);
        if (!within.has_value()) {
            return within;
        }
        return !within.value();
    }

    for (size_t dim = 0; dim < num_dims; ++dim) {
        if (!matches_dimension(query_type, query, packed_value, num_dims, bytes_per_dim, dim)) {
            return false;
        }
    }
    return true;
}

RangeFieldQuery::RangeFieldQuery(std::string field, std::vector<uint8_t> ranges, size_t num_dims, QueryType query_type,
                                 std::shared_ptr<const RangeFieldFormat> format)
        : field_(std::move(field)),
          ranges_(std::move(ranges)),
          num_dims_(num_dims),
          bytes_per_dim_(2 * num_dims == 0 ? 0 : ranges_.size() / (2 * num_dims)),
          query_type_(query_type),
          format_(std::move(format)) {}

Result<RangeFieldQuery> RangeFieldQuery::create(std::string field, std::vector<uint8_t> ranges, size_t num_dims,
                                                QueryType query_type, std::shared_ptr<const RangeFieldFormat> format) {
    if (num_dims > 4) {
        return err(LuceneError::illegal_argument("dimension size cannot be greater than 4"));
    }
    if (ranges.empty()) {
        return err(LuceneError::illegal_argument("encoded ranges cannot be null or empty"));
    }
    if (num_dims == 0) {
        return err(LuceneError::illegal_argument("dimension size cannot be zero"));
    }
    if (ranges.size() % (2 * num_dims) != 0) {
        return err(LuceneError::illegal_argument("encoded ranges are not a multiple of 2 * num_dims"));
    }
    if (format == nullptr) {
        return err(LuceneError::illegal_argument("the range field format is null"));
    }
    return RangeFieldQuery(std::move(field), std::move(ranges), num_dims, query_type, std::move(format));
}

Result<std::unique_ptr<Weight>> RangeFieldQuery::create_weight(const IndexSearcher & /*searcher*/, ScoreMode score_mode,
                                                              float boost) const {
    return std::unique_ptr<Weight>(new RangeFieldWeight(*this, score_mode, boost));
}

VoidResult RangeFieldQuery::visit(QueryVisitor &visitor) const {
    if (visitor.accept_field(field_)) {
        return visitor.visit_leaf(*this);
    }
    return {};
}

Result<std::string> RangeFieldQuery::to_string(std::string_view field) const {
    std::string result;
    if (field != field_) {
        result += field_ + ":";
    }
    result += "<ranges:";
    for (size_t dim = 0; dim < num_dims_; ++dim) {
        if (dim > 0) {
            result += ' ';
        }
        auto formatted = format_->format(ranges_, dim);
        if (!formatted.has_value()) {
            return std::unexpected(std::move(formatted.error()));
        }
        result += formatted.value();
    }
    result += '>';
    return result;
}

bool RangeFieldQuery::equals(const Query &other) const {
    const auto *query = dynamic_cast<const RangeFieldQuery *>(&other);
    if (query == nullptr) {
        return false;
    }
    // The format decides whether the packed bytes are ints, longs, floats or
    // doubles, and two of those share a width: a range over `int` must never
    // compare equal to one over `float` with the same bytes.
    if (format_ == nullptr || query->format_ == nullptr) {
        return false;
    }
    const RangeFieldFormat &self_format = *format_;
    const RangeFieldFormat &other_format = *query->format_;
    if (typeid(self_format) != typeid(other_format)) {
        return false;
    }
    return field_ == query->field_ && num_dims_ == query->num_dims_ && query_type_ == query->query_type_ &&
           ranges_ == query->ranges_;
}

size_t RangeFieldQuery::hash() const {
    size_t result = std::hash<std::string>{}(field_);
    result = result * 31 + num_dims_;
    result = result * 31 + static_cast<size_t>(query_type_);
    // Same reason as in `equals`: the format type is part of the identity.
    size_t format_hash = 0;
    if (format_ != nullptr) {
        const RangeFieldFormat &self_format = *format_;
        format_hash = typeid(self_format).hash_code();
    }
    result = result * 31 + format_hash;
    for (const uint8_t byte : ranges_) {
        result = result * 31 + byte;
    }
    return result;
}

} // namespace lucene
