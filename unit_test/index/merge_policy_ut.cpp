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
#include <utility>
#include <vector>

module lucene_core:ut.merge_policy;

import lucene_core;

using namespace lucene;

namespace {

SegmentCommitInfo make_segment(std::string name, int32_t max_doc, int32_t del_count = 0,
                               std::vector<std::string> files = {}) {
    SegmentCommitInfo commit;
    commit.info.name = std::move(name);
    commit.info.id.assign(ID_LENGTH, 0x11);
    commit.info.max_doc = max_doc;
    commit.info.codec_name = "Lucene101";
    commit.del_count = del_count;
    commit.info.files = std::set<std::string>(files.begin(), files.end());
    return commit;
}

SegmentInfos make_infos(std::vector<SegmentCommitInfo> segments) {
    SegmentInfos infos;
    infos.segments = std::move(segments);
    return infos;
}

/// Segment names of every merge in `spec`, in order.
std::vector<std::vector<std::string>> merge_names(const std::optional<MergeSpecification> &spec) {
    std::vector<std::vector<std::string>> names;
    if (!spec.has_value()) {
        return names;
    }
    for (const OneMerge &merge : spec->merges()) {
        std::vector<std::string> group;
        group.reserve(merge.size());
        for (const SegmentCommitInfo &segment : merge.segments()) {
            group.push_back(segment.info.name);
        }
        names.push_back(std::move(group));
    }
    return names;
}

void write_file(Directory &dir, const std::string &name, size_t size) {
    auto out = dir.create_output(name, io_context_default());
    ASSERT_TRUE(out.has_value()) << (out ? "" : out.error().to_string());
    std::vector<uint8_t> bytes(size, 0x5A);
    ASSERT_TRUE(out.value()->write_bytes(bytes).has_value());
    ASSERT_TRUE(out.value()->close().has_value());
}

} // namespace

TEST(MergePolicyTest, NoMergePolicyNeverMerges) {
    auto policy = NoMergePolicy::create();
    SegmentInfos infos = make_infos({make_segment("_0", 10, 2), make_segment("_1", 10)});

    ByteBuffersDirectory dir;
    SimpleMergeContext context(get_default_info_stream(), &dir);

    auto merges = policy->find_merges(MergeTrigger::SegmentFlush, infos, context);
    ASSERT_TRUE(merges.has_value());
    EXPECT_FALSE(merges.value().has_value());
    EXPECT_EQ(policy->to_string(), "NoMergePolicy");

    auto forced = policy->find_forced_merges(infos, 1, context);
    ASSERT_TRUE(forced.has_value());
    EXPECT_FALSE(forced.value().has_value());
}

TEST(MergePolicyTest, LogDocMergesAFullLevel) {
    auto policy = LogDocMergePolicy::create();
    ASSERT_TRUE(policy->set_merge_factor(3).has_value());
    policy->set_min_merge_docs(0);

    ByteBuffersDirectory dir;
    SimpleMergeContext context(get_default_info_stream(), &dir);

    // Two segments are below the merge factor, so nothing happens.
    SegmentInfos two = make_infos({make_segment("_0", 10), make_segment("_1", 10)});
    auto none = policy->find_merges(MergeTrigger::SegmentFlush, two, context);
    ASSERT_TRUE(none.has_value());
    EXPECT_FALSE(none.value().has_value());

    // Three equally sized segments form one full level.
    SegmentInfos three = make_infos({make_segment("_0", 10), make_segment("_1", 10), make_segment("_2", 10)});
    auto merged = policy->find_merges(MergeTrigger::SegmentFlush, three, context);
    ASSERT_TRUE(merged.has_value());
    ASSERT_TRUE(merged.value().has_value());
    EXPECT_EQ(merge_names(merged.value()), (std::vector<std::vector<std::string>>{{"_0", "_1", "_2"}}));
    EXPECT_EQ(merged.value()->merges()[0].total_max_doc(), 30);
}

