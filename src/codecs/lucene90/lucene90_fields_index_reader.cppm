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

export module lucene_core:lucene90_fields_index_reader;

import :types;
import std;

import :directory;
import :direct_monotonic_reader;
import :error;
import :fields_index;
import :index_input;
import :io_context;
import :random_access_input;

namespace lucene {

/// Reads the `.fdx`-style index written by [`Lucene90FieldsIndexWriter`].
///
/// Port of `FieldsIndexReader`
/// (`codecs/lucene90/fields_index_reader.rs`).  Both the chunk start doc ids and
/// the chunk start pointers are monotonic numerics inside the index file, which
/// the reader slices out once and binary-searches afterwards.
export class Lucene90FieldsIndexReader final : public FieldsIndex {
public:
    /// Reads the metadata that `finish` appended to `meta_in`, then opens the
    /// index file named after `name`/`suffix`/`extension`.
    [[nodiscard]] static Result<std::unique_ptr<Lucene90FieldsIndexReader>> create(
            const Directory &dir, std::string_view name, std::string_view suffix, std::string_view extension,
            std::string_view codec_name, std::span<const uint8_t> id, DataInput &meta_in,
            const IOContext &context);

    ~Lucene90FieldsIndexReader() override;

    Lucene90FieldsIndexReader(const Lucene90FieldsIndexReader &) = delete;
    Lucene90FieldsIndexReader &operator=(const Lucene90FieldsIndexReader &) = delete;

    [[nodiscard]] Result<int64_t> get_block_id(int32_t doc_id) const override;
    [[nodiscard]] Result<size_t> get_block_start_pointer(int64_t block_id) const override;
    [[nodiscard]] Result<size_t> get_block_length(int64_t block_id) const override;
    [[nodiscard]] VoidResult check_integrity() const override;
    [[nodiscard]] Result<std::unique_ptr<FieldsIndex>> clone() const override;

    /// Closes the index file; further reads fail.
    [[nodiscard]] VoidResult close() const override;

    /// [`clone`] with the concrete type, so callers that use the format-specific
    /// accessors do not have to down-cast the interface result.
    [[nodiscard]] Result<std::unique_ptr<Lucene90FieldsIndexReader>> clone_reader() const;

    /// The length of the chunked file, i.e. the end of its last chunk.
    [[nodiscard]] size_t get_max_pointer() const noexcept { return max_pointer_; }

private:
    Lucene90FieldsIndexReader(int32_t max_doc, int32_t block_shift, int32_t num_chunks,
                              DirectMonotonicMeta docs_meta, DirectMonotonicMeta start_pointers_meta,
                              std::unique_ptr<IndexInput> index_input, size_t docs_start_pointer,
                              size_t docs_end_pointer, size_t start_pointers_start_pointer,
                              size_t start_pointers_end_pointer, size_t max_pointer);

    /// Opens the two monotonic readers over `input`; the slices must outlive them.
    VoidResult open_readers();

    int32_t max_doc_{0};
    int32_t block_shift_{0};
    int32_t num_chunks_{0};
    DirectMonotonicMeta docs_meta_;
    DirectMonotonicMeta start_pointers_meta_;
    /// `mutable` because [`CloseableRef::close`] is `const` and drops the handle.
    mutable std::unique_ptr<IndexInput> index_input_;
    size_t docs_start_pointer_{0};
    size_t docs_end_pointer_{0};
    size_t start_pointers_start_pointer_{0};
    size_t start_pointers_end_pointer_{0};
    size_t max_pointer_{0};
    /// Declared before the readers, which point into them.
    std::unique_ptr<RandomAccessInput> docs_slice_;
    std::unique_ptr<RandomAccessInput> start_pointers_slice_;
    std::optional<DirectMonotonicReader> docs_;
    std::optional<DirectMonotonicReader> start_pointers_;
};

} // namespace lucene
