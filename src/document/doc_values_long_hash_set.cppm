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

export module lucene_core:doc_values_long_hash_set;

import :types;
import std;

import :error;

namespace lucene {

/// The value that marks an empty slot of the hash table, and the value the set
/// tracks separately because it can't be hashed as a present value.
inline constexpr int64_t kDocValuesLongHashSetMissing = std::numeric_limits<int64_t>::min();

/// A set of `int64` values optimized for doc values usage.
///
/// Port of `DocValuesLongHashSet` (`core/document/doc_values_long_hash_set.rs`).
///
/// The table is an open-addressed hash set whose size is a power of two just
/// above `1.5 * values`, and `INT64_MIN` is tracked by a flag instead of by the
/// table, so that it can be told apart from an empty slot.
///
/// **Deviation:** the reference implementation's `Accountable` / `Display`
/// plumbing is reduced to [`to_string`]; the port has no accountability
/// framework.
export class DocValuesLongHashSet {
public:
    /// Builds the set from `values`, which must be sorted.
    [[nodiscard]] static Result<DocValuesLongHashSet> create(std::vector<int64_t> values);

    /// Whether `value` is in the set.
    [[nodiscard]] bool contains(int64_t value) const;

    /// The number of values, `INT64_MIN` included when it is present.
    [[nodiscard]] size_t size() const noexcept { return size_; }

    /// Whether `INT64_MIN` is in the set.
    [[nodiscard]] bool has_missing_value() const noexcept { return has_missing_value_; }

    /// The smallest value, `INT64_MAX` for an empty set.
    [[nodiscard]] int64_t min_value() const noexcept { return min_value_; }

    /// The largest value, `INT64_MIN` for an empty set.
    [[nodiscard]] int64_t max_value() const noexcept { return max_value_; }

    /// The hash table, for tests and diagnostics.
    [[nodiscard]] const std::vector<int64_t> &table() const noexcept { return table_; }

    /// `[value, value, ...]`, with a leading `INT64_MIN` when it is present.
    [[nodiscard]] std::string to_string() const;

private:
    DocValuesLongHashSet(std::vector<int64_t> table, size_t mask, bool has_missing_value, size_t size,
                         int64_t min_value, int64_t max_value);

    /// Inserts `value` into `table`; answers whether it was absent.
    [[nodiscard]] static bool add(std::vector<int64_t> &table, size_t mask, int64_t value);

    /// The hash the set uses, i.e. `Long.hashCode` from the reference
    /// implementation: the high and low words folded together.
    [[nodiscard]] static int32_t hash_code(int64_t value);

    std::vector<int64_t> table_;
    size_t mask_{0};
    bool has_missing_value_{false};
    size_t size_{0};
    int64_t min_value_{std::numeric_limits<int64_t>::max()};
    int64_t max_value_{std::numeric_limits<int64_t>::min()};
};

} // namespace lucene
