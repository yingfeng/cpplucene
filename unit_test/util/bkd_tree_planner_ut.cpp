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
#include <vector>

module lucene_core:ut.bkd_tree_planner;

import lucene_core;

using namespace lucene;

namespace {

BKDConfig config_of(size_t dims, size_t index_dims) {
    auto config = BKDConfig::create(dims, index_dims, 4, 512);
    EXPECT_TRUE(config.has_value());
    return config.value();
}

std::vector<uint8_t> pack2(int32_t dim0, int32_t dim1) {
    std::vector<uint8_t> packed(8, 0);
    BitUtil::set_i32_be(packed, 0, dim0);
    BitUtil::set_i32_be(packed, 4, dim1);
    return packed;
}

} // namespace

TEST(BkdTreePlannerTest, SplitsUntilEveryBlockFits) {
    const BKDConfig config = config_of(1, 1);
    HeapPointWriter writer(config, 100);
    // Deliberately unsorted so the planner has to rearrange the block.
    for (int32_t i = 0; i < 100; ++i) {
        std::vector<uint8_t> packed(4, 0);
        BitUtil::set_i32_be(packed, 0, (i * 7) % 100);
        ASSERT_TRUE(writer.append_bytes(packed, i).has_value());
    }
    ASSERT_TRUE(writer.close().has_value());

    BkdTreePlanner planner(writer, 8);
    auto plan = planner.plan();
    ASSERT_TRUE(plan.has_value()) << (plan ? "" : plan.error().to_string());

    // ceil(100 / 8) = 13 leaves filled by quota: twelve of 8 points, the last 4.
    EXPECT_EQ(plan->leaves.size(), 13u);
    EXPECT_EQ(planner.max_leaf_count(), 8u);
    EXPECT_EQ(planner.split_count(), 12u);
    ASSERT_EQ(plan->splits.size(), 12u);

    size_t covered = 0;
    for (size_t i = 0; i < plan->leaves.size(); ++i) {
        EXPECT_LE(plan->leaves[i].count(), 8u);
        EXPECT_EQ(plan->leaves[i].from, covered);
        covered = plan->leaves[i].to;
    }
    EXPECT_EQ(covered, 100u);
}

TEST(BkdTreePlannerTest, LeavesAreOrderedByTheSplitDimension) {
    const BKDConfig config = config_of(1, 1);
    HeapPointWriter writer(config, 64);
    for (int32_t i = 0; i < 64; ++i) {
        std::vector<uint8_t> packed(8, 0);
        BitUtil::set_i32_be(packed, 0, 63 - i);
        ASSERT_TRUE(writer.append_bytes(std::span<const uint8_t>(packed.data(), 4), i).has_value());
    }
    ASSERT_TRUE(writer.close().has_value());

    BkdTreePlanner planner(writer, 4);
    auto leaves = planner.build();
    ASSERT_TRUE(leaves.has_value());
    ASSERT_EQ(leaves->size(), 16u);

    // Every leaf is internally sorted and the leaves are globally ordered, which
    // is what makes the packed index a valid binary search structure.
    int32_t previous = -1;
    for (const auto &leaf : *leaves) {
        for (size_t i = leaf.from; i < leaf.to; ++i) {
            const int32_t value = BitUtil::get_i32_be(writer.packed_value(i).packed_value(), 0);
            EXPECT_GT(value, previous) << "point " << i;
            previous = value;
        }
    }
    EXPECT_EQ(previous, 63);
}

TEST(BkdTreePlannerTest, SplitsConstantBlocksByQuota) {
    const BKDConfig config = config_of(2, 2);
    HeapPointWriter writer(config, 40);
    for (int32_t i = 0; i < 40; ++i) {
        ASSERT_TRUE(writer.append_bytes(pack2(5, 9), i).has_value());
    }
    ASSERT_TRUE(writer.close().has_value());

    // Every indexed dimension is constant, so no value separates the points — but
    // the on-disk tree shape is derived from the leaf count, so the block still
    // has to be split into ceil(40 / 8) = 5 leaves.
    BkdTreePlanner planner(writer, 8);
    auto plan = planner.plan();
    ASSERT_TRUE(plan.has_value());
    ASSERT_EQ(plan->leaves.size(), 5u);
    ASSERT_EQ(plan->splits.size(), 4u);
    EXPECT_EQ(planner.split_count(), 4u);

    for (const auto &split : plan->splits) {
        EXPECT_EQ(split.dim, 0u);  // choose_split_dim found nothing, dim 0 is the fallback
        EXPECT_EQ(split.value, std::vector<uint8_t>({0, 0, 0, 5}));
    }
}

