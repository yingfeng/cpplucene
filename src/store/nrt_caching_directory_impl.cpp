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

module lucene_core:nrt_caching_directory.impl;

import :nrt_caching_directory;

import std;

import :byte_buffers_directory;
import :directory;
import :error;
import :index_input;
import :index_output;
import :io_context;
import :lock;
import :types;

namespace lucene {

namespace {

constexpr double kBytesPerMb = 1024.0 * 1024.0;

} // namespace

NRTCachingDirectory::NRTCachingDirectory(std::unique_ptr<Directory> delegate, double max_merge_size_mb,
                                         double max_cached_mb)
    : delegate_(std::move(delegate)),
      cache_(std::make_unique<ByteBuffersDirectory>()),
      max_merge_size_bytes_(static_cast<int64_t>(max_merge_size_mb * kBytesPerMb)),
      max_cached_bytes_(static_cast<int64_t>(max_cached_mb * kBytesPerMb)) {}

bool NRTCachingDirectory::cache_file_exists(std::string_view name) const {
    return cache_->file_length(name).has_value();
}

Result<int64_t> NRTCachingDirectory::cached_bytes() const {
    auto files = cache_->list_all();
    if (!files.has_value()) {
        return std::unexpected(std::move(files.error()));
    }
    int64_t total = 0;
    for (const std::string &name : files.value()) {
        auto length = cache_->file_length(name);
        if (!length.has_value()) {
            continue; // The file disappeared between the listing and the lookup.
        }
        total += static_cast<int64_t>(length.value());
    }
    return total;
}

bool NRTCachingDirectory::do_cache_write(std::string_view /*name*/, const IOContext &context) const {
    int64_t bytes = 0;
    if (context.merge_info().has_value()) {
        bytes = context.merge_info().value().estimated_merge_bytes;
    } else if (context.flush_info().has_value()) {
        bytes = context.flush_info().value().estimated_segment_size;
    } else {
        return false;
    }
    auto cached = cached_bytes();
    if (!cached.has_value()) {
        return false;
    }
    return bytes <= max_merge_size_bytes_ && bytes + cached.value() <= max_cached_bytes_;
}

VoidResult NRTCachingDirectory::un_cache(std::string_view name) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (!cache_file_exists(name)) {
        // Another thread beat us to it.
        return {};
    }
    if (auto copied = delegate_->copy_from(*cache_, name, name, io_context_default()); !copied.has_value()) {
        return copied;
    }
    return cache_->delete_file(name);
}

Result<std::vector<std::string>> NRTCachingDirectory::list_cached_files() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return cache_->list_all();
}

Result<std::vector<std::string>> NRTCachingDirectory::list_all() const {
    std::vector<std::string> cache_files;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        auto listed = cache_->list_all();
        if (!listed.has_value()) {
            return std::unexpected(std::move(listed.error()));
        }
        cache_files = std::move(listed.value());
    }
    auto delegate_files = delegate_->list_all();
    if (!delegate_files.has_value()) {
        return std::unexpected(std::move(delegate_files.error()));
    }
    cache_files.insert(cache_files.end(), delegate_files.value().begin(), delegate_files.value().end());
    std::sort(cache_files.begin(), cache_files.end());
    cache_files.erase(std::unique(cache_files.begin(), cache_files.end()), cache_files.end());
    return cache_files;
}

VoidResult NRTCachingDirectory::delete_file(std::string_view name) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (cache_file_exists(name)) {
        return cache_->delete_file(name);
    }
    return delegate_->delete_file(name);
}

Result<size_t> NRTCachingDirectory::file_length(std::string_view name) const {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (cache_file_exists(name)) {
        return cache_->file_length(name);
    }
    return delegate_->file_length(name);
}

Result<std::unique_ptr<IndexOutput>> NRTCachingDirectory::create_output(std::string_view name,
                                                                        const IOContext &context) {
    if (do_cache_write(name, context)) {
        return cache_->create_output(name, context);
    }
    return delegate_->create_output(name, context);
}

Result<CreatedOutput> NRTCachingDirectory::create_temp_output(std::string_view prefix, std::string_view suffix,
                                                              const IOContext &context) {
    // The temporary name must be free in *both* directories.
    const bool cache_first = do_cache_write(prefix, context);
    Directory &first = cache_first ? static_cast<Directory &>(*cache_) : *delegate_;
    Directory &second = cache_first ? *delegate_ : static_cast<Directory &>(*cache_);

    auto created = first.create_temp_output(prefix, suffix, context);
    if (!created.has_value()) {
        return created;
    }
    while (second.file_length(created.value().name).has_value()) {
        // The other directory already has that name: drop ours and try again.
        const std::string conflicting = created.value().name;
        if (auto closed = created.value().output->close(); !closed.has_value()) {
            return std::unexpected(std::move(closed.error()));
        }
        if (auto deleted = first.delete_file(conflicting); !deleted.has_value()) {
            return std::unexpected(std::move(deleted.error()));
        }
        created = first.create_temp_output(prefix, suffix, context);
        if (!created.has_value()) {
            return created;
        }
    }
    return created;
}

VoidResult NRTCachingDirectory::sync(std::span<const std::string> names) {
    std::vector<std::string> cached;
    for (const std::string &name : names) {
        if (cache_file_exists(name)) {
            cached.push_back(name);
        }
    }
    for (const std::string &name : cached) {
        if (auto moved = un_cache(name); !moved.has_value()) {
            return moved;
        }
    }
    return delegate_->sync(names);
}

VoidResult NRTCachingDirectory::sync_metadata() { return delegate_->sync_metadata(); }

VoidResult NRTCachingDirectory::rename(std::string_view source, std::string_view dest) {
    if (auto moved = un_cache(source); !moved.has_value()) {
        return moved;
    }
    if (cache_file_exists(dest)) {
        return err(LuceneError::illegal_argument("target file " + std::string(dest) + " already exists"));
    }
    return delegate_->rename(source, dest);
}

Result<std::unique_ptr<IndexInput>> NRTCachingDirectory::open_input(std::string_view name,
                                                                    const IOContext &context) const {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (cache_file_exists(name)) {
        return cache_->open_input(name, context);
    }
    return delegate_->open_input(name, context);
}

Result<std::unique_ptr<Lock>> NRTCachingDirectory::obtain_lock(std::string_view name) {
    return delegate_->obtain_lock(name);
}

bool NRTCachingDirectory::is_fs_directory() const { return delegate_->is_fs_directory(); }

std::string NRTCachingDirectory::to_string() const {
    return std::format("NRTCachingDirectory({}; maxCacheMB={:g} maxMergeSizeMB={:g})", delegate_->to_string(),
                       static_cast<double>(max_cached_bytes_) / kBytesPerMb,
                       static_cast<double>(max_merge_size_bytes_) / kBytesPerMb);
}

VoidResult NRTCachingDirectory::close() {
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (closed_) {
            return {};
        }
        closed_ = true;
    }
    auto files = cache_->list_all();
    if (!files.has_value()) {
        return std::unexpected(std::move(files.error()));
    }
    for (const std::string &name : files.value()) {
        if (auto moved = un_cache(name); !moved.has_value()) {
            return moved;
        }
    }
    return {};
}

} // namespace lucene
