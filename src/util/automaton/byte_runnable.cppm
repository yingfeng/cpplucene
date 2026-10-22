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

export module lucene_core:byte_runnable;

import :types;
import std;

import :error;

namespace lucene {

/// A runnable automaton that accepts a byte array as input.
///
/// Port of the `ByteRunnable` trait (`core/util/automation/byte_runnable.rs`).
export class ByteRunnable {
public:
    virtual ~ByteRunnable() = default;

    /// The state reached by reading `c` in `state`, or `-1` when there is no
    /// such transition.
    [[nodiscard]] virtual Result<int32_t> step(int32_t state, int32_t c) = 0;

    /// Whether `state` is an accepting state.
    [[nodiscard]] virtual Result<bool> is_accept(int32_t state) const = 0;

    /// Number of states; for an NFA this may be an approximation.
    [[nodiscard]] virtual size_t get_size() const = 0;

    /// Whether `s[offset, offset + length)` is accepted.
    [[nodiscard]] Result<bool> run(std::span<const uint8_t> s, size_t offset, size_t length) {
        int32_t state = 0;
        const size_t end = offset + length;
        if (end > s.size()) {
            return err(LuceneError::array_index_out_of_bounds(
                    std::format("offset: {}, length: {}, size: {}", offset, length, s.size())));
        }
        for (size_t i = offset; i < end; ++i) {
            auto next = step(state, static_cast<int32_t>(s[i]));
            if (!next) {
                return std::unexpected(std::move(next.error()));
            }
            state = next.value();
            if (state == -1) {
                return false;
            }
        }
        return is_accept(state);
    }
};

} // namespace lucene
