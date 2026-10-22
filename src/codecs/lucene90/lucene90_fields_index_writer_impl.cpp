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

module lucene_core:lucene90_fields_index_writer.impl;

import :lucene90_fields_index_writer;

import :codec_util;
import :data_input;
import :data_output;
import :directory;
import :direct_monotonic_writer;
import :error;
import :index_input;
import :index_output;
import :io_context;
import :segment_info;
import :types;

import std;

namespace lucene {

namespace {

constexpr int32_t VERSION_START = 0;
/// Only the current version is written and read, by design.
constexpr int32_t VERSION_CURRENT = VERSION_START;

} // namespace

Lucene90FieldsIndexWriter::Lucene90FieldsIndexWriter(Directory &dir, std::string name, std::string suffix,
                                                    std::string extension, std::string codec_name,
                                                    std::vector<uint8_t> id, int32_t block_shift,
                                                    const IOContext &context,
                                                    std::unique_ptr<IndexOutput> docs_out,
                                                    std::unique_ptr<IndexOutput> file_pointers_out)
    : dir_(dir),
      name_(std::move(name)),
      suffix_(std::move(suffix)),
      extension_(std::move(extension)),
      codec_name_(std::move(codec_name)),
      id_(std::move(id)),
      block_shift_(block_shift),
      context_(context),
      docs_out_(std::move(docs_out)),
      file_pointers_out_(std::move(file_pointers_out)) {}

Lucene90FieldsIndexWriter::~Lucene90FieldsIndexWriter() { static_cast<void>(close()); }

Result<std::unique_ptr<Lucene90FieldsIndexWriter>> Lucene90FieldsIndexWriter::create(
        Directory &dir, std::string_view name, std::string_view suffix, std::string_view extension,
        std::string_view codec_name, std::span<const uint8_t> id, int32_t block_shift, const IOContext &context) {
    auto docs_out = dir.create_temp_output(name, std::string(codec_name) + "-doc_ids", context);
    if (!docs_out.has_value()) {
        return std::unexpected(std::move(docs_out.error()));
    }
    std::unique_ptr<IndexOutput> docs_output = std::move(docs_out.value().output);

    // Both temporary files carry a header, so a truncated one is detected when
    // `finish` reads it back.
    if (auto header = CodecUtil::write_header(*docs_output, std::string(codec_name) + "Docs", VERSION_CURRENT);
        !header.has_value()) {
        static_cast<void>(docs_output->close());
        static_cast<void>(dir.delete_file(docs_out.value().name));
        return std::unexpected(std::move(header.error()));
    }

    auto file_pointers = dir.create_temp_output(name, std::string(codec_name) + "-file_pointers", context);
    if (!file_pointers.has_value()) {
        static_cast<void>(docs_output->close());
        static_cast<void>(dir.delete_file(docs_out.value().name));
        return std::unexpected(std::move(file_pointers.error()));
    }
    std::unique_ptr<IndexOutput> file_pointers_output = std::move(file_pointers.value().output);
    if (auto header = CodecUtil::write_header(*file_pointers_output, std::string(codec_name) + "FilePointers",
                                              VERSION_CURRENT);
        !header.has_value()) {
        static_cast<void>(docs_output->close());
        static_cast<void>(file_pointers_output->close());
        static_cast<void>(dir.delete_file(docs_out.value().name));
        static_cast<void>(dir.delete_file(file_pointers.value().name));
        return std::unexpected(std::move(header.error()));
    }

    std::unique_ptr<Lucene90FieldsIndexWriter> writer(new Lucene90FieldsIndexWriter(
            dir, std::string(name), std::string(suffix), std::string(extension), std::string(codec_name),
            std::vector<uint8_t>(id.begin(), id.end()), block_shift, context, std::move(docs_output),
            std::move(file_pointers_output)));
    return writer;
}

VoidResult Lucene90FieldsIndexWriter::write_index(int32_t num_docs, size_t start_pointer) {
    if (start_pointer < previous_fp_) {
        return err(LuceneError::illegal_argument("chunk pointers must be appended in order"));
    }
    if (auto written = docs_out_->write_vint(num_docs); !written.has_value()) {
        return written;
    }
    if (auto written = file_pointers_out_->write_vlong(static_cast<int64_t>(start_pointer - previous_fp_));
        !written.has_value()) {
        return written;
    }
    previous_fp_ = start_pointer;
    total_docs_ += num_docs;
    ++total_chunks_;
    return {};
}

VoidResult Lucene90FieldsIndexWriter::finish(int32_t num_docs, size_t max_pointer, IndexOutput &meta_out) {
    if (num_docs != total_docs_) {
        return err(LuceneError::illegal_state("expected " + std::to_string(num_docs) + " docs, but got " +
                                              std::to_string(total_docs_)));
    }

    if (auto footer = CodecUtil::write_footer(*docs_out_); !footer.has_value()) {
        return footer;
    }
    if (auto footer = CodecUtil::write_footer(*file_pointers_out_); !footer.has_value()) {
        return footer;
    }
    const std::string docs_name(docs_out_->get_name());
    const std::string file_pointers_name(file_pointers_out_->get_name());

    auto docs_closed = docs_out_->close();
    auto pointers_closed = file_pointers_out_->close();
    if (!docs_closed.has_value()) {
        return docs_closed;
    }
    if (!pointers_closed.has_value()) {
        return pointers_closed;
    }
    temp_outputs_closed_ = true;

    auto data_out = dir_.create_output(segment_file_name(name_, suffix_, extension_), context_);
    if (!data_out.has_value()) {
        return std::unexpected(std::move(data_out.error()));
    }

    auto header = CodecUtil::write_index_header(*data_out.value(), codec_name_ + "Idx", VERSION_CURRENT, id_,
                                                suffix_);
    if (!header.has_value()) {
        static_cast<void>(data_out.value()->close());
        return std::unexpected(std::move(header.error()));
    }

    auto pointer = data_out.value()->get_file_pointer();
    if (!pointer.has_value()) {
        static_cast<void>(data_out.value()->close());
        return std::unexpected(std::move(pointer.error()));
    }
    if (auto written = meta_out.write_int(num_docs); !written.has_value()) {
        static_cast<void>(data_out.value()->close());
        return written;
    }
    if (auto written = meta_out.write_int(block_shift_); !written.has_value()) {
        static_cast<void>(data_out.value()->close());
        return written;
    }
    if (auto written = meta_out.write_int(total_chunks_ + 1); !written.has_value()) {
        static_cast<void>(data_out.value()->close());
        return written;
    }
    if (auto written = meta_out.write_long(static_cast<int64_t>(pointer.value())); !written.has_value()) {
        static_cast<void>(data_out.value()->close());
        return written;
    }

    // First the chunk start doc ids, read back from the temporary file.
    {
        auto docs_in = dir_.open_checksum_input(docs_name);
        if (!docs_in.has_value()) {
            static_cast<void>(data_out.value()->close());
            return std::unexpected(std::move(docs_in.error()));
        }
        auto checked = CodecUtil::check_header(*docs_in.value(), codec_name_ + "Docs", VERSION_CURRENT,
                                               VERSION_CURRENT);
        if (!checked.has_value()) {
            static_cast<void>(docs_in.value()->close());
            static_cast<void>(data_out.value()->close());
            return std::unexpected(std::move(checked.error()));
        }
        auto docs = DirectMonotonicWriter::get_instance(meta_out, *data_out.value(), total_chunks_ + 1,
                                                        block_shift_);
        if (!docs.has_value()) {
            static_cast<void>(docs_in.value()->close());
            static_cast<void>(data_out.value()->close());
            return std::unexpected(std::move(docs.error()));
        }
        int64_t doc = 0;
        if (auto added = docs.value().add(doc); !added.has_value()) {
            static_cast<void>(docs_in.value()->close());
            static_cast<void>(data_out.value()->close());
            return added;
        }
        for (int32_t chunk = 0; chunk < total_chunks_; ++chunk) {
            auto num_docs_in_chunk = docs_in.value()->read_vint();
            if (!num_docs_in_chunk.has_value()) {
                static_cast<void>(docs_in.value()->close());
                static_cast<void>(data_out.value()->close());
                return std::unexpected(std::move(num_docs_in_chunk.error()));
            }
            doc += num_docs_in_chunk.value();
            if (auto added = docs.value().add(doc); !added.has_value()) {
                static_cast<void>(docs_in.value()->close());
                static_cast<void>(data_out.value()->close());
                return added;
            }
        }
        if (auto finished = docs.value().finish(); !finished.has_value()) {
            static_cast<void>(docs_in.value()->close());
            static_cast<void>(data_out.value()->close());
            return finished;
        }
        if (doc != total_docs_) {
            static_cast<void>(docs_in.value()->close());
            static_cast<void>(data_out.value()->close());
            return err(LuceneError::corrupt_index("the chunk document counts do not add up"));
        }
        auto footer = CodecUtil::check_footer(*docs_in.value());
        auto closed = docs_in.value()->close();
        if (!footer.has_value()) {
            static_cast<void>(data_out.value()->close());
            return std::unexpected(std::move(footer.error()));
        }
        if (!closed.has_value()) {
            static_cast<void>(data_out.value()->close());
            return closed;
        }
    }

    if (auto deleted = dir_.delete_file(docs_name); !deleted.has_value()) {
        static_cast<void>(data_out.value()->close());
        return deleted;
    }
    docs_out_pending_delete_ = false;

    pointer = data_out.value()->get_file_pointer();
    if (!pointer.has_value()) {
        static_cast<void>(data_out.value()->close());
        return std::unexpected(std::move(pointer.error()));
    }
    if (auto written = meta_out.write_long(static_cast<int64_t>(pointer.value())); !written.has_value()) {
        static_cast<void>(data_out.value()->close());
        return written;
    }

    // Then the chunk start pointers.
    {
        auto pointers_in = dir_.open_checksum_input(file_pointers_name);
        if (!pointers_in.has_value()) {
            static_cast<void>(data_out.value()->close());
            return std::unexpected(std::move(pointers_in.error()));
        }
        auto checked = CodecUtil::check_header(*pointers_in.value(), codec_name_ + "FilePointers",
                                               VERSION_CURRENT, VERSION_CURRENT);
        if (!checked.has_value()) {
            static_cast<void>(pointers_in.value()->close());
            static_cast<void>(data_out.value()->close());
            return std::unexpected(std::move(checked.error()));
        }
        auto pointers = DirectMonotonicWriter::get_instance(meta_out, *data_out.value(), total_chunks_ + 1,
                                                            block_shift_);
        if (!pointers.has_value()) {
            static_cast<void>(pointers_in.value()->close());
            static_cast<void>(data_out.value()->close());
            return std::unexpected(std::move(pointers.error()));
        }
        int64_t file_pointer = 0;
        for (int32_t chunk = 0; chunk < total_chunks_; ++chunk) {
            auto delta = pointers_in.value()->read_vlong();
            if (!delta.has_value()) {
                static_cast<void>(pointers_in.value()->close());
                static_cast<void>(data_out.value()->close());
                return std::unexpected(std::move(delta.error()));
            }
            file_pointer += delta.value();
            if (auto added = pointers.value().add(file_pointer); !added.has_value()) {
                static_cast<void>(pointers_in.value()->close());
                static_cast<void>(data_out.value()->close());
                return added;
            }
        }
        if (static_cast<int64_t>(max_pointer) < file_pointer) {
            static_cast<void>(pointers_in.value()->close());
            static_cast<void>(data_out.value()->close());
            return err(LuceneError::corrupt_index("the chunk pointers do not add up"));
        }
        if (auto added = pointers.value().add(static_cast<int64_t>(max_pointer)); !added.has_value()) {
            static_cast<void>(pointers_in.value()->close());
            static_cast<void>(data_out.value()->close());
            return added;
        }
        if (auto finished = pointers.value().finish(); !finished.has_value()) {
            static_cast<void>(pointers_in.value()->close());
            static_cast<void>(data_out.value()->close());
            return finished;
        }
        auto footer = CodecUtil::check_footer(*pointers_in.value());
        auto closed = pointers_in.value()->close();
        if (!footer.has_value()) {
            static_cast<void>(data_out.value()->close());
            return std::unexpected(std::move(footer.error()));
        }
        if (!closed.has_value()) {
            static_cast<void>(data_out.value()->close());
            return closed;
        }
    }

    if (auto deleted = dir_.delete_file(file_pointers_name); !deleted.has_value()) {
        static_cast<void>(data_out.value()->close());
        return deleted;
    }
    file_pointers_out_pending_delete_ = false;

    pointer = data_out.value()->get_file_pointer();
    if (!pointer.has_value()) {
        static_cast<void>(data_out.value()->close());
        return std::unexpected(std::move(pointer.error()));
    }
    if (auto written = meta_out.write_long(static_cast<int64_t>(pointer.value())); !written.has_value()) {
        static_cast<void>(data_out.value()->close());
        return written;
    }
    if (auto written = meta_out.write_long(static_cast<int64_t>(max_pointer)); !written.has_value()) {
        static_cast<void>(data_out.value()->close());
        return written;
    }
    if (auto footer = CodecUtil::write_footer(*data_out.value()); !footer.has_value()) {
        static_cast<void>(data_out.value()->close());
        return footer;
    }

    auto closed = data_out.value()->close();
    if (!closed.has_value()) {
        return closed;
    }
    return {};
}

VoidResult Lucene90FieldsIndexWriter::close_temp_outputs(bool temp_outputs_closed,
                                                        bool docs_out_pending_delete,
                                                        bool file_pointers_out_pending_delete) {
    VoidResult close_result{};
    if (!temp_outputs_closed) {
        auto docs_closed = docs_out_->close();
        auto pointers_closed = file_pointers_out_->close();
        close_result = docs_closed.has_value() ? pointers_closed : docs_closed;
    }

    VoidResult delete_result{};
    if (docs_out_pending_delete) {
        delete_result = dir_.delete_file(docs_out_->get_name());
    }
    if (delete_result.has_value() && file_pointers_out_pending_delete) {
        delete_result = dir_.delete_file(file_pointers_out_->get_name());
    }

    if (!close_result.has_value()) {
        return close_result;
    }
    return delete_result;
}

VoidResult Lucene90FieldsIndexWriter::close() {
    if (!docs_out_pending_delete_ && !file_pointers_out_pending_delete_) {
        return {};
    }
    auto cleaned = close_temp_outputs(temp_outputs_closed_, docs_out_pending_delete_,
                                     file_pointers_out_pending_delete_);
    docs_out_pending_delete_ = false;
    file_pointers_out_pending_delete_ = false;
    temp_outputs_closed_ = true;
    return cleaned;
}

} // namespace lucene
