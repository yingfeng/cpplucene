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

export module lucene_core:block_tree_terms_constants;

import :types;
import std;

import :bytes_ref;

namespace lucene {

// The FST output of a block tree term dictionary entry packs two flags into the
// low bits and, for floor blocks, the block file pointer in the remaining bits.

/// Number of low bits of an output that hold flags.
inline constexpr int32_t OUTPUT_FLAGS_NUM_BITS = 2;
inline constexpr int32_t OUTPUT_FLAGS_MASK = 0x3;
inline constexpr int32_t OUTPUT_FLAG_IS_FLOOR = 0x1;
inline constexpr int32_t OUTPUT_FLAG_HAS_TERMS = 0x2;

inline constexpr std::string_view TERMS_EXTENSION = "tim";
inline constexpr std::string_view TERMS_CODEC_NAME = "BlockTreeTermsDict";

/// Version that switched the term dictionary outputs to MSB vlongs.
///
/// **Deviation:** rlucene reaches these as
/// `lucene90_block_tree_terms_reader::VERSION_CURRENT`; in this port they live
/// in the flat `lucene` namespace next to the FST `VERSION_CURRENT` of
/// `:version`, so they carry a `BLOCK_TREE_` prefix.
inline constexpr int32_t BLOCK_TREE_VERSION_MSB_VLONG_OUTPUT = 1;
/// Version that introduced continuous FST arcs; the current format.
inline constexpr int32_t BLOCK_TREE_VERSION_FST_CONTINUOUS_ARCS = 2;
/// The only version this port reads and writes (older ones are rejected).
inline constexpr int32_t BLOCK_TREE_VERSION_CURRENT = BLOCK_TREE_VERSION_FST_CONTINUOUS_ARCS;

inline constexpr std::string_view TERMS_INDEX_EXTENSION = "tip";
inline constexpr std::string_view TERMS_INDEX_CODEC_NAME = "BlockTreeTermsIndex";

inline constexpr std::string_view TERMS_META_EXTENSION = "tmd";
inline constexpr std::string_view TERMS_META_CODEC_NAME = "BlockTreeTermsMeta";

/// rlucene's `NO_OUTPUT`: the empty output of the byte sequence FST algebra.
///
/// **Deviation:** rlucene has a `LazyLock<BytesRef<Arc<Vec<u8>>>>` static; the
/// port returns a reference to a function local static, which avoids a
/// module-initialization order problem.
[[nodiscard]] const BytesRef &no_output();

} // namespace lucene
