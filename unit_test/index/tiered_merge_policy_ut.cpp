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
#include <memory>
#include <string>
#include <string_view>
#include <vector>

module lucene_core:ut.tiered_merge_policy;

import lucene_core;

using namespace lucene;

namespace {

constexpr size_t kSegmentBytes = 100 * 1024;

SegmentCommitInfo make_segment(std::string name, int32_t max_doc, int32_t del_count = 0) {
    SegmentCommitInfo commit;
    commit.info.files = {name + ".fdt"};
    commit.info.name = std::move(name);
    commit.info.id.assign(ID_LENGTH, 0x22);
    commit.info.max_doc = max_doc;
    commit.info.codec_name = "Lucene101";
    commit.del_count = del_count;
    return commit;
}

SegmentInfos make_infos(std::vector<SegmentCommitInfo> segments) {
    SegmentInfos infos;
    infos.segments = std::move(segments);
    return infos;
}

void write_file(Directory &dir, const std::string &name, size_t size) {
    auto out = dir.create_output(name, io_context_default());
    ASSERT_TRUE(out.has_value()) << (out ? "" : out.error().to_string());
    std::vector<uint8_t> bytes(size, 0x5A);
    ASSERT_TRUE(out.value()->write_bytes(bytes).has_value());
    ASSERT_TRUE(out.value()->close().has_value());
}

/// The segment names of every merge of `spec`, in order.
std::vector<std::vector<std::string>> merge_names(const std::optional<MergeSpecification> &spec) {
    std::vector<std::vector<std::string>> names;
    if (!spec.has_value()) {
        return names;
    }
    for (const OneMerge &merge : spec->merges()) {
        std::vector<std::string> group;
        for (const SegmentCommitInfo &segment : merge.segments()) {
            group.push_back(segment.info.name);
        }
        names.push_back(std::move(group));
    }
    return names;
}

/// A policy that measures exact sizes: no floor, no delete calibration surprises.
std::shared_ptr<TieredMergePolicy> small_tier_policy() {
    auto policy = TieredMergePolicy::create();
    EXPECT_TRUE(policy->set_floor_segment_mb(0.0).has_value());
    EXPECT_TRUE(policy->set_segments_per_tier(2.0).has_value());
    return std::shared_ptr<TieredMergePolicy>(std::move(policy));
}

} // namespace

TEST(TieredMergePolicyTest, Defaults) {
    auto policy = TieredMergePolicy::create();
    EXPECT_EQ(policy->max_merge_at_once(), 10);
    EXPECT_DOUBLE_EQ(policy->max_merged_segment_mb(), 5120.0);
    EXPECT_DOUBLE_EQ(policy->floor_segment_mb(), 2.0);
    EXPECT_DOUBLE_EQ(policy->segments_per_tier(), 10.0);
    EXPECT_DOUBLE_EQ(policy->deletes_pct_allowed(), 20.0);
    EXPECT_DOUBLE_EQ(policy->force_merge_deletes_pct_allowed(), 10.0);
    EXPECT_EQ(policy->target_search_concurrency(), 1);
    EXPECT_EQ(policy->max_full_flush_merge_size(), static_cast<int64_t>(2 * 1024 * 1024));
    EXPECT_NE(policy->to_string().find("TieredMergePolicy"), std::string::npos);
}

TEST(TieredMergePolicyTest, RejectsInvalidSettings) {
    auto policy = TieredMergePolicy::create();
    EXPECT_FALSE(policy->set_max_merge_at_once(1).has_value());
    EXPECT_FALSE(policy->set_segments_per_tier(1.5).has_value());
    EXPECT_FALSE(policy->set_deletes_pct_allowed(101.0).has_value());
    EXPECT_FALSE(policy->set_deletes_pct_allowed(-1.0).has_value());
    EXPECT_FALSE(policy->set_force_merge_deletes_pct_allowed(-1.0).has_value());
    EXPECT_FALSE(policy->set_max_merged_segment_mb(-1.0).has_value());
    EXPECT_FALSE(policy->set_floor_segment_mb(-1.0).has_value());
    EXPECT_FALSE(policy->set_target_search_concurrency(0).has_value());
}

TEST(TieredMergePolicyTest, MergesWhenOverBudget) {
    ByteBuffersDirectory dir;
    std::vector<SegmentCommitInfo> segments;
    for (int32_t i = 0; i < 6; ++i) {
        const std::string name = "_" + std::to_string(i);
        write_file(dir, name + ".fdt", kSegmentBytes);
        segments.push_back(make_segment(name, 10));
    }
    SegmentInfos infos = make_infos(std::move(segments));

    auto policy = small_tier_policy();
    SimpleMergeContext context(get_default_info_stream(), &dir);

    auto merged = policy->find_merges(MergeTrigger::SegmentFlush, infos, context);
    ASSERT_TRUE(merged.has_value()) << (merged ? "" : merged.error().to_string());
    ASSERT_TRUE(merged.value().has_value());
    // Segments are picked largest first (ties by name), so the two smallest of a
    // tier are merged together.
    EXPECT_EQ(merge_names(merged.value()), (std::vector<std::vector<std::string>>{{"_0", "_1"}}));
}

TEST(TieredMergePolicyTest, StaysWithinBudget) {
    ByteBuffersDirectory dir;
    std::vector<SegmentCommitInfo> segments;
    for (int32_t i = 0; i < 3; ++i) {
        const std::string name = "_" + std::to_string(i);
        write_file(dir, name + ".fdt", kSegmentBytes);
        segments.push_back(make_segment(name, 10));
    }
    SegmentInfos infos = make_infos(std::move(segments));

    auto policy = small_tier_policy();
    SimpleMergeContext context(get_default_info_stream(), &dir);

    auto merged = policy->find_merges(MergeTrigger::SegmentFlush, infos, context);
    ASSERT_TRUE(merged.has_value()) << (merged ? "" : merged.error().to_string());
    EXPECT_FALSE(merged.value().has_value());
}

