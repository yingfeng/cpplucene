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

module lucene_core:ngram_tokenizer.impl;

import :ngram_tokenizer;

import :error;
import :tokenizer;
import :types;

import std;

namespace lucene {

NGramTokenizer::NGramTokenizer(int32_t min_gram, int32_t max_gram)
    : min_gram_(min_gram), max_gram_(max_gram), gram_size_(min_gram) {
    // Constructors cannot report an error in C++, so an invalid range is clamped
    // to a usable one; callers that need the error can call `validate`.
    if (min_gram_ < 1) {
        min_gram_ = 1;
    }
    if (max_gram_ < min_gram_) {
        max_gram_ = min_gram_;
    }
    gram_size_ = min_gram_;
}

VoidResult NGramTokenizer::reset() {
    gram_start_ = 0;
    gram_size_ = min_gram_;
    started_ = false;
    return Tokenizer::reset();
}

Result<bool> NGramTokenizer::increment_token() {
    while (true) {
        if (!started_) {
            started_ = true;
            gram_start_ = 0;
            gram_size_ = min_gram_;
        }
        if (gram_size_ > max_gram_ || gram_start_ + static_cast<size_t>(gram_size_) > code_point_count()) {
            ++gram_start_;
            gram_size_ = min_gram_;
        }
        if (gram_start_ + static_cast<size_t>(gram_size_) <= code_point_count()) {
            emit_token(gram_start_, gram_start_ + static_cast<size_t>(gram_size_));
            ++gram_size_;
            return true;
        }
        return false;
    }
}

EdgeNGramTokenizer::EdgeNGramTokenizer(int32_t min_gram, int32_t max_gram)
    : min_gram_(min_gram), max_gram_(max_gram), gram_size_(min_gram) {
    if (min_gram_ < 1) {
        min_gram_ = 1;
    }
    if (max_gram_ < min_gram_) {
        max_gram_ = min_gram_;
    }
    gram_size_ = min_gram_;
}

VoidResult EdgeNGramTokenizer::reset() {
    gram_size_ = min_gram_;
    started_ = false;
    return Tokenizer::reset();
}

Result<bool> EdgeNGramTokenizer::increment_token() {
    if (!started_) {
        started_ = true;
        gram_size_ = min_gram_;
    }
    if (gram_size_ > max_gram_ || static_cast<size_t>(gram_size_) > code_point_count()) {
        return false;
    }
    emit_token(0, static_cast<size_t>(gram_size_));
    ++gram_size_;
    return true;
}

} // namespace lucene
