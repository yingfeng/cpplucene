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

export module lucene_core:fields_index;

import :types;
import std;

import :close;
import :error;

namespace lucene {

/// Maps doc ids to the chunk of a stored fields or term vectors file that holds
/// them.
///
/// Port of the `FieldsIndex` trait
/// (`codecs/lucene90/fields_index.rs`).  A `.fdx`/`.tvd`-style file keeps the
/// documents in chunks of `1 << block_shift`; the index stores, as monotonic
/// numerics, the first doc id and the start pointer of every chunk, which makes
/// both `doc -> chunk` and `chunk -> pointer` a binary search.
export class FieldsIndex : public CloseableRef {
public:
    ~FieldsIndex() override = default;

    /// The id of the chunk that holds `doc_id`.
    [[nodiscard]] virtual Result<int64_t> get_block_id(int32_t doc_id) const = 0;

    /// The file pointer the chunk starts at.
    [[nodiscard]] virtual Result<size_t> get_block_start_pointer(int64_t block_id) const = 0;

    /// The number of bytes of the chunk.
    [[nodiscard]] virtual Result<size_t> get_block_length(int64_t block_id) const = 0;

    /// The file pointer the chunk that holds `doc_id` starts at.
    [[nodiscard]] virtual Result<size_t> get_start_pointer(int32_t doc_id) const {
        auto block = get_block_id(doc_id);
        if (!block.has_value()) {
            return std::unexpected(std::move(block.error()));
        }
        return get_block_start_pointer(block.value());
    }

    /// Verifies the checksum of the whole index file.
    [[nodiscard]] virtual VoidResult check_integrity() const = 0;

    /// A reader over the same file with its own cursor.
    ///
    /// The reference implementation gets this from `TryClone`; C++ needs it in the interface, because
    /// the callers only hold a [`FieldsIndex`].
    [[nodiscard]] virtual Result<std::unique_ptr<FieldsIndex>> clone() const = 0;
};

} // namespace lucene