TEST(MergePolicyTest, LogDocHandlesSeveralLevels) {
    auto policy = LogDocMergePolicy::create();
    ASSERT_TRUE(policy->set_merge_factor(10).has_value());
    policy->set_min_merge_docs(0);

    ByteBuffersDirectory dir;
    SimpleMergeContext context(get_default_info_stream(), &dir);

    // Twelve equal segments: the first ten are merged, the tail of two is left.
    std::vector<SegmentCommitInfo> segments;
    for (int32_t i = 0; i < 12; ++i) {
        segments.push_back(make_segment("_" + std::to_string(i), 10));
    }
    auto merged = policy->find_merges(MergeTrigger::SegmentFlush, make_infos(std::move(segments)), context);
    ASSERT_TRUE(merged.has_value());
    ASSERT_TRUE(merged.value().has_value());
    ASSERT_EQ(merged.value()->size(), 1U);
    EXPECT_EQ(merged.value()->merges()[0].size(), 10U);

    // Twenty-five equal segments need two merges.
    std::vector<SegmentCommitInfo> many;
    for (int32_t i = 0; i < 25; ++i) {
        many.push_back(make_segment("_" + std::to_string(i), 10));
    }
    auto two_merges = policy->find_merges(MergeTrigger::Commit, make_infos(std::move(many)), context);
    ASSERT_TRUE(two_merges.has_value());
    ASSERT_TRUE(two_merges.value().has_value());
    EXPECT_EQ(two_merges.value()->size(), 2U);
}

TEST(MergePolicyTest, LogDocRespectsMaxMergeDocs) {
    auto policy = LogDocMergePolicy::create();
    ASSERT_TRUE(policy->set_merge_factor(3).has_value());
    policy->set_min_merge_docs(0);
    policy->set_max_merge_docs(15);

    ByteBuffersDirectory dir;
    SimpleMergeContext context(get_default_info_stream(), &dir);

    // Three segments of ten documents would need thirty: above the cap, and the
    // first segment alone is below it, so no merge is possible.
    SegmentInfos infos = make_infos({make_segment("_0", 10), make_segment("_1", 10), make_segment("_2", 10)});
    auto merged = policy->find_merges(MergeTrigger::SegmentFlush, infos, context);
    ASSERT_TRUE(merged.has_value());
    EXPECT_FALSE(merged.value().has_value());
}

TEST(MergePolicyTest, DeletedDocumentsAreProrated) {
    auto policy = LogDocMergePolicy::create();
    ASSERT_TRUE(policy->set_merge_factor(2).has_value());
    policy->set_min_merge_docs(0);

    ByteBuffersDirectory dir;
    SimpleMergeContext context(get_default_info_stream(), &dir);

    // With calibration on, the 900 deleted documents make `_0` the same size as
    // `_1`, so the two segments share a level and are merged.
    SegmentInfos infos = make_infos({make_segment("_0", 1000, 900), make_segment("_1", 100)});
    auto calibrated = policy->find_merges(MergeTrigger::SegmentFlush, infos, context);
    ASSERT_TRUE(calibrated.has_value());
    ASSERT_TRUE(calibrated.value().has_value());
    EXPECT_EQ(merge_names(calibrated.value()), (std::vector<std::vector<std::string>>{{"_0", "_1"}}));

    // Without calibration `_0` is ten times bigger, which puts it in its own
    // level, and a single segment is never merged on its own.
    policy->set_calibrate_size_by_deletes(false);
    auto uncalibrated = policy->find_merges(MergeTrigger::SegmentFlush, infos, context);
    ASSERT_TRUE(uncalibrated.has_value());
    EXPECT_FALSE(uncalibrated.value().has_value());
}

TEST(MergePolicyTest, SegmentsBeingMergedAreSkipped) {
    auto policy = LogDocMergePolicy::create();
    ASSERT_TRUE(policy->set_merge_factor(2).has_value());
    policy->set_min_merge_docs(0);

    ByteBuffersDirectory dir;
    SimpleMergeContext context(get_default_info_stream(), &dir);
    context.set_merging("_0", true);

    SegmentInfos infos = make_infos({make_segment("_0", 10), make_segment("_1", 10)});
    auto merged = policy->find_merges(MergeTrigger::SegmentFlush, infos, context);
    ASSERT_TRUE(merged.has_value());
    EXPECT_FALSE(merged.value().has_value());
}

TEST(MergePolicyTest, ForcedMergeDownToOneSegment) {
    auto policy = LogDocMergePolicy::create();
    ASSERT_TRUE(policy->set_merge_factor(2).has_value());
    policy->set_min_merge_docs(0);

    ByteBuffersDirectory dir;
    SimpleMergeContext context(get_default_info_stream(), &dir);

    SegmentInfos infos = make_infos(
            {make_segment("_0", 10), make_segment("_1", 10), make_segment("_2", 10), make_segment("_3", 10)});
    auto merged = policy->find_forced_merges(infos, 1, context);
    ASSERT_TRUE(merged.has_value());
    ASSERT_TRUE(merged.value().has_value());
    // mergeFactor 2: the two rightmost segments and then the two leftmost ones.
    EXPECT_EQ(merge_names(merged.value()),
              (std::vector<std::vector<std::string>>{{"_2", "_3"}, {"_0", "_1"}}));
}