TEST(TieredMergePolicyTest, MergesToReclaimDeletes) {
    ByteBuffersDirectory dir;
    // Two segments of 1000 documents with half of them deleted: the deletes are
    // above the 20% the policy tolerates, so the segments are rewritten.
    write_file(dir, "_0.fdt", kSegmentBytes);
    write_file(dir, "_1.fdt", kSegmentBytes);
    SegmentInfos infos = make_infos({make_segment("_0", 1000, 500), make_segment("_1", 1000, 500)});

    auto policy = small_tier_policy();
    SimpleMergeContext context(get_default_info_stream(), &dir);

    auto merged = policy->find_merges(MergeTrigger::SegmentFlush, infos, context);
    ASSERT_TRUE(merged.has_value()) << (merged ? "" : merged.error().to_string());
    ASSERT_TRUE(merged.value().has_value());
    EXPECT_EQ(merge_names(merged.value()), (std::vector<std::vector<std::string>>{{"_0", "_1"}}));
}

TEST(TieredMergePolicyTest, ForcedMergeToOneSegment) {
    ByteBuffersDirectory dir;
    std::vector<SegmentCommitInfo> segments;
    for (int32_t i = 0; i < 4; ++i) {
        const std::string name = "_" + std::to_string(i);
        write_file(dir, name + ".fdt", kSegmentBytes);
        segments.push_back(make_segment(name, 10));
    }
    SegmentInfos infos = make_infos(std::move(segments));

    auto policy = TieredMergePolicy::create();
    SimpleMergeContext context(get_default_info_stream(), &dir);

    auto merged = policy->find_forced_merges(infos, 1, context);
    ASSERT_TRUE(merged.has_value()) << (merged ? "" : merged.error().to_string());
    ASSERT_TRUE(merged.value().has_value());
    ASSERT_EQ(merged.value()->size(), 1U);
    EXPECT_EQ(merged.value()->merges()[0].size(), 4U);
}

TEST(TieredMergePolicyTest, ForcedMergeOfAnAlreadyMergedIndex) {
    ByteBuffersDirectory dir;
    write_file(dir, "_0.fdt", kSegmentBytes);
    SegmentInfos infos = make_infos({make_segment("_0", 10)});

    auto policy = TieredMergePolicy::create();
    SimpleMergeContext context(get_default_info_stream(), &dir);

    auto merged = policy->find_forced_merges(infos, 1, context);
    ASSERT_TRUE(merged.has_value()) << (merged ? "" : merged.error().to_string());
    EXPECT_FALSE(merged.value().has_value());

    // Merging down to two segments is a no-op as well.
    auto two = policy->find_forced_merges(infos, 2, context);
    ASSERT_TRUE(two.has_value());
    EXPECT_FALSE(two.value().has_value());
}

TEST(TieredMergePolicyTest, ForcedDeletesRewritesSegmentsAboveTheThreshold) {
    ByteBuffersDirectory dir;
    write_file(dir, "_0.fdt", kSegmentBytes);
    write_file(dir, "_1.fdt", kSegmentBytes);
    // 20% deletes is above the 10% `force_merge_deletes_pct_allowed`, 5% is not.
    SegmentInfos infos = make_infos({make_segment("_0", 100, 20), make_segment("_1", 100, 5)});

    auto policy = TieredMergePolicy::create();
    SimpleMergeContext context(get_default_info_stream(), &dir);

    auto merged = policy->find_forced_deletes_merges(infos, context);
    ASSERT_TRUE(merged.has_value()) << (merged ? "" : merged.error().to_string());
    ASSERT_TRUE(merged.value().has_value());
    ASSERT_EQ(merged.value()->size(), 1U);
    // A single segment with deletes is rewritten on its own.
    EXPECT_EQ(merge_names(merged.value()), (std::vector<std::vector<std::string>>{{"_0"}}));
}

TEST(TieredMergePolicyTest, NoForcedDeletesBelowTheThreshold) {
    ByteBuffersDirectory dir;
    write_file(dir, "_0.fdt", kSegmentBytes);
    SegmentInfos infos = make_infos({make_segment("_0", 100, 5)});

    auto policy = TieredMergePolicy::create();
    SimpleMergeContext context(get_default_info_stream(), &dir);

    auto merged = policy->find_forced_deletes_merges(infos, context);
    ASSERT_TRUE(merged.has_value()) << (merged ? "" : merged.error().to_string());
    EXPECT_FALSE(merged.value().has_value());
}

TEST(TieredMergePolicyTest, SegmentsBeingMergedAreSkipped) {
    ByteBuffersDirectory dir;
    std::vector<SegmentCommitInfo> segments;
    for (int32_t i = 0; i < 6; ++i) {
        const std::string name = "_" + std::to_string(i);
        write_file(dir, name + ".fdt", kSegmentBytes);
        segments.push_back(make_segment(name, 10));
    }
    SegmentInfos infos = make_infos(std::move(segments));

    auto policy = small_tier_policy();
    SimpleMergeContext context(get_default_info_stream(), &dir);
    for (int32_t i = 0; i < 6; ++i) {
        context.set_merging("_" + std::to_string(i), true);
    }

    auto merged = policy->find_merges(MergeTrigger::SegmentFlush, infos, context);
    ASSERT_TRUE(merged.has_value()) << (merged ? "" : merged.error().to_string());
    EXPECT_FALSE(merged.value().has_value());
}
