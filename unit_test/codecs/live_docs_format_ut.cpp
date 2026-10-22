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
#include <set>
#include <string>
#include <vector>

module lucene_core:ut.live_docs_format;

import lucene_core;

using namespace lucene;

namespace {

/// A commit of a segment that already has `del_count` deletions recorded.
SegmentCommitInfo segment_with(int32_t max_doc, int32_t del_count, int64_t del_gen) {
    SegmentCommitInfo segment;
    segment.info.name = "_0";
    segment.info.id.assign(ID_LENGTH, 0x7F);
    segment.info.version = Version{10, 1, 0};
    segment.info.min_version = Version{10, 0, 0};
    segment.info.max_doc = max_doc;
    segment.del_count = del_count;
    segment.del_gen = del_gen;
    return segment;
}

} // namespace

TEST(FixedBitSetTest, SetsClearsAndCounts) {
    FixedBitSet bits(130);
    EXPECT_EQ(bits.length(), 130u);
    EXPECT_EQ(bits.words().size(), 3u);
    EXPECT_EQ(bits.cardinality(), 0u);

    EXPECT_TRUE(bits.set(0));
    EXPECT_FALSE(bits.set(0));
    bits.set(63);
    bits.set(64);
    bits.set(129);
    EXPECT_EQ(bits.cardinality(), 4u);
    EXPECT_EQ(bits.get_bit(63), true);
    EXPECT_EQ(bits.get_bit(65), false);

    EXPECT_TRUE(bits.clear(64));
    EXPECT_EQ(bits.cardinality(), 3u);
    EXPECT_EQ(bits.next_set_bit(0), 0);
    EXPECT_EQ(bits.next_set_bit(1), 63);
    EXPECT_EQ(bits.next_set_bit(64), 129);
    EXPECT_EQ(bits.next_set_bit(130), -1);

    EXPECT_FALSE(bits.get(130).has_value());
    // `flip` reports the previous state, so flipping a clear bit returns false.
    EXPECT_FALSE(bits.flip(1));
    EXPECT_TRUE(bits.get_bit(1));
    EXPECT_EQ(bits.next_set_bit(0), 0);
    EXPECT_EQ(bits.next_set_bit(1), 1);
}

