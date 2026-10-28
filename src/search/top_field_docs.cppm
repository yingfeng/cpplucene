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

export module lucene_core:top_field_docs;

import :types;
import std;

import :error;
import :field_comparator;
import :score_doc;
import :sort_field;
import :total_hits;

namespace lucene {

/// A hit of a sorted search: the usual hit plus the sort values of the document.
///
/// Port of `FieldDoc` (`core/search/top_field_docs.rs`).
export struct FieldDoc : ScoreDoc {
    /// The sort values, in the order of the [`Sort`] fields.
    std::vector<SortValue> fields;

    /// A hit with its sort values.
    [[nodiscard]] static FieldDoc create(int32_t doc, float score, std::vector<SortValue> fields) {
        FieldDoc hit;
        hit.doc = doc;
        hit.score = score;
        hit.fields = std::move(fields);
        return hit;
    }

    /// The [`ScoreDoc`] text plus the sort values.
    [[nodiscard]] std::string to_string() const {
        std::string result = ScoreDoc::to_string() + " fields=[";
        for (size_t i = 0; i < fields.size(); ++i) {
            if (i > 0) {
                result += ", ";
            }
            result += std::visit(
                    [](const auto &value) -> std::string {
                        using Value = std::decay_t<decltype(value)>;
                        if constexpr (std::is_same_v<Value, std::string>) {
                            return value;
                        } else if constexpr (std::is_arithmetic_v<Value>) {
                            return std::to_string(value);
                        } else {
                            return "null";
                        }
                    },
                    fields[i]);
        }
        return result + "]";
    }
};

/// The hits of a search ordered by a [`Sort`].
///
/// Port of `TopFieldDocs` (`core/search/top_field_docs.rs`).
export struct TopFieldDocs {
    /// The total number of hits of the query.
    TotalHits total_hits;
    /// The top hits, best first.
    std::vector<FieldDoc> score_docs;
    /// The sort the hits are ordered by.
    Sort sort;

    TopFieldDocs(TotalHits total_hits, std::vector<FieldDoc> score_docs, Sort sort)
            : total_hits(std::move(total_hits)), score_docs(std::move(score_docs)), sort(std::move(sort)) {}

    /// The hits as plain [`ScoreDoc`]s, when the sort values are not needed.
    [[nodiscard]] std::vector<ScoreDoc> as_score_docs() const {
        std::vector<ScoreDoc> docs;
        docs.reserve(score_docs.size());
        for (const FieldDoc &hit : score_docs) {
            docs.push_back(ScoreDoc{hit.score, hit.doc, hit.shard_index});
        }
        return docs;
    }
};

/// Merges sorted shard results by their sort values, skipping the first `start`
/// hits and keeping the best `top_n`.
///
/// Port of `merge_top_field_docs_with_start` (`core/search/top_docs.rs`).
///
/// **Deviation:** the custom tie-breaker overload
/// (`merge_top_field_docs_with_comparator`) is not ported; every merge breaks
/// ties with `DefaultTieBreaker`, which is what a single searcher that merged its
/// own leaf slices needs.
[[nodiscard]] Result<TopFieldDocs> merge_top_field_docs_with_start(const Sort &sort, size_t start, size_t top_n,
                                                                   const std::vector<TopFieldDocs> &shard_hits);

/// [`merge_top_field_docs_with_start`] without skipping any hit.
[[nodiscard]] Result<TopFieldDocs> merge_top_field_docs(const Sort &sort, size_t top_n,
                                                        const std::vector<TopFieldDocs> &shard_hits);

} // namespace lucene
