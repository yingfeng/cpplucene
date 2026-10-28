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

module lucene_core:query_timeout_impl.impl;

import :query_timeout_impl;
import :types;
import std;

import :query_timeout;

namespace lucene {

int64_t steady_clock_nanos() noexcept {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
}

QueryTimeoutImpl::QueryTimeoutImpl(int64_t time_allowed_ms) {
    if (time_allowed_ms < 0 || time_allowed_ms == std::numeric_limits<int64_t>::max()) {
        return;
    }
    const int64_t nanos = time_allowed_ms * 1'000'000;
    timeout_at_ = steady_clock_nanos() + nanos;
}

bool QueryTimeoutImpl::should_exit() const {
    if (!timeout_at_.has_value()) {
        return false;
    }
    return steady_clock_nanos() >= timeout_at_.value();
}

std::string QueryTimeoutImpl::to_string() const {
    if (!timeout_at_.has_value()) {
        return "QueryTimeoutImpl(disabled)";
    }
    return "QueryTimeoutImpl(timeoutAt=" + std::to_string(timeout_at_.value()) + ")";
}

} // namespace lucene
