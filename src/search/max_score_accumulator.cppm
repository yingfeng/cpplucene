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

export module lucene_core:max_score_accumulator;

import :types;

import std;

namespace lucene {

/// Keeps the maximum score and its document id, concurrently.
///
/// Port of `MaxScoreAccumulator` (`core/search/max_score_accumulator.rs`).
///
/// The pair is packed into one `int64_t` (score bits in the high half, document
/// id in the low half) so a single atomic compare-and-swap can update both,
/// which is what lets leaf partitions skip non-competitive documents without
/// locking.
export class MaxScoreAccumulator {
public:
    /// How often a leaf collector re-reads the accumulator: `2^10 - 1` so the
    /// check is a bitwise `and`.
    static constexpr size_t DEFAULT_INTERVAL = 0x3ff;

    MaxScoreAccumulator() = default;

    MaxScoreAccumulator(const MaxScoreAccumulator &) = delete;
    MaxScoreAccumulator &operator=(const MaxScoreAccumulator &) = delete;

    /// Records `score` for `doc_id` when it beats the current maximum.
    void accumulate(int32_t doc_id, float score);

    /// The score packed in `value`.
    [[nodiscard]] static float to_score(int64_t value);

    /// The document id packed in `value`.
    [[nodiscard]] static int32_t doc_id(int64_t value);

    /// The current packed maximum; `INT64_MIN` when nothing was accumulated.
    [[nodiscard]] int64_t get_raw() const noexcept { return acc_.load(std::memory_order_relaxed); }

    /// The interval at which collectors should re-read the accumulator.
    [[nodiscard]] size_t mod_interval() const noexcept { return mod_interval_; }

private:
    /// The greater of two packed values, preferring the smaller document id on a
    /// score tie.
    [[nodiscard]] static int64_t max_encode(int64_t left, int64_t right);

    std::atomic<int64_t> acc_{std::numeric_limits<int64_t>::min()};
    size_t mod_interval_{DEFAULT_INTERVAL};
};

} // namespace lucene
