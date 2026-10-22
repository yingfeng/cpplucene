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

module lucene_core:doc_values_consumer.impl;

import :doc_values_consumer;

import std;

import :binary_doc_values;
import :bytes_ref;
import :doc_map;
import :doc_values_merge;
import :doc_values_producer;
import :error;
import :field_infos;
import :index_types;
import :merge_state;
import :numeric_doc_values;
import :sorted_doc_values;
import :sorted_numeric_doc_values;
import :sorted_set_doc_values;

namespace lucene {

namespace {

/// One input segment that has doc values for the field being merged.
struct FieldSource {
    const std::shared_ptr<DocValuesProducer> *producer;
    const FieldInfo *reader_field_info;
    size_t segment;
};

/// The segments whose doc values for `field` have type `expected`.
[[nodiscard]] std::vector<FieldSource> sources_for(const FieldInfo &field, const MergeState &merge_state,
                                                   DocValuesType expected) {
    std::vector<FieldSource> sources;
    const auto &producers = merge_state.doc_values_producers;
    sources.reserve(producers.size());
    for (size_t i = 0; i < producers.size(); ++i) {
        const auto &producer = producers[i];
        if (producer == nullptr) {
            continue;
        }
        const FieldInfo *reader_field_info = merge_state.field_infos->at(i)->by_name(field.name);
        if (reader_field_info == nullptr || reader_field_info->doc_values_type != expected) {
            continue;
        }
        sources.push_back(FieldSource{&producer, reader_field_info, i});
    }
    return sources;
}

/// Sorts and deduplicates `terms` into the merged dictionary.
void finish_dictionary(std::vector<BytesRef> &terms) {
    std::sort(terms.begin(), terms.end());
    terms.erase(std::unique(terms.begin(), terms.end()), terms.end());
}

/// Index of `key` in the sorted dictionary, or `-insertion_point - 1`.
[[nodiscard]] int32_t dictionary_lookup(const std::vector<BytesRef> &terms, std::span<const uint8_t> key) {
    size_t low = 0;
    size_t high = terms.size();
    while (low < high) {
        const size_t mid = low + (high - low) / 2;
        if (terms[mid].compare_to(key) < 0) {
            low = mid + 1;
        } else {
            high = mid;
        }
    }
    if (low < terms.size() && terms[low].bytes_equals(key)) {
        return static_cast<int32_t>(low);
    }
    return -static_cast<int32_t>(low) - 1;
}

/// Resolves the merged doc id of `doc`, propagating errors; `-1` means deleted.
[[nodiscard]] Result<int32_t> map_doc(const MergeState &merge_state, size_t segment, int32_t doc) {
    return merge_state.doc_maps->at(segment)->get(doc);
}

/// A [`DocValuesProducer`] that exposes the merge of the per-segment doc values
/// for one field.
///
/// Port of the reference implementation's `EmptyDocValuesProducerMerge*` family, collapsed into one
/// class because the port's producer interface is type erased.
///
/// **Deviation:** the sorted, sorted-set and sorted-numeric merges materialise
/// the merged values in memory (the reference implementation streams them through an `OrdinalMap` and
/// packed ordinals).  Merging sorted values needs the union of the dictionaries
/// before the first document can be emitted anyway, and buffering is what makes
/// the port's simpler implementation correct without the ordinal-map machinery.
class DocValuesMergeProducer final : public DocValuesProducer {
public:
    DocValuesMergeProducer(const FieldInfo &merge_field_info, const MergeState &merge_state)
            : merge_field_info_(&merge_field_info), merge_state_(&merge_state) {}

    [[nodiscard]] Result<std::unique_ptr<NumericDocValues>> get_numeric(const FieldInfo &field) const override {
        if (auto owned = owns(field); !owned.has_value()) {
            return std::unexpected(std::move(owned.error()));
        }
        std::vector<NumericDocValuesSub> subs;
        for (const auto &source : sources_for(field, *merge_state_, DocValuesType::Numeric)) {
            auto values = source.producer->get()->get_numeric(*source.reader_field_info);
            if (!values.has_value()) {
                return std::unexpected(std::move(values.error()));
            }
            subs.emplace_back(std::move(values.value()), *merge_state_->doc_maps->at(source.segment));
        }
        return NumericDocValuesMerge::create(std::move(subs), merge_state_->needs_index_sort);
    }

