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

module lucene_core:lz4.impl;

import :lz4;
import :types;
import :error;
import :debug;
import :bit_util;
import :core_helper;
import :data_input;
import :data_output;

import std;

namespace lucene {

namespace {

/// `hash(i, hash_bits)` of the LZ4 reference implementation: a multiplicative
/// hash with the golden-ratio constant (`-1640531535` == `0x9E3779B1`).
inline size_t hash_offset(int32_t i, int32_t hash_bits) noexcept {
    const uint32_t h = static_cast<uint32_t>(i) * 2654435761U;
    return static_cast<size_t>(h >> (32 - hash_bits));
}

inline size_t hash_hc(int32_t i) noexcept {
    return hash_offset(i, LZ4::HASH_LOG_HC);
}

/// Little-endian read of 4 bytes.  LZ4 does not care about endianness as long
/// as encoder and decoder agree, but the format was defined as little endian.
inline int32_t read_int(std::span<const uint8_t> buf, int32_t i) noexcept {
    return BitUtil::get_i32_le(buf, static_cast<size_t>(i));
}

/// Number of equal bytes of `b[o1..limit]` and `b[o2..limit]`.
inline int32_t common_bytes(std::span<const uint8_t> b, int32_t o1, int32_t o2, int32_t limit) noexcept {
    const auto left = b.subspan(static_cast<size_t>(o1), static_cast<size_t>(limit - o1));
    const auto right = b.subspan(static_cast<size_t>(o2), static_cast<size_t>(limit - o2));
    return CoreHelper::mismatch_u8(left, right);
}

/// Copies `len` bytes from `dest[d_off - match_dec..]` to `dest[d_off..]`,
/// honouring LZ4's overlapping-match semantics (a small distance repeats the
/// pattern that was just written).
inline void copy_match(std::span<uint8_t> dest, int32_t d_off, int32_t match_dec, int32_t len) noexcept {
    const size_t source = static_cast<size_t>(d_off - match_dec);
    const size_t target = static_cast<size_t>(d_off);
    if (static_cast<int32_t>(match_dec) >= len) {
        // Disjoint ranges: a plain bulk copy is safe and much faster.
        std::memcpy(dest.data() + target, dest.data() + source, static_cast<size_t>(len));
        return;
    }
    for (int32_t i = 0; i < len; ++i) {
        dest[target + static_cast<size_t>(i)] = dest[source + static_cast<size_t>(i)];
    }
}

VoidResult encode_len(int32_t l, DataOutput &out) {
    while (l >= 0xFF) {
        if (auto result = out.write_byte(0xFF); !result.has_value()) {
            return result;
        }
        l -= 0xFF;
    }
    return out.write_byte(static_cast<uint8_t>(l));
}

VoidResult encode_literals(std::span<const uint8_t> bytes, int32_t token, int32_t anchor, int32_t literal_len,
                           DataOutput &out) {
    if (auto result = out.write_byte(static_cast<uint8_t>(token)); !result.has_value()) {
        return result;
    }
    if (literal_len >= 0x0F) {
        if (auto result = encode_len(literal_len - 0x0F, out); !result.has_value()) {
            return result;
        }
    }
    return out.write_bytes(bytes, static_cast<size_t>(anchor), static_cast<size_t>(literal_len));
}

VoidResult encode_last_literals(std::span<const uint8_t> bytes, int32_t anchor, int32_t literal_len, DataOutput &out) {
    const int32_t token = std::min(literal_len, 0x0F) << 4;
    return encode_literals(bytes, token, anchor, literal_len, out);
}

VoidResult encode_sequence(std::span<const uint8_t> bytes, int32_t anchor, int32_t match_ref, int32_t match_off,
                           int32_t match_len, DataOutput &out) {
    const int32_t literal_len = match_off - anchor;
    debug_check(match_len >= 4, "LZ4 match length must be at least the minimum match");
    const int32_t token = (std::min(literal_len, 0x0F) << 4) | std::min(match_len - 4, 0x0F);
    if (auto result = encode_literals(bytes, token, anchor, literal_len, out); !result.has_value()) {
        return result;
    }

    const int32_t match_dec = match_off - match_ref;
    debug_check(match_dec > 0 && match_dec < (1 << 16), "LZ4 match distance must be within the window");
    if (auto result = out.write_short(static_cast<int16_t>(match_dec)); !result.has_value()) {
        return result;
    }

    if (match_len >= LZ4::MIN_MATCH + 0x0F) {
        return encode_len(match_len - 0x0F - LZ4::MIN_MATCH, out);
    }
    return {};
}

} // namespace

// ---------------- FastCompressionHashTable ----------------

void FastCompressionHashTable::reset(int32_t off, int32_t len) {
    base_ = off;
    end_ = off + len;

    const int32_t bits_per_offset = (len - LZ4::LAST_LITERALS) < (1 << 16) ? 16 : 32;
    const int32_t bits_per_offset_log =
        32 - static_cast<int32_t>(std::countl_zero(static_cast<uint32_t>(bits_per_offset - 1)));
    hash_log_ = LZ4::MEMORY_USAGE + 3 - bits_per_offset_log;

    const bool need_new_table =
        table_.empty() || table_.size() < (size_t{1} << hash_log_) || bits_per_value_ < bits_per_offset;
    if (need_new_table) {
        table_.assign(size_t{1} << hash_log_, 0);
        bits_per_value_ = bits_per_offset;
    }
    // When the table is reused it is deliberately *not* cleared: `get` rejects
    // references that are not below the current offset, which keeps compressing
    // many short inputs cheap.
    last_off_ = off - 1;
}

void FastCompressionHashTable::init_dictionary(int32_t dict_len, std::span<const uint8_t> bytes) {
    for (int32_t i = 0; i < dict_len; ++i) {
        const int32_t v = read_int(bytes, base_ + i);
        table_[hash_offset(v, hash_log_)] = i;
    }
    last_off_ += dict_len;
}

Result<std::optional<int32_t>> FastCompressionHashTable::get(int32_t off, std::span<const uint8_t> bytes) {
    if (table_.empty()) {
        return err(LuceneError::illegal_state("LZ4 hash table is not initialized"));
    }
    const int32_t v = read_int(bytes, off);
    const size_t h = hash_offset(v, hash_log_);
    const int32_t reference = base_ + table_[h];
    table_[h] = off - base_;
    last_off_ = off;

    if (reference < off && off - reference < LZ4::MAX_DISTANCE && read_int(bytes, reference) == v) {
        return std::optional<int32_t>(reference);
    }
    return std::optional<int32_t>{};
}

std::optional<int32_t> FastCompressionHashTable::previous(int32_t /*off*/, std::span<const uint8_t> /*bytes*/) {
    return std::nullopt;
}

// ---------------- HighCompressionHashTable ----------------

void HighCompressionHashTable::reset(int32_t off, int32_t len) {
    if (chain_table_.empty()) {
        hash_table_.assign(static_cast<size_t>(LZ4::HASH_TABLE_SIZE_HC), -1);
        chain_table_.assign(static_cast<size_t>(LZ4::MAX_DISTANCE), 0xFFFF);
    } else if (end_ - base_ < static_cast<int32_t>(chain_table_.size())) {
        // The previous call compressed less than 64kB: only the reachable part
        // of the chain table has to be reset, which keeps repeated compression
        // of short inputs cheap.
        const size_t start_offset = static_cast<size_t>(base_ & MASK);
        const size_t end_offset = end_ == 0 ? 0 : static_cast<size_t>(((end_ - 1) & MASK) + 1);
        if (start_offset < end_offset) {
            std::fill(chain_table_.begin() + static_cast<std::vector<uint16_t>::difference_type>(start_offset),
                      chain_table_.begin() + static_cast<std::vector<uint16_t>::difference_type>(end_offset),
                      uint16_t{0xFFFF});
        } else {
            std::fill(chain_table_.begin(),
                      chain_table_.begin() + static_cast<std::vector<uint16_t>::difference_type>(end_offset),
                      uint16_t{0xFFFF});
            std::fill(chain_table_.begin() + static_cast<std::vector<uint16_t>::difference_type>(start_offset),
                      chain_table_.end(), uint16_t{0xFFFF});
        }
    } else {
        std::fill(hash_table_.begin(), hash_table_.end(), -1);
        std::fill(chain_table_.begin(), chain_table_.end(), uint16_t{0xFFFF});
    }
    base_ = off;
    next_ = off;
    end_ = off + len;
}

void HighCompressionHashTable::add_hash(int32_t off, std::span<const uint8_t> bytes) {
    const int32_t v = read_int(bytes, off);
    const size_t h = hash_hc(v);
    int32_t delta = off - hash_table_[h];
    if (delta <= 0 || delta >= LZ4::MAX_DISTANCE) {
        delta = LZ4::MAX_DISTANCE - 1;
    }
    chain_table_[static_cast<size_t>(off & MASK)] = static_cast<uint16_t>(delta);
    hash_table_[h] = off;
}

void HighCompressionHashTable::init_dictionary(int32_t dict_len, std::span<const uint8_t> bytes) {
    debug_check(next_ == base_, "the dictionary must be registered before any offset is hashed");
    for (int32_t i = 0; i < dict_len; ++i) {
        add_hash(base_ + i, bytes);
    }
    next_ += dict_len;
}

Result<std::optional<int32_t>> HighCompressionHashTable::get(int32_t off, std::span<const uint8_t> bytes) {
    if (chain_table_.empty()) {
        return err(LuceneError::illegal_state("LZ4 hash table is not initialized"));
    }
    while (next_ < off) {
        add_hash(next_, bytes);
        ++next_;
    }
    const int32_t v = read_int(bytes, off);
    const size_t h = hash_hc(v);

    attempts_ = 0;
    int32_t reference = hash_table_[h];
    if (reference >= off) {
        // Remainder from a previous `compress` call.
        return std::optional<int32_t>{};
    }
    const int32_t min = std::max(base_, off - LZ4::MAX_DISTANCE + 1);
    while (reference >= min && attempts_ < MAX_ATTEMPTS) {
        if (read_int(bytes, reference) == v) {
            return std::optional<int32_t>(reference);
        }
        reference -= static_cast<int32_t>(chain_table_[static_cast<size_t>(reference & MASK)]);
        ++attempts_;
    }
    return std::optional<int32_t>{};
}

std::optional<int32_t> HighCompressionHashTable::previous(int32_t off, std::span<const uint8_t> bytes) {
    const int32_t v = read_int(bytes, off);
    int32_t reference = off - static_cast<int32_t>(chain_table_[static_cast<size_t>(off & MASK)]);
    while (reference >= base_ && attempts_ < MAX_ATTEMPTS) {
        if (read_int(bytes, reference) == v) {
            return std::optional<int32_t>(reference);
        }
        reference -= static_cast<int32_t>(chain_table_[static_cast<size_t>(reference & MASK)]);
        ++attempts_;
    }
    return std::nullopt;
}

// ---------------- LZ4 ----------------

Result<int32_t> LZ4::decompress(DataInput &compressed, int32_t decompressed_len, std::span<uint8_t> dest,
                                int32_t d_off) {
    const int32_t dest_end = d_off + decompressed_len;
    while (true) {
        auto token_result = compressed.read_byte();
        if (!token_result.has_value()) {
            return err(token_result.error());
        }
        const int32_t token = static_cast<int32_t>(token_result.value());
        int32_t literal_len = static_cast<int32_t>(static_cast<uint32_t>(token) >> 4);

        if (literal_len != 0) {
            if (literal_len == 0x0F) {
                while (true) {
                    auto len_result = compressed.read_byte();
                    if (!len_result.has_value()) {
                        return err(len_result.error());
                    }
                    const int32_t len = static_cast<int32_t>(len_result.value());
                    if (len != 0xFF) {
                        literal_len += len;
                        break;
                    }
                    literal_len += 0xFF;
                }
            }
            if (auto result = compressed.read_bytes(dest, static_cast<size_t>(d_off),
                                                    static_cast<size_t>(literal_len));
                !result.has_value()) {
                return err(result.error());
            }
            d_off += literal_len;
        }

        if (d_off >= dest_end) {
            break;
        }

        auto match_result = compressed.read_short();
        if (!match_result.has_value()) {
            return err(match_result.error());
        }
        const int32_t match_dec = static_cast<int32_t>(static_cast<uint16_t>(match_result.value()));
        debug_check(match_dec > 0, "LZ4 match distance must be positive");

        int32_t match_len = token & 0x0F;
        if (match_len == 0x0F) {
            while (true) {
                auto len_result = compressed.read_byte();
                if (!len_result.has_value()) {
                    return err(len_result.error());
                }
                const int32_t len = static_cast<int32_t>(len_result.value());
                if (len != 0xFF) {
                    match_len += len;
                    break;
                }
                match_len += 0xFF;
            }
        }
        match_len += MIN_MATCH;

        // Copying a multiple of 8 bytes makes decompression 5% to 10% faster.
        const int32_t fast_len = (match_len + 7) & 0xFFF8;
        const int32_t room = static_cast<int32_t>(dest.size()) - d_off;

        if (match_dec < match_len || d_off + fast_len > dest_end) {
            if (match_len > room) {
                return err(LuceneError::illegal_argument("LZ4: decompressed data does not fit the target buffer"));
            }
            copy_match(dest, d_off, match_dec, match_len);
            d_off += match_len;
        } else {
            // The overrun up to `fast_len` is always rewritten by a following
            // sequence, so it is enough to stay inside `dest`.
            copy_match(dest, d_off, match_dec, std::min(fast_len, room));
            d_off += match_len;
        }

        if (d_off >= dest_end) {
            break;
        }
    }
    return d_off;
}

VoidResult LZ4::compress(std::span<const uint8_t> bytes, int32_t off, int32_t len, DataOutput &out,
                         Lz4HashTable &ht) {
    return compress_with_dictionary(bytes, off, 0, len, out, ht);
}

VoidResult LZ4::compress_with_dictionary(std::span<const uint8_t> bytes, int32_t dict_off, int32_t dict_len,
                                         int32_t len, DataOutput &out, Lz4HashTable &ht) {
    if (auto checked = CoreHelper::check_from_index_size(static_cast<size_t>(dict_off), static_cast<size_t>(dict_len),
                                                        bytes.size());
        !checked.has_value()) {
        return err(checked.error());
    }
    if (auto checked = CoreHelper::check_from_index_size(static_cast<size_t>(dict_off + dict_len),
                                                        static_cast<size_t>(len), bytes.size());
        !checked.has_value()) {
        return err(checked.error());
    }
    if (dict_len > MAX_DISTANCE) {
        return err(LuceneError::illegal_argument("dictLen must not be greater than 64kB, but got " +
                                                 std::to_string(dict_len)));
    }

    const int32_t end = dict_off + dict_len + len;
    int32_t off = dict_off + dict_len;
    int32_t anchor = off;

    if (len > LAST_LITERALS + MIN_MATCH) {
        const int32_t limit = end - LAST_LITERALS;
        const int32_t match_limit = limit - MIN_MATCH;
        ht.reset(dict_off, dict_len + len);
        ht.init_dictionary(dict_len, bytes);

        bool done = false;
        while (!done && off <= limit) {
            // Find a match.
            int32_t reference = 0;
            while (true) {
                if (off >= match_limit) {
                    done = true;
                    break;
                }
                auto candidate = ht.get(off, bytes);
                if (!candidate.has_value()) {
                    return err(candidate.error());
                }
                if (candidate.value().has_value()) {
                    reference = *candidate.value();
                    debug_check(reference >= dict_off && reference < off,
                                "LZ4 hash table returned an out-of-window reference");
                    break;
                }
                ++off;
            }
            if (done) {
                break;
            }

            // Compute the match length.
            int32_t match_len = MIN_MATCH + common_bytes(bytes, reference + MIN_MATCH, off + MIN_MATCH, limit);

            // Try to find a better match.
            const int32_t min = std::max(off - MAX_DISTANCE + 1, dict_off);
            auto previous = ht.previous(reference, bytes);
            while (previous.has_value() && *previous >= min) {
                const int32_t candidate_ref = *previous;
                const int32_t candidate_len =
                    MIN_MATCH + common_bytes(bytes, candidate_ref + MIN_MATCH, off + MIN_MATCH, limit);
                if (candidate_len > match_len) {
                    reference = candidate_ref;
                    match_len = candidate_len;
                }
                previous = ht.previous(candidate_ref, bytes);
            }

            // Encode the match.
            if (auto result = encode_sequence(bytes, anchor, reference, off, match_len, out); !result.has_value()) {
                return result;
            }
            off += match_len;
            anchor = off;
        }
    }

    // Handle the last literals.
    const int32_t literal_len = end - anchor;
    debug_check(literal_len >= LAST_LITERALS || literal_len == len, "LZ4 must end with at least 5 literal bytes");
    return encode_last_literals(bytes, anchor, literal_len, out);
}

} // namespace lucene
