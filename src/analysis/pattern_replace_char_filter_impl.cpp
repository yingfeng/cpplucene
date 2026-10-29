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

module lucene_core:pattern_replace_char_filter.impl;

import :pattern_replace_char_filter;

import :char_filter;
import :error;
import :types;

import std;

namespace lucene {

PatternReplaceCharFilter::PatternReplaceCharFilter(std::string pattern, std::string replacement, std::regex compiled)
    : pattern_(std::move(pattern)), replacement_(std::move(replacement)), compiled_(std::move(compiled)) {}

Result<PatternReplaceCharFilter> PatternReplaceCharFilter::create(std::string pattern, std::string replacement) {
    try {
        std::regex compiled(pattern);
        return PatternReplaceCharFilter(std::move(pattern), std::move(replacement), std::move(compiled));
    } catch (const std::regex_error &error) {
        return err(LuceneError::illegal_argument("invalid pattern `" + pattern + "`: " + error.what()));
    }
}

Result<std::string> PatternReplaceCharFilter::filter(std::string_view input) const {
    try {
        return std::regex_replace(std::string(input), compiled_, replacement_);
    } catch (const std::regex_error &error) {
        return err(LuceneError::illegal_state(std::string("pattern replacement failed: ") + error.what()));
    }
}

std::string PatternReplaceCharFilter::to_string() const {
    return "PatternReplaceCharFilter(" + pattern_ + " -> " + replacement_ + ")";
}

} // namespace lucene
