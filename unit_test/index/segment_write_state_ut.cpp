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

#include <memory>
#include <string>

module lucene_core:ut.segment_write_state;

import lucene_core;

using namespace lucene;

namespace {

std::shared_ptr<const FieldInfos> empty_field_infos() {
    return std::make_shared<const FieldInfos>(FieldInfos::none());
}

} // namespace

TEST(SegmentWriteStateTest, ValidatesSegmentSuffix) {
    EXPECT_TRUE(SegmentWriteState::is_valid_segment_suffix(""));
    // Textual suffix: exactly two parts separated by an underscore.
    EXPECT_TRUE(SegmentWriteState::is_valid_segment_suffix("_0"));
    EXPECT_TRUE(SegmentWriteState::is_valid_segment_suffix("a_b"));
    // Base36 generation.
    EXPECT_TRUE(SegmentWriteState::is_valid_segment_suffix("1a"));
    EXPECT_TRUE(SegmentWriteState::is_valid_segment_suffix("ZZ"));
    // A generation followed by the group a per-field writer appends.
    EXPECT_TRUE(SegmentWriteState::is_valid_segment_suffix("1_Lucene90_0"));
    EXPECT_TRUE(SegmentWriteState::is_valid_segment_suffix("2_Lucene101_1"));

    EXPECT_FALSE(SegmentWriteState::is_valid_segment_suffix("_a_b"));
    EXPECT_FALSE(SegmentWriteState::is_valid_segment_suffix("_0_0_0"));
    EXPECT_FALSE(SegmentWriteState::is_valid_segment_suffix("1_a_b_c"));
    EXPECT_FALSE(SegmentWriteState::is_valid_segment_suffix("!"));
    EXPECT_FALSE(SegmentWriteState::is_valid_segment_suffix("a-b"));
}

TEST(SegmentWriteStateTest, DefaultsAndSuffix) {
    ByteBuffersDirectory directory;
    const IOContext &context = io_context_default();
    SegmentWriteState state{directory, empty_field_infos(), context};

    EXPECT_EQ(state.segment_suffix, "");
    EXPECT_EQ(state.del_count_on_flush, 0);
    EXPECT_EQ(state.soft_del_count_on_flush, 0);
    EXPECT_EQ(state.live_docs, nullptr);
    EXPECT_NE(state.info_stream, nullptr);
    EXPECT_EQ(&state.directory, &directory);
    EXPECT_EQ(&state.context, &context);

    SegmentWriteState with_suffix{get_default_info_stream(), directory, empty_field_infos(), context, "_7"};
    EXPECT_EQ(with_suffix.segment_suffix, "_7");
}

TEST(SegmentWriteStateTest, CopyWithSuffixKeepsCounts) {
    ByteBuffersDirectory directory;
    const IOContext &context = io_context_default();
    SegmentWriteState state{directory, empty_field_infos(), context};
    state.del_count_on_flush = 3;
    state.soft_del_count_on_flush = 1;

    SegmentWriteState copy = SegmentWriteState::copy_with_suffix(state, "_1");
    EXPECT_EQ(copy.segment_suffix, "_1");
    EXPECT_EQ(copy.del_count_on_flush, 3);
    EXPECT_EQ(copy.soft_del_count_on_flush, 1);
    EXPECT_EQ(&copy.directory, &directory);
    EXPECT_EQ(copy.field_infos, state.field_infos);
    EXPECT_EQ(copy.info_stream, state.info_stream);
}
