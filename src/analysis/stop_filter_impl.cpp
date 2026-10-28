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

module lucene_core:stop_filter.impl;

import :stop_filter;
import :types;
import :error;
import :attributes;
import :token_stream;

import std;

namespace lucene {

StopFilter::StopFilter(std::unique_ptr<TokenStream> input, const CharArraySet &stop_words)
        : TokenFilter(std::move(input)), char_stop_words_(stop_words) {}

bool StopFilter::is_stop_word(std::string_view word) const {
    if (char_stop_words_.has_value()) {
        return char_stop_words_.value().contains_key_str(word);
    }
    return stop_words_.contains(word);
}

StopFilter::StopFilter(std::unique_ptr<TokenStream> input, std::set<std::string, std::less<>> stop_words)
    : TokenFilter(std::move(input)), stop_words_(std::move(stop_words)) {
    attributes().add_attribute<CharTermAttribute>();
    attributes().add_attribute<PositionIncrementAttribute>();
}

Result<bool> StopFilter::increment_token() {
    while (true) {
        auto more = input_->increment_token();
        if (!more) {
            return std::unexpected(std::move(more.error()));
        }
        if (!more.value()) {
            return false;
        }
        auto *term = attributes().get_attribute<CharTermAttribute>();
        auto *position_increment = attributes().get_attribute<PositionIncrementAttribute>();
        const std::string_view text = term->text();
        if (!is_stop_word(text)) {
            if (skipped_positions_ != 0) {
                position_increment->set_position_increment(position_increment->position_increment() +
                                                           skipped_positions_);
                skipped_positions_ = 0;
            }
            return true;
        }
        // Drop the token but remember its position increment.
        skipped_positions_ += position_increment->position_increment();
    }
}

const std::set<std::string, std::less<>> &StopFilter::english_stop_words() {
    // `org.apache.lucene.analysis.en.EnglishAnalyzer.ENGLISH_STOP_WORDS_SET`
    // (the 33 "classic" words followed by the extended list).
    static const std::set<std::string, std::less<>> kWords = {
        "a",        "an",       "and",      "are",       "as",        "at",       "be",       "but",
        "by",       "for",      "if",       "in",        "into",      "is",       "it",       "no",
        "not",      "of",       "on",       "or",        "such",      "that",     "the",      "their",
        "then",     "there",    "these",    "they",      "this",      "to",       "was",      "will",
        "with",     "would",    "about",    "above",     "after",     "again",    "against",  "all",
        "am",       "any",      "because",  "been",      "before",    "being",    "below",    "between",
        "both",     "can",      "could",    "did",       "do",        "does",     "doing",    "down",
        "during",   "each",     "few",      "from",      "further",   "had",      "has",      "have",
        "having",   "he",       "her",      "here",      "hers",      "herself",  "him",      "himself",
        "his",      "how",      "i",        "its",       "itself",    "just",     "me",       "more",
        "most",     "my",       "myself",   "now",       "off",       "once",     "only",     "other",
        "our",      "ours",     "ourselves", "out",      "over",      "own",      "same",     "she",
        "should",   "so",       "some",     "than",      "them",      "themselves", "those",  "through",
        "too",      "under",    "until",    "up",        "very",      "we",       "were",     "what",
        "when",     "where",    "which",    "while",     "who",       "whom",     "why",      "you",
        "your",     "yours",    "yourself", "yourselves",
    };
    return kWords;
}

} // namespace lucene