    [[nodiscard]] Result<std::unique_ptr<BinaryDocValues>> get_binary(const FieldInfo &field) const override {
        if (auto owned = owns(field); !owned.has_value()) {
            return std::unexpected(std::move(owned.error()));
        }
        std::vector<BinaryDocValuesSub> subs;
        for (const auto &source : sources_for(field, *merge_state_, DocValuesType::Binary)) {
            auto values = source.producer->get()->get_binary(*source.reader_field_info);
            if (!values.has_value()) {
                return std::unexpected(std::move(values.error()));
            }
            subs.emplace_back(std::move(values.value()), *merge_state_->doc_maps->at(source.segment));
        }
        return BinaryDocValuesMerge::create(std::move(subs), merge_state_->needs_index_sort);
    }

    [[nodiscard]] Result<std::unique_ptr<SortedDocValues>> get_sorted(const FieldInfo &field) const override {
        if (auto owned = owns(field); !owned.has_value()) {
            return std::unexpected(std::move(owned.error()));
        }
        return build_sorted(field);
    }

    [[nodiscard]] Result<std::unique_ptr<SortedNumericDocValues>> get_sorted_numeric(
            const FieldInfo &field) const override {
        if (auto owned = owns(field); !owned.has_value()) {
            return std::unexpected(std::move(owned.error()));
        }
        return build_sorted_numeric(field);
    }

    [[nodiscard]] Result<std::unique_ptr<SortedSetDocValues>> get_sorted_set(const FieldInfo &field) const override {
        if (auto owned = owns(field); !owned.has_value()) {
            return std::unexpected(std::move(owned.error()));
        }
        return build_sorted_set(field);
    }

    [[nodiscard]] VoidResult check_integrity() const override { return {}; }

private:
    [[nodiscard]] VoidResult owns(const FieldInfo &field) const {
        if (&field != merge_field_info_) {
            return err(LuceneError::illegal_argument("wrong fieldInfo"));
        }
        return {};
    }

    [[nodiscard]] Result<std::unique_ptr<SortedDocValues>> build_sorted(const FieldInfo &field) const {
        const auto sources = sources_for(field, *merge_state_, DocValuesType::Sorted);

        // Pass 1: collect the terms that survive the merge.
        std::vector<BytesRef> dictionary;
        for (const auto &source : sources) {
            auto values = source.producer->get()->get_sorted(*source.reader_field_info);
            if (!values.has_value()) {
                return std::unexpected(std::move(values.error()));
            }
            while (true) {
                auto doc = values.value()->next_doc();
                if (!doc.has_value()) {
                    return std::unexpected(std::move(doc.error()));
                }
                if (doc.value() == NO_MORE_DOCS) {
                    break;
                }
                auto ord = values.value()->ord_value();
                if (!ord.has_value()) {
                    return std::unexpected(std::move(ord.error()));
                }
                if (ord.value() < 0) {
                    continue;
                }
                auto term = values.value()->lookup_ord(ord.value());
                if (!term.has_value()) {
                    return std::unexpected(std::move(term.error()));
                }
                dictionary.push_back(std::move(term.value()));
            }
        }
        finish_dictionary(dictionary);

        std::vector<int32_t> ords(static_cast<size_t>(merge_state_->segment_info.max_doc), -1);

        // Pass 2: translate each document's ordinal into the merged dictionary.
        for (const auto &source : sources) {
            auto values = source.producer->get()->get_sorted(*source.reader_field_info);
            if (!values.has_value()) {
                return std::unexpected(std::move(values.error()));
            }
            auto value_count = values.value()->get_value_count();
            if (!value_count.has_value()) {
                return std::unexpected(std::move(value_count.error()));
            }
            std::vector<int32_t> local_to_global(static_cast<size_t>(value_count.value()), -1);
            for (int32_t ord = 0; ord < value_count.value(); ++ord) {
                auto term = values.value()->lookup_ord(ord);
                if (!term.has_value()) {
                    return std::unexpected(std::move(term.error()));
                }
                local_to_global[static_cast<size_t>(ord)] = dictionary_lookup(dictionary, term.value().as_slice());
            }

            while (true) {
                auto doc = values.value()->next_doc();
                if (!doc.has_value()) {
                    return std::unexpected(std::move(doc.error()));
                }
                if (doc.value() == NO_MORE_DOCS) {
                    break;
                }
                auto ord = values.value()->ord_value();
                if (!ord.has_value()) {
                    return std::unexpected(std::move(ord.error()));
                }
                if (ord.value() < 0) {
                    continue;
                }
                auto mapped = map_doc(*merge_state_, source.segment, doc.value());
                if (!mapped.has_value()) {
                    return std::unexpected(std::move(mapped.error()));
                }
                if (mapped.value() < 0) {
                    continue;
                }
                ords[static_cast<size_t>(mapped.value())] = local_to_global[static_cast<size_t>(ord.value())];
            }
        }

        return std::unique_ptr<SortedDocValues>(
                new InMemorySortedDocValues(std::move(ords), std::move(dictionary)));
    }

