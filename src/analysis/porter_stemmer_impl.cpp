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

module lucene_core:porter_stemmer.impl;

import :porter_stemmer;

import :string_helper;
import :types;

import std;

namespace lucene {

bool PorterStemmer::cons(int32_t index) const {
    switch (buffer_[static_cast<size_t>(index)]) {
        case U'a':
        case U'e':
        case U'i':
        case U'o':
        case U'u':
            return false;
        case U'y':
            return index == k0_ ? true : !cons(index - 1);
        default:
            return true;
    }
}

int32_t PorterStemmer::measure() const {
    int32_t n = 0;
    int32_t i = k0_;
    while (true) {
        if (i > j_) {
            return n;
        }
        if (!cons(i)) {
            break;
        }
        ++i;
    }
    ++i;
    while (true) {
        while (true) {
            if (i > j_) {
                return n;
            }
            if (cons(i)) {
                break;
            }
            ++i;
        }
        ++i;
        ++n;
        while (true) {
            if (i > j_) {
                return n;
            }
            if (!cons(i)) {
                break;
            }
            ++i;
        }
        ++i;
    }
}

bool PorterStemmer::vowel_in_stem() const {
    for (int32_t i = k0_; i <= j_; ++i) {
        if (!cons(i)) {
            return true;
        }
    }
    return false;
}

bool PorterStemmer::double_consonant(int32_t index) const {
    if (index < k0_ + 1) {
        return false;
    }
    if (buffer_[static_cast<size_t>(index)] != buffer_[static_cast<size_t>(index - 1)]) {
        return false;
    }
    return cons(index);
}

bool PorterStemmer::consonant_vowel_consonant(int32_t index) const {
    if (index < k0_ + 2 || !cons(index) || cons(index - 1) || !cons(index - 2)) {
        return false;
    }
    const char32_t ch = buffer_[static_cast<size_t>(index)];
    return ch != U'w' && ch != U'x' && ch != U'y';
}

bool PorterStemmer::ends(std::string_view suffix) {
    const int32_t length = static_cast<int32_t>(suffix.size());
    const int32_t offset = k_ - length + 1;
    if (offset < k0_) {
        return false;
    }
    for (int32_t i = 0; i < length; ++i) {
        if (buffer_[static_cast<size_t>(offset + i)] != static_cast<char32_t>(suffix[static_cast<size_t>(i)])) {
            return false;
        }
    }
    j_ = k_ - length;
    return true;
}

void PorterStemmer::set_to(std::string_view suffix) {
    const int32_t length = static_cast<int32_t>(suffix.size());
    const int32_t offset = j_ + 1;
    for (int32_t i = 0; i < length; ++i) {
        buffer_[static_cast<size_t>(offset + i)] = static_cast<char32_t>(suffix[static_cast<size_t>(i)]);
    }
    k_ = j_ + length;
    dirty_ = true;
}

void PorterStemmer::replace_if_measure_positive(std::string_view suffix) {
    if (measure() > 0) {
        set_to(suffix);
    }
}

void PorterStemmer::step1() {
    if (buffer_[static_cast<size_t>(k_)] == U's') {
        if (ends("sses")) {
            k_ -= 2;
        } else if (ends("ies")) {
            set_to("i");
        } else if (buffer_[static_cast<size_t>(k_ - 1)] != U's') {
            --k_;
        }
    }
    if (ends("eed")) {
        if (measure() > 0) {
            --k_;
        }
    } else if ((ends("ed") || ends("ing")) && vowel_in_stem()) {
        k_ = j_;
        if (ends("at")) {
            set_to("ate");
        } else if (ends("bl")) {
            set_to("ble");
        } else if (ends("iz")) {
            set_to("ize");
        } else if (double_consonant(k_)) {
            const char32_t ch = buffer_[static_cast<size_t>(k_)];
            --k_;
            if (ch == U'l' || ch == U's' || ch == U'z') {
                ++k_;
            }
        } else if (measure() == 1 && consonant_vowel_consonant(k_)) {
            set_to("e");
        }
    }
}

void PorterStemmer::step2() {
    if (ends("y") && vowel_in_stem()) {
        buffer_[static_cast<size_t>(k_)] = U'i';
        dirty_ = true;
    }
}

void PorterStemmer::step3() {
    if (k_ == k0_) {
        return; // Bug 1 of the published algorithm.
    }
    switch (buffer_[static_cast<size_t>(k_ - 1)]) {
        case U'a':
            if (ends("ational")) {
                replace_if_measure_positive("ate");
                break;
            }
            if (ends("tional")) {
                replace_if_measure_positive("tion");
            }
            break;
        case U'c':
            if (ends("enci")) {
                replace_if_measure_positive("ence");
                break;
            }
            if (ends("anci")) {
                replace_if_measure_positive("ance");
            }
            break;
        case U'e':
            if (ends("izer")) {
                replace_if_measure_positive("ize");
            }
            break;
        case U'l':
            if (ends("bli")) {
                replace_if_measure_positive("ble");
                break;
            }
            if (ends("alli")) {
                replace_if_measure_positive("al");
                break;
            }
            if (ends("entli")) {
                replace_if_measure_positive("ent");
                break;
            }
            if (ends("eli")) {
                replace_if_measure_positive("e");
                break;
            }
            if (ends("ousli")) {
                replace_if_measure_positive("ous");
            }
            break;
        case U'o':
            if (ends("ization")) {
                replace_if_measure_positive("ize");
                break;
            }
            if (ends("ation")) {
                replace_if_measure_positive("ate");
                break;
            }
            if (ends("ator")) {
                replace_if_measure_positive("ate");
            }
            break;
        case U's':
            if (ends("alism")) {
                replace_if_measure_positive("al");
                break;
            }
            if (ends("iveness")) {
                replace_if_measure_positive("ive");
                break;
            }
            if (ends("fulness")) {
                replace_if_measure_positive("ful");
                break;
            }
            if (ends("ousness")) {
                replace_if_measure_positive("ous");
            }
            break;
        case U't':
            if (ends("aliti")) {
                replace_if_measure_positive("al");
                break;
            }
            if (ends("iviti")) {
                replace_if_measure_positive("ive");
                break;
            }
            if (ends("biliti")) {
                replace_if_measure_positive("ble");
            }
            break;
        case U'g':
            if (ends("logi")) {
                replace_if_measure_positive("log");
            }
            break;
        default:
            break;
    }
}

void PorterStemmer::step4() {
    switch (buffer_[static_cast<size_t>(k_)]) {
        case U'e':
            if (ends("icate")) {
                replace_if_measure_positive("ic");
                break;
            }
            if (ends("ative")) {
                replace_if_measure_positive("");
                break;
            }
            if (ends("alize")) {
                replace_if_measure_positive("al");
            }
            break;
        case U'i':
            if (ends("iciti")) {
                replace_if_measure_positive("ic");
            }
            break;
        case U'l':
            if (ends("ical")) {
                replace_if_measure_positive("ic");
                break;
            }
            if (ends("ful")) {
                replace_if_measure_positive("");
            }
            break;
        case U's':
            if (ends("ness")) {
                replace_if_measure_positive("");
            }
            break;
        default:
            break;
    }
}

void PorterStemmer::step5() {
    if (k_ == k0_) {
        return; // Bug 1 of the published algorithm.
    }
    switch (buffer_[static_cast<size_t>(k_ - 1)]) {
        case U'a':
            if (ends("al")) {
                break;
            }
            return;
        case U'c':
            if (ends("ance")) {
                break;
            }
            if (ends("ence")) {
                break;
            }
            return;
        case U'e':
            if (ends("er")) {
                break;
            }
            return;
        case U'i':
            if (ends("ic")) {
                break;
            }
            return;
        case U'l':
            if (ends("able")) {
                break;
            }
            if (ends("ible")) {
                break;
            }
            return;
        case U'n':
            if (ends("ant")) {
                break;
            }
            if (ends("ement")) {
                break;
            }
            if (ends("ment")) {
                break;
            }
            // `element` is not stripped before the m.
            if (ends("ent")) {
                break;
            }
            return;
        case U'o':
            if (ends("ion") && j_ >= 0 && (buffer_[static_cast<size_t>(j_)] == U's' ||
                                          buffer_[static_cast<size_t>(j_)] == U't')) {
                break;
            }
            // Bug 2 of the published algorithm: `j >= 0` matters for short words.
            if (ends("ou")) {
                break;
            }
            return;
        case U's':
            if (ends("ism")) {
                break;
            }
            return;
        case U't':
            if (ends("ate")) {
                break;
            }
            if (ends("iti")) {
                break;
            }
            return;
        case U'u':
            if (ends("ous")) {
                break;
            }
            return;
        case U'v':
            if (ends("ive")) {
                break;
            }
            return;
        case U'z':
            if (ends("ize")) {
                break;
            }
            return;
        default:
            return;
    }
    if (measure() > 1) {
        k_ = j_;
    }
}

void PorterStemmer::step6() {
    j_ = k_;
    if (buffer_[static_cast<size_t>(k_)] == U'e') {
        const int32_t a = measure();
        if (a > 1 || (a == 1 && !consonant_vowel_consonant(k_ - 1))) {
            --k_;
        }
    }
    if (buffer_[static_cast<size_t>(k_)] == U'l' && double_consonant(k_) && measure() > 1) {
        --k_;
    }
}

bool PorterStemmer::run(int32_t k0) {
    k_ = length_ - 1;
    k0_ = k0;
    if (k_ > k0_ + 1) {
        step1();
        step2();
        step3();
        step4();
        step5();
        step6();
    }
    if (length_ != k_ + 1) {
        dirty_ = true;
    }
    length_ = k_ + 1;
    return dirty_;
}

std::string PorterStemmer::stem(std::string_view word) {
    auto code_points = StringHelper::utf8_to_code_points(word);
    if (!code_points.has_value()) {
        return std::string(word);
    }
    buffer_.assign(code_points.value().begin(), code_points.value().end());
    length_ = static_cast<int32_t>(buffer_.size());
    // `set_to` can grow the word (a suffix is replaced by a longer one).
    buffer_.resize(buffer_.size() + 8, U'\0');
    dirty_ = false;
    j_ = 0;
    k_ = 0;
    k0_ = 0;

    if (!run(0)) {
        return std::string(word);
    }

    std::string stemmed;
    stemmed.reserve(word.size() + 4);
    for (int32_t i = 0; i < length_; ++i) {
        if (!StringHelper::utf8_append(stemmed, static_cast<int32_t>(buffer_[static_cast<size_t>(i)])).has_value()) {
            return std::string(word);
        }
    }
    return stemmed;
}

} // namespace lucene
