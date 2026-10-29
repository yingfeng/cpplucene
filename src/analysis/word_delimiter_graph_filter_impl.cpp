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

module lucene_core:word_delimiter_graph_filter.impl;

import :word_delimiter_graph_filter;

import :attribute_source;
import :attributes;
import :char_array_set;
import :token_stream;
import :types;
import :word_delimiter;

import std;

namespace lucene {

namespace {

constexpr uint32_t kKnownFlags = WordDelimiterGraphFilter::kGenerateWordParts |
                                 WordDelimiterGraphFilter::kGenerateNumberParts |
                                 WordDelimiterGraphFilter::kCatenateWords |
                                 WordDelimiterGraphFilter::kCatenateNumbers |
                                 WordDelimiterGraphFilter::kCatenateAll |
                                 WordDelimiterGraphFilter::kPreserveOriginal |
                                 WordDelimiterGraphFilter::kSplitOnCaseChange |
                                 WordDelimiterGraphFilter::kSplitOnNumerics |
                                 WordDelimiterGraphFilter::kStemEnglishPossessive |
                                 WordDelimiterGraphFilter::kIgnoreKeywords;

} // namespace

WordDelimiterGraphFilter::WordDelimiterGraphFilter(std::unique_ptr<TokenStream> input, uint32_t flags,
                                                   const CharArraySet *prot_words, bool adjust_internal_offsets)
    : TokenFilter(std::move(input)),
      flags_(flags),
      prot_words_(prot_words),
      adjust_internal_offsets_(adjust_internal_offsets),
      iterator_(std::string_view{}, (flags & kSplitOnCaseChange) != 0, (flags & kSplitOnNumerics) != 0,
                (flags & kStemEnglishPossessive) != 0) {
    if ((flags_ & ~kKnownFlags) != 0) {
        throw std::invalid_argument("WordDelimiterGraphFilter: unrecognized flag in " + std::to_string(flags_));
    }
    term_ = attributes().add_attribute<CharTermAttribute>();
    keyword_ = attributes().add_attribute<KeywordAttribute>();
    offset_ = attributes().add_attribute<OffsetAttribute>();
    pos_inc_ = attributes().add_attribute<PositionIncrementAttribute>();
    pos_len_ = attributes().add_attribute<PositionLengthAttribute>();
}

VoidResult WordDelimiterGraphFilter::reset() {
    auto result = TokenFilter::reset();
    if (!result.has_value()) {
        return result;
    }
    accum_pos_inc_ = 0;
    has_saved_state_ = false;
    last_start_offset_ = 0;
    concat_.clear();
    concat_all_.clear();
    parts_.clear();
    buffered_pos_ = 0;
    word_pos_ = 0;
    last_concat_count_ = 0;
    return {};
}

void WordDelimiterGraphFilter::save_state() {
    saved_term_.assign(term_->text());
    saved_start_offset_ = static_cast<int32_t>(offset_->start_offset());
    saved_end_offset_ = static_cast<int32_t>(offset_->end_offset());
    saved_state_ = attributes().capture_state();
    has_saved_state_ = true;
}

void WordDelimiterGraphFilter::buffer_part(int32_t start_pos, int32_t end_pos, int32_t start_part, int32_t end_part) {
    parts_.push_back(Part{start_pos, end_pos, start_part, end_part, std::nullopt});
}

void WordDelimiterGraphFilter::buffer_part(std::string term, int32_t start_pos, int32_t end_pos, int32_t start_part,
                                          int32_t end_part) {
    parts_.push_back(Part{start_pos, end_pos, start_part, end_part, std::move(term)});
}

bool WordDelimiterGraphFilter::should_concatenate(int32_t word_type) const {
    return (has_flag(kCatenateWords) && WordDelimiterIterator::is_alpha(word_type)) ||
           (has_flag(kCatenateNumbers) && WordDelimiterIterator::is_digit(word_type));
}

bool WordDelimiterGraphFilter::should_generate_parts(int32_t word_type) const {
    return (has_flag(kGenerateWordParts) && WordDelimiterIterator::is_alpha(word_type)) ||
           (has_flag(kGenerateNumberParts) && WordDelimiterIterator::is_digit(word_type));
}

void WordDelimiterGraphFilter::concatenate(Concatenation &concatenation) {
    if (concatenation.empty()) {
        concatenation.type = iterator_.type();
        concatenation.start_part = iterator_.current();
        concatenation.start_pos = word_pos_;
    }
    const int32_t start = iterator_.current();
    const int32_t end = iterator_.end();
    concatenation.append(std::string_view(saved_term_).substr(static_cast<size_t>(start),
                                                             static_cast<size_t>(end - start)));
    concatenation.end_part = end;
}

void WordDelimiterGraphFilter::flush_concatenation(Concatenation &concat) {
    if (word_pos_ == concat.start_pos) {
        // We are not generating parts, so the position has to advance now.
        ++word_pos_;
    }
    last_concat_count_ = concat.subword_count;
    if (concat.subword_count != 1 || !should_generate_parts(concat.type)) {
        buffer_part(concat.buffer, concat.start_pos, word_pos_, concat.start_part, concat.end_part);
    }
    concat.clear();
}

void WordDelimiterGraphFilter::sort_parts(size_t from) {
    // Smaller start, then longer end: the longer token wins the shared position.
    std::stable_sort(parts_.begin() + static_cast<std::ptrdiff_t>(from), parts_.end(),
                     [](const Part &left, const Part &right) {
                         if (left.start_part != right.start_part) {
                             return left.start_part < right.start_part;
                         }
                         return left.end_part > right.end_part;
                     });
}

void WordDelimiterGraphFilter::buffer_word_parts() {
    save_state();

    // When the offsets do not match the term (an injected synonym, a stemmed form)
    // every part keeps the offsets of the whole token.
    adjusting_offsets_ = adjust_internal_offsets_ &&
                         saved_end_offset_ - saved_start_offset_ == static_cast<int32_t>(saved_term_.size());

    parts_.clear();
    buffered_pos_ = 0;
    last_concat_count_ = 0;
    word_pos_ = 0;

    if (has_flag(kPreserveOriginal)) {
        // The original is always emitted first; its position length is fixed up at
        // the end, once the number of parts is known.
        buffer_part(0, 1, 0, static_cast<int32_t>(saved_term_.size()));
    }

    if (iterator_.is_single_word()) {
        buffer_part(word_pos_, word_pos_ + 1, iterator_.current(), iterator_.end());
        ++word_pos_;
        iterator_.next();
    } else {
        while (iterator_.end() != WordDelimiterIterator::kDone) {
            const int32_t word_type = iterator_.type();

            // Do we already have a queued concatenation of an incompatible type?
            if (!concat_.empty() && (concat_.type & word_type) == 0) {
                flush_concatenation(concat_);
            }

            if (should_concatenate(word_type)) {
                concatenate(concat_);
            }
            if (has_flag(kCatenateAll)) {
                concatenate(concat_all_);
            }
            if (should_generate_parts(word_type)) {
                buffer_part(word_pos_, word_pos_ + 1, iterator_.current(), iterator_.end());
                ++word_pos_;
            }
            iterator_.next();
        }

        if (!concat_.empty()) {
            flush_concatenation(concat_);
        }

        if (!concat_all_.empty()) {
            // Only if the same combination was not already emitted (e.g. "PowerShot"
            // with kCatenateWords).
            if (concat_all_.subword_count > last_concat_count_) {
                if (word_pos_ == concat_all_.start_pos) {
                    // Not generating parts, so the position has to advance now.
                    ++word_pos_;
                }
                buffer_part(concat_all_.buffer, concat_all_.start_pos, word_pos_, concat_all_.start_part,
                            concat_all_.end_part);
            }
            concat_all_.clear();
        }
    }

    if (has_flag(kPreserveOriginal)) {
        if (word_pos_ == 0) {
            // Can happen with strange flag combinations and inputs.
            ++word_pos_;
        }
        parts_[0].end_pos = word_pos_;
    }

    sort_parts(has_flag(kPreserveOriginal) ? 1 : 0);
    word_pos_ = 0;
    buffered_pos_ = 0;
}

Result<bool> WordDelimiterGraphFilter::increment_token() {
    while (true) {
        if (!has_saved_state_) {
            // Process a new input token.
            auto more = input_->increment_token();
            if (!more.has_value()) {
                return std::unexpected(std::move(more.error()));
            }
            if (!more.value()) {
                return false;
            }
            if (has_flag(kIgnoreKeywords) && keyword_->is_keyword()) {
                return true;
            }

            const std::string_view term_text = term_->text();
            const auto term_length = static_cast<int32_t>(term_text.size());
            accum_pos_inc_ += static_cast<int32_t>(pos_inc_->position_increment());

            iterator_.set_text(term_text);
            iterator_.next();

            // No delimiters, or a protected word: return it untouched.
            if ((iterator_.current() == 0 && iterator_.end() == term_length) ||
                (prot_words_ != nullptr && prot_words_->contains_key_str(term_text))) {
                pos_inc_->set_position_increment(static_cast<uint32_t>(accum_pos_inc_));
                accum_pos_inc_ = 0;
                return true;
            }

            // Only delimiters: swallow the token, leaving a hole.
            if (iterator_.end() == WordDelimiterIterator::kDone) {
                if (!has_flag(kPreserveOriginal)) {
                    continue;
                }
                accum_pos_inc_ = 0;
                return true;
            }

            buffer_word_parts();
        }

        if (buffered_pos_ < parts_.size()) {
            attributes().clear_attributes();
            attributes().restore_state(saved_state_);

            const Part &part = parts_[buffered_pos_];
            ++buffered_pos_;

            int32_t start_offset;
            int32_t end_offset;
            if (!adjusting_offsets_) {
                start_offset = saved_start_offset_;
                end_offset = saved_end_offset_;
            } else {
                start_offset = saved_start_offset_ + part.start_part;
                end_offset = saved_start_offset_ + part.end_part;
            }

            // Never let the offsets go backwards.
            start_offset = std::max(start_offset, last_start_offset_);
            end_offset = std::max(end_offset, last_start_offset_);

            offset_->set_offset(static_cast<uint32_t>(start_offset), static_cast<uint32_t>(end_offset));
            last_start_offset_ = start_offset;

            if (part.term.has_value()) {
                term_->set_text(*part.term);
            } else {
                term_->set_text(std::string_view(saved_term_).substr(static_cast<size_t>(part.start_part),
                                                                    static_cast<size_t>(part.end_part - part.start_part)));
            }

            pos_inc_->set_position_increment(
                static_cast<uint32_t>(accum_pos_inc_ + part.start_pos - word_pos_));
            accum_pos_inc_ = 0;
            pos_len_->set_position_length(static_cast<uint32_t>(part.end_pos - part.start_pos));
            word_pos_ = part.start_pos;
            return true;
        }

        // No buffered parts left: move on to the next input word.
        has_saved_state_ = false;
    }
}

std::string WordDelimiterGraphFilter::flags_to_string(uint32_t flags) {
    struct Entry {
        uint32_t flag;
        std::string_view name;
    };
    static constexpr Entry entries[] = {
        {kGenerateWordParts, "GENERATE_WORD_PARTS"},
        {kGenerateNumberParts, "GENERATE_NUMBER_PARTS"},
        {kCatenateWords, "CATENATE_WORDS"},
        {kCatenateNumbers, "CATENATE_NUMBERS"},
        {kCatenateAll, "CATENATE_ALL"},
        {kPreserveOriginal, "PRESERVE_ORIGINAL"},
        {kSplitOnCaseChange, "SPLIT_ON_CASE_CHANGE"},
        {kSplitOnNumerics, "SPLIT_ON_NUMERICS"},
        {kStemEnglishPossessive, "STEM_ENGLISH_POSSESSIVE"},
        {kIgnoreKeywords, "IGNORE_KEYWORDS"},
    };
    std::string result;
    for (const Entry &entry : entries) {
        if ((flags & entry.flag) == 0) {
            continue;
        }
        if (!result.empty()) {
            result += " | ";
        }
        result += entry.name;
    }
    return result;
}

} // namespace lucene
