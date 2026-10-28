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

module lucene_core:per_field_merge_state.impl;

import :per_field_merge_state;

import :types;
import std;

import :close;
import :error;
import :field_infos;
import :fields;
import :fields_producer;
import :merge_state;
import :terms;

namespace lucene {

namespace {

/// Iterates the field names of a [`FilterFieldsProducer`].
class NameListIterator final : public FieldNameIterator {
public:
    explicit NameListIterator(const std::vector<std::string> &names) : names_(&names) {}

    Result<const std::string *> next() override {
        if (index_ >= names_->size()) {
            return static_cast<const std::string *>(nullptr);
        }
        return &(*names_)[index_++];
    }

private:
    const std::vector<std::string> *names_;
    size_t index_{0};
};

/// Exposes a slice of another producer's fields, for one per-field group.
///
/// Port of `FilterFieldsProducer`.  It owns its delegate (the reference
/// implementation borrows it) so a restricted merge state never dangles.
class FilterFieldsProducer final : public FieldsProducer {
public:
    FilterFieldsProducer(std::shared_ptr<FieldsProducer> in, std::vector<std::string> fields)
            : in_(std::move(in)), fields_(std::move(fields)) {}

    Result<std::unique_ptr<FieldNameIterator>> iterator() const override {
        return std::unique_ptr<FieldNameIterator>(new NameListIterator(fields_));
    }

    Result<std::unique_ptr<Terms>> terms(std::string_view field) const override {
        if (std::find(fields_.begin(), fields_.end(), field) == fields_.end()) {
            std::string available;
            for (const auto &name : fields_) {
                if (!available.empty()) {
                    available += ", ";
                }
                available += name;
            }
            return err(LuceneError::illegal_argument("The field named '" + std::string(field) +
                                                     "' is not accessible in the current merge context, "
                                                     "available ones are: [" +
                                                     available + "]"));
        }
        return in_->terms(field);
    }

    Result<int32_t> size() const override { return static_cast<int32_t>(fields_.size()); }

    VoidResult check_integrity() const override { return in_->check_integrity(); }

    /// Does nothing: the delegate belongs to the merge state this view borrows
    /// from, which closes it.
    VoidResult close() const override { return {}; }

private:
    std::shared_ptr<FieldsProducer> in_;
    std::vector<std::string> fields_;
};

} // namespace

std::unique_ptr<MergeState> restrict_merge_state_fields(const MergeState &state,
                                                        const std::vector<std::string> &fields) {
    const std::set<std::string> names(fields.begin(), fields.end());

    std::vector<std::shared_ptr<FieldsProducer>> producers;
    producers.reserve(state.fields_producers.size());
    for (const auto &producer : state.fields_producers) {
        if (producer == nullptr) {
            producers.push_back(nullptr);
            continue;
        }
        producers.push_back(std::make_shared<FilterFieldsProducer>(producer, fields));
    }

    std::shared_ptr<const FieldInfos> merge_field_infos;
    if (state.merge_field_infos != nullptr) {
        merge_field_infos = std::make_shared<const FieldInfos>(state.merge_field_infos->with_filter(names));
    }

    MergeState::FieldInfosList field_infos;
    if (state.field_infos != nullptr) {
        field_infos.reserve(state.field_infos->size());
        for (const auto &infos : *state.field_infos) {
            if (infos == nullptr) {
                field_infos.push_back(nullptr);
                continue;
            }
            field_infos.push_back(std::make_shared<const FieldInfos>(infos->with_filter(names)));
        }
    } else {
        field_infos.resize(producers.size());
    }

    return MergeState::copy_replacing(state, std::move(producers), std::move(merge_field_infos),
                                      std::move(field_infos));
}

} // namespace lucene
