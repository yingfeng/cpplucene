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

module lucene_core:max_score_accumulator.impl;

import :max_score_accumulator;

import :bit_util;
import :core_helper;
import :types;

import std;

namespace lucene {

namespace {

/// Packs `score` (high 32 bits) and `doc_id` (low 32 bits); both are
/// non-negative, so the packed value keeps its numeric order.
int64_t encode(int32_t doc_id, float score) {
    const auto score_bits = static_cast<int64_t>(static_cast<uint32_t>(BitUtil::float_to_int_bits(score)));
    return (score_bits << 32) | (static_cast<int64_t>(doc_id) & 0xFFFFFFFFLL);
}

} // namespace

int64_t MaxScoreAccumulator::max_encode(int64_t left, int64_t right) {
    const auto left_score = std::bit_cast<float>(static_cast<uint32_t>(left >> 32));
    const auto right_score = std::bit_cast<float>(static_cast<uint32_t>(right >> 32));
    const int32_t compared = CoreHelper::compare_f32(left_score, right_score);
    if (compared == 0) {
        // Tie break on the minimum document id.
        return static_cast<int32_t>(left) < static_cast<int32_t>(right) ? left : right;
    }
    return compared > 0 ? left : right;
}

void MaxScoreAccumulator::accumulate(int32_t doc_id, float score) {
    const int64_t encoded = encode(doc_id, score);
    int64_t previous = acc_.load(std::memory_order_relaxed);
    while (true) {
        const int64_t desired = max_encode(previous, encoded);
        if (desired == previous) {
            return;
        }
        // On failure `previous` is refreshed with the value that won, so the
        // maximum is recomputed from the current state.
        if (acc_.compare_exchange_weak(previous, desired, std::memory_order_relaxed, std::memory_order_relaxed)) {
            return;
        }
    }
}

float MaxScoreAccumulator::to_score(int64_t value) {
    return std::bit_cast<float>(static_cast<uint32_t>(value >> 32));
}

int32_t MaxScoreAccumulator::doc_id(int64_t value) { return static_cast<int32_t>(value); }

} // namespace lucene
