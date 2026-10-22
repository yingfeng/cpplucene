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

export module lucene_core:term_vectors_reader;

import :types;
import std;

import :close;
import :error;
import :term_vectors;

namespace lucene {

/// Reads the term vectors of a segment.
///
/// Port of the `TermVectorsReader` trait (`codecs/term_vectors_reader.rs`): a
/// [`TermVectors`] cursor plus the lifecycle of the files it holds open, so it
/// hands out clones instead of being shared.
export class TermVectorsReader : public TermVectors, public CloseableRef {
public:
    ~TermVectorsReader() override = default;

    /// Verifies the checksums of the files this reader holds; this may read the
    /// whole segment.
    [[nodiscard]] virtual VoidResult check_integrity() const = 0;

    /// An instance tuned for merging, or `nullptr` when this reader is already
    /// suitable.
    [[nodiscard]] virtual Result<std::unique_ptr<TermVectorsReader>> get_merge_instance() const = 0;

    /// A reader over the same files with its own cursor.
    [[nodiscard]] virtual Result<std::unique_ptr<TermVectorsReader>> clone() const = 0;
};

} // namespace lucene
