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

module lucene_core:field_infos_builder.impl;

import :field_infos_builder;

import :error;
import :field_infos;
import :index_types;
import :types;

import std;

namespace lucene {

Result<std::shared_ptr<FieldInfo>> FieldInfosBuilder::add(FieldInfo fi) {
    return add_with_dv_gen(std::move(fi), -1);
}

Result<std::shared_ptr<FieldInfo>> FieldInfosBuilder::add_existing(FieldInfo fi) {
    // The merged segment numbers its fields from scratch, so the number of the
    // source segment is meaningless here.
    fi.number = 0;
    return add_with_dv_gen(std::move(fi), -1);
}

Result<std::shared_ptr<FieldInfo>> FieldInfosBuilder::add_with_dv_gen(FieldInfo fi, int64_t dv_gen) {
    if (fi.number != 0) {
        return err(LuceneError::illegal_argument("the field number must be assigned by the builder"));
    }
    if (fi.name.empty()) {
        return err(LuceneError::illegal_argument("the field name must not be empty"));
    }

    auto existing = by_name_.find(fi.name);
    if (existing == by_name_.end()) {
        fi.number = static_cast<int32_t>(fields_.size());
        fi.doc_values_gen = dv_gen;
        auto shared = std::make_shared<FieldInfo>(std::move(fi));
        by_name_.emplace(shared->name, shared);
        fields_.push_back(shared);
        return shared;
    }

    // Merge into the field that was added before.
    auto &merged = *existing->second;
    merged.index_options = std::max(merged.index_options, fi.index_options);
    merged.doc_values_skip_index = std::max(merged.doc_values_skip_index, fi.doc_values_skip_index);
    merged.store_term_vector = merged.store_term_vector || fi.store_term_vector;
    merged.omit_norms = merged.omit_norms || fi.omit_norms;
    merged.store_payloads = merged.store_payloads || fi.store_payloads;
    merged.soft_deletes = merged.soft_deletes || fi.soft_deletes;
    merged.parent = merged.parent || fi.parent;

    if (fi.doc_values_type != DocValuesType::None) {
        if (merged.doc_values_type == DocValuesType::None) {
            merged.doc_values_type = fi.doc_values_type;
            merged.doc_values_gen = dv_gen;
        } else if (merged.doc_values_type != fi.doc_values_type) {
            return err(LuceneError::illegal_argument(
                    "cannot change the DocValues type of field \"" + fi.name + "\" from " +
                    std::to_string(static_cast<int32_t>(merged.doc_values_type)) + " to " +
                    std::to_string(static_cast<int32_t>(fi.doc_values_type))));
        }
    }

    if (fi.point_dimension_count != 0) {
        if (merged.point_dimension_count == 0) {
            merged.point_dimension_count = fi.point_dimension_count;
            merged.point_index_dimension_count = fi.point_index_dimension_count;
            merged.point_num_bytes = fi.point_num_bytes;
        } else if (merged.point_dimension_count != fi.point_dimension_count ||
                   merged.point_num_bytes != fi.point_num_bytes) {
            return err(LuceneError::illegal_argument("cannot change the point dimension or num bytes of field \"" +
                                                     fi.name + "\""));
        } else {
            merged.point_index_dimension_count =
                    std::max(merged.point_index_dimension_count, fi.point_index_dimension_count);
        }
    }

    if (fi.vector_dimension != 0) {
        if (merged.vector_dimension == 0) {
            merged.vector_dimension = fi.vector_dimension;
            merged.vector_encoding = fi.vector_encoding;
            merged.vector_similarity = fi.vector_similarity;
        } else if (merged.vector_dimension != fi.vector_dimension ||
                   merged.vector_encoding != fi.vector_encoding ||
                   merged.vector_similarity != fi.vector_similarity) {
            return err(LuceneError::illegal_argument(
                    "cannot change the vector dimension, encoding or similarity function of field \"" + fi.name +
                    "\""));
        }
    }

    for (const auto &[key, value] : fi.attributes) {
        merged.attributes[key] = value;
    }
    return existing->second;
}

std::shared_ptr<FieldInfo> FieldInfosBuilder::field_info(std::string_view name) const {
    auto found = by_name_.find(name);
    return found == by_name_.end() ? nullptr : found->second;
}

Result<FieldInfos> FieldInfosBuilder::finish() const {
    std::vector<FieldInfo> copies;
    copies.reserve(fields_.size());
    for (const auto &field : fields_) {
        copies.push_back(*field);
    }
    return FieldInfos::of(std::move(copies));
}

std::set<std::string> FieldInfosBuilder::get_field_names() const {
    std::set<std::string> names;
    for (const auto &[name, field] : by_name_) {
        (void)field;
        names.insert(name);
    }
    return names;
}

void FieldInfosBuilder::clear() noexcept {
    by_name_.clear();
    fields_.clear();
}

std::optional<std::string> FieldInfosBuilder::soft_deletes_field() const {
    for (const auto &[name, field] : by_name_) {
        if (field->soft_deletes) {
            return name;
        }
    }
    return std::nullopt;
}

std::optional<std::string> FieldInfosBuilder::parent_field() const {
    for (const auto &[name, field] : by_name_) {
        if (field->parent) {
            return name;
        }
    }
    return std::nullopt;
}

bool FieldInfosBuilder::is_soft_deletes_field_name(std::string_view field_name) const {
    auto found = by_name_.find(field_name);
    return found != by_name_.end() && found->second->soft_deletes;
}

bool FieldInfosBuilder::is_parent_field_name(std::string_view field_name) const {
    auto found = by_name_.find(field_name);
    return found != by_name_.end() && found->second->parent;
}

} // namespace lucene
