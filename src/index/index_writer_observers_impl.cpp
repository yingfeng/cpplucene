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

module lucene_core:index_writer_observers.impl;

import :index_writer_observers;
import :types;
import std;

import :field_infos;
import :info_stream;
import :leaf_reader;
import :terms;

namespace lucene {

std::string MergeStat::Segment::id_hex() const {
    static constexpr char DIGITS[] = "0123456789abcdef";
    std::string text;
    text.reserve(id.size() * 2);
    for (const uint8_t byte : id) {
        text.push_back(DIGITS[(byte >> 4) & 0x0FU]);
        text.push_back(DIGITS[byte & 0x0FU]);
    }
    return text;
}

std::string MergeStat::to_string() const {
    std::string text = "MergeStat(gen=" + std::to_string(merge_gen) + ", segments=[";
    for (size_t i = 0; i < segments.size(); ++i) {
        if (i > 0) {
            text += ", ";
        }
        text += segments[i].name;
    }
    text += "])";
    return text;
}

SimpleMergedSegmentWarmer::SimpleMergedSegmentWarmer(InfoStreamMT info_stream)
    : info_stream_(std::move(info_stream)) {}

VoidResult SimpleMergedSegmentWarmer::warm(LeafReader &reader) {
    indexed_fields_ = 0;
    doc_values_fields_ = 0;
    norms_fields_ = 0;

    auto infos = reader.field_infos();
    if (!infos) {
        return std::unexpected(std::move(infos.error()));
    }
    if (infos.value() == nullptr) {
        return {};
    }

    for (const FieldInfo &info : infos.value()->fields()) {
        if (info.is_indexed()) {
            auto terms = reader.terms(info.name);
            if (!terms) {
                return std::unexpected(std::move(terms.error()));
            }
            indexed_fields_ += 1;
        }
        if (info.has_doc_values()) {
            // One cursor per doc values type; opening it is what warms the block
            // index of the field.
            auto value_count = reader.doc_count(info.name);
            if (!value_count) {
                return std::unexpected(std::move(value_count.error()));
            }
            doc_values_fields_ += 1;
        }
        if (info.has_norms()) {
            auto norms = reader.get_norm_values(info.name);
            if (!norms) {
                return std::unexpected(std::move(norms.error()));
            }
            norms_fields_ += 1;
        }
    }

    if (info_stream_ != nullptr) {
        auto logged = info_stream_->message(
                "IW", std::format("Warm phase took {:.3f}s for {} fields, {} doc values fields, {} norms",
                                  0.0, indexed_fields_, doc_values_fields_, norms_fields_));
        if (!logged) {
            return logged;
        }
    }
    return {};
}

} // namespace lucene
