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

export module lucene_core:postings_util;

import :types;
import std;

import :error;
import :data_input;
import :data_output;

namespace lucene {

/// Utility struct to encode/decode a postings block.  Port of `PostingsUtil`
/// (`core/codecs/lucene101/postings_util.rs`).
///
/// Used for the tail blocks of a postings list, where plain group-varint
/// encoding is cheaper than bit-packing.  When frequencies are stored, the
/// common case (`freq == 1`) is folded into the low bit of the doc id and only
/// the remaining frequencies are written as vints.
export struct PostingsUtil {
    /// Reads values written by [`write_vint_block`].
    ///
    /// `doc_buffer` and (when `index_has_freq && decode_freq`) `freq_buffer`
    /// must hold at least `num` values.
    [[nodiscard]] static VoidResult read_vint_block(DataInput &doc_in,
                                                    std::span<int32_t> doc_buffer,
                                                    std::span<int32_t> freq_buffer,
                                                    size_t num,
                                                    bool index_has_freq,
                                                    bool decode_freq);

    /// Writes `doc_buffer[0..num)` with group-varint encoding and the
    /// corresponding frequencies (`freq_buffer`) with vint encoding.
    [[nodiscard]] static VoidResult write_vint_block(DataOutput &doc_out,
                                                     std::span<int32_t> doc_buffer,
                                                     std::span<const int32_t> freq_buffer,
                                                     size_t num,
                                                     bool write_freqs);
};

} // namespace lucene
