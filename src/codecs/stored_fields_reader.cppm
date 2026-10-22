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

export module lucene_core:stored_fields_reader;

import :types;
import std;

import :close;
import :error;
import :stored_fields;

namespace lucene {

/// Reads the stored fields of a segment.
///
/// Port of the `StoredFieldsReader` trait (`codecs/stored_fields_reader.rs`).
/// A reader is a [`StoredFields`] cursor plus the lifecycle of the files it holds
/// open, so every reader hands out clones instead of being shared.
export class StoredFieldsReader : public StoredFields, public CloseableRef {
public:
    ~StoredFieldsReader() override = default;

    /// Verifies the checksums of the files this reader holds.
    ///
    /// This may read the whole segment, so it is not part of opening.
    [[nodiscard]] virtual VoidResult check_integrity() const = 0;

    /// An instance tuned for merging, or `nullptr` when this reader is already
    /// suitable.
    [[nodiscard]] virtual Result<std::unique_ptr<StoredFieldsReader>> get_merge_instance() const = 0;

    /// A reader over the same files with its own cursor.
    [[nodiscard]] virtual Result<std::unique_ptr<StoredFieldsReader>> clone() const = 0;
};

} // namespace lucene
