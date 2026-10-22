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

export module lucene_core:stored_fields_consumer;

import :types;
import std;

import :codec;
import :directory;
import :document;
import :error;
import :field_infos;
import :io_context;
import :segment_info;
import :stored_fields_format;
import :stored_fields_writer;

namespace lucene {

/// Feeds the stored fields of the documents being indexed to the codec.
///
/// Port of `StoredFieldsConsumer` (`index/stored_fields_consumer.rs`),
/// reduced to the default (non-sorting) hook: index sorting is not ported, so
/// the `Sorting`/`Test` variants of the reference implementation's `StoredFieldsConsumerHook` have
/// no equivalent here.
///
/// **Deviations** (C++ mechanics only):
///  * the directory, the format and the segment info are captured at
///    construction instead of being passed to every call;
///  * `flush` does not need the write state: the writer already knows its
///    directory and segment.
export class StoredFieldsConsumer {
public:
    StoredFieldsConsumer(Directory &directory, const StoredFieldsFormat &format, SegmentInfo &segment_info);

    StoredFieldsConsumer(const StoredFieldsConsumer &) = delete;
    StoredFieldsConsumer &operator=(const StoredFieldsConsumer &) = delete;
    StoredFieldsConsumer(StoredFieldsConsumer &&) noexcept = default;
    StoredFieldsConsumer &operator=(StoredFieldsConsumer &&) noexcept = default;
    ~StoredFieldsConsumer() = default;

    /// Starts `doc_id`, emitting empty documents for any gap.
    [[nodiscard]] VoidResult start_document(int32_t doc_id);

    /// Writes one stored field value.
    [[nodiscard]] VoidResult write_field(const FieldInfo &field_info, const FieldValue &value);

    [[nodiscard]] VoidResult finish_document();

    /// Emits the empty documents up to `max_doc - 1`.
    [[nodiscard]] VoidResult finish(int32_t max_doc);

    /// [`finish`] followed by closing the writer.
    [[nodiscard]] VoidResult flush(int32_t max_doc);

    /// Closes the writer without finishing the segment.
    [[nodiscard]] VoidResult abort();

    /// Whether the codec writer has been created (i.e. a document was started).
    [[nodiscard]] bool is_initialized() const noexcept { return writer_ != nullptr; }

private:
    [[nodiscard]] VoidResult init_writer();

    Directory *directory_{nullptr};
    const StoredFieldsFormat *format_{nullptr};
    SegmentInfo *segment_info_{nullptr};
    std::unique_ptr<StoredFieldsWriter> writer_;
    int32_t last_doc_{-1};
};

} // namespace lucene
