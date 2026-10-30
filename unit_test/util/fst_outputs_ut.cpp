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

module lucene_core:ut.fst_outputs;

import lucene_core;

using namespace lucene;

namespace {

FstOutput bytes_output(std::string_view text) {
    return FstOutput{BytesRef::from_string(text)};
}

std::string text_of(const FstOutput &value) {
    const auto *bytes = value.as_ptr<BytesRef>();
    if (bytes == nullptr) {
        return "<not bytes>";
    }
    return std::string(reinterpret_cast<const char *>(bytes->as_slice().data()), bytes->length());
}

} // namespace

TEST(FstByteSequenceOutputsTest, CommonIsTheLongestCommonPrefix) {
    const auto &outputs = FstByteSequenceOutputs::singleton();

    EXPECT_EQ(text_of(outputs.common(bytes_output("foobar"), bytes_output("food"))), "foo");
    // Identical outputs stay untouched.
    EXPECT_EQ(text_of(outputs.common(bytes_output("foobar"), bytes_output("foobar"))), "foobar");
    // No common prefix at all means NO_OUTPUT.
    EXPECT_TRUE(outputs.is_no_output(outputs.common(bytes_output("aaa"), bytes_output("bbb"))));
    // A prefix of the other output wins.
    EXPECT_EQ(text_of(outputs.common(bytes_output("foo"), bytes_output("foobar"))), "foo");
    EXPECT_EQ(text_of(outputs.common(bytes_output("foobar"), bytes_output("foo"))), "foo");
}

TEST(FstByteSequenceOutputsTest, SubtractRemovesThePrefix) {
    const auto &outputs = FstByteSequenceOutputs::singleton();

    EXPECT_EQ(text_of(outputs.subtract(bytes_output("foobar"), bytes_output("foo"))), "bar");
    // Subtracting NO_OUTPUT is a no-op.
    EXPECT_EQ(text_of(outputs.subtract(bytes_output("foobar"), outputs.get_no_output())), "foobar");
    // Subtracting the whole output yields NO_OUTPUT.
    EXPECT_TRUE(outputs.is_no_output(outputs.subtract(bytes_output("foo"), bytes_output("foo"))));
}

TEST(FstByteSequenceOutputsTest, AddPrependsAndHandlesNoOutput) {
    const auto &outputs = FstByteSequenceOutputs::singleton();

    EXPECT_EQ(text_of(outputs.add(bytes_output("foo"), bytes_output("bar"))), "foobar");
    EXPECT_EQ(text_of(outputs.add(outputs.get_no_output(), bytes_output("bar"))), "bar");
    EXPECT_EQ(text_of(outputs.add(bytes_output("foo"), outputs.get_no_output())), "foo");

    FstOutput prefix = bytes_output("foo");
    FstOutput output = bytes_output("bar");
    outputs.add_assign(prefix, output);
    EXPECT_EQ(text_of(prefix), "foobar");
}

TEST(FstByteSequenceOutputsTest, RoundTripsThroughADataOutput) {
    const auto &outputs = FstByteSequenceOutputs::singleton();

    std::vector<uint8_t> buffer(64, 0);
    ByteArrayDataOutput out(buffer);
    ASSERT_TRUE(outputs.write(bytes_output("hello"), out).has_value());
    // NO_OUTPUT is encoded as a zero-length vint.
    ASSERT_TRUE(outputs.write(outputs.get_no_output(), out).has_value());

    ByteArrayDataInput in(std::span<const uint8_t>(buffer.data(), out.get_position()));
    auto first = outputs.read(in);
    ASSERT_TRUE(first.has_value()) << (first ? "" : first.error().to_string());
    EXPECT_EQ(text_of(first.value()), "hello");
    auto second = outputs.read(in);
    ASSERT_TRUE(second.has_value());
    EXPECT_TRUE(outputs.is_no_output(second.value()));
    EXPECT_TRUE(in.eof());

    // `skip_output` must consume exactly the same bytes.
    ByteArrayDataInput skipping(std::span<const uint8_t>(buffer.data(), out.get_position()));
    ASSERT_TRUE(outputs.skip_output(skipping).has_value());
    ASSERT_TRUE(outputs.skip_output(skipping).has_value());
    EXPECT_TRUE(skipping.eof());
}