    [[nodiscard]] Result<std::unique_ptr<SortedSetDocValues>> build_sorted_set(const FieldInfo &field) const {
        const auto sources = sources_for(field, *merge_state_, DocValuesType::SortedSet);

        std::vector<BytesRef> dictionary;
        for (const auto &source : sources) {
            auto values = source.producer->get()->get_sorted_set(*source.reader_field_info);
            if (!values.has_value()) {
                return std::unexpected(std::move(values.error()));
            }
            while (true) {
                auto doc = values.value()->next_doc();
                if (!doc.has_value()) {
                    return std::unexpected(std::move(doc.error()));
                }
                if (doc.value() == NO_MORE_DOCS) {
                    break;
                }
                while (true) {
                    auto ord = values.value()->next_ord();
                    if (!ord.has_value()) {
                        return std::unexpected(std::move(ord.error()));
                    }
                    if (ord.value() == NO_MORE_ORDS) {
                        break;
                    }
                    auto term = values.value()->lookup_ord(ord.value());
                    if (!term.has_value()) {
                        return std::unexpected(std::move(term.error()));
                    }
                    dictionary.push_back(std::move(term.value()));
                }
            }
        }
        finish_dictionary(dictionary);

        std::vector<std::vector<int64_t>> ords(static_cast<size_t>(merge_state_->segment_info.max_doc));
        std::vector<int64_t> discarded;

        for (const auto &source : sources) {
            auto values = source.producer->get()->get_sorted_set(*source.reader_field_info);
            if (!values.has_value()) {
                return std::unexpected(std::move(values.error()));
            }
            auto value_count = values.value()->get_value_count();
            if (!value_count.has_value()) {
                return std::unexpected(std::move(value_count.error()));
            }
            std::vector<int64_t> local_to_global(static_cast<size_t>(value_count.value()), -1);
            for (int64_t ord = 0; ord < value_count.value(); ++ord) {
                auto term = values.value()->lookup_ord(ord);
                if (!term.has_value()) {
                    return std::unexpected(std::move(term.error()));
                }
                local_to_global[static_cast<size_t>(ord)] = dictionary_lookup(dictionary, term.value().as_slice());
            }

            while (true) {
                auto doc = values.value()->next_doc();
                if (!doc.has_value()) {
                    return std::unexpected(std::move(doc.error()));
                }
                if (doc.value() == NO_MORE_DOCS) {
                    break;
                }
                auto mapped = map_doc(*merge_state_, source.segment, doc.value());
                if (!mapped.has_value()) {
                    return std::unexpected(std::move(mapped.error()));
                }
                const bool live = mapped.value() >= 0;
                // Deleted documents still have to be drained, so their ords go to
                // a scratch buffer.
                discarded.clear();
                auto &target = live ? ords[static_cast<size_t>(mapped.value())] : discarded;
                while (true) {
                    auto ord = values.value()->next_ord();
                    if (!ord.has_value()) {
                        return std::unexpected(std::move(ord.error()));
                    }
                    if (ord.value() == NO_MORE_ORDS) {
                        break;
                    }
                    if (live) {
                        target.push_back(local_to_global[static_cast<size_t>(ord.value())]);
                    }
                }
            }
        }

        return std::unique_ptr<SortedSetDocValues>(
                new InMemorySortedSetDocValues(std::move(ords), std::move(dictionary)));
    }

