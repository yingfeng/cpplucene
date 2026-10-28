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

module lucene_core:field_exists_query.impl;

import :field_exists_query;

import std;

import :binary_doc_values;
import :constant_score_scorer;
import :doc_id_set_iterator;
import :error;
import :explanation;
import :field_infos;
import :index_searcher;
import :leaf_reader;
import :leaf_reader_context;
import :numeric_doc_values;
import :query;
import :query_visitor;
import :score_mode;
import :scorer;
import :scorer_supplier;
import :sorted_doc_values;
import :sorted_numeric_doc_values;
import :sorted_set_doc_values;
import :types;
import :weight;

namespace lucene {

namespace {

/// Hands a single scorer out once.
class FieldExistsScorerSupplier final : public ScorerSupplier {
public:
    FieldExistsScorerSupplier(std::unique_ptr<Scorer> scorer, int64_t cost)
            : scorer_(std::move(scorer)), cost_(cost) {}

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

FieldExistsWeight::FieldExistsWeight(const FieldExistsQuery &query, float score, ScoreMode score_mode)
        : query_(query), score_(score), score_mode_(score_mode) {}

Result<std::unique_ptr<Scorer>> FieldExistsWeight::scorer_for(const LeafReaderContext &context) const {
    LeafReader *reader = context.reader();
    if (reader == nullptr) {
        return std::unique_ptr<Scorer>{};
    }
    auto field_infos = reader->field_infos();
    if (!field_infos.has_value()) {
        return std::unexpected(std::move(field_infos.error()));
    }
    const FieldInfo *field_info = field_infos.value()->by_name(query_.field());
    if (field_info == nullptr) {
        // The field does not exist in this segment.
        return std::unique_ptr<Scorer>{};
    }

    const std::string &field = query_.field();
    std::unique_ptr<DocIdSetIterator> iterator;
    if (field_info->has_norms()) {
        auto norms = reader->get_norm_values(field);
        if (!norms.has_value()) {
            return std::unexpected(std::move(norms.error()));
        }
        iterator = std::move(norms.value());
    } else if (field_info->has_vector_values()) {
        return err(LuceneError::unsupported_operation(
                "FieldExistsQuery on a field that only indexes vectors is not supported by this port"));
    } else if (field_info->has_doc_values()) {
        switch (field_info->doc_values_type) {
            case DocValuesType::Numeric: {
                auto values = reader->get_numeric_doc_values(field);
                if (!values.has_value()) {
                    return std::unexpected(std::move(values.error()));
                }
                iterator = std::move(values.value());
                break;
            }
            case DocValuesType::Binary: {
                auto values = reader->get_binary_doc_values(field);
                if (!values.has_value()) {
                    return std::unexpected(std::move(values.error()));
                }
                iterator = std::move(values.value());
                break;
            }
            case DocValuesType::Sorted: {
                auto values = reader->get_sorted_doc_values(field);
                if (!values.has_value()) {
                    return std::unexpected(std::move(values.error()));
                }
                iterator = std::move(values.value());
                break;
            }
            case DocValuesType::SortedNumeric: {
                auto values = reader->get_sorted_numeric_doc_values(field);
                if (!values.has_value()) {
                    return std::unexpected(std::move(values.error()));
                }
                iterator = std::move(values.value());
                break;
            }
            case DocValuesType::SortedSet: {
                auto values = reader->get_sorted_set_doc_values(field);
                if (!values.has_value()) {
                    return std::unexpected(std::move(values.error()));
                }
                iterator = std::move(values.value());
                break;
            }
            case DocValuesType::None:
                break;
        }
    } else {
        return err(LuceneError::illegal_argument(std::format(
                "FieldExistsQuery requires that the field indexes doc values, norms or vectors, but field '{}' "
                "exists and indexes neither of these data structures",
                field)));
    }

    if (iterator == nullptr) {
        return std::unique_ptr<Scorer>{};
    }
    return std::unique_ptr<Scorer>(ConstantScoreScorer::from_disi(score_, score_mode_, std::move(iterator)));
}

Result<std::unique_ptr<Weight>> FieldExistsQuery::create_weight(const IndexSearcher &searcher, ScoreMode score_mode,
                                                                float boost) const {
    (void)searcher;
    return std::unique_ptr<Weight>(new FieldExistsWeight(*this, boost, score_mode));
}

VoidResult FieldExistsQuery::visit(QueryVisitor &visitor) const {
    if (visitor.accept_field(field_)) {
        return visitor.visit_leaf(*this);
    }
    return {};
}

Result<std::string> FieldExistsQuery::to_string(std::string_view /*field*/) const {
    return std::format("FieldExistsQuery [field={}]", field_);
}

bool FieldExistsQuery::equals(const Query &other) const {
    const auto *exists = dynamic_cast<const FieldExistsQuery *>(&other);
    return exists != nullptr && field_ == exists->field_;
}

size_t FieldExistsQuery::hash() const { return std::hash<std::string>{}(field_); }

Result<int32_t> FieldExistsWeight::count(const LeafReaderContext &context, const IndexSearcher & /*searcher*/) {
    LeafReader *reader = context.reader();
    if (reader == nullptr) {
        return 0;
    }
    auto field_infos = reader->field_infos();
    if (!field_infos.has_value()) {
        return std::unexpected(std::move(field_infos.error()));
    }
    const FieldInfo *field_info = field_infos.value()->by_name(query_.field());
    if (field_info == nullptr) {
        // The field does not exist in this segment, so no document has a value.
        return 0;
    }
    const std::string &field = query_.field();

    if (field_info->has_norms()) {
        // Norms are dense, so when the field has a norm for every document the
        // count is just the number of live documents.
        auto covered = reader->doc_count(field);
        if (!covered.has_value()) {
            return std::unexpected(std::move(covered.error()));
        }
        auto max_doc = reader->max_doc();
        if (!max_doc.has_value()) {
            return std::unexpected(std::move(max_doc.error()));
        }
        if (covered.value() == max_doc.value()) {
            return reader->num_docs();
        }
        return default_count(context);
    }

    if (field_info->has_vector_values()) {
        // The port has no vector values to read the size from.
        return default_count(context);
    }

    if (field_info->has_doc_values()) {
        // The point and term counts skip deleted documents, so they can only be
        // used while the leaf has none.
        auto deletions = reader->has_deletions();
        if (!deletions.has_value()) {
            return std::unexpected(std::move(deletions.error()));
        }
        if (!deletions.value()) {
            if (field_info->point_dimension_count > 0) {
                auto points = reader->point_values(field);
                if (!points.has_value()) {
                    return std::unexpected(std::move(points.error()));
                }
                return points.value() == nullptr ? 0 : points.value()->get_doc_count();
            }
            if (field_info->index_options != IndexOptions::None) {
                auto terms = reader->terms(field);
                if (!terms.has_value()) {
                    return std::unexpected(std::move(terms.error()));
                }
                return terms.value() == nullptr ? 0 : terms.value()->doc_count();
            }
        }
        return default_count(context);
    }

    return err(LuceneError::illegal_argument(std::format(
            "FieldExistsQuery requires that the field indexes doc values, norms or vectors, but field '{}' "
            "exists and indexes neither of these data structures",
            field)));
}

Result<std::unique_ptr<ScorerSupplier>> FieldExistsWeight::scorer_supplier(const LeafReaderContext &context,
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
    std::unique_ptr<ScorerSupplier> supplier(new FieldExistsScorerSupplier(std::move(scorer.value()), cost.value()));
    return supplier;
}

Result<Explanation> FieldExistsWeight::explain(const LeafReaderContext &context, int32_t doc,
                                               const IndexSearcher & /*searcher*/) const {
    auto scorer = scorer_for(context);
    if (!scorer.has_value()) {
        return std::unexpected(std::move(scorer.error()));
    }
    if (scorer.value() == nullptr) {
        return Explanation::no_match_no_details("the field does not exist in this segment");
    }
    auto advanced = scorer.value()->advance(doc);
    if (!advanced.has_value()) {
        return std::unexpected(std::move(advanced.error()));
    }
    if (advanced.value() != doc) {
        return Explanation::no_match_no_details("the document has no value for the field");
    }
    auto score = scorer.value()->score();
    if (!score.has_value()) {
        return std::unexpected(std::move(score.error()));
    }
    return Explanation::match_no_details(score.value(), std::format("the document has a value, score={}", score.value()));
}

} // namespace lucene
