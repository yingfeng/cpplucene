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

export module lucene_core:query_timeout_impl;

import :types;
import std;

import :query_timeout;

namespace lucene {

/// A deadline based [`QueryTimeout`].
///
/// Port of `QueryTimeoutImpl` (`index/query_timeout_impl.rs`).
///
/// **Deviation:** the deadline is stored as a `std::chrono::steady_clock`
/// time point already reduced to nanoseconds, so [`should_exit`] is a clock read
/// and a comparison with no optional handling.
export class QueryTimeoutImpl final : public QueryTimeout {
public:
    /// Times out `time_allowed_ms` from now.  A negative value, and
    /// `INT64_MAX`, mean "never time out"; `0` means "exit immediately".
    explicit QueryTimeoutImpl(int64_t time_allowed_ms);

    /// `true` once the deadline has passed; never for a disabled timeout.
    [[nodiscard]] bool should_exit() const override;

    /// The deadline as nanoseconds on the steady clock, if there is one.
    [[nodiscard]] std::optional<int64_t> timeout_at_nanos() const noexcept { return timeout_at_; }

    /// Disables the timeout.
    void reset() noexcept { timeout_at_.reset(); }

    [[nodiscard]] std::string to_string() const;

private:
    std::optional<int64_t> timeout_at_;
};

/// The current steady clock reading in nanoseconds.
[[nodiscard]] int64_t steady_clock_nanos() noexcept;

} // namespace lucene
