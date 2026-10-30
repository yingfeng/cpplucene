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
#include <optional>
#include <string>
#include <vector>

module lucene_core:ut.fst_builder;

import lucene_core;

using namespace lucene;

namespace {

/// A [`FstBytesReader`] over a buffer, enough for read-only FST traversal.
///
/// Reads walk **backwards**, like every FST reader: the format addresses each
/// node at its last byte.
class BufferReader final : public FstBytesReader {
public:
    explicit BufferReader(std::span<const uint8_t> bytes) : bytes_(bytes) {}

    [[nodiscard]] Result<uint8_t> read_byte() override {
        if (pos_ < 0 || static_cast<size_t>(pos_) >= bytes_.size()) {
            return err(LuceneError::eof("read past the end"));
        }
        return bytes_[static_cast<size_t>(pos_--)];
    }

    [[nodiscard]] VoidResult read_bytes(std::span<uint8_t> b) override {
        for (auto &byte : b) {
            auto value = read_byte();
            if (!value.has_value()) {
                return err(value.error());
            }
            byte = value.value();
        }
        return {};
    }

    [[nodiscard]] VoidResult skip_bytes(int64_t num_bytes) override {
        pos_ -= num_bytes;
        return {};
    }

    [[nodiscard]] int64_t get_position() const override { return pos_; }
    void set_position(int64_t pos) override { pos_ = pos; }

private:
    std::span<const uint8_t> bytes_;
    int64_t pos_{0};
};

std::string text_of(const FstOutput &value) {
    if (const auto *bytes = value.as_ptr<BytesRef>(); bytes != nullptr) {
        return std::string(reinterpret_cast<const char *>(bytes->as_slice().data()), bytes->length());
    }
    if (const auto *number = value.as_ptr<int64_t>(); number != nullptr) {
        return std::to_string(*number);
    }
    return "<other>";
}

/// Walks `key` through `fst`, accumulating arc outputs like Lucene's `get`.
std::optional<std::string> lookup(FST &fst, std::span<const int32_t> key, const FstOutputs &outputs) {
    Arc arc;
    fst.get_first_arc(arc);
    FstOutput accumulated = outputs.get_no_output();
    for (const int32_t label : key) {
        auto found = fst.find_target_arc(label, &arc, arc, fst.reader());
        if (!found.has_value()) {
            return "<error>";
        }
        if (!found.value()) {
            return std::nullopt;
        }
        accumulated = outputs.add(accumulated, arc.output());
    }
    auto accepted = fst.find_target_arc(END_LABEL, &arc, arc, fst.reader());
    if (!accepted.has_value()) {
        return "<error>";
    }
    if (!accepted.value()) {
        return std::nullopt;
    }
    accumulated = outputs.add(accumulated, arc.output());
    return text_of(accumulated);
}

std::vector<int32_t> key_of(std::string_view text) {
    std::vector<int32_t> key;
    key.reserve(text.size());
    for (const char character : text) {
        key.push_back(static_cast<int32_t>(static_cast<unsigned char>(character)));
    }
    return key;
}

} // namespace

TEST(FstBuilderTest, BuildsAndReadsBackByteSequenceOutputs) {
    const auto &outputs = FstByteSequenceOutputs::singleton();
    FstBuilder builder(outputs, InputType::kByte1);

    const std::vector<std::pair<std::string, std::string>> entries{
        {"", "root"}, {"a", "1"}, {"ab", "22"}, {"b", "3"}, {"cd", "4"}};
    for (const auto &[key, output] : entries) {
        auto added = builder.add(key_of(key), FstOutput{BytesRef::from_string(output)});
        ASSERT_TRUE(added.has_value()) << (added ? "" : added.error().to_string());
        EXPECT_TRUE(added.value()) << key;
    }

    auto result = builder.finish();
    ASSERT_TRUE(result.has_value()) << (result ? "" : result.error().to_string());

    // Address 0 stays reserved: the data starts with a padding byte.
    ASSERT_GT(result->data.size(), 1u);
    EXPECT_EQ(result->data[0], 0);
    EXPECT_EQ(result->metadata.start_node, result->metadata.num_bytes > 1 ? result->metadata.start_node : 0);
    ASSERT_TRUE(result->metadata.empty_output.has_value());
    EXPECT_EQ(text_of(result->metadata.empty_output.value()), "root");

    BufferReader reader(result->data);
    FST fst(result->metadata, std::make_unique<BufferReader>(result->data));
    for (const auto &[key, expected] : entries) {
        const auto actual = lookup(fst, key_of(key), outputs);
        ASSERT_TRUE(actual.has_value()) << "key " << key;
        EXPECT_EQ(actual.value(), expected) << "key " << key;
    }

    // Keys that were never added are not accepted.
    EXPECT_FALSE(lookup(fst, key_of("abc"), outputs).has_value());
    EXPECT_FALSE(lookup(fst, key_of("z"), outputs).has_value());
}

TEST(FstBuilderTest, BuildsAndReadsBackPositiveIntOutputs) {
    const auto &outputs = FstPositiveIntOutputs::singleton();
    FstBuilder builder(outputs, InputType::kByte1);

    const std::vector<std::pair<std::string, int64_t>> entries{{"a", 1}, {"ab", 5}, {"b", 9}};
    for (const auto &[key, output] : entries) {
        auto added = builder.add(key_of(key), FstOutput{output});
        ASSERT_TRUE(added.has_value());
    }

    auto result = builder.finish();
    ASSERT_TRUE(result.has_value()) << (result ? "" : result.error().to_string());
    EXPECT_FALSE(result->metadata.empty_output.has_value());

    FST fst(result->metadata, std::make_unique<BufferReader>(result->data));
    for (const auto &[key, expected] : entries) {
        const auto actual = lookup(fst, key_of(key), outputs);
        ASSERT_TRUE(actual.has_value()) << "key " << key;
        EXPECT_EQ(actual.value(), std::to_string(expected)) << "key " << key;
    }
    EXPECT_FALSE(lookup(fst, key_of("abc"), outputs).has_value());
}

TEST(FstBuilderTest, SharesIdenticalSubtrees) {
    const auto &outputs = FstPositiveIntOutputs::singleton();
    FstBuilder builder(outputs, InputType::kByte1);

    // 200 keys, all ending in the same 40 byte tail: without `NodeHash` every
    // tail would be written once per key.
    const std::string tail(40, 'z');
    std::vector<std::pair<std::string, int64_t>> entries;
    for (int i = 0; i < 200; ++i) {
        std::string key = std::string("k") + static_cast<char>('0' + i / 100) + static_cast<char>('0' + (i / 10) % 10) +
                          static_cast<char>('0' + i % 10) + tail;
        entries.emplace_back(key, static_cast<int64_t>(i) + 1);
    }
    for (const auto &[key, value] : entries) {
        auto added = builder.add(key_of(key), FstOutput{value});
        ASSERT_TRUE(added.has_value()) << (added ? "" : added.error().to_string());
        EXPECT_TRUE(added.value()) << "key " << key;
    }

    auto result = builder.finish();
    ASSERT_TRUE(result.has_value()) << (result ? "" : result.error().to_string());

    // 200 * 44 bytes of labels alone would be ~8.8 kB before de-duplication.
    EXPECT_LT(result->data.size(), 4000u);

    FST fst(result->metadata, std::make_unique<BufferReader>(result->data));
    for (const auto &[key, value] : entries) {
        const auto actual = lookup(fst, key_of(key), outputs);
        ASSERT_TRUE(actual.has_value()) << "key " << key;
        EXPECT_EQ(actual.value(), std::to_string(value)) << "key " << key;
    }
}
