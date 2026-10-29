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

module lucene_core:cjk_analyzer.impl;

import :cjk_analyzer;

import :analyzer;
import :cjk_bigram_filter;
import :cjk_width_filter;
import :lower_case_filter;
import :standard_tokenizer;
import :stop_filter;
import :token_stream;
import :types;

import std;

namespace lucene {

CJKAnalyzer::CJKAnalyzer() : CJKAnalyzer(std::set<std::string, std::less<>>{}) {}

CJKAnalyzer::CJKAnalyzer(std::set<std::string, std::less<>> stop_words)
    : Analyzer(ReuseStrategy::Global), stop_words_(std::move(stop_words)) {}

Result<std::unique_ptr<TokenStreamComponents>> CJKAnalyzer::create_components(std::string_view /*field_name*/) {
    auto tokenizer = std::make_unique<StandardTokenizer>();
    auto *head = tokenizer.get();

    std::unique_ptr<TokenStream> chain = std::make_unique<CJKWidthFilter>(std::move(tokenizer));
    chain = std::make_unique<LowerCaseFilter>(std::move(chain));
    chain = std::make_unique<CJKBigramFilter>(std::move(chain));
    if (!stop_words_.empty()) {
        chain = std::make_unique<StopFilter>(std::move(chain), stop_words_);
    }

    return std::unique_ptr<TokenStreamComponents>(std::make_unique<TokenStreamComponents>(head, std::move(chain)));
}

} // namespace lucene
