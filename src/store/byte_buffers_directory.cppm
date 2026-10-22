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

export module lucene_core:byte_buffers_directory;

import :types;
import :error;

import std;

import :directory;
import :index_input;
import :index_output;
import :io_context;
import :lock;
import :lock_factory;

namespace lucene {

/// An in-memory [`Directory`]: every file is a byte buffer.
///
/// Port of `byte_buffers_directory.rs`.  A file only becomes visible once its
/// `IndexOutput` is closed, which is exactly when the codec has finished
/// writing it; this keeps the directory free of partially written files.
export class ByteBuffersDirectory : public Directory {
public:
    using Directory::create_output;
    using Directory::open_input;

    /// Uses a [`SingleInstanceLockFactory`], like Lucene's `ByteBuffersDirectory`.
    ByteBuffersDirectory();
    explicit ByteBuffersDirectory(std::unique_ptr<LockFactory> lock_factory);

    [[nodiscard]] Result<std::vector<std::string>> list_all() const override;
    [[nodiscard]] VoidResult delete_file(std::string_view name) override;
    [[nodiscard]] Result<size_t> file_length(std::string_view name) const override;
    [[nodiscard]] Result<std::unique_ptr<IndexOutput>> create_output(std::string_view name,
                                                                     const IOContext &context) override;
    [[nodiscard]] Result<CreatedOutput> create_temp_output(std::string_view prefix,
                                                           std::string_view suffix,
                                                           const IOContext &context) override;
    [[nodiscard]] VoidResult sync(std::span<const std::string> names) override;

    [[nodiscard]] VoidResult rename(std::string_view source, std::string_view dest) override;
    [[nodiscard]] Result<std::unique_ptr<IndexInput>> open_input(std::string_view name,
                                                                 const IOContext &context) const override;
    [[nodiscard]] Result<std::unique_ptr<Lock>> obtain_lock(std::string_view name) override;
    [[nodiscard]] std::string to_string() const override { return "ByteBuffersDirectory"; }

    /// Number of files currently in the directory.
    [[nodiscard]] size_t file_count() const;

    /// Total bytes held by all files (`ram_bytes_used`).
    [[nodiscard]] size_t ram_bytes_used() const;

private:
    using Buffer = std::shared_ptr<const std::vector<uint8_t>>;

    [[nodiscard]] Result<Buffer> buffer_for(std::string_view name) const;
    void commit(std::string name, std::vector<uint8_t> bytes);
    [[nodiscard]] std::string new_file_name(std::string_view prefix, std::string_view suffix);

    mutable std::mutex mutex_;
    std::map<std::string, Buffer, std::less<>> files_;
    std::unique_ptr<LockFactory> lock_factory_;
    std::atomic<int64_t> counter_{0};
};

} // namespace lucene
