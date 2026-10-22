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

module lucene_core:term_vectors_writer.impl;

import :term_vectors_writer;

import std;

import :bytes_ref;
import :data_input;
import :doc_id_merger;
import :doc_id_set_iterator;
import :doc_map;
import :error;
import :field_infos;
import :fields;
import :merge_state;
import :postings_enum;
import :term_vectors_reader;
import :terms;
import :terms_enum;

namespace lucene {

VoidResult TermVectorsWriter::add_prox(size_t /*num_prox*/, DataInput * /*positions*/, DataInput * /*offsets*/) {
    return err(LuceneError::need_implemented("TermVectorsWriter::add_prox"));
}

namespace {

/// One input segment taking part in a term vectors merge.
///
/// Port of the reference implementation's `TermVectorsMergeSub`.
class TermVectorsMergeSub {
public:
    TermVectorsMergeSub(size_t reader_index, int32_t max_doc, const DocMap &doc_map)
            : reader_index_(reader_index), max_doc_(max_doc), doc_map_(&doc_map) {}

    [[nodiscard]] Result<int32_t> next_doc() {
        ++doc_id_;
        if (doc_id_ == max_doc_) {
            return NO_MORE_DOCS;
        }
        return doc_id_;
    }

    [[nodiscard]] const DocMap &get_doc_map() const { return *doc_map_; }