TEST(BkdTreePlannerTest, UsesBothDimensions) {
    const BKDConfig config = config_of(2, 2);
    HeapPointWriter writer(config, 32);
    for (int32_t i = 0; i < 16; ++i) {
        ASSERT_TRUE(writer.append_bytes(pack2(i, 0), i).has_value());
        ASSERT_TRUE(writer.append_bytes(pack2(0, 1000 - i * 10), 16 + i).has_value());
    }
    ASSERT_TRUE(writer.close().has_value());

    BkdTreePlanner planner(writer, 4);
    const auto root_dim = planner.choose_split_dim(0, 32);
    ASSERT_TRUE(root_dim.has_value());
    // Dimension 0 spans 0..15, dimension 1 spans 850..1000: the wider one wins.
    EXPECT_EQ(root_dim.value(), 1u);

    auto leaves = planner.build();
    ASSERT_TRUE(leaves.has_value());
    ASSERT_EQ(leaves->size(), 8u);
    size_t covered = 0;
    for (const auto &leaf : *leaves) {
        covered += leaf.count();
    }
    EXPECT_EQ(covered, 32u);
}

TEST(BkdTreePlannerTest, LeftLeafCountsAreSemiBalanced) {
    // rlucene's get_num_left_leaf_nodes: complete levels are filled, leftovers go
    // left, and the left child never holds more than twice the right child.
    EXPECT_EQ(BkdTreePlanner::num_left_leaf_nodes(2), 1u);
    EXPECT_EQ(BkdTreePlanner::num_left_leaf_nodes(3), 2u);
    EXPECT_EQ(BkdTreePlanner::num_left_leaf_nodes(4), 2u);
    EXPECT_EQ(BkdTreePlanner::num_left_leaf_nodes(5), 3u);
    EXPECT_EQ(BkdTreePlanner::num_left_leaf_nodes(8), 4u);
    EXPECT_EQ(BkdTreePlanner::num_left_leaf_nodes(13), 8u);
    for (size_t leaves = 2; leaves <= 64; ++leaves) {
        const size_t left = BkdTreePlanner::num_left_leaf_nodes(leaves);
        EXPECT_GE(left, leaves - left) << leaves;
        EXPECT_LE(left, 2 * (leaves - left)) << leaves;
    }
}

TEST(BkdTreePlannerTest, PlanRecordsOneSplitPerLeafBoundary) {
    const BKDConfig config = config_of(1, 1);
    HeapPointWriter writer(config, 20);
    for (int32_t i = 0; i < 20; ++i) {
        std::vector<uint8_t> packed(4, 0);
        BitUtil::set_i32_be(packed, 0, i * 2);
        ASSERT_TRUE(writer.append_bytes(packed, i).has_value());
    }
    ASSERT_TRUE(writer.close().has_value());

    BkdTreePlanner planner(writer, 2);
    auto plan = planner.plan();
    ASSERT_TRUE(plan.has_value());
    ASSERT_EQ(plan->leaves.size(), 10u);
    ASSERT_EQ(plan->splits.size(), 9u);

    // splits[i] separates leaves[i] and leaves[i + 1], so its value lies between
    // the last point of the left leaf and the first point of the right one.
    for (size_t i = 0; i + 1 < plan->leaves.size(); ++i) {
        const int32_t left_value = BitUtil::get_i32_be(writer.packed_value(plan->leaves[i].to - 1).packed_value(), 0);
        const int32_t right_value = BitUtil::get_i32_be(writer.packed_value(plan->leaves[i + 1].from).packed_value(), 0);
        const int32_t split_value = BitUtil::get_i32_be(plan->splits[i].value, 0);
        EXPECT_EQ(plan->splits[i].dim, 0u);
        EXPECT_LE(left_value, split_value) << "boundary " << i;
        EXPECT_LE(split_value, right_value) << "boundary " << i;
    }
}

TEST(BkdTreePlannerTest, RejectsEmptyAndOpenWriters) {
    const BKDConfig config = config_of(1, 1);

    {
        HeapPointWriter writer(config, 4);
        BkdTreePlanner planner(writer, 2);
        EXPECT_FALSE(planner.build().has_value());  // still open
        EXPECT_FALSE(planner.plan().has_value());
        ASSERT_TRUE(writer.close().has_value());
        EXPECT_FALSE(planner.build().has_value());  // no points
    }
}
