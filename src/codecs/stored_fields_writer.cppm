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

export module lucene_core:stored_fields_writer;

import :types;
import std;

import :close;
import :error;
import :field_infos;
import :merge_state;

namespace lucene {

/// Writes the stored fields of the documents of a segment.
///
/// Port of the `StoredFieldsWriter` trait (`codecs/stored_fields_writer.rs`).
/// The sequence is always `start_document`, one call per stored field, then
/// `finish_document`; a document without stored fields still calls the first and
/// the last.
export class StoredFieldsWriter : public Closeable {
public:
    ~StoredFieldsWriter() override = default;

    /// Called before the stored fields of a document are written.
    [[nodiscard]] virtual VoidResult start_document() = 0;

    /// Called when a document and all its fields have been added.
    [[nodiscard]] virtual VoidResult finish_document() = 0;

    [[nodiscard]] virtual VoidResult write_field_i32(const FieldInfo &field_info, int32_t value) = 0;
    [[nodiscard]] virtual VoidResult write_field_i64(const FieldInfo &field_info, int64_t value) = 0;
    [[nodiscard]] virtual VoidResult write_field_f32(const FieldInfo &field_info, float value) = 0;
    [[nodiscard]] virtual VoidResult write_field_f64(const FieldInfo &field_info, double value) = 0;

    /// Writes a binary value; implementations that keep the bytes past this call
    /// must copy them.
    [[nodiscard]] virtual VoidResult write_field_bytes(const FieldInfo &field_info,
                                                       std::span<const uint8_t> value) = 0;

    [[nodiscard]] virtual VoidResult write_field_str(const FieldInfo &field_info, std::string_view value) = 0;

    /// Called once, with the number of documents that were written.
    ///
    /// The count is redundant with the calls to [`start_document`], which is the
    /// point: a codec can detect a document that was started but never finished
    /// (LUCENE-1282).
    [[nodiscard]] virtual VoidResult finish(int32_t num_docs) = 0;

    /// Merges the stored fields of the readers in `merge_state`, skipping deleted
    /// documents, and returns the number of documents written.
    ///
    /// Port of `StoredFieldsWriter::merge` / `StoredFieldsWriterDefaults::merge`.
    /// The default implementation re-serialises every field -- a codec that can
    /// copy raw bytes may override it.
    [[nodiscard]] virtual Result<int32_t> merge(MergeState &merge_state);
};

} // namespace lucene
