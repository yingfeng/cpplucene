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

export module lucene_core:bit_mixer;

import :types;

namespace lucene {

/// Bit mixing utilities that spread a key space evenly over the `int32_t`
/// range.
///
/// Port of `BitMixer` (`core/internal/hppc/bit_mixer.rs`), forked from
/// `com.carrotsearch.hppc.BitMixer` 0.10.0.
export struct BitMixer {
    static constexpr uint32_t PHI_C32 = 0x9e3779b9U;
    static constexpr uint64_t PHI_C64 = 0x9e3779b97f4a7c15ULL;

    /// MH3's finalization step, the mixing used for `int32_t` keys.
    [[nodiscard]] static constexpr uint32_t mix32(uint32_t k) noexcept {
        k ^= k >> 16;
        k *= 0x85ebca6bU;
        k ^= k >> 13;
        k *= 0xc2b2ae35U;
        return k ^ (k >> 16);
    }

    [[nodiscard]] static constexpr uint32_t mix_i32(int32_t key) noexcept {
        return mix32(static_cast<uint32_t>(key));
    }

    /// David Stafford's variant 9, the mixing used for `int64_t` keys.
    [[nodiscard]] static constexpr uint64_t mix64(uint64_t z) noexcept {
        z ^= z >> 32;
        z *= 0x4cd6944c5cc20b6dULL;
        z ^= z >> 29;
        z *= 0xfc12c5b19d3259e9ULL;
        return z ^ (z >> 32);
    }
};

} // namespace lucene
