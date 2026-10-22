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

export module lucene_core:info_stream;

import :types;
import std;

import :close;
import :error;

namespace lucene {

/// Debugging API for Lucene components such as `IndexWriter` and
/// `SegmentInfos`.
///
/// Port of the `InfoStream` trait (`core/util/info_stream.rs`).
export class InfoStream : public CloseableRef {
public:
    ~InfoStream() override = default;

    /// Prints a message.
    [[nodiscard]] virtual VoidResult message(std::string_view component, std::string_view text) const = 0;

    /// `true` when messages are enabled and should be posted to [`message`].
    [[nodiscard]] virtual bool is_enabled(std::string_view component) const = 0;
};

/// An [`InfoStream`] that does no logging at all.
export class NoOutput : public InfoStream {
public:
    /// Messages must never be posted to a disabled stream; the call is accepted
    /// (and dropped) anyway so that a mis-behaving caller cannot crash.
    [[nodiscard]] VoidResult message(std::string_view /*component*/, std::string_view /*text*/) const override {
        return {};
    }

    [[nodiscard]] bool is_enabled(std::string_view /*component*/) const override { return false; }

    [[nodiscard]] std::string to_string() const { return "NoOutput"; }
};

/// The concrete [`InfoStream`] the index code holds: either [`NoOutput`] or a
/// user supplied stream, mirroring `InfoStreamEnum`.
export class InfoStreamEnum : public InfoStream {
public:
    /// `NoOutput` (rlucene's `Default`).
    InfoStreamEnum() = default;

    /// Wraps a custom stream.  A null pointer is treated like `NoOutput`.
    explicit InfoStreamEnum(std::shared_ptr<InfoStream> stream);

    [[nodiscard]] VoidResult message(std::string_view component, std::string_view text) const override;

    [[nodiscard]] bool is_enabled(std::string_view component) const override;

    [[nodiscard]] VoidResult close() const override;

    /// `true` when this instance wraps [`NoOutput`].
    [[nodiscard]] bool is_no_output() const noexcept { return std::holds_alternative<NoOutput>(inner_); }

    /// rlucene's `Debug` output (`NoOutput` / `CustomInfoStream`).
    [[nodiscard]] std::string to_string() const { return is_no_output() ? "InfoStreamEnum(NoOutput)" : "InfoStreamEnum(CustomInfoStream)"; }

private:
    std::variant<NoOutput, std::shared_ptr<InfoStream>> inner_{NoOutput{}};
};

/// Shared handle to an [`InfoStreamEnum`] (rlucene's `InfoStreamMT`).
export using InfoStreamMT = std::shared_ptr<InfoStreamEnum>;

/// The default [`InfoStream`] used by newly created types.
[[nodiscard]] InfoStreamMT get_default_info_stream();

/// Sets the default [`InfoStream`] used by newly created types.
void set_default_info_stream(InfoStreamMT info_stream);

/// Convenience overload for a custom stream.
void set_default_info_stream(std::shared_ptr<InfoStream> info_stream);

} // namespace lucene
