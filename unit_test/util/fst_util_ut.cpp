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
#include <optional>
#include <string>
#include <vector>

module lucene_core:ut.fst_util;

import lucene_core;

using namespace lucene;

namespace {

/// A [`FstBytesReader`] over a buffer; the FST only walks forward here.
class BufferReader final : public FstBytesReader {
public:
    explicit BufferReader(std::span<const uint8_t> bytes) : bytes_(bytes) {}

    [[nodiscard]] Result<uint8_t> read_byte() override {
        if (pos_ < 0 || static_cast<size_t>(pos_) >= bytes_.size()) {
            return err(LuceneError::eof("read past the end"));
        }
        return bytes_[static_cast<size_t>(pos_++)];
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
        pos_ += num_bytes;
        return {};
    }

    [[nodiscard]] int64_t get_position() const override { return pos_; }
    void set_position(int64_t pos) override { pos_ = pos; }

private:
    std::span<const uint8_t> bytes_;
    int64_t pos_{0};
};

std::vector<int32_t> key_of(std::string_view text) {
    std::vector<int32_t> key;
    key.reserve(text.size());
    for (const char character : text) {
        key.push_back(static_cast<int32_t>(static_cast<unsigned char>(character)));
    }
    return key;
}

std::string text_of(const std::any &value) {
    if (value.type() == typeid(BytesRef)) {
        const auto &bytes = std::any_cast<const BytesRef &>(value);
        const auto slice = bytes.as_slice();
        return std::string(reinterpret_cast<const char *>(slice.data()), slice.size());
    }
    if (value.type() == typeid(int64_t)) {
        return std::to_string(std::any_cast<int64_t>(value));
    }
    return "<other>";
}

/// Builds a BYTE1 FST over byte-sequence outputs.
struct ByteFst {
    std::vector<uint8_t> data;
    std::unique_ptr<FST> fst;
    const FstOutputs *outputs{nullptr};
};

ByteFst build_byte_fst() {
    const auto &outputs = FstByteSequenceOutputs::singleton();
    FstBuilder builder(outputs, InputType::kByte1);
    const std::vector<std::pair<std::string, std::string>> entries{
        {"", "root"}, {"a", "1"}, {"ab", "22"}, {"b", "3"}, {"cd", "4"}};
    for (const auto &[key, output] : entries) {
        auto added = builder.add(key_of(key), std::any{BytesRef::from_string(output)});
        EXPECT_TRUE(added.has_value());
    }
    auto result = builder.finish();
    EXPECT_TRUE(result.has_value());

    ByteFst built;
    built.data = std::move(result->data);
    built.outputs = &outputs;
    built.fst = std::make_unique<FST>(result->metadata, std::make_unique<BufferReader>(built.data));
    return built;
}

ByteFst build_int_fst() {
    const auto &outputs = FstPositiveIntOutputs::singleton();
    FstBuilder builder(outputs, InputType::kByte1);
    const std::vector<std::pair<std::string, int64_t>> entries{{"a", 1}, {"ab", 5}, {"b", 9}};
    for (const auto &[key, output] : entries) {
        auto added = builder.add(key_of(key), std::any{output});
        EXPECT_TRUE(added.has_value());
    }
    auto result = builder.finish();
    EXPECT_TRUE(result.has_value());

    ByteFst built;
    built.data = std::move(result->data);
    built.outputs = &outputs;
    built.fst = std::make_unique<FST>(result->metadata, std::make_unique<BufferReader>(built.data));
    return built;
}

} // namespace

TEST(FstUtilTest, GetFromBytesFindsEveryKey) {
    auto built = build_byte_fst();

    const std::vector<std::pair<std::string, std::string>> expected{
        {"", "root"}, {"a", "1"}, {"ab", "22"}, {"b", "3"}, {"cd", "4"}};
    for (const auto &[key, output] : expected) {
        auto value = FstUtil::get_from_bytes(*built.fst, BytesRef::from_string(key));
        ASSERT_TRUE(value.has_value()) << (value ? "" : value.error().to_string());
        ASSERT_TRUE(value->has_value()) << "key " << key;
        EXPECT_EQ(text_of(value->value()), output) << "key " << key;
    }

    // Keys that were never added are reported as absent, not as errors.
    for (const std::string missing : {"abc", "z", "aa", "c"}) {
        auto value = FstUtil::get_from_bytes(*built.fst, BytesRef::from_string(missing));
        ASSERT_TRUE(value.has_value());
        EXPECT_FALSE(value->has_value()) << "key " << missing;
    }
}

