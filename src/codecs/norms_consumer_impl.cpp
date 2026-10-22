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

module lucene_core:norms_consumer.impl;

import :norms_consumer;

import std;

import :doc_values_merge;
import :error;
import :field_infos;
import :merge_state;
import :norms_producer;
import :numeric_doc_values;

namespace lucene {

namespace {

/// A [`NormsProducer`] that forwards to the per-segment norms producers in a
/// merge, filtering deleted documents.
///
/// Port of the reference implementation's `NormsProducerMerge`.
class NormsProducerMerge final : public NormsProducer {
public:
    NormsProducerMerge(const FieldInfo &merge_field_info, const MergeState &merge_state)
            : merge_field_info_(&merge_field_info), merge_state_(&merge_state) {}

    [[nodiscard]] Result<std::unique_ptr<NumericDocValues>> get_norms(const FieldInfo &field) const override {
        if (&field != merge_field_info_) {
            return err(LuceneError::illegal_argument("wrong fieldInfo"));
        }

        std::vector<NumericDocValuesSub> subs;
        subs.reserve(merge_state_->norms_producers.size());
        for (size_t i = 0; i < merge_state_->norms_producers.size(); ++i) {
            const auto &norms_producer = merge_state_->norms_producers[i];
            if (norms_producer == nullptr) {
                continue;
            }
            const FieldInfos &reader_infos = *merge_state_->field_infos->at(i);
            const FieldInfo *reader_field_info = reader_infos.by_name(merge_field_info_->name);
            if (reader_field_info == nullptr || !reader_field_info->has_norms()) {
                continue;
            }
            auto norms = norms_producer->get_norms(*reader_field_info);
            if (!norms.has_value()) {
                return std::unexpected(std::move(norms.error()));
            }
            subs.emplace_back(std::move(norms.value()), *merge_state_->doc_maps->at(i));
        }

        return NumericDocValuesMerge::create(std::move(subs), merge_state_->needs_index_sort);
    }

    [[nodiscard]] VoidResult check_integrity() const override { return {}; }

private:
    const FieldInfo *merge_field_info_;
    const MergeState *merge_state_;
};

} // namespace

VoidResult NormsConsumer::merge(const MergeState &merge_state) {
    for (const auto &producer : merge_state.norms_producers) {
        if (producer == nullptr) {
            continue;
        }
        if (auto checked = producer->check_integrity(); !checked.has_value()) {
            return checked;
        }
    }

    for (const auto &field_info : merge_state.merge_field_infos->fields()) {
        if (field_info.has_norms()) {
            if (auto merged = merge_norms_field(field_info, merge_state); !merged.has_value()) {
                return merged;
            }
        }
    }
    return {};
}

VoidResult NormsConsumer::merge_norms_field(const FieldInfo &merge_field_info, const MergeState &merge_state) {
    NormsProducerMerge producer(merge_field_info, merge_state);
    return add_norms_field(merge_field_info, producer);
}

} // namespace lucene
