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
#include <variant>

module lucene_core:ut.block_term_state;

import lucene_core;

using namespace lucene;

TEST(BlockTermStateTest, DefaultsAndOrdinal) {
    BlockTermState state;
    EXPECT_EQ(state.doc_freq, 0);
    EXPECT_EQ(state.total_term_freq, 0);
    EXPECT_EQ(state.term_block_ord, 0);
    EXPECT_EQ(state.block_file_pointer, 0);
    EXPECT_EQ(state.ord(), 0);

    state.set_ord(42);
    EXPECT_EQ(state.ord(), 42);

    state.doc_freq = 5;
    state.total_term_freq = 9;
    state.term_block_ord = 1;
    state.block_file_pointer = 128;
    EXPECT_EQ(state.to_string(), "OrdTermState ord=42 docFreq=5 totalTermFreq=9 termBlockOrd=1 blockFP=128");
}

TEST(BlockTermStateTest, CloneAndCopyFrom) {
    BlockTermState source;
    source.doc_freq = 3;
    source.total_term_freq = 7;
    source.term_block_ord = 2;
    source.block_file_pointer = 64;
    source.set_ord(11);

    std::unique_ptr<TermState> cloned = source.clone();
    ASSERT_NE(cloned, nullptr);
    auto *clone = dynamic_cast<BlockTermState *>(cloned.get());
    ASSERT_NE(clone, nullptr);
    EXPECT_EQ(clone->doc_freq, 3);
    EXPECT_EQ(clone->ord(), 11);

    BlockTermState target;
    ASSERT_TRUE(target.copy_from(*clone).has_value());
    EXPECT_EQ(target.doc_freq, 3);
    EXPECT_EQ(target.total_term_freq, 7);
    EXPECT_EQ(target.term_block_ord, 2);
    EXPECT_EQ(target.block_file_pointer, 64);
    EXPECT_EQ(target.ord(), 11);

    // A different term state must be rejected rather than partially copied.
    OrdTermState other{5};
    auto copied = target.copy_from(other);
    ASSERT_FALSE(copied.has_value());
    EXPECT_EQ(copied.error().kind(), ErrorKind::kIllegalArgument);
}

TEST(IntBlockTermStateTest, DefaultsAndRoundTrip) {
    IntBlockTermState state;
    EXPECT_EQ(state.last_pos_block_offset, -1);
    EXPECT_EQ(state.singleton_doc_id, -1);
    EXPECT_EQ(state.doc_start_fp, 0);

    state.base.doc_freq = 4;
    state.base.total_term_freq = 12;
    state.base.set_ord(9);
    state.doc_start_fp = 100;
    state.pos_start_fp = 200;
    state.pay_start_fp = 300;
    state.last_pos_block_offset = 400;
    state.singleton_doc_id = 7;

    std::unique_ptr<TermState> cloned = state.clone();
    ASSERT_NE(cloned, nullptr);
    auto *clone = dynamic_cast<IntBlockTermState *>(cloned.get());
    ASSERT_NE(clone, nullptr);
    EXPECT_EQ(clone->base.doc_freq, 4);
    EXPECT_EQ(clone->pos_start_fp, 200);

    IntBlockTermState target;
    ASSERT_TRUE(target.copy_from(state).has_value());
    EXPECT_EQ(target.base.doc_freq, 4);
    EXPECT_EQ(target.base.total_term_freq, 12);
    EXPECT_EQ(target.base.ord(), 9);
    EXPECT_EQ(target.doc_start_fp, 100);
    EXPECT_EQ(target.pos_start_fp, 200);
    EXPECT_EQ(target.pay_start_fp, 300);
    EXPECT_EQ(target.last_pos_block_offset, 400);
    EXPECT_EQ(target.singleton_doc_id, 7);
    EXPECT_EQ(target.to_string(),
              "OrdTermState ord=9 docFreq=4 totalTermFreq=12 termBlockOrd=0 blockFP=0 docStartFP=100 posStartFP=200 "
              "payStartFP=300 lastPosBlockOffset=400 singletonDocID=7");

    BlockTermState wrong;
    auto copied = target.copy_from(wrong);
    ASSERT_FALSE(copied.has_value());
    EXPECT_EQ(copied.error().kind(), ErrorKind::kIllegalArgument);
}

TEST(TermStateEnumTest, BlockAccessorsCoverIntVariant) {
    TermStateEnum block{BlockTermState{}};
    auto block_state = block.get_block_term_state_mut();
    ASSERT_TRUE(block_state.has_value());
    (*block_state)->doc_freq = 6;
    EXPECT_EQ(std::get<BlockTermState>(block.value()).doc_freq, 6);

    TermStateEnum integer{IntBlockTermState{}};
    auto int_state = integer.get_block_term_state_mut();
    ASSERT_TRUE(int_state.has_value());
    (*int_state)->doc_freq = 8;
    EXPECT_EQ(std::get<IntBlockTermState>(integer.value()).base.doc_freq, 8);

    auto const_state = integer.get_block_term_state();
    ASSERT_TRUE(const_state.has_value());
    EXPECT_EQ((*const_state)->doc_freq, 8);
}

TEST(TermStateEnumTest, OrdinalsPerVariant) {
    TermStateEnum block{BlockTermState{}};
    ASSERT_TRUE(block.set_ord(3).has_value());
    EXPECT_EQ(block.ord().value(), 3);

    TermStateEnum integer{IntBlockTermState{}};
    ASSERT_TRUE(integer.set_ord(4).has_value());
    EXPECT_EQ(integer.ord().value(), 4);
    EXPECT_EQ(std::get<IntBlockTermState>(integer.value()).base.ord(), 4);

    TermStateEnum ord{OrdTermState{5}};
    ASSERT_TRUE(ord.set_ord(6).has_value());
    EXPECT_EQ(ord.ord().value(), 6);
    EXPECT_EQ(std::get<OrdTermState>(ord.value()).ord, 6);
}

TEST(TermStateEnumTest, CopyFromRequiresMatchingVariant) {
    TermStateEnum source{IntBlockTermState{}};
    auto source_state = source.get_block_term_state_mut();
    ASSERT_TRUE(source_state.has_value());
    (*source_state)->doc_freq = 11;
    std::get<IntBlockTermState>(source.value_mut()).doc_start_fp = 512;

    TermStateEnum same{IntBlockTermState{}};
    ASSERT_TRUE(same.copy_from(source).has_value());
    EXPECT_EQ(std::get<IntBlockTermState>(same.value()).base.doc_freq, 11);
    EXPECT_EQ(std::get<IntBlockTermState>(same.value()).doc_start_fp, 512);

    TermStateEnum other{OrdTermState{1}};
    auto copied = same.copy_from(other);
    ASSERT_FALSE(copied.has_value());
    EXPECT_EQ(copied.error().kind(), ErrorKind::kIllegalState);
}