TEST(FstUtilTest, GetFromIntsMatchesGetFromBytes) {
    auto built = build_int_fst();

    auto a = FstUtil::get_from_ints(*built.fst, key_of("a"));
    ASSERT_TRUE(a.has_value());
    ASSERT_TRUE(a->has_value());
    EXPECT_EQ(text_of(a->value()), "1");

    auto ab = FstUtil::get_from_ints(*built.fst, key_of("ab"));
    ASSERT_TRUE(ab.has_value());
    ASSERT_TRUE(ab->has_value());
    EXPECT_EQ(text_of(ab->value()), "5");

    auto missing = FstUtil::get_from_ints(*built.fst, key_of("abc"));
    ASSERT_TRUE(missing.has_value());
    EXPECT_FALSE(missing->has_value());
}

TEST(FstUtilTest, ReadCeilArcReturnsTheNextHigherLabel) {
    auto built = build_byte_fst();
    FST &fst = *built.fst;

    Arc follow;
    fst.get_first_arc(follow);

    // "aa" has no arc of its own; the ceiling is "b".
    Arc arc;
    auto found = FstUtil::read_ceil_arc(static_cast<int32_t>('a'), fst, follow, arc);
    ASSERT_TRUE(found.has_value());
    ASSERT_TRUE(found.value());
    EXPECT_EQ(arc.label(), static_cast<int32_t>('a'));

    auto next = FstUtil::read_ceil_arc(static_cast<int32_t>('a') + 1, fst, follow, arc);
    ASSERT_TRUE(next.has_value());
    ASSERT_TRUE(next.value());
    EXPECT_EQ(arc.label(), static_cast<int32_t>('b'));

    // Exactly the last label.
    auto last = FstUtil::read_ceil_arc(static_cast<int32_t>('c'), fst, follow, arc);
    ASSERT_TRUE(last.has_value());
    ASSERT_TRUE(last.value());
    EXPECT_EQ(arc.label(), static_cast<int32_t>('c'));

    // Beyond the highest label there is no ceiling arc.
    auto beyond = FstUtil::read_ceil_arc(static_cast<int32_t>('z'), fst, follow, arc);
    ASSERT_TRUE(beyond.has_value());
    EXPECT_FALSE(beyond.value());
}

TEST(FstUtilTest, ReadCeilArcReadsTheArcOnAMiss) {
    // A gap between two root labels: the miss must leave the ceiling arc loaded,
    // not the stale arc it was called with.
    const auto &outputs = FstByteSequenceOutputs::singleton();
    FstBuilder builder(outputs, InputType::kByte1);
    const std::vector<std::pair<std::string, std::string>> entries{{"", "root"}, {"a", "1"}, {"c", "3"}};
    for (const auto &[key, output] : entries) {
        ASSERT_TRUE(builder.add(key_of(key), std::any{BytesRef::from_string(output)}).has_value());
    }
    auto result = builder.finish();
    ASSERT_TRUE(result.has_value());
    std::vector<uint8_t> data = std::move(result->data);
    FST fst(result->metadata, std::make_unique<BufferReader>(data));

    Arc follow;
    fst.get_first_arc(follow);

    Arc arc;
    auto found = FstUtil::read_ceil_arc(static_cast<int32_t>('b'), fst, follow, arc);
    ASSERT_TRUE(found.has_value()) << (found.has_value() ? "" : found.error().to_string());
    ASSERT_TRUE(found.value());
    EXPECT_EQ(arc.label(), static_cast<int32_t>('c'));
}

TEST(FstUtilTest, ShortestPathsReturnsCheapestInputs) {
    auto built = build_int_fst();
    FST &fst = *built.fst;

    Arc start;
    fst.get_first_arc(start);

    const OutputComparator comparator = [](const std::any &left, const std::any &right) -> int32_t {
        const auto l = std::any_cast<int64_t>(left);
        const auto r = std::any_cast<int64_t>(right);
        return l < r ? -1 : (l > r ? 1 : 0);
    };

    auto results = FstUtil::shortest_paths(fst, start, built.outputs->get_no_output(), comparator, 3, false);
    ASSERT_TRUE(results.has_value()) << (results ? "" : results.error().to_string());
    ASSERT_EQ(results->top_n.size(), 3u);

    // Outputs are monotonically increasing: 1 ("a"), 5 ("ab"), 9 ("b").
    const std::vector<std::string> expected_inputs{"a", "ab", "b"};
    const std::vector<std::string> expected_outputs{"1", "5", "9"};
    for (size_t i = 0; i < results->top_n.size(); ++i) {
        std::string input;
        for (const int32_t label : results->top_n[i].input.as_slice()) {
            input.push_back(static_cast<char>(label));
        }
        EXPECT_EQ(input, expected_inputs[i]) << "index " << i;
        EXPECT_EQ(text_of(results->top_n[i].output), expected_outputs[i]) << "index " << i;
    }
    EXPECT_TRUE(results->is_complete);
}

