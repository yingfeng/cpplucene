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

export module lucene_core:print_stream_info_stream;

import :info_stream;
import :types;

import std;

namespace lucene {

/// An [`InfoStream`] that writes one line per message to a `std::ostream`.
///
/// Port of `util/print_stream_info_stream.rs`.  The line format is
/// `<component> <message id> [<timestamp>; <thread>]: <message>`, where the
/// timestamp is ISO-8601 in UTC.
///
/// **Deviations:**
///  * the stream is **borrowed**, not owned: there is no `stdout()`/`stderr()`
///    factory and no `is_system_stream()`, because a `std::ostream` has no
///    ownership model to mirror and closing it is the caller's business;
///  * C++ has no thread names, so the thread column shows the
///    implementation-defined `std::thread::id`.
export class PrintStreamInfoStream final : public InfoStream {
public:
    /// Writes to `stream`, using a process wide message id.
    explicit PrintStreamInfoStream(std::ostream &stream);

    /// Writes to `stream`, using `message_id`.
    PrintStreamInfoStream(std::ostream &stream, int32_t message_id);

    [[nodiscard]] VoidResult message(std::string_view component, std::string_view text) const override;

    /// Always `true`, like the reference implementation.
    [[nodiscard]] bool is_enabled(std::string_view /*component*/) const override { return true; }

    /// Flushes the stream; the stream itself stays open.
    [[nodiscard]] VoidResult close() const override;

    [[nodiscard]] int32_t message_id() const noexcept { return message_id_; }

    /// The current UTC time, formatted for the log line.
    [[nodiscard]] static std::string get_timestamp();

private:
    mutable std::mutex mutex_;
    std::ostream *stream_;
    int32_t message_id_;
    mutable bool closed_{false};
};

} // namespace lucene
