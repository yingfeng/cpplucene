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

export module lucene_core:check_sum_index_input;

import :types;
import std;

import :error;
import :index_input;

namespace lucene {

/// Buffer size used by [`CheckSumIndexInput::skip_by_reading`].
export inline constexpr size_t SKIP_BUFFER_SIZE = 1024;

/// An [`IndexInput`] that computes a checksum while it reads.
///
/// Port of the `ChecksumIndexInput` trait (`core/store/check_sum_index_input.rs`).
///
/// Seeks can only move forward, and they are expensive because the skipped
/// bytes have to be read to keep the checksum correct.
export class CheckSumIndexInput : public IndexInput {
public:
    ~CheckSumIndexInput() override = default;

    /// The checksum of everything read so far.
    [[nodiscard]] virtual int64_t get_checksum() = 0;

    /// Forward-only seek; fails when `pos` is behind the current position.
    [[nodiscard]] VoidResult seek(size_t pos) override;

    /// Reads (and therefore checksums) `num_bytes` bytes and discards them.
    [[nodiscard]] virtual VoidResult skip_by_reading(size_t num_bytes);
};

} // namespace lucene
