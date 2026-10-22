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

module lucene_core:simple_analyzers.impl;

import :simple_analyzers;
import :types;
import :error;
import :analyzer;
import :token_stream;
import :tokenizer;
import :lower_case_filter;

import std;

namespace lucene {

WhitespaceAnalyzer::WhitespaceAnalyzer() : Analyzer(ReuseStrategy::Global) {}

Result<std::unique_ptr<TokenStreamComponents>> WhitespaceAnalyzer::create_components(std::string_view /*field_name*/) {
    auto tokenizer = std::make_unique<WhitespaceTokenizer>();
    auto *head = tokenizer.get();
    return std::unique_ptr<TokenStreamComponents>(
        std::make_unique<TokenStreamComponents>(head, std::move(tokenizer)));
}

SimpleAnalyzer::SimpleAnalyzer() : Analyzer(ReuseStrategy::Global) {}

Result<std::unique_ptr<TokenStreamComponents>> SimpleAnalyzer::create_components(std::string_view /*field_name*/) {
    auto tokenizer = std::make_unique<LetterTokenizer>();
    auto *head = tokenizer.get();
    std::unique_ptr<TokenStream> chain = std::make_unique<LowerCaseFilter>(std::move(tokenizer));
    return std::unique_ptr<TokenStreamComponents>(
        std::make_unique<TokenStreamComponents>(head, std::move(chain)));
}

KeywordAnalyzer::KeywordAnalyzer() : Analyzer(ReuseStrategy::Global) {}

Result<std::unique_ptr<TokenStreamComponents>> KeywordAnalyzer::create_components(std::string_view /*field_name*/) {
    auto tokenizer = std::make_unique<KeywordTokenizer>();
    auto *head = tokenizer.get();
    return std::unique_ptr<TokenStreamComponents>(
        std::make_unique<TokenStreamComponents>(head, std::move(tokenizer)));
}

} // namespace lucene