TEST(FstPositiveIntOutputsTest, MinSumAndDifference) {
    const auto &outputs = FstPositiveIntOutputs::singleton();

    EXPECT_EQ((outputs.common(FstOutput{int64_t{5}}, FstOutput{int64_t{3}})).get<int64_t>(), 3);
    EXPECT_TRUE(outputs.is_no_output(outputs.common(FstOutput{int64_t{5}}, outputs.get_no_output())));
    EXPECT_EQ((outputs.add(FstOutput{int64_t{5}}, FstOutput{int64_t{3}})).get<int64_t>(), 8);
    EXPECT_EQ((outputs.add(outputs.get_no_output(), FstOutput{int64_t{3}})).get<int64_t>(), 3);
    EXPECT_EQ((outputs.subtract(FstOutput{int64_t{5}}, FstOutput{int64_t{3}})).get<int64_t>(), 2);
    EXPECT_TRUE(outputs.is_no_output(outputs.subtract(FstOutput{int64_t{5}}, FstOutput{int64_t{5}})));
    EXPECT_EQ((outputs.subtract(FstOutput{int64_t{5}}, outputs.get_no_output())).get<int64_t>(), 5);

    std::vector<uint8_t> buffer(32, 0);
    ByteArrayDataOutput out(buffer);
    ASSERT_TRUE(outputs.write(FstOutput{int64_t{300}}, out).has_value());
    ASSERT_TRUE(outputs.write(outputs.get_no_output(), out).has_value());

    ByteArrayDataInput in(std::span<const uint8_t>(buffer.data(), out.get_position()));
    auto first = outputs.read(in);
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ((first.value()).get<int64_t>(), 300);
    auto second = outputs.read(in);
    ASSERT_TRUE(second.has_value());
    EXPECT_TRUE(outputs.is_no_output(second.value()));
}

TEST(FstNoOutputsTest, EverythingIsNoOutput) {
    const auto &outputs = FstNoOutputs::singleton();

    EXPECT_TRUE(outputs.is_no_output(outputs.get_no_output()));
    EXPECT_TRUE(outputs.is_no_output(outputs.add(outputs.get_no_output(), outputs.get_no_output())));
    EXPECT_TRUE(outputs.is_no_output(outputs.common(outputs.get_no_output(), outputs.get_no_output())));
    EXPECT_EQ(outputs.output_to_string(outputs.get_no_output()), "");
    EXPECT_EQ(outputs.ram_bytes_used(outputs.get_no_output()), 0);

    std::vector<uint8_t> buffer(8, 0);
    ByteArrayDataOutput out(buffer);
    ASSERT_TRUE(outputs.write(outputs.get_no_output(), out).has_value());
    EXPECT_EQ(out.get_position(), 0u) << "NoOutputs writes nothing";

    ByteArrayDataInput in(std::span<const uint8_t>(buffer.data(), 0));
    auto read = outputs.read(in);
    ASSERT_TRUE(read.has_value());
    EXPECT_TRUE(outputs.is_no_output(read.value()));

    // `NoOutputs` overrides `merge`; the byte-sequence algebra does not.
    EXPECT_TRUE(outputs.merge(outputs.get_no_output(), outputs.get_no_output()).has_value());
    EXPECT_FALSE(FstByteSequenceOutputs::singleton()
                     .merge(FstByteSequenceOutputs::singleton().get_no_output(),
                            FstByteSequenceOutputs::singleton().get_no_output())
                     .has_value());
}

TEST(FstIntSequenceOutputsTest, PrefixAlgebraAndRoundTrip) {
    const auto &outputs = FstIntSequenceOutputs::singleton();

    const FstOutput ab{IntsRef{std::vector<int32_t>{1, 2}}};
    const FstOutput ac{IntsRef{std::vector<int32_t>{1, 3}}};
    const FstOutput a{IntsRef{std::vector<int32_t>{1}}};

    EXPECT_EQ((outputs.common(ab, ac)).get<IntsRef>().to_string(), "[1]");
    EXPECT_EQ((outputs.common(ab, a)).get<IntsRef>().to_string(), "[1]");
    EXPECT_EQ((outputs.common(a, ac)).get<IntsRef>().to_string(), "[1]");
    // Disjoint sequences share no prefix at all.
    EXPECT_TRUE(outputs.is_no_output(
        outputs.common(IntsRef{std::vector<int32_t>{1, 2}}, IntsRef{std::vector<int32_t>{3, 4}})));
    EXPECT_EQ((outputs.subtract(ab, a)).get<IntsRef>().to_string(), "[2]");
    EXPECT_TRUE(outputs.is_no_output(outputs.subtract(ab, ab)));
    EXPECT_EQ((outputs.add(a, IntsRef{std::vector<int32_t>{2, 3}})).get<IntsRef>().to_string(), "[1, 2, 3]");
    EXPECT_EQ((outputs.add(outputs.get_no_output(), a)).get<IntsRef>().to_string(), "[1]");
    EXPECT_EQ(outputs.output_to_string(outputs.get_no_output()), "[]");

    // Values larger than one byte exercise the vint encoding.
    const FstOutput wide{IntsRef{std::vector<int32_t>{3, 1000, 100000}}};
    std::vector<uint8_t> buffer(64, 0);
    ByteArrayDataOutput out(buffer);
    ASSERT_TRUE(outputs.write(wide, out).has_value());
    ASSERT_TRUE(outputs.write(outputs.get_no_output(), out).has_value());

    ByteArrayDataInput in(std::span<const uint8_t>(buffer.data(), out.get_position()));
    auto first = outputs.read(in);
    ASSERT_TRUE(first.has_value()) << (first ? "" : first.error().to_string());
    EXPECT_EQ((first.value()).get<IntsRef>().to_string(), "[3, 1000, 100000]");
    auto second = outputs.read(in);
    ASSERT_TRUE(second.has_value());
    EXPECT_TRUE(outputs.is_no_output(second.value()));
    EXPECT_EQ(outputs.ram_bytes_used(wide), 3 * 4);
}
