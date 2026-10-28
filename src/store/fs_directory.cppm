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

export module lucene_core:fs_directory;

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

/// A [`Directory`] backed by a real directory, using `pread`/`pwrite` and
/// `fsync`.
///
/// Port of the behaviour of `nio_fs_directory.rs` / `fs_directory_base.rs`,
/// restricted to what the codecs need: buffered sequential writes, positional
/// reads, slicing, random access and `sync`.  Reads are memory mapped by default,
/// which is what the reference implementation's [`MMapDirectory`] does; the
/// buffered fallback covers files that cannot be mapped.
export class FSDirectory : public Directory {
public:
    using Directory::create_output;
    using Directory::open_input;

    /// Opens (creating it when missing) the directory at `path`.
    [[nodiscard]] static Result<std::unique_ptr<FSDirectory>> open(const std::filesystem::path &path);

    [[nodiscard]] Result<std::vector<std::string>> list_all() const override;
    [[nodiscard]] VoidResult delete_file(std::string_view name) override;
    [[nodiscard]] Result<size_t> file_length(std::string_view name) const override;
    [[nodiscard]] Result<std::unique_ptr<IndexOutput>> create_output(std::string_view name,
                                                                     const IOContext &context) override;
    [[nodiscard]] Result<CreatedOutput> create_temp_output(std::string_view prefix,
                                                           std::string_view suffix,
                                                           const IOContext &context) override;
    [[nodiscard]] VoidResult sync(std::span<const std::string> names) override;
    [[nodiscard]] VoidResult sync_metadata() override;

    [[nodiscard]] VoidResult rename(std::string_view source, std::string_view dest) override;
    [[nodiscard]] Result<std::unique_ptr<IndexInput>> open_input(std::string_view name,
                                                                 const IOContext &context) const override;
    [[nodiscard]] Result<std::unique_ptr<Lock>> obtain_lock(std::string_view name) override;
    [[nodiscard]] bool is_fs_directory() const override { return true; }
    [[nodiscard]] std::string to_string() const override { return std::format("FSDirectory({})", path_.string()); }

    [[nodiscard]] const std::filesystem::path &directory_path() const noexcept { return path_; }

    /// Whether `open_input` memory maps files (the default).
    ///
    /// Mapping is what the reference implementation does for its default file
    /// system directory.  Setting this to `false` falls back to a
    /// [`BufferedIndexInput`] over positional reads, which is far slower for
    /// random access but avoids keeping address space mapped.
    void set_use_mmap(bool value) noexcept { use_mmap_ = value; }
    [[nodiscard]] bool use_mmap() const noexcept { return use_mmap_; }

protected:
    explicit FSDirectory(std::filesystem::path path);

private:

    [[nodiscard]] std::filesystem::path file_path(std::string_view name) const;
    [[nodiscard]] std::string new_file_name(std::string_view prefix, std::string_view suffix);

    /// Opens `path` as a non-mapped, buffered input (`use_buffer` picks
    /// between cache sizes derived from `context`).
    [[nodiscard]] Result<std::unique_ptr<IndexInput>> open_buffered_input(std::string_view name,
                                                                         const IOContext &context) const;

    std::filesystem::path path_;
    std::unique_ptr<LockFactory> lock_factory_;
    std::atomic<int64_t> counter_{0};
    bool use_mmap_{true};
};

/// The file system directory under the name the reference implementation uses for
/// the mapping one.
///
/// Port of `MMapDirectory` (`core/store/mmap_directory.rs`).  [`FSDirectory`]
/// already memory maps every file it reads, so this class pins that behaviour:
/// it exists so a caller that asks for the reference's `MMapDirectory` gets the
/// mapping directory, and so `to_string` says which one was opened.
export class MMapDirectory final : public FSDirectory {
public:
    /// Opens (creating it when missing) the directory at `path`.
    [[nodiscard]] static Result<std::unique_ptr<MMapDirectory>> open(const std::filesystem::path &path);

    [[nodiscard]] std::string to_string() const override {
        return std::format("MMapDirectory({})", directory_path().string());
    }

private:
    explicit MMapDirectory(std::filesystem::path path) : FSDirectory(std::move(path)) { set_use_mmap(true); }
};

} // namespace lucene
