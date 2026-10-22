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

export module lucene_core:block_tree_msb_vlong;

import :types;
import std;

import :data_input;
import :data_output;
import :error;

namespace lucene {

/// Reads a variable-length `int64_t` written by [`write_msb_vlong`].
///
/// Port of `read_msb_vlong`
/// (`core/codecs/lucene90/block_tree/field_reader.rs`).  Unlike the regular
/// vlong this keeps zero bits in the most significant byte, so that sibling
/// block pointers share their prefix bytes.
[[nodiscard]] Result<int64_t> read_msb_vlong(DataInput &input);

/// Writes `value` (which must not be negative) as an MSB first vlong.
///
/// Port of `write_msb_vlong`
/// (`core/codecs/lucene90/block_tree/lucene90_block_tree_terms_writer.rs`).
[[nodiscard]] VoidResult write_msb_vlong(int64_t value, DataOutput &out);

} // namespace lucene
