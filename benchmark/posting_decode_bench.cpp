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

// Micro-benchmark for the two postings decode kernels this port vectorises:
// `PostingDecodingUtil::split_ints_same` / `split_ints_diff`, plus the
// `PackedInts` bulk codec used by the packed stream helpers.
//
// The input read is measured separately, because `split_ints_*` also reads the
// block through `DataInput`; only the kernel timings exclude it.
//
// Build with -DLUCENE_CORE_BUILD_BENCHMARKS=ON and run:
//     ./benchmark/bench_posting_decode

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <vector>

import lucene_core;

using namespace lucene;

namespace {

using Clock = std::chrono::steady_clock;

constexpr size_t kBufferSize = 256;

[[nodiscard]] uint32_t unsigned_shift(uint32_t value, int32_t shift) noexcept {
    return shift >= 32 ? 0u : (value >> static_cast<uint32_t>(shift));
}

/// The scalar kernel `split_ints_same` replaced.
void scalar_split_same(size_t count,
                       std::span<int32_t> b_and_c,
                       int32_t b_shift,
                       int32_t dec,
                       int32_t b_mask,
                       size_t c_index,
                       int32_t c_mask) {
    const int32_t max_iter = (b_shift - 1) / dec;
    for (int32_t j = 0; j <= max_iter; ++j) {
        const int32_t shift = b_shift - j * dec;
        if (shift <= 0) {
            continue;
        }
        const size_t base = count * static_cast<size_t>(j);
        for (size_t i = 0; i < count; ++i) {
            b_and_c[base + i] = static_cast<int32_t>(unsigned_shift(static_cast<uint32_t>(b_and_c[c_index + i]), shift) &
                                                     static_cast<uint32_t>(b_mask));
        }
    }
    for (size_t i = 0; i < count; ++i) {
        b_and_c[c_index + i] &= c_mask;
    }
}

/// A `Format.PACKED` value read bit by bit: the naive scalar baseline.
[[nodiscard]] int64_t scalar_packed_get(std::span<const uint8_t> bytes, int32_t bits, size_t index) {
    uint64_t value = 0;
    for (int32_t b = 0; b < bits; ++b) {
        const size_t bit = index * static_cast<size_t>(bits) + static_cast<size_t>(b);
        value = (value << 1) | static_cast<uint64_t>((bytes[bit >> 3] >> (7 - (bit & 7))) & 1);
    }
    return static_cast<int64_t>(value);
}

/// The same value read the way a tuned scalar decoder does it: one unaligned
/// 64-bit load covering the value, byte-swapped to big-endian, then one shift
/// and one mask.
///
/// `bytes` must have eight readable bytes past the last value, which
/// `bench_bulk_decode` guarantees by padding.
[[nodiscard]] int64_t scalar_packed_get_wordbased(std::span<const uint8_t> bytes, int32_t bits, size_t index) {
    const size_t bit = index * static_cast<size_t>(bits);
    const size_t byte_start = bit >> 3;
    const uint32_t bit_offset = static_cast<uint32_t>(bit & 7);
    uint64_t word = 0;
    std::memcpy(&word, bytes.data() + byte_start, sizeof(word));
    word = __builtin_bswap64(word);
    const uint32_t total = bit_offset + static_cast<uint32_t>(bits);
    if (total <= 64) {
        return static_cast<int64_t>((word << bit_offset) >> (64 - static_cast<uint32_t>(bits)));
    }
    // Wider than one word (only possible for bits close to 64): fall back.
    return scalar_packed_get(bytes, bits, index);
}

struct Shape {
    const char *name;
    size_t count;
    int32_t b_shift;
    int32_t dec;
    int32_t b_mask;
    size_t c_index;
    int32_t c_mask;
};

/// The argument tuples `ForUtil` / `ForDeltaUtil` actually pass.
constexpr Shape kShapes[] = {
        {"decode8   (count=4,  7 planes)", 4, 7, 1, 0xFF, 28, 0xFF},
        {"decode4   (count=16, 1 plane )", 16, 4, 4, 0xFF, 16, 0xFF},
        {"delta b=4 (count=16, 3 planes)", 16, 12, 4, 0xFFFF, 48, 0xFFFF},
        {"delta b=8 (count=32, 1 plane )", 32, 8, 8, 0xFFFF, 32, 0xFFFF},
        {"delta b=16(count=64, 1 plane )", 64, 16, 16, 0xFFFF, 64, 0xFFFF},
};

[[nodiscard]] std::vector<uint8_t> make_block(size_t count) {
    std::vector<uint8_t> bytes(count * 4, 0);
    uint32_t state = 12345;
    for (size_t i = 0; i < count; ++i) {
        state = state * 1664525u + 1013904223u;
        for (size_t b = 0; b < 4; ++b) {
            bytes[i * 4 + b] = static_cast<uint8_t>(state >> (8 * b));
        }
    }
    return bytes;
}

void bench_split_same() {
    constexpr size_t kIterations = 20'000'000;
    std::printf("\nsplit_ints_same: %zu iterations per shape, ns per call\n", kIterations);
    std::printf("%-34s %10s %12s %12s %11s %10s\n", "shape", "read only", "read+scalar", "read+vector", "scalar kern",
                "speedup");
    uint64_t sink = 0;
    std::vector<int32_t> buffer(kBufferSize, 0);
    for (const Shape &shape : kShapes) {
        const std::vector<uint8_t> bytes = make_block(shape.count);

        auto start = Clock::now();
        for (size_t n = 0; n < kIterations; ++n) {
            ByteArrayDataInput in{std::span<const uint8_t>(bytes)};
            if (!in.read_ints(buffer, shape.c_index, shape.count).has_value()) {
                std::fprintf(stderr, "read failed\n");
                std::exit(1);
            }
            sink += static_cast<uint64_t>(buffer[n % kBufferSize]);
        }
        const double read_ns =
                static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count()) /
                static_cast<double>(kIterations);

        start = Clock::now();
        for (size_t n = 0; n < kIterations; ++n) {
            ByteArrayDataInput in{std::span<const uint8_t>(bytes)};
            if (!in.read_ints(buffer, shape.c_index, shape.count).has_value()) {
                std::fprintf(stderr, "read failed\n");
                std::exit(1);
            }
            scalar_split_same(shape.count, buffer, shape.b_shift, shape.dec, shape.b_mask, shape.c_index, shape.c_mask);
            sink += static_cast<uint64_t>(buffer[n % kBufferSize]);
        }
        const double scalar_total_ns =
                static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count()) /
                static_cast<double>(kIterations);

