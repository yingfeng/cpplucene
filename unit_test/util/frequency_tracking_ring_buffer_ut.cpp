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

module;

#include <gtest/gtest.h>

#include <cstdint>
#include <expected>
#include <string>

module lucene_core:ut.frequency_tracking_ring_buffer;

import lucene_core;

using namespace lucene;

TEST(FrequencyTrackingRingBufferTest, RejectsTooSmallBuffers) {
    auto buffer = FrequencyTrackingRingBuffer::create(1, 42);
    ASSERT_FALSE(buffer.has_value());
    EXPECT_EQ(buffer.error().kind(), ErrorKind::kIllegalArgument);
}

TEST(FrequencyTrackingRingBufferTest, StartsFullOfTheSentinel) {
    auto buffer = FrequencyTrackingRingBuffer::create(4, 7);
    ASSERT_TRUE(buffer.has_value()) << buffer.error().to_string();
    EXPECT_EQ(buffer.value().max_size(), 4U);
    EXPECT_EQ(buffer.value().frequency(7), 4);
    EXPECT_EQ(buffer.value().frequency(1), 0);
}

TEST(FrequencyTrackingRingBufferTest, EvictsTheOldestValues) {
    auto buffer = FrequencyTrackingRingBuffer::create(3, 0);
    ASSERT_TRUE(buffer.has_value());
    auto &ring = buffer.value();

    ring.add(1);
    ring.add(2);
    EXPECT_EQ(ring.frequency(1), 1);
    EXPECT_EQ(ring.frequency(2), 1);
    // Each add evicts one sentinel from the slot it wraps around to.
    EXPECT_EQ(ring.frequency(0), 1);

    ring.add(3);
    EXPECT_EQ(ring.frequency(1), 1);
    EXPECT_EQ(ring.frequency(2), 1);
    EXPECT_EQ(ring.frequency(3), 1);
    EXPECT_EQ(ring.frequency(0), 0);

    // Wrapping around replaces the oldest value, 1.
    ring.add(9);
    EXPECT_EQ(ring.frequency(1), 0);
    EXPECT_EQ(ring.frequency(9), 1);
    EXPECT_EQ(ring.frequency(2), 1);

    // Re-adding a value that is still in the buffer increases its frequency and
    // evicts the value of the slot it replaces.
    ring.add(3);
    EXPECT_EQ(ring.frequency(3), 2);
    EXPECT_EQ(ring.frequency(2), 0);
    EXPECT_EQ(ring.frequency(9), 1);
}

TEST(FrequencyTrackingRingBufferTest, CountsRepeatedValues) {
    auto buffer = FrequencyTrackingRingBuffer::create(16, -1);
    ASSERT_TRUE(buffer.has_value());
    auto &ring = buffer.value();
    for (int32_t i = 0; i < 5; ++i) {
        ring.add(11);
    }
    EXPECT_EQ(ring.frequency(11), 5);
    EXPECT_EQ(ring.frequency(12), 0);
}

TEST(IntBagTest, TracksFrequencies) {
    IntBag bag(8);
    EXPECT_EQ(bag.frequency(3), 0);
    EXPECT_EQ(bag.add(3), 1);
    EXPECT_EQ(bag.add(3), 2);
    EXPECT_EQ(bag.frequency(3), 2);
    EXPECT_EQ(bag.frequency(4), 0);

    EXPECT_TRUE(bag.remove(3));
    EXPECT_EQ(bag.frequency(3), 1);
    EXPECT_TRUE(bag.remove(3));
    EXPECT_EQ(bag.frequency(3), 0);
    // Nothing left to remove.
    EXPECT_FALSE(bag.remove(3));
}

TEST(IntBagTest, KeepsCollisionChainsReachableAfterRemoval) {
    IntBag bag(16);
    // The capacity is a power of two, so these keys collide in the low bits.
    const std::vector<int32_t> keys{0, 32, 64, 96, 1, 33};
    for (const int32_t key : keys) {
        bag.add(key);
    }
    for (const int32_t key : keys) {
        EXPECT_EQ(bag.frequency(key), 1) << "key " << key;
    }

    // Removing the head of a chain relocates the rest of it, without losing any
    // frequency.
    EXPECT_TRUE(bag.remove(0));
    EXPECT_TRUE(bag.remove(32));
    for (const int32_t key : keys) {
        EXPECT_EQ(bag.frequency(key), key == 0 || key == 32 ? 0 : 1) << "key " << key;
    }
}
