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

export module lucene_core:nrt_caching_directory;

import :byte_buffers_directory;
import :directory;
import :error;
import :index_input;
import :index_output;
import :io_context;
import :lock;
import :types;

import std;

namespace lucene {

/// Wraps a RAM-resident directory around a delegate directory, for near-real-time
/// search.
///
/// Port of `store/nrt_caching_directory.rs`.  Newly flushed segments and merges
/// whose estimated size is at most `max_merge_size_mb` are written to RAM, as
/// long as the net cached bytes stay below `max_cached_mb`; when
/// [`sync`](Self::sync) is called (which `IndexWriter::commit` does) the cached
/// files are copied to the delegate first.
///
/// **Deviations:**
///  * the reference implementation tracks the cache size with a hook on the RAM
///    output; this port recomputes it from the cached files, which is equivalent
///    once a file is closed and avoids owning an output wrapper;
///  * `Directory` is not `Closeable` here, so [`close`](Self::close) is a plain
///    method: it flushes the cache and drops it, but does not close the delegate
///    (the caller owns it);
///  * the test-only `AssertCacheWrite` hook is not ported.
export class NRTCachingDirectory final : public Directory {
public:
    using Directory::create_output;
    using Directory::open_input;

    /// Caches a flush or merge whose estimated size is at most `max_merge_size_mb`,
    /// while the cache holds at most `max_cached_mb`.
    NRTCachingDirectory(std::unique_ptr<Directory> delegate, double max_merge_size_mb, double max_cached_mb);

    NRTCachingDirectory(const NRTCachingDirectory &) = delete;
    NRTCachingDirectory &operator=(const NRTCachingDirectory &) = delete;
    ~NRTCachingDirectory() override = default;

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
    [[nodiscard]] bool is_fs_directory() const override;
    [[nodiscard]] std::string to_string() const override;

    /// The files currently held in RAM.
    [[nodiscard]] Result<std::vector<std::string>> list_cached_files() const;

    /// Bytes currently held in RAM.
    [[nodiscard]] Result<int64_t> cached_bytes() const;

    /// Files are written to the cache when the flush or merge context estimates
    /// them small enough; everything else goes straight to the delegate.
    [[nodiscard]] bool do_cache_write(std::string_view name, const IOContext &context) const;

    /// Copies every cached file to the delegate and empties the cache.
    [[nodiscard]] VoidResult close();

    [[nodiscard]] const Directory &delegate() const noexcept { return *delegate_; }
    [[nodiscard]] const Directory &cache_directory() const noexcept { return *cache_; }

private:
    [[nodiscard]] bool cache_file_exists(std::string_view name) const;
    /// Copies `name` from the cache to the delegate and drops it from the cache.
    [[nodiscard]] VoidResult un_cache(std::string_view name);

    std::unique_ptr<Directory> delegate_;
    std::unique_ptr<ByteBuffersDirectory> cache_;
    mutable std::mutex mutex_;
    int64_t max_merge_size_bytes_;
    int64_t max_cached_bytes_;
    mutable bool closed_{false};
};

} // namespace lucene
