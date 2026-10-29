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

module lucene_core:porter_stem_filter.impl;

import :porter_stem_filter;

import :attributes;
import :porter_stemmer;
import :token_stream;
import :types;

import std;

namespace lucene {

PorterStemFilter::PorterStemFilter(std::unique_ptr<TokenStream> input) : TokenFilter(std::move(input)) {
    attributes().add_attribute<CharTermAttribute>();
    attributes().add_attribute<KeywordAttribute>();
}

Result<bool> PorterStemFilter::increment_token() {
    auto more = input_->increment_token();
    if (!more.has_value()) {
        return std::unexpected(std::move(more.error()));
    }
    if (!more.value()) {
        return false;
    }

    auto *keyword = attributes().get_attribute<KeywordAttribute>();
    if (keyword != nullptr && keyword->is_keyword()) {
        // Protected terms keep their exact form.
        return true;
    }
    auto *term = attributes().get_attribute<CharTermAttribute>();
    if (term != nullptr && !term->empty()) {
        std::string stemmed = stemmer_.stem(term->text());
        if (stemmed != term->text()) {
            term->set_text(stemmed);
        }
    }
    return true;
}

} // namespace lucene
