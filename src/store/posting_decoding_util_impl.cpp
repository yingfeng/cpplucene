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

module lucene_core:posting_decoding_util.impl;

import :posting_decoding_util;

import :types;
import std;

import :error;
import :data_input;

namespace lucene {

namespace {

/// `value >> shift` with the 32-bit shift semantics of the reference
/// implementation: a shift distance of 32 or more yields 0 instead of being
/// undefined behaviour.
[[nodiscard]] inline uint32_t unsigned_shift(uint32_t value, int32_t shift) noexcept {
    return shift >= 32 ? 0u : (value >> static_cast<uint32_t>(shift));
}

/// The reference implementation drives these kernels with the `wide` crate.  This
/// port uses clang's native vector types, which lower to the same SSE2/AVX2 (x86-64)
/// or NEON (aarch64) instructions without a third-party dependency.  Both targets
/// have their 128-bit vector unit as part of the base ISA, so no runtime dispatch
/// is needed; other architectures get the scalar kernel below.
#if defined(__x86_64__) || defined(__aarch64__)
using I32x4 = int32_t __attribute__((ext_vector_type(4)));
using I32x8 = int32_t __attribute__((ext_vector_type(8)));

/// The spans the postings decoders hand in are `int32_t` arrays, i.e. 4-byte
/// aligned, so every vector access goes through `memcpy`.  Clang folds these into
/// single unaligned loads/stores.
[[nodiscard]] inline I32x4 load4(const int32_t *p) noexcept {
    I32x4 value;
    std::memcpy(&value, p, sizeof(value));
    return value;
}

[[nodiscard]] inline I32x8 load8(const int32_t *p) noexcept {
    I32x8 value;
    std::memcpy(&value, p, sizeof(value));
    return value;
}

inline void store4(int32_t *p, I32x4 value) noexcept { std::memcpy(p, &value, sizeof(value)); }

inline void store8(int32_t *p, I32x8 value) noexcept { std::memcpy(p, &value, sizeof(value)); }

[[nodiscard]] inline I32x4 splat4(int32_t v) noexcept { return I32x4{v, v, v, v}; }

[[nodiscard]] inline I32x8 splat8(int32_t v) noexcept { return I32x8{v, v, v, v, v, v, v, v}; }

/// `values >> shift`; the caller guarantees `1 <= shift <= 31`.
template <typename V>
[[nodiscard]] inline V shift_right(V values, int32_t shift) noexcept {
    return values >> static_cast<int>(shift);
}

/// `split_ints_same`, vectorised: `b` and `c` share one buffer.
void split_same_kernel(size_t count,
                       std::span<int32_t> b_and_c,
                       int32_t b_shift,
                       int32_t dec,
                       int32_t b_mask,
                       size_t c_index,
                       int32_t c_mask) {
    int32_t *const data = b_and_c.data();
    const size_t size = b_and_c.size();
    const int32_t max_iter = (b_shift - 1) / dec;
    const bool shift_ok = b_shift >= 1 && b_shift <= 31;

    // Four inputs spread over seven planes with adjacent shifts: one load, seven
    // masked shifts.  This is the layout of the 8-bit decoders.
    if (count == 4 && b_shift == 7 && dec == 1 && size >= 28 && c_index >= 28 && c_index + 4 <= size) {
        const I32x4 values = load4(data + c_index);
        const I32x4 mask = splat4(b_mask);
        store4(data + 0, shift_right(values, 7) & mask);
        store4(data + 4, shift_right(values, 6) & mask);
        store4(data + 8, shift_right(values, 5) & mask);
        store4(data + 12, shift_right(values, 4) & mask);
        store4(data + 16, shift_right(values, 3) & mask);
        store4(data + 20, shift_right(values, 2) & mask);
        store4(data + 24, shift_right(values, 1) & mask);
        store4(data + c_index, values & splat4(c_mask));
        return;
    }

    // Eight inputs over three planes, every other bit: the 16-bit layout.
    if (count == 8 && b_shift == 6 && dec == 2 && size >= 24 && c_index >= 24 && c_index + 8 <= size) {
        const I32x8 values = load8(data + c_index);
        const I32x8 mask = splat8(b_mask);
        store8(data + 0, shift_right(values, 6) & mask);
        store8(data + 8, shift_right(values, 4) & mask);
        store8(data + 16, shift_right(values, 2) & mask);
        store8(data + c_index, values & splat8(c_mask));
        return;
    }

    const size_t output_size = static_cast<size_t>(max_iter + 1) * count;
    const bool planes_in_bounds = shift_ok && output_size <= size && output_size <= c_index && c_index + count <= size;

    // A single output plane: one contiguous pass over input and output.
    if (max_iter == 0 && planes_in_bounds) {
        const int32_t shift = b_shift;
        const I32x8 mask8 = splat8(b_mask);
        const I32x8 cmask8 = splat8(c_mask);
        size_t i = 0;
        for (; i + 8 <= count; i += 8) {
            const I32x8 values = load8(data + c_index + i);
            store8(data + i, shift_right(values, shift) & mask8);
            store8(data + c_index + i, values & cmask8);
        }
        if (i + 4 <= count) {
            const I32x4 values = load4(data + c_index + i);
            store4(data + i, shift_right(values, shift) & splat4(b_mask));
            store4(data + c_index + i, values & splat4(c_mask));
            i += 4;
        }
        for (; i < count; ++i) {
            const int32_t raw = data[c_index + i];
            data[i] = static_cast<int32_t>(unsigned_shift(static_cast<uint32_t>(raw), shift) &
                                          static_cast<uint32_t>(b_mask));
            data[c_index + i] = raw & c_mask;
        }
        return;
    }

    // Two or three planes: reuse each input vector across the planes instead of
    // re-reading it per plane.
    if (planes_in_bounds && count % 4 == 0 && (max_iter == 1 || max_iter == 2)) {
        const int32_t shift0 = b_shift;
        const int32_t shift1 = b_shift - dec;
        const int32_t shift2 = b_shift - 2 * dec;
        const I32x8 mask8 = splat8(b_mask);
        const I32x8 cmask8 = splat8(c_mask);
        size_t i = 0;
        for (; i + 8 <= count; i += 8) {
            const I32x8 values = load8(data + c_index + i);
            store8(data + i, shift_right(values, shift0) & mask8);
            store8(data + count + i, shift_right(values, shift1) & mask8);
            if (max_iter == 2) {
                store8(data + 2 * count + i, shift_right(values, shift2) & mask8);
            }
            store8(data + c_index + i, values & cmask8);
        }
        if (i + 4 <= count) {
            const I32x4 mask4 = splat4(b_mask);
            const I32x4 cmask4 = splat4(c_mask);
            const I32x4 values = load4(data + c_index + i);
            store4(data + i, shift_right(values, shift0) & mask4);
            store4(data + count + i, shift_right(values, shift1) & mask4);
            if (max_iter == 2) {
                store4(data + 2 * count + i, shift_right(values, shift2) & mask4);
            }
            store4(data + c_index + i, values & cmask4);
            i += 4;
        }
        for (; i < count; ++i) {
            const int32_t raw = data[c_index + i];
            const uint32_t value = static_cast<uint32_t>(raw);
            data[i] = static_cast<int32_t>(unsigned_shift(value, shift0) & static_cast<uint32_t>(b_mask));
            data[count + i] =
                    static_cast<int32_t>(unsigned_shift(value, shift1) & static_cast<uint32_t>(b_mask));
            if (max_iter == 2) {
                data[2 * count + i] =
                        static_cast<int32_t>(unsigned_shift(value, shift2) & static_cast<uint32_t>(b_mask));
            }
            data[c_index + i] = raw & c_mask;
        }
        return;
    }

    if (planes_in_bounds) {
        // General case: `max_iter + 1` planes, each a masked shift by a decreasing
        // distance.  The planes are produced vector-at-a-time so each input vector
        // is loaded once.
        const I32x8 mask8 = splat8(b_mask);
        const I32x8 cmask8 = splat8(c_mask);
        size_t i = 0;
        for (; i + 8 <= count; i += 8) {
            const I32x8 values = load8(data + c_index + i);
            for (int32_t j = 0; j <= max_iter; ++j) {
                const int32_t shift = b_shift - j * dec;
                if (shift > 0) {
                    store8(data + count * static_cast<size_t>(j) + i, shift_right(values, shift) & mask8);
                }
            }
            store8(data + c_index + i, values & cmask8);
        }
        if (i + 4 <= count) {
            const I32x4 values = load4(data + c_index + i);
            for (int32_t j = 0; j <= max_iter; ++j) {
                const int32_t shift = b_shift - j * dec;
                if (shift > 0) {
                    store4(data + count * static_cast<size_t>(j) + i, shift_right(values, shift) & splat4(b_mask));
                }
            }
            store4(data + c_index + i, values & splat4(c_mask));
            i += 4;
        }
        for (; i < count; ++i) {
            const int32_t raw = data[c_index + i];
            const uint32_t value = static_cast<uint32_t>(raw);
            for (int32_t j = 0; j <= max_iter; ++j) {
                const int32_t shift = b_shift - j * dec;
                if (shift > 0) {
                    data[count * static_cast<size_t>(j) + i] =
                            static_cast<int32_t>(unsigned_shift(value, shift) & static_cast<uint32_t>(b_mask));
                }
            }
            data[c_index + i] = raw & c_mask;
        }
        return;
    }

    // Out-of-range or overlapping layouts: the scalar element-by-element path.
    for (int32_t j = 0; j <= max_iter; ++j) {
        const int32_t shift = b_shift - j * dec;
        if (shift <= 0) {
            continue;
        }
        const size_t base = count * static_cast<size_t>(j);
        for (size_t i = 0; i < count; ++i) {
            data[base + i] = static_cast<int32_t>(unsigned_shift(static_cast<uint32_t>(data[c_index + i]), shift) &
                                                 static_cast<uint32_t>(b_mask));
        }
    }
    for (size_t i = 0; i < count; ++i) {
        data[c_index + i] &= c_mask;
    }
}

/// `split_ints_diff`, vectorised: `b` and `c` are different buffers.
void split_diff_kernel(size_t count,
                       std::span<int32_t> b,
                       int32_t b_shift,
                       int32_t dec,
                       int32_t b_mask,
                       std::span<int32_t> c,
                       size_t c_index,
                       int32_t c_mask) {
    int32_t *const out = b.data();
    int32_t *const in = c.data();
    const int32_t max_iter = (b_shift - 1) / dec;
    const bool shift_ok = b_shift >= 1 && b_shift <= 31;
    const bool output_ok = shift_ok && static_cast<size_t>(max_iter + 1) * count <= b.size() && c_index + count <= c.size();

    if (count == 4 && b_shift == 7 && dec == 1 && b.size() >= 28 && c_index + 4 <= c.size()) {
        const I32x4 values = load4(in + c_index);
        const I32x4 mask = splat4(b_mask);
        store4(out + 0, shift_right(values, 7) & mask);
        store4(out + 4, shift_right(values, 6) & mask);
        store4(out + 8, shift_right(values, 5) & mask);
        store4(out + 12, shift_right(values, 4) & mask);
        store4(out + 16, shift_right(values, 3) & mask);
        store4(out + 20, shift_right(values, 2) & mask);
        store4(out + 24, shift_right(values, 1) & mask);
        store4(in + c_index, values & splat4(c_mask));
        return;
    }

    if (count == 8 && b_shift == 6 && dec == 2 && b.size() >= 24 && c_index + 8 <= c.size()) {
        const I32x8 values = load8(in + c_index);
        const I32x8 mask = splat8(b_mask);
        store8(out + 0, shift_right(values, 6) & mask);
        store8(out + 8, shift_right(values, 4) & mask);
        store8(out + 16, shift_right(values, 2) & mask);
        store8(in + c_index, values & splat8(c_mask));
        return;
    }

    if (max_iter == 0 && shift_ok && count <= b.size() && c_index + count <= c.size()) {
        const int32_t shift = b_shift;
        const I32x8 mask8 = splat8(b_mask);
        const I32x8 cmask8 = splat8(c_mask);
        size_t i = 0;
        for (; i + 8 <= count; i += 8) {
            const I32x8 values = load8(in + c_index + i);
            store8(out + i, shift_right(values, shift) & mask8);
            store8(in + c_index + i, values & cmask8);
        }
        if (i + 4 <= count) {
            const I32x4 values = load4(in + c_index + i);
            store4(out + i, shift_right(values, shift) & splat4(b_mask));
            store4(in + c_index + i, values & splat4(c_mask));
            i += 4;
        }
        for (; i < count; ++i) {
            const int32_t raw = in[c_index + i];
            out[i] = static_cast<int32_t>(unsigned_shift(static_cast<uint32_t>(raw), shift) &
                                          static_cast<uint32_t>(b_mask));
            in[c_index + i] = raw & c_mask;
        }
        return;
    }

    if (output_ok && count % 4 == 0 && (max_iter == 1 || max_iter == 2)) {
        const int32_t shift0 = b_shift;
        const int32_t shift1 = b_shift - dec;
        const int32_t shift2 = b_shift - 2 * dec;
        const I32x8 mask8 = splat8(b_mask);
        const I32x8 cmask8 = splat8(c_mask);
        size_t i = 0;
        for (; i + 8 <= count; i += 8) {
            const I32x8 values = load8(in + c_index + i);
            store8(out + i, shift_right(values, shift0) & mask8);
            store8(out + count + i, shift_right(values, shift1) & mask8);
            if (max_iter == 2) {
                store8(out + 2 * count + i, shift_right(values, shift2) & mask8);
            }
            store8(in + c_index + i, values & cmask8);
        }
        if (i + 4 <= count) {
            const I32x4 mask4 = splat4(b_mask);
            const I32x4 cmask4 = splat4(c_mask);
            const I32x4 values = load4(in + c_index + i);
            store4(out + i, shift_right(values, shift0) & mask4);
            store4(out + count + i, shift_right(values, shift1) & mask4);
            if (max_iter == 2) {
                store4(out + 2 * count + i, shift_right(values, shift2) & mask4);
            }
            store4(in + c_index + i, values & cmask4);
            i += 4;
        }
        for (; i < count; ++i) {
            const int32_t raw = in[c_index + i];
            const uint32_t value = static_cast<uint32_t>(raw);
            out[i] = static_cast<int32_t>(unsigned_shift(value, shift0) & static_cast<uint32_t>(b_mask));
            out[count + i] = static_cast<int32_t>(unsigned_shift(value, shift1) & static_cast<uint32_t>(b_mask));
            if (max_iter == 2) {
                out[2 * count + i] =
                        static_cast<int32_t>(unsigned_shift(value, shift2) & static_cast<uint32_t>(b_mask));
            }
            in[c_index + i] = raw & c_mask;
        }
        return;
    }

    if (output_ok) {
        const I32x8 mask8 = splat8(b_mask);
        const I32x8 cmask8 = splat8(c_mask);
        size_t i = 0;
        for (; i + 8 <= count; i += 8) {
            const I32x8 values = load8(in + c_index + i);
            for (int32_t j = 0; j <= max_iter; ++j) {
                const int32_t shift = b_shift - j * dec;
                if (shift > 0) {
                    store8(out + count * static_cast<size_t>(j) + i, shift_right(values, shift) & mask8);
                }
            }
            store8(in + c_index + i, values & cmask8);
        }
        if (i + 4 <= count) {
            const I32x4 values = load4(in + c_index + i);
            for (int32_t j = 0; j <= max_iter; ++j) {
                const int32_t shift = b_shift - j * dec;
                if (shift > 0) {
                    store4(out + count * static_cast<size_t>(j) + i, shift_right(values, shift) & splat4(b_mask));
                }
            }
            store4(in + c_index + i, values & splat4(c_mask));
            i += 4;
        }
        for (; i < count; ++i) {
            const int32_t raw = in[c_index + i];
            const uint32_t value = static_cast<uint32_t>(raw);
            for (int32_t j = 0; j <= max_iter; ++j) {
                const int32_t shift = b_shift - j * dec;
                if (shift > 0) {
                    out[count * static_cast<size_t>(j) + i] =
                            static_cast<int32_t>(unsigned_shift(value, shift) & static_cast<uint32_t>(b_mask));
                }
            }
            in[c_index + i] = raw & c_mask;
        }
        return;
    }

    for (int32_t j = 0; j <= max_iter; ++j) {
        const int32_t shift = b_shift - j * dec;
        if (shift <= 0) {
            continue;
        }
        const size_t base = count * static_cast<size_t>(j);
        for (size_t i = 0; i < count; ++i) {
            out[base + i] = static_cast<int32_t>(unsigned_shift(static_cast<uint32_t>(in[c_index + i]), shift) &
                                                static_cast<uint32_t>(b_mask));
        }
    }
    for (size_t i = 0; i < count; ++i) {
        in[c_index + i] &= c_mask;
    }
}

#else // no native vector types: the scalar kernel

void split_same_kernel(size_t count,
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

void split_diff_kernel(size_t count,
                       std::span<int32_t> b,
                       int32_t b_shift,
                       int32_t dec,
                       int32_t b_mask,
                       std::span<int32_t> c,
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
            b[base + i] = static_cast<int32_t>(unsigned_shift(static_cast<uint32_t>(c[c_index + i]), shift) &
                                               static_cast<uint32_t>(b_mask));
        }
    }
    for (size_t i = 0; i < count; ++i) {
        c[c_index + i] &= c_mask;
    }
}

#endif

} // namespace

VoidResult PostingDecodingUtil::split_ints_same(size_t count,
                                                std::span<int32_t> b_and_c,
                                                int32_t b_shift,
                                                int32_t dec,
                                                int32_t b_mask,
                                                size_t c_index,
                                                int32_t c_mask) {
    if (dec <= 0) {
        return err(LuceneError::illegal_argument("dec must be positive"));
    }
    auto read = input_.read_ints(b_and_c, c_index, count);
    if (!read) {
        return read;
    }
    split_same_kernel(count, b_and_c, b_shift, dec, b_mask, c_index, c_mask);
    return {};
}

VoidResult PostingDecodingUtil::split_ints_diff(size_t count,
                                                std::span<int32_t> b,
                                                int32_t b_shift,
                                                int32_t dec,
                                                int32_t b_mask,
                                                std::span<int32_t> c,
                                                size_t c_index,
                                                int32_t c_mask) {
    if (dec <= 0) {
        return err(LuceneError::illegal_argument("dec must be positive"));
    }
    auto read = input_.read_ints(c, c_index, count);
    if (!read) {
        return read;
    }
    split_diff_kernel(count, b, b_shift, dec, b_mask, c, c_index, c_mask);
    return {};
}

} // namespace lucene
