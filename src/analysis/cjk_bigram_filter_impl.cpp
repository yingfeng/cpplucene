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

module lucene_core:cjk_bigram_filter.impl;

import :cjk_bigram_filter;

import :attributes;
import :string_helper;
import :token_stream;
import :types;

import std;

namespace lucene {

namespace {

enum class CJKScript : int8_t { kOther = -1, kHan = 0, kHiragana = 1, kKatakana = 2, kHangul = 3 };

[[nodiscard]] CJKScript script_of(char32_t value) noexcept {
    // Han: the main block, the extension A block, the compatibility block and the
    // supplementary planes.
    if ((value >= 0x4E00U && value <= 0x9FFFU) || (value >= 0x3400U && value <= 0x4DBFU) ||
        (value >= 0xF900U && value <= 0xFAFFU) || (value >= 0x20000U && value <= 0x2FA1FU)) {
        return CJKScript::kHan;
    }
    if ((value >= 0x3041U && value <= 0x3096U) || (value >= 0x309DU && value <= 0x309FU)) {
        return CJKScript::kHiragana;
    }
    if ((value >= 0x30A1U && value <= 0x30FAU) || (value >= 0x30FDU && value <= 0x30FFU) ||
        (value >= 0x31F0U && value <= 0x31FFU) || (value >= 0xFF66U && value <= 0xFF9FU)) {
        return CJKScript::kKatakana;
    }
    if ((value >= 0x1100U && value <= 0x11FFU) || (value >= 0x3130U && value <= 0x318FU) ||
        (value >= 0xA960U && value <= 0xA97FU) || (value >= 0xAC00U && value <= 0xD7FFU)) {
        return CJKScript::kHangul;
    }
    return CJKScript::kOther;
}

/// The script shared by every code point of `term`, or `kOther` when the term is
/// empty, mixed, or not CJK at all.
[[nodiscard]] CJKScript uniform_script_of(std::string_view term) {
    auto code_points = StringHelper::utf8_to_code_points(term);
    if (!code_points.has_value() || code_points.value().empty()) {
        return CJKScript::kOther;
    }
    const CJKScript first = script_of(static_cast<char32_t>(code_points.value().front()));
    if (first == CJKScript::kOther) {
        return CJKScript::kOther;
    }
    for (const int32_t code_point : code_points.value()) {
        if (script_of(static_cast<char32_t>(code_point)) != first) {
            return CJKScript::kOther;
        }
    }
    return first;
}

} // namespace

CJKBigramFilter::CJKBigramFilter(std::unique_ptr<TokenStream> input) : TokenFilter(std::move(input)) {
    attributes().add_attribute<CharTermAttribute>();
    attributes().add_attribute<PositionIncrementAttribute>();
}

VoidResult CJKBigramFilter::reset() {
    // The analyzer reuses one filter chain for every field and document, so the
    // buffered bigrams and the end-of-stream flag must not survive a reset.
    output_.clear();
    run_.clear();
    script_ = -1;
    exhausted_ = false;
    return input_->reset();
}

void CJKBigramFilter::fold_into_run(PendingToken token) {
    if (run_.empty()) {
        run_.push_back(std::move(token));
        return;
    }
    run_.push_back(std::move(token));
}

void CJKBigramFilter::flush_run() {
    if (run_.empty()) {
        script_ = -1;
        return;
    }
    if (run_.size() == 1) {
        // A single character of a run stays a unigram.
        output_.push_back(std::move(run_.front()));
    } else {
        for (size_t i = 0; i + 1 < run_.size(); ++i) {
            PendingToken bigram;
            bigram.term = run_[i].term + run_[i + 1].term;
            bigram.position_increment = i == 0 ? run_[0].position_increment : 1;
            output_.push_back(std::move(bigram));
        }
    }
    run_.clear();
    script_ = -1;
}

Result<bool> CJKBigramFilter::increment_token() {
    auto *term = attributes().get_attribute<CharTermAttribute>();
    auto *position_increment = attributes().get_attribute<PositionIncrementAttribute>();

    while (output_.empty()) {
        if (exhausted_) {
            return false;
        }

        auto more = input_->increment_token();
        if (!more.has_value()) {
            return std::unexpected(std::move(more.error()));
        }
        if (!more.value()) {
            flush_run();
            exhausted_ = true;
            if (output_.empty()) {
                return false;
            }
            break;
        }

        PendingToken token;
        token.term = std::string(term->text());
        token.position_increment = position_increment->position_increment();

        const CJKScript script = uniform_script_of(token.term);
        if (script == CJKScript::kOther) {
            flush_run();
            output_.push_back(std::move(token));
            continue;
        }
        if (static_cast<int8_t>(script) != script_) {
            flush_run();
            script_ = static_cast<int8_t>(script);
        }

        // The reference implementation receives one token per CJK character from
        // its tokenizer.  When a tokenizer here hands over a whole run instead
        // (Hangul does, being an `ALetter` run under UAX#29), split it so the
        // bigrams still overlap character by character.
        auto code_points = StringHelper::utf8_to_code_points(token.term);
        if (!code_points.has_value() || code_points.value().size() <= 1) {
            fold_into_run(std::move(token));
            continue;
        }
        std::vector<PendingToken> characters;
        bool split_ok = true;
        for (const int32_t code_point : code_points.value()) {
            PendingToken character;
            if (!StringHelper::utf8_append(character.term, code_point).has_value()) {
                split_ok = false;
                break;
            }
            character.position_increment = 1;
            characters.push_back(std::move(character));
        }
        if (!split_ok) {
            fold_into_run(std::move(token));
            continue;
        }
        for (PendingToken &character : characters) {
            run_.push_back(std::move(character));
        }
    }

    PendingToken next = std::move(output_.front());
    output_.pop_front();
    term->set_text(next.term);
    position_increment->set_position_increment(next.position_increment);
    return true;
}

} // namespace lucene
