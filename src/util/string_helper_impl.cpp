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

module lucene_core:string_helper.impl;

import :string_helper;
import :bit_util;
import :core_helper;

import :types;
import :debug;
import :error;
import std;

namespace lucene {

namespace {

/// `Character.MAX_CODE_POINT`.
constexpr int32_t MAX_UNICODE_CODE_POINT = 0x10FFFF;

int32_t compute_good_fast_hash_seed() {
    // The reference implementation reads the `tests.seed` system property and falls back to the
    // current wall clock (milliseconds).
    if (const char *prop = std::getenv("tests.seed"); prop != nullptr && *prop != '\0') {
        int32_t hash = 0;
        for (const char *p = prop; *p != '\0'; ++p) {
            hash = static_cast<int32_t>(static_cast<uint32_t>(hash) * 31u + static_cast<uint8_t>(*p));
        }
        return hash;
    }
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<int32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

/// Debug rendering of a byte range: `[61 62 63]`, matching the reference implementation's `Display`.
std::string hex_view(std::span<const uint8_t> bytes) {
    std::string result;
    result.reserve(bytes.size() * 3 + 2);
    result.push_back('[');
    for (size_t i = 0; i < bytes.size(); ++i) {
        if (i > 0) {
            result.push_back(' ');
        }
        static constexpr char kHex[] = "0123456789abcdef";
        result.push_back(kHex[bytes[i] >> 4]);
        result.push_back(kHex[bytes[i] & 0x0f]);
    }
    result.push_back(']');
    return result;
}

/// A 128-bit counter rendered big-endian, seeded once per process.
struct IdState {
    uint64_t high;
    uint64_t low;
};

IdState make_initial_id_state() {
    uint64_t x0 = 0;
    uint64_t x1 = 0;
    if (const char *prop = std::getenv("tests.seed"); prop != nullptr && *prop != '\0') {
        std::string_view text(prop);
        const std::string_view tail = text.size() > 8 ? text.substr(text.size() - 8) : text;
        uint64_t seed = 0;
        auto [ptr, ec] = std::from_chars(tail.data(), tail.data() + tail.size(), seed, 16);
        seed = (ec == std::errc()) ? seed : 0;
        x0 = seed;
        x1 = seed;
    } else {
        std::random_device device;
        std::mt19937_64 rng((static_cast<uint64_t>(device()) << 32) ^ device());
        x0 = rng();
        x1 = rng();
    }
    // scatter_id_seed: xorshift-based mixing, 10 rounds.
    for (int i = 0; i < 10; ++i) {
        uint64_t s1 = x0;
        const uint64_t s0 = x1;
        x0 = s0;
        s1 ^= s1 << 23;
        x1 = s1 ^ s0 ^ (s1 >> 17) ^ (s0 >> 26);
    }
    return IdState{x0, x1};
}

IdState &next_id_state() {
    static IdState state = make_initial_id_state();
    return state;
}

std::mutex next_id_mutex;

} // namespace

int32_t StringHelper::good_fast_hash_seed() noexcept {
    static const int32_t seed = compute_good_fast_hash_seed();
    return seed;
}

Result<size_t> StringHelper::bytes_difference(std::span<const uint8_t> prior_term, std::span<const uint8_t> current_term) {
    const int32_t mismatch = CoreHelper::mismatch_u8(prior_term, current_term);
    if (mismatch < 0) {
        return err(LuceneError::illegal_argument(std::format("terms out of order: priorTerm={}, currentTerm={}",
                                                             hex_view(prior_term),
                                                             hex_view(current_term))));
    }
    return static_cast<size_t>(mismatch);
}

Result<size_t> StringHelper::sort_key_length(std::span<const uint8_t> prior_term, std::span<const uint8_t> current_term) {
    auto difference = bytes_difference(prior_term, current_term);
    if (!difference) {
        return std::unexpected(std::move(difference.error()));
    }
    return difference.value() + 1;
}

bool StringHelper::starts_with(std::span<const uint8_t> ref, std::span<const uint8_t> prefix) noexcept {
    return starts_with(ref, 0, ref.size(), prefix, 0, prefix.size());
}

bool StringHelper::starts_with(std::span<const uint8_t> ref,
                               size_t ref_offset,
                               size_t ref_length,
                               std::span<const uint8_t> prefix,
                               size_t prefix_offset,
                               size_t prefix_length) noexcept {
    // Not long enough to start with the prefix.
    if (ref_length < prefix_length) {
        return false;
    }
    debug_check(ref_offset + prefix_length <= ref.size());
    debug_check(prefix_offset + prefix_length <= prefix.size());
    // An empty prefix matches everything, and `memcmp` must not be handed a null
    // pointer (an empty span may have one) even when no byte is compared.
    if (prefix_length == 0) {
        return true;
    }
    return std::memcmp(ref.data() + ref_offset, prefix.data() + prefix_offset, prefix_length) == 0;
}

bool StringHelper::ends_with(std::span<const uint8_t> ref, std::span<const uint8_t> suffix) noexcept {
    if (ref.size() < suffix.size()) {
        return false;
    }
    const size_t start_at = ref.size() - suffix.size();
    // Same null-pointer caveat as [`starts_with`]: an empty suffix always matches.
    if (suffix.empty()) {
        return true;
    }
    return std::memcmp(ref.data() + start_at, suffix.data(), suffix.size()) == 0;
}

int32_t StringHelper::murmurhash3_x86_32(std::span<const uint8_t> data, size_t offset, size_t len, int32_t seed) {
    constexpr uint32_t c1 = 0xcc9e2d51u;
    constexpr uint32_t c2 = 0x1b873593u;

    debug_check(offset + len <= data.size());

    uint32_t h1 = static_cast<uint32_t>(seed);
    const size_t rounded_end = offset + (len & 0xfffffffcull);

    // Walk the validated prefix as fixed four-byte blocks: the loop bound is a
    // counter instead of a comparison against the end offset, so the address is
    // a simple increment and the caller's bounds check is not repeated per word.
    const uint8_t *cursor = data.data() + offset;
    for (size_t blocks = len >> 2; blocks > 0; --blocks, cursor += 4) {
        uint32_t k1 =
            static_cast<uint32_t>(BitUtil::get_i32_le(std::span<const uint8_t>(cursor, 4), 0));
        k1 *= c1;
        k1 = std::rotl(k1, 15);
        k1 *= c2;

        h1 ^= k1;
        h1 = std::rotl(h1, 13);
        h1 = h1 * 5u + 0xe6546b64u;
    }

    // Tail.
    uint32_t k1 = 0;
    switch (len & 0x03ull) {
        case 3:
            k1 ^= static_cast<uint32_t>(data[rounded_end + 2]) << 16;
            [[fallthrough]];
        case 2:
            k1 ^= static_cast<uint32_t>(data[rounded_end + 1]) << 8;
            [[fallthrough]];
        case 1:
            k1 ^= static_cast<uint32_t>(data[rounded_end]);
            k1 *= c1;
            k1 = std::rotl(k1, 15);
            k1 *= c2;
            h1 ^= k1;
            break;
        default:
            break;
    }

    // Finalization.
    debug_check(len <= static_cast<size_t>(std::numeric_limits<int32_t>::max()));
    h1 ^= static_cast<uint32_t>(len);
    h1 ^= h1 >> 16;
    h1 *= 0x85ebca6bu;
    h1 ^= h1 >> 13;
    h1 *= 0xc2b2ae35u;
    h1 ^= h1 >> 16;

    return static_cast<int32_t>(h1);
}

Result<int32_t> StringHelper::utf8_code_point_at(std::span<const uint8_t> utf8, size_t pos, size_t &num_bytes) {
    if (pos >= utf8.size()) {
        return err(LuceneError::illegal_argument(
                std::format("Position {} out of bounds for utf8 array of length {}", pos, utf8.size())));
    }

    const auto lead_byte = static_cast<uint32_t>(utf8[pos]);
    size_t length = 0;
    int32_t value = 0;
    if (lead_byte < 0x80u) {
        num_bytes = 1;
        return static_cast<int32_t>(lead_byte);
    }
    if ((lead_byte & 0xE0u) == 0xC0u) {
        length = 2;
        value = static_cast<int32_t>(lead_byte & 0x1Fu);
    } else if ((lead_byte & 0xF0u) == 0xE0u) {
        length = 3;
        value = static_cast<int32_t>(lead_byte & 0x0Fu);
    } else if ((lead_byte & 0xF8u) == 0xF0u) {
        length = 4;
        value = static_cast<int32_t>(lead_byte & 0x07u);
    } else {
        return err(LuceneError::illegal_argument(std::format("Invalid UTF8 header byte: 0x{:X}", lead_byte)));
    }

    const size_t limit = pos + length;
    for (size_t i = pos + 1; i < limit; ++i) {
        if (i >= utf8.size()) {
            return err(LuceneError::illegal_argument("UTF-8 sequence truncated"));
        }
        const auto byte = static_cast<uint32_t>(utf8[i]);
        if ((byte & 0xC0u) != 0x80u) {
            return err(LuceneError::illegal_argument(std::format("Invalid UTF-8 continuation byte: 0x{:X}", byte)));
        }
        value = (value << 6) | static_cast<int32_t>(byte & 0x3Fu);
    }

    const bool overlong = (length == 2 && value < 0x80) || (length == 3 && value < 0x800) ||
                          (length == 4 && value < 0x10000);
    if (overlong || value > MAX_UNICODE_CODE_POINT || (value >= 0xD800 && value <= 0xDFFF)) {
        return err(LuceneError::illegal_argument("UTF-8 sequence is not a Unicode scalar value"));
    }

    num_bytes = length;
    return value;
}

Result<int32_t> StringHelper::utf8_code_point_at(std::string_view text, size_t pos, size_t &num_bytes) {
    return utf8_code_point_at(std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(text.data()), text.size()),
                              pos, num_bytes);
}

VoidResult StringHelper::utf8_append(std::string &out, int32_t code_point) {
    if (code_point < 0 || code_point > MAX_UNICODE_CODE_POINT || (code_point >= 0xD800 && code_point <= 0xDFFF)) {
        return err(LuceneError::illegal_state(
                std::format("not a Unicode scalar value: {}", code_point)));
    }
    const auto value = static_cast<uint32_t>(code_point);
    if (value < 0x80u) {
        out.push_back(static_cast<char>(value));
    } else if (value < 0x800u) {
        out.push_back(static_cast<char>(0xC0u | (value >> 6)));
        out.push_back(static_cast<char>(0x80u | (value & 0x3Fu)));
    } else if (value < 0x10000u) {
        out.push_back(static_cast<char>(0xE0u | (value >> 12)));
        out.push_back(static_cast<char>(0x80u | ((value >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (value & 0x3Fu)));
    } else {
        out.push_back(static_cast<char>(0xF0u | (value >> 18)));
        out.push_back(static_cast<char>(0x80u | ((value >> 12) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | ((value >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (value & 0x3Fu)));
    }
    return {};
}

Result<std::vector<int32_t>> StringHelper::utf8_to_code_points(std::string_view text) {
    std::vector<int32_t> code_points;
    code_points.reserve(text.size());
    size_t pos = 0;
    while (pos < text.size()) {
        size_t num_bytes = 0;
        auto code_point = utf8_code_point_at(text, pos, num_bytes);
        if (!code_point) {
            return std::unexpected(std::move(code_point.error()));
        }
        code_points.push_back(code_point.value());
        pos += num_bytes;
    }
    return code_points;
}

std::array<uint8_t, StringHelper::ID_LENGTH> StringHelper::random_id() {
    std::array<uint8_t, ID_LENGTH> result{};
    {
        std::lock_guard<std::mutex> guard(next_id_mutex);
        IdState &state = next_id_state();
        // Big-endian rendering of the 128-bit counter.
        for (size_t i = 0; i < 8; ++i) {
            result[i] = static_cast<uint8_t>(state.high >> (56 - 8 * i));
            result[8 + i] = static_cast<uint8_t>(state.low >> (56 - 8 * i));
        }
        // Wrapping increment, low word first.
        if (++state.low == 0) {
            ++state.high;
        }
    }
    return result;
}

std::string StringHelper::id_to_string(std::span<const uint8_t> id) {
    if (id.empty()) {
        return "(null)";
    }
    // Base-36 rendering of a big-endian unsigned integer, done with repeated
    // division on a scratch copy.
    std::vector<uint8_t> scratch(id.begin(), id.end());
    std::string digits;
    bool nonzero = std::any_of(scratch.begin(), scratch.end(), [](uint8_t b) { return b != 0; });
    while (nonzero) {
        uint32_t remainder = 0;
        nonzero = false;
        for (auto &byte : scratch) {
            const uint32_t current = (remainder << 8) | byte;
            byte = static_cast<uint8_t>(current / 36u);
            remainder = current % 36u;
            if (byte != 0) {
                nonzero = true;
            }
        }
        static constexpr char kDigits[] = "0123456789abcdefghijklmnopqrstuvwxyz";
        digits.push_back(kDigits[remainder]);
    }
    if (digits.empty()) {
        digits.push_back('0');
    }
    std::ranges::reverse(digits);
    return digits;
}

} // namespace lucene
