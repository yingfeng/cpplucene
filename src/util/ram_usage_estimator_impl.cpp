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

module lucene_core:ram_usage_estimator.impl;

import :ram_usage_estimator;
import :types;
import std;

namespace lucene {

namespace {

/// `{:.1}` with a trailing `.0` removed, the reference's `format_with_one_decimal`.
std::string format_with_one_decimal(double value) {
    std::string formatted = std::format("{:.1f}", value);
    if (formatted.ends_with(".0")) {
        formatted.resize(formatted.size() - 2);
    }
    return formatted;
}

} // namespace

std::string human_readable_units(int64_t bytes) {
    if (bytes / ONE_GB > 0) {
        return format_with_one_decimal(static_cast<double>(bytes) / static_cast<double>(ONE_GB)) + " GB";
    }
    if (bytes / ONE_MB > 0) {
        return format_with_one_decimal(static_cast<double>(bytes) / static_cast<double>(ONE_MB)) + " MB";
    }
    if (bytes / ONE_KB > 0) {
        return format_with_one_decimal(static_cast<double>(bytes) / static_cast<double>(ONE_KB)) + " KB";
    }
    return std::to_string(bytes) + " bytes";
}

} // namespace lucene
