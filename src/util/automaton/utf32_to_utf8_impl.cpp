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

module lucene_core:utf32_to_utf8.impl;

import :utf32_to_utf8;

import :automaton;
import :debug;
import :error;
import :transition;
import :types;

import std;

namespace lucene {

void UTF8Sequence::set(int32_t code) {
    if (code < 0x80) {
        // 0xxxxxxx
        bytes_[0].value = static_cast<uint8_t>(code);
        bytes_[0].bits = 7;
        len = 1;
    } else if (code < 0x800) {
        // 110yyyxx 10xxxxxx
        bytes_[0].value = static_cast<uint8_t>((0b110 << 5) | (code >> 6));
        bytes_[0].bits = 5;
        set_rest(code, 1);
        len = 2;
    } else if (code < 0x10000) {
        // 1110yyyy 10yyyyxx 10xxxxxx
        bytes_[0].value = static_cast<uint8_t>((0b1110 << 4) | (code >> 12));
        bytes_[0].bits = 4;
        set_rest(code, 2);
        len = 3;
    } else {
        // 11110zzz 10zzyyyy 10yyyyxx 10xxxxxx
        bytes_[0].value = static_cast<uint8_t>((0b11110 << 3) | (code >> 18));
        bytes_[0].bits = 3;
        set_rest(code, 3);
        len = 4;
    }
}

void UTF8Sequence::set_first_byte(int32_t code) {
    if (code < 0x80) {
        // 0xxxxxxx
        bytes_[0].value = static_cast<uint8_t>(code);
        len = 1;
    } else if (code < 0x800) {
        // 110yyyxx
        bytes_[0].value = static_cast<uint8_t>((0b110 << 5) | (code >> 6));
        len = 2;
    } else if (code < 0x10000) {
        // 1110yyyy
        bytes_[0].value = static_cast<uint8_t>((0b1110 << 4) | (code >> 12));
        len = 3;
    } else {
        // 11110zzz
        bytes_[0].value = static_cast<uint8_t>((0b11110 << 3) | (code >> 18));
        len = 4;
    }
}

void UTF8Sequence::set_rest(int32_t code, size_t num_bytes) {
    for (size_t i = 0; i < num_bytes; ++i) {
        const size_t idx = num_bytes - i;
        bytes_[idx].value = static_cast<uint8_t>(0b1000'0000 | (code & UTF32ToUTF8::MASKS[6]));
        bytes_[idx].bits = 6;
        code >>= 6;
    }
}

namespace {

/// The conversion state of one [`UTF32ToUTF8::convert`] run.
class Converter {
public:
    explicit Converter(AutomatonBuilder &utf8) : utf8_(utf8) {}

    /// Builds the necessary UTF-8 edges between the code points
    /// `[start_code_point, end_code_point]`.
    VoidResult convert_one_edge(int32_t start, int32_t end, int32_t start_code_point, int32_t end_code_point) {
        start_utf8_.set(start_code_point);
        end_utf8_.set(end_code_point);
        return build(start, end, 0);
    }

private:
    VoidResult build(int32_t start, int32_t end, size_t upto) {
        if (start_utf8_.byte_at(upto) == end_utf8_.byte_at(upto)) {
            // Degenerate: lead with the same byte.
            if (upto == start_utf8_.len - 1 && upto == end_utf8_.len - 1) {
                // Super degenerate: a single edge, one UTF-8 byte.
                return utf8_.add_transition(start, end, start_utf8_.byte_at(upto), end_utf8_.byte_at(upto));
            }
            debug_check(start_utf8_.len > upto + 1, "the start sequence is longer");
            debug_check(end_utf8_.len > upto + 1, "the end sequence is longer");

            const int32_t n = utf8_.create_state();
            if (auto added = utf8_.add_transition_label(start, n, start_utf8_.byte_at(upto)); !added) {
                return added;
            }
            return build(n, end, upto + 1);
        }

        if (start_utf8_.len == end_utf8_.len) {
            if (upto == start_utf8_.len - 1) {
                return utf8_.add_transition(start, end, start_utf8_.byte_at(upto), end_utf8_.byte_at(upto));
            }
            if (auto started = this->start(start, end, upto, false); !started) {
                return started;
            }
            if (end_utf8_.byte_at(upto) - start_utf8_.byte_at(upto) > 1) {
                if (auto all_done = all(start, end, start_utf8_.byte_at(upto) + 1, end_utf8_.byte_at(upto) - 1,
                                        start_utf8_.len - upto - 1);
                    !all_done) {
                    return all_done;
                }
            }
            return this->end(start, end, upto, false);
        }

        if (auto started = this->start(start, end, upto, true); !started) {
            return started;
        }

        // Possibly middle, spanning multiple byte counts.
        size_t byte_count = 1 + start_utf8_.len - upto;
        const size_t limit = end_utf8_.len - upto;
        while (byte_count < limit) {
            tmp_utf8a_.set_first_byte(UTF32ToUTF8::START_CODES[byte_count - 1]);
            tmp_utf8b_.set_first_byte(UTF32ToUTF8::END_CODES[byte_count - 1]);
            if (auto all_done = all(start, end, tmp_utf8a_.byte_at(0), tmp_utf8b_.byte_at(0), tmp_utf8a_.len - 1);
                !all_done) {
                return all_done;
            }
            byte_count += 1;
        }

        return this->end(start, end, upto, true);
    }

    VoidResult start(int32_t from, int32_t to, size_t upto, bool do_all) {
        if (upto == start_utf8_.len - 1) {
            // Done recursing.
            const int32_t b = start_utf8_.byte_at(upto);
            const auto mask = static_cast<int32_t>(UTF32ToUTF8::MASKS[start_utf8_.num_bits(upto)]);
            return utf8_.add_transition(from, to, b, b | mask);
        }
        const int32_t n = utf8_.create_state();
        if (auto added = utf8_.add_transition_label(from, n, start_utf8_.byte_at(upto)); !added) {
            return added;
        }
        if (auto started = start(n, to, upto + 1, true); !started) {
            return started;
        }

        const int32_t start_byte = start_utf8_.byte_at(upto);
        const int32_t end_code = start_byte | static_cast<int32_t>(UTF32ToUTF8::MASKS[start_utf8_.num_bits(upto)]);
        if (do_all && start_byte != end_code) {
            return all(from, to, start_byte + 1, end_code, start_utf8_.len - upto - 1);
        }
        return {};
    }

    VoidResult end(int32_t from, int32_t to, size_t upto, bool do_all) {
        if (upto == end_utf8_.len - 1) {
            // Done recursing.
            const int32_t b = end_utf8_.byte_at(upto);
            const auto mask = static_cast<int32_t>(UTF32ToUTF8::MASKS[end_utf8_.num_bits(upto)]);
            return utf8_.add_transition(from, to, b & ~mask, b);
        }

        // GH-ISSUE#12472: UTF-8 special cases for the first byte of 2, 3 and 4
        // byte characters, which are not simply the masked first byte.
        int32_t start_code = 0;
        if (end_utf8_.len == 2 && upto == 0) {
            // The first length=2 character is C2 80.
            start_code = 0xC2;
        } else if (end_utf8_.len == 3 && upto == 1 && end_utf8_.byte_at(0) == 0xE0) {
            // The first length=3 character is E0 A0 80.
            start_code = 0xA0;
        } else if (end_utf8_.len == 4 && upto == 1 && end_utf8_.byte_at(0) == 0xF0) {
            // The first length=4 character is F0 90 80 80.
            start_code = 0x90;
        } else {
            const int32_t b = end_utf8_.byte_at(upto);
            const auto mask = static_cast<int32_t>(UTF32ToUTF8::MASKS[end_utf8_.num_bits(upto)]);
            start_code = b & ~mask;
        }

        const int32_t end_byte = end_utf8_.byte_at(upto);
        if (do_all && end_byte != start_code) {
            if (auto all_done = all(from, to, start_code, end_byte - 1, end_utf8_.len - upto - 1); !all_done) {
                return all_done;
            }
        }

        const int32_t n = utf8_.create_state();
        if (auto added = utf8_.add_transition_label(from, n, end_byte); !added) {
            return added;
        }
        return end(n, to, upto + 1, true);
    }

    VoidResult all(int32_t from, int32_t to, int32_t start_code, int32_t end_code, size_t left) {
        if (left == 0) {
            return utf8_.add_transition(from, to, start_code, end_code);
        }
        int32_t last_n = utf8_.create_state();
        if (auto added = utf8_.add_transition(from, last_n, start_code, end_code); !added) {
            return added;
        }
        while (left > 1) {
            const int32_t n = utf8_.create_state();
            // Continuation byte range.
            if (auto added = utf8_.add_transition(last_n, n, 128, 191); !added) {
                return added;
            }
            left -= 1;
            last_n = n;
        }
        return utf8_.add_transition(last_n, to, 128, 191);
    }

    AutomatonBuilder &utf8_;
    UTF8Sequence start_utf8_{};
    UTF8Sequence end_utf8_{};
    UTF8Sequence tmp_utf8a_{};
    UTF8Sequence tmp_utf8b_{};
};

} // namespace

Result<std::optional<Automaton>> UTF32ToUTF8::convert(const Automaton &utf32) {
    if (utf32.get_num_states() == 0) {
        return std::optional<Automaton>{};
    }

    std::vector<int32_t> map(static_cast<size_t>(utf32.get_num_states()), -1);
    std::vector<int32_t> pending;

    const int32_t utf32_state = 0;
    pending.push_back(utf32_state);

    AutomatonBuilder utf8;
    Converter converter(utf8);

    const int32_t utf8_state = utf8.create_state();
    utf8.set_accept(utf8_state, utf32.is_accept(utf32_state));
    map[0] = utf8_state;

    Transition scratch;
    while (!pending.empty()) {
        const int32_t current_utf32 = pending.back();
        pending.pop_back();
        const int32_t current_utf8 = map[static_cast<size_t>(current_utf32)];
        debug_check(current_utf8 != -1, "a pending state was mapped");

        const int32_t num_transitions = utf32.get_num_transitions_with_state(current_utf32);
        (void)utf32.init_transition(current_utf32, scratch);

        for (int32_t i = 0; i < num_transitions; ++i) {
            if (auto next = utf32.get_next_transition(scratch); !next) {
                return err(std::move(next.error()));
            }

            const int32_t dest_utf32 = scratch.dest;
            const auto dest_index = static_cast<size_t>(dest_utf32);
            int32_t dest_utf8 = map[dest_index];
            if (dest_utf8 == -1) {
                dest_utf8 = utf8.create_state();
                utf8.set_accept(dest_utf8, utf32.is_accept(dest_utf32));
                map[dest_index] = dest_utf8;
                pending.push_back(dest_utf32);
            }

            if (auto built = converter.convert_one_edge(current_utf8, dest_utf8, scratch.min, scratch.max); !built) {
                return err(std::move(built.error()));
            }
        }
    }

    auto finished = utf8.finish();
    if (!finished) {
        return err(std::move(finished.error()));
    }
    return std::optional<Automaton>(std::move(finished.value()));
}

} // namespace lucene
