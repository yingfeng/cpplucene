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

module lucene_core:standard_analyzer.impl;

import :standard_analyzer;
import :types;
import :error;
import :analyzer;
import :token_stream;
import :tokenizer;
import :standard_tokenizer;
import :lower_case_filter;
import :stop_filter;

import std;

namespace lucene {

StandardAnalyzer::StandardAnalyzer() : StandardAnalyzer(StopFilter::english_stop_words()) {}

StandardAnalyzer::StandardAnalyzer(std::set<std::string, std::less<>> stop_words)
    : Analyzer(ReuseStrategy::Global), stop_words_(std::move(stop_words)) {}

VoidResult StandardAnalyzer::set_max_token_length(size_t length) {
    if (length < 1) {
        return err(LuceneError::illegal_argument("maxTokenLength must be greater than zero"));
    }
    if (length > StandardTokenizer::MAX_TOKEN_LENGTH_LIMIT) {
        return err(LuceneError::illegal_argument(
            "maxTokenLength may not exceed " + std::to_string(StandardTokenizer::MAX_TOKEN_LENGTH_LIMIT)));
    }
    max_token_length_ = length;
    return {};
}

Result<std::unique_ptr<TokenStreamComponents>> StandardAnalyzer::create_components(
    std::string_view /*field_name*/) {
    auto tokenizer = std::make_unique<StandardTokenizer>(max_token_length_);
    auto *head = tokenizer.get();
    std::unique_ptr<TokenStream> chain = std::make_unique<LowerCaseFilter>(std::move(tokenizer));
    chain = std::make_unique<StopFilter>(std::move(chain), stop_words_);
    return std::unique_ptr<TokenStreamComponents>(std::make_unique<TokenStreamComponents>(head, std::move(chain)));
}

} // namespace lucene
