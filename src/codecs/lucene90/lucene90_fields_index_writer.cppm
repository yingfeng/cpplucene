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

export module lucene_core:lucene90_fields_index_writer;

import :types;
import std;

import :close;
import :directory;
import :error;
import :index_output;
import :io_context;

namespace lucene {

/// Writes the `.fdx`-style index of a chunked file.
///
/// Port of `FieldsIndexWriter`
/// (`codecs/lucene90/fields_index_writer.rs`).  The writer is fed one
/// `(num_docs, start_pointer)` pair per chunk while the caller writes the chunks
/// themselves; `finish` copies both sequences into the index file as monotonic
/// numerics and appends the metadata to the caller's `.fdt`/`.tvm` metadata file.
///
/// The counts and pointers are first buffered in two temporary files (the writer
/// does not know how many chunks there will be), and those files are deleted once
/// `finish` has consumed them.
export class Lucene90FieldsIndexWriter final : public Closeable {
public:
    /// `codec_name` prefixes the temporary file names and the index header.
    [[nodiscard]] static Result<std::unique_ptr<Lucene90FieldsIndexWriter>> create(
            Directory &dir, std::string_view name, std::string_view suffix, std::string_view extension,
            std::string_view codec_name, std::span<const uint8_t> id, int32_t block_shift,
            const IOContext &context);

    ~Lucene90FieldsIndexWriter() override;

    Lucene90FieldsIndexWriter(const Lucene90FieldsIndexWriter &) = delete;
    Lucene90FieldsIndexWriter &operator=(const Lucene90FieldsIndexWriter &) = delete;

    /// Records one chunk: `num_docs` documents starting at `start_pointer`.
    [[nodiscard]] VoidResult write_index(int32_t num_docs, size_t start_pointer);

    /// Writes the index file and appends its metadata to `meta_out`.
    ///
    /// `max_pointer` is the end of the last chunk, i.e. the length of the chunked
    /// file.
    [[nodiscard]] VoidResult finish(int32_t num_docs, size_t max_pointer, IndexOutput &meta_out);

    /// Closes and deletes the temporary files that `finish` has not consumed.
    [[nodiscard]] VoidResult close() override;

private:
    Lucene90FieldsIndexWriter(Directory &dir, std::string name, std::string suffix, std::string extension,
                             std::string codec_name, std::vector<uint8_t> id, int32_t block_shift,
                             const IOContext &context, std::unique_ptr<IndexOutput> docs_out,
                             std::unique_ptr<IndexOutput> file_pointers_out);

    VoidResult close_temp_outputs(bool temp_outputs_closed, bool docs_out_pending_delete,
                                  bool file_pointers_out_pending_delete);

    Directory &dir_;
    std::string name_;
    std::string suffix_;
    std::string extension_;
    std::string codec_name_;
    std::vector<uint8_t> id_;
    int32_t block_shift_{0};
    IOContext context_;
    std::unique_ptr<IndexOutput> docs_out_;
    std::unique_ptr<IndexOutput> file_pointers_out_;
    bool docs_out_pending_delete_{true};
    bool file_pointers_out_pending_delete_{true};
    bool temp_outputs_closed_{false};
    int32_t total_docs_{0};
    int32_t total_chunks_{0};
    size_t previous_fp_{0};
};

} // namespace lucene
