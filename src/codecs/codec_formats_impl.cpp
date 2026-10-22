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

module lucene_core:codec_formats.impl;

import :codec_formats;

import :codec;
import :error;
import :lucene101_codec;
import :lucene99_codec;
import :types;

import std;

namespace lucene {

namespace {

/// Name → codec.  Owned through `shared_ptr<const Codec>` so a registered codec
/// can outlive the caller that created it.
using Registry = std::map<std::string, std::shared_ptr<const Codec>>;

struct RegistryHolder {
    std::mutex mutex;
    Registry codecs;
    /// Installed once, lazily, so no static initialisation order is assumed.
    bool builtins_installed{false};
};

RegistryHolder &holder() {
    static RegistryHolder instance;
    return instance;
}

/// Adds the built-in codecs; the caller holds the mutex.
void install_builtins(Registry &codecs) {
    if (codecs.find(std::string(Lucene101Codec::CODEC_NAME)) == codecs.end()) {
        auto codec = Lucene101Codec::create();
        if (codec.has_value()) {
            std::shared_ptr<const Codec> shared(std::move(codec.value()));
            codecs.emplace(std::string(Lucene101Codec::CODEC_NAME), std::move(shared));
        }
    }
    if (codecs.find(std::string(Lucene99Codec::CODEC_NAME)) == codecs.end()) {
        codecs.emplace(std::string(Lucene99Codec::CODEC_NAME), std::make_shared<Lucene99Codec>());
    }
}

/// The registry, with the built-ins in place.
Registry &registry() {
    auto &state = holder();
    if (!state.builtins_installed) {
        install_builtins(state.codecs);
        state.builtins_installed = true;
    }
    return state.codecs;
}

} // namespace

const Codec &CodecFormats::default_codec() {
    const Codec *codec = for_name(Lucene101Codec::CODEC_NAME);
    if (codec != nullptr) {
        return *codec;
    }
    // `Lucene101Codec::create` only fails on invalid static parameters; falling
    // back to the metadata-only codec keeps the accessor total.
    const Codec *fallback = for_name(Lucene99Codec::CODEC_NAME);
    if (fallback == nullptr) {
        throw std::logic_error("lucene: no codec registered");
    }
    return *fallback;
}

const Codec *CodecFormats::for_name(std::string_view name) {
    const std::lock_guard<std::mutex> guard(holder().mutex);
    auto &codecs = registry();
    const auto found = codecs.find(std::string(name));
    if (found == codecs.end()) {
        return nullptr;
    }
    return found->second.get();
}

std::vector<std::string_view> CodecFormats::available_codecs() {
    const std::lock_guard<std::mutex> guard(holder().mutex);
    auto &codecs = registry();
    std::vector<std::string_view> names;
    names.reserve(codecs.size());
    for (const auto &[name, codec] : codecs) {
        (void)codec;
        names.push_back(name);
    }
    return names;
}

void CodecFormats::register_codec(std::shared_ptr<const Codec> codec) {
    if (codec == nullptr) {
        return;
    }
    const std::string name(codec->name());
    const std::lock_guard<std::mutex> guard(holder().mutex);
    auto &codecs = registry();
    codecs[name] = std::move(codec);
}

} // namespace lucene
