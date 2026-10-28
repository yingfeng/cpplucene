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

export module lucene_core:soft_deletes_reader;

import :types;
import std;

import :bits;
import :codec_reader;
import :standard_directory_reader;
import :error;
import :filter_codec_reader;

namespace lucene {

/// A [`CodecReader`] whose live docs additionally hide the soft-deleted
/// documents of a segment.
///
/// Port of `SoftDeletesFilterCodecReader`
/// (`index/soft_deletes_directory_reader_wrapper.rs`).
export class SoftDeletesFilterCodecReader final : public FilterCodecReader {
public:
    /// `live_docs` must already exclude both hard and soft deletes.
    SoftDeletesFilterCodecReader(std::shared_ptr<CodecReader> delegate, std::shared_ptr<const Bits> live_docs,
                                 int32_t num_docs);

    [[nodiscard]] Result<int32_t> num_docs() const override { return num_docs_; }
    [[nodiscard]] Result<int32_t> num_deleted_docs() const override;
    [[nodiscard]] Result<bool> has_deletions() const override { return true; }
    [[nodiscard]] Result<const Bits *> live_docs() const override { return live_docs_.get(); }
    [[nodiscard]] std::string to_string() const override;

private:
    std::shared_ptr<const Bits> live_docs_;
    int32_t num_docs_;
};

/// Returns `reader` filtered by the soft-delete field, or `reader` itself when
/// it has no soft-deleted documents (or the field is absent).
///
/// `field` is the doc values field written by
/// [`IndexWriter::soft_delete_documents`](IndexWriter); a document is
/// soft-deleted when its value there is non-zero.  Hard deletes are preserved.
[[nodiscard]] Result<std::shared_ptr<CodecReader>> wrap_soft_deletes(std::shared_ptr<CodecReader> reader,
                                                                    std::string_view field);

/// Applies [`wrap_soft_deletes`] to every leaf of `reader`, dropping the leaves
/// that end up with no live documents.
///
/// Port of `SoftDeletesDirectoryReaderWrapper`
/// (`index/soft_deletes_directory_reader_wrapper.rs`).
[[nodiscard]] Result<std::shared_ptr<DirectoryReader>>
wrap_soft_deletes_directory_reader(std::shared_ptr<DirectoryReader> reader, std::string field);

} // namespace lucene
