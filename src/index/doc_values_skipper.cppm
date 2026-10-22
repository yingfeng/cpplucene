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

export module lucene_core:doc_values_skipper;

import :types;
import std;

import :doc_id_set_iterator;
import :error;

namespace lucene {

/// Skips ranges of documents whose values cannot match a query.
///
/// Port of the `DocValuesSkipper` trait (`core/index/doc_values_skipper.rs`): the
/// position of a skipper can only move forward through [`advance`], and its
/// current interval is described per level — level `0` is the finest.  A level
/// reports the doc-id range, the value range and the number of documents that
/// have a value, which lets a range query jump over whole blocks.
export class DocValuesSkipper {
public:
    virtual ~DocValuesSkipper() = default;

    /// Moves so that every level contains the next document on or after `target`.
    ///
    /// Undefined when `target` is less than or equal to [`max_doc_id`] of level 0.
    [[nodiscard]] virtual VoidResult advance(int32_t target) = 0;

    /// Number of levels; it may change from one interval to the next.
    [[nodiscard]] virtual size_t num_levels() const = 0;

    /// First doc id of the interval at `level`, inclusive; `-1` before the first
    /// [`advance`] and [`NO_MORE_DOCS`] once exhausted.
    [[nodiscard]] virtual int32_t min_doc_id(size_t level) const = 0;

    /// Last doc id of the interval at `level`, inclusive.
    [[nodiscard]] virtual int32_t max_doc_id(size_t level) const = 0;

    /// Lower bound of the values in the interval at `level`.
    [[nodiscard]] virtual int64_t min_value(size_t level) const = 0;

    /// Upper bound of the values in the interval at `level`.
    [[nodiscard]] virtual int64_t max_value(size_t level) const = 0;

    /// Number of documents with a value in the interval at `level`.
    [[nodiscard]] virtual int32_t doc_count(size_t level) const = 0;

    /// Lower bound of all values in the field.
    [[nodiscard]] virtual int64_t global_min_value() const = 0;

    /// Upper bound of all values in the field.
    [[nodiscard]] virtual int64_t global_max_value() const = 0;

    /// Number of documents in the field that have a value.
    [[nodiscard]] virtual int32_t global_doc_count() const = 0;

    /// Advances so that all levels intersect `[min_value, max_value]`, skipping as
    /// many documents as the coarser levels allow.
    [[nodiscard]] virtual VoidResult advance_with_range(int64_t min_value, int64_t max_value) {
        if (min_doc_id(0) == -1) {
            if (auto advanced = advance(0); !advanced.has_value()) {
                return advanced;
            }
        }
        while (min_doc_id(0) != NO_MORE_DOCS && (this->min_value(0) > max_value || this->max_value(0) < min_value)) {
            int32_t max_doc = max_doc_id(0);
            size_t next_level = 1;
            while (next_level < num_levels() &&
                   (this->min_value(next_level) > max_value || this->max_value(next_level) < min_value)) {
                max_doc = max_doc_id(next_level);
                ++next_level;
            }
            if (auto advanced = advance(max_doc + 1); !advanced.has_value()) {
                return advanced;
            }
        }
        return {};
    }
};

} // namespace lucene
