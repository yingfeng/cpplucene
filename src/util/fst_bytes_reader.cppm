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

export module lucene_core:fst_bytes_reader;

import :types;
import :error;

import :data_input;

namespace lucene {

/// A [`DataInput`] whose position can be read and set, which is what the FST
/// reader needs to seek inside a serialized automaton.
///
/// Port of the `BytesReader` trait (`util/fst_impl/fst.rs`).  It lives in its own
/// partition because both the byte-array readers and the on-disk readers
/// implement it.
export class FstBytesReader : public DataInput {
public:
    /// Current position (in bytes) inside the underlying storage.
    [[nodiscard]] virtual int64_t get_position() const = 0;

    /// Moves the read cursor to `pos`.
    virtual void set_position(int64_t pos) = 0;
};

} // namespace lucene
