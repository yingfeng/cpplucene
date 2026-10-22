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

export module lucene_core:debug;

import :types;

import std;

namespace lucene {

/// Reports a failed assertion and aborts.  Only called from `debug_check`.
export void report_assertion_failure(std::string_view expression, const std::source_location &location);

/// The `debug_assert!(..)` of the port.
///
/// Macros are not exported by modules, so the port uses this function instead of
/// `assert`.  It compiles away when `NDEBUG` is defined, which matches rlucene's
/// `debug_assert!` semantics.
export inline void debug_check(bool condition,
                               std::string_view expression = {},
                               const std::source_location &location = std::source_location::current()) {
#ifndef NDEBUG
    if (!condition) [[unlikely]] {
        report_assertion_failure(expression, location);
    }
#else
    (void)condition;
    (void)expression;
    (void)location;
#endif
}

} // namespace lucene
