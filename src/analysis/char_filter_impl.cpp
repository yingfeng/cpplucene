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

module lucene_core:char_filter.impl;

import :char_filter;

import :analyzer;
import :error;
import :token_stream;
import :types;

import std;

namespace lucene {

CharFilterAnalyzer::CharFilterAnalyzer(std::unique_ptr<Analyzer> delegate,
                                       std::vector<std::unique_ptr<CharFilter>> filters)
    : Analyzer(delegate->reuse_strategy()), delegate_(std::move(delegate)), filters_(std::move(filters)) {
    debug_check(delegate_ != nullptr, "a char filter analyzer needs a wrapped analyzer");
}

Result<TokenStream *> CharFilterAnalyzer::token_stream(std::string_view field_name, std::string_view text) {
    std::string transformed(text);
    for (const std::unique_ptr<CharFilter> &filter : filters_) {
        auto filtered = filter->filter(transformed);
        if (!filtered.has_value()) {
            return std::unexpected(std::move(filtered.error()));
        }
        transformed = std::move(filtered.value());
    }
    return delegate_->token_stream(field_name, transformed);
}

std::string CharFilterAnalyzer::to_string() const {
    std::string text = "CharFilterAnalyzer(";
    for (const std::unique_ptr<CharFilter> &filter : filters_) {
        text += filter->to_string();
        text += ", ";
    }
    text += delegate_->to_string();
    text += ")";
    return text;
}

Result<std::unique_ptr<TokenStreamComponents>> CharFilterAnalyzer::create_components(std::string_view /*field_name*/) {
    return err(LuceneError::illegal_state("a char filter analyzer delegates its components to the wrapped analyzer"));
}

} // namespace lucene