TEST(MergePolicyTest, ForcedMergeOfASingleSegmentWithDeletes) {
    auto policy = LogDocMergePolicy::create();
    policy->set_min_merge_docs(0);

    ByteBuffersDirectory dir;
    SimpleMergeContext context(get_default_info_stream(), &dir);

    // A single segment without deletes is already merged.
    SegmentInfos clean = make_infos({make_segment("_0", 10)});
    auto none = policy->find_forced_merges(clean, 1, context);
    ASSERT_TRUE(none.has_value());
    EXPECT_FALSE(none.value().has_value());

    // With deletes it has to be rewritten.
    SegmentInfos dirty = make_infos({make_segment("_0", 10, 4)});
    auto merged = policy->find_forced_merges(dirty, 1, context);
    ASSERT_TRUE(merged.has_value());
    ASSERT_TRUE(merged.value().has_value());
    EXPECT_EQ(merge_names(merged.value()), (std::vector<std::vector<std::string>>{{"_0"}}));
}

TEST(MergePolicyTest, ForcedDeletesMergesRunsOfSegments) {
    auto policy = LogDocMergePolicy::create();
    ASSERT_TRUE(policy->set_merge_factor(10).has_value());
    policy->set_min_merge_docs(0);

    ByteBuffersDirectory dir;
    SimpleMergeContext context(get_default_info_stream(), &dir);

    // `_0` has deletes and `_1` does not, so `_0` alone is rewritten; `_3` ends
    // the second run.
    SegmentInfos infos = make_infos({make_segment("_0", 10, 1), make_segment("_1", 10), make_segment("_2", 10, 2),
                                     make_segment("_3", 10)});
    auto merged = policy->find_forced_deletes_merges(infos, context);
    ASSERT_TRUE(merged.has_value());
    ASSERT_TRUE(merged.value().has_value());
    EXPECT_EQ(merge_names(merged.value()), (std::vector<std::vector<std::string>>{{"_0"}, {"_2"}}));
}

TEST(MergePolicyTest, LogByteSizeUsesFileSizes) {
    ByteBuffersDirectory dir;
    // Six segments of 100 KB each, plus their file lists.
    std::vector<SegmentCommitInfo> segments;
    for (int32_t i = 0; i < 6; ++i) {
        const std::string name = "_" + std::to_string(i);
        const std::string file = name + ".fdt";
        write_file(dir, file, 100 * 1024);
        segments.push_back(make_segment(name, 10, 0, {file}));
    }
    SegmentInfos infos = make_infos(std::move(segments));

    auto policy = LogByteSizeMergePolicy::create();
    // A level has to hold at least `merge_factor` segments to be merged.
    ASSERT_TRUE(policy->set_merge_factor(6).has_value());
    SimpleMergeContext context(get_default_info_stream(), &dir);

    // Six equally sized segments form one full level.
    auto merged = policy->find_merges(MergeTrigger::SegmentFlush, infos, context);
    ASSERT_TRUE(merged.has_value());
    ASSERT_TRUE(merged.value().has_value());
    EXPECT_EQ(merged.value()->merges()[0].size(), 6U);

    // A maximum of 512 KB allows only five of the six 100 KB segments.
    ASSERT_TRUE(policy->set_max_merge_mb(0.5).has_value());
    auto limited = policy->find_merges(MergeTrigger::SegmentFlush, infos, context);
    ASSERT_TRUE(limited.has_value());
    ASSERT_TRUE(limited.value().has_value());
    ASSERT_EQ(limited.value()->size(), 1U);
    EXPECT_EQ(limited.value()->merges()[0].size(), 5U);
}

TEST(MergePolicyTest, ByteSizeNeedsADirectory) {
    SegmentCommitInfo segment = make_segment("_0", 10);
    segment.info.files = {"_0.fdt"};
    SimpleMergeContext context(get_default_info_stream(), nullptr);

    auto size = segment_size_in_bytes(segment, context);
    ASSERT_FALSE(size.has_value());
    EXPECT_EQ(size.error().kind(), ErrorKind::kUnsupportedOperation);
}
