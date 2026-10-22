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

export module lucene_core:version;

import :types;
import :error;

import std;

namespace lucene {

/// A Lucene release, as `major.minor.bugfix`.
///
/// Port of `util/version.rs`.  Only the parts the index formats need are here:
/// the segment metadata stores a creation version and a minimum version, and
/// both are written as three big-endian ints.
export struct Version {
    int32_t major{0};
    int32_t minor{0};
    int32_t bug_fix{0};

    /// Validates the three components, rejecting negative values.
    [[nodiscard]] static Result<Version> from_bits(int32_t major, int32_t minor, int32_t bug_fix) {
        if (major < 0 || minor < 0 || bug_fix < 0) {
            return err(LuceneError::illegal_argument("version components must not be negative: " +
                                                     std::to_string(major) + "." + std::to_string(minor) + "." +
                                                     std::to_string(bug_fix)));
        }
        return Version{major, minor, bug_fix};
    }

    [[nodiscard]] std::string to_string() const {
        return std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(bug_fix);
    }

    [[nodiscard]] bool on_or_after(const Version &other) const { return *this >= other; }

    auto operator<=>(const Version &) const = default;
};

/// Version of the index format this port writes.
///
/// The formats here are the Lucene 10.1 ones; a segment written by this library
/// declares this version, which is what tools use to decide whether a segment
/// needs upgrading.
export constexpr Version LUCENE_CURRENT_VERSION{10, 1, 0};

/// The oldest segment version this port can still read.
export constexpr Version LUCENE_MIN_SUPPORTED_VERSION{7, 0, 0};

} // namespace lucene
