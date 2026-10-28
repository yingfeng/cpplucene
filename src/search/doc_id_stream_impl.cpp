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

module lucene_core:doc_id_stream.impl;

import :doc_id_stream;

import std;

import :doc_id_set_iterator;
import :error;
import :scorable;
import :types;

namespace lucene {

Result<int32_t> DocIdStream::count(Scorable &scorer) {
    CountConsumer counter(scorer);
    if (auto consumed = for_each(counter); !consumed.has_value()) {
        return std::unexpected(std::move(consumed.error()));
    }
    return counter.count();
}

Result<std::optional<int32_t>> DocIdSetIteratorStream::next_doc() {
    if (pending_.has_value()) {
        const int32_t doc = pending_.value();
        pending_.reset();
        return std::optional<int32_t>{doc};
    }
    auto doc = iterator_->next_doc();
    if (!doc.has_value()) {
        return std::unexpected(std::move(doc.error()));
    }
    if (doc.value() == NO_MORE_DOCS) {
        return std::optional<int32_t>{};
    }
    return std::optional<int32_t>{doc.value()};
}

VoidResult DocIdSetIteratorStream::for_each(DocIdStreamConsumer &consumer) {
    while (true) {
        auto doc = next_doc();
        if (!doc.has_value()) {
            return std::unexpected(std::move(doc.error()));
        }
        if (!doc.value().has_value()) {
            return {};
        }
        if (doc.value().value() >= max_) {
            // The window ended: keep the document for the next window.
            pending_ = doc.value();
            return {};
        }
        if (auto consumed = consumer.accept(doc.value().value()); !consumed.has_value()) {
            return consumed;
        }
    }
}

Result<int32_t> DocIdSetIteratorStream::count(Scorable & /*scorer*/) {
    int32_t count = 0;
    while (true) {
        auto doc = next_doc();
        if (!doc.has_value()) {
            return std::unexpected(std::move(doc.error()));
        }
        if (!doc.value().has_value()) {
            return count;
        }
        if (doc.value().value() >= max_) {
            pending_ = doc.value();
            return count;
        }
        ++count;
    }
}

} // namespace lucene
