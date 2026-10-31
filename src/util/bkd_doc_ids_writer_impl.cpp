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

module;

#if defined(__SSSE3__)
#include <immintrin.h>
#endif

module lucene_core:bkd_doc_ids_writer.impl;

import :bkd_doc_ids_writer;

import :data_input;
import :data_output;
import :debug;
import :error;
import :types;

import std;

namespace lucene {

namespace {

/// Number of 64-bit words needed to hold `num_bits` bits.
[[nodiscard]] size_t bits_to_words(uint64_t num_bits) { return static_cast<size_t>((num_bits + 63) >> 6); }

/// Serialises `doc_ids` as a bit set, streaming the words out in order.
///
/// The first two vints are the word offset (so a block of high doc ids does not
/// have to start at word 0) and the number of words.
[[nodiscard]] VoidResult write_ids_as_bit_set(std::span<const int32_t> doc_ids, DataOutput &out) {
    const int32_t min = doc_ids.front();
    const int32_t max = doc_ids.back();

    const int32_t offset_words = min >> 6;
    const int32_t offset_bits = offset_words << 6;
    const size_t total_word_count = bits_to_words(static_cast<uint64_t>(max) - offset_bits + 1);

    if (auto written = out.write_vint(offset_words); !written.has_value()) {
        return written;
    }
    if (auto written = out.write_vint(static_cast<int32_t>(total_word_count)); !written.has_value()) {
        return written;
    }

    int64_t current_word = 0;
    size_t current_word_index = 0;
    for (const int32_t doc_id : doc_ids) {
        const size_t index = static_cast<size_t>(doc_id - offset_bits);
        const size_t next_word_index = index >> 6;
        if (current_word_index < next_word_index) {
            // Flush the word that is complete now, then any all-zero words in
            // between, leaving `current_word_index` on `next_word_index`.
            if (auto written = out.write_long(current_word); !written.has_value()) {
                return written;
            }
            current_word = 0;
            ++current_word_index;
            while (current_word_index < next_word_index) {
                if (auto written = out.write_long(0); !written.has_value()) {
                    return written;
                }
                ++current_word_index;
            }
        }
        current_word |= int64_t{1} << (index & 63);
    }
    return out.write_long(current_word);
}

} // namespace

Result<int8_t> BkdDocIdsWriter::write_doc_ids(std::span<const int32_t> doc_ids, DataOutput &out) {
    debug_check(!doc_ids.empty(), "a leaf block always has at least one doc id");

    // Doc ids are sorted when every point in the block carries the same value, or
    // when the segment itself is sorted; that is what makes the run-length and
    // bit set encodings worth trying.
    bool strictly_sorted = true;
    int32_t min = doc_ids.front();
    int32_t max = doc_ids.front();
    for (size_t i = 1; i < doc_ids.size(); ++i) {
        if (doc_ids[i - 1] >= doc_ids[i]) {
            strictly_sorted = false;
        }
        min = std::min(min, doc_ids[i]);
        max = std::max(max, doc_ids[i]);
    }

    const uint64_t span = static_cast<uint64_t>(max) - static_cast<uint64_t>(min) + 1;
    const size_t count = doc_ids.size();

    if (strictly_sorted) {
        if (span == count) {
            if (auto written = out.write_byte(static_cast<uint8_t>(CONTINUOUS_IDS)); !written.has_value()) {
                return err(written.error());
            }
            if (auto written = out.write_vint(doc_ids.front()); !written.has_value()) {
                return err(written.error());
            }
            return CONTINUOUS_IDS;
        }
        if (span <= (count << 4)) {
            // Only worth it when the bit set does not expand by more than 16x.
            // A span equal to `count` is contiguous and already handled above,
            // so the bit set is always strictly larger than the id list.
            debug_check(span > count, "a contiguous block is encoded as CONTINUOUS_IDS");
            if (auto written = out.write_byte(static_cast<uint8_t>(BITSET_IDS)); !written.has_value()) {
                return err(written.error());
            }
            if (auto written = write_ids_as_bit_set(doc_ids, out); !written.has_value()) {
                return err(written.error());
            }
            return BITSET_IDS;
        }
    }

    if (span <= 0xFFFF) {
        if (auto written = out.write_byte(static_cast<uint8_t>(DELTA_BPV_16)); !written.has_value()) {
            return err(written.error());
        }
        if (auto written = out.write_vint(min); !written.has_value()) {
            return err(written.error());
        }
        // Two 16-bit deltas per int: the first half of the block in the high
        // half-word, the second half in the low one.
        std::vector<int32_t> deltas(count, 0);
        for (size_t i = 0; i < count; ++i) {
            deltas[i] = doc_ids[i] - min;
        }
        const size_t half = count >> 1;
        for (size_t i = 0; i < half; ++i) {
            deltas[i] = static_cast<int32_t>((static_cast<uint32_t>(deltas[half + i]) & 0xFFFFU) |
                                             (static_cast<uint32_t>(deltas[i]) << 16));
        }
        for (size_t i = 0; i < half; ++i) {
            if (auto written = out.write_int(deltas[i]); !written.has_value()) {
                return err(written.error());
            }
        }
        if ((count & 1) != 0) {
            if (auto written = out.write_short(static_cast<int16_t>(deltas[count - 1])); !written.has_value()) {
                return err(written.error());
            }
        }
        return DELTA_BPV_16;
    }

    if (max <= 0xFFFFFF) {
        if (auto written = out.write_byte(static_cast<uint8_t>(BPV_24)); !written.has_value()) {
            return err(written.error());
        }
        size_t i = 0;
        // Eight DocIDs are packed into three longs (24 bytes) whose byte order is
        // the same on every platform.  The scalar version below needs a handful of
        // shifts and ors per output byte; the shuffle version moves the low three
        // bytes of each DocID into place with one instruction per output vector.
#if defined(__SSSE3__)
        for (; i + 7 < count; i += 8) {
            const __m128i first = _mm_loadu_si128(reinterpret_cast<const __m128i *>(doc_ids.data() + i));
            const __m128i second =
                _mm_loadu_si128(reinterpret_cast<const __m128i *>(doc_ids.data() + i + 4));
            // `l1`'s eight bytes, then `l2`'s low four.
            const __m128i first_two = _mm_or_si128(
                _mm_shuffle_epi8(first, _mm_setr_epi8(9, 10, 4, 5, 6, 0, 1, 2, -1, -1, -1, -1, 12, 13,
                                                      14, 8)),
                _mm_shuffle_epi8(second, _mm_setr_epi8(-1, -1, -1, -1, -1, -1, -1, -1, 6, 0, 1, 2, -1,
                                                       -1, -1, -1)));
            // `l3`'s eight bytes.
            const __m128i last =
                _mm_shuffle_epi8(second, _mm_setr_epi8(12, 13, 14, 8, 9, 10, 4, 5, -1, -1, -1, -1, -1,
                                                       -1, -1, -1));
            int64_t head[2];
            int64_t tail[2];
            _mm_storeu_si128(reinterpret_cast<__m128i *>(head), first_two);
            _mm_storeu_si128(reinterpret_cast<__m128i *>(tail), last);
            if (auto written = out.write_long(head[0]); !written.has_value()) {
                return err(written.error());
            }
            if (auto written = out.write_long(head[1]); !written.has_value()) {
                return err(written.error());
            }
            if (auto written = out.write_long(tail[0]); !written.has_value()) {
                return err(written.error());
            }
        }
#endif
        for (; i + 7 < count; i += 8) {
            const int64_t d1 = doc_ids[i];
            const int64_t d2 = doc_ids[i + 1];
            const int64_t d3 = doc_ids[i + 2];
            const int64_t d4 = doc_ids[i + 3];
            const int64_t d5 = doc_ids[i + 4];
            const int64_t d6 = doc_ids[i + 5];
            const int64_t d7 = doc_ids[i + 6];
            const int64_t d8 = doc_ids[i + 7];
            const int64_t l1 = ((d1 & 0xFFFFFF) << 40) | ((d2 & 0xFFFFFF) << 16) |
                               ((static_cast<uint32_t>(doc_ids[i + 2]) >> 8) & 0xFFFF);
            const int64_t l2 = ((d3 & 0xFF) << 56) | ((d4 & 0xFFFFFF) << 32) | ((d5 & 0xFFFFFF) << 8) |
                               ((static_cast<uint32_t>(doc_ids[i + 5]) >> 16) & 0xFF);
            const int64_t l3 = ((d6 & 0xFFFF) << 48) | ((d7 & 0xFFFFFF) << 24) | (d8 & 0xFFFFFF);
            if (auto written = out.write_long(l1); !written.has_value()) {
                return err(written.error());
            }
            if (auto written = out.write_long(l2); !written.has_value()) {
                return err(written.error());
            }
            if (auto written = out.write_long(l3); !written.has_value()) {
                return err(written.error());
            }
        }
        for (; i < count; ++i) {
            if (auto written = out.write_short(static_cast<int16_t>(static_cast<uint32_t>(doc_ids[i]) >> 8));
                !written.has_value()) {
                return err(written.error());
            }
            if (auto written = out.write_byte(static_cast<uint8_t>(doc_ids[i] & 0xFF)); !written.has_value()) {
                return err(written.error());
            }
        }
        return BPV_24;
    }

    if (auto written = out.write_byte(static_cast<uint8_t>(BPV_32)); !written.has_value()) {
        return err(written.error());
    }
    for (const int32_t doc_id : doc_ids) {
        if (auto written = out.write_int(doc_id); !written.has_value()) {
            return err(written.error());
        }
    }
    return BPV_32;
}

VoidResult BkdDocIdsWriter::read_ints(DataInput &input, size_t count, std::span<int32_t> doc_ids) {
    debug_check(doc_ids.size() >= count, "the destination must hold `count` doc ids");

    auto tag = input.read_byte();
    if (!tag.has_value()) {
        return err(tag.error());
    }

    switch (static_cast<int8_t>(tag.value())) {
        case CONTINUOUS_IDS: {
            auto start = input.read_vint();
            if (!start.has_value()) {
                return err(start.error());
            }
            for (size_t i = 0; i < count; ++i) {
                doc_ids[i] = start.value() + static_cast<int32_t>(i);
            }
            return {};
        }

        case BITSET_IDS: {
            auto offset_words = input.read_vint();
            if (!offset_words.has_value()) {
                return err(offset_words.error());
            }
            auto word_count = input.read_vint();
            if (!word_count.has_value()) {
                return err(word_count.error());
            }
            if (word_count.value() < 0) {
                return err(LuceneError::corrupt_index("the BKD bit set word count is negative"));
            }
            std::vector<int64_t> words(static_cast<size_t>(word_count.value()), 0);
            if (!words.empty()) {
                if (auto read = input.read_longs(words, 0, words.size()); !read.has_value()) {
                    return err(read.error());
                }
            }

            const int64_t doc_base = static_cast<int64_t>(offset_words.value()) << 6;
            size_t position = 0;
            for (size_t word_index = 0; word_index < words.size(); ++word_index) {
                uint64_t bits = static_cast<uint64_t>(words[word_index]);
                if (bits == ~uint64_t{0} && position + 64 <= count) {
                    // A full word means 64 consecutive ids: fill them without
                    // walking the bits one by one.
                    const int32_t base =
                        static_cast<int32_t>(doc_base + static_cast<int64_t>(word_index << 6));
                    for (size_t bit = 0; bit < 64; ++bit) {
                        doc_ids[position + bit] = base + static_cast<int32_t>(bit);
                    }
                    position += 64;
                    continue;
                }
                while (bits != 0) {
                    const size_t bit = static_cast<size_t>(std::countr_zero(bits));
                    if (position >= count) {
                        return err(LuceneError::corrupt_index("the BKD bit set holds more ids than expected"));
                    }
                    doc_ids[position] = static_cast<int32_t>(doc_base + static_cast<int64_t>((word_index << 6) + bit));
                    ++position;
                    bits &= bits - 1;
                }
            }
            if (position != count) {
                return err(LuceneError::corrupt_index("the BKD bit set holds fewer ids than expected"));
            }
            return {};
        }

        case DELTA_BPV_16: {
            auto min = input.read_vint();
            if (!min.has_value()) {
                return err(min.error());
            }
            const size_t half = count >> 1;
            if (half > 0) {
                if (auto read = input.read_ints(doc_ids, 0, half); !read.has_value()) {
                    return err(read.error());
                }
            }
            for (size_t i = 0; i < half; ++i) {
                const int32_t packed = doc_ids[i];
                doc_ids[i] = static_cast<int32_t>(static_cast<uint32_t>(packed) >> 16) + min.value();
                doc_ids[half + i] = (packed & 0xFFFF) + min.value();
            }
            if ((count & 1) != 0) {
                auto tail = input.read_short();
                if (!tail.has_value()) {
                    return err(tail.error());
                }
                doc_ids[count - 1] = static_cast<int32_t>(static_cast<uint16_t>(tail.value())) + min.value();
            }
            return {};
        }

        case BPV_24: {
            size_t i = 0;
            while (i + 7 < count) {
                auto l1 = input.read_long();
                if (!l1.has_value()) {
                    return err(l1.error());
                }
                auto l2 = input.read_long();
                if (!l2.has_value()) {
                    return err(l2.error());
                }
                auto l3 = input.read_long();
                if (!l3.has_value()) {
                    return err(l3.error());
                }
                const uint64_t u1 = static_cast<uint64_t>(l1.value());
                const uint64_t u2 = static_cast<uint64_t>(l2.value());
                const uint64_t u3 = static_cast<uint64_t>(l3.value());
                doc_ids[i] = static_cast<int32_t>(u1 >> 40);
                doc_ids[i + 1] = static_cast<int32_t>((u1 >> 16) & 0xFFFFFF);
                doc_ids[i + 2] = static_cast<int32_t>(((u1 & 0xFFFF) << 8) | ((u2 >> 56) & 0xFF));
                doc_ids[i + 3] = static_cast<int32_t>((u2 >> 32) & 0xFFFFFF);
                doc_ids[i + 4] = static_cast<int32_t>((u2 >> 8) & 0xFFFFFF);
                doc_ids[i + 5] = static_cast<int32_t>(((u2 & 0xFF) << 16) | ((u3 >> 48) & 0xFFFF));
                doc_ids[i + 6] = static_cast<int32_t>((u3 >> 24) & 0xFFFFFF);
                doc_ids[i + 7] = static_cast<int32_t>(u3 & 0xFFFFFF);
                i += 8;
            }
            for (; i < count; ++i) {
                auto high = input.read_short();
                if (!high.has_value()) {
                    return err(high.error());
                }
                auto low = input.read_byte();
                if (!low.has_value()) {
                    return err(low.error());
                }
                doc_ids[i] = (static_cast<int32_t>(static_cast<uint16_t>(high.value())) << 8) |
                             static_cast<int32_t>(low.value());
            }
            return {};
        }

        case BPV_32:
            return input.read_ints(doc_ids, 0, count);

        case LEGACY_DELTA_VINT: {
            int32_t doc = 0;
            for (size_t i = 0; i < count; ++i) {
                auto delta = input.read_vint();
                if (!delta.has_value()) {
                    return err(delta.error());
                }
                doc += delta.value();
                doc_ids[i] = doc;
            }
            return {};
        }

        default:
            return err(LuceneError::corrupt_index("unsupported BKD doc id encoding " +
                                                 std::to_string(tag.value())));
    }
}

} // namespace lucene
