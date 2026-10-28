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

module;

#include <ctime>

module lucene_core:print_stream_info_stream.impl;

import :print_stream_info_stream;

import std;

import :info_stream;
import :types;

namespace lucene {

namespace {

/// One id per stream instance, like the reference implementation's atomic
/// counter.
int32_t next_message_id() {
    static std::atomic<int32_t> counter{0};
    return counter.fetch_add(1, std::memory_order_seq_cst);
}

/// `std::this_thread::get_id()` as text; C++ threads have no names.
std::string thread_name() {
    std::ostringstream out;
    out << std::this_thread::get_id();
    return out.str();
}

} // namespace

PrintStreamInfoStream::PrintStreamInfoStream(std::ostream &stream)
    : stream_(&stream), message_id_(next_message_id()) {}

PrintStreamInfoStream::PrintStreamInfoStream(std::ostream &stream, int32_t message_id)
    : stream_(&stream), message_id_(message_id) {}

VoidResult PrintStreamInfoStream::message(std::string_view component, std::string_view text) const {
    const std::string thread = thread_name();
    const std::string timestamp = get_timestamp();
    const std::lock_guard<std::mutex> lock(mutex_);
    if (closed_) {
        return {};
    }
    // I/O failures are recorded in the stream state instead of being reported,
    // matching the reference implementation's `println!` behaviour.
    *stream_ << component << ' ' << message_id_ << " [" << timestamp << "; " << thread << "]: " << text << '\n';
    stream_->flush();
    return {};
}

VoidResult PrintStreamInfoStream::close() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (closed_) {
        return {};
    }
    stream_->flush();
    closed_ = true;
    return {};
}

std::string PrintStreamInfoStream::get_timestamp() {
    const std::time_t now = std::time(nullptr);
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &now);
#else
    gmtime_r(&now, &utc);
#endif
    return std::format("{:04d}-{:02d}-{:02d}T{:02d}:{:02d}:{:02d}Z", utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
                       utc.tm_hour, utc.tm_min, utc.tm_sec);
}

} // namespace lucene