    [[nodiscard]] size_t reader_index() const noexcept { return reader_index_; }
    [[nodiscard]] int32_t doc_id() const noexcept { return doc_id_; }

private:
    size_t reader_index_;
    int32_t max_doc_;
    const DocMap *doc_map_;
    int32_t doc_id_{-1};
};

} // namespace

VoidResult TermVectorsWriter::add_all_doc_vectors(Fields *vectors, const MergeStateMeta &merge_state_meta) {
    if (vectors == nullptr) {
        if (auto started = start_document(0); !started.has_value()) {
            return started;
        }
        return finish_document();
    }

    auto num_fields = vectors->size();
    if (!num_fields.has_value()) {
        return std::unexpected(std::move(num_fields.error()));
    }
    if (num_fields.value() == -1) {
        // `Fields::size` is a statistic, so it may be unknown: count instead.
        int32_t counted = 0;
        auto iterator = vectors->iterator();
        if (!iterator.has_value()) {
            return std::unexpected(std::move(iterator.error()));
        }
        while (true) {
            auto name = iterator.value()->next();
            if (!name.has_value()) {
                return std::unexpected(std::move(name.error()));
            }
            if (name.value() == nullptr) {
                break;
            }
            ++counted;
        }
        num_fields = counted;
    }

    if (auto started = start_document(num_fields.value()); !started.has_value()) {
        return started;
    }

    auto fields_iterator = vectors->iterator();
    if (!fields_iterator.has_value()) {
        return std::unexpected(std::move(fields_iterator.error()));
    }

    std::unique_ptr<PostingsEnum> docs_and_positions_reuse;
    int32_t field_count = 0;

    while (true) {
        auto field_name = fields_iterator.value()->next();
        if (!field_name.has_value()) {
            return std::unexpected(std::move(field_name.error()));
        }
        if (field_name.value() == nullptr) {
            break;
        }
        ++field_count;

        const FieldInfo *field_info = merge_state_meta.merge_field_infos->by_name(*field_name.value());
        if (field_info == nullptr) {
            return err(LuceneError::illegal_state("missing FieldInfo for field " + *field_name.value()));
        }

        auto terms = vectors->terms(*field_name.value());
        if (!terms.has_value()) {
            return std::unexpected(std::move(terms.error()));
        }
        if (terms.value() == nullptr) {
            // A fields iterator must not lie: it listed a field it cannot open.
            continue;
        }

        const bool has_positions = terms.value()->has_positions();
        const bool has_offsets = terms.value()->has_offsets();
        const bool has_payloads = terms.value()->has_payloads();

        auto num_terms = terms.value()->size();
        if (!num_terms.has_value()) {
            return std::unexpected(std::move(num_terms.error()));
        }
        if (num_terms.value() == -1) {
            int64_t counted = 0;
            auto counter = terms.value()->iterator();
            if (!counter.has_value()) {
                return std::unexpected(std::move(counter.error()));
            }
            while (true) {
                auto term = counter.value()->next();
                if (!term.has_value()) {
                    return std::unexpected(std::move(term.error()));
                }
                if (term.value() == nullptr) {
                    break;
                }
                ++counted;
            }
            num_terms = counted;
        }

        if (auto started = start_field(*field_info, static_cast<int32_t>(num_terms.value()), has_positions,
                                       has_offsets, has_payloads);
            !started.has_value()) {
            return started;
        }

        auto terms_enum = terms.value()->iterator();
        if (!terms_enum.has_value()) {
            return std::unexpected(std::move(terms_enum.error()));
        }

        while (true) {
            auto term = terms_enum.value()->next();
            if (!term.has_value()) {
                return std::unexpected(std::move(term.error()));
            }
            if (term.value() == nullptr) {
                break;
            }
            auto freq = terms_enum.value()->total_term_freq();
            if (!freq.has_value()) {
                return std::unexpected(std::move(freq.error()));
            }
            if (auto started = start_term(term.value()->as_slice(), static_cast<int32_t>(freq.value()));
                !started.has_value()) {
                return started;
            }

            if (has_positions || has_offsets) {
                auto postings = terms_enum.value()->postings_with_flags(std::move(docs_and_positions_reuse),
                                                                        OFFSETS | PAYLOADS);
                if (!postings.has_value()) {
                    return std::unexpected(std::move(postings.error()));
                }
                auto doc = postings.value()->next_doc();
                if (!doc.has_value()) {
                    return std::unexpected(std::move(doc.error()));
                }
                if (doc.value() == NO_MORE_DOCS) {
                    return err(LuceneError::illegal_state("term vector postings must have at least one document"));
                }

                for (int64_t i = 0; i < freq.value(); ++i) {
                    auto position = postings.value()->next_position();
                    if (!position.has_value()) {
                        return std::unexpected(std::move(position.error()));
                    }
                    auto start_offset = postings.value()->start_offset();
                    if (!start_offset.has_value()) {
                        return std::unexpected(std::move(start_offset.error()));
                    }
                    auto end_offset = postings.value()->end_offset();
                    if (!end_offset.has_value()) {
                        return std::unexpected(std::move(end_offset.error()));
                    }
                    auto payload = postings.value()->get_payload();
                    if (!payload.has_value()) {
                        return std::unexpected(std::move(payload.error()));
                    }
                    if (auto added = add_position(position.value(), start_offset.value(), end_offset.value(),
                                                  payload.value());
                        !added.has_value()) {
                        return added;
                    }
                }
                docs_and_positions_reuse = std::move(postings.value());
            }

        }

        if (auto finished = finish_field(); !finished.has_value()) {
            return finished;
        }
    }

    if (field_count != num_fields.value()) {
        return err(LuceneError::illegal_state(
                std::format("expected {} fields but wrote {}", num_fields.value(), field_count)));
    }

    return finish_document();
}

Result<int32_t> TermVectorsWriter::merge(MergeState &merge_state) {
    std::vector<DocIdMergerSub<TermVectorsMergeSub>> subs;
    subs.reserve(merge_state.term_vectors_readers.size());
    for (size_t i = 0; i < merge_state.term_vectors_readers.size(); ++i) {
        const auto &reader = merge_state.term_vectors_readers[i];
        if (reader != nullptr) {
            if (auto checked = reader->check_integrity(); !checked.has_value()) {
                return std::unexpected(std::move(checked.error()));
            }
        }
        subs.emplace_back(TermVectorsMergeSub(i, merge_state.max_docs[i], *merge_state.doc_maps->at(i)));
    }

    auto merger = DocIdMerger<TermVectorsMergeSub>::of(std::move(subs), merge_state.needs_index_sort);
    if (!merger.has_value()) {
        return std::unexpected(std::move(merger.error()));
    }
    const MergeStateMeta merge_state_meta = merge_state.get_meta();

    int32_t doc_count = 0;
    while (true) {
        auto sub_index = merger.value().next();
        if (!sub_index.has_value()) {
            return std::unexpected(std::move(sub_index.error()));
        }
        if (!sub_index.value().has_value()) {
            break;
        }
        const auto &sub = merger.value().subs()[sub_index.value().value()].sub;

        // NOTE: the vectors must be materialised before the writer is called, see
        // LUCENE-1282.
        std::unique_ptr<Fields> vectors;
        auto &reader = merge_state.term_vectors_readers[sub.reader_index()];
        if (reader != nullptr) {
            auto got = reader->get(sub.doc_id());
            if (!got.has_value()) {
                return std::unexpected(std::move(got.error()));
            }
            vectors = std::move(got.value());
        }

        if (auto added = add_all_doc_vectors(vectors.get(), merge_state_meta); !added.has_value()) {
            return std::unexpected(std::move(added.error()));
        }
        ++doc_count;
    }

    if (auto finished = finish(doc_count); !finished.has_value()) {
        return std::unexpected(std::move(finished.error()));
    }
    return doc_count;
}

} // namespace lucene
