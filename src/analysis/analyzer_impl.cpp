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

module lucene_core:analyzer.impl;

import :analyzer;
import :types;
import :error;
import :token_stream;
import :tokenizer;
import :debug;

import std;

namespace lucene {

TokenStreamComponents::TokenStreamComponents(Tokenizer *source, std::unique_ptr<TokenStream> tail)
    : source_(source), tail_(std::move(tail)) {
    debug_check(source_ != nullptr && tail_ != nullptr);
}

Analyzer::Analyzer(ReuseStrategy strategy) : strategy_(strategy) {}

Result<TokenStreamComponents *> Analyzer::components_for(std::string_view field_name) {
    if (strategy_ == ReuseStrategy::Global) {
        if (!global_components_) {
            auto created = create_components(field_name);
            if (!created) {
                return std::unexpected(std::move(created.error()));
            }
            global_components_ = std::move(created.value());
        }
        return global_components_.get();
    }

    auto it = per_field_components_.find(field_name);
    if (it != per_field_components_.end()) {
        return it->second.get();
    }
    auto created = create_components(field_name);
    if (!created) {
        return std::unexpected(std::move(created.error()));
    }
    auto *raw = created.value().get();
    per_field_components_.emplace(std::string(field_name), std::move(created.value()));
    return raw;
}

Result<TokenStream *> Analyzer::token_stream(std::string_view field_name, std::string_view text) {
    auto components = components_for(field_name);
    if (!components) {
        return std::unexpected(std::move(components.error()));
    }
    if (auto r = components.value()->set_reader(text); !r) {
        return std::unexpected(std::move(r.error()));
    }
    TokenStream &sink = components.value()->token_stream();
    if (auto r = sink.reset(); !r) {
        return std::unexpected(std::move(r.error()));
    }
    return &sink;
}

} // namespace lucene