        start = Clock::now();
        for (size_t n = 0; n < kIterations; ++n) {
            ByteArrayDataInput in{std::span<const uint8_t>(bytes)};
            PostingDecodingUtil pdu(in);
            if (!pdu.split_ints_same(shape.count, buffer, shape.b_shift, shape.dec, shape.b_mask, shape.c_index,
                                     shape.c_mask)
                         .has_value()) {
                std::fprintf(stderr, "vector kernel failed\n");
                std::exit(1);
            }
            sink += static_cast<uint64_t>(buffer[n % kBufferSize]);
        }
        const double vector_total_ns =
                static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count()) /
                static_cast<double>(kIterations);

        const double scalar_kernel_ns = scalar_total_ns - read_ns;
        const double vector_kernel_ns = vector_total_ns - read_ns;
        std::printf("%-34s %10.3f %12.3f %12.3f %11.3f %9.2fx\n",
                    shape.name,
                    read_ns,
                    scalar_total_ns,
                    vector_total_ns,
                    scalar_kernel_ns,
                    scalar_kernel_ns / vector_kernel_ns);
    }
    if (sink == 0xFFFFFFFFFFFFFFFFULL) {
        std::printf(" ");
    }
}

/// Round-trips every bit width through the packed codec.  This runs in the
/// benchmark's build type (Release), so it also covers the optimised codegen of
/// the decode kernels, not just the Debug build the unit tests use.
bool verify_all_widths() {
    constexpr size_t kValues = 997;
    for (int32_t bits = 1; bits <= 64; ++bits) {
        auto codec = bulk_operation(bits);
        if (!codec.has_value()) {
            std::fprintf(stderr, "no bulk operation for %d bits\n", bits);
            return false;
        }
        std::vector<int64_t> values(kValues, 0);
        uint64_t state = 12345;
        for (size_t i = 0; i < kValues; ++i) {
            state = state * 6364136223846793005ULL + 1442695040888963407ULL;
            values[i] = static_cast<int64_t>(state >> 3) & ((bits == 64) ? ~int64_t{0} : ((int64_t{1} << bits) - 1));
        }
        const size_t plain_size = (kValues * static_cast<size_t>(bits) + 7) / 8;
        std::vector<uint8_t> bytes(plain_size, 0);
        if (!codec.value().encode(values, 0, bytes).has_value()) {
            std::fprintf(stderr, "encode failed at %d bits\n", bits);
            return false;
        }
        std::vector<int64_t> decoded(kValues, 0);
        if (!codec.value().decode(std::span<const uint8_t>(bytes), 0, decoded).has_value()) {
            std::fprintf(stderr, "decode failed at %d bits\n", bits);
            return false;
        }
        if (decoded != values) {
            std::fprintf(stderr, "round trip mismatch at %d bits\n", bits);
            return false;
        }
    }
    return true;
}

