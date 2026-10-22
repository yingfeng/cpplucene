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

export module lucene_core:simple_analyzers;

import :types;
import :error;

import std;

import :analyzer;
import :token_stream;
import :tokenizer;

namespace lucene {

/// Splits on whitespace and keeps the tokens unchanged (`WhitespaceAnalyzer`).
export class WhitespaceAnalyzer : public Analyzer {
public:
    WhitespaceAnalyzer();
    [[nodiscard]] std::string to_string() const override { return "WhitespaceAnalyzer"; }

protected:
    [[nodiscard]] Result<std::unique_ptr<TokenStreamComponents>> create_components(
        std::string_view field_name) override;
};

/// Splits on non-letters and lower-cases (`SimpleAnalyzer`).
export class SimpleAnalyzer : public Analyzer {
public:
    SimpleAnalyzer();
    [[nodiscard]] std::string to_string() const override { return "SimpleAnalyzer"; }

protected:
    [[nodiscard]] Result<std::unique_ptr<TokenStreamComponents>> create_components(
        std::string_view field_name) override;
};

/// Emits the whole input as one token (`KeywordAnalyzer`).
export class KeywordAnalyzer : public Analyzer {
public:
    KeywordAnalyzer();
    [[nodiscard]] std::string to_string() const override { return "KeywordAnalyzer"; }

protected:
    [[nodiscard]] Result<std::unique_ptr<TokenStreamComponents>> create_components(
        std::string_view field_name) override;
};

} // namespace lucene