TEST(FixedBitSetTest, CombinesSets) {
    FixedBitSet left(70);
    left.set(1);
    left.set(65);

    FixedBitSet right(130);
    right.set(1);
    right.set(65);
    right.set(100);

    EXPECT_TRUE(left.intersects(right));
    EXPECT_EQ(FixedBitSet::intersection_count(left, right), 2u);

    auto unioned = left;
    unioned.or_with(right);
    EXPECT_EQ(unioned.cardinality(), 3u);
    EXPECT_EQ(unioned.length(), 70u);  // or_with never grows the logical length
    EXPECT_EQ(unioned.words().size(), 3u);

    auto intersected = right;
    intersected.and_with(left);
    EXPECT_EQ(intersected.cardinality(), 2u);
    EXPECT_FALSE(intersected.get_bit(100));

    auto difference = right;
    difference.and_not(left);
    EXPECT_EQ(difference.cardinality(), 1u);
    EXPECT_TRUE(difference.get_bit(100));

    // Padding bits of the last word must never survive construction.
    auto padded = FixedBitSet::with_capacity({0xFFFF'FFFF'FFFF'FFFFULL}, 3);
    ASSERT_TRUE(padded.has_value());
    EXPECT_EQ(padded->cardinality(), 3u);
    EXPECT_FALSE(FixedBitSet::with_capacity({0ULL}, 65).has_value());
}

TEST(Lucene90LiveDocsFormatTest, RoundTripsTheBitmap) {
    ByteBuffersDirectory dir;
    SegmentCommitInfo segment = segment_with(80, 0, -1);

    FixedBitSet live(80);
    for (size_t i = 0; i < live.length(); ++i) {
        live.set(i);
    }
    // Delete documents 3, 10 and 79.
    live.clear(3);
    live.clear(10);
    live.clear(79);

    const auto &codec = lucene99_codec();
    ASSERT_TRUE(codec.live_docs_format().write_live_docs(live, dir, segment, 3, io_context_default()).has_value());
    EXPECT_EQ(dir.file_count(), 1u);

    // A fresh commit has `del_gen == -1`, so the first write lands in `_0_0.liv`;
    // recording the new generation is the caller's job, exactly like
    // `SegmentInfos` does for a real commit.
    segment.del_gen = 0;
    segment.del_count = 3;

    auto read = codec.live_docs_format().read_live_docs(dir, segment, io_context_default());
    ASSERT_TRUE(read.has_value()) << (read ? "" : read.error().to_string());
    EXPECT_EQ(read->length(), 80u);
    EXPECT_EQ(read->cardinality(), 77u);
    EXPECT_FALSE(read->get_bit(3));
    EXPECT_FALSE(read->get_bit(10));
    EXPECT_FALSE(read->get_bit(79));
    EXPECT_TRUE(read->get_bit(0));
    EXPECT_TRUE(read->get_bit(78));

    std::set<std::string> files;
    ASSERT_TRUE(codec.live_docs_format().files(segment, files).has_value());
    EXPECT_EQ(files, (std::set<std::string>{"_0_0.liv"}));
    EXPECT_EQ(Lucene90LiveDocsFormat::file_name("_0", 0), "_0_0.liv");
    EXPECT_EQ(Lucene90LiveDocsFormat::file_name("_0", 11), "_0_b.liv");
}

TEST(Lucene90LiveDocsFormatTest, RejectsInconsistentDeletionCounts) {
    ByteBuffersDirectory dir;
    SegmentCommitInfo segment = segment_with(40, 2, -1);

    FixedBitSet live(40);
    for (size_t i = 0; i < live.length(); ++i) {
        live.set(i);
    }
    live.clear(5);  // only one deletion, while the commit claims two

    const auto &format = lucene99_codec().live_docs_format();
    EXPECT_FALSE(format.write_live_docs(live, dir, segment, 0, io_context_default()).has_value());
    // The rejected file must not be left behind.
    EXPECT_EQ(dir.file_count(), 0u);

    // A bitmap whose length disagrees with maxDoc is refused up front.
    FixedBitSet wrong_length(41);
    wrong_length.set(0);
    EXPECT_FALSE(format.write_live_docs(wrong_length, dir, segment, -2, io_context_default()).has_value());
}

TEST(Lucene90LiveDocsFormatTest, RejectsAForeignSegmentId) {
    ByteBuffersDirectory dir;
    SegmentCommitInfo segment = segment_with(16, 1, -1);

    FixedBitSet live(16);
    for (size_t i = 0; i < live.length(); ++i) {
        live.set(i);
    }
    live.clear(0);

    const auto &format = lucene99_codec().live_docs_format();
    // The commit already records one deletion, so this write adds none.
    ASSERT_TRUE(format.write_live_docs(live, dir, segment, 0, io_context_default()).has_value());

    segment.del_gen = 0;
    segment.del_count = 1;
    // The segment id is part of the header, so a copied file is refused.
    segment.info.id.assign(ID_LENGTH, 0x11);
    auto read = format.read_live_docs(dir, segment, io_context_default());
    ASSERT_FALSE(read.has_value());
    EXPECT_EQ(read.error().kind(), ErrorKind::kCorruptIndex);

    // Without a live docs generation there is no file to read at all.
    SegmentCommitInfo fresh = segment_with(16, 0, -1);
    auto missing = format.read_live_docs(dir, fresh, io_context_default());
    ASSERT_FALSE(missing.has_value());
    EXPECT_EQ(missing.error().kind(), ErrorKind::kIllegalState);
}