void bench_bulk_decode() {
    constexpr size_t kValues = 1024;
    constexpr size_t kIterations = 100'000;
    std::printf("\npacked decode of %zu values per call, %zu iterations, ns per call\n", kValues, kIterations);
    std::printf("%-6s %14s %14s %12s %14s\n", "bits", "bit-by-bit", "word scalar", "bulk", "bulk vs word");

    for (const int32_t bits : {4, 8, 12, 16, 21}) {
        auto codec = bulk_operation(bits);
        if (!codec.has_value()) {
            std::fprintf(stderr, "no bulk operation for %d bits\n", bits);
            std::exit(1);
        }
        std::vector<int64_t> values(kValues, 0);
        const uint64_t mask = bits >= 64 ? ~uint64_t{0} : ((uint64_t{1} << static_cast<uint32_t>(bits)) - 1);
        uint64_t state = 99;
        for (size_t i = 0; i < kValues; ++i) {
            state = state * 6364136223846793005ULL + 1442695040888963407ULL;
            values[i] = static_cast<int64_t>((state >> 7) & mask);
        }
        const size_t plain_size = (kValues * static_cast<size_t>(bits) + 7) / 8;
        // Eight bytes of slack so the word-based scalar reader can always load a
        // full word for the last value.
        std::vector<uint8_t> bytes(plain_size + 8, 0);
        if (!codec.value().encode(values, 0, std::span<uint8_t>(bytes.data(), plain_size)).has_value()) {
            std::fprintf(stderr, "encode failed\n");
            std::exit(1);
        }

        uint64_t sink = 0;
        std::vector<int64_t> decoded(kValues, 0);
        auto start = Clock::now();
        for (size_t n = 0; n < kIterations; ++n) {
            for (size_t i = 0; i < kValues; ++i) {
                decoded[i] = scalar_packed_get(bytes, bits, i);
            }
            sink += static_cast<uint64_t>(decoded[n % kValues]);
        }
        const double bit_by_bit_ns =
                static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count()) /
                static_cast<double>(kIterations);

        start = Clock::now();
        for (size_t n = 0; n < kIterations; ++n) {
            for (size_t i = 0; i < kValues; ++i) {
                decoded[i] = scalar_packed_get_wordbased(bytes, bits, i);
            }
            sink += static_cast<uint64_t>(decoded[n % kValues]);
        }
        const double word_ns =
                static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count()) /
                static_cast<double>(kIterations);

        start = Clock::now();
        for (size_t n = 0; n < kIterations; ++n) {
            if (!codec.value().decode(std::span<const uint8_t>(bytes.data(), plain_size), 0, decoded).has_value()) {
                std::fprintf(stderr, "decode failed\n");
                std::exit(1);
            }
            sink += static_cast<uint64_t>(decoded[n % kValues]);
        }
        const double bulk_ns =
                static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count()) /
                static_cast<double>(kIterations);

        std::printf("%-6d %14.1f %14.1f %12.1f %13.2fx\n", bits, bit_by_bit_ns, word_ns, bulk_ns, word_ns / bulk_ns);
        // The timed loops re-decode into `decoded` many times; check the last
        // round trip so the benchmark also validates correctness.
        if (decoded != values) {
            std::fprintf(stderr, "bulk decode mismatch at %d bits\n", bits);
            std::exit(1);
        }
        if (sink == 0xFFFFFFFFFFFFFFFFULL) {
            std::printf(" ");
        }
    }
}

} // namespace

int main() {
    std::printf("vector kernels compiled in: %s\n", PostingDecodingUtil::has_vector_kernels ? "yes" : "no");
    if (!verify_all_widths()) {
        return 1;
    }
    bench_split_same();
    bench_bulk_decode();
    return 0;
}
