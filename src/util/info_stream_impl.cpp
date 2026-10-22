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

module lucene_core:info_stream.impl;

import :error;
import :info_stream;
import :types;

import std;

namespace lucene {

namespace {

/// Guards [`default_stream`].  The reference implementation uses a `RwLock`, but the stream is
/// swapped rarely and read once per `IndexWriter`, so a plain mutex is enough.
std::mutex &default_stream_mutex() {
    static std::mutex mutex;
    return mutex;
}

/// The global default stream; `NoOutput` until somebody calls
/// [`set_default_info_stream`].
InfoStreamMT &default_stream() {
    static InfoStreamMT stream = std::make_shared<InfoStreamEnum>();
    return stream;
}

} // namespace

InfoStreamEnum::InfoStreamEnum(std::shared_ptr<InfoStream> stream) {
    if (stream != nullptr) {
        inner_ = std::move(stream);
    }
}

VoidResult InfoStreamEnum::message(std::string_view component, std::string_view text) const {
    return std::visit(
            [component, text](const auto &inner) -> VoidResult {
                using Inner = std::decay_t<decltype(inner)>;
                if constexpr (std::is_same_v<Inner, NoOutput>) {
                    return inner.message(component, text);
                } else {
                    if (inner == nullptr) {
                        return err(LuceneError::illegal_state("InfoStreamEnum wraps a null InfoStream"));
                    }
                    return inner->message(component, text);
                }
            },
            inner_);
}

bool InfoStreamEnum::is_enabled(std::string_view component) const {
    return std::visit(
            [component](const auto &inner) {
                using Inner = std::decay_t<decltype(inner)>;
                if constexpr (std::is_same_v<Inner, NoOutput>) {
                    return inner.is_enabled(component);
                } else {
                    return inner != nullptr && inner->is_enabled(component);
                }
            },
            inner_);
}

VoidResult InfoStreamEnum::close() const {
    return std::visit(
            [](const auto &inner) -> VoidResult {
                using Inner = std::decay_t<decltype(inner)>;
                if constexpr (std::is_same_v<Inner, NoOutput>) {
                    return inner.close();
                } else {
                    return inner == nullptr ? VoidResult{} : inner->close();
                }
            },
            inner_);
}

InfoStreamMT get_default_info_stream() {
    std::lock_guard lock(default_stream_mutex());
    return default_stream();
}

void set_default_info_stream(InfoStreamMT info_stream) {
    if (info_stream == nullptr) {
        info_stream = std::make_shared<InfoStreamEnum>();
    }
    InfoStreamMT previous;
    {
        std::lock_guard lock(default_stream_mutex());
        previous = std::exchange(default_stream(), std::move(info_stream));
    }
    // Released after the lock: a custom stream's destructor may use InfoStream.
    previous.reset();
}

void set_default_info_stream(std::shared_ptr<InfoStream> info_stream) {
    set_default_info_stream(std::make_shared<InfoStreamEnum>(std::move(info_stream)));
}

} // namespace lucene
