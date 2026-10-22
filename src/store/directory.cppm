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

export module lucene_core:directory;

import :types;
import :error;

import std;

import :index_input;
import :index_output;
import :io_context;
import :lock;

namespace lucene {

/// A newly created file plus its name (`create_temp_output` result).
export struct CreatedOutput {
    std::string name;
    std::unique_ptr<IndexOutput> output;
};

/// A file system abstraction used by the index codecs.
///
/// Port of the `Directory` trait (`core/store/directory.rs`), minus the
/// type-erasure machinery (`ErasedDirectory`, `IndexInputEnum`), which C++
/// virtual dispatch makes unnecessary.
///
/// Implementations must be safe for concurrent use from multiple threads.
export class Directory {
public:
    virtual ~Directory() = default;

    /// All file names in this directory, sorted.
    [[nodiscard]] virtual Result<std::vector<std::string>> list_all() const = 0;

    /// Removes `name`.  Returns an error when the file does not exist.
    [[nodiscard]] virtual VoidResult delete_file(std::string_view name) = 0;

    /// Length of `name` in bytes.
    [[nodiscard]] virtual Result<size_t> file_length(std::string_view name) const = 0;

    /// Creates `name` for writing, overwriting any existing file.
    [[nodiscard]] virtual Result<std::unique_ptr<IndexOutput>> create_output(std::string_view name,
                                                                             const IOContext &context) = 0;

    /// Creates a uniquely named file for writing.
    [[nodiscard]] virtual Result<CreatedOutput> create_temp_output(std::string_view prefix,
                                                                   std::string_view suffix,
                                                                   const IOContext &context) = 0;

    /// Forces the listed files to stable storage.
    [[nodiscard]] virtual VoidResult sync(std::span<const std::string> names) = 0;

    /// Forces the directory metadata (the file names) to stable storage.
    [[nodiscard]] virtual VoidResult sync_metadata() { return {}; }

    /// Atomically renames `source` to `dest`.
    [[nodiscard]] virtual VoidResult rename(std::string_view source, std::string_view dest) = 0;

    /// Opens `name` for reading.
    [[nodiscard]] virtual Result<std::unique_ptr<IndexInput>> open_input(std::string_view name,
                                                                         const IOContext &context) const = 0;

    /// Obtains an exclusive lock named `name`.
    [[nodiscard]] virtual Result<std::unique_ptr<Lock>> obtain_lock(std::string_view name) = 0;

    /// Copies `src` of `from` into the file `dest` of this directory.
    [[nodiscard]] virtual VoidResult copy_from(const Directory &from,
                                               std::string_view src,
                                               std::string_view dest,
                                               const IOContext &context);

    /// `true` for real file system directories (used by `NRTCachingDirectory`).
    [[nodiscard]] virtual bool is_fs_directory() const { return false; }

    [[nodiscard]] virtual std::string to_string() const = 0;

    // ---- convenience overloads ----

    [[nodiscard]] Result<std::unique_ptr<IndexOutput>> create_output(std::string_view name) {
        return create_output(name, io_context_default());
    }
    [[nodiscard]] Result<std::unique_ptr<IndexInput>> open_input(std::string_view name) const {
        return open_input(name, io_context_default());
    }
};

} // namespace lucene
