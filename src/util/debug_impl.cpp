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

module lucene_core:debug.impl;

import :debug;
import :types;

import std;

namespace lucene {

void report_assertion_failure(std::string_view expression, const std::source_location &location) {
    const std::string message = std::format("lucene_core assertion failed: {} at {}:{}:{}",
                                            expression.empty() ? "<no expression>" : expression,
                                            location.file_name(),
                                            location.line(),
                                            location.column());
    std::cerr << message << '\n';
    std::cerr.flush();
    std::abort();
}

} // namespace lucene
