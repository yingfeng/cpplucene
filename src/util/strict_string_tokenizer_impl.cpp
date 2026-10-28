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

module lucene_core:strict_string_tokenizer.impl;

import :strict_string_tokenizer;

import std;

import :error;
import :types;

namespace lucene {

Result<std::string_view> StrictStringTokenizer::next_token() {
    if (!pos_.has_value()) {
        return err(LuceneError::illegal_state("no more tokens"));
    }
    const size_t start = pos_.value();
    if (start >= text_.size()) {
        pos_.reset();
        return err(LuceneError::illegal_state("no more tokens"));
    }
    const size_t end = text_.find(delimiter_, start);
    if (end == std::string_view::npos) {
        pos_.reset();
        return text_.substr(start);
    }
    pos_ = end + 1;
    return text_.substr(start, end - start);
}

} // namespace lucene
