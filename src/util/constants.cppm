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

export module lucene_core:constants;

import :types;
import std;

namespace lucene {

/// Buffer pool constants (`core/util/byte_block_pool.rs`).
export struct ByteBlockPoolConstants {
    /// `global_offset >> BYTE_BLOCK_SHIFT` == `global_offset / BYTE_BLOCK_SIZE`.
    static constexpr int32_t BYTE_BLOCK_SHIFT = 15;
    /// The size of each buffer in the pool.
    static constexpr int32_t BYTE_BLOCK_SIZE = 1 << BYTE_BLOCK_SHIFT;
    /// `position_in_current_buffer = global_offset & BYTE_BLOCK_MASK`.
    static constexpr int32_t BYTE_BLOCK_MASK = BYTE_BLOCK_SIZE - 1;
};

/// Largest allowed term position, so that `position + 128` cannot overflow.
///
/// Port of `index_writer::MAX_POSITION`; the reference implementation declares it in `index_writer`,
/// which does not exist in this port yet.
export inline constexpr int32_t MAX_POSITION = std::numeric_limits<int32_t>::max() - 128;

/// Some useful constants (`core/util/constants.rs`).
export struct Constants {
    /// Operating system type, e.g. `Linux`.
    [[nodiscard]] static std::string os_name();
    /// Operating system architecture, e.g. `x86_64`, or `unknown`.
    [[nodiscard]] static std::string os_arch();
    /// Operating system version / release string.
    [[nodiscard]] static std::string os_version();
};

} // namespace lucene
