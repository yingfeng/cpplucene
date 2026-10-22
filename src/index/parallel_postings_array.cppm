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

export module lucene_core:parallel_postings_array;

import :types;
import std;

import :array_util;
import :error;

namespace lucene {

/// Bookkeeping arrays of the in-memory postings, indexed by term id.
///
/// Port of `parallel_postings_array.rs`.  The reference implementation models the two concrete
/// flavours (`FreqProxPostingsArray`, `TermVectorsPostingsArray`) plus the
/// shared `ParallelPostingsArray` base with an enum (`PostingsArrayEnum`) whose
/// methods match on the variant.  C++ uses the same class hierarchy the enum
/// wraps: [`ParallelPostingsArray`] is the base and the flavours derive from
/// it, so the enum dispatch becomes a virtual call.
///
/// **Deviation:** the RAM counter (`bytes_used`) is not part of the postings
/// arrays here, because the port has no `Accountable`/`Counter`
/// infrastructure.
export class PostingsArray {
public:
    explicit PostingsArray(size_t size);

    PostingsArray(const PostingsArray &) = delete;
    PostingsArray &operator=(const PostingsArray &) = delete;
    PostingsArray(PostingsArray &&) noexcept = default;
    PostingsArray &operator=(PostingsArray &&) noexcept = default;
    virtual ~PostingsArray() = default;

    /// Number of term ids the arrays can hold.
    [[nodiscard]] size_t size() const noexcept { return size_; }

    /// Bytes retained per term id; used to size the next growth step.
    [[nodiscard]] virtual size_t bytes_per_posting() const = 0;

    /// Grows every array to exactly `new_size`.
    [[nodiscard]] virtual VoidResult copy_to(size_t new_size) = 0;

    /// Grows to `ArrayUtil::oversize(size + 1, bytes_per_posting)`.
    [[nodiscard]] VoidResult grow() {
        auto new_size = ArrayUtil::oversize(size_ + 1, bytes_per_posting());
        if (!new_size.has_value()) {
            return std::unexpected(std::move(new_size.error()));
        }
        return copy_to(new_size.value());
    }

    /// Maps a term id to the start of its term text in the bytes hash.
    std::vector<int32_t> text_starts;
    /// Maps a term id to the current postings stream address.
    std::vector<int32_t> address_offset;
    /// Maps a term id to the start of its postings stream.
    std::vector<int32_t> byte_starts;

protected:
    size_t size_{0};
};

/// The three arrays every postings array needs.
///
/// Port of `ParallelPostingsArray` (`parallel_postings_array.rs`).
export class ParallelPostingsArray : public PostingsArray {
public:
    /// `text_starts`, `address_offset`, `byte_starts`, all `int32_t`.
    static constexpr size_t BYTES_PER_POSTING = 3 * sizeof(int32_t);

    explicit ParallelPostingsArray(size_t size) : PostingsArray(size) {}

    [[nodiscard]] size_t bytes_per_posting() const override { return BYTES_PER_POSTING; }
    [[nodiscard]] VoidResult copy_to(size_t new_size) override;
};

} // namespace lucene
