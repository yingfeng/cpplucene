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
#include <string>
#include <vector>

module lucene_core:ut.block_tree_output_accumulator;

import lucene_core;

using namespace lucene;

namespace {

BytesRef bytes_ref(std::vector<uint8_t> bytes) {
    return BytesRef(std::move(bytes));
}

} // namespace

TEST(BlockTreeTermsConstantsTest, FlagsAndExtensionsMatchTheFormat) {
    EXPECT_EQ(OUTPUT_FLAGS_NUM_BITS, 2);
    EXPECT_EQ(OUTPUT_FLAGS_MASK, 0x3);
    EXPECT_EQ(OUTPUT_FLAG_IS_FLOOR, 0x1);
    EXPECT_EQ(OUTPUT_FLAG_HAS_TERMS, 0x2);

    EXPECT_EQ(TERMS_EXTENSION, "tim");
    EXPECT_EQ(TERMS_CODEC_NAME, "BlockTreeTermsDict");
    EXPECT_EQ(TERMS_INDEX_EXTENSION, "tip");
    EXPECT_EQ(TERMS_INDEX_CODEC_NAME, "BlockTreeTermsIndex");
    EXPECT_EQ(TERMS_META_EXTENSION, "tmd");
    EXPECT_EQ(TERMS_META_CODEC_NAME, "BlockTreeTermsMeta");

    // Only the current version is read and written.
    EXPECT_EQ(BLOCK_TREE_VERSION_CURRENT, BLOCK_TREE_VERSION_FST_CONTINUOUS_ARCS);
    EXPECT_EQ(BLOCK_TREE_VERSION_CURRENT, 2);
}

TEST(BlockTreeTermsConstantsTest, NoOutputIsEmpty) {
    EXPECT_EQ(no_output().length(), 0u);
    EXPECT_TRUE(no_output().as_slice().empty());
}

TEST(OutputAccumulatorTest, PushIgnoresEmptyOutputs) {
    OutputAccumulator accumulator;
    accumulator.push(no_output());
    EXPECT_EQ(accumulator.output_count(), 0u);

    accumulator.push(bytes_ref({0x01, 0x02}));
    accumulator.push(no_output());
    accumulator.push(bytes_ref({0x03}));
    EXPECT_EQ(accumulator.output_count(), 2u);
    EXPECT_EQ(accumulator.output_at(0), bytes_ref({0x01, 0x02}));
    EXPECT_EQ(accumulator.output_at(1), bytes_ref({0x03}));
}

TEST(OutputAccumulatorTest, PopAndPopNTrimTheLastOutputs) {
    OutputAccumulator accumulator;
    const BytesRef first = bytes_ref({0x10});
    const BytesRef second = bytes_ref({0x20, 0x21});
    accumulator.push(first);
    accumulator.push(second);
    EXPECT_EQ(accumulator.output_count(), 2u);

    accumulator.pop(second);
    EXPECT_EQ(accumulator.output_count(), 1u);

    // Popping the empty output is a no-op, like pushing it.
    accumulator.pop(no_output());
    EXPECT_EQ(accumulator.output_count(), 1u);

    accumulator.push(second);
    accumulator.pop_n(2);
    EXPECT_EQ(accumulator.output_count(), 0u);

    accumulator.reset();
    EXPECT_EQ(accumulator.output_count(), 0u);
}

TEST(OutputAccumulatorTest, ReadsAcrossThePushedOutputs) {
    OutputAccumulator accumulator;
    accumulator.push(bytes_ref({0x01, 0x02}));
    accumulator.push(bytes_ref({0x03}));
    accumulator.prepare_read();

    EXPECT_EQ(accumulator.read_byte().value(), 0x01);
    EXPECT_EQ(accumulator.read_byte().value(), 0x02);
    EXPECT_EQ(accumulator.read_byte().value(), 0x03);
    auto exhausted = accumulator.read_byte();
    ASSERT_FALSE(exhausted.has_value());
    EXPECT_EQ(exhausted.error().kind(), ErrorKind::kIllegalState);

    accumulator.prepare_read();
    EXPECT_EQ(accumulator.read_byte().value(), 0x01);
}

TEST(OutputAccumulatorTest, FloorDataIsTheUnreadTailOfTheLastOutput) {
    OutputAccumulator accumulator;
    accumulator.push(bytes_ref({0x01}));
    accumulator.push(bytes_ref({0x02, 0x03, 0x04}));
    accumulator.prepare_read();
    // Consume the first output and one byte of the last one.
    EXPECT_EQ(accumulator.read_byte().value(), 0x01);
    EXPECT_EQ(accumulator.read_byte().value(), 0x02);

    std::vector<uint8_t> floor_data;
    ASSERT_TRUE(accumulator.set_floor_data(floor_data).has_value());
    const std::vector<uint8_t> expected{0x03, 0x04};
    EXPECT_EQ(floor_data, expected);

    // set_floor_data does not move the cursor.
    EXPECT_EQ(accumulator.read_byte().value(), 0x03);
}