    [[nodiscard]] Result<std::unique_ptr<SortedNumericDocValues>> build_sorted_numeric(const FieldInfo &field) const {
        const auto sources = sources_for(field, *merge_state_, DocValuesType::SortedNumeric);

        std::vector<std::vector<int64_t>> values(static_cast<size_t>(merge_state_->segment_info.max_doc));
        for (const auto &source : sources) {
            auto cursor = source.producer->get()->get_sorted_numeric(*source.reader_field_info);
            if (!cursor.has_value()) {
                return std::unexpected(std::move(cursor.error()));
            }
            while (true) {
                auto doc = cursor.value()->next_doc();
                if (!doc.has_value()) {
                    return std::unexpected(std::move(doc.error()));
                }
                if (doc.value() == NO_MORE_DOCS) {
                    break;
                }
                auto mapped = map_doc(*merge_state_, source.segment, doc.value());
                if (!mapped.has_value()) {
                    return std::unexpected(std::move(mapped.error()));
                }
                const bool live = mapped.value() >= 0;
                auto count = cursor.value()->doc_value_count();
                if (!count.has_value()) {
                    return std::unexpected(std::move(count.error()));
                }
                std::vector<int64_t> *target = live ? &values[static_cast<size_t>(mapped.value())] : nullptr;
                for (int32_t i = 0; i < count.value(); ++i) {
                    auto value = cursor.value()->next_value();
                    if (!value.has_value()) {
                        return std::unexpected(std::move(value.error()));
                    }
                    if (target != nullptr) {
                        target->push_back(value.value());
                    }
                }
            }
        }

        return std::unique_ptr<SortedNumericDocValues>(
                new InMemorySortedNumericDocValues(std::move(values)));
    }

    const FieldInfo *merge_field_info_;
    const MergeState *merge_state_;
};

} // namespace

VoidResult DocValuesConsumer::merge(const MergeState &merge_state) {
    for (const auto &producer : merge_state.doc_values_producers) {
        if (producer == nullptr) {
            continue;
        }
        if (auto checked = producer->check_integrity(); !checked.has_value()) {
            return checked;
        }
    }

    for (const auto &field : merge_state.merge_field_infos->fields()) {
        switch (field.doc_values_type) {
            case DocValuesType::None:
                continue;
            case DocValuesType::Numeric:
                if (auto merged = merge_numeric_field(field, merge_state); !merged.has_value()) {
                    return merged;
                }
                break;
            case DocValuesType::Binary:
                if (auto merged = merge_binary_field(field, merge_state); !merged.has_value()) {
                    return merged;
                }
                break;
            case DocValuesType::Sorted:
                if (auto merged = merge_sorted_field(field, merge_state); !merged.has_value()) {
                    return merged;
                }
                break;
            case DocValuesType::SortedSet:
                if (auto merged = merge_sorted_set_field(field, merge_state); !merged.has_value()) {
                    return merged;
                }
                break;
            case DocValuesType::SortedNumeric:
                if (auto merged = merge_sorted_numeric_field(field, merge_state); !merged.has_value()) {
                    return merged;
                }
                break;
            default:
                return err(LuceneError::illegal_state(
                        std::format("type= {}", static_cast<int32_t>(field.doc_values_type))));
        }
    }
    return {};
}

VoidResult DocValuesConsumer::merge_numeric_field(const FieldInfo &merge_field_info,
                                                  const MergeState &merge_state) {
    DocValuesMergeProducer producer(merge_field_info, merge_state);
    return add_numeric_field(merge_field_info, producer);
}

VoidResult DocValuesConsumer::merge_binary_field(const FieldInfo &merge_field_info, const MergeState &merge_state) {
    DocValuesMergeProducer producer(merge_field_info, merge_state);
    return add_binary_field(merge_field_info, producer);
}

VoidResult DocValuesConsumer::merge_sorted_field(const FieldInfo &merge_field_info, const MergeState &merge_state) {
    DocValuesMergeProducer producer(merge_field_info, merge_state);
    return add_sorted_field(merge_field_info, producer);
}

VoidResult DocValuesConsumer::merge_sorted_numeric_field(const FieldInfo &merge_field_info,
                                                         const MergeState &merge_state) {
    DocValuesMergeProducer producer(merge_field_info, merge_state);
    return add_sorted_numeric_field(merge_field_info, producer);
}

VoidResult DocValuesConsumer::merge_sorted_set_field(const FieldInfo &merge_field_info,
                                                     const MergeState &merge_state) {
    DocValuesMergeProducer producer(merge_field_info, merge_state);
    return add_sorted_set_field(merge_field_info, producer);
}

} // namespace lucene