TEST(FstUtilTest, ShortestPathsStopsAtTheRequestedCount) {
    auto built = build_byte_fst();
    FST &fst = *built.fst;

    Arc start;
    fst.get_first_arc(start);

    // Byte-sequence outputs compare lexicographically.
    const auto &outputs = FstByteSequenceOutputs::singleton();
    const OutputComparator comparator = [](const std::any &left, const std::any &right) -> int32_t {
        const auto &l = std::any_cast<const BytesRef &>(left);
        const auto &r = std::any_cast<const BytesRef &>(right);
        return l.compare_to(r);
    };

    auto results = FstUtil::shortest_paths(fst, start, outputs.get_no_output(), comparator, 1, false);
    ASSERT_TRUE(results.has_value()) << (results ? "" : results.error().to_string());
    ASSERT_EQ(results->top_n.size(), 1u);
    // Exactly one result is produced; the ranking itself is covered above.
    const std::string only = text_of(results->top_n[0].output);
    EXPECT_TRUE(only == "1" || only == "22" || only == "3" || only == "4") << only;

    // With `allow_empty_string` the FST's empty-string output becomes a
    // candidate path of its own.
    auto with_empty = FstUtil::shortest_paths(fst, start, outputs.get_no_output(), comparator, 6, true);
    ASSERT_TRUE(with_empty.has_value()) << (with_empty ? "" : with_empty.error().to_string());
    bool saw_empty = false;
    for (const auto &result : with_empty->top_n) {
        if (result.input.as_slice().empty()) {
            saw_empty = true;
            EXPECT_EQ(text_of(result.output), "root");
        }
    }
    EXPECT_TRUE(saw_empty);
}

TEST(FstUtilTest, ToDotDescribesEveryNodeAndEdge) {
    auto built = build_byte_fst();

    std::string dot;
    ASSERT_TRUE(FstUtil::to_dot(*built.fst, dot, true, false).has_value());

    EXPECT_NE(dot.find("digraph FST {"), std::string::npos);
    EXPECT_NE(dot.find("initial -> "), std::string::npos);
    EXPECT_NE(dot.find("rank=same"), std::string::npos);
    EXPECT_NE(dot.find("label=\"a"), std::string::npos);
    EXPECT_NE(dot.find("label=\"b"), std::string::npos);
    EXPECT_NE(dot.find("{rank=sink; -1 }"), std::string::npos);

    // Labelled states include the serialized addresses.
    std::string labelled;
    ASSERT_TRUE(FstUtil::to_dot(*built.fst, labelled, false, true).has_value());
    EXPECT_EQ(labelled.find("node [shape=circle"), std::string::npos);
}

TEST(FstUtilTest, ToUtf32SplitsCodePoints) {
    IntsRefBuilder scratch;
    ASSERT_TRUE(FstUtil::to_utf32("a\xC3\xA9\xE4\xB8\xAD", scratch).has_value());
    ASSERT_EQ(scratch.length(), 3u);
    EXPECT_EQ(scratch.int_at(0), static_cast<int32_t>('a'));
    EXPECT_EQ(scratch.int_at(1), 0x00E9);
    EXPECT_EQ(scratch.int_at(2), 0x4E2D);

    // Malformed input is rejected instead of producing garbage.
    IntsRefBuilder broken;
    EXPECT_FALSE(FstUtil::to_utf32("\xC3", broken).has_value());
    EXPECT_FALSE(FstUtil::to_utf32("\xC3\x28", broken).has_value());
}

TEST(FstUtilTest, ToIntsRefAndToBytesRefRoundTrip) {
    const BytesRef source = BytesRef::from_string("FST");

    IntsRefBuilder ints;
    ASSERT_TRUE(FstUtil::to_ints_ref(source, ints).has_value());
    ASSERT_EQ(ints.length(), 3u);
    EXPECT_EQ(ints.int_at(0), static_cast<int32_t>('F'));
    EXPECT_EQ(ints.int_at(2), static_cast<int32_t>('T'));

    BytesRefBuilder bytes;
    auto restored = FstUtil::to_bytes_ref(ints.to_ints_ref(), bytes);
    ASSERT_TRUE(restored.has_value());
    EXPECT_EQ(restored->as_slice().size(), source.as_slice().size());
    EXPECT_TRUE(std::equal(restored->as_slice().begin(), restored->as_slice().end(), source.as_slice().begin()));
}
