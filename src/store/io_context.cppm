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

export module lucene_core:io_context;

import :types;
import std;

import :error;

namespace lucene {

/// Advice regarding the expected read access pattern.  Port of `ReadAdvice`.
export enum class ReadAdvice : uint8_t {
    /// Normal behavior, data is expected to be read mostly sequentially.
    Normal = 0,
    /// Data is expected to be read in a random-access fashion.
    Random = 1,
    /// Data is expected to be read sequentially with very little seeking.
    Sequential = 2,
    /// Data is treated as random-access memory in practice (small files).
    RandomPreload = 3,
};

/// Parses `NORMAL`, `RANDOM`, `SEQUENTIAL`, `RANDOM_PRELOAD` (case insensitive).
export [[nodiscard]] Result<ReadAdvice> read_advice_from_str(std::string_view text);

/// `RANDOM` unless `apache.lucene.store.defaultReadAdvice` says otherwise.
export [[nodiscard]] Result<ReadAdvice> default_read_advice();

/// Port of `FlushInfo`.
export struct FlushInfo {
    int32_t num_docs{0};
    int64_t estimated_segment_size{0};

    FlushInfo() = default;
    FlushInfo(int32_t num_docs, int64_t estimated_segment_size)
        : num_docs(num_docs), estimated_segment_size(estimated_segment_size) {}

    [[nodiscard]] bool operator==(const FlushInfo &) const = default;
};

/// Port of `MergeInfo`.
export struct MergeInfo {
    int32_t total_max_doc{0};
    int64_t estimated_merge_bytes{0};
    bool is_external{false};
    int32_t merge_max_num_segments{0};

    MergeInfo() = default;
    MergeInfo(int32_t total_max_doc, int64_t estimated_merge_bytes, bool is_external, int32_t merge_max_num_segments)
        : total_max_doc(total_max_doc),
          estimated_merge_bytes(estimated_merge_bytes),
          is_external(is_external),
          merge_max_num_segments(merge_max_num_segments) {}

    [[nodiscard]] bool operator==(const MergeInfo &) const = default;
};

/// Port of `Context`.
export enum class IOContextType : uint8_t {
    /// Context for reads and writes that are associated with a merge.
    Merge = 0,
    /// Context for writes that are associated with a segment flush.
    Flush = 1,
    /// Default context, usable for reading or writing.
    Default = 2,
};

/// Holds additional details on the merge/search context.  Port of `IOContext`.
export class IOContext {
public:
    IOContext() = default;

    /// Validating constructor; mirrors `IOContext::new`.
    [[nodiscard]] static Result<IOContext> create(IOContextType context,
                                                  ReadAdvice read_advice,
                                                  std::optional<MergeInfo> merge_info,
                                                  std::optional<FlushInfo> flush_info);

    /// A default context with the given read advice.
    [[nodiscard]] static Result<IOContext> with_read_advice(ReadAdvice read_advice);
    /// A flush context.
    [[nodiscard]] static Result<IOContext> with_flush(FlushInfo flush_info);
    /// A merge context.
    [[nodiscard]] static Result<IOContext> with_merge(MergeInfo merge_info);
    /// The default read-once context (sequential).
    [[nodiscard]] static Result<IOContext> read_once_io_context();

    [[nodiscard]] IOContextType context() const noexcept { return context_; }
    [[nodiscard]] ReadAdvice read_advice() const noexcept { return read_advice_; }
    [[nodiscard]] const std::optional<MergeInfo> &merge_info() const noexcept { return merge_info_; }
    [[nodiscard]] const std::optional<FlushInfo> &flush_info() const noexcept { return flush_info_; }

    /// Replaces the read advice, but only for `Default` contexts.
    [[nodiscard]] Result<IOContext> with_read_advice_self(ReadAdvice read_advice) const;

    [[nodiscard]] bool operator==(const IOContext &other) const = default;

private:
    IOContextType context_{IOContextType::Default};
    ReadAdvice read_advice_{ReadAdvice::Random};
    std::optional<MergeInfo> merge_info_{};
    std::optional<FlushInfo> flush_info_{};
};

/// Default context for normal reads/writes (`IO_CONTEXT_DEFAULT`).
export [[nodiscard]] const IOContext &io_context_default();
/// Default context for read-once access (`IO_CONTEXT_READ_ONCE`).
export [[nodiscard]] const IOContext &io_context_read_once();

} // namespace lucene
