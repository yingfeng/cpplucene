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

module lucene_core:ut.postings_util;

import lucene_core;

using namespace lucene;

namespace {

constexpr size_t kBufferSize = 16;

} // namespace

TEST(PostingsUtilTest, RoundTripsDocsAndFreqs) {
    std::vector<int32_t> docs = {1, 2, 3, 5, 8, 13, 21, 34, 55};
    const std::vector<int32_t> freqs = {1, 1, 3, 1, 2, 1, 1, 7, 1};
    const std::vector<int32_t> expected_docs = docs;
    const size_t num = docs.size();

    std::vector<uint8_t> bytes(kBufferSize * 8);
    ByteArrayDataOutput out(bytes);
    ASSERT_TRUE(PostingsUtil::write_vint_block(out, docs, freqs, num, true).has_value());
    bytes.resize(out.get_position());

    std::vector<int32_t> decoded_docs(kBufferSize, -1);
    std::vector<int32_t> decoded_freqs(kBufferSize, -1);
    ByteArrayDataInput in(bytes);
    ASSERT_TRUE(PostingsUtil::read_vint_block(in, decoded_docs, decoded_freqs, num, true, true).has_value());
    EXPECT_TRUE(in.eof());

    for (size_t i = 0; i < num; ++i) {
        EXPECT_EQ(decoded_docs[i], expected_docs[i]) << "i=" << i;
        EXPECT_EQ(decoded_freqs[i], freqs[i]) << "i=" << i;
    }
}

TEST(PostingsUtilTest, DecodeWithoutFreqsDropsTheFlagBit) {
    std::vector<int32_t> docs = {1, 2, 3, 4};
    const std::vector<int32_t> freqs = {1, 4, 1, 2};
    const std::vector<int32_t> expected_docs = docs;

    std::vector<uint8_t> bytes(kBufferSize * 8);
    ByteArrayDataOutput out(bytes);
    ASSERT_TRUE(PostingsUtil::write_vint_block(out, docs, freqs, docs.size(), true).has_value());
    bytes.resize(out.get_position());

    std::vector<int32_t> decoded_docs(kBufferSize, -1);
    std::vector<int32_t> decoded_freqs(kBufferSize, -1);
    ByteArrayDataInput in(bytes);
    ASSERT_TRUE(PostingsUtil::read_vint_block(in, decoded_docs, decoded_freqs, docs.size(), true, false).has_value());
    for (size_t i = 0; i < docs.size(); ++i) {
        EXPECT_EQ(decoded_docs[i], expected_docs[i]) << "i=" << i;
    }
}

TEST(PostingsUtilTest, WithoutFreqsTheDocsAreStoredVerbatim) {
    std::vector<int32_t> docs = {7, 9, 11, 13, 15};
    const std::vector<int32_t> expected_docs = docs;
    const size_t num = docs.size();

    std::vector<uint8_t> bytes(kBufferSize * 8);
    ByteArrayDataOutput out(bytes);
    ASSERT_TRUE(PostingsUtil::write_vint_block(out, docs, std::span<const int32_t>{}, num, false).has_value());
    bytes.resize(out.get_position());

    std::vector<int32_t> decoded_docs(kBufferSize, -1);
    std::vector<int32_t> decoded_freqs(kBufferSize, -1);
    ByteArrayDataInput in(bytes);
    ASSERT_TRUE(PostingsUtil::read_vint_block(in, decoded_docs, decoded_freqs, num, false, false).has_value());
    EXPECT_TRUE(in.eof());
    for (size_t i = 0; i < num; ++i) {
        EXPECT_EQ(decoded_docs[i], expected_docs[i]) << "i=" << i;
    }
}
